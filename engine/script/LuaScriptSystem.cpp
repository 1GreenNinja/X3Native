// LuaScriptSystem — implementation of IScriptSystem on Lua 5.4 + sol2 v3.x
// ("sol3"). Spec: specs/D14-script.spec.md. CLEAN-ROOM, original work: built
// from the public Lua 5.4 reference manual and the sol2 documentation only.
//
// ARCHITECTURE
// ------------
//  * ONE lua_State for the whole system, MANY sol::environment sandboxes (one
//    per script). Environments share the VM (cheap, one GC, cross-script
//    events are a table lookup) while keeping globals isolated — script A's
//    `health = 5` can never collide with script B's.
//  * SANDBOX: each environment gets a hand-picked whitelist copied from the
//    base library (pairs/ipairs/tostring/... and the math/string/table libs).
//    io/os/require/load/dofile/loadstring/collectgarbage are simply ABSENT —
//    not stubbed, absent — so pak content cannot reach the host machine.
//  * EVERY call into Lua goes through callProtected(): sol::protected_function
//    with a traceback handler. An error marks the script failed (skipped until
//    reload) and logs once — the engine and the other scripts keep running.
//  * HOT RELOAD keeps the ScriptId stable: the slot's environment is rebuilt,
//    the new chunk runs, onInit re-fires. Old timers belonging to the script
//    are cancelled (their captured closures are stale).
//
// BINDING EXTENSION POINT
// -----------------------
// Engine/app systems that want richer Lua APIs (entities, audio, triggers,
// vehicles) extend buildX3Api() below — sol is visible here, so usertypes and
// typed lambdas are one-liners, e.g.:
//     api.set_function("playSound", [audio](const std::string& cue){ ... });
// Keep IScriptSystem.h scripting-API-free; add typed surface HERE.
#include "engine/script/IScriptSystem.h"
#include "engine/core/IConsole.h"
#include "engine/core/x3_log.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

namespace x3::script {

namespace {

// Whitelisted base-library globals copied into every sandbox. Deliberately
// excludes: io, os, require, package, load, loadstring, dofile, loadfile,
// rawset/rawget/rawequal (metatable bypass), collectgarbage, debug, coroutine
// (until a script needs it), and getmetatable/setmetatable.
constexpr const char* kSafeGlobals[] = {
    "assert", "error", "ipairs", "next", "pairs", "pcall", "select",
    "tonumber", "tostring", "type", "unpack", "xpcall",
};
constexpr const char* kSafeLibs[] = { "math", "string", "table" };

std::string statusName(std::string_view name) { return std::string(name); }

// ---- sol <-> ScriptValue marshaling (kept here so the header stays sol-free) --
// The native-function boundary speaks ScriptValue; these two helpers are the
// ONLY place sol types meet it. Lua nil/bool/number/string map 1:1; anything
// else (tables, functions) is passed through as its tostring() so a binding at
// least sees a stable string instead of throwing.
ScriptValue solToValue(const sol::object& o, sol::state_view L) {
    switch (o.get_type()) {
        case sol::type::nil:     return ScriptValue{};
        case sol::type::boolean: return ScriptValue(o.as<bool>());
        case sol::type::number:  return ScriptValue(o.as<double>());
        case sol::type::string:  return ScriptValue(o.as<std::string>());
        default:                 return ScriptValue(L["tostring"](o).get<std::string>());
    }
}

sol::object valueToSol(const ScriptValue& v, sol::state_view L) {
    switch (v.type) {
        case ScriptValue::Type::Bool:   return sol::make_object(L, v.b);
        case ScriptValue::Type::Number: return sol::make_object(L, v.n);
        case ScriptValue::Type::String: return sol::make_object(L, v.s);
        case ScriptValue::Type::Nil:
        default:                        return sol::make_object(L, sol::nil);
    }
}

} // namespace

std::string ScriptValue::asString() const {
    switch (type) {
        case Type::Bool:   return b ? "true" : "false";
        case Type::Number: {
            // Integers print without a trailing ".0" (door/zone ids read cleanly).
            if (n == (double)(long long)n) return std::to_string((long long)n);
            return std::to_string(n);
        }
        case Type::String: return s;
        case Type::Nil:
        default:           return "";
    }
}

class LuaScriptSystem final : public IScriptSystem {
public:
    explicit LuaScriptSystem(con::IConsole* console) : console_(console) {
        lua_.open_libraries(sol::lib::base, sol::lib::math, sol::lib::string,
                            sol::lib::table);
        if (console_) registerConsoleCommands();
    }

