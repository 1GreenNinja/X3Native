#pragma once
// ===========================================================================
// TUNNEL CORRIDOR — the VISIBLE half of the terrain-corridor primitive.
//
// app/terrain.h's TerrainCorridor lowers h(x,z) along a polyline. That is a
// NUMBER. This module turns it into something you can look at and drive
// through: it picks a real hill by SAMPLING the canonical height field, grades
// a road profile through it, registers ONE corridor whose depth profile is
// derived from that grade, and then lays a road ribbon + an arched tunnel tube
// + portal headwalls into the depression the corridor just made.
//
// THE SHAPE OF THE TRICK — REVISED (fix/tunnel-mouth). The original build had
// TWO depth regimes: cut to road level on the approaches, cut only to
// (road + tube + cover) under the hill so the TERRAIN was the tunnel's roof.
// That shipped a defect its own author flagged as irreducible: ~9 m of road per
// mouth buried under an earth ramp. It is worse than irreducible — it is
// UNFIXABLE in that form, and here is the proof:
//
//   Walk the road centreline. Where the road is open to the sky the ground is
//   AT road level. Where the terrain is the roof the ground is ABOVE the crown.
//   h(s) is continuous. By the intermediate value theorem it must pass through
//   every height in between — including the whole interval (road, crown), which
//   is the inside of the bore. So there is ALWAYS an s-interval where the
//   ground surface is inside the tunnel, on the roadway. Tightening the falloff
//   only shortens that interval; driving it to zero would need an infinite
//   gradient, and the limit case is a vertical WALL of earth across the bore,
//   which is worse, not better. A single-valued heightfield cannot roof a
//   drivable tunnel mouth. Full stop.
//
// So this module now builds the tunnel the way one is actually built:
//   * CUT-AND-COVER. The corridor is cut to ROAD LEVEL along its ENTIRE length
//     — under the ridge as well as on the approaches. h(x,z) is therefore never
//     above the road anywhere on the roadway, for any route, on any regenerated
//     terrain. The defect is gone BY CONSTRUCTION, not by tuning.
//   * THE BACKFILL LID. The hillside over the tube is put back as ONE swept
//     static mesh reconstructing the PRE-corridor surface, drawn with the
//     terrain splat marker so it shades identically to the ground and meets the
//     untouched terrain EXACTLY at the corridor's zero-delta boundary (the seam
//     is exact by construction — terrainCorridorDelta() is precisely 0 there).
//     This is the one deliberate, LOCAL break of the single-value rule: the
//     heightfield itself stays single-valued (streamer, collision soup, horizon
//     ring, placeOnTerrain, worldWaterLevelAt all untouched); one mesh, ~8 k
//     triangles, one draw call carries the overhang. That is the honest cost.
//   * THE PORTAL is then a real portal: a headwall standing proud of the
//     backfill with the arch cut through its spandrel, WINGWALLS tapering out
//     of it into the hillside until they die at the zero-delta seam, and a
//     projecting arch ring in front of the face.
//   * THE APPROACH CUTTING is part of the tunnel: retaining walls stand at the
//     toe of the cut batter on both shoulders, their tops following the natural
//     grade while the road stays flat, splaying outward into the headwall.
//     The ground is HELD BACK BESIDE the road instead of climbing across it.
//
// PROVENANCE (clean-room — docs/CLEANROOM_PROCESS.md). The TECHNIQUE was
// learned by studying the behaviour of the Babylon/BL predecessor world, as
// analysed in docs/design/BL_WORLD_PORT.md:
//   * §2.2 — BL pins the road to a flat datum and LOWERS THE TERRAIN onto it
//     rather than putting the road on piers. That inversion is the whole idea,
//     and it is what app/terrain.h's corridor primitive already implements.
//   * §2.3 / §3.2 — BL has two tunnel cross-sections: a swept semi-elliptical
//     arch (auto-tunnels) and a five-box rectangular shell (named tunnels). We
//     port the CROSS-SECTION IDEA (arch, springing off short vertical walls,
//     with a constant-thickness shell and an annular portal face) and none of
//     BL's placement, dimensions or code. BL builds its bore with
//     ExtrudeShape + a CSG boolean; we emit the annulus directly (§4.3a), which
//     needs no boolean and gives the portal ring for free as the end cap.
//   * §3.3 — the DRESSING intervals (light strips ~15 m, lane markings on a 5 m
//     grid, solid edge lines) are ported AS DATA, deliberately thinned.
//   * §4.4 — the LIGHT BUDGET. BL's four named tunnels would want 48 of the
//     engine's 64 forward point lights. We do NOT port the "a PointLight on
//     every 3rd strip" ratio: the strips are EMISSIVE GEOMETRY (free) and only
//     a handful of real point lights are spent. See kMaxBoreLights.
// No BL code was transcribed; the implementation is entirely our own.
//
// Everything here is app-layer: prims + Scene entities + addStaticMesh. No
// engine/ change, no renderer change.
// ===========================================================================

