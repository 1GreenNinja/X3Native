// Screen-space HUD layer (S7). See app/hud.h.
//
// Clean-room: built only from the public IRenderDevice + IConsole interfaces.
// No id Tech / RBDOOM source consulted.
#include "hud.h"
#include "alert.h"   // alertLevelName (the LIVING-WORLD alert indicator)
#include "engine/rhi/Visibility.h"   // unified vis stats block (vis-unify)

#include <cstdio>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

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
    // r_stats: 1 shows the perf instrumentation panel (FPS/CPU/GPU ms, draws,
    // tris, objects). Off by default; toggled by `r_stats`/`stats` or F3.
    console.registerCVar("r_stats", "0", "show the renderer stats panel (0/1)");

    console.registerCommand("quit", [quitFlag](const std::vector<std::string>&) {
        if (quitFlag) *quitFlag = true;
    }, "exit the game");

    console.registerCommand("fps", [&console](const std::vector<std::string>& a) {
        // `fps` toggles; `fps 0|1` sets explicitly.
        if (!a.empty()) { console.set("hud_fps", a[0]); }
        else            { console.set("hud_fps", console.getInt("hud_fps") ? "0" : "1"); }
        console.print(std::string("hud_fps = ") + console.getString("hud_fps"));
    }, "toggle/set the FPS meter");

    // `stats` / `r_speeds`: toggle (or set 0|1) the renderer stats panel. Both
    // names drive the same r_stats cvar (r_speeds is the id-Tech-style alias).
    auto statsToggle = [&console](const std::vector<std::string>& a) {
        if (!a.empty()) { console.set("r_stats", a[0]); }
        else            { console.set("r_stats", console.getInt("r_stats") ? "0" : "1"); }
        console.print(std::string("r_stats = ") + console.getString("r_stats"));
    };
    console.registerCommand("stats",    statsToggle, "toggle/set the renderer stats panel");
    console.registerCommand("r_speeds", statsToggle, "toggle/set the renderer stats panel (alias)");

    console.print("X3 console ready. type 'help' for commands, '~' to close.");
}

// ---- Console open/close + input routing -----------------------------------
void Hud::toggleConsole() {
    m_consoleOpen = !m_consoleOpen;
    if (m_consoleOpen) { m_historyPos = -1; }
    m_consoleScroll = 0;   // always reopen/close pinned to the live bottom
}

void Hud::consoleScroll(int deltaLines) {
    if (!m_consoleOpen) return;
    m_consoleScroll += deltaLines;
    if (m_consoleScroll < 0) m_consoleScroll = 0;   // upper bound clamped in drawConsole
}

