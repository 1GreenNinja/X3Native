// EFLZ Level 1 "The Spire" — vertical B1->F7 graybox stack. See app/level1.h.
//
// Clean-room: built from the Scene + IRenderDevice + IPhysicsWorld interfaces and
// the mesh_prims box builder only. No purchased C# / id Tech source consulted.
#include "level1.h"
#include "mesh_prims.h"

#include "engine/core/x3_log.h"

#include <algorithm>
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
//
// FLOOR-1 RELAY (2026-05): the B1 plate was grown from the old 24x16 m placeholder to
// the REAL Floor-1 detention footprint (docs/design/SPIRE_LEVELARCHITECT_DIMS.md):
// X -24..+60 (~84 m, covering the detention core X -20..+18 AND the eastern stairs->
// caves arm out to the Side Grotto at x=55) and zHalf=38 (covering the detention Z span
// -36..+7 with margin). The B1 plate carries the full 29-room interior (built in
// buildLevel1). The DETENTION content bounding box is ~75 x 43 m (asserted by the
// self-test via level1DetentionFootprint()); the raw plate is a superset.
//
// FLOORS 2-7 DIMENSIONING (2026-05): F2-F7 no longer share B1's plate — each gets its
// OWN identity-appropriate footprint at REAL LevelArchitect scale (Floor 1 is ~75x43 m;
// the v10.9 source has NO authored geometry for F2-7, so they are authored fresh at that
// scale or larger — see SPIRE_LEVELARCHITECT_DIMS.md). KEY LAYOUT INVARIANT: the elevator
// shaft (kShaftCx=21) stays at the EAST edge of every floor (x1 ~= 25, just past the
// shaft), so a rider always arrives at x~=17.5 right at the shaft mouth AND the per-floor
// "hub" arrival trigger (spire_mid/top place it at [x1-8, x1]) lands ON the arrival point
// (it was detached at x~52-60 while F2-7 shared B1's x1=60 plate). Each floor then grows
// WESTWARD (x0 negative) + DEEP (zHalf) to its real size; the existing encounters sit in
// the eastern arrival third (content is authored at absolute x in [0,18], z in [-6.5,6.5]
// — all inside every new plate) and the western space is partitioned into identity rooms
// (see SS4). F5 Drone Manufacturing is the largest (a high-bay assembly hall). B1 + F1 are
// UNCHANGED (F1 Atrium has no LevelArchitect source; B1's footprint is read by the
// sub-levels). The 5 m vertical pitch is UNCHANGED (elevator + spire_*/sublevel content
// that reads floorBaseY[] stay in lockstep); ceilings stay <= 4.8 m (except the open-sky
// rooftop F7) so stacked plates never overlap.
//   x0,    x1,    zHalf,  ceil, y0 (= floor index * 5 m)
const L1RoomDef kFloors[(uint32_t)L1Floor::Count] = {
    { -24.0f, 60.0f, 38.0f, 4.0f,  0.0f },  // B1 — Detention Level (full 29-room interior) [unchanged]
    { -24.0f, 60.0f, 38.0f, 4.6f,  5.0f },  // F1 — Atrium / lobby (no LevelArchitect source) [unchanged]
    { -50.0f, 25.0f, 22.0f, 3.8f, 10.0f },  // F2 — Medical Bay          (75 x 44; wards wing)
    { -50.0f, 25.0f, 22.0f, 4.0f, 15.0f },  // F3 — Genetics Lab         (75 x 44; research + gene-vats)
    { -54.0f, 25.0f, 23.0f, 3.8f, 20.0f },  // F4 — Cybernetics Workshop (79 x 46; + Nexus connector W)
    { -72.0f, 25.0f, 32.0f, 4.8f, 25.0f },  // F5 — Drone Manufacturing  (97 x 64; large high-bay)
    { -58.0f, 25.0f, 26.0f, 4.4f, 30.0f },  // F6 — Alien Technology Lab (83 x 52; tech halls)
    { -54.0f, 25.0f, 23.0f, 7.0f, 35.0f },  // F7 — Executive Laboratory (79 x 46; open finale)
};

