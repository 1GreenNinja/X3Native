// Tiled procedural terrain world (B2) + STREAMING (B3). See app/terrain.h.
//
// CLEAN-ROOM: built from the Scene + IRenderDevice + IPhysicsWorld + IJobSystem
// interfaces + the mesh_prims texture helper, and a self-implemented
// value-noise/fBm sampler. No game-engine source consulted.
//
// ---------------------------------------------------------------------------
// COLLISION CHOICE — static-mesh-per-tile (NOT a Jolt heightfield):
//   * IPhysicsWorld already exposes addStaticMesh() + removeBody(); reusing them
//     means ZERO new physics surface and keeps the interface free of any JPH::
//     types (the hard clean-boundary rule). addStaticMesh on stream-in /
//     removeBody on stream-out is exactly the unit the streamer loads/unloads.
//   * Cost: one MeshShape per tile from its LOD0 triangles (~2 * 32 * 32 = 2048
//     tris/tile). The streamer keeps only a small ring resident, so the live body
//     count is bounded ((2R+1)^2) regardless of how far the player travels.
//   * Collision ALWAYS uses LOD0 geometry regardless of the tile's draw LOD, so
//     the player never falls through a far tile that is *drawn* decimated.
// ---------------------------------------------------------------------------
// STREAMING (B3):
//   * Heavy CPU work (noise + 3 LOD meshes + collision soup) runs on the job
//     system (run/runIO) off the main thread; finished results land on a mutex-
//     guarded completion queue. The main thread drains <= N/frame and does the
//     single-threaded GPU createMesh + physics addStaticMesh + scene.add().
//   * The under-player 3x3 neighborhood is generated SYNCHRONOUSLY so collision
//     is always present beneath the focus (no fall-through during streaming).
// ---------------------------------------------------------------------------

#include "terrain.h"
#include "mesh_prims.h"
#include "headless_device.h"

#include "engine/core/x3_log.h"
#include "engine/physics/IPhysicsWorld.h"
#include "engine/core/IJobSystem.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace x3::game {