    // ---- IScriptSystem -----------------------------------------------------

    ScriptId load(std::string_view name, std::string_view source) override {
        Slot* slot = findByName(name);
        ScriptId id;
        if (slot) {                       // HOT RELOAD: same id, fresh sandbox
            id = slot->id;
            cancelTimersFor(id);
        } else {
            slots_.push_back({});
            slot = &slots_.back();
            slot->id = nextId_++;
            id = slot->id;
        }
        slot->name = statusName(name);
        slot->source.assign(source.data(), source.size());
        slot->failed = false;
        slot->loaded = false;
        slot->lastError.clear();
        slot->env = makeSandbox(*slot);

        // Compile + run the chunk inside the sandbox, then onInit if present.
        sol::load_result chunk = lua_.load(slot->source, "@" + slot->name);
        if (!chunk.valid()) {
            failSlot(*slot, chunk.get<sol::error>().what());
            return id;                    // valid id; fix + reload under same id
        }
        sol::protected_function body = chunk;
        sol::set_environment(slot->env, body);
        if (!callProtected(*slot, body)) return id;
        slot->loaded = true;
        callHook(*slot, "onInit");
        return id;
    }

    bool unload(ScriptId id) override {
        auto it = std::find_if(slots_.begin(), slots_.end(),
                               [&](const Slot& s) { return s.id == id; });
        if (it == slots_.end()) return false;
        cancelTimersFor(id);
        slots_.erase(it);
        lua_.collect_garbage();           // drop the orphaned environment now
        return true;
    }

    ScriptStatus status(ScriptId id) const override {
        for (const Slot& s : slots_)
            if (s.id == id) return { s.name, s.loaded, s.failed, s.lastError };
        return {};
    }

    std::vector<ScriptId> loadedScripts() const override {
        std::vector<ScriptId> out;
        out.reserve(slots_.size());
        for (const Slot& s : slots_) out.push_back(s.id);
        return out;
    }

    void update(double dt) override {
        time_ += dt;

        // Fire due one-shot timers. A timer callback may schedule more timers,
        // so collect-then-call: stable against mutation during iteration.
        std::vector<Timer> due;
        for (auto it = timers_.begin(); it != timers_.end();) {
            if (it->dueAt <= time_) { due.push_back(std::move(*it)); it = timers_.erase(it); }
            else ++it;
        }
        for (Timer& t : due) {
            Slot* s = findById(t.owner);
            if (s && !s->failed) callProtected(*s, t.fn);
        }

        for (Slot& s : slots_)
            if (s.loaded && !s.failed) callHook(s, "onUpdate", dt);
    }

    void fire(std::string_view event, const EventArgs& args) override {
        // Build the args table once; environments share the VM so one table
        // can be handed to every script.
        sol::table t = lua_.create_table((int)0, (int)args.size());
        for (const auto& [k, v] : args) t[k] = v;
        const std::string ev(event);
        for (Slot& s : slots_)
            if (s.loaded && !s.failed) callHook(s, "onEvent", ev, t);
    }

    void registerFunction(std::string_view name, NativeFn fn) override {
        hostFns_[std::string(name)] = std::move(fn);
        // Already-loaded scripts see it too: refresh each env's x3 table entry.
        for (Slot& s : slots_) installHostFn(s, std::string(name));
    }

    std::string eval(ScriptId id, std::string_view code) override {
        Slot* s = findById(id);
        if (!s) return "ERROR: no such script";
        sol::load_result chunk = lua_.load("return " + std::string(code), "@eval");
        if (!chunk.valid())                 // not an expression? try a statement
            chunk = lua_.load(std::string(code), "@eval");
        if (!chunk.valid()) return std::string("ERROR: ") + chunk.get<sol::error>().what();
        sol::protected_function body = chunk;
        sol::set_environment(s->env, body);
        sol::protected_function_result r = body();
        if (!r.valid()) return std::string("ERROR: ") + r.get<sol::error>().what();
        sol::object o = r;
        return o == sol::nil ? "nil" : lua_["tostring"](o).get<std::string>();
    }

    size_t memoryUsedBytes() const override {
        // LUA_GCCOUNT = KB in use + LUA_GCCOUNTB = remainder bytes.
        lua_State* L = lua_.lua_state();
        return (size_t)lua_gc(L, LUA_GCCOUNT, 0) * 1024
             + (size_t)lua_gc(L, LUA_GCCOUNTB, 0);
    }

private:
    struct Slot {
        ScriptId id = kInvalidScript;
        std::string name;
        std::string source;               // kept for script_reload
        sol::environment env;
        bool loaded = false;
        bool failed = false;
        std::string lastError;
    };
    struct Timer {
        double dueAt = 0.0;
        ScriptId owner = kInvalidScript;
        sol::protected_function fn;
    };

