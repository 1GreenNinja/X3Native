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
#include "asset_root.h"

#include "engine/core/x3_log.h"
#include "engine/physics/IPhysicsWorld.h"
#include "engine/core/IJobSystem.h"

// File-local STB copy for the real terrain splat albedos (same precedent as
// surface_library.cpp / cinematic.cpp: each app TU that decodes PNGs hosts its
// own STATIC instance so there's no symbol clash with the engine's copy).
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#if defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning(disable : 4244 4456 4457)
#endif
#include <stb_image.h>
#if defined(_MSC_VER)
#  pragma warning(pop)
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
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
// 2D distance helpers for the corridor x tile-LOD refinement. Pure, world-space
// — BOTH tiles sharing a border evaluate these on the SAME world coordinates,
// which is what makes the hot/cold decision seam-consistent by construction.
// ---------------------------------------------------------------------------
inline float segPointDist2(float ax, float az, float bx, float bz, float px, float pz) {
    const float abx = bx - ax, abz = bz - az;
    const float len2 = abx * abx + abz * abz;
    float t = (len2 > 1e-12f) ? ((px - ax) * abx + (pz - az) * abz) / len2 : 0.0f;
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    const float dx = px - (ax + abx * t), dz = pz - (az + abz * t);
    return dx * dx + dz * dz;
}
inline float cross2(float ox, float oz, float ax, float az, float bx, float bz) {
    return (ax - ox) * (bz - oz) - (az - oz) * (bx - ox);
}
inline float segSegDist2(float ax, float az, float bx, float bz,
                         float cx, float cz, float dx, float dz) {
    // Proper intersection => distance 0.
    const float d1 = cross2(cx, cz, dx, dz, ax, az);
    const float d2 = cross2(cx, cz, dx, dz, bx, bz);
    const float d3 = cross2(ax, az, bx, bz, cx, cz);
    const float d4 = cross2(ax, az, bx, bz, dx, dz);
    if (((d1 > 0.0f && d2 < 0.0f) || (d1 < 0.0f && d2 > 0.0f)) &&
        ((d3 > 0.0f && d4 < 0.0f) || (d3 < 0.0f && d4 > 0.0f)))
        return 0.0f;
    float best = segPointDist2(ax, az, bx, bz, cx, cz);
    best = std::min(best, segPointDist2(ax, az, bx, bz, dx, dz));
    best = std::min(best, segPointDist2(cx, cz, dx, dz, ax, az));
    best = std::min(best, segPointDist2(cx, cz, dx, dz, bx, bz));
    return best;
}
// Distance^2 from a segment to an axis-aligned rect (0 if it touches/enters).
inline float segRectDist2(float ax, float az, float bx, float bz,
                          float minX, float minZ, float maxX, float maxZ) {
    auto inside = [&](float x, float z) {
        return x >= minX && x <= maxX && z >= minZ && z <= maxZ;
    };
    if (inside(ax, az) || inside(bx, bz)) return 0.0f;
    float best = segSegDist2(ax, az, bx, bz, minX, minZ, maxX, minZ);
    best = std::min(best, segSegDist2(ax, az, bx, bz, maxX, minZ, maxX, maxZ));
    best = std::min(best, segSegDist2(ax, az, bx, bz, maxX, maxZ, minX, maxZ));
    best = std::min(best, segSegDist2(ax, az, bx, bz, minX, maxZ, minX, minZ));
    return best;
}

// Fwd decl — defined beside kRanges below (same anonymous namespace resumes
// there). Task #26 ridge filter: true when (x,z) lies within `pad` of an
// authored mountain-range band's footprint (the only narrow-crest country).
bool terrainNearRangeBand(float x, float z, float pad);

} // namespace — buildTileMeshAbs below is EXPORTED (terrain.h): self-tests
  //             survey the real emitted mesh through it. Helpers above stay
  //             internal; it may still call them (same TU, defined earlier).

