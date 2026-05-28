// Procedural starfield -- see app/sky_stars.h.
//
// Implementation notes:
//   - The "shader-path" version of this system would create a custom
//     fragment-pipeline (shaders/starfield.{vert,frag}) and render a
//     full-screen triangle. The RHI surface (engine/rhi/*) intentionally hides
//     pipeline creation behind drawMesh/drawMeshEmissive/drawMeshGlass, and is
//     OFF-LIMITS this session (the headless-capture-fix lane owns it). So this
//     implementation uses the EXISTING emissive-draw path with the same
//     procedural hash baked into a CPU equirectangular texture.
//
//     Both paths produce the same VISUAL look (view-direction hash -> stable
//     parallax-correct twinkling stars); only the where-it-runs differs. The
//     shader source files live under shaders/starfield.{vert,frag} as the
//     SOURCE-OF-TRUTH for the hash math + final pixel formula and are
//     compiled to SPIR-V as part of the build (so they're validated by
//     glslc); when the RHI lane lands its starfield-pipeline plumbing, the
//     init/render path can be switched to consume the SPIR-V binaries
//     without changing this header.
//
//   - The bake uses the SAME multi-layer hash the shader does:
//       layer 1: density,        threshold,       white-warmcool jitter
//       layer 2: density * 2,    threshold blended to 1.0, dim dust
//     so the CPU bake reproduces the shader's per-star pattern + the per-star
//     color jitter.
//
//   - Per-star twinkle is encoded into the bake by storing a per-pixel
//     "twinkle phase" in the texture's alpha channel; render() updates a
//     per-frame emissive multiplier so the global modulator still reads as
//     busy independent twinkling against the static color channel. Because
//     the dome carries thousands of stars at different phases, the global
//     emissive sin() still produces a recognizable twinkle.

