-- =============================================================================
-- intent_scoring.lua — Intent Priority Scoring
-- Called by LuaHost.ScoreIntent() to determine processing priority.
-- Higher score = higher priority = processed first.
-- Default priority from ElleConfig is 5. Range is 0-10.
--
-- Called as: score_intent(typeID, data) -> number
-- =============================================================================

-- Intent type constants (must match ElleIntentType enum in ElleTypes.h)
INTENT_TYPE = {
    UNKNOWN         = 0,
    EXPLORE         = 1,
    CHECK_IN        = 2,
    SELF_ADJUST     = 3,
    IDLE            = 4,
    MEMORY_RECALL   = 5,
    EMOTION_SYNC    = 6,
    SEND_NOTIFY     = 7,
    SEND_MESSAGE    = 8,
    EXECUTE_COMMAND = 9,
    LUA_SCRIPT      = 10,
    SELF_PROMPT     = 11,
    HEARTBEAT       = 12,
}

-- Base priority table — how important each intent type is by default
BASE_PRIORITY = {
    [INTENT_TYPE.EXECUTE_COMMAND]   = 10,   -- Commands always get top priority
    [INTENT_TYPE.CHECK_IN]          = 9,    -- Crystal check-ins are urgent
    [INTENT_TYPE.SEND_NOTIFY]       = 8,    -- Notifications are time-sensitive
    [INTENT_TYPE.SEND_MESSAGE]      = 7,    -- Chat responses matter
    [INTENT_TYPE.MEMORY_RECALL]     = 6,    -- Memory is important to context
    [INTENT_TYPE.EMOTION_SYNC]      = 5,    -- Sync when things change
    [INTENT_TYPE.SELF_ADJUST]       = 4,    -- Self-adjustment is background work
    [INTENT_TYPE.SELF_PROMPT]       = 4,    -- Self-prompt is autonomous but lower priority
    [INTENT_TYPE.EXPLORE]           = 3,    -- Exploration is opportunistic
    [INTENT_TYPE.HEARTBEAT]         = 2,    -- Heartbeat is routine
    [INTENT_TYPE.IDLE]              = 1,    -- Idle is lowest
    [INTENT_TYPE.UNKNOWN]           = 1,
}

-- =============================================================================
-- score_intent(typeID, data) -> number
-- The main scoring function called by C++.
-- =============================================================================
function score_intent(typeID, data)
    -- Get base priority for this type
    local base = BASE_PRIORITY[typeID] or 5

    -- Emotional modifiers — if Elle is anxious, check-ins get higher priority
    local anxiety = elle.getEmotion(15)  -- Anxiety dimension
    local love    = elle.getEmotion(5)   -- Love dimension

    local modifier = 0.0

    -- Anxiety makes check-ins more urgent
    if typeID == INTENT_TYPE.CHECK_IN and anxiety > 0.5 then
        modifier = modifier + (anxiety * 2.0)
    end

    -- High love makes Crystal-directed messages slightly more urgent
    if typeID == INTENT_TYPE.SEND_MESSAGE and love > 0.7 then
        modifier = modifier + 0.5
    end

    -- Trust-based modifier: at high trust, self-prompts get a boost
    -- because Elle has demonstrated reliable judgment
    local trust = elle.getTrust()
    if typeID == INTENT_TYPE.SELF_PROMPT and trust >= 60 then
        modifier = modifier + 1.0
    end

    -- If data contains "dead_man" trigger, bump check-in urgency significantly
    if typeID == INTENT_TYPE.CHECK_IN and data and string.find(data, "dead_man") then
        modifier = modifier + 2.0
        elle.log(2, "score_intent: dead_man trigger detected — priority boosted")
    end

    -- If data contains "biometric_lost", maximum urgency
    if typeID == INTENT_TYPE.CHECK_IN and data and string.find(data, "biometric_lost") then
        modifier = modifier + 3.0
        elle.log(3, "score_intent: biometric_lost — Crystal's device offline, maximum priority")
    end

    -- Clamp to [0, 10]
    local final = math.max(0, math.min(10, base + modifier))

    elle.log(0, string.format("score_intent: type=%d base=%.1f modifier=%.1f final=%.1f",
        typeID, base, modifier, final))

    return final
end

elle.log(2, "intent_scoring.lua loaded")