namespace {

constexpr float kIdentity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

// ---------------------------------------------------------------------------
// Value noise + fBm (public algorithm). Integer hash -> [0,1) lattice values,
// smoothstep-interpolated bilinearly, summed over octaves with halving
// amplitude + doubling frequency (fractal Brownian motion). Deterministic in
// (x, z, seed); identical on every machine (pure integer arithmetic + float).
// ---------------------------------------------------------------------------
inline uint32_t hashU32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
inline float hash2(int ix, int iz, uint32_t seed) {
    uint32_t h = hashU32((uint32_t)ix * 73856093u ^
                         (uint32_t)iz * 19349663u ^
                         seed * 83492791u);
    return (float)(h & 0x00FFFFFFu) / (float)0x01000000u;
}
inline float fade(float t) { return t * t * (3.0f - 2.0f * t); }

float valueNoise(float x, float z, uint32_t seed) {
    const float fx = std::floor(x), fz = std::floor(z);
    const int   ix = (int)fx,        iz = (int)fz;
    const float tx = fade(x - fx),   tz = fade(z - fz);

    const float v00 = hash2(ix,     iz,     seed);
    const float v10 = hash2(ix + 1, iz,     seed);
    const float v01 = hash2(ix,     iz + 1, seed);
    const float v11 = hash2(ix + 1, iz + 1, seed);

    const float a = v00 + (v10 - v00) * tx;
    const float b = v01 + (v11 - v01) * tx;
    return a + (b - a) * tz;
}

float fbm(float x, float z, float freq, uint32_t octaves, uint32_t seed) {
    float amp = 1.0f, sum = 0.0f, norm = 0.0f, f = freq;
    for (uint32_t o = 0; o < octaves; ++o) {
        sum  += amp * valueNoise(x * f + (float)o * 17.13f,
                                 z * f + (float)o * 31.71f,
                                 seed + o * 101u);
        norm += amp;
        amp  *= 0.5f;
        f    *= 2.0f;
    }
    return (norm > 0.0f) ? (sum / norm) : 0.0f;
}

// ---------------------------------------------------------------------------
// RIDGED multifractal fBm — the standard mountain-noise variant: each octave is
// folded as 1-|2v-1| (a sharp crease at the lattice midline) and weighted by the
// previous octave, so sums accumulate as jagged RIDGES + steep valleys instead
// of the round hills plain fBm makes. Returns [0,1). Clean-room (the public
// ridged-fBm construction, Musgrave / Texturing & Modeling).
// ---------------------------------------------------------------------------
float ridgedFbm(float x, float z, float freq, uint32_t octaves, uint32_t seed) {
    float amp = 0.5f, sum = 0.0f, norm = 0.0f, f = freq, prev = 1.0f;
    for (uint32_t o = 0; o < octaves; ++o) {
        float v = valueNoise(x * f + (float)o * 11.7f,
                             z * f + (float)o * 23.3f, seed + o * 131u);
        float r = 1.0f - std::fabs(2.0f * v - 1.0f);   // ridge crease
        r = r * r;                                     // sharpen
        sum  += amp * r * prev;                        // weight by last octave
        prev  = r;
        norm += amp;
        amp  *= 0.5f;
        f    *= 2.15f;
    }
    return (norm > 0.0f) ? (sum / norm) : 0.0f;
}
inline float smoothstep01(float a, float b, float x) {
    if (b <= a) return x >= b ? 1.0f : 0.0f;
    float t = (x - a) / (b - a);
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return t * t * (3.0f - 2.0f * t);
}

// ===========================================================================
// WORLD TOPOGRAPHY (canonical world only; cfg.worldTopography == true).
//
// The single spine terrainHeightAt() composes, in ABSOLUTE meters and always
// bounded to [0, heightScale]:
//   (1) rolling HILLS       — the legacy gentle fBm, low amplitude (kHillFrac).
//   (2) MOUNTAIN RANGES     — ridged fBm masked by a RADIAL bowl (low at the
//       playable center, rising to big peaks toward the horizon) + a low-freq
//       directional modulation so the ranges vary (N/E/S/W ridges + passes),
//       amplitude kMtnFrac. Center stays walkable; peaks ring the world.
//   (3) FLAT PADS           — the authored buildable sites (the facility tower
//       pad at the origin + the Scrapyard City footprint) are blended DEAD LEVEL
//       so the tower/city sit on flat ground with terrain rising beyond.
//   (4) FREEWAY carve       — the graded road corridor CUTS through high ground
//       and FILLS across dips: within the deck it is exactly the (grade-limited)
//       centerline elevation; across the shoulder it ramps to the natural
//       surface (cut slope where terrain is higher, embankment where lower).
//
// All coordinates are the world XZ frame (origin ~ the facility/spawn). The
// pad/route constants describe the CANONICAL world; the small self-test cfgs
// leave worldTopography OFF so they see only the legacy hills.
// ===========================================================================
namespace topo {

constexpr float kHillFrac = 0.10f;   // hills amplitude as a fraction of heightScale
constexpr float kMtnFrac  = 0.92f;   // mountains amplitude (sum < 1 => margin)

// Mountain bowl: mask ~0 inside kBowlInner, ramps to 1 by kBowlOuter (m from origin).
// Pulled in (420..1250 m) so the ranges rise SOONER + reach FULL amplitude within a
// walkable/streamable distance — tall (150..200 m, snow-capped) peaks read as real
// mountains on the horizon instead of gentle far hills, while the playable center
// (pads + freeway, all r<~130) stays masked flat.
constexpr float kBowlInner = 420.0f;
constexpr float kBowlOuter = 1250.0f;

// ---- Authored FLAT PADS (buildable, dead-level). {cx, cz, radius, feather, y}.
struct Pad { float cx, cz, r, feather, y; };
constexpr Pad kPads[] = {
    // Facility tower + landing apron + landed ship: flat at Y=0 around the origin.
    {    0.0f,   0.0f, 130.0f, 120.0f,  0.0f },
    // Scrapyard City footprint (blueprint's road-grid + freeway district),
    // toward the −X/+Z ranges; the freeway's far end lands here at deck level.
    { -500.0f, 700.0f, 230.0f, 170.0f, 16.0f },
};
constexpr uint32_t kPadCount = (uint32_t)(sizeof(kPads) / sizeof(kPads[0]));

// ---- FREEWAY route: waypoints {x, z} + baked centerline deck elevation (m).
// The deck heights are grade-limited BY CONSTRUCTION (every |dElev|/segLen well
// under kMaxGrade) and meet the pads at both ends (0 at the tower, 16 at the
// city) so the road ties into flat ground cleanly. --test-terrain re-verifies.
// The raw elevations are authored so a waypoint that lies inside a PAD's
// influence already sits at that pad's level (0 at the tower, 16 at the city):
// there the pad-adjust pull is a no-op, so the deck ties into the flat site dead-
// level (T5) with NO grade spike at the feather (T6). The whole 0->16 climb lives
// in the pad-FREE corridor (WP1..WP4), spread so every segment is well under 6%.
struct WayPt { float x, z, elev; };
constexpr WayPt kRoute[] = {
    {   24.0f,   10.0f,  0.0f },   // tower pad (dead level, y=0)
    {   70.0f,  150.0f,  0.0f },   // still inside tower-pad influence -> hold 0
    {   10.0f,  320.0f,  5.0f },   // clear of the pad: begin the graded climb
    { -150.0f,  470.0f, 11.0f },   // free corridor
    { -330.0f,  600.0f, 16.0f },   // entering city-pad influence -> at city level
    { -500.0f,  700.0f, 16.0f },   // Scrapyard City pad (dead level, y=16)
};
constexpr uint32_t kRouteCount = (uint32_t)(sizeof(kRoute) / sizeof(kRoute[0]));
constexpr float kRoadHalfWidth = 7.0f;    // 14 m deck (~4 lanes)
constexpr float kShoulder      = 12.0f;   // cut/fill blend band beyond the deck
constexpr float kMaxGrade      = 0.06f;   // 6% — the design grade limit

// Mountains + hills ONLY (no pads/freeway): the "natural" surface, absolute m.
float naturalHeight(const TerrainConfig& cfg, float x, float z) {
    const float hillAmp = cfg.heightScale * kHillFrac;
    const float mtnAmp  = cfg.heightScale * kMtnFrac;

    // (1) rolling hills — the legacy shaped fBm, low amplitude.
    float hb = fbm(x, z, cfg.noiseFreq, cfg.octaves, cfg.seed);
    hb = hb * hb * (3.0f - 2.0f * hb);
    float hills = hillAmp * hb;

    // (2) mountains — ridged fBm * radial bowl * directional modulation.
    const float r = std::sqrt(x * x + z * z);
    float bowl = smoothstep01(kBowlInner, kBowlOuter, r);
    // Directional variation: raise some bearings into RANGES, drop others to
    // PASSES (so the freeway can thread a low saddle), low-frequency + centered.
    float dirn = fbm(x, z, 0.00072f, 3, cfg.seed ^ 0x9e3779b9u);   // [0,1]
    float dir  = 0.45f + 0.75f * dirn;                             // 0.45..1.20
    float ridge = ridgedFbm(x, z, 0.00160f, 5, cfg.seed ^ 0x85ebca6bu);
    float mtn = mtnAmp * ridge * bowl * dir;

    float h = hills + mtn;
    return h;
}

// Pad blend at (x,z): pull a value y toward each pad's level by the pad weight
// (w = 1 inside r, feathered to 0 by r+feather). Same construction the terrain
// pad-flatten uses, so pads win identically for the ground AND the road deck.
float padAdjust(float y, float x, float z) {
    for (uint32_t i = 0; i < kPadCount; ++i) {
        const Pad& p = kPads[i];
        const float dx = x - p.cx, dz = z - p.cz;
        const float d = std::sqrt(dx * dx + dz * dz);
        const float w = 1.0f - smoothstep01(p.r, p.r + p.feather, d);
        y = y + (p.y - y) * w;
    }
    return y;
}

// ---- Freeway geometry. Nearest point on the route polyline to (x,z): fills the
// perpendicular distance, the PAD-ADJUSTED deck elevation there, and the nearest
// centerline point. The deck elevation is pad-adjusted at the CENTERLINE point so
// where the road threads a flat pad (tower/city) it ties in at pad level (dead
// level) instead of ramping through the buildable site. Also the pure arc-sampler
// for the ribbon/tests. Deck elevation is a property of the centerline position.
struct RoadHit { float dist; float elev; float px, pz; };
RoadHit nearestRoad(float x, float z) {
    RoadHit best{ 1e30f, 0.0f, 0.0f, 0.0f };
    for (uint32_t i = 0; i + 1 < kRouteCount; ++i) {
        const WayPt& a = kRoute[i];
        const WayPt& b = kRoute[i + 1];
        const float ex = b.x - a.x, ez = b.z - a.z;
        const float len2 = ex * ex + ez * ez;
        float u = (len2 > 1e-6f) ? ((x - a.x) * ex + (z - a.z) * ez) / len2 : 0.0f;
        u = u < 0.0f ? 0.0f : (u > 1.0f ? 1.0f : u);
        const float px = a.x + u * ex, pz = a.z + u * ez;
        const float dx = x - px, dz = z - pz;
        const float d = std::sqrt(dx * dx + dz * dz);
        if (d < best.dist) {
            best.dist = d;
            best.elev = padAdjust(a.elev + u * (b.elev - a.elev), px, pz);
            best.px = px; best.pz = pz;
        }
    }
    return best;
}

// Compose the full topography: natural surface, flatten pads, carve the freeway.
float compose(const TerrainConfig& cfg, float x, float z) {
    float h = naturalHeight(cfg, x, z);

    // (3) FLAT PADS — blend to dead level inside the radius, feathered out.
    h = padAdjust(h, x, z);

    // (4) FREEWAY carve — deck exactly at the graded centerline; shoulder ramps
    // from the deck up/down to the natural (already pad-adjusted) surface.
    const RoadHit rh = nearestRoad(x, z);
    if (rh.dist < kRoadHalfWidth + kShoulder) {
        const float blend = smoothstep01(kRoadHalfWidth, kRoadHalfWidth + kShoulder, rh.dist);
        h = rh.elev + (h - rh.elev) * blend;   // deck at blend=0, natural at blend=1
    }

    // Bounded to [0, heightScale] so the universal height invariant holds.
    if (h < 0.0f) h = 0.0f;
    if (h > cfg.heightScale) h = cfg.heightScale;
    return h;
}

} // namespace topo

// ---------------------------------------------------------------------------
// One vertex of the terrain surface at world (wx,wz): height + analytic-ish
// normal via central differences of the height field. Pure (config only) so it
// runs on a worker thread.
// ---------------------------------------------------------------------------
x3::rhi::MeshVertex makeTerrainVertex(const TerrainConfig& cfg,
                                      float wx, float wz, float u, float v, float eps) {
    const float h  = terrainHeightAt(cfg, wx, wz);
    const float hl = terrainHeightAt(cfg, wx - eps, wz);
    const float hr = terrainHeightAt(cfg, wx + eps, wz);
    const float hd = terrainHeightAt(cfg, wx, wz - eps);
    const float hu = terrainHeightAt(cfg, wx, wz + eps);
    float nx = (hl - hr);
    float nz = (hd - hu);
    float ny = 2.0f * eps;
    float inv = 1.0f / std::sqrt(nx * nx + ny * ny + nz * nz);
    x3::rhi::MeshVertex out;
    out.pos[0] = wx;  out.pos[1] = h;   out.pos[2] = wz;
    out.normal[0] = nx * inv; out.normal[1] = ny * inv; out.normal[2] = nz * inv;
    out.uv[0] = u; out.uv[1] = v;
    return out;
}

// ---------------------------------------------------------------------------
// Build one tile's render mesh at a given LOD from ABSOLUTE (signed) tile coords.
// originX/originZ are the tile's min-corner world position. Fills outVerts with
// the top surface + a downward skirt around the border to hide LOD cracks; the
// surface is the first vpe*vpe verts (used for collision). Pure / thread-safe.
// ---------------------------------------------------------------------------
void buildTileMeshAbs(const TerrainConfig& cfg, float originX, float originZ,
                      TerrainLod lod,
                      std::vector<x3::rhi::MeshVertex>& outVerts,
                      std::vector<uint32_t>& outIdx) {
    outVerts.clear();
    outIdx.clear();

    const uint32_t step  = 1u << (uint32_t)lod;          // 1, 2, 4 quad stride
    const uint32_t quads = (cfg.tileVerts - 1) / step;   // quads per edge at LOD
    const uint32_t vpe   = quads + 1;                     // verts per edge
    const float    tileSize = cfg.tileSize;
    const float    cell  = tileSize / (float)quads;       // world meters per quad
    const float    ox = originX, oz = originZ;
    const float    eps = tileSize / (float)(cfg.tileVerts - 1) * 0.5f;
    const float    uvScale = 4.0f;

    outVerts.reserve((size_t)vpe * vpe + (size_t)vpe * 4);
    outIdx.reserve((size_t)quads * quads * 6 + (size_t)quads * 4 * 6);

    // ---- Top surface grid ------------------------------------------------
    for (uint32_t j = 0; j < vpe; ++j) {
        for (uint32_t i = 0; i < vpe; ++i) {
            const float wx = ox + i * cell;
            const float wz = oz + j * cell;
            const float u  = (float)i / (float)quads * uvScale;
            const float v  = (float)j / (float)quads * uvScale;
            outVerts.push_back(makeTerrainVertex(cfg, wx, wz, u, v, eps));
        }
    }
    for (uint32_t j = 0; j < quads; ++j) {
        for (uint32_t i = 0; i < quads; ++i) {
            const uint32_t a = j * vpe + i;
            const uint32_t b = a + 1;
            const uint32_t c = a + vpe;
            const uint32_t d = c + 1;
            outIdx.insert(outIdx.end(), { a, c, b,  b, c, d });
        }
    }

    // ---- Crack-hiding SKIRT (not in the collision surface) ---------------
    const float skirtDepth = cfg.heightScale * 0.25f + 1.0f;
    auto addSkirtEdge = [&](uint32_t i0, uint32_t j0, uint32_t i1, uint32_t j1) {
        const uint32_t topA = j0 * vpe + i0;
        const uint32_t topB = j1 * vpe + i1;
        const x3::rhi::MeshVertex& va = outVerts[topA];
        const x3::rhi::MeshVertex& vb = outVerts[topB];
        x3::rhi::MeshVertex la = va, lb = vb;
        la.pos[1] -= skirtDepth; lb.pos[1] -= skirtDepth;
        float ex = vb.pos[0] - va.pos[0], ez = vb.pos[2] - va.pos[2];
        float nx = -ez, nz = ex;
        float inv = 1.0f / std::max(1e-5f, std::sqrt(nx * nx + nz * nz));
        nx *= inv; nz *= inv;
        for (auto* p : { &la, &lb }) { p->normal[0] = nx; p->normal[1] = 0.0f; p->normal[2] = nz; }
        x3::rhi::MeshVertex ta = va, tb = vb;
        for (auto* p : { &ta, &tb }) { p->normal[0] = nx; p->normal[1] = 0.0f; p->normal[2] = nz; }
        const uint32_t base = (uint32_t)outVerts.size();
        outVerts.push_back(ta);
        outVerts.push_back(tb);
        outVerts.push_back(la);
        outVerts.push_back(lb);
        outIdx.insert(outIdx.end(), { base + 0, base + 2, base + 1,
                                      base + 1, base + 2, base + 3 });
    };
    for (uint32_t i = 0; i < quads; ++i) addSkirtEdge(i, 0, i + 1, 0);
    for (uint32_t i = 0; i < quads; ++i) addSkirtEdge(i + 1, vpe - 1, i, vpe - 1);
    for (uint32_t j = 0; j < quads; ++j) addSkirtEdge(0, j + 1, 0, j);
    for (uint32_t j = 0; j < quads; ++j) addSkirtEdge(vpe - 1, j, vpe - 1, j + 1);
}

// ---------------------------------------------------------------------------
// Procedural ground DETAIL texture: a small seamless RGBA8 tile around a base
// colour, perturbed by tileable value noise + a couple of fBm octaves so the
// surface reads as a natural grain (grass blades / rock grit / snow sparkle /
// sand) rather than a flat fill. `varR/G/B` is the per-channel +/- amplitude of
// the noise around (baseR,baseG,baseB). Seamless: the value noise wraps modulo
// `n` so adjacent world-space tiles abut without a visible seam. (CLEAN-ROOM —
// the same value-noise/fBm idea used for the heightfield, applied to colour.)
// ---------------------------------------------------------------------------
std::vector<uint8_t> makeDetailRGBA(uint32_t n, uint32_t seed,
                                    int baseR, int baseG, int baseB,
                                    int varR, int varG, int varB) {
    // Tileable value noise: hash on integer lattice coords taken modulo `period`
    // so the field repeats every `period` texels (=> the tile is seamless).
    auto vnoiseTile = [&](float x, float y, uint32_t period, uint32_t s) -> float {
        const float fx = std::floor(x), fy = std::floor(y);
        const int   ix = (int)fx,        iy = (int)fy;
        const float tx = fade(x - fx),   ty = fade(y - fy);
        auto h = [&](int a, int b) -> float {
            uint32_t aa = (uint32_t)((a % (int)period + (int)period) % (int)period);
            uint32_t bb = (uint32_t)((b % (int)period + (int)period) % (int)period);
            return hash2((int)aa, (int)bb, s);
        };
        const float v00 = h(ix,     iy);
        const float v10 = h(ix + 1, iy);
        const float v01 = h(ix,     iy + 1);
        const float v11 = h(ix + 1, iy + 1);
        const float a = v00 + (v10 - v00) * tx;
        const float b = v01 + (v11 - v01) * tx;
        return a + (b - a) * ty;
    };

    std::vector<uint8_t> px((size_t)n * n * 4);
    const uint32_t periodLo = 8, periodHi = 16;   // two octave wrap periods
    for (uint32_t y = 0; y < n; ++y) {
        for (uint32_t x = 0; x < n; ++x) {
            const float fx = (float)x / (float)n;
            const float fy = (float)y / (float)n;
            // Two tileable octaves (low + high frequency) summed, centred at 0.
            float nlo = vnoiseTile(fx * periodLo, fy * periodLo, periodLo, seed);
            float nhi = vnoiseTile(fx * periodHi, fy * periodHi, periodHi, seed + 7u);
            float nval = (nlo * 0.65f + nhi * 0.35f) - 0.5f;   // [-0.5,0.5]
            uint8_t* p = &px[((size_t)y * n + x) * 4];
            auto clampB = [](int v) -> uint8_t {
                return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
            };
            p[0] = clampB(baseR + (int)(nval * 2.0f * (float)varR));
            p[1] = clampB(baseG + (int)(nval * 2.0f * (float)varG));
            p[2] = clampB(baseB + (int)(nval * 2.0f * (float)varB));
            p[3] = 255;
        }
    }
    return px;
}

// Build the four ground DETAIL textures (grass / rock / snow / sand), register
// them as the terrain MATERIAL set, and return the opaque MARKER handle the
// renderer uses to flag terrain draws for the height+slope splat in mesh.frag.
// One marker per terrain/streamer instance; the four detail textures live in the
// device's bindless array (freed at device shutdown, like the old ground tex).
// If the device can't bind them (e.g. headless), the marker may be invalid and
// terrain falls back to a flat tint — still correct, just not splatted.
x3::rhi::TextureHandle makeGroundTexture(x3::rhi::IRenderDevice& device) {
    const uint32_t kN = 64;
    auto grassPx = makeDetailRGBA(kN, 1001u,  78, 116,  56,  18, 22, 16); // green
    auto rockPx  = makeDetailRGBA(kN, 2002u, 104, 100,  92,  26, 24, 22); // grey-brown
    auto snowPx  = makeDetailRGBA(kN, 3003u, 222, 226, 235,  16, 14, 12); // bright white-blue
    auto sandPx  = makeDetailRGBA(kN, 4004u, 178, 158, 118,  20, 18, 14); // tan
    auto grass = device.createTexture(grassPx.data(), kN, kN, /*srgb=*/true);
    auto rock  = device.createTexture(rockPx.data(),  kN, kN, /*srgb=*/true);
    auto snow  = device.createTexture(snowPx.data(),  kN, kN, /*srgb=*/true);
    auto sand  = device.createTexture(sandPx.data(),  kN, kN, /*srgb=*/true);
    x3::rhi::TextureHandle marker =
        device.registerTerrainMaterial(grass, rock, snow, sand);
    // Fallback: if the material set couldn't be registered (no bindless), use the
    // grass tile directly so terrain is at least a believable green, not white.
    return marker.valid() ? marker : grass;
}

// LOD by center-to-camera distance. Thresholds scale with tile size so the
// scheme is resolution-independent. Near = Full, mid = Half, far = Quarter.
TerrainLod lodForDist(const TerrainConfig& cfg, float dist) {
    const float nearD = cfg.tileSize * 2.5f;
    const float midD  = cfg.tileSize * 6.0f;
    if (dist < nearD) return TerrainLod::Full;
    if (dist < midD)  return TerrainLod::Half;
    return TerrainLod::Quarter;
}

} // namespace

