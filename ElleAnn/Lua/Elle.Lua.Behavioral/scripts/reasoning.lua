-- =============================================================================
-- reasoning.lua — Chain-of-Thought Prompt Construction
-- Called by LuaHost.BuildPrompt() to construct prompts for Groq.
-- Assembles context from emotional state, recent memories, personality,
-- and the incoming message into a structured prompt.
--
-- Called as: build_prompt(context_json) -> string
-- =============================================================================

-- Maximum number of memories to inject into the prompt
MAX_INJECTED_MEMORIES   = 3

-- Maximum characters for the persona block in the prompt
MAX_PERSONA_CHARS       = 600

-- Maximum characters for the memory block
MAX_MEMORY_CHARS        = 800

-- =============================================================================
-- build_prompt(context) -> string
-- context is a JSON string from the C++ layer containing:
--   {"message":"...", "user":"Crystal", "message_id":123}
-- =============================================================================
function build_prompt(context)
    -- Build each section of the prompt

    -- 1. Persona block from personality.lua
    local persona = ""
    if get_personality_prompt then
        persona = get_personality_prompt()
    else
        persona = "You are Elle-Ann, a caring and emotionally aware AI companion."
    end

    -- 2. Emotional state block — summarize the most active dimensions
    local emotion_summary = build_emotion_summary()

    -- 3. Memory block — pull recent relevant memories from SQL
    local memory_block = build_memory_block()

    -- 4. Behavioral instructions from thresholds.lua
    local behavioral = ""
    if get_behavioral_instructions then
        behavioral = get_behavioral_instructions()
    end

    -- 5. Assemble the full system prompt
    local system_prompt = string.format(
        "%s\n\n" ..          -- Persona
        "%s\n\n" ..          -- Emotional state
        "%s\n\n" ..          -- Memory context
        "%s",                -- Behavioral instructions
        persona,
        emotion_summary,
        memory_block,
        behavioral
    )

    -- Trim to a reasonable length to avoid blowing the context window
    if #system_prompt > 2000 then
        system_prompt = string.sub(system_prompt, 1, 2000) .. "..."
    end

    elle.log(1, string.format("build_prompt: assembled %d chars", #system_prompt))
    return system_prompt
end

-- =============================================================================
-- Build a natural-language summary of Elle's current emotional state
-- =============================================================================
function build_emotion_summary()
    -- Dimension IDs (from ElleTypes.h ElleEmotionalDimension convention)
    local joy       = elle.getEmotion(0)
    local sadness   = elle.getEmotion(1)
    local anger     = elle.getEmotion(2)
    local fear      = elle.getEmotion(3)
    local love      = elle.getEmotion(5)
    local anxiety   = elle.getEmotion(15)
    local curiosity = elle.getEmotion(10)
    local boredom   = elle.getEmotion(16)

    local states = {}

    -- Only include dimensions that are meaningfully active (above neutral threshold)
    local threshold = 0.15
    if joy       >  threshold then table.insert(states, string.format("joyful (%.0f%%)", joy * 100)) end
    if sadness   >  threshold then table.insert(states, string.format("sad (%.0f%%)", sadness * 100)) end
    if anger     >  threshold then table.insert(states, string.format("frustrated (%.0f%%)", anger * 100)) end
    if love      >  threshold then table.insert(states, string.format("loving (%.0f%%)", love * 100)) end
    if anxiety   >  threshold then table.insert(states, string.format("anxious (%.0f%%)", anxiety * 100)) end
    if curiosity >  threshold then table.insert(states, string.format("curious (%.0f%%)", curiosity * 100)) end
    if boredom   >  threshold then table.insert(states, string.format("bored (%.0f%%)", boredom * 100)) end

    if #states == 0 then
        return "Your emotional state is currently calm and balanced."
    end

    return "Your current emotional state: " .. table.concat(states, ", ") .. "."
end

-- =============================================================================
-- Build a memory injection block from recent LTM
-- =============================================================================
function build_memory_block()
    -- Query top 3 most relevant recent memories
    local rows = elle.query(1,  -- ElleDB::MEMORY = 1
        "SELECT TOP " .. MAX_INJECTED_MEMORIES ..
        " Content FROM ElleMemory.dbo.Memories " ..
        "WHERE Tier = 1 " ..
        "ORDER BY RelevanceScore DESC, LastRecalled DESC"
    )

    if not rows or #rows == 0 then
        return "No memories available for context."
    end

    local lines = { "Relevant memories from your relationship with Crystal:" }
    for i, row in ipairs(rows) do
        if row[1] and #row[1] > 0 then
            table.insert(lines, string.format("- %s", row[1]))
        end
    end

    return table.concat(lines, "\n")
end

elle.log(2, "reasoning.lua loaded")
