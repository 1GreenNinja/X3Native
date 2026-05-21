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
    void onBackspace();
    void historyPrev();
    void historyNext();
    void complete(x3::con::IConsole& console);

    // ---- Per-frame draw ----------------------------------------------------
    // Smooth + draw "FPS NNN  M.M ms" top-left (gated by the hud_fps cvar).
    void drawFps(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                 x3::con::IConsole& console, float dt);
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

    // Console open/close slide animation: 0 = fully hidden (above the top edge),
    // 1 = fully on-screen. Advanced by dt in drawConsole toward the logical open
    // state; the panel Y is lerped by the eased value so it slides down/up.
    float       m_consoleAnim = 0.0f;
};

} // namespace x3::game
