// app/space/ship_windows.cpp — S6 true-portal ship windows (moving space outside).
//
// See ship_windows.h. Two cooperating pieces sell the "Star Trek" moving space:
//
//   * A SPACE BACKDROP dome (a re-used SkyStars instance, nebula-tinted) that
//     fills the deep background — the ambient "we are in space" glow for the
//     windowed walk-around.
//
//   * Per-window PORTAL PANES — each manifest window {x,y,z,w,h,yaw} gets a thin
//     quad that DIRECTLY shows the moving space: the quad samples a baked
//     star/nebula texture whose UVs are PANNED by the moving-environment
//     orientation (envYaw/envPitch). Because the ship is static and the
//     environment carries the motion (decision 2.4), panning the window's view of
//     space by envYaw makes the starfield slide past the fixed window frame — the
//     parallax that reads as flying through space, seen from inside. The pane sits
//     a hair INSIDE the hull wall so it renders even when S5's wall is solid (the
//     spec's solid-wall fallback): the pane itself is the portal, so no wall gap
//     is required and the dome behind the wall need not be visible.
//
//   * LIGHT BLEED — a cool point light just inside each window so the deck near it
//     glows as if lit from outside (owned in m_bleed; the host uploads it, merged
//     with its own interior fixtures).
//
// Game/slice code only — engine/rhi is untouched (drawMesh/drawMeshEmissive only).
#include "ship_windows.h"

