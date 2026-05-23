// EFLZ Level 1 "The Spire" — vertical B1->F7 graybox stack. See app/level1.h.
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

constexpr float kWallT = 0.2f;        // wall thickness
constexpr float kDoorHalf = 0.6f;     // doorway opening half-width (1.2 m wide)
constexpr float kLintelBottom = 2.1f; // head clearance under a doorway lintel
constexpr float kCeilT = 0.2f;        // ceiling cap thickness (collision lid)

// ---- Canonical Spire FLOOR table (footprints + base Y + RAISED ceiling heights).
// Single source of truth shared with env_art.cpp (via level1Rooms()): the art
// overlay tiles its GLB floors/walls/ceilings/lights to these EXACT bounds + base
// height + ceiling, so geometry, art and lights stay in lockstep across the whole
// tower. Floors stack along +Y at kFloorSpacing (5 m) so the elevator's 5 m stops
// land on each plate. All floors share the SAME XZ footprint so the elevator + stair
// shafts line up vertically. Ceilings are varied (NOT uniform) but stay within the
// 5 m floor pitch so plates never overlap (collision-clean); the rooftop F7 is open
// to the sky so it gets a genuinely tall cap.
//   x0,   x1,   zHalf, ceil, y0 (= floor index * 5 m)
const L1RoomDef kFloors[(uint32_t)L1Floor::Count] = {
    {  0.0f, 24.0f, 8.0f, 3.6f,  0.0f },  // B1 — Basement security (snug)
    {  0.0f, 24.0f, 8.0f, 4.6f,  5.0f },  // F1 — Atrium / lobby (tallest interior)
    {  0.0f, 24.0f, 8.0f, 3.8f, 10.0f },  // F2 — Medical wards
    {  0.0f, 24.0f, 8.0f, 4.0f, 15.0f },  // F3 — Labs
    {  0.0f, 24.0f, 8.0f, 3.6f, 20.0f },  // F4 — Offices
    {  0.0f, 24.0f, 8.0f, 4.5f, 25.0f },  // F5 — Synth bay (high-bay feel)
    {  0.0f, 24.0f, 8.0f, 4.2f, 30.0f },  // F6 — Executive
    {  0.0f, 24.0f, 8.0f, 7.0f, 35.0f },  // F7 — Rooftop (open sky, tall)
};

// ---- Central elevator shaft (a vertical column shared by every floor). XZ is
// constant up the whole tower; the cab rides it. The shaft sits at the +X end of
// each plate; a doorway opens from the shaft (its -X face, at x=shaftX0, z=0) into
// each floor plate. The stairwell mirrors it at the -X end (see buildLevel1).
constexpr float kShaftCx = 21.0f;     // shaft center X
constexpr float kShaftCz = 0.0f;      // shaft center Z (on the spine)
constexpr float kShaftHx = 1.5f;      // shaft half-width X (3 m wide)
constexpr float kShaftHz = 1.5f;      // shaft half-depth Z (3 m deep)
constexpr float kShaftX0 = kShaftCx - kShaftHx; // 19.5 — shaft -X face (door into plate)

// Per-floor tints so each plate reads as a distinct wing in the graybox fallback.
const float kFloorTints[(uint32_t)L1Floor::Count][4] = {
    { 0.55f, 0.60f, 0.75f, 1.0f }, // B1 — cool blue (detention/security)
    { 0.78f, 0.82f, 0.90f, 1.0f }, // F1 — bright glass atrium
    { 0.65f, 0.80f, 0.78f, 1.0f }, // F2 — clinical teal (medical)
    { 0.55f, 0.72f, 0.55f, 1.0f }, // F3 — lab green
    { 0.75f, 0.70f, 0.55f, 1.0f }, // F4 — office tan
    { 0.50f, 0.55f, 0.78f, 1.0f }, // F5 — synth blue
    { 0.72f, 0.62f, 0.45f, 1.0f }, // F6 — executive warm
    { 0.60f, 0.66f, 0.80f, 1.0f }, // F7 — sky/rooftop
};

const float kWallTint[4]  = { 0.62f, 0.66f, 0.78f, 1.0f };
const float kShaftTint[4] = { 0.40f, 0.42f, 0.50f, 1.0f };
const float kStairTint[4] = { 0.48f, 0.50f, 0.56f, 1.0f };

