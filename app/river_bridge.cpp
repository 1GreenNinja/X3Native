// THE RIVER CROSSING — see app/river_bridge.h.
#include "river_bridge.h"

#include "terrain.h"
#include "scene.h"
#include "surface_library.h"
#include "asset_root.h"
#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace x3::game {

namespace {

constexpr float kMToFt = 3.28084f;

// The natural (pre-corridor) surface. The river carve is part of the authored
// landform layer UNDER the corridors, so subtracting the corridor delta gives
// the river bed and levees exactly as authored, whether or not roads have
// registered yet — the same trick tunnelNaturalHeightAt uses.
float naturalAt(float x, float z) {
    return terrainHeightAtWorld(x, z) - terrainCorridorDelta(x, z);
}

// -------------------------------------------------------------------------
// THE VALLEY ROAD's authored waypoints (world XZ, metres).
//
// West end sits ON the inner tour (radius 3842 about (-592,-352), at -80°);
// east end likewise at -10.5°. Between them the road runs north past the
// ocean basin's WEST shore (the first draft of this line went east of it and
// the distance check said "that is open sea"), squares up to the river over
// an authored straight reach, and crosses at the plan's named site. The
// straight reach exists because a bridge is crossed SQUARE (skew <= 15°) and
// because the deck, slabs and abutments all live on one axis.
// -------------------------------------------------------------------------
struct Waypoint { float x, z; };
const Waypoint kWestLeg[] = {
    {   75.2f, -4135.6f },   // on the inner tour, -80°
    {  330.0f, -1650.0f },   // west of the basin shore (measured 716 m off centre)
    {  330.0f, -1150.0f },
    {  352.0f,  -905.0f },   // begins the swing onto the bridge axis
};
const Waypoint kEastLeg[] = {
    { 1000.0f,  -560.0f },   // clear of ravine R2's mouth (>= 280 m, measured)
    { 1600.0f,  -620.0f },
    { 2400.0f,  -760.0f },
    { 3185.6f, -1052.1f },   // on the inner tour, -10.5°
};
constexpr float kNodeSpacing = 61.0f;   // ~200 ft, the tours' spacing

// Emits the leg's START node (unless told not to — used where the start was
// already pushed by the previous element) plus the interior nodes; never the
// end node, which the NEXT leg emits as its own start. The first cut of this
// emitted neither end for interior legs and silently dropped every shared
// waypoint — caught on paper before it shipped, recorded so it stays caught.
void expandLeg(std::vector<float>& xs, std::vector<float>& zs,
               float ax, float az, float bx, float bz, bool emitStart) {
    const float len = std::sqrt((bx-ax)*(bx-ax) + (bz-az)*(bz-az));
    const int n = std::max(1, (int)std::ceil(len / kNodeSpacing));
    for (int k = emitStart ? 0 : 1; k < n; ++k) {
        const float t = (float)k / (float)n;
        xs.push_back(ax + (bx - ax) * t);
        zs.push_back(az + (bz - az) * t);
    }
}

} // namespace

RiverBridgePlan planRiverBridge() {
    RiverBridgePlan p;

    // The site: the middle of the authored N5-N6 reach — the plan's Bridge
    // No. 1. GOTCHA, caught by this module's own gate on first run:
    // worldRiverNodes() returns the CHAIKIN WORKING CHAIN (16 nodes), not the
    // 9 authored nodes — indexing it as authored planned the bridge on the
    // beach reach by the facility, where the levee is 8 in tall and the
    // soffit-clearance check failed instantly. The mid-reach of authored
    // N5->N6 is the chain segment between N5's downstream cut point and N6's
    // upstream one: chain[10] -> chain[11].
    uint32_t nRiver = 0;
    const WorldRiverNode* rn = worldRiverNodes(nRiver);
    if (!rn || nRiver < 13) { p.whyNot = "no river chain"; return p; }
    const WorldRiverNode& n5 = rn[10];
    const WorldRiverNode& n6 = rn[11];
    p.cx = (n5.x + n6.x) * 0.5f;
    p.cz = (n5.z + n6.z) * 0.5f;

    // Deck axis: perpendicular to the reach, pointing across to the NE bank.
    float rdx = n6.x - n5.x, rdz = n6.z - n5.z;
    if (std::sqrt(rdx*rdx + rdz*rdz) < 100.0f) {
        // the chain shape changed under us — refuse to guess
        p.whyNot = "river chain shape changed: mid-reach segment too short";
        return p;
    }
    const float rl = std::sqrt(rdx * rdx + rdz * rdz);
    if (rl < 1.0f) { p.whyNot = "degenerate river reach"; return p; }
    rdx /= rl; rdz /= rl;
    p.dirX = -rdz; p.dirZ = rdx;
    if (p.dirX < 0.0f) { p.dirX = -p.dirX; p.dirZ = -p.dirZ; }   // NE-ish (+x)
    // skew = how far off square the deck meets the river: 0° for perpendicular
    const float dot = std::fabs(p.dirX * rdx + p.dirZ * rdz);
    p.skewDeg = 90.0f - std::acos(std::min(dot, 1.0f)) * 57.29578f;

    // Water and bed at the crossing. worldWaterLevelAt answers from the SAME
    // working chain the carve and the ribbon use.
    p.waterY = worldWaterLevelAt(p.cx, p.cz);
    if (p.waterY <= kWorldWaterDry + 1.0f) { p.whyNot = "site is dry"; return p; }
    p.bedY = naturalAt(p.cx, p.cz);

    // Bank crests, measured along the deck axis over the levee band each side.
    auto crest = [&](float sgn) {
        float hi = -1e9f;
        for (float s = 25.0f; s <= p.abutS + 0.1f; s += 1.0f)
            hi = std::max(hi, naturalAt(p.cx + p.dirX * s * sgn,
                                        p.cz + p.dirZ * s * sgn));
        return hi;
    };
    p.crestW = crest(-1.0f);
    p.crestE = crest(+1.0f);

    // LOW-SET deck (the plan's default, and the decision Tim still owns): top
    // of deck = the higher crest + 2 ft. High-set (waterY + 16-20 ft) stays
    // rejected until embankment approaches exist — corridors cannot fill.
    p.deckY = std::max(p.crestW, p.crestE) + 0.61f;
    p.pierTopY = p.deckY - p.depthPier;
    p.pierBedY[0] = naturalAt(p.cx - p.dirX * p.pierS, p.cz - p.dirZ * p.pierS);
    p.pierBedY[1] = naturalAt(p.cx + p.dirX * p.pierS, p.cz + p.dirZ * p.pierS);
    p.soffitClearM = (p.deckY - p.depthMid) - p.waterY;

    // Structural sanity, asserted in the plan itself (B8's builder half).
    if (p.skewDeg > 15.0f)            { p.whyNot = "crossing not square";  return p; }
    if (p.pierS <= 12.0f)             { p.whyNot = "piers inside the full-depth floor"; return p; }
    if (p.soffitClearM < 1.22f)       { p.whyNot = "midspan soffit under 4 ft over water"; return p; }
    if (p.pierBedY[0] > p.pierTopY || p.pierBedY[1] > p.pierTopY) {
        p.whyNot = "pier bed above girder"; return p;
    }
    p.ok = true;
    return p;
}

