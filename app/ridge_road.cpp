// ridge_road.cpp — see ridge_road.h for the design and for Tim's brief.
#include "ridge_road.h"

#include "terrain.h"
#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace x3::game {
namespace {

constexpr float kFtPerM  = 1.0f / 0.3048f;
constexpr float kMPerMile = 1609.344f;

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// The NATURAL surface — corridor carves subtracted. A ridge is a property of
// the mountain, not of whatever has been dug into it. (Same recovery
// road_network's summit spur and roadnet gate O6b both use; sampling the raw
// field here would let the road follow its OWN excavation.)
inline float naturalAt(float x, float z) {
    return terrainHeightAtWorld(x, z) - terrainCorridorDelta(x, z);
}

float segPointDist2(float px, float pz, float ax, float az, float bx, float bz) {
    const float ex = bx - ax, ez = bz - az;
    const float l2 = ex * ex + ez * ez;
    float t = (l2 > 1e-9f) ? ((px - ax) * ex + (pz - az) * ez) / l2 : 0.0f;
    t = clampf(t, 0.0f, 1.0f);
    const float qx = ax + ex * t, qz = az + ez * t;
    return (px - qx) * (px - qx) + (pz - qz) * (pz - qz);
}

// 8-way gradient ascent on the natural field. Used to find the massif the demo
// bore passes under, rather than hardcoding the coordinates the tunnelmouth
// gate happens to print — a constant copied out of a log goes stale silently
// the first time the terrain seed moves.
void climbToPeak(float& x, float& z, int iters = 400, float step = 18.0f) {
    float h = naturalAt(x, z);
    for (int it = 0; it < iters; ++it) {
        float bx = x, bz = z, bh = h;
        for (int k = 0; k < 8; ++k) {
            const float a = (float)k * 0.7853982f;
            const float qx = x + std::cos(a) * step;
            const float qz = z + std::sin(a) * step;
            const float qh = naturalAt(qx, qz);
            if (qh > bh) { bh = qh; bx = qx; bz = qz; }
        }
        if (bh <= h + 1e-4f) break;
        x = bx; z = bz; h = bh;
    }
}

} // namespace

