#pragma once
// EFLZ Level 1 "Awakening" graybox layout (§2 of specs/EFLZ_LEVEL_01.spec.md).
//
// Game/slice code only — engine/ stays pure. Builds the six connected graybox
// rooms (cell -> corridor -> armory -> checkpoint -> arena -> elevator) as boxes
// with distinct tints + static Jolt collision, plus the doorway gaps the
// DoorSystem doors fill. Geometry follows the mesh_prims box pattern used by
// buildTestLevel; rooms are laid out along +X so the player walks the spine in
// one direction.
//
// buildLevel1 returns the authored coordinates (spawn, doorway centers, room
// centers) the host needs to place doors, pickups, enemies, and trigger volumes.
// Doors A-E, the pistol pickup, enemy placements and the beat sequence live in
// app/main.cpp (the host wires the spec's §3 beats) — this file is geometry only.

#include "scene.h"

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"

namespace x3::game {

// Authored Level 1 coordinates (meters), filled by buildLevel1(). All Y values
// are at floor level (y=0) unless noted. Doorway centers are at the floor; doors
// slide up from there. Room centers are the box centers used to place contents.
struct Level1Layout {
    // Player spawn (feet) in the cell.
    x3::phys::Vec3 spawn{};

    // Doorway centers (floor level) for the five doors A-E.
    x3::phys::Vec3 doorA{};   // cell -> corridor
    x3::phys::Vec3 doorB{};   // corridor -> armory
    x3::phys::Vec3 doorC{};   // armory -> checkpoint (locked until armed)
    x3::phys::Vec3 doorD{};   // checkpoint -> arena (auto on arena trigger)
    x3::phys::Vec3 doorE{};   // arena -> elevator (opens on Martinez death)

    // Room floor centers (for placing pickups / enemies / triggers).
    x3::phys::Vec3 cellCenter{};
    x3::phys::Vec3 corridorCenter{};
    x3::phys::Vec3 armoryCenter{};
    x3::phys::Vec3 checkpointCenter{};
    x3::phys::Vec3 arenaCenter{};
    x3::phys::Vec3 elevatorCenter{};

    // Half-extents (x,z) of each room footprint (the y component carries the
    // room's full ceiling height — see ceil* below — so triggers/HUD can read it).
    x3::phys::Vec3 cellHalf{};
    x3::phys::Vec3 corridorHalf{};
    x3::phys::Vec3 armoryHalf{};
    x3::phys::Vec3 checkpointHalf{};
    x3::phys::Vec3 arenaHalf{};
    x3::phys::Vec3 elevatorHalf{};

    // ---- Per-room CEILING HEIGHT (meters, floor at y=0). D-content raises the
    // ceilings so the spaces feel grander — varied, NOT uniform: corridors are
    // moderately taller; the arena + elevator shaft are notably tall (a real
    // boss-room feel). buildLevel1() builds the walls (stacked GLB-friendly) +
    // ceiling caps to these heights, and EnvArtSystem reads the SAME values to
    // place its GLB ceiling tiles + Light_A fixtures (and their point lights) at
    // the matching height per room — so geometry + art + lights stay in lockstep
    // with no duplicated magic constant. Order = the room spine.
    float ceilCell       = 3.0f;
    float ceilCorridor   = 3.0f;
    float ceilArmory     = 3.0f;
    float ceilCheckpoint = 3.0f;
    float ceilArena      = 3.0f;
    float ceilElevator   = 3.0f;

    // The "medical equipment" prop entity id (strength-discovery target, beat 1):
    // hidden when the strength trigger fires. kNoLink if not built.
    uint32_t equipmentProp = kNoLink;
};

// The six Level 1 rooms in spine order. EnvArtSystem and buildLevel1 share this
// so the room footprints + ceiling heights are authored in exactly ONE place.
enum class L1Room : uint32_t {
    Cell = 0, Corridor, Armory, Checkpoint, Arena, Elevator, Count
};

// One room's authored footprint + ceiling height (meters). x in [x0,x1], z in
// [-zHalf,+zHalf], floor at y=0, ceiling at y=ceil. Returned by level1Rooms() so
// both the geometry builder and the art overlay tile to identical bounds/heights.
struct L1RoomDef {
    float x0, x1, zHalf, ceil;
};

// The canonical Level 1 room table (footprints + raised ceiling heights). Single
// source of truth shared by level1.cpp (collision/graybox) and env_art.cpp (GLB
// floor/wall/ceiling/light tiling). Index with L1Room.
const L1RoomDef* level1Rooms();   // -> array of (uint32_t)L1Room::Count entries

// Art-overlay mask (EFLZ art pass). When the converted sci-fi GLBs load, the real
// art is drawn OVER the graybox collision (by EnvArtSystem), so the corresponding
// graybox SURFACE render meshes are suppressed (collision is always kept). Each
// flag = true means "build that surface collision-only (invisible)". A flag left
// false keeps the original visible graybox for that surface (per-piece fallback:
// if a GLB failed to load, its graybox stays visible and the level never breaks).
struct Level1ArtMask {
    bool walls    = false;   // hide graybox side walls + cross walls render
    bool floors   = false;   // hide graybox floor render
};

// Build the Level 1 graybox into `scene` (render meshes via `device`, static
// collision via `physics`) and return its authored coordinates. Call once on a
// fresh scene. The doors/pickups/enemies/triggers are added by the host using the
// returned layout. `artMask` suppresses graybox surface renders that real GLB art
// will cover (collision is always built); default mask = full graybox (legacy).
Level1Layout buildLevel1(Scene& scene,
                         x3::rhi::IRenderDevice& device,
                         x3::phys::IPhysicsWorld& physics,
                         const Level1ArtMask& artMask = {});

} // namespace x3::game