// Add one world-baked graybox box (render mesh + optional static collision) to the
// scene. `visible`: when false the render mesh is omitted (collision-only) so real
// GLB art can be drawn over this volume without z-fighting (EFLZ art pass).
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

// Floor slab for a plate (thin slab whose TOP surface is flush with floorY).
void addFloor(Scene& scene, x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics,
              float cx, float cz, float hx, float hz, float floorY,
              x3::rhi::TextureHandle tex, const float color[4], bool visible = true) {
    addBox(scene, device, physics, hx, 0.05f, hz, cx, floorY - 0.05f, cz, tex, color,
           (uint32_t)Tag::Static, /*collide*/true, visible);
}

// A solid wall running along X (its plane is z = const). Spans x in [x0,x1] and
// rises from floorY to floorY+wallH (floor-to-ceiling on this plate).
void addWallX(Scene& scene, x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics,
              float x0, float x1, float z, float floorY, float wallH,
              x3::rhi::TextureHandle tex, const float color[4], bool visible = true) {
    const float hx = (x1 - x0) * 0.5f, cx = (x0 + x1) * 0.5f;
    addBox(scene, device, physics, hx, wallH * 0.5f, kWallT * 0.5f, cx, floorY + wallH * 0.5f, z,
           tex, color, (uint32_t)Tag::Static, /*collide*/true, visible);
}

// A cross-wall running along Z (its plane is x = const), spanning z in [z0,z1] and
// rising from floorY to floorY+wallH, optionally WITH a 1.2 m doorway gap centered
// at z=zDoor (the wall above the doorway lintel stays solid to the ceiling). If
// withDoorway is false, builds a fully solid wall (end cap).
void addCrossWall(Scene& scene, x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics,
                  float x, float z0, float z1, float zDoor, bool withDoorway, float floorY,
                  float wallH, x3::rhi::TextureHandle tex, const float color[4], bool visible = true) {
    if (!withDoorway) {
        const float hz = (z1 - z0) * 0.5f, cz = (z0 + z1) * 0.5f;
        addBox(scene, device, physics, kWallT * 0.5f, wallH * 0.5f, hz, x, floorY + wallH * 0.5f, cz,
               tex, color, (uint32_t)Tag::Static, /*collide*/true, visible);
        return;
    }
    // Left segment: z in [z0, zDoor - kDoorHalf]
    {
        const float lo = z0, hi = zDoor - kDoorHalf;
        if (hi > lo) {
            const float hz = (hi - lo) * 0.5f, cz = (lo + hi) * 0.5f;
            addBox(scene, device, physics, kWallT * 0.5f, wallH * 0.5f, hz, x, floorY + wallH * 0.5f, cz,
                   tex, color, (uint32_t)Tag::Static, /*collide*/true, visible);
        }
    }
    // Right segment: z in [zDoor + kDoorHalf, z1]
    {
        const float lo = zDoor + kDoorHalf, hi = z1;
        if (hi > lo) {
            const float hz = (hi - lo) * 0.5f, cz = (lo + hi) * 0.5f;
            addBox(scene, device, physics, kWallT * 0.5f, wallH * 0.5f, hz, x, floorY + wallH * 0.5f, cz,
                   tex, color, (uint32_t)Tag::Static, /*collide*/true, visible);
        }
    }
    // Solid header above the doorway, lintel-bottom up to the ceiling.
    {
        const float lh = (wallH - kLintelBottom) * 0.5f;
        const float lcy = floorY + kLintelBottom + lh;
        if (lh > 0.0f)
            addBox(scene, device, physics, kWallT * 0.5f, lh, kDoorHalf, x, lcy, zDoor, tex, color,
                   (uint32_t)Tag::Static, /*collide*/true, visible);
    }
}

