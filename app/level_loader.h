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
    // The room's one-line FLAVOUR LINE, straight from the project JSON's "desc"
    // field ("Brain-computer links. Memory extraction."). It was in the data all
    // along and the parser silently dropped it; the CUTAWAY VIEW's hover cards
    // (app/cutaway.h) are its first consumer. Floors 2-7 ship one per room; the
    // 53 Floor-1 rooms have an EMPTY desc in the JSON and are filled in from
    // kF1Desc in app/cutaway.cpp — PAIRED VALUE (NO_SLOP rule 4): if a Floor-1
    // room here ever gains a real "desc" in the JSON, drop its kF1Desc row.
    std::string desc;
    float cx = 0, cy = 0, cz = 0;   // center (world m)
    float w = 0, h = 0, d = 0;      // FULL extents (x, y, z) (world m)

    float x0() const { return cx - w * 0.5f; }
    float x1() const { return cx + w * 0.5f; }
    float y0() const { return cy - h * 0.5f; }   // floor
    float y1() const { return cy + h * 0.5f; }   // ceiling
    float z0() const { return cz - d * 0.5f; }
    float z1() const { return cz + d * 0.5f; }

    // W5-1 (level 4.5): OPEN PLATFORMS — rooms authored as thin slabs (h <= 1 m, the
    // Nexus Chamber tiers) build as a floor slab ONLY (no walls/lid/minted light) and
    // take NO doorways (they hang inside a cavern; canon_45 provides the climb path).
    bool platform = false;
    // A room whose ceiling is deliberately ABSENT (the Nexus Chamber Access — the
    // cavern void opens directly above it). Walls and floor build normally.
    bool openCeiling = false;
    // W5-1b (fix/spire-hollow-core, owner canon 2026-07-25: "4.5 is HIDDEN — only the
    // elevator reaches it"): this room's ceiling lid RENDERS (not collision-only).
    // Set for rooms whose roof is exposed to a legitimate player vantage ABOVE them
    // (the ex-open-ceiling Nexus Access room under the 4.5 cavern) — an invisible lid
    // there is a one-way hole in the world (the D3/D4 defect class).
    bool solidLid = false;
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
    // W2-E connectivity fix (LAW 1): per-doorway cut half-width (Overlap junctions widen
    // to their full shared throat) + the junction flag — an Overlap doorway is an OPEN
    // corridor junction: no slab is ever placed there (a door never stands in open space).
    float cutHalf = 0.8f;      // half-width of the wall cut (default kDoorHalf)
    bool  junction = false;    // true = open junction (no door slab, widened throat)
    // Index into the host DoorSystem of the SM_Door_A slab that fills this doorway, or
    // kNoLink if this doorway has NO door (a doorless opening / gap-bridge mouth / cross-
    // level tube — visibility ALWAYS passes through it). Set by buildCanonFloor when it
    // places the slabs (CanonBuildOpts::doors). The portal flood-fill (floodVisibleRoomsAt)
    // queries this door's DoorState: a CLOSED door is opaque and stops the flood; an
    // Open/Opening/Closing door lets visibility flow to the room behind it.
    uint32_t doorIndex = kNoLink;
};

// ---- Camera frustum (for the frustum-directional portal flood-fill). 6 world-space
// planes (left/right/bottom/top/near/far) with inward-pointing normals, built from a
// camera pose (engine convention: fwd = (cos p cos y, sin p, cos p sin y), up = +Y).
// A point is inside the frustum iff it is on the positive side of all 6 planes; a room
// AABB is visible iff it is not fully outside any single plane. Plain math (no glm) so
// the headless self-test can build a frustum without a window/device. ----
struct Frustum {
    // plane[i] = {nx, ny, nz, d}; a point p is inside the half-space iff dot(n,p)+d >= 0.
    float planes[6][4] = {};
    bool valid = false;

    // Build from a camera pose. fovYDeg is the VERTICAL field of view (matches
    // VulkanRenderDevice's glm::perspective), aspect = width/height, near/far in meters
    // (defaults match the device's 0.1 / 200). After build(), aabbVisible() is usable.
    static Frustum build(float eyeX, float eyeY, float eyeZ, float yaw, float pitch,
                         float fovYDeg, float aspect, float nearZ = 0.1f, float farZ = 200.0f);

