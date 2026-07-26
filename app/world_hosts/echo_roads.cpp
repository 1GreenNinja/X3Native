// ECHO ROADS implementation — see echo_roads.h for the stance + INTEGRATION.
//
// Shape of the build:
//   1. RING: closed Catmull-Rom through the SAME ten waypoints as the host's
//      kRoute freeway (gates/corridors stay put), arc-length resampled to 4 m,
//      elevation = ridge-clearing profile (raise-only grade limiter + smooth),
//      superelevation (banking) from curvature.
//   2. INTERCHANGES (x2, trumpet): Recife gate (820,1120) and Urban gate
//      (700,420) — the two waypoints the original route itself names as
//      district gates. A trumpet is the standard compact interchange where a
//      single trunk road tees into a freeway: one 250° loop ramp + one
//      directional ramp each (v1 half-trumpet; the mirrored pair is v2), both
//      grading deck -> ground under 7%.
//   3. AVENUES: gate connectors to the Recife/Urban pads, the Hivemind spur
//      (east, off-ring), the crown drag spur (mesa top).
//   4. HARBOR GRID: three street-grid sections along the south bay shore,
//      each rotated to its shoreline tangent ("nicely angled", never
//      axis-aligned), streets truncated at the waterline via hf.
//   5. GEOMETRY: everything batches into 4 material buckets (asphalt / paint /
//      concrete / sidewalk) -> 4 meshes -> 4 draws. Flat PBR colors v1; UVs
//      are road-metric (u across, v = meters/10) so a texture atlas is a
//      drop-in v2. Quads are emitted DOUBLE-SIDED (index-only cost): the deck
//      is seen from below, and this module cannot verify the device's cull
//      winding from here — robustness beats 12 extra bytes a quad.
//   6. LIGHTS: lamp PointLights along freeway + streets, returned via
//      lights() — the HOST gates them by cityLightsOn (never baked emissive;
//      that is the exact bug class Tim flagged on the old glow ribbon).
//
// Determinism: pure math over authored constants + h01() hash jitter — no
// rand, no time. Rebuilding on the same heightfield yields identical meshes.

