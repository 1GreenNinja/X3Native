// Holographic terminal — see app/holo_terminal.h.
//
// This is the FLAGSHIP VARIANT of the holo platform (app/holo_panel.h), not a rival
// implementation. The fixture — black glass, chrome round-pipe frame, ceiling support
// pipe — is a HoloPanel. The line-art rasterizer, the font and the palette are the
// shared holo:: toolkit. What lives HERE is what makes a TERMINAL a terminal: the
// security-console content bake, and the readout/input state machine.
//
// If you are adding a new glowing screen: add a content baker, not a second terminal.
#include "holo_terminal.h"
#include "holo_panel.h"

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <string>
#include <vector>

namespace x3::game {

using namespace holo;

namespace {

// ---------------------------------------------------------------------------
// THE SECURITY-CONSOLE CONTENT BAKE.
//
// A crisp glowing LINE-ART HUD printed on black glass: a bracket frame, a header
// rule with a hexagon emblem, a left data column, a center schematic node (Cell
// layout), a right column of warning icons and value bars, and a bottom data strip.
// The readout text is rasterized straight in, so it lives ON the glass and tilts
// with the panel.
//
// COMPOSITION LAW — READABILITY BEATS DECORATION (the owner's note: "the way it
// conflicts with the graphics behind the text"). The old bake drew decorative
// line-art and glowing text ADDITIVELY into the same pixels, so every tick-mark
// under a glyph ADDED to it and ate its contrast. Worse, the "fine data text" tick
// paragraph was drawn down the SAME left column the body rows occupy — decoration
// printed straight through the readout. Three rules now hold, and the bake is
// ordered to enforce them:
//   1. Decoration keeps to the MARGINS. The tick paragraph is GONE: it was filler
//      for a column that now carries real text.
//   2. Text gets its OWN ZONE — a quiet, dark band (holo::quietBand) knocked into
//      whatever line-art remains underneath, feathered so it reads as a deliberate
//      inset rather than a hole.
//   3. Text is drawn LAST and HOT, so nothing can ever be composited over it.
// ---------------------------------------------------------------------------
std::vector<uint8_t> makeHologramRGBA(uint32_t n,
                                      const std::vector<std::string>& lines,
                                      const std::string& inputLine,
                                      const float* inkOverride = nullptr,
                                      bool wide = false) {
    const float fn = (float)n;
    auto P = [&](float f) { return f * fn; };

    // Right edge of the text column — it runs up to the right-hand icon/bar column in
    // BOTH layouts.
    //
    // It used to stop at 0.345 in the Cell layout: a 27%-wide gutter, because a
    // decorative CENTER SCHEMATIC owned the middle of the glass. That column could not
    // hold a line like "MUSCULOSKELETAL OUTPUT: +400%", and since the fit pass shrinks
    // the type until the LONGEST row fits, one long line dragged every other row down to
    // the minimum size. The result was a crisp header over a block of unreadable
    // 18-pixel mush — decoration winning an argument it should never have been in.
    // The schematic is gone (see below) and the text gets the space. Readability beats
    // decoration: the panel exists to be READ, at [E] range.
    const float kTextX1 = wide ? 0.655f : 0.620f;

    Canvas c(n);
    blackGlassBase(c);

    const float th  = std::max(1.4f, fn / 512.0f);
    const float thh = th * 1.5f;

    // ---- LINE-ART (margins only). --------------------------------------------
    bracketFrame(c, P(0.045f), P(0.05f), P(0.955f), P(0.95f),
                 P(0.075f), P(0.055f), kBlue, 0.95f, thh);

    // Header: hexagon emblem + rule. The TITLE text is drawn later, over a quiet strip.
    const float hdrY = P(0.135f);
    hexagon(c, P(0.085f), P(0.092f), P(0.030f), kBlueHi, 1.0f, thh);
    line(c, P(0.085f)-P(0.012f), P(0.092f), P(0.085f)+P(0.012f), P(0.092f), kBlueHi, 0.9f, th);
    line(c, P(0.130f), hdrY, P(0.92f), hdrY, kBlueHi, 1.0f, thh);
    line(c, P(0.130f), hdrY+P(0.010f), P(0.55f), hdrY+P(0.010f), kBlue, 0.5f, th);

    // Left column: three icon squares. They sit ABOVE the first body row (which starts
    // at 0.258) — they never print through it.
    const float lx0 = P(0.075f), lx1 = P(kTextX1);
    for (int i = 0; i < 3; ++i) {
        const float ix = lx0 + i * P(0.055f);
        rectFrame(c, ix, P(0.20f), ix + P(0.035f), P(0.235f), kBlue, 0.9f, th);
        if (i == 1) line(c, ix, P(0.20f), ix + P(0.035f), P(0.235f), kBlue, 0.7f, th);
    }
    // (The decorative tick-mark paragraph that used to fill x[lx0..lx1], y[0.275..0.86]
    //  is DELETED. That is precisely the rectangle the body rows live in. It was
    //  "fine data text" filler for a column with nothing to say; the column has real
    //  text now, and filler that prints through a readout is not decoration, it is damage.)

    // Faint divider closing the text column.
    line(c, lx1 + P(0.015f), P(0.19f), lx1 + P(0.015f), P(0.87f), kBlue, 0.25f, th);

    // (The CENTER SCHEMATIC — a rounded node with branching spurs and a chevron, drawn
    //  at x[0.43..0.63] — is DELETED. It was pure decoration sitting exactly where the
    //  readout needed to be, and it is the reason the cell's text column was squeezed to
    //  27% of the glass. The panel is not poorer for it: the bracket frame, the hexagon
    //  emblem, the header rules, the warning triangles, the value bars and the bottom
    //  data strip all still read as a dense sci-fi console — and they live in the
    //  MARGINS, where decoration belongs.)

    // Right column: warning triangles (ORANGE — they are warnings) + value bars.
    const float rx0 = P(0.70f), rx1 = P(0.92f);
    for (int i = 0; i < 3; ++i)
        warnTriangle(c, rx0 + i * P(0.075f) + P(0.030f), P(0.205f), P(0.030f), P(0.052f),
                     kOrange, 0.95f, th);
    {
        float fy = P(0.33f);
        const float fieldH = P(0.06f);
        const float barLens[3] = { 0.62f, 0.40f, 0.85f };
        for (int i = 0; i < 3; ++i) {
            line(c, rx0, fy, rx0 + P(0.06f), fy, kBlue, 0.6f, th);
            const float barY = fy + P(0.014f);
            rectFrame(c, rx0, barY, rx1, barY + P(0.018f), kBlue, 0.4f, th);
            const float fillX = rx0 + (rx1 - rx0) * barLens[i];
            // The bars are STATUS: the two healthy ones read green, the low one orange.
            rectFill(c, rx0+th, barY+th, fillX, barY + P(0.018f) - th,
                     (barLens[i] < 0.5f) ? kOrange : kGreen, 0.85f);
            fy += fieldH;
        }
        rectFill(c, rx1 - P(0.030f), fy + P(0.005f), rx1, fy + P(0.035f), kGreen, 1.0f);
        line(c, rx0, fy + P(0.020f), rx1 - P(0.045f), fy + P(0.020f), kBlue, 0.5f, th);
    }

    // Bottom coded data strip.
    {
        const float by = P(0.875f);
        line(c, P(0.075f), by - P(0.014f), P(0.92f), by - P(0.014f), kBlue, 0.6f, th);
        float dx = P(0.075f);
        int k = 0;
        while (dx < P(0.92f)) {
            const float dlen = (k % 3 == 0) ? P(0.020f) : ((k % 3 == 1) ? P(0.008f) : P(0.013f));
            line(c, dx, by, dx + dlen, by, kBlue, (k % 4 == 0) ? 0.95f : 0.55f, thh);
            dx += dlen + P(0.009f);
            ++k;
        }
    }

    // ---- READOUT TEXT: measure -> QUIET THE ZONE -> draw hot. -----------------
    if (fontReady() && !lines.empty()) {
        const float HOT = 1.0f;

        // -- Metrics first (we must know the zone before we can quiet it). --
        const float lx0b   = P(0.075f);
        const float zoneW  = P(kTextX1) - lx0b;
        const float tyTop  = P(0.258f);            // below the header strip + icon squares

        // -- FIT BY WRAPPING, NOT BY SHRINKING. ---------------------------------
        // The old pass only ever SHRANK: it walked the rows and scaled the type down
        // until the LONGEST one fitted the column. So a single long row (the canon
        // readout has "MUSCULOSKELETAL OUTPUT: +400%") dragged EVERY row down with it,
        // and the whole readout bottomed out at the minimum size. A terminal wraps its
        // text — it does not set it in 6-point. So: pick a size the player can read from
        // [E] range, WRAP to the column, and only shrink if the wrapped block is too
        // TALL for the glass. Type size is now driven by the panel, not by the worst line.
        const float vBudget = P(0.860f) - tyTop;
        auto wrapTo = [&](float px, std::vector<std::pair<std::string, Ink>>& out) {
            out.clear();
            for (size_t li = 1; li < lines.size(); ++li) {
                const Ink k = inkOverride
                    ? Ink{ inkOverride[0] * 1.15f, inkOverride[1], inkOverride[2] }
                    : statusInk(lines[li]);
                const std::string& s = lines[li];
                if (s.empty()) { out.push_back({ "", k }); continue; }
                // Greedy word wrap. The status colour is chosen from the WHOLE line, so a
                // wrapped continuation keeps its parent's colour (a row does not change
                // meaning halfway through).
                std::string cur;
                size_t i = 0;
                while (i < s.size()) {
                    size_t j = s.find(' ', i);
                    if (j == std::string::npos) j = s.size();
                    const std::string w = s.substr(i, j - i);
                    const std::string trial = cur.empty() ? w : cur + " " + w;
                    if (!cur.empty() && textWidth(trial, px) > zoneW) {
                        out.push_back({ cur, k });
                        cur = w;
                    } else {
                        cur = trial;
                    }
                    i = j + 1;
                }
                if (!cur.empty()) out.push_back({ cur, k });
            }
        };

        float bpx = wide ? P(0.060f) : P(0.048f);
        std::vector<std::pair<std::string, Ink>> rows;
        for (int attempt = 0; attempt < 6; ++attempt) {
            wrapTo(bpx, rows);
            const float rowH0 = bpx * 1.30f;
            const float need = rows.size() * rowH0
                             + (inputLine.empty() ? 0.0f : rowH0 * 1.18f + rowH0 * 0.25f);
            if (need <= vBudget || bpx <= P(0.020f)) break;
            bpx *= std::max(0.72f, vBudget / need);      // shrink, then RE-WRAP
        }
        if (bpx < P(0.020f)) bpx = P(0.020f);
        const float rowH = bpx * 1.30f;
        const float bodyH = rows.size() * rowH
                          + (inputLine.empty() ? 0.0f : rowH * 1.18f + rowH * 0.25f);
        const size_t bodyRows = rows.size();

        // -- THE QUIET BANDS. Knock the line-art down where the type is going. --
        // Header strip: the rule + emblem must not run through the title.
        quietBand(c, P(0.120f), P(0.052f), P(0.930f), P(0.128f), 0.10f, P(0.010f));
        // Body zone: sized to the text that is actually about to be drawn.
        if (bodyRows > 0 || !inputLine.empty()) {
            quietBand(c, lx0b - P(0.012f), tyTop - P(0.014f),
                      lx0b + zoneW + P(0.012f), tyTop + bodyH + P(0.012f),
                      0.10f, P(0.014f));
        }

        // -- HEADER TITLE. --
        {
            float tpx = P(0.052f);
            const float tw = textWidth(lines[0], tpx);
            if (tw > P(0.79f) && tw > 1.0f) tpx *= P(0.79f) / tw;
            drawText(c, lines[0], P(0.130f), P(0.060f), tpx, kBlueHi, HOT);
        }

        // -- BODY ROWS, in STATUS COLOUR. This is what makes it read like a real
        // console instead of one flat colour: GREEN for OK/SECURE/ONLINE, ORANGE for
        // FAILING/AUGMENTED/LOCKED, BLUE for everything else. An explicit host ink
        // override (VIGIL speaking) wins over the keyword read. --
        float ty = tyTop;
        for (size_t ri = 0; ri < rows.size(); ++ri) {
            drawText(c, rows[ri].first, lx0b, ty, bpx, rows[ri].second, (ri == 0) ? HOT : 0.94f);
            ty += rowH;
        }

        // -- LIVE INPUT LINE (amber prompt). --
        if (!inputLine.empty())
            drawText(c, inputLine, lx0b, ty + rowH * 0.25f, bpx * 1.18f, kAmber, HOT);
    }

    return finish(c);
}

} // namespace

// ---- REGRESSION GUARD (see holo_terminal.h) --------------------------------
bool HoloTerminal::screenHasContent() const {
    if (!m_panel.built() || !m_panel.screenHasContent()) return false;
    if (m_lines.empty()) return false;      // something to say
    if (!m_textOnGlass) return false;       // glyphs really baked
    return true;
}

uint32_t HoloTerminal::entity() const { return m_panel.paneEntity(); }
bool HoloTerminal::built() const { return m_panel.built(); }
x3::rhi::TextureHandle HoloTerminal::screenTexture() const { return m_panel.screenTexture(); }

// ---- HEADLESS INK PROBE ----------------------------------------------------
float holoReadoutInkFraction(const std::vector<std::string>& lines,
                             const std::string& inputLine, bool wideReadout) {
    const uint32_t n = 256;
    std::vector<uint8_t> px = makeHologramRGBA(n, lines, inputLine, nullptr, wideReadout);
    // Sample ONLY the BODY-ROW band: inside the data column, below the header rule and
    // the icon squares, above the bottom data strip. Nothing decorative is drawn in that
    // window any more (the tick paragraph is gone and the zone is quieted), so every lit
    // pixel here is TEXT. That is the whole point: a probe that could see the frame would
    // report healthy ink on a panel with no readout at all — a test that passes on a
    // blank screen is exactly the hole this bug kept slipping through.
    const uint32_t x0 = (uint32_t)(0.075f * n), x1 = (uint32_t)(0.340f * n);
    const uint32_t y0 = (uint32_t)(0.270f * n), y1 = (uint32_t)(0.800f * n);
    uint64_t lit = 0, total = 0;
    for (uint32_t y = y0; y < y1; ++y) {
        for (uint32_t x = x0; x < x1; ++x) {
            const uint8_t* p = &px[((size_t)y * n + x) * 4];
            const float lum = (0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]) / 255.0f;
            if (lum > 0.35f) ++lit;
            ++total;
        }
    }
    return total ? (float)lit / (float)total : 0.0f;
}

