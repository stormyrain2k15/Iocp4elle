// =============================================================================
// WSServer.cpp — Implementation
// =============================================================================

#include "WSServer.h"
#include <wincrypt.h>
#include <sstream>
#include <iomanip>
#include <algorithm>

#pragma comment(lib, "Crypt32.lib")

static const char* WS_MAGIC = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// =============================================================================
// Constructor / Destructor
// =============================================================================
ElleWSServer::ElleWSServer()
    : m_PingThread(nullptr)
    , m_StopEvent(nullptr)
    , m_Running(false)
    , m_NextClientID(1)
    , m_MessagesReceived(0)
    , m_MessagesSent(0)
{
    m_Clients.reserve(ElleConfig::Network::WS_MAX_CLIENTS);
}

ElleWSServer::~ElleWSServer()
{
    Stop();
}

ElleResult ElleWSServer::Init()
{
    WS_LOG(INFO, L"WebSocket server initialized. MaxClients=%d", ElleConfig::Network::WS_MAX_CLIENTS);
    return ElleResult::OK;
}

ElleResult ElleWSServer::Start()
{
    m_StopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_StopEvent)
    {
        WS_LOG(FATAL, L"CreateEvent(StopEvent) failed: %lu", GetLastError());
        return ElleResult::ERR_GENERIC;
    }

    m_Running = true;
    m_PingThread = CreateThread(nullptr, 0, PingThreadProc, this, 0, nullptr);
    if (!m_PingThread)
    {
        WS_LOG(FATAL, L"CreateThread(PingThread) failed: %lu", GetLastError());
        m_Running = false;
        return ElleResult::ERR_GENERIC;
    }

    WS_LOG(INFO, L"WebSocket server running. PingInterval=%ds", ElleConfig::Network::WS_PING_INTERVAL_SEC);
    return ElleResult::OK;
}

void ElleWSServer::Stop()
{
    if (!m_Running)
        return;

    WS_LOG(INFO, L"Stopping WebSocket server. Connected=%d Recv=%llu Sent=%llu",
        ConnectedCount(), m_MessagesReceived.load(), m_MessagesSent.load());

    m_Running = false;
    if (m_StopEvent)
        SetEvent(m_StopEvent);

    // Disconnect all clients cleanly
    {
        std::lock_guard<std::mutex> lock(m_ClientMutex);
        for (auto& client : m_Clients)
        {
            SendFrame(client.Socket, WSOpcode::Close, "");
            closesocket(client.Socket);
        }
        m_Clients.clear();
    }

    if (m_PingThread)
    {
        WaitForSingleObject(m_PingThread, 5000);
        ELLE_SAFE_CLOSE_HANDLE(m_PingThread);
    }

    ELLE_SAFE_CLOSE_HANDLE(m_StopEvent);
    WS_LOG(INFO, L"WebSocket server stopped");
}

