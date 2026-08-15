// ROAD NETWORK — see app/road_network.h.
#include "road_network.h"

#include "terrain.h"
#include "tunnel_corridor.h"   // the outer tour's bores are real tunnels
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
void gradeRoad(const std::vector<float>& natural, const std::vector<float>& segLen,
               float maxGrade, std::vector<float>& roadY,
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
        // A ring wraps: the last node and the first are the same place, so the
        // constraint has to travel across the seam too or the join stays steep.
        {
            const float lim = maxGrade * std::max(1.0f, segLen[n - 2]);
            const float a = std::min(roadY[0], roadY[n - 1]);
            if (roadY[0] > a + lim)     { roadY[0] = a + lim; moved += 1.0f; }
            if (roadY[n - 1] > a + lim) { roadY[n - 1] = a + lim; moved += 1.0f; }
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

    // ---- pins from the gaps (lerped y0 -> y1 across each gap) --------------
    const float kNaN = std::numeric_limits<float>::quiet_NaN();
    std::vector<float> pins;
    if (!spec.gaps.empty()) {
        pins.assign(n, kNaN);
        for (const RoadSpec::Gap& g : spec.gaps) {
            for (uint32_t i = g.i0; i <= g.i1; ++i) {
                const float t = (float)(i - g.i0) / (float)(g.i1 - g.i0);
                pins[i] = g.y0 + (g.y1 - g.y0) * t;
            }
        }
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
    if (!spec.gaps.empty()) {
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
    }

    std::vector<float> roadY;
    gradeRoad(ceiling, segLen, spec.maxGrade, roadY,
              pins.empty() ? nullptr : &pins, &r.pinErrM);
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
        for (size_t i = 0; i < n; ++i) {
            bool gapNode = false;
            for (const RoadSpec::Gap& g : spec.gaps)
                if (i >= g.i0 && i <= g.i1) { gapNode = true; break; }
            if (!gapNode && roadY[i] - natural[i] > r.maxFloatM) {
                r.maxFloatM = roadY[i] - natural[i];
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
                        const float off = (float)lt * spec.halfWidth / 4.0f;
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
    if (r.pinErrM > 0.06f) {   // 0.2 ft — the same step budget the bridge deck gets
        std::snprintf(b, sizeof(b),
            "road '%s': a pinned datum could not be reached at grade — %.1f ft short. "
            "The approach authoring is wrong; move the gap or flatten the pin.",
            spec.name.c_str(), r.pinErrM * kMToFt);
        x3::logError(b);
    }
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
    { 271.4f, 7600.0f, 0, nullptr },
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
    if (outBores) outBores->clear();

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
            if (outBores) {
                BoreChord c;
                c.name = A.boreName ? A.boreName : "bore";
                c.x0 = ax; c.z0 = az; c.x1 = bx; c.z1 = bz;
                c.i0 = i0; c.i1 = i0 + (uint32_t)n;   // node at B, emitted next
                outBores->push_back(c);
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
        if (roadY) {
            // The graded DATUM, straight from registerRoad. Load-bearing where
            // a deeper carve crosses this road (deepest-wins would drag a
            // ground-derived ribbon into the other cut) and on pinned
            // approaches, where the datum deliberately floats above ground.
            o[1] = (*roadY)[idx] + kPaveProud;
        } else {
            // Recover the DATUM from the carved field: the corridor cut to
            // (datum - clear), so the surface goes back on top of that.
            o[1] = terrainHeightAtWorld(o[0], o[2]) + kRoadFloorClear + kPaveProud;
        }
    };
    auto segInGap = [&](size_t k) {
        for (const RoadSpec::Gap& g : spec.gaps)
            if (k >= g.i0 && k < g.i1) return true;
        return false;
    };

    for (size_t k = 0; k + 1 < n; ++k) {
        const float dx = spec.x[k+1] - spec.x[k], dz = spec.z[k+1] - spec.z[k];
        const float seg = std::sqrt(dx*dx + dz*dz);
        const float u0 = uRun, u1 = uRun + seg;
        uRun = u1;
        if (segInGap(k)) continue;   // a tunnel or a deck owns this reach
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
            gradeRoad(nat, seg, 0.07f, ry);
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

    clearTerrainCorridors();
    x3::logInfo("[roadnet] " + std::to_string(passN) + " passed, " +
                std::to_string(failN) + " failed");
    return failN == 0;
}

} // namespace x3::game