// ---------------------------------------------------------------------------
// Build one tile's render mesh at a given LOD from ABSOLUTE (signed) tile coords.
// originX/originZ are the tile's min-corner world position. Fills outVerts with
// the top surface + a downward skirt around the border to hide LOD cracks; the
// surface is the first vpe*vpe verts (used for collision). Pure / thread-safe.
// ---------------------------------------------------------------------------
void buildTileMeshAbs(const TerrainConfig& cfg, float originX, float originZ,
                      TerrainLod lod,
                      std::vector<x3::rhi::MeshVertex>& outVerts,
                      std::vector<uint32_t>& outIdx,
                      uint32_t* outSurfIdxCount /*= nullptr, see terrain.h*/,
                      const float* refineFocusXZ /*= nullptr, see terrain.h*/) {
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

    // Corridor influence near this tile — gathered up front because BOTH the
    // corridor x tile-LOD refinement below AND the ridge filter in the grid
    // loop consult it. X3_NO_CORRIDOR_LOD_REFINE=1 is the A/B instrument
    // (mirroring X3_NO_VCURVE): it reproduces the pre-fix mesh so the wedge
    // can be MEASURED from one build. Nothing else should ever set it.
    // Gather pad = one coarse cell + 0.5 m: the far-field clamp below also
    // tests the RING of cells just outside the tile (a border vertex's clamp
    // must see the same hot cells from both sides of a seam), so segments
    // reaching only that ring must be in the list too. Harmless for the exact
    // path — cellHot() still measures true distance.
    static const bool kRefineDisabled = [] {
        const char* e = std::getenv("X3_NO_CORRIDOR_LOD_REFINE");
        return e && e[0] == '1';
    }();
    std::vector<TerrainCorridorSegRef> hotSegs;
    if (!kRefineDisabled && lod != TerrainLod::Full && terrainCorridorCount() > 0)
        terrainCorridorSegmentsNearRect(ox - cell - 0.5f, oz - cell - 0.5f,
                                        ox + tileSize + cell + 0.5f,
                                        oz + tileSize + cell + 0.5f,
                                        hotSegs);
    const float fineCell = tileSize / (float)(cfg.tileVerts - 1);

    // ---- RIDGE-PRESERVING FILTER (task #26 — the blade towers) -------------
    // buildTileMeshAbs POINT-samples h(x,z) at the LOD stride (4 m at Quarter).
    // A mountain crest only a few metres across falls BETWEEN samples and
    // ceases to exist except where a vertex happens to land on it — distant
    // ridges render as thin vertical BLADES. Fix (TUNNEL_MOUTH_LOD.md bug 1):
    // at coarse LODs an interior vertex takes the MAX of the field over the
    // cell-sized block it represents (the blocks tile the plane, so every
    // fine-lattice crest sample survives into some vertex). Scoped to the
    // authored mountain-range bands (the only narrow-crest country), and OFF:
    //   * on BORDER verts — they stay exact point samples, so two tiles at
    //     ANY LOD pair still agree bit-for-bit along a shared edge (the max
    //     kernel is LOD-sized, so a filtered border would disagree across a
    //     Full/coarse seam — the step TUNNEL_MOUTH_LOD.md §caveat warns of);
    //   * within a corridor's influence (+1 cell) — the corridor refine and
    //     the M7/C7/W1b parity gates own that ground; raising a vertex there
    //     could resurrect a portal-hole curtain or stand above the carve;
    //   * at Full LOD (1 m sampling carries the crest already).
    // X3_NO_RIDGE_FILTER=1 is the A/B instrument (measure the blades from one
    // build); --test-terrain T4 gates the crest survival. Nothing else sets it.
    static const bool kRidgeFilterDisabled = [] {
        const char* e = std::getenv("X3_NO_RIDGE_FILTER");
        return e && e[0] == '1';
    }();
    const bool ridgeFilterOn = !kRidgeFilterDisabled && cfg.worldFeatures &&
                               lod != TerrainLod::Full;
    auto vertInCorridorInfluence = [&](float wx, float wz) {
        for (const TerrainCorridorSegRef& s : hotSegs) {
            const float pad = s.reach + cell;
            if (segPointDist2(s.ax, s.az, s.bx, s.bz, wx, wz) <= pad * pad)
                return true;
        }
        return false;
    };

    // ---- Top surface grid ------------------------------------------------
    for (uint32_t j = 0; j < vpe; ++j) {
        for (uint32_t i = 0; i < vpe; ++i) {
            const float wx = ox + i * cell;
            const float wz = oz + j * cell;
            const float u  = (float)i / (float)quads * uvScale;
            const float v  = (float)j / (float)quads * uvScale;
            outVerts.push_back(makeTerrainVertex(cfg, wx, wz, u, v, eps));
            if (ridgeFilterOn && i > 0 && j > 0 && i < vpe - 1 && j < vpe - 1 &&
                terrainNearRangeBand(wx, wz, cell) &&
                !vertInCorridorInfluence(wx, wz)) {
                float& y = outVerts.back().pos[1];
                const int h = (int)(step >> 1);
                for (int oj = -h; oj <= h; ++oj)
                    for (int oi = -h; oi <= h; ++oi) {
                        if (oi == 0 && oj == 0) continue;
                        y = std::max(y, terrainHeightAt(cfg,
                                wx + (float)oi * fineCell, wz + (float)oj * fineCell));
                    }
            }
        }
    }
    // PORTAL HOLES (see terrain.h): with none registered this is the exact
    // pre-hole index build. With holes, each surface triangle is tested by XZ
    // centroid + lowest vertex — the tunnel-mouth CURTAIN (triangles reaching
    // down into the bore) is skipped from render AND, because the collision
    // soup reuses these indices, from collision too.
    const bool anyHole = (terrainPortalHoleCount() > 0);
    auto pushTri = [&](uint32_t a, uint32_t b, uint32_t c) {
        if (anyHole) {
            const x3::rhi::MeshVertex& va = outVerts[a];
            const x3::rhi::MeshVertex& vb = outVerts[b];
            const x3::rhi::MeshVertex& vc = outVerts[c];
            const float cx = (va.pos[0] + vb.pos[0] + vc.pos[0]) * (1.0f / 3.0f);
            const float cz = (va.pos[2] + vb.pos[2] + vc.pos[2]) * (1.0f / 3.0f);
            const float mY = std::min(va.pos[1], std::min(vb.pos[1], vc.pos[1]));
            if (terrainPortalHoleDrops(cx, cz, mY)) return;
        }
        outIdx.insert(outIdx.end(), { a, b, c });
    };

    // ---- CORRIDOR x TILE-LOD REFINEMENT (the spawn-road green-wedge fix) ---
    // The carve is part of h(x,z), so the VERTICES above already agree at
    // every LOD. The wedge lives BETWEEN them: a Half/Quarter cell interpolates
    // 2/4 m chords, and a chord across the corridor's smoothstep shoulder
    // reconstructs ABOVE the carved datum — measured 1.75 m of terrain standing
    // through the spawn-road pavement at Quarter LOD (it survived lifting the
    // road slab 0.07 m proud, because it is a MESH artifact, not a carve
    // disagreement; see --test-terraincorridor C7 / --test-tunnelmouth M7).
    //
    // Fix: any coarse cell whose square lies within a corridor's influence
    // (halfWidth + falloff of a spine segment) is meshed at FULL resolution,
    // so inside the influence the surface is IDENTICAL at every LOD — the
    // wedge cannot exist, and a tile border crossing a corridor matches its
    // differently-LODed neighbour vertex for vertex. Hot/cold is decided from
    // pure world-space geometry, so both sides of a border agree.
    //
    // WATERTIGHTNESS: an inserted vertex on a refined cell's EDGE samples the
    // true field only when that EDGE is itself inside the influence; otherwise
    // it is pinned to the coarse chord. An edge inside the influence forces
    // BOTH its cells hot (edge ⊂ cell ⇒ cellDist <= edgeDist), so a hot cell
    // meets a cold neighbour only across a cold edge — where its inserted
    // verts lie exactly ON the cold neighbour's straight triangle edge.
    // (kRefineDisabled + hotSegs gathered above the grid loop — the ridge
    // filter consults them too.)
    const bool refineOn = !hotSegs.empty();
    auto edgeHot = [&](float x0, float z0, float x1, float z1) {
        for (const TerrainCorridorSegRef& s : hotSegs)
            if (segSegDist2(s.ax, s.az, s.bx, s.bz, x0, z0, x1, z1) <= s.reach * s.reach)
                return true;
        return false;
    };
    auto cellHot = [&](uint32_t i, uint32_t j) {
        const float x0 = ox + i * cell, z0 = oz + j * cell;
        for (const TerrainCorridorSegRef& s : hotSegs)
            if (segRectDist2(s.ax, s.az, s.bx, s.bz, x0, z0, x0 + cell, z0 + cell)
                    <= s.reach * s.reach)
                return true;
        return false;
    };
    // ---- FAR-FIELD CLAMP (W-PERF, task #33) — the refine's distance scope --
    // The exact refine above is the NEAR-FIELD fix: within kCorridorRefineNearM
    // of the stream-request focus a hot cell is re-meshed on the LOD0 lattice.
    // Paying that full-res triangle bill at EVERY distance is what it costs
    // today; at range the correctness requirement is only one-sided — terrain
    // must never stand ABOVE the carved road (the green strip), while standing
    // a little BELOW it is invisible (the road ribbon draws on top). So beyond
    // the scope a hot cell keeps its 2 coarse triangles and instead every
    // corner vertex of every CARVED hot cell is clamped to
    //     y = min(y, min(carved field over the cell's LOD0 lattice))
    // Every interior chord point of the cell is a convex combination of its
    // corners, so mesh(P) <= max(corner y) <= min(field over cell) <= field(P)
    // for every P in the cell — the wedge is unrepresentable, at 2 tris/cell.
    // Seam-safe: the clamp of a border vertex scans its full 2x2 incident-cell
    // ring in WORLD space (including cells outside this tile — the gather pad
    // above), so both tiles sharing the vertex compute the identical minimum.
    // Bored reaches (depth ~0: a tunnel does not carve the mountain above it)
    // are exempt — clamping there would pull the LID down toward the portal
    // holes' yTop and tear it open; there is no wedge over a bore to kill.
    // Gate: --test-terraincorridor C8 (wedge dead + triangles actually saved +
    // seam parity at range).
    bool farClamp = false;
    if (refineOn && refineFocusXZ) {
        const float fx = refineFocusXZ[0], fz = refineFocusXZ[1];
        const float nxp = std::min(std::max(fx, ox), ox + tileSize);
        const float nzp = std::min(std::max(fz, oz), oz + tileSize);
        const float ddx = fx - nxp, ddz = fz - nzp;   // focus -> nearest tile point
        farClamp = (ddx * ddx + ddz * ddz) >
                   kCorridorRefineNearM * kCorridorRefineNearM;
    }
    if (farClamp) {
        // Hot-for-clamp: within reach of a CARVED segment (depth > 0.3 m).
        auto cellHotCarved = [&](float x0, float z0) {
            for (const TerrainCorridorSegRef& s : hotSegs)
                if (std::max(s.depth0, s.depth1) > 0.3f &&
                    segRectDist2(s.ax, s.az, s.bx, s.bz,
                                 x0, z0, x0 + cell, z0 + cell) <= s.reach * s.reach)
                    return true;
            return false;
        };
        // Scan the tile's cells PLUS the one-cell ring outside it, so border
        // vertices receive the same clamp from either side of the seam.
        for (int cj = -1; cj <= (int)quads; ++cj) {
            for (int ci = -1; ci <= (int)quads; ++ci) {
                const float x0 = ox + (float)ci * cell;
                const float z0 = oz + (float)cj * cell;
                if (!cellHotCarved(x0, z0)) continue;
                float m = std::numeric_limits<float>::infinity();
                for (uint32_t fj = 0; fj <= step; ++fj)
                    for (uint32_t fi = 0; fi <= step; ++fi)
                        m = std::min(m, terrainHeightAt(cfg,
                                x0 + (float)fi * fineCell, z0 + (float)fj * fineCell));
                for (int dj = 0; dj <= 1; ++dj)
                    for (int di = 0; di <= 1; ++di) {
                        const int vi = ci + di, vj = cj + dj;
                        if (vi < 0 || vj < 0 || vi >= (int)vpe || vj >= (int)vpe)
                            continue;
                        float& y = outVerts[(size_t)vj * vpe + (size_t)vi].pos[1];
                        y = std::min(y, m);
                    }
            }
        }
    }
    // Refined border cells hand their fine outer-edge vertex chains to the
    // skirt pass below (a coarse-chord skirt under a refined border would
    // stand proud of the carved surface — the wedge back again, as a curtain).
    std::vector<std::vector<uint32_t>> southChain(refineOn ? quads : 0);
    std::vector<std::vector<uint32_t>> northChain(refineOn ? quads : 0);
    std::vector<std::vector<uint32_t>> westChain (refineOn ? quads : 0);
    std::vector<std::vector<uint32_t>> eastChain (refineOn ? quads : 0);

    for (uint32_t j = 0; j < quads; ++j) {
        for (uint32_t i = 0; i < quads; ++i) {
            const uint32_t a = j * vpe + i;
            const uint32_t b = a + 1;
            const uint32_t c = a + vpe;
            const uint32_t d = c + 1;
            if (!refineOn || farClamp || !cellHot(i, j)) {
                // Coarse cell (also every FAR-CLAMPED hot cell: its corner
                // verts were already pulled under the carved field above).
                pushTri(a, c, b);
                pushTri(b, c, d);
                continue;
            }
            // FULL-RES REFINEMENT of this cell. Fine grid indices run over the
            // LOD0 lattice (gi = i*step + fi), so positions/heights/normals are
            // bit-identical to what a Full-LOD build emits at the same spots.
            const uint32_t n   = step;
            const uint32_t gi0 = i * step, gj0 = j * step;
            // Corner verts are the coarse grid's own (same world position — the
            // fine lattice contains the coarse one).
            const x3::rhi::MeshVertex vA = outVerts[a], vB = outVerts[b];
            const x3::rhi::MeshVertex vC = outVerts[c], vD = outVerts[d];
            const float ex0 = ox + (float)gi0 * fineCell;
            const float ez0 = oz + (float)gj0 * fineCell;
            const float ex1 = ox + (float)(gi0 + n) * fineCell;
            const float ez1 = oz + (float)(gj0 + n) * fineCell;
            const bool hotS = edgeHot(ex0, ez0, ex1, ez0);
            const bool hotN = edgeHot(ex0, ez1, ex1, ez1);
            const bool hotW = edgeHot(ex0, ez0, ex0, ez1);
            const bool hotE = edgeHot(ex1, ez0, ex1, ez1);
            auto lerpVert = [](const x3::rhi::MeshVertex& A, const x3::rhi::MeshVertex& B,
                               float t) {
                x3::rhi::MeshVertex o;
                for (int q = 0; q < 3; ++q) {
                    o.pos[q]    = A.pos[q]    + (B.pos[q]    - A.pos[q])    * t;
                    o.normal[q] = A.normal[q] + (B.normal[q] - A.normal[q]) * t;
                }
                const float nl = std::sqrt(o.normal[0]*o.normal[0] + o.normal[1]*o.normal[1] +
                                           o.normal[2]*o.normal[2]);
                if (nl > 1e-6f) { o.normal[0] /= nl; o.normal[1] /= nl; o.normal[2] /= nl; }
                o.uv[0] = A.uv[0] + (B.uv[0] - A.uv[0]) * t;
                o.uv[1] = A.uv[1] + (B.uv[1] - A.uv[1]) * t;
                return o;
            };
            std::vector<uint32_t> fidx((size_t)(n + 1) * (n + 1));
            for (uint32_t fj = 0; fj <= n; ++fj) {
                for (uint32_t fi = 0; fi <= n; ++fi) {
                    uint32_t& slot = fidx[(size_t)fj * (n + 1) + fi];
                    const bool onS = (fj == 0), onN = (fj == n);
                    const bool onW = (fi == 0), onE = (fi == n);
                    if ((onS || onN) && (onW || onE)) {   // corner: reuse coarse vert
                        slot = onS ? (onW ? a : b) : (onW ? c : d);
                        continue;
                    }
                    slot = (uint32_t)outVerts.size();
                    bool trueField = true;
                    if      (onS) trueField = hotS;
                    else if (onN) trueField = hotN;
                    else if (onW) trueField = hotW;
                    else if (onE) trueField = hotE;
                    if (trueField) {
                        const float wx = ox + (float)(gi0 + fi) * fineCell;
                        const float wz = oz + (float)(gj0 + fj) * fineCell;
                        const float u  = (float)(gi0 + fi) / (float)(cfg.tileVerts - 1) * uvScale;
                        const float v  = (float)(gj0 + fj) / (float)(cfg.tileVerts - 1) * uvScale;
                        outVerts.push_back(makeTerrainVertex(cfg, wx, wz, u, v, eps));
                    } else {
                        // chord-pinned: exactly on the cold neighbour's edge
                        if      (onS) outVerts.push_back(lerpVert(vA, vB, (float)fi / (float)n));
                        else if (onN) outVerts.push_back(lerpVert(vC, vD, (float)fi / (float)n));
                        else if (onW) outVerts.push_back(lerpVert(vA, vC, (float)fj / (float)n));
                        else          outVerts.push_back(lerpVert(vB, vD, (float)fj / (float)n));
                    }
                }
            }
            for (uint32_t fj = 0; fj < n; ++fj) {
                for (uint32_t fi = 0; fi < n; ++fi) {
                    const uint32_t fa = fidx[(size_t)fj * (n + 1) + fi];
                    const uint32_t fb = fidx[(size_t)fj * (n + 1) + fi + 1];
                    const uint32_t fc = fidx[(size_t)(fj + 1) * (n + 1) + fi];
                    const uint32_t fd = fidx[(size_t)(fj + 1) * (n + 1) + fi + 1];
                    pushTri(fa, fc, fb);
                    pushTri(fb, fc, fd);
                }
            }
            // Hand fine border chains to the skirt pass (+i / +j order).
            if (j == 0) {
                auto& ch = southChain[i]; ch.resize(n + 1);
                for (uint32_t fi = 0; fi <= n; ++fi) ch[fi] = fidx[fi];
            }
            if (j == quads - 1) {
                auto& ch = northChain[i]; ch.resize(n + 1);
                for (uint32_t fi = 0; fi <= n; ++fi) ch[fi] = fidx[(size_t)n * (n + 1) + fi];
            }
            if (i == 0) {
                auto& ch = westChain[j]; ch.resize(n + 1);
                for (uint32_t fj = 0; fj <= n; ++fj) ch[fj] = fidx[(size_t)fj * (n + 1)];
            }
            if (i == quads - 1) {
                auto& ch = eastChain[j]; ch.resize(n + 1);
                for (uint32_t fj = 0; fj <= n; ++fj) ch[fj] = fidx[(size_t)fj * (n + 1) + n];
            }
        }
    }
    // The surface/skirt boundary is no longer a fixed quads*quads*6 once holes
    // drop triangles — report it so the collision soup can take exactly the
    // surface prefix.
    if (outSurfIdxCount) *outSurfIdxCount = (uint32_t)outIdx.size();

    // ---- Crack-hiding SKIRT (not in the collision surface) ---------------
    // Features worlds carry real mountains (slopes far steeper than the base
    // field), so LOD-decimation error at tile borders can exceed the old depth —
    // drop the skirt further there.
    const float skirtDepth = cfg.heightScale * 0.25f + (cfg.worldFeatures ? 40.0f : 0.0f) + 1.0f;
    // ...EXCEPT inside a registered CORRIDOR. A skirt is a curtain hanging off
    // the tile border to hide LOD cracks when you look at the border from
    // OUTSIDE the ground; it is invisible because the neighbouring tile stands
    // in front of it. A corridor deep enough to walk or drive through puts the
    // camera UNDER the surface, and a ~55 m curtain then hangs straight across
    // the bore every 32 m — a solid wall of terrain in the middle of the
    // tunnel. Inside the corridor the field is a smooth flat-floored channel,
    // so the LOD error there is centimetres and a short skirt is ample.
    // Evaluated from the segment MIDPOINT, which is identical from either side
    // of a shared seam, so the two tiles still agree bit-for-bit (the
    // --test-terraincorridor C3 seam check).
    const bool anyCorridor = (terrainCorridorCount() > 0);
    auto addSkirtEdgeV = [&](uint32_t topA, uint32_t topB) {
        const x3::rhi::MeshVertex va = outVerts[topA];
        const x3::rhi::MeshVertex vb = outVerts[topB];
        float depth = skirtDepth;
        const float mx = (va.pos[0] + vb.pos[0]) * 0.5f;
        const float mz = (va.pos[2] + vb.pos[2]) * 0.5f;
        // A skirt is ALSO a curtain: inside a portal hole it would hang exactly
        // where the surface triangles were just dropped and wall the bore off
        // again wherever a tile border crosses a mouth. Same predicate, same
        // seam-consistency argument (midpoint is identical from both sides).
        if (anyHole &&
            terrainPortalHoleDrops(mx, mz, std::min(va.pos[1], vb.pos[1]) - depth))
            return;
        // Short skirts over the corridor FOOTPRINT — by containment, not by
        // delta. The old test (`terrainCorridorDelta < -0.25`) was blind to
        // BORED reaches, whose depth is 0 by design ("a tunnel does not carve
        // the mountain above it"): over the bore the lid is natural ground, the
        // delta is exactly 0, and a full ~55 m skirt at a tile border hung from
        // the lid STRAIGHT THROUGH THE TUBE — measured 74 full-LOD skirt
        // triangles inside the demo bore, reaching to 0.3 m above the road: a
        // render-only rock wall across the carriageway (skirts carry no
        // collision, so the car drove through it). Containment sees the tube
        // under the lid; delta cannot.
        if (anyCorridor && terrainCorridorContains(mx, mz)) {
            float cap = 2.5f;
            // Far-clamped tiles sit BELOW the field near the carve; the
            // neighbouring tile may mesh the field itself (exact refine, or a
            // different LOD's clamp). Deepen the short corridor skirt by the
            // local drop so that seam gap stays curtained.
            if (farClamp)
                cap += std::max(0.0f, terrainHeightAt(cfg, mx, mz) -
                                      std::min(va.pos[1], vb.pos[1]));
            depth = std::min(depth, cap);
        }
        x3::rhi::MeshVertex la = va, lb = vb;
        la.pos[1] -= depth; lb.pos[1] -= depth;
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
    // Border skirts, per border CELL: a refined border cell hangs its skirt
    // from its FINE outer-edge chain (a coarse chord under a refined border
    // would stand proud of the carved surface inside a corridor — the wedge
    // again, as a curtain); an unrefined cell keeps the coarse pair. `fwd`
    // preserves each border's original winding (south/east run +, north/west
    // run -, exactly the four legacy loops).
    auto skirtCell = [&](const std::vector<uint32_t>* chain,
                         uint32_t coarseA, uint32_t coarseB, bool fwd) {
        if (chain && !chain->empty()) {
            const auto& ch = *chain;
            if (fwd) { for (size_t k = 0; k + 1 < ch.size(); ++k) addSkirtEdgeV(ch[k], ch[k + 1]); }
            else     { for (size_t k = ch.size(); k >= 2; --k)    addSkirtEdgeV(ch[k - 1], ch[k - 2]); }
            return;
        }
        if (fwd) addSkirtEdgeV(coarseA, coarseB);
        else     addSkirtEdgeV(coarseB, coarseA);
    };
    for (uint32_t i = 0; i < quads; ++i)
        skirtCell(refineOn ? &southChain[i] : nullptr,
                  0 * vpe + i, 0 * vpe + i + 1, /*fwd=*/true);
    for (uint32_t i = 0; i < quads; ++i)
        skirtCell(refineOn ? &northChain[i] : nullptr,
                  (vpe - 1) * vpe + i, (vpe - 1) * vpe + i + 1, /*fwd=*/false);
    for (uint32_t j = 0; j < quads; ++j)
        skirtCell(refineOn ? &westChain[j] : nullptr,
                  j * vpe + 0, (j + 1) * vpe + 0, /*fwd=*/false);
    for (uint32_t j = 0; j < quads; ++j)
        skirtCell(refineOn ? &eastChain[j] : nullptr,
                  j * vpe + (vpe - 1), (j + 1) * vpe + (vpe - 1), /*fwd=*/true);
}

namespace {   // internal helpers resume

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

// Load one surface_library set's real albedo (curated 2K PNG, tools/tex_curate.py
// conventions) as an sRGB terrain detail texture. Falls back to the old
// procedural noise tile (small, seamless) if the file is missing — e.g. a
// headless CI box that hasn't run `asset_store.py fetch --all` yet — so terrain
// always has SOMETHING to splat rather than failing to build.
x3::rhi::TextureHandle loadTerrainAlbedo(x3::rhi::IRenderDevice& device,
                                         const std::string& setName,
                                         uint32_t fallbackSeed,
                                         int fbR, int fbG, int fbB, int fbVar) {
    const std::string path = x3::game::assetRoot() + "/surface_library/" + setName + "/albedo.png";
    int w = 0, h = 0, comp = 0;
    stbi_uc* px = stbi_load(path.c_str(), &w, &h, &comp, 4);
    if (px) {
        x3::rhi::TextureHandle t = device.createTexture(px, (uint32_t)w, (uint32_t)h, /*srgb=*/true);
        stbi_image_free(px);
        if (t.valid()) return t;
    }
    x3::logWarn("[terrain] surface set '" + setName + "' missing/unreadable at " + path +
               " -- falling back to procedural noise tile");
    const uint32_t kN = 64;
    auto fb = makeDetailRGBA(kN, fallbackSeed, fbR, fbG, fbB, fbVar, fbVar, fbVar);
    return device.createTexture(fb.data(), kN, kN, /*srgb=*/true);
}

// Load one surface_library set's NORMAL map as a LINEAR (non-sRGB) texture — a
// normal map is a vector field, not colour, and sRGB-decoding it would bend every
// normal toward the surface. There is deliberately NO procedural fallback: an
// invented normal map is worse than none, because "none" (an invalid handle ->
// bindless index 0) makes mesh.frag shade that layer from the geometry normal,
// which is exactly the pre-relief behaviour. Returns an invalid handle if the
// file is missing.
x3::rhi::TextureHandle loadTerrainNormal(x3::rhi::IRenderDevice& device,
                                         const std::string& setName) {
    const std::string path = x3::game::assetRoot() + "/surface_library/" + setName + "/normal.png";
    int w = 0, h = 0, comp = 0;
    stbi_uc* px = stbi_load(path.c_str(), &w, &h, &comp, 4);
    if (px) {
        x3::rhi::TextureHandle t = device.createTexture(px, (uint32_t)w, (uint32_t)h, /*srgb=*/false);
        stbi_image_free(px);
        if (t.valid()) return t;
    }
    x3::logWarn("[terrain] surface set '" + setName + "' has NO normal map at " + path +
                " -- that layer will shade FLAT (geometry normal only)");
    return {};
}

// Build the four ground DETAIL textures (grass / rock / snow / sand), register
// them as the terrain MATERIAL set, and return the opaque MARKER handle the
// renderer uses to flag terrain draws for the height+slope splat in mesh.frag.
// One marker per terrain/streamer instance; the four detail textures live in the
// device's bindless array (freed at device shutdown, like the old ground tex).
// If the device can't bind them (e.g. headless), the marker may be invalid and
// terrain falls back to a flat tint — still correct, just not splatted.
//
// Real 2K albedos (curated via tools/tex_curate.py -> assets/surface_library/,
// published via tools/asset_store.py) replace the old 64px procedural-noise
// fill (W3-4's placeholder). Sets picked from docs/tex_catalog.json:
//   grass -> terrain_grass (Rocky Hills Environment - Whitebark Pine, GrassTileRHEP)
//   rock  -> terrain_rock  (Rocky Hills Environment - Whitebark Pine, CliffTileRHEP)
//     ROCK WAS sr_concrete_01, AND THAT IS WHY THE GROUND HAD A GRID IN IT.
//     sr_concrete_01 is a photo of a CONCRETE PANEL WALL - flat slabs divided by
//     straight vertical and horizontal joints, with form-tie bolt holes. Splatted
//     onto terrain and tiled every 5.56 m it produced the rectangular grid
//     marching up the cut faces in 08_exit_portal and the grey-blue slab blobs
//     scattered over the field in 04_saddle. It was never a rock texture; it was
//     borrowed because it was "already game-ready". CliffTileRHEP is a purpose-
//     built seamless natural cliff tile from the SAME pack as the grass, so the
//     two read as one landscape.
//   sand  -> terrain_sand  (Landscape Ground Pack 3, T_ground_sand_01)
//   snow  -> terrain_snow  (Landscape Ground Pack 3, T_Ground_32_Snow_BC_SM +
//            _N -- the SAME NatureManufacture pack as grass and sand above, so
//            all three read as one landscape rather than three purchases).
//            REAL SNOW as of 2026-08-15. It was MarbleWhite00 -- cracked white
//            marble -- because the CURATED catalog has no snow in its 2626
//            entries. The raw fleet share has several; this is the best of them.
//            Marble's failure was NOT resolution or tiling: both were 2048, and
//            the stochastic sampler was already doing its job. Marble simply HAS
//            large directional veins, and no anti-tiling technique can remove a
//            distinctive feature from a source -- it can only stop it landing on
//            a grid. Snow tiles invisibly because it has nothing to repeat.
//
//            MEASURED, because "more detail" is not the same as "better": the
//            marble carried 10.97 HF energy at sd 42.1 against this set's 1.11
//            at sd 2.3. That collapse is CORRECT. A real snow albedo is nearly
//            flat white; all of its structure lives in the normal and the
//            roughness. High albedo contrast is what made the marble read as
//            cracked stone, and would make any snow read as white concrete.
//
//            Nothing is synthesised on top. An earlier pass here upscaled a
//            700px 20 KB JPEG (blocking ratio 2.51) to 2K and added procedural
//            crystal grain to cover the softness; this is native uncompressed
//            2K with an authored normal, so grain would be synthetic detail
//            REPLACING real detail. Roughness likewise comes from the pack's own
//            smoothness alpha -- mean 0.57 and varying (sd 22.9), which is
//            crusted facets against packed powder. The flat 0.86 guess it
//            replaces was both too rough and too uniform: a matte sheet that
//            never glints as you drive past it.
x3::rhi::TextureHandle makeGroundTexture(x3::rhi::IRenderDevice& device) {
    auto grass = loadTerrainAlbedo(device, "terrain_grass", 1001u,  78, 116,  56, 18); // green
    // BOTH halves of the 2026-08-14 terrain work, kept whole:
    //   * the rock ALBEDO is Predator's real cliff set (the slot used to be
    //     sr_concrete_01, so every cliff and cutting in the game was textured
    //     with CONCRETE and streaked with form-lines under the triplanar blend);
    //   * the per-layer NORMAL maps are 14900k's relief fix -- mesh.frag skipped
    //     its normal path for terrain entirely, so a cut face took the rock
    //     albedo and then lit like plaster.
    // Neither replaces the other: one decides what the ground IS, the other
    // decides how it CATCHES LIGHT.
    auto rock  = loadTerrainAlbedo(device, "fw_rock_cliff",  2002u, 104, 100,  92, 24); // real cliff rock
    auto snow  = loadTerrainAlbedo(device, "terrain_snow",  3003u, 222, 226, 235, 14); // bright white-blue
    auto sand  = loadTerrainAlbedo(device, "terrain_sand",  4004u, 178, 158, 118, 18); // tan
    // THE SECOND ROCK BAND (Tim: "you NEED more than one texture thats for
    // SURE"). The low rock slot stays the warm tan cliff — it textures the
    // road cuttings — and this darker blue-grey craggy slate takes over with
    // ALTITUDE (mesh_terrain.glsl kAlpineTint band), so the massif reads as
    // different stone from the roadside without dragging the cuttings cold.
    // terrain_rock_dark = UniStorm Rock_2 (see tools/tex_curate.py
    // SETS_EXPLICIT for provenance + the contact-sheet rationale).
    auto rockHi = loadTerrainAlbedo(device, "terrain_rock_dark", 5005u, 84, 88, 98, 20); // dark craggy
    // ...and each layer's NORMAL map. This is what turns the splat from four flat
    // COLOURS into four SURFACES. X3_TERRAIN_NORMALS=0 loads none of them, which
    // is the exact pre-relief renderer. It exists so the before/after can be
    // captured from ONE build at ONE viewpoint — an A/B where the only difference
    // is the relief, with no recompile in between to smuggle in anything else.
    const char* nrmEnv = std::getenv("X3_TERRAIN_NORMALS");
    const bool wantNormals = !(nrmEnv && nrmEnv[0] == '0');
    x3::rhi::TextureHandle grassN, rockN, snowN, sandN;
    if (wantNormals) {
        grassN = loadTerrainNormal(device, "terrain_grass");
        // MATCHES THE ALBEDO ABOVE. This said "terrain_rock" on the branch that
        // introduced it, which was right THEN; now that the rock slot is
        // fw_rock_cliff, loading terrain_rock's normal here would light the
        // cliff with a different stone's bumps. The set ships its own normal.png.
        rockN  = loadTerrainNormal(device, "fw_rock_cliff");
        snowN  = loadTerrainNormal(device, "terrain_snow");
        sandN  = loadTerrainNormal(device, "terrain_sand");
    } else {
        x3::logWarn("[terrain] X3_TERRAIN_NORMALS=0 -- terrain relief DISABLED (albedo only)");
    }
    // NOTE: the high-altitude rock band is albedo-only for now — it borrows the
    // low rock's normal via the shader's blend. A 10th slot for its own normal is
    // a follow-up, not a merge decision.
    x3::rhi::TextureHandle marker =
        device.registerTerrainMaterial(grass, rock, snow, sand, rockHi,
                                       grassN, rockN, snowN, sandN);
    // Fallback: if the material set couldn't be registered (no bindless), use the
    // grass tile directly so terrain is at least a believable green, not white.
    return marker.valid() ? marker : grass;
}

// LOD by center-to-camera distance. Thresholds scale with tile size so the
// scheme is resolution-independent. Near = Full, mid = Half, far = Quarter.
// 9-point corridor-touch test for a tile footprint (corners, edge midpoints,
// center). 32 m tiles vs ~27 m corridors: if any sample is inside, pin.
bool tileTouchesCorridor(float ox, float oz, float size) {
    for (int j = 0; j <= 2; ++j)
        for (int i = 0; i <= 2; ++i)
            if (terrainCorridorContains(ox + size * 0.5f * i, oz + size * 0.5f * j))
                return true;
    return false;
}

TerrainLod lodForDist(const TerrainConfig& cfg, float dist) {
    const float nearD = cfg.tileSize * 2.5f;
    const float midD  = cfg.tileSize * 6.0f;
    if (dist < nearD) return TerrainLod::Full;
    if (dist < midD)  return TerrainLod::Half;
    return TerrainLod::Quarter;
}

// A/B instrument for the corridor Full-LOD pin (mirrors X3_NO_CORRIDOR_LOD_REFINE):
// X3_NO_CORRIDOR_PIN=1 drops the "<420 m => Full" pin so its triangle/fps cost
// can be MEASURED against the corridor x tile-LOD refinement, which already
// makes the coarse meshes IDENTICAL to Full over every corridor floor
// (--test-terraincorridor C7 / --test-roadnetwork W1b). Task #33's question:
// is the pin now pure belt-and-suspenders? Nothing else should ever set this.
bool corridorPinEnabled() {
    static const bool on = [] {
        const char* e = std::getenv("X3_NO_CORRIDOR_PIN");
        return !(e && e[0] == '1');
    }();
    return on;
}

} // namespace

// ---------------------------------------------------------------------------
// W8-3 — the CANONICAL WORLD MAP layers (cfg.worldFeatures). Ported from the
// Babylon X3 world's terrain personality (Q3Engine src/world/x3-world-terrain.js
// ps2_heightAt + x3-mountains.js — layout/dimensions as CONTENT reference; the
// math here is the engine's own value-noise/fBm, clean-room):
//   * MACRO RELIEF — a ~2.5 km-wavelength amplitude field so the map reads as
//     plains / rolling country / hill country instead of one noise character.
//   * 4 MOUNTAIN RANGES ringing the world 7-10 km out (native compass, +Z = N):
//     N snow (jagged, to ~480 m) / E volcanic (tallest, ~500 m) / S mesa
//     (capped flat tops ~200 m) / W crystal highlands (rolling, ~350 m).
//     Ridged noise along each range band => a chain of real peaks, not a blob.
//   * FLAT PADS — facility/crash pad at the origin + the three city districts
//     (Scrapyard / New District / Industrial), so streets + building rows sit
//     level (the Babylon map flattens the same zones).
//   * OCEAN BASIN — a shore-falloff depression around the offshore ocean_base
//     region (1100,-1350) so the water plane reads as a sea with a beach ring,
//     deepening to ~-90 m over the undersea disc.
// Pure + deterministic in (cfg, x, z); shared by render, collision, and every
// placeOnTerrain query, so content anchors to the same surface it draws on.
// ---------------------------------------------------------------------------
namespace {

inline float sstep(float e0, float e1, float x) {
    float t = (x - e0) / (e1 - e0);
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return t * t * (3.0f - 2.0f * t);
}

// Distance from p to segment ab (XZ).
inline float segDist(float px, float pz, float ax, float az, float bx, float bz) {
    const float abx = bx - ax, abz = bz - az;
    const float apx = px - ax, apz = pz - az;
    const float len2 = abx * abx + abz * abz;
    float t = (len2 > 1e-6f) ? (apx * abx + apz * abz) / len2 : 0.0f;
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    const float dx = apx - abx * t, dz = apz - abz * t;
    return std::sqrt(dx * dx + dz * dz);
}

// One mountain range: a band (segment + width) carrying ridged-noise peaks.
struct RangeDef {
    float ax, az, bx, bz;    // band spine (world XZ)
    float coreW, outW;       // full-strength half-width / zero-strength half-width
    float amp;               // peak amplitude (m above the base field)
    float ridgeExp;          // ridge sharpness (higher = more jagged)
    float jagAmp;            // extra high-freq jag amplitude (m)
    float capY;              // clamp (mesa flat tops); <=0 = no cap
};
// Native compass: +Z = north (regions.json / world_regions gazetteer). Band
// placement + peak heights follow the Babylon map (N z~8300 to 480 m snow /
// E x~9200 to 500 m volcanic / S z~-9000 mesas ~200 m / W x~-8600 rolling 350 m).
constexpr int kRangeCount = 5;
const RangeDef kRanges[kRangeCount] = {
    { -2200.0f,  8300.0f,  2800.0f,  8300.0f, 500.0f, 2200.0f, 380.0f, 2.2f, 45.0f,   0.0f }, // N snow
    {  9200.0f, -2000.0f,  9200.0f,  2500.0f, 550.0f, 2300.0f, 460.0f, 2.0f, 45.0f,   0.0f }, // E volcanic
    { -2800.0f, -9000.0f,  3500.0f, -9000.0f, 500.0f, 2100.0f, 230.0f, 1.1f,  0.0f, 195.0f }, // S mesa
    { -8600.0f, -2000.0f, -8600.0f,  1800.0f, 600.0f, 2400.0f, 320.0f, 1.3f,  0.0f,   0.0f }, // W crystal hills
    // TUNNEL RIDGE — the sketch's LARGE MOUNTAIN (ROAD_NETWORK_SKETCH.png, NW
    // massif; route-spec law). The four ranges above sit 8-9 km out, so the
    // corridor route at (-592,-352) bored through the BASE rolling field
    // (heightScale 55 m) — which is why the worst soil cover over the shell was
    // only 2.46 m. A freeway tunnel needs a mountain. Runs PERPENDICULAR to the
    // route heading so the bore crosses it square, and is short enough that
    // outW does not reach the Scrapyard City pad at (-600,500) — outW is the
    // pad-clearance cap (spine end (-431,36) to pad centre is 494 m; pad r 250),
    // so TALLER comes from amp, not footprint.
    // amp 285 -> 460 (owner, 2026-08-16: "The mountain in that scene should be
    // taller"; measured natural summit went 168 m -> 262 m over the bore —
    // the M-gates re-derive the cut-and-cover from the field, so the bore,
    // backfill lid and 4.5% road profile adapt at boot). ridgeExp 2.5 -> 2.3
    // widens the crest mass a touch so the raise reads as a massif, not a spike.
    {  -753.0f,  -740.0f,  -431.0f,    36.0f, 110.0f,  240.0f, 460.0f, 2.3f, 64.0f,   0.0f }, // tunnel ridge / Large Mountain
};

// Task #26 (ridge filter) — see the fwd decl above buildTileMeshAbs.
bool terrainNearRangeBand(float x, float z, float pad) {
    for (int i = 0; i < kRangeCount; ++i) {
        const RangeDef& r = kRanges[i];
        if (segDist(x, z, r.ax, r.az, r.bx, r.bz) < r.outW + pad) return true;
    }
    return false;
}

// Flat pads: blend the field toward padY inside r, fully the field by r*1.7.
struct PadDef { float cx, cz, r, padY; };
const PadDef kPads[4] = {
    // SEAM 3: the Spire grade is the CANON one — the canonical tower's F1 floor
    // (and so its facade base / apron / soil skirt) sits at Y=-2, not 0. The pad
    // matches it so the streamed ground meets the facility's own ground flush
    // (was 0.0 => a 2 m terrain cliff ringing the soil skirt). The --world
    // surface host's Y=0 plate now blends down via the horizon ring's flatten.
    {    0.0f,   0.0f, 260.0f, -2.0f },   // facility/crash pad (the CANON Spire grade)
    { -600.0f, 500.0f, 250.0f, 16.0f },   // Scrapyard City
    {  200.0f, 500.0f, 190.0f, 15.0f },   // New District
    { -200.0f, 350.0f, 150.0f, 17.0f },   // Industrial Zone
};

// Ocean basin (matches app/ocean_base.cpp kBaseCx/kBaseCz).
constexpr float kBasinCx = 1100.0f, kBasinCz = -1350.0f;

// ===========================================================================
// W9 — AUTHORED LANDFORM DRAMA (Tim, 2026-07-09: "steeper local features —
// ravines, bluffs, a canyon pass near the facility.. AND WATER FEATURES..
// RIver..."). Deterministic, authored polyline features layered on the field
// AFTER the flat pads (so a channel may cross the pad's outer BLEND ring), each
// guarded to zero influence near every piece of graded/authored content.
//
// THE MAP (all world XZ, meters; facility keep-out rect = facade footprint
// x[-3..47] z[-34.5..55.5] + kExtPad 3 + soilOut 150 => x[-156..200]
// z[-187.5..208.5], see app_run.cpp SEAM 2/3):
//
//   THE RIVER (carve nodes N0..N7, water ribbon adds N8; waterY descends):
//     N0 ( 780,  180) w=+3.5 c=2.2   source reach, NE rolling country
//     N1 ( 560,  120) w=+1.0 c=2.2
//     N2 ( 410,   40) w=-1.8 c=0.4
//     N3 ( 320,  -30) w=-2.6 c=0.4   closest approach: ~301 m E of the tower
//                              center (22,10), 120 m clear of the keep-out rect
//     N4 ( 360, -300) w=-3.4 c=0.4
//     N5 ( 480, -560) w=-5.0 c=1.4
//     N6 ( 620, -830) w=-7.5 c=2.2   enters the ocean-basin shore falloff
//     N7 ( 760,-1010) w=-9.2 c=2.2   basin proper (terrain far below bed)
//     N8 ( 900,-1120) w=-9.9 c=2.2   ribbon-only: reaches the sea (Y=-10)
//     profile: floor half-width 12 m (24 m walkable bed), banks over 26 m,
//     bed = waterY-3.2, authored levee crest = waterY+c where the country is
//     low. c is PER-NODE: the facility-adjacent reach (N2..N4) uses c=0.2 — a
//     shelving BEACH, no berm — and a FLOODPLAIN SHELF (strength (2.2-c)/2)
//     pulls the flanking country DOWN to waterY+0.2 out to ~130 m. Both exist
//     for the apron sightline: the natural pad-blend country between the soil
//     skirt (x=200) and the river rose ~1.5 m ABOVE the plain, hiding sunken
//     water from ANY eye-height vantage on the apron. The shelf is a pure
//     LOWERING (min) and carries a deliberately looser guard (0 at 10 m from
//     the keep-out rect, 1 by 60 m): the corridor it touches (x 210..320,
//     z -120..40) holds no authored content (roads x=22/170 and the crash site
//     are INSIDE the rect), and the origin pad flatten still owns x<260, so
//     the worst seam is a ~40 cm dip easing off the skirt edge. Far reaches
//     keep the full 2.2 m crest and get only a faint valley (shelf 0..0.4).
//     Clearances: crash site (140,205) >=300 m; approach-road legs x=22/x=170
//     >=150 m; New District blend ring >=60 m; East Outpost (800,400) 221 m;
//     coast-spur road z=500 >=320 m; NO road crossing => NO bridge needed.
//
//   CANYON PASS (nodes C0..C3, authored floor Y; walkable 22 m floor):
//     C0 (-140, -520) f=+2.0   mouth, ~540 m off the plain's south edge
//     C1 (-230, -760) f= 0.0
//     C2 (-160,-1040) f=-2.0
//     C3 ( -40,-1260) f=-4.0
//     a +16 m ridge (half-width 60 m, fading by 210 m) is raised ALONG the
//     spine first (faded near C0 so the mouth opens at grade), then the channel
//     is cut through it: floor half-width 11 m, walls over 15 m => 25-45 m
//     walls at ~60 deg. West Outpost (-880,-320) >=760 m; south freeway-tunnel
//     bore (x=-200, z 40..240) >=760 m.
//
//   BLUFF LINE (band B0 (-450,-420) -> B1 (-450, 80), face on the +X side):
//     the country WEST of x=-450 steps up in TWO terraces (12 m over
//     sd 10..-30, bench, + 8 m over sd -34..-70; ~20 m total), plateau fading
//     back to the natural field by sd=-620. Along-band envelope fades 80 m past
//     each end. Visible from the apron's west sightline (~430-470 m); does not
//     touch the +Z breach walk-out or the approach road (north). Industrial
//     Zone blend ring >=60 m clear (band north end z=80).
//
//   RAVINES (10-15 m cuts; floor half-width 4 m, walls over 11 m):
//     R1 (feeds the canyon off the bluff's south shoulder):
//        (-410, -455) bed +12 -> (-300, -560) bed +8 -> (-215, -695) bed +1
//        (mouth hangs at the canyon's C0..C1 floor grade).
//     R2 (dry gulch feeding the river's west bank):
//        ( 660, -330) bed +9 -> ( 445, -450) bed -3.4
//        (mouth notches the levee crest ~2 m ABOVE the water line — dry).
//
//   GUARDS (multiply every feature delta):
//     * facility rect guard: 0 within 60 m of the keep-out rect, 1 by 120 m.
//     * city district pads (kPads[1..3]): 0 inside r*1.7, 1 by r*1.7+60.
//     * outposts (800,400)/(-880,-320): 0 within 120 m, 1 by 170 m.
//     * ocean basin (river only): 0 within 430 m of the basin center, 1 by
//       530 m — the carve releases before the undersea disc (r80, >=350 m clear)
//       and the already-deep floor; a depth guard also skips the levee where
//       the natural floor is >=5-10 m below the bed (no underwater berms).
//
// Cost: each feature is inside an XZ bounding box (4 compares) before any
// segment math runs; the whole layer adds a handful of segment-distance
// evaluations per height sample only near the features themselves.
// ===========================================================================

// Closest approach to a polyline (XZ): min distance over the segments + the
// per-node value(s) interpolated at the closest point (used for bed/floor/
// water grades, and optionally a second channel parameter, along a channel).
// Pure; n >= 2; nv2/outV2 may be null.
inline void polyClosest(const float* nx, const float* nz, const float* nv, int n,
                        float x, float z, float& outD, float& outV,
                        const float* nv2 = nullptr, float* outV2 = nullptr) {
    float best = 1e30f, bestV = nv[0], bestV2 = nv2 ? nv2[0] : 0.0f;
    for (int i = 0; i + 1 < n; ++i) {
        const float ax = nx[i], az = nz[i], bx = nx[i+1], bz = nz[i+1];
        const float abx = bx - ax, abz = bz - az;
        const float len2 = abx * abx + abz * abz;
        float t = (len2 > 1e-6f) ? ((x - ax) * abx + (z - az) * abz) / len2 : 0.0f;
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        const float dx = x - (ax + abx * t), dz = z - (az + abz * t);
        const float d2 = dx * dx + dz * dz;
        if (d2 < best) {
            best = d2;
            bestV = nv[i] + (nv[i+1] - nv[i]) * t;
            if (nv2) bestV2 = nv2[i] + (nv2[i+1] - nv2[i]) * t;
        }
    }
    outD = std::sqrt(best);
    outV = bestV;
    if (outV2) *outV2 = bestV2;
}

// THE RIVER (see the map block above). Shared with world_regions.cpp through
// worldRiverNodes() so the water ribbon and the carve can never drift apart.
// The AUTHORED nodes below are corner-cut ONCE (Chaikin, endpoints kept) into
// the working chain both consumers use: the carve's waterline rounds a bend
// RADIALLY (distance to the polyline) while the fixed-width ribbon has
// polygonal corners — at the sharp N3 bend (~66 deg) that left a crescent of
// sub-waterline ground outside the ribbon corner. Halving the bend angles
// shrinks the residual crescent to <0.15 m of bank height (under the 0.2 m
// beach freeboard).
constexpr int kRiverNodeCount  = 9;   // N0..N8 (ribbon)
constexpr int kRiverCarveCount = 8;   // N0..N7 (terrain carve)
const float kRiverX[kRiverNodeCount] = {  780.0f, 560.0f, 410.0f, 320.0f, 360.0f, 480.0f, 620.0f, 760.0f, 900.0f };
const float kRiverZ[kRiverNodeCount] = {  180.0f, 120.0f,  40.0f, -30.0f,-300.0f,-560.0f,-830.0f,-1010.0f,-1120.0f };
const float kRiverW[kRiverNodeCount] = {    3.5f,   1.0f,  -1.8f,  -2.6f,  -3.4f,  -5.0f,  -7.5f,  -9.2f,  -9.9f };
// Per-node levee-crest height ABOVE waterY (see the map block: beach 0.4 on the
// facility-adjacent reach so the apron sightline grazes onto the water).
const float kRiverC[kRiverNodeCount] = {    2.2f,   2.2f,   0.2f,   0.2f,   0.2f,   1.4f,   2.2f,   2.2f,   2.2f };

// The working river chain: one Chaikin corner-cut pass over the authored nodes
// (interior node -> the 25% points of its two adjacent segments; endpoints
// kept). waterY stays strictly decreasing (averages of a strictly decreasing
// sequence). carveN = the chain prefix that carves terrain (through authored
// N7's pair); the tail only extends the water ribbon over the deep basin.
struct RiverChain {
    static constexpr int kMax = 2 * kRiverNodeCount;
    float x[kMax] = {}, z[kMax] = {}, w[kMax] = {}, c[kMax] = {};
    int   n = 0, carveN = 0;
    RiverChain() {
        auto push = [&](float px, float pz, float pw, float pc) {
            x[n] = px; z[n] = pz; w[n] = pw; c[n] = pc; ++n;
        };
        auto lerpNode = [&](int i, int j, float t) {
            push(kRiverX[i] + (kRiverX[j] - kRiverX[i]) * t,
                 kRiverZ[i] + (kRiverZ[j] - kRiverZ[i]) * t,
                 kRiverW[i] + (kRiverW[j] - kRiverW[i]) * t,
                 kRiverC[i] + (kRiverC[j] - kRiverC[i]) * t);
        };
        for (int i = 0; i < kRiverNodeCount; ++i) {
            if (i == 0 || i == kRiverNodeCount - 1) {
                push(kRiverX[i], kRiverZ[i], kRiverW[i], kRiverC[i]);
            } else {
                lerpNode(i, i - 1, 0.25f);
                lerpNode(i, i + 1, 0.25f);
            }
            if (i == kRiverCarveCount - 1) carveN = n;
        }
    }
};
inline const RiverChain& riverChain() { static const RiverChain kChain; return kChain; }

// CANYON PASS.
const float kCanyonX[4] = { -140.0f, -230.0f, -160.0f,  -40.0f };
const float kCanyonZ[4] = { -520.0f, -760.0f, -1040.0f, -1260.0f };
const float kCanyonF[4] = {    2.0f,    0.0f,    -2.0f,    -4.0f };

// RAVINES.
const float kRav1X[3] = { -410.0f, -300.0f, -215.0f };
const float kRav1Z[3] = { -455.0f, -560.0f, -695.0f };
const float kRav1B[3] = {   12.0f,    8.0f,    1.0f };
const float kRav2X[2] = {  660.0f,  445.0f };
const float kRav2Z[2] = { -330.0f, -450.0f };
const float kRav2B[2] = {    9.0f,   -3.4f };

// Facility graded-ground guard: 0 influence within 60 m of the keep-out rect
// (facade + apron + 150 m soil skirt — the rect app_run feeds setKeepOut), full
// influence by 120 m. Protects the pad, the apron walk, the breach walk-out,
// BOTH approach-road legs (x=22 and x=170, z<=~208 inside the rect) and the
// crash-site sightline foreground.
inline float facilityGuard(float x, float z) {
    const float dx = std::max(std::max(-156.0f - x, x - 200.0f), 0.0f);
    const float dz = std::max(std::max(-187.5f - z, z - 208.5f), 0.0f);
    return sstep(60.0f, 120.0f, std::sqrt(dx * dx + dz * dz));
}

// City-district + outpost guard: zero influence inside each pad's full blend
// ring (r*1.7) / each outpost's camp, ramping to full 60/50 m further out.
inline float contentGuard(float x, float z) {
    float g = 1.0f;
    for (int i = 1; i < 4; ++i) {   // kPads[1..3] = the city districts
        const float dx = x - kPads[i].cx, dz = z - kPads[i].cz;
        const float r  = kPads[i].r * 1.7f;
        g *= sstep(r, r + 60.0f, std::sqrt(dx * dx + dz * dz));
    }
    const float e1x = x - 800.0f, e1z = z - 400.0f;    // East Outpost
    const float e2x = x + 880.0f, e2z = z + 320.0f;    // West Outpost
    g *= sstep(120.0f, 170.0f, std::sqrt(e1x * e1x + e1z * e1z));
    g *= sstep(120.0f, 170.0f, std::sqrt(e2x * e2x + e2z * e2z));
    return g;
}

// Apply the authored landform layer (bluff -> canyon ridge+cut -> ravines ->
// river levee+cut). Pure + deterministic in (x,z); only runs when
// cfg.worldFeatures (the canonical world), so every legacy self-test config
// keeps its exact field.
float authoredLandforms(float h, float x, float z) {
    // ---- BLUFF LINE (raise; applied first so the ravine can cut its toe) ----
    if (x > -1080.0f && x < -330.0f && z > -560.0f && z < 220.0f) {
        const float a   = z - (-420.0f);                    // along the band (+Z)
        const float env = sstep(-80.0f, 20.0f, a) * (1.0f - sstep(480.0f, 580.0f, a));
        if (env > 0.0f) {
            const float sd   = x - (-450.0f);               // + east (face side), - west
            const float step = 12.0f * sstep(10.0f, -30.0f, sd)
                             +  8.0f * sstep(-34.0f, -70.0f, sd);
            const float westFade = sstep(-620.0f, -300.0f, sd);
            const float g = facilityGuard(x, z) * contentGuard(x, z);
            h += step * env * westFade * g;
        }
    }

    // ---- CANYON PASS (ridge raised along the spine, then the cut through it) ----
    if (x > -640.0f && x < 170.0f && z > -1470.0f && z < -310.0f) {
        float d, fY;
        polyClosest(kCanyonX, kCanyonZ, kCanyonF, 4, x, z, d, fY);
        if (d < 220.0f) {
            const float g = facilityGuard(x, z) * contentGuard(x, z);
            // Ridge: guaranteed walls whatever the natural country does. Faded
            // near the mouth (C0) so the pass is entered at grade from the plain.
            const float dmx = x - kCanyonX[0], dmz = z - kCanyonZ[0];
            const float ridgeEnv = sstep(40.0f, 160.0f, std::sqrt(dmx * dmx + dmz * dmz));
            h += 16.0f * (1.0f - sstep(60.0f, 210.0f, d)) * ridgeEnv * g;
            // Cut: 22 m walkable floor, walls over 15 m (~60 deg at full depth).
            const float target = fY + (h - fY) * sstep(11.0f, 26.0f, d);
            if (target < h) h += (target - h) * g;
        }
    }

    // ---- RAVINES (pure cuts, 10-15 m) ----
    if (x > -500.0f && x < -140.0f && z > -780.0f && z < -380.0f) {
        float d, bed;
        polyClosest(kRav1X, kRav1Z, kRav1B, 3, x, z, d, bed);
        if (d < 30.0f) {
            const float g = facilityGuard(x, z) * contentGuard(x, z);
            const float target = bed + (h - bed) * sstep(4.0f, 15.0f, d);
            if (target < h) h += (target - h) * g;
        }
    }
    if (x > 380.0f && x < 720.0f && z > -520.0f && z < -270.0f) {
        float d, bed;
        polyClosest(kRav2X, kRav2Z, kRav2B, 2, x, z, d, bed);
        if (d < 30.0f) {
            const float g = facilityGuard(x, z) * contentGuard(x, z);
            const float target = bed + (h - bed) * sstep(4.0f, 15.0f, d);
            if (target < h) h += (target - h) * g;
        }
    }

    // ---- THE RIVER (floodplain shelf, levee where the country is low, then
    // the channel cut) ----
    if (x > 190.0f && x < 880.0f && z > -1120.0f && z < 260.0f) {
        float d, w, c;
        const RiverChain& rc = riverChain();
        polyClosest(rc.x, rc.z, rc.w, rc.carveN, x, z, d, w, rc.c, &c);
        if (d < 130.0f) {
            const float bed = w - 3.2f, crest = w + c;
            float g = facilityGuard(x, z) * contentGuard(x, z);
            const float bx = x - kBasinCx, bz = z - kBasinCz;
            const float gSea = sstep(430.0f, 530.0f, std::sqrt(bx * bx + bz * bz));
            g *= gSea;                                     // release into the sea
            // Floodplain SHELF (see the map block): a pure LOWERING that opens
            // the apron sightline onto the beach-reach water. Looser rect guard
            // (10->60 m) by design; content guard + sea release still apply.
            {
                const float dxr = std::max(std::max(-156.0f - x, x - 200.0f), 0.0f);
                const float dzr = std::max(std::max(-187.5f - z, z - 208.5f), 0.0f);
                const float gShelf = sstep(10.0f, 60.0f, std::sqrt(dxr * dxr + dzr * dzr))
                                   * contentGuard(x, z) * gSea;
                const float strength = std::min(std::max((2.2f - c) / 2.0f, 0.0f), 1.0f);
                const float m = (1.0f - sstep(90.0f, 130.0f, d)) * strength * gShelf;
                const float shelfTarget = h + ((w + 0.2f) - h) * m;
                if (shelfTarget < h) h = shelfTarget;
            }
            if (g > 0.0f && d < 90.0f) {
                // Levee: hold the bank crest at waterY+c where the natural
                // country is lower — the water is contained everywhere. Skipped
                // where the floor is already deep underwater (no berms).
                const float leveeTarget = crest + (h - crest) * sstep(32.0f, 72.0f, d);
                const float deepSkip    = sstep(bed - 10.0f, bed - 5.0f, h);
                if (leveeTarget > h) h += (leveeTarget - h) * g * deepSkip;
                // Channel: 24 m bed, banks over 26 m (gentler than the canyon).
                const float target = bed + (h - bed) * sstep(12.0f, 38.0f, d);
                if (target < h) h += (target - h) * g;
                // DEEP CHANNEL (owner, 2026-08: "the water is 18 feet deep").
                // The waterline STAYS where it is — the bounded-water law
                // forbids raising waterY — so the BED is carved deeper
                // instead: a second, narrower cut down the spine to
                // kWorldRiverMidDrop (5.5 m = 18 ft) below the surface,
                // feathering back to the original 3.2 m shelf by 26 m out so
                // the bank shallows, levee and crests are byte-identical. The
                // bridge plan's piers sample this carved bed at boot
                // (planRiverBridge reads terrain), so the pier collars land
                // on the NEW bed with no bridge change.
                const float deepBed = w - kWorldRiverMidDrop;
                const float deepTarget = deepBed + (h - deepBed) * sstep(4.0f, 26.0f, d);
                if (deepTarget < h) h += (deepTarget - h) * g;
            }
        }
    }
    return h;
}

// Bluff terracing (see mountainHeight). Band height is the vertical spacing of
// the cliff/shelf pairs; strength 1 would be a full staircase, which reads as
// machined — 0.55 leaves the fractal relief still visible through the benches.
constexpr float kBluffStart    = 55.0f;   // m of range height before benching starts
constexpr float kBluffFull     = 90.0f;   // m over which it reaches full strength
constexpr float kBluffBandH    = 26.0f;   // vertical spacing of the bluffs
constexpr float kBluffStrength = 0.55f;
inline float clampf01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

float mountainHeight(float x, float z, uint32_t seed) {
    float h = 0.0f;
    for (int i = 0; i < kRangeCount; ++i) {
        const RangeDef& r = kRanges[i];
        const float d = segDist(x, z, r.ax, r.az, r.bx, r.bz);
        if (d >= r.outW) continue;
        const float mask = sstep(r.outW, r.coreW, d);   // 1 in the core band
        // Ridged fBm along the band => a chain of peaks with V-valleys between.
        const float n = fbm(x, z, 0.0011f, 3, seed + 7000u + (uint32_t)i * 131u);
        float ridge = 1.0f - std::fabs(2.0f * n - 1.0f);
        ridge = std::pow(ridge, r.ridgeExp);
        float hm = mask * r.amp * (0.30f + 0.70f * ridge);
        if (r.jagAmp > 0.0f) {
            const float j = fbm(x, z, 0.006f, 3, seed + 9000u + (uint32_t)i * 197u);
            hm += mask * (j - 0.5f) * 2.0f * r.jagAmp * ridge;   // jag rides the peaks
            // ROCK-SCALE RELIEF. The two terms above are a ~900 m ridge shape and
            // a ~166 m jag: both are LANDFORM frequencies, so a range built from
            // them alone reads as a smooth dune no matter how tall it is made.
            // Raising amp just makes a bigger dune. What was missing is relief at
            // the scale of an actual rock face — ~45 m and ~15 m features — which
            // is what the eye reads as "mountainous" rather than "hill".
            // Ridged (1 - |2n-1|) rather than plain fBm so it makes CRESTS and
            // gullies instead of lumps, and it rides `ridge` so the flanks stay
            // calmer than the crests, as real erosion leaves them.
            const float r1 = fbm(x, z, 0.022f, 3, seed + 11000u + (uint32_t)i * 271u);
            const float r2 = fbm(x, z, 0.061f, 2, seed + 13000u + (uint32_t)i * 331u);
            const float c1 = 1.0f - std::fabs(2.0f * r1 - 1.0f);
            const float c2 = 1.0f - std::fabs(2.0f * r2 - 1.0f);
            // NOT gated on `ridge`. The first cut multiplied this by it, which was
            // self-defeating: ridge = pow(1-|2n-1|, ridgeExp) with ridgeExp 2.5 is
            // near ZERO over most of the flanks, so the rock detail was suppressed
            // exactly where the mountain reads as a smooth dune. A real massif is
            // broken everywhere, not only along its crest line — the crest is
            // merely MORE broken. So apply it everywhere and let `ridge` add a
            // modest extra bite on top.
            const float bite = 0.55f + 0.45f * ridge;
            hm += mask * bite * r.jagAmp * (0.85f * (c1 - 0.5f) + 0.45f * (c2 - 0.5f));
            // JAGGED BLUFFS ON CRAGGY CLIFFS. Fractal relief alone gives a rough
            // but CONTINUOUS slope. Real massifs are not continuous: harder strata
            // stand out as near-vertical risers with shelves between them, which
            // is what reads as "bluffs" and "crags" rather than "rough hill".
            // Terracing does that with the height it already has — quantise into
            // bands and push each sample toward its band top, so the riser
            // steepens and the tread flattens. The smoothstep IS the cliff: a
            // linear ramp would just re-draw the slope it replaced.
            // Strength rises with altitude so the foot stays a natural talus
            // slope and only the upper massif breaks into benches.
            if (hm > kBluffStart) {
                const float up01 = clampf01((hm - kBluffStart) / kBluffFull);
                // SCALE-VARIED STRATA (owner 2026-08-16: "stepped bluffs").
                // A fixed 26 m band spacing benched every face at the same
                // vertical rhythm, which is the tell of a machined terrace, not
                // strata. Real bed thickness varies ACROSS a massif (thin
                // shales here, massive sandstone there), so the band height is
                // modulated ~19..33 m by a ~1 km-wavelength noise — locally
                // coherent (one face keeps one rhythm; sstep continuity in f is
                // untouched), globally varied (no two shoulders band alike).
                // Same field also wanders the band PHASE (hm/bandH shifts with
                // bandH), so shelf lines are not world-height contours. Pure in
                // (x,z,seed): the corridor refine and every carve sit on top of
                // this bit-stably, exactly as before.
                const float sv    = fbm(x, z, 0.0011f, 2, seed + 15000u + (uint32_t)i * 389u);
                const float bandH = kBluffBandH * (0.72f + 0.56f * sv);
                const float band = hm / bandH;
                const float f    = band - std::floor(band);
                const float shaped = sstep(0.42f, 0.92f, f);
                hm += bandH * (shaped - f) * kBluffStrength * up01 * mask;
            }
        }
        if (r.capY > 0.0f && hm > r.capY) hm = r.capY;           // mesa flat tops
        if (hm > 0.0f) h += hm;
    }
    return h;
}

// ===========================================================================
// TERRAIN CORRIDOR DEPRESSION — see the block comment on TerrainCorridor in
// app/terrain.h for the API contract and the BL provenance of the technique.
//
// This is the POLYLINE generalization of the river carve above: the river carve
// is a fixed-profile channel following one authored spline with a levee, a
// floodplain shelf and a pile of content guards; a corridor is the bare
// primitive — closest approach to an ordered point list, a flat floor of
// half-width `halfWidth`, and a smoothstep shoulder `falloff` wide. It shares
// the river's exact style: pure, deterministic, bbox-guarded, `sstep`-shaped,
// and combined into h by LOWERING only (never raising).
//
// GEOMETRY, precisely — the corridor is the UNION of one capsule per segment:
//   per segment i:  t_i   = clamp(projection of p onto the segment, 0, 1)
//                   d_i   = |p - segment_i(t_i)|            (exact, clamped ends)
//                   dep_i = depth[i] + (depth[i+1]-depth[i]) * t_i
//                   w_i   = 1 - smoothstep(halfWidth, halfWidth+falloff, d_i)
//                   c_i   = dep_i * w_i
//   depth(p) = max_i c_i          delta = -depth(p)   (<= 0; a pure lowering)
//
// WHY MAX-OF-SEGMENTS AND NOT MIN-DISTANCE-THEN-PROFILE (this is the whole
// reason there is no crease): the obvious formulation — find the single closest
// point on the polyline, then read the profile there — has a genuine VALUE
// DISCONTINUITY on the medial axis inside a bend. On that bisector two feet are
// exactly equidistant but sit at different arc positions, so they carry
// different profile depths, and the winner-takes-all switch steps the ground by
// |slope difference| * distance-from-joint. It is invisible on a flat profile
// and a hard visible step as soon as the corridor grades. Taking the MAX of
// per-segment contributions removes it by construction: each c_i is continuous
// everywhere (t_i, d_i and dep_i all are), and the max of continuous functions
// is continuous — at any switch the two contributions are EQUAL by definition.
//
// It also gives, for free:
//   * no double-dig at a joint. Both adjacent segments evaluate the joint to
//     exactly depth[joint] (t=1 and t=0 of the shared node), so max = the node's
//     own depth. A SUM (or BL's overlapping per-sample circle stamps) digs ~2x
//     there — the classic corridor-carve artifact.
//   * a Lipschitz bound the self-test can assert against:
//       |grad depth| <= max|d(depth)/ds| + maxDepth * 1.5/falloff
//     (smoothstep' peaks at 1.5; the distance field is 1-Lipschitz; max of
//     L-Lipschitz functions is L-Lipschitz).
//   * rounded end caps, so the floor is a proper swept corridor, and the flat
//     floor extends halfWidth past a node into the next reach (the union, not a
//     kink). That is the one place the union differs from "the profile" on the
//     spine: within ~halfWidth of a node the deeper neighbour wins. Deliberate,
//     and asserted in the self-test.
//
// COST: the comparison runs on SQUARED distance; sqrt is taken only for the
// segments that actually land in the shoulder band (inside the flat floor w is
// exactly 1 and outside the reach the contribution is exactly 0 — both skip it).
// ===========================================================================

// One registered corridor + its precomputed XZ bounding box (expanded by the
// full influence radius halfWidth+falloff). The box is a PURE ACCELERATOR: a
// point outside it is provably >= halfWidth+falloff from every segment (all the
// nodes are inside the unexpanded box), where w(d) is exactly 0 — so skipping it
// cannot change a single bit of the result.
struct CorridorRec {
    TerrainCorridor c;
    float minX = 0.0f, minZ = 0.0f, maxX = 0.0f, maxZ = 0.0f;
};

// The registry: a FIXED-CAPACITY array, no dynamic allocation (same shape as
// kRanges/kPads above). Written only by registerTerrainCorridor/clear at boot;
// read-only from every generation thread thereafter.
struct CorridorRegistry {
    CorridorRec rec[kMaxTerrainCorridors];
    uint32_t    count = 0;
};
CorridorRegistry& corridorRegistry() {
    static CorridorRegistry kReg;
    return kReg;
}

// Exact SQUARED distance from p to segment i of a corridor, plus the depth
// profile interpolated at the clamped closest point. Pure; i is a valid segment.
inline void corridorSegment(const TerrainCorridor& c, int i, float x, float z,
                            float& outD2, float& outDepth) {
    const float ax = c.x[i], az = c.z[i];
    const float abx = c.x[i+1] - ax, abz = c.z[i+1] - az;
    const float len2 = abx * abx + abz * abz;
    // Degenerate (repeated) control point: collapse to the node itself rather
    // than dividing by ~0 — a duplicated point must never spike or produce NaN.
    float t = (len2 > 1e-12f) ? ((x - ax) * abx + (z - az) * abz) / len2 : 0.0f;
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);   // clamp => rounded end caps
    const float dx = x - (ax + abx * t), dz = z - (az + abz * t);
    outD2    = dx * dx + dz * dz;
    outDepth = c.depth[i] + (c.depth[i+1] - c.depth[i]) * t;
}

} // namespace