// ---- HEADLESS PALETTE PROBE ------------------------------------------------
void holoReadoutPalette(const std::vector<std::string>& lines, bool wideReadout,
                        float& blueF, float& greenF, float& orangeF,
                        const float* inkOverride) {
    // Bake at a REALISTIC resolution. The ink probe gets away with 256 because it only
    // asks "is anything lit", but a colour probe has to look at glyph CORES: at 256 the
    // cell layout's long rows shrink the type to ~4 px, every pixel is a partial-coverage
    // blend of ink and black substrate, and the hue washes out. That would make this test
    // measure the rasterizer's antialiasing, not the palette.
    const uint32_t n = 1024;
    Canvas c(n);
    std::vector<uint8_t> px = makeHologramRGBA(n, lines, "", inkOverride, wideReadout);
    for (uint32_t y = 0; y < n; ++y)
        for (uint32_t x = 0; x < n; ++x) {
            const uint8_t* p = &px[((size_t)y * n + x) * 4];
            const size_t i = (size_t)y * n + x;
            c.r[i] = p[0] / 255.0f; c.g[i] = p[1] / 255.0f; c.b[i] = p[2] / 255.0f;
        }
    const float x0 = 0.075f * n, x1 = 0.340f * n;
    const float y0 = 0.270f * n, y1 = 0.800f * n;
    blueF   = blueFraction(c, x0, y0, x1, y1);
    greenF  = greenFraction(c, x0, y0, x1, y1);
    orangeF = orangeFraction(c, x0, y0, x1, y1);
}

