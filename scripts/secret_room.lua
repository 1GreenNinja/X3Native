-- secret_room.lua — Tim's terminal-code -> trapdoor -> secret-room mechanic, as
-- DATA. The cell HoloTerminal fires "terminal_code" with the typed value on
-- submit; on the override code we open the cell floor hatch and update the
-- objective line. No C++ door/objective logic is required for this beat — it
-- lives entirely in this pak script via the host bindings:
--   x3.openTrapdoor()      -> the secret-room floor hatch
--   x3.setObjective(text)  -> the GTA-style under-minimap objective line
-- (See app/main.cpp: scripts->fire("terminal_code", {code=...}) at the terminal
--  submit, and the registerFunction() bindings wired to Level1Game.)

-- The override code that opens the cell trapdoor (matches app/secret_room.h
-- kSecretRoomCode = "1278"). Kept here so the SECRET lives in the pak, not C++.
local SECRET_CODE = "1278"

local opened = false

function onInit()
    x3.log("secret_room.lua ready — waiting for terminal code " .. SECRET_CODE)
end

function onEvent(name, args)
    if name ~= "terminal_code" then return end
    local code = args.code or ""
    if code == SECRET_CODE and not opened then
        opened = true
        x3.log("secret_room.lua: override code accepted -> opening trapdoor")
        x3.openTrapdoor()
        x3.setObjective("A hatch grinds open in the cell floor... drop through")
    elseif code ~= SECRET_CODE then
        x3.log("secret_room.lua: rejected code '" .. code .. "'")
    end
end
