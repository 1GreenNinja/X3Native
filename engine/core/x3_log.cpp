#include "x3_log.h"
#include "x3_log_region.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

// ===========================================================================
// The logging sink. Two personalities, chosen ONCE at first log call:
//
//  * PLAIN (stdout is redirected / a pipe / headless CI, or X3_LOG_REGION=0):
//    the historical behavior, BYTE-IDENTICAL — "[TAG]  msg\n" via fprintf,
//    errors on stderr, fflush after every line. Every --smoketest/--test-*
//    gate in the repo parses this format; it MUST NOT change.
//
//  * REGION (stdout is a live console and VT processing enables): the pinned
//    bottom block from x3_log_region.h — 3 in-place telemetry rows + a typed
//    command input row — with everything else scrolling cleanly above it.
//    Telemetry repaints are rate-limited to ~10 Hz so the perf display can
//    never become a perf problem. A stdin reader thread (console-stdin only)
//    feeds typed lines to x3::conregion::popInput() for the host's main loop
//    to exec on the shared engine console.
// ===========================================================================

namespace {

using Clock = std::chrono::steady_clock;
constexpr auto kMinRepaintInterval = std::chrono::milliseconds(100);   // ~10 Hz

const char* levelTag(x3::LogLevel level) {
    switch (level) {
        case x3::LogLevel::Warn:  return "[WARN] ";
        case x3::LogLevel::Error: return "[ERROR]";
        default:                  return "[INFO] ";
    }
}

#ifdef _WIN32

struct RegionState {
    bool                     active = false;      // region protocol live on stdout
    HANDLE                   hOut = INVALID_HANDLE_VALUE;
    HANDLE                   hIn  = INVALID_HANDLE_VALUE;
    DWORD                    outModeOrig = 0;
    DWORD                    inModeOrig  = 0;
    UINT                     cpOrig = 0;
    bool                     restoreCp = false;
    bool                     restoreIn = false;
    bool                     errIsConsole = false; // route errors through the block
    x3::conregion::LogRegionProto proto;
    std::mutex               mx;
    std::deque<std::string>  cmdQueue;
    Clock::time_point        lastPaint{};
    bool                     telemetryDirty = false;

    RegionState() { init(); }
    ~RegionState() { shutdown(); }

    void init() {
        const char* kill = std::getenv("X3_LOG_REGION");
        if (kill && kill[0] == '0') return;

        hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        if (hOut == INVALID_HANDLE_VALUE || !GetConsoleMode(hOut, &mode))
            return;   // redirected / headless -> PLAIN, byte-compatible
        outModeOrig = mode;
        if (!SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING))
            return;   // ancient conhost without VT -> PLAIN

        // Rounded box-drawing + the ❯ prompt need UTF-8 output; verify the
        // codepage actually took, else the frame degrades to ASCII (+---+, |,
        // >) — degrade, never garble (legacy conhost rule).
        cpOrig = GetConsoleOutputCP();
        restoreCp = (SetConsoleOutputCP(CP_UTF8) != 0);
        proto.setUtf8(restoreCp && GetConsoleOutputCP() == CP_UTF8);

        proto.setWidth(queryWidth());
        HANDLE hErr = GetStdHandle(STD_ERROR_HANDLE);
        DWORD em = 0;
        errIsConsole = (hErr != INVALID_HANDLE_VALUE && GetConsoleMode(hErr, &em));

        active = true;
        emit(proto.bootstrap());

        startInputThread();
    }

    void shutdown() {
        std::lock_guard<std::mutex> lk(mx);
        if (!active) return;
        emit(proto.shutdown());
        SetConsoleMode(hOut, outModeOrig);
        if (restoreCp) SetConsoleOutputCP(cpOrig);
        if (restoreIn) SetConsoleMode(hIn, inModeOrig);
        active = false;
        // stdin thread is detached and blocked in ReadConsoleInputW; process
        // teardown reclaims it. It only touches state under mx and checks
        // `active` first, so post-shutdown keystrokes are ignored.
    }

    int queryWidth() const {
        CONSOLE_SCREEN_BUFFER_INFO sbi{};
        if (GetConsoleScreenBufferInfo(hOut, &sbi))
            return (int)(sbi.srWindow.Right - sbi.srWindow.Left + 1);
        return 120;
    }

    // Single write + flush so each protocol emission hits the console atomically.
    void emit(const std::string& bytes) {
        if (bytes.empty()) return;
        std::fwrite(bytes.data(), 1, bytes.size(), stdout);
        std::fflush(stdout);
    }

    // --- normal scrolling line (callers hold mx) ---------------------------
    void writeNormal(x3::LogLevel level, std::string_view msg) {
        // The scrolling history stays STRICTLY default-styled (no SGR) so it
        // reads/greps exactly like the redirected log; only the pinned block
        // below carries color.
        std::string line = levelTag(level);
        line += ' ';
        line.append(msg.data(), msg.size());
        proto.setWidth(queryWidth());
        emit(proto.normalLine(line));
        lastPaint = Clock::now();     // normalLine repainted the block too
        telemetryDirty = false;
        // A redirected stderr (`2> err.log`) still captures errors even though
        // the visible copy went through the block on stdout.
        if (level == x3::LogLevel::Error && !errIsConsole) {
            std::fprintf(stderr, "%s %.*s\n", levelTag(level),
                         (int)msg.size(), msg.data());
            std::fflush(stderr);
        }
    }