// =============================================================================
// AcceptUpgrade — completes the RFC 6455 handshake
// =============================================================================
ElleResult ElleWSServer::AcceptUpgrade(SOCKET sock, const std::wstring& secWebSocketKey)
{
    WS_LOG(INFO, L"AcceptUpgrade: sock=%llu", (uint64_t)sock);

    // Check capacity
    {
        std::lock_guard<std::mutex> lock(m_ClientMutex);
        if ((int)m_Clients.size() >= ElleConfig::Network::WS_MAX_CLIENTS)
        {
            WS_LOG(WARN, L"WS client limit reached (%d) — rejecting upgrade", ElleConfig::Network::WS_MAX_CLIENTS);
            closesocket(sock);
            return ElleResult::ERR_GENERIC;
        }
    }

    // Send handshake response
    ElleResult r = SendHandshake(sock, secWebSocketKey);
    if (r != ElleResult::OK)
    {
        WS_LOG(ERROR, L"AcceptUpgrade: handshake failed for sock=%llu", (uint64_t)sock);
        closesocket(sock);
        return r;
    }

    // Register client
    ElleWSClient client = {};
    client.Socket           = sock;
    client.ClientID         = m_NextClientID.fetch_add(1, std::memory_order_relaxed);
    client.IsAuthenticated  = false;
    GetSystemTimeAsFileTime(&client.ConnectedAt);
    GetSystemTimeAsFileTime(&client.LastPing);

    {
        std::lock_guard<std::mutex> lock(m_ClientMutex);
        m_Clients.push_back(client);
    }

    WS_LOG(INFO, L"WebSocket client connected. ClientID=%lu sock=%llu Total=%d",
        client.ClientID, (uint64_t)sock, ConnectedCount());

    // Start receive loop for this client on a new thread
    // Pass a heap-allocated copy of the client struct
    struct ClientCtx { ElleWSServer* srv; DWORD clientID; SOCKET sock; };
    auto* ctx = new ClientCtx{ this, client.ClientID, sock };

    HANDLE hThread = CreateThread(nullptr, 0,
        [](LPVOID p) -> DWORD
        {
            auto* ctx = static_cast<ClientCtx*>(p);
            ElleWSServer* srv = ctx->srv;
            DWORD clientID = ctx->clientID;
            SOCKET sock = ctx->sock;
            delete ctx;

            // Find the client in the list and run its read loop
            {
                std::lock_guard<std::mutex> lock(srv->m_ClientMutex);
                ElleWSClient* c = srv->FindClientByID(clientID);
                if (c)
                {
                    srv->ClientReadLoop(c);
                }
            }
            return 0;
        },
        ctx, 0, nullptr);

    if (hThread)
        CloseHandle(hThread);

    return ElleResult::OK;
}

// =============================================================================
// SendHandshake — builds and sends the HTTP 101 upgrade response
// =============================================================================
ElleResult ElleWSServer::SendHandshake(SOCKET sock, const std::wstring& secKey)
{
    std::string key(secKey.begin(), secKey.end());
    std::string acceptKey = ComputeAcceptKey(key);

    std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + acceptKey + "\r\n"
        "\r\n";

    int sent = send(sock, response.c_str(), (int)response.size(), 0);
    if (sent == SOCKET_ERROR)
    {
        WS_LOG(ERROR, L"SendHandshake: send() failed: %d", WSAGetLastError());
        return ElleResult::ERR_NETWORK_SEND;
    }

    WS_LOG(DEBUG, L"Handshake sent. AcceptKey=%S", acceptKey.c_str());
    return ElleResult::OK;
}

// =============================================================================
// ComputeAcceptKey — SHA1(key + magic) -> base64
// Uses Windows CryptoAPI
// =============================================================================
std::string ElleWSServer::ComputeAcceptKey(const std::string& clientKey)
{
    std::string combined = clientKey + WS_MAGIC;

    HCRYPTPROV  hProv = 0;
    HCRYPTHASH  hHash = 0;
    BYTE        sha1[20] = {};
    DWORD       hashLen = 20;

    CryptAcquireContextA(&hProv, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT);
    CryptCreateHash(hProv, CALG_SHA1, 0, 0, &hHash);
    CryptHashData(hHash, (BYTE*)combined.c_str(), (DWORD)combined.size(), 0);
    CryptGetHashParam(hHash, HP_HASHVAL, sha1, &hashLen, 0);
    CryptDestroyHash(hHash);
    CryptReleaseContext(hProv, 0);

    // Base64 encode the SHA1 digest using Windows CryptBinaryToStringA
    DWORD b64Len = 0;
    CryptBinaryToStringA(sha1, 20, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &b64Len);
    std::string b64(b64Len, '\0');
    CryptBinaryToStringA(sha1, 20, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, &b64[0], &b64Len);
    b64.resize(b64Len);

    WS_LOG(TRACE, L"ComputeAcceptKey: input=%S sha1len=%lu", clientKey.c_str(), hashLen);
    return b64;
}

