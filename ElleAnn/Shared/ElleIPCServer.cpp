// =============================================================================
// ElleIPCServer.cpp — Implementation
// =============================================================================

#include "ElleIPCServer.h"
#include <algorithm>

// =============================================================================
// ElleIPCServer
// =============================================================================
ElleIPCServer::ElleIPCServer()
    : m_IOCP(nullptr)
    , m_StopEvent(nullptr)
    , m_Running(false)
    , m_AcceptThread(nullptr)
    , m_RequestsHandled(0)
    , m_ActiveConnections(0)
{
}

ElleIPCServer::~ElleIPCServer()
{
    Stop();
}

ElleResult ElleIPCServer::Init(const wchar_t* serviceName)
{
    m_ServiceName = serviceName;
    m_PipeName = ElleIPCGetPipeName(serviceName);

    IPC_LOG(INFO, L"Initializing IPC server for service '%s' on pipe '%s'",
        serviceName, m_PipeName.c_str());

    // Create IOCP
    m_IOCP = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, WORKER_THREAD_COUNT);
    if (!m_IOCP)
    {
        IPC_LOG(FATAL, L"CreateIoCompletionPort failed: %lu", GetLastError());
        return ElleResult::ERR_GENERIC;
    }

    IPC_LOG(INFO, L"IPC server initialized. Service=%s Pipe=%s WorkerThreads=%d",
        serviceName, m_PipeName.c_str(), WORKER_THREAD_COUNT);
    return ElleResult::OK;
}

void ElleIPCServer::RegisterHandler(const std::wstring& command, ElleIPCHandler handler)
{
    std::lock_guard<std::mutex> lock(m_HandlerMutex);
    m_Handlers[command] = handler;
    IPC_LOG(DEBUG, L"Registered IPC handler for command '%s'", command.c_str());
}

ElleResult ElleIPCServer::Start()
{
    m_StopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_StopEvent)
    {
        IPC_LOG(FATAL, L"CreateEvent failed: %lu", GetLastError());
        return ElleResult::ERR_GENERIC;
    }

    m_Running = true;

    // Start IOCP worker threads
    for (int i = 0; i < WORKER_THREAD_COUNT; i++)
    {
        HANDLE hThread = CreateThread(nullptr, 0, IOCPWorkerProc, this, 0, nullptr);
        if (!hThread)
        {
            IPC_LOG(ERROR, L"Failed to create IOCP worker thread %d: %lu", i, GetLastError());
            m_Running = false;
            return ElleResult::ERR_GENERIC;
        }
        m_WorkerThreads.push_back(hThread);
        IPC_LOG(DEBUG, L"IOCP worker thread %d started. TID=%lu", i, GetThreadId(hThread));
    }

    // Start accept thread
    m_AcceptThread = CreateThread(nullptr, 0, AcceptLoopProc, this, 0, nullptr);
    if (!m_AcceptThread)
    {
        IPC_LOG(FATAL, L"Failed to create accept thread: %lu", GetLastError());
        m_Running = false;
        return ElleResult::ERR_GENERIC;
    }

    IPC_LOG(INFO, L"IPC server running. Service=%s AcceptTID=%lu WorkerThreads=%d",
        m_ServiceName.c_str(), GetThreadId(m_AcceptThread), WORKER_THREAD_COUNT);
    return ElleResult::OK;
}

