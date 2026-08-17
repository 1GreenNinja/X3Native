// THE GLIMVALE CONFECTIONERY WORKS — see app/factory.h for the whole story
// (siting argument, clean-room rule, art sources, measured GLB sizes).
#include "factory.h"

#include "scene.h"
#include "terrain.h"
#include "river_bridge.h"      // --test-factory hides a ticket on Bridge No.1
#include "asset_root.h"
#include "mesh_prims.h"
#include "lns_shop.h"          // makeSignRGBA (5x7 neon baker) + makeMr1x1
#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace x3::game {

namespace {

constexpr float kMToFt = 3.28084f;

// ---------------------------------------------------------------------------
// MESH BUFFER in the SITE FRAME. Same shape as river_bridge.cpp's MeshBuf (one
// world frame, box + quad, render verts doubling as collision) — deliberately,
// because that one is proven and its winding law is written in blood there:
// the collider shares the render index buffer and Jolt's wheel raycasts cull
// back faces, so a top face wound the wrong way renders fine and drops the car
// through the floor. Top quads here wind CCW seen from above (+Y normals).
// ---------------------------------------------------------------------------
struct SiteFrame {
    float cx = 0, cz = 0, fX = 1, fZ = 0, rX = 0, rZ = 1;
    void world(float a, float b, float& x, float& z) const {
        x = cx + fX * a + rX * b;
        z = cz + fZ * a + rZ * b;
    }
};

struct MeshBuf {
    std::vector<x3::rhi::MeshVertex> v;
    std::vector<uint32_t> i;
    float uvPerM = 0.25f;          // texel density: one tile per 4 m by default

    void quad(const float a[3], const float b[3], const float c[3], const float d[3]) {
        const float e1[3] = { b[0]-a[0], b[1]-a[1], b[2]-a[2] };
        const float e2[3] = { d[0]-a[0], d[1]-a[1], d[2]-a[2] };
        float n[3] = { e1[1]*e2[2]-e1[2]*e2[1], e1[2]*e2[0]-e1[0]*e2[2],
                       e1[0]*e2[1]-e1[1]*e2[0] };
        const float nl = std::sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);
        if (nl > 1e-6f) { n[0]/=nl; n[1]/=nl; n[2]/=nl; }
        // WORLD-SPACE triplanar-ish UVs: pick the two axes least aligned with
        // the normal so a 40 m wall and a 40 m slab get the same texel density
        // and neither stretches. (A 0..1 quad UV would smear a big face.)
        const int ax = (std::fabs(n[0]) > std::fabs(n[1]) && std::fabs(n[0]) > std::fabs(n[2])) ? 0
                     : (std::fabs(n[1]) >= std::fabs(n[2]) ? 1 : 2);
        const int u0 = (ax == 0) ? 2 : 0, u1 = (ax == 1) ? 2 : 1;
        const uint32_t base = (uint32_t)v.size();
        const float* pts[4] = { a, b, c, d };
        for (int k = 0; k < 4; ++k) {
            x3::rhi::MeshVertex mv{};
            mv.pos[0]=pts[k][0]; mv.pos[1]=pts[k][1]; mv.pos[2]=pts[k][2];
            mv.normal[0]=n[0]; mv.normal[1]=n[1]; mv.normal[2]=n[2];
            mv.uv[0]=pts[k][u0]*uvPerM; mv.uv[1]=pts[k][u1]*uvPerM;
            v.push_back(mv);
        }
        i.push_back(base+0); i.push_back(base+1); i.push_back(base+2);
        i.push_back(base+0); i.push_back(base+2); i.push_back(base+3);
    }

    // A box in site coords: a along forward, b lateral, y world.
    void box(const SiteFrame& s, float a0, float a1, float b0, float b1,
             float y0, float y1) {
        float c[8][3];
        auto pt = [&](float aa, float bb, float yy, float o[3]) {
            s.world(aa, bb, o[0], o[2]); o[1] = yy;
        };
        pt(a0,b0,y0,c[0]); pt(a1,b0,y0,c[1]); pt(a1,b1,y0,c[2]); pt(a0,b1,y0,c[3]);
        pt(a0,b0,y1,c[4]); pt(a1,b0,y1,c[5]); pt(a1,b1,y1,c[6]); pt(a0,b1,y1,c[7]);
        quad(c[4],c[5],c[6],c[7]);   // top    (+Y)
        quad(c[3],c[2],c[1],c[0]);   // bottom (-Y)
        quad(c[0],c[1],c[5],c[4]);   // b0 face
        quad(c[2],c[3],c[7],c[6]);   // b1 face
        quad(c[1],c[2],c[6],c[5]);   // a1 face
        quad(c[3],c[0],c[4],c[7]);   // a0 face
    }

    // ONE face with 0..1 UVs — for a panel whose texture is a PICTURE, not a
    // tile. The world-space UVs above are right for concrete and wrong for a
    // wordmark: the first sign shipped reading "GLIMVA GLIMVA GLIMVA GLIMVA"
    // because a 26 m board at 0.25 uv/m wrapped its baked panel four times.
    // Facing the -a direction (the freeway side of the hall).
    void signFace(const SiteFrame& s, float a, float b0, float b1,
                  float y0, float y1) {
        float c[4][3];
        auto pt = [&](float bb, float yy, float o[3]) {
            s.world(a, bb, o[0], o[2]); o[1] = yy; };
        // WINDING: the face normal must point -a (out toward the freeway). The
        // first cut ran b1 -> b0 and the cross product came out +a, i.e. into
        // the building — the sign was there, textured, correctly UV'd, and
        // BACKFACE-CULLED. It cost a capture to notice, which is the whole
        // argument for rule 2 (eyes on, full res) over "it compiled".
        pt(b0, y0, c[0]); pt(b1, y0, c[1]); pt(b1, y1, c[2]); pt(b0, y1, c[3]);
        const float e1[3] = { c[1][0]-c[0][0], c[1][1]-c[0][1], c[1][2]-c[0][2] };
        const float e2[3] = { c[3][0]-c[0][0], c[3][1]-c[0][1], c[3][2]-c[0][2] };
        float nn[3] = { e1[1]*e2[2]-e1[2]*e2[1], e1[2]*e2[0]-e1[0]*e2[2],
                        e1[0]*e2[1]-e1[1]*e2[0] };
        const float nl = std::sqrt(nn[0]*nn[0]+nn[1]*nn[1]+nn[2]*nn[2]);
        if (nl > 1e-6f) { nn[0]/=nl; nn[1]/=nl; nn[2]/=nl; }
        const uint32_t base = (uint32_t)v.size();
        const float us[4] = { 0, 1, 1, 0 }, vs[4] = { 1, 1, 0, 0 };
        for (int k = 0; k < 4; ++k) {
            x3::rhi::MeshVertex mv{};
            mv.pos[0]=c[k][0]; mv.pos[1]=c[k][1]; mv.pos[2]=c[k][2];
            mv.normal[0]=nn[0]; mv.normal[1]=nn[1]; mv.normal[2]=nn[2];
            mv.uv[0]=us[k]; mv.uv[1]=vs[k];
            v.push_back(mv);
        }
        i.push_back(base+0); i.push_back(base+1); i.push_back(base+2);
        i.push_back(base+0); i.push_back(base+2); i.push_back(base+3);
    }

    // A vertical prism (silo / stack): `seg`-sided, radius r, centred (a,b).
    void cylinder(const SiteFrame& s, float a, float b, float r,
                  float y0, float y1, int seg) {
        const uint32_t base = (uint32_t)v.size();
        for (int k = 0; k < seg; ++k) {
            const float t0 = 6.2831853f * (float)k / (float)seg;
            const float t1 = 6.2831853f * (float)(k + 1) / (float)seg;
            float p0[3], p1[3], p2[3], p3[3];
            s.world(a + std::cos(t0)*r, b + std::sin(t0)*r, p0[0], p0[2]); p0[1] = y0;
            s.world(a + std::cos(t1)*r, b + std::sin(t1)*r, p1[0], p1[2]); p1[1] = y0;
            p2[0]=p1[0]; p2[1]=y1; p2[2]=p1[2];
            p3[0]=p0[0]; p3[1]=y1; p3[2]=p0[2];
            quad(p0, p1, p2, p3);
        }
        (void)base;
        // Cap: a triangle fan wound CCW from above so a body can rest on it.
        float ctr[3]; s.world(a, b, ctr[0], ctr[2]); ctr[1] = y1;
        for (int k = 0; k < seg; ++k) {
            const float t0 = 6.2831853f * (float)k / (float)seg;
            const float t1 = 6.2831853f * (float)(k + 1) / (float)seg;
            float p0[3], p1[3];
            s.world(a + std::cos(t0)*r, b + std::sin(t0)*r, p0[0], p0[2]); p0[1] = y1;
            s.world(a + std::cos(t1)*r, b + std::sin(t1)*r, p1[0], p1[2]); p1[1] = y1;
            const uint32_t bi = (uint32_t)v.size();
            const float nn[3] = { 0, 1, 0 };
            for (const float* p : { ctr, p1, p0 }) {
                x3::rhi::MeshVertex mv{};
                mv.pos[0]=p[0]; mv.pos[1]=p[1]; mv.pos[2]=p[2];
                mv.normal[0]=nn[0]; mv.normal[1]=nn[1]; mv.normal[2]=nn[2];
                mv.uv[0]=p[0]*uvPerM; mv.uv[1]=p[2]*uvPerM;
                v.push_back(mv);
            }
            i.push_back(bi); i.push_back(bi+1); i.push_back(bi+2);
        }
    }

    // A horizontal pipe run along the `a` axis, `seg`-sided.
    void tubeA(const SiteFrame& s, float a0, float a1, float b, float y,
               float r, int seg) {
        for (int k = 0; k < seg; ++k) {
            const float t0 = 6.2831853f * (float)k / (float)seg;
            const float t1 = 6.2831853f * (float)(k + 1) / (float)seg;
            float p0[3], p1[3], p2[3], p3[3];
            s.world(a0, b + std::cos(t0)*r, p0[0], p0[2]); p0[1] = y + std::sin(t0)*r;
            s.world(a1, b + std::cos(t0)*r, p1[0], p1[2]); p1[1] = y + std::sin(t0)*r;
            s.world(a1, b + std::cos(t1)*r, p2[0], p2[2]); p2[1] = y + std::sin(t1)*r;
            s.world(a0, b + std::cos(t1)*r, p3[0], p3[2]); p3[1] = y + std::sin(t1)*r;
            quad(p0, p1, p2, p3);
        }
    }

    bool empty() const { return i.empty(); }
};

// A column-major model matrix that points the model's LOCAL +X along (dX,dZ).
// (Right-handed Y-up: col0 = (dX,0,dZ) forces col2 = (-dZ,0,dX) for det +1.)
void axisMat(float dX, float dZ, float sX, float sY, float sZ,
             float wx, float wy, float wz, float out[16]) {
    out[0]=sX*dX; out[1]=0;   out[2]=sX*dZ;  out[3]=0;
    out[4]=0;     out[5]=sY;  out[6]=0;      out[7]=0;
    out[8]=-sZ*dZ;out[9]=0;   out[10]=sZ*dX; out[11]=0;
    out[12]=wx;   out[13]=wy; out[14]=wz;    out[15]=1;
}

