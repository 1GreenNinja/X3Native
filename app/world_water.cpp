// world_water.cpp — see world_water.h. Body ported VERBATIM (values and
// rationale) from the tunnel host's applyRiverWater lambda
// (app/world_hosts/host_tunnel.cpp, W-WATER / W-UNDERRIVER / W-NIGHT), which
// now calls this instead. Every number below is owner-approved on the tunnel
// world; change it HERE and both worlds move together.
#include "world_water.h"
#include "terrain.h"
#include "underground_river.h"
// --test-canonunderriver
#include "city.h"
#include "dealership.h"
#include "drive_layer.h"
#include "headless_device.h"
#include "scene.h"
#include "world_stream.h"     // kStreamedExteriorRoom (the canon PVS stamp)
#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <set>
#include <string>
#include <vector>

namespace x3::game {

using WP = x3::rhi::IRenderDevice::WaterParams;

namespace {

// X3_WATER_CLARITY: read once. <0 = not set (approved constants apply).
float clarityOverride() {
    static const float s_v = [] {
        const char* e = std::getenv("X3_WATER_CLARITY");
        return e && e[0] ? std::clamp((float)std::atof(e), 0.0f, 1.0f) : -1.0f;
    }();
    return s_v;
}

// Nearest-segment interpolation of a polyline's Y at (fx,fz). Used for the
// dry-focus seaLevel fallback on both channels.
template <class GetX, class GetZ, class GetY>
float polylineYNear(int n, GetX X, GetZ Z, GetY Y, float fx, float fz) {
    float best = n > 0 ? Y(0) : 0.0f, bd2 = 1e18f;
    for (int i = 0; i + 1 < n; ++i) {
        const float ax = X(i), az = Z(i), bx = X(i+1), bz = Z(i+1);
        const float ex = bx - ax, ez = bz - az;
        const float L2 = std::max(ex*ex + ez*ez, 1e-4f);
        const float t = std::clamp(((fx-ax)*ex + (fz-az)*ez) / L2, 0.0f, 1.0f);
        const float qx = ax + ex*t, qz = az + ez*t;
        const float d2 = (fx-qx)*(fx-qx) + (fz-qz)*(fz-qz);
        if (d2 < bd2) { bd2 = d2; best = Y(i) + (Y(i+1) - Y(i)) * t; }
    }
    return best;
}

} // namespace

float worldWaterClarityFor(bool inCavern) {
    const float o = clarityOverride();
    if (o >= 0.0f) return o;
    return inCavern ? kWorldWaterClarityCavern : kWorldWaterClaritySurface;
}

bool buildWorldWaterParams(const WorldWaterInput& in, WP& wpr, bool* outInCavern) {
    // THE CAVERN IS A BODY OF WATER TOO. When the focus is inside the
    // underground river's corridor the SAME pass draws THAT channel: same
    // Gerstner surface, same clarity, same foam, same caustics. It used to
    // be a CaveRiver ribbon and photographed as flat blue paper — see
    // app/underground_river.h. The two channels are 1.5 km apart and the
    // patch is 480 m, so they can never both be near the camera; one
    // polyline is enough and the switch cannot pop.
    const float focus3[3] = { in.focusX, 0.0f, in.focusZ };
    const bool inCavern = UndergroundRiver::insideCorridor(focus3);
    if (outInCavern) *outInCavern = inCavern;
    if (!inCavern && !in.surfaceOn) return false;
    // A DETERMINISTIC RESET, padding included. `wpr = WP{}` copies a temporary
    // whose padding bytes (3 after `enabled`) are whatever the stack held;
    // the CU4 gate's "back out is byte-identical" memcmp then depends on the
    // compiler's copy strategy, and it flipped the day the struct grew past
    // 2 KB (the room-light block). Zero the storage first, then run the
    // default member initialisers over it: every byte is now defined.
    std::memset(&wpr, 0, sizeof(WP));
    new (&wpr) WP();
    wpr.enabled = true;
    // ONE WATER TRUTH (task #32): the drawn surface follows the SAME node
    // table worldWaterLevelAt interpolates — stepped down the channel per
    // vertex in water.vert, estuary handed off to the real sea. The old
    // single flat plane at plan.waterY stood ~1.2 m/chain-node above the
    // carved table downstream and climbed the banks (receipt: the bench
    // that shipped submerged at (-340,11,-468) while PASSING the
    // worldWaterLevelAt+0.5 check).
    WorldRiverNode rn[WP::kMaxRiverNodes];
    uint32_t rnN = 0;
    if (inCavern) {
        const UnderRiverChain& uc = worldUnderRiverChain();
        const uint32_t n = std::min<uint32_t>((uint32_t)uc.n, WP::kMaxRiverNodes);
        wpr.riverNodeCount = n;
        for (uint32_t i = 0; i < n; ++i) {
            wpr.riverNodes[i][0] = uc.x[i];
            wpr.riverNodes[i][1] = uc.z[i];
            wpr.riverNodes[i][2] = uc.w[i];
        }
        // ONE number shared with worldWaterLevelAt's wet test (terrain.h
        // kURHalfWidth says why it is a constant), so drawn coverage and
        // the model cannot disagree down here either.
        wpr.riverHalfWidth = kURHalfWidth;
        // No ocean disc and no shoreline table underground: basinRadius 0
        // switches the estuary hand-off off entirely, which is what stops
        // the sea being drawn through a hill 3 km away.
        wpr.basinRadius    = 0.0f;
        // RUSHING WATER — but foam is a MASK strength, not a quantity of
        // whitewater. At 1.0 with a sun overhead the first capture came
        // back as white bands across the whole channel; the churn belongs
        // to the spray particles at the steps, and this is just the lace
        // where the water meets the rock. That 0.45 was a brightness trim
        // for sun-lit foam. Now the foam is ALBEDO under the room's light
        // (water.frag foamLit <- roomIrr): white under a bank lamp, grey
        // between them, never brighter than the lit rock beside it — so the
        // mask can run near full and the lace along the banks and steps
        // actually shows up in a dark cave.
        wpr.foam = 0.90f;
    } else {
        rnN = worldRiverRisenNodes(rn, WP::kMaxRiverNodes);
        wpr.riverNodeCount = rnN;
        for (uint32_t i = 0; i < rnN; ++i) {
            wpr.riverNodes[i][0] = rn[i].x;
            wpr.riverNodes[i][1] = rn[i].z;
            wpr.riverNodes[i][2] = rn[i].waterY;
        }
        wpr.riverHalfWidth = kWorldRiverHalfWidth;
        wpr.basinCenter[0] = kWorldOceanBasinX;
        wpr.basinCenter[1] = kWorldOceanBasinZ;
        wpr.basinRadius    = kWorldOceanBasinR;
        wpr.oceanLevel     = kWorldSeaLevel;
        // THE SHORELINE TABLE (W-UNDERRIVER): without it the shader draws
        // the sea across the whole basin disc — under the dry beach ring
        // and the rim hills too (the owner, noclip: "we do indeed have
        // water underground"). Computed ONCE from the same height field
        // worldWaterLevelAt tests (terrain.cpp worldOceanShoreTable),
        // lazily here so every road corridor is already registered.
        // RB11 (river_bridge.cpp) gates drawn-vs-model coverage map-wide.
        // Default ON; X3_WATER_SHORE=0 is the door for turning it OFF
        // (NO_SLOP rule 6) — it exists so the underground-sea defect can
        // be reproduced for an A/B receipt from the same binary.
        {
            static const bool kShoreOn = [] {
                const char* e = std::getenv("X3_WATER_SHORE");
                return !(e && e[0] == '0');
            }();
            static const std::vector<float> kShore = [] {
                std::vector<float> r(WP::kShoreSectors, 0.0f);
                worldOceanShoreTable(r.data(), WP::kShoreSectors);
                return r;
            }();
            if (kShoreOn) {
                wpr.shoreSectorCount = WP::kShoreSectors;
                std::memcpy(wpr.shoreRadii, kShore.data(), sizeof(float) * WP::kShoreSectors);
            }
        }
        // FOAM (the owner: "alive.. pulsing... writhing.. foaming if
        // needed"): contact foam hugs the banks, rocks and anything
        // breaking the surface; whitecaps stay quiet at this amplitude.
        wpr.foam = 0.85f;
    }
    // seaLevel carries the LOCAL level at the focus (underside-view gate +
    // caustics plane); dry land falls back per channel.
    const float lw = worldWaterLevelAt(in.focusX, in.focusZ);
    if (lw > kWorldWaterDry + 1.0f) {
        wpr.seaLevel = lw;
    } else if (inCavern) {
        // DRY FOCUS INSIDE THE CAVERN. seaLevel drives the underside-view
        // gate and the caustics plane, and the old fallback was the SURFACE
        // river's level — 1.5 km away and ~13 m ABOVE the cavern's own
        // water. Standing on a cavern beach (a dry query) therefore told
        // the shader the camera was submerged in a river it was nowhere
        // near. Fall back to THIS chain's own interpolated level instead.
        const UnderRiverChain& uc = worldUnderRiverChain();
        wpr.seaLevel = polylineYNear(uc.n,
            [&](int i) { return uc.x[i]; }, [&](int i) { return uc.z[i]; },
            [&](int i) { return uc.w[i]; }, in.focusX, in.focusZ);
    } else if (in.dryFallbackY) {
        wpr.seaLevel = *in.dryFallbackY;         // the tunnel's bridge plan level
    } else {
        // No bridge plan (canon): the nearest surface reach's own level, so
        // a camera on a hill above the river is not told it is underwater.
        wpr.seaLevel = polylineYNear((int)rnN,
            [&](int i) { return rn[i].x; }, [&](int i) { return rn[i].z; },
            [&](int i) { return rn[i].waterY; }, in.focusX, in.focusZ);
    }
    wpr.time      = in.time;
    wpr.amplitude = 0.16f;          // a river swell, not an ocean — and low
                                    // enough that a treading head clears
                                    // the crests instead of strobing them
    wpr.steepness = 0.35f;
    wpr.waveLength= 9.0f;
    wpr.speed     = 0.8f;
    wpr.deepColor[0]    = 0.008f; wpr.deepColor[1]    = 0.030f; wpr.deepColor[2]    = 0.038f;
    wpr.shallowColor[0] = 0.050f; wpr.shallowColor[1] = 0.150f; wpr.shallowColor[2] = 0.140f;
    wpr.specular  = 5.0f;
    wpr.fresnel   = 0.012f;
    // See-through shallows (WaterParams::clarity): the bed, the fish and a
    // swimmer's body read THROUGH face-on water; depth + grazing angles
    // close it back to a surface. 0 would be the legacy opaque plane.
    // ERR HIGH ON CLARITY (owner, 2026-08-28). The see-through ceiling that
    // made anything past six metres opaque is gone, so clarity now buys real
    // visibility instead of just thinning the shallows. The SURFACE river is
    // the everyday water: clear enough to read the bed, the fish and a
    // swimmer, still unmistakably a river. (0.82; X3_WATER_CLARITY overrides.)
    wpr.clarity   = worldWaterClarityFor(false);
    // W-NIGHT: the river glints to the LIVE luminary (sun by day, moon at
    // night), not to a phantom 14:00 sun that set hours ago.
    wpr.sunDir[0] = in.sunDir[0]; wpr.sunDir[1] = in.sunDir[1]; wpr.sunDir[2] = in.sunDir[2];
    if (inCavern) {
        // There is no sun down here, so a surface tuned to glint at one
        // renders as a black hole in the floor. The cavern's luminary is
        // the run's own bank lights: a steep overhead direction with a
        // soft, wide highlight, a cooler and slightly lifted shallow tint
        // so the carved bed reads THROUGH the water, and a choppier,
        // quicker swell for water that is actually falling.
        // ENCLOSED: no sky to mirror. Without this the surface reflects
        // the analytic daylight sky (a fixed bright gradient plus a sun
        // disk, driven to a full mirror at grazing angles by Schlick) and
        // the cave river photographed as crumpled chrome foil. enclosed=1
        // hands the reflection AND the distance/edge fade to horizonColor
        // and winds the sun glint out — so the water is lit by the room.
        wpr.enclosed = 1.0f;
        // What it sees instead of sky: the wet rock of its own vault — and
        // the vault is DARK (UndergroundRiver::applyAtmosphere winds the
        // daylight IBL down to a trace under the lid). This is the water's
        // floor between lamps; the lamps themselves arrive through
        // roomLights below. Any brighter and the sheet glows on its own
        // against the black rock — the flat cyan slab this replaced.
        wpr.horizonColor[0] = 0.006f;
        wpr.horizonColor[1] = 0.008f;
        wpr.horizonColor[2] = 0.012f;
        wpr.sunDir[0] = 0.12f; wpr.sunDir[1] = 0.92f; wpr.sunDir[2] = 0.10f;
        wpr.specular  = 0.0f;    // wound out by `enclosed` anyway; say it
        wpr.fresnel   = 0.020f;
        // THE SHOWPIECE WATER. Down here there is no silt, no runoff and no
        // weather — a cavern pool is the clearest water in the world, and the
        // owner asked for somewhere that is "very, very nice and clear". At
        // 0.94 the carved bed, the rock beaches and anything swimming read
        // straight through tens of metres of it.
        wpr.clarity   = worldWaterClarityFor(true);
        // A river swell, not a storm. 0.26/0.55 over a 5.5 m wavelength
        // pinched the Gerstner crests into shards in the first capture.
        wpr.amplitude = 0.11f;
        wpr.steepness = 0.30f;
        wpr.waveLength= 7.0f;
        wpr.speed     = 1.5f;
        wpr.deepColor[0]    = 0.010f; wpr.deepColor[1] = 0.030f; wpr.deepColor[2] = 0.044f;
        wpr.shallowColor[0] = 0.055f; wpr.shallowColor[1]= 0.150f; wpr.shallowColor[2]= 0.185f;
        // LIT BY THE ROOM. Every colour above is a radiance under an implied
        // daylight irradiance of 1, which is why the first captures of this
        // channel came back as a flat, glowing cyan slab — a light source in
        // a dark cave, ten times the brightness of the rock beside it and
        // indifferent to the 44 bank lights that light that rock. With the
        // lights handed over, water.frag treats deep/shallow as what the
        // water column scatters back PER UNIT of light it receives, lights it
        // with PI*horizonColor (the vault) plus these lamps, and puts each
        // lamp's lobe on the ripples. Dark water, lit where the banks are lit.
        // Picked at the focus AT THE WATER'S LEVEL (seaLevel was resolved
        // above for wet and dry focus alike): the run drops ~30 m end to end
        // and a lamp on the shelf above a fall is not "near" the pool below.
        x3::rhi::PointLight roomL[WP::kMaxRoomLights];
        uint32_t ln = 0;
        if (in.cavern && in.cavern->built()) {
            const float at[3] = { in.focusX, wpr.seaLevel, in.focusZ };
            ln = in.cavern->nearestLights(at, roomL, WP::kMaxRoomLights);
        }
        wpr.roomLightCount = ln;
        for (uint32_t i = 0; i < ln; ++i) {
            const x3::rhi::PointLight& l = roomL[i];
            wpr.roomLightPos[i][0] = l.pos[0]; wpr.roomLightPos[i][1] = l.pos[1];
            wpr.roomLightPos[i][2] = l.pos[2];
            wpr.roomLightRange[i]  = l.range;
            wpr.roomLightColor[i][0] = l.color[0]; wpr.roomLightColor[i][1] = l.color[1];
            wpr.roomLightColor[i][2] = l.color[2];
        }
    }
    return true;
}

bool applyWorldWater(x3::rhi::IRenderDevice& device, const WorldWaterInput& in,
                     WP* outParams) {
    WP wpr{};
    if (!buildWorldWaterParams(in, wpr)) return false;
    device.setWaterParams(wpr);
    if (outParams) *outParams = wpr;
    return true;
}



// ===========================================================================
// --test-canonunderriver
// ===========================================================================
namespace {

// Distance from point (px,pz) to segment (ax,az)-(bx,bz).
float segDist(float px, float pz, float ax, float az, float bx, float bz) {
    const float ex = bx - ax, ez = bz - az;
    const float L2 = std::max(ex*ex + ez*ez, 1e-6f);
    const float t = std::clamp(((px-ax)*ex + (pz-az)*ez) / L2, 0.0f, 1.0f);
    const float qx = ax + ex*t, qz = az + ez*t;
    return std::sqrt((px-qx)*(px-qx) + (pz-qz)*(pz-qz));
}

// Shortest distance between two segments (sampled along the first at 2 m —
// the chain segments are hundreds of metres; a closed form buys nothing here).
float segSegDist(float ax, float az, float bx, float bz,
                 float cx, float cz, float dx, float dz) {
    const float L = std::sqrt((bx-ax)*(bx-ax) + (bz-az)*(bz-az));
    const int n = std::max(1, (int)(L / 2.0f));
    float best = 1e30f;
    for (int i = 0; i <= n; ++i) {
        const float t = (float)i / (float)n;
        best = std::min(best, segDist(ax + (bx-ax)*t, az + (bz-az)*t, cx, cz, dx, dz));
    }
    return best;
}

// Clearance from the under-river BAND (chain spine grown by kURWallOutW — the
// carve's full lateral reach) to a point with its own radius / to a segment
// with its own half-width. Negative = overlap.
float bandToPoint(const UnderRiverChain& uc, float px, float pz, float r) {
    float best = 1e30f;
    for (int i = 0; i + 1 < uc.n; ++i)
        best = std::min(best, segDist(px, pz, uc.x[i], uc.z[i], uc.x[i+1], uc.z[i+1]));
    return best - kURWallOutW - r;
}
float bandToSeg(const UnderRiverChain& uc, float ax, float az, float bx, float bz, float halfW) {
    float best = 1e30f;
    for (int i = 0; i + 1 < uc.n; ++i)
        best = std::min(best, segSegDist(uc.x[i], uc.z[i], uc.x[i+1], uc.z[i+1], ax, az, bx, bz));
    return best - kURWallOutW - halfW;
}

// A HeadlessRenderDevice that keeps the set of live mesh ids so CU2 can prove
// every mesh the build made is owned by a captured scene entity.
struct MeshLedgerDevice : HeadlessRenderDevice {
    std::set<uint32_t> live;
    x3::rhi::MeshHandle createMesh(const x3::rhi::MeshVertex* v, uint32_t vc,
                                   const uint32_t* idx, uint32_t ic) override {
        const auto h = HeadlessRenderDevice::createMesh(v, vc, idx, ic);
        live.insert(h.id); return h;
    }
    void destroyMesh(x3::rhi::MeshHandle h) override {
        live.erase(h.id); HeadlessRenderDevice::destroyMesh(h);
    }
};

} // namespace

bool runCanonUnderRiverSelfTest() {
    int passN = 0, failN = 0;
    char d[400];
    auto check = [&](bool ok, const char* name, const char* detail = nullptr) {
        std::string m = std::string(ok ? "PASS " : "FAIL ") + name;
        if (detail && *detail) m += std::string(" -- ") + detail;
        if (ok) { ++passN; x3::logInfo("[canon-underriver] " + m); }
        else    { ++failN; x3::logError("[canon-underriver] " + m); }
    };

    // ---- THE CANON FIELD. The canon boot slot registers the city freeway and
    // stands up the interchange BEFORE the first height query; the survey
    // inputs are the ones --test-drivelayer D2/D5 use (the city region's
    // anchor + reach from regions.canon.json, the facility's measured facade
    // + 150 m soil skirt, the tower as the reach-from). Same order here so
    // every number below is measured on the field the player walks.
    clearTerrainCorridors();
    CityFreewaySurveyInput cin;
    cin.regionAnchorX = -200.0f; cin.regionAnchorZ = 425.0f;
    cin.regionReachM  = 1350.0f;
    cin.haveKeepOut = true;
    cin.keepOutX0 = -154.5f; cin.keepOutZ0 = -184.5f;
    cin.keepOutX1 =  197.0f; cin.keepOutZ1 =  205.5f;
    cin.haveReachFrom = true; cin.reachFromX = 22.0f; cin.reachFromZ = 10.5f;
    const CityFreewaySurvey fsv = surveyCityFreeway(cin);
    std::vector<float> fwyY;
    InterchangeResult ic;
    if (fsv.ok) {
        const RoadBuildResult rb = registerRoad(fsv.spec, &fwyY);
        if (rb.ok) ic = standUpInterchange("--test-canonunderriver", true, fsv.spec, fwyY, nullptr);
        std::snprintf(d, sizeof(d), "canon freeway '%s' (%.0f,%.0f)->(%.0f,%.0f) registered=%d; "
                      "interchange built=%d at (%.0f,%.0f)", fsv.alignmentName,
                      fsv.x0, fsv.z0, fsv.x1, fsv.z1, rb.ok ? 1 : 0, ic.built ? 1 : 0, ic.cx, ic.cz);
        x3::logInfo(std::string("[canon-underriver] field: ") + d);
    } else {
        x3::logInfo(std::string("[canon-underriver] field: NO canon freeway -- ") + fsv.whyNot);
    }

    const UnderRiverChain& uc = worldUnderRiverChain();

    // ---- CU1 the trench is ALREADY carved in the canon field --------------
    // terrain.cpp's authoredLandforms runs inside terrainHeightAt for EVERY
    // caller, and the canon host initialises its ring with worldTerrainConfig()
    // -- so there is nothing to wire: the same query that placed the tower
    // returns the bed here. Measured: pre-UR ground above the water by at least
    // the cover minimum along the vaulted reach, the bed under the water, and
    // worldWaterLevelAt answering the table on every node.
    {
        float minCover = 1e9f, minDepth = 1e9f, maxTableErr = 0.0f;
        int nodes = 0;
        for (int i = 0; i < uc.n; ++i) {
            const float bed = terrainHeightAtWorld(uc.x[i], uc.z[i]);
            const float pre = worldPreUnderRiverHeight(uc.x[i], uc.z[i]);
            const float lw  = worldWaterLevelAt(uc.x[i], uc.z[i]);
            minDepth = std::min(minDepth, uc.w[i] - bed);
            // the last kURGorgeLen is the open gorge / plunge pool -- no lid owed
            if (uc.cum[uc.n - 1] - uc.cum[i] > kURGorgeLen)
                minCover = std::min(minCover, pre - uc.w[i]);
            maxTableErr = std::max(maxTableErr, std::fabs(lw - uc.w[i]));
            ++nodes;
        }
        std::snprintf(d, sizeof(d), "%d nodes: bed under water by >= %.2f m, rock over the "
                      "water by >= %.2f m (min %.0f), worldWaterLevelAt vs table <= %.3f m",
                      nodes, minDepth, minCover, kURCoverMin, maxTableErr);
        check(nodes >= 8 && minDepth > 0.5f && minCover >= kURCoverMin - 0.01f && maxTableErr < 0.05f,
              "CU1 the trench is already carved in the canon height field (no boot wiring owed)", d);
    }

    // ---- CU2 the drawn cavern builds, fully captured, nothing orphaned ------
    MeshLedgerDevice dev;
    Scene scene;
    UndergroundRiver ur;
    UndergroundRiver::Result res;
    std::vector<uint32_t> ids;
    {
        const uint32_t before = scene.size();
        scene.beginEntityCapture(&ids);
        res = ur.build(scene, dev, nullptr, nullptr);
        scene.endEntityCapture();
        for (uint32_t id : ids) scene.get(id).roomId = kStreamedExteriorRoom;
        // Every mesh the build created must be referenced by a captured entity:
        // that is what makes "device shutdown frees all" a receipt, not a hope.
        std::set<uint32_t> owned;
        for (uint32_t id : ids) if (scene.get(id).mesh.valid()) owned.insert(scene.get(id).mesh.id);
        uint32_t orphans = 0;
        for (uint32_t m : dev.live) if (!owned.count(m)) ++orphans;
        const bool allCaptured = (scene.size() - before) == (uint32_t)ids.size();
        std::snprintf(d, sizeof(d), "built=%d vault=%d beaches=%d waterSegs=%d lights=%d mist=%d; "
                      "%u entities (%s captured), %u meshes, %u orphan",
                      res.built ? 1 : 0, res.vaultChunks, res.beachChunks, res.waterSegs,
                      res.lightCount, res.mistSources, (uint32_t)ids.size(),
                      allCaptured ? "all" : "NOT ALL", (uint32_t)dev.live.size(), orphans);
        check(res.built && res.vaultChunks > 0 && res.beachChunks > 0 && res.waterSegs > 0 &&
              res.lightCount > 0 && res.mistSources > 0 && !ids.empty() && allCaptured &&
              orphans == 0,
              "CU2 the vault, beaches, water, lights and mist build into the canon scene, "
              "every entity captured for the room stamp, no orphan mesh", d);
        // A tick and a nearest-lights query from inside the corridor must
        // return lights (the host merges them into its ONE setPointLights).
        const float hall[3] = { uc.x[uc.n / 2], uc.w[uc.n / 2] + 1.7f, uc.z[uc.n / 2] };
        ur.update(1.0f / 60.0f, scene);
        x3::rhi::PointLight ul[12];
        const uint32_t un = ur.nearestLights(hall, ul, 12);
        std::snprintf(d, sizeof(d), "%u lights within reach of (%.0f, %.0f)", un, hall[0], hall[2]);
        check(UndergroundRiver::insideCorridor(hall) && un > 0,
              "CU2 the cavern hands the host bank lights from inside the corridor", d);
    }

    // ---- CU3 headroom on THIS field (U9's own measure) ---------------------
    {
        const UndergroundRiver::Headroom h = UndergroundRiver::measureHeadroom();
        std::snprintf(d, sizeof(d), "%d probes over %.0f m; headroom %.2f..%.2f m (tightest at "
                      "(%.0f, %.0f)); over the beaches >= %.2f m", h.probes, h.vaultLen,
                      h.minHead, h.maxHead, h.atX, h.atZ, h.minBeachHead);
        check(h.minHead > 0.05f && h.minBeachHead >= 2.5f && h.probes > 500,
              "CU3 there is a cavern in the canon field you can stand up in", d);
    }

    // ---- CU4 / CU5 ONE WATER switches channel with the focus, clarity ON ---
    {
        using WP = x3::rhi::IRenderDevice::WaterParams;
        uint32_t nSurf = 0; const WorldRiverNode* srn = worldRiverNodes(nSurf);
        WorldWaterInput in;
        in.time = 3.0f; in.sunDir[0] = 0.55f; in.sunDir[1] = 0.16f; in.sunDir[2] = -0.35f;
        // Outside: a surface river reach.
        WP a{}, b{}, c{};
        bool inA = true, inB = false, inC = true;
        in.focusX = srn[nSurf / 2].x; in.focusZ = srn[nSurf / 2].z;
        const bool okA = buildWorldWaterParams(in, a, &inA);
        // Inside: the Great Hall pool (the chain's middle node).
        in.focusX = uc.x[uc.n / 2]; in.focusZ = uc.z[uc.n / 2];
        const bool okB = buildWorldWaterParams(in, b, &inB);
        // And back out -- byte-identical to the first answer (no state carried).
        in.focusX = srn[nSurf / 2].x; in.focusZ = srn[nSurf / 2].z;
        const bool okC = buildWorldWaterParams(in, c, &inC);
        const bool sameAC = std::memcmp(&a, &c, sizeof(WP)) == 0;
        const char* shoreEnv = std::getenv("X3_WATER_SHORE");
        const bool shoreOn = !(shoreEnv && shoreEnv[0] == '0');
        const bool surfOk = okA && !inA && a.enabled && a.riverNodeCount >= 2 &&
            a.riverNodeCount <= WP::kMaxRiverNodes && a.riverHalfWidth == kWorldRiverHalfWidth &&
            a.basinRadius == kWorldOceanBasinR && a.oceanLevel == kWorldSeaLevel &&
            a.enclosed == 0.0f && (!shoreOn || a.shoreSectorCount == WP::kShoreSectors) &&
            std::fabs(a.seaLevel - srn[nSurf / 2].waterY) < 1.0f;
        const bool caveOk = okB && inB && b.enabled && b.riverNodeCount == (uint32_t)uc.n &&
            b.riverHalfWidth == kURHalfWidth && b.basinRadius == 0.0f && b.enclosed == 1.0f &&
            b.shoreSectorCount == 0 && std::fabs(b.seaLevel - uc.w[uc.n / 2]) < 1.0f;
        std::snprintf(d, sizeof(d), "surface: %u nodes hw=%.0f basinR=%.0f shore=%u seaLevel=%.2f | "
                      "cavern: %u nodes hw=%.0f basinR=%.0f enclosed=%.0f seaLevel=%.2f | back out %s",
                      a.riverNodeCount, a.riverHalfWidth, a.basinRadius, a.shoreSectorCount, a.seaLevel,
                      b.riverNodeCount, b.riverHalfWidth, b.basinRadius, b.enclosed, b.seaLevel,
                      sameAC ? "identical" : "DIFFERS");
        check(surfOk && caveOk && okC && !inC && sameAC,
              "CU4 ONE WATER follows the focus: surface polyline outside, cavern polyline inside, "
              "and back", d);
        // A host with no surface river and a focus outside the corridor gets nothing.
        WorldWaterInput none = in; none.surfaceOn = false;
        WP n{}; n.clarity = 0.5f;
        const bool okN = buildWorldWaterParams(none, n);
        check(!okN && n.clarity == 0.5f,
              "CU4 no surface river + focus outside the corridor = nothing drawn, params untouched");
        // Clarity: ON, at the approved values, on both channels.
        const char* ovEnv = std::getenv("X3_WATER_CLARITY");
        std::snprintf(d, sizeof(d), "surface clarity=%.2f (approved %.2f) cavern clarity=%.2f (approved %.2f)%s",
                      a.clarity, kWorldWaterClaritySurface, b.clarity, kWorldWaterClarityCavern,
                      ovEnv ? " -- X3_WATER_CLARITY override is SET; this gate expects it unset" : "");
        check(a.clarity > 0.0f && b.clarity > 0.0f &&
              std::fabs(a.clarity - kWorldWaterClaritySurface) < 1e-6f &&
              std::fabs(b.clarity - kWorldWaterClarityCavern)  < 1e-6f &&
              b.clarity > a.clarity,
              "CU5 clarity is ON on every canon water set at the approved values", d);
    }

    // ---- CU6 corridor clearance --------------------------------------------
    {
        // (a) NOTHING registered touches the band: the tunnel host's conflict
        // probe, promoted to a gate. Whole band, both signs.
        float worst = 0.0f, wx = 0, wz = 0;
        for (int i = 0; i + 1 < uc.n; ++i) {
            const float sx = uc.x[i], sz = uc.z[i], ex = uc.x[i+1], ez = uc.z[i+1];
            const float len = std::sqrt((ex-sx)*(ex-sx) + (ez-sz)*(ez-sz));
            const float ux = (ex-sx)/len, uz = (ez-sz)/len, px = -uz, pz = ux;
            for (float t = 0.0f; t <= len; t += 10.0f)
                for (int k = -4; k <= 4; ++k) {
                    const float lat = (float)k * (kURWallOutW / 4.0f);
                    const float qx = sx + ux*t + px*lat, qz = sz + uz*t + pz*lat;
                    const float dc = terrainCorridorDelta(qx, qz);
                    if (std::fabs(dc) > std::fabs(worst)) { worst = dc; wx = qx; wz = qz; }
                }
        }
        std::snprintf(d, sizeof(d), "max |corridor delta| over the band = %.3f m at (%.0f, %.0f)",
                      std::fabs(worst), wx, wz);
        check(std::fabs(worst) < 0.05f,
              "CU6 no registered road corridor touches the under-river band", d);

        // (b) measured clearances, band edge to feature edge. Logged one per
        // line so the report can quote them; the gate is "all positive".
        float minClear = 1e30f; std::string tightest;
        auto note = [&](const std::string& what, float clear) {
            std::snprintf(d, sizeof(d), "  clearance %-46s %8.1f m", what.c_str(), clear);
            x3::logInfo(std::string("[canon-underriver]") + d);
            if (clear < minClear) { minClear = clear; tightest = what; }
        };
        if (fsv.ok) {
            const RoadSpec& fs = fsv.spec;
            const float fw = fs.halfWidth + fs.falloff;      // the whole carve, not the pavement
            float best = 1e30f;
            for (size_t i = 0; i + 1 < fs.x.size(); ++i)
                best = std::min(best, bandToSeg(uc, fs.x[i], fs.z[i], fs.x[i+1], fs.z[i+1], fw));
            note(std::string("canon freeway '") + fsv.alignmentName + "' (carve edge)", best);
        }
        if (ic.built) {
            const RoadSpec& cs = ic.spec;
            const float cw = cs.halfWidth + cs.falloff;
            float best = 1e30f;
            for (size_t i = 0; i + 1 < cs.x.size(); ++i)
                best = std::min(best, bandToSeg(uc, cs.x[i], cs.z[i], cs.x[i+1], cs.z[i+1], cw));
            note("interchange crossroad (carve edge)", best);
        }
        {
            const DealershipSite& ds = kDealershipSite;
            const float r = std::sqrt(ds.halfDepth*ds.halfDepth + ds.halfWidth*ds.halfWidth) + ds.forecourtDepth;
            note("dealership (hall + forecourt)", bandToPoint(uc, ds.cx, ds.cz, r));
            note("dealership fronted road", bandToSeg(uc, ds.roadX, ds.roadZ0, ds.roadX, ds.roadZ1, ds.roadHalfW));
        }
        for (uint32_t i = 0; i < cityDistrictCount(); ++i) {
            const CityDistrictFootprint& f = cityDistrictFootprint(i);
            note(std::string("district '") + f.name + "' (flat pad)", bandToPoint(uc, f.cx, f.cz, f.radius));
        }
        for (uint32_t i = 0; i < cityConnectorCount(); ++i) {
            const CityRoadAlignment& al = cityConnector(i);
            note(std::string("connector '") + al.name + "'", bandToSeg(uc, al.x0, al.z0, al.x1, al.z1, al.halfW));
        }
        for (uint32_t i = 0; i < kFreewayTunnelCount; ++i) {
            const FreewayTunnelPlan& t = cityFreewayTunnelPlan(i);
            note(std::string("freeway tunnel plan '") + t.name + "'",
                 bandToSeg(uc, t.mouthX, t.mouthZ, t.mouthX + t.dirX*t.length, t.mouthZ + t.dirZ*t.length, 12.0f));
        }
        note("the tower (22, 10) r 70", bandToPoint(uc, 22.0f, 10.0f, 70.0f));
        std::snprintf(d, sizeof(d), "tightest: %s at %.1f m", tightest.c_str(), minClear);
        check(minClear > 0.0f, "CU6 the under-river band clears every canon feature", d);
    }

    clearTerrainCorridors();
    std::snprintf(d, sizeof(d), "[canon-underriver] %d passed, %d failed", passN, failN);
    if (failN) x3::logError(d); else x3::logInfo(d);
    return failN == 0;
}

} // namespace x3::game
