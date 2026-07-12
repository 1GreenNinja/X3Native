// Salvari crystal-matrix wormhole VFX -- see app/space/wormhole_vfx.h.
//
// Implementation notes:
//   - Like app/sky_stars.cpp, the "shader-path" version of this effect would
//     create a custom fragment pipeline (shaders/wormhole.{vert,frag}) and march
//     a tunnel per pixel. The RHI surface (engine/rhi/*) intentionally hides
//     pipeline creation behind drawMesh*/drawMeshGlass and is OFF-LIMITS this
//     lane. So this uses the EXISTING emissive-draw path with the crystal-matrix
//     pattern baked into a CPU texture wrapped around a faceted tube mesh. The
//     shader source files (shaders/wormhole.{vert,frag}) live as the
//     SOURCE-OF-TRUTH for the per-pixel formula + are compiled to SPIR-V as part
//     of the build (validated by glslc); when an RHI lane lands custom-pipeline
//     plumbing the init/render path can switch to consuming the SPIR-V without
//     changing this header.
//
//   - GEOMETRY: an inside-out tube of `rings` axial segments x `facets` radial
//     segments. Each radial segment is a FLAT crystal pane: the four corners of a
//     facet quad share one outward-then-inverted normal, so the lit result reads
//     as discrete crystal facets rather than a smooth cylinder. Winding is
//     duplicated (both orientations) so the inside surface survives BACK_BIT cull
//     exactly like the skydome does.
//
//   - TEXTURE: a baked RGBA8 wrapped so U = angle-around-the-ring, V = distance
//     along the axis. Each texel blends the blue WALL color, PURPLE accent glints
//     at facet seams, and a WHITE-HOT band near the far (convergence) end. Energy
//     STREAKS are baked as bright axial ridges; the per-frame emissive PULSE
//     scrolls them so light reads as racing along the facets toward the camera.
//
//   - ANIMATION: the texture is baked once (static). The flow illusion comes from
//     (a) the camera advancing down the axis (the showcase does this) and (b) a
//     per-frame emissive core PULSE in render() driven by timeSec; `progress`
//     (0..1) raises the white-hot core + convergence so the tunnel blooms out as
//     the jump completes.

#include "wormhole_vfx.h"