#include "scene.h"
#include "terrain.h"
#include "surface_library.h"   // real PBR bore-lining / portal sets (albedo+normal+mr)

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"

#include <cstdint>
#include <vector>

namespace x3::game {

// --- Cross-section + corridor constants (metres) ---------------------------
// WIDENED 2026-08-15 TO CARRY THE ROAD NETWORK. The bore was sized as a 2-lane
// motorway tube (39 ft roadway) and TUNNEL_INTERIOR_PLAN.md still records that
// figure as fact. Then app/road_network.h laid the ring at FOUR 12 ft lanes --
// a 48 ft running surface. The tunnel was therefore 9 ft NARROWER than the road
// feeding it: the outer lanes ran into the wall, and the lane markings the
// ribbon paints would have died at the portal.
//
// Nothing caught this because the two were authored days apart and neither
// owns the other's numbers. It is the same class of defect as the built-but-
// unwired features: each half correct, the SEAM never measured.
//
// WIDENED AGAIN 2026-08-17 (W-TUNNEL v2, SEVEN_LANE_PLAN lane 1). Tim: "Tunnels
// got the widening treatment too.. they are 4 lanes with low divider.. sidewalk
// off a concrete shoulder on each side." The 2026-08-15 pass fixed the SEAM (the
// bore now carried the road's 4 lanes) but kept the 2-lane FURNITURE: a 3 ft
// maintenance catwalk hard against the traffic lane and nothing between the two
// directions. That is a bore you drive through, not one you can stand in.
//
// Sized from the road outward, in feet, because that is the unit the road is
// specified in and the conversion belongs at one end or the other, not both:
//
//   centre divider    jersey F-shape, base half 1.0 ft   (half  1.0 ft)
//   running surface   4 lanes x 12.0 ft = 48.0 ft        (half 24.0 ft)
//   concrete shoulder 6.0 ft each side                   (half  6.0 ft)
//   raised sidewalk   6.0 ft each side                   (half  6.0 ft)
//   interior          1 + 24 + 6 + 6    = 37.0 ft half = 11.28 m (74.0 ft span)
//   shell             0.9 m                              (unchanged)
//   corridor floor    must clear BOTH the OUTER shell (12.18 m) and, outside
//                     the bore, the demo road's now-full-spec 20 ft apron
//                     (edge at 13.72 m) -> 14.0 m = 45.9 ft
//
// THE DIVIDER COSTS A FOOT PER SIDE. Four 12 ft lanes plus a 2 ft divider is a
// 50 ft running surface, not 48: kTcRoadHalfWidth goes 24.0 -> 25.0 ft. The
// feeding road_network route is 48 ft (kRunningHalfM), so the pavement flares
// 1 ft each side across the portal — absorbed by the shoulder/apron, which is
// 6 ft and 20 ft wide respectively. Stealing the foot from the two inner lanes
// instead would have made them 11 ft, and "4 lanes" would have been a lie in
// the one place a driver can measure it.
//
// The arch RISE keeps its old proportion (0.571 of the half-span) rather than
// its old absolute height: holding the crown over a span 37 % wider would have
// flattened the vault into a culvert. Rise 6.44 m over a 3.8 m wall puts the
// crown at 10.24 m = 33.6 ft. (Same rule, same reason, as the 2026-08-15 pass:
// 4.70 m of rise over 8.23 m of half-span was also 0.571.)
constexpr float kTcLaneW          = 3.658f; // ONE lane: 12.0 ft. The single source
                                            // for lane width; the paint reads it.
constexpr float kTcRoadHalfWidth  = 7.62f;  // pavement half-width: 25.0 ft (divider half + 4 x 12 ft)
constexpr float kTcTubeHalfWidth  = 11.28f; // interior half-width at the springing: 37.0 ft
constexpr float kTcTubeWallH      = 3.80f;  // vertical wall height before the arch: 12.5 ft
constexpr float kTcTubeCrownH     = 10.24f; // interior crown above the road datum: 33.6 ft
constexpr float kTcShellThick     = 0.9f;   // wall/crown thickness

// ---- THE LOW CENTRE DIVIDER -----------------------------------------------
// The SAME F-shape jersey profile road_network extrudes for its offside
// barriers and its freeway median (road_network.cpp `jp[4][2]`), at the same
// dimensions — one wall profile in the world, not a second one invented for
// the tunnel. Base half 0.30 m, crown half 0.115 m, height 0.81 m: low enough
// to see over (it is a sight-line, not a wall), tall enough to stop a
// crossover. It runs the ROOFED span only; outside the portals the demo road
// is undivided, the way the feeding route is.
constexpr float kTcDividerHalfW   = 0.305f; // base half-width: 1.0 ft
constexpr float kTcDividerH       = 0.81f;  // crown height (road_network jp[3][1])

// ---- THE CONCRETE SHOULDER -------------------------------------------------
// Between the outer lane's edge line and the sidewalk kerb. This is the
// "sidewalk OFF a concrete shoulder" of the brief: a broken-down car pulls onto
// it clear of the running lane, and the kerb beyond it is what the walk stands
// on. 6 ft is the tunnel refuge width; the demo road's apron outside the portal
// is the same surface at 20 ft (see kApronW in the .cpp — PAIRED).
constexpr float kTcShoulderW      = 1.829f; // 6.0 ft

// ---- RAISED SIDEWALKS (TUNNEL_INTERIOR_PLAN.md W1) ------------------------
// The raised walk each side, which is WHAT THE EXTRA WIDTH IS FOR. A bore with
// a bare wall at the lane edge reads as a pipe; a kerb, a deck and the shadow
// line under it read as infrastructure someone maintains. It is also the
// element every later fitting hangs off -- railings, SOS niches, door
// thresholds and the lay-by kerb all sit at deck level.
//
// WIDENED 3.0 -> 6.0 ft 2026-08-17: at 3 ft it was a maintenance catwalk you
// edge along, and the brief asks for a SIDEWALK. Six feet is two people
// abreast, and it is the width the door alcoves in tunnel_rooms.* open onto —
// a 2.1 m door discharging onto a 0.91 m ledge was the mismatch. The deck
// COLLIDES (see the walk upload in the .cpp): CONTACT LAW, you stand on it.
constexpr float kTcWalkKerbH      = 0.305f; // kerb height: 1.0 ft (a step, not a climb)
constexpr float kTcWalkDeckW      = 1.829f; // deck width:  6.0 ft
constexpr float kTcMinSoilCover   = 3.5f;   // ground kept above the tube's outer crown
// Corridor footprint. The flat floor must be wider than the tube's outer shell
// so the tube sits INSIDE the depression, never straddling its shoulder --
// AND, on the open approaches, wider than the demo road's paved edge so the
// apron never lands on uncut ground (road_network's halfWidth obeys the same
// rule). Binding constraint is now the apron: 7.62 + 6.10 = 13.72 m.
constexpr float kTcCorridorHalfW  = 14.0f;  // 45.9 ft -- clears shell 12.18 and apron 13.72

// THE SECTION ADDS UP, checked by the compiler rather than by a comment. This
// is the exact class of defect the 2026-08-15 pass shipped by hand (48 ft of
// road fed into a 39 ft bore, found days later): arithmetic in a block comment
// is a claim, a static_assert is a receipt. Anyone editing one number above now
// has to edit the others or the build stops.
static_assert(kTcRoadHalfWidth > kTcDividerHalfW + 2.0f * kTcLaneW - 0.005f &&
              kTcRoadHalfWidth < kTcDividerHalfW + 2.0f * kTcLaneW + 0.005f,
              "pavement half-width must be the divider half plus two 12 ft lanes");
static_assert(kTcTubeHalfWidth > kTcRoadHalfWidth + kTcShoulderW + kTcWalkDeckW - 0.005f &&
              kTcTubeHalfWidth < kTcRoadHalfWidth + kTcShoulderW + kTcWalkDeckW + 0.005f,
              "interior half-width must be pavement + shoulder + sidewalk exactly: "
              "the sidewalk deck lands ON the springing, with no sliver between");
static_assert(kTcCorridorHalfW > kTcTubeHalfWidth + kTcShellThick + 0.5f,
              "the corridor's flat floor must clear the tube's OUTER shell, or the "
              "tube straddles the cut's shoulder instead of sitting in it");
// The shoulder run. Under the old two-regime build this had to be kept tight
// because every metre of it was a metre of earth ramp inside the mouth. With
// cut-and-cover the depth profile no longer steps at the portal at all, so the
// falloff is now free to be a believable cut batter — and it is also the run
// over which the BACKFILL LID blends back to the untouched hillside. At
// halfWidth + falloff the corridor delta is exactly 0, which is what makes the
// lid's outer seam exact rather than tuned.
constexpr float kTcCorridorFall   = 14.0f;
// Road grading. kTcMinCut keeps the ribbon in a shallow groove even on open
// ground so it never fights the bumpy natural surface; kTcMaxGrade is the
// steepest the profile may climb (4.5 %, a real motorway limit).
constexpr float kTcMinCut         = 1.6f;
constexpr float kTcMaxGrade       = 0.045f;
// BL_WORLD_PORT.md §4.3b warned that "the hill above a tunnel gets a visible
// saddle". The old build answered that with kTcMaxScar, a cap on how much the
// bore reach could dig. Cut-and-cover answers it properly: the cut is BACKFILLED
// and the ridge is restored to its pre-corridor profile by the lid, so there is
// no saddle at all. kTcMaxScar is retired.
//
// ---- BACKFILL LID (the mesh that carries the overhang) --------------------
constexpr float kTcLidCover       = 1.20f;  // soil kept over the shell's outer crown
constexpr float kTcLidSink        = 0.55f;  // apron dives under the terrain at the seam
constexpr float kTcLidApron       = 5.0f;   // lid runs this far PAST the zero-delta edge
constexpr float kTcLidStep        = 3.0f;   // longitudinal sample spacing (m)
constexpr int   kTcLidLateral     = 45;     // lateral samples across the lid
// ---- PORTAL STRUCTURE ------------------------------------------------------
// 11.0 -> 14.1 with the 2026-08-17 widening: the headwall must still stand
// proud of the bore it frames. Outer shell is 12.18 m, so this keeps the same
// ~1.9 m of masonry each side the 8.23 m section had (11.0 over 9.13).
constexpr float kTcPortalHalfW    = 14.1f;  // headwall half-width (bore is 12.18 outer)
static_assert(kTcPortalHalfW > kTcTubeHalfWidth + kTcShellThick + 1.0f,
              "the headwall must stand proud of the arch it frames");
constexpr float kTcPortalThick    = 1.7f;   // headwall slab thickness along the road
constexpr float kTcPortalProud    = 0.75f;  // headwall parapet above the backfill
constexpr float kTcPortalSplay    = 6.5f;   // headwall -> wingwall taper run (m)
constexpr float kTcCanopy         = 3.0f;   // arch ring projects past the headwall face
// How far back from each mouth the backfill eases DOWN onto the cut face,
// outside the headwall's own width. Without it the lid arrives at the portal at
// full natural height right out to the seam, the wingwall has to retain all of
// it, and the wingwall runs out across the hillside as an isolated concrete fin
// standing 3 m proud of the ground (b.png, pass 4). Real backfill over a portal
// slopes down at the flanks onto the cut, and then the wingwall dies with it.
constexpr float kTcPortalTaper    = 15.0f;
// How far the roofed span may run out past the point where the natural hillside
// still gives full cover — the CUT-AND-COVER EXTENSION (a "false tunnel"), the
// thing that lets the mouth sit in daylight on a shallow bank instead of being
// dragged back into the hill.
constexpr float kTcPortalExtend   = 20.0f;
constexpr float kTcPortalMinBank  = 2.6f;   // stop extending once the bank is this low
// ---- APPROACH-CUTTING RETAINING WALLS --------------------------------------
constexpr float kTcWallBench      = 4.5f;   // nominal bench between wall and batter
// Where up the cut batter the wall's top is taken from, as a fraction of the
// falloff. A real cutting is retained for roughly the lower half of its height
// and battered in rock above that; sampling too close to the toe (the first
// attempt used the bench width) reads the batter at ~20 %% and no wall is ever
// tall enough to qualify, which is exactly what happened.
constexpr float kTcWallTopFrac    = 0.45f;
constexpr float kTcWallBenchMax   = 6.0f;  // widest catch berm behind a wall
constexpr int   kTcWallMinRun     = 3;     // stations; below this a wall floats
constexpr float kTcWallThick      = 0.55f;
constexpr float kTcWallMinH       = 1.4f;   // below this the cutting needs no wall
constexpr float kTcWallMaxH       = 9.0f;   // above this the batter carries the rest
constexpr float kTcWallSplay      = 16.0f;  // run over which the wall flares into the portal
// §4.4: keep at most a handful of REAL point lights in the bore. BL's own
// ratio (a PointLight on every 3rd 15 m strip) would be ~23 lights for a 350 m
// tunnel — 36 % of the engine's entire 64-light forward budget for one tunnel.
constexpr uint32_t kTcMaxBoreLights = 6;

// One sampled station along the corridor centreline.
struct TunnelStation {
    float x = 0.0f, z = 0.0f;    // world XZ on the spine
    float s = 0.0f;              // arc length from the first node (m)
    float ground = 0.0f;         // NATURAL height on the spine (pre-corridor)
    float latMin = 0.0f;         // min/max natural height across the corridor band
    float latMax = 0.0f;
    float roadY = 0.0f;          // graded road datum (the ribbon's top sits just above)
    float depth = 0.0f;          // what the corridor is asked to remove here
    bool  bore = false;          // design intent: this reach is enclosed
};

// The chosen route + everything derived from it. Filled by registerRoute().
struct TunnelRoute {
    std::vector<TunnelStation> st;
    // MEAN heading of the whole route (node 0 -> last node), unit XZ.
    //
    // This used to be THE heading: posAt/worldAt were `origin + dir * s`, which
    // made every route a straight run by construction. The stations always
    // carried their own x/z/s, so the polyline was already in the data — the
    // frame simply ignored it. It no longer does; see tangentAt().
    //
    // Kept because a route still has a useful overall bearing (spawn facing,
    // chase-camera rest yaw in host_tunnel.cpp) and because for a straight route
    // it IS the tangent everywhere, so nothing that reads it changes.
    float dirX = 1.0f, dirZ = 0.0f;
    float ox = 0.0f, oz = 0.0f;       // world XZ of node 0 (s = 0)
    // The route's OWN origin. posAt() used to read file-scope constants, which is
    // what pinned the module to exactly one tunnel; carrying them per route is
    // what lets several coexist.
    float cx = 0.0f, cz = 0.0f;       // route centre (world XZ)
    float halfLen = 0.0f;             // half the spine length (m)
    const char* name = "";
    float totalLen = 0.0f;
    // The ROOFED span [boreS0, boreS1] in arc length: shell + backfill lid +
    // a portal at each end. It is decided against the NATURAL (pre-corridor)
    // hillside — "is there a hill here to go under?" — and then extended
    // outward by the cut-and-cover extension, NOT against the post-corridor
    // field. With cut-and-cover the post-corridor field is flat by
    // construction, so it has nothing left to say about where a tunnel is.
    float boreS0 = 0.0f, boreS1 = 0.0f;
    // The sub-span where the NATURAL hillside alone already gives full cover —
    // i.e. what would have been bored rather than cut-and-covered. Reported.
    float coverS0 = 0.0f, coverS1 = 0.0f;
    // Total length of road anywhere on the route that has ground above it.
    // Under cut-and-cover this is 0 by construction and the boot log says so;
    // it is kept because it is the number this whole lane exists to drive to 0,
    // and --test-tunnelmouth asserts it.
    float buriedRoadLen = 0.0f;
    float maxRoadBury   = 0.0f;   // worst ground-above-road on the roadway (m)
    bool  boreValid = false;

