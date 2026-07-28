#pragma once
// EFLZ Level 1 "The Spire" — vertical B1->F7 graybox stack
// (specs/EFLZ_SPIRE_7FLOOR.spec.md, supersedes the flat 6-room graybox).
//
// Game/slice code only — engine/ stays pure. Builds a VERTICAL stack of 8 floor
// plates (B1 basement security, F1 atrium, F2 medical wards, F3 labs, F4 offices,
// F5 synth bay, F6 executive, F7 rooftop) as boxes with distinct tints + static
// Jolt collision, connected by a central elevator shaft (a vertical column with a
// doorway opening on each floor) and an emergency stairwell. Floors stack in +Y at
// the REAL NON-UNIFORM canon pitch (docs/design/X3_WORLD_BLUEPRINT.md §2.1 — B1=0,
// F1=5, F2=10, F3=20, F4=30, F5=65, F6=78, F7=91 m; ~91 m, was an 8x-compressed
// uniform 5 m / 35 m stack). The elevator builds one stop per floor straight from
// floorBaseY[] (elevStops = floorBaseY[fi]+cabHY) so the stops auto-follow the real
// heights (see specs/ELEVATOR.spec.md + main.cpp's elevator build).
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

#include <cstdint>
#include <vector>

namespace x3::game {

// ---- THE FACILITY'S OWN AIR (fix/prim-point-light, 2026-07-12) --------------------
// The tower is a WINDOWLESS DETENTION BASEMENT. It has no sky, so it cannot have a
// sky's ambient: its environment is THE ROOM (a scene IBL probe), its ambient is a
// near-black NEUTRAL floor, and its light is the ceiling fixtures. Previously level1
// declared nothing and ran the engine defaults — ambient {0.42,0.44,0.50} AND a
// full-strength analytic-BLUE-SKY environment cube — which is why every graybox wall
// read B > G > R under a tungsten lamp. Shared with app_run so the ELEVATOR hands the
// same air back when you step out of the cab (it used to hand back the engine default
// and clobber this on frame one).
constexpr float kLevel1Ambient[3] = { 0.030f, 0.030f, 0.033f };
constexpr float kLevel1Ibl        = 0.5f;   // the standing SEAM-2 interior IBL value (now a SCENE probe: the room, not a sky)

// The 8 floors of the Spire, low -> high (B1 at the bottom, F7 the rooftop). Index
// shared by buildLevel1() (collision/graybox) and env_art.cpp (GLB tiling) so the
// floor footprints, base heights and ceiling heights are authored in ONE place.
// Canon Y (X3_WORLD_BLUEPRINT §2.1): B1=Detention(0), F1=Atrium(5), F2=Medical(10),
// F3=Genetics(20), F4=Cybernetics(30), F5=Drone(65), F6=Alien(78), F7=Executive(91).
enum class L1Floor : uint32_t {
    B1 = 0,  // Detention Level (canon "F1") — Jake's spawn + Martinez gate            [Y=0]
    F1,      // Atrium / lobby — glass curtain-wall breather                            [Y=5]
    F2,      // Medical Bay — 3 rescue rooms (Aria / Keisha / Emily); Dr. Chen          [Y=10]
    F3,      // Genetics Lab — gene-vats / nursery; Failed Experiment #7                [Y=20]
    F4,      // Cybernetics Workshop — augmentation; Humanity meter (-> F4.5 Nexus)     [Y=30]
    F5,      // Drone Manufacturing — the drone level; Swarm Controller + Sarah's hack  [Y=65]
    F6,      // Alien Technology Lab — Salvari first contact, the cure; Alien Overseer  [Y=78]
    F7,      // Executive Laboratory — finale, Sarah, timeline LOCK; Jake's Clone       [Y=91]
    Count
};

// Floors that the elevator stops at (== L1Floor::Count == 8 stops). The pitch is now
// NON-UNIFORM (real canon Y per floor in level1.cpp kFloors[]) — the elevator derives
// its stops from floorBaseY[] so the per-floor gaps (5/5/10/10/35/13/13 m) auto-follow.
constexpr uint32_t kSpireFloorCount = (uint32_t)L1Floor::Count;
// Nominal reference floor spacing (the OLD uniform pitch, retained as the sub-level
// stack's reference unit + a default step granularity). It is NO LONGER the inter-floor
// pitch of the main tower (that is now non-uniform, read from kFloors[].y0).
constexpr float    kFloorSpacing    = 5.0f;   // m — nominal reference unit (NOT the tower pitch)

// One floor's authored footprint + base height + ceiling height (meters). The
// plate spans x in [x0,x1], z in [-zHalf,+zHalf], its WALKABLE FLOOR at world
// y=y0, ceiling at y=y0+ceil. Returned by level1Rooms() so both the geometry
// builder and the art overlay tile to identical bounds / base / heights.
struct L1RoomDef {
    float x0, x1, zHalf, ceil, y0;
};

// ---- Cell trapdoor cutout (secret-room feature, app/secret_room.*) ----------
// A rectangular HOLE carved in the B1 plate floor under Jake's cell so the
// code-locked floor-hatch trapdoor actually drops the player into the secret room
// below. The hatch slab (built by SecretRoom) fills this hole when closed and
// slides aside to reveal it. SINGLE SOURCE OF TRUTH for the hole rect, shared by
// level1.cpp (which carves the B1 floor around it) and secret_room.cpp (which
// places the hatch + room here). Centered in XZ, half-extents kCellHatch*Half.
// Coordinates are world. Jake's Cell spans x[-3.5,3.5], z[-3,3]; the spawn is at
// (1.5,0) and the terminal at (3.0,-2.6), so the hatch is tucked into the cell's -X/+Z
// quadrant — clear of the spawn capsule and the terminal — at (-1.5, 1.8). It slides
// +X to open (toward x=1.2), staying inside the cell.
constexpr float kCellHatchCx     = -1.5f;
constexpr float kCellHatchCz     =  1.8f;
constexpr float kCellHatchHalf   =  0.9f;   // 1.8 m square opening

// The canonical Spire floor table (footprints + base heights + ceiling heights).
// Single source of truth shared by level1.cpp (collision/graybox) and env_art.cpp
// (GLB floor/wall/ceiling/light tiling). Index with L1Floor. Array length =
// (uint32_t)L1Floor::Count.
const L1RoomDef* level1Rooms();

// ---- F2-F7 WEST-WING IDENTITY ROOM table (the per-floor identity interiors partitioned
// into the western space of each plate). SINGLE SOURCE OF TRUTH shared by level1.cpp
// (buildLevel1 builds each room's collision graybox via roomBox) and wing_dressing.cpp
// (the WAVE-3 recipe art pass that dresses these rooms). Each entry: which floor, the
// room center (cx,cz) + half-extents (hw,hd) in world XZ, the doorway side
// ('W'=-X,'E'=+X,'S'=-Z,'N'=+Z; 0=sealed), and a canonical NAME that routes the dressing
// recipe (the name matches a room-dressing recipe branch — see wing_dressing.cpp). The
// room's floor Y + ceiling come from the shared kFloors table (level1Rooms()) for that
// floor. Built at the floor's y0 / ceil like the detention rooms.
struct L1WingRoom {
    L1Floor floor;
    float   cx, cz, hw, hd;
    char    door;
    const char* name;   // dressing recipe key (see wing_dressing.cpp classify)
};

// The F2-F7 wing room table + its count. Iterated by buildLevel1 (collision) and
// wing_dressing.cpp (art). The two stay in lockstep because both read THIS table.
const L1WingRoom* level1WingRooms(uint32_t& outCount);

// ---- SPIRE EMERGENCY STAIRWELL — the SHARED layout (QA upper-floors sweep, D19-D21).
// ONE source of truth for the B1->F7 stair, consumed by BOTH buildLevel1() (which turns
// it into meshes + Jolt bodies + the floor/lid cutouts) and the geometric lint
// (--test-levellint, SPIRE block). The lint therefore measures the boxes that ACTUALLY
// ship — it is not a mirror of the builder's arithmetic and cannot go blind when the
// layout changes (level_lint.cpp's opening comment explains why that matters).
struct SpireStair {
    struct Box { float x0, x1, z0, z1, y0, y1; };
    // One straight run between two level landings. `steps` carries discrete step
    // blocks (LAW 3 stairs); when `ramp` is true the run is a single wedge whose
    // slope must stay <= 30 deg (LAW 3 ramp) and `solid` is its bounding box.
    struct Flight {
        Box   solid{};
        float baseY = 0.0f, topY = 0.0f;
        uint32_t axis = 0;          // 0 = run along X, 1 = run along Z
        float dir  = 1.0f;          // +1 climbs toward +axis
        bool  ramp = false;
        std::vector<Box> steps;
    };
    std::vector<Flight> flights;
    std::vector<Box>    landings;    // level pads; top face (y1) is the walkable surface
    std::vector<Box>    soffits;     // closed undersides (a wedge has no bottom face)
    // Enclosure skin. Excluded from the lint's pierce/interpenetration probe on
    // purpose: like the elevator shaft's own walls (level1.cpp §2) a vertical shaft
    // skin runs the full height of the tower THROUGH every slab plane by design.
    std::vector<Box>    walls;
    // The stair WELL footprint. Every floor slab and ceiling lid is built with this
    // rectangle cut OUT of it, so the stair never passes through solid geometry.
    float wellX0 = 0.0f, wellX1 = 0.0f, wellZ0 = 0.0f, wellZ1 = 0.0f;
    float baseY = 0.0f, topY = 0.0f;
    float doorZ = 0.0f;              // stairwell doorway center Z on the -X enclosure face
    std::vector<float> arrivalY;     // walkable pad level at each served floor (B1..F7)
};

// The resolved stairwell layout (built once, cached). Pure arithmetic — no device.
const SpireStair& spireStair();

// ---- THE ART/GRAYBOX CONTRACT AT THE STAIR WELL (D19) ----------------------------
// env_art tiles GLB floor + ceiling panels across each WHOLE plate. Left alone it
// paints a solid-looking floor straight over the stairwell opening — art you can walk
// onto and fall through, because the collision slab is (correctly) cut. So the overlay
// must SKIP every tile the well touches, and the graybox must lay a visible APRON over
// exactly those skipped tiles minus the well, or the skip leaves a void ring.
// Both sides therefore agree on ONE rect: spireWellTileSpan().
constexpr float kSpireArtTileX = 4.0f;   // SM_Floor_A / SM_Ceiling_A footprint (m)
constexpr float kSpireArtTileZ = 3.0f;
// The tile-aligned opening on floor `floorIndex` (an L1Floor value): the union of the
// overlay tiles the well overlaps. x0/x1/z0/z1 are meaningful; y0/y1 are unused.
SpireStair::Box spireWellTileSpan(uint32_t floorIndex);

// The neutral hue-preserving lift buildLevel1 applies to the procedural deck map so
// the Spire's walkable surfaces land inside the interior reflectance band (D22).
const float* level1DeckMapLift();

// The central elevator shaft column footprint in world XZ (constant up the tower).
// Exported so the lint's keep-out probe reads the builder's own numbers.
void level1ShaftFootprint(float& x0, float& x1, float& z0, float& z1);

// The graybox surface tints buildLevel1 multiplies over the procedural maps, exported
// for the lint's reflectance-band probe (surface_library.h: real interior surfaces sit
// in [0.08, 0.40] LINEAR; D16 fixed the door leaves against the same law).
struct L1Surface {
    const char* name;
    float       tint[4];
    uint32_t    kind;      // 0 = floor deck, 1 = wall panel, 2 = ceiling panel
};
const L1Surface* level1Surfaces(uint32_t& outCount);

// ---- Floor-1 "Detention Level" room table (the authoritative Babylon LevelArchitect
// transcription — docs/design/SPIRE_LEVELARCHITECT_DIMS.md). One entry per room: a
// name + a TYPE flag + an axis-aligned box. Coordinates are transcribed DIRECTLY from
// the LevelArchitect (both RH, +X right / +Y up / -Z forward, meters): `cx,cz` is the
// room CENTER in XZ, `floorY` the room's walkable floor (relative to the B1 plate base,
// 0 for the ground rooms, negative for the descending cave arm), `w,d` the FULL extents
// (x,z), `h` the ceiling height. Half-extents are w/2, d/2 — NO axis flip. The
// detention complex is laid on the native B1 plate (Jake's spawn / the legacy beats).
struct L1DetentionRoom {
    const char* name;
    float cx, cz;     // room center in XZ (world, B1-plate-relative for Y)
    float floorY;     // room floor offset from the B1 plate base (0 = ground; <0 = caves)
    float w, h, d;    // FULL extents: width(x) x height(y) x depth(z)
    bool  monster;    // a "Cell (Monster)" / hazard room
    bool  npc;        // Sarah's empty cell (the npc flag)
};

// The 29-room Floor-1 detention table + its count, and the door connections (index
// pairs into the room table) — both transcribed from the LevelArchitect. Shared by
// buildLevel1() (interior graybox) and runLevel1SelfTest() (room/footprint asserts).
const L1DetentionRoom* level1DetentionRooms();
uint32_t               level1DetentionRoomCount();
const uint32_t*        level1DetentionDoors();     // flat pairs: [a0,b0, a1,b1, ...]
uint32_t               level1DetentionDoorPairCount();

// Named indices into the detention room table (for the legacy-accessor mapping + the
// self-test). These pin the rooms the existing Level-1 beats re-anchor onto.
enum L1DetRoom : uint32_t {
    kDetJakeCell      = 0,    // Jake's Cell (spawn)
    kDetMainHallway   = 4,    // Main Hallway (the spine)
    kDetGuardStation  = 5,
    kDetStorage       = 6,
    kDetMedicalBay    = 7,
    kDetArmory        = 8,    // the pistol pickup room
    kDetElevatorLobby = 9,    // the elevator stop / win room
    kDetSarahCell     = 18,   // Cell 10 (Sarah's — Empty)
    kDetDescStairs    = 25,
    kDetCaveTunnel    = 26,
    kDetCrystalCavern = 27,
    kDetSideGrotto    = 28,
};

// The Floor-1 detention CONTENT footprint (the bounding box of all 29 rooms, meters).
// ~75 (X) x ~43 (Z) per the authoritative dims — distinct from the raw B1 plate, which
// is a slightly larger superset. Filled by buildLevel1(); also computable standalone.
struct L1Footprint { float minX, maxX, minZ, maxZ; float width() const { return maxX - minX; } float depth() const { return maxZ - minZ; } };
L1Footprint level1DetentionFootprint();

// Authored Spire coordinates (meters), filled by buildLevel1(). Floor coords carry
// their world Y (y0 of the floor). The legacy door/room accessors map onto the
// bottom floors (B1/F1) so the existing Level-1 beats (cell -> corridor -> armory
// -> checkpoint -> arena -> elevator) keep working on the basement + atrium.
struct Level1Layout {
    // ---- Per-floor base Y (REAL non-uniform canon height) + center, low -> high. The
    // host builds the elevator with one stop per entry (cab top at the floor) so a ride
    // arrives at walkable geometry on every floor. floorBaseY[B1]=0 .. floorBaseY[F7]=91.
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
    bool ceilings = false;   // GLB ceiling panels cover the plate lids
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
