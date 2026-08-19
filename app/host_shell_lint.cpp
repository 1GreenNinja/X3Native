// HOST-SHELL LINT — see host_shell_lint.h for why this exists.
#include "host_shell_lint.h"

#include "asset_root.h"
#include "engine/core/x3_log.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace x3::game {

namespace {

// ---------------------------------------------------------------------------
// Comment stripping. Without it the probe reads its own gravestones: several
// migrated hosts carry lines like "(The old hud.drawConsole call is gone ...)",
// and a raw substring search would score those as violations — a gate that
// fails hardest on the hosts that did the right thing is worse than none.
// Handles // and /* */ and skips string/char literals so a marker inside a
// quoted string is not mistaken for code either.
// ---------------------------------------------------------------------------
std::string stripComments(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    enum { Code, LineC, BlockC, Str, Chr } st = Code;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        const char n = (i + 1 < s.size()) ? s[i + 1] : '\0';
        switch (st) {
            case Code:
                if      (c == '/' && n == '/') { st = LineC;  ++i; out += ' '; }
                else if (c == '/' && n == '*') { st = BlockC; ++i; out += ' '; }
                else if (c == '"')  { st = Str; out += ' '; }
                else if (c == '\'') { st = Chr; out += ' '; }
                else out += c;
                break;
            case LineC:
                if (c == '\n') { st = Code; out += '\n'; }
                break;
            case BlockC:
                if (c == '*' && n == '/') { st = Code; ++i; out += ' '; }
                else if (c == '\n') out += '\n';
                break;
            case Str:
                if      (c == '\\') ++i;                 // escape: skip the next char
                else if (c == '"')  { st = Code; out += ' '; }
                break;
            case Chr:
                if      (c == '\\') ++i;
                else if (c == '\'') { st = Code; out += ' '; }
                break;
        }
    }
    return out;
}

bool contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

// True if some line in `code` both starts (after indent) with "int host" and
// declares a HostContext& parameter — i.e. this TU defines a world entry point.
// Deliberately NOT "contains (HostContext&)": HostShell::attach(HostContext&)
// lives in host_shell.cpp, and the shell is not a world.
bool definesWorldHostEntry(const std::string& code) {
    std::size_t pos = 0;
    while ((pos = code.find("(HostContext&", pos)) != std::string::npos) {
        const std::size_t lineStart = code.rfind('\n', pos);
        std::size_t b = (lineStart == std::string::npos) ? 0 : lineStart + 1;
        while (b < pos && (code[b] == ' ' || code[b] == '\t')) ++b;
        if (code.compare(b, 8, "int host") == 0) return true;
        pos += 13;
    }
    return false;
}

// The console FRONT-END entry points HostShell owns exclusively. A world host
// calling either on a Hud OF ITS OWN has, by definition, built its own console
// surface — which is how Echo Harbor ended up with no shared commands.
//
// THE ONE SANCTIONED ROUTE is HostShell::hudForCallbacks(). Anything reached
// through it IS the shared console by construction — there is no other object
// it can return — so `shell.hudForCallbacks().toggleConsole()` is a host
// DRIVING the shared drop-down, not replacing it. host_tunnel's pause-menu
// "console" row and host_echoharbor's screenshot/teleport commands both do
// exactly that, and HS8 locks the carve-out so it cannot be widened by
// accident. closeConsole is not a marker at all for the same reason: closing
// the shared drop-down from inside a command is normal host business.
const char* const kBespokeConsoleMarkers[] = {
    "drawConsole(",
    "toggleConsole(",
};

// True if `marker` appears in `code` at least once NOT reached through
// HostShell::hudForCallbacks().
bool hasUnmediatedMarker(const std::string& code, const char* marker) {
    static const std::string kMediator = "hudForCallbacks()";
    const std::size_t mlen = std::strlen(marker);
    auto isWs = [](char c) { return c == 0x20 || c == 0x09 || c == 0x0A || c == 0x0D; };
    std::size_t pos = 0;
    while ((pos = code.find(marker, pos)) != std::string::npos) {
        // Walk back over the member-access dot (and any whitespace around it).
        std::size_t b = pos;
        while (b > 0 && isWs(code[b - 1])) --b;
        bool mediated = false;
        if (b > 0 && code[b - 1] == '.') {
            --b;
            while (b > 0 && isWs(code[b - 1])) --b;
            mediated = (b >= kMediator.size() &&
                        code.compare(b - kMediator.size(), kMediator.size(), kMediator) == 0);
        }
        if (!mediated) return true;
        pos += mlen;
    }
    return false;
}

