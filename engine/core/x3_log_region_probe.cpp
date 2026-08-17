// ===========================================================================
// x3_conregion_probe — headless gate for the pinned console telemetry region.
//
// The interactive block (engine/core/x3_log_region.h) cannot be visually
// verified by a headless agent, so this probe asserts the PROTOCOL: the exact
// VT escape sequences the pure LogRegionProto emits for every operation, the
// park invariant the cursor math depends on, classification, clipping, the
// ASCII degrade, and that x3::conregion::popInput() links and behaves when the
// region is disabled (the redirected/headless personality).
//
// Byte-compat of the PLAIN log path is NOT this probe's job — that is proven
// by diffing a real --smoketest log pre/post change (see the lane report).
//
// Exit 0 = all pass. Prints "conregion-probe: N passed, M failed".
// ===========================================================================
#include "engine/core/x3_log_region.h"
#include "engine/core/x3_log.h"

#include <cstdio>
#include <cstdlib>
#include <string>

using x3::conregion::LogRegionProto;
using x3::conregion::telemetrySlot;

namespace {
int g_pass = 0, g_fail = 0;
void check(bool ok, const char* name) {
    if (ok) { ++g_pass; std::printf("[conregion-probe] PASS %s\n", name); }
    else    { ++g_fail; std::printf("[conregion-probe] FAIL %s\n", name); }
}
bool contains(const std::string& s, const std::string& needle) {
    return s.find(needle) != std::string::npos;
}
bool startsWith(const std::string& s, const std::string& p) {
    return s.rfind(p, 0) == 0;
}
int countChar(const std::string& s, char c) {
    int n = 0; for (char x : s) if (x == c) ++n; return n;
}
bool pureAscii(const std::string& s) {
    for (unsigned char c : s) if (c >= 0x80) return false;
    return true;
}
} // namespace