RoadSpec makeValleyRoad(const RiverBridgePlan& p, uint32_t* outGapA, uint32_t* outGapB) {
    RoadSpec s;
    s.name      = "valley road";
    s.halfWidth = kPavedHalfM + 1.0f;
    s.falloff   = 14.0f;
    s.maxGrade  = 0.07f;

    auto axis = [&](float ss, float& x, float& z) {
        x = p.cx + p.dirX * ss; z = p.cz + p.dirZ * ss;
    };
    // west leg down to the start of the straight reach
    float rx0, rz0, rx1, rz1;
    axis(-170.0f, rx0, rz0);
    axis(+170.0f, rx1, rz1);
    const int nW = (int)(sizeof(kWestLeg) / sizeof(kWestLeg[0]));
    for (int i = 0; i + 1 < nW; ++i)
        expandLeg(s.x, s.z, kWestLeg[i].x, kWestLeg[i].z,
                  kWestLeg[i+1].x, kWestLeg[i+1].z, true);
    expandLeg(s.x, s.z, kWestLeg[nW-1].x, kWestLeg[nW-1].z, rx0, rz0, true);

    // THE STRAIGHT REACH. Exact stations, because the gap edges, the abutments
    // and the deck all key off them: -170, -slabS, -abutS, 0, +abutS, +slabS,
    // +170. The gap spans slab end to slab end — the carve stops where the
    // structure begins, and the structure includes the approach slabs.
    const float stations[] = { -170.0f, -p.slabS, -p.abutS, 0.0f,
                               +p.abutS, +p.slabS, +170.0f };
    for (int i = 0; i < 7; ++i) {
        float x, z; axis(stations[i], x, z);
        if (i == 1 && outGapA) *outGapA = (uint32_t)s.x.size();
        if (i == 5 && outGapB) *outGapB = (uint32_t)s.x.size();
        s.x.push_back(x); s.z.push_back(z);
    }

    // east leg away from the reach (+170 was pushed by the stations block, so
    // the first leg does not re-emit its start)
    const int nE = (int)(sizeof(kEastLeg) / sizeof(kEastLeg[0]));
    expandLeg(s.x, s.z, rx1, rz1, kEastLeg[0].x, kEastLeg[0].z, false);
    for (int i = 0; i + 1 < nE; ++i)
        expandLeg(s.x, s.z, kEastLeg[i].x, kEastLeg[i].z,
                  kEastLeg[i+1].x, kEastLeg[i+1].z, true);
    s.x.push_back(kEastLeg[nE-1].x); s.z.push_back(kEastLeg[nE-1].z);
    return s;
}

