#pragma once
// EFLZ Level 1 "The Spire" — vertical B1->F7 graybox stack
// (specs/EFLZ_SPIRE_7FLOOR.spec.md, supersedes the flat 6-room graybox).
//
// Game/slice code only — engine/ stays pure. Builds a VERTICAL stack of 8 floor
// plates (B1 basement security, F1 atrium, F2 medical wards, F3 labs, F4 offices,
// F5 synth bay, F6 executive, F7 rooftop) as boxes with distinct tints + static
// Jolt collision, connected by a central elevator shaft (a vertical column with a
// doorway opening on each floor) and an emergency stairwell. Floors are spaced
// 5 m apart in +Y so the elevator's 5 m stops land on walkable floor geometry at
// each floor (see specs/ELEVATOR.spec.md + main.cpp's elevator build).
//
// Per the conventions doc: +X right, +Y up, -Z forward. Floors stack along +Y.
// Each floor's plate shares the SAME XZ footprint so the elevator/stair shafts
// line up vertically through the whole tower.
//
// buildLevel1 returns the authored coordinates the host needs to place doors,
// pickups, enemies, trigger volumes and the elevator. The env-art GLB overlay
// (env_art.cpp) reads the SAME per-floor table (level1Rooms()) so the GLB
// floors/walls/ceilings/lights tile to every floor's exact bounds + base Y +
// ceiling height. Beats / doors / pickups / the elevator live in level1_game.cpp
// + main.cpp — this file is geometry only.

#include "scene.h"

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"

namespace x3::game {

// The 8 floors of the Spire, low -> high (B1 at the bottom, F7 the rooftop). Index
// shared by buildLevel1() (collision/graybox) and env_art.cpp (GLB tiling) so the
// floor footprints, base heights and ceiling heights are authored in ONE place.
enum class L1Floor : uint32_t {
    B1 = 0,  // Basement security — Jake's spawn + strength terminal + Martinez gate
    F1,      // Atrium / lobby — glass curtain-wall breather
    F2,      // Medical wards — 3 rescue rooms (Aria / Keisha / Emily)
    F3,      // Labs — research / hazards / crafting
    F4,      // Offices — cubicle combat sprawl
    F5,      // Synth bay — synth waves (tall high-bay)
    F6,      // Executive — exec suites (Sarah)
    F7,      // Rooftop — helipad / finale (tall)
    Count
};

// Floors that the elevator stops at (== L1Floor::Count == 8 stops, 5 m apart).
constexpr uint32_t kSpireFloorCount = (uint32_t)L1Floor::Count;
constexpr float    kFloorSpacing    = 5.0f;   // m between floor plates (elevator stop pitch)

// One floor's authored footprint + base height + ceiling height (meters). The
// plate spans x in [x0,x1], z in [-zHalf,+zHalf], its WALKABLE FLOOR at world
// y=y0, ceiling at y=y0+ceil. Returned by level1Rooms() so both the geometry
// builder and the art overlay tile to identical bounds / base / heights.
struct L1RoomDef {
    float x0, x1, zHalf, ceil, y0;
};

// The canonical Spire floor table (footprints + base heights + ceiling heights).
// Single source of truth shared by level1.cpp (collision/graybox) and env_art.cpp
// (GLB floor/wall/ceiling/light tiling). Index with L1Floor. Array length =
// (uint32_t)L1Floor::Count.
const L1RoomDef* level1Rooms();

// Authored Spire coordinates (meters), filled by buildLevel1(). Floor coords carry
// their world Y (y0 of the floor). The legacy door/room accessors map onto the
// bottom floors (B1/F1) so the existing Level-1 beats (cell -> corridor -> armory
// -> checkpoint -> arena -> elevator) keep working on the basement + atrium.
struct Level1Layout {
    // ---- Per-floor base Y (world floor height) + center, low -> high. The host
    // builds the elevator with one stop per entry (cab top at the floor) so a ride
    // arrives at walkable geometry on every floor. floorBaseY[B1]=0.
    float          floorBaseY[kSpireFloorCount] = {};
    x3::phys::Vec3 floorCenter[kSpireFloorCount] = {};  // (cx, y0, cz) per floor
    float          floorCeil[kSpireFloorCount]   = {};  // ceiling height per floor

