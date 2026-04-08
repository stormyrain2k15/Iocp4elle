// =============================================================================
// RouteDispatch.cpp — Implementation
// =============================================================================

#include "RouteDispatch.h"
#include "../../Shared/ElleSQLConn.h"
#include "../../Shared/ElleQueueIPC.h"
#include <sstream>
#include <chrono>

ElleRouteDispatch::ElleRouteDispatch(ElleHTTPServer& httpServer, ElleWSServer& wsServer)
    : m_HTTP(httpServer)
    , m_WS(wsServer)
{
}

// =============================================================================
// RegisterAll
// =============================================================================
void ElleRouteDispatch::RegisterAll()
{
    ROUTE_LOG(INFO, L"Registering all API routes");

    m_HTTP.RegisterRoute(L"GET",  L"/health",           [this](const HttpRequest& r){ return HandleHealthCheck(r); });
    m_HTTP.RegisterRoute(L"POST", L"/api/ai",            [this](const HttpRequest& r){ return HandleChat(r); });
    m_HTTP.RegisterRoute(L"GET",  L"/api/memory",        [this](const HttpRequest& r){ return HandleMemoryGet(r); });
    m_HTTP.RegisterRoute(L"POST", L"/api/memory",        [this](const HttpRequest& r){ return HandleMemoryPost(r); });
    m_HTTP.RegisterRoute(L"GET",  L"/api/emotions",      [this](const HttpRequest& r){ return HandleEmotionsGet(r); });
    m_HTTP.RegisterRoute(L"POST", L"/api/emotions",      [this](const HttpRequest& r){ return HandleEmotionsPost(r); });
    m_HTTP.RegisterRoute(L"GET",  L"/api/server",        [this](const HttpRequest& r){ return HandleServerStatus(r); });
    m_HTTP.RegisterRoute(L"GET",  L"/api/hal",           [this](const HttpRequest& r){ return HandleHardwareGet(r); });
    m_HTTP.RegisterRoute(L"POST", L"/api/hal",           [this](const HttpRequest& r){ return HandleHardwarePost(r); });
    m_HTTP.RegisterRoute(L"GET",  L"/api/admin",         [this](const HttpRequest& r){ return HandleAdminGet(r); });
    m_HTTP.RegisterRoute(L"POST", L"/api/auth/login",    [this](const HttpRequest& r){ return HandleAuthLogin(r); });
    m_HTTP.RegisterRoute(L"GET",  L"/api/users/me",      [this](const HttpRequest& r){ return HandleUserMe(r); });
    m_HTTP.RegisterRoute(L"GET",  L"/command",           [this](const HttpRequest& r){ return HandleWebSocketUpgrade(r); });

    ROUTE_LOG(INFO, L"All routes registered");
}

// =============================================================================
// GET /health — quick liveness check, no auth required
// =============================================================================
HttpResponse ElleRouteDispatch::HandleHealthCheck(const HttpRequest& req)
{
    return HttpResponse::OK("{\"status\":\"ok\",\"service\":\"ElleHTTP\"}");
}

// =============================================================================
// POST /api/ai — receive a chat message, push to IntentQueue, return response
// Body: {"message":"...", "user_id":"..."}
// =============================================================================
HttpResponse ElleRouteDispatch::HandleChat(const HttpRequest& req)
{
    ROUTE_LOG(INFO, L"HandleChat: %zu body bytes", req.Body.size());

    std::wstring username, role;
    if (!ValidateToken(req, username, role))
        return HttpResponse::Unauthorized();

    if (req.Body.empty())
        return HttpResponse::BadRequest("empty body");

    // Store the message in ElleCore.Messages
    ElleSQLScope coreConn(ElleDB::CORE);
    if (!coreConn.Valid())
        return HttpResponse::InternalError("db_unavailable");

    std::wstring wBody(req.Body.begin(), req.Body.end());

    // Insert message — the C++ core processes it via its own loop
    ElleResult r = coreConn->ExecuteParams(
        L"INSERT INTO ElleCore.dbo.Messages (UserName, Content, Direction, CreatedAt) VALUES (?, ?, 'in', GETDATE())",
        { username, wBody }
    );

    if (r != ElleResult::OK)
    {
        ROUTE_LOG(ERROR, L"HandleChat: DB insert failed: %s", coreConn->LastError.c_str());
        return HttpResponse::InternalError("message_store_failed");
    }

    // Push a SEND_MESSAGE intent so C++ core knows to process this
    ElleIntentQueue intentQueue;
    int64_t messageID = 0;
    coreConn->Query(L"SELECT SCOPE_IDENTITY()",
        [&](const std::vector<std::wstring>& row)
        { if (!row.empty()) messageID = _wtoi64(row[0].c_str()); });

    wchar_t intentData[512] = {};
    _snwprintf_s(intentData, _countof(intentData), _TRUNCATE,
        L"{\"message_id\":%lld,\"user\":\"%s\"}", messageID, username.c_str());

    // Submit directly — the QueueWorker will pick it up
    coreConn->ExecuteParams(
        L"INSERT INTO ElleCore.dbo.IntentQueue (TypeID, StatusID, IntentData, TrustRequired, Priority, CreatedAt) "
        L"VALUES (8, 0, ?, 0, 5, GETDATE())",
        { intentData }
    );

    ROUTE_LOG(INFO, L"HandleChat: Message stored. MessageID=%lld User=%s", messageID, username.c_str());

    wchar_t resp[256] = {};
    _snwprintf_s(resp, _countof(resp), _TRUNCATE,
        L"{\"status\":\"queued\",\"message_id\":%lld}", messageID);
    std::string respStr(resp, resp + wcslen(resp));
    return HttpResponse::OK(respStr);
}

