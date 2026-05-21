// Screen-space HUD layer (S7). See app/hud.h.
//
// Clean-room: built only from the public IRenderDevice + IConsole interfaces.
// No id Tech / RBDOOM source consulted.
#include "hud.h"

#include <cstdio>
#include <algorithm>

namespace x3::game {

namespace {
// Glyph cell size in pixels for HUD text (the 8x8 font scaled up).
constexpr float kGlyphPx = 16.0f;
// Console panel covers the top fraction of the screen.
constexpr float kConsoleHeightFrac = 0.45f;
// Console open/close slide duration (seconds). The anim value crosses [0..1] in
// this time, so 1/kConsoleSlideTime is the per-second rate.
constexpr float kConsoleSlideTime = 0.18f;

// Smoothstep ease (3t^2 - 2t^3) on [0,1]; flat slopes at both ends.
float smoothstep01(float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return t * t * (3.0f - 2.0f * t);
}
} // namespace

void Hud::init(x3::con::IConsole& console, bool* quitFlag) {
    // hud_fps: 1 shows the FPS/frame-time meter, 0 hides it.
    console.registerCVar("hud_fps", "1", "show the FPS / frame-time meter (0/1)");

    console.registerCommand("quit", [quitFlag](const std::vector<std::string>&) {
        if (quitFlag) *quitFlag = true;
    }, "exit the game");

    console.registerCommand("fps", [&console](const std::vector<std::string>& a) {
        // `fps` toggles; `fps 0|1` sets explicitly.
        if (!a.empty()) { console.set("hud_fps", a[0]); }
        else            { console.set("hud_fps", console.getInt("hud_fps") ? "0" : "1"); }
        console.print(std::string("hud_fps = ") + console.getString("hud_fps"));
    }, "toggle/set the FPS meter");

    console.print("X3 console ready. type 'help' for commands, '~' to close.");
}

// ---- Console open/close + input routing -----------------------------------
void Hud::toggleConsole() {
    m_consoleOpen = !m_consoleOpen;
    if (m_consoleOpen) { m_historyPos = -1; }
}

void Hud::onChar(unsigned int codepoint) {
    if (!m_consoleOpen) return;
    // Ignore the toggle keys and anything outside printable ASCII (the embedded
    // font only covers 0x20..0x7E).
    if (codepoint == '`' || codepoint == '~') return;
    if (codepoint >= 0x20 && codepoint < 0x7F) {
        m_input.push_back(static_cast<char>(codepoint));
        m_historyPos = -1;
    }
}

void Hud::onEnter(x3::con::IConsole& console) {
    if (!m_consoleOpen) return;
    if (m_input.empty()) return;
    console.print(std::string("> ") + m_input);   // echo the entered line
    console.exec(m_input);
    m_history.push_back(m_input);
    m_input.clear();
    m_historyPos = -1;
}

void Hud::onBackspace() {
    if (!m_consoleOpen || m_input.empty()) return;
    m_input.pop_back();
    m_historyPos = -1;
}

void Hud::historyPrev() {
    if (!m_consoleOpen || m_history.empty()) return;
    if (m_historyPos == -1) m_historyPos = static_cast<int>(m_history.size()) - 1;
    else if (m_historyPos > 0) --m_historyPos;
    m_input = m_history[m_historyPos];
}

void Hud::historyNext() {
    if (!m_consoleOpen || m_history.empty() || m_historyPos == -1) return;
    if (m_historyPos < static_cast<int>(m_history.size()) - 1) {
        ++m_historyPos;
        m_input = m_history[m_historyPos];
    } else {
        m_historyPos = -1;
        m_input.clear();
    }
}

void Hud::complete(x3::con::IConsole& console) {
    if (!m_consoleOpen || m_input.empty()) return;
    auto matches = console.complete(m_input);
    if (matches.empty()) return;
    if (matches.size() == 1) {
        m_input = matches[0] + " ";
    } else {
        // Multiple: list them and extend to the longest common prefix.
        std::string lcp = matches[0];
        for (size_t i = 1; i < matches.size(); ++i) {
            size_t n = 0;
            while (n < lcp.size() && n < matches[i].size() && lcp[n] == matches[i][n]) ++n;
            lcp.resize(n);
        }
        if (lcp.size() > m_input.size()) m_input = lcp;
        std::string line;
        for (size_t i = 0; i < matches.size(); ++i) { if (i) line += "  "; line += matches[i]; }
        console.print(line);
    }
    m_historyPos = -1;
}

// ---- Per-frame draw -------------------------------------------------------
void Hud::drawFps(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                  x3::con::IConsole& console, float dt) {
    // Smooth dt with an exponential moving average so the readout is stable.
    if (dt > 0.0f) {
        if (!m_emaSeeded) { m_emaDt = dt; m_emaSeeded = true; }
        else m_emaDt = m_emaDt * 0.9f + dt * 0.1f;
    }
    if (console.getInt("hud_fps") == 0) return;

    float ms  = m_emaDt * 1000.0f;
    float fps = (m_emaDt > 1e-6f) ? (1.0f / m_emaDt) : 0.0f;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "FPS %3.0f  %4.1f ms", fps, ms);