void ElleIPCServer::Stop()
{
    if (!m_Running)
        return;

    IPC_LOG(INFO, L"Stopping IPC server. Service=%s Requests=%llu ActiveConns=%llu",
        m_ServiceName.c_str(), m_RequestsHandled.load(), m_ActiveConnections.load());

    m_Running = false;

    if (m_StopEvent)
        SetEvent(m_StopEvent);

    // Wake up IOCP workers with null completions
    if (m_IOCP)
    {
        for (int i = 0; i < WORKER_THREAD_COUNT; i++)
            PostQueuedCompletionStatus(m_IOCP, 0, 0, nullptr);
    }

    // Wait for accept thread
    if (m_AcceptThread)
    {
        WaitForSingleObject(m_AcceptThread, 5000);
        ELLE_SAFE_CLOSE_HANDLE(m_AcceptThread);
    }

    // Wait for IOCP workers
    if (!m_WorkerThreads.empty())
    {
        WaitForMultipleObjects((DWORD)m_WorkerThreads.size(),
                               m_WorkerThreads.data(), TRUE, 5000);
        for (auto h : m_WorkerThreads)
            CloseHandle(h);
        m_WorkerThreads.clear();
    }

    // Close all connections
    {
        std::lock_guard<std::mutex> lock(m_ConnectionsMutex);
        for (auto* conn : m_Connections)
        {
            if (conn->PipeHandle != INVALID_HANDLE_VALUE)
            {
                DisconnectNamedPipe(conn->PipeHandle);
                CloseHandle(conn->PipeHandle);
            }
            delete conn;
        }
        m_Connections.clear();
    }

    if (m_IOCP)
    {
        CloseHandle(m_IOCP);
        m_IOCP = nullptr;
    }

    ELLE_SAFE_CLOSE_HANDLE(m_StopEvent);
    IPC_LOG(INFO, L"IPC server stopped. Service=%s", m_ServiceName.c_str());
}

// =============================================================================
// Accept Loop
// =============================================================================
DWORD WINAPI ElleIPCServer::AcceptLoopProc(LPVOID param)
{
    static_cast<ElleIPCServer*>(param)->AcceptLoop();
    return 0;
}

void ElleIPCServer::AcceptLoop()
{
    IPC_LOG(INFO, L"Accept loop running. Pipe=%s", m_PipeName.c_str());

    while (m_Running)
    {
        // Create new connection
        ElleIPCConnection* conn = nullptr;
        ElleResult r = CreateConnection(conn);
        if (r != ElleResult::OK || !conn)
        {
            IPC_LOG(ERROR, L"CreateConnection failed: %s", ElleResultStr(r));
            Sleep(1000);
            continue;
        }

        // Wait for client to connect
        BOOL connected = ConnectNamedPipe(conn->PipeHandle, &conn->Overlapped);
        DWORD err = GetLastError();

        if (!connected && err != ERROR_IO_PENDING && err != ERROR_PIPE_CONNECTED)
        {
            IPC_LOG(WARN, L"ConnectNamedPipe failed: %lu", err);
            CloseConnection(conn);
            continue;
        }

        if (err == ERROR_PIPE_CONNECTED)
        {
            // Client already connected before we called ConnectNamedPipe
            IPC_LOG(DEBUG, L"Client connected immediately. ConnID=%lu", conn->ConnectionID);
            
            // Start reading
            BeginRead(conn);
        }
        else
        {
            // Connection is pending via IOCP
            IPC_LOG(DEBUG, L"Client connection pending. ConnID=%lu", conn->ConnectionID);
        }

        m_ActiveConnections.fetch_add(1, std::memory_order_relaxed);

        // Small sleep to prevent tight loop
        Sleep(10);
    }

    IPC_LOG(INFO, L"Accept loop exiting");
}

ElleResult ElleIPCServer::CreateConnection(ElleIPCConnection*& outConn)
{
    // Create named pipe instance
    HANDLE hPipe = CreateNamedPipeW(
        m_PipeName.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,  // Overlapped for IOCP
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        PIPE_UNLIMITED_INSTANCES,
        65536,      // Output buffer size
        65536,      // Input buffer size
        0,          // Default timeout
        nullptr
    );

    if (hPipe == INVALID_HANDLE_VALUE)
    {
        IPC_LOG(ERROR, L"CreateNamedPipe failed: %lu", GetLastError());
        return ElleResult::ERR_GENERIC;
    }

    // Associate pipe with IOCP
    HANDLE hIOCP = CreateIoCompletionPort(hPipe, m_IOCP, (ULONG_PTR)hPipe, 0);
    if (!hIOCP)
    {
        IPC_LOG(ERROR, L"CreateIoCompletionPort (associate) failed: %lu", GetLastError());
        CloseHandle(hPipe);
        return ElleResult::ERR_GENERIC;
    }

    // Create connection object
    ElleIPCConnection* conn = new ElleIPCConnection();
    conn->PipeHandle = hPipe;
    conn->ConnectionID = (DWORD)hPipe;  // Use pipe handle as connection ID
    conn->Reset();

    // Add to connections list
    {
        std::lock_guard<std::mutex> lock(m_ConnectionsMutex);
        m_Connections.push_back(conn);
    }

    IPC_LOG(DEBUG, L"Created connection. ConnID=%lu Pipe=%s",
        conn->ConnectionID, m_PipeName.c_str());

    outConn = conn;
    return ElleResult::OK;
}

