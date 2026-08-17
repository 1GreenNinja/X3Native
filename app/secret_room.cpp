// CODE-LOCKED TRAPDOOR -> SECRET ROOM. See app/secret_room.h.
//
// Clean-room: built from the Scene/Door/Weapon/HoloTerminal systems + the engine
// interfaces only. No purchased C# / id Tech source consulted.
#include "secret_room.h"
#include "level1.h"        // shared cell-hatch cutout constants (kCellHatch*)
#include "level1_game.h"   // integrated S7: the real Level1 carves the B1 floor hole
#include "mesh_prims.h"
#include "headless_device.h"

#include "engine/core/x3_log.h"

#include <cmath>
#include <memory>
#include <string>

namespace x3::game {

namespace {
// ---- Secret-room geometry (relative to the cell). The cell floor sits at
// cellCenter.y; the room is dug straight DOWN below the hatch. ----
constexpr float kRoomDrop      = 6.0f;   // secret-room floor is 6 m below the cell floor
constexpr float kRoomCeilGap   = 0.25f;  // room ceiling sits just below the cell floor (open shaft above)
constexpr float kRoomHalfX     = 5.0f;   // room half-extent X (10 m wide)
constexpr float kRoomHalfZ     = 4.0f;   // room half-extent Z (8 m deep)
constexpr float kWallT         = 0.2f;   // wall thickness (matches level graybox)

// A flat-color collision box (level-style). hx/hy/hz half-extents, centered at
// (cx,cy,cz). Returns the entity id. `collide` adds static physics; `emissive`
// adds an HDR glow term (for screens / loot).
uint32_t addBox(Scene& scene, x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics,
                float hx, float hy, float hz, float cx, float cy, float cz,
                float r, float g, float b, float a, bool collide,
                float er = 0, float eg = 0, float eb = 0, float es = 0,
                uint32_t tag = (uint32_t)Tag::Static) {
    x3::prims::PrimMesh geo = x3::prims::makeBox(hx, hy, hz, 0, 0, 0, 0.5f);
    Entity e;
    e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                               geo.index.data(), (uint32_t)geo.index.size());
    e.baseColor[0]=r; e.baseColor[1]=g; e.baseColor[2]=b; e.baseColor[3]=a;
    e.emissive[0]=er; e.emissive[1]=eg; e.emissive[2]=eb; e.emissive[3]=es;
    e.tag = tag;
    e.transform[12]=cx; e.transform[13]=cy; e.transform[14]=cz;
    if (collide)
        e.body = physics.addBox(x3::phys::Vec3{hx, hy, hz}, x3::phys::Vec3{cx, cy, cz},
                                0.0f, x3::phys::Layer::Static);
    return scene.add(e);
}
} // namespace

uint32_t SecretRoom::addScreen(Scene& scene, x3::rhi::IRenderDevice& device,
                               const x3::phys::Vec3& center, float yaw,
                               float w, float h, float r, float g, float b, float strength) {
    // A thin emissive panel on a wall, yaw-rotated so it lies flat against the wall.
    // (Thin in local Z; yaw turns local Z toward the wall normal.) Translucent base
    // + strong HDR emissive => a glowing sci-fi display, NOT a flat solid quad.
    const float cs = std::cos(yaw), sn = std::sin(yaw);
    const float hw = w * 0.5f, hh = h * 0.5f, ht = 0.03f;
    x3::prims::PrimMesh geo = x3::prims::makeBox(hw, hh, ht, 0, 0, 0, 0.5f);
    Entity e;
    e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                               geo.index.data(), (uint32_t)geo.index.size());
    e.baseColor[0]=r*0.25f; e.baseColor[1]=g*0.25f; e.baseColor[2]=b*0.25f; e.baseColor[3]=0.55f;
    e.emissive[0]=r; e.emissive[1]=g; e.emissive[2]=b; e.emissive[3]=strength;
    e.tag = (uint32_t)Tag::Prop;
    e.transform[0]=cs;  e.transform[2]=-sn;
    e.transform[8]=sn;  e.transform[10]=cs;
    e.transform[12]=center.x; e.transform[13]=center.y; e.transform[14]=center.z;
    ++m_screenCount;
    return scene.add(e);
}

uint32_t SecretRoom::addPickup(Scene& scene, x3::rhi::IRenderDevice& device,
                               SecretPickup::Kind kind, const x3::phys::Vec3& pos,
                               int healAmount, float r, float g, float b, float strength,
                               float halfSize) {
    // A glowing collectible prop (visual only — no collision, so the player walks
    // into it). Emissive so it reads as loot/tech. Collected by SecretRoom::tick().
    // Built inline (no physics body for a free-floating collectible).
    x3::prims::PrimMesh geo = x3::prims::makeBox(halfSize, halfSize, halfSize, 0, 0, 0, 0.5f);
    Entity e;
    e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                               geo.index.data(), (uint32_t)geo.index.size());
    e.baseColor[0]=r; e.baseColor[1]=g; e.baseColor[2]=b; e.baseColor[3]=1.0f;
    e.emissive[0]=r; e.emissive[1]=g; e.emissive[2]=b; e.emissive[3]=strength;
    e.tag = (uint32_t)Tag::Prop;
    e.transform[12]=pos.x; e.transform[13]=pos.y; e.transform[14]=pos.z;
    uint32_t id = scene.add(e);

    SecretPickup p;
    p.kind = kind; p.entity = id; p.pos = pos; p.healAmount = healAmount;
    m_pickups.push_back(p);
    return id;
}

