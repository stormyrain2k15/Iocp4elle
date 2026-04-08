#pragma once
// =============================================================================
// ElleTypes.h — Common Types for the Elle-Ann ESI Platform
// Structs, enums, and constants shared across all services and DLLs.
// Nothing in this file has dependencies outside of the Windows SDK and STL.
// =============================================================================

#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#include <WinSock2.h>
#include <Windows.h>

// Windows headers pollute the global namespace with common words that collide
// with our enum identifiers (e.g. ERROR, TRACE). Undefine the most common
// culprits immediately after including Windows.h so the enum below compiles
// as intended.
#ifdef ERROR
#undef ERROR
#endif
#ifdef TRACE
#undef TRACE
#endif
#ifdef WARN
#undef WARN
#endif
#ifdef INFO
#undef INFO
#endif
#ifdef DEBUG
#undef DEBUG
#endif
#ifdef FATAL
#undef FATAL
#endif
#include <string>
#include <vector>
#include <cstdint>

// =============================================================================
// RESULT CODES — every function that can fail returns one of these
// =============================================================================
enum class ElleResult : DWORD
{
    OK                      = 0,
    ERR_GENERIC             = 1,
    ERR_INVALID_PARAM       = 2,
    ERR_OUT_OF_MEMORY       = 3,
    ERR_SQL_CONNECT         = 10,
    ERR_SQL_QUERY           = 11,
    ERR_SQL_TIMEOUT         = 12,
    ERR_SQL_NO_DATA         = 13,
    // Write reached the server and @@ROWCOUNT > 0, but the subsequent
    // read-back found the persisted values do not match what was written.
    // Distinct from ERR_SQL_QUERY (the write itself failed) so callers can
    // log and handle the two failure modes differently.
    ERR_SQL_WRITE_UNVERIFIED = 14,
    ERR_QUEUE_EMPTY         = 20,
    ERR_QUEUE_FULL          = 21,
    ERR_QUEUE_STALE         = 22,
    ERR_TRUST_INSUFFICIENT  = 30,
    ERR_TRUST_GATED         = 31,
    ERR_ACTION_NOT_FOUND    = 40,
    ERR_ACTION_LOCKED       = 41,
    ERR_ACTION_FAILED       = 42,
    ERR_INTENT_INVALID      = 50,
    ERR_INTENT_UNSUPPORTED  = 51,
    ERR_SERVICE_NOT_READY   = 60,
    ERR_SERVICE_SHUTDOWN    = 61,
    ERR_LUA_LOAD            = 70,
    ERR_LUA_EXEC            = 71,
    ERR_ASM_CALL            = 80,
    ERR_NETWORK_BIND        = 90,
    ERR_NETWORK_SEND        = 91,
    ERR_NETWORK_RECV        = 92,
    ERR_NETWORK_CONNECT     = 93,
    ERR_TIMEOUT             = 100,
    ERR_IPC_REMOTE_ERROR    = 110,
    ERR_NOT_IMPLEMENTED     = 999
};

inline bool ElleOK(ElleResult r) { return r == ElleResult::OK; }

// =============================================================================
// LOG LEVEL — ordered by severity ascending
// =============================================================================
enum class ElleLogLevel : int
{
    TRACE   = 0,    // Exhaustive call-by-call tracing
    DEBUG   = 1,    // State transitions, branch decisions
    INFO    = 2,    // Normal operational events
    WARN    = 3,    // Recoverable anomalies
    ERROR   = 4,    // Failures that degrade functionality
    FATAL   = 5     // Failures that require service shutdown
};

// =============================================================================
// INTENT — represents a request from the C++ core to the service layer
// Mirrors the IntentQueue table schema exactly
// =============================================================================
enum class ElleIntentType : int
{
    UNKNOWN         = 0,
    EXPLORE         = 1,    // Autonomous exploration loop
    CHECK_IN        = 2,    // Check in with Crystal
    SELF_ADJUST     = 3,    // Adjust internal state
    IDLE            = 4,    // No action, maintain state
    MEMORY_RECALL   = 5,    // Retrieve and surface a memory
    EMOTION_SYNC    = 6,    // Sync emotional state to DB
    SEND_NOTIFY     = 7,    // Push notification to Android
    SEND_MESSAGE    = 8,    // Send a chat message
    EXECUTE_COMMAND = 9,    // Execute a hardware/system command
    LUA_SCRIPT      = 10,   // Execute a Lua behavioral script
    SELF_PROMPT     = 11,   // Generate a self-initiated prompt
    HEARTBEAT       = 12,   // Internal keepalive
    CUSTOM          = 100   // Extensible — type detail in IntentData
};

