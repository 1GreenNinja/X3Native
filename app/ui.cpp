// X3 GENERAL game-UI layer. See app/ui.h.
//
// Clean-room: built only from the public IRenderDevice + IConsole interfaces.
// No id Tech / RBDOOM source consulted.
#include "ui.h"
#include "headless_device.h"   // shared no-op IRenderDevice (for --test-ui)
#include "world_menu.h"        // U31: the world/place selection menu's own gate

#include "engine/core/x3_log.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <string>

namespace x3::ui {

namespace {
// 8x8 bitmap font: each glyph cell is square, so a string of N chars at glyph
// size `px` is N*px wide and px tall (matches the engine's drawHudText layout).
constexpr float kFontAspect = 1.0f;

// Palette (linear RGBA). Kept module-local so widgets share one look.
constexpr float kColText[4]     = { 0.90f, 0.95f, 0.92f, 1.0f };
constexpr float kColTextDim[4]  = { 0.62f, 0.68f, 0.66f, 1.0f };
constexpr float kColShadow[4]   = { 0.0f,  0.0f,  0.0f,  0.80f };
constexpr float kColBtn[4]      = { 0.10f, 0.13f, 0.18f, 0.82f };
constexpr float kColBtnHot[4]   = { 0.16f, 0.42f, 0.66f, 0.92f }; // focused/hover
constexpr float kColBtnEdge[4]  = { 0.30f, 0.62f, 0.95f, 0.95f };
constexpr float kColPanel[4]    = { 0.02f, 0.03f, 0.06f, 0.78f };
constexpr float kColPanelEdge[4]= { 0.20f, 0.55f, 0.95f, 0.85f };
constexpr float kColOn[4]       = { 0.20f, 0.80f, 0.30f, 0.95f };
constexpr float kColOff[4]      = { 0.45f, 0.45f, 0.48f, 0.90f };
constexpr float kColTrack[4]    = { 0.10f, 0.10f, 0.12f, 0.85f };
} // namespace

// ===========================================================================
// UiContext
// ===========================================================================
// Live device the STATIC textWidth() queries for true per-role glyph metrics.
x3::rhi::IRenderDevice* UiContext::s_metricsDevice = nullptr;

void UiContext::begin(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                      const UiInput& input) {
    m_device = &device;
    s_metricsDevice = &device;   // textWidth() reads real per-role advances from here
    m_frame  = frame;
    m_in     = input;
    m_draw   = frame.valid;
    device.hudSize(m_w, m_h);

    m_widgetIndex     = 0;
    m_mouseMovedFocus = false;
}

void UiContext::end() {
    // Number of focusable widgets emitted this frame.
    m_lastFocusCount = m_widgetIndex;
    if (m_lastFocusCount <= 0) { m_focus = 0; return; }

    // Apply keyboard nav (wrap-around) unless a hover already moved focus this
    // frame (mouse takes precedence so the two never fight on the same frame).
    if (!m_mouseMovedFocus) {
        if (m_in.navDown) m_focus = (m_focus + 1) % m_lastFocusCount;
        if (m_in.navUp)   m_focus = (m_focus - 1 + m_lastFocusCount) % m_lastFocusCount;
    }
    if (m_focus < 0) m_focus = 0;
    if (m_focus >= m_lastFocusCount) m_focus = m_lastFocusCount - 1;
}

float UiContext::textWidth(FontRole role, const char* s, float px) {
    if (!s) return 0.0f;
    // Real per-role advances from the live device (proportional-aware). This keeps
    // centering/right-alignment pixel-exact for Title/Menu/Enemy. If no device is
    // bound yet (pre-begin / headless logic before begin), fall back to the legacy
    // monospace estimate N*px so layout math stays deterministic.
    if (s_metricsDevice) return s_metricsDevice->textAdvance(role, s, px);
    return (float)std::strlen(s) * px * kFontAspect;
}

bool UiContext::pointIn(float x, float y, float w, float h) const {
    return m_in.mouseX >= x && m_in.mouseX < x + w &&
           m_in.mouseY >= y && m_in.mouseY < y + h;
}

void UiContext::quad(float x, float y, float w, float h, const float rgba[4]) const {
    if (m_draw && m_device) m_device->drawHudQuad(m_frame, x, y, w, h, rgba);
}

float UiContext::text(const char* s, float x, float y, float px, const float rgba[4],
                      FontRole role) const {
    if (m_draw && m_device && s) {
        // 1px drop shadow for legibility over bright scene pixels.
        m_device->drawHudTextF(m_frame, role, s, x + 1.0f, y + 1.0f, px, kColShadow);
        m_device->drawHudTextF(m_frame, role, s, x, y, px, rgba);
    }
    return textWidth(role, s, px);
}

float UiContext::textCentered(const char* s, float cx, float y, float px, const float rgba[4],
                              FontRole role) const {
    const float w = textWidth(role, s, px);
    const float left = cx - w * 0.5f;
    text(s, left, y, px, rgba, role);
    return left;
}

void UiContext::label(const char* s, float x, float y, float px, const float rgba[4],
                      FontRole role) const {
    text(s, x, y, px, rgba, role);
}

void UiContext::panel(float x, float y, float w, float h, const float rgba[4]) const {
    quad(x, y, w, h, rgba);
    quad(x, y, w, 2.0f, kColPanelEdge);   // bright top edge
}

float UiContext::enemyNameplate(const char* s, float cx, float top, float px,
                                const float rgba[4]) const {
    // Centered in the Enemy font (Tektur Condensed Bold). textCentered already lays
    // a drop shadow + the tinted text; the role gives the condensed threat look.
    return textCentered(s, cx, top, px, rgba, FontRole::Enemy);
}

bool UiContext::button(const char* label, float x, float y, float w, float h) {
    const int idx = m_widgetIndex++;

    // Hover claims keyboard focus so mouse + keyboard agree on what's "hot".
    const bool hovered = pointIn(x, y, w, h);
    if (hovered) { m_focus = idx; m_mouseMovedFocus = true; }
    const bool hot = (m_focus == idx);

    // Background + (when hot) a bright outline.
    quad(x, y, w, h, hot ? kColBtnHot : kColBtn);
    if (hot) {
        const float t = 2.0f;
        quad(x, y, w, t, kColBtnEdge);
        quad(x, y + h - t, w, t, kColBtnEdge);
        quad(x, y, t, h, kColBtnEdge);
        quad(x + w - t, y, t, h, kColBtnEdge);
    }

    // Centered label, vertically centered in the button.
    const float px = h * 0.42f;
    const float ty = y + (h - px) * 0.5f;
    textCentered(label, x + w * 0.5f, ty, px, hot ? kColText : kColTextDim);

    // Activation: mouse click inside, OR keyboard activate while focused.
    const bool clicked = hovered && m_in.mousePressed;
    const bool keyed   = hot && m_in.navActivate;
    return clicked || keyed;
}

bool UiContext::toggle(const char* label, bool value, float x, float y, float w, float h) {
    const int idx = m_widgetIndex++;

    const bool hovered = pointIn(x, y, w, h);
    if (hovered) { m_focus = idx; m_mouseMovedFocus = true; }
    const bool hot = (m_focus == idx);

    // Row background.
    quad(x, y, w, h, hot ? kColBtnHot : kColBtn);
    if (hot) {
        const float t = 2.0f;
        quad(x, y, w, t, kColBtnEdge);
        quad(x, y + h - t, w, t, kColBtnEdge);
    }

    // Left: the label.
    const float px = h * 0.42f;
    const float ty = y + (h - px) * 0.5f;
    text(label, x + 14.0f, ty, px, hot ? kColText : kColTextDim);

    // Right: an ON/OFF pill.
    const float pillW = 64.0f, pillH = h - 12.0f;
    const float pillX = x + w - pillW - 12.0f;
    const float pillY = y + 6.0f;
    quad(pillX, pillY, pillW, pillH, value ? kColOn : kColOff);
    const char* st = value ? "ON" : "OFF";
    const float stpx = pillH * 0.55f;
    textCentered(st, pillX + pillW * 0.5f, pillY + (pillH - stpx) * 0.5f, stpx, kColText);

    // Toggle on: click anywhere in the row, or keyboard activate / left / right.
    const bool clicked = hovered && m_in.mousePressed;
    const bool keyed   = hot && (m_in.navActivate || m_in.navLeft || m_in.navRight);
    return clicked || keyed;
}

bool UiContext::slider(const char* label, float& value, float x, float y, float w, float h) {
    const int idx = m_widgetIndex++;

    const bool hovered = pointIn(x, y, w, h);
    if (hovered) { m_focus = idx; m_mouseMovedFocus = true; }
    const bool hot = (m_focus == idx);

    // Clamp the incoming value into [0,1] for display + math.
    float v = value; if (v < 0.0f) v = 0.0f; if (v > 1.0f) v = 1.0f;

    // Row background (matches toggle/button look).
    quad(x, y, w, h, hot ? kColBtnHot : kColBtn);
    if (hot) {
        const float t = 2.0f;
        quad(x, y, w, t, kColBtnEdge);
        quad(x, y + h - t, w, t, kColBtnEdge);
    }

    // Left: the label.
    const float px = h * 0.42f;
    const float ty = y + (h - px) * 0.5f;
    text(label, x + 14.0f, ty, px, hot ? kColText : kColTextDim);

    // Right: a "NN%" readout (fixed cell at the far right).
    char pct[8];
    std::snprintf(pct, sizeof(pct), "%d%%", (int)(v * 100.0f + 0.5f));
    const float pctW = 56.0f;
    const float pctPx = h * 0.40f;
    textCentered(pct, x + w - pctW * 0.5f - 10.0f, y + (h - pctPx) * 0.5f, pctPx, kColText);

    // Track geometry: a horizontal bar between the label and the percent readout.
    const float labelW = 164.0f;            // reserved width for the label (was 132 — keep "Music Volume"/"SFX Volume" from overrunning the track)
    const float trackX = x + labelW;
    const float trackR = x + w - pctW - 16.0f;
    const float trackW = std::max(8.0f, trackR - trackX);
    const float trackH = std::max(4.0f, h * 0.16f);
    const float trackY = y + (h - trackH) * 0.5f;

    quad(trackX, trackY, trackW, trackH, kColTrack);          // empty track
    quad(trackX, trackY, trackW * v, trackH, kColOn);          // filled portion
    // Handle: a small bright knob at the value position.
    const float knobW = 10.0f, knobH = h * 0.55f;
    const float knobX = trackX + trackW * v - knobW * 0.5f;
    const float knobY = y + (h - knobH) * 0.5f;
    quad(knobX, knobY, knobW, knobH, kColBtnEdge);

    // ---- Interaction --------------------------------------------------------
    float nv = v;
    bool changed = false;

    // Mouse: click OR drag while held anywhere over the row maps the cursor x onto
    // the track range (click-to-position + drag-to-scrub). Use the row hover for the
    // initial grab so the whole row is an easy target.
    if (hovered && m_in.mouseDown) {
        float t = (m_in.mouseX - trackX) / trackW;
        if (t < 0.0f) t = 0.0f; if (t > 1.0f) t = 1.0f;
        nv = t;
    }
    // Keyboard: nudge by 5% while focused.
    if (hot) {
        if (m_in.navLeft)  nv -= 0.05f;
        if (m_in.navRight) nv += 0.05f;
    }
    if (nv < 0.0f) nv = 0.0f; if (nv > 1.0f) nv = 1.0f;
    if (nv != v) { value = nv; changed = true; }
    return changed;
}

// ===========================================================================
// R8 — THE GLOWING CONTROL SURFACE ("the sliders glow").
//
// One shared law for all three widgets: a control's colour IS its danger readout.
// The player never has to read a number to know the rift is about to bite — the
// thing under their hand goes amber, then red, then starts to flicker.
//
// CAP LAW (borrowed from the membrane, and for the same reason): emissive-looking
// HUD colour is still colour. Push a "glow" past ~1.0 on all three channels and it
// reads as WHITE — the chroma that carries the meaning dies exactly when the
// meaning matters most. So the hot colour is built by SHIFTING HUE (green -> amber
// -> red) and lifting alpha, never by lifting all three channels together.
// ===========================================================================
namespace {

// The console palette: black glass, blue + green light, amber/red for warnings.
constexpr float kGlassBg[4]   = { 0.015f, 0.020f, 0.030f, 0.90f };  // black glass slab
constexpr float kGlassEdge[4] = { 0.10f,  0.55f,  0.85f,  0.75f };  // blue pipe-frame edge
constexpr float kGlowCalm[3]  = { 0.10f,  0.95f,  0.70f };          // calm: blue-green
constexpr float kGlowWarn[3]  = { 1.00f,  0.62f,  0.10f };          // warning: amber
constexpr float kGlowHot[3]   = { 1.00f,  0.16f,  0.12f };          // catastrophe: red
constexpr float kGlowMaxA     = 0.98f;   // HARD CAP — a control never clips to white

// danger01 -> the control's live glow colour + its pulse. Calm is steady; the
// closer to catastrophe, the hotter the hue and the faster/deeper the flicker.
void glowColor(float danger01, float clock, float phase, float out[4]) {
    float d = danger01 < 0.0f ? 0.0f : (danger01 > 1.0f ? 1.0f : danger01);
    // Hue: calm -> amber over the first half, amber -> red over the second.
    const float* a = (d < 0.5f) ? kGlowCalm : kGlowWarn;
    const float* b = (d < 0.5f) ? kGlowWarn : kGlowHot;
    const float t = (d < 0.5f) ? (d * 2.0f) : ((d - 0.5f) * 2.0f);
    for (int c = 0; c < 3; ++c) out[c] = a[c] + (b[c] - a[c]) * t;
    // Pulse: nothing while calm, a slow throb as it warms, a hot flicker at the top.
    const float hz    = 0.8f + 7.0f * d * d;
    const float depth = 0.30f * d * d;
    const float s     = 0.5f * (std::sin(clock * 6.2831853f * hz + phase) + 1.0f);
    float alpha = (0.62f + 0.30f * d) * (1.0f - depth + depth * s);
    if (alpha > kGlowMaxA) alpha = kGlowMaxA;
    out[3] = alpha;
}

// The same colour at a fraction of the intensity (unlit track, dim ticks).
void glowDim(const float lit[4], float k, float out[4]) {
    out[0] = lit[0] * k; out[1] = lit[1] * k; out[2] = lit[2] * k;
    out[3] = lit[3] * (0.30f + 0.30f * k);
}

} // namespace

bool UiContext::glowSlider(const char* label, float& value, float x, float y,
                           float w, float h, float danger01, float clock) {
    const int idx = m_widgetIndex++;
    const bool hovered = pointIn(x, y, w, h);
    if (hovered) { m_focus = idx; m_mouseMovedFocus = true; }
    const bool hot = (m_focus == idx);

    float v = value; if (v < 0.0f) v = 0.0f; if (v > 1.0f) v = 1.0f;

    float lit[4], dim[4];
    glowColor(danger01, clock, (float)idx * 0.7f, lit);
    glowDim(lit, 0.22f, dim);

    // The row sits ON the black glass; only a focused row gets a faint lift.
    if (hot) {
        const float lift[4] = { lit[0] * 0.10f, lit[1] * 0.10f, lit[2] * 0.10f, 0.30f };
        quad(x, y, w, h, lift);
    }

    const float px = h * 0.40f;
    const float ty = y + (h - px) * 0.5f;
    text(label, x + 10.0f, ty, px, hot ? lit : dim, FontRole::HudMono);

    char pct[8];
    std::snprintf(pct, sizeof(pct), "%d%%", (int)(v * 100.0f + 0.5f));
    const float pctW = 52.0f;
    textCentered(pct, x + w - pctW * 0.5f - 6.0f, ty, px, lit, FontRole::HudMono);

    // Track: a LIT CHANNEL. The filled portion is charged (bright); the rest is a
    // dim groove. The handle is the brightest thing on the row.
    const float labelW = 150.0f;
    const float trackX = x + labelW;
    const float trackR = x + w - pctW - 12.0f;
    const float trackW = std::max(8.0f, trackR - trackX);
    const float trackH = std::max(5.0f, h * 0.22f);
    const float trackY = y + (h - trackH) * 0.5f;

    quad(trackX, trackY - 1.0f, trackW, trackH + 2.0f, kGlassBg);   // recessed channel
    quad(trackX, trackY, trackW, trackH, dim);                      // unlit remainder
    quad(trackX, trackY, trackW * v, trackH, lit);                  // CHARGED fill
    // Handle: brighter core + a glow halo either side (fake bloom, 2 quads).
    const float knobW = 9.0f, knobH = h * 0.62f;
    const float knobX = trackX + trackW * v - knobW * 0.5f;
    const float knobY = y + (h - knobH) * 0.5f;
    float halo[4] = { lit[0], lit[1], lit[2], lit[3] * 0.35f };
    quad(knobX - 4.0f, knobY - 3.0f, knobW + 8.0f, knobH + 6.0f, halo);
    float core[4] = { lit[0], lit[1], lit[2], kGlowMaxA };
    quad(knobX, knobY, knobW, knobH, core);

    float nv = v;
    if (hovered && m_in.mouseDown) {
        float t = (m_in.mouseX - trackX) / trackW;
        if (t < 0.0f) t = 0.0f; if (t > 1.0f) t = 1.0f;
        nv = t;
    }
    if (hot) {
        if (m_in.navLeft)  nv -= 0.05f;
        if (m_in.navRight) nv += 0.05f;
    }
    if (nv < 0.0f) nv = 0.0f; if (nv > 1.0f) nv = 1.0f;
    if (nv != v) { value = nv; return true; }
    return false;
}

bool UiContext::knob(const char* label, float& value, float cx, float cy,
                     float radius, float danger01, float clock) {
    const int idx = m_widgetIndex++;
    // Hit box = the knob's bounding square (plus the caption strip below it).
    const float bx = cx - radius, by = cy - radius;
    const bool hovered = pointIn(bx, by, radius * 2.0f, radius * 2.0f);
    if (hovered) { m_focus = idx; m_mouseMovedFocus = true; }
    const bool hot = (m_focus == idx);

    float v = value; if (v < 0.0f) v = 0.0f; if (v > 1.0f) v = 1.0f;

    float lit[4], dim[4];
    glowColor(danger01, clock, (float)idx * 0.7f, lit);
    glowDim(lit, 0.25f, dim);

    // The dial sweeps 240 deg CLOCKWISE ON SCREEN: t=0 at 7 o'clock, t=0.5 straight
    // up at 12, t=1 at 5 o'clock, with a 120 deg dead zone across the bottom.
    //
    // The stamps use uy = -sin(a) (screen Y grows DOWN), which means a DECREASING
    // math angle travels clockwise on screen. So the sweep starts at 210 deg (7
    // o'clock: cos = -0.87, -sin = +0.5, i.e. left-and-down) and runs to -30 deg.
    // Getting this wrong is not cosmetic — a knob whose midpoint isn't at 12 o'clock
    // lies to the player about the value they are dialling in, and on this console a
    // mis-read value implodes a gate. (The first cut had 150 deg here, which put t=0
    // at TEN o'clock and 12 o'clock at t=0.25. --test-ui U29 caught it.)
    const float kA0    =  3.6651914f;   // 210 deg
    const float kSweep = -4.1887902f;   // -240 deg (clockwise on screen)
    auto ring = [&](float t, float rr, float len, float thick, const float col[4]) {
        const float a = kA0 + kSweep * t;
        const float ux = std::cos(a), uy = -std::sin(a);
        // A tick is a short quad laid along the radius at angle a (approximated by
        // stamping `len` little squares — the HUD layer only has axis-aligned quads).
        const int steps = (int)std::max(2.0f, len);
        for (int s = 0; s < steps; ++s) {
            const float r = rr + (float)s;
            quad(cx + ux * r - thick * 0.5f, cy + uy * r - thick * 0.5f,
                 thick, thick, col);
        }
    };
    // Recessed black-glass well.
    quad(bx - 2.0f, by - 2.0f, radius * 2.0f + 4.0f, radius * 2.0f + 4.0f, kGlassBg);
    // The dial RING: 44 stamps around the 240 deg sweep — lit up to the value,
    // dim past it (the same "charged fill" read as the slider).
    for (int s = 0; s <= 44; ++s) {
        const float t = (float)s / 44.0f;
        ring(t, radius - 5.0f, 4.0f, 3.0f, (t <= v) ? lit : dim);
    }
    // Glowing TICK MARKS every 25%.
    for (int s = 0; s <= 4; ++s)
        ring((float)s * 0.25f, radius + 1.0f, 4.0f, 3.0f, dim);
    // The INDICATOR: a bright lit mark from the hub out to the ring at the value.
    ring(v, radius * 0.30f, radius * 0.62f, hot ? 5.0f : 4.0f, lit);
    // Hub + caption.
    float hub[4] = { lit[0], lit[1], lit[2], kGlowMaxA };
    quad(cx - 4.0f, cy - 4.0f, 8.0f, 8.0f, hub);
    const float px = 12.0f;
    textCentered(label, cx, cy + radius + 4.0f, px, hot ? lit : dim, FontRole::HudMono);
    char pct[8];
    std::snprintf(pct, sizeof(pct), "%d", (int)(v * 100.0f + 0.5f));
    textCentered(pct, cx, cy + radius + 4.0f + px + 2.0f, px, lit, FontRole::HudMono);

    // ---- ANGULAR DRAG: the value follows the cursor's angle about the center ----
    float nv = v;
    if (hovered && m_in.mouseDown) {
        const float dx = m_in.mouseX - cx, dy = -(m_in.mouseY - cy);
        if (dx * dx + dy * dy > 16.0f) {                  // ignore a dead hub grab
            float a = std::atan2(dy, dx);                 // [-pi, pi]
            // Solve t from a = kA0 + kSweep*t, wrapped into the live sweep. The dead
            // zone (the 120 deg gap at the bottom) snaps to the nearer end instead of
            // letting the dial teleport 0 <-> 1.
            float t = (a - kA0) / kSweep;
            while (t < -0.5f) t += 6.2831853f / (-kSweep);
            while (t >  1.5f) t -= 6.2831853f / (-kSweep);
            if (t < 0.0f) t = (t < -0.25f) ? 1.0f : 0.0f;
            if (t > 1.0f) t = (t >  1.25f) ? 0.0f : 1.0f;
            nv = t;
        }
    }
    if (hot) {
        if (m_in.navLeft)  nv -= 0.05f;
        if (m_in.navRight) nv += 0.05f;
    }
    if (nv < 0.0f) nv = 0.0f; if (nv > 1.0f) nv = 1.0f;
    if (nv != v) { value = nv; return true; }
    return false;
}

bool UiContext::textField(const char* label, char* buf, int cap, float x, float y,
                          float w, float h, float danger01, float clock) {
    const int idx = m_widgetIndex++;
    const bool hovered = pointIn(x, y, w, h);
    if (hovered && m_in.mousePressed) { m_focus = idx; m_mouseMovedFocus = true; }
    else if (hovered) { m_focus = idx; m_mouseMovedFocus = true; }
    const bool hot = (m_focus == idx);

    float lit[4], dim[4];
    glowColor(danger01, clock, (float)idx * 0.7f, lit);
    glowDim(lit, 0.25f, dim);

    const float px = h * 0.44f;
    const float ty = y + (h - px) * 0.5f;
    text(label, x + 10.0f, ty, px, hot ? lit : dim, FontRole::HudMono);

    // The field: a black-glass well with a LIT BORDER + a glowing underline.
    const float fx = x + 150.0f;
    const float fw = std::max(40.0f, (x + w) - fx - 10.0f);
    quad(fx, y + 2.0f, fw, h - 4.0f, kGlassBg);
    const float bt = hot ? 2.0f : 1.0f;
    quad(fx, y + 2.0f, fw, bt, hot ? lit : dim);                 // top
    quad(fx, y + h - 2.0f - bt, fw, bt, hot ? lit : dim);        // underline (brighter)
    quad(fx, y + 2.0f, bt, h - 4.0f, hot ? lit : dim);
    quad(fx + fw - bt, y + 2.0f, bt, h - 4.0f, hot ? lit : dim);

    // Glowing typed text + a blinking caret while focused.
    if (buf) {
        text(buf, fx + 8.0f, ty, px, lit, FontRole::HudMono);
        if (hot) {
            const bool on = std::fmod(clock, 1.0f) < 0.55f;
            if (on) {
                const float cw = textWidth(FontRole::HudMono, buf, px);
                float caret[4] = { lit[0], lit[1], lit[2], kGlowMaxA };
                quad(fx + 8.0f + cw + 1.0f, ty, 2.0f, px, caret);
            }
        }
    }

    // ---- INPUT: consumed ONLY while this field owns focus --------------------
    bool committed = false;
    if (hot && buf) {
        int n = (int)std::strlen(buf);
        for (int i = 0; i < m_in.typedCount && n < cap - 1; ++i) {
            const char c = m_in.typed[i];
            if (c >= 32 && c < 127) buf[n++] = c;
        }
        buf[n] = 0;
        if (m_in.backspace && n > 0) buf[n - 1] = 0;
        if (m_in.enter) committed = true;   // caller PARSES it — no clamp: typing an
                                            // out-of-range value on purpose is the
                                            // intended road to the dangerous outcomes.
    }
    return committed;
}

void UiContext::bar(float x, float y, float w, float h, float frac,
                    const float fill[4], const char* caption) const {
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    const float border = 2.0f;
    const float plate[4] = { 0.0f, 0.0f, 0.0f, 0.55f };
    quad(x - border, y - border, w + 2 * border, h + 2 * border, plate);
    quad(x, y, w, h, kColTrack);
    quad(x, y, w * frac, h, fill);
    if (caption) {
        const float px = h * 0.75f;
        text(caption, x + 6.0f, y + (h - px) * 0.5f, px, kColText);
    }
}

// ===========================================================================
// MainMenu
// ===========================================================================
GameState MainMenu::update(UiContext& ui, const char* title, const char* subtitle,
                           int dispW, int dispH, bool& outSaveDefault) {
    const float w = (float)ui.screenW();
    const float h = (float)ui.screenH();
    if (w <= 0.0f || h <= 0.0f) return GameState::MainMenu;
    const float cx = w * 0.5f;

    // Backdrop wash so the title reads on any startup frame.
    const float wash[4] = { 0.02f, 0.03f, 0.05f, 0.55f };
    ui.quad(0, 0, w, h, wash);

    // Title (ORBITRON) + subtitle (Space Grotesk). Scale the title down so it always
    // fits the width with a margin, capped at 72 px. Orbitron is PROPORTIONAL + wide,
    // so size it from its TRUE rendered width at a probe size, not chars*px.
    using FontRole = UiContext::FontRole;
    const float titleCol[4] = { 0.35f, 0.85f, 1.0f, 1.0f };
    float titlePx = 72.0f;
    if (title && title[0]) {
        const float probe = 72.0f;
        const float probeW = UiContext::textWidth(FontRole::Title, title, probe);
        if (probeW > 1.0f) titlePx = std::min(72.0f, probe * (w * 0.92f) / probeW);
    }
    ui.textCentered(title, cx, h * 0.20f, titlePx, titleCol, FontRole::Title);
    const float subPx = std::max(14.0f, titlePx * 0.26f);
    const float subCol[4] = { 0.70f, 0.78f, 0.80f, 1.0f };
    ui.textCentered(subtitle, cx, h * 0.20f + titlePx + 12.0f, subPx, subCol, FontRole::Menu);

    // Buttons stacked + centered.
    const float bw = std::min(360.0f, w * 0.5f);
    const float bh = std::max(44.0f, h * 0.075f);
    const float gap = bh * 0.35f;
    float by = h * 0.48f;

    GameState next = GameState::MainMenu;
    if (ui.button("START", cx - bw * 0.5f, by, bw, bh)) next = GameState::Playing;
    by += bh + gap;
    if (ui.button("QUIT", cx - bw * 0.5f, by, bw, bh)) next = GameState::Quit;

    // Footer hint.
    const float hintPx = std::max(12.0f, subPx * 0.8f);
    ui.textCentered("Mouse or Arrows/WASD to navigate, Enter to select",
                    cx, h - hintPx * 2.2f, hintPx, kColTextDim);

    // Live framebuffer resolution (updates as the window is dragged) + a button that
    // asks the host to persist the current size as the startup default.
    char resBuf[64];
    std::snprintf(resBuf, sizeof(resBuf), "RESOLUTION:  %d x %d", dispW, dispH);
    ui.text(resBuf, 16.0f, h - 100.0f, 18.0f, kColText);
    if (ui.button("SET AS DEFAULT", 16.0f, h - 68.0f, 240.0f, 36.0f))
        outSaveDefault = true;
    return next;
}

// ===========================================================================
// PauseMenu
// ===========================================================================
GameState PauseMenu::update(UiContext& ui, PauseAction& outAction) {
    outAction = PauseAction::None;
    const float w = (float)ui.screenW();
    const float h = (float)ui.screenH();
    if (w <= 0.0f || h <= 0.0f) return GameState::Paused;
    const float cx = w * 0.5f;

    // Dim the frozen scene behind the menu.
    const float dim[4] = { 0.0f, 0.0f, 0.0f, 0.55f };
    ui.quad(0, 0, w, h, dim);

    // Panel (RESUME / TRAVEL / SAVE / LOAD / SETTINGS / QUIT = 6 buttons).
    const float pw = std::min(420.0f, w * 0.6f);
    const float ph = std::min(560.0f, h * 0.86f);
    const float px = cx - pw * 0.5f;
    const float py = h * 0.5f - ph * 0.5f;
    ui.panel(px, py, pw, ph, kColPanel);

    const float titlePx = std::max(24.0f, pw / 14.0f);
    const float titleCol[4] = { 0.40f, 0.88f, 1.0f, 1.0f };
    ui.textCentered("PAUSED", cx, py + 24.0f, titlePx, titleCol, UiContext::FontRole::Title);

    const float bw = pw - 48.0f;
    const float bh = std::max(38.0f, ph * 0.115f);
    const float gap = bh * 0.26f;
    float by = py + 24.0f + titlePx + 22.0f;

    GameState next = GameState::Paused;
    if (ui.button("RESUME", px + 24.0f, by, bw, bh))        next = GameState::Playing;
    by += bh + gap;
    // TRAVEL: the world / place selection menu — every place in the game, and an
    // honest word about how each one is reached. (Also on F6 in the canon loop.)
    if (ui.button("TRAVEL / WORLDS", px + 24.0f, by, bw, bh)) outAction = PauseAction::Worlds;
    by += bh + gap;
    if (ui.button("SAVE CHECKPOINT", px + 24.0f, by, bw, bh)) outAction = PauseAction::Save;
    by += bh + gap;
    if (ui.button("LOAD CHECKPOINT", px + 24.0f, by, bw, bh)) outAction = PauseAction::Load;
    by += bh + gap;
    if (ui.button("SETTINGS", px + 24.0f, by, bw, bh))      next = GameState::Settings;
    by += bh + gap;
    if (ui.button("QUIT TO MENU", px + 24.0f, by, bw, bh))  next = GameState::MainMenu;

    return next;
}

// ===========================================================================
// SettingsMenu
// ===========================================================================
// Settings-screen layout metrics, derived once from the screen size. Shared by the
// render (SettingsMenu::update) and the headless test so the test can compute the
// exact center of the Nth row without duplicating (and drifting from) the math.
namespace {
struct SettingsLayout {
    float cx, px, py, pw, ph, rx, rw, rh, gap, ry0;
    // Center y of row index `i` (0 = Bloom, ... see the call order in update()).
    float rowCenterY(int i) const { return ry0 + (rh + gap) * (float)i + rh * 0.5f; }
    float rowCenterX() const { return rx + rw * 0.5f; }
};
SettingsLayout computeSettingsLayout(float w, float h) {
    SettingsLayout L{};
    L.cx = w * 0.5f;
    L.pw = std::min(560.0f, w * 0.75f);
    // Taller panel: it hosts 10 rows (6 render toggles + Music toggle + 2 volume
    // sliders + resolution) plus title + Back, AND the extra gap*1.5 that separates
    // the audio group from the render toggles. Cap to 92% of the window height.
    L.ph = std::min(693.0f, h * 0.92f);   // was 680; +~gap*1.5 so the audio-group gap still fits
    L.px = L.cx - L.pw * 0.5f;
    L.py = h * 0.5f - L.ph * 0.5f;
    const float titlePx = std::max(24.0f, L.pw / 18.0f);
    L.rw = L.pw - 48.0f;
    L.rh = std::max(34.0f, L.ph * 0.062f);
    L.gap = L.rh * 0.20f;
    L.ry0 = L.py + 20.0f + titlePx + 20.0f;
    L.rx = L.px + 24.0f;
    return L;
}
} // namespace

GameState SettingsMenu::update(UiContext& ui, SettingsModel& model, GameState back,
                               bool& outChanged) {
    outChanged = false;
    const float w = (float)ui.screenW();
    const float h = (float)ui.screenH();
    if (w <= 0.0f || h <= 0.0f) return GameState::Settings;

    const SettingsLayout L = computeSettingsLayout(w, h);
    const float cx = L.cx;

    const float dim[4] = { 0.0f, 0.0f, 0.0f, 0.6f };
    ui.quad(0, 0, w, h, dim);

    const float pw = L.pw, ph = L.ph, px = L.px, py = L.py;
    ui.panel(px, py, pw, ph, kColPanel);

    const float titlePx = std::max(24.0f, pw / 18.0f);
    const float titleCol[4] = { 0.40f, 0.88f, 1.0f, 1.0f };
    ui.textCentered("SETTINGS", cx, py + 20.0f, titlePx, titleCol, UiContext::FontRole::Title);

    const float rw = L.rw;
    const float rh = L.rh;
    const float gap = L.gap;
    float ry = L.ry0;
    const float rx = L.rx;

    // Toggle rows (each takes one focus slot, in this order).
    if (ui.toggle("Bloom",       model.bloom,   rx, ry, rw, rh)) { model.bloom   = !model.bloom;   outChanged = true; } ry += rh + gap;
    if (ui.toggle("SSAO",        model.ssao,    rx, ry, rw, rh)) { model.ssao    = !model.ssao;    outChanged = true; } ry += rh + gap;
    if (ui.toggle("SSGI (GI)",   model.ssgi,    rx, ry, rw, rh)) { model.ssgi    = !model.ssgi;    outChanged = true; } ry += rh + gap;
    if (ui.toggle("Shadows",     model.shadows, rx, ry, rw, rh)) { model.shadows = !model.shadows; outChanged = true; } ry += rh + gap;
    if (ui.toggle("VSync",       model.vsync,   rx, ry, rw, rh)) { model.vsync   = !model.vsync;   outChanged = true; } ry += rh + gap;
    if (ui.toggle("RT AO (ray-traced)", model.rtao, rx, ry, rw, rh)) { model.rtao = !model.rtao;   outChanged = true; } ry += rh + gap;

    ry += gap * 1.5f;   // a little gap separating the AUDIO group from the render/display toggles

    // ---- Audio rows: Music on/off (toggle) + Music & SFX volume (0..1 sliders).
    // The host pushes these to the audio system live (setMusicEnabled / setMusicVolume
    // / setMasterSfxVolume) whenever outChanged fires. ----
    if (ui.toggle("Music",        model.musicOn, rx, ry, rw, rh)) { model.musicOn = !model.musicOn; outChanged = true; } ry += rh + gap;
    if (ui.slider("Music Volume", model.musicVol, rx, ry, rw, rh)) { outChanged = true; } ry += rh + gap;
    if (ui.slider("SFX Volume",   model.sfxVol,   rx, ry, rw, rh)) { outChanged = true; } ry += rh + gap;

    // Flight Mode row: label left + a CYCLE button right (Arcade -> Assist ->
    // Loose -> back). The host bridges model.flightMode to the space-pilot's
    // shared flight-mode latch and persists it (see UiController / app_run).
    {
        static const char* kFmNames[3] = { "ARCADE", "ASSIST", "LOOSE" };
        int fmIdx = model.flightMode; if (fmIdx < 0 || fmIdx > 2) fmIdx = 0;
        const float notePx = std::min(20.0f, std::max(14.0f, rh * 0.40f));
        ui.label("Flight Mode", rx + 4.0f, ry + (rh - notePx) * 0.5f, notePx, kColText);
        const float fbw = std::min(190.0f, rw * 0.46f);
        if (ui.button(kFmNames[fmIdx], rx + rw - fbw, ry, fbw, rh)) {
            model.flightMode = (fmIdx + 1) % 3;
            outChanged = true;
        }
    }
    ry += rh + gap;

    // Resolution row: LIVE framebuffer size on the left (updates as the window is
    // dragged) + a "SET DEFAULT" button on the RIGHT (where the old --width/--height
    // note used to sit). The button persists the current size as the startup default.
    {
        const uint32_t dw = model.dispW ? model.dispW : model.width;
        const uint32_t dh = model.dispH ? model.dispH : model.height;
        char resBuf[64];
        std::snprintf(resBuf, sizeof(resBuf), "RESOLUTION:  %u x %u", dw, dh);
        const float notePx = std::min(20.0f, std::max(14.0f, rh * 0.40f));
        ui.label(resBuf, rx + 4.0f, ry + (rh - notePx) * 0.5f, notePx, kColText);
        const float sdw = std::min(190.0f, rw * 0.46f);
        if (ui.button("SET DEFAULT", rx + rw - sdw, ry, sdw, rh)) {
            model.width = dw; model.height = dh;   // capture the current window size
            model.saveDefault = true;              // host persists it as the new default
            outChanged = true;
        }
    }
    ry += rh + gap;

    // Back button (one focus slot).
    const float bbw = std::min(200.0f, rw * 0.5f);
    GameState next = GameState::Settings;
    if (ui.button("BACK", cx - bbw * 0.5f, py + ph - rh - 16.0f, bbw, rh))
        next = back;
    return next;
}

// ===========================================================================
// GameHud — production HUD
// ===========================================================================
void GameHud::draw(UiContext& ui, const HudModel& m, float dt) {
    m_t += dt;
    const float w = (float)ui.screenW();
    const float h = (float)ui.screenH();
    if (w <= 0.0f || h <= 0.0f) return;

    // ---- Damage flash (full-screen red wash) -------------------------------
    if (m.damageFlash > 0.0f) {
        float s = m.damageFlash; if (s > 1.0f) s = 1.0f;
        const float red[4] = { 0.85f, 0.05f, 0.05f, 0.45f * s };
        ui.quad(0, 0, w, h, red);
    }

    // ---- Crosshair (center) ------------------------------------------------
    if (m.showCrosshair && m.alive) {
        const float ccx = w * 0.5f, ccy = h * 0.5f;
        const float arm = 8.0f, thick = 2.0f, g = 3.0f;
        const float col[4] = { 0.2f, 1.0f, 0.35f, 0.95f };
        ui.quad(ccx - g - arm, ccy - thick * 0.5f, arm, thick, col);
        ui.quad(ccx + g,       ccy - thick * 0.5f, arm, thick, col);
        ui.quad(ccx - thick * 0.5f, ccy - g - arm, thick, arm, col);
        ui.quad(ccx - thick * 0.5f, ccy + g,       thick, arm, col);
    }

    // ---- HP bar (bottom-left) ----------------------------------------------
    if (m.maxHp > 0) {
        int hp = m.hp; if (hp < 0) hp = 0; if (hp > m.maxHp) hp = m.maxHp;
        const float frac = (float)hp / (float)m.maxHp;
        const float barW = 280.0f, barH = 22.0f;
        const float bx = 18.0f;
        const float by = h - barH - 20.0f;
        float fill[4];
        if (frac > 0.5f) {            // green -> amber
            float t = (1.0f - frac) / 0.5f;
            fill[0] = 0.2f + 0.7f * t; fill[1] = 0.9f; fill[2] = 0.2f; fill[3] = 0.95f;
        } else {                      // amber -> red
            float t = (0.5f - frac) / 0.5f;
            fill[0] = 0.9f; fill[1] = 0.9f * (1.0f - t); fill[2] = 0.15f; fill[3] = 0.95f;
        }
        ui.bar(bx, by, barW, barH, frac, fill);
        char hpBuf[32];
        std::snprintf(hpBuf, sizeof(hpBuf), "HP %3d", hp);
        const float hpPx = 16.0f;
        // HP numeric -> the mono HUD font (fixed-width digits read steady).
        ui.text(hpBuf, bx + barW + 12.0f, by + (barH - hpPx) * 0.5f, hpPx, kColText,
                UiContext::FontRole::HudMono);
    }

    // ---- Weapon + ammo (bottom-right) --------------------------------------
    if (m.weapon && m.weapon[0]) {
        const float px = 18.0f;
        const float ammoPx = 30.0f;
        const float ay = h - ammoPx - 22.0f;
        if (m.isCharge) {
            // CHARGE weapon (Lightning): big charge number + a blue-electric bar
            // (fills to the stacking cap) instead of "MAG / RESERVE". Pulses red at
            // near-empty so the player feels the beam running dry.
            char chBuf[48];
            std::snprintf(chBuf, sizeof(chBuf), "%d", m.chargeCur);
            const int cap = (m.chargeCap > 0) ? m.chargeCap : 1;
            const float frac = (float)m.chargeCur / (float)cap;
            const float chW = UiContext::textWidth(UiContext::FontRole::HudMono, chBuf, ammoPx);
            const float ax = w - chW - px;
            const bool low = m.chargeCur <= (cap / 10);
            float col[4] = { 0.55f, 0.85f, 1.0f, 1.0f };   // blue-electric
            if (low) { const float pulse = 0.5f + 0.5f * std::sin(m_t * 8.0f);
                       col[0] = 1.0f; col[1] = 0.35f; col[2] = 0.3f; col[3] = 0.6f + 0.4f * pulse; }
            // PASSIVE REGEN: while the pool is refilling, the number BREATHES (a gentle
            // alpha pulse) so "it is coming back" reads at a glance without a second
            // widget. Low-charge red always wins — running dry outranks recovering.
            if (m.chargeRegen && !low) {
                const float breathe = 0.5f + 0.5f * std::sin(m_t * (m.chargeRegenSlow ? 1.6f : 3.2f));
                col[3] = 0.72f + 0.28f * breathe;   // pulse SLOWER in the half-speed band
            }
            ui.text(chBuf, ax, ay, ammoPx, col, UiContext::FontRole::HudMono);
            // Blue charge bar under the number.
            const float barW = 150.0f, barH = 8.0f;
            const float barX = w - barW - px, barY = ay + ammoPx + 4.0f;
            float fill[4] = { 0.3f, 0.7f, 1.0f, 0.95f };
            if (low) { fill[0] = 1.0f; fill[1] = 0.4f; fill[2] = 0.3f; }
            ui.bar(barX, barY, barW, barH, frac, fill);
            // THE SLOW-BAND NOTCH: a hairline at the charge where passive regen halves
            // (150 of 300). Above it the bar creeps at half speed — the player can SEE
            // where the crawl starts instead of having to be told.
            if (m.chargeSlowAbove > 0 && m.chargeSlowAbove < cap) {
                const float nx = barX + barW * ((float)m.chargeSlowAbove / (float)cap);
                const float notch[4] = { 0.85f, 0.92f, 1.0f, 0.75f };
                ui.quad(nx - 1.0f, barY - 2.0f, 2.0f, barH + 4.0f, notch);
            }
            // Label above the number: "CHARGE" normally, but the TRUTH while it refills —
            // and whether it is in the fast band or the half-speed crawl. Never a bare
            // "CHARGE" that hides the fact the gun is quietly recovering.
            const float labPx = 14.0f;
            const char* lab = "CHARGE";
            float labCol[4] = { kColTextDim[0], kColTextDim[1], kColTextDim[2], kColTextDim[3] };
            if (m.chargeRegen) {
                lab = m.chargeRegenSlow ? "RECHARGING - SLOW" : "RECHARGING";
                labCol[0] = 0.45f; labCol[1] = 0.85f; labCol[2] = 1.0f; labCol[3] = 0.9f;
            }
            const float labW = UiContext::textWidth(UiContext::FontRole::Menu, lab, labPx);
            ui.text(lab, w - labW - px, ay - labPx - 6.0f, labPx, labCol,
                    UiContext::FontRole::Menu);
        } else {
            // Ammo line: "MAG / RESERVE" big, weapon name above it.
            char ammoBuf[48];
            if (m.reloading) std::snprintf(ammoBuf, sizeof(ammoBuf), "RELOADING...");
            else             std::snprintf(ammoBuf, sizeof(ammoBuf), "%d / %d", m.ammoInMag, m.ammoReserve);
            // Ammo readout -> mono HUD font (steady-width digits); width query MATCHES role.
            const float ammoW  = UiContext::textWidth(UiContext::FontRole::HudMono, ammoBuf, ammoPx);
            const float ax = w - ammoW - px;
            // Low-ammo pulse (mag empty-ish): tint amber/red and pulse alpha.
            float col[4] = { 0.95f, 0.97f, 0.92f, 1.0f };
            if (!m.reloading && m.ammoInMag == 0) {
                const float pulse = 0.5f + 0.5f * std::sin(m_t * 8.0f);
                col[0] = 1.0f; col[1] = 0.3f; col[2] = 0.25f; col[3] = 0.6f + 0.4f * pulse;
            }
            ui.text(ammoBuf, ax, ay, ammoPx, col, UiContext::FontRole::HudMono);
        }
        // Weapon name above, right-aligned (Menu/Space Grotesk).
        const float namePx = 16.0f;
        const float nameW = UiContext::textWidth(UiContext::FontRole::Menu, m.weapon, namePx);
        ui.text(m.weapon, w - nameW - px, ay - namePx - 6.0f - (m.isCharge ? 18.0f : 0.0f), namePx,
                kColTextDim, UiContext::FontRole::Menu);
    }

    // ---- Objective: drawn GTA/Cyberpunk-style UNDER THE MINIMAP (see below, after
    // the minimap box is laid out). ----

    // ---- Enemies-remaining counter (TOP-LEFT, just under the FPS meter). <0 hides
    // it (non-combat HUDs / vantages). Reads "AREA CLEAR" in green when none remain. ----
    if (m.enemiesRemaining >= 0) {
        char enBuf[48];
        const bool clear = (m.enemiesRemaining == 0);
        if (clear) std::snprintf(enBuf, sizeof(enBuf), "AREA CLEAR");
        else       std::snprintf(enBuf, sizeof(enBuf), "ENEMIES: %d", m.enemiesRemaining);
        const float enPx = 15.0f;
        float enCol[4];
        if (clear) { enCol[0]=0.45f; enCol[1]=1.0f;  enCol[2]=0.55f; enCol[3]=1.0f; }
        else       { enCol[0]=1.0f;  enCol[1]=0.62f; enCol[2]=0.30f; enCol[3]=1.0f; }
        // Top-left, below the FPS/ms stats line (~y 8, ~24 tall). News font (Space
        // Mono Bold) — the event/ticker voice for pickups / "AREA CLEAR".
        ui.text(enBuf, 10.0f, 40.0f, enPx, enCol, UiContext::FontRole::News);
    }

    // ---- Minimap RADAR (top-right box) -------------------------------------
    // Player-centered top-down radar. World XZ is translated by -player then rotated
    // by -yaw so the player's FORWARD points UP in the box; a fixed meters->pixels
    // scale maps the local offset to box pixels (anything outside the box is culled).
    // Draw order: faint room outlines (under) -> enemy (red) / ally (pulsing green)
    // blips -> player blip (always centered, on top). Falls back to the old stub
    // box + "MAP" caption when the host hasn't fed the radar (radarValid=false).
    {
        const float mmW = 150.0f, mmH = 150.0f;
        const float mmx = w - mmW - 16.0f;
        const float mmy = 16.0f;
        const float frameCol[4] = { 0.10f, 0.13f, 0.18f, 0.55f };
        const float edgeCol[4]  = { 0.30f, 0.62f, 0.95f, 0.75f };
        ui.quad(mmx, mmy, mmW, mmH, frameCol);
        // 1px border.
        ui.quad(mmx, mmy, mmW, 1.0f, edgeCol);
        ui.quad(mmx, mmy + mmH - 1.0f, mmW, 1.0f, edgeCol);
        ui.quad(mmx, mmy, 1.0f, mmH, edgeCol);
        ui.quad(mmx + mmW - 1.0f, mmy, 1.0f, mmH, edgeCol);

        const float cxp = mmx + mmW * 0.5f;   // box center (player) in pixels
        const float cyp = mmy + mmH * 0.5f;
        const float half = mmW * 0.5f - 3.0f;  // usable radius inside the border

        if (m.radarValid) {
            // World->radar transform. Rotate the player-relative offset by -yaw so
            // forward (yaw dir) maps to screen-UP. With yaw 0 looking +X (world), the
            // player forward is (cos,sin) in XZ; we want forward -> -Y (up) on screen.
            // Screen Y grows DOWN, so up = -forward-distance. Right (screen +X) = the
            // player's right-hand side.
            const float mPerPx = 28.0f / half;       // ~28 m radius fits the box edge
            const float s = std::sin(m.playerYaw), c = std::cos(m.playerYaw);
            // Map a world XZ to a box pixel; returns false if outside the box.
            auto toRadar = [&](float wx, float wz, float& ox, float& oy) -> bool {
                const float dx = wx - m.playerX;     // world offset from player
                const float dz = wz - m.playerZ;
                // Forward component (along yaw dir) and right component.
                const float fwd   =  dx * c + dz * s;    // + = ahead of the player
                const float right = -dx * s + dz * c;    // + = to the player's right
                ox = cxp + (right / mPerPx);
                oy = cyp - (fwd   / mPerPx);             // ahead -> up (smaller y)
                return ox >= mmx + 1.0f && ox <= mmx + mmW - 1.0f &&
                       oy >= mmy + 1.0f && oy <= mmy + mmH - 1.0f;
            };

            // --- Faint room outlines (drawn first, under the blips). Each room is an
            // axis-aligned world rect; its 4 corners transform into the rotated radar
            // space, so we draw it as 4 thin edges between the projected corners. To
            // stay cheap + allocation-free we approximate each edge as a short run of
            // 1px dots (the radar is small; a handful of dots reads as a faint line).
            const float roomCol[4] = { 0.40f, 0.55f, 0.70f, 0.30f };  // dim blue-grey
            for (int r = 0; r < m.roomCount && r < HudModel::kMaxRooms; ++r) {
                const float rx = m.roomCx[r], rz = m.roomCz[r];
                const float hx = m.roomHx[r], hz = m.roomHz[r];
                // 4 corners in world XZ.
                const float cxw[4] = { rx - hx, rx + hx, rx + hx, rx - hx };
                const float czw[4] = { rz - hz, rz - hz, rz + hz, rz + hz };
                float px[4], py[4];
                for (int k = 0; k < 4; ++k) toRadar(cxw[k], czw[k], px[k], py[k]);
                // Draw each of the 4 edges as a dotted line, clamped into the box.
                for (int e = 0; e < 4; ++e) {
                    const int a = e, b = (e + 1) & 3;
                    const float ex = px[b] - px[a], ey = py[b] - py[a];
                    const float len = std::sqrt(ex * ex + ey * ey);
                    const int steps = (int)std::min(40.0f, std::max(1.0f, len / 4.0f));
                    for (int t = 0; t <= steps; ++t) {
                        const float f = (float)t / (float)steps;
                        const float dxp = px[a] + ex * f;
                        const float dyp = py[a] + ey * f;
                        if (dxp < mmx + 1.0f || dxp > mmx + mmW - 1.0f ||
                            dyp < mmy + 1.0f || dyp > mmy + mmH - 1.0f) continue;
                        ui.quad(dxp, dyp, 1.0f, 1.0f, roomCol);
                    }
                }
            }

            // --- Enemy blips (RED). Cull anything outside the box.
            const float enemyCol[4] = { 1.0f, 0.22f, 0.18f, 0.95f };
            for (int i = 0; i < m.enemyCount && i < HudModel::kMaxBlips; ++i) {
                float bx, by;
                if (!toRadar(m.enemyX[i], m.enemyZ[i], bx, by)) continue;
                ui.quad(bx - 2.0f, by - 2.0f, 4.0f, 4.0f, enemyCol);
            }

            // --- Ally (companion) blips (GREEN, SLOW FLASH). Pulse alpha at ~0.7 Hz
            // off the HUD clock so allies read as friendly + "active".
            const float pulse = 0.5f + 0.5f * std::sin(m_t * 4.4f);   // ~0.7 Hz
            const float allyCol[4] = { 0.25f, 1.0f, 0.40f, 0.35f + 0.6f * pulse };
            for (int i = 0; i < m.allyCount && i < HudModel::kMaxBlips; ++i) {
                float bx, by;
                if (!toRadar(m.allyX[i], m.allyZ[i], bx, by)) continue;
                ui.quad(bx - 2.5f, by - 2.5f, 5.0f, 5.0f, allyCol);
            }

            // --- Secret TRAPDOOR marker (GOLD, pulsing, larger than a blip) so the
            // cell floor hatch is findable. Four short ticks around it read as a
            // marked objective rather than just another dot.
            if (m.trapValid) {
                float bx, by;
                if (toRadar(m.trapX, m.trapZ, bx, by)) {
                    const float tpulse = 0.5f + 0.5f * std::sin(m_t * 5.2f);
                    const float gold[4] = { 1.0f, 0.80f, 0.15f, 0.55f + 0.40f * tpulse };
                    ui.quad(bx - 3.5f, by - 3.5f, 7.0f, 7.0f, gold);
                    const float tick[4] = { 1.0f, 0.92f, 0.45f, 0.45f * tpulse };
                    ui.quad(bx - 7.0f, by - 1.0f, 4.0f, 2.0f, tick);   // left
                    ui.quad(bx + 3.0f, by - 1.0f, 4.0f, 2.0f, tick);   // right
                    ui.quad(bx - 1.0f, by - 7.0f, 2.0f, 4.0f, tick);   // up
                    ui.quad(bx - 1.0f, by + 3.0f, 2.0f, 4.0f, tick);   // down
                }
            }

            // --- WORLD-MAP WAYPOINT (MAGENTA). Inside the radar radius it draws as
            // a small cross at its true spot; beyond it, it CLAMPS to the box edge
            // as a chevron (three ticks shrinking toward the waypoint direction)
            // with a "WP NNNm" distance readout under the radar box.
            if (m.wpValid) {
                const float mag[4] = { 1.0f, 0.35f, 0.95f, 0.95f };
                float bx, by;
                if (toRadar(m.wpX, m.wpZ, bx, by)) {
                    ui.quad(bx - 5.0f, by - 1.0f, 10.0f, 2.0f, mag);
                    ui.quad(bx - 1.0f, by - 5.0f, 2.0f, 10.0f, mag);
                } else {
                    // Edge-clamp the (rotated) radar-space direction to the box rim.
                    const float dxp = bx - cxp, dyp = by - cyp;
                    const float adx = std::fabs(dxp), ady = std::fabs(dyp);
                    const float k = (half - 4.0f) / std::max(1e-3f, std::max(adx, ady));
                    const float ex = cxp + dxp * k, ey = cyp + dyp * k;
                    // Chevron: three ticks stepping in from the rim toward the player.
                    const float il = std::sqrt(dxp * dxp + dyp * dyp);
                    const float ux = dxp / std::max(1e-3f, il), uy = dyp / std::max(1e-3f, il);
                    for (int t = 0; t < 3; ++t) {
                        const float sz = 5.0f - (float)t * 1.4f;
                        const float px = ex - ux * (float)t * 3.5f;
                        const float py = ey - uy * (float)t * 3.5f;
                        ui.quad(px - sz * 0.5f, py - sz * 0.5f, sz, sz, mag);
                    }
                }
                // Distance readout under the radar box.
                const float ddx = m.wpX - m.playerX, ddz = m.wpZ - m.playerZ;
                const int distM = (int)std::sqrt(ddx * ddx + ddz * ddz);
                char wpTxt[32];
                std::snprintf(wpTxt, sizeof(wpTxt), "WP %dm", distM);
                ui.textCentered(wpTxt, cxp, mmy + mmH + 6.0f, 12.0f, mag);
            }

            // --- Player blip (always centered, on top) + a forward "nose" tick.
            const float blip[4] = { 0.2f, 1.0f, 0.35f, 0.95f };
            ui.quad(cxp - 2.0f, cyp - 2.0f, 4.0f, 4.0f, blip);
            ui.quad(cxp - 1.0f, cyp - 8.0f, 2.0f, 6.0f, blip);   // points UP = forward
        } else {
            // No radar feed -> the legacy stub: a centered player blip + "MAP" caption.
            const float blip[4] = { 0.2f, 1.0f, 0.35f, 0.9f };
            ui.quad(cxp - 2.0f, cyp - 2.0f, 4.0f, 4.0f, blip);
            ui.textCentered("MAP", cxp, mmy + mmH - 18.0f, 12.0f, kColTextDim);
        }
    }

    // ---- Enemy NAMEPLATES (world-anchored threat labels) -------------------
    // Over each live, on-screen enemy within range, project the head position (body
    // center + ~1.8 m up) to the screen and draw a small Tektur-Condensed label.
    // worldToScreen returns false for points behind the camera / off-screen, so those
    // are skipped for free; we also distance-cull so far enemies don't clutter.
    if (m.radarValid && m.alive) {
        const float maxDist = 45.0f;             // metres; skip enemies farther than this
        for (int i = 0; i < m.enemyCount && i < HudModel::kMaxBlips; ++i) {
            if (!m.enemyVisible[i]) continue;         // occluded by a wall/door -> no label (radar still shows it)
            const float dx = m.enemyX[i] - m.playerX;
            const float dz = m.enemyZ[i] - m.playerZ;
            const float d2 = dx * dx + dz * dz;
            if (d2 > maxDist * maxDist) continue;
            float sx = 0.0f, sy = 0.0f;
            if (!ui.worldToScreen(m.enemyX[i], m.enemyY[i] + 1.8f, m.enemyZ[i], sx, sy))
                continue;                         // behind camera / off-screen
            const char* label = (m.enemyLabel[i] && m.enemyLabel[i][0]) ? m.enemyLabel[i]
                                                                        : "HOSTILE";
            // Fade slightly with distance so near threats read strongest.
            float a = 1.0f - (std::sqrt(d2) / maxDist) * 0.5f;   // 1.0 near -> 0.5 far
            const float plate[4] = { 1.0f, 0.32f, 0.28f, a };    // reddish threat tint
            ui.enemyNameplate(label, sx, sy, 15.0f, plate);
        }
    }

    // ---- Objective (GTA/Cyberpunk style): UNDER THE MINIMAP, right-aligned to the
    // map's right edge, a cyan header + the objective word-wrapped to <=3 lines in
    // Cyberpunk yellow (non-white). ----
    if (m.objective && m.objective[0]) {
        const float mmW = 150.0f, mmH = 150.0f, mmy = 16.0f;
        const float right = w - 16.0f;            // map's right edge
        const float maxW  = mmW + 90.0f;          // allow a touch wider than the map
        float oy = mmy + mmH + 8.0f;              // just under the map box
        // Header chip.
        const float hdrPx = 12.0f;
        const float hdrCol[4] = { 0.32f, 0.86f, 1.0f, 0.95f };   // cyan
        ui.text("OBJECTIVE", right - UiContext::textWidth("OBJECTIVE", hdrPx), oy, hdrPx, hdrCol);
        oy += hdrPx + 5.0f;
        // Word-wrap the body to up to 3 lines, Cyberpunk yellow.
        const float bodyPx = 15.0f;
        const float bodyCol[4] = { 0.99f, 0.92f, 0.07f, 1.0f };  // CP yellow
        std::string text = m.objective;
        std::string lines[3];
        int nLines = 0;
        std::string cur;
        size_t i = 0;
        while (i <= text.size() && nLines < 3) {
            size_t sp = text.find(' ', i);
            std::string word = text.substr(i, sp == std::string::npos ? text.size() - i : sp - i);
            std::string trial = cur.empty() ? word : cur + " " + word;
            if (cur.empty() || UiContext::textWidth(trial.c_str(), bodyPx) <= maxW) {
                cur = trial;
            } else {
                lines[nLines++] = cur;
                cur = word;
            }
            if (sp == std::string::npos) break;
            i = sp + 1;
        }
        if (!cur.empty() && nLines < 3) lines[nLines++] = cur;
        for (int li = 0; li < nLines; ++li) {
            ui.text(lines[li].c_str(), right - UiContext::textWidth(lines[li].c_str(), bodyPx),
                    oy, bodyPx, bodyCol);
            oy += bodyPx + 3.0f;
        }
    }

    // ---- Death banner ------------------------------------------------------
    if (!m.alive) {
        const float dark[4] = { 0.0f, 0.0f, 0.0f, 0.6f };
        ui.quad(0, 0, w, h, dark);
        const float big = std::min(64.0f, w / 12.0f);
        const float red[4] = { 0.9f, 0.12f, 0.12f, 1.0f };
        ui.textCentered("YOU DIED", w * 0.5f, h * 0.5f - big, big, red, UiContext::FontRole::Title);
        ui.textCentered("Respawning...", w * 0.5f, h * 0.5f + 8.0f, 16.0f, kColTextDim,
                        UiContext::FontRole::Menu);
    }
}

// ===========================================================================
// UiController
// ===========================================================================
void UiController::init(x3::rhi::IRenderDevice& device, x3::con::IConsole* console,
                        const SettingsModel& initial) {
    m_console  = console;
    m_settings = initial;
    m_state    = GameState::MainMenu;

    if (console) {
        // Register the settings cvars seeded from the initial model. These are the
        // single source of truth the host (and the SettingsMenu) read/write.
        console->registerCVar("ui_bloom",  initial.bloom   ? "1" : "0", "bloom on/off (UI settings)");
        console->registerCVar("r_ssao",    initial.ssao    ? "1" : "0", "SSAO on/off");
        console->registerCVar("r_ssgi",    initial.ssgi    ? "1" : "0", "SSGI / dynamic GI on/off");
        console->registerCVar("r_shadows", initial.shadows ? "1" : "0", "sun shadows on/off");
        console->registerCVar("r_vsync",   initial.vsync   ? "1" : "0", "vertical sync on/off");
    }
    applySettings(device, console);
}

void UiController::setTitle(const char* title, const char* subtitle) {
    if (title)    m_title    = title;
    if (subtitle) m_subtitle = subtitle;
}

void UiController::applySettings(x3::rhi::IRenderDevice& device, x3::con::IConsole* console) {
    // SSAO: real live param. enabled=false skips the SSAO chain (engine default on).
    x3::rhi::IRenderDevice::SsaoParams sp{};
    sp.enabled = m_settings.ssao;
    device.setSsaoParams(sp);

    // SSGI: real live param. enabled=false disables the GI chain.
    x3::rhi::IRenderDevice::GiParams gp{};
    gp.enabled = m_settings.ssgi;
    device.setGiParams(gp);

    // Shadows: the engine's shadow term is driven via the sky's sun; there is no
    // dedicated runtime shadow toggle in the RHI, so we keep the cvar authoritative
    // and let the host decide (note in settings). We DO apply vsync via the device
    // if it supports a live present-mode switch (added as a small additive method).
    device.setVsync(m_settings.vsync);

    // Mirror into cvars so the rest of the app + the console can read the state.
    if (console) {
        console->set("ui_bloom",  m_settings.bloom   ? "1" : "0");
        console->set("r_ssao",    m_settings.ssao    ? "1" : "0");
        console->set("r_ssgi",    m_settings.ssgi    ? "1" : "0");
        console->set("r_shadows", m_settings.shadows ? "1" : "0");
        console->set("r_vsync",   m_settings.vsync   ? "1" : "0");
        console->set("r_rtao",    m_settings.rtao    ? "1" : "0");   // hardware RT ambient occlusion
    }
}

void UiController::update(const UiInput& input, x3::rhi::IRenderDevice& device,
                          const x3::rhi::FrameContext& frame, const HudModel& hud, float dt) {
    m_ui.begin(device, frame, input);

    switch (m_state) {
        case GameState::MainMenu: {
            bool saveDef = false;
            const GameState next = m_main.update(m_ui, m_title.c_str(), m_subtitle.c_str(),
                                                 hud.dispW, hud.dispH, saveDef);
            if (saveDef) m_saveDefaults = true;
            if (next != m_state) m_state = next;
            break;
        }
        case GameState::Playing: {
            // Esc -> pause.
            if (input.navBack) { m_state = GameState::Paused; }
            else               { m_hud.draw(m_ui, hud, dt); }
            break;
        }
        case GameState::Paused: {
            // Esc resumes from pause.
            if (input.navBack) { m_state = GameState::Playing; }
            else {
                PauseAction action = PauseAction::None;
                const GameState next = m_pause.update(m_ui, action);
                if (action != PauseAction::None) m_pendingAction = action;  // host polls + clears
                if (next != m_state) m_state = next;
            }
            break;
        }
        case GameState::Settings: {
            bool changed = false;
            // Feed the LIVE framebuffer size so the resolution readout updates as the
            // window is dragged (mirrors the MainMenu readout).
            m_settings.dispW = (uint32_t)hud.dispW;
            m_settings.dispH = (uint32_t)hud.dispH;
            // Esc backs out to pause.
            GameState next = m_state;
            if (input.navBack) next = GameState::Paused;
            else               next = m_settingsScreen.update(m_ui, m_settings, GameState::Paused, changed);
            // "SET DEFAULT" button -> persist the current window size (same sink the
            // MainMenu SET-AS-DEFAULT uses).
            if (m_settings.saveDefault) { m_saveDefaults = true; m_settings.saveDefault = false; }
            if (changed) applySettings(device, m_console);
            if (next != m_state) m_state = next;
            break;
        }
        case GameState::Quit:
        default:
            break;
    }

    m_ui.end();
}

// ===========================================================================
// Self-test (--test-ui) — headless, no window / Vulkan.
// ===========================================================================
namespace {
// A headless render device for the UI logic tests, derived from the shared
// no-op HeadlessRenderDevice so it tracks the interface automatically. It only
// overrides what the UI tests need: a non-zero hudSize (so layout math runs) and
// observation of the SSAO/SSGI/vsync settings the controller applies.
class StubDevice final : public x3::game::HeadlessRenderDevice {
public:
    void hudSize(uint32_t& w, uint32_t& h) const override { w = 1280; h = 720; }
    void setSsaoParams(const SsaoParams& p) override { ssaoEnabled = p.enabled; }
    void setGiParams(const GiParams& p) override { ssgiEnabled = p.enabled; }
    void setVsync(bool v) override { vsyncOn = v; }

