// ECHO ROADS implementation — see echo_roads.h for the stance + INTEGRATION.
//
// V3 ARCHITECTURE (Tim: "the roads near the end of the bridge do not even
// join each other. This is NOT how roads look."):
//   PHASE 1  COLLECT   every edge's centerline (ring / trumpet ramps / gate
//                      avenues / spurs / harbor boulevard / fanned grid
//                      blocks) into Pending records — NO geometry yet. V4:
//                      the ring is a mesa-rim + flats loop (fixed NE/E arc +
//                      probed SW rim waypoints — see the kRingFixed comment);
//                      the harbor probes the real waterline from seeds.
//   PHASE 2  JUNCTIONS detect where edges meet:
//                        a) ENDPOINT CAPTURES — an edge end inside another
//                           ground edge's corridor (tee / ramp foot / gate).
//                           Stop-short ends are EXTENDED straight into the
//                           target corridor first (the "bridge end" fix).
//                        b) INTERIOR CROSSINGS — per ground-edge pair, the
//                           global closest-approach under the corridor sum
//                           (grid street X, street x boulevard).
//                      Candidates cluster (14 m) into junctions; the freeway
//                      ring is grade-separated and never patched (ramp deck
//                      merges stay tangential overlap by design).
//   PHASE 3  EMIT      per-edge ribbons with per-sample SUPPRESSION inside
//                      junction patches (asphalt/paint/curbs/sidewalks all
//                      trim; lane paint BREAKS; stop bars at every entry),
//                      then the filled 12-gon junction patches, lamp poles +
//                      arms under every light, pillars, barriers.
//   COLLISION          the asphalt top surface (decks/ramps/streets/patches)
//                      accumulates into RoadCollisionMesh for the integrator
//                      to feed phys->addStaticMesh (the fall-through fix).
//
// Determinism: pure math over authored constants + h01() hash — no rand.

#include "echo_roads.h"

#include "engine/core/x3_log.h"
#include "../asset_root.h"     // V7: texture tiles resolve under assetRoot()/roads
#include <stb_image.h>          // V7: PNG decode (STBI_NO_STDIO — memory loader)
#include <fstream>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace x3::game {