SurfaceLibrary& factorySurfaces() { static SurfaceLibrary lib; return lib; }

// Deterministic integer hash -> [0,1). No rand(): the works must be identical
// every boot (the placement law road_trees/forest already live under).
inline float hash01(uint32_t& s) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return (float)(s & 0xFFFFFFu) / 16777216.0f;
}

} // namespace

// ===========================================================================
// THE PLAN
// ===========================================================================
FactoryPlan planFactoryWorks(const RoadSpec& freeway,
                             const std::vector<float>& freewayY) {
    FactoryPlan best;
    const size_t n = freeway.x.size();
    if (n < 8 || freewayY.size() != n) {
        best.whyNot = "no freeway spec/datum to site against";
        return best;
    }

    // The tour centre, which the sketch's whole radial geometry is about
    // (ROAD_NETWORK_PLAN.md anchor table: centre (-592, -352) for BOTH tours,
    // so "outward radial" is well defined and the drive arrives square).
    constexpr float kTourCX = -592.0f, kTourCZ = -352.0f;
    // THE NE SECTOR, in the sketch's own compass (+Z north, +X east): the arc
    // between due east and north-north-east, which is the reach that runs past
    // the centre-north forest patch. Outside it the freeway is either in the
    // southern woods belt or round the back of the ranges.
    constexpr float kSectorLoDeg = 26.0f, kSectorHiDeg = 74.0f;
    // How far off the pavement the works stands. Near enough to fill the
    // windscreen at speed, far enough that the drive is a drive.
    constexpr float kOffsetM = 330.0f;

    // The median plan, once — a branch off a DIVIDED freeway meets the near
    // carriageway, not the centreline (registerSpawnConnector's measurement).
    const std::vector<float> mplan = freeway.dualCarriageway
        ? computeMedianPlan(freeway, freewayY) : std::vector<float>{};

    const float padHalfA = (kFacPadA1 - kFacPadA0) * 0.5f;
    const float padHalfB = (kFacPadB1 - kFacPadB0) * 0.5f;

    float bestScore = 1e18f;
    uint32_t considered = 0, rejWater = 0, rejCorridor = 0, rejGrade = 0;

    for (size_t k = 0; k + 1 < n; ++k) {
        const float dx = freeway.x[k] - kTourCX, dz = freeway.z[k] - kTourCZ;
        const float rad = std::sqrt(dx*dx + dz*dz);
        if (rad < 100.0f) continue;
        float deg = std::atan2(dz, dx) * 57.29578f;
        if (deg < 0.0f) deg += 360.0f;
        if (deg < kSectorLoDeg || deg > kSectorHiDeg) continue;
        ++considered;

        // Outward radial: away from the tour centre. The pad centre sits
        // kOffsetM out; `f` points from the road INTO the site.
        const float fX = dx / rad, fZ = dz / rad;
        const float rX = -fZ, rZ = fX;
        SiteFrame sf; sf.fX = fX; sf.fZ = fZ; sf.rX = rX; sf.rZ = rZ;
        sf.cx = freeway.x[k] + fX * kOffsetM;
        sf.cz = freeway.z[k] + fZ * kOffsetM;

        // ---- sample the footprint (7 x 7 over the pad + a 6 m margin) -----
        float lo = 1e18f, hi = -1e18f;
        bool wet = false, onRoad = false;
        for (int ia = 0; ia <= 6 && !wet && !onRoad; ++ia) {
            for (int ib = 0; ib <= 6; ++ib) {
                const float a = -padHalfA - 6.0f + (padHalfA + 6.0f) * 2.0f * (float)ia / 6.0f;
                const float b = -padHalfB - 6.0f + (padHalfB + 6.0f) * 2.0f * (float)ib / 6.0f;
                float wx, wz; sf.world(a, b, wx, wz);
                const float h = terrainHeightAtWorld(wx, wz);
                lo = std::min(lo, h); hi = std::max(hi, h);
                if (h <= worldWaterLevelAt(wx, wz) + 0.6f) { wet = true; break; }
                // A works pad ON a registered corridor would be a building in
                // the middle of somebody's road. terrainCorridorContains covers
                // every route AND every bore in the world.
                if (terrainCorridorContains(wx, wz)) { onRoad = true; break; }
            }
        }
        if (wet)    { ++rejWater;    continue; }
        if (onRoad) { ++rejCorridor; continue; }

        // The platform: top clear of the highest ground under it. 0.45 m of
        // freeboard is a kerb's worth — enough that the slab reads as poured
        // ON the ground rather than sunk into it.
        const float padY  = hi + 0.45f;
        const float jy    = freewayY[k];
        // Near-carriageway edges (the drive must start OUTSIDE the freeway).
        float mainShoulderEdge = kShoulderHalfM, mainPavedEdge = kPavedHalfM;
        if (freeway.dualCarriageway) {
            const float mJ = (k < mplan.size()) ? mplan[k] : kFwyMedianMinHalfM;
            mainShoulderEdge = mJ + kFwyPavedHalfM + kFwyShoulderHalfM;
            mainPavedEdge    = mJ + 2.0f * kFwyPavedHalfM;
        }
        // The drive runs from the junction setback out to the gate.
        const float gateA   = kFacPadA0;                       // pad's near edge
        const float setback = mainPavedEdge + 40.0f;
        const float driveLen = (kOffsetM + gateA) - setback;
        if (driveLen < 60.0f) { ++rejGrade; continue; }
        const float gradePct = std::fabs(padY - jy) / driveLen * 100.0f;
        // The freeway's own class ceiling. A drive that cannot be built at
        // 7% is a drive that would arrive as a ramp, so the site loses.
        if (gradePct > 7.0f) { ++rejGrade; continue; }

        // SCORE: relief first (a flat site is a buildable site), then the
        // platform's exposed face, then the climb. All in metres, so the
        // weights are honest ratios and not magic.
        const float relief  = hi - lo;
        const float exposed = padY - lo;
        const float score   = relief * 2.0f + exposed * 1.0f + gradePct * 3.0f;
        if (score >= bestScore) continue;

        bestScore = score;
        best.ok = true;
        best.cx = sf.cx; best.cz = sf.cz;
        best.fX = fX; best.fZ = fZ; best.rX = rX; best.rZ = rZ;
        best.padY = padY; best.padLowY = lo;
        best.exposedM = exposed; best.reliefM = relief;
        best.fwyNode = (uint32_t)k;
        best.jx = freeway.x[k]; best.jz = freeway.z[k]; best.jy = jy;
        best.mainShoulderEdgeM = mainShoulderEdge;
        best.mainPavedEdgeM    = mainPavedEdge;
        best.offsetM = kOffsetM;
        best.driveGradePct = gradePct;
        float gx, gz; sf.world(gateA, 0.0f, gx, gz);
        best.gateX = gx; best.gateZ = gz;
    }

    if (!best.ok) {
        best.whyNot = "no NE-sector site is flat, dry, off every corridor and "
                      "reachable at 7%";
        char b[220];
        std::snprintf(b, sizeof(b),
            "factory: NO SITE — %u NE-arc candidates, %u wet, %u on a road, "
            "%u unreachable at grade", considered, rejWater, rejCorridor, rejGrade);
        x3::logWarn(b);
        return best;
    }

    // The freeway's frame at the landing (the mouth patch needs it).
    const size_t J = best.fwyNode;
    const size_t jp = (J + 1 < n - 1) ? J + 1 : 0;
    const size_t jm = (J > 0) ? J - 1 : n - 2;
    float mtx = freeway.x[jp] - freeway.x[jm], mtz = freeway.z[jp] - freeway.z[jm];
    const float mtl = std::sqrt(mtx*mtx + mtz*mtz);
    if (mtl > 1e-4f) {
        best.mainTX = mtx / mtl; best.mainTZ = mtz / mtl;
        best.mainGrade = (freewayY[jp] - freewayY[jm]) / mtl;
    }

    char b[420];
    std::snprintf(b, sizeof(b),
        "factory: THE GLIMVALE WORKS sited at (%.0f, %.0f) — NE arc, tour node %u "
        "(%.0f, %.0f), %.0f m off the freeway | platform %.1f ft, relief %.1f ft "
        "over 130x120 m, exposed face %.1f ft, drive %.2f%% | %u candidates "
        "(%u wet, %u on a road, %u too steep)",
        best.cx, best.cz, best.fwyNode, best.jx, best.jz, best.offsetM,
        best.padY * kMToFt, best.reliefM * kMToFt, best.exposedM * kMToFt,
        best.driveGradePct, considered, rejWater, rejCorridor, rejGrade);
    x3::logInfo(b);
    return best;
}

// ===========================================================================
// THE DRIVE
// ===========================================================================
FactoryDriveResult registerFactoryDrive(const FactoryPlan& plan,
                                        const RoadSpec& freeway,
                                        const std::vector<float>& freewayY) {
    FactoryDriveResult out;
    if (!plan.ok) return out;

    // Straight in on the outward radial — a works drive IS straight; the only
    // curvature is the swoop the junction mouth puts on the merge. Node 0 sits
    // at the freeway setback, the last node at the gate.
    const float setback = plan.mainPavedEdgeM + 40.0f;
    const float gateOff = plan.offsetM + kFacPadA0;   // gate distance from the tour line
    std::vector<CourseWaypoint> wp;
    wp.push_back({ plan.jx + plan.fX * setback, plan.jz + plan.fZ * setback, 200.0f });
    wp.push_back({ plan.jx + plan.fX * gateOff, plan.jz + plan.fZ * gateOff, 200.0f });
    RoadSpec s = makeRoadFromWaypoints("factory drive", wp, 24.0f, /*closed=*/false);
    s.maxGrade        = 0.07f;
    s.minTurnRadiusM  = 200.0f;
    s.maxDeflectionDeg = 3.0f;
    smoothHorizontalCurves(s);
    if (s.x.size() < 3) return out;

    // BOTH ENDS PINNED. The landing takes the freeway's graded datum so the
    // mouth does not step; the gate end takes the PLATFORM top so the pavement
    // meets the forecourt flush instead of onto its lip. If the relaxation
    // cannot hold either, registerRoad reports it in pinErrM — loudly.
    const float kNaN = std::numeric_limits<float>::quiet_NaN();
    s.pinY.assign(s.x.size(), kNaN);
    s.pinY.front() = plan.jy;
    s.pinY.back()  = plan.padY;

    out.spec = s;
    out.road = registerRoad(out.spec, &out.roadY);
    if (!out.road.ok || out.roadY.empty()) {
        x3::logError("factory drive: registration FAILED");
        return out;
    }
    registerRoadJunctionThroat(out.spec.x.front(), out.spec.z.front(),
                               plan.jx, plan.jz, plan.jy);
    out.jct.valid  = true;
    out.jct.jx = plan.jx; out.jct.jz = plan.jz; out.jct.jy = plan.jy;
    out.jct.mainTX = plan.mainTX; out.jct.mainTZ = plan.mainTZ;
    out.jct.mainGrade = plan.mainGrade;
    out.jct.endX = out.spec.x.front();
    out.jct.endZ = out.spec.z.front();
    out.jct.endY = out.roadY.front();
    out.jct.mainShoulderEdgeM = plan.mainShoulderEdgeM;
    out.jct.mainPavedEdgeM    = plan.mainPavedEdgeM;
    out.lengthM = out.road.lengthM;
    out.ok = true;

    char b[320];
    std::snprintf(b, sizeof(b),
        "factory drive: %.0f m off the freeway at tour node %u — %u nodes, "
        "max grade %.1f%%, pin deficit %.2f ft (landing datum %.1f, gate datum %.1f)",
        out.road.lengthM, plan.fwyNode, out.road.nodeCount, out.road.maxGradePct,
        out.road.pinErrM * kMToFt, out.roadY.front(), out.roadY.back());
    x3::logInfo(b);
    return out;
}

