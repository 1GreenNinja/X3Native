// capture_manifest — see capture_manifest.h for WHY. This file is the whole
// implementation: a small ordered registry plus one loud printer.
#include "capture_manifest.h"

#include "engine/core/x3_log.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <vector>

namespace x3 { namespace capture {
namespace {

struct Entry {
    std::string name;
    std::string consequence;
    unsigned    ticks   = 0;   // times the subsystem actually ran
    unsigned    passed  = 0;   // gate() decisions that let content through
    unsigned    blocked = 0;   // gate() decisions that suppressed content
};

// Armed is atomic and checked FIRST by every entry point, so a normal play or
// smoketest run pays one relaxed load per call and records nothing.
std::atomic<bool> g_armed{false};
std::atomic<bool> g_reported{false};

std::mutex               g_mu;         // the streamer can tick off a job thread
std::vector<Entry>       g_entries;    // declaration order — stable, greppable
std::string              g_runFlag, g_world, g_out;

Entry* find(const char* name) {   // caller holds g_mu
    for (auto& e : g_entries) if (e.name == name) return &e;
    return nullptr;
}

// Every manifest line carries the same prefix so one grep finds the whole
// verdict: `... 2>&1 | Select-String "\[capture\]"`.
void info(const std::string& s)  { x3::logInfo ("[capture] " + s); }
void loud(const std::string& s)  { x3::logError("[capture] !!! " + s); }

// Pad so the name column lines up without pulling in <iomanip>.
std::string pad(const std::string& s, size_t w) {
    return s.size() >= w ? s : s + std::string(w - s.size(), ' ');
}

// The static destructor catch-all. A capture path that exits somewhere other
// than the screenshot dispatch (the --world hosts and the default host both
// do) still gets its manifest, with no call site to remember. x3::log is a
// stateless fprintf+fflush, so it is safe this late in teardown.
struct ReportAtExit { ~ReportAtExit() { report(); } };
ReportAtExit g_reportAtExit;

} // namespace

void arm(const std::string& runFlag) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_runFlag = runFlag;
    g_armed.store(true, std::memory_order_relaxed);
}

bool armed() { return g_armed.load(std::memory_order_relaxed); }

void setWorld(const std::string& world) {
    std::lock_guard<std::mutex> lk(g_mu); g_world = world;
}
void setOutput(const std::string& outPath) {
    std::lock_guard<std::mutex> lk(g_mu); g_out = outPath;
}

void declare(const char* name, const char* consequence) {
    if (!armed() || !name) return;
    std::lock_guard<std::mutex> lk(g_mu);
    if (Entry* e = find(name)) { if (consequence) e->consequence = consequence; return; }
    Entry e; e.name = name; e.consequence = consequence ? consequence : "";
    g_entries.push_back(std::move(e));
}

void tick(const char* name) {
    if (!armed() || !name) return;
    std::lock_guard<std::mutex> lk(g_mu);
    // An undeclared tick still registers: better a nameless-consequence entry
    // in the ACTIVE list than a silent one.
    if (Entry* e = find(name)) { ++e->ticks; return; }
    Entry e; e.name = name; e.ticks = 1; g_entries.push_back(std::move(e));
}

void gate(const char* name, unsigned passed, unsigned blocked) {
    if (!armed() || !name) return;
    std::lock_guard<std::mutex> lk(g_mu);
    Entry* e = find(name);
    if (!e) { Entry n; n.name = name; g_entries.push_back(std::move(n)); e = &g_entries.back(); }
    e->passed  += passed;
    e->blocked += blocked;
    if (passed) ++e->ticks;   // something got through, so it "ran" this frame
}

void report() {
    if (!armed()) return;
    if (g_reported.exchange(true)) return;   // print exactly once per process

    std::lock_guard<std::mutex> lk(g_mu);

    std::vector<const Entry*> active, partial, dead;
    size_t nameW = 4;
    for (const auto& e : g_entries) {
        nameW = std::max(nameW, e.name.size());
        if (e.ticks == 0)                   dead.push_back(&e);
        else if (e.blocked > 0)             partial.push_back(&e);
        else                                active.push_back(&e);
    }

    info("================= CAPTURE MANIFEST =================");
    info("run=" + (g_runFlag.empty() ? std::string("(capture)") : g_runFlag) +
         "  world=" + (g_world.empty() ? std::string("(default)") : g_world) +
         (g_out.empty() ? std::string() : ("  out=" + g_out)));

    if (!active.empty()) {
        info("ACTIVE — these contributed to the captured frame(s):");
        for (const auto* e : active)
            info("  " + pad(e->name, nameW) + "  ran " + std::to_string(e->ticks) + "x" +
                 (e->passed ? ("  (" + std::to_string(e->passed) + " drawn)") : std::string()));
    }

    if (!partial.empty()) {
        x3::logWarn("[capture] PARTIAL — these were gated OFF some of the time:");
        for (const auto* e : partial)
            x3::logWarn("[capture]   " + pad(e->name, nameW) + "  " +
                        std::to_string(e->passed) + " drawn / " +
                        std::to_string(e->blocked) + " SUPPRESSED — " + e->consequence);
    }

    if (!dead.empty()) {
        loud("THIS CAPTURE DID NOT RENDER THE FOLLOWING.");
        loud("They were DECLARED by this binary and then NEVER RAN in the capture path:");
        for (const auto* e : dead)
            loud("  " + pad(e->name, nameW) + "  ABSENT — " +
                 (e->consequence.empty() ? std::string("(no consequence declared)") : e->consequence) +
                 (e->blocked ? ("  [gate blocked " + std::to_string(e->blocked) + "x, passed 0]")
                             : std::string()));
        loud("---------------------------------------------------");
        loud("ANY VISUAL CONCLUSION ABOUT THE ABOVE, DRAWN FROM THIS FRAME, IS MEANINGLESS.");
        loud("The frame does not show them missing from the GAME. It shows them missing");
        loud("from the CAPTURE. Absence here is NOT evidence of absence in play.");
        loud("If you need them in the still, see docs/CAPTURE_MANIFEST.md.");
    } else if (!g_entries.empty()) {
        info("NOTHING DECLARED WAS SKIPPED — every declared subsystem ran in this capture.");
    }

    // AN EMPTY MANIFEST IS NOT A CLEAN BILL OF HEALTH, and must never read as
    // one. This is the exact failure mode the manifest exists to kill, turned on
    // itself: a capture host where nobody has instrumented anything would
    // otherwise print "nothing was skipped" and sound reassuring while knowing
    // literally nothing. Say the true thing instead, at ERROR volume.
    if (g_entries.empty()) {
        loud("NO SUBSYSTEM DECLARED ITSELF IN THIS CAPTURE HOST.");
        loud("This manifest therefore tells you NOTHING about what is missing from");
        loud("this frame. It is NOT a clean bill of health — it is an uninstrumented");
        loud("capture rig. See docs/CAPTURE_MANIFEST.md to add declarations.");
    }

    info("manifest scope: " + std::to_string(g_entries.size()) +
         " subsystem(s) declared by THIS binary. A subsystem that never calls");
    info("x3::capture::declare() is NOT tracked here — this list is a floor, not a ceiling.");
    info("====================================================");
}

}} // namespace x3::capture