#include "echo_roads.h"

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace x3::game {

namespace {

// ---- tunables (all of them; nothing magic inline) --------------------------
constexpr float kSampleStep     = 4.0f;    // m between centerline samples (2 on ramps)
constexpr float kRampStep       = 2.0f;
constexpr float kFreewayWidth   = 14.0f;   // 2+2 lanes + shoulders
constexpr float kRampWidth      = 7.0f;
constexpr float kAvenueWidth    = 9.0f;
constexpr float kStreetWidth    = 9.0f;
constexpr float kLaneWidth      = 3.4f;    // graph laneCenterOffset unit
constexpr float kDeckClearance  = 11.0f;   // above cleared terrain (old deckH kept)
constexpr float kDeckMinY       = 13.0f;   // over water (max(g,2)+11 in old code)
constexpr float kRidgeProbe     = 38.0f;   // diamond probe radius for ridge clearing
constexpr float kMaxGrade       = 0.06f;   // 6% freeway; ramps allow 0.07
constexpr float kRampMaxGrade   = 0.07f;
constexpr float kBankPerKappa   = 55.0f;   // bank(rad) = clamp(kappa * this, +-kBankMax)
constexpr float kBankMax        = 0.10f;   // ~5.7 deg superelevation cap
constexpr int   kSmoothWin      = 7;       // profile box-smooth half-window (samples)
constexpr float kBarrierInset   = 0.35f;   // barrier centerline from deck edge
constexpr float kBarrierW       = 0.35f;
constexpr float kBarrierH       = 1.00f;
constexpr float kEdgeLineInset  = 0.90f;   // solid edge line from deck edge
constexpr float kPaintW         = 0.16f;
constexpr float kPaintLift      = 0.03f;   // paint floats above deck (no z-fight)
constexpr float kDashOn         = 3.0f, kDashOff = 9.0f;    // freeway lane dashes
constexpr float kStreetDashOn   = 2.0f, kStreetDashOff = 6.0f;
constexpr float kPillarEvery    = 35.0f;   // m of arc between pillar pairs
constexpr float kPillarHalf     = 1.30f;   // square pier half-extent
constexpr float kPillarMinAir   = 3.0f;    // no pillar if deck hugs ground closer
constexpr float kCurbW          = 0.35f, kCurbLift = 0.13f;
constexpr float kWalkW          = 1.80f, kWalkLift = 0.12f;
constexpr float kGroundLift     = 0.15f;   // tarmac above terrain
constexpr float kLampEveryFwy   = 34.0f;   // m between freeway poles (alternating)
constexpr float kLampEveryStr   = 26.0f;
constexpr float kWaterMinLand   = 1.5f;    // hf below this = water, truncate street
// Loop-ramp arc: radius + sweep of the trumpet curl.
constexpr float kLoopR          = 30.0f;
constexpr float kLoopSweepDeg   = 250.0f;

// The ring shadows the host's kRoute EXACTLY (host_echotropolis.cpp ~1541) so
// district gates / woodlands corridor / car AI handoff keep their geography.
struct Wp { float x, z; };
constexpr Wp kRing[] = {
    { -160.0f,  720.0f }, {  120.0f,  720.0f }, {  480.0f,  900.0f },
    {  820.0f, 1120.0f }, { 1060.0f,  900.0f }, {  980.0f,  560.0f },
    {  700.0f,  420.0f }, {  300.0f,  430.0f }, {  -60.0f,  560.0f },
};  // closed: last->first wraps (the old list's tenth point duplicated its first)
constexpr int kRingN = (int)(sizeof(kRing) / sizeof(kRing[0]));

// Woodlands-style deterministic hash -> [0,1) (same recipe as the scatter).
inline float h01(uint32_t n) {
    n = (n ^ 61u) ^ (n >> 16); n *= 9u; n ^= n >> 4; n *= 0x27d4eb2du;
    n ^= n >> 15; return (float)(n & 0xffffffu) / (float)0x1000000;
}
inline float clampf2(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Closed-loop Catmull-Rom position at (segment i, t in [0,1)).
inline void catmull(const Wp* p, int n, int i, float t, float& ox, float& oz) {
    const Wp& p0 = p[(i - 1 + n) % n]; const Wp& p1 = p[i];
    const Wp& p2 = p[(i + 1) % n];     const Wp& p3 = p[(i + 2) % n];
    const float t2 = t * t, t3 = t2 * t;
    ox = 0.5f * ((2*p1.x) + (-p0.x + p2.x)*t + (2*p0.x - 5*p1.x + 4*p2.x - p3.x)*t2
                 + (-p0.x + 3*p1.x - 3*p2.x + p3.x)*t3);
    oz = 0.5f * ((2*p1.z) + (-p0.z + p2.z)*t + (2*p0.z - 5*p1.z + 4*p2.z - p3.z)*t2
                 + (-p0.z + 3*p1.z - 3*p2.z + p3.z)*t3);
}

// XZ cubic Hermite (ramps/connectors): endpoints + scaled tangents.
inline void hermite(float ax, float az, float atx, float atz,
                    float bx, float bz, float btx, float btz,
                    float t, float& ox, float& oz) {
    const float t2 = t * t, t3 = t2 * t;
    const float h00 = 2*t3 - 3*t2 + 1, h10 = t3 - 2*t2 + t;
    const float h01_ = -2*t3 + 3*t2,   h11 = t3 - t2;
    ox = h00*ax + h10*atx + h01_*bx + h11*btx;
    oz = h00*az + h10*atz + h01_*bz + h11*btz;
}

// Right-perp of a unit XZ tangent (right-hand-traffic side). Convention used
// EVERYWHERE here and exported through RoadGraph::laneCenterOffset.
inline void rperp(float tx, float tz, float& px, float& pz) { px = tz; pz = -tx; }

// Ridge-clearing minimum deck height (the old deckH, verbatim probe pattern).
inline float deckFloor(const Heightfield& hf, float x, float z) {
    float g = hf.heightAt(x, z);
    for (int s = 0; s < 4; ++s)
        g = std::max(g, hf.heightAt(x + (s % 2 ? kRidgeProbe : -kRidgeProbe),
                                    z + (s < 2 ? kRidgeProbe : -kRidgeProbe)));
    return std::max(g, 2.0f) + kDeckClearance;
}

// Arc-length resample of a dense polyline to a fixed step. Fills tangents.
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
    // Tangents from neighbors (ends one-sided).
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

} // namespace

// ---------------------------------------------------------------------------
float RoadGraph::laneCenterOffset(const RoadEdge& e, int lane, bool forward) {
    (void)e;   // v2: clamp `lane` to e.lanesF/lanesB; v1 trusts the caller
    const float off = ((float)lane + 0.5f) * kLaneWidth;
    return forward ? off : -off;   // along rperp(tangent): + is the right side
}

// ============================ geometry emitters ============================
// Small private helpers need bucket access — implemented as member-free
// statics taking the vertex/index vectors directly (keeps the header slim).
namespace {

struct Buck { std::vector<x3::rhi::MeshVertex>* v; std::vector<uint32_t>* i; };

// One DOUBLE-SIDED quad A->B->C->D (A/B = near left/right, D/C = far left/right).
inline void quad(Buck& bk, const float A[3], const float B[3],
                 const float C[3], const float D[3],
                 const float n[3], float v0, float v1) {
    const uint32_t base = (uint32_t)bk.v->size();
    auto push = [&](const float p[3], float u, float vv) {
        x3::rhi::MeshVertex mv;
        mv.pos[0]=p[0]; mv.pos[1]=p[1]; mv.pos[2]=p[2];
        mv.normal[0]=n[0]; mv.normal[1]=n[1]; mv.normal[2]=n[2];
        mv.uv[0]=u; mv.uv[1]=vv;
        bk.v->push_back(mv);
    };
    push(A, 0.0f, v0); push(B, 1.0f, v0); push(C, 1.0f, v1); push(D, 0.0f, v1);
    const uint32_t q[12] = { base,base+1,base+2, base,base+2,base+3,     // front
                             base,base+2,base+1, base,base+3,base+2 };   // back
    bk.i->insert(bk.i->end(), q, q + 12);
}

// Ribbon along samples at lateral [off - w/2, off + w/2], lifted by `lift`,
// banked (the lateral ends tilt by sample.bank). Normal up. v = arc/10.
void ribbon(Buck bk, const std::vector<RoadSample>& s, float off, float w,
            float lift, bool applyBank = true) {
    if (s.size() < 2) return;
    const float n[3] = { 0, 1, 0 };
    float arc = 0.0f;
    for (size_t i = 0; i + 1 < s.size(); ++i) {
        const RoadSample& a = s[i]; const RoadSample& b = s[i + 1];
        float apx, apz, bpx, bpz; rperp(a.tx, a.tz, apx, apz); rperp(b.tx, b.tz, bpx, bpz);
        const float aL = off - w * 0.5f, aR = off + w * 0.5f;
        const float bankA = applyBank ? a.bank : 0.0f, bankB = applyBank ? b.bank : 0.0f;
        const float A[3] = { a.x + apx*aL, a.y + lift + std::sin(bankA)*aL, a.z + apz*aL };
        const float B[3] = { a.x + apx*aR, a.y + lift + std::sin(bankA)*aR, a.z + apz*aR };
        const float dx = b.x - a.x, dz = b.z - a.z;
        const float seg = std::sqrt(dx*dx + dz*dz);
        const float C[3] = { b.x + bpx*aR, b.y + lift + std::sin(bankB)*aR, b.z + bpz*aR };
        const float D[3] = { b.x + bpx*aL, b.y + lift + std::sin(bankB)*aL, b.z + bpz*aL };
        quad(bk, A, B, C, D, n, arc / 10.0f, (arc + seg) / 10.0f);
        arc += seg;
    }
}

// Dashed ribbon: same as ribbon but only emits quads inside the ON phase.
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

// Barrier: an upright box run (outer face + top + inner face) at lateral `off`.
void barrier(Buck bk, const std::vector<RoadSample>& s, float off) {
    if (s.size() < 2) return;
    for (size_t i = 0; i + 1 < s.size(); ++i) {
        const RoadSample& a = s[i]; const RoadSample& b = s[i + 1];
        float apx, apz, bpx, bpz; rperp(a.tx,a.tz,apx,apz); rperp(b.tx,b.tz,bpx,bpz);
        const float hw = kBarrierW * 0.5f;
        auto P = [&](const RoadSample& sm, float px, float pz, float lat, float up,
                     float out[3]) {
            out[0] = sm.x + px * lat; out[1] = sm.y + std::sin(sm.bank)*lat + up;
            out[2] = sm.z + pz * lat;
        };
        float aOL[3], aOH[3], aIL[3], aIH[3], bOL[3], bOH[3], bIL[3], bIH[3];
        P(a, apx, apz, off + hw, 0, aOL);  P(a, apx, apz, off + hw, kBarrierH, aOH);
        P(a, apx, apz, off - hw, 0, aIL);  P(a, apx, apz, off - hw, kBarrierH, aIH);
        P(b, bpx, bpz, off + hw, 0, bOL);  P(b, bpx, bpz, off + hw, kBarrierH, bOH);
        P(b, bpx, bpz, off - hw, 0, bIL);  P(b, bpx, bpz, off - hw, kBarrierH, bIH);
        const float nSide[3] = { apx, 0, apz }, nUp[3] = { 0, 1, 0 };
        quad(bk, aOL, aOH, bOH, bOL, nSide, 0.0f, 0.4f);   // outer face
        quad(bk, aIH, aIL, bIL, bIH, nSide, 0.0f, 0.4f);   // inner face
        quad(bk, aIH, aOH, bOH, bIH, nUp,   0.0f, 0.4f);   // cap
    }
}

// Square pier from ground into the deck underside.
void pillar(Buck bk, float x, float z, float yTop, float yGround) {
    const float h = kPillarHalf;
    const float y0 = yGround - 1.5f, y1 = yTop - 0.5f;
    if (y1 <= y0) return;
    const float c[4][2] = { {-h,-h}, {h,-h}, {h,h}, {-h,h} };
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

} // namespace

// ================================ build ====================================
bool EchoRoads::build(x3::rhi::IRenderDevice& device, const Heightfield& hf) {
    if (m_built) return true;
    if (!hf.ok()) {
        x3::logWarn("[roads] heightfield not loaded — keeping the legacy freeway");
        return false;
    }

    Buck asphalt { &m_buckets[kBucketAsphalt].v,  &m_buckets[kBucketAsphalt].i };
    Buck paint   { &m_buckets[kBucketPaint].v,    &m_buckets[kBucketPaint].i };
    Buck conc    { &m_buckets[kBucketConcrete].v, &m_buckets[kBucketConcrete].i };
    Buck walk    { &m_buckets[kBucketSidewalk].v, &m_buckets[kBucketSidewalk].i };
    // Material palette (flat PBR factors; texture atlas is the v2 swap).
    const float cAsphalt[4]  = { 0.085f, 0.088f, 0.095f, 1.0f };
    const float cPaint[4]    = { 0.80f,  0.82f,  0.85f,  1.0f };
    const float cConcrete[4] = { 0.42f,  0.41f,  0.39f,  1.0f };
    const float cWalk[4]     = { 0.295f, 0.30f,  0.31f,  1.0f };
    std::copy(cAsphalt,  cAsphalt + 4,  m_buckets[kBucketAsphalt].color);
    std::copy(cPaint,    cPaint + 4,    m_buckets[kBucketPaint].color);
    std::copy(cConcrete, cConcrete + 4, m_buckets[kBucketConcrete].color);
    std::copy(cWalk,     cWalk + 4,     m_buckets[kBucketSidewalk].color);

    // ---- 1. THE RING ------------------------------------------------------
    // Dense param sweep of the closed Catmull-Rom, then arc-length resample.
    std::vector<RoadSample> dense;
    dense.reserve(4096);
    for (int i = 0; i < kRingN; ++i)
        for (float t = 0.0f; t < 1.0f; t += 0.02f) {
            RoadSample s; catmull(kRing, kRingN, i, t, s.x, s.z);
            dense.push_back(s);
        }
    dense.push_back(dense.front());   // close the loop for resampling
    std::vector<RoadSample> ring;
    resample(dense, kSampleStep, ring);

    // Elevation: ridge-clearing floor, then a RAISE-ONLY grade limiter (the
    // approach climbs early instead of the deck ever dipping below its floor),
    // then a box smooth, then one re-clamp to the floor.
    std::vector<float> floorY(ring.size());
    for (size_t i = 0; i < ring.size(); ++i) {
        floorY[i] = std::max(deckFloor(hf, ring[i].x, ring[i].z), kDeckMinY);
        ring[i].y = floorY[i];
    }
    const float maxStep = kMaxGrade * kSampleStep;
    for (int pass = 0; pass < 3; ++pass) {
        for (size_t i = 1; i < ring.size(); ++i)                       // forward
            ring[i].y = std::max(ring[i].y, ring[i-1].y - maxStep);
        for (size_t i = ring.size() - 1; i-- > 0; )                    // backward
            ring[i].y = std::max(ring[i].y, ring[i+1].y - maxStep);
    }
    {   // box smooth + floor re-clamp
        std::vector<float> sm(ring.size());
        const int W = kSmoothWin;
        for (int i = 0; i < (int)ring.size(); ++i) {
            float acc = 0; int cnt = 0;
            for (int k = -W; k <= W; ++k) {
                const int j = i + k;
                if (j < 0 || j >= (int)ring.size()) continue;
                acc += ring[j].y; ++cnt;
            }
            sm[i] = acc / (float)cnt;
        }
        for (size_t i = 0; i < ring.size(); ++i)
            ring[i].y = std::max(sm[i], floorY[i]);
    }
    // Banking from signed curvature (tangent swing per meter), smoothed.
    for (size_t i = 0; i < ring.size(); ++i) {
        const RoadSample& p = ring[i > 0 ? i - 1 : ring.size() - 1];
        const RoadSample& q = ring[(i + 1) % ring.size()];
        const float cross = p.tx * q.tz - p.tz * q.tx;   // + = turning right
        const float kappa = cross / (2.0f * kSampleStep);
        ring[i].bank = clampf2(kappa * kBankPerKappa, -kBankMax, kBankMax);
    }
    for (int pass = 0; pass < 2; ++pass)
        for (size_t i = 1; i + 1 < ring.size(); ++i)
            ring[i].bank = (ring[i-1].bank + ring[i].bank + ring[i+1].bank) / 3.0f;

    // Ring geometry: deck, barriers, paint, pillars, lamps.
    ribbon(asphalt, ring, 0.0f, kFreewayWidth, 0.0f);
    barrier(conc, ring,  (kFreewayWidth * 0.5f - kBarrierInset));
    barrier(conc, ring, -(kFreewayWidth * 0.5f - kBarrierInset));
    ribbon(paint, ring,  (kFreewayWidth * 0.5f - kEdgeLineInset), kPaintW, kPaintLift);
    ribbon(paint, ring, -(kFreewayWidth * 0.5f - kEdgeLineInset), kPaintW, kPaintLift);
    ribbon(paint, ring,  0.30f, kPaintW, kPaintLift);   // median double solid
    ribbon(paint, ring, -0.30f, kPaintW, kPaintLift);
    dashes(paint, ring,  kLaneWidth, kPaintW, kPaintLift, kDashOn, kDashOff);
    dashes(paint, ring, -kLaneWidth, kPaintW, kPaintLift, kDashOn, kDashOff);
    {   // pillars + lamps along the ring
        float sinceP = kPillarEvery, sinceL = kLampEveryFwy * 0.5f;
        int lampSide = 1;
        for (size_t i = 0; i + 1 < ring.size(); ++i) {
            sinceP += kSampleStep; sinceL += kSampleStep;
            const RoadSample& s = ring[i];
            if (sinceP >= kPillarEvery) {
                sinceP = 0;
                const float g = hf.heightAt(s.x, s.z);
                if (s.y - g > kPillarMinAir) {
                    float px, pz; rperp(s.tx, s.tz, px, pz);
                    pillar(conc, s.x + px * 4.8f, s.z + pz * 4.8f, s.y, g);
                    pillar(conc, s.x - px * 4.8f, s.z - pz * 4.8f, s.y, g);
                }
            }
            if (sinceL >= kLampEveryFwy) {
                sinceL = 0; lampSide = -lampSide;
                float px, pz; rperp(s.tx, s.tz, px, pz);
                x3::rhi::PointLight L;
                const float lat = lampSide * (kFreewayWidth * 0.5f + 0.6f);
                L.pos[0] = s.x + px * lat; L.pos[1] = s.y + 6.0f; L.pos[2] = s.z + pz * lat;
                L.range = 26.0f;
                L.color[0] = 2.2f; L.color[1] = 1.35f; L.color[2] = 0.62f;   // sodium
                m_lights.push_back(L);
            }
        }
    }
    RoadEdge ringEdge;
    ringEdge.cls = RoadClass::Freeway; ringEdge.width = kFreewayWidth;
    ringEdge.lanesF = 2; ringEdge.lanesB = 2;
    m_graph.nodes.push_back({ ring.front().x, ring.front().z });
    ringEdge.a = ringEdge.b = 0;
    ringEdge.length = kSampleStep * (float)ring.size();
    m_pavedMeters += ringEdge.length;
    ringEdge.center = ring;   // copied AFTER geometry so the edge carries final y/bank
    m_graph.edges.push_back(std::move(ringEdge));
    // NOTE: keep reading from the LOCAL `ring` below — a reference into
    // m_graph.edges[0].center would dangle when later push_backs reallocate.
    const std::vector<RoadSample>& ringC = ring;

    // ---- shared: ground-conforming edge builder --------------------------
    auto nearestRingSample = [&](float x, float z) -> size_t {
        size_t best = 0; float bd = 1e30f;
        for (size_t i = 0; i < ringC.size(); ++i) {
            const float dx = ringC[i].x - x, dz = ringC[i].z - z;
            const float d = dx*dx + dz*dz;
            if (d < bd) { bd = d; best = i; }
        }
        return best;
    };
    auto groundEdge = [&](RoadClass cls, float width, int lanes,
                          std::vector<RoadSample>& s, bool curbs, bool lamps) {
        if (s.size() < 2) return;
        for (auto& p : s) p.y = hf.heightAt(p.x, p.z) + kGroundLift;
        for (int pass = 0; pass < 2; ++pass)                    // tiny smooth
            for (size_t i = 1; i + 1 < s.size(); ++i)
                s[i].y = (s[i-1].y + s[i].y + s[i+1].y) / 3.0f;
        for (auto& p : s) p.bank = 0.0f;
        ribbon(asphalt, s, 0.0f, width, 0.0f, false);
        dashes(paint, s, 0.0f, kPaintW, kPaintLift, kStreetDashOn, kStreetDashOff);
        if (curbs) {
            const float e = width * 0.5f;
            ribbon(conc, s,  e + kCurbW * 0.5f, kCurbW, kCurbLift, false);
            ribbon(conc, s, -e - kCurbW * 0.5f, kCurbW, kCurbLift, false);
            ribbon(walk, s,  e + kCurbW + kWalkW * 0.5f, kWalkW, kWalkLift, false);
            ribbon(walk, s, -e - kCurbW - kWalkW * 0.5f, kWalkW, kWalkLift, false);
        }
        if (lamps) {
            // Deterministic per-street phase stagger (h01, the woodlands hash)
            // so parallel grid streets don't strobe their lamps in sync rows.
            float since = kLampEveryStr * (0.3f + 0.5f * h01((uint32_t)m_graph.edges.size()));
            int side = 1;
            for (size_t i = 0; i + 1 < s.size(); ++i) {
                since += kSampleStep;
                if (since < kLampEveryStr) continue;
                since = 0; side = -side;
                float px, pz; rperp(s[i].tx, s[i].tz, px, pz);
                x3::rhi::PointLight L;
                const float lat = side * (width * 0.5f + kCurbW + 0.4f);
                L.pos[0] = s[i].x + px * lat; L.pos[1] = s[i].y + 4.2f;
                L.pos[2] = s[i].z + pz * lat;
                L.range = 18.0f;
                L.color[0] = 1.7f; L.color[1] = 1.15f; L.color[2] = 0.62f;
                m_lights.push_back(L);
            }
        }
        RoadEdge e2;
        e2.cls = cls; e2.width = width; e2.lanesF = lanes; e2.lanesB = lanes;
        m_graph.nodes.push_back({ s.front().x, s.front().z });
        m_graph.nodes.push_back({ s.back().x,  s.back().z  });
        e2.a = (uint32_t)m_graph.nodes.size() - 2;
        e2.b = (uint32_t)m_graph.nodes.size() - 1;
        e2.length = kSampleStep * (float)s.size();
        m_pavedMeters += e2.length;
        e2.center = std::move(s);
        m_graph.edges.push_back(std::move(e2));
    };

    // ---- 2. INTERCHANGES (trumpet halves at the two named gates) ---------
    // Each: a DIRECTIONAL ramp leaving the deck tangentially and grading to
    // the gate's ground node, plus a 250-degree LOOP ramp curling under for
    // the opposing movement. Both are Ramp-class edges with barriers.
    struct Gate { float gx, gz;         // where the trunk avenue starts (ground)
                  float ex, ez; };      // avenue end (district pad edge)
    const Gate kGates[2] = {
        { 830.0f, 1150.0f,  935.0f, 1235.0f },   // Recife 2050 SW gate -> pad
        { 700.0f,  452.0f,  700.0f,  368.0f },   // Urban District N gate -> pad
    };
    for (int gi = 0; gi < 2; ++gi) {
        const Gate& g = kGates[gi];
        const size_t at = nearestRingSample(g.gx, g.gz);
        const RoadSample deck = ringC[at];
        const float gy = hf.heightAt(g.gx, g.gz) + kGroundLift;
        // Directional ramp: hermite deck->gate, y hermite-eased, grade-checked.
        {
            std::vector<RoadSample> d2;
            const float lead = std::max(120.0f, (deck.y - gy) / kRampMaxGrade);
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
                const float e = t * t * (3.0f - 2.0f * t);          // smoothstep
                ramp[i].y = deck.y + (gy - deck.y) * e;
                ramp[i].bank = 0.0f;
            }
            ribbon(asphalt, ramp, 0.0f, kRampWidth, 0.0f, false);
            barrier(conc, ramp,  (kRampWidth * 0.5f - kBarrierInset));
            barrier(conc, ramp, -(kRampWidth * 0.5f - kBarrierInset));
            for (size_t i = 0; i < ramp.size(); i += (size_t)(kPillarEvery / kRampStep)) {
                const float gr = hf.heightAt(ramp[i].x, ramp[i].z);
                if (ramp[i].y - gr > kPillarMinAir)
                    pillar(conc, ramp[i].x, ramp[i].z, ramp[i].y, gr);
            }
            RoadEdge e2; e2.cls = RoadClass::Ramp; e2.width = kRampWidth;
            e2.lanesF = 1; e2.lanesB = 0;
            m_graph.nodes.push_back({ deck.x, deck.z });
            m_graph.nodes.push_back({ g.gx, g.gz });
            e2.a = (uint32_t)m_graph.nodes.size() - 2;
            e2.b = (uint32_t)m_graph.nodes.size() - 1;
            e2.length = kRampStep * (float)ramp.size();
            m_pavedMeters += e2.length;
            e2.center = std::move(ramp);
            m_graph.edges.push_back(std::move(e2));
        }
        // Loop ramp: circle beside the gate, sweeping kLoopSweepDeg, grading
        // ground->deck (the on-ramp of the trumpet's curl).
        {
            float px, pz; rperp(deck.tx, deck.tz, px, pz);
            const float cx = g.gx + px * (kLoopR + kRampWidth),
                        cz = g.gz + pz * (kLoopR + kRampWidth);
            const float a0 = std::atan2(g.gz - cz, g.gx - cx);
            const float sweep = kLoopSweepDeg * 3.1415926f / 180.0f;
            std::vector<RoadSample> d2;
            for (float a = 0.0f; a <= sweep; a += 0.05f) {
                RoadSample s;
                s.x = cx + std::cos(a0 + a) * kLoopR;
                s.z = cz + std::sin(a0 + a) * kLoopR;
                d2.push_back(s);
            }
            std::vector<RoadSample> loop;
            resample(d2, kRampStep, loop);
            for (size_t i = 0; i < loop.size(); ++i) {
                const float t = (float)i / (float)(loop.size() - 1);
                const float e = t * t * (3.0f - 2.0f * t);
                loop[i].y = gy + (deck.y - gy) * e;
                loop[i].bank = clampf2(-0.06f, -kBankMax, kBankMax);  // constant curl bank
            }
            ribbon(asphalt, loop, 0.0f, kRampWidth, 0.0f);
            barrier(conc, loop,  (kRampWidth * 0.5f - kBarrierInset));
            barrier(conc, loop, -(kRampWidth * 0.5f - kBarrierInset));
            for (size_t i = 0; i < loop.size(); i += (size_t)(kPillarEvery / kRampStep)) {
                const float gr = hf.heightAt(loop[i].x, loop[i].z);
                if (loop[i].y - gr > kPillarMinAir)
                    pillar(conc, loop[i].x, loop[i].z, loop[i].y, gr);
            }
            RoadEdge e2; e2.cls = RoadClass::Ramp; e2.width = kRampWidth;
            e2.lanesF = 1; e2.lanesB = 0;
            m_graph.nodes.push_back({ g.gx, g.gz });
            m_graph.nodes.push_back({ deck.x, deck.z });
            e2.a = (uint32_t)m_graph.nodes.size() - 2;
            e2.b = (uint32_t)m_graph.nodes.size() - 1;
            e2.length = kRampStep * (float)loop.size();
            m_pavedMeters += e2.length;
            e2.center = std::move(loop);
            m_graph.edges.push_back(std::move(e2));
        }
        // Gate avenue into the district pad.
        {
            std::vector<RoadSample> d2;
            for (float t = 0.0f; t <= 1.0f; t += 0.05f) {
                RoadSample s;
                s.x = g.gx + (g.ex - g.gx) * t; s.z = g.gz + (g.ez - g.gz) * t;
                d2.push_back(s);
            }
            std::vector<RoadSample> av; resample(d2, kSampleStep, av);
            groundEdge(RoadClass::Avenue, kAvenueWidth, 1, av, true, true);
        }
    }

    // ---- 3. SPUR AVENUES --------------------------------------------------
    {   // Hivemind spur: east off-ring ground trunk to the pad.
        std::vector<RoadSample> d2;
        const float sx = 1075.0f, sz = 905.0f, ex = 1310.0f, ez = 1000.0f;
        for (float t = 0.0f; t <= 1.0f; t += 0.03f) {
            RoadSample s;
            hermite(sx, sz, 180.0f, 30.0f, ex, ez, 160.0f, 20.0f, t, s.x, s.z);
            d2.push_back(s);
        }
        std::vector<RoadSample> av; resample(d2, kSampleStep, av);
        groundEdge(RoadClass::Avenue, kAvenueWidth, 1, av, true, true);
    }
    {   // Crown drag spur: mesa-top link from the ring toward the drag.
        std::vector<RoadSample> d2;
        for (float t = 0.0f; t <= 1.0f; t += 0.05f) {
            RoadSample s;
            hermite(-100.0f, 690.0f, 90.0f, 40.0f, -24.0f, 752.0f, 60.0f, 30.0f,
                    t, s.x, s.z);
            d2.push_back(s);
        }
        std::vector<RoadSample> av; resample(d2, kSampleStep, av);
        groundEdge(RoadClass::Avenue, kAvenueWidth, 1, av, true, true);
    }

    // ---- 4. HARBOR GRIDS --------------------------------------------------
    // Three sections along the south bay shore, each rotated to its shoreline
    // tangent. A street is generated in section-local space, transformed,
    // then TRUNCATED at the waterline (first sample whose ground is water).
    struct GridSec { float ax, az;      // section anchor (shore side)
                     float angDeg;      // rotation (shoreline tangent)
                     int nLong, nCross; };
    const GridSec kSecs[3] = {
        {  40.0f, 300.0f,  12.0f, 3, 4 },
        { 430.0f, 318.0f, -16.0f, 3, 4 },
        { 780.0f, 350.0f, -40.0f, 2, 3 },
    };
    constexpr float kCell = 42.0f;          // block pitch between long streets
    constexpr float kCrossPitch = 52.0f;    // pitch between cross streets
    for (int si = 0; si < 3; ++si) {
        const GridSec& sec = kSecs[si];
        const float ca = std::cos(sec.angDeg * 3.1415926f / 180.0f);
        const float sa = std::sin(sec.angDeg * 3.1415926f / 180.0f);
        auto toWorld = [&](float lx, float lz, float& wx, float& wz) {
            wx = sec.ax + lx * ca - lz * sa;
            wz = sec.az + lx * sa + lz * ca;
        };
        auto emitStreet = [&](float x0, float z0, float x1, float z1) {
            std::vector<RoadSample> d2;
            for (float t = 0.0f; t <= 1.0f; t += 0.04f) {
                RoadSample s;
                float wx, wz; toWorld(x0 + (x1-x0)*t, z0 + (z1-z0)*t, wx, wz);
                s.x = wx; s.z = wz;
                d2.push_back(s);
            }
            std::vector<RoadSample> st; resample(d2, kSampleStep, st);
            size_t keep = st.size();                     // waterline truncation
            for (size_t i = 0; i < st.size(); ++i)
                if (hf.heightAt(st[i].x, st[i].z) < kWaterMinLand) { keep = i; break; }
            if (keep < 2) return;
            st.resize(keep);
            groundEdge(RoadClass::HarborStreet, kStreetWidth, 1, st, true, true);
        };
        const float longLen = (float)(sec.nCross - 1) * kCrossPitch;
        for (int L = 0; L < sec.nLong; ++L)              // long streets (shore-parallel)
            emitStreet(0.0f, (float)L * kCell, longLen, (float)L * kCell);
        const float crossLen = (float)(sec.nLong - 1) * kCell;
        for (int Cx = 0; Cx < sec.nCross; ++Cx)          // cross streets (to the water)
            emitStreet((float)Cx * kCrossPitch, 0.0f, (float)Cx * kCrossPitch, crossLen);
    }

    // ---- 5. UPLOAD --------------------------------------------------------
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
    x3::logInfo("[roads] network built — " + std::to_string((int)m_pavedMeters) +
                " paved m, " + std::to_string(m_graph.edges.size()) + " edges, " +
                std::to_string(m_vertexCount) + " verts, " +
                std::to_string(m_lights.size()) + " lamps");
    return true;
}

void EchoRoads::draw(x3::rhi::IRenderDevice& device,
                     const x3::rhi::FrameContext& frame) const {
    if (!m_built) return;
    static const float kIdent[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    static const float kNoEmis[4] = { 0, 0, 0, 0 };
    for (int b = 0; b < kBucketCount; ++b) {
        const Bucket& bk = m_buckets[b];
        if (!bk.mesh.valid()) continue;
        device.drawMeshPBR(frame, bk.mesh, {}, {}, {}, bk.color, kNoEmis, kIdent);
    }
}

} // namespace x3::game
