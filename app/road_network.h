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
// ONE profile, applied to every route — no route can opt out of a strip:
//
//   |<------------------------- 96 ft paved ------------------------->|
//   | 20 ft apron | 4 ft shldr | 12 | 12 | 12 | 12 | 4 ft shldr | 20 ft apron |
//                              |<---- 48 ft running ---->|
//   [prism skirt]                [edge line at the running edge]      [prism skirt]
//   (guardrail wherever the drop test fires, just inside the apron edge)
//
// The APRONS are load-bearing, not trim: Tim asked for "HUGE cement aprons on
// the side.. that you can pull a dead car on to". A car is ~15 ft long, so 20 ft
// of apron is the width where that is actually true rather than nearly true.
// The SHOULDERS are Tim's "Still need shoulders, and aprons": a 4 ft paved
// asphalt strip between the edge line and the cement, weathered a shade
// differently so leaving the running surface READS.
constexpr float kLaneFt      = 12.0f;   // US freeway standard
constexpr int   kLaneCount   = 4;
constexpr float kShoulderFt  = 4.0f;    // paved asphalt shoulder, each side
constexpr float kApronFt     = 20.0f;   // each side; a dead car is ~15 ft
constexpr float kFtToM       = 0.3048f;
constexpr float kRunningHalfM  = (kLaneFt * (float)kLaneCount * 0.5f) * kFtToM;  // 24 ft
constexpr float kShoulderHalfM = kRunningHalfM + kShoulderFt * kFtToM;           // 28 ft
constexpr float kPavedHalfM    = kShoulderHalfM + kApronFt * kFtToM;             // 48 ft

// ---------------------------------------------------------------------------
// THE FREEWAY — twin separate carriageways, I-17 style (Tim: "make the road
// wider... much wider. Its a freeway", then "If we make 8 lanes each side,
// that is better. You can have a separate road carrying north and south
// traffic like I17 does in AZ").
//
// EACH carriageway is a full roadway of its own — 8 x 12 ft lanes, the same
// 4 ft shoulders / 20 ft aprons / prism skirts / own barriers as the base
// profile — and the two are separated by a MEDIAN that varies with the
// terrain: tens of metres of graded ground where the country is close to the
// datum, narrowing to a concrete jersey-wall median (the F-shape from the
// barrier work) where the route is in cut or on fill. TURNAROUND CROSSOVERS
// pave the median every ~1.7 km (and at every junction landing, so crossing
// traffic has its gap where the side road arrives). Lane paint is WHITE ONLY
// — solid edges, dashed lane lines, and NO double yellow: opposing traffic
// is on the other roadway, which is the whole point of dividing it.
//
// One centreline drives everything (RoadSpec::dualCarriageway): the grader,
// the corridor chain (ONE chain — the whole span is carved flat, so the
// median is graded ground like a real depressed freeway median, and the
// registry budget is untouched), the ribbon, the barriers and the junctions
// all derive the twin roadways from it. Tunnels stay 4-lane bores, so routes
// that hand reaches to a bore keep the base profile; the freeway profile is
// applied to the INNER TOUR (the main drag) first.
// ---------------------------------------------------------------------------
constexpr int   kFwyLaneCount     = 8;                                             // per carriageway
constexpr float kFwyRunningHalfM  = (kLaneFt * (float)kFwyLaneCount * 0.5f) * kFtToM; // 48 ft
constexpr float kFwyShoulderHalfM = kFwyRunningHalfM + kShoulderFt * kFtToM;          // 52 ft
constexpr float kFwyPavedHalfM    = kFwyShoulderHalfM + kApronFt * kFtToM;            // 72 ft
constexpr float kFwyMedianMinHalfM  = 1.1f;   // jersey-median: inner aprons abut the wall
constexpr float kFwyMedianMaxHalfM  = 12.0f;  // wide graded median (24 m between inner aprons)
constexpr float kFwyMedianWallHalfM = 3.0f;   // below this half-width the median wall stands
// Total half-span of the dual cross-section at the widest median, + 1 m of
// cut ground past the outer apron edges (same margin the base profile keeps).
constexpr float kFwyDualMaxHalfM  = kFwyMedianMaxHalfM + 2.0f * kFwyPavedHalfM + 1.0f;
constexpr float kFwyTurnaroundSpacingM = 1700.0f;  // ~1.5-2 km, Tim's turnarounds
constexpr float kFwyTurnaroundLenM     = 42.0f;    // paved crossover length along the route

