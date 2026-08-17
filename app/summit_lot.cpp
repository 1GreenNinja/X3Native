// SUMMIT PARKING LOT — see summit_lot.h for what this is and what it is not.
#include "summit_lot.h"

#include "terrain.h"
#include "tunnel_corridor.h"    // tunnelNaturalHeightAt: the PRE-carve hillside
#include "surface_library.h"
#include "asset_root.h"
#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace x3::game {
namespace {

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// The slab stands this far proud of the pad datum, and the carved floor is cut
// this far below it. Same two numbers, same reasons, as the road ribbon's
// kSlabProud / kFloorClear: a hair of lift so the markings are not z-fighting
// the slab, and enough floor clearance that depth precision never lets the
// terrain poke through the tarmac.
constexpr float kSlabProud  = 0.02f;
constexpr float kFloorClear = 0.22f;

// The pad's rim prism, the same shape the demo road's apron edge uses: a short
// lip you can see, then a batter you can drive back up. The lot sits on a
// PEAK — the ground falls away on every side — so this is the one edge
// treatment that matters, and gating it is why the self-test measures a step.
constexpr float kRimFace  = 0.20f;
constexpr float kRimSlope = 3.0f;    // 18.4 deg
constexpr float kRimLap   = 0.6f;    // toe buried under the carved floor
constexpr float kRimMax   = 14.0f;

struct LotMesh {
    std::vector<x3::rhi::MeshVertex> v;
    std::vector<uint32_t>            i;
    bool empty() const { return i.empty(); }
    void quad(const float a[3], const float b[3], const float c[3], const float d[3],
              const float n[3], float u0, float u1, float w0, float w1) {
        const uint32_t base = (uint32_t)v.size();
        auto push = [&](const float p[3], float u, float w) {
            x3::rhi::MeshVertex mv{};
            mv.pos[0] = p[0]; mv.pos[1] = p[1]; mv.pos[2] = p[2];
            mv.normal[0] = n[0]; mv.normal[1] = n[1]; mv.normal[2] = n[2];
            mv.uv[0] = u; mv.uv[1] = w;
            v.push_back(mv);
        };
        push(a, u0, w0); push(b, u1, w0); push(c, u1, w1); push(d, u0, w1);
        i.push_back(base+0); i.push_back(base+1); i.push_back(base+2);
        i.push_back(base+0); i.push_back(base+2); i.push_back(base+3);
    }
    // Winding-derived normal, for the rim faces: lighting a vertical concrete
    // face as if it pointed at the sky flattens the only depth it has.
    void quadN(const float a[3], const float b[3], const float c[3], const float d[3],
               float u0, float u1, float w0, float w1) {
        const float e0[3] = { b[0]-a[0], b[1]-a[1], b[2]-a[2] };
        const float e1[3] = { d[0]-a[0], d[1]-a[1], d[2]-a[2] };
        float n[3] = { e0[1]*e1[2]-e0[2]*e1[1], e0[2]*e1[0]-e0[0]*e1[2],
                       e0[0]*e1[1]-e0[1]*e1[0] };
        const float l = std::sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);
        if (l > 1e-6f) { n[0]/=l; n[1]/=l; n[2]/=l; } else { n[0]=0; n[1]=1; n[2]=0; }
        quad(a, b, c, d, n, u0, u1, w0, w1);
    }
};

SurfaceLibrary& lotSurfaces() { static SurfaceLibrary lib; return lib; }

} // namespace

