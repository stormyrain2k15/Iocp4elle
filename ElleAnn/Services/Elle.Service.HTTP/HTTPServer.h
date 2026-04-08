#pragma once
// =============================================================================
// HTTPServer.h — Raw Winsock HTTP/1.1 Server for Elle-Ann
//
// Handles incoming HTTP requests from the Android app and REST clients.
// No framework. Winsock2 directly. Single listening socket, I/O completion
// ports for concurrent client handling.
//
// Responsibilities:
//   - Accept connections on HTTP_PORT (8000)
//   - Parse HTTP/1.1 request line, headers, and body
//   - Detect WebSocket upgrade requests and hand off to WSServer
//   - Dispatch REST requests to RouteDispatch
//   - Write HTTP responses with correct status, headers, and body
//   - Keep-alive connection management
// =============================================================================

#include "../../Shared/ElleTypes.h"
#include "../../Shared/ElleConfig.h"
#include "../../Shared/ElleLogger.h"
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>

#pragma comment(lib, "Ws2_32.lib")

#define HTTP_LOG(lvl, fmt, ...) ELLE_LOG_##lvl(L"HTTPServer", fmt, ##__VA_ARGS__)

// =============================================================================
// HTTP primitives
// =============================================================================
struct HttpRequest
{
    std::wstring                                    Method;     // GET, POST, etc.
    std::wstring                                    Path;       // /api/ai, /api/memory, etc.
    std::wstring                                    Query;      // Everything after ?
    std::wstring                                    Version;    // HTTP/1.1
    std::unordered_map<std::wstring, std::wstring>  Headers;
    std::string                                     Body;       // Raw bytes (UTF-8 from client)
    SOCKET                                          ClientSocket;
    std::wstring                                    RemoteAddr;
    bool                                            IsWebSocketUpgrade;
};

struct HttpResponse
{
    int                                             StatusCode; // 200, 404, 500, etc.
    std::wstring                                    StatusText; // OK, Not Found, etc.
    std::unordered_map<std::wstring, std::wstring>  Headers;
    std::string                                     Body;       // UTF-8 encoded

    // Convenience constructors
    static HttpResponse OK(const std::string& jsonBody);
    static HttpResponse BadRequest(const std::string& message);
    static HttpResponse NotFound();
    static HttpResponse InternalError(const std::string& message);
    static HttpResponse Unauthorized();
    static HttpResponse MethodNotAllowed();
};

// =============================================================================
// Route handler signature
// =============================================================================
using HttpRouteHandler = std::function<HttpResponse(const HttpRequest&)>;

// =============================================================================
// ElleHTTPServer
// =============================================================================
class ElleHTTPServer
{
    ELLE_NONCOPYABLE(ElleHTTPServer)
public:
    ElleHTTPServer();
    ~ElleHTTPServer();

    // Initialize Winsock, create listening socket, bind to HTTP_PORT
    ElleResult Init();

    // Start the accept loop and I/O completion port worker threads
    ElleResult Start();

    // Signal shutdown, close all sockets, wait for threads
    void Stop();

    // Register a route handler.
    // method: L"GET", L"POST", L"DELETE", etc. (or L"*" for any method)
    // path:   exact path match, e.g. L"/api/ai" or L"/api/memory"
    void RegisterRoute(const std::wstring& method, const std::wstring& path, HttpRouteHandler handler);

    bool IsRunning() const { return m_Running; }

    // Stats
    uint64_t RequestsHandled() const { return m_RequestsHandled; }
    uint64_t ActiveConnections() const;

private:
    // Accept loop thread — accepts new connections and posts them to IOCP
    static DWORD WINAPI AcceptThreadProc(LPVOID param);

    // IOCP worker threads — handle completed I/O operations
    static DWORD WINAPI IOCPWorkerProc(LPVOID param);

    void AcceptLoop();
    void HandleCompletion(SOCKET clientSock, DWORD bytesTransferred, OVERLAPPED* pOverlapped);

    // Read a complete HTTP request from a client socket
    ElleResult ReadRequest(SOCKET sock, HttpRequest& outReq);

    // Write an HTTP response to a client socket
    ElleResult WriteResponse(SOCKET sock, const HttpResponse& resp);

    // Parse raw HTTP bytes into an HttpRequest
    ElleResult ParseRequest(const std::string& raw, HttpRequest& outReq, SOCKET sock);

    // Find and call the registered handler for a request
    HttpResponse Dispatch(const HttpRequest& req);

    // Serialize HttpResponse to bytes ready to send
    std::string SerializeResponse(const HttpResponse& resp);

    struct RouteKey
    {
        std::wstring    Method;
        std::wstring    Path;
        bool operator==(const RouteKey& o) const { return Method == o.Method && Path == o.Path; }
    };

    struct RouteKeyHash
    {
        size_t operator()(const RouteKey& k) const
        {
            return std::hash<std::wstring>()(k.Method) ^ (std::hash<std::wstring>()(k.Path) << 1);
        }
    };

    std::unordered_map<RouteKey, HttpRouteHandler, RouteKeyHash>    m_Routes;
    std::mutex                                                        m_RouteMutex;

    SOCKET              m_ListenSocket;
    HANDLE              m_IOCP;
    HANDLE              m_AcceptThread;
    std::vector<HANDLE> m_WorkerThreads;
    HANDLE              m_StopEvent;
    volatile bool       m_Running;
    std::atomic<uint64_t> m_RequestsHandled;
    std::atomic<int>    m_ActiveConnections;

    static constexpr int WORKER_THREAD_COUNT = 4;
    static constexpr int RECV_BUFFER_BYTES   = ElleConfig::Network::RECV_BUFFER_SIZE;
};
