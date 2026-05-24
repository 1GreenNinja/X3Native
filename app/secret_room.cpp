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
                       const x3::phys::Vec3& cellCenter, std::string_view modelDir) {
    m_doorsPtr = &doors;

    // ====================================================================
    // 1) THE FLOOR-HATCH TRAPDOOR in the cell floor (starts LOCKED).
    // ====================================================================
    // Place it at the SHARED cell-hatch cutout location (level1.h kCellHatch*), so the
    // hatch slab lines up exactly with the hole carved in the B1 plate floor — an open
    // hatch then drops the player straight through. Only the Y comes from cellCenter
    // (the cell floor); the XZ is the world cutout center. It slides aside along +X by
    // its full width (2*halfWidth) to clear the opening.
    const x3::phys::Vec3 hatchCenter{ kCellHatchCx, cellCenter.y, kCellHatchCz };
    {
        DoorSpec h;
        h.doorwayCenter = hatchCenter;
        h.floorHatch    = true;
        h.halfWidth     = kCellHatchHalf;
        h.thickness     = 0.2f;
        h.duration      = 1.2f;
        h.withButton    = false;       // opened by the terminal code, not a wall button
        h.locked        = true;        // ONLY the override code opens it
        h.code          = 1127;        // the lore override code (matches kSecretRoomCode)
        h.tint[0]=0.30f; h.tint[1]=0.34f; h.tint[2]=0.40f; h.tint[3]=1.0f;  // dark steel hatch
        m_hatchIdx = buildLevelDoor(scene, doors, device, physics, h);
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
    m_terminal.build(scene, device, termPos, /*yaw*/kPI, 1.4f, 0.9f, cellCenter.y + 2.8f);
    DoorSystem* dptr = &doors;
    uint32_t hatchIdx = m_hatchIdx;
    m_terminal.setSubmitSink([this, dptr, hatchIdx](const std::string& v) -> bool {
        if (v != kSecretRoomCode) return false;            // reject any other code
        if (hatchIdx == kNoLink || hatchIdx >= dptr->count()) return false;
        Door& hatch = dptr->at(hatchIdx);
        bool opened = dptr->unlockAndOpen(hatch);          // unlock + start the slide
        if (opened)
            x3::logInfo("[secret] override code accepted — cell trapdoor opening");
        return opened;
    });
    // Add a clear hint line to the readout (the code is the lore code 1127).
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
              "S3 correct code (1127) unlocks + opens the hatch");
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