// One authored route: a centreline in world XZ. Y is derived from the terrain.
struct RoadSpec {
    std::string name  = "road";
    // Carve half-width. Must cover the FULL paved width or the apron's outer
    // edge lands on uncut ground and the shoulder tilts into the hillside.
    float halfWidth   = kPavedHalfM + 1.0f;
    float falloff     = 14.0f;   // smoothstep run outward from halfWidth (m)
    float maxGrade    = 0.07f;   // 7% — a real mountain highway's ceiling
    // VERTICAL-CURVE LIMIT: max |d(grade)/ds| per metre. Grade alone is not
    // drivability — a crest where +7% flips to -7% across one node is a KINK,
    // and a car at speed goes light or airborne over it ("the road changes
    // angles sharply with respect to elevation, making the car lose traction").
    // Real highways bound the RATE of grade change with parabolic vertical
    // curves (the K-value: metres of curve per 1% of grade change; this field
    // is 0.01/K). Default 5e-4/m == K 20 m/%: at 100 mph (44.7 m/s) the
    // vertical acceleration over such a curve is v^2 * rate = 1.0 m/s^2, a
    // tenth of g — the car stays loaded. The summit spur runs looser (its
    // design speed is a mountain switchback's, not a freeway's).
    float maxGradeRate = 5.0e-4f;
    // HORIZONTAL FLOW (the "sharp points" fix). Wave-2 bounded the VERTICAL
    // rate of change; nothing bounded the HORIZONTAL one, so a route sampled
    // at ~61 m facets — an 11.7 deg corner at every node of a 300 m arc reads
    // as a chain of kinks at speed. smoothHorizontalCurves() (called by every
    // route producer, before grading) Catmull-Rom-subdivides the polyline
    // until no node deflects more than maxDeflectionDeg, then eases any bend
    // tighter than minTurnRadiusM — the class floor: a freeway never asks for
    // a corner a freeway cannot take, while the range circuit's ~68 m hairpin
    // sits ABOVE its 60 m floor and is deliberately kept. X3_NO_HCURVE=1
    // skips the pass (the A/B instrument, mirroring X3_NO_VCURVE).
    float minTurnRadiusM   = 200.0f;   // class floor: bends tighter get eased
    float maxDeflectionDeg = 3.0f;     // max heading change per node after smoothing
    // TWIN CARRIAGEWAYS (see the FREEWAY block above): the centreline becomes
    // the median axis; registerRoad carves the full dual span, buildRoadRibbon
    // lays two 8-lane roadways + the median features, planRoadBarriers samples
    // each carriageway's own offside. Routes with bore/deck gaps must keep
    // this false — a tunnel is a 4-lane tube and cannot swallow a freeway.
    bool  dualCarriageway  = false;
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
    // VERTICAL FLOW, measured: max |d(grade)/ds| (per metre) after the grade
    // relaxation but BEFORE vertical-curve smoothing, and after it. The
    // before/after pair is the proof the smoothing did something real, and
    // the after value is what the self-test gates against spec.maxGradeRate.
    float    maxGradeRatePre  = 0.0f;
    float    maxGradeRatePost = 0.0f;
};

// ---------------------------------------------------------------------------
// HORIZONTAL CURVE SMOOTHING — the "sharp points" fix (see RoadSpec fields).
//
// Catmull-Rom SUBDIVISION, not relocation: every existing node keeps its
// exact position (so pinned junction nodes, portal chord ends and route ends
// are preserved by construction), and new nodes are inserted on the C1 spline
// through them until no node deflects more than spec.maxDeflectionDeg.
// Adaptive: straights get nothing, curves get refined — the node budget goes
// where the curvature is. Then a MINIMUM-RADIUS pass eases any bend tighter
// than spec.minTurnRadiusM (locked nodes never move; the ring seam stays
// welded). Deterministic. X3_NO_HCURVE=1 skips both passes — the A/B
// instrument, mirroring X3_NO_VCURVE — and nothing else should ever set it.
//
// `lockMask` (optional, sized to s.x): nonzero nodes are LAW — never moved by
// the easing pass, and segments between two consecutive locked nodes are kept
// STRAIGHT (subdivided linearly, not splined). makeOuterTour uses it for its
// bore chords: a tunnel's spine is straight, so the road polyline through the
// gap must stay on the chord.
// ---------------------------------------------------------------------------
struct HorizontalSmoothResult {
    uint32_t nodesBefore = 0, nodesAfter = 0;
    float    maxDeflBeforeDeg = 0.0f;   // worst per-node heading change, raw
    float    maxDeflAfterDeg  = 0.0f;   // ... after subdivision + easing
    float    minRadiusAfterM  = 0.0f;   // tightest surviving bend (curved nodes)
    uint32_t easedNodes       = 0;      // nodes the radius-floor pass moved
    bool     skipped          = false;  // X3_NO_HCURVE=1
    // old node index -> new node index (subdivision only inserts, so every
    // original node survives). Gap/pin bookkeeping remaps through this.
    std::vector<uint32_t> newIndexOfOld;
};
HorizontalSmoothResult smoothHorizontalCurves(RoadSpec& s,
                                              const std::vector<uint8_t>* lockMask = nullptr);