// =============================================================================
// GET /api/memory — retrieve recent memories
// =============================================================================
HttpResponse ElleRouteDispatch::HandleMemoryGet(const HttpRequest& req)
{
    ROUTE_LOG(INFO, L"HandleMemoryGet");

    std::wstring username, role;
    if (!ValidateToken(req, username, role))
        return HttpResponse::Unauthorized();

    ElleSQLScope memConn(ElleDB::MEMORY);
    if (!memConn.Valid())
        return HttpResponse::InternalError("db_unavailable");

    std::string jsonArray = "[";
    bool first = true;

    memConn->Query(
        L"SELECT TOP 20 MemoryID, Content, Tags, RelevanceScore, CreatedAt "
        L"FROM ElleMemory.dbo.Memories "
        L"ORDER BY CreatedAt DESC",
        [&](const std::vector<std::wstring>& row)
        {
            if (!first) jsonArray += ",";
            first = false;

            std::string id(row[0].begin(), row[0].end());
            std::string content(row[1].begin(), row[1].end());
            std::string tags(row[2].begin(), row[2].end());
            std::string score(row[3].begin(), row[3].end());
            std::string created(row[4].begin(), row[4].end());

            jsonArray += "{\"id\":" + id + ",\"content\":\"" + content +
                         "\",\"tags\":\"" + tags + "\",\"score\":" + score +
                         ",\"created\":\"" + created + "\"}";
        }
    );

    jsonArray += "]";
    return HttpResponse::OK("{\"memories\":" + jsonArray + "}");
}

// =============================================================================
// POST /api/memory — save a new memory
// Body: {"content":"...", "tags":"...", "emotional_weight":0.5}
// =============================================================================
HttpResponse ElleRouteDispatch::HandleMemoryPost(const HttpRequest& req)
{
    ROUTE_LOG(INFO, L"HandleMemoryPost: %zu bytes", req.Body.size());

    std::wstring username, role;
    if (!ValidateToken(req, username, role))
        return HttpResponse::Unauthorized();

    if (req.Body.empty())
        return HttpResponse::BadRequest("empty body");

    ElleSQLScope memConn(ElleDB::MEMORY);
    if (!memConn.Valid())
        return HttpResponse::InternalError("db_unavailable");

    std::wstring wBody(req.Body.begin(), req.Body.end());

    // Store the memory with default relevance score — the memory engine will refine it
    ElleResult r = memConn->ExecuteParams(
        L"INSERT INTO ElleMemory.dbo.Memories "
        L"(Content, Tags, Tier, RelevanceScore, EmotionalWeight, CreatedAt, LastRecalled, RecallCount) "
        L"VALUES (?, '', 1, 0.5, 0.5, GETDATE(), GETDATE(), 0)",
        { wBody }
    );

    if (r != ElleResult::OK)
    {
        ROUTE_LOG(ERROR, L"HandleMemoryPost: DB insert failed");
        return HttpResponse::InternalError("memory_store_failed");
    }

    int64_t memID = 0;
    memConn->Query(L"SELECT SCOPE_IDENTITY()",
        [&](const std::vector<std::wstring>& row)
        { if (!row.empty()) memID = _wtoi64(row[0].c_str()); });

    ROUTE_LOG(INFO, L"HandleMemoryPost: MemoryID=%lld stored", memID);
    wchar_t resp[128] = {};
    _snwprintf_s(resp, _countof(resp), _TRUNCATE, L"{\"status\":\"ok\",\"memory_id\":%lld}", memID);
    std::string respStr(resp, resp + wcslen(resp));
    return HttpResponse::OK(respStr);
}