#include "engine/core/x3_log.h"
#include "../headless_device.h"   // HeadlessRenderDevice for the self-test

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace x3::space {

namespace {

constexpr float kPi  = 3.14159265358979f;
constexpr float kTau = 6.28318530717959f;

static inline float fract1(float x) { return x - std::floor(x); }

// Cheap 1D->[0,1) hash for per-facet color jitter (prismatic variance).
static float hash1(float x) {
    float p = fract1(x * 0.1031f);
    p *= p + 33.33f;
    p *= p + p;
    return fract1(p);
}

// A faceted, inside-out tube mesh. The tube runs along +Z from z=0 to z=length,
// centered on X/Y. `facets` panes around the ring, `rings` segments along Z.
// Per-FACET flat normals (all 4 corners of a pane share the pane's inward normal)
// give the crystalline read. UV: u = angle [0,1] around the ring, v = z [0,1].
struct TubeMesh {
    std::vector<rhi::MeshVertex> verts;
    std::vector<uint32_t>        index;
};

static TubeMesh buildTube(float radius, float length, uint32_t facets, uint32_t rings) {
    TubeMesh m;
    facets = std::max<uint32_t>(facets, 3u);
    rings  = std::max<uint32_t>(rings, 2u);
    // We emit 4 UNSHARED vertices per (ring, facet) quad so each pane carries its
    // own flat normal + crisp UV seam -> true faceted crystal shading.
    m.verts.reserve((size_t)facets * rings * 4);
    m.index.reserve((size_t)facets * rings * 12);

    for (uint32_t r = 0; r < rings; ++r) {
        float v0 = (float)r / (float)rings;
        float v1 = (float)(r + 1) / (float)rings;
        float z0 = v0 * length;
        float z1 = v1 * length;
        for (uint32_t f = 0; f < facets; ++f) {
            float a0 = (float)f / (float)facets * kTau;
            float a1 = (float)(f + 1) / (float)facets * kTau;
            float ca0 = std::cos(a0), sa0 = std::sin(a0);
            float ca1 = std::cos(a1), sa1 = std::sin(a1);
            // The 4 corners of this crystal pane.
            float p00[3] = { ca0 * radius, sa0 * radius, z0 };
            float p10[3] = { ca1 * radius, sa1 * radius, z0 };
            float p11[3] = { ca1 * radius, sa1 * radius, z1 };
            float p01[3] = { ca0 * radius, sa0 * radius, z1 };
            // Flat INWARD normal = -(midpoint-direction) of the pane (it faces the
            // camera flying down the axis). Radial only (z component 0).
            float am = 0.5f * (a0 + a1);
            float nx = -std::cos(am), ny = -std::sin(am), nz = 0.0f;
            float uMid = (float)(f) / (float)facets;
            float uMid1 = (float)(f + 1) / (float)facets;
            auto push = [&](const float* p, float uu, float vv) {
                rhi::MeshVertex vx{};
                vx.pos[0] = p[0]; vx.pos[1] = p[1]; vx.pos[2] = p[2];
                vx.normal[0] = nx; vx.normal[1] = ny; vx.normal[2] = nz;
                vx.uv[0] = uu; vx.uv[1] = vv;
                m.verts.push_back(vx);
            };
            uint32_t base = (uint32_t)m.verts.size();
            push(p00, uMid,  v0);  // 0
            push(p10, uMid1, v0);  // 1
            push(p11, uMid1, v1);  // 2
            push(p01, uMid,  v1);  // 3
            // Inside-out: emit BOTH windings (like the skydome) so the inner face
            // survives BACK_BIT cull regardless of which way the pipeline culls.
            // Tri A: 0,1,2 ; Tri B: 0,2,3  (and their reverses).
            m.index.push_back(base + 0); m.index.push_back(base + 1); m.index.push_back(base + 2);
            m.index.push_back(base + 0); m.index.push_back(base + 2); m.index.push_back(base + 3);
            m.index.push_back(base + 0); m.index.push_back(base + 2); m.index.push_back(base + 1);
            m.index.push_back(base + 0); m.index.push_back(base + 3); m.index.push_back(base + 2);
        }
    }
    return m;
}

// Crystal-matrix brightness/color at (theta around ring, zNorm along axis).
// Returns linear RGB (HDR-ish). Shared by the bake and the test reference.
static void evalCrystal(float theta, float zNorm, const WormholeVfx::Tuning& t,
                        float& outR, float& outG, float& outB) {
    // Facet index around the ring -> seam proximity drives PURPLE prismatic glints.
    float facetF = theta / kTau * t.facetDensity;     // 0..facetDensity
    float seam   = std::fabs(fract1(facetF) - 0.5f) * 2.0f; // 0 at seam center, 1 at edges
    // Glint sharpens at the facet EDGES (seam ~1): a bright prismatic line.
    float glint  = std::pow(seam, 6.0f);
    // Per-facet prismatic color jitter so facets don't read uniform.
    float fi     = std::floor(facetF);
    float jit    = hash1(fi * 1.37f);

    // Energy STREAKS along the axis: bright axial ridges (baked; scrolled by the
    // emissive pulse at runtime). A few sine harmonics keep it crystalline-busy.
    float streak = 0.5f + 0.5f * std::sin(zNorm * kTau * 6.0f + facetF * 0.7f);
    streak = std::pow(streak, 3.0f);

    // WHITE-HOT convergence: the far end (zNorm -> 1) gets hotter. The bake stores
    // a moderate gradient; render() raises it with `progress`.
    float conv = std::pow(std::clamp(zNorm, 0.0f, 1.0f), 2.5f);

    // Compose: blue WALL base + purple ACCENT at glints + white-hot CORE at the
    // convergence end and along the brightest streaks. The wall keeps a modest
    // floor (so the near tube still reads as dim blue crystal) but the streaks +
    // glints carry the contrast so the multiply-by-HDR-baseColor path blooms the
    // BRIGHT facets without washing the whole tube to white.
    float wall = 0.18f + 0.55f * streak;             // streak modulates wall brightness
    float r = t.wallColor[0] * wall;
    float g = t.wallColor[1] * wall;
    float b = t.wallColor[2] * wall;
    // Purple prismatic glints (with per-facet jitter).
    float gA = glint * (0.6f + 0.4f * jit);
    r += t.accentColor[0] * gA;
    g += t.accentColor[1] * gA;
    b += t.accentColor[2] * gA;
    // White-hot core blended in at the convergence end + on the hottest streaks.
    float core = conv * (0.4f + 0.6f * streak);
    r += t.coreColor[0] * core;
    g += t.coreColor[1] * core;
    b += t.coreColor[2] * core;
    outR = r; outG = g; outB = b;
}

// Bake the crystal-matrix texture. U = angle around the ring, V = distance along
// the axis. Stored LINEAR (srgb=false) -- the emissive draw multiplies these
// texels by the per-object HDR term.
static std::vector<uint8_t> bakeCrystalRGBA(uint32_t w, uint32_t h,
                                            const WormholeVfx::Tuning& t) {
    std::vector<uint8_t> px((size_t)w * h * 4, 0);
    for (uint32_t y = 0; y < h; ++y) {
        float zNorm = (y + 0.5f) / (float)h;          // along axis
        for (uint32_t x = 0; x < w; ++x) {
            float theta = (x + 0.5f) / (float)w * kTau; // around ring
            float r, g, b;
            evalCrystal(theta, zNorm, t, r, g, b);
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
WormholeVfx::Tuning clampTuning(const WormholeVfx::Tuning& in) {
    WormholeVfx::Tuning t = in;
    if (!(t.length > 0.0f))       t.length       = 1.0f;
    if (!(t.radius > 0.0f))       t.radius        = 0.1f;
    if (!(t.flowSpeed >= 0.0f))   t.flowSpeed     = 0.0f;
    if (!(t.facetDensity >= 3.0f))t.facetDensity  = 3.0f;
    return t;
}

// ---------------------------------------------------------------------------
// CPU reference for the test: brightness (luminance-ish) of the baked crystal.
// ---------------------------------------------------------------------------
float WormholeVfx::sampleFacetBrightness(float theta, float zNorm, const Tuning& t) {
    Tuning c = clampTuning(t);
    float r, g, b;
    evalCrystal(theta, zNorm, c, r, g, b);
    return 0.299f * r + 0.587f * g + 0.114f * b;
}

// ---------------------------------------------------------------------------
// init / shutdown / setOrigin
// ---------------------------------------------------------------------------
void WormholeVfx::init(rhi::IRenderDevice& dev, const Tuning& t) {
    if (m_initialized) return;
    const Tuning c = clampTuning(t);

    // Faceted tube. `facetDensity` panes around the ring (the crystalline read);
    // rings along the axis tessellate the length so the camera always has facets
    // immediately ahead. 64 axial rings over a long tunnel keeps the streaks crisp.
    const uint32_t facets = std::max<uint32_t>(3u, (uint32_t)std::lround(c.facetDensity));
    const uint32_t rings  = 64;
    TubeMesh tube = buildTube(c.radius, c.length, facets, rings);
    m_mesh = dev.createMesh(tube.verts.data(), (uint32_t)tube.verts.size(),
                            tube.index.data(), (uint32_t)tube.index.size());

    // Bake the crystal-matrix texture. 512 (U, around) x 1024 (V, along axis):
    // the axis carries the streak/convergence detail so it gets the higher res.
    std::vector<uint8_t> bake = bakeCrystalRGBA(/*w=*/512, /*h=*/1024, c);
    m_tex = dev.createTexture(bake.data(), 512, 1024, /*srgb=*/false);

    m_initialized = m_mesh.valid() && m_tex.valid();
    m_lastTuning  = c;
    if (!m_initialized) {
        x3::logError("WormholeVfx::init: mesh or texture creation failed");
    }
}

void WormholeVfx::setOrigin(float ox, float oy, float oz) {
    m_ox = ox; m_oy = oy; m_oz = oz;
}

void WormholeVfx::shutdown(rhi::IRenderDevice& dev) {
    if (m_mesh.valid()) { dev.destroyMesh(m_mesh); m_mesh = rhi::MeshHandle{}; }
    if (m_tex.valid())  { dev.destroyTexture(m_tex); m_tex = rhi::TextureHandle{}; }
    m_initialized = false;
}

// ---------------------------------------------------------------------------
// render
// ---------------------------------------------------------------------------
void WormholeVfx::render(rhi::IRenderDevice& dev, const rhi::FrameContext& fr,
                         const float* viewProj16, float timeSec, float progress,
                         const Tuning& t) {
    (void)viewProj16;   // shader-path would use it for the per-pixel ray
    if (!m_initialized) return;

    const Tuning c = clampTuning(t);
    m_lastTuning = c;
    progress = std::clamp(progress, 0.0f, 1.0f);
    m_lastProgress = progress;

    // ENERGY-FLOW PULSE: a scrolling sine on the baseColor multiplier so the baked
    // streaks read as racing toward the camera. flowSpeed sets the rate. The pulse
    // stays positive so the tunnel never blinks off.
    const float flow  = 1.0f + 0.25f * std::sin(timeSec * c.flowSpeed);
    // CORE/CONVERGENCE strength: progress raises the OVERALL crystal brightness so
    // the white-hot streaks/convergence in the BAKED texture bloom out as the jump
    // completes. Like sky_stars, brightness is carried by the per-pixel texture *
    // this baseColor multiplier (NOT a uniform per-object emissive, which would
    // add a flat field across every facet and bloom the whole frame to white).
    // kBaseGlow lifts the baked crystal (linear ~0..1.5) into the HDR bloom range;
    // progress sharpens it toward white-hot convergence.
    const float kBaseGlow = 2.2f;
    const float coreStrength = kBaseGlow * (1.0f + 1.4f * progress * progress);
    m_lastCore = coreStrength;
    const float strength = coreStrength * flow;

    // Model: translate the tunnel mouth to the placed origin (no rotation -- the
    // tube is authored along +Z; S3 can pre-rotate by composing into setOrigin's
    // world if needed, but the showcase flies straight down +Z).
    const float m[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        m_ox, m_oy, m_oz, 1.0f
    };
    // baseColorFactor: the per-pixel crystal texel is MULTIPLIED by this -- bright
    // streaks/glints/convergence land at ~strength (HDR -> bloom) while the dark
    // facet valleys stay near-black, so the tunnel reads as faceted crystal with
    // contrast (not a flat bloomed field). A slight blue bias keeps it Salvari.
    const float baseFactor[4] = {
        strength, strength, strength * 1.08f, 1.0f
    };
    // Per-object EMISSIVE ZERO: a uniform emissive would add a flat glow to EVERY
    // facet (mesh.frag adds it independent of the texture), washing out the crystal
    // contrast + blooming the whole frame white. The per-pixel texture is the sole
    // brightness source; albedo*lighting + the HDR baseColor carry the glow.
    const float emissive[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    dev.drawMeshEmissive(fr, m_mesh, m_tex, baseFactor, emissive, m);
}

// ---------------------------------------------------------------------------
// --test-wormhole: Salvari crystal-matrix wormhole VFX self-test (Act-3 jump
// transition). Headless -- exercises the WormholeVfx init/render/shutdown
// lifecycle (leak-clean round-trip), a VUID-safe render() with a sample
// viewProj, Tuning param clamping, progress 0..1 handling, and that the baked
// crystal-matrix pattern is non-trivial + faceted. Ported byte-faithfully from
// the pre-split monolith main() inline block into this lane TU (integration
// feast fold of the 14900K's feat/wormhole-transit lane).
// ---------------------------------------------------------------------------
bool runWormholeSelfTest() {
    int pass = 0, total = 0;
    auto check = [&](bool c, const char* name) {
        ++total;
        if (c) { ++pass; x3::logInfo(std::string("  [ok] ") + name); }
        else   {          x3::logError(std::string("  [FAIL] ") + name); }
    };

    x3::game::HeadlessRenderDevice hdev;
    // T1: init + shutdown lifecycle is leak-clean (valid handles in, released
    // on shutdown, and a second init() succeeds -- no leaked resources).
    {
        WormholeVfx wh;
        wh.init(hdev);
        check(wh.initialized() && wh.mesh().valid() && wh.texture().valid(),
              "T1 init() produces valid mesh + texture, initialized()=true");
        wh.shutdown(hdev);
        check(!wh.initialized() && !wh.mesh().valid() && !wh.texture().valid(),
              "T1b shutdown() releases handles, initialized()=false");
        wh.init(hdev);
        check(wh.initialized(), "T1c re-init after shutdown succeeds (no leak)");
        rhi::MeshHandle before = wh.mesh();
        wh.init(hdev);
        check(wh.mesh().id == before.id, "T1d double-init() is a no-op");
        wh.shutdown(hdev);
        wh.shutdown(hdev);
        check(!wh.initialized(), "T1e double-shutdown() is safe");
    }
    // T2: render() with a sample viewProj runs without crashing (VUID-safe in
    // the headless stub) and updates the core/convergence strength.
    {
        WormholeVfx wh;
        wh.init(hdev);
        rhi::FrameContext fr = hdev.beginFrame();
        const float idM[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        wh.setOrigin(0.0f, 0.0f, 0.0f);
        wh.render(hdev, fr, idM, /*timeSec=*/0.25f, /*progress=*/0.0f);
        check(wh.lastCoreStrength() > 0.0f,
              "T2 render() applies a positive core glow even at progress 0");
        wh.render(hdev, fr, idM, /*timeSec=*/0.25f, /*progress=*/1.0f);
        float fullCore = wh.lastCoreStrength();
        wh.render(hdev, fr, idM, /*timeSec=*/0.25f, /*progress=*/0.0f);
        float zeroCore = wh.lastCoreStrength();
        check(fullCore > zeroCore && fullCore - zeroCore > 1.0f,
              "T2b progress 1.0 intensifies the core vs progress 0.0");
        hdev.endFrame(fr);
        wh.shutdown(hdev);
    }
    // T3: Tuning parameter clamping (length>0, radius>0, flowSpeed>=0,
    // facetDensity>=3) -- the bake/mesh need these to be sane.
    {
        WormholeVfx::Tuning bad;
        bad.length       = -50.0f;
        bad.radius       = -2.0f;
        bad.flowSpeed    = -4.0f;
        bad.facetDensity = 1.0f;
        auto c = clampTuning(bad);
        check(c.length       > 0.0f,  "T3a length clamps to > 0");
        check(c.radius       > 0.0f,  "T3b radius clamps to > 0");
        check(c.flowSpeed    >= 0.0f, "T3c flowSpeed clamps to >= 0");
        check(c.facetDensity >= 3.0f, "T3d facetDensity clamps to >= 3");
        WormholeVfx wh;
        wh.init(hdev, bad);
        check(wh.initialized(), "T3e init() survives an out-of-range Tuning");
        wh.shutdown(hdev);
    }
    // T4: progress is clamped to [0,1] inside render() (S3 may drive it from a
    // sequence that overshoots), and the baked crystal-matrix is non-trivial +
    // faceted (the convergence end is brighter than the mouth; facet seams
    // produce a brightness variance around the ring).
    {
        WormholeVfx wh;
        wh.init(hdev);
        rhi::FrameContext fr = hdev.beginFrame();
        const float idM[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        wh.render(hdev, fr, idM, 0.0f, /*progress=*/5.0f);   // overshoot high
        check(wh.lastProgress() == 1.0f, "T4a progress > 1 clamps to 1.0");
        wh.render(hdev, fr, idM, 0.0f, /*progress=*/-3.0f);  // overshoot low
        check(wh.lastProgress() == 0.0f, "T4b progress < 0 clamps to 0.0");
        hdev.endFrame(fr);
        wh.shutdown(hdev);

        WormholeVfx::Tuning t;
        float bMouth = WormholeVfx::sampleFacetBrightness(0.3f, 0.0f, t);
        float bFar   = WormholeVfx::sampleFacetBrightness(0.3f, 1.0f, t);
        check(bFar > bMouth, "T4c convergence end is brighter than the mouth");
        float lo = 1e9f, hi = -1e9f;
        const int N = 256;
        for (int i = 0; i < N; ++i) {
            float th = (i + 0.5f) / (float)N * 6.2831853f;
            float b = WormholeVfx::sampleFacetBrightness(th, 0.5f, t);
            lo = std::min(lo, b); hi = std::max(hi, b);
        }
        check(hi - lo > 0.05f, "T4d crystal facets produce brightness variance around the ring");
        float a0 = WormholeVfx::sampleFacetBrightness(1.1f, 0.4f, t);
        float a1 = WormholeVfx::sampleFacetBrightness(1.1f, 0.4f, t);
        check(a0 == a1, "T4e crystal sample is deterministic");
    }
    x3::logInfo("wormhole: " + std::to_string(pass) + "/" + std::to_string(total) + " passed");
    std::printf("wormhole: %d/%d passed\n", pass, total);
    std::fflush(stdout);
    return pass == total;
}

} // namespace x3::space
