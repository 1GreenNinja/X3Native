// THE ONE HUD PANEL PRIMITIVE. See app/hud_panel.h.
//
// Clean-room: built only from the public IRenderDevice interface.
#include "hud_panel.h"

#include <algorithm>
#include <cmath>

namespace x3::game {

void hudPanel(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
              float x, float y, float w, float h,
              float radius, const float* fill, const float* accent,
              float alpha, bool topEdge, bool roundTop, bool roundBottom) {
    if (w <= 0.0f || h <= 0.0f || alpha <= 0.0f) return;
    if (alpha > 1.0f) alpha = 1.0f;

    const float* f = fill ? fill : kHudPanelFill;
    const float fc[4] = { f[0], f[1], f[2], f[3] * alpha };

    float r = std::max(0.0f, radius);
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

    // Optional accent bar down the left inner edge (between the corner bands).
    if (accent) {
        const float ac[4] = { accent[0], accent[1], accent[2], accent[3] * alpha };
        const float ay = y + std::max(rTop, 2.0f);
        const float ah = h - std::max(rTop, 2.0f) - std::max(rBot, 2.0f);
        if (ah > 0.0f) device.drawHudQuad(frame, x, ay, 3.0f, ah, ac);
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