void SecretRoom::build(Scene& scene, x3::rhi::IRenderDevice& device,
                       x3::phys::IPhysicsWorld& physics, DoorSystem& doors,
                       const x3::phys::Vec3& cellCenter, std::string_view modelDir,
                       float hatchCx, float hatchCz, float cellCeilY) {
    m_doorsPtr = &doors;

    // Resolve the relocation params (NaN / <=floor => legacy values), so the legacy
    // Level-1 tower call sites stay byte-identical while --world canonlevel can seat
    // the feature at the canon cell's coordinates.
    if (std::isnan(hatchCx)) hatchCx = kCellHatchCx;
    if (std::isnan(hatchCz)) hatchCz = kCellHatchCz;
    const float termCeilY = (cellCeilY > cellCenter.y) ? cellCeilY : (cellCenter.y + 2.8f);

    // ====================================================================
    // 1) THE FLOOR-HATCH TRAPDOOR in the cell floor (starts LOCKED).
    // ====================================================================
    // Place it at the shared cell-hatch cutout location (legacy: level1.h kCellHatch*;
    // canon: the caller's hatchCx/hatchCz, matched to the hole the canon floor builder
    // carved), so the hatch slab lines up exactly with the hole in the floor — an open
    // hatch then drops the player straight through. Only the Y comes from cellCenter
    // (the cell floor); the XZ is the world cutout center. It slides aside along +X by
    // its full width (2*halfWidth) to clear the opening.
    const x3::phys::Vec3 hatchCenter{ hatchCx, cellCenter.y, hatchCz };
    {
        DoorSpec h;
        h.doorwayCenter = hatchCenter;
        h.floorHatch    = true;
        h.halfWidth     = kCellHatchHalf;
        h.thickness     = 0.1f;        // thin flush panel (matches the 0.1 floor slab top)
        h.duration      = 1.2f;
        h.withButton    = false;       // opened by the terminal code, not a wall button
        h.locked        = true;        // ONLY the override code opens it
        h.code          = 1278;        // the override code (matches kSecretRoomCode; 1127 is taken)
        h.tint[0]=0.30f; h.tint[1]=0.34f; h.tint[2]=0.40f; h.tint[3]=1.0f;  // steel fallback
        // FLUSH + FLOOR-TEXTURED: generate the SAME cell-floor grate texture + B1 tint
        // the level uses (level1.cpp makeFloorGrateRGBA, kTexN=512, B1 cool-blue tint)
        // so the two parting panels read as the cell floor, not a grey block.
        constexpr uint32_t kHatchTexN = 512;
        // R7: hazard=true — the yellow/black caution band bakes around each panel's UV
        // edge, so each panel carries its own outline and the two inner edges meet as a
        // striped CENTER SEAM ("parts here"), a read that TRAVELS with the panels when
        // they slide (no extra entities). Tint lifted 0.55 -> 0.72: the hatch sits in
        // the cell's pooled shadow and at 0.55 the panels rendered DARKER than the deck
        // around them — an unreadable black patch (R6 review shots).
        // D22 follow-through (QA upper-floors sweep): the deck map is generated with
        // level1's neutral value lift, so the hatch keeps its authored ~1.3x-brighter-
        // than-the-deck ratio. Without it the lifted deck would leave the panels 5x
        // darker than the floor around them — the exact "unreadable black patch" the R6
        // review fixed, just from the other direction.
        auto floorPx = x3::prims::makeFloorGrateRGBA(kHatchTexN, /*tiles*/2,
                                                     x3::game::level1DeckMapLift(), /*hazard*/true);
        h.floorTex = device.createTexture(floorPx.data(), kHatchTexN, kHatchTexN, true);
        h.floorTint[0]=0.72f; h.floorTint[1]=0.78f; h.floorTint[2]=0.95f; h.floorTint[3]=1.0f;
        m_hatchIdx = buildLevelDoor(scene, doors, device, physics, h);
    }

    // ---- AAA HATCH READ (R6): a raised industrial-yellow RIM framing the opening +
    // a status light. Without these the flush panels read as floor (nobody finds the
    // trapdoor); with them it reads as an engineered, code-locked deck hatch. The rim
    // is four low curb boxes AROUND the opening (outside the panels' slide path — the
    // panels part along X UNDER the rim ends), hazard yellow with a faint self-glow so
    // it reads in the cell's pooled shadow. The status light sits on the -Z rim corner:
    // RED while locked, flipped GREEN by tick() the frame the hatch unlocks/opens.
    // R8 (owner review 2026-08-16: "a trap door frame in the middle of the cell? That
    // is pure 3 year old block crayon slop"): the raised hazard-YELLOW curb rim +
    // glowing status CUBE are gone. A maintenance hatch is MACHINED INTO the deck, not
    // placed on it: the read is now a thin, near-flush, dark-gunmetal frame trim — two
    // nested low rings (outer 8 mm, inner 16 mm proud: a stepped chamfer profile) with
    // the floor-textured panels continuing the deck inside it, so the hatch is
    // discovered by its clean machined seam line, not by a painted block. The status
    // LENS survives (gameplay: tick() flips it red -> green on unlock) but is a small
    // recessed dot sunk into the trim ring corner, not a glowing brick.
    {
        const float ho    = kCellHatchHalf;         // opening half-extent
        // Dark worn gunmetal, no glow: the frame catches the hatch downlight
        // (cell_dressing) and reads as metal; discovery comes from the seam geometry.
        const float steel[4] = { 0.105f, 0.110f, 0.120f, 1.0f };
        auto ring = [&](float hx, float hz, float ox, float oz, float h) {
            x3::prims::PrimMesh geo = x3::prims::makeBox(hx, h, hz, 0, 0, 0, 1.0f);
            Entity e;
            e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                                       geo.index.data(), (uint32_t)geo.index.size());
            e.baseColor[0]=steel[0]; e.baseColor[1]=steel[1]; e.baseColor[2]=steel[2]; e.baseColor[3]=1.0f;
            e.tag = (uint32_t)Tag::Prop;
            // Base 5 mm above the floor plane (never coplanar with the slab top or the
            // panels sliding underneath).
            e.transform[12]=hatchCenter.x+ox; e.transform[13]=cellCenter.y+h+0.005f;
            e.transform[14]=hatchCenter.z+oz;
            scene.add(e);
        };
        // A full ring = two long bars along X (+/-Z sides, spanning the corners) + two
        // short bars along Z between them. Outer ring: wide + almost flush (the frame
        // plate let into the deck). Inner ring: narrower + a step taller (the seat lip
        // the lid closes onto) — the step is the machined chamfer read.
        auto fullRing = [&](float inner, float w, float h) {
            ring(inner + w, w * 0.5f, 0.0f, -(inner + w * 0.5f), h);
            ring(inner + w, w * 0.5f, 0.0f,  (inner + w * 0.5f), h);
            ring(w * 0.5f, inner, -(inner + w * 0.5f), 0.0f, h);
            ring(w * 0.5f, inner,  (inner + w * 0.5f), 0.0f, h);
        };
        fullRing(ho + 0.05f, 0.07f, 0.008f);        // outer frame plate, 8 mm proud
        fullRing(ho,         0.05f, 0.016f);        // inner seat lip, 16 mm proud
        // STATUS LENS: a low 4 cm dot recessed into the +Z trim bar's -X corner (faces
        // the room; the -Z corner is occluded by the code console's foot). Same
        // entity contract as before — tick() flips emissive red -> green.
        {
            x3::prims::PrimMesh geo = x3::prims::makeBox(0.040f, 0.014f, 0.040f, 0, 0, 0, 1.0f);
            Entity e;
            e.mesh = device.createMesh(geo.verts.data(), (uint32_t)geo.verts.size(),
                                       geo.index.data(), (uint32_t)geo.index.size());
            e.baseColor[0]=0.12f; e.baseColor[1]=0.05f; e.baseColor[2]=0.05f; e.baseColor[3]=1.0f;
            e.emissive[0]=1.0f; e.emissive[1]=0.08f; e.emissive[2]=0.05f; e.emissive[3]=0.9f;  // locked RED
            e.tag = (uint32_t)Tag::Prop;
            e.transform[12]=hatchCenter.x-(ho+0.02f); e.transform[13]=cellCenter.y+0.030f;
            e.transform[14]=hatchCenter.z+(ho+0.085f);
            m_statusEnt = scene.add(e);
        }
    }

    // ====================================================================
    // 2) THE SECRET ROOM below the cell (reached by dropping through the hatch).
    // ====================================================================
    const float cellFloorY = cellCenter.y;
    m_roomFloorY = cellFloorY - kRoomDrop;                 // 6 m down
    const float ceilY = cellFloorY - kRoomCeilGap;         // room ceiling just below the cell floor
    const float roomH = ceilY - m_roomFloorY;              // wall height
    m_roomCenter = x3::phys::Vec3{ hatchCenter.x, m_roomFloorY, hatchCenter.z };
    const float cx = m_roomCenter.x, cz = m_roomCenter.z;

    // Distinct dark sci-fi vault tint for the structure.
    const float wallR=0.12f, wallG=0.13f, wallB=0.16f;

    // Floor slab (top flush with m_roomFloorY).
    addBox(scene, device, physics, kRoomHalfX, 0.1f, kRoomHalfZ, cx, m_roomFloorY - 0.1f, cz,
           0.10f,0.11f,0.13f,1.0f, /*collide*/true);
    // Ceiling lid WITH a hole under the hatch: build it as 4 segments around the
    // hatch opening so a body dropping through the hatch falls into the room (the
    // open hatch + this hole line up at hatchCenter). Hatch opening half = kCellHatchHalf.
    {
        const float ho = kCellHatchHalf;      // hatch half-opening (relative to hatchCenter == cx,cz)
        const float ct = 0.1f;                // ceiling slab half-thickness
        const float cyl = ceilY + ct;         // ceiling slab center Y
        // -X segment (x: cx-kRoomHalfX .. cx-ho)
        if (kRoomHalfX > ho) {
            float a = cx - kRoomHalfX, b = cx - ho;
            addBox(scene, device, physics, (b-a)*0.5f, ct, kRoomHalfZ, (a+b)*0.5f, cyl, cz,
                   0.10f,0.11f,0.13f,1.0f, true);
        }
        // +X segment (x: cx+ho .. cx+kRoomHalfX)
        if (kRoomHalfX > ho) {
            float a = cx + ho, b = cx + kRoomHalfX;
            addBox(scene, device, physics, (b-a)*0.5f, ct, kRoomHalfZ, (a+b)*0.5f, cyl, cz,
                   0.10f,0.11f,0.13f,1.0f, true);
        }
        // -Z segment over the central band (x in [cx-ho,cx+ho], z: cz-kRoomHalfZ .. cz-ho)
        if (kRoomHalfZ > ho) {
            float a = cz - kRoomHalfZ, b = cz - ho;
            addBox(scene, device, physics, ho, ct, (b-a)*0.5f, cx, cyl, (a+b)*0.5f,
                   0.10f,0.11f,0.13f,1.0f, true);
        }
        // +Z segment over the central band
        if (kRoomHalfZ > ho) {
            float a = cz + ho, b = cz + kRoomHalfZ;
            addBox(scene, device, physics, ho, ct, (b-a)*0.5f, cx, cyl, (a+b)*0.5f,
                   0.10f,0.11f,0.13f,1.0f, true);
        }
    }
    // Four walls (floor-to-ceiling), thin slabs at the room edges.
    const float wcy = m_roomFloorY + roomH * 0.5f;
    addBox(scene, device, physics, kRoomHalfX, roomH*0.5f, kWallT*0.5f, cx, wcy, cz - kRoomHalfZ,
           wallR,wallG,wallB,1.0f, true);   // -Z wall
    addBox(scene, device, physics, kRoomHalfX, roomH*0.5f, kWallT*0.5f, cx, wcy, cz + kRoomHalfZ,
           wallR,wallG,wallB,1.0f, true);   // +Z wall
    addBox(scene, device, physics, kWallT*0.5f, roomH*0.5f, kRoomHalfZ, cx - kRoomHalfX, wcy, cz,
           wallR,wallG,wallB,1.0f, true);   // -X wall
    addBox(scene, device, physics, kWallT*0.5f, roomH*0.5f, kRoomHalfZ, cx + kRoomHalfX, wcy, cz,
           wallR,wallG,wallB,1.0f, true);   // +X wall

    // ====================================================================
    // 3) MANY SCREENS on the walls (emissive sci-fi-UI displays, varied colors).
    // ====================================================================
    const float kPI = 3.14159265358979f;
    const float screenY = m_roomFloorY + 1.7f;             // ~eye height
    // A varied palette so the wall of displays reads like a live op-center.
    struct Disp { float r,g,b,s,w,h; };
    const Disp pal[] = {
        {0.20f,0.85f,1.00f, 2.4f, 1.1f, 0.7f},   // cyan map
        {0.30f,1.00f,0.55f, 2.2f, 0.8f, 0.9f},   // green readout
        {1.00f,0.55f,0.20f, 2.0f, 0.9f, 0.6f},   // amber telemetry
        {0.85f,0.25f,1.00f, 2.3f, 1.0f, 0.8f},   // violet schematic
        {1.00f,0.30f,0.35f, 2.1f, 0.7f, 0.7f},   // red alert
        {0.25f,0.55f,1.00f, 2.5f, 1.2f, 0.6f},   // blue grid
    };
    const int npal = (int)(sizeof(pal)/sizeof(pal[0]));
    // -Z wall (faces +Z, yaw 0): 4 screens spread along X.
    for (int i = 0; i < 4; ++i) {
        const Disp& d = pal[i % npal];
        float x = cx - kRoomHalfX*0.6f + (kRoomHalfX*1.2f) * (i / 3.0f);
        addScreen(scene, device, x3::phys::Vec3{ x, screenY, cz - kRoomHalfZ + 0.06f }, 0.0f,
                  d.w, d.h, d.r, d.g, d.b, d.s);
    }
    // +Z wall (faces -Z): 4 screens.
    for (int i = 0; i < 4; ++i) {
        const Disp& d = pal[(i+2) % npal];
        float x = cx - kRoomHalfX*0.6f + (kRoomHalfX*1.2f) * (i / 3.0f);
        addScreen(scene, device, x3::phys::Vec3{ x, screenY, cz + kRoomHalfZ - 0.06f }, kPI,
                  d.w, d.h, d.r, d.g, d.b, d.s);
    }
    // -X wall (faces +X, yaw 90): 3 screens along Z.
    for (int i = 0; i < 3; ++i) {
        const Disp& d = pal[(i+1) % npal];
        float z = cz - kRoomHalfZ*0.5f + (kRoomHalfZ) * (i / 2.0f);
        addScreen(scene, device, x3::phys::Vec3{ cx - kRoomHalfX + 0.06f, screenY, z }, kPI*0.5f,
                  d.w, d.h, d.r, d.g, d.b, d.s);
    }
    // +X wall (faces -X, yaw -90): 3 screens along Z.
    for (int i = 0; i < 3; ++i) {
        const Disp& d = pal[(i+3) % npal];
        float z = cz - kRoomHalfZ*0.5f + (kRoomHalfZ) * (i / 2.0f);
        addScreen(scene, device, x3::phys::Vec3{ cx + kRoomHalfX - 0.06f, screenY, z }, -kPI*0.5f,
                  d.w, d.h, d.r, d.g, d.b, d.s);
    }

    // ====================================================================
    // 4) TREASURE — a glowing gold loot cache on a small pedestal, center stage.
    // ====================================================================
    {
        const x3::phys::Vec3 tpos{ cx, m_roomFloorY + 0.55f, cz };
        // Pedestal (collision, dark).
        addBox(scene, device, physics, 0.45f, 0.25f, 0.45f, cx, m_roomFloorY + 0.25f, cz,
               0.08f,0.09f,0.11f,1.0f, true);
        // The glowing cache (collectible).
        addPickup(scene, device, SecretPickup::Kind::Treasure, tpos, /*heal*/0,
                  1.0f, 0.84f, 0.25f, 3.0f, /*halfSize*/0.30f);
    }

    // ====================================================================
    // 5) WEAPON pickup (the existing WeaponSystem bobbing/spinning pickup).
    // ====================================================================
    // TODO(arsenal): this is the secret room's OWN WeaponSystem — walking into it sets
    // THIS system's hasWeapon()/hides the prop. Fully granting the held arsenal weapon
    // (so the player can fire it) means routing the pickup event into Level1Game's
    // weapon/Arsenal; left as a clear follow-up (the prop + collection already work).
    m_weapon.buildWeaponPickup(scene, device, modelDir,
                               x3::phys::Vec3{ cx - kRoomHalfX*0.55f, m_roomFloorY + 1.0f, cz - kRoomHalfZ*0.5f });

    // ====================================================================
    // 6) HEALTH packs (emissive red-cross-style boxes; heal on pickup).
    // ====================================================================
    addPickup(scene, device, SecretPickup::Kind::Health,
              x3::phys::Vec3{ cx + kRoomHalfX*0.55f, m_roomFloorY + 0.6f, cz - kRoomHalfZ*0.45f },
              /*heal*/50, 0.95f, 0.15f, 0.20f, 2.0f, 0.22f);
    addPickup(scene, device, SecretPickup::Kind::Health,
              x3::phys::Vec3{ cx + kRoomHalfX*0.70f, m_roomFloorY + 0.6f, cz - kRoomHalfZ*0.10f },
              /*heal*/50, 0.95f, 0.15f, 0.20f, 2.0f, 0.22f);

    // ====================================================================
    // 7) NANO-BOOSTER — a special tech/bio upgrade pickup (distinct teal glow).
    // ====================================================================
    // TODO(upgrade-system): A full augment/perk system is deeper than a flag. For
    // now collecting this latches nanoBoosterActive() (queried by the host/HUD) and,
    // when a heal callback is supplied, applies a one-shot bio surge (full heal) as a
    // stand-in effect. A future Augment system should consume nanoBoosterActive() to
    // raise maxHP / add an ability (Player has no public maxHP setter yet).
    addPickup(scene, device, SecretPickup::Kind::NanoBooster,
              x3::phys::Vec3{ cx - kRoomHalfX*0.55f, m_roomFloorY + 0.7f, cz + kRoomHalfZ*0.5f },
              /*heal*/0, 0.10f, 1.0f, 0.80f, 3.2f, 0.26f);

    // ====================================================================
    // 8) THE CELL HOLO-TERMINAL — build it in Jake's cell + wire the submit sink to
    //    unlock+open the hatch on the correct code. (Faces -Z toward Jake's spawn.)
    // ====================================================================
    // Place the terminal on the +Z cell wall in front of the spawn, ~1.3 m high.
    const x3::phys::Vec3 termPos{ cellCenter.x, cellCenter.y + 1.3f, cellCenter.z - 2.6f };
    m_terminal.build(scene, device, termPos, /*yaw*/kPI, 1.4f, 0.9f, termCeilY);
    // The glass plate is SOLID (owner playtest: "you can't walk through glass") —
    // a thin static box matching the panel footprint (yaw==pi keeps it X-aligned).
    physics.addBox(x3::phys::Vec3{ 0.75f, 0.50f, 0.05f }, termPos,
                   /*mass*/0.0f, x3::phys::Layer::Static);
    DoorSystem* dptr = &doors;
    uint32_t hatchIdx = m_hatchIdx;
    m_terminal.setSubmitSink([this, dptr, hatchIdx](const std::string& v) -> bool {
        if (v != kSecretRoomCode) {                        // reject any other code
            // Wrong-code BUZZ at the terminal (the elevator's keypad reject cue).
            if (m_audio && m_snd.buzz.valid()) {
                const x3::phys::Vec3 a = m_terminal.anchor();
                m_audio->playSound3D(m_snd.buzz, a.x, a.y, a.z, 0.8f, 1.0f);
            }
            return false;
        }
        if (hatchIdx == kNoLink || hatchIdx >= dptr->count()) return false;
        Door& hatch = dptr->at(hatchIdx);
        // IDEMPOTENT ACCEPT: the D14 scripts/secret_room.lua path reacts to the
        // fired terminal_code BEFORE submit() runs this sink (see main.cpp
        // submitTerminalToScripts), so the hatch may already be Opening/Open.
        // The correct code must still read ACCEPTED — without this the live
        // terminal showed [REJECTED] on the right code whenever the script won
        // the race (caught by --test-hatch C8).
        if (hatch.state == DoorState::Opening || hatch.state == DoorState::Open)
            return true;
        bool opened = dptr->unlockAndOpen(hatch);          // unlock + start the slide
        if (opened)
            x3::logInfo("[secret] override code accepted — cell trapdoor opening");
        return opened;
    });
    // Add a clear hint line to the readout (the cell-hatch code is kSecretRoomCode
    // = 1278; 1127 is the distinct Club/elevator code — do not conflate them).
    m_terminal.addLine("** MAINTENANCE: floor hatch override available **");

    m_built = true;
    x3::logInfo(std::string("[secret] built: trapdoor (hatch idx ") + std::to_string(m_hatchIdx) +
                ") + secret room below cell at y=" + std::to_string((int)m_roomFloorY) +
                " — " + std::to_string(m_screenCount) + " screens, " +
                std::to_string(m_pickups.size()) + " pickups (treasure/health/nano) + 1 weapon");
}

