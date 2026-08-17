#pragma once
// ===========================================================================
// THE SUMMIT RIDGE ROAD — the dirt road along the tops, and the long leg of
// Tim's loop.
//
// WHAT TIM ASKED FOR, verbatim, after an earlier pass of this lane measured the
// gap from the tunnel's mountain to the summit parking lot, called it "7.1 km"
// and filed it as too far to be a road:
//
//   "7KM.. use miles.. its not far.. and you can have a road that is dirt, on
//    top of the mountains, that is that long, that is what I planned.. curving
//    over mountain features"
//
// He is right and the earlier reading was wrong. 7.1 km is 4.4 MILES. The inner
// tour is 31 miles; the outer is 30.75; the summit spur alone is 1.44. In the
// units this world is actually built in — road_network works in feet and miles
// throughout — 4.4 miles is a medium road, not a distance that needs designing
// around. Everything in this file is stated in miles and feet for that reason.
//
// WHAT MAKES IT A RIDGE ROAD AND NOT A LINE ON A MAP. "On top of the mountains
// .. curving over mountain features" is a routing instruction, and it is the
// whole design:
//
//   * A straight line between the two ends would cross every saddle between
//     them, and registerRoad would answer that by trenching each one — a 4-mile
//     gash through the range with the road at the bottom of it. The exact
//     opposite of a road on top of the mountains.
//   * So the router SEEKS HIGH GROUND. At each step it looks at a fan of
//     headings within reach of the bearing to the goal and takes the one whose
//     ground stands highest, trading real distance for elevation at a fixed
//     rate (kRrSeek). Following a ridge is what makes the curves: the road
//     bends because the mountain bends, not because a spline said so.
//   * That is also what keeps the grades and the cut small. A ridge line is
//     already a smooth-ish path through a range; you do not have to cut one.
//     --test-ridgeroad's G4 is exactly this claim, A/B'd against the straight
//     line it refuses to be.
//
// IT IS DIRT. Narrow, unpaved, no lane markings — a mountain track. That rides
// on two new RoadSpec fields (surfaceSet, widthScale) rather than a second
// hand-written ribbon, so it inherits the road prism, the batter, the D5b
// apron-skirt fix and the barrier drop-test that buildRoadRibbon already has.
//
// WHAT THIS DOES NOT DO, SAID PLAINLY. It runs LOT -> the massif the demo bore
// passes under. The two ends of Tim's full loop that remain are the short ones:
// the tunnel garage up to this road's massif end, and the lot down into the
// second bore. Neither is built. This is the long leg, not the loop.
// ===========================================================================

#include "scene.h"
#include "road_network.h"
#include "summit_lot.h"
#include "tunnel_corridor.h"

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"

