// EFLZ Level 1 game controller + --test-level1. See app/level1_game.h.
//
// Clean-room: built from the Scene/door/weapon/monster/objective/trigger systems
// and the engine interfaces only. No purchased C# / id Tech source consulted.
#include "level1_game.h"
#include "mesh_prims.h"
#include "elevator.h"
#include "player.h"        // Player (save/load bridge)
#include "weapon.h"        // Arsenal (save/load bridge)
#include "spire_mid.h"     // SpireMidFloors (save/load bridge)
#include "spire_top.h"     // SpireTopFloors (save/load bridge)
#include "headless_device.h"
#include "asset_root.h"

#include "engine/core/x3_log.h"

#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace x3::game {

// ---------------------------------------------------------------------------
// Tuning for Level 1 enemy placements (geometry from level1.cpp). All positions
// are body-center world coords; the crawler GLB sits at ~y=0.4.
// ---------------------------------------------------------------------------
namespace {
constexpr float kEnemyY = 0.4f;

// ---- EFLZ art pass: the converted character GLBs (real sci-fi enemies). The
// host passes `modelDir` = rigged_glb (for the legacy pistol/crawler fallback);
// these tunings point the enemy meshes at the converted_glb Characters instead.
// On load failure the MonsterSystem falls back to its procedural box (per-enemy),
// so the level never breaks. Characters are Z-up (lying flat) -> standUpZtoY. -----
// Portable asset roots (assets-LFS pass): resolved relative to the repo/exe via
// assetRoot(), falling back to G:/GameModels on machines that still have it.
// Lazy-resolved once (the exe path is stable at runtime).
const std::string& convertedDir() { static const std::string d = convertedGlbRoot(); return d; }
// The original rigged GLBs: these retain their skeleton AND an "Idle" animation
// clip (the converted_glb/Characters export dropped the animation, leaving only a
// bind-pose skin). For J1 (make the characters ANIMATE) the humanoid enemies load
// the rigged source so they play their idle clip via CPU skinning. Authored Y-up
// (feet at y=0, head ~1.8 m), so NO Z->Y stand-up is needed.
const std::string& riggedDir() { static const std::string d = riggedGlbRoot(); return d; }

// Apply the converted-character model to a tuning (file under Characters/, the
// converted dir, the Z->Y stand-up, and a real-scale humanoid size).
void useCharacter(MonsterSystem::Tuning& t, const char* file, float scale) {
    t.modelFile        = std::string("Characters/") + file;
    t.modelDirOverride = convertedDir();
    t.standUpZtoY      = true;          // converted characters are authored Z-up
    t.modelScale       = scale;         // ~1.0 => ~1.77 m tall humanoid
}

// Apply a RIGGED + ANIMATED source character (J1/T1). Same role as useCharacter
// but from rigged_glb (so the MonsterSystem's Skinner finds the locomotion clips
// and plays/blends them). Y-up, so standUpZtoY stays false. On load failure the
// per-enemy box fallback still applies (the level never breaks).
//
// T1 ASSET PREFERENCE (small, distinct block): prefer the MULTI-CLIP "<name>_anim
// .glb" (Idle/Walk/Run/Jump — the retargeted artifact that enables the locomotion
// blend) when it exists ON DISK; otherwise fall back to the Idle-only "<name>.glb".
// The big *_anim.glb are generated artifacts and may be ABSENT in a clean checkout
// — in that case we silently use the base GLB (which plays Idle), and if THAT is
// also missing the MonsterSystem falls back to its procedural box. This keeps
// clean checkouts working while light-up locomotion when the assets are present.
void useRiggedCharacter(MonsterSystem::Tuning& t, const char* file, float scale) {
    namespace fs = std::filesystem;
    std::string base(file);
    std::string chosen = base;
    const std::string stem = (base.size() > 4 && base.substr(base.size() - 4) == ".glb")
        ? base.substr(0, base.size() - 4) : base;
    const std::string animName = stem + "_anim.glb";
    std::error_code ec;
    if (fs::exists(fs::path(riggedDir()) / animName, ec)) {
        chosen = animName;
        x3::logInfo("[level1] using multi-clip locomotion asset: " + animName);
    } else {
        x3::logInfo("[level1] " + animName + " absent — falling back to Idle-only " + base);
    }
    t.modelFile        = chosen;
    t.modelDirOverride = riggedDir();
    t.standUpZtoY      = false;         // rigged sources are authored Y-up
    t.modelScale       = scale;
}

// Boss-tier Martinez (§8): more HP + a bit faster + bigger + a distinct tint +
// stronger melee. NO new AI/phases this pass — purely params over the basic
// monster (boss phases are Phase 2b). Mesh: Security_Chief (the armed humanoid).
MonsterSystem::Tuning martinezTuning() {
    MonsterSystem::Tuning t;
    t.hp         = 340;     // ~10 pistol shots (basic monster is 100 / 3 shots)
    t.chaseSpeed = 3.4f;    // a bit faster than the basic 2.5 m/s
    t.tint[0] = 1.0f; t.tint[1] = 0.55f; t.tint[2] = 0.55f; t.tint[3] = 1.0f; // reddish
    // Melee boss: hits harder + a touch faster than a guard.
    t.type           = MonsterType::Boss;
    t.damage         = 15;
    t.attackRange    = 2.4f;
    t.attackCooldown = 1.1f;
    t.attackWindup   = 0.30f;
    t.ranged         = false;
    // J1: rigged + ANIMATED Chief Martinez (plays its Idle clip via CPU skinning).
    useRiggedCharacter(t, "chief_martinez.glb", 1.35f);  // boss reads taller than a guard
    return t;
}
MonsterSystem::Tuning guardTuning() {
    MonsterSystem::Tuning t;          // basic enemy stats (defaults), neutral tint
    // Melee guard: baton-range chip damage on a ~1 s cadence. Pulls from the GENERAL
    // combat balance params (monster.h, namespace combat) so the squad's hit-rate +
    // damage stay inside the sane, winnable bands — not magic numbers buried here.
    // The dogpile cap (combat::kMaxMeleeAttackers) is enforced by MonsterManager.
    t.type           = MonsterType::Guard;
    t.damage         = combat::kMeleeDamageDefault;   // 8 HP per swing (band 6..10)
    t.attackRange    = combat::kMeleeRange;           // 1.9 m baton reach
    t.attackCooldown = combat::kMeleeCooldownDefault; // ~1.1 s between swings
    t.attackWindup   = combat::kMeleeWindup;          // 0.25 s telegraph
    t.ranged         = false;
    // J1: rigged + ANIMATED guard (the rigged source plays an Idle clip; the
    // converted_glb/Characters/Security_Chief.glb has only a bind-pose skin).
    useRiggedCharacter(t, "marcus_webb.glb", 1.0f);   // real ~1.8 m animated guard
    return t;
}
MonsterSystem::Tuning droneTuning() {
    MonsterSystem::Tuning t;
    t.hp         = 66;                 // squishier (2 shots)
    t.chaseSpeed = 3.0f;               // faster, flits about
    t.tint[0] = 0.6f; t.tint[1] = 0.8f; t.tint[2] = 1.0f; t.tint[3] = 1.0f; // pale blue
    // Ranged drone: keeps a standoff distance and fires a taser hitscan. Damage +
    // standoff pull from the GENERAL combat balance params (monster.h, combat::*).
    t.type           = MonsterType::Drone;
    t.flyer          = true;           // ACTUAL flier: hovers, center-origin model
    t.damage         = combat::kRangedDamageDefault;  // 5 HP per bolt (band 4..6)
    t.attackRange    = 14.0f;          // can fire from across the corridor
    t.attackCooldown = combat::kRangedCooldownDefault;// ~1.4 s between bolts
    t.attackWindup   = 0.35f;          // a beat of telegraph before the bolt
    t.ranged         = true;
    t.standoff       = combat::kRangedStandoff;       // hold ~7 m out
    // Animated drone (Tim's authored model — rotors spin via the GLB's baked motion).
    // Falls back to the procedural box if the load fails. Scale may need a tweak.
    useCharacter(t, "DroneExportWMotion.glb", 1.0f);
    return t;
}
} // namespace

