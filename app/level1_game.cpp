// EFLZ Level 1 game controller + --test-level1. See app/level1_game.h.
//
// Clean-room: built from the Scene/door/weapon/monster/objective/trigger systems
// and the engine interfaces only. No purchased C# / id Tech source consulted.
#include "level1_game.h"
#include "mesh_prims.h"

#include "engine/core/x3_log.h"

#include <cmath>
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
const char* kConvertedDir = "G:/GameModels/converted_glb";
// The original rigged GLBs: these retain their skeleton AND an "Idle" animation
// clip (the converted_glb/Characters export dropped the animation, leaving only a
// bind-pose skin). For J1 (make the characters ANIMATE) the humanoid enemies load
// the rigged source so they play their idle clip via CPU skinning. Authored Y-up
// (feet at y=0, head ~1.8 m), so NO Z->Y stand-up is needed.
const char* kRiggedDir = "G:/GameModels/rigged_glb";

// Apply the converted-character model to a tuning (file under Characters/, the
// converted dir, the Z->Y stand-up, and a real-scale humanoid size).
void useCharacter(MonsterSystem::Tuning& t, const char* file, float scale) {
    t.modelFile        = std::string("Characters/") + file;
    t.modelDirOverride = kConvertedDir;
    t.standUpZtoY      = true;          // converted characters are authored Z-up
    t.modelScale       = scale;         // ~1.0 => ~1.77 m tall humanoid
}

// Apply a RIGGED + ANIMATED source character (J1). Same role as useCharacter but
// from rigged_glb (so the MonsterSystem's Skinner finds the "Idle" clip and plays
// it). Y-up, so standUpZtoY stays false. On load failure the per-enemy box
// fallback still applies (the level never breaks).
void useRiggedCharacter(MonsterSystem::Tuning& t, const char* file, float scale) {
    t.modelFile        = file;
    t.modelDirOverride = kRiggedDir;
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
    // Melee guard: baton-range chip damage on a ~1s cadence.
    t.type           = MonsterType::Guard;
    t.damage         = 8;
    t.attackRange    = 1.9f;
    t.attackCooldown = 1.0f;
    t.attackWindup   = 0.25f;
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
    // Ranged drone: keeps a standoff distance and fires a taser hitscan.
    t.type           = MonsterType::Drone;
    t.damage         = 5;
    t.attackRange    = 14.0f;          // can fire from across the corridor
    t.attackCooldown = 1.4f;
    t.attackWindup   = 0.35f;          // a beat of telegraph before the bolt
    t.ranged         = true;
    t.standoff       = 7.0f;
    // Drone GLB is NOT rigged and authored Z-up like the characters; stand it up.
    useCharacter(t, "Drone.glb", 1.0f);
    return t;
}
} // namespace