void HoloTerminal::shutdown(x3::rhi::IRenderDevice& device) {
    m_panel.shutdown(device);
    m_device = nullptr;
}

void HoloTerminal::build(Scene& scene, x3::rhi::IRenderDevice& device,
                         x3::phys::Vec3 pos, float yaw, float width, float height,
                         float ceilingY) {
    m_pos = pos; m_width = width; m_height = height;
    m_device = &device;

    // Seed the boot readout BEFORE the first bake so the static lines are rasterized
    // into the glass on frame one. Line 0 is the HEADER TITLE; lines 1+ are data rows,
    // each coloured by its own status keyword (SECURE => green, FAILING => orange).
    // The Awakening terminal (EFLZ 3).
    m_lines = {
        "SECURITY CELL 07  --  STATUS: SECURE",
        "SUBJECT: JAKE",
        "STATUS: AUGMENTED",
        "MUSCULOSKELETAL OUTPUT: +400%",
        "RESTRAINT INTEGRITY: FAILING",
        "MAINTENANCE: AUTO-DIAG OK",
        "",
        "ENTER OVERRIDE CODE TO UNLOCK CELL:",
    };
    m_textOnGlass = fontReady();

    // ---- THE FIXTURE IS A HoloPanel. --------------------------------------------
    // Black glass + shiny metallic round-pipe frame + a single support pipe up to the
    // ceiling (it HANGS, it does not float). Everything structural — including the law
    // that NOTHING may stand in front of the screen — lives in the platform now.
    HoloPanelParams p;
    p.pos = pos;
    p.yaw = yaw;
    p.width = width;
    p.height = height;
    p.ceilingY = ceilingY;                 // 0 => platform default (pos.y + 1.7)
    p.frame = HoloFrame::Pipe;
    p.mount = HoloMount::CeilingPipe;
    p.texN = 1024;                         // fine line-art + crisp glyphs
    p.emissiveStrength = 2.1f;
    p.contentBake = [this](uint32_t n) {
        return makeHologramRGBA(n, m_lines, "",
                                m_inkOverride ? m_textColor : nullptr,
                                m_layout == Layout::Readout);
    };
    p.glowLight = true;
    m_panel.build(scene, device, p);

    m_texN = p.texN;
    m_lastInputShown = false;
    m_texDirty = false;

    x3::logInfo("[holoterm] built black-glass terminal (HoloPanel platform) at (" +
                std::to_string((int)pos.x) + "," + std::to_string((int)pos.y) + "," +
                std::to_string((int)pos.z) + ")");
}

