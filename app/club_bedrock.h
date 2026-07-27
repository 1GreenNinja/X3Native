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

} // namespace x3::game