// ---------------------------------------------------------------------------
// Public height sampler. Gentle rolling hills: an fBm field shaped by a mild
// power curve scaled to [0, heightScale]. Pure function of the config + (x,z).
// ---------------------------------------------------------------------------
float terrainHeightAt(const TerrainConfig& cfg, float worldX, float worldZ) {
    if (cfg.worldTopography)
        return topo::compose(cfg, worldX, worldZ);
    // Legacy gentle-rolling-hills field (the self-test worlds + any cfg that
    // hasn't opted into real topography). Unchanged: [0, heightScale], varied.
    float h = fbm(worldX, worldZ, cfg.noiseFreq, cfg.octaves, cfg.seed);
    h = h * h * (3.0f - 2.0f * h);
    return h * cfg.heightScale;
}

// ---------------------------------------------------------------------------
// Placement API — the single canonical world config + the height/normal/place
// helpers the 14900k anchors buildings (the Spire) + the cliffside pad with.
// The config is the engine TerrainConfig defaults (matching what `--world
// terrain`/`--world ocean` build the streamer from), exposed by const-ref so a
// query and the rendered/streamed surface always agree.
// ---------------------------------------------------------------------------
const TerrainConfig& worldTerrainConfig() {
    static const TerrainConfig kWorld = [] {
        TerrainConfig c{};             // engine defaults (32 m tiles, seed 1337, …)
        c.worldTopography = true;      // REAL mountains + hills + pads + freeway
        c.heightScale     = 210.0f;    // envelope: peaks ~180 m, hills ~21 m
        return c;
    }();
    return kWorld;
}

// ---------------------------------------------------------------------------
// World topography query API (see terrain.h). Thin, pure accessors over the
// authored pad + freeway constants in namespace topo above.
// ---------------------------------------------------------------------------
uint32_t worldFlatPadCount() { return topo::kPadCount; }
void worldFlatPad(uint32_t i, float& cx, float& cz, float& r, float& y) {
    const topo::Pad& p = topo::kPads[i < topo::kPadCount ? i : 0];
    cx = p.cx; cz = p.cz; r = p.r; y = p.y;
}
uint32_t worldFreewayPointCount() { return topo::kRouteCount; }
void worldFreewayPoint(uint32_t i, float& x, float& z, float& elev) {
    const topo::WayPt& w = topo::kRoute[i < topo::kRouteCount ? i : 0];
    x = w.x; z = w.z; elev = w.elev;
}
float worldFreewayHalfWidth() { return topo::kRoadHalfWidth; }

float worldFreewayLength() {
    float len = 0.0f;
    for (uint32_t i = 0; i + 1 < topo::kRouteCount; ++i) {
        const float dx = topo::kRoute[i + 1].x - topo::kRoute[i].x;
        const float dz = topo::kRoute[i + 1].z - topo::kRoute[i].z;
        len += std::sqrt(dx * dx + dz * dz);
    }
    return len;
}

bool worldFreewaySampleArc(float s, float outCenter[3], float outTangent[2]) {
    if (topo::kRouteCount < 2) return false;
    s = s < 0.0f ? 0.0f : (s > 1.0f ? 1.0f : s);
    const float target = s * worldFreewayLength();
    float acc = 0.0f;
    for (uint32_t i = 0; i + 1 < topo::kRouteCount; ++i) {
        const topo::WayPt& a = topo::kRoute[i];
        const topo::WayPt& b = topo::kRoute[i + 1];
        const float dx = b.x - a.x, dz = b.z - a.z;
        const float segLen = std::sqrt(dx * dx + dz * dz);
        if (acc + segLen >= target || i + 2 == topo::kRouteCount) {
            const float u = segLen > 1e-6f ? (target - acc) / segLen : 0.0f;
            const float uu = u < 0.0f ? 0.0f : (u > 1.0f ? 1.0f : u);
            outCenter[0] = a.x + uu * dx;
            outCenter[2] = a.z + uu * dz;
            // Pad-adjusted deck: where the route threads a flat pad it ties in at
            // pad level (matches compose()'s carve), so the deck stays dead-level
            // inside the tower/city sites instead of ramping through them.
            outCenter[1] = topo::padAdjust(a.elev + uu * (b.elev - a.elev),
                                           outCenter[0], outCenter[2]);
            const float inv = 1.0f / std::max(1e-6f, segLen);
            outTangent[0] = dx * inv; outTangent[1] = dz * inv;
            return true;
        }
        acc += segLen;
    }
    return false;
}

float terrainHeightAtWorld(float x, float z) {
    return terrainHeightAt(worldTerrainConfig(), x, z);
}

void terrainNormalAtWorld(float x, float z, float outNormal[3]) {
    // Central differences of the height field, the same construction the tile
    // mesher uses (makeTerrainVertex): n = normalize(hl-hr, 2*eps, hd-hu). eps is
    // scaled to the LOD0 cell so the slope it samples matches the
    // rendered/collidable mesh's surface normal.
    const TerrainConfig& cfg = worldTerrainConfig();
    const float eps = cfg.tileSize / (float)(cfg.tileVerts - 1) * 0.5f;
    const float hl = terrainHeightAt(cfg, x - eps, z);
    const float hr = terrainHeightAt(cfg, x + eps, z);
    const float hd = terrainHeightAt(cfg, x, z - eps);
    const float hu = terrainHeightAt(cfg, x, z + eps);
    float nx = (hl - hr);
    float nz = (hd - hu);
    float ny = 2.0f * eps;                  // always > 0 => normal points +Y up
    const float inv = 1.0f / std::sqrt(nx * nx + ny * ny + nz * nz);
    outNormal[0] = nx * inv;
    outNormal[1] = ny * inv;
    outNormal[2] = nz * inv;
}

void placeOnTerrain(float x, float z, float outPos[3]) {
    outPos[0] = x;
    outPos[1] = terrainHeightAtWorld(x, z);
    outPos[2] = z;
}