void ElleIPCServer::CloseConnection(ElleIPCConnection* conn)
{
    if (!conn)
        return;

    IPC_LOG(DEBUG, L"Closing connection. ConnID=%lu", conn->ConnectionID);

    if (conn->PipeHandle != INVALID_HANDLE_VALUE)
    {
        DisconnectNamedPipe(conn->PipeHandle);
        CloseHandle(conn->PipeHandle);
        conn->PipeHandle = INVALID_HANDLE_VALUE;
    }

    // Remove from connections list
    {
        std::lock_guard<std::mutex> lock(m_ConnectionsMutex);
        auto it = std::find(m_Connections.begin(), m_Connections.end(), conn);
        if (it != m_Connections.end())
            m_Connections.erase(it);
    }

    delete conn;
    m_ActiveConnections.fetch_add(-1, std::memory_order_relaxed);
}

// =============================================================================
// IOCP Worker Loop
// =============================================================================
DWORD WINAPI ElleIPCServer::IOCPWorkerProc(LPVOID param)
{
    static_cast<ElleIPCServer*>(param)->IOCPWorkerLoop();
    return 0;
}

void ElleIPCServer::IOCPWorkerLoop()
{
    IPC_LOG(DEBUG, L"IOCP worker loop started. TID=%lu", GetCurrentThreadId());

    while (m_Running)
    {
        DWORD bytesTransferred = 0;
        ULONG_PTR completionKey = 0;
        OVERLAPPED* pOverlapped = nullptr;

        BOOL ok = GetQueuedCompletionStatus(
            m_IOCP,
            &bytesTransferred,
            &completionKey,
            &pOverlapped,
            INFINITE
        );

        // Shutdown signal
        if (!ok && pOverlapped == nullptr)
            break;

        if (!m_Running)
            break;

        // Find connection from overlapped structure
        ElleIPCConnection* conn = CONTAINING_RECORD(pOverlapped, ElleIPCConnection, Overlapped);

        if (!ok)
        {
            DWORD err = GetLastError();
            if (err != ERROR_BROKEN_PIPE && err != ERROR_PIPE_NOT_CONNECTED)
            {
                IPC_LOG(WARN, L"GetQueuedCompletionStatus failed for ConnID=%lu: %lu",
                    conn->ConnectionID, err);
            }
            CloseConnection(conn);
            continue;
        }

        // Handle completion based on I/O type
        if (conn->CurrentIOType == ElleIPCIOType::READ)
        {
            conn->BytesRead += bytesTransferred;

            // Check if we have complete header
            if (!conn->HeaderReceived && conn->BytesRead >= sizeof(ElleIPCMessageHeader))
            {
                memcpy(&conn->ExpectedHeader, conn->ReadBuffer.data(), sizeof(ElleIPCMessageHeader));
                
                if (!conn->ExpectedHeader.IsValid())
                {
                    IPC_LOG(WARN, L"Invalid message header from ConnID=%lu", conn->ConnectionID);
                    CloseConnection(conn);
                    continue;
                }

                conn->HeaderReceived = true;
            }

            // Check if we have complete message
            if (conn->HeaderReceived)
            {
                size_t expectedTotal = sizeof(ElleIPCMessageHeader) + conn->ExpectedHeader.PayloadLength;
                
                if (conn->BytesRead >= expectedTotal)
                {
                    // Complete message received
                    ProcessReceivedMessage(conn);
                    
                    // Reset for next message
                    conn->Reset();
                    BeginRead(conn);
                }
                else
                {
                    // Need more data
                    BeginRead(conn);
                }
            }
            else
            {
                // Still reading header
                BeginRead(conn);
            }
        }
        else if (conn->CurrentIOType == ElleIPCIOType::WRITE)
        {
            conn->BytesWritten += bytesTransferred;

            if (conn->BytesWritten < conn->WriteBuffer.size())
            {
                // Continue writing
                BeginWrite(conn, conn->WriteBuffer);
            }
            else
            {
                // Write complete, go back to reading
                conn->Reset();
                BeginRead(conn);
            }
        }
    }

    IPC_LOG(DEBUG, L"IOCP worker loop exiting. TID=%lu", GetCurrentThreadId());
}

