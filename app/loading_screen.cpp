// EFLZ loading screen — see app/loading_screen.h.
//
// Clean-room: built only from the public IRenderDevice 2D path (drawHudQuad +
// drawHudTextF, the same path app/ui.cpp uses) + one procedural texture. No new
// pipeline, no id Tech / RBDOOM source consulted.
#include "loading_screen.h"
#include "headless_device.h"   // shared no-op IRenderDevice (for --test-loading)

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace x3::game {

namespace {

// ---- Tuning -----------------------------------------------------------------
constexpr float kFadeIn      = 0.40f;   // seconds: 0 -> full
constexpr float kFadeOut     = 0.45f;   // seconds: full -> 0
constexpr float kTipPeriod   = 2.0f;    // seconds per tip line
constexpr uint32_t kBgW      = 256;     // procedural background size (stretched full-bleed)
constexpr uint32_t kBgH      = 256;

// Cumulative bar fraction reached at each LoadStep (real work milestones).
constexpr float kStepFrac[(int)LoadStep::Count] = {
    0.00f,  // Start
    0.10f,  // DeviceReady
    0.20f,  // AssetsMounted
    0.30f,  // AudioReady
    0.40f,  // PhysicsReady
    0.65f,  // WorldGeometry
    0.85f,  // Spawns
    0.95f,  // FxReady
    1.00f,  // Done
};

// Hand-authored rotating tips / lore (EFLZ tone). The bar cycles through these
// every kTipPeriod seconds so the wait teaches the controls + the stakes.
const char* const kTips[] = {
    "F1 / F2 toggle first- and third-person view.",
    "Press E at a terminal - code 1278 unlocks your cell.",
    "Save the captives before the impregnation timer runs out.",
    "Cycle weapons with the number keys or the mouse wheel.",
    "L toggles your flashlight - the labs run dark.",
    "Some cell floors hide a trapdoor. Listen for the hollow tile.",
    "Interrupt a breeder mid-impregnation to save the girl.",
    "Esc pauses. The clock does not.",
    "Reload often - empty mags get you killed in the wards.",
    "Martinez guards the arena. Bring the elevator down behind him.",
};
constexpr int kTipCount = (int)(sizeof(kTips) / sizeof(kTips[0]));

// Palette (linear RGBA).
constexpr float kTitleCol[4]  = { 0.36f, 0.86f, 1.00f, 1.0f };  // cyan title
constexpr float kSubCol[4]    = { 0.62f, 0.70f, 0.74f, 1.0f };
constexpr float kTipCol[4]    = { 0.78f, 0.84f, 0.86f, 1.0f };
constexpr float kBarTrack[4]  = { 0.06f, 0.08f, 0.11f, 1.0f };
constexpr float kBarFill[4]   = { 0.25f, 0.78f, 0.98f, 1.0f };  // cyan energy
constexpr float kBarEdge[4]   = { 0.40f, 0.62f, 0.95f, 1.0f };
constexpr float kShadow[4]    = { 0.0f, 0.0f, 0.0f, 0.8f };

inline uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v)); }

// Cheap deterministic value-noise hash in [0,1).
inline float hash01(uint32_t x, uint32_t y, uint32_t salt) {
    uint32_t h = x * 374761393u + y * 668265263u + salt * 2147483647u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return (float)(h & 0xFFFFFFu) / (float)0x1000000u;
}

