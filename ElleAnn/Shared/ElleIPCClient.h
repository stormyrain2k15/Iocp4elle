#pragma once
// =============================================================================
// ElleIPCClient.h — IOCP-based IPC Client for Inter-Service Communication
//
// Allows ElleAnn services to make synchronous requests to other services:
//
// Usage:
//   ElleIPCClient client;
//   client.Init();
//   
//   std::wstring response;
//   ElleResult r = client.Request(
//       ElleIPCServiceNames::EMOTIONAL,
//       "GetEmotionalState",
//       L"{\"dimensions\":[0,1,2]}",
//       response,
//       5000  // 5 second timeout
//   );
//
// Features:
//   - Automatic connection to target service's named pipe
//   - Request/Response with correlation ID matching
//   - Timeout handling
//   - Auto-reconnect on connection failures
//   - Thread-safe (can be used from multiple threads)
// =============================================================================

#include "ElleIPCMessage.h"
#include "ElleLogger.h"
#include <mutex>
#include <unordered_map>
#include <condition_variable>

#define IPCCLIENT_LOG(lvl, fmt, ...) ELLE_LOG_##lvl(L"IPCClient", fmt, ##__VA_ARGS__)

// =============================================================================
// ElleIPCPendingRequest — Tracks an in-flight request waiting for response
// =============================================================================
struct ElleIPCPendingRequest
{
    uint64_t                    CorrelationID;
    std::wstring                Response;
    ElleResult                  Result;
    bool                        Completed;
    std::mutex                  Mutex;
    std::condition_variable     CondVar;

    ElleIPCPendingRequest()
        : CorrelationID(0)
        , Result(ElleResult::ERR_GENERIC)
        , Completed(false)
    {}
};

// =============================================================================
// ElleIPCClient — Client for making requests to IPC servers
// =============================================================================
class ElleIPCClient
{
    ELLE_NONCOPYABLE(ElleIPCClient)

public:
    ElleIPCClient();
    ~ElleIPCClient();

    // Initialize the client
    ElleResult Init();

    // Shutdown and cleanup
    void Shutdown();

    // Make a synchronous request to a service
    // serviceName: Target service (e.g., ElleIPCServiceNames::EMOTIONAL)
    // command: Command to execute (e.g., "GetEmotionalState")
    // params: JSON parameters for the command
    // outResponse: Response JSON from the service
    // timeoutMs: Timeout in milliseconds (default 5000ms)
    ElleResult Request(
        const wchar_t*      serviceName,
        const std::wstring& command,
        const std::wstring& params,
        std::wstring&       outResponse,
        DWORD               timeoutMs = 5000
    );

private:
    // Connection management
    ElleResult ConnectToService(const wchar_t* serviceName, HANDLE& outPipe);
    void DisconnectFromService(HANDLE hPipe);

    // Send/receive operations
    ElleResult SendRequest(
        HANDLE              hPipe,
        const std::wstring& command,
        const std::wstring& params,
        uint64_t            correlationID
    );

    ElleResult ReceiveResponse(
        HANDLE              hPipe,
        uint64_t            expectedCorrelationID,
        std::wstring&       outResponse,
        DWORD               timeoutMs
    );

    // Pending requests tracking
    void RegisterPendingRequest(uint64_t correlationID, ElleIPCPendingRequest* request);
    void UnregisterPendingRequest(uint64_t correlationID);
    ElleIPCPendingRequest* FindPendingRequest(uint64_t correlationID);

    std::mutex                                          m_Mutex;
    std::unordered_map<std::wstring, HANDLE>            m_ConnectionCache;  // Service name -> pipe handle
    std::unordered_map<uint64_t, ElleIPCPendingRequest*> m_PendingRequests;
    std::mutex                                          m_PendingMutex;
};
