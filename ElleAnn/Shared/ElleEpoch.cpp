// =============================================================================
// ElleEpoch.cpp — Implementation
// =============================================================================

#include "ElleEpoch.h"
#include "ElleSQLConn.h"
#include <objbase.h>    // CoCreateGuid
#include <combaseapi.h>

#pragma comment(lib, "Ole32.lib")

ElleEpochManager& ElleEpochManager::Get()
{
    static ElleEpochManager instance;
    return instance;
}

ElleEpochManager::ElleEpochManager()
    : m_Initialized(false)
{}

// =============================================================================
// GenerateGUID — produces a formatted UUID string without braces
// =============================================================================
std::wstring ElleEpochManager::GenerateGUID()
{
    GUID guid = {};
    CoCreateGuid(&guid);

    wchar_t buf[40] = {};
    StringFromGUID2(guid, buf, _countof(buf));

    // StringFromGUID2 returns {xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}
    // Strip the braces
    std::wstring result(buf + 1, 36);
    return result;
}

// =============================================================================
// Init
// =============================================================================
ElleResult ElleEpochManager::Init(bool isFirstService)
{
    EPOCH_LOG(INFO, L"EpochManager::Init isFirstService=%d", isFirstService ? 1 : 0);

    ElleSQLScope sysConn(ElleDB::SYSTEM);
    if (!sysConn.Valid())
    {
        EPOCH_LOG(FATAL, L"EpochManager::Init — cannot connect to ElleSystem");
        return ElleResult::ERR_SQL_CONNECT;
    }

    if (isFirstService)
    {
        // Generate a fresh epoch — this is a new platform run.
        // Overwrite whatever was there from a prior crashed or stopped run.
        m_Epoch = GenerateGUID();

        ElleResult r = sysConn->ExecuteParams(
            L"IF EXISTS (SELECT 1 FROM ElleSystem.dbo.RuntimeEpoch) "
            L"    UPDATE ElleSystem.dbo.RuntimeEpoch SET EpochID = ?, StartedAt = GETDATE(), ServiceCount = 1 "
            L"ELSE "
            L"    INSERT INTO ElleSystem.dbo.RuntimeEpoch (EpochID, StartedAt, ServiceCount) VALUES (?, GETDATE(), 1)",
            { m_Epoch, m_Epoch }
        );

        if (r != ElleResult::OK)
        {
            EPOCH_LOG(FATAL, L"EpochManager::Init — failed to write new epoch: %s",
                sysConn->LastError.c_str());
            return r;
        }

        EPOCH_LOG(INFO, L"EpochManager: New epoch generated: %s", m_Epoch.c_str());
    }
    else
    {
        // Read the epoch that QueueWorker wrote.
        // If QueueWorker hasn't started yet, retry a few times.
        int retries = 0;
        bool found = false;

        while (retries < 10 && !found)
        {
            sysConn->Query(
                L"SELECT EpochID FROM ElleSystem.dbo.RuntimeEpoch",
                [&](const std::vector<std::wstring>& row)
                {
                    if (!row.empty() && !row[0].empty())
                    {
                        m_Epoch = row[0];
                        found = true;
                    }
                }
            );

            if (!found)
            {
                EPOCH_LOG(WARN, L"EpochManager: No epoch in DB yet — waiting for QueueWorker (attempt %d/10)",
                    retries + 1);
                Sleep(500);
                retries++;
            }
        }

        if (!found)
        {
            // QueueWorker never wrote an epoch — generate a fallback.
            // This should not happen in normal operation but must not crash.
            m_Epoch = GenerateGUID();
            EPOCH_LOG(WARN, L"EpochManager: QueueWorker epoch not found — using fallback: %s",
                m_Epoch.c_str());
        }
        else
        {
            // Increment service count so we know how many services joined
            sysConn->Execute(
                L"UPDATE ElleSystem.dbo.RuntimeEpoch SET ServiceCount = ServiceCount + 1");
            EPOCH_LOG(INFO, L"EpochManager: Adopted epoch: %s", m_Epoch.c_str());
        }
    }

    m_Initialized = true;
    return ElleResult::OK;
}

// =============================================================================
// ReclaimStranded — reset rows from prior epochs back to PENDING
// =============================================================================
ElleResult ElleEpochManager::ReclaimStranded(int& outIntentsReclaimed, int& outActionsReclaimed)
{
    outIntentsReclaimed = 0;
    outActionsReclaimed = 0;

    if (!m_Initialized || m_Epoch.empty())
    {
        EPOCH_LOG(WARN, L"ReclaimStranded called before Init — skipping");
        return ElleResult::ERR_SERVICE_NOT_READY;
    }

    EPOCH_LOG(INFO, L"ReclaimStranded: Epoch=%s — scanning for stranded rows", m_Epoch.c_str());

    ElleSQLScope coreConn(ElleDB::CORE);
    if (!coreConn.Valid())
        return ElleResult::ERR_SQL_CONNECT;

    // Reclaim stranded intents: PROCESSING rows whose EpochID is not the current epoch.
    // These were claimed by a prior run that died without completing them.
    // Reset to PENDING so the current run picks them up.
    wchar_t batchIntents[512] = {};
    _snwprintf_s(batchIntents, _countof(batchIntents), _TRUNCATE,
        L"DECLARE @r INT; "
        L"UPDATE ElleCore.dbo.IntentQueue "
        L"SET StatusID = 0, ClaimedByThread = NULL, EpochID = NULL, UpdatedAt = GETDATE() "
        L"WHERE StatusID = 1 AND (EpochID IS NULL OR EpochID != '%s'); "
        L"SET @r = @@ROWCOUNT; SELECT @r AS RowsAffected;",
        m_Epoch.c_str()
    );

    int rowCount = 0;
    coreConn->QueryRows(batchIntents,
        [&](const std::vector<std::wstring>& row)
        { if (!row.empty()) outIntentsReclaimed = _wtoi(row[0].c_str()); },
        rowCount
    );

    if (outIntentsReclaimed > 0)
        EPOCH_LOG(WARN, L"ReclaimStranded: Reset %d stranded PROCESSING intents to PENDING",
            outIntentsReclaimed);

    // Reclaim stranded actions: LOCKED rows from a prior epoch.
    wchar_t batchActions[512] = {};
    _snwprintf_s(batchActions, _countof(batchActions), _TRUNCATE,
        L"DECLARE @r INT; "
        L"UPDATE ElleCore.dbo.ActionQueue "
        L"SET StatusID = 0, ClaimedByThread = NULL, EpochID = NULL "
        L"WHERE StatusID = 1 AND (EpochID IS NULL OR EpochID != '%s'); "
        L"SET @r = @@ROWCOUNT; SELECT @r AS RowsAffected;",
        m_Epoch.c_str()
    );

    coreConn->QueryRows(batchActions,
        [&](const std::vector<std::wstring>& row)
        { if (!row.empty()) outActionsReclaimed = _wtoi(row[0].c_str()); },
        rowCount
    );

    if (outActionsReclaimed > 0)
        EPOCH_LOG(WARN, L"ReclaimStranded: Reset %d stranded LOCKED actions to PENDING",
            outActionsReclaimed);

    EPOCH_LOG(INFO, L"ReclaimStranded complete. IntentsReclaimed=%d ActionsReclaimed=%d",
        outIntentsReclaimed, outActionsReclaimed);

    return ElleResult::OK;
}
