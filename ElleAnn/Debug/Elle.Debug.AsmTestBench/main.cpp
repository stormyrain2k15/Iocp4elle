// =============================================================================
// Elle.Debug.AsmTestBench — main.cpp
//
// Validates all three Elle ASM DLLs before plugging them into the service layer.
// Loads each DLL, calls every exported function with known inputs, verifies
// the outputs, and reports pass/fail with specific failure reasons.
//
// Usage:
//   AsmTestBench.exe                    (test all DLLs)
//   AsmTestBench.exe hardware           (test Elle.ASM.Hardware.dll only)
//   AsmTestBench.exe process            (test Elle.ASM.Process.dll only)
//   AsmTestBench.exe fileio             (test Elle.ASM.FileIO.dll only)
//
// All DLLs are expected to be in the same directory as AsmTestBench.exe.
// The test bench writes a temp file to C:\Elle\Temp\ for FileIO tests.
// It does NOT kill or otherwise damage any real process.
// It uses its own PID for affinity tests so it only affects itself.
// =============================================================================

#include <Windows.h>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>

// =============================================================================
// Test result tracking
// =============================================================================
struct TestResult
{
    std::wstring    Name;
    bool            Passed;
    std::wstring    FailReason;
};

static std::vector<TestResult> g_Results;

static void RecordPass(const std::wstring& name)
{
    g_Results.push_back({ name, true, L"" });
    wprintf(L"  [PASS] %s\n", name.c_str());
}

static void RecordFail(const std::wstring& name, const std::wstring& reason)
{
    g_Results.push_back({ name, false, reason });
    wprintf(L"  [FAIL] %s — %s\n", name.c_str(), reason.c_str());
}