// ---------------------------------------------------------------------------
// EXEMPTIONS. Same deal as destinations.cpp's kRegistryExclusions: a file may
// be off the hook only with a reason a reviewer can check, and HS2 fails on a
// stale entry (a listed file that no longer exists, or one that has no reason).
// ---------------------------------------------------------------------------
struct Exemption { const char* file; const char* why; };
const Exemption kExemptions[] = {
    { "host_shell.cpp",
      "IS the shared shell — the one place allowed to construct the console, "
      "the Hud and the pause menu (it is also not a world: it defines no "
      "int hostX(HostContext&) entry point, so R1 never reaches it)" },
    { "host_menu.cpp",
      "the MAIN MENU, not a world: it runs before any world is chosen and has "
      "no --world flag, so a pause menu over it would be a menu over a menu" },
};

bool isExempt(const std::string& filename) {
    for (const Exemption& e : kExemptions)
        if (filename == e.file) return true;
    return false;
}

// ---------------------------------------------------------------------------
// THE CONTROLS. Verbatim shapes, so the probe's teeth are proven on every run
// even in a shipped build with no source tree.
// ---------------------------------------------------------------------------

// NEGATIVE CONTROL — what host_echotropolis.cpp really looked like before the
// 2026-08-18 migration (excerpted from the deleted lines; the markers are the
// originals, not paraphrases). This MUST trip R1 (no HostShell anywhere) and
// R2 (its own console front-end).
const char* const kPreMigrationEchoHarbor = R"CPP(
#include "../hud.h"
#include "engine/core/IConsole.h"
namespace { x3::game::Hud* g_hud = nullptr;
void charCB(GLFWwindow*, unsigned int c) {
    if (g_hud && g_hud->consoleOpen()) g_hud->onChar(c);
} }
int hostEchotropolis(HostContext& hc) {
    bool menuOpen = false, prevEsc = false, prevGrave = false;
    std::unique_ptr<x3::con::IConsole> console(x3::con::createConsole());
    x3::game::Hud hud;
    bool conQuit = false;
    hud.init(*console, &conQuit);
    g_hud = &hud;
    while (!glfwWindowShouldClose(window) && !wantQuit) {
        const bool gr = glfwGetKey(window, GLFW_KEY_GRAVE_ACCENT) == GLFW_PRESS;
        if (gr && !prevGrave) hud.toggleConsole();
        if (hud.consoleOpen()) {
            auto frame = device->beginFrame();
            hud.drawConsole(*device, frame, *console, dt);
            device->endFrame(frame);
            continue;
        }
        if (menuOpen) { /* 80 lines of hand-drawn pause panel */ continue; }
    }
    return 0;
}
)CPP";

// POSITIVE CONTROL — the migrated shape. Clean under both rules, so a probe
// that simply flags every world host cannot pass this suite.
const char* const kMigratedEchoHarbor = R"CPP(
#include "host_shell.h"
int hostEchoHarbor(HostContext& hc) {
    HostShell shell;
    shell.attach(hc);
    shell.setFreezesSim(true);
    x3::con::IConsole* console = shell.console();
    while (!glfwWindowShouldClose(window) && !shell.wantQuit()) {
        shell.beginFrame();
        auto kd = [&](int k) { return shell.key(k); };
        if (shell.paused()) { continue; }
        auto frame = device->beginFrame();
        shell.draw(frame, dt);
        device->endFrame(frame);
    }
    return 0;
}
)CPP";

// SNEAK CONTROL — wires the shell AND keeps its own console front-end. R1 is
// satisfied, so only R2 can catch it. Proves R2 is load-bearing rather than a
// second opinion on what R1 already found.
const char* const kShellPlusBespokeConsole = R"CPP(
#include "host_shell.h"
int hostSneaky(HostContext& hc) {
    HostShell shell;
    shell.attach(hc);
    x3::game::Hud hud;
    hud.init(*myConsole, &quit);
    while (running) {
        auto frame = device->beginFrame();
        hud.drawConsole(*device, frame, *myConsole, dt);
        shell.draw(frame, dt);
        device->endFrame(frame);
    }
    return 0;
}
)CPP";

