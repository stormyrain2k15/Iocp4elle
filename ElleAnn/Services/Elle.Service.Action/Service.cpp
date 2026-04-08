// =============================================================================
// Elle.Service.Action — Service.cpp
//
// Action lifecycle management and capability execution.
//
// What this service does:
//   - Polls ActionQueue for PENDING actions (not claimed by QueueWorker)
//   - Maps action types to the correct execution path:
//       Hardware actions (VIBRATE, FLASH)   → Elle.ASM.Hardware.dll
//       Process actions (EXEC, KILL, LIST)   → Elle.ASM.Process.dll
//       File actions (WRITE, READ)           → Elle.ASM.FileIO.dll
//       Android actions (NOTIFY, OPEN_APP)   → WS_BROADCAST to Android app
//       Memory actions (SAVE, READ)          → Direct SQL to ElleMemory
//       WebSocket actions                    → HTTP service via named pipe IPC
//   - Enforces trust gating — actions above current trust level are rejected
//   - Writes results back to ActionQueue via sp_SubmitActionResult
//   - Adjusts trust score on success/failure via ElleCore
// =============================================================================

#include "../../Shared/ElleServiceBase.h"
#include "../../Shared/ElleEpoch.h"
#include "../../Shared/ElleSQLConn.h"
#include "../../Shared/ElleQueueIPC.h"
#include "../../Shared/ElleLogger.h"
#include "../../Shared/ElleConfig.h"
#include "../../Shared/ElleTypes.h"
#include <Windows.h>

#define SVCNAME     ElleConfig::ServiceNames::ACTION
#define LOG(lvl, fmt, ...) ELLE_LOG_##lvl(SVCNAME, fmt, ##__VA_ARGS__)

// =============================================================================
// ASM DLL function typedefs — must match Hardware.asm exports exactly
// =============================================================================
typedef BOOL    (WINAPI* PfnVibrateDevice)(DWORD durationMs);
typedef BOOL    (WINAPI* PfnToggleFlash)(BOOL enable);
typedef BOOL    (WINAPI* PfnSetCpuAffinity)(DWORD processID, DWORD_PTR affinityMask);
typedef BOOL    (WINAPI* PfnGetSystemLoad)(DWORD* outCpuPercent, DWORDLONG* outFreeMemBytes);

typedef BOOL    (WINAPI* PfnLaunchProcess)(LPCWSTR exePath, LPCWSTR args, DWORD* outPID);
typedef BOOL    (WINAPI* PfnKillProcess)(DWORD pid);
typedef BOOL    (WINAPI* PfnListProcesses)(LPWSTR outBuffer, DWORD bufferChars);

typedef BOOL    (WINAPI* PfnWriteFile)(LPCWSTR path, LPCWSTR content);
typedef BOOL    (WINAPI* PfnReadFile)(LPCWSTR path, LPWSTR outBuffer, DWORD bufferChars);

// =============================================================================
// ElleActionExecutor — loads ASM DLLs and routes actions to them
// =============================================================================
class ElleActionExecutor
{
    ELLE_NONCOPYABLE(ElleActionExecutor)
public:
    ElleActionExecutor()
        : m_hHardwareDLL(nullptr)
        , m_hProcessDLL(nullptr)
        , m_hFileIODLL(nullptr)
        , m_Running(false)
        , m_PollThread(nullptr)
        , m_StopEvent(nullptr)
    {}

    ~ElleActionExecutor() { Stop(); }

    ElleResult Start(HANDLE stopEvent)
    {
        LOG(INFO, L"ElleActionExecutor starting");
        m_StopEvent = stopEvent;

        // Load ASM DLLs — non-fatal if they're not yet built, fall back gracefully
        LoadDLL(L"Elle.ASM.Hardware.dll", m_hHardwareDLL, "Hardware DLL");
        LoadDLL(L"Elle.ASM.Process.dll",  m_hProcessDLL,  "Process DLL");
        LoadDLL(L"Elle.ASM.FileIO.dll",   m_hFileIODLL,   "FileIO DLL");

        m_Running = true;
        m_PollThread = CreateThread(nullptr, 0, PollThreadProc, this, 0, nullptr);
        if (!m_PollThread)
        {
            LOG(FATAL, L"CreateThread(Poll) failed: %lu", GetLastError());
            m_Running = false;
            return ElleResult::ERR_GENERIC;
        }

        LOG(INFO, L"ElleActionExecutor running");
        return ElleResult::OK;
    }

