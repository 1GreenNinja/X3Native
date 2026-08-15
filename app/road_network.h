#pragma once
// ---------------------------------------------------------------------------
// ROAD NETWORK — long routes carved into the terrain as CHAINED corridors.
//
// A TerrainCorridor is a 32-node polyline. That is plenty for a tunnel and
// nowhere near enough for a ring road, so a long route is registered as a CHAIN
// of corridors sharing endpoint nodes. The union is deepest-wins and the joint
// carries the same depth from both sides, so a chained seam is invisible — the
// same property --test-terraincorridor C4 already proves for a single polyline's
// interior joints.
//
// WHY NOT TunnelRoute: that type carries a tunnel's whole derivation — bore
// span detection, portal plugs, backfill lid, cut-and-cover grading. A plain
// road needs none of it. It needs a graded centreline and a carve. Tunnels stay
// where they are and get used where a route meets a mountain.
//
// CORRIDORS ONLY CUT. terrain.h's depression can lower ground and never raise
// it, so a road can be cut into a hillside but cannot be embanked across a
// hollow. The grader therefore only ever pulls the road DOWN to meet the
// ground, never floats it above — see gradeRoad() in the .cpp. Fill, viaducts
// and bridges are how the real network will cross low ground; this is the
// honest v1 and it says so.
//
// BOOT CONTRACT: like every corridor producer, these must be registered BEFORE
// the first terrain height query / TerrainStreamer::init(). See app/terrain.h.
// ---------------------------------------------------------------------------
#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"

#include <cstdint>
#include <string>
#include <vector>

namespace x3::game {

// THE CROSS-SECTION, in the units Tim gave it (feet), converted once here.
//
//      |<--------------------- 88 ft paved --------------------->|
//      | 20 ft apron |  12 | 12 | 12 | 12  (4 lanes) | 20 ft apron |
//                    |<------- 48 ft running -------->|
//
// The APRONS are load-bearing, not trim: Tim asked for "HUGE cement aprons on
// the side.. that you can pull a dead car on to". A car is ~15 ft long, so 20 ft
// of shoulder is the width where that is actually true rather than nearly true.
constexpr float kLaneFt      = 12.0f;   // US freeway standard
constexpr int   kLaneCount   = 4;
constexpr float kApronFt     = 20.0f;   // each side; a dead car is ~15 ft
constexpr float kFtToM       = 0.3048f;
constexpr float kRunningHalfM = (kLaneFt * (float)kLaneCount * 0.5f) * kFtToM;  // 24 ft
constexpr float kPavedHalfM   = kRunningHalfM + kApronFt * kFtToM;              // 44 ft

// One authored route: a centreline in world XZ. Y is derived from the terrain.
struct RoadSpec {
    std::string name  = "road";
    // Carve half-width. Must cover the FULL paved width or the apron's outer
    // edge lands on uncut ground and the shoulder tilts into the hillside.
    float halfWidth   = kPavedHalfM + 1.0f;
    float falloff     = 14.0f;   // smoothstep run outward from halfWidth (m)
    float maxGrade    = 0.07f;   // 7% — a real mountain highway's ceiling
    std::vector<float> x, z;     // centreline nodes, world (same length, >= 2)
};

// What actually got built, for logging and for the gates.
struct RoadBuildResult {
    bool     ok            = false;
    uint32_t corridorCount = 0;   // how many chained TerrainCorridors
    uint32_t nodeCount     = 0;
    float    lengthM       = 0.0f;
    float    maxGradePct   = 0.0f;   // steepest graded segment, percent
    float    maxCutM       = 0.0f;   // deepest carve
    float    minRoadY      = 0.0f, maxRoadY = 0.0f;
};

// Grade the route against the natural height field and register it as chained
// corridors. Returns what was built; ok == false if the registry is full or the
// spec is degenerate.
RoadBuildResult registerRoad(const RoadSpec& spec);

// A closed ring of `nodeCount` nodes, radius `radiusM`, centred on (cx, cz).
// The ring closes exactly (last node == first) so the chain has no seam.
RoadSpec makeRingRoad(const char* name, float cx, float cz,
                      float radiusM, uint32_t nodeCount);

// THE INNER TOUR — Tim's 15-mile ring, laid around the tunnel ridge. One call so
// a host or a self-test can put it on the ground identically.
RoadBuildResult registerInnerRing();

// ---------------------------------------------------------------------------
// THE RIBBON — the surface you actually drive on.
//
// registerRoad() only CARVES: it grades a datum and tells the height field to
// cut down to it, which leaves an 88 ft graded cutting and nothing to drive on.
// This lays the pavement into that cutting:
//
//   * ASPHALT running surface, 48 ft of it, 4 lanes wide
//   * CEMENT APRONS, 20 ft each side, a different material because they are a
//     different surface — you can tell you have left the running lane
//   * LANE MARKINGS: solid white at both edges of the running surface, dashed
//     white on the three interior lane lines
//   * collision, so the car drives ON it rather than through it
//
// Must be called AFTER the terrain streamer exists (it reads the carved field)
// and after registerRoad() put the corridor in — the ribbon follows the GRADED
// datum, not the raw ground.
// ---------------------------------------------------------------------------
class Scene;

struct RoadRibbonResult {
    bool     ok        = false;
    uint32_t meshCount = 0;
    uint32_t quadCount = 0;
    float    lengthM   = 0.0f;
};

RoadRibbonResult buildRoadRibbon(const RoadSpec& spec, Scene& scene,
                                 x3::rhi::IRenderDevice& device,
                                 x3::phys::IPhysicsWorld& phys);

// --test-roadnetwork
bool runRoadNetworkSelfTest();

} // namespace x3::game