void Hud::onChar(unsigned int codepoint) {
    if (!m_consoleOpen) return;
    // Ignore the toggle keys and anything outside printable ASCII (the embedded
    // font only covers 0x20..0x7E).
    if (codepoint == '`' || codepoint == '~') return;
    if (codepoint >= 0x20 && codepoint < 0x7F) {
        m_input.push_back(static_cast<char>(codepoint));
        m_historyPos = -1;
        m_consoleScroll = 0;   // typing snaps back to the live bottom
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
    m_consoleScroll = 0;   // a submitted command snaps back to the live bottom
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

void Hud::drawStats(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                    x3::con::IConsole& console, float dt, bool force) {
    // Keep the CPU-ms EMA fresh even when the panel is hidden (drawFps shares it,
    // but stats may be the only consumer when hud_fps is off).
    if (dt > 0.0f) {
        if (!m_emaSeeded) { m_emaDt = dt; m_emaSeeded = true; }
        else m_emaDt = m_emaDt * 0.9f + dt * 0.1f;
    }
    if (!force && console.getInt("r_stats") == 0) return;

    const x3::rhi::RenderStats s = device.stats();
    const float cpuMs = m_emaDt * 1000.0f;
    const float fps   = (m_emaDt > 1e-6f) ? (1.0f / m_emaDt) : 0.0f;

    // Format the GPU line: "(n/a)" when timestamps are unsupported (gpuFrameMs==0
    // can also mean a degenerate first frame, but it reads fine as a tiny number).
    char gpuStr[24];
    if (s.gpuFrameMs > 0.0f) std::snprintf(gpuStr, sizeof(gpuStr), "%5.2f ms", s.gpuFrameMs);
    else                     std::snprintf(gpuStr, sizeof(gpuStr), "  n/a  ");

    char lines[11][64];
    int lineCount = 6;
    std::snprintf(lines[0], sizeof(lines[0]), "FPS %4.0f", fps);
    std::snprintf(lines[1], sizeof(lines[1]), "CPU %5.2f ms", cpuMs);
    std::snprintf(lines[2], sizeof(lines[2]), "GPU %s", gpuStr);
    std::snprintf(lines[3], sizeof(lines[3]), "draws %u", s.drawCalls);
    std::snprintf(lines[4], sizeof(lines[4]), "tris  %u", s.triangles);
    std::snprintf(lines[5], sizeof(lines[5]), "objs  %u/%u", s.objectsDrawn, s.objectsSubmitted);
    // ONE unified visibility block (vis-unify): the whole conserving chain
    // rooms -> frustum -> hzb -> drawn + per-stage times, on EVERY path (the
    // CPU path derives frustum-culled from submitted-drawn; the GPU path reads
    // the cull.comp counters back with frames-in-flight latency).
    {
        x3::rhi::VisCaps caps;
        caps.gpuCull = s.gpuCullSupported; caps.asyncCull = s.asyncCullSupported;
        caps.hzb = s.hzbSupported;
        const x3::rhi::VisPolicy pol = x3::rhi::resolveVisPolicy(
            console.getInt("r_vis"), caps,
            console.getInt("r_roomcull") != 0 ? -1 : 0);
        const x3::rhi::VisFrameStats v = x3::rhi::assembleVisStats(s, pol.mode);
        const char* pathName = (v.activePath == 2) ? "tier1 async"
                             : (v.activePath == 3) ? "tier2 mesh"
                             : (v.activePath == 1) ? "tier0 gfx" : "cpu";
        std::snprintf(lines[6],  sizeof(lines[6]),  "vis L%d %s%s", v.mode, pathName,
                      v.conserves ? "" : " !");
        std::snprintf(lines[7],  sizeof(lines[7]),  "cand %u rooms %u", v.candidates, v.roomsCulled);
        std::snprintf(lines[8],  sizeof(lines[8]),  "frustum %u hzb %u", v.frustumCulled, v.hzbCulled);
        std::snprintf(lines[9],  sizeof(lines[9]),  "drawn %u", v.drawn);
        std::snprintf(lines[10], sizeof(lines[10]), "pvs %.2f cull %.2f/%.2f", v.pvsMs,
                      v.cullCpuMs, v.cullGpuMs + v.hzbGpuMs);
        lineCount = 11;
    }

    // Right-aligned panel in the top-right corner so it never collides with the
    // top-left FPS meter / objective text.
    uint32_t w = 0, h = 0;
    device.hudSize(w, h);
    const float glyph = 14.0f;
    const float pad = 8.0f;
    // Widest line drives the panel width (monospace font: glyph*0.? approximated by
    // glyph width == glyph height for this 8x8 atlas scaled square).
    size_t widest = 0;
    for (int i = 0; i < lineCount; ++i) widest = std::max(widest, std::char_traits<char>::length(lines[i]));
    const float panelW = widest * glyph + pad * 2.0f;
    const float panelH = lineCount * (glyph * 1.5f) + pad * 2.0f;
    const float x0 = (w > 0) ? ((float)w - panelW - 8.0f) : 8.0f;
    const float y0 = 8.0f;

    // Backing plate for legibility over bright scene pixels.
    const float plate[4] = { 0.0f, 0.0f, 0.0f, 0.55f };
    device.drawHudQuad(frame, x0, y0, panelW, panelH, plate);

    const float white[4]  = { 0.85f, 1.0f, 0.85f, 1.0f };
    const float shadow[4] = { 0.0f, 0.0f, 0.0f, 0.8f };
    float ty = y0 + pad;
    for (int i = 0; i < lineCount; ++i) {
        const float tx = x0 + pad;
        device.drawHudText(frame, lines[i], tx + 1.0f, ty + 1.0f, glyph, shadow);
        device.drawHudText(frame, lines[i], tx, ty, glyph, white);
        ty += glyph + 2.0f;
    }
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

// ---- Phase 2a: player health + damage feedback ----------------------------
void Hud::drawHealth(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                     int hp, int maxHp) const {
    uint32_t w = 0, h = 0;
    device.hudSize(w, h);
    if (w == 0 || h == 0 || maxHp <= 0) return;

    if (hp < 0) hp = 0;
    if (hp > maxHp) hp = maxHp;
    const float frac = (float)hp / (float)maxHp;

    // Bar geometry: bottom-left, above the screen edge.
    const float barW = 260.0f, barH = 20.0f;
    const float x = 16.0f;
    const float y = (float)h - barH - 16.0f;
    const float border = 2.0f;

    // Backing plate (dark) + inner empty track.
    const float plate[4] = { 0.0f, 0.0f, 0.0f, 0.55f };
    const float track[4] = { 0.10f, 0.10f, 0.12f, 0.85f };
    device.drawHudQuad(frame, x - border, y - border,
                       barW + 2 * border, barH + 2 * border, plate);
    device.drawHudQuad(frame, x, y, barW, barH, track);

    // Fill: green when healthy, amber mid, red when low.
    float fill[4];
    if (frac > 0.5f) {        // green -> amber across [1.0, 0.5]
        float t = (1.0f - frac) / 0.5f;  // 0 at full, 1 at half
        fill[0] = 0.2f + 0.7f * t; fill[1] = 0.9f; fill[2] = 0.2f; fill[3] = 0.95f;
    } else {                  // amber -> red across [0.5, 0.0]
        float t = (0.5f - frac) / 0.5f;  // 0 at half, 1 at empty
        fill[0] = 0.9f; fill[1] = 0.9f * (1.0f - t); fill[2] = 0.15f; fill[3] = 0.95f;
    }
    device.drawHudQuad(frame, x, y, barW * frac, barH, fill);

    // HP number to the right of the bar.
    char buf[32];
    std::snprintf(buf, sizeof(buf), "HP %3d", hp);
    const float shadow[4] = { 0.0f, 0.0f, 0.0f, 0.7f };
    const float white[4]  = { 1.0f, 1.0f, 1.0f, 1.0f };
    const float tx = x + barW + 12.0f;
    const float ty = y + (barH - kGlyphPx) * 0.5f;
    device.drawHudText(frame, buf, tx + 1.0f, ty + 1.0f, kGlyphPx, shadow);
    device.drawHudText(frame, buf, tx, ty, kGlyphPx, white);
}

void Hud::drawDamageFlash(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                          float strength) const {
    if (strength <= 0.0f) return;
    if (strength > 1.0f) strength = 1.0f;
    uint32_t w = 0, h = 0;
    device.hudSize(w, h);
    if (w == 0 || h == 0) return;
    // Translucent red wash over the whole screen, peaking at ~0.45 alpha.
    const float red[4] = { 0.85f, 0.05f, 0.05f, 0.45f * strength };
    device.drawHudQuad(frame, 0.0f, 0.0f, (float)w, (float)h, red);
}

void Hud::drawDeathOverlay(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame) const {
    uint32_t w = 0, h = 0;
    device.hudSize(w, h);
    if (w == 0 || h == 0) return;
    // Dark vignette over the scene.
    const float dark[4] = { 0.0f, 0.0f, 0.0f, 0.6f };
    device.drawHudQuad(frame, 0.0f, 0.0f, (float)w, (float)h, dark);

    // Centered "YOU DIED" in big red text, with a drop shadow.
    const char* msg = "YOU DIED";
    const float bigGlyph = 48.0f;
    const float msgW = (float)std::char_traits<char>::length(msg) * bigGlyph;
    const float mx = ((float)w - msgW) * 0.5f;
    const float my = (float)h * 0.5f - bigGlyph;
    const float shadow[4] = { 0.0f, 0.0f, 0.0f, 0.85f };
    const float red[4]    = { 0.9f, 0.12f, 0.12f, 1.0f };
    device.drawHudText(frame, msg, mx + 3.0f, my + 3.0f, bigGlyph, shadow);
    device.drawHudText(frame, msg, mx, my, bigGlyph, red);

    // "Respawning..." subtitle.
    const char* sub = "Respawning...";
    const float subGlyph = 16.0f;
    const float subW = (float)std::char_traits<char>::length(sub) * subGlyph;
    const float sx = ((float)w - subW) * 0.5f;
    const float sy = my + bigGlyph + 16.0f;
    const float grey[4] = { 0.85f, 0.85f, 0.85f, 1.0f };
    device.drawHudText(frame, sub, sx + 1.0f, sy + 1.0f, subGlyph, shadow);
    device.drawHudText(frame, sub, sx, sy, subGlyph, grey);
}

void Hud::drawAlert(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                    int level, float redShift, float time) const {
    if (level <= 0) return;
    uint32_t w = 0, h = 0;
    device.hudSize(w, h);
    if (w == 0 || h == 0) return;
    if (level > 4) level = 4;

    // Level tint: amber at 1-2, hot red at 3-4 (pulsing in lockdown).
    float r = 0.95f, g = 0.72f, b = 0.18f;
    if (level >= 3) { r = 0.95f; g = 0.16f; b = 0.10f; }
    const float pulse = (redShift > 0.0f)
                          ? 0.75f + 0.25f * std::sin(time * 6.0f)
                          : 1.0f;

    // Four pips, top-right, filled up to the level.
    const float pipW = 16.0f, pipH = 8.0f, gap = 4.0f;
    const float rowW = 4.0f * pipW + 3.0f * gap;
    const float x0 = (float)w - rowW - 16.0f;
    const float y0 = 14.0f;
    const float plate[4] = { 0.0f, 0.0f, 0.0f, 0.45f };
    device.drawHudQuad(frame, x0 - 6.0f, y0 - 4.0f, rowW + 12.0f, pipH + 22.0f, plate);
    for (int k = 0; k < 4; ++k) {
        const bool on = (k < level);
        const float c[4] = { on ? r * pulse : 0.25f,
                             on ? g * pulse : 0.25f,
                             on ? b * pulse : 0.28f, on ? 0.95f : 0.6f };
        device.drawHudQuad(frame, x0 + (float)k * (pipW + gap), y0, pipW, pipH, c);
    }
    // The level name under the pips, right-aligned.
    const char* name = alertLevelName(level);
    const float px = 10.0f;
    const float nameW = device.textAdvance(x3::rhi::FontRole::HudMono, name, px);
    const float tc[4] = { r * pulse, g * pulse, b * pulse, 0.95f };
    device.drawHudText(frame, name, x0 + rowW - nameW, y0 + pipH + 4.0f, px, tc);

    // Lockdown: a faint pulsing red frame at the screen edges.
    if (redShift > 0.0f) {
        const float a = (0.10f + 0.10f * std::sin(time * 6.0f)) * redShift;
        const float edge[4] = { 0.9f, 0.05f, 0.05f, a < 0.0f ? 0.0f : a };
        const float t = 6.0f;
        device.drawHudQuad(frame, 0, 0, (float)w, t, edge);
        device.drawHudQuad(frame, 0, (float)h - t, (float)w, t, edge);
        device.drawHudQuad(frame, 0, 0, t, (float)h, edge);
        device.drawHudQuad(frame, (float)w - t, 0, t, (float)h, edge);
    }
}

void Hud::drawVigilBark(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                        const char* text, float alpha) const {
    if (!text || !*text || alpha <= 0.0f) return;
    if (alpha > 1.0f) alpha = 1.0f;
    uint32_t w = 0, h = 0;
    device.hudSize(w, h);
    if (w == 0 || h == 0) return;

    // VIGIL's terminal orange (matches the on-glass ink the terminal turns while he
    // speaks). A dark plate keeps him readable over any scene.
    const float px = std::max(12.0f, (float)h * 0.020f);   // glyph size scales with res
    const float pad = px * 0.9f;
    const float lineH = px * 1.4f;
    const float maxTextW = (float)w * 0.70f;               // wrap to 70% of the screen

    // Word-wrap "VIGIL: <text>" to maxTextW using the proportional Menu atlas.
    const std::string full = std::string("VIGIL: ") + text;
    std::vector<std::string> lines;
    {
        std::string cur;
        size_t i = 0;
        while (i < full.size()) {
            size_t j = full.find(' ', i);
            if (j == std::string::npos) j = full.size();
            const std::string word = full.substr(i, j - i);
            const std::string trial = cur.empty() ? word : cur + " " + word;
            if (!cur.empty() &&
                device.textAdvance(x3::rhi::FontRole::Menu, trial.c_str(), px) > maxTextW) {
                lines.push_back(cur); cur = word;
            } else cur = trial;
            i = j + 1;
        }
        if (!cur.empty()) lines.push_back(cur);
    }
    if (lines.empty()) return;

    // Plate: centered horizontally, sitting in the lower third (above the health HUD).
    float blockW = 0.0f;
    for (const auto& l : lines)
        blockW = std::max(blockW, device.textAdvance(x3::rhi::FontRole::Menu, l.c_str(), px));
    const float blockH = lineH * (float)lines.size();
    const float x0 = ((float)w - blockW) * 0.5f;
    const float y0 = (float)h * 0.70f;
    const float plate[4] = { 0.02f, 0.02f, 0.03f, 0.62f * alpha };
    device.drawHudQuad(frame, x0 - pad, y0 - pad * 0.6f,
                       blockW + pad * 2.0f, blockH + pad * 1.2f, plate);
    // A thin orange accent bar on the left edge — his "signal".
    const float bar[4] = { 1.0f, 0.60f, 0.16f, 0.9f * alpha };
    device.drawHudQuad(frame, x0 - pad, y0 - pad * 0.6f, 3.0f, blockH + pad * 1.2f, bar);

    // The text: label row hotter, body rows slightly dimmer; drop shadow for punch.
    const float col[4] = { 1.0f, 0.68f, 0.26f, 0.98f * alpha };
    const float sh[4]  = { 0.0f, 0.0f, 0.0f, 0.7f * alpha };
    float ty = y0;
    for (const auto& l : lines) {
        device.drawHudTextF(frame, x3::rhi::FontRole::Menu, l.c_str(), x0 + 1.5f, ty + 1.5f, px, sh);
        device.drawHudTextF(frame, x3::rhi::FontRole::Menu, l.c_str(), x0, ty, px, col);
        ty += lineH;
    }
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
    const float lineH = kGlyphPx * 1.5f;   // TTF glyphs need ~1.5x leading (was +2 -> lines overlapped)

    // Slide: at anim=0 the panel sits fully above the top edge (top = -panelH);
    // at anim=1 it rests on-screen (top = 0). Lerp by the eased value.
    const float eased = smoothstep01(m_consoleAnim);
    const float top = -panelH + eased * panelH;   // lerp(-panelH, 0, eased)

    // Background panel + a bright bottom edge separator (offset by the slide).
    const float panel[4] = { 0.02f, 0.03f, 0.06f, 0.85f };
    const float edge[4]  = { 0.2f, 0.7f, 1.0f, 0.9f };
    device.drawHudQuad(frame, 0.0f, top, (float)w, panelH, panel);
    device.drawHudQuad(frame, 0.0f, top + panelH - 2.0f, (float)w, 2.0f, edge);

    // Input line near the bottom of the (slid) panel — offset by the full line height
    // (not just the glyph size) so the text clears the bright bottom-edge separator.
    const float inputY = top + panelH - pad - lineH;
    const float inText[4] = { 1.0f, 1.0f, 0.6f, 1.0f };
    std::string inLine = "] " + m_input + "_";   // blinking-ish caret marker
    device.drawHudText(frame, inLine.c_str(), pad, inputY, kGlyphPx, inText);

    // Scrollback above the input line, newest at the bottom (clipped to the
    // panel's slid top edge so text doesn't spill above the panel while sliding).
    // m_consoleScroll shifts the visible window UP through history: the bottom row
    // is line (size-1 - scroll). Clamp the offset to the scrollable range here,
    // where the panel height (hence how many rows fit) is known.
    const auto& lines = console.outputLines();
    const float outText[4] = { 0.8f, 0.85f, 0.8f, 1.0f };
    const int total    = (int)lines.size();
    const int visRows  = std::max(1, (int)((inputY - (top + pad)) / lineH));
    const int maxScroll = std::max(0, total - visRows);
    if (m_consoleScroll > maxScroll) m_consoleScroll = maxScroll;
    if (m_consoleScroll < 0)         m_consoleScroll = 0;

    // A subtle "▲ more" marker at the top-right while scrolled off the bottom, so
    // the player knows there's live text below the current view.
    if (m_consoleScroll > 0) {
        const float mk[4] = { 0.5f, 0.75f, 1.0f, 0.9f };
        device.drawHudText(frame, "^ scrollback", (float)w - 130.0f, top + pad, kGlyphPx, mk);
    }

    float y = inputY - lineH;
    for (int i = (total - 1) - m_consoleScroll; i >= 0 && y > top + pad - lineH; --i) {
        device.drawHudText(frame, lines[(size_t)i].c_str(), pad, y, kGlyphPx, outText);
        y -= lineH;
    }
}

} // namespace x3::game
