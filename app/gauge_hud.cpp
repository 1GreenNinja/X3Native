// THE CAR GAUGE CLUSTER — see gauge_hud.h for why this is a module and not
// inline host code. Everything here was lifted VERBATIM out of host_tunnel's
// interactive HUD block (including its tuning comments, which are receipts for
// numbers that were argued over); the only change is that the values come from
// GaugeClusterState instead of straight off the car.

#include "gauge_hud.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace x3::game {

namespace {

// ONE needle-atlas lookup. The tach and the boost dial share the atlas — same
// bezel, same sweep start and span, so frame i points at the same angle on both
// faces; only the meaning of the angle differs, and that lives in the scale.
// Two copies of this arithmetic is how one dial ends up a frame off the other.
void needleCell(float frac01, float& u0, float& v0, float& du) {
    constexpr int NF = 64, AT = 8;      // 64 rotations in an 8x8 grid
    int fi = (int)(frac01 * (NF - 1) + 0.5f);
    fi = fi < 0 ? 0 : (fi > NF - 1 ? NF - 1 : fi);
    u0 = (float)(fi % AT) / (float)AT;
    v0 = (float)(fi / AT) / (float)AT;
    du = 1.0f / (float)AT;
}

// The limiter, in rpm. Shift lights ramp from kShiftFrom and flash at/above it.
constexpr float kLimiterRpm = 7312.0f;
constexpr float kShiftFrom  = 6000.0f;

} // namespace