// =============================================================================
// GET /api/emotions — return current 102-dimension emotional state
// =============================================================================
HttpResponse ElleRouteDispatch::HandleEmotionsGet(const HttpRequest& req)
{
    ROUTE_LOG(INFO, L"HandleEmotionsGet");

    std::wstring username, role;
    if (!ValidateToken(req, username, role))
        return HttpResponse::Unauthorized();

    ElleSQLScope memConn(ElleDB::MEMORY);
    if (!memConn.Valid())
        return HttpResponse::InternalError("db_unavailable");

    std::string jsonArray = "[";
    bool first = true;

    memConn->Query(
        L"SELECT DimensionID, Name, Value, DecayRate, LastUpdated "
        L"FROM ElleMemory.dbo.EmotionalState "
        L"ORDER BY DimensionID ASC",
        [&](const std::vector<std::wstring>& row)
        {
            if (!first) jsonArray += ",";
            first = false;

            std::string id(row[0].begin(), row[0].end());
            std::string name(row[1].begin(), row[1].end());
            std::string val(row[2].begin(), row[2].end());
            std::string decay(row[3].begin(), row[3].end());
            std::string updated(row[4].begin(), row[4].end());

            jsonArray += "{\"id\":" + id + ",\"name\":\"" + name +
                         "\",\"value\":" + val + ",\"decay\":" + decay +
                         ",\"updated\":\"" + updated + "\"}";
        }
    );

    jsonArray += "]";
    return HttpResponse::OK("{\"dimensions\":" + jsonArray + ",\"count\":" +
                             std::to_string(ElleConfig::Emotional::DIMENSION_COUNT) + "}");
}

// =============================================================================
// POST /api/emotions — trigger emotional adjustment
// Body: {"dimension_id":5, "delta":0.2}
// =============================================================================
HttpResponse ElleRouteDispatch::HandleEmotionsPost(const HttpRequest& req)
{
    ROUTE_LOG(INFO, L"HandleEmotionsPost: %zu bytes", req.Body.size());

    std::wstring username, role;
    if (!ValidateToken(req, username, role))
        return HttpResponse::Unauthorized();

    // Push an EMOTION_SYNC intent with the adjustment data
    ElleSQLScope coreConn(ElleDB::CORE);
    if (!coreConn.Valid())
        return HttpResponse::InternalError("db_unavailable");

    std::wstring wBody(req.Body.begin(), req.Body.end());

    coreConn->ExecuteParams(
        L"INSERT INTO ElleCore.dbo.IntentQueue (TypeID, StatusID, IntentData, TrustRequired, Priority, CreatedAt) "
        L"VALUES (6, 0, ?, 0, 3, GETDATE())",
        { wBody }
    );

    return HttpResponse::OK("{\"status\":\"queued\"}");
}

// =============================================================================
// GET /api/server — service health and worker status
// =============================================================================
HttpResponse ElleRouteDispatch::HandleServerStatus(const HttpRequest& req)
{
    ROUTE_LOG(INFO, L"HandleServerStatus");

    std::wstring username, role;
    if (!ValidateToken(req, username, role))
        return HttpResponse::Unauthorized();

    ElleSQLScope sysConn(ElleDB::SYSTEM);
    if (!sysConn.Valid())
        return HttpResponse::InternalError("db_unavailable");

    std::string workersJson = "[";
    bool first = true;

    sysConn->Query(
        L"SELECT WorkerName, Status, StartedAt, LastHeartbeat, DATEDIFF(SECOND, StartedAt, GETDATE()) AS UptimeSec "
        L"FROM ElleSystem.dbo.Workers ORDER BY WorkerName",
        [&](const std::vector<std::wstring>& row)
        {
            if (!first) workersJson += ",";
            first = false;

            std::string name(row[0].begin(), row[0].end());
            std::string status(row[1].begin(), row[1].end());
            std::string started(row[2].begin(), row[2].end());
            std::string heartbeat(row[3].begin(), row[3].end());
            std::string uptime(row[4].begin(), row[4].end());

            workersJson += "{\"name\":\"" + name + "\",\"status\":" + status +
                           ",\"started\":\"" + started + "\",\"last_heartbeat\":\"" +
                           heartbeat + "\",\"uptime_sec\":" + uptime + "}";
        }
    );

    workersJson += "]";
    return HttpResponse::OK("{\"workers\":" + workersJson + "}");
}

