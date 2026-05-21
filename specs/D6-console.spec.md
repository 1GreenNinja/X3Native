# Spec: Console + CVar System  (D6)

> Clean-room — implement from THIS FILE + public refs ONLY. No RBDOOM source.
> Standard, trivial fresh rewrite — zero 14900K dependency.

- **Implements interface:** `IConsole` (`engine/core/IConsole.h`)
- **Status:** SPEC (ready)

## 1. Purpose
A Quake/Doom-style developer console: registered commands, config variables (cvars) with get/set + persistence, and a command-line parser. Drives debugging, the quality presets (mirroring the Babylon X3 `quality` command), and live tuning.

## 2. Interface contract
```cpp
// engine/core/IConsole.h — clean, no third-party deps
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace x3::con {

using CommandFn = std::function<void(const std::vector<std::string>& args)>;

class IConsole {
public:
    virtual ~IConsole() = default;

    virtual void registerCommand(std::string_view name, CommandFn fn, std::string_view help = "") = 0;

    // CVars: typed-ish via string storage + typed accessors.
    virtual void  registerCVar(std::string_view name, std::string_view defaultValue, std::string_view help = "") = 0;
    virtual std::string getString(std::string_view name) const = 0;
    virtual float       getFloat (std::string_view name) const = 0;
    virtual int         getInt   (std::string_view name) const = 0;
    virtual void        set      (std::string_view name, std::string_view value) = 0;

    // Execute a console line ("quality low", "g_speed 12", "bind ...").
    virtual void exec(std::string_view line) = 0;

    // Output sink (the UI/log layer subscribes).
    virtual void print(std::string_view msg) = 0;

    // Persistence: write set-cvars to a config file; load at startup.
    virtual void saveConfig(std::string_view path) const = 0;
    virtual bool loadConfig(std::string_view path) = 0;

    // Introspection (for autocomplete / `cvarlist` / `cmdlist`).
    virtual std::vector<std::string> complete(std::string_view prefix) const = 0;
};

IConsole* createConsole();

} // namespace x3::con
```

## 3. Behavior
- Parse: split a line into command + args (whitespace; support quoted args). First token is a command or cvar name.
- If the token is a command → invoke its `CommandFn(args)`. If a cvar → no args prints value, one arg sets it.
- CVars store strings; typed accessors parse on read (cache optional). Persist only cvars that differ from default (or all, per a flag).
- Built-in commands: `help`, `cmdlist`, `cvarlist`, `echo`, `exec <file>`, `bind`/`unbind` (if input system present), `clear`.

## 4. Edge cases & error handling
- Unknown command/cvar → print "unknown: X", suggest closest match (Levenshtein, optional).
- `getFloat`/`getInt` on a non-numeric cvar → return 0, log once.
- Malformed quoted args → best-effort parse, don't crash.
- `exec` of a missing config file → return false, no crash.
- Re-registering an existing command/cvar → replace + log a warning.

## 5. Performance targets
- Not perf-critical (dev tool). O(1) avg lookup via hash maps. No allocations on `getFloat` hot reads (cache parsed value, invalidate on `set`).

## 6. Acceptance tests
1. **T1 — Command:** register `quit`; `exec("quit")` invokes it with empty args.
2. **T2 — Args:** register `echo`; `exec("echo hello world")` prints "hello world".
3. **T3 — CVar get/set:** register `g_speed`=10; `exec("g_speed")` prints 10; `exec("g_speed 12")` sets it; `getInt("g_speed")==12`.
4. **T4 — Typed:** `set("r_scale","0.85")` then `getFloat("r_scale")≈0.85`.
5. **T5 — Persist:** `set` a few cvars, `saveConfig`, fresh console, `loadConfig` → values restored.
6. **T6 — Quoted args:** `exec("bind \"x\" \"say hi\"")` parses 2 args correctly.
7. **T7 — Complete:** `complete("qu")` returns `quit`, `quality`, etc.
8. **T8 — Unknown:** `exec("blarg")` prints unknown + (optional) suggestion, no crash.

## 7. Public references
- Quake console / cvar design (public articles, the original Quake source's cvar concept — concept only, not code).
- Standard command-line tokenization references.

## 8. Suggested permissive libraries
- None required (std only). Optionally a tiny Levenshtein for suggestions.

## 9. Notes for the clean-room implementer
- Port the Babylon X3 `quality <preset>` command onto this so the native engine has the same quality-preset UX (see the Babylon X3 `x3-quality-presets.js` for the preset *values*, not code).
- The console UI (input box, log view, autocomplete dropdown) is the UI layer (M8) — this is the engine-side model only.
- Keep cvar storage string-based for simplicity; typed accessors parse + cache.