    void Stop()
    {
        if (!m_Running) return;
        LOG(INFO, L"ElleActionExecutor stopping");
        m_Running = false;
        if (m_PollThread)
        {
            WaitForSingleObject(m_PollThread, 10000);
            ELLE_SAFE_CLOSE_HANDLE(m_PollThread);
        }
        UnloadDLLs();
        LOG(INFO, L"ElleActionExecutor stopped");
    }

    uint64_t ActionsExecuted() const    { return m_ActionsExecuted; }
    uint64_t ActionsFailed() const      { return m_ActionsFailed; }

private:
    void LoadDLL(LPCWSTR name, HMODULE& handle, const char* label)
    {
        handle = LoadLibraryW(name);
        if (!handle)
        {
            LOG(WARN, L"LoadDLL: %S not loaded (error=%lu) — that capability type will use fallback",
                label, GetLastError());
        }
        else
        {
            LOG(INFO, L"LoadDLL: %S loaded successfully", label);
        }
    }

    void UnloadDLLs()
    {
        if (m_hHardwareDLL) { FreeLibrary(m_hHardwareDLL); m_hHardwareDLL = nullptr; }
        if (m_hProcessDLL)  { FreeLibrary(m_hProcessDLL);  m_hProcessDLL  = nullptr; }
        if (m_hFileIODLL)   { FreeLibrary(m_hFileIODLL);   m_hFileIODLL   = nullptr; }
    }

    // Read current trust score from ElleCore.
    // Checks UpdatedAt freshness before returning the score.
    // If TrustState hasn't been updated within TRUST_STATE_MAX_AGE_SEC, the
    // C++ core may have stopped writing — log a warning and return the last
    // known value rather than silently acting on stale data.
    int ReadTrustScore()
    {
        ElleSQLScope coreConn(ElleDB::CORE);
        if (!coreConn.Valid()) return 0;

        int score = 0;
        int ageSec = -1;

        coreConn->Query(
            L"SELECT TOP 1 TrustScore, DATEDIFF(SECOND, UpdatedAt, GETDATE()) AS AgeSec "
            L"FROM ElleCore.dbo.TrustState ORDER BY UpdatedAt DESC",
            [&](const std::vector<std::wstring>& row)
            {
                if (row.size() >= 2)
                {
                    score  = _wtoi(row[0].c_str());
                    ageSec = _wtoi(row[1].c_str());
                }
            }
        );

        if (ageSec < 0)
        {
            LOG(WARN, L"ReadTrustScore: No TrustState row found — defaulting to 0");
            return 0;
        }

        if (ageSec > ElleConfig::Freshness::TRUST_STATE_MAX_AGE_SEC)
        {
            LOG(WARN, L"ReadTrustScore: TrustState is %d seconds old (max=%d) — "
                L"C++ core may have stopped updating. Using last known score=%d.",
                ageSec, ElleConfig::Freshness::TRUST_STATE_MAX_AGE_SEC, score);
            // Return last known value — stale trust is better than no trust gating.
            // The WARN log is the signal that something upstream is wrong.
        }

        LOG(TRACE, L"ReadTrustScore: score=%d age=%ds", score, ageSec);
        return score;
    }

    // Update trust score after action result
    void AdjustTrust(bool success)
    {
        ElleSQLScope coreConn(ElleDB::CORE);
        if (!coreConn.Valid()) return;

        int delta = success ? ElleConfig::Trust::SUCCESS_REWARD : -ElleConfig::Trust::FAILURE_PENALTY;
        coreConn->ExecuteParams(
            L"UPDATE ElleCore.dbo.TrustState SET TrustScore = TrustScore + ?, UpdatedAt = GETDATE()",
            { std::to_wstring(delta) }
        );
        LOG(DEBUG, L"AdjustTrust: delta=%d success=%d", delta, success ? 1 : 0);
    }

    static DWORD WINAPI PollThreadProc(LPVOID param)
    {
        auto* exec = static_cast<ElleActionExecutor*>(param);
        LOG(INFO, L"Action poll loop running. Interval=%dms", ElleConfig::Queue::POLL_INTERVAL_MS);

        while (exec->m_Running)
        {
            DWORD wait = WaitForSingleObject(exec->m_StopEvent, ElleConfig::Queue::POLL_INTERVAL_MS);
            if (wait == WAIT_OBJECT_0) break;
            exec->PollAndExecute();
        }

        LOG(INFO, L"Action poll loop exiting");
        return 0;
    }