// ---------------------------------------------------------------------------
// REGISTRATION — pick the pad frame off the spur, size the cut, carve it.
// ---------------------------------------------------------------------------
SummitLotResult registerSummitLot(const SummitSpurResult& spur) {
    SummitLotResult out;
    const size_t n = spur.spec.x.size();
    if (!spur.built || n < 2 || spur.roadY.size() != n) {
        out.whyNot = "no summit spur was built, so there is no summit to park on";
        return out;
    }

    // THE PAD'S FRAME comes from the spur's LAST LEG, not from the peak's
    // position. The spur already solved the hard problem (which way you can
    // approach this peak at a legal grade); aligning the lot with anything else
    // would put its kerb across the road that arrives at it.
    const float ax = spur.spec.x[n - 2], az = spur.spec.z[n - 2];
    const float bx = spur.spec.x[n - 1], bz = spur.spec.z[n - 1];
    float tx = bx - ax, tz = bz - az;
    const float tl = std::sqrt(tx * tx + tz * tz);
    if (tl < 1e-3f) { out.whyNot = "the spur's last leg is degenerate"; return out; }
    tx /= tl; tz /= tl;

    // Centre the pad HALF A LENGTH PAST the spur's last node, so the road
    // arrives at the pad's near end (through the mouth) instead of stopping in
    // the middle of it. The datum is the spur's own top: the lot is level with
    // the road that reaches it, which is what makes the entry a step and not a
    // ramp.
    out.dirX = tx; out.dirZ = tz;
    out.cx = bx + tx * kSlHalfLen;
    out.cz = bz + tz * kSlHalfLen;
    out.y  = spur.roadY[n - 1];
    out.mouthX = bx; out.mouthZ = bz;
    out.stalls = kSlBaysPerSide * 2;

    // HOW DEEP. Sample the PRE-carve hillside over the whole pad (plus the
    // carve margin) and take the worst standing ground: the corridor removes a
    // constant depth per node, so the number that matters is the highest point
    // the slab has to get out from under. tunnelNaturalHeightAt subtracts every
    // registered corridor's delta, so this reads the same before and after the
    // spur registered its own carve — the lot is sized against the mountain,
    // not against whatever happens to have been carved first.
    const float rx = -tz, rz = tx;
    float worstUp = 0.0f, worstDown = 0.0f;
    for (int li = -8; li <= 8; ++li) {
        for (int wi = -4; wi <= 4; ++wi) {
            const float sl = (float)li * (kSlHalfLen + kSlCarveMargin) / 8.0f;
            const float sw = (float)wi * (kSlHalfW  + kSlCarveMargin) / 4.0f;
            const float qx = out.cx + tx * sl + rx * sw;
            const float qz = out.cz + tz * sl + rz * sw;
            const float d  = tunnelNaturalHeightAt(qx, qz) - out.y;
            worstUp   = std::max(worstUp,   d);
            worstDown = std::max(worstDown, -d);
        }
    }
    out.cutM  = worstUp;
    out.fillM = worstDown;
    if (worstUp > kSlMaxCutM) {
        out.whyNot = "the peak would have to be gouged away to seat the pad "
                     "(cut over kSlMaxCutM) — the spur tops out on too steep a shoulder";
        return out;
    }

    // THE CARVE. One corridor, two nodes, along the pad's long axis. Its flat
    // floor is a stadium of halfWidth around that segment, which covers the
    // rectangle's corners because halfWidth exceeds the pad's half-WIDTH and
    // the nodes sit at the pad's own ends. Depth is (worst standing ground +
    // floor clearance) at both nodes: constant, because a lerped profile over
    // 44 m would tilt the floor under a slab that is dead level.
    TerrainCorridor c;
    c.nodeCount = 2;
    c.x[0] = out.cx - tx * kSlHalfLen; c.z[0] = out.cz - tz * kSlHalfLen;
    c.x[1] = out.cx + tx * kSlHalfLen; c.z[1] = out.cz + tz * kSlHalfLen;
    c.depth[0] = c.depth[1] = worstUp + kFloorClear;
    c.halfWidth = kSlHalfW + kSlCarveMargin;
    c.falloff   = kSlCarveFall;
    if (!registerTerrainCorridor(c)) {
        out.whyNot = "the terrain-corridor registry is full (kMaxTerrainCorridors)";
        return out;
    }

    out.built = true;
    char b[240];
    std::snprintf(b, sizeof(b),
        "summit lot: %d stalls on a %.0f x %.0f ft pad at (%.0f, %.0f), datum %.0f ft | "
        "cut %.1f ft, worst rim drop %.1f ft",
        out.stalls, kSlHalfLen * 2.0f / 0.3048f, kSlHalfW * 2.0f / 0.3048f,
        out.cx, out.cz, out.y / 0.3048f,
        out.cutM / 0.3048f, out.fillM / 0.3048f);
    x3::logInfo(b);
    return out;
}

