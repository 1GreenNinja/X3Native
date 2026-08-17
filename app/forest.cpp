// WORLD FORESTS — implementation. See forest.h for the contract; this file is
// the region table + the arithmetic.

#include "forest.h"

#include "asset_root.h"
#include "terrain.h"

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace x3::game {

namespace {

// The two published broadleaf species. PAIRED with road_trees.cpp — same GLBs,
// same 70-100 ft scale rolls (NO_SLOP rule 8: the owner's number is spec).
constexpr const char* kOakGlb    = "nature/OakBigTree01.glb";
constexpr const char* kPoplarGlb = "nature/PoplarTree001.glb";

// --- Placement constants (metres) ------------------------------------------
constexpr float kSink         = 0.15f;  // trunk sunk so no root plate floats (rule 4)
constexpr float kMaxLocalDrop = 2.2f;   // reject slopes steeper than this per 2 m
                                        // (PAIRED with road_trees.cpp kMaxLocalDrop)
constexpr float kMinSpace     = 4.2f;   // global cross-region trunk spacing floor
constexpr float kJunctionKeep = 60.0f;  // junction sightlines stay open
constexpr float kDemoBandKeep = 26.0f;  // road_trees owns |lat| 14-24 on the demo
                                        // route; we start past its outer edge
// --- Draw tiers -------------------------------------------------------------
constexpr float kNearIn   = 85.0f;   // enter LOD0 inside this
constexpr float kNearOut  = 100.0f;  // leave LOD0 outside this (hysteresis band)
constexpr float kFarMax   = 5200.0f; // beyond this a tree is not drawn at all
                                     // (3600 left the north belt visibly
                                     // truncated in wide shots; cards are 4
                                     // tris — distance is the cheap dial)
constexpr float kChunkM   = 160.0f;  // chunk edge (pruning granularity)

// --- Deterministic order-independent hashing --------------------------------
inline uint32_t hashCell(int32_t ix, int32_t iz, uint32_t seed) {
    uint32_t h = (uint32_t)ix * 374761393u ^ (uint32_t)iz * 668265263u ^ seed * 2246822519u;
    h ^= h >> 13; h *= 1274126177u; h ^= h >> 16;
    return h;
}
inline float h01(uint32_t h, uint32_t lane) {           // [0,1), decorrelated lanes
    h ^= lane * 2654435761u; h *= 2246822519u; h ^= h >> 15;
    return (float)((h >> 8) & 0xFFFFFFu) / 16777216.0f;
}
// 80 m stand noise: one-species patches are how real woods read (same intent
// as road_trees' per-grove `kind` roll, expressed spatially for area planting).
inline bool standIsOak(float x, float z, float oakBias) {
    const int32_t sx = (int32_t)std::floor(x / 80.0f);
    const int32_t sz = (int32_t)std::floor(z / 80.0f);
    return h01(hashCell(sx, sz, 0xA110A5EDu), 7u) < oakBias;
}

inline float segDist2(float px, float pz, float ax, float az, float bx, float bz) {
    const float abx = bx - ax, abz = bz - az;
    const float apx = px - ax, apz = pz - az;
    const float ab2 = abx * abx + abz * abz;
    float t = ab2 > 1e-6f ? (apx * abx + apz * abz) / ab2 : 0.0f;
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    const float dx = apx - abx * t, dz = apz - abz * t;
    return dx * dx + dz * dz;
}

// ---- Keep-out geometry mirrored from terrain.cpp's authored map -----------
// PAIRED with terrain.cpp: kPads[] / facilityGuard() / contentGuard(). Those
// are file-local there by design; a change to either site is a change to both
// (NO_SLOP rule 4).
struct Disc { float x, z, r; };
constexpr Disc kPadKeep[] = {
    {    0.0f,   0.0f, 260.0f * 1.7f + 20.0f },  // facility/crash pad
    { -600.0f, 500.0f, 250.0f * 1.7f + 20.0f },  // Scrapyard City
    {  200.0f, 500.0f, 190.0f * 1.7f + 20.0f },  // New District
    { -200.0f, 350.0f, 150.0f * 1.7f + 20.0f },  // Industrial Zone
    {  800.0f, 400.0f, 190.0f },                 // East Outpost
    { -880.0f, -320.0f, 190.0f },                // West Outpost
    {  393.0f, 6752.0f, 150.0f },                // summit lot pad (spur peak)
};
inline bool inPadKeep(float x, float z) {
    for (const Disc& d : kPadKeep) {
        const float dx = x - d.x, dz = z - d.z;
        if (dx * dx + dz * dz < d.r * d.r) return true;
    }
    // facility keep-out rect (+40 m margin), terrain.cpp facilityGuard():
    if (x > -196.0f && x < 240.0f && z > -227.5f && z < 248.5f) return true;
    return false;
}

} // anonymous namespace

// ===========================================================================
// BUILD
// ===========================================================================
bool WorldForests::build(x3::rhi::IRenderDevice& device, const Inputs& in) {
    if (m_built) return !m_trees.empty();
    m_built = true;

    // ---- load the two species (model cache shared with road_trees) --------
    m_assets.reset(x3::asset::createAssetSource());
    if (!m_assets || !m_assets->mountDir(convertedGlbRoot(), 0)) {
        x3::logWarn("forest: converted_glb mount failed — no forests");
        return false;
    }
    m_loader.reset(x3::asset::createModelLoader(&device, m_assets.get()));
    const char* paths[2] = { kOakGlb, kPoplarGlb };
    for (int s = 0; s < 2; ++s) {
        Species& sp = m_species[s];
        sp.model = m_loader->load(paths[s]);
        if (!sp.model.ok) {
            x3::logWarn(std::string("forest: GLB load failed: ") + paths[s] +
                        " — run `python tools/asset_store.py fetch --all`");
            continue;
        }
        std::vector<std::string> names;
        std::vector<x3::asset::ModelDrawable> ds = x3::asset::makeDrawablesNamed(sp.model, names);
        for (size_t i = 0; i < ds.size(); ++i) {
            std::string ln = names[i];
            for (char& c : ln) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
            if (ln.find("billboard") != std::string::npos) {
                // The far-LOD cross-card. Textured 2026-08-16 (see forest.h /
                // tools/inject_billboard_tex.py); refuse to use it untextured —
                // NO_SLOP rule 3 — a grey card forest is worse than a short
                // draw distance.
                if (ds[i].baseColorTexId != 0) sp.card.push_back({ ds[i] });
                else x3::logWarn(std::string("forest: ") + paths[s] +
                                 " billboard has NO texture — far tier disabled "
                                 "for this species (stale store copy?)");
            } else {
                sp.lod0.push_back({ ds[i] });
            }
        }
        sp.ok = !sp.lod0.empty();
    }
    if (!m_species[0].ok && !m_species[1].ok) {
        x3::logWarn("forest: no species loaded — no forests");
        return false;
    }

    // ---- demo-route spine polyline (road_trees' lane keep-out) ------------
    std::vector<float> spine;             // x,z pairs every ~20 m
    float spineMinX = 1e9f, spineMaxX = -1e9f, spineMinZ = 1e9f, spineMaxZ = -1e9f;
    if (in.demoRoute) {
        for (float s = 0.0f; s <= in.demoRoute->totalLen; s += 20.0f) {
            float p[3]; in.demoRoute->posAt(s, p);
            spine.push_back(p[0]); spine.push_back(p[2]);
            spineMinX = std::min(spineMinX, p[0]); spineMaxX = std::max(spineMaxX, p[0]);
            spineMinZ = std::min(spineMinZ, p[2]); spineMaxZ = std::max(spineMaxZ, p[2]);
        }
        spineMinX -= kDemoBandKeep; spineMaxX += kDemoBandKeep;
        spineMinZ -= kDemoBandKeep; spineMaxZ += kDemoBandKeep;
    }
    auto nearDemoSpine = [&](float x, float z) -> bool {
        if (spine.empty()) return false;
        if (x < spineMinX || x > spineMaxX || z < spineMinZ || z > spineMaxZ) return false;
        const float r2 = kDemoBandKeep * kDemoBandKeep;
        for (size_t i = 2; i < spine.size(); i += 2)
            if (segDist2(x, z, spine[i - 2], spine[i - 1], spine[i], spine[i + 1]) < r2)
                return true;
        return false;
    };

    // ---- global occupancy hash (cross-region spacing floor) ---------------
    std::unordered_map<uint64_t, std::vector<uint32_t>> occ;   // 8 m cells
    auto occKey = [](int32_t ix, int32_t iz) {
        return ((uint64_t)(uint32_t)ix << 32) | (uint32_t)iz;
    };
    auto tooClose = [&](float x, float z) -> bool {
        const int32_t cx = (int32_t)std::floor(x / 8.0f);
        const int32_t cz = (int32_t)std::floor(z / 8.0f);
        for (int32_t dx = -1; dx <= 1; ++dx)
            for (int32_t dz = -1; dz <= 1; ++dz) {
                auto it = occ.find(occKey(cx + dx, cz + dz));
                if (it == occ.end()) continue;
                for (uint32_t ti : it->second) {
                    const float ddx = m_trees[ti].t[12] - x, ddz = m_trees[ti].t[14] - z;
                    if (ddx * ddx + ddz * ddz < kMinSpace * kMinSpace) return true;
                }
            }
        return false;
    };

    // ---- one candidate through the full accept pipeline -------------------
    // Reject counters PER REASON (NO_SLOP rule 9: diagnose with measurements —
    // the southern belt starved twice and the reason was a number both times).
    uint32_t rejected = 0;
    uint32_t rjPad = 0, rjWater = 0, rjCorr = 0, rjJct = 0, rjDemo = 0,
             rjClose = 0, rjSlope = 0, rjSea = 0;
    auto tryPlant = [&](float x, float z, uint32_t h, float oakBias) {
        // cheap authored keep-outs first
        if (inPadKeep(x, z)) { ++rejected; ++rjPad; return; }
        if (worldWaterLevelAt(x, z) != kWorldWaterDry) { ++rejected; ++rjWater; return; }
        // every registered corridor footprint: pavement + aprons + falloff of
        // every road, and every bore's tube footprint (junction throats too —
        // they register 2-node corridors).
        if (terrainCorridorContains(x, z)) { ++rejected; ++rjCorr; return; }
        if (distToNearestRoadJunction(x, z) < kJunctionKeep) { ++rejected; ++rjJct; return; }
        if (nearDemoSpine(x, z)) { ++rejected; ++rjDemo; return; }
        if (tooClose(x, z)) { ++rejected; ++rjClose; return; }

        // contact law: base on the FINAL carved field, believable ground only
        const float y  = terrainHeightAtWorld(x, z);
        const float hX = terrainHeightAtWorld(x + 2.0f, z);
        const float hZ = terrainHeightAtWorld(x, z + 2.0f);
        if (std::fabs(hX - y) > kMaxLocalDrop || std::fabs(hZ - y) > kMaxLocalDrop) { ++rejected; ++rjSlope; return; }
        if (y < kWorldSeaLevel + 0.5f) { ++rejected; ++rjSea; return; }

        const bool oak = standIsOak(x, z, oakBias);
        const Species& sp = m_species[oak ? 0 : 1];
        if (!sp.ok) { ++rejected; return; }

        // 70-100 ft (owner spec) — PAIRED with road_trees.cpp's sc rolls.
        const float sclRoll = h01(h, 1u);
        const float sc  = oak ? (0.580f + sclRoll * 0.249f)
                              : (0.945f + sclRoll * 0.405f);
        const float yaw = h01(h, 2u) * 6.2831853f;
        const float c = std::cos(yaw), sn = std::sin(yaw);
        Tree tr{};
        const float T[16] = { c * sc, 0, -sn * sc, 0,
                              0,      sc, 0,       0,
                              sn * sc, 0, c * sc,  0,
                              x, y - kSink, z, 1 };
        std::memcpy(tr.t, T, sizeof(T));
        tr.species  = oak ? 0 : 1;
        tr.nearTier = 0;
        const uint32_t ti = (uint32_t)m_trees.size();
        m_trees.push_back(tr);
        occ[occKey((int32_t)std::floor(x / 8.0f), (int32_t)std::floor(z / 8.0f))].push_back(ti);
        if (oak) ++m_oaks; else ++m_poplars;
    };

    // POLYLINE BAND sampler — one grid sweep over the union bbox, each cell
    // tested against the WHOLE polyline (min distance over segments, bbox
    // early-out per segment). The first version swept per-segment bboxes with
    // a consumed-cell dedupe set; measured on the southern belt that LOST
    // ~2/3 of the band: after curve subdivision segments are ~15-60 m long
    // while the bbox pad is dMax (165 m), so a cell is claimed by a segment
    // far BEHIND it, fails that segment's along-axis distance, and is dead
    // before the segment it actually flanks ever sweeps it (belt=558, then
    // 1261, of an expected ~4-5k — the reject counters told the story).
    // `side`: 0 = both sides, -1 / +1 = only that sign of
    // cross(seg dir, p - a) against the NEAREST segment.
    struct Seg { float ax, az, bx, bz, minX, minZ, maxX, maxZ; };
    auto plantPolylineBand = [&](const std::vector<Seg>& segs, float dMin, float dMax,
                                 float sp, uint32_t seed, float oakBias, int side) {
        if (segs.empty()) return;
        float x0 = 1e9f, z0 = 1e9f, x1 = -1e9f, z1 = -1e9f;
        for (const Seg& s : segs) {
            x0 = std::min(x0, s.minX); z0 = std::min(z0, s.minZ);
            x1 = std::max(x1, s.maxX); z1 = std::max(z1, s.maxZ);
        }
        x0 -= dMax; z0 -= dMax; x1 += dMax; z1 += dMax;
        const float dMin2 = dMin * dMin, dMax2 = dMax * dMax;
        const int32_t ix0 = (int32_t)std::floor(x0 / sp), ix1 = (int32_t)std::ceil(x1 / sp);
        const int32_t iz0 = (int32_t)std::floor(z0 / sp), iz1 = (int32_t)std::ceil(z1 / sp);
        for (int32_t iz = iz0; iz <= iz1; ++iz)
            for (int32_t ix = ix0; ix <= ix1; ++ix) {
                const uint32_t h = hashCell(ix, iz, seed);
                const float x = ((float)ix + 0.07f + h01(h, 3u) * 0.86f) * sp;
                const float z = ((float)iz + 0.07f + h01(h, 4u) * 0.86f) * sp;
                float best2 = 1e18f;
                const Seg* bestSeg = nullptr;
                for (const Seg& s : segs) {
                    if (x < s.minX - dMax || x > s.maxX + dMax ||
                        z < s.minZ - dMax || z > s.maxZ + dMax) continue;
                    const float d2 = segDist2(x, z, s.ax, s.az, s.bx, s.bz);
                    if (d2 < best2) { best2 = d2; bestSeg = &s; }
                }
                if (!bestSeg || best2 < dMin2 || best2 > dMax2) continue;
                if (side != 0) {
                    const float cr = (bestSeg->bx - bestSeg->ax) * (z - bestSeg->az) -
                                     (bestSeg->bz - bestSeg->az) * (x - bestSeg->ax);
                    if (side < 0 ? (cr > 0.0f) : (cr < 0.0f)) continue;
                }
                tryPlant(x, z, h, oakBias);
            }
    };
    auto makeSeg = [](float ax, float az, float bx, float bz) {
        Seg s{ ax, az, bx, bz,
               std::min(ax, bx), std::min(az, bz),
               std::max(ax, bx), std::max(az, bz) };
        return s;
    };

    // jittered-grid sampler over a bbox with an arbitrary shape test
    auto plantGrid = [&](float x0, float z0, float x1, float z1, float sp,
                         uint32_t seed, float oakBias, auto&& inShape) {
        const int32_t ix0 = (int32_t)std::floor(x0 / sp), ix1 = (int32_t)std::ceil(x1 / sp);
        const int32_t iz0 = (int32_t)std::floor(z0 / sp), iz1 = (int32_t)std::ceil(z1 / sp);
        for (int32_t iz = iz0; iz <= iz1; ++iz)
            for (int32_t ix = ix0; ix <= ix1; ++ix) {
                const uint32_t h = hashCell(ix, iz, seed);
                const float x = ((float)ix + 0.07f + h01(h, 3u) * 0.86f) * sp;
                const float z = ((float)iz + 0.07f + h01(h, 4u) * 0.86f) * sp;
                if (x < x0 || x > x1 || z < z0 || z > z1) continue;
                if (!inShape(x, z)) continue;
                tryPlant(x, z, h, oakBias);
            }
    };

    struct RegionLog { const char* name; uint32_t count, corr, slope, jct; };
    std::vector<RegionLog> rlog;
    uint32_t lastCorr = 0, lastSlope = 0, lastJct = 0;
    auto logRegion = [&](const char* name, uint32_t before) {
        rlog.push_back({ name, (uint32_t)m_trees.size() - before,
                         rjCorr - lastCorr, rjSlope - lastSlope, rjJct - lastJct });
        lastCorr = rjCorr; lastSlope = rjSlope; lastJct = rjJct;
    };

    // =======================================================================
    // THE REGIONS — the sketch's brown, in world coordinates (+Z north).
    // =======================================================================
    uint32_t mark;

    // 1. THE NORTH BELT — "Forest" across the whole north edge: the N snow
    //    range's south foothills (kRanges[0] spine z~8300, outW 2200). The
    //    outer tour's north reach drives through it.
    mark = (uint32_t)m_trees.size();
    plantGrid(-4200.0f, 6350.0f, 4200.0f, 7250.0f, 32.0f, 0xF00E571u, 0.45f,
              [](float, float) { return true; });
    logRegion("north belt", mark);

    // 2. THE CENTRE-NORTH PATCH — the free-standing "Forest" blob between the
    //    inner tour's north bulge and the outer tour's NE reach.
    mark = (uint32_t)m_trees.size();
    plantGrid(550.0f, 3300.0f, 2850.0f, 4500.0f, 26.0f, 0xCE47E2u, 0.55f,
              [](float x, float z) {
                  const float nx = (x - 1700.0f) / 1150.0f, nz = (z - 3900.0f) / 600.0f;
                  return nx * nx + nz * nz <= 1.0f;
              });
    logRegion("centre-north patch", mark);

    // 3. THE NE CORNER — "This Color is All Forest": the country between the
    //    N range's east end and the E range's north end.
    mark = (uint32_t)m_trees.size();
    plantGrid(5600.0f, 5600.0f, 8300.0f, 8300.0f, 44.0f, 0x4EC04E4u, 0.5f,
              [](float, float) { return true; });
    logRegion("NE corner", mark);

    // 4. THE SOUTHERN BELT — "Tthick woods on much of the road!!!!": a band
    //    flanking the inner tour's whole southern arc (nodes south of the
    //    tour centre by 2 km). Trunks 28..115 m off the centreline, BOTH
    //    sides — the corridor keep-out holds the pavement + aprons clear, so
    //    the woods wall the drive without touching it.
    if (in.innerTour && in.innerTour->x.size() >= 2) {
        mark = (uint32_t)m_trees.size();
        const RoadSpec& rs = *in.innerTour;
        const float zCut = -352.0f - 2000.0f;   // tour centre (-592,-352) — ROAD_NETWORK_PLAN
        // The inner tour is a DUAL freeway: its carve footprint
        // (kFwyDualMaxHalfM ~57 m + 18 m falloff ~= 75 m) already rejects
        // everything inside it via terrainCorridorContains — measured: an
        // inner band of 28 m starved the belt to 558 trees. So the authored
        // band opens at 30 m and the FOOTPRINT trims the real inner edge:
        // the front row lands right at the falloff lip, which is exactly
        // "woods walling the drive".
        std::vector<Seg> segs;
        for (size_t i = 1; i < rs.x.size(); ++i)
            if (rs.z[i - 1] <= zCut || rs.z[i] <= zCut)
                segs.push_back(makeSeg(rs.x[i - 1], rs.z[i - 1], rs.x[i], rs.z[i]));
        plantPolylineBand(segs, 30.0f, 165.0f, 16.0f, 0x50D7BE17u, 0.5f, 0);
        logRegion("southern belt", mark);
    }

    // 5. LARGE MOUNTAIN SKIRT — forest around the tunnel ridge's foot. Spine
    //    segment PAIRED with terrain.cpp kRanges[4] (-753,-740)->(-431,36):
    //    band 250..500 m out (outW 240, so planting starts where the rock
    //    releases into the rolling field). The spawn bore, its portals, the
    //    connector and the garage all live inside corridor/junction keep-outs.
    mark = (uint32_t)m_trees.size();
    plantGrid(-753.0f - 500.0f, -740.0f - 500.0f, -431.0f + 500.0f, 36.0f + 500.0f,
              19.0f, 0x14B6E571u, 0.6f,
              [](float x, float z) {
                  const float d2 = segDist2(x, z, -753.0f, -740.0f, -431.0f, 36.0f);
                  return d2 >= 250.0f * 250.0f && d2 <= 500.0f * 500.0f;
              });
    logRegion("large-mountain skirt", mark);

    // 6. THE SECOND MOUNTAIN'S SKIRT — the summit-spur peak (393, 6752), the
    //    sketch's spiral-road mountain family (ROAD_NETWORK_PLAN anchor
    //    table). Band 160..520 m — the summit lot disc is in kPadKeep.
    mark = (uint32_t)m_trees.size();
    plantGrid(393.0f - 520.0f, 6752.0f - 520.0f, 393.0f + 520.0f, 6752.0f + 520.0f,
              22.0f, 0x5B02572u, 0.5f,
              [](float x, float z) {
                  const float dx = x - 393.0f, dz = z - 6752.0f;
                  const float d2 = dx * dx + dz * dz;
                  return d2 >= 160.0f * 160.0f && d2 <= 520.0f * 520.0f;
              });
    logRegion("spur-mountain skirt", mark);

    // 7+8. THE RIVER STRIPS — the sketch's brown strip along the CLIFFSIDE
    //    HIGHWAY beside the LARGE RIVER: a thick band down the west bank
    //    (between the river road and the water) and a lighter scatter on the
    //    east bank. Offsets clear the 34 m water ribbon + the 26 m bank shelf;
    //    the river road's own corridor keep-out carves its right-of-way out.
    {
        uint32_t rc = 0;
        const WorldRiverNode* rn = worldRiverNodes(rc);
        if (rn && rc >= 2) {
            std::vector<Seg> segs;
            for (uint32_t i = 1; i < rc; ++i)
                segs.push_back(makeSeg(rn[i - 1].x, rn[i - 1].z, rn[i].x, rn[i].z));
            // side of the channel: cross(seg dir, p - a).y — the flow runs
            // north->south, so negative = WEST bank (the cliffside strip).
            mark = (uint32_t)m_trees.size();
            plantPolylineBand(segs, 46.0f, 105.0f, 12.0f, 0x81BE21u, 0.35f, -1);
            logRegion("river west strip", mark);
            mark = (uint32_t)m_trees.size();
            plantPolylineBand(segs, 46.0f, 90.0f, 20.0f, 0x81BE22u, 0.35f, +1);
            logRegion("river east scatter", mark);
        }
    }

    // ---- chunk the trees for per-frame pruning ----------------------------
    {
        std::unordered_map<uint64_t, std::vector<uint32_t>> byChunk;
        for (uint32_t i = 0; i < (uint32_t)m_trees.size(); ++i) {
            const int32_t cx = (int32_t)std::floor(m_trees[i].t[12] / kChunkM);
            const int32_t cz = (int32_t)std::floor(m_trees[i].t[14] / kChunkM);
            byChunk[((uint64_t)(uint32_t)cx << 32) | (uint32_t)cz].push_back(i);
        }
        std::vector<Tree> sorted;
        sorted.reserve(m_trees.size());
        m_chunks.reserve(byChunk.size());
        // deterministic chunk order (hash-map iteration is not)
        std::vector<uint64_t> keys;
        keys.reserve(byChunk.size());
        for (const auto& kv : byChunk) keys.push_back(kv.first);
        std::sort(keys.begin(), keys.end());
        for (uint64_t k : keys) {
            const auto& v = byChunk[k];
            Chunk c{};
            c.begin = (uint32_t)sorted.size();
            for (uint32_t ti : v) sorted.push_back(m_trees[ti]);
            c.end = (uint32_t)sorted.size();
            const int32_t cx = (int32_t)(k >> 32), cz = (int32_t)(k & 0xFFFFFFFFu);
            c.cx = ((float)cx + 0.5f) * kChunkM;
            c.cz = ((float)cz + 0.5f) * kChunkM;
            m_chunks.push_back(c);
        }
        m_trees.swap(sorted);
    }

    std::string msg = "forest: " + std::to_string(m_trees.size()) + " trees (" +
                      std::to_string(m_oaks) + " oak, " + std::to_string(m_poplars) +
                      " poplar) in " + std::to_string(m_chunks.size()) + " chunks (" +
                      std::to_string(rejected) + " rejected):";
    for (const RegionLog& r : rlog)
        msg += std::string(" ") + r.name + "=" + std::to_string(r.count) +
               "(rj c" + std::to_string(r.corr) + "/s" + std::to_string(r.slope) +
               "/j" + std::to_string(r.jct) + ")";
    x3::logInfo(msg);
    x3::logInfo("forest: rejects — pad=" + std::to_string(rjPad) +
                " water=" + std::to_string(rjWater) +
                " corridor=" + std::to_string(rjCorr) +
                " junction=" + std::to_string(rjJct) +
                " demoband=" + std::to_string(rjDemo) +
                " spacing=" + std::to_string(rjClose) +
                " slope=" + std::to_string(rjSlope) +
                " sea=" + std::to_string(rjSea));
    if (m_trees.empty())
        x3::logWarn("forest: NOTHING PLANTED — GLBs missing or keep-outs ate the map?");
    return !m_trees.empty();
}

// ===========================================================================
// DRAW
// ===========================================================================
void WorldForests::submitTree(x3::rhi::IRenderDevice& device,
                              const x3::rhi::FrameContext& frame,
                              Tree& tr, bool nearTier, uint32_t& drawn) {
    const Species& sp = m_species[tr.species];
    // No textured card (stale store copy)? The honest degrade is a SHORT draw
    // distance, not 10^4 full LOD0 meshes — a far tree simply does not draw.
    if (!nearTier && sp.card.empty()) return;
    const std::vector<Drawable>& list = nearTier ? sp.lod0 : sp.card;
    for (const Drawable& dw : list) {
        const x3::asset::ModelDrawable& d = dw.d;
        float fin[16];
        x3::asset::mulMat4(tr.t, d.nodeTransform, fin);
        const float emis[4] = { 0, 0, 0, 0 };
        device.drawMeshPBR(frame,
                           x3::rhi::MeshHandle{ d.meshId },
                           x3::rhi::TextureHandle{ d.baseColorTexId },
                           x3::rhi::TextureHandle{ d.normalTexId },
                           x3::rhi::TextureHandle{ d.mrTexId },
                           d.baseColorFactor, emis, fin,
                           d.alphaMask, d.alphaBlend,
                           x3::rhi::TextureHandle{ d.emissiveTexId },
                           x3::rhi::TextureHandle{ d.detailTexId },
                           d.detailUvScale,
                           d.clearcoat, d.clearcoatRough,
                           /*selfLight=*/0.0f, /*metallicScale=*/1.0f,
                           /*foliage=*/1.0f);   // canopy wrap + back-translucency
        ++drawn;
    }
}

uint32_t WorldForests::draw(x3::rhi::IRenderDevice& device,
                            const x3::rhi::FrameContext& frame,
                            const float cam[3], float fwdX, float fwdZ) {
    if (m_destroyed || m_trees.empty()) return 0;
    // A/B instruments (perf isolation only — never ship set): X3_FOREST_NEAR=0
    // skips the LOD0 tier, X3_FOREST_FAR=0 skips the billboard tier.
    static const bool kSkipNear = [] {
        const char* e = std::getenv("X3_FOREST_NEAR"); return e && e[0] == '0'; }();
    static const bool kSkipFar = [] {
        const char* e = std::getenv("X3_FOREST_FAR"); return e && e[0] == '0'; }();
    // normalize the forward (callers pass cos/sin of yaw or a raw dir)
    const float fl = std::sqrt(fwdX * fwdX + fwdZ * fwdZ);
    if (fl > 1e-5f) { fwdX /= fl; fwdZ /= fl; }
    const float chunkR = kChunkM * 0.7071f + 1.0f;
    const float farCull2 = (kFarMax + chunkR) * (kFarMax + chunkR);
    uint32_t drawn = 0;
    for (const Chunk& c : m_chunks) {
        const float dx = c.cx - cam[0], dz = c.cz - cam[2];
        const float d2 = dx * dx + dz * dz;
        if (d2 > farCull2) continue;
        // behind-the-camera half-space, conservative: never applied to the
        // chunk the camera stands in/near (mirrors, shadows still see near
        // geometry via the CSM light frustum — CSM renders only what the
        // main-view records submitted, so keep a generous pad).
        if (d2 > (chunkR + 220.0f) * (chunkR + 220.0f) &&
            fl > 1e-5f && dx * fwdX + dz * fwdZ < -(chunkR + 120.0f)) continue;
        for (uint32_t i = c.begin; i < c.end; ++i) {
            Tree& tr = m_trees[i];
            const float tx = tr.t[12] - cam[0], tz = tr.t[14] - cam[2];
            const float td2 = tx * tx + tz * tz;
            if (td2 > kFarMax * kFarMax) continue;
            // tier with hysteresis (kNearIn enter, kNearOut leave)
            bool nearTier = tr.nearTier != 0;
            if (nearTier) { if (td2 > kNearOut * kNearOut) nearTier = false; }
            else          { if (td2 < kNearIn  * kNearIn)  nearTier = true;  }
            tr.nearTier = nearTier ? 1 : 0;
            if (nearTier ? kSkipNear : kSkipFar) continue;
            submitTree(device, frame, tr, nearTier, drawn);
        }
    }
    return drawn;
}

void WorldForests::shutdown(x3::rhi::IRenderDevice& device) {
    (void)device;
    if (!m_built || m_destroyed) return;
    m_destroyed = true;
    // Unload through the SAME loader that created the resources (textures are
    // process-wide refcounted — see EnvArtSystem::destroy's design notes).
    if (m_loader) {
        for (Species& sp : m_species)
            if (sp.model.ok) m_loader->unload(sp.model);
    }
    m_trees.clear();
    m_chunks.clear();
}

} // namespace x3::game
