#pragma once
// ===========================================================================
// PINNED CONSOLE TELEMETRY REGION — pure VT-sequence protocol (no OS calls).
//
// THE PROBLEM (Tim, 2026-08-16, verbatim): "Can we prevent these from
// scrolling away the boot messages? I want to read those.. so make an area
// where 3 lines of this can display, scrolling to their heart's content, and
// i can scroll up to the beginning of this console and see all the messages
// from boot."
//
// THE DESIGN — the OS terminal is split into two streams:
//   * a pinned 6-row block at the BOTTOM:
//       row 0..2  telemetry rows — high-frequency categories ([perf]/[pacing])
//                 repaint IN PLACE, never entering scrollback; dim per-slot
//                 colors so the region reads as an instrument panel
//       row 3..5  a rounded-border framed typed-command input line
//                     ╭──────────────────────────────╮
//                     │ ❯ r_csm 1                    │
//                     ╰──────────────────────────────╯
//   * everything else (boot, warnings, errors, world events) scrolls normally
//     ABOVE the block — strictly default-styled so logs stay grep-clean — and
//     scrolling up reads the full history from boot.
//
// WHY NOT DECSTBM (ESC[t;br scroll margins)? Because on Windows Terminal and
// conhost, lines scrolled off the top of a PARTIAL scroll region are DISCARDED
// — they never enter the scrollback buffer. That silently destroys the exact
// thing Tim asked to keep. Instead this uses the classic "sticky footer"
// protocol (same family as cargo/npm progress lines): every normal line first
// wipes the block, prints itself where the block began (so any screen scroll
// it causes is a FULL-screen scroll, which DOES feed scrollback in every
// terminal), then repaints the block below itself.
//
// THE PARK INVARIANT — after EVERY emission the cursor rests at the CARET
// position inside the input row (block row 4), and NO emission leaves the
// cursor with a pending '\n' on the screen's bottom row. All row math is
// RELATIVE ("\r" + CSI nA/nB from the park position) so the protocol needs no
// absolute row addressing and survives window resizes; only column addressing
// (CSI nG) is absolute, which is resize-safe. The clip width is re-queried by
// the caller before each paint.
//
// GLYPHS — rounded box-drawing (U+256D..U+2570) + the ❯ prompt need the
// console output codepage at UTF-8 (the OS layer calls SetConsoleOutputCP);
// when that fails the protocol degrades to an ASCII frame (+---+, |, >) —
// degrade, never garble.
//
// This header is PURE (string in, escape-bytes out) so the x3_conregion_probe
// gate can assert the emitted sequences without a live console. The OS side
// (VT enable, redirect detection, codepage, stdin thread, 10 Hz rate limit,
// locking) lives in x3_log.cpp.
// ===========================================================================
#include <string>
#include <string_view>

namespace x3::conregion {

// --- Telemetry classification (data-driven; ONE list) -----------------------
// A log message whose text starts with one of these prefixes is telemetry: it
// renders on its pinned row instead of scrolling. New spammy category = one
// entry here. Slot is the row (0..2); slot 2 is the shared spare row for any
// future category that has no dedicated row of its own.
struct TelemetryTag {
    std::string_view prefix;
    int              slot;
};
inline constexpr TelemetryTag kTelemetryTags[] = {
    { "[perf]",   0 },
    { "[pacing]", 1 },
    // slot 2 = spare (route the next chatty category here)
};
inline constexpr int kTelemetryRows = 3;   // pinned telemetry rows
inline constexpr int kInputRow      = 4;   // caret row (border above at 3, below at 5)
inline constexpr int kBlockRows     = 6;   // 3 telemetry + 3-row framed input line

// Returns the pinned row for a telemetry message, or -1 for a normal line.
inline int telemetrySlot(std::string_view msg) {
    for (const auto& t : kTelemetryTags)
        if (msg.size() >= t.prefix.size() &&
            msg.compare(0, t.prefix.size(), t.prefix) == 0)
            return t.slot;
    return -1;
}

// --- The protocol -----------------------------------------------------------
class LogRegionProto {
public:
    // Console width in columns; every block row is built to width-1 columns so
    // nothing can ever auto-wrap (a wrapped block row breaks the row math).
    void setWidth(int w) { m_width = (w > 16) ? w : 16; }
    int  width() const { return m_width; }