    Slot* findByName(std::string_view name) {
        for (Slot& s : slots_) if (s.name == name) return &s;
        return nullptr;
    }
    Slot* findById(ScriptId id) {
        for (Slot& s : slots_) if (s.id == id) return &s;
        return nullptr;
    }

    void failSlot(Slot& s, std::string err) {
        s.failed = true;
        s.lastError = std::move(err);
        logError("[script] '" + s.name + "' failed: " + s.lastError);
        if (console_) console_->print("[script] '" + s.name + "' failed: " + s.lastError);
    }

    // Single choke point for entering Lua. Returns false (and fails the slot)
    // on error. Variadic so hooks/timers/evals all share the protection.
    template <class... A>
    bool callProtected(Slot& s, sol::protected_function& fn, A&&... a) {
        if (!fn.valid()) return true;     // hook absent — fine, not an error
        sol::protected_function_result r = fn(std::forward<A>(a)...);
        if (!r.valid()) { failSlot(s, r.get<sol::error>().what()); return false; }
        return true;
    }
    template <class... A>
    void callHook(Slot& s, const char* hook, A&&... a) {
        sol::object o = s.env[hook];
        if (o.get_type() != sol::type::function) return;
        sol::protected_function fn = o;
        callProtected(s, fn, std::forward<A>(a)...);
    }

    void cancelTimersFor(ScriptId id) {
        timers_.erase(std::remove_if(timers_.begin(), timers_.end(),
                      [&](const Timer& t) { return t.owner == id; }),
                      timers_.end());
    }

    void installHostFn(Slot& s, const std::string& name) {
        sol::table api = s.env["x3"];
        if (!api.valid()) return;
        auto& fn = hostFns_[name];
        api.set_function(name, [&fn](sol::variadic_args va, sol::this_state ts) -> sol::object {
            sol::state_view L(ts);
            std::vector<ScriptValue> args;
            args.reserve(va.size());
            for (auto v : va) args.push_back(solToValue(v.get<sol::object>(), L));
            ScriptValue r = fn(args);
            return valueToSol(r, L);     // Nil-typed result surfaces to Lua as nil
        });
    }

    sol::environment makeSandbox(Slot& slot) {
        sol::environment env(lua_, sol::create);

        // Whitelisted stdlib (copies of the references — the libs themselves
        // are immutable from the sandbox's point of view in practice; a
        // hardened build can deep-copy or freeze them).
        for (const char* g : kSafeGlobals) env[g] = lua_[g];
        for (const char* l : kSafeLibs)    env[l] = lua_[l];

        // ---- the x3 API table (see header for the contract) ----
        sol::table api = lua_.create_table();
        api["name"] = slot.name;
        api.set_function("log",   [](const std::string& m) { logInfo ("[lua] " + m); });
        api.set_function("warn",  [](const std::string& m) { logWarn ("[lua] " + m); });
        api.set_function("error", [](const std::string& m) { logError("[lua] " + m); });
        api.set_function("time",  [this]() { return time_; });
        api.set_function("cvar", [this](const std::string& n) -> std::string {
            return console_ ? console_->getString(n) : "";
        });
        api.set_function("setcvar", [this](const std::string& n, const std::string& v) {
            if (console_) console_->set(n, v);
        });
        api.set_function("exec", [this](const std::string& line) {
            if (console_) console_->exec(line);
        });
        const ScriptId owner = slot.id;
        api.set_function("after", [this, owner](double seconds, sol::protected_function fn) {
            timers_.push_back({ time_ + seconds, owner, std::move(fn) });
        });
        api.set_function("fire", [this](const std::string& ev, sol::optional<sol::table> t) {
            EventArgs args;
            if (t) for (auto& [k, v] : *t)
                args.emplace_back(k.as<std::string>(),
                                  lua_["tostring"](v).get<std::string>());
            fire(ev, args);               // re-entrant-safe: builds a new table
        });
        // print() inside a script goes to the log, tagged with the script name.
        const std::string tag = slot.name;
        env.set_function("print", [tag](sol::variadic_args va, sol::this_state ts) {
            sol::state_view L(ts);
            std::string line;
            for (auto v : va) {
                if (!line.empty()) line += "\t";
                line += L["tostring"](v.get<sol::object>()).get<std::string>();
            }
            logInfo("[" + tag + "] " + line);
        });

        env["x3"] = api;
        for (auto& [n, _] : hostFns_) { Slot tmp; tmp.env = env; installHostFn(tmp, n); }
        return env;
    }

