#pragma once
// Script System interface — Lua gameplay scripting for the .x3pak content model.
// Spec: specs/D14-script.spec.md (write me)
//
// WHY THIS EXISTS
// ---------------
// The engine plan locks "Lua via sol3" as the scripting layer, and the business
// model is ONE engine binary + MANY games shipped as .x3pak data (X3, TTT 1995,
// Pin-Pull-Tomb, ...). Without scripting, every game's logic must be compiled
// C++ in app/ — which means the pak model ships assets but not BEHAVIOR. This
// system closes that gap: a pak can carry scripts/*.lua and a game becomes
// fully data-driven.
//
// DESIGN RULES (matching the rest of the engine)
// ----------------------------------------------
//  * This header is SCRIPTING-API-FREE. No Lua, no sol types leak out — the
//    same quarantine IRenderDevice.h applies to Vulkan. Lua/sol3 live only in
//    LuaScriptSystem.cpp (vcpkg: lua + sol2; sol2 v3.x is the "sol3" library).
//  * PAK-FRIENDLY LOADING: load() takes the script SOURCE as bytes the caller
//    already read (assetSource->read("scripts/door.lua")). The script system
//    never touches the filesystem — dev-dir hot reload falls out of the asset
//    layer's mountDir() override for free.
//  * SANDBOXED: each script runs in its own environment with a whitelisted
//    stdlib (math/string/table + safe base functions). No io, no os, no
//    require, no load/dofile — pak content cannot touch the host machine.
//    Scripts see the engine only through the `x3` API table.
//  * CRASH-PROOF: every entry into Lua is error-protected. A faulty script is
//    marked failed, logged, and SKIPPED from then on; the engine and all other
//    scripts keep running. Content bugs must never take down the runtime.
//  * HOT RELOAD: load() with an already-loaded name replaces that script
//    in-place (same ScriptId), re-running its chunk + onInit. Combined with
//    mountDir() this gives edit-and-see iteration without restarting.
//
// SCRIPT CONTRACT (what a .lua file may define — all optional)
// ------------------------------------------------------------
//   function onInit()              -- after (re)load
//   function onUpdate(dt)          -- every frame, dt in seconds
//   function onEvent(name, args)   -- fired events; args is a string->string table
//
// BUILT-IN `x3` API available to every script:
//   x3.name                        -- this script's name (string)
//   x3.log(msg) / x3.warn(msg) / x3.error(msg)
//   x3.time()                      -- engine-driven clock, seconds (sum of dt)
//   x3.cvar(name) -> string        -- read a console cvar ("" if no console)
//   x3.setcvar(name, value)
//   x3.exec(line)                  -- run a console command line
//   x3.fire(event, argsTable)      -- broadcast to ALL scripts' onEvent
//   x3.after(seconds, fn)          -- one-shot timer, fires during update()
//
// EXTENDING THE API: engine/app systems add richer bindings (entities, audio,
// triggers) inside LuaScriptSystem.cpp via the host-function registry —
// registerFunction() covers the common string-in/nothing-out case from API-free
// code; typed/usertype bindings are added where sol is visible. See the
// "BINDING EXTENSION POINT" block in the .cpp.
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace x3::con { class IConsole; }

namespace x3::script {

using ScriptId = uint32_t;            // stable across hot reloads of the same name
constexpr ScriptId kInvalidScript = 0;

struct ScriptStatus {
    std::string name;
    bool loaded = false;              // chunk compiled + ran at least once
    bool failed = false;              // last entry errored; skipped until reload
    std::string lastError;            // human-readable Lua error, "" if none
};

// args for fire(): ordered key/value string pairs, surfaced to Lua as a table.
using EventArgs = std::vector<std::pair<std::string, std::string>>;

class IScriptSystem {
public:
    virtual ~IScriptSystem() = default;

    // Load a script from in-memory source. If `name` is already loaded this is
    // a HOT RELOAD: same ScriptId, fresh environment, chunk + onInit re-run.
    // Returns kInvalidScript only on allocation-level failure; COMPILE errors
    // still return a valid id with status().failed set (so a broken edit can
    // be fixed and reloaded under the same id).
    virtual ScriptId load(std::string_view name, std::string_view source) = 0;

    virtual bool unload(ScriptId id) = 0;
    virtual ScriptStatus status(ScriptId id) const = 0;
    virtual std::vector<ScriptId> loadedScripts() const = 0;

    // Frame tick: advances x3.time(), fires due x3.after() timers, then calls
    // onUpdate(dt) on every healthy script. O(scripts), allocation-light.
    virtual void update(double dt) = 0;

    // Broadcast an event to every healthy script's onEvent(name, args).
    virtual void fire(std::string_view event, const EventArgs& args = {}) = 0;

    // Host→Lua surface from API-free code: exposes x3.<name>(...) taking any
    // args (stringified). Typed bindings live in the .cpp where sol is visible.
    virtual void registerFunction(std::string_view name,
                                  std::function<void(const std::vector<std::string>&)> fn) = 0;

    // Run a snippet inside a script's environment; returns tostring(result) or
    // "ERROR: ...". Powers the console `lua <script> <code>` command.
    virtual std::string eval(ScriptId id, std::string_view code) = 0;

    // Total Lua heap across all VMs (KB-accurate), for the perf HUD.
    virtual size_t memoryUsedBytes() const = 0;
};

// console may be null (cvar/exec bindings become no-ops that return "").
// If non-null, also registers console commands:
//   script_list                 — print loaded scripts + status
//   script_reload <name>        — re-run a loaded script's last source
//   lua <name> <code...>        — eval in that script's environment
IScriptSystem* createLuaScriptSystem(con::IConsole* console);

// Runs the D14 acceptance tests in-process (load/init/update/events/sandbox/
// error-containment/hot-reload/timers/eval/memory). Returns true if all pass.
bool runScriptSelfTest();

} // namespace x3::script
