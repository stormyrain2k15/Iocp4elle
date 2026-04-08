// =============================================================================
// LuaHost.cpp — Implementation
// =============================================================================

#include "LuaHost.h"
#include "../Shared/ElleSQLConn.h"
#include "../Shared/ElleQueueIPC.h"

ElleLuaHost::ElleLuaHost()
    : m_State(nullptr)
    , m_Initialized(false)
    , m_HotReloadThread(nullptr)
    , m_StopEvent(nullptr)
    , m_ReloadCount(0)
{
}

ElleLuaHost::~ElleLuaHost()
{
    Shutdown();
}

// =============================================================================
// Init
// =============================================================================
ElleResult ElleLuaHost::Init()
{
    LUA_LOG(INFO, L"Initializing Lua host. ScriptDir=%s", ElleConfig::Lua::SCRIPT_DIR);

    std::lock_guard<std::mutex> lock(m_StateMutex);

    // Create a new Lua state
    m_State = luaL_newstate();
    if (!m_State)
    {
        LUA_LOG(FATAL, L"luaL_newstate() returned NULL — out of memory");
        return ElleResult::ERR_OUT_OF_MEMORY;
    }

    // Open safe standard libraries only — no io, no os, no debug
    // We explicitly open only what Elle's scripts need
    luaL_requiref(m_State, "_G",        luaopen_base,    1); lua_pop(m_State, 1);
    luaL_requiref(m_State, "math",      luaopen_math,    1); lua_pop(m_State, 1);
    luaL_requiref(m_State, "string",    luaopen_string,  1); lua_pop(m_State, 1);
    luaL_requiref(m_State, "table",     luaopen_table,   1); lua_pop(m_State, 1);

    // Register C++ bindings as the "elle" global table
    RegisterBindings();

    // Register script files
    auto addScript = [&](const wchar_t* filename) {
        ScriptEntry entry;
        entry.Path      = std::wstring(ElleConfig::Lua::SCRIPT_DIR) + filename;
        entry.Name      = std::string(filename, filename + wcslen(filename) - 4); // strip .lua
        entry.Loaded    = false;
        ZeroMemory(&entry.LastModified, sizeof(entry.LastModified));
        m_Scripts.push_back(entry);
    };

    addScript(ElleConfig::Lua::THRESHOLDS);     // Load thresholds first — others may depend on it
    addScript(ElleConfig::Lua::PERSONALITY);
    addScript(ElleConfig::Lua::INTENT_SCORING);
    addScript(ElleConfig::Lua::REASONING);

    // Load all scripts
    int loaded = 0;
    int failed = 0;
    for (auto& script : m_Scripts)
    {
        std::string chunkName = "@" + script.Name;
        ElleResult r = LoadScript(script.Path, chunkName);
        if (r == ElleResult::OK)
        {
            script.Loaded = true;
            // Record modification time for hot-reload tracking
            WIN32_FILE_ATTRIBUTE_DATA info = {};
            if (GetFileAttributesExW(script.Path.c_str(), GetFileExInfoStandard, &info))
                script.LastModified = info.ftLastWriteTime;
            loaded++;
        }
        else
        {
            LUA_LOG(WARN, L"Script '%s' failed to load — behavioral layer running without it",
                script.Path.c_str());
            failed++;
        }
    }

    m_Initialized = true;
    LUA_LOG(INFO, L"Lua host initialized. Scripts loaded=%d failed=%d", loaded, failed);
    return ElleResult::OK;
}