// ---------------------------------------------------------------------------
// Corridor depression — public surface (see app/terrain.h).
// ---------------------------------------------------------------------------
float terrainCorridorDepthAt(const TerrainCorridor& c, float x, float z) {
    if (c.nodeCount < 2) return 0.0f;
    const float reach  = c.halfWidth + c.falloff;
    const float reach2 = reach * reach;
    const float flat2  = c.halfWidth * c.halfWidth;
    float best = 0.0f;                             // never RAISES, so 0 is the floor
    for (int i = 0; i + 1 < c.nodeCount; ++i) {
        float d2, depth;
        corridorSegment(c, i, x, z, d2, depth);
        if (d2 >= reach2) continue;                // outside the shoulder: exactly 0
        // w == 1 on the flat floor (no sqrt); smoothstep only in the shoulder.
        const float cont = (d2 <= flat2)
            ? depth
            : depth * (1.0f - sstep(c.halfWidth, reach, std::sqrt(d2)));
        if (cont > best) best = cont;              // UNION (max), never a sum
    }
    return best;
}

bool registerTerrainCorridor(const TerrainCorridor& c) {
    CorridorRegistry& reg = corridorRegistry();
    if (reg.count >= kMaxTerrainCorridors) return false;
    if (c.nodeCount < 2 || c.nodeCount > TerrainCorridor::kMaxNodes) return false;
    if (!(c.halfWidth >= 0.0f) || !(c.falloff >= 0.0f)) return false;   // also rejects NaN
    if (c.halfWidth + c.falloff <= 0.0f) return false;                  // zero influence
    for (int i = 0; i < c.nodeCount; ++i)
        if (!std::isfinite(c.x[i]) || !std::isfinite(c.z[i]) || !std::isfinite(c.depth[i]))
            return false;

    CorridorRec& r = reg.rec[reg.count];
    r.c = c;
    r.minX = r.maxX = c.x[0];
    r.minZ = r.maxZ = c.z[0];
    for (int i = 1; i < c.nodeCount; ++i) {
        r.minX = std::min(r.minX, c.x[i]); r.maxX = std::max(r.maxX, c.x[i]);
        r.minZ = std::min(r.minZ, c.z[i]); r.maxZ = std::max(r.maxZ, c.z[i]);
    }
    const float reach = c.halfWidth + c.falloff;
    r.minX -= reach; r.maxX += reach;
    r.minZ -= reach; r.maxZ += reach;
    ++reg.count;
    return true;
}

