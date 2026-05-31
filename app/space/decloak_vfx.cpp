// Decloak shimmer VFX -- see app/space/decloak_vfx.h.
//
// Implementation notes:
//   - Like app/space/wormhole_vfx.cpp + app/sky_stars.cpp, the "shader-path"
//     version of this effect would create a custom fragment pipeline
//     (shaders/decloak.{vert,frag}) and trace a per-pixel Fresnel edge-glow +
//     scanline shimmer over the ship silhouette. The RHI surface (engine/rhi/*)
//     intentionally hides pipeline creation behind drawMesh*/drawMeshGlass and
//     is OFF-LIMITS this lane. So this uses the EXISTING translucent GLASS draw
//     path (drawMeshGlass: post-opaque, depth-tested, alpha-blended, no depth
//     write) with the cyan-white shimmer pattern baked into a CPU texture wrapped
//     around a SHELL BOX mesh that hugs the ship bounds. The glass path is used
//     (rather than the opaque emissive path the wormhole uses) precisely so the
//     shimmer shell COMPOSITES OVER the ship the host draws underneath instead of
//     occluding it; the per-object emissive term carries the additive HDR glow.
//
//   - GEOMETRY: a unit-ish box centered on the origin, scaled up slightly
//     (`shellScale`) so it forms a thin "phase shell" / halo just OUTSIDE the
//     hull. Both windings are emitted (like the skydome / wormhole tube) so the
//     shell survives BACK_BIT cull whether the camera is inside or outside it.
//     The box is authored as a UNIT box [-1,1]; the host's modelTransform16
//     scales/places it over the actual ship's local bounds.
//
//   - TEXTURE: a baked RGBA8 carrying horizontal SCANLINES (energy bands sweeping
//     the hull), a value-noise SHIMMER mottling, and a bright EDGE-GLOW ramp near
//     the UV borders (so each box face glows hottest at its silhouette edge --
//     the cyan-white "outline traces the hull" read). Stored LINEAR; the emissive
//     draw multiplies these texels by the per-object HDR strength.
//
//   - ANIMATION: the texture is baked once (static). The shimmer FLOW comes from
//     a per-frame emissive PULSE in render() whose phase scrolls with timeSec, so
//     the scanlines read as sweeping the hull. `progress` (0..1) shapes the
//     overall strength with a WINDOW curve: faint at 0 (cloaked outline), PEAK
//     near the middle of the reveal (intense distorting shimmer), then FADES OUT
//     toward 1 (the solid ship needs no overlay) -- mirrored by revealAlpha()
//     ramping the host's ship-mesh opacity 0->1 underneath.