void Level1Game::build(Scene& scene, x3::rhi::IRenderDevice& device,
                       x3::phys::IPhysicsWorld& physics, std::string_view modelDir) {
    m_modelDir = std::string(modelDir);
    m_devicePtr = &device;   // cached so tick() can spawn enemies on their beats

    // ---- EFLZ art pass: load the converted sci-fi GLBs and place visual props
    // over the Spire graybox. EnvArt tiles its GLB floors/walls/ceilings/lights
    // from the shared per-floor table (level1Rooms()) and anchors door frames at
    // each floor's elevator doorway + the B1 spine doors. Those positions are
    // deterministic, so we seed a minimal layout (the same constants buildLevel1
    // uses); EnvArt returns a mask of which graybox surfaces real art covers. If
    // the GLBs are missing, the mask is all-false (full graybox). --
    {
        Level1Layout seed;
        const L1RoomDef* tbl = level1Rooms();
        const float shaftX0 = 19.5f;     // matches level1.cpp kShaftX0
        for (uint32_t fi = 0; fi < (uint32_t)L1Floor::Count; ++fi)
            seed.elevatorDoor[fi] = x3::phys::Vec3{ shaftX0, tbl[fi].y0, 0.0f };
        const float b1y = tbl[(uint32_t)L1Floor::B1].y0;
        seed.doorA = x3::phys::Vec3{  5.0f, b1y, 0.0f };
        seed.doorB = x3::phys::Vec3{ 10.0f, b1y, 0.0f };
        seed.doorC = x3::phys::Vec3{ 14.0f, b1y, 0.0f };
        seed.doorD = x3::phys::Vec3{ 18.0f, b1y, 0.0f };
        seed.doorE = x3::phys::Vec3{ shaftX0, b1y, 0.0f };
        m_artMask = m_envArt.build(device, convertedDir(), seed);
    }

    // ---- Lighting: register a forward point light at each Light_A ceiling fixture
    // the env-art placed, so the corridor is lit (not just decorated with dark
    // fixture meshes). The fixtures are static, so one call is enough — the device
    // caches the set and re-uploads it into each frame's UBO. mesh.frag accumulates
    // them on TOP of the existing directional sun + shadow pass. ----
    {
        const auto& fixtures = m_envArt.lightFixtures();
        if (!fixtures.empty())
            device.setPointLights(fixtures.data(), (uint32_t)fixtures.size());
    }

    // ---- Geometry (graybox collision; surface renders suppressed where real art
    // covers them — see m_artMask). ----
    m_layout = buildLevel1(scene, device, physics, m_artMask);

    // ---- Doors A-E (generalized builder). Doorways sit in cross-walls whose
    // plane is x=const (the wall runs along Z), so DoorAxis::AlongZ. ----
    {
        DoorSpec a; a.doorwayCenter = m_layout.doorA; a.axis = DoorAxis::AlongZ;
        a.withButton = true; a.locked = false;
        a.tint[0]=0.52f; a.tint[1]=0.30f; a.tint[2]=0.26f;  // cell door (industrial red, metallic)
        m_doorIdx[0] = buildLevelDoor(scene, m_doors, device, physics, a);
    }
    {
        DoorSpec b; b.doorwayCenter = m_layout.doorB; b.axis = DoorAxis::AlongZ;
        b.withButton = true; b.locked = false;
        b.tint[0]=0.34f; b.tint[1]=0.44f; b.tint[2]=0.56f;  // corridor door (steel blue)
        m_doorIdx[1] = buildLevelDoor(scene, m_doors, device, physics, b);
    }
    {
        DoorSpec c; c.doorwayCenter = m_layout.doorC; c.axis = DoorAxis::AlongZ;
        c.withButton = true; c.locked = true;               // §6.4 locked until armed
        c.code = 1127;                                       // keypad: enter 1127 to open early (lore code)
        c.tint[0]=0.58f; c.tint[1]=0.48f; c.tint[2]=0.28f;  // armory gate (brushed brass)
        m_doorIdx[2] = buildLevelDoor(scene, m_doors, device, physics, c);
    }
    {
        DoorSpec d; d.doorwayCenter = m_layout.doorD; d.axis = DoorAxis::AlongZ;
        d.withButton = false; d.locked = true;              // auto on arena trigger
        d.tint[0]=0.46f; d.tint[1]=0.38f; d.tint[2]=0.38f;  // dark steel
        m_doorIdx[3] = buildLevelDoor(scene, m_doors, device, physics, d);
    }
    {
        DoorSpec e; e.doorwayCenter = m_layout.doorE; e.axis = DoorAxis::AlongZ;
        e.withButton = false; e.locked = true;              // opens on Martinez death
        e.tint[0]=0.38f; e.tint[1]=0.50f; e.tint[2]=0.56f;  // cyan steel
        m_doorIdx[4] = buildLevelDoor(scene, m_doors, device, physics, e);
    }

    // ---- Starting sidearm IN THE CELL: the player grabs a pistol right at spawn so
    // they're never defenseless against the corridor monsters (the armory then adds the
    // heavier guns — see m_armory below). Placed a step ahead of spawn (1.5) in the cell. ----
    m_weapon.buildWeaponPickup(scene, device, m_modelDir,
                               x3::phys::Vec3{ m_layout.cellCenter.x, 1.0f,
                                               m_layout.cellCenter.z });

    // ---- Checkpoint encounter (built at level build; beat 8). Now the player is
    // armed, so this is a proper firefight: 3 Guards holding the line behind the
    // barricade crates + 1 Drone covering from the back of the room. ----
    // Spire B1: the checkpoint zone is the walled sub-room between Door C (x=14) and
    // Door D (x=18); keep all 4 enemies inside it (and off the z=0 LOS spine) so they
    // stay clear of the arena/shaft and the corridor encounter.
    // BESTIARY PASS: two of the four slots use the data-driven roster — a Verthani
    // (insectoid flanker) holding the line and a BlueSynth (synthetic flier) covering
    // from the back — alongside two baseline Dominion troopers. ADDITIVE: same COUNT
    // (4) + same roles (3 melee + 1 ranged), so --test-level1's clear loop is intact.
    const x3::phys::Vec3 cc = m_layout.checkpointCenter;   // ~13.7 on B1
    m_checkpoint.spawn(scene, device, physics, m_modelDir,
                       x3::phys::Vec3{ cc.x - 0.6f, kEnemyY, cc.z - 2.0f },
                       tuningFor(EnemyType::DominionTrooper));
    m_checkpoint.spawn(scene, device, physics, m_modelDir,
                       x3::phys::Vec3{ cc.x - 0.4f, kEnemyY, cc.z + 2.0f },
                       tuningFor(EnemyType::Verthani));
    m_checkpoint.spawn(scene, device, physics, m_modelDir,
                       x3::phys::Vec3{ cc.x + 0.6f, kEnemyY, cc.z - 1.0f },
                       tuningFor(EnemyType::DominionTrooper));
    m_checkpoint.spawn(scene, device, physics, m_modelDir,
                       x3::phys::Vec3{ cc.x + 0.8f, kEnemyY, cc.z + 2.5f },
                       tuningFor(EnemyType::BlueSynth));

    // ---- Explosive barrels: shootable, scattered along the corridor + a checkpoint
    // cluster (a hit detonates violently + chains to neighbors). ----
    {
        m_barrels.init(device, physics);
        const float by = level1Rooms()[(uint32_t)L1Floor::B1].y0;
        const x3::phys::Vec3 co = m_layout.corridorCenter;
        m_barrels.spawn(co.x - 2.0f, by, co.z + 1.6f);
        m_barrels.spawn(co.x + 3.0f, by, co.z - 1.6f);
        m_barrels.spawn(cc.x - 1.8f, by, cc.z + 0.2f);   // checkpoint pair -> chain reaction
        m_barrels.spawn(cc.x - 1.1f, by, cc.z + 0.9f);
        // MORE barrels (Tim playtest): scatter from the cell through the corridor so there
        // is always one in sight to shoot + bigger chain reactions. Kept within the ~z +/-1.6
        // corridor footprint and clear of the spawn (1.5) + the cell pickup (cellCenter,z=0).
        const float bx = m_layout.cellCenter.x;          // ~3.0, right by the spawn
        m_barrels.spawn(bx + 0.9f, by,  1.3f);           // cell/start — immediately shootable
        m_barrels.spawn(bx + 1.6f, by, -1.3f);
        m_barrels.spawn(co.x - 3.5f, by, -1.4f);
        m_barrels.spawn(co.x - 0.6f, by,  1.5f);
        m_barrels.spawn(co.x + 1.4f, by,  1.4f);
        m_barrels.spawn(co.x + 4.6f, by,  1.3f);
        // Two more in the checkpoint room, placed CLEAR of the eye->enemy firing lanes
        // (the test fires from cc.x-0.9 at z~0 toward enemies spread over z in [-2.5,2.5]).
        // One just inside Door C behind the firing line (cc.x-2.4, harmless), one tucked
        // against the +Z back of the room (z=+3.0) beyond every enemy — both still
        // shootable for the chain, neither intercepts a kill shot.
        m_barrels.spawn(cc.x - 2.4f, by, -1.6f);   // behind the eye, near Door C mouth
        m_barrels.spawn(cc.x + 1.2f, by,  3.0f);   // +Z back corner, beyond the enemies
        x3::logInfo("[level1] spawned " + std::to_string(m_barrels.count()) + " explosive barrels");
    }

    // ---- Trigger volumes. Strength (cell), arena (boss entry), elevator (win,
    // disabled until the boss dies). ----
    // Strength: a box around the equipment prop at (1.5, 0.4, -1.8) in the cell.
    m_triggers.add(x3::phys::Vec3{ 0.3f, 0.0f, -3.0f },
                   x3::phys::Vec3{ 3.0f, 2.5f, -0.4f }, (uint32_t)L1Trigger::Strength, true);
    // Arena: just past Door D (x=42), spanning the arena width.
    m_triggers.add(x3::phys::Vec3{ m_layout.doorD.x + 1.0f, 0.0f, -m_layout.arenaHalf.z },
                   x3::phys::Vec3{ m_layout.doorD.x + 4.0f, 3.0f,  m_layout.arenaHalf.z },
                   (uint32_t)L1Trigger::Arena, true);
    // Elevator: inside the elevator room; disabled until Martinez is dead so the
    // win can only fire after the boss fight (T6).
    m_triggers.add(x3::phys::Vec3{ m_layout.doorE.x + 0.5f, 0.0f, -m_layout.elevatorHalf.z },
                   x3::phys::Vec3{ m_layout.elevatorCenter.x + m_layout.elevatorHalf.x, 3.0f,
                                   m_layout.elevatorHalf.z },
                   (uint32_t)L1Trigger::Elevator, /*enabled*/false);

    // ---- Objectives (§3): the ordered list, cursor at the first. D-content fills
    // out the middle so the on-screen text tracks the WHOLE path the player walks:
    // escape -> arm in the armory -> clear the checkpoint -> beat the boss -> ride
    // the elevator. Five steps; each transition is wired in tick() below.
    //   0 escape | 1 arm | 2 checkpoint | 3 boss | 4 elevator
    m_objectives.set({ "Escape the detention cell",
                       "Fight to the armory and arm yourself",
                       "Clear the security checkpoint",
                       "Defeat Chief Martinez",
                       "Take the elevator to Floor 2" });

    // ---- F2 MEDICAL BAY rescue system (spec §5): 3 victims (Aria/Keisha/Emily) on
    // 5-min timers — the floor's signature triage. The 7-floor Spire build places these
    // in wards A/B/C on F2; on the current graybox we anchor them in the arena room (a
    // roomy space) at three distinct spots so the rescue/companion/expire loop is
    // exercisable today. The F2 floor BOSS — Dr. Chen (Corrupted) — is placed ALONGSIDE
    // this rescue on the F2 plate, gated on the same F2 ward hub (see tick() / spawnChen).
    //
    // PLAYTEST-FIX (Issue 2): the countdowns are GATED on the F2 ward hub being
    // reached and DEFAULT OFF — we do NOT call activate()/setHubReached at build, or
    // the 5-min timers count down from load and expire instantly, spawning all three
    // bosses (Siren/BreederQueen/Oracle) on the first frame. Instead we register an
    // L1Trigger::Hub volume around the ward area; entering it (see tick()) calls
    // m_rescue.activate(), which starts the clocks. Until then the victims stay
    // captive with no countdown. (For the future Spire build the F2-floor transition
    // flips the same activate() instead of this graybox trigger.) ----
    {
        const x3::phys::Vec3 ac = m_layout.arenaCenter;
        const x3::phys::Vec3 wardA{ ac.x - 3.0f, kEnemyY, ac.z - 3.0f };
        const x3::phys::Vec3 wardB{ ac.x,        kEnemyY, ac.z + 3.0f };
        const x3::phys::Vec3 wardC{ ac.x + 3.0f, kEnemyY, ac.z - 3.0f };
        m_rescue.build(scene, device, physics, m_modelDir, wardA, wardB, wardC);
        // Hub trigger: a box covering the ward cluster (the arena room). The first
        // frame the player steps into it, tick() fires activate() and the timers run.
        m_triggers.add(x3::phys::Vec3{ ac.x - 6.0f, 0.0f, ac.z - 6.0f },
                       x3::phys::Vec3{ ac.x + 6.0f, 3.0f, ac.z + 6.0f },
                       (uint32_t)L1Trigger::Hub, true);
    }

    // ---- CODE-LOCKED TRAPDOOR -> SECRET ROOM (cell HoloTerminal). The cell floor is
    // at the B1 plate base (y0); Jake's Cell center is the legacy cellCenter (XZ at
    // the spawn). Build the cell terminal + the floor-hatch trapdoor + the stocked
    // secret room below, wiring the terminal's submit sink to unlock+open the hatch
    // on code 1127. Additive — its own HoloTerminal + a hatch registered in m_doors. ----
    {
        const float b1y = level1Rooms()[(uint32_t)L1Floor::B1].y0;
        const x3::phys::Vec3 cellCenter{ m_layout.cellCenter.x, b1y, m_layout.cellCenter.z };
        m_secretRoom.build(scene, device, physics, m_doors, cellCenter, riggedDir());
    }

    m_built = true;
    x3::logInfo("Level1Game::build complete — doors A-E, pistol pickup, 4 checkpoint enemies, 4 triggers, 3 rescue victims (timers gated on F2-hub trigger), cell trapdoor + secret room");
}