namespace {

// ---- tunables (all of them; nothing magic inline) --------------------------
constexpr float kSampleStep     = 4.0f;
constexpr float kRampStep       = 2.0f;
constexpr float kFreewayWidth   = 14.0f;
constexpr float kRampWidth      = 7.0f;
constexpr float kAvenueWidth    = 9.0f;
constexpr float kStreetWidth    = 9.0f;
constexpr float kLaneWidth      = 3.4f;
constexpr float kDeckClearance  = 11.0f;
constexpr float kDeckMinY       = 13.0f;
// V3.2 (integrator): was 38.0 — wide enough to catch the MESA CLIFF on legs
// running PARALLEL to it (west shanty shore), inheriting 195m of floor the
// road never crosses -> the sky-pier forest. Probe only what the DECK ITSELF
// spans: half deck width + shoulder. Legs that genuinely cross a ridge still
// catch it dead-ahead along the centerline samples.
constexpr float kRidgeProbe     = 9.0f;
constexpr float kMaxGrade       = 0.06f;   // (legacy reference; ramps use their own)
// V3.1: the PROFILE grade. 6% could not descend the ~200 m crown within the
// available run, so the raise-only relaxation held the deck at ridge height
// across the whole shore bowl (ranks of ~200 m piers — Tim's capture). 14%
// is steeper than a real interstate but matches the legacy per-leg look and
// lets the deck hug terrain down the mesa flank; piers return to real scale.
// V4: 22%. The rim route removed the cliff descents that drove this to
// absurd values during v3.1 tuning (0.60 still logged ~172 m piers — proof
// the ROUTE was the bug, not the grade). What remains is one flats->rim
// flank climb on the south-east, which 22% covers while the deck hugs the
// probed terrain; everywhere else the profile rides ground+clearance.
constexpr float kDeckMaxGrade   = 0.22f;
constexpr float kRampMaxGrade   = 0.07f;
constexpr float kBankPerKappa   = 55.0f;
constexpr float kBankMax        = 0.10f;
// (kSmoothWin retired in V3.1 — the profile's grade bound IS the smoothness;
//  box smoothing was re-inflating valleys back toward summit height.)
constexpr float kBarrierInset   = 0.35f;
// V7.2 (Tim, on the gate vista): a REAL breakdown lane, not a worn strip —
// "what if your tranny blows on your stolen joy ride?" CA standard ~3 m:
// room to pull a car fully out of lane 2 against the barrier.
constexpr float kShoulderW      = 3.2f;
constexpr float kBarrierW       = 0.35f;
constexpr float kBarrierH       = 1.00f;
constexpr float kEdgeLineInset  = 0.90f;
constexpr float kPaintW         = 0.16f;
constexpr float kPaintLift      = 0.03f;
constexpr float kDashOn         = 3.0f, kDashOff = 9.0f;
constexpr float kStreetDashOn   = 2.0f, kStreetDashOff = 6.0f;
// V7.2 (Tim): CA viaducts run MUCH longer unsupported spans — half the pier
// count, each pier a touch heavier so the longer span reads structural.
constexpr float kPillarEvery    = 70.0f;
constexpr float kPillarHalf     = 1.55f;
constexpr float kPillarMinAir   = 3.0f;
// V7.3 STRUCTURAL PASS (slop verdict: "paper ribbon on black toothpicks"):
// the elevated deck gets a real BOX SECTION — side fascia + soffit at this
// depth — and piers get footing/taper/hammerhead (see pillar()).
constexpr float kDeckDepth      = 1.8f;    // fascia/soffit drop below deck top
constexpr float kCapDepth       = 1.7f;    // hammerhead cap beam height
constexpr float kCapHalfLen     = 5.2f;    // cap half-length across the deck
constexpr float kCurbW          = 0.35f, kCurbLift = 0.13f;
constexpr float kWalkW          = 1.80f, kWalkLift = 0.12f;
constexpr float kGroundLift     = 0.15f;
constexpr float kLampEveryFwy   = 34.0f;
constexpr float kLampEveryStr   = 26.0f;
constexpr float kLampHFwy       = 6.0f;
constexpr float kLampHStr       = 4.2f;
constexpr float kPoleHalf       = 0.10f;   // lamp pole shaft half-extent
constexpr float kArmLen         = 0.9f;    // lamp arm reach toward the road
constexpr float kWaterMinLand   = 1.5f;
constexpr float kLoopR          = 30.0f;
constexpr float kLoopSweepDeg   = 250.0f;
// V3 junctions:
constexpr float kJuncCapture    = 0.70f;   // endpoint capture: d < this*(wA+wB)/2
constexpr float kJuncCross      = 0.55f;   // crossing: minDist < this*(wA+wB)/2
constexpr float kJuncCluster    = 14.0f;   // candidates within this merge
constexpr float kPatchApron     = 3.0f;    // patch radius = max halfwidth + this
constexpr float kPatchMinR      = 7.0f;
constexpr float kPatchTuck      = 1.2f;    // ribbons trim to r - tuck (no crack)
constexpr float kStopBarW       = 0.5f;    // stop-line thickness (along tangent)
constexpr int   kPatchSides     = 12;      // junction polygon fan

// V4 REROUTE (the real fix behind the pier-forest saga): the legacy route's
// west/south legs ((700,420)->(300,430)->(-60,560)) crossed the SHORE BOWL at
// mesa-approach height — no grade can descend a 195 m cliff without either a
// 172 m pier forest through the shanty village (v3.1's honest log proved it)
// or a wall of earthworks. V4 splits responsibilities the way real coastal
// cities do:
//   * the FREEWAY stays HIGH: the fixed NE/E arc (crown crossing + the gentle
//     north-east slope descent + the flats sweep) is kept verbatim, and the
//     west/south side is replaced by RADIALLY PROBED MESA-RIM waypoints
//     (V4.1) — per bearing from the crown center, an outward march finds the
//     LAST point at ~80% of crown elevation (the true cliff lip, whatever the
//     arc's shape), and the waypoint sits 45 m inside it. The deck rides the
//     rim (piers ~11-18 m; short viaducts over draws only). Degenerate
//     bearings and backtrack pockets drop out with logs (convexify pass).
//   * the SHORE belongs to the ground-level Harbor Boulevard (v2), which
//     already serves the shanty arc the old freeway overflew.
//   * the URBAN GATE needs NO code change: nearestRingSample() re-finds the
//     closest deck point on the new route (the flats/flank leg) and the
//     trumpet lead auto-lengthens via kRampMaxGrade — judgment call: keep the
//     gate at (700,452) fed from the EAST approach (gentle terrain), exactly
//     what the reroute order suggested.
struct Wp { float x, z; };
constexpr Wp kRingFixed[] = {          // crown crossing + NE descent + flats
    { -160.0f,  720.0f }, {  120.0f,  720.0f }, {  480.0f,  900.0f },
    {  820.0f, 1120.0f }, { 1060.0f,  900.0f }, {  980.0f,  560.0f },
};
constexpr int kRingFixedN = (int)(sizeof(kRingFixed) / sizeof(kRingFixed[0]));
// V4.1: RADIAL rim probe. The fixed-x march FAILED in the field (4/6 seeds
// skipped, ring still spanned the bowl, 183 m piers): the mesa is the
// NORTHWEST quadrant — its cliff arc curves from ~(250,450) around the west
// to ~(-500,1100) — so due-north lines at eastern x never touch mesa.
// Probing radially FROM THE CROWN traces the ACTUAL cliff arc whatever its
// shape: walk OUTWARD per bearing; the rim is the LAST sample still at rim
// elevation (inner dips don't fool it); waypoint sits kRimInset INSIDE it.
constexpr float kCrownX      = -20.0f, kCrownZ = 760.0f;  // probe origin (mesa datum)
constexpr float kRimFrac     = 0.80f;    // "rim" = this * crown elevation
constexpr float kRimInset    = 45.0f;    // waypoint this far inside the lip
constexpr float kRimBearing0 = 140.0f;   // NW ...
constexpr float kRimBearing1 = 320.0f;   // ... to SE (0 deg = +x east, CCW, +z north)
constexpr float kRimBearingStep = 20.0f;
constexpr float kRimMaxR     = 700.0f;   // outward march limit (8 m steps)
constexpr float kRimMinR     = 120.0f;   // rim closer than this = degenerate bearing
constexpr float kRimTurnDrop = -0.17f;   // convexify: drop waypoint when turn dot < this (~>100 deg)

inline float h01(uint32_t n) {
    n = (n ^ 61u) ^ (n >> 16); n *= 9u; n ^= n >> 4; n *= 0x27d4eb2du;
    n ^= n >> 15; return (float)(n & 0xffffffu) / (float)0x1000000;
}
inline float clampf2(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

inline void catmull(const Wp* p, int n, int i, float t, float& ox, float& oz) {
    const Wp& p0 = p[(i - 1 + n) % n]; const Wp& p1 = p[i];
    const Wp& p2 = p[(i + 1) % n];     const Wp& p3 = p[(i + 2) % n];
    const float t2 = t * t, t3 = t2 * t;
    ox = 0.5f * ((2*p1.x) + (-p0.x + p2.x)*t + (2*p0.x - 5*p1.x + 4*p2.x - p3.x)*t2
                 + (-p0.x + 3*p1.x - 3*p2.x + p3.x)*t3);
    oz = 0.5f * ((2*p1.z) + (-p0.z + p2.z)*t + (2*p0.z - 5*p1.z + 4*p2.z - p3.z)*t2
                 + (-p0.z + 3*p1.z - 3*p2.z + p3.z)*t3);
}

inline void hermite(float ax, float az, float atx, float atz,
                    float bx, float bz, float btx, float btz,
                    float t, float& ox, float& oz) {
    const float t2 = t * t, t3 = t2 * t;
    const float h00 = 2*t3 - 3*t2 + 1, h10 = t3 - 2*t2 + t;
    const float h01_ = -2*t3 + 3*t2,   h11 = t3 - t2;
    ox = h00*ax + h10*atx + h01_*bx + h11*btx;
    oz = h00*az + h10*atz + h01_*bz + h11*btz;
}

inline void rperp(float tx, float tz, float& px, float& pz) { px = tz; pz = -tx; }

inline float deckFloor(const Heightfield& hf, float x, float z) {
    float g = hf.heightAt(x, z);
    for (int s = 0; s < 4; ++s)
        g = std::max(g, hf.heightAt(x + (s % 2 ? kRidgeProbe : -kRidgeProbe),
                                    z + (s < 2 ? kRidgeProbe : -kRidgeProbe)));
    return std::max(g, 2.0f) + kDeckClearance;
}

void resample(const std::vector<RoadSample>& dense, float step,
              std::vector<RoadSample>& out) {
    out.clear();
    if (dense.size() < 2) return;
    float carry = 0.0f;
    out.push_back(dense.front());
    for (size_t i = 1; i < dense.size(); ++i) {
        float ax = dense[i-1].x, az = dense[i-1].z;
        const float bx = dense[i].x, bz = dense[i].z;
        float dx = bx - ax, dz = bz - az;
        float len = std::sqrt(dx*dx + dz*dz);
        while (carry + len >= step) {
            const float need = step - carry;
            const float f = need / len;
            ax += dx * f; az += dz * f;
            RoadSample s; s.x = ax; s.z = az;
            out.push_back(s);
            dx = bx - ax; dz = bz - az;
            len = std::sqrt(dx*dx + dz*dz);
            carry = 0.0f;
        }
        carry += len;
    }
    const size_t n = out.size();
    for (size_t i = 0; i < n; ++i) {
        const RoadSample& p = out[i > 0 ? i - 1 : 0];
        const RoadSample& q = out[i + 1 < n ? i + 1 : n - 1];
        float tx = q.x - p.x, tz = q.z - p.z;
        const float l = std::sqrt(tx*tx + tz*tz);
        if (l > 1e-5f) { tx /= l; tz /= l; }
        out[i].tx = tx; out[i].tz = tz;
    }
}

// ============================ geometry emitters ============================
struct Buck { std::vector<x3::rhi::MeshVertex>* v; std::vector<uint32_t>* i; };

// V6 WELD — winding escape hatch: if top surfaces cull wrong on the real
// device (this module cannot query cull mode from here), flip this ONE
// constant and every single-sided top face reverses.
constexpr bool kFlipTopWinding = false;

// V6 WELD — unwelded-equivalent vert counter (the v3 emitters spent 4 verts
// per quad); reset each build(), logged against the welded actual.
uint32_t g_unweldedEquiv = 0;

inline void pushVert(Buck& bk, float x, float y, float z,
                     float nx2, float ny, float nz2, float u, float v) {
    x3::rhi::MeshVertex mv;
    mv.pos[0]=x; mv.pos[1]=y; mv.pos[2]=z;
    mv.normal[0]=nx2; mv.normal[1]=ny; mv.normal[2]=nz2;
    mv.uv[0]=u; mv.uv[1]=v;
    bk.v->push_back(mv);
}
inline void pushTri(Buck& bk, uint32_t a, uint32_t b, uint32_t c, bool flip) {
    if (flip) { bk.i->push_back(a); bk.i->push_back(c); bk.i->push_back(b); }
    else      { bk.i->push_back(a); bk.i->push_back(b); bk.i->push_back(c); }
}

// One DOUBLE-SIDED visual quad; optionally also a SINGLE-SIDED collision quad.
// (Retained for dashes/box4 — small-volume emitters; ribbons + barriers weld.)
inline void quad(Buck& bk, const float A[3], const float B[3],
                 const float C[3], const float D[3],
                 const float n[3], float v0, float v1,
                 RoadCollisionMesh* col = nullptr) {
    const uint32_t base = (uint32_t)bk.v->size();
    auto push = [&](const float p[3], float u, float vv) {
        x3::rhi::MeshVertex mv;
        mv.pos[0]=p[0]; mv.pos[1]=p[1]; mv.pos[2]=p[2];
        mv.normal[0]=n[0]; mv.normal[1]=n[1]; mv.normal[2]=n[2];
        mv.uv[0]=u; mv.uv[1]=vv;
        bk.v->push_back(mv);
    };
    push(A, 0.0f, v0); push(B, 1.0f, v0); push(C, 1.0f, v1); push(D, 0.0f, v1);
    const uint32_t q[12] = { base,base+1,base+2, base,base+2,base+3,
                             base,base+2,base+1, base,base+3,base+2 };
    bk.i->insert(bk.i->end(), q, q + 12);
    g_unweldedEquiv += 4;
    if (col) {
        const uint32_t cb = (uint32_t)(col->verts.size() / 3);
        const float* ps[4] = { A, B, C, D };
        for (int k = 0; k < 4; ++k) {
            col->verts.push_back(ps[k][0]);
            col->verts.push_back(ps[k][1]);
            col->verts.push_back(ps[k][2]);
        }
        const uint32_t ct[6] = { cb, cb+1, cb+2, cb, cb+2, cb+3 };
        col->indices.insert(col->indices.end(), ct, ct + 6);
    }
}

// V6 WELDED ribbon: 2 SHARED verts per sample (was 4 per segment), smooth
// per-vertex bank-tilted normals, CONTINUOUS arc-length UV (u across, v =
// meters/10), SINGLE-SIDED +Y top (winding verified analytically: for
// A=left/near, forward x right = +Y — kFlipTopWinding is the hatch).
// Elevated asphalt passes underside=true for an INDEX-ONLY underside
// (shared verts, reversed winding — the same shading compromise the old
// double-sided quads had, at zero extra verts). Collision welds too.
void ribbon(Buck bk, const std::vector<RoadSample>& s, float off, float w,
            float lift, bool applyBank = true, RoadCollisionMesh* col = nullptr,
            bool underside = false) {
    const size_t n = s.size();
    if (n < 2) return;
    const uint32_t base = (uint32_t)bk.v->size();
    uint32_t cbase = 0;
    if (col) cbase = (uint32_t)(col->verts.size() / 3);
    float arc = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        const RoadSample& a = s[i];
        float px, pz; rperp(a.tx, a.tz, px, pz);
        const float b = applyBank ? a.bank : 0.0f;
        const float sb = std::sin(b), cb = std::cos(b);
        const float nX = -sb * px, nY = cb, nZ = -sb * pz;   // bank-tilted up
        const float aL = off - w * 0.5f, aR = off + w * 0.5f;
        if (i > 0) {
            const float dx = a.x - s[i-1].x, dz = a.z - s[i-1].z;
            arc += std::sqrt(dx*dx + dz*dz);
        }
        pushVert(bk, a.x + px*aL, a.y + lift + sb*aL, a.z + pz*aL,
                 nX, nY, nZ, 0.0f, arc / 10.0f);
        pushVert(bk, a.x + px*aR, a.y + lift + sb*aR, a.z + pz*aR,
                 nX, nY, nZ, 1.0f, arc / 10.0f);
        if (col) {
            col->verts.push_back(a.x + px*aL); col->verts.push_back(a.y + lift + sb*aL);
            col->verts.push_back(a.z + pz*aL);
            col->verts.push_back(a.x + px*aR); col->verts.push_back(a.y + lift + sb*aR);
            col->verts.push_back(a.z + pz*aR);
        }
    }
    for (size_t i = 0; i + 1 < n; ++i) {
        const uint32_t L0 = base + (uint32_t)(2*i),     R0 = L0 + 1;
        const uint32_t L1 = base + (uint32_t)(2*(i+1)), R1 = L1 + 1;
        pushTri(bk, L0, L1, R1, kFlipTopWinding);
        pushTri(bk, L0, R1, R0, kFlipTopWinding);
        if (underside) {
            pushTri(bk, L0, R1, L1, kFlipTopWinding);
            pushTri(bk, L0, R0, R1, kFlipTopWinding);
        }
        if (col) {
            const uint32_t cL0 = cbase + (uint32_t)(2*i),     cR0 = cL0 + 1;
            const uint32_t cL1 = cbase + (uint32_t)(2*(i+1)), cR1 = cL1 + 1;
            col->indices.push_back(cL0); col->indices.push_back(cL1); col->indices.push_back(cR1);
            col->indices.push_back(cL0); col->indices.push_back(cR1); col->indices.push_back(cR0);
        }
        g_unweldedEquiv += 4;
    }
}

void dashes(Buck bk, const std::vector<RoadSample>& s, float off, float w,
            float lift, float onLen, float offLen) {
    if (s.size() < 2) return;
    const float n[3] = { 0, 1, 0 };
    const float cyc = onLen + offLen;
    float arc = 0.0f;
    for (size_t i = 0; i + 1 < s.size(); ++i) {
        const RoadSample& a = s[i]; const RoadSample& b = s[i + 1];
        const float dx = b.x - a.x, dz = b.z - a.z;
        const float seg = std::sqrt(dx*dx + dz*dz);
        const float phase = std::fmod(arc, cyc);
        if (phase < onLen) {
            float apx, apz, bpx, bpz; rperp(a.tx,a.tz,apx,apz); rperp(b.tx,b.tz,bpx,bpz);
            const float L = off - w*0.5f, R = off + w*0.5f;
            const float A[3] = { a.x+apx*L, a.y+lift+std::sin(a.bank)*L, a.z+apz*L };
            const float B[3] = { a.x+apx*R, a.y+lift+std::sin(a.bank)*R, a.z+apz*R };
            const float C[3] = { b.x+bpx*R, b.y+lift+std::sin(b.bank)*R, b.z+bpz*R };
            const float D[3] = { b.x+bpx*L, b.y+lift+std::sin(b.bank)*L, b.z+bpz*L };
            quad(bk, A, B, C, D, n, 0.0f, seg / 10.0f);
        }
        arc += seg;
    }
}

// V6 WELDED barrier: 6 shared verts per sample (outer pair, inner pair, cap
// pair — each with its own face normal), 3 faces per segment. Faces stay
// DOUBLE-SIDED: a jersey barrier is a thin wall legitimately seen from both
// sides; verts are still 4x fewer than the old per-quad emission.
void barrier(Buck bk, const std::vector<RoadSample>& s, float off) {
    const size_t n = s.size();
    if (n < 2) return;
    const uint32_t base = (uint32_t)bk.v->size();
    const float hw = kBarrierW * 0.5f;
    for (size_t i = 0; i < n; ++i) {
        const RoadSample& a = s[i];
        float px, pz; rperp(a.tx, a.tz, px, pz);
        const float bl = std::sin(a.bank);
        auto P = [&](float lat, float up, float& X, float& Y, float& Z) {
            X = a.x + px * lat; Y = a.y + bl * lat + up; Z = a.z + pz * lat;
        };
        float X, Y, Z;
        P(off + hw, 0.0f, X, Y, Z);        pushVert(bk, X,Y,Z,  px,0,pz, 0.0f, 0.0f);
        P(off + hw, kBarrierH, X, Y, Z);   pushVert(bk, X,Y,Z,  px,0,pz, 0.0f, 0.4f);
        P(off - hw, 0.0f, X, Y, Z);        pushVert(bk, X,Y,Z, -px,0,-pz, 0.0f, 0.0f);
        P(off - hw, kBarrierH, X, Y, Z);   pushVert(bk, X,Y,Z, -px,0,-pz, 0.0f, 0.4f);
        P(off + hw, kBarrierH, X, Y, Z);   pushVert(bk, X,Y,Z,  0,1,0,    0.0f, 0.0f);
        P(off - hw, kBarrierH, X, Y, Z);   pushVert(bk, X,Y,Z,  0,1,0,    1.0f, 0.0f);
    }
    for (size_t i = 0; i + 1 < n; ++i) {
        const uint32_t a0 = base + (uint32_t)(6*i), b0 = base + (uint32_t)(6*(i+1));
        auto quad2 = [&](uint32_t p, uint32_t q, uint32_t r, uint32_t t) {
            pushTri(bk, p, q, r, false); pushTri(bk, p, r, t, false);   // front
            pushTri(bk, p, r, q, false); pushTri(bk, p, t, r, false);   // back
        };
        quad2(a0 + 0, b0 + 0, b0 + 1, a0 + 1);   // outer face
        quad2(a0 + 2, b0 + 2, b0 + 3, a0 + 3);   // inner face
        quad2(a0 + 4, b0 + 4, b0 + 5, a0 + 5);   // cap
        g_unweldedEquiv += 12;
    }
}

// V7.3: the elevated deck's BOX SECTION — side fascia walls + a soffit at
// -kDeckDepth, welded per sample like ribbon(). Kills the knife-edge/monorail
// read (slop verdict: "the deck has literally zero thickness"). `w` = the FULL
// top surface (lanes + shoulders); banked edges follow the superelevation.
void deckFascia(Buck bk, const std::vector<RoadSample>& s, float w,
                bool applyBank = true) {
    const size_t n = s.size();
    if (n < 2) return;
    const uint32_t base = (uint32_t)bk.v->size();
    float arc = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        const RoadSample& a = s[i];
        float px, pz; rperp(a.tx, a.tz, px, pz);
        const float b = applyBank ? a.bank : 0.0f;
        const float sb = std::sin(b);
        const float aL = -w * 0.5f, aR = w * 0.5f;
        if (i > 0) {
            const float dx = a.x - s[i-1].x, dz = a.z - s[i-1].z;
            arc += std::sqrt(dx*dx + dz*dz);
        }
        const float v = arc / 10.0f;
        // 6 verts/sample: topL, botL (left fascia, -perp normal), botL, botR
        // (soffit, -Y), botR, topR (right fascia, +perp normal).
        const float xL = a.x + px*aL, yL = a.y + sb*aL, zL = a.z + pz*aL;
        const float xR = a.x + px*aR, yR = a.y + sb*aR, zR = a.z + pz*aR;
        pushVert(bk, xL, yL,             zL, -px, 0, -pz, 0.0f, v);
        pushVert(bk, xL, yL - kDeckDepth, zL, -px, 0, -pz, 0.18f, v);
        pushVert(bk, xL, yL - kDeckDepth, zL, 0, -1, 0, 0.0f, v);
        pushVert(bk, xR, yR - kDeckDepth, zR, 0, -1, 0, 1.0f, v);
        pushVert(bk, xR, yR - kDeckDepth, zR,  px, 0,  pz, 0.18f, v);
        pushVert(bk, xR, yR,             zR,  px, 0,  pz, 0.0f, v);
    }
    for (size_t i = 0; i + 1 < n; ++i) {
        const uint32_t a0 = base + (uint32_t)(6*i), b0 = base + (uint32_t)(6*(i+1));
        auto face = [&](uint32_t p, uint32_t q, uint32_t r, uint32_t t) {
            pushTri(bk, p, q, r, false); pushTri(bk, p, r, t, false);
            pushTri(bk, p, r, q, false); pushTri(bk, p, t, r, false);
        };
        face(a0 + 0, b0 + 0, b0 + 1, a0 + 1);   // left fascia
        face(a0 + 2, b0 + 2, b0 + 3, a0 + 3);   // soffit
        face(a0 + 4, b0 + 4, b0 + 5, a0 + 5);   // right fascia
        g_unweldedEquiv += 12;
    }
}

// V7.3: an ORIENTED box (hammerhead cap) — axis (ax,az) horizontal, half
// extents halfA along the axis / halfB across, y0..y1 tall. 4 sides + lid +
// soffit via quad() (cheap; caps are rare).
void obox(Buck bk, float cx, float cz, float y0, float y1,
          float ax, float az, float halfA, float halfB) {
    const float bx2 = -az, bz2 = ax;   // horizontal perp
    const float cor[4][2] = {
        { cx + ax*halfA + bx2*halfB, cz + az*halfA + bz2*halfB },
        { cx + ax*halfA - bx2*halfB, cz + az*halfA - bz2*halfB },
        { cx - ax*halfA - bx2*halfB, cz - az*halfA - bz2*halfB },
        { cx - ax*halfA + bx2*halfB, cz - az*halfA + bz2*halfB },
    };
    for (int f = 0; f < 4; ++f) {
        const int g = (f + 1) % 4;
        const float A[3] = { cor[f][0], y0, cor[f][1] };
        const float B[3] = { cor[g][0], y0, cor[g][1] };
        const float C[3] = { cor[g][0], y1, cor[g][1] };
        const float D[3] = { cor[f][0], y1, cor[f][1] };
        const float n[3] = { cor[f][0] + cor[g][0] - 2*cx, 0,
                             cor[f][1] + cor[g][1] - 2*cz };
        quad(bk, A, B, C, D, n, 0.0f, (y1 - y0) / 10.0f);
    }
    const float T0[3] = { cor[0][0], y1, cor[0][1] }, T1[3] = { cor[1][0], y1, cor[1][1] };
    const float T2[3] = { cor[2][0], y1, cor[2][1] }, T3[3] = { cor[3][0], y1, cor[3][1] };
    const float nu[3] = { 0, 1, 0 }, nd[3] = { 0, -1, 0 };
    quad(bk, T0, T1, T2, T3, nu, 0.0f, 1.0f);
    const float B0[3] = { cor[0][0], y0, cor[0][1] }, B1[3] = { cor[1][0], y0, cor[1][1] };
    const float B2[3] = { cor[2][0], y0, cor[2][1] }, B3[3] = { cor[3][0], y0, cor[3][1] };
    quad(bk, B3, B2, B1, B0, nd, 0.0f, 1.0f);
}