RiverRoadResult registerRiverRoad(const RoadSpec* ringSpec,
                                  const std::vector<float>* ringRoadY) {
    RiverRoadResult out;
    out.plan = planRiverBridge();
    if (!out.plan.ok) {
        x3::logError(std::string("river road: bridge plan failed — ") + out.plan.whyNot);
        return out;
    }
    uint32_t gapA = 0, gapB = 0;
    out.spec = makeValleyRoad(out.plan, &gapA, &gapB);
    RoadSpec::Gap g;
    g.i0 = gapA; g.i1 = gapB;
    g.y0 = g.y1 = out.plan.deckY;   // the deck is LEVEL — no superelevation,
                                    // no grade, exactly as bridges are built
    out.spec.gaps.push_back(g);
    // HORIZONTAL FLOW, freeway class — same law as every route. The gap goes
    // on FIRST so the smoother locks the bridge reach (a deck is straight)
    // and remaps the gap's node indices through the subdivision.
    out.spec.minTurnRadiusM   = 250.0f;
    out.spec.maxDeflectionDeg = 3.0f;
    smoothHorizontalCurves(out.spec);
    // THE RING LANDINGS — attach both ends to the tour at grade (see .h).
    if (ringSpec && ringRoadY) {
        out.ringJctA = attachRoadEndToRoute(out.spec, /*atFront=*/true,
                                            *ringSpec, *ringRoadY, &out.ringNodeA);
        out.ringJctB = attachRoadEndToRoute(out.spec, /*atFront=*/false,
                                            *ringSpec, *ringRoadY, &out.ringNodeB);
        if (!out.ringJctA.valid || !out.ringJctB.valid)
            x3::logWarn("valley road: a leg end is not on the tour — landing skipped");
    }
    out.road = registerRoad(out.spec, &out.roadY);
    if (out.road.ok && !out.roadY.empty()) {
        if (out.ringJctA.valid) {
            out.ringJctA.endY = out.roadY.front();
            registerRoadJunctionThroat(out.ringJctA.endX, out.ringJctA.endZ,
                                       out.ringJctA.jx, out.ringJctA.jz,
                                       out.ringJctA.jy);
        }
        if (out.ringJctB.valid) {
            out.ringJctB.endY = out.roadY.back();
            registerRoadJunctionThroat(out.ringJctB.endX, out.ringJctB.endZ,
                                       out.ringJctB.jx, out.ringJctB.jz,
                                       out.ringJctB.jy);
        }
        if (out.ringJctA.valid || out.ringJctB.valid) {
            char jb[300];
            std::snprintf(jb, sizeof(jb),
                "valley road: ring landings at grade — west (%.0f, %.0f) datum "
                "%.1f (end off %.2f ft), east (%.0f, %.0f) datum %.1f (end off "
                "%.2f ft), pin deficit %.2f ft",
                out.ringJctA.jx, out.ringJctA.jz, out.ringJctA.jy,
                (out.roadY.front() - out.ringJctA.jy) * 3.28084f,
                out.ringJctB.jx, out.ringJctB.jz, out.ringJctB.jy,
                (out.roadY.back() - out.ringJctB.jy) * 3.28084f,
                out.road.pinErrM * 3.28084f);
            x3::logInfo(jb);
        }
    }

    char b[300];
    std::snprintf(b, sizeof(b),
        "valley road: %.2f miles, bridge No.1 at (%.0f, %.0f) — deck %.0f ft, "
        "%.1f ft over the water, crests %.1f/%.1f ft above it",
        out.road.lengthM / 1609.34f, out.plan.cx, out.plan.cz,
        out.plan.abutS * 2.0f * kMToFt,
        (out.plan.deckY - out.plan.waterY) * kMToFt,
        (out.plan.crestW - out.plan.waterY) * kMToFt,
        (out.plan.crestE - out.plan.waterY) * kMToFt);
    x3::logInfo(b);
    return out;
}

// ---------------------------------------------------------------------------
// THE BUILDER — meshes + collision. Same quad-buffer approach as the road
// ribbon; the bridge is boxes and swept sections, not sculpture.
// ---------------------------------------------------------------------------
namespace {

struct MeshBuf {
    std::vector<x3::rhi::MeshVertex> v;
    std::vector<uint32_t> i;
    void quad(const float a[3], const float b[3], const float c[3], const float d[3],
              float uScale = 0.25f) {
        // face normal from the winding
        const float e1[3] = { b[0]-a[0], b[1]-a[1], b[2]-a[2] };
        const float e2[3] = { d[0]-a[0], d[1]-a[1], d[2]-a[2] };
        float n[3] = { e1[1]*e2[2]-e1[2]*e2[1], e1[2]*e2[0]-e1[0]*e2[2],
                       e1[0]*e2[1]-e1[1]*e2[0] };
        const float nl = std::sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);
        if (nl > 1e-6f) { n[0]/=nl; n[1]/=nl; n[2]/=nl; }
        const uint32_t base = (uint32_t)v.size();
        const float* pts[4] = { a, b, c, d };
        const float us[4] = { 0, 1, 1, 0 }, ws[4] = { 0, 0, 1, 1 };
        for (int k = 0; k < 4; ++k) {
            x3::rhi::MeshVertex mv{};
            mv.pos[0]=pts[k][0]; mv.pos[1]=pts[k][1]; mv.pos[2]=pts[k][2];
            mv.normal[0]=n[0]; mv.normal[1]=n[1]; mv.normal[2]=n[2];
            mv.uv[0]=us[k]*uScale*4.0f; mv.uv[1]=ws[k]*uScale*4.0f;
            v.push_back(mv);
        }
        i.push_back(base+0); i.push_back(base+1); i.push_back(base+2);
        i.push_back(base+0); i.push_back(base+2); i.push_back(base+3);
    }
    // axis-aligned-to-frame box: centre (s,l,y sizes along deck frame)
    void box(const RiverBridgePlan& p, float s0, float s1, float l0, float l1,
             float y0, float y1) {
        auto pt = [&](float ss, float ll, float yy, float o[3]) {
            o[0] = p.cx + p.dirX * ss + (-p.dirZ) * ll;
            o[1] = yy;
            o[2] = p.cz + p.dirZ * ss + ( p.dirX) * ll;
        };
        float c[8][3];
        pt(s0,l0,y0,c[0]); pt(s1,l0,y0,c[1]); pt(s1,l1,y0,c[2]); pt(s0,l1,y0,c[3]);
        pt(s0,l0,y1,c[4]); pt(s1,l0,y1,c[5]); pt(s1,l1,y1,c[6]); pt(s0,l1,y1,c[7]);
        quad(c[4],c[5],c[6],c[7]);          // top
        quad(c[3],c[2],c[1],c[0]);          // bottom
        quad(c[0],c[1],c[5],c[4]);          // side l0
        quad(c[2],c[3],c[7],c[6]);          // side l1
        quad(c[1],c[2],c[6],c[5]);          // end s1
        quad(c[3],c[0],c[4],c[7]);          // end s0
    }
    bool empty() const { return i.empty(); }
};

SurfaceLibrary& bridgeSurfaces() { static SurfaceLibrary lib; return lib; }

} // namespace