    // Interpolated queries (clamped to the ends).
    float roadYAt(float s) const;
    void  posAt(float s, float out[3]) const;   // {x, roadY, z}
    // (s, lateral offset) -> world XZ. `lat` is metres right of the direction
    // of travel, using the LOCAL tangent at s — so the frame follows the
    // polyline instead of being constant, and a lateral offset stays
    // perpendicular to the road through a bend.
    void  worldAt(float s, float lat, float& outX, float& outZ) const;

    // Unit XZ tangent at arc length s, from the station polyline. Clamped to
    // the end segments, so extrapolating past either end runs straight out
    // along that end's heading — which is what the approach cuttings rely on.
    // For a straight route this returns dirX/dirZ at every s.
    void  tangentAt(float s, float& outTx, float& outTz) const;

    // Which polyline segment arc-length s falls in: outI is the END node index
    // (>= 1), outT the 0..1 parameter along that segment. Extrapolates on the
    // end segments rather than clamping, so the approach cuttings keep running
    // straight out past the last node.
    void  segmentAt(float s, uint32_t& outI, float& outT) const;
};

// The PRE-corridor surface at world (x,z). terrainHeightAtWorld() applies the
// registered corridors last, and terrainCorridorDelta() is exactly the amount
// they removed, so subtracting it recovers the natural hillside — before OR
// after registration, with no second height field to keep in sync.
float tunnelNaturalHeightAt(float x, float z);

// The BACKFILL LID surface at (s, lat) — the reconstructed hillside over the
// tube. Public because both the builder and --test-tunnelmouth need the exact
// same function: the test asserts the lid clears the shell and lands on the
// terrain at the seam, and it can only do that if it asks the real thing.
float tunnelLidHeightAt(const TunnelRoute& route, float s, float lat);

// BOOT ENTRY POINT. Samples the canonical height field, grades the road,
// registers exactly ONE TerrainCorridor, then re-samples the FINAL field to
// find the genuinely enclosed span. MUST be called before the first
// TerrainStreamer::init() / horizon-ring build, per app/terrain.h's registry
// contract ("register at BOOT, before the first height query"). Idempotent:
// the second call returns the cached route without touching the registry.
// WHERE a tunnel goes. One per bore; the module used to hard-code exactly one.
struct TunnelSpec {
    const char* name = "tunnel";
    float cx = 0.0f, cz = 0.0f;      // route CENTRE (world XZ) — the hill to bore
    float dirX = 1.0f, dirZ = 0.0f;  // unit XZ heading (normalized on entry)
    float halfLen = 320.0f;          // half the spine: bore + approach cuttings
};

// BOOT ENTRY POINT, one call per tunnel. Samples the canonical height field,
// grades the road, registers ONE TerrainCorridor for this spec, then re-samples
// the FINAL field to find the genuinely enclosed span. MUST be called before the
// first TerrainStreamer::init() / horizon-ring build, per app/terrain.h's
// registry contract ("register at BOOT, before the first height query").
//
// Returns nullptr when the corridor registry is full (kMaxTerrainCorridors) or
// the route is degenerate. A NON-null route with boreValid == false is a real
// answer, not a failure: it means the terrain along that heading has no hill to
// bore, and the caller should NOT dress a tunnel there. Callers must check.
//
// Routes accumulate and are owned by the module; the returned pointer is stable.
const TunnelRoute* registerTunnelCorridorFor(const TunnelSpec& spec);

// How many routes have been registered, and indexed access to them.
uint32_t          tunnelRouteCount();
const TunnelRoute* tunnelRouteAt(uint32_t i);

// The original single-tunnel demo route (--world tunnel), unchanged: the same
// authored hill, the same constants. Idempotent — the second call returns the
// cached route without touching the registry.
const TunnelRoute& registerTunnelCorridor();

// The demo route's spec (the authored hill at (-592,-352), heading 157.5 deg).
// Exposed so the drive-through self-test registers its route through the SAME
// door the city's freeway bores use — the cached entry point above can't, by
// design.
TunnelSpec demoTunnelSpec();

// DRIVE-THROUGH self-test (--test-tunneldrive). The acceptance for this module
// is not a screenshot — it is that a vehicle can drive in one portal and out
// the other: the car must reach the far portal, staying at road level through
// the bore — through it, not over it. Streams the real terrain (headless
// device), builds the real road/shell collision, drives the real Jolt wheeled
// rig. No window/Vulkan. The NEGATIVE CONTROL is field-level (N1): the natural
// hillside must bury a long reach of the roadway, or the drive-through proves
// nothing. (The old two-phase X3_TUNNEL_PORTAL_CUT control asserted a code
// path the cut-and-cover redesign deleted — see the note above
// driveTheDemoRoute in the .cpp for which gates died and why.)
bool runTunnelDriveSelfTest();

// --test-routeframe — P1's gate: TunnelRoute's frame follows its station
// polyline. Proves BOTH that a straight route is unchanged by the change (every
// existing tunnel gate depends on that) and that a curved route actually
// follows its arc with a perpendicular frame. Pure maths — no device, no
// physics, no assets.
bool runRouteFrameSelfTest();

// Release the SHARED tunnel surface sets. Call ONCE, after the last
// TunnelCorridorWorld has shut down — a single bore's shutdown must not free
// textures its neighbours are still drawing with. Safe to call with no tunnels
// ever built.
void shutdownTunnelSurfaces(x3::rhi::IRenderDevice& device);

// ---------------------------------------------------------------------------
// THE MERGED TUNNEL LIGHT POOL.
//
// A dressed bore spends EIGHT real point lights — kTcMaxBoreLights (6) down the
// barrel plus one at each mouth. That was sized for ONE showcase tunnel. It does
// not survive multiplication:
//
//     4 city freeway bores  = 32 lights  (CITY_BORES_PLAN B1's cap, exactly,
//                                         with zero headroom for street lighting)
//     8 network bores       = 64 lights  (the ENTIRE legacy pooled budget)
//
// and the ring roads bore through four mountain ranges, so eight is the plan,
// not the worst case.
//
// The fix is not fewer lights per bore — it is to stop pretending every bore in
// the world needs to be lit at once. You can only be inside one tunnel. Each
// frame, merge every live bore's lights, keep the nearest K to the camera, and
// upload those. Cost becomes O(K) regardless of how many tunnels exist.
//
// This also cures a SECOND defect on its own: the host used to call
// setPointLights ONCE at build time, so anything else that touched the light
// array left the bore black for the rest of the run (the "lit in headless
// capture, black when driven" bug). A per-frame upload cannot go stale.
constexpr uint32_t kMaxTunnelLightsInFlight = 16;

// Upload the nearest tunnel lights to `camPos`, merged across every built
// TunnelCorridorWorld. Returns how many were uploaded. Call once per frame,
// AFTER any other system that sets point lights. With no tunnels built it
// uploads nothing and is free.
//
// `extra`/`extraCount` (weapons task): TRANSIENT lights merged in FRONT of the
// pooled tunnel lights — muzzle-flash and grenade-detonation pulses. They ride
// the same single setPointLights upload (a second call would overwrite the
// pool), are never distance-culled (a flash is by definition next to the
// camera's subject), and cost nothing when the count is 0.
uint32_t uploadTunnelLights(x3::rhi::IRenderDevice& device, const float camPos[3],
                            const x3::rhi::PointLight* extra = nullptr,
                            uint32_t extraCount = 0);

// A built bore joins the pool; shutdown() leaves it. Called by
// TunnelCorridorWorld itself — callers do not need these.
class TunnelCorridorWorld;
void registerTunnelLightSource(const TunnelCorridorWorld* t);
void unregisterTunnelLightSource(const TunnelCorridorWorld* t);

// The built world: road ribbon, tunnel shell, portals, markings, lights.
class TunnelCorridorWorld {
public:
    // `route` must come from registerTunnelCorridor().
    // `groundTex` MUST be the terrain splat MARKER (TerrainStreamer::
    // groundTexture() / makeTerrainSplatMarker()). The renderer flags any draw
    // that uses that handle as terrain and shades it through the same
    // height/slope splat as the streamed tiles (vk_passes.cpp m_terrainMarkerId)
    // — which is the ONLY reason the backfill lid is invisible as a distinct
    // object. Pass an invalid handle and the lid falls back to a procedural
    // earth tone, which reads as a tarpaulin over the hill; the build warns.
    bool build(Scene& scene, x3::rhi::IRenderDevice& device,
               x3::phys::IPhysicsWorld& physics, const TunnelRoute& route,
               x3::rhi::TextureHandle groundTex = {});