// MEDIATED CONTROL — drives the SHARED drop-down through the shell's own
// accessor. Must come back CLEAN, or the rule would punish the correct way to
// put a "console" row in a pause menu and hosts would go back to rolling their
// own.
const char* const kShellMediatedConsole = R"CPP(
#include "host_shell.h"
int hostMediated(HostContext& hc) {
    HostShell shell;
    shell.attach(hc);
    while (running) {
        if (gameMenu.takeConsoleRequest()) shell.hudForCallbacks().toggleConsole();
        auto frame = device->beginFrame();
        shell.draw(frame, dt);
        device->endFrame(frame);
    }
    return 0;
}
)CPP";

// ---------------------------------------------------------------------------
// Source-tree resolution. Same candidate list as --test-sealevel's findFile:
// tests run from the repo root in CI and from build/bin/Release by hand.
// ---------------------------------------------------------------------------
std::string findWorldHostsDir() {
    namespace fs = std::filesystem;
    const std::string cands[] = {
        "app/world_hosts",
        "../../../app/world_hosts",
        assetRoot() + "/../app/world_hosts",
    };
    std::error_code ec;
    for (const std::string& c : cands) {
        // The marker file makes a wrong-but-existing directory impossible to
        // mistake for the right one (asset_root.h learned this the hard way).
        if (fs::exists(fs::path(c) / "host_shell.h", ec)) return c;
    }
    return {};
}

int hsPass = 0, hsFail = 0;
void hsCheck(bool cond, const char* name) {
    if (cond) { ++hsPass; x3::logInfo(std::string("[hostshell] PASS ") + name); }
    else      { ++hsFail; x3::logError(std::string("[hostshell] FAIL ") + name); }
}

} // namespace

HostShellVerdict inspectWorldHostSource(const std::string& src) {
    const std::string code = stripComments(src);
    HostShellVerdict v;
    v.isWorldHost = definesWorldHostEntry(code);
    v.wiresShell  = contains(code, "HostShell");
    for (const char* m : kBespokeConsoleMarkers)
        if (hasUnmediatedMarker(code, m)) v.bespokeConsole.push_back(m);
    return v;
}