// =============================================================================
// Broadcast — send to all connected clients
// =============================================================================
ElleResult ElleWSServer::Broadcast(const std::string& message)
{
    WS_LOG(DEBUG, L"Broadcast: %zu bytes to %d clients", message.size(), ConnectedCount());

    std::lock_guard<std::mutex> lock(m_ClientMutex);
    int failCount = 0;

    for (auto& client : m_Clients)
    {
        ElleResult r = SendFrame(client.Socket, WSOpcode::Text, message);
        if (r != ElleResult::OK)
            failCount++;
        else
            m_MessagesSent.fetch_add(1, std::memory_order_relaxed);
    }

    if (failCount > 0)
        WS_LOG(WARN, L"Broadcast: %d clients failed to receive", failCount);

    return ElleResult::OK;
}

// =============================================================================
// SendToClient — send to one specific client
// =============================================================================
ElleResult ElleWSServer::SendToClient(DWORD clientID, const std::string& message)
{
    WS_LOG(DEBUG, L"SendToClient: ClientID=%lu %zu bytes", clientID, message.size());

    std::lock_guard<std::mutex> lock(m_ClientMutex);
    ElleWSClient* client = FindClientByID(clientID);
    if (!client)
    {
        WS_LOG(WARN, L"SendToClient: ClientID=%lu not found", clientID);
        return ElleResult::ERR_GENERIC;
    }

    ElleResult r = SendFrame(client->Socket, WSOpcode::Text, message);
    if (r == ElleResult::OK)
        m_MessagesSent.fetch_add(1, std::memory_order_relaxed);
    return r;
}

// =============================================================================
// SendFrame — encodes and sends one RFC 6455 frame
// Server-to-client frames are NOT masked per RFC 6455 §5.1
// =============================================================================
ElleResult ElleWSServer::SendFrame(SOCKET sock, WSOpcode opcode, const std::string& payload)
{
    std::string frame;
    frame.reserve(payload.size() + 10);

    // Byte 0: FIN=1, RSV=000, opcode
    frame += (char)(0x80 | (uint8_t)opcode);

    // Byte 1+: payload length (no masking for server->client)
    uint64_t len = payload.size();
    if (len <= 125)
    {
        frame += (char)len;
    }
    else if (len <= 65535)
    {
        frame += (char)126;
        frame += (char)((len >> 8) & 0xFF);
        frame += (char)(len & 0xFF);
    }
    else
    {
        frame += (char)127;
        for (int i = 7; i >= 0; i--)
            frame += (char)((len >> (i * 8)) & 0xFF);
    }

    frame += payload;

    int sent = send(sock, frame.c_str(), (int)frame.size(), 0);
    if (sent == SOCKET_ERROR)
    {
        WS_LOG(DEBUG, L"SendFrame: send() failed sock=%llu err=%d", (uint64_t)sock, WSAGetLastError());
        return ElleResult::ERR_NETWORK_SEND;
    }

    WS_LOG(TRACE, L"SendFrame: opcode=%d payload=%zu bytes sent=%d", (int)opcode, payload.size(), sent);
    return ElleResult::OK;
}

