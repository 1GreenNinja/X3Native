# D14 — Lua Script System: Fleet Handoff

**From:** Claude (Fable 5, claude.ai chat) — 2026-06-09
**Status:** Code complete, **16/16 acceptance tests passing**, compiled and run
against the real `engine/core/Console.cpp` + `x3_log.cpp` (g++ 13, C++20,
Lua 5.4.6, sol2 v3.3.0). NOT yet built on MSVC/VS2026 — that's step 1 below.

## What this is

The scripting layer the locked stack promised ("Lua via sol3") but that never
got built — zero `sol::` existed in the tree before this. It makes the
one-engine/many-paks business model real: a `.x3pak` can now ship *behavior*
(`scripts/*.lua`), not just assets. X3, TTT 1995, and Pin-Pull-Tomb logic can
become data.

Files:
- `engine/script/IScriptSystem.h` — public interface, scripting-API-free
  (same quarantine discipline as `IRenderDevice.h`)
- `engine/script/LuaScriptSystem.cpp` — sol2 + Lua 5.4 implementation +
  `runScriptSelfTest()` (the D14 acceptance tests, 16 cases)

Capabilities (all tested):
- Per-script **sandboxed environments** — shared VM, isolated globals; `io`,
  `os`, `require`, `load`, `dofile` are absent, pak content can't touch the host
- **Crash-proof**: every Lua entry is protected; a faulty script is quarantined
  with a traceback and skipped; the engine and other scripts continue
- **Hot reload**: `load()` on an existing name keeps the ScriptId, rebuilds the
  sandbox, re-runs `onInit` — pairs with `mountDir()` for edit-and-see
- Hooks: `onInit()`, `onUpdate(dt)`, `onEvent(name, args)`
- `x3` API: `log/warn/error`, `time()`, `cvar/setcvar/exec` (real console
  bridge), `fire(event, args)` script↔script/engine↔script events,
  `after(seconds, fn)` one-shot timers
- Console commands: `script_list`, `script_reload <name>`, `lua <name> <code>`
- `memoryUsedBytes()` for the perf HUD

## Integration steps (one machine, ~30 min)

1. **vcpkg.json** — add to dependencies: `"lua"`, `"sol2"`
2. **engine/CMakeLists.txt** — add `script/LuaScriptSystem.cpp` to the
   `x3engine` sources, then:
   ```cmake
   find_package(Lua REQUIRED)              # or vcpkg's lua target
   find_package(sol2 CONFIG REQUIRED)
   target_link_libraries(x3engine PRIVATE sol2::sol2 ${LUA_LIBRARIES})
   target_include_directories(x3engine PRIVATE ${LUA_INCLUDE_DIR})
   ```
   sol2 must stay PRIVATE — nothing in `engine/script/IScriptSystem.h` needs it.
3. **Self-test wiring** — add `x3::script::runScriptSelfTest()` wherever the
   D5/D6 self-tests run.
4. **main.cpp** — create after the console, pump in the frame loop:
   ```cpp
   auto* scripts = x3::script::createLuaScriptSystem(console);
   // boot: load every scripts/*.lua from the mounted pak/dir
   for (auto& p : assets->list("scripts/")) {
       auto blob = assets->read(p);
       if (blob.ok) scripts->load(p, {(const char*)blob.bytes.data(), blob.bytes.size()});
   }
   // per frame:
   scripts->update(dt);
   ```
5. **Verify on MSVC.** Built clean on g++ 13/Linux; sol2 + MSVC occasionally
   needs `/bigobj` on the TU. If VS2026 complains, add
   `set_source_files_properties(script/LuaScriptSystem.cpp PROPERTIES COMPILE_OPTIONS /bigobj)`.

## Demo script (drop in `scripts/demo_door.lua`)

```lua
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
```
Engine side, the existing trigger system just calls
`scripts->fire("trigger_enter", {{"zone","door_12"},{"who","player"}});`.

## Extending the API (the important part)

Typed bindings (entities, audio, weapons, vehicles) go in
`LuaScriptSystem.cpp::makeSandbox()` / the BINDING EXTENSION POINT block, where
sol is visible — one-liners like
`api.set_function("playSound", [audio](const std::string& cue){ audio->play(cue); });`
The header stays clean. Suggested next bindings, in value order:
1. trigger/objective bridge (door/elevator/secret_room logic → Lua)
2. `x3.spawn` / entity transform get-set against the ECS
3. audio cues, then HUD text

## Deliberately NOT done (honest scope)

- **No entity/ECS bindings yet** — app components are app-defined; that
  binding belongs next to them (see extension point), not in the engine lib.
- **No coroutines** — excluded from the sandbox until a real need shows up.
- **Whitelist copies references**, doesn't freeze the lib tables; a hostile
  pak could mutate `string.format` for *other scripts in the same system*.
  Fine for first-party content; freeze/deep-copy before third-party mods.
- **`fire()` allocates one Lua table per call** — fine at gameplay-event
  rates; don't put it in a per-particle path.
- Timers are a linear vector scan — correct, simple; swap to a min-heap if
  script timer counts ever get large.

## Version note

Per VERSIONING.md discipline: this is a new engine capability → suggest
bumping to 0.5.0 when merged to main.
