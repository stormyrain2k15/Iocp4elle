// =============================================================================
// Elle.Lua.Behavioral — Service.cpp
//
// Windows Service that hosts the Lua 5.4 behavioral engine.
// Loads personality.lua, intent_scoring.lua, reasoning.lua, thresholds.lua.
// Watches scripts for changes and hot-reloads without service restart.
// Exposes C++ bindings to Lua for SQL, queue, emotional state, and logging.
// =============================================================================

#include "../Shared/ElleServiceBase.h"
#include "LuaHost.h"

#define SVCNAME     ElleConfig::ServiceNames::LUA_BEHAVIORAL
#define LOG(lvl, fmt, ...) ELLE_LOG_##lvl(SVCNAME, fmt, ##__VA_ARGS__)

class ElleLuaService : public ElleServiceBase
{
public:
    ElleLuaService() : ElleServiceBase(SVCNAME) {}

protected:
    ElleResult OnStart() override
    {
        LOG(INFO, L"OnStart: Initializing Lua Behavioral service");

        ElleResult r = InitSharedInfrastructure();
        if (r != ElleResult::OK) return r;

        {
            ElleSQLScope sysConn(ElleDB::SYSTEM);
            if (sysConn.Valid())
                sysConn->ExecuteParams(
                    L"IF EXISTS (SELECT 1 FROM ElleSystem.dbo.Workers WHERE WorkerName = ?) "
                    L"UPDATE ElleSystem.dbo.Workers SET StartedAt = GETDATE(), LastHeartbeat = GETDATE(), Status = 1 WHERE WorkerName = ? "
                    L"ELSE INSERT INTO ElleSystem.dbo.Workers (WorkerName, StartedAt, LastHeartbeat, Status) VALUES (?, GETDATE(), GETDATE(), 1)",
                    { std::wstring(SVCNAME), std::wstring(SVCNAME), std::wstring(SVCNAME) });
        }

        r = m_Host.Init();
        if (r != ElleResult::OK)
        {
            // Non-fatal: the behavioral layer enhances Elle but doesn't block operation
            LOG(WARN, L"LuaHost::Init() returned %s — service continuing without Lua layer", ElleResultStr(r));
        }
        else
        {
            r = m_Host.StartHotReload(StopEvent());
            if (r != ElleResult::OK)
                LOG(WARN, L"StartHotReload failed: %s — hot-reload disabled", ElleResultStr(r));
        }

        LOG(INFO, L"Lua Behavioral service running. LuaLoaded=%d", m_Host.IsLoaded() ? 1 : 0);
        return ElleResult::OK;
    }

    void OnStop() override
    {
        LOG(INFO, L"OnStop: Stopping Lua Behavioral service");
        m_Host.Shutdown();

        ElleSQLScope sysConn(ElleDB::SYSTEM);
        if (sysConn.Valid())
            sysConn->ExecuteParams(
                L"UPDATE ElleSystem.dbo.Workers SET Status = 0, StoppedAt = GETDATE() WHERE WorkerName = ?",
                { std::wstring(SVCNAME) });

        ShutdownSharedInfrastructure();
    }

private:
    ElleLuaHost m_Host;
};

int wmain(int argc, wchar_t* argv[])
{
    ElleLuaService svc;

    if (argc >= 2)
    {
        if (_wcsicmp(argv[1], L"install") == 0)
        {
            wchar_t exePath[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, exePath, MAX_PATH);
            return svc.Install(L"Elle Lua Behavioral Engine",
                L"Elle-Ann ESI — Lua 5.4 behavioral scripting. Personality, reasoning, intent scoring. Hot-reloaded.",
                exePath) == ElleResult::OK ? 0 : 1;
        }
        else if (_wcsicmp(argv[1], L"uninstall") == 0)
            return svc.Uninstall() == ElleResult::OK ? 0 : 1;
        else if (_wcsicmp(argv[1], L"console") == 0) { svc.RunAsConsole(); return 0; }
    }

    svc.RunAsService();
    return 0;
}