    // True iff the world-AABB [min..max] is at least partially inside the frustum
    // (conservative: tests the AABB's most-positive corner against each plane).
    bool aabbVisible(float minX, float minY, float minZ,
                     float maxX, float maxY, float maxZ) const;
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
    // W3-2: per-room source floor number (filled by loadCanonTower; EMPTY on the
    // single-floor loadCanonFloor path — treat empty as "floorNum for every room").
    // Used for per-floor lint/diagnostic grouping only.
    std::vector<int>           roomFloorNum;
    // BREACH CUTS (fix/spire-hollow-core): openings buildCanonFloor cut in room walls
    // OUTSIDE the doorway graph (the SEAM-2 exterior breach + the stairwell's
    // per-floor connector mouths). Recorded so opening-aware consumers (RoomDressing's
    // wall paneling) do not seal them back up with panels. face: 0=-X 1=+X 2=-Z 3=+Z;
    // lo/hi = the cut span along the wall run (Z for X-plane faces, X for Z-plane).
    struct BreachCut { uint32_t room = kNoRoom; int face = 0; float lo = 0, hi = 0; };
    std::vector<BreachCut>     breachCuts;

    bool valid() const { return !rooms.empty(); }

    // Point-in-room test (XZ + Y span, with a small margin so a player standing in a
    // doorway or just inside a wall still resolves to a room). Returns the room id, or
    // kNoRoom if the point is in no room.
    uint32_t roomAt(float x, float y, float z, float margin = 0.5f) const;

    // The visible-room set for a camera at (x,y,z): the room the camera is in plus that
    // room's PVS (doored neighbours). If the camera is in no room, returns the nearest
    // room's PVS (so the player never sees an empty world at a doorway seam). Written
    // into `out` (cleared first). Feed to Scene::setVisibleRooms.
    //
    // NOTE: this is the LEGACY 1-hop set (current room + DIRECT doored neighbours). The
    // live path now prefers floodVisibleRoomsAt (frustum-directional multi-hop flood-fill);
    // this remains for the headless light-feed fallback + back-compat with older callers.
    void visibleRoomsAt(float x, float y, float z, std::vector<uint32_t>& out) const;