    void registerConsoleCommands() {
        console_->registerCommand("script_list",
            [this](const std::vector<std::string>&) {
                for (const Slot& s : slots_)
                    console_->print("  " + s.name +
                        (s.failed ? "  [FAILED: " + s.lastError + "]"
                                  : s.loaded ? "  [ok]" : "  [loading]"));
            }, "list loaded scripts + status");
        console_->registerCommand("script_reload",
            [this](const std::vector<std::string>& a) {
                if (a.empty()) { console_->print("usage: script_reload <name>"); return; }
                Slot* s = findByName(a[0]);
                if (!s) { console_->print("no script '" + a[0] + "'"); return; }
                load(s->name, s->source);
                console_->print("reloaded '" + a[0] + "'");
            }, "re-run a loaded script's last source");
        console_->registerCommand("lua",
            [this](const std::vector<std::string>& a) {
                if (a.size() < 2) { console_->print("usage: lua <script> <code...>"); return; }
                Slot* s = findByName(a[0]);
                if (!s) { console_->print("no script '" + a[0] + "'"); return; }
                std::string code;
                for (size_t i = 1; i < a.size(); ++i) { if (i > 1) code += " "; code += a[i]; }
                console_->print(eval(s->id, code));
            }, "evaluate Lua in a script's environment");
    }