void clearTerrainCorridors() { corridorRegistry().count = 0; }

uint32_t terrainCorridorCount() { return corridorRegistry().count; }

// ---------------------------------------------------------------------------
// Portal holes — the mesh-level mouth exclusion (see app/terrain.h). Same
// fixed-capacity boot-registered shape as the corridor registry, with a
// precomputed XZ bounding box per hole for the universal early-out.
// ---------------------------------------------------------------------------
namespace {
struct PortalHoleRec {
    TerrainPortalHole h;
    float minX = 0, maxX = 0, minZ = 0, maxZ = 0;
};
struct PortalHoleRegistry {
    PortalHoleRec rec[kMaxTerrainPortalHoles];
    uint32_t      count = 0;
};
PortalHoleRegistry& portalHoleRegistry() {
    static PortalHoleRegistry kReg;
    return kReg;
}
} // namespace

bool registerTerrainPortalHole(const TerrainPortalHole& h) {
    PortalHoleRegistry& reg = portalHoleRegistry();
    if (reg.count >= kMaxTerrainPortalHoles) return false;
    if (!std::isfinite(h.x0) || !std::isfinite(h.z0) ||
        !std::isfinite(h.x1) || !std::isfinite(h.z1) ||
        !std::isfinite(h.halfWidth) || !std::isfinite(h.yTop)) return false;
    if (!(h.halfWidth > 0.0f)) return false;
    const float dx = h.x1 - h.x0, dz = h.z1 - h.z0;
    if (dx * dx + dz * dz < 1e-6f) return false;      // zero-length spine
    PortalHoleRec& r = reg.rec[reg.count];
    r.h = h;
    r.minX = std::min(h.x0, h.x1) - h.halfWidth;
    r.maxX = std::max(h.x0, h.x1) + h.halfWidth;
    r.minZ = std::min(h.z0, h.z1) - h.halfWidth;
    r.maxZ = std::max(h.z0, h.z1) + h.halfWidth;
    ++reg.count;
    return true;
}

