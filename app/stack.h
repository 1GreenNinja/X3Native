#pragma once
// ---------------------------------------------------------------------------
// THE MEGA STACK — a FOUR-LEVEL directional interchange, I-17/I-10 (Phoenix)
// as the owner's reference: "High concrete barriers swooping curving ramps...
// high speed arcs." He drives the real thing.
//
// WHAT IT IS, level by level:
//   L1  the inner tour (a dual 8+8 freeway) stays AT GRADE.
//   L2  a second freeway-class dual carriageway crosses it square, carried
//       OVER on twin box-girder decks on piers.
//   L3  two DIRECTIONAL flyover ramps over both mainlines.
//   L4  the other two, over the mainlines AND over L3.
// Four levels, four directional movements, one pinwheel. Every elevated edge
// carries a continuous 1.25 m solid parapet with collision on: at 60 mph you
// lean on the wall, you do not fly off it.
//
// THE PINWHEEL, derived once so nobody has to re-derive it. Work in the
// crossing's frame: u along freeway A's tangent, v along freeway B's axis
// (B is square to A). Ramp 0 leaves A running WITH the traffic on a straight
// climbing leg at lateral v = +g (g = A's paved edge + clearance), turns
// through ONE 90-degree constant-radius arc, and lands on B running with ITS
// traffic on a straight leg at u = +g. Tangency forces the arc centre to
// (-Sa, -Sa) with R = Sa + g. Ramps 1/2/3 are the same shape rotated 90/180/
// 270 degrees. Two consequences fall straight out and both are load-bearing:
//   * OPPOSITE ramps never touch. Their centres are 2*Sa*sqrt(2) apart, so
//     the circles miss entirely whenever Sa*(sqrt2 - 1) > g. They can share
//     a level.
//   * ADJACENT ramps always cross, right over the middle of the interchange.
//     They cannot. Hence exactly two ramp levels — which is what makes this
//     a FOUR-level stack and not a wish.
// The arc passes 0.414*Sa - g from the crossing point and cuts clean across
// BOTH mainlines' full paved width on its way through.
//
// COMPOSED, NOT INVENTED (NO_SLOP rule 1). Everything below already worked
// somewhere in this tree before the Stack existed:
//   * app/interchange.{h,cpp} — THE DIAMOND. Its RoadSpec::Gap-over-a-route
//     pattern, its measured site scoring, its INTERPOLATING clearance sampler
//     (the one that caught a 4.3 m quantisation error), its AUTHORED vertical
//     alignment (grade-limited envelope fit + a K-rate-limited tracking walk
//     with dv clamped on the AVERAGE segment length — the per-segment clamp
//     leaked 1.43x at a station transition), its noteInterchangeZone()
//     crossover suppression, and its ramps-land-both-datums-at-grade law.
//   * app/world_hosts/echo_roads.cpp — THE TALL-PIER VOCABULARY. pillar()
//     (footing pad / tapered shaft / hammerhead cap beam) and deckFascia()
//     (side fascia + soffit = a real box section, not a paper ribbon), plus
//     the acceptance discipline that came out of its "pier forest" saga: LOG
//     THE TALLEST PIER EVERY BUILD, and the owner's own note that "CA
//     viaducts run MUCH longer unsupported spans — half the pier count, each
//     pier a touch heavier so the longer span reads structural" (kPierEveryM
//     below is that number, PAIRED with echo_roads.cpp's kPillarEvery = 70).
//   * app/road_network.{h,cpp} — every route, ribbon, render path, median
//     plan, junction throat and barrier plan in the world.
//
// THE ONE NEW MECHANISM: **multi-route gap authoring** (planFlyoverGaps).
// The diamond declares ONE Gap over ONE road at a known level. A stack ramp
// must clear a LIST of things — the freeway below it, the other freeway's
// deck, and (on L4) two ramp decks — none of whose surfaces are the terrain.
// planFlyoverGaps() takes a ramp centreline and a vector of UnderRoute
// surfaces, finds EVERY crossing by measurement, derives the one level datum
// that clears all of them by the 16.5 ft law, authors a grade- and K-limited
// climb/hold/descend profile onto it, and emits the Gap run that carries it.
// It is written against a generic surface list, so an L5 would need no new
// code — only another entry in the vector.
//
// WHY PER-SEGMENT GAPS: RoadSpec::Gap lerps its datum LINEARLY from y0 to y1,
// which is right for a bore chord and wrong for a flyover — a ramp is a
// CURVE in elevation. The gap list is a vector, so the authored profile is
// carried as ONE GAP PER SEGMENT: no carve, no ribbon (the deck owns the
// reach), and the datum is the authored curve node for node.
// buildRoadRenderPath keeps gap reaches linear on the chord, so the ramps are
// authored at 6 m node spacing — on a 283 m radius that is a 1.2 deg facet,
// well under the no-jointed-bends gate.
//
// BOOT CONTRACT: registers corridors and reads the carved field — call after
// every other road (including the diamond, whose zone it must stay out of),
// before the first terrain height query / TerrainStreamer::init().
// ---------------------------------------------------------------------------
#include "road_network.h"
#include "interchange.h"