    // ---- PORTAL FLOOD-FILL PVS (frustum-directional, the live cull) -----------------
    // BFS from the camera's current room OUT through doorways, accumulating every reached
    // room into `out` (cleared first). A doorway is TRAVERSABLE iff:
    //   * it has no door (doorIndex == kNoLink) — a doorless opening / gap-bridge mouth /
    //     cross-level tube ALWAYS passes; OR
    //   * its door is NOT fully Closed (Open / Opening / Closing all pass) — a closed door
    //     is opaque and stops the flood through that doorway (the room behind stays culled).
    // A reached room is ADDED to the visible set only if its world-AABB intersects `frustum`
    // (so the bubble stretches down whatever hall you LOOK at, not a fixed radius). The
    // SOURCE room is always added (you're standing in it) regardless of the frustum. The
    // flood stops at `maxDepth` doorway hops and at `roomBudget` total rooms so it can never
    // fall back to drawing the whole tower. `doors` may be null (then every doorway passes —
    // matches the headless build before doors are wired). `frustum.valid==false` disables
    // the frustum gate (every reachable room within depth/budget is added — used by tests).
    void floodVisibleRoomsAt(float x, float y, float z,
                             const Frustum& frustum,
                             const DoorSystem* doors,
                             uint32_t maxDepth,
                             uint32_t roomBudget,
                             std::vector<uint32_t>& out) const;

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

// A detention-cell OBSERVATION WINDOW: a sealed, see-through armored viewport punched in
// Jake's Cell hall-facing (+Z) graybox wall so he can look OUT into the Main Hall (and be
// seen from it) — the "real transparent glass" that replaces the old opaque grey privacy
// pane. cellObsWindow() derives it from the SAME resolved geometry both the graybox
// builder and CellDressing consume, so the wall hole, the glass pane and the frame all
// land on one span. Placed on the WIDER of the two stubs flanking the +Z Main-Hall
// opening (never over a doorway), full room height, inset from the corner + door jamb.
// buildCanonFloor opens the graybox there and re-seals it with an INVISIBLE collision box
// (the cell stays escape-proof); CellDressing glazes it with clear glass + mullions.
struct CellWindow {
    uint32_t room  = kNoRoom;
    int      wall  = 3;                  // 3 = +Z (Main-Hall-facing)
    float    lo    = 0.0f, hi = 0.0f;    // X span of the opening (the +Z wall runs in X)
    float    y0    = 0.0f, y1 = 0.0f;    // vertical span (full floor..ceiling)
    float    plane = 0.0f;               // Z plane of the wall (cell z1)
    bool valid() const { return room != kNoRoom && hi > lo + 0.3f; }
};
CellWindow cellObsWindow(const CanonFloor& floor);

// ---- SEAL-BAND closure rules (fix/cell-shell-hole, owner playtest 2026-08-16: "This...
// is a hole in Jake's cell" — a sightline over the cell's wall tops into the lit Main
// Hall / West Cell Hall). A room that INTERPENETRATES a taller room (Overlap junction)
// has NO wall of its own above its ceiling on the shared boundary — its ceiling LID is
// the only closure of that band, so the lid must RENDER there; and where the partner's
// floor sits LOWER, the slot under the room's floor slab must be closed by a PLINTH
// band. These are the single rules BOTH the graybox builder and the seal-band lint
// consume (the same shared-rule pattern as the wall dedup); `legacyRule` reproduces the
// pre-fix behavior and is the lint's permanent negative control.
bool canonLidVisible(const CanonFloor& floor, uint32_t ri, bool legacyRule = false);

// A vertical closure band on `room`'s boundary face inside an Overlap partner:
// face 0..3 = -X/+X/-Z/+Z, `plane` = the face plane coordinate, lo..hi = the run span
// along the face (Z coords for X faces, X coords for Z faces), y0..y1 = vertical band.
struct CanonBand {
    uint32_t room = kNoRoom;
    int      face = 0;
    float    plane = 0, lo = 0, hi = 0, y0 = 0, y1 = 0;
};
std::vector<CanonBand> canonPlinthBands(const CanonFloor& floor, bool legacyRule = false);

// Parse + resolve one floor ("1".."7") from the v2 project JSON at `jsonPath`. Returns
// a CanonFloor; on failure (file missing / parse error / floor absent) returns a floor
// with valid()==false (rooms empty) so the caller can fall back to the legacy build.
CanonFloor loadCanonFloor(std::string_view jsonPath, int floorNum);

// W3-2 — load + MERGE every floor ("1", "2", ... while present, up to maxFloors) of the
// v2 project into ONE CanonFloor: the whole tower. Each floor runs the FULL existing
// per-floor pipeline (parse -> doorway resolve -> W2-E normalize -> PVS) via
// loadCanonFloor; rooms/doorways/pvs are then appended with index offsets. The data
// ships ABSOLUTE elevations (F1 y=0 .. F7 y=91), so rooms stack correctly and
// roomAt()'s Y test disambiguates floors. Floor-1 rooms come FIRST, so every existing
// roomByName / canonBeats / spawn lookup resolves exactly as before. Consecutive
// floors' Elevator Lobby rooms are joined by SYNTHESIZED CrossLevel doorways (the
// building's vertical spine: honest lint reachability + the builder's shaft tube),
// recorded past jsonDoorCount like the resolver's other synthesized edges. Fills
// roomFloorNum. Returns valid()==false if floor 1 itself fails (same fallback path).
CanonFloor loadCanonTower(std::string_view jsonPath, int maxFloors = 16);

// ---- PER-ROOM CEILING LIGHTING --------------------------------------------------------
// One warm-white ceiling point-light tagged with the room it belongs to. The canonlevel
// builder (level1.cpp env_art path) does NOT register the env_art Light_A fixtures, so the
// data-driven floor would otherwise only get ambient + the flashlight (too dark). We mint
// one (or a few, for big rooms) ceiling light per room here, near the ceiling Y at the room
// center, mirroring env_art.cpp's warm-tungsten intensity/range, and the host feeds ONLY
// the lights for the player's currently VISIBLE rooms each frame (room + PVS neighbours) so
// the active count stays well under the device's 64-light cap (53 rooms would blow it).
struct CanonLight {
    x3::rhi::PointLight light;
    uint32_t            room = kNoRoom;   // which CanonFloor room this light lights
};

// Build the per-room ceiling lights for a parsed floor: one warm-white omni at each room's
// center just below its ceiling, range scaled to reach the floor, with a second/third light
// for wide rooms (boss arena etc.) so they read evenly lit. Returns one CanonLight per
// emitter (tagged with its room id). Cheap; call once at level build.
std::vector<CanonLight> buildCanonLights(const CanonFloor& floor);

// Select the lights whose room is in `visibleRooms` (the per-frame PVS set), capped to
// `maxLights` (default 16 — keeps us well under the device's 64 hard cap while leaving
// headroom for the flashlight's 2 lights). The closest lights to `eye` win when over the
// cap. Appends into `out` (NOT cleared — the host inserts the flashlight first). Returns
// the number of room lights appended.
uint32_t selectVisibleCanonLights(const std::vector<CanonLight>& all,
                                   const std::vector<uint32_t>& visibleRooms,
                                   float eyeX, float eyeY, float eyeZ,
                                   std::vector<x3::rhi::PointLight>& out,
                                   uint32_t maxLights = 16);

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
    // Gameplay: when true, buildCanonFloor drops LOCKABLE slabs at the secured center
    // rooms' (Security / Medical / Armory) otherwise-open bridge-mouth entrances and
    // locks them. Left false by the geometry self-test so its open-passage assertions
    // (e.g. C11 "walk Main Hall -> Security through the gap-bridge") still hold.
    bool lockSecuredRooms = false;
    // TRAPDOOR CARVE (the canon-cell secret-room port): when hatchRoom is a valid room
    // index, that room's floor slab is built as FOUR segments around a square opening
    // at (hatchCx, hatchCz) with half-extent hatchHalf — the hole the SecretRoom's
    // flush floor-hatch panels cover (and drop the player through when open). Every
    // other room keeps its single full slab.
    uint32_t hatchRoom = kNoRoom;
    float hatchCx = 0.0f, hatchCz = 0.0f, hatchHalf = 0.9f;
    // SEAM 2 (world merge): EXTERIOR BREACH — cut ONE doorway-style opening
    // (gap + lintel, exactly like a resolved doorway's cut) in `breachRoom`'s
    // EXTERIOR wall on `breachFace` (0=-X 1=+X 2=-Z 3=+Z) at `breachCenter`
    // (the run coordinate along that wall), half-width `breachHalf`. This is
    // the interior half of the facility-exterior breach: the player walks from
    // the room, through this cut, across the vestibule, out the facade
    // (app/facility_exterior.*). kNoRoom (default) = no breach; the geometry
    // self-tests build without it so their wall assertions are untouched.
    uint32_t breachRoom = kNoRoom;
    int   breachFace = 3;
    float breachCenter = 0.0f, breachHalf = 1.5f;
    // FACILITY STAIRWELL (fix/spire-hollow-core): additional doorway-style cuts, one
    // per floor, where the stairwell's per-floor connector meets its target room's
    // wall. Same cut machinery as the single breach above (gap + lintel through the
    // wall builders); the stairwell module builds the connector + shaft geometry that
    // seals onto each cut (seam law: shared plane, zero gap). Empty by default.
    struct ExtraBreach {
        uint32_t room = kNoRoom;
        int      face = 0;              // 0=-X 1=+X 2=-Z 3=+Z
        float    center = 0.0f;         // run coordinate along the wall
        float    half = 1.1f;           // cut half-width
    };
    std::vector<ExtraBreach> extraBreaches;
    // W5-1b (fix/spire-hollow-core): the 4.5 ARRIVAL MOUTH — a doorway-style opening
    // cut in the vertical elevator-spine tube's +Z wall where the hidden level's
    // arrival tunnel joins the shaft. Applied to the ONE spine CrossLevel segment
    // whose Y span contains [spineMouthY0, spineMouthY1]. half <= 0 disables.
    float spineMouthY0 = 0.0f, spineMouthY1 = 0.0f, spineMouthHalf = 0.0f;
};
// NOTE: `floor` is taken by NON-const reference because the builder records each cut
// doorway's DoorSystem slab index back into floor.doorways[].doorIndex (so the portal
// flood-fill can later query that door's open/closed state). Pass the host's authoritative
// CanonFloor — the same one fed to floodVisibleRoomsAt / the cull.
void buildCanonFloor(CanonFloor& floor, Scene& scene,
                     x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics,
                     const CanonBuildOpts& opts = {});

// W5-1: the loader's textured-collidable-brush path, exported for content modules
// (canon_45's cavern shell / scaffold stairs) so hand-built geometry rides the SAME
// scene/physics/vis pipeline as the level itself. Half-extents + center, meters.
uint32_t canonAddBrush(Scene& scene, x3::rhi::IRenderDevice& device,
                       x3::phys::IPhysicsWorld& physics,
                       float hx, float hy, float hz, float cx, float cy, float cz,
                       x3::rhi::TextureHandle tex, const float color[4], uint32_t roomId,
                       bool collide = true, bool visible = true);

// Headless self-test (--test-canonlevel). Loads Floor 1, asserts the 53 rooms / 111
// doors parse, the doorway resolver produces the expected kind histogram, room ids are
// assigned to built entities, the portal PVS prunes the drawn set in a sample room
// (cull proof), and the isolated/cross-level rooms are linked. Logs PASS/FAIL C#,
// returns true iff all pass. No window/Vulkan.
bool runCanonLevelSelfTest();

} // namespace x3::game
