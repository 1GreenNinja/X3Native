#include "engine/core/x3_boot.h"
#include "engine/core/x3_log.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

namespace x3::boot {
namespace {

using Clock = std::chrono::steady_clock;

// Process-start anchor: initialized during static init of the engine image
// (before main), so the timeline covers CRT/static-init time too.
const Clock::time_point g_start = Clock::now();

struct Mark { std::string phase; double totalMs; };

// Boot is single-threaded up to the first interactive frame; plain statics.
std::vector<Mark>& marks() { static std::vector<Mark> m; return m; }

} // namespace

double sinceStartMs() {
    return std::chrono::duration<double, std::milli>(Clock::now() - g_start).count();
}

void mark(const char* phase) {
    const double t    = sinceStartMs();
    const double prev = marks().empty() ? 0.0 : marks().back().totalMs;
    marks().push_back({ phase, t });
    char line[256];
    std::snprintf(line, sizeof(line), "[boot] %-38s +%8.1f ms  (t=%8.1f ms)",
                  phase, t - prev, t);
    logInfo(line);
}

double report(const char* totalLabel) {
    const double total = sinceStartMs();
    logInfo("[boot] ---- phase table ----");
    double prev = 0.0;
    for (const Mark& m : marks()) {
        char line[256];
        std::snprintf(line, sizeof(line), "[boot]   %-38s %8.1f ms",
                      m.phase.c_str(), m.totalMs - prev);
        logInfo(line);
        prev = m.totalMs;
    }
    char line[256];
    std::snprintf(line, sizeof(line), "[boot] TOTAL %s: %.1f ms", totalLabel, total);
    logInfo(line);
    return total;
}

} // namespace x3::boot