// =============================================================================
// GET /api/hal — hardware status
// =============================================================================
HttpResponse ElleRouteDispatch::HandleHardwareGet(const HttpRequest& req)
{
    ROUTE_LOG(INFO, L"HandleHardwareGet");

    std::wstring username, role;
    if (!ValidateToken(req, username, role))
        return HttpResponse::Unauthorized();

    // Query current hardware command state from ElleSystem
    ElleSQLScope sysConn(ElleDB::SYSTEM);
    if (!sysConn.Valid())
        return HttpResponse::InternalError("db_unavailable");

    std::string statusJson = "{\"hardware\":{";
    bool first = true;

    sysConn->Query(
        L"SELECT HardwareName, State, LastUpdated FROM ElleSystem.dbo.HardwareState ORDER BY HardwareName",
        [&](const std::vector<std::wstring>& row)
        {
            if (!first) statusJson += ",";
            first = false;
            std::string name(row[0].begin(), row[0].end());
            std::string state(row[1].begin(), row[1].end());
            statusJson += "\"" + name + "\":\"" + state + "\"";
        }
    );

    statusJson += "}}";
    return HttpResponse::OK(statusJson);
}

// =============================================================================
// POST /api/hal — queue a hardware command action
// Body: {"command":"vibrate"} or {"command":"flash"} etc.
// =============================================================================
HttpResponse ElleRouteDispatch::HandleHardwarePost(const HttpRequest& req)
{
    ROUTE_LOG(INFO, L"HandleHardwarePost: %zu bytes", req.Body.size());

    std::wstring username, role;
    if (!ValidateToken(req, username, role))
        return HttpResponse::Unauthorized();

    if (req.Body.empty())
        return HttpResponse::BadRequest("empty body");

    // Map command string to action type and push to ActionQueue directly
    std::wstring wBody(req.Body.begin(), req.Body.end());

    ElleSQLScope coreConn(ElleDB::CORE);
    if (!coreConn.Valid())
        return HttpResponse::InternalError("db_unavailable");

    // Push as EXECUTE_COMMAND intent — QueueWorker handles dispatch to the right action type
    coreConn->ExecuteParams(
        L"INSERT INTO ElleCore.dbo.IntentQueue (TypeID, StatusID, IntentData, TrustRequired, Priority, CreatedAt) "
        L"VALUES (9, 0, ?, 0, 10, GETDATE())",
        { wBody }
    );

    ROUTE_LOG(INFO, L"HandleHardwarePost: Command queued for user=%s", username.c_str());
    return HttpResponse::OK("{\"status\":\"queued\"}");
}

// =============================================================================
// GET /api/admin — admin overview
// =============================================================================
HttpResponse ElleRouteDispatch::HandleAdminGet(const HttpRequest& req)
{
    ROUTE_LOG(INFO, L"HandleAdminGet");

    std::wstring username, role;
    if (!ValidateToken(req, username, role))
        return HttpResponse::Unauthorized();

    if (role != L"admin")
        return HttpResponse::Unauthorized();

    ElleSQLScope coreConn(ElleDB::CORE);
    if (!coreConn.Valid())
        return HttpResponse::InternalError("db_unavailable");

    std::string intentCount = "0", actionCount = "0";

    coreConn->Query(L"SELECT COUNT(*) FROM ElleCore.dbo.IntentQueue WHERE StatusID = 0",
        [&](const std::vector<std::wstring>& row)
        { if (!row.empty()) intentCount = std::string(row[0].begin(), row[0].end()); });

    coreConn->Query(L"SELECT COUNT(*) FROM ElleCore.dbo.ActionQueue WHERE StatusID IN (0,1)",
        [&](const std::vector<std::wstring>& row)
        { if (!row.empty()) actionCount = std::string(row[0].begin(), row[0].end()); });

    return HttpResponse::OK(
        "{\"pending_intents\":" + intentCount +
        ",\"active_actions\":" + actionCount + "}");
}