// Measure the horizontal flow of a spec, for the gates: worst per-node
// deflection (degrees) skipping gap reaches, their edge nodes and the open
// ends; optionally the tightest discrete curve radius over genuinely curved
// nodes (deflection > 0.25 deg).
float measureMaxDeflectionDeg(const RoadSpec& s, float* minRadiusM = nullptr);

// ---------------------------------------------------------------------------
// JUNCTION EXCLUSION ZONES — Tim, pinned against a skirt wall at a junction:
// "INTERSECTIONS NEED TO NOT HAVE RAILINGS.. AND THEY NEED SWOOPING CURVES
// FROM BOTH WAYS." Every junction (mouth centre AND branch end) is noted in a
// module registry when its junction box registers; barrier planning of BOTH
// types refuses to place anything within kJunctionBarrierClearM of a noted
// point, and the ribbon's prism skirt feathers down to a drivable batter
// through the same zone. Cleared alongside the corridor registry in tests.
// ---------------------------------------------------------------------------
constexpr float kJunctionBarrierClearM = 45.0f;
void     noteRoadJunction(float x, float z);
void     clearRoadJunctions();
uint32_t roadJunctionCount();
float    distToNearestRoadJunction(float x, float z);   // 1e9 if none noted

// Grade the route against the natural height field and register it as chained
// corridors. Returns what was built; ok == false if the registry is full or the
// spec is degenerate. If outRoadY is given it receives the graded datum per
// node — the ribbon and the bridge builder need the DATUM, not the carved
// ground, because over a pinned gap the two differ by design.
RoadBuildResult registerRoad(const RoadSpec& spec,
                             std::vector<float>* outRoadY = nullptr);

// A closed ring of `nodeCount` nodes, radius `radiusM`, centred on (cx, cz).
// The ring closes exactly (last node == first) so the chain has no seam.
// KEPT for the survey instrument and the O0 negative control — no shipping
// route is a circle any more (Tim: "its a perfect circle. NO roads do that").
RoadSpec makeRingRoad(const char* name, float cx, float cz,
                      float radiusM, uint32_t nodeCount);

// ---------------------------------------------------------------------------
// COURSE AUTHORING — nodes in, road out.
//
// A route is authored as a WAYPOINT POLYLINE with a fillet radius per corner:
// the generator emits straight reaches between waypoints and a constant-radius
// arc at each corner (tangent length r*tan(theta/2), clamped to fit the legs),
// resampled at `spacingM`. Straights are genuine straights, corners are
// genuine constant-radius arcs of whatever radius each corner asks for —
// which is exactly what a real road is and a noise-perturbed circle is not.
// This is also the door a hand-drawn network walks in through: digitise the
// sketch to a waypoint list, call this, get a road.
// ---------------------------------------------------------------------------
struct CourseWaypoint {
    float x = 0.0f, z = 0.0f;   // world XZ
    float fillet = 200.0f;      // corner arc radius (m); ignored at open ends
};
RoadSpec makeRoadFromWaypoints(const char* name,
                               const std::vector<CourseWaypoint>& pts,
                               float spacingM, bool closed);

// THE INNER TOUR — Tim's ~15-mile course around the tunnel ridge, third shape.
// v1 was a perfect circle and Tim rejected it from the world map ("NO roads do
// that"). Now a closed course from an authored leg list: straights, arcs of
// different radii, S-complexes, and a bulge toward the north foothills — one
// leg is a straight through the OLD ring node 173 position (-4135.7, 1132.2),
// square to the spawn corridor's exit ray, so the spawn connector still lands
// at the same junction. One call so a host or a self-test builds it identically.
RoadSpec makeInnerCourse();
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
// THE MEDIAN PLAN (dual routes) — per-node median HALF-width, decided by the
// terrain: wide (kFwyMedianMaxHalfM) where the natural country across the
// median zone sits within ~2.5 m of the graded datum, narrow
// (kFwyMedianMinHalfM, the jersey-wall median) where the route is in real cut
// or on fill. Slew-limited so the carriageway offset curves stay gentle. PURE
// and registration-order independent (the natural surface is recovered as
// field - corridorDelta), so registerRoad (which carves against it) and
// buildRoadRibbon / planRoadBarriers (which lay pavement and barriers by it)
// always agree. Returns one value per spec node; empty for a non-dual spec.
// ---------------------------------------------------------------------------
std::vector<float> computeMedianPlan(const RoadSpec& spec,
                                     const std::vector<float>& roadY);

