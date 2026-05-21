# Spec: Script VM (Lua / sol3)  (M4)

> Clean-room — implement from THIS FILE + public refs ONLY. No RBDOOM source.
> sol3 + LuaJIT are MIT — clean. Zero 14900K dependency.

- **Implements interface:** `IScriptVM` (`engine/script/IScriptVM.h`)
- **Status:** SPEC (ready)
- **Library:** sol3 (binding) + LuaJIT (runtime — see Q5; default LuaJIT).

## 1. Purpose
Embed Lua so game logic (entity behavior, weapons, AI state machines, level scripts, cutscene sequencing) is authored in Lua with hot-reload, while the engine stays C++. The engine exposes a curated API to Lua; Lua never touches raw engine internals.

## 2. Interface contract
```cpp
// engine/script/IScriptVM.h — clean; sol3/lua hidden in .cpp
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace x3::script {

class IScriptVM {
public:
    virtual ~IScriptVM() = default;

    virtual bool init() = 0;
    virtual void shutdown() = 0;

    // Run a script file (via IAssetSource virtual path) or a string.
    virtual bool doFile(std::string_view virtualPath) = 0;
    virtual bool doString(std::string_view code) = 0;

    // Bind a C++ function callable from Lua as a global (or under a table name).
    // Keep the bound surface small + curated; document each in LUA_API.md.
    virtual void bind(std::string_view luaName, std::function<void()> fn) = 0;

    // Call a global Lua function by name (no args/return for v1; extend later).
    virtual bool call(std::string_view luaFn) = 0;

    // Per-frame + fixed-step hooks Lua can register via the event bridge.
    virtual void tick(float dt) = 0;          // calls registered OnTick handlers
    virtual void hotReloadChanged() = 0;      // re-run changed scripts (dev)
};

IScriptVM* createScriptVM(class asset::IAssetSource* assets);

} // namespace x3::script
```

## 3. Behavior
- Runtime: LuaJIT (fast; Lua 5.1 syntax) via sol3. (If Q5 picks Lua 5.4, swap the VM; sol3 supports both.)
- Bound API surface (curated, grows per game need): `log(msg)`, `cvar.get/set`, `spawn(type, x,y,z)`, `entity.setPos/getPos`, `phys.raycast`, `on(event, fn)` (events: OnTick, OnCollision, OnTrigger, OnDamage, OnSpawn, OnDeath).
- Hot-reload: file-watch Lua scripts; on change, re-execute + re-bind handlers without engine restart.
- Sandboxing: every Lua call wrapped in `pcall`; a script error logs (script + line + stack) and does NOT crash the engine.

## 4. Edge cases & error handling
- Syntax error in a script → log with file:line, skip that script, engine continues.
- Runtime error in a handler → log stack, unregister or keep per policy (default: log + keep, so one bad frame doesn't kill the handler).
- Calling an unbound global → log "unknown function", no crash.
- Infinite loop in Lua → out of scope to fully guard; document the risk (optional instruction-count hook later).
- Hot-reload of a script mid-frame → defer reload to end of frame.

## 5. Performance targets
- LuaJIT call overhead negligible for typical per-entity logic (<0.1 ms for 100 entities' OnTick).
- No per-call heap allocation in the hot path where avoidable (reuse sol::function handles).
- Hot-reload check throttled (file-watch event-driven, not per-frame stat).

## 6. Acceptance tests
1. **T1 — Hello:** `doString("log('hello from lua')")` prints via engine log.
2. **T2 — Bind + call:** bind a C++ `quit()`; a Lua script calling `quit()` invokes it.
3. **T3 — cvar bridge:** Lua `cvar.set('g_speed', 12)` then C++ reads 12; `cvar.get('g_speed')` returns 12 in Lua.
4. **T4 — Spawn:** Lua `spawn('cube', 0,1,0)` creates a visible entity (with D1 rendering; for now: spawn callback fires with correct args).
5. **T5 — Event:** Lua registers `on('OnTick', fn)`; `tick(dt)` calls it each frame with dt.
6. **T6 — Error survives:** a script with a runtime error → logged with line, engine still runs the next frame.
7. **T7 — Hot-reload:** edit a script's printed string, trigger reload, see new output without restart.
8. **T8 — API doc:** `docs/LUA_API.md` auto-lists every bound function (from the binding table).

## 7. Public references
- sol3 documentation (tutorial, usage, performance).
- LuaJIT documentation; Lua 5.1/5.4 reference manual.
- "Embedding Lua" general patterns (public articles).

## 8. Suggested permissive libraries
- **sol3** (MIT) — C++↔Lua binding.
- **LuaJIT** (MIT) — runtime. (Or **Lua 5.4**, MIT, if Q5 chooses it.)

## 9. Notes for the clean-room implementer
- Keep sol3 + lua headers in the .cpp. The interface is plain `std::function` + strings for v1; a richer typed-args API can come later.
- The bound API should call ENGINE INTERFACES (IRenderDevice/IPhysicsWorld/entity system), never raw subsystems — keeps Lua decoupled.
- Generate `docs/LUA_API.md` from the binding registration table so the doc never drifts from the code.
- Game logic lives in the `.x3pak` (Lua scripts shipped inside the pak). The VM loads scripts via `IAssetSource` so dev loose-files and shipped paks both work.
