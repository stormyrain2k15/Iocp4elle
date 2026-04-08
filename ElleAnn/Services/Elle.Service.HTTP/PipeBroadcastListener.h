// =============================================================================
// Elle.Service.HTTP — PipeBroadcastListener.h
//
// Listens on \\.\pipe\ElleWSBroadcast for incoming broadcast commands
// from Elle.Service.Action. When the Action service executes a WS_BROADCAST
// action (VIBRATE, FLASH, NOTIFY, etc.), it writes the message to this pipe.
// This listener reads it and calls WSServer::Broadcast() to push it to all
// connected Android clients.
//
// Protocol:
//   Writer sends: [DWORD msgByteLen][msgByteLen bytes of UTF-16 LE message]
//   Listener reads, decodes, calls Broadcast(message)
//
// The pipe server runs on its own thread. One outstanding ConnectNamedPipe
// at a time — this is sufficient for Elle's single-writer architecture.
// =============================================================================

#pragma once
#include "../../Shared/ElleTypes.h"
#include "../../Shared/ElleLogger.h"
#include "WSServer.h"

#define PIPE_LOG(lvl, fmt, ...) ELLE_LOG_##lvl(L"PipeBroadcast", fmt, ##__VA_ARGS__)

class EllePipeBroadcastListener
{
    ELLE_NONCOPYABLE(EllePipeBroadcastListener)
public:
    explicit EllePipeBroadcastListener(ElleWSServer& wsServer)
        : m_WS(wsServer)
        , m_Running(false)
        , m_Thread(nullptr)
        , m_StopEvent(nullptr)
    {}

    ~EllePipeBroadcastListener() { Stop(); }

    ElleResult Start(HANDLE stopEvent)
    {
        m_StopEvent = stopEvent;
        m_Running   = true;

        m_Thread = CreateThread(nullptr, 0, PipeProc, this, 0, nullptr);
        if (!m_Thread)
        {
            PIPE_LOG(ERROR, L"CreateThread failed: %lu", GetLastError());
            m_Running = false;
            return ElleResult::ERR_GENERIC;
        }

        PIPE_LOG(INFO, L"Pipe broadcast listener started on \\\\.\\pipe\\ElleWSBroadcast");
        return ElleResult::OK;
    }

    void Stop()
    {
        if (!m_Running) return;
        m_Running = false;

        // Open and immediately close a client connection to unblock ConnectNamedPipe
        HANDLE hWake = CreateFileW(L"\\\\.\\pipe\\ElleWSBroadcast",
            GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (hWake != INVALID_HANDLE_VALUE) CloseHandle(hWake);

        if (m_Thread)
        {
            WaitForSingleObject(m_Thread, 5000);
            ELLE_SAFE_CLOSE_HANDLE(m_Thread);
        }
        PIPE_LOG(INFO, L"Pipe broadcast listener stopped");
    }

private:
    static DWORD WINAPI PipeProc(LPVOID param)
    {
        auto* self = static_cast<EllePipeBroadcastListener*>(param);
        self->PipeLoop();
        return 0;
    }

    void PipeLoop()
    {
        while (m_Running)
        {
            // Create a new named pipe instance for each connection
            HANDLE hPipe = CreateNamedPipeW(
                L"\\\\.\\pipe\\ElleWSBroadcast",
                PIPE_ACCESS_INBOUND,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                PIPE_UNLIMITED_INSTANCES,
                0,
                65536,
                0,
                nullptr
            );

            if (hPipe == INVALID_HANDLE_VALUE)
            {
                PIPE_LOG(ERROR, L"CreateNamedPipe failed: %lu", GetLastError());
                Sleep(1000);
                continue;
            }

            PIPE_LOG(TRACE, L"Waiting for pipe client connection");
            BOOL connected = ConnectNamedPipe(hPipe, nullptr);
            if (!connected && GetLastError() != ERROR_PIPE_CONNECTED)
            {
                CloseHandle(hPipe);
                continue;
            }

            if (!m_Running)
            {
                DisconnectNamedPipe(hPipe);
                CloseHandle(hPipe);
                break;
            }

            // Read length prefix (DWORD = byte count)
            DWORD msgByteLen = 0;
            DWORD bytesRead  = 0;
            BOOL ok = ReadFile(hPipe, &msgByteLen, sizeof(msgByteLen), &bytesRead, nullptr);

            if (!ok || bytesRead != sizeof(msgByteLen) || msgByteLen == 0 || msgByteLen > 1024 * 1024)
            {
                PIPE_LOG(WARN, L"PipeLoop: Invalid length prefix (bytesRead=%lu len=%lu)", bytesRead, msgByteLen);
                DisconnectNamedPipe(hPipe);
                CloseHandle(hPipe);
                continue;
            }

            // Read the message body
            std::vector<BYTE> rawMsg(msgByteLen + 2, 0);  // +2 for null terminator
            DWORD totalRead = 0;

            while (totalRead < msgByteLen)
            {
                DWORD chunk = 0;
                ok = ReadFile(hPipe, rawMsg.data() + totalRead, msgByteLen - totalRead, &chunk, nullptr);
                if (!ok || chunk == 0) break;
                totalRead += chunk;
            }

            DisconnectNamedPipe(hPipe);
            CloseHandle(hPipe);

            if (totalRead != msgByteLen)
            {
                PIPE_LOG(WARN, L"PipeLoop: Short read (%lu/%lu bytes)", totalRead, msgByteLen);
                continue;
            }

            // Message is UTF-16 LE — convert to narrow UTF-8 string for WSServer::Broadcast
            const wchar_t* wMsg = reinterpret_cast<const wchar_t*>(rawMsg.data());
            int narrowLen = WideCharToMultiByte(CP_UTF8, 0, wMsg, -1, nullptr, 0, nullptr, nullptr);
            if (narrowLen <= 0) continue;

            std::string narrowMsg(narrowLen, '\0');
            WideCharToMultiByte(CP_UTF8, 0, wMsg, -1, &narrowMsg[0], narrowLen, nullptr, nullptr);
            narrowMsg.resize(narrowLen - 1);  // Remove null terminator

            PIPE_LOG(DEBUG, L"PipeLoop: Broadcasting %zu bytes to WS clients", narrowMsg.size());
            m_WS.Broadcast(narrowMsg);
        }

        PIPE_LOG(INFO, L"PipeLoop exiting");
    }

    ElleWSServer&   m_WS;
    HANDLE          m_Thread;
    HANDLE          m_StopEvent;
    volatile bool   m_Running;
};