RidgeRoadResult registerRidgeRoad(const SummitLotResult& lot,
                                  const TunnelRoute& bore,
                                  const std::vector<const RoadSpec*>* avoid) {
    RidgeRoadResult out;
    if (!lot.built)      { out.whyNot = "no summit lot to start from";  return out; }
    if (bore.st.empty()) { out.whyNot = "no bore to head for";          return out; }

    // ---- THE TWO ENDS ------------------------------------------------------
    // START: the lot's entry mouth. The road leaves the pad the way a car
    // leaves it, so the two meet at a surface rather than at a coordinate.
    const float ax = lot.mouthX, az = lot.mouthZ;

    // END: the bore's PORTAL SHOULDER, not the summit above it.
    //
    // The first cut aimed at the massif's high flank — climb from the bore's
    // mid-span to the peak, step back 180 m — and --test-ridgeroad answered
    // with a 723 ft cut. The reason is worth keeping written down: the lot
    // stands at 350 ft, the massif's flank at ~800 ft, and the ground BETWEEN
    // them is lowland (the straight line's mean is 117 ft). A road pinned to
    // both of those ends has to sit somewhere in the middle of a profile that
    // dives 300 ft and climbs 800, and wherever the ground then stands above
    // that datum, registerRoad digs. The "ridge road" was boring through the
    // very mountain it was named after.
    //
    // The end this road actually wants is where the LOOP wants it: down at the
    // bore's portal, at the tunnel's own road level (~55 ft), on the shoulder
    // beside the spine where the garage tie-in comes out. Then the profile is
    // one honest climb from the portal up to the lot, and the "on top of the
    // mountains" character is the router's job in between — which is exactly
    // what G4 measures. climbToPeak is still used, but only to find WHICH WAY
    // the massif lies so the road leaves the portal on its uphill side.
    const TunnelStation& mid = bore.st[bore.st.size() / 2];
    float px = mid.x, pz = mid.z;
    climbToPeak(px, pz);            // the massif's summit — a bearing, not a target
    // Stand off the spine on the side the lot is on, clear of the keep-clear
    // band, at the mouth end nearest the lot.
    const TunnelStation& p0 = bore.st.front();
    const TunnelStation& p1 = bore.st.back();
    const float d0 = (p0.x - ax) * (p0.x - ax) + (p0.z - az) * (p0.z - az);
    const float d1 = (p1.x - ax) * (p1.x - ax) + (p1.z - az) * (p1.z - az);
    const TunnelStation& mouth = (d0 <= d1) ? p0 : p1;
    float bx = mouth.x, bz = mouth.z;
    {
        // Push perpendicular to the spine, toward the lot, until clear.
        float sx = p1.x - p0.x, sz = p1.z - p0.z;
        const float sl = std::sqrt(sx * sx + sz * sz);
        if (sl > 1e-3f) { sx /= sl; sz /= sl; }
        float nx = -sz, nz = sx;
        if ((ax - bx) * nx + (az - bz) * nz < 0.0f) { nx = -nx; nz = -nz; }
        const float stand = kRrClearSpineM + 60.0f;
        bx += nx * stand; bz += nz * stand;
    }
    out.endX = bx; out.endZ = bz;
    out.massifX = px; out.massifZ = pz;

    // Keep-clear tests (see kRrClear* in the header).
    const float sx0 = bore.st.front().x, sz0 = bore.st.front().z;
    const float sx1 = bore.st.back().x,  sz1 = bore.st.back().z;
    auto clearOfEverything = [&](float x, float z) {
        if (segPointDist2(x, z, sx0, sz0, sx1, sz1) < kRrClearSpineM * kRrClearSpineM)
            return false;
        if (avoid)
            for (const RoadSpec* a : *avoid) {
                if (!a) continue;
                for (size_t i = 0; i + 1 < a->x.size(); ++i)
                    if (segPointDist2(x, z, a->x[i], a->z[i], a->x[i + 1], a->z[i + 1])
                        < kRrClearRouteM * kRrClearRouteM) return false;
            }
        return true;
    };

    // ---- THE RIDGE ROUTER --------------------------------------------------
    // Step toward the goal; at each step take the heading in the fan whose
    // ground stands highest, paying kRrSeek metres of travel per metre of
    // elevation. That is the whole of "on top of the mountains, curving over
    // mountain features": the curves are the range's, not a spline's.
    const float goalH = naturalAt(bx, bz);   // the portal shoulder's ground
    std::vector<float> rx, rz;
    rx.push_back(ax); rz.push_back(az);
    float cx = ax, cz = az;
    bool arrived = false;
    for (int step = 0; step < kRrMaxSteps; ++step) {
        const float gdx = bx - cx, gdz = bz - cz;
        const float gd  = std::sqrt(gdx * gdx + gdz * gdz);
        if (gd <= kRrArriveM) { arrived = true; break; }
        const float base = std::atan2(gdz, gdx);

        float bestScore = -1e30f, bestX = cx, bestZ = cz;
        bool  any = false;
        for (int k = 0; k < kRrFanCount; ++k) {
            const float f = (kRrFanCount == 1) ? 0.0f
                          : (float)k / (float)(kRrFanCount - 1) * 2.0f - 1.0f;   // -1..1
            const float ang = base + f * (kRrFanDeg * 0.017453292f);
            const float ca = std::cos(ang), sa = std::sin(ang);
            const float qx = cx + ca * kRrStepM;
            const float qz = cz + sa * kRrStepM;
            if (!clearOfEverything(qx, qz)) continue;
            // Probe this heading several steps out. RIDGE-NESS is how far the
            // ground stands above its own neighbours ACROSS the direction of
            // travel — the spine test. GRADE COST is whatever the road would
            // have to climb or drop faster than it is allowed to. See the
            // header for why altitude itself is not in here.
            const float nx = -sa, nz = ca;                 // cross-track
            const float freeClimb = kRrStepM * kRrFreeClimbFrac;
            float ridge = 0.0f, gradeCost = 0.0f, cross = 0.0f, prevH = naturalAt(cx, cz);
            for (int p = 1; p <= kRrLookahead; ++p) {
                const float px = cx + ca * kRrStepM * (float)p;
                const float pz = cz + sa * kRrStepM * (float)p;
                const float h  = naturalAt(px, pz);
                const float hL = naturalAt(px - nx * kRrProbeM, pz - nz * kRrProbeM);
                const float hR = naturalAt(px + nx * kRrProbeM, pz + nz * kRrProbeM);
                ridge     += h - 0.5f * (hL + hR);
                // CROSS-SLOPE: how hard would it be to bench a shelf here? A
                // gentle flank is cheap, a cliff face or a summit is not. This
                // is the cut, estimated before the grader has to pay for it.
                cross     += std::fabs(hL - hR) / (2.0f * kRrProbeM);
                gradeCost += std::max(0.0f, std::fabs(h - prevH) - freeClimb);
                prevH = h;
            }
            const float inv = 1.0f / (float)kRrLookahead;
            ridge *= inv; gradeCost *= inv; cross *= inv;
            const float ndx = bx - qx, ndz = bz - qz;
            const float nd  = std::sqrt(ndx * ndx + ndz * ndz);
            // REACHABILITY, soft: measured against the rope still in hand, not
            // the straight line — a mountain road sheds height by wrapping
            // around the mountain, and it is allowed to take the length.
            const float rope = std::max(nd, (float)(kRrMaxSteps - step) * kRrStepM * 0.5f);
            const float need = std::fabs(naturalAt(qx, qz) - goalH) / std::max(1.0f, rope);
            const float over = std::max(0.0f, need - kRrMaxGrade * kRrReachFrac);
            const float score = ridge * kRrRidgeW - gradeCost * kRrGradeW
                              - cross * kRrCrossW - over * kRrReachW - nd;
            if (score > bestScore) { bestScore = score; bestX = qx; bestZ = qz; any = true; }
        }
        if (!any) {   // boxed in by keep-clear — take the direct step and move on
            bestX = cx + gdx / gd * kRrStepM;
            bestZ = cz + gdz / gd * kRrStepM;
        }
        cx = bestX; cz = bestZ;
        rx.push_back(cx); rz.push_back(cz);
    }
    if (!arrived) { out.whyNot = "router ran out of rope before reaching the massif"; return out; }
    rx.push_back(bx); rz.push_back(bz);
    if (rx.size() < 8) { out.whyNot = "route too short to be a road"; return out; }

    // ---- THE SPEC ----------------------------------------------------------
    RoadSpec s;
    s.name         = "summit ridge road";
    s.surfaceSet   = "terrain_bluff_clay";   // dirt
    s.laneMarkings = false;                  // no stripes on a mountain track
    s.widthScale   = kRrWidthScale;
    s.halfWidth    = kPavedHalfM * kRrWidthScale + kRrCarveMargin;
    s.falloff      = 14.0f;
    s.maxGrade     = kRrMaxGrade;
    // VERTICAL CURVE. This is the constant that decides whether the road can
    // follow a ridge at all. At the spur's 1.6e-3 (K 6.2) the graded datum is
    // too stiff to rise and fall over the saddles and knolls of a ridge line,
    // so it runs through them straight and the grader answers with hundreds of
    // feet of cut. Loosened to K 2.0 — which is not a licence to be rough, it
    // is this road's DESIGN SPEED written down: RoadSpec's own comment gives
    // vertical acceleration as v^2 * rate, so 5.0e-3 at a dirt track's 30 mph
    // (13.4 m/s) is 0.9 m/s^2, the same tenth of g the freeway constant buys at
    // 100 mph. Same comfort, slower road. (PAIRED with kRrFreeClimbFrac, which
    // is what the router is allowed to plan for.)
    s.maxGradeRate = 5.0e-3f;
    s.minTurnRadiusM   = 60.0f;    // a dirt road may bend like one
    s.maxDeflectionDeg = 6.0f;
    s.x = rx; s.z = rz;

    out.road = registerRoad(s, &out.roadY);
    if (!out.road.ok) { out.whyNot = "registerRoad failed"; return out; }
    out.spec        = s;   // AFTER registerRoad: it smooths the polyline in place
    out.built       = true;
    out.lengthMi    = out.road.lengthM / kMPerMile;
    out.maxGradePct = out.road.maxGradePct;
    out.maxCutFt    = out.road.maxCutM * kFtPerM;

    // ---- WHERE IS THE CUT? registerRoad reports how deep the deepest cut is
    // and never where, and tuning a router against a depth with no position is
    // guessing. Walk the graded datum against the natural ground and find it.
    {
        float worst = -1e30f, run = 0.0f, at = 0.0f, wx = 0.0f, wz = 0.0f;
        for (size_t i = 0; i < out.spec.x.size() && i < out.roadY.size(); ++i) {
            if (i) {
                const float dx = out.spec.x[i] - out.spec.x[i-1];
                const float dz = out.spec.z[i] - out.spec.z[i-1];
                run += std::sqrt(dx * dx + dz * dz);
            }
            const float cut = naturalAt(out.spec.x[i], out.spec.z[i]) - out.roadY[i];
            if (cut > worst) { worst = cut; at = run; wx = out.spec.x[i]; wz = out.spec.z[i]; }
        }
        out.maxCutAtMi = at / kMPerMile;
        out.maxCutX = wx; out.maxCutZ = wz;
    }

    // ---- THE RECEIPT, and the A/B that justifies the router ----------------
    // Mean natural ground along the line we chose, against the straight line
    // between the SAME two ends, sampled the same number of times. If the
    // router is doing anything, the first number is higher and the cut the
    // straight line would need is worse. --test-ridgeroad G4 gates this; it is
    // reported every boot so a regression shows up in the log, not just in CI.
    {
        const int N = 240;
        double sumR = 0.0, sumS = 0.0; float worstStraightCut = 0.0f;
        // Ours: walk the registered polyline by arc length.
        std::vector<float> cum(out.spec.x.size(), 0.0f);
        for (size_t i = 1; i < out.spec.x.size(); ++i) {
            const float dx = out.spec.x[i] - out.spec.x[i-1];
            const float dz = out.spec.z[i] - out.spec.z[i-1];
            cum[i] = cum[i-1] + std::sqrt(dx * dx + dz * dz);
        }
        const float L = cum.back();
        size_t seg = 0;
        for (int i = 0; i < N; ++i) {
            const float want = L * (float)i / (float)(N - 1);
            while (seg + 2 < cum.size() && cum[seg + 1] < want) ++seg;
            const float span = std::max(1e-3f, cum[seg + 1] - cum[seg]);
            const float t = clampf((want - cum[seg]) / span, 0.0f, 1.0f);
            const float px = out.spec.x[seg] + (out.spec.x[seg+1] - out.spec.x[seg]) * t;
            const float pz = out.spec.z[seg] + (out.spec.z[seg+1] - out.spec.z[seg]) * t;
            sumR += naturalAt(px, pz);
        }
        // Theirs: the straight line, plus what it would have to cut to hold a
        // constant grade between the same endpoints (the trench through every
        // saddle — the thing the router exists to avoid).
        const float y0 = naturalAt(ax, az), y1 = naturalAt(bx, bz);
        for (int i = 0; i < N; ++i) {
            const float t = (float)i / (float)(N - 1);
            const float px = ax + (bx - ax) * t, pz = az + (bz - az) * t;
            const float h  = naturalAt(px, pz);
            sumS += h;
            worstStraightCut = std::max(worstStraightCut, h - (y0 + (y1 - y0) * t));
        }
        out.meanElevFt     = (float)(sumR / N) * kFtPerM;
        out.straightElevFt = (float)(sumS / N) * kFtPerM;
        out.straightCutFt  = worstStraightCut * kFtPerM;
    }

    char b[420];
    std::snprintf(b, sizeof(b),
        "summit ridge road: %.2f miles of dirt, %u nodes -> %u corridors | "
        "max grade %.1f%% (cap %.0f%%) | deepest cut %.0f ft | formation %.1f ft wide | "
        "deepest cut is at mile %.2f of %.2f, at (%.0f, %.0f) | "
        "TOPS: mean ground %.0f ft vs %.0f ft on the straight line "
        "(which would have had to cut %.0f ft)",
        (double)out.lengthMi, out.road.nodeCount, out.road.corridorCount,
        (double)out.maxGradePct, (double)(kRrMaxGrade * 100.0f), (double)out.maxCutFt,
        (double)kRrFormationFt,
        (double)out.maxCutAtMi, (double)out.lengthMi, (double)out.maxCutX, (double)out.maxCutZ,
        (double)out.meanElevFt, (double)out.straightElevFt,
        (double)out.straightCutFt);
    x3::logInfo(b);
    return out;
}