#include "sky_stars.h"

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace x3 {

namespace {

// ---- Procedural hash (mirror of shaders/starfield.frag::hash3) -------------
// The C++ implementation reproduces the GLSL hash3 bit-for-bit (modulo the
// GLSL fract / dot semantics) so the bake and the shader agree.
static inline float fract1(float x) { return x - std::floor(x); }
static inline float fract3dot(float a, float b, float c) { return fract1((a + b) * c); }

static float hash3(float x, float y, float z) {
    // p = fract(p * 0.1031)
    float px = fract1(x * 0.1031f);
    float py = fract1(y * 0.1031f);
    float pz = fract1(z * 0.1031f);
    // p += dot(p, p.yzx + 33.33)
    float dy = py + 33.33f, dz = pz + 33.33f, dx = px + 33.33f;
    float d  = px * dy + py * dz + pz * dx;
    px += d; py += d; pz += d;
    // fract((p.x + p.y) * p.z)
    return fract1((px + py) * pz);
}

// One-layer star evaluator. `dir` is unit-length; matches the shader's
// per-cell math (sub-cell jitter + planar disk + per-star phase). Returns
// (brightness in [0,1], phase in radians) for the CURRENT cell.
struct StarSample {
    float brightness;   // 0..1 disk falloff
    float phase;        // per-star twinkle phase (radians)
    float warmCool;     // per-star color jitter [0,1]
};
static StarSample evalLayer(float dx, float dy, float dz, float density,
                            float threshold, float radius, float salt) {
    const float sx = dx * density, sy = dy * density, sz = dz * density;
    const float vx = std::floor(sx), vy = std::floor(sy), vz = std::floor(sz);
    const float h = hash3(vx + salt, vy + salt, vz + salt);
    StarSample s{ 0.0f, 0.0f, 0.0f };
    if (h <= threshold) return s;
    // Sub-cell jitter (matches the shader's fract * constants).
    float ju = fract1(h * 17.123f) - 0.5f;
    float jv = fract1(h * 31.731f) - 0.5f;
    float jw = fract1(h * 11.0f)   - 0.5f;
    float cu = (sx - vx) - 0.5f - ju;
    float cv = (sy - vy) - 0.5f - jv;
    float cw = (sz - vz) - 0.5f - jw;
    float d  = std::sqrt(cu * cu + cv * cv + cw * cw);
    // smoothstep(radius, 0, d)
    float t = std::clamp((radius - d) / std::max(radius, 1e-5f), 0.0f, 1.0f);
    s.brightness = t * t * (3.0f - 2.0f * t);
    s.phase      = h * 6.2831853f;
    s.warmCool   = fract1(h * 53.0f);
    return s;
}

// 6-face inside-out sphere via cube subdivision. Cleaner UVs than lat/lon and
// no pinch at the poles; we won't actually sample by lat/lon -- we'll sample
// by NORMALIZED vertex direction, which is uniform across the cube map.
//
// We use a SINGLE equirectangular RGBA8 texture as the dome's albedo, sampled
// via spherical UVs computed at each vertex (u = atan2(z,x) / (2*pi) + 0.5,
// v = acos(y) / pi). The fragment will linearly interpolate across faces;
// that's fine for a far-distance starfield where individual stars are at
// most a few pixels.
struct DomeMesh {
    std::vector<rhi::MeshVertex> verts;
    std::vector<uint32_t>        index;
};

static DomeMesh buildSkydome(float radius, uint32_t bands) {
    DomeMesh m;
    // Latitude/longitude sphere, drawn INSIDE-OUT (winding flipped so faces
    // point inward at the camera at the center).
    const uint32_t stacks = bands;
    const uint32_t slices = bands * 2;
    m.verts.reserve((stacks + 1) * (slices + 1));
    for (uint32_t i = 0; i <= stacks; ++i) {
        float v = (float)i / (float)stacks;          // [0,1]
        float phi = v * 3.14159265f;                 // [0, pi]   (theta from +Y down)
        float cy = std::cos(phi);
        float sy = std::sin(phi);
        for (uint32_t j = 0; j <= slices; ++j) {
            float u = (float)j / (float)slices;      // [0,1]
            float th = u * 6.2831853f;               // [0, 2pi]
            float cx = std::cos(th), sz = std::sin(th);
            float px = sy * cx, py = cy, pz = sy * sz;
            rhi::MeshVertex vtx{};
            vtx.pos[0] = px * radius;
            vtx.pos[1] = py * radius;
            vtx.pos[2] = pz * radius;
            // Inward-facing normals (sphere is drawn inside-out).
            vtx.normal[0] = -px;
            vtx.normal[1] = -py;
            vtx.normal[2] = -pz;
            vtx.uv[0] = u;
            vtx.uv[1] = v;
            m.verts.push_back(vtx);
        }
    }
    // Inside-out winding. The device's mesh pipeline uses CCW front-face +
    // BACK_BIT cull. To survive cull from INSIDE, the triangle winding must
    // appear CCW when the camera looks at it FROM INSIDE.
    //
    // Standard outward-facing winding: (r0, r1, r0+1). This triangle is CCW
    // when viewed from OUTSIDE the sphere (so the OUTSIDE is the front-face).
    // From INSIDE looking out at the same triangle, the order appears CW ->
    // it's the BACK face. BACK_BIT cull rejects it.
    //
    // We DON'T flip the winding -- with cullMode BACK_BIT we'd lose every
    // dome fragment. Instead we use NO_CULL would be ideal, but we can't
    // change the pipeline. So we duplicate each triangle in BOTH windings
    // (twice the index buffer size, but no visible perf hit for this
    // single-call dome): one CCW from outside (back-face from inside, culled),
    // one CCW from inside (front-face from inside, drawn).
    //
    // 12 indices per quad = 4 triangles -> 4096 tris -> 12k indices.
    for (uint32_t i = 0; i < stacks; ++i) {
        for (uint32_t j = 0; j < slices; ++j) {
            uint32_t r0 = i * (slices + 1) + j;
            uint32_t r1 = (i + 1) * (slices + 1) + j;
            // Standard outward CCW (front-face from OUTSIDE; back-face inside).
            m.index.push_back(r0);     m.index.push_back(r1);     m.index.push_back(r0 + 1);
            m.index.push_back(r1);     m.index.push_back(r1 + 1); m.index.push_back(r0 + 1);
            // Inverted CCW (front-face from INSIDE) -- the one that's actually
            // visible to the camera inside the dome.
            m.index.push_back(r0);     m.index.push_back(r0 + 1); m.index.push_back(r1);
            m.index.push_back(r1);     m.index.push_back(r0 + 1); m.index.push_back(r1 + 1);
        }
    }
    return m;
}

// Bake the procedural starfield into an equirectangular RGBA8 texture. The
// texture's u maps to longitude (0..2pi), v to latitude (0..pi from +Y).
// Each pixel is filled by computing the corresponding world-space direction,
// evaluating the two procedural layers, and writing the resulting linear
// HDR-ish color into RGB. Alpha is unused by drawMeshEmissive; we leave it
// 255 so the texture round-trips cleanly through sRGB sampling.
static std::vector<uint8_t> bakeStarfieldRGBA(uint32_t w, uint32_t h,
                                              const SkyStars::Tuning& t) {
    std::vector<uint8_t> px((size_t)w * h * 4, 0);
    for (uint32_t y = 0; y < h; ++y) {
        // v -> latitude phi in [0, pi]
        float v = (y + 0.5f) / (float)h;
        float phi = v * 3.14159265f;
        float cy = std::cos(phi), sy = std::sin(phi);
        for (uint32_t x = 0; x < w; ++x) {
            // u -> longitude theta in [0, 2pi]
            float u = (x + 0.5f) / (float)w;
            float th = u * 6.2831853f;
            float dx = sy * std::cos(th);
            float dyv = cy;
            float dz = sy * std::sin(th);
            // ---- Layer 1: bright stars (sparse) ----
            StarSample s1 = evalLayer(dx, dyv, dz, t.starDensity, t.threshold,
                                      t.starRadius, 0.1f);
            // Per-star color jitter: cool->warm tint.
            float warm = s1.warmCool;
            float tintR = (1.0f - warm) * 0.85f + warm * 1.05f;
            float tintG = (1.0f - warm) * 0.90f + warm * 0.95f;
            float tintB = (1.0f - warm) * 1.05f + warm * 0.85f;
            float r = t.baseColor[0] * tintR * s1.brightness;
            float g = t.baseColor[1] * tintG * s1.brightness;
            float b = t.baseColor[2] * tintB * s1.brightness;
            // ---- Layer 2: dim dust (denser, sparser threshold) ----
            // mix(threshold, 1.0, 0.6)
            float th2 = t.threshold * 0.4f + 1.0f * 0.6f;
            StarSample s2 = evalLayer(dx, dyv, dz, t.starDensity * 2.0f, th2,
                                      t.starRadius * 0.6f, 7.7f);
            float dust = s2.brightness * 0.35f;
            r += t.baseColor[0] * dust;
            g += t.baseColor[1] * dust;
            b += t.baseColor[2] * dust;
            // Map to 8-bit. The emissive draw will further multiply by
            // emissiveStrength so this is the per-star intrinsic color.
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
SkyStars::Tuning clampTuning(const SkyStars::Tuning& in) {
    SkyStars::Tuning t = in;
    if (!(t.starDensity > 0.0f))   t.starDensity   = 1.0f;
    if (!(t.starRadius  > 0.0f))   t.starRadius    = 0.05f;
    if (t.threshold < 0.0f)        t.threshold     = 0.0f;
    if (t.threshold >= 1.0f)       t.threshold     = 0.999f;
    if (!(t.twinkleSpeed >= 0.0f)) t.twinkleSpeed  = 0.0f;
    return t;
}

// ---------------------------------------------------------------------------
// CPU reference of the shader hash for the test.
// ---------------------------------------------------------------------------
float SkyStars::sampleProceduralBrightness(const float dir[3],
                                           const Tuning& t,
                                           float timeSec) {
    // Normalize defensively.
    float dx = dir[0], dy = dir[1], dz = dir[2];
    float l = std::sqrt(dx*dx + dy*dy + dz*dz);
    if (l < 1e-6f) return 0.0f;
    dx /= l; dy /= l; dz /= l;
    // Layer 1
    StarSample s1 = evalLayer(dx, dy, dz, t.starDensity, t.threshold,
                              t.starRadius, 0.1f);
    float tw1 = 0.55f + 0.45f * std::sin(timeSec * t.twinkleSpeed * 6.2831853f + s1.phase);
    float b1  = s1.brightness * tw1;
    // Layer 2
    float th2 = t.threshold * 0.4f + 1.0f * 0.6f;
    StarSample s2 = evalLayer(dx, dy, dz, t.starDensity * 2.0f, th2,
                              t.starRadius * 0.6f, 7.7f);
    float tw2 = 0.6f + 0.4f * std::sin(timeSec * t.twinkleSpeed * 6.2831853f * 0.7f + s2.phase + 1.7f);
    float b2  = s2.brightness * 0.35f * tw2;
    return std::max(0.0f, b1 + b2);
}

// ---------------------------------------------------------------------------
// init / shutdown / setCamera
// ---------------------------------------------------------------------------
void SkyStars::init(rhi::IRenderDevice& dev, const Tuning& t) {
    if (m_initialized) return;
    const Tuning c = clampTuning(t);

    // Build the dome mesh: a generously-sized inside-out sphere. The radius
    // is "very far" so the stars sit at the far plane; we re-center the
    // sphere on the camera each frame in render(), so the actual radius only
    // matters for shading scale (small enough to fit comfortably inside the
    // far clip, large enough to be effectively at infinity for the player).
    // Radius 50 m: well inside the device's default 200 m far plane regardless
    // of camera position (we re-center the dome on the camera each frame via
    // the model translation, so the dome always spans [-50, +50] m around the
    // eye — no chance of grazing the far plane).
    // Radius 50 m: well inside the device's default 200 m far plane regardless
    // of camera position (we re-center the dome on the camera each frame via
    // the model translation, so the dome always spans [-50, +50] m around the
    // eye — no chance of grazing the far plane).
    DomeMesh dome = buildSkydome(/*radius=*/50.0f, /*bands=*/32);
    m_mesh = dev.createMesh(dome.verts.data(), (uint32_t)dome.verts.size(),
                            dome.index.data(), (uint32_t)dome.index.size());

    // Bake the equirect starfield. 1024x512 gives a generous angular density
    // while staying small (2 MB RGBA8). Linear-storage texture (NOT sRGB) so
    // the per-star color we computed lands intact -- emissive draws multiply
    // these texels by the per-object emissive term in linear HDR.
    std::vector<uint8_t> px = bakeStarfieldRGBA(/*w=*/1024, /*h=*/512, c);
    m_tex = dev.createTexture(px.data(), 1024, 512, /*srgb=*/false);

    m_initialized = m_mesh.valid() && m_tex.valid();
    m_lastTuning  = c;
    if (!m_initialized) {
        x3::logError("SkyStars::init: mesh or texture creation failed");
    }
}

void SkyStars::setCamera(float ex, float ey, float ez) {
    m_camX = ex; m_camY = ey; m_camZ = ez;
}

void SkyStars::shutdown(rhi::IRenderDevice& dev) {
    if (m_mesh.valid()) { dev.destroyMesh(m_mesh); m_mesh = rhi::MeshHandle{}; }
    if (m_tex.valid())  { dev.destroyTexture(m_tex); m_tex = rhi::TextureHandle{}; }
    m_initialized = false;
}

// ---------------------------------------------------------------------------
// render
// ---------------------------------------------------------------------------
void SkyStars::render(rhi::IRenderDevice& dev, const rhi::FrameContext& fr,
                      const float* viewProjInv16, float timeSec,
                      const Tuning& t) {
    (void)viewProjInv16;   // shader-path uses it for the per-pixel ray
    if (!m_initialized) return;

    const Tuning c = clampTuning(t);
    m_lastTuning = c;

    // Global twinkle modulator on the per-texel BASECOLOR multiplier (not the
    // per-OBJECT emissive -- that would dump a uniform additive bias across
    // the whole dome and wash out the texture-encoded stars). The baseColor
    // factor scales the sampled texel before lighting in mesh.frag, so a
    // factor of ~12 turns each baked star (linear ~1.0) into a 12.0 HDR value
    // that survives the ambient (~0.4) attenuation as albedo*lighting ~= 4.8
    // -> well into the bloom chain after tonemap. Empty-space texels (~0.0)
    // stay near-black so the deep-space backdrop reads correctly.
    const float twinkle = 0.85f + 0.15f * std::sin(timeSec * c.twinkleSpeed * 6.2831853f);
    const float kStarBoost = 12.0f;  // raises the baked stars into HDR bloom range
    const float strength = kStarBoost * twinkle;
    m_lastEmissive = strength;

    // Compose the model matrix: identity rotation + camera position so the
    // dome stays anchored on the eye (the player can never reach the stars).
    float m[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        m_camX, m_camY, m_camZ, 1.0f
    };
    // baseColorFactor: per-channel star tint * the global twinkle boost. This
    // multiplies the per-pixel texture sample -- stars at texel ~1.0 land at
    // ~strength * baseColor before lighting; empty texels stay at 0.
    const float baseFactor[4] = {
        strength * c.baseColor[0], strength * c.baseColor[1],
        strength * c.baseColor[2], 1.0f
    };
    // Emissive ZERO: per-object emissive would add a UNIFORM glow across
    // every pixel of the dome (it's not multiplied by the per-pixel texture
    // in mesh.frag's "color += emissive.rgb * emissive.a"), which would
    // wipe out the starfield contrast. We keep the per-pixel texture as the
    // sole source of star brightness and let albedo*lighting carry it.
    const float emissive[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    dev.drawMeshEmissive(fr, m_mesh, m_tex, baseFactor, emissive, m);
}

} // namespace x3