// Square shaft between two heights (piers AND lamp poles share this).
void box4(Buck bk, float x, float z, float y0, float y1, float half) {
    if (y1 <= y0) return;
    const float c[4][2] = { {-half,-half}, {half,-half}, {half,half}, {-half,half} };
    for (int f = 0; f < 4; ++f) {
        const int g = (f + 1) % 4;
        const float A[3] = { x + c[f][0], y0, z + c[f][1] };
        const float B[3] = { x + c[g][0], y0, z + c[g][1] };
        const float C[3] = { x + c[g][0], y1, z + c[g][1] };
        const float D[3] = { x + c[f][0], y1, z + c[f][1] };
        const float n[3] = { c[f][0] + c[g][0], 0, c[f][1] + c[g][1] };
        quad(bk, A, B, C, D, n, 0.0f, (y1 - y0) / 10.0f);
    }
}

// V7.3 STRUCTURAL (slop verdict: "the piers are black sticks — 61:1, no cap,
// no taper, no bearing seat"): footing pad at grade, a tapered shaft (more
// sections the taller the pier), and a HAMMERHEAD cap beam seated under the
// deck, oriented across it via the caller's perp (px,pz). Lamp poles keep
// raw box4 — this is piers only.
void pillar(Buck bk, float x, float z, float yTop, float yGround,
            float px = 1.0f, float pz = 0.0f) {
    const float h = yTop - yGround;
    if (h <= 0.0f) return;
    // Footing: squat pad half-buried at grade (contact, not lawn-spearing).
    box4(bk, x, z, yGround - 1.2f, yGround + 0.9f, kPillarHalf * 2.2f);
    // Tapered shaft up to the cap seat: 1.9x at the base -> 1.15x at the top,
    // in 1-3 sections (each section overlaps the next 0.3 m — no gaps).
    const float capBase = yTop - kDeckDepth - kCapDepth;
    const float shaftTop = std::max(capBase + 0.3f, yGround + 1.0f);
    const int secs = h > 60.0f ? 3 : (h > 20.0f ? 2 : 1);
    for (int i = 0; i < secs; ++i) {
        const float t0 = (float)i / (float)secs, t1 = (float)(i + 1) / (float)secs;
        const float half = kPillarHalf * (1.9f - 0.75f * t0);
        const float y0 = yGround + 0.6f + (shaftTop - yGround - 0.6f) * t0;
        const float y1 = yGround + 0.6f + (shaftTop - yGround - 0.6f) * t1;
        box4(bk, x, z, y0 - 0.3f, y1, half);
    }
    // Hammerhead: the transverse cap beam the deck bears on.
    obox(bk, x, z, capBase, capBase + kCapDepth + 0.25f, px, pz,
         kCapHalfLen, kPillarHalf * 1.15f);
}

} // namespace

// ---------------------------------------------------------------------------
float RoadGraph::laneCenterOffset(const RoadEdge& e, int lane, bool forward) {
    (void)e;   // v2+: clamp `lane` to e.lanesF/lanesB; v1 trusts the caller
    const float off = ((float)lane + 0.5f) * kLaneWidth;
    return forward ? off : -off;
}

// #34a CORRIDOR AUDIT: point-vs-corridor over every edge's centerline. The
// samples are arc-even (~kSampleStep), so testing sample CIRCLES of radius
// (halfCorridor + clear + step/2) covers the swept ribbon without segment
// math — boot-time placement audits don't need better.
bool EchoRoads::corridorHits(float x, float z, float clear) const {
    for (const RoadEdge& e : m_graph.edges) {
        const bool elevated = e.cls == RoadClass::Freeway || e.cls == RoadClass::Ramp;
        const float half = e.width * 0.5f + (elevated ? kShoulderW + kBarrierW : 0.5f);
        const float r = half + clear + kSampleStep * 0.5f;
        const float r2 = r * r;
        for (const RoadSample& s : e.center) {
            const float dx = s.x - x, dz = s.z - z;
            if (dx * dx + dz * dz < r2) return true;
        }
    }
    return false;
}

bool EchoRoads::corridorHitsAABB(float minX, float minZ, float maxX, float maxZ,
                                 float clear) const {
    for (const RoadEdge& e : m_graph.edges) {
        const bool elevated = e.cls == RoadClass::Freeway || e.cls == RoadClass::Ramp;
        const float half = e.width * 0.5f + (elevated ? kShoulderW + kBarrierW : 0.5f);
        const float g = half + clear + kSampleStep * 0.5f;
        for (const RoadSample& s : e.center)
            if (s.x > minX - g && s.x < maxX + g &&
                s.z > minZ - g && s.z < maxZ + g) return true;
    }
    return false;
}

