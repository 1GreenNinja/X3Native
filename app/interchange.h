#pragma once
// ---------------------------------------------------------------------------
// THE DIAMOND INTERCHANGE — the network's first STRUCTURAL grade split.
//
// Why it exists: every road in the network meets the freeway AT GRADE.
// attachRoadEndToRoute retreats a branch to the junction setback and
// buildJunctionMouth lays a ruled twist patch with swooping fillets — honest
// machinery, and still a T-JUNCTION GLUED ONTO AN EIGHT-LANE DIVIDED FREEWAY.
// Freeways do not have T-junctions: crossing traffic would stop in the
// median, which is wrong for the player and miserable for the 300-car AI
// fleet (app/traffic.cpp). road_network.h already names the fix, verbatim:
// "A STRUCTURAL grade split — one route bridging over another — is different
// machinery: a span gap over the lower road plus RAMP branches built from
// exactly this attachment."  This module IS that machinery, COMPOSED from
// parts that all work today (NO_SLOP rule 1 — nothing here is new physics):
//
//   * the CROSSROAD is a RoadSpec whose reach over the freeway is a
//     RoadSpec::Gap — the same span mechanism the outer tour's five bores
//     and the river bridge deck ride. Gap segments neither carve nor get a
//     ribbon; the deck owns them. Gap datum y0 == y1 == deckY, level, like
//     Bridge No. 1.
//   * the DECK is built like buildRiverBridge builds its deck: slab +
//     parapets (collision) + abutments + wingwalls, plus ONE median pier —
//     the freeway median is the only ground the span owns.
//   * the four RAMPS are short RoadSpecs at HALF the base cross-section
//     (widthScale 0.5 — two 12 ft lanes), landing on the freeway and the
//     crossroad through the SAME junction machinery every branch uses
//     (setback terminal, pinned datum, throat box, mouth patch, swooping
//     merge fillets). Their 48 m radius floor sits deliberately UNDER the
//     200 m class floor — per-spec floors are design, precedent set by the
//     range circuit's ~68 m hairpin and the summit spur's 14% grade.
//   * planTurnarounds SUPPRESSION: an at-grade junction landing earns a
//     median crossover (crossing traffic needs its gap there); a ramp pair
//     must NOT (a median U-turn beside an off-ramp). registerInterchange
//     notes an INTERCHANGE ZONE (road_network.h) and the turnaround planner
//     refuses to pave a crossover inside it.
//
// The site is chosen by MEASUREMENT (NO_SLOP rule 9), not picked on a map:
// every candidate freeway node is scored for local straightness, median
// width (the pier wants a real median), terrain fit under the crossroad's
// approach embankments, and clearance from every existing junction and
// route. Deterministic in (spec, terrain).
//
// Toward Tim's I-17/I-10 STACK: a stack is this SAME mechanism applied
// recursively — ramps that carry their own span gaps over other ramps. The
// diamond is deliberately the first composable storey of it.
//
// BOOT CONTRACT: registers corridors (throat boxes) and reads the carved
// field — call after every other road, before the first terrain height
// query / TerrainStreamer::init(). Same slot as every corridor producer.
// ---------------------------------------------------------------------------
#include "road_network.h"

namespace x3::game {

class Scene;

// Vertical clearance law over the freeway: 16.5 ft — the US standard minimum
// for interstate overpasses — measured from the freeway's pavement surface to
// the deck SOFFIT, across the full paved width of BOTH carriageways.
constexpr float kOverpassClearM = 5.03f;   // 16.5 ft
// Structure depth of the deck box girder (top of pavement to soffit).
constexpr float kOverpassDepthM = 1.6f;
// The zone radius: covers all four ramp landings (~340 m out) with margin.
constexpr float kInterchangeZoneR = 430.0f;

struct InterchangeResult {
    bool        built = false;
    const char* whyNot = "";

    // the crossroad, with its span gap over the freeway
    RoadBuildResult    road;
    RoadSpec           spec;
    std::vector<float> roadY;
    uint32_t           fwyNode = 0;     // freeway spec node under the deck
    float cx = 0.0f, cz = 0.0f;         // crossing point (freeway centreline)
    float tX = 1.0f, tZ = 0.0f;         // freeway unit tangent at the crossing
    float cX = 0.0f, cZ = 1.0f;         // crossroad unit axis (perp, toward +side)
    float deckY = 0.0f;                 // deck datum (gap y0 == y1)
    float abutS = 0.0f;                 // abutment faces at +-s along the axis
    float medianHalfAtCrossing = 0.0f;  // for the pier + the gates
    float fwyMaxSurfaceY = 0.0f;        // highest freeway pavement under the span
    float clearanceM = 0.0f;            // measured min soffit - pavement clearance
    float fwyUAtDeck = 0.0f;            // arc length along the freeway at the crossing

    // the four ramps, one per quadrant: [0]=(-t,-c) [1]=(-t,+c) [2]=(+t,-c) [3]=(+t,+c)
    struct Ramp {
        bool               built = false;
        RoadBuildResult    road;
        RoadSpec           spec;
        std::vector<float> roadY;
        RoadJunction       fwyJct;      // mouth onto the freeway
        RoadJunction       crossJct;    // mouth onto the crossroad
        uint32_t           fwyNode = 0;
        float              filletR = 0.0f;   // measured tightest bend (m)
    } ramp[4];
};

// Register the whole interchange against the freeway (the inner tour).
// `avoid`: every other registered route's centreline — the crossroad and the
// ramps must stay off them (rule R2's law, held by construction here).
InterchangeResult registerInterchange(const RoadSpec& fwySpec,
                                      const std::vector<float>& fwyRoadY,
                                      const std::vector<const RoadSpec*>* avoid = nullptr);

// The overpass structure: deck slab riding the gap datum, parapets WITH
// collision, abutment walls + wingwalls at the gap edges, one median pier.
// Cement set, same family as the tunnel portals and Bridge No. 1. Call after
// the terrain streamer exists and after the crossroad's ribbon is in.
struct OverpassBuildResult {
    bool     ok = false;
    uint32_t meshCount = 0;
    uint32_t quadCount = 0;
};
OverpassBuildResult buildOverpassDeck(const InterchangeResult& ic, Scene& scene,
                                      x3::rhi::IRenderDevice& device,
                                      x3::phys::IPhysicsWorld& phys);

// --test-interchange — headless. Gates:
//   I1 the interchange registers on the freeway (site chosen by measurement)
//   I2 deck clearance >= 16.5 ft over the FULL paved width of both carriageways
//   I3 every ramp holds grade <= 6.5% and lands both datums at grade
//   I4 NO median crossover inside the interchange zone (the ramp-pair law)
//   I5 ramp flow: under-floor fillet radii survive (45-60 m), no jointed bends
//   I6 the crossing is STRUCTURAL: outside the gap the crossroad never comes
//      within the freeway's paved width (R2's law, held deliberately)
//   I7 determinism: re-registration carves bit-identically
bool runInterchangeSelfTest();

} // namespace x3::game
