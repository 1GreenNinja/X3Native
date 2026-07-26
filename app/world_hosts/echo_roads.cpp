// ECHO ROADS implementation — see echo_roads.h for the stance + INTEGRATION.
//
// V3 ARCHITECTURE (Tim: "the roads near the end of the bridge do not even
// join each other. This is NOT how roads look."):
//   PHASE 1  COLLECT   every edge's centerline (ring / trumpet ramps / gate
//                      avenues / spurs / harbor boulevard / fanned grid
//                      blocks) into Pending records — NO geometry yet. The
//                      authoring math is v2's, unchanged (ring shadows the
//                      host's ten kRoute waypoints; harbor probes the real
//                      waterline from seeds).
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
constexpr float kDeckMaxGrade   = 0.60f;
constexpr float kRampMaxGrade   = 0.07f;
constexpr float kBankPerKappa   = 55.0f;
constexpr float kBankMax        = 0.10f;
// (kSmoothWin retired in V3.1 — the profile's grade bound IS the smoothness;
//  box smoothing was re-inflating valleys back toward summit height.)
constexpr float kBarrierInset   = 0.35f;
constexpr float kBarrierW       = 0.35f;
constexpr float kBarrierH       = 1.00f;
constexpr float kEdgeLineInset  = 0.90f;
constexpr float kPaintW         = 0.16f;
constexpr float kPaintLift      = 0.03f;
constexpr float kDashOn         = 3.0f, kDashOff = 9.0f;
constexpr float kStreetDashOn   = 2.0f, kStreetDashOff = 6.0f;
constexpr float kPillarEvery    = 35.0f;
constexpr float kPillarHalf     = 1.30f;
constexpr float kPillarMinAir   = 3.0f;
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

// The ring shadows the host's kRoute EXACTLY (host_echotropolis.cpp ~1541).
struct Wp { float x, z; };
constexpr Wp kRing[] = {
    { -160.0f,  720.0f }, {  120.0f,  720.0f }, {  480.0f,  900.0f },
    {  820.0f, 1120.0f }, { 1060.0f,  900.0f }, {  980.0f,  560.0f },
    {  700.0f,  420.0f }, {  300.0f,  430.0f }, {  -60.0f,  560.0f },
};
constexpr int kRingN = (int)(sizeof(kRing) / sizeof(kRing[0]));

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

// One DOUBLE-SIDED visual quad; optionally also a SINGLE-SIDED collision quad.
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

void ribbon(Buck bk, const std::vector<RoadSample>& s, float off, float w,
            float lift, bool applyBank = true, RoadCollisionMesh* col = nullptr) {
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
        quad(bk, A, B, C, D, n, arc / 10.0f, (arc + seg) / 10.0f, col);
        arc += seg;
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
        quad(bk, aOL, aOH, bOH, bOL, nSide, 0.0f, 0.4f);
        quad(bk, aIH, aIL, bIL, bIH, nSide, 0.0f, 0.4f);
        quad(bk, aIH, aOH, bOH, bIH, nUp,   0.0f, 0.4f);
    }
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

void pillar(Buck bk, float x, float z, float yTop, float yGround) {
    // V3.1 polish: tall piers read slab-like at height — split anything over
    // 20 m into a wider base section + a standard upper shaft (still one
    // bucket, ~4 extra quads). Short piers keep the single shaft.
    const float h = yTop - yGround;
    if (h > 20.0f) {
        const float mid = yGround + h * 0.5f;
        box4(bk, x, z, yGround - 1.5f, mid,        kPillarHalf * 1.4f);
        box4(bk, x, z, mid - 0.5f,     yTop - 0.5f, kPillarHalf);
    } else {
        box4(bk, x, z, yGround - 1.5f, yTop - 0.5f, kPillarHalf);
    }
}

} // namespace

// ---------------------------------------------------------------------------
float RoadGraph::laneCenterOffset(const RoadEdge& e, int lane, bool forward) {
    (void)e;   // v2+: clamp `lane` to e.lanesF/lanesB; v1 trusts the caller
    const float off = ((float)lane + 0.5f) * kLaneWidth;
    return forward ? off : -off;
}

