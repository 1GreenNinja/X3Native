// EFLZ Act-2 open world — surrounding surface regions + the 4 mountain ranges.
// See world_regions.h.
//
// Clean-room: built ONLY from X3Native's own Scene / terrain / mesh_prims systems +
// the engine interfaces + the EFLZ design (Tim's own Q3Engine world modules as the
// content reference). No RBDOOM / id Tech / Doom / Quake source consulted. Graybox
// landmark geometry on the procedural terrain surface; mirrors act2_world.cpp.
#include "world_regions.h"
#include "headless_device.h"
#include "mesh_prims.h"

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace x3::game {

namespace {

// World position sitting ON the canonical terrain surface at (x,z) + a Y offset.
x3::phys::Vec3 surfaceAt(float x, float z, float yOff) {
    float p[3];
    placeOnTerrain(x, z, p);   // {x, surfaceY, z}
    return x3::phys::Vec3{ p[0], p[1] + yOff, p[2] };
}

// ---- Region table (blueprint gazetteer §1, W8-3: re-centered on the REAL
// terrain mountain ranges — terrain.cpp kRanges band midpoints; peak heights =
// the ranges' terrain amplitudes). cx,cz world center; radius footprint; peakH
// peak height above surface (0 = flat outpost); isMountain; biome one-liner. ----
struct RegionDef { const char* name; const char* biome; float cx, cz, radius, peakH; bool mtn;
                   float r, g, b; float er, eg, eb, es; };  // tint + emissive(rgb,strength)
const RegionDef kRegions[kWorldRegionCount] = {
    // SEAM 3 (world merge): the Crash Site moved OFF the origin — the canon
    // tower + its apron/facade own (0,0) now (footprint x0..50, z-40..40 +
    // skirt). Blueprint spirit kept: the wreck sits in the middle distance on
    // the +Z (breach/Entrance) face, ~230 m from the tower center with a clear
    // sightline from the apron, between the Spire-approach road legs (x=22 and
    // x=170) and clear of both, so nothing straddles the asphalt.
    { "Crash Site",        "shuttle wreck — surface start point",      140.0f,   205.0f,  30.0f,   0.0f, false, 0.42f,0.42f,0.46f, 0,0,0,0 },
    { "East Outpost",      "military camp + antenna farm",             800.0f,   400.0f,  45.0f,   0.0f, false, 0.55f,0.50f,0.40f, 0,0,0,0 },
    { "West Outpost",      "drill rig + processing plant (industrial)",-880.0f,  -320.0f,  45.0f,   0.0f, false, 0.52f,0.40f,0.30f, 0,0,0,0 },
    { "Northern Range",    "jagged snow-capped peaks",                 300.0f,  8300.0f, 2500.0f, 380.0f, true,  0.90f,0.92f,0.96f, 0,0,0,0 },
    { "Eastern Range",     "volcanic basalt + lava veins",            9200.0f,   250.0f, 2250.0f, 460.0f, true,  0.18f,0.16f,0.18f, 0.85f,0.25f,0.05f, 2.2f },
    { "Southern Range",    "mesa / plateau sandstone + ancient ruins", 350.0f, -9000.0f, 3150.0f, 230.0f, true,  0.72f,0.55f,0.35f, 0,0,0,0 },
    { "Western Highlands", "mossy rolling hills + crystal formations",-8600.0f, -100.0f, 1900.0f, 320.0f, true,  0.30f,0.52f,0.40f, 0.30f,0.65f,0.95f, 1.6f },
};

// The mountain-range band spines (MUST match terrain.cpp kRanges): summit
// accent props are placed ALONG these on the real terrain surface.
struct RangeBand { float ax, az, bx, bz; };
const RangeBand kRangeBands[4] = {
    { -2200.0f,  8300.0f,  2800.0f,  8300.0f },   // Northern
    {  9200.0f, -2000.0f,  9200.0f,  2500.0f },   // Eastern
    { -2800.0f, -9000.0f,  3500.0f, -9000.0f },   // Southern
    { -8600.0f, -2000.0f, -8600.0f,  1800.0f },   // Western
};

} // namespace

// ---- AUTHORED PLANS, readable in the BOOT slot (see world_regions.h) ------
uint32_t worldRegionPlanCount() { return kWorldRegionCount; }

const WorldRegionPlan& worldRegionPlanAuthored(uint32_t i) {
    static WorldRegionPlan pl[kWorldRegionCount];
    static bool init = false;
    if (!init) {
        for (uint32_t k = 0; k < kWorldRegionCount; ++k) {
            pl[k].region = (WorldRegion)k;
            pl[k].name   = kRegions[k].name;
            pl[k].biome  = kRegions[k].biome;
            pl[k].cx     = kRegions[k].cx;
            pl[k].cz     = kRegions[k].cz;
            pl[k].radius = kRegions[k].radius;
            pl[k].peakH  = kRegions[k].peakH;
            pl[k].isMountain = kRegions[k].mtn;
        }
        init = true;
    }
    return pl[i < kWorldRegionCount ? i : 0];
}

void WorldRegions::build(Scene& scene, x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics) {
    (void)physics;   // graybox landmarks are visual-only this pass (no collision body)

    // A graybox box prop sitting in world space; recorded as a Scene entity. `emiss`
    // (rgb,strength) glows for lava/crystal accents. Returns nothing; appends the id.
    auto addBoxProp = [&](float cx, float cy, float cz, float hx, float hy, float hz,
                          const float col[4], const float emiss[4]) {
        x3::prims::PrimMesh m = x3::prims::makeBox(hx, hy, hz, cx, cy, cz, 0.5f);
        Entity e;
        e.mesh = device.createMesh(m.verts.data(), (uint32_t)m.verts.size(),
                                   m.index.data(), (uint32_t)m.index.size());
        e.baseColor[0] = col[0]; e.baseColor[1] = col[1]; e.baseColor[2] = col[2]; e.baseColor[3] = 1.0f;
        if (emiss) { e.emissive[0]=emiss[0]; e.emissive[1]=emiss[1]; e.emissive[2]=emiss[2]; e.emissive[3]=emiss[3]; }
        e.tag = (uint32_t)Tag::Prop;
        m_props.push_back(scene.add(e));
    };

    for (uint32_t i = 0; i < kWorldRegionCount; ++i) {
        const RegionDef& d = kRegions[i];
        WorldRegionPlan& p = m_plan[i];
        p.region   = (WorldRegion)i;
        p.name     = d.name;
        p.biome    = d.biome;
        p.cx       = d.cx; p.cz = d.cz;
        p.radius   = d.radius;
        p.peakH    = d.peakH;
        p.isMountain = d.mtn;

        const float col[4]   = { d.r, d.g, d.b, 1.0f };
        const float emiss[4] = { d.er, d.eg, d.eb, d.es };
        const float* emp = (d.es > 0.0f) ? emiss : nullptr;
        const uint32_t propsBefore = (uint32_t)m_props.size();

        float base[3];
        placeOnTerrain(d.cx, d.cz, base);   // surface Y at the region center

        if (!d.mtn) {
            // ---- OUTPOST: a small cluster of graybox buildings on the surface. ----
            const float bx[5] = { 0.0f,  d.radius*0.5f, -d.radius*0.5f,  d.radius*0.3f, -d.radius*0.35f };
            const float bz[5] = { 0.0f, -d.radius*0.4f,  d.radius*0.45f, d.radius*0.5f, -d.radius*0.5f };
            const float bh[5] = { 5.0f,  3.5f,           4.0f,           3.0f,          6.0f };  // building heights
            for (int b = 0; b < 5; ++b) {
                float s[3]; placeOnTerrain(d.cx + bx[b], d.cz + bz[b], s);
                addBoxProp(s[0], s[1] + bh[b] * 0.5f, s[2], 5.0f, bh[b] * 0.5f, 5.0f, col, nullptr);
            }
        } else {
            // ---- MOUNTAIN (W8-3): the peaks are REAL TERRAIN now (terrain.cpp
            // worldFeatures mountain ranges) — the old floating stepped-box
            // massifs are retired. This lane places SUMMIT ACCENTS conformal to
            // the terrain surface along the range's band spine: lava vents on
            // the volcanic east / crystal shards on the western highlands (the
            // emissive accent), ruin slabs on the southern mesas, rock cairns on
            // the northern snow range. Small props — character up close, the
            // RANGE ITSELF is the landmark from afar. ----
            const RangeBand& band = kRangeBands[i - 3];   // regions 3..6 are the ranges
            const float ts[4] = { 0.15f, 0.40f, 0.62f, 0.85f };
            for (int a = 0; a < 4; ++a) {
                const float px = band.ax + (band.bx - band.ax) * ts[a];
                const float pz = band.az + (band.bz - band.az) * ts[a];
                float s[3]; placeOnTerrain(px, pz, s);
                // A conformal accent cluster: one main block + one lean-to shard.
                const bool glow = (d.es > 0.0f) && (a % 2 == 0);   // accent every other site
                addBoxProp(s[0], s[1] + 4.0f, s[2], 3.5f, 4.0f, 3.5f, col, glow ? emp : nullptr);
                addBoxProp(s[0] + 5.0f, s[1] + 2.0f, s[2] - 3.0f, 1.6f, 2.4f, 1.6f, col,
                           (d.es > 0.0f && a % 2 == 1) ? emp : nullptr);
            }
        }
        p.propCount = (uint32_t)m_props.size() - propsBefore;
    }

    // ==== W9 (TERRAIN DRAMA) — THE RIVER's water surface. ====
    // One mitred ribbon mesh following the authored river spline exported by
    // terrain.h (worldRiverNodes — the SAME table the height-field carve uses,
    // so the water always lies inside its own channel: the terrain layer holds
    // the bed at waterY-3.2 and the bank crests at >= waterY+2.2). The ribbon
    // is 68 m wide (kWorldRiverHalfWidth*2) — the banks cross the water level
    // between ~+/-27 m (full-crest reaches) and ~+/-34 m (the beach reach with
    // its floodplain shelf), so the ribbon edges land ON or just inside the
    // banks (no floating edges; the shallow bank slopes cut the plane at a
    // clean waterline).
    // Water Y slopes node-to-node (a real downhill gradient, 0.3-3%), ending at
    // the sea surface (-9.9 vs the ocean plane's -10, 0.1 m proud where they
    // overlap). Rendered like the ocean plane, as a translucent water-tinted
    // surface — routed through the existing GLASS pass (transparent=true) so it
    // picks up real alpha blend + specular shimmer; same water tint as the
    // ocean slab (ocean_base.cpp). NO collision — the carved bed underneath is
    // the walkable/wadable surface (v1: wading, no swimming).
    {
        uint32_t nNodes = 0;
        const WorldRiverNode* rn = worldRiverNodes(nNodes);
        if (nNodes >= 2) {
            std::vector<x3::rhi::MeshVertex> verts;
            std::vector<uint32_t> idx;
            verts.reserve(nNodes * 2);
            idx.reserve((nNodes - 1) * 6);
            float arc = 0.0f;
            for (uint32_t i = 0; i < nNodes; ++i) {
                // Mitre direction: average of the adjacent segment directions.
                const uint32_t iPrev = (i == 0) ? 0 : i - 1;
                const uint32_t iNext = (i + 1 < nNodes) ? i + 1 : i;
                float dx = rn[iNext].x - rn[iPrev].x;
                float dz = rn[iNext].z - rn[iPrev].z;
                const float len = std::sqrt(dx * dx + dz * dz);
                if (len > 1e-4f) { dx /= len; dz /= len; }
                const float px = -dz, pz = dx;             // left perpendicular
                if (i > 0) {
                    const float sx = rn[i].x - rn[i-1].x, sz = rn[i].z - rn[i-1].z;
                    arc += std::sqrt(sx * sx + sz * sz);
                }
                x3::rhi::MeshVertex vL{}, vR{};
                vL.pos[0] = rn[i].x + px * kWorldRiverHalfWidth;
                vL.pos[1] = rn[i].waterY;
                vL.pos[2] = rn[i].z + pz * kWorldRiverHalfWidth;
                vR.pos[0] = rn[i].x - px * kWorldRiverHalfWidth;
                vR.pos[1] = rn[i].waterY;
                vR.pos[2] = rn[i].z - pz * kWorldRiverHalfWidth;
                vL.normal[0] = 0; vL.normal[1] = 1; vL.normal[2] = 0;
                vR.normal[0] = 0; vR.normal[1] = 1; vR.normal[2] = 0;
                vL.uv[0] = 0.0f; vL.uv[1] = arc / 24.0f;
                vR.uv[0] = 1.0f; vR.uv[1] = arc / 24.0f;
                verts.push_back(vL);
                verts.push_back(vR);
            }
            for (uint32_t i = 0; i + 1 < nNodes; ++i) {
                const uint32_t a = i * 2, b = a + 1, c = a + 2, d = a + 3;
                idx.insert(idx.end(), { a, c, b,  b, c, d });
            }
            Entity e;
            e.mesh = device.createMesh(verts.data(), (uint32_t)verts.size(),
                                       idx.data(), (uint32_t)idx.size());
            // The ocean plane's water tint (ocean_base.cpp), through the glass pass.
            e.baseColor[0] = 0.10f; e.baseColor[1] = 0.28f;
            e.baseColor[2] = 0.42f; e.baseColor[3] = 0.55f;
            e.transparent = true;
            e.glass.opacity    = 0.62f;
            e.glass.refraction = 0.02f;
            e.glass.roughness  = 0.08f;
            e.glass.specular   = 0.9f;
            e.glass.tint[0] = 0.16f; e.glass.tint[1] = 0.34f; e.glass.tint[2] = 0.42f;
            e.tag = (uint32_t)Tag::Prop;
            m_props.push_back(scene.add(e));
            m_riverSegments = nNodes - 1;
        }
    }

    m_built = true;
    x3::logInfo("WorldRegions::build complete — 7 regions (Crash Site + East/West outposts + "
                "Northern snow / Eastern volcanic / Southern mesa / Western crystal ranges) + "
                "THE RIVER water ribbon (" + std::to_string(m_riverSegments) + " segments); "
                + std::to_string((uint32_t)m_props.size()) + " props on the surface");
}

uint32_t WorldRegions::mountainCount() const {
    uint32_t n = 0;
    for (uint32_t i = 0; i < kWorldRegionCount; ++i) if (m_plan[i].isMountain) ++n;
    return n;
}

// ===========================================================================
// Headless self-test (--test-worldregions).
// ===========================================================================
namespace {

int g_pass = 0, g_fail = 0;
void check(bool cond, const std::string& name) {
    if (cond) { ++g_pass; x3::logInfo("[worldregions-test] PASS " + name); }
    else      { ++g_fail; x3::logError("[worldregions-test] FAIL " + name); }
}
using HeadlessDevice = x3::game::HeadlessRenderDevice;
bool nonEmpty(const char* s) { return s && s[0] != '\0'; }

} // namespace

bool runWorldRegionsSelfTest() {
    g_pass = g_fail = 0;

    std::unique_ptr<x3::phys::IPhysicsWorld> physics(x3::phys::createPhysicsWorld());
    physics->init();
    HeadlessDevice device;
    Scene scene;

    WorldRegions w;
    w.build(scene, device, *physics);

    check(w.built(), "W0 world regions built");

    // ---- All 7 regions present, named + biomed, at their authored centers. ----
    {
        bool ok = true;
        for (uint32_t i = 0; i < kWorldRegionCount; ++i) {
            const WorldRegionPlan& p = w.plan((WorldRegion)i);
            if (!nonEmpty(p.name) || !nonEmpty(p.biome)) ok = false;
            // SEAM 3: NO region sits at the origin anymore (the canon tower owns it).
            if (p.cx == 0.0f && p.cz == 0.0f) ok = false;
        }
        check(ok, "W1 all 7 regions carry a name + biome + center");
    }

    // ---- SEAM 3: the Crash Site is CLEAR of the canon tower + apron (it was
    // authored at the origin; the tower footprint x0..50/z-40..40 + facade +
    // apron own that ground now) yet still in the middle distance (<400 m). ----
    {
        const WorldRegionPlan& crash = w.plan(WorldRegion::CrashSite);
        const float pad = 30.0f;   // footprint + facade/apron padding
        const bool clearOfTower =
            (crash.cx - crash.radius > 50.0f + pad) || (crash.cx + crash.radius < 0.0f - pad) ||
            (crash.cz - crash.radius > 40.0f + pad) || (crash.cz + crash.radius < -40.0f - pad);
        const float dist = std::sqrt((crash.cx - 25.0f) * (crash.cx - 25.0f) +
                                     crash.cz * crash.cz);
        check(clearOfTower && dist > 120.0f && dist < 400.0f,
              "W6 Crash Site relocated: clear of the canon tower footprint, middle distance");
    }

    // ---- 3 flat outposts (peakH==0) + 4 tall mountain ranges (peakH>0). ----
    {
        const WorldRegionPlan& crash = w.plan(WorldRegion::CrashSite);
        const WorldRegionPlan& east  = w.plan(WorldRegion::EastOutpost);
        const WorldRegionPlan& west  = w.plan(WorldRegion::WestOutpost);
        bool outpostsFlat = crash.peakH == 0.0f && !crash.isMountain &&
                            east.peakH  == 0.0f && !east.isMountain &&
                            west.peakH  == 0.0f && !west.isMountain;
        bool fourMountains = w.mountainCount() == 4;
        check(outpostsFlat && fourMountains,
              "W2 3 flat outposts + 4 mountain ranges (mountainCount==4)");
    }

    // ---- Mountains are FAR (6-9 km out) + tall; outposts are near (<2 km). ----
    {
        bool ok = true;
        for (uint32_t i = 0; i < kWorldRegionCount; ++i) {
            const WorldRegionPlan& p = w.plan((WorldRegion)i);
            const float dist = std::sqrt(p.cx * p.cx + p.cz * p.cz);
            if (p.isMountain) { if (dist < 5000.0f || p.peakH < 100.0f) ok = false; }
            else              { if (dist > 2000.0f) ok = false; }
        }
        check(ok, "W3 mountains 6-9 km out + >=100 m tall; outposts within 2 km");
    }

    // ---- Every region placed >=1 graybox prop; total props > 0. ----
    {
        bool eachHasProps = true;
        for (uint32_t i = 0; i < kWorldRegionCount; ++i)
            if (w.plan((WorldRegion)i).propCount == 0) eachHasProps = false;
        check(eachHasProps && w.propCount() > 0,
              "W4 every region placed graybox props (total " + std::to_string(w.propCount()) + ")");
    }

    // ---- A mountain has a multi-tier massif (>=2 boxes); an outpost a cluster (>=2). ----
    {
        bool ok = w.plan(WorldRegion::NorthernRange).propCount >= 2 &&
                  w.plan(WorldRegion::EastOutpost).propCount   >= 2;
        check(ok, "W5 mountain massif (>=2 tiers) + outpost cluster (>=2 buildings)");
    }

    // ==== W9 (terrain drama) — THE RIVER + canyon pass + bluff line. Pure
    // height-field assertions against the canonical world config (the same
    // field every host renders/streams/places on). ====

    // ---- W7: the river — ribbon placed; water levels descend monotonically;
    // the carved bed sits under the water; the banks contain it. ----
    {
        uint32_t n = 0;
        const WorldRiverNode* rn = worldRiverNodes(n);
        bool ribbon = w.riverSegmentCount() == n - 1 && n >= 8;
        bool downhill = true, bedUnder = true, contained = true;
        for (uint32_t i = 0; i + 1 < n; ++i)
            if (rn[i+1].waterY >= rn[i].waterY) downhill = false;
        const uint32_t nc = worldRiverCarveCount();
        for (uint32_t i = 0; i < nc; ++i) {
            const float bed = terrainHeightAtWorld(rn[i].x, rn[i].z);
            if (bed > rn[i].waterY - 1.0f) bedUnder = false;   // >=1 m of water
            // The estuary reach descends into the (deep) basin — skip there.
            { const float bx = rn[i].x - 1100.0f, bz = rn[i].z + 1350.0f;
              if (bx * bx + bz * bz < 700.0f * 700.0f) continue; }
            // Bank flanks to each side must hold the water: by construction
            // ground there is >= (closest-spine waterY)+0.2. Sample 55 m out
            // along the CHORD perpendicular — at a sharp bend (N3) the chord
            // perpendicular is up to ~35 deg off the segment normal, so 55 m
            // keeps the sample outside the waterline (~34 m) for any node; and
            // at a bend's inside the closest spine point sits slightly
            // DOWNSTREAM (water up to ~0.15 m lower), so allow -0.25 vs the
            // node's own level.
            const uint32_t j = (i + 1 < nc) ? i + 1 : i - 1;
            float dx = rn[j].x - rn[i].x, dz = rn[j].z - rn[i].z;
            const float len = std::sqrt(dx * dx + dz * dz);
            dx /= len; dz /= len;
            const float bL = terrainHeightAtWorld(rn[i].x - dz * 55.0f, rn[i].z + dx * 55.0f);
            const float bR = terrainHeightAtWorld(rn[i].x + dz * 55.0f, rn[i].z - dx * 55.0f);
            if (bL < rn[i].waterY - 0.25f || bR < rn[i].waterY - 0.25f) {
                contained = false;
                x3::logError("[worldregions-test] W7 node " + std::to_string(i) +
                             " w=" + std::to_string(rn[i].waterY) +
                             " bL=" + std::to_string(bL) + " bR=" + std::to_string(bR));
            }
        }
        if (!(ribbon && downhill && bedUnder && contained))
            x3::logError("[worldregions-test] W7 detail: ribbon=" + std::to_string(ribbon) +
                         " downhill=" + std::to_string(downhill) +
                         " bedUnder=" + std::to_string(bedUnder) +
                         " contained=" + std::to_string(contained));
        check(ribbon && downhill && bedUnder && contained,
              "W7 THE RIVER: ribbon placed, water descends to the sea, bed under water, banks contain it");
    }

    // ---- W8: the canyon pass — floor at the authored walkable grade, walls
    // standing >=10 m over it on both sides at the mid nodes. ----
    {
        const float cx[4] = { -140.0f, -230.0f, -160.0f,  -40.0f };   // = terrain.cpp kCanyon*
        const float cz[4] = { -520.0f, -760.0f, -1040.0f, -1260.0f };
        const float cf[4] = {    2.0f,    0.0f,    -2.0f,    -4.0f };
        bool floorOk = true, wallsOk = true;
        for (int i = 1; i < 3; ++i) {   // mid nodes (mouth/exit fade by design)
            const float f = terrainHeightAtWorld(cx[i], cz[i]);
            if (std::fabs(f - cf[i]) > 2.5f) floorOk = false;
            float dx = cx[i+1] - cx[i-1], dz = cz[i+1] - cz[i-1];
            const float len = std::sqrt(dx * dx + dz * dz);
            dx /= len; dz /= len;
            const float wL = terrainHeightAtWorld(cx[i] - dz * 45.0f, cz[i] + dx * 45.0f);
            const float wR = terrainHeightAtWorld(cx[i] + dz * 45.0f, cz[i] - dx * 45.0f);
            if (wL < f + 10.0f || wR < f + 10.0f) wallsOk = false;
        }
        check(floorOk && wallsOk, "W8 canyon pass: walkable authored floor + steep walls both sides");
    }

    // ---- W9: the bluff line — somewhere along the band the west (upper) side
    // stands well above the east (facility-plain) side. ----
    {
        float best = -1e9f;
        for (int i = 0; i < 5; ++i) {
            const float z = -380.0f + 100.0f * (float)i;   // along B0..B1
            const float west = terrainHeightAtWorld(-520.0f, z);
            const float east = terrainHeightAtWorld(-380.0f, z);
            best = std::max(best, west - east);
        }
        check(best >= 10.0f, "W9 bluff line: terraced step-up reads (west-east >= 10 m somewhere)");
    }

    // ---- W10 (SWIMMING): worldWaterLevelAt — the pure water-surface query the
    // swim controller runs on. Single source with the carve + ribbon: ON the
    // spine it returns that node's exact waterY; on the BANK (beyond the 34 m
    // half-width) it is dry; the facility plain is dry; the basin core is the
    // sea surface (-10 == kWorldSeaLevel, the ocean plane's own constant). ----
    {
        uint32_t n = 0;
        const WorldRiverNode* rn = worldRiverNodes(n);
        bool onRiver = true, banksDry = true;
        const uint32_t nc = worldRiverCarveCount();
        for (uint32_t i = 1; i + 1 < nc; ++i) {   // interior carve nodes
            // ON the spine: the query must return this node's waterY exactly.
            const float wq = worldWaterLevelAt(rn[i].x, rn[i].z);
            if (std::fabs(wq - rn[i].waterY) > 0.01f) {
                onRiver = false;
                x3::logError("[worldregions-test] W10 node " + std::to_string(i) +
                             " spine query " + std::to_string(wq) + " != " +
                             std::to_string(rn[i].waterY));
            }
            // BANK: 60 m out along the chord perpendicular (outside the 34 m
            // ribbon for any bend angle) must be dry. Skip the estuary reach —
            // there the flanks descend into the (legitimately wet) sea basin,
            // same exemption W7 uses.
            { const float bx = rn[i].x - 1100.0f, bz = rn[i].z + 1350.0f;
              if (bx * bx + bz * bz < 700.0f * 700.0f) continue; }
            float dx = rn[i+1].x - rn[i-1].x, dz = rn[i+1].z - rn[i-1].z;
            const float len = std::sqrt(dx * dx + dz * dz);
            dx /= len; dz /= len;
            const float bL = worldWaterLevelAt(rn[i].x - dz * 60.0f, rn[i].z + dx * 60.0f);
            const float bR = worldWaterLevelAt(rn[i].x + dz * 60.0f, rn[i].z - dx * 60.0f);
            if (bL > kWorldWaterDry || bR > kWorldWaterDry) {
                banksDry = false;
                x3::logError("[worldregions-test] W10 node " + std::to_string(i) +
                             " bank wet: bL=" + std::to_string(bL) +
                             " bR=" + std::to_string(bR));
            }
        }
        const bool facilityDry = worldWaterLevelAt(0.0f, 0.0f) <= kWorldWaterDry + 1.0f;
        const float sea = worldWaterLevelAt(1100.0f, -1350.0f);   // basin core
        const bool oceanOk = std::fabs(sea - kWorldSeaLevel) < 0.01f;
        // The mouth: the ribbon's last node rides 0.1 m proud of the sea — the
        // query must prefer the RIVER answer inside the ribbon (matches visuals).
        const float mouth = worldWaterLevelAt(rn[n-1].x, rn[n-1].z);
        const bool mouthOk = std::fabs(mouth - rn[n-1].waterY) < 0.01f;
        check(onRiver && banksDry && facilityDry && oceanOk && mouthOk,
              "W10 worldWaterLevelAt: river reaches wet at node waterY, banks + facility dry, "
              "sea = kWorldSeaLevel, estuary prefers the ribbon");
    }

    physics->shutdown();
    x3::logInfo(std::string("worldregions: ") + std::to_string(g_pass) + "/" +
                std::to_string(g_pass + g_fail) + " passed");
    return g_fail == 0;
}

} // namespace x3::game
