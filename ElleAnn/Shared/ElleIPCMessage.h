#pragma once
// =============================================================================
// ElleIPCMessage.h — IOCP-based Inter-Service Communication Protocol
//
// Real-time, low-latency IPC for ElleAnn services to query each other directly
// without going through SQL. Complements the async IntentQueue/ActionQueue
// pattern with synchronous request/response for context sharing.
//
// Use cases:
//   - Speech service queries Emotional service for current joy level
//   - Cognitive service requests relevant memories from Memory service
//   - Any service needs real-time state from another service
//
// Protocol:
//   Named Pipes with IOCP for async I/O
//   Request/Response pattern with correlation IDs
//   JSON payloads for flexibility
//   Timeout handling and auto-reconnect
// =============================================================================

#include "ElleTypes.h"
#include <string>
#include <functional>
#include <unordered_map>

// =============================================================================
// Message Protocol Constants
// =============================================================================
#define ELLE_IPC_MAGIC              0x454C4C45      // "ELLE" in ASCII
#define ELLE_IPC_VERSION            0x0001
#define ELLE_IPC_MAX_PAYLOAD_SIZE   (1024 * 1024)  // 1MB max message size

// Message types
enum class ElleIPCMessageType : uint16_t
{
    REQUEST     = 1,
    RESPONSE    = 2,
    ERROR       = 3,
    HEARTBEAT   = 4
};

// =============================================================================
// ElleIPCMessageHeader — 20 bytes, fixed size
// =============================================================================
#pragma pack(push, 1)
struct ElleIPCMessageHeader
{
    uint32_t            Magic;              // Must be ELLE_IPC_MAGIC
    uint16_t            Version;            // Protocol version
    ElleIPCMessageType  MessageType;        // REQUEST, RESPONSE, ERROR
    uint64_t            CorrelationID;      // Unique ID for request/response matching
    uint32_t            PayloadLength;      // Size of payload in bytes

    ElleIPCMessageHeader()
        : Magic(ELLE_IPC_MAGIC)
        , Version(ELLE_IPC_VERSION)
        , MessageType(ElleIPCMessageType::REQUEST)
        , CorrelationID(0)
        , PayloadLength(0)
    {}

    bool IsValid() const
    {
        return Magic == ELLE_IPC_MAGIC 
            && Version == ELLE_IPC_VERSION
            && PayloadLength <= ELLE_IPC_MAX_PAYLOAD_SIZE;
    }
};
#pragma pack(pop)

static_assert(sizeof(ElleIPCMessageHeader) == 20, "ElleIPCMessageHeader must be exactly 20 bytes");

// =============================================================================
// ElleIPCMessage — Complete message (header + payload)
// =============================================================================
struct ElleIPCMessage
{
    ElleIPCMessageHeader    Header;
    std::wstring            Payload;        // JSON payload (UTF-16)

    ElleIPCMessage()
    {
        Header = {};
    }

    // Create a request message
    static ElleIPCMessage CreateRequest(
        uint64_t correlationID,
        const std::wstring& jsonPayload)
    {
        ElleIPCMessage msg;
        msg.Header.MessageType      = ElleIPCMessageType::REQUEST;
        msg.Header.CorrelationID    = correlationID;
        msg.Payload                 = jsonPayload;
        msg.Header.PayloadLength    = (uint32_t)(jsonPayload.size() * sizeof(wchar_t));
        return msg;
    }

    // Create a response message
    static ElleIPCMessage CreateResponse(
        uint64_t correlationID,
        const std::wstring& jsonPayload)
    {
        ElleIPCMessage msg;
        msg.Header.MessageType      = ElleIPCMessageType::RESPONSE;
        msg.Header.CorrelationID    = correlationID;
        msg.Payload                 = jsonPayload;
        msg.Header.PayloadLength    = (uint32_t)(jsonPayload.size() * sizeof(wchar_t));
        return msg;
    }

    // Create an error response
    static ElleIPCMessage CreateError(
        uint64_t correlationID,
        const std::wstring& errorMessage)
    {
        ElleIPCMessage msg;
        msg.Header.MessageType      = ElleIPCMessageType::ERROR;
        msg.Header.CorrelationID    = correlationID;
        
        // Format error as JSON
        wchar_t errJson[512] = {};
        _snwprintf_s(errJson, _countof(errJson), _TRUNCATE,
            L"{\"error\":\"%s\"}", errorMessage.c_str());
        msg.Payload = errJson;
        msg.Header.PayloadLength = (uint32_t)(wcslen(errJson) * sizeof(wchar_t));
        return msg;
    }