    void PollAndExecute()
    {
        ElleActionQueue actionQueue;
        std::vector<ElleAction> actions;

        ElleResult r = actionQueue.GetPendingActions(actions, ElleConfig::Queue::MAX_BATCH_SIZE);
        if (r == ElleResult::ERR_QUEUE_EMPTY || actions.empty())
            return;

        LOG(DEBUG, L"PollAndExecute: %zu actions to execute", actions.size());

        int currentTrust = ReadTrustScore();
        const std::wstring& currentEpoch = ElleEpochManager::Get().CurrentEpoch();

        for (auto& action : actions)
        {
            // Epoch gate: reject actions from a prior run that slipped through reclaim.
            // This should be rare — ReclaimStranded on startup catches most of them.
            if (!action.EpochID.empty() && !currentEpoch.empty() && action.EpochID != currentEpoch)
            {
                LOG(WARN, L"ActionID=%lld has EpochID=%s, current epoch=%s — "
                    L"stale action from prior run, marking STALE",
                    action.ActionID, action.EpochID.c_str(), currentEpoch.c_str());
                actionQueue.SubmitActionResult(action.ActionID, ElleActionStatus::FAILED,
                    L"{\"error\":\"stale_epoch\"}");
                continue;
            }

            // Trust gate check
            if ((int)action.TrustRequired > currentTrust)
            {
                LOG(WARN, L"ActionID=%lld Type=%d requires trust=%lu but current=%d — REJECTED",
                    action.ActionID, (int)action.Type, action.TrustRequired, currentTrust);

                actionQueue.SubmitActionResult(action.ActionID, ElleActionStatus::FAILED,
                    L"{\"error\":\"trust_insufficient\"}");
                AdjustTrust(false);
                m_ActionsFailed.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            std::wstring resultJson;
            ElleResult execResult = DispatchAction(action, resultJson);

            ElleActionStatus finalStatus = (execResult == ElleResult::OK)
                ? ElleActionStatus::SUCCESS
                : ElleActionStatus::FAILED;

            actionQueue.SubmitActionResult(action.ActionID, finalStatus, resultJson);
            AdjustTrust(execResult == ElleResult::OK);

            if (execResult == ElleResult::OK)
                m_ActionsExecuted.fetch_add(1, std::memory_order_relaxed);
            else
                m_ActionsFailed.fetch_add(1, std::memory_order_relaxed);
        }
    }

    ElleResult DispatchAction(const ElleAction& action, std::wstring& outResult)
    {
        LOG(INFO, L"DispatchAction: ActionID=%lld Type=%d", action.ActionID, (int)action.Type);

        switch (action.Type)
        {
            case ElleActionType::VIBRATE:
                return ExecuteVibrate(action, outResult);

            case ElleActionType::FLASH:
                return ExecuteFlash(action, outResult);

            case ElleActionType::NOTIFY:
                return ExecuteNotify(action, outResult);

            case ElleActionType::OPEN_APP:
                return ExecuteOpenApp(action, outResult);

            case ElleActionType::SAVE_MEMORY:
                return ExecuteSaveMemory(action, outResult);

            case ElleActionType::READ_MEMORY:
                return ExecuteReadMemory(action, outResult);

            case ElleActionType::WRITE_FILE:
                return ExecuteWriteFile(action, outResult);

            case ElleActionType::READ_FILE:
                return ExecuteReadFile(action, outResult);

            case ElleActionType::EXEC_PROCESS:
                return ExecuteProcess(action, outResult);

            case ElleActionType::KILL_PROCESS:
                return ExecuteKillProcess(action, outResult);

            case ElleActionType::GET_SYSTEM_INFO:
                return ExecuteGetSystemInfo(action, outResult);

            case ElleActionType::SET_CPU_AFFINITY:
                return ExecuteSetCpuAffinity(action, outResult);

            case ElleActionType::WS_BROADCAST:
                return ExecuteWSBroadcast(action, outResult);

            default:
                LOG(WARN, L"DispatchAction: No executor for type=%d", (int)action.Type);
                outResult = L"{\"error\":\"unsupported_action_type\"}";
                return ElleResult::ERR_ACTION_NOT_FOUND;
        }
    }

    // -------------------------------------------------------------------------
    // VIBRATE — calls Hardware DLL or queues command to Android via WS
    // -------------------------------------------------------------------------
    ElleResult ExecuteVibrate(const ElleAction& action, std::wstring& outResult)
    {
        LOG(INFO, L"ExecuteVibrate");

        // Android vibrate: push command through WebSocket
        // The Android app handles vibrate locally when it receives this
        return ExecuteWSBroadcast_Raw(L"{\"command\":\"vibrate\",\"duration\":500}", outResult);
    }

    // -------------------------------------------------------------------------
    // FLASH — toggle flashlight via Android WebSocket command
    // -------------------------------------------------------------------------
    ElleResult ExecuteFlash(const ElleAction& action, std::wstring& outResult)
    {
        LOG(INFO, L"ExecuteFlash");
        return ExecuteWSBroadcast_Raw(L"{\"command\":\"flash\"}", outResult);
    }

    // -------------------------------------------------------------------------
    // NOTIFY — push a notification to Android via WebSocket
    // -------------------------------------------------------------------------
    ElleResult ExecuteNotify(const ElleAction& action, std::wstring& outResult)
    {
        LOG(INFO, L"ExecuteNotify: Data=%s", action.ActionData.c_str());

        // Wrap in a notify command envelope if not already
        std::wstring payload;
        if (action.ActionData.find(L"command") == std::wstring::npos)
            payload = L"{\"command\":\"notify\",\"data\":" + action.ActionData + L"}";
        else
            payload = action.ActionData;

        return ExecuteWSBroadcast_Raw(payload, outResult);
    }

    // -------------------------------------------------------------------------
    // OPEN_APP — tell Android to open a specific app
    // -------------------------------------------------------------------------
    ElleResult ExecuteOpenApp(const ElleAction& action, std::wstring& outResult)
    {
        LOG(INFO, L"ExecuteOpenApp: Data=%s", action.ActionData.c_str());
        std::wstring payload = L"{\"command\":\"open_app\",\"data\":" + action.ActionData + L"}";
        return ExecuteWSBroadcast_Raw(payload, outResult);
    }

    // -------------------------------------------------------------------------
    // SAVE_MEMORY — write directly to ElleMemory
    // -------------------------------------------------------------------------
    ElleResult ExecuteSaveMemory(const ElleAction& action, std::wstring& outResult)
    {
        LOG(INFO, L"ExecuteSaveMemory");

        ElleSQLScope memConn(ElleDB::MEMORY);
        if (!memConn.Valid())
        {
            outResult = L"{\"error\":\"db_unavailable\"}";
            return ElleResult::ERR_SQL_CONNECT;
        }

        ElleResult r = memConn->ExecuteParams(
            L"INSERT INTO ElleMemory.dbo.Memories "
            L"(Content, Tags, Tier, RelevanceScore, EmotionalWeight, CreatedAt, LastRecalled, RecallCount) "
            L"VALUES (?, '', 0, 0.5, 0.5, GETDATE(), GETDATE(), 0)",  // Tier 0 = STM initially
            { action.ActionData }
        );

        if (r != ElleResult::OK)
        {
            outResult = L"{\"error\":\"write_failed\"}";
            return r;
        }

        int64_t memID = 0;
        memConn->Query(L"SELECT SCOPE_IDENTITY()",
            [&](const std::vector<std::wstring>& row) { if (!row.empty()) memID = _wtoi64(row[0].c_str()); });

        LOG(INFO, L"ExecuteSaveMemory: MemoryID=%lld stored", memID);
        wchar_t resp[128] = {};
        _snwprintf_s(resp, _countof(resp), _TRUNCATE, L"{\"result\":\"ok\",\"memory_id\":%lld}", memID);
        outResult = resp;
        return ElleResult::OK;
    }

    // -------------------------------------------------------------------------
    // READ_MEMORY — retrieve relevant memories
    // -------------------------------------------------------------------------
    ElleResult ExecuteReadMemory(const ElleAction& action, std::wstring& outResult)
    {
        LOG(INFO, L"ExecuteReadMemory");

        ElleSQLScope memConn(ElleDB::MEMORY);
        if (!memConn.Valid())
        {
            outResult = L"{\"error\":\"db_unavailable\"}";
            return ElleResult::ERR_SQL_CONNECT;
        }

        std::wstring jsonArray = L"[";
        bool first = true;

        memConn->Query(
            L"SELECT TOP 5 MemoryID, Content, RelevanceScore FROM ElleMemory.dbo.Memories "
            L"WHERE Tier = 1 ORDER BY RelevanceScore DESC",
            [&](const std::vector<std::wstring>& row)
            {
                if (!first) jsonArray += L",";
                first = false;
                jsonArray += L"{\"id\":" + row[0] + L",\"content\":\"" + row[1] +
                             L"\",\"score\":" + row[2] + L"}";
            }
        );

        jsonArray += L"]";
        outResult = L"{\"result\":\"ok\",\"memories\":" + jsonArray + L"}";
        return ElleResult::OK;
    }

    // -------------------------------------------------------------------------
    // WRITE_FILE — calls Elle.ASM.FileIO.dll WriteFile export
    // -------------------------------------------------------------------------
    ElleResult ExecuteWriteFile(const ElleAction& action, std::wstring& outResult)
    {
        LOG(INFO, L"ExecuteWriteFile");

        if (!m_hFileIODLL)
        {
            LOG(WARN, L"ExecuteWriteFile: FileIO DLL not loaded");
            outResult = L"{\"error\":\"dll_not_loaded\"}";
            return ElleResult::ERR_ASM_CALL;
        }

        auto pfnWrite = (PfnWriteFile)GetProcAddress(m_hFileIODLL, "ElleWriteFile");
        if (!pfnWrite)
        {
            LOG(ERROR, L"ExecuteWriteFile: GetProcAddress(ElleWriteFile) failed: %lu", GetLastError());
            outResult = L"{\"error\":\"proc_not_found\"}";
            return ElleResult::ERR_ASM_CALL;
        }

        // ActionData format: {"path":"C:\\Elle\\...","content":"..."}
        // Simple extraction — path and content
        auto extractField = [&](const std::wstring& json, const std::wstring& key) -> std::wstring
        {
            std::wstring search = L"\"" + key + L"\":\"";
            size_t pos = json.find(search);
            if (pos == std::wstring::npos) return L"";
            pos += search.size();
            size_t end = json.find(L'"', pos);
            if (end == std::wstring::npos) return L"";
            return json.substr(pos, end - pos);
        };

        std::wstring path    = extractField(action.ActionData, L"path");
        std::wstring content = extractField(action.ActionData, L"content");

        if (path.empty())
        {
            outResult = L"{\"error\":\"missing_path\"}";
            return ElleResult::ERR_INVALID_PARAM;
        }

        BOOL ok = pfnWrite(path.c_str(), content.c_str());
        if (!ok)
        {
            LOG(ERROR, L"ExecuteWriteFile: ElleWriteFile returned FALSE for path=%s", path.c_str());
            outResult = L"{\"error\":\"write_failed\"}";
            return ElleResult::ERR_ASM_CALL;
        }

        LOG(INFO, L"ExecuteWriteFile: Wrote to path=%s", path.c_str());
        outResult = L"{\"result\":\"ok\"}";
        return ElleResult::OK;
    }

    // -------------------------------------------------------------------------
    // READ_FILE — calls Elle.ASM.FileIO.dll ReadFile export
    // -------------------------------------------------------------------------
    ElleResult ExecuteReadFile(const ElleAction& action, std::wstring& outResult)
    {
        LOG(INFO, L"ExecuteReadFile");

        if (!m_hFileIODLL)
        {
            outResult = L"{\"error\":\"dll_not_loaded\"}";
            return ElleResult::ERR_ASM_CALL;
        }

        auto pfnRead = (PfnReadFile)GetProcAddress(m_hFileIODLL, "ElleReadFile");
        if (!pfnRead)
        {
            outResult = L"{\"error\":\"proc_not_found\"}";
            return ElleResult::ERR_ASM_CALL;
        }

        // Extract path from action data
        std::wstring path;
        size_t pos = action.ActionData.find(L"\"path\":\"");
        if (pos != std::wstring::npos)
        {
            pos += 8;
            size_t end = action.ActionData.find(L'"', pos);
            if (end != std::wstring::npos)
                path = action.ActionData.substr(pos, end - pos);
        }

        if (path.empty())
        {
            outResult = L"{\"error\":\"missing_path\"}";
            return ElleResult::ERR_INVALID_PARAM;
        }

        wchar_t buf[65536] = {};
        BOOL ok = pfnRead(path.c_str(), buf, _countof(buf));
        if (!ok)
        {
            outResult = L"{\"error\":\"read_failed\"}";
            return ElleResult::ERR_ASM_CALL;
        }

        LOG(INFO, L"ExecuteReadFile: Read %zu chars from %s", wcslen(buf), path.c_str());
        outResult = L"{\"result\":\"ok\",\"content\":\"" + std::wstring(buf) + L"\"}";
        return ElleResult::OK;
    }

    // -------------------------------------------------------------------------
    // EXEC_PROCESS — calls Elle.ASM.Process.dll LaunchProcess export
    // -------------------------------------------------------------------------
    ElleResult ExecuteProcess(const ElleAction& action, std::wstring& outResult)
    {
        LOG(INFO, L"ExecuteProcess: %s", action.ActionData.c_str());

        if (!m_hProcessDLL)
        {
            outResult = L"{\"error\":\"dll_not_loaded\"}";
            return ElleResult::ERR_ASM_CALL;
        }

        auto pfnLaunch = (PfnLaunchProcess)GetProcAddress(m_hProcessDLL, "ElleLaunchProcess");
        if (!pfnLaunch)
        {
            outResult = L"{\"error\":\"proc_not_found\"}";
            return ElleResult::ERR_ASM_CALL;
        }

        // ActionData: {"exe":"path","args":"..."}
        // Extract exe path — args optional
        std::wstring exe, args;
        auto extract = [&](const std::wstring& json, const std::wstring& key) -> std::wstring
        {
            std::wstring search = L"\"" + key + L"\":\"";
            size_t pos = json.find(search);
            if (pos == std::wstring::npos) return L"";
            pos += search.size();
            size_t end = json.find(L'"', pos);
            return (end != std::wstring::npos) ? json.substr(pos, end - pos) : L"";
        };

        exe  = extract(action.ActionData, L"exe");
        args = extract(action.ActionData, L"args");

        if (exe.empty())
        {
            outResult = L"{\"error\":\"missing_exe\"}";
            return ElleResult::ERR_INVALID_PARAM;
        }

        DWORD pid = 0;
        BOOL ok = pfnLaunch(exe.c_str(), args.empty() ? nullptr : args.c_str(), &pid);

        if (!ok)
        {
            LOG(ERROR, L"ExecuteProcess: ElleLaunchProcess failed for exe=%s", exe.c_str());
            outResult = L"{\"error\":\"launch_failed\"}";
            return ElleResult::ERR_ASM_CALL;
        }

        LOG(INFO, L"ExecuteProcess: Launched exe=%s PID=%lu", exe.c_str(), pid);
        wchar_t resp[128] = {};
        _snwprintf_s(resp, _countof(resp), _TRUNCATE, L"{\"result\":\"ok\",\"pid\":%lu}", pid);
        outResult = resp;
        return ElleResult::OK;
    }

    // -------------------------------------------------------------------------
    // KILL_PROCESS
    // -------------------------------------------------------------------------
    ElleResult ExecuteKillProcess(const ElleAction& action, std::wstring& outResult)
    {
        LOG(INFO, L"ExecuteKillProcess: %s", action.ActionData.c_str());

        if (!m_hProcessDLL)
        {
            outResult = L"{\"error\":\"dll_not_loaded\"}";
            return ElleResult::ERR_ASM_CALL;
        }

        auto pfnKill = (PfnKillProcess)GetProcAddress(m_hProcessDLL, "ElleKillProcess");
        if (!pfnKill)
        {
            outResult = L"{\"error\":\"proc_not_found\"}";
            return ElleResult::ERR_ASM_CALL;
        }

        // ActionData: {"pid":12345}
        DWORD pid = 0;
        size_t pos = action.ActionData.find(L"\"pid\":");
        if (pos != std::wstring::npos)
            pid = (DWORD)_wtoi(action.ActionData.c_str() + pos + 6);

        if (pid == 0)
        {
            outResult = L"{\"error\":\"missing_pid\"}";
            return ElleResult::ERR_INVALID_PARAM;
        }

        BOOL ok = pfnKill(pid);
        if (!ok)
        {
            LOG(ERROR, L"ExecuteKillProcess: PID=%lu kill failed", pid);
            outResult = L"{\"error\":\"kill_failed\"}";
            return ElleResult::ERR_ASM_CALL;
        }

        LOG(INFO, L"ExecuteKillProcess: PID=%lu terminated", pid);
        outResult = L"{\"result\":\"ok\"}";
        return ElleResult::OK;
    }

    // -------------------------------------------------------------------------
    // GET_SYSTEM_INFO
    // -------------------------------------------------------------------------
    ElleResult ExecuteGetSystemInfo(const ElleAction& action, std::wstring& outResult)
    {
        LOG(INFO, L"ExecuteGetSystemInfo");

        DWORD cpuPercent = 0;
        DWORDLONG freeMemBytes = 0;

        if (m_hHardwareDLL)
        {
            auto pfnGetLoad = (PfnGetSystemLoad)GetProcAddress(m_hHardwareDLL, "ElleGetSystemLoad");
            if (pfnGetLoad)
                pfnGetLoad(&cpuPercent, &freeMemBytes);
        }
        else
        {
            // Fallback: use Windows API directly
            MEMORYSTATUSEX ms = {};
            ms.dwLength = sizeof(ms);
            GlobalMemoryStatusEx(&ms);
            freeMemBytes = ms.ullAvailPhys;
            cpuPercent = 0;  // Would need PDH for accurate CPU % — DLL handles this
        }

        wchar_t resp[256] = {};
        _snwprintf_s(resp, _countof(resp), _TRUNCATE,
            L"{\"result\":\"ok\",\"cpu_percent\":%lu,\"free_mem_bytes\":%llu}",
            cpuPercent, freeMemBytes);
        outResult = resp;
        return ElleResult::OK;
    }

    // -------------------------------------------------------------------------
    // SET_CPU_AFFINITY
    // -------------------------------------------------------------------------
    ElleResult ExecuteSetCpuAffinity(const ElleAction& action, std::wstring& outResult)
    {
        LOG(INFO, L"ExecuteSetCpuAffinity: %s", action.ActionData.c_str());

        if (!m_hHardwareDLL)
        {
            outResult = L"{\"error\":\"dll_not_loaded\"}";
            return ElleResult::ERR_ASM_CALL;
        }

        auto pfnAffinity = (PfnSetCpuAffinity)GetProcAddress(m_hHardwareDLL, "ElleSetCpuAffinity");
        if (!pfnAffinity)
        {
            outResult = L"{\"error\":\"proc_not_found\"}";
            return ElleResult::ERR_ASM_CALL;
        }

        // ActionData: {"pid":123,"mask":15}
        DWORD pid = 0;
        DWORD_PTR mask = 0;
        size_t pidPos  = action.ActionData.find(L"\"pid\":");
        size_t maskPos = action.ActionData.find(L"\"mask\":");

        if (pidPos != std::wstring::npos)   pid  = (DWORD)_wtoi(action.ActionData.c_str() + pidPos + 6);
        if (maskPos != std::wstring::npos)  mask = (DWORD_PTR)_wtoi64(action.ActionData.c_str() + maskPos + 7);

        BOOL ok = pfnAffinity(pid, mask);
        outResult = ok ? L"{\"result\":\"ok\"}" : L"{\"error\":\"affinity_failed\"}";
        return ok ? ElleResult::OK : ElleResult::ERR_ASM_CALL;
    }

    // -------------------------------------------------------------------------
    // WS_BROADCAST — push a message to all Android WebSocket clients
    // Uses a named pipe to communicate with Elle.Service.HTTP
    // -------------------------------------------------------------------------
    ElleResult ExecuteWSBroadcast(const ElleAction& action, std::wstring& outResult)
    {
        return ExecuteWSBroadcast_Raw(action.ActionData, outResult);
    }

    ElleResult ExecuteWSBroadcast_Raw(const std::wstring& message, std::wstring& outResult)
    {
        LOG(DEBUG, L"ExecuteWSBroadcast_Raw: %s", message.c_str());

        // Write the broadcast command to the IPC pipe that ElleHTTP listens on
        // Pipe name: \\.\pipe\ElleWSBroadcast
        HANDLE hPipe = CreateFileW(
            L"\\\\.\\pipe\\ElleWSBroadcast",
            GENERIC_WRITE,
            0, nullptr,
            OPEN_EXISTING,
            0, nullptr
        );

        if (hPipe == INVALID_HANDLE_VALUE)
        {
            DWORD err = GetLastError();
            if (err == ERROR_FILE_NOT_FOUND)
                LOG(WARN, L"ExecuteWSBroadcast_Raw: HTTP service pipe not available — message dropped");
            else
                LOG(ERROR, L"ExecuteWSBroadcast_Raw: CreateFile(pipe) failed: %lu", err);

            // Non-fatal: the HTTP service might not be running
            outResult = L"{\"result\":\"pipe_unavailable\"}";
            return ElleResult::OK;  // Don't fail the action just because HTTP is down
        }

        // Write message as UTF-16 LE with length prefix
        DWORD msgBytes = (DWORD)(message.size() * sizeof(wchar_t));
        DWORD written = 0;
        WriteFile(hPipe, &msgBytes, sizeof(msgBytes), &written, nullptr);
        WriteFile(hPipe, message.c_str(), msgBytes, &written, nullptr);
        CloseHandle(hPipe);

        LOG(DEBUG, L"ExecuteWSBroadcast_Raw: Sent %lu bytes to HTTP pipe", msgBytes);
        outResult = L"{\"result\":\"ok\",\"bytes\":" + std::to_wstring(msgBytes) + L"}";
        return ElleResult::OK;
    }

    HMODULE         m_hHardwareDLL;
    HMODULE         m_hProcessDLL;
    HMODULE         m_hFileIODLL;
    HANDLE          m_PollThread;
    HANDLE          m_StopEvent;
    volatile bool   m_Running;
    std::atomic<uint64_t> m_ActionsExecuted{ 0 };
    std::atomic<uint64_t> m_ActionsFailed{ 0 };
};

// =============================================================================
// ElleActionService
// =============================================================================
class ElleActionService : public ElleServiceBase
{
public:
    ElleActionService() : ElleServiceBase(SVCNAME) {}

protected:
    ElleResult OnStart() override
    {
        LOG(INFO, L"OnStart: Initializing Action service");
        ElleResult r = InitSharedInfrastructure();
        if (r != ElleResult::OK) return r;

        // Adopt the epoch written by QueueWorker
        r = ElleEpochManager::Get().Init(false);
        if (r != ElleResult::OK)
            LOG(WARN, L"EpochManager::Init failed: %s — epoch enforcement disabled", ElleResultStr(r));

        RegisterWorker();

        r = m_Executor.Start(StopEvent());
        if (r != ElleResult::OK)
        {
            LOG(FATAL, L"ActionExecutor::Start() failed: %s", ElleResultStr(r));
            return r;
        }

        LOG(INFO, L"Action service running");
        return ElleResult::OK;
    }

