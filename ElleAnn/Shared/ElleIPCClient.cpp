// =============================================================================
// ElleIPCClient.cpp — Implementation
// =============================================================================

#include "ElleIPCClient.h"

// =============================================================================
// ElleIPCClient
// =============================================================================
ElleIPCClient::ElleIPCClient()
{
}

ElleIPCClient::~ElleIPCClient()
{
    Shutdown();
}

ElleResult ElleIPCClient::Init()
{
    IPCCLIENT_LOG(INFO, L"IPC client initialized");
    return ElleResult::OK;
}

void ElleIPCClient::Shutdown()
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    // Close all cached connections
    for (auto& pair : m_ConnectionCache)
    {
        if (pair.second != INVALID_HANDLE_VALUE)
        {
            CloseHandle(pair.second);
        }
    }
    m_ConnectionCache.clear();

    IPCCLIENT_LOG(INFO, L"IPC client shutdown");
}

ElleResult ElleIPCClient::Request(
    const wchar_t*      serviceName,
    const std::wstring& command,
    const std::wstring& params,
    std::wstring&       outResponse,
    DWORD               timeoutMs)
{
    IPCCLIENT_LOG(DEBUG, L"IPC Request: Service=%s Command=%s Timeout=%lums",
        serviceName, command.c_str(), timeoutMs);

    // Generate correlation ID
    uint64_t correlationID = ElleIPCGenerateCorrelationID();

    // Connect to service
    HANDLE hPipe = INVALID_HANDLE_VALUE;
    ElleResult r = ConnectToService(serviceName, hPipe);
    if (r != ElleResult::OK)
    {
        IPCCLIENT_LOG(ERROR, L"Failed to connect to service '%s': %s",
            serviceName, ElleResultStr(r));
        return r;
    }

    // Send request
    r = SendRequest(hPipe, command, params, correlationID);
    if (r != ElleResult::OK)
    {
        IPCCLIENT_LOG(ERROR, L"Failed to send request to '%s': %s",
            serviceName, ElleResultStr(r));
        DisconnectFromService(hPipe);
        return r;
    }

    // Receive response
    r = ReceiveResponse(hPipe, correlationID, outResponse, timeoutMs);
    if (r != ElleResult::OK)
    {
        IPCCLIENT_LOG(ERROR, L"Failed to receive response from '%s': %s",
            serviceName, ElleResultStr(r));
        DisconnectFromService(hPipe);
        return r;
    }

    IPCCLIENT_LOG(DEBUG, L"IPC Request completed: Service=%s Command=%s ResponseLen=%zu",
        serviceName, command.c_str(), outResponse.size());

    return ElleResult::OK;
}

// =============================================================================
// Connection Management
// =============================================================================
ElleResult ElleIPCClient::ConnectToService(const wchar_t* serviceName, HANDLE& outPipe)
{
    std::wstring pipeName = ElleIPCGetPipeName(serviceName);

    IPCCLIENT_LOG(DEBUG, L"Connecting to service '%s' via pipe '%s'",
        serviceName, pipeName.c_str());

    // Try to open the pipe
    HANDLE hPipe = CreateFileW(
        pipeName.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        0,  // Synchronous I/O for client simplicity
        nullptr
    );

    if (hPipe == INVALID_HANDLE_VALUE)
    {
        DWORD err = GetLastError();
        
        // If pipe doesn't exist yet, wait for it
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PIPE_BUSY)
        {
            IPCCLIENT_LOG(DEBUG, L"Pipe '%s' not ready (err=%lu), waiting...",
                pipeName.c_str(), err);
            
            // Wait up to 2 seconds for pipe to become available
            if (!WaitNamedPipeW(pipeName.c_str(), 2000))
            {
                IPCCLIENT_LOG(WARN, L"WaitNamedPipe failed for '%s': %lu",
                    pipeName.c_str(), GetLastError());
                return ElleResult::ERR_NETWORK_CONNECT;
            }

            // Try again
            hPipe = CreateFileW(
                pipeName.c_str(),
                GENERIC_READ | GENERIC_WRITE,
                0,
                nullptr,
                OPEN_EXISTING,
                0,
                nullptr
            );

            if (hPipe == INVALID_HANDLE_VALUE)
            {
                IPCCLIENT_LOG(ERROR, L"CreateFile failed for pipe '%s' after wait: %lu",
                    pipeName.c_str(), GetLastError());
                return ElleResult::ERR_NETWORK_CONNECT;
            }
        }
        else
        {
            IPCCLIENT_LOG(ERROR, L"CreateFile failed for pipe '%s': %lu",
                pipeName.c_str(), err);
            return ElleResult::ERR_NETWORK_CONNECT;
        }
    }

    // Set pipe to message mode
    DWORD mode = PIPE_READMODE_BYTE;
    if (!SetNamedPipeHandleState(hPipe, &mode, nullptr, nullptr))
    {
        IPCCLIENT_LOG(WARN, L"SetNamedPipeHandleState failed: %lu", GetLastError());
        // Non-fatal, continue
    }

    IPCCLIENT_LOG(DEBUG, L"Connected to service '%s'", serviceName);
    outPipe = hPipe;
    return ElleResult::OK;
}

void ElleIPCClient::DisconnectFromService(HANDLE hPipe)
{
    if (hPipe != INVALID_HANDLE_VALUE)
    {
        CloseHandle(hPipe);
    }
}