bool SecretRoom::submitCode(const std::string& code, DoorSystem& doors) {
    (void)doors;   // the sink already captured the DoorSystem
    m_terminal.setActive(true);
    // Replay the code through the input line so the sink fires exactly as in-app.
    for (char c : code) m_terminal.pushChar(c);
    return m_terminal.submit();
}

void SecretRoom::tick(float dt, Scene& scene, const x3::phys::Vec3& playerPos,
                      const std::function<void(int)>& heal) {
    if (!m_built) return;
    m_terminal.update(dt);
    m_weapon.update(dt, scene, playerPos);

    // ---- Hatch STATUS LIGHT: locked-RED -> GREEN the frame the hatch unlocks (the
    // correct code was accepted; the door leaves Closed/locked). One-way latch. ----
    if (!m_statusGreen && m_statusEnt != kNoLink && m_statusEnt < scene.size() &&
        m_doorsPtr && m_hatchIdx != kNoLink && m_hatchIdx < m_doorsPtr->count()) {
        const Door& h = m_doorsPtr->at(m_hatchIdx);
        if (!h.locked || h.state != DoorState::Closed) {
            Entity& e = scene.get(m_statusEnt);
            e.emissive[0]=0.12f; e.emissive[1]=1.0f; e.emissive[2]=0.30f; e.emissive[3]=3.0f;  // match the R7 hotter lens
            e.baseColor[0]=0.04f; e.baseColor[1]=0.12f; e.baseColor[2]=0.06f;
            m_statusGreen = true;
            // ACCESS-GRANTED CHIME on the same one-way unlock edge as the lens flip —
            // this is the single choke point ALL open paths cross (terminal sink, the
            // Lua script race, console/test submitCode), so the chime can never be
            // missed or double-fired.
            if (m_audio && m_snd.chime.valid()) {
                const x3::phys::Vec3 hp = h.closedPos;
                m_audio->playSound3D(m_snd.chime, hp.x, hp.y + 0.4f, hp.z, 0.9f, 1.0f);
            }
        }
    }

    // ---- Hatch SERVO + THUNK (the elevator's motor treatment): a heavy looped
    // servo voice while the panels slide, distance-attenuated at the hatch each
    // frame, then a seat THUNK on the edge where travel completes (open OR closed).
    // Driven off door STATE, so every open path sounds. ----
    if (m_audio && m_doorsPtr && m_hatchIdx != kNoLink && m_hatchIdx < m_doorsPtr->count()) {
        const Door& h = m_doorsPtr->at(m_hatchIdx);
        const bool moving = (h.state == DoorState::Opening) || (h.state == DoorState::Closing);
        const x3::phys::Vec3 hp = h.closedPos;
        const float dx = playerPos.x - hp.x, dy = playerPos.y - hp.y, dz = playerPos.z - hp.z;
        const float att = 1.0f / (1.0f + 0.18f * (dx*dx + dy*dy + dz*dz));   // soft rolloff
        if (moving && !m_hatchWasMoving && m_snd.servo.valid())
            m_servoVoice = m_audio->startLoop(m_snd.servo, 0.55f * att, 0.80f);  // deep + heavy
        else if (moving && m_servoVoice.valid())
            m_audio->setLoopParams(m_servoVoice, 0.55f * att, 0.80f);
        if (!moving && m_hatchWasMoving) {
            if (m_servoVoice.valid()) { m_audio->stopLoop(m_servoVoice); m_servoVoice = {}; }
            if (m_snd.thunk.valid())
                m_audio->playSound3D(m_snd.thunk, hp.x, hp.y, hp.z, 0.9f, 0.78f);  // heavy seat
        }
        m_hatchWasMoving = moving;
    }

    // ---- Pickup collection: walk within radius (XZ) to collect. ----
    const float r2 = kSecretPickupRadius * kSecretPickupRadius;
    for (SecretPickup& p : m_pickups) {
        if (p.collected) continue;
        const float dx = playerPos.x - p.pos.x;
        const float dz = playerPos.z - p.pos.z;
        const float dy = playerPos.y - p.pos.y;       // also require vertical proximity
        if (dx*dx + dz*dz > r2 || std::fabs(dy) > 2.0f) continue;
        p.collected = true;
        if (p.entity != kNoLink && p.entity < scene.size())
            scene.get(p.entity).visible = false;       // consume the prop
        switch (p.kind) {
            case SecretPickup::Kind::Treasure:
                ++m_treasureGot;
                x3::logInfo("[secret] TREASURE collected");
                break;
            case SecretPickup::Kind::Health:
                ++m_healthGot;
                if (heal) heal(p.healAmount);
                x3::logInfo("[secret] HEALTH pack collected (+" + std::to_string(p.healAmount) + ")");
                break;
            case SecretPickup::Kind::NanoBooster:
                m_nanoActive = true;
                // Stand-in effect (see build() TODO): a bio surge that fully heals. The
                // heal callback clamps to the player's maxHP, so a large amount = "full".
                if (heal) heal(9999);
                x3::logInfo("[secret] NANO-BOOSTER acquired — augment online");
                break;
        }
    }
}