    // Serialize message to raw bytes for transmission
    std::vector<BYTE> Serialize() const
    {
        std::vector<BYTE> buffer;
        buffer.reserve(sizeof(ElleIPCMessageHeader) + Header.PayloadLength);

        // Copy header
        const BYTE* headerBytes = reinterpret_cast<const BYTE*>(&Header);
        buffer.insert(buffer.end(), headerBytes, headerBytes + sizeof(ElleIPCMessageHeader));

        // Copy payload
        if (Header.PayloadLength > 0)
        {
            const BYTE* payloadBytes = reinterpret_cast<const BYTE*>(Payload.c_str());
            buffer.insert(buffer.end(), payloadBytes, payloadBytes + Header.PayloadLength);
        }

        return buffer;
    }

    // Deserialize from raw bytes
    static ElleResult Deserialize(
        const BYTE* data,
        size_t dataLen,
        ElleIPCMessage& outMessage)
    {
        if (dataLen < sizeof(ElleIPCMessageHeader))
            return ElleResult::ERR_INVALID_PARAM;

        // Parse header
        memcpy(&outMessage.Header, data, sizeof(ElleIPCMessageHeader));

        if (!outMessage.Header.IsValid())
            return ElleResult::ERR_INVALID_PARAM;

        // Check total size
        size_t expectedSize = sizeof(ElleIPCMessageHeader) + outMessage.Header.PayloadLength;
        if (dataLen < expectedSize)
            return ElleResult::ERR_INVALID_PARAM;

        // Parse payload
        if (outMessage.Header.PayloadLength > 0)
        {
            const wchar_t* payloadStart = reinterpret_cast<const wchar_t*>(
                data + sizeof(ElleIPCMessageHeader));
            size_t payloadCharLen = outMessage.Header.PayloadLength / sizeof(wchar_t);
            outMessage.Payload.assign(payloadStart, payloadCharLen);
        }

        return ElleResult::OK;
    }
};

// =============================================================================
// ElleIPCRequest — High-level request structure
// =============================================================================
struct ElleIPCRequest
{
    std::wstring    Command;        // Command name (e.g., "GetEmotionalState")
    std::wstring    Parameters;     // JSON parameters
    DWORD           TimeoutMs;      // Request timeout in milliseconds

    ElleIPCRequest()
        : TimeoutMs(5000)  // Default 5 second timeout
    {}
};

// =============================================================================
// Handler signature for IPC server
// Receives a request, returns a response payload
// =============================================================================
using ElleIPCHandler = std::function<ElleResult(
    const ElleIPCRequest&   request,
    std::wstring&           outResponseJson
)>;

// =============================================================================
// Service Name Constants
// Used for named pipe construction: \\.\pipe\ElleService.{ServiceName}
// =============================================================================
namespace ElleIPCServiceNames
{
    constexpr const wchar_t* EMOTIONAL  = L"Emotional";
    constexpr const wchar_t* COGNITIVE  = L"Cognitive";
    constexpr const wchar_t* MEMORY     = L"Memory";
    constexpr const wchar_t* ACTION     = L"Action";
    constexpr const wchar_t* IDENTITY   = L"Identity";
    constexpr const wchar_t* HEARTBEAT  = L"Heartbeat";
    constexpr const wchar_t* HTTP       = L"HTTP";
}

// Helper: Build pipe name for a service
inline std::wstring ElleIPCGetPipeName(const wchar_t* serviceName)
{
    wchar_t pipeName[256] = {};
    _snwprintf_s(pipeName, _countof(pipeName), _TRUNCATE,
        L"\\\\.\\pipe\\ElleService.%s", serviceName);
    return pipeName;
}

// Helper: Generate unique correlation ID (thread-safe)
inline uint64_t ElleIPCGenerateCorrelationID()
{
    static std::atomic<uint64_t> counter{ 1 };
    
    // High 32 bits: timestamp in milliseconds since epoch
    // Low 32 bits: atomic counter
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER uli;
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    
    uint64_t timestamp = uli.QuadPart / 10000;  // Convert to milliseconds
    uint64_t seq = counter.fetch_add(1, std::memory_order_relaxed);
    
    return (timestamp << 32) | (seq & 0xFFFFFFFF);
}
