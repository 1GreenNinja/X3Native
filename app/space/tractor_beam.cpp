// Tractor-beam VFX -- see app/space/tractor_beam.h.
//
// Implementation notes:
//   - Like app/space/wormhole_vfx.cpp + app/sky_stars.cpp, the "shader-path"
//     version of this effect would create a custom fragment pipeline and march the
//     cone per pixel. The RHI surface (engine/rhi/*) intentionally hides pipeline
//     creation behind drawMesh*/drawMeshGlass and is OFF-LIMITS this lane. So the
//     beam is the EXISTING emissive-draw path: a baked energy texture wrapped on a
//     cone mesh, drawn via drawMeshEmissive() in linear HDR (so the cyan core
//     feeds the bloom chain).
//
//   - GEOMETRY: a UNIT cone authored along +Z. The APEX (emitter) sits at z=0 with
//     a small radius (emitterRadius); the BASE (captured ship) sits at z=1 with a
//     large radius (captureRadius) -- the tractor "funnel". It is a thin tube of
//     `slices` radial segments x `rings` axial segments (both windings emitted so
//     the surface survives BACK_BIT cull from any view, exactly like the
//     skydome/wormhole tube). UV: u = angle around the cone, v = distance along the
//     axis (0 at emitter, 1 at capture). The cone is built ONCE; each frame it is
//     re-oriented onto the live from->to axis by a model matrix this file builds
//     (basis from the axis + a perpendicular up), so no per-frame mesh churn.
//
//   - TEXTURE: a baked RGBA8 carrying CONCENTRIC ENERGY RINGS along the axis (V)
//     plus an EDGE-FALLOFF brightness so the beam reads soft (brightest at the
//     leading rings, fading at the cone seams). U (angle) is near-uniform so the
//     beam looks like a clean energy cone, not a striped tube.
//
//   - ANIMATION: the texture is baked static. The "pull" illusion = a per-frame
//     emissive PULSE whose phase scrolls with timeSec*flowSpeed (so the rings read
//     as flowing emitter->capture), and `intensity` (0..1) scales the overall beam
//     strength (the lock-on ramp). The host moves `to` toward `from` over the
//     capture beat; this class just draws the beam between whatever pair it's given.