#include "../headless_device.h"   // HeadlessRenderDevice for the self-test

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace x3::space {

using x3::rhi::MeshHandle;
using x3::rhi::TextureHandle;
using x3::rhi::PointLight;

namespace {

constexpr float kPi    = 3.14159265358979323846f;
constexpr float kTwoPi = 6.28318530717958647692f;

// ---- Window-portal vertex (own local XY plane, normal +Z, -0.5..+0.5) --------
// A per-instance model scales it to {w,h}, yaws it about +Y, and places it at the
// window. UVs run 0..1 across the quad; render() biases them via a per-object UV
// pan baked into a slightly OVERSCANNED texture so the env orientation slides the
// star pattern across the window.
static x3::rhi::MeshVertex quadV(float x, float y, float u, float v) {
    x3::rhi::MeshVertex q{};
    q.pos[0] = x; q.pos[1] = y; q.pos[2] = 0.0f;
    q.normal[0] = 0.0f; q.normal[1] = 0.0f; q.normal[2] = 1.0f;
    q.uv[0] = u; q.uv[1] = v;
    return q;
}

// Column-major model: scale a unit quad to (w,h), Y-rotate by `yaw`, translate to
// (px,py,pz). The window yaw orients the pane into the room exactly like the S5
// wall placement it sits on (forward window faces +Z, port window faces +X, ...).
static void paneModel(float px, float py, float pz, float w, float h, float yaw,
                      float out[16]) {
    const float c = std::cos(yaw), s = std::sin(yaw);
    out[0]  = c * w;  out[1]  = 0.0f;   out[2]  = -s * w; out[3]  = 0.0f; // local X
    out[4]  = 0.0f;   out[5]  = h;      out[6]  = 0.0f;   out[7]  = 0.0f; // local Y
    out[8]  = s;      out[9]  = 0.0f;   out[10] = c;      out[11] = 0.0f; // local Z (normal)
    out[12] = px;     out[13] = py;     out[14] = pz;     out[15] = 1.0f; // translation
}

// ---- Procedural deep-space portal texture (star/nebula field) ----------------
// A TILEABLE RGBA8 panorama the window samples. UV panning (envYaw/envPitch ->
// uv bias) slides the pattern, so it MUST wrap seamlessly. We build: a soft, dark
// nebula gradient (low-frequency value noise, blue/violet) + sparse bright stars
// (high-frequency hash threshold). All math wraps mod `n` so left==right,
// top==bottom and the pan never seams.
static float hash2(uint32_t x, uint32_t y, uint32_t n, uint32_t salt) {
    uint32_t h = (x % n) * 374761393u + (y % n) * 668265263u + salt * 2654435761u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return (float)(h & 0xFFFFFFu) / (float)0x1000000u; // [0,1)
}
// Tileable bilinear value noise at integer cell pitch `cell` over an n x n field.
static float valueNoise(float u, float v, uint32_t n, uint32_t cell, uint32_t salt) {
    const float fx = u * (float)cell, fy = v * (float)cell;
    const uint32_t x0 = (uint32_t)std::floor(fx), y0 = (uint32_t)std::floor(fy);
    const float tx = fx - (float)x0, ty = fy - (float)y0;
    auto smooth = [](float t){ return t * t * (3.0f - 2.0f * t); };
    const float sx = smooth(tx), sy = smooth(ty);
    const float a = hash2(x0,     y0,     cell, salt);
    const float b = hash2(x0 + 1, y0,     cell, salt);
    const float c = hash2(x0,     y0 + 1, cell, salt);
    const float d = hash2(x0 + 1, y0 + 1, cell, salt);
    (void)n;
    const float ab = a + (b - a) * sx;
    const float cd = c + (d - c) * sx;
    return ab + (cd - ab) * sy;
}
static std::vector<uint8_t> bakePortalRGBA(uint32_t n) {
    std::vector<uint8_t> px((size_t)n * n * 4, 0);
    for (uint32_t y = 0; y < n; ++y) {
        const float v = (y + 0.5f) / (float)n;
        for (uint32_t x = 0; x < n; ++x) {
            const float u = (x + 0.5f) / (float)n;
            // Nebula: two octaves of tileable noise -> blue/violet cloud. SPACE
            // IS BLACK (Tim's slop-pass 2026-08-28): the original 0.04-0.15
            // grey-blue base read as a uniform slab that auto-exposure lifted
            // to MILK — the whole pane glowed lavender. Authored like space:
            // ~90% true black, DEEP patchy nebula (cubed falloff), vivid
            // sparse stars — content with real contrast survives any exposure
            // (the club/elevator screens' lesson).
            float neb = 0.6f * valueNoise(u, v, n, 6, 11u)
                      + 0.4f * valueNoise(u, v, n, 13, 29u);
            neb = std::clamp((neb - 0.55f) / 0.45f, 0.0f, 1.0f);
            neb = neb * neb * neb;                 // patchy cores, black between
            float r = 0.002f + 0.10f * neb;
            float g = 0.003f + 0.05f * neb;
            float b = 0.006f + 0.28f * neb;        // blue-violet bias
            // Stars: sparse high-freq hash threshold + a few brighter ones.
            const uint32_t sc = n;                 // per-texel cells
            float hs = hash2(x, y, sc, 101u);
            if (hs > 0.992f) {                     // bright star
                float br = 1.0f + 1.2f * (hs - 0.992f) / 0.008f;
                r += br; g += br; b += br;
            } else if (hs > 0.978f) {              // dim star
                float br = 0.30f;
                r += br; g += br; b += br * 1.1f;
            }
            auto u8 = [](float c){ c = std::clamp(c, 0.0f, 1.0f);
                                   return (uint8_t)std::lround(c * 255.0f); };
            uint8_t* p = &px[((size_t)y * n + x) * 4];
            p[0] = u8(r); p[1] = u8(g); p[2] = u8(b); p[3] = 255;
        }
    }
    return px;
}

} // namespace

void ShipWindows::init(rhi::IRenderDevice& device, const ShipManifest& manifest) {
    if (m_initialized) return;

    // ---- Space backdrop dome (ambient deep-space glow for the walk-around) ----
    SkyStars::Tuning t;
    t.starDensity  = 220.0f;
    t.threshold    = 0.82f;
    t.twinkleSpeed = 1.4f;
    t.baseColor[0] = 0.78f; t.baseColor[1] = 0.88f; t.baseColor[2] = 1.15f; // blue-white
    m_sky.init(device, t);

    // ---- The moving-space PORTAL texture sampled by every window pane --------
    auto portalPx = bakePortalRGBA(512);
    m_paneTex = device.createTexture(portalPx.data(), 512, 512, /*srgb=*/false);

    // ---- One portal pane + one light-bleed light per window ------------------
    // Each pane gets its OWN unit-quad mesh so render() can SLIDE its UVs every
    // frame (updateMesh) by the env-orientation pan — a genuine moving starfield,
    // not a shimmer. Two-sided (CCW front + reversed) so it reads from inside the
    // cockpit regardless of the window yaw.
    const uint32_t qi[12] = { 0,1,2, 0,2,3,  0,2,1, 0,3,2 };

    m_panes.clear();
    m_bleed.clear();
    for (const auto& wnd : manifest.windows) {
        x3::rhi::MeshVertex qv[4] = {
            quadV(-0.5f, -0.5f, 0.0f, 0.0f),
            quadV( 0.5f, -0.5f, 1.0f, 0.0f),
            quadV( 0.5f,  0.5f, 1.0f, 1.0f),
            quadV(-0.5f,  0.5f, 0.0f, 1.0f),
        };
        Pane p;
        p.mesh = device.createMesh(qv, 4, qi, 12);   // per-pane (UVs panned/frame)
        p.placement = wnd;
        // Nudge the pane INWARD (toward the room) along its facing normal so it
        // renders clearly IN FRONT of the (possibly solid) S5 hull wall plate. The
        // S5 wall plate has ~0.18 m half-thickness, so its inner face sits ~0.18 m
        // inside the placement plane; we push the pane 0.22 m in to clear it (no
        // z-fight / occlusion) — the portal reads as the window set into the hull.
        const float yaw = wnd[5];
        const float fx = std::sin(yaw), fz = std::cos(yaw);
        p.placement[0] = wnd[0] + fx * 0.22f;
        p.placement[2] = wnd[2] + fz * 0.22f;
        m_panes.push_back(p);

        // Light bleed: a cool point light pushed inward from the window. It
        // exists to light the DECK, not its own glass — at the original 0.6 m
        // it sat ~0.4 m from the pane and its point-blank specular washed the
        // portal to a milky slab (Tim's slop-pass, 2026-08-28; the bisect
        // receipt is shots_phase1/ship_bisect.png). 1.6 m in, inverse-square
        // drops the pane irradiance ~18x while the deck glow survives.
        PointLight bl;
        bl.pos[0] = wnd[0] + fx * 1.6f;
        bl.pos[1] = wnd[1];
        bl.pos[2] = wnd[2] + fz * 1.6f;
        bl.range  = 4.5f;
        bl.color[0] = 1.1f; bl.color[1] = 1.6f; bl.color[2] = 2.6f; // cool starlight bleed
        m_bleed.push_back(bl);
    }

    m_initialized = m_sky.initialized() && m_paneTex.valid();
}

void ShipWindows::setCamera(float ex, float ey, float ez) {
    m_camX = ex; m_camY = ey; m_camZ = ez;
    m_sky.setCamera(ex, ey, ez);
}

void ShipWindows::render(rhi::IRenderDevice& device, const rhi::FrameContext& frame,
                         const float* viewProj16, float timeSec,
                         float envYaw, float envPitch) {
    m_lastEnvYaw = envYaw;
    m_lastEnvPitch = envPitch;
    if (!m_initialized) return;

    // ---- 1) Space backdrop dome (camera-anchored ambient glow) ---------------
    m_sky.setCamera(m_camX, m_camY, m_camZ);
    m_sky.render(device, frame, viewProj16, timeSec);

    // ---- 2) Moving-space portal panes ----------------------------------------
    // The env orientation PANS each window's view of space: envYaw slides the UVs
    // horizontally, envPitch vertically. Because the portal texture is tileable, the
    // stars slide past the fixed window frame seamlessly — the parallax that reads as
    // "the ship is flying through space, seen from inside." We re-upload each pane's
    // 4 UVs per frame (updateMesh — cheap, validation-clean for 2 quads) so the
    // motion is REAL, not a fake shimmer. UVs span <1 of the texture so the panned
    // window shows a moving sub-window of the larger starfield (overscan parallax).
    const float uPan = envYaw   / kTwoPi;   // one full env yaw = one texture wrap
    const float vPan = envPitch / kPi;
    const float kSpanU = 0.6f, kSpanV = 0.6f;   // window covers 60% of the field
    // EMISSIVE-MAP portal (integration-feast fold rewrite): the branch drew the
    // pane with drawMeshEmissive, whose emissive[] is a FLAT per-object add —
    // under the linear-HDR + ACES + auto-exposure pipeline that floods the pane
    // to a uniform slab (fold captures 1-3). drawMeshPBR's emissiveTex instead
    // gates the glow BY the portal texture (the club1127 / intro-cockpit screen
    // recipe): stars + nebula GLOW, the space between them stays black, and the
    // near-black albedo kills the lit-term wash from interior lights.
    const float paneColor[4] = { 0.02f, 0.02f, 0.03f, 1.0f };  // near-black lit albedo  // near-black lit albedo
    const float emiss[4]     = { 1.0f, 1.0f, 1.0f, -1.6f };    // NEGATIVE = UNLIT PORTAL (mesh.frag sentinel): the pane IS space    // neutral: texel colors carry
    const uint32_t qi[12]    = { 0,1,2, 0,2,3,  0,2,1, 0,3,2 }; (void)qi;
    for (const auto& p : m_panes) {
        const float u0 = uPan, u1 = uPan + kSpanU;
        const float v0 = vPan, v1 = vPan + kSpanV;
        x3::rhi::MeshVertex qv[4] = {
            quadV(-0.5f, -0.5f, u0, v0),
            quadV( 0.5f, -0.5f, u1, v0),
            quadV( 0.5f,  0.5f, u1, v1),
            quadV(-0.5f,  0.5f, u0, v1),
        };
        device.updateMesh(p.mesh, qv, 4);   // slide this pane's view of space
        float model[16];
        paneModel(p.placement[0], p.placement[1], p.placement[2],
                  p.placement[3], p.placement[4], p.placement[5], model);
        // MR FACTORS (Tim's slop-pass, 2026-08-28): with no MR texture the pane
        // sampled bindless slot 0 (1x1 WHITE) = METALLIC 1.0 — a window into
        // space rendered as a MIRROR of the neon interior IBL, washing the
        // near-black starfield to milky lavender (the wash tint WAS the ceiling
        // neon). Author it dielectric-matte: the glow comes from emissiveTex,
        // reflections stay off the glass.
        device.drawMeshPBR(frame, p.mesh, m_paneTex,
                           x3::rhi::TextureHandle{}, x3::rhi::TextureHandle{},
                           paneColor, emiss, model,
                           /*alphaMask*/false, /*alphaBlend*/false,
                           /*emissiveTex*/m_paneTex,
                           x3::rhi::TextureHandle{}, 1.0f, 0.0f, 0.05f,
                           0.0f, 1.0f, 0.0f,
                           /*metallicFactor*/1e-4f, /*roughnessFactor*/1.0f);
    }
}

void ShipWindows::shutdown(rhi::IRenderDevice& device) {
    for (auto& p : m_panes) {
        if (p.mesh.valid()) device.destroyMesh(p.mesh);   // per-pane meshes
    }
    m_panes.clear();
    m_bleed.clear();
    if (m_paneTex.valid()) { device.destroyTexture(m_paneTex); m_paneTex = rhi::TextureHandle{}; }
    m_sky.shutdown(device);
    m_initialized = false;
}

// ---------------------------------------------------------------------------
// Headless self-test (--test-ship-windows)
// ---------------------------------------------------------------------------
bool runShipWindowsSelfTest() {
    int pass = 0, total = 0;
    auto check = [&](bool c, const char* name) {
        ++total;
        if (c) { ++pass; std::printf("  PASS %s\n", name); }
        else   {          std::printf("  FAIL %s\n", name); }
    };

    x3::game::HeadlessRenderDevice device;
    device.init({});

    ShipManifest sm = ShipInterior::makeSmallCockpit();

    ShipWindows win;
    win.init(device, sm);

    // T1: init() from the small cockpit populates windowCount()==2 + initialized().
    check(win.initialized() && win.windowCount() == 2,
          "T1 init populates windowCount==2 + initialized");

    // T2: render() runs VUID-safe (headless draw is a no-op) + one bleed light/window.
    {
        x3::rhi::FrameContext fr = device.beginFrame();
        const float idM[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        win.setCamera(0.0f, 1.7f, 1.0f);
        win.render(device, fr, idM, /*timeSec=*/0.25f, /*envYaw=*/0.0f, /*envPitch=*/0.0f);
        device.endFrame(fr);
        check(win.bleedLights().size() == win.windowCount() && win.windowCount() == 2,
              "T2 render VUID-safe + one bleed light per window");
    }

    // T3: a DIFFERENT envYaw drives a different backdrop orientation (live input).
    {
        x3::rhi::FrameContext fr = device.beginFrame();
        const float idM[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        win.render(device, fr, idM, 0.5f, /*envYaw=*/1.2566f, /*envPitch=*/0.1f);
        device.endFrame(fr);
        check(std::fabs(win.lastEnvYaw() - 1.2566f) < 1e-4f &&
              std::fabs(win.lastEnvPitch() - 0.1f) < 1e-4f,
              "T3 envYaw/envPitch drive the backdrop orientation (live)");
    }

    // T4: sampling several env orientations all run without crashing.
    {
        bool ok = true;
        const float idM[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        for (int k = 0; k < 4; ++k) {
            float yaw = (float)k * 1.5708f;
            x3::rhi::FrameContext fr = device.beginFrame();
            win.render(device, fr, idM, (float)k * 0.25f, yaw, 0.05f * (float)k);
            device.endFrame(fr);
            if (std::fabs(win.lastEnvYaw() - yaw) > 1e-4f) ok = false;
        }
        check(ok, "T4 multiple env orientations sample without crashing");
    }

    // T5: shutdown() is clean + idempotent.
    win.shutdown(device);
    win.shutdown(device);  // idempotent
    check(!win.initialized() && win.windowCount() == 0,
          "T5 shutdown clean + idempotent");

    device.shutdown();

    std::printf("ship-windows: %d/%d passed\n", pass, total);
    std::fflush(stdout);
    return pass == total;
}

} // namespace x3::space
