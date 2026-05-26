#pragma once
// EFLZ DATA-DRIVEN level loader — builds a floor's geometry from the canonical
// LevelArchitect v2.project.json + a per-room portal PVS for occlusion culling.
//
// Game/slice code only — engine/ stays pure. This is the NEW canonical path that
// replaces the hand-coded level1.cpp kDetention table with the owner's authoritative
// facility data. The loader:
//   1. Parses ONE floor (rooms[] + doors[]) from the v2 project JSON.
//   2. Builds each room's SHELL (floor + ceiling + 4 walls) as graybox COLLISION +
//      a render fallback, mirroring level1.cpp's addBox/addWall helpers. Every built
//      entity is tagged with its ROOM ID (Scene::Entity::roomId).
//   3. Runs a DOORWAY RESOLVER over the door list: adjacent rooms (shared wall) get a
//      cut doorway; ~1 m gaps are treated as wall thickness (doorway through the gap);
//      ~2 m gaps get a SHORT connecting corridor; overlaps are nudged; cross-level /
//      isolated rooms (Cave System / Hidden Sub-Level at y=-174/-178) are linked
//      VERTICALLY via a descent tube. The level ends up fully navigable.
//   4. Optionally drapes the env_art GLB overlay where it fits (caller wires that).
//   5. Builds a PORTAL PVS: each room's directly-doored neighbour set, so the host can
//      ask "which rooms are visible from room R" each frame and feed Scene::setVisibleRooms.
//
// COORDINATES: the JSON is already in engine convention (docs/CONVENTIONS.md: +X right,
// +Y up, -Z forward; 1 unit = 1 m). Room x/y/z = CENTER, w/h/d = FULL extents. NO axis
// flip (verified against the source). y carries the room floor base (0 for the main
// floor; deeply negative for the cave / sub-level rooms).
//
// FALLBACK: level1.cpp's kDetention path is UNCHANGED and stays the default build. This
// loader is reached via --world canonlevel / --test-canonlevel; if the JSON is absent the
// caller falls back to the legacy build (the loader reports load failure cleanly).