// =============================================================================
// RegisterBindings — builds the "elle" Lua table with C++ function entries
// =============================================================================
void ElleLuaHost::RegisterBindings()
{
    // Create the "elle" global table
    lua_newtable(m_State);

    // elle.log(level, message)
    lua_pushcfunction(m_State, LuaLog);
    lua_setfield(m_State, -2, "log");

    // elle.query(db_index, sql) -> table of rows
    lua_pushcfunction(m_State, LuaQuery);
    lua_setfield(m_State, -2, "query");

    // elle.execute(db_index, sql) -> bool
    lua_pushcfunction(m_State, LuaExecute);
    lua_setfield(m_State, -2, "execute");

    // elle.pushIntent(typeID, data, priority) -> bool
    lua_pushcfunction(m_State, LuaPushIntent);
    lua_setfield(m_State, -2, "pushIntent");

    // elle.getEmotion(dimensionID) -> number [-1.0, 1.0]
    lua_pushcfunction(m_State, LuaGetEmotion);
    lua_setfield(m_State, -2, "getEmotion");

    // elle.getTrust() -> number
    lua_pushcfunction(m_State, LuaGetTrust);
    lua_setfield(m_State, -2, "getTrust");

    // Set the table as global "elle"
    lua_setglobal(m_State, "elle");

    LUA_LOG(DEBUG, L"RegisterBindings: elle.* bindings registered");
}

// =============================================================================
// LoadScript
// =============================================================================
ElleResult ElleLuaHost::LoadScript(const std::wstring& path, const std::string& chunkName)
{
    LUA_LOG(INFO, L"LoadScript: %s", path.c_str());

    // Convert wide path to narrow for luaL_loadfile
    char narrowPath[MAX_PATH] = {};
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, narrowPath, MAX_PATH, nullptr, nullptr);

    int ret = luaL_loadfile(m_State, narrowPath);
    if (ret != LUA_OK)
    {
        const char* errMsg = lua_tostring(m_State, -1);
        LUA_LOG(ERROR, L"LoadScript: luaL_loadfile failed for %s: %S", path.c_str(), errMsg ? errMsg : "unknown");
        lua_pop(m_State, 1);  // Pop the error
        return ElleResult::ERR_LUA_LOAD;
    }

    // Execute the script (runs the top-level chunk, defines functions)
    ret = lua_pcall(m_State, 0, 0, 0);
    if (ret != LUA_OK)
    {
        const char* errMsg = lua_tostring(m_State, -1);
        LUA_LOG(ERROR, L"LoadScript: lua_pcall failed for %s: %S", path.c_str(), errMsg ? errMsg : "unknown");
        lua_pop(m_State, 1);
        return ElleResult::ERR_LUA_EXEC;
    }

    LUA_LOG(INFO, L"LoadScript: %s loaded OK", path.c_str());
    return ElleResult::OK;
}

// =============================================================================
// ReloadAll
// =============================================================================
ElleResult ElleLuaHost::ReloadAll()
{
    LUA_LOG(INFO, L"ReloadAll: Reloading all Lua scripts");
    std::lock_guard<std::mutex> lock(m_StateMutex);

    // Close old state, open fresh one
    if (m_State)
    {
        lua_close(m_State);
        m_State = nullptr;
    }

    m_Initialized = false;
    m_ReloadCount.fetch_add(1, std::memory_order_relaxed);

    // Re-init (without holding the lock — Init acquires it)
    // Temporarily release then re-acquire
    // Actually Init acquires its own lock — but we hold it. Use internal reinit.
    m_State = luaL_newstate();
    if (!m_State) return ElleResult::ERR_OUT_OF_MEMORY;

    luaL_requiref(m_State, "_G",     luaopen_base,   1); lua_pop(m_State, 1);
    luaL_requiref(m_State, "math",   luaopen_math,   1); lua_pop(m_State, 1);
    luaL_requiref(m_State, "string", luaopen_string, 1); lua_pop(m_State, 1);
    luaL_requiref(m_State, "table",  luaopen_table,  1); lua_pop(m_State, 1);

    RegisterBindings();

    int loaded = 0;
    for (auto& script : m_Scripts)
    {
        std::string chunkName = "@" + script.Name;
        ElleResult r = LoadScript(script.Path, chunkName);
        if (r == ElleResult::OK)
        {
            script.Loaded = true;
            WIN32_FILE_ATTRIBUTE_DATA info = {};
            if (GetFileAttributesExW(script.Path.c_str(), GetFileExInfoStandard, &info))
                script.LastModified = info.ftLastWriteTime;
            loaded++;
        }
    }

    m_Initialized = true;
    LUA_LOG(INFO, L"ReloadAll: Complete. Scripts loaded=%d ReloadCount=%llu",
        loaded, m_ReloadCount.load());
    return ElleResult::OK;
}