// ---------------------------------------------------------------------------
// TURNAROUND CROSSOVERS (dual routes) — paved median gaps, as [u0, u1] arc-
// length intervals along the route: one every ~kFwyTurnaroundSpacingM, one at
// u=0 on a closed route (the spawn stands there), and one aligned at every
// noted junction landing within reach of the route (a side road meeting a
// divided freeway needs its median gap where it arrives). Gap reaches
// (bores/decks) never get one. Pure; deterministic in (spec, junctions).
// ---------------------------------------------------------------------------
struct RoadTurnaround { float u0 = 0.0f, u1 = 0.0f; };
std::vector<RoadTurnaround> planTurnarounds(const RoadSpec& spec);

// ---------------------------------------------------------------------------
// THE RENDER PATH — the fine-sampled spline every ribbon rides (the owner's
// "Get rid of ALLLLL jointed bends"). The corridor CARVE stays at the coarse
// nodes (registry budget untouched); the RIBBON — pavement edges, painted
// lines, shoulders, aprons, barrier runs, the median wall — samples a
// Catmull-Rom spline THROUGH the final smoothed nodes at 6..20 m intervals,
// adaptive by curvature, so a curve's edge line reads as a continuous arc at
// driving height instead of a chain of ~40-60 m facets. Original nodes are
// interpolated exactly (pins, portal edges and junction landings hold);
// gap-flagged reaches stay linear on the chord (a tunnel spine is straight).
// ---------------------------------------------------------------------------
struct RoadRenderStation {
    float    x = 0.0f, z = 0.0f;   // centreline position
    float    y = 0.0f;             // datum (pavement rides y + proud)
    float    tx = 1.0f, tz = 0.0f; // unit tangent
    float    u = 0.0f;             // arc length from the route start
    uint32_t seg = 0;              // source spec segment (barrier mask / gaps)
    bool     gap = false;          // inside a bore/deck gap: no ribbon here
    float    medianHalf = 0.0f;    // dual routes: median half-width here
};
void buildRoadRenderPath(const RoadSpec& spec, const std::vector<float>* roadY,
                         const std::vector<float>* medianPlan,
                         std::vector<RoadRenderStation>& out);

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
//   * THE ROAD PRISM: a concrete base skirt extruded DOWN from each apron's
//     outer edge — a vertical face (>= 0.6 m visible) then a battered face
//     whose bottom laps UNDER the carved terrain. Tim: "THICK CONCRETE in the
//     base and aprons.. not floating on top!!!" — a bare surface ribbon reads
//     as paper wherever the ground falls away from the apron edge; the skirt
//     is what makes the road read as a poured structure. Cement set, collision.
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
    // BARRIERS (Tim: "We really need BARRIERS." / "thick concrete barriers to
    // not go off in the ditch"): placed automatically from the drop test 6 m
    // beyond the apron. Drop > 2 m earns the W-beam guardrail (taller
    // protection); a ditch-depth drop (0.6–2 m) earns a continuous CONCRETE
    // JERSEY BARRIER — classic F-shape, ~0.81 m tall on a 0.6 m base, cement
    // set, full collision. Counts + drop extremes, for the gates.
    uint32_t railSegments = 0;
    float    railMinDropM = 0.0f;
    uint32_t jerseySegments = 0;
    float    jerseyMinDropM = 0.0f;
    // Dual-carriageway dressing (0 on single routes):
    uint32_t medianWallRuns  = 0;   // continuous jersey-median wall runs emitted
    uint32_t turnaroundCount = 0;   // paved median crossovers
    uint32_t workZoneCones   = 0;   // the work-zone taper's cones
    uint32_t workZoneBarrels = 0;   // ... and its drums (dynamic bodies)
    uint32_t fineStations    = 0;   // render-path stations the ribbon rode
};