// ===========================================================================
// THE WORKS
// ===========================================================================
void FactoryWorks::gatePoint(float out[3]) const {
    out[0] = m_plan.gateX; out[1] = m_plan.padY; out[2] = m_plan.gateZ;
}

bool FactoryWorks::build(Scene& scene, x3::rhi::IRenderDevice& device,
                         x3::phys::IPhysicsWorld& phys, const FactoryPlan& plan) {
    if (!plan.ok) { x3::logWarn("factory: no plan — nothing built"); return false; }
    m_plan = plan;

    SiteFrame sf;
    sf.cx = plan.cx; sf.cz = plan.cz;
    sf.fX = plan.fX; sf.fZ = plan.fZ; sf.rX = plan.rX; sf.rZ = plan.rZ;
    const float padY = plan.padY;

    SurfaceLibrary& surf = factorySurfaces();
    surf.mount(assetRoot() + "/surface_library");
    // ONE WORLD, ONE CONCRETE: the same family the bridge piers and the tunnel
    // portals wear, so the works does not read as a visitor from another game.
    const SurfaceSet& concrete = surf.get(device, "mw_concrete_panels_a");
    const SurfaceSet& yardCem  = surf.get(device, "sr_concrete_01");
    const SurfaceSet& cladding = surf.get(device, "mw_metal_panels_a");
    const SurfaceSet& enamel   = surf.get(device, "cc_cement_white");
    const SurfaceSet& steel    = surf.get(device, "sr_metal_b");

    auto upload = [&](MeshBuf& m, const SurfaceSet* set, const float tint[4],
                      bool collide) -> uint32_t {
        if (m.empty()) return 0xFFFFFFFFu;
        Entity e;
        e.mesh = device.createMesh(m.v.data(), (uint32_t)m.v.size(),
                                   m.i.data(), (uint32_t)m.i.size());
        if (!e.mesh.valid()) return 0xFFFFFFFFu;
        if (set && set->ok) { e.tex = set->albedo; e.mrTex = set->mr; e.normalTex = set->normal; }
        for (int c = 0; c < 4; ++c) e.baseColor[c] = tint[c];
        const uint32_t id = scene.add(e);
        ++m_meshCount; m_triCount += (uint32_t)(m.i.size() / 3);
        if (collide) {
            std::vector<float> cv; cv.reserve(m.v.size() * 3);
            for (const auto& vv : m.v) { cv.push_back(vv.pos[0]); cv.push_back(vv.pos[1]); cv.push_back(vv.pos[2]); }
            phys.addStaticMesh(cv.data(), (uint32_t)(cv.size() / 3),
                               m.i.data(), (uint32_t)m.i.size());
        }
        return id;
    };
    const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    const float pale [4] = { 0.90f, 0.88f, 0.84f, 1.0f };
    const float cream[4] = { 1.00f, 0.96f, 0.86f, 1.0f };
    const float slate[4] = { 0.62f, 0.64f, 0.68f, 1.0f };

    // ---- 1) THE PLATFORM ---------------------------------------------------
    // Top slab at padY, skirt extruded down past the LOWEST carved ground under
    // the footprint and lapped 1.5 m beneath it — the road prism's law, applied
    // to a building pad: a bare slab reads as paper the moment the ground falls
    // away from its edge.
    {
        MeshBuf pad; pad.uvPerM = 0.22f;
        float lowest = 1e18f;
        for (int ia = 0; ia <= 8; ++ia)
            for (int ib = 0; ib <= 8; ++ib) {
                const float a = kFacPadA0 + (kFacPadA1-kFacPadA0) * (float)ia/8.0f;
                const float b = kFacPadB0 + (kFacPadB1-kFacPadB0) * (float)ib/8.0f;
                float wx, wz; sf.world(a, b, wx, wz);
                lowest = std::min(lowest, terrainHeightAtWorld(wx, wz));
            }
        pad.box(sf, kFacPadA0, kFacPadA1, kFacPadB0, kFacPadB1, lowest - 1.5f, padY);
        upload(pad, &yardCem, pale, true);
    }

    // ---- 2) THE PRODUCTION HALL -------------------------------------------
    // Concrete base storey + clad upper + a north-light SAWTOOTH roof. The
    // sawtooth is what makes it read as a WORKS and not a warehouse box: five
    // teeth, glazed on the shaded face, stepped along the b axis.
    {
        MeshBuf base, upper, roof;
        base.uvPerM = 0.20f; upper.uvPerM = 0.16f; roof.uvPerM = 0.20f;
        base.box(sf, kFacHallA0, kFacHallA1, kFacHallB0, kFacHallB1, padY, padY + 5.4f);
        upper.box(sf, kFacHallA0 + 0.25f, kFacHallA1 - 0.25f,
                  kFacHallB0 + 0.25f, kFacHallB1 - 0.25f, padY + 5.4f, padY + kFacHallH);
        const int teeth = 5;
        const float tw = (kFacHallB1 - kFacHallB0) / (float)teeth;
        for (int t = 0; t < teeth; ++t) {
            const float b0 = kFacHallB0 + tw * (float)t;
            // Solid sloping mass + the vertical riser that carries the glazing.
            roof.box(sf, kFacHallA0, kFacHallA1, b0, b0 + tw * 0.62f,
                     padY + kFacHallH, padY + kFacHallH + kFacSawH * 0.35f);
            roof.box(sf, kFacHallA0, kFacHallA1, b0 + tw * 0.62f, b0 + tw,
                     padY + kFacHallH, padY + kFacHallH + kFacSawH);
        }
        upload(base,  &concrete, pale,  true);
        upload(upper, &cladding, slate, true);
        upload(roof,  &cladding, slate, true);
    }

    // ---- 3) THE HOTHOUSE — the glass mass ---------------------------------
    // The owner's brief is "glassy", and X3_WORLD_RULES rule 5 says how: a
    // near-black BACKING box first (or the glass reads as a hole in the world),
    // then real translucent panes over it (Entity::transparent -> the
    // post-opaque glass pass), then warm interior bands showing THROUGH so the
    // volume has something inside it to look at.
    {
        MeshBuf backing, frame;
        backing.uvPerM = 0.2f; frame.uvPerM = 0.3f;
        backing.box(sf, kFacGlassA0 + 0.6f, kFacGlassA1 - 0.6f,
                    kFacGlassB0 + 0.6f, kFacGlassB1 - 0.6f, padY, padY + kFacGlassH);
        const float nearBlack[4] = { 0.055f, 0.05f, 0.06f, 1.0f };
        upload(backing, &concrete, nearBlack, true);

        // Mullion grid: floor bands every 6 m + corner piers. Slim, so the
        // glass reads as glazing and not as a window punched in a wall.
        for (float y = padY; y < padY + kFacGlassH - 0.1f; y += 6.0f)
            frame.box(sf, kFacGlassA0, kFacGlassA1, kFacGlassB0, kFacGlassB1,
                      y - 0.18f, y + 0.18f);
        frame.box(sf, kFacGlassA0, kFacGlassA1, kFacGlassB0, kFacGlassB1,
                  padY + kFacGlassH - 0.5f, padY + kFacGlassH + 0.9f);   // crown
        for (int c = 0; c < 4; ++c) {
            const float a = (c & 1) ? kFacGlassA1 - 0.55f : kFacGlassA0;
            const float b = (c & 2) ? kFacGlassB1 - 0.55f : kFacGlassB0;
            frame.box(sf, a, a + 0.55f, b, b + 0.55f, padY, padY + kFacGlassH + 0.9f);
        }
        // MULLIONS ARE DARK. The first cut wore the cream enamel and the whole
        // tower read as a pale slab with cream stripes — the mullion grid was
        // the brightest thing in it. Glazing bars in front of glass are almost
        // always the DARKEST line in the elevation; that contrast is most of
        // what makes a curtain wall read as glass at all.
        const float bar[4] = { 0.30f, 0.31f, 0.33f, 1.0f };
        upload(frame, &steel, bar, false);

        // THE PANES. One entity per face, drawn in the glass pass. Opacity is
        // low (this is a hothouse, not spandrel) with a warm tint; the backing
        // behind it is what keeps it from looking like a cut-out.
        for (int face = 0; face < 4; ++face) {
            MeshBuf pane; pane.uvPerM = 0.12f;
            const float in = 0.42f;
            if (face == 0) pane.box(sf, kFacGlassA0, kFacGlassA0 + in, kFacGlassB0, kFacGlassB1, padY, padY + kFacGlassH);
            if (face == 1) pane.box(sf, kFacGlassA1 - in, kFacGlassA1, kFacGlassB0, kFacGlassB1, padY, padY + kFacGlassH);
            if (face == 2) pane.box(sf, kFacGlassA0, kFacGlassA1, kFacGlassB0, kFacGlassB0 + in, padY, padY + kFacGlassH);
            if (face == 3) pane.box(sf, kFacGlassA0, kFacGlassA1, kFacGlassB1 - in, kFacGlassB1, padY, padY + kFacGlassH);
            Entity e;
            e.mesh = device.createMesh(pane.v.data(), (uint32_t)pane.v.size(),
                                       pane.i.data(), (uint32_t)pane.i.size());
            if (!e.mesh.valid()) continue;
            e.transparent = true;
            // MEASURED BEFORE, PAID FOR AGAIN. facility_exterior.cpp's facade
            // audit found glass.frag's split-sum environment reflection driving
            // an architectural pane to sRGB 161 — "flat beige stripes" — and the
            // fix was a dark tint with the specular pulled right down. The first
            // cut here ignored that (tint 0.86, specular 0.45) and the Hothouse
            // rendered as a WHITE SLAB: the single brightest object in the
            // frame, and the exact opposite of "glassy".
            //
            // So: a green-bottle tint at low specular, a little more opacity
            // than a canopy needs, and the warm interior bands behind it doing
            // the work. Lighter than that facade's near-opaque black spandrel
            // (0.90 / 0.15) on purpose — this is a glasshouse you are meant to
            // see into, not a curtain wall you are meant to see yourself in.
            e.glass.opacity    = 0.46f;
            e.glass.refraction = 0.02f;
            e.glass.roughness  = 0.14f;
            e.glass.specular   = 0.16f;
            e.glass.tint[0] = 0.34f; e.glass.tint[1] = 0.46f; e.glass.tint[2] = 0.40f;
            scene.add(e);
            ++m_meshCount; m_triCount += (uint32_t)(pane.i.size() / 3);
        }
        // What is inside: warm working light, in bands, TEXTURE-GATED nowhere
        // because these are geometry (rule 5's clip only bites flat emissive
        // above ~0.5 — these sit at 0.42 and read as lit floors, not slabs).
        MeshBuf glow;
        for (int fl = 1; fl <= 6; ++fl) {
            const float y = padY + (float)fl * 6.0f;
            glow.box(sf, kFacGlassA0 + 1.2f, kFacGlassA1 - 1.2f,
                     kFacGlassB0 + 1.2f, kFacGlassB1 - 1.2f, y - 0.30f, y + 0.05f);
        }
        if (!glow.empty()) {
            Entity e;
            e.mesh = device.createMesh(glow.v.data(), (uint32_t)glow.v.size(),
                                       glow.i.data(), (uint32_t)glow.i.size());
            if (e.mesh.valid()) {
                e.baseColor[0]=0.26f; e.baseColor[1]=0.16f; e.baseColor[2]=0.06f;
                e.emissive[0]=1.00f; e.emissive[1]=0.58f; e.emissive[2]=0.22f;
                e.emissive[3]=0.48f;   // still under the ~0.5 ACES clip (rule 5)
                scene.add(e);
                ++m_meshCount; m_triCount += (uint32_t)(glow.i.size() / 3);
            }
        }
    }

    // ---- 4) THE SILOS ------------------------------------------------------
    {
        MeshBuf silos, skirts;
        silos.uvPerM = 0.18f; skirts.uvPerM = 0.25f;
        for (int k = 0; k < kFacSiloCount; ++k) {
            const float b = -40.0f + 14.0f * (float)k;
            silos.cylinder(sf, 62.0f, b, kFacSiloR, padY + 5.0f, padY + kFacSiloH, 20);
            silos.cylinder(sf, 62.0f, b, kFacSiloR * 0.55f, padY + kFacSiloH,
                           padY + kFacSiloH + 3.2f, 16);            // the cone/head
            skirts.cylinder(sf, 62.0f, b, kFacSiloR * 0.42f, padY, padY + 5.0f, 12);
        }
        upload(silos,  &enamel, cream, true);
        upload(skirts, &steel,  slate, true);
    }

    // ---- 5) THE PIPEWORK — "shiny and cool and alive and breathing" -------
    // Two layers, because the brief asks for two things at once. STEEL runs
    // (pack GLBs below + these procedural gantry legs) carry the industry;
    // GLASS TUBES with a lit core carry the confection, and it is the core
    // that pulses. Rule 5 again: the pulse peaks at 0.46, under the ACES clip,
    // so it reads as something FLOWING and never as a white bar.
    {
        MeshBuf legs; legs.uvPerM = 0.3f;
        const float bridgeY = padY + 24.0f;
        for (float a = 22.0f; a <= 62.0f; a += 10.0f) {
            legs.box(sf, a - 0.35f, a + 0.35f, -12.0f, -11.3f, padY, bridgeY);
            legs.box(sf, a - 0.35f, a + 0.35f,  -6.7f,  -6.0f, padY, bridgeY);
        }
        legs.box(sf, 20.0f, 63.0f, -12.4f, -5.6f, bridgeY, bridgeY + 0.35f);  // walkway deck
        upload(legs, &steel, slate, true);

        // Three glass tubes riding the bridge, each with its own core.
        for (int t = 0; t < 3; ++t) {
            const float b = -11.4f + 2.0f * (float)t;
            MeshBuf shell; shell.uvPerM = 0.5f;
            shell.tubeA(sf, 18.0f, 63.0f, b, bridgeY + 1.5f, 0.52f, 14);
            Entity g;
            g.mesh = device.createMesh(shell.v.data(), (uint32_t)shell.v.size(),
                                       shell.i.data(), (uint32_t)shell.i.size());
            if (g.mesh.valid()) {
                g.transparent = true;
                g.glass.opacity = 0.20f; g.glass.roughness = 0.04f;
                g.glass.specular = 0.85f; g.glass.refraction = 0.05f;
                g.glass.tint[0]=0.94f; g.glass.tint[1]=0.97f; g.glass.tint[2]=1.0f;
                scene.add(g);
                ++m_meshCount; m_triCount += (uint32_t)(shell.i.size() / 3);
            }
            MeshBuf core; core.uvPerM = 0.5f;
            core.tubeA(sf, 18.2f, 62.8f, b, bridgeY + 1.5f, 0.34f, 12);
            Entity c;
            c.mesh = device.createMesh(core.v.data(), (uint32_t)core.v.size(),
                                       core.i.data(), (uint32_t)core.i.size());
            if (c.mesh.valid()) {
                c.baseColor[0]=0.22f; c.baseColor[1]=0.09f; c.baseColor[2]=0.03f;
                c.emissive[0]=1.00f; c.emissive[1]=0.46f; c.emissive[2]=0.14f;
                c.emissive[3]=0.24f;
                const uint32_t id = scene.add(c);
                ++m_meshCount; m_triCount += (uint32_t)(core.i.size() / 3);
                m_tubes.push_back({ id, (float)t * 2.1f });
            }
        }
    }

    // ---- 6) THE MACHINERY SHAKE -------------------------------------------
    // "and the machinery shaking as it does its thing". Four plant blocks on
    // the yard, each with a small running wobble driven in update().
    {
        for (int k = 0; k < 4; ++k) {
            MeshBuf mach; mach.uvPerM = 0.35f;
            const float a = 56.0f, b = 6.0f + 11.0f * (float)k;
            mach.box(sf, a - 3.0f, a + 3.0f, b - 2.4f, b + 2.4f, padY, padY + 4.2f);
            mach.box(sf, a - 1.1f, a + 1.1f, b - 1.1f, b + 1.1f, padY + 4.2f, padY + 6.4f);
            const uint32_t id = upload(mach, &steel, slate, true);
            if (id != 0xFFFFFFFFu) {
                m_shakers.push_back(id);
                const Entity& e = scene.get(id);
                for (int q = 0; q < 16; ++q) m_shakerBase.push_back(e.transform[q]);
            }
        }
    }

    // ---- 7) THE SIGN — texture-gated emissive over near-black -------------
    // X3_WORLD_RULES rule 5, verbatim: "the durable glow is texture-gated
    // emissiveTex at ~1.1 over a near-black albedo — bright texels bloom, dark
    // stays dark". makeSignRGBA bakes exactly that panel (dark field, hot
    // letters); the MR texel forces the PBR route, which is the ONLY route that
    // honours emissiveTex at all (the emissive-path draw has no such argument).
    {
        auto px = lns::makeSignRGBA(512, 96, "GLIMVALE", 1.0f, 0.62f, 0.22f);
        auto mr = lns::makeMr1x1(200, 0);
        x3::rhi::TextureHandle tex = device.createTexture(px.data(), 512, 96, true);
        x3::rhi::TextureHandle mrt = device.createTexture(mr.data(), 1, 1, false);
        MeshBuf back, sign;
        // A shallow board to stand it off the wall (world UVs, concrete), then
        // the wordmark itself as ONE 0..1-UV face on its front.
        back.uvPerM = 0.3f;
        back.box(sf, kFacHallA0 - 0.55f, kFacHallA0 - 0.12f, -44.6f, -17.4f,
                 padY + 12.2f, padY + 17.4f);
        upload(back, &steel, slate, false);
        // 26 m of letters at 13 m up, on the hall's freeway-facing gable.
        sign.signFace(sf, kFacHallA0 - 0.58f, -44.0f, -18.0f,
                      padY + 12.6f, padY + 17.0f);
        Entity e;
        e.mesh = device.createMesh(sign.v.data(), (uint32_t)sign.v.size(),
                                   sign.i.data(), (uint32_t)sign.i.size());
        if (e.mesh.valid() && tex.valid()) {
            e.tex = tex; e.mrTex = mrt; e.emissiveTex = tex;
            e.baseColor[0]=1.0f; e.baseColor[1]=1.0f; e.baseColor[2]=1.0f;
            e.emissive[0]=1.0f; e.emissive[1]=0.68f; e.emissive[2]=0.30f;
            e.emissive[3]=1.15f;
            scene.add(e);
            ++m_meshCount; m_triCount += (uint32_t)(sign.i.size() / 3);
        }
    }

    // ---- 8) PACK KIT: chimneys, gate, fence, yard dressing -----------------
    m_assets.reset(x3::asset::createAssetSource());
    const std::string glbRoot = convertedGlbRoot();
    const bool haveKit = m_assets && m_assets->mountDir(glbRoot, 0);
    if (haveKit) m_loader.reset(x3::asset::createModelLoader(&device, m_assets.get()));

    // One cached load per distinct GLB; instances share the device meshes.
    struct Loaded { x3::asset::Model model; std::vector<x3::asset::ModelDrawable> draws; };
    std::vector<std::pair<std::string, Loaded>> cache;
    auto get = [&](const char* rel) -> Loaded* {
        if (!m_loader) return nullptr;
        for (auto& kv : cache) if (kv.first == rel) return kv.second.model.ok ? &kv.second : nullptr;
        Loaded L; L.model = m_loader->load(rel);
        if (L.model.ok) L.draws = x3::asset::makeDrawables(L.model);
        cache.emplace_back(rel, std::move(L));
        return cache.back().second.model.ok ? &cache.back().second : nullptr;
    };
    // Place a GLB with its LOCAL +X along the site direction (dA, dB).
    auto place = [&](const char* rel, float a, float b, float y,
                     float dA, float dB, float scale) -> uint32_t {
        Loaded* L = get(rel);
        if (!L) return 0xFFFFFFFFu;
        const float dX = plan.fX * dA + plan.rX * dB;
        const float dZ = plan.fZ * dA + plan.rZ * dB;
        const float dl = std::sqrt(dX*dX + dZ*dZ);
        float wx, wz; sf.world(a, b, wx, wz);
        float obj[16];
        axisMat(dl > 1e-5f ? dX/dl : 1.0f, dl > 1e-5f ? dZ/dl : 0.0f,
                scale, scale, scale, wx, y, wz, obj);
        uint32_t first = 0xFFFFFFFFu;
        for (const auto& d : L->draws) {
            if (!d.meshId) continue;
            float fin[16];
            x3::asset::mulMat4(obj, d.nodeTransform, fin);
            Entity e;
            e.mesh        = x3::rhi::MeshHandle{ d.meshId };
            e.tex         = x3::rhi::TextureHandle{ d.baseColorTexId };
            e.normalTex   = x3::rhi::TextureHandle{ d.normalTexId };
            e.mrTex       = x3::rhi::TextureHandle{ d.mrTexId };
            e.emissiveTex = x3::rhi::TextureHandle{ d.emissiveTexId };
            for (int q = 0; q < 4; ++q) e.baseColor[q] = d.baseColorFactor[q];
            e.alphaBlend = d.alphaBlend;
            for (int q = 0; q < 16; ++q) e.transform[q] = fin[q];
            e.tag = (uint32_t)Tag::Prop;
            const uint32_t id = scene.add(e);
            if (first == 0xFFFFFFFFu) first = id;
            ++m_propCount;
        }
        return first;
    };

    // THE CHIMNEYS — 61.11 m of measured pack geometry, origin on the base, so
    // placement Y is simply the platform top (rule 4, nothing to correct).
    // Behind the hall, so the skyline read is stack-over-roof from the road.
    {
        const float stack[2][2] = { { 64.0f, -34.0f }, { 66.0f, -14.0f } };
        const char* which[2] = { "Factory/sm_Chimney_01_01.glb",
                                 "Factory/sm_Chimney_01_02.glb" };
        for (int k = 0; k < 2; ++k) {
            place(which[k], stack[k][0], stack[k][1], padY, 1.0f, 0.0f, 1.0f);
            float wx, wz; sf.world(stack[k][0], stack[k][1], wx, wz);
            m_stackTop[k][0] = wx;
            m_stackTop[k][1] = padY + kFacChimneyH - 0.6f;
            m_stackTop[k][2] = wz;
            // A stack you can hit. One collider column, not the pack's hull.
            MeshBuf col; col.uvPerM = 0.2f;
            col.cylinder(sf, stack[k][0], stack[k][1], 3.4f, padY, padY + kFacChimneyH, 10);
            std::vector<float> cv; cv.reserve(col.v.size()*3);
            for (const auto& vv : col.v) { cv.push_back(vv.pos[0]); cv.push_back(vv.pos[1]); cv.push_back(vv.pos[2]); }
            phys.addStaticMesh(cv.data(), (uint32_t)(cv.size()/3),
                               col.i.data(), (uint32_t)col.i.size());
        }
    }

    // THE FENCE LINE. Pack panels for the look, ONE procedural wall strip per
    // run for the collision — 200-odd per-panel colliders would be 200 Jolt
    // bodies for a wall a car meets as a straight line.
    {
        MeshBuf wall; wall.uvPerM = 0.3f;
        auto run = [&](float a0, float b0, float a1, float b1, bool gateGap) {
            const float da = a1 - a0, db = b1 - b0;
            const float len = std::sqrt(da*da + db*db);
            if (len < 1.0f) return;
            const float ua = da/len, ub = db/len;
            const int   n  = (int)(len / 2.24f);
            for (int k = 0; k < n; ++k) {
                const float t = 2.24f * ((float)k + 1.0f);   // panel spans x[-2.22,0.02]
                const float a = a0 + ua*t, b = b0 + ub*t;
                if (gateGap && std::fabs(b) < kFacGateB + 2.6f) continue;
                place("Factory/sm_ConcreteFence_01_01.glb", a, b, padY, ua, ub, 1.0f);
            }
        };
        // THE PACK PANELS GO ON THE FRONT RUN ONLY, AND THAT IS A MEASUREMENT,
        // NOT A SHRUG. Fencing the whole 500 m perimeter with them put 223
        // photogrammetry panels in the world and the capture priced them: 2.13
        // MILLION triangles, the works camera going 1193 -> 568 fps, for a wall
        // that on three of its four sides is only ever seen at 200 m+. (The
        // panels are scan-density LOD0 — tools/convert_scansfactory.py prunes
        // the pack's LOD1, which is right for a hero prop and wrong for two
        // hundred of them.) The front run is the one the drive, the gate shot
        // and the freeway all look straight at; the other three keep the
        // procedural concrete wall below, which is the same textured concrete
        // and reads identically at that range. If a later lane wants panels all
        // the way round, the answer is a re-convert that KEEPS LOD1 and a
        // distance switch — not 223 LOD0 scans.
        run(kFacPadA0, kFacPadB0, kFacPadA0, kFacPadB1, true);
        // Collision walls (four straight slabs, gate gap left open).
        wall.box(sf, kFacPadA0 - 0.1f, kFacPadA0 + 0.1f, kFacPadB0, -kFacGateB - 2.6f, padY, padY + 2.0f);
        wall.box(sf, kFacPadA0 - 0.1f, kFacPadA0 + 0.1f,  kFacGateB + 2.6f, kFacPadB1, padY, padY + 2.0f);
        wall.box(sf, kFacPadA1 - 0.1f, kFacPadA1 + 0.1f, kFacPadB0, kFacPadB1, padY, padY + 2.0f);
        wall.box(sf, kFacPadA0, kFacPadA1, kFacPadB0 - 0.1f, kFacPadB0 + 0.1f, padY, padY + 2.0f);
        wall.box(sf, kFacPadA0, kFacPadA1, kFacPadB1 - 0.1f, kFacPadB1 + 0.1f, padY, padY + 2.0f);
        const float fenceTint[4] = { 0.78f, 0.78f, 0.76f, 1.0f };
        upload(wall, &concrete, fenceTint, true);
    }

    // THE GATE — two pack leaves meeting on the centreline, flanked by piers
    // that neck the drive down from the freeway's 29 m to a 9.4 m opening.
    {
        MeshBuf piers; piers.uvPerM = 0.3f;
        for (int s2 = -1; s2 <= 1; s2 += 2) {
            const float b = (float)s2 * (kFacGateB + 1.3f);
            piers.box(sf, kFacPadA0 - 1.1f, kFacPadA0 + 1.1f, b - 1.3f, b + 1.3f,
                      padY, padY + 6.6f);
        }
        piers.box(sf, kFacPadA0 - 0.6f, kFacPadA0 + 0.6f, -kFacGateB - 1.3f,
                  kFacGateB + 1.3f, padY + 6.6f, padY + 7.6f);          // the lintel
        upload(piers, &concrete, pale, true);

        for (int s2 = -1; s2 <= 1; s2 += 2) {
            // Each leaf's long axis runs across the drive (the b axis) and its
            // hinge edge is at the pier, so it slides OUTWARD to open.
            const float bAnchor = (float)s2 * kFacGateB;
            const uint32_t id = place("Factory/sm_MainGate_01_01.glb",
                                      kFacPadA0, bAnchor, padY,
                                      0.0f, -(float)s2, 1.0f);
            if (id != 0xFFFFFFFFu) {
                SlideLeaf L; L.ent = id; L.dir = (float)s2;
                const Entity& e = scene.get(id);
                for (int q = 0; q < 16; ++q) L.base[q] = e.transform[q];
                m_gateLeaves.push_back(L);
            }
        }
        // A gate you cannot drive through until it opens. One slab, removed
        // outright when the fifth ticket lands — no half-open collision state.
        MeshBuf shut; shut.uvPerM = 0.4f;
        shut.box(sf, kFacPadA0 - 0.16f, kFacPadA0 + 0.16f, -kFacGateB, kFacGateB,
                 padY, padY + 5.4f);
        Entity ge;
        ge.mesh = device.createMesh(shut.v.data(), (uint32_t)shut.v.size(),
                                    shut.i.data(), (uint32_t)shut.i.size());
        if (ge.mesh.valid()) {
            ge.visible = false;                 // the pack leaves are the visual
            m_gateBlockEnt = scene.add(ge);
            ++m_meshCount;
        }
        std::vector<float> cv; cv.reserve(shut.v.size()*3);
        for (const auto& vv : shut.v) { cv.push_back(vv.pos[0]); cv.push_back(vv.pos[1]); cv.push_back(vv.pos[2]); }
        m_gateBody = phys.addStaticMesh(cv.data(), (uint32_t)(cv.size()/3),
                                        shut.i.data(), (uint32_t)shut.i.size());
        m_physRef = &phys;
    }

    // YARD DRESSING + the external steel pipe run on the hall's flank.
    {
        uint32_t rng = 0x51AB1EEDu;
        for (int k = 0; k < 14; ++k) {
            const float a = -46.0f + hash01(rng) * 44.0f;
            const float b = 20.0f + hash01(rng) * 34.0f;
            place(hash01(rng) < 0.55f ? "Factory/sm_Barrel_01_01.glb"
                                      : "Factory/sm_Container_Body_01_01.glb",
                  a, b, padY, std::cos(hash01(rng) * 6.283f),
                  std::sin(hash01(rng) * 6.283f), 1.0f);
        }
        // Pipes: 8 m modules chained along the hall's b0 flank at two heights,
        // elbows turning up the wall. Measured: the straight runs +X 8 m from
        // its origin, so each module starts where the last ended.
        for (int lane = 0; lane < 2; ++lane) {
            const float y = padY + (lane ? 8.4f : 3.2f);
            for (int k = 0; k < 5; ++k)
                place("Factory/sm_Pipe_Straight_8m_03_14.glb",
                      kFacHallA0 + 2.0f + 8.0f * (float)k, kFacHallB0 - 1.1f, y,
                      1.0f, 0.0f, 1.0f);
            place("Factory/sm_Pipe_Elbow_03_02.glb",
                  kFacHallA0 + 42.0f, kFacHallB0 - 1.1f, y, 1.0f, 0.0f, 1.0f);
        }
    }

    m_built = m_meshCount > 0;
    char b[300];
    std::snprintf(b, sizeof(b),
        "factory: THE GLIMVALE WORKS built — %u meshes / %u tris + %u pack "
        "instances | hall %.0fx%.0f m to %.0f ft, hothouse %.0f ft of glass, "
        "chimneys %.0f ft | gate SHUT",
        m_meshCount, m_triCount, m_propCount,
        kFacHallA1 - kFacHallA0, kFacHallB1 - kFacHallB0,
        (kFacHallH + kFacSawH) * kMToFt, kFacGlassH * kMToFt,
        kFacChimneyH * kMToFt);
    x3::logInfo(b);
    if (!haveKit)
        x3::logWarn("factory: converted_glb/Factory not mounted — pack kit absent "
                    "(massing still stands; run tools/convert_scansfactory.py)");
    return m_built;
}