// =============================================================================
// StartHotReload
// =============================================================================
ElleResult ElleLuaHost::StartHotReload(HANDLE stopEvent)
{
    m_StopEvent = stopEvent;
    m_HotReloadThread = CreateThread(nullptr, 0, HotReloadProc, this, 0, nullptr);
    if (!m_HotReloadThread)
    {
        LUA_LOG(ERROR, L"StartHotReload: CreateThread failed: %lu", GetLastError());
        return ElleResult::ERR_GENERIC;
    }
    LUA_LOG(INFO, L"Hot-reload watcher running. CheckInterval=%dms", ElleConfig::Lua::HOT_RELOAD_CHECK_MS);
    return ElleResult::OK;
}

DWORD WINAPI ElleLuaHost::HotReloadProc(LPVOID param)
{
    static_cast<ElleLuaHost*>(param)->HotReloadLoop();
    return 0;
}

void ElleLuaHost::HotReloadLoop()
{
    while (true)
    {
        DWORD wait = WaitForSingleObject(m_StopEvent, ElleConfig::Lua::HOT_RELOAD_CHECK_MS);
        if (wait == WAIT_OBJECT_0) break;
        CheckAndReload();
    }
    LUA_LOG(INFO, L"Hot-reload loop exiting");
}

void ElleLuaHost::CheckAndReload()
{
    bool anyChanged = false;

    for (auto& script : m_Scripts)
    {
        WIN32_FILE_ATTRIBUTE_DATA info = {};
        if (!GetFileAttributesExW(script.Path.c_str(), GetFileExInfoStandard, &info))
            continue;

        if (CompareFileTime(&info.ftLastWriteTime, &script.LastModified) != 0)
        {
            LUA_LOG(INFO, L"Hot-reload: Script changed: %s", script.Path.c_str());
            anyChanged = true;
        }
    }

    if (anyChanged)
        ReloadAll();
}

// =============================================================================
// CallVoid
// =============================================================================
ElleResult ElleLuaHost::CallVoid(const std::string& funcName)
{
    std::lock_guard<std::mutex> lock(m_StateMutex);
    if (!m_State || !m_Initialized) return ElleResult::ERR_SERVICE_NOT_READY;

    lua_getglobal(m_State, funcName.c_str());
    if (!lua_isfunction(m_State, -1))
    {
        lua_pop(m_State, 1);
        LUA_LOG(WARN, L"CallVoid: '%S' is not a function", funcName.c_str());
        return ElleResult::ERR_LUA_EXEC;
    }

    int ret = lua_pcall(m_State, 0, 0, 0);
    if (ret != LUA_OK)
    {
        const char* err = lua_tostring(m_State, -1);
        LUA_LOG(ERROR, L"CallVoid '%S': %S", funcName.c_str(), err ? err : "error");
        lua_pop(m_State, 1);
        return ElleResult::ERR_LUA_EXEC;
    }
    return ElleResult::OK;
}

// =============================================================================
// ScoreIntent — calls intent_scoring.lua's score_intent(typeID, data) -> number
// =============================================================================
ElleResult ElleLuaHost::ScoreIntent(int intentType, const std::wstring& intentData, double& outScore)
{
    std::lock_guard<std::mutex> lock(m_StateMutex);
    if (!m_State || !m_Initialized) { outScore = 5.0; return ElleResult::OK; }

    lua_getglobal(m_State, "score_intent");
    if (!lua_isfunction(m_State, -1))
    {
        lua_pop(m_State, 1);
        outScore = 5.0;  // Default priority
        return ElleResult::OK;
    }

    lua_pushinteger(m_State, intentType);
    std::string narrowData(intentData.begin(), intentData.end());
    lua_pushstring(m_State, narrowData.c_str());

    int ret = lua_pcall(m_State, 2, 1, 0);
    if (ret != LUA_OK)
    {
        const char* err = lua_tostring(m_State, -1);
        LUA_LOG(WARN, L"ScoreIntent: lua_pcall failed: %S", err ? err : "error");
        lua_pop(m_State, 1);
        outScore = 5.0;
        return ElleResult::ERR_LUA_EXEC;
    }

    outScore = lua_isnumber(m_State, -1) ? lua_tonumber(m_State, -1) : 5.0;
    lua_pop(m_State, 1);
    return ElleResult::OK;
}