// PURE barrier planning — which segments of a route earn a barrier, per side,
// from the drop-off test alone. Exposed separately so the self-test can gate
// placement without a render device. Per SEGMENT (spec node i -> i+1):
// bit0 = left W-beam, bit1 = right W-beam, bit2 = left jersey, bit3 = right
// jersey. Drop > 2 m rails; 0.6–2 m (the ditch band) gets the jersey wall;
// segments within kJunctionBarrierClearM of a noted junction get NOTHING —
// an intersection needs its mouths open, not railed shut.
struct BarrierPlan {
    std::vector<uint8_t> mask;
    uint32_t railSegments = 0;    // total W-beam (side,segment) pairs
    float    minDropM = 0.0f;     // smallest drop among railed segments
    uint32_t jerseySegments = 0;  // total jersey (side,segment) pairs
    float    jerseyMinDropM = 0.0f, jerseyMaxDropM = 0.0f;
};
BarrierPlan planRoadBarriers(const RoadSpec& spec, const std::vector<float>* roadY);

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
    // MAIN-ROAD CROSS-SECTION at the junction, as lateral offsets from the
    // main centreline. A branch landing on a DIVIDED freeway meets the NEAR
    // carriageway: the merge fillets go tangent to its outer shoulder edge and
    // the twist must be complete by its outer apron edge — both much further
    // out than a single roadway's. Producers fill these from the main spec
    // (median at the landing + the freeway offsets); the defaults are the
    // single-carriageway profile, so existing junctions are unchanged.
    float mainShoulderEdgeM = kShoulderHalfM;  // fillets go tangent here
    float mainPavedEdgeM    = kPavedHalfM;     // twist complete by here
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
// THE OUTER CONNECTOR — the road off the inner tour out to the 31-mile outer
// tour. Until it existed the outer tour was an ISLAND: audited at 2,958 m
// (9,705 ft) from the nearest inner-ring point, five real bores and thirty
// miles of pavement that nothing could drive to.
//
// Both tours are concentric about the same centre, so the shortest crossing is
// RADIAL and arrives square at both ends with no contrivance. Which radius it
// takes is chosen by MEASUREMENT, not by picking the minimum distance and
// hoping: every candidate landing is scored by the worst cut-or-fill its line
// would need against the natural hillside, and the cheapest wins. Landings
// inside (or within a portal's reach of) one of the tour's BORE GAPS are
// excluded outright — a slip road into the middle of a tunnel is not a road.
//
// Registered AFTER both tours, like the spawn connector and for the same
// reason: its end pins read their graded datums.
// ---------------------------------------------------------------------------
struct OuterConnectorResult {
    RoadBuildResult    road;
    RoadSpec           spec;
    std::vector<float> roadY;       // graded datum per node (the ribbon rides this)
    RoadJunction       ringJct;     // where it leaves the inner tour
    RoadJunction       outerJct;    // where it lands on the outer tour
    uint32_t           ringNode = 0, outerNode = 0;
    float gapBeforeM = 0.0f;        // measured inner->outer gap this closes
    float worstFitM  = 0.0f;        // worst cut-or-fill of the chosen line
};
OuterConnectorResult registerOuterConnector(const RoadSpec& ringSpec,
                                            const std::vector<float>& ringRoadY,
                                            const RoadSpec& outerSpec,
                                            const std::vector<float>& outerRoadY);

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

// ---------------------------------------------------------------------------
// THE RANGE CIRCUIT — Tim: "31 miles may be way too long. we need a 3-5 mile
// track around the range in addition." A lap-able closed course in the open
// country beside the spawn connector, and NOT a circle: a ~950 m start/finish
// straight, a second back straight, an S-curve complex, a hairpin, and
// whatever climb the country under it gives (measured and reported, never
// asserted blind). Connected to the network by a short ACCESS ROAD off the
// connector, with a junction mouth + junction box at BOTH ends — the same
// machinery the connector itself lands on the tour with. The circuit registers
// first (free grading), then the access pins its two ends to the connector's
// and the circuit's graded datums exactly.
// ---------------------------------------------------------------------------
struct RangeCircuitResult {
    bool               built = false;
    const char*        whyNot = "";
    RoadBuildResult    road;         // the circuit itself
    RoadSpec           spec;
    std::vector<float> roadY;
    RoadBuildResult    accessRoad;   // the connector -> circuit link
    RoadSpec           accessSpec;
    std::vector<float> accessRoadY;
    RoadJunction       connJct;      // access mouth onto the connector
    RoadJunction       circJct;      // access mouth onto the circuit
    uint32_t           hostNode = 0; // connector node the access launches from
    float              climbM = 0.0f;        // circuit datum max - min
    float              longestStraightM = 0.0f;
    float              hairpinTurnDeg = 0.0f; // sharpest 250 m heading change
};
RangeCircuitResult registerRangeCircuit(const RoadSpec& connSpec,
                                        const std::vector<float>& connRoadY,
                                        const TunnelRoute* keepClearOf,
                                        const std::vector<const RoadSpec*>* avoid = nullptr);

// --test-roadnetwork
bool runRoadNetworkSelfTest();

} // namespace x3::game