void FactoryWorks::openGate() {
    if (m_gateOpen) return;
    m_gateOpen = true;
    if (m_physRef && m_gateBody.id) { m_physRef->removeBody(m_gateBody); m_gateBody = {}; }
    x3::logInfo("factory: FIVE TICKETS — the works gate is opening");
}

void FactoryWorks::update(Scene& scene, float dt) {
    if (!m_built) return;
    m_clock += dt;

    // The gate slide: 4.4 m each way over 2.5 s, eased. 2.5 and not 3.5 so the
    // slide COMPLETES inside the 200-frame headless settle (3.3 s) — a gate
    // whose open state no capture can reach is a gate nobody can review.
    if (m_gateOpen && m_gateT < 1.0f) {
        m_gateT = std::min(1.0f, m_gateT + dt / 2.5f);
        const float e = m_gateT * m_gateT * (3.0f - 2.0f * m_gateT);
        for (const SlideLeaf& L : m_gateLeaves) {
            if (L.ent >= scene.size()) continue;
            Entity& en = scene.get(L.ent);
            for (int q = 0; q < 16; ++q) en.transform[q] = L.base[q];
            const float slide = e * 4.4f * L.dir;
            en.transform[12] += m_plan.rX * slide;
            en.transform[14] += m_plan.rZ * slide;
        }
    }

    // The tubes BREATHE. A slow travelling swell per tube, peaking at 0.46 —
    // deliberately under the ~0.5 ACES clip (X3_WORLD_RULES rule 5), so what
    // the eye reads is flow, never a blown white bar.
    for (const PulseTube& t : m_tubes) {
        if (t.ent >= scene.size()) continue;
        Entity& en = scene.get(t.ent);
        const float s = 0.5f + 0.5f * std::sin(m_clock * 1.35f + t.phase);
        en.emissive[3] = 0.16f + 0.30f * s * s;
    }

    // The machinery shakes as it does its thing: a ~1.6 cm buzz at 7-11 Hz,
    // per-block phase, applied to the AUTHORED transform (never accumulated —
    // an accumulating jitter walks the building off its pad in five minutes).
    for (size_t k = 0; k < m_shakers.size(); ++k) {
        const uint32_t id = m_shakers[k];
        if (id >= scene.size() || (k + 1) * 16 > m_shakerBase.size()) continue;
        Entity& en = scene.get(id);
        const float ph = (float)k * 1.9f;
        const float f1 = 7.0f + 1.3f * (float)k;
        for (int q = 0; q < 16; ++q) en.transform[q] = m_shakerBase[k * 16 + q];
        en.transform[13] += 0.016f * std::sin(m_clock * f1 * 6.2832f + ph);
        en.transform[12] += 0.009f * std::sin(m_clock * (f1 * 0.7f) * 6.2832f + ph * 2.0f);
    }

    // ---- chimney smoke, integrated here and SUBMITTED in drawSmoke --------
    m_emitAcc += dt;
    const float kEmitEvery = 0.11f;
    while (m_emitAcc >= kEmitEvery) {
        m_emitAcc -= kEmitEvery;
        for (int s2 = 0; s2 < 2; ++s2) {
            if (m_puffs.size() >= 320) break;
            Puff p;
            p.x = m_stackTop[s2][0] + (hash01(m_rng) - 0.5f) * 2.4f;
            p.y = m_stackTop[s2][1];
            p.z = m_stackTop[s2][2] + (hash01(m_rng) - 0.5f) * 2.4f;
            p.vy    = 3.4f + hash01(m_rng) * 1.8f;
            p.drift = 0.7f + hash01(m_rng) * 0.9f;
            p.age   = 0.0f;
            p.life  = 7.5f + hash01(m_rng) * 4.0f;
            p.size0 = 2.4f + hash01(m_rng) * 1.6f;
            m_puffs.push_back(p);
        }
    }
    for (size_t k = 0; k < m_puffs.size();) {
        Puff& p = m_puffs[k];
        p.age += dt;
        if (p.age >= p.life) { p = m_puffs.back(); m_puffs.pop_back(); continue; }
        p.y += p.vy * dt;
        p.vy = std::max(0.7f, p.vy - 0.32f * dt);      // buoyancy bleeds off
        p.x += p.drift * dt * 1.5f;                    // the prevailing drift
        p.z += p.drift * dt * 0.55f;
        ++k;
    }
}

