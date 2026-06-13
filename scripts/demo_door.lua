-- Proves the contract end-to-end: state, events, timers, cvars.
opened = false

function onInit()
    x3.log("door script ready, difficulty=" .. x3.cvar("g_difficulty"))
end

function onEvent(name, args)
    if name == "trigger_enter" and args.zone == "door_12" and not opened then
        opened = true
        x3.log("opening door 12 for " .. (args.who or "?"))
        x3.after(3.0, function()
            opened = false
            x3.fire("door_closed", { id = "12" })
        end)
    end
end
