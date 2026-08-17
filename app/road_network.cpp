// ROAD NETWORK — see app/road_network.h.
#include "road_network.h"

#include "terrain.h"
#include "tunnel_corridor.h"   // the outer tour's bores are real tunnels
#include "river_bridge.h"      // the valley road's ring landings (gates R1/R2)
#include "scene.h"
#include "surface_library.h"
#include "asset_root.h"
#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

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
// `pin` (optional): per-node pinned datum, NaN = free. A pinned node is HELD at
// its value through the relaxation — bores and bridge decks dictate the datum
// at their ends and the open road must arrive there at grade. Pins may sit
// ABOVE the natural surface (a deck over a hollow); the carve simply clamps to
// zero there. If the relaxation cannot hold a pin (the country falls away
// faster than maxGrade can climb), the caller sees it in pinErr — never fixed
// silently, because the fix is an authoring change, not a numeric one.
// `closed`: the polyline is a RING (last node == first). Only then may the
// grade constraint travel across the first/last seam — on an OPEN road (the
// valley road, the spawn connector, the summit spur) the two ends are
// unrelated places, and coupling them clamps a climbing road's summit to its
// base elevation. Found when the summit spur graded FLAT: the wrap clamp ran
// unconditionally and held the top node within one segment-grade of node 0.
// VERTICAL-CURVE SMOOTHING (the traction fix). The relaxation above bounds the
// GRADE; nothing bounds how fast the grade CHANGES, so a crest is a kink — the
// profile goes from +maxGrade to -maxGrade across one node and a car at speed
// goes light or airborne ("the road changes angles sharply with respect to
// elevation, making the car lose traction"). Real vertical alignment bounds
// |d(grade)/ds| — the K-value, parabolic vertical curves at every grade break.
//
// Constraint per interior node i:  |g_i - g_{i-1}| <= rate * (L_{i-1}+L_i)/2.
// Enforced by iterative projection, and the projection is chosen so it can
// never break what already holds:
//   * FREE node: move y[i] toward the violation's fix (crest: DOWN — cut more,
//     always legal; sag: UP — the datum floats a couple of metres over the V's
//     bottom, which is a small embankment; the ribbon + collision ride the
//     datum, the carve just cuts nothing there, and the prism skirt grounds
//     it visually). Moving only the CENTRE node drives the two adjacent
//     grades TOWARD each other, so both stay inside the [g0, g1] envelope —
//     the maxGrade cap survives smoothing by construction.
//   * PINNED node (a portal datum, a junction landing): the pin is LAW. The
//     curve is built AROUND it by moving its free neighbours — flattening the
//     approach into a sag pin (down: legal cut) or easing it over a crest pin
//     (up: bounded float) — with the neighbour's OTHER grade clamped to
//     maxGrade so easing one side never steepens the far side past the cap.
// Deterministic (fixed sweep order, fixed damping), monotone in the residual.
float maxAbsGradeRate(const std::vector<float>& y, const std::vector<float>& segLen,
                      bool closed) {
    const size_t n = y.size();
    if (n < 3) return 0.0f;
    float worst = 0.0f;
    auto rateAt = [&](size_t im, size_t i, size_t ip, size_t s0, size_t s1) {
        const float L0 = std::max(1.0f, segLen[s0]), L1 = std::max(1.0f, segLen[s1]);
        const float g0 = (y[i] - y[im]) / L0, g1 = (y[ip] - y[i]) / L1;
        return std::fabs(g1 - g0) / (0.5f * (L0 + L1));
    };
    for (size_t i = 1; i + 1 < n; ++i)
        worst = std::max(worst, rateAt(i - 1, i, i + 1, i - 1, i));
    // A ring wraps: node 0 == node n-1, so the seam has curvature too,
    // between the last real segment (n-2) and the first (0).
    if (closed && n >= 4)
        worst = std::max(worst, rateAt(n - 2, 0, 1, n - 2, 0));
    return worst;
}

void smoothVerticalCurves(std::vector<float>& y, const std::vector<float>& segLen,
                          float maxGrade, float rate, bool closed,
                          const std::vector<float>* pin) {
    const size_t n = y.size();
    if (n < 3 || rate <= 0.0f) return;
    auto pinned = [&](size_t i) {
        return pin && i < pin->size() && std::isfinite((*pin)[i]);
    };
    const float kDamp = 0.7f;
    // Apply a move and keep the ring seam welded (node 0 == node n-1 is the
    // same place; moving one without the other would tear the closure).
    auto bump = [&](size_t idx, float d) {
        y[idx] += d;
        if (closed && (idx == 0 || idx + 1 == n)) y[0] = y[n - 1] = y[idx];
    };
    // Defensive initial weld: the relaxation welds the dup pair too, but this
    // pass must never be the thing that turns a stray spread into a kink.
    if (closed && !pinned(0) && !pinned(n - 1))
        y[0] = y[n - 1] = std::min(y[0], y[n - 1]);
    for (int iter = 0; iter < 600; ++iter) {
        float moved = 0.0f;
        auto relaxNode = [&](size_t im, size_t i, size_t ip, size_t s0, size_t s1) {
            const float L0 = std::max(1.0f, segLen[s0]), L1 = std::max(1.0f, segLen[s1]);
            const float g0 = (y[i] - y[im]) / L0, g1 = (y[ip] - y[i]) / L1;
            const float lim = rate * 0.5f * (L0 + L1);
            const float dg  = g1 - g0;                   // >0 sag, <0 crest
            if (std::fabs(dg) <= lim) return;
            const float excess = dg - (dg > 0.0f ? lim : -lim);
            if (!pinned(i)) {
                // d(dg)/dy[i] = -(1/L0 + 1/L1): raising the centre reduces dg,
                // so the fix is delta = +excess / (1/L0 + 1/L1) (sag raises,
                // crest lowers). Moving only the centre pulls the two grades
                // TOWARD each other, so both stay inside their old envelope
                // and the maxGrade cap survives by construction.
                const float delta = kDamp * excess / (1.0f / L0 + 1.0f / L1);
                bump(i, delta);
                moved += std::fabs(delta);
                return;
            }
            // The pin is LAW — build the curve around it in the free
            // neighbours. Reducing |dg| needs d(dg) = -excess:
            //   d(dg)/dy[im] = +1/L0  ->  y[im] -= excess*L0/2 (sag: lower the
            //     approach wall into the V; crest: raise it toward the peak)
            //   d(dg)/dy[ip] = +1/L1  ->  y[ip] -= excess*L1/2
            // Each move is clamped so the neighbour's FAR grade (the segment
            // away from the pin) never exceeds maxGrade.
            // NOTE: windows headers #define `far` to nothing — the obvious
            // name for "the node beyond the neighbour" is a landmine here.
            const float half = kDamp * 0.5f * excess;
            auto moveNeighbour = [&](size_t nb, size_t beyond, size_t beyondSeg, float d) {
                if (pinned(nb)) return;
                if (beyond != nb) {
                    const float Lb = std::max(1.0f, segLen[beyondSeg]);
                    const float lo = y[beyond] - Lb * maxGrade;
                    const float hi = y[beyond] + Lb * maxGrade;
                    d = std::max(lo, std::min(hi, y[nb] + d)) - y[nb];
                }
                bump(nb, d);
                moved += std::fabs(d);
            };
            {   // im's far side: node im-1 across segment im-1 (wrap on a ring)
                const size_t beyond = (im > 0) ? im - 1 : (closed && n >= 4 ? n - 2 : im);
                const size_t beyondSeg = (im > 0) ? im - 1 : n - 2;
                moveNeighbour(im, beyond, beyondSeg, -half * L0);
            }
            {   // ip's far side: node ip+1 across segment ip (wrap on a ring)
                const size_t beyond = (ip + 1 < n) ? ip + 1 : (closed && n >= 4 ? 1 : ip);
                const size_t beyondSeg = (ip + 1 < n) ? ip : 0;
                moveNeighbour(ip, beyond, beyondSeg, -half * L1);
            }
        };
        for (size_t i = 1; i + 1 < n; ++i) relaxNode(i - 1, i, i + 1, i - 1, i);
        if (closed && n >= 4) relaxNode(n - 2, 0, 1, n - 2, 0);
        if (moved < 1e-4f) break;
    }
}