void Level1Game::build(Scene& scene, x3::rhi::IRenderDevice& device,
                       x3::phys::IPhysicsWorld& physics, std::string_view modelDir) {
    m_modelDir = std::string(modelDir);
    m_devicePtr = &device;   // cached so tick() can spawn enemies on their beats

    // ---- EFLZ art pass: load the converted sci-fi GLBs and place visual props
    // over the graybox. The door positions are deterministic (the cross-wall X
    // boundaries at z=0), so we seed a minimal layout for EnvArt to anchor door
    // frames/consoles; EnvArt returns a mask of which graybox surfaces real art
    // now covers. If the GLBs are missing, the mask is all-false (full graybox). --
    {
        Level1Layout seed;
        seed.doorA = x3::phys::Vec3{  6.0f, 0.0f, 0.0f };
        seed.doorB = x3::phys::Vec3{ 22.0f, 0.0f, 0.0f };
        seed.doorC = x3::phys::Vec3{ 30.0f, 0.0f, 0.0f };
        seed.doorD = x3::phys::Vec3{ 42.0f, 0.0f, 0.0f };
        seed.doorE = x3::phys::Vec3{ 56.0f, 0.0f, 0.0f };
        m_artMask = m_envArt.build(device, kConvertedDir, seed);
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
        a.tint[0]=0.85f; a.tint[1]=0.30f; a.tint[2]=0.18f;  // cell door (orange-red)
        m_doorIdx[0] = buildLevelDoor(scene, m_doors, device, physics, a);
    }
    {
        DoorSpec b; b.doorwayCenter = m_layout.doorB; b.axis = DoorAxis::AlongZ;
        b.withButton = true; b.locked = false;
        b.tint[0]=0.30f; b.tint[1]=0.55f; b.tint[2]=0.85f;  // corridor door (blue)
        m_doorIdx[1] = buildLevelDoor(scene, m_doors, device, physics, b);
    }
    {
        DoorSpec c; c.doorwayCenter = m_layout.doorC; c.axis = DoorAxis::AlongZ;
        c.withButton = true; c.locked = true;               // §6.4 locked until armed
        c.tint[0]=0.85f; c.tint[1]=0.65f; c.tint[2]=0.20f;  // armory gate (amber)
        m_doorIdx[2] = buildLevelDoor(scene, m_doors, device, physics, c);
    }
    {
        DoorSpec d; d.doorwayCenter = m_layout.doorD; d.axis = DoorAxis::AlongZ;
        d.withButton = false; d.locked = true;              // auto on arena trigger
        d.tint[0]=0.70f; d.tint[1]=0.30f; d.tint[2]=0.30f;
        m_doorIdx[3] = buildLevelDoor(scene, m_doors, device, physics, d);
    }
    {
        DoorSpec e; e.doorwayCenter = m_layout.doorE; e.axis = DoorAxis::AlongZ;
        e.withButton = false; e.locked = true;              // opens on Martinez death
        e.tint[0]=0.40f; e.tint[1]=0.70f; e.tint[2]=0.90f;
        m_doorIdx[4] = buildLevelDoor(scene, m_doors, device, physics, e);
    }

    // ---- Armory pistol pickup (beat 6). ----
    m_weapon.buildWeaponPickup(scene, device, m_modelDir,
                               x3::phys::Vec3{ m_layout.armoryCenter.x, 1.0f,
                                               m_layout.armoryCenter.z });

    // ---- Checkpoint encounter (built at level build; beat 8). Now the player is
    // armed, so this is a proper firefight: 3 Guards holding the line behind the
    // barricade crates + 1 Drone covering from the back of the room. ----
    const x3::phys::Vec3 cc = m_layout.checkpointCenter;
    m_checkpoint.spawn(scene, device, physics, m_modelDir,
                       x3::phys::Vec3{ cc.x - 2.5f, kEnemyY, cc.z - 2.0f }, guardTuning());
    m_checkpoint.spawn(scene, device, physics, m_modelDir,
                       x3::phys::Vec3{ cc.x - 2.0f, kEnemyY, cc.z + 2.0f }, guardTuning());
    m_checkpoint.spawn(scene, device, physics, m_modelDir,
                       x3::phys::Vec3{ cc.x + 2.5f, kEnemyY, cc.z }, guardTuning());
    m_checkpoint.spawn(scene, device, physics, m_modelDir,
                       x3::phys::Vec3{ cc.x + 4.0f, kEnemyY, cc.z - 1.5f }, droneTuning());

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

    m_built = true;
    x3::logInfo("Level1Game::build complete — doors A-E, pistol pickup, 4 checkpoint enemies, 3 triggers");
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

void Level1Game::playSfx(x3::audio::SoundHandle h, const x3::phys::Vec3& at, float vol) {
    if (m_audio.sys && h.valid())
        m_audio.sys->playSound3D(h, at.x, at.y, at.z, vol, 1.0f);
}

void Level1Game::spawnCorridorEnemies(Scene& scene, x3::rhi::IRenderDevice& device,
                                      x3::phys::IPhysicsWorld& physics) {
    if (m_corridorSpawned) return;
    m_corridorSpawned = true;
    // The alarm encounter (x in [6,22]): a near pair of Guards that advance on the
    // player, then a flanking Drone pair further down that keep a ranged standoff —
    // a sensible escalation as the player pushes from the cell toward the armory.
    // (3 guards + 2 drones = 5; was 2+1. The combat AI makes them advance/flank.)
    const float cz = m_layout.corridorCenter.z;
    // Near guards — advance from just past Door A.
    m_corridor.spawn(scene, device, physics, m_modelDir,
                     x3::phys::Vec3{ 10.0f, kEnemyY, cz - 1.6f }, guardTuning());
    m_corridor.spawn(scene, device, physics, m_modelDir,
                     x3::phys::Vec3{ 12.0f, kEnemyY, cz + 1.6f }, guardTuning());
    // Mid guard — backs up the front pair.
    m_corridor.spawn(scene, device, physics, m_modelDir,
                     x3::phys::Vec3{ 15.0f, kEnemyY, cz }, guardTuning());
    // Flanking drones — hang back near Door B and snipe down the corridor.
    m_corridor.spawn(scene, device, physics, m_modelDir,
                     x3::phys::Vec3{ 19.0f, kEnemyY, cz - 2.0f }, droneTuning());
    m_corridor.spawn(scene, device, physics, m_modelDir,
                     x3::phys::Vec3{ 19.0f, kEnemyY, cz + 2.0f }, droneTuning());
    x3::logInfo("Level1: ALARM — corridor enemies spawned (3 guards + 2 drones)");
}

void Level1Game::spawnMartinez(Scene& scene, x3::rhi::IRenderDevice& device,
                               x3::phys::IPhysicsWorld& physics) {
    if (m_martinezSpawned) return;
    m_martinezSpawned = true;
    m_martinez.buildMonsterTuned(scene, device, physics, m_modelDir,
                                 x3::phys::Vec3{ m_layout.arenaCenter.x, 0.6f,
                                                 m_layout.arenaCenter.z },
                                 martinezTuning());
    x3::logInfo("Level1: BOSS — Chief Martinez spawned in the arena (boss-tier HP/speed)");
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
        }
    }
}

