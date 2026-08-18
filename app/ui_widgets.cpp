// ===========================================================================
// THE ImGui-STUDY FOLD — layout cursor, collapsing headers, tabs, tooltips,
// the colour editor and the combo cycler. Declared in app/ui.h; kept in their
// own translation unit because ui.cpp is already the UI's biggest file and
// these are one coherent addition rather than edits scattered through it.
//
// Dear ImGui is NOT linked into the game (it stays in X3LevelArchitect). What
// was taken from it is INTERACTION DESIGN, re-implemented on this project's own
// widgets in this project's own look:
//
//   ImGui idea                     what landed here
//   ----------------------------   ------------------------------------------
//   ctrl+click a slider to type    folded into UiContext::sliderEx (ui.cpp), so
//                                  EVERY existing slider got it at once
//   BeginChild/SameLine/Separator  beginLayout/row/rowSplit/rest/separator
//   CollapsingHeader               collapsingHeader(label, bool&)
//   BeginTabBar                    tabBar(labels, count, int&)
//   SetTooltip                     tooltip(text), queued + drawn after end()
//   ColorEdit4                     colorEdit4(label, float[4]) + live swatch
//   Combo                          combo() as a cycler (see the note there)
//   Tab / Shift+Tab focus          UiInput::navNext/navPrev, applied in end()
//
// EVERYTHING RETURNS WHETHER IT CHANGED. That is the property that keeps
// immediate-mode call sites short: a panel pushes a value straight into a cvar
// on the frame it moved, with no previous-value bookkeeping of its own.
//
// The look is the SHARED one: hud_panel.h near-black glass at the measured 0.86
// alpha, the tiny 3 px corner radius from the same constant every other panel
// reads, and the established accent inks. No second visual system.
// ===========================================================================
#include "ui.h"
#include "hud_panel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace x3::ui {

namespace {

// Section/label inks, shared by the widgets below.
constexpr float kSecCol[4]   = { 0.40f, 0.88f, 1.00f, 1.00f };  // section headings
constexpr float kDimCol[4]   = { 0.56f, 0.63f, 0.69f, 1.00f };  // sub-labels
constexpr float kRuleCol[4]  = { 0.30f, 0.55f, 0.70f, 0.35f };  // separators
constexpr float kHotFill[4]  = { 0.16f, 0.42f, 0.66f, 0.92f };  // focused row
constexpr float kColdFill[4] = { 0.10f, 0.13f, 0.18f, 0.82f };  // idle row
constexpr float kEdge[4]     = { 0.30f, 0.62f, 0.95f, 0.95f };  // lit edge
constexpr float kTextCol[4]  = { 0.90f, 0.95f, 0.92f, 1.00f };

} // namespace

// ---------------------------------------------------------------------------
// LAYOUT CURSOR
// ---------------------------------------------------------------------------
void UiContext::beginLayout(float x, float y, float w, float rowH, float gap) {
    m_layX = x; m_layY = y; m_layW = w;
    m_layRowH = (rowH > 0.0f) ? rowH : 26.0f;
    m_layGap = gap;
    m_layIndent = 0.0f;
    m_layRest = Rect{};
}

Rect UiContext::row(float h) {
    const float rh = (h > 0.0f) ? h : m_layRowH;
    Rect r{ m_layX + m_layIndent, m_layY, m_layW - m_layIndent, rh };
    m_layY += rh + m_layGap;
    m_layRest = Rect{};
    return r;
}

Rect UiContext::rowSplit(float frac, float h) {
    const float rh = (h > 0.0f) ? h : m_layRowH;
    const float full = m_layW - m_layIndent;
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    const float lw = std::max(0.0f, full * frac - m_layGap * 0.5f);
    Rect left{ m_layX + m_layIndent, m_layY, lw, rh };
    m_layRest = Rect{ left.x + lw + m_layGap, m_layY, std::max(0.0f, full - lw - m_layGap), rh };
    m_layY += rh + m_layGap;
    return left;
}

Rect UiContext::rowSplitPx(float rightW, float h) {
    const float rh = (h > 0.0f) ? h : m_layRowH;
    const float full = m_layW - m_layIndent;
    const float rw = std::clamp(rightW, 0.0f, full);
    const float lw = std::max(0.0f, full - rw - m_layGap);
    Rect left{ m_layX + m_layIndent, m_layY, lw, rh };
    m_layRest = Rect{ left.x + lw + m_layGap, m_layY, rw, rh };
    m_layY += rh + m_layGap;
    return left;
}

Rect UiContext::rest() { return m_layRest; }

void UiContext::spacing(float px) { m_layY += px; }

void UiContext::indent(float px)  { m_layIndent += px; }
void UiContext::unindent()        { m_layIndent = std::max(0.0f, m_layIndent - 12.0f); }

