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
    // ... plus the spec's own per-node pins (junction datums — see RoadSpec).
    const float kNaN = std::numeric_limits<float>::quiet_NaN();
    std::vector<float> pins;
    if (!spec.gaps.empty() || !spec.pinY.empty()) {
        pins.assign(n, kNaN);
        for (const RoadSpec::Gap& g : spec.gaps) {
            for (uint32_t i = g.i0; i <= g.i1; ++i) {
                const float t = (float)(i - g.i0) / (float)(g.i1 - g.i0);
                pins[i] = g.y0 + (g.y1 - g.y0) * t;
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
    // 88 ft of pavement: 4 x 12 ft lanes plus a 20 ft cement apron each side,
    // wide enough to pull a dead car fully clear of the running surface.
    s.halfWidth = kPavedHalfM + 1.0f;   // +1 m so the apron edge sits on cut ground
    s.falloff   = 18.0f;                // a wider road wants a longer batter
    s.maxGrade  = 0.07f;
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
constexpr float kJctSetbackM = kPavedHalfM + 20.0f;   // 33.4 m

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

    // CENTRELINE: exit -> a point set back square of the ring, as a gentle
    // S-curve (Tim: "they do not curve"). Ends forced straight so the first
    // segment continues the tunnel's heading and the last arrives radially.
    const float setback = kJctSetbackM;
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
    for (int k = 0; k <= nseg; ++k) {
        const float t = (float)k / (float)nseg;
        float lat = amp * std::sin(6.2831853f * t) * std::sin(3.1415926f * t);
        if (k <= 1 || k >= nseg - 1) lat = 0.0f;           // straight ends, square joints
        s.x.push_back(e[0] + (bex - e[0]) * t + pxd * lat);
        s.z.push_back(e[2] + (bez - e[2]) * t + pzd * lat);
    }
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
    {
        float bestScore = -1e18f; bool found = false;
        for (float frac : { 0.42f, 0.58f, 0.30f }) {
            const size_t h = std::max((size_t)4, std::min(n - 5, (size_t)((float)n * frac)));
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

// PURE barrier planning — see road_network.h. The drop test: a segment's side
// earns a rail when the carved ground ~6 m beyond the apron edge sits more
// than 2 m below the road datum at either segment end. Gap segments never
// rail (a bore's walls and a bridge's parapets are their own protection).
BarrierPlan planRoadBarriers(const RoadSpec& spec, const std::vector<float>* roadY) {
    BarrierPlan plan;
    const size_t n = spec.x.size();
    if (n < 2) return plan;
    if (roadY && roadY->size() != n) roadY = nullptr;
    plan.mask.assign(n - 1, 0);
    plan.minDropM = 1e9f;

    auto datumAt = [&](size_t i) {
        if (roadY) return (*roadY)[i];
        return terrainHeightAtWorld(spec.x[i], spec.z[i]) + kRoadFloorClear;
    };
    auto dropAt = [&](size_t i, int side) {
        const size_t ip = (i + 1 < n) ? i + 1 : i;
        const size_t im = (i > 0) ? i - 1 : i;
        float tx = spec.x[ip] - spec.x[im], tz = spec.z[ip] - spec.z[im];
        const float tl = std::sqrt(tx * tx + tz * tz);
        if (tl > 1e-4f) { tx /= tl; tz /= tl; }
        const float lat = (kPavedHalfM + 6.0f) * (float)side;
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
        for (int side = -1; side <= 1; side += 2) {
            const float d0 = dropAt(k, side), d1 = dropAt(k + 1, side);
            const float d = std::max(d0, d1);
            if (d > 2.0f) {
                plan.mask[k] |= (side < 0) ? 1 : 2;
                ++plan.railSegments;
                plan.minDropM = std::min(plan.minDropM, d);
            }
        }
    }
    if (plan.railSegments == 0) plan.minDropM = 0.0f;
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
    const SurfaceSet& asphalt = surf.get(device, "rd_asphalt_01");
    const SurfaceSet& cement  = surf.get(device, "mw_concrete_panels_a");
    if (!asphalt.ok || !cement.ok)
        x3::logWarn("road ribbon: surface set(s) unavailable - flat colour fallback");

    const float run = kRunningHalfM;   // 24 ft: half of the 4-lane running width
    const float sho = kShoulderHalfM;  // 28 ft: + the 4 ft paved shoulder
    const float pav = kPavedHalfM;     // 48 ft: + the 20 ft cement apron

    // THE ROAD PRISM. Tim: "it also needs to be THICK CONCRETE in the base and
    // aprons.. not floating on top!!!" A bare ribbon is a zero-thickness sheet:
    // anywhere the ground falls away from the apron's outer edge (side-slopes,
    // hollows, the pinned portal-ramp floats) the pavement reads as paper. The
    // fix is geometry: from each apron edge, a VERTICAL concrete face at least
    // kSkirtFace deep, then a BATTERED face running out and down until its toe
    // laps kSkirtLap UNDER the carved ground — buried where the road sits in a
    // cutting (harmless, hidden), a real poured base wherever the terrain drops.
    // Never coplanar with the terrain, so nothing z-fights.
    constexpr float kSkirtFace    = 0.62f;   // minimum visible vertical face (m)
    constexpr float kSkirtOut     = 0.9f;    // batter run outward (m)
    constexpr float kSkirtLap     = 0.6f;    // toe depth under the carved ground
    constexpr float kSkirtMaxDrop = 14.0f;   // don't build a curtain wall off a cliff

    RibbonMesh road, shoulders, apronL, apronR, paint, skirt, rails;
    // Guardrail COLLISION is a continuous thin wall per railed run — the car
    // must be able to LEAN on it and be saved; per-post boxes would shred it.
    std::vector<float>    railColV;
    std::vector<uint32_t> railColI;
    const BarrierPlan barriers = [&]() {
        const char* e = std::getenv("X3_ROAD_BARRIERS");   // 0 = A/B capture off
        if (e && e[0] == '0') return BarrierPlan{};
        return planRoadBarriers(spec, roadY);
    }();
    out.railSegments = barriers.railSegments;
    out.railMinDropM = barriers.minDropM;
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

        // THE SHOULDER: 4 ft of asphalt outside the edge line, same surface,
        // its own strip so it can weather a shade differently from the lanes.
        float aLs[3], bLs[3], aRs[3], bRs[3];
        at(k, -sho, aLs); at(k+1, -sho, bLs);
        at(k,  sho, aRs); at(k+1,  sho, bRs);
        shoulders.quad(aLs, aL, bL, bLs, 0.0f, 1.0f, u0 * 0.06f, u1 * 0.06f);
        shoulders.quad(aR, aRs, bRs, bR, 0.0f, 1.0f, u0 * 0.06f, u1 * 0.06f);

        float aLo[3], bLo[3], aRo[3], bRo[3];
        at(k, -pav, aLo); at(k+1, -pav, bLo);
        at(k,  pav, aRo); at(k+1,  pav, bRo);
        apronL.quad(aLo, aLs, bLs, bLo, 0.0f, 1.0f, u0 * 0.06f, u1 * 0.06f);
        apronR.quad(aRs, aRo, bRo, bRs, 0.0f, 1.0f, u0 * 0.06f, u1 * 0.06f);

        // GUARDRAILS (Tim: "We really need BARRIERS.") where the drop test
        // fired: two horizontal W-beam-read bands (0.26-0.36 m and 0.50-0.60 m
        // up) on posts every ~4 m, just inside the apron's outer edge, both
        // faces emitted (a rail is seen from the road AND from the drop).
        // Collision is the continuous 1 m wall accumulated per railed run.
        if (k < barriers.mask.size() && barriers.mask[k]) {
            for (int side = -1; side <= 1; side += 2) {
                const uint8_t bit = (side < 0) ? 1 : 2;
                if (!(barriers.mask[k] & bit)) continue;
                const float lat = (pav - 0.3f) * (float)side;
                float a0[3], b0[3];
                at(k, lat, a0); at(k+1, lat, b0);
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
                // posts on a global 4 m rhythm so runs stay in step across nodes
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
                // the continuous collision wall for this railed segment
                const uint32_t cb = (uint32_t)(railColV.size() / 3);
                const float wall[4][3] = {
                    { a0[0], a0[1],         a0[2] }, { b0[0], b0[1],         b0[2] },
                    { b0[0], b0[1] + 1.0f,  b0[2] }, { a0[0], a0[1] + 1.0f,  a0[2] } };
                for (const auto& w : wall) {
                    railColV.push_back(w[0]); railColV.push_back(w[1]); railColV.push_back(w[2]);
                }
                const uint32_t wi[6] = { cb, cb + 1, cb + 2, cb, cb + 2, cb + 3 };
                railColI.insert(railColI.end(), wi, wi + 6);
            }
        }

        // THE PRISM SKIRT, both sides. The top edge reuses the apron edge
        // verts' positions exactly (same at() call), so pavement and skirt
        // share an edge — no hairline crack for the sky to leak through.
        for (int side = -1; side <= 1; side += 2) {
            const float lat = pav * (float)side;
            float aT[3], bT[3];
            at(k, lat, aT); at(k+1, lat, bT);
            // knee: bottom of the vertical face
            float aK[3] = { aT[0], aT[1] - kSkirtFace, aT[2] };
            float bK[3] = { bT[0], bT[1] - kSkirtFace, bT[2] };
            // toe: out kSkirtOut, down to kSkirtLap under the carved ground
            float aO[3], bO[3];
            at(k,   lat + kSkirtOut * (float)side, aO);
            at(k+1, lat + kSkirtOut * (float)side, bO);
            auto toeY = [&](const float top[3], const float o[3]) {
                const float ground = terrainHeightAtWorld(o[0], o[2]);
                float y = std::min(top[1] - kSkirtFace, ground) - kSkirtLap;
                return std::max(y, top[1] - kSkirtMaxDrop);
            };
            aO[1] = toeY(aT, aO);
            bO[1] = toeY(bT, bO);
            const float faceV0 = 0.0f, faceV1 = kSkirtFace / 6.1f;
            const float batV1  = faceV1 + (kSkirtFace + kSkirtLap) / 6.1f;
            if (side > 0) {
                skirt.quadN(aT, aK, bK, bT, faceV0, faceV1, u0 * 0.06f, u1 * 0.06f);
                skirt.quadN(aK, aO, bO, bK, faceV1, batV1,  u0 * 0.06f, u1 * 0.06f);
            } else {
                skirt.quadN(aK, aT, bT, bK, faceV1, faceV0, u0 * 0.06f, u1 * 0.06f);
                skirt.quadN(aO, aK, bK, bO, batV1,  faceV1, u0 * 0.06f, u1 * 0.06f);
            }
        }

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

    // FALLBACK TINTS THAT STILL READ AS A ROAD. rd_asphalt_01 does not exist in
    // the tree yet, so every ribbon road was hitting the untextured path with a
    // WHITE tint — 46 miles of glowing white pavement (and the junction laps
    // read sky-blue against it). Until the asphalt set lands, the untextured
    // fallback is tinted like the material it stands in for.
    float roadTint [4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    if (!asphalt.ok) { roadTint[0] = 0.055f; roadTint[1] = 0.056f; roadTint[2] = 0.060f; }
    float apronTint[4] = { 0.86f, 0.85f, 0.82f, 1.0f };
    if (!cement.ok)  { apronTint[0] = 0.42f; apronTint[1] = 0.41f; apronTint[2] = 0.38f; }
    const float paintC[4] = { 1.8f, 1.8f, 1.7f, 1.0f };
    upload(road,   &asphalt, roadTint,  true);
    // The shoulder weathers a shade differently from the lanes — that contrast
    // is what tells you at speed you have left the running surface.
    float shoulderTint[4] = { 0.78f, 0.78f, 0.79f, 1.0f };
    if (!asphalt.ok) { shoulderTint[0] = 0.072f; shoulderTint[1] = 0.072f; shoulderTint[2] = 0.076f; }
    upload(shoulders, &asphalt, shoulderTint, true);
    upload(apronL, &cement,  apronTint, true);
    upload(apronR, &cement,  apronTint, true);
    // The prism skirt is CONCRETE STRUCTURE, slightly darker than the apron it
    // holds up so the edge reads as a poured base, not more paving.
    float skirtTint[4] = { 0.74f, 0.73f, 0.70f, 1.0f };
    if (!cement.ok) { skirtTint[0] = 0.34f; skirtTint[1] = 0.33f; skirtTint[2] = 0.31f; }
    upload(skirt,  &cement,  skirtTint, true);
    // Galvanized-steel gray, dielectric — an untextured metallic-1 surface
    // renders BLACK (X3_WORLD_RULES rule 5), so the rail is painted gray with
    // its geometry (two bands + posts) doing the W-beam read.
    const float railTint[4] = { 0.60f, 0.62f, 0.65f, 1.0f };
    upload(rails,  nullptr,  railTint,  false);
    if (!railColI.empty())
        phys.addStaticMesh(railColV.data(), (uint32_t)(railColV.size() / 3),
                           railColI.data(), (uint32_t)railColI.size());
    upload(paint,  nullptr,  paintC,    false);

    out.ok = out.meshCount > 0;
    char b[256];
    std::snprintf(b, sizeof(b),
        "road ribbon: %.2f miles | %u meshes, %u quads | 4 x %.0f ft lanes + %.0f ft "
        "shoulders + %.0f ft aprons = %.0f ft paved | prism skirt %.2f m face | "
        "%u guardrail segments (min drop railed %.1f m)",
        out.lengthM / 1609.34f, out.meshCount, out.quadCount,
        kLaneFt, kShoulderFt, kApronFt, kPavedHalfM * 2.0f / kFtToM, kSkirtFace,
        out.railSegments, out.railMinDropM);
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
    const float twistRun = std::max(2.0f, setb - kPavedHalfM);
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

    RibbonMesh road, wings;
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
    out.lengthM = V;
    out.ok = out.meshCount > 0;
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

            const uint32_t used = terrainCorridorCount();
            std::snprintf(d, sizeof(d),
                "%u corridors of %u (bore + ring + connector + circuit + spur)",
                used, kMaxTerrainCorridors);
            check(used <= kMaxTerrainCorridors - 40,
                  "K7 the connected spawn network leaves headroom", d);
        }
    }

    clearTerrainCorridors();
    x3::logInfo("[roadnet] " + std::to_string(passN) + " passed, " +
                std::to_string(failN) + " failed");
    return failN == 0;
}

} // namespace x3::game
