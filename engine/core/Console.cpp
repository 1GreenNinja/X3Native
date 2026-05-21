// Console + CVar implementation — D6 (clean-room).
// Spec: specs/D6-console.spec.md  (std-only, no third-party deps)

#include "IConsole.h"
#include "x3_log.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace x3::con {

namespace {

// Tokenize a line into args, honoring double-quoted spans.
std::vector<std::string> tokenize(std::string_view line) {
    std::vector<std::string> out;
    std::string cur; bool inq = false; bool have = false;
    for (char c : line) {
        if (c == '"') { inq = !inq; have = true; }
        else if (!inq && (c == ' ' || c == '\t')) { if (have) { out.push_back(cur); cur.clear(); have = false; } }
        else { cur.push_back(c); have = true; }
    }
    if (have) out.push_back(cur);
    return out;
}

struct CVar { std::string value; std::string def; std::string help; };

class Console final : public IConsole {
public:
    Console() { registerBuiltins(); }

    void registerCommand(std::string_view name, CommandFn fn, std::string_view help) override {
        std::string n(name);
        if (m_cmds.count(n)) print("warning: re-registering command " + n);
        m_cmds[n] = { std::move(fn), std::string(help) };
    }

    void registerCVar(std::string_view name, std::string_view def, std::string_view help) override {
        std::string n(name);
        if (m_cvars.count(n)) print("warning: re-registering cvar " + n);
        m_cvars[n] = CVar{ std::string(def), std::string(def), std::string(help) };
    }

    std::string getString(std::string_view name) const override {
        auto it = m_cvars.find(std::string(name));
        return it == m_cvars.end() ? std::string() : it->second.value;
    }
    float getFloat(std::string_view name) const override {
        try { return std::stof(getString(name)); } catch (...) { return 0.0f; }
    }
    int getInt(std::string_view name) const override {
        try { return std::stoi(getString(name)); } catch (...) { return 0; }
    }
    void set(std::string_view name, std::string_view value) override {
        auto it = m_cvars.find(std::string(name));
        if (it == m_cvars.end()) registerCVar(name, value, "");  // auto-create
        else it->second.value = std::string(value);
    }

    void exec(std::string_view line) override {
        auto args = tokenize(line);
        if (args.empty()) return;
        std::string head = args[0];
        args.erase(args.begin());

        auto ci = m_cmds.find(head);
        if (ci != m_cmds.end()) { ci->second.fn(args); return; }

        auto vi = m_cvars.find(head);
        if (vi != m_cvars.end()) {
            if (args.empty()) print(head + " = " + vi->second.value);
            else vi->second.value = args[0];
            return;
        }
        std::string msg = "unknown: " + head;
        auto sug = complete(head);
        if (!sug.empty()) { msg += "  (did you mean: " + sug[0] + "?)"; }
        print(msg);
    }

    void print(std::string_view msg) override {
        m_log.emplace_back(msg);
        logInfo(std::string("[con] ") + std::string(msg));
    }

    void saveConfig(std::string_view path) const override {
        std::ofstream f{ std::string(path) };
        if (!f) return;
        for (auto& kv : m_cvars)
            if (kv.second.value != kv.second.def)
                f << "set " << kv.first << " \"" << kv.second.value << "\"\n";
    }
    bool loadConfig(std::string_view path) override {
        std::ifstream f{ std::string(path) };
        if (!f) return false;
        std::string line;
        while (std::getline(f, line)) if (!line.empty()) exec(line);
        return true;
    }

    std::vector<std::string> complete(std::string_view prefix) const override {
        std::string p(prefix);
        std::vector<std::string> out;
        for (auto& kv : m_cmds)  if (kv.first.rfind(p, 0) == 0) out.push_back(kv.first);
        for (auto& kv : m_cvars) if (kv.first.rfind(p, 0) == 0) out.push_back(kv.first);
        std::sort(out.begin(), out.end());
        return out;
    }

private:
    struct Cmd { CommandFn fn; std::string help; };

