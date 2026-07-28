#pragma once
// Club 1127 BEDROCK ENCASEMENT — "carve the club out of solid earth".
//
// Club 1127 ("THE DEEP") sits ~800 m underground (Y=-200), but it was authored as
// a HOLLOW BOX floating in the void: disable the sky and you were left with the
// engine's dark-slate HDR clear ("grayness") visible through the ceiling hole, and
// noclipping out of the room dropped you into empty black nothing. Tim's long-
// standing ask: the club must be a CAVITY CARVED OUT OF A SOLID BLOCK OF EARTH —
// noclip out in ANY direction and you are embedded in visible rocky dirt, never the
// void, never sky.
//
// This module builds that encasement as a NEW, self-contained unit (kept out of
// club1127.cpp so it doesn't collide with concurrent bar/interior edits). It is
// called ONCE from the club world host (app/world_hosts/host_club.cpp) right after
// Club1127World::build(), using the club's authored footprint (Stats) to size the
// cavity.
//
// THE MODEL — "solid earth minus cavities":
//   * A big solid EARTH volume surrounds the club on all six sides (cap above the
//     ceiling, base below the floor, and four thick walls beyond the room). The
//     club footprint is the CAVITY left empty inside it. Extend the earth many tens
//     of meters past the room in every direction, so noclipping out lands you deep
//     inside solid rock.
//   * The rock boxes are authored DOUBLE-SIDED (each face emitted with BOTH
//     windings) because the main mesh pipeline back-face-culls: without the inner
//     winding you would see rock only from OUTSIDE, and noclipping INTO the solid
//     would show black. Double-siding means from any point inside the earth you see
//     rock faces all around you — the acceptance test Tim runs.
//   * The rock carries a dim self-EMISSIVE floor so it reads as visible dark earth
//     even out where the club's own point lights don't reach (otherwise the void
//     would just be black again). Its albedo is a procedural mottled rock/dirt.
//   * NON-colliding: it is a purely visual shell. The club's own walls/floor own
//     collision; the earth must never block the player or fight the club bodies.
//
// EXTENSIBLE / CARVABLE (the tunnel-network foundation) — see buildClubBedrock()'s
// doc for the recipe to carve the NEXT cavity (a tunnel, the descent, the Complex)
// out of this same earth, and the CSG upgrade path.

#include "scene.h"
#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"

namespace x3::game {

// The earth is a MASSIVE solid block (the `earth*` outer AABB) with ONE empty
// CAVITY (the `cav*` AABB — the club plus breathing room) carved out of it. The
// block spans from below the club UP to the surface ground plane and WIDE enough
// to underlie the whole city above, so noclipping out of the club in any direction
// (including straight up toward the surface) stays buried in hundreds of meters of
// solid rock. All values world-meters; the host fills them from the club Stats
// footprint (cavity) + the city footprint / surface (earth block).
struct BedrockConfig {
    // Empty CAVITY bounds — the club's occupied volume plus a little clearance.
    float cavMinX = -30.0f, cavMaxX = 30.0f;
    float cavMinY = -206.0f, cavMaxY = -184.0f;
    float cavMinZ = -25.0f, cavMaxZ = 25.0f;

    // Outer EARTH block bounds — the solid mass the cavity is carved from. Defaults
    // reach up to the surface (Y=0) and out under a city-scale footprint; the host
    // overwrites them from the region graph. Must fully contain the cavity.
    float earthMinX = -1150.0f, earthMaxX = 750.0f;
    float earthMinY = -260.0f,  earthMaxY = 0.0f;      // below the club .. surface
    float earthMinZ = -525.0f,  earthMaxZ = 1375.0f;

    // Overlap (m) neighbouring slabs weld into each other by, so the six boxes read
    // as ONE solid mass with no coplanar-face z-fighting at the seams (the overlap
    // faces end up buried inside rock, never visible).
    float weld = 2.0f;

    // UV tiles per meter for the rock texture (small = coarse, chunky rock).
    float uvScale = 0.12f;

    // Seed the earth with ANCIENT SALVARI CRYSTAL hollows (glowing blue crystal-
    // people deposits): a handful of small rock pockets scattered through the
    // strata, each holding a cluster of emissive-blue crystal shards + a blue point
    // light that pools onto the pocket walls. Discover them by digging/noclipping
    // through the dead earth. Set false to build bare rock.
    bool  salvariCrystals = true;

    // Dark earth albedo TINT (multiplies the procedural rock texel).
    float tint[4] = { 0.42f, 0.30f, 0.22f, 1.0f };

