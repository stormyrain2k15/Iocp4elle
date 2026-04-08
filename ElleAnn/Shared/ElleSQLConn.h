#pragma once
// =============================================================================
// ElleSQLConn.h — SQL Server Connection Pool for the Elle-Ann ESI Platform
//
// Manages a pool of ODBC connections to each of the five Elle databases.
// All services use this instead of managing their own connections.
//
// Thread-safe. Connections are checked out, used, and returned to the pool.
// If a connection is found dead on checkout it is dropped and a fresh one
// is established transparently. Retry logic is built in per ElleConfig::DB.
//
// Usage:
//   auto conn = ElleSQLPool::Get().Checkout(ElleDB::CORE);
//   if (!conn) { ELLE_LOG_ERROR(...); return ElleResult::ERR_SQL_CONNECT; }
//   // use conn->Execute(...)
//   ElleSQLPool::Get().Return(conn);   // or use RAII via ElleSQLScope
// =============================================================================

#include "ElleTypes.h"
#include "ElleConfig.h"
#include "ElleLogger.h"
#include <sqlext.h>
#include <sql.h>
#include <string>
#include <vector>
#include <mutex>
#include <memory>
#include <functional>

// Which database to connect to
enum class ElleDB : int
{
    CORE        = 0,
    MEMORY      = 1,
    KNOWLEDGE   = 2,
    SYSTEM      = 3,
    HEART       = 4,
    COUNT       = 5
};

// =============================================================================
// ElleSQLResult — structured result from any SQL operation.
//
// Replaces the pattern of callers inferring success from ElleResult alone.
// Every write path now returns this object so the caller always knows:
//   - Whether the command executed without error     (WriteSucceeded)
//   - How many rows the server reports were changed  (RowsAffected)
//   - Whether a subsequent read confirmed the values (WriteVerified)
//   - The full error context if anything went wrong  (ErrorDetail)
//
// WriteSucceeded and WriteVerified are separate concepts deliberately.
// WriteSucceeded = true means ODBC returned success and RowsAffected > 0.
// WriteVerified  = true means a read-back confirmed the persisted values
//                  match what was written, field by field.
// A result where WriteSucceeded is true but WriteVerified is false means
// the write round-tripped cleanly but persistence cannot be confirmed —
// the correct status code in that case is ERR_SQL_WRITE_UNVERIFIED.
// =============================================================================
struct ElleSQLResult
{
    ElleResult      Status          = ElleResult::ERR_GENERIC;
    int             RowsAffected    = 0;    // From proc OUTPUT or ROWCOUNT batch
    int             RowsFetched     = 0;    // Rows delivered to rowCallback
    bool            WriteSucceeded  = false;// Write cmd executed, RowsAffected > 0
    bool            WriteVerified   = false;// Read-back confirmed persisted values
    std::wstring    ErrorDetail;            // Populated on any failure

    bool OK()       const { return Status == ElleResult::OK; }
    bool Verified() const { return WriteSucceeded && WriteVerified; }
};

// =============================================================================
// ElleSQLConnection — one live ODBC connection
// =============================================================================
class ElleSQLConnection
{
    ELLE_NONCOPYABLE(ElleSQLConnection)
public:
    ElleSQLConnection(ElleDB db, const std::wstring& connStr);
    ~ElleSQLConnection();

    // Connect/disconnect
    ElleResult Connect();
    void Disconnect();
    bool IsConnected() const;
    bool Ping();        // Sends a trivial SELECT 1 to verify connection is alive

    // Execute a statement with no result set (INSERT, UPDATE, DELETE, EXEC)
    // Returns ERR_SQL_QUERY on failure, sets lastError
    ElleResult Execute(const std::wstring& sql);

    // Execute a parameterized statement. params is a vector of wstring values
    // bound positionally to ? placeholders in sql.
    ElleResult ExecuteParams(
        const std::wstring& sql,
        const std::vector<std::wstring>& params
    );

    // Execute a write statement and return a structured result that captures
    // rows affected in the same batch (no separate @@ROWCOUNT round-trip).
    // The sql must be a single statement — DECLARE/@affected wrapping is
    // applied internally. outResult.RowsAffected is populated from the server.
    ElleResult ExecuteStructured(
        const std::wstring& sql,
        const std::vector<std::wstring>& params,
        ElleSQLResult& outResult
    );