void clearTerrainPortalHoles() { portalHoleRegistry().count = 0; }

uint32_t terrainPortalHoleCount() { return portalHoleRegistry().count; }

bool terrainPortalHoleDrops(float cx, float cz, float minY) {
    const PortalHoleRegistry& reg = portalHoleRegistry();
    if (reg.count == 0) return false;                 // the universal fast path
    for (uint32_t i = 0; i < reg.count; ++i) {
        const PortalHoleRec& r = reg.rec[i];
        if (cx < r.minX || cx > r.maxX || cz < r.minZ || cz > r.maxZ) continue;
        if (minY >= r.h.yTop) continue;               // whole tri above the tube
        // Inside the prism? Project onto the spine segment; RECTANGULAR ends
        // (t clamped to [0,1] would round them; a mouth wants a hard edge so
        // the kept lid resumes exactly where the shell no longer covers).
        const float ax = r.h.x0, az = r.h.z0;
        const float abx = r.h.x1 - ax, abz = r.h.z1 - az;
        const float len2 = abx * abx + abz * abz;
        const float t = ((cx - ax) * abx + (cz - az) * abz) / len2;
        if (t < 0.0f || t > 1.0f) continue;
        const float px = ax + abx * t, pz = az + abz * t;
        const float dx = cx - px, dz = cz - pz;
        if (dx * dx + dz * dz <= r.h.halfWidth * r.h.halfWidth) return true;
    }
    return false;
}

float terrainCorridorDelta(float x, float z) {
    const CorridorRegistry& reg = corridorRegistry();
    if (reg.count == 0) return 0.0f;               // the universal fast path
    float deepest = 0.0f;                          // meters BELOW the surface
    for (uint32_t i = 0; i < reg.count; ++i) {
        const CorridorRec& r = reg.rec[i];
        // Early-out: 4 compares, and terrain far from every corridor pays only
        // this. Provably lossless (see CorridorRec).
        if (x < r.minX || x > r.maxX || z < r.minZ || z > r.maxZ) continue;
        const float d = terrainCorridorDepthAt(r.c, x, z);
        if (d > deepest) deepest = d;              // deepest wins; never a sum
    }
    return -deepest;
}

bool terrainCorridorContains(float x, float z) {
    const CorridorRegistry& reg = corridorRegistry();
    if (reg.count == 0) return false;              // the universal fast path
    for (uint32_t i = 0; i < reg.count; ++i) {
        const CorridorRec& r = reg.rec[i];
        if (x < r.minX || x > r.maxX || z < r.minZ || z > r.maxZ) continue;
        const TerrainCorridor& c = r.c;
        const float reach2 = (c.halfWidth + c.falloff) * (c.halfWidth + c.falloff);
        for (int s = 0; s + 1 < c.nodeCount; ++s) {
            float d2, depth;
            corridorSegment(c, s, x, z, d2, depth);
            if (d2 < reach2) return true;          // depth deliberately ignored
        }
    }
    return false;
}

void terrainCorridorSegmentsNearRect(float minX, float minZ, float maxX, float maxZ,
                                     std::vector<TerrainCorridorSegRef>& out) {
    const CorridorRegistry& reg = corridorRegistry();
    for (uint32_t i = 0; i < reg.count; ++i) {
        const CorridorRec& r = reg.rec[i];
        if (maxX < r.minX || minX > r.maxX || maxZ < r.minZ || minZ > r.maxZ) continue;
        const TerrainCorridor& c = r.c;
        const float reach = c.halfWidth + c.falloff;
        for (int s = 0; s + 1 < c.nodeCount; ++s) {
            const float ax = c.x[s], az = c.z[s], bx = c.x[s + 1], bz = c.z[s + 1];
            if (std::max(ax, bx) + reach < minX || std::min(ax, bx) - reach > maxX ||
                std::max(az, bz) + reach < minZ || std::min(az, bz) - reach > maxZ)
                continue;
            out.push_back({ ax, az, bx, bz, c.halfWidth, reach,
                            c.depth[s], c.depth[s + 1] });
        }
    }
}

// SURVEY INSTRUMENT — see terrain.h. Rasterise every surface triangle of the
// REAL emitted mesh over a 0.5 m grid, keep the samples on corridor FLAT FLOOR
// (>= 0.5 m inside halfWidth, carve depth > 0.3 m), and report the worst
// (meshY - trueFieldY). This is how the spawn-road green wedge was measured
// (a field query cannot see it: the field is correct, the MESH stands above it).
float terrainTileCorridorWedge(const TerrainConfig& cfg, float originX, float originZ,
                               TerrainLod lod, float* outWorstX, float* outWorstZ,
                               const float* refineFocusXZ) {
    std::vector<TerrainCorridorSegRef> segs;
    terrainCorridorSegmentsNearRect(originX, originZ,
                                    originX + cfg.tileSize, originZ + cfg.tileSize, segs);
    if (segs.empty()) return 0.0f;
    auto onFloor = [&](float x, float z) {
        for (const TerrainCorridorSegRef& s : segs) {
            const float hw = s.halfWidth - 0.5f;
            if (hw <= 0.0f) continue;
            const float abx = s.bx - s.ax, abz = s.bz - s.az;
            const float len2 = abx * abx + abz * abz;
            float t = (len2 > 1e-12f) ? ((x - s.ax) * abx + (z - s.az) * abz) / len2 : 0.0f;
            t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
            const float dx = x - (s.ax + abx * t), dz = z - (s.az + abz * t);
            if (dx * dx + dz * dz > hw * hw) continue;
            if (s.depth0 + (s.depth1 - s.depth0) * t > 0.3f) return true;
        }
        return false;
    };
    std::vector<x3::rhi::MeshVertex> verts;
    std::vector<uint32_t> idx;
    uint32_t surfIdxCount = 0;
    buildTileMeshAbs(cfg, originX, originZ, lod, verts, idx, &surfIdxCount,
                     refineFocusXZ);
    const float stepM = 0.5f;
    float worst = 0.0f;
    for (uint32_t t = 0; t + 2 < surfIdxCount; t += 3) {
        const x3::rhi::MeshVertex& A = verts[idx[t]];
        const x3::rhi::MeshVertex& B = verts[idx[t + 1]];
        const x3::rhi::MeshVertex& C = verts[idx[t + 2]];
        const float minX = std::min(A.pos[0], std::min(B.pos[0], C.pos[0]));
        const float maxX = std::max(A.pos[0], std::max(B.pos[0], C.pos[0]));
        const float minZ = std::min(A.pos[2], std::min(B.pos[2], C.pos[2]));
        const float maxZ = std::max(A.pos[2], std::max(B.pos[2], C.pos[2]));
        const float den = (B.pos[0] - A.pos[0]) * (C.pos[2] - A.pos[2]) -
                          (C.pos[0] - A.pos[0]) * (B.pos[2] - A.pos[2]);
        if (std::fabs(den) < 1e-6f) continue;
        for (float sz = std::ceil(minZ / stepM) * stepM; sz <= maxZ; sz += stepM) {
            for (float sx = std::ceil(minX / stepM) * stepM; sx <= maxX; sx += stepM) {
                const float w0 = ((B.pos[0] - sx) * (C.pos[2] - sz) -
                                  (C.pos[0] - sx) * (B.pos[2] - sz)) / den;
                const float w1 = ((C.pos[0] - sx) * (A.pos[2] - sz) -
                                  (A.pos[0] - sx) * (C.pos[2] - sz)) / den;
                const float w2 = 1.0f - w0 - w1;
                if (w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f) continue;
                if (!onFloor(sx, sz)) continue;
                const float meshY = w0 * A.pos[1] + w1 * B.pos[1] + w2 * C.pos[1];
                const float err = meshY - terrainHeightAt(cfg, sx, sz);
                if (err > worst) {
                    worst = err;
                    if (outWorstX) *outWorstX = sx;
                    if (outWorstZ) *outWorstZ = sz;
                }
            }
        }
    }
    return worst;
}

namespace {
// Fold the registered corridors into a height. The `== 0` guard is deliberate,
// not defensive: it guarantees the returned float is the SAME OBJECT, bit for
// bit, whenever no corridor influences the point — which is what makes "no
// global regression" a bit-exact property rather than an epsilon one.
inline float applyCorridors(float h, float x, float z) {
    const float d = terrainCorridorDelta(x, z);
    return (d == 0.0f) ? h : h + d;
}
} // namespace

// ---------------------------------------------------------------------------
// Public height sampler. Base field: gentle rolling hills — an fBm field shaped
// by a mild power curve scaled to [0, heightScale]. With cfg.worldFeatures the
// canonical world-map layers (relief/mountains/pads/basin) apply on top.
// Pure function of the config + (x,z).
// ---------------------------------------------------------------------------
float terrainHeightAt(const TerrainConfig& cfg, float worldX, float worldZ) {
    float h = fbm(worldX, worldZ, cfg.noiseFreq, cfg.octaves, cfg.seed);
    h = h * h * (3.0f - 2.0f * h);
    h *= cfg.heightScale;
    if (!cfg.worldFeatures) return applyCorridors(h, worldX, worldZ);

    // MACRO RELIEF: modulate the base amplitude by a very-low-frequency field so
    // some country is near-flat plain and some carries the full hill height.
    // Factor stays in [0.35, 1.0] — the base field never exceeds heightScale.
    const float macro = fbm(worldX, worldZ, 0.00028f, 2, cfg.seed ^ 0x5157u);
    h *= 0.35f + 0.65f * sstep(0.25f, 0.75f, macro);

    // MOUNTAIN RANGES (adds above heightScale by design).
    h += mountainHeight(worldX, worldZ, cfg.seed);

    // OCEAN BASIN: shore falloff into the offshore water, deepening over the
    // undersea base so the disc sits in a real pit under the water plane.
    {
        const float dx = worldX - kBasinCx, dz = worldZ - kBasinCz;
        const float d = std::sqrt(dx * dx + dz * dz);
        if (d < 950.0f) {
            const float t = sstep(950.0f, 300.0f, d);          // 1 at the core
            const float target = -6.0f - 84.0f * sstep(0.35f, 1.0f, t);  // -6 shore .. -90 core
            h = h + (target - h) * t;
        }
    }

    // FLAT PADS (facility + city districts) — applied last so streets win.
    for (int i = 0; i < 4; ++i) {
        const PadDef& p = kPads[i];
        const float dx = worldX - p.cx, dz = worldZ - p.cz;
        const float d = std::sqrt(dx * dx + dz * dz);
        const float outR = p.r * 1.7f;
        if (d < outR) {
            const float t = sstep(outR, p.r, d);               // 1 inside the pad
            h = h + (p.padY - h) * t;
        }
    }

    // W9 — AUTHORED LANDFORM DRAMA (canyon pass / bluffs / ravines / THE RIVER).
    // After the pads so a channel may cross a pad's outer blend ring; every
    // feature is internally guarded to zero near the graded content itself.
    h = authoredLandforms(h, worldX, worldZ);

    // TERRAIN CORRIDOR DEPRESSION — LAST, so a registered corridor wins over
    // every authored layer (a tunnel bore must be able to cut through a pad's
    // blend ring or a mountain flank; the road it serves is pinned flat and does
    // not move). Exactly zero-cost + bit-neutral when nothing is registered.
    return applyCorridors(h, worldX, worldZ);
}

