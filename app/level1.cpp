// EFLZ Level 1 "Awakening" graybox layout (§2). See app/level1.h.
//
// Clean-room: built from the Scene + IRenderDevice + IPhysicsWorld interfaces and
// the mesh_prims box builder only. No purchased C# / id Tech source consulted.
#include "level1.h"
#include "mesh_prims.h"

#include "engine/core/x3_log.h"

#include <cstdint>
#include <string>
#include <vector>

namespace x3::game {

namespace {

constexpr float kIdentity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

constexpr float kWallT = 0.2f;   // wall thickness
constexpr float kDoorHalf = 0.6f; // doorway opening half-width (1.2 m wide)
constexpr float kLintelBottom = 2.1f; // head clearance under the doorway lintel
constexpr float kCeilT = 0.2f;   // ceiling cap thickness (collision lid)

// ---- Canonical Level 1 room table (footprints + RAISED ceiling heights). This
// is the single source of truth shared with env_art.cpp (via level1Rooms()): the
// art overlay tiles its GLB floors/walls/ceilings/lights to these EXACT bounds +
// heights, so geometry, art and lights stay in lockstep. Ceilings are varied (NOT
// uniform): the cell is snug, corridors moderately taller, the armory/checkpoint
// roomier, and the arena + elevator shaft notably tall for a boss-room feel.
//   x0,   x1,   zHalf, ceil
const L1RoomDef kRooms[(uint32_t)L1Room::Count] = {
    {  0.0f,  6.0f, 3.0f, 3.5f },  // Cell       — snug detention
    {  6.0f, 22.0f, 3.0f, 4.0f },  // Corridor   — moderately taller
    { 22.0f, 30.0f, 4.0f, 4.5f },  // Armory     — roomier
    { 30.0f, 42.0f, 4.0f, 4.5f },  // Checkpoint — roomier
    { 42.0f, 56.0f, 7.0f, 8.0f },  // Arena      — TALL boss room
    { 56.0f, 59.0f, 1.5f, 9.0f },  // Elevator   — tall shaft
};

// Add one world-baked graybox box (render mesh + optional static collision) to
// the scene. Mirrors level.cpp's addPiece. Returns the entity id.
// `visible`: when false the render mesh is omitted (collision-only) so real GLB
// art can be drawn over this volume without z-fighting (EFLZ art pass).
uint32_t addBox(Scene& scene, x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics,
                float hx, float hy, float hz, float cx, float cy, float cz,
                x3::rhi::TextureHandle tex, const float color[4],
                uint32_t tag = (uint32_t)Tag::Static, bool collide = true,
                bool visible = true) {
    x3::prims::PrimMesh geo = x3::prims::makeBox(hx, hy, hz, cx, cy, cz, 0.5f);
    Entity e;
    if (visible)
        e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                                   geo.index.data(), (uint32_t)geo.index.size());
    e.tex = tex;
    e.baseColor[0] = color[0]; e.baseColor[1] = color[1];
    e.baseColor[2] = color[2]; e.baseColor[3] = color[3];
    for (int i = 0; i < 16; ++i) e.transform[i] = kIdentity[i];
    e.tag = tag;
    e.visible = visible;
    if (collide)
        e.body = physics.addStaticMesh(geo.cverts.data(), (uint32_t)(geo.cverts.size() / 3),
                                       geo.cindex.data(), (uint32_t)geo.cindex.size());
    return scene.add(e);
}

// Floor slab for a room (thin slab at y=0, top surface flush with floor).
void addFloor(Scene& scene, x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics,
              float cx, float cz, float hx, float hz, x3::rhi::TextureHandle tex,
              const float color[4], bool visible = true) {
    addBox(scene, device, physics, hx, 0.05f, hz, cx, -0.05f, cz, tex, color,
           (uint32_t)Tag::Static, /*collide*/true, visible);
}

// A solid wall running along X (its plane is z = const). Spans x in [x0,x1] and
// rises floor-to-ceiling (y in [0, wallH]) so the taller rooms have no gap above
// the wall the player could see/escape through.
void addWallX(Scene& scene, x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics,
              float x0, float x1, float z, float wallH, x3::rhi::TextureHandle tex,
              const float color[4], bool visible = true) {
    const float hx = (x1 - x0) * 0.5f, cx = (x0 + x1) * 0.5f;
    addBox(scene, device, physics, hx, wallH * 0.5f, kWallT * 0.5f, cx, wallH * 0.5f, z,
           tex, color, (uint32_t)Tag::Static, /*collide*/true, visible);
}

// A cross-wall running along Z (its plane is x = const), spanning z in [z0,z1] and
// rising floor-to-ceiling (y in [0, wallH]), WITH a 1.2 m doorway gap centered at
// z=zDoor (the wall above the doorway lintel stays solid up to the ceiling). If
// withDoorway is false, builds a fully solid wall (end cap).
void addCrossWall(Scene& scene, x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics,
                  float x, float z0, float z1, float zDoor, bool withDoorway, float wallH,
                  x3::rhi::TextureHandle tex, const float color[4], bool visible = true) {
    if (!withDoorway) {
        const float hz = (z1 - z0) * 0.5f, cz = (z0 + z1) * 0.5f;
        addBox(scene, device, physics, kWallT * 0.5f, wallH * 0.5f, hz, x, wallH * 0.5f, cz,
               tex, color, (uint32_t)Tag::Static, /*collide*/true, visible);
        return;
    }
    // Left segment: z in [z0, zDoor - kDoorHalf]
    {
        const float lo = z0, hi = zDoor - kDoorHalf;
        if (hi > lo) {
            const float hz = (hi - lo) * 0.5f, cz = (lo + hi) * 0.5f;
            addBox(scene, device, physics, kWallT * 0.5f, wallH * 0.5f, hz, x, wallH * 0.5f, cz,
                   tex, color, (uint32_t)Tag::Static, /*collide*/true, visible);
        }
    }
    // Right segment: z in [zDoor + kDoorHalf, z1]
    {
        const float lo = zDoor + kDoorHalf, hi = z1;
        if (hi > lo) {
            const float hz = (hi - lo) * 0.5f, cz = (lo + hi) * 0.5f;
            addBox(scene, device, physics, kWallT * 0.5f, wallH * 0.5f, hz, x, wallH * 0.5f, cz,
                   tex, color, (uint32_t)Tag::Static, /*collide*/true, visible);
        }
    }
    // Solid header above the doorway, lintel-bottom up to the (now taller) ceiling.
    // It is mostly hidden by the door frame art, so it stays visible regardless of
    // the wall mask (a cheap header that reads fine over the GLB walls) and seals
    // the gap between the doorway opening and the raised ceiling.
    {
        const float lh = (wallH - kLintelBottom) * 0.5f;
        const float lcy = kLintelBottom + lh;
        if (lh > 0.0f)
            addBox(scene, device, physics, kWallT * 0.5f, lh, kDoorHalf, x, lcy, zDoor, tex, color,
                   (uint32_t)Tag::Static, /*collide*/true, visible);
    }
}

// A ceiling lid over a room (thin collision slab at the ceiling height). Always
// collision-only invisible: the GLB ceiling panels (env_art) provide the visible
// ceiling; this just stops the player/camera leaving through the top of the taller
// rooms (and seals the open elevator shaft / tall arena). Spans the full room.
void addCeiling(Scene& scene, x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics,
                float cx, float cz, float hx, float hz, float ceilY,
                x3::rhi::TextureHandle tex) {
    const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    addBox(scene, device, physics, hx, kCeilT * 0.5f, hz, cx, ceilY + kCeilT * 0.5f, cz,
           tex, white, (uint32_t)Tag::Static, /*collide*/true, /*visible*/false);
}

} // namespace