uint32_t Level1Game::doorIndex(char letter) const {
    int i = letter - 'A';
    if (i < 0 || i > 4) return kNoLink;
    return m_doorIdx[i];
}

DoorState Level1Game::doorState(char letter) const {
    uint32_t i = doorIndex(letter);
    if (i == kNoLink || i >= m_doors.count()) return DoorState::Closed;
    return m_doors.at(i).state;
}

bool Level1Game::doorLocked(char letter) const {
    uint32_t i = doorIndex(letter);
    if (i == kNoLink || i >= m_doors.count()) return false;
    return m_doors.at(i).locked;
}

bool Level1Game::nearLockedCodedDoor(const x3::phys::Vec3& playerPos, float range) const {
    const float r2 = range * range;
    for (uint32_t i = 0; i < m_doors.count(); ++i) {
        const Door& d = m_doors.at(i);
        if (!d.locked || d.code == 0) continue;
        const float dx = playerPos.x - d.closedPos.x;
        const float dz = playerPos.z - d.closedPos.z;
        if (dx * dx + dz * dz <= r2) return true;
    }
    return false;
}

bool Level1Game::tryDoorCode(const x3::phys::Vec3& playerPos, int code, float range) {
    const float r2 = range * range;
    int best = -1; float bestD2 = r2;
    for (uint32_t i = 0; i < m_doors.count(); ++i) {
        const Door& d = m_doors.at(i);
        if (!d.locked || d.code == 0) continue;
        const float dx = playerPos.x - d.closedPos.x;
        const float dz = playerPos.z - d.closedPos.z;
        const float d2 = dx * dx + dz * dz;
        if (d2 <= bestD2) { bestD2 = d2; best = (int)i; }
    }
    if (best < 0) return false;
    Door& d = m_doors.at((uint32_t)best);
    if (d.code != code) return false;
    m_doors.unlock(d);
    return m_doors.startOpening(d);
}

void Level1Game::playSfx(x3::audio::SoundHandle h, const x3::phys::Vec3& at, float vol) {
    if (m_audio.sys && h.valid())
        m_audio.sys->playSound3D(h, at.x, at.y, at.z, vol, 1.0f);
}

void Level1Game::setCueSink(const GameCueFn& sink) {
    m_cueSink = sink;
    // Fan to every enemy group (managers store + re-apply to future spawns) and the
    // single boss instance. Corridor / Martinez / adds may not exist yet — the
    // managers carry the sink onto on-beat spawns; the boss is wired at spawn time
    // (and here too, harmlessly, if it's already up).
    m_corridor.setCueSink(sink);
    m_checkpoint.setCueSink(sink);
    m_bossAdds.setCueSink(sink);
    m_chen.setCueSink(sink);
    if (m_martinezSpawned) m_martinez.setCueSink(sink);
}

void Level1Game::setDeathFxSink(const DeathFxFn& sink) {
    m_deathFx = sink;
    // Fan the gib-burst death FX to every enemy group (managers store + re-apply to
    // future spawns) and the single Martinez boss instance — exactly like setCueSink.
    // Groups not yet spawned carry the sink onto their on-beat spawns; Martinez is
    // wired here once up. The monster fires it ONCE at the kill moment (HP->0).
    m_corridor.setDeathFxSink(sink);
    m_checkpoint.setDeathFxSink(sink);
    m_bossAdds.setDeathFxSink(sink);
    m_chen.setDeathFxSink(sink);
    if (m_martinezSpawned) m_martinez.setDeathFxSink(sink);
}

void Level1Game::cheatArm(Scene& scene) { m_weapon.forceArm(scene); }  // IDKFA/IDFA

void Level1Game::spawnCorridorEnemies(Scene& scene, x3::rhi::IRenderDevice& device,
                                      x3::phys::IPhysicsWorld& physics) {
    if (m_corridorSpawned) return;
    m_corridorSpawned = true;
    // The alarm encounter (x in [6,22]): a near pair of Guards that advance on the
    // player, then a flanking Drone pair further down that keep a ranged standoff —
    // a sensible escalation as the player pushes from the cell toward the armory.
    // (3 guards + 2 drones = 5; was 2+1. The combat AI makes them advance/flank.)
    // Spire B1: the corridor zone occupies low X (between the cell spawn and the
    // armory pickup) so the alarm enemies stay clear of the later checkpoint fight
    // (the firing ray hits the nearest Enemy-layer body, so the two encounters must
    // not interleave in the compressed basement). Centered on corridorCenter.
    const float cx = m_layout.corridorCenter.x;     // ~7.0 on B1
    const float cz = m_layout.corridorCenter.z;
    // Near guards — advance from just past Door A.
    m_corridor.spawn(scene, device, physics, m_modelDir,
                     x3::phys::Vec3{ cx - 1.2f, kEnemyY, cz - 1.6f }, guardTuning());
    m_corridor.spawn(scene, device, physics, m_modelDir,
                     x3::phys::Vec3{ cx - 0.6f, kEnemyY, cz + 1.6f }, guardTuning());
    // Mid attacker — backs up the front pair. BESTIARY PASS: this slot is now a
    // Verthani (data-driven roster: insectoid, faster, strafe-heavy flanker) instead
    // of a plain guard, so the alarm wave shows the new species in the actual level.
    // ADDITIVE: same enemy COUNT (5) + same MELEE role, so --test-level1 is unchanged.
    m_corridor.spawn(scene, device, physics, m_modelDir,
                     x3::phys::Vec3{ cx + 0.2f, kEnemyY, cz },
                     tuningFor(EnemyType::Verthani));
    // Flanking synths — hang back near the armory side and snipe down the corridor.
    // BESTIARY PASS: these two ranged slots are BlueSynth (synthetic flier from the
    // roster) instead of the legacy drone tuning — same RANGED role + count.
    m_corridor.spawn(scene, device, physics, m_modelDir,
                     x3::phys::Vec3{ cx + 1.0f, kEnemyY, cz - 2.0f },
                     tuningFor(EnemyType::BlueSynth));
    m_corridor.spawn(scene, device, physics, m_modelDir,
                     x3::phys::Vec3{ cx + 1.0f, kEnemyY, cz + 2.0f },
                     tuningFor(EnemyType::BlueSynth));
    x3::logInfo("Level1: ALARM — corridor enemies spawned (2 troopers + 1 Verthani + 2 BlueSynth)");
}

void Level1Game::spawnMartinez(Scene& scene, x3::rhi::IRenderDevice& device,
                               x3::phys::IPhysicsWorld& physics) {
    if (m_martinezSpawned) return;
    m_martinezSpawned = true;
    m_martinez.buildMonsterTuned(scene, device, physics, m_modelDir,
                                 x3::phys::Vec3{ m_layout.arenaCenter.x, 0.6f,
                                                 m_layout.arenaCenter.z },
                                 martinezTuning());
    if (m_cueSink) m_martinez.setCueSink(m_cueSink);   // inherit footstep/impact cues
    if (m_deathFx) m_martinez.setDeathFxSink(m_deathFx); // inherit the gib-burst death FX
    x3::logInfo("Level1: BOSS — Chief Martinez spawned in the arena (boss-tier HP/speed)");
}

// ---- F2 Medical Bay boss: Dr. Chen (Corrupted), Wave-2 placement. Spawns on the F2
// plate, gated on the F2 ward hub (NOT at load), so he is part of the Medical Bay
// floor alongside the 3-victim rescue but never pursues a player still on B1. Single-
// body Boss via the Wave-1 roster (bossTuning(BossType::DrChen)): 3 phases + the
// KILL-vs-CURE outcome. Idempotent. ----
void Level1Game::spawnChen(Scene& scene, x3::rhi::IRenderDevice& device,
                           x3::phys::IPhysicsWorld& physics) {
    if (m_chenSpawned) return;
    m_chenSpawned = true;
    // Place on the F2 plate, off the elevator-doorway spine, in the +X open half (the
    // ward cluster sits on the F2 floor; Chen anchors the Medical Bay across from it).
    const float f2y = m_layout.floorBaseY[(uint32_t)L1Floor::F2];
    m_chen.spawn(scene, device, physics, m_modelDir,
                 x3::phys::Vec3{ 10.0f, f2y + kEnemyY, 0.0f },
                 bossTuning(BossType::DrChen));
    x3::logInfo("Level1: F2 MEDICAL BAY BOSS — Dr. Chen (Corrupted) spawned "
                "(3 phases; KILL-vs-CURE outcome)");
}

// ---- TEARDOWN: remove any in-flight death-ragdoll bodies (and Martinez's) while
// the physics world is still alive, BEFORE physics->shutdown(). See level1_game.h.
// Mirrors MonsterManager::shutdown() across every group + the single Martinez boss. -
void Level1Game::shutdown() {
    m_corridor.shutdown();
    m_checkpoint.shutdown();
    m_bossAdds.shutdown();
    m_chen.shutdown();
    m_martinez.shutdownRagdoll();
}