void FactoryWorks::drawSmoke(x3::rhi::IRenderDevice& device, const float cam[3]) {
    if (!m_built || m_puffs.empty()) return;
    // Cull by distance: a stack plume 3 km away is four pixels of nothing.
    const float dx = cam[0] - m_plan.cx, dz = cam[2] - m_plan.cz;
    if (dx*dx + dz*dz > 2600.0f * 2600.0f) return;
    m_smokeOut.clear();
    m_smokeOut.reserve(m_puffs.size());
    for (const Puff& p : m_puffs) {
        const float t = p.age / p.life;
        x3::rhi::IRenderDevice::ParticleInstance pi;
        pi.pos[0] = p.x; pi.pos[1] = p.y; pi.pos[2] = p.z;
        pi.size = p.size0 * (1.0f + 3.2f * t);
        // Warm-grey confection smoke: dark and dense at the lip, paling and
        // thinning out. ALPHA blend — RGB held, A faded (the river_life idiom;
        // additive smoke would light the sky instead of occluding it).
        const float pale2 = 0.30f + 0.42f * t;
        pi.color[0] = pale2 * 1.02f; pi.color[1] = pale2 * 0.99f; pi.color[2] = pale2 * 0.95f;
        const float fadeIn = std::min(1.0f, t * 12.0f);
        pi.color[3] = 0.62f * fadeIn * (1.0f - t) * (1.0f - t);
        m_smokeOut.push_back(pi);
    }
    device.submitParticles(m_smokeOut.data(), (uint32_t)m_smokeOut.size(),
                           x3::rhi::IRenderDevice::ParticleBlend::Alpha);
}