namespace x3::game {

class Scene;

// The vertical clearance law is the SAME law the diamond holds and for the
// same reason — 16.5 ft is the US interstate minimum — so it is the SAME
// constant (NO_SLOP rule 4: paired values are one value).
constexpr float kStackClearM = kOverpassClearM;      // 5.03 m == 16.5 ft
// Structure depths, top of pavement to soffit. A freeway-class twin deck is
// deeper than a two-lane ramp deck; both are box sections.
constexpr float kStackMainDepthM = 2.00f;
constexpr float kStackRampDepthM = 1.60f;
// Bearing/haunch margin on top of the clearance law, so the LAW is a floor
// the structure never has to argue with (the diamond keeps 0.45 m).
constexpr float kStackBearingM   = 0.20f;
// THE PARAPET. The owner's words are the spec: "High concrete barriers" you
// can lean on at 60 mph. 1.25 m sits in the 1.1-1.4 m band he asked for and
// above the 1.07 m (42 in) US standard for a high-speed bridge rail.
constexpr float kStackParapetH   = 1.25f;
constexpr float kStackParapetW   = 0.42f;
// DECK WIDTHS. Running surface + shoulders + a kerb — the APRONS stop at the
// abutment, exactly as the diamond's deck does and for the same reason: real
// bridges drop them and the wingwalls close the corner. A ramp carries the
// half-scale cross-section (two 12 ft lanes), the mainline carries a full
// 8-lane carriageway. PAIRED with RoadSpec::widthScale 0.5 on the ramps.
constexpr float kStackRampDeckHalfM = kShoulderHalfM * 0.5f + 0.55f;   // ~9.6 m wide
constexpr float kStackMainDeckHalfM = kFwyShoulderHalfM + 0.55f;       // ~32.8 m wide
// Pier rhythm. PAIRED with echo_roads.cpp's kPillarEvery (70 m).
constexpr float kPierEveryM      = 70.0f;
constexpr float kPierMinAirM     = 1.5f;     // below this the abutment carries it
// The high-speed arc band the owner asked for: 150-300 m radius, 55-65 mph.
constexpr float kStackArcMinR    = 150.0f;
constexpr float kStackArcMaxR    = 300.0f;
// The zone. Covers the four ramp terminals and their gore tapers with margin:
// no median crossover, no work zone, in any of it.
constexpr float kStackZoneR      = 1080.0f;
// The STRAIGHTNESS window is not the zone: the outer 180 m of the zone is the
// gore taper, which follows the freeway's own curve and does not care. Sized
// so a site exists on a 16-mile tour whose fillets run 300-900 m.
constexpr float kStackSiteWindowM = 900.0f;

// ---------------------------------------------------------------------------
// THE NEW MECHANISM — one surface a flyover has to clear.
//
// Deliberately NOT "a RoadSpec": the thing under a ramp may be a road at
// grade, a freeway ON A DECK, or another ramp's deck, and only the last of
// those has a spec whose datum IS its driving surface everywhere. What every
// case DOES have is a sampled centreline with a datum and a half-span, which
// is exactly what a clearance measurement needs.
// ---------------------------------------------------------------------------
struct UnderRoute {
    std::string name = "route";
    std::vector<RoadRenderStation> path;   // sampled centreline + datum
    float halfSpanM  = kPavedHalfM;        // half the surface width to clear
    bool  dual       = false;              // half-span varies with the median
    static UnderRoute fromRoute(const char* name, const RoadSpec& spec,
                                const std::vector<float>& roadY, bool dual,
                                float halfSpanM);
    // The driving SURFACE under (x, z), or false if (x, z) is not over it.
    // INTERPOLATED along the winning segment, never snapped to a station —
    // the diamond's 4.3 m quantisation bug, not repeated here.
    bool surfaceAt(float x, float z, float* outY) const;
};

// One measured crossing of a flyover over an UnderRoute.
struct StackCrossing {
    uint32_t under = 0;          // index into the UnderRoute vector
    float    u = 0.0f;           // arc length along the flyover
    float    x = 0.0f, z = 0.0f;
    float    surfaceY = 0.0f;    // the driving surface below, interpolated
    float    clearanceM = 0.0f;  // soffit - surfaceY, once the level is set
};

struct FlyoverPlan {
    bool  ok = false;
    const char* whyNot = "";
    float levelY = 0.0f;             // the level the flyover holds over the pile
    float minClearanceM = 1e9f;      // worst measured clearance
    float maxGradePct = 0.0f;        // authored profile, before registration
    float maxGradeRate = 0.0f;       // authored |d(grade)/ds|, per metre
    float plateauM = 0.0f;           // metres held EXACTLY at levelY
    float pileM = 0.0f;              // metres of flyover over something
    float uLoM = 0.0f, uHiM = 0.0f;  // ...and where that reach starts/ends
    float lengthM = 0.0f;            // the flyover's own arc length
    float crestLoM = 0.0f, crestHiM = 0.0f;   // the reach that needs the TOP level
    float crestDeficitM = 1e9f;      // worst (profile - requirement); < 0 == illegal
    uint32_t gapI0 = 0, gapI1 = 0;   // the elevated node run
    std::vector<StackCrossing> cross;
    std::vector<float>         profile;   // authored datum, one per spec node
};

// THE MECHANISM. `over` is a finished horizontal centreline (already
// smoothed); `under` is everything it must clear, in any order. Fills
// over.gaps (one per elevated segment) and over.pinY (the whole authored
// profile), and reports every crossing it measured. `forceLevelY` > 0 holds a
// level decided elsewhere (the four ramps of one level must be COPLANAR even
// where the ground under one of them runs high). Pure: no device, no
// registration.
FlyoverPlan planFlyoverGaps(RoadSpec& over, float startDatum, float endDatum,
                            const std::vector<UnderRoute>& under,
                            float structDepthM, float clearM,
                            float maxGrade, float maxGradeRate,
                            float forceLevelY = 0.0f);

// ---------------------------------------------------------------------------
struct StackResult {
    bool        built = false;
    const char* whyNot = "";