// =============================================================================
// I/O Operations
// =============================================================================
ElleResult ElleIPCServer::BeginRead(ElleIPCConnection* conn)
{
    conn->CurrentIOType = ElleIPCIOType::READ;
    
    // Resize buffer if needed
    size_t bufferSize = sizeof(ElleIPCMessageHeader) + ELLE_IPC_MAX_PAYLOAD_SIZE;
    if (conn->ReadBuffer.size() < bufferSize)
        conn->ReadBuffer.resize(bufferSize);

    ZeroMemory(&conn->Overlapped, sizeof(OVERLAPPED));

    DWORD toRead = (DWORD)(conn->ReadBuffer.size() - conn->BytesRead);
    DWORD bytesRead = 0;

    BOOL ok = ReadFile(
        conn->PipeHandle,
        conn->ReadBuffer.data() + conn->BytesRead,
        toRead,
        &bytesRead,
        &conn->Overlapped
    );

    if (!ok && GetLastError() != ERROR_IO_PENDING)
    {
        DWORD err = GetLastError();
        if (err != ERROR_BROKEN_PIPE)
        {
            IPC_LOG(WARN, L"ReadFile failed for ConnID=%lu: %lu", conn->ConnectionID, err);
        }
        return ElleResult::ERR_NETWORK_RECV;
    }

    return ElleResult::OK;
}

ElleResult ElleIPCServer::BeginWrite(ElleIPCConnection* conn, const std::vector<BYTE>& data)
{
    conn->CurrentIOType = ElleIPCIOType::WRITE;
    conn->WriteBuffer = data;
    conn->BytesWritten = 0;

    ZeroMemory(&conn->Overlapped, sizeof(OVERLAPPED));

    DWORD bytesWritten = 0;
    BOOL ok = WriteFile(
        conn->PipeHandle,
        conn->WriteBuffer.data(),
        (DWORD)conn->WriteBuffer.size(),
        &bytesWritten,
        &conn->Overlapped
    );

    if (!ok && GetLastError() != ERROR_IO_PENDING)
    {
        IPC_LOG(WARN, L"WriteFile failed for ConnID=%lu: %lu", conn->ConnectionID, GetLastError());
        return ElleResult::ERR_NETWORK_SEND;
    }

    return ElleResult::OK;
}

// =============================================================================
// Message Processing
// =============================================================================
void ElleIPCServer::ProcessReceivedMessage(ElleIPCConnection* conn)
{
    // Deserialize message
    ElleIPCMessage reqMsg;
    ElleResult r = ElleIPCMessage::Deserialize(
        conn->ReadBuffer.data(),
        conn->BytesRead,
        reqMsg
    );

    if (r != ElleResult::OK)
    {
        IPC_LOG(WARN, L"Failed to deserialize message from ConnID=%lu", conn->ConnectionID);
        CloseConnection(conn);
        return;
    }

    IPC_LOG(DEBUG, L"Received %s from ConnID=%lu. CorrelID=%llu PayloadLen=%lu",
        reqMsg.Header.MessageType == ElleIPCMessageType::REQUEST ? L"REQUEST" : L"MESSAGE",
        conn->ConnectionID, reqMsg.Header.CorrelationID, reqMsg.Header.PayloadLength);

    // Handle based on message type
    if (reqMsg.Header.MessageType == ElleIPCMessageType::REQUEST)
    {
        DispatchRequest(conn, reqMsg);
    }
    else
    {
        IPC_LOG(WARN, L"Unexpected message type %d from ConnID=%lu",
            (int)reqMsg.Header.MessageType, conn->ConnectionID);
    }
}