    // ---- Central elevator shaft XZ (same on every floor) + the shaft footprint
    // half-extents. The cab rides this column; each floor has a doorway opening
    // from the shaft into the plate at elevatorDoor[floor].
    x3::phys::Vec3 elevatorCenter{};                     // (x, B1 floor y, z) of the shaft
    x3::phys::Vec3 elevatorHalf{};                        // (hx, topY, hz) of the shaft
    x3::phys::Vec3 elevatorDoor[kSpireFloorCount] = {};  // shaft->plate doorway per floor

    // Player spawn (feet) in B1.
    x3::phys::Vec3 spawn{};

    // ---- F2 medical wards: the 3 rescue-room centers (Aria / Keisha / Emily). The
    // rescue hub (app/rescue.*, future) places victims here. Y carries the F2 floor.
    x3::phys::Vec3 wardA{};   // Ward A — Aria
    x3::phys::Vec3 wardB{};   // Ward B — Keisha
    x3::phys::Vec3 wardC{};   // Ward C — Emily

    // ---- F6 executive: Sarah's holding office (4th rescue victim).
    x3::phys::Vec3 execOffice{};

    // ---- F7 rooftop: the final-boss arena center (helipad).
    x3::phys::Vec3 rooftopCenter{};

    // ====================================================================
    // LEGACY accessors (the existing §3 beats in level1_game.cpp). These map the
    // old 6-room single-floor names onto the new vertical stack so the current
    // Level-1 logic (doors A-E, pistol pickup, checkpoint guards, Martinez, the
    // win trigger) keeps building + running on B1/F1. cell/corridor/armory/
    // checkpoint = B1 sub-zones; arena = B1 boss zone; elevator = the shaft.
    // ====================================================================
    x3::phys::Vec3 doorA{};   // cell -> corridor (B1)
    x3::phys::Vec3 doorB{};   // corridor -> armory (B1)
    x3::phys::Vec3 doorC{};   // armory -> checkpoint (B1, locked until armed)
    x3::phys::Vec3 doorD{};   // checkpoint -> arena (B1, auto on arena trigger)
    x3::phys::Vec3 doorE{};   // arena -> elevator (B1, opens on Martinez death)

    x3::phys::Vec3 cellCenter{};
    x3::phys::Vec3 corridorCenter{};
    x3::phys::Vec3 armoryCenter{};
    x3::phys::Vec3 checkpointCenter{};
    x3::phys::Vec3 arenaCenter{};

    x3::phys::Vec3 cellHalf{};
    x3::phys::Vec3 corridorHalf{};
    x3::phys::Vec3 armoryHalf{};
    x3::phys::Vec3 checkpointHalf{};
    x3::phys::Vec3 arenaHalf{};

    float ceilCell       = 3.5f;
    float ceilCorridor   = 3.5f;
    float ceilArmory     = 3.5f;
    float ceilCheckpoint = 3.5f;
    float ceilArena      = 5.0f;
    float ceilElevator   = 5.0f;

    // The "medical equipment" prop entity id (strength-discovery target, beat 1):
    // hidden when the strength trigger fires. kNoLink if not built.
    uint32_t equipmentProp = kNoLink;
};

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

// Build the Spire graybox into `scene` (render meshes via `device`, static
// collision via `physics`) and return its authored coordinates. Call once on a
// fresh scene. Doors/pickups/enemies/triggers/elevator are added by the host using
// the returned layout. `artMask` suppresses graybox surface renders that real GLB
// art will cover (collision is always built); default mask = full graybox (legacy).
Level1Layout buildLevel1(Scene& scene,
                         x3::rhi::IRenderDevice& device,
                         x3::phys::IPhysicsWorld& physics,
                         const Level1ArtMask& artMask = {});

} // namespace x3::game