RiverBridgeBuildResult buildRiverBridge(const RiverBridgePlan& p, Scene& scene,
                                        x3::rhi::IRenderDevice& device,
                                        x3::phys::IPhysicsWorld& phys) {
    RiverBridgeBuildResult out;
    if (!p.ok) return out;

    SurfaceLibrary& surf = bridgeSurfaces();
    surf.mount(assetRoot() + "/surface_library");
    // The exact concrete family the tunnel portals wear — one world, one concrete.
    const SurfaceSet& concrete = surf.get(device, "mw_concrete_panels_a");
    const SurfaceSet& asphalt  = surf.get(device, "rd_asphalt_01");

    auto pt = [&](float ss, float ll, float yy, float o[3]) {
        o[0] = p.cx + p.dirX * ss + (-p.dirZ) * ll;
        o[1] = yy;
        o[2] = p.cz + p.dirZ * ss + ( p.dirX) * ll;
    };
    // Haunched structure depth at station s: 3.5 ft at midspan and the
    // abutments, 6 ft over each pier, parabolic between — the varying soffit
    // line IS the "beautiful concrete" the night lighting will pick out.
    auto depthAt = [&](float s) {
        const float a = std::fabs(s);
        float q;
        if (a <= p.pierS) { const float u = a / p.pierS; q = u * u; }
        else { const float u = (a - p.pierS) / (p.abutS - p.pierS); q = (1.0f-u)*(1.0f-u); }
        return p.depthMid + (p.depthPier - p.depthMid) * q;
    };

    MeshBuf deckTop, structure, paint;

    // ---- the deck: swept box girder, ~3 m stations --------------------------
    {
        const float w = p.deckHalfWidth;
        const float webL = 4.25f;      // box webs at +-14 ft; slab cantilevers to 21.5
        float prevS = -p.abutS;
        for (float s = -p.abutS + 3.0f; s <= p.abutS + 0.01f; s += 3.0f) {
            const float s1 = std::min(s, p.abutS);
            float a[3], b[3], c[3], d[3];
            // running surface (asphalt).
            // WINDING: d,c,b,a — the normal must point UP. As a,b,c,d this
            // quad faced DOWN, which produced Tim's exact double bug: "The
            // Bridge renders from below but not above! and.. ou fall
            // through!!!" — backface culling hid the deck from above, and the
            // wheel raycasts (which cull back faces in Jolt) sailed through
            // the same inverted triangles, because the collider shares this
            // very index buffer. One winding, both symptoms.
            pt(prevS, -w, p.deckY, a); pt(s1, -w, p.deckY, b);
            pt(s1,  w, p.deckY, c);    pt(prevS, w, p.deckY, d);
            deckTop.quad(d, c, b, a);
            // fascia edges (0.35 m of slab side)
            for (int side = -1; side <= 1; side += 2) {
                pt(prevS, w*side, p.deckY, a); pt(s1, w*side, p.deckY, b);
                pt(s1, w*side, p.deckY-0.35f, c); pt(prevS, w*side, p.deckY-0.35f, d);
                structure.quad(a, b, c, d);
                // cantilever underside, slab edge in to the web
                pt(prevS, w*side, p.deckY-0.35f, a); pt(s1, w*side, p.deckY-0.35f, b);
                pt(s1, webL*side, p.deckY-0.55f, c); pt(prevS, webL*side, p.deckY-0.55f, d);
                structure.quad(a, b, c, d);
            }
            // webs down to the soffit, and the soffit itself (the haunch line)
            const float d0 = depthAt(prevS), d1 = depthAt(s1);
            for (int side = -1; side <= 1; side += 2) {
                pt(prevS, webL*side, p.deckY-0.55f, a); pt(s1, webL*side, p.deckY-0.55f, b);
                pt(s1, webL*side, p.deckY-d1, c);       pt(prevS, webL*side, p.deckY-d0, d);
                structure.quad(a, b, c, d);
            }
            pt(prevS, -webL, p.deckY-d0, a); pt(s1, -webL, p.deckY-d1, b);
            pt(s1,  webL, p.deckY-d1, c);    pt(prevS,  webL, p.deckY-d0, d);
            structure.quad(a, b, c, d);
            prevS = s1;
        }
    }

    // ---- approach slabs (level, with side skirts to the ground) -------------
    for (int side = -1; side <= 1; side += 2) {
        const float s0 = p.abutS * side, s1 = p.slabS * side;
        structure.box(p, std::min(s0,s1), std::max(s0,s1),
                      -p.deckHalfWidth, p.deckHalfWidth, p.deckY - 0.45f, p.deckY);
        // skirts: slab edge down to sampled ground
        for (float s = std::min(s0,s1); s < std::max(s0,s1) - 0.01f; s += 6.0f) {
            const float sn = std::min(s + 6.0f, std::max(s0,s1));
            for (int ls = -1; ls <= 1; ls += 2) {
                float a[3], b[3], c[3], d[3];
                pt(s, p.deckHalfWidth*ls, p.deckY-0.45f, a);
                pt(sn, p.deckHalfWidth*ls, p.deckY-0.45f, b);
                float g0[3], g1[3];
                pt(s, p.deckHalfWidth*ls, 0, g0); pt(sn, p.deckHalfWidth*ls, 0, g1);
                const float gy0 = terrainHeightAtWorld(g0[0], g0[2]);
                const float gy1 = terrainHeightAtWorld(g1[0], g1[2]);
                pt(sn, p.deckHalfWidth*ls, gy1 - 0.3f, c);
                pt(s,  p.deckHalfWidth*ls, gy0 - 0.3f, d);
                structure.quad(a, b, c, d);
            }
        }
    }

    // ---- abutment seats ------------------------------------------------------
    for (int side = -1; side <= 1; side += 2) {
        const float s = p.abutS * side;
        const float ground = std::min(
            terrainHeightAtWorld(p.cx + p.dirX*s, p.cz + p.dirZ*s), p.deckY - 1.2f);
        structure.box(p, s - 0.6f, s + 0.6f, -p.deckHalfWidth, p.deckHalfWidth,
                      ground - 1.5f, p.deckY - 0.35f);
    }

    // ---- piers: rounded-nose wall piers + waterline collars ------------------
    for (int side = -1; side <= 1; side += 2) {
        const float s = p.pierS * side;
        const float bed = p.pierBedY[side < 0 ? 0 : 1] - 0.5f;   // embedded
        // the wall (long axis ALONG the river = the deck frame's lateral)
        structure.box(p, s - p.pierHalfThick, s + p.pierHalfThick,
                      -p.pierHalfWide, p.pierHalfWide, bed, p.pierTopY);
        // cutwater noses: chamfer boxes at each end
        structure.box(p, s - p.pierHalfThick*0.62f, s + p.pierHalfThick*0.62f,
                      p.pierHalfWide, p.pierHalfWide + 0.9f, bed, p.pierTopY);
        structure.box(p, s - p.pierHalfThick*0.62f, s + p.pierHalfThick*0.62f,
                      -p.pierHalfWide - 0.9f, -p.pierHalfWide, bed, p.pierTopY);
        // the collar at the waterline: what a river pier shows, and what hides
        // the pier/water-ribbon mesh intersection (B6)
        structure.box(p, s - p.pierHalfThick - 0.35f, s + p.pierHalfThick + 0.35f,
                      -p.pierHalfWide - 1.25f, p.pierHalfWide + 1.25f,
                      p.waterY - 0.45f, p.waterY + 0.30f);
    }

    // ---- parapets (WITH collision — a car must not leave the deck sideways) --
    MeshBuf parapet;
    for (int side = -1; side <= 1; side += 2) {
        const float l = (p.deckHalfWidth - 0.225f) * side;
        parapet.box(p, -p.slabS, p.slabS, l - 0.225f*side, l + 0.225f*side,
                    p.deckY, p.deckY + 0.88f);
    }

    // ---- eight parapet lamps: emissive geometry, zero pooled lights ----------
    MeshBuf lamps;
    for (int k = 0; k < 8; ++k) {
        const float s = -74.6f + 21.3f * (float)k;
        const int side = (k & 1) ? 1 : -1;
        const float l = (p.deckHalfWidth - 0.45f) * side;
        lamps.box(p, s - 0.07f, s + 0.07f, l - 0.07f, l + 0.07f,
                  p.deckY + 0.88f, p.deckY + 4.4f);                 // post
        lamps.box(p, s - 0.42f, s + 0.42f, l - 0.16f, l + 0.16f,
                  p.deckY + 4.4f, p.deckY + 4.72f);                 // head
    }

    auto upload = [&](MeshBuf& m, const SurfaceSet* set, const float tint[4],
                      bool collide) {
        if (m.empty()) return;
        Entity e;
        e.mesh = device.createMesh(m.v.data(), (uint32_t)m.v.size(),
                                   m.i.data(), (uint32_t)m.i.size());
        if (!e.mesh.valid()) return;
        if (set && set->ok) { e.tex = set->albedo; e.mrTex = set->mr; e.normalTex = set->normal; }
        for (int c = 0; c < 4; ++c) e.baseColor[c] = tint[c];
        scene.add(e);
        ++out.meshCount;
        out.triCount += (uint32_t)(m.i.size() / 3);
        if (collide) {
            std::vector<float> cv; cv.reserve(m.v.size() * 3);
            for (const auto& vv : m.v) { cv.push_back(vv.pos[0]); cv.push_back(vv.pos[1]); cv.push_back(vv.pos[2]); }
            phys.addStaticMesh(cv.data(), (uint32_t)(cv.size() / 3),
                               m.i.data(), (uint32_t)m.i.size());
        }
    };
    const float dark  [4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    const float pale  [4] = { 0.88f, 0.87f, 0.84f, 1.0f };
    const float warm  [4] = { 1.9f, 1.7f, 1.3f, 1.0f };   // emissive-read heads
    upload(deckTop,   &asphalt,  dark, true);
    upload(structure, &concrete, pale, true);
    upload(parapet,   &concrete, pale, true);
    upload(lamps,     nullptr,   warm, false);
    (void)paint;

    out.ok = out.meshCount > 0;
    char b[200];
    std::snprintf(b, sizeof(b),
        "bridge No.1: %u meshes, %u tris — %.0f ft deck, piers at ±%.0f ft, "
        "soffit %.1f ft over the water",
        out.meshCount, out.triCount, p.abutS * 2.0f * kMToFt, p.pierS * kMToFt,
        p.soffitClearM * kMToFt);
    x3::logInfo(b);
    return out;
}

// ---------------------------------------------------------------------------
// --test-riverbridge
// ---------------------------------------------------------------------------
bool runRiverBridgeSelfTest() {
    int passN = 0, failN = 0;
    auto check = [&](bool ok, const char* name, const char* detail = nullptr) {
        std::string m = std::string(ok ? "PASS " : "FAIL ") + name;
        if (detail && *detail) m += std::string(" — ") + detail;
        if (ok) { ++passN; x3::logInfo("[riverbridge] " + m); }
        else    { ++failN; x3::logError("[riverbridge] " + m); }
    };
    char d[320];

    // RB1 — the river FLOWS. waterY must strictly descend node to node; a
    // river that runs uphill is the one defect nobody forgives (plan gate R4).
    {
        uint32_t n = 0;
        const WorldRiverNode* rn = worldRiverNodes(n);
        float worst = -1e9f; bool desc = n >= 2;
        for (uint32_t i = 0; i + 1 < n; ++i) {
            worst = std::max(worst, rn[i+1].waterY - rn[i].waterY);
            if (rn[i+1].waterY >= rn[i].waterY) desc = false;
        }
        std::snprintf(d, sizeof(d), "%u nodes, steepest adverse step %.2f ft",
                      n, worst * kMToFt);
        check(desc, "RB1 the river flows downhill its whole run", d);
    }

    // RB0 — NEGATIVE CONTROL: the same road WITHOUT the span gap. An at-grade
    // crossing fails one of two ways, and both prove the deck is not
    // decoration: the graded datum DROWNS (roadY below the water surface — the
    // original symptom), or, since vertical-curve smoothing landed in the
    // grader (W-ROADS2), the sag curve lifts the V's bottom and the datum
    // STRANDS in mid-air over the gouged channel instead — corridors cannot
    // fill, so there is still nothing to drive on. Either way the carve also
    // gouges the riverbed. Registered first, cleared before the real thing.
    {
        clearTerrainCorridors();
        const RiverBridgePlan p = planRiverBridge();
        check(p.ok, "RB2a the bridge plan closes", p.ok ? "" : p.whyNot);
        if (!p.ok) return false;

        uint32_t gA = 0, gB = 0;
        RoadSpec naive = makeValleyRoad(p, &gA, &gB);
        naive.name = "valley road (no bridge)";
        std::vector<float> ry;
        const RoadBuildResult nv = registerRoad(naive, &ry);
        float minRoadY = 1e9f;
        for (uint32_t i = gA; i <= gB && i < ry.size(); ++i)
            minRoadY = std::min(minRoadY, ry[i]);
        float channelDelta = 0.0f;
        for (float s = -30.0f; s <= 30.0f; s += 2.0f)
            channelDelta = std::min(channelDelta,
                terrainCorridorDelta(p.cx + p.dirX * s, p.cz + p.dirZ * s));
        // Post-carve ground at the channel centre: what the datum would have
        // to stand on. Above it by 3 m+ with no deck = stranded in the air.
        const float chanGround = terrainHeightAtWorld(p.cx, p.cz);
        const bool drowned  = minRoadY < p.waterY - 1.0f;
        const bool stranded = minRoadY > chanGround + 3.0f;
        std::snprintf(d, sizeof(d),
            "road datum %.1f ft vs water, %.1f ft above the gouged bed (carved %.1f ft off the river) — %s",
            (minRoadY - p.waterY) * kMToFt, (minRoadY - chanGround) * kMToFt,
            -channelDelta * kMToFt,
            drowned ? "drowned" : (stranded ? "stranded mid-air" : "NEITHER, which is the failure"));
        check(nv.ok && channelDelta < -0.5f && (drowned || stranded),
              "RB0 NEGATIVE CONTROL: at-grade crossing cannot work", d);
        clearTerrainCorridors();
    }

    // The real thing.
    const RiverRoadResult rr = registerRiverRoad();
    const RiverBridgePlan& p = rr.plan;

    // RB2 — structural sanity (B8's plan half).
    std::snprintf(d, sizeof(d),
        "skew %.1f°, piers ±%.0f ft (floor is ±39), soffit %.1f ft over water, deck %.1f ft over crest",
        p.skewDeg, p.pierS * kMToFt, p.soffitClearM * kMToFt,
        (p.deckY - std::max(p.crestW, p.crestE)) * kMToFt);
    check(p.ok && p.skewDeg <= 15.0f && p.pierS > 12.0f && p.soffitClearM >= 1.22f,
          "RB2 the structure is sane: square, piers clear, soffit clear", d);

    // RB3 — the road GENUINELY meets the river: wet water directly under the
    // span, dry ground at both slab ends. Non-empty by construction.
    {
        int wet = 0;
        for (float s = -p.abutS; s <= p.abutS; s += 2.0f)
            if (worldWaterLevelAt(p.cx + p.dirX * s, p.cz + p.dirZ * s)
                > kWorldWaterDry + 1.0f) ++wet;
        const bool dryW = worldWaterLevelAt(p.cx - p.dirX * p.slabS,
                                            p.cz - p.dirZ * p.slabS) <= kWorldWaterDry + 1.0f;
        const bool dryE = worldWaterLevelAt(p.cx + p.dirX * p.slabS,
                                            p.cz + p.dirZ * p.slabS) <= kWorldWaterDry + 1.0f;
        std::snprintf(d, sizeof(d), "%d wet samples under the span (%.0f ft of water), banks dry",
                      wet, (float)wet * 2.0f * kMToFt);
        check(rr.road.ok && wet >= 30 && dryW && dryE,
              "RB3 the road actually crosses open water", d);
    }

    // RB4 — THE SPAN GAP (B3/B7): the terrain under the STRUCTURAL span —
    // water, full-depth floor, piers, levee crests, |s| <= 60 m — is
    // BIT-UNTOUCHED river. Not "small" — zero. The window is ±60 m and not the
    // full ±99 m because the slab-end nodes legitimately CUT (the road
    // descends to the low-set deck through the floodplain edge) and a
    // capsule's clamped end carries that cut up to halfWidth+falloff = 29 m
    // past the last carved node — measured to reach |s| ≈ 70, clear of
    // everything that is river.
    {
        float worst = 0.0f;
        for (float s = -60.0f; s <= 60.0f; s += 1.0f)
            for (float l = -25.0f; l <= 25.0f; l += 5.0f)
                worst = std::min(worst, terrainCorridorDelta(
                    p.cx + p.dirX * s + (-p.dirZ) * l,
                    p.cz + p.dirZ * s + ( p.dirX) * l));
        std::snprintf(d, sizeof(d), "max carve inside the span %.4f ft", -worst * kMToFt);
        check(worst == 0.0f, "RB4 the river under the span is untouched, bit-exactly", d);
    }

    // RB5 — R2 through the bridge reach: bank crests at the water's edge stay
    // ABOVE the water for 500 ft up- and downstream. The approach cuts must
    // never notch the levee below the waterline.
    {
        uint32_t n = 0;
        const WorldRiverNode* rn = worldRiverNodes(n);
        float rdx = rn[11].x - rn[10].x, rdz = rn[11].z - rn[10].z;   // the N5-N6 mid-reach
        const float rl = std::sqrt(rdx*rdx + rdz*rdz); rdx /= rl; rdz /= rl;
        float worst = 1e9f;
        for (float t = -150.0f; t <= 150.0f; t += 10.0f) {
            const float px = p.cx + rdx * t, pz = p.cz + rdz * t;
            const float wy = worldWaterLevelAt(px, pz);
            if (wy <= kWorldWaterDry + 1.0f) continue;
            for (int side = -1; side <= 1; side += 2) {
                // crest = highest ground over the levee band beside the ribbon
                float crest = -1e9f;
                for (float l = 34.5f; l <= 60.0f; l += 2.5f)
                    crest = std::max(crest, terrainHeightAtWorld(
                        px + p.dirX * l * side, pz + p.dirZ * l * side));
                worst = std::min(worst, crest - wy);
            }
        }
        std::snprintf(d, sizeof(d), "lowest bank crest %.2f ft above its water", worst * kMToFt);
        check(worst > 0.0f, "RB5 the levee holds through the bridge reach", d);
    }

    // RB6 — grades and pinned datums: the deck is reached at grade, level, and
    // the step at each end of the structure is zero by construction.
    std::snprintf(d, sizeof(d), "max grade %.1f%%, pin deficit %.2f ft, float %.1f ft",
                  rr.road.maxGradePct, rr.road.pinErrM * kMToFt,
                  rr.road.maxFloatM * kMToFt);
    check(rr.road.maxGradePct <= 7.5f && rr.road.pinErrM <= 0.06f &&
          rr.road.maxFloatM <= 8.0f,
          "RB6 the approaches reach the deck at grade", d);

    // =======================================================================
    // THE STATION SWEEP (W-WATER, task #32). RB5 above proves the levee holds
    // for 500 ft around the crossing — but the FLOOD that started this lane
    // was 300 m downstream of it: the drawn Gerstner plane was FLAT at the
    // bridge's waterY while the channel descends ~1.2 m per node, so far from
    // the bridge the painted surface stood over the banks and a bench shipped
    // submerged at (-340, 11, -468). A gate that only looks at the bridge
    // cannot see that. RB8-RB10 walk the WHOLE carved run.
    // =======================================================================

    // The DRAWN surface, computed the way water.vert computes it: closest
    // approach to the node polyline the host feeds the shader
    // (worldRiverRisenNodes — the risen table under rain), lerping waterY
    // along the closest segment. This is deliberately an INDEPENDENT
    // re-implementation of the query: if the drawn plane and worldWaterLevelAt
    // ever split again (the whole defect), RB10 catches it.
    auto drawnSurface = [](const WorldRiverNode* rn, uint32_t n,
                           float x, float z, float& outDist) {
        float best = 1e30f, lvl = n ? rn[0].waterY : 0.0f;
        for (uint32_t i = 0; i + 1 < n; ++i) {
            const float ax = rn[i].x,   az = rn[i].z;
            const float bx = rn[i+1].x, bz = rn[i+1].z;
            const float ex = bx - ax,   ez = bz - az;
            const float l2 = std::max(ex*ex + ez*ez, 1e-6f);
            float t = ((x - ax) * ex + (z - az) * ez) / l2;
            t = std::clamp(t, 0.0f, 1.0f);
            const float dx = x - (ax + ex * t), dz = z - (az + ez * t);
            const float d2 = dx*dx + dz*dz;
            if (d2 < best) { best = d2; lvl = rn[i].waterY + (rn[i+1].waterY - rn[i].waterY) * t; }
        }
        outDist = std::sqrt(best);
        return lvl;
    };

    // Walk the carved run and report, per station, how much freeboard the bank
    // crest has over the DRAWN water. Returns the worst (smallest) margin and
    // logs the worst station. `label` names the weather state.
    auto sweep = [&](const char* label, float& worstOut,
                     float& worstX, float& worstZ, float& worstWY) {
        WorldRiverNode risen[64];
        const uint32_t n = worldRiverRisenNodes(risen, 64);
        const uint32_t carve = std::min(worldRiverCarveCount(), n);
        worstOut = 1e9f; worstX = worstZ = worstWY = 0.0f;
        uint32_t stations = 0;
        for (uint32_t i = 0; i + 1 < carve; ++i) {
            float tx = risen[i+1].x - risen[i].x, tz = risen[i+1].z - risen[i].z;
            const float segLen = std::sqrt(tx*tx + tz*tz);
            if (segLen < 1e-3f) continue;
            tx /= segLen; tz /= segLen;
            const float px = -tz, pz = tx;          // left-hand normal
            for (float s = 0.0f; s < segLen; s += 15.0f) {
                const float cx = risen[i].x + tx * s, cz = risen[i].z + tz * s;
                float dist = 0.0f;
                const float wy = drawnSurface(risen, n, cx, cz, dist);
                if (dist > kWorldRiverHalfWidth) continue;   // shader draws nothing here
                // THE ESTUARY IS NOT A RIVER. Inside the ocean basin disc the
                // shader hands the level off to the sea (water.vert's inBasin
                // branch) and the basin floor is 190 ft down — there are no
                // banks to hold, and grading the sea by a river's rule would
                // fail every station. The sweep gates the reach the shader
                // draws as RIVER; the sea is the ocean pass's business.
                {
                    const float bx = cx - kWorldOceanBasinX, bz = cz - kWorldOceanBasinZ;
                    if (bx*bx + bz*bz < kWorldOceanBasinR * kWorldOceanBasinR) continue;
                }
                ++stations;
                for (int side = -1; side <= 1; side += 2) {
                    // The bank: highest ground over the levee band just
                    // outside the ribbon. Water standing above THIS is water
                    // on dry land.
                    float crest = -1e9f;
                    for (float l = kWorldRiverHalfWidth + 0.5f;
                         l <= kWorldRiverHalfWidth + 26.0f; l += 2.5f)
                        crest = std::max(crest,
                            terrainHeightAtWorld(cx + px * l * side, cz + pz * l * side));
                    if (crest - wy < worstOut) {
                        worstOut = crest - wy; worstX = cx; worstZ = cz; worstWY = wy;
                    }
                }
            }
        }
        x3::logInfo("[riverbridge] sweep(" + std::string(label) + "): " +
                    std::to_string(stations) + " stations over " +
                    std::to_string(carve) + " carved nodes; worst bank freeboard " +
                    std::to_string(worstOut * kMToFt) + " ft at (" +
                    std::to_string(worstX) + ", " + std::to_string(worstZ) +
                    "), water Y " + std::to_string(worstWY));
    };

    // RB8 — DRY WEATHER: nowhere on the carved run does the drawn water stand
    // above its bank crest. (Under the old flat plane this failed downstream.)
    float dryWorst = 0, dwx = 0, dwz = 0, dwy = 0;
    {
        setWorldRiverRainRise(0.0f);
        sweep("dry", dryWorst, dwx, dwz, dwy);
        std::snprintf(d, sizeof(d),
                      "worst bank freeboard %.2f ft at (%.0f, %.0f)",
                      dryWorst * kMToFt, dwx, dwz);
        check(dryWorst > 0.0f, "RB8 the drawn river stays inside its banks, whole run", d);
    }

    // RB9 — FULL STORM: the runoff rise is VISIBLE and still bounded by the
    // banks. Both halves matter — a rise nobody can see is not a feature, and
    // a rise that tops a bank is a flood. The rise is measured AT THE CROSSING
    // (where the player stands and the captures are framed), not at the worst
    // dry station: the cap is per-node freeboard, so the tightest bank on the
    // run is exactly where the river is SUPPOSED to barely move.
    {
        setWorldRiverRainRise(0.0f);
        const float dryAtBridge = worldWaterLevelAt(p.cx, p.cz);
        setWorldRiverRainRise(kWorldRiverRainRiseMax);
        const float rise = worldWaterLevelAt(p.cx, p.cz) - dryAtBridge;
        float wetWorst = 0, wx = 0, wz = 0, wwy = 0;
        sweep("storm", wetWorst, wx, wz, wwy);
        std::snprintf(d, sizeof(d),
                      "river up %.2f ft at the crossing, worst bank freeboard still "
                      "%.2f ft at (%.0f, %.0f)",
                      rise * kMToFt, wetWorst * kMToFt, wx, wz);
        check(rise > 0.15f && wetWorst > 0.0f,
              "RB9 heavy rain swells the river VISIBLY and never tops a bank", d);
        setWorldRiverRainRise(0.0f);
        (void)dwy;
    }

    // RB10 — ONE TRUTH (the lane's whole point): the surface the shader draws
    // and the level worldWaterLevelAt reports are the same number everywhere
    // inside the reach, dry AND swollen. The submerged bench happened because
    // these two disagreed by 0.26 m only 32 m from the bridge.
    {
        float worst = 0.0f, wx = 0, wz = 0;
        for (int pass = 0; pass < 2; ++pass) {
            setWorldRiverRainRise(pass ? kWorldRiverRainRiseMax : 0.0f);
            WorldRiverNode risen[64];
            const uint32_t n = worldRiverRisenNodes(risen, 64);
            const uint32_t carve = std::min(worldRiverCarveCount(), n);
            for (uint32_t i = 0; i + 1 < carve; ++i) {
                float tx = risen[i+1].x - risen[i].x, tz = risen[i+1].z - risen[i].z;
                const float segLen = std::sqrt(tx*tx + tz*tz);
                if (segLen < 1e-3f) continue;
                tx /= segLen; tz /= segLen;
                const float px = -tz, pz = tx;
                for (float s = 0.0f; s < segLen; s += 12.0f)
                    for (float l = -30.0f; l <= 30.0f; l += 10.0f) {
                        const float cx = risen[i].x + tx * s + px * l;
                        const float cz = risen[i].z + tz * s + pz * l;
                        float dist = 0.0f;
                        const float drawn = drawnSurface(risen, n, cx, cz, dist);
                        if (dist > kWorldRiverHalfWidth) continue;
                        const float q = worldWaterLevelAt(cx, cz);
                        if (q <= kWorldWaterDry + 1.0f) continue;
                        if (std::fabs(drawn - q) > worst) {
                            worst = std::fabs(drawn - q); wx = cx; wz = cz;
                        }
                    }
            }
        }
        setWorldRiverRainRise(0.0f);
        std::snprintf(d, sizeof(d),
                      "largest drawn-vs-query disagreement %.4f m at (%.0f, %.0f)",
                      worst, wx, wz);
        check(worst < 0.01f,
              "RB10 the drawn surface and worldWaterLevelAt are ONE truth", d);
    }

    // RB7 — determinism: re-register, the carved field answers identically
    // along the whole road.
    {
        auto hash = [&]() {
            uint64_t h = 1469598103934665603ull;
            for (size_t i = 0; i < rr.spec.x.size(); ++i) {
                const float v = terrainCorridorDelta(rr.spec.x[i], rr.spec.z[i]);
                uint32_t bits; std::memcpy(&bits, &v, sizeof(bits));
                h = (h ^ bits) * 1099511628211ull;
            }
            return h;
        };
        const uint64_t h1 = hash();
        clearTerrainCorridors();
        (void)registerRiverRoad();
        const uint64_t h2 = hash();
        std::snprintf(d, sizeof(d), "field hash %016llx", (unsigned long long)h1);
        check(h1 == h2, "RB7 the crossing is deterministic", d);
    }

    clearTerrainCorridors();
    x3::logInfo("[riverbridge] " + std::to_string(passN) + " passed, " +
                std::to_string(failN) + " failed");
    return failN == 0;
}

} // namespace x3::game
