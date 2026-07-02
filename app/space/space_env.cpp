// Space environment (S1, Lane C) — see app/space/space_env.h.
//
// Implementation notes:
//   - Like app/sky_stars.*, this uses ONLY the existing drawMesh / drawMeshEmissive
//     path. The RHI surface (engine/rhi/*) intentionally hides pipeline creation,
//     so a deep-space scene is composed entirely from meshes + baked textures:
//       * dome   : inside-out sphere + baked equirect (nebula gradient + stars)
//       * planet : a shared unit UV sphere, transformed per-planet, default
//                  procedural banded/checker material tinted by the albedo
//       * sun    : a camera-facing billboard quad with a baked radial-glow
//                  texture, drawn additively-bright (emissive) along the sun dir
//   - The dome + planet-default + sun-glow textures are baked once on init().
//   - Everything is deterministic + headless-safe (no per-frame heap churn in
//     the hot path beyond the per-planet draw loop).

#include "space_env.h"

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace x3::space {

namespace {

constexpr float kPi  = 3.14159265358979f;
constexpr float kTau = 6.28318530717959f;

// ---- Procedural hash (mirror of shaders/starfield.frag::hash3) --------------
inline float fract1(float x) { return x - std::floor(x); }

float hash3(float x, float y, float z) {
    float px = fract1(x * 0.1031f);
    float py = fract1(y * 0.1031f);
    float pz = fract1(z * 0.1031f);
    float dy = py + 33.33f, dz = pz + 33.33f, dx = px + 33.33f;
    float d  = px * dy + py * dz + pz * dx;
    px += d; py += d; pz += d;
    return fract1((px + py) * pz);
}

// One-layer star evaluator (same scheme as sky_stars.cpp::evalLayer). Returns
// the disk brightness in [0,1] for a unit direction at the given lattice.
float evalStar(float dx, float dy, float dz, float density,
               float threshold, float radius, float salt) {
    const float sx = dx * density, sy = dy * density, sz = dz * density;
    const float vx = std::floor(sx), vy = std::floor(sy), vz = std::floor(sz);
    const float h  = hash3(vx + salt, vy + salt, vz + salt);
    if (h <= threshold) return 0.0f;
    float ju = fract1(h * 17.123f) - 0.5f;
    float jv = fract1(h * 31.731f) - 0.5f;
    float jw = fract1(h * 11.0f)   - 0.5f;
    float cu = (sx - vx) - 0.5f - ju;
    float cv = (sy - vy) - 0.5f - jv;
    float cw = (sz - vz) - 0.5f - jw;
    float d  = std::sqrt(cu * cu + cv * cv + cw * cw);
    float t  = std::clamp((radius - d) / std::max(radius, 1e-5f), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

inline uint8_t toU8(float c) {
    c = std::clamp(c, 0.0f, 1.0f);
    return (uint8_t)std::min(255, (int)std::lround(c * 255.0f));
}

// ---- Inside-out lat/long sphere (dome). Duplicated winding so the inward face
// survives the device's BACK_BIT cull (same trick as sky_stars::buildSkydome).
struct Mesh { std::vector<rhi::MeshVertex> verts; std::vector<uint32_t> index; };

Mesh buildDome(float radius, uint32_t bands) {
    Mesh m;
    const uint32_t stacks = bands, slices = bands * 2;
    m.verts.reserve((stacks + 1) * (slices + 1));
    for (uint32_t i = 0; i <= stacks; ++i) {
        float v = (float)i / (float)stacks;
        float phi = v * kPi;
        float cy = std::cos(phi), sy = std::sin(phi);
        for (uint32_t j = 0; j <= slices; ++j) {
            float u = (float)j / (float)slices;
            float th = u * kTau;
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
            // outward CCW (back-face from inside, culled) ...
            m.index.push_back(r0); m.index.push_back(r1);     m.index.push_back(r0 + 1);
            m.index.push_back(r1); m.index.push_back(r1 + 1); m.index.push_back(r0 + 1);
            // ... and inward CCW (front-face from inside, the visible one).
            m.index.push_back(r0); m.index.push_back(r0 + 1); m.index.push_back(r1);
            m.index.push_back(r1); m.index.push_back(r0 + 1); m.index.push_back(r1 + 1);
        }
    }
    return m;
}

// ---- Outward-facing unit UV sphere (planets). Standard winding (drawn from
// OUTSIDE), so the default BACK_BIT cull keeps the visible front faces.
// bands=24 -> 24*48*2 = ~2304 tris: a "low-poly" planet per the spec.
Mesh buildUvSphere(float radius, uint32_t bands) {
    Mesh m;
    const uint32_t stacks = bands, slices = bands * 2;
    m.verts.reserve((stacks + 1) * (slices + 1));
    for (uint32_t i = 0; i <= stacks; ++i) {
        float v = (float)i / (float)stacks;
        float phi = v * kPi;
        float cy = std::cos(phi), sy = std::sin(phi);
        for (uint32_t j = 0; j <= slices; ++j) {
            float u = (float)j / (float)slices;
            float th = u * kTau;
            float cx = std::cos(th), sz = std::sin(th);
            float px = sy * cx, py = cy, pz = sy * sz;
            rhi::MeshVertex vtx{};
            vtx.pos[0] = px * radius; vtx.pos[1] = py * radius; vtx.pos[2] = pz * radius;
            vtx.normal[0] = px; vtx.normal[1] = py; vtx.normal[2] = pz;  // outward
            vtx.uv[0] = u; vtx.uv[1] = v;
            m.verts.push_back(vtx);
        }
    }
    for (uint32_t i = 0; i < stacks; ++i) {
        for (uint32_t j = 0; j < slices; ++j) {
            uint32_t r0 = i * (slices + 1) + j;
            uint32_t r1 = (i + 1) * (slices + 1) + j;
            m.index.push_back(r0); m.index.push_back(r0 + 1); m.index.push_back(r1);
            m.index.push_back(r1); m.index.push_back(r0 + 1); m.index.push_back(r1 + 1);
        }
    }
    return m;
}

// A single camera-facing unit quad in the XY plane (the sun billboard). render()
// composes a model matrix that orients it toward the camera + scales/places it.
Mesh buildQuad() {
    Mesh m;
    m.verts = {
        {{-1, -1, 0}, {0, 0, 1}, {0, 0}},
        {{ 1, -1, 0}, {0, 0, 1}, {1, 0}},
        {{ 1,  1, 0}, {0, 0, 1}, {1, 1}},
        {{-1,  1, 0}, {0, 0, 1}, {0, 1}},
    };
    m.index = { 0, 1, 2, 0, 2, 3 };
    return m;
}

// ---- Bake the nebula + starfield equirect texture --------------------------
// A dark deep-space gradient (deep blue->violet, slightly brighter toward one
// "galactic band") with a few low-frequency nebula blobs, plus a sparse + a
// dust starfield layer on top. Tuned so the screenshot pixel-variance gate
// (std>15, uniqColors>100) clears easily: many distinct star texels on a
// non-uniform dark gradient.
std::vector<uint8_t> bakeNebulaStars(uint32_t w, uint32_t h) {
    std::vector<uint8_t> px((size_t)w * h * 4, 0);
    for (uint32_t y = 0; y < h; ++y) {
        float v = (y + 0.5f) / (float)h;
        float phi = v * kPi;
        float cy = std::cos(phi), sy = std::sin(phi);
        for (uint32_t x = 0; x < w; ++x) {
            float u = (x + 0.5f) / (float)w;
            float th = u * kTau;
            float dx = sy * std::cos(th), dyv = cy, dz = sy * std::sin(th);

            // ---- Base nebula gradient (linear, kept dim for deep space). ----
            // Vertical band brighter near the "galactic equator" (v~0.5), with a
            // blue->violet tint and a couple of soft low-frequency colour blobs.
            float band = std::exp(-((v - 0.5f) * (v - 0.5f)) / (2.0f * 0.10f * 0.10f));
            float baseR = 0.015f + 0.045f * band;
            float baseG = 0.020f + 0.030f * band;
            float baseB = 0.045f + 0.080f * band;
            // Soft, SMOOTH nebula clouds: trilinearly-interpolated value noise
            // (a couple of octaves) instead of flat voxel cells, so the backdrop
            // reads as drifting gas rather than blocky tiles.
            auto vnoise = [](float x, float y, float z, float salt) {
                float ix = std::floor(x), iy = std::floor(y), iz = std::floor(z);
                float fx = x - ix, fy = y - iy, fz = z - iz;
                // smoothstep the fractional weights.
                auto sm = [](float a){ return a * a * (3.0f - 2.0f * a); };
                fx = sm(fx); fy = sm(fy); fz = sm(fz);
                auto H = [&](float a, float b, float c){ return hash3(a + salt, b + salt, c + salt); };
                float c000 = H(ix,   iy,   iz),   c100 = H(ix+1, iy,   iz);
                float c010 = H(ix,   iy+1, iz),   c110 = H(ix+1, iy+1, iz);
                float c001 = H(ix,   iy,   iz+1), c101 = H(ix+1, iy,   iz+1);
                float c011 = H(ix,   iy+1, iz+1), c111 = H(ix+1, iy+1, iz+1);
                float x00 = c000 + (c100 - c000) * fx;
                float x10 = c010 + (c110 - c010) * fx;
                float x01 = c001 + (c101 - c001) * fx;
                float x11 = c011 + (c111 - c011) * fx;
                float y0 = x00 + (x10 - x00) * fy;
                float y1 = x01 + (x11 - x01) * fy;
                return y0 + (y1 - y0) * fz;
            };
            float n1 = vnoise(dx * 2.2f, dyv * 2.2f, dz * 2.2f, 1.0f) * 0.65f
                     + vnoise(dx * 5.0f, dyv * 5.0f, dz * 5.0f, 4.0f) * 0.35f;
            float n2 = vnoise(dx * 3.3f, dyv * 3.3f, dz * 3.3f, 9.0f);
            float neb = std::clamp((n1 - 0.45f) * 2.2f, 0.0f, 1.0f) * (0.4f + 0.6f * band);
            baseR += neb * 0.14f;
            baseB += neb * 0.08f;
            float neb2 = std::clamp((n2 - 0.55f) * 2.2f, 0.0f, 1.0f);
            baseG += neb2 * 0.06f;
            baseR += neb2 * 0.04f;

            float r = baseR, g = baseG, b = baseB;

            // ---- Layer 1: bright stars (sparse). ----
            float s1 = evalStar(dx, dyv, dz, 200.0f, 0.85f, 0.4f, 0.1f);
            if (s1 > 0.0f) {
                float warm = fract1(hash3(dx * 12.0f, dyv * 12.0f, dz * 12.0f) * 53.0f);
                float tintR = (1.0f - warm) * 0.85f + warm * 1.05f;
                float tintG = (1.0f - warm) * 0.90f + warm * 0.95f;
                float tintB = (1.0f - warm) * 1.05f + warm * 0.85f;
                r += tintR * s1; g += tintG * s1; b += tintB * s1;
            }
            // ---- Layer 2: dim dust (denser, sparser threshold). ----
            float s2 = evalStar(dx, dyv, dz, 400.0f, 0.94f, 0.24f, 7.7f);
            float dust = s2 * 0.35f;
            r += dust; g += dust; b += dust;

            uint8_t* p = &px[((size_t)y * w + x) * 4];
            p[0] = toU8(r); p[1] = toU8(g); p[2] = toU8(b); p[3] = 255;
        }
    }
    return px;
}

// ---- Default planet material: latitudinal colour bands + a checker overlay so
// the sphere reads with surface detail (substitutes for a real albedo map until
// planet-texture packs are wired in via assetRoot). White-ish base so the
// per-planet albedo tint (baseColorFactor) recolours each planet.
std::vector<uint8_t> bakePlanetTexture(uint32_t w, uint32_t h) {
    std::vector<uint8_t> px((size_t)w * h * 4, 0);
    const uint32_t cell = std::max(1u, w / 24);
    for (uint32_t y = 0; y < h; ++y) {
        float v = (y + 0.5f) / (float)h;     // 0 at +Y pole, 1 at -Y pole
        for (uint32_t x = 0; x < w; ++x) {
            // Latitude bands: light poles, banded mid-latitudes (gas-giant vibe).
            float lat = std::fabs(v - 0.5f) * 2.0f;             // 0 equator .. 1 pole
            float band = 0.55f + 0.30f * std::sin(v * kPi * 7.0f);
            float pole = 0.75f + 0.25f * lat;
            float shade = std::clamp(band * pole, 0.0f, 1.0f);
            // Checker overlay so the texture has high-frequency detail (pixel
            // variance) and the sphere never reads as a flat disc.
            bool chk = (((x / cell) ^ (y / cell)) & 1u) != 0;
            float c = shade * (chk ? 1.0f : 0.78f);
            // Faint hue variation per band for richer colour.
            float r = c, g = c * (0.93f + 0.07f * std::sin(v * kPi * 5.0f)), bl = c * 0.88f;
            uint8_t* p = &px[((size_t)y * w + x) * 4];
            p[0] = toU8(r); p[1] = toU8(g); p[2] = toU8(bl); p[3] = 255;
        }
    }
    return px;
}

// ---- Radial sun-glow sprite: bright white-hot core -> warm halo -> 0 alpha. -
std::vector<uint8_t> bakeSunGlow(uint32_t n) {
    std::vector<uint8_t> px((size_t)n * n * 4, 0);
    const float c = (n - 1) * 0.5f;
    const float maxR = c;
    for (uint32_t y = 0; y < n; ++y) {
        for (uint32_t x = 0; x < n; ++x) {
            float ddx = (x - c), ddy = (y - c);
            float d = std::sqrt(ddx * ddx + ddy * ddy) / maxR;   // 0 center .. 1 edge
            float core = std::clamp(1.0f - d / 0.20f, 0.0f, 1.0f);      // white-hot disc
            float halo = std::exp(-d * d * 6.0f);                       // soft glow
            float a = std::clamp(core + halo, 0.0f, 1.0f);
            // Colour: white core blending to warm orange in the halo.
            float r = 1.0f;
            float g = 0.75f + 0.25f * core;
            float bl = 0.45f + 0.55f * core;
            uint8_t* p = &px[((size_t)y * n + x) * 4];
            p[0] = toU8(r); p[1] = toU8(g); p[2] = toU8(bl); p[3] = toU8(a);
        }
    }
    return px;
}

} // namespace

// ---------------------------------------------------------------------------
void SpaceEnv::init(rhi::IRenderDevice& dev, float domeRadius) {
    if (m_initialized) return;

    // Dome: an inside-out sphere re-centered on the camera each frame (so it reads
    // at infinity). The dome writes depth, so its radius must ENCLOSE all scene
    // geometry — anything beyond the dome is occluded by it. Default 50 m suits the
    // close-range showcase; deep scenes (the intro capital at ~280 m + the far plane
    // at ~12 km) pass a large radius so nothing pops out behind the backdrop.
    m_domeRadius = (domeRadius > 1.0f) ? domeRadius : 50.0f;
    Mesh dome = buildDome(m_domeRadius, 32);
    m_domeMesh = dev.createMesh(dome.verts.data(), (uint32_t)dome.verts.size(),
                                dome.index.data(), (uint32_t)dome.index.size());
    std::vector<uint8_t> domePx = bakeNebulaStars(1024, 512);
    m_domeTex = dev.createTexture(domePx.data(), 1024, 512, /*srgb=*/false);

    // Shared planet sphere (unit radius; per-planet model scales it).
    Mesh sphere = buildUvSphere(1.0f, 24);          // ~2.3k tris
    m_sphereMesh = dev.createMesh(sphere.verts.data(), (uint32_t)sphere.verts.size(),
                                  sphere.index.data(), (uint32_t)sphere.index.size());
    std::vector<uint8_t> planetPx = bakePlanetTexture(512, 256);
    m_planetTex = dev.createTexture(planetPx.data(), 512, 256, /*srgb=*/true);

    // Sun billboard quad + radial-glow sprite.
    Mesh quad = buildQuad();
    m_spriteMesh = dev.createMesh(quad.verts.data(), (uint32_t)quad.verts.size(),
                                  quad.index.data(), (uint32_t)quad.index.size());
    std::vector<uint8_t> sunPx = bakeSunGlow(128);
    m_sunTex = dev.createTexture(sunPx.data(), 128, 128, /*srgb=*/true);

    m_initialized = m_domeMesh.valid() && m_domeTex.valid() &&
                    m_sphereMesh.valid() && m_planetTex.valid() &&
                    m_spriteMesh.valid() && m_sunTex.valid();
    if (!m_initialized)
        x3::logError("SpaceEnv::init: mesh or texture creation failed");
}

uint32_t SpaceEnv::addPlanet(const float pos[3], float radius, const float albedo[3]) {
    Planet p;
    p.pos[0] = pos[0]; p.pos[1] = pos[1]; p.pos[2] = pos[2];
    p.radius = (radius > 0.0f) ? radius : 1.0f;
    p.albedo[0] = albedo[0]; p.albedo[1] = albedo[1]; p.albedo[2] = albedo[2];
    m_planets.push_back(p);
    return (uint32_t)(m_planets.size() - 1);
}

void SpaceEnv::setSun(const float dir[3], const float color[3], float intensity) {
    float l = std::sqrt(dir[0]*dir[0] + dir[1]*dir[1] + dir[2]*dir[2]);
    if (l < 1e-6f) {
        // Degenerate direction: fall back to a sensible default (up + forward).
        m_sunDir[0] = 0.0f; m_sunDir[1] = 0.4f; m_sunDir[2] = 1.0f;
        l = std::sqrt(m_sunDir[1]*m_sunDir[1] + m_sunDir[2]*m_sunDir[2]);
        m_sunDir[1] /= l; m_sunDir[2] /= l;
    } else {
        m_sunDir[0] = dir[0] / l;
        m_sunDir[1] = dir[1] / l;
        m_sunDir[2] = dir[2] / l;
    }
    m_sunColor[0] = color[0]; m_sunColor[1] = color[1]; m_sunColor[2] = color[2];
    m_sunIntensity = (intensity > 0.0f) ? intensity : 1.0f;
    m_sunSet = true;
}

void SpaceEnv::setCamera(float ex, float ey, float ez) {
    m_camX = ex; m_camY = ey; m_camZ = ez;
}

void SpaceEnv::shutdown(rhi::IRenderDevice& dev) {
    if (m_domeMesh.valid())   { dev.destroyMesh(m_domeMesh);     m_domeMesh   = {}; }
    if (m_domeTex.valid())    { dev.destroyTexture(m_domeTex);   m_domeTex    = {}; }
    if (m_sphereMesh.valid()) { dev.destroyMesh(m_sphereMesh);   m_sphereMesh = {}; }
    if (m_planetTex.valid())  { dev.destroyTexture(m_planetTex); m_planetTex  = {}; }
    if (m_spriteMesh.valid()) { dev.destroyMesh(m_spriteMesh);   m_spriteMesh = {}; }
    if (m_sunTex.valid())     { dev.destroyTexture(m_sunTex);    m_sunTex     = {}; }
    m_planets.clear();
    m_initialized = false;
    m_sunSet = false;
}

// ---------------------------------------------------------------------------
void SpaceEnv::render(rhi::IRenderDevice& dev, const rhi::FrameContext& fr,
                      const float* viewProj16, float timeSec) {
    (void)viewProj16;   // accepted for parity; dome is camera-anchored implicitly
    if (!m_initialized) return;

    // ---- 1. Star/nebula dome (camera-anchored, far). ----
    {
        const float twinkle = 0.85f + 0.15f * std::sin(timeSec * 2.0f * kTau);
        const float boost = 6.0f * twinkle;    // lifts baked stars; kept modest so
                                               // the lit planets still read in front
        float m[16] = {
            1, 0, 0, 0,
            0, 1, 0, 0,
            0, 0, 1, 0,
            m_camX, m_camY, m_camZ, 1,
        };
        const float baseFactor[4] = { boost, boost, boost, 1.0f };
        const float emissive[4]   = { 0, 0, 0, 0 };  // stars carried by the texture
        dev.drawMeshEmissive(fr, m_domeMesh, m_domeTex, baseFactor, emissive, m);
    }

    // ---- 2. Planets. Each is the shared unit sphere scaled by radius + placed.
    for (const Planet& p : m_planets) {
        float m[16] = {
            p.radius, 0, 0, 0,
            0, p.radius, 0, 0,
            0, 0, p.radius, 0,
            p.pos[0], p.pos[1], p.pos[2], 1,
        };
        const float baseFactor[4] = { p.albedo[0], p.albedo[1], p.albedo[2], 1.0f };
        // A faint self-illumination (a touch of the albedo) so the unlit
        // hemisphere isn't pure black against the dim nebula (deep space has no
        // ambient fill); the sun side still reads brighter via the directional
        // term an integrator may add. Keeps the planet recognizable headless.
        const float emissive[4] = {
            p.albedo[0] * 0.55f, p.albedo[1] * 0.55f, p.albedo[2] * 0.55f, 1.0f
        };
        dev.drawMeshEmissive(fr, m_sphereMesh, m_planetTex, baseFactor, emissive, m);
    }

    // ---- 3. Sun: a bright emissive SPHERE far along the sun dir. ----
    // An opaque sphere (not a billboard quad) so it reads as a clean ROUND star
    // with no square-footprint artifact (the opaque mesh pass has no alpha
    // blending; a textured quad would show its dark corners over the starfield).
    // It sits inside the dome (radius 50) and is camera-anchored so it stays at
    // a fixed sky position. The radial-glow texture tints the limb; the strong
    // emissive term pushes the whole disc into the HDR bloom chain.
    if (m_sunSet) {
        // Keep the sun just inside the dome and scale its size with distance so its
        // angular size (and thus the god-ray/flare source) is stable across radii.
        const float dist = m_domeRadius * 0.76f;  // inside the dome
        float cx = m_camX + m_sunDir[0] * dist;
        float cy = m_camY + m_sunDir[1] * dist;
        float cz = m_camZ + m_sunDir[2] * dist;
        const float pulse = 0.9f + 0.1f * std::sin(timeSec * 1.3f);
        // Ratio chosen so the DEFAULT dome (50 m -> dist 38) yields the historical
        // 5 m sun radius exactly (byte-identical showcase); scales for large domes.
        const float s = (5.0f / 38.0f) * dist * pulse;
        float m[16] = {
            s, 0, 0, 0,
            0, s, 0, 0,
            0, 0, s, 0,
            cx, cy, cz, 1,
        };
        const float baseFactor[4] = {
            m_sunColor[0], m_sunColor[1], m_sunColor[2], 1.0f
        };
        const float k = 12.0f * m_sunIntensity * pulse;   // HDR bloom source
        const float emissive[4] = {
            m_sunColor[0] * k, m_sunColor[1] * k, m_sunColor[2] * k, 1.0f
        };
        dev.drawMeshEmissive(fr, m_sphereMesh, m_sunTex, baseFactor, emissive, m);
    }
}

} // namespace x3::space