// ===========================================================================
// Headless self-test (--test-secretroom). S1-S6.
// ===========================================================================
namespace {
int g_pass = 0, g_fail = 0;
void check(bool cond, const char* name) {
    if (cond) { ++g_pass; x3::logInfo(std::string("[secretroom-test] PASS ") + name); }
    else      { ++g_fail; x3::logError(std::string("[secretroom-test] FAIL ") + name); }
}
constexpr float kFixedDt = 1.0f / 60.0f;
using HeadlessDevice = x3::game::HeadlessRenderDevice;
} // namespace

bool runSecretRoomSelfTest() {
    g_pass = g_fail = 0;

    std::unique_ptr<x3::phys::IPhysicsWorld> physics(x3::phys::createPhysicsWorld());
    physics->init();
    HeadlessDevice device;
    Scene scene;
    DoorSystem doors;
    SecretRoom secret;

    // Cell at world origin (matches Jake's Cell center (0,0,0), floor at y=0).
    const x3::phys::Vec3 cellCenter{ 0.0f, 0.0f, 0.0f };
    secret.build(scene, device, *physics, doors, cellCenter, /*modelDir*/"");

    // ---- S1: the hatch starts LOCKED + Closed. ----
    {
        bool ok = secret.hatchBuilt();
        const Door& h = doors.at(secret.hatchDoorIndex());
        check(ok && h.locked && h.state == DoorState::Closed && h.floorHatch,
              "S1 trapdoor hatch starts LOCKED + Closed (floor hatch)");
    }

    // ---- S2: a WRONG code does NOT open the hatch. ----
    {
        bool accepted = secret.submitCode("9999", doors);
        const Door& h = doors.at(secret.hatchDoorIndex());
        check(!accepted && h.locked && h.state == DoorState::Closed,
              "S2 wrong code rejected — hatch stays locked + closed");
    }

    // ---- S3: the CORRECT code unlocks + opens the hatch (via the terminal sink). --
    {
        bool accepted = secret.submitCode(kSecretRoomCode, doors);
        const Door& h = doors.at(secret.hatchDoorIndex());
        bool opening = (h.state == DoorState::Opening || h.state == DoorState::Open);
        check(accepted && !h.locked && opening,
              "S3 correct code (1278) unlocks + opens the hatch");
    }

    // ---- S4: the secret room exists BELOW the cell (floor at negative Y). ----
    {
        bool below = secret.roomFloorY() < cellCenter.y - 3.0f;
        check(secret.built() && below,
              "S4 secret room built below the cell (floor at negative Y)");
    }

    // ---- S5: the room is stocked — >=1 weapon + >=1 health + >=1 treasure +
    // >=1 nano-booster + MANY screens. ----
    {
        uint32_t weap=0, hp=0, treas=0, nano=0;
        for (uint32_t i = 0; i < secret.pickupCount(); ++i) {
            switch (secret.pickup(i).kind) {
                case SecretPickup::Kind::Treasure:    ++treas; break;
                case SecretPickup::Kind::Health:      ++hp;    break;
                case SecretPickup::Kind::NanoBooster: ++nano;  break;
            }
        }
        bool weaponBuilt = secret.secretWeapon().pickupEntity() != kNoLink;
        if (weaponBuilt) weap = 1;
        bool screens = secret.screenCount() >= 8;   // "many"
        check(weap >= 1 && hp >= 1 && treas >= 1 && nano >= 1 && screens,
              "S5 room stocked: weapon + health + treasure + nano + many screens");
    }

    // ---- S6: a body dropped through the OPEN hatch reaches the secret-room floor. --
    {
        // Finish opening the hatch.
        for (int i = 0; i < 90; ++i) { doors.update(kFixedDt, scene, *physics); physics->step(kFixedDt); }
        const Door& h = doors.at(secret.hatchDoorIndex());
        bool open = h.state == DoorState::Open;
        // Drop a dynamic box from just above the hatch center (the shared cutout XZ).
        const x3::phys::Vec3 hc{ kCellHatchCx, cellCenter.y + 1.0f, kCellHatchCz };
        x3::phys::BodyId b = physics->addBox(x3::phys::Vec3{0.2f,0.2f,0.2f}, hc,
                                             2.0f, x3::phys::Layer::Dynamic);
        for (int i = 0; i < 240; ++i) { doors.update(kFixedDt, scene, *physics); physics->step(kFixedDt); }
        const float y = physics->getBodyPosition(b).y;
        // It should fall well below the cell floor and come to rest near the room floor
        // (room floor is at roomFloorY; the box half is 0.2, so it rests ~roomFloorY+0.2).
        bool reachedRoom = y < cellCenter.y - 3.0f && y > secret.roomFloorY() - 0.5f;
        check(open && reachedRoom,
              "S6 body dropped through the open hatch reaches the secret-room floor");
    }

    // ---- S8: TWO-PANEL hatch — the hatch is built from two flush panels that PART
    // from the centre, and the collision CLEARS on Open (the drop-through blocker).
    // Proven in a FRESH world: (a) the door carries a valid second panel body, (b)
    // a body rests ON the closed two-panel hatch, and (c) once open BOTH panels have
    // slid outward (centres parted) and the same body now drops through. ----
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> p8(x3::phys::createPhysicsWorld());
        p8->init();
        HeadlessDevice dev8;
        Scene s8;
        DoorSystem d8;
        SecretRoom sec8;
        sec8.build(s8, dev8, *p8, d8, x3::phys::Vec3{0,0,0}, "");
        const Door& h0 = d8.at(sec8.hatchDoorIndex());
        const bool twoPanel = h0.body2.valid();
        // Closed-state panel centres (the two halves meet at the opening centre).
        const float c1x0 = p8->getBodyPosition(h0.body).x;
        const float c2x0 = p8->getBodyPosition(h0.body2).x;

        // (b) A body dropped on the CLOSED hatch rests on top (collision holds).
        const x3::phys::Vec3 dropPos{ kCellHatchCx, 1.0f, kCellHatchCz };
        x3::phys::BodyId held = p8->addBox(x3::phys::Vec3{0.2f,0.2f,0.2f}, dropPos, 2.0f,
                                           x3::phys::Layer::Dynamic);
        for (int i = 0; i < 90; ++i) { d8.update(kFixedDt, s8, *p8); p8->step(kFixedDt); }
        const float yHeld = p8->getBodyPosition(held).y;
        const bool heldUp = yHeld > -1.0f;          // still up on the closed hatch

        // (c) Open the hatch: BOTH panels slide OUTWARD (centres part), collision clears.
        d8.unlockAndOpen(d8.at(sec8.hatchDoorIndex()));
        for (int i = 0; i < 120; ++i) { d8.update(kFixedDt, s8, *p8); p8->step(kFixedDt); }
        const Door& h1 = d8.at(sec8.hatchDoorIndex());
        const float c1x1 = p8->getBodyPosition(h1.body).x;
        const float c2x1 = p8->getBodyPosition(h1.body2).x;
        const bool parted = (c1x1 < c1x0 - 0.5f) && (c2x1 > c2x0 + 0.5f);  // panels moved apart
        const bool opened = h1.state == DoorState::Open;
        // The previously-held body now falls through the cleared opening.
        x3::phys::BodyId drop = p8->addBox(x3::phys::Vec3{0.2f,0.2f,0.2f}, dropPos, 2.0f,
                                           x3::phys::Layer::Dynamic);
        for (int i = 0; i < 180; ++i) { d8.update(kFixedDt, s8, *p8); p8->step(kFixedDt); }
        const bool dropped = p8->getBodyPosition(drop).y < -1.0f;
        check(twoPanel && heldUp && opened && parted && dropped,
              "S8 two-panel hatch: holds when closed, panels part + collision clears (drop-through) when open");
        p8->shutdown();
    }

    physics->shutdown();

    // ---- S7 (INTEGRATED): build the REAL Level 1 (which carves the B1 cell floor hole)
    // + its secret room, open the hatch via the terminal, and drop a body through the
    // carved cell floor. Proves the hatch + the B1 floor cutout line up so the trapdoor
    // actually works in-game (not just on the isolated room). ----
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> p2(x3::phys::createPhysicsWorld());
        p2->init();
        HeadlessDevice dev2;
        Scene s2;
        Level1Game game;
        game.setDevice(dev2);
        game.build(s2, dev2, *p2, /*modelDir*/"");

        // Open the cell trapdoor via the terminal override code. submitCode replays the
        // code through the terminal input line + submit(), which fires the sink captured
        // at build (it holds the real DoorSystem); the DoorSystem arg here is unused.
        DoorSystem unusedDoors;
        bool accepted = game.secret().submitCode(kSecretRoomCode, unusedDoors);
        // Step the level so the hatch finishes opening (tick drives m_doors.update()).
        const x3::phys::Vec3 spawn = game.layout().spawn;
        for (int i = 0; i < 120; ++i) { game.tick(kFixedDt, s2, *p2, spawn, spawn); p2->step(kFixedDt); s2.update(*p2); }

        // Drop a body at the carved hole (shared cutout XZ) from just above the cell floor.
        const float cellY = game.secret().roomFloorY() + 6.0f;   // == B1 cell floor (room is 6 m down)
        const x3::phys::Vec3 hc{ kCellHatchCx, cellY + 1.0f, kCellHatchCz };
        x3::phys::BodyId b = p2->addBox(x3::phys::Vec3{0.2f,0.2f,0.2f}, hc, 2.0f, x3::phys::Layer::Dynamic);
        for (int i = 0; i < 300; ++i) { game.tick(kFixedDt, s2, *p2, spawn, spawn); p2->step(kFixedDt); s2.update(*p2); }
        const float y = p2->getBodyPosition(b).y;
        bool fellThroughCarvedFloor = y < cellY - 3.0f && y > game.secret().roomFloorY() - 0.6f;
        check(accepted && fellThroughCarvedFloor,
              "S7 integrated: terminal code opens the trapdoor; body drops through the carved cell floor into the room");
        p2->shutdown();
    }

    x3::logInfo(std::string("[secretroom-test] ") + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

} // namespace x3::game
