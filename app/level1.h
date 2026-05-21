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

    // Half-extents (x,z) of each room footprint (y is the 3 m wall height).
    x3::phys::Vec3 cellHalf{};
    x3::phys::Vec3 corridorHalf{};
    x3::phys::Vec3 armoryHalf{};
    x3::phys::Vec3 checkpointHalf{};
    x3::phys::Vec3 arenaHalf{};
    x3::phys::Vec3 elevatorHalf{};

    // The "medical equipment" prop entity id (strength-discovery target, beat 1):
    // hidden when the strength trigger fires. kNoLink if not built.
    uint32_t equipmentProp = kNoLink;
};

// Build the Level 1 graybox into `scene` (render meshes via `device`, static
// collision via `physics`) and return its authored coordinates. Call once on a
// fresh scene. The doors/pickups/enemies/triggers are added by the host using the
// returned layout.
Level1Layout buildLevel1(Scene& scene,
                         x3::rhi::IRenderDevice& device,
                         x3::phys::IPhysicsWorld& physics);

} // namespace x3::game
