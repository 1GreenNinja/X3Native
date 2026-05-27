// X3 GENERAL game-UI layer. See app/ui.h.
//
// Clean-room: built only from the public IRenderDevice + IConsole interfaces.
// No id Tech / RBDOOM source consulted.
#include "ui.h"
#include "headless_device.h"   // shared no-op IRenderDevice (for --test-ui)

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

    // Panel (taller now: RESUME / SAVE / LOAD / SETTINGS / QUIT = 5 buttons).
    const float pw = std::min(420.0f, w * 0.6f);
    const float ph = std::min(480.0f, h * 0.78f);
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
GameState SettingsMenu::update(UiContext& ui, SettingsModel& model, GameState back,
                               bool& outChanged) {
    outChanged = false;
    const float w = (float)ui.screenW();
    const float h = (float)ui.screenH();
    if (w <= 0.0f || h <= 0.0f) return GameState::Settings;
    const float cx = w * 0.5f;

    const float dim[4] = { 0.0f, 0.0f, 0.0f, 0.6f };
    ui.quad(0, 0, w, h, dim);

    const float pw = std::min(560.0f, w * 0.75f);
    const float ph = std::min(560.0f, h * 0.85f);
    const float px = cx - pw * 0.5f;
    const float py = h * 0.5f - ph * 0.5f;
    ui.panel(px, py, pw, ph, kColPanel);

    const float titlePx = std::max(24.0f, pw / 18.0f);
    const float titleCol[4] = { 0.40f, 0.88f, 1.0f, 1.0f };
    ui.textCentered("SETTINGS", cx, py + 20.0f, titlePx, titleCol, UiContext::FontRole::Title);

    const float rw = pw - 48.0f;
    const float rh = std::max(38.0f, ph * 0.085f);
    const float gap = rh * 0.22f;
    float ry = py + 20.0f + titlePx + 20.0f;
    const float rx = px + 24.0f;

    // Toggle rows (each takes one focus slot, in this order).
    if (ui.toggle("Bloom",       model.bloom,   rx, ry, rw, rh)) { model.bloom   = !model.bloom;   outChanged = true; } ry += rh + gap;
    if (ui.toggle("SSAO",        model.ssao,    rx, ry, rw, rh)) { model.ssao    = !model.ssao;    outChanged = true; } ry += rh + gap;
    if (ui.toggle("SSGI (GI)",   model.ssgi,    rx, ry, rw, rh)) { model.ssgi    = !model.ssgi;    outChanged = true; } ry += rh + gap;
    if (ui.toggle("Shadows",     model.shadows, rx, ry, rw, rh)) { model.shadows = !model.shadows; outChanged = true; } ry += rh + gap;
    if (ui.toggle("VSync",       model.vsync,   rx, ry, rw, rh)) { model.vsync   = !model.vsync;   outChanged = true; } ry += rh + gap;
    if (ui.toggle("RT AO (ray-traced)", model.rtao, rx, ry, rw, rh)) { model.rtao = !model.rtao;   outChanged = true; } ry += rh + gap;

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
        // Ammo line: "MAG / RESERVE" big, weapon name above it.
        char ammoBuf[48];
        if (m.reloading) std::snprintf(ammoBuf, sizeof(ammoBuf), "RELOADING...");
        else             std::snprintf(ammoBuf, sizeof(ammoBuf), "%d / %d", m.ammoInMag, m.ammoReserve);
        const float ammoPx = 30.0f;
        // Ammo readout -> mono HUD font (steady-width digits); width query MATCHES role.
        const float ammoW  = UiContext::textWidth(UiContext::FontRole::HudMono, ammoBuf, ammoPx);
        const float ax = w - ammoW - px;
        const float ay = h - ammoPx - 22.0f;
        // Low-ammo pulse (mag empty-ish): tint amber/red and pulse alpha.
        float col[4] = { 0.95f, 0.97f, 0.92f, 1.0f };
        if (!m.reloading && m.ammoInMag == 0) {
            const float pulse = 0.5f + 0.5f * std::sin(m_t * 8.0f);
            col[0] = 1.0f; col[1] = 0.3f; col[2] = 0.25f; col[3] = 0.6f + 0.4f * pulse;
        }
        ui.text(ammoBuf, ax, ay, ammoPx, col, UiContext::FontRole::HudMono);
        // Weapon name above, right-aligned to the ammo line (Menu/Space Grotesk).
        const float namePx = 16.0f;
        const float nameW = UiContext::textWidth(UiContext::FontRole::Menu, m.weapon, namePx);
        ui.text(m.weapon, w - nameW - px, ay - namePx - 6.0f, namePx, kColTextDim,
                UiContext::FontRole::Menu);
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
        // SSGI is the 3rd toggle row; compute its rect like SettingsMenu does.
        {
            const float w = 1280.0f, h = 720.0f, cx = w * 0.5f;
            const float pw = std::min(560.0f, w * 0.75f);
            const float ph = std::min(560.0f, h * 0.85f);
            const float px = cx - pw * 0.5f, py = h * 0.5f - ph * 0.5f;
            const float titlePx = std::max(24.0f, pw / 18.0f);
            const float rw = pw - 48.0f;
            const float rh = std::max(38.0f, ph * 0.085f);
            const float gap = rh * 0.22f;
            const float rx = px + 24.0f;
            float ry = py + 20.0f + titlePx + 20.0f;
            ry += (rh + gap) * 2.0f;   // skip Bloom (0) + SSAO (1) -> SSGI (2)
            UiInput in{}; in.mouseX = rx + rw * 0.5f; in.mouseY = ry + rh * 0.5f;
            in.mouseDown = true; in.mousePressed = true;
            const bool ssgiBefore = ctl.settings().ssgi;
            ctl.update(in, dev, fc, hud, 0.016f);
            check(ctl.settings().ssgi != ssgiBefore, "U19 clicking SSGI row toggles the model");
            check(dev.ssgiEnabled == ctl.settings().ssgi, "U20 SSGI live param follows the toggle");
            check(con->getInt("r_ssgi") == (ctl.settings().ssgi ? 1 : 0), "U21 r_ssgi cvar follows the toggle");
        }
        delete con;
    }

    x3::logInfo(std::string("--test-ui: ") + std::to_string(pass) + " passed, " +
                std::to_string(fail) + " failed");
    return fail == 0;
}

} // namespace x3::ui