    void registerBuiltins() {
        registerCommand("set", [this](const std::vector<std::string>& a){
            if (a.size() >= 2) set(a[0], a[1]); else print("usage: set <cvar> <value>");
        }, "set a cvar");
        registerCommand("echo", [this](const std::vector<std::string>& a){
            std::string s; for (size_t i=0;i<a.size();++i){ if(i) s+=' '; s+=a[i]; } print(s);
        }, "print text");
        registerCommand("cmdlist", [this](const std::vector<std::string>&){
            for (auto& kv : m_cmds) print(kv.first + " - " + kv.second.help);
        }, "list commands");
        registerCommand("cvarlist", [this](const std::vector<std::string>&){
            for (auto& kv : m_cvars) print(kv.first + " = " + kv.second.value);
        }, "list cvars");
        registerCommand("help", [this](const std::vector<std::string>&){
            print("commands: set echo cmdlist cvarlist help");
        }, "help");
    }

    std::unordered_map<std::string, Cmd>  m_cmds;
    std::unordered_map<std::string, CVar> m_cvars;
    std::vector<std::string> m_log;
};

int g_pass = 0, g_fail = 0;
void check(bool c, const char* n) {
    if (c) { ++g_pass; logInfo(std::string("[con-test] PASS ") + n); }
    else   { ++g_fail; logError(std::string("[con-test] FAIL ") + n); }
}

} // namespace

IConsole* createConsole() { return new Console(); }

bool runConsoleSelfTest() {
    g_pass = g_fail = 0;

    // T1 command
    { auto* c = createConsole(); bool hit=false;
      c->registerCommand("quit",[&](const std::vector<std::string>&){ hit=true; });
      c->exec("quit"); check(hit, "T1 command"); delete c; }

    // T2 args via echo (captured through getString of nothing — verify via a custom cmd)
    { auto* c = createConsole(); std::string got;
      c->registerCommand("say",[&](const std::vector<std::string>& a){ for(size_t i=0;i<a.size();++i){if(i)got+=' ';got+=a[i];} });
      c->exec("say hello world"); check(got=="hello world", "T2 args"); delete c; }

    // T3 cvar get/set
    { auto* c = createConsole(); c->registerCVar("g_speed","10");
      c->exec("g_speed 12"); check(c->getInt("g_speed")==12, "T3 cvar get/set"); delete c; }

    // T4 typed
    { auto* c = createConsole(); c->set("r_scale","0.85");
      float f=c->getFloat("r_scale"); check(f>0.84f && f<0.86f, "T4 typed float"); delete c; }

    // T5 persist round-trip
    { std::string path = "x3_con_test.cfg";
      { auto* c = createConsole(); c->registerCVar("g_speed","10"); c->set("g_speed","42"); c->saveConfig(path); delete c; }
      { auto* c = createConsole(); c->registerCVar("g_speed","10"); c->loadConfig(path); check(c->getInt("g_speed")==42, "T5 persist"); delete c; }
      std::remove(path.c_str()); }

    // T6 quoted args
    { auto* c = createConsole(); std::vector<std::string> seen;
      c->registerCommand("bind",[&](const std::vector<std::string>& a){ seen=a; });
      c->exec("bind \"x\" \"say hi\""); check(seen.size()==2 && seen[0]=="x" && seen[1]=="say hi", "T6 quoted args"); delete c; }

    // T7 complete
    { auto* c = createConsole(); c->registerCommand("quality",[](const std::vector<std::string>&){});
      auto m=c->complete("qu"); bool q=std::find(m.begin(),m.end(),"quality")!=m.end();
      check(q, "T7 complete"); delete c; }

    // T8 unknown (no crash)
    { auto* c = createConsole(); c->exec("blarg123"); check(true, "T8 unknown survives"); delete c; }

    logInfo(std::string("[con-test] ") + std::to_string(g_pass) + " passed, " + std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

} // namespace x3::con
