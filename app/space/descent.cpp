// app/space/descent.cpp — S4 cinematic atmospheric descent. See descent.h.
//
// Implementation notes:
//   - NO custom pipelines / NO RHI changes. The entry effect is composed from
//     the EXISTING IRenderDevice draw paths over self-contained procedural
//     meshes, exactly like app/sky_stars.* :
//       * drawMeshGlass() over a camera-anchored inside-out dome -> the
//         translucent atmosphere FOG / re-entry HEAT tint (alpha + emissive
//         glow ramped by the descent curve), and
//       * drawMeshGlass() over a handful of camera-facing quad slabs -> the
//         CLOUD-STREAK layers that rush past as the environment falls.
//   - The runner the SpaceLayer spine ticks is a small lambda closing over this
//     object: each update(dt) advances m_timer/m_duration -> m_progress and
//     returns true at >= 1.0, so S0 flips AtmoDescent -> Surface (the surface
//     `--world` handoff is STUBBED for Wave 2 — see descent.h).

#include "descent.h"
#include "space_layer.h"

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace x3::space {

namespace {

// Inside-out sphere (same construction as SkyStars::buildSkydome): duplicated
// winding so it survives BACK_BIT cull from the inside without a pipeline change.
struct DomeMesh {
    std::vector<rhi::MeshVertex> verts;
    std::vector<uint32_t>        index;
};

static DomeMesh buildDome(float radius, uint32_t bands) {
    DomeMesh m;
    const uint32_t stacks = bands;
    const uint32_t slices = bands * 2;
    m.verts.reserve((stacks + 1) * (slices + 1));
    for (uint32_t i = 0; i <= stacks; ++i) {
        float v = (float)i / (float)stacks;
        float phi = v * 3.14159265f;
        float cy = std::cos(phi), sy = std::sin(phi);
        for (uint32_t j = 0; j <= slices; ++j) {
            float u = (float)j / (float)slices;
            float th = u * 6.2831853f;
            float cx = std::cos(th), sz = std::sin(th);
            float px = sy * cx, py = cy, pz = sy * sz;
            rhi::MeshVertex vtx{};
            vtx.pos[0] = px * radius; vtx.pos[1] = py * radius; vtx.pos[2] = pz * radius;
            vtx.normal[0] = -px; vtx.normal[1] = -py; vtx.normal[2] = -pz;
            vtx.uv[0] = u; vtx.uv[1] = v;
            m.verts.push_back(vtx);
        }
    }
    for (uint32_t i = 0; i < stacks; ++i) {
        for (uint32_t j = 0; j < slices; ++j) {
            uint32_t r0 = i * (slices + 1) + j;
            uint32_t r1 = (i + 1) * (slices + 1) + j;
            // Outward CCW (back-face from inside, culled).
            m.index.push_back(r0); m.index.push_back(r1); m.index.push_back(r0 + 1);
            m.index.push_back(r1); m.index.push_back(r1 + 1); m.index.push_back(r0 + 1);
            // Inverted CCW (front-face from INSIDE — the visible one).
            m.index.push_back(r0); m.index.push_back(r0 + 1); m.index.push_back(r1);
            m.index.push_back(r1); m.index.push_back(r0 + 1); m.index.push_back(r1 + 1);
        }
    }
    return m;
}

// A unit XY quad centered at origin, facing +Z. Reused for every cloud-streak
// layer (the model matrix scales/positions each instance).
static void buildQuad(std::vector<rhi::MeshVertex>& verts, std::vector<uint32_t>& index) {
    const float h = 0.5f;
    rhi::MeshVertex v0{}; v0.pos[0]=-h; v0.pos[1]=-h; v0.pos[2]=0; v0.normal[2]=1; v0.uv[0]=0; v0.uv[1]=0;
    rhi::MeshVertex v1{}; v1.pos[0]= h; v1.pos[1]=-h; v1.pos[2]=0; v1.normal[2]=1; v1.uv[0]=1; v1.uv[1]=0;
    rhi::MeshVertex v2{}; v2.pos[0]= h; v2.pos[1]= h; v2.pos[2]=0; v2.normal[2]=1; v2.uv[0]=1; v2.uv[1]=1;
    rhi::MeshVertex v3{}; v3.pos[0]=-h; v3.pos[1]= h; v3.pos[2]=0; v3.normal[2]=1; v3.uv[0]=0; v3.uv[1]=1;
    verts = { v0, v1, v2, v3 };
    // Double-winded so it shows from either side under BACK_BIT cull.
    index = { 0,1,2,  0,2,3,   0,2,1,  0,3,2 };
}

// Soft horizontal cloud-streak texture: bright wispy bands on a transparent
// field. Alpha encodes the streak shape; the glass pass uses it for cloud cover.
static std::vector<uint8_t> bakeStreakRGBA(uint32_t w, uint32_t h) {
    std::vector<uint8_t> px((size_t)w * h * 4, 0);
    auto fract = [](float x) { return x - std::floor(x); };
    auto hash = [&](float x) { return fract(std::sin(x * 12.9898f) * 43758.5453f); };
    for (uint32_t y = 0; y < h; ++y) {
        float fy = (float)y / (float)h;
        for (uint32_t x = 0; x < w; ++x) {
            float fx = (float)x / (float)w;
            // Stacked horizontal bands with per-band jitter -> wispy streaks.
            float band = 0.0f;
            for (int k = 0; k < 5; ++k) {
                float center = hash((float)k * 7.7f + 1.3f);
                float width  = 0.04f + 0.06f * hash((float)k * 3.1f);
                float d = std::fabs(fy - center);
                float along = 0.5f + 0.5f * std::sin((fx * 6.2831853f) * (1.0f + (float)k) + (float)k);
                float b = std::max(0.0f, 1.0f - d / std::max(width, 1e-4f)) * (0.4f + 0.6f * along);
                band = std::max(band, b);
            }
            float a = std::clamp(band, 0.0f, 1.0f);
            uint8_t* p = &px[((size_t)y * w + x) * 4];
            // Warm, slightly orange cloud (re-entry lit) — RGB scaled by alpha.
            p[0] = (uint8_t)std::lround(std::clamp(1.0f, 0.0f, 1.0f) * a * 255.0f);
            p[1] = (uint8_t)std::lround(0.78f * a * 255.0f);
            p[2] = (uint8_t)std::lround(0.62f * a * 255.0f);
            p[3] = (uint8_t)std::lround(a * 255.0f);
        }
    }
    return px;
}

} // namespace

// ---------------------------------------------------------------------------
// Heat-intensity curve: 0 at p=0, peaks near the middle (the re-entry fireball),
// eases back to ~0 (clear sky) at p=1. A simple parabola-ish bump biased to the
// mid-descent, with a little persistent atmospheric haze early/late.
// ---------------------------------------------------------------------------
float descentHeatIntensity(float p) {
    p = std::clamp(p, 0.0f, 1.0f);
    // 4*p*(1-p) is a unit parabola that is exactly 0 at p=0 and p=1 and peaks at
    // 1.0 at p=0.5 — so the fireball glow starts at clear sky, swells through the
    // heart of atmospheric entry, and eases back to clear sky on touchdown. The
    // exponent sharpens the peak so the hot phase reads as a punchy fireball
    // rather than a broad smear. (Residual atmospheric haze is added separately
    // in render() via an opacity floor, NOT here, so the curve is true-zero at
    // the ends and the --test asserts that shape.)
    float bump = std::max(0.0f, 4.0f * p * (1.0f - p));
    bump = std::pow(bump, 1.3f);
    return std::clamp(bump, 0.0f, 1.0f);
}

// ---------------------------------------------------------------------------
// init / shutdown / setCamera
// ---------------------------------------------------------------------------
void AtmoDescent::init(rhi::IRenderDevice& dev, SpaceLayer& layer, float durationSec) {
    m_duration = std::max(0.25f, durationSec);
    m_timer    = 0.0f;
    m_progress = 0.0f;
    m_active   = false;

    if (!m_initialized) {
        // Fog/heat dome — modest radius, re-centered on the camera each frame so
        // it reads as a fullscreen-ish tint regardless of camera position.
        DomeMesh dome = buildDome(/*radius=*/40.0f, /*bands=*/24);
        m_dome = dev.createMesh(dome.verts.data(), (uint32_t)dome.verts.size(),
                                dome.index.data(), (uint32_t)dome.index.size());

        std::vector<rhi::MeshVertex> qv; std::vector<uint32_t> qi;
        buildQuad(qv, qi);
        m_streak = dev.createMesh(qv.data(), (uint32_t)qv.size(), qi.data(), (uint32_t)qi.size());

        std::vector<uint8_t> spx = bakeStreakRGBA(256, 256);
        m_streakTex = dev.createTexture(spx.data(), 256, 256, /*srgb=*/true);

        m_initialized = m_dome.valid() && m_streak.valid() && m_streakTex.valid();
        if (!m_initialized) x3::logError("AtmoDescent::init: mesh/texture creation failed");
    }

    // Register the descent runner with the S0 spine (frozen contract). The lambda
    // closes over `this`; each update(dt) the spine ticks it until it returns true
    // (descent complete), at which point S0 lands in Context::Surface. The surface
    // `--world` handoff itself is STUBBED for Wave 2 (see descent.h).
    layer.registerDescentRunner([this](float dt) { return this->tick(dt); });
}

void AtmoDescent::setCamera(float ex, float ey, float ez) {
    m_camX = ex; m_camY = ey; m_camZ = ez;
}

void AtmoDescent::shutdown(rhi::IRenderDevice& dev) {
    if (m_dome.valid())      { dev.destroyMesh(m_dome);        m_dome = {}; }
    if (m_streak.valid())    { dev.destroyMesh(m_streak);      m_streak = {}; }
    if (m_streakTex.valid()) { dev.destroyTexture(m_streakTex); m_streakTex = {}; }
    m_initialized = false;
    m_active = false;
}

// ---------------------------------------------------------------------------
// Runner tick — the function the SpaceLayer spine drives each update(dt).
// ---------------------------------------------------------------------------
bool AtmoDescent::tick(float dt) {
    if (dt > 0.0f) m_timer += dt;
    m_progress = std::clamp(m_timer / m_duration, 0.0f, 1.0f);
    if (m_progress >= 1.0f) {
        m_active = false;
        return true;   // sequence complete -> S0 lands in Surface
    }
    m_active = true;
    return false;      // still descending
}

// ---------------------------------------------------------------------------
// render — hull glow + atmosphere fog + cloud streaks, ramped by progress().
// ---------------------------------------------------------------------------
void AtmoDescent::render(rhi::IRenderDevice& dev, const rhi::FrameContext& fr,
                         const float* viewProj16, float timeSec) {
    (void)viewProj16;  // dome is camera-anchored in this baked-mesh path
    if (!m_initialized) return;

    const float p    = std::clamp(m_progress, 0.0f, 1.0f);
    const float heat = descentHeatIntensity(p);   // 0 -> peak mid -> 0 clear

    // ---- Atmosphere fog / re-entry HEAT dome ------------------------------
    // A translucent dome whose tint goes from thin upper-atmosphere haze (cool
    // blue) toward a hot orange fireball at the heat peak, then clears. Opacity
    // and emissive glow both track `heat` so mid-descent reads as the hull-glow
    // fireball and the end eases to a clear sky.
    {
        // Lerp tint: cool blue haze (heat=0) -> hot orange (heat=1).
        const float coolR = 0.45f, coolG = 0.62f, coolB = 0.95f;
        const float hotR  = 1.00f, hotG  = 0.42f, hotB  = 0.12f;
        const float tr = coolR + (hotR - coolR) * heat;
        const float tg = coolG + (hotG - coolG) * heat;
        const float tb = coolB + (hotB - coolB) * heat;

        // baseColorFactor carries the tint; the glass opacity + emissive carry
        // the intensity. A small floor of haze keeps the sky non-black early/late.
        // Kept moderate so the heat tints the scene without flooding it to a flat
        // wash — the cloud streaks below must still read THROUGH the fog.
        const float haze = 0.12f + 0.30f * heat;     // dome opacity (semi-transparent)
        const float glow = 0.3f + 3.0f * heat;       // HDR emissive (drives bloom)
        const float baseFactor[4] = { tr, tg, tb, 1.0f };
        const float emissive[4]   = { tr * 0.9f, tg * 0.7f, tb * 0.5f, glow };

        // Camera-anchored model (identity rotation + camera translation).
        const float model[16] = {
            1,0,0,0, 0,1,0,0, 0,0,1,0, m_camX, m_camY, m_camZ, 1 };

        rhi::IRenderDevice::GlassMaterial g{};
        g.opacity      = std::clamp(haze, 0.0f, 1.0f);
        g.refraction   = 0.0f;            // a clean tint, no screen distortion
        g.roughness    = 1.0f;            // fully diffuse fog
        g.specular     = 0.0f;
        g.tint[0] = tr; g.tint[1] = tg; g.tint[2] = tb;
        dev.drawMeshGlass(fr, m_dome, rhi::TextureHandle{}, baseFactor, emissive, g, model);
    }

    // ---- CLOUD-STREAK layers ----------------------------------------------
    // A handful of camera-facing slabs at increasing distance ahead of the eye
    // that scroll/grow as the environment falls. They're densest in the early-to-
    // mid descent (entering the cloud deck) and thin out near the ground. Each
    // layer is the SAME quad, repositioned/scaled via its model matrix.
    {
        const int kLayers = 7;
        // Cloud cover fades in over the first ~60% then fades to clear sky.
        float cover = std::clamp(1.0f - std::fabs(p - 0.4f) / 0.6f, 0.0f, 1.0f);
        for (int i = 0; i < kLayers; ++i) {
            float fi = (float)i / (float)kLayers;
            auto fract = [](float x) { return x - std::floor(x); };
            // Each layer drifts past: scroll downward + outward with time + p so
            // successive frames differ and the deck "rushes" by during descent.
            float phase = timeSec * (0.6f + 0.3f * fi) + p * 4.0f + fi * 2.0f;
            float drift = std::fmod(phase, 2.0f) - 1.0f;          // [-1,1] loop
            // Spread layers in depth AHEAD of the eye (camera looks toward -Z),
            // each filling the frame; nearer slabs are smaller-looking but the
            // streak texture tiles so the whole deck rushes by.
            float depth = -8.0f - fi * 5.0f;                       // ahead of eye
            float scale = 26.0f + fi * 8.0f;                       // wide screen coverage
            float yoff  = drift * (5.0f + fi * 2.0f) - 1.5f;       // vertical rush, biased down
            // Stagger horizontally per layer so streaks don't all stack on one column.
            float xoff  = (fract((float)i * 0.61803f) - 0.5f) * 8.0f;

            // Per-layer opacity: cover * a per-layer falloff, with a soft flicker.
            float flick = 0.85f + 0.15f * std::sin(phase * 3.1f);
            float op = cover * (0.34f + 0.16f * (1.0f - fi)) * flick;
            if (op <= 0.01f) continue;

            // Camera-facing slab positioned ahead + below, staggered horizontally.
            const float model[16] = {
                scale, 0,     0,     0,
                0,     scale, 0,     0,
                0,     0,     1,     0,
                m_camX + xoff, m_camY + yoff, m_camZ + depth, 1 };

            // Warm sunlit cloud, tinted slightly toward the heat color mid-descent.
            float warm = 0.3f + 0.7f * heat;
            const float baseFactor[4] = {
                0.9f + 0.1f * warm, 0.85f - 0.1f * warm, 0.85f - 0.25f * warm, 1.0f };
            const float emissive[4] = { 0.6f * warm, 0.35f * warm, 0.15f * warm,
                                        1.5f * heat };
            rhi::IRenderDevice::GlassMaterial g{};
            g.opacity   = std::clamp(op, 0.0f, 1.0f);
            g.refraction = 0.0f;
            g.roughness  = 1.0f;
            g.specular   = 0.0f;
            dev.drawMeshGlass(fr, m_streak, m_streakTex, baseFactor, emissive, g, model);
        }
    }
}

} // namespace x3::space