// A ceiling lid over a plate (thin collision slab at floorY+ceilH). Collision-only
// invisible: the GLB ceiling panels (env_art) provide the visible ceiling; this
// stops the player/camera leaving through the top. Spans the full plate.
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
    x3::logInfo(std::string("buildLevel1: EFLZ 'The Spire' — vertical B1->F7 stack (8 floors, 5 m pitch)")
                + std::string(artMask.walls ? " [GLB walls]" : "")
                + std::string(artMask.floors ? " [GLB floors]" : ""));
    const bool wallVis  = !artMask.walls;   // graybox SIDE-wall render on iff no GLB wall art
    const bool crossWallVis = true;         // cross-walls aren't covered by GLB art (no see-through)
    const bool floorVis = !artMask.floors;  // graybox floor render on iff no GLB floor art

    // ---- Shared graybox textures. ----
    auto floorPx = x3::prims::makeCheckerRGBA(256, 32, 200,205,215, 45,55,80);
    x3::rhi::TextureHandle floorTex = device.createTexture(floorPx.data(), 256, 256, true);
    auto wallPx = x3::prims::makeCheckerRGBA(256, 32, 170,175,185, 60,65,80);
    x3::rhi::TextureHandle wallTex = device.createTexture(wallPx.data(), 256, 256, true);

    Level1Layout L;

    // ===================================================================
    // 1) FLOOR PLATES — for each floor: floor slab, the 4 perimeter walls (two
    //    long side walls along X at z=±zHalf, plus -X end cap and +X end cap; the
    //    +X end cap carries the elevator-shaft doorway), and a ceiling lid. The
    //    elevator + stairwell columns (built below) pass through the +X / -X ends.
    // ===================================================================
    for (uint32_t fi = 0; fi < (uint32_t)L1Floor::Count; ++fi) {
        const L1RoomDef& f = kFloors[fi];
        const float cx = (f.x0 + f.x1) * 0.5f;
        const float* tint = kFloorTints[fi];
        const bool isRooftop = (fi == (uint32_t)L1Floor::F7);

        // Floor slab (every floor including B1; the rooftop still has a deck).
        addFloor(scene, device, physics, cx, 0.0f, (f.x1 - f.x0) * 0.5f, f.zHalf, f.y0,
                 floorTex, tint, floorVis);
        // Two long side walls (z = ±zHalf), floor-to-ceiling.
        addWallX(scene, device, physics, f.x0, f.x1, -f.zHalf, f.y0, f.ceil, wallTex, tint, wallVis);
        addWallX(scene, device, physics, f.x0, f.x1,  f.zHalf, f.y0, f.ceil, wallTex, tint, wallVis);
        // -X end cap (solid; the stairwell tucks just outside it via a doorway built
        // below). +X end cap carries the elevator-shaft doorway (z=0).
        addCrossWall(scene, device, physics, f.x0, -f.zHalf, f.zHalf, -f.zHalf - 2.0f,
                     /*withDoorway*/true, f.y0, f.ceil, wallTex, kWallTint, crossWallVis);
        addCrossWall(scene, device, physics, f.x1, -f.zHalf, f.zHalf, 0.0f,
                     /*withDoorway*/true, f.y0, f.ceil, wallTex, kWallTint, crossWallVis);
        // Ceiling lid (skip the rooftop: F7 is open to the sky). Collision-only.
        if (!isRooftop)
            addCeiling(scene, device, physics, cx, 0.0f, (f.x1 - f.x0) * 0.5f, f.zHalf,
                       f.y0 + f.ceil, floorTex);

        // Fill the per-floor layout result.
        L.floorBaseY[fi]  = f.y0;
        L.floorCeil[fi]   = f.ceil;
        L.floorCenter[fi] = x3::phys::Vec3{ cx, f.y0, 0.0f };
        // Elevator doorway: the shaft's -X face at z=0 on this floor.
        L.elevatorDoor[fi] = x3::phys::Vec3{ kShaftX0, f.y0, 0.0f };
    }

    // ===================================================================
    // 2) CENTRAL ELEVATOR SHAFT — a vertical hollow column at (kShaftCx,kShaftCz)
    //    spanning the full tower height. Two side walls (z=±kShaftHz) and a +X back
    //    wall enclose it; the -X face is open per floor (the doorway into the plate,
    //    already a gap in each floor's +X end cap at z=0). The cab platform itself
    //    is built by the host (main.cpp) so it can be animated. The shaft top is
    //    capped above F7 so the cab can't fly off.
    // ===================================================================
    const float shaftBottom = kFloors[(uint32_t)L1Floor::B1].y0;
    const float shaftTop    = kFloors[(uint32_t)L1Floor::F7].y0 + kFloors[(uint32_t)L1Floor::F7].ceil;
    const float shaftH      = shaftTop - shaftBottom;
    // Shaft side walls (run along X, plane z=±kShaftHz), from bottom to top.
    addBox(scene, device, physics, kShaftHx, shaftH * 0.5f, kWallT * 0.5f,
           kShaftCx, shaftBottom + shaftH * 0.5f, kShaftCz - kShaftHz,
           wallTex, kShaftTint, (uint32_t)Tag::Static, true, crossWallVis);
    addBox(scene, device, physics, kShaftHx, shaftH * 0.5f, kWallT * 0.5f,
           kShaftCx, shaftBottom + shaftH * 0.5f, kShaftCz + kShaftHz,
           wallTex, kShaftTint, (uint32_t)Tag::Static, true, crossWallVis);
    // Shaft +X back wall (plane x=kShaftCx+kShaftHx), full height.
    addBox(scene, device, physics, kWallT * 0.5f, shaftH * 0.5f, kShaftHz,
           kShaftCx + kShaftHx, shaftBottom + shaftH * 0.5f, kShaftCz,
           wallTex, kShaftTint, (uint32_t)Tag::Static, true, crossWallVis);
    // Shaft top cap (collision lid above F7's ceiling).
    addBox(scene, device, physics, kShaftHx, kCeilT * 0.5f, kShaftHz,
           kShaftCx, shaftTop + kCeilT * 0.5f, kShaftCz,
           wallTex, kShaftTint, (uint32_t)Tag::Static, true, /*visible*/false);

    // ===================================================================
    // 3) EMERGENCY STAIRWELL — a switchback ramp column at the -X end (just outside
    //    each floor's -X doorway) linking every adjacent pair of floors. Each 5 m
    //    rise is a single straight ramp of stepped boxes within a 4 m landing well
    //    at x in [-4, 0], z in [-zHalf, -zHalf+... ] — kept simple: a straight stair
    //    of 10 steps per floor (0.5 m rise each). Purely collision graybox + a tint.
    // ===================================================================
    {
        const float stairX0 = -4.0f, stairX1 = 0.0f;     // stair well footprint (X)
        const float stairZ  = -8.0f + 1.6f;              // near the -X doorway (z<0 side)
        const float stepRun = (stairX1 - stairX0) / 10.0f; // 0.4 m run/step
        const float stepRise = kFloorSpacing / 10.0f;     // 0.5 m rise/step
        // Stair-well floor + outer walls spanning all floors (a single tall shaft).
        const float swBottom = shaftBottom;
        const float swTop     = kFloors[(uint32_t)L1Floor::F7].y0;
        const float swH       = swTop - swBottom + 0.5f;
        // Outer -X wall of the stair well.
        addBox(scene, device, physics, kWallT * 0.5f, swH * 0.5f, 2.0f,
               stairX0, swBottom + swH * 0.5f, stairZ,
               wallTex, kStairTint, (uint32_t)Tag::Static, true, crossWallVis);
        // Steps: one straight run per floor transition (B1->F1 ... F6->F7).
        for (uint32_t fi = 0; fi + 1 < (uint32_t)L1Floor::Count; ++fi) {
            const float baseY = kFloors[fi].y0;
            for (int s = 0; s < 10; ++s) {
                const float topY = baseY + (float)(s + 1) * stepRise;     // step top surface
                const float sx   = stairX0 + ((float)s + 0.5f) * stepRun; // step center X
                addBox(scene, device, physics, stepRun * 0.5f, topY * 0.5f - swBottom * 0.5f, 1.4f,
                       sx, (topY + swBottom) * 0.5f, stairZ,
                       wallTex, kStairTint, (uint32_t)Tag::Static, true, /*visible*/true);
            }
        }
    }

    // ===================================================================
    // 4) PER-FLOOR INTERIOR PARTITIONS (collision graybox; extra, not in the env_art
    //    floor table). These give the wings their distinct layouts per the spec.
    // ===================================================================
    // ---- F2 Medical wards: 3 ward rooms (Aria/Keisha/Emily) split by two partition
    //      walls along Z, each with a doorway. Wards run along +X; the rescue hub
    //      places the victims at the ward centers (returned in the layout).
    {
        const L1RoomDef& f2 = kFloors[(uint32_t)L1Floor::F2];
        const float y0 = f2.y0, h = f2.ceil;
        const float wx1 = 8.0f, wx2 = 15.0f;     // partition X positions (split into 3)
        addCrossWall(scene, device, physics, wx1, -f2.zHalf, f2.zHalf, 0.0f, true, y0, h,
                     wallTex, kWallTint, crossWallVis);
        addCrossWall(scene, device, physics, wx2, -f2.zHalf, f2.zHalf, 0.0f, true, y0, h,
                     wallTex, kWallTint, crossWallVis);
        L.wardA = x3::phys::Vec3{  4.0f, y0, -3.0f };  // Ward A (Aria)
        L.wardB = x3::phys::Vec3{ 11.5f, y0,  3.0f };  // Ward B (Keisha)
        L.wardC = x3::phys::Vec3{ 18.0f, y0, -3.0f };  // Ward C (Emily)
    }
    // ---- F6 Executive: Sarah's holding office partitioned in the -Z corner.
    {
        const L1RoomDef& f6 = kFloors[(uint32_t)L1Floor::F6];
        const float y0 = f6.y0, h = f6.ceil;
        // A small office box: partition wall along X at z=-3 (x in [0,8]) + a cross
        // wall at x=8 with a doorway, enclosing x in [0,8], z in [-zHalf,-3].
        addWallX(scene, device, physics, 0.0f, 8.0f, -3.0f, y0, h, wallTex, kWallTint, crossWallVis);
        addCrossWall(scene, device, physics, 8.0f, -f6.zHalf, -3.0f, -5.5f, true, y0, h,
                     wallTex, kWallTint, crossWallVis);
        L.execOffice = x3::phys::Vec3{ 4.0f, y0, -5.5f };
    }
    // ---- F7 rooftop: the finale arena center (helipad) is just the open plate.
    {
        const L1RoomDef& f7 = kFloors[(uint32_t)L1Floor::F7];
        L.rooftopCenter = x3::phys::Vec3{ (f7.x0 + f7.x1) * 0.5f, f7.y0, 0.0f };
    }

    // ===================================================================
    // 5) B1 LEGACY SUB-ZONES + props — the existing §3 beats run on the basement.
    //    The B1 plate is wide enough (x 0..24) to host the cell/corridor/armory/
    //    checkpoint/arena spine the original level used; we partition it lightly and
    //    map the legacy door/room accessors onto these sub-zones so level1_game.cpp
    //    keeps building + the Awakening beat sequence still plays in B1.
    // ===================================================================
    const float b1y = kFloors[(uint32_t)L1Floor::B1].y0;
    const float b1ceil = kFloors[(uint32_t)L1Floor::B1].ceil;
    // Cell props (medical pod + the strength-target "equipment" prop, beat 1).
    const float podTint[4] = { 0.30f, 0.45f, 0.65f, 1.0f };
    addBox(scene, device, physics, 1.0f, 0.25f, 0.5f, 2.0f, b1y + 0.25f, 1.8f, floorTex, podTint,
           (uint32_t)Tag::Prop, /*collide*/false);
    const float equipTint[4] = { 0.85f, 0.75f, 0.30f, 1.0f };
    uint32_t equip = addBox(scene, device, physics, 0.3f, 0.4f, 0.3f, 1.5f, b1y + 0.4f, -1.8f,
                            floorTex, equipTint, (uint32_t)Tag::Prop, /*collide*/false);

    // ---- Legacy door + room mapping (all on B1; the doorway centers are placed
    //      along the B1 spine, and the elevator gate is the shaft doorway). ----
    L.spawn = x3::phys::Vec3{ 1.5f, b1y + 0.05f, 0.0f };

    // B1 spine zones laid left->right inside the 24 m plate, ending in a real arena
    // in front of the elevator shaft (x>=19.5) so Martinez has clearance (not inside
    // the shaft). Doors A-D partition the sub-rooms; Door E is the shaft doorway.
    // Door X positions come from the shared header constants (kB1Door*X) so the
    // env_art GLB door FRAMES (seeded in level1_game.cpp from the same constants)
    // line up exactly with these doorway gaps — no more frame floating mid-room.
    L.doorA = x3::phys::Vec3{ kB1DoorAX, b1y, 0.0f };
    L.doorB = x3::phys::Vec3{ kB1DoorBX, b1y, 0.0f };
    L.doorC = x3::phys::Vec3{ kB1DoorCX, b1y, 0.0f };
    L.doorD = x3::phys::Vec3{ kB1DoorDX, b1y, 0.0f };
    L.doorE = L.elevatorDoor[(uint32_t)L1Floor::B1];  // arena -> elevator shaft (x=19.5)

    // Sub-room centers sit mid-zone between adjacent doors (widened spacing):
    //   cell [0,4.5] corridor [4.5,9] armory [9,13] checkpoint [13,16] arena [16,19.5].
    L.cellCenter       = x3::phys::Vec3{  2.25f, b1y, 0.0f };
    L.corridorCenter   = x3::phys::Vec3{  6.75f, b1y, 0.0f };
    L.armoryCenter     = x3::phys::Vec3{ 11.0f,  b1y, 0.0f };
    L.checkpointCenter = x3::phys::Vec3{ 14.5f,  b1y, 0.0f };
    L.arenaCenter      = x3::phys::Vec3{ 17.75f, b1y, 0.0f };

    // ---- B1 interior partitions: cross-walls (with 1.2 m doorway gaps at z=0) at
    //      each spine door X, so the Awakening sub-rooms (cell/corridor/armory/
    //      checkpoint/arena) are walled off like the original 6-room level. This
    //      confines each encounter to its zone (the alarm enemies can't roam into
    //      the later checkpoint fight) and gives the doors A-D a real wall to sit
    //      in. The DoorSystem slab fills each z=0 gap. ----
    {
        const float bz = kFloors[(uint32_t)L1Floor::B1].zHalf;
        const float bh = b1ceil;
        const float partX[4] = { L.doorA.x, L.doorB.x, L.doorC.x, L.doorD.x };
        for (float px : partX)
            addCrossWall(scene, device, physics, px, -bz, bz, 0.0f, true, b1y, bh,
                         wallTex, kWallTint, crossWallVis);
    }

    // Half-extents fit WITHIN each widened sub-room (no longer overshoot the
    // partition walls the way the old 3.0/2.5 cell/corridor extents did).
    L.cellHalf       = x3::phys::Vec3{ 2.0f, b1ceil, 8.0f };
    L.corridorHalf   = x3::phys::Vec3{ 2.0f, b1ceil, 8.0f };
    L.armoryHalf     = x3::phys::Vec3{ 1.8f, b1ceil, 8.0f };
    L.checkpointHalf = x3::phys::Vec3{ 1.3f, b1ceil, 8.0f };
    L.arenaHalf      = x3::phys::Vec3{ 1.5f, b1ceil, 8.0f };

    L.ceilCell = L.ceilCorridor = L.ceilArmory = L.ceilCheckpoint = b1ceil;
    L.ceilArena = b1ceil;
    L.ceilElevator = kFloors[(uint32_t)L1Floor::B1].ceil;

    // ---- Elevator shaft layout (the host builds the cab here). ----
    L.elevatorCenter = x3::phys::Vec3{ kShaftCx, b1y, kShaftCz };
    L.elevatorHalf   = x3::phys::Vec3{ kShaftHx, shaftTop, kShaftHz };

    L.equipmentProp = equip;

    x3::logInfo("buildLevel1: " + std::to_string(scene.size()) + " entities; spawn ("
                + std::to_string(L.spawn.x) + ", " + std::to_string(L.spawn.y) + ", "
                + std::to_string(L.spawn.z) + "); floors B1..F7 baseY = "
                + std::to_string((int)L.floorBaseY[0]) + ".."
                + std::to_string((int)L.floorBaseY[(uint32_t)L1Floor::F7]) + " m, pitch "
                + std::to_string((int)kFloorSpacing) + " m; shaft @ ("
                + std::to_string((int)kShaftCx) + "," + std::to_string((int)kShaftCz) + ")");
    return L;
}

// Single source of truth for the floor table (shared with env_art.cpp).
const L1RoomDef* level1Rooms() { return kFloors; }

} // namespace x3::game
