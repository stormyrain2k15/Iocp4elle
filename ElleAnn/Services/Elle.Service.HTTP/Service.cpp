// =============================================================================
// Elle.Service.HTTP — Service.cpp
//
// Windows Service that hosts the HTTP/1.1 and WebSocket server.
// Replaces the FastAPI layer entirely. No framework. Raw Winsock.
// Handles all Android app and REST client communication.
// =============================================================================

#include "../../Shared/ElleServiceBase.h"
#include "../../Shared/ElleEpoch.h"
#include "../../Shared/ElleQueueIPC.h"
#include "HTTPServer.h"
#include "WSServer.h"
#include "RouteDispatch.h"
#include "PipeBroadcastListener.h"

#define SVCNAME     ElleConfig::ServiceNames::HTTP
#define LOG(lvl, fmt, ...) ELLE_LOG_##lvl(SVCNAME, fmt, ##__VA_ARGS__)

class ElleHTTPService : public ElleServiceBase
{
public:
    ElleHTTPService()
        : ElleServiceBase(SVCNAME)
        , m_Dispatch(m_HTTP, m_WS)
    {}

protected:
    ElleResult OnStart() override
    {
        LOG(INFO, L"OnStart: Initializing HTTP service");

        ElleResult r = InitSharedInfrastructure();
        if (r != ElleResult::OK)
            return r;

        ElleEpochManager::Get().Init(false);
        RegisterWorker();

        r = m_WS.Init();
        if (r != ElleResult::OK)
        {
            LOG(FATAL, L"WSServer::Init() failed: %s", ElleResultStr(r));
            return r;
        }

        r = m_HTTP.Init();
        if (r != ElleResult::OK)
        {
            LOG(FATAL, L"HTTPServer::Init() failed: %s", ElleResultStr(r));
            return r;
        }

        // Register all API routes before starting to accept connections
        m_Dispatch.RegisterAll();

        // Wire up WebSocket incoming message handler — push to IntentQueue
        m_WS.SetMessageCallback([](DWORD clientID, const std::string& message)
        {
            ELLE_LOG_INFO(SVCNAME, L"WS message from ClientID=%lu: %zu bytes", clientID, message.size());

            ElleSQLScope coreConn(ElleDB::CORE);
            if (!coreConn.Valid())
            {
                ELLE_LOG_ERROR(SVCNAME, L"WS message handler: DB unavailable for ClientID=%lu", clientID);
                return;
            }

            std::wstring wMsg(message.begin(), message.end());

            // Push as EXECUTE_COMMAND intent — commands from Android come through WebSocket
            coreConn->ExecuteParams(
                L"INSERT INTO ElleCore.dbo.IntentQueue (TypeID, StatusID, IntentData, TrustRequired, Priority, CreatedAt) "
                L"VALUES (9, 0, ?, 0, 8, GETDATE())",
                { wMsg }
            );
        });

        m_WS.SetDisconnectCallback([](DWORD clientID)
        {
            ELLE_LOG_INFO(SVCNAME, L"WS client disconnected. ClientID=%lu", clientID);
        });

        r = m_WS.Start();
        if (r != ElleResult::OK)
        {
            LOG(FATAL, L"WSServer::Start() failed: %s", ElleResultStr(r));
            return r;
        }

        r = m_HTTP.Start();
        if (r != ElleResult::OK)
        {
            LOG(FATAL, L"HTTPServer::Start() failed: %s", ElleResultStr(r));
            return r;
        }

        // Start the named pipe listener so Action service can broadcast to WebSocket clients
        ElleResult pipeResult = m_PipeListener.Start(StopEvent());
        if (pipeResult != ElleResult::OK)
            LOG(WARN, L"PipeBroadcastListener failed to start — WS_BROADCAST actions will be dropped");

        LOG(INFO, L"HTTP service running. Port=%d WebSocket ready. Pipe broadcast active.", ElleConfig::Network::HTTP_PORT);
        return ElleResult::OK;
    }

    void OnStop() override
    {
        LOG(INFO, L"OnStop: Stopping HTTP service. Requests=%llu WSClients=%d",
            m_HTTP.RequestsHandled(), m_WS.ConnectedCount());

        m_PipeListener.Stop();
        m_HTTP.Stop();
        m_WS.Stop();
        UnregisterWorker();
        ShutdownSharedInfrastructure();
    }

private:
    ElleHTTPServer              m_HTTP;
    ElleWSServer                m_WS;
    ElleRouteDispatch           m_Dispatch;
    EllePipeBroadcastListener   m_PipeListener{ m_WS };
};

int wmain(int argc, wchar_t* argv[])
{
    ElleHTTPService svc;

    if (argc >= 2)
    {
        if (_wcsicmp(argv[1], L"install") == 0)
        {
            wchar_t exePath[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, exePath, MAX_PATH);
            return svc.Install(L"Elle HTTP Server",
                L"Elle-Ann ESI — HTTP/1.1 and WebSocket server for Android app and REST clients.",
                exePath) == ElleResult::OK ? 0 : 1;
        }
        else if (_wcsicmp(argv[1], L"uninstall") == 0)
            return svc.Uninstall() == ElleResult::OK ? 0 : 1;
        else if (_wcsicmp(argv[1], L"console") == 0)
        {
            svc.RunAsConsole();
            return 0;
        }
    }

    // Double-click (no args) interactive install path
    if (argc == 1 && ElleServiceBase::IsInteractiveSession())
    {
        wchar_t exePath[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        ElleResult r = svc.Install(L"Elle HTTP Server",
            L"Elle-Ann ESI — HTTP/1.1 and WebSocket server for Android app and REST clients.",
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