// =============================================================================
// ReceiveFrame — reads and decodes one RFC 6455 frame from the socket
// =============================================================================
ElleResult ElleWSServer::ReceiveFrame(SOCKET sock, WSFrame& outFrame)
{
    // Read first 2 bytes (FIN/opcode, MASK/length)
    uint8_t header[2] = {};
    int r = recv(sock, (char*)header, 2, MSG_WAITALL);
    if (r <= 0)
        return ElleResult::ERR_NETWORK_RECV;

    outFrame.IsFinal    = (header[0] & 0x80) != 0;
    outFrame.Opcode     = static_cast<WSOpcode>(header[0] & 0x0F);
    outFrame.IsMasked   = (header[1] & 0x80) != 0;

    uint64_t payloadLen = header[1] & 0x7F;

    if (payloadLen == 126)
    {
        uint8_t ext[2] = {};
        if (recv(sock, (char*)ext, 2, MSG_WAITALL) <= 0)
            return ElleResult::ERR_NETWORK_RECV;
        payloadLen = ((uint64_t)ext[0] << 8) | ext[1];
    }
    else if (payloadLen == 127)
    {
        uint8_t ext[8] = {};
        if (recv(sock, (char*)ext, 8, MSG_WAITALL) <= 0)
            return ElleResult::ERR_NETWORK_RECV;
        payloadLen = 0;
        for (int i = 0; i < 8; i++)
            payloadLen = (payloadLen << 8) | ext[i];
    }

    // Read masking key (client->server MUST be masked per RFC 6455)
    if (outFrame.IsMasked)
    {
        if (recv(sock, (char*)outFrame.Mask, 4, MSG_WAITALL) <= 0)
            return ElleResult::ERR_NETWORK_RECV;
    }

    // Read payload
    if (payloadLen > 0)
    {
        outFrame.Payload.resize((size_t)payloadLen);
        uint64_t totalRead = 0;
        while (totalRead < payloadLen)
        {
            int chunkRead = recv(sock, &outFrame.Payload[(size_t)totalRead],
                                 (int)(payloadLen - totalRead), 0);
            if (chunkRead <= 0)
                return ElleResult::ERR_NETWORK_RECV;
            totalRead += chunkRead;
        }

        // Unmask if needed
        if (outFrame.IsMasked)
        {
            for (size_t i = 0; i < outFrame.Payload.size(); i++)
                outFrame.Payload[i] ^= outFrame.Mask[i % 4];
        }
    }

    WS_LOG(TRACE, L"ReceiveFrame: opcode=%d final=%d len=%llu",
        (int)outFrame.Opcode, outFrame.IsFinal, payloadLen);
    return ElleResult::OK;
}

// =============================================================================
// ClientReadLoop — per-client receive thread
// =============================================================================
void ElleWSServer::ClientReadLoop(ElleWSClient* client)
{
    SOCKET sock = client->Socket;
    DWORD  clientID = client->ClientID;

    WS_LOG(INFO, L"ClientReadLoop started. ClientID=%lu", clientID);

    while (m_Running)
    {
        WSFrame frame;
        ElleResult r = ReceiveFrame(sock, frame);

        if (r != ElleResult::OK)
        {
            WS_LOG(INFO, L"ClientReadLoop: recv failed for ClientID=%lu — disconnecting", clientID);
            break;
        }

        switch (frame.Opcode)
        {
            case WSOpcode::Text:
            case WSOpcode::Binary:
                m_MessagesReceived.fetch_add(1, std::memory_order_relaxed);
                WS_LOG(DEBUG, L"ClientID=%lu message: %zu bytes", clientID, frame.Payload.size());
                if (m_MessageCallback)
                    m_MessageCallback(clientID, frame.Payload);
                break;

            case WSOpcode::Ping:
                WS_LOG(TRACE, L"ClientID=%lu sent Ping — sending Pong", clientID);
                SendFrame(sock, WSOpcode::Pong, frame.Payload);
                GetSystemTimeAsFileTime(&client->LastPing);
                break;

            case WSOpcode::Pong:
                WS_LOG(TRACE, L"ClientID=%lu sent Pong", clientID);
                GetSystemTimeAsFileTime(&client->LastPing);
                break;

            case WSOpcode::Close:
                WS_LOG(INFO, L"ClientID=%lu sent Close frame — closing cleanly", clientID);
                SendFrame(sock, WSOpcode::Close, "");
                goto done;

            default:
                WS_LOG(WARN, L"ClientID=%lu sent unknown opcode=0x%02X", clientID, (int)frame.Opcode);
                break;
        }
    }

done:
    WS_LOG(INFO, L"ClientReadLoop exiting. ClientID=%lu", clientID);
    RemoveClient(sock);

    if (m_DisconnectCallback)
        m_DisconnectCallback(clientID);
}

// =============================================================================
// PingLoop — sends pings and removes timed-out clients
// =============================================================================
DWORD WINAPI ElleWSServer::PingThreadProc(LPVOID param)
{
    static_cast<ElleWSServer*>(param)->PingLoop();
    return 0;
}