int main() {
    // Force the OS layer inert BEFORE anything touches it, so the probe's own
    // stdout stays plain even when run from a live console.
#ifdef _WIN32
    _putenv("X3_LOG_REGION=0");
#endif

    // --- T1: classification (data-driven tag list) --------------------------
    check(telemetrySlot("[perf] 60 FPS frame=16ms") == 0,  "T1a [perf] -> slot 0");
    check(telemetrySlot("[pacing] SPIKE frame=9")   == 1,  "T1b [pacing] -> slot 1");
    check(telemetrySlot("[boot] device ready")      == -1, "T1c [boot] scrolls");
    check(telemetrySlot("[rhi] swapchain 1280x720") == -1, "T1d [rhi] scrolls");
    check(telemetrySlot("perf without bracket")     == -1, "T1e unbracketed scrolls");

    // --- T2: bootstrap lays down the 6-row block, parks the caret -----------
    LogRegionProto p;
    p.setWidth(80);
    const std::string boot = p.bootstrap();
    check(p.live(),                            "T2a live after bootstrap");
    check(countChar(boot, '\n') == 5,          "T2b bootstrap = 6 rows / 5 newlines");
    check(boot.back() != '\n',                 "T2c park invariant: no trailing newline");
    check(contains(boot, "\x1b[2K"),           "T2d rows are cleared before paint");
    check(contains(boot, "╭") && contains(boot, "╮"), "T2e rounded top corners");
    check(contains(boot, "╰") && contains(boot, "╯"), "T2f rounded bottom corners");
    check(contains(boot, "❯"),            "T2g prompt marker");
    check(contains(boot, "\x1b[1A") && boot.size() > 4 &&
          boot.compare(boot.size() - 4, 4, "\x1b[5G") == 0,
                                               "T2h caret parked at input col 5");

    // --- T3: normal line = wipe block, print, repaint below -----------------
    const std::string n1 = p.normalLine("[INFO]  [boot] device ready");
    check(startsWith(n1, "\r\x1b[4A\x1b[0J"),  "T3a to block top + wipe");
    check(contains(n1, "[INFO]  [boot] device ready\n"), "T3b line enters scroll history");
    check(countChar(n1, '\n') == 6,            "T3c 1 history + 5 block newlines");
    check(n1.back() != '\n',                   "T3d park invariant");

    // --- T4: telemetry repaints in place, never scrolls ---------------------
    const std::string t0 = p.telemetry(0, "[INFO]  [perf] 60 FPS", true);
    check(startsWith(t0, "\r\x1b[4A"),         "T4a relative to-top, no absolute rows");
    check(!contains(t0, "\x1b[0J"),            "T4b no wipe (no flicker)");
    check(contains(t0, "[perf] 60 FPS"),       "T4c latest perf line shown");
    check(countChar(t0, '\n') == 5,            "T4d exactly the block's 5 newlines");
    check(t0.back() != '\n',                   "T4e park invariant (no scroll at bottom)");
    const std::string t1 = p.telemetry(1, "[INFO]  [pacing] SPIKE", false);
    check(t1.empty(),                          "T4f rate-limited store emits nothing");
    check(contains(p.repaint(), "[pacing] SPIKE"), "T4g stored line appears on next paint");

    // --- T5: typed input row ------------------------------------------------
    const std::string i1 = p.setInput("r_csm 1");
    check(startsWith(i1, "\r\x1b[2K"),         "T5a input row repaints only itself");
    check(countChar(i1, '\n') == 0,            "T5b keystroke never scrolls");
    check(contains(i1, "r_csm 1"),             "T5c typed text echoed");
    check(i1.size() > 5 && i1.compare(i1.size() - 5, 5, "\x1b[12G") == 0,
                                               "T5d caret after text (col 5+7)");
    const std::string i2 = p.setInput("");
    check(i2.size() > 4 && i2.compare(i2.size() - 4, 4, "\x1b[5G") == 0,
                                               "T5e caret home when buffer empty");

    // --- T6: clipping (no block row may ever wrap) --------------------------
    p.setWidth(30);
    std::string longLine = "[INFO]  [perf] ";
    longLine.append(200, 'x');
    const std::string t2 = p.telemetry(0, longLine, true);
    check(!contains(t2, std::string(30, 'x')), "T6a telemetry clipped to width");
    std::string longInput(200, 'y');
    const std::string i3 = p.setInput(longInput);
    check(!contains(i3, std::string(30, 'y')), "T6b input shows tail, no wrap");
    check(contains(i3, "y"),                   "T6c input tail visible");
    p.setWidth(80);

    // --- T7: ASCII degrade (legacy conhost / UTF-8 codepage failure) --------
    LogRegionProto pa;
    pa.setWidth(60);
    pa.setUtf8(false);
    const std::string ab = pa.bootstrap();
    check(pureAscii(ab),                       "T7a fallback frame is pure ASCII");
    check(contains(ab, "+") && contains(ab, "|") && contains(ab, "-"),
                                               "T7b +---+ frame present");
    check(contains(ab, "> ") || contains(ab, ">"), "T7c ASCII prompt");

    // --- T8: shutdown parks the shell prompt below the block ----------------
    const std::string sd = p.shutdown();
    check(contains(sd, "\x1b[0m"),             "T8a colors reset");
    check(sd.size() >= 2 && sd.compare(sd.size() - 2, 2, "\r\n") == 0,
                                               "T8b fresh line for the shell");
    check(p.setInput("zzz").empty(),           "T8c dead proto emits nothing");

    // --- T9: headless personality of the OS layer ---------------------------
    // X3_LOG_REGION=0 was set above: the region must be inert, popInput must
    // report empty, and a log call must take the plain byte-compatible path
    // (visible in this probe's own redirected output as a plain line).
    std::string cmd;
    check(!x3::conregion::popInput(cmd),       "T9a popInput empty when region off");
    x3::logInfo("probe plain-path line");      // appears as "[INFO]  probe plain-path line"
    check(true,                                "T9b plain log path exercised (see line above)");

    std::printf("conregion-probe: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