bool Level1Game::cureChen(Scene& scene, x3::phys::IPhysicsWorld& physics) {
    if (!m_chenSpawned || m_chen.count() == 0) return false;
    const bool cured = m_chen.at(0).cure(scene, physics);
    if (cured)
        x3::logInfo("Level1: F2 — Dr. Chen INCAPACITATED + CURED (100% cure; Chen survives as ally)");
    return cured;
}

void Level1Game::spawnBossAdds(Scene& scene, x3::rhi::IRenderDevice& device,
                               x3::phys::IPhysicsWorld& physics) {
    if (m_bossSummoned) return;
    m_bossSummoned = true;
    // Phase 3 "summons" beat: spawn N Guard adds flanking the arena center. The
    // count comes from the boss's tuning so the boss + host stay in agreement.
    const int n = m_martinez.summonCount();
    for (int i = 0; i < n; ++i) {
        const float side = (i % 2 == 0) ? -3.0f : 3.0f;
        const float fwd  = (float)(i / 2) * 2.0f;
        m_bossAdds.spawn(scene, device, physics, m_modelDir,
                         x3::phys::Vec3{ m_layout.arenaCenter.x - 3.0f + fwd, kEnemyY,
                                         m_layout.arenaCenter.z + side }, guardTuning());
    }
    x3::logInfo("Level1: MARTINEZ SUMMONS — " + std::to_string(n) + " Guard add(s) spawned");
}

void Level1Game::tick(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics,
                      const x3::phys::Vec3& eye, const x3::phys::Vec3& playerPos) {
    tick(dt, scene, physics, eye, playerPos, nullptr, AttackFxFn{});
}

void Level1Game::tick(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics,
                      const x3::phys::Vec3& eye, const x3::phys::Vec3& playerPos,
                      IDamageSink* player, const AttackFxFn& attackFx) {
    if (!m_built || !m_devicePtr) return;

    // ---- Melee cooldown advances (Phase 2b super-strength punch). ----
    m_melee.update(dt);

    // ---- Explosive barrels: detonate any shot this frame (scatter + chain + FX). ----
    m_barrels.update(dt);

    // ---- Doors advance ----
    m_doors.update(dt, scene, physics);

    // ---- Beat 3: Door A reaches Open -> spawn corridor enemies + advance objective.
    {
        DoorState a = doorState('A');
        bool aOpen = (a == DoorState::Open);
        if (aOpen && !m_doorAWasOpen) {
            spawnCorridorEnemies(scene, *m_devicePtr, physics);
            // objective 0 -> 1 ("Find weapons in the armory")
            if (m_objectives.current() == 0) m_objectives.advance();
        }
        m_doorAWasOpen = aOpen;
    }

    // ---- Pickup arming (beat 6) ----
    m_weapon.update(dt, scene, playerPos);
    if (m_weapon.hasWeapon() && !m_armedLatch) {
        m_armedLatch = true;
        // Beat 7: unlock + open Door C now that we are armed.
        uint32_t ci = doorIndex('C');
        if (ci != kNoLink && ci < m_doors.count()) {
            m_doors.unlockAndOpen(m_doors.at(ci));
            playSfx(m_audio.door, m_layout.doorC, 0.9f);
        }
        // objective 1 -> 2 ("Clear the security checkpoint")
        if (m_objectives.current() == 1) m_objectives.advance();
        if (m_audio.sys && m_audio.pickup.valid())
            m_audio.sys->playSound2D(m_audio.pickup, 0.8f, 1.0f);
        x3::logInfo("Level1: ARMED — Door C unlocked + opening");
    }

    // ---- Checkpoint cleared -> objective 2 -> 3 ("Defeat Chief Martinez"). Only
    // fires once the player is armed (they cannot meaningfully clear it before),
    // and only after every checkpoint enemy is dead. This advances the on-screen
    // text the moment the firefight is won, pointing the player at the boss arena. -
    if (m_armedLatch && !m_checkpointClearLatch &&
        m_checkpoint.count() > 0 && m_checkpoint.aliveCount() == 0) {
        m_checkpointClearLatch = true;
        if (m_objectives.current() == 2) m_objectives.advance();
        x3::logInfo("Level1: CHECKPOINT CLEAR — advance to the boss arena");
    }

    // ---- Phase banner flash decay (Phase 2b: "PHASE 2!/PHASE 3!"). ----
    if (m_phaseBannerTimer > 0.0f) {
        m_phaseBannerTimer -= dt;
        if (m_phaseBannerTimer <= 0.0f) { m_phaseBannerTimer = 0.0f; m_phaseBanner.clear(); }
    }

    // ---- Monsters update (now attack the player on cooldown, Phase 2a). Enemies
    // only attack while the player is alive; once dead they keep moving but stop
    // dealing damage (the player is respawning). ----
    IDamageSink* atkTarget = (player && player->isAlive()) ? player : nullptr;
    m_corridor.update(dt, scene, physics, eye, atkTarget, attackFx);
    m_checkpoint.update(dt, scene, physics, eye, atkTarget, attackFx);
    m_bossAdds.update(dt, scene, physics, eye, atkTarget, attackFx);
    // F2 Medical Bay boss (Dr. Chen): runs the same HP-keyed phase machine as Martinez
    // (3 phases; the cure window opens in Phase3). Spawned on the F2 hub (below).
    m_chen.update(dt, scene, physics, eye, atkTarget, attackFx);

    // ---- Beat 9b (Phase 2b): Martinez runs its HP-keyed phase machine. On a
    // phase transition the callback raises the "PHASE N!" HUD banner + plays a cue,
    // and on Phase 3 summons Guard adds once (the bible's "summons" beat). ----
    if (m_martinezSpawned) {
        const float kPhaseBannerTime = 2.2f;
        BossPhaseFn onPhase = [&](BossPhase p) {
            if (p == BossPhase::Phase2) {
                m_phaseBanner = "PHASE 2!  ENRAGED";
                x3::logInfo("Level1: MARTINEZ PHASE 2 — ENRAGE");
            } else if (p == BossPhase::Phase3) {
                m_phaseBanner = "PHASE 3!  DESPERATE";
                x3::logInfo("Level1: MARTINEZ PHASE 3 — DESPERATE (summoning adds)");
                spawnBossAdds(scene, *m_devicePtr, physics);
            }
            m_phaseBannerTimer = kPhaseBannerTime;
            playSfx(m_audio.death, m_layout.arenaCenter, 0.7f);  // reuse a sting cue
        };
        m_martinez.update(dt, scene, physics, eye, atkTarget, attackFx, onPhase);
    }

    // ---- Beat 10: Martinez dies -> Door E unlock + open + objective -> "Take the
    // elevator" (index 4). Advance the cursor up to the elevator step regardless of
    // where it currently sits, so the text is correct even if the player slipped
    // past the checkpoint without clearing it (the boss is the gate that matters). -
    if (m_martinezSpawned && !m_martinez.alive() && !m_martinezDeadLatch) {
        m_martinezDeadLatch = true;
        uint32_t ei = doorIndex('E');
        if (ei != kNoLink && ei < m_doors.count()) {
            m_doors.unlockAndOpen(m_doors.at(ei));
            playSfx(m_audio.door, m_layout.doorE, 0.9f);
        }
        m_triggers.setEnabled((uint32_t)L1Trigger::Elevator, true);  // arm the win
        // Catch the objective cursor up to the final "Take the elevator" step.
        constexpr uint32_t kElevatorObjective = 4;
        while (m_objectives.current() != kNoObjective &&
               m_objectives.current() < kElevatorObjective)
            m_objectives.advance();
        playSfx(m_audio.death, m_layout.arenaCenter, 1.0f);
        x3::logInfo("Level1: MARTINEZ DOWN — Door E opening; take the elevator");
    }

    // ---- F2 rescue system (spec §5): tick the victims (countdowns / companion
    // follow) + the transformed-victim bosses. Additive + self-contained: it spawns
    // its own bosses on timer-expiry and follows the player when rescued. The host
    // (main.cpp) pokes onRescue() on an E-edge and draws the timers/victims. ----
    m_rescue.tick(dt, scene, physics, playerPos);

    // ---- Code-locked trapdoor -> SECRET ROOM: tick the cell terminal blink + the
    // secret weapon pickup + the room's loot collection. Heals are applied here via
    // the IDamageSink when it exposes a heal hook; otherwise the host applies them
    // off the collected-count deltas (it owns the concrete Player). The hatch itself
    // animates via m_doors.update() above (it's registered in the same DoorSystem). ----
    m_secretRoom.tick(dt, scene, playerPos);

    // ---- Triggers (per-frame point test on the player position) ----
    for (uint32_t id : m_triggers.update(playerPos)) {
        switch ((L1Trigger)id) {
            case L1Trigger::Strength:
                // Beat 1: hide the "equipment" prop (strength discovery).
                if (m_layout.equipmentProp != kNoLink && m_layout.equipmentProp < scene.size())
                    scene.get(m_layout.equipmentProp).visible = false;
                x3::logInfo("Level1: STRENGTH DISCOVERED — equipment crushed");
                break;
            case L1Trigger::Arena:
                // Beat 9: open Door D + spawn Martinez.
                {
                    uint32_t di = doorIndex('D');
                    if (di != kNoLink && di < m_doors.count()) {
                        m_doors.unlockAndOpen(m_doors.at(di));
                        playSfx(m_audio.door, m_layout.doorD, 0.9f);
                    }
                }
                spawnMartinez(scene, *m_devicePtr, physics);
                break;
            case L1Trigger::Elevator:
                // Beat 11: WIN. Logged exactly once; sets the completed state.
                if (!m_complete) {
                    m_complete = true;
                    m_objectives.complete();  // clear "Take the elevator"
                    x3::logInfo("LEVEL 1 COMPLETE");
                }
                break;
            case L1Trigger::Hub:
                // PLAYTEST-FIX (Issue 2): the player reached the F2 MEDICAL BAY ward hub
                // — START the rescue countdowns now (not at load) AND place the F2 floor
                // boss Dr. Chen on the F2 plate (Wave-2). Idempotent + latched by the
                // trigger so it only fires once. Until this, the victims had no timer and
                // Chen was unplaced (so he never pursues a player still down on B1).
                if (!m_rescue.hubReached()) {
                    m_rescue.activate();
                    spawnChen(scene, *m_devicePtr, physics);
                    x3::logInfo("Level1: F2 MEDICAL BAY HUB REACHED — rescue timers started "
                                "(Aria/Keisha/Emily counting down) + Dr. Chen boss placed");
                }
                break;
        }
    }
}