void UiContext::separator(const char* label) {
    if (label && *label) {
        m_layY += 2.0f;
        text(label, m_layX + m_layIndent, m_layY, 11.0f, kSecCol, FontRole::HudMono);
        m_layY += 15.0f;
    }
    quad(m_layX + m_layIndent, m_layY, m_layW - m_layIndent, 1.0f, kRuleCol);
    m_layY += 1.0f + m_layGap;
}

// ---------------------------------------------------------------------------
// COLLAPSING HEADER — the widget that lets ONE panel hold a growing cvar set
// instead of becoming an unusable flat list.
// ---------------------------------------------------------------------------
bool UiContext::collapsingHeader(const char* label, bool& open) {
    const Rect r = row(22.0f);
    const int idx = m_widgetIndex++;

    const bool hovered = pointIn(r.x, r.y, r.w, r.h);
    m_lastHovered = hovered;
    if (hovered) { m_focus = idx; m_mouseMovedFocus = true; }
    const bool hot = (m_focus == idx);

    // A header reads as a title band, not a button: a low-alpha wash plus a lit
    // left bar (the same accent-bar grammar hudPanel() uses), so a stack of them
    // is legible as structure rather than as a column of chunky buttons.
    const float wash[4] = { kHotFill[0], kHotFill[1], kHotFill[2], hot ? 0.55f : 0.22f };
    quad(r.x, r.y, r.w, r.h, wash);
    quad(r.x, r.y, 2.0f, r.h, hot ? kEdge : kRuleCol);

    // Disclosure triangle, drawn as stacked strips (no glyph for it in the atlas).
    const float tri = 8.0f;
    const float tx = r.x + 8.0f, tyc = r.y + r.h * 0.5f;
    for (int i = 0; i < (int)tri; ++i) {
        const float f = (float)i / tri;
        if (open) {
            // Pointing DOWN: a triangle that narrows as it descends.
            quad(tx + f * tri * 0.5f, tyc - tri * 0.5f + f * tri * 0.7f,
                 tri * (1.0f - f), 1.0f, hot ? kTextCol : kDimCol);
        } else {
            // Pointing RIGHT: narrows as it advances.
            quad(tx + f * tri * 0.7f, tyc - tri * 0.5f + f * tri * 0.5f,
                 1.0f, tri * (1.0f - f), hot ? kTextCol : kDimCol);
        }
    }

    text(label, r.x + 22.0f, r.y + (r.h - 12.0f) * 0.5f, 12.0f,
         hot ? kTextCol : kDimCol, FontRole::HudMono);

    if ((hovered && m_in.mousePressed) || (hot && m_in.navActivate)) open = !open;
    return open;
}

// ---------------------------------------------------------------------------
// TAB BAR — Weapons / Lighting / Camera / World as pages of one panel.
// ---------------------------------------------------------------------------
bool UiContext::tabBar(const char* const* labels, int count, int& active, float h) {
    if (!labels || count <= 0) return false;
    const Rect r = row(h);
    if (active < 0) active = 0;
    if (active >= count) active = count - 1;

    const float tabW = r.w / (float)count;
    bool changed = false;
    for (int i = 0; i < count; ++i) {
        const int idx = m_widgetIndex++;
        const float tx = r.x + (float)i * tabW;
        const bool hovered = pointIn(tx, r.y, tabW, r.h);
        m_lastHovered = hovered;
        if (hovered) { m_focus = idx; m_mouseMovedFocus = true; }
        const bool hot = (m_focus == idx);
        const bool sel = (i == active);

        // The SELECTED tab is the bright one and is the only one with an
        // underline; focus adds an edge. Selection and focus are different
        // things and must not look the same, or keyboard nav reads as a change.
        // The SELECTED tab is a lit plate with a 3px underline; the others are
        // nearly bare glass. Selection has to be legible at a glance from across
        // the desk, and "slightly bluer" is not (the first capture proved it).
        const float fill[4] = { kHotFill[0], kHotFill[1], kHotFill[2],
                                sel ? 0.85f : (hot ? 0.30f : 0.10f) };
        quad(tx, r.y, tabW - 1.0f, r.h, fill);
        if (sel) quad(tx, r.y + r.h - 3.0f, tabW - 1.0f, 3.0f, kEdge);
        if (hot && !sel) quad(tx, r.y, tabW - 1.0f, 1.0f, kEdge);

        const float px = 11.0f;
        textCentered(labels[i], tx + tabW * 0.5f, r.y + (r.h - px) * 0.5f, px,
                     sel ? kTextCol : kDimCol, FontRole::HudMono);

        if ((hovered && m_in.mousePressed) || (hot && m_in.navActivate)) {
            if (active != i) { active = i; changed = true; }
        }
    }
    return changed;
}

