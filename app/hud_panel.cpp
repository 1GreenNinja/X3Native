// THE ONE HUD PANEL PRIMITIVE. See app/hud_panel.h.
//
// Clean-room: built only from the public IRenderDevice interface.
#include "hud_panel.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace x3::game {

// ---- Live glass tuning (see hud_panel.h). Defaults ARE the constants. -------
HudPanelTuning& hudPanelTuning() {
    static HudPanelTuning s;
    return s;
}

void registerHudPanelCVars(x3::con::IConsole& console) {
    console.registerCVar("hud_glass_r", "0.008", "HUD GLASS: panel fill RED (linear 0..1). Live. The near-black glass every HUD plate, menu and the console share.");
    console.registerCVar("hud_glass_g", "0.012", "HUD GLASS: panel fill GREEN (linear 0..1). Live.");
    console.registerCVar("hud_glass_b", "0.018", "HUD GLASS: panel fill BLUE (linear 0..1). Live.");
    console.registerCVar("hud_glass_a", "0.86",  "HUD GLASS: panel fill ALPHA. MEASURED, not taste: below ~0.86 the plate composites to mid grey over bright scenes and light text washes out. Live.");
    console.registerCVar("hud_radius",  "3",     "HUD GLASS: corner radius in px for EVERY panel (HUD blocks, menus, console, tuning panels). Tiny = machined chamfer; large = soft app card. Live.");
}

void applyHudPanelCVars(const x3::con::IConsole& console) {
    HudPanelTuning& t = hudPanelTuning();
    // An unregistered cvar reads empty -> leave the shipped value alone (the same
    // rule applyWeaponFxCVars uses, so a bare host without the catalog is
    // byte-identical rather than reset to zero — a 0-alpha HUD would be invisible).
    auto sync = [&console](const char* name, float& dst, float lo, float hi) {
        const std::string s = console.getString(name);
        if (s.empty()) return;
        float v = 0.0f;
        try { v = std::stof(s); } catch (...) { return; }
        dst = (v < lo) ? lo : (v > hi ? hi : v);
    };
    sync("hud_glass_r", t.fill[0], 0.0f, 1.0f);
    sync("hud_glass_g", t.fill[1], 0.0f, 1.0f);
    sync("hud_glass_b", t.fill[2], 0.0f, 1.0f);
    sync("hud_glass_a", t.fill[3], 0.0f, 1.0f);
    // 24 px is a hard cap: the corner cost is 2*R one-pixel strips per panel, and
    // beyond a quarter of a HUD chip's height the "rounding" is just a lozenge.
    sync("hud_radius",  t.radius,  0.0f, 24.0f);
}

void hudPanel(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
              float x, float y, float w, float h,
              float radius, const float* fill, const float* accent,
              float alpha, bool topEdge, bool roundTop, bool roundBottom) {
    if (w <= 0.0f || h <= 0.0f || alpha <= 0.0f) return;
    if (alpha > 1.0f) alpha = 1.0f;

    const HudPanelTuning& tun = hudPanelTuning();
    const float* f = fill ? fill : tun.fill;
    const float fc[4] = { f[0], f[1], f[2], f[3] * alpha };

    // THE ONE RADIUS. Callers pass kHudPanelRadius (or a local number) but the
    // live value wins, so dragging hud_radius moves the ENTIRE panel family at
    // once instead of only the screens that happened to use the default. A
    // caller asking for 0 still gets 0 — a deliberately square edge (the console
    // slab's top) is a shape decision, not a style one.
    float radiusIn = (radius <= 0.0f) ? 0.0f : tun.radius;
    float r = std::max(0.0f, radiusIn);
    r = std::min(r, std::floor(std::min(w, h) * 0.5f));
    const float rTop = roundTop    ? r : 0.0f;
    const float rBot = roundBottom ? r : 0.0f;

    // Corner rows: one strip per pixel row, inset by the circle equation so the
    // silhouette is a real quarter-round, not a chamfer.
    auto cornerRows = [&](float rr, bool top) {
        for (float iy = 0.0f; iy < rr; iy += 1.0f) {
            const float d = rr - iy - 0.5f;                    // row center dist from the arc line
            const float inset = rr - std::sqrt(std::max(0.0f, rr * rr - d * d));
            const float sy = top ? (y + iy) : (y + h - 1.0f - iy);
            device.drawHudQuad(frame, x + inset, sy, w - 2.0f * inset, 1.0f, fc);
        }
    };
    cornerRows(rTop, true);
    cornerRows(rBot, false);

    // Body between the rounded bands.
    const float bodyY = y + rTop;
    const float bodyH = h - rTop - rBot;
    if (bodyH > 0.0f) device.drawHudQuad(frame, x, bodyY, w, bodyH, fc);

    // Subtle 1px lighter line along the flat top span.
    if (topEdge) {
        const float ec[4] = { kHudPanelEdge[0], kHudPanelEdge[1], kHudPanelEdge[2],
                              kHudPanelEdge[3] * alpha };
        device.drawHudQuad(frame, x + rTop, y, w - 2.0f * rTop, 1.0f, ec);
    }

    // Optional accent bar down the left inner edge. It starts BELOW the top arc
    // and ends ABOVE the bottom arc, and is nudged 1px inboard — drawn flush at
    // `x` it juts into open scene at the corners, where the rounded fill has
    // already pulled away, and reads as a detached tick floating off the panel.
    if (accent) {
        const float ac[4] = { accent[0], accent[1], accent[2], accent[3] * alpha };
        const float ay = y + rTop;
        const float ah = h - rTop - rBot;
        if (ah > 0.0f) device.drawHudQuad(frame, x + 1.0f, ay, 3.0f, ah, ac);
    }
}

float hudPromptChip(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                    const char* text, float cx, float yTop, float px,
                    const float* textCol, float alpha, const float* accent,
                    x3::rhi::FontRole role) {
    if (!text || !*text || px <= 0.0f || alpha <= 0.0f) return 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;

    const float adv  = device.textAdvance(role, text, px);
    const float padX = std::max(8.0f, px * 0.7f) + (accent ? 4.0f : 0.0f);
    const float padY = std::max(5.0f, px * 0.42f);
    const float w    = adv + padX * 2.0f;
    const float h    = px + padY * 2.0f;
    const float x    = cx - w * 0.5f;

    hudPanel(device, frame, x, yTop, w, h, kHudPanelRadius, nullptr, accent, alpha);

    const float* tc = textCol ? textCol : kHudTextLight;
    const float col[4] = { tc[0], tc[1], tc[2], tc[3] * alpha };
    const float sh[4]  = { 0.0f, 0.0f, 0.0f, 0.65f * alpha };
    const float tx = x + padX + (accent ? 2.0f : 0.0f);
    const float ty = yTop + padY;
    device.drawHudTextF(frame, role, text, tx + 1.5f, ty + 1.5f, px, sh);
    device.drawHudTextF(frame, role, text, tx, ty, px, col);
    return h;
}

} // namespace x3::game
