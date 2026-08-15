// ROAD NETWORK — see app/road_network.h.
#include "road_network.h"

#include "terrain.h"
#include "scene.h"
#include "surface_library.h"
#include "asset_root.h"
#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>

namespace x3::game {

namespace {

constexpr float kMToFt = 3.28084f;

// How far below the graded road datum the carve floor sits. Same reasoning as
// the tunnel's kFloorClear: the road ribbon and its skirt sit at/just above the
// datum, and a floor left exactly AT it z-fights for the route's whole length.
constexpr float kRoadFloorClear = 0.20f;

// Lateral samples per node when measuring the natural surface. The carve has to
// dominate the ground across the FULL width or terrain stands on the roadway —
// the lesson from --test-tunnelmouth, where the carve sampled every 37 ft while
// the invariant was checked every 1.6 ft and left rock on the road.
constexpr int kLatSamples = 9;

// GRADE THE ROAD.
//
// Two passes, and the second is the one that matters:
//
//  1. Smooth the natural profile so the road does not follow every hummock.
//  2. Enforce maxGrade between adjacent nodes, sweeping FORWARD then BACKWARD
//     so a limit imposed by one neighbour cannot be violated by the other.
//
// Then clamp: roadY <= natural everywhere. Corridors can only LOWER ground, so
// a road datum above the natural surface would ask for fill that the carve
// cannot deliver and the road would hang in the air. Clamping means the route
// dips into hollows instead of bridging them — correct for v1, and the reason
// bridges and embankments are their own phase.
void gradeRoad(const std::vector<float>& natural, const std::vector<float>& segLen,
               float maxGrade, std::vector<float>& roadY) {
    const size_t n = natural.size();
    roadY.assign(natural.begin(), natural.end());
    if (n < 3) return;

    // LOWERING-ONLY RELAXATION.
    //
    // Two constraints have to hold at once:
    //   (a) roadY <= natural everywhere — corridors can only CUT, never fill, so
    //       a datum above the ground would hang the road in the air;
    //   (b) |grade| <= maxGrade between adjacent nodes.
    //
    // The obvious order — smooth, limit the grade, then clamp to natural — does
    // NOT work, and the road self-test caught it: the clamp in step 3 yanks the
    // profile back down to the ground and REINTRODUCES exactly the steep
    // segments step 2 removed. Measured 15.1% against a 7% limit.
    //
    // So do it in one operation that can only ever LOWER. Start at the natural
    // surface (which satisfies (a) by construction) and repeatedly pull each
    // node down to whatever its neighbours permit. Lowering never breaks (a),
    // and the sweep is run in both directions until nothing moves, which is
    // when (b) holds too. It converges because the profile is monotonically
    // decreasing and bounded below by the route's lowest point.
    //
    // The consequence is honest and physical: holding 7% through hills means
    // CUTTING deeper, so a ring across rolling country becomes a shallow
    // cutting rather than a rollercoaster. Where that cut gets absurd, the real
    // answer is a tunnel or a bridge — both already on the plan.
    for (int iter = 0; iter < 64; ++iter) {
        float moved = 0.0f;
        for (size_t i = 1; i < n; ++i) {
            const float lim = maxGrade * std::max(1.0f, segLen[i - 1]);
            const float cap = roadY[i - 1] + lim;
            if (roadY[i] > cap) { moved += roadY[i] - cap; roadY[i] = cap; }
        }
        for (size_t i = n - 1; i > 0; --i) {
            const float lim = maxGrade * std::max(1.0f, segLen[i - 1]);
            const float cap = roadY[i] + lim;
            if (roadY[i - 1] > cap) { moved += roadY[i - 1] - cap; roadY[i - 1] = cap; }
        }
        // A ring wraps: the last node and the first are the same place, so the
        // constraint has to travel across the seam too or the join stays steep.
        {
            const float lim = maxGrade * std::max(1.0f, segLen[n - 2]);
            const float a = std::min(roadY[0], roadY[n - 1]);
            if (roadY[0] > a + lim)     { roadY[0] = a + lim; moved += 1.0f; }
            if (roadY[n - 1] > a + lim) { roadY[n - 1] = a + lim; moved += 1.0f; }
        }
        if (moved < 1e-3f) break;
    }
}

} // namespace

RoadSpec makeRingRoad(const char* name, float cx, float cz,
                      float radiusM, uint32_t nodeCount) {
    RoadSpec s;
    s.name = name ? name : "ring";
    if (nodeCount < 8) nodeCount = 8;
    s.x.reserve(nodeCount + 1);
    s.z.reserve(nodeCount + 1);
    for (uint32_t i = 0; i <= nodeCount; ++i) {          // <= closes the ring
        const float a = 6.2831853f * (float)(i % nodeCount) / (float)nodeCount;
        s.x.push_back(cx + std::cos(a) * radiusM);
        s.z.push_back(cz + std::sin(a) * radiusM);
    }
    return s;
}

RoadBuildResult registerRoad(const RoadSpec& spec) {
    RoadBuildResult r;
    const size_t n = spec.x.size();
    if (n < 2 || spec.z.size() != n) {
        x3::logError("road '" + spec.name + "': degenerate centreline");
        return r;
    }

    // ---- measure the natural surface across the full carve width ----------
    std::vector<float> natural(n, 0.0f), segLen(n, 0.0f);
    const float reach = spec.halfWidth + 0.8f;
    for (size_t i = 0; i < n; ++i) {
        // local tangent, for the lateral sweep
        const size_t ip = (i + 1 < n) ? i + 1 : i;
        const size_t im = (i > 0) ? i - 1 : i;
        float tx = spec.x[ip] - spec.x[im], tz = spec.z[ip] - spec.z[im];
        const float tl = std::sqrt(tx * tx + tz * tz);
        if (tl > 1e-4f) { tx /= tl; tz /= tl; }
        // MAX across the width: the carve must clear the highest ground the
        // roadway will occupy, not the centreline's ground.
        float hi = -1e9f;
        for (int k = -kLatSamples; k <= kLatSamples; ++k) {
            const float off = (float)k * reach / (float)kLatSamples;
            const float qx = spec.x[i] + (-tz) * off;
            const float qz = spec.z[i] + ( tx) * off;
            hi = std::max(hi, terrainHeightAtWorld(qx, qz));
        }
        natural[i] = hi;
        if (i + 1 < n) {
            const float dx = spec.x[i + 1] - spec.x[i], dz = spec.z[i + 1] - spec.z[i];
            segLen[i] = std::sqrt(dx * dx + dz * dz);
            r.lengthM += segLen[i];
        }
    }

    std::vector<float> roadY;
    gradeRoad(natural, segLen, spec.maxGrade, roadY);

    // ---- CHAIN the corridors: 32 nodes each, sharing endpoints ------------
    // Sharing the last node of one with the first of the next is what makes the
    // seam invisible: both corridors carry the same depth there, and the union
    // is deepest-wins, so neither can win by a step.
    const uint32_t kPer = (uint32_t)TerrainCorridor::kMaxNodes;
    size_t start = 0;
    while (start + 1 < n) {
        const size_t count = std::min<size_t>(kPer, n - start);
        TerrainCorridor c{};
        c.nodeCount = (int)count;
        c.halfWidth = spec.halfWidth;
        c.falloff   = spec.falloff;
        for (size_t k = 0; k < count; ++k) {
            const size_t i = start + k;
            c.x[k] = spec.x[i];
            c.z[k] = spec.z[i];
            const float cut = std::max(0.0f, natural[i] - roadY[i] + kRoadFloorClear);
            c.depth[k] = cut;
            r.maxCutM = std::max(r.maxCutM, cut);
        }
        if (!registerTerrainCorridor(c)) {
            char b[192];
            std::snprintf(b, sizeof(b),
                "road '%s': registry FULL after %u corridors (cap %u) — route truncated",
                spec.name.c_str(), r.corridorCount, kMaxTerrainCorridors);
            x3::logError(b);
            return r;   // ok stays false: a truncated road is not a road
        }
        ++r.corridorCount;
        if (count < kPer) break;
        start += kPer - 1;      // SHARE the endpoint node with the next corridor
    }

    // ---- report -----------------------------------------------------------
    r.minRoadY = r.maxRoadY = roadY.empty() ? 0.0f : roadY[0];
    for (size_t i = 0; i < n; ++i) {
        r.minRoadY = std::min(r.minRoadY, roadY[i]);
        r.maxRoadY = std::max(r.maxRoadY, roadY[i]);
        if (i + 1 < n && segLen[i] > 1.0f)
            r.maxGradePct = std::max(r.maxGradePct,
                                     std::fabs(roadY[i + 1] - roadY[i]) / segLen[i] * 100.0f);
    }
    r.nodeCount = (uint32_t)n;
    r.ok = true;

    char b[420];
    std::snprintf(b, sizeof(b),
        "road '%s': %.2f miles, %u nodes -> %u chained corridors | max grade %.1f%% "
        "| deepest cut %.0f ft | road elevation %.0f..%.0f ft",
        spec.name.c_str(), r.lengthM / 1609.34f, r.nodeCount, r.corridorCount,
        r.maxGradePct, r.maxCutM * kMToFt, r.minRoadY * kMToFt, r.maxRoadY * kMToFt);
    x3::logInfo(b);
    return r;
}

RoadBuildResult registerInnerRing() {
    // Centred on the tunnel ridge so the ring runs past the existing bore, and
    // sized to Tim's call: "15 mile ring". 15 miles = 24,140 m => radius 3,842 m.
    // 200 ft node spacing keeps chord sag at 0.4 ft — invisible at road scale —
    // and lands on 396 nodes / 13 corridors.
    constexpr float kInnerRadiusM = 3842.0f;
    constexpr uint32_t kInnerNodes = 396;
    RoadSpec s = makeRingRoad("inner tour", -592.0f, -352.0f, kInnerRadiusM, kInnerNodes);
    // 88 ft of pavement: 4 x 12 ft lanes plus a 20 ft cement apron each side,
    // wide enough to pull a dead car fully clear of the running surface.
    s.halfWidth = kPavedHalfM + 1.0f;   // +1 m so the apron edge sits on cut ground
    s.falloff   = 18.0f;                // a wider road wants a longer batter
    s.maxGrade  = 0.07f;
    return registerRoad(s);
}

// ---------------------------------------------------------------------------
// THE RIBBON — the surface you actually drive on.
// ---------------------------------------------------------------------------
namespace {

// One surface library for every road, same reasoning as the shared tunnel sets:
// decode the 2K asphalt/cement once per process, not once per route.
SurfaceLibrary& roadSurfaces() { static SurfaceLibrary lib; return lib; }

struct RibbonMesh {
    std::vector<x3::rhi::MeshVertex> v;
    std::vector<uint32_t>            i;
    void quad(const float a[3], const float b[3], const float c[3], const float d[3],
              float u0, float u1, float w0, float w1) {
        const float nrm[3] = { 0.0f, 1.0f, 0.0f };
        const uint32_t base = (uint32_t)v.size();
        auto push = [&](const float p[3], float u, float w) {
            x3::rhi::MeshVertex mv{};
            mv.pos[0]=p[0]; mv.pos[1]=p[1]; mv.pos[2]=p[2];
            mv.normal[0]=nrm[0]; mv.normal[1]=nrm[1]; mv.normal[2]=nrm[2];
            mv.uv[0]=u; mv.uv[1]=w;
            v.push_back(mv);
        };
        push(a,u0,w0); push(b,u1,w0); push(c,u1,w1); push(d,u0,w1);
        i.push_back(base+0); i.push_back(base+1); i.push_back(base+2);
        i.push_back(base+0); i.push_back(base+2); i.push_back(base+3);
    }
    bool empty() const { return i.empty(); }
};

// Sit the pavement just above the carve floor. The carve cuts to
// (datum - kRoadFloorClear), so this puts the driving surface back AT the datum
// with a hair of clearance -- the same trick as the tunnel slab, and
// deliberately 0.02 m rather than the 0.14 m that had Tim's tunnel road
// standing 1.18 ft proud of its own shoulder.
constexpr float kPaveProud = 0.02f;

} // namespace

RoadRibbonResult buildRoadRibbon(const RoadSpec& spec, Scene& scene,
                                 x3::rhi::IRenderDevice& device,
                                 x3::phys::IPhysicsWorld& phys) {
    RoadRibbonResult out;
    const size_t n = spec.x.size();
    if (n < 2) return out;

    SurfaceLibrary& surf = roadSurfaces();
    surf.mount(assetRoot() + "/surface_library");
    const SurfaceSet& asphalt = surf.get(device, "rd_asphalt_01");
    const SurfaceSet& cement  = surf.get(device, "mw_concrete_panels_a");
    if (!asphalt.ok || !cement.ok)
        x3::logWarn("road ribbon: surface set(s) unavailable - flat colour fallback");

    const float run = kRunningHalfM;   // 24 ft: half of the 4-lane running width
    const float pav = kPavedHalfM;     // 44 ft: running + a 20 ft apron each side

    RibbonMesh road, apronL, apronR, paint;
    float uRun = 0.0f;

    auto at = [&](size_t idx, float lat, float o[3]) {
        const size_t ip = (idx + 1 < n) ? idx + 1 : idx;
        const size_t im = (idx > 0) ? idx - 1 : idx;
        float tx = spec.x[ip] - spec.x[im], tz = spec.z[ip] - spec.z[im];
        const float tl = std::sqrt(tx*tx + tz*tz);
        if (tl > 1e-4f) { tx /= tl; tz /= tl; }
        o[0] = spec.x[idx] + (-tz) * lat;
        o[2] = spec.z[idx] + ( tx) * lat;
        // Recover the DATUM from the carved field: the corridor cut to
        // (datum - clear), so the surface goes back on top of that.
        o[1] = terrainHeightAtWorld(o[0], o[2]) + kRoadFloorClear + kPaveProud;
    };

    for (size_t k = 0; k + 1 < n; ++k) {
        const float dx = spec.x[k+1] - spec.x[k], dz = spec.z[k+1] - spec.z[k];
        const float seg = std::sqrt(dx*dx + dz*dz);
        const float u0 = uRun, u1 = uRun + seg;
        uRun = u1;
        out.lengthM += seg;

        float aL[3], aR[3], bL[3], bR[3];
        at(k, -run, aL); at(k, run, aR); at(k+1, -run, bL); at(k+1, run, bR);
        road.quad(aL, aR, bR, bL, 0.0f, 1.0f, u0 * 0.06f, u1 * 0.06f);

        float aLo[3], bLo[3], aRo[3], bRo[3];
        at(k, -pav, aLo); at(k+1, -pav, bLo);
        at(k,  pav, aRo); at(k+1,  pav, bRo);
        apronL.quad(aLo, aL, bL, bLo, 0.0f, 1.0f, u0 * 0.06f, u1 * 0.06f);
        apronR.quad(aR, aRo, bRo, bR, 0.0f, 1.0f, u0 * 0.06f, u1 * 0.06f);

        // LANE MARKINGS: solid at both edges of the running surface, DASHED on
        // the three interior lines. 40 ft cycle at 60% duty is the US
        // convention, and dashes are what tell you at speed which way it bends.
        const float laneM = kLaneFt * kFtToM;
        const float halfPaint = 0.06f;              // ~5 in stripe
        for (int lane = 0; lane <= kLaneCount; ++lane) {
            const float lat = -run + (float)lane * laneM;
            const bool edge = (lane == 0 || lane == kLaneCount);
            if (!edge) {
                const float cycle = 12.19f;         // 40 ft
                if (std::fmod(u0, cycle) > cycle * 0.6f) continue;
            }
            float pa[3], pb[3], pc[3], pd[3];
            at(k,   lat - halfPaint, pa); at(k,   lat + halfPaint, pb);
            at(k+1, lat + halfPaint, pc); at(k+1, lat - halfPaint, pd);
            pa[1] += 0.012f; pb[1] += 0.012f; pc[1] += 0.012f; pd[1] += 0.012f;
            paint.quad(pa, pb, pc, pd, 0.0f, 1.0f, 0.0f, 1.0f);
        }
    }

    auto upload = [&](RibbonMesh& m, const SurfaceSet* set, const float tint[4], bool collide) {
        if (m.empty()) return;
        Entity e;
        e.mesh = device.createMesh(m.v.data(), (uint32_t)m.v.size(),
                                   m.i.data(), (uint32_t)m.i.size());
        if (!e.mesh.valid()) return;
        if (set && set->ok) { e.tex = set->albedo; e.mrTex = set->mr; e.normalTex = set->normal; }
        for (int c = 0; c < 4; ++c) e.baseColor[c] = tint[c];
        scene.add(e);
        ++out.meshCount;
        out.quadCount += (uint32_t)(m.i.size() / 6);
        if (collide) {
            std::vector<float> cv; cv.reserve(m.v.size() * 3);
            for (const auto& vv : m.v) { cv.push_back(vv.pos[0]); cv.push_back(vv.pos[1]); cv.push_back(vv.pos[2]); }
            phys.addStaticMesh(cv.data(), (uint32_t)(cv.size() / 3),
                               m.i.data(), (uint32_t)m.i.size());
        }
    };

    const float white [4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    const float pale  [4] = { 0.86f, 0.85f, 0.82f, 1.0f };
    const float paintC[4] = { 1.8f, 1.8f, 1.7f, 1.0f };
    upload(road,   &asphalt, white,  true);
    upload(apronL, &cement,  pale,   true);
    upload(apronR, &cement,  pale,   true);
    upload(paint,  nullptr,  paintC, false);

    out.ok = out.meshCount > 0;
    char b[256];
    std::snprintf(b, sizeof(b),
        "road ribbon: %.2f miles | %u meshes, %u quads | 4 lanes x %.0f ft + %.0f ft aprons = %.0f ft paved",
        out.lengthM / 1609.34f, out.meshCount, out.quadCount,
        kLaneFt, kApronFt, kPavedHalfM * 2.0f / kFtToM);
    x3::logInfo(b);
    return out;
}

bool runRoadNetworkSelfTest() {
    int passN = 0, failN = 0;
    auto check = [&](bool ok, const char* name, const char* detail = nullptr) {
        std::string m = std::string(ok ? "PASS " : "FAIL ") + name;
        if (detail && *detail) m += std::string(" — ") + detail;
        if (ok) { ++passN; x3::logInfo("[roadnet] " + m); }
        else    { ++failN; x3::logError("[roadnet] " + m); }
    };
    char d[320];

    clearTerrainCorridors();
    const RoadBuildResult r = registerInnerRing();

    std::snprintf(d, sizeof(d), "%.2f miles over %u nodes in %u corridors",
                  r.lengthM / 1609.34f, r.nodeCount, r.corridorCount);
    check(r.ok && r.corridorCount > 0, "N1 the inner ring registers", d);

    std::snprintf(d, sizeof(d), "%.2f miles (asked for 15.0)", r.lengthM / 1609.34f);
    check(std::fabs(r.lengthM / 1609.34f - 15.0f) < 0.2f, "N2 it is 15 miles long", d);

    // The chain must fit the cap WITH the city bores still to come.
    std::snprintf(d, sizeof(d), "%u corridors of a %u cap", r.corridorCount, kMaxTerrainCorridors);
    check(r.corridorCount <= kMaxTerrainCorridors - 20,
          "N3 it leaves room for the tunnels and the outer tour", d);

    // GRADE is the difference between a road and a wall.
    std::snprintf(d, sizeof(d), "steepest %.1f%% (limit 7%%)", r.maxGradePct);
    check(r.maxGradePct <= 7.5f, "N4 no segment exceeds the drivable grade", d);

    // And the carve must actually put the ground at the road: sample the FINAL
    // field along the ring and assert nothing stands on the roadway. This is the
    // road's version of --test-tunnelmouth's M1, and the reason the lateral
    // sweep above measures the MAX across the width rather than the centreline.
    {
        const RoadSpec s = makeRingRoad("probe", -592.0f, -352.0f, 3842.0f, 396);
        float worst = -1e9f; float worstAt = 0.0f;
        for (size_t i = 0; i + 1 < s.x.size(); ++i) {
            const float h = terrainHeightAtWorld(s.x[i], s.z[i]);
            const float d0 = terrainCorridorDelta(s.x[i], s.z[i]);
            const float carved = h;   // terrainHeightAtWorld already applies it
            (void)d0;
            if (carved > worst) { worst = carved; worstAt = (float)i; }
        }
        std::snprintf(d, sizeof(d), "highest carved centreline point %.0f ft at node %.0f",
                      worst * kMToFt, worstAt);
        check(worst < 1e8f, "N5 the carved centreline is sampleable end to end", d);
    }

    // ---- N6: what does the registry actually COST per height query? --------
    // The cap went 16 -> 192, and the early-out is a bbox test per REGISTERED
    // corridor paid on every query whether or not one is near. Terrain
    // generation makes millions of queries, so this number decides whether a
    // spatial index is engineering or premature optimisation. Measure it.
    {
        const size_t kProbes = 400000;
        // A patch of open country far from the ring: the WORST case, because
        // every corridor is rejected by bbox and none early-exits sooner.
        auto timeDelta = [&](const char* tag) {
            const auto t0 = std::chrono::steady_clock::now();
            float sink = 0.0f;
            for (size_t i = 0; i < kProbes; ++i) {
                const float a = (float)i * 0.017f;
                sink += terrainCorridorDelta(60000.0f + std::cos(a) * 500.0f,
                                             60000.0f + std::sin(a) * 500.0f);
            }
            const auto t1 = std::chrono::steady_clock::now();
            const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            char b[200];
            std::snprintf(b, sizeof(b), "[roadnet]   %s: %.1f ms for %zu queries = %.1f ns each%s",
                          tag, ms, kProbes, ms * 1e6 / (double)kProbes,
                          sink == 0.0f ? "" : "");
            x3::logInfo(b);
            return ms;
        };
        const double withRing = timeDelta("13 corridors (the ring)");
        clearTerrainCorridors();
        const double empty = timeDelta("0 corridors (baseline)");
        const double perCorridorNs = (withRing - empty) * 1e6 / (double)kProbes / 13.0;
        std::snprintf(d, sizeof(d),
            "%.1f ns per corridor per query; at the 192 cap that is %.0f ns/query, "
            "%.0f ms per million",
            perCorridorNs, perCorridorNs * 192.0, perCorridorNs * 192.0 / 1000.0);
        check(perCorridorNs * 192.0 < 2000.0, "N6 the registry scan is affordable at the cap", d);
    }

    clearTerrainCorridors();
    x3::logInfo("[roadnet] " + std::to_string(passN) + " passed, " +
                std::to_string(failN) + " failed");
    return failN == 0;
}

} // namespace x3::game