// Build the full-bleed background: a dark blue-black vertical gradient (lighter
// toward the bottom where the bar sits), a faint horizontal scanline pattern, a
// soft radial vignette, and low-frequency grime — reads as a sci-fi prison/lab
// ambient backdrop. RGBA8, sRGB.
std::vector<uint8_t> makeBackgroundRGBA(uint32_t w, uint32_t h) {
    std::vector<uint8_t> px((size_t)w * h * 4);
    const float cx = (w - 1) * 0.5f, cy = (h - 1) * 0.45f;
    const float maxR = std::sqrt(cx * cx + cy * cy);
    for (uint32_t y = 0; y < h; ++y) {
        const float vy = (float)y / (float)(h - 1);     // 0 top -> 1 bottom
        for (uint32_t x = 0; x < w; ++x) {
            // Base vertical gradient: deep blue-black at top, faint steel toward the
            // bottom so the title + bar read against it.
            int r = (int)(6.0f  + 10.0f * vy);
            int g = (int)(9.0f  + 16.0f * vy);
            int b = (int)(18.0f + 30.0f * vy);
            // Vignette: darken toward the corners.
            const float dx = (float)x - cx, dy = (float)y - cy;
            const float rad = std::sqrt(dx * dx + dy * dy) / maxR;   // 0 center -> 1 corner
            const float vig = 1.0f - 0.55f * rad * rad;
            r = (int)(r * vig); g = (int)(g * vig); b = (int)(b * vig);
            // Faint scanlines (every other row a touch brighter) — CRT/lab monitor feel.
            if ((y & 1u) == 0) { r += 3; g += 4; b += 6; }
            // Low-frequency grime mottle.
            const int grime = (int)((hash01(x / 12, y / 12, 5u) - 0.5f) * 7.0f);
            r += grime; g += grime; b += grime;
            uint8_t* p = &px[((size_t)y * w + x) * 4];
            p[0] = clamp8(r); p[1] = clamp8(g); p[2] = clamp8(b); p[3] = 255;
        }
    }
    return px;
}

} // namespace

LoadingScreen::LoadingScreen() = default;

int LoadingScreen::tipCount() const { return kTipCount; }
const char* LoadingScreen::tip(int i) const {
    if (i < 0 || i >= kTipCount) return "";
    return kTips[i];
}

void LoadingScreen::init(x3::rhi::IRenderDevice& device) {
    if (m_inited) return;
    auto bg = makeBackgroundRGBA(kBgW, kBgH);
    m_bg = device.createTexture(bg.data(), kBgW, kBgH, /*srgb=*/true);
    m_inited = true;
}

void LoadingScreen::shutdown(x3::rhi::IRenderDevice& device) {
    if (m_bg.valid()) device.destroyTexture(m_bg);
    m_bg = {};
    m_inited = false;
}

void LoadingScreen::setProgress(float frac, const char* label) {
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    if (frac > m_progress) m_progress = frac;   // monotonic high-water mark
    if (label && label[0]) m_label = label;
}

void LoadingScreen::step(LoadStep s, const char* label) {
    const int i = (int)s;
    if (i < 0 || i >= (int)LoadStep::Count) return;
    setProgress(kStepFrac[i], label);
}

void LoadingScreen::beginFadeOut(bool fast) {
    m_fadeOut = true;
    // BLACKOUT (post-cold-open wake): the fade-out IS the slow wake into the cell —
    // ~2.5 s up from black, overriding the fast-boot quick hand-off.
    if (m_blackout)  m_fadeOutScale = 0.18f;
    else             m_fadeOutScale = fast ? 3.0f : 1.0f;   // BOOT-TIME: quick hand-off on fast boots
}

void LoadingScreen::render(x3::rhi::IRenderDevice& device,
                           const x3::rhi::FrameContext& frame, float dt) {
    if (dt < 0.0f) dt = 0.0f;

    // ---- Advance fade (runs headed AND headless so logic is deterministic) ----
    if (m_fadeOut) {
        m_fade -= dt * m_fadeOutScale / kFadeOut;
        if (m_fade < 0.0f) m_fade = 0.0f;
    } else {
        m_fade += dt / kFadeIn;
        if (m_fade > 1.0f) m_fade = 1.0f;
    }

    // ---- Advance the tip rotation clock ----
    m_tipTimer += dt;
    while (m_tipTimer >= kTipPeriod) {
        m_tipTimer -= kTipPeriod;
        m_tipIndex = (m_tipIndex + 1) % kTipCount;
    }

    // Draw only when there's a real frame (headless: state advanced, nothing drawn).
    if (frame.valid && m_fade > 0.0f) draw(device, frame);
}