namespace x3::game {

// --- The track, in feet, converted once (road_network's unit) --------------
// buildRoadRibbon's base profile is a freeway: 48 ft running + 4 ft shoulders
// + 20 ft aprons = 96 ft of pavement. A dirt road over a ridge wants roughly a
// quarter of that — two vehicles can pass, and nothing wider would have been
// cut up here by hand.
//   running   48.0 * 0.30 = 14.4 ft   (two narrow tracks)
//   shoulder   4.0 * 0.30 =  1.2 ft
//   apron     20.0 * 0.30 =  6.0 ft   (a passing bay's worth of verge)
//   total     96.0 * 0.30 = 28.8 ft of formation
constexpr float kRrWidthScale = 0.30f;
constexpr float kRrFormationFt = 96.0f * kRrWidthScale;   // 28.8 ft

// The carve must cover the formation with margin, same rule every RoadSpec
// obeys — an apron edge on uncut ground tilts into the hillside.
constexpr float kRrCarveMargin = 2.0f;                    // m

// Grades. The summit spur runs the 14% switchback cap because it is climbing a
// mountain face; a road that FOLLOWS the tops should rarely need it, and if it
// does the route is wrong. Kept at the spur's cap so the router is never the
// thing that fails, and G3 measures what it actually used.
constexpr float kRrMaxGrade = 0.14f;

// THE CUT CEILING — how deep this road is allowed to dig, anywhere.
//
// --test-ridgeroad G5 originally asked for something else: that the ridge road
// cut LESS than the straight line between its own two ends. That gate was
// wrong, and it took building the road to see why. The lot stands at 350 ft and
// the bore's portal at ~55 ft, with open ground between; the straight line is
// therefore the LOW road, running downhill the whole way, and a low road is
// cheap to build. Measuring a hill road against it punishes the route for
// climbing at all — it would have been satisfied only by giving up on the tops.
//
// The two honest questions are separate, so they are separate gates now:
//   G4  does it actually run higher ground than the straight line? (the tops)
//   G5  does it SIT on the landscape instead of trenching through it? (this)
// 40 ft is a dirt mountain road's cut: a shelf bench and a bit of saddle
// levelling. Anything deeper is a road that bulldozed the feature it was
// supposed to curve over.
constexpr float kRrMaxCutFt = 40.0f;

// WHAT THE ROUTER MAXIMISES — and two wrong answers before the right one.
//
// ATTEMPT 1: absolute height, one step of lookahead (kRrSeek 3). The road fell
// off the lot's knoll into the lowland and then could not climb out; 744 ft of
// cut.
// ATTEMPT 2: absolute height, four steps of lookahead (kRrSeek 16). Now it DID
// find high ground — G4's mean rose from +20 ft to +59 ft over the straight
// line — and the cut got WORSE, 774 ft. That failure is the useful one: a
// router that maximises altitude drives at SUMMITS, and a drivable road cannot
// go over a summit, so registerRoad's grader answers by cutting straight
// through it. Seeking the highest ground and running on top of the mountains
// are not the same instruction. Real mountain roads contour: they hold a ridge,
// cross its saddles, and bend AROUND the knolls — "curving over mountain
// features", which is what Tim asked for and what attempt 2 was not doing.
//
// ATTEMPT 3, below. Two terms, and neither of them is altitude:
//
//   RIDGE-NESS — how far the ground stands above its own neighbours to either
//   side, measured across the direction of travel. A spine scores high, a
//   valley floor scores negative, and a flat plateau scores ~0 no matter how
//   high it is. This is the term that puts the road on the tops.
//
//   GRADE COST — anything the road would have to climb or drop faster than it
//   is allowed to, per step. A summit is expensive by construction, so the
//   route goes round it; a saddle is cheap, so the route crosses there. This is
//   the term that stops the cut.
//
// Both are probed several steps ahead (kRrLookahead), because one 110 m step
// cannot tell a ridge from a knoll.
constexpr float kRrRidgeW  = 20.0f;   // score per metre of ridge-ness
constexpr float kRrGradeW  = 34.0f;   // penalty per metre of un-drivable climb
constexpr float kRrProbeM  = 95.0f;   // cross-track offset the ridge test samples
//
//   CROSS-SLOPE — the term that was actually missing, and the one that made
//   the cut collapse. Attempt 4 tried to stop the digging by forbidding the
//   climb: penalise any position the road could not descend from to the goal
//   inside its grade cap. Tim, watching that go in:
//
//     "well we WANT the road to CLIMB up the mountain, as mountain roads do"
//     "up around the back or side"  "through wild territory"
//
//   He is right, and the penalty was solving the wrong problem. Cut does not
//   come from CLIMBING, it comes from climbing in the wrong PLACE. A road
//   benched across a gentle flank needs a shelf a few feet deep; the same climb
//   driven at a cliff or over a summit needs hundreds. So the router now reads
//   the CROSS-SLOPE — how steeply the ground falls away to either side of the
//   direction of travel — and prefers ground it can cut a shelf into. That is
//   what sends it "up around the back or side" instead of straight over the
//   top: the flank is benchable, the summit is not, and no rule about altitude
//   is needed to express it.
//
//   The reachability term stays, but soft and measured against the ROPE the
//   router still has (kRrMaxSteps), not against the straight line to the goal.
//   A mountain road descends by wrapping around the mountain, so the straight
//   line was never the distance it had to do it in.
constexpr float kRrCrossW    = 900.0f;    // penalty per unit of cross-slope
constexpr float kRrReachW    = 6000.0f;   // soft: climb you have no rope left to shed
constexpr float kRrReachFrac = 0.85f;     // of kRrMaxGrade, against remaining rope
// Elevation change a step may make for free: the road's own grade cap, less a
// margin so the grader is never asked for exactly its limit.
constexpr float kRrFreeClimbFrac = 0.10f;

constexpr int   kRrLookahead  = 4;        // probe steps per candidate heading

constexpr float kRrStepM      = 110.0f;   // router step (pre-smoothing node gap)
constexpr float kRrFanDeg     = 70.0f;    // heading fan each side of the bearing
constexpr int   kRrFanCount   = 17;       // samples across the fan
constexpr int   kRrMaxSteps   = 200;      // rope for a ridge that wanders
constexpr float kRrArriveM    = 220.0f;   // inside this, run straight to the end
// Stay off everything already built. A dirt road crossing the tour's centreline
// would carve a notch through it (deepest-wins), and inside the bore's spine it
// would drop the ground under the backfill lid.
constexpr float kRrClearRouteM = 120.0f;
constexpr float kRrClearSpineM = 150.0f;

// THE TUNING LOG. Six configurations, measured, because the next person to
// touch the router should not have to rediscover the shape of the trade-off.
// (mean ground above the straight line / deepest cut; ceiling is 40 ft)
//
//   1  altitude, 1-step lookahead, seek 3        +20 ft / 744 ft
//   2  altitude, 4-step lookahead, seek 16       +59 ft / 774 ft
//   3  ridge-ness + grade cost (ridge 26)        +11 ft /  52 ft
//   4  3 + hard reachability (ridge 48)          +14 ft / 306 ft
//   5  4 + cross-slope, soft reach (ridge 20)    +13 ft /  52 ft   <- SHIPPED
//   6  5 with ridge 70, cross 320               +106 ft / 774 ft
//
// The road is BIMODAL and nothing tried so far lands between the modes: it
// either stays low and clean, or climbs the massif and gets 700+ ft of cut.
// SHIPPED is 5 — a real, drivable, buildable dirt road — and G4 is left RED
// rather than relaxed, because red is the truth: Tim asked for a road that
// climbs the mountain "up around the back or side, through wild territory",
// and this one hugs lower ground than that.
//
// AND THEN I LOOKED, WHICH I SHOULD HAVE DONE FIRST. That 774 ft was
// suspiciously INVARIANT across configurations whose supposed causes were
// moving, which is the signature of tuning against the wrong thing. So the
// receipt below now prints WHERE the deepest cut is, and the answer for the
// shipped route is:
//
//     deepest cut is at mile 0.14 of 4.83, at (354, 6536)
//
// Mile 0.14. It is not the massif at the far end, which is what five rounds of
// weight-tuning were aimed at. It is the first 750 ft of road, coming DOWN OFF
// THE SUMMIT LOT'S OWN KNOLL: the pad sits at 350 ft on a small steep hill, the
// road may only shed height at 14%, and so it benches into the hillside right
// under the lot. The router was never the problem at this end.
//
// WHAT TO DO NEXT, in this order:
//   1. Fix the lot end. The descent off the knoll wants SWITCHBACKS — the
//      sawtooth legs registerSummitSpur (road_network.cpp:1934) already builds,
//      which climb 247 ft in 1.44 miles at this same 14% cap. The spur climbs
//      that hill; the ridge road should leave the same way it came up. That
//      alone should put G5 under its 40 ft ceiling.
//   2. THEN re-tune for G4, and check the cut's location every time, not just
//      its depth. Configurations 2 and 6 got the road onto the tops (+59 ft,
//      +106 ft) and their cuts may well also be at an end rather than spread
//      along the route — in which case the same switchback fix unlocks them and
//      the bimodality was never real.
struct RidgeRoadResult {
    bool               built  = false;
    const char*        whyNot = "";
    RoadBuildResult    road;
    RoadSpec           spec;
    std::vector<float> roadY;
    float lengthMi     = 0.0f;   // miles, because that is the unit this is in
    float maxGradePct  = 0.0f;
    float maxCutFt     = 0.0f;
    float meanElevFt   = 0.0f;   // mean NATURAL ground along the chosen line
    float straightElevFt = 0.0f; // ... and along the straight line it refused
    float straightCutFt  = 0.0f; // ... and what that line would have had to cut
    float maxCutAtMi = 0.0f;                // WHERE the deepest cut is (miles along)
    float maxCutX = 0.0f, maxCutZ = 0.0f;   // ... and where in the world
    float endX = 0.0f, endZ = 0.0f;         // the portal-shoulder end (garage tie-in)
    float massifX = 0.0f, massifZ = 0.0f;   // the summit the bore passes under (reported)
};

// Register the ridge road's carve. BOOT ONLY, before the first height query /
// TerrainStreamer::init() (the terrain.h registry contract), and AFTER the lot
// (one end is the lot's rim) and the bore (the other end is its massif, and the
// route must keep clear of its spine).
RidgeRoadResult registerRidgeRoad(const SummitLotResult& lot,
                                  const TunnelRoute& bore,
                                  const std::vector<const RoadSpec*>* avoid);

// Lay the dirt. Collides, like every other ribbon.
void buildRidgeRoad(const RidgeRoadResult& rr, Scene& scene,
                    x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& phys);

// --test-ridgeroad. Registers the real world and asserts the road exists, is
// the length Tim described, is drivable, and — the one that matters — that it
// actually runs the TOPS, A/B'd against the straight line between its own two
// ends.
bool runRidgeRoadSelfTest();

} // namespace x3::game
