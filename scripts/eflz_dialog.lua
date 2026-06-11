-- eflz_dialog.lua — chat-tree narrative beats, as DATA (the secret_room.lua
-- pattern). The x3.chattree/1 runner (app/chat_tree.*) broadcasts every tree
-- {"fire"} effect through x3.fire; this script turns those story events into
-- world state via the SAME host bindings the trigger mechanic uses:
--   x3.setObjective(text)  -> the GTA-style under-minimap objective line
-- Chat trees stay pure data; story side-effects live here, hot-reloadable.
--
-- Events handled (see NPC_CHAT_TREE_FORMAT.md and chat_trees/*.json):
--   dialog_hint {code, source, npc} — an NPC taught the player a door/terminal
--     code (Lena's trust scene teaches 1278, the cell-terminal override that
--     secret_room.lua acts on). Surfaces it as the objective line.
--   companion_joined {npc}          — a first-meeting spine landed follow.
--   sidequest_done {npc, quest}     — a personal quest resolved.
--   rumor {topic, npc}              — world lore breadcrumbs (logged for now).
--   dialog_end {npc, verb}          — a tree exited with a host verb
--     (fight/flee/shop...); the boss/shop hosts subscribe when they land.

function onInit()
    x3.log("eflz_dialog.lua ready — chat-tree story beats live")
end

-- {"lua": "fn"} condition escape hatch: the chat runner evals fns in THIS
-- script's sandbox. Example used by tests/future trees.
function always_true() return true end

function onEvent(name, args)
    if name == "dialog_hint" then
        local code = args.code or "????"
        local src  = args.source or args.npc or "someone"
        x3.log("eflz_dialog: " .. src .. " taught code " .. code)
        x3.setObjective(string.upper(string.sub(src, 1, 1)) .. string.sub(src, 2) ..
                        " mentioned a cell terminal code: " .. code)
    elseif name == "companion_joined" then
        x3.log("eflz_dialog: companion joined — " .. (args.npc or "?"))
    elseif name == "sidequest_done" then
        x3.log("eflz_dialog: sidequest done — " .. (args.npc or "?") ..
               " / " .. (args.quest or "?"))
        x3.setObjective("")   -- clear the override; the beat list resumes
    elseif name == "rumor" then
        x3.log("eflz_dialog: rumor — " .. (args.topic or "?"))
    elseif name == "dialog_end" then
        x3.log("eflz_dialog: dialog_end — " .. (args.npc or "?") ..
               " verb=" .. (args.verb or ""))
    end
end