bool Level1Game::onUse(const x3::phys::Vec3& eye, const x3::phys::Vec3& dir,
                       Scene& scene, x3::phys::IPhysicsWorld& physics) {
    bool opened = tryUse(eye, dir, 3.0f, scene, m_doors, physics);
    if (opened) playSfx(m_audio.door, eye, 0.9f);
    return opened;
}

FireResult Level1Game::onFire(const x3::phys::Vec3& eye, const x3::phys::Vec3& dir,
                              Scene& scene, x3::phys::IPhysicsWorld& physics) {
    FireResult r;
    if (!m_weapon.hasWeapon()) return r;   // gate: only effective when armed
    // Fire across all three monster groups; the first live monster hit takes it.
    r = m_corridor.fire(eye, dir, scene, physics);
    if (!r.hitMonster) {
        FireResult r2 = m_checkpoint.fire(eye, dir, scene, physics);
        if (r2.hitMonster) r = r2;
        else if (!r.hit && r2.hit) r = r2;
    }
    if (!r.hitMonster && m_martinezSpawned && m_martinez.alive()) {
        FireResult r3 = m_martinez.fire(eye, dir, scene, physics);
        if (r3.hitMonster) r = r3;
        else if (!r.hit && r3.hit) r = r3;
    }
    return r;
}