void HoloTerminal::setActive(bool on) {
    if (m_active == on) return;
    m_active = on;
    m_texDirty = true;   // show/hide the live input line ON the glass
}

void HoloTerminal::pushChar(char c) {
    if (!m_active) return;
    if (c < 32 || c > 126) return;                 // printable ASCII only
    if (m_input.size() >= kMaxInput) return;
    m_input += c;
    m_texDirty = true;
}

void HoloTerminal::backspace() {
    if (!m_active || m_input.empty()) return;
    m_input.pop_back();
    m_texDirty = true;
}

bool HoloTerminal::submit() {
    if (!m_active) return false;
    const std::string v = m_input;
    bool accept = m_submit ? m_submit(v) : true;
    if (accept) {
        if (!v.empty()) m_lines.push_back("> " + v + "   [ACCEPTED]");
    } else {
        m_lines.push_back("> " + v + "   [REJECTED]");
    }
    m_input.clear();
    m_texDirty = true;   // readout grew + input cleared -> re-bake the glass
    return accept;
}

// Re-rasterize the readout (static lines + live input line) and hand it to the panel.
// Called from update() only when m_texDirty — never every frame.
void HoloTerminal::regenTexture() {
    if (!m_device || !m_panel.built()) { m_texDirty = false; return; }
    // The typed override code is baked on-glass while active. A static '_' caret marks
    // the field (we do not re-bake twice a second just to flash a cursor).
    std::string inputLine;
    if (m_active) inputLine = "> " + m_input + "_";
    m_lastInputShown = m_active;

    m_panel.setContent(makeHologramRGBA(m_texN, m_lines, inputLine,
                                        m_inkOverride ? m_textColor : nullptr,
                                        m_layout == Layout::Readout));
    m_texDirty = false;
}