// ================================ build ====================================
bool EchoRoads::build(x3::rhi::IRenderDevice& device, const Heightfield& hf) {
    if (m_built) return true;
    if (!hf.ok()) {
        x3::logWarn("[roads] heightfield not loaded — keeping the legacy freeway");
        return false;
    }

    // ======================= PHASE 1 — COLLECT ============================
    // Pending: an edge's centerline + emission recipe, geometry deferred.
    struct Pending {
        RoadClass cls = RoadClass::Avenue;
        float width = kAvenueWidth;
        int lanesF = 1, lanesB = 1;
        bool banked = false, barriers = false, medianPaint = false,
             edgeLines = false, laneDash = false, streetDash = false,
             curbs = false, lamps = false, pillarPair = false,
             pillarSingle = false, closedLoop = false;
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

    // ---- 1a. THE RING (v2 math verbatim) ---------------------------------
    {
        std::vector<RoadSample> dense;
        dense.reserve(4096);
        for (int i = 0; i < kRingN; ++i)
            for (float t = 0.0f; t < 1.0f; t += 0.02f) {
                RoadSample s; catmull(kRing, kRingN, i, t, s.x, s.z);
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
        const Gate& g = kGates[gi];
        const size_t at = nearestRingSample(g.gx, g.gz);
        const RoadSample deck = ringC[at];
        const float gy = hf.heightAt(g.gx, g.gz) + kGroundLift;
        {   // directional ramp deck -> gate
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
                const float e = t * t * (3.0f - 2.0f * t);
                ramp[i].y = deck.y + (gy - deck.y) * e;
                ramp[i].bank = 0.0f;
            }
            Pending pe;
            pe.cls = RoadClass::Ramp; pe.width = kRampWidth;
            pe.lanesF = 1; pe.lanesB = 0;
            pe.barriers = true; pe.pillarSingle = true;
            pe.s = std::move(ramp);
            P.push_back(std::move(pe));
        }
        {   // trumpet loop ramp ground -> deck
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
                loop[i].bank = -0.06f;
            }
            Pending pe;
            pe.cls = RoadClass::Ramp; pe.width = kRampWidth;
            pe.lanesF = 1; pe.lanesB = 0;
            pe.banked = true; pe.barriers = true; pe.pillarSingle = true;
            pe.s = std::move(loop);
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
        {   // east tie-in to the Urban gate node
            const RoadSample& e = blvdC.back();
            std::vector<RoadSample> d2;
            for (float t = 0.0f; t <= 1.0f; t += 0.04f) {
                RoadSample s;
                hermite(e.x, e.z, e.tx * 120.0f, e.tz * 120.0f,
                        700.0f, 452.0f, 0.0f, 90.0f, t, s.x, s.z);
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
    const float cAsphalt[4]  = { 0.085f, 0.088f, 0.095f, 1.0f };
    const float cPaint[4]    = { 0.80f,  0.82f,  0.85f,  1.0f };
    const float cConcrete[4] = { 0.42f,  0.41f,  0.39f,  1.0f };
    const float cWalk[4]     = { 0.295f, 0.30f,  0.31f,  1.0f };
    std::copy(cAsphalt,  cAsphalt + 4,  m_buckets[kBucketAsphalt].color);
    std::copy(cPaint,    cPaint + 4,    m_buckets[kBucketPaint].color);
    std::copy(cConcrete, cConcrete + 4, m_buckets[kBucketConcrete].color);
    std::copy(cWalk,     cWalk + 4,     m_buckets[kBucketSidewalk].color);

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
            ribbon(asphalt, run, 0.0f, pe.width, 0.0f, pe.banked, &m_collision);
            if (pe.barriers) {
                barrier(conc, run,  (pe.width * 0.5f - kBarrierInset));
                barrier(conc, run, -(pe.width * 0.5f - kBarrierInset));
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
                    if (pe.pillarPair) {
                        float px, pz; rperp(s.tx, s.tz, px, pz);
                        pillar(conc, s.x + px * 4.8f, s.z + pz * 4.8f, s.y, g);
                        pillar(conc, s.x - px * 4.8f, s.z - pz * 4.8f, s.y, g);
                    } else {
                        pillar(conc, s.x, s.z, s.y, g);
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
    x3::logInfo("[roads] network built — " + std::to_string((int)m_pavedMeters) +
                " paved m, " + std::to_string(m_graph.edges.size()) + " edges, " +
                std::to_string(m_vertexCount) + " verts, " +
                std::to_string(lampCount) + " lamps (with fixtures)");
    x3::logInfo("[roads] junctions: " + std::to_string(m_junctionCount) +
                " patches; collision: " +
                std::to_string(m_collision.indices.size() / 3) + " tris");
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