// =============================================================================
// BuildPrompt — calls reasoning.lua's build_prompt(context) -> string
// =============================================================================
ElleResult ElleLuaHost::BuildPrompt(const std::wstring& context, std::wstring& outPrompt)
{
    std::lock_guard<std::mutex> lock(m_StateMutex);
    if (!m_State || !m_Initialized)
    {
        outPrompt = context;  // Pass-through if Lua not available
        return ElleResult::OK;
    }

    lua_getglobal(m_State, "build_prompt");
    if (!lua_isfunction(m_State, -1))
    {
        lua_pop(m_State, 1);
        outPrompt = context;
        return ElleResult::OK;
    }

    std::string narrowCtx(context.begin(), context.end());
    lua_pushstring(m_State, narrowCtx.c_str());

    int ret = lua_pcall(m_State, 1, 1, 0);
    if (ret != LUA_OK)
    {
        const char* err = lua_tostring(m_State, -1);
        LUA_LOG(WARN, L"BuildPrompt: lua_pcall failed: %S", err ? err : "error");
        lua_pop(m_State, 1);
        outPrompt = context;
        return ElleResult::ERR_LUA_EXEC;
    }

    const char* result = lua_tostring(m_State, -1);
    if (result)
        outPrompt = std::wstring(result, result + strlen(result));
    else
        outPrompt = context;

    lua_pop(m_State, 1);
    return ElleResult::OK;
}

// =============================================================================
// Shutdown
// =============================================================================
void ElleLuaHost::Shutdown()
{
    if (m_HotReloadThread)
    {
        WaitForSingleObject(m_HotReloadThread, 5000);
        ELLE_SAFE_CLOSE_HANDLE(m_HotReloadThread);
    }

    std::lock_guard<std::mutex> lock(m_StateMutex);
    if (m_State)
    {
        lua_close(m_State);
        m_State = nullptr;
    }
    m_Initialized = false;
    LUA_LOG(INFO, L"Lua host shut down. ReloadCount=%llu", m_ReloadCount.load());
}

// =============================================================================
// C++ bindings exposed to Lua
// =============================================================================

// elle.log(level, message)
// level: 0=TRACE 1=DEBUG 2=INFO 3=WARN 4=ERROR 5=FATAL
int ElleLuaHost::LuaLog(lua_State* L)
{
    int level   = (int)luaL_checkinteger(L, 1);
    const char* msg = luaL_checkstring(L, 2);

    ElleLogLevel logLevel = static_cast<ElleLogLevel>(max(0, min(5, level)));
    std::wstring wMsg(msg, msg + strlen(msg));

    ElleLogger::Get().Write(logLevel, L"Lua", __FILEW__, __LINE__, __FUNCTIONW__, L"%s", wMsg.c_str());
    return 0;
}