// ================================ build ====================================
bool EchoRoads::build(x3::rhi::IRenderDevice& device, const Heightfield& hf) {
    if (m_built) return true;
    if (!hf.ok()) {
        x3::logWarn("[roads] heightfield not loaded — keeping the legacy freeway");
        return false;
    }
    g_unweldedEquiv = 0;   // V6 weld accounting (logged at the end of build)

    // ======================= PHASE 1 — COLLECT ============================
    // Pending: an edge's centerline + emission recipe, geometry deferred.
    struct Pending {
        RoadClass cls = RoadClass::Avenue;
        float width = kAvenueWidth;
        int lanesF = 1, lanesB = 1;
        bool banked = false, barriers = false, medianPaint = false,
             edgeLines = false, laneDash = false, streetDash = false,
             curbs = false, lamps = false, pillarPair = false,
             pillarSingle = false, closedLoop = false,
             lawExempt = false;   // V6: intentional tight loops (trumpet curl,
                                  // cul-de-sac) are LEGAL curves the zigzag law
                                  // must not execute - see PHASE 1.9.
        float lampEvery = kLampEveryStr, lampH = kLampHStr;
        std::vector<RoadSample> s;
    };
    std::vector<Pending> P;

    auto collectGround = [&](RoadClass cls, float width, int lanes,
                             std::vector<RoadSample>& s) {
        if (s.size() < 2) return;
        for (auto& p : s) p.y = hf.heightAt(p.x, p.z) + kGroundLift;
        for (int pass = 0; pass < 2; ++pass)
            for (size_t i = 1; i + 1 < s.size(); ++i)
                s[i].y = (s[i-1].y + s[i].y + s[i+1].y) / 3.0f;
        for (auto& p : s) p.bank = 0.0f;
        Pending pe;
        pe.cls = cls; pe.width = width; pe.lanesF = lanes; pe.lanesB = lanes;
        pe.streetDash = true; pe.curbs = true; pe.lamps = true;
        pe.lampEvery = kLampEveryStr; pe.lampH = kLampHStr;
        pe.s = std::move(s);
        P.push_back(std::move(pe));
    };

    // ---- 1a. THE RING (V4.1: fixed NE/E arc + RADIALLY probed rim) -------
    // Splice order: the fixed arc ends at the flats' south corner (980,560),
    // bearing ~349 deg from the crown; the loop closes back into the arc's
    // start (-160,720), bearing ~196 deg. Appending rim waypoints in
    // DECREASING bearing (320 -> 140) therefore continues the loop SE -> S ->
    // SW -> W -> NW and hands off cleanly to the closing Catmull leg. A
    // convexify pass drops any waypoint whose insertion turns the path back
    // on itself (concave rim pockets), per the field order.
    std::vector<Wp> ringWp(kRingFixed, kRingFixed + kRingFixedN);
    {
        const float crownRef = hf.heightAt(kCrownX, kCrownZ);   // mesa datum
        const float rimElev  = crownRef * kRimFrac;
        std::vector<Wp> rim;
        int probed = 0, degenerate = 0;
        for (float deg = kRimBearing1; deg >= kRimBearing0 - 0.5f;
             deg -= kRimBearingStep) {                     // decreasing bearing
            ++probed;
            const float th = deg * 3.1415926f / 180.0f;
            const float dx = std::cos(th), dz = std::sin(th);
            float rimR = -1.0f;
            for (float r = kRimMinR; r <= kRimMaxR; r += 8.0f)   // outward march
                if (hf.heightAt(kCrownX + dx * r, kCrownZ + dz * r) >= rimElev)
                    rimR = r;                              // LAST at rim elevation
            if (rimR < kRimMinR) {
                ++degenerate;
                x3::logInfo("[roads] rim bearing " + std::to_string((int)deg) +
                            " deg degenerate (rim < " + std::to_string((int)kRimMinR) +
                            " m) — skipped");
                continue;
            }
            const float wr = rimR - kRimInset;
            rim.push_back({ kCrownX + dx * wr, kCrownZ + dz * wr });
        }
        // Convexify by skipping: walking [arcEnd, rim..., arcStart], drop any
        // rim waypoint whose turn exceeds ~100 deg (backtrack pocket).
        const Wp arcEnd   = kRingFixed[kRingFixedN - 1];   // (980,560)
        const Wp arcStart = kRingFixed[0];                 // (-160,720)
        bool dropped = true;
        while (dropped && !rim.empty()) {
            dropped = false;
            for (size_t i = 0; i < rim.size(); ++i) {
                const Wp& p = (i == 0) ? arcEnd : rim[i - 1];
                const Wp& q = rim[i];
                const Wp& r = (i + 1 == rim.size()) ? arcStart : rim[i + 1];
                float ax = q.x - p.x, az = q.z - p.z;
                float bx = r.x - q.x, bz = r.z - q.z;
                const float la = std::sqrt(ax*ax + az*az), lb = std::sqrt(bx*bx + bz*bz);
                if (la < 1e-3f || lb < 1e-3f) { rim.erase(rim.begin() + i); dropped = true; break; }
                const float dot = (ax*bx + az*bz) / (la * lb);
                if (dot < kRimTurnDrop) {
                    x3::logInfo("[roads] rim waypoint (" + std::to_string((int)q.x) +
                                "," + std::to_string((int)q.z) +
                                ") dropped — backtrack pocket (convexify)");
                    rim.erase(rim.begin() + i);
                    dropped = true;
                    break;
                }
            }
        }
        for (const Wp& w : rim) ringWp.push_back(w);
        x3::logInfo("[roads] V4.1 rim route: " + std::to_string(rim.size()) + "/" +
                    std::to_string(probed) + " radial rim waypoints kept (" +
                    std::to_string(degenerate) + " degenerate; crown datum " +
                    std::to_string((int)crownRef) + " m)");
        // With zero kept rim points (pathological terrain) the fixed arc still
        // closes into a valid — if east-heavy — loop; the log above flags it.
    }
    {
        std::vector<RoadSample> dense;
        dense.reserve(4096);
        const int nWp = (int)ringWp.size();
        for (int i = 0; i < nWp; ++i)
            for (float t = 0.0f; t < 1.0f; t += 0.02f) {
                RoadSample s; catmull(ringWp.data(), nWp, i, t, s.x, s.z);
                dense.push_back(s);
            }
        dense.push_back(dense.front());
        std::vector<RoadSample> ring;
        resample(dense, kSampleStep, ring);
        std::vector<float> floorY(ring.size());
        for (size_t i = 0; i < ring.size(); ++i) {
            floorY[i] = std::max(deckFloor(hf, ring[i].x, ring[i].z), kDeckMinY);
            ring[i].y = floorY[i];
        }
        // V3.1 PROFILE (the "200 m pier ranks" hotfix): a BIDIRECTIONAL
        // grade-limited relaxation toward the LOCAL floor — the rubber band
        // under tension. Forward sweep caps every DESCENT at kDeckMaxGrade
        // (after each summit the deck drops toward local clearance as fast as
        // grade allows, instead of the old raise-only plateau that carried
        // ridge height across the whole shore bowl); backward sweep caps every
        // ASCENT (approaches climb early). No box smoothing — it re-inflated
        // the valleys; profile smoothness comes from the grade bound itself.
        // Both sweeps only ever raise ABOVE floorY, so the never-below-
        // clearance floor holds by construction. The ring is CLOSED: each
        // sweep runs two wraps so the result is start-sample independent.
        {
            const float maxStep = kDeckMaxGrade * kSampleStep;
            const size_t n = ring.size();
            for (size_t k = 1; k < 2 * n; ++k) {           // forward (descent cap)
                const size_t i = k % n, p = (k - 1) % n;
                ring[i].y = std::max(floorY[i],
                                     std::max(ring[i].y, ring[p].y - maxStep));
            }
            for (size_t k = 2 * n; k-- > 1; ) {            // backward (ascent cap)
                const size_t i = (k - 1) % n, q = k % n;
                ring[i].y = std::max(ring[i].y, ring[q].y - maxStep);
            }
        }
        // V5 RIM-EDGE INSET (the "pier colonnade down the cliff face" fix):
        // between rim waypoints the Catmull chord cuts across concave rim
        // pockets and hangs OUTBOARD of the lip — piers then drop the whole
        // cliff to the district below. Per-sample clamp, RIM ZONE ONLY (the
        // NE-slope and flats legs are legitimate viaduct and must not be
        // dragged sideways): if the terrain directly under a sample sits more
        // than kInsetMaxDrop below the deck, migrate the sample INBOARD along
        // its perp-toward-the-crown in kInsetStep hops (max kInsetMaxMove)
        // until ground is back within reach. Then re-derive tangents, reset
        // the elevation to the NEW local floors, and rerun the relaxation —
        // the migrated deck now follows the lip's actual curve with its piers
        // landing on mesa.
        {
            constexpr float kInsetMaxDrop = 25.0f;   // deck-to-ground trigger
            constexpr float kInsetStep    = 6.0f;    // inboard hop
            constexpr float kInsetMaxMove = 120.0f;  // migration cap per sample
            constexpr float kInsetZone    = 620.0f;  // rim zone: within this of the crown
            // V6 §2: MEASURE the needed displacement per sample first, then
            // LOW-PASS it along arc length (+-10 samples = 80 m full window,
            // 2 iters, wrap) so migration ramps in and out — raw independent
            // hops were the "right-angle switchbacks mid-air" Tim outlawed.
            constexpr int kInsetFiltHalf  = 10;
            constexpr int kInsetFiltIters = 2;
            const size_t nR = ring.size();
            std::vector<float> want(nR, 0.0f);
            int migrated = 0;
            for (size_t si = 0; si < nR; ++si) {
                const RoadSample& s = ring[si];
                const float cdx = kCrownX - s.x, cdz = kCrownZ - s.z;
                if (cdx*cdx + cdz*cdz > kInsetZone * kInsetZone) continue;
                float px, pz; rperp(s.tx, s.tz, px, pz);
                if (px * cdx + pz * cdz < 0.0f) { px = -px; pz = -pz; }
                float moved = 0.0f, mx = s.x, mz = s.z;
                while (moved < kInsetMaxMove) {
                    if (s.y - hf.heightAt(mx, mz) <= kInsetMaxDrop) break;
                    mx += px * kInsetStep; mz += pz * kInsetStep;
                    moved += kInsetStep;
                }
                if (moved > 0.0f) { want[si] = moved; ++migrated; }
            }
            if (migrated > 0) {
                std::vector<float> smD(nR);
                for (int it = 0; it < kInsetFiltIters; ++it) {
                    for (size_t si = 0; si < nR; ++si) {
                        float acc = 0; int cnt = 0;
                        for (int k = -kInsetFiltHalf; k <= kInsetFiltHalf; ++k) {
                            const size_t j = (size_t)((((long)si + k) % (long)nR + (long)nR) % (long)nR);
                            acc += want[j]; ++cnt;
                        }
                        smD[si] = acc / (float)cnt;
                    }
                    want = smD;
                }
                for (size_t si = 0; si < nR; ++si) {
                    if (want[si] <= 0.01f) continue;
                    RoadSample& s = ring[si];
                    float px, pz; rperp(s.tx, s.tz, px, pz);
                    if (px * (kCrownX - s.x) + pz * (kCrownZ - s.z) < 0.0f) {
                        px = -px; pz = -pz;
                    }
                    s.x += px * want[si]; s.z += pz * want[si];
                }
            }
            if (migrated > 0) {
                // Tangents from the migrated positions (wrap-aware).
                const size_t n = ring.size();
                for (size_t i = 0; i < n; ++i) {
                    const RoadSample& p = ring[(i + n - 1) % n];
                    const RoadSample& q = ring[(i + 1) % n];
                    float tx = q.x - p.x, tz = q.z - p.z;
                    const float l = std::sqrt(tx*tx + tz*tz);
                    if (l > 1e-5f) { tx /= l; tz /= l; }
                    ring[i].tx = tx; ring[i].tz = tz;
                }
                // Fresh floors at the new positions + full re-relaxation (the
                // migrated deck should DROP to mesa clearance, and only a
                // reset-then-relax lets it come down — the sweeps only raise).
                for (size_t i = 0; i < n; ++i) {
                    floorY[i] = std::max(deckFloor(hf, ring[i].x, ring[i].z), kDeckMinY);
                    ring[i].y = floorY[i];
                }
                const float maxStep = kDeckMaxGrade * kSampleStep;
                for (size_t k = 1; k < 2 * n; ++k) {
                    const size_t i = k % n, p = (k - 1) % n;
                    ring[i].y = std::max(floorY[i],
                                         std::max(ring[i].y, ring[p].y - maxStep));
                }
                for (size_t k = 2 * n; k-- > 1; ) {
                    const size_t i = (k - 1) % n, q = k % n;
                    ring[i].y = std::max(ring[i].y, ring[q].y - maxStep);
                }
            }
            x3::logInfo("[roads] V5 rim inset: " + std::to_string(migrated) +
                        " deck samples migrated inboard (arc-filtered, V6)");
        }
        {   // diagnostics (v3.1 acceptance): tallest pier this profile makes
            float maxPier = 0.0f;
            for (const RoadSample& s : ring)
                maxPier = std::max(maxPier, s.y - hf.heightAt(s.x, s.z));
            x3::logInfo("[roads] deck profile: max pier height " +
                        std::to_string((int)maxPier) + " m (grade cap " +
                        std::to_string((int)(kDeckMaxGrade * 100.0f)) + "%)");
        }
        for (size_t i = 0; i < ring.size(); ++i) {
            const RoadSample& p = ring[i > 0 ? i - 1 : ring.size() - 1];
            const RoadSample& q = ring[(i + 1) % ring.size()];
            const float cross = p.tx * q.tz - p.tz * q.tx;
            const float kappa = cross / (2.0f * kSampleStep);
            ring[i].bank = clampf2(kappa * kBankPerKappa, -kBankMax, kBankMax);
        }
        for (int pass = 0; pass < 2; ++pass)
            for (size_t i = 1; i + 1 < ring.size(); ++i)
                ring[i].bank = (ring[i-1].bank + ring[i].bank + ring[i+1].bank) / 3.0f;

        Pending pe;
        pe.cls = RoadClass::Freeway; pe.width = kFreewayWidth;
        pe.lanesF = 2; pe.lanesB = 2;
        pe.banked = true; pe.barriers = true; pe.medianPaint = true;
        pe.edgeLines = true; pe.laneDash = true; pe.lamps = true;
        pe.pillarPair = true; pe.closedLoop = true;
        pe.lampEvery = kLampEveryFwy; pe.lampH = kLampHFwy;
        pe.s = std::move(ring);
        P.push_back(std::move(pe));
    }
    // P[0] is the ring and P NEVER reorders; but it DOES grow — take a copy
    // of the ring samples for gate math instead of holding a reference into
    // the vector (reallocation safety, the same class of bug v1 fixed once).
    const std::vector<RoadSample> ringC = P[0].s;
    auto nearestRingSample = [&](float x, float z) -> size_t {
        size_t best = 0; float bd = 1e30f;
        for (size_t i = 0; i < ringC.size(); ++i) {
            const float dx = ringC[i].x - x, dz = ringC[i].z - z;
            const float d = dx*dx + dz*dz;
            if (d < bd) { bd = d; best = i; }
        }
        return best;
    };

    // ---- 1b. TRUMPET GATES (v2 math; collect-only) -----------------------
    struct Gate { float gx, gz, ex, ez; };
    const Gate kGates[2] = {
        { 830.0f, 1150.0f,  935.0f, 1235.0f },   // Recife 2050 SW gate -> pad
        { 700.0f,  452.0f,  700.0f,  368.0f },   // Urban District N gate -> pad
    };
    for (int gi = 0; gi < 2; ++gi) {
        Gate g = kGates[gi];   // mutable: the ground node may be nudged to land
        // NUDGE THE GROUND NODE ONTO DRY LAND. Some gates seed into a harbor
        // inlet where the terrain is BELOW sea level (Urban gate: y ~ -10) — the
        // ramps would then descend into the sea instead of onto a navigable
        // street. Walk the ground node up the terrain gradient (steepest ascent)
        // until it clears the waterline, capped. Gates already on land (Recife,
        // y > 0) don't move. The pad target (ex,ez) is unchanged; the gate
        // avenue re-aims from the nudged node.
        {
            constexpr float kGateLandSafe = 2.0f;
            constexpr float kGateNudge    = 14.0f;
            constexpr int   kGateNudgeMax = 14;
            for (int n2 = 0; n2 < kGateNudgeMax &&
                             hf.heightAt(g.gx, g.gz) < kGateLandSafe; ++n2) {
                const float hx = hf.heightAt(g.gx + 8.0f, g.gz) - hf.heightAt(g.gx - 8.0f, g.gz);
                const float hz = hf.heightAt(g.gx, g.gz + 8.0f) - hf.heightAt(g.gx, g.gz - 8.0f);
                const float gl = std::sqrt(hx * hx + hz * hz);
                if (gl < 1e-4f) break;                    // flat spot — give up
                g.gx += (hx / gl) * kGateNudge;
                g.gz += (hz / gl) * kGateNudge;
            }
        }
        const size_t at = nearestRingSample(g.gx, g.gz);
        const RoadSample deck = ringC[at];
        const float gy = hf.heightAt(g.gx, g.gz) + kGroundLift;
        // Deck<->gate chord: the tangent lead is capped to a small multiple of
        // THIS so the hermite hugs the deck->gate corridor. The old lead =
        // (deck.y-gy)/grade was UNBOUNDED (hundreds..thousands of m on a mesa
        // deck) — the hermite then ballooned into a giant swoop that read as a
        // ramp "curving off into midair" (Tim's tell). Length for grade comes
        // from the profile smoothstep over arc length, not from oversized
        // tangents.
        const float gChord = std::sqrt((g.gx - deck.x) * (g.gx - deck.x) +
                                       (g.gz - deck.z) * (g.gz - deck.z));
        {   // directional OFF-ramp: deck -> gate (down to the ground pad node)
            std::vector<RoadSample> d2;
            const float grNeed = (deck.y - gy) / kRampMaxGrade;   // len for <=7%
            // Cap tangents to a small multiple of the chord (no balloon) but with
            // an absolute floor so a SHORT chord under a HIGH deck (nudged Urban
            // gate: 11 m chord, 58 m drop) still curves down instead of dropping
            // as a vertical wall of asphalt.
            const float lead   = clampf2(grNeed, gChord * 0.8f,
                                         std::max(gChord * 1.4f, 55.0f));
            for (float t = 0.0f; t <= 1.0f; t += 0.02f) {
                RoadSample s;
                hermite(deck.x, deck.z, deck.tx * lead, deck.tz * lead,
                        g.gx, g.gz, deck.tx * lead * 0.3f, deck.tz * lead * 0.3f,
                        t, s.x, s.z);
                d2.push_back(s);
            }
            std::vector<RoadSample> ramp;
            resample(d2, kRampStep, ramp);
            for (size_t i = 0; i < ramp.size(); ++i) {
                const float t = (float)i / (float)(ramp.size() - 1);
                const float e = t * t * (3.0f - 2.0f * t);
                ramp[i].y = deck.y + (gy - deck.y) * e;
                ramp[i].bank = 0.0f;
            }
            // Pin the terminals: top exactly on the deck (merge onto the ring),
            // foot exactly on the gate ground node (so Pass-A endpoint capture
            // ties it into the gate avenue + on-ramp = one interchange node).
            if (ramp.size() >= 2) {
                ramp.front().x = deck.x; ramp.front().z = deck.z; ramp.front().y = deck.y;
                ramp.back().x  = g.gx;   ramp.back().z  = g.gz;   ramp.back().y  = gy;
            }
            Pending pe;
            pe.cls = RoadClass::Ramp; pe.width = kRampWidth;
            pe.lanesF = 1; pe.lanesB = 0;
            pe.barriers = true; pe.pillarSingle = true;
            pe.s = std::move(ramp);
            P.push_back(std::move(pe));
        }
        {   // V7 CALIFORNIA ON-RAMP (Tim: "California style swooping ramps,
            // with arc"): the tight 250-deg trumpet curl (r=30) is retired.
            // The on-ramp is now ONE long sweeping arc — foot at the gate
            // ground node, head merging TANGENTIALLY onto a deck sample well
            // DOWNSTREAM of the off-ramp's, so the pair reads as a wide
            // parclo: traffic leaves the deck, sweeps down, and rejoins it
            // 100-160m later in the direction of travel. Effective radius
            // 60-90m => ~0.7-1.0 deg/m: PASSES the ramp law WITHOUT an
            // exemption (the law is the gate now, as it should be).
            const size_t nR = ringC.size();
            // V7.1: PER-GATE bow + merge, chosen against the LAW'S OWN
            // verdicts (probe experiments 2026-07-30 — no local metric
            // predicted the post-smooth result, because the law measures
            // central-difference tangents after a pinned-end smooth over the
            // node-captured edge, not the authored hermite):
            //   gate 0: the V7 arc (35 deg bow, 140 m merge) — law 0.02 deg/m.
            //   gate 1: the V7 arc hooked post-smooth (11.6 then 8.3 deg/m,
            //           DROPPED — the "one CA on-ramp" of the punchlist); a
            //           gentler 20 deg bow to the 200 m merge PASSES.
            // The zigzag law stays the loud runtime gate: if a terrain bake
            // ever re-breaks a ramp it drops with a WARN, not a zigzag.
            static constexpr float kOnRampRot[]   = { 0.61f, 0.35f };
            static constexpr float kOnRampMerge[] = { 140.0f, 200.0f };
            const size_t gci = gi < 2 ? (size_t)gi : 1;
            const float rot    = kOnRampRot[gci];
            const float mergeM = kOnRampMerge[gci];
            const size_t at2 = (at + (size_t)(mergeM / kSampleStep)) % nR;
            const RoadSample deck2 = ringC[at2];
            const float mChord2 = std::sqrt((deck2.x - g.gx)*(deck2.x - g.gx) +
                                            (deck2.z - g.gz)*(deck2.z - g.gz));
            // Long tangents bow the hermite outward into the sweep; the foot
            // tangent aims `rot` off the direct chord so the curve arcs wide
            // instead of cutting straight.
            const float cdx = (deck2.x - g.gx) / mChord2, cdz = (deck2.z - g.gz) / mChord2;
            const float fdx = cdx * std::cos(rot) - cdz * std::sin(rot);
            const float fdz = cdx * std::sin(rot) + cdz * std::cos(rot);
            const float lead2 = std::max(mChord2 * 0.9f, 90.0f);
            std::vector<RoadSample> d2;
            for (float t = 0.0f; t <= 1.0f; t += 0.015f) {
                RoadSample sr;
                hermite(g.gx, g.gz, fdx * lead2, fdz * lead2,
                        deck2.x, deck2.z, deck2.tx * lead2 * 0.8f, deck2.tz * lead2 * 0.8f,
                        t, sr.x, sr.z);
                d2.push_back(sr);
            }
            std::vector<RoadSample> swp;
            resample(d2, kRampStep, swp);
            for (size_t i = 0; i < swp.size(); ++i) {
                const float t = (float)i / (float)(swp.size() - 1);
                const float e2 = t * t * (3.0f - 2.0f * t);
                swp[i].y = gy + (deck2.y - gy) * e2;
            }
            if (swp.size() >= 2) {
                swp.front().x = g.gx;    swp.front().z = g.gz;    swp.front().y = gy;
                swp.back().x  = deck2.x; swp.back().z  = deck2.z; swp.back().y  = deck2.y;
            }
            x3::logInfo("[roads] on-ramp gate " + std::to_string(gi) + ": bow " +
                        std::to_string(rot) + " rad, merge " +
                        std::to_string((int)mergeM) + " m (V7.1 per-gate arc)");
            Pending pe;
            pe.cls = RoadClass::Ramp; pe.width = kRampWidth;
            pe.lanesF = 1; pe.lanesB = 0;
            pe.banked = true;   // the law's rebank paints continuous superelevation
            pe.barriers = true; pe.pillarSingle = true;
            pe.s = std::move(swp);
            P.push_back(std::move(pe));
        }
        {   // gate avenue into the pad
            std::vector<RoadSample> d2;
            for (float t = 0.0f; t <= 1.0f; t += 0.05f) {
                RoadSample s;
                s.x = g.gx + (g.ex - g.gx) * t; s.z = g.gz + (g.ez - g.gz) * t;
                d2.push_back(s);
            }
            std::vector<RoadSample> av; resample(d2, kSampleStep, av);
            collectGround(RoadClass::Avenue, kAvenueWidth, 1, av);
        }
        x3::logInfo("[roads] gate " + std::to_string(gi) + " connected: deck(" +
                    std::to_string((int)deck.x) + "," + std::to_string((int)deck.z) +
                    ",y" + std::to_string((int)deck.y) + ") <-ramps-> ground(" +
                    std::to_string((int)g.gx) + "," + std::to_string((int)g.gz) +
                    ",y" + std::to_string((int)gy) + "); chord " +
                    std::to_string((int)gChord) + " m");
    }

    // ---- 1c. SPUR AVENUES (v2 verbatim) ----------------------------------
    {
        std::vector<RoadSample> d2;
        for (float t = 0.0f; t <= 1.0f; t += 0.03f) {
            RoadSample s;
            hermite(1075.0f, 905.0f, 180.0f, 30.0f, 1310.0f, 1000.0f,
                    160.0f, 20.0f, t, s.x, s.z);
            d2.push_back(s);
        }
        std::vector<RoadSample> av; resample(d2, kSampleStep, av);
        collectGround(RoadClass::Avenue, kAvenueWidth, 1, av);
    }
    {
        std::vector<RoadSample> d2;
        for (float t = 0.0f; t <= 1.0f; t += 0.05f) {
            RoadSample s;
            hermite(-100.0f, 690.0f, 90.0f, 40.0f, -24.0f, 752.0f, 60.0f, 30.0f,
                    t, s.x, s.z);
            d2.push_back(s);
        }
        std::vector<RoadSample> av; resample(d2, kSampleStep, av);
        collectGround(RoadClass::Avenue, kAvenueWidth, 1, av);
    }

    // ---- 1e. MINE SPUR (V5 — Tim at the west shoulder: "No COHESIVE
    // ROADS"): rim highway -> gold-mine truck lot (-556,814). Three pieces,
    // all riding the existing machinery:
    //   (a) a MINI-RAMP off the nearest rim-deck sample (the rim deck rides
    //       ~11 m of clearance even on mesa, so a short graded descent is the
    //       honest tee — the ramp FOOT then endpoint-captures into (b) and
    //       the junction system builds the tee patch);
    //   (b) a terrain-conformed AVENUE foot -> lot (mesa top, gentle — no
    //       viaduct, collectGround handles it);
    //   (c) a CUL-DE-SAC loop at the lot (300-degree circle, both endpoints
    //       capture into (b)'s end -> one junction patch = the turnaround).
    {
        constexpr float kLotX = -556.0f, kLotZ = 814.0f;
        constexpr float kLoopCulR = 14.0f;
        const size_t at = nearestRingSample(kLotX, kLotZ);
        const RoadSample deck = ringC[at];
        float dirX = kLotX - deck.x, dirZ = kLotZ - deck.z;
        const float dLen = std::sqrt(dirX*dirX + dirZ*dirZ);
        if (dLen > 40.0f) {          // degenerate only if the rim IS the lot
            dirX /= dLen; dirZ /= dLen;
            // (a) mini-ramp: deck -> foot (~35% of the way, min 90 m lead).
            const float footD = std::max(90.0f, dLen * 0.35f);
            const float footX = deck.x + dirX * footD, footZ = deck.z + dirZ * footD;
            const float footY = hf.heightAt(footX, footZ) + kGroundLift;
            {
                std::vector<RoadSample> d2;
                const float lead = std::max(90.0f, (deck.y - footY) / kRampMaxGrade);
                for (float t = 0.0f; t <= 1.0f; t += 0.02f) {
                    RoadSample s;
                    hermite(deck.x, deck.z, deck.tx * lead, deck.tz * lead,
                            footX, footZ, dirX * lead * 0.4f, dirZ * lead * 0.4f,
                            t, s.x, s.z);
                    d2.push_back(s);
                }
                std::vector<RoadSample> ramp; resample(d2, kRampStep, ramp);
                for (size_t i = 0; i < ramp.size(); ++i) {
                    const float t = (float)i / (float)(ramp.size() - 1);
                    const float e = t * t * (3.0f - 2.0f * t);
                    ramp[i].y = deck.y + (footY - deck.y) * e;
                    ramp[i].bank = 0.0f;
                }
                Pending pe;
                pe.cls = RoadClass::Ramp; pe.width = kRampWidth;
                pe.lanesF = 1; pe.lanesB = 1;   // two-way spur ramp
                pe.barriers = true; pe.pillarSingle = true;
                pe.s = std::move(ramp);
                P.push_back(std::move(pe));
            }
            // (b) avenue foot -> lot, terrain-conformed.
            {
                std::vector<RoadSample> d2;
                for (float t = 0.0f; t <= 1.0f; t += 0.04f) {
                    RoadSample s;
                    s.x = footX + (kLotX - footX) * t;
                    s.z = footZ + (kLotZ - footZ) * t;
                    d2.push_back(s);
                }
                std::vector<RoadSample> av; resample(d2, kSampleStep, av);
                collectGround(RoadClass::Avenue, kAvenueWidth, 1, av);
            }
            // (c) cul-de-sac: 300-degree loop past the lot; both endpoints sit
            // at the lot so they capture into (b) -> the turnaround patch.
            {
                const float cx = kLotX + dirX * kLoopCulR,
                            cz = kLotZ + dirZ * kLoopCulR;
                const float a0 = std::atan2(kLotZ - cz, kLotX - cx);
                const float sweep = 300.0f * 3.1415926f / 180.0f;
                std::vector<RoadSample> d2;
                for (float a = 0.0f; a <= sweep; a += 0.08f) {
                    RoadSample s;
                    s.x = cx + std::cos(a0 + a) * kLoopCulR;
                    s.z = cz + std::sin(a0 + a) * kLoopCulR;
                    d2.push_back(s);
                }
                std::vector<RoadSample> cul; resample(d2, kSampleStep, cul);
                collectGround(RoadClass::HarborStreet, kStreetWidth, 1, cul);
                if (!P.empty()) P.back().lawExempt = true;   // 300-deg turnaround is intentional
            }
            x3::logInfo("[roads] V5 mine spur: rim deck (" +
                        std::to_string((int)deck.x) + "," + std::to_string((int)deck.z) +
                        ") -> lot (" + std::to_string((int)kLotX) + "," +
                        std::to_string((int)kLotZ) + "), " + std::to_string((int)dLen) +
                        " m with tee ramp + cul-de-sac");
        } else {
            x3::logWarn("[roads] V5 mine spur skipped — rim deck already at the lot (" +
                        std::to_string((int)dLen) + " m)");
        }
    }

    // ---- 1d. HARBOR BOULEVARD + FANNED BLOCKS (v2 probe; collect-only) ---
    constexpr float kBlvdWidth    = 12.5f;
    constexpr float kBlvdSetback  = 42.0f;
    constexpr float kBlockGap     = 14.0f;
    constexpr float kCell         = 42.0f;
    constexpr float kCrossPitch   = 52.0f;
    constexpr float kLandSafe     = 2.5f;
    constexpr float kNudgeStep    = 12.0f;
    constexpr int   kNudgeMax     = 5;
    static const Wp kShoreSeed[] = {
        { -140.0f, 470.0f }, { -40.0f, 455.0f }, {  90.0f, 450.0f },
        {  230.0f, 452.0f }, { 370.0f, 440.0f }, {  510.0f, 408.0f },
        {  650.0f, 356.0f }, { 780.0f, 300.0f }, {  860.0f, 268.0f },
    };
    const int nSeed = (int)(sizeof(kShoreSeed) / sizeof(kShoreSeed[0]));
    auto waterlineFrom = [&](float sx, float sz, float dx, float dz,
                             float& wx, float& wz) -> bool {
        const bool startWet = hf.heightAt(sx, sz) < kWaterMinLand;
        float lo = 0.0f, hi = -1.0f;
        for (float m = 8.0f; m <= 400.0f; m += 8.0f) {
            const bool wet = hf.heightAt(sx + dx*m, sz + dz*m) < kWaterMinLand;
            if (wet != startWet) { hi = m; break; }
            lo = m;
        }
        if (hi < 0.0f) return false;
        for (int it = 0; it < 8; ++it) {
            const float mid = (lo + hi) * 0.5f;
            const bool wet = hf.heightAt(sx + dx*mid, sz + dz*mid) < kWaterMinLand;
            if (wet != startWet) hi = mid; else lo = mid;
        }
        wx = sx + dx * hi; wz = sz + dz * hi;
        return true;
    };
    std::vector<RoadSample> blvdPts;
    for (int i = 0; i < nSeed; ++i) {
        const Wp& s0 = kShoreSeed[i > 0 ? i - 1 : 0];
        const Wp& s1 = kShoreSeed[i + 1 < nSeed ? i + 1 : nSeed - 1];
        float tx = s1.x - s0.x, tz = s1.z - s0.z;
        const float tl = std::sqrt(tx*tx + tz*tz);
        if (tl > 1e-4f) { tx /= tl; tz /= tl; }
        float px, pz; rperp(tx, tz, px, pz);
        const float hR = hf.heightAt(kShoreSeed[i].x + px*60.0f, kShoreSeed[i].z + pz*60.0f);
        const float hL = hf.heightAt(kShoreSeed[i].x - px*60.0f, kShoreSeed[i].z - pz*60.0f);
        const float ix = (hR >= hL) ? px : -px, iz = (hR >= hL) ? pz : -pz;
        float wx, wz;
        bool ok;
        if (hf.heightAt(kShoreSeed[i].x, kShoreSeed[i].z) < kWaterMinLand)
            ok = waterlineFrom(kShoreSeed[i].x, kShoreSeed[i].z,  ix,  iz, wx, wz);
        else
            ok = waterlineFrom(kShoreSeed[i].x, kShoreSeed[i].z, -ix, -iz, wx, wz);
        if (!ok) { x3::logWarn("[roads] shore seed " + std::to_string(i) +
                               " found no waterline within 400m — skipped"); continue; }
        RoadSample p; p.x = wx + ix * kBlvdSetback; p.z = wz + iz * kBlvdSetback;
        for (int n2 = 0; n2 < kNudgeMax &&
             hf.heightAt(p.x, p.z) < kLandSafe; ++n2) {
            p.x += ix * kNudgeStep; p.z += iz * kNudgeStep;
            x3::logInfo("[roads] boulevard point " + std::to_string(i) +
                        " nudged inland (cove)");
        }
        blvdPts.push_back(p);
    }
    if (blvdPts.size() >= 3) {
        for (int pass = 0; pass < 2; ++pass)
            for (size_t i = 1; i + 1 < blvdPts.size(); ++i) {
                blvdPts[i].x = (blvdPts[i-1].x + blvdPts[i].x + blvdPts[i+1].x) / 3.0f;
                blvdPts[i].z = (blvdPts[i-1].z + blvdPts[i].z + blvdPts[i+1].z) / 3.0f;
            }
        std::vector<RoadSample> blvd;
        resample(blvdPts, kSampleStep, blvd);
        const std::vector<RoadSample> blvdC = blvd;
        {
            std::vector<RoadSample> tmp = blvd;
            collectGround(RoadClass::Avenue, kBlvdWidth, 2, tmp);
        }
        {   // east tie-in to the Urban gate node.
            // V6 §3 — THE WOODLANDS ZIGZAG, root-caused: this edge was a
            // hermite feeding a 120-scaled boulevard-end tangent against a
            // hard (0,90) northbound gate tangent; whenever the probed
            // boulevard ended pointing east/south-east, the mismatched
            // control magnitudes hooked the curve into Z-reversals on the
            // woodlands descent (Tim's lamp-trail capture). Re-authored as a
            // QUADRATIC BEZIER (end, end + tangent*90, gate): the curve lives
            // inside its control triangle — monotone progression toward the
            // gate, reversal-impossible by construction.
            const RoadSample& e = blvdC.back();
            const float c1x = e.x + e.tx * 90.0f, c1z = e.z + e.tz * 90.0f;
            std::vector<RoadSample> d2;
            for (float t = 0.0f; t <= 1.0f; t += 0.04f) {
                const float u = 1.0f - t;
                RoadSample s;
                s.x = u*u*e.x + 2.0f*u*t*c1x + t*t*700.0f;
                s.z = u*u*e.z + 2.0f*u*t*c1z + t*t*452.0f;
                d2.push_back(s);
            }
            std::vector<RoadSample> av; resample(d2, kSampleStep, av);
            collectGround(RoadClass::Avenue, kAvenueWidth, 1, av);
        }
        struct Block { float att; int nLong, nCross; };
        static const Block kBlocks[] = {
            { 0.10f, 3, 4 }, { 0.28f, 2, 3 }, { 0.46f, 3, 4 },
            { 0.64f, 2, 3 }, { 0.82f, 3, 4 },
        };
        for (int b = 0; b < (int)(sizeof(kBlocks)/sizeof(kBlocks[0])); ++b) {
            const Block& blk = kBlocks[b];
            const size_t ai = std::min(blvdC.size() - 1,
                                       (size_t)(blk.att * (float)blvdC.size()));
            const RoadSample& at = blvdC[ai];
            float px, pz; rperp(at.tx, at.tz, px, pz);
            const float hR = hf.heightAt(at.x + px*60.0f, at.z + pz*60.0f);
            const float hL = hf.heightAt(at.x - px*60.0f, at.z - pz*60.0f);
            const float ix = (hR >= hL) ? px : -px, iz = (hR >= hL) ? pz : -pz;
            float ax = at.x + ix * kBlockGap, az = at.z + iz * kBlockGap;
            const float longLen = (float)(blk.nCross - 1) * kCrossPitch;
            const float depth   = (float)(blk.nLong - 1) * kCell;
            int n2 = 0;
            for (; n2 <= kNudgeMax; ++n2) {
                const float c1x = ax + ix*depth + at.tx*longLen*0.5f;
                const float c1z = az + iz*depth + at.tz*longLen*0.5f;
                const float c2x = ax + ix*depth - at.tx*longLen*0.5f;
                const float c2z = az + iz*depth - at.tz*longLen*0.5f;
                if (hf.heightAt(ax, az) >= kLandSafe &&
                    hf.heightAt(c1x, c1z) >= kLandSafe &&
                    hf.heightAt(c2x, c2z) >= kLandSafe) break;
                ax += ix * kNudgeStep; az += iz * kNudgeStep;
            }
            if (n2 > kNudgeMax) {
                x3::logWarn("[roads] harbor block " + std::to_string(b) +
                            " could not find dry land — skipped");
                continue;
            }
            if (n2 > 0)
                x3::logInfo("[roads] harbor block " + std::to_string(b) +
                            " nudged " + std::to_string((int)(n2 * kNudgeStep)) +
                            " m inland");
            auto emitStreet = [&](float lx0, float ld0, float lx1, float ld1) {
                std::vector<RoadSample> d2;
                for (float t = 0.0f; t <= 1.0f; t += 0.04f) {
                    const float lx = lx0 + (lx1 - lx0) * t;
                    const float ld = ld0 + (ld1 - ld0) * t;
                    RoadSample s;
                    s.x = ax + at.tx * lx + ix * ld;
                    s.z = az + at.tz * lx + iz * ld;
                    d2.push_back(s);
                }
                std::vector<RoadSample> st; resample(d2, kSampleStep, st);
                size_t keep = st.size();
                for (size_t i2 = 0; i2 < st.size(); ++i2)
                    if (hf.heightAt(st[i2].x, st[i2].z) < kWaterMinLand) { keep = i2; break; }
                if (keep < 2) return;
                st.resize(keep);
                collectGround(RoadClass::HarborStreet, kStreetWidth, 1, st);
            };
            for (int L = 0; L < blk.nLong; ++L)
                emitStreet(-longLen*0.5f, (float)L * kCell,
                            longLen*0.5f, (float)L * kCell);
            for (int Cx = 0; Cx < blk.nCross; ++Cx) {
                const float lx = -longLen*0.5f + (float)Cx * kCrossPitch;
                emitStreet(lx, depth + 6.0f, lx, -(kBlockGap + kBlvdWidth*0.5f + 24.0f));
            }
        }
    } else {
        x3::logWarn("[roads] harbor boulevard probe yielded <3 land points — "
                    "harbor district skipped (check shore seeds vs terrain)");
    }

    // ==================== PHASE 1.9 — THE ZIGZAG LAW ======================
    // Tim (2026-07-27, after two captures — right-angle switchbacks on the
    // rim descent, hard Z-reversals on a woodlands avenue): "No roads can
    // ever be zig zag." The universal FINAL geometry pass: runs after every
    // authoring/probe/inset/relaxation pass and before junctions read the
    // centerlines. Smooth every edge, then VALIDATE curvature per class; an
    // edge that cannot meet its limit after smoothing + one decimate-refit is
    // DROPPED with a loud log — the owner's ruling: a missing road beats a
    // zigzag. Intentional loops (trumpet curl, cul-de-sac) carry lawExempt.
    {
        constexpr float kMaxTurnStreet = 2.5f;   // deg per meter of centerline
        constexpr float kMaxTurnRamp   = 1.2f;
        constexpr float kMaxTurnFwy    = 0.8f;   // ring arc fillets run r~90m
        auto classLimit = [&](RoadClass c) {
            return c == RoadClass::Freeway ? kMaxTurnFwy
                 : c == RoadClass::Ramp    ? kMaxTurnRamp : kMaxTurnStreet;
        };
        auto stepOf = [&](const Pending& pe) {
            return pe.cls == RoadClass::Ramp ? kRampStep : kSampleStep;
        };
        auto retangent = [&](Pending& pe) {
            auto& sv = pe.s; const size_t n = sv.size();
            for (size_t i = 0; i < n; ++i) {
                const size_t ip = pe.closedLoop ? (i + n - 1) % n : (i > 0 ? i - 1 : 0);
                const size_t iq = pe.closedLoop ? (i + 1) % n : (i + 1 < n ? i + 1 : n - 1);
                float dx = sv[iq].x - sv[ip].x, dz = sv[iq].z - sv[ip].z;
                const float l = std::sqrt(dx*dx + dz*dz);
                if (l > 1e-5f) { sv[i].tx = dx / l; sv[i].tz = dz / l; }
            }
        };
        // Worst heading change per meter along the edge (degrees).
        auto worstTurn = [&](const Pending& pe) {
            const auto& sv = pe.s; const size_t n = sv.size();
            if (n < 3) return 0.0f;
            const float step = stepOf(pe);
            float worst = 0.0f;
            const size_t last = pe.closedLoop ? n : n - 1;
            for (size_t i = 0; i < last; ++i) {
                const size_t j = (i + 1) % n;
                const float dot = clampf2(sv[i].tx*sv[j].tx + sv[i].tz*sv[j].tz, -1.0f, 1.0f);
                worst = std::max(worst, std::acos(dot) * 57.29578f / step);
            }
            return worst;
        };
        // Lateral smoothing: moving average over a ~40m arc window, 2 passes.
        // Open edges PIN their first/last 2 samples (junction handoffs must not
        // move); closed loops wrap. Ground classes re-seat on the terrain after
        // (smoothing must never float a street); decks keep their graded y,
        // smoothed by the same window (grade caps already held pre-law).
        auto smoothOnce = [&](Pending& pe, int halfWin) {
            auto& sv = pe.s; const size_t n = sv.size();
            if (n < 5) return;
            std::vector<RoadSample> src = sv;
            for (size_t i = 0; i < n; ++i) {
                if (!pe.closedLoop && (i < 2 || i + 2 >= n)) continue;
                float ax = 0, ay = 0, az = 0; int c = 0;
                for (int k = -halfWin; k <= halfWin; ++k) {
                    long j = (long)i + k;
                    if (pe.closedLoop) j = ((j % (long)n) + (long)n) % (long)n;
                    else               j = j < 0 ? 0 : (j >= (long)n ? (long)n - 1 : j);
                    ax += src[(size_t)j].x; ay += src[(size_t)j].y; az += src[(size_t)j].z; ++c;
                }
                sv[i].x = ax / c; sv[i].y = ay / c; sv[i].z = az / c;
            }
        };
        auto reseat = [&](Pending& pe) {
            if (pe.cls == RoadClass::Avenue || pe.cls == RoadClass::HarborStreet)
                for (auto& sp : pe.s) sp.y = hf.heightAt(sp.x, sp.z) + kGroundLift;
        };
        auto rebank = [&](Pending& pe) {
            if (!pe.banked) return;
            auto& sv = pe.s; const size_t n = sv.size();
            if (n < 3) return;
            const float step = stepOf(pe);
            for (size_t i = 0; i < n; ++i) {
                const RoadSample& pp = sv[pe.closedLoop ? (i + n - 1) % n : (i > 0 ? i - 1 : 0)];
                const RoadSample& qq = sv[pe.closedLoop ? (i + 1) % n : (i + 1 < n ? i + 1 : n - 1)];
                const float cross = pp.tx * qq.tz - pp.tz * qq.tx;
                sv[i].bank = clampf2((cross / (2.0f * step)) * kBankPerKappa, -kBankMax, kBankMax);
            }
            for (int pass = 0; pass < 2; ++pass)
                for (size_t i = 1; i + 1 < n; ++i)
                    sv[i].bank = (sv[i-1].bank + sv[i].bank + sv[i+1].bank) / 3.0f;
        };
        float lawWorstAll = 0.0f; int lawDropped = 0;
        for (size_t ei = 0; ei < P.size(); ++ei) {
            Pending& pe = P[ei];
            if (pe.lawExempt || pe.s.size() < 5) continue;
            const int halfWin = (int)(20.0f / stepOf(pe)) > 2 ? (int)(20.0f / stepOf(pe)) : 2;
            for (int it = 0; it < 2; ++it) smoothOnce(pe, halfWin);
            reseat(pe); retangent(pe);
            float w = worstTurn(pe);
            // ESCALATION (first field run: the law DROPPED THE RING — a ~12m
            // kink at a rim-splice corner that a 40m window cannot flatten).
            // Before execution, the cure escalates: doubling windows, re-seat,
            // re-measure. Only geometry that resists a 320m window dies.
            for (int esc = 1; esc <= 3 && w > classLimit(pe.cls); ++esc) {
                for (int it = 0; it < 2; ++it) smoothOnce(pe, halfWin << esc);
                reseat(pe); retangent(pe);
                w = worstTurn(pe);
            }
            if (w > classLimit(pe.cls)) {
                // Decimate-refit: keep every 4th sample as a waypoint, rebuild
                // the polyline, resample at the class step, re-smooth once —
                // kills sample-scale oscillation an averaging window cannot.
                std::vector<RoadSample> wp2;
                for (size_t i = 0; i < pe.s.size(); i += 4) wp2.push_back(pe.s[i]);
                if (!pe.closedLoop) wp2.push_back(pe.s.back());
                std::vector<RoadSample> dense2;
                for (size_t i = 0; i + 1 < wp2.size(); ++i)
                    for (int k = 0; k < 8; ++k) {
                        const float t = (float)k / 8.0f;
                        RoadSample ns;
                        ns.x = wp2[i].x + (wp2[i+1].x - wp2[i].x) * t;
                        ns.z = wp2[i].z + (wp2[i+1].z - wp2[i].z) * t;
                        ns.y = wp2[i].y + (wp2[i+1].y - wp2[i].y) * t;
                        dense2.push_back(ns);
                    }
                std::vector<RoadSample> refit; resample(dense2, stepOf(pe), refit);
                if (refit.size() >= 5) {
                    pe.s = std::move(refit);
                    for (int it = 0; it < 2; ++it) smoothOnce(pe, halfWin);
                    reseat(pe); retangent(pe);
                    w = worstTurn(pe);
                }
            }
            if (w > classLimit(pe.cls) * 1.15f) {   // 15% grace over the limit
                x3::logWarn("[roads] zigzag law: DROPPED edge " + std::to_string(ei) +
                            " (class " + std::to_string((int)pe.cls) + ", worst " +
                            std::to_string(w) + " deg/m > limit " +
                            std::to_string(classLimit(pe.cls)) +
                            ") — a missing road beats a zigzag");
                pe.s.clear(); ++lawDropped;
                continue;
            }
            rebank(pe);
            // V6 order: per-edge worst curvature at boot (survivors).
            x3::logInfo("[roads] curvature: worst " + std::to_string(w) +
                        " deg/m on class " + std::to_string((int)pe.cls) +
                        " edge " + std::to_string(ei));
            lawWorstAll = lawWorstAll > w ? lawWorstAll : w;
        }
        P.erase(std::remove_if(P.begin(), P.end(),
                    [](const Pending& pe){ return pe.s.size() < 2; }), P.end());
        x3::logInfo(std::string("[roads] zigzag law: ") +
                    (lawDropped == 0 ? std::string("PASS")
                                     : ("FAIL (" + std::to_string(lawDropped) + " edges dropped)")) +
                    " — worst surviving curvature " + std::to_string(lawWorstAll) + " deg/m");
    }

    // ======================= PHASE 2 — JUNCTIONS ==========================
    auto isGround = [&](size_t i) {
        return P[i].cls == RoadClass::Avenue || P[i].cls == RoadClass::HarborStreet;
    };
    struct Junc { float x = 0, z = 0, y = 0, r = 0; std::vector<size_t> es; };
    std::vector<Junc> J;
    auto addCandidate = [&](float x, float z, size_t ea, size_t eb) {
        for (Junc& j : J) {
            const float dx = j.x - x, dz = j.z - z;
            if (dx*dx + dz*dz < kJuncCluster * kJuncCluster) {
                j.x = (j.x + x) * 0.5f; j.z = (j.z + z) * 0.5f;
                if (std::find(j.es.begin(), j.es.end(), ea) == j.es.end()) j.es.push_back(ea);
                if (std::find(j.es.begin(), j.es.end(), eb) == j.es.end()) j.es.push_back(eb);
                return;
            }
        }
        Junc j; j.x = x; j.z = z; j.es = { ea, eb };
        J.push_back(std::move(j));
    };
    // Pass A: endpoint captures + straight extension into the target corridor
    // (the "roads near the end of the bridge do not even join" fix).
    for (size_t e = 0; e < P.size(); ++e) {
        if (P[e].cls == RoadClass::Freeway) continue;   // grade-separated
        for (int endSel = 0; endSel < 2; ++endSel) {
            if (P[e].closedLoop) break;
            // NOTE: end position is COPIED — extension mutates P[e].s.
            const RoadSample end = endSel ? P[e].s.back() : P[e].s.front();
            size_t bestO = SIZE_MAX, bestJ = 0; float bestD = 1e30f;
            for (size_t o = 0; o < P.size(); ++o) {
                if (o == e || !isGround(o)) continue;
                for (size_t j = 0; j < P[o].s.size(); ++j) {
                    const float dx = P[o].s[j].x - end.x, dz = P[o].s[j].z - end.z;
                    const float d = dx*dx + dz*dz;
                    if (d < bestD) { bestD = d; bestO = o; bestJ = j; }
                }
            }
            if (bestO == SIZE_MAX) continue;
            const float d = std::sqrt(bestD);
            const float cap = kJuncCapture * 0.5f * (P[e].width + P[bestO].width);
            if (d > cap) continue;
            const RoadSample tgt = P[bestO].s[bestJ];
            if (d > 2.0f) {   // EXTEND the stop-short end straight to the target
                float dx = tgt.x - end.x, dz = tgt.z - end.z;
                const float len = std::sqrt(dx*dx + dz*dz);
                dx /= len; dz /= len;
                const int steps = (int)(len / kSampleStep) + 1;
                for (int k2 = 1; k2 <= steps; ++k2) {
                    const float f = std::min(1.0f, (float)k2 * kSampleStep / len);
                    RoadSample ns;
                    ns.x = end.x + (tgt.x - end.x) * f;
                    ns.z = end.z + (tgt.z - end.z) * f;
                    ns.y = end.y + (tgt.y - end.y) * f;
                    ns.tx = dx; ns.tz = dz; ns.bank = 0.0f;
                    if (endSel) P[e].s.push_back(ns);
                    else        P[e].s.insert(P[e].s.begin(), ns);
                }
                // Head insertions land in reverse travel order — recompute ALL
                // tangents from final positions so no quad twists at the seam.
                std::vector<RoadSample>& sv = P[e].s;
                for (size_t i = 0; i < sv.size(); ++i) {
                    const RoadSample& p = sv[i > 0 ? i - 1 : 0];
                    const RoadSample& q = sv[i + 1 < sv.size() ? i + 1 : sv.size() - 1];
                    float tx2 = q.x - p.x, tz2 = q.z - p.z;
                    const float l2 = std::sqrt(tx2*tx2 + tz2*tz2);
                    if (l2 > 1e-5f) { tx2 /= l2; tz2 /= l2; }
                    sv[i].tx = tx2; sv[i].tz = tz2;
                }
            }
            addCandidate((end.x + tgt.x) * 0.5f, (end.z + tgt.z) * 0.5f, e, bestO);
        }
    }
    // Pass B: interior crossings — per ground pair, global closest approach.
    for (size_t a = 0; a < P.size(); ++a) {
        if (!isGround(a)) continue;
        for (size_t b2 = a + 1; b2 < P.size(); ++b2) {
            if (!isGround(b2)) continue;
            float bd = 1e30f; size_t bi = 0, bj = 0;
            for (size_t i = 0; i < P[a].s.size(); i += 2)
                for (size_t j = 0; j < P[b2].s.size(); j += 2) {
                    const float dx = P[a].s[i].x - P[b2].s[j].x;
                    const float dz = P[a].s[i].z - P[b2].s[j].z;
                    const float d = dx*dx + dz*dz;
                    if (d < bd) { bd = d; bi = i; bj = j; }
                }
            const float thresh = kJuncCross * 0.5f * (P[a].width + P[b2].width);
            if (bd < thresh * thresh)
                addCandidate((P[a].s[bi].x + P[b2].s[bj].x) * 0.5f,
                             (P[a].s[bi].z + P[b2].s[bj].z) * 0.5f, a, b2);
        }
    }
    // Finalize: radius from the widest participant, y from nearest samples.
    for (Junc& j : J) {
        float maxHalf = 0.0f, y = -1e30f;
        for (size_t e : j.es) {
            maxHalf = std::max(maxHalf, P[e].width * 0.5f);
            float bd = 1e30f; float by = 0.0f;
            for (const RoadSample& s : P[e].s) {
                const float dx = s.x - j.x, dz = s.z - j.z;
                const float d = dx*dx + dz*dz;
                if (d < bd) { bd = d; by = s.y; }
            }
            y = std::max(y, by);
        }
        j.r = std::max(kPatchMinR, maxHalf + kPatchApron);
        j.y = y;
    }
    m_junctionCount = (uint32_t)J.size();

    // ======================= GRAPH (full centers) =========================
    for (size_t e = 0; e < P.size(); ++e) {
        RoadEdge re;
        re.cls = P[e].cls; re.width = P[e].width;
        re.lanesF = P[e].lanesF; re.lanesB = P[e].lanesB;
        if (P[e].closedLoop) {
            m_graph.nodes.push_back({ P[e].s.front().x, P[e].s.front().z });
            re.a = re.b = (uint32_t)m_graph.nodes.size() - 1;
        } else {
            m_graph.nodes.push_back({ P[e].s.front().x, P[e].s.front().z });
            m_graph.nodes.push_back({ P[e].s.back().x,  P[e].s.back().z  });
            re.a = (uint32_t)m_graph.nodes.size() - 2;
            re.b = (uint32_t)m_graph.nodes.size() - 1;
        }
        re.length = kSampleStep * (float)P[e].s.size();
        m_pavedMeters += re.length;
        re.center = P[e].s;   // full (extended) centerline — car AI drives THROUGH junctions
        m_graph.edges.push_back(std::move(re));
    }

    // ======================= PHASE 3 — EMISSION ===========================
    Buck asphalt { &m_buckets[kBucketAsphalt].v,  &m_buckets[kBucketAsphalt].i };
    Buck paint   { &m_buckets[kBucketPaint].v,    &m_buckets[kBucketPaint].i };
    Buck conc    { &m_buckets[kBucketConcrete].v, &m_buckets[kBucketConcrete].i };
    Buck walk    { &m_buckets[kBucketSidewalk].v, &m_buckets[kBucketSidewalk].i };
    Buck shoul   { &m_buckets[kBucketShoulder].v, &m_buckets[kBucketShoulder].i };
    Buck grime   { &m_buckets[kBucketGrime].v,    &m_buckets[kBucketGrime].i };
    Buck nglow   { &m_buckets[kBucketNightGlow].v,&m_buckets[kBucketNightGlow].i };
    // V7 SURFACE PASS: textured buckets. The tint is a MULTIPLIER on the tile
    // albedo (v1's road-metric UVs finally collect their bet) — near-unity for
    // textured buckets; the old flat colors remain the no-texture fallback.
    {
        auto loadTile = [&](const char* rel) -> x3::rhi::TextureHandle {
            const std::string path = assetRoot() + "/roads/" + rel;
            std::ifstream f(path, std::ios::binary);
            if (!f) { x3::logWarn(std::string("[roads] V7 texture MISSING: ") + path); return {}; }
            std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(f)),
                                              std::istreambuf_iterator<char>());
            int w = 0, h = 0, comp = 0;
            unsigned char* d = stbi_load_from_memory(bytes.data(), (int)bytes.size(),
                                                     &w, &h, &comp, 4);
            if (!d) { x3::logWarn(std::string("[roads] V7 texture DECODE failed: ") + path); return {}; }
            x3::rhi::TextureHandle t = device.createTexture(d, (uint32_t)w, (uint32_t)h, true);
            stbi_image_free(d);
            return t;
        };
        m_buckets[kBucketAsphalt].tex  = loadTile("asphalt_tile.png");
        m_buckets[kBucketConcrete].tex = loadTile("concrete_tile.png");
        m_buckets[kBucketSidewalk].tex = loadTile("sidewalk_tile.png");
        // V7.3 (slop verdict: "your huge shoulders are invisible"): the
        // breakdown lane goes back to ASPHALT, crushed dark below — a hard
        // value break against the pale concrete lanes that reads at vista
        // range, with the white edge line dividing them.
        m_buckets[kBucketShoulder].tex = m_buckets[kBucketAsphalt].tex;
        m_buckets[kBucketGrime].tex    = loadTile("grime_tile.png");
        const int texOk = (m_buckets[kBucketAsphalt].tex.valid() ? 1 : 0) +
                          (m_buckets[kBucketConcrete].tex.valid() ? 1 : 0) +
                          (m_buckets[kBucketSidewalk].tex.valid() ? 1 : 0) +
                          (m_buckets[kBucketGrime].tex.valid() ? 1 : 0);
        x3::logInfo("[roads] V7 textures: " + std::to_string(texOk) + "/4 tiles loaded");
    }
    const bool texA = m_buckets[kBucketAsphalt].tex.valid();
    const float cAsphalt[4]  = { texA ? 0.95f : 0.085f, texA ? 0.95f : 0.088f, texA ? 1.0f : 0.095f, 1.0f };
    const float cPaint[4]    = { 0.80f,  0.82f,  0.85f,  1.0f };
    const bool texC = m_buckets[kBucketConcrete].tex.valid();
    const float cConcrete[4] = { texC ? 0.95f : 0.42f, texC ? 0.95f : 0.41f, texC ? 0.95f : 0.39f, 1.0f };
    const bool texW = m_buckets[kBucketSidewalk].tex.valid();
    const float cWalk[4]     = { texW ? 0.92f : 0.295f, texW ? 0.92f : 0.30f, texW ? 0.92f : 0.31f, 1.0f };
    // Shoulder (V7.3): asphalt crushed DARK — the value break vs pale lanes.
    const float cShoulder[4] = { texA ? 0.55f : 0.10f, texA ? 0.55f : 0.10f, texA ? 0.60f : 0.11f, 1.0f };
    // Grime: the blotch tile crushed DARK — reads as rubber + oil on asphalt.
    const float cGrime[4]    = { 0.16f, 0.16f, 0.17f, 1.0f };
    // NightGlow: warm HDR lamp heads (drawn by drawNightGlow with emissive).
    // Tim slop-pass 2026-07-29: 2.1 HDR at close range bloomed into a street-
    // swallowing supernova. Warm and visible, not nuclear.
    const float cGlow[4]     = { 1.05f, 0.72f, 0.34f, 1.0f };
    std::copy(cAsphalt,  cAsphalt + 4,  m_buckets[kBucketAsphalt].color);
    std::copy(cPaint,    cPaint + 4,    m_buckets[kBucketPaint].color);
    std::copy(cConcrete, cConcrete + 4, m_buckets[kBucketConcrete].color);
    std::copy(cWalk,     cWalk + 4,     m_buckets[kBucketSidewalk].color);
    std::copy(cShoulder, cShoulder + 4, m_buckets[kBucketShoulder].color);
    std::copy(cGrime,    cGrime + 4,    m_buckets[kBucketGrime].color);
    std::copy(cGlow,     cGlow + 4,     m_buckets[kBucketNightGlow].color);

    uint32_t lampCount = 0;
    for (size_t e = 0; e < P.size(); ++e) {
        const Pending& pe = P[e];
        // Suppression: a sample inside any participating junction's trim disc.
        std::vector<char> keep(pe.s.size(), 1);
        for (const Junc& j : J) {
            if (std::find(j.es.begin(), j.es.end(), e) == j.es.end()) continue;
            const float rr = (j.r - kPatchTuck) * (j.r - kPatchTuck);
            for (size_t i = 0; i < pe.s.size(); ++i) {
                const float dx = pe.s[i].x - j.x, dz = pe.s[i].z - j.z;
                if (dx*dx + dz*dz < rr) keep[i] = 0;
            }
        }
        // Stop bars at every keep-transition of a ground edge (junction entry;
        // paint bucket — lane paint itself BREAKS because runs split here).
        if (isGround(e)) {
            for (size_t i = 0; i + 1 < pe.s.size(); ++i) {
                if (keep[i] == keep[i + 1]) continue;
                const RoadSample& s = pe.s[keep[i] ? i : i + 1];
                float px, pz; rperp(s.tx, s.tz, px, pz);
                const float wHalf = pe.width * 0.40f;
                const float A[3] = { s.x + px*(-wHalf) - s.tx*kStopBarW*0.5f, s.y + kPaintLift,
                                     s.z + pz*(-wHalf) - s.tz*kStopBarW*0.5f };
                const float B[3] = { s.x + px*( wHalf) - s.tx*kStopBarW*0.5f, s.y + kPaintLift,
                                     s.z + pz*( wHalf) - s.tz*kStopBarW*0.5f };
                const float C[3] = { s.x + px*( wHalf) + s.tx*kStopBarW*0.5f, s.y + kPaintLift,
                                     s.z + pz*( wHalf) + s.tz*kStopBarW*0.5f };
                const float D[3] = { s.x + px*(-wHalf) + s.tx*kStopBarW*0.5f, s.y + kPaintLift,
                                     s.z + pz*(-wHalf) + s.tz*kStopBarW*0.5f };
                const float n[3] = { 0, 1, 0 };
                quad(paint, A, B, C, D, n, 0.0f, 0.05f);
                // V7 CROSSWALK: zebra stripes just outboard of the stop bar
                // (ground streets/avenues only — WD2's street language).
                {
                    const int nStripes = (int)(wHalf * 2.0f / 0.9f);
                    for (int st2 = 0; st2 < nStripes; ++st2) {
                        const float l0 = -wHalf + (float)st2 * 0.9f + 0.15f;
                        const float l1 = l0 + 0.55f;
                        const float f0 = kStopBarW * 0.5f + 0.5f;
                        const float f1 = f0 + 2.4f;
                        const float CA2[3] = { s.x + px*l0 + s.tx*f0, s.y + kPaintLift, s.z + pz*l0 + s.tz*f0 };
                        const float CB2[3] = { s.x + px*l1 + s.tx*f0, s.y + kPaintLift, s.z + pz*l1 + s.tz*f0 };
                        const float CC2[3] = { s.x + px*l1 + s.tx*f1, s.y + kPaintLift, s.z + pz*l1 + s.tz*f1 };
                        const float CD2[3] = { s.x + px*l0 + s.tx*f1, s.y + kPaintLift, s.z + pz*l0 + s.tz*f1 };
                        quad(paint, CA2, CB2, CC2, CD2, n, 0.0f, 0.24f);
                    }
                }
            }
        }
        // Split into kept runs; emit each with the edge's recipe.
        std::vector<std::vector<RoadSample>> runs;
        {
            std::vector<RoadSample> cur;
            for (size_t i = 0; i < pe.s.size(); ++i) {
                if (keep[i]) cur.push_back(pe.s[i]);
                else if (!cur.empty()) { runs.push_back(std::move(cur)); cur.clear(); }
            }
            if (!cur.empty()) runs.push_back(std::move(cur));
        }
        for (const auto& run : runs) {
            if (run.size() < 2) continue;
            // V7.2 (Tim: "the bridge still looks like asphalt"): elevated
            // decks are CONCRETE — CA pours its viaducts, it doesn't pave
            // them. Ground classes stay asphalt.
            const bool elevated = pe.cls == RoadClass::Freeway ||
                                  pe.cls == RoadClass::Ramp;
            ribbon(elevated ? conc : asphalt, run, 0.0f, pe.width, 0.0f,
                   pe.banked, &m_collision, /*underside=*/false);
            // V7.3: the box section replaces the old reversed-index underside
            // sheet — real fascia walls + a soffit at -kDeckDepth spanning the
            // FULL top surface (lanes + shoulders on the freeway).
            if (elevated) {
                const float fullW = pe.width +
                    (pe.cls == RoadClass::Freeway ? 2.0f * kShoulderW : 0.0f);
                deckFascia(conc, run, fullW, pe.banked);
            }
            // V7 SHOULDERS (Tim: "does it have a shoulder?"): a worn, lighter
            // 1.6m band each side of the deck, DRIVABLE (in the collision mesh);
            // barriers move outboard of them.
            const bool hasShoulder = (pe.cls == RoadClass::Freeway);
            if (hasShoulder) {
                const float so = pe.width * 0.5f + kShoulderW * 0.5f;
                ribbon(shoul, run,  so, kShoulderW, 0.0f, pe.banked, &m_collision, true);
                ribbon(shoul, run, -so, kShoulderW, 0.0f, pe.banked, &m_collision, true);
            }
            if (pe.barriers) {
                const float bo = hasShoulder ? (pe.width * 0.5f + kShoulderW - kBarrierInset)
                                             : (pe.width * 0.5f - kBarrierInset);
                barrier(conc, run,  bo);
                barrier(conc, run, -bo);
            }
            // V7 SKID MARKS: dark rubber streaks in the wheel lanes through
            // banked curves — deterministic (hash on edge index + arc bucket).
            if (pe.cls == RoadClass::Freeway || pe.cls == RoadClass::Ramp) {
                float arc2 = 0.0f;
                for (size_t i = 0; i + 3 < run.size(); i += 3) {
                    const float dx = run[i+3].x - run[i].x, dz = run[i+3].z - run[i].z;
                    const float seg = std::sqrt(dx*dx + dz*dz);
                    if (std::fabs(run[i].bank) > 0.025f &&
                        h01((uint32_t)e * 97u + (uint32_t)(arc2 * 0.13f)) > 0.62f) {
                        std::vector<RoadSample> sk(run.begin() + i,
                                                   run.begin() + std::min(i + 4, run.size()));
                        const float lane = ((uint32_t)(arc2) & 1u) ? kLaneWidth : -kLaneWidth;
                        const float drift = (h01((uint32_t)(arc2 * 7.0f)) - 0.5f) * 0.7f;
                        ribbon(grime, sk, lane + drift, 0.30f, kPaintLift * 0.6f, pe.banked);
                        ribbon(grime, sk, lane + drift + 1.55f, 0.30f, kPaintLift * 0.6f, pe.banked);
                    }
                    arc2 += seg;
                }
            }
            // V7 ROADSIDE DEBRIS: hash-scattered crates/barrier blocks on the
            // verges of ground roads (never on the deck itself).
            if (pe.cls == RoadClass::Avenue || pe.cls == RoadClass::HarborStreet) {
                float since2 = 40.0f * h01((uint32_t)e * 53u);
                for (size_t i = 0; i + 1 < run.size(); ++i) {
                    const float dx = run[i+1].x - run[i].x, dz = run[i+1].z - run[i].z;
                    since2 += std::sqrt(dx*dx + dz*dz);
                    if (since2 < 85.0f) continue;
                    since2 = 0.0f;
                    const uint32_t hsd = (uint32_t)e * 131u + (uint32_t)i * 17u;
                    if (h01(hsd) > 0.55f) continue;      // sparse, not littered
                    const RoadSample& sm2 = run[i];
                    float px2, pz2; rperp(sm2.tx, sm2.tz, px2, pz2);
                    const float side2 = (h01(hsd + 3u) > 0.5f) ? 1.0f : -1.0f;
                    const float lat2 = side2 * (pe.width * 0.5f + kCurbW + kWalkW + 0.9f);
                    const float bx2 = sm2.x + px2 * lat2, bz2 = sm2.z + pz2 * lat2;
                    const float gy2 = hf.heightAt(bx2, bz2);
                    const float hh2 = 0.45f + 0.5f * h01(hsd + 7u);
                    box4(conc, bx2, bz2, gy2, gy2 + hh2, 0.35f + 0.25f * h01(hsd + 11u));
                }
            }
            if (pe.edgeLines) {
                ribbon(paint, run,  (pe.width * 0.5f - kEdgeLineInset), kPaintW, kPaintLift, pe.banked);
                ribbon(paint, run, -(pe.width * 0.5f - kEdgeLineInset), kPaintW, kPaintLift, pe.banked);
            }
            if (pe.medianPaint) {
                ribbon(paint, run,  0.30f, kPaintW, kPaintLift, pe.banked);
                ribbon(paint, run, -0.30f, kPaintW, kPaintLift, pe.banked);
            }
            if (pe.laneDash) {
                dashes(paint, run,  kLaneWidth, kPaintW, kPaintLift, kDashOn, kDashOff);
                dashes(paint, run, -kLaneWidth, kPaintW, kPaintLift, kDashOn, kDashOff);
            }
            if (pe.streetDash)
                dashes(paint, run, 0.0f, kPaintW, kPaintLift, kStreetDashOn, kStreetDashOff);
            if (pe.curbs) {
                const float ee = pe.width * 0.5f;
                ribbon(conc, run,  ee + kCurbW * 0.5f, kCurbW, kCurbLift, false);
                ribbon(conc, run, -ee - kCurbW * 0.5f, kCurbW, kCurbLift, false);
                ribbon(walk, run,  ee + kCurbW + kWalkW * 0.5f, kWalkW, kWalkLift, false);
                ribbon(walk, run, -ee - kCurbW - kWalkW * 0.5f, kWalkW, kWalkLift, false);
            }
            if (pe.pillarPair || pe.pillarSingle) {
                float since = kPillarEvery;
                for (size_t i = 0; i + 1 < run.size(); ++i) {
                    const float dx = run[i+1].x - run[i].x, dz = run[i+1].z - run[i].z;
                    since += std::sqrt(dx*dx + dz*dz);
                    if (since < kPillarEvery) continue;
                    since = 0;
                    const RoadSample& s = run[i];
                    const float g = hf.heightAt(s.x, s.z);
                    if (s.y - g <= kPillarMinAir) continue;
                    // V7.3: piers carry the deck perp for their hammerheads.
                    float px, pz; rperp(s.tx, s.tz, px, pz);
                    if (pe.pillarPair) {
                        pillar(conc, s.x + px * 4.8f, s.z + pz * 4.8f, s.y, g, px, pz);
                        pillar(conc, s.x - px * 4.8f, s.z - pz * 4.8f, s.y, g, px, pz);
                    } else {
                        pillar(conc, s.x, s.z, s.y, g, px, pz);
                    }
                }
            }
            if (pe.lamps) {
                float since = pe.lampEvery * (0.3f + 0.5f * h01((uint32_t)e * 31u));
                int side = ((uint32_t)e & 1u) ? 1 : -1;
                for (size_t i = 0; i + 1 < run.size(); ++i) {
                    const float dx = run[i+1].x - run[i].x, dz = run[i+1].z - run[i].z;
                    since += std::sqrt(dx*dx + dz*dz);
                    if (since < pe.lampEvery) continue;
                    since = 0; side = -side;
                    const RoadSample& s = run[i];
                    float px, pz; rperp(s.tx, s.tz, px, pz);
                    const float latEdge = pe.curbs ? (pe.width * 0.5f + kCurbW + 0.4f)
                                                   : (pe.width * 0.5f + 0.6f);
                    const float lat = side * latEdge;
                    const float lx = s.x + px * lat, lz = s.z + pz * lat;
                    x3::rhi::PointLight L;
                    L.pos[0] = lx; L.pos[1] = s.y + pe.lampH; L.pos[2] = lz;
                    L.range = (pe.cls == RoadClass::Freeway) ? 26.0f : 18.0f;
                    if (pe.cls == RoadClass::Freeway) {
                        L.color[0] = 2.2f; L.color[1] = 1.35f; L.color[2] = 0.62f;
                    } else {
                        L.color[0] = 1.7f; L.color[1] = 1.15f; L.color[2] = 0.62f;
                    }
                    m_lights.push_back(L);
                    ++lampCount;
                    // V3: the fixture — pole shaft + arm toward the roadway,
                    // so the light no longer floats (concrete bucket).
                    box4(conc, lx, lz, s.y, s.y + pe.lampH - 0.10f, kPoleHalf);
                    const float ax2 = -(float)side * px, az2 = -(float)side * pz;
                    const float ay = s.y + pe.lampH - 0.18f;
                    const float A[3] = { lx - px*0.06f,               ay, lz - pz*0.06f };
                    const float B[3] = { lx + px*0.06f,               ay, lz + pz*0.06f };
                    const float C[3] = { lx + px*0.06f + ax2*kArmLen, ay, lz + pz*0.06f + az2*kArmLen };
                    const float D[3] = { lx - px*0.06f + ax2*kArmLen, ay, lz - pz*0.06f + az2*kArmLen };
                    const float nA[3] = { 0, 1, 0 };
                    quad(conc, A, B, C, D, nA, 0.0f, kArmLen / 10.0f);
                    // V7 NIGHT GLOW HEAD: a small warm quad under the arm tip —
                    // emitted always, DRAWN only via drawNightGlow (integrator
                    // gates on cityLightsOn; slop-sweep finding D).
                    {
                        const float gx2 = lx + ax2 * (kArmLen - 0.25f);
                        const float gz2 = lz + az2 * (kArmLen - 0.25f);
                        const float gy3 = ay - 0.12f;
                        const float GA[3] = { gx2 - px*0.35f, gy3, gz2 - pz*0.35f };
                        const float GB[3] = { gx2 + px*0.35f, gy3, gz2 + pz*0.35f };
                        const float GC[3] = { gx2 + px*0.35f + ax2*0.7f, gy3, gz2 + pz*0.35f + az2*0.7f };
                        const float GD[3] = { gx2 - px*0.35f + ax2*0.7f, gy3, gz2 - pz*0.35f + az2*0.7f };
                        const float gn[3] = { 0, -1, 0 };
                        quad(nglow, GA, GB, GC, GD, gn, 0.0f, 1.0f);
                    }
                }
            }
        }
    }

    // Junction patches: filled 12-gon per junction (asphalt bucket, visual
    // double-sided fan) + the same fan single-sided into the collision mesh —
    // the patch surface is what a car/player actually stands on at a crossing.
    for (const Junc& j : J) {
        const uint32_t base = (uint32_t)m_buckets[kBucketAsphalt].v.size();
        auto pushV = [&](float x, float y, float z, float u, float v) {
            x3::rhi::MeshVertex mv;
            mv.pos[0]=x; mv.pos[1]=y; mv.pos[2]=z;
            mv.normal[0]=0; mv.normal[1]=1; mv.normal[2]=0;
            mv.uv[0]=u; mv.uv[1]=v;
            m_buckets[kBucketAsphalt].v.push_back(mv);
        };
        pushV(j.x, j.y + 0.02f, j.z, 0.5f, 0.5f);
        for (int k2 = 0; k2 < kPatchSides; ++k2) {
            const float a = (float)k2 / (float)kPatchSides * 6.2831853f;
            pushV(j.x + std::cos(a) * j.r, j.y + 0.02f, j.z + std::sin(a) * j.r,
                  0.5f + 0.5f * std::cos(a), 0.5f + 0.5f * std::sin(a));
        }
        const uint32_t cbase = (uint32_t)(m_collision.verts.size() / 3);
        for (int k2 = 0; k2 <= kPatchSides; ++k2) {
            const x3::rhi::MeshVertex& mv = m_buckets[kBucketAsphalt].v[base + k2];
            m_collision.verts.push_back(mv.pos[0]);
            m_collision.verts.push_back(mv.pos[1]);
            m_collision.verts.push_back(mv.pos[2]);
        }
        for (int k2 = 0; k2 < kPatchSides; ++k2) {
            const uint32_t i0 = base, i1 = base + 1 + (uint32_t)k2,
                           i2 = base + 1 + (uint32_t)((k2 + 1) % kPatchSides);
            const uint32_t f[6] = { i0, i1, i2, i0, i2, i1 };
            m_buckets[kBucketAsphalt].i.insert(m_buckets[kBucketAsphalt].i.end(), f, f + 6);
            const uint32_t c[3] = { cbase, cbase + 1 + (uint32_t)k2,
                                    cbase + 1 + (uint32_t)((k2 + 1) % kPatchSides) };
            m_collision.indices.insert(m_collision.indices.end(), c, c + 3);
        }
    }

    // ---- UPLOAD -----------------------------------------------------------
    device.beginUploadBatch();
    for (int b = 0; b < kBucketCount; ++b) {
        Bucket& bk = m_buckets[b];
        if (bk.v.empty()) continue;
        bk.mesh = device.createMesh(bk.v.data(), (uint32_t)bk.v.size(),
                                    bk.i.data(), (uint32_t)bk.i.size());
        m_vertexCount += (uint32_t)bk.v.size();
    }
    device.endUploadBatch();
    m_built = true;
    // #34a: ECHO_EXPORT_CORRIDORS=<path> — dump the corridor polylines for the
    // OFFLINE bakes (SimCityLLM2 place_props.py): the baked skyline/props GLB
    // predates the road network, so its towers stand in the freeway's footprint
    // (Tim's pier-through-tower capture). The bake consumes this and avoids.
    if (const char* cp = std::getenv("ECHO_EXPORT_CORRIDORS")) {
        std::ofstream cf(cp);
        if (cf) {
            cf << "{\"corridors\":[\n";
            for (size_t ei = 0; ei < m_graph.edges.size(); ++ei) {
                const RoadEdge& e = m_graph.edges[ei];
                const bool elevated = e.cls == RoadClass::Freeway || e.cls == RoadClass::Ramp;
                const float half = e.width * 0.5f + (elevated ? kShoulderW + kBarrierW : 0.5f);
                cf << "{\"cls\":" << (int)e.cls << ",\"half\":" << half << ",\"pts\":[";
                for (size_t i = 0; i < e.center.size(); i += 2) {
                    if (i) cf << ",";
                    cf << "[" << e.center[i].x << "," << e.center[i].z << "]";
                }
                cf << "]}" << (ei + 1 < m_graph.edges.size() ? ",\n" : "\n");
            }
            cf << "]}\n";
            x3::logInfo(std::string("[roads] corridors exported -> ") + cp);
        }
    }
    x3::logInfo("[roads] network built — " + std::to_string((int)m_pavedMeters) +
                " paved m, " + std::to_string(m_graph.edges.size()) + " edges, " +
                std::to_string(m_vertexCount) + " verts, " +
                std::to_string(lampCount) + " lamps (with fixtures)");
    x3::logInfo("[roads] junctions: " + std::to_string(m_junctionCount) +
                " patches; collision: " +
                std::to_string(m_collision.indices.size() / 3) + " tris");
    x3::logInfo("[roads] weld: " + std::to_string(m_vertexCount) +
                " verts vs " + std::to_string(g_unweldedEquiv) +
                " unwelded-equivalent (" +
                std::to_string(g_unweldedEquiv > m_vertexCount ?
                    (int)(100.0f * (1.0f - (float)m_vertexCount / (float)g_unweldedEquiv)) : 0) +
                "% saved)");
    return true;
}