    // UTF-8 rounded frame vs ASCII fallback (legacy conhost / codepage failure).
    void setUtf8(bool on) { m_utf8 = on; }
    bool utf8() const { return m_utf8; }

    bool live() const { return m_live; }

    // First activation: lay down the (empty) block at the current cursor
    // position. After this the park invariant holds.
    std::string bootstrap() {
        m_live = true;
        std::string out;
        paintRows(out);
        return out;
    }

    // A normal (scrolling) line. `rendered` is the full display line WITHOUT a
    // trailing newline (e.g. "[INFO]  [boot] device ready"). It scrolls into
    // history above the block; the block repaints below it.
    std::string normalLine(std::string_view rendered) {
        std::string out;
        toBlockTop(out);
        out += "\x1b[0J";            // wipe the old block (cursor stays put)
        out += rendered;
        out += '\n';
        paintRows(out);
        return out;
    }

    // Update a telemetry slot. Paint only when the caller says so (the OS layer
    // owns the 10 Hz rate limit); an empty return means "stored, not painted".
    std::string telemetry(int slot, std::string_view rendered, bool paint) {
        if (slot >= 0 && slot < kTelemetryRows) m_slots[slot] = rendered;
        if (!paint || !m_live) return std::string();
        return repaint();
    }

    // Repaint the whole block in place (no scroll, no history pollution).
    std::string repaint() {
        std::string out;
        toBlockTop(out);
        paintRows(out);
        return out;
    }

    // Typed-command input buffer. setInput replaces it and repaints ONLY the
    // input row — keystrokes never fight the telemetry repaint cadence.
    std::string setInput(std::string_view buf) {
        m_input = buf;
        if (!m_live) return std::string();
        std::string out = "\r\x1b[2K";
        paintInputText(out);
        parkCaret(out);
        return out;
    }
    const std::string& input() const { return m_input; }

    // Park the shell prompt on a fresh line below the block and reset colors.
    std::string shutdown() {
        if (!m_live) return std::string();
        m_live = false;
        // caret sits on the input row: drop past the bottom border, then a
        // fresh line for the shell prompt (block stays visible above it).
        return std::string("\x1b[0m\x1b[")
             + std::to_string(kBlockRows - 1 - kInputRow) + "B\r\n";
    }

private:
    // --- palette (region-only; the scrolling history stays default-styled) --
    static constexpr const char* kReset   = "\x1b[0m";
    static constexpr const char* kBorder  = "\x1b[38;2;110;110;110m";  // dim gray frame
    static constexpr const char* kPrompt  = "\x1b[38;2;80;200;255m";   // cyan caret marker
    static constexpr const char* kTyped   = "\x1b[38;2;235;235;235m";  // bright typed text
    static const char* slotColor(int i) {
        switch (i) {
            case 0:  return "\x1b[38;2;96;168;176m";   // [perf]   dim cyan
            case 1:  return "\x1b[38;2;176;156;88m";   // [pacing] dim yellow
            default: return "\x1b[38;2;128;128;128m";  // spare    dim gray
        }
    }

    // --- glyphs -------------------------------------------------------------
    const char* gTL() const { return m_utf8 ? "╭" : "+"; }   // ╭
    const char* gTR() const { return m_utf8 ? "╮" : "+"; }   // ╮
    const char* gBL() const { return m_utf8 ? "╰" : "+"; }   // ╰
    const char* gBR() const { return m_utf8 ? "╯" : "+"; }   // ╯
    const char* gH()  const { return m_utf8 ? "─" : "-"; }   // ─
    const char* gV()  const { return m_utf8 ? "│" : "|"; }   // │
    const char* gPr() const { return m_utf8 ? "❯" : ">"; }   // ❯