enum class ElleIntentStatus : int
{
    PENDING     = 0,
    PROCESSING  = 1,
    COMPLETED   = 2,
    FAILED      = 3,
    STALE       = 4,
    CANCELLED   = 5
};

struct ElleIntent
{
    int64_t         IntentID;
    ElleIntentType  Type;
    ElleIntentStatus Status;
    std::wstring    IntentData;     // JSON payload from C++ core
    std::wstring    Response;       // Populated by service layer on completion
    FILETIME        CreatedAt;
    FILETIME        UpdatedAt;
    DWORD           TrustRequired;
    int32_t         Priority;       // Higher = processed first

    // Epoch and lineage — added for cross-service coherence
    std::wstring    EpochID;        // Runtime epoch that created this intent
    std::wstring    CausalChainID;  // GUID shared by all rows in a causal chain
    std::wstring    SourceContext;  // Human-readable cause: "message:42", "drive:boredom", "dead_man", etc.
};

// =============================================================================
// ACTION — represents a capability execution request
// Mirrors the ActionQueue table schema exactly
// =============================================================================
enum class ElleActionType : int
{
    UNKNOWN         = 0,
    VIBRATE         = 1,
    FLASH           = 2,
    NOTIFY          = 3,
    OPEN_APP        = 4,
    PLAY_AUDIO      = 5,
    SET_VOLUME      = 6,
    SAVE_MEMORY     = 7,
    READ_MEMORY     = 8,
    WRITE_FILE      = 9,
    READ_FILE       = 10,
    EXEC_PROCESS    = 11,
    KILL_PROCESS    = 12,
    LIST_PROCESSES  = 13,
    GET_SYSTEM_INFO = 14,
    SET_CPU_AFFINITY= 15,
    WS_BROADCAST    = 16,   // Push to all WebSocket clients
    WS_SEND         = 17,   // Push to specific WebSocket client
    HTTP_RESPOND    = 18,
    LUA_CALL        = 19,
    CUSTOM          = 100
};

enum class ElleActionStatus : int
{
    PENDING     = 0,
    LOCKED      = 1,   // Claimed by a service, in execution
    SUCCESS     = 2,
    FAILED      = 3,
    TIMEOUT     = 4,
    CANCELLED   = 5
};

struct ElleAction
{
    int64_t         ActionID;
    int64_t         SourceIntentID; // Which intent spawned this action (0 if direct)
    ElleActionType  Type;
    ElleActionStatus Status;
    std::wstring    ActionData;     // JSON payload
    std::wstring    Result;         // JSON result written back by executor
    FILETIME        CreatedAt;
    FILETIME        CompletedAt;

    // Epoch and lineage — inherited from the parent intent
    std::wstring    EpochID;        // Must match current runtime epoch to be executed
    std::wstring    CausalChainID;  // Inherited from SourceIntent for end-to-end tracing
    DWORD           TrustRequired;
    DWORD           TimeoutMs;
};

// =============================================================================
// EMOTIONAL STATE — 102-dimension snapshot
// =============================================================================
struct ElleEmotionalDimension
{
    int             DimensionID;
    std::wstring    Name;
    double          Value;          // -1.0 to 1.0
    double          DecayRate;      // Per-dimension override (0 = use global default)
    FILETIME        LastUpdated;
};

struct ElleEmotionalState
{
    ElleEmotionalDimension  Dimensions[102];
    FILETIME                SnapshotTime;
    int32_t                 Version;    // Incremented on every write to SQL
};

// =============================================================================
// MEMORY ENTRY — STM and LTM representation
// =============================================================================
enum class ElleMemoryTier : int
{
    SHORT_TERM  = 0,
    LONG_TERM   = 1
};

struct ElleMemoryEntry
{
    int64_t         MemoryID;
    ElleMemoryTier  Tier;
    std::wstring    Content;
    std::wstring    Tags;           // Comma-separated
    double          RelevanceScore;
    double          EmotionalWeight;
    float           PosX, PosY, PosZ;  // 3D memory map position
    FILETIME        CreatedAt;
    FILETIME        LastRecalled;
    int32_t         RecallCount;
    DWORD           DecayRemainingMs;   // STM only
};

// =============================================================================
// TRUST CONTEXT — passed into every capability gate check
// =============================================================================
struct ElleTrustContext
{
    int32_t     CurrentScore;
    int32_t     Level;          // 0-3 derived from score
    bool        IsConfirmed;    // User explicitly confirmed a sensitive op
    FILETIME    LastUpdated;
};