// ---------------------------------------------------------------------------
// W9 — THE RIVER's authored spline, exported for the water-surface ribbon
// (world_regions.cpp). Same table the carve uses — see kRiver* above.
// ---------------------------------------------------------------------------
const WorldRiverNode* worldRiverNodes(uint32_t& count) {
    static const std::vector<WorldRiverNode> kNodes = [] {
        const RiverChain& rc = riverChain();
        std::vector<WorldRiverNode> v;
        v.reserve((size_t)rc.n);
        for (int i = 0; i < rc.n; ++i) v.push_back({ rc.x[i], rc.z[i], rc.w[i] });
        return v;
    }();
    count = (uint32_t)kNodes.size();
    return kNodes.data();
}

uint32_t worldRiverCarveCount() { return (uint32_t)riverChain().carveN; }

// ---------------------------------------------------------------------------
// W10 (SWIMMING) — worldWaterLevelAt. Pure; see terrain.h. River first (its
// ribbon rides 0.1 m proud of the sea where they overlap at the estuary, so
// the river answer wins there, matching the visuals), then the ocean basin.
// ---------------------------------------------------------------------------
float worldWaterLevelAt(float x, float z) {
    // RIVER: closest approach to the SAME working chain the carve + ribbon use
    // (full chain incl. the estuary tail nodes that only carry water).
    const RiverChain& rc = riverChain();
    float d, w;
    polyClosest(rc.x, rc.z, rc.w, rc.n, x, z, d, w);
    if (d <= kWorldRiverHalfWidth) return w;
    // OCEAN: inside the offshore basin's influence ring AND the terrain bowl is
    // actually below the sea surface there (the -6 shore ring stays dry beach).
    const float bx = x - kBasinCx, bz = z - kBasinCz;
    if (bx * bx + bz * bz < 950.0f * 950.0f &&
        terrainHeightAtWorld(x, z) < kWorldSeaLevel)
        return kWorldSeaLevel;
    return kWorldWaterDry;
}