    // Display columns of a UTF-8 string (all glyphs used here are 1 column).
    static int cols(std::string_view s) {
        int n = 0;
        for (unsigned char c : s) if ((c & 0xC0) != 0x80) ++n;
        return n;
    }

    int frameW() const { return m_width - 1; }   // total columns the frame uses

    // From the park position (caret on the input row) to block top, column 1.
    void toBlockTop(std::string& out) const {
        out += '\r';
        out += "\x1b[";
        out += std::to_string(kInputRow);
        out += 'A';
    }

    // Paint all rows downward from the current cursor row (must be block top).
    // Exactly kBlockRows-1 newlines, NONE after the last row, then the cursor
    // climbs back to the caret (park invariant).
    void paintRows(std::string& out) const {
        for (int i = 0; i < kTelemetryRows; ++i) {
            out += "\x1b[2K";
            out += slotColor(i);
            out += clip(m_slots[i], frameW());
            out += kReset;
            out += '\n';
        }
        out += "\x1b[2K";
        hRule(out, gTL(), gTR());
        out += '\n';
        out += "\x1b[2K";
        paintInputText(out);
        out += '\n';
        out += "\x1b[2K";
        hRule(out, gBL(), gBR());
        out += "\x1b[1A";            // back up to the input row...
        parkCaret(out);              // ...and onto the caret column
    }

    void hRule(std::string& out, const char* l, const char* r) const {
        out += kBorder;
        out += l;
        for (int i = 0; i < frameW() - 2; ++i) out += gH();
        out += r;
        out += kReset;
    }

    // "│ ❯ typed-text            │" — text keeps its TAIL visible when long.
    void paintInputText(std::string& out) const {
        const int innerW = frameW() - 2;      // between the two │
        const int room   = innerW - 3;        // after " ❯ "
        std::string shown = m_input;
        if ((int)shown.size() > room && room > 0)
            shown = shown.substr(shown.size() - (size_t)room);
        out += kBorder; out += gV(); out += kReset;
        out += ' ';
        out += kPrompt; out += gPr(); out += kReset;
        out += ' ';
        out += kTyped; out += shown; out += kReset;
        for (int i = cols(shown); i < room; ++i) out += ' ';
        out += kBorder; out += gV(); out += kReset;
    }

    // Caret column: │(1) + space(1) + ❯(1) + space(1) = text starts col 5.
    void parkCaret(std::string& out) const {
        const int room = frameW() - 2 - 3;
        int shownCols = cols(m_input);
        if (shownCols > room && room > 0) shownCols = room;
        out += "\x1b[";
        out += std::to_string(5 + shownCols);
        out += 'G';
    }

    static std::string clip(const std::string& s, int maxCols) {
        if (maxCols < 1) maxCols = 1;
        if (cols(s) <= maxCols) return s;
        // walk to the byte offset of the maxCols-th column (UTF-8 aware)
        int n = 0;
        size_t i = 0;
        for (; i < s.size(); ++i)
            if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80 && ++n > maxCols) break;
        return s.substr(0, i);
    }

    bool        m_live = false;
    bool        m_utf8 = true;
    int         m_width = 120;
    std::string m_slots[kTelemetryRows];
    std::string m_input;
};

// --- OS-layer API (implemented in x3_log.cpp) --------------------------------
// Typed terminal commands: the stdin reader thread queues submitted lines; the
// HOST drains them on the MAIN thread (IConsole::exec is not thread-safe) and
// feeds them to the SAME dispatcher the in-game dev-shell console uses.
// Returns false when the queue is empty. Always safe to call (headless runs
// simply never queue anything).
bool popInput(std::string& out);

} // namespace x3::conregion