    // Dim self-emissive floor { r, g, b, strength } so the earth is VISIBLE in the
    // unlit void (well under the bloom threshold — it must read as dark rock, not
    // glow). Tuned so noclip shots show brown rock, not black.
    float emissive[4] = { 0.085f, 0.065f, 0.048f, 1.0f };
};

// Build the solid-earth encasement around the club into `scene` (render meshes via
// `device`). It adds six large double-sided, non-colliding rock boxes (cap, base,
// four walls) that fill the `earth*` block everywhere OUTSIDE the `cav*` cavity —
// so the club is a small hollow near the bottom of a massive solid-earth body that
// reaches up to the surface. COARSE bulk (6 boxes) — the interior is just solid
// rock, so the triangle cost is tiny. Returns the number of Scene entities added.
//
// CARVING THE NEXT CAVITY (tunnel network — the recipe):
//   Today the earth is the six-slab shell around ONE cavity (the club). To dig a
//   tunnel or a new room out of this same earth WITHOUT a full CSG kernel:
//     1) Decide the new cavity's world AABB (the tunnel bore / room box).
//     2) Split whichever earth slab the tunnel passes through into pieces that go
//        AROUND the new cavity opening (leave the bore empty), exactly as the four
//        walls here go around the club cavity — i.e. add the same six-slab pattern
//        for the tunnel, sharing the earth where they meet.
//     3) Line the new cavity's own walls (a thin inner box) if it needs a distinct
//        surface, and connect its opening to the adjacent cavity (remove the shared
//        wall segment between them).
//   CSG UPGRADE PATH (when the network gets complex): author the earth as a single
//   mesh in Blender (a big block with the union of all cavities boolean-subtracted)
//   and load it as a static mesh; OR add a code-side AABB/BSP carve that takes the
//   earth AABB + a list of cavity AABBs and emits the boundary faces. Either way the
//   authoring stays "solid earth, cavities subtracted" — this module is that model's
//   first, single-cavity instance.
// `outCrystalLights` (optional): appended with the blue point light of each Salvari
// crystal hollow. The host must MERGE these into the light set it pushes every
// frame (Club1127World::update re-pushes only the club's own lights, so the host
// concatenates club.pointLights() + these before setPointLights). Empty when
// cfg.salvariCrystals is false.
int buildClubBedrock(Scene& scene, x3::rhi::IRenderDevice& device,
                     x3::phys::IPhysicsWorld& physics, const BedrockConfig& cfg,
                     std::vector<x3::rhi::PointLight>* outCrystalLights = nullptr);

// ============================================================================
// THE EARTH TUNNEL NETWORK (feat/earth-tunnels) — "a tunnel network to dig out".
// ============================================================================
// AUTHORED passages bored through the solid club_bedrock earth (the engine has no
// voxel/CSG, so the network is hand-authored coarse geometry — the model
// club_bedrock.h §"CARVING THE NEXT CAVITY" describes). This is the FIRST tunnel
// growth: a vertical STRATA DESCENT from near the surface (Y~=-3) down to the club
// floor (Y=-200) plus a few OFFSHOOT passages/chambers branching off it. Called
// ONCE from host_club.cpp right after buildClubBedrock(), so the descent is embedded
// in the same earth body (the earth is its surround; these tunnels are the cavities).
//
// WHAT IT BUILDS (all inside the earth block, east of the club):
//   * DESCENT SHAFT — a rock-walled, fully-enclosed vertical bore centered at
//     (shaftX, shaftZ) with a WALKABLE SWITCHBACK RAMP inside (colliding ramps +
//     turn landings zig-zagging up the bore, ~38 deg slope, well under the 50 deg
//     Jolt walk limit). This is the vertical spine the elevator/descent rides. The
//     bore is lined by 4 double-sided rock walls (framed around the openings) + a
//     top rock cap at the surface + a bottom floor at the club level.
//   * CONNECTOR — a short horizontal rock corridor at Y=-200 from the shaft bottom
//     WEST into the club's east elevator doorway (the club shell east wall is open
//     below 2.8 m at z~=3.8): walk out of the club, into the connector, up the shaft.
//   * OFFSHOOTS — 3 passages branching off descent landings through the shaft's +X
//     wall: two DEAD-END passages + one that opens into a small CHAMBER holding a
//     Salvari crystal hollow (the reward-to-find). The beginnings of the network.
//
// WALKABLE END TO END: every floor/ramp/landing/corridor floor carries Jolt
// collision (addStaticMesh); the rock walls/ceilings are the earth's own visual
// non-colliding shell. `outCrystalLights` is appended with the descent's dim mood
// lights + the offshoot crystal-hollow blue light (host merges them into the set it
// pushes each frame, distance-culled — same channel as buildClubBedrock's lights).
//
// AUTHOR THE *NEXT* TUNNEL (so the network can grow) — the recipe:
//   1) Pick where the new passage BRANCHES: a descent landing (use its world Y =
//      tc.bottomY + k*flightRise and its Z = south/north turn) or an existing
//      chamber wall.
//   2) Punch its MOUTH: add the new opening rect to the wall it exits through so the
//      framed liner leaves a doorway there (see wallXHoles() — a wall minus a list of
//      z/y hole rects, built as horizontal bands; add your rect to that list).
//   3) Lay the CORRIDOR as connected rock-walled segments following the path: a
//      colliding FLOOR box (top at the walk Y) + two visual side walls + a visual
//      ceiling per segment, welding segment ends by a small overlap (as the earth
//      slabs weld). Turns = a landing box where two segments meet.
//   4) Cap the far end (endcap wall) OR open it into a CHAMBER (a wider room: floor +
//      4 walls + ceiling, with short "front flank" walls framing the corridor mouth
//      where the room is wider than the passage).
//   5) Reward/hook: drop content in the chamber — e.g. addSalvariHollow() for a
//      crystal pocket (push its returned blue light into outCrystalLights), a prop,
//      or a trigger. Add a dim mood point light so the passage reads on camera.
//   Keep it COARSE (boxes + ramps); the earth is ~1k tris and the whole network
//   should stay a modest few thousand. CSG UPGRADE PATH: see buildClubBedrock's doc
//   (author the earth+all cavities as one boolean-subtracted Blender mesh when the
//   network outgrows hand-authored boxes).
struct TunnelConfig {
    // Rock look — matches the surrounding earth (tint * procedural rock texel + a
    // dim self-emissive floor so the bore reads as dark rock, not black void).
    float tint[4]     = { 0.42f, 0.30f, 0.22f, 1.0f };
    float emissive[4] = { 0.085f, 0.065f, 0.048f, 1.0f };
    float uvScale     = 0.14f;