// ---- FLOOR 1 "Detention Level" — the authoritative LevelArchitect transcription
// (docs/design/SPIRE_LEVELARCHITECT_DIMS.md). 29 rooms, transcribed DIRECTLY (no axis
// flip): center (x,z) + full extents (w,h,d). floorY = the room's floor relative to the
// B1 plate base (0 = ground; the descending stairs/caves dip below into negative Y, but
// CLAMPED above the sub-levels at y=-5 — see buildLevel1's cave-arm note). monster/npc
// carry the Cell(Monster) / Sarah's-cell flags. Laid on the native B1 plate.
const L1DetentionRoom kDetention[] = {
    // name                          cx,    cz,   floorY, w,    h,   d,    monster, npc
    { "Jake's Cell",                  0.0f,   0.0f,  0.0f,  7.0f, 4.0f, 6.0f, false, false },
    { "Cell 2 (Abandoned)",           0.0f,  -8.0f,  0.0f,  6.0f, 3.5f, 5.0f, false, false },
    { "Cell 3 (Failed Exp)",          0.0f, -15.0f,  0.0f,  6.0f, 3.5f, 5.0f, true,  false },
    { "Cell 4 (Skeleton)",            0.0f, -22.0f,  0.0f,  6.0f, 3.5f, 5.0f, false, false },
    { "Main Hallway",                 5.5f, -12.0f,  0.0f,  3.0f, 3.5f, 26.0f, false, false },
    { "Guard Station",               11.0f,  -2.0f,  0.0f,  5.0f, 3.5f, 5.0f, false, false },
    { "Storage",                     11.0f,  -9.0f,  0.0f,  5.0f, 3.5f, 5.0f, false, false },
    { "Medical Bay",                 11.0f, -16.0f,  0.0f,  5.0f, 3.5f, 5.0f, false, false },
    { "Armory",                      11.0f, -23.0f,  0.0f,  5.0f, 3.5f, 5.0f, false, false },
    { "Elevator Lobby",               5.5f, -27.0f,  0.0f,  5.0f, 4.0f, 4.0f, false, false },
    { "Adjacent Cell",                5.5f,   5.0f,  0.0f,  5.0f, 3.5f, 4.0f, false, false },
    { "Old Armory",                  -1.0f,   7.0f,  0.0f,  7.0f, 3.5f, 5.0f, false, false },
    { "Creepy Passage",              16.0f,  -2.0f,  0.0f,  4.0f, 3.0f, 3.0f, false, false },
    { "Cell 5 (Vacated)",           -20.0f,  -4.0f,  0.0f,  6.0f, 3.5f, 5.0f, false, false },
    { "Cell 6 (Infected)",          -20.0f, -11.0f,  0.0f,  6.0f, 3.5f, 5.0f, true,  false },
    { "Cell 7 (Dead Guard)",        -20.0f, -18.0f,  0.0f,  6.0f, 3.5f, 5.0f, false, false },
    { "Cell 8 (Containment)",       -20.0f, -25.0f,  0.0f,  6.0f, 3.5f, 5.0f, false, false },
    { "Cell 9 (Collapsed)",         -20.0f, -32.0f,  0.0f,  6.0f, 3.5f, 5.0f, false, false },
    { "Cell 10 (Sarah's - Empty)",   -7.0f,  -4.0f,  0.0f,  6.0f, 3.5f, 5.0f, false, true  },
    { "Cell 11 (Feral)",             -7.0f, -11.0f,  0.0f,  6.0f, 3.5f, 5.0f, true,  false },
    { "Cell 12 (Flooded)",           -7.0f, -18.0f,  0.0f,  6.0f, 3.5f, 5.0f, false, false },
    { "Cell 13 (Mutation)",          -7.0f, -25.0f,  0.0f,  6.0f, 3.5f, 5.0f, true,  false },
    { "Cell 14 (Blood Trail)",       -7.0f, -32.0f,  0.0f,  6.0f, 3.5f, 5.0f, false, false },
    { "Cell Block B Hallway",       -13.5f, -18.0f,  0.0f,  4.0f, 3.5f, 32.0f, false, false },
    { "CB South Connector",         -13.5f, -36.0f,  0.0f, 14.0f, 3.5f, 3.0f, false, false },
    { "Descending Stairs",           20.0f,  -2.0f, -1.0f,  4.0f, 5.0f, 3.0f, false, false },
    { "Cave Tunnel",                 27.0f,  -2.0f, -2.0f, 10.0f, 3.5f, 3.0f, false, false },
    { "Crystal Cavern",              41.0f,  -2.0f, -3.0f, 18.0f, 8.0f, 16.0f, false, false },
    { "Side Grotto",                 55.0f,   1.0f, -2.5f,  8.0f, 6.0f, 8.0f, false, false },
};
constexpr uint32_t kDetCount = sizeof(kDetention) / sizeof(kDetention[0]);