// ---------------------------------------------------------------------------
// TOOLTIP — queued when the most recently emitted widget was hovered, drawn
// over everything from end(). The tuning panels feed it IConsole::cvarHelp(),
// so the panel documents itself from the console's own registered string.
// ---------------------------------------------------------------------------
void UiContext::tooltip(const char* text_) {
    if (!m_lastHovered || !text_ || !*text_) return;
    m_tipText = text_;
    m_tipPending = true;
}

void UiContext::drawQueuedTooltip() {
    if (!m_tipPending || m_tipText.empty()) return;
    m_tipPending = false;
    if (!m_draw) return;   // headless: the queue still works, the paint is skipped

    // Wrap to a readable measure. cvar help strings are one or two sentences.
    constexpr int   kWrapCols = 52;
    constexpr float kPx       = 11.0f;
    std::string lines[8];
    int lineCount = 0;
    {
        std::string cur;
        std::string word;
        auto flush = [&]() {
            if (lineCount < 8) lines[lineCount++] = cur;
            cur.clear();
        };
        for (size_t i = 0; i <= m_tipText.size() && lineCount < 8; ++i) {
            const char c = (i < m_tipText.size()) ? m_tipText[i] : ' ';
            if (c == ' ' || c == '\n') {
                if (!word.empty()) {
                    if (!cur.empty() && (int)(cur.size() + 1 + word.size()) > kWrapCols) flush();
                    if (!cur.empty()) cur += ' ';
                    cur += word;
                    word.clear();
                }
                if (c == '\n') flush();
            } else {
                word += c;
            }
        }
        if (!cur.empty() && lineCount < 8) lines[lineCount++] = cur;
    }
    if (lineCount == 0) return;

    float widest = 0.0f;
    for (int i = 0; i < lineCount; ++i)
        widest = std::max(widest, textWidth(FontRole::HudMono, lines[i].c_str(), kPx));

    const float padX = 8.0f, padY = 6.0f;
    const float bw = widest + padX * 2.0f;
    const float bh = (float)lineCount * x3::game::hudLineH(kPx) + padY * 2.0f;
    // Anchor below-right of the cursor, then flip/clamp so it never leaves the
    // screen — a tooltip that runs off the edge is worse than none.
    float bx = m_in.mouseX + 14.0f;
    float by = m_in.mouseY + 18.0f;
    if (bx + bw > (float)m_w) bx = std::max(0.0f, m_in.mouseX - bw - 8.0f);
    if (by + bh > (float)m_h) by = std::max(0.0f, m_in.mouseY - bh - 8.0f);

    // The shared plate: same glass, same tiny radius, cyan accent bar.
    panel(bx, by, bw, bh, x3::game::kHudPanelFill, x3::game::kHudAccentCyan);
    for (int i = 0; i < lineCount; ++i)
        text(lines[i].c_str(), bx + padX, by + padY + (float)i * x3::game::hudLineH(kPx),
             kPx, x3::game::kHudTextLight, FontRole::HudMono);
}