// ---------------------------------------------------------------------------
// Placement API — the single canonical world config + the height/normal/place
// helpers the 14900k anchors buildings (the Spire) + the cliffside pad with.
// The config is the engine TerrainConfig defaults (matching what `--world
// terrain`/`--world ocean` build the streamer from), exposed by const-ref so a
// query and the rendered/streamed surface always agree.
// ---------------------------------------------------------------------------
const TerrainConfig& worldTerrainConfig() {
    // Defaults = the streamed world's config, with the CANONICAL WORLD FEATURES
    // layer ON (mountain ranges / macro relief / city pads / ocean basin). Tests
    // that build their own TerrainConfig keep worldFeatures=false (legacy field).
    static const TerrainConfig kWorld = [] {
        TerrainConfig c{};
        c.worldFeatures = true;
        return c;
    }();
    return kWorld;
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

x3::rhi::TextureHandle makeTerrainSplatMarker(x3::rhi::IRenderDevice& device) {
    return makeGroundTexture(device);
}

// ---------------------------------------------------------------------------
// W8-3 — the horizon ring (far-terrain stitch). One static polar-grid mesh
// sampled from the CANONICAL field (worldTerrainConfig), geometric ring
// spacing, recessed by yBias so streamed LOD0 tiles win where they overlap.
// ---------------------------------------------------------------------------
uint32_t addTerrainHorizonRing(Scene& scene, x3::rhi::IRenderDevice& device,
                               x3::rhi::TextureHandle splatMarker,
                               const HorizonRingDesc& d) {
    if (d.rings < 2 || d.segments < 8 || d.rInner <= 0.0f || d.rOuter <= d.rInner)
        return kNoLink;

    const uint32_t nR = d.rings + 1, nS = d.segments + 1;
    const float logRatio = std::log(d.rOuter / d.rInner);

    std::vector<x3::rhi::MeshVertex> verts;
    std::vector<uint32_t> idx;
    verts.reserve((size_t)nR * nS);
    idx.reserve((size_t)d.rings * d.segments * 6);

    auto ringHeight = [&](float wx, float wz, float r) -> float {
        float h = terrainHeightAtWorld(wx, wz);
        if (d.flatten) {
            const float t = sstep(d.rInner, d.flattenBlendR, r);   // 0 at inner -> 1 out
            h = d.flattenY + (h - d.flattenY) * t;
        }
        return h + d.yBias;
    };

    for (uint32_t ri = 0; ri < nR; ++ri) {
        const float fr = (float)ri / (float)d.rings;
        const float r  = d.rInner * std::exp(logRatio * fr);
        // Local half-cell sizes: radial ring spacing + tangential arc length.
        const float drHalf = r * (std::exp(logRatio / (float)d.rings) - 1.0f) * 0.5f;
        const float daHalf = r * (6.2831853f / (float)d.segments) * 0.5f;
        // Normal sampling eps scales with the local cell size (coarse far out).
        const float eps = std::max(0.75f, drHalf * 0.5f);
        for (uint32_t si = 0; si < nS; ++si) {
            const float a  = (float)(si % d.segments) / (float)d.segments * 6.2831853f;
            const float ca = std::cos(a), sa = std::sin(a);
            const float wx = d.centerX + ca * r;
            const float wz = d.centerZ + sa * r;
            // LOWER-ENVELOPE height: min of the vertex sample + its half-cell
            // neighbors, so the coarse mesh's linear interpolation stays AT or
            // BELOW the true field between samples — the ring never pokes up
            // through the full-detail streamed tiles or reads as floating
            // sheets in valleys. Blended in with radius: near the center the
            // ring IS the ground (the city host) and must track the field.
            const float hC = ringHeight(wx, wz, r);
            float hMin = hC;
            hMin = std::min(hMin, ringHeight(wx + ca * drHalf, wz + sa * drHalf, r));
            hMin = std::min(hMin, ringHeight(wx - ca * drHalf, wz - sa * drHalf, r));
            hMin = std::min(hMin, ringHeight(wx - sa * daHalf, wz + ca * daHalf, r));
            hMin = std::min(hMin, ringHeight(wx + sa * daHalf, wz - ca * daHalf, r));
            const float env = sstep(150.0f, 500.0f, r);   // envelope strength by radius
            const float h = hC + (hMin - hC) * env;
            const float hl = ringHeight(wx - eps, wz, r);
            const float hr = ringHeight(wx + eps, wz, r);
            const float hd = ringHeight(wx, wz - eps, r);
            const float hu = ringHeight(wx, wz + eps, r);
            float nx = (hl - hr), nz = (hd - hu), ny = 2.0f * eps;
            const float inv = 1.0f / std::sqrt(nx * nx + ny * ny + nz * nz);
            x3::rhi::MeshVertex v{};
            v.pos[0] = wx; v.pos[1] = h; v.pos[2] = wz;
            v.normal[0] = nx * inv; v.normal[1] = ny * inv; v.normal[2] = nz * inv;
            v.uv[0] = wx * 0.125f; v.uv[1] = wz * 0.125f;   // unused: splat is world-space
            verts.push_back(v);
        }
    }
    for (uint32_t ri = 0; ri < d.rings; ++ri) {
        for (uint32_t si = 0; si < d.segments; ++si) {
            const uint32_t a = ri * nS + si;
            const uint32_t b = a + 1;
            const uint32_t c = a + nS;
            const uint32_t e = c + 1;
            // Wind CCW as seen from +Y: tangential (b-a) x radial (c-a) => the
            // face normal points UP (the first cut wound these downward — the
            // whole ring rendered as backfaces: white/black sheets).
            idx.insert(idx.end(), { a, b, c,  b, e, c });
        }
    }

    Entity e;
    e.mesh = device.createMesh(verts.data(), (uint32_t)verts.size(),
                               idx.data(), (uint32_t)idx.size());
    e.tex = splatMarker;
    e.baseColor[0] = e.baseColor[1] = e.baseColor[2] = e.baseColor[3] = 1.0f;
    for (int i = 0; i < 16; ++i) e.transform[i] = kIdentity[i];
    e.tag = (uint32_t)Tag::Static;
    e.visible = true;
    return scene.add(e);
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
            t.corridorPin = tileTouchesCorridor(t.originX, t.originZ, m_cfg.tileSize);

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

            // Collision: LOD0 top surface only (skirts excluded). The surface
            // index count comes from the builder — it is only quads*quads*6
            // when no portal hole dropped any triangle.
            {
                uint32_t surfIdx = 0;
                buildTileMeshAbs(m_cfg, t.originX, t.originZ, TerrainLod::Full, verts, idx,
                                 &surfIdx);
                const uint32_t quads = (m_cfg.tileVerts - 1);
                const uint32_t vpe   = quads + 1;
                const uint32_t surfVerts = vpe * vpe;
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
        TerrainLod want = lodForDist(m_cfg, dist);
        // The seam fix, SCOPED (26 fps lesson): pinning every corridor tile
        // at every distance tripled the triangle load — 46 miles of road all
        // at full res. The wedge only reads near the camera; past 420 m it is
        // sub-pixel. Near pin stays absolute.
        if (corridorPinEnabled() && t.corridorPin && dist < 420.0f)
            want = TerrainLod::Full;
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
    float                          focusX = 0.0f, focusZ = 0.0f;   // request focus
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
                               float focusX, float focusZ, TileGenResult& out) {
    out.gx = gx; out.gz = gz;
    out.originX = gx * cfg.tileSize;
    out.originZ = gz * cfg.tileSize;
    out.centerX = out.originX + cfg.tileSize * 0.5f;
    out.centerZ = out.originZ + cfg.tileSize * 0.5f;

    const float focusXZ[2] = { focusX, focusZ };   // corridor-refine scope
    uint32_t surfIdx = 0;   // LOD0 surface index count (holes may shrink it)
    for (int l = 0; l < (int)TerrainLod::Count; ++l) {
        buildTileMeshAbs(cfg, out.originX, out.originZ, (TerrainLod)l,
                         out.lodVerts[l], out.lodIdx[l],
                         l == 0 ? &surfIdx : nullptr, focusXZ);
    }

    // Collision: LOD0 top surface only (first vpe*vpe verts / surfIdx indices).
    const uint32_t quads = (cfg.tileVerts - 1);
    const uint32_t vpe   = quads + 1;
    const uint32_t surfVerts = vpe * vpe;
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
    generate(self->m_cfg, job->gx, job->gz, job->focusX, job->focusZ, *job->result);
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

    // SEAM 3: tiles FULLY inside the keep-out rect are never generated — the
    // canon facility's own ground (interior floors / apron ring / soil skirt)
    // covers that area, and a Y=0 pad tile under it would z-fight coplanarly
    // (see setKeepOut). Intersecting edge tiles still generate.
    if (m_keepOutOn) {
        const float tx0 = (float)gx * m_cfg.tileSize, tx1 = tx0 + m_cfg.tileSize;
        const float tz0 = (float)gz * m_cfg.tileSize, tz1 = tz0 + m_cfg.tileSize;
        if (tx0 >= m_keepOut[0] && tx1 <= m_keepOut[2] &&
            tz0 >= m_keepOut[1] && tz1 <= m_keepOut[3]) return;
    }

    if (synchronous || !m_jobs) {
        // Generate inline now (used for the under-player neighborhood + the no-job
        // headless mode). The upload still happens on the main thread in update().
        // Counted as in-flight (an outstanding result to drain) so the drain's
        // per-result --m_inFlight stays balanced across sync + async alike.
        auto r = obtainResult();
        generate(m_cfg, gx, gz, m_reqFocusX, m_reqFocusZ, *r);
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
    job->focusX = m_reqFocusX; job->focusZ = m_reqFocusZ;
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
    t.corridorPin = tileTouchesCorridor(t.originX, t.originZ, m_cfg.tileSize);   // seam fix
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
    m_reqFocusX = focusX; m_reqFocusZ = focusZ;   // corridor-refine scope

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
    m_reqFocusX = focusX; m_reqFocusZ = focusZ;   // corridor-refine scope
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
        TerrainLod want = lodForDist(m_cfg, dist);
        // The seam fix, SCOPED (26 fps lesson): pinning every corridor tile
        // at every distance tripled the triangle load — 46 miles of road all
        // at full res. The wedge only reads near the camera; past 420 m it is
        // sub-pixel. Near pin stays absolute.
        if (corridorPinEnabled() && t.corridorPin && dist < 420.0f)
            want = TerrainLod::Full;
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

    // ---- T4: RIDGE SURVIVAL at coarse LOD (task #26 — the blade towers) ------
    // Point-sampling at the 4 m Quarter stride drops any crest narrower than
    // the stride: the ridge vanishes except where a vertex lands on it — thin
    // vertical blades on every distant mountain. Gate: walk the tunnel ridge's
    // spine (the narrowest authored range), find each covering tile's true
    // crest (max field over the LOD0 lattice, interior cells only), and demand
    // the REAL Quarter mesh carries a vertex within one coarse cell of it at
    // >= that height (the ridge filter's tiling-blocks guarantee). Pre-fix
    // (X3_NO_RIDGE_FILTER=1) this measures the blades: metres of lost crest.
    {
        const TerrainConfig& wcfg = worldTerrainConfig();
        const float ax = -753.0f, az = -740.0f, bx = -431.0f, bz = 36.0f; // kRanges[4]
        const float ts = wcfg.tileSize;
        const float fine = ts / (float)(wcfg.tileVerts - 1);
        float worstDeficit = -1e9f, wpx = 0.0f, wpz = 0.0f;
        std::vector<x3::rhi::MeshVertex> mv; std::vector<uint32_t> mi;
        for (int si = 1; si <= 11; ++si) {
            const float t = (float)si / 12.0f;
            const float sx = ax + (bx - ax) * t, sz = az + (bz - az) * t;
            const float tx = std::floor(sx / ts) * ts, tz = std::floor(sz / ts) * ts;
            // true crest over the tile's INTERIOR (>= 1 Quarter cell from the
            // border — border verts stay point samples by design)
            const float cellQ = ts / (float)((wcfg.tileVerts - 1) / 4);
            float crest = -1e9f, px = 0.0f, pz = 0.0f;
            for (uint32_t fj = 0; fj < wcfg.tileVerts; ++fj)
                for (uint32_t fi = 0; fi < wcfg.tileVerts; ++fi) {
                    const float x = tx + fi * fine, z = tz + fj * fine;
                    if (x < tx + cellQ || x > tx + ts - cellQ ||
                        z < tz + cellQ || z > tz + ts - cellQ) continue;
                    const float h = terrainHeightAt(wcfg, x, z);
                    if (h > crest) { crest = h; px = x; pz = z; }
                }
            if (crest < -1e8f) continue;
            mv.clear(); mi.clear();
            buildTileMeshAbs(wcfg, tx, tz, TerrainLod::Quarter, mv, mi);
            const uint32_t vpeQ = (wcfg.tileVerts - 1) / 4 + 1;
            float best = -1e9f;
            for (uint32_t v = 0; v < vpeQ * vpeQ; ++v) {
                if (std::fabs(mv[v].pos[0] - px) > cellQ + 0.01f ||
                    std::fabs(mv[v].pos[2] - pz) > cellQ + 0.01f) continue;
                best = std::max(best, mv[v].pos[1]);
            }
            const float deficit = crest - best;
            if (deficit > worstDeficit) { worstDeficit = deficit; wpx = px; wpz = pz; }
        }
        x3::logInfo("[terrain-test] T4 worst Quarter-LOD crest deficit along the "
                    "tunnel ridge: " + std::to_string(worstDeficit) + " m at (" +
                    std::to_string(wpx) + ", " + std::to_string(wpz) + ")");
        check(worstDeficit <= 0.01f,
              "T4 narrow ridge crests survive Quarter LOD (no blade towers)");
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

// ===========================================================================
// Headless CORRIDOR-DEPRESSION self-test (--test-terraincorridor). Pure math —
// no window/Vulkan/physics/jobs. Asserts the five properties the primitive has
// to have before any tunnel can be built on it:
//   C1 NO GLOBAL REGRESSION — with a corridor registered, every world point
//      outside its bounds returns a BIT-IDENTICAL height to the unmodified
//      field, and clearing the registry restores the field exactly.
//   C2 CENTRELINE DEPTH — on the spine the surface is lowered by exactly the
//      requested depth profile (at the nodes AND at the interpolated
//      mid-segment values), and (C2b) the cross-section is a flat floor out to
//      halfWidth with a monotone shoulder reaching EXACTLY zero at the reach.
//   C3 TILE-SEAM CONSISTENCY — two adjacent tiles are actually MESHED (the same
//      buildTileMeshAbs the streamer's generate() runs) with a corridor lying
//      across their shared edge; every vertex on that edge must match bit for
//      bit between the two tiles. This is the property the whole design rests
//      on: the field is a function of world (x,z), never of the tile origin.
//   C4 NO CREASE / NO SPIKE AT JOINTS — an arc sweep around an interior joint
//      stays within the analytic Lipschitz bound of the profile (a naive
//      per-node circle stamp fails this), and the joint itself is never dug
//      deeper than its own node depth (a summing implementation digs 2x there).
//   C5 DETERMINISM — repeated evaluation, reversed traversal order, and a
//      clear/re-register cycle all reproduce the field bit for bit.
// ===========================================================================
bool runTerrainCorridorSelfTest() {
    int cPass = 0, cFail = 0;
    auto checkC = [&](bool cond, const char* name) {
        if (cond) { ++cPass; x3::logInfo(std::string("[corridor-test] PASS ") + name); }
        else      { ++cFail; x3::logError(std::string("[corridor-test] FAIL ") + name); }
    };

    // The test owns the global corridor registry for its duration; nothing else
    // in a headless run registers corridors. Start and finish empty.
    clearTerrainCorridors();

    // The corridor under test: a 4-node dog-leg with two REAL joints (~21 deg at
    // N1, ~66 deg at N2 — a concave bend is exactly where a naive
    // closest-point-then-profile carve steps), and a depth profile that eases in
    // from a portal mouth, deepens under the ridge, and eases back out. Segments
    // are long relative to the 28 m influence reach so each one has a genuine
    // interior where only it contributes.
    TerrainCorridor bore{};
    bore.nodeCount = 4;
    bore.x[0] = -260.0f; bore.z[0] =  -80.0f; bore.depth[0] =  0.0f;   // mouth
    bore.x[1] = -100.0f; bore.z[1] =  -20.0f; bore.depth[1] = 10.0f;   // joint (~21 deg)
    bore.x[2] =  120.0f; bore.z[2] =  -20.0f; bore.depth[2] = 18.0f;   // joint (~66 deg)
    bore.x[3] =  200.0f; bore.z[3] =  160.0f; bore.depth[3] =  4.0f;   // mouth
    bore.halfWidth = 7.0f;    // 14 m flat floor (a 4-lane deck + shoulders)
    bore.falloff   = 21.0f;   // shoulder out to 28 m
    const float kReach = bore.halfWidth + bore.falloff;

    const TerrainConfig& wcfg = worldTerrainConfig();

    // Probe points FAR from the corridor (negatives, the canonical world's own
    // authored features, and the deep unbounded field).
    const float farPts[][2] = {
        {  600.0f,  400.0f }, { -900.0f, -500.0f }, { 3000.0f, 2000.0f },
        { -3333.0f, 1234.5f }, {  480.0f, -560.0f }, { -230.0f, -760.0f },
        {    0.0f, 5000.0f }, { 1100.0f,-1350.0f }, { -8600.0f,  0.0f },
        {  700.0f,    0.0f }, {    0.0f, -400.0f }, { -600.0f,  500.0f },
    };
    const int nFar = (int)(sizeof(farPts) / sizeof(farPts[0]));
    float baseFar[16] = {};
    for (int i = 0; i < nFar; ++i) baseFar[i] = terrainHeightAt(wcfg, farPts[i][0], farPts[i][1]);

    // A dense baseline grid over the corridor's own neighbourhood, so C1 also
    // proves that points merely NEAR the corridor but past its shoulder are
    // untouched (the interesting failure mode: an unclamped falloff).
    const int   kG = 81;
    const float gx0 = -320.0f, gz0 = -180.0f, gStep = 8.0f;   // spans the whole dog-leg
    std::vector<float> baseGrid((size_t)kG * kG);
    for (int j = 0; j < kG; ++j)
        for (int i = 0; i < kG; ++i)
            baseGrid[(size_t)j * kG + i] = terrainHeightAt(wcfg, gx0 + i * gStep, gz0 + j * gStep);

    const bool registered = registerTerrainCorridor(bore);
    checkC(registered && terrainCorridorCount() == 1u,
           "C0 registerTerrainCorridor accepts a valid corridor (fixed-capacity registry)");

    // ---- C1: no global regression (BIT-identical outside the bounds) --------
    {
        bool farExact = true;
        for (int i = 0; i < nFar; ++i) {
            const float h = terrainHeightAt(wcfg, farPts[i][0], farPts[i][1]);
            if (h != baseFar[i]) {                    // bit-exact, not epsilon
                farExact = false;
                x3::logError("[corridor-test] far point (" + std::to_string(farPts[i][0]) + "," +
                             std::to_string(farPts[i][1]) + ") moved " +
                             std::to_string(h - baseFar[i]) + " m");
            }
        }
        // Near-field grid: every sample with a zero corridor delta must be
        // bit-identical to the baseline; every sample with a non-zero delta must
        // be strictly LOWER, and must lie strictly inside the influence reach.
        bool gridExact = true, loweredOnly = true;
        int touched = 0, untouched = 0;
        for (int j = 0; j < kG; ++j) {
            for (int i = 0; i < kG; ++i) {
                const float x = gx0 + i * gStep, z = gz0 + j * gStep;
                const float h = terrainHeightAt(wcfg, x, z);
                const float d = terrainCorridorDelta(x, z);
                if (d == 0.0f) {
                    ++untouched;
                    if (h != baseGrid[(size_t)j * kG + i]) gridExact = false;
                } else {
                    ++touched;
                    if (d > 0.0f || h >= baseGrid[(size_t)j * kG + i]) loweredOnly = false;
                    if (terrainCorridorDepthAt(bore, x, z) <= 0.0f) loweredOnly = false;
                }
            }
        }
        checkC(farExact && gridExact && loweredOnly && touched > 0 && untouched > 0,
               "C1 outside the corridor bounds the height is BIT-identical; inside it only LOWERS");
    }

    // ---- C1b: clearing the registry restores the field exactly --------------
    {
        clearTerrainCorridors();
        bool restored = (terrainCorridorCount() == 0u);
        for (int j = 0; j < kG; ++j)
            for (int i = 0; i < kG; ++i)
                if (terrainHeightAt(wcfg, gx0 + i * gStep, gz0 + j * gStep) !=
                    baseGrid[(size_t)j * kG + i]) restored = false;
        registerTerrainCorridor(bore);   // put it back for the remaining cases
        checkC(restored, "C1b clearTerrainCorridors() restores the field bit-for-bit");
    }

    // ---- C2: the centreline is lowered by exactly the requested depth -------
    // Three claims, in increasing strength:
    //   (i)   ON THE SPINE the ground is lowered by AT LEAST the profile, always.
    //   (ii)  AT EVERY NODE it is lowered by EXACTLY that node's depth.
    //   (iii) In each segment's INTERIOR (t in [0.25,0.75]; segments are longer
    //         than 4x the reach here, so no neighbour contributes) it is lowered
    //         by EXACTLY the interpolated profile.
    // The union's flat end caps mean (iii) does NOT extend into the last
    // ~halfWidth before a deeper node — deliberate, and covered by (i).
    {
        bool atLeast = true, atNodes = true, interior = true;
        float worstNode = 0.0f, worstInterior = 0.0f;

        for (int s = 0; s + 1 < bore.nodeCount; ++s) {
            for (int k = 0; k <= 64; ++k) {
                const float t  = (float)k / 64.0f;
                const float px = bore.x[s] + (bore.x[s+1] - bore.x[s]) * t;
                const float pz = bore.z[s] + (bore.z[s+1] - bore.z[s]) * t;
                const float want = bore.depth[s] + (bore.depth[s+1] - bore.depth[s]) * t;
                const float got  = terrainCorridorDepthAt(bore, px, pz);
                if (got < want - 1e-3f) atLeast = false;                    // (i)
                if (t >= 0.25f && t <= 0.75f) {                             // (iii)
                    worstInterior = std::max(worstInterior, std::fabs(got - want));
                    if (std::fabs(got - want) > 1e-3f) interior = false;
                }
                // The SURFACE must actually move by that much, not just the
                // primitive: measure the real field with and without the
                // corridor registered.
                if (k % 16 == 0) {
                    clearTerrainCorridors();
                    const float hBase = terrainHeightAt(wcfg, px, pz);
                    registerTerrainCorridor(bore);
                    const float hCut = terrainHeightAt(wcfg, px, pz);
                    if (std::fabs((hBase - hCut) - got) > 1e-3f) atLeast = false;
                    if (hBase - hCut < want - 1e-3f) atLeast = false;
                }
            }
        }
        for (int n = 0; n < bore.nodeCount; ++n) {                          // (ii)
            const float got = terrainCorridorDepthAt(bore, bore.x[n], bore.z[n]);
            worstNode = std::max(worstNode, std::fabs(got - bore.depth[n]));
            if (std::fabs(got - bore.depth[n]) > 1e-3f) atNodes = false;
        }
        if (!atNodes || !interior)
            x3::logError("[corridor-test] centreline error: node=" + std::to_string(worstNode) +
                         " m interior=" + std::to_string(worstInterior) + " m");
        checkC(atLeast && atNodes && interior,
               "C2 centreline lowered by exactly the requested depth profile (nodes + segment interiors)");
    }

    // ---- C2b: cross-section — flat floor, smoothstep shoulder, exact zero ---
    {
        bool shoulderOk = true;
        // Probe perpendicular to the middle segment (runs +X along z=-20); the
        // sample x=10 is >100 m from either neighbouring segment.
        const float px = 10.0f, pz = -20.0f;
        const float mid = terrainCorridorDepthAt(bore, px, pz);
        if (std::fabs(mid - 14.0f) > 1e-3f) shoulderOk = false;   // profile midpoint
        // Flat out to halfWidth, both sides.
        for (int k = 0; k <= 7; ++k) {
            const float off = (float)k;
            if (std::fabs(terrainCorridorDepthAt(bore, px, pz + off) - mid) > 1e-3f) shoulderOk = false;
            if (std::fabs(terrainCorridorDepthAt(bore, px, pz - off) - mid) > 1e-3f) shoulderOk = false;
        }
        // Strictly between 0 and full depth in the shoulder, monotone decreasing.
        float prev = mid;
        for (int k = 1; k <= 20; ++k) {
            const float off = bore.halfWidth + (float)k * (bore.falloff / 20.0f);
            const float v = terrainCorridorDepthAt(bore, px, pz + off);
            if (v > prev + 1e-4f) shoulderOk = false;         // never increases outward
            if (k < 20 && !(v < mid + 1e-4f && v >= 0.0f)) shoulderOk = false;
            prev = v;
        }
        // EXACTLY zero at and beyond the reach — this is what makes C1's
        // bit-identity claim hold rather than being an epsilon.
        for (int k = 0; k < 8; ++k) {
            const float off = kReach + (float)k * 5.0f;
            if (terrainCorridorDepthAt(bore, px, pz + off) != 0.0f) shoulderOk = false;
            if (terrainCorridorDepthAt(bore, px, pz - off) != 0.0f) shoulderOk = false;
        }
        checkC(shoulderOk,
               "C2b cross-section: flat floor to halfWidth, monotone smoothstep shoulder, exact 0 at reach");
    }

    // ---- C3: TILE-SEAM CONSISTENCY (the critical correctness property) ------
    // Mesh two ADJACENT tiles with the very same buildTileMeshAbs the streamer's
    // generate() runs, with a corridor lying across their shared edge, and
    // compare the shared edge vertex for vertex.
    {
        TerrainConfig tcfg{};                 // legacy (worldFeatures=false) field
        tcfg.tileSize = 32.0f; tcfg.tileVerts = 33; tcfg.heightScale = 40.0f; tcfg.seed = 909u;

        clearTerrainCorridors();
        TerrainCorridor seam{};
        seam.nodeCount = 3;
        seam.x[0] = 10.0f; seam.z[0] = -18.0f; seam.depth[0] =  3.0f;
        seam.x[1] = 40.0f; seam.z[1] =  14.0f; seam.depth[1] = 11.0f;
        seam.x[2] = 26.0f; seam.z[2] =  50.0f; seam.depth[2] =  6.0f;
        seam.halfWidth = 6.0f; seam.falloff = 18.0f;
        registerTerrainCorridor(seam);

        std::vector<x3::rhi::MeshVertex> vA, vB;
        std::vector<uint32_t> iA, iB;
        const uint32_t vpe = tcfg.tileVerts;      // LOD0: 33 verts per edge
        bool seamExact = true, seamInteresting = false;
        float worstSeam = 0.0f;

        // Seam 1: tile (0,0) spans x[0,32]; tile (1,0) spans x[32,64]. Shared
        // edge x = 32, z in [0,32].
        buildTileMeshAbs(tcfg,  0.0f, 0.0f, TerrainLod::Full, vA, iA);
        buildTileMeshAbs(tcfg, 32.0f, 0.0f, TerrainLod::Full, vB, iB);
        for (uint32_t j = 0; j < vpe; ++j) {
            const x3::rhi::MeshVertex& a = vA[(size_t)j * vpe + (vpe - 1)];   // A's +X edge
            const x3::rhi::MeshVertex& b = vB[(size_t)j * vpe + 0];           // B's -X edge
            if (a.pos[0] != b.pos[0] || a.pos[2] != b.pos[2]) { seamExact = false; continue; }
            worstSeam = std::max(worstSeam, std::fabs(a.pos[1] - b.pos[1]));
            if (a.pos[1] != b.pos[1]) seamExact = false;              // BIT-exact
            // The NORMALS must match too: they are central differences of the
            // same field, so a tile-dependent corridor would shade a visible
            // crease down the seam even if the positions agreed.
            for (int k = 0; k < 3; ++k) if (a.normal[k] != b.normal[k]) seamExact = false;
            if (terrainCorridorDelta(a.pos[0], a.pos[2]) != 0.0f) seamInteresting = true;
        }
        // Seam 2: the OTHER axis — tile (0,-1) above tile (0,0), shared edge z=0.
        buildTileMeshAbs(tcfg, 0.0f, -32.0f, TerrainLod::Full, vA, iA);
        buildTileMeshAbs(tcfg, 0.0f,   0.0f, TerrainLod::Full, vB, iB);
        for (uint32_t i = 0; i < vpe; ++i) {
            const x3::rhi::MeshVertex& a = vA[(size_t)(vpe - 1) * vpe + i];   // A's +Z edge
            const x3::rhi::MeshVertex& b = vB[(size_t)0 * vpe + i];           // B's -Z edge
            if (a.pos[0] != b.pos[0] || a.pos[2] != b.pos[2]) { seamExact = false; continue; }
            worstSeam = std::max(worstSeam, std::fabs(a.pos[1] - b.pos[1]));
            if (a.pos[1] != b.pos[1]) seamExact = false;
            for (int k = 0; k < 3; ++k) if (a.normal[k] != b.normal[k]) seamExact = false;
            if (terrainCorridorDelta(a.pos[0], a.pos[2]) != 0.0f) seamInteresting = true;
        }
        // A far-away tile origin must resolve the same world point identically:
        // the field may not depend on which tile's local indexing reached it.
        bool farTileExact = true;
        for (int k = 0; k <= 32; ++k) {
            const float wz = 16.0f;
            const float fromA = terrainHeightAt(tcfg,     0.0f + (float)(k + 32), wz);
            const float fromB = terrainHeightAt(tcfg,    32.0f + (float)k,        wz);
            const float fromC = terrainHeightAt(tcfg, -8192.0f + (float)(k + 8224), wz);
            if (fromA != fromB || fromA != fromC) farTileExact = false;
        }
        if (!seamExact)
            x3::logError("[corridor-test] seam mismatch, worst dY=" + std::to_string(worstSeam) + " m");
        checkC(seamExact && seamInteresting && farTileExact,
               "C3 tile seam: a corridor crossing a seam meshes bit-identically from either tile");

        clearTerrainCorridors();
        registerTerrainCorridor(bore);
    }

    // ---- C4: no crease and no spike at a polyline joint ---------------------
    {
        // Analytic Lipschitz bound of the depression field (see the .cpp):
        //   |grad(depth * w)| <= max|d(depth)/ds| + maxDepth * 1.5/falloff
        // smoothstep' peaks at 1.5, the distance field is 1-Lipschitz, and the
        // max of L-Lipschitz functions is L-Lipschitz.
        float maxDepth = 0.0f, maxSlope = 0.0f;
        for (int i = 0; i < bore.nodeCount; ++i) maxDepth = std::max(maxDepth, bore.depth[i]);
        for (int i = 0; i + 1 < bore.nodeCount; ++i) {
            const float dx = bore.x[i+1] - bore.x[i], dz = bore.z[i+1] - bore.z[i];
            const float len = std::sqrt(dx * dx + dz * dz);
            if (len > 1e-3f) maxSlope = std::max(maxSlope,
                                std::fabs(bore.depth[i+1] - bore.depth[i]) / len);
        }
        const float lip = maxSlope + maxDepth * 1.5f / bore.falloff;

        bool smooth = true, noSpike = true;
        float worstRatio = 0.0f;
        const int kArc = 2048;
        // Full-circle sweeps around BOTH interior joints, at radii inside the
        // flat floor, through the shoulder, and just past the reach.
        const float radii[] = { 2.0f, 7.0f, 12.0f, 20.0f, 27.0f, 30.0f };
        for (int jn = 1; jn <= 2; ++jn) {
            const float cx = bore.x[jn], cz = bore.z[jn];
            for (float r : radii) {
                const float arcStep = 6.28318531f * r / (float)kArc;
                const float allow = lip * arcStep * 1.25f + 1e-4f;   // 25% slack
                float prev = 0.0f;
                for (int k = 0; k <= kArc; ++k) {
                    const float a = 6.28318531f * (float)k / (float)kArc;
                    const float d = terrainCorridorDepthAt(bore, cx + std::cos(a) * r,
                                                                 cz + std::sin(a) * r);
                    if (k > 0) {
                        const float jump = std::fabs(d - prev);
                        worstRatio = std::max(worstRatio, jump / allow);
                        if (jump > allow) smooth = false;
                    }
                    prev = d;
                }
            }
            // NO SPIKE: the joint itself is dug to exactly its OWN node depth. A
            // per-segment SUM (or BL-style overlapping circle stamps) digs ~2x
            // here, because both adjacent segments claim the joint.
            const float atJoint = terrainCorridorDepthAt(bore, cx, cz);
            if (std::fabs(atJoint - bore.depth[jn]) > 1e-3f) noSpike = false;
        }
        // The same must hold ACROSS corridors: two co-located registered
        // corridors must combine deepest-wins, not additively.
        {
            clearTerrainCorridors();
            TerrainCorridor a{};
            a.nodeCount = 2;
            a.x[0] = -50.0f; a.z[0] = 0.0f; a.depth[0] = 6.0f;
            a.x[1] =  50.0f; a.z[1] = 0.0f; a.depth[1] = 6.0f;
            a.halfWidth = 5.0f; a.falloff = 15.0f;
            TerrainCorridor b = a;                     // exactly co-located
            registerTerrainCorridor(a);
            registerTerrainCorridor(b);
            if (std::fabs(terrainCorridorDelta(0.0f, 0.0f) + 6.0f) > 1e-4f) noSpike = false;
            // A deeper overlapping corridor wins outright.
            clearTerrainCorridors();
            b.depth[0] = b.depth[1] = 9.0f;
            registerTerrainCorridor(a);
            registerTerrainCorridor(b);
            if (std::fabs(terrainCorridorDelta(0.0f, 0.0f) + 9.0f) > 1e-4f) noSpike = false;
            clearTerrainCorridors();
            registerTerrainCorridor(bore);
        }
        if (!smooth)
            x3::logError("[corridor-test] joint arc jump exceeded the Lipschitz bound by " +
                         std::to_string(worstRatio) + "x");
        checkC(smooth && noSpike,
               "C4 joints are crease-free (arc sweep within the Lipschitz bound) and never double-dug");
    }

    // ---- C5: determinism (repeat / reverse order / re-register cycle) -------
    {
        const int kN = 4096;
        std::vector<float> pass1((size_t)kN);
        auto sampleAt = [](int k, float& x, float& z) {
            // A deterministic scatter across the corridor and its surroundings.
            x = -340.0f + (float)((k * 37) % 900) * 0.75f;
            z = -200.0f + (float)((k * 61) % 520) * 0.75f;
        };
        for (int k = 0; k < kN; ++k) {
            float x, z; sampleAt(k, x, z);
            pass1[(size_t)k] = terrainHeightAt(wcfg, x, z);
        }
        bool repeatSame = true, reverseSame = true;
        for (int k = 0; k < kN; ++k) {
            float x, z; sampleAt(k, x, z);
            if (terrainHeightAt(wcfg, x, z) != pass1[(size_t)k]) repeatSame = false;
        }
        for (int k = kN - 1; k >= 0; --k) {
            float x, z; sampleAt(k, x, z);
            if (terrainHeightAt(wcfg, x, z) != pass1[(size_t)k]) reverseSame = false;
        }
        // Clear + re-register must reproduce the identical field.
        clearTerrainCorridors();
        registerTerrainCorridor(bore);
        bool cycleSame = true;
        for (int k = 0; k < kN; ++k) {
            float x, z; sampleAt(k, x, z);
            if (terrainHeightAt(wcfg, x, z) != pass1[(size_t)k]) cycleSame = false;
        }
        checkC(repeatSame && reverseSame && cycleSame,
               "C5 deterministic: repeat / reverse order / clear+re-register are bit-identical");
    }

    // ---- C6: registry hygiene (capacity + degenerate rejection) -------------
    {
        clearTerrainCorridors();
        uint32_t accepted = 0;
        for (uint32_t i = 0; i < kMaxTerrainCorridors + 3u; ++i)
            if (registerTerrainCorridor(bore)) ++accepted;
        const bool capped = (accepted == kMaxTerrainCorridors) &&
                            (terrainCorridorCount() == kMaxTerrainCorridors);
        clearTerrainCorridors();

        TerrainCorridor bad{};
        bad.nodeCount = 1; bad.halfWidth = 5.0f; bad.falloff = 5.0f;
        const bool rejectShort = !registerTerrainCorridor(bad);
        bad.nodeCount = TerrainCorridor::kMaxNodes + 1;
        const bool rejectLong = !registerTerrainCorridor(bad);
        bad.nodeCount = 2; bad.halfWidth = 0.0f; bad.falloff = 0.0f;
        const bool rejectZero = !registerTerrainCorridor(bad);
        bad.halfWidth = 5.0f; bad.falloff = 5.0f;
        bad.x[1] = std::numeric_limits<float>::quiet_NaN();
        const bool rejectNan = !registerTerrainCorridor(bad);
        const bool empty = (terrainCorridorCount() == 0u);

        // A degenerate (duplicated) control point must not spike, divide by zero
        // or produce NaN — authored polylines pick up duplicates.
        TerrainCorridor dup{};
        dup.nodeCount = 3;
        dup.x[0] =  0.0f; dup.z[0] = 0.0f; dup.depth[0] = 5.0f;
        dup.x[1] =  0.0f; dup.z[1] = 0.0f; dup.depth[1] = 5.0f;   // duplicate node
        dup.x[2] = 30.0f; dup.z[2] = 0.0f; dup.depth[2] = 5.0f;
        dup.halfWidth = 4.0f; dup.falloff = 10.0f;
        const float dd = terrainCorridorDepthAt(dup, 0.0f, 0.0f);
        const bool dupOk = std::isfinite(dd) && std::fabs(dd - 5.0f) < 1e-3f;

        // A single-node / empty corridor is inert, never a NaN.
        TerrainCorridor none{};
        const bool inert = (terrainCorridorDepthAt(none, 0.0f, 0.0f) == 0.0f);

        checkC(capped && rejectShort && rejectLong && rejectZero && rejectNan &&
               empty && dupOk && inert,
               "C6 registry caps at kMaxTerrainCorridors + rejects degenerate corridors");
    }

    // ---- C7: CORRIDOR x TILE-LOD WEDGE (the spawn-road green-strip class) ---
    // A field query cannot see this defect: the FIELD is correct at every
    // point; the coarse MESH interpolates 2/4 m chords across the carve's
    // smoothstep shoulder and reconstructs a wedge ABOVE the carved datum —
    // the strip of grass knifing through the spawn-road pavement, which
    // survived lifting the slab 0.07 m proud because it is a mesh artifact.
    // Register a demo-route-class corridor (10.1/14 profile, 8 m deep,
    // diagonal at the spawn heading so it crosses cells and tile borders at
    // a grazing angle) and survey every tile it touches through the REAL
    // mesher at all three LODs. Gate: the mesh never stands more than 2 cm
    // above the true field anywhere on the corridor floor.
    // (X3_NO_CORRIDOR_LOD_REFINE=1 reproduces the pre-fix mesh; this gate
    // then fails with the measured wedge — the A/B instrument.)
    {
        clearTerrainCorridors();
        TerrainCorridor road{};
        road.nodeCount = 3;
        // heading 157.5 deg (the demo spawn route's), through a tile corner
        const float dirX = -0.9239f, dirZ = 0.3827f;
        for (int i = 0; i < 3; ++i) {
            const float s = -90.0f + 90.0f * (float)i;
            road.x[i] = -600.0f + dirX * s;
            road.z[i] = -350.0f + dirZ * s;
            road.depth[i] = 8.0f;
        }
        road.halfWidth = 10.1f;   // the tunnel corridor's own cross-section
        road.falloff   = 14.0f;
        checkC(registerTerrainCorridor(road), "C7a the demo-class corridor registers");

        // The metric is LOD PARITY, not an absolute: at Full LOD a 1 m chord
        // over the rocky natural relief already stands ~0.3 m above the field's
        // interior dips (honest mesh interpolation — invisible, because the
        // carve derivation cuts against the field's MAX, not its dips). The
        // wedge class is the EXCESS a coarser LOD adds on top of Full; with
        // the refinement in, Half/Quarter emit the IDENTICAL surface inside
        // the corridor, so the excess must be ~0. Measured pre-fix (the A/B
        // env): Full 0.36 m, Half 1.24 m, Quarter 3.34 m — the strip.
        float worstPerLod[3] = { 0.0f, 0.0f, 0.0f };
        float worstX = 0.0f, worstZ = 0.0f;
        const float pad  = road.halfWidth + road.falloff;
        const float bx0  = std::min(road.x[0], road.x[2]) - pad;
        const float bx1  = std::max(road.x[0], road.x[2]) + pad;
        const float bz0  = std::min(road.z[0], road.z[2]) - pad;
        const float bz1  = std::max(road.z[0], road.z[2]) + pad;
        const float ts   = wcfg.tileSize;
        for (int lod = 0; lod < (int)TerrainLod::Count; ++lod) {
            for (float tz = std::floor(bz0 / ts) * ts; tz < bz1; tz += ts) {
                for (float tx = std::floor(bx0 / ts) * ts; tx < bx1; tx += ts) {
                    float wx = 0.0f, wz = 0.0f;
                    const float w = terrainTileCorridorWedge(wcfg, tx, tz,
                                                             (TerrainLod)lod, &wx, &wz);
                    if (w > worstPerLod[lod]) {
                        worstPerLod[lod] = w;
                        if (lod > 0) { worstX = wx; worstZ = wz; }
                    }
                }
            }
            x3::logInfo("[corridor-test] C7 LOD" + std::to_string(lod) +
                        " worst mesh-above-field on the corridor floor: " +
                        std::to_string(worstPerLod[lod]) + " m");
        }
        const float excess = std::max(worstPerLod[1], worstPerLod[2]) - worstPerLod[0];
        if (excess > 0.02f)
            x3::logError("[corridor-test] C7 LOD excess over Full: " + std::to_string(excess) +
                         " m, worst at (" + std::to_string(worstX) + ", " +
                         std::to_string(worstZ) + ")");
        checkC(excess <= 0.02f,
               "C7 Half/Quarter mesh the corridor floor IDENTICALLY to Full (LOD excess <= 2 cm)");
        clearTerrainCorridors();
    }

    // ---- C8: the refine's DISTANCE SCOPE (W-PERF task #33) -----------------
    // Beyond kCorridorRefineNearM of the tile-request focus the mesher swaps
    // the exact LOD0-lattice refine for the far-field CLAMP (2 coarse tris per
    // hot cell, corner verts pulled under the carved field). Gates, over the
    // same demo-class corridor as C7:
    //   C8a  a NEAR focus is bit-identical to the no-focus build (plumbing a
    //        focus through the streamer changed nothing near-field), and Full
    //        LOD (collision source) is bit-identical even FAR.
    //   C8b  the clamp holds at range: far-built Half/Quarter meshes never
    //        stand > 2 cm above the carved field on the corridor floor.
    //   C8c  the triangles actually died: far coarse meshes carry strictly
    //        fewer indices than the exact refine's.
    //   C8d  seam parity at range: two adjacent far tiles agree bit-for-bit
    //        on their shared border (the clamp scans incident cells in world
    //        space, so a border vertex resolves identically from both sides).
    {
        clearTerrainCorridors();
        TerrainCorridor road{};
        road.nodeCount = 3;
        const float dirX = -0.9239f, dirZ = 0.3827f;   // C7's grazing heading
        for (int i = 0; i < 3; ++i) {
            const float s = -90.0f + 90.0f * (float)i;
            road.x[i] = -600.0f + dirX * s;
            road.z[i] = -350.0f + dirZ * s;
            road.depth[i] = 8.0f;
        }
        road.halfWidth = 10.1f;
        road.falloff   = 14.0f;
        checkC(registerTerrainCorridor(road), "C8 the corridor registers");

        const float focusNear[2] = { -600.0f, -350.0f };            // on the road
        const float focusFar[2]  = { -600.0f + kCorridorRefineNearM + 400.0f,
                                     -350.0f };                     // ~730 m out
        const float pad = road.halfWidth + road.falloff;
        const float bx0 = std::min(road.x[0], road.x[2]) - pad;
        const float bx1 = std::max(road.x[0], road.x[2]) + pad;
        const float bz0 = std::min(road.z[0], road.z[2]) - pad;
        const float bz1 = std::max(road.z[0], road.z[2]) + pad;
        const float ts  = wcfg.tileSize;

        bool nearIdentical = true, fullIdentical = true;
        uint64_t exactIdx = 0, farIdx = 0;
        std::vector<x3::rhi::MeshVertex> vA, vB;
        std::vector<uint32_t> iA, iB;
        for (int lod = 0; lod < (int)TerrainLod::Count; ++lod) {
            for (float tz = std::floor(bz0 / ts) * ts; tz < bz1; tz += ts) {
                for (float tx = std::floor(bx0 / ts) * ts; tx < bx1; tx += ts) {
                    // exact (no focus) vs near-focus vs far-focus builds
                    buildTileMeshAbs(wcfg, tx, tz, (TerrainLod)lod, vA, iA);
                    buildTileMeshAbs(wcfg, tx, tz, (TerrainLod)lod, vB, iB,
                                     nullptr, focusNear);
                    if (vA.size() != vB.size() || iA != iB ||
                        std::memcmp(vA.data(), vB.data(),
                                    vA.size() * sizeof(vA[0])) != 0)
                        nearIdentical = false;
                    buildTileMeshAbs(wcfg, tx, tz, (TerrainLod)lod, vB, iB,
                                     nullptr, focusFar);
                    if (lod == 0) {
                        if (vA.size() != vB.size() || iA != iB ||
                            std::memcmp(vA.data(), vB.data(),
                                        vA.size() * sizeof(vA[0])) != 0)
                            fullIdentical = false;
                    } else {
                        exactIdx += iA.size();
                        farIdx   += iB.size();
                    }
                }
            }
        }
        checkC(nearIdentical && fullIdentical,
               "C8a near-focus build bit-identical to no-focus; Full LOD unchanged even far");

        // C8b — the clamp holds at range, gated the way C7 gates: RELATIVE to
        // the Full-LOD mesh, never against absolute zero.
        // WHY RELATIVE (the trap that failed this gate on its first run, at an
        // absolute 2 cm): terrainTileCorridorWedge probes triangles on a 0.5 m
        // grid, but the finest mesh lattice is 1 m, so between two lattice
        // samples ANY mesh — Full LOD included, the very surface collision uses
        // — stands slightly above the field. C7 measures that floor at 0.625 m
        // on its corridor and so compares coarse-to-Full; C8b must too, or it
        // is gating the probe's resolution rather than the clamp.
        // The clamp's guarantee is pointwise and STRONGER than "no worse than
        // Full": a far cell's corners are all pulled to
        //   m = min(field over the cell's LOD0 lattice) <= min over the 1 m
        // cell containing P <= fullMesh(P), and the coarse chord is a convex
        // combination of those corners — so farMesh(P) <= fullMesh(P) for every
        // P. The gate therefore demands NO EXCESS OVER FULL (2 cm slack only to
        // keep it a float comparison), and the measured excess is negative.
        {
            bool clampHolds = true;
            float worstFar = 0.0f, worstFull = 0.0f, worstExcess = -1e9f;
            float wx = 0.0f, wz = 0.0f;
            for (float tz = std::floor(bz0 / ts) * ts; tz < bz1; tz += ts) {
                for (float tx = std::floor(bx0 / ts) * ts; tx < bx1; tx += ts) {
                    const float wFull = terrainTileCorridorWedge(
                        wcfg, tx, tz, TerrainLod::Full);
                    worstFull = std::max(worstFull, wFull);
                    for (int lod = 1; lod < (int)TerrainLod::Count; ++lod) {
                        float px = 0.0f, pz = 0.0f;
                        const float wFar = terrainTileCorridorWedge(
                            wcfg, tx, tz, (TerrainLod)lod, &px, &pz, focusFar);
                        worstFar = std::max(worstFar, wFar);
                        if (wFar - wFull > worstExcess) {
                            worstExcess = wFar - wFull; wx = px; wz = pz;
                        }
                        if (wFar > wFull + 0.02f) clampHolds = false;
                    }
                }
            }
            x3::logInfo("[corridor-test] C8b far-field worst mesh-above-field on "
                        "the corridor floor: far=" + std::to_string(worstFar) +
                        " m vs Full(collision)=" + std::to_string(worstFull) +
                        " m; worst per-tile excess over Full=" +
                        std::to_string(worstExcess) + " m at (" +
                        std::to_string(wx) + ", " + std::to_string(wz) + ")");
            checkC(clampHolds,
                   "C8b the far-field clamp holds at range (never above the Full-LOD mesh)");
        }
        x3::logInfo("[corridor-test] C8c coarse-LOD indices exact=" +
                    std::to_string(exactIdx) + " far=" + std::to_string(farIdx) +
                    " (saved " + std::to_string(exactIdx > 0 ?
                        (double)(exactIdx - farIdx) * 100.0 / (double)exactIdx : 0.0) +
                    "%)");
        checkC(farIdx < exactIdx,
               "C8c the far variant emits strictly fewer triangles than the exact refine");

        // C8d — seam parity: walk tile pairs sharing a vertical border where
        // both sides feel the corridor, and demand bit-equal border verts.
        {
            bool seamOk = true; int pairs = 0;
            for (int lod = 1; lod < (int)TerrainLod::Count; ++lod) {
                for (float tz = std::floor(bz0 / ts) * ts; tz < bz1; tz += ts) {
                    for (float tx = std::floor(bx0 / ts) * ts; tx + ts < bx1; tx += ts) {
                        buildTileMeshAbs(wcfg, tx, tz, (TerrainLod)lod, vA, iA,
                                         nullptr, focusFar);
                        buildTileMeshAbs(wcfg, tx + ts, tz, (TerrainLod)lod, vB, iB,
                                         nullptr, focusFar);
                        const float bx = tx + ts;
                        ++pairs;
                        // surface verts only (skirts duplicate positions)
                        const uint32_t stepL = 1u << lod;
                        const uint32_t vpeL  = (wcfg.tileVerts - 1) / stepL + 1;
                        for (uint32_t j = 0; j < vpeL; ++j) {
                            const x3::rhi::MeshVertex& a =
                                vA[(size_t)j * vpeL + (vpeL - 1)];   // A's east edge
                            const x3::rhi::MeshVertex& b =
                                vB[(size_t)j * vpeL + 0];            // B's west edge
                            if (a.pos[0] != bx || b.pos[0] != bx ||
                                a.pos[1] != b.pos[1] || a.pos[2] != b.pos[2])
                                seamOk = false;
                        }
                    }
                }
            }
            x3::logInfo("[corridor-test] C8d checked " + std::to_string(pairs) +
                        " adjacent far-tile pairs");
            checkC(seamOk && pairs > 0,
                   "C8d far tiles agree bit-for-bit across shared borders");
        }
        clearTerrainCorridors();
    }

    clearTerrainCorridors();   // leave the global registry exactly as we found it

    x3::logInfo(std::string("[corridor-test] ") + std::to_string(cPass) + " passed, " +
                std::to_string(cFail) + " failed");
    return cFail == 0;
}

} // namespace x3::game