#include "decloak_vfx.h"

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace x3::space {

namespace {

constexpr float kTau = 6.28318530717959f;

static inline float fract1(float x) { return x - std::floor(x); }

// Cheap 2D->[0,1) value hash for the shimmer mottling.
static float hash2(float x, float y) {
    float p = fract1(x * 0.1031f + y * 0.1973f);
    p *= p + 33.33f;
    p *= p + p;
    return fract1(p);
}

// Smooth value noise (bilinear-lerped hash) for the shimmer cloud.
static float valueNoise(float x, float y) {
    float xi = std::floor(x), yi = std::floor(y);
    float xf = x - xi,        yf = y - yi;
    float a = hash2(xi,        yi);
    float b = hash2(xi + 1.0f, yi);
    float c = hash2(xi,        yi + 1.0f);
    float d = hash2(xi + 1.0f, yi + 1.0f);
    float ux = xf * xf * (3.0f - 2.0f * xf);
    float uy = yf * yf * (3.0f - 2.0f * yf);
    return a * (1 - ux) * (1 - uy) + b * ux * (1 - uy)
         + c * (1 - ux) * uy       + d * ux * uy;
}

// A unit box [-1,1]^3 centered on the origin, both windings emitted so it
// survives BACK_BIT cull from either side. Per-face flat normals; UV maps each
// face to the full [0,1] square (so the baked edge-glow ramp traces every face's
// border == the hull silhouette).
struct BoxMesh {
    std::vector<rhi::MeshVertex> verts;
    std::vector<uint32_t>        index;
};

static BoxMesh buildShellBox(float half) {
    BoxMesh m;
    // 6 faces; each face = 4 unshared verts (own normal + full-square UV).
    struct Face { float n[3]; float c[4][3]; };
    const float h = half;
    const Face faces[6] = {
        // +X
        {{ 1,0,0}, {{ h,-h,-h},{ h, h,-h},{ h, h, h},{ h,-h, h}}},
        // -X
        {{-1,0,0}, {{-h,-h, h},{-h, h, h},{-h, h,-h},{-h,-h,-h}}},
        // +Y
        {{0, 1,0}, {{-h, h,-h},{-h, h, h},{ h, h, h},{ h, h,-h}}},
        // -Y
        {{0,-1,0}, {{-h,-h, h},{-h,-h,-h},{ h,-h,-h},{ h,-h, h}}},
        // +Z
        {{0,0, 1}, {{-h,-h, h},{ h,-h, h},{ h, h, h},{-h, h, h}}},
        // -Z
        {{0,0,-1}, {{ h,-h,-h},{-h,-h,-h},{-h, h,-h},{ h, h,-h}}},
    };
    const float uv[4][2] = { {0,0},{1,0},{1,1},{0,1} };
    for (const Face& f : faces) {
        uint32_t base = (uint32_t)m.verts.size();
        for (int i = 0; i < 4; ++i) {
            rhi::MeshVertex v{};
            v.pos[0] = f.c[i][0]; v.pos[1] = f.c[i][1]; v.pos[2] = f.c[i][2];
            v.normal[0] = f.n[0]; v.normal[1] = f.n[1]; v.normal[2] = f.n[2];
            v.uv[0] = uv[i][0];   v.uv[1] = uv[i][1];
            m.verts.push_back(v);
        }
        // Both windings (drawn from inside OR outside the shell -> always visible).
        m.index.push_back(base+0); m.index.push_back(base+1); m.index.push_back(base+2);
        m.index.push_back(base+0); m.index.push_back(base+2); m.index.push_back(base+3);
        m.index.push_back(base+0); m.index.push_back(base+2); m.index.push_back(base+1);
        m.index.push_back(base+0); m.index.push_back(base+3); m.index.push_back(base+2);
    }
    return m;
}

// Cyan-white shimmer brightness/color at shell UV (u,v in [0,1)). Returns linear
// RGB (HDR-ish). Shared by the bake and the test reference.
static void evalShimmer(float u, float v, const DecloakVfx::Tuning& t,
                        float& outR, float& outG, float& outB) {
    // SCANLINES: bright horizontal energy bands sweeping the hull (sharpened sine).
    float scan = 0.5f + 0.5f * std::sin(v * kTau * t.scanDensity);
    scan = std::pow(scan, 3.0f);
    // SHIMMER mottling: a value-noise cloud so the bands aren't uniform.
    float n = valueNoise(u * 9.0f, v * 9.0f);
    float shimmer = 0.35f + 0.65f * n;
    // EDGE-GLOW: hottest near the UV border (== each box face's silhouette edge),
    // so the overlay traces the hull outline. Distance to the nearest border.
    float edgeDist = std::min(std::min(u, 1.0f - u), std::min(v, 1.0f - v));
    float edge = std::pow(std::clamp(1.0f - edgeDist * 4.0f, 0.0f, 1.0f), 2.0f);

    // Compose: a dim cyan base lifted by the scanline*shimmer, plus a bright
    // white-hot core on the hottest scanline peaks, plus the cyan-white EDGE glow.
    float band = scan * shimmer;
    float r = t.edgeColor[0] * (0.12f + 0.6f * band);
    float g = t.edgeColor[1] * (0.12f + 0.6f * band);
    float b = t.edgeColor[2] * (0.12f + 0.6f * band);
    // White-hot shimmer peaks.
    float core = std::pow(band, 2.0f);
    r += t.coreColor[0] * core * 0.5f;
    g += t.coreColor[1] * core * 0.5f;
    b += t.coreColor[2] * core * 0.5f;
    // Cyan-white edge glow tracing the silhouette.
    r += t.edgeColor[0] * edge;
    g += t.edgeColor[1] * edge;
    b += t.edgeColor[2] * edge;
    outR = r; outG = g; outB = b;
}

// Bake the shimmer texture. U/V span a box face. Stored LINEAR (srgb=false).
static std::vector<uint8_t> bakeShimmerRGBA(uint32_t w, uint32_t h,
                                            const DecloakVfx::Tuning& t) {
    std::vector<uint8_t> px((size_t)w * h * 4, 0);
    for (uint32_t y = 0; y < h; ++y) {
        float v = (y + 0.5f) / (float)h;
        for (uint32_t x = 0; x < w; ++x) {
            float u = (x + 0.5f) / (float)w;
            float r, g, b;
            evalShimmer(u, v, t, r, g, b);
            auto toU8 = [](float c) {
                c = std::clamp(c, 0.0f, 1.0f);
                return (uint8_t)std::min(255, (int)std::lround(c * 255.0f));
            };
            uint8_t* p = &px[((size_t)y * w + x) * 4];
            p[0] = toU8(r); p[1] = toU8(g); p[2] = toU8(b); p[3] = 255;
        }
    }
    return px;
}

} // namespace

// ---------------------------------------------------------------------------
// Tuning clamp (public utility; tests assert this).
// ---------------------------------------------------------------------------
DecloakVfx::Tuning clampTuning(const DecloakVfx::Tuning& in) {
    DecloakVfx::Tuning t = in;
    if (!(t.shellScale  > 0.0f)) t.shellScale   = 0.01f;
    if (!(t.shimmerSpeed>= 0.0f))t.shimmerSpeed  = 0.0f;
    if (!(t.scanDensity >= 1.0f))t.scanDensity   = 1.0f;
    return t;
}

// ---------------------------------------------------------------------------
// revealAlpha: host ship-mesh opacity ramp. Smoothstep 0->1.
// ---------------------------------------------------------------------------
float DecloakVfx::revealAlpha(float progress) {
    float p = std::clamp(progress, 0.0f, 1.0f);
    // smoothstep(0,1,p): exactly 0 at 0, exactly 1 at 1, monotonic, eased.
    return p * p * (3.0f - 2.0f * p);
}

// ---------------------------------------------------------------------------
// CPU reference for the test: brightness (luminance-ish) of the baked shimmer.
// ---------------------------------------------------------------------------
float DecloakVfx::sampleShimmerBrightness(float u, float v, const Tuning& t) {
    Tuning c = clampTuning(t);
    float r, g, b;
    evalShimmer(fract1(u), fract1(v), c, r, g, b);
    return 0.299f * r + 0.587f * g + 0.114f * b;
}

// ---------------------------------------------------------------------------
// init / shutdown
// ---------------------------------------------------------------------------
void DecloakVfx::init(rhi::IRenderDevice& dev, const Tuning& t) {
    if (m_initialized) return;
    const Tuning c = clampTuning(t);

    // The shell is a UNIT box (half-extent = shellScale); the host's model
    // transform scales it onto the real ship bounds.
    BoxMesh box = buildShellBox(c.shellScale);
    m_mesh = dev.createMesh(box.verts.data(), (uint32_t)box.verts.size(),
                            box.index.data(), (uint32_t)box.index.size());

    // Bake the shimmer texture. 512x512 carries the scanline + noise + edge detail.
    std::vector<uint8_t> bake = bakeShimmerRGBA(/*w=*/512, /*h=*/512, c);
    m_tex = dev.createTexture(bake.data(), 512, 512, /*srgb=*/false);

    m_initialized = m_mesh.valid() && m_tex.valid();
    m_lastTuning  = c;
    if (!m_initialized) {
        x3::logError("DecloakVfx::init: mesh or texture creation failed");
    }
}

void DecloakVfx::shutdown(rhi::IRenderDevice& dev) {
    if (m_mesh.valid()) { dev.destroyMesh(m_mesh); m_mesh = rhi::MeshHandle{}; }
    if (m_tex.valid())  { dev.destroyTexture(m_tex); m_tex = rhi::TextureHandle{}; }
    m_initialized = false;
}

// ---------------------------------------------------------------------------
// render
// ---------------------------------------------------------------------------
void DecloakVfx::render(rhi::IRenderDevice& dev, const rhi::FrameContext& fr,
                        const float* viewProj16, const float* modelTransform16,
                        float timeSec, float progress, const Tuning& t) {
    (void)viewProj16;   // shader-path would use it for the per-pixel ray
    if (!m_initialized) return;

    const Tuning c = clampTuning(t);
    m_lastTuning = c;
    progress = std::clamp(progress, 0.0f, 1.0f);
    m_lastProgress = progress;

    // INTENSITY WINDOW: the shimmer is FAINT when fully cloaked (progress 0 ->
    // just a ghost outline), PEAKS through the middle of the reveal (intense
    // distorting energy), then FADES OUT toward fully revealed (progress 1 -> the
    // solid ship needs no overlay). A raised sine window: sin(pi*progress) peaks
    // at 0.5 and is 0 at both ends; we add a small floor so the cloaked outline
    // is still faintly visible at progress 0.
    const float kFloor   = 0.18f;                          // faint cloaked outline
    const float window   = std::sin(progress * 3.14159265f); // 0 at ends, 1 at 0.5
    const float intensity= kFloor + (1.0f - kFloor) * window;
    // SHIMMER SCROLL pulse: scrolls the scanlines so the bands sweep the hull.
    const float pulse    = 0.7f + 0.3f * std::sin(timeSec * c.shimmerSpeed);

    // Lift the baked shimmer (linear ~0..1.5) into the HDR bloom range, scaled by
    // the intensity window + the scroll pulse.
    const float kBaseGlow = 2.6f;
    const float shimmer   = kBaseGlow * intensity * pulse;
    m_lastShimmer = kBaseGlow * intensity;   // record the window-shaped strength (pulse-independent)

    // Model: the host-supplied ship transform (column-major 4x4). If null, sit at
    // the origin at unit scale (fine for tests / a centered showcase).
    const float idM[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    const float* m = modelTransform16 ? modelTransform16 : idM;

    // The shimmer shell must COMPOSITE OVER the ship (which the host draws in the
    // opaque pass underneath), NOT occlude it -- so it is drawn through the GLASS
    // (post-opaque, depth-tested, alpha-blended, NO depth-write) path rather than
    // the opaque emissive path. Low `opacity` keeps the shell see-through (the
    // ship reads through it); the per-object EMISSIVE term carries the cyan-white
    // HDR glow (independent of scene light, so it blooms even in deep space). The
    // per-pixel shimmer texel modulates the body via baseColorFactor; the bright
    // scanline/edge peaks bloom while the dark valleys stay faint -> the overlay
    // reads as an energy shell tracing the hull, not a solid cube.
    const float baseFactor[4] = { shimmer * 0.85f, shimmer * 0.95f, shimmer * 1.1f, 1.0f };
    // Emissive glow: cyan-white, scaled by the window-shaped strength so it fades
    // in then out across the reveal. The texture multiplies into the body color;
    // the emissive is the additive HDR halo that drives the bloom chain.
    const float kEmis = 1.4f;
    const float emissive[4] = {
        c.edgeColor[0] * kEmis * intensity * pulse,
        c.edgeColor[1] * kEmis * intensity * pulse,
        c.edgeColor[2] * kEmis * intensity * pulse,
        1.0f
    };
    // Glass material: very low opacity (mostly transmissive -> the shimmer reads
    // as energy over the ship), polished, no refraction (a clean additive-style
    // overlay). Opacity tracks the intensity window so the shell is faintest at
    // the cloaked/resolved ends and most present mid-reveal.
    rhi::IRenderDevice::GlassMaterial glass{};
    glass.opacity    = 0.12f + 0.28f * intensity;  // see-through; peaks mid-reveal
    glass.refraction = 0.0f;
    glass.roughness  = 0.0f;
    glass.specular   = 0.0f;
    glass.metallic   = 0.0f;
    glass.ior        = 1.0f;
    dev.drawMeshGlass(fr, m_mesh, m_tex, baseFactor, emissive, glass, m);
}

} // namespace x3::space