void drawGaugeCluster(x3::rhi::IRenderDevice& device,
                      const x3::rhi::FrameContext& frame,
                      float fw, float fh,
                      const GaugeClusterTex& tex,
                      const GaugeClusterState& st,
                      const FuelTank& fuel, bool refuelling) {
    float R = 0.0f, gcx = 0.0f, gcy = 0.0f;
    gaugeClusterAnchor(fw, fh, R, gcx, gcy);
    const float gateH = R * 0.90f;
    const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    const float dt = std::max(st.dt, 1e-4f);

    // ---- TACH: face, needle, shift gate --------------------------------
    if (tex.dial.valid()) {
        const float frac = std::min(1.0f, std::max(0.0f, st.rpm / kGaugeRpmMax));
        // Framerate-independent needle smoothing — raw rpm buzzes at 165 Hz.
        static float shownFrac = 0.0f;
        shownFrac += (frac - shownFrac) * (1.0f - std::exp(-9.0f * dt));

        device.drawHudImage(frame, tex.dial, gcx - R, gcy - R, 2.0f * R, 2.0f * R, white);

        if (tex.needle.valid()) {
            float u0, v0, du; needleCell(shownFrac, u0, v0, du);
            device.drawHudImage(frame, tex.needle, gcx - R, gcy - R, 2.0f * R, 2.0f * R,
                                white, u0, v0, u0 + du, v0 + du);
        }
        if (tex.gate.valid()) {
            const float gw = gateH * 2.0f, gh = gateH;
            device.drawHudImage(frame, tex.gate, gcx - gw * 0.5f,
                                gcy + R + R * 0.12f, gw, gh, white);
        }
    }

    // ---- BOOST DIAL ------------------------------------------------------
    // The ROUND dial, left of the tach at 0.70 of its radius — the secondary
    // instrument, not a second primary. Sunday's build replaced this with a
    // gray segmented bar; the dial art reads as an instrument where the bar
    // read as UI.
    //
    // It reads NEGATIVE off-throttle. A boost gauge pinned at zero whenever you
    // lift is the tell that no manifold model is behind it, and vacuum is where
    // a real one lives most of the time.
    const float R2  = R * 0.70f;
    const float bcx = gcx - R - R2 - R * 0.10f;
    const float bcy = gcy + R - R2;              // bottoms line up
    if (tex.boost.valid()) {
        const float bf = std::min(1.0f, std::max(0.0f,
            (st.boostPsi - kGaugeMinPsi) / (kGaugeMaxPsi - kGaugeMinPsi)));
        static float shownBoost = 0.0f;
        shownBoost += (bf - shownBoost) * (1.0f - std::exp(-12.0f * dt));

        device.drawHudImage(frame, tex.boost, bcx - R2, bcy - R2,
                            2.0f * R2, 2.0f * R2, white);
        if (tex.needle.valid()) {
            float u0, v0, du; needleCell(shownBoost, u0, v0, du);
            device.drawHudImage(frame, tex.needle, bcx - R2, bcy - R2,
                                2.0f * R2, 2.0f * R2, white, u0, v0, u0 + du, v0 + du);
        }
        char bbuf[32];
        std::snprintf(bbuf, sizeof(bbuf), "%+.1f", (double)st.boostPsi);
        const float bp = R2 * 0.26f;
        const float bw = (float)std::strlen(bbuf) * bp;
        const bool  over = st.boostPsi >= kGaugeHotPsi;   // the art's red band
        const float bc[4] = { over ? 1.0f : 0.97f, over ? 0.32f : 0.98f,
                              over ? 0.24f : 1.0f, 1.0f };
        device.drawHudText(frame, bbuf, bcx - bw * 0.5f, bcy + R2 * 0.26f, bp, bc);
    }

    // ---- NOS TANK — SOLID LUMINESCENT CURVED BAR (Tim: "Curving bar like NFS
    // had 20 years ago... not beads. solid luminescent bars"). A 32-state baked
    // arc atlas (hot core + glow, husk for the spent span); the frame is picked
    // by tank level — the needle-atlas pattern applied to a fill.
    if (tex.nos.valid()) {
        // THE 8x4 ATLAS CONTRACT (paired with compose_nos_atlas in
        // tools/compose_gauge_dial.py: NOS_FRAMES 32, NOS_ATLAS_N 8, so 4 rows).
        // v spans 0.25 per row, not 1/8 — this is the one place the NOS atlas
        // differs from the needle's square grid, and getting it wrong samples
        // the wrong quarter of the sheet.
        constexpr int NF2 = 32, AC = 8;
        int fi = (int)(st.nosFrac * (NF2 - 1) + 0.5f);
        fi = fi < 0 ? 0 : (fi > NF2 - 1 ? NF2 - 1 : fi);
        const float u0 = (float)(fi % AC) / (float)AC;
        const float v0 = (float)(fi / AC) / 4.0f;
        // Cell arc radius is 0.86 * half-cell; on screen the arc sits at
        // 1.22 * R2, so the drawn cell spans 2 * 1.22 / 0.86 * R2.
        const float side = 2.837f * R2;
        float tint[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        if (st.nosActive) { tint[0] = 1.25f; tint[1] = 1.15f; }   // spray flare
        device.drawHudImage(frame, tex.nos, bcx - side * 0.5f, bcy - side * 0.5f,
                            side, side, tint, u0, v0, u0 + 1.0f / AC, v0 + 0.25f);
        const float lp2 = R * 0.085f;
        const float lc2[4] = { 0.55f, 0.85f, 1.0f, 1.0f };
        device.drawHudText(frame, "NOS", bcx - R2 * 1.22f - lp2 * 1.2f,
                           bcy + R2 * 0.95f, lp2, lc2);
    }

    // ---- THE FUEL GAUGE (W-STATIONS). Drawn by the SAME function the headless
    // pump proof calls (app/gas_station.h), anchored on this cluster's tach so
    // it always rides under the dials.
    drawFuelBar(device, frame, fuel, refuelling, R, gcx, gcy);

    // ---- MPH + GEAR --------------------------------------------------------
    {
        char gbuf[64];
        std::snprintf(gbuf, sizeof(gbuf), "%d", (int)(st.mph + 0.5f));
        // 0.275R, not 0.34R: at three digits the wider face ran into the "0"
        // and "8" numerals, which sit at x = +-0.455R.
        const float px = R * 0.275f;
        const float w  = (float)std::strlen(gbuf) * px;
        const float col[4] = { 0.97f, 0.98f, 1.0f, 1.0f };
        device.drawHudText(frame, gbuf, gcx - w * 0.5f, gcy + R * 0.235f, px, col);
        const float lp = R * 0.095f;
        const float lc[4] = { 0.35f, 0.78f, 0.95f, 1.0f };   // cyan, per the reference
        device.drawHudText(frame, "MPH", gcx - 1.5f * lp, gcy + R * 0.55f, lp, lc);
    }
    {
        const char* gs = (st.gear < 0) ? "R"
                       : (st.gear == 0 ? "N" : "123456" + ((st.gear - 1) % 6));
        char one[2] = { gs[0], 0 };
        const bool hot = st.rpm > kLimiterRpm * 0.985f;
        const float px = R * 0.22f;
        const float col[4] = { hot ? 1.0f : 0.35f, hot ? 0.30f : 0.82f,
                               hot ? 0.22f : 0.98f, 1.0f };
        device.drawHudText(frame, one, gcx - px * 0.5f, gcy - R * 0.46f, px, col);
    }
    {   // shift lights along the top of the bezel
        const int   NL = 8;
        const float lw = R * 0.115f, lh = R * 0.052f, gp = lw * 0.30f;
        const float tot = NL * lw + (NL - 1) * gp;
        const float x0 = gcx - tot * 0.5f, y0 = gcy - R * 1.17f;
        const float lit = std::min(1.0f, std::max(0.0f,
                              (st.rpm - kShiftFrom) / (kLimiterRpm - kShiftFrom)));
        const bool  fl  = st.rpm >= kLimiterRpm &&
                          std::fmod((float)st.now * 4.5f, 1.0f) < 0.5f;
        for (int i = 0; i < NL; ++i) {
            const bool on = lit >= (float)(i + 1) / (float)NL || fl;
            const float tt = (float)i / (float)(NL - 1);
            float c4[4];
            if (fl)       { c4[0]=1.0f; c4[1]=0.16f; c4[2]=0.12f; c4[3]=1.0f; }
            else if (!on) { c4[0]=0.12f; c4[1]=0.14f; c4[2]=0.18f; c4[3]=0.8f; }
            else          { c4[0]=0.25f+0.75f*tt; c4[1]=0.85f-0.58f*tt;
                            c4[2]=0.98f-0.84f*tt; c4[3]=1.0f; }
            device.drawHudQuad(frame, x0 + i * (lw + gp), y0, lw, lh, c4);
        }
    }

    // ---- KEY HINTS on the glass. A binding nobody can see does not exist: T
    // toggled traction control for a whole session while the only mention of it
    // went to a log file.
    {
        // THE LINES WERE ON TOP OF EACH OTHER. Eyes-on, first capture of this
        // cluster (X3_SHOT_GAUGES): the five hints were pitched 0.12R apart at
        // 0.085R text, which at the shipping R = 0.150 * screen height is a
        // 13 px pitch for ~14 px glyphs — "~ CONSOLE" and "T TRACTION" were
        // legibly interleaved, and the bottom of the stack ran into the shift
        // lights at gcy - 1.17R. Pitch is 0.18R now and the stack starts above
        // the TC line, so nothing in this corner touches anything else.
        // (The pitch and the shift-light / TC row positions are a set: moving
        // any of them means re-checking the other two against a capture.)
        const float hp = R * 0.085f;
        const float hcol[4] = { 0.52f, 0.57f, 0.66f, 1.0f };
        static const char* kHints[5] = {
            "SPACE  HANDBRAKE", "T  TRACTION", "~  CONSOLE", "C  CLIMB", "SHIFT  NITROUS"
        };
        for (int i = 0; i < 5; ++i)
            device.drawHudText(frame, kHints[i], gcx - R * 0.95f,
                               gcy - R * (1.46f + 0.18f * (float)i), hp, hcol);
    }
    {
        // TC goes ON TOP of the hint stack, not between it and the shift
        // lights. At gcy - 1.30R it was sandwiched between the bottom hint
        // (1.46R) and the shift-light row (1.17R) with ~16 px of glyph in a
        // 15 px gap, and the capture showed "TC OFF" printed across the lit
        // segments. Above the stack it has the whole corner to itself, and it
        // is also where the eye goes first — which is right for a state that
        // changes how the car behaves.
        const char* t = st.tcOn ? "TC ON" : "TC OFF";
        const float px = R * 0.10f;
        const float c4[4] = { st.tcOn ? 0.45f : 1.0f, st.tcOn ? 0.85f : 0.55f,
                              st.tcOn ? 1.0f : 0.25f, 1.0f };
        device.drawHudText(frame, t, gcx - R * 0.95f, gcy - R * 2.42f, px, c4);
    }
}

} // namespace x3::game