// ---------------------------------------------------------------------------
// COLOUR EDITOR — the highest-value widget of this fold. Fog, emissives, lamp
// tint, hull darkness and HUD alpha are all art-directed by eye, and every one
// of them used to cost edit -> rebuild -> relaunch -> walk back to the room.
//
// A label row carrying a LIVE SWATCH plus the hex readout, then one slider per
// channel. Channels are 0..1 LINEAR — the same space the render params and the
// hud_panel palette use — so what the swatch shows is what the parameter is.
// ---------------------------------------------------------------------------
bool UiContext::colorEdit4(const char* label, float rgba[4], bool withAlpha) {
    if (!rgba) return false;
    bool changed = false;

    // ---- Header row: [label ............ swatch] [#RRGGBBAA] --------------
    {
        const Rect r = rowSplitPx(96.0f, 22.0f);
        text(label, r.x, r.y + (r.h - 12.0f) * 0.5f, 12.0f, kSecCol, FontRole::HudMono);

        // The swatch sits at the right end of the LABEL cell, so the hex value
        // and the colour are side by side and read as one statement.
        const float sw = 40.0f, sh = r.h - 4.0f;
        const float sx = r.x + r.w - sw, sy = r.y + 2.0f;
        // Checkerboard behind it, so alpha is legible as transparency rather
        // than as "a slightly different colour".
        const float ck1[4] = { 0.30f, 0.30f, 0.32f, 1.0f };
        const float ck2[4] = { 0.16f, 0.16f, 0.18f, 1.0f };
        const float cell = sh * 0.5f;
        for (int cy = 0; cy < 2; ++cy)
            for (int cx = 0; cx * cell < sw; ++cx)
                quad(sx + (float)cx * cell, sy + (float)cy * cell,
                     std::min(cell, sw - (float)cx * cell), cell,
                     ((cx + cy) & 1) ? ck1 : ck2);
        const float swatch[4] = { rgba[0], rgba[1], rgba[2], withAlpha ? rgba[3] : 1.0f };
        quad(sx, sy, sw, sh, swatch);
        quad(sx, sy, sw, 1.0f, kRuleCol);

        const Rect hr = rest();
        char hex[16];
        auto b255 = [](float f) {
            const int v = (int)std::lround(std::clamp(f, 0.0f, 1.0f) * 255.0f);
            return v;
        };
        if (withAlpha)
            std::snprintf(hex, sizeof(hex), "#%02X%02X%02X%02X",
                          b255(rgba[0]), b255(rgba[1]), b255(rgba[2]), b255(rgba[3]));
        else
            std::snprintf(hex, sizeof(hex), "#%02X%02X%02X",
                          b255(rgba[0]), b255(rgba[1]), b255(rgba[2]));
        text(hex, hr.x, hr.y + (hr.h - 11.0f) * 0.5f, 11.0f, kTextCol, FontRole::HudMono);
    }

    // ---- One slider per channel. Each is a full sliderEx, so each one also
    // gets ctrl+click-to-type for free — which is how an exact tint gets
    // entered rather than approached.
    static const char* kChan[4] = { "R", "G", "B", "A" };
    const int chans = withAlpha ? 4 : 3;
    indent(10.0f);
    for (int c = 0; c < chans; ++c) {
        char ro[16];
        std::snprintf(ro, sizeof(ro), "%.3f", (double)rgba[c]);
        if (sliderEx(kChan[c], rgba[c], 0.0f, 1.0f, 0.005f, ro, row(22.0f)))
            changed = true;
    }
    unindent();
    return changed;
}

// ---------------------------------------------------------------------------
// COMBO — pick one of N.
//
// Implemented as a CYCLER, not a drop-down popup: a popup has to draw and
// hit-test ABOVE widgets that are emitted after it, which in a single
// immediate-mode pass needs a deferred input layer this UI does not have (the
// tooltip queue defers DRAWING only, which is safe; deferring INPUT is not).
// A cycler does the same job — choose one of a fixed set, by mouse or keyboard —
// with no ordering hazard. Click or Right steps forward, Left steps back.
// ---------------------------------------------------------------------------
bool UiContext::combo(const char* label, const char* const* items, int count,
                      int& idx, Rect r) {
    if (!items || count <= 0) return false;
    const int wid = m_widgetIndex++;
    if (idx < 0) idx = 0;
    if (idx >= count) idx = count - 1;

    const bool hovered = pointIn(r.x, r.y, r.w, r.h);
    m_lastHovered = hovered;
    if (hovered) { m_focus = wid; m_mouseMovedFocus = true; }
    const bool hot = (m_focus == wid);

    quad(r.x, r.y, r.w, r.h, hot ? kHotFill : kColdFill);
    if (hot) {
        quad(r.x, r.y, r.w, 1.0f, kEdge);
        quad(r.x, r.y + r.h - 1.0f, r.w, 1.0f, kEdge);
    }

    const float px = std::min(12.0f, r.h * 0.5f);
    const float ty = r.y + (r.h - px) * 0.5f;
    if (label && *label)
        text(label, r.x + 8.0f, ty, px, hot ? kTextCol : kDimCol, FontRole::HudMono);

    // The value cell carries its own < > affordances, so it is obvious the row
    // steps rather than opens.
    const float vx = r.x + r.w * 0.42f;
    const float vw = r.w - (vx - r.x) - 8.0f;
    text("<", vx, ty, px, hot ? kTextCol : kDimCol, FontRole::HudMono);
    text(">", vx + vw - px * 0.7f, ty, px, hot ? kTextCol : kDimCol, FontRole::HudMono);
    textCentered(items[idx], vx + vw * 0.5f, ty, px, kTextCol, FontRole::HudMono);

    const int before = idx;
    if (hovered && m_in.mousePressed) {
        // Click on the LEFT third steps back, anywhere else steps forward — the
        // "< value >" the row draws is therefore literally what it does.
        if (m_in.mouseX < vx + vw * 0.33f) idx = (idx - 1 + count) % count;
        else                               idx = (idx + 1) % count;
    }
    if (hot) {
        if (m_in.navRight || m_in.navActivate) idx = (idx + 1) % count;
        if (m_in.navLeft)                      idx = (idx - 1 + count) % count;
    }
    return idx != before;
}

} // namespace x3::ui