    sol::state lua_;
    con::IConsole* console_ = nullptr;
    std::vector<Slot> slots_;
    std::vector<Timer> timers_;
    std::unordered_map<std::string, NativeFn> hostFns_;
    double time_ = 0.0;
    ScriptId nextId_ = 1;
};

IScriptSystem* createLuaScriptSystem(con::IConsole* console) {
    return new LuaScriptSystem(console);
}

// ---------------------------------------------------------------------------
// D14 acceptance tests — in-process, no files, real console.
// ---------------------------------------------------------------------------
bool runScriptSelfTest() {
    int pass = 0, fail = 0;
    auto check = [&](bool ok, const char* what) {
        (ok ? pass : fail)++;
        log(ok ? LogLevel::Info : LogLevel::Error,
            std::string(ok ? "  [PASS] " : "  [FAIL] ") + what);
    };

    con::IConsole* con = con::createConsole();
    IScriptSystem* sys = createLuaScriptSystem(con);

    // 1) load + onInit runs, state persists in the sandbox
    ScriptId a = sys->load("alpha", R"(
        counter = 0
        inited = false
        function onInit() inited = true end
        function onUpdate(dt) counter = counter + 1 end
        function onEvent(name, args)
            if name == "ping" then last = args.from end
        end
    )");
    check(a != kInvalidScript && sys->status(a).loaded, "load: script loads");
    check(sys->eval(a, "inited") == "true",             "init: onInit ran");

    // 2) update drives onUpdate
    sys->update(0.016); sys->update(0.016); sys->update(0.016);
    check(sys->eval(a, "counter") == "3",               "update: onUpdate per tick");

    // 3) events with args
    sys->fire("ping", {{"from", "engine"}});
    check(sys->eval(a, "last") == "engine",             "events: fire -> onEvent(args)");

    // 4) sandbox isolation between scripts
    ScriptId b = sys->load("beta", "counter = 999");
    check(sys->eval(a, "counter") == "3",               "sandbox: globals isolated");

    // 5) host machine is unreachable
    check(sys->eval(a, "io") == "nil" &&
          sys->eval(a, "os") == "nil" &&
          sys->eval(a, "require") == "nil" &&
          sys->eval(a, "load") == "nil" &&
          sys->eval(a, "dofile") == "nil",              "sandbox: io/os/require/load absent");

    // 6) error containment: a bad script fails alone, others keep running
    ScriptId c = sys->load("crashy", R"(
        function onUpdate(dt) error("boom") end
    )");
    sys->update(0.016);
    check(sys->status(c).failed &&
          !sys->status(a).failed,                       "containment: bad script quarantined");
    int before = std::stoi(sys->eval(a, "counter"));
    sys->update(0.016);
    check(std::stoi(sys->eval(a, "counter")) == before + 1,
                                                        "containment: healthy scripts continue");

    // 7) compile errors are contained too (and keep their id)
    ScriptId d = sys->load("syntax", "function oops( end");
    check(d != kInvalidScript && sys->status(d).failed, "containment: compile error -> failed, id valid");

    // 8) hot reload: same id, fresh env, onInit re-runs, fix revives a failed script
    ScriptId d2 = sys->load("syntax", "fixed = true function onInit() ok = 1 end");
    check(d2 == d && !sys->status(d).failed &&
          sys->eval(d, "ok") == "1",                    "hot reload: same id, healthy again");

    // 9) cvar bridge through the REAL console
    con->registerCVar("g_difficulty", "2", "test cvar");
    check(sys->eval(a, "x3.cvar('g_difficulty')") == "2", "console: cvar read");
    sys->eval(a, "x3.setcvar('g_difficulty', '5')");
    check(con->getInt("g_difficulty") == 5,             "console: cvar write");

    // 10) timers: x3.after fires once, at/after the due time, not before
    sys->load("timer", R"(
        fired = 0
        function onInit() x3.after(0.05, function() fired = fired + 1 end) end
    )");
    ScriptId t = sys->load("timer", ""); (void)t;       // (reload wipes it; redo properly)
    t = sys->load("timer", R"(
        fired = 0
        function onInit() x3.after(0.05, function() fired = fired + 1 end) end
    )");
    sys->update(0.02);
    bool notYet = sys->eval(t, "fired") == "0";
    sys->update(0.04);
    bool firedOnce = sys->eval(t, "fired") == "1";
    sys->update(0.10);
    bool onlyOnce = sys->eval(t, "fired") == "1";
    check(notYet && firedOnce && onlyOnce,              "timers: one-shot at due time");

    // 11) script -> script events via x3.fire
    sys->eval(t, "x3.fire('ping', { from = 'timer' })");
    check(sys->eval(a, "last") == "timer",              "events: script-to-script fire");

    // 12) registerFunction round-trip: C++ sees typed args, Lua sees the return.
    //     Register a native fn that records what it was called with and returns a
    //     computed value, then call it from Lua and assert BOTH directions.
    {
        std::vector<ScriptValue> seen;
        sys->registerFunction("hostAdd", [&seen](const std::vector<ScriptValue>& a) -> ScriptValue {
            seen = a;                                  // capture args C++ received
            double sum = 0.0;
            for (const auto& v : a) sum += v.asNumber();
            return ScriptValue(sum);                   // hand a number back to Lua
        });
        // One call with two numbers: C++ must see the typed args, AND the numeric
        // result must arrive back in Lua (compared in-Lua so 5.4's "42.0" float
        // formatting is irrelevant). `okRet` is captured INSIDE the same eval so the
        // `seen` snapshot below reflects exactly this call's args.
        check(sys->eval(a, "okRet = (x3.hostAdd(2, 40) == 42); return okRet") == "true",
                                                        "registerFunction: Lua receives the return value");
        check(seen.size() == 2 &&
              seen[0].type == ScriptValue::Type::Number && seen[0].asInt() == 2 &&
              seen[1].type == ScriptValue::Type::Number && seen[1].asInt() == 40,
                                                        "registerFunction: C++ saw the typed args");
        // Mixed types + a string return, and a Nil (void-style) binding => nil in Lua.
        sys->registerFunction("hostEcho", [](const std::vector<ScriptValue>& a) -> ScriptValue {
            return a.empty() ? ScriptValue() : ScriptValue(a[0].asString() + "!");
        });
        check(sys->eval(a, "x3.hostEcho('ok')") == "ok!",
                                                        "registerFunction: string in/out marshals");
        sys->registerFunction("hostVoid", [](const std::vector<ScriptValue>&) -> ScriptValue {
            return ScriptValue();                      // void-style => nil
        });
        check(sys->eval(a, "x3.hostVoid(1) == nil and 'nil' or 'val'") == "nil",
                                                        "registerFunction: Nil result -> Lua nil");
        // A function registered AFTER a script loaded is still visible to it.
        bool late = false;
        sys->registerFunction("hostLate", [&late](const std::vector<ScriptValue>&) -> ScriptValue {
            late = true; return ScriptValue(true);
        });
        sys->eval(a, "x3.hostLate()");
        check(late,                                     "registerFunction: late registration reaches loaded scripts");
    }

    // 13) memory metric is sane and unload reclaims
    size_t memBefore = sys->memoryUsedBytes();
    check(memBefore > 1024,                             "memory: metric reports usage");
    sys->unload(b);
    check(sys->status(b).name.empty(),                  "unload: script removed");

    log(fail == 0 ? LogLevel::Info : LogLevel::Error,
        "[script selftest] " + std::to_string(pass) + " passed, " +
        std::to_string(fail) + " failed");
    delete sys;
    delete con;
    return fail == 0;
}

} // namespace x3::script
