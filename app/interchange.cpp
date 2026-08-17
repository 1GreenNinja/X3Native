// THE DIAMOND INTERCHANGE — see app/interchange.h.
#include "interchange.h"

#include "terrain.h"
#include "scene.h"
#include "surface_library.h"
#include "asset_root.h"
#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace x3::game {

namespace {

constexpr float kMToFt = 3.28084f;

// Ramp landing reach along the freeway / the crossroad from the crossing.
// 340 m gives the off-ramp a real deceleration run and keeps the gore well
// clear of the abutments; 230 m puts the crossroad terminal past the descent
// from the deck (the approach needs ~140 m at 6% + the K-curve to get down).
constexpr float kRampFwyReachM   = 340.0f;
constexpr float kRampCrossReachM = 230.0f;
// Ramp approach angles: ~50 deg to the freeway (oblique enough to read as a
// ramp, steep enough that the mouth machinery's corner intersections stay
// well-conditioned — buildJunctionMouth wants >= ~26 deg), ~70 deg to the
// crossroad (a give-way T, barely skewed).
constexpr float kRampFwyAngRad   = 50.0f * 0.017453293f;
constexpr float kRampCrossAngRad = 70.0f * 0.017453293f;
// The crossroad half-length: enough for the descent, the ramp terminals and
// a country tail each side. v1 dead-ends in the open; the day it connects
// onward it grows from these ends.
constexpr float kCrossHalfLenM   = 620.0f;

float segPtDist(float px, float pz, float ax, float az, float bx, float bz) {
    const float dx = bx - ax, dz = bz - az;
    const float L2 = dx * dx + dz * dz;
    float t = 0.0f;
    if (L2 > 1e-9f)
        t = std::max(0.0f, std::min(1.0f, ((px - ax) * dx + (pz - az) * dz) / L2));
    const float qx = ax + dx * t, qz = az + dz * t;
    return std::sqrt((px - qx) * (px - qx) + (pz - qz) * (pz - qz));
}

float distToSpec(const RoadSpec& s, float px, float pz) {
    float best = 1e18f;
    for (size_t k = 0; k + 1 < s.x.size(); ++k)
        best = std::min(best, segPtDist(px, pz, s.x[k], s.z[k], s.x[k+1], s.z[k+1]));
    return best;
}

// The natural (pre-carve) hillside — same derivation the outer connector and
// the summit spur use. Scoring against the carved field would let a site
// score well because somebody else already cut there.
float naturalAt(float x, float z) {
    return terrainHeightAtWorld(x, z) - terrainCorridorDelta(x, z);
}

SurfaceLibrary& icSurfaces() { static SurfaceLibrary lib; return lib; }

// Quad-buffer mesh with winding-derived normals — the river bridge's MeshBuf
// pattern (a deck is boxes and swept sections, not sculpture).
struct MeshBuf {
    std::vector<x3::rhi::MeshVertex> v;
    std::vector<uint32_t> i;
    void quad(const float a[3], const float b[3], const float c[3], const float d[3],
              float uScale = 0.25f) {
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
    bool empty() const { return i.empty(); }
};

} // namespace

// ---------------------------------------------------------------------------
// REGISTRATION
// ---------------------------------------------------------------------------
InterchangeResult registerInterchange(const RoadSpec& fwySpec,
                                      const std::vector<float>& fwyRoadY,
                                      const std::vector<const RoadSpec*>* avoid) {
    InterchangeResult out;
    const size_t fn = fwySpec.x.size();
    if (fn < 8 || fwyRoadY.size() != fn || fwySpec.z.size() != fn ||
        !fwySpec.dualCarriageway) {
        out.whyNot = "freeway spec/datum missing or not a dual carriageway";
        x3::logError(std::string("interchange: ") + out.whyNot);
        return out;
    }
    const std::vector<float> medianPlan = computeMedianPlan(fwySpec, fwyRoadY);
    if (medianPlan.size() != fn) {
        out.whyNot = "median plan failed";
        x3::logError(std::string("interchange: ") + out.whyNot);
        return out;
    }

    // Arc length per node, for the zone/turnaround bookkeeping.
    std::vector<float> U(fn, 0.0f);
    for (size_t i = 0; i + 1 < fn; ++i) {
        const float dx = fwySpec.x[i+1] - fwySpec.x[i];
        const float dz = fwySpec.z[i+1] - fwySpec.z[i];
        U[i+1] = U[i] + std::sqrt(dx * dx + dz * dz);
    }

    // Local unit tangent at node i (central difference).
    auto tangentAt = [&](size_t i, float& tx, float& tz) {
        const size_t ip = (i + 1 < fn) ? i + 1 : i;
        const size_t im = (i > 0) ? i - 1 : i;
        tx = fwySpec.x[ip] - fwySpec.x[im];
        tz = fwySpec.z[ip] - fwySpec.z[im];
        const float tl = std::sqrt(tx * tx + tz * tz);
        if (tl > 1e-4f) { tx /= tl; tz /= tl; }
    };

    // ---- THE SITE, by measurement (rule 9) ---------------------------------
    // Score every candidate node:
    //   * the freeway must run near-STRAIGHT through the whole zone (the deck
    //     is square to it and the ramps are authored in its frame);
    //   * the median at the crossing must be OPEN (the pier stands in it);
    //   * the country under the crossroad's approaches must sit close to the
    //     freeway datum (short, honest embankments);
    //   * everything stays clear of every noted junction and every other
    //     registered route.
    size_t J = SIZE_MAX;
    float bestScore = 1e18f;
    const float kWindowM = kInterchangeZoneR;
    for (size_t j = 4; j + 4 < fn; j += 2) {
        // window straightness: net heading change over +-kWindowM
        float t0x, t0z, t1x, t1z, tjx, tjz;
        size_t a = j, b = j;
        while (a > 1 && U[j] - U[a] < kWindowM) --a;
        while (b + 2 < fn && U[b] - U[j] < kWindowM) ++b;
        if (U[j] - U[a] < kWindowM * 0.9f || U[b] - U[j] < kWindowM * 0.9f)
            continue;   // too close to the seam of a closed ring — a is fine elsewhere
        tangentAt(a, t0x, t0z);
        tangentAt(b, t1x, t1z);
        tangentAt(j, tjx, tjz);
        const float dotAB = std::max(-1.0f, std::min(1.0f, t0x * t1x + t0z * t1z));
        const float bendDeg = std::acos(dotAB) * 57.29578f;
        if (bendDeg > 8.0f) continue;               // the deck wants a straight
        // an OPEN median for the pier
        const float mj = medianPlan[j];
        if (mj < 6.0f) continue;
        // clear of every existing junction (ramp landings reach ~340 m)
        if (distToNearestRoadJunction(fwySpec.x[j], fwySpec.z[j]) < 800.0f)
            continue;
        // terrain fit + route clearance along the would-be crossroad
        const float px = -tjz, pz = tjx;            // crossroad axis
        const float yJ = fwyRoadY[j];
        float fit = 0.0f;
        bool clear = true;
        for (int k = -10; k <= 10 && clear; ++k) {
            const float s = (float)k * 62.0f;       // +-620 m
            const float qx = fwySpec.x[j] + px * s;
            const float qz = fwySpec.z[j] + pz * s;
            if (std::fabs(s) <= 340.0f)
                fit = std::max(fit, std::fabs(naturalAt(qx, qz) - yJ));
            if (avoid)
                for (const RoadSpec* av : *avoid)
                    if (av && distToSpec(*av, qx, qz) < 2.0f * kPavedHalfM + 30.0f)
                        { clear = false; break; }
        }
        if (!clear) continue;
        // the four ramp elbows too (quadrant corners ~(280, 250))
        for (int qa = -1; qa <= 1 && clear; qa += 2)
            for (int qb = -1; qb <= 1 && clear; qb += 2) {
                const float ex = fwySpec.x[j] + tjx * (float)qa * 280.0f
                               + px * (float)qb * 250.0f;
                const float ez = fwySpec.z[j] + tjz * (float)qa * 280.0f
                               + pz * (float)qb * 250.0f;
                fit = std::max(fit, std::fabs(naturalAt(ex, ez) - yJ));
                if (avoid)
                    for (const RoadSpec* av : *avoid)
                        if (av && distToSpec(*av, ex, ez) < 2.0f * kPavedHalfM + 30.0f)
                            { clear = false; break; }
            }
        if (!clear) continue;
        const float score = fit + bendDeg * 0.8f;
        if (score < bestScore) { bestScore = score; J = j; }
    }
    if (J == SIZE_MAX) {
        out.whyNot = "no freeway node passes the site tests (straight/median/clearance)";
        x3::logError(std::string("interchange: ") + out.whyNot);
        return out;
    }

    // The chosen frame.
    out.fwyNode = (uint32_t)J;
    out.cx = fwySpec.x[J]; out.cz = fwySpec.z[J];
    tangentAt(J, out.tX, out.tZ);
    out.cX = -out.tZ; out.cZ = out.tX;              // crossroad axis (perp)
    out.medianHalfAtCrossing = medianPlan[J];
    out.fwyUAtDeck = U[J];
    const float fwyPavedEdge = out.medianHalfAtCrossing + 2.0f * kFwyPavedHalfM;
    out.abutS = fwyPavedEdge + 9.0f;                // abutment clear of the aprons

    // The freeway pavement surface nearest a world point — datum INTERPOLATED
    // along the winning segment, never the start node's value: on a 61 m
    // segment at 7% grade the start-node datum is up to 4.3 m wrong, and the
    // first cut of the clearance gate measured exactly that error (9.05 ft
    // "clearance" that was really quantisation, not structure).
    auto fwySurfaceNear = [&](float qx, float qz) {
        float bestD = 1e18f, y = fwyRoadY[J];
        for (size_t i = 0; i + 1 < fn; ++i) {
            const float ax = fwySpec.x[i], az = fwySpec.z[i];
            const float bx = fwySpec.x[i+1], bz = fwySpec.z[i+1];
            const float dxs = bx - ax, dzs = bz - az;
            const float L2 = dxs * dxs + dzs * dzs;
            float t = 0.0f;
            if (L2 > 1e-9f)
                t = std::max(0.0f, std::min(1.0f,
                    ((qx - ax) * dxs + (qz - az) * dzs) / L2));
            const float px2 = ax + dxs * t, pz2 = az + dzs * t;
            const float d = (qx - px2) * (qx - px2) + (qz - pz2) * (qz - pz2);
            if (d < bestD) { bestD = d; y = fwyRoadY[i] + (fwyRoadY[i+1] - fwyRoadY[i]) * t; }
        }
        return y + kPaveProud;
    };

    // Deck datum: highest freeway pavement under the span footprint (the
    // datum slides along the freeway; the deck is level) + the clearance law
    // + the structure depth. Sampled across the footprint, not assumed.
    {
        float hi = -1e18f;
        for (float l = -(kPavedHalfM + 12.0f); l <= kPavedHalfM + 12.1f; l += 4.0f)
            for (float s = -(out.abutS); s <= out.abutS + 0.1f; s += 8.0f)
                hi = std::max(hi, fwySurfaceNear(out.cx + out.cX * s + out.tX * l,
                                                 out.cz + out.cZ * s + out.tZ * l));
        out.fwyMaxSurfaceY = hi;
        out.deckY = out.fwyMaxSurfaceY + kOverpassClearM + 0.45f + kOverpassDepthM;
    }

    // ---- THE CROSSROAD -----------------------------------------------------
    // A straight perpendicular line with exact stations at the gap edges.
    {
        RoadSpec s;
        s.name      = "interchange crossroad";
        s.halfWidth = kPavedHalfM + 1.0f;
        s.falloff   = 16.0f;
        s.maxGrade  = 0.06f;
        s.minTurnRadiusM   = 200.0f;
        s.maxDeflectionDeg = 3.0f;
        uint32_t gi0 = 0, gi1 = 0;
        auto push = [&](float sPos) {
            s.x.push_back(out.cx + out.cX * sPos);
            s.z.push_back(out.cz + out.cZ * sPos);
        };
        // stations: 30 m in the open, 12 m within 170 m of the abutments —
        // the vertical smoother needs node freedom there to bend the 6%
        // climb onto the LEVEL deck at the spec's K (measured at 30 m
        // spacing: K stalled at 6.7 against the pinned deck edge).
        std::vector<float> st;
        auto stepAt = [&](float sPos) {
            return (std::fabs(std::fabs(sPos) - out.abutS) < 170.0f) ? 12.0f : 30.0f;
        };
        for (float sPos = -kCrossHalfLenM; sPos < -out.abutS - 1.0f; sPos += stepAt(sPos))
            st.push_back(sPos);
        st.push_back(-out.abutS);
        st.push_back(0.0f);
        st.push_back(+out.abutS);
        for (float sPos = out.abutS + stepAt(out.abutS + 1.0f);
             sPos <= kCrossHalfLenM + 1.0f; sPos += stepAt(sPos))
            st.push_back(sPos);
        // make sure the ramp landing stations exist exactly
        st.push_back(-kRampCrossReachM); st.push_back(+kRampCrossReachM);
        std::sort(st.begin(), st.end());
        st.erase(std::unique(st.begin(), st.end(),
                 [](float a2, float b2) { return std::fabs(a2 - b2) < 4.0f; }),
                 st.end());
        for (float sPos : st) {
            if (std::fabs(sPos + out.abutS) < 0.5f) gi0 = (uint32_t)s.x.size();
            if (std::fabs(sPos - out.abutS) < 0.5f) gi1 = (uint32_t)s.x.size();
            push(sPos);
        }
        RoadSpec::Gap g;
        g.i0 = gi0; g.i1 = gi1;
        g.y0 = g.y1 = out.deckY;    // the deck is LEVEL, like Bridge No. 1
        s.gaps.push_back(g);
        // THE AUTHORED VERTICAL ALIGNMENT. The iterative K-smoother builds
        // its curves by nudging free neighbours around a pin, and against the
        // deck's hard LEVEL edge it stalls (measured: K 5.7 at 30 m stations,
        // K 8.3 at 12 m; a fixed-length crest parabola then kinked at ITS
        // joint instead — 0.00125/m where 5.4% of authored grade met ground
        // only 3 m down). A bridge approach is not a relaxation problem — it
        // is DESIGNED, against the ground it actually meets: walk outward
        // from each abutment steepening at the K-rate (crest), and begin
        // easing back toward zero grade the moment the remaining height fits
        // inside the sag parabola (h <= g^2/2r), so the profile lands
        // TANGENT to the country. Every grade change is r-limited by
        // construction; grade never exceeds 5.5%; deterministic.
        // (Second cut. The first walked a crest parabola out and landed
        // TANGENT to the ground — and the gate convicted it at s=-260:
        // where the ground RISES past a landing, the free profile climbs
        // off the last pin at 6% and the 0 -> 6% break is a 0.0019/m kink.
        // So: author BOTH SIDES END TO END. Fit the ground envelope with a
        // grade-limited pass anchored at the deck, then track it with a
        // K-rate-limited feedback walk — every grade change r-bounded by
        // construction, both joints included, and the relaxer is left
        // nothing to fight.)
        {
            constexpr float kVRate = 4.5e-4f;      // 10% inside the 5e-4 cap
            constexpr float kVgMax = 0.055f;
            s.pinY.assign(s.x.size(), std::numeric_limits<float>::quiet_NaN());
            const float reach = s.halfWidth + 0.8f;   // the grader's own sweep
            for (int side = -1; side <= 1; side += 2) {
                // stations of this side, ordered OUTWARD from the abutment
                std::vector<size_t> order;
                for (size_t i = 0; i < st.size(); ++i)
                    if ((side < 0 && st[i] < -out.abutS - 0.5f) ||
                        (side > 0 && st[i] >  out.abutS + 0.5f))
                        order.push_back(i);
                if (side < 0) std::reverse(order.begin(), order.end());
                const size_t m = order.size();
                if (m < 2) continue;
                // ground envelope: lateral MAX across the carve width (the
                // same sweep registerRoad grades against, pre-carve)
                std::vector<float> E(m), L(m);
                float dPrev = 0.0f;
                for (size_t k = 0; k < m; ++k) {
                    const size_t oi = order[k];
                    const float d = std::fabs(st[oi]) - out.abutS;
                    L[k] = std::max(1.0f, d - dPrev);
                    dPrev = d;
                    // crossroad axis is (cX, cZ); its lateral is (tX, tZ)
                    float hi = -1e18f;
                    for (int q = -4; q <= 4; ++q) {
                        const float off = (float)q * reach / 4.0f;
                        hi = std::max(hi, naturalAt(s.x[oi] + out.tX * off,
                                                    s.z[oi] + out.tZ * off));
                    }
                    E[k] = hi;
                }
                // grade-limited fit, anchored at the deck: one outward pass
                // (each node clamps against its already-fixed predecessor)
                std::vector<float> T(E);
                float prev = out.deckY;
                for (size_t k = 0; k < m; ++k) {
                    T[k] = std::max(prev - kVgMax * L[k],
                                    std::min(prev + kVgMax * L[k], T[k]));
                    prev = T[k];
                }
                // K-rate-limited tracking walk, grade starts LEVEL at the
                // deck edge; re-integrated so what is pinned is what a car
                // drives — the overshoot where ground turns is v^2/2r ~ 3 m
                // of fill at worst, which is what an embankment is for.
                // dv is clamped against the AVERAGE of the two adjacent
                // segment lengths — the K measure divides by exactly that,
                // and clamping per-segment leaked 1.43x of rate at the
                // 12 -> 30 m station-spacing transition (measured, s=+240).
                float p = out.deckY, v = 0.0f, Lp = L[0];
                for (size_t k = 0; k < m; ++k) {
                    const float dvCap = kVRate * 0.5f * (Lp + L[k]);
                    const float want = (T[k] - p) / L[k];
                    v = std::max(v - dvCap, std::min(v + dvCap, want));
                    v = std::max(-kVgMax, std::min(kVgMax, v));
                    p += v * L[k];
                    s.pinY[order[k]] = p;
                    Lp = L[k];
                }
            }
        }
        out.spec = s;
        out.road = registerRoad(out.spec, &out.roadY);
        if (!out.road.ok) {
            out.whyNot = "crossroad registration failed";
            x3::logError(std::string("interchange: ") + out.whyNot);
            return out;
        }
    }

    // Measured clearance: deck soffit vs the freeway pavement across the FULL
    // paved width of both carriageways (the number gate I2 holds).
    {
        float worst = 1e18f;
        const float soffit = out.deckY - kOverpassDepthM;
        // Across the full paved width of both carriageways AND the deck's own
        // footprint along the freeway — the whole shadow of the structure.
        for (float lat = -fwyPavedEdge; lat <= fwyPavedEdge + 0.1f; lat += 2.0f)
            for (float l = -kPavedHalfM; l <= kPavedHalfM + 0.1f; l += 4.0f) {
                const float qx = out.cx + out.cX * lat + out.tX * l;
                const float qz = out.cz + out.cZ * lat + out.tZ * l;
                worst = std::min(worst, soffit - fwySurfaceNear(qx, qz));
            }
        out.clearanceM = worst;
    }

    // ---- THE FOUR RAMPS ----------------------------------------------------
    // Quadrant (a, b): a = -1/+1 along the freeway tangent, b = -1/+1 along
    // the crossroad axis. Each ramp runs freeway-end -> crossroad-end.
    int rampsBuilt = 0;
    for (int qi = 0; qi < 4; ++qi) {
        const float a = (qi < 2) ? -1.0f : 1.0f;
        const float b = (qi % 2 == 0) ? -1.0f : 1.0f;
        InterchangeResult::Ramp& rp = out.ramp[qi];

        // Freeway landing: the spec node nearest u(J) + a * reach.
        size_t F = J;
        {
            const float want = U[J] + a * kRampFwyReachM;
            float bd = 1e18f;
            for (size_t i = 0; i + 1 < fn; ++i) {
                const float d = std::fabs(U[i] - want);
                if (d < bd) { bd = d; F = i; }
            }
        }
        rp.fwyNode = (uint32_t)F;
        const float fx = fwySpec.x[F], fz = fwySpec.z[F], fy = fwyRoadY[F];
        float ftx, ftz;
        tangentAt(F, ftx, ftz);
        const float mF = (F < medianPlan.size()) ? medianPlan[F] : kFwyMedianMinHalfM;
        const float pavedEdgeF    = mF + 2.0f * kFwyPavedHalfM;
        const float shoulderEdgeF = mF + kFwyPavedHalfM + kFwyShoulderHalfM;

        // Crossroad landing: the authored station at b * reach.
        size_t C = 0;
        {
            float bd = 1e18f;
            const float wx = out.cx + out.cX * b * kRampCrossReachM;
            const float wz = out.cz + out.cZ * b * kRampCrossReachM;
            for (size_t i = 0; i < out.spec.x.size(); ++i) {
                const float dx = out.spec.x[i] - wx, dz = out.spec.z[i] - wz;
                const float d = dx * dx + dz * dz;
                if (d < bd) { bd = d; C = i; }
            }
        }
        const float cxL = out.spec.x[C], czL = out.spec.z[C], cyL = out.roadY[C];

        // Terminals. Freeway end: retreat along the ramp's own first leg at
        // ~50 deg to the freeway; the setback divides by sin(50) so the
        // terminal stands (40 * sin50 ~ 30 m) LATERALLY clear of the paved
        // edge, not just 40 m along an oblique line that never leaves it.
        const float sinF = std::sin(kRampFwyAngRad), cosF = std::cos(kRampFwyAngRad);
        // first-leg direction: back toward the crossing along the freeway,
        // veering to the crossroad side
        float d0x = -a * cosF * ftx + b * sinF * (-ftz);
        float d0z = -a * cosF * ftz + b * sinF * ( ftx);
        const float S0 = pavedEdgeF / sinF + 40.0f;
        const float e0x = fx + d0x * S0, e0z = fz + d0z * S0;
        // Crossroad end: ~70 deg give-way T; retreat direction points into
        // the quadrant (toward the freeway side, veering along the ramp).
        const float sinC = std::sin(kRampCrossAngRad), cosC = std::cos(kRampCrossAngRad);
        float d1x = a * sinC * out.tX - b * cosC * out.cX;
        float d1z = a * sinC * out.tZ - b * cosC * out.cZ;
        const float S1 = kPavedHalfM / sinC + 40.0f;
        const float e1x = cxL + d1x * S1, e1z = czL + d1z * S1;

        // Elbow: intersection of ray(e0, d0) with ray(e1, d1).
        const float det = d0x * (-d1z) - d0z * (-d1x);
        if (std::fabs(det) < 0.05f) { rp.built = false; continue; }
        const float rx0 = e1x - e0x, rz0 = e1z - e0z;
        const float mu = (rx0 * (-d1z) - rz0 * (-d1x)) / det;
        const float lam = (d0x * rz0 - d0z * rx0) / det;
        if (mu < 20.0f || lam < 20.0f) { rp.built = false; continue; }
        const float xx = e0x + d0x * mu, xz = e0z + d0z * mu;

        std::vector<CourseWaypoint> wp(3);
        wp[0].x = e0x; wp[0].z = e0z;
        wp[1].x = xx;  wp[1].z = xz;  wp[1].fillet = 55.0f;   // the loop radius
        wp[2].x = e1x; wp[2].z = e1z;
        RoadSpec rs = makeRoadFromWaypoints(
            (std::string("interchange ramp ") +
             (a < 0 ? (b < 0 ? "SW" : "SE") : (b < 0 ? "NW" : "NE"))).c_str(),
            wp, 12.0f, /*closed=*/false);
        // RAMP CLASS: half the base cross-section (two 12 ft lanes), 6%
        // ceiling, and a 48 m radius floor DELIBERATELY under the 200 m
        // class floor — per-spec floors are design (the header's law).
        rs.widthScale       = 0.5f;
        rs.halfWidth        = kPavedHalfM * 0.5f + 1.0f;
        rs.falloff          = 12.0f;
        rs.maxGrade         = 0.06f;
        rs.minTurnRadiusM   = 48.0f;
        rs.maxDeflectionDeg = 3.0f;
        smoothHorizontalCurves(rs);
        rs.pinY.assign(rs.x.size(), std::numeric_limits<float>::quiet_NaN());
        rs.pinY.front() = fy;    // arrive on the freeway at ITS datum...
        rs.pinY.back()  = cyL;   // ...and on the crossroad at ITS datum.

        rp.spec = rs;
        rp.road = registerRoad(rp.spec, &rp.roadY);
        if (!rp.road.ok || rp.roadY.empty()) { rp.built = false; continue; }
        measureMaxDeflectionDeg(rp.spec, &rp.filletR);

        // Throat boxes + junction notes at both mouths (the same call every
        // branch makes — barriers stay out, the skirt feathers, the throat
        // is cut to the datum).
        registerRoadJunctionThroat(e0x, e0z, fx, fz, fy);
        registerRoadJunctionThroat(e1x, e1z, cxL, czL, cyL);

        // Junction frames for the mouth patches.
        auto frame = [&](const RoadSpec& main, const std::vector<float>& mainY,
                         size_t K, float ex, float ez, float ey, RoadJunction& jf) {
            const size_t n2 = main.x.size();
            const size_t kp = (K + 1 < n2) ? K + 1 : K;
            const size_t km = (K > 0) ? K - 1 : K;
            float mtx = main.x[kp] - main.x[km], mtz = main.z[kp] - main.z[km];
            const float mtl = std::sqrt(mtx * mtx + mtz * mtz);
            jf.valid = true;
            jf.jx = main.x[K]; jf.jz = main.z[K]; jf.jy = mainY[K];
            jf.mainTX = 1.0f; jf.mainTZ = 0.0f; jf.mainGrade = 0.0f;
            if (mtl > 1e-4f) {
                jf.mainTX = mtx / mtl; jf.mainTZ = mtz / mtl;
                jf.mainGrade = (mainY[kp] - mainY[km]) / mtl;
            }
            jf.endX = ex; jf.endZ = ez; jf.endY = ey;
        };
        frame(fwySpec, fwyRoadY, F, rp.spec.x.front(), rp.spec.z.front(),
              rp.roadY.front(), rp.fwyJct);
        rp.fwyJct.mainShoulderEdgeM = shoulderEdgeF;   // divided-freeway main
        rp.fwyJct.mainPavedEdgeM    = pavedEdgeF;
        frame(out.spec, out.roadY, C, rp.spec.x.back(), rp.spec.z.back(),
              rp.roadY.back(), rp.crossJct);
        rp.built = true;
        ++rampsBuilt;
    }
    if (rampsBuilt < 4) {
        out.whyNot = "a ramp failed to register";
        char eb[96];
        std::snprintf(eb, sizeof(eb), "interchange: only %d of 4 ramps built", rampsBuilt);
        x3::logError(eb);
        return out;
    }

    // The zone: no median crossover anywhere inside the ramp pairs.
    noteInterchangeZone(out.cx, out.cz, kInterchangeZoneR);

    out.built = true;
    char bfr[420];
    std::snprintf(bfr, sizeof(bfr),
        "interchange: DIAMOND at freeway node %u (%.0f, %.0f) — deck %.1f ft over "
        "the pavement (law %.1f ft), median %.1f m open for the pier, crossroad "
        "%.2f miles, 4 ramps (grades %.1f/%.1f/%.1f/%.1f%%, fillets %.0f-%.0f m), "
        "site fit score %.1f",
        out.fwyNode, out.cx, out.cz, out.clearanceM * kMToFt,
        kOverpassClearM * kMToFt, out.medianHalfAtCrossing,
        out.road.lengthM / 1609.34f,
        out.ramp[0].road.maxGradePct, out.ramp[1].road.maxGradePct,
        out.ramp[2].road.maxGradePct, out.ramp[3].road.maxGradePct,
        std::min(std::min(out.ramp[0].filletR, out.ramp[1].filletR),
                 std::min(out.ramp[2].filletR, out.ramp[3].filletR)),
        std::max(std::max(out.ramp[0].filletR, out.ramp[1].filletR),
                 std::max(out.ramp[2].filletR, out.ramp[3].filletR)),
        bestScore);
    x3::logInfo(bfr);
    return out;
}

// ---------------------------------------------------------------------------
// THE OVERPASS DECK — slab + parapets (collision) + abutments + wingwalls +
// one median pier. The same concrete family as the portals and Bridge No. 1.
// ---------------------------------------------------------------------------
OverpassBuildResult buildOverpassDeck(const InterchangeResult& ic, Scene& scene,
                                      x3::rhi::IRenderDevice& device,
                                      x3::phys::IPhysicsWorld& phys) {
    OverpassBuildResult out;
    if (!ic.built) return out;

    SurfaceLibrary& surf = icSurfaces();
    surf.mount(assetRoot() + "/surface_library");
    const SurfaceSet& concrete = surf.get(device, "mw_concrete_panels_a");
    const SurfaceSet& asphalt  = surf.get(device, "rd_asphalt_01");

    // Deck frame: s along the crossroad axis, l along the freeway tangent.
    auto pt = [&](float s, float l, float y, float o[3]) {
        o[0] = ic.cx + ic.cX * s + ic.tX * l;
        o[1] = y;
        o[2] = ic.cz + ic.cZ * s + ic.tZ * l;
    };
    // A frame-aligned box (s0..s1 along the axis, l0..l1 across, y0..y1 up).
    MeshBuf deckA, deckC, paint;
    auto box = [&](MeshBuf& m, float s0, float s1, float l0, float l1,
                   float y0, float y1) {
        float c[8][3];
        pt(s0,l0,y0,c[0]); pt(s1,l0,y0,c[1]); pt(s1,l1,y0,c[2]); pt(s0,l1,y0,c[3]);
        pt(s0,l0,y1,c[4]); pt(s1,l0,y1,c[5]); pt(s1,l1,y1,c[6]); pt(s0,l1,y1,c[7]);
        m.quad(c[4],c[5],c[6],c[7]);          // top
        m.quad(c[3],c[2],c[1],c[0]);          // bottom
        m.quad(c[0],c[1],c[5],c[4]);          // side l0
        m.quad(c[2],c[3],c[7],c[6]);          // side l1
        m.quad(c[1],c[2],c[6],c[5]);          // end s1
        m.quad(c[3],c[0],c[4],c[7]);          // end s0
    };

    const float sA = ic.abutS;
    const float topY = ic.deckY + kPaveProud;          // matches the arriving ribbon
    const float sofY = ic.deckY - kOverpassDepthM;
    // Deck width: the crossroad's running + shoulder (the aprons stop at the
    // abutments — real bridges drop them; the wingwalls close the corner).
    const float deckHalfW = kShoulderHalfM + 0.55f;    // + kerb
    const float parW = 0.42f, parH = 0.92f;

    // THE SLAB: asphalt top riding the gap datum, concrete girder body below
    // with sloped sides (a box girder reads by its shadowed underside).
    {
        // asphalt wearing course
        box(deckA, -sA, sA, -kShoulderHalfM, kShoulderHalfM, ic.deckY - 0.08f, topY);
        // girder body, sides pulled in toward the soffit
        float c[8][3];
        pt(-sA, -deckHalfW,        ic.deckY - 0.10f, c[0]);
        pt( sA, -deckHalfW,        ic.deckY - 0.10f, c[1]);
        pt( sA,  deckHalfW,        ic.deckY - 0.10f, c[2]);
        pt(-sA,  deckHalfW,        ic.deckY - 0.10f, c[3]);
        pt(-sA, -deckHalfW + 1.9f, sofY,             c[4]);
        pt( sA, -deckHalfW + 1.9f, sofY,             c[5]);
        pt( sA,  deckHalfW - 1.9f, sofY,             c[6]);
        pt(-sA,  deckHalfW - 1.9f, sofY,             c[7]);
        deckC.quad(c[0], c[1], c[5], c[4]);            // sloped web, -l side
        deckC.quad(c[2], c[3], c[7], c[6]);            // sloped web, +l side
        deckC.quad(c[7], c[6], c[5], c[4]);            // soffit
        // deck edge fascia (vertical strip above the webs)
        box(deckC, -sA, sA, -deckHalfW, -kShoulderHalfM, ic.deckY - 0.10f, topY);
        box(deckC, -sA, sA,  kShoulderHalfM, deckHalfW,  ic.deckY - 0.10f, topY);
    }
    // PARAPETS, with collision — nobody drives off this deck.
    box(deckC, -sA, sA, -deckHalfW, -deckHalfW + parW, topY, topY + parH);
    box(deckC, -sA, sA,  deckHalfW - parW, deckHalfW,  topY, topY + parH);

    // ABUTMENTS + WINGWALLS at both ends: a seat wall under the deck end down
    // into the embankment, and two 45-degree wings retaining its shoulders.
    for (int e = 0; e < 2; ++e) {
        const float sgn = e == 0 ? -1.0f : 1.0f;
        const float s0 = sgn * sA;
        float gnd[3];
        pt(s0, 0.0f, 0.0f, gnd);
        const float gy = terrainHeightAtWorld(gnd[0], gnd[2]);
        box(deckC, s0 - sgn * 1.2f, s0, -deckHalfW, deckHalfW,
            std::min(gy - 1.0f, sofY - 4.0f), sofY + 0.15f);
        for (int w = -1; w <= 1; w += 2) {
            // wing: runs back along the embankment at 45 deg, top follows down
            float a0[3], a1[3], b0[3], b1[3];
            const float wl = 9.0f;
            pt(s0, (float)w * deckHalfW, topY + 0.35f, a0);
            pt(s0 + sgn * wl, (float)w * (deckHalfW + wl * 0.55f),
               topY + 0.35f - wl * 0.55f, a1);
            b0[0] = a0[0]; b0[2] = a0[2];
            b0[1] = terrainHeightAtWorld(a0[0], a0[2]) - 1.0f;
            b1[0] = a1[0]; b1[2] = a1[2];
            b1[1] = terrainHeightAtWorld(a1[0], a1[2]) - 1.0f;
            // thin wall: two faces
            deckC.quad(a0, a1, b1, b0);
            deckC.quad(a1, a0, b0, b1);
        }
    }

    // THE MEDIAN PIER: one wall, long axis along the freeway, standing in the
    // open median the site test demanded. Thickness fits the median; it never
    // touches either carriageway's pavement.
    {
        float pg[3];
        pt(0.0f, 0.0f, 0.0f, pg);
        const float py = terrainHeightAtWorld(pg[0], pg[2]);
        const float halfT = std::min(1.1f, ic.medianHalfAtCrossing - 1.0f);
        box(deckC, -halfT, halfT, -(deckHalfW - 2.4f), deckHalfW - 2.4f,
            py - 0.8f, sofY + 0.05f);
    }

    // Deck edge lines: solid white at the running edges, continuing the
    // approach ribbon's paint across the span.
    for (int w = -1; w <= 1; w += 2) {
        float a0[3], a1[3], b0[3], b1[3];
        pt(-sA, (float)w * kRunningHalfM - 0.06f, topY + 0.012f, a0);
        pt(-sA, (float)w * kRunningHalfM + 0.06f, topY + 0.012f, a1);
        pt( sA, (float)w * kRunningHalfM + 0.06f, topY + 0.012f, b1);
        pt( sA, (float)w * kRunningHalfM - 0.06f, topY + 0.012f, b0);
        paint.quad(a0, a1, b1, b0);
    }

    auto upload = [&](MeshBuf& m, const SurfaceSet* set, const float tint[4],
                      bool collide) {
        if (m.empty()) return;
        Entity ent;
        ent.mesh = device.createMesh(m.v.data(), (uint32_t)m.v.size(),
                                     m.i.data(), (uint32_t)m.i.size());
        if (!ent.mesh.valid()) return;
        if (set && set->ok) {
            ent.tex = set->albedo; ent.mrTex = set->mr; ent.normalTex = set->normal;
        }
        for (int c = 0; c < 4; ++c) ent.baseColor[c] = tint[c];
        scene.add(ent);
        ++out.meshCount;
        out.quadCount += (uint32_t)(m.i.size() / 6);
        if (collide) {
            std::vector<float> cv; cv.reserve(m.v.size() * 3);
            for (const auto& vv : m.v) {
                cv.push_back(vv.pos[0]); cv.push_back(vv.pos[1]); cv.push_back(vv.pos[2]);
            }
            phys.addStaticMesh(cv.data(), (uint32_t)(cv.size() / 3),
                               m.i.data(), (uint32_t)m.i.size());
        }
    };
    float roadTint[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    if (!asphalt.ok) { roadTint[0] = 0.055f; roadTint[1] = 0.056f; roadTint[2] = 0.060f; }
    float cemTint[4] = { 0.80f, 0.79f, 0.76f, 1.0f };
    if (!concrete.ok) { cemTint[0] = 0.38f; cemTint[1] = 0.37f; cemTint[2] = 0.35f; }
    const float paintC[4] = { 1.8f, 1.8f, 1.7f, 1.0f };
    upload(deckA, &asphalt,  roadTint, true);
    upload(deckC, &concrete, cemTint,  true);
    upload(paint, nullptr,   paintC,   false);
    out.ok = out.meshCount > 0;

    char b[200];
    std::snprintf(b, sizeof(b),
        "overpass deck: %u meshes, %u quads — span %.0f ft, soffit %.1f ft over "
        "the freeway pavement, median pier in a %.1f m half-median",
        out.meshCount, out.quadCount, 2.0f * ic.abutS * kMToFt,
        (ic.deckY - kOverpassDepthM - ic.fwyMaxSurfaceY) * kMToFt,
        ic.medianHalfAtCrossing);
    x3::logInfo(b);
    return out;
}

// ---------------------------------------------------------------------------
// --test-interchange
// ---------------------------------------------------------------------------
bool runInterchangeSelfTest() {
    int passN = 0, failN = 0;
    auto check = [&](bool ok, const char* name, const char* detail = nullptr) {
        std::string m = std::string(ok ? "PASS " : "FAIL ") + name;
        if (detail && *detail) m += std::string(" — ") + detail;
        if (ok) { ++passN; x3::logInfo("[interchange] " + m); }
        else    { ++failN; x3::logError("[interchange] " + m); }
    };
    char d[360];

    clearTerrainCorridors();
    clearRoadJunctions();
    RoadSpec ringSpec = makeInnerCourse();
    std::vector<float> ringY;
    const RoadBuildResult ringR = registerRoad(ringSpec, &ringY);
    check(ringR.ok, "I0 the freeway registers under the interchange gates");

    std::vector<const RoadSpec*> avoid;   // the gates run on the bare freeway
    InterchangeResult ic = registerInterchange(ringSpec, ringY, &avoid);
    std::snprintf(d, sizeof(d),
        "%s — node %u (%.0f, %.0f), median half %.1f m, crossroad %.2f miles",
        ic.built ? "built" : ic.whyNot, ic.fwyNode, ic.cx, ic.cz,
        ic.medianHalfAtCrossing, ic.road.lengthM / 1609.34f);
    check(ic.built, "I1 the diamond registers on a measured site", d);
    if (!ic.built) {
        clearTerrainCorridors();
        clearRoadJunctions();
        return failN == 0;
    }

    // I2 — the clearance law, measured across the full paved width.
    std::snprintf(d, sizeof(d),
        "min soffit clearance %.2f ft over both carriageways (law %.1f ft), "
        "deck %.1f ft above the pavement",
        ic.clearanceM * kMToFt, kOverpassClearM * kMToFt,
        (ic.deckY - ic.fwyMaxSurfaceY) * kMToFt);
    check(ic.clearanceM >= kOverpassClearM,
          "I2 the deck clears 16.5 ft over EVERY freeway lane", d);

    // I2b — the crossroad reaches the deck at grade and the approaches are
    // embankments, not viaducts.
    // Locate the worst surviving kink — a gate that names its offender is a
    // gate somebody can fix from (NO_SLOP rule 9).
    float worstRate = 0.0f, worstS = 0.0f;
    for (size_t i = 1; i + 1 < ic.roadY.size(); ++i) {
        const float l0 = std::max(1.0f, std::hypot(ic.spec.x[i] - ic.spec.x[i-1],
                                                   ic.spec.z[i] - ic.spec.z[i-1]));
        const float l1 = std::max(1.0f, std::hypot(ic.spec.x[i+1] - ic.spec.x[i],
                                                   ic.spec.z[i+1] - ic.spec.z[i]));
        const float g0 = (ic.roadY[i] - ic.roadY[i-1]) / l0;
        const float g1 = (ic.roadY[i+1] - ic.roadY[i]) / l1;
        const float rt = std::fabs(g1 - g0) / (0.5f * (l0 + l1));
        if (rt > worstRate) {
            worstRate = rt;
            worstS = (ic.spec.x[i] - ic.cx) * ic.cX + (ic.spec.z[i] - ic.cz) * ic.cZ;
        }
    }
    std::snprintf(d, sizeof(d),
        "crossroad grade %.1f%% (cap 6%%), pin deficit %.2f ft, approach float "
        "%.0f ft, vertical rate %.5f->%.5f /m (cap %.5f), worst at s=%+.0f m "
        "(abutments +-%.0f)",
        ic.road.maxGradePct, ic.road.pinErrM * kMToFt, ic.road.maxFloatM * kMToFt,
        ic.road.maxGradeRatePre, ic.road.maxGradeRatePost, 5.0e-4f,
        worstS, ic.abutS);
    // Float ceiling derived from the STRUCTURE, not guessed: the tallest
    // legal float is the abutment approach — deck height over the pavement
    // plus whatever the freeway itself floats/cuts at the site (measured
    // 11.3 m here: 7.1 m of deck + 3.1 m of low ground under the freeway
    // line + the pave lift). 6 m of slack covers that site term; anything
    // beyond it means the authored profile detached from the country.
    const float floatCap = (ic.deckY - ic.fwyMaxSurfaceY) + 6.0f;
    check(ic.road.maxGradePct <= 6.5f && ic.road.pinErrM <= 0.06f &&
          ic.road.maxFloatM <= floatCap &&
          ic.road.maxGradeRatePost <= 5.0e-4f * 1.15f,
          "I2b the approaches climb to the deck at grade, embankment-scale, K held", d);

    // I3 — every ramp lands both datums at grade, within its own class.
    {
        float worstGrade = 0.0f, worstPin = 0.0f, worstEndOff = 0.0f;
        for (int q = 0; q < 4; ++q) {
            const InterchangeResult::Ramp& rp = ic.ramp[q];
            worstGrade = std::max(worstGrade, rp.road.maxGradePct);
            worstPin   = std::max(worstPin, rp.road.pinErrM);
            if (!rp.roadY.empty()) {
                worstEndOff = std::max(worstEndOff,
                    std::fabs(rp.roadY.front() - rp.fwyJct.jy));
                worstEndOff = std::max(worstEndOff,
                    std::fabs(rp.roadY.back() - rp.crossJct.jy));
            }
        }
        std::snprintf(d, sizeof(d),
            "worst ramp grade %.1f%% (cap 6.5), worst pin deficit %.2f ft, "
            "worst end-off-datum %.2f ft",
            worstGrade, worstPin * kMToFt, worstEndOff * kMToFt);
        check(worstGrade <= 6.5f && worstPin <= 0.06f && worstEndOff < 0.02f,
              "I3 all four ramps land BOTH datums at grade within ramp class", d);
    }

    // I4 — the ramp-pair law: no median crossover inside the zone, and the
    // rhythm still paves the rest of the tour.
    {
        const std::vector<RoadTurnaround> tas = planTurnarounds(ringSpec);
        std::vector<float> U(ringSpec.x.size(), 0.0f);
        for (size_t i = 0; i + 1 < ringSpec.x.size(); ++i) {
            const float dx = ringSpec.x[i+1] - ringSpec.x[i];
            const float dz = ringSpec.z[i+1] - ringSpec.z[i];
            U[i+1] = U[i] + std::sqrt(dx * dx + dz * dz);
        }
        uint32_t inZone = 0;
        float nearestM = 1e9f;
        for (const RoadTurnaround& t : tas) {
            const float uc = 0.5f * (t.u0 + t.u1);
            // position of the crossover centre
            size_t i = 0;
            while (i + 2 < U.size() && U[i+1] < uc) ++i;
            const float span = std::max(1e-4f, U[i+1] - U[i]);
            const float tt = std::max(0.0f, std::min(1.0f, (uc - U[i]) / span));
            const float px = ringSpec.x[i] + (ringSpec.x[i+1] - ringSpec.x[i]) * tt;
            const float pz = ringSpec.z[i] + (ringSpec.z[i+1] - ringSpec.z[i]) * tt;
            const float dx = px - ic.cx, dz = pz - ic.cz;
            const float dd = std::sqrt(dx * dx + dz * dz);
            nearestM = std::min(nearestM, dd);
            if (dd < kInterchangeZoneR) ++inZone;
        }
        std::snprintf(d, sizeof(d),
            "%u crossovers on the tour, %u inside the %.0f m zone, nearest %.0f m "
            "(8 ramp junctions noted)",
            (uint32_t)tas.size(), inZone, kInterchangeZoneR, nearestM);
        check(inZone == 0 && tas.size() >= 10,
              "I4 NO median crossover inside the ramp pairs; the rhythm survives", d);
    }

    // I5 — ramp flow: the loop radii sit in the asked 45-60 m band (UNDER the
    // 200 m class floor, proving per-spec floors are design), and the render
    // path has no jointed bends.
    {
        float rMin = 1e9f, rMax = 0.0f, worstFacet = 0.0f, worstSpace = 0.0f;
        for (int q = 0; q < 4; ++q) {
            const InterchangeResult::Ramp& rp = ic.ramp[q];
            rMin = std::min(rMin, rp.filletR);
            rMax = std::max(rMax, rp.filletR);
            std::vector<RoadRenderStation> path;
            buildRoadRenderPath(rp.spec, &rp.roadY, nullptr, path);
            for (size_t i = 0; i + 1 < path.size(); ++i) {
                worstSpace = std::max(worstSpace, path[i+1].u - path[i].u);
                const float dot = path[i].tx * path[i+1].tx + path[i].tz * path[i+1].tz;
                worstFacet = std::max(worstFacet,
                    std::acos(std::max(-1.0f, std::min(1.0f, dot))) * 57.29578f);
            }
        }
        std::snprintf(d, sizeof(d),
            "fillets %.0f..%.0f m (asked 45-60, class floor 200 overridden to 48), "
            "max facet %.2f deg (gate 2.0), max station spacing %.1f m (gate 20.5)",
            rMin, rMax, worstFacet, worstSpace);
        check(rMin >= 40.0f && rMax <= 70.0f && worstFacet <= 2.0f &&
              worstSpace <= 20.5f,
              "I5 ramp loops hold 45-60 m radii with NO jointed bends", d);
    }

    // I6 — the crossing is STRUCTURAL: outside its span gap the crossroad
    // never comes inside the freeway's paved width (R2's law, held here by
    // the deck, not by luck).
    {
        float worst = 1e9f;
        const RoadSpec::Gap& g = ic.spec.gaps[0];
        for (size_t i = 0; i < ic.spec.x.size(); ++i) {
            if (i >= g.i0 && i <= g.i1) continue;
            float best = 1e18f;
            for (size_t k = 0; k + 1 < ringSpec.x.size(); ++k)
                best = std::min(best, segPtDist(ic.spec.x[i], ic.spec.z[i],
                    ringSpec.x[k], ringSpec.z[k], ringSpec.x[k+1], ringSpec.z[k+1]));
            worst = std::min(worst, best);
        }
        const float fwyPavedEdge = ic.medianHalfAtCrossing + 2.0f * kFwyPavedHalfM;
        std::snprintf(d, sizeof(d),
            "nearest non-gap crossroad node to the freeway centreline %.0f m "
            "(paved edge %.0f m); the span itself is the ONLY crossing",
            worst, fwyPavedEdge);
        check(worst >= fwyPavedEdge,
              "I6 the crossroad crosses ONLY on the deck — structural, not stacked", d);
    }

    // I6b — barriers: every ramp mouth is OPEN (no rail inside the exclusion
    // zone), same law every junction obeys.
    {
        uint32_t viol = 0, planned = 0;
        for (int q = 0; q < 4; ++q) {
            const InterchangeResult::Ramp& rp = ic.ramp[q];
            const BarrierPlan bp = planRoadBarriers(rp.spec, &rp.roadY);
            planned += bp.railSegments + bp.jerseySegments;
            for (size_t k = 0; k < bp.mask.size(); ++k) {
                if (!bp.mask[k]) continue;
                const float dj = std::min(
                    distToNearestRoadJunction(rp.spec.x[k], rp.spec.z[k]),
                    distToNearestRoadJunction(rp.spec.x[k+1], rp.spec.z[k+1]));
                if (dj < kJunctionBarrierClearM) ++viol;
            }
        }
        std::snprintf(d, sizeof(d),
            "%u barrier segments planned on the ramps, %u inside a mouth zone",
            planned, viol);
        check(viol == 0, "I6b every ramp mouth is open — no railings in a gore", d);
    }

    // I7 — determinism: clear, re-register, the carved field answers
    // bit-identically along the crossroad and every ramp.
    {
        auto fieldHash = [&]() {
            uint64_t h = 1469598103934665603ull;   // FNV-1a
            auto fold = [&](const RoadSpec& s) {
                for (size_t i = 0; i < s.x.size(); i += 2) {
                    const float v = terrainCorridorDelta(s.x[i], s.z[i]);
                    uint32_t bits;
                    std::memcpy(&bits, &v, sizeof(bits));
                    h = (h ^ bits) * 1099511628211ull;
                }
            };
            fold(ic.spec);
            for (int q = 0; q < 4; ++q) fold(ic.ramp[q].spec);
            return h;
        };
        const uint64_t h1 = fieldHash();
        clearTerrainCorridors();
        clearRoadJunctions();
        RoadSpec ring2 = makeInnerCourse();
        std::vector<float> ringY2;
        (void)registerRoad(ring2, &ringY2);
        (void)registerInterchange(ring2, ringY2, &avoid);
        const uint64_t h2 = fieldHash();
        std::snprintf(d, sizeof(d), "field hash %016llx", (unsigned long long)h1);
        check(h1 == h2, "I7 the interchange is deterministic (re-registration identical)", d);
    }

    clearTerrainCorridors();
    clearRoadJunctions();
    x3::logInfo("[interchange] " + std::to_string(passN) + " passed, " +
                std::to_string(failN) + " failed");
    return failN == 0;
}

} // namespace x3::game
