#pragma once
// ---------------------------------------------------------------------------
// THE RIVER CROSSING — the valley road and Bridge No. 1.
//
// The problem, measured before anything was built: as authored, NOTHING crosses
// the river. Its entire run is 0.20-0.89 miles from the origin (kRiverX/kRiverZ,
// terrain.cpp); the inner tour orbits at 2.39 miles and the outer at ~4.9.
// Nearest approach ~1.5 miles. Tim has asked repeatedly for "deep rivers with
// fish and beautiful lit concrete bridges" — and there was nothing to span.
//
// ROAD_NETWORK_PLAN.md settled the approach ("both, valley route first"): bring
// a road to the water. This module lays THE VALLEY ROAD — inner ring's south
// arc, north past the ocean basin's west shore, across the river SQUARE to the
// channel on the plan's named site (the N5-N6 reach, Bridge No. 1), then east
// to the inner ring's east arc. Two ring junctions, one bridge, and the river
// finally earns its crossing.
//
// WHY A BRIDGE AND NOT A CARVE — the plan's pass-2 analysis, verified here by
// the self-test's negative control: corridors only LOWER ground, so a road cut
// across the channel (a) cannot fill the 223 ft of water it meets, (b) leaves
// the graded datum hanging above the bed when the 7% clamp meets the 14% bank,
// and (c) notches the LEVEE below the waterline — a dry trench beside standing
// water. The carve therefore STOPS at the span (RoadSpec::Gap — the same span
// mechanism the outer tour's bores use) and a DECK carries the road.
//
// THE STRUCTURE (from the plan's engineering table, low-set option):
//   continuous prestressed-concrete HAUNCHED BOX GIRDER, three spans 80/120/80
//   ft, deck 280 ft x 43 ft out-to-out, two rounded-nose wall piers at +-60 ft
//   of the channel centreline (outside the 79 ft full-depth floor), structure
//   depth 6 ft at the piers haunching to 3.5 ft at midspan, seat abutments on
//   the levee crests, approach slabs behind them, 2.9 ft parapets WITH
//   collision, deck top = crest + 2 ft. Emissive-first lighting: eight parapet
//   lamps as emissive geometry, zero pooled lights spent (the merged tunnel
//   light pool is tunnel-only today; pier uplights join when a general pool
//   exists — recorded, not forgotten).
// ---------------------------------------------------------------------------
#include "road_network.h"

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"

#include <cstdint>
#include <vector>

namespace x3::game {

class Scene;

// Every DECISION the bridge makes, computed headlessly from the same public
// terrain/water queries the world is built from. planRiverBridge() is pure:
// no registration, no GPU, no state — the self-test interrogates the plan and
// the builder consumes it, so what is tested is what is built.
struct RiverBridgePlan {
    bool  ok = false;
    const char* whyNot = "";      // set when !ok — a plan that fails says why

    // the crossing
    float cx = 0.0f, cz = 0.0f;   // channel centreline crossing point (world XZ)
    float dirX = 0.0f, dirZ = 0.0f;   // deck axis (unit, points NE across the river)
    float skewDeg = 0.0f;         // |90° - angle(deck, river)| — plan wants <= 15°
    float waterY = 0.0f;          // water surface at the crossing
    float bedY = 0.0f;            // carved channel bed at the crossing

    // the deck (all positions on the deck axis through (cx,cz))
    float deckY = 0.0f;           // deck TOP surface Y (crest + 2 ft, low-set)
    float abutS = 42.65f;         // abutment faces at +-s (m): 280 ft deck
    float slabS = 99.25f;         // approach slabs run out to +-s (m)
    float deckHalfWidth = 6.55f;  // 43 ft out-to-out / 2
    float crestW = 0.0f, crestE = 0.0f;   // measured bank crests (info/log)

    // the piers
    float pierS = 18.29f;         // +-60 ft from channel centre
    float pierTopY = 0.0f;        // underside of the girder at the pier
    float pierBedY[2] = { 0.0f, 0.0f };   // sampled carved bed at each pier
    float pierHalfThick = 0.915f; // 6 ft thick wall pier
    float pierHalfWide  = 3.05f;  // 20 ft wide (along the river)

    // structure depths (haunched box)
    float depthPier = 1.83f;      // 6 ft at the piers
    float depthMid  = 1.07f;      // 3.5 ft at midspan
    float soffitClearM = 0.0f;    // midspan soffit above waterY (B8 wants >= 4 ft)
};

// Pure decision pass. Reads terrain + water queries only.
RiverBridgePlan planRiverBridge();

// The valley road's polyline, authored around the plan's crossing. Exposed so
// the self-test can register it WITHOUT the span gap (the negative control).
// outGapA/outGapB receive the node indices of the slab ends.
RoadSpec makeValleyRoad(const RiverBridgePlan& plan,
                        uint32_t* outGapA, uint32_t* outGapB);

// The registered valley road + everything the host needs to dress it.
struct RiverRoadResult {
    RiverBridgePlan    plan;
    RoadBuildResult    road;
    RoadSpec           spec;      // includes the span gap
    std::vector<float> roadY;     // graded datum (ribbon + bridge ride this)
};

// BOOT ENTRY POINT: plan the bridge, author the valley road around it,
// register the carve with the span gap. Registry contract as ever: call
// before the first terrain height query.
RiverRoadResult registerRiverRoad();

// Build the bridge meshes + collision into the scene. Deck, approach slabs,
// piers with waterline collars, abutment seats, parapets (with collision),
// eight emissive parapet lamps. Requires the road to be registered (the deck
// datum comes from the plan) and the terrain streamer to exist.
struct RiverBridgeBuildResult {
    bool     ok = false;
    uint32_t meshCount = 0;
    uint32_t triCount  = 0;
};
RiverBridgeBuildResult buildRiverBridge(const RiverBridgePlan& plan, Scene& scene,
                                        x3::rhi::IRenderDevice& device,
                                        x3::phys::IPhysicsWorld& phys);

// --test-riverbridge — headless. RB0 negative control (the at-grade crossing
// notches the levee below the waterline — the defect the span gap exists to
// prevent), RB1 the river actually flows (waterY strictly descends), RB2 the
// plan's structural sanity (skew, pier placement, soffit clearance), RB3 the
// road genuinely meets the water, RB4 the span gap leaves the river untouched,
// RB5 the levee holds through the bridge reach, RB6 grades + pinned datums,
// RB7 determinism.
bool runRiverBridgeSelfTest();

} // namespace x3::game