#include "tractor_beam.h"

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace x3::space {

namespace {

constexpr float kTau = 6.28318530717959f;

static inline float fract1(float x) { return x - std::floor(x); }

// A thin cone tube authored along +Z: apex (emitter) at z=0 radius r0, base
// (capture) at z=1 radius r1. `slices` panes around the ring, `rings` axial
// segments. Both windings emitted so the surface survives BACK_BIT cull from any
// camera angle (the beam can be viewed from outside OR looking down its axis).
// UV: u = angle [0,1] around the cone, v = z [0,1] (0 emitter -> 1 capture).
struct ConeMesh {
    std::vector<rhi::MeshVertex> verts;
    std::vector<uint32_t>        index;
};

static ConeMesh buildCone(float r0, float r1, uint32_t slices, uint32_t rings) {
    ConeMesh m;
    slices = std::max<uint32_t>(slices, 3u);
    rings  = std::max<uint32_t>(rings, 1u);
    m.verts.reserve((size_t)slices * rings * 4);
    m.index.reserve((size_t)slices * rings * 12);

    for (uint32_t r = 0; r < rings; ++r) {
        float v0 = (float)r / (float)rings;
        float v1 = (float)(r + 1) / (float)rings;
        float z0 = v0;                       // unit cone: z in [0,1]
        float z1 = v1;
        float rad0 = r0 + (r1 - r0) * v0;    // linear taper emitter->capture
        float rad1 = r0 + (r1 - r0) * v1;
        for (uint32_t f = 0; f < slices; ++f) {
            float a0 = (float)f / (float)slices * kTau;
            float a1 = (float)(f + 1) / (float)slices * kTau;
            float ca0 = std::cos(a0), sa0 = std::sin(a0);
            float ca1 = std::cos(a1), sa1 = std::sin(a1);
            float p00[3] = { ca0 * rad0, sa0 * rad0, z0 };
            float p10[3] = { ca1 * rad0, sa1 * rad0, z0 };
            float p11[3] = { ca1 * rad1, sa1 * rad1, z1 };
            float p01[3] = { ca0 * rad1, sa0 * rad1, z1 };
            // Outward radial normal (z component left at 0; the beam is additive
            // emissive so exact shading normals barely matter).
            float am = 0.5f * (a0 + a1);
            float nx = std::cos(am), ny = std::sin(am), nz = 0.0f;
            float u0 = (float)f / (float)slices;
            float u1 = (float)(f + 1) / (float)slices;
            auto push = [&](const float* p, float uu, float vv) {
                rhi::MeshVertex vx{};
                vx.pos[0] = p[0]; vx.pos[1] = p[1]; vx.pos[2] = p[2];
                vx.normal[0] = nx; vx.normal[1] = ny; vx.normal[2] = nz;
                vx.uv[0] = uu; vx.uv[1] = vv;
                m.verts.push_back(vx);
            };
            uint32_t base = (uint32_t)m.verts.size();
            push(p00, u0, v0);  // 0
            push(p10, u1, v0);  // 1
            push(p11, u1, v1);  // 2
            push(p01, u0, v1);  // 3
            // Both windings (like the wormhole tube) so the cone reads from inside
            // the beam, outside, AND looking straight down the axis.
            m.index.push_back(base + 0); m.index.push_back(base + 1); m.index.push_back(base + 2);
            m.index.push_back(base + 0); m.index.push_back(base + 2); m.index.push_back(base + 3);
            m.index.push_back(base + 0); m.index.push_back(base + 2); m.index.push_back(base + 1);
            m.index.push_back(base + 0); m.index.push_back(base + 3); m.index.push_back(base + 2);
        }
    }
    return m;
}

// Energy brightness/color at sAlong (0 emitter -> 1 capture). Returns linear RGB.
// Concentric RINGS scroll along the axis; the leading (capture) end is the
// brightest so the beam reads as pulling INTO the ship. Shared by bake + test.
static void evalEnergy(float sAlong, const TractorBeam::Tuning& t,
                       float& outR, float& outG, float& outB) {
    float s = std::clamp(sAlong, 0.0f, 1.0f);
    // Concentric rings along the axis -> bright bands the runtime pulse scrolls.
    float ringP = 0.5f + 0.5f * std::sin(s * kTau * t.ringDensity);
    float ring  = std::pow(ringP, 4.0f);           // crisp ring crests
    // A steady glow floor along the whole beam so it never reads as gaps.
    float floorGlow = 0.22f;
    // The CAPTURE end (s->1) is hotter (energy converging into the hold).
    float lead = std::pow(s, 1.5f);
    float core = floorGlow + 0.85f * ring + 0.5f * lead;
    // Compose: teal EDGE color as the base wash, cyan/white CORE on the ring
    // crests + the leading capture end.
    float r = t.edgeColor[0] * floorGlow + t.coreColor[0] * (core - floorGlow);
    float g = t.edgeColor[1] * floorGlow + t.coreColor[1] * (core - floorGlow);
    float b = t.edgeColor[2] * floorGlow + t.coreColor[2] * (core - floorGlow);
    outR = r; outG = g; outB = b;
}

// Bake the energy texture. U = angle around the cone (kept near-uniform), V =
// distance along the axis (carries the rings + leading-end brightening). An EDGE
// FALLOFF on U softens the silhouette so the cone reads as a beam, not a tube.
static std::vector<uint8_t> bakeEnergyRGBA(uint32_t w, uint32_t h,
                                           const TractorBeam::Tuning& t) {
    std::vector<uint8_t> px((size_t)w * h * 4, 0);
    for (uint32_t y = 0; y < h; ++y) {
        float sAlong = (y + 0.5f) / (float)h;
        float r, g, b;
        evalEnergy(sAlong, t, r, g, b);
        for (uint32_t x = 0; x < w; ++x) {
            // Slight azimuthal shimmer so the cone surface isn't dead-flat.
            float u = (x + 0.5f) / (float)w;
            float shimmer = 0.85f + 0.15f * std::sin(u * kTau * 3.0f + sAlong * 5.0f);
            auto toU8 = [](float c) {
                c = std::clamp(c, 0.0f, 1.0f);
                return (uint8_t)std::min(255, (int)std::lround(c * 255.0f));
            };
            uint8_t* p = &px[((size_t)y * w + x) * 4];
            p[0] = toU8(r * shimmer);
            p[1] = toU8(g * shimmer);
            p[2] = toU8(b * shimmer);
            p[3] = 255;
        }
    }
    return px;
}

} // namespace

// ---------------------------------------------------------------------------
// Tuning clamp (public utility; tests assert this).
// ---------------------------------------------------------------------------
TractorBeam::Tuning clampTuning(const TractorBeam::Tuning& in) {
    TractorBeam::Tuning t = in;
    if (!(t.emitterRadius >= 0.0f)) t.emitterRadius = 0.0f;
    if (!(t.captureRadius  > 0.0f)) t.captureRadius = 0.1f;
    if (!(t.ringDensity   >= 1.0f)) t.ringDensity   = 1.0f;
    if (!(t.flowSpeed     >= 0.0f)) t.flowSpeed     = 0.0f;
    return t;
}

// ---------------------------------------------------------------------------
// CPU reference for the test: luminance of the baked energy at sAlong.
// ---------------------------------------------------------------------------
float TractorBeam::sampleEnergyBrightness(float sAlong, const Tuning& t) {
    Tuning c = clampTuning(t);
    float r, g, b;
    evalEnergy(sAlong, c, r, g, b);
    return 0.299f * r + 0.587f * g + 0.114f * b;
}

// ---------------------------------------------------------------------------
// init / shutdown
// ---------------------------------------------------------------------------
void TractorBeam::init(rhi::IRenderDevice& dev, const Tuning& t) {
    if (m_initialized) return;
    const Tuning c = clampTuning(t);

    // Unit cone (z in [0,1]); re-oriented + stretched onto the live from->to axis
    // each frame. 48 slices around + 64 rings along keep the rings smooth + the
    // funnel taper clean over a long beam.
    ConeMesh cone = buildCone(c.emitterRadius, c.captureRadius, /*slices=*/48, /*rings=*/64);
    m_mesh = dev.createMesh(cone.verts.data(), (uint32_t)cone.verts.size(),
                            cone.index.data(), (uint32_t)cone.index.size());

    // Bake the energy texture. 256 (U, around) x 1024 (V, along axis): the axis
    // carries the ring/lead detail so it gets the higher res. Linear storage
    // (srgb=false) -- the emissive draw multiplies these texels by the HDR term.
    std::vector<uint8_t> bake = bakeEnergyRGBA(/*w=*/256, /*h=*/1024, c);
    m_tex = dev.createTexture(bake.data(), 256, 1024, /*srgb=*/false);

    m_initialized = m_mesh.valid() && m_tex.valid();
    m_lastTuning  = c;
    if (!m_initialized) {
        x3::logError("TractorBeam::init: mesh or texture creation failed");
    }
}

void TractorBeam::shutdown(rhi::IRenderDevice& dev) {
    if (m_mesh.valid()) { dev.destroyMesh(m_mesh); m_mesh = rhi::MeshHandle{}; }
    if (m_tex.valid())  { dev.destroyTexture(m_tex); m_tex = rhi::TextureHandle{}; }
    m_initialized = false;
    m_lastStrength = 0.0f;
    m_lastDrawn = false;
}

// ---------------------------------------------------------------------------
// render
// ---------------------------------------------------------------------------
void TractorBeam::render(rhi::IRenderDevice& dev, const rhi::FrameContext& fr,
                         const float* viewProj16, const float from[3], const float to[3],
                         float intensity, float timeSec, const Tuning& t) {
    (void)viewProj16;   // shader-path would use it for the per-pixel ray
    m_lastDrawn = false;
    m_lastStrength = 0.0f;
    if (!m_initialized || !from || !to) return;

    const Tuning c = clampTuning(t);
    m_lastTuning = c;
    intensity = std::clamp(intensity, 0.0f, 1.0f);
    m_lastIntensity = intensity;

    // ---- Build the from->to basis. The unit cone's +Z axis must map onto the
    // (to - from) direction; the cone is stretched to the beam length. -----------
    float ax = to[0] - from[0];
    float ay = to[1] - from[1];
    float az = to[2] - from[2];
    float len = std::sqrt(ax*ax + ay*ay + az*az);
    // DEGENERATE GUARD: from==to (or a vanishing beam) -> skip the draw entirely.
    // No NaN, no zero-volume transform handed to the GPU.
    if (!(len > 1e-4f)) return;
    float zx = ax / len, zy = ay / len, zz = az / len;   // beam forward (+Z maps here)

    // A perpendicular X basis: cross(forward, world-up); fall back to world-X if
    // the beam is (anti)parallel to up (avoid a zero cross product -> NaN basis).
    float upx = 0.0f, upy = 1.0f, upz = 0.0f;
    if (std::fabs(zy) > 0.999f) { upx = 1.0f; upy = 0.0f; upz = 0.0f; }
    // X = normalize(cross(up, forward))
    float xx = upy * zz - upz * zy;
    float xy = upz * zx - upx * zz;
    float xz = upx * zy - upy * zx;
    float xl = std::sqrt(xx*xx + xy*xy + xz*xz);
    if (!(xl > 1e-5f)) { xx = 1.0f; xy = 0.0f; xz = 0.0f; xl = 1.0f; }
    xx /= xl; xy /= xl; xz /= xl;
    // Y = cross(forward, X) (already unit; forward & X are orthonormal).
    float yx = zy * xz - zz * xy;
    float yy = zz * xx - zx * xz;
    float yz = zx * xy - zy * xx;

    // Model matrix (column-major). Columns 0/1 keep the cone's cross-section at its
    // authored radii (no XY scale); column 2 is the forward axis scaled by `len`
    // (stretch the unit cone to the beam length); column 3 translates the apex to
    // `from`. Result: apex@from, base@to, radii = the Tuning radii.
    const float m[16] = {
        xx,           xy,           xz,           0.0f,   // X basis (col 0)
        yx,           yy,           yz,           0.0f,   // Y basis (col 1)
        zx * len,     zy * len,     zz * len,     0.0f,   // forward * length (col 2)
        from[0],      from[1],      from[2],      1.0f    // translation (col 3)
    };

    // ENERGY-FLOW PULSE: a scrolling sine on the baseColor multiplier so the baked
    // rings read as flowing emitter->capture (the "pull"). flowSpeed sets the rate;
    // it stays positive so the beam never blinks fully off.
    const float flow = 1.0f + 0.30f * std::sin(timeSec * c.flowSpeed * kTau);
    // Beam strength: intensity (lock-on ramp) raises the overall brightness into
    // the HDR bloom range. A small floor at intensity 0 keeps a faint targeting
    // glow; intensity 1 is the full lock-on. Like sky_stars/wormhole, brightness is
    // carried by the per-pixel texture * this baseColor multiplier (NOT a uniform
    // per-object emissive, which would flat-fill the cone + wash the silhouette).
    const float kBaseGlow = 2.4f;
    const float strength = kBaseGlow * (0.15f + 0.85f * intensity) * flow;
    m_lastStrength = strength;

    // baseColorFactor: the per-pixel energy texel is MULTIPLIED by this -- bright
    // rings/leading-end land at ~strength (HDR -> bloom) while the dim wash stays
    // low, so the beam reads as a soft cyan cone with flowing rings (not a solid
    // tube). A teal bias keeps the tractor look.
    const float baseFactor[4] = {
        strength * 0.85f, strength, strength * 1.05f, 1.0f
    };
    // Per-object EMISSIVE ZERO (same rationale as wormhole/sky_stars): the
    // per-pixel texture is the sole brightness source; a uniform emissive would
    // flat-fill every facet + bloom the whole cone white.
    const float emissive[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    dev.drawMeshEmissive(fr, m_mesh, m_tex, baseFactor, emissive, m);
    m_lastDrawn = true;
}

} // namespace x3::space