void ElleIPCServer::DispatchRequest(ElleIPCConnection* conn, const ElleIPCMessage& reqMsg)
{
    // Parse request payload to extract command
    // Expected format: {"command":"CommandName","params":{...}}
    std::wstring command;
    std::wstring params;

    // Simple JSON parsing for command extraction
    size_t cmdPos = reqMsg.Payload.find(L"\"command\"");
    if (cmdPos != std::wstring::npos)
    {
        size_t colonPos = reqMsg.Payload.find(L':', cmdPos);
        size_t startQuote = reqMsg.Payload.find(L'\"', colonPos);
        size_t endQuote = reqMsg.Payload.find(L'\"', startQuote + 1);
        
        if (startQuote != std::wstring::npos && endQuote != std::wstring::npos)
        {
            command = reqMsg.Payload.substr(startQuote + 1, endQuote - startQuote - 1);
        }
    }

    size_t paramsPos = reqMsg.Payload.find(L"\"params\"");
    if (paramsPos != std::wstring::npos)
    {
        size_t colonPos = reqMsg.Payload.find(L':', paramsPos);
        size_t openBrace = reqMsg.Payload.find(L'{', colonPos);
        if (openBrace != std::wstring::npos)
        {
            int braceCount = 1;
            size_t pos = openBrace + 1;
            while (pos < reqMsg.Payload.size() && braceCount > 0)
            {
                if (reqMsg.Payload[pos] == L'{') braceCount++;
                else if (reqMsg.Payload[pos] == L'}') braceCount--;
                pos++;
            }
            params = reqMsg.Payload.substr(openBrace, pos - openBrace);
        }
    }

    if (command.empty())
    {
        IPC_LOG(WARN, L"No command found in request from ConnID=%lu", conn->ConnectionID);
        
        ElleIPCMessage errorMsg = ElleIPCMessage::CreateError(
            reqMsg.Header.CorrelationID,
            L"missing_command"
        );
        
        std::vector<BYTE> respData = errorMsg.Serialize();
        BeginWrite(conn, respData);
        return;
    }

    IPC_LOG(INFO, L"Dispatching command '%s' from ConnID=%lu. CorrelID=%llu",
        command.c_str(), conn->ConnectionID, reqMsg.Header.CorrelationID);

    // Find handler
    ElleIPCHandler handler;
    {
        std::lock_guard<std::mutex> lock(m_HandlerMutex);
        auto it = m_Handlers.find(command);
        if (it != m_Handlers.end())
            handler = it->second;
    }

    ElleIPCMessage responseMsg;
    
    if (handler)
    {
        // Invoke handler
        ElleIPCRequest request;
        request.Command = command;
        request.Parameters = params;
        
        std::wstring responseJson;
        ElleResult handlerResult = handler(request, responseJson);
        
        if (handlerResult == ElleResult::OK)
        {
            responseMsg = ElleIPCMessage::CreateResponse(
                reqMsg.Header.CorrelationID,
                responseJson
            );
            m_RequestsHandled.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
            wchar_t errBuf[256] = {};
            _snwprintf_s(errBuf, _countof(errBuf), _TRUNCATE,
                L"handler_error:%s", ElleResultStr(handlerResult));
            
            responseMsg = ElleIPCMessage::CreateError(
                reqMsg.Header.CorrelationID,
                errBuf
            );
        }
    }
    else
    {
        IPC_LOG(WARN, L"No handler registered for command '%s'", command.c_str());
        
        responseMsg = ElleIPCMessage::CreateError(
            reqMsg.Header.CorrelationID,
            L"command_not_found"
        );
    }

    // Send response
    std::vector<BYTE> respData = responseMsg.Serialize();
    BeginWrite(conn, respData);
}