void FactoryWorks::shutdown(x3::rhi::IRenderDevice& device) {
    (void)device;   // meshes are Scene-owned; the Scene/device teardown frees them
    m_loader.reset();
    m_assets.reset();
    m_puffs.clear();
    m_tubes.clear();
    m_shakers.clear();
    m_gateLeaves.clear();
    m_built = false;
}

// ===========================================================================
// THE GOLDEN TICKETS
// ===========================================================================
uint32_t factoryTicketSpots(const FactoryPlan& plan, TicketSpotDef out[kTicketCount]) {
    uint32_t n = 0;

    // 1. THE WORKS GATE — OUTSIDE the fence, beside the drive. Inside would be
    //    a card you cannot reach until the gate the cards open is already open.
    if (plan.ok) {
        const float a = kFacPadA0 - 5.0f, b = kFacGateB + 4.4f;
        const float x = plan.cx + plan.fX * a + plan.rX * b;
        const float z = plan.cz + plan.fZ * a + plan.rZ * b;
        out[n++] = { "THE WORKS GATE", x, terrainHeightAtWorld(x, z), z };
    }

    // 2. BRIDGE No.1 — on the deck, at the west abutment end of the parapet
    //    walk, where a driver who stops actually stands.
    {
        const RiverBridgePlan rb = planRiverBridge();
        if (rb.ok) {
            const float s = -(rb.abutS - 6.0f), lat = rb.deckHalfWidth - 1.4f;
            const float x = rb.cx + rb.dirX * s + (-rb.dirZ) * lat;
            const float z = rb.cz + rb.dirZ * s + ( rb.dirX) * lat;
            out[n++] = { "BRIDGE No.1", x, rb.deckY + 0.05f, z };
        }
    }

    // 3. THE RIVERBANK — MEASURED, not guessed. Two ways this went wrong before
    //    --test-factory caught them, both worth writing down:
    //      (a) a hardcoded lateral offset put the card IN the channel;
    //      (b) "walk out from the LAST river node" put it on the lip of the
    //          OCEAN BASIN (terrain.cpp kBasinCx 1100 / kBasinCz -1350). The
    //          ribbon's final node is (900, -1120), where the ground measures
    //          -90 m — 80 m UNDER the water it was supposed to be a bank of.
    //    So: walk every node, skip the reach Bridge No.1 already owns, and take
    //    the first bank that is dry against THAT REACH's own water level and
    //    flat enough to leave a card standing on.
    {
        uint32_t rc = 0;
        const WorldRiverNode* rn = worldRiverNodes(rc);
        const RiverBridgePlan rbp = planRiverBridge();
        bool found = false;
        for (uint32_t i = 1; rn && i + 1 < rc && !found; ++i) {
            if (rbp.ok) {
                const float bdx = rn[i].x - rbp.cx, bdz = rn[i].z - rbp.cz;
                if (bdx*bdx + bdz*bdz < 500.0f * 500.0f) continue;   // the bridge's reach
            }
            float dx = rn[i+1].x - rn[i-1].x, dz = rn[i+1].z - rn[i-1].z;
            const float dl = std::sqrt(dx*dx + dz*dz);
            if (dl < 1e-3f) continue;
            dx /= dl; dz /= dl;
            const float px = -dz, pz = dx;             // perpendicular: the banks
            for (int side = 1; side >= -1 && !found; side -= 2) {
                for (float o = 36.0f; o <= 150.0f && !found; o += 4.0f) {
                    const float tx = rn[i].x + px * o * (float)side;
                    const float tz = rn[i].z + pz * o * (float)side;
                    const float h  = terrainHeightAtWorld(tx, tz);
                    // Dry against THIS REACH's surface. worldWaterLevelAt reads
                    // -FLT_MAX the moment you step off the ribbon, so on its own
                    // it calls the bottom of the ocean basin "dry" — which is
                    // exactly how (b) happened. rn[i].waterY is the real level.
                    if (h < rn[i].waterY + 1.2f) continue;
                    if (h < worldWaterLevelAt(tx, tz) + 1.2f) continue;
                    if (terrainCorridorContains(tx, tz)) continue;   // not on a road
                    const float hA = terrainHeightAtWorld(tx + 2.0f, tz);
                    const float hB = terrainHeightAtWorld(tx, tz + 2.0f);
                    const float slope = std::max(std::fabs(hA - h), std::fabs(hB - h)) / 2.0f;
                    if (slope > 0.42f) continue;      // ~23 deg is a levee face, not a bank
                    out[n++] = { "THE RIVERBANK", tx, h, tz };
                    found = true;
                }
            }
        }
        if (!found)
            x3::logWarn("tickets: no dry, flat riverbank found — the riverbank "
                        "card is NOT hidden (--test-factory F5 will say so)");
    }

    // 4. THE SUMMIT LOT — the spur peak (ROAD_NETWORK_PLAN anchor table:
    //    peak (393, 6752); forest.cpp keeps a 150 m keep-out disc there for
    //    exactly this lot, so the card is not inside a tree).
    {
        const float x = 393.0f, z = 6752.0f;
        out[n++] = { "THE SUMMIT LOT", x, terrainHeightAtWorld(x, z), z };
    }

    // 5. See the TODO on TicketSpotDef — the town square when Lane 4 lands.
    {
        const float x = -2020.0f, z = 583.0f;          // J-CIRCUIT-LAND
        out[n++] = { "THE RANGE CIRCUIT", x, terrainHeightAtWorld(x, z), z };
    }
    return n;
}