// =============================================================================
// Send/Receive Operations
// =============================================================================
ElleResult ElleIPCClient::SendRequest(
    HANDLE              hPipe,
    const std::wstring& command,
    const std::wstring& params,
    uint64_t            correlationID)
{
    // Build request JSON
    wchar_t requestJson[4096] = {};
    _snwprintf_s(requestJson, _countof(requestJson), _TRUNCATE,
        L"{\"command\":\"%s\",\"params\":%s}",
        command.c_str(),
        params.empty() ? L"{}" : params.c_str()
    );

    // Create request message
    ElleIPCMessage reqMsg = ElleIPCMessage::CreateRequest(correlationID, requestJson);
    std::vector<BYTE> data = reqMsg.Serialize();

    IPCCLIENT_LOG(TRACE, L"Sending request: CorrelID=%llu PayloadLen=%lu",
        correlationID, reqMsg.Header.PayloadLength);

    // Write to pipe
    DWORD bytesWritten = 0;
    BOOL ok = WriteFile(
        hPipe,
        data.data(),
        (DWORD)data.size(),
        &bytesWritten,
        nullptr
    );

    if (!ok || bytesWritten != data.size())
    {
        IPCCLIENT_LOG(ERROR, L"WriteFile failed: %lu (wrote %lu/%zu bytes)",
            GetLastError(), bytesWritten, data.size());
        return ElleResult::ERR_NETWORK_SEND;
    }

    // Flush to ensure it's sent immediately
    FlushFileBuffers(hPipe);

    return ElleResult::OK;
}

ElleResult ElleIPCClient::ReceiveResponse(
    HANDLE              hPipe,
    uint64_t            expectedCorrelationID,
    std::wstring&       outResponse,
    DWORD               timeoutMs)
{
    IPCCLIENT_LOG(TRACE, L"Waiting for response: CorrelID=%llu Timeout=%lums",
        expectedCorrelationID, timeoutMs);

    // Set read timeout
    COMMTIMEOUTS timeouts = {};
    timeouts.ReadIntervalTimeout = 0;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.ReadTotalTimeoutConstant = timeoutMs;
    SetCommTimeouts(hPipe, &timeouts);

    // Read header
    ElleIPCMessageHeader header;
    DWORD bytesRead = 0;
    
    BOOL ok = ReadFile(
        hPipe,
        &header,
        sizeof(header),
        &bytesRead,
        nullptr
    );

    if (!ok || bytesRead != sizeof(header))
    {
        DWORD err = GetLastError();
        if (err == ERROR_TIMEOUT)
        {
            IPCCLIENT_LOG(WARN, L"Timeout waiting for response header");
            return ElleResult::ERR_TIMEOUT;
        }
        
        IPCCLIENT_LOG(ERROR, L"ReadFile (header) failed: %lu (read %lu/%zu bytes)",
            err, bytesRead, sizeof(header));
        return ElleResult::ERR_NETWORK_RECV;
    }

    // Validate header
    if (!header.IsValid())
    {
        IPCCLIENT_LOG(ERROR, L"Invalid response header: Magic=0x%08X Version=0x%04X PayloadLen=%lu",
            header.Magic, header.Version, header.PayloadLength);
        return ElleResult::ERR_INVALID_PARAM;
    }

    // Check correlation ID
    if (header.CorrelationID != expectedCorrelationID)
    {
        IPCCLIENT_LOG(WARN, L"Correlation ID mismatch: expected %llu, got %llu",
            expectedCorrelationID, header.CorrelationID);
        return ElleResult::ERR_INVALID_PARAM;
    }

    // Read payload
    if (header.PayloadLength > 0)
    {
        std::vector<BYTE> payloadBuffer(header.PayloadLength);
        DWORD totalRead = 0;

        while (totalRead < header.PayloadLength)
        {
            DWORD chunk = 0;
            ok = ReadFile(
                hPipe,
                payloadBuffer.data() + totalRead,
                header.PayloadLength - totalRead,
                &chunk,
                nullptr
            );

            if (!ok || chunk == 0)
            {
                IPCCLIENT_LOG(ERROR, L"ReadFile (payload) failed: %lu (read %lu/%lu bytes)",
                    GetLastError(), totalRead, header.PayloadLength);
                return ElleResult::ERR_NETWORK_RECV;
            }

            totalRead += chunk;
        }

        // Convert payload to wstring
        const wchar_t* wPayload = reinterpret_cast<const wchar_t*>(payloadBuffer.data());
        size_t wcharCount = header.PayloadLength / sizeof(wchar_t);
        outResponse.assign(wPayload, wcharCount);
    }

    // Check message type
    if (header.MessageType == ElleIPCMessageType::ERROR)
    {
        IPCCLIENT_LOG(WARN, L"Received error response: %s", outResponse.c_str());
        return ElleResult::ERR_IPC_REMOTE_ERROR;
    }

    IPCCLIENT_LOG(TRACE, L"Received response: CorrelID=%llu Type=%d PayloadLen=%lu",
        header.CorrelationID, (int)header.MessageType, header.PayloadLength);

    return ElleResult::OK;
}

// =============================================================================
// Pending Requests Management
// =============================================================================
void ElleIPCClient::RegisterPendingRequest(uint64_t correlationID, ElleIPCPendingRequest* request)
{
    std::lock_guard<std::mutex> lock(m_PendingMutex);
    m_PendingRequests[correlationID] = request;
}

void ElleIPCClient::UnregisterPendingRequest(uint64_t correlationID)
{
    std::lock_guard<std::mutex> lock(m_PendingMutex);
    m_PendingRequests.erase(correlationID);
}

ElleIPCPendingRequest* ElleIPCClient::FindPendingRequest(uint64_t correlationID)
{
    std::lock_guard<std::mutex> lock(m_PendingMutex);
    auto it = m_PendingRequests.find(correlationID);
    return (it != m_PendingRequests.end()) ? it->second : nullptr;
}
