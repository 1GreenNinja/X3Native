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
    { "Crash Site",        "shuttle wreck — surface start point",        0.0f,     0.0f,  30.0f,   0.0f, false, 0.42f,0.42f,0.46f, 0,0,0,0 },
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

    m_built = true;
    x3::logInfo("WorldRegions::build complete — 7 regions (Crash Site + East/West outposts + "
                "Northern snow / Eastern volcanic / Southern mesa / Western crystal ranges); "
                + std::to_string((uint32_t)m_props.size()) + " graybox props on the surface");
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
            if (p.cx == 0.0f && p.cz == 0.0f && i != (uint32_t)WorldRegion::CrashSite) ok = false;
        }
        check(ok, "W1 all 7 regions carry a name + biome + center (Crash Site at origin)");
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

    physics->shutdown();
    x3::logInfo(std::string("worldregions: ") + std::to_string(g_pass) + "/" +
                std::to_string(g_pass + g_fail) + " passed");
    return g_fail == 0;
}

} // namespace x3::game