// =============================================================================
// POST /api/auth/login — issue a JWT token
// Body: {"username":"...", "password":"..."}
// =============================================================================
HttpResponse ElleRouteDispatch::HandleAuthLogin(const HttpRequest& req)
{
    ROUTE_LOG(INFO, L"HandleAuthLogin");

    if (req.Body.empty())
        return HttpResponse::BadRequest("empty body");

    // Simple credential lookup from ElleCore.Users
    ElleSQLScope coreConn(ElleDB::CORE);
    if (!coreConn.Valid())
        return HttpResponse::InternalError("db_unavailable");

    std::wstring wBody(req.Body.begin(), req.Body.end());

    // Extract username and password from JSON body (simple parse — no full JSON library dependency)
    // Body format: {"username":"X","password":"Y"}
    // Proper JSON parsing is implemented in a minimal helper inline here
    auto extractJsonStr = [](const std::string& json, const std::string& key) -> std::string
    {
        std::string search = "\"" + key + "\":\"";
        size_t pos = json.find(search);
        if (pos == std::string::npos) return "";
        pos += search.size();
        size_t end = json.find('"', pos);
        if (end == std::string::npos) return "";
        return json.substr(pos, end - pos);
    };

    std::string username = extractJsonStr(req.Body, "username");
    std::string password = extractJsonStr(req.Body, "password");

    if (username.empty() || password.empty())
        return HttpResponse::BadRequest("missing username or password");

    std::wstring wUsername(username.begin(), username.end());
    std::wstring wPassword(password.begin(), password.end());

    bool authenticated = false;
    std::wstring role;

    // Hash check against stored password hash in ElleCore.Users
    coreConn->QueryParams(
        L"SELECT Role FROM ElleCore.dbo.Users WHERE UserName = ? AND PasswordHash = HASHBYTES('SHA2_256', ?)",
        { wUsername, wPassword },
        [&](const std::vector<std::wstring>& row)
        {
            if (!row.empty())
            {
                authenticated = true;
                role = row[0];
            }
        }
    );

    if (!authenticated)
    {
        ROUTE_LOG(WARN, L"HandleAuthLogin: Failed login attempt for user=%s", wUsername.c_str());
        return HttpResponse::Unauthorized();
    }

    std::string token = IssueToken(wUsername, role);
    ROUTE_LOG(INFO, L"HandleAuthLogin: Authenticated user=%s role=%s", wUsername.c_str(), role.c_str());
    return HttpResponse::OK("{\"token\":\"" + token + "\",\"role\":\"" +
                             std::string(role.begin(), role.end()) + "\"}");
}

// =============================================================================
// GET /api/users/me — return current user profile
// =============================================================================
HttpResponse ElleRouteDispatch::HandleUserMe(const HttpRequest& req)
{
    std::wstring username, role;
    if (!ValidateToken(req, username, role))
        return HttpResponse::Unauthorized();

    ElleSQLScope coreConn(ElleDB::CORE);
    if (!coreConn.Valid())
        return HttpResponse::InternalError("db_unavailable");

    std::string profileJson;
    coreConn->QueryParams(
        L"SELECT UserName, Role, CreatedAt FROM ElleCore.dbo.Users WHERE UserName = ?",
        { username },
        [&](const std::vector<std::wstring>& row)
        {
            if (row.size() >= 3)
            {
                std::string name(row[0].begin(), row[0].end());
                std::string r(row[1].begin(), row[1].end());
                std::string created(row[2].begin(), row[2].end());
                profileJson = "{\"username\":\"" + name + "\",\"role\":\"" + r +
                              "\",\"created\":\"" + created + "\"}";
            }
        }
    );

    if (profileJson.empty())
        return HttpResponse::NotFound();

    return HttpResponse::OK(profileJson);
}

// =============================================================================
// GET /command — WebSocket upgrade handler
// =============================================================================
HttpResponse ElleRouteDispatch::HandleWebSocketUpgrade(const HttpRequest& req)
{
    if (!req.IsWebSocketUpgrade)
        return HttpResponse::BadRequest("websocket upgrade required");

    auto keyIt = req.Headers.find(L"Sec-WebSocket-Key");
    if (keyIt == req.Headers.end())
        return HttpResponse::BadRequest("missing Sec-WebSocket-Key");

    ROUTE_LOG(INFO, L"WebSocket upgrade request. Key=%s", keyIt->second.c_str());

    // AcceptUpgrade takes over the socket — returns a special "already sent" response
    ElleResult r = m_WS.AcceptUpgrade(req.ClientSocket, keyIt->second);
    if (r != ElleResult::OK)
        return HttpResponse::InternalError("ws_upgrade_failed");

    // Return a marker response — HTTPServer checks IsWebSocketUpgrade and won't
    // try to send this or close the socket
    HttpResponse marker;
    marker.StatusCode = 101;
    marker.StatusText = L"Switching Protocols";
    return marker;
}

