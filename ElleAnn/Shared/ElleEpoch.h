#pragma once
// =============================================================================
// ElleEpoch.h — Runtime Epoch Management for Elle-Ann
//
// An epoch is a UNIQUEIDENTIFIER (GUID) generated once per platform startup
// and shared across all services via ElleSystem.RuntimeEpoch.
//
// It solves three problems that timestamps and SPIDs cannot:
//
//   1. STRANDED ROW RECLAIM
//      If a service crashes with rows in PROCESSING or LOCKED state, those rows
//      belong to a dead epoch. On next startup, any row whose EpochID does not
//      match the new current epoch is stranded and gets reset to PENDING. The
//      30-second stale purge is a fallback; epoch-based reclaim is immediate.
//
//   2. SPID REUSE DEFENSE
//      SQL Server recycles SPIDs. A ClaimedByThread value from a dead session
//      can match a live session's SPID. EpochID is unique per run so a SPID
//      from epoch A cannot be confused with the same numeric SPID in epoch B.
//
//   3. MULTI-SERVICE COHERENCE
//      All services call ElleEpochManager::Get().Init() at startup. They all
//      read the same epoch from ElleSystem.RuntimeEpoch. Any service that reads
//      an epoch other than the current one knows it is talking to stale state.
//
// Usage:
//   // In OnStart() before anything else:
//   ElleEpochManager::Get().Init();
//   ElleEpochManager::Get().ReclaimStranded();
//
//   // Read current epoch for SQL claims:
//   const std::wstring& epoch = ElleEpochManager::Get().CurrentEpoch();
// =============================================================================

#include "ElleTypes.h"
#include "ElleLogger.h"
#include <string>
#include <atomic>

#define EPOCH_LOG(lvl, fmt, ...) ELLE_LOG_##lvl(L"EpochManager", fmt, ##__VA_ARGS__)

class ElleEpochManager
{
    ELLE_NONCOPYABLE(ElleEpochManager)
public:
    static ElleEpochManager& Get();

    // Initialize the epoch manager.
    // If no epoch exists in ElleSystem.RuntimeEpoch, generates a new one and
    // writes it. If one already exists (another service started first), reads
    // and adopts it. All services in a platform run share the same epoch GUID.
    //
    // isFirstService: set true only by the QueueWorker (the first service
    // to start). When true, Init() generates a new epoch unconditionally,
    // replacing any prior epoch from a previous run. Other services pass
    // false and adopt whatever QueueWorker wrote.
    ElleResult Init(bool isFirstService = false);

    // Reclaim any IntentQueue rows in PROCESSING state and ActionQueue rows
    // in LOCKED state whose EpochID does not match the current epoch.
    // Those rows belonged to a prior run. Reset them to PENDING so the
    // current run can process them. Call once in OnStart() after Init().
    ElleResult ReclaimStranded(int& outIntentsReclaimed, int& outActionsReclaimed);

    // The current epoch as a string suitable for SQL parameters.
    // Format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx (36 chars, no braces)
    const std::wstring& CurrentEpoch() const { return m_Epoch; }

    bool IsInitialized() const { return m_Initialized; }

    // Generate a new GUID string using CoCreateGuid/StringFromGUID2
    static std::wstring GenerateGUID();

private:
    ElleEpochManager();

    std::wstring        m_Epoch;
    bool                m_Initialized;
};