// elle.query(db_index, sql) -> table of row-tables
// db_index: 0=CORE 1=MEMORY 2=KNOWLEDGE 3=SYSTEM 4=HEART
int ElleLuaHost::LuaQuery(lua_State* L)
{
    int dbIdx       = (int)luaL_checkinteger(L, 1);
    const char* sql = luaL_checkstring(L, 2);

    if (dbIdx < 0 || dbIdx >= static_cast<int>(ElleDB::COUNT))
    {
        lua_pushnil(L);
        return 1;
    }

    ElleDB db = static_cast<ElleDB>(dbIdx);
    ElleSQLScope conn(db);
    if (!conn.Valid())
    {
        lua_pushnil(L);
        return 1;
    }

    std::wstring wSql(sql, sql + strlen(sql));

    // Build a Lua table of row-tables
    lua_newtable(L);   // outer table
    int rowIndex = 1;

    conn->Query(wSql,
        [&](const std::vector<std::wstring>& row)
        {
            lua_newtable(L);   // inner table for this row
            for (int col = 0; col < (int)row.size(); col++)
            {
                std::string val(row[col].begin(), row[col].end());
                lua_pushstring(L, val.c_str());
                lua_rawseti(L, -2, col + 1);
            }
            lua_rawseti(L, -2, rowIndex++);
        }
    );

    return 1;
}

// elle.execute(db_index, sql) -> bool
int ElleLuaHost::LuaExecute(lua_State* L)
{
    int dbIdx       = (int)luaL_checkinteger(L, 1);
    const char* sql = luaL_checkstring(L, 2);

    if (dbIdx < 0 || dbIdx >= static_cast<int>(ElleDB::COUNT))
    {
        lua_pushboolean(L, 0);
        return 1;
    }

    ElleDB db = static_cast<ElleDB>(dbIdx);
    ElleSQLScope conn(db);
    if (!conn.Valid())
    {
        lua_pushboolean(L, 0);
        return 1;
    }

    std::wstring wSql(sql, sql + strlen(sql));
    ElleResult r = conn->Execute(wSql);
    lua_pushboolean(L, r == ElleResult::OK ? 1 : 0);
    return 1;
}

// elle.pushIntent(typeID, data, priority) -> bool
int ElleLuaHost::LuaPushIntent(lua_State* L)
{
    int typeID          = (int)luaL_checkinteger(L, 1);
    const char* data    = luaL_checkstring(L, 2);
    int priority        = (int)luaL_optinteger(L, 3, 5);

    ElleSQLScope coreConn(ElleDB::CORE);
    if (!coreConn.Valid())
    {
        lua_pushboolean(L, 0);
        return 1;
    }

    std::wstring wData(data, data + strlen(data));

    ElleResult r = coreConn->ExecuteParams(
        L"INSERT INTO ElleCore.dbo.IntentQueue (TypeID, StatusID, IntentData, TrustRequired, Priority, CreatedAt) "
        L"VALUES (?, 0, ?, 0, ?, GETDATE())",
        { std::to_wstring(typeID), wData, std::to_wstring(priority) }
    );

    lua_pushboolean(L, r == ElleResult::OK ? 1 : 0);
    return 1;
}

// elle.getEmotion(dimensionID) -> number
int ElleLuaHost::LuaGetEmotion(lua_State* L)
{
    int dimID = (int)luaL_checkinteger(L, 1);

    ElleSQLScope memConn(ElleDB::MEMORY);
    if (!memConn.Valid())
    {
        lua_pushnumber(L, 0.0);
        return 1;
    }

    double value = 0.0;
    memConn->QueryParams(
        L"SELECT Value FROM ElleMemory.dbo.EmotionalState WHERE DimensionID = ?",
        { std::to_wstring(dimID) },
        [&](const std::vector<std::wstring>& row)
        {
            if (!row.empty()) value = _wtof(row[0].c_str());
        }
    );

    lua_pushnumber(L, value);
    return 1;
}

// elle.getTrust() -> number
int ElleLuaHost::LuaGetTrust(lua_State* L)
{
    ElleSQLScope coreConn(ElleDB::CORE);
    if (!coreConn.Valid())
    {
        lua_pushinteger(L, 0);
        return 1;
    }

    int score = 0;
    coreConn->Query(
        L"SELECT TOP 1 TrustScore FROM ElleCore.dbo.TrustState ORDER BY UpdatedAt DESC",
        [&](const std::vector<std::wstring>& row)
        {
            if (!row.empty()) score = _wtoi(row[0].c_str());
        }
    );

    lua_pushinteger(L, score);
    return 1;
}
