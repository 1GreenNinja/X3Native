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

    // THE SPAN GAP (bores + bridges). A gap is a run of nodes [i0..i1] whose
    // SEGMENTS neither carve nor get a ribbon: something else owns that reach —
    // a tunnel bore (the tunnel corridor cuts the mountain, the ring must NOT,
    // or deepest-wins would trench the very hill the bore goes under and leave
    // the backfill lid floating), or a bridge deck (the ground under the span
    // is untouched river, by design). The road DATUM is pinned across the gap,
    // lerped y0 -> y1: for a bore these are the tunnel's own end datums so the
    // carves meet without a step; for a bridge y0 == y1 == the deck elevation.
    struct Gap { uint32_t i0 = 0, i1 = 0; float y0 = 0.0f, y1 = 0.0f; };
    std::vector<Gap> gaps;

    // OPTIONAL PER-NODE DATUM PINS (NaN = free; empty = none). Same machinery
    // the gaps use internally, exposed for the case a JUNCTION needs: a road
    // that BRANCHES from another road must arrive at the other road's graded
    // datum exactly, or the two pavements meet with a step. A pinned node is
    // held through the grade relaxation and gets the same bounded ceiling-raise
    // ramp as a portal pin, so the approach can climb to it at maxGrade. If a
    // pin cannot be honoured the deficit lands in RoadBuildResult::pinErrM,
    // loudly — never silently.
    std::vector<float> pinY;
};

// What actually got built, for logging and for the gates.
struct RoadBuildResult {
    bool     ok            = false;
    uint32_t corridorCount = 0;   // how many chained TerrainCorridors
    uint32_t nodeCount     = 0;
    float    lengthM       = 0.0f;
    float    maxGradePct   = 0.0f;   // steepest graded segment, percent
    float    maxCutM       = 0.0f;   // deepest carve (open reaches only; gaps carve nothing)
    float    minRoadY      = 0.0f, maxRoadY = 0.0f;
    // Worst |graded datum - pinned datum| over the pinned nodes. A pin the
    // relaxation could not hold means the approach cannot reach the structure
    // at this grade — an authoring error, and it must be loud, not silent.
    float    pinErrM       = 0.0f;
    float    gapLenM       = 0.0f;   // route length inside gaps (bored/decked)
    // Highest the datum floats ABOVE the natural surface (the portal-ramp
    // approaches — see registerRoad). 0 on a road with no gaps.
    float    maxFloatM     = 0.0f;
};

// Grade the route against the natural height field and register it as chained
// corridors. Returns what was built; ok == false if the registry is full or the
// spec is degenerate. If outRoadY is given it receives the graded datum per
// node — the ribbon and the bridge builder need the DATUM, not the carved
// ground, because over a pinned gap the two differ by design.
RoadBuildResult registerRoad(const RoadSpec& spec,
                             std::vector<float>* outRoadY = nullptr);

// A closed ring of `nodeCount` nodes, radius `radiusM`, centred on (cx, cz).
// The ring closes exactly (last node == first) so the chain has no seam.
RoadSpec makeRingRoad(const char* name, float cx, float cz,
                      float radiusM, uint32_t nodeCount);

// THE INNER TOUR — Tim's 15-mile ring, laid around the tunnel ridge. One call so
// a host or a self-test can put it on the ground identically.
RoadBuildResult registerInnerRing();

// ---------------------------------------------------------------------------
// THE OUTER TOUR — Tim's 31-mile ring, and it is a TOUR, not a circle.
//
// Measured (survey 2026-08-15, 1024 samples/circle at r 7600/7934/8300 about
// the ring centre): a naive circle at the nominal 4.93-mile radius drives
// through the north range's massif (θ 88-110°, graded cut up to 236 m = 775 ft)
// and runs near-PARALLEL to the west range's spine for ~60° of arc (cuts to
// 213 m). Terrain genuinely forces bores. Tim's ruling: "We CAN drive through a
// mountain!!!! we have TUNNELS!!!!" — so the tour stays close to the circle and
// BORES the peak groups, riding measured flank benches and saddles in the open
// between them.
// ---------------------------------------------------------------------------
struct TunnelRoute;   // app/tunnel_corridor.h — the bores are real tunnels

// One bore chord of the tour: a straight reach the ring hands over to a tunnel.
struct BoreChord {
    const char* name = "bore";
    float x0 = 0.0f, z0 = 0.0f, x1 = 0.0f, z1 = 0.0f;   // chord ends (world XZ)
    uint32_t i0 = 0, i1 = 0;                            // ring node span of the gap
};

// Build the authored tour polyline + its bore chords. Pure authoring — no
// terrain query, no registration — so the self-test can interrogate the shape
// (and register it WITHOUT the bores as a negative control).
RoadSpec makeOuterTour(std::vector<BoreChord>* outBores);

struct OuterRingResult {
    RoadBuildResult    road;
    uint32_t           boreCount = 0;       // tunnels registered AND roofed
    float              boredLenM = 0.0f;    // total roofed length
    std::vector<const TunnelRoute*> bores;  // one per chord, in tour order
    std::vector<float> roadY;               // graded datum per ring node
    RoadSpec           spec;                // the polyline actually registered
};

// BOOT ENTRY POINT: register the bores (tunnels first — the ring's gap edges
// pin to their end datums), then the ring road around them. Same registry
// contract as every corridor producer: call before the first height query.
OuterRingResult registerOuterRing();

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