    // --- telemetry line (callers hold mx) ----------------------------------
    void writeTelemetry(int slot, x3::LogLevel level, std::string_view msg) {
        std::string line = levelTag(level);
        line += ' ';
        line.append(msg.data(), msg.size());
        const auto now = Clock::now();
        const bool paint = (now - lastPaint) >= kMinRepaintInterval;
        if (paint) {
            proto.setWidth(queryWidth());
            lastPaint = now;
            telemetryDirty = false;
        } else {
            telemetryDirty = true;
        }
        emit(proto.telemetry(slot, line, paint));
    }

    void flushDirtyTelemetry() {
        const auto now = Clock::now();
        if (!telemetryDirty || (now - lastPaint) < kMinRepaintInterval) return;
        lastPaint = now;
        telemetryDirty = false;
        emit(proto.repaint());
    }

    // --- typed command input row -------------------------------------------
    void startInputThread() {
        hIn = GetStdHandle(STD_INPUT_HANDLE);
        DWORD im = 0;
        if (hIn == INVALID_HANDLE_VALUE || !GetConsoleMode(hIn, &im))
            return;   // stdin redirected/closed -> no input row thread
        inModeOrig = im;
        // Raw-ish: we own echo + editing (the input row repaints itself), but
        // keep PROCESSED_INPUT so Ctrl+C still terminates the process.
        SetConsoleMode(hIn, (im & ~(DWORD)(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT))
                               | ENABLE_PROCESSED_INPUT);
        restoreIn = true;
        std::thread([this] { inputLoop(); }).detach();
    }

    void inputLoop() {
        std::string buf;
        for (;;) {
            INPUT_RECORD rec;
            DWORD n = 0;
            if (!ReadConsoleInputW(hIn, &rec, 1, &n) || n == 0) return;
            if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown)
                continue;
            const auto& ke = rec.Event.KeyEvent;
            std::lock_guard<std::mutex> lk(mx);
            if (!active) return;
            for (WORD r = 0; r < ke.wRepeatCount; ++r) {
                if (ke.wVirtualKeyCode == VK_RETURN) {
                    if (buf.empty()) continue;
                    // Echo into the scrolling history so it reads like a
                    // session log, queue for the host's main-thread exec.
                    std::string cmd = buf;
                    buf.clear();
                    proto.setWidth(queryWidth());
                    emit(proto.normalLine("> " + cmd));   // session-log echo, unstyled
                    lastPaint = Clock::now();
                    telemetryDirty = false;
                    cmdQueue.push_back(std::move(cmd));
                } else if (ke.wVirtualKeyCode == VK_BACK) {
                    if (!buf.empty()) buf.pop_back();
                    emit(proto.setInput(buf));
                } else {
                    const wchar_t ch = ke.uChar.UnicodeChar;
                    if (ch >= 32 && ch < 127) {   // ASCII v1; enough for cvars
                        buf.push_back((char)ch);
                        emit(proto.setInput(buf));
                    }
                }
            }
        }
    }

    bool popCommand(std::string& out) {
        std::lock_guard<std::mutex> lk(mx);
        if (!active) return false;
        flushDirtyTelemetry();   // main loop heartbeat: settle a stale row
        if (cmdQueue.empty()) return false;
        out = std::move(cmdQueue.front());
        cmdQueue.pop_front();
        return true;
    }
};

RegionState& regionState() {
    static RegionState s;   // first log call constructs; dtor restores console
    return s;
}

#endif // _WIN32

} // namespace

namespace x3 {

void log(LogLevel level, std::string_view msg) {
#ifdef _WIN32
    RegionState& rs = regionState();
    if (rs.active) {
        std::lock_guard<std::mutex> lk(rs.mx);
        if (rs.active) {   // re-check: shutdown() may have raced the lock
            const int slot = conregion::telemetrySlot(msg);
            if (slot >= 0) rs.writeTelemetry(slot, level, msg);
            else           rs.writeNormal(level, msg);
            return;
        }
    }
#endif
    // PLAIN path — the historical sink, byte-for-byte. Every headless gate
    // (--smoketest, --test-*) parses this; do not reformat it.
    const char* tag = levelTag(level);
    std::FILE* out = (level == LogLevel::Error) ? stderr : stdout;
    std::fprintf(out, "%s %.*s\n", tag, static_cast<int>(msg.size()), msg.data());
    std::fflush(out); // flush so output survives redirection + forced exit
}

} // namespace x3

namespace x3::conregion {

bool popInput(std::string& out) {
#ifdef _WIN32
    return regionState().popCommand(out);
#else
    (void)out;
    return false;
#endif
}

} // namespace x3::conregion