    // the frame
    uint32_t fwyNode = 0;
    float cx = 0.0f, cz = 0.0f;          // the crossing point (on A's centreline)
    float tX = 1.0f, tZ = 0.0f;          // A's unit tangent there
    float pX = 0.0f, pZ = 1.0f;          // right-perp of tX == B's axis
    float medianHalfA = 0.0f, medianHalfB = 0.0f;
    float baseSurfaceY = 0.0f;           // highest A pavement under the pile
    float levelY[3] = { 0, 0, 0 };       // L2 deck, L3 ramps, L4 ramps
    float arcRadiusDesignM = 0.0f;       // the authored sweep radius
    float rampOffsetM = 0.0f;            // straight legs' lateral offset

    // L1 — the freeway the Stack was measured onto (copied so the structure
    // builder and the gates never depend on the host's lifetime)
    RoadSpec           aSpec;
    std::vector<float> aRoadY;

    // L2 — the crossing freeway
    RoadSpec           bSpec;
    std::vector<float> bRoadY;
    RoadBuildResult    bRoad;
    float              abutS = 0.0f;     // B's deck abutment faces at +-s
    float              bClearanceM = 0.0f;

    // L3 / L4 — four directional flyovers in pinwheel order 0..3 (each the
    // 90-degree-rotated copy of the last). 0 and 2 ride L3, 1 and 3 ride L4.
    struct Ramp {
        bool               built = false;
        RoadSpec           spec;
        std::vector<float> roadY;
        RoadBuildResult    road;
        FlyoverPlan        plan;
        float              arcRadiusM = 0.0f;   // MEASURED tightest bend
        int                level = 3;
        float              termAy = 0.0f, termBy = 0.0f;
    } ramp[4];