void GoldenTickets::addSpot(const char* name, float x, float y, float z) {
    if (m_built) return;
    Spot s; s.name = name ? name : "?"; s.x = x; s.y = y; s.z = z;
    m_spots.push_back(std::move(s));
}

void GoldenTickets::spotPos(uint32_t i, float out[3]) const {
    if (i >= m_spots.size()) { out[0]=out[1]=out[2]=0; return; }
    out[0] = m_spots[i].x; out[1] = m_spots[i].y + kTicketHoverM; out[2] = m_spots[i].z;
}
const char* GoldenTickets::spotName(uint32_t i) const {
    return i < m_spots.size() ? m_spots[i].name.c_str() : "";
}
bool GoldenTickets::spotTaken(uint32_t i) const {
    return i < m_spots.size() && m_spots[i].taken;
}

bool GoldenTickets::build(Scene& scene, x3::rhi::IRenderDevice& device) {
    if (m_spots.empty()) return false;

    // THE CARD, baked. A near-black border with a hot gold field and darker
    // printed rules inside it, used as BOTH albedo and emissiveTex over a 1x1
    // MR — the rule-5 recipe. A flat emissive card would clip to a white chip
    // the moment auto-exposure looked at it; this one keeps its edges.
    constexpr uint32_t W = 64, H = 40;
    std::vector<uint8_t> px((size_t)W * H * 4, 0);
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) {
            uint8_t* p = &px[((size_t)y * W + x) * 4];
            const bool border = (x < 3 || y < 3 || x >= W - 3 || y >= H - 3);
            const bool rule   = (!border && (y % 9 == 4) && x > 8 && x < W - 9);
            if (border)    { p[0]=10;  p[1]=8;   p[2]=4;   }
            else if (rule) { p[0]=90;  p[1]=52;  p[2]=8;   }
            else           { p[0]=255; p[1]=206; p[2]=96;  }
            p[3] = 255;
        }
    m_cardTex = device.createTexture(px.data(), W, H, true);
    // X3_WORLD_RULES RULE 5, PAID FOR IN A CAPTURE. The first cut used
    // makeMr1x1(70, 210) — metallic 0.82 — reasoning "gold foil is metal, and
    // metal glints". It rendered as a BLACK SPECK inside its own spark: a full
    // metal has no diffuse lobe, so its only light is a reflection, and there
    // is no environment out in a field to reflect. That is the F2 black-prop
    // plague, in a 0.2 m object, and the rule names the fix: clamp metalness.
    // The glint comes from the additive spark and the emissiveTex, not from
    // metalness.
    auto mr = lns::makeMr1x1(80, 60);      // roughness 0.31, metallic 0.24
    m_cardMr = device.createTexture(mr.data(), 1, 1, false);

    // 26 x 18 cm. A real ticket is 9 x 13 cm and at that size it was two
    // pixels of nothing at the range you actually spot one from; this is the
    // readable-prop size, the same licence a pickup always takes.
    x3::prims::PrimMesh card = x3::prims::makeBox(0.130f, 0.090f, 0.004f,
                                                  0.0f, 0.0f, 0.0f, 1.0f);
    m_cardMesh = device.createMesh(card.verts.data(), (uint32_t)card.verts.size(),
                                   card.index.data(), (uint32_t)card.index.size());
    if (!m_cardMesh.valid()) return false;

    for (Spot& s : m_spots) {
        Entity e;
        e.mesh = m_cardMesh;
        e.tex = m_cardTex; e.mrTex = m_cardMr; e.emissiveTex = m_cardTex;
        e.emissive[0]=1.0f; e.emissive[1]=0.80f; e.emissive[2]=0.34f;
        e.emissive[3]=1.10f;
        e.transform[12] = s.x; e.transform[13] = s.y + kTicketHoverM; e.transform[14] = s.z;
        s.ent = scene.add(e);
    }
    m_built = true;
    char b[220];
    std::snprintf(b, sizeof(b), "tickets: %u golden tickets hidden — %s",
                  (uint32_t)m_spots.size(),
                  [&]{ static std::string j; j.clear();
                       for (size_t k = 0; k < m_spots.size(); ++k) {
                           if (k) j += ", "; j += m_spots[k].name; }
                       return j.c_str(); }());
    x3::logInfo(b);
    return true;
}

int GoldenTickets::update(Scene& scene, float dt, float px, float py, float pz,
                          bool interactEdge) {
    if (!m_built) return -1;
    m_clock += dt;
    if (m_toast > 0.0f) m_toast -= dt;
    int got = -1;
    for (size_t k = 0; k < m_spots.size(); ++k) {
        Spot& s = m_spots[k];
        if (s.taken || s.ent >= scene.size()) continue;
        Entity& e = scene.get(s.ent);
        // Spin + bob. The spin is what makes a 9 cm card findable at 40 m:
        // the face catches the sun once a second.
        const float ang = m_clock * 1.9f + (float)k * 1.3f;
        const float ca = std::cos(ang), sa = std::sin(ang);
        e.transform[0]=ca;  e.transform[1]=0; e.transform[2]=sa;  e.transform[3]=0;
        e.transform[4]=0;   e.transform[5]=1; e.transform[6]=0;   e.transform[7]=0;
        e.transform[8]=-sa; e.transform[9]=0; e.transform[10]=ca; e.transform[11]=0;
        e.transform[12]=s.x;
        e.transform[13]=s.y + kTicketHoverM + 0.11f * std::sin(m_clock * 1.4f + (float)k);
        e.transform[14]=s.z;

        const float dx = px - s.x, dy = py - (s.y + kTicketHoverM), dz = pz - s.z;
        const float d2 = dx*dx + dz*dz;
        if (d2 <= kTicketReachM * kTicketReachM && std::fabs(dy) <= 6.0f && interactEdge) {
            s.taken = true;
            e.visible = false;
            ++m_collected;
            got = (int)k;
            m_toast = 3.2f;
            m_toastText = "GOLDEN TICKET  " + std::to_string(m_collected) + "/" +
                          std::to_string(m_spots.size()) + "  -  " + s.name;
            x3::logInfo("tickets: found the " + s.name + " ticket (" +
                        std::to_string(m_collected) + "/" +
                        std::to_string(m_spots.size()) + ")");
        }
    }
    return got;
}

void GoldenTickets::drawGlints(x3::rhi::IRenderDevice& device) {
    if (!m_built) return;
    m_glintOut.clear();
    for (size_t k = 0; k < m_spots.size(); ++k) {
        const Spot& s = m_spots[k];
        if (s.taken) continue;
        // An additive spark that swells and dies twice a second. Additive VFX
        // keep a GLOW FLOOR (rule 5: an emissive that drops below the
        // background subtracts under alpha-over and darkens the scene), so the
        // amplitude never reaches zero.
        const float ph = m_clock * 2.2f + (float)k * 1.7f;
        const float g  = 0.35f + 0.65f * std::pow(std::max(0.0f, std::sin(ph)), 6.0f);
        x3::rhi::IRenderDevice::ParticleInstance pi;
        pi.pos[0] = s.x; pi.pos[1] = s.y + kTicketHoverM; pi.pos[2] = s.z;
        pi.size = 0.20f + 0.42f * g;
        pi.color[0] = 1.00f * g; pi.color[1] = 0.78f * g; pi.color[2] = 0.30f * g;
        pi.color[3] = 1.0f;
        m_glintOut.push_back(pi);
    }
    if (!m_glintOut.empty())
        device.submitParticles(m_glintOut.data(), (uint32_t)m_glintOut.size(),
                               x3::rhi::IRenderDevice::ParticleBlend::Additive);
}

void GoldenTickets::drawHud(x3::rhi::IRenderDevice& device,
                            const x3::rhi::FrameContext& frame,
                            float px, float py, float pz) const {
    if (!m_built) return;
    uint32_t hw = 0, hh = 0; device.hudSize(hw, hh);
    if (!hw || !hh) return;
    const float sh[4] = { 0.0f, 0.0f, 0.0f, 0.78f };
    const float gold[4] = { 1.0f, 0.86f, 0.42f, 1.0f };
    auto text = [&](const char* t, float x, float y, float p, const float col[4]) {
        device.drawHudText(frame, t, x + 1.5f, y + 1.5f, p, sh);
        device.drawHudText(frame, t, x, y, p, col);
    };

    // THE COUNTER, top-right under the clock line every other HUD keeps clear.
    char cb[32];
    std::snprintf(cb, sizeof(cb), "TICKETS %d/%d", m_collected, (int)m_spots.size());
    const float cp = std::floor((float)hh * 0.024f);
    text(cb, (float)hw - (float)std::strlen(cb) * cp - 24.0f, (float)hh * 0.055f, cp, gold);

    // THE PROMPT, when a card is in reach. "A control nobody can see is a
    // control nobody has" (host_tunnel's own note on the E prompt).
    for (const Spot& s : m_spots) {
        if (s.taken) continue;
        const float dx = px - s.x, dy = py - (s.y + kTicketHoverM), dz = pz - s.z;
        if (dx*dx + dz*dz > kTicketReachM * kTicketReachM || std::fabs(dy) > 6.0f) continue;
        const char* p = "E   TAKE THE GOLDEN TICKET";
        const float pp = std::floor((float)hh * 0.026f);
        text(p, ((float)hw - (float)std::strlen(p) * pp) * 0.5f, (float)hh * 0.80f, pp, gold);
        break;
    }

    // THE TOAST.
    if (m_toast > 0.0f && !m_toastText.empty()) {
        const float tp = std::floor((float)hh * 0.030f);
        text(m_toastText.c_str(),
             ((float)hw - (float)m_toastText.size() * tp) * 0.5f, (float)hh * 0.15f,
             tp, gold);
    }
}

void GoldenTickets::setCollected(Scene& scene, int n) {
    n = std::max(0, std::min((int)m_spots.size(), n));
    m_collected = n;
    for (size_t k = 0; k < m_spots.size(); ++k) {
        const bool taken = (int)k < n;
        m_spots[k].taken = taken;
        if (m_built && m_spots[k].ent < scene.size())
            scene.get(m_spots[k].ent).visible = !taken;
    }
}

void GoldenTickets::shutdown(x3::rhi::IRenderDevice& device) {
    if (m_cardMesh.valid()) device.destroyMesh(m_cardMesh);
    if (m_cardTex.valid())  device.destroyTexture(m_cardTex);
    if (m_cardMr.valid())   device.destroyTexture(m_cardMr);
    m_cardMesh = {}; m_cardTex = {}; m_cardMr = {};
    m_spots.clear();
    m_built = false;
}

// ===========================================================================
// --test-factory
// ===========================================================================
namespace {

int g_pass = 0, g_fail = 0;
void check(bool ok, const std::string& what) {
    if (ok) { ++g_pass; x3::logInfo("[factory] PASS " + what); }
    else    { ++g_fail; x3::logError("[factory] FAIL " + what); }
}

} // namespace