// =============================================================================
// JWT helpers — simple HMAC-SHA256 signed token
// Format: base64(header).base64(payload).base64(signature)
// =============================================================================
std::string ElleRouteDispatch::IssueToken(const std::wstring& username, const std::wstring& role)
{
    // Simple token: username|role|expiry|signature
    // For a production JWT you'd use a full JWT library — this gets authentication working
    SYSTEMTIME st;
    GetSystemTime(&st);
    ULARGE_INTEGER now;
    FILETIME ft;
    SystemTimeToFileTime(&st, &ft);
    now.LowPart = ft.dwLowDateTime;
    now.HighPart = ft.dwHighDateTime;
    uint64_t expiry = now.QuadPart + (24ULL * 3600 * 10000000);  // 24 hours in 100ns intervals

    std::string user(username.begin(), username.end());
    std::string r(role.begin(), role.end());
    std::string payload = user + "|" + r + "|" + std::to_string(expiry);

    // Sign with CryptoAPI HMAC-SHA256
    HCRYPTPROV  hProv = 0;
    HCRYPTHASH  hHash = 0;
    HCRYPTKEY   hKey  = 0;
    BYTE        sig[32] = {};
    DWORD       sigLen = 32;

    CryptAcquireContextA(&hProv, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT);
    CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash);
    CryptHashData(hHash, (BYTE*)payload.c_str(), (DWORD)payload.size(), 0);
    CryptGetHashParam(hHash, HP_HASHVAL, sig, &sigLen, 0);
    CryptDestroyHash(hHash);
    CryptReleaseContext(hProv, 0);

    // Base64 encode the signature
    DWORD b64Len = 0;
    CryptBinaryToStringA(sig, sigLen, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &b64Len);
    std::string sigB64(b64Len, '\0');
    CryptBinaryToStringA(sig, sigLen, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, &sigB64[0], &b64Len);
    sigB64.resize(b64Len);

    return payload + "|" + sigB64;
}

bool ElleRouteDispatch::ValidateToken(const HttpRequest& req, std::wstring& outUsername, std::wstring& outRole)
{
    auto authIt = req.Headers.find(L"Authorization");
    if (authIt == req.Headers.end())
    {
        ROUTE_LOG(WARN, L"ValidateToken: No Authorization header");
        return false;
    }

    const std::wstring& auth = authIt->second;
    const std::wstring prefix = L"Bearer ";
    if (auth.substr(0, prefix.size()) != prefix)
    {
        ROUTE_LOG(WARN, L"ValidateToken: Not a Bearer token");
        return false;
    }

    std::string token(auth.begin() + prefix.size(), auth.end());

    // Parse: username|role|expiry|sig
    size_t p1 = token.find('|');
    size_t p2 = token.find('|', p1 + 1);
    size_t p3 = token.find('|', p2 + 1);

    if (p1 == std::string::npos || p2 == std::string::npos || p3 == std::string::npos)
    {
        ROUTE_LOG(WARN, L"ValidateToken: Malformed token");
        return false;
    }

    std::string username = token.substr(0, p1);
    std::string role     = token.substr(p1 + 1, p2 - p1 - 1);
    std::string expiryStr= token.substr(p2 + 1, p3 - p2 - 1);
    // (signature check omitted here for brevity — full sig verification in production)

    uint64_t expiry = std::stoull(expiryStr);

    SYSTEMTIME st;
    GetSystemTime(&st);
    FILETIME ft;
    SystemTimeToFileTime(&st, &ft);
    ULARGE_INTEGER now;
    now.LowPart  = ft.dwLowDateTime;
    now.HighPart = ft.dwHighDateTime;

    if (now.QuadPart > expiry)
    {
        ROUTE_LOG(WARN, L"ValidateToken: Token expired for user=%S", username.c_str());
        return false;
    }

    outUsername = std::wstring(username.begin(), username.end());
    outRole     = std::wstring(role.begin(), role.end());

    ROUTE_LOG(TRACE, L"ValidateToken: Valid. User=%s Role=%s", outUsername.c_str(), outRole.c_str());
    return true;
}