bool runHostShellLint() {
    hsPass = hsFail = 0;

    // ---- HS4/HS5/HS6: the controls. These run FIRST and ALWAYS, so the gate's
    // red-capability is proven on every invocation, source tree or not.
    {
        const HostShellVerdict pre = inspectWorldHostSource(kPreMigrationEchoHarbor);
        const bool caught = pre.isWorldHost && !pre.wiresShell && !pre.bespokeConsole.empty();
        if (!caught)
            x3::logError("[hostshell]   negative control NOT caught: isWorldHost=" +
                         std::to_string((int)pre.isWorldHost) + " wiresShell=" +
                         std::to_string((int)pre.wiresShell) + " bespokeMarkers=" +
                         std::to_string((int)pre.bespokeConsole.size()));
        else
            x3::logInfo("[hostshell] negative control (pre-migration Echo Harbor) trips "
                        "R1 (no HostShell) and R2 (" +
                        std::to_string((int)pre.bespokeConsole.size()) +
                        " console front-end call(s)) — the probe can go RED");
        hsCheck(caught, "HS4 NEGATIVE CONTROL: pre-migration Echo Harbor is CAUGHT by R1 and R2");
    }
    {
        const HostShellVerdict post = inspectWorldHostSource(kMigratedEchoHarbor);
        hsCheck(post.isWorldHost && post.wiresShell && post.bespokeConsole.empty(),
                "HS5 POSITIVE CONTROL: the migrated shape is CLEAN (the probe is not a blanket)");
    }
    {
        const HostShellVerdict sneak = inspectWorldHostSource(kShellPlusBespokeConsole);
        hsCheck(sneak.isWorldHost && sneak.wiresShell && !sneak.bespokeConsole.empty(),
                "HS6 SNEAK CONTROL: shell-wired host with its OWN console is caught by R2 alone");
    }

    {
        const HostShellVerdict med = inspectWorldHostSource(kShellMediatedConsole);
        hsCheck(med.isWorldHost && med.wiresShell && med.bespokeConsole.empty(),
                "HS8 MEDIATED CONTROL: driving the SHARED console via "
                "shell.hudForCallbacks() is CLEAN (the sanctioned route stays open)");
    }

    // ---- HS2: exemption hygiene, before the sweep uses the list.
    const std::string dir = findWorldHostsDir();
    {
        namespace fs = std::filesystem;
        bool ok = true;
        std::error_code ec;
        for (const Exemption& e : kExemptions) {
            if (!e.why[0]) { ok = false; continue; }
            if (dir.empty()) continue;   // cannot check existence without the tree
            if (!fs::exists(fs::path(dir) / e.file, ec)) {
                ok = false;
                x3::logError(std::string("[hostshell]   exemption '") + e.file +
                             "' names a file that does not exist — stale exemption");
            }
        }
        hsCheck(ok, "HS2 exemptions are reasoned and name real files");
    }

    // ---- HS1/HS3: the live sweep.
    if (dir.empty()) {
        x3::logWarn("[hostshell] SKIP the live sweep: app/world_hosts/ not reachable from "
                    "this working directory (run from the repo root to lint the real "
                    "tree). The controls above still proved the probe red-capable.");
        x3::logInfo("hostshell: " + std::to_string(hsPass) + "/" +
                    std::to_string(hsPass + hsFail) + " passed (sweep skipped)");
        return hsFail == 0;
    }

    namespace fs = std::filesystem;
    int worldHosts = 0, shellWired = 0;
    std::vector<std::string> viols;
    std::error_code ec;
    std::vector<fs::path> files;
    for (const auto& de : fs::directory_iterator(dir, ec)) {
        const std::string fn = de.path().filename().string();
        if (fn.rfind("host_", 0) != 0) continue;
        if (de.path().extension() != ".cpp") continue;
        files.push_back(de.path());
    }
    std::sort(files.begin(), files.end());

    for (const fs::path& f : files) {
        const std::string fn = f.filename().string();
        std::ifstream in(f, std::ios::binary);
        if (!in) { viols.push_back("HOSTSHELL " + fn + ": unreadable"); continue; }
        std::ostringstream ss; ss << in.rdbuf();
        const HostShellVerdict v = inspectWorldHostSource(ss.str());
        if (!v.isWorldHost) continue;          // builder module / helper TU
        ++worldHosts;
        if (v.wiresShell) ++shellWired;
        if (!v.wiresShell && !isExempt(fn))
            viols.push_back("HOSTSHELL " + fn + ": defines a world host but NEVER references "
                            "HostShell — this world has no shared console, no pause menu, no "
                            "FPS overlay and none of the ~118 shared commands (R1)");
        for (const std::string& m : v.bespokeConsole)
            viols.push_back("HOSTSHELL " + fn + ": calls " + m + " — a world host must not "
                            "build its own console front-end; HostShell owns it (R2)");
    }

    for (const std::string& s : viols) x3::logError("[hostshell]   " + s);
    hsCheck(viols.empty(), "HS1/HS3 every world host wires HostShell and none builds its own console");

    // A glob that matched nothing would pass HS1 vacuously — the classic
    // guard-that-does-not-guard. Floor it well under today's count so adding or
    // folding a host does not trip it, but a broken path does.
    hsCheck(worldHosts >= 20,
            "HS7 the sweep actually read the world hosts (>= 20 entry points found)");

    x3::logInfo("[hostshell] swept " + std::to_string((int)files.size()) + " host_*.cpp in " +
                dir + ": " + std::to_string(worldHosts) + " world host(s), " +
                std::to_string(shellWired) + " on the shared shell, " +
                std::to_string((int)viols.size()) + " violation(s)");
    x3::logInfo("hostshell: " + std::to_string(hsPass) + "/" +
                std::to_string(hsPass + hsFail) + " passed");
    return hsFail == 0;
}

} // namespace x3::game