    // Vertical span of the descent (world Y). topY ~ just under the surface ground
    // plane (Y=0); bottomY = the club floor level (the connector runs in at this Y).
    float topY    =   -3.0f;
    float bottomY = -200.0f;

    // Descent shaft center XZ — solid earth just EAST of the club; the switchback
    // walkway (and the elevator spine) live here.
    float shaftX = 38.0f;
    float shaftZ =  4.0f;

    // Club east elevator doorway the bottom connector walks into (x = club shell
    // east face, z = the doorway center). Defaults are the authored Club 1127 values.
    float clubDoorX = 15.24f;
    float clubDoorZ =  3.8f;

    // Seed a Salvari crystal hollow in one offshoot chamber (the reward-to-find).
    bool  crystalOffshoot = true;
};

// ============================================================================
// THE DESCENT FALL (feat/descent-fall) — replaces the walkable switchback ramp.
// ============================================================================
// buildEarthTunnels() now bores a VERTICAL FALL SHAFT instead of a walkable ramp:
// you DROP down an open chute through the strata and land in a DARK ROOM just above
// the club, where a computer terminal + a code-locked keypad door lead down a hall
// to an elevator that takes the final leg into Club 1127. The geometry (shaft, dark
// room, hall, elevator alcove, side-shoot rooms) is built here; the INTERACTIVE
// layer (the fall-catch, the terminal, the keypad door, the elevator ride) lives in
// descent_fall.{h,cpp} and reads the world positions from this layout struct.
struct DescentFallLayout {
    // FALL SHAFT (open vertical bore).
    float shaftX = 0, shaftZ = 0;   // bore center XZ
    float shaftHalfW = 4.0f;        // bore half-width (clear radius)
    float mouthY = -3.0f;           // trapdoor mouth Y (top of the fall)
    float catchTopY = 0;            // Y where the slowdown/catch volume begins (last ~10 m)
    // DARK LANDING ROOM (sealed, dark, just above the club).
    float roomCx = 0, roomCz = 0;   // room center XZ
    float roomFloorY = 0;           // landing floor Y
    float roomCeilY = 0;            // room ceiling Y
    float roomHalfX = 6.0f, roomHalfZ = 6.0f;
    // KEYPAD DOOR (opening in the room's -X wall) -> HALL.
    float doorX = 0, doorZ = 0, doorY = 0;   // door opening center (floor level)
    float doorHalfW = 1.1f;
    // ELEVATOR (the last leg down into the club).
    float elevX = 0, elevZ = 0;     // elevator shaft center
    float elevTopY = 0;             // top stop (hall floor level)
    float elevBotY = 0;             // bottom stop (club floor)
    // CLUB east doorway the elevator's bottom connector runs into.
    float clubDoorX = 0, clubDoorZ = 0;
};

// Build the earth tunnel network into `scene` (+ Jolt collision via `physics`, meshes
// via `device`). Returns the number of Scene entities added. `outCrystalLights` is
// appended with the descent mood lights + the offshoot crystal light (see above).
int buildEarthTunnels(Scene& scene, x3::rhi::IRenderDevice& device,
                      x3::phys::IPhysicsWorld& physics, const TunnelConfig& tc,
                      std::vector<x3::rhi::PointLight>* outCrystalLights = nullptr,
                      DescentFallLayout* outLayout = nullptr);

} // namespace x3::game