    void shutdown(x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics);

    const std::vector<x3::rhi::PointLight>& lights() const { return m_lights; }
    uint32_t entityCount() const { return m_entities; }

    // Camera poses for the proof shots: {x,y,z,yaw,pitch}. `which`:
    //   0 approach (on the road, outside the entrance portal, looking in)
    //   1 inside   (mid-bore)
    //   2 far mouth (outside the exit portal, looking BACK at it)
    //   3 saddle   (above the ridge — under cut-and-cover it should read as an
    //               INTACT hill, which is the whole point of the backfill lid)
    //   4 portal detail (three-quarter close-up of the concrete/terrain seam)
    //   5 mouth head-on, close (the headwall + wingwalls + the arch ring)
    //   6 inside looking OUT at the entrance mouth (the dark-to-light frame —
    //     this is the shot the old build could not survive: it is where the
    //     earth ramp filled the arch)
    //   7 far-mouth three-quarter (the exit portal from the side, on the bank)
    //   8 THE GARAGE — inside the Late Night Speed bay, from the entry end,
    //     looking down the lifts / checker floor / neon (falls back to shot 1
    //     on a bore with no garage program)
    void showcaseCamera(const TunnelRoute& route, int which, float cam[5]) const;
    static constexpr int kShowcaseShots = 9;

private:
    std::vector<x3::rhi::MeshHandle>    m_meshes;
    std::vector<x3::rhi::TextureHandle> m_textures;
    std::vector<x3::phys::BodyId>       m_bodies;
    std::vector<x3::rhi::PointLight>    m_lights;
    // REAL ART. The bore lining and the portal headwalls are dressed from
    // assets/surface_library sets (albedo + normal + mr), not the procedural
    // checkers this demo booted on — see build() for which sets and why. The
    // LIBRARY owns those textures; m_textures holds only the ones this class
    // created itself, so shutdown() has to release both.
    // NOTE: the surface sets are NOT owned per tunnel any more — see
    // tunnelSurfaces() in the .cpp. Every dressed bore used to mount its own
    // SurfaceLibrary and decode the same 2K albedo/normal/mr sets again, which
    // was fine for one showcase bore and is not fine for eight.
    uint32_t m_entities = 0;
    // Camera pose for showcase shot 8 (the LNS garage interior), captured while
    // the bay's geometry is being emitted — the bay's placement is decided by
    // the rooms program (the door with the most rock), so build() is the only
    // party that knows where it ended up. Invalid on Tier B/C bores.
    bool  m_garageCamValid = false;
    float m_garageCam[5] = { 0, 0, 0, 0, 0 };
};

// ---------------------------------------------------------------------------
// --test-tunnelmouth — THE DEFECT GATE. Headless, no device, no physics: it
// registers the corridor and interrogates the real height field.
//   M0  the corridor registers and a roofed span is found
//   M1  THE DEFECT: sample terrainHeightAtWorld across the FULL road width at
//       0.5 m for the whole route — the ground is never above the road datum.
//       This is the assertion the old build fails by ~18 m of road.
//   M2  route.buriedRoadLen == 0 and the boot log's number agrees with M1
//   M3  the backfill lid clears the shell's outer skin by >= kTcLidCover*0.8
//       everywhere over the roofed span (the lid never cuts into the bore)
//   M4  the lid lands EXACTLY on the untouched terrain at the zero-delta seam
//       (|lid - terrain| tiny at halfWidth+falloff) and dives under it beyond
//   M5  the road grade never exceeds kTcMaxGrade and the profile is smooth
//   M6  REGENERATION PROOF: re-derive the whole route against three perturbed
//       height fields (shifted route centres) and assert M1 still holds — the
//       fix must be a property of the construction, not of this one hillside.
// ---------------------------------------------------------------------------
bool runTunnelMouthSelfTest();

} // namespace x3::game