bool Level1Game::onUse(const x3::phys::Vec3& eye, const x3::phys::Vec3& dir,
                       Scene& scene, x3::phys::IPhysicsWorld& physics) {
    bool opened = tryUse(eye, dir, 3.0f, scene, m_doors, physics);
    if (opened) playSfx(m_audio.door, eye, 0.9f);
    return opened;
}

bool Level1Game::aimedDoorPrompt(const x3::phys::Vec3& eye, const x3::phys::Vec3& dir,
                                 Scene& scene, x3::phys::IPhysicsWorld& physics,
                                 float reach, x3::phys::Vec3& anchor, bool& isOpen) {
    Door* d = pickAimedDoor(eye, dir, reach, scene, m_doors, physics);
    if (!d) return false;
    // A locked, fully-closed door can't be toggled by E (the keypad/event path
    // owns it) — don't promise "Press E to open" on it.
    if (d->locked && d->state == DoorState::Closed) return false;
    isOpen = (d->state == DoorState::Open || d->state == DoorState::Opening);
    // Stable anchor at the doorway opening (does NOT ride up with the slab), a
    // touch above the closed slab center so the text sits ~head height.
    anchor = x3::phys::Vec3{ d->closedPos.x, d->closedPos.y + 0.3f, d->closedPos.z };
    return true;
}

bool Level1Game::onRescue(const x3::phys::Vec3& playerPos, float range) {
    bool rescued = m_rescue.tryRescue(playerPos, range);
    if (rescued) playSfx(m_audio.pickup, playerPos, 0.9f);   // reuse the pickup cue
    return rescued;
}

FireResult Level1Game::onFire(const x3::phys::Vec3& eye, const x3::phys::Vec3& dir,
                              Scene& scene, x3::phys::IPhysicsWorld& physics,
                              int damage) {
    FireResult r;
    if (!m_weapon.hasWeapon()) return r;   // gate: only effective when armed
    // Explosive barrels: a shot that hits a barrel detonates it (independent of
    // whether it also hits a monster — the ray can pass a barrel on the way).
    {
        const float e3[3] = { eye.x, eye.y, eye.z };
        const float d3[3] = { dir.x, dir.y, dir.z };
        m_barrels.onShot(e3, d3);
    }
    // Fire across all three monster groups; the first live monster hit takes it.
    r = m_corridor.fire(eye, dir, scene, physics, damage);
    if (!r.hitMonster) {
        FireResult r2 = m_checkpoint.fire(eye, dir, scene, physics, damage);
        if (r2.hitMonster) r = r2;
        else if (!r.hit && r2.hit) r = r2;
    }
    if (!r.hitMonster && m_martinezSpawned && m_martinez.alive()) {
        FireResult r3 = m_martinez.fire(eye, dir, scene, physics, damage);
        if (r3.hitMonster) r = r3;
        else if (!r.hit && r3.hit) r = r3;
    }
    // Phase-3 boss ADDS (summoned during the Martinez fight). These were invincible
    // to GUNFIRE — onFire never included them (only melee did), so they could never
    // die and the area never cleared. Include them in the gun-damage chain.
    if (!r.hitMonster && m_bossAdds.count() > 0) {
        FireResult ra = m_bossAdds.fire(eye, dir, scene, physics, damage);
        if (ra.hitMonster) r = ra;
        else if (!r.hit && ra.hit) r = ra;
    }
    // F2 Medical Bay boss (Dr. Chen), if placed.
    if (!r.hitMonster && m_chenSpawned) {
        FireResult r4 = m_chen.fire(eye, dir, scene, physics, damage);
        if (r4.hitMonster) r = r4;
        else if (!r.hit && r4.hit) r = r4;
    }
    return r;
}

MeleeResult Level1Game::onMelee(const x3::phys::Vec3& eye, const x3::phys::Vec3& dir,
                                Scene& scene, x3::phys::IPhysicsWorld& physics) {
    // Super-strength punch across all manager-held enemy groups + the doors (the
    // brute-force). Works whether or not armed (the pistol is a separate verb).
    std::vector<MonsterManager*> groups{ &m_corridor, &m_checkpoint, &m_bossAdds, &m_chen };
    MeleeResult r = m_melee.strike(eye, dir, scene, physics, groups, &m_doors);
    if (r.onCooldown) return r;   // the strike was suppressed; nothing else to do

    // Martinez is a single MonsterSystem (not a manager), so handle it inline with
    // the same arc test the MeleeSystem uses for the groups.
    if (m_martinezSpawned && m_martinez.alive()) {
        const Entity& e = scene.get(m_martinez.entity());
        const x3::phys::Vec3 c{ e.transform[12], e.transform[13], e.transform[14] };
        if (inMeleeArc(eye, dir, c, kMeleeRange, kMeleeHalfAngle)) {
            x3::phys::Vec3 kb{ c.x - eye.x, 0.0f, c.z - eye.z };
            float kl = std::sqrt(kb.x * kb.x + kb.z * kb.z);
            if (kl < 1e-4f) kl = 1.0f;
            if (m_martinez.body().valid())
                physics.applyImpulse(m_martinez.body(),
                    x3::phys::Vec3{ kb.x / kl * kMeleeKnockback, 2.0f, kb.z / kl * kMeleeKnockback });
            bool killed = m_martinez.takeMeleeDamage(kMeleeDamage, scene, physics);
            ++r.enemiesHit;
            if (killed) ++r.enemiesKilled;
        }
    }
    if (r.doorForced) playSfx(m_audio.door, eye, 1.0f);
    if (r.enemiesKilled > 0) playSfx(m_audio.death, eye, 0.9f);
    return r;
}

void Level1Game::drawWorldExtras(x3::rhi::IRenderDevice& device,
                                 const x3::rhi::FrameContext& frame,
                                 const Scene& scene) const {
    m_envArt.draw(device, frame, scene);   // sci-fi env art (per-room cull via scene.roomVisible)
    m_barrels.render(frame);        // intact barrels + their tumbling debris
    m_weapon.drawPickup(device, frame, scene);
    m_corridor.drawAll(device, frame, scene);
    m_checkpoint.drawAll(device, frame, scene);
    m_bossAdds.drawAll(device, frame, scene);
    if (m_martinezSpawned) m_martinez.drawMonster(device, frame, scene);
    m_chen.drawAll(device, frame, scene);  // F2 Medical Bay boss: Dr. Chen (if placed)
    m_rescue.draw(device, frame, scene);   // F2 victims + transformed-victim bosses
}

void Level1Game::drawViewmodel(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                               float ex, float ey, float ez, float yaw, float pitch,
                               float yawOff, float pitchOff, float rollOff,
                               float fwd, float right, float down) const {
    m_weapon.drawViewmodel(device, frame, ex, ey, ez, yaw, pitch,
                           yawOff, pitchOff, rollOff, fwd, right, down);
}

void Level1Game::drawObjective(x3::rhi::IRenderDevice& device,
                               const x3::rhi::FrameContext& frame) const {
    m_objectives.drawCurrent(device, frame);
}

// ===========================================================================
// Headless self-test (--test-level1). T1-T6 (+ T7 objective flow).
// ===========================================================================
namespace {

int g_pass = 0, g_fail = 0;
void check(bool cond, const char* name) {
    if (cond) { ++g_pass; x3::logInfo(std::string("[level1-test] PASS ") + name); }
    else      { ++g_fail; x3::logError(std::string("[level1-test] FAIL ") + name); }
}

constexpr float kFixedDt = 1.0f / 60.0f;

// Headless IRenderDevice: the shared no-op test-double (app/headless_device.h).
// Mints monotonically-increasing valid handles so build() runs with no Vulkan;
// all draw/frame/camera calls are no-ops.
using HeadlessDevice = x3::game::HeadlessRenderDevice;

x3::phys::Vec3 sub(const x3::phys::Vec3& a, const x3::phys::Vec3& b) {
    return x3::phys::Vec3{ a.x - b.x, a.y - b.y, a.z - b.z };
}

// Step the controller + physics for n frames with the player held at `pos`,
// looking in `dir` is not needed here (no use/fire). eye == pos for triggers.
void run(Level1Game& g, Scene& s, x3::phys::IPhysicsWorld& p, x3::rhi::IRenderDevice& dev,
         const x3::phys::Vec3& pos, int frames) {
    (void)dev;
    for (int i = 0; i < frames; ++i) {
        g.tick(kFixedDt, s, p, pos, pos);
        p.step(kFixedDt);
        s.update(p);
    }
}

} // namespace