// ---------------------------------------------------------------------------
// GEOMETRY
// ---------------------------------------------------------------------------
void buildSummitLot(const SummitLotResult& lot, Scene& scene,
                    x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& phys) {
    if (!lot.built) return;

    SurfaceLibrary& surf = lotSurfaces();
    surf.mount(assetRoot() + "/surface_library");
    const SurfaceSet& asphalt = surf.get(device, "rd_asphalt_01");
    const SurfaceSet& cement  = surf.get(device, "mw_concrete_panels_a");

    const float tx = lot.dirX, tz = lot.dirZ;
    const float rx = -tz,      rz = tx;
    const float topY = lot.y + kSlabProud;
    // (length-along, width-across) -> world. One place converts, so nothing
    // below can pick up the wrong axis.
    auto P = [&](float sl, float sw, float y, float o[3]) {
        o[0] = lot.cx + tx * sl + rx * sw;
        o[1] = y;
        o[2] = lot.cz + tz * sl + rz * sw;
    };

    LotMesh slab, kerb, paint;

    // ---- THE SLAB. Tiled in 4 m cells rather than emitted as one giant quad:
    // the pad is 44 m long, and a single quad would stretch one texture tile
    // across the whole thing (and give the lighting exactly four normals to
    // work with over 800 m^2).
    {
        const int NL = 12, NW = 6;
        const float nU[3] = { 0, 1, 0 };
        for (int a = 0; a < NL; ++a) {
            for (int b = 0; b < NW; ++b) {
                const float l0 = -kSlHalfLen + 2.0f * kSlHalfLen * (float)a / NL;
                const float l1 = -kSlHalfLen + 2.0f * kSlHalfLen * (float)(a+1) / NL;
                const float w0 = -kSlHalfW  + 2.0f * kSlHalfW  * (float)b / NW;
                const float w1 = -kSlHalfW  + 2.0f * kSlHalfW  * (float)(b+1) / NW;
                float p0[3], p1[3], p2[3], p3[3];
                P(l0, w0, topY, p0); P(l0, w1, topY, p1);
                P(l1, w1, topY, p2); P(l1, w0, topY, p3);
                slab.quad(p0, p1, p2, p3, nU, w0 * 0.12f, w1 * 0.12f,
                          l0 * 0.12f, l1 * 0.12f);
            }
        }
    }

    // ---- THE RIM PRISM, all four edges. Lip, then batter, then a toe buried
    // in the carved floor. On a peak this is the whole silhouette of the lot
    // from below, and without it the slab is a card floating over a dome.
    {
        auto rimEdge = [&](float l0, float w0, float l1, float w1) {
            // Outward normal direction in (along, across) space, for the batter.
            const float el = l1 - l0, ew = w1 - w0;
            const float elen = std::sqrt(el*el + ew*ew);
            if (elen < 1e-4f) return;
            // Outward = edge tangent rotated so it points away from the centre.
            float ol = ew / elen, ow = -el / elen;
            if (ol * (0.5f*(l0+l1)) + ow * (0.5f*(w0+w1)) < 0.0f) { ol = -ol; ow = -ow; }
            const int N = 10;
            for (int k = 0; k < N; ++k) {
                const float t0 = (float)k / N, t1 = (float)(k+1) / N;
                const float al0 = l0 + el*t0, aw0 = w0 + ew*t0;
                const float al1 = l0 + el*t1, aw1 = w0 + ew*t1;
                auto ground = [&](float sl, float sw) {
                    float q[3]; P(sl, sw, 0.0f, q);
                    return terrainHeightAtWorld(q[0], q[2]);
                };
                auto run = [&](float sl, float sw) {
                    const float drop = clampf(topY - ground(sl + ol*2.0f, sw + ow*2.0f),
                                              0.0f, kRimMax);
                    return clampf(drop * kRimSlope, 0.9f, 24.0f);
                };
                const float r0 = run(al0, aw0), r1 = run(al1, aw1);
                auto toe = [&](float sl, float sw) {
                    return std::max(std::min(topY - kRimFace, ground(sl, sw)) - kRimLap,
                                    topY - kRimMax);
                };
                float aT[3], bT[3], aK[3], bK[3], aO[3], bO[3];
                P(al0, aw0, topY, aT);              P(al1, aw1, topY, bT);
                P(al0, aw0, topY - kRimFace, aK);   P(al1, aw1, topY - kRimFace, bK);
                P(al0 + ol*r0, aw0 + ow*r0, toe(al0 + ol*r0, aw0 + ow*r0), aO);
                P(al1 + ol*r1, aw1 + ow*r1, toe(al1 + ol*r1, aw1 + ow*r1), bO);
                kerb.quadN(aT, aK, bK, bT, 0.0f, 0.2f, t0, t1);
                kerb.quadN(aK, aO, bO, bK, 0.2f, 1.0f, t0, t1);
            }
        };
        rimEdge(-kSlHalfLen, -kSlHalfW,  kSlHalfLen, -kSlHalfW);
        rimEdge( kSlHalfLen,  kSlHalfW, -kSlHalfLen,  kSlHalfW);
        rimEdge( kSlHalfLen, -kSlHalfW,  kSlHalfLen,  kSlHalfW);
        rimEdge(-kSlHalfLen,  kSlHalfW, -kSlHalfLen, -kSlHalfW);
    }

    // ---- THE KERB, standing on the rim — except at the MOUTH. The mouth is
    // the near end (the end the spur arrives at, -length), centred on the
    // aisle: a car drives straight off the spur, through the gap, down the
    // aisle. A kerb all the way round would make the lot a photograph.
    {
        auto kerbRun = [&](float l0, float w0, float l1, float w1) {
            const float el = l1 - l0, ew = w1 - w0;
            const float elen = std::sqrt(el*el + ew*ew);
            if (elen < 1e-4f) return;
            float ol = ew / elen, ow = -el / elen;
            if (ol * (0.5f*(l0+l1)) + ow * (0.5f*(w0+w1)) < 0.0f) { ol = -ol; ow = -ow; }
            const float iw = kSlKerbW;
            float a0[3], a1[3], b0[3], b1[3], a0t[3], a1t[3], b0t[3], b1t[3];
            P(l0, w0, topY, a0);                        P(l1, w1, topY, b0);
            P(l0 - ol*iw, w0 - ow*iw, topY, a1);        P(l1 - ol*iw, w1 - ow*iw, topY, b1);
            P(l0, w0, topY + kSlKerbH, a0t);            P(l1, w1, topY + kSlKerbH, b0t);
            P(l0 - ol*iw, w0 - ow*iw, topY + kSlKerbH, a1t);
            P(l1 - ol*iw, w1 - ow*iw, topY + kSlKerbH, b1t);
            kerb.quadN(a0t, a1t, b1t, b0t, 0, 1, 0, 1);   // top
            kerb.quadN(a1t, a1,  b1,  b1t, 0, 1, 0, 1);   // inner face (toward the bays)
            kerb.quadN(a0,  a0t, b0t, b0,  0, 1, 0, 1);   // outer face (over the rim)
        };
        // Both long sides, the far end, and the two stubs of the near end that
        // flank the mouth.
        kerbRun(-kSlHalfLen, -kSlHalfW,  kSlHalfLen, -kSlHalfW);
        kerbRun( kSlHalfLen,  kSlHalfW, -kSlHalfLen,  kSlHalfW);
        kerbRun( kSlHalfLen, -kSlHalfW,  kSlHalfLen,  kSlHalfW);
        const float mh = kSlMouthW * 0.5f;
        kerbRun(-kSlHalfLen,  kSlHalfW, -kSlHalfLen,  mh);
        kerbRun(-kSlHalfLen, -mh,       -kSlHalfLen, -kSlHalfW);
    }

    // ---- BAY MARKINGS. 16 stalls a side, 90 degrees to the aisle, plus the
    // two aisle edge lines. The stripe sits 2 cm over the slab, the same lift
    // the road ribbon's paint uses.
    {
        const float py = topY + 0.02f;
        const float nU[3] = { 0, 1, 0 };
        const float halfStripe = 0.06f;
        const float aisleHalf = kSlAisleW * 0.5f;
        auto stripe = [&](float l, float wA, float wB) {
            float p0[3], p1[3], p2[3], p3[3];
            P(l - halfStripe, wA, py, p0); P(l - halfStripe, wB, py, p1);
            P(l + halfStripe, wB, py, p2); P(l + halfStripe, wA, py, p3);
            paint.quad(p0, p1, p2, p3, nU, 0, 1, 0, 1);
        };
        for (int k = 0; k <= kSlBaysPerSide; ++k) {
            const float l = -kSlHalfLen + 2.0f * kSlHalfLen * (float)k / kSlBaysPerSide;
            stripe(l,  aisleHalf,  kSlHalfW - kSlKerbW);
            stripe(l, -aisleHalf, -(kSlHalfW - kSlKerbW));
        }
        // Aisle edge lines, run the length of the pad.
        for (int side = -1; side <= 1; side += 2) {
            const float w = (float)side * aisleHalf;
            float p0[3], p1[3], p2[3], p3[3];
            P(-kSlHalfLen, w - halfStripe, py, p0); P(-kSlHalfLen, w + halfStripe, py, p1);
            P( kSlHalfLen, w + halfStripe, py, p2); P( kSlHalfLen, w - halfStripe, py, p3);
            paint.quad(p0, p1, p2, p3, nU, 0, 1, 0, 1);
        }
    }

    auto upload = [&](LotMesh& m, const SurfaceSet* set, const float tint[4], bool collide) {
        if (m.empty()) return;
        Entity e;
        e.mesh = device.createMesh(m.v.data(), (uint32_t)m.v.size(),
                                   m.i.data(), (uint32_t)m.i.size());
        if (!e.mesh.valid()) return;
        if (set && set->ok) { e.tex = set->albedo; e.mrTex = set->mr; e.normalTex = set->normal; }
        for (int c = 0; c < 4; ++c) e.baseColor[c] = tint[c];
        scene.add(e);
        if (collide) {
            std::vector<float> cv; cv.reserve(m.v.size() * 3);
            for (const auto& vv : m.v) { cv.push_back(vv.pos[0]); cv.push_back(vv.pos[1]); cv.push_back(vv.pos[2]); }
            phys.addStaticMesh(cv.data(), (uint32_t)(cv.size() / 3),
                               m.i.data(), (uint32_t)m.i.size());
        }
    };

    float slabTint[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    if (!asphalt.ok) { slabTint[0] = 0.055f; slabTint[1] = 0.056f; slabTint[2] = 0.060f; }
    float kerbTint[4] = { 0.86f, 0.85f, 0.82f, 1.0f };
    if (!cement.ok)  { kerbTint[0] = 0.42f; kerbTint[1] = 0.41f; kerbTint[2] = 0.38f; }
    const float paintTint[4] = { 1.8f, 1.8f, 1.7f, 1.0f };
    // CONTACT LAW: both the surface you drive on and the step you walk up.
    upload(slab,  &asphalt, slabTint,  /*collide*/true);
    upload(kerb,  &cement,  kerbTint,  /*collide*/true);
    upload(paint, nullptr,  paintTint, /*collide*/false);

    // A RECEIPT THAT THE PAD WAS DRAWN. registerSummitLot already logs that the
    // lot was PLANNED; nothing said whether the geometry ever reached the
    // scene, and the first eyes-on run found bare hillside where the plan said
    // pavement. Registration and construction are two different claims.
    char b[220];
    std::snprintf(b, sizeof(b),
                  "summit lot BUILT at (%.0f, %.0f, %.0f): slab %zu verts%s, kerb %zu%s, "
                  "paint %zu | asphalt %s, cement %s",
                  (double)lot.cx, (double)topY, (double)lot.cz,
                  slab.v.size(), asphalt.ok ? "" : " (UNTEXTURED fallback)",
                  kerb.v.size(), cement.ok ? "" : " (UNTEXTURED fallback)",
                  paint.v.size(),
                  asphalt.ok ? "ok" : "MISSING", cement.ok ? "ok" : "MISSING");
    x3::logInfo(b);
}

// ---------------------------------------------------------------------------
// --test-summitlot
// ---------------------------------------------------------------------------
bool runSummitLotSelfTest() {
    int passN = 0, failN = 0;
    auto check = [&](bool ok, const char* name, const char* detail) {
        char b[300];
        std::snprintf(b, sizeof(b), "[summitlot] %s %s — %s",
                      ok ? "PASS" : "FAIL", name, detail ? detail : "");
        if (ok) { ++passN; x3::logInfo(b); }
        else    { ++failN; x3::logError(b); }
    };
    char d[220];

    clearTerrainCorridors();

    // THE LOT NEEDS THE WORLD IT REALLY LIVES IN, booted in host_tunnel.cpp's
    // order: the spawn bore first (the spur is required to keep clear of it),
    // then the inner course, then the connector, then the spur — which is tried
    // off the connector and FALLS BACK to the ring, exactly as the host does,
    // because the connector runs through rolling lowland with no peak in reach.
    // A lot that only exists in a test-shaped world proves nothing.
    const TunnelRoute& bore = registerTunnelCorridor();
    RoadSpec ringSpec = makeInnerCourse();
    std::vector<float> ringRoadY;
    const RoadBuildResult rr = registerRoad(ringSpec, &ringRoadY);
    check(rr.ok, "L0 the inner course the spur branches from is carved",
          rr.ok ? "ring registered" : "registerRoad failed");
    SpawnConnectorResult conn = registerSpawnConnector(bore, ringSpec, ringRoadY);

    std::vector<const RoadSpec*> avoid{ &conn.spec };
    SummitSpurResult spur = registerSummitSpur(conn.spec, conn.roadY, &bore, &avoid);
    if (!spur.built) spur = registerSummitSpur(ringSpec, ringRoadY, &bore, &avoid);
    std::snprintf(d, sizeof(d), "%s", spur.built ? "spur built" : spur.whyNot);
    check(spur.built, "L1 the summit spur reaches a peak (there is a summit to park on)", d);
    if (!spur.built) {
        clearTerrainCorridors();
        x3::logInfo("[summitlot] " + std::to_string(passN) + " passed, " +
                    std::to_string(failN + 1) + " failed");
        return false;
    }

    const uint32_t before = terrainCorridorCount();
    SummitLotResult lot = registerSummitLot(spur);
    std::snprintf(d, sizeof(d), "%s", lot.built ? "registered" : lot.whyNot);
    check(lot.built, "L2 the lot registers on the spur's summit", d);
    if (!lot.built) {
        clearTerrainCorridors();
        x3::logInfo("[summitlot] " + std::to_string(passN) + " passed, " +
                    std::to_string(failN + 1) + " failed");
        return false;
    }
    std::snprintf(d, sizeof(d), "corridors %u -> %u", before, terrainCorridorCount());
    check(terrainCorridorCount() == before + 1,
          "L3 the pad is CARVED — exactly one corridor, not a slab on a dome", d);

    // L4 — THE FLOOR IS FLAT AND IT IS UNDER THE SLAB. Walk the pad and measure
    // the carved field against the datum. Every sample must sit below the slab
    // (nothing standing on the tarmac) and none may sit so far below that the
    // rim prism cannot reach it.
    const float tx = lot.dirX, tz = lot.dirZ, rx = -tz, rz = tx;
    float worstAbove = -1e9f, worstBelow = 0.0f;
    for (int li = -10; li <= 10; ++li) {
        for (int wi = -6; wi <= 6; ++wi) {
            const float sl = (float)li * kSlHalfLen / 10.0f;
            const float sw = (float)wi * kSlHalfW  / 6.0f;
            const float qx = lot.cx + tx * sl + rx * sw;
            const float qz = lot.cz + tz * sl + rz * sw;
            const float dy = terrainHeightAtWorld(qx, qz) - lot.y;
            worstAbove = std::max(worstAbove, dy);
            worstBelow = std::max(worstBelow, -dy);
        }
    }
    std::snprintf(d, sizeof(d), "worst ground-above-slab %.2f m, worst drop %.2f m (rim reach %.1f m)",
                  worstAbove, worstBelow, kRimMax);
    check(worstAbove <= -0.02f && worstBelow < kRimMax,
          "L4 the carved floor is under the slab everywhere and inside the rim's reach", d);

    // L5 — THE MOUNTAIN SURVIVES. A lot is a place on a peak, not a decapitated
    // peak. Cut ceiling, same guard the bore's D2 keeps over the massif.
    std::snprintf(d, sizeof(d), "deepest cut %.1f m (ceiling %.1f m)", lot.cutM, kSlMaxCutM);
    check(lot.cutM <= kSlMaxCutM, "L5 seating the pad does not gouge the peak away", d);

    // L6 — IT IS ACTUALLY UP A MOUNTAIN. The spur's own gate proves the climb;
    // this proves the LOT inherited it, i.e. the pad did not get placed on some
    // shoulder hundreds of feet below the peak the spur worked to reach.
    const float belowPeak = spur.peakNaturalY - lot.y;
    std::snprintf(d, sizeof(d), "pad datum %.0f ft, peak %.0f ft, pad sits %.0f ft below it",
                  lot.y / 0.3048f, spur.peakNaturalY / 0.3048f, belowPeak / 0.3048f);
    check(belowPeak < 25.0f && lot.y > 60.0f,
          "L6 the pad is ON the summit, not on a shoulder below it", d);

    // L7 — YOU CAN DRIVE IN. The mouth is where the spur's last node meets the
    // pad's near rim; the step there must be inside chassis clearance, measured
    // the way --test-tunneldrive D5b measures a road mount: the largest single
    // upward step walking in along the aisle, 0.4 m apart.
    //
    // AGAINST THE DRIVABLE SURFACE, not the height field. First cut of this
    // check sampled terrainHeightAtWorld() outside the pad and read a 2.43 m
    // step — but a car arriving here is on the SPUR'S PAVEMENT, a slab at the
    // spur's graded datum, and the field beneath it is the trench that slab
    // spans. Measuring the field there is measuring our own excavation, the
    // identical mistake road_network's O6b was making (fixed this same session).
    // The surface a wheel touches is max(field, pavement), so that is what this
    // walks.
    float worstStep = 0.0f, worstAt = 0.0f;
    {
        const size_t sn = spur.spec.x.size();
        // Height of the spur's pavement at (x,z), or -inf if off the pavement.
        auto spurPaveAt = [&](float qx, float qz) {
            float best = -1e9f;
            for (size_t k = 0; k + 1 < sn; ++k) {
                const float ex = spur.spec.x[k+1] - spur.spec.x[k];
                const float ez = spur.spec.z[k+1] - spur.spec.z[k];
                const float L2 = ex*ex + ez*ez;
                if (L2 < 1e-6f) continue;
                float t = ((qx - spur.spec.x[k]) * ex + (qz - spur.spec.z[k]) * ez) / L2;
                t = clampf(t, 0.0f, 1.0f);
                const float px = spur.spec.x[k] + ex * t, pz = spur.spec.z[k] + ez * t;
                const float dx = qx - px, dz = qz - pz;
                if (dx*dx + dz*dz <= kPavedHalfM * kPavedHalfM)
                    best = std::max(best, spur.roadY[k] + (spur.roadY[k+1] - spur.roadY[k]) * t);
            }
            return best;
        };
        float prev = 0.0f; bool have = false;
        for (float sl = -kSlHalfLen - 12.0f; sl <= -kSlHalfLen + 4.0f; sl += 0.4f) {
            const float qx = lot.cx + tx * sl, qz = lot.cz + tz * sl;
            // Inside the pad the surface is the slab; outside it is whichever of
            // the field and the spur's pavement is higher.
            const float h = (sl >= -kSlHalfLen)
                ? lot.y + 0.02f
                : std::max(terrainHeightAtWorld(qx, qz), spurPaveAt(qx, qz));
            if (have && h - prev > worstStep) { worstStep = h - prev; worstAt = sl; }
            prev = h; have = true;
        }
    }
    std::snprintf(d, sizeof(d), "worst step into the mouth %.2f m at %.1f m from centre (clearance 0.45 m)",
                  worstStep, worstAt);
    check(worstStep <= 0.45f, "L7 a car can drive in — the mouth step is inside chassis clearance", d);

    // L8 — the lot is marked out as what it claims to be.
    std::snprintf(d, sizeof(d), "%d stalls, bay %.2f x %.2f m, aisle %.2f m, pad %.1f x %.1f m",
                  lot.stalls, kSlBayW, kSlBayDepth, kSlAisleW,
                  kSlHalfLen * 2.0f, kSlHalfW * 2.0f);
    check(lot.stalls == kSlBaysPerSide * 2 &&
          kSlBaysPerSide * kSlBayW <= kSlHalfLen * 2.0f + 0.01f,
          "L8 the stalls it claims actually fit on the pad", d);

    clearTerrainCorridors();
    x3::logInfo("[summitlot] " + std::to_string(passN) + " passed, " +
                std::to_string(failN) + " failed");
    return failN == 0;
}

} // namespace x3::game