void LoadingScreen::draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame) {
    uint32_t W = 0, H = 0;
    device.hudSize(W, H);
    if (W == 0 || H == 0) return;
    const float w = (float)W, h = (float)H;
    const float a = std::clamp(m_fade, 0.0f, 1.0f);

    using FontRole = x3::rhi::FontRole;

    // ---- BLACKOUT mode (post-cold-open wake): the overlay is a PURE BLACK field —
    // no gradient / title / bar / tips — so the cutscene's smash-to-black holds
    // seamlessly through the cell build, and the fade-out is the slow wake. ----
    if (m_blackout) {
        const float black[4] = { 0.0f, 0.0f, 0.0f, a };
        device.drawHudQuad(frame, 0.0f, 0.0f, w, h, black);
        return;
    }

    // ---- Full-bleed background (textured quad). drawHudQuad is a flat color, so
    // for the textured background we draw the bg texture via drawMeshless? No — the
    // HUD path only exposes a colored quad + text. We emulate the gradient with a
    // stack of horizontal color bands so the backdrop reads as the same sci-fi
    // gradient even though it is not a sampled texture. (The procedural texture is
    // still created at init for any future textured-quad HUD extension.) ----
    {
        const int bands = 48;
        for (int i = 0; i < bands; ++i) {
            const float t0 = (float)i / (float)bands;
            // Deep blue-black at top -> faint steel toward the bottom.
            float r = 0.018f + 0.030f * t0;
            float g = 0.028f + 0.050f * t0;
            float b = 0.060f + 0.110f * t0;
            const float band[4] = { r, g, b, a };
            device.drawHudQuad(frame, 0.0f, t0 * h, w, h / (float)bands + 1.0f, band);
        }
        // Vignette: darken top + bottom strips a touch.
        const float topDark[4] = { 0.0f, 0.0f, 0.0f, 0.35f * a };
        device.drawHudQuad(frame, 0.0f, 0.0f, w, h * 0.12f, topDark);
        device.drawHudQuad(frame, 0.0f, h * 0.88f, w, h * 0.12f, topDark);
    }

    // ---- Title "ESCAPE FROM LAB ZERO" (Orbitron). Sized to fit the width. ----
    {
        const char* title = "ESCAPE FROM LAB ZERO";
        const float cx = w * 0.5f;
        const float probe = 72.0f;
        float titlePx = 72.0f;
        const float probeW = device.textAdvance(FontRole::Title, title, probe);
        if (probeW > 1.0f) titlePx = std::min(72.0f, probe * (w * 0.86f) / probeW);
        const float tw = device.textAdvance(FontRole::Title, title, titlePx);
        const float tx = cx - tw * 0.5f;
        const float ty = h * 0.28f;
        const float col[4] = { kTitleCol[0], kTitleCol[1], kTitleCol[2], a };
        const float sh[4]  = { 0.0f, 0.0f, 0.0f, kShadow[3] * a };
        device.drawHudTextF(frame, FontRole::Title, title, tx + 2.0f, ty + 2.0f, titlePx, sh);
        device.drawHudTextF(frame, FontRole::Title, title, tx, ty, titlePx, col);

        // Subtitle.
        const char* sub = "Level 1 - Awakening";
        const float subPx = std::max(14.0f, titlePx * 0.24f);
        const float sw = device.textAdvance(FontRole::Menu, sub, subPx);
        const float scol[4] = { kSubCol[0], kSubCol[1], kSubCol[2], a };
        device.drawHudTextF(frame, FontRole::Menu, sub, cx - sw * 0.5f,
                            ty + titlePx + 10.0f, subPx, scol);
    }

    // ---- Progress bar (thin, near the bottom) ----
    {
        const float barW = std::min(w * 0.6f, 720.0f);
        const float barH = 10.0f;
        const float bx = (w - barW) * 0.5f;
        const float by = h * 0.84f;
        const float frac = std::clamp(m_progress, 0.0f, 1.0f);
        // Plate / border.
        const float border = 2.0f;
        const float plate[4] = { 0.0f, 0.0f, 0.0f, 0.55f * a };
        device.drawHudQuad(frame, bx - border, by - border, barW + 2 * border, barH + 2 * border, plate);
        const float track[4] = { kBarTrack[0], kBarTrack[1], kBarTrack[2], a };
        device.drawHudQuad(frame, bx, by, barW, barH, track);
        const float fill[4]  = { kBarFill[0], kBarFill[1], kBarFill[2], a };
        device.drawHudQuad(frame, bx, by, barW * frac, barH, fill);
        // Bright leading edge on the fill.
        if (frac > 0.001f) {
            const float edge[4] = { kBarEdge[0], kBarEdge[1], kBarEdge[2], a };
            device.drawHudQuad(frame, bx + barW * frac - 2.0f, by, 2.0f, barH, edge);
        }
        // Percent + status label, right under the bar (right-aligned percent, left label).
        char pct[64];
        std::snprintf(pct, sizeof(pct), "%3d%%", (int)(frac * 100.0f + 0.5f));
        const float lblPx = 14.0f;
        const float pw = device.textAdvance(FontRole::HudMono, pct, lblPx);
        const float lcol[4] = { kSubCol[0], kSubCol[1], kSubCol[2], a };
        device.drawHudTextF(frame, FontRole::HudMono, pct, bx + barW - pw,
                            by + barH + 6.0f, lblPx, lcol);
        if (!m_label.empty())
            device.drawHudTextF(frame, FontRole::Menu, m_label.c_str(), bx,
                                by + barH + 6.0f, lblPx, lcol);
    }

    // ---- Rotating tip / lore line, centered under the bar ----
    {
        const char* t = kTips[m_tipIndex % kTipCount];
        const float tipPx = 16.0f;
        const float tw = device.textAdvance(FontRole::Menu, t, tipPx);
        const float tx = (w - tw) * 0.5f;
        const float ty = h * 0.84f + 36.0f;
        const float col[4] = { kTipCol[0], kTipCol[1], kTipCol[2], a };
        const float sh[4]  = { 0.0f, 0.0f, 0.0f, kShadow[3] * a };
        device.drawHudTextF(frame, FontRole::Menu, t, tx + 1.0f, ty + 1.0f, tipPx, sh);
        device.drawHudTextF(frame, FontRole::Menu, t, tx, ty, tipPx, col);
    }
}