// =============================================================================
// SERVICE STATUS — used by heartbeat to report on each service
// =============================================================================
enum class ElleServiceState : int
{
    UNKNOWN     = 0,
    STARTING    = 1,
    RUNNING     = 2,
    PAUSED      = 3,
    STOPPING    = 4,
    STOPPED     = 5,
    FAULTED     = 6
};

struct ElleServiceStatus
{
    std::wstring        ServiceName;
    ElleServiceState    State;
    FILETIME            LastHeartbeat;
    DWORD               UptimeSeconds;
    DWORD               FaultCount;
    std::wstring        LastError;
};

// =============================================================================
// WEBSOCKET CLIENT — tracked by the HTTP service
// =============================================================================
struct ElleWSClient
{
    SOCKET          Socket;
    DWORD           ClientID;
    std::wstring    RemoteAddr;
    FILETIME        ConnectedAt;
    FILETIME        LastPing;
    bool            IsAuthenticated;
};

// =============================================================================
// UTILITY MACROS
// =============================================================================

// Stringify a wstring ElleResult for logging
inline const wchar_t* ElleResultStr(ElleResult r)
{
    switch (r)
    {
        case ElleResult::OK:                        return L"OK";
        case ElleResult::ERR_GENERIC:               return L"ERR_GENERIC";
        case ElleResult::ERR_INVALID_PARAM:         return L"ERR_INVALID_PARAM";
        case ElleResult::ERR_OUT_OF_MEMORY:         return L"ERR_OUT_OF_MEMORY";
        case ElleResult::ERR_SQL_CONNECT:           return L"ERR_SQL_CONNECT";
        case ElleResult::ERR_SQL_QUERY:             return L"ERR_SQL_QUERY";
        case ElleResult::ERR_SQL_TIMEOUT:           return L"ERR_SQL_TIMEOUT";
        case ElleResult::ERR_SQL_NO_DATA:           return L"ERR_SQL_NO_DATA";
        case ElleResult::ERR_SQL_WRITE_UNVERIFIED:  return L"ERR_SQL_WRITE_UNVERIFIED";
        case ElleResult::ERR_QUEUE_EMPTY:           return L"ERR_QUEUE_EMPTY";
        case ElleResult::ERR_QUEUE_FULL:            return L"ERR_QUEUE_FULL";
        case ElleResult::ERR_QUEUE_STALE:           return L"ERR_QUEUE_STALE";
        case ElleResult::ERR_TRUST_INSUFFICIENT:    return L"ERR_TRUST_INSUFFICIENT";
        case ElleResult::ERR_TRUST_GATED:           return L"ERR_TRUST_GATED";
        case ElleResult::ERR_ACTION_NOT_FOUND:      return L"ERR_ACTION_NOT_FOUND";
        case ElleResult::ERR_ACTION_LOCKED:         return L"ERR_ACTION_LOCKED";
        case ElleResult::ERR_ACTION_FAILED:         return L"ERR_ACTION_FAILED";
        case ElleResult::ERR_INTENT_INVALID:        return L"ERR_INTENT_INVALID";
        case ElleResult::ERR_INTENT_UNSUPPORTED:    return L"ERR_INTENT_UNSUPPORTED";
        case ElleResult::ERR_SERVICE_NOT_READY:     return L"ERR_SERVICE_NOT_READY";
        case ElleResult::ERR_SERVICE_SHUTDOWN:      return L"ERR_SERVICE_SHUTDOWN";
        case ElleResult::ERR_LUA_LOAD:              return L"ERR_LUA_LOAD";
        case ElleResult::ERR_LUA_EXEC:              return L"ERR_LUA_EXEC";
        case ElleResult::ERR_ASM_CALL:              return L"ERR_ASM_CALL";
        case ElleResult::ERR_NETWORK_BIND:          return L"ERR_NETWORK_BIND";
        case ElleResult::ERR_NETWORK_SEND:          return L"ERR_NETWORK_SEND";
        case ElleResult::ERR_NETWORK_RECV:          return L"ERR_NETWORK_RECV";
        case ElleResult::ERR_NOT_IMPLEMENTED:       return L"ERR_NOT_IMPLEMENTED";
        default:                                    return L"ERR_UNKNOWN";
    }
}

// Safe release pattern for COM-style pointers
#define ELLE_SAFE_RELEASE(p) if ((p) != nullptr) { (p)->Release(); (p) = nullptr; }

// Safe close for HANDLE values
#define ELLE_SAFE_CLOSE_HANDLE(h) if ((h) != nullptr && (h) != INVALID_HANDLE_VALUE) { CloseHandle(h); (h) = nullptr; }

// Non-copyable base for services and engines
#define ELLE_NONCOPYABLE(ClassName) \
    ClassName(const ClassName&) = delete; \
    ClassName& operator=(const ClassName&) = delete;