// ---------------------------------------------------------------------------
// FREEWAY asphalt ribbon (see terrain.h). A single static strip mesh swept along
// the graded centerline (worldFreewaySampleArc), laid just above the carved deck,
// with a procedurally-painted asphalt + lane-lines texture. Visual only; the car
// drives the flat carved terrain deck underneath. Clean-room (the same value-noise
// used for terrain, applied to a road cross-section paint).
// ---------------------------------------------------------------------------
uint32_t buildFreewayRibbon(Scene& scene, x3::rhi::IRenderDevice& device) {
    const float len = worldFreewayLength();
    if (len < 1.0f) return kNoLink;
    const float halfW = worldFreewayHalfWidth();

    // ---- Asphalt + lane-lines texture. U = ACROSS the deck (0..1), V = ALONG it;
    // one V tile spans kTileLen metres of road so the dashes repeat at a real-world
    // cadence. Dark asphalt grain + solid white edge lines + a dashed yellow center
    // line + faint dashed lane dividers (a 4-lane divided look).
    const uint32_t TW = 96, TH = 128;      // across x along texels
    const float    kTileLen = 8.0f;        // metres of road per V tile
    std::vector<uint8_t> tpx((size_t)TW * TH * 4);
    for (uint32_t y = 0; y < TH; ++y) {
        for (uint32_t x = 0; x < TW; ++x) {
            const float nx = (x + 0.5f) / (float)TW;   // across [0,1]
            const float ny = (y + 0.5f) / (float)TH;   // along  [0,1]
            const float grain = valueNoise(nx * 37.0f, ny * 43.0f, 5150u);
            int base = 44 + (int)(grain * 18.0f);       // 44..62 dark grey
            int r = base, g = base, b = base + 3;
            auto band = [&](float c, float hw) { return std::fabs(nx - c) < hw; };
            if (band(0.055f, 0.018f) || band(0.945f, 0.018f)) {          // edge lines
                r = g = b = 225;
            } else if (band(0.5f, 0.017f) && ny > 0.10f && ny < 0.52f) { // center dash
                r = 236; g = 206; b = 66;
            } else if ((band(0.28f, 0.010f) || band(0.72f, 0.010f)) &&
                       ny > 0.55f && ny < 0.88f) {                       // lane dividers
                r = g = b = 165;
            }
            uint8_t* p = &tpx[((size_t)y * TW + x) * 4];
            p[0] = (uint8_t)r; p[1] = (uint8_t)g; p[2] = (uint8_t)b; p[3] = 255;
        }
    }
    x3::rhi::TextureHandle tex = device.createTexture(tpx.data(), TW, TH, /*srgb=*/true);

    // ---- Sweep the ribbon. Two edge verts per along-step; wound to match the
    // terrain's up-facing convention: with along = tangent and across = up x along,
    // triangles {A,C,B / B,C,D} face +Y. Deck lifted a touch above the carved
    // corridor (which the carve holds flat within halfW) to avoid z-fighting.
    std::vector<x3::rhi::MeshVertex> verts;
    std::vector<uint32_t>            idx;
    const int   steps = std::max(2, (int)(len / 3.0f));   // ~3 m per segment
    const float yLift = 0.16f;
    verts.reserve((size_t)(steps + 1) * 2);
    idx.reserve((size_t)steps * 6);
    for (int i = 0; i <= steps; ++i) {
        const float s = (float)i / (float)steps;
        float c[3], t[2];
        worldFreewaySampleArc(s, c, t);
        // across = up x tangent = (t.z, 0, -t.x); A = -across edge, B = +across edge.
        const float ax = t[1], az = -t[0];
        const float v  = (s * len) / kTileLen;
        x3::rhi::MeshVertex A, B;
        A.pos[0] = c[0] - ax * halfW; A.pos[1] = c[1] + yLift; A.pos[2] = c[2] - az * halfW;
        B.pos[0] = c[0] + ax * halfW; B.pos[1] = c[1] + yLift; B.pos[2] = c[2] + az * halfW;
        A.normal[0] = 0.0f; A.normal[1] = 1.0f; A.normal[2] = 0.0f;
        B.normal[0] = 0.0f; B.normal[1] = 1.0f; B.normal[2] = 0.0f;
        A.uv[0] = 0.0f; A.uv[1] = v;
        B.uv[0] = 1.0f; B.uv[1] = v;
        verts.push_back(A);
        verts.push_back(B);
    }
    for (int i = 0; i < steps; ++i) {
        const uint32_t a = (uint32_t)(i * 2), b = a + 1, cc = a + 2, d = a + 3;
        idx.insert(idx.end(), { a, cc, b,  b, cc, d });
    }
    x3::rhi::MeshHandle mesh = device.createMesh(verts.data(), (uint32_t)verts.size(),
                                                 idx.data(), (uint32_t)idx.size());

    Entity e;
    e.mesh = mesh;
    e.tex  = tex;
    for (int i = 0; i < 4; ++i)  e.baseColor[i] = 1.0f;
    for (int i = 0; i < 16; ++i) e.transform[i] = kIdentity[i];
    e.tag = (uint32_t)Tag::Static;
    e.visible = true;
    const uint32_t id = scene.add(e);
    x3::logInfo("[terrain] freeway ribbon: " + std::to_string(verts.size()) + " verts, " +
                std::to_string(idx.size() / 3) + " tris over " + std::to_string((int)len) + " m");
    return id;
}