void buildRidgeRoad(const RidgeRoadResult& rr, Scene& scene,
                    x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& phys) {
    if (!rr.built) return;
    const RoadRibbonResult rib =
        buildRoadRibbon(rr.spec, scene, device, phys, &rr.roadY);
    char b[200];
    std::snprintf(b, sizeof(b),
                  "summit ridge road BUILT: %u meshes, %u quads, %.2f miles | "
                  "guardrail %u, jersey %u",
                  rib.meshCount, rib.quadCount, (double)(rib.lengthM / kMPerMile),
                  rib.railSegments, rib.jerseySegments);
    x3::logInfo(b);
}

// ---------------------------------------------------------------------------
// --test-ridgeroad
// ---------------------------------------------------------------------------
bool runRidgeRoadSelfTest() {
    int passN = 0, failN = 0;
    auto check = [&](bool ok, const char* name, const char* detail) {
        char b[320];
        std::snprintf(b, sizeof(b), "[ridgeroad] %s %s — %s",
                      ok ? "PASS" : "FAIL", name, detail ? detail : "");
        if (ok) { ++passN; x3::logInfo(b); } else { ++failN; x3::logError(b); }
    };
    char d[300];
    auto bail = [&]() {
        clearTerrainCorridors();
        x3::logInfo("[ridgeroad] " + std::to_string(passN) + " passed, " +
                    std::to_string(failN) + " failed");
        return false;
    };

    clearTerrainCorridors();

    // The real world, booted in host_tunnel.cpp's order — the ridge road has to
    // keep clear of everything already in it, so a test world with fewer roads
    // in it would be an easier problem than the one that ships.
    const TunnelRoute& bore = registerTunnelCorridor();
    RoadSpec ringSpec = makeInnerCourse();
    std::vector<float> ringRoadY;
    const RoadBuildResult rr0 = registerRoad(ringSpec, &ringRoadY);
    check(rr0.ok, "G0 the world the road lives in is registered",
          rr0.ok ? "inner course carved" : "registerRoad failed");
    if (!rr0.ok) return bail();
    SpawnConnectorResult conn = registerSpawnConnector(bore, ringSpec, ringRoadY);

    std::vector<const RoadSpec*> avoid{ &conn.spec, &ringSpec };
    SummitSpurResult spur = registerSummitSpur(conn.spec, conn.roadY, &bore, &avoid);
    if (!spur.built) spur = registerSummitSpur(ringSpec, ringRoadY, &bore, &avoid);
    if (!spur.built) { check(false, "G0b the summit spur exists", spur.whyNot); return bail(); }
    SummitLotResult lot = registerSummitLot(spur);
    if (!lot.built) { check(false, "G0c the summit lot exists", lot.whyNot); return bail(); }
    avoid.push_back(&spur.spec);

    const uint32_t before = terrainCorridorCount();
    RidgeRoadResult rr = registerRidgeRoad(lot, bore, &avoid);
    std::snprintf(d, sizeof(d), "%s", rr.built ? "registered" : rr.whyNot);
    check(rr.built, "G1 the ridge road registers between the lot and the bore's massif", d);
    if (!rr.built) return bail();

    // G2 — IT IS THE ROAD TIM DESCRIBED. "that long" is ~4.4 miles as the crow
    // flies; a ridge line is longer than the crow's line, never shorter, and a
    // route that came in UNDER the straight-line distance would mean the router
    // cheated the endpoints.
    const float crowMi = [&]{
        const float dx = rr.endX - lot.mouthX, dz = rr.endZ - lot.mouthZ;
        return std::sqrt(dx * dx + dz * dz) / kMPerMile;
    }();
    std::snprintf(d, sizeof(d), "%.2f miles of road over %.2f miles of crow flight (%.0f%% longer), %u corridors",
                  (double)rr.lengthMi, (double)crowMi,
                  (double)((rr.lengthMi / std::max(0.01f, crowMi) - 1.0f) * 100.0f),
                  terrainCorridorCount() - before);
    check(rr.lengthMi >= crowMi * 0.98f && rr.lengthMi <= 9.0f && crowMi >= 3.0f,
          "G2 it is a multi-mile mountain road, and longer than the straight line", d);

    // G3 — DRIVABLE. registerRoad's own grade cap, measured on what shipped.
    std::snprintf(d, sizeof(d), "worst grade %.1f%% (cap %.0f%%)",
                  (double)rr.maxGradePct, (double)(kRrMaxGrade * 100.0f));
    check(rr.maxGradePct <= kRrMaxGrade * 100.0f + 0.05f,
          "G3 the grades are inside the mountain-road cap", d);

    // G4 — THE ONE THAT MATTERS: IT RUNS THE TOPS. A/B against the straight
    // line between its own two ends. This is the whole brief ("on top of the
    // mountains .. curving over mountain features") reduced to two numbers, and
    // it is a real control: set kRrSeek to 0 and the router IS the straight
    // line, both numbers converge, and this gate fails.
    std::snprintf(d, sizeof(d),
                  "mean ground under the road %.0f ft vs %.0f ft under the straight line "
                  "(+%.0f ft); the straight line would have had to cut %.0f ft, we cut %.0f ft",
                  (double)rr.meanElevFt, (double)rr.straightElevFt,
                  (double)(rr.meanElevFt - rr.straightElevFt),
                  (double)rr.straightCutFt, (double)rr.maxCutFt);
    check(rr.meanElevFt > rr.straightElevFt + 25.0f, "G4 the road runs the TOPS, not the line", d);

    // G5 — IT SITS ON THE LANDSCAPE. A dirt road over the tops is benched into
    // the hillside, not blasted through it. Absolute ceiling, not a comparison
    // against the straight line — see kRrMaxCutFt for why that comparison was
    // the wrong question. (The straight line's number is still reported as
    // context: it is the LOW road, and low roads are cheap.)
    // This gate has teeth: scoring altitude instead of ridge-ness read 774 ft
    // here, because a road that drives at summits gets cut through them.
    std::snprintf(d, sizeof(d), "deepest cut %.0f ft (ceiling %.0f ft); the low straight line's would be %.0f ft",
                  (double)rr.maxCutFt, (double)kRrMaxCutFt, (double)rr.straightCutFt);
    check(rr.maxCutFt <= kRrMaxCutFt, "G5 it is benched into the hill, not trenched through it", d);

    // G6 — IT ACTUALLY MEETS THE LOT. A long road that stops short of the pad
    // is scenery; this is the same "does it connect" question the connector's
    // gate asks, at the other end of the world.
    const float dx0 = rr.spec.x.front() - lot.mouthX, dz0 = rr.spec.z.front() - lot.mouthZ;
    const float gap = std::sqrt(dx0 * dx0 + dz0 * dz0);
    std::snprintf(d, sizeof(d), "starts %.1f ft from the lot's entry mouth", (double)(gap * kFtPerM));
    check(gap < 30.0f, "G6 it starts at the summit lot's mouth", d);

    clearTerrainCorridors();
    x3::logInfo("[ridgeroad] " + std::to_string(passN) + " passed, " +
                std::to_string(failN) + " failed");
    return failN == 0;
}

} // namespace x3::game