    // Execute a stored procedure and return structured result.
    // The proc must SELECT its affected row count as its last result set.
    // See the stored procedure documentation in ElleAnn_Schema.sql.
    ElleResult ExecProcStructured(
        const std::wstring& procName,
        const std::vector<std::pair<std::wstring, std::wstring>>& params,
        ElleSQLResult& outResult,
        std::function<void(const std::vector<std::wstring>&)> rowCallback = nullptr
    );

    // Execute a query and invoke callback once per row.
    // callback receives a vector of wstring column values.
    // Returns ERR_SQL_NO_DATA if no rows returned.
    ElleResult Query(
        const std::wstring& sql,
        std::function<void(const std::vector<std::wstring>&)> rowCallback
    );

    // Execute a parameterized query
    ElleResult QueryParams(
        const std::wstring& sql,
        const std::vector<std::wstring>& params,
        std::function<void(const std::vector<std::wstring>&)> rowCallback
    );

    // Query variant that returns ERR_SQL_NO_DATA when zero rows come back.
    // Use this when the caller genuinely requires at least one row (e.g. auth
    // checks, existence lookups). For cases where zero rows is a valid outcome
    // (queue polling, conditional reads), use Query() and check outRowCount.
    ElleResult QueryRows(
        const std::wstring& sql,
        std::function<void(const std::vector<std::wstring>&)> rowCallback,
        int& outRowCount
    );

    // Call a stored procedure with named parameters
    ElleResult ExecProc(
        const std::wstring& procName,
        const std::vector<std::pair<std::wstring, std::wstring>>& params
    );

    // Last SQL error state (populated on any failure)
    std::wstring    LastError;
    ElleDB          Database;

private:
    // Extract error info from ODBC handles into LastError
    void CaptureError(SQLSMALLINT handleType, SQLHANDLE handle);

    // Internal query execution after statement is prepared and params bound
    ElleResult ExecuteStatement(SQLHSTMT hStmt,
        std::function<void(const std::vector<std::wstring>&)> rowCallback,
        int* outRowCount = nullptr);

    std::wstring    m_ConnStr;
    SQLHENV         m_hEnv;
    SQLHDBC         m_hDBC;
    bool            m_Connected;
};

// =============================================================================
// ElleSQLScope — RAII checkout wrapper
// Checks out a connection on construction, returns it on destruction.
// Usage:
//   ElleSQLScope conn(ElleDB::CORE);
//   if (!conn.Valid()) return ElleResult::ERR_SQL_CONNECT;
//   conn->Execute(L"...");
// =============================================================================
class ElleSQLScope
{
    ELLE_NONCOPYABLE(ElleSQLScope)
public:
    explicit ElleSQLScope(ElleDB db);
    ~ElleSQLScope();

    bool Valid() const              { return m_Conn != nullptr; }
    ElleSQLConnection* operator->() { return m_Conn; }
    ElleSQLConnection& operator*()  { return *m_Conn; }

private:
    ElleSQLConnection* m_Conn;
    ElleDB             m_DB;
};

// =============================================================================
// ElleSQLPool — singleton connection pool
// =============================================================================
class ElleSQLPool
{
    ELLE_NONCOPYABLE(ElleSQLPool)
public:
    static ElleSQLPool& Get();

    // Initialize pool for all databases. Must be called once at service startup.
    // serverInstance: e.g. L"localhost\\ELLE"
    ElleResult Init(const std::wstring& serverInstance = ElleConfig::DB::SERVER);

    // Shutdown — disconnect and destroy all pooled connections
    void Shutdown();

    // Check out a connection. Returns nullptr if pool exhausted or connect fails.
    // Caller MUST call Return() when done.
    ElleSQLConnection* Checkout(ElleDB db);

    // Return a connection to the pool
    void Return(ElleSQLConnection* conn);

    // Pool statistics
    int ActiveCount(ElleDB db) const;
    int IdleCount(ElleDB db) const;

private:
    ElleSQLPool();
    ~ElleSQLPool();

    // Build ODBC connection string for a given database
    std::wstring BuildConnStr(ElleDB db, const std::wstring& server) const;

    struct PoolSlot
    {
        ElleSQLConnection*  Conn;
        bool                InUse;
    };

    std::vector<PoolSlot>   m_Pools[static_cast<int>(ElleDB::COUNT)];
    mutable std::mutex      m_Mutex[static_cast<int>(ElleDB::COUNT)];
    std::wstring            m_Server;
    bool                    m_Initialized;
};