    void OnStop() override
    {
        LOG(INFO, L"OnStop: Stopping Action service. Executed=%llu Failed=%llu",
            m_Executor.ActionsExecuted(), m_Executor.ActionsFailed());
        m_Executor.Stop();

        UnregisterWorker();

        ShutdownSharedInfrastructure();
    }

private:
    ElleActionExecutor m_Executor;
};

int wmain(int argc, wchar_t* argv[])
{
    ElleActionService svc;

    if (argc >= 2)
    {
        if (_wcsicmp(argv[1], L"install") == 0)
        {
            wchar_t exePath[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, exePath, MAX_PATH);
            return svc.Install(L"Elle Action Executor",
                L"Elle-Ann ESI — Action lifecycle, capability execution, ASM DLL dispatch, trust gating.",
                exePath) == ElleResult::OK ? 0 : 1;
        }
        else if (_wcsicmp(argv[1], L"uninstall") == 0)
            return svc.Uninstall() == ElleResult::OK ? 0 : 1;
        else if (_wcsicmp(argv[1], L"console") == 0) { svc.RunAsConsole(); return 0; }
    }

    // Double-click (no args) interactive install path
    if (argc == 1 && ElleServiceBase::IsInteractiveSession())
    {
        wchar_t exePath[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        ElleResult r = svc.Install(L"Elle Action Executor",
            L"Elle-Ann ESI — Action lifecycle, capability execution, ASM DLL dispatch, trust gating.",
            exePath);

        if (r == ElleResult::OK)
            MessageBoxW(nullptr, L"Service uploaded ok", L"Install", MB_OK | MB_ICONINFORMATION);
        else
            MessageBoxW(nullptr, L"Service upload failed", L"Install", MB_OK | MB_ICONERROR);

        return (r == ElleResult::OK) ? 0 : 1;
    }

    svc.RunAsService();
    return 0;
}