// Door / connection pairs (room-index pairs into kDetention) — LA.FLOOR1_DOORS.
const uint32_t kDetDoors[] = {
    0,4, 1,4, 2,4, 3,4, 4,5, 4,6, 4,7, 4,8, 4,9, 4,10,
    0,18, 1,18, 1,19, 2,19, 2,20, 3,20, 3,21,
    5,12, 12,25, 25,26, 26,27, 27,28,
    0,11, 10,11,
    13,23, 14,23, 15,23, 16,23, 17,23,
    18,23, 19,23, 20,23, 21,23, 22,23,
    17,24, 22,24, 23,24,
};
constexpr uint32_t kDetDoorPairCount = (sizeof(kDetDoors) / sizeof(kDetDoors[0])) / 2;

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

// ---- Detention-room interior builder (Floor 1). Each room is an axis-aligned box;
// we build its 4 perimeter walls (running along Z at x=cx±w/2, and along X at z=cz±d/2)
// to the room's ceiling height. Where a CONNECTED neighbor (per the door list) abuts a
// given face (its box touches/overlaps that face's plane within the shared span), that
// wall gets a 1.2 m doorway gap centered on the overlap so the rooms link. The shared
// graybox helpers (addCrossWall / addWallX) handle the gap + lintel. Adjacent rooms
// each build their own wall (a thin double-wall on shared faces) — graybox-acceptable.

// True if rooms a,b are listed as connected in the detention door table.
bool detConnected(const uint32_t* doors, uint32_t pairs, uint32_t a, uint32_t b) {
    for (uint32_t i = 0; i < pairs; ++i) {
        uint32_t r0 = doors[2*i], r1 = doors[2*i+1];
        if ((r0 == a && r1 == b) || (r0 == b && r1 == a)) return true;
    }
    return false;
}

// The legacy Awakening-spine corridor carve-out (B1 only): the z=0 playable lane
// x in [carveX0,carveX1], z in [-carveZ,carveZ]. Detention room walls that fall inside
// this lane are suppressed / given a z=0 doorway so the corridor passes through any
// room in its way (Jake's Cell, Guard Station, Creepy Passage), keeping the Awakening
// route clear while the rooms' other walls stay. Set carveX1 < carveX0 to disable.
struct SpineCarve { float x0, x1, zHalf; };

