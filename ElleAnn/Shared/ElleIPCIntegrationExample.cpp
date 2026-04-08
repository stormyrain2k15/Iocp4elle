// =============================================================================
// ElleIPCIntegrationExample.cpp
//
// This file demonstrates how to integrate the IOCP-based IPC layer into
// ElleAnn services for real-time inter-service communication.
//
// EXAMPLE 1: Adding IPC Server to Elle.Service.Emotional
// EXAMPLE 2: Querying Emotional state from Elle.Service.Cognitive
// EXAMPLE 3: Memory service querying Identity service
// =============================================================================

/*
// =============================================================================
// EXAMPLE 1: Integrate IPC Server into Elle.Service.Emotional
// =============================================================================

File: /app/ElleAnn/Services/Elle.Service.Emotional/Service.cpp

Add to the class:
--------------------
class ElleEmotionalService : public ElleServiceBase
{
private:
    ElleEmotionalEngine m_Engine;
    ElleIPCServer       m_IPCServer;     // <-- ADD THIS
};


Modify OnStart():
--------------------
ElleResult OnStart() override
{
    LOG(INFO, L"OnStart: Initializing Emotional service");

    ElleResult r = InitSharedInfrastructure();
    if (r != ElleResult::OK)
        return r;

    ElleEpochManager::Get().Init(false);
    RegisterWorker();

    // Start emotional engine
    r = m_Engine.Start(StopEvent());
    if (r != ElleResult::OK)
    {
        LOG(FATAL, L"EmotionalEngine::Start() failed: %s", ElleResultStr(r));
        return r;
    }

    // Initialize IPC server
    r = m_IPCServer.Init(ElleIPCServiceNames::EMOTIONAL);
    if (r != ElleResult::OK)
    {
        LOG(ERROR, L"IPCServer::Init() failed: %s", ElleResultStr(r));
        return r;
    }

    // Register IPC handlers
    RegisterIPCHandlers();

    // Start IPC server
    r = m_IPCServer.Start();
    if (r != ElleResult::OK)
    {
        LOG(ERROR, L"IPCServer::Start() failed: %s", ElleResultStr(r));
        return r;
    }

    LOG(INFO, L"Emotional service running with IPC enabled");
    return ElleResult::OK;
}


Add handler registration method:
--------------------
void RegisterIPCHandlers()
{
    // Handler: GetEmotionalState
    // Returns current 102-dimensional emotional state
    m_IPCServer.RegisterHandler(L"GetEmotionalState",
        [this](const ElleIPCRequest& req, std::wstring& outResp) -> ElleResult
        {
            LOG(DEBUG, L"IPC: GetEmotionalState request received");

            // Get current emotional snapshot
            std::array<ElleEmotionalDimension, 102> snapshot;
            m_Engine.GetSnapshot(snapshot);

            // Build JSON response with first 20 named dimensions
            std::wostringstream json;
            json << L"{\"dimensions\":[";
            
            for (int i = 0; i < 20; i++)
            {
                if (i > 0) json << L",";
                json << L"{\"id\":" << i
                     << L",\"name\":\"" << snapshot[i].Name << L"\""
                     << L",\"value\":" << snapshot[i].Value
                     << L"}";
            }
            
            json << L"]}";
            outResp = json.str();

            LOG(DEBUG, L"IPC: GetEmotionalState response sent (%zu bytes)", outResp.size());
            return ElleResult::OK;
        });

    // Handler: GetDimensionValue
    // Returns value for a specific dimension ID
    m_IPCServer.RegisterHandler(L"GetDimensionValue",
        [this](const ElleIPCRequest& req, std::wstring& outResp) -> ElleResult
        {
            // Parse dimension ID from params: {"dimensionID":0}
            int dimID = -1;
            size_t pos = req.Parameters.find(L"\"dimensionID\"");
            if (pos != std::wstring::npos)
            {
                size_t colonPos = req.Parameters.find(L':', pos);
                if (colonPos != std::wstring::npos)
                {
                    dimID = _wtoi(req.Parameters.c_str() + colonPos + 1);
                }
            }

            if (dimID < 0 || dimID >= 102)
            {
                outResp = L"{\"error\":\"invalid_dimension_id\"}";
                return ElleResult::ERR_INVALID_PARAM;
            }

            double value = m_Engine.GetDimensionValue(dimID);

            wchar_t jsonBuf[256] = {};
            _snwprintf_s(jsonBuf, _countof(jsonBuf), _TRUNCATE,
                L"{\"dimensionID\":%d,\"value\":%.4f}", dimID, value);
            outResp = jsonBuf;

            return ElleResult::OK;
        });

    LOG(INFO, L"IPC handlers registered: GetEmotionalState, GetDimensionValue");
}


Modify OnStop():
--------------------
void OnStop() override
{
    LOG(INFO, L"OnStop: Stopping Emotional service");
    
    m_IPCServer.Stop();     // <-- ADD THIS
    m_Engine.Stop();
    
    UnregisterWorker();
    ShutdownSharedInfrastructure();
}


// =============================================================================
// EXAMPLE 2: Query Emotional Service from Cognitive Service
// =============================================================================

File: /app/ElleAnn/Services/Elle.Service.Cognitive/Service.cpp

Add to the class:
--------------------
class ElleCognitiveService : public ElleServiceBase
{
private:
    ElleCognitiveEngine m_Engine;
    ElleIPCClient       m_IPCClient;     // <-- ADD THIS
};


Modify OnStart():
--------------------
ElleResult OnStart() override
{
    LOG(INFO, L"OnStart: Initializing Cognitive service");
    ElleResult r = InitSharedInfrastructure();
    if (r != ElleResult::OK) return r;

    ElleEpochManager::Get().Init(false);
    RegisterWorker();

    // Initialize IPC client
    r = m_IPCClient.Init();
    if (r != ElleResult::OK)
    {
        LOG(WARN, L"IPCClient::Init() failed: %s (IPC queries disabled)", ElleResultStr(r));
        // Non-fatal - continue without IPC support
    }

    r = m_Engine.Start(StopEvent());
    if (r != ElleResult::OK)
    {
        LOG(FATAL, L"CognitiveEngine::Start() failed: %s", ElleResultStr(r));
        return r;
    }

    LOG(INFO, L"Cognitive service running");
    return ElleResult::OK;
}


Usage in ProcessMessage (existing method):
--------------------
void ProcessMessage(int64_t messageID, const std::wstring& content, ElleSQLConnection* conn)
{
    LOG(INFO, L"ProcessMessage: MessageID=%lld Content='%s'",
        messageID, content.substr(0, 60).c_str());

    // ... existing NLP processing ...

    // NEW: Query current emotional state for context-aware response generation
    std::wstring emotionalContext;
    ElleResult r = m_IPCClient.Request(
        ElleIPCServiceNames::EMOTIONAL,
        L"GetEmotionalState",
        L"{}",  // No parameters needed
        emotionalContext,
        2000    // 2 second timeout
    );

    if (r == ElleResult::OK)
    {
        LOG(DEBUG, L"Retrieved emotional context: %s", emotionalContext.c_str());
        
        // Parse joy and sadness values from response
        // {"dimensions":[{"id":0,"name":"Joy","value":0.35},{"id":1,"name":"Sadness","value":-0.12},...]}
        
        double joyLevel = 0.0;
        size_t joyPos = emotionalContext.find(L"\"Joy\"");
        if (joyPos != std::wstring::npos)
        {
            size_t valuePos = emotionalContext.find(L"\"value\":", joyPos);
            if (valuePos != std::wstring::npos)
            {
                joyLevel = _wtof(emotionalContext.c_str() + valuePos + 8);
            }
        }

        LOG(INFO, L"Current joy level: %.3f (will adjust response tone)", joyLevel);
        
        // Use joyLevel to adjust response generation...
        // If joy is high, generate more enthusiastic responses
        // If joy is low, generate more empathetic responses
    }
    else
    {
        LOG(WARN, L"Failed to query emotional state via IPC: %s (proceeding without context)",
            ElleResultStr(r));
        // Fall back to context-free processing
    }

    // Continue with existing message processing...
    // Generate SEND_MESSAGE intent with emotional context considered
}


// =============================================================================
// EXAMPLE 3: Query specific emotional dimension
// =============================================================================

// In Speech Service, before generating speech output:
void GenerateSpeechWithEmotionalContext(const std::wstring& text)
{
    // Query specific dimension (e.g., Joy = 0)
    std::wstring params = L"{\"dimensionID\":0}";
    std::wstring response;
    
    ElleResult r = m_IPCClient.Request(
        ElleIPCServiceNames::EMOTIONAL,
        L"GetDimensionValue",
        params,
        response,
        1000  // 1 second timeout
    );

    if (r == ElleResult::OK)
    {
        // Parse: {"dimensionID":0,"value":0.3500}
        double joyValue = 0.0;
        size_t pos = response.find(L"\"value\":");
        if (pos != std::wstring::npos)
        {
            joyValue = _wtof(response.c_str() + pos + 8);
        }

        LOG(INFO, L"Joy value: %.4f - adjusting speech prosody", joyValue);
        
        // Adjust speech synthesis parameters based on joy level:
        // - Higher joy = faster tempo, higher pitch
        // - Lower joy = slower tempo, lower pitch
        float pitchMultiplier = 1.0f + (float)(joyValue * 0.2);  // ±20% pitch variation
        float tempoMultiplier = 1.0f + (float)(joyValue * 0.15); // ±15% tempo variation
        
        // Pass to TTS engine...
    }
}


// =============================================================================
// EXAMPLE 4: Memory Service IPC Server
// =============================================================================

// In Elle.Service.Memory, expose recent memories:
void RegisterIPCHandlers()
{
    m_IPCServer.RegisterHandler(L"GetRecentMemories",
        [this](const ElleIPCRequest& req, std::wstring& outResp) -> ElleResult
        {
            // Parse parameters: {"count":10,"topic":"conversation"}
            int count = 10;  // default
            std::wstring topic;
            
            // Parse count
            size_t countPos = req.Parameters.find(L"\"count\"");
            if (countPos != std::wstring::npos)
            {
                size_t colonPos = req.Parameters.find(L':', countPos);
                if (colonPos != std::wstring::npos)
                    count = _wtoi(req.Parameters.c_str() + colonPos + 1);
            }

            // Query ElleMemory database
            ElleSQLScope memConn(ElleDB::MEMORY);
            if (!memConn.Valid())
            {
                outResp = L"{\"error\":\"database_unavailable\"}";
                return ElleResult::ERR_SQL_CONNECT;
            }

            std::wostringstream json;
            json << L"{\"memories\":[";
            
            int memCount = 0;
            memConn->QueryParams(
                L"SELECT TOP (@count) MemoryID, Content, Timestamp "
                L"FROM ElleMemory.dbo.Memories "
                L"ORDER BY Timestamp DESC",
                { { L"@count", std::to_wstring(count) } },
                [&](const std::vector<std::wstring>& row)
                {
                    if (row.size() >= 3)
                    {
                        if (memCount > 0) json << L",";
                        json << L"{\"id\":" << row[0]
                             << L",\"content\":\"" << row[1] << L"\""
                             << L",\"timestamp\":\"" << row[2] << L"\"}";
                        memCount++;
                    }
                }
            );

            json << L"]}";
            outResp = json.str();

            LOG(INFO, L"IPC: GetRecentMemories returned %d memories", memCount);
            return ElleResult::OK;
        });
}


// =============================================================================
// PERFORMANCE CHARACTERISTICS
// =============================================================================

Typical IPC request/response latency:
  - Same machine, services running: 0.5-2ms
  - Includes serialization, named pipe I/O, deserialization
  - 500x faster than SQL polling (typical 1000ms query)

Use Cases for IOCP IPC (real-time queries):
  ✓ Get current emotional state
  ✓ Retrieve recent memories
  ✓ Check trust score for a user
  ✓ Query active attention threads
  ✓ Get drive state (boredom, curiosity)

Use Cases for SQL IntentQueue (async, persistent):
  ✓ Generate LLM response (long-running)
  ✓ Execute hardware actions (vibrate, flash)
  ✓ Complex multi-step workflows
  ✓ Work that must survive service restarts


// =============================================================================
// TROUBLESHOOTING
// =============================================================================

Q: "IPC request times out"
A: Check that the target service is running and its IPC server started:
   - Check Windows Services: Elle Emotional Engine should be "Running"
   - Check logs: Search for "IPC server running" in service logs
   - Verify pipe exists: dir \\.\pipe\ | findstr ElleService

Q: "Cannot connect to service"
A: Named pipe not created yet:
   - Ensure target service OnStart() calls m_IPCServer.Init() and Start()
   - Check for errors in service startup logs
   - Verify no firewall blocking named pipes

Q: "Response is empty or malformed"
A: Handler not returning proper JSON:
   - Check handler implementation returns valid JSON
   - Enable TRACE logging to see raw responses
   - Verify wstring encoding (UTF-16 LE)

Q: "High latency on IPC calls"
A: IOCP should be <2ms for local pipes:
   - Check if services are on same machine
   - Review IOCP worker thread count (default 4)
   - Check for database queries inside IPC handlers (move to background)

*/