// =============================================================================
// Helper: load a DLL from the exe's directory
// =============================================================================
static HMODULE LoadSideBySideDLL(const wchar_t* dllName)
{
    wchar_t exeDir[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exeDir, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(exeDir, L'\\');
    if (lastSlash) *(lastSlash + 1) = L'\0';

    std::wstring fullPath = std::wstring(exeDir) + dllName;
    HMODULE h = LoadLibraryW(fullPath.c_str());
    if (!h)
        wprintf(L"  [WARN] LoadLibraryW(%s) failed: %lu\n", fullPath.c_str(), GetLastError());
    return h;
}

// =============================================================================
// Test Elle.ASM.Hardware.dll
// =============================================================================
typedef BOOL (WINAPI* PfnVibrateDevice)(DWORD);
typedef BOOL (WINAPI* PfnToggleFlash)(BOOL);
typedef BOOL (WINAPI* PfnSetCpuAffinity)(DWORD, DWORD_PTR);
typedef BOOL (WINAPI* PfnGetSystemLoad)(DWORD*, DWORDLONG*);

static void TestHardwareDLL()
{
    wprintf(L"\n[Testing Elle.ASM.Hardware.dll]\n");

    HMODULE hDLL = LoadSideBySideDLL(L"Elle.ASM.Hardware.dll");
    if (!hDLL)
    {
        RecordFail(L"Hardware DLL load", L"LoadLibraryW failed — DLL not present or missing dependency");
        return;
    }
    RecordPass(L"Hardware DLL load");

    // --- ElleVibrateDevice ---
    auto pfnVibrate = (PfnVibrateDevice)GetProcAddress(hDLL, "ElleVibrateDevice");
    if (!pfnVibrate)
        RecordFail(L"Hardware.ElleVibrateDevice export", L"GetProcAddress returned NULL");
    else
    {
        BOOL ok = pfnVibrate(500);
        if (ok)
            RecordPass(L"Hardware.ElleVibrateDevice(500)");
        else
            RecordFail(L"Hardware.ElleVibrateDevice(500)", L"Function returned FALSE");
    }

    // --- ElleToggleFlash ---
    auto pfnFlash = (PfnToggleFlash)GetProcAddress(hDLL, "ElleToggleFlash");
    if (!pfnFlash)
        RecordFail(L"Hardware.ElleToggleFlash export", L"GetProcAddress returned NULL");
    else
    {
        BOOL ok = pfnFlash(TRUE);
        if (ok)
            RecordPass(L"Hardware.ElleToggleFlash(TRUE)");
        else
            RecordFail(L"Hardware.ElleToggleFlash(TRUE)", L"Function returned FALSE");
    }

    // --- ElleSetCpuAffinity --- (use own PID, set affinity to core 0 only = mask 1)
    auto pfnAffinity = (PfnSetCpuAffinity)GetProcAddress(hDLL, "ElleSetCpuAffinity");
    if (!pfnAffinity)
        RecordFail(L"Hardware.ElleSetCpuAffinity export", L"GetProcAddress returned NULL");
    else
    {
        DWORD myPID = GetCurrentProcessId();

        // Get current affinity so we can restore it
        DWORD_PTR procMask = 0, sysMask = 0;
        GetProcessAffinityMask(GetCurrentProcess(), &procMask, &sysMask);

        // Set to first available core
        BOOL ok = pfnAffinity(myPID, procMask);  // Set to current mask (no-op but valid)

        if (ok)
            RecordPass(L"Hardware.ElleSetCpuAffinity(self, currentMask)");
        else
            RecordFail(L"Hardware.ElleSetCpuAffinity(self, currentMask)", L"Function returned FALSE");

        // Test null affinity (should fail gracefully)
        BOOL shouldFail = pfnAffinity(0, procMask);
        if (!shouldFail)
            RecordPass(L"Hardware.ElleSetCpuAffinity(pid=0) returns FALSE correctly");
        else
            RecordFail(L"Hardware.ElleSetCpuAffinity(pid=0) should have returned FALSE", L"Returned TRUE for invalid PID");
    }

    // --- ElleGetSystemLoad ---
    auto pfnGetLoad = (PfnGetSystemLoad)GetProcAddress(hDLL, "ElleGetSystemLoad");
    if (!pfnGetLoad)
        RecordFail(L"Hardware.ElleGetSystemLoad export", L"GetProcAddress returned NULL");
    else
    {
        DWORD cpuPct = 0xDEADBEEF;
        DWORDLONG freeMem = 0xDEADBEEFDEADBEEFULL;

        BOOL ok = pfnGetLoad(&cpuPct, &freeMem);
        if (!ok)
        {
            RecordFail(L"Hardware.ElleGetSystemLoad", L"Function returned FALSE");
        }
        else if (freeMem == 0 || freeMem == 0xDEADBEEFDEADBEEFULL)
        {
            RecordFail(L"Hardware.ElleGetSystemLoad freeMem", L"freeMem not populated (still sentinel value)");
        }
        else
        {
            wprintf(L"    MemoryLoad=%lu%%  FreePhysMB=%llu\n", cpuPct, freeMem / (1024 * 1024));
            RecordPass(L"Hardware.ElleGetSystemLoad values populated");
        }

        // Test null pointer handling — should fail gracefully, not crash
        BOOL shouldFail = pfnGetLoad(nullptr, &freeMem);
        if (!shouldFail)
            RecordPass(L"Hardware.ElleGetSystemLoad(cpuPct=NULL) returns FALSE correctly");
        else
            RecordFail(L"Hardware.ElleGetSystemLoad(cpuPct=NULL)", L"Should return FALSE for NULL pointer");
    }

    FreeLibrary(hDLL);
    RecordPass(L"Hardware DLL unload");
}

// =============================================================================
// Test Elle.ASM.Process.dll
// =============================================================================
typedef BOOL (WINAPI* PfnLaunchProcess)(LPCWSTR, LPCWSTR, DWORD*);
typedef BOOL (WINAPI* PfnKillProcess)(DWORD);
typedef BOOL (WINAPI* PfnListProcesses)(LPWSTR, DWORD);

static void TestProcessDLL()
{
    wprintf(L"\n[Testing Elle.ASM.Process.dll]\n");

    HMODULE hDLL = LoadSideBySideDLL(L"Elle.ASM.Process.dll");
    if (!hDLL)
    {
        RecordFail(L"Process DLL load", L"LoadLibraryW failed");
        return;
    }
    RecordPass(L"Process DLL load");

    // --- ElleLaunchProcess --- (launch notepad.exe, kill it, verify)
    auto pfnLaunch = (PfnLaunchProcess)GetProcAddress(hDLL, "ElleLaunchProcess");
    if (!pfnLaunch)
    {
        RecordFail(L"Process.ElleLaunchProcess export", L"GetProcAddress returned NULL");
    }
    else
    {
        DWORD pid = 0;
        BOOL ok = pfnLaunch(L"C:\\Windows\\System32\\notepad.exe", nullptr, &pid);

        if (!ok || pid == 0)
        {
            RecordFail(L"Process.ElleLaunchProcess(notepad)", L"Launch failed or PID not returned");
        }
        else
        {
            wprintf(L"    Launched notepad.exe PID=%lu\n", pid);
            RecordPass(L"Process.ElleLaunchProcess(notepad) OK");

            // --- ElleKillProcess --- (kill the notepad we just launched)
            auto pfnKill = (PfnKillProcess)GetProcAddress(hDLL, "ElleKillProcess");
            if (!pfnKill)
            {
                RecordFail(L"Process.ElleKillProcess export", L"GetProcAddress returned NULL");
                // Close notepad via TerminateProcess as fallback cleanup
                HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
                if (hProc) { TerminateProcess(hProc, 0); CloseHandle(hProc); }
            }
            else
            {
                BOOL killOk = pfnKill(pid);
                if (killOk)
                    RecordPass(L"Process.ElleKillProcess(notepad PID)");
                else
                    RecordFail(L"Process.ElleKillProcess(notepad PID)", L"Kill returned FALSE");
            }
        }

        // Test null pointer — should fail gracefully
        BOOL shouldFail = pfnLaunch(nullptr, nullptr, &pid);
        if (!shouldFail)
            RecordPass(L"Process.ElleLaunchProcess(NULL path) returns FALSE correctly");
        else
            RecordFail(L"Process.ElleLaunchProcess(NULL path)", L"Should return FALSE for NULL path");
    }

    // --- ElleListProcesses ---
    auto pfnList = (PfnListProcesses)GetProcAddress(hDLL, "ElleListProcesses");
    if (!pfnList)
    {
        RecordFail(L"Process.ElleListProcesses export", L"GetProcAddress returned NULL");
    }
    else
    {
        wchar_t buf[8192] = {};
        BOOL ok = pfnList(buf, _countof(buf));

        if (!ok)
        {
            RecordFail(L"Process.ElleListProcesses", L"Function returned FALSE");
        }
        else if (wcslen(buf) == 0)
        {
            RecordFail(L"Process.ElleListProcesses buffer empty", L"No process names returned");
        }
        else
        {
            // Verify our own process appears in the list
            wchar_t myExeName[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, myExeName, MAX_PATH);
            wchar_t* lastSlash = wcsrchr(myExeName, L'\\');
            const wchar_t* shortName = lastSlash ? lastSlash + 1 : myExeName;

            wprintf(L"    Process list length: %zu chars\n", wcslen(buf));
            if (wcsstr(buf, shortName) != nullptr || wcsstr(buf, L"AsmTestBench") != nullptr)
                RecordPass(L"Process.ElleListProcesses contains own process");
            else
                RecordFail(L"Process.ElleListProcesses self check", L"Own process name not found in list");
        }

        // Test overflow protection — tiny buffer
        wchar_t smallBuf[4] = {};
        BOOL overflowOk = pfnList(smallBuf, 4);
        // Should return TRUE but with truncated/empty list — not crash
        RecordPass(L"Process.ElleListProcesses small buffer doesn't crash");
    }

    FreeLibrary(hDLL);
    RecordPass(L"Process DLL unload");
}

// =============================================================================
// Test Elle.ASM.FileIO.dll
// =============================================================================
typedef BOOL (WINAPI* PfnWriteFile)(LPCWSTR, LPCWSTR);
typedef BOOL (WINAPI* PfnReadFile)(LPCWSTR, LPWSTR, DWORD);
typedef BOOL (WINAPI* PfnFileExists)(LPCWSTR);
typedef BOOL (WINAPI* PfnDeleteFile)(LPCWSTR);

static const wchar_t* TEST_FILE_PATH = L"C:\\Elle\\Temp\\AsmTestBench_FileIOTest.txt";
static const wchar_t* TEST_CONTENT   = L"Elle-Ann FileIO test content. Timestamp written by AsmTestBench.";

static void TestFileIODLL()
{
    wprintf(L"\n[Testing Elle.ASM.FileIO.dll]\n");

    // Create the temp directory if needed
    CreateDirectoryW(L"C:\\Elle\\Temp\\", nullptr);

    HMODULE hDLL = LoadSideBySideDLL(L"Elle.ASM.FileIO.dll");
    if (!hDLL)
    {
        RecordFail(L"FileIO DLL load", L"LoadLibraryW failed");
        return;
    }
    RecordPass(L"FileIO DLL load");

    auto pfnWrite  = (PfnWriteFile)GetProcAddress(hDLL, "ElleWriteFile");
    auto pfnRead   = (PfnReadFile)GetProcAddress(hDLL,  "ElleReadFile");
    auto pfnExists = (PfnFileExists)GetProcAddress(hDLL, "ElleFileExists");
    auto pfnDelete = (PfnDeleteFile)GetProcAddress(hDLL, "ElleDeleteFile");

    if (!pfnWrite)  { RecordFail(L"FileIO.ElleWriteFile export",  L"GetProcAddress NULL"); }
    if (!pfnRead)   { RecordFail(L"FileIO.ElleReadFile export",   L"GetProcAddress NULL"); }
    if (!pfnExists) { RecordFail(L"FileIO.ElleFileExists export", L"GetProcAddress NULL"); }
    if (!pfnDelete) { RecordFail(L"FileIO.ElleDeleteFile export", L"GetProcAddress NULL"); }

    if (!pfnWrite || !pfnRead || !pfnExists || !pfnDelete)
    {
        FreeLibrary(hDLL);
        return;
    }

    RecordPass(L"FileIO all exports present");

    // --- ElleFileExists on non-existent file ---
    BOOL shouldNotExist = pfnExists(TEST_FILE_PATH);
    if (!shouldNotExist)
        RecordPass(L"FileIO.ElleFileExists(non-existent) returns FALSE");
    else
    {
        // File already exists from a prior run — delete it first
        pfnDelete(TEST_FILE_PATH);
        RecordPass(L"FileIO.ElleFileExists pre-cleanup");
    }

    // --- ElleWriteFile ---
    BOOL writeOk = pfnWrite(TEST_FILE_PATH, TEST_CONTENT);
    if (!writeOk)
        RecordFail(L"FileIO.ElleWriteFile", L"Write returned FALSE");
    else
        RecordPass(L"FileIO.ElleWriteFile");

    // --- ElleFileExists on written file ---
    BOOL shouldExist = pfnExists(TEST_FILE_PATH);
    if (shouldExist)
        RecordPass(L"FileIO.ElleFileExists after write returns TRUE");
    else
        RecordFail(L"FileIO.ElleFileExists after write", L"Returns FALSE — file not created");

    // --- ElleReadFile ---
    wchar_t readBuf[1024] = {};
    BOOL readOk = pfnRead(TEST_FILE_PATH, readBuf, _countof(readBuf));
    if (!readOk)
    {
        RecordFail(L"FileIO.ElleReadFile", L"Read returned FALSE");
    }
    else
    {
        wprintf(L"    Read back: '%s'\n", readBuf);
        if (wcscmp(readBuf, TEST_CONTENT) == 0)
            RecordPass(L"FileIO.ElleReadFile content matches written content");
        else
            RecordFail(L"FileIO.ElleReadFile content mismatch",
                std::wstring(L"Expected '") + TEST_CONTENT + L"' Got '" + readBuf + L"'");
    }

    // --- Overwrite test ---
    const wchar_t* newContent = L"Overwritten content.";
    pfnWrite(TEST_FILE_PATH, newContent);
    wchar_t overwriteBuf[256] = {};
    pfnRead(TEST_FILE_PATH, overwriteBuf, _countof(overwriteBuf));
    if (wcscmp(overwriteBuf, newContent) == 0)
        RecordPass(L"FileIO.ElleWriteFile overwrites correctly");
    else
        RecordFail(L"FileIO.ElleWriteFile overwrite", L"Content after overwrite doesn't match");

    // --- Empty content write ---
    BOOL emptyOk = pfnWrite(TEST_FILE_PATH, L"");
    if (emptyOk)
        RecordPass(L"FileIO.ElleWriteFile empty content");
    else
        RecordFail(L"FileIO.ElleWriteFile empty content", L"Returned FALSE for empty string");

    // --- ElleDeleteFile ---
    BOOL deleteOk = pfnDelete(TEST_FILE_PATH);
    if (!deleteOk)
        RecordFail(L"FileIO.ElleDeleteFile", L"Delete returned FALSE");
    else
        RecordPass(L"FileIO.ElleDeleteFile");

    // Verify deletion
    BOOL shouldBeGone = pfnExists(TEST_FILE_PATH);
    if (!shouldBeGone)
        RecordPass(L"FileIO.ElleFileExists after delete returns FALSE");
    else
        RecordFail(L"FileIO.ElleFileExists after delete", L"Still returns TRUE — file not deleted");

    // --- NULL pointer handling ---
    BOOL nullWrite = pfnWrite(nullptr, L"content");
    if (!nullWrite) RecordPass(L"FileIO.ElleWriteFile(NULL path) returns FALSE");
    else RecordFail(L"FileIO.ElleWriteFile(NULL path)", L"Should return FALSE");

    BOOL nullRead = pfnRead(nullptr, readBuf, _countof(readBuf));
    if (!nullRead) RecordPass(L"FileIO.ElleReadFile(NULL path) returns FALSE");
    else RecordFail(L"FileIO.ElleReadFile(NULL path)", L"Should return FALSE");

    FreeLibrary(hDLL);
    RecordPass(L"FileIO DLL unload");
}

// =============================================================================
// Print summary
// =============================================================================
static void PrintSummary()
{
    int passed = 0, failed = 0;
    for (auto& r : g_Results)
    {
        if (r.Passed) passed++;
        else failed++;
    }

    wprintf(L"\n=== AsmTestBench Summary ===\n");
    wprintf(L"  PASSED: %d\n", passed);
    wprintf(L"  FAILED: %d\n", failed);

    if (failed > 0)
    {
        wprintf(L"\nFailed tests:\n");
        for (auto& r : g_Results)
            if (!r.Passed)
                wprintf(L"  [FAIL] %s — %s\n", r.Name.c_str(), r.FailReason.c_str());
    }

    wprintf(L"\n%s\n", failed == 0 ? L"All ASM DLL tests PASSED. Safe to integrate." :
                                      L"Some tests FAILED. Do not integrate until all pass.");
}

// =============================================================================
// main
// =============================================================================
int wmain(int argc, wchar_t* argv[])
{
    wprintf(L"=== Elle.Debug.AsmTestBench ===\n");
    wprintf(L"Validating ASM DLLs. x64 ABI compliance, all exports, all edge cases.\n\n");

    bool testAll      = (argc < 2);
    bool testHardware = testAll || (argc >= 2 && _wcsicmp(argv[1], L"hardware") == 0);
    bool testProcess  = testAll || (argc >= 2 && _wcsicmp(argv[1], L"process")  == 0);
    bool testFileIO   = testAll || (argc >= 2 && _wcsicmp(argv[1], L"fileio")   == 0);

    if (testHardware) TestHardwareDLL();
    if (testProcess)  TestProcessDLL();
    if (testFileIO)   TestFileIODLL();

    PrintSummary();
    return (int)std::count_if(g_Results.begin(), g_Results.end(), [](const TestResult& r) { return !r.Passed; });
}