// ===========================================================================
// --test-loading : headless self-test (no window / Vulkan).
// ===========================================================================
bool runLoadingSelfTest() {
    int pass = 0, fail = 0;
    auto check = [&](bool ok, const char* name) {
        if (ok) { ++pass; x3::logInfo(std::string("  PASS ") + name); }
        else    { ++fail; x3::logError(std::string("  FAIL ") + name); }
    };

    HeadlessRenderDevice dev;
    x3::rhi::FrameContext fc{};   // invalid -> draws skipped, state still advances

    // ---- 1) Progress is monotonic 0 -> 1 across the ordered steps ----------
    {
        LoadingScreen ls;
        ls.init(dev);
        check(ls.progress() == 0.0f, "L1 starts at 0");
        float prev = ls.progress();
        bool mono = true;
        const LoadStep order[] = {
            LoadStep::Start, LoadStep::DeviceReady, LoadStep::AssetsMounted,
            LoadStep::AudioReady, LoadStep::PhysicsReady, LoadStep::WorldGeometry,
            LoadStep::Spawns, LoadStep::FxReady, LoadStep::Done,
        };
        for (LoadStep s : order) {
            ls.step(s);
            if (ls.progress() < prev) mono = false;   // never goes backwards
            prev = ls.progress();
        }
        check(mono, "L2 progress monotonic non-decreasing");
        check(ls.progress() == 1.0f, "L3 reaches 1.0 at Done");

        // A later call with an EARLIER step must not lower the high-water mark.
        ls.step(LoadStep::AssetsMounted);
        check(ls.progress() == 1.0f, "L4 earlier step does not regress progress");

        // Direct setProgress is also clamped + monotonic.
        ls.setProgress(0.1f);
        check(ls.progress() == 1.0f, "L5 setProgress cannot lower the mark");
    }

    // ---- 2) A fresh screen ramps progress strictly upward through the steps -
    {
        LoadingScreen ls; ls.init(dev);
        const LoadStep order[] = {
            LoadStep::DeviceReady, LoadStep::AssetsMounted, LoadStep::AudioReady,
            LoadStep::PhysicsReady, LoadStep::WorldGeometry, LoadStep::Spawns,
            LoadStep::FxReady, LoadStep::Done,
        };
        bool strictlyUp = true;
        float prev = ls.progress();
        for (LoadStep s : order) {
            ls.step(s);
            if (!(ls.progress() > prev)) strictlyUp = false;
            prev = ls.progress();
        }
        check(strictlyUp, "L6 each step advances the bar (strictly increasing)");
    }

    // ---- 3) Tip rotation cycles through the array over time ----------------
    {
        LoadingScreen ls; ls.init(dev);
        const int n = ls.tipCount();
        check(n >= 3, "L7 has several authored tips");
        const int start = ls.tipIndex();
        // Advance ~one period at a time; the index must change, and over n periods
        // it must visit every index (a full cycle) and return to the start.
        std::vector<bool> seen((size_t)n, false);
        seen[(size_t)start] = true;
        int changes = 0, lastIdx = start;
        for (int i = 0; i < n; ++i) {
            ls.render(dev, fc, 2.0f);   // exactly one tip period
            const int idx = ls.tipIndex();
            if (idx != lastIdx) ++changes;
            seen[(size_t)idx] = true;
            lastIdx = idx;
        }
        bool all = true;
        for (bool s : seen) if (!s) all = false;
        check(changes >= n - 1, "L8 tip index advances each ~2s period");
        check(all, "L9 tip rotation visits every authored tip in one cycle");
        check(ls.tipIndex() == start, "L10 tip rotation wraps back to the start");
    }

    // ---- 4) Fade-in ramps up, fade-out ramps to gone (no pop) --------------
    {
        LoadingScreen ls; ls.init(dev);
        check(ls.alpha() == 0.0f, "L11 fade starts at 0 (fade-in)");
        ls.render(dev, fc, 1.0f);   // > kFadeIn -> fully in
        check(ls.alpha() >= 0.999f, "L12 fade-in reaches full");
        ls.beginFadeOut();
        check(ls.fadingOut() && !ls.faded(), "L13 fade-out begun, not yet gone");
        ls.render(dev, fc, 1.0f);   // > kFadeOut -> gone
        check(ls.alpha() <= 0.001f, "L14 fade-out reaches 0");
        check(ls.faded(), "L15 faded() true once fully out");
    }

    // ---- 5) render() with an INVALID frame must not draw / crash (headless) -
    {
        LoadingScreen ls; ls.init(dev);
        ls.step(LoadStep::WorldGeometry);
        ls.render(dev, fc, 0.016f);   // invalid frame: state advances, no draw, no hang
        check(ls.progress() > 0.0f, "L16 headless render advances state without a frame");
        ls.shutdown(dev);
    }

    x3::logInfo(std::string("--test-loading: ") + std::to_string(pass) + " passed, " +
                std::to_string(fail) + " failed");
    return fail == 0;
}

} // namespace x3::game
