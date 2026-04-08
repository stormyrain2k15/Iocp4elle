#pragma once
// =============================================================================
// RouteDispatch.h — API Route Registration for Elle-Ann HTTP Service
//
// Registers all HTTP routes from the old Python FastAPI layer,
// now implemented as real calls into the C++ service infrastructure.
//
// Routes registered:
//   POST /api/ai          — Chat message, routed to Groq via SQL queue
//   GET  /api/memory      — Memory retrieval from ElleMemory
//   POST /api/memory      — Save a new memory
//   GET  /api/emotions    — Current 102-dimension emotional state
//   POST /api/emotions    — Trigger an emotional adjustment
//   GET  /api/server      — Service health and status from ElleSystem
//   GET  /api/hal         — Hardware status
//   POST /api/hal         — Hardware command (vibrate, flash, etc.)
//   GET  /api/admin       — Admin overview
//   POST /api/auth/login  — JWT token issuance
//   GET  /api/users/me    — Current user profile
//   WS   /command         — WebSocket command channel
// =============================================================================

#include "../../Shared/ElleTypes.h"
#include "../../Shared/ElleLogger.h"
#include "HTTPServer.h"
#include "WSServer.h"
#include <memory>

#define ROUTE_LOG(lvl, fmt, ...) ELLE_LOG_##lvl(L"RouteDispatch", fmt, ##__VA_ARGS__)

class ElleRouteDispatch
{
    ELLE_NONCOPYABLE(ElleRouteDispatch)
public:
    explicit ElleRouteDispatch(ElleHTTPServer& httpServer, ElleWSServer& wsServer);

    // Register all routes with the HTTP server.
    // Call once during service startup after HTTPServer::Init().
    void RegisterAll();

private:
    // Route handlers — each one is a complete implementation
    HttpResponse HandleChat(const HttpRequest& req);
    HttpResponse HandleMemoryGet(const HttpRequest& req);
    HttpResponse HandleMemoryPost(const HttpRequest& req);
    HttpResponse HandleEmotionsGet(const HttpRequest& req);
    HttpResponse HandleEmotionsPost(const HttpRequest& req);
    HttpResponse HandleServerStatus(const HttpRequest& req);
    HttpResponse HandleHardwareGet(const HttpRequest& req);
    HttpResponse HandleHardwarePost(const HttpRequest& req);
    HttpResponse HandleAdminGet(const HttpRequest& req);
    HttpResponse HandleAuthLogin(const HttpRequest& req);
    HttpResponse HandleUserMe(const HttpRequest& req);
    HttpResponse HandleWebSocketUpgrade(const HttpRequest& req);
    HttpResponse HandleHealthCheck(const HttpRequest& req);

    // JWT helpers
    std::string IssueToken(const std::wstring& username, const std::wstring& role);
    bool        ValidateToken(const HttpRequest& req, std::wstring& outUsername, std::wstring& outRole);

    ElleHTTPServer& m_HTTP;
    ElleWSServer&   m_WS;
};
