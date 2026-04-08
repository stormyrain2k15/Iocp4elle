#pragma once
// =============================================================================
// ElleIPCServer.h — IOCP-based IPC Server for Inter-Service Communication
//
// Each ElleAnn service hosts an IPC server that:
//   - Listens on a named pipe (\\.\pipe\ElleService.{ServiceName})
//   - Uses IOCP for high-performance async I/O
//   - Handles multiple concurrent client connections
//   - Dispatches requests to registered handlers
//   - Sends responses back to clients
//
// Integration:
//   In your service's OnStart():
//     m_IPCServer.Init(ElleIPCServiceNames::EMOTIONAL);
//     m_IPCServer.RegisterHandler("GetState", [](req, resp) { ... });
//     m_IPCServer.Start();
// =============================================================================

#include "ElleIPCMessage.h"
#include "ElleLogger.h"
#include <mutex>
#include <unordered_map>
#include <vector>

#define IPC_LOG(lvl, fmt, ...) ELLE_LOG_##lvl(L"IPCServer", fmt, ##__VA_ARGS__)

// Forward declarations
struct ElleIPCConnection;

// =============================================================================
// ElleIPCServer — IOCP-based named pipe server
// =============================================================================
class ElleIPCServer
{
    ELLE_NONCOPYABLE(ElleIPCServer)

public:
    ElleIPCServer();
    ~ElleIPCServer();

    // Initialize the server with a service name
    // Creates pipe: \\.\pipe\ElleService.{serviceName}
    ElleResult Init(const wchar_t* serviceName);

    // Register a command handler
    // Command handlers are invoked when a client sends a request with matching command name
    void RegisterHandler(const std::wstring& command, ElleIPCHandler handler);

    // Start the IOCP server (creates worker threads and accept loop)
    ElleResult Start();

    // Stop the server and clean up all connections
    void Stop();

    bool IsRunning() const { return m_Running; }

    uint64_t RequestsHandled() const { return m_RequestsHandled.load(); }
    uint64_t ActiveConnections() const { return m_ActiveConnections.load(); }

private:
    // IOCP worker thread
    static DWORD WINAPI IOCPWorkerProc(LPVOID param);
    void IOCPWorkerLoop();

    // Accept loop thread
    static DWORD WINAPI AcceptLoopProc(LPVOID param);
    void AcceptLoop();

    // Connection management
    ElleResult CreateConnection(ElleIPCConnection*& outConn);
    void CloseConnection(ElleIPCConnection* conn);
    void ProcessReceivedMessage(ElleIPCConnection* conn);

    // I/O operations
    ElleResult BeginRead(ElleIPCConnection* conn);
    ElleResult BeginWrite(ElleIPCConnection* conn, const std::vector<BYTE>& data);

    // Request dispatching
    void DispatchRequest(ElleIPCConnection* conn, const ElleIPCMessage& reqMsg);

    std::wstring                m_ServiceName;
    std::wstring                m_PipeName;
    
    HANDLE                      m_IOCP;
    HANDLE                      m_StopEvent;
    volatile bool               m_Running;

    // Worker threads for IOCP
    std::vector<HANDLE>         m_WorkerThreads;
    static constexpr int        WORKER_THREAD_COUNT = 4;

    // Accept thread
    HANDLE                      m_AcceptThread;

    // Handler registry
    std::unordered_map<std::wstring, ElleIPCHandler>    m_Handlers;
    std::mutex                                          m_HandlerMutex;

    // Active connections
    std::vector<ElleIPCConnection*>     m_Connections;
    std::mutex                          m_ConnectionsMutex;

    // Statistics
    std::atomic<uint64_t>       m_RequestsHandled;
    std::atomic<uint64_t>       m_ActiveConnections;
};

// =============================================================================
// ElleIPCConnection — Per-connection state for IOCP operations
// =============================================================================
enum class ElleIPCIOType
{
    READ,
    WRITE
};

struct ElleIPCConnection
{
    HANDLE              PipeHandle;
    OVERLAPPED          Overlapped;         // For IOCP operations
    ElleIPCIOType       CurrentIOType;
    
    // Read state
    std::vector<BYTE>   ReadBuffer;
    DWORD               BytesRead;
    bool                HeaderReceived;
    ElleIPCMessageHeader ExpectedHeader;

    // Write state
    std::vector<BYTE>   WriteBuffer;
    DWORD               BytesWritten;

    // Connection ID for logging
    DWORD               ConnectionID;

    ElleIPCConnection()
        : PipeHandle(INVALID_HANDLE_VALUE)
        , Overlapped{}
        , CurrentIOType(ElleIPCIOType::READ)
        , BytesRead(0)
        , HeaderReceived(false)
        , ExpectedHeader{}
        , BytesWritten(0)
        , ConnectionID(0)
    {
        ReadBuffer.reserve(ELLE_IPC_MAX_PAYLOAD_SIZE + sizeof(ElleIPCMessageHeader));
    }

    void Reset()
    {
        ZeroMemory(&Overlapped, sizeof(Overlapped));
        ReadBuffer.clear();
        WriteBuffer.clear();
        BytesRead = 0;
        HeaderReceived = false;
        ExpectedHeader = {};
        BytesWritten = 0;
        CurrentIOType = ElleIPCIOType::READ;
    }
};
