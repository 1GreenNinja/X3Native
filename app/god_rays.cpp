// GOD RAYS — shafts of sunlight under the water surface. See god_rays.h.
//
// Clean-room: X3Native's own Scene / terrain / engine interfaces only. The
// shaft material is the street-light cone's proven additive-glow mode; the
// geometry, gradient bake and placement are original.
#include "god_rays.h"
#include "terrain.h"

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace x3::game {

namespace {

constexpr float kPi = 3.14159265358979f;

// Deterministic integer-hash 0..1 (the street-light/facility hash).
float h01(uint32_t s) {
    s ^= s >> 16; s *= 0x7feb352du; s ^= s >> 15; s *= 0x846ca68bu; s ^= s >> 16;
    return (float)(s & 0xffffffu) / 16777216.0f;
}

float smoothstepf(float a, float b, float x) {
    float t = (x - a) / (b - a);
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return t * t * (3.0f - 2.0f * t);
}

// One SHAFT, WORLD-BAKED (the street-light doctrine: exact world-space
// normals + a translation-only transform, because the mesh path transforms
// normals with the plain model mat3 and any non-uniform scale would skew
// them and kill the additive rim fade). A crossed pair of tapered quads —
// both windings, so some face is always toward the eye — with the origin at
// the shaft TOP (just under the surface), dropping `drop` meters, widening
// topHalfW -> botHalfW, and LEANING down-sun by (leanX, leanZ) per meter of
// drop. `crossAngle` rotates the cross per shaft so no two read identical.
// UV: u across the width, v = 0 at the surface -> 1 at the tip (the 2D
// gradient bake rides it).
void makeShaftMesh(float topHalfW, float botHalfW, float drop,
                   float leanX, float leanZ, float crossAngle,
                   std::vector<x3::rhi::MeshVertex>& verts,
                   std::vector<uint32_t>& idx) {
    verts.clear(); idx.clear();
    for (int arm = 0; arm < 2; ++arm) {
        const float a  = crossAngle + (float)arm * (kPi * 0.5f);
        const float ux = std::cos(a), uz = std::sin(a);     // width axis
        const float nx = -std::sin(a), nz = std::cos(a);    // quad normal
        const float bx = leanX * drop, bz = leanZ * drop;   // bottom lean
        for (int w = 0; w < 2; ++w) {                        // both windings
            const float s = (w == 0) ? 1.0f : -1.0f;
            const uint32_t base = (uint32_t)verts.size();
            auto push = [&](float px, float py, float pz, float u, float v) {
                x3::rhi::MeshVertex mv{};
                mv.pos[0] = px; mv.pos[1] = py; mv.pos[2] = pz;
                mv.normal[0] = nx * s; mv.normal[1] = 0.0f; mv.normal[2] = nz * s;
                mv.uv[0] = u; mv.uv[1] = v;
                verts.push_back(mv);
            };
            push(-ux * topHalfW,      0.0f, -uz * topHalfW,      0.0f, 0.0f);
            push( ux * topHalfW,      0.0f,  uz * topHalfW,      1.0f, 0.0f);
            push(-ux * botHalfW + bx, -drop, -uz * botHalfW + bz, 0.0f, 1.0f);
            push( ux * botHalfW + bx, -drop,  uz * botHalfW + bz, 1.0f, 1.0f);
            if (w == 0) idx.insert(idx.end(), { base, base + 2, base + 1,
                                                base + 1, base + 2, base + 3 });
            else        idx.insert(idx.end(), { base, base + 1, base + 2,
                                                base + 1, base + 3, base + 2 });
        }
    }
}

} // namespace

void GodRays::build(const Config& cfg, Scene& scene, x3::rhi::IRenderDevice& device) {
    if (m_built) return;
    m_roomId   = cfg.roomId;
    m_sunScale = std::max(cfg.sunScale, 0.0f);

    // Down-sun LEAN per meter of drop: the horizontal run of the refracted sun
    // ray. Snell compresses the in-air angle (n = 1.33 caps the underwater
    // angle at ~48.6 deg from vertical, tan ~1.13), so a low golden-hour sun
    // still gives a believable slant instead of a horizontal beam. 0.55 is the
    // compression fudge standing in for the full Snell solve; the cap is the
    // physical limit.
    {
        const float hx = -cfg.sunDirX, hz = -cfg.sunDirZ;
        const float hlen = std::sqrt(hx * hx + hz * hz);
        const float upY  = std::max(cfg.sunDirY, 0.05f);
        const float lean = std::min(hlen / upY * 0.55f, 1.05f);
        if (hlen > 1e-4f) { m_leanX = hx / hlen * lean; m_leanZ = hz / hlen * lean; }
    }

    // A dusk/night sun keeps NO shafts: don't even build the geometry.
    if (m_sunScale < 0.06f) {
        m_built = true;
        x3::logInfo("god-rays: sun too low (sunScale < 0.06) — no shafts built");
        return;
    }

    // 2D falloff bake (LINEAR, srgb=false): v axial (bright at the surface,
    // dissolved by ~78% of the drop), u across the width (soft sin^1.4 lobe so
    // a quad edge never reads as a plank edge). Row-flipped like the street
    // cone bake: the upload lands row 0 at v=1, so compute the axial
    // coordinate from the far end — v=0 (the surface) must be the bright end.
    {
        const int W = 32, H = 64;
        std::vector<uint8_t> px(W * H * 4);
        for (int y = 0; y < H; ++y) {
            const float v  = (float)(H - 1 - y) / (float)(H - 1);
            const float ax = std::pow(1.0f - smoothstepf(0.0f, 0.78f, v), 1.6f);
            for (int x = 0; x < W; ++x) {
                const float u = ((float)x + 0.5f) / (float)W;
                // sin^2.6: the energy lives in a center column and dies well
                // before the geometric edge — a hard quad silhouette is the
                // "solid plank" tell (first capture round proved it at 1.4).
                const float wd = std::pow(std::sin(kPi * u), 2.6f);
                const uint8_t b = (uint8_t)std::lround(255.0f * ax * wd);
                uint8_t* p = &px[(y * W + x) * 4];
                p[0] = p[1] = p[2] = b; p[3] = 255;
            }
        }
        m_grad = device.createTexture(px.data(), W, H, false);
    }

    // ---- Placement (deterministic LCG on authored coordinates) ------------
    uint32_t rn = 0;
    const WorldRiverNode* nodes = worldRiverNodes(rn);
    uint32_t riverN = 0, seaN = 0;
    if (nodes && rn >= 4) {
        // THE RIVER REACH nearest the facility — the same node window the fish
        // schools seed on (nearest-2 .. nearest+7), so the shafts fall over the
        // water the player actually swims.
        uint32_t nearest = 0; float best = 1e30f;
        for (uint32_t i = 0; i < rn; ++i) {
            const float dx = nodes[i].x - cfg.nearX, dz = nodes[i].z - cfg.nearZ;
            const float d2 = dx * dx + dz * dz;
            if (d2 < best) { best = d2; nearest = i; }
        }
        for (int offs = -2; offs <= 7; ++offs) {
            int i = (int)nearest + offs;
            if (i < 0) i = 0;
            if (i > (int)rn - 1) i = (int)rn - 1;
            const WorldRiverNode& nd = nodes[i];
            const uint32_t seed = (uint32_t)(i * 2654435761u) ^ 0x9d2c5680u;
            // 1-2 shafts per node, jittered inside the channel.
            const int count = 1 + (int)(h01(seed) > 0.55f);
            for (int k = 0; k < count; ++k) {
                const uint32_t s2 = seed + (uint32_t)k * 747796405u + 13u;
                const float jx = (h01(s2) - 0.5f) * 24.0f;
                const float jz = (h01(s2 * 3u + 7u) - 0.5f) * 24.0f;
                const uint32_t before = (uint32_t)m_shafts.size();
                addShaft(scene, device, nd.x + jx, nd.z + jz, s2);
                riverN += (uint32_t)m_shafts.size() - before;
            }
        }
        // THE ESTUARY / SEA SHALLOWS: a loose cluster walking out from the
        // river mouth into the sea (where the great white hunts the swimmer).
        const WorldRiverNode& E = nodes[rn - 1];
        const WorldRiverNode& P = nodes[rn - 2];
        const float eh = std::atan2(E.z - P.z, E.x - P.x);
        for (int k = 0; k < 6; ++k) {
            const uint32_t s2 = 0xE57A0000u + (uint32_t)k * 2246822519u;
            const float along = 12.0f + (float)k * 11.0f + h01(s2) * 6.0f;
            const float side  = (h01(s2 * 5u + 3u) - 0.5f) * 44.0f;
            const float px = E.x + std::cos(eh) * along - std::sin(eh) * side;
            const float pz = E.z + std::sin(eh) * along + std::cos(eh) * side;
            const uint32_t before = (uint32_t)m_shafts.size();
            addShaft(scene, device, px, pz, s2);
            seaN += (uint32_t)m_shafts.size() - before;
        }
    }

    m_built = true;
    x3::logInfo("god-rays: " + std::to_string(m_shafts.size()) +
                " sun shafts under the surface (" + std::to_string(riverN) +
                " river reach, " + std::to_string(seaN) +
                " estuary/sea) — additive glass, sunScale=" +
                std::to_string(m_sunScale));
}

void GodRays::addShaft(Scene& scene, x3::rhi::IRenderDevice& device,
                       float x, float z, uint32_t seed) {
    const float wY = worldWaterLevelAt(x, z);
    if (wY <= kWorldWaterDry * 0.5f) return;                 // dry — no water column
    const float bed   = terrainHeightAtWorld(x, z);
    const float depth = wY - bed;
    if (depth < 1.6f) return;                                // too shallow to read
    // Length capped by the real water column (never pokes the bed) and by the
    // ~9 m where the gradient has long dissolved anyway.
    const float drop = std::min(depth - 0.45f, 9.0f);
    const float topY = wY - 0.12f;                           // never above the surface

    // Per-shaft character: width grows with drop, cross rotation + strength
    // spread off the seed so no two shafts are clones.
    const float topHalfW = 0.16f + drop * (0.045f + 0.035f * h01(seed * 11u + 5u));
    const float botHalfW = topHalfW * (2.1f + 0.7f * h01(seed * 17u + 9u));
    const float crossAng = h01(seed * 29u + 1u) * kPi;

    std::vector<x3::rhi::MeshVertex> verts; std::vector<uint32_t> idx;
    makeShaftMesh(topHalfW, botHalfW, drop, m_leanX, m_leanZ, crossAng, verts, idx);

    Entity e;
    e.mesh = device.createMesh(verts.data(), (uint32_t)verts.size(),
                               idx.data(), (uint32_t)idx.size());
    e.tex  = m_grad;
    // Translation-only transform (world-baked mesh) — update() drifts it.
    e.transform[12] = x; e.transform[13] = topY; e.transform[14] = z;
    // Filtered sun: water eats the red first, so the shaft is a cool cyan-white.
    e.emissive[0] = 0.58f; e.emissive[1] = 0.84f; e.emissive[2] = 1.00f;
    // A WHISPER: additive over a sunlit bed saturates to white fast (the first
    // capture round read as glowing slabs at ~2x this) — the real light does
    // the lighting, the shaft only suggests the path it took.
    const float strength = (0.70f + 0.45f * h01(seed * 41u + 23u)) * m_sunScale;
    e.emissive[3] = strength;
    e.baseColor[3] = 1.0f;
    e.transparent = true;
    e.glass.opacity = 0.0f; e.glass.refraction = 0.0f;
    e.glass.roughness = 0.0f; e.glass.specular = 0.0f;
    e.glass.additive = 2.8f;              // strong view-angle rim fade (no planks)
    e.roomId = m_roomId;                  // outdoor PVS gates the draw
    e.tag = (uint32_t)Tag::Prop;

    Shaft s;
    s.ent = scene.handle(scene.add(e));
    s.x = x; s.z = z; s.topY = topY;
    s.baseStrength = strength;
    s.phase = h01(seed * 53u + 31u);
    m_shafts.push_back(s);
}

void GodRays::update(float dt, Scene& scene) {
    if (!m_built || m_shafts.empty() || dt <= 0.0f) return;
    m_time += dt;
    for (Shaft& s : m_shafts) {
        Entity* e = scene.getChecked(s.ent);
        if (!e) continue;
        // Slow alpha BREATHING (never to zero, never a strobe) + a slow
        // surface DRIFT — the wave lens wandering. All dt-scaled via m_time.
        const float ph = s.phase * 6.2831853f;
        const float breathe = 0.74f + 0.26f * std::sin(m_time * 0.31f + ph);
        e->emissive[3] = s.baseStrength * breathe;
        e->transform[12] = s.x + 0.45f * std::sin(m_time * 0.11f  + ph * 1.7f);
        e->transform[14] = s.z + 0.45f * std::cos(m_time * 0.087f + ph * 2.3f);
    }
}

} // namespace x3::game