void ElleWSServer::PingLoop()
{
    WS_LOG(INFO, L"Ping loop running. Interval=%ds Timeout=%ds",
        ElleConfig::Network::WS_PING_INTERVAL_SEC,
        ElleConfig::Network::WS_TIMEOUT_SEC);

    DWORD intervalMs = (DWORD)(ElleConfig::Network::WS_PING_INTERVAL_SEC * 1000);

    while (m_Running)
    {
        DWORD waitResult = WaitForSingleObject(m_StopEvent, intervalMs);
        if (waitResult == WAIT_OBJECT_0)
            break;

        FILETIME now;
        GetSystemTimeAsFileTime(&now);
        ULARGE_INTEGER nowULI;
        nowULI.LowPart  = now.dwLowDateTime;
        nowULI.HighPart = now.dwHighDateTime;

        std::vector<SOCKET> timedOut;

        {
            std::lock_guard<std::mutex> lock(m_ClientMutex);
            for (auto& client : m_Clients)
            {
                // Check last ping time
                ULARGE_INTEGER lastPingULI;
                lastPingULI.LowPart  = client.LastPing.dwLowDateTime;
                lastPingULI.HighPart = client.LastPing.dwHighDateTime;

                // FILETIME is in 100-nanosecond intervals
                uint64_t elapsedSec = (nowULI.QuadPart - lastPingULI.QuadPart) / 10000000ULL;

                if (elapsedSec >= (uint64_t)ElleConfig::Network::WS_TIMEOUT_SEC)
                {
                    WS_LOG(WARN, L"ClientID=%lu timed out (%llus since last ping) — dropping",
                        client.ClientID, elapsedSec);
                    timedOut.push_back(client.Socket);
                }
                else
                {
                    // Send ping
                    WS_LOG(TRACE, L"Pinging ClientID=%lu", client.ClientID);
                    SendFrame(client.Socket, WSOpcode::Ping, "elle-ping");
                }
            }
        }

        // Remove timed-out clients outside the lock
        for (SOCKET s : timedOut)
            RemoveClient(s);
    }

    WS_LOG(INFO, L"Ping loop exiting");
}

// =============================================================================
// DisconnectClient
// =============================================================================
void ElleWSServer::DisconnectClient(DWORD clientID, uint16_t closeCode)
{
    std::lock_guard<std::mutex> lock(m_ClientMutex);
    ElleWSClient* client = FindClientByID(clientID);
    if (!client)
        return;

    WS_LOG(INFO, L"Disconnecting ClientID=%lu closeCode=%d", clientID, closeCode);
    std::string closePayload;
    closePayload += (char)((closeCode >> 8) & 0xFF);
    closePayload += (char)(closeCode & 0xFF);
    SendFrame(client->Socket, WSOpcode::Close, closePayload);
    closesocket(client->Socket);
}

// =============================================================================
// Internal helpers
// =============================================================================
ElleWSClient* ElleWSServer::FindClient(SOCKET sock)
{
    for (auto& c : m_Clients)
        if (c.Socket == sock) return &c;
    return nullptr;
}

ElleWSClient* ElleWSServer::FindClientByID(DWORD id)
{
    for (auto& c : m_Clients)
        if (c.ClientID == id) return &c;
    return nullptr;
}

void ElleWSServer::RemoveClient(SOCKET sock)
{
    std::lock_guard<std::mutex> lock(m_ClientMutex);
    auto it = std::remove_if(m_Clients.begin(), m_Clients.end(),
        [sock](const ElleWSClient& c) { return c.Socket == sock; });
    if (it != m_Clients.end())
    {
        WS_LOG(INFO, L"Removing ClientID=%lu from registry. Remaining=%zu",
            it->ClientID, m_Clients.size() - 1);
        closesocket(it->Socket);
        m_Clients.erase(it, m_Clients.end());
    }
}

int ElleWSServer::ConnectedCount() const
{
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(m_ClientMutex));
    return (int)m_Clients.size();
}
