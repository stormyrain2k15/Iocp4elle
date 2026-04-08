#pragma once
// =============================================================================
// LuaHost.h — Lua 5.4 Embedded Host for Elle-Ann Behavioral Engine
//
// Embeds Lua 5.4 into the Elle process. Provides:
//   - Script loading and execution from the scripts directory
//   - Hot-reload: scripts are watched for changes and reloaded without restart
//   - C++ bindings exposed to Lua: SQL access, queue IPC, logging, emotional state
//   - Sandboxing: only the registered bindings are available — no os.execute,
//     no io.popen, no loadfile of arbitrary paths
//   - Thread safety: a mutex protects the lua_State across all calls
//
// Lua scripts in Elle's behavioral layer define:
//   personality.lua     — Elle's personality constants and response biases
//   intent_scoring.lua  — How Elle scores and prioritizes intents
//   reasoning.lua       — Chain-of-thought construction for responses
//   thresholds.lua      — Trust, emotional, and drive thresholds
//
// Scripts call back into C++ through registered functions:
//   elle.log(level, message)
//   elle.query(db, sql)           — returns rows as Lua tables
//   elle.execute(db, sql)
//   elle.pushIntent(typeID, data, priority)
//   elle.getEmotion(dimensionID)  — returns current value [-1.0, 1.0]
//   elle.getTrust()               — returns current trust score
// =============================================================================

#include "../Shared/ElleTypes.h"
#include "../Shared/ElleConfig.h"
#include "../Shared/ElleLogger.h"
#include <string>
#include <mutex>
#include <unordered_map>

// Lua 5.4 headers — must be available in the include path
// Add $(SolutionDir)External\lua54\include to project Additional Include Directories
extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}

#define LUA_LOG(lvl, fmt, ...) ELLE_LOG_##lvl(L"LuaHost", fmt, ##__VA_ARGS__)

// Script file tracking for hot-reload
struct ScriptEntry
{
    std::wstring    Path;
    std::wstring    Name;           // Logical name (e.g., L"personality")
    FILETIME        LastModified;
    bool            Loaded;
};

// =============================================================================
// ElleLuaHost
// =============================================================================
class ElleLuaHost
{
    ELLE_NONCOPYABLE(ElleLuaHost)
public:
    ElleLuaHost();
    ~ElleLuaHost();

    // Initialize Lua state, register C++ bindings, load all scripts
    ElleResult Init();

    // Shutdown and free the Lua state
    void Shutdown();

    // Start hot-reload watcher thread
    ElleResult StartHotReload(HANDLE stopEvent);

    // Call a Lua function by name with no arguments, no return value
    ElleResult CallVoid(const std::string& funcName);

    // Call a Lua function with a string argument, returns a string
    ElleResult CallString(const std::string& funcName, const std::string& arg, std::string& outResult);

    // Call a Lua function with a number argument, returns a number
    ElleResult CallNumber(const std::string& funcName, double arg, double& outResult);

    // Call intent_scoring.lua's score_intent(intentType, intentData) -> priority
    ElleResult ScoreIntent(int intentType, const std::wstring& intentData, double& outScore);

    // Call reasoning.lua's build_prompt(context) -> promptString
    ElleResult BuildPrompt(const std::wstring& context, std::wstring& outPrompt);

    // Force reload of all scripts (called by hot-reload watcher or on demand)
    ElleResult ReloadAll();

    bool IsLoaded() const { return m_State != nullptr && m_Initialized; }

private:
    // Load and execute a single script file into the Lua state
    ElleResult LoadScript(const std::wstring& path, const std::string& chunkName);

    // Register all C++ functions into the Lua "elle" table
    void RegisterBindings();

    // Hot-reload watcher thread
    static DWORD WINAPI HotReloadProc(LPVOID param);
    void HotReloadLoop();

    // Check all script files for modifications and reload changed ones
    void CheckAndReload();

    // ---- C++ functions exposed to Lua ----
    // All follow the lua_CFunction signature: int func(lua_State* L)

    static int LuaLog(lua_State* L);
    static int LuaQuery(lua_State* L);
    static int LuaExecute(lua_State* L);
    static int LuaPushIntent(lua_State* L);
    static int LuaGetEmotion(lua_State* L);
    static int LuaGetTrust(lua_State* L);

    lua_State*                      m_State;
    std::mutex                      m_StateMutex;
    std::vector<ScriptEntry>        m_Scripts;
    bool                            m_Initialized;
    HANDLE                          m_HotReloadThread;
    HANDLE                          m_StopEvent;
    std::atomic<uint64_t>           m_ReloadCount;
};