// Build the 4 walls of one detention room `ri`, cutting a doorway on any face that a
// connected neighbor abuts. `floorY` is the room floor (world); `ceilH` the (clamped)
// ceiling height. `carve` suppresses walls inside the legacy spine lane (B1).
void buildDetentionRoom(Scene& scene, x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics,
                        const L1DetentionRoom* rooms, uint32_t roomCount,
                        const uint32_t* doors, uint32_t pairs, uint32_t ri,
                        float floorY, float ceilH,
                        x3::rhi::TextureHandle wallTex, const float tint[4], bool vis,
                        const SpineCarve& carve) {
    const L1DetentionRoom& r = rooms[ri];
    const float x0 = r.cx - r.w * 0.5f, x1 = r.cx + r.w * 0.5f;
    const float z0 = r.cz - r.d * 0.5f, z1 = r.cz + r.d * 0.5f;
    const bool carveOn = (carve.x1 > carve.x0);
    // Does this room's footprint overlap the spine lane (so the corridor runs through)?
    const bool inLane = carveOn && (x1 > carve.x0 && x0 < carve.x1 &&
                                    z1 > -carve.zHalf && z0 < carve.zHalf);
    // A Z-wall (at x=xFace) lies inside the lane x-span (so it would block the corridor).
    auto zWallInLane = [&](float xFace) {
        return carveOn && xFace > carve.x0 - 0.01f && xFace < carve.x1 + 0.01f;
    };
    // An X-wall (at z=zFace) lies inside the lane (so it would block the corridor).
    auto xWallInLane = [&](float zFace) {
        return carveOn && zFace > -carve.zHalf - 0.01f && zFace < carve.zHalf + 0.01f &&
               x1 > carve.x0 && x0 < carve.x1;
    };

    // For a face, find a connected neighbor abutting it and return the doorway center
    // coordinate (along the wall run) inside the overlap; returns false if no doorway.
    auto doorwayOnZWall = [&](float xFace, float& outZDoor) -> bool {  // wall at x=xFace (runs along Z)
        for (uint32_t j = 0; j < roomCount; ++j) {
            if (j == ri || !detConnected(doors, pairs, ri, j)) continue;
            const L1DetentionRoom& n = rooms[j];
            const float nx0 = n.cx - n.w * 0.5f, nx1 = n.cx + n.w * 0.5f;
            const float nz0 = n.cz - n.d * 0.5f, nz1 = n.cz + n.d * 0.5f;
            if (nx0 - 0.5f <= xFace && xFace <= nx1 + 0.5f) {     // neighbor spans this x plane
                const float lo = std::max(z0, nz0), hi = std::min(z1, nz1);
                if (hi - lo >= 1.3f) { outZDoor = (lo + hi) * 0.5f; return true; }
            }
        }
        return false;
    };
    auto doorwayOnXWall = [&](float zFace, float& outXDoor) -> bool {  // wall at z=zFace (runs along X)
        for (uint32_t j = 0; j < roomCount; ++j) {
            if (j == ri || !detConnected(doors, pairs, ri, j)) continue;
            const L1DetentionRoom& n = rooms[j];
            const float nx0 = n.cx - n.w * 0.5f, nx1 = n.cx + n.w * 0.5f;
            const float nz0 = n.cz - n.d * 0.5f, nz1 = n.cz + n.d * 0.5f;
            if (nz0 - 0.5f <= zFace && zFace <= nz1 + 0.5f) {     // neighbor spans this z plane
                const float lo = std::max(x0, nx0), hi = std::min(x1, nx1);
                if (hi - lo >= 1.3f) { outXDoor = (lo + hi) * 0.5f; return true; }
            }
        }
        return false;
    };

    float dCoord;
    // -X / +X walls (run along Z). If the wall sits inside the spine lane, SUPPRESS it
    // entirely — the legacy spine's own partition walls (at the door X positions) gate
    // the Awakening corridor, so a doubled detention Z-wall here would just block it.
    auto buildZWall = [&](float xFace) {
        if (zWallInLane(xFace))
            return;
        else if (doorwayOnZWall(xFace, dCoord))
            addCrossWall(scene, device, physics, xFace, z0, z1, dCoord, true, floorY, ceilH, wallTex, tint, vis);
        else
            addCrossWall(scene, device, physics, xFace, z0, z1, 0.0f, false, floorY, ceilH, wallTex, tint, vis);
    };
    buildZWall(x0);
    buildZWall(x1);
    // -Z / +Z walls (run along X). addWallX has no doorway variant; split manually. If
    // the wall lies inside the spine lane, suppress the segment within the lane x-span
    // (the corridor's own long walls confine it) so the corridor runs through.
    auto wallXWithDoor = [&](float zFace) {
        const bool laneCut = xWallInLane(zFace);
        float xd; bool haveDoor = doorwayOnXWall(zFace, xd);
        // Build a sub-segment [a,b] of this X-wall, skipping the lane carve-out.
        auto seg = [&](float a, float b) {
            if (b - a < 0.05f) return;
            if (laneCut) {
                const float cl = std::max(a, carve.x0), cr = std::min(b, carve.x1);
                if (cr > cl) {   // overlaps the lane: build the parts outside it
                    if (carve.x0 > a) addWallX(scene, device, physics, a, carve.x0, zFace, floorY, ceilH, wallTex, tint, vis);
                    if (b > carve.x1) addWallX(scene, device, physics, carve.x1, b, zFace, floorY, ceilH, wallTex, tint, vis);
                    return;
                }
            }
            addWallX(scene, device, physics, a, b, zFace, floorY, ceilH, wallTex, tint, vis);
        };
        if (haveDoor) {
            seg(x0, xd - kDoorHalf);
            seg(xd + kDoorHalf, x1);
            const float lh = (ceilH - kLintelBottom) * 0.5f;
            if (lh > 0.0f)
                addBox(scene, device, physics, kDoorHalf, lh, kWallT * 0.5f, xd,
                       floorY + kLintelBottom + lh, zFace, wallTex, tint,
                       (uint32_t)Tag::Static, true, vis);
        } else {
            seg(x0, x1);
        }
    };
    wallXWithDoor(z0);
    wallXWithDoor(z1);
    (void)inLane;
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
    // 1) FLOOR PLATES — for each floor: floor slab, the 4 SOLID perimeter walls (two
    //    long side walls along X at z=±zHalf, plus the -X and +X end caps), and a
    //    ceiling lid. The plate is the real ~75x43 m detention footprint (grown from
    //    the old 24x16 placeholder). The elevator shaft is now an INTERIOR free-
    //    standing column (built in §2) at (kShaftCx,kShaftCz) with its own -X doorway
    //    opening straight into the plate, so the perimeter end caps are fully solid.
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
        // -X and +X end caps (now fully SOLID perimeter walls — the shaft is interior).
        addCrossWall(scene, device, physics, f.x0, -f.zHalf, f.zHalf, 0.0f,
                     /*withDoorway*/false, f.y0, f.ceil, wallTex, kWallTint, crossWallVis);
        addCrossWall(scene, device, physics, f.x1, -f.zHalf, f.zHalf, 0.0f,
                     /*withDoorway*/false, f.y0, f.ceil, wallTex, kWallTint, crossWallVis);
        // Ceiling lid (skip the rooftop: F7 is open to the sky). Collision-only.
        if (!isRooftop)
            addCeiling(scene, device, physics, cx, 0.0f, (f.x1 - f.x0) * 0.5f, f.zHalf,
                       f.y0 + f.ceil, floorTex);

        // Fill the per-floor layout result.
        L.floorBaseY[fi]  = f.y0;
        L.floorCeil[fi]   = f.ceil;
        L.floorCenter[fi] = x3::phys::Vec3{ cx, f.y0, 0.0f };
        // Elevator doorway: the shaft's -X face at z=0 on this floor (consistent XZ up
        // the whole tower so a ride always lands on walkable floor at the shaft mouth).
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
    // 3) EMERGENCY STAIRWELL — a straight ramp column linking every adjacent pair of
    //    floors. Each 5 m rise is a straight stair of 10 stepped boxes (0.5 m rise
    //    each). Placed in a NORTH BAND (x in [10,14], z=15) that is inside EVERY floor's
    //    plate — B1's big detention plate (rooms all at z<=9.5) AND the resized F2-F7
    //    plates (zHalf>=22) — and clear of the elevator shaft (x=21,z=0) + all encounter
    //    content (z in [-6.5,6.5]). (Was x in [30,34],z=20, which fell OUTSIDE the
    //    resized F2-F7 plates.) Purely collision graybox + tint.
    // ===================================================================
    {
        const float stairX0 = 10.0f, stairX1 = 14.0f;    // stair well footprint (X) — north band, inside every plate
        const float stairZ  = 15.0f;                     // +Z north band: clear of detention (z<=9.5) + content (|z|<=6.5)
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
    // 4) F2-F7 PER-FLOOR IDENTITY INTERIORS (collision graybox). Each floor now has its
    //    OWN real-scale plate (kFloors). The EASTERN third (x in [-2,25]) is kept OPEN as
    //    the arrival + combat hall where the spire_mid/top encounters play (content is
    //    authored at absolute x in [0,18]); the WESTERN space (x < -2) is partitioned into
    //    a wing of identity rooms from the LevelArchitect room vocabulary (cells/labs
    //    ~6-8 m, a big signature hall, a large open bay for the drone floor). Rooms open
    //    onto the central west corridor (the z in [-3,3] lane, itself open at its east end
    //    into the arrival hall), so every room is reachable and nothing is sealed.
    // ===================================================================
    // One graybox room: 4 floor-to-ceiling walls with an optional 1.2 m doorway on ONE
    // side ('W'=-X, 'E'=+X, 'S'=-Z, 'N'=+Z; 0 = sealed). Absolute world coords. Reuses
    // the shared wall helpers (addCrossWall handles the Z-running walls + doorway/lintel;
    // the X-running walls are split manually for a doorway, mirroring buildDetentionRoom).
    auto roomBox = [&](float rx0, float rx1, float rz0, float rz1, float ry0, float rh, char door) {
        const float zc = (rz0 + rz1) * 0.5f, xc = (rx0 + rx1) * 0.5f;
        addCrossWall(scene, device, physics, rx0, rz0, rz1, zc, door == 'W', ry0, rh, wallTex, kWallTint, crossWallVis);
        addCrossWall(scene, device, physics, rx1, rz0, rz1, zc, door == 'E', ry0, rh, wallTex, kWallTint, crossWallVis);
        auto wallXDoor = [&](float zf, bool d) {
            if (!d) { addWallX(scene, device, physics, rx0, rx1, zf, ry0, rh, wallTex, kWallTint, crossWallVis); return; }
            addWallX(scene, device, physics, rx0, xc - kDoorHalf, zf, ry0, rh, wallTex, kWallTint, crossWallVis);
            addWallX(scene, device, physics, xc + kDoorHalf, rx1, zf, ry0, rh, wallTex, kWallTint, crossWallVis);
            const float lh = (rh - kLintelBottom) * 0.5f;
            if (lh > 0.0f)
                addBox(scene, device, physics, kDoorHalf, lh, kWallT * 0.5f, xc, ry0 + kLintelBottom + lh, zf,
                       wallTex, kWallTint, (uint32_t)Tag::Static, true, crossWallVis);
        };
        wallXDoor(rz0, door == 'S');
        wallXDoor(rz1, door == 'N');
    };
    // A floor's WEST wing: a set of identity rooms, each centered at (cx,cz) with
    // half-extents (hw,hd) + a doorway side, built at the floor's y0 + ceiling height.
    struct WingRoom { float cx, cz, hw, hd; char door; };
    auto buildWing = [&](L1Floor fl, const std::vector<WingRoom>& rooms) {
        const L1RoomDef& f = kFloors[(uint32_t)fl];
        for (const WingRoom& r : rooms)
            roomBox(r.cx - r.hw, r.cx + r.hw, r.cz - r.hd, r.cz + r.hd, f.y0, f.ceil, r.door);
    };

    // ---- F2 Medical Bay: 3 ward markers (Aria/Keisha/Emily) in the east hall (rescue
    //      hub places victims here) + a west wing: big Recovery Ward + Medbay/Lab/Cure
    //      Lab/Observation rooms.
    {
        const L1RoomDef& f2 = kFloors[(uint32_t)L1Floor::F2];
        L.wardA = x3::phys::Vec3{  4.0f, f2.y0, -3.0f };  // Ward A (Aria)
        L.wardB = x3::phys::Vec3{ 11.5f, f2.y0,  3.0f };  // Ward B (Keisha)
        L.wardC = x3::phys::Vec3{ 18.0f, f2.y0, -3.0f };  // Ward C (Emily)
    }
    buildWing(L1Floor::F2, {
        { -38.0f,  0.0f, 10.0f, 14.0f, 'E' },  // Recovery Ward (big signature hall)
        { -18.0f,  8.0f,  6.0f,  5.0f, 'S' },  // Medbay
        { -18.0f, -8.0f,  6.0f,  5.0f, 'N' },  // Surgery / Lab
        {  -7.0f,  8.0f,  4.0f,  5.0f, 'S' },  // Cure Lab
        {  -7.0f, -8.0f,  4.0f,  5.0f, 'N' },  // Observation
    });
    // ---- F3 Genetics Lab: gene-vat hall + clone/cure labs + research office (FE-#7
    //      arena stays in the east hall).
    buildWing(L1Floor::F3, {
        { -38.0f,  0.0f, 10.0f, 14.0f, 'E' },  // Gene-Vat Hall (big)
        { -18.0f,  8.0f,  6.0f,  5.0f, 'S' },  // Clone Lab
        { -18.0f, -8.0f,  6.0f,  5.0f, 'N' },  // Cure Lab
        {  -7.0f,  8.0f,  4.0f,  5.0f, 'S' },  // Specimen Storage
        {  -7.0f, -8.0f,  4.0f,  5.0f, 'N' },  // Research Office
    });
    // ---- F4 Cybernetics Workshop: server room + augmentation bays + a (flavor) Nexus
    //      connector room (the real F4->4.5 hook is at x=1.5 in the east, set by spire_mid).
    buildWing(L1Floor::F4, {
        { -40.0f,  0.0f, 11.0f, 15.0f, 'E' },  // Server Room (big)
        { -18.0f,  9.0f,  6.0f,  6.0f, 'S' },  // Augmentation Bay
        { -18.0f, -9.0f,  6.0f,  6.0f, 'N' },  // Augmentation Bay 2
        {  -7.0f,  9.0f,  4.0f,  5.0f, 'S' },  // Nexus Connector (to Floor 4.5)
        {  -7.0f, -9.0f,  4.0f,  5.0f, 'N' },  // Control Room
    });
    // ---- F5 Drone Manufacturing: one LARGE open assembly bay + a couple of control
    //      rooms (the Swarm-AI arena stays in the east hall). The bay is the floor's
    //      signature: a big hangar, not many small rooms.
    buildWing(L1Floor::F5, {
        { -44.0f,  0.0f, 26.0f, 26.0f, 'E' },  // Assembly Bay (huge open hangar)
        {  -8.0f, 12.0f,  5.0f,  6.0f, 'S' },  // Drone Control
        {  -8.0f,-12.0f,  5.0f,  6.0f, 'N' },  // Power Plant
    });
    // ---- F6 Alien Technology Lab: containment hall + archive vault + observation + tech
    //      lab (the Alien-Overseer arena + the 2 keypad doors stay in the east hall).
    buildWing(L1Floor::F6, {
        { -42.0f,  0.0f, 13.0f, 18.0f, 'E' },  // Containment Hall (big)
        { -18.0f, 10.0f,  6.0f,  6.0f, 'S' },  // Archive Vault
        { -18.0f,-10.0f,  6.0f,  6.0f, 'N' },  // Observation
        {  -7.0f, 10.0f,  4.0f,  5.0f, 'S' },  // Tech Lab
        {  -7.0f,-10.0f,  4.0f,  5.0f, 'N' },  // Specimen Bay
    });
    // ---- F7 Executive Laboratory: boardroom + exec offices + archive + server (the
    //      Clone/Sarah finale stays in the east hall; the open helipad center is recorded).
    buildWing(L1Floor::F7, {
        { -40.0f,  0.0f, 11.0f, 15.0f, 'E' },  // Boardroom (big)
        { -18.0f,  8.0f,  6.0f,  5.0f, 'S' },  // Executive Office
        { -18.0f, -8.0f,  6.0f,  5.0f, 'N' },  // Executive Office 2
        {  -7.0f,  8.0f,  4.0f,  5.0f, 'S' },  // Archive
        {  -7.0f, -8.0f,  4.0f,  5.0f, 'N' },  // Server
    });
    // ---- F6 Executive holding office (Sarah's 4th-rescue marker) in a -Z pocket of the
    //      east hall + F7 rooftop finale-arena center (the open eastern plate). ----
    {
        const L1RoomDef& f6 = kFloors[(uint32_t)L1Floor::F6];
        const float y0 = f6.y0, h = f6.ceil;
        addWallX(scene, device, physics, 0.0f, 8.0f, -3.0f, y0, h, wallTex, kWallTint, crossWallVis);
        addCrossWall(scene, device, physics, 8.0f, -8.0f, -3.0f, -5.5f, true, y0, h,
                     wallTex, kWallTint, crossWallVis);
        L.execOffice = x3::phys::Vec3{ 4.0f, y0, -5.5f };
    }
    {
        const L1RoomDef& f7 = kFloors[(uint32_t)L1Floor::F7];
        L.rooftopCenter = x3::phys::Vec3{ 10.0f, f7.y0, 0.0f };  // finale arena (east hall, by the Clone at x=8)
    }

    // ===================================================================
    // 5a) FLOOR-1 DETENTION INTERIOR (the authoritative LevelArchitect layout). The
    //    29 rooms are built on the B1 plate (Jake's spawn) from kDetention/kDetDoors:
    //    each room's 4 walls with 1.2 m doorway gaps on every face a CONNECTED neighbor
    //    abuts (the door pairs). 3 cell blocks (Jake's 0-3, west 13-17, mid 18-22) hang
    //    off the Main Hallway (4) + the Cell Block B Hallway (23); the support rooms
    //    (Guard Station 5 / Storage 6 / Medical Bay 7 / Armory 8) + the Elevator Lobby
    //    (9) line the east of the main hall; the eastern arm runs Guard Station ->
    //    Creepy Passage (12) -> Descending Stairs (25) -> Cave Tunnel (26) -> Crystal
    //    Cavern (27) -> Side Grotto (28) — the Act-1 -> caves bridge. Cave-arm rooms dip
    //    below the plate base but are CLAMPED above the y=-5 sub-levels and their
    //    ceilings clamped under the plate top so nothing pokes into F1 / collides with
    //    SL1. Built as collision graybox + the env_art GLB tiling overlays it.
    // ===================================================================
    const float b1y    = kFloors[(uint32_t)L1Floor::B1].y0;
    const float b1ceil = kFloors[(uint32_t)L1Floor::B1].ceil;
    const float plateTop = b1y + b1ceil;     // detention ceilings clamp under this
    {
        const float detTint[4] = { 0.50f, 0.55f, 0.68f, 1.0f };   // detention concrete
        // The legacy Awakening-spine corridor carve (x in [0,19.5], z in [-4,4]) so the
        // playable z=0 lane punches through any detention room in its way.
        const SpineCarve carve{ 0.0f, 19.5f, 4.0f };
        for (uint32_t ri = 0; ri < kDetCount; ++ri) {
            const L1DetentionRoom& r = kDetention[ri];
            const float floorY = b1y + r.floorY;
            // Per-room ceiling: the authored h, clamped so the room top never pokes
            // above the B1 plate top (avoids clipping F1's floor at y=5 / the lid).
            float ceilH = r.h;
            if (floorY + ceilH > plateTop - 0.3f) ceilH = (plateTop - 0.3f) - floorY;
            if (ceilH < 2.4f) ceilH = 2.4f;   // keep head clearance even when clamped
            // A floor slab for cave-arm rooms that dip below the plate base (the rest
            // share the B1 plate slab). Avoid a second slab at y=b1y (z-fight the plate).
            if (r.floorY < -0.01f)
                addFloor(scene, device, physics, r.cx, r.cz, r.w * 0.5f, r.d * 0.5f, floorY,
                         floorTex, detTint, floorVis);
            buildDetentionRoom(scene, device, physics, kDetention, kDetCount,
                               kDetDoors, kDetDoorPairCount, ri, floorY, ceilH,
                               wallTex, detTint, crossWallVis, carve);
        }
    }

    // ===================================================================
    // 5b) B1 LEGACY AWAKENING SPINE + props — the existing §3 beats. Re-anchored as a
    //    self-contained walled CORRIDOR along z=0, x in [0,19.5], confined to a narrow
    //    z in [-4,4] band (so its partition cross-walls do NOT slice the detention cell
    //    blocks at z<-4). It runs from Jake's Cell out toward the elevator shaft. The
    //    legacy door/room accessors map onto this spine so level1_game.cpp keeps
    //    building + the Awakening beat sequence (cell -> corridor -> armory ->
    //    checkpoint -> arena -> elevator) plays unchanged. The spine is the playable
    //    route; the §5a detention rooms are the authored complex around it.
    // ===================================================================
    const float kSpineZ = 4.0f;   // half-depth of the Awakening corridor lane
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

    // B1 spine zones laid left->right, ending in a real arena in front of the elevator
    // shaft (x>=19.5) so Martinez has clearance (not inside the shaft). Doors A-D
    // partition the sub-rooms; Door E is the shaft doorway.
    L.doorA = x3::phys::Vec3{  5.0f, b1y, 0.0f };
    L.doorB = x3::phys::Vec3{  9.0f, b1y, 0.0f };
    L.doorC = x3::phys::Vec3{ 12.5f, b1y, 0.0f };
    L.doorD = x3::phys::Vec3{ 15.0f, b1y, 0.0f };
    L.doorE = L.elevatorDoor[(uint32_t)L1Floor::B1];  // arena -> elevator shaft (x=19.5)

    L.cellCenter       = x3::phys::Vec3{  3.0f, b1y, 0.0f };
    L.corridorCenter   = x3::phys::Vec3{  7.0f, b1y, 0.0f };
    L.armoryCenter     = x3::phys::Vec3{ 11.0f, b1y, 0.0f };
    L.checkpointCenter = x3::phys::Vec3{ 13.7f, b1y, 0.0f };
    L.arenaCenter      = x3::phys::Vec3{ 17.5f, b1y, 0.0f };

    // ---- B1 spine corridor walls: the two long lane walls at z=±kSpineZ (x in
    //      [0,19.5]) confine the Awakening encounters to the z=0 lane (so the alarm
    //      enemies can't roam into the later checkpoint fight, and the firing rays
    //      along z=0 hit only the intended group), plus cross-wall partitions (with a
    //      1.2 m doorway gap at z=0) at each spine door X. The DoorSystem slab fills
    //      each z=0 gap. The lane walls span only ±kSpineZ so they leave the detention
    //      cell blocks (at |z|>4) intact. ----
    {
        const float bh = b1ceil;
        addWallX(scene, device, physics, 0.0f, 19.5f, -kSpineZ, b1y, bh, wallTex, kWallTint, crossWallVis);
        addWallX(scene, device, physics, 0.0f, 19.5f,  kSpineZ, b1y, bh, wallTex, kWallTint, crossWallVis);
        const float partX[4] = { L.doorA.x, L.doorB.x, L.doorC.x, L.doorD.x };
        for (float px : partX)
            addCrossWall(scene, device, physics, px, -kSpineZ, kSpineZ, 0.0f, true, b1y, bh,
                         wallTex, kWallTint, crossWallVis);
    }

    L.cellHalf       = x3::phys::Vec3{ 3.0f, b1ceil, kSpineZ };
    L.corridorHalf   = x3::phys::Vec3{ 2.5f, b1ceil, kSpineZ };
    L.armoryHalf     = x3::phys::Vec3{ 2.0f, b1ceil, kSpineZ };
    L.checkpointHalf = x3::phys::Vec3{ 2.0f, b1ceil, kSpineZ };
    L.arenaHalf      = x3::phys::Vec3{ 2.0f, b1ceil, kSpineZ };

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

// Floor-1 detention table accessors (shared with the self-test).
const L1DetentionRoom* level1DetentionRooms()       { return kDetention; }
uint32_t               level1DetentionRoomCount()   { return kDetCount; }
const uint32_t*        level1DetentionDoors()        { return kDetDoors; }
uint32_t               level1DetentionDoorPairCount(){ return kDetDoorPairCount; }

// The bounding box of all 29 detention rooms (XZ, meters) — the authored ~75x43 m
// Floor-1 footprint (distinct from the larger raw B1 plate).
L1Footprint level1DetentionFootprint() {
    L1Footprint fp{ 1e9f, -1e9f, 1e9f, -1e9f };
    for (uint32_t i = 0; i < kDetCount; ++i) {
        const L1DetentionRoom& r = kDetention[i];
        fp.minX = std::min(fp.minX, r.cx - r.w * 0.5f);
        fp.maxX = std::max(fp.maxX, r.cx + r.w * 0.5f);
        fp.minZ = std::min(fp.minZ, r.cz - r.d * 0.5f);
        fp.maxZ = std::max(fp.maxZ, r.cz + r.d * 0.5f);
    }
    return fp;
}

} // namespace x3::game