    const float white[4] = { 0.85f, 1.0f, 0.85f, 1.0f };
    const float shadow[4] = { 0.0f, 0.0f, 0.0f, 0.7f };
    // 1px drop shadow for legibility over bright scene pixels.
    device.drawHudText(frame, buf, 9.0f, 9.0f, kGlyphPx, shadow);
    device.drawHudText(frame, buf, 8.0f, 8.0f, kGlyphPx, white);
}

void Hud::drawCrosshair(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame) const {
    uint32_t w = 0, h = 0;
    device.hudSize(w, h);
    if (w == 0 || h == 0) return;
    const float cx = w * 0.5f, cy = h * 0.5f;
    const float arm = 8.0f;     // half-length of each crosshair arm (px)
    const float thick = 2.0f;   // arm thickness (px)
    const float gap = 3.0f;     // center gap so the very middle stays clear
    const float color[4] = { 0.2f, 1.0f, 0.35f, 0.95f };  // bright green

    // Four arms (left/right/top/bottom) leaving a small center gap.
    device.drawHudQuad(frame, cx - gap - arm, cy - thick * 0.5f, arm, thick, color); // left
    device.drawHudQuad(frame, cx + gap,       cy - thick * 0.5f, arm, thick, color); // right
    device.drawHudQuad(frame, cx - thick * 0.5f, cy - gap - arm, thick, arm, color); // top
    device.drawHudQuad(frame, cx - thick * 0.5f, cy + gap,       thick, arm, color); // bottom
}

void Hud::drawConsole(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                      x3::con::IConsole& console, float dt) {
    // Advance the slide animation toward the logical open state. Input handling
    // and gameplay-input suppression remain tied to m_consoleOpen (unchanged);
    // this only governs where/whether the panel is drawn.
    const float rate = (kConsoleSlideTime > 0.0f) ? (dt / kConsoleSlideTime) : 1.0f;
    if (m_consoleOpen) m_consoleAnim = std::min(1.0f, m_consoleAnim + rate);
    else               m_consoleAnim = std::max(0.0f, m_consoleAnim - rate);

    // Draw whenever the slide is in progress (anim > 0) so the close animation is
    // visible; once fully closed there is nothing to draw.
    if (m_consoleAnim <= 0.0f) return;

    uint32_t w = 0, h = 0;
    device.hudSize(w, h);
    if (w == 0 || h == 0) return;

    const float panelH = h * kConsoleHeightFrac;
    const float pad = 8.0f;
    const float lineH = kGlyphPx + 2.0f;

    // Slide: at anim=0 the panel sits fully above the top edge (top = -panelH);
    // at anim=1 it rests on-screen (top = 0). Lerp by the eased value.
    const float eased = smoothstep01(m_consoleAnim);
    const float top = -panelH + eased * panelH;   // lerp(-panelH, 0, eased)

    // Background panel + a bright bottom edge separator (offset by the slide).
    const float panel[4] = { 0.02f, 0.03f, 0.06f, 0.85f };
    const float edge[4]  = { 0.2f, 0.7f, 1.0f, 0.9f };
    device.drawHudQuad(frame, 0.0f, top, (float)w, panelH, panel);
    device.drawHudQuad(frame, 0.0f, top + panelH - 2.0f, (float)w, 2.0f, edge);

    // Input line at the bottom of the (slid) panel.
    const float inputY = top + panelH - pad - kGlyphPx;
    const float inText[4] = { 1.0f, 1.0f, 0.6f, 1.0f };
    std::string inLine = "] " + m_input + "_";   // blinking-ish caret marker
    device.drawHudText(frame, inLine.c_str(), pad, inputY, kGlyphPx, inText);

    // Scrollback above the input line, newest at the bottom (clipped to the
    // panel's slid top edge so text doesn't spill above the panel while sliding).
    const auto& lines = console.outputLines();
    const float outText[4] = { 0.8f, 0.85f, 0.8f, 1.0f };
    float y = inputY - lineH;
    for (int i = (int)lines.size() - 1; i >= 0 && y > top + pad - lineH; --i) {
        device.drawHudText(frame, lines[(size_t)i].c_str(), pad, y, kGlyphPx, outText);
        y -= lineH;
    }
}

} // namespace x3::game