bool runFactorySelfTest() {
    g_pass = g_fail = 0;
    clearTerrainCorridors();
    clearRoadJunctions();

    // The world, built the same way the host builds it — the freeway first,
    // because the works is sited against it and nothing else.
    RoadSpec fwy = makeInnerCourse();
    std::vector<float> fwyY;
    const RoadBuildResult fr = registerRoad(fwy, &fwyY);
    if (!fr.ok) { x3::logError("[factory] the freeway would not register"); return false; }

    const FactoryPlan plan = planFactoryWorks(fwy, fwyY);
    check(plan.ok, std::string("F1a a site was found — ") +
          (plan.ok ? "yes" : plan.whyNot));
    if (!plan.ok) { x3::logError("[factory] 0 passed, 1 failed"); return false; }

    constexpr float kTourCX = -592.0f, kTourCZ = -352.0f;
    char b[420];

    // ---- F0 THE NEGATIVE CONTROL ------------------------------------------
    // The brief offered "NE forest edge OR riverside". The riverside option is
    // rejected BY MEASUREMENT, not by taste: measure how far the river's own
    // reach is from the freeway and show it is not a landmark anyone driving
    // would ever see. (Same number river_bridge.h records from the other side.)
    {
        float worst = 1e18f;
        uint32_t rc = 0;
        const WorldRiverNode* rn = worldRiverNodes(rc);
        for (uint32_t i = 0; i < rc; ++i) {
            float d = 1e18f;
            for (size_t k = 0; k + 1 < fwy.x.size(); ++k) {
                const float dx = rn[i].x - fwy.x[k], dz = rn[i].z - fwy.z[k];
                d = std::min(d, std::sqrt(dx*dx + dz*dz));
            }
            worst = std::min(worst, d);
        }
        // The chosen site, by contrast, must be inside a glance of the road.
        const float dxs = plan.cx - plan.jx, dzs = plan.cz - plan.jz;
        const float siteD = std::sqrt(dxs*dxs + dzs*dzs);
        std::snprintf(b, sizeof(b),
            "F0 the riverside site is NOT freeway-visible and the NE site is — "
            "river's nearest approach to the tour %.0f m (%.2f mi), chosen site "
            "%.0f m off the pavement", worst, worst / 1609.34f, siteD);
        check(worst > 1500.0f && siteD < 500.0f, b);
    }

    // ---- F1 the sketch's NE sector, at the forest edge ---------------------
    {
        const float dx = plan.cx - kTourCX, dz = plan.cz - kTourCZ;
        float deg = std::atan2(dz, dx) * 57.29578f;
        if (deg < 0.0f) deg += 360.0f;
        // The centre-north forest patch (forest.cpp region 2), as authored.
        const float nx = (plan.cx - 1700.0f) / 1150.0f, nz = (plan.cz - 3900.0f) / 600.0f;
        const float ell = nx*nx + nz*nz;
        std::snprintf(b, sizeof(b),
            "F1 the works stands in the sketch's NE sector at the forest edge — "
            "bearing %.1f deg about the tour centre (sector 26-74), forest "
            "ellipse t=%.2f (1.0 = the treeline; >1 is the clearing outside it)",
            deg, ell);
        check(deg >= 26.0f && deg <= 74.0f && ell > 1.0f && ell < 6.0f, b);
    }

    // ---- F2 the platform is buildable --------------------------------------
    {
        std::snprintf(b, sizeof(b),
            "F2 the platform sits on ground flat enough to build on — relief "
            "%.1f ft over the 130x120 m footprint, tallest exposed face %.1f ft "
            "(caps 40 / 26 ft)", plan.reliefM * kMToFt, plan.exposedM * kMToFt);
        check(plan.reliefM * kMToFt < 40.0f && plan.exposedM * kMToFt < 26.0f, b);
    }

    // ---- F3 the drive ------------------------------------------------------
    const FactoryDriveResult drv = registerFactoryDrive(plan, fwy, fwyY);
    {
        std::snprintf(b, sizeof(b),
            "F3 the drive reaches the freeway at a legal grade and lands AT "
            "grade — %.0f m, max grade %.2f%% (cap 7.0), pin deficit %.2f ft "
            "(cap 1.0), landing datum %.1f vs the tour's %.1f",
            drv.road.lengthM, drv.road.maxGradePct, drv.road.pinErrM * kMToFt,
            drv.ok ? drv.roadY.front() : 0.0f, plan.jy);
        check(drv.ok && drv.road.maxGradePct <= 7.05f &&
              drv.road.pinErrM * kMToFt < 1.0f, b);
    }
    {
        // The drive's gate end must actually arrive at the platform, or the
        // pavement steps onto the forecourt (the defect the ring landings
        // exist to prevent, and the one the owner screenshotted).
        const float step = drv.ok ? std::fabs(drv.roadY.back() - plan.padY) : 99.0f;
        std::snprintf(b, sizeof(b),
            "F3b the pavement meets the forecourt FLUSH — gate-end datum %.2f "
            "vs platform %.2f, step %.2f ft (cap 1.0)",
            drv.ok ? drv.roadY.back() : 0.0f, plan.padY, step * kMToFt);
        check(step * kMToFt < 1.0f, b);
    }

    // ---- F4 the works fits inside its own fence, gate drivable -------------
    {
        const bool hallIn  = kFacHallA0  > kFacPadA0 + 2.0f && kFacHallA1  < kFacPadA1 - 2.0f &&
                             kFacHallB0  > kFacPadB0 + 2.0f && kFacHallB1  < kFacPadB1 - 2.0f;
        const bool glassIn = kFacGlassA0 > kFacPadA0 + 2.0f && kFacGlassA1 < kFacPadA1 - 2.0f &&
                             kFacGlassB0 > kFacPadB0 + 2.0f && kFacGlassB1 < kFacPadB1 - 2.0f;
        const bool noOverlap = (kFacGlassB0 >= kFacHallB1) || (kFacGlassB1 <= kFacHallB0) ||
                               (kFacGlassA0 >= kFacHallA1) || (kFacGlassA1 <= kFacHallA0);
        // A car is ~1.9 m wide; two lanes through the gate is the ask.
        const bool gateWide = kFacGateB * 2.0f >= 7.0f;
        std::snprintf(b, sizeof(b),
            "F4 every mass stands inside the fence and the gate is drivable — "
            "hall %s, hothouse %s, no interpenetration %s, gate opening %.2f m "
            "(%.1f ft, floor 7 m)",
            hallIn ? "in" : "OUT", glassIn ? "in" : "OUT", noOverlap ? "ok" : "NO",
            kFacGateB * 2.0f, kFacGateB * 2.0f * kMToFt);
        check(hallIn && glassIn && noOverlap && gateWide, b);
    }
    {
        // The skyline read is the whole point of a landmark: the stacks must
        // clear everything in front of them from the road's eye height.
        const float tallest = std::max(kFacHallH + kFacSawH, kFacGlassH);
        std::snprintf(b, sizeof(b),
            "F4b the chimneys top the skyline — stacks %.0f ft vs the tallest "
            "mass in front of them %.0f ft (clearance %.0f ft)",
            kFacChimneyH * kMToFt, tallest * kMToFt,
            (kFacChimneyH - tallest) * kMToFt);
        check(kFacChimneyH > tallest + 10.0f, b);
    }

    // ---- F5 the five ticket spots ------------------------------------------
    {
        // The SAME call the host makes — one definition, so the test cannot
        // pass on a list the world does not hide.
        TicketSpotDef sp[kTicketCount];
        const uint32_t ns = factoryTicketSpots(plan, sp);

        // Every card must be DRY and ON THE GROUND. "On the ground" is the check
        // that would have caught the first riverbank card (which sat in the
        // channel) and it is the runtime twin of X3_WORLD_RULES rule 4 —
        // Bridge No.1's card rides its DECK, so that one is allowed to float
        // above the field by the structure's own height.
        bool dry = true, grounded = true;
        float closest = 1e18f, worstFloat = 0.0f;
        const char* wetOne = ""; const char* offOne = "";
        for (uint32_t i = 0; i < ns; ++i) {
            if (sp[i].y <= worldWaterLevelAt(sp[i].x, sp[i].z) + 0.25f) {
                dry = false; wetOne = sp[i].name;
            }
            const bool onDeck = std::strcmp(sp[i].name, "BRIDGE No.1") == 0;
            const float dh = std::fabs(sp[i].y - terrainHeightAtWorld(sp[i].x, sp[i].z));
            if (!onDeck) {
                worstFloat = std::max(worstFloat, dh);
                if (dh > 0.6f) { grounded = false; offOne = sp[i].name; }
            }
            for (uint32_t k2 = i + 1; k2 < ns; ++k2) {
                const float dx = sp[i].x - sp[k2].x, dz = sp[i].z - sp[k2].z;
                closest = std::min(closest, std::sqrt(dx*dx + dz*dz));
            }
        }
        std::snprintf(b, sizeof(b),
            "F5 five tickets, five real places, spread, dry and ON the ground — "
            "%u spots, closest pair %.0f m (%.2f mi; floor 200 m), water %s%s, "
            "worst off-surface %.2f m (cap 0.6)%s%s",
            ns, closest, closest / 1609.34f, dry ? "clean" : "FAILED at ", wetOne,
            worstFloat, grounded ? "" : " — FLOATING: ", offOne);
        check(ns == (uint32_t)kTicketCount && closest >= 200.0f && dry && grounded, b);
        for (uint32_t i = 0; i < ns; ++i) {
            char sb2[160];
            std::snprintf(sb2, sizeof(sb2),
                "  [factory] ticket %u %-18s (%8.0f, %8.0f) y %7.1f  water %7.1f",
                i + 1, sp[i].name, sp[i].x, sp[i].z, sp[i].y,
                worldWaterLevelAt(sp[i].x, sp[i].z));
            x3::logInfo(sb2);
        }
    }

    // ---- F6 determinism -----------------------------------------------------
    {
        clearTerrainCorridors();
        clearRoadJunctions();
        RoadSpec f2 = makeInnerCourse();
        std::vector<float> f2Y;
        registerRoad(f2, &f2Y);
        const FactoryPlan p2 = planFactoryWorks(f2, f2Y);
        const bool same = p2.ok && p2.fwyNode == plan.fwyNode &&
                          std::fabs(p2.cx - plan.cx) < 0.01f &&
                          std::fabs(p2.cz - plan.cz) < 0.01f &&
                          std::fabs(p2.padY - plan.padY) < 0.001f;
        std::snprintf(b, sizeof(b),
            "F6 the works stands in the same place every boot — node %u vs %u, "
            "(%.2f, %.2f) vs (%.2f, %.2f), padY %.3f vs %.3f",
            p2.fwyNode, plan.fwyNode, p2.cx, p2.cz, plan.cx, plan.cz,
            p2.padY, plan.padY);
        check(same, b);
    }

    char sb[96];
    std::snprintf(sb, sizeof(sb), "[factory] %d passed, %d failed", g_pass, g_fail);
    if (g_fail) x3::logError(sb); else x3::logInfo(sb);
    return g_fail == 0;
}

} // namespace x3::game