Level1Layout buildLevel1(Scene& scene,
                         x3::rhi::IRenderDevice& device,
                         x3::phys::IPhysicsWorld& physics,
                         const Level1ArtMask& artMask) {
    x3::logInfo("buildLevel1: EFLZ 'Awakening' — 6 graybox rooms (cell/corridor/armory/checkpoint/arena/elevator)"
                + std::string(artMask.walls ? " [GLB walls]" : "")
                + std::string(artMask.floors ? " [GLB floors]" : ""));
    const bool wallVis  = !artMask.walls;   // graybox SIDE-wall render on iff no GLB wall art
    // Cross-walls (room end-caps + the doorway walls) are NOT covered by GLB art (only the
    // door FRAMES are), so keep their graybox render ON regardless of the wall mask —
    // otherwise the suppressed cross-walls show the void (the "see-through walls" bug).
    const bool crossWallVis = true;
    const bool floorVis = !artMask.floors;  // graybox floor render on iff no GLB floor art

    // ---- Shared graybox textures (a couple of checkers reused per surface). ----
    auto floorPx = x3::prims::makeCheckerRGBA(256, 32, 200,205,215, 45,55,80);
    x3::rhi::TextureHandle floorTex = device.createTexture(floorPx.data(), 256, 256, true);
    auto wallPx = x3::prims::makeCheckerRGBA(256, 32, 170,175,185, 60,65,80);
    x3::rhi::TextureHandle wallTex = device.createTexture(wallPx.data(), 256, 256, true);

    // ---- Room footprints + RAISED ceiling heights come from the shared canonical
    // table (kRooms / level1Rooms()), so env_art.cpp tiles its GLB art to the EXACT
    // same bounds + heights. z spans are symmetric: x in [x0,x1], z in [-zHalf,+zHalf].
    struct Room { float x0, x1, zHalf, ceil; float tint[4]; };
    const float tints[(uint32_t)L1Room::Count][4] = {
        { 0.55f, 0.60f, 0.75f, 1.0f }, // cell      — cool blue (detention)
        { 0.70f, 0.55f, 0.45f, 1.0f }, // corridor  — warm corridor
        { 0.50f, 0.70f, 0.55f, 1.0f }, // armory    — green
        { 0.75f, 0.70f, 0.40f, 1.0f }, // checkpoint— amber (security)
        { 0.65f, 0.40f, 0.40f, 1.0f }, // arena     — red (boss)
        { 0.45f, 0.60f, 0.80f, 1.0f }, // elevator  — blue
    };
    Room rooms[(uint32_t)L1Room::Count];
    for (uint32_t i = 0; i < (uint32_t)L1Room::Count; ++i) {
        rooms[i].x0 = kRooms[i].x0; rooms[i].x1 = kRooms[i].x1;
        rooms[i].zHalf = kRooms[i].zHalf; rooms[i].ceil = kRooms[i].ceil;
        for (int c = 0; c < 4; ++c) rooms[i].tint[c] = tints[i][c];
    }
    const Room& cell      = rooms[(uint32_t)L1Room::Cell];
    const Room& corridor  = rooms[(uint32_t)L1Room::Corridor];
    const Room& armory    = rooms[(uint32_t)L1Room::Armory];
    const Room& checkpoint= rooms[(uint32_t)L1Room::Checkpoint];
    const Room& arena     = rooms[(uint32_t)L1Room::Arena];
    const Room& elevator  = rooms[(uint32_t)L1Room::Elevator];

    auto roomCenter = [](const Room& r) {
        return x3::phys::Vec3{ (r.x0 + r.x1) * 0.5f, 0.0f, 0.0f };
    };
    auto roomHalf = [](const Room& r) {
        return x3::phys::Vec3{ (r.x1 - r.x0) * 0.5f, r.ceil, r.zHalf };
    };

    // ---- Per-room floor + the two long side walls (along X at z = ±zHalf), each
    // rising floor-to-(this room's)-ceiling, + an invisible ceiling collision lid. ----
    for (const Room& r : rooms) {
        const float cx = (r.x0 + r.x1) * 0.5f;
        addFloor(scene, device, physics, cx, 0.0f, (r.x1 - r.x0) * 0.5f, r.zHalf, floorTex, r.tint, floorVis);
        addWallX(scene, device, physics, r.x0, r.x1, -r.zHalf, r.ceil, wallTex, r.tint, wallVis);
        addWallX(scene, device, physics, r.x0, r.x1,  r.zHalf, r.ceil, wallTex, r.tint, wallVis);
        addCeiling(scene, device, physics, cx, 0.0f, (r.x1 - r.x0) * 0.5f, r.zHalf, r.ceil, floorTex);
    }

    // ---- Cross-walls at each X boundary. The cell back (x=0) and elevator far
    // (x=59) walls are solid end caps. The five interior boundaries each carry a
    // doorway gap (z=0) for doors A-E; the cross-wall spans the WIDER of the two
    // adjacent rooms (z + height) so it seals the back of the narrower/shorter
    // neighbor — no gap above a shorter room where the player could see/escape. ----
    const float wallTint[4] = { 0.62f, 0.66f, 0.78f, 1.0f };
    // Cell back wall (solid end cap), spans the cell width + height.
    addCrossWall(scene, device, physics, cell.x0, -cell.zHalf, cell.zHalf, 0.0f, false, cell.ceil, wallTex, wallTint, crossWallVis);
    // Elevator far wall (solid end cap).
    addCrossWall(scene, device, physics, elevator.x1, -elevator.zHalf, elevator.zHalf, 0.0f, false, elevator.ceil, wallTex, wallTint, crossWallVis);
    // Interior boundaries with doorways. For each, span the wider/taller neighbor.
    auto boundary = [&](float x, const Room& a, const Room& b) {
        const float zh = (a.zHalf > b.zHalf) ? a.zHalf : b.zHalf;
        const float h  = (a.ceil  > b.ceil ) ? a.ceil  : b.ceil;
        addCrossWall(scene, device, physics, x, -zh, zh, 0.0f, true, h, wallTex, wallTint, crossWallVis);
    };
    boundary(corridor.x0,   cell,       corridor);   // Door A x=6
    boundary(armory.x0,     corridor,   armory);     // Door B x=22
    boundary(checkpoint.x0, armory,     checkpoint); // Door C x=30
    boundary(arena.x0,      checkpoint, arena);      // Door D x=42
    boundary(elevator.x0,   arena,      elevator);   // Door E x=56

    // ---- Cell props (medical pod + the "medical equipment" strength target). ----
    // Medical pod: a low slab the player "wakes" on. Purely visual, no collision
    // so the player can stand near it. Equipment: a small box prop hidden when the
    // strength trigger fires (beat 1).
    const float podTint[4] = { 0.30f, 0.45f, 0.65f, 1.0f };
    addBox(scene, device, physics, 1.0f, 0.25f, 0.5f, 2.0f, 0.25f, 1.8f, floorTex, podTint,
           (uint32_t)Tag::Prop, /*collide*/false);
    const float equipTint[4] = { 0.85f, 0.75f, 0.30f, 1.0f };
    uint32_t equip = addBox(scene, device, physics, 0.3f, 0.4f, 0.3f, 1.5f, 0.4f, -1.8f,
                            floorTex, equipTint, (uint32_t)Tag::Prop, /*collide*/false);

    // ---- Fill the layout result. ----
    Level1Layout L;
    L.spawn = x3::phys::Vec3{ 1.5f, 0.05f, 0.0f };

    L.doorA = x3::phys::Vec3{ corridor.x0,   0.0f, 0.0f };
    L.doorB = x3::phys::Vec3{ armory.x0,     0.0f, 0.0f };
    L.doorC = x3::phys::Vec3{ checkpoint.x0, 0.0f, 0.0f };
    L.doorD = x3::phys::Vec3{ arena.x0,      0.0f, 0.0f };
    L.doorE = x3::phys::Vec3{ elevator.x0,   0.0f, 0.0f };

    L.cellCenter       = roomCenter(cell);
    L.corridorCenter   = roomCenter(corridor);
    L.armoryCenter     = roomCenter(armory);
    L.checkpointCenter = roomCenter(checkpoint);
    L.arenaCenter      = roomCenter(arena);
    L.elevatorCenter   = roomCenter(elevator);

    L.cellHalf       = roomHalf(cell);
    L.corridorHalf   = roomHalf(corridor);
    L.armoryHalf     = roomHalf(armory);
    L.checkpointHalf = roomHalf(checkpoint);
    L.arenaHalf      = roomHalf(arena);
    L.elevatorHalf   = roomHalf(elevator);

    L.ceilCell       = cell.ceil;
    L.ceilCorridor   = corridor.ceil;
    L.ceilArmory     = armory.ceil;
    L.ceilCheckpoint = checkpoint.ceil;
    L.ceilArena      = arena.ceil;
    L.ceilElevator   = elevator.ceil;

    L.equipmentProp = equip;

    x3::logInfo("buildLevel1: " + std::to_string(scene.size()) + " entities built; spawn ("
                + std::to_string(L.spawn.x) + ", " + std::to_string(L.spawn.z) + ")"
                + "; ceilings cell/cor/arm/chk/arena/elev = "
                + std::to_string((int)cell.ceil) + "/" + std::to_string((int)corridor.ceil) + "/"
                + std::to_string((int)armory.ceil) + "/" + std::to_string((int)checkpoint.ceil) + "/"
                + std::to_string((int)arena.ceil) + "/" + std::to_string((int)elevator.ceil) + " m");
    return L;
}

// Single source of truth for the room table (shared with env_art.cpp).
const L1RoomDef* level1Rooms() { return kRooms; }

} // namespace x3::game