#include "scene.h"
#include "door.h"        // DoorSystem + buildLevelDoor (SM_Door_A GLB doors at openings)

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace x3::game {

// One parsed room (a node in the floor graph). Coordinates are world meters, engine
// convention. center = (x,y,z) is the room CENTER; (w,h,d) are FULL extents. The
// FLOOR of the room is at y = center.y - h/2 (a room with center.y=0, h=4 sits with
// its floor at y=-2 .. ceiling y=+2). roomId == the room's index in the rooms vector.
struct CanonRoom {
    std::string name;
    std::string type;       // "Cell" / "Hallway" / "Boss Arena" / "Cave" / ...
    float cx = 0, cy = 0, cz = 0;   // center (world m)
    float w = 0, h = 0, d = 0;      // FULL extents (x, y, z) (world m)

    float x0() const { return cx - w * 0.5f; }
    float x1() const { return cx + w * 0.5f; }
    float y0() const { return cy - h * 0.5f; }   // floor
    float y1() const { return cy + h * 0.5f; }   // ceiling
    float z0() const { return cz - d * 0.5f; }
    float z1() const { return cz + d * 0.5f; }
};

// How a door pair was RESOLVED by the doorway resolver (diagnostics + the self-test).
enum class DoorwayKind : uint32_t {
    None        = 0,   // could not be resolved (should not happen post-resolve)
    AdjacentX   = 1,   // rooms share a wall whose plane is X=const -> doorway cut there
    AdjacentZ   = 2,   // rooms share a wall whose plane is Z=const -> doorway cut there
    GapBridge   = 3,   // ~1-2.5 m gap -> a short connecting corridor segment bridges it
    Overlap     = 4,   // rooms interpenetrate -> opening cut in the overlap (no bridge)
    CrossLevel  = 5,   // big Y delta (>3 m) -> a vertical descent tube links them
};

// One resolved doorway between two rooms (indices a,b into the rooms vector). center is
// the world point of the opening; kind records how the resolver treated it.
struct CanonDoorway {
    uint32_t a = 0, b = 0;
    DoorwayKind kind = DoorwayKind::None;
    float cx = 0, cy = 0, cz = 0;   // doorway center (world m) — at the opening, floor-ish Y
    // For a cut doorway: which axis the host wall runs along (matches DoorAxis):
    // 0 = wall plane is X=const (door thin in X), 1 = wall plane is Z=const (thin in Z).
    uint32_t axis = 0;
};

// The fully parsed + resolved floor. rooms/doorways are 1:1 with the JSON after the
// resolver runs (every JSON door becomes a CanonDoorway). pvs[i] is the list of room
// ids visible from room i (== i + its directly-doored neighbours), used for the cull.
struct CanonFloor {
    int                        floorNum = 0;
    std::string                name;
    std::vector<CanonRoom>     rooms;
    std::vector<CanonDoorway>  doorways;
    // Doorways that came straight from the JSON door list (== the parsed door count).
    // Any EXTRA doorways beyond this index were SYNTHESIZED by the isolated-room
    // resolver (the vertical descent tubes for Cave System / Hidden Sub-Level).
    uint32_t                   jsonDoorCount = 0;
    std::vector<std::vector<uint32_t>> pvs;   // pvs[room] = visible room set (incl. self)

    bool valid() const { return !rooms.empty(); }

    // Point-in-room test (XZ + Y span, with a small margin so a player standing in a
    // doorway or just inside a wall still resolves to a room). Returns the room id, or
    // kNoRoom if the point is in no room.
    uint32_t roomAt(float x, float y, float z, float margin = 0.5f) const;

    // The visible-room set for a camera at (x,y,z): the room the camera is in plus that
    // room's PVS (doored neighbours). If the camera is in no room, returns the nearest
    // room's PVS (so the player never sees an empty world at a doorway seam). Written
    // into `out` (cleared first). Feed to Scene::setVisibleRooms.
    void visibleRoomsAt(float x, float y, float z, std::vector<uint32_t>& out) const;

    // Find a room by an exact name, or by a name SUBSTRING (case-sensitive). Returns its
    // index or kNoRoom. Used to re-aim the Level-1 beat sequence onto the REAL canonical
    // room centers (Jake's Cell -> Main Hall -> Security/Research/Medical/Armory ->
    // Boss Arena (Martinez) -> Elevator Lobby), instead of the legacy hand-coded spine.
    uint32_t roomByName(const std::string& exactOrSub) const;
};

// The canonical Level-1 beat sequence (the re-aimed spawn -> boss -> exit path), resolved
// to REAL room centers from a loaded CanonFloor. Any field is kNoRoom if that room is
// absent. The host spawns in `jakeCell`, the player descends the wide `mainHall`, fights
// through `security`/`research`/`medical`/`armory`, faces Martinez in `bossArena`, and
// exits via `elevatorLobby`. Cells line both sides (the WL/WR/EL/ER columns).
struct CanonBeats {
    uint32_t jakeCell      = kNoRoom;
    uint32_t mainHall      = kNoRoom;
    uint32_t security      = kNoRoom;
    uint32_t research      = kNoRoom;
    uint32_t medical       = kNoRoom;
    uint32_t armory        = kNoRoom;
    uint32_t bossArena     = kNoRoom;   // Martinez
    uint32_t elevatorLobby = kNoRoom;
};
// Resolve the beat rooms from a loaded floor by canonical name.
CanonBeats canonBeats(const CanonFloor& floor);

// Parse + resolve one floor ("1".."7") from the v2 project JSON at `jsonPath`. Returns
// a CanonFloor; on failure (file missing / parse error / floor absent) returns a floor
// with valid()==false (rooms empty) so the caller can fall back to the legacy build.
CanonFloor loadCanonFloor(std::string_view jsonPath, int floorNum);

// The canonical source path baked into the repo's environment (the owner's project).
// Override via the --world canonlevel arg if needed.
std::string canonProjectJsonPath();

// Build a parsed+resolved CanonFloor into `scene` (render meshes via `device`, static
// collision via `physics`). Every room shell entity is tagged with its room id. The
// doorway resolver's cut openings + gap bridges + descent tubes are realised here so
// the floor is navigable. `artMaskWalls/Floors` (default false) suppress the graybox
// SURFACE render where a GLB overlay will cover it (collision always built), mirroring
// level1.cpp's Level1ArtMask. Returns nothing (the CanonFloor already carries the data
// the host needs: room centers, the PVS, doorway list).
struct CanonBuildOpts {
    bool artMaskWalls  = false;
    bool artMaskFloors = false;
    // If non-null, an animated SM_Door_A + SM_DoorFrame_A door (wired into this
    // DoorSystem via buildLevelDoor) is placed + scaled to fit EACH adjacency/overlap
    // doorway opening (the cut doorways). Gap-bridge corridors + cross-level tubes get
    // no slab (they are open passages). Missing GLB -> the door's graybox box stays
    // (buildLevelDoor's per-piece fallback) so the level never breaks. The host ticks +
    // draws this DoorSystem (doors.update / doors.drawMeshes) each frame.
    DoorSystem* doors = nullptr;
};
void buildCanonFloor(const CanonFloor& floor, Scene& scene,
                     x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics,
                     const CanonBuildOpts& opts = {});

// Headless self-test (--test-canonlevel). Loads Floor 1, asserts the 53 rooms / 111
// doors parse, the doorway resolver produces the expected kind histogram, room ids are
// assigned to built entities, the portal PVS prunes the drawn set in a sample room
// (cull proof), and the isolated/cross-level rooms are linked. Logs PASS/FAIL C#,
// returns true iff all pass. No window/Vulkan.
bool runCanonLevelSelfTest();

} // namespace x3::game