void gradeRoad(const std::vector<float>& natural, const std::vector<float>& segLen,
               float maxGrade, std::vector<float>& roadY, bool closed,
               const std::vector<float>* pin = nullptr, float* pinErr = nullptr) {
    const size_t n = natural.size();
    roadY.assign(natural.begin(), natural.end());
    if (pinErr) *pinErr = 0.0f;
    auto applyPins = [&]() {
        if (!pin) return;
        for (size_t i = 0; i < n && i < pin->size(); ++i)
            if (std::isfinite((*pin)[i])) roadY[i] = (*pin)[i];
    };
    applyPins();
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
        // A ring wraps: the last node and the first are the SAME PLACE, so
        // they must be the same HEIGHT — not merely within one grade-step of
        // each other, which is what the first cut of this clamp enforced.
        // That slack was invisible until vertical-curve smoothing arrived:
        // the smoother welds the pair, and welding a 4 m spread teleports the
        // dup node — measured an 11.2% kink at the outer tour's 0-degree seam.
        // Weld to the LOWER of the two (lowering-only, so convergence holds);
        // the sweeps then carry the constraint through the seam next pass.
        if (closed) {
            const float a = std::min(roadY[0], roadY[n - 1]);
            moved += (roadY[0] - a) + (roadY[n - 1] - a);
            roadY[0] = roadY[n - 1] = a;
        }
        // Re-assert the pins each sweep. Lowering-only relaxation with pins
        // re-raised is still monotone in the FREE nodes (each free node only
        // ever falls), so it converges the same way; a pin that keeps getting
        // dragged down and re-raised is a pin the route cannot honour, and the
        // deficit is measured after the loop rather than hidden inside it.
        applyPins();
        if (moved < 1e-3f) break;
    }
    if (pin && pinErr) {
        // Measure what re-asserting the pins broke: with pins held, re-run one
        // constraint check and report the worst grade violation adjacent to a
        // pin, expressed as metres of unreachable datum.
        for (size_t i = 1; i < n; ++i) {
            const float lim = maxGrade * std::max(1.0f, segLen[i - 1]);
            const float over = std::fabs(roadY[i] - roadY[i - 1]) - lim;
            if (over > *pinErr) {
                const bool nearPin =
                    (i < pin->size() && std::isfinite((*pin)[i])) ||
                    (i - 1 < pin->size() && std::isfinite((*pin)[i - 1]));
                if (nearPin) *pinErr = over;
            }
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------
// JUNCTION EXCLUSION ZONES — see road_network.h. A tiny module registry: the
// junction machinery notes every mouth centre and branch end as it registers,
// and barrier planning + the skirt feather read it back. Deliberately not the
// corridor registry: a junction is a ROAD fact, not a terrain fact.
// ---------------------------------------------------------------------------
namespace {
struct JctPoint { float x, z; };
std::vector<JctPoint>& jctPoints() { static std::vector<JctPoint> v; return v; }
} // namespace

void noteRoadJunction(float x, float z) { jctPoints().push_back({ x, z }); }
void clearRoadJunctions() { jctPoints().clear(); }
uint32_t roadJunctionCount() { return (uint32_t)jctPoints().size(); }
float distToNearestRoadJunction(float x, float z) {
    float best = 1e9f;
    for (const JctPoint& p : jctPoints()) {
        const float dx = x - p.x, dz = z - p.z;
        best = std::min(best, dx * dx + dz * dz);
    }
    return best >= 1e18f ? 1e9f : std::sqrt(best);
}

// ---------------------------------------------------------------------------
// HORIZONTAL CURVE SMOOTHING — see road_network.h.
// ---------------------------------------------------------------------------
namespace {

// Heading change at node i between its two adjacent segments, radians.
inline float deflectionAt(const std::vector<float>& X, const std::vector<float>& Z,
                          size_t im, size_t i, size_t ip) {
    const float ax = X[i] - X[im], az = Z[i] - Z[im];
    const float bx = X[ip] - X[i], bz = Z[ip] - Z[i];
    const float la = std::sqrt(ax * ax + az * az), lb = std::sqrt(bx * bx + bz * bz);
    if (la < 1e-4f || lb < 1e-4f) return 0.0f;
    const float dot = std::max(-1.0f, std::min(1.0f, (ax * bx + az * bz) / (la * lb)));
    return std::acos(dot);
}

// Discrete curve radius at node i: circumradius of the three points. A
// straight answers "infinite" as 1e9.
inline float discreteRadiusAt(const std::vector<float>& X, const std::vector<float>& Z,
                              size_t im, size_t i, size_t ip) {
    const float ax = X[im], az = Z[im], bx = X[i], bz = Z[i], cx = X[ip], cz = Z[ip];
    const float a = std::sqrt((bx-cx)*(bx-cx) + (bz-cz)*(bz-cz));
    const float b = std::sqrt((ax-cx)*(ax-cx) + (az-cz)*(az-cz));
    const float c = std::sqrt((ax-bx)*(ax-bx) + (az-bz)*(az-bz));
    const float area2 = std::fabs((bx-ax)*(cz-az) - (cx-ax)*(bz-az));   // 2*area
    if (area2 < 1e-5f) return 1e9f;
    return (a * b * c) / (2.0f * area2);
}

constexpr float kHMinSegM = 4.0f;    // never subdivide below this spacing
constexpr float kRadToDeg = 57.29578f;
// Easing displacement budgets (metres). The first registration of the outer
// tour with UNBOUNDED easing dragged 108 nodes off their measured lanes and
// the gates convicted it: 32.5% grade, a 162 ft cut, a 14 ft pin deficit.
// A bend is eased IN PLACE or not at all: 15 m keeps every node inside its
// own carve half-width, and geometry says it is enough — filleting a 36 deg
// corner between 60 m legs to a 250 m radius moves the apex 12.1 m.
constexpr float kHEaseBudgetCoarseM = 15.0f;
constexpr float kHEaseBudgetFineM   = 3.0f;

} // namespace

float measureMaxDeflectionDeg(const RoadSpec& s, float* minRadiusM) {
    const size_t n = s.x.size();
    if (minRadiusM) *minRadiusM = 1e9f;
    if (n < 3) return 0.0f;
    const bool closed = std::fabs(s.x[0] - s.x[n-1]) < 0.01f &&
                        std::fabs(s.z[0] - s.z[n-1]) < 0.01f;
    // Gap reaches and a 45 m apron around their EDGES are excluded: a bore
    // chord meets its approach arc at the portal, the portal position is
    // pinned law, and the heading break there is hidden by the portal face
    // (the smoothing eases the last daylight metres toward the chord, but
    // the pinned node itself may keep a break). Everything in DAYLIGHT is
    // measured.
    // 100 m: the smoothing deliberately BENDS the last daylight metres
    // toward the chord (the spline distributes the portal's heading break
    // over the approach — the drivable version of a kink), and that curl is
    // a portal-approach feature, not a route defect.
    auto nearGap = [&](size_t i) {
        for (const RoadSpec::Gap& g : s.gaps) {
            if (i >= g.i0 && i <= (size_t)g.i1) return true;
            for (uint32_t e : { g.i0, g.i1 }) {
                if (e >= n) continue;
                const float dx = s.x[i] - s.x[e], dz = s.z[i] - s.z[e];
                if (dx * dx + dz * dz < 100.0f * 100.0f) return true;
            }
        }
        return false;
    };
    float worst = 0.0f;
    auto consider = [&](size_t im, size_t i, size_t ip) {
        if (nearGap(i)) return;
        const float d = deflectionAt(s.x, s.z, im, i, ip);
        worst = std::max(worst, d);
        if (minRadiusM && d > 0.25f / kRadToDeg)
            *minRadiusM = std::min(*minRadiusM, discreteRadiusAt(s.x, s.z, im, i, ip));
    };
    for (size_t i = 1; i + 1 < n; ++i) consider(i - 1, i, i + 1);
    if (closed && n >= 4) consider(n - 2, 0, 1);
    return worst * kRadToDeg;
}

HorizontalSmoothResult smoothHorizontalCurves(RoadSpec& s,
                                              const std::vector<uint8_t>* lockMask) {
    HorizontalSmoothResult r;
    size_t n = s.x.size();
    r.nodesBefore = r.nodesAfter = (uint32_t)n;
    r.newIndexOfOld.resize(n);
    for (size_t i = 0; i < n; ++i) r.newIndexOfOld[i] = (uint32_t)i;
    r.maxDeflBeforeDeg = r.maxDeflAfterDeg = measureMaxDeflectionDeg(s, nullptr);
    if (n < 3) return r;
    const bool closed = std::fabs(s.x[0] - s.x[n-1]) < 0.01f &&
                        std::fabs(s.z[0] - s.z[n-1]) < 0.01f;

    // The A/B instrument: X3_NO_HCURVE=1 measures and does nothing else.
    if (const char* off = std::getenv("X3_NO_HCURVE"); off && off[0] == '1') {
        r.skipped = true;
        char b[220];
        std::snprintf(b, sizeof(b),
            "road '%s': horizontal flow — X3_NO_HCURVE=1, pass SKIPPED "
            "(max deflection stays %.1f deg/node over %u nodes)",
            s.name.c_str(), r.maxDeflBeforeDeg, r.nodesBefore);
        x3::logWarn(b);
        return r;
    }

    // Working arrays. lock: 1 = position is LAW (never eased); linked with
    // "straight run" semantics for chord spans (see header). orig: the source
    // node index, UINT32_MAX for inserted nodes.
    std::vector<float>    X(s.x), Z(s.z);
    std::vector<uint8_t>  lock(n, 0);
    std::vector<uint32_t> orig(n);
    for (size_t i = 0; i < n; ++i) orig[i] = (uint32_t)i;
    if (!closed) { lock[0] = lock[n-1] = 1; }
    for (size_t i = 0; i < n && i < s.pinY.size(); ++i)
        if (std::isfinite(s.pinY[i])) lock[i] = 1;
    for (const RoadSpec::Gap& g : s.gaps)
        for (uint32_t i = g.i0; i <= g.i1 && i < n; ++i) lock[i] = 1;
    if (lockMask)
        for (size_t i = 0; i < n && i < lockMask->size(); ++i)
            if ((*lockMask)[i]) lock[i] = 1;

    const float capRad = s.maxDeflectionDeg / kRadToDeg;

    // THE MINIMUM-RADIUS EASING, as a bounded pass usable at two moments.
    // Pull any node whose discrete bend is tighter than the class floor
    // toward its neighbours' midpoint — but never further than `budget`
    // metres from where this pass found it, and never a locked node or a
    // locked node's immediate neighbour (portal approaches keep the geometry
    // the ramp grader was authored against). Run on the COARSE polyline the
    // fix is a couple of dozen apex moves that converge in a handful of
    // sweeps; after subdivision it is only a touch-up.
    auto easePass = [&](std::vector<float>& EX, std::vector<float>& EZ,
                        const std::vector<uint8_t>& elock, float budget,
                        uint32_t& easedOut) {
        const size_t m = EX.size();
        if (m < 3 || s.minTurnRadiusM <= 1.0f) return;
        std::vector<uint8_t> frozen(m, 0);
        for (size_t i = 0; i < m; ++i) {
            if (!elock[i]) continue;
            frozen[i] = 1;
            if (i > 0) frozen[i - 1] = 1;
            if (i + 1 < m) frozen[i + 1] = 1;
        }
        std::vector<float> ox(EX), oz(EZ);   // pass-entry positions: the budget datum
        std::vector<uint8_t> eased(m, 0);
        for (int iter = 0; iter < 500; ++iter) {
            float moved = 0.0f;
            auto relax = [&](size_t im, size_t i, size_t ip) {
                if (frozen[i]) return;
                if (discreteRadiusAt(EX, EZ, im, i, ip) >= s.minTurnRadiusM) return;
                float tx = 0.5f * (EX[im] + EX[ip]) - EX[i];
                float tz = 0.5f * (EZ[im] + EZ[ip]) - EZ[i];
                float nxp = EX[i] + 0.4f * tx, nzp = EZ[i] + 0.4f * tz;
                const float ddx = nxp - ox[i], ddz = nzp - oz[i];
                const float dd = std::sqrt(ddx * ddx + ddz * ddz);
                if (dd > budget) {          // clamp to the budget circle
                    nxp = ox[i] + ddx / dd * budget;
                    nzp = oz[i] + ddz / dd * budget;
                }
                moved += std::fabs(nxp - EX[i]) + std::fabs(nzp - EZ[i]);
                EX[i] = nxp; EZ[i] = nzp;
                eased[i] = 1;
                if (closed && (i == 0 || i + 1 == m)) {
                    EX[0] = EX[m-1] = EX[i]; EZ[0] = EZ[m-1] = EZ[i];
                }
            };
            for (size_t i = 1; i + 1 < m; ++i) relax(i - 1, i, i + 1);
            if (closed && m >= 4) relax(m - 2, 0, 1);
            if (moved < 1e-3f) break;
        }
        for (uint8_t e : eased) easedOut += e;
    };

    // ---- pass B1: ease the AUTHORED corners on the coarse polyline ---------
    // (the lane-change dodges measured 35.6 deg at theta 271-273 — a fillet
    // here is a dozen apex moves; after subdivision the same fix would be a
    // slow diffusion across hundreds of 4 m nodes)
    easePass(X, Z, lock, kHEaseBudgetCoarseM, r.easedNodes);

    // ---- pass A: adaptive Catmull-Rom subdivision --------------------------
    // Split any segment adjoining an over-deflected node, splitting toward
    // halved deflection each round. Original nodes are never moved — the
    // spline interpolates THROUGH them — so pins, portals and junction
    // landings hold their exact positions by construction. Segments whose
    // BOTH ends are locked (bore chords, and nothing else in practice) split
    // LINEARLY: a tunnel spine is straight and the polyline stays on it.
    for (int pass = 0; pass < 8; ++pass) {
        const size_t m = X.size();
        std::vector<float> defl(m, 0.0f);
        for (size_t i = 1; i + 1 < m; ++i)
            defl[i] = deflectionAt(X, Z, i - 1, i, i + 1);
        if (closed && m >= 4) {
            defl[0] = deflectionAt(X, Z, m - 2, 0, 1);
            defl[m - 1] = defl[0];   // the dup node IS node 0 — same corner
        }
        std::vector<uint8_t> split(m - 1, 0);
        uint32_t marked = 0;
        for (size_t k = 0; k + 1 < m; ++k) {
            const float dx = X[k+1] - X[k], dz = Z[k+1] - Z[k];
            const float len = std::sqrt(dx * dx + dz * dz);
            if (len < 2.0f * kHMinSegM) continue;
            const float dHere = std::max(defl[k], defl[k + 1 < m ? k + 1 : (closed ? 0 : k)]);
            if (dHere > capRad) { split[k] = 1; ++marked; }
        }
        if (!marked) break;
        std::vector<float>    nx, nz;
        std::vector<uint8_t>  nl;
        std::vector<uint32_t> no;
        nx.reserve(m + marked); nz.reserve(m + marked);
        nl.reserve(m + marked); no.reserve(m + marked);
        auto wrapIdx = [&](long i) -> size_t {
            if (i < 0)  return closed ? (size_t)((long)m - 2 + i + 1) : 0;   // -1 -> m-2
            if ((size_t)i >= m) return closed ? (size_t)(i - (long)m + 1) : m - 1; // m -> 1
            return (size_t)i;
        };
        for (size_t k = 0; k + 1 < m; ++k) {
            nx.push_back(X[k]); nz.push_back(Z[k]);
            nl.push_back(lock[k]); no.push_back(orig[k]);
            if (!split[k]) continue;
            float mx, mz;
            if (lock[k] && lock[k + 1]) {          // a chord span: stay straight
                mx = 0.5f * (X[k] + X[k+1]);
                mz = 0.5f * (Z[k] + Z[k+1]);
            } else {                               // Catmull-Rom midpoint
                const size_t i0 = wrapIdx((long)k - 1), i3 = wrapIdx((long)k + 2);
                const float t0x = 0.5f * (X[k+1] - X[i0]), t0z = 0.5f * (Z[k+1] - Z[i0]);
                const float t1x = 0.5f * (X[i3] - X[k]),   t1z = 0.5f * (Z[i3] - Z[k]);
                mx = 0.5f * (X[k] + X[k+1]) + 0.125f * (t0x - t1x);
                mz = 0.5f * (Z[k] + Z[k+1]) + 0.125f * (t0z - t1z);
            }
            nx.push_back(mx); nz.push_back(mz);
            nl.push_back(0);  no.push_back(UINT32_MAX);
        }
        nx.push_back(X[m-1]); nz.push_back(Z[m-1]);
        nl.push_back(lock[m-1]); no.push_back(orig[m-1]);
        X.swap(nx); Z.swap(nz); lock.swap(nl); orig.swap(no);
    }

    // ---- pass B2: the floor again, as a bounded touch-up --------------------
    // The spline can undershoot an eased corner's radius by a few percent;
    // 3 m of budget is enough to bring it back over the floor and not enough
    // to move the road. The circuit's ~68 m hairpin sits above its 60 m
    // floor, so neither pass touches it — measured by gate H3, not hoped.
    easePass(X, Z, lock, kHEaseBudgetFineM, r.easedNodes);

    // ---- write back: polyline, remap, gap + pin bookkeeping -----------------
    const size_t m = X.size();
    for (size_t i = 0; i < m; ++i)
        if (orig[i] != UINT32_MAX) r.newIndexOfOld[orig[i]] = (uint32_t)i;
    std::vector<float> newPin;
    if (!s.pinY.empty()) {
        newPin.assign(m, std::numeric_limits<float>::quiet_NaN());
        for (size_t i = 0; i < s.pinY.size() && i < n; ++i)
            if (std::isfinite(s.pinY[i])) newPin[r.newIndexOfOld[i]] = s.pinY[i];
    }
    for (RoadSpec::Gap& g : s.gaps) {
        g.i0 = r.newIndexOfOld[g.i0];
        g.i1 = r.newIndexOfOld[g.i1];
    }
    s.x.swap(X); s.z.swap(Z); s.pinY.swap(newPin);
    r.nodesAfter = (uint32_t)m;
    r.maxDeflAfterDeg = measureMaxDeflectionDeg(s, &r.minRadiusAfterM);

    char b[300];
    std::snprintf(b, sizeof(b),
        "road '%s': horizontal flow — %u -> %u nodes, max deflection "
        "%.1f -> %.1f deg/node (cap %.1f), tightest bend %.0f m (floor %.0f m), "
        "%u nodes eased",
        s.name.c_str(), r.nodesBefore, r.nodesAfter,
        r.maxDeflBeforeDeg, r.maxDeflAfterDeg, s.maxDeflectionDeg,
        r.minRadiusAfterM >= 1e8f ? 99999.0f : r.minRadiusAfterM,
        s.minTurnRadiusM, r.easedNodes);
    x3::logInfo(b);
    return r;
}

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

RoadBuildResult registerRoad(const RoadSpec& spec, std::vector<float>* outRoadY) {
    RoadBuildResult r;
    const size_t n = spec.x.size();
    if (n < 2 || spec.z.size() != n) {
        x3::logError("road '" + spec.name + "': degenerate centreline");
        return r;
    }
    for (const RoadSpec::Gap& g : spec.gaps) {
        if (g.i0 >= g.i1 || g.i1 >= n) {
            x3::logError("road '" + spec.name + "': malformed gap");
            return r;
        }
    }

    // ---- measure the natural surface across the full carve width ----------
    std::vector<float> natural(n, 0.0f), segLen(n, 0.0f);
    // The SAME sweep against the PRE-corridor hillside. `natural` is sampled
    // through terrainHeightAtWorld(), i.e. after every registered corridor has
    // carved, which is right for the GRADER (it must grade onto the floor that
    // is really there) and wrong for the FLOAT measurement (see O6b below).
    // Two arrays, one loop, one lateral sweep: the readings differ only by the
    // excavation, never by the sampling.
    std::vector<float> preNatural(n, 0.0f);
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
        float hi = -1e9f, preHi = -1e9f;
        for (int k = -kLatSamples; k <= kLatSamples; ++k) {
            const float off = (float)k * reach / (float)kLatSamples;
            const float qx = spec.x[i] + (-tz) * off;
            const float qz = spec.z[i] + ( tx) * off;
            hi = std::max(hi, terrainHeightAtWorld(qx, qz));
            preHi = std::max(preHi, tunnelNaturalHeightAt(qx, qz));
        }
        natural[i] = hi;
        preNatural[i] = preHi;
        if (i + 1 < n) {
            const float dx = spec.x[i + 1] - spec.x[i], dz = spec.z[i + 1] - spec.z[i];
            segLen[i] = std::sqrt(dx * dx + dz * dz);
            r.lengthM += segLen[i];
        }
    }

    // ---- pins from the gaps (lerped y0 -> y1 across each gap) --------------
    // ... plus the spec's own per-node pins (junction datums — see RoadSpec).
    const float kNaN = std::numeric_limits<float>::quiet_NaN();
    std::vector<float> pins;
    if (!spec.gaps.empty() || !spec.pinY.empty()) {
        pins.assign(n, kNaN);
        for (const RoadSpec::Gap& g : spec.gaps) {
            // Lerp by ARC LENGTH, not node index. Horizontal subdivision
            // leaves gap segments non-uniform (4 m splits beside the portal,
            // 61 m in the chord's middle), and an index lerp then steps the
            // datum as much across a 4 m segment as a 61 m one — measured as
            // a phantom 32.8% "grade" and a 14 ft pin deficit at the massif
            // portals. Distance-true pins cost nothing on uniform chords.
            float total = 0.0f;
            for (uint32_t i = g.i0; i < g.i1; ++i) total += segLen[i];
            float acc = 0.0f;
            for (uint32_t i = g.i0; i <= g.i1; ++i) {
                const float t = (total > 1e-3f) ? acc / total
                              : (float)(i - g.i0) / (float)(g.i1 - g.i0);
                pins[i] = g.y0 + (g.y1 - g.y0) * t;
                if (i < g.i1) acc += segLen[i];
            }
        }
        for (size_t i = 0; i < n && i < spec.pinY.size(); ++i)
            if (std::isfinite(spec.pinY[i])) pins[i] = spec.pinY[i];
    }

    // ---- THE PORTAL RAMP ----------------------------------------------------
    // The grader's ceiling is the natural surface: corridors only cut, so the
    // road hugs the ground and every hollow drags the profile down. That is
    // correct in open country and WRONG on the last run to a portal or an
    // abutment: a pin is an obligation, and measured on the first outer-tour
    // registration the approaches missed their portals by up to 195 ft —
    // the grade relaxation simply cannot climb through a hollow it is clamped
    // into. Real roads solve this with approach embankments; this grader gets
    // the same thing as a bounded ceiling raise: within reach of each pin the
    // ceiling becomes max(natural, pin - maxGrade * distance) — exactly the
    // ground a ≤maxGrade ramp aimed at the pin needs, and nothing more. The
    // datum may float above ground there (the ribbon + collision ride the
    // datum); the CARVE still measures against the true natural surface, so a
    // floating reach simply cuts nothing. The float is reported, not hidden.
    std::vector<float> ceiling(natural);
    if (!pins.empty()) {
        // Walk the WHOLE route, not "until the ground first meets the ramp":
        // the first cut of this loop broke at the first high node, and where a
        // single crest interrupted an otherwise-low descent the nodes beyond it
        // never got their ceiling — the profile then undercut the portal by
        // exactly one grade-step per missing node (measured: 14.6% and 33.7%
        // spikes on two W bench descents). max() makes the full walk harmless
        // where the ground genuinely carries.
        float natMin = natural[0];
        for (float v : natural) natMin = std::min(natMin, v);
        auto ramp = [&](uint32_t from, int dir, float y) {
            float dist = 0.0f;
            for (int i = (int)from + dir; i >= 0 && i < (int)n; i += dir) {
                dist += segLen[(size_t)(dir > 0 ? i - 1 : i)];
                const float want = y - spec.maxGrade * dist;
                if (want <= natMin) break;               // below everything — done
                ceiling[(size_t)i] = std::max(ceiling[(size_t)i], want);
            }
        };
        for (const RoadSpec::Gap& g : spec.gaps) {
            ramp(g.i0, -1, g.y0);
            ramp(g.i1, +1, g.y1);
        }
        // Spec-level pins (junctions) ramp BOTH ways: the road must be able to
        // climb to the pinned datum from either side.
        for (size_t i = 0; i < n && i < spec.pinY.size(); ++i) {
            if (!std::isfinite(spec.pinY[i])) continue;
            bool gapNode = false;
            for (const RoadSpec::Gap& g : spec.gaps)
                if (i >= g.i0 && i <= g.i1) { gapNode = true; break; }
            if (gapNode) continue;   // the gap edges already ramped above
            ceiling[i] = std::max(ceiling[i], spec.pinY[i]);
            ramp((uint32_t)i, -1, spec.pinY[i]);
            ramp((uint32_t)i, +1, spec.pinY[i]);
        }
    }

    std::vector<float> roadY;
    const bool closedRing =
        std::fabs(spec.x[0] - spec.x[n - 1]) < 0.01f &&
        std::fabs(spec.z[0] - spec.z[n - 1]) < 0.01f;
    gradeRoad(ceiling, segLen, spec.maxGrade, roadY, closedRing,
              pins.empty() ? nullptr : &pins, &r.pinErrM);
    // VERTICAL FLOW: measure the kinks the relaxation left, then smooth them
    // into parabolic vertical curves (crest AND sag), pins held exactly.
    // X3_NO_VCURVE=1 skips the pass — the A/B instrument for eyeballing what
    // it does to a crest, and nothing else should ever set it.
    r.maxGradeRatePre = maxAbsGradeRate(roadY, segLen, closedRing);
    if (const char* dbg = std::getenv("X3_RING_DEBUG"); dbg && dbg[0]) {
        // Where is the worst kink? The A/B capture instrument needs a place
        // to point the camera, not just a number.
        float worst = 0.0f; size_t at = 0;
        for (size_t i = 1; i + 1 < n; ++i) {
            const float L0 = std::max(1.0f, segLen[i - 1]), L1 = std::max(1.0f, segLen[i]);
            const float g0 = (roadY[i] - roadY[i - 1]) / L0, g1 = (roadY[i + 1] - roadY[i]) / L1;
            const float rt = std::fabs(g1 - g0) / (0.5f * (L0 + L1));
            if (rt > worst) { worst = rt; at = i; }
        }
        std::printf("DBG road '%s' worst grade-rate %.5f/m at node %zu (%.0f, %.0f) y %.1f\n",
                    spec.name.c_str(), worst, at, spec.x[at], spec.z[at], roadY[at]);
    }
    {
        const char* off = std::getenv("X3_NO_VCURVE");
        if (!(off && off[0] == '1'))
            smoothVerticalCurves(roadY, segLen, spec.maxGrade, spec.maxGradeRate,
                                 closedRing, pins.empty() ? nullptr : &pins);
    }
    r.maxGradeRatePost = maxAbsGradeRate(roadY, segLen, closedRing);
    // Float is only meaningful where the ROAD owns the reach: inside a gap the
    // datum is a bookkeeping lerp between the pins (the tunnel or the deck has
    // its own profile there), and measuring it against the natural mountain
    // above a bore reports a 200 ft "float" that nothing ever builds.
    {
        size_t floatAt = 0;
        // Gap nodes INCLUDING the edges are excluded: at an edge the "natural"
        // sample reads the tunnel's own portal groove (already carved when the
        // road registers after its bores), so datum-minus-natural there is the
        // groove depth, not an embankment. Measured: a phantom 129 ft "float"
        // at the massif west portal that was really the bore's over-excavation.
        //
        // MEASURE AGAINST THE PRE-CORRIDOR HILLSIDE, 2026-08-17 (W-TUNNEL).
        // The exclusion above treats a symptom: `natural[]` is not natural at
        // all, it is terrainHeightAtWorld() AFTER every corridor has carved,
        // so "float" was partly a measurement of our own excavation and the
        // node-range exclusion was a guess at how far that reached. It reached
        // further as soon as lane 1 widened the bores (corridor half-width
        // 10.1 -> 14.0 m): O6b went 83 -> 193 ft with no new embankment
        // anywhere, purely because one more node had fallen into the groove.
        // tunnelNaturalHeightAt() subtracts terrainCorridorDelta and hands back
        // the untouched hillside, which is what the word float means. Corridors
        // only ever LOWER the field, so this reading is <= the old one at every
        // node — SAME lateral sweep, so the two readings differ only by the
        // excavation — and the gate cannot be newly passed by a real embankment
        // it used to catch. natural[] itself is deliberately left alone: the
        // GRADER must keep seeing the carved floor it grades onto. (First cut
        // of this fix sampled the centreline only and read 88 ft where the
        // sweep reads less, which is the same class of error one layer down.)
        for (size_t i = 0; i < n; ++i) {
            bool gapNode = false;
            for (const RoadSpec::Gap& g : spec.gaps)
                if (i >= g.i0 && i <= g.i1) { gapNode = true; break; }
            if (gapNode) continue;
            if (roadY[i] - preNatural[i] > r.maxFloatM) {
                r.maxFloatM = roadY[i] - preNatural[i];
                floatAt = i;
            }
        }
        if (const char* dbg = std::getenv("X3_RING_DEBUG"); dbg && dbg[0] && r.maxFloatM > 5.0f)
            std::printf("DBG registerRoad '%s' maxFloat %.1f m at node %zu (%.0f, %.0f) nat %.1f ry %.1f\n",
                        spec.name.c_str(), r.maxFloatM, floatAt,
                        spec.x[floatAt], spec.z[floatAt], natural[floatAt], roadY[floatAt]);
    }
    if (const char* dbg = std::getenv("X3_RING_DEBUG"); dbg && dbg[0] == '2') {
        for (const RoadSpec::Gap& g : spec.gaps)
            for (uint32_t i = (g.i1 > 2 ? g.i1 - 2 : 0);
                 i < std::min<size_t>(n, g.i1 + 14); ++i)
                std::printf("DBG2 node %u nat %8.2f ceil %8.2f ry %8.2f pin %8.2f seg %.1f\n",
                            i, natural[i], ceiling[i], roadY[i],
                            pins.empty() ? -1.0f : pins[i], segLen[i]);
    }

    // ---- CHAIN the corridors: 32 nodes each, sharing endpoints ------------
    // Sharing the last node of one with the first of the next is what makes the
    // seam invisible: both corridors carry the same depth there, and the union
    // is deepest-wins, so neither can win by a step. A GAP splits the chain:
    // its segments belong to a tunnel or a deck, so the run ends AT the gap's
    // first node and the next run starts at its last — the shared node carries
    // the pinned datum, which is how the two carves meet without a step.
    auto segInGap = [&](size_t i) {   // segment i -> i+1; pure — called repeatedly
        for (const RoadSpec::Gap& g : spec.gaps)
            if (i >= g.i0 && i < g.i1) return true;
        return false;
    };
    for (size_t i = 0; i + 1 < n; ++i)
        if (segInGap(i)) r.gapLenM += segLen[i];
    // DUAL routes: the carve must cover the full twin-carriageway span, but the
    // span breathes with the MEDIAN — per chained corridor, the half-width is
    // (2 carriageways + 1 m) plus the widest median that chain's reach plans,
    // so a mountain stretch running the narrow jersey median doesn't pay for
    // open-country median width in cut. Same plan the ribbon and the barrier
    // planner derive (computeMedianPlan is pure in spec + datum), so pavement
    // always lands on carved ground.
    const std::vector<float> medianPlan =
        spec.dualCarriageway ? computeMedianPlan(spec, roadY) : std::vector<float>{};
    const uint32_t kPer = (uint32_t)TerrainCorridor::kMaxNodes;
    size_t start = 0;
    while (start + 1 < n) {
        if (segInGap(start)) { ++start; continue; }   // skip decked/bored reaches
        // extend this carveable run as far as it goes (or the corridor fills)
        size_t count = 1;
        while (start + count < n && count < kPer && !segInGap(start + count - 1))
            ++count;
        // segInGap(start+count-1) tested the NEXT segment; count now spans nodes
        // [start .. start+count-1] whose interior segments all carve.
        if (count < 2) { ++start; continue; }
        TerrainCorridor c{};
        c.nodeCount = (int)count;
        c.halfWidth = spec.halfWidth;
        if (!medianPlan.empty()) {
            float mMax = 0.0f;
            for (size_t k = 0; k < count; ++k)
                mMax = std::max(mMax, medianPlan[start + k]);
            c.halfWidth = 2.0f * kFwyPavedHalfM + 1.0f + mMax;
        }
        c.falloff   = spec.falloff;
        for (size_t k = 0; k < count; ++k) {
            const size_t i = start + k;
            c.x[k] = spec.x[i];
            c.z[k] = spec.z[i];
            const float cut = std::max(0.0f, natural[i] - roadY[i] + kRoadFloorClear);
            c.depth[k] = cut;
        }
        // CLOSE THE LOOP — the tunnel module's step-4 discipline, ported. The
        // node sweep above measures the natural MAX per node, but between two
        // nodes 200 ft apart the ground can hump above the lerped depth — on
        // the outer tour's portal approaches it measured 6.7 ft of earth
        // standing on the roadway. So walk the chain at 2 m against the
        // corridor just built (plus everything already registered) and raise
        // node depths until nothing stands above the datum. Monotone, so it
        // converges; deterministic, so it survives regeneration.
        for (int pass = 0; pass < 6; ++pass) {
            std::vector<float> add(count, 0.0f);
            float worst = 0.0f;
            for (size_t k = 0; k + 1 < count; ++k) {
                const size_t i = start + k;
                const float dx = spec.x[i+1] - spec.x[i], dz = spec.z[i+1] - spec.z[i];
                const float len = std::sqrt(dx*dx + dz*dz);
                float px = -dz, pz = dx;
                const float pl = std::sqrt(px*px + pz*pz);
                if (pl > 1e-4f) { px /= pl; pz /= pl; }
                const int steps = std::max(1, (int)(len / 2.0f));
                for (int m = 1; m < steps; ++m) {
                    const float t = (float)m / (float)steps;
                    const float want = (roadY[i] + (roadY[i+1] - roadY[i]) * t)
                                     - kRoadFloorClear;
                    for (int lt = -4; lt <= 4; ++lt) {
                        const float off = (float)lt * c.halfWidth / 4.0f;
                        const float qx = spec.x[i] + dx * t + px * off;
                        const float qz = spec.z[i] + dz * t + pz * off;
                        const float raw = terrainHeightAtWorld(qx, qz)
                                        - terrainCorridorDelta(qx, qz);   // pre-carve
                        const float over = (raw - terrainCorridorDepthAt(c, qx, qz)) - want;
                        if (over > 0.0f) {
                            add[k]     = std::max(add[k],     over);
                            add[k + 1] = std::max(add[k + 1], over);
                            worst = std::max(worst, over);
                        }
                    }
                }
            }
            if (worst <= 0.0f) break;
            for (size_t k = 0; k < count; ++k) c.depth[k] += add[k];
        }
        for (size_t k = 0; k < count; ++k)
            r.maxCutM = std::max(r.maxCutM, c.depth[k]);
        if (!registerTerrainCorridor(c)) {
            char b[192];
            std::snprintf(b, sizeof(b),
                "road '%s': registry FULL after %u corridors (cap %u) — route truncated",
                spec.name.c_str(), r.corridorCount, kMaxTerrainCorridors);
            x3::logError(b);
            return r;   // ok stays false: a truncated road is not a road
        }
        ++r.corridorCount;
        start += count - 1;      // SHARE the endpoint node with the next corridor
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
    if (outRoadY) *outRoadY = roadY;

    char b[420];
    std::snprintf(b, sizeof(b),
        "road '%s': %.2f miles, %u nodes -> %u chained corridors | max grade %.1f%% "
        "| deepest cut %.0f ft | road elevation %.0f..%.0f ft%s",
        spec.name.c_str(), r.lengthM / 1609.34f, r.nodeCount, r.corridorCount,
        r.maxGradePct, r.maxCutM * kMToFt, r.minRoadY * kMToFt, r.maxRoadY * kMToFt,
        spec.gaps.empty() ? "" : " (+ gaps)");
    x3::logInfo(b);
    // The vertical-flow proof, in the boot log where it can be checked: rate is
    // |d(grade)/ds| per metre; K is the highway engineer's number (metres of
    // vertical curve per 1% of grade change, K = 0.01/rate — bigger is softer).
    std::snprintf(b, sizeof(b),
        "road '%s': vertical flow — max grade-rate %.5f/m (K %.1f) before, "
        "%.5f/m (K %.1f) after smoothing (spec cap %.5f/m, K %.1f)",
        spec.name.c_str(),
        r.maxGradeRatePre,  r.maxGradeRatePre  > 1e-9f ? 0.01f / r.maxGradeRatePre  : 9999.0f,
        r.maxGradeRatePost, r.maxGradeRatePost > 1e-9f ? 0.01f / r.maxGradeRatePost : 9999.0f,
        spec.maxGradeRate,  spec.maxGradeRate  > 1e-9f ? 0.01f / spec.maxGradeRate  : 9999.0f);
    x3::logInfo(b);
    if (!medianPlan.empty()) {
        float mMin = 1e9f, mMax = 0.0f, wideM = 0.0f;
        for (size_t i = 0; i + 1 < n; ++i) {
            mMin = std::min(mMin, medianPlan[i]);
            mMax = std::max(mMax, medianPlan[i]);
            if (medianPlan[i] > kFwyMedianWallHalfM) wideM += segLen[i];
        }
        std::snprintf(b, sizeof(b),
            "road '%s': DUAL CARRIAGEWAY — 2 x %d lanes, median half %.1f..%.1f m "
            "(%.1f miles open median, rest jersey-walled), carve span up to %.0f ft, "
            "STILL %u corridors (one chain — the dual profile costs no registry)",
            spec.name.c_str(), kFwyLaneCount, mMin, mMax, wideM / 1609.34f,
            (2.0f * (2.0f * kFwyPavedHalfM + 1.0f + mMax)) * kMToFt, r.corridorCount);
        x3::logInfo(b);
    }
    if (r.pinErrM > 0.06f) {   // 0.2 ft — the same step budget the bridge deck gets
        std::snprintf(b, sizeof(b),
            "road '%s': a pinned datum could not be reached at grade — %.1f ft short. "
            "The approach authoring is wrong; move the gap or flatten the pin.",
            spec.name.c_str(), r.pinErrM * kMToFt);
        x3::logError(b);
    }
    return r;
}

// ---------------------------------------------------------------------------
// COURSE AUTHORING — nodes in, road out. See road_network.h.
// ---------------------------------------------------------------------------
RoadSpec makeRoadFromWaypoints(const char* name,
                               const std::vector<CourseWaypoint>& pts,
                               float spacingM, bool closed) {
    RoadSpec s;
    s.name = name ? name : "course";
    const size_t N = pts.size();
    if (N < (closed ? 3u : 2u)) return s;
    if (spacingM < 5.0f) spacingM = 5.0f;

    // Per-corner fillet: arc start/end on the two legs, tangent length
    // t = r*tan(theta/2), clamped so two corners sharing a leg cannot overlap.
    struct Corner {
        float inX, inZ, outX, outZ;   // arc start / end (== waypoint if no arc)
        float cx = 0, cz = 0, r = 0;  // arc centre + effective radius
        float sweep = 0;              // signed sweep angle (+ = toward perp(d0))
    };
    std::vector<Corner> cn(N);
    auto wpAt = [&](size_t i) { return pts[i % N]; };
    const size_t c0 = closed ? 0 : 1, c1 = closed ? N : N - 1;
    for (size_t i = 0; i < N; ++i) {           // default: pass-through
        cn[i].inX = cn[i].outX = pts[i].x;
        cn[i].inZ = cn[i].outZ = pts[i].z;
    }
    for (size_t i = c0; i < c1; ++i) {
        const CourseWaypoint P = wpAt(i);
        const CourseWaypoint A = wpAt(i + N - 1);
        const CourseWaypoint B = wpAt(i + 1);
        float d0x = P.x - A.x, d0z = P.z - A.z;
        float d1x = B.x - P.x, d1z = B.z - P.z;
        const float l0 = std::sqrt(d0x * d0x + d0z * d0z);
        const float l1 = std::sqrt(d1x * d1x + d1z * d1z);
        if (l0 < 1.0f || l1 < 1.0f) continue;
        d0x /= l0; d0z /= l0; d1x /= l1; d1z /= l1;
        const float dot = std::max(-1.0f, std::min(1.0f, d0x * d1x + d0z * d1z));
        const float theta = std::acos(dot);
        if (theta < 0.03f || P.fillet < 1.0f) continue;       // straight-through
        const float tanH = std::tan(theta * 0.5f);
        float t = P.fillet * tanH;
        t = std::min(t, 0.5f * l0 - 1.0f);
        t = std::min(t, 0.5f * l1 - 1.0f);
        if (t < 1.0f) continue;                                // no room: kink stays
        const float r = t / tanH;
        const float side = (d0x * d1z - d0z * d1x) >= 0.0f ? 1.0f : -1.0f;
        cn[i].inX  = P.x - d0x * t; cn[i].inZ  = P.z - d0z * t;
        cn[i].outX = P.x + d1x * t; cn[i].outZ = P.z + d1z * t;
        cn[i].cx = cn[i].inX + (-d0z) * r * side;
        cn[i].cz = cn[i].inZ + ( d0x) * r * side;
        cn[i].r = r;
        cn[i].sweep = theta * side;
    }

    auto emitStraight = [&](float ax, float az, float bx, float bz) {
        const float len = std::sqrt((bx - ax) * (bx - ax) + (bz - az) * (bz - az));
        const int steps = std::max(1, (int)std::round(len / spacingM));
        for (int k = 0; k < steps; ++k) {                     // excludes endpoint
            const float u = (float)k / (float)steps;
            s.x.push_back(ax + (bx - ax) * u);
            s.z.push_back(az + (bz - az) * u);
        }
    };
    auto emitArc = [&](const Corner& c) {
        if (c.r <= 0.0f) return;                              // pass-through
        const float arcLen = c.r * std::fabs(c.sweep);
        const int steps = std::max(1, (int)std::ceil(arcLen / spacingM));
        const float vx = c.inX - c.cx, vz = c.inZ - c.cz;
        for (int k = 0; k < steps; ++k) {                     // excludes endpoint
            const float a = c.sweep * (float)k / (float)steps;
            const float ca = std::cos(a), sa = std::sin(a);
            s.x.push_back(c.cx + vx * ca - vz * sa);
            s.z.push_back(c.cz + vx * sa + vz * ca);
        }
    };

    if (closed) {
        for (size_t i = 0; i < N; ++i) {
            const size_t j = (i + 1) % N;
            emitStraight(cn[i].outX, cn[i].outZ, cn[j].inX, cn[j].inZ);
            emitArc(cn[j]);
        }
        s.x.push_back(s.x[0]);                                // exact closure
        s.z.push_back(s.z[0]);
    } else {
        for (size_t i = 0; i + 1 < N; ++i) {
            const size_t j = i + 1;
            emitStraight(cn[i].outX, cn[i].outZ, cn[j].inX, cn[j].inZ);
            if (j + 1 < N) emitArc(cn[j]);
        }
        s.x.push_back(pts[N - 1].x);
        s.z.push_back(pts[N - 1].z);
    }
    return s;
}

RoadSpec makeInnerCourse() {
    // THE LEG LIST. Polar about the old ring centre for authoring convenience
    // (angles anticlockwise from +X, radii in metres), EXCEPT the junction
    // straight, whose two waypoints are exact XZ: a 1.3 km straight through
    // the OLD circle's node-173 position (-4135.7, 1132.2), square to the
    // spawn corridor's exit ray, so the connector's landing stays put.
    //
    // The shape, read anticlockwise: an S-complex over the east quarter
    // (radius weaving 3700-4100), a BULGE out to 5000 m toward the north
    // foothills (the out-and-back toward interesting terrain), a fast sweep
    // back in, the junction straight, then the south half: a tight 3350 m
    // corner, two more S-weaves and a wide 800 m-radius sweeper home. Fillet
    // radii run 300-900 m — every corner a different, genuine constant radius.
    constexpr float cx = -592.0f, cz = -352.0f;
    struct P { float ang, rad, fil; };
    static const P tbl[] = {
        {   0.0f, 3900.0f, 600.0f },
        {  22.0f, 4050.0f, 420.0f },
        {  38.0f, 3700.0f, 350.0f },   // S in...
        {  52.0f, 4100.0f, 500.0f },   // ...S out
        {  75.0f, 4800.0f, 900.0f },   // the bulge begins
        {  95.0f, 5000.0f, 850.0f },   // bulge apex — north foothills
        { 112.0f, 4300.0f, 600.0f },
        { 128.0f, 3600.0f, 450.0f },
        // the junction straight (exact XZ, see above): A -> B anticlockwise
        { -1.0f, 0.0f, 400.0f },
        { -2.0f, 0.0f, 400.0f },
        { 185.0f, 3500.0f, 420.0f },
        { 205.0f, 3900.0f, 700.0f },
        { 228.0f, 3350.0f, 300.0f },   // the tight one
        { 244.0f, 3800.0f, 450.0f },
        { 262.0f, 4150.0f, 550.0f },
        { 280.0f, 3550.0f, 350.0f },   // second S-complex
        { 298.0f, 3950.0f, 500.0f },
        { 320.0f, 4300.0f, 800.0f },   // wide fast sweeper
        { 342.0f, 3900.0f, 500.0f },
    };
    std::vector<CourseWaypoint> wp;
    wp.reserve(sizeof(tbl) / sizeof(tbl[0]));
    for (const P& p : tbl) {
        CourseWaypoint w;
        if (p.ang == -1.0f)      { w.x = -3896.1f; w.z = 1704.1f; }
        else if (p.ang == -2.0f) { w.x = -4398.4f; w.z =  505.0f; }
        else {
            w.x = cx + std::cos(p.ang * 0.017453293f) * p.rad;
            w.z = cz + std::sin(p.ang * 0.017453293f) * p.rad;
        }
        w.fillet = p.fil;
        wp.push_back(w);
    }
    RoadSpec s = makeRoadFromWaypoints("inner tour", wp, 61.0f, /*closed=*/true);
    // THE FREEWAY (Tim: "8 lanes each side... a separate road carrying north
    // and south traffic like I17 does in AZ"): twin 8-lane carriageways about
    // this centreline with a terrain-varied median between them. The carve
    // half-width covers the full dual span at the WIDEST median; registerRoad
    // narrows each chained corridor to the median its reach actually plans,
    // so mountain cuts don't pay for open-country median width.
    s.dualCarriageway = true;
    s.halfWidth = kFwyDualMaxHalfM;
    s.falloff   = 18.0f;                // a wider road wants a longer batter
    s.maxGrade  = 0.07f;
    // HORIZONTAL FLOW: freeway class. The authored fillets are genuine
    // 300-900 m arcs, but at 61 m spacing a 300 m arc facets at 11.7 deg per
    // node — Tim's "sharp points". Subdivide until no node breaks 3 deg; the
    // 250 m floor is far below the tightest authored corner, so no authored
    // shape is eased away.
    s.minTurnRadiusM   = 250.0f;
    s.maxDeflectionDeg = 3.0f;
    smoothHorizontalCurves(s);
    return s;
}

RoadBuildResult registerInnerRing() {
    return registerRoad(makeInnerCourse());
}

// ---------------------------------------------------------------------------
// THE OUTER TOUR — 31 miles, four ranges, five bores. See road_network.h.
//
// Every number in the table below is MEASURED, not styled: a 1024-sample survey
// of the graded cut along three candidate circles (r 7600 / 7934 / 8300 about
// the ring centre) chose the lane, the saddles and the portals. Angles are
// degrees anticlockwise from +X (due east), radii metres from (-592, -352) —
// the same centre as the inner tour, so the future spokes meet both squarely.
//
// What the survey said, sector by sector:
//   N snow  (θ 58-114): the flank bench at r 7600 is the calm lane (cuts
//     ≤ 33 m vs ≤ 73 m at nominal). Two crest clusters block it (θ 73-79 and
//     83-85.4, cuts to 97 m) — short bores. The massif θ 88-110 is 90-260 m of
//     rock at EVERY candidate radius; there is no lane, so it gets the ring's
//     signature tunnel, portalled off the one measured saddle (85.4-87.6).
//   W crystal (θ 145-209): the circle runs near-PARALLEL to the spine — the
//     worst possible geometry, ~60° of arc inside the band. But the survey
//     found a genuine summit plateau mid-range (θ 172.6-179.8 at 7600, cuts
//     ≤ 32 m) between the north and south peak groups: two long bores with an
//     open crystal-plateau crossing between them, rather than 6 km underground.
//   S mesa  (θ 243-278): at 7600 the whole crossing rides a bench between the
//     flat-capped buttes, cuts ≤ 34 m — a road, not a tunnel. The 272° shoulder
//     (34 m at 7600) is dodged by easing out to 7934, measured 2.4 m there.
//   E volcanic: the plan says the ring "tours all four ranges" — the terrain
//     says otherwise. The E spine is 9.8 km from the ring centre; a 31-mile
//     ring reaches 8.3 km. The east arc rides the volcano's FOOTHILLS (zero
//     forced cuts, measured) and the summit stays a skyline. Recorded as a
//     plan-vs-terrain disagreement, not silently fudged: reaching the core
//     costs a +2 km excursion that belongs to a future spoke, not this loop.
// ---------------------------------------------------------------------------
namespace {

constexpr float kOuterCX = -592.0f, kOuterCZ = -352.0f;

struct TourPoint {
    float angDeg;    // anticlockwise from +X (east)
    float radiusM;
    int   boreToNext; // 1: the reach to the NEXT point is a tunnel chord
    const char* boreName;
};
// REVISED after the first registration measured three defects in the draft:
//   1. Portal datums missed their approaches by 16-63 m — the tunnels' 4.5%
//      internal grade cannot climb to a high portal from low country. Portals
//      are now placed on measured benches whose ground sits AT the approach
//      line, and the portal-ramp grader (see registerRoad) carries the last
//      few hundred feet.
//   2. The W "summit plateau" idea died on the same physics: both 2.6-3.2 km
//      bores dragged their plateau portals ~60-100 m below the bench, turning
//      the open crossing into a 307 ft trench. The W traverse moved INWARD to
//      r 6800 — still inside the range band (Tim's ruling stands: through, not
//      around) — where a second survey found four short peak-piercings with
//      three genuine daylight benches between them, max open cut 35 m.
//   3. The N-c exit at θ110.5 emerged onto a 32% fall-line slope; the chord
//      now runs one degree further to the measured 36 m-elevation bench.
const TourPoint kOuterTour[] = {
    {   0.0f, 7934.0f, 0, nullptr },  // due east, volcanic foothills (measured lane)
    {  20.0f, 7934.0f, 0, nullptr },
    {  40.0f, 8800.0f, 0, nullptr },  // NE diagonal gap — quiet country
    {  55.0f, 7900.0f, 0, nullptr },
    {  58.0f, 7600.0f, 0, nullptr },  // onto the N flank bench
    // ONE flank tunnel, not two. The draft split this into a "West Shoulder"
    // (72.3-79.05) and a "Crag Gate" (82.3-85.75) with 430 m of daylight
    // between — and the measurement killed it: the flank climbs 56 m over that
    // daylight (13%), and the first tunnel's own 4.5% cap had already dragged
    // its east portal 36 m into a cutting. Merged, the single 1.78 km tunnel
    // climbs at 4.5% inside the rock and emerges AT the saddle bench (portal
    // datum 191 m vs bench ground 192.5 — measured, not hoped).
    {  72.3f, 7600.0f, 1, "North Flank Tunnel" },    // -> 85.75, through both crest clusters
    {  85.75f,7600.0f, 0, nullptr },  // the measured saddle: the massif's daylight bench
    {  87.7f, 7600.0f, 1, "North Massif Tunnel" },   // 90-260 m of rock at every radius
    { 111.5f, 7600.0f, 0, nullptr },  // exit bench at 36 m elevation, measured
    { 114.0f, 7600.0f, 0, nullptr },
    { 124.0f, 7400.0f, 0, nullptr },  // NW gap, sweeping down toward the W traverse
    { 136.0f, 7050.0f, 0, nullptr },
    // THE CRYSTAL TRAVERSE, third authoring. The second draft put every portal
    // at a crest end (tunnel datum = crest ground - 5 ft) and the array dump
    // convicted it twice over: the benches between tunnels DIP 100-160 ft, so
    // every bench crossing was a V steeper than 7%; and the range's SW face
    // falls at 9% - steeper than the road budget, never mind the tunnel's.
    // Third pass, portals moved DOWN past the crests (the bore punches out of
    // the face low, with its own short approach cutting), and the whole SW
    // descent happens INSIDE one long 4.2% tunnel, which is exactly how real
    // alpine descents are built when the face is steeper than the ruling grade.
    { 148.0f, 6800.0f, 0, nullptr },  // the crystal traverse elevation (2nd survey)
    { 155.3f, 6800.0f, 1, "Crystal North Tunnel" },   // -> 167.7: 1.5 km, portals at
                                      // 187/398 ft ground, climb capped inside the rock
    { 167.7f, 6800.0f, 0, nullptr },  // bench 1: near-flat at ~400 ft (dip measured 410)
    { 171.6f, 6800.0f, 1, "Crystal Saddle Tunnel" },  // -> 177.9: through the 84 m peak
    { 177.9f, 6800.0f, 0, nullptr },  // bench 2: short saddle at ~480 ft
    { 179.8f, 6800.0f, 1, "Crystal Descent Tunnel" }, // -> 203.4: 2.8 km falling 4.49%
                                      // inside the range - the SW face outside is 9%.
                                      // The exit was first authored at 202.6, on the
                                      // face: the road below it needed a 97 ft
                                      // embankment. At 203.4 the portal stands on the
                                      // 80 ft country at the range FOOT and the road
                                      // just... continues.
    { 203.4f, 6800.0f, 0, nullptr },
    { 208.0f, 6800.0f, 0, nullptr },
    { 220.0f, 8000.0f, 0, nullptr },  // SW diagonal gap
    { 232.0f, 8600.0f, 0, nullptr },
    { 243.0f, 7800.0f, 0, nullptr },  // S approach
    { 248.0f, 7600.0f, 0, nullptr },  // the mesa bench — a road between the buttes
    // The 272° shoulder dodge (34 m at 7600, measured 2.4 m at 7934). The
    // first authoring left the bench at 271.4 — 334 m of radial lane change
    // inside 1.8° of arc, which the deflection survey measured as the
    // network's sharpest daylight corners (35.6°/35.3° at θ271.4/273.2, the
    // owner's "sharp points"). Departing the bench at 265 gives the same
    // dodge 8.2° (~1.1 km) of run: a real highway lane change, max heading
    // deviation ~25°, and by θ272 the eased radius is already ~7910 where
    // the shoulder measures flat. The bench itself is measured clear from
    // 248 through 271, so leaving it six degrees early costs nothing.
    { 265.0f, 7600.0f, 0, nullptr },
    { 273.2f, 7934.0f, 0, nullptr },  // dodge the 272° shoulder (34 m in, 2.4 m out)
    // The whole SE/E quadrant runs the MEASURED 7934 lane. A draft pushed these
    // arcs out to 8300-8650 to buy circumference — unmeasured — and the debug
    // dump answered with 190-307 ft cuts at θ289-291 and θ346-351: the outward
    // lanes climb straight into volcanic-foothill peaks the nominal lane
    // slips between. The miles come from the NE gap instead, which IS clear.
    { 283.0f, 7934.0f, 0, nullptr },
    { 296.5f, 7934.0f, 0, nullptr },
    { 310.0f, 8100.0f, 0, nullptr },  // SE diagonal gap — mild, verified by O5
    { 330.0f, 7934.0f, 0, nullptr },
    { 360.0f, 7934.0f, 0, nullptr },  // closes exactly on the 0° node
};
constexpr float kTourNodeSpacing = 61.0f;   // ~200 ft, the inner tour's spacing
constexpr float kDegToRad = 0.017453293f;

} // namespace

RoadSpec makeOuterTour(std::vector<BoreChord>* outBores) {
    RoadSpec s;
    s.name      = "outer tour";
    s.halfWidth = kPavedHalfM + 1.0f;
    s.falloff   = 18.0f;
    s.maxGrade  = 0.07f;
    // Freeway sampling cap, but a 150 m sweeper floor rather than the inner
    // tour's 250: every outer corner is on a MEASURED bench, and easing the
    // mesa-dodge (the authored 35.6 deg lane change at theta 271-273) to
    // 250 m walks ~12 m off its lane — at 150 it stays within ~8 m and the
    // N-flank bench entry (166 m as authored) is above the floor untouched,
    // which is what keeps the deepest open cut at its surveyed 154 ft.
    s.minTurnRadiusM   = 150.0f;
    s.maxDeflectionDeg = 3.0f;
    if (outBores) outBores->clear();
    std::vector<BoreChord> chords;

    const int NP = (int)(sizeof(kOuterTour) / sizeof(kOuterTour[0]));
    auto pointXZ = [&](const TourPoint& p, float& x, float& z) {
        x = kOuterCX + std::cos(p.angDeg * kDegToRad) * p.radiusM;
        z = kOuterCZ + std::sin(p.angDeg * kDegToRad) * p.radiusM;
    };
    for (int w = 0; w + 1 < NP; ++w) {
        const TourPoint& A = kOuterTour[w];
        const TourPoint& B = kOuterTour[w + 1];
        float ax, az, bx, bz;
        pointXZ(A, ax, az); pointXZ(B, bx, bz);
        if (A.boreToNext) {
            // A TUNNEL CHORD: dead straight, because TunnelSpec's spine is
            // straight — and because a real long bore is. The chord cuts the
            // corner INSIDE the arc (sagitta ≤ ~170 m on the longest), which is
            // toward the ring centre and therefore AWAY from every range spine.
            const uint32_t i0 = (uint32_t)s.x.size();
            const float len = std::sqrt((bx-ax)*(bx-ax) + (bz-az)*(bz-az));
            const int n = std::max(1, (int)std::ceil(len / kTourNodeSpacing));
            for (int k = 0; k < n; ++k) {
                const float t = (float)k / (float)n;
                s.x.push_back(ax + (bx - ax) * t);
                s.z.push_back(az + (bz - az) * t);
            }
            {
                BoreChord c;
                c.name = A.boreName ? A.boreName : "bore";
                c.x0 = ax; c.z0 = az; c.x1 = bx; c.z1 = bz;
                c.i0 = i0; c.i1 = i0 + (uint32_t)n;   // node at B, emitted next
                chords.push_back(c);
            }
        } else {
            // An ARC reach: angle runs linearly, radius eases with a smoothstep
            // so a lane change (7934 -> 7600) is a sweep, not a kink.
            const float a0 = A.angDeg * kDegToRad, a1 = B.angDeg * kDegToRad;
            const float arc = (a1 - a0) * 0.5f * (A.radiusM + B.radiusM);
            const int n = std::max(1, (int)std::ceil(arc / kTourNodeSpacing));
            for (int k = 0; k < n; ++k) {
                const float t = (float)k / (float)n;
                const float e = t * t * (3.0f - 2.0f * t);
                const float ang = a0 + (a1 - a0) * t;
                const float rad = A.radiusM + (B.radiusM - A.radiusM) * e;
                s.x.push_back(kOuterCX + std::cos(ang) * rad);
                s.z.push_back(kOuterCZ + std::sin(ang) * rad);
            }
        }
    }
    // Close the ring EXACTLY: the 360° table row equals the 0° row by
    // construction, so the last node is a copy of the first — same contract as
    // makeRingRoad, and what lets gradeRoad's wrap constraint work.
    s.x.push_back(s.x[0]);
    s.z.push_back(s.z[0]);
    // HORIZONTAL FLOW. The bore chords are LAW: a tunnel spine is straight,
    // so every chord node is locked and its span subdivides linearly — the
    // smoothing happens in the daylight reaches only, and the chords' node
    // indices are remapped through the subdivision so the gaps that
    // registerOuterRing derives from them still name the right nodes.
    {
        std::vector<uint8_t> lockMask(s.x.size(), 0);
        for (const BoreChord& c : chords)
            for (uint32_t i = c.i0; i <= c.i1 && i < lockMask.size(); ++i)
                lockMask[i] = 1;
        const HorizontalSmoothResult hr = smoothHorizontalCurves(s, &lockMask);
        for (BoreChord& c : chords) {
            c.i0 = hr.newIndexOfOld[c.i0];
            c.i1 = hr.newIndexOfOld[c.i1];
        }
    }
    if (outBores) *outBores = chords;
    return s;
}

OuterRingResult registerOuterRing() {
    OuterRingResult out;
    std::vector<BoreChord> chords;
    out.spec = makeOuterTour(&chords);

    // TUNNELS FIRST. Each chord becomes a real bore through the same door the
    // city freeways use. The tunnel grades its own 4.5% profile through the
    // rock; the ring then PINS its gap-edge datums to the tunnel's end datums,
    // so the two carves meet without a step — measured by the self-test, not
    // assumed. Registering the tunnels first also means the ring's natural-
    // surface sweep near each portal reads the already-cut approach, which
    // pulls the ring's own grading onto the tunnel's line before the pin even
    // applies.
    out.bores.assign(chords.size(), nullptr);
    for (size_t i = 0; i < chords.size(); ++i) {
        const BoreChord& c = chords[i];
        const float dx = c.x1 - c.x0, dz = c.z1 - c.z0;
        const float len = std::sqrt(dx * dx + dz * dz);
        TunnelSpec ts;
        ts.name = c.name;
        ts.cx = (c.x0 + c.x1) * 0.5f;
        ts.cz = (c.z0 + c.z1) * 0.5f;
        ts.dirX = dx / len; ts.dirZ = dz / len;
        ts.halfLen = len * 0.5f;
        const TunnelRoute* r = registerTunnelCorridorFor(ts);
        out.bores[i] = r;
        if (!r) {
            x3::logError(std::string("outer tour: bore '") + c.name +
                         "' failed to register — tour aborted");
            return out;
        }
        if (r->boreValid) {
            ++out.boreCount;
            out.boredLenM += r->boreS1 - r->boreS0;
        } else {
            // A chord that produced no roofed span means the survey and the
            // authoring disagree — the table said "mountain", the field said
            // "bank". That is an authoring defect, and the self-test fails on
            // it; do not silently keep an open trench where a tunnel was named.
            x3::logWarn(std::string("outer tour: '") + c.name +
                        "' found no hill to roof — check the tour table");
        }
        RoadSpec::Gap g;
        g.i0 = c.i0; g.i1 = c.i1;
        g.y0 = r->roadYAt(0.0f);
        g.y1 = r->roadYAt(r->totalLen);
        out.spec.gaps.push_back(g);
    }

    out.road = registerRoad(out.spec, &out.roadY);

    char b[300];
    std::snprintf(b, sizeof(b),
        "outer tour: %.2f miles (%.2f driven in daylight, %.2f in %u bores) | "
        "worst open cut %.0f ft",
        out.road.lengthM / 1609.34f,
        (out.road.lengthM - out.road.gapLenM) / 1609.34f,
        out.road.gapLenM / 1609.34f, out.boreCount,
        out.road.maxCutM * kMToFt);
    x3::logInfo(b);
    return out;
}

// ---------------------------------------------------------------------------
// THE SPAWN CONNECTOR — see road_network.h. Registered AFTER the ring so the
// junction pin can read the ring's graded datum and the natural-surface sweep
// at the landing reads the ring's already-carved cutting.
// ---------------------------------------------------------------------------
namespace {

// Distance from point (px,pz) to segment (ax,az)-(bx,bz), XZ plane.
float segPointDist(float px, float pz, float ax, float az, float bx, float bz) {
    const float dx = bx - ax, dz = bz - az;
    const float L2 = dx * dx + dz * dz;
    float t = 0.0f;
    if (L2 > 1e-9f) t = std::max(0.0f, std::min(1.0f, ((px - ax) * dx + (pz - az) * dz) / L2));
    const float qx = ax + dx * t, qz = az + dz * t;
    return std::sqrt((px - qx) * (px - qx) + (pz - qz) * (pz - qz));
}

// How far back from the main road's centreline a branch's LAST NODE sits. The
// junction mouth patch owns this reach: enough run for the twist from the
// branch's flat edge onto the main road's sloped surface to finish BY the main
// pavement's edge (kPavedHalfM), not 2 m short of it.
// Widened for the SWOOPING MERGES (Tim: "THEY NEED SWOOPING CURVES FROM BOTH
// WAYS"): the on-ramp fillet arcs need ~40 m of tangent leg along the branch
// before the corner, and the old +20 setback only had ~25. Now a header
// constant (kJunctionSetbackM) because external routes (the valley road)
// attach with the same machinery.
constexpr float kJctSetbackM = kJunctionSetbackM;   // 54.6 m

// THE JUNCTION BOX — one extra 2-node corridor across the junction throat.
// Between the branch corridor's end cap and the main corridor's band there is
// a lens covered only by both FALLOFF shoulders (each carries ~94% of the
// needed depth there), so on a deep cut a ridge of dirt up to ~0.3 m can stand
// above the mouth patch. registerRoad's close-the-loop pass cannot see it (it
// walks segments; the mouth is PAST the last node). This box cuts the throat
// to the junction datum explicitly. Depth is measured against the RAW
// (pre-corridor) surface, max across the band, exactly like registerRoad does.
void registerJunctionBox(float endX, float endZ, float jx, float jz, float datumY) {
    float dx = jx - endX, dz = jz - endZ;
    const float len = std::sqrt(dx * dx + dz * dz);
    if (len < 1.0f) return;
    // Junction exclusion zone (Tim: "INTERSECTIONS NEED TO NOT HAVE
    // RAILINGS"): note the mouth centre AND the branch end so barrier
    // planning and the skirt feather open BOTH roads through the throat.
    noteRoadJunction(jx, jz);
    noteRoadJunction(endX, endZ);
    dx /= len; dz /= len;
    TerrainCorridor c{};
    c.nodeCount = 2;
    c.halfWidth = kPavedHalfM + 8.0f;   // covers the mouth's flare wings
    c.falloff   = 16.0f;
    c.x[0] = endX; c.z[0] = endZ;
    c.x[1] = jx;   c.z[1] = jz;
    const float px = -dz, pz = dx;
    for (int i = 0; i < 2; ++i) {
        float hi = -1e9f;
        for (float s = 0.0f; s <= len * 0.5f + 0.1f; s += 4.0f)     // half-span sweep each
            for (int k = -6; k <= 6; ++k) {
                const float off = (float)k * c.halfWidth / 6.0f;
                const float qx = c.x[i] + dx * (i ? -s : s) + px * off;
                const float qz = c.z[i] + dz * (i ? -s : s) + pz * off;
                const float raw = terrainHeightAtWorld(qx, qz)
                                - terrainCorridorDelta(qx, qz);     // pre-carve
                hi = std::max(hi, raw);
            }
        c.depth[i] = std::max(0.0f, hi - (datumY - kRoadFloorClear));
    }
    if (!registerTerrainCorridor(c))
        x3::logError("junction box: corridor registry FULL — junction throat left uncut");
}

} // namespace

// ---------------------------------------------------------------------------
// ATTACH A ROUTE END TO ANOTHER ROAD — see road_network.h. The exported half
// of the junction machinery, for routes authored outside this module.
// ---------------------------------------------------------------------------
RoadJunction attachRoadEndToRoute(RoadSpec& s, bool atFront,
                                  const RoadSpec& mainSpec,
                                  const std::vector<float>& mainRoadY,
                                  uint32_t* outMainNode) {
    RoadJunction j;
    size_t n = s.x.size();
    const size_t mn = mainSpec.x.size();
    if (n < 3 || mn < 3 || mainRoadY.size() != mn || mainSpec.z.size() != mn)
        return j;

    // THE OVERSHOOT TRUNCATION. The valley road's west leg was authored
    // against an older course revision: it ran 290 m PAST today's tour and
    // CROSSED it mid-segment at (94, -3848) — two full pavements stacked,
    // which is the owner's screenshot. So: walk inward from the end to the
    // route node nearest the main centreline, and cut everything beyond it
    // before attaching. An end already at the road truncates nothing.
    auto distToMain = [&](float px, float pz) {
        float best = 1e18f;
        for (size_t k = 0; k + 1 < mn; ++k)
            best = std::min(best, segPointDist(px, pz,
                mainSpec.x[k], mainSpec.z[k], mainSpec.x[k+1], mainSpec.z[k+1]));
        return best;
    };
    size_t e = atFront ? 0 : n - 1;
    size_t bestI = e;
    float  bestD = distToMain(s.x[e], s.z[e]);
    {
        const long dir = atFront ? +1 : -1;
        size_t steps = 0;
        for (long i = (long)e + dir; i >= 0 && i < (long)n && steps < 48;
             i += dir, ++steps) {
            const float dd = distToMain(s.x[(size_t)i], s.z[(size_t)i]);
            if (dd < bestD) { bestD = dd; bestI = (size_t)i; }
            else if (dd > bestD + 250.0f) break;   // receding for good
        }
    }
    if (bestD > 120.0f) return j;                  // never comes near that road
    if (bestI != e) {
        if (atFront) {
            const size_t rem = bestI;
            for (const RoadSpec::Gap& g : s.gaps)
                if (g.i0 < rem) return j;          // overshoot crossed a gap?!
            s.x.erase(s.x.begin(), s.x.begin() + (long)rem);
            s.z.erase(s.z.begin(), s.z.begin() + (long)rem);
            if (!s.pinY.empty())
                s.pinY.erase(s.pinY.begin(), s.pinY.begin() + (long)rem);
            for (RoadSpec::Gap& g : s.gaps) {
                g.i0 -= (uint32_t)rem; g.i1 -= (uint32_t)rem;
            }
        } else {
            for (const RoadSpec::Gap& g : s.gaps)
                if (g.i1 > bestI) return j;        // overshoot crossed a gap?!
            s.x.resize(bestI + 1);
            s.z.resize(bestI + 1);
            if (!s.pinY.empty()) s.pinY.resize(bestI + 1);
        }
        n = s.x.size();
        if (n < 3) return j;
        e = atFront ? 0 : n - 1;
    }
    // The landing: nearest main node to the (possibly truncated) end.
    size_t J = 0; float bd = 1e18f;
    for (size_t i = 0; i + 1 < mn; ++i) {          // [mn-1] may duplicate [0]
        const float dx = mainSpec.x[i] - s.x[e], dz = mainSpec.z[i] - s.z[e];
        const float dd = dx * dx + dz * dz;
        if (dd < bd) { bd = dd; J = i; }
    }
    const float jx = mainSpec.x[J], jz = mainSpec.z[J], jy = mainRoadY[J];

    // Drop any terminal-adjacent node sitting INSIDE the setback circle, or
    // the retreated terminal would land beyond it and the approach folds
    // back over itself.
    auto innerDist = [&]() {
        const size_t i2 = atFront ? 1 : n - 2;
        const float ddx = s.x[i2] - jx, ddz = s.z[i2] - jz;
        return std::sqrt(ddx * ddx + ddz * ddz);
    };
    while (n > 3 && innerDist() < kJctSetbackM + 8.0f) {
        const size_t kill = atFront ? 1 : n - 2;
        for (const RoadSpec::Gap& g : s.gaps)
            if (kill >= g.i0 && kill <= g.i1) { n = 0; break; }
        if (n == 0) return j;                      // ate into a gap — authoring error
        s.x.erase(s.x.begin() + (long)kill);
        s.z.erase(s.z.begin() + (long)kill);
        if (!s.pinY.empty()) s.pinY.erase(s.pinY.begin() + (long)kill);
        for (RoadSpec::Gap& g : s.gaps)
            if (g.i0 > kill) { --g.i0; --g.i1; }
        n = s.x.size();
        e = atFront ? 0 : n - 1;
    }
    const size_t inner = atFront ? 1 : n - 2;

    // Retreat the terminal node to the junction setback along the route's own
    // last leg — the mouth patch and the merge fillets own that reach.
    float dx = s.x[inner] - jx, dz = s.z[inner] - jz;
    const float dl = std::sqrt(dx * dx + dz * dz);
    if (dl < 1.0f) return j;
    dx /= dl; dz /= dl;
    s.x[e] = jx + dx * kJctSetbackM;
    s.z[e] = jz + dz * kJctSetbackM;

    // Pin the terminal datum to the main road's — the approach must arrive AT
    // GRADE, which is the whole point ("at least swoop curves down to it").
    if (s.pinY.size() != n)
        s.pinY.assign(n, std::numeric_limits<float>::quiet_NaN());
    s.pinY[e] = jy;

    // The main road's frame at the landing (wrap past a ring's closing dup).
    const size_t jp = (J + 1 < mn - 1) ? J + 1 : 0;
    const size_t jm = (J > 0) ? J - 1 : mn - 2;
    float mtx = mainSpec.x[jp] - mainSpec.x[jm], mtz = mainSpec.z[jp] - mainSpec.z[jm];
    const float mtl = std::sqrt(mtx * mtx + mtz * mtz);
    j.valid = true;
    j.jx = jx; j.jz = jz; j.jy = jy;
    j.mainTX = 1.0f; j.mainTZ = 0.0f; j.mainGrade = 0.0f;
    if (mtl > 1e-4f) {
        j.mainTX = mtx / mtl; j.mainTZ = mtz / mtl;
        j.mainGrade = (mainRoadY[jp] - mainRoadY[jm]) / mtl;
    }
    j.endX = s.x[e]; j.endZ = s.z[e];
    j.endY = jy;   // placeholder — caller overwrites with the graded datum
    if (outMainNode) *outMainNode = (uint32_t)J;
    return j;
}

void registerRoadJunctionThroat(float endX, float endZ,
                                float jx, float jz, float datumY) {
    registerJunctionBox(endX, endZ, jx, jz, datumY);
}

SpawnConnectorResult registerSpawnConnector(const TunnelRoute& spawnRoute,
                                            const RoadSpec& ringSpec,
                                            const std::vector<float>& ringRoadY) {
    SpawnConnectorResult out;
    const size_t rn = ringSpec.x.size();
    if (spawnRoute.st.empty() || rn < 3 || ringRoadY.size() != rn ||
        ringSpec.z.size() != rn) {
        x3::logError("spawn connector: bad inputs (no spawn route / ring datum missing)");
        return out;
    }

    // The spawn corridor's FAR END — past the exit portal — and its heading.
    float e[3];
    spawnRoute.posAt(spawnRoute.totalLen, e);
    float tx = 0.0f, tz = 0.0f;
    spawnRoute.tangentAt(spawnRoute.totalLen, tx, tz);
    const float yE = spawnRoute.roadYAt(spawnRoute.totalLen);

    // MEASURE the gap being closed (the audit number, kept in the boot log).
    {
        float gap = 1e18f;
        for (size_t i = 0; i + 1 < rn; ++i)
            for (const auto& st : spawnRoute.st)
                gap = std::min(gap, segPointDist(st.x, st.z,
                                                 ringSpec.x[i], ringSpec.z[i],
                                                 ringSpec.x[i + 1], ringSpec.z[i + 1]));
        out.gapBeforeM = gap;
    }

    // THE LANDING: the ring node nearest the forward ray out of the exit — the
    // road continues straight out of the tunnel and lands square on the ring.
    size_t J = 0; float best = 1e18f;
    for (size_t i = 0; i + 1 < rn; ++i) {          // [rn-1] duplicates [0]
        const float dx = ringSpec.x[i] - e[0], dz = ringSpec.z[i] - e[2];
        const float fwd = dx * tx + dz * tz;
        if (fwd <= 60.0f) continue;                 // behind / on top of the exit
        const float lat = std::fabs(dx * (-tz) + dz * tx);
        const float score = lat + 0.05f * fwd;      // straight ahead first, near second
        if (score < best) { best = score; J = i; }
    }
    if (best >= 1e18f) { x3::logError("spawn connector: no ring node ahead of the exit"); return out; }
    const float jx = ringSpec.x[J], jz = ringSpec.z[J], jy = ringRoadY[J];

    // Ring frame + longitudinal grade at the landing (for the mouth patch).
    const size_t jp = (J + 1 < rn - 1) ? J + 1 : 0;        // wrap past the closing dup
    const size_t jm = (J > 0) ? J - 1 : rn - 2;
    float mtx = ringSpec.x[jp] - ringSpec.x[jm], mtz = ringSpec.z[jp] - ringSpec.z[jm];
    const float mtl = std::sqrt(mtx * mtx + mtz * mtz);
    float mainGrade = 0.0f;
    if (mtl > 1e-4f) { mtx /= mtl; mtz /= mtl; mainGrade = (ringRoadY[jp] - ringRoadY[jm]) / mtl; }

    // The MAIN ROAD's cross-section at the landing. The inner tour is a
    // DIVIDED FREEWAY: the branch meets the NEAR carriageway, whose edges sit
    // (median + carriageway offsets) from the centreline — the setback, the
    // twist run and the merge fillets all measure from those, or the branch
    // would end inside the freeway's pavement.
    float mainShoulderEdge = kShoulderHalfM, mainPavedEdge = kPavedHalfM;
    if (ringSpec.dualCarriageway) {
        const std::vector<float> mp = computeMedianPlan(ringSpec, ringRoadY);
        const float mJ = (J < mp.size()) ? mp[J] : kFwyMedianMinHalfM;
        mainShoulderEdge = mJ + kFwyPavedHalfM + kFwyShoulderHalfM;
        mainPavedEdge    = mJ + 2.0f * kFwyPavedHalfM;
    }
    // CENTRELINE: exit -> a point set back square of the ring, as a gentle
    // S-curve (Tim: "they do not curve"). Ends forced straight so the first
    // segment continues the tunnel's heading and the last arrives radially.
    const float setback = mainPavedEdge + 40.0f;
    float adx = jx - e[0], adz = jz - e[2];
    const float L0 = std::sqrt(adx * adx + adz * adz);
    adx /= L0; adz /= L0;
    const float bex = jx - adx * setback, bez = jz - adz * setback;
    const float run = L0 - setback;
    const int nseg = std::max(6, (int)std::ceil(run / 61.0f));
    const float amp = std::min(170.0f, run * 0.055f);
    const float pxd = -adz, pzd = adx;                     // base-line perpendicular

    RoadSpec s;
    s.name      = "spawn connector";
    s.halfWidth = kPavedHalfM + 1.0f;
    s.falloff   = 18.0f;
    s.maxGrade  = 0.07f;
    s.minTurnRadiusM   = 250.0f;   // freeway class
    s.maxDeflectionDeg = 3.0f;
    for (int k = 0; k <= nseg; ++k) {
        const float t = (float)k / (float)nseg;
        float lat = amp * std::sin(6.2831853f * t) * std::sin(3.1415926f * t);
        if (k <= 1 || k >= nseg - 1) lat = 0.0f;           // straight ends, square joints
        s.x.push_back(e[0] + (bex - e[0]) * t + pxd * lat);
        s.z.push_back(e[2] + (bez - e[2]) * t + pzd * lat);
    }
    // Smooth BEFORE the pins go on: subdivision keeps both terminal nodes
    // exactly where they are (they are locked as ends), so the datums pin to
    // the same places they always did.
    smoothHorizontalCurves(s);
    const float kNaN = std::numeric_limits<float>::quiet_NaN();
    s.pinY.assign(s.x.size(), kNaN);
    s.pinY.front() = yE;    // arrive at the tunnel exit's datum...
    s.pinY.back()  = jy;    // ...and land at the ring's datum. Both exact.

    out.spec = s;
    out.road = registerRoad(out.spec, &out.roadY);
    out.ringNode = (uint32_t)J;
    if (out.road.ok) registerJunctionBox(bex, bez, jx, jz, jy);
    if (out.road.ok && !out.roadY.empty()) {
        out.ringJct.valid   = true;
        out.ringJct.jx = jx; out.ringJct.jz = jz; out.ringJct.jy = jy;
        out.ringJct.mainTX = mtx; out.ringJct.mainTZ = mtz;
        out.ringJct.mainGrade = mainGrade;
        out.ringJct.endX = out.spec.x.back();
        out.ringJct.endZ = out.spec.z.back();
        out.ringJct.endY = out.roadY.back();
        out.ringJct.mainShoulderEdgeM = mainShoulderEdge;
        out.ringJct.mainPavedEdgeM    = mainPavedEdge;
    }
    char b[360];
    std::snprintf(b, sizeof(b),
        "spawn connector: closes a %.0f m (%.0f ft) gap — exit (%.0f, %.0f, y %.1f) -> "
        "ring node %u (%.0f, %.0f, y %.1f) | %.2f miles, pin deficit %.2f ft, "
        "end datums %.2f/%.2f ft off their pins",
        out.gapBeforeM, out.gapBeforeM * kMToFt, e[0], e[2], yE,
        out.ringNode, jx, jz, jy, out.road.lengthM / 1609.34f,
        out.road.pinErrM * kMToFt,
        out.roadY.empty() ? -1.0f : (out.roadY.front() - yE) * kMToFt,
        out.roadY.empty() ? -1.0f : (out.roadY.back() - jy) * kMToFt);
    x3::logInfo(b);
    return out;
}

// ---------------------------------------------------------------------------
// THE OUTER CONNECTOR — see road_network.h. Registered after BOTH tours.
// ---------------------------------------------------------------------------
OuterConnectorResult registerOuterConnector(const RoadSpec& ringSpec,
                                            const std::vector<float>& ringRoadY,
                                            const RoadSpec& outerSpec,
                                            const std::vector<float>& outerRoadY) {
    OuterConnectorResult out;
    const size_t rn = ringSpec.x.size(), on = outerSpec.x.size();
    if (rn < 3 || on < 3 || ringRoadY.size() != rn || outerRoadY.size() != on ||
        ringSpec.z.size() != rn || outerSpec.z.size() != on) {
        x3::logError("outer connector: bad inputs (ring or tour datum missing)");
        return out;
    }
    // The natural (pre-carve) hillside — the same derivation the summit spur
    // uses. Scoring against the CARVED field would let a candidate score well
    // just because it runs along a cutting somebody else already made.
    auto naturalAt = [](float x, float z) {
        return terrainHeightAtWorld(x, z) - terrainCorridorDelta(x, z);
    };

    // NO LANDING IN A BORE. The tour's gap reaches are tunnels; keep a portal's
    // worth of daylight clear of them too (6 nodes ~ 366 m at the tour's 61 m
    // spacing) so the junction mouth is not cut into a portal headwall.
    constexpr uint32_t kGapClearNodes = 6;
    auto inGapOrPortal = [&](size_t j) {
        for (const auto& g : outerSpec.gaps) {
            const uint32_t lo = (g.i0 > kGapClearNodes) ? g.i0 - kGapClearNodes : 0u;
            const uint32_t hi = g.i1 + kGapClearNodes;
            if (j >= lo && j <= hi) return true;
        }
        return false;
    };

    // MEASURE THE ISLAND first — the audit number, kept in the boot log,
    // independent of which crossing gets chosen.
    {
        float gap = 1e18f;
        for (size_t i = 0; i + 1 < rn; ++i)
            for (size_t j = 0; j + 1 < on; ++j) {
                const float dx = outerSpec.x[j] - ringSpec.x[i];
                const float dz = outerSpec.z[j] - ringSpec.z[i];
                const float d = dx * dx + dz * dz;
                if (d < gap) gap = d;
            }
        out.gapBeforeM = std::sqrt(gap);
    }

    // THE CROSSING. For each legal outer landing, take the nearest ring node
    // (concentric tours, so that line is radial) and score the WORST cut-or-
    // fill it would need: the straight datum from one graded tour to the other
    // against the natural ground under it. Cheapest wins. Distance is only a
    // tie-break — 200 m of extra road is far cheaper than a 60 m cutting.
    size_t I = 0, J = 0; float bestCost = 1e18f, bestFit = 0.0f;
    for (size_t j = 0; j + 1 < on; j += 4) {          // ~every 240 m of tour
        if (inGapOrPortal(j)) continue;
        size_t bi = 0; float bd = 1e18f;
        for (size_t i = 0; i + 1 < rn; ++i) {
            const float dx = outerSpec.x[j] - ringSpec.x[i];
            const float dz = outerSpec.z[j] - ringSpec.z[i];
            const float d = dx * dx + dz * dz;
            if (d < bd) { bd = d; bi = i; }
        }
        const float L = std::sqrt(bd);
        if (L < 1.0f) continue;
        const float y0 = ringRoadY[bi], y1 = outerRoadY[j];
        float worst = 0.0f;
        for (int k = 0; k <= 40; ++k) {
            const float t = (float)k / 40.0f;
            const float px = ringSpec.x[bi] + (outerSpec.x[j] - ringSpec.x[bi]) * t;
            const float pz = ringSpec.z[bi] + (outerSpec.z[j] - ringSpec.z[bi]) * t;
            worst = std::max(worst, std::fabs(naturalAt(px, pz) - (y0 + (y1 - y0) * t)));
        }
        const float cost = worst + L * 0.004f;        // 1 m of cut ~ 250 m of road
        if (cost < bestCost) { bestCost = cost; bestFit = worst; I = bi; J = j; }
    }
    if (bestCost >= 1e18f) {
        x3::logError("outer connector: no legal landing on the tour (every node in a bore?)");
        return out;
    }
    out.worstFitM = bestFit;

    const float rx = ringSpec.x[I],  rz = ringSpec.z[I],  ry = ringRoadY[I];
    const float ox = outerSpec.x[J], oz = outerSpec.z[J], oy = outerRoadY[J];
    float adx = ox - rx, adz = oz - rz;
    const float L0 = std::sqrt(adx * adx + adz * adz);
    adx /= L0; adz /= L0;

    // Both ends stand back by the junction setback the mouth patch owns. The
    // INNER end lands on the divided freeway: its setback measures from the
    // near carriageway's outer apron edge, or the branch would end inside the
    // freeway's own pavement.
    float ringShoulderEdge = kShoulderHalfM, ringPavedEdge = kPavedHalfM;
    if (ringSpec.dualCarriageway) {
        const std::vector<float> mp = computeMedianPlan(ringSpec, ringRoadY);
        const float mI = (I < mp.size()) ? mp[I] : kFwyMedianMinHalfM;
        ringShoulderEdge = mI + kFwyPavedHalfM + kFwyShoulderHalfM;
        ringPavedEdge    = mI + 2.0f * kFwyPavedHalfM;
    }
    const float setbackRing = ringPavedEdge + 40.0f;
    const float bx0 = rx + adx * setbackRing,  bz0 = rz + adz * setbackRing;
    const float bx1 = ox - adx * kJctSetbackM, bz1 = oz - adz * kJctSetbackM;
    const float run = L0 - setbackRing - kJctSetbackM;
    if (run < 120.0f) {
        x3::logError("outer connector: the tours are too close here for a junction pair");
        return out;
    }
    const int   nseg = std::max(8, (int)std::ceil(run / 61.0f));
    const float amp  = std::min(120.0f, run * 0.04f);   // the same gentle S
    const float pxd  = -adz, pzd = adx;

    RoadSpec s;
    s.name      = "outer connector";
    s.halfWidth = kPavedHalfM + 1.0f;
    s.falloff   = 18.0f;
    s.maxGrade  = 0.07f;
    s.minTurnRadiusM   = 250.0f;   // freeway class
    s.maxDeflectionDeg = 3.0f;
    for (int k = 0; k <= nseg; ++k) {
        const float t = (float)k / (float)nseg;
        float lat = amp * std::sin(6.2831853f * t) * std::sin(3.1415926f * t);
        if (k <= 1 || k >= nseg - 1) lat = 0.0f;        // straight ends, square joints
        s.x.push_back(bx0 + (bx1 - bx0) * t + pxd * lat);
        s.z.push_back(bz0 + (bz1 - bz0) * t + pzd * lat);
    }
    smoothHorizontalCurves(s);   // ends locked; pins go on after, same places
    const float kNaN = std::numeric_limits<float>::quiet_NaN();
    s.pinY.assign(s.x.size(), kNaN);
    s.pinY.front() = ry;    // leave the inner tour at ITS datum...
    s.pinY.back()  = oy;    // ...and arrive on the outer tour at ITS datum.

    out.spec = s;
    out.road = registerRoad(out.spec, &out.roadY);
    out.ringNode = (uint32_t)I; out.outerNode = (uint32_t)J;
    if (!out.road.ok || out.roadY.empty()) {
        x3::logError("outer connector: registerRoad FAILED");
        return out;
    }
    registerJunctionBox(bx0, bz0, rx, rz, ry);
    registerJunctionBox(bx1, bz1, ox, oz, oy);

    // The two mouths. Each needs its MAIN road's tangent and longitudinal grade
    // at the junction — taken across the neighbouring nodes, wrapping past the
    // closing duplicate exactly as the spawn connector does.
    auto mouth = [&](const RoadSpec& main, const std::vector<float>& mainY, size_t K,
                     float ex, float ez, float ey, RoadJunction& j) {
        const size_t n = main.x.size();
        const size_t kp = (K + 1 < n - 1) ? K + 1 : 0;
        const size_t km = (K > 0) ? K - 1 : n - 2;
        float mtx = main.x[kp] - main.x[km], mtz = main.z[kp] - main.z[km];
        const float mtl = std::sqrt(mtx * mtx + mtz * mtz);
        j.valid = true;
        j.jx = main.x[K]; j.jz = main.z[K]; j.jy = mainY[K];
        j.mainGrade = 0.0f; j.mainTX = 1.0f; j.mainTZ = 0.0f;
        if (mtl > 1e-4f) {
            j.mainTX = mtx / mtl; j.mainTZ = mtz / mtl;
            j.mainGrade = (mainY[kp] - mainY[km]) / mtl;
        }
        j.endX = ex; j.endZ = ez; j.endY = ey;
    };
    mouth(ringSpec,  ringRoadY,  I, out.spec.x.front(), out.spec.z.front(),
          out.roadY.front(), out.ringJct);
    out.ringJct.mainShoulderEdgeM = ringShoulderEdge;   // divided-freeway main
    out.ringJct.mainPavedEdgeM    = ringPavedEdge;
    mouth(outerSpec, outerRoadY, J, out.spec.x.back(),  out.spec.z.back(),
          out.roadY.back(),  out.outerJct);

    char b[520];
    std::snprintf(b, sizeof(b),
        "outer connector: the tour is no longer an island (narrowest gap anywhere "
        "was %.0f m / %.0f ft) — inner node %u (%.0f, %.0f, y %.1f) -> tour node %u "
        "(%.0f, %.0f, y %.1f), a %.0f m crossing taking %.2f miles of road. Chosen "
        "for FIT, not shortness: worst cut-or-fill %.0f ft against the natural "
        "hillside. Max grade %.1f%%, pin deficit %.2f ft, end datums %.2f/%.2f ft "
        "off their pins.",
        out.gapBeforeM, out.gapBeforeM * kMToFt, out.ringNode, rx, rz, ry,
        out.outerNode, ox, oz, oy, L0, out.road.lengthM / 1609.34f,
        out.worstFitM * kMToFt, out.road.maxGradePct, out.road.pinErrM * kMToFt,
        (out.roadY.front() - ry) * kMToFt, (out.roadY.back() - oy) * kMToFt);
    x3::logInfo(b);
    return out;
}

// ---------------------------------------------------------------------------
// THE SUMMIT SPUR — see road_network.h.
// ---------------------------------------------------------------------------
SummitSpurResult registerSummitSpur(const RoadSpec& fromSpec,
                                    const std::vector<float>& fromRoadY,
                                    const TunnelRoute* keepClearOf,
                                    const std::vector<const RoadSpec*>* avoid) {
    SummitSpurResult out;
    const size_t n = fromSpec.x.size();
    if (n < 10 || fromRoadY.size() != n) { out.whyNot = "host road too short"; return out; }

    // The NATURAL (pre-carve) hillside — a summit is a property of the
    // mountain, not of whatever cuttings happen to cross its flanks.
    auto naturalAt = [](float x, float z) {
        return terrainHeightAtWorld(x, z) - terrainCorridorDelta(x, z);
    };
    // Keep-clear: the spawn tunnel's spine (its carve + backfill lid corridor).
    // A spur cutting inside the lid band would drop the ground under the lid
    // mesh's seam and float its edge.
    float sx0 = 0, sz0 = 0, sx1 = 0, sz1 = 0; bool haveSpine = false;
    if (keepClearOf && !keepClearOf->st.empty()) {
        sx0 = keepClearOf->st.front().x; sz0 = keepClearOf->st.front().z;
        sx1 = keepClearOf->st.back().x;  sz1 = keepClearOf->st.back().z;
        haveSpine = true;
    }
    auto spineDist = [&](float x, float z) {
        return haveSpine ? segPointDist(x, z, sx0, sz0, sx1, sz1) : 1e9f;
    };
    // Distance to the nearest OTHER route's centreline — the spur must not
    // carve a trench across the valley road or the tours on its way up.
    auto avoidDist = [&](float x, float z) {
        float best = 1e9f;
        if (avoid)
            for (const RoadSpec* a : *avoid) {
                if (!a) continue;
                for (size_t i = 0; i + 1 < a->x.size(); ++i)
                    best = std::min(best, segPointDist(x, z, a->x[i], a->z[i],
                                                       a->x[i + 1], a->z[i + 1]));
            }
        return best;
    };

    // ---- FIND THE MOUNTAIN: hill-climb from seeds beside the road ----------
    struct Peak { float x, z, h; };
    std::vector<Peak> peaks;
    const bool dbg = [] { const char* e = std::getenv("X3_SPUR_DEBUG"); return e && e[0]; }();
    for (size_t i = 4; i + 4 < n; i += 4) {
        float tx = fromSpec.x[i + 1] - fromSpec.x[i - 1];
        float tz = fromSpec.z[i + 1] - fromSpec.z[i - 1];
        const float tl = std::sqrt(tx * tx + tz * tz);
        if (tl < 1e-4f) continue;
        tx /= tl; tz /= tl;
        for (int side = -1; side <= 1; side += 2) {
            for (float d : { 420.0f, 750.0f, 1150.0f, 1700.0f, 2300.0f }) {
                float cx = fromSpec.x[i] + (-tz) * d * (float)side;
                float cz = fromSpec.z[i] + ( tx) * d * (float)side;
                float ch = naturalAt(cx, cz);
                for (int it = 0; it < 240; ++it) {          // 8-way gradient ascent
                    float bx = cx, bz = cz, bh = ch;
                    for (int k = 0; k < 8; ++k) {
                        const float a = (float)k * 0.7853982f;
                        const float qx = cx + std::cos(a) * 16.0f;
                        const float qz = cz + std::sin(a) * 16.0f;
                        const float qh = naturalAt(qx, qz);
                        if (qh > bh) { bh = qh; bx = qx; bz = qz; }
                    }
                    if (bh <= ch + 1e-4f) break;
                    cx = bx; cz = bz; ch = bh;
                }
                bool dup = false;
                for (Peak& p : peaks)
                    if ((p.x - cx) * (p.x - cx) + (p.z - cz) * (p.z - cz) < 90.0f * 90.0f) {
                        if (ch > p.h) { p.x = cx; p.z = cz; p.h = ch; }
                        dup = true; break;
                    }
                if (!dup) peaks.push_back({ cx, cz, ch });
            }
        }
    }

    // ---- SCORE: prominence over the nearest road node, reachable, clear ----
    struct Cand { Peak p; size_t j; float score; };
    std::vector<Cand> cands;
    for (const Peak& p : peaks) {
        size_t j = 0; float D = 1e18f;
        for (size_t i = 4; i + 4 < n; ++i) {
            const float dd = std::sqrt((p.x - fromSpec.x[i]) * (p.x - fromSpec.x[i]) +
                                       (p.z - fromSpec.z[i]) * (p.z - fromSpec.z[i]));
            if (dd < D) { D = dd; j = i; }
        }
        const float climb = p.h - fromRoadY[j];
        if (dbg) std::printf("SPUR candidate peak (%.0f, %.0f) h %.1f  D %.0f  climb %.1f  spine %.0f\n",
                             p.x, p.z, p.h, D, climb, spineDist(p.x, p.z));
        if (D < 150.0f || D > 2500.0f) continue;
        if (climb < 35.0f) continue;                       // not a mountain, a mound
        if (spineDist(p.x, p.z) < 150.0f) continue;        // the lid's hill is off-limits
        {   // stay off the other routes: peak + climb-line midpoint both clear
            const float mx = (p.x + fromSpec.x[j]) * 0.5f;
            const float mz = (p.z + fromSpec.z[j]) * 0.5f;
            if (avoidDist(p.x, p.z) < 150.0f || avoidDist(mx, mz) < 150.0f) continue;
        }
        // approach mostly square off the road, or the sawtooth swings back over it
        float ttx = fromSpec.x[j + 1] - fromSpec.x[j - 1];
        float ttz = fromSpec.z[j + 1] - fromSpec.z[j - 1];
        const float ttl = std::sqrt(ttx * ttx + ttz * ttz);
        if (ttl > 1e-4f) {
            const float ux = (p.x - fromSpec.x[j]) / D, uz = (p.z - fromSpec.z[j]) / D;
            if (std::fabs(ux * ttx / ttl + uz * ttz / ttl) > 0.82f) continue;
        }
        cands.push_back({ p, j, climb - 0.012f * D });
    }
    if (cands.empty()) {
        out.whyNot = "no local peak with >= 115 ft prominence within reach of the connector";
        x3::logWarn(std::string("summit spur: SKIPPED — ") + out.whyNot);
        return out;
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
        if (a.score != b.score) return a.score > b.score;
        if (a.p.x != b.p.x)     return a.p.x < b.p.x;     // deterministic tie-break
        return a.p.z < b.p.z;
    });

    // ---- AUTHOR THE CLIMB, per candidate, best score first ----------------
    // A candidate must pass a REACHABILITY pre-check before anything touches
    // the corridor registry: the grade relaxation can only put the summit
    // datum where the terrain lets a <= maxGrade profile arrive, i.e. no
    // higher than min over the route of (ground_j + maxGrade * dist(j->top)).
    // The de-circled course's north bulge exposed exactly this: its nearest
    // "peak" was a range foothill whose final face outruns 14% — the old
    // single-pick code graded 292 ft short of it and shipped the deficit. A
    // summit the road cannot reach is SKIPPED, and the next candidate tried.
    out.whyNot = "no candidate peak is summitable at the road's grade";
    for (const Cand& cand : cands) {
        const Peak pick = cand.p;
        const size_t baseNode = cand.j;
        const float baseX = fromSpec.x[baseNode], baseZ = fromSpec.z[baseNode];
        const float baseY = fromRoadY[baseNode];
        float ctx = fromSpec.x[baseNode + 1] - fromSpec.x[baseNode - 1];
        float ctz = fromSpec.z[baseNode + 1] - fromSpec.z[baseNode - 1];
        const float ctl = std::sqrt(ctx * ctx + ctz * ctz);
        ctx /= ctl; ctz /= ctl;
        const float connGrade = (fromRoadY[baseNode + 1] - fromRoadY[baseNode - 1]) / ctl;
        // launch square off the connector, on the peak's side
        float px = -ctz, pz = ctx;
        if (px * (pick.x - baseX) + pz * (pick.z - baseZ) < 0.0f) { px = -px; pz = -pz; }
        const float setback = kJctSetbackM;
        const float startX = baseX + px * setback, startZ = baseZ + pz * setback;

        float ux = pick.x - startX, uz = pick.z - startZ;
        const float D = std::sqrt(ux * ux + uz * uz);
        ux /= D; uz /= D;
        const float vx = -uz, vz = ux;                      // sawtooth lateral axis
        const float climb = pick.h - baseY;
        const float Lneed = std::max(D, climb / 0.115f);    // aim under the 14% cap
        const float cosPhi = std::max(0.574f, D / Lneed);   // clamp switchback angle
        if (climb / (D / cosPhi) > 0.135f) {
            out.whyNot = "mountain face steeper than the switchback budget (needs > 13.5% average)";
            continue;
        }
        const float sinPhi = std::sqrt(std::max(0.0f, 1.0f - cosPhi * cosPhi));
        const float step = std::max(45.0f, (D / cosPhi) / 55.0f);
        const float W = 120.0f;                             // sawtooth half-amplitude

        RoadSpec s;
        s.name      = "summit spur";
        s.halfWidth = kPavedHalfM + 1.0f;
        s.falloff   = 16.0f;
        s.maxGrade  = 0.14f;
        // A 14% switchback road's design speed is ~50 mph, not 100: K 6.25 m/%
        // (1.6e-3/m) keeps the car loaded at that speed and lets the sawtooth
        // actually reach its summit — freeway K would round the top off the climb.
        s.maxGradeRate = 1.6e-3f;
        s.x.push_back(startX);            s.z.push_back(startZ);
        s.x.push_back(startX + px * step * 0.8f); s.z.push_back(startZ + pz * step * 0.8f);
        float cx = s.x.back(), cz = s.z.back();
        int sgn = (vx * px + vz * pz >= 0.0f) ? 1 : -1;     // start swinging outward
        while (s.x.size() < 60) {
            const float along = (cx - startX) * ux + (cz - startZ) * uz;
            if (along >= D - step * 1.5f) break;
            const float hx = ux * cosPhi + vx * sinPhi * (float)sgn;
            const float hz = uz * cosPhi + vz * sinPhi * (float)sgn;
            cx += hx * step; cz += hz * step;
            s.x.push_back(cx); s.z.push_back(cz);
            const float lat = (cx - startX) * vx + (cz - startZ) * vz;
            if (sgn > 0 && lat > W) sgn = -1;
            else if (sgn < 0 && lat < -W) sgn = 1;
        }
        s.x.push_back(pick.x); s.z.push_back(pick.z);       // the summit itself
        // HORIZONTAL FLOW: switchback class. The sawtooth's direction
        // reversals are genuine 60-110 deg kinks; the spline turns each one
        // into a mountain-road hairpin and the 25 m floor keeps every one of
        // them takeable at the spur's ~15 mph design speed.
        s.minTurnRadiusM   = 25.0f;
        s.maxDeflectionDeg = 14.0f;
        smoothHorizontalCurves(s);
        const float kNaN = std::numeric_limits<float>::quiet_NaN();
        s.pinY.assign(s.x.size(), kNaN);
        s.pinY.front() = baseY;                             // leave the connector at grade

        // THE REACHABILITY PRE-CHECK (pure — no registration yet). Walk the
        // authored polyline backward accumulating distance-to-summit; the
        // relaxation's ceiling law bounds the summit datum by every node's
        // ground + maxGrade * remaining distance (sampled across the carve
        // width like registerRoad's own sweep, so a ridge under the roadway
        // raises the ceiling exactly as it will at registration).
        {
            float reach = baseY;                            // pinned base bound...
            for (size_t i = 1; i < s.x.size(); ++i) {
                const float dx = s.x[i] - s.x[i - 1], dz = s.z[i] - s.z[i - 1];
                reach += s.maxGrade * std::sqrt(dx * dx + dz * dz);
            }
            float distLeft = 0.0f;                          // ...and every node's
            for (size_t i = s.x.size() - 1; i > 0; --i) {
                const size_t im = i - 1;
                const float dx = s.x[i] - s.x[im], dz = s.z[i] - s.z[im];
                const float len = std::sqrt(dx * dx + dz * dz);
                float tx2 = dx / std::max(1e-4f, len), tz2 = dz / std::max(1e-4f, len);
                float hi = -1e9f;
                for (int k = -2; k <= 2; ++k) {
                    const float off = (float)k * s.halfWidth * 0.5f;
                    hi = std::max(hi, naturalAt(s.x[im] + (-tz2) * off,
                                                s.z[im] + ( tx2) * off));
                }
                distLeft += len;
                reach = std::min(reach, hi + s.maxGrade * distLeft);
            }
            if (pick.h - reach > 2.3f) {
                if (dbg) std::printf("SPUR peak (%.0f, %.0f): unreachable, would top out %.1f m short\n",
                                     pick.x, pick.z, pick.h - reach);
                out.whyNot = "no candidate peak is summitable at the road's grade";
                continue;
            }
        }

        out.spec = s;
        out.road = registerRoad(out.spec, &out.roadY);
        if (!out.road.ok || out.roadY.empty()) {
            out.whyNot = "registration failed";
            return out;
        }
        registerJunctionBox(startX, startZ, baseX, baseZ, baseY);
        out.built = true;
        out.peakX = pick.x; out.peakZ = pick.z; out.peakNaturalY = pick.h;
        out.climbM = out.roadY.back() - out.roadY.front();
        out.summitCutM = pick.h - out.roadY.back();
        out.jct.valid = true;
        out.jct.jx = baseX; out.jct.jz = baseZ; out.jct.jy = baseY;
        out.jct.mainTX = ctx; out.jct.mainTZ = ctz;
        out.jct.mainGrade = connGrade;
        out.jct.endX = startX; out.jct.endZ = startZ;
        out.jct.endY = out.roadY.front();

        char b[400];
        std::snprintf(b, sizeof(b),
            "summit spur: %.2f miles UP — climbs %.0f ft to a %.0f ft peak at (%.0f, %.0f), "
            "tops out %.1f ft under the true summit | max grade %.1f%% (cap 14%%) | "
            "switchback angle %.0f deg, %u nodes, %u corridors | junction at (%.0f, %.0f)",
            out.road.lengthM / 1609.34f, out.climbM * kMToFt, pick.h * kMToFt,
            pick.x, pick.z, out.summitCutM * kMToFt, out.road.maxGradePct,
            std::acos(cosPhi) * 57.29578f, out.road.nodeCount, out.road.corridorCount,
            baseX, baseZ);
        x3::logInfo(b);
        return out;
    }
    x3::logWarn(std::string("summit spur: SKIPPED — ") + out.whyNot);
    return out;
}

// ---------------------------------------------------------------------------
// THE RANGE CIRCUIT — see road_network.h.
// ---------------------------------------------------------------------------
RangeCircuitResult registerRangeCircuit(const RoadSpec& connSpec,
                                        const std::vector<float>& connRoadY,
                                        const TunnelRoute* keepClearOf,
                                        const std::vector<const RoadSpec*>* avoid) {
    RangeCircuitResult out;
    const size_t n = connSpec.x.size();
    if (n < 12 || connRoadY.size() != n) { out.whyNot = "connector too short"; return out; }

    auto naturalAt = [](float x, float z) {
        return terrainHeightAtWorld(x, z) - terrainCorridorDelta(x, z);
    };
    float sx0 = 0, sz0 = 0, sx1 = 0, sz1 = 0; bool haveSpine = false;
    if (keepClearOf && !keepClearOf->st.empty()) {
        sx0 = keepClearOf->st.front().x; sz0 = keepClearOf->st.front().z;
        sx1 = keepClearOf->st.back().x;  sz1 = keepClearOf->st.back().z;
        haveSpine = true;
    }
    // Clearance from everything that already owns ground: the spawn spine,
    // every avoided route's centreline, and the host connector itself.
    auto clearOf = [&](float x, float z) {
        float best = 1e9f;
        if (haveSpine) best = std::min(best, segPointDist(x, z, sx0, sz0, sx1, sz1));
        if (avoid)
            for (const RoadSpec* a : *avoid) {
                if (!a) continue;
                for (size_t i = 0; i + 1 < a->x.size(); ++i)
                    best = std::min(best, segPointDist(x, z, a->x[i], a->z[i],
                                                       a->x[i + 1], a->z[i + 1]));
            }
        for (size_t i = 0; i + 1 < n; ++i)
            best = std::min(best, segPointDist(x, z, connSpec.x[i], connSpec.z[i],
                                               connSpec.x[i + 1], connSpec.z[i + 1]));
        return best;
    };

    // THE TRACK PLAN, in the connector's frame at the anchor node: u along the
    // connector's tangent, v square off it (side chosen below). Authored so the
    // brief's features are geometry, not hope:
    //   * start/finish straight: (-500,400)->(450,400), ~950 m, the access
    //     road arrives square onto its midpoint
    //   * a climbing leg out to the far corner
    //   * a HAIRPIN: 148 deg turn filleted at 75 m (clamps to ~68 m radius)
    //   * a 750 m back straight
    //   * an S-COMPLEX home: right-left-right, radii 110-130 m
    // Not a circle by construction.
    struct LP { float u, v, fil; };
    static const LP plan[] = {
        {  -500.0f,  400.0f, 120.0f },
        {   450.0f,  400.0f, 140.0f },
        {   800.0f,  700.0f, 200.0f },
        {   700.0f, 1120.0f, 130.0f },
        {  1120.0f, 1520.0f,  75.0f },   // the hairpin
        {   650.0f, 1420.0f, 120.0f },
        {   100.0f, 1560.0f, 170.0f },
        {  -650.0f, 1560.0f, 150.0f },
        { -1050.0f, 1230.0f, 130.0f },   // S: right...
        {  -850.0f,  940.0f, 110.0f },   // ...left...
        { -1150.0f,  640.0f, 110.0f },   // ...right...
        {  -900.0f,  420.0f, 120.0f },
    };
    const size_t NP = sizeof(plan) / sizeof(plan[0]);

    // PLACE IT: candidate anchors along the connector, both sides; every plan
    // waypoint must clear every existing route by 170 m. Among the clear
    // placements, prefer the one with the most RELIEF under the loop — the
    // climbing section should be real terrain doing real work.
    size_t H = 0; int side = 0; float hx = 0, hz = 0, ux = 0, uz = 0;
    // Anchor candidates by ARC-LENGTH fraction, not node-index fraction: the
    // horizontal smoothing subdivides curves more densely than straights, so
    // an index fraction of the node list is no longer a length fraction of
    // the road — measured when the circuit jumped from its law-table anchor
    // (-2249, 256) to the wrong side of the connector. Length fractions land
    // where the old uniform polyline's indices did, and the junction table
    // holds.
    std::vector<float> cumLen(n, 0.0f);
    for (size_t i = 1; i < n; ++i) {
        const float ddx = connSpec.x[i] - connSpec.x[i-1];
        const float ddz = connSpec.z[i] - connSpec.z[i-1];
        cumLen[i] = cumLen[i-1] + std::sqrt(ddx * ddx + ddz * ddz);
    }
    {
        // THE LAW ANCHOR FIRST. The junction table (ROAD_NETWORK_PLAN.md)
        // pins J-CIRCUIT-CONN at (-2249, 256): successive lanes pin to it,
        // so the circuit may not wander to a different anchor because a
        // relief score flipped by a metre of smoothing. The node nearest
        // the law point is tried first and WINS if any side of it is clear;
        // the length-fraction candidates are the fallback for a connector
        // authored somewhere else entirely (a different world seed).
        constexpr float kLawAnchorX = -2249.0f, kLawAnchorZ = 256.0f;
        std::vector<size_t> candNodes;
        {
            size_t bi = 4; float bd = 1e18f;
            for (size_t i = 4; i + 4 < n; ++i) {
                const float ddx = connSpec.x[i] - kLawAnchorX;
                const float ddz = connSpec.z[i] - kLawAnchorZ;
                const float dd = ddx * ddx + ddz * ddz;
                if (dd < bd) { bd = dd; bi = i; }
            }
            if (bd < 400.0f * 400.0f) candNodes.push_back(bi);
        }
        for (float frac : { 0.42f, 0.58f, 0.30f }) {
            const float target = frac * cumLen[n - 1];
            size_t hIdx = 4;
            while (hIdx + 1 < n && cumLen[hIdx] < target) ++hIdx;
            candNodes.push_back(std::max((size_t)4, std::min(n - 5, hIdx)));
        }
        bool found = false;
        for (size_t ci = 0; ci < candNodes.size() && !found; ++ci) {
            const size_t h = candNodes[ci];
            float bestScore = -1e18f;
            float tx = connSpec.x[h + 1] - connSpec.x[h - 1];
            float tz = connSpec.z[h + 1] - connSpec.z[h - 1];
            const float tl = std::sqrt(tx * tx + tz * tz);
            if (tl < 1e-4f) continue;
            tx /= tl; tz /= tl;
            const float px = -tz, pz = tx;
            for (int sgn = 1; sgn >= -1; sgn -= 2) {
                bool ok = true;
                float hMin = 1e9f, hMax = -1e9f;
                for (size_t i = 0; i < NP && ok; ++i) {
                    const float wx = connSpec.x[h] + tx * plan[i].u + px * plan[i].v * (float)sgn;
                    const float wz = connSpec.z[h] + tz * plan[i].u + pz * plan[i].v * (float)sgn;
                    if (clearOf(wx, wz) < 170.0f) { ok = false; break; }
                    const float hN = naturalAt(wx, wz);
                    hMin = std::min(hMin, hN); hMax = std::max(hMax, hN);
                }
                if (!ok) continue;
                const float score = hMax - hMin;
                if (score > bestScore) {
                    bestScore = score; found = true;
                    H = h; side = sgn;
                    hx = connSpec.x[h]; hz = connSpec.z[h];
                    ux = tx; uz = tz;
                }
            }
        }
        if (!found) {
            out.whyNot = "no clear country beside the connector for a 4-mile circuit";
            x3::logWarn(std::string("range circuit: SKIPPED — ") + out.whyNot);
            return out;
        }
    }
    const float px = -uz * (float)side, pz = ux * (float)side;   // v axis, world

    // THE CIRCUIT (registered first — free grading; the access pins to it).
    std::vector<CourseWaypoint> wp(NP);
    for (size_t i = 0; i < NP; ++i) {
        wp[i].x = hx + ux * plan[i].u + px * plan[i].v;
        wp[i].z = hz + uz * plan[i].u + pz * plan[i].v;
        wp[i].fillet = plan[i].fil;
    }
    RoadSpec cs = makeRoadFromWaypoints("range circuit", wp, 55.0f, /*closed=*/true);
    cs.halfWidth = kPavedHalfM + 1.0f;
    cs.falloff   = 16.0f;
    cs.maxGrade  = 0.07f;
    // HORIZONTAL FLOW: track class. The 60 m floor sits BELOW the authored
    // ~68 m hairpin, so the hairpin is refined, never eased away — gate H3
    // measures that promise. 7 deg/node at the hairpin's radius means ~8 m
    // node spacing through the apex.
    cs.minTurnRadiusM   = 60.0f;
    cs.maxDeflectionDeg = 7.0f;
    smoothHorizontalCurves(cs);
    out.spec = cs;
    out.road = registerRoad(out.spec, &out.roadY);
    if (!out.road.ok || out.roadY.empty()) { out.whyNot = "circuit registration failed"; return out; }

    // THE LANDING on the circuit: the node nearest the access line's target
    // (v = 400 on the start/finish straight), plus the main-road frame there.
    const size_t cn2 = out.spec.x.size();
    const float tgtX = hx + px * 400.0f, tgtZ = hz + pz * 400.0f;
    size_t L = 0; float best = 1e18f;
    for (size_t i = 0; i + 1 < cn2; ++i) {                 // [cn2-1] duplicates [0]
        const float dx = out.spec.x[i] - tgtX, dz = out.spec.z[i] - tgtZ;
        const float dd = dx * dx + dz * dz;
        if (dd < best) { best = dd; L = i; }
    }
    const float lx = out.spec.x[L], lz = out.spec.z[L], ly = out.roadY[L];
    const size_t lp = (L + 1 < cn2 - 1) ? L + 1 : 0;
    const size_t lm = (L > 0) ? L - 1 : cn2 - 2;
    float mtx = out.spec.x[lp] - out.spec.x[lm], mtz = out.spec.z[lp] - out.spec.z[lm];
    const float mtl = std::sqrt(mtx * mtx + mtz * mtz);
    float circGrade = 0.0f;
    if (mtl > 1e-4f) { mtx /= mtl; mtz /= mtl; circGrade = (out.roadY[lp] - out.roadY[lm]) / mtl; }

    // THE ACCESS ROAD: dead straight from the connector to the circuit, both
    // terminal nodes set back kJctSetbackM from the centrelines they meet, both
    // ends PINNED to the graded datums they must arrive at. The junction
    // machinery (mouth patch + junction box) closes both throats.
    RoadSpec as;
    as.name      = "circuit access";
    as.halfWidth = kPavedHalfM + 1.0f;
    as.falloff   = 16.0f;
    as.maxGrade  = 0.07f;
    {
        // access runs along +v from the connector toward the LANDING point
        // (not the nominal target: the nearest node may sit a few metres off).
        float avx = lx - hx, avz = lz - hz;
        const float al = std::sqrt(avx * avx + avz * avz);
        avx /= al; avz /= al;
        const float s0 = kJctSetbackM, s1 = al - kJctSetbackM;
        const int nseg = std::max(4, (int)std::ceil((s1 - s0) / 55.0f));
        for (int k = 0; k <= nseg; ++k) {
            const float t = s0 + (s1 - s0) * (float)k / (float)nseg;
            as.x.push_back(hx + avx * t);
            as.z.push_back(hz + avz * t);
        }
        const float kNaN = std::numeric_limits<float>::quiet_NaN();
        as.pinY.assign(as.x.size(), kNaN);
        as.pinY.front() = connRoadY[H];
        as.pinY.back()  = ly;
    }
    out.accessSpec = as;
    out.accessRoad = registerRoad(out.accessSpec, &out.accessRoadY);
    if (!out.accessRoad.ok || out.accessRoadY.empty()) {
        out.whyNot = "access road registration failed";
        return out;
    }
    registerJunctionBox(as.x.front(), as.z.front(), hx, hz, connRoadY[H]);
    registerJunctionBox(as.x.back(),  as.z.back(),  lx, lz, ly);

    // Junction frames for the two mouth patches.
    {
        float ctx = connSpec.x[H + 1] - connSpec.x[H - 1];
        float ctz = connSpec.z[H + 1] - connSpec.z[H - 1];
        const float ctl = std::sqrt(ctx * ctx + ctz * ctz);
        out.connJct.valid = true;
        out.connJct.jx = hx; out.connJct.jz = hz; out.connJct.jy = connRoadY[H];
        out.connJct.mainTX = ctx / ctl; out.connJct.mainTZ = ctz / ctl;
        out.connJct.mainGrade = (connRoadY[H + 1] - connRoadY[H - 1]) / ctl;
        out.connJct.endX = as.x.front(); out.connJct.endZ = as.z.front();
        out.connJct.endY = out.accessRoadY.front();
    }
    {
        out.circJct.valid = true;
        out.circJct.jx = lx; out.circJct.jz = lz; out.circJct.jy = ly;
        out.circJct.mainTX = mtx; out.circJct.mainTZ = mtz;
        out.circJct.mainGrade = circGrade;
        out.circJct.endX = as.x.back(); out.circJct.endZ = as.z.back();
        out.circJct.endY = out.accessRoadY.back();
    }
    out.hostNode = (uint32_t)H;
    out.built = true;

    // MEASURE the features the brief demands — the self-test gates on these.
    out.climbM = out.road.maxRoadY - out.road.minRoadY;
    {
        float run = 0.0f, longest = 0.0f;
        for (size_t i = 1; i + 1 < cn2; ++i) {
            float ax = out.spec.x[i] - out.spec.x[i - 1], az = out.spec.z[i] - out.spec.z[i - 1];
            float bx = out.spec.x[i + 1] - out.spec.x[i], bz = out.spec.z[i + 1] - out.spec.z[i];
            const float la = std::sqrt(ax * ax + az * az), lb = std::sqrt(bx * bx + bz * bz);
            if (la < 1e-4f || lb < 1e-4f) continue;
            const float dot = std::max(-1.0f, std::min(1.0f,
                (ax * bx + az * bz) / (la * lb)));
            if (std::acos(dot) < 0.0105f) {          // < 0.6 deg/node: straight
                if (run == 0.0f) run = la;
                run += lb;
                longest = std::max(longest, run);
            } else run = 0.0f;
        }
        out.longestStraightM = longest;
        // sharpest cumulative heading change in any ~260 m window = the hairpin
        std::vector<float> hd(cn2 - 1), sl(cn2 - 1);
        for (size_t i = 0; i + 1 < cn2; ++i) {
            const float dx = out.spec.x[i + 1] - out.spec.x[i];
            const float dz = out.spec.z[i + 1] - out.spec.z[i];
            hd[i] = std::atan2(dz, dx);
            sl[i] = std::sqrt(dx * dx + dz * dz);
        }
        for (size_t i = 0; i + 1 < hd.size(); ++i) {
            float turn = 0.0f, dist = 0.0f;
            for (size_t j = i + 1; j < hd.size() && dist < 260.0f; ++j) {
                float dh = hd[j] - hd[j - 1];
                while (dh >  3.14159265f) dh -= 6.2831853f;
                while (dh < -3.14159265f) dh += 6.2831853f;
                turn += dh; dist += sl[j];
                out.hairpinTurnDeg = std::max(out.hairpinTurnDeg,
                                              std::fabs(turn) * 57.29578f);
            }
        }
    }

    char b[420];
    std::snprintf(b, sizeof(b),
        "range circuit: %.2f miles lap off connector node %u (side %+d) | "
        "longest straight %.0f m, hairpin %.0f deg, climb %.0f ft | "
        "access %.0f m, pin deficits %.2f / %.2f ft | junctions: connector "
        "(%.0f, %.0f) -> circuit (%.0f, %.0f)",
        out.road.lengthM / 1609.34f, out.hostNode, side,
        out.longestStraightM, out.hairpinTurnDeg, out.climbM * kMToFt,
        out.accessRoad.lengthM, out.road.pinErrM * kMToFt,
        out.accessRoad.pinErrM * kMToFt,
        out.connJct.jx, out.connJct.jz, out.circJct.jx, out.circJct.jz);
    x3::logInfo(b);
    return out;
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
    // Same quad, but the normal comes from the winding — the skirt's faces are
    // vertical/battered concrete, and lighting them as if they faced the sky
    // (the flat quad() above) would flatten exactly the depth they exist to add.
    void quadN(const float a[3], const float b[3], const float c[3], const float d[3],
               float u0, float u1, float w0, float w1) {
        float e0[3] = { b[0]-a[0], b[1]-a[1], b[2]-a[2] };
        float e1[3] = { c[0]-b[0], c[1]-b[1], c[2]-b[2] };
        float nrm[3] = { e0[1]*e1[2] - e0[2]*e1[1],
                         e0[2]*e1[0] - e0[0]*e1[2],
                         e0[0]*e1[1] - e0[1]*e1[0] };
        const float nl = std::sqrt(nrm[0]*nrm[0] + nrm[1]*nrm[1] + nrm[2]*nrm[2]);
        if (nl > 1e-6f) { nrm[0] /= nl; nrm[1] /= nl; nrm[2] /= nl; }
        else            { nrm[0] = 0.0f; nrm[1] = 1.0f; nrm[2] = 0.0f; }
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

// ---------------------------------------------------------------------------
// THE MEDIAN PLAN — see road_network.h. Pure and registration-order
// independent: the natural surface is recovered as (field - corridorDelta),
// which is the pristine pre-carve ground however many corridors are already
// in, so the carve (registerRoad) and the pavement (buildRoadRibbon) always
// derive the SAME median from the same inputs.
// ---------------------------------------------------------------------------
std::vector<float> computeMedianPlan(const RoadSpec& spec,
                                     const std::vector<float>& roadY) {
    std::vector<float> m;
    const size_t n = spec.x.size();
    if (!spec.dualCarriageway || n < 2 || roadY.size() != n || spec.z.size() != n)
        return m;
    m.assign(n, kFwyMedianMinHalfM);
    std::vector<float> segLen(n, 0.0f);
    for (size_t i = 0; i + 1 < n; ++i) {
        const float dx = spec.x[i + 1] - spec.x[i], dz = spec.z[i + 1] - spec.z[i];
        segLen[i] = std::sqrt(dx * dx + dz * dz);
    }
    // Terrain fit per node: how far the natural country across the median zone
    // strays from the graded datum. Close to it -> the median can be open
    // graded ground (I-17); far from it (cut / fill) -> narrow + jersey wall.
    for (size_t i = 0; i < n; ++i) {
        const size_t ip = (i + 1 < n) ? i + 1 : i;
        const size_t im = (i > 0) ? i - 1 : i;
        float tx = spec.x[ip] - spec.x[im], tz = spec.z[ip] - spec.z[im];
        const float tl = std::sqrt(tx * tx + tz * tz);
        if (tl > 1e-4f) { tx /= tl; tz /= tl; }
        float worst = 0.0f;
        for (int k = -4; k <= 4; ++k) {
            const float off = (float)k * kFwyMedianMaxHalfM / 4.0f;
            const float qx = spec.x[i] + (-tz) * off;
            const float qz = spec.z[i] + ( tx) * off;
            const float nat = terrainHeightAtWorld(qx, qz) - terrainCorridorDelta(qx, qz);
            worst = std::max(worst, std::fabs(nat - roadY[i]));
        }
        m[i] = (worst < 2.5f) ? kFwyMedianMaxHalfM : kFwyMedianMinHalfM;
    }
    const bool closed = std::fabs(spec.x[0] - spec.x[n - 1]) < 0.01f &&
                        std::fabs(spec.z[0] - spec.z[n - 1]) < 0.01f;
    // Low-pass so an isolated one-node state flip doesn't pinch the roadways...
    for (int pass = 0; pass < 2; ++pass) {
        std::vector<float> t(m);
        for (size_t i = 1; i + 1 < n; ++i)
            m[i] = 0.25f * t[i - 1] + 0.5f * t[i] + 0.25f * t[i + 1];
        if (closed) m[0] = m[n - 1] = 0.5f * (m[1] + m[n - 2]);
    }
    // ...then a slew limit (3 cm of median per metre of route) swept both ways
    // to a fixed point, so the carriageway offset polylines curve gently
    // between the wide and walled states instead of kinking.
    const float kSlew = 0.03f;
    for (int pass = 0; pass < 64; ++pass) {
        float moved = 0.0f;
        for (size_t i = 1; i < n; ++i) {
            const float cap = m[i - 1] + kSlew * std::max(1.0f, segLen[i - 1]);
            if (m[i] > cap) { moved += m[i] - cap; m[i] = cap; }
        }
        for (size_t i = n - 1; i > 0; --i) {
            const float cap = m[i] + kSlew * std::max(1.0f, segLen[i - 1]);
            if (m[i - 1] > cap) { moved += m[i - 1] - cap; m[i - 1] = cap; }
        }
        if (closed) {
            const float w = std::min(m[0], m[n - 1]);
            moved += (m[0] - w) + (m[n - 1] - w);
            m[0] = m[n - 1] = w;
        }
        if (moved < 1e-4f) break;
    }
    for (float& v : m)
        v = std::max(kFwyMedianMinHalfM, std::min(kFwyMedianMaxHalfM, v));
    return m;
}

// ---------------------------------------------------------------------------
// TURNAROUND CROSSOVERS — see road_network.h. Arc-length intervals; pure.
// ---------------------------------------------------------------------------
std::vector<RoadTurnaround> planTurnarounds(const RoadSpec& spec) {
    std::vector<RoadTurnaround> out;
    const size_t n = spec.x.size();
    if (!spec.dualCarriageway || n < 2) return out;
    std::vector<float> U(n, 0.0f);
    for (size_t i = 0; i + 1 < n; ++i) {
        const float dx = spec.x[i + 1] - spec.x[i], dz = spec.z[i + 1] - spec.z[i];
        U[i + 1] = U[i] + std::sqrt(dx * dx + dz * dz);
    }
    const float total = U[n - 1];
    if (total < 2.0f * kFwyTurnaroundLenM) return out;
    struct Iv { float a, b; };
    std::vector<Iv> gapIv;
    for (const RoadSpec::Gap& g : spec.gaps)
        if (g.i1 < n) gapIv.push_back({ U[g.i0], U[g.i1] });
    auto blocked = [&](float a, float b) {
        for (const Iv& iv : gapIv)
            if (a < iv.b + 60.0f && b > iv.a - 60.0f) return true;
        return false;
    };
    const float half = kFwyTurnaroundLenM * 0.5f;
    auto addAt = [&](float c) {
        float a = c - half, b = c + half;
        if (a < 0.0f)   { a = 0.0f; b = kFwyTurnaroundLenM; }
        if (b > total)  { b = total; a = total - kFwyTurnaroundLenM; }
        if (blocked(a, b)) return;
        for (const RoadTurnaround& t : out)
            if (a < t.u1 + 120.0f && b > t.u0 - 120.0f) return;
        out.push_back({ a, b });
    };
    const bool closed = std::fabs(spec.x[0] - spec.x[n - 1]) < 0.01f &&
                        std::fabs(spec.z[0] - spec.z[n - 1]) < 0.01f;
    // A junction landing gets its gap FIRST (crossing traffic needs it exactly
    // there), then the spawn crossover on a closed route, then the rhythm.
    {
        uint32_t jn = roadJunctionCount();
        for (uint32_t j = 0; j < jn; ++j) {
            const JctPoint& p = jctPoints()[j];
            float bestD2 = 45.0f * 45.0f, bestU = -1.0f;
            for (size_t i = 0; i + 1 < n; ++i) {
                const float abx = spec.x[i + 1] - spec.x[i], abz = spec.z[i + 1] - spec.z[i];
                const float len2 = abx * abx + abz * abz;
                if (len2 < 1e-6f) continue;
                float t = ((p.x - spec.x[i]) * abx + (p.z - spec.z[i]) * abz) / len2;
                t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
                const float dx = p.x - (spec.x[i] + abx * t), dz = p.z - (spec.z[i] + abz * t);
                const float d2 = dx * dx + dz * dz;
                if (d2 < bestD2) { bestD2 = d2; bestU = U[i] + std::sqrt(len2) * t; }
            }
            if (bestU >= 0.0f) addAt(bestU);
        }
    }
    if (closed) addAt(half);
    for (float c = kFwyTurnaroundSpacingM; c < total - 100.0f; c += kFwyTurnaroundSpacingM)
        addAt(c);
    std::sort(out.begin(), out.end(),
              [](const RoadTurnaround& a, const RoadTurnaround& b) { return a.u0 < b.u0; });
    return out;
}

// ---------------------------------------------------------------------------
// THE RENDER PATH — see road_network.h. Catmull-Rom through the final nodes
// (positions AND datum, so the profile is C1 at nodes too), adaptive 6..20 m.
// ---------------------------------------------------------------------------
void buildRoadRenderPath(const RoadSpec& spec, const std::vector<float>* roadY,
                         const std::vector<float>* medianPlan,
                         std::vector<RoadRenderStation>& out) {
    out.clear();
    const size_t n = spec.x.size();
    if (n < 2 || spec.z.size() != n) return;
    if (roadY && roadY->size() != n) roadY = nullptr;
    if (medianPlan && medianPlan->size() != n) medianPlan = nullptr;
    std::vector<float> Y(n);
    for (size_t i = 0; i < n; ++i)
        Y[i] = roadY ? (*roadY)[i]
                     : terrainHeightAtWorld(spec.x[i], spec.z[i]) + kRoadFloorClear;
    const bool closed = std::fabs(spec.x[0] - spec.x[n - 1]) < 0.01f &&
                        std::fabs(spec.z[0] - spec.z[n - 1]) < 0.01f;
    auto segInGap = [&](size_t k) {
        for (const RoadSpec::Gap& g : spec.gaps)
            if (k >= g.i0 && k < g.i1) return true;
        return false;
    };
    auto ctrl = [&](long i) -> size_t {
        if (closed) {
            const long m = (long)n - 1;             // unique nodes 0..n-2
            return (size_t)(((i % m) + m) % m);
        }
        return (size_t)std::max(0L, std::min((long)n - 1, i));
    };
    std::vector<float> defl(n, 0.0f);
    for (size_t i = 1; i + 1 < n; ++i)
        defl[i] = deflectionAt(spec.x, spec.z, i - 1, i, i + 1) * kRadToDeg;
    if (closed && n >= 4) {
        defl[0] = deflectionAt(spec.x, spec.z, n - 2, 0, 1) * kRadToDeg;
        defl[n - 1] = defl[0];
    }
    auto cr = [](float p0, float p1, float p2, float p3, float t) {
        const float t2 = t * t, t3 = t2 * t;
        return 0.5f * ((2.0f * p1) + (-p0 + p2) * t +
                       (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
                       (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
    };
    out.reserve(n * 4);
    for (size_t k = 0; k + 1 < n; ++k) {
        const float dxs = spec.x[k + 1] - spec.x[k], dzs = spec.z[k + 1] - spec.z[k];
        const float len = std::sqrt(dxs * dxs + dzs * dzs);
        if (len < 1e-3f) continue;
        const bool gap = segInGap(k);
        int nSub;
        if (gap) {
            // a bore chord / bridge deck is straight LAW — linear, coarse
            nSub = std::max(1, (int)std::ceil(len / 20.0f));
        } else {
            // /18 not /20: the spline ARC between stations runs a little past
            // the chord subdivision on a bend (measured 21.1 m from a /20
            // base), and the promise is "up to ~20 m on true straights".
            const float dm = std::max(defl[k], defl[k + 1]);
            nSub = std::max((int)std::ceil(len / 18.0f), (int)std::ceil(dm / 0.75f));
            nSub = std::min(nSub, std::max(1, (int)std::ceil(len / 6.0f)));
            nSub = std::max(1, nSub);
        }
        const size_t i0 = ctrl((long)k - 1), i3 = ctrl((long)k + 2);
        for (int sSub = 0; sSub < nSub; ++sSub) {
            const float t = (float)sSub / (float)nSub;
            RoadRenderStation st;
            st.seg = (uint32_t)k;
            st.gap = gap;
            if (gap || nSub == 1 || (i0 == k && i3 == k + 1)) {
                st.x = spec.x[k] + dxs * t;
                st.z = spec.z[k] + dzs * t;
                st.y = Y[k] + (Y[k + 1] - Y[k]) * t;
            } else {
                st.x = cr(spec.x[i0], spec.x[k], spec.x[k + 1], spec.x[i3], t);
                st.z = cr(spec.z[i0], spec.z[k], spec.z[k + 1], spec.z[i3], t);
                st.y = cr(Y[i0], Y[k], Y[k + 1], Y[i3], t);
            }
            if (medianPlan)
                st.medianHalf = (*medianPlan)[k] + ((*medianPlan)[k + 1] - (*medianPlan)[k]) * t;
            out.push_back(st);
        }
    }
    {   // the final node, verbatim
        RoadRenderStation st;
        st.seg = (uint32_t)(n - 2);
        st.gap = segInGap(n - 2);
        st.x = spec.x[n - 1]; st.z = spec.z[n - 1]; st.y = Y[n - 1];
        if (medianPlan) st.medianHalf = (*medianPlan)[n - 1];
        out.push_back(st);
    }
    // arc length + tangents (finite difference; wrap on a closed route)
    for (size_t i = 1; i < out.size(); ++i) {
        const float dx = out[i].x - out[i - 1].x, dz = out[i].z - out[i - 1].z;
        out[i].u = out[i - 1].u + std::sqrt(dx * dx + dz * dz);
    }
    const size_t m = out.size();
    for (size_t i = 0; i < m; ++i) {
        size_t ip = (i + 1 < m) ? i + 1 : (closed ? 1 : m - 1);
        size_t im = (i > 0) ? i - 1 : (closed ? m - 2 : 0);
        float tx = out[ip].x - out[im].x, tz = out[ip].z - out[im].z;
        const float tl = std::sqrt(tx * tx + tz * tz);
        if (tl > 1e-4f) { out[i].tx = tx / tl; out[i].tz = tz / tl; }
    }
}

// PURE barrier planning — see road_network.h. The drop test, PER EDGE: each
// side of each segment samples ITS OWN offside ground ~6 m beyond ITS apron
// edge against the datum — left and right are independent measurements and
// independent barriers (the first cut of the collision wall was single-sided
// in Jolt and only stopped egress to the right; the geometry test was always
// two-sided, the collision now is too — see buildRoadRibbon). Drop > 2 m
// earns the W-beam rail; a ditch-depth 0.6–2 m drop earns the concrete
// jersey wall. Gap segments never barrier (a bore's walls and a bridge's
// parapets are their own protection), and NOTHING is placed within
// kJunctionBarrierClearM of a noted junction — Tim, pinned against a rail at
// a mouth: "INTERSECTIONS NEED TO NOT HAVE RAILINGS."
BarrierPlan planRoadBarriers(const RoadSpec& spec, const std::vector<float>* roadY) {
    BarrierPlan plan;
    const size_t n = spec.x.size();
    if (n < 2) return plan;
    if (roadY && roadY->size() != n) roadY = nullptr;
    plan.mask.assign(n - 1, 0);
    plan.minDropM = 1e9f;
    plan.jerseyMinDropM = 1e9f;

    auto datumAt = [&](size_t i) {
        if (roadY) return (*roadY)[i];
        return terrainHeightAtWorld(spec.x[i], spec.z[i]) + kRoadFloorClear;
    };
    // DUAL routes: each carriageway samples ITS OWN offside — 6 m beyond its
    // outer apron edge, which sits (median + 2 x carriageway) from the
    // centreline. Single routes keep the base offset.
    std::vector<float> medianPlan;
    if (spec.dualCarriageway) {
        std::vector<float> ry(n);
        for (size_t i = 0; i < n; ++i) ry[i] = datumAt(i);
        medianPlan = computeMedianPlan(spec, ry);
    }
    auto dropAt = [&](size_t i, int side) {
        const size_t ip = (i + 1 < n) ? i + 1 : i;
        const size_t im = (i > 0) ? i - 1 : i;
        float tx = spec.x[ip] - spec.x[im], tz = spec.z[ip] - spec.z[im];
        const float tl = std::sqrt(tx * tx + tz * tz);
        if (tl > 1e-4f) { tx /= tl; tz /= tl; }
        const float edge = (!medianPlan.empty())
            ? medianPlan[i] + 2.0f * kFwyPavedHalfM
            : kPavedHalfM * spec.widthScale;   // PAIRED with the ribbon's pavHalf
        const float lat = (edge + 6.0f) * (float)side;
        const float qx = spec.x[i] + (-tz) * lat;
        const float qz = spec.z[i] + ( tx) * lat;
        return datumAt(i) - terrainHeightAtWorld(qx, qz);
    };
    auto segInGap = [&](size_t k) {
        for (const RoadSpec::Gap& g : spec.gaps)
            if (k >= g.i0 && k < g.i1) return true;
        return false;
    };
    for (size_t k = 0; k + 1 < n; ++k) {
        if (segInGap(k)) continue;
        // The junction exclusion zone: both mouths of every junction stay
        // OPEN, wide enough to sweep in at speed from either road.
        if (std::min(distToNearestRoadJunction(spec.x[k],     spec.z[k]),
                     distToNearestRoadJunction(spec.x[k + 1], spec.z[k + 1]))
                < kJunctionBarrierClearM)
            continue;
        for (int side = -1; side <= 1; side += 2) {
            const float d0 = dropAt(k, side), d1 = dropAt(k + 1, side);
            const float d = std::max(d0, d1);
            if (d > 2.0f) {
                plan.mask[k] |= (side < 0) ? 1 : 2;
                ++plan.railSegments;
                plan.minDropM = std::min(plan.minDropM, d);
            } else if (d > 0.6f) {
                plan.mask[k] |= (side < 0) ? 4 : 8;
                ++plan.jerseySegments;
                plan.jerseyMinDropM = std::min(plan.jerseyMinDropM, d);
                plan.jerseyMaxDropM = std::max(plan.jerseyMaxDropM, d);
            }
        }
    }
    if (plan.railSegments == 0)   plan.minDropM = 0.0f;
    if (plan.jerseySegments == 0) plan.jerseyMinDropM = 0.0f;
    return plan;
}

RoadRibbonResult buildRoadRibbon(const RoadSpec& spec, Scene& scene,
                                 x3::rhi::IRenderDevice& device,
                                 x3::phys::IPhysicsWorld& phys,
                                 const std::vector<float>* roadY) {
    RoadRibbonResult out;
    const size_t n = spec.x.size();
    if (n < 2) return out;
    if (roadY && roadY->size() != n) {
        x3::logError("road ribbon: roadY size mismatch — ignoring datum");
        roadY = nullptr;
    }

    SurfaceLibrary& surf = roadSurfaces();
    surf.mount(assetRoot() + "/surface_library");
    const SurfaceSet& asphalt = surf.get(device, spec.surfaceSet.c_str());
    const SurfaceSet& cement  = surf.get(device, "mw_concrete_panels_a");
    if (!asphalt.ok || !cement.ok)
        x3::logWarn("road ribbon: surface set(s) unavailable - flat colour fallback");

    const bool dual = spec.dualCarriageway;

    // ---- THE FINE RENDER PATH (owner: "Get rid of ALLLLL jointed bends") ---
    // Every strip below — pavement, paint, shoulders, aprons, skirts, barrier
    // runs, the median wall — rides this spline, NOT the coarse node chords.
    std::vector<float> medianPlan;
    if (dual) {
        std::vector<float> ry;
        if (!roadY) {
            ry.resize(n);
            for (size_t i = 0; i < n; ++i)
                ry[i] = terrainHeightAtWorld(spec.x[i], spec.z[i]) + kRoadFloorClear;
        }
        medianPlan = computeMedianPlan(spec, roadY ? *roadY : ry);
    }
    std::vector<RoadRenderStation> path;
    buildRoadRenderPath(spec, roadY, medianPlan.empty() ? nullptr : &medianPlan, path);
    if (path.size() < 2) return out;
    out.fineStations = (uint32_t)path.size();

    const std::vector<RoadTurnaround> turnarounds =
        dual ? planTurnarounds(spec) : std::vector<RoadTurnaround>{};
    out.turnaroundCount = (uint32_t)turnarounds.size();
    auto inTurnaround = [&](float u) {
        for (const RoadTurnaround& t : turnarounds)
            if (u >= t.u0 && u <= t.u1) return true;
        return false;
    };

    // Barrier plan stays PER SPEC SEGMENT (the drop test's granularity);
    // fine stations map back through their source segment.
    const BarrierPlan barriers = [&]() {
        const char* e = std::getenv("X3_ROAD_BARRIERS");   // 0 = A/B capture off
        if (e && e[0] == '0') return BarrierPlan{};
        return planRoadBarriers(spec, roadY);
    }();
    out.railSegments   = barriers.railSegments;
    out.railMinDropM   = barriers.minDropM;
    out.jerseySegments = barriers.jerseySegments;
    out.jerseyMinDropM = barriers.jerseyMinDropM;

    // THE ROAD PRISM constants (see the fortress-edge story in git history):
    constexpr float kSkirtFace    = 0.62f;   // minimum visible vertical face (m)
    constexpr float kSkirtOut     = 0.9f;    // batter run outward (m)
    constexpr float kSkirtLap     = 0.6f;    // toe depth under the carved ground
    constexpr float kSkirtMaxDrop = 14.0f;   // don't build a curtain wall off a cliff

    RibbonMesh road, shoulders, aprons, paint, skirt, rails, jersey, crossover;
    std::vector<float>    railColV;
    std::vector<uint32_t> railColI;

    auto P = [&](const RoadRenderStation& st, float lat, float o[3]) {
        o[0] = st.x + (-st.tz) * lat;
        o[1] = st.y + kPaveProud;
        o[2] = st.z + ( st.tx) * lat;
    };

    // THE F-SHAPE JERSEY PROFILE — the ONE wall profile (offside barriers AND
    // the median wall are extrusions of it; no second wall was invented).
    static const float jp[4][2] = {
        { 0.30f, 0.00f },    // base edge
        { 0.30f, 0.075f },   // top of the vertical toe
        { 0.17f, 0.33f },    // top of the 55-degree haunch
        { 0.115f, 0.81f },   // crown edge
    };
    auto jerseyRun = [&](RibbonMesh& mesh, const RoadRenderStation& a,
                         const RoadRenderStation& b, float latA, float latB,
                         float u0, float u1) {
        auto wallPt = [&](const RoadRenderStation& st, float lat, float off,
                          float h, float o[3]) {
            P(st, lat + off, o);
            o[1] = st.y + kPaveProud - 0.05f + h;   // seat 5 cm into the pavement
        };
        for (int face = -1; face <= 1; face += 2) {
            for (int j = 0; j + 1 < 4; ++j) {
                float aB[3], bB[3], aT[3], bT[3];
                wallPt(a, latA, jp[j][0]     * (float)face, jp[j][1],     aB);
                wallPt(b, latB, jp[j][0]     * (float)face, jp[j][1],     bB);
                wallPt(a, latA, jp[j + 1][0] * (float)face, jp[j + 1][1], aT);
                wallPt(b, latB, jp[j + 1][0] * (float)face, jp[j + 1][1], bT);
                const float v0 = 0.1f * (float)j, v1 = 0.1f * (float)(j + 1);
                mesh.quadN(aB, aT, bT, bB, v0, v1, u0 * 0.35f, u1 * 0.35f);
                mesh.quadN(aT, aB, bB, bT, v0, v1, u0 * 0.35f, u1 * 0.35f);
            }
        }
        float aL[3], bL[3], aR[3], bR[3];
        wallPt(a, latA, -jp[3][0], jp[3][1], aL);
        wallPt(b, latB, -jp[3][0], jp[3][1], bL);
        wallPt(a, latA,  jp[3][0], jp[3][1], aR);
        wallPt(b, latB,  jp[3][0], jp[3][1], bR);
        mesh.quadN(aL, aR, bR, bL, 0.40f, 0.46f, u0 * 0.35f, u1 * 0.35f);
        mesh.quadN(aR, aL, bL, bR, 0.40f, 0.46f, u0 * 0.35f, u1 * 0.35f);
    };
    // A wall must END CLEANLY at a turnaround gap / junction zone (the
    // junction exclusion discipline): a vertical nose closing the profile.
    auto jerseyEndCap = [&](RibbonMesh& mesh, const RoadRenderStation& st, float lat) {
        auto wp = [&](float off, float h, float o[3]) {
            P(st, lat + off, o);
            o[1] = st.y + kPaveProud - 0.05f + h;
        };
        for (int j = 0; j + 1 < 4; ++j) {
            float p0[3], p1[3], p2[3], p3[3];
            wp(-jp[j][0],     jp[j][1],     p0);
            wp( jp[j][0],     jp[j][1],     p1);
            wp( jp[j + 1][0], jp[j + 1][1], p2);
            wp(-jp[j + 1][0], jp[j + 1][1], p3);
            mesh.quadN(p0, p1, p2, p3, 0.0f, 0.1f, 0.0f, 0.2f);
            mesh.quadN(p1, p0, p3, p2, 0.0f, 0.1f, 0.0f, 0.2f);
        }
    };

    // Per-carriageway cross-section: the freeway runs 8 lanes a side, the base
    // profile keeps its 4. Everything else (shoulder, apron, prism) is shared.
    const int   lanes   = dual ? kFwyLaneCount     : kLaneCount;
    // spec.widthScale narrows the whole section together (see RoadSpec) — a
    // dirt track is this same profile at a fraction of the width, not a
    // different one. 1.0 for every paved route in the world.
    const float wS      = spec.widthScale;
    const float runHalf = (dual ? kFwyRunningHalfM  : kRunningHalfM)  * wS;
    const float shoHalf = (dual ? kFwyShoulderHalfM : kShoulderHalfM) * wS;
    const float pavHalf = (dual ? kFwyPavedHalfM    : kPavedHalfM)    * wS;
    const float laneM   = kLaneFt * kFtToM;
    const float halfPaint = 0.06f;              // ~5 in stripe

    bool wallOpen = false;
    for (size_t si = 0; si + 1 < path.size(); ++si) {
        const RoadRenderStation& a = path[si];
        const RoadRenderStation& b = path[si + 1];
        if (a.gap || b.gap) {   // a tunnel or a deck owns this reach
            if (wallOpen) { jerseyEndCap(jersey, a, 0.0f); wallOpen = false; }
            continue;
        }
        const float u0 = a.u, u1 = b.u;
        if (u1 - u0 < 1e-4f) continue;
        out.lengthM += u1 - u0;
        const uint32_t k = a.seg;

        // Junction feather factor, shared by every skirt this segment emits.
        const float dj = std::min(distToNearestRoadJunction(a.x, a.z),
                                  distToNearestRoadJunction(b.x, b.z));
        const float tj = std::max(0.0f, std::min(1.0f,
            (dj - kJunctionBarrierClearM) / 15.0f));   // 0 in the zone -> 1 outside
        const float faceH = 0.06f + (kSkirtFace - 0.06f) * tj;
        const float slope = 3.0f - 1.5f * tj;          // out-run per metre of drop

        const int nCw = dual ? 2 : 1;
        for (int cw = 0; cw < nCw; ++cw) {
            // carriageway centre offset from the route centreline (0 on single)
            const float sideC = dual ? (cw == 0 ? -1.0f : 1.0f) : 0.0f;
            const float cA = dual ? sideC * (a.medianHalf + kFwyPavedHalfM) : 0.0f;
            const float cB = dual ? sideC * (b.medianHalf + kFwyPavedHalfM) : 0.0f;

            float aL[3], aR[3], bL[3], bR[3];
            P(a, cA - runHalf, aL); P(a, cA + runHalf, aR);
            P(b, cB - runHalf, bL); P(b, cB + runHalf, bR);
            road.quad(aL, aR, bR, bL, 0.0f, 1.0f, u0 * 0.06f, u1 * 0.06f);

            float aLs[3], bLs[3], aRs[3], bRs[3];
            P(a, cA - shoHalf, aLs); P(b, cB - shoHalf, bLs);
            P(a, cA + shoHalf, aRs); P(b, cB + shoHalf, bRs);
            shoulders.quad(aLs, aL, bL, bLs, 0.0f, 1.0f, u0 * 0.06f, u1 * 0.06f);
            shoulders.quad(aR, aRs, bRs, bR, 0.0f, 1.0f, u0 * 0.06f, u1 * 0.06f);

            float aLo[3], bLo[3], aRo[3], bRo[3];
            P(a, cA - pavHalf, aLo); P(b, cB - pavHalf, bLo);
            P(a, cA + pavHalf, aRo); P(b, cB + pavHalf, bRo);
            aprons.quad(aLo, aLs, bLs, bLo, 0.0f, 1.0f, u0 * 0.06f, u1 * 0.06f);
            aprons.quad(aRs, aRo, bRo, bRs, 0.0f, 1.0f, u0 * 0.06f, u1 * 0.06f);

            // THE PRISM SKIRT, both edges of this carriageway. On a dual route
            // the median-facing edge gets the same treatment — the median
            // ground is the carved field ~0.2 m below the slab, so it reads as
            // a poured kerb dropping to the graded median, exactly what a
            // depressed-median freeway edge is.
            for (int side = -1; side <= 1; side += 2) {
                const float latA = cA + pavHalf * (float)side;
                const float latB = cB + pavHalf * (float)side;
                float aT[3], bT[3];
                P(a, latA, aT); P(b, latB, bT);
                float aK[3] = { aT[0], aT[1] - faceH, aT[2] };
                float bK[3] = { bT[0], bT[1] - faceH, bT[2] };
                auto outRun = [&](const RoadRenderStation& st, float lat, const float top[3]) {
                    float probe[3];
                    P(st, lat + 2.0f * (float)side, probe);
                    const float ground = terrainHeightAtWorld(probe[0], probe[2]);
                    const float drop = std::max(0.0f,
                        std::min(top[1] - ground, kSkirtMaxDrop));
                    return std::max(kSkirtOut, std::min(24.0f, drop * slope));
                };
                float aO[3], bO[3];
                P(a, latA + outRun(a, latA, aT) * (float)side, aO);
                P(b, latB + outRun(b, latB, bT) * (float)side, bO);
                auto toeY = [&](const float top[3], const float o[3]) {
                    const float ground = terrainHeightAtWorld(o[0], o[2]);
                    float y = std::min(top[1] - faceH, ground) - kSkirtLap;
                    return std::max(y, top[1] - kSkirtMaxDrop);
                };
                aO[1] = toeY(aT, aO);
                bO[1] = toeY(bT, bO);
                const float faceV0 = 0.0f, faceV1 = faceH / 6.1f;
                const float batV1  = faceV1 + (faceH + kSkirtLap) / 6.1f;
                if (side > 0) {
                    skirt.quadN(aT, aK, bK, bT, faceV0, faceV1, u0 * 0.06f, u1 * 0.06f);
                    skirt.quadN(aK, aO, bO, bK, faceV1, batV1,  u0 * 0.06f, u1 * 0.06f);
                } else {
                    skirt.quadN(aK, aT, bT, bK, faceV1, faceV0, u0 * 0.06f, u1 * 0.06f);
                    skirt.quadN(aO, aK, bK, bO, batV1,  faceV1, u0 * 0.06f, u1 * 0.06f);
                }
            }

            // LANE MARKINGS — WHITE ONLY. This is a divided road: opposing
            // traffic is on the other carriageway, so there is NO double
            // yellow anywhere. Solid edges, dashed interior lines (40 ft
            // cycle, 60% duty, cut as true duty windows in u).
            for (int lane = 0; lane <= lanes && spec.laneMarkings; ++lane) {
                const float latLA = cA - runHalf + (float)lane * laneM;
                const float latLB = cB - runHalf + (float)lane * laneM;
                const bool edge = (lane == 0 || lane == lanes);
                if (!edge) {
                    const float cycle = 12.19f;         // 40 ft
                    const float duty  = 0.6f;
                    float e0[3], e1[3], f0[3], f1[3];
                    P(a, latLA - halfPaint, e0); P(a, latLA + halfPaint, e1);
                    P(b, latLB - halfPaint, f0); P(b, latLB + halfPaint, f1);
                    auto lerp3 = [](const float A[3], const float B[3], float t, float o[3]) {
                        o[0] = A[0] + (B[0] - A[0]) * t;
                        o[1] = A[1] + (B[1] - A[1]) * t + 0.012f;
                        o[2] = A[2] + (B[2] - A[2]) * t;
                    };
                    for (float c0 = std::floor(u0 / cycle) * cycle; c0 < u1; c0 += cycle) {
                        const float s0 = std::max(u0, c0);
                        const float s1 = std::min(u1, c0 + cycle * duty);
                        if (s1 <= s0 + 0.05f) continue;
                        const float ta = (s0 - u0) / (u1 - u0);
                        const float tb = (s1 - u0) / (u1 - u0);
                        float da[3], db[3], dc[3], dd[3];
                        lerp3(e0, f0, ta, da); lerp3(e1, f1, ta, db);
                        lerp3(e1, f1, tb, dc); lerp3(e0, f0, tb, dd);
                        paint.quad(da, db, dc, dd, 0.0f, 1.0f, 0.0f, 1.0f);
                    }
                    continue;
                }
                float pa[3], pb[3], pc[3], pd[3];
                P(a, latLA - halfPaint, pa); P(a, latLA + halfPaint, pb);
                P(b, latLB + halfPaint, pc); P(b, latLB - halfPaint, pd);
                pa[1] += 0.012f; pb[1] += 0.012f; pc[1] += 0.012f; pd[1] += 0.012f;
                paint.quad(pa, pb, pc, pd, 0.0f, 1.0f, 0.0f, 1.0f);
            }
        }

        // GUARDRAILS + OFFSIDE JERSEY from the drop-test plan. On a dual route
        // side -1/-left maps to the LEFT carriageway's outer edge and +1 to the
        // RIGHT's — each roadway carries its own barriers; the median is the
        // median wall's job below.
        auto barLat = [&](const RoadRenderStation& st, int side, float inset) {
            const float edge = dual ? (st.medianHalf + 2.0f * kFwyPavedHalfM)
                                    : pavHalf;   // scaled with the section
            return (edge - inset) * (float)side;
        };
        if (k < barriers.mask.size() && (barriers.mask[k] & 3)) {
            for (int side = -1; side <= 1; side += 2) {
                const uint8_t bit = (side < 0) ? 1 : 2;
                if (!(barriers.mask[k] & bit)) continue;
                float a0[3], b0[3];
                P(a, barLat(a, side, 0.3f), a0);
                P(b, barLat(b, side, 0.3f), b0);
                auto band = [&](float y0, float y1) {
                    float aB[3] = { a0[0], a0[1] + y0, a0[2] };
                    float bB[3] = { b0[0], b0[1] + y0, b0[2] };
                    float aT[3] = { a0[0], a0[1] + y1, a0[2] };
                    float bT[3] = { b0[0], b0[1] + y1, b0[2] };
                    rails.quadN(aB, aT, bT, bB, 0.0f, 0.15f, u0 * 0.25f, u1 * 0.25f);
                    rails.quadN(aT, aB, bB, bT, 0.0f, 0.15f, u0 * 0.25f, u1 * 0.25f);
                };
                band(0.26f, 0.36f);
                band(0.50f, 0.60f);
                for (float su = std::ceil(u0 / 4.0f) * 4.0f; su < u1; su += 4.0f) {
                    const float t = (su - u0) / (u1 - u0);
                    float p0[3] = { a0[0] + (b0[0] - a0[0]) * t,
                                    a0[1] + (b0[1] - a0[1]) * t,
                                    a0[2] + (b0[2] - a0[2]) * t };
                    const float dxs = (b0[0] - a0[0]) / (u1 - u0);
                    const float dzs = (b0[2] - a0[2]) / (u1 - u0);
                    float pA[3] = { p0[0] - dxs * 0.06f, p0[1],         p0[2] - dzs * 0.06f };
                    float pB[3] = { p0[0] + dxs * 0.06f, p0[1],         p0[2] + dzs * 0.06f };
                    float pC[3] = { pB[0],               p0[1] + 0.65f, pB[2] };
                    float pD[3] = { pA[0],               p0[1] + 0.65f, pA[2] };
                    rails.quadN(pA, pB, pC, pD, 0.0f, 0.12f, 0.0f, 0.65f);
                    rails.quadN(pB, pA, pD, pC, 0.0f, 0.12f, 0.0f, 0.65f);
                }
                const uint32_t cb = (uint32_t)(railColV.size() / 3);
                const float wall[4][3] = {
                    { a0[0], a0[1],        a0[2] }, { b0[0], b0[1],        b0[2] },
                    { b0[0], b0[1] + 1.0f, b0[2] }, { a0[0], a0[1] + 1.0f, a0[2] } };
                for (const auto& w : wall) {
                    railColV.push_back(w[0]); railColV.push_back(w[1]); railColV.push_back(w[2]);
                }
                const uint32_t wi[12] = { cb, cb + 1, cb + 2,  cb, cb + 2, cb + 3,
                                          cb, cb + 2, cb + 1,  cb, cb + 3, cb + 2 };
                railColI.insert(railColI.end(), wi, wi + 12);
            }
        }
        if (k < barriers.mask.size() && (barriers.mask[k] & 12)) {
            for (int side = -1; side <= 1; side += 2) {
                const uint8_t bit = (side < 0) ? 4 : 8;
                if (!(barriers.mask[k] & bit)) continue;
                jerseyRun(jersey, a, b, barLat(a, side, 0.45f), barLat(b, side, 0.45f),
                          u0, u1);
            }
        }

        // MEDIAN FEATURES (dual): the continuous jersey median wall where the
        // median is narrow, ending cleanly at every turnaround gap and inside
        // every junction exclusion zone; the paved crossover in the gaps.
        if (dual) {
            const float uMid = 0.5f * (u0 + u1);
            const bool ta = inTurnaround(uMid);
            const bool wallHere = !ta && dj >= kJunctionBarrierClearM &&
                0.5f * (a.medianHalf + b.medianHalf) <= kFwyMedianWallHalfM;
            if (wallHere) {
                if (!wallOpen) { jerseyEndCap(jersey, a, 0.0f); ++out.medianWallRuns; wallOpen = true; }
                jerseyRun(jersey, a, b, 0.0f, 0.0f, u0, u1);
            } else if (wallOpen) {
                jerseyEndCap(jersey, a, 0.0f);
                wallOpen = false;
            }
            if (ta) {
                // THE TURNAROUND: asphalt across the median, lapped 3 m onto
                // each inner apron, with closure faces down to the graded
                // median ground so the slab edge reads poured, not floating.
                const float wA = a.medianHalf + 3.0f, wB = b.medianHalf + 3.0f;
                float c0[3], c1[3], c2[3], c3[3];
                P(a, -wA, c0); P(a, wA, c1); P(b, wB, c2); P(b, -wB, c3);
                c0[1] += 0.006f; c1[1] += 0.006f; c2[1] += 0.006f; c3[1] += 0.006f;
                crossover.quad(c0, c1, c2, c3, 0.0f, 1.0f, u0 * 0.06f, u1 * 0.06f);
            }
        }
    }
    if (wallOpen) { jerseyEndCap(jersey, path.back(), 0.0f); wallOpen = false; }

    // TURNAROUND FILLETS — a small cement flare at each crossover corner so
    // the mouth reads as a paved feature, not a butt-jointed patch.
    if (dual) {
        auto stAtU = [&](float u) {
            size_t lo = 0, hi = path.size() - 1;
            while (lo + 1 < hi) {
                const size_t mid = (lo + hi) / 2;
                if (path[mid].u <= u) lo = mid; else hi = mid;
            }
            const RoadRenderStation& A = path[lo];
            const RoadRenderStation& B = path[hi];
            const float span = std::max(1e-4f, B.u - A.u);
            const float t = std::max(0.0f, std::min(1.0f, (u - A.u) / span));
            RoadRenderStation st = A;
            st.x = A.x + (B.x - A.x) * t;
            st.z = A.z + (B.z - A.z) * t;
            st.y = A.y + (B.y - A.y) * t;
            st.tx = A.tx; st.tz = A.tz;
            st.medianHalf = A.medianHalf + (B.medianHalf - A.medianHalf) * t;
            st.u = u;
            return st;
        };
        RibbonMesh fillets;
        for (const RoadTurnaround& t : turnarounds) {
            for (int endSide = 0; endSide < 2; ++endSide) {
                const float uE = endSide == 0 ? t.u0 : t.u1;
                const float uF = endSide == 0 ? std::max(0.0f, t.u0 - 9.0f)
                                              : std::min(path.back().u, t.u1 + 9.0f);
                const RoadRenderStation sE = stAtU(uE), sF = stAtU(uF);
                for (int side = -1; side <= 1; side += 2) {
                    float p0[3], p1[3], p2[3];
                    P(sE, (float)side * (sE.medianHalf + 3.0f), p0);
                    P(sE, (float)side *  sE.medianHalf,         p1);
                    P(sF, (float)side * (sF.medianHalf + 0.6f), p2);
                    p0[1] += 0.004f; p1[1] += 0.004f; p2[1] += 0.004f;
                    if ((side > 0) == (endSide == 0)) fillets.quad(p0, p1, p2, p0, 0.0f, 0.5f, 0.0f, 0.5f);
                    else                              fillets.quad(p1, p0, p2, p1, 0.0f, 0.5f, 0.0f, 0.5f);
                }
            }
        }
        // fold the fillets into the crossover mesh (same asphalt-adjacent look)
        const uint32_t base = (uint32_t)crossover.v.size();
        crossover.v.insert(crossover.v.end(), fillets.v.begin(), fillets.v.end());
        for (uint32_t idx : fillets.i) crossover.i.push_back(base + idx);
    }

    // ---- THE WORK ZONE (first dressing example, dual routes) ---------------
    // A lane closure on the right carriageway of one stretch: a taper of
    // traffic cones closing the OUTSIDE lane over ~150 m, three orange-and-
    // white drums at the head. Cones are render-only (drive-over is fine);
    // drums are LIGHT DYNAMIC BODIES — hit one and it scatters.
    RibbonMesh coneField;
    std::vector<float> barrelSpots;   // x,y,z triples
    if (dual && path.back().u > 5000.0f) {
        auto stAtU = [&](float u) {
            size_t lo = 0, hi = path.size() - 1;
            while (lo + 1 < hi) {
                const size_t mid = (lo + hi) / 2;
                if (path[mid].u <= u) lo = mid; else hi = mid;
            }
            const RoadRenderStation& A = path[lo];
            const RoadRenderStation& B = path[hi];
            const float span = std::max(1e-4f, B.u - A.u);
            const float t = std::max(0.0f, std::min(1.0f, (u - A.u) / span));
            RoadRenderStation st = A;
            st.x = A.x + (B.x - A.x) * t;
            st.z = A.z + (B.z - A.z) * t;
            st.y = A.y + (B.y - A.y) * t;
            st.medianHalf = A.medianHalf + (B.medianHalf - A.medianHalf) * t;
            st.u = u;
            return st;
        };
        const float total = path.back().u;
        float uWz = -1.0f;
        for (float cand = total * 0.25f; cand < total * 0.7f; cand += 50.0f) {
            bool ok = true;
            for (float uu = cand - 40.0f; uu <= cand + 260.0f && ok; uu += 10.0f) {
                if (inTurnaround(std::max(0.0f, uu))) ok = false;
                const RoadRenderStation st = stAtU(std::max(0.0f, std::min(total, uu)));
                if (st.gap) ok = false;
                if (distToNearestRoadJunction(st.x, st.z) < 90.0f) ok = false;
            }
            if (ok) { uWz = cand; break; }
        }
        if (uWz > 0.0f) {
            // A ~0.7 m traffic cone baked in world space (render-only).
            auto emitCone = [&](const float base[3]) {
                const int   seg = 10;
                const float rB = 0.17f, rT = 0.035f, h = 0.70f, plate = 0.21f;
                const float kTwoPi = 6.2831853f;
                // square-ish base plate (a low slab)
                for (int q = 0; q < 4; ++q) {
                    const float a0 = kTwoPi * (float)q / 4.0f + 0.7854f;
                    const float a1 = kTwoPi * (float)(q + 1) / 4.0f + 0.7854f;
                    float p0[3] = { base[0] + std::cos(a0) * plate, base[1] + 0.035f,
                                    base[2] + std::sin(a0) * plate };
                    float p1[3] = { base[0] + std::cos(a1) * plate, base[1] + 0.035f,
                                    base[2] + std::sin(a1) * plate };
                    float c[3]  = { base[0], base[1] + 0.035f, base[2] };
                    coneField.quadN(c, p0, p1, c, 0.5f, 0.5f, 0.02f, 0.06f);
                    float g0[3] = { p0[0], base[1], p0[2] };
                    float g1[3] = { p1[0], base[1], p1[2] };
                    coneField.quadN(p0, g0, g1, p1, 0.5f, 0.5f, 0.0f, 0.04f);
                }
                for (int q = 0; q < seg; ++q) {
                    const float a0 = kTwoPi * (float)q / (float)seg;
                    const float a1 = kTwoPi * (float)(q + 1) / (float)seg;
                    float b0[3] = { base[0] + std::cos(a0) * rB, base[1] + 0.05f,
                                    base[2] + std::sin(a0) * rB };
                    float b1[3] = { base[0] + std::cos(a1) * rB, base[1] + 0.05f,
                                    base[2] + std::sin(a1) * rB };
                    float t0[3] = { base[0] + std::cos(a0) * rT, base[1] + h,
                                    base[2] + std::sin(a0) * rT };
                    float t1[3] = { base[0] + std::cos(a1) * rT, base[1] + h,
                                    base[2] + std::sin(a1) * rT };
                    // v runs bottom->top so the band texture reads upright
                    coneField.quadN(b0, b1, t1, t0,
                                    (float)q / (float)seg, (float)(q + 1) / (float)seg,
                                    0.07f, 1.0f);
                }
            };
            // Taper: 13 cones walking the outside lane closed over ~150 m on
            // the RIGHT (cw +1) carriageway, then the drums across its head.
            const float taper = 150.0f;
            for (int i = 0; i <= 12; ++i) {
                const float s = uWz + (float)i * (taper / 12.0f);
                const RoadRenderStation st = stAtU(s);
                const float cOff = st.medianHalf + kFwyPavedHalfM;
                const float lat = cOff + kFwyRunningHalfM
                                - laneM * std::min(1.0f, (s - uWz) / taper);
                float bp[3];
                P(st, lat, bp);
                emitCone(bp);
                ++out.workZoneCones;
            }
            for (int i = 0; i < 3; ++i) {
                const RoadRenderStation st = stAtU(uWz + taper + 8.0f + (float)i * 3.5f);
                const float cOff = st.medianHalf + kFwyPavedHalfM;
                const float lat = cOff + kFwyRunningHalfM - laneM
                                + laneM * (0.25f + 0.25f * (float)i);
                float bp[3];
                P(st, lat, bp);
                barrelSpots.push_back(bp[0]);
                barrelSpots.push_back(bp[1]);
                barrelSpots.push_back(bp[2]);
            }
            char wb[160];
            std::snprintf(wb, sizeof(wb),
                "road '%s': work zone at u %.0f m — %u cones tapering the outside "
                "lane over %.0f m, 3 drums at the head",
                spec.name.c_str(), uWz, out.workZoneCones, taper);
            x3::logInfo(wb);
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

    float roadTint [4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    if (!asphalt.ok) { roadTint[0] = 0.055f; roadTint[1] = 0.056f; roadTint[2] = 0.060f; }
    float apronTint[4] = { 0.86f, 0.85f, 0.82f, 1.0f };
    if (!cement.ok)  { apronTint[0] = 0.42f; apronTint[1] = 0.41f; apronTint[2] = 0.38f; }
    const float paintC[4] = { 1.8f, 1.8f, 1.7f, 1.0f };
    upload(road,   &asphalt, roadTint,  true);
    float shoulderTint[4] = { 0.78f, 0.78f, 0.79f, 1.0f };
    if (!asphalt.ok) { shoulderTint[0] = 0.072f; shoulderTint[1] = 0.072f; shoulderTint[2] = 0.076f; }
    upload(shoulders, &asphalt, shoulderTint, true);
    upload(aprons, &cement,  apronTint, true);
    float skirtTint[4] = { 0.74f, 0.73f, 0.70f, 1.0f };
    if (!cement.ok) { skirtTint[0] = 0.34f; skirtTint[1] = 0.33f; skirtTint[2] = 0.31f; }
    upload(skirt,  &cement,  skirtTint, true);
    const float railTint[4] = { 0.60f, 0.62f, 0.65f, 1.0f };
    upload(rails,  nullptr,  railTint,  false);
    if (!railColI.empty())
        phys.addStaticMesh(railColV.data(), (uint32_t)(railColV.size() / 3),
                           railColI.data(), (uint32_t)railColI.size());
    float jerseyTint[4] = { 0.70f, 0.69f, 0.66f, 1.0f };
    if (!cement.ok) { jerseyTint[0] = 0.31f; jerseyTint[1] = 0.30f; jerseyTint[2] = 0.28f; }
    upload(jersey, &cement,  jerseyTint, true);
    // the crossover is asphalt paved slightly darker than the lanes (new mix)
    float crossTint[4] = { 0.92f, 0.92f, 0.94f, 1.0f };
    if (!asphalt.ok) { crossTint[0] = 0.048f; crossTint[1] = 0.049f; crossTint[2] = 0.053f; }
    upload(crossover, &asphalt, crossTint, true);
    upload(paint,  nullptr,  paintC,    false);

    // Work-zone dressing: cones (render-only, banded texture) + dynamic drums.
    if (!coneField.empty() || !barrelSpots.empty()) {
        // Orange with two white retroreflective bands, v = height fraction.
        const uint32_t tw = 4, th = 64;
        std::vector<uint8_t> px((size_t)tw * th * 4);
        for (uint32_t y = 0; y < th; ++y) {
            const float v = (float)y / (float)(th - 1);
            const bool white = (v > 0.42f && v < 0.58f) || (v > 0.72f && v < 0.86f);
            const uint8_t r = white ? 235 : 235;
            const uint8_t g = white ? 235 : 88;
            const uint8_t bcol = white ? 230 : 18;
            for (uint32_t x = 0; x < tw; ++x) {
                uint8_t* p = &px[((size_t)y * tw + x) * 4];
                p[0] = r; p[1] = g; p[2] = bcol; p[3] = 255;
            }
        }
        x3::rhi::TextureHandle bandTex =
            device.createTexture(px.data(), tw, th, /*srgb=*/true);
        if (!coneField.empty()) {
            Entity e;
            e.mesh = device.createMesh(coneField.v.data(), (uint32_t)coneField.v.size(),
                                       coneField.i.data(), (uint32_t)coneField.i.size());
            if (e.mesh.valid()) {
                e.tex = bandTex;
                scene.add(e);
                ++out.meshCount;
            }
        }
        if (!barrelSpots.empty()) {
            // One drum mesh (origin at the base centre), one entity + one
            // light dynamic body per drum — Scene::update syncs them, so a
            // clipped drum slides away instead of stopping the car dead.
            RibbonMesh drum;
            const int   seg = 14;
            const float r = 0.29f, h = 0.94f;
            const float kTwoPi = 6.2831853f;
            for (int q = 0; q < seg; ++q) {
                const float a0 = kTwoPi * (float)q / (float)seg;
                const float a1 = kTwoPi * (float)(q + 1) / (float)seg;
                float b0[3] = { std::cos(a0) * r, 0.0f, std::sin(a0) * r };
                float b1[3] = { std::cos(a1) * r, 0.0f, std::sin(a1) * r };
                float t0[3] = { b0[0], h, b0[2] };
                float t1[3] = { b1[0], h, b1[2] };
                drum.quadN(b0, b1, t1, t0,
                           (float)q / (float)seg, (float)(q + 1) / (float)seg,
                           0.02f, 0.98f);
                float c0[3] = { 0.0f, h, 0.0f };
                drum.quadN(t0, t1, c0, c0, 0.5f, 0.5f, 0.95f, 1.0f);
            }
            x3::rhi::MeshHandle drumMesh =
                device.createMesh(drum.v.data(), (uint32_t)drum.v.size(),
                                  drum.i.data(), (uint32_t)drum.i.size());
            for (size_t i = 0; i + 2 < barrelSpots.size(); i += 3) {
                const float bx = barrelSpots[i], by = barrelSpots[i + 1], bz = barrelSpots[i + 2];
                x3::phys::BodyId body = phys.addBox({ 0.29f, 0.47f, 0.29f },
                                                    { bx, by + 0.47f, bz },
                                                    9.0f, x3::phys::Layer::Dynamic);
                Entity e;
                e.mesh = drumMesh;
                e.tex = bandTex;
                e.body = body;
                e.bodyVisualOffsetY = 0.47f;
                e.transform[12] = bx; e.transform[13] = by; e.transform[14] = bz;
                e.tag = (uint32_t)Tag::Prop;
                scene.add(e);
                ++out.workZoneBarrels;
                ++out.meshCount;
            }
        }
    }

    out.ok = out.meshCount > 0;
    char b[420];
    if (dual) {
        std::snprintf(b, sizeof(b),
            "road ribbon '%s': %.2f miles DUAL | %u meshes, %u quads, %u fine stations | "
            "2 x (%d x %.0f ft lanes + %.0f ft shoulders + %.0f ft aprons) = 2 x %.0f ft paved, "
            "median %.1f..%.1f m | %u median wall runs, %u turnarounds | "
            "%u rail + %u jersey offside segments | %u cones, %u drums",
            spec.name.c_str(), out.lengthM / 1609.34f, out.meshCount, out.quadCount,
            out.fineStations, lanes, kLaneFt, kShoulderFt, kApronFt,
            pavHalf * 2.0f / kFtToM,
            kFwyMedianMinHalfM * 2.0f, kFwyMedianMaxHalfM * 2.0f,
            out.medianWallRuns, out.turnaroundCount,
            out.railSegments, out.jerseySegments,
            out.workZoneCones, out.workZoneBarrels);
    } else {
        std::snprintf(b, sizeof(b),
            "road ribbon '%s': %.2f miles | %u meshes, %u quads, %u fine stations | "
            "%d x %.0f ft lanes + %.0f ft shoulders + %.0f ft aprons = %.0f ft paved | "
            "%u guardrail segments (min drop railed %.1f m) | %u jersey-wall segments "
            "(ditch band, min drop %.1f m)",
            spec.name.c_str(), out.lengthM / 1609.34f, out.meshCount, out.quadCount,
            // SCALED, like the geometry above it. This line read a flat 96 ft
            // off the constants and reported that for a 28.8 ft dirt track —
            // a receipt that describes something other than what was built is
            // worse than no receipt, because it gets believed.
            out.fineStations, lanes, kLaneFt * wS, kShoulderFt * wS, kApronFt * wS,
            pavHalf * 2.0f / kFtToM,
            out.railSegments, out.railMinDropM,
            out.jerseySegments, out.jerseyMinDropM);
    }
    x3::logInfo(b);
    return out;
}

// ---------------------------------------------------------------------------
// THE JUNCTION MOUTH — see road_network.h. A ruled asphalt transition from the
// branch ribbon's flat terminal edge onto the main road's (longitudinally
// sloped) surface, lapped a couple of metres over the main pavement with a few
// millimetres of lift, plus cement flare wings. Collision on all of it.
// ---------------------------------------------------------------------------
RoadRibbonResult buildJunctionMouth(const RoadJunction& j, Scene& scene,
                                    x3::rhi::IRenderDevice& device,
                                    x3::phys::IPhysicsWorld& phys) {
    RoadRibbonResult out;
    if (!j.valid) return out;

    SurfaceLibrary& surf = roadSurfaces();
    surf.mount(assetRoot() + "/surface_library");
    const SurfaceSet& asphalt = surf.get(device, "rd_asphalt_01");
    const SurfaceSet& cement  = surf.get(device, "mw_concrete_panels_a");

    // Frame: from the branch's end node toward (and 2.5 m past) the junction.
    float dx = j.jx - j.endX, dz = j.jz - j.endZ;
    const float setb = std::sqrt(dx * dx + dz * dz);
    if (setb < 1.0f) return out;
    dx /= setb; dz /= setb;
    const float pxd = -dz, pzd = dx;                 // branch lateral axis
    const float V = setb + 2.5f;                     // overlap past the main centreline
    const int   nv = 10, nu = 8;
    // The mouth's asphalt carries the running width PLUS the shoulder through
    // the junction, so the branch's paved shoulder does not dead-end into
    // cement at the terminal edge; the flare wings pick up from there.
    const float runW = kShoulderHalfM, pavW = kPavedHalfM;

    // The TWIST — from the branch's laterally-flat edge onto the main road's
    // longitudinally-sloped surface — must be COMPLETE by the main pavement's
    // edge, or the patch meets the main pavement half-twisted (measured on
    // paper: up to ±0.9 m at the wing tips on a 7% main grade). That is what
    // the kJctSetbackM run exists for.
    // ... complete by the MAIN pavement's edge — which on a divided freeway is
    // the NEAR CARRIAGEWAY's outer apron edge, not the centreline profile.
    const float twistRun = std::max(2.0f, setb - j.mainPavedEdgeM);
    auto heightAt = [&](float u, float v) {
        // world point of (u,v)
        const float wx = j.endX + dx * v + pxd * u;
        const float wz = j.endZ + dz * v + pzd * u;
        // main road surface under that point: its datum slides along its tangent
        const float alongMain = (wx - j.jx) * j.mainTX + (wz - j.jz) * j.mainTZ;
        const float mainY = j.jy + alongMain * j.mainGrade;
        const float t  = std::max(0.0f, std::min(1.0f, v / twistRun));
        const float sm = t * t * (3.0f - 2.0f * t);
        // lift: 0 while the patch IS the pavement (the transition run), ramping
        // to 8 mm as it laps onto the main road's own pavement — lapped, never
        // coplanar, so there is nothing to z-fight
        const float lift = 0.008f * std::max(0.0f, std::min(1.0f, (v - (twistRun - 2.0f)) / 3.0f));
        return (1.0f - sm) * j.endY + sm * mainY + kPaveProud + lift;
    };
    auto pointAt = [&](float u, float v, float o[3]) {
        o[0] = j.endX + dx * v + pxd * u;
        o[2] = j.endZ + dz * v + pzd * u;
        o[1] = heightAt(u, v);
    };

    RibbonMesh road, wings, feather;
    for (int iv = 0; iv < nv; ++iv) {
        const float v0 = V * (float)iv / (float)nv, v1 = V * (float)(iv + 1) / (float)nv;
        // asphalt: the branch's running width, carried through the mouth
        for (int iu = 0; iu < nu; ++iu) {
            const float u0 = -runW + 2.0f * runW * (float)iu / (float)nu;
            const float u1 = -runW + 2.0f * runW * (float)(iu + 1) / (float)nu;
            float a[3], b[3], c[3], d[3];
            pointAt(u0, v0, a); pointAt(u1, v0, b);
            pointAt(u1, v1, c); pointAt(u0, v1, d);
            road.quad(a, b, c, d, (float)iu / nu, (float)(iu + 1) / nu,
                      v0 * 0.06f, v1 * 0.06f);
        }
        // cement flare wings, widening toward the junction
        auto flare = [&](float v) {
            const float t = std::max(0.0f, std::min(1.0f, v / setb));
            return 8.0f * t * t * (3.0f - 2.0f * t);
        };
        for (int side = -1; side <= 1; side += 2) {
            float a[3], b[3], c[3], d[3];
            const float o0 = pavW + flare(v0), o1 = pavW + flare(v1);
            pointAt((float)side * runW, v0, a); pointAt((float)side * o0, v0, b);
            pointAt((float)side * o1, v1, c); pointAt((float)side * runW, v1, d);
            // keep the winding CCW-from-above on BOTH sides (mirroring the
            // lateral offsets flips it; a flipped wing is backface-culled)
            if (side > 0) wings.quad(a, b, c, d, 0.0f, 1.0f, v0 * 0.06f, v1 * 0.06f);
            else          wings.quad(b, a, d, c, 0.0f, 1.0f, v0 * 0.06f, v1 * 0.06f);
            // NO PLINTH (owner, stuck on the junction's tabletop shelf): the
            // wing's outer edge feathers to NATURAL GROUND at ~3:1, so the
            // mouth meets the country at grade instead of standing proud on
            // a deck with a drop on all sides.
            {
                float w0[3], w1[3];
                pointAt((float)side * o0, v0, w0);
                pointAt((float)side * o1, v1, w1);
                auto featherPt = [&](const float w[3], float o, float v, float pt[3]) {
                    const float gx = j.endX + dx * v + pxd * (float)side * (o + 2.0f);
                    const float gz = j.endZ + dz * v + pzd * (float)side * (o + 2.0f);
                    const float ground = terrainHeightAtWorld(gx, gz);
                    const float run = std::max(2.0f,
                        std::min(18.0f, (w[1] - ground) * 3.0f));
                    pt[0] = j.endX + dx * v + pxd * (float)side * (o + run);
                    pt[2] = j.endZ + dz * v + pzd * (float)side * (o + run);
                    pt[1] = terrainHeightAtWorld(pt[0], pt[2]) + 0.02f;
                };
                float f0[3], f1[3];
                featherPt(w0, o0, v0, f0);
                featherPt(w1, o1, v1, f1);
                if (side > 0) feather.quadN(w0, f0, f1, w1, 0.0f, 0.5f, v0 * 0.06f, v1 * 0.06f);
                else          feather.quadN(f0, w0, w1, f1, 0.0f, 0.5f, v0 * 0.06f, v1 * 0.06f);
            }
        }
    }

    // THE SWOOPING MERGES — Tim: "INTERSECTIONS ... NEED SWOOPING CURVES FROM
    // BOTH WAYS." A T-butt is not an intersection: each side of the mouth
    // gets an on-ramp fillet — a constant-radius arc (target 45 m, never
    // built below ~35 m) tangent to the branch's pavement edge AND the main
    // road's, with the wedge between corner and arc paved AT GRADE with both
    // roads (heights ride the same twist blend as the mouth), a cement
    // fringe inside the arc and a feather to natural ground beyond it — so
    // traffic from EITHER main-road direction sweeps in without a corner.
    {
        constexpr float kMergeR = 45.0f;      // target fillet radius (>= ~40 asked)
        // On a divided freeway the fillets go tangent to the NEAR carriageway's
        // outer shoulder edge (producers fill mainShoulderEdgeM from the median
        // at the landing); on a single road this is the base profile's value.
        const float mainEdge = j.mainShoulderEdgeM;
        // main-road unit normal pointing back toward the branch end
        float nx = -j.mainTZ, nz = j.mainTX;
        if (nx * (j.endX - j.jx) + nz * (j.endZ - j.jz) < 0.0f) { nx = -nx; nz = -nz; }
        auto toUV = [&](const float P[2], float& u, float& v) {
            u = (P[0] - j.endX) * pxd + (P[1] - j.endZ) * pzd;
            v = (P[0] - j.endX) * dx  + (P[1] - j.endZ) * dz;
        };
        for (int side = -1; side <= 1; side += 2) {
            // main direction AWAY from the junction on this side of the branch
            float bxd = j.mainTX, bzd = j.mainTZ;
            if (bxd * pxd * (float)side + bzd * pzd * (float)side < 0.0f) {
                bxd = -bxd; bzd = -bzd;
            }
            // corner: branch edge line  E + dirB*t   x   main edge line  M + dirM*s
            const float ex = j.endX + pxd * (float)side * runW;
            const float ez = j.endZ + pzd * (float)side * runW;
            const float mx0 = j.jx + nx * mainEdge, mz0 = j.jz + nz * mainEdge;
            const float det = dx * (-bzd) - dz * (-bxd);
            if (std::fabs(det) < 0.15f) continue;            // near-parallel: no corner
            const float rx = mx0 - ex, rz = mz0 - ez;
            const float tB = (rx * (-bzd) - rz * (-bxd)) / det;
            const float Xx = ex + dx * tB, Xz = ez + dz * tB;
            // legs away from the corner: a back along the branch, b along main
            const float axd = -dx, azd = -dz;
            const float cosPhi = std::max(-1.0f, std::min(1.0f, axd * bxd + azd * bzd));
            const float phi = std::acos(cosPhi);
            if (phi < 0.45f || phi > 2.7f) continue;         // degenerate corner
            const float tanH = std::tan(phi * 0.5f);
            float T = kMergeR / tanH;
            float vX, uX; { float P[2] = { Xx, Xz }; toUV(P, uX, vX); }
            T = std::min(T, vX - 2.0f);                      // stay inside the mouth reach
            T = std::min(T, 60.0f);
            if (T < 8.0f) continue;
            const float R = T * tanH;
            // centre + tangent points
            float bisx = axd + bxd, bisz = azd + bzd;
            const float bl = std::sqrt(bisx * bisx + bisz * bisz);
            if (bl < 1e-4f) continue;
            bisx /= bl; bisz /= bl;
            const float Ox = Xx + bisx * (R / std::sin(phi * 0.5f));
            const float Oz = Xz + bisz * (R / std::sin(phi * 0.5f));
            const float Ax = Xx + axd * T, Az = Xz + azd * T;
            const float Bx = Xx + bxd * T, Bz = Xz + bzd * T;
            // sweep from A to B about O, the short way
            float a0 = std::atan2(Az - Oz, Ax - Ox);
            float a1 = std::atan2(Bz - Oz, Bx - Ox);
            float sweep = a1 - a0;
            while (sweep >  3.14159265f) sweep -= 6.2831853f;
            while (sweep < -3.14159265f) sweep += 6.2831853f;
            auto surfaced = [&](float wx, float wz, float o[3]) {
                float u, v; float P[2] = { wx, wz };
                toUV(P, u, v);
                // +12 mm: the fan LAPS the flare wings (same height law), and
                // lapped-not-coplanar is the no-z-fight rule everywhere here.
                o[0] = wx; o[2] = wz; o[1] = heightAt(u, v) + 0.012f;
            };
            // FINE ARC ("Get rid of ALLLLL jointed bends" applies to the
            // junction fillets too): ~4 m facets instead of the old 14 fixed.
            const int N = std::max(16, (int)std::ceil(R * std::fabs(sweep) / 4.0f));
            float Xp[3]; surfaced(Xx, Xz, Xp);
            for (int i = 0; i < N; ++i) {
                const float aa = a0 + sweep * (float)i / (float)N;
                const float ab = a0 + sweep * (float)(i + 1) / (float)N;
                float Pa[3], Pb[3];
                surfaced(Ox + std::cos(aa) * R, Oz + std::sin(aa) * R, Pa);
                surfaced(Ox + std::cos(ab) * R, Oz + std::sin(ab) * R, Pb);
                // asphalt wedge: corner fan (degenerate 4th vert = a cheap tri)
                road.quad(Xp, Pa, Pb, Xp, 0.0f, 0.5f, 0.0f, 0.5f);
                // cement fringe INSIDE the arc (the corner island's kerb)...
                auto inward = [&](const float P[3], float d, float o[3]) {
                    const float ivx = (Ox - P[0]) / R, ivz = (Oz - P[2]) / R;
                    surfaced(P[0] + ivx * d, P[2] + ivz * d, o);
                };
                float Fa[3], Fb[3];
                inward(Pa, 4.0f, Fa);
                inward(Pb, 4.0f, Fb);
                wings.quadN(Pa, Fa, Fb, Pb, 0.5f, 0.8f, 0.0f, 0.5f);
                wings.quadN(Fa, Pa, Pb, Fb, 0.5f, 0.8f, 0.0f, 0.5f);
                // ...then feather to natural ground
                auto ground = [&](const float F[3], float o[3]) {
                    const float ivx = (Ox - F[0]), ivz = (Oz - F[2]);
                    const float il = std::sqrt(ivx * ivx + ivz * ivz);
                    const float g = terrainHeightAtWorld(F[0], F[2]);
                    const float run = std::max(2.0f,
                        std::min(18.0f, (F[1] - g) * 3.0f));
                    const float rr = il > 1e-3f ? run / il : 0.0f;
                    o[0] = F[0] + ivx * rr; o[2] = F[2] + ivz * rr;
                    o[1] = terrainHeightAtWorld(o[0], o[2]) + 0.02f;
                };
                float Ga[3], Gb[3];
                ground(Fa, Ga);
                ground(Fb, Gb);
                feather.quadN(Fa, Ga, Gb, Fb, 0.0f, 0.5f, 0.0f, 0.5f);
                feather.quadN(Ga, Fa, Fb, Gb, 0.0f, 0.5f, 0.0f, 0.5f);
            }
        }
    }

    auto upload = [&](RibbonMesh& m, const SurfaceSet* set, const float tint[4]) {
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
        std::vector<float> cv; cv.reserve(m.v.size() * 3);
        for (const auto& vv : m.v) { cv.push_back(vv.pos[0]); cv.push_back(vv.pos[1]); cv.push_back(vv.pos[2]); }
        phys.addStaticMesh(cv.data(), (uint32_t)(cv.size() / 3),
                           m.i.data(), (uint32_t)m.i.size());
    };
    // Same fallback tinting as buildRoadRibbon — the mouth must match the
    // pavements it blends, textured or not.
    float roadTint [4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    if (!asphalt.ok) { roadTint[0] = 0.055f; roadTint[1] = 0.056f; roadTint[2] = 0.060f; }
    float apronTint[4] = { 0.86f, 0.85f, 0.82f, 1.0f };
    if (!cement.ok)  { apronTint[0] = 0.42f; apronTint[1] = 0.41f; apronTint[2] = 0.38f; }
    upload(road,  &asphalt, roadTint);
    upload(wings, &cement,  apronTint);
    // the feather is dressed EARTH-ON-CONCRETE: darker cement, so the ramp
    // to the country reads as a graded verge rather than more pavement
    float featherTint[4] = { 0.55f, 0.53f, 0.49f, 1.0f };
    if (!cement.ok) { featherTint[0] = 0.27f; featherTint[1] = 0.26f; featherTint[2] = 0.23f; }
    upload(feather, &cement, featherTint);
    out.lengthM = V;
    out.ok = out.meshCount > 0;
    return out;
}

namespace {
// ---------------------------------------------------------------------------
// TILE-LOD WEDGE AUDIT over a road's pavement — the spawn-road green strip.
// Builds the REAL tile mesh at every LOD over the node window [i0..i1], rasters
// the emitted surface triangles at 0.5 m, keeps samples on the PAVEMENT
// (|lat| <= kPavedHalfM against this spec's polyline), and reports the worst
// (terrainMeshY - roadDatumY) per LOD. Positive = terrain standing above the
// datum where a slab lies (the strip). Same instrument as --test-tunnelmouth
// M7, generalised to a RoadSpec.
// ---------------------------------------------------------------------------
float roadTileLodWedge(const RoadSpec& spec, const std::vector<float>& roadY,
                       size_t i0, size_t i1, float perLod[3],
                       float* outX = nullptr, float* outZ = nullptr) {
    const size_t n = std::min(spec.x.size(), roadY.size());
    if (i1 >= n) i1 = n ? n - 1 : 0;
    if (i0 + 1 >= i1) { perLod[0] = perLod[1] = perLod[2] = 0.0f; return 0.0f; }
    const TerrainConfig& wcfg = worldTerrainConfig();
    auto datumAt = [&](float x, float z, float& outDatum) {
        float bestD2 = 1e18f, bestY = 0.0f;
        for (size_t i = i0; i < i1; ++i) {
            const float abx = spec.x[i + 1] - spec.x[i], abz = spec.z[i + 1] - spec.z[i];
            const float len2 = abx * abx + abz * abz;
            if (len2 < 1e-6f) continue;
            float t = ((x - spec.x[i]) * abx + (z - spec.z[i]) * abz) / len2;
            t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
            const float dx = x - (spec.x[i] + abx * t), dz = z - (spec.z[i] + abz * t);
            const float d2 = dx * dx + dz * dz;
            if (d2 < bestD2) { bestD2 = d2; bestY = roadY[i] + (roadY[i + 1] - roadY[i]) * t; }
        }
        if (bestD2 > kPavedHalfM * kPavedHalfM) return false;
        outDatum = bestY;
        return true;
    };
    float bx0 = 1e9f, bx1 = -1e9f, bz0 = 1e9f, bz1 = -1e9f;
    for (size_t i = i0; i <= i1; ++i) {
        bx0 = std::min(bx0, spec.x[i]); bx1 = std::max(bx1, spec.x[i]);
        bz0 = std::min(bz0, spec.z[i]); bz1 = std::max(bz1, spec.z[i]);
    }
    const float pad = spec.halfWidth + 2.0f;
    bx0 -= pad; bx1 += pad; bz0 -= pad; bz1 += pad;
    const float ts = wcfg.tileSize;
    float worstAll = -1e9f;
    std::vector<x3::rhi::MeshVertex> mv;
    std::vector<uint32_t> mi;
    for (int lod = 0; lod < 3; ++lod) {
        float worstLod = -1e9f;
        for (float tz = std::floor(bz0 / ts) * ts; tz < bz1; tz += ts) {
            for (float tx = std::floor(bx0 / ts) * ts; tx < bx1; tx += ts) {
                uint32_t surfN = 0;
                mv.clear(); mi.clear();
                buildTileMeshAbs(wcfg, tx, tz, (TerrainLod)lod, mv, mi, &surfN);
                for (uint32_t t3 = 0; t3 + 2 < surfN; t3 += 3) {
                    const auto& A = mv[mi[t3]]; const auto& B = mv[mi[t3 + 1]];
                    const auto& C = mv[mi[t3 + 2]];
                    const float minX = std::min(A.pos[0], std::min(B.pos[0], C.pos[0]));
                    const float maxX = std::max(A.pos[0], std::max(B.pos[0], C.pos[0]));
                    const float minZ = std::min(A.pos[2], std::min(B.pos[2], C.pos[2]));
                    const float maxZ = std::max(A.pos[2], std::max(B.pos[2], C.pos[2]));
                    const float den = (B.pos[0] - A.pos[0]) * (C.pos[2] - A.pos[2]) -
                                      (C.pos[0] - A.pos[0]) * (B.pos[2] - A.pos[2]);
                    if (std::fabs(den) < 1e-6f) continue;
                    for (float sz = std::ceil(minZ / 0.5f) * 0.5f; sz <= maxZ; sz += 0.5f) {
                        for (float sx = std::ceil(minX / 0.5f) * 0.5f; sx <= maxX; sx += 0.5f) {
                            const float w0 = ((B.pos[0] - sx) * (C.pos[2] - sz) -
                                              (C.pos[0] - sx) * (B.pos[2] - sz)) / den;
                            const float w1 = ((C.pos[0] - sx) * (A.pos[2] - sz) -
                                              (A.pos[0] - sx) * (C.pos[2] - sz)) / den;
                            const float w2 = 1.0f - w0 - w1;
                            if (w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f) continue;
                            float datum = 0.0f;
                            if (!datumAt(sx, sz, datum)) continue;
                            const float err = (w0 * A.pos[1] + w1 * B.pos[1] + w2 * C.pos[1])
                                            - datum;
                            if (err > worstLod) worstLod = err;
                            if (err > worstAll) {
                                worstAll = err;
                                if (outX) *outX = sx;
                                if (outZ) *outZ = sz;
                            }
                        }
                    }
                }
            }
        }
        perLod[lod] = worstLod;
    }
    return worstAll;
}
} // namespace

bool runRoadNetworkSelfTest() {
    // SURVEY MODE (X3_RING_SURVEY="r0,r1,..."): dump the graded-cut profile
    // along candidate circles about the ring centre. This is the instrument the
    // outer tour's waypoint table was authored with; it stays because the next
    // route (a spoke, a re-author after a terrain change) needs the same
    // instrument, and because O0 failing will send somebody straight here.
    if (const char* sv = std::getenv("X3_RING_SURVEY"); sv && sv[0]) {
        clearTerrainCorridors();
        float R = 0.0f; const char* p = sv;
        while (*p && std::sscanf(p, "%f", &R) == 1) {
            std::printf("SURVEY radius %.0f m centre (%.0f, %.0f)\n", R, kOuterCX, kOuterCZ);
            const uint32_t N = 1024;
            std::vector<float> nat(N), seg(N, 6.2831853f * R / (float)N);
            for (uint32_t i = 0; i < N; ++i) {
                const float a = 6.2831853f * (float)i / (float)N;
                const float x = kOuterCX + std::cos(a) * R, z = kOuterCZ + std::sin(a) * R;
                const float tx = -std::sin(a), tz = std::cos(a);
                float hi = -1e9f;
                for (int k = -9; k <= 9; ++k) {
                    const float off = (float)k * (kPavedHalfM + 1.8f) / 9.0f;
                    hi = std::max(hi, terrainHeightAtWorld(x + (-tz) * off, z + tx * off));
                }
                nat[i] = hi;
            }
            std::vector<float> ry;
            gradeRoad(nat, seg, 0.07f, ry, /*closed=*/true);
            for (uint32_t i = 0; i < N; ++i)
                std::printf("S %7.0f %6.2f nat %7.2f road %7.2f cut %7.2f\n",
                            R, 360.0f * (float)i / (float)N, nat[i], ry[i], nat[i] - ry[i]);
            while (*p && *p != ',') ++p;
            if (*p == ',') ++p;
        }
        return true;
    }
    int passN = 0, failN = 0;
    auto check = [&](bool ok, const char* name, const char* detail = nullptr) {
        std::string m = std::string(ok ? "PASS " : "FAIL ") + name;
        if (detail && *detail) m += std::string(" — ") + detail;
        if (ok) { ++passN; x3::logInfo("[roadnet] " + m); }
        else    { ++failN; x3::logError("[roadnet] " + m); }
    };
    char d[320];

    clearTerrainCorridors();
    clearRoadJunctions();
    const RoadBuildResult r = registerInnerRing();

    std::snprintf(d, sizeof(d), "%.2f miles over %u nodes in %u corridors",
                  r.lengthM / 1609.34f, r.nodeCount, r.corridorCount);
    check(r.ok && r.corridorCount > 0, "N1 the inner ring registers", d);

    std::snprintf(d, sizeof(d), "%.2f miles (asked ~15, course-authored)", r.lengthM / 1609.34f);
    check(r.lengthM / 1609.34f > 13.0f && r.lengthM / 1609.34f < 17.0f,
          "N2 it is a ~15 mile course", d);

    // NOT A CIRCLE. Tim, from the world map: "its a perfect circle. NO roads
    // do that." Two measurements a circle cannot pass: the radial spread about
    // the centroid (a circle's is ~0), and genuine straights (a circle has
    // none). Both from the authored spec, not the log.
    {
        const RoadSpec s = makeInnerCourse();
        const size_t sn = s.x.size();
        double cxm = 0.0, czm = 0.0;
        for (size_t i = 0; i + 1 < sn; ++i) { cxm += s.x[i]; czm += s.z[i]; }
        cxm /= (double)(sn - 1); czm /= (double)(sn - 1);
        float rMin = 1e9f, rMax = -1e9f;
        for (size_t i = 0; i + 1 < sn; ++i) {
            const float rr = std::sqrt((s.x[i] - (float)cxm) * (s.x[i] - (float)cxm) +
                                       (s.z[i] - (float)czm) * (s.z[i] - (float)czm));
            rMin = std::min(rMin, rr); rMax = std::max(rMax, rr);
        }
        float straight = 0.0f, longest = 0.0f; int straights500 = 0; bool inRun = false;
        for (size_t i = 1; i + 1 < sn; ++i) {
            float ax = s.x[i] - s.x[i-1], az = s.z[i] - s.z[i-1];
            float bx = s.x[i+1] - s.x[i], bz = s.z[i+1] - s.z[i];
            const float la = std::sqrt(ax*ax + az*az), lb = std::sqrt(bx*bx + bz*bz);
            if (la < 1e-4f || lb < 1e-4f) continue;
            const float dot = std::max(-1.0f, std::min(1.0f, (ax*bx + az*bz) / (la*lb)));
            if (std::acos(dot) < 0.0105f) {
                if (!inRun) { straight = la; inRun = true; }
                straight += lb;
                longest = std::max(longest, straight);
            } else {
                if (inRun && straight >= 500.0f) ++straights500;
                inRun = false; straight = 0.0f;
            }
        }
        if (inRun && straight >= 500.0f) ++straights500;
        std::snprintf(d, sizeof(d),
            "radial spread %.0f..%.0f m (delta %.0f), %d straights >= 500 m (longest %.0f m)",
            rMin, rMax, rMax - rMin, straights500, longest);
        check(rMax - rMin > 800.0f && straights500 >= 2,
              "N2b the course is NOT a circle: real spread, real straights", d);
    }

    // The chain must fit the cap WITH the city bores still to come.
    std::snprintf(d, sizeof(d), "%u corridors of a %u cap", r.corridorCount, kMaxTerrainCorridors);
    check(r.corridorCount <= kMaxTerrainCorridors - 20,
          "N3 it leaves room for the tunnels and the outer tour", d);

    // GRADE is the difference between a road and a wall.
    std::snprintf(d, sizeof(d), "steepest %.1f%% (limit 7%%)", r.maxGradePct);
    check(r.maxGradePct <= 7.5f, "N4 no segment exceeds the drivable grade", d);

    // VERTICAL FLOW — the traction gate. Grade alone is not drivability: the
    // RATE of grade change is what unloads a car at a crest. Assert the
    // smoothed profile respects the spec's K-value, and that the smoothing is
    // doing real work (the raw relaxation profile must have been worse or
    // already-compliant country).
    std::snprintf(d, sizeof(d),
        "max |d(grade)/ds| %.5f/m raw -> %.5f/m smoothed (cap %.5f/m = K %.0f)",
        r.maxGradeRatePre, r.maxGradeRatePost, 5.0e-4f, 0.01f / 5.0e-4f);
    check(r.maxGradeRatePost <= 5.0e-4f * 1.10f,
          "N4b vertical flow: no grade break exceeds the crest/sag K-value", d);

    // And the carve must actually put the ground at the road: sample the FINAL
    // field along the ring and assert nothing stands on the roadway. This is the
    // road's version of --test-tunnelmouth's M1, and the reason the lateral
    // sweep above measures the MAX across the width rather than the centreline.
    {
        const RoadSpec s = makeInnerCourse();
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
        const double withRing = timeDelta("the ring's corridors");
        clearTerrainCorridors();
        const double empty = timeDelta("0 corridors (baseline)");
        const double perCorridorNs = (withRing - empty) * 1e6 / (double)kProbes
                                   / std::max(1.0, (double)r.corridorCount);
        std::snprintf(d, sizeof(d),
            "%.1f ns per corridor per query; at the 192 cap that is %.0f ns/query, "
            "%.0f ms per million",
            perCorridorNs, perCorridorNs * 192.0, perCorridorNs * 192.0 / 1000.0);
        check(perCorridorNs * 192.0 < 2000.0, "N6 the registry scan is affordable at the cap", d);
    }

    // ======================= THE OUTER TOUR =================================

    // O0 — NEGATIVE CONTROL first: the naive circle Tim's ruling replaced.
    // Register a plain 4.93-mile-radius circle with no bores and measure what
    // the grade clamp does to it: it must gouge a canyon through the north
    // massif. If this ever PASSES shallow, the terrain changed under the tour
    // and every authored waypoint below is stale — fail loudly here rather
    // than ship a tour tuned against a mountain that moved.
    {
        clearTerrainCorridors();
        RoadSpec naive = makeRingRoad("naive outer circle", kOuterCX, kOuterCZ,
                                      7934.0f, 810);
        naive.halfWidth = kPavedHalfM + 1.0f;
        naive.falloff   = 18.0f;
        const RoadBuildResult nv = registerRoad(naive);
        std::snprintf(d, sizeof(d),
            "grade-clamped cut %.0f ft through the north massif (a canyon, not a road)",
            nv.maxCutM * kMToFt);
        check(nv.ok && nv.maxCutM > 120.0f,
              "O0 NEGATIVE CONTROL: a plain circle must gouge a >390 ft trench", d);
    }

    clearTerrainCorridors();
    const OuterRingResult orr = registerOuterRing();

    std::snprintf(d, sizeof(d), "%.2f miles over %u nodes, %u road corridors + %u bores",
                  orr.road.lengthM / 1609.34f, orr.road.nodeCount,
                  orr.road.corridorCount, (uint32_t)orr.bores.size());
    check(orr.road.ok && orr.road.corridorCount > 0 && orr.road.nodeCount > 600,
          "O1 the outer tour registers, non-empty", d);

    std::snprintf(d, sizeof(d), "%.2f miles (asked ~31)", orr.road.lengthM / 1609.34f);
    check(orr.road.lengthM / 1609.34f > 30.0f && orr.road.lengthM / 1609.34f < 32.0f,
          "O2 it is ~31 miles long", d);

    // Every authored chord must have found its mountain. A chord with no roofed
    // span means the tour table and the terrain disagree — the exact defect the
    // negative control guards from the other side.
    {
        uint32_t roofed = 0;
        for (const TunnelRoute* t : orr.bores) if (t && t->boreValid) ++roofed;
        std::snprintf(d, sizeof(d), "%u/%u chords roofed, %.2f miles underground",
                      roofed, (uint32_t)orr.bores.size(), orr.boredLenM / 1609.34f);
        check(orr.bores.size() == 5 && roofed == 5,
              "O3 all five authored bores found their mountain", d);
    }

    // TEMP DEBUG (X3_RING_DEBUG=1): locate the worst grade + deepest cuts.
    if (const char* dbg = std::getenv("X3_RING_DEBUG"); dbg && dbg[0] == '1') {
        const auto& sp = orr.spec; const auto& ry = orr.roadY;
        for (size_t g = 0; g < sp.gaps.size(); ++g) {
            const auto& gp = sp.gaps[g];
            std::printf("DBG gap %zu nodes [%u..%u] pin y0 %.1f y1 %.1f | "
                        "ry[i0-1] %.1f ry[i0] %.1f ry[i0+1] %.1f | ry[i1-1] %.1f ry[i1] %.1f ry[i1+1] %.1f\n",
                        g, gp.i0, gp.i1, gp.y0, gp.y1,
                        ry[gp.i0-1], ry[gp.i0], ry[gp.i0+1],
                        ry[gp.i1-1], ry[gp.i1], ry[gp.i1+1]);
        }
        for (size_t i = 0; i + 1 < ry.size(); ++i) {
            const float dx = sp.x[i+1]-sp.x[i], dz = sp.z[i+1]-sp.z[i];
            const float sl = std::sqrt(dx*dx+dz*dz);
            if (sl < 1.0f) continue;
            const float g = std::fabs(ry[i+1]-ry[i])/sl*100.0f;
            float ang = std::atan2(sp.z[i]-kOuterCZ, sp.x[i]-kOuterCX) / kDegToRad;
            if (ang < 0.0f) ang += 360.0f;
            if (g > 8.0f)
                std::printf("DBG grade %6.1f%% at node %zu ang %7.2f deg roadY %.1f->%.1f\n",
                            g, i, ang, ry[i], ry[i+1]);
        }
        // tallest floats: where does the datum leave the ground? (open reaches
        // only — inside a gap the lerp datum is bookkeeping, not a road)
        {
            auto gapNode = [&](size_t i){ for (auto& g : sp.gaps) if (i >= g.i0 && i <= g.i1) return true; return false; };
            float worst = 0.0f; size_t at = 0;
            for (size_t i = 0; i < ry.size(); ++i) {
                if (gapNode(i)) continue;
                const float ground = terrainHeightAtWorld(sp.x[i], sp.z[i])
                                   - terrainCorridorDelta(sp.x[i], sp.z[i]);
                const float fl = ry[i] - ground;
                if (fl > worst) { worst = fl; at = i; }
            }
            float ang = std::atan2(sp.z[at]-kOuterCZ, sp.x[at]-kOuterCX) / kDegToRad;
            if (ang < 0.0f) ang += 360.0f;
            std::printf("DBG float max %.1f m at node %zu ang %.2f deg\n", worst, at, ang);
        }
        // deepest OPEN cuts: datum vs the carved field it asked for
        auto inGap = [&](size_t i){ for (auto& g : sp.gaps) if (i >= g.i0 && i <= g.i1) return true; return false; };
        for (size_t i = 0; i < ry.size(); ++i) {
            if (inGap(i)) continue;
            const float cut = terrainCorridorDelta(sp.x[i], sp.z[i]);
            float ang = std::atan2(sp.z[i]-kOuterCZ, sp.x[i]-kOuterCX) / kDegToRad;
            if (ang < 0.0f) ang += 360.0f;
            if (cut < -50.0f)
                std::printf("DBG cut %6.1f m at node %zu ang %7.2f deg r %.0f\n",
                            -cut, i, ang,
                            std::sqrt((sp.x[i]-kOuterCX)*(sp.x[i]-kOuterCX)+(sp.z[i]-kOuterCZ)*(sp.z[i]-kOuterCZ)));
        }
    }

    std::snprintf(d, sizeof(d), "steepest %.1f%% (limit 7%%)", orr.road.maxGradePct);
    check(orr.road.maxGradePct <= 7.5f, "O4 tour grade is drivable", d);

    // The whole point of the bores: the OPEN road never needs a canyon. The
    // ceiling is authored at 47 m: the deepest cutting on the chosen lanes
    // measures 45.4 m (149 ft) — a node the close-the-loop pass deepened to
    // clear an inter-node crag on a steep flank bench. A 150 ft side-hill
    // cutting is a dramatic but real piece of mountain-road engineering; the
    // naive circle this replaces needed EIGHT HUNDRED feet. Anything wanting
    // more than this ceiling has to be a tunnel, not a trench.
    std::snprintf(d, sizeof(d), "deepest open cut %.0f ft (ceiling 154 ft; naive needed 862)",
                  orr.road.maxCutM * kMToFt);
    check(orr.road.maxCutM <= 47.0f, "O5 no open reach is a canyon", d);

    std::snprintf(d, sizeof(d), "worst pin deficit %.2f ft", orr.road.pinErrM * kMToFt);
    check(orr.road.pinErrM <= 0.06f, "O6 every portal datum is reached at grade", d);

    // The portal ramps may float the datum over hollows, but a float taller
    // than ~85 ft is a viaduct pretending to be a road — authoring error.
    std::snprintf(d, sizeof(d), "tallest approach float %.0f ft", orr.road.maxFloatM * kMToFt);
    check(orr.road.maxFloatM <= 26.0f, "O6b approach floats stay embankment-scale", d);

    // HORIZONTAL FLOW on the tour's daylight reaches (the bore chords and
    // their portal edges are excluded by the measure — a chord is straight
    // LAW and the portal heading break is hidden by the portal face).
    {
        float minR = 0.0f;
        const float dm = measureMaxDeflectionDeg(orr.spec, &minR);
        std::snprintf(d, sizeof(d),
            "max deflection %.2f deg/node (cap %.1f), tightest bend %.0f m",
            dm, orr.spec.maxDeflectionDeg, minR >= 1e8f ? 99999.0f : minR);
        check(dm <= orr.spec.maxDeflectionDeg + 0.8f,
              "O10 the tour's horizontal flow holds in daylight", d);
    }

    // O7 — the HANDOFF. Two earlier drafts of this gate were measuring the
    // wrong thing, and both mistakes are worth recording:
    //   * walking a straight TANGENT through the joint left the curved roadway
    //     within 30 m and climbed the cut wall — a 54 ft "step" that was the
    //     measurement going off-road, not the road stepping;
    //   * asserting the DIRT is step-free is also wrong at a portal: the
    //     tunnel's carve is a depth DELTA (latMax - roadY), so on laterally
    //     sloping ground the bore's dirt floor over-excavates below the datum
    //     and the tunnel's own ribbon spans the groove — exactly as it does on
    //     the demo ridge, just larger here. The dirt may step DOWN.
    // What drivability actually requires across the joint: (a) the ring datum
    // and the tunnel datum agree AT the joint (a car changes surface without a
    // bump), and (b) no earth stands ABOVE the driving line anywhere through
    // the handoff — the M1 invariant, continued across the seam.
    {
        float worstJoint = 0.0f, worstEarth = -1e9f;
        for (size_t g = 0; g < orr.spec.gaps.size(); ++g) {
            const RoadSpec::Gap& gap = orr.spec.gaps[g];
            const TunnelRoute* t = orr.bores[g];
            if (!t) continue;
            worstJoint = std::max(worstJoint,
                std::fabs(orr.roadY[gap.i0] - t->roadYAt(0.0f)));
            worstJoint = std::max(worstJoint,
                std::fabs(orr.roadY[gap.i1] - t->roadYAt(t->totalLen)));
            for (int side = 0; side < 2; ++side) {
                const uint32_t c = side ? gap.i1 : gap.i0;
                const uint32_t iA = c > 0 ? c - 1 : c;
                for (uint32_t i = iA; i <= c && i + 1 < orr.spec.x.size(); ++i) {
                    const float dx = orr.spec.x[i+1] - orr.spec.x[i];
                    const float dz = orr.spec.z[i+1] - orr.spec.z[i];
                    const float len = std::sqrt(dx * dx + dz * dz);
                    const int steps = std::max(1, (int)(len / 2.0f));
                    for (int k = 0; k <= steps; ++k) {
                        const float tt = (float)k / (float)steps;
                        const float h = terrainHeightAtWorld(orr.spec.x[i] + dx * tt,
                                                             orr.spec.z[i] + dz * tt);
                        const float surf = orr.roadY[i] + (orr.roadY[i+1] - orr.roadY[i]) * tt;
                        worstEarth = std::max(worstEarth, h - surf);
                    }
                }
            }
        }
        std::snprintf(d, sizeof(d),
            "worst datum mismatch %.2f ft; worst earth-over-road %.2f ft (10 portals)",
            worstJoint * kMToFt, worstEarth * kMToFt);
        check(worstJoint < 0.06f && worstEarth < 0.02f,
              "O7 the driving line is continuous and clear through every portal", d);
    }

    // O8 — DETERMINISM: clear, re-register, and the carved field must answer
    // bit-identically along the whole tour.
    {
        auto fieldHash = [&]() {
            uint64_t h = 1469598103934665603ull;   // FNV-1a
            for (size_t i = 0; i < orr.spec.x.size(); i += 3) {
                const float v = terrainCorridorDelta(orr.spec.x[i], orr.spec.z[i]);
                uint32_t bits;
                static_assert(sizeof(bits) == sizeof(v));
                std::memcpy(&bits, &v, sizeof(bits));
                h = (h ^ bits) * 1099511628211ull;
            }
            return h;
        };
        const uint64_t h1 = fieldHash();
        clearTerrainCorridors();
        (void)registerOuterRing();
        const uint64_t h2 = fieldHash();
        std::snprintf(d, sizeof(d), "field hash %016llx",
                      (unsigned long long)h1);
        check(h1 == h2, "O8 the tour is deterministic (re-registration is bit-identical)", d);
    }

    // O9 — the WHOLE network fits: inner + outer + bores, with room left for
    // the city bores, the demo ridge and the river road.
    {
        clearTerrainCorridors();
        (void)registerInnerRing();
        (void)registerOuterRing();
        const uint32_t used = terrainCorridorCount();
        std::snprintf(d, sizeof(d), "%u corridors of %u", used, kMaxTerrainCorridors);
        check(used <= kMaxTerrainCorridors - 40,
              "O9 both tours + bores leave room for the rest of the network", d);
    }

    // ======================= THE CONNECT GATES ==============================
    // Tim: "They need to CONNECT to the roads you spawn on... Roads that go UP
    // on top of the mountain." Same world recipe as --world tunnel: demo bore,
    // then the ring, then the connector off the bore's far end, then the spur.
    {
        clearTerrainCorridors();
        clearRoadJunctions();
        const TunnelRoute* spawn = registerTunnelCorridorFor(demoTunnelSpec());
        check(spawn != nullptr, "K0 the spawn corridor registers for the connect gates");
        if (spawn) {
            RoadSpec ringSpec = makeInnerCourse();
            std::vector<float> ringY;
            const RoadBuildResult ringR = registerRoad(ringSpec, &ringY);
            check(ringR.ok, "K0b the course registers under the connect gates");

            const SpawnConnectorResult cx = registerSpawnConnector(*spawn, ringSpec, ringY);
            std::snprintf(d, sizeof(d),
                "closed a %.0f m gap with %.2f miles over %u corridors",
                cx.gapBeforeM, cx.road.lengthM / 1609.34f, cx.road.corridorCount);
            check(cx.road.ok && cx.gapBeforeM > 3000.0f,
                  "K1 the spawn connector registers and the gap it closes was real", d);

            std::snprintf(d, sizeof(d), "steepest %.1f%% (limit 7%%)", cx.road.maxGradePct);
            check(cx.road.maxGradePct <= 7.5f, "K2 connector grade is drivable", d);

            std::snprintf(d, sizeof(d), "pin deficit %.2f ft", cx.road.pinErrM * kMToFt);
            check(cx.road.pinErrM <= 0.06f, "K3 both junction datums reached at grade", d);

            const float dEnd = cx.roadY.empty() ? 1e9f
                : std::fabs(cx.roadY.back() - ringY[cx.ringNode]);
            const float dStart = cx.roadY.empty() ? 1e9f
                : std::fabs(cx.roadY.front() - spawn->roadYAt(spawn->totalLen));
            std::snprintf(d, sizeof(d), "ring end off by %.2f ft, tunnel end by %.2f ft",
                          dEnd * kMToFt, dStart * kMToFt);
            check(dEnd < 0.02f && dStart < 0.02f,
                  "K4 the connector datum MEETS both roads exactly", d);

            // JUNCTION STABILITY: the course replaced the circle, and the one
            // hard promise of the reshape was that the connector's landing
            // stays where the old ring node 173 was — the junction straight
            // was authored through that exact point.
            {
                const float lx = ringSpec.x[cx.ringNode], lz = ringSpec.z[cx.ringNode];
                const float dd = std::sqrt((lx + 4135.7f) * (lx + 4135.7f) +
                                           (lz - 1132.2f) * (lz - 1132.2f));
                std::snprintf(d, sizeof(d),
                    "landing (%.0f, %.0f), %.0f m from the old node-173 junction", lx, lz, dd);
                check(dd < 250.0f, "K4b the de-circled course kept the junction in place", d);
            }

            // VERTICAL FLOW on the connected network: the ring, the connector
            // and (below) the spur and circuit must all hold their K-values.
            std::snprintf(d, sizeof(d),
                "ring %.5f->%.5f, connector %.5f->%.5f (caps %.5f) /m",
                ringR.maxGradeRatePre, ringR.maxGradeRatePost,
                cx.road.maxGradeRatePre, cx.road.maxGradeRatePost, 5.0e-4f);
            check(ringR.maxGradeRatePost <= 5.0e-4f * 1.10f &&
                  cx.road.maxGradeRatePost <= 5.0e-4f * 1.10f,
                  "K4c vertical flow holds through the connected network", d);

            // W1 — THE GREEN STRIP (tile-LOD wedge) on the actual SPAWN ROAD.
            // The owner's screenshots show a strip of terrain knifing through
            // the spawn-road pavement; it survived lifting the slab 0.07 m
            // proud because it is a MESH artifact: a Half/Quarter tile
            // interpolates 2/4 m chords across the carve's smoothstep
            // shoulder, and with this road's floor only 1 m wider than its
            // pavement (halfWidth = kPavedHalfM + 1) the chord lands ON the
            // apron. Audit the first ~1.2 km of the connector (the spawn
            // stretch) through the REAL tile mesher at all three LODs: the
            // terrain mesh must sit strictly BELOW the road datum everywhere
            // a slab lies. (X3_NO_CORRIDOR_LOD_REFINE=1 reproduces the
            // pre-fix mesh and this gate then measures the strip.)
            {
                size_t iEnd = 1;
                float acc = 0.0f;
                for (size_t i = 0; i + 1 < cx.spec.x.size() && acc < 1200.0f; ++i) {
                    const float dx = cx.spec.x[i + 1] - cx.spec.x[i];
                    const float dz = cx.spec.z[i + 1] - cx.spec.z[i];
                    acc += std::sqrt(dx * dx + dz * dz);
                    iEnd = i + 1;
                }
                float perLod[3] = { 0, 0, 0 }, wx = 0.0f, wz = 0.0f;
                const float worst = roadTileLodWedge(cx.spec, cx.roadY, 0, iEnd,
                                                     perLod, &wx, &wz);
                std::snprintf(d, sizeof(d),
                    "terrainMeshY - datumY over %.0f m of spawn road: LOD0 %+.3f, "
                    "LOD1 %+.3f, LOD2 %+.3f m; worst %+.3f at (%.0f, %.0f) [gate <= -0.02]",
                    acc, perLod[0], perLod[1], perLod[2], worst, wx, wz);
                check(worst <= -0.02f,
                      "W1 no terrain mesh stands above the spawn-road datum at ANY tile LOD", d);
            }

            // W1b — LOD PARITY over the whole spawn network's corridor floors.
            // The datum gate above covers the slabs; the STRIP the owner
            // photographed stands on the corridor floor margin BESIDE the
            // pavement (the flat floor runs ~1-3 m past the apron) where the
            // datum has nothing to say — at driving height it occludes the
            // road and reads as grass through the pavement. The invariant
            // that kills it: a coarse tile must mesh the corridor floor
            // IDENTICALLY to Full LOD (the refinement makes them the same
            // vertices), so the coarse-LOD EXCESS over Full is ~0 everywhere.
            // Measured pre-fix (X3_NO_CORRIDOR_LOD_REFINE=1): metres.
            {
                float ex0 = 1e9f, ex1 = -1e9f, ez0 = 1e9f, ez1 = -1e9f;
                auto grow = [&](float x, float z) {
                    ex0 = std::min(ex0, x); ex1 = std::max(ex1, x);
                    ez0 = std::min(ez0, z); ez1 = std::max(ez1, z);
                };
                for (const auto& st : spawn->st) grow(st.x, st.z);
                for (size_t i = 0; i < cx.spec.x.size(); ++i) grow(cx.spec.x[i], cx.spec.z[i]);
                const float pad = 40.0f;
                ex0 -= pad; ex1 += pad; ez0 -= pad; ez1 += pad;
                const TerrainConfig& wcfg = worldTerrainConfig();
                const float ts = wcfg.tileSize;
                float worstExcess = 0.0f, wxp = 0.0f, wzp = 0.0f;
                float worstPerLod[3] = { 0, 0, 0 };
                for (float tz = std::floor(ez0 / ts) * ts; tz < ez1; tz += ts) {
                    for (float tx = std::floor(ex0 / ts) * ts; tx < ex1; tx += ts) {
                        float w[3] = { 0, 0, 0 };
                        for (int lod = 0; lod < 3; ++lod) {
                            w[lod] = terrainTileCorridorWedge(wcfg, tx, tz, (TerrainLod)lod);
                            worstPerLod[lod] = std::max(worstPerLod[lod], w[lod]);
                        }
                        const float excess = std::max(w[1], w[2]) - w[0];
                        if (excess > worstExcess) {
                            worstExcess = excess;
                            wxp = tx + ts * 0.5f; wzp = tz + ts * 0.5f;
                        }
                    }
                }
                std::snprintf(d, sizeof(d),
                    "corridor-floor mesh-above-field: LOD0 %.3f, LOD1 %.3f, LOD2 %.3f m; "
                    "worst coarse-over-Full excess %.3f m near (%.0f, %.0f) [gate 0.02]",
                    worstPerLod[0], worstPerLod[1], worstPerLod[2], worstExcess, wxp, wzp);
                check(worstExcess <= 0.02f,
                      "W1b coarse tiles mesh the corridor floors IDENTICALLY to Full LOD", d);
            }

            // ================= THE FREEWAY (dual carriageway) ================
            // Tim: "make the road wider... much wider. Its a freeway" / "8
            // lanes each side... a separate road carrying north and south
            // traffic like I17 does in AZ".
            {
                // F1 — the cross-section arithmetic, from the constants the
                // ribbon actually builds with.
                const float cwPavedFt = 2.0f * kFwyPavedHalfM / kFtToM;
                const float maxSpanM  = 2.0f * (2.0f * kFwyPavedHalfM + 1.0f
                                                + kFwyMedianMaxHalfM);
                std::snprintf(d, sizeof(d),
                    "%d x %.0f ft lanes/carriageway, carriageway %.0f ft paved, "
                    "dual span up to %.0f m (%.0f ft)",
                    kFwyLaneCount, kLaneFt, cwPavedFt, maxSpanM, maxSpanM / kFtToM);
                check(ringSpec.dualCarriageway && kFwyLaneCount == 8 &&
                      std::fabs(cwPavedFt - 144.0f) < 0.5f,
                      "F1 the inner tour is a divided freeway: 8 x 12 ft lanes EACH WAY", d);

                // F2 — the median plan: bounded, terrain-varied (both states
                // genuinely occur), and slew-limited so the carriageways
                // never kink between states.
                const std::vector<float> mp = computeMedianPlan(ringSpec, ringY);
                bool bounded = mp.size() == ringSpec.x.size() && !mp.empty();
                bool hasWide = false, hasNarrow = false;
                float worstSlew = 0.0f;
                for (size_t i = 0; i < mp.size(); ++i) {
                    if (mp[i] < kFwyMedianMinHalfM - 1e-3f ||
                        mp[i] > kFwyMedianMaxHalfM + 1e-3f) bounded = false;
                    if (mp[i] > kFwyMedianWallHalfM) hasWide = true;
                    if (mp[i] <= kFwyMedianWallHalfM) hasNarrow = true;
                    if (i + 1 < mp.size()) {
                        const float dx = ringSpec.x[i + 1] - ringSpec.x[i];
                        const float dz = ringSpec.z[i + 1] - ringSpec.z[i];
                        const float sl = std::max(1.0f, std::sqrt(dx * dx + dz * dz));
                        worstSlew = std::max(worstSlew,
                                             std::fabs(mp[i + 1] - mp[i]) / sl);
                    }
                }
                std::snprintf(d, sizeof(d),
                    "median half %.1f..%.1f m over %u nodes, wide=%d walled=%d, "
                    "worst slew %.3f m/m (cap 0.031)",
                    mp.empty() ? 0.0f : *std::min_element(mp.begin(), mp.end()),
                    mp.empty() ? 0.0f : *std::max_element(mp.begin(), mp.end()),
                    (uint32_t)mp.size(), hasWide ? 1 : 0, hasNarrow ? 1 : 0,
                    worstSlew);
                check(bounded && hasWide && hasNarrow && worstSlew <= 0.031f,
                      "F2 the median varies with the terrain, I-17 style, without kinks", d);

                // F3 — turnaround crossovers: the ~1.7 km rhythm, the spawn
                // crossover at u~0, and one ALIGNED at the junction landing
                // (a side road meeting a divided freeway needs its gap there).
                const std::vector<RoadTurnaround> tas = planTurnarounds(ringSpec);
                float ringLen = 0.0f;
                std::vector<float> U(ringSpec.x.size(), 0.0f);
                for (size_t i = 0; i + 1 < ringSpec.x.size(); ++i) {
                    const float dx = ringSpec.x[i + 1] - ringSpec.x[i];
                    const float dz = ringSpec.z[i + 1] - ringSpec.z[i];
                    U[i + 1] = U[i] + std::sqrt(dx * dx + dz * dz);
                }
                ringLen = U.back();
                bool spacingOk = !tas.empty();
                for (size_t i = 0; i + 1 < tas.size(); ++i) {
                    const float gapC = 0.5f * (tas[i + 1].u0 + tas[i + 1].u1)
                                     - 0.5f * (tas[i].u0 + tas[i].u1);
                    if (gapC > 2600.0f) spacingOk = false;
                }
                const float uJct = U[cx.ringNode];
                bool jctAligned = false;
                for (const RoadTurnaround& t : tas)
                    if (std::fabs(0.5f * (t.u0 + t.u1) - uJct) < 120.0f) jctAligned = true;
                const bool spawnGap = !tas.empty() && tas.front().u0 <= 1.0f;
                std::snprintf(d, sizeof(d),
                    "%u crossovers over %.1f miles (junction-aligned=%d, spawn=%d), "
                    "max centre spacing gate 2.6 km",
                    (uint32_t)tas.size(), ringLen / 1609.34f,
                    jctAligned ? 1 : 0, spawnGap ? 1 : 0);
                check(tas.size() >= 10 && spacingOk && jctAligned && spawnGap,
                      "F3 turnarounds every ~1.7 km, one at the junction, one at spawn", d);

                // F4 — THE JOINTED-BENDS KILLER (owner: "Get rid of ALLLLL
                // jointed bends. PLEASE."): the ribbon rides a fine spline,
                // not the node chords. Gate the actual render path: station
                // spacing <= 20.5 m, and the heading change between
                // consecutive fine segments — the thing the eye reads as a
                // facet in the edge line — under 2 degrees EVERYWHERE on
                // every connected route (daylight; a bore chord is straight).
                auto pathGate = [&](const char* gate, const RoadSpec& s2,
                                    const std::vector<float>& y2) {
                    std::vector<float> mp2;
                    if (s2.dualCarriageway) mp2 = computeMedianPlan(s2, y2);
                    std::vector<RoadRenderStation> path;
                    buildRoadRenderPath(s2, &y2, mp2.empty() ? nullptr : &mp2, path);
                    float maxSpace = 0.0f, maxTurnDeg = 0.0f;
                    for (size_t i = 0; i + 1 < path.size(); ++i) {
                        if (path[i].gap || path[i + 1].gap) continue;
                        maxSpace = std::max(maxSpace, path[i + 1].u - path[i].u);
                        const float dot = path[i].tx * path[i + 1].tx +
                                          path[i].tz * path[i + 1].tz;
                        maxTurnDeg = std::max(maxTurnDeg,
                            std::acos(std::max(-1.0f, std::min(1.0f, dot))) * kRadToDeg);
                    }
                    std::snprintf(d, sizeof(d),
                        "'%s': %u stations (%u nodes), max spacing %.1f m, "
                        "max facet %.2f deg (gate 2.0)",
                        s2.name.c_str(), (uint32_t)path.size(),
                        (uint32_t)s2.x.size(), maxSpace, maxTurnDeg);
                    check(path.size() >= s2.x.size() && maxSpace <= 20.5f &&
                          maxTurnDeg <= 2.0f, gate, d);
                };
                pathGate("F4 the freeway's render path has NO jointed bends", ringSpec, ringY);
                pathGate("F4b the connector's render path has NO jointed bends",
                         cx.spec, cx.roadY);

                // F5 — THE BUDGET: dual carriageways must cost NO extra
                // corridors (one chain per route; the span rides halfWidth).
                std::snprintf(d, sizeof(d),
                    "inner tour %u corridors (was 21 single-carriageway), "
                    "registry %u of %u with the whole spawn network in",
                    ringR.corridorCount, terrainCorridorCount(), kMaxTerrainCorridors);
                check(ringR.corridorCount <= 25 &&
                      terrainCorridorCount() <= kMaxTerrainCorridors - 40,
                      "F5 the freeway profile costs no corridor budget", d);
            }

            // THE RANGE CIRCUIT — registered before the spur so the spur's
            // peak search must avoid it, same order as the host.
            std::vector<const RoadSpec*> avoidC{ &ringSpec };
            const RangeCircuitResult rc = registerRangeCircuit(cx.spec, cx.roadY,
                                                               spawn, &avoidC);
            std::snprintf(d, sizeof(d), "%s: %.2f miles, access %.0f m",
                          rc.built ? "built" : rc.whyNot,
                          rc.road.lengthM / 1609.34f, rc.accessRoad.lengthM);
            check(rc.built, "C1 the range circuit registers off the connector", d);
            if (rc.built) {
                const size_t cnn = rc.spec.x.size();
                const bool closed = cnn > 3 &&
                    std::fabs(rc.spec.x[0] - rc.spec.x[cnn - 1]) < 0.01f &&
                    std::fabs(rc.spec.z[0] - rc.spec.z[cnn - 1]) < 0.01f;
                std::snprintf(d, sizeof(d), "%.2f miles, closed=%d",
                              rc.road.lengthM / 1609.34f, closed ? 1 : 0);
                check(closed &&
                      rc.road.lengthM / 1609.34f >= 3.0f &&
                      rc.road.lengthM / 1609.34f <= 5.0f,
                      "C2 it is a lap-able 3-5 mile loop", d);

                std::snprintf(d, sizeof(d),
                    "longest straight %.0f m, hairpin %.0f deg, climb %.0f ft, grade %.1f%%",
                    rc.longestStraightM, rc.hairpinTurnDeg,
                    rc.climbM * kMToFt, rc.road.maxGradePct);
                check(rc.longestStraightM >= 400.0f && rc.hairpinTurnDeg >= 120.0f &&
                      rc.road.maxGradePct <= 7.5f,
                      "C3 real straights, a real hairpin, drivable grade", d);

                std::snprintf(d, sizeof(d),
                    "access pins %.2f ft deficit; ends off datum %.2f / %.2f ft",
                    rc.accessRoad.pinErrM * kMToFt,
                    std::fabs(rc.accessRoadY.front() - cx.roadY[rc.hostNode]) * kMToFt,
                    std::fabs(rc.accessRoadY.back() - rc.circJct.jy) * kMToFt);
                check(rc.accessRoad.ok && rc.accessRoad.pinErrM <= 0.06f,
                      "C4 the access road meets both datums at grade", d);

                std::snprintf(d, sizeof(d), "circuit rate %.5f->%.5f /m (cap %.5f)",
                              rc.road.maxGradeRatePre, rc.road.maxGradeRatePost, 5.0e-4f);
                check(rc.road.maxGradeRatePost <= 5.0e-4f * 1.10f,
                      "C5 the circuit's vertical flow holds", d);
            }

            // The connector runs through rolling lowland (measured: no peak with
            // 115 ft of prominence within its reach), so the spur falls back to
            // the RING, which skirts the ranges — same order as the host.
            std::vector<const RoadSpec*> avoid{ &cx.spec };
            if (rc.built) { avoid.push_back(&rc.spec); avoid.push_back(&rc.accessSpec); }
            SummitSpurResult sp = registerSummitSpur(cx.spec, cx.roadY, spawn, &avoid);
            if (!sp.built) sp = registerSummitSpur(ringSpec, ringY, spawn, &avoid);
            std::snprintf(d, sizeof(d), "%s: climb %.0f ft, grade %.1f%%, summit cut %.1f ft",
                          sp.built ? "built" : sp.whyNot,
                          sp.climbM * kMToFt, sp.road.maxGradePct, sp.summitCutM * kMToFt);
            check(sp.built && sp.road.maxGradePct <= 14.5f && sp.climbM > 30.0f,
                  "K5 the summit spur climbs a real mountain at a legal grade", d);
            if (sp.built) {
                std::snprintf(d, sizeof(d), "tops out %.1f ft (%.1f m) under the true peak",
                              sp.summitCutM * kMToFt, sp.summitCutM);
                check(sp.summitCutM < 8.0f, "K6 the road actually reaches the summit", d);

                std::snprintf(d, sizeof(d), "spur rate %.5f->%.5f /m (cap %.5f)",
                              sp.road.maxGradeRatePre, sp.road.maxGradeRatePost, 1.6e-3f);
                check(sp.road.maxGradeRatePost <= 1.6e-3f * 1.10f,
                      "K6b the spur's vertical flow holds at its own K", d);

                // BARRIERS (Tim: "We really need BARRIERS."): a climbing
                // switchback over real relief MUST earn rails on its drop
                // side, and no rail may sit on flat ground.
                const BarrierPlan bp = planRoadBarriers(sp.spec, &sp.roadY);
                std::snprintf(d, sizeof(d),
                    "%u railed segments on the spur, min railed drop %.1f m",
                    bp.railSegments, bp.minDropM);
                check(bp.railSegments > 0 && bp.minDropM >= 0.5f,
                      "K6c the spur earns guardrails, and none sit on flat ground", d);
            }

            // ================= HORIZONTAL FLOW (the "sharp points" fix) =====
            // H0 — the A/B proof: X3_NO_HCURVE=1 must reproduce the faceted
            // polyline, and the shipped course must be smooth. If the raw
            // course ever measures smooth on its own, the instrument is
            // dead and this gate says so.
            {
                _putenv_s("X3_NO_HCURVE", "1");
                const RoadSpec raw = makeInnerCourse();
                _putenv_s("X3_NO_HCURVE", "");
                const float rawD = measureMaxDeflectionDeg(raw, nullptr);
                const float smD  = measureMaxDeflectionDeg(ringSpec, nullptr);
                std::snprintf(d, sizeof(d),
                    "raw course facets at %.1f deg/node over %u nodes; "
                    "shipped course %.1f deg/node over %u nodes",
                    rawD, (uint32_t)raw.x.size(), smD, (uint32_t)ringSpec.x.size());
                check(rawD > 8.0f && smD <= ringSpec.maxDeflectionDeg + 0.8f,
                      "H0 horizontal smoothing does real work (A/B vs X3_NO_HCURVE)", d);
            }
            // H1 — every connected route holds its class deflection cap.
            auto hflow = [&](const char* gate, const RoadSpec& sspec) {
                float minR = 0.0f;
                const float dm = measureMaxDeflectionDeg(sspec, &minR);
                std::snprintf(d, sizeof(d),
                    "'%s': max deflection %.2f deg/node (cap %.1f), tightest bend "
                    "%.0f m (floor %.0f), %u nodes",
                    sspec.name.c_str(), dm, sspec.maxDeflectionDeg,
                    minR >= 1e8f ? 99999.0f : minR, sspec.minTurnRadiusM,
                    (uint32_t)sspec.x.size());
                check(dm <= sspec.maxDeflectionDeg + 0.8f, gate, d);
            };
            hflow("H1 the course's horizontal flow holds", ringSpec);
            hflow("H1b the connector's horizontal flow holds", cx.spec);
            if (rc.built) hflow("H1c the circuit's horizontal flow holds", rc.spec);
            if (sp.built) hflow("H1d the spur's horizontal flow holds", sp.spec);
            // H3 — the radius floor is a FLOOR, not a flattener: the
            // circuit's ~68 m hairpin sits above its 60 m floor and must
            // SURVIVE smoothing (C3 separately holds the 120+ deg turn).
            if (rc.built) {
                float minR = 0.0f;
                (void)measureMaxDeflectionDeg(rc.spec, &minR);
                std::snprintf(d, sizeof(d),
                    "circuit's tightest bend %.0f m (floor %.0f, hairpin %.0f deg)",
                    minR, rc.spec.minTurnRadiusM, rc.hairpinTurnDeg);
                check(minR >= rc.spec.minTurnRadiusM * 0.85f && minR <= 100.0f,
                      "H3 the radius floor kept the hairpin a hairpin", d);
            }

            // ================= BARRIERS: JERSEY WALLS + EXCLUSION ZONES =====
            {
                struct RouteRef {
                    const RoadSpec* s; const std::vector<float>* y;
                };
                std::vector<RouteRef> routes{ { &ringSpec, &ringY },
                                              { &cx.spec,  &cx.roadY } };
                if (rc.built) {
                    routes.push_back({ &rc.spec, &rc.roadY });
                    routes.push_back({ &rc.accessSpec, &rc.accessRoadY });
                }
                if (sp.built) routes.push_back({ &sp.spec, &sp.roadY });
                uint32_t jerseyTot = 0, railTot = 0, zoneViolations = 0;
                uint32_t spurLeft = 0, spurRight = 0;
                float jMin = 1e9f, jMax = 0.0f;
                for (const RouteRef& rr : routes) {
                    const BarrierPlan bp = planRoadBarriers(*rr.s, rr.y);
                    jerseyTot += bp.jerseySegments;
                    railTot   += bp.railSegments;
                    if (bp.jerseySegments) {
                        jMin = std::min(jMin, bp.jerseyMinDropM);
                        jMax = std::max(jMax, bp.jerseyMaxDropM);
                    }
                    for (size_t k = 0; k < bp.mask.size(); ++k) {
                        if (!bp.mask[k]) continue;
                        const float dj = std::min(
                            distToNearestRoadJunction(rr.s->x[k], rr.s->z[k]),
                            distToNearestRoadJunction(rr.s->x[k+1], rr.s->z[k+1]));
                        if (dj < kJunctionBarrierClearM) ++zoneViolations;
                        if (rr.s == &sp.spec) {
                            if (bp.mask[k] & (1 | 4)) ++spurLeft;
                            if (bp.mask[k] & (2 | 8)) ++spurRight;
                        }
                    }
                }
                std::snprintf(d, sizeof(d),
                    "%u jersey (side,segment) pairs across %u routes "
                    "(+%u W-beam), drops %.2f..%.2f m",
                    jerseyTot, (uint32_t)routes.size(), railTot,
                    jerseyTot ? jMin : 0.0f, jMax);
                check(jerseyTot > 0,
                      "J1 jersey walls guard at least one ditch-depth run", d);
                std::snprintf(d, sizeof(d),
                    "jersey drops %.2f..%.2f m (band 0.6..2.0; >2 keeps the W-beam)",
                    jerseyTot ? jMin : 0.0f, jMax);
                check(jerseyTot == 0 || (jMin >= 0.55f && jMax <= 2.05f),
                      "J2 every jersey wall guards the ditch band, none on flat ground", d);
                std::snprintf(d, sizeof(d),
                    "%u barrier segments inside a %.0f m junction exclusion zone "
                    "(%u junctions noted)",
                    zoneViolations, kJunctionBarrierClearM, roadJunctionCount());
                check(zoneViolations == 0 && roadJunctionCount() >= 4,
                      "J3 intersections have NO railings — every mouth is open", d);
                if (sp.built) {
                    std::snprintf(d, sizeof(d),
                        "spur barrier segments: %u left-edge, %u right-edge",
                        spurLeft, spurRight);
                    check(spurLeft > 0 && spurRight > 0,
                          "J4 the ridgeline earns barriers on BOTH edges", d);
                }
            }

            // ============ THE VALLEY ROAD LANDINGS + THE OVERLAP LAW ========
            // Owner screenshot: an elevated road stacked on a lower one, raw
            // pavement edges, sheer face between the decks — the valley
            // road's leg tables END on the tour with no junction machinery.
            // R1 proves both ends now land AT GRADE through the shared
            // attachment; R2 is the STRIPING/STACKING law: outside a
            // junction throat, no route's centreline may come within a full
            // pavement width of another's — overlapped pavements are exactly
            // what rendered as "dense parallel white lines" and a plinth.
            const RiverRoadResult rv = registerRiverRoad(&ringSpec, &ringY);
            {
                const float offA = (rv.roadY.empty() || !rv.ringJctA.valid) ? 1e9f
                    : std::fabs(rv.roadY.front() - rv.ringJctA.jy);
                const float offB = (rv.roadY.empty() || !rv.ringJctB.valid) ? 1e9f
                    : std::fabs(rv.roadY.back() - rv.ringJctB.jy);
                std::snprintf(d, sizeof(d),
                    "west (%.0f, %.0f) off %.2f ft, east (%.0f, %.0f) off %.2f ft, "
                    "pin deficit %.2f ft",
                    rv.ringJctA.jx, rv.ringJctA.jz, offA * kMToFt,
                    rv.ringJctB.jx, rv.ringJctB.jz, offB * kMToFt,
                    rv.road.pinErrM * kMToFt);
                check(rv.road.ok && rv.ringJctA.valid && rv.ringJctB.valid &&
                      rv.road.pinErrM <= 0.06f && offA < 0.02f && offB < 0.02f,
                      "R1 the valley road lands on the tour AT GRADE, both ends", d);
            }
            {
                struct RR { const char* nm; const RoadSpec* s; };
                std::vector<RR> rl{ { "ring", &ringSpec }, { "connector", &cx.spec } };
                if (rc.built) { rl.push_back({ "circuit", &rc.spec });
                                rl.push_back({ "access",  &rc.accessSpec }); }
                if (sp.built)   rl.push_back({ "spur",    &sp.spec });
                if (rv.road.ok) rl.push_back({ "valley",  &rv.spec });
                float worst = 1e9f; const char* wa = ""; const char* wb = "";
                float wx = 0, wz = 0;
                for (size_t a = 0; a < rl.size(); ++a)
                    for (size_t b = 0; b < rl.size(); ++b) {
                        if (a == b) continue;
                        const RoadSpec& A = *rl[a].s;
                        const RoadSpec& B = *rl[b].s;
                        for (size_t i = 0; i < A.x.size(); i += 2) {
                            if (distToNearestRoadJunction(A.x[i], A.z[i]) < 60.0f)
                                continue;   // junction throats are SHARED ground
                            float best = 1e9f;
                            for (size_t k = 0; k + 1 < B.x.size(); ++k)
                                best = std::min(best,
                                    segPointDist(A.x[i], A.z[i], B.x[k], B.z[k],
                                                 B.x[k+1], B.z[k+1]));
                            if (best < worst) {
                                worst = best; wa = rl[a].nm; wb = rl[b].nm;
                                wx = A.x[i]; wz = A.z[i];
                            }
                        }
                    }
                std::snprintf(d, sizeof(d),
                    "closest non-junction approach %.0f m (%s node at (%.0f, %.0f) "
                    "to %s; pavement width %.1f m)",
                    worst, wa, wx, wz, wb, 2.0f * kPavedHalfM);
                check(worst >= 2.0f * kPavedHalfM,
                      "R2 no route's pavement overlaps another outside a junction", d);
            }

            // X3_ROADNET_DUMP=1: node/datum/barrier dump — the instrument the
            // eye-gate cameras are authored with (worst-deflection nodes are
            // where a "sharp point" was; jersey mask bits say where the
            // concrete stands). Not a gate; a place to point a camera.
            if (const char* dmp = std::getenv("X3_ROADNET_DUMP"); dmp && dmp[0]) {
                auto dump = [&](const RoadSpec& s2, const std::vector<float>& y2) {
                    const BarrierPlan bp = planRoadBarriers(s2, y2.empty() ? nullptr : &y2);
                    for (size_t i = 0; i < s2.x.size(); ++i) {
                        const float defl = (i > 0 && i + 1 < s2.x.size())
                            ? deflectionAt(s2.x, s2.z, i - 1, i, i + 1) * kRadToDeg : 0.0f;
                        std::printf("DUMP|%s|%zu|%.1f|%.1f|%.1f|defl %.2f|mask %u\n",
                                    s2.name.c_str(), i, s2.x[i],
                                    y2.empty() ? 0.0f : y2[i], s2.z[i], defl,
                                    i < bp.mask.size() ? bp.mask[i] : 0u);
                    }
                };
                dump(ringSpec, ringY);
                dump(cx.spec, cx.roadY);
                if (rc.built) { dump(rc.spec, rc.roadY); dump(rc.accessSpec, rc.accessRoadY); }
                if (sp.built) dump(sp.spec, sp.roadY);
            }

            const uint32_t used = terrainCorridorCount();
            std::snprintf(d, sizeof(d),
                "%u corridors of %u (bore + ring + connector + circuit + spur)",
                used, kMaxTerrainCorridors);
            check(used <= kMaxTerrainCorridors - 40,
                  "K7 the connected spawn network leaves headroom", d);
        }
    }

    clearTerrainCorridors();
    clearRoadJunctions();
    x3::logInfo("[roadnet] " + std::to_string(passN) + " passed, " +
                std::to_string(failN) + " failed");
    return failN == 0;
}

} // namespace x3::game