MeleeResult Level1Game::onMelee(const x3::phys::Vec3& eye, const x3::phys::Vec3& dir,
                                Scene& scene, x3::phys::IPhysicsWorld& physics) {
    // Super-strength punch across all manager-held enemy groups + the doors (the
    // brute-force). Works whether or not armed (the pistol is a separate verb).
    std::vector<MonsterManager*> groups{ &m_corridor, &m_checkpoint, &m_bossAdds };
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
    m_envArt.draw(device, frame);   // converted sci-fi environment art over graybox
    m_weapon.drawPickup(device, frame, scene);
    m_corridor.drawAll(device, frame, scene);
    m_checkpoint.drawAll(device, frame, scene);
    m_bossAdds.drawAll(device, frame, scene);
    if (m_martinezSpawned) m_martinez.drawMonster(device, frame, scene);
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

// Minimal headless IRenderDevice (same shape as door.cpp / monster.cpp): mints
// monotonically-increasing valid handles so build() runs with no Vulkan.
class HeadlessDevice final : public x3::rhi::IRenderDevice {
public:
    bool init(const x3::rhi::DeviceDesc&) override { return true; }
    void shutdown() override {}
    void onResize(uint32_t, uint32_t) override {}
    void setCamera(float, float, float, float, float, float) override {}
    x3::rhi::FrameContext beginFrame() override { return {}; }
    void endFrame(const x3::rhi::FrameContext&) override {}
    x3::rhi::MeshHandle createMesh(const x3::rhi::MeshVertex*, uint32_t,
                                   const uint32_t*, uint32_t) override {
        return x3::rhi::MeshHandle{ m_next++ };
    }
    void destroyMesh(x3::rhi::MeshHandle) override {}
    void updateMesh(x3::rhi::MeshHandle, const x3::rhi::MeshVertex*, uint32_t) override {}
    x3::rhi::TextureHandle createTexture(const void*, uint32_t, uint32_t, bool) override {
        return x3::rhi::TextureHandle{ m_next++ };
    }
    void destroyTexture(x3::rhi::TextureHandle) override {}
    void drawMesh(const x3::rhi::FrameContext&, x3::rhi::MeshHandle,
                  x3::rhi::TextureHandle, const float[4], const float[16]) override {}
    void setPointLights(const x3::rhi::PointLight*, uint32_t) override {}
    void setSkyParams(const x3::rhi::IRenderDevice::SkyParams&) override {}
    void drawHudQuad(const x3::rhi::FrameContext&, float, float, float, float, const float[4]) override {}
    void drawHudText(const x3::rhi::FrameContext&, const char*, float, float, float, const float[4]) override {}
    void hudSize(uint32_t& w, uint32_t& h) const override { w = 0; h = 0; }
    x3::rhi::RenderStats stats() const override { return {}; }
    void armCapture(const char*) override {}                    // headless: no swapchain
    bool captureFrame(const char*) override { return false; }  // headless: no swapchain
    bool supportsDescriptorIndexing() const override { return false; }
    bool supportsMeshShaders() const override { return false; }
private:
    uint32_t m_next = 1;
};

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
    game.build(scene, device, *physics, "G:/GameModels/rigged_glb");

    const Level1Layout& L = game.layout();

    // Helper to aim from an eye toward a target point.
    auto aimFromTo = [](const x3::phys::Vec3& eye, const x3::phys::Vec3& tgt) {
        return sub(tgt, eye);
    };

    // ---- T7 (objective flow start): first objective is the cell escape. ----
    check(game.objectives().current() == 0 &&
          game.objectives().currentLabel() == "Escape the detention cell",
          "T7a first objective = escape the cell");

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

    // ---- T4 (locked gate, part 1): Door C does NOT open before armed. ----
    {
        // Aim a use-ray at Door C's button while UNARMED; it must refuse.
        x3::phys::Vec3 btn{ L.doorC.x - 0.10f - 0.12f, 1.3f, L.doorC.z + 0.6f + 0.5f };
        x3::phys::Vec3 eye{ btn.x - 1.5f, btn.y, btn.z };
        bool usedLocked = game.onUse(eye, aimFromTo(eye, btn), scene, *physics);
        bool stillClosed = game.doorState('C') == DoorState::Closed;
        check(!usedLocked && stillClosed && game.doorLocked('C'),
              "T4a Door C stays locked + closed before armed");
    }

    // ---- T3: walk onto the armory pickup -> armed; pistol can deal damage. ----
    {
        run(game, scene, *physics, device,
            x3::phys::Vec3{ L.armoryCenter.x, 0.05f, L.armoryCenter.z }, 3);
        check(game.armed(), "T3 walking onto the pickup arms the player");
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
        x3::phys::Vec3 eye{ L.checkpointCenter.x - 6.0f, 0.6f, L.checkpointCenter.z };
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

    physics->shutdown();
    x3::logInfo(std::string("[level1-test] ") + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

} // namespace x3::game