    bool ssaoEnabled = true, ssgiEnabled = true, vsyncOn = true;
};

bool nearlyHits(UiContext& ui, float bx, float by, float bw, float bh,
                float mx, float my) {
    // Drive one frame with a click at (mx,my) and report whether a button at the
    // given rect activates. The UiContext needs a begin/end around the widget.
    StubDevice dev;
    x3::rhi::FrameContext fc{};   // invalid -> draws skipped, hit-test still runs
    UiInput in{};
    in.mouseX = mx; in.mouseY = my; in.mouseDown = true; in.mousePressed = true;
    ui.begin(dev, fc, in);
    const bool hit = ui.button("X", bx, by, bw, bh);
    ui.end();
    return hit;
}
} // namespace

bool runUiSelfTest() {
    int pass = 0, fail = 0;
    auto check = [&](bool ok, const char* name) {
        if (ok) { ++pass; x3::logInfo(std::string("  PASS ") + name); }
        else    { ++fail; x3::logError(std::string("  FAIL ") + name); }
    };

    // ---- 1) Button hit-testing -------------------------------------------
    {
        UiContext ui;
        // Click inside a 100x40 button at (200,150): should activate.
        check(nearlyHits(ui, 200, 150, 100, 40, 250, 170), "U1 click inside button activates");
        // Click outside (far away): should NOT activate.
        check(!nearlyHits(ui, 200, 150, 100, 40, 50, 50), "U2 click outside button ignored");
        // Click just past the right edge (x==300 is exclusive): should NOT activate.
        check(!nearlyHits(ui, 200, 150, 100, 40, 300, 170), "U3 click on exclusive edge ignored");
    }

    // ---- 2) Keyboard focus + activation ----------------------------------
    {
        StubDevice dev; x3::rhi::FrameContext fc{};
        UiContext ui;
        // Mouse parked off the buttons so hover doesn't claim focus (keyboard-only).
        const float kOff = 9000.0f;
        // Frame A: emit 3 buttons, press navDown once -> focus should move 0->1.
        UiInput in{}; in.navDown = true; in.mouseX = kOff; in.mouseY = kOff;
        ui.begin(dev, fc, in);
        (void)ui.button("a", 0, 0, 10, 10);
        (void)ui.button("b", 0, 20, 10, 10);
        (void)ui.button("c", 0, 40, 10, 10);
        ui.end();
        check(ui.focus() == 1, "U4 navDown moves focus 0->1");
        // Frame B: navActivate with focus on index 1 -> that button returns true.
        UiInput in2{}; in2.navActivate = true; in2.mouseX = kOff; in2.mouseY = kOff;
        ui.begin(dev, fc, in2);
        bool a0 = ui.button("a", 0, 0, 10, 10);
        bool a1 = ui.button("b", 0, 20, 10, 10);
        bool a2 = ui.button("c", 0, 40, 10, 10);
        ui.end();
        check(!a0 && a1 && !a2, "U5 navActivate fires the focused button only");
    }

    // ---- 3) State machine: Menu <-> Playing <-> Paused -------------------
    {
        StubDevice dev;
        x3::con::IConsole* con = x3::con::createConsole();
        UiController ctl;
        SettingsModel sm{};
        ctl.init(dev, con, sm);
        check(ctl.state() == GameState::MainMenu, "U6 starts in MainMenu");
        check(ctl.shouldFreezeSim(), "U7 sim frozen in MainMenu");

        x3::rhi::FrameContext fc{};
        HudModel hud{};

        // Click START (the first button on the main menu). Compute its rect the
        // same way MainMenu does for 1280x720.
        {
            const float w = 1280.0f, h = 720.0f, cx = w * 0.5f;
            const float bw = std::min(360.0f, w * 0.5f);
            const float bh = std::max(44.0f, h * 0.075f);
            const float bx = cx - bw * 0.5f, by = h * 0.48f;
            UiInput in{}; in.mouseX = bx + bw * 0.5f; in.mouseY = by + bh * 0.5f;
            in.mouseDown = true; in.mousePressed = true;
            ctl.update(in, dev, fc, hud, 0.016f);
        }
        check(ctl.state() == GameState::Playing, "U8 START -> Playing");
        check(!ctl.shouldFreezeSim(), "U9 sim runs while Playing");
        check(!ctl.showCursor(),       "U10 cursor hidden while Playing");

        // Esc -> Paused.
        { UiInput in{}; in.navBack = true; ctl.update(in, dev, fc, hud, 0.016f); }
        check(ctl.state() == GameState::Paused, "U11 Esc -> Paused");
        check(ctl.shouldFreezeSim(), "U12 sim frozen while Paused");
        check(ctl.showCursor(),       "U13 cursor shown while Paused");

        // Esc again -> back to Playing.
        { UiInput in{}; in.navBack = true; ctl.update(in, dev, fc, hud, 0.016f); }
        check(ctl.state() == GameState::Playing, "U14 Esc again -> Playing");

        delete con;
    }

    // ---- 4) Settings toggle flips a cvar + the live param ----------------
    {
        StubDevice dev;
        x3::con::IConsole* con = x3::con::createConsole();
        UiController ctl;
        SettingsModel sm{};               // all defaults true
        ctl.init(dev, con, sm);
        // After init, SSAO is on (default) and the cvar reflects it.
        check(dev.ssaoEnabled == true, "U15 SSAO applied on init (on)");
        check(con->getInt("r_ssao") == 1, "U16 r_ssao cvar = 1 on init");

        // Flip SSAO off in the model + apply -> device + cvar follow.
        ctl.settings().ssao = false;
        ctl.applySettings(dev, con);
        check(dev.ssaoEnabled == false, "U17 SSAO disabled after apply");
        check(con->getInt("r_ssao") == 0, "U18 r_ssao cvar = 0 after apply");

        // Drive the Settings SCREEN: click the SSGI row to toggle it off via the UI.
        ctl.setState(GameState::Settings);
        x3::rhi::FrameContext fc{}; HudModel hud{};
        // Row order in SettingsMenu::update: 0 Bloom, 1 SSAO, 2 SSGI, 3 Shadows,
        // 4 VSync, 5 RT-AO, 6 Music (toggle), 7 Music Volume, 8 SFX Volume.
        const SettingsLayout L = computeSettingsLayout(1280.0f, 720.0f);
        {
            UiInput in{}; in.mouseX = L.rowCenterX(); in.mouseY = L.rowCenterY(2); // SSGI
            in.mouseDown = true; in.mousePressed = true;
            const bool ssgiBefore = ctl.settings().ssgi;
            ctl.update(in, dev, fc, hud, 0.016f);
            check(ctl.settings().ssgi != ssgiBefore, "U19 clicking SSGI row toggles the model");
            check(dev.ssgiEnabled == ctl.settings().ssgi, "U20 SSGI live param follows the toggle");
            check(con->getInt("r_ssgi") == (ctl.settings().ssgi ? 1 : 0), "U21 r_ssgi cvar follows the toggle");
        }

        // ---- Audio settings: Music toggle + Music/SFX sliders edit the model + the
        // screen reports outChanged so the host can push to the audio system. ----
        {
            // Defaults the model ships with.
            check(ctl.settings().musicOn == true,  "U22 musicOn defaults true");
            check(std::abs(ctl.settings().musicVol - 0.0f) < 1e-4f, "U23 musicVol default 0 (muted on boot)");
            check(std::abs(ctl.settings().sfxVol   - 1.0f)  < 1e-4f, "U24 sfxVol default ~1.0");

            // The audio group is pushed DOWN by an extra gap*1.5 (the separator added
            // before the Music row), so the audio rows (6/7/8) sit that much lower than
            // a plain rowCenterY() would compute. Add it back here for the click Y.
            const float audioGap = L.gap * 1.5f;

            // Click the Music toggle row (index 6) -> musicOn flips.
            const bool musicBefore = ctl.settings().musicOn;
            { UiInput in{}; in.mouseX = L.rowCenterX(); in.mouseY = L.rowCenterY(6) + audioGap;
              in.mouseDown = true; in.mousePressed = true;
              ctl.update(in, dev, fc, hud, 0.016f); }
            check(ctl.settings().musicOn != musicBefore, "U25 clicking Music row toggles musicOn");

            // Click near the LEFT end of the Music Volume slider track (index 7) ->
            // value drops toward 0. The track starts at rx + labelW (164).
            {
                const float trackX = L.rx + 164.0f;
                UiInput in{}; in.mouseX = trackX + 2.0f; in.mouseY = L.rowCenterY(7) + audioGap;
                in.mouseDown = true; in.mousePressed = true;
                ctl.update(in, dev, fc, hud, 0.016f);
                check(ctl.settings().musicVol < 0.1f, "U26 dragging Music slider to the left lowers musicVol");
            }
            // Click near the RIGHT end of the SFX Volume slider (index 8) -> ~1.0.
            {
                const float trackR = L.rx + L.rw - 56.0f - 16.0f;  // pctW=56, margin=16
                ctl.settings().sfxVol = 0.0f;                       // start low so the change is visible
                UiInput in{}; in.mouseX = trackR - 1.0f; in.mouseY = L.rowCenterY(8) + audioGap;
                in.mouseDown = true; in.mousePressed = true;
                ctl.update(in, dev, fc, hud, 0.016f);
                check(ctl.settings().sfxVol > 0.9f, "U27 dragging SFX slider to the right raises sfxVol");
            }
        }
        delete con;
    }

    // =======================================================================
    // ROUND 8 — THE GLOWING CONTROL SURFACE (glowSlider / knob / textField).
    // These drive the rift console, where a mis-set value implodes a gate — so
    // they are gated like gameplay, not like chrome.
    // =======================================================================
    {
        StubDevice dev;
        x3::rhi::FrameContext frame{};   // invalid -> hit-testing runs, drawing is skipped
        UiContext ui;

        // U28 — GLOW SLIDER drag. Press at 90% along the track and the value must
        //       follow the cursor (the track starts 150 px in, per the widget).
        {
            UiInput in{};
            in.mouseX = 100.0f + 150.0f + (600.0f - 150.0f - 52.0f - 12.0f) * 0.9f;
            in.mouseY = 200.0f + 17.0f;
            in.mouseDown = true;
            float v = 0.10f;
            ui.begin(dev, frame, in);
            const bool moved = ui.glowSlider("POWER", v, 100.0f, 200.0f, 600.0f, 34.0f,
                                             /*danger*/0.0f, /*clock*/0.0f);
            ui.end();
            check(moved && v > 0.85f && v <= 1.0f,
                  "U28 glowSlider: dragging near the right rail drives the value up");
        }

        // U29 — KNOB angular drag. The dial sweeps 240 deg from 7 o'clock (t=0) to
        //       5 o'clock (t=1), so a cursor placed straight UP from the center is
        //       the MIDPOINT of the sweep -> ~0.5. This is the whole point of a
        //       rotary: you grab it and turn it, you do not slide it.
        {
            const float cx = 400.0f, cy = 300.0f, r = 46.0f;
            UiInput in{};
            in.mouseX = cx;              // straight up (screen Y grows DOWN)
            in.mouseY = cy - 30.0f;
            in.mouseDown = true;
            float v = 0.0f;
            ui.begin(dev, frame, in);
            const bool turned = ui.knob("FREQUENCY", v, cx, cy, r, 0.0f, 0.0f);
            ui.end();
            check(turned && std::fabs(v - 0.5f) < 0.08f,
                  "U29 knob: an angular drag to 12 o'clock lands mid-sweep (~0.5)");
        }

        // U30 — TEXT FIELD: typed characters land, backspace edits, ENTER commits —
        //       and NONE of it happens unless the field owns focus. That focus gate
        //       is the same discipline the cell terminal enforces: while a field is
        //       taking input, the input belongs to it and nothing else.
        {
            char buf[24] = {};
            // Focus the field by hovering it, and type "club".
            UiInput in{};
            in.mouseX = 300.0f; in.mouseY = 415.0f;    // inside the row
            in.typed[0] = 'c'; in.typed[1] = 'l'; in.typed[2] = 'u'; in.typed[3] = 'b';
            in.typedCount = 4;
            ui.begin(dev, frame, in);
            ui.textField("TARGET", buf, (int)sizeof(buf), 100.0f, 400.0f, 600.0f, 34.0f,
                         0.0f, 0.0f);
            ui.end();
            const bool typedOk = std::strcmp(buf, "club") == 0;

            // Backspace trims one char.
            UiInput bs{};
            bs.mouseX = 300.0f; bs.mouseY = 415.0f;
            bs.backspace = true;
            ui.begin(dev, frame, bs);
            ui.textField("TARGET", buf, (int)sizeof(buf), 100.0f, 400.0f, 600.0f, 34.0f,
                         0.0f, 0.0f);
            ui.end();
            const bool bsOk = std::strcmp(buf, "clu") == 0;

            // ENTER commits.
            UiInput en{};
            en.mouseX = 300.0f; en.mouseY = 415.0f;
            en.enter = true;
            ui.begin(dev, frame, en);
            const bool committed =
                ui.textField("TARGET", buf, (int)sizeof(buf), 100.0f, 400.0f, 600.0f,
                             34.0f, 0.0f, 0.0f);
            ui.end();

            // ...and with the cursor PARKED SOMEWHERE ELSE (focus on widget 0, not
            // the field), the same keystrokes must be ignored entirely.
            char other[24] = {};
            UiInput away{};
            away.mouseX = 300.0f; away.mouseY = 215.0f;   // hovering the slider row
            away.typed[0] = 'x'; away.typedCount = 1;
            ui.begin(dev, frame, away);
            float dummy = 0.5f;
            ui.glowSlider("POWER", dummy, 100.0f, 200.0f, 600.0f, 34.0f, 0.0f, 0.0f);
            ui.textField("TARGET", other, (int)sizeof(other), 100.0f, 400.0f, 600.0f,
                         34.0f, 0.0f, 0.0f);
            ui.end();
            const bool ignoredWhenUnfocused = other[0] == 0;

            check(typedOk && bsOk && committed && ignoredWhenUnfocused,
                  "U30 textField: types, backspaces, commits on ENTER — and eats "
                  "nothing while unfocused");
        }
    }

    // ---- U31: THE WORLD / PLACE SELECTION MENU ------------------------------
    // The world menu is a SCREEN built on these widgets (app/world_menu.*), so its
    // gate lives here with the rest of the UI: every registry destination gets a row,
    // unreachable ones take no focus slot (they are unpickable, not merely grey), and
    // activating the focused row returns that destination.
    check(x3::game::runWorldMenuSelfTest(),
          "U31 world menu: a row per destination, unavailable rows unpickable, "
          "activate picks the focused place");

    x3::logInfo(std::string("--test-ui: ") + std::to_string(pass) + " passed, " +
                std::to_string(fail) + " failed");
    return fail == 0;
}

} // namespace x3::ui