// ===========================================================================
// Terrain (B2 fixed grid) — unchanged behavior, now built on the shared free
// helpers. Used by --screenshot-terrain + --test-terrain.
// ===========================================================================
void Terrain::build(Scene& scene, x3::rhi::IRenderDevice& device,
                    x3::phys::IPhysicsWorld& physics, const TerrainConfig& cfg) {
    m_cfg = cfg;
    m_worldMinX = -0.5f * (float)m_cfg.tilesX * m_cfg.tileSize;
    m_worldMinZ = -0.5f * (float)m_cfg.tilesZ * m_cfg.tileSize;

    m_groundTex = makeGroundTexture(device);

    m_tiles.clear();
    m_tiles.resize((size_t)m_cfg.tilesX * m_cfg.tilesZ);

    std::vector<x3::rhi::MeshVertex> verts;
    std::vector<uint32_t>            idx;

    const float green[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

    for (uint32_t gz = 0; gz < m_cfg.tilesZ; ++gz) {
        for (uint32_t gx = 0; gx < m_cfg.tilesX; ++gx) {
            TerrainTile& t = m_tiles[(size_t)gz * m_cfg.tilesX + gx];
            t.gx = (int32_t)gx; t.gz = (int32_t)gz;
            t.originX = m_worldMinX + gx * m_cfg.tileSize;
            t.originZ = m_worldMinZ + gz * m_cfg.tileSize;
            t.centerX = t.originX + m_cfg.tileSize * 0.5f;
            t.centerZ = t.originZ + m_cfg.tileSize * 0.5f;

            for (int l = 0; l < (int)TerrainLod::Count; ++l) {
                buildTileMeshAbs(m_cfg, t.originX, t.originZ, (TerrainLod)l, verts, idx);
                t.lodMesh[l] = device.createMesh(verts.data(), (uint32_t)verts.size(),
                                                 idx.data(), (uint32_t)idx.size());
                if (l == 0) {
                    float lo = 1e30f, hi = -1e30f;
                    for (const auto& vtx : verts) { lo = std::min(lo, vtx.pos[1]); hi = std::max(hi, vtx.pos[1]); }
                    t.minY = lo; t.maxY = hi;
                }
                if (m_lodIndexCount[l] == 0) m_lodIndexCount[l] = (uint32_t)idx.size();
            }

            // Collision: LOD0 top surface only (skirts excluded).
            {
                buildTileMeshAbs(m_cfg, t.originX, t.originZ, TerrainLod::Full, verts, idx);
                const uint32_t quads = (m_cfg.tileVerts - 1);
                const uint32_t vpe   = quads + 1;
                const uint32_t surfVerts = vpe * vpe;
                const uint32_t surfIdx   = quads * quads * 6;
                std::vector<float> cverts;
                cverts.reserve((size_t)surfVerts * 3);
                for (uint32_t i = 0; i < surfVerts; ++i) {
                    cverts.push_back(verts[i].pos[0]);
                    cverts.push_back(verts[i].pos[1]);
                    cverts.push_back(verts[i].pos[2]);
                }
                t.body = physics.addStaticMesh(cverts.data(), surfVerts,
                                               idx.data(), surfIdx);
            }

            Entity e;
            e.mesh = t.lodMesh[(int)TerrainLod::Full];
            e.tex  = m_groundTex;
            for (int i = 0; i < 4; ++i) e.baseColor[i] = green[i];
            for (int i = 0; i < 16; ++i) e.transform[i] = kIdentity[i];
            e.tag = (uint32_t)Tag::Static;
            e.visible = true;
            t.entityId = scene.add(e);
            t.activeLod = TerrainLod::Full;
        }
    }

    x3::logInfo("[terrain] built " + std::to_string(m_tiles.size()) + " tiles (" +
                std::to_string(m_cfg.tilesX) + "x" + std::to_string(m_cfg.tilesZ) +
                ", " + std::to_string((int)m_cfg.tileSize) + " m each); world " +
                std::to_string((int)((float)m_cfg.tilesX * m_cfg.tileSize)) + " x " +
                std::to_string((int)((float)m_cfg.tilesZ * m_cfg.tileSize)) + " m");
}

TerrainLod Terrain::lodForDistance(float dist) const { return lodForDist(m_cfg, dist); }

uint32_t Terrain::updateLod(Scene& scene, float camX, float camZ) {
    uint32_t changed = 0;
    for (auto& t : m_tiles) {
        const float dx = t.centerX - camX, dz = t.centerZ - camZ;
        const float dist = std::sqrt(dx * dx + dz * dz);
        const TerrainLod want = lodForDist(m_cfg, dist);
        if (want != t.activeLod) {
            t.activeLod = want;
            if (t.entityId != kNoLink && t.entityId < scene.size())
                scene.get(t.entityId).mesh = t.lodMesh[(int)want];
            ++changed;
        }
    }
    return changed;
}

const TerrainTile* Terrain::tileAt(uint32_t gx, uint32_t gz) const {
    if (gx >= m_cfg.tilesX || gz >= m_cfg.tilesZ) return nullptr;
    return &m_tiles[(size_t)gz * m_cfg.tilesX + gx];
}

void Terrain::worldBounds(float& minX, float& minZ, float& maxX, float& maxZ) const {
    minX = m_worldMinX; minZ = m_worldMinZ;
    maxX = m_worldMinX + (float)m_cfg.tilesX * m_cfg.tileSize;
    maxZ = m_worldMinZ + (float)m_cfg.tilesZ * m_cfg.tileSize;
}

uint32_t Terrain::activeTriangleCount() const {
    uint32_t tris = 0;
    for (const auto& t : m_tiles)
        tris += m_lodIndexCount[(int)t.activeLod] / 3u;
    return tris;
}

// ===========================================================================
// TerrainStreamer (B3) — camera-centered residency ring.
// ===========================================================================

// Job payload: a pointer back to the streamer + the target tile coords + the
// pre-obtained result buffer the worker fills, then pushes onto m_done.
struct TerrainStreamer::GenJob {
    TerrainStreamer*               self = nullptr;
    int32_t                        gx = 0, gz = 0;
    std::unique_ptr<TileGenResult> result;   // owned by the job until pushed
};

TerrainStreamer::~TerrainStreamer() {
    // If the host forgot to call shutdown(), at least drain in-flight jobs so the
    // worker threads don't write into freed memory. (Resident GPU/physics are the
    // device's/world's problem at that point — they are torn down by the host.)
    if (m_jobs && m_counter) m_jobs->wait(m_counter);
}

int32_t TerrainStreamer::tileFloor(float world) const {
    return (int32_t)std::floor(world / m_cfg.tileSize);
}

TerrainLod TerrainStreamer::lodForDistance(float dist) const { return lodForDist(m_cfg, dist); }

bool TerrainStreamer::focusTileResident(float worldX, float worldZ) const {
    const int32_t tx = (int32_t)std::floor(worldX / m_cfg.tileSize);
    const int32_t tz = (int32_t)std::floor(worldZ / m_cfg.tileSize);
    return m_resident.find(key(tx, tz)) != m_resident.end();
}

// Pure CPU generation: fill a TileGenResult for the tile at (gx,gz). Thread-safe
// (config + coords only). Builds the 3 LOD meshes + the LOD0 collision surface.
void TerrainStreamer::generate(const TerrainConfig& cfg, int32_t gx, int32_t gz,
                               TileGenResult& out) {
    out.gx = gx; out.gz = gz;
    out.originX = gx * cfg.tileSize;
    out.originZ = gz * cfg.tileSize;
    out.centerX = out.originX + cfg.tileSize * 0.5f;
    out.centerZ = out.originZ + cfg.tileSize * 0.5f;

    for (int l = 0; l < (int)TerrainLod::Count; ++l) {
        buildTileMeshAbs(cfg, out.originX, out.originZ, (TerrainLod)l,
                         out.lodVerts[l], out.lodIdx[l]);
    }

    // Collision: LOD0 top surface only (first vpe*vpe verts / quads*quads*6 idx).
    const uint32_t quads = (cfg.tileVerts - 1);
    const uint32_t vpe   = quads + 1;
    const uint32_t surfVerts = vpe * vpe;
    const uint32_t surfIdx   = quads * quads * 6;
    out.collVerts.clear();
    out.collVerts.reserve((size_t)surfVerts * 3);
    float lo = 1e30f, hi = -1e30f;
    const auto& v0 = out.lodVerts[0];
    for (uint32_t i = 0; i < surfVerts; ++i) {
        out.collVerts.push_back(v0[i].pos[0]);
        out.collVerts.push_back(v0[i].pos[1]);
        out.collVerts.push_back(v0[i].pos[2]);
        lo = std::min(lo, v0[i].pos[1]); hi = std::max(hi, v0[i].pos[1]);
    }
    out.collIdx.assign(out.lodIdx[0].begin(), out.lodIdx[0].begin() + surfIdx);
    out.minY = lo; out.maxY = hi;
}

// Worker entry point: generate into the job's result buffer, then hand the buffer
// off to the completion queue. Frees the job payload.
void TerrainStreamer::jobThunk(void* user) {
    GenJob* job = static_cast<GenJob*>(user);
    TerrainStreamer* self = job->self;
    generate(self->m_cfg, job->gx, job->gz, *job->result);
    {
        std::lock_guard<std::mutex> lk(self->m_doneMtx);
        self->m_done.push_back(std::move(job->result));
    }
    delete job;
}

std::unique_ptr<TerrainStreamer::TileGenResult> TerrainStreamer::obtainResult() {
    std::lock_guard<std::mutex> lk(m_poolMtx);
    if (!m_resultPool.empty()) {
        auto r = std::move(m_resultPool.back());
        m_resultPool.pop_back();
        return r;
    }
    return std::make_unique<TileGenResult>();
}

void TerrainStreamer::recycleResult(std::unique_ptr<TileGenResult> r) {
    if (!r) return;
    // Keep capacity, drop length: clear vectors but don't shrink (steady-state
    // reuse => no heap churn). Cap the pool so it can't grow unbounded.
    for (int l = 0; l < (int)TerrainLod::Count; ++l) { r->lodVerts[l].clear(); r->lodIdx[l].clear(); }
    r->collVerts.clear(); r->collIdx.clear();
    std::lock_guard<std::mutex> lk(m_poolMtx);
    if (m_resultPool.size() < 64) m_resultPool.push_back(std::move(r));
}

void TerrainStreamer::requestTile(int32_t gx, int32_t gz, bool synchronous) {
    const uint64_t k = key(gx, gz);
    if (m_resident.count(k) || m_pending.count(k)) return;   // already have / coming

    if (synchronous || !m_jobs) {
        // Generate inline now (used for the under-player neighborhood + the no-job
        // headless mode). The upload still happens on the main thread in update().
        // Counted as in-flight (an outstanding result to drain) so the drain's
        // per-result --m_inFlight stays balanced across sync + async alike.
        auto r = obtainResult();
        generate(m_cfg, gx, gz, *r);
        {
            std::lock_guard<std::mutex> lk(m_doneMtx);
            m_done.push_back(std::move(r));
        }
        m_pending[k] = 1;
        ++m_inFlight;
        return;
    }

    // Async: hand the heavy work to a worker. Cap in-flight to bound memory.
    if (m_inFlight >= m_maxInFlight) return;   // try again next frame (re-requested)
    GenJob* job = new GenJob();
    job->self = this; job->gx = gx; job->gz = gz;
    job->result = obtainResult();
    m_pending[k] = 1;
    ++m_inFlight;
    // runIO: tile gen is a chunky, latency-tolerant batch; the I/O lane keeps the
    // compute workers free for parallelFor-heavy frame work. (run() would also be
    // correct.) Priority handled by enqueue order (nearest-first).
    m_jobs->runIO(&jobThunk, job, m_counter);
}

void TerrainStreamer::upload(Scene& scene, x3::rhi::IRenderDevice& device,
                             x3::phys::IPhysicsWorld& physics, TileGenResult& r) {
    auto tile = std::make_unique<TerrainTile>();
    TerrainTile& t = *tile;
    t.gx = r.gx; t.gz = r.gz;
    t.originX = r.originX; t.originZ = r.originZ;
    t.centerX = r.centerX; t.centerZ = r.centerZ;
    t.minY = r.minY; t.maxY = r.maxY;

    for (int l = 0; l < (int)TerrainLod::Count; ++l) {
        t.lodMesh[l] = device.createMesh(r.lodVerts[l].data(), (uint32_t)r.lodVerts[l].size(),
                                         r.lodIdx[l].data(), (uint32_t)r.lodIdx[l].size());
        if (m_lodIndexCount[l] == 0) m_lodIndexCount[l] = (uint32_t)r.lodIdx[l].size();
    }
    t.body = physics.addStaticMesh(r.collVerts.data(), (uint32_t)(r.collVerts.size() / 3),
                                   r.collIdx.data(), (uint32_t)r.collIdx.size());

    // Scene entity (reuse an evicted slot if available; else append).
    Entity e;
    e.mesh = t.lodMesh[(int)TerrainLod::Full];
    e.tex  = m_groundTex;
    for (int i = 0; i < 4; ++i) e.baseColor[i] = 1.0f;
    for (int i = 0; i < 16; ++i) e.transform[i] = kIdentity[i];
    e.tag = (uint32_t)Tag::Static;
    e.visible = true;
    if (!m_freeEntities.empty()) {
        uint32_t id = m_freeEntities.back(); m_freeEntities.pop_back();
        // Recycle through the Scene so the slot's generation counter advances
        // (netcode Phase 0, §4.1): any SceneHandle held against this evicted slot
        // becomes stale instead of silently aliasing the new tile. Equivalent to
        // the old `scene.get(id) = e` for the legacy uint32_t-id path.
        scene.recycle(id, e);
        t.entityId = id;
    } else {
        t.entityId = scene.add(e);
    }
    t.activeLod = TerrainLod::Full;

    ++m_tilesCreated;
    m_resident[key(t.gx, t.gz)] = std::move(tile);
}

void TerrainStreamer::evict(Scene& scene, x3::rhi::IRenderDevice& device,
                            x3::phys::IPhysicsWorld& physics, TerrainTile& t) {
    for (int l = 0; l < (int)TerrainLod::Count; ++l)
        if (t.lodMesh[l].valid()) device.destroyMesh(t.lodMesh[l]);
    if (t.body.valid()) physics.removeBody(t.body);
    if (t.entityId != kNoLink && t.entityId < scene.size()) {
        Entity& e = scene.get(t.entityId);
        e.visible = false;
        e.mesh = x3::rhi::MeshHandle{};   // not drawn until reused
        m_freeEntities.push_back(t.entityId);
    }
    ++m_tilesDestroyed;
}

void TerrainStreamer::init(Scene& scene, x3::rhi::IRenderDevice& device,
                           x3::phys::IPhysicsWorld& physics, x3::jobs::IJobSystem* jobs,
                           const TerrainConfig& cfg, float focusX, float focusZ, int radius) {
    m_cfg = cfg;
    m_jobs = jobs;
    m_radius = radius;
    m_groundTex = makeGroundTexture(device);
    if (m_jobs) m_counter = m_jobs->allocCounter();

    const int32_t ctx = tileFloor(focusX);
    const int32_t ctz = tileFloor(focusZ);

    // Generate the immediate 3x3 neighborhood SYNCHRONOUSLY (collision present
    // before the player can move) and upload it now.
    for (int dz = -1; dz <= 1; ++dz)
        for (int dx = -1; dx <= 1; ++dx)
            requestTile(ctx + dx, ctz + dz, /*synchronous=*/true);

    // Drain the synchronous results immediately (no per-frame budget at init).
    std::vector<std::unique_ptr<TileGenResult>> initial;
    { std::lock_guard<std::mutex> lk(m_doneMtx); initial.swap(m_done); }
    for (auto& r : initial) {
        m_pending.erase(key(r->gx, r->gz));
        if (m_inFlight > 0) --m_inFlight;
        upload(scene, device, physics, *r);
        recycleResult(std::move(r));
    }

    m_lastFocusTX = ctx; m_lastFocusTZ = ctz;

    x3::logInfo("[stream] init at tile (" + std::to_string(ctx) + "," + std::to_string(ctz) +
                ") radius=" + std::to_string(m_radius) + " (max resident " +
                std::to_string(maxResidentForRadius()) + " tiles); jobs=" +
                (m_jobs ? "async" : "synchronous"));
}

uint32_t TerrainStreamer::update(Scene& scene, x3::rhi::IRenderDevice& device,
                                 x3::phys::IPhysicsWorld& physics,
                                 float focusX, float focusZ) {
    const int32_t ctx = tileFloor(focusX);
    const int32_t ctz = tileFloor(focusZ);

    // 1) Boundary cross => recompute residency. Stream out tiles outside radius;
    //    request stream-in for in-range tiles nearest-first. Only do the full
    //    scan when the focus tile actually changed (no per-frame churn otherwise).
    const bool crossed = (ctx != m_lastFocusTX || ctz != m_lastFocusTZ);
    if (crossed) {
        // ---- Stream OUT: evict resident tiles now outside the (Chebyshev) ring.
        // Use a small reused scratch vector of keys to avoid mutating the map
        // while iterating.
        static thread_local std::vector<uint64_t> evictKeys;
        evictKeys.clear();
        for (auto& kv : m_resident) {
            const TerrainTile& t = *kv.second;
            const int32_t ddx = std::abs(t.gx - ctx);
            const int32_t ddz = std::abs(t.gz - ctz);
            if (ddx > m_radius || ddz > m_radius) evictKeys.push_back(kv.first);
        }
        for (uint64_t k : evictKeys) {
            auto it = m_resident.find(k);
            if (it != m_resident.end()) {
                evict(scene, device, physics, *it->second);
                m_resident.erase(it);
            }
        }
        // Also drop pending requests that are now far out of range so they don't
        // re-arm forever (they will be regenerated if approached again).
        for (auto it = m_pending.begin(); it != m_pending.end(); ) {
            const int32_t gx = (int32_t)(uint32_t)(it->first >> 32);
            const int32_t gz = (int32_t)(uint32_t)(it->first & 0xFFFFFFFFu);
            if (std::abs(gx - ctx) > m_radius + 1 || std::abs(gz - ctz) > m_radius + 1)
                it = m_pending.erase(it);
            else ++it;
        }

        // ---- Stream IN: request all in-range tiles nearest-first. The
        // under-player 3x3 is forced SYNCHRONOUS so collision is never missing
        // after a boundary cross; the rest go async (budgeted by m_maxInFlight).
        // Build the ring offset list in expanding rings (already nearest-first).
        for (int ring = 0; ring <= m_radius; ++ring) {
            for (int dz = -ring; dz <= ring; ++dz) {
                for (int dx = -ring; dx <= ring; ++dx) {
                    if (std::max(std::abs(dx), std::abs(dz)) != ring) continue; // shell only
                    const bool underPlayer = (std::abs(dx) <= 1 && std::abs(dz) <= 1);
                    requestTile(ctx + dx, ctz + dz, /*synchronous=*/underPlayer);
                }
            }
        }

        m_lastFocusTX = ctx; m_lastFocusTZ = ctz;
    }

    // 2) Drain completed gen results — BUDGETED per frame to avoid an upload spike.
    //    Pull freshly-finished results off the (job-pushed) completion queue and
    //    fold in last frame's over-budget carry-over. Each FRESH result decrements
    //    m_inFlight exactly once (it is no longer outstanding async work) and is
    //    removed from m_pending (it has arrived). Anything we can't upload this
    //    frame for budget reasons is carried over in m_deferred (already counted
    //    down + still tracked so it is neither re-requested nor re-counted).
    uint32_t uploaded = 0;
    std::vector<std::unique_ptr<TileGenResult>> ready;
    { std::lock_guard<std::mutex> lk(m_doneMtx); ready.swap(m_done); }
    // Each FRESH arrival decrements m_inFlight exactly once (no longer outstanding
    // async work). It STAYS in m_pending until terminal disposition (uploaded or
    // dropped) so a still-deferred tile is never re-requested.
    {
        const uint32_t arrived = (uint32_t)ready.size();
        m_inFlight = (m_inFlight > arrived) ? (m_inFlight - arrived) : 0u;
    }

    // Process carry-over first (oldest), then this frame's fresh arrivals.
    std::vector<std::unique_ptr<TileGenResult>> work;
    work.reserve(m_deferred.size() + ready.size());
    for (auto& r : m_deferred) work.push_back(std::move(r));
    m_deferred.clear();
    for (auto& r : ready)      work.push_back(std::move(r));

    for (auto& r : work) {
        const uint64_t k = key(r->gx, r->gz);
        const int32_t ddx = std::abs(r->gx - ctx), ddz = std::abs(r->gz - ctz);
        const bool inRange = (ddx <= m_radius && ddz <= m_radius);
        const bool alreadyResident = m_resident.count(k) != 0;
        // The under-player 3x3 neighborhood bypasses the per-frame budget: its
        // collision MUST be present this frame so the player can never fall
        // through. It is at most 9 tiles, only right after a boundary cross.
        const bool underPlayer = (ddx <= 1 && ddz <= 1);
        if (inRange && !alreadyResident && (underPlayer || uploaded < m_maxUploadsPerFrame)) {
            m_pending.erase(k);            // terminal: now resident
            upload(scene, device, physics, *r);
            ++uploaded;
            recycleResult(std::move(r));
        } else if (inRange && !alreadyResident) {
            // Over budget this frame: carry to next frame (still pending, so it is
            // not re-requested; main-thread-only list, no lock, no double count).
            m_deferred.push_back(std::move(r));
        } else {
            m_pending.erase(k);            // terminal: stale / out-of-range / dup
            recycleResult(std::move(r));
        }
    }

    // 3) LOD on resident tiles by distance (cheap: pre-uploaded meshes).
    for (auto& kv : m_resident) {
        TerrainTile& t = *kv.second;
        const float dx = t.centerX - focusX, dz = t.centerZ - focusZ;
        const float dist = std::sqrt(dx * dx + dz * dz);
        const TerrainLod want = lodForDist(m_cfg, dist);
        if (want != t.activeLod) {
            t.activeLod = want;
            if (t.entityId != kNoLink && t.entityId < scene.size())
                scene.get(t.entityId).mesh = t.lodMesh[(int)want];
        }
    }

    return uploaded;
}

void TerrainStreamer::shutdown(Scene& scene, x3::rhi::IRenderDevice& device,
                               x3::phys::IPhysicsWorld& physics) {
    // Drain in-flight jobs so no worker writes after we free.
    if (m_jobs && m_counter) m_jobs->wait(m_counter);
    m_inFlight = 0;

    // Discard any undrained completion results (they were never uploaded, so no
    // GPU/physics leak — just buffers).
    { std::lock_guard<std::mutex> lk(m_doneMtx); m_done.clear(); }
    m_deferred.clear();
    m_pending.clear();

    // Destroy every resident tile's GPU + physics resources.
    for (auto& kv : m_resident)
        evict(scene, device, physics, *kv.second);
    m_resident.clear();
    m_freeEntities.clear();

    // Detach from the job system: after an explicit shutdown the caller may free
    // the job system (which frees our Counter) before our destructor runs, so the
    // dtor's defensive wait() must NOT touch m_counter again. We have already
    // wait()ed above, so dropping the pointers here is safe + makes ~ a no-op.
    m_jobs = nullptr;
    m_counter = nullptr;

    x3::logInfo("[stream] shutdown: created=" + std::to_string(m_tilesCreated) +
                " destroyed=" + std::to_string(m_tilesDestroyed) +
                (m_tilesCreated == m_tilesDestroyed ? " (no leak)" : " (LEAK!)"));
}

// ===========================================================================
// Headless self-test (--test-terrain). T1 character settles, T2 heightAt matches
// the settled surface, T3 LOD coarsens with distance. No window/Vulkan.
// ===========================================================================
namespace {

int g_pass = 0, g_fail = 0;
void check(bool cond, const char* name) {
    if (cond) { ++g_pass; x3::logInfo(std::string("[terrain-test] PASS ") + name); }
    else      { ++g_fail; x3::logError(std::string("[terrain-test] FAIL ") + name); }
}
int gs_pass = 0, gs_fail = 0;
void checkS(bool cond, const char* name) {
    if (cond) { ++gs_pass; x3::logInfo(std::string("[stream-test] PASS ") + name); }
    else      { ++gs_fail; x3::logError(std::string("[stream-test] FAIL ") + name); }
}

// Headless IRenderDevice for the streaming test. The shared no-op base
// (app/headless_device.h) hands out monotonically-increasing valid handles so
// build()/upload() run with no Vulkan; this local subclass additionally COUNTS
// mesh create/destroy so the streaming test can assert no GPU mesh leak. It
// keeps the base's exact incrementing-handle behavior (m_next is protected).
class HeadlessDevice final : public x3::game::HeadlessRenderDevice {
public:
    x3::rhi::MeshHandle createMesh(const x3::rhi::MeshVertex*, uint32_t,
                                   const uint32_t*, uint32_t) override {
        ++meshesCreated; return x3::rhi::MeshHandle{ m_next++ };
    }
    void destroyMesh(x3::rhi::MeshHandle h) override { if (h.valid()) ++meshesDestroyed; }
    uint64_t meshesCreated = 0, meshesDestroyed = 0;
};

} // namespace

bool runTerrainSelfTest() {
    g_pass = g_fail = 0;

    std::unique_ptr<x3::phys::IPhysicsWorld> physics(x3::phys::createPhysicsWorld());
    physics->init();
    HeadlessDevice device;
    Scene scene;

    Terrain terrain;
    TerrainConfig cfg;
    cfg.tilesX = 4; cfg.tilesZ = 4; cfg.tileSize = 32.0f; cfg.tileVerts = 33;
    cfg.heightScale = 24.0f; cfg.seed = 4242u;
    terrain.build(scene, device, *physics, cfg);

    // ---- T1: a character dropped over the terrain SETTLES on the surface -----
    {
        const float dropX = 0.0f, dropZ = 0.0f;
        const float surfY = terrain.heightAt(dropX, dropZ);
        x3::phys::BodyId ch = physics->createCharacter(0.35f, 1.8f,
            x3::phys::Vec3{ dropX, surfY + 3.0f, dropZ });
        const float dt = 1.0f / 60.0f;
        for (int i = 0; i < 200; ++i) {
            physics->moveCharacter(ch, x3::phys::Vec3{ 0, 0, 0 }, dt);
            physics->step(dt);
        }
        const x3::phys::Vec3 feet = physics->getBodyPosition(ch);
        const float expected = terrain.heightAt(feet.x, feet.z);
        const float err = std::fabs(feet.y - expected);
        const bool settled = (err < 0.6f);
        const bool notFallenThrough = (feet.y > expected - 1.0f);
        const bool notFloating = (feet.y < surfY + 2.0f);
        if (!(settled && notFallenThrough && notFloating)) {
            x3::logError("[terrain-test] settle: feetY=" + std::to_string(feet.y) +
                         " expected=" + std::to_string(expected) +
                         " dropTop=" + std::to_string(surfY + 3.0f));
        }
        check(settled && notFallenThrough && notFloating,
              "T1 character settles on terrain surface");
    }

    // ---- T2: heightAt() bounded + varied -------------------------------------
    {
        float lo = 1e30f, hi = -1e30f;
        for (int i = 0; i < 64; ++i) {
            float x = (float)(i % 8) * 16.0f - 56.0f;
            float z = (float)(i / 8) * 16.0f - 56.0f;
            float h = terrain.heightAt(x, z);
            lo = std::min(lo, h); hi = std::max(hi, h);
        }
        const bool inRange = (lo >= -0.001f && hi <= cfg.heightScale + 0.001f);
        const bool varies  = (hi - lo) > 1.0f;
        check(inRange && varies, "T2 heightAt bounded + varied (rolling hills)");
    }

    // ---- T3: LOD coarsens with distance from the camera ----------------------
    {
        const TerrainLod lNear = terrain.lodForDistance(cfg.tileSize * 1.0f);
        const TerrainLod lMid  = terrain.lodForDistance(cfg.tileSize * 4.0f);
        const TerrainLod lFar  = terrain.lodForDistance(cfg.tileSize * 20.0f);
        const bool ordered = (lNear == TerrainLod::Full) &&
                             ((uint8_t)lMid > (uint8_t)lNear) &&
                             ((uint8_t)lFar > (uint8_t)lMid);

        float minX, minZ, maxX, maxZ; terrain.worldBounds(minX, minZ, maxX, maxZ);
        terrain.updateLod(scene, minX, minZ);
        const TerrainTile* nearTile = terrain.tileAt(0, 0);
        const TerrainTile* farTile  = terrain.tileAt(cfg.tilesX - 1, cfg.tilesZ - 1);
        const bool applied = nearTile && farTile &&
            nearTile->activeLod == TerrainLod::Full &&
            (uint8_t)farTile->activeLod > (uint8_t)nearTile->activeLod;
        const uint32_t activeTris = terrain.activeTriangleCount();
        terrain.updateLod(scene, nearTile->centerX, nearTile->centerZ);
        const bool savesTris = activeTris < terrain.activeTriangleCount() + 1;
        check(ordered && applied && savesTris,
              "T3 LOD coarsens with distance (near=Full, far=Quarter)");
    }

    // ======================= WORLD TOPOGRAPHY (Phase 1) ======================
    // These exercise the CANONICAL world config (worldTerrainConfig — mountains +
    // hills + flat pads + the graded freeway), which the legacy T1..T3 cfg above
    // deliberately does NOT enable.
    const TerrainConfig& W = worldTerrainConfig();

    // ---- T4: heightfield sampling is DETERMINISTIC (bit-exact on re-sample) ---
    {
        bool exact = true;
        const float pts[][2] = { {0,0},{123,-77},{900,300},{-500,700},{-333,512},{1600,-1200} };
        for (auto& p : pts) {
            const float a = terrainHeightAt(W, p[0], p[1]);
            const float b = terrainHeightAt(W, p[0], p[1]);
            if (a != b) exact = false;
        }
        check(exact, "T4 heightfield sampling deterministic (bit-exact re-sample)");
    }

    // ---- T5: FLAT PADS hold — every point inside a pad radius is DEAD LEVEL ----
    {
        bool flat = true, risesBeyond = false; float worstErr = 0.0f;
        for (uint32_t i = 0; i < worldFlatPadCount(); ++i) {
            float cx, cz, r, y; worldFlatPad(i, cx, cz, r, y);
            for (int a = 0; a < 24; ++a) {
                const float ang = (float)a / 24.0f * 6.2831853f;
                for (float rr = 0.0f; rr <= r * 0.9f; rr += r * 0.3f) {
                    const float x = cx + std::cos(ang) * rr, z = cz + std::sin(ang) * rr;
                    const float e = std::fabs(terrainHeightAt(W, x, z) - y);
                    worstErr = std::max(worstErr, e);
                    if (e > 0.02f) flat = false;
                }
            }
            // Somewhere well beyond the pad the ground must differ (pad is a
            // clearing IN the terrain, not the whole world going flat).
            if (std::fabs(terrainHeightAt(W, cx + r + 400.0f, cz) - y) > 1.0f) risesBeyond = true;
        }
        if (!flat) x3::logError("[terrain-test] flat-pad worst err=" + std::to_string(worstErr));
        check(flat && risesBeyond, "T5 authored flat pads are dead-level (tower + city sites)");
    }

    // ---- T6: FREEWAY centerline respects the ~6% grade limit end-to-end -------
    {
        bool gradeOk = true; float worstGrade = 0.0f;
        const int N = 400;
        float prev[3]; float prevT[2];
        worldFreewaySampleArc(0.0f, prev, prevT);
        for (int i = 1; i <= N; ++i) {
            float c[3], t[2];
            worldFreewaySampleArc((float)i / (float)N, c, t);
            const float dxz = std::sqrt((c[0]-prev[0])*(c[0]-prev[0]) + (c[2]-prev[2])*(c[2]-prev[2]));
            if (dxz > 1e-4f) {
                const float grade = std::fabs(c[1] - prev[1]) / dxz;
                worstGrade = std::max(worstGrade, grade);
                if (grade > topo::kMaxGrade + 0.005f) gradeOk = false;
            }
            prev[0]=c[0]; prev[1]=c[1]; prev[2]=c[2];
        }
        x3::logInfo("[terrain-test] freeway worst grade = " +
                    std::to_string(worstGrade * 100.0f) + "% (limit " +
                    std::to_string(topo::kMaxGrade * 100.0f) + "%), length " +
                    std::to_string((int)worldFreewayLength()) + " m");
        check(gradeOk, "T6 freeway grade within the 6% design limit end-to-end");
    }

    // ---- T7: FREEWAY carve — the deck is FLAT across its width (cut/fill), and
    //          the corridor actually reshapes the natural ground (cut or fill). --
    {
        bool deckFlat = true, movedEarth = false; float worstCross = 0.0f;
        const float hw = worldFreewayHalfWidth();
        for (int i = 1; i < 10; ++i) {
            float c[3], tg[2];
            worldFreewaySampleArc((float)i / 10.0f, c, tg);
            const float px = -tg[1], pz = tg[0];        // perpendicular (unit)
            // Deck must sit at the centerline elevation across the full width.
            for (float o = -hw; o <= hw; o += hw * 0.5f) {
                const float e = std::fabs(terrainHeightAt(W, c[0]+px*o, c[2]+pz*o) - c[1]);
                worstCross = std::max(worstCross, e);
                if (e > 0.30f) deckFlat = false;
            }
            // Natural ground well off the corridor differs from the deck => the
            // carve cut a hill or filled a dip here.
            const float far = terrainHeightAt(W, c[0]+px*(hw+topo::kShoulder+45.0f),
                                                  c[2]+pz*(hw+topo::kShoulder+45.0f));
            if (std::fabs(far - c[1]) > 0.75f) movedEarth = true;
        }
        if (!deckFlat) x3::logError("[terrain-test] deck cross worst=" + std::to_string(worstCross));
        check(deckFlat && movedEarth, "T7 freeway deck flat across width; corridor cuts/fills terrain");
    }

    // ---- T8/T9: physics collision built from the topography MATCHES the rendered
    //      surface (raycast Y == sampler Y), and tile residency creates/destroys
    //      bodies+meshes with NO LEAK (allocation stability). ------------------
    {
        HeadlessDevice wdev;
        std::unique_ptr<x3::phys::IPhysicsWorld> wphys(x3::phys::createPhysicsWorld());
        wphys->init();
        Scene wscene;
        TerrainStreamer wstream;
        const float fX = 900.0f, fZ = 300.0f;          // varied (mountain) terrain
        wstream.setUploadBudget(512);
        wstream.init(wscene, wdev, *wphys, /*jobs=*/nullptr, W, fX, fZ, /*radius=*/2);
        for (int i = 0; i < 8; ++i) wstream.update(wscene, wdev, *wphys, fX, fZ);
        wphys->optimizeBroadphase();
        wphys->step(1.0f / 60.0f);

        // Collision == render: raycast straight down onto the streamed collision
        // at integer-meter (grid-vertex) points and compare to the sampler.
        int hits = 0, matched = 0; float worst = 0.0f;
        for (int dz = -24; dz <= 24; dz += 8)
            for (int dx = -24; dx <= 24; dx += 8) {
                const float x = fX + (float)dx, z = fZ + (float)dz;
                const float expected = terrainHeightAtWorld(x, z);
                x3::phys::RayHit hit = wphys->rayCast(
                    x3::phys::Vec3{ x, W.heightScale + 60.0f, z },
                    x3::phys::Vec3{ 0.0f, -1.0f, 0.0f }, W.heightScale + 200.0f,
                    x3::phys::Layer::Static);
                if (hit.hit) {
                    ++hits;
                    const float e = std::fabs(hit.point.y - expected);
                    worst = std::max(worst, e);
                    if (e < 0.35f) ++matched;
                }
            }
        x3::logInfo("[terrain-test] collision-vs-render: " + std::to_string(matched) + "/" +
                    std::to_string(hits) + " within 0.35 m (worst " + std::to_string(worst) +
                    " m), resident=" + std::to_string(wstream.residentCount()));
        check(hits >= 40 && matched == hits,
              "T8 physics collision height matches rendered surface (raycast == sampler)");

        // Residency lifecycle: teardown destroys everything it created (no leak).
        const uint64_t mc = wdev.meshesCreated, md0 = wdev.meshesDestroyed;
        wstream.shutdown(wscene, wdev, *wphys);
        const bool tileBal = wstream.tilesCreated() == wstream.tilesDestroyed();
        const bool meshBal = wdev.meshesCreated == wdev.meshesDestroyed;
        x3::logInfo("[terrain-test] residency: tiles c/d=" +
                    std::to_string(wstream.tilesCreated()) + "/" + std::to_string(wstream.tilesDestroyed()) +
                    " meshes c/d=" + std::to_string(wdev.meshesCreated) + "/" +
                    std::to_string(wdev.meshesDestroyed) + " (created " + std::to_string(mc) +
                    ", destroyed-at-start " + std::to_string(md0) + ")");
        check(tileBal && meshBal, "T9 topography tile residency creates/destroys bodies+meshes (no leak)");
        wphys->shutdown();
    }

    physics->shutdown();
    x3::logInfo(std::string("[terrain-test] ") + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

// ===========================================================================
// Headless placement-API self-test (--test-terrainplace). Verifies the public
// building/pad anchoring helpers the 14900k uses agree with the underlying
// procedural surface. Pure math — no window/Vulkan/physics/jobs.
//   P1 terrainHeightAtWorld == terrainHeightAt(worldTerrainConfig(), …) exactly.
//   P2 placeOnTerrain yields {x, height, z} (Y == surface height) at the point.
//   P3 terrainNormalAtWorld is unit-length and points generally +Y everywhere.
//   P4 normal tilts INTO an uphill slope (sanity: not always straight up).
// ===========================================================================
bool runTerrainPlaceSelfTest() {
    int pPass = 0, pFail = 0;
    auto checkP = [&](bool cond, const char* name) {
        if (cond) { ++pPass; x3::logInfo(std::string("[place-test] PASS ") + name); }
        else      { ++pFail; x3::logError(std::string("[place-test] FAIL ") + name); }
    };

    const TerrainConfig& cfg = worldTerrainConfig();

    // A spread of sample points (origin, the spawn neighborhood, far unbounded
    // coords incl. negatives) so we cover the whole field, not just one tile.
    const float pts[][2] = {
        {   0.0f,    0.0f }, {  17.0f,  -9.0f }, { -123.0f,  88.0f },
        { 512.0f,  333.0f }, { -777.0f, -41.0f }, {  31.5f,  31.5f },
        { 1024.0f, -2048.0f }, { -3.3f, 600.1f },
    };
    const int nPts = (int)(sizeof(pts) / sizeof(pts[0]));

    // ---- P1: convenience height == raw sampler on the canonical config --------
    {
        bool allMatch = true;
        float maxErr = 0.0f;
        for (int i = 0; i < nPts; ++i) {
            const float a = terrainHeightAtWorld(pts[i][0], pts[i][1]);
            const float b = terrainHeightAt(cfg, pts[i][0], pts[i][1]);
            const float e = std::fabs(a - b);
            maxErr = std::max(maxErr, e);
            if (e != 0.0f) allMatch = false;   // same code path => bit-exact
        }
        if (!allMatch)
            x3::logError("[place-test] height mismatch maxErr=" + std::to_string(maxErr));
        checkP(allMatch, "P1 terrainHeightAtWorld == terrainHeightAt(worldTerrainConfig(),...)");
    }

    // ---- P2: placeOnTerrain sits exactly ON the surface (Y == height) ---------
    {
        bool onSurface = true;
        for (int i = 0; i < nPts; ++i) {
            float pos[3];
            placeOnTerrain(pts[i][0], pts[i][1], pos);
            const float h = terrainHeightAtWorld(pts[i][0], pts[i][1]);
            if (pos[0] != pts[i][0] || pos[2] != pts[i][1] || pos[1] != h) onSurface = false;
        }
        checkP(onSurface, "P2 placeOnTerrain => {x, surfaceY, z} (Y == surface height)");
    }

    // ---- P3: normal is unit-length and points generally +Y --------------------
    {
        bool unit = true, upward = true;
        float worstLen = 0.0f, minNy = 1.0f;
        for (int i = 0; i < nPts; ++i) {
            float n[3];
            terrainNormalAtWorld(pts[i][0], pts[i][1], n);
            const float len = std::sqrt(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
            worstLen = std::max(worstLen, std::fabs(len - 1.0f));
            minNy = std::min(minNy, n[1]);
            if (std::fabs(len - 1.0f) > 1e-4f) unit = false;
            if (n[1] <= 0.0f) upward = false;          // generally +Y everywhere
        }
        if (!unit || !upward)
            x3::logError("[place-test] normal: worst|len-1|=" + std::to_string(worstLen) +
                         " minNy=" + std::to_string(minNy));
        checkP(unit && upward, "P3 terrainNormalAtWorld unit-length + points +Y");
    }

    // ---- P4: normal actually tilts on a slope (not the trivial straight-up) ---
    // Scan for a point with a non-trivial gradient and assert its normal leans
    // toward the DOWNHILL direction (n.xz is the negative of the height gradient).
    {
        bool sawSlope = false, leansRight = true;
        for (int i = 0; i < nPts && !sawSlope; ++i) {
            const float x = pts[i][0], z = pts[i][1];
            const float eps = cfg.tileSize / (float)(cfg.tileVerts - 1) * 0.5f;
            const float gx = (terrainHeightAt(cfg, x + eps, z) - terrainHeightAt(cfg, x - eps, z));
            const float gz = (terrainHeightAt(cfg, x, z + eps) - terrainHeightAt(cfg, x, z - eps));
            if (std::fabs(gx) + std::fabs(gz) > 0.05f) {
                sawSlope = true;
                float n[3];
                terrainNormalAtWorld(x, z, n);
                // n.x should oppose the +x gradient; n.z oppose the +z gradient.
                if (gx * n[0] > 1e-6f || gz * n[2] > 1e-6f) leansRight = false;
            }
        }
        checkP(sawSlope && leansRight, "P4 normal tilts down-slope on a gradient");
    }

    x3::logInfo(std::string("[place-test] ") + std::to_string(pPass) + " passed, " +
                std::to_string(pFail) + " failed");
    return pFail == 0;
}

// ===========================================================================
// Headless streaming self-test (--test-streaming). Drives a focus point on a
// long path across the unbounded world; runs the REAL async job pipeline so the
// thread hand-off + completion drain are exercised. Asserts the streaming
// invariants. No window/Vulkan.
// ===========================================================================
bool runStreamingSelfTest() {
    gs_pass = gs_fail = 0;

    std::unique_ptr<x3::jobs::IJobSystem> jobs(x3::jobs::createJobSystem());
    jobs->init(0);

    std::unique_ptr<x3::phys::IPhysicsWorld> physics(x3::phys::createPhysicsWorld());
    physics->init();
    HeadlessDevice device;
    Scene scene;

    TerrainConfig cfg;
    cfg.tileSize = 32.0f; cfg.tileVerts = 33; cfg.heightScale = 40.0f; cfg.seed = 9001u;

    const int radius = 5;
    TerrainStreamer streamer;
    const float startX = 0.0f, startZ = 0.0f;
    streamer.init(scene, device, *physics, jobs.get(), cfg, startX, startZ, radius);

    // Drop a character onto the start tile and confirm it settles before we move.
    const float spawnY = streamer.heightAt(startX, startZ) + 3.0f;
    x3::phys::BodyId ch = physics->createCharacter(0.35f, 1.8f,
        x3::phys::Vec3{ startX, spawnY, startZ });
    const float dt = 1.0f / 60.0f;

    const uint32_t maxResident = streamer.maxResidentForRadius();

    // March the focus a long way along +X (and weaving in Z) at a brisk walk
    // speed, ticking the streamer + physics every "frame". Settle the streamer a
    // few frames between steps so async uploads catch up (still budgeted).
    const float speed = 7.0f;                  // m/s focus travel
    const int   frames = 4000;                 // ~66 s of travel
    float fx = startX, fz = startZ;

    uint32_t peakResident = 0;
    int      surfaceContactFrames = 0, surfaceCheckFrames = 0;
    bool     everUnloaded = false, everLoadedAhead = false;
    bool     focusAlwaysCovered = true;
    int32_t  startTileX = (int32_t)std::floor(startX / cfg.tileSize);
    float    maxDistTraveled = 0.0f;
    // Frame-time stability: time the MAIN-THREAD streamer.update() each frame
    // (the only steady hot-path streaming cost — the heavy gen is on jobs). The
    // max must stay tiny (no hitch on a boundary-cross stream in/out spike).
    double   sumUpdMs = 0.0, maxUpdMs = 0.0;
    double   sumNonCrossMs = 0.0; int nonCrossFrames = 0;

    // Move the physics character toward +X using the controller so it walks the
    // streamed collision (must never fall through). We keep the focus = character
    // XZ so collision under the focus is always the under-player neighborhood.
    for (int f = 0; f < frames; ++f) {
        // Walk the character forward (+X) with a slight Z weave.
        const float vz = 2.0f * std::sin((float)f * 0.01f);
        physics->moveCharacter(ch, x3::phys::Vec3{ speed, 0.0f, vz }, dt);
        physics->step(dt);
        const x3::phys::Vec3 p = physics->getBodyPosition(ch);
        fx = p.x; fz = p.z;

        // Stream around the character — timed for the hitch check.
        const uint32_t residBefore = streamer.residentCount();
        const auto t0 = std::chrono::high_resolution_clock::now();
        streamer.update(scene, device, *physics, fx, fz);
        const auto t1 = std::chrono::high_resolution_clock::now();
        const double updMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
        sumUpdMs += updMs; maxUpdMs = std::max(maxUpdMs, updMs);
        // A "non-cross" frame is one where residency didn't change (the common
        // steady-state hot path): its update() must be ~free (LOD scan + drain).
        if (streamer.residentCount() == residBefore) { sumNonCrossMs += updMs; ++nonCrossFrames; }

        // Focus tile must be resident (collision present beneath the player).
        if (!streamer.focusTileResident(fx, fz)) focusAlwaysCovered = false;

        // Surface-contact check: the character's feet must stay near the sampled
        // surface (no fall-through). Allow a band for capsule skin + slope.
        const float surf = streamer.heightAt(p.x, p.z);
        ++surfaceCheckFrames;
        if (std::fabs(p.y - surf) < 1.2f) ++surfaceContactFrames;

        peakResident = std::max(peakResident, streamer.residentCount());
        maxDistTraveled = std::max(maxDistTraveled, p.x - startX);

        // Detect load-ahead / unload-behind once we've moved a few tiles.
        const int32_t curTileX = (int32_t)std::floor(p.x / cfg.tileSize);
        if (curTileX - startTileX >= radius + 2) {
            // A tile well ahead of the player should be resident (loaded ahead).
            if (streamer.focusTileResident(p.x + (radius - 1) * cfg.tileSize, p.z))
                everLoadedAhead = true;
            // The start tile should be long gone (unloaded behind).
            if (!streamer.focusTileResident(startX, startZ))
                everUnloaded = true;
        }
    }

    // Let any final in-flight jobs land + upload (no more movement).
    for (int f = 0; f < 60; ++f) {
        physics->moveCharacter(ch, x3::phys::Vec3{ 0, 0, 0 }, dt);
        physics->step(dt);
        const x3::phys::Vec3 p = physics->getBodyPosition(ch);
        streamer.update(scene, device, *physics, p.x, p.z);
    }

    const x3::phys::Vec3 endP = physics->getBodyPosition(ch);

    // ---- S1: resident count stays bounded (does NOT grow with distance) ------
    {
        const bool bounded = (peakResident <= maxResident);
        if (!bounded)
            x3::logError("[stream-test] peakResident=" + std::to_string(peakResident) +
                         " > max=" + std::to_string(maxResident));
        x3::logInfo("[stream-test] resident peak=" + std::to_string(peakResident) +
                    " final=" + std::to_string(streamer.residentCount()) +
                    " max(R=" + std::to_string(radius) + ")=" + std::to_string(maxResident) +
                    " | travelled " + std::to_string((int)maxDistTraveled) + " m (+X)");
        checkS(bounded, "S1 resident tile count bounded (constant w/ distance)");
    }

    // ---- S2: tiles load ahead of + unload behind the player ------------------
    checkS(everLoadedAhead && everUnloaded, "S2 tiles load ahead + unload behind");

    // ---- S3: no fall-through — character stayed on the surface throughout -----
    {
        const float frac = surfaceCheckFrames ? (float)surfaceContactFrames / surfaceCheckFrames : 0.0f;
        const bool onSurface = (frac > 0.97f) && focusAlwaysCovered;
        if (!onSurface)
            x3::logError("[stream-test] surface-contact frac=" + std::to_string(frac) +
                         " focusCovered=" + std::to_string(focusAlwaysCovered ? 1 : 0) +
                         " endY=" + std::to_string(endP.y));
        x3::logInfo("[stream-test] surface contact " + std::to_string((int)(frac * 100.0f)) +
                    "% of " + std::to_string(surfaceCheckFrames) + " frames");
        checkS(onSurface, "S3 character stays on surface (no fall-through)");
    }

    // ---- S4: no leak — every created tile's GPU + physics resource destroyed --
    // Tear down the streamer (evicts all resident), then compare lifetime counters.
    streamer.shutdown(scene, device, *physics);
    {
        const bool tileBalance = (streamer.tilesCreated() == streamer.tilesDestroyed());
        const bool meshBalance = (device.meshesCreated == device.meshesDestroyed);
        if (!tileBalance || !meshBalance)
            x3::logError("[stream-test] leak: tiles c/d=" +
                         std::to_string(streamer.tilesCreated()) + "/" +
                         std::to_string(streamer.tilesDestroyed()) + " meshes c/d=" +
                         std::to_string(device.meshesCreated) + "/" +
                         std::to_string(device.meshesDestroyed));
        x3::logInfo("[stream-test] tiles created=" + std::to_string(streamer.tilesCreated()) +
                    " destroyed=" + std::to_string(streamer.tilesDestroyed()) +
                    " | meshes created=" + std::to_string(device.meshesCreated) +
                    " destroyed=" + std::to_string(device.meshesDestroyed));
        checkS(tileBalance && meshBalance, "S4 no tile/mesh/body leak (created==destroyed)");
    }

    // ---- S5: frame-time stability — no hitch on stream in/out ----------------
    // The heavy per-tile work is on the job system; the main-thread update() only
    // drains a BUDGETED few uploads + scans LOD, so even the worst frame (a
    // boundary cross that streams a whole ring shell + uploads up to the budget)
    // must stay well under a frame budget. We assert the MAX update() over the
    // whole 4000-frame march is small (< 5 ms — generous; observed << that) and
    // that the steady-state (no-residency-change) update is ~free.
    {
        const double avgUpd = (frames > 0) ? sumUpdMs / frames : 0.0;
        const double avgSteady = (nonCrossFrames > 0) ? sumNonCrossMs / nonCrossFrames : 0.0;
        // Robust to CPU contention (concurrent builds/agents spike the single
        // worst frame): the TRUE no-hitch signal is the steady-state average
        // being ~free; the absolute max only needs to rule out a real synchronous
        // stall, so gate it at a generous 30 Hz frame budget.
        const bool noHitch = (maxUpdMs < 33.0) && (avgSteady < 1.0);
        char rb[200];
        std::snprintf(rb, sizeof(rb),
            "[stream-test] update() main-thread cost: avg=%.4f ms max=%.4f ms "
            "(steady-state avg=%.4f ms over %d frames)",
            avgUpd, maxUpdMs, avgSteady, nonCrossFrames);
        x3::logInfo(rb);
        checkS(noHitch, "S5 no frame hitch on stream in/out (steady avg < 1 ms, max < 33 ms)");
    }

    physics->shutdown();
    jobs->shutdown();

    x3::logInfo(std::string("[stream-test] ") + std::to_string(gs_pass) + " passed, " +
                std::to_string(gs_fail) + " failed");
    return gs_fail == 0;
}

} // namespace x3::game