// `roadY` (optional): the graded datum per node, from registerRoad(). With it,
// the pavement rides the DATUM — which matters wherever another, deeper carve
// crosses the road (deepest-wins would drag a ground-derived ribbon into the
// other cut) and across pinned approaches. Without it the legacy behaviour
// (recover the datum from the carved field) is unchanged. Gap segments are
// skipped either way: a bore's ribbon belongs to its tunnel, a bridge's to its
// deck, and a second coplanar ribbon there would z-fight both.
RoadRibbonResult buildRoadRibbon(const RoadSpec& spec, Scene& scene,
                                 x3::rhi::IRenderDevice& device,
                                 x3::phys::IPhysicsWorld& phys,
                                 const std::vector<float>* roadY = nullptr);

// ---------------------------------------------------------------------------
// JUNCTIONS — where one road MEETS another.
//
// Measured before this existed (scripts/audit_connectivity.py, 2026-08-16):
// the spawn corridor's far end dead-ends 3,522 m (11,555 ft) short of the
// inner tour, 6,480 m short of the outer tour, 778 m short of the river road.
// Tim: "They need to CONNECT to the roads you spawn on." So: a CONNECTOR from
// the spawn corridor's far end to the inner ring, and a SUMMIT SPUR climbing
// off the connector ("roads that go UP on top of the mountain").
//
// A junction is where two independently-graded ribbons meet, and a butt joint
// there shows a seam (and, where the main road has grade, a wedge-shaped gap:
// the branch's end edge is laterally FLAT while the main road's surface slopes
// along its own axis under it). buildJunctionMouth() closes that with a ruled
// transition patch: it starts at the branch ribbon's flat terminal edge and
// twists onto the main road's sloped surface, overlapping the main pavement a
// couple of metres with a few millimetres of lift (lapped, not coplanar — no
// z-fight), plus cement flare wings so the mouth reads as a widened apron
// rather than a T of butted rectangles. An honest v1: a real intersection
// system (kerb radii, stop lines, signage, traffic priority) is future work.
// ---------------------------------------------------------------------------
struct RoadJunction {
    bool  valid = false;
    float jx = 0.0f, jz = 0.0f, jy = 0.0f;  // junction point ON the main road's centreline + datum
    float mainTX = 1.0f, mainTZ = 0.0f;     // main road unit tangent there
    float mainGrade = 0.0f;                 // main datum slope along (mainTX, mainTZ), m/m
    float endX = 0.0f, endZ = 0.0f, endY = 0.0f;  // branch ribbon's terminal node + datum
};

// The mouth patch (asphalt transition + cement flare wings), with collision.
RoadRibbonResult buildJunctionMouth(const RoadJunction& j, Scene& scene,
                                    x3::rhi::IRenderDevice& device,
                                    x3::phys::IPhysicsWorld& phys);

// ---------------------------------------------------------------------------
// THE SPAWN CONNECTOR — the road from the spawn corridor's far end (past the
// tunnel exit portal) to the inner tour. Registered AFTER the ring (its
// junction pin needs the ring's graded datum, and its natural-surface sweep
// then reads the ring's already-carved cutting at the landing). The centreline
// is a gentle S-curve — Tim: "they do not curve" — with both ends pinned:
// node 0 to the spawn route's exit datum, the last node to the ring's datum at
// the landing node. The last leg arrives radially (square to the ring).
// ---------------------------------------------------------------------------
struct SpawnConnectorResult {
    RoadBuildResult    road;
    RoadSpec           spec;
    std::vector<float> roadY;      // graded datum per node (the ribbon rides this)
    RoadJunction       ringJct;    // where it lands on the inner ring
    uint32_t           ringNode = 0;   // ring spec node index of the landing
    float gapBeforeM = 0.0f;       // measured spawn-route -> ring gap this closes
};
SpawnConnectorResult registerSpawnConnector(const TunnelRoute& spawnRoute,
                                            const RoadSpec& ringSpec,
                                            const std::vector<float>& ringRoadY);

// ---------------------------------------------------------------------------
// THE SUMMIT SPUR — one climbing road from the connector up to a real local
// peak, found by hill-climbing the natural (pre-carve) height field from seeds
// beside the connector. Steeper than the tours (maxGrade up to 14%) and built
// with sawtooth switchback legs when the direct line is steeper than that.
// The peak is required to stay clear of the spawn tunnel's spine so the spur's
// carve can never undermine the backfill lid. If no peak within reach earns a
// road (prominence < ~35 m), it reports why and builds nothing — honestly.
// ---------------------------------------------------------------------------
struct SummitSpurResult {
    bool               built = false;
    const char*        whyNot = "";
    RoadBuildResult    road;
    RoadSpec           spec;
    std::vector<float> roadY;
    RoadJunction       jct;        // where it leaves the connector
    float peakX = 0.0f, peakZ = 0.0f, peakNaturalY = 0.0f;
    float climbM = 0.0f;           // datum climb, junction -> summit
    float summitCutM = 0.0f;       // how far below the true peak the road tops out
};
// `avoid` (optional): other routes' centrelines the spur must stay off — its
// carve would trench across their pavement otherwise.
SummitSpurResult registerSummitSpur(const RoadSpec& fromSpec,
                                    const std::vector<float>& fromRoadY,
                                    const TunnelRoute* keepClearOf,
                                    const std::vector<const RoadSpec*>* avoid = nullptr);

// --test-roadnetwork
bool runRoadNetworkSelfTest();

} // namespace x3::game
