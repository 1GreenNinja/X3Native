#pragma once
// Screen-space HUD layer (S7): FPS / frame-time meter, on-screen console (wired
// to the D6 IConsole backend), and a 2D crosshair. All drawing goes through the
// engine's screen-space overlay API (IRenderDevice::drawHudQuad / drawHudText /
// hudSize) — no Vulkan types here.
//
// Clean-room: built only from the public IRenderDevice + IConsole interfaces.
// No id Tech / RBDOOM source consulted. The bitmap font is the public-domain
// font8x8_basic set, embedded in the render device.

#include "engine/rhi/IRenderDevice.h"
#include "engine/core/IConsole.h"

#include <string>
#include <vector>

namespace x3::game {

// Owns HUD state (FPS smoothing + console UI) and renders the overlay each frame.
// The console UI is a thin front-end over an x3::con::IConsole: text input is
// captured here, submitted via exec(), and scrollback is read via outputLines().
class Hud {
public:
    // Bind to the console backend and register a few demo commands/cvars
    // (hud_fps toggle, quit, clear, plus the backend's built-ins). `quitFlag`
    // is set true when the user runs the `quit` command.
    void init(x3::con::IConsole& console, bool* quitFlag);

    // ---- Console open/close + input routing -------------------------------
    bool consoleOpen() const { return m_consoleOpen; }
    void toggleConsole();
    void closeConsole() { m_consoleOpen = false; }

    // GLFW char callback feed (printable Unicode codepoint). Ignored unless open
    // and not the toggle key '`'/'~'.
    void onChar(unsigned int codepoint);
    // Editing keys while open. Enter submits, Backspace deletes, Up/Down recall
    // history, Tab completes against the backend. Returns true if consumed.
    void onEnter(x3::con::IConsole& console);

    // Scrollback: shift the visible output window UP (positive delta = older
    // lines) or DOWN (negative = toward the live bottom). Driven by PAGE_UP /
    // PAGE_DOWN and the mouse wheel while the console is open. The lower bound is
    // clamped here (>=0); the upper bound depends on the panel size and is
    // clamped each frame in drawConsole. A no-op while the console is closed.
    void consoleScroll(int deltaLines);
    void onBackspace();
    void historyPrev();
    void historyNext();
    void complete(x3::con::IConsole& console);

    // ---- Per-frame draw ----------------------------------------------------
    // Smooth + draw "FPS NNN  M.M ms" top-left (gated by the hud_fps cvar).
    void drawFps(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                 x3::con::IConsole& console, float dt);
    // ---- Perf instrumentation overlay (r_stats / r_speeds) ----------------
    // Multi-line "stats" panel below the FPS line: FPS, CPU ms (smoothed), GPU ms
    // (timestamp queries), draw calls, triangles, visible objects. Gated by the
    // `r_stats` cvar (0/1); the host can also force it via the `force` arg (F3).
    // CPU ms reuses the same smoothed dt as drawFps. No-op if hidden / headless.
    void drawStats(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                   x3::con::IConsole& console, float dt, bool force = false);
    // Crisp 2D crosshair at the framebuffer center.
    void drawCrosshair(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame) const;
    // ---- Phase 2a: player health + damage feedback ------------------------
    // Bottom-left health bar (background + fill scaled by hp/maxHp) + an "HP NNN"
    // number. The fill tints green->amber->red as health drops. No-op if the HUD
    // size is 0 (headless).
    void drawHealth(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                    int hp, int maxHp) const;
    // Brief translucent red full-screen flash when hit. `strength` in [0,1] (1 just
    // after a hit, fading to 0); a 0 strength draws nothing.
    void drawDamageFlash(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                         float strength) const;
    // "YOU DIED" overlay while dead: a dark vignette + centered red text, plus a
    // small "Respawning..." line. Drawn while the player is in the death state.
    void drawDeathOverlay(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame) const;

    // ---- LIVING WORLD: facility alert indicator (pillar 3) -----------------
    // Subtle top-right indicator: four pips (filled up to the alert level) + the
    // level name, tinting amber -> red as it climbs; pulses + adds a faint red
    // screen edge while in LOCKDOWN (redShift > 0). Draws NOTHING at level 0.
    // `time` drives the lockdown pulse (pass an accumulating seconds clock).
    void drawAlert(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                   int level, float redShift, float time) const;

    // ---- VIGIL BARK toast: the ambient companion line. A "VIGIL: ..." caption
    // in his terminal-orange voice on a dark plate, low-center of the screen, word-
    // wrapped, fading with `alpha` (0..1). Draws nothing when alpha <= 0 or empty.
    // The text does NOT include the "VIGIL:" prefix — this adds the styled label.
    void drawVigilBark(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                       const char* text, float alpha) const;

    // Semi-transparent panel + scrollback + input line. Slides down from the top
    // edge when opening and back up when closing over ~0.18 s (smoothstep). The
    // panel is drawn whenever the slide animation is in progress (anim > 0) so the
    // close animation is visible; input routing stays tied to consoleOpen(). `dt`
    // advances the animation toward the logical open state.
    void drawConsole(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                     x3::con::IConsole& console, float dt);

private:
    // FPS exponential moving average (seconds/frame).
    float m_emaDt = 1.0f / 60.0f;
    bool  m_emaSeeded = false;

    // Console UI state.
    bool        m_consoleOpen = false;
    std::string m_input;
    std::vector<std::string> m_history;   // submitted command lines
    int         m_historyPos = -1;        // -1 == editing a fresh line

    // Scrollback offset: how many lines the visible output window is shifted UP
    // from the live bottom (0 = pinned to the newest line). Reset to 0 (live) on
    // any new input or on open/close. Upper-clamped to the scrollable range in
    // drawConsole each frame (it needs the panel height to know how many rows fit).
    int         m_consoleScroll = 0;

    // Console open/close slide animation: 0 = fully hidden (above the top edge),
    // 1 = fully on-screen. Advanced by dt in drawConsole toward the logical open
    // state; the panel Y is lerped by the eased value so it slides down/up.
    float       m_consoleAnim = 0.0f;
};

// THE THERMOMETER. A real instrument -- bulb, tube, a column that rises,
// and a scale with the freezing mark on it -- rather than a number in a
// corner. The point of drawing it as a thermometer at all is that you can
// read "nearly freezing" from the column's position against that mark
// WITHOUT reading the digits, at speed, which is exactly when it matters.
//
//   tempF        -- air temperature, FAHRENHEIT (the sample stores C;
//                   convert at this boundary and nowhere else)
//   condition    -- surfaceConditionName(): "dry"/"wet"/"ice"/"snow"...
//   snowInches   -- lying depth; the depth line is drawn only when > 0
//   iceWarn      -- the surface is actually icy, not merely cold
void drawThermometer(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                     float tempF, const char* condition,
                     float snowInches, bool iceWarn);

// ---------------------------------------------------------------------------
// ROUND HUD PRIMITIVES — curves built from the one primitive the render API
// actually has (drawHudQuad), as 1-pixel scanline strips.
//
// The HUD had no way to draw anything that was not an axis-aligned rectangle,
// which is why Tim called the thermometer "very 1998 basic blocky": its bulb
// was a rectangle because a rectangle was the only option. Any gauge, dial,
// rounded panel or badge wants these — use them instead of adding another
// square-cornered box.
// ---------------------------------------------------------------------------
void hudDisc(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
             float cx, float cy, float r, const float rgba[4]);
void hudRoundRect(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                  float x, float y, float w, float h, float r, const float rgba[4]);


} // namespace x3::game