void HoloTerminal::update(float dt) {
    m_blink += dt;
    if (m_blink >= 0.5f) { m_blink -= 0.5f; m_cursorOn = !m_cursorOn; }
    m_clock += dt;

    // Re-bake only when the readout actually changed. Skipped on the headless self-test
    // path (no device / no panel).
    if (m_texDirty) regenTexture();

    // The shimmer lives in the platform: it pulses the screen emissive, and because
    // emissiveMap is on, the pulse rides the readout ink itself (the text breathes)
    // instead of a flat sheet of blue laid over it.
    m_panel.update(dt);
}

// ===========================================================================
// Headless self-test (--test-holoterm). H0-H4 = the input state machine.
// H5-H10 = THE SCREEN ITSELF: ink, palette, readability, and negative controls.
// ===========================================================================
namespace {
int g_pass = 0, g_fail = 0;
void check(bool cond, const char* name) {
    if (cond) { ++g_pass; x3::logInfo(std::string("[holoterm-test] PASS ") + name); }
    else      { ++g_fail; x3::logError(std::string("[holoterm-test] FAIL ") + name); }
}
}

bool runHoloTerminalSelfTest() {
    g_pass = g_fail = 0;

    HoloTerminal t;

    // ---- H0: boot readout is present (not blank). ----
    t.setLines({
        "DETENTION TERMINAL  // CELL 01",
        "ENTER OVERRIDE CODE TO UNLOCK CELL:",
    });
    check(t.lines().size() >= 2 && !t.lines()[0].empty(), "H0 terminal boots with readout (not blank)");

    // ---- H1: typing while INACTIVE is ignored; ACTIVE builds the input line. ----
    t.pushChar('1'); bool ignoredWhenInactive = t.input().empty();
    t.setActive(true);
    t.pushChar('1'); t.pushChar('1'); t.pushChar('2'); t.pushChar('7');
    check(ignoredWhenInactive && t.input() == "1127", "H1 input only accepted when active");

    // ---- H2: backspace edits the input line. ----
    t.backspace();
    check(t.input() == "112", "H2 backspace edits the input");

    // ---- H3: submit calls the sink with the value; ACCEPT clears + logs a line. ----
    std::string got; bool sinkCalled = false;
    t.setSubmitSink([&](const std::string& v){ got = v; sinkCalled = true; return v == "1127"; });
    t.pushChar('7');
    size_t before = t.lines().size();
    bool accepted = t.submit();
    check(sinkCalled && got == "1127" && accepted && t.input().empty() && t.lines().size() == before + 1,
          "H3 submit fires the sink, accepts, clears, logs");

    // ---- H4: a REJECTED code keeps a reject line + clears, and the cursor blinks. ----
    t.pushChar('9'); t.pushChar('9'); t.pushChar('9'); t.pushChar('9');
    bool rej = t.submit();
    bool blink0 = t.cursorOn();
    t.update(0.6f);
    bool blinkToggled = (t.cursorOn() != blink0);
    check(!rej && t.input().empty() && blinkToggled, "H4 reject path + cursor blinks");

    // =======================================================================
    // THE SCREEN. These gates exist because "the panel is a featureless slab" has
    // been re-fixed about ten times, and every previous fix shipped without a test
    // that could have caught the next one.
    // =======================================================================
    const std::vector<std::string> canon = {
        "SECURITY CELL 07  --  STATUS: SECURE",
        "SUBJECT: JAKE",
        "STATUS: AUGMENTED",
        "RESTRAINT INTEGRITY: FAILING",
        "MAINTENANCE: AUTO-DIAG OK",
    };

    // ---- H5: the bake puts real INK on the glass, in BOTH layouts. ----
    const float inkCell = holoReadoutInkFraction(canon, "", /*wide*/false);
    const float inkWide = holoReadoutInkFraction(canon, "", /*wide*/true);
    check(inkCell > 0.01f && inkWide > 0.01f,
          "H5 readout bakes INK onto the glass (cell + readout layouts)");

    // ---- H6: NEGATIVE CONTROL. The probe must be able to FAIL. A blank readout has
    // no ink — if this ever passes, H5 is measuring decoration and is worthless. ----
    const float inkBlank = holoReadoutInkFraction({ "", "" }, "", /*wide*/true);
    check(inkBlank < 0.002f,
          "H6 NEGATIVE CONTROL: a blank screen has NO ink (the probe can fail)");

    // ---- H7: THE PALETTE. Glowing BLUE / GREEN / ORANGE — the owner's spec.
    // The floor is one SHORT row's worth of glyph cores (~0.0009 of the zone for a
    // 13-character line): enough that a colour which is genuinely absent reads zero,
    // low enough that a palette which is present but sparse still passes. The canon
    // readout is deliberately lopsided — one blue row, one green, two orange. ----
    float blueF = 0, greenF = 0, orangeF = 0;
    holoReadoutPalette(canon, /*wide*/false, blueF, greenF, orangeF, nullptr);
    x3::logInfo("[holoterm-test] palette: blue=" + std::to_string(blueF) +
                " green=" + std::to_string(greenF) + " orange=" + std::to_string(orangeF));
    check(blueF > 0.0004f && greenF > 0.0004f && orangeF > 0.0004f,
          "H7 palette: BLUE + GREEN + ORANGE all present in the readout (not one flat colour)");

    // ---- H7b: THE CYAN GATE. "You fixed it here, but we already fixed it with
    // different color text." The owner's spec says BLUE, and this project has shipped
    // CYAN at least twice. So: force the ink cyan and prove the blue probe COLLAPSES.
    // If cyan can pass as blue, H7 is decoration and the regression ships again. ----
    const float cyanInk[4] = { 0.55f, 1.30f, 1.45f, 1.0f };   // the exact cyan we just removed
    float cyBlue = 0, cyGreen = 0, cyOrange = 0;
    holoReadoutPalette(canon, /*wide*/false, cyBlue, cyGreen, cyOrange, cyanInk);
    x3::logInfo("[holoterm-test] cyan control: blue=" + std::to_string(cyBlue));
    check(cyBlue < blueF * 0.5f,
          "H7b CYAN CONTROL: cyan ink does NOT read as blue (BLUE, not cyan)");

    // ---- H7c: NEGATIVE CONTROL on the palette probe — a blank readout has no colour
    // of any kind. Guards against the probe simply seeing the line-art. ----
    float bB = 0, bG = 0, bO = 0;
    holoReadoutPalette({ "", "" }, /*wide*/false, bB, bG, bO, nullptr);
    check(bB < 0.0002f && bG < 0.0002f && bO < 0.0002f,
          "H7c NEGATIVE CONTROL: a blank readout has NO palette (probe sees text, not decor)");

    // ---- H8: status keywords drive the colour (not position, not luck). ----
    const Ink secure = statusInk("STATUS: SECURE");
    const Ink fail   = statusInk("RESTRAINT INTEGRITY: FAILING");
    const Ink plain  = statusInk("SUBJECT: JAKE");
    const bool secureIsGreen = secure.g > secure.b * 1.4f && secure.g > secure.r * 1.4f;
    const bool failIsOrange  = fail.r > fail.b * 2.0f && fail.r > fail.g * 1.3f;
    const bool plainIsBlue   = plain.b > plain.g * 1.3f && plain.b > plain.r * 1.6f;
    check(secureIsGreen && failIsOrange && plainIsBlue,
          "H8 status keywords map to GREEN / ORANGE / BLUE");

    // ---- H9: READABILITY. The text zone must be QUIET behind the type — the owner's
    // note, "the way it conflicts with the graphics behind the text". Bake the panel
    // with NO body rows and probe the body zone: whatever decoration survives there is
    // what the text would otherwise have had to fight. It must be almost nothing. ----
    const float decorUnderText = holoReadoutInkFraction({ "HEADER ONLY" }, "", /*wide*/false);
    check(decorUnderText < 0.004f,
          "H9 the text zone is CLEAR of decoration (line-art keeps to the margins)");

    // ---- H10: THE VARIANTS. Every shipped baker must put ink on its glass — one
    // platform, four screens, all gated. ----
    {
        auto bakeInk = [](const std::vector<uint8_t>& px, uint32_t n) {
            uint64_t lit = 0, total = 0;
            for (uint32_t y = (uint32_t)(0.10f*n); y < (uint32_t)(0.90f*n); ++y)
                for (uint32_t x = (uint32_t)(0.06f*n); x < (uint32_t)(0.94f*n); ++x) {
                    const uint8_t* p = &px[((size_t)y*n + x) * 4];
                    const float lum = (0.2126f*p[0] + 0.7152f*p[1] + 0.0722f*p[2]) / 255.0f;
                    if (lum > 0.35f) ++lit;
                    ++total;
                }
            return total ? (float)lit / (float)total : 0.0f;
        };
        const uint32_t n = 512;
        const float fElev = bakeInk(bakeFloorSelect(n, { "F1 DETENTION", "R1 RIFT HUB", "C1 CLUB 1127" },
                                                    0, "IDLE", 1.0f), n);
        const float fKeys = bakeInk(bakeKeypad(n, "112", /*locked*/true, 1.0f), n);
        const float fPlac = bakeInk(bakePlacard(n, { "DETENTION WING", "AUTHORIZED PERSONNEL ONLY" },
                                                1.0f), n);
        x3::logInfo("[holoterm-test] variant ink: elevator=" + std::to_string(fElev) +
                    " keypad=" + std::to_string(fKeys) + " placard=" + std::to_string(fPlac));
        check(fElev > 0.004f && fKeys > 0.004f && fPlac > 0.004f,
              "H10 every VARIANT bakes ink: elevator floor-select / keypad / placard");
    }

    x3::logInfo(std::string("[holoterm-test] ") + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

} // namespace x3::game