bool runLevel1SelfTest() {
    g_pass = g_fail = 0;

    // First fold in the unit self-tests for the new pure systems.
    bool objOk = runObjectiveSelfTest();
    bool trgOk = runTriggerSelfTest();
    check(objOk, "objective system unit checks");
    check(trgOk, "trigger system unit checks");

    std::unique_ptr<x3::phys::IPhysicsWorld> physics(x3::phys::createPhysicsWorld());
    physics->init();

    HeadlessDevice device;
    Scene scene;
    Level1Game game;
    game.setDevice(device);     // cache device for event spawns (see header note)
    game.build(scene, device, *physics, riggedDir());

    const Level1Layout& L = game.layout();

    // Helper to aim from an eye toward a target point.
    auto aimFromTo = [](const x3::phys::Vec3& eye, const x3::phys::Vec3& tgt) {
        return sub(tgt, eye);
    };

    // ---- T7 (objective flow start): first objective is the cell escape. ----
    check(game.objectives().current() == 0 &&
          game.objectives().currentLabel() == "Escape the detention cell",
          "T7a first objective = escape the cell");

    // ======================================================================
    // FLOOR-1 RELAY: the detention layout (docs/design/SPIRE_LEVELARCHITECT_DIMS.md)
    // is now built at real scale. Assert the footprint + key rooms + elevator align.
    // ======================================================================
    // ---- D1: the B1 plate grew from the old 24x16 placeholder to the real footprint;
    //          the DETENTION room-center span is ~75 (X) x ~43 (Z) m (the doc's stated
    //          footprint: X -20..+55 = 75 m, Z -36..+7 = 43 m). The raw plate + the
    //          half-extent AABB are larger supersets. ----
    {
        const L1RoomDef* tbl = level1Rooms();
        const L1RoomDef& b1 = tbl[(uint32_t)L1Floor::B1];
        const float plateW = b1.x1 - b1.x0, plateD = 2.0f * b1.zHalf;
        const L1DetentionRoom* dr = level1DetentionRooms();
        float minCx = 1e9f, maxCx = -1e9f, minCz = 1e9f, maxCz = -1e9f;
        for (uint32_t i = 0; i < level1DetentionRoomCount(); ++i) {
            minCx = std::min(minCx, dr[i].cx); maxCx = std::max(maxCx, dr[i].cx);
            minCz = std::min(minCz, dr[i].cz); maxCz = std::max(maxCz, dr[i].cz);
        }
        const float spanX = maxCx - minCx, spanZ = maxCz - minCz;
        bool plateGrew = plateW >= 70.0f && plateD >= 40.0f;            // no longer 24x16
        bool spanOk = spanX >= 73.0f && spanX <= 77.0f &&               // ~75 m wide (X -20..+55)
                      spanZ >= 41.0f && spanZ <= 45.0f;                 // ~43 m deep (Z -36..+7)
        // The half-extent AABB is a sane superset that fits inside the raw B1 plate.
        L1Footprint fp = level1DetentionFootprint();
        bool aabbInPlate = fp.minX >= b1.x0 - 0.01f && fp.maxX <= b1.x1 + 0.01f &&
                           fp.minZ >= -b1.zHalf - 0.01f && fp.maxZ <= b1.zHalf + 0.01f;
        check(plateGrew && spanOk && aabbInPlate,
              "D1 F1 footprint grew to ~75x43 m detention complex (was 24x16)");
    }
    // ---- D2: the 29 authored rooms are present with the right key dimensions. ----
    {
        const L1DetentionRoom* dr = level1DetentionRooms();
        bool count29 = level1DetentionRoomCount() == 29;
        auto dim = [&](uint32_t i, float w, float h, float d) {
            const L1DetentionRoom& r = dr[i];
            return std::fabs(r.w - w) < 0.01f && std::fabs(r.h - h) < 0.01f &&
                   std::fabs(r.d - d) < 0.01f;
        };
        constexpr uint32_t kCellBlockBHall = 23;              // Cell Block B Hallway index
        bool jake   = std::string(dr[kDetJakeCell].name) == "Jake's Cell" && dim(kDetJakeCell, 7,4,6);
        bool mainHall = dim(kDetMainHallway, 3,3.5f,26);      // Main Hallway 3x3.5x26
        bool cbHall   = dim(kCellBlockBHall, 4,3.5f,32);      // Cell Block B Hallway 4x3.5x32
        bool cavern   = dim(kDetCrystalCavern, 18,8,16);      // Crystal Cavern 18x8x16
        bool armory   = dim(kDetArmory, 5,3.5f,5);
        check(count29 && jake && mainHall && cbHall && cavern && armory,
              "D2 29 rooms present with key dims (Jake 7x4x6, MainHall 3x3.5x26, CBHall 4x3.5x32, Cavern 18x8x16)");
    }
    // ---- D3: Sarah's empty cell (npc flag) + a monster cell exist; the elevator-lobby
    //          room is authored and the functional elevator shaft lands on walkable
    //          floor (z=0 lane, x at the shaft) so a ride arrives in the complex. ----
    {
        const L1DetentionRoom* dr = level1DetentionRooms();
        bool sarah = std::string(dr[kDetSarahCell].name).find("Sarah") != std::string::npos &&
                     dr[kDetSarahCell].npc;
        uint32_t monsters = 0;
        for (uint32_t i = 0; i < level1DetentionRoomCount(); ++i) if (dr[i].monster) ++monsters;
        bool elevLobby = std::string(dr[kDetElevatorLobby].name) == "Elevator Lobby";
        // The functional shaft (legacy elevatorCenter) is at z=0 inside the plate so the
        // ride lands on the playable spine lane (walkable on every floor).
        const L1RoomDef& b1r = level1Rooms()[(uint32_t)L1Floor::B1];
        bool shaftWalkable = std::fabs(L.elevatorCenter.z) < 0.01f &&
                             L.elevatorCenter.x > b1r.x0 && L.elevatorCenter.x < b1r.x1;
        check(sarah && monsters >= 4 && elevLobby && shaftWalkable,
              "D3 Sarah's empty cell (npc) + monster cells + Elevator Lobby; shaft lands on walkable floor");
    }

    // ---- Beat 1: walk to the strength trigger; equipment hidden. ----
    {
        // Equipment prop is at (1.5,0.4,-1.8); the trigger box covers it.
        run(game, scene, *physics, device, x3::phys::Vec3{ 1.5f, 0.05f, -1.8f }, 2);
        bool hidden = (L.equipmentProp != kNoLink) && !scene.get(L.equipmentProp).visible;
        check(hidden, "beat1 strength trigger hides the equipment prop");
    }

    // ---- T1: use Door A button -> Door A opens; player can walk into corridor.
    {
        // Find Door A's button entity: it's the Button whose link is Door A's entity.
        // Easier: aim from a point in front of Door A's button at it. The button is
        // mounted at x = doorA.x - ht - 0.12, z = doorA.x... we know buildLevelDoor
        // places it at (doorwayCenter.x - 0.1 - 0.12, 1.3, doorwayCenter.z + hw+0.5).
        // Reconstruct that and aim a use-ray at it.
        x3::phys::Vec3 btn{ L.doorA.x - 0.10f - 0.12f, 1.3f, L.doorA.z + 0.6f + 0.5f };
        x3::phys::Vec3 eye{ btn.x - 1.5f, btn.y, btn.z };  // in front of the button (−X side)
        bool used = game.onUse(eye, aimFromTo(eye, btn), scene, *physics);
        // Step the door open.
        run(game, scene, *physics, device, x3::phys::Vec3{ L.spawn.x, 0.05f, 0.0f }, 80);
        bool open = game.doorState('A') == DoorState::Open;
        check(used && open, "T1 use Door A button -> Door A opens");
    }

    // ---- T2: once Door A is open, the alarm encounter exists in the corridor
    // (>=2 guards + >=1 drone). D-content escalates this to 3 guards + 2 drones. ----
    {
        bool spawned = game.corridorEnemies().count() == 5;
        // 3 guards + 2 drones => 5 monsters, all alive at spawn.
        bool allAlive = game.corridorEnemies().aliveCount() == 5;
        check(spawned && allAlive, "T2 alarm spawns 3 guards + 2 drones in corridor");
    }

    // ---- T7b: objective advanced to the armory step after Door A. ----
    check(game.objectives().currentLabel() == "Fight to the armory and arm yourself",
          "T7b objective advances after Door A opens");

    // ---- T4 (locked gate, part 1): Door C does NOT open before armed. The player has
    // only just opened Door A and is still at spawn -- they have not yet stepped into
    // the cell to grab the sidearm, so they are unarmed and Door C must stay shut. ----
    {
        x3::phys::Vec3 btn{ L.doorC.x - 0.10f - 0.12f, 1.3f, L.doorC.z + 0.6f + 0.5f };
        x3::phys::Vec3 eye{ btn.x - 1.5f, btn.y, btn.z };
        bool usedLocked = game.onUse(eye, aimFromTo(eye, btn), scene, *physics);
        bool stillClosed = game.doorState('C') == DoorState::Closed;
        check(!usedLocked && stillClosed && game.doorLocked('C'),
              "T4a Door C stays locked + closed before armed");
    }

    // ---- T3: step into the cell onto the starting sidearm pickup -> armed. The pistol
    // sits in the detention cell so Jake is never defenseless against the corridor alarm
    // enemies; the armory's Door C (below) is the gate he opens once armed. ----
    {
        run(game, scene, *physics, device,
            x3::phys::Vec3{ L.cellCenter.x, 0.05f, L.cellCenter.z }, 6);
        check(game.armed(), "T3 grabbing the cell sidearm arms the player");
    }

    // ---- Corridor clear (gameplay-faithful): on the compact Spire B1, Jake fights
    // through the alarm enemies en route to the armory. Now armed, he melees them down
    // so they don't trail into the later checkpoint encounter (the fire ray hits the
    // nearest Enemy body, so the encounters must not interleave).
    {
        const float cx = L.corridorCenter.x, cz = L.corridorCenter.z;
        x3::phys::Vec3 eye{ cx - 4.0f, 0.6f, cz };
        for (int guard = 0; guard < 200 && game.corridorEnemies().aliveCount() > 0; ++guard) {
            MonsterManager& co = game.corridorEnemies();
            for (uint32_t i = 0; i < co.count(); ++i) {
                if (co.at(i).alive()) {
                    const Entity& e = scene.get(co.at(i).entity());
                    x3::phys::Vec3 t{ e.transform[12], e.transform[13], e.transform[14] };
                    x3::phys::Vec3 ey{ t.x - 1.0f, 0.6f, t.z };   // melee range
                    game.onMelee(ey, aimFromTo(ey, t), scene, *physics);
                    break;
                }
            }
            run(game, scene, *physics, device, eye, 2);
        }
    }

    // ---- T4 (locked gate, part 2): Door C unlocked + opening after arming. ----
    {
        run(game, scene, *physics, device,
            x3::phys::Vec3{ L.armoryCenter.x, 0.05f, L.armoryCenter.z }, 80);
        bool openedAfter = game.doorState('C') == DoorState::Open;
        check(!game.doorLocked('C') && openedAfter, "T4b Door C opens after armed");
    }

    // ---- T7c: objective advanced to "Clear the security checkpoint" after pickup. ----
    check(game.objectives().currentLabel() == "Clear the security checkpoint",
          "T7c objective advances after pickup");

    // ---- Checkpoint encounter: it exists (4 enemies) and clearing it advances the
    // objective to "Defeat Chief Martinez". Shoot each checkpoint enemy dead from a
    // point near the checkpoint, re-aiming per target. ----
    {
        bool spawned = game.checkpointEnemies().count() == 4;
        // Spire B1: the checkpoint is a compact walled sub-room between Door C (x=12.5)
        // and Door D (x=15). Fire from just INSIDE it (past Door C's collision slab)
        // so the shots reach the checkpoint enemies without a door body intercepting
        // the Enemy-layer ray (the alarm enemies were already cleared above).
        x3::phys::Vec3 eye{ L.checkpointCenter.x - 0.9f, 0.6f, L.checkpointCenter.z };
        for (int guard = 0; guard < 100 && game.checkpointEnemies().aliveCount() > 0; ++guard) {
            // Aim at the first still-alive checkpoint enemy.
            MonsterManager& cp = game.checkpointEnemies();
            for (uint32_t i = 0; i < cp.count(); ++i) {
                if (cp.at(i).alive()) {
                    const Entity& e = scene.get(cp.at(i).entity());
                    x3::phys::Vec3 t{ e.transform[12], e.transform[13], e.transform[14] };
                    game.onFire(eye, aimFromTo(eye, t), scene, *physics);
                    break;
                }
            }
            run(game, scene, *physics, device, eye, 2);
        }
        bool cleared = game.checkpointEnemies().aliveCount() == 0;
        check(spawned && cleared, "checkpoint encounter (4 enemies) can be cleared");
        check(game.objectives().currentLabel() == "Defeat Chief Martinez",
              "T7c2 objective advances to the boss after checkpoint cleared");
    }

    // ---- Cross the arena trigger -> Door D opens + Martinez spawns. ----
    {
        run(game, scene, *physics, device,
            x3::phys::Vec3{ L.doorD.x + 2.0f, 0.05f, 0.0f }, 80);
        check(game.martinezSpawned() && game.martinezAlive(),
              "beat9 arena trigger spawns Martinez");
    }

    // ---- T5: Door E stays closed until Martinez HP reaches 0, then opens. ----
    {
        bool closedBefore = game.doorState('E') == DoorState::Closed && game.doorLocked('E');
        // Shoot Martinez from a fixed eye until dead. Aim at the arena center.
        x3::phys::Vec3 tgt{ L.arenaCenter.x, 0.6f, L.arenaCenter.z };
        x3::phys::Vec3 eye{ L.arenaCenter.x - 4.0f, 0.6f, L.arenaCenter.z };
        x3::phys::Vec3 dir = aimFromTo(eye, tgt);
        for (int i = 0; i < 30 && game.martinezAlive(); ++i) {
            game.onFire(eye, dir, scene, *physics);
            // Keep the boss in place enough to keep hitting it: re-aim each shot.
            run(game, scene, *physics, device, eye, 2);
            dir = aimFromTo(eye, x3::phys::Vec3{ L.arenaCenter.x, 0.6f, L.arenaCenter.z });
        }
        // Let Door E animate open.
        run(game, scene, *physics, device, eye, 80);
        bool deadNow = game.martinezDead();
        bool openAfter = game.doorState('E') == DoorState::Open;
        check(closedBefore && deadNow && openAfter,
              "T5 Door E closed until Martinez dead, then opens");
    }

    // ---- T7c3: after the boss dies the objective points at the elevator. ----
    check(game.objectives().currentLabel() == "Take the elevator to Floor 2",
          "T7c3 objective advances to the elevator after Martinez dies");

    // ---- T6: entering the elevator trigger AFTER the boss is dead -> complete once.
    {
        bool notCompleteYet = !game.complete();
        // Walk into the elevator room.
        run(game, scene, *physics, device,
            x3::phys::Vec3{ L.elevatorCenter.x, 0.05f, 0.0f }, 3);
        bool completeNow = game.complete();
        // Re-entering does not "complete" a second time (latch). Step again.
        run(game, scene, *physics, device,
            x3::phys::Vec3{ L.elevatorCenter.x, 0.05f, 0.0f }, 3);
        bool stillComplete = game.complete();
        check(notCompleteYet && completeNow && stillComplete,
              "T6 elevator trigger after boss dead -> level complete (once)");
    }

    // ---- T7d: objective list finished after taking the elevator. ----
    check(game.objectives().allComplete(), "T7d all objectives complete at the end");

    // Tear down any in-flight death ragdolls (the boss died in T5) while the Jolt
    // world is still alive — without this, ~Level1Game would later remove ragdoll
    // bodies from an already-shut-down physics world (crash on exit). Mirrors the
    // --test-nexus body-owner teardown discipline.
    game.shutdown();
    physics->shutdown();
    x3::logInfo(std::string("[level1-test] ") + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

// ===========================================================================
// Door-code keypad self-test (--test-doorcode). K1-K6.
// Builds Level 1 and exercises Door C's keypad (code 1127) headlessly: proximity
// gating, the wrong-code reject, the right-code open, and the KeypadEntry state
// machine (digit/backspace/clear/value/prompt) the host wires to GLFW key edges.
// ===========================================================================
bool runDoorCodeSelfTest() {
    g_pass = g_fail = 0;

    std::unique_ptr<x3::phys::IPhysicsWorld> physics(x3::phys::createPhysicsWorld());
    physics->init();

    HeadlessDevice device;
    Scene scene;
    Level1Game game;
    game.setDevice(device);
    game.build(scene, device, *physics, riggedDir());

    const Level1Layout& L = game.layout();

    // A point standing right at Door C's doorway (within the default 3.5 m range).
    const x3::phys::Vec3 atDoorC{ L.doorC.x, 1.7f, L.doorC.z };
    // A point far away from any coded door (out of range of Door C).
    const x3::phys::Vec3 farAway{ L.doorC.x, 1.7f, L.doorC.z + 50.0f };

    // ---- K1: Door C is the keypad door — locked, with a code, and the player is
    // detected as "near a locked coded door" only when actually close to it. ----
    check(game.doorLocked('C'), "K1a Door C starts LOCKED (keypad gate)");
    check(game.nearLockedCodedDoor(atDoorC) && !game.nearLockedCodedDoor(farAway),
          "K1b nearLockedCodedDoor true at Door C, false far away");

    // ---- K2: the KeypadEntry state machine builds the code from key edges. ----
    {
        KeypadEntry kp;
        check(kp.empty() && kp.value() == -1, "K2a fresh keypad is empty (value -1)");
        kp.pushDigit(1); kp.pushDigit(1); kp.pushDigit(2); kp.pushDigit(7);
        check(kp.buf == "1127" && kp.value() == 1127, "K2b digits 1,1,2,7 -> 1127");
        kp.backspace();
        check(kp.buf == "112" && kp.value() == 112, "K2c backspace removes last digit");
        kp.pushDigit(7);
        check(kp.value() == 1127, "K2d re-entered digit restores 1127");
        // Cap at 6 digits (extra digits ignored).
        KeypadEntry cap;
        for (int i = 0; i < 9; ++i) cap.pushDigit(9);
        check(cap.buf.size() == (size_t)KeypadEntry::kMaxLen, "K2e entry capped at 6 digits");
        // Out-of-range digit is ignored; HUD prompt tracks the buffer.
        KeypadEntry pr; pr.pushDigit(11); pr.pushDigit(1); pr.pushDigit(2);
        check(pr.buf == "12" && pr.prompt() == "DOOR LOCKED   ENTER CODE: 12_",
              "K2f bad digit ignored; prompt reflects entry");
    }

    // ---- K3: WRONG code does NOT open Door C (and it stays locked). ----
    {
        KeypadEntry kp; kp.pushDigit(9); kp.pushDigit(9); kp.pushDigit(9); kp.pushDigit(9);
        bool opened = game.tryDoorCode(atDoorC, kp.value());
        bool stillClosed = game.doorState('C') == DoorState::Closed;
        check(!opened && stillClosed && game.doorLocked('C'),
              "K3 wrong code (9999) does NOT open Door C; stays locked+closed");
    }

    // ---- K4: cancelling (clear) before submit leaves the door untouched. ----
    {
        KeypadEntry kp; kp.pushDigit(1); kp.pushDigit(1); kp.clear();
        check(kp.empty() && game.doorState('C') == DoorState::Closed && game.doorLocked('C'),
              "K4 cancel (clear) leaves Door C locked+closed");
    }

    // ---- K5: the RIGHT code (1127) unlocks + opens Door C. ----
    {
        KeypadEntry kp; kp.pushDigit(1); kp.pushDigit(1); kp.pushDigit(2); kp.pushDigit(7);
        bool opened = game.tryDoorCode(atDoorC, kp.value());
        // Step the door system so it animates to Open.
        run(game, scene, *physics, device, atDoorC, 80);
        bool nowOpen = game.doorState('C') == DoorState::Open;
        check(opened && !game.doorLocked('C') && nowOpen,
              "K5 correct code (1127) unlocks + opens Door C");
    }

    // ---- K6: out-of-range submit with the right code does NOT open a fresh door.
    // (Use a brand-new level so Door C is closed/locked again.) ----
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> p2(x3::phys::createPhysicsWorld());
        p2->init();
        HeadlessDevice dev2;
        Scene s2;
        Level1Game g2;
        g2.setDevice(dev2);
        g2.build(s2, dev2, *p2, riggedDir());
        const Level1Layout& L2 = g2.layout();
        x3::phys::Vec3 far2{ L2.doorC.x, 1.7f, L2.doorC.z + 50.0f };
        bool opened = g2.tryDoorCode(far2, 1127);   // right code, but out of range
        check(!opened && g2.doorState('C') == DoorState::Closed && g2.doorLocked('C'),
              "K6 right code out of range does NOT open Door C");
        p2->shutdown();
    }

    physics->shutdown();
    x3::logInfo(std::string("[doorcode-test] ") + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

// ===========================================================================
// Advanced-elevator self-test (--test-elevator). E1-E6.
// Builds an ElevatorSystem (cab platform = moved static body, same technique as
// DoorSystem) with two stops in a 9 m shaft (ground + 6 m) on a headless device +
// Jolt world, and exercises the call/travel/carry mechanism that mediates the
// Level 1 exit lift. No window/Vulkan.
// ===========================================================================
bool runElevatorSelfTest() {
    g_pass = g_fail = 0;

    std::unique_ptr<x3::phys::IPhysicsWorld> physics(x3::phys::createPhysicsWorld());
    physics->init();

    HeadlessDevice device;
    Scene scene;

    // Two-stop cab in a tall shaft at the origin: ground (cab center y=0.15, top at
    // floor) + 6 m up. Cab half-extents 1.4 x 0.15 x 1.4 (matches main.cpp wiring).
    const float cabHY = 0.15f;
    const float groundY = cabHY;          // cab CENTER at the room floor
    const float topY    = cabHY + 6.0f;   // cab CENTER 6 m up
    ElevatorSystem elev;
    bool built = elev.build(scene, device, *physics,
                            /*shaftX*/0.0f, /*shaftZ*/0.0f,
                            1.4f, cabHY, 1.4f, std::vector<float>{ groundY, topY }, /*startStop*/0);
    // Speed up the ride so the test converges in a bounded frame count.
    elev.setSpeed(8.0f);

    // ---- E1: builds, idle at the low stop, 2 stops, cab center at the ground stop.
    check(built && elev.built() && !elev.moving() && elev.stopCount() == 2 &&
          std::fabs(elev.cabCenter().y - groundY) < 1e-3f,
          "E1 elevator builds: idle at the ground stop, 2 stops");

    // ---- E2: a rider standing on the cab top is detected; one beside it is not.
    {
        const x3::phys::Vec3 onTop{ 0.0f, elev.cabTopY() + 0.05f, 0.0f };
        const x3::phys::Vec3 beside{ 5.0f, elev.cabTopY() + 0.05f, 0.0f };
        check(elev.playerRiding(onTop) && !elev.playerRiding(beside),
              "E2 playerRiding: feet ON the cab detected, feet OFF not");
    }

    // ---- E3: callNext() starts the cab moving toward the high stop.
    {
        elev.callNext();
        check(elev.moving() && elev.targetStop() == 1,
              "E3 callNext starts moving toward the top stop");
    }

    // ---- E4: stepping update() carries the cab UP and arrives at the high stop;
    // a simulated rider's feet rise by the SAME total delta (the carry contract).
    {
        x3::phys::Vec3 feet{ 0.0f, elev.cabTopY() + 0.05f, 0.0f };
        float carried = 0.0f;
        int frames = 0;
        for (; frames < 600 && elev.moving(); ++frames) {
            float edy = elev.update(kFixedDt, scene, *physics);
            if (edy != 0.0f && elev.playerRiding(feet)) { feet.y += edy; carried += edy; }
        }
        bool arrived = !elev.moving() && std::fabs(elev.cabCenter().y - topY) < 1e-3f;
        bool carriedRight = std::fabs(carried - (topY - groundY)) < 1e-2f;
        check(arrived && carriedRight,
              "E4 update carries the cab up to the top stop; rider lifted by the full 6 m");
    }

    // ---- E5: the cab's physics body tracks the cab center (so it blocks/stands
    // like ground at the new stop — what the level-exit lift relies on).
    {
        x3::phys::Vec3 bodyPos = physics->getBodyPosition(scene.get(0).body);
        // Entity 0 is the cab (first thing added to the fresh scene).
        check(std::fabs(bodyPos.y - elev.cabCenter().y) < 1e-3f &&
              std::fabs(bodyPos.y - topY) < 1e-3f,
              "E5 cab physics body tracks the cab center at the top stop");
    }

    // ---- E6: callNext() again returns the cab DOWN to the ground stop.
    {
        elev.callNext();
        bool wraps = elev.moving() && elev.targetStop() == 0;
        for (int i = 0; i < 600 && elev.moving(); ++i)
            elev.update(kFixedDt, scene, *physics);
        bool home = !elev.moving() && std::fabs(elev.cabCenter().y - groundY) < 1e-3f;
        check(wraps && home, "E6 callNext wraps top->ground; cab returns to the floor stop");
    }

    physics->shutdown();
    x3::logInfo(std::string("[elevator-test] ") + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

// ===========================================================================
// EFLZ <-> save::SaveState bridge (programmatic save/load API). See level1_game.h.
// ===========================================================================
namespace {

// "enemies cleared" on a manager = spawned-so-far minus still-alive. count() is the
// number ever spawned into the manager; aliveCount() is those not yet dead.
uint32_t clearedOf(const MonsterManager& m) {
    const uint32_t total = m.count();
    const uint32_t alive = m.aliveCount();
    return (alive <= total) ? (total - alive) : 0u;
}

// Count doors that have reached the Open state across a DoorSystem, and how many of
// those carried a keypad code (== "keypads solved").
void countDoors(const DoorSystem& doors, uint32_t& opened, uint32_t& keypadsSolved) {
    opened = 0; keypadsSolved = 0;
    for (uint32_t i = 0; i < doors.count(); ++i) {
        const Door& d = doors.at(i);
        if (d.state == DoorState::Open) {
            ++opened;
            if (d.code != 0) ++keypadsSolved;
        }
    }
}

} // namespace

x3::save::SaveState captureCheckpoint(const Player& player, const Arsenal& arsenal,
                                      const Level1Game& game,
                                      const SpireMidFloors& mid, const SpireTopFloors& top,
                                      uint32_t currentFloor) {
    x3::save::SaveState s;

    // ---- Player transform + look + health ----
    const x3::phys::Vec3 feet = player.feet();
    s.playerX = feet.x; s.playerY = feet.y; s.playerZ = feet.z;
    s.playerYaw = player.yaw(); s.playerPitch = player.pitch();
    s.playerHp = player.hp(); s.playerMaxHp = player.maxHp();

    // ---- Inventory (arsenal) ----
    s.equippedWeapon = (uint32_t)(arsenal.selected() < 0 ? 0 : arsenal.selected());
    s.weapons.reserve((size_t)arsenal.count());
    for (int i = 0; i < arsenal.count(); ++i) {
        const Arsenal::WeaponState& ws = arsenal.state(i);
        x3::save::WeaponSave w;
        w.ammoInMag = (uint32_t)(ws.ammoInMag < 0 ? 0 : ws.ammoInMag);
        w.reserve   = (uint32_t)(ws.reserve   < 0 ? 0 : ws.reserve);
        s.weapons.push_back(w);
    }

    // ---- Progression ----
    s.levelIndex     = 0;                  // EFLZ Level 1
    s.currentFloor   = currentFloor;
    s.objectiveIndex = game.objectives().current();   // kNoObjective (0xFFFFFFFF) when finished
    s.levelComplete  = game.complete() ? 1u : 0u;

    // ---- Rescue (F2 hub + per-victim lifecycle + remaining timer) ----
    const RescueSystem& rescue = game.rescue();
    s.rescueHubReached = rescue.hubReached() ? 1u : 0u;
    s.rescue.reserve(rescue.victimCount());
    for (uint32_t i = 0; i < rescue.victimCount(); ++i) {
        const RescueVictim& v = rescue.victim(i);
        x3::save::RescueSave r;
        r.state    = (uint32_t)v.state();
        r.timeLeft = v.timeLeft();
        s.rescue.push_back(r);
    }

    // ---- Per-floor world flags (doors / keypads / enemies cleared) ----
    // B1 (Level-1 base): the corridor + checkpoint + Martinez groups, and doors A-E.
    {
        x3::save::FloorFlags f;
        f.floorIndex = (uint32_t)L1Floor::B1;
        // The DoorSystem inside Level1Game isn't directly exposed, so derive door
        // opened/solved from the per-letter door state queries (A..E).
        uint32_t opened = 0, solved = 0;
        for (char L = 'A'; L <= 'E'; ++L) {
            if (game.doorState(L) == DoorState::Open) {
                ++opened;
                // Door C carries the keypad code in EFLZ Level 1; count it solved when open
                // AND it was a coded/locked door (it is unlocked-then-opened via the keypad).
                if (L == 'C') ++solved;
            }
        }
        f.doorsOpened    = opened;
        f.keypadsSolved  = solved;
        f.enemiesCleared = clearedOf(game.corridorEnemies()) +
                           clearedOf(game.checkpointEnemies()) +
                           (game.martinezDead() ? 1u : 0u);
        f.enemiesTotal   = game.corridorEnemies().count() +
                           game.checkpointEnemies().count() +
                           (game.martinezSpawned() ? 1u : 0u);
        s.floors.push_back(f);
    }
    // F2: rescue floor — "enemies cleared" tracked via rescued/expired bookkeeping.
    {
        x3::save::FloorFlags f;
        f.floorIndex     = (uint32_t)L1Floor::F2;
        f.enemiesCleared = rescue.rescuedCount();        // victims secured
        f.enemiesTotal   = rescue.victimCount();
        s.floors.push_back(f);
    }
    // F3/F4/F5 (Spire mid floors), if built.
    if (mid.built()) {
        const SpireMidFloor order[3] = { SpireMidFloor::F3, SpireMidFloor::F4, SpireMidFloor::F5 };
        for (SpireMidFloor mf : order) {
            const SpireFloorPlan& plan = mid.plan(mf);
            x3::save::FloorFlags f;
            f.floorIndex     = (uint32_t)plan.floor;
            f.enemiesCleared = clearedOf(mid.enemies(mf));
            f.enemiesTotal   = plan.totalCount;
            countDoors(mid.doors(), f.doorsOpened, f.keypadsSolved);
            s.floors.push_back(f);
        }
    }
    // F6/F7 (Spire top floors), if built.
    if (top.built()) {
        const SpireTopFloor order[2] = { SpireTopFloor::F6, SpireTopFloor::F7 };
        for (SpireTopFloor tf : order) {
            const SpireTopPlan& plan = top.plan(tf);
            x3::save::FloorFlags f;
            f.floorIndex     = (uint32_t)plan.floor;
            f.enemiesCleared = clearedOf(top.enemies(tf)) + clearedOf(top.boss());
            f.enemiesTotal   = plan.totalCount;
            countDoors(top.doors(), f.doorsOpened, f.keypadsSolved);
            s.floors.push_back(f);
        }
    }

    return s;
}

bool applyCheckpoint(const x3::save::SaveState& s, Player& player,
                     x3::phys::IPhysicsWorld& physics, Arsenal& arsenal,
                     Level1Game& game, SpireMidFloors& mid, SpireTopFloors& top,
                     uint32_t& outCurrentFloor) {
    (void)mid; (void)top;   // floor flags are captured/persisted; live re-sim is out of scope (see header)

    // ---- Player transform + look + health ----
    player.setFeetPosition(physics, x3::phys::Vec3{ s.playerX, s.playerY, s.playerZ });
    player.setLook(s.playerYaw, s.playerPitch);
    player.setHp(s.playerHp);

    // ---- Inventory (arsenal) ----
    std::vector<std::pair<int,int>> ammo;
    ammo.reserve(s.weapons.size());
    for (const auto& w : s.weapons) ammo.emplace_back((int)w.ammoInMag, (int)w.reserve);
    arsenal.restore((int)s.equippedWeapon, ammo);

    // ---- Progression: objective cursor + complete flag ----
    game.objectives().setCurrent(s.objectiveIndex);
    game.setComplete(s.levelComplete != 0);

    // ---- Rescue: hub + per-victim lifecycle + remaining timer ----
    RescueSystem::SaveState rs;
    rs.hubReached = (s.rescueHubReached != 0);
    rs.victims.reserve(s.rescue.size());
    for (const auto& r : s.rescue) rs.victims.push_back({ r.state, r.timeLeft });
    game.rescue().deserialize(rs);

    outCurrentFloor = s.currentFloor;
    x3::logInfo("[save] checkpoint applied to live game (floor " +
                std::to_string(s.currentFloor) + ", HP " + std::to_string(s.playerHp) + ")");
    return true;
}

} // namespace x3::game
