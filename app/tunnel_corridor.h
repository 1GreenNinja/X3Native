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
// Road-scale, not BL's cavernous 72x34 auto-tunnels (BL_WORLD_PORT.md §2.3
// notes those are sized to swallow a hill, not to carry a road). These are in
// the range of BL's NAMED tunnels (14-16 wide, 8-9 tall) and of a real 2-lane
// motorway bore.
constexpr float kTcRoadHalfWidth  = 6.0f;   // drivable ribbon half-width
constexpr float kTcTubeHalfWidth  = 7.0f;   // interior half-width at the springing
constexpr float kTcTubeWallH      = 3.6f;   // vertical wall height before the arch
constexpr float kTcTubeCrownH     = 7.6f;   // interior crown height above the road datum
constexpr float kTcShellThick     = 0.9f;   // wall/crown thickness
constexpr float kTcMinSoilCover   = 3.5f;   // ground kept above the tube's outer crown
// Corridor footprint. The flat floor must be wider than the tube's outer shell
// so the tube sits INSIDE the depression, never straddling its shoulder.
constexpr float kTcCorridorHalfW  = 8.8f;
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
constexpr float kTcPortalHalfW    = 11.0f;  // headwall half-width (bore is 7.9 outer)
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
    float dirX = 1.0f, dirZ = 0.0f;   // unit XZ heading (constant — a straight run)
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
    // of travel. The corridor is a straight run, so the frame is constant.
    void  worldAt(float s, float lat, float& outX, float& outZ) const;
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
// Exposed so the drive-through self-test can register the SAME route more than
// once against different portal-cut settings — the cached entry point above
// can't, by design.
TunnelSpec demoTunnelSpec();

// DRIVE-THROUGH self-test (--test-tunneldrive). The acceptance for this module
// is not a screenshot — it is that a vehicle can drive in one portal and out
// the other. Registers the demo route TWICE: once with the portal cut disabled
// (X3_TUNNEL_PORTAL_CUT=0 — the NEGATIVE CONTROL, where the earth ramp at the
// mouth must STOP the car) and once enabled (the car must reach the far
// portal, staying at road level through the bore — through it, not over it).
// Streams the real terrain (headless device), builds the real road/shell
// collision, drives the real Jolt wheeled rig. No window/Vulkan.
bool runTunnelDriveSelfTest();

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
    void showcaseCamera(const TunnelRoute& route, int which, float cam[5]) const;
    static constexpr int kShowcaseShots = 8;

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
    SurfaceLibrary                      m_surf;
    uint32_t m_entities = 0;
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