void EchoRoads::draw(x3::rhi::IRenderDevice& device,
                     const x3::rhi::FrameContext& frame) const {
    if (!m_built) return;
    static const float kIdent[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    static const float kNoEmis[4] = { 0, 0, 0, 0 };
    for (int b = 0; b < kBucketCount; ++b) {
        if (b == kBucketNightGlow) continue;   // V7: night-gated (drawNightGlow)
        const Bucket& bk = m_buckets[b];
        if (!bk.mesh.valid()) continue;
        device.drawMeshPBR(frame, bk.mesh, bk.tex, {}, {}, bk.color, kNoEmis, kIdent);
    }
}

void EchoRoads::drawNightGlow(x3::rhi::IRenderDevice& device,
                              const x3::rhi::FrameContext& frame) const {
    // V7: warm emissive lamp heads — the INTEGRATOR calls this only when
    // tod.sample().cityLightsOn (the module never day-gates itself; the
    // noon-lamp slop class dies at the call site).
    if (!m_built) return;
    static const float kIdent[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    const Bucket& bk = m_buckets[kBucketNightGlow];
    if (!bk.mesh.valid()) return;
    const float emis[4] = { bk.color[0], bk.color[1], bk.color[2], 1.0f };
    device.drawMeshPBR(frame, bk.mesh, {}, {}, {}, bk.color, emis, kIdent);
}

} // namespace x3::game