    float minClearanceM = 1e9f;          // worst over EVERY crossing, every level
    uint32_t crossingCount = 0;
    float siteScore = 0.0f;
};

// Register the whole Stack against the freeway. `avoid`: every other route's
// centreline (the diamond's crossroad and ramps included — the Stack must not
// be sited on top of them).
StackResult registerStack(const RoadSpec& fwySpec,
                          const std::vector<float>& fwyRoadY,
                          const std::vector<const RoadSpec*>* avoid = nullptr);

// ---------------------------------------------------------------------------
// THE STRUCTURE PLAN — pure geometry, so --test-stack gates exactly what
// buildStack() emits (a gate that measures a re-derivation is not a gate).
// ---------------------------------------------------------------------------
struct StackDeckStation {
    float x = 0, z = 0, y = 0;     // centreline + DATUM (pavement rides y+proud)
    float tx = 1, tz = 0;          // unit tangent
    float u  = 0;                  // arc length along this run
};
struct StackDeckRun {
    std::vector<StackDeckStation> s;
    float halfW = 5.0f;
    float depth = kStackRampDepthM;
    int   level = 3;               // 2, 3 or 4 — for the log and the gates
    std::string name;
};
struct StackPier {
    float x = 0, z = 0;
    float ySoffit = 0, yGround = 0;
    float px = 1, pz = 0;          // deck perp (the hammerhead's axis)
    float capHalfLenM = 5.0f;
    float spanM = 0.0f;            // distance back to the previous support
    uint32_t run = 0;
};
struct StackStructurePlan {
    std::vector<StackDeckRun> runs;
    std::vector<StackPier>    piers;
    float deckM     = 0.0f;   // metres of elevated deck
    float parapetM  = 0.0f;   // metres of parapet (both edges of every deck)
    float maxPierM  = 0.0f;   // THE acceptance gate echo_roads taught us
    float maxSpanM  = 0.0f;   // longest unsupported span
    float minPierClearM = 1e9f;   // closest a pier stands to any pavement edge
};
StackStructurePlan planStackStructure(const StackResult& st);

struct StackBuildResult {
    bool     ok = false;
    uint32_t meshCount = 0;
    uint32_t quadCount = 0;
    uint32_t pierCount = 0;
    float    maxPierM = 0.0f;
    float    maxSpanM = 0.0f;
    float    parapetM = 0.0f;
};
// The structure: twin box-girder decks for L2, four ramp decks for L3/L4,
// parapets with collision on every elevated edge, piers with footing / taper
// / hammerhead, abutments, and the gore tapers that join the ramps to the
// mainlines. Call after the terrain streamer exists and after the ribbons.
StackBuildResult buildStack(const StackResult& st, Scene& scene,
                            x3::rhi::IRenderDevice& device,
                            x3::phys::IPhysicsWorld& phys);

// --test-stack — headless. Gates:
//   S0 the network SURVEY: do two freeway-class routes cross? (measured —
//      they do not, which is why the Stack brings its own crossing freeway)
//   S1 the Stack registers on a measured site, clear of the diamond
//   S2 FOUR LEVELS, each above the last by the clearance law + structure
//   S3 clearance >= 16.5 ft at EVERY measured crossing, every level
//   S4 grades <= 6% and the K-rate law held on every ramp and on L2
//   S5 HIGH-SPEED ARCS: every ramp's tightest bend is 150-300 m
//   S6 no jointed bends at ramp scale (render-path facets)
//   S7 NO median crossover inside the Stack zone; the rhythm survives
//   S8 PARAPET CONTINUITY: every metre of elevated deck is walled both sides
//   S9 piers stand clear of every route's pavement (traffic cannot hit one)
//   S10 determinism: re-registration carves bit-identically
bool runStackSelfTest();

} // namespace x3::game
