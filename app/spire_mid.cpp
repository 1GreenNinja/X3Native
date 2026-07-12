// EFLZ Act 1 "The Spire" — F3/F4/F5 mid-floor encounter content. See spire_mid.h.
//
// Clean-room: built ONLY from the existing Scene/monster/rescue/door/trigger
// systems + the engine interfaces. No purchased C# / id Tech / RBDOOM source
// consulted. CONTENT/LEVEL-SCRIPT ONLY — no renderer or core-engine changes; this
// composes the data-driven roster (monster.*) + the Wave-1 boss machine
// (bossTuning(BossType) + ScriptedFightHook) + rescue/door/trigger onto the plates
// buildLevel1() already produced. Mirrors level1_game.cpp's authoring style exactly.
#include "spire_mid.h"
#include "asset_root.h"
#include "headless_device.h"

#include "engine/core/x3_log.h"

#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace x3::game {

namespace {

// Enemy body-center Y above a plate's floor (the rigged GLBs sit ~0.4 m up, same as
// Level1Game::kEnemyY) — added to the floor's base Y so a placement lands on the
// plate, not the ground.
constexpr float kEnemyYOff = 0.4f;

// Portable converted-GLB root (same lazy resolve Level1Game uses for the rescue
// victim / character meshes). The roster's tunings carry their own model dir; the
// rescue victim build needs the rigged dir (passed in as modelDir).
const std::string& convertedDir() { static const std::string d = convertedGlbRoot(); return d; }

// A SWARM-DRONE tuning for F5 Drone Manufacturing: the wireframe combat drones the
// Swarm Controller AI commands and that Sarah's master hack flips to allied. Built on
// the BlueSynth (Combat Drone) profile — a ranged flier — so they keep a standoff and
// fit the roster, but tinted swarm-amber so the floor reads as a drone factory.
MonsterSystem::Tuning swarmDroneTuning(std::string_view /*modelDir*/) {
    MonsterSystem::Tuning t = tuningFor(EnemyType::BlueSynth);
    t.tint[0] = 1.0f; t.tint[1] = 0.72f; t.tint[2] = 0.30f; t.tint[3] = 1.0f; // swarm amber
    return t;
}

// The mini-boss the F5 captive transforms into on timer expiry (spec §2 F5: "a
// transformed-victim mini-boss if a timer expired"). A Boss-type so it runs the
// SAME phase machine as Martinez / the F2 transformed victims. Reuses the Oracle
// boss mesh from the rescue roster (an existing asset); falls back to a box if the
// GLB is absent (the level never breaks).
MonsterSystem::Tuning f5VictimBossTuning(std::string_view modelDir) {
    MonsterSystem::Tuning bt;
    bt.type           = MonsterType::Boss;
    bt.hp             = 460;          // mid-boss: tougher than a grunt, lighter than Martinez (340 + adds)
    bt.chaseSpeed     = 3.2f;
    bt.damage         = 13;
    bt.attackRange    = 2.3f;
    bt.attackCooldown = 1.1f;
    bt.attackWindup   = 0.30f;
    bt.ranged         = false;
    bt.tint[0] = 0.65f; bt.tint[1] = 0.45f; bt.tint[2] = 1.0f; bt.tint[3] = 1.0f; // synth-violet
    bt.modelFile        = "Oracle.glb";
    bt.modelDirOverride = std::string(modelDir);
    bt.standUpZtoY      = false;      // rigged boss authored Y-up
    bt.modelScale       = 1.35f;
    return bt;
}

} // namespace

void SpireMidFloors::build(Scene& scene, x3::rhi::IRenderDevice& device,
                           x3::phys::IPhysicsWorld& physics, const Level1Layout& layout,
                           TriggerSystem& triggers, std::string_view modelDir) {
    m_modelDir = std::string(modelDir);
    m_device   = &device;

    // The canonical floor table (footprints + base Y) — single source of truth shared
    // with level1.cpp / env_art.cpp. We place encounters within each plate's bounds.
    const L1RoomDef* tbl = level1Rooms();

    // Convenience: a world point on a given mid-floor at (x, baseY+yOff, z).
    auto at = [&](L1Floor f, float x, float yOff, float z) {
        return x3::phys::Vec3{ x, layout.floorBaseY[(uint32_t)f] + yOff, z };
    };

    // ===================================================================
    // F3 — GENETICS LAB (hybrid-horror research wing). The mass-experimentation
    // floor: a melee-led hybrid pack (2 DominionTroopers reading as Stage-1 hybrids +
    // 1 Verthani flanker) + 1 ranged BlueSynth covering the back, PLUS the floor BOSS
    // **Failed Experiment #7 (Marcus Webb)** — the tragic 400% predecessor anchoring
    // the spawning chamber. Placed in the +X half of the plate, off the elevator-
    // doorway spine (z=0 near the shaft) so an arriving rider isn't ambushed in the
    // shaft mouth. A keypad door (lab keycode) gates the inner spawning chamber.
    // ===================================================================
    {
        const uint32_t fi = (uint32_t)SpireMidFloor::F3;
        const L1Floor   f = L1Floor::F3;
        SpireFloorPlan& p = m_plan[fi];
        p.floor    = f;
        p.name     = "Genetics Lab";
        p.elevStop = (uint32_t)f;                       // one elevator stop per floor
        p.baseY    = layout.floorBaseY[(uint32_t)f];
        p.arrival  = at(f, 17.5f, 0.05f, 0.0f);         // step off the shaft (x=19.5) onto the plate

        // Hybrid pack — a melee pair advances, a Verthani darts/flanks, a BlueSynth
        // snipes from the far -X end. Counts/roles from the roster.
        m_enemies[fi].spawn(scene, device, physics, m_modelDir,
                            at(f, 12.0f, kEnemyYOff, -3.0f), tuningFor(EnemyType::DominionTrooper));
        m_enemies[fi].spawn(scene, device, physics, m_modelDir,
                            at(f, 12.0f, kEnemyYOff,  3.0f), tuningFor(EnemyType::DominionTrooper));
        m_enemies[fi].spawn(scene, device, physics, m_modelDir,
                            at(f,  9.0f, kEnemyYOff,  0.0f), tuningFor(EnemyType::Verthani));
        m_enemies[fi].spawn(scene, device, physics, m_modelDir,
                            at(f,  4.0f, kEnemyYOff, -4.0f), tuningFor(EnemyType::BlueSynth));
        p.meleeCount  = 3;   // 2 DominionTrooper + 1 Verthani
        p.rangedCount = 1;   // 1 BlueSynth

        // ---- F3 BOSS: Failed Experiment #7 (Marcus Webb). The tragic predecessor —
        // 3 phases (Rage -> Despair -> Release) with the Memory-Flash vulnerability
        // window on each phase transition (see bossTuning(BossType::FailedExperiment7)).
        // Anchors the spawning chamber center, deep in the -X / inner half (gated by the
        // lab door), off the shaft spine. Its own manager so its role/phase are distinct
        // from the hybrid pack.
        m_f3Boss.spawn(scene, device, physics, m_modelDir,
                       at(f, 5.0f, kEnemyYOff, 4.0f), bossTuning(BossType::FailedExperiment7));
        p.bossCount = 1;
        p.hasBoss   = true;
        p.totalCount = p.meleeCount + p.rangedCount + p.bossCount;   // 5

        // Lab keypad door: locked, code 3300 (research wing). Sits in a partition in
        // the middle of the plate (x=8) so the inner spawning chamber is gated. AlongZ
        // (wall runs along Z, door thin in X), same as the B1 spine doors.
        DoorSpec d; d.doorwayCenter = at(f, 8.0f, 0.0f, 0.0f); d.axis = DoorAxis::AlongZ;
        d.withButton = false; d.locked = true; d.code = 3300;   // lab keycode (terminal-found)
        d.tint[0]=0.45f; d.tint[1]=0.62f; d.tint[2]=0.45f;      // lab green
        buildLevelDoor(scene, m_doors, device, physics, d);
        p.doorCode  = 3300;
        p.hasVictim = false;

        // Hub trigger where the rider arrives (alarm/objective hook).
        triggers.add(x3::phys::Vec3{ tbl[(uint32_t)f].x1 - 8.0f, p.baseY,        -6.0f },
                     x3::phys::Vec3{ tbl[(uint32_t)f].x1,        p.baseY + 3.0f,  6.0f },
                     (uint32_t)SpireMidTrigger::F3Hub, true);
    }

    // ===================================================================
    // F4 — CYBERNETICS WORKSHOP (human-machine fusion). Escalates F3's standard pack:
    // 5 enemies, a ranged ELITE added. "Occupation cyborgs; cover; door-override
    // puzzle." 3 melee (DominionTroopers, reading as augmented cyborgs) holding cover
    // lines + 1 BlueSynth + 1 Illuminated elite holding a long standoff. A door-
    // override keypad (code 4040). NO floor boss here — the Cybernetics "Collective"/
    // Chorus boss is staged in the off-elevator Floor 4.5 Nexus by spire_nexus.*; we
    // only leave the labeled F4 -> Floor 4.5 transition hook + the Humanity meter +
    // the augmentation-chair choice.
    // ===================================================================
    {
        const uint32_t fi = (uint32_t)SpireMidFloor::F4;
        const L1Floor   f = L1Floor::F4;
        SpireFloorPlan& p = m_plan[fi];
        p.floor    = f;
        p.name     = "Cybernetics Workshop";
        p.elevStop = (uint32_t)f;
        p.baseY    = layout.floorBaseY[(uint32_t)f];
        p.arrival  = at(f, 17.5f, 0.05f, 0.0f);

        // Three cyborgs across the workshop floor (cover lines), a synth flanker, and
        // a golden Illuminated elite holding the far standoff (the new escalation).
        m_enemies[fi].spawn(scene, device, physics, m_modelDir,
                            at(f, 14.0f, kEnemyYOff, -4.0f), tuningFor(EnemyType::DominionTrooper));
        m_enemies[fi].spawn(scene, device, physics, m_modelDir,
                            at(f, 14.0f, kEnemyYOff,  4.0f), tuningFor(EnemyType::DominionTrooper));
        m_enemies[fi].spawn(scene, device, physics, m_modelDir,
                            at(f, 10.0f, kEnemyYOff,  0.0f), tuningFor(EnemyType::DominionTrooper));
        m_enemies[fi].spawn(scene, device, physics, m_modelDir,
                            at(f,  6.0f, kEnemyYOff, -3.0f), tuningFor(EnemyType::BlueSynth));
        m_enemies[fi].spawn(scene, device, physics, m_modelDir,
                            at(f,  3.0f, kEnemyYOff,  3.0f), tuningFor(EnemyType::Illuminated));
        p.meleeCount  = 3;   // 3 DominionTrooper (Guard archetype)
        p.rangedCount = 2;   // 1 BlueSynth + 1 Illuminated (Drone archetype)
        p.bossCount   = 0;   // the Collective/Chorus lives on Floor 4.5 (spire_nexus)
        p.hasBoss     = false;
        p.totalCount  = p.meleeCount + p.rangedCount + p.bossCount;   // 5

        // Door-override keypad: locked, code 4040. Partition at x=8.
        DoorSpec d; d.doorwayCenter = at(f, 8.0f, 0.0f, 0.0f); d.axis = DoorAxis::AlongZ;
        d.withButton = false; d.locked = true; d.code = 4040;   // door-override code
        d.tint[0]=0.70f; d.tint[1]=0.66f; d.tint[2]=0.50f;      // workshop steel/tan
        buildLevelDoor(scene, m_doors, device, physics, d);
        p.doorCode  = 4040;
        p.hasVictim = false;

        // The labeled F4 -> Floor 4.5 (Nexus Chamber / The Chorus) transition hook. We
        // do NOT build the Chorus here — that is spire_nexus's lane. The hook sits at
        // the inner edge of the F4 plate (past the door-override puzzle), the point the
        // host routes off-elevator to the Nexus.
        m_nexusTransition = at(f, 1.5f, 0.05f, 0.0f);

        triggers.add(x3::phys::Vec3{ tbl[(uint32_t)f].x1 - 8.0f, p.baseY,        -6.0f },
                     x3::phys::Vec3{ tbl[(uint32_t)f].x1,        p.baseY + 3.0f,  6.0f },
                     (uint32_t)SpireMidTrigger::F4Hub, true);
    }

    // ===================================================================
    // F5 — DRONE MANUFACTURING (THE drone level). The Swarm-drone factory: a set of
    // wireframe Swarm drones (5 ranged) + 1 melee Verthani harrier weaving in, all
    // commanded by the floor BOSS **Swarm Controller AI** anchoring the Hive-Mind
    // chamber. **Sarah's master hack** is a scripted PRE-FIGHT beat (gated, NOT at
    // load) that strips ~75% of the Swarm AI's HP and flips the drone set to allied.
    // PLUS one rescue captive (a lab tech) on a hub-gated timer; if it expires the
    // captive transforms into a mini-boss. A drone-bay keypad door (code 5500).
    // ===================================================================
    {
        const uint32_t fi = (uint32_t)SpireMidFloor::F5;
        const L1Floor   f = L1Floor::F5;
        SpireFloorPlan& p = m_plan[fi];
        p.floor    = f;
        p.name     = "Drone Manufacturing";
        p.elevStop = (uint32_t)f;
        p.baseY    = layout.floorBaseY[(uint32_t)f];
        p.arrival  = at(f, 17.5f, 0.05f, 0.0f);

        // The Swarm-drone set: a lone Verthani melee harrier weaving in, plus five
        // wireframe Swarm drones (the hackable drone army) laying down ranged fire
        // across the bay. The melee cap (combat::kMaxMeleeAttackers) is irrelevant with
        // one melee unit; the drones hold their distance, so the floor is dense but
        // winnable. These five ranged drones are exactly what Sarah's hack flips.
        m_enemies[fi].spawn(scene, device, physics, m_modelDir,
                            at(f, 13.0f, kEnemyYOff,  0.0f), tuningFor(EnemyType::Verthani));
        m_enemies[fi].spawn(scene, device, physics, m_modelDir,
                            at(f, 10.0f, kEnemyYOff, -4.0f), swarmDroneTuning(m_modelDir));
        m_enemies[fi].spawn(scene, device, physics, m_modelDir,
                            at(f, 10.0f, kEnemyYOff,  4.0f), swarmDroneTuning(m_modelDir));
        m_enemies[fi].spawn(scene, device, physics, m_modelDir,
                            at(f,  6.0f, kEnemyYOff,  0.0f), swarmDroneTuning(m_modelDir));
        m_enemies[fi].spawn(scene, device, physics, m_modelDir,
                            at(f,  3.0f, kEnemyYOff, -4.0f), swarmDroneTuning(m_modelDir));
        m_enemies[fi].spawn(scene, device, physics, m_modelDir,
                            at(f,  3.0f, kEnemyYOff,  4.0f), swarmDroneTuning(m_modelDir));
        p.meleeCount  = 1;   // 1 Verthani (Guard archetype)
        p.rangedCount = 5;   // 5 Swarm drones (Drone archetype) — the hackable army

        // ---- F5 BOSS: Swarm Controller AI. The AI commanding all Lab Zero drones —
        // its own manager. Sarah's master hack debuffs it pre-fight (strips ~75% HP)
        // and flips its drone army to your side. Reuse the Alien Overseer Boss tuning
        // (ranged psychic-commander profile) as the "holographic avatar" stand-in: a
        // ranged Boss that commands from a standoff and runs the phase machine.
        m_swarmBoss.spawn(scene, device, physics, m_modelDir,
                          at(f, 2.0f, kEnemyYOff, 0.0f), bossTuning(BossType::AlienOverseer));
        p.bossCount  = 1;
        p.hasBoss    = true;
        p.totalCount = p.meleeCount + p.rangedCount + p.bossCount;   // 7

        // Drone-bay keypad door: locked, code 5500. Partition at x=8.
        DoorSpec d; d.doorwayCenter = at(f, 8.0f, 0.0f, 0.0f); d.axis = DoorAxis::AlongZ;
        d.withButton = false; d.locked = true; d.code = 5500;   // drone-bay code
        d.tint[0]=0.45f; d.tint[1]=0.55f; d.tint[2]=0.85f;      // factory blue
        buildLevelDoor(scene, m_doors, device, physics, d);
        p.doorCode  = 5500;

        // ---- F5 rescue captive (single victim). PLAYTEST-FIX mirror: the timer is
        // GATED on the F5 hub being reached (m_f5HubReached, default FALSE). We do NOT
        // activate it here, so the 5-min clock cannot expire at load and spawn the
        // mini-boss on the first frame. The host registers the F5Hub trigger below;
        // entering it (onTrigger) flips m_f5HubReached and the clock starts. ----
        const x3::phys::Vec3 victimPos = at(f, 18.5f, kEnemyYOff, -5.5f);  // tucked in a -Z bay corner
        m_victim = std::make_unique<RescueVictim>();
        m_victim->build(scene, device, physics, m_modelDir, victimPos,
                        VictimId::Emily, "Lena", "AnnaTactical.glb",
                        kRescueTimer, f5VictimBossTuning(m_modelDir));
        p.hasVictim = true;

        triggers.add(x3::phys::Vec3{ tbl[(uint32_t)f].x1 - 8.0f, p.baseY,        -6.0f },
                     x3::phys::Vec3{ tbl[(uint32_t)f].x1,        p.baseY + 3.0f,  6.0f },
                     (uint32_t)SpireMidTrigger::F5Hub, true);
    }

    // Try to load the shared real-door GLB so the keypad doors render as meshes (the
    // collision box stays). No-op/harmless if the GLB is absent (graybox fallback).
    m_doors.loadDoorMesh(device, convertedDir());

    m_built = true;
    x3::logInfo("SpireMidFloors::build complete — F3 GENETICS LAB (4 enemies + boss "
                "'Failed Experiment #7', code 3300), F4 CYBERNETICS WORKSHOP (5 enemies, "
                "Humanity meter + augment chair, F4->4.5 Nexus hook, code 4040), "
                "F5 DRONE MANUFACTURING (1 melee + 5 Swarm drones + boss 'Swarm Controller "
                "AI' + Sarah's master hack [gated] + 1 rescue captive 'Lena', code 5500); "
                "3 keypad doors, 3 floor-hub triggers");
}

void SpireMidFloors::tick(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics,
                          const x3::phys::Vec3& eye, const x3::phys::Vec3& playerPos,
                          IDamageSink* player, const AttackFxFn& attackFx) {
    if (!m_built) return;

    // Keypad doors animate.
    m_doors.update(dt, scene, physics);

    // Enemies attack only while the player is alive (matches Level1Game::tick).
    IDamageSink* atkTarget = (player && player->isAlive()) ? player : nullptr;
    for (uint32_t i = 0; i < (uint32_t)SpireMidFloor::Count; ++i)
        m_enemies[i].update(dt, scene, physics, eye, atkTarget, attackFx);
    // Floor bosses (F3 FE#7 + F5 Swarm AI) run the same phase machine as Martinez.
    m_f3Boss.update(dt, scene, physics, eye, atkTarget, attackFx);
    m_swarmBoss.update(dt, scene, physics, eye, atkTarget, attackFx);

    // F5 rescue victim: tick the timer (gated on m_f5HubReached) + companion follow,
    // and spawn the mini-boss the FRAME the timer expires (mirrors RescueSystem::tick).
    if (m_victim) {
        const bool expiredNow =
            m_victim->tick(dt, m_f5HubReached, scene, physics, playerPos);
        if (expiredNow && m_device) {
            const x3::phys::Vec3 bossAt{ m_victim->pos().x, kEnemyYOff, m_victim->pos().z };
            m_victimBoss.spawn(scene, *m_device, physics, m_modelDir, bossAt,
                               m_victim->bossTuning());
            x3::logInfo("[spiremid] F5 captive 'Lena' transformed — drone-bay mini-boss spawned");
        }
    }
    // The transformed mini-boss chases/attacks like the rescue bosses do.
    m_victimBoss.update(dt, scene, physics, eye, atkTarget, attackFx);
}

void SpireMidFloors::shutdown() {
    // Tear down any in-flight death ragdolls (Jolt bodies) across every mid-floor
    // enemy manager BEFORE the physics world dies. MonsterManager::shutdown() is
    // itself idempotent (fans shutdownRagdoll() -> clearDeathRagdoll() over its
    // monsters), so a double call or a call with nothing dead is a harmless no-op.
    for (auto& m : m_enemies) m.shutdown();
    m_f3Boss.shutdown();
    m_swarmBoss.shutdown();
    m_victimBoss.shutdown();
}

void SpireMidFloors::onTrigger(uint32_t triggerId) {
    switch ((SpireMidTrigger)triggerId) {
        case SpireMidTrigger::F3Hub:
            if (!m_f3HubReached) {
                m_f3HubReached = true;
                x3::logInfo("SpireMid: F3 GENETICS LAB hub reached — hybrid wing + "
                            "Failed Experiment #7 encounter armed");
            }
            break;
        case SpireMidTrigger::F4Hub:
            if (!m_f4HubReached) {
                m_f4HubReached = true;
                x3::logInfo("SpireMid: F4 CYBERNETICS WORKSHOP hub reached — augmentation "
                            "bay armed (Humanity meter live; Floor 4.5 Nexus ahead)");
            }
            break;
        case SpireMidTrigger::F5Hub:
            // PLAYTEST-FIX mirror: the player reached the F5 drone factory — START the
            // rescue clock NOW (not at load), and run SARAH'S MASTER HACK as the floor's
            // scripted pre-fight beat (strip the Swarm Controller AI + flip the drone
            // army to allied). Gated on the hub (NOT at load) + idempotent.
            if (!m_f5HubReached) {
                m_f5HubReached = true;
                x3::logInfo("SpireMid: F5 DRONE MANUFACTURING hub reached — rescue timer "
                            "started ('Lena' now counting down); running Sarah's master hack");
                runSarahMasterHack();   // the F5 signature beat (idempotent)
            }
            break;
    }
}

bool SpireMidFloors::onRescue(const x3::phys::Vec3& playerPos, float range) {
    if (!m_victim) return false;
    return m_victim->tryRescue(playerPos, range);
}

FireResult SpireMidFloors::onFire(const x3::phys::Vec3& eye, const x3::phys::Vec3& dir,
                                  Scene& scene, x3::phys::IPhysicsWorld& physics,
                                  int damage, x3::DamageType type) {
    FireResult r;
    for (uint32_t i = 0; i < (uint32_t)SpireMidFloor::Count; ++i) {
        FireResult ri = m_enemies[i].fire(eye, dir, scene, physics, damage, type);
        if (ri.hitMonster) return ri;          // a live enemy took it — done
        if (!r.hit && ri.hit) r = ri;          // remember the nearest geometry hit
    }
    // F3 boss (Failed Experiment #7).
    FireResult rf3 = m_f3Boss.fire(eye, dir, scene, physics, damage, type);
    if (rf3.hitMonster) return rf3;
    if (!r.hit && rf3.hit) r = rf3;
    // F5 boss (Swarm Controller AI).
    FireResult rsw = m_swarmBoss.fire(eye, dir, scene, physics, damage, type);
    if (rsw.hitMonster) return rsw;
    if (!r.hit && rsw.hit) r = rsw;
    // The transformed mini-boss, if any.
    FireResult rb = m_victimBoss.fire(eye, dir, scene, physics, damage, type);
    if (rb.hitMonster) return rb;
    if (!r.hit && rb.hit) r = rb;
    return r;
}

void SpireMidFloors::draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                          const Scene& scene) const {
    for (uint32_t i = 0; i < (uint32_t)SpireMidFloor::Count; ++i)
        m_enemies[i].drawAll(device, frame, scene);
    m_f3Boss.drawAll(device, frame, scene);
    m_swarmBoss.drawAll(device, frame, scene);
    if (m_victim) m_victim->draw(device, frame, scene);
    m_victimBoss.drawAll(device, frame, scene);
}

// ---- F4 Cybernetics: the Humanity meter + augmentation-chair choice ----------
int SpireMidFloors::adjustHumanity(int delta) {
    m_humanity += delta;
    if (m_humanity < 0)            m_humanity = 0;
    if (m_humanity > kHumanityMax) m_humanity = kHumanityMax;
    return m_humanity;
}

bool SpireMidFloors::augmentChairChoice(bool accept) {
    if (m_augmentChairUsed) return false;     // the chair is a one-time choice
    m_augmentChairUsed = true;
    if (accept) {
        adjustHumanity(-kAugmentHumanityCost);
        x3::logInfo("SpireMid: F4 AUGMENTATION CHAIR USED — Jake took the augment "
                    "(Humanity " + std::to_string(m_humanity) + "/" +
                    std::to_string(kHumanityMax) + ")");
    } else {
        x3::logInfo("SpireMid: F4 AUGMENTATION CHAIR REFUSED — Jake kept his Humanity ("
                    + std::to_string(m_humanity) + "/" + std::to_string(kHumanityMax) + ")");
    }
    return true;
}

// ---- F5 Drone Manufacturing: Sarah's master hack -----------------------------
uint32_t SpireMidFloors::f5DroneCount() const {
    // The hackable drone set = the ranged (Drone-archetype) enemies on F5.
    uint32_t n = 0;
    const MonsterManager& m = m_enemies[(uint32_t)SpireMidFloor::F5];
    for (uint32_t i = 0; i < m.count(); ++i) if (m.at(i).ranged()) ++n;
    return n;
}

ScriptedFightHook::Result SpireMidFloors::runSarahMasterHack() {
    ScriptedFightHook::Result res;
    if (!m_built || m_sarahHackDone) return res;     // gated + idempotent
    if (m_swarmBoss.count() == 0)     return res;
    m_sarahHackDone = true;

    // Gather the F5 drone set (the ranged Swarm drones) to flip to allied.
    std::vector<MonsterSystem*> drones;
    MonsterManager& f5 = m_enemies[(uint32_t)SpireMidFloor::F5];
    for (uint32_t i = 0; i < f5.count(); ++i)
        if (f5.at(i).ranged()) drones.push_back(&f5.at(i));

    // Sarah's 90-second master hack, as a scripted pre-fight beat: strip ~75% of the
    // Swarm Controller AI's max HP off + flip the drone army to allied (their attack
    // damage zeroed). The boss still spawns + fights (never killed outright by the hack).
    res = ScriptedFightHook::masterHack(m_swarmBoss.at(0), 0.75f, drones);
    x3::logInfo("SpireMid: SARAH'S MASTER HACK COMPLETE — Swarm Controller AI stripped " +
                std::to_string(res.hpStripped) + " HP; " +
                std::to_string(res.dronesFlipped) + " drones flipped to the drone army");
    return res;
}

bool SpireMidFloors::nearLockedCodedDoor(const x3::phys::Vec3& playerPos, float range) const {
    const float r2 = range * range;
    for (uint32_t i = 0; i < m_doors.count(); ++i) {
        const Door& d = m_doors.at(i);
        if (!d.locked || d.code == 0) continue;
        // 3D distance: the mid-floor doors STACK at the same XZ on the vertical
        // tower (F3/F4/F5 at x=8,z=0, differing only in Y), so Y must distinguish
        // them — an XZ-only test would treat all three as the same spot.
        const float dx = playerPos.x - d.closedPos.x;
        const float dy = playerPos.y - d.closedPos.y;
        const float dz = playerPos.z - d.closedPos.z;
        if (dx * dx + dy * dy + dz * dz <= r2) return true;
    }
    return false;
}

bool SpireMidFloors::tryDoorCode(const x3::phys::Vec3& playerPos, int code, float range) {
    const float r2 = range * range;
    int best = -1; float bestD2 = r2;
    for (uint32_t i = 0; i < m_doors.count(); ++i) {
        const Door& d = m_doors.at(i);
        if (!d.locked || d.code == 0) continue;
        // 3D distance (include Y) so the nearest LOCKED coded door is the one on the
        // player's CURRENT floor, not whichever stacked door wins an XZ tie.
        const float dx = playerPos.x - d.closedPos.x;
        const float dy = playerPos.y - d.closedPos.y;
        const float dz = playerPos.z - d.closedPos.z;
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 <= bestD2) { bestD2 = d2; best = (int)i; }
    }
    if (best < 0) return false;
    Door& d = m_doors.at((uint32_t)best);
    if (d.code != code) return false;
    m_doors.unlock(d);
    return m_doors.startOpening(d);
}

bool SpireMidFloors::victimCaptive() const {
    return m_victim && m_victim->captive();
}
float SpireMidFloors::victimTimeLeft() const {
    return m_victim ? m_victim->timeLeft() : 0.0f;
}

bool SpireMidFloors::reachableViaElevator(SpireMidFloor f, uint32_t elevatorStopCount) const {
    if (!m_built) return false;
    const uint32_t stop = m_plan[(uint32_t)f].elevStop;
    return stop < elevatorStopCount;   // the host builds one stop per floor (0..count-1)
}

// ===========================================================================
// Headless self-test (--test-spiremid).
// ===========================================================================
namespace {

int g_pass = 0, g_fail = 0;
void check(bool cond, const char* name) {
    if (cond) { ++g_pass; x3::logInfo(std::string("[spiremid-test] PASS ") + name); }
    else      { ++g_fail; x3::logError(std::string("[spiremid-test] FAIL ") + name); }
}

constexpr float kFixedDt = 1.0f / 60.0f;

using HeadlessDevice = x3::game::HeadlessRenderDevice;

// Count melee (Guard-archetype, non-ranged) vs ranged (ranged==true) enemies in a
// manager — proves the ROLE split, not just the count.
void roleSplit(const MonsterManager& m, uint32_t& melee, uint32_t& ranged) {
    melee = ranged = 0;
    for (uint32_t i = 0; i < m.count(); ++i) {
        if (m.at(i).ranged()) ++ranged; else ++melee;
    }
}

bool nameIs(const char* a, const char* b) { return std::string(a) == std::string(b); }

} // namespace

bool runSpireMidSelfTest() {
    g_pass = g_fail = 0;

    std::unique_ptr<x3::phys::IPhysicsWorld> physics(x3::phys::createPhysicsWorld());
    physics->init();

    HeadlessDevice device;
    Scene scene;

    // Build the Spire geometry (gives us floorBaseY[] + the plate footprints), then
    // author the mid floors on top, sharing a TriggerSystem like the host does.
    Level1Layout layout = buildLevel1(scene, device, *physics);
    TriggerSystem triggers;
    SpireMidFloors mid;
    const std::string rigged = riggedGlbRoot();
    mid.build(scene, device, *physics, layout, triggers, rigged);

    check(mid.built(), "S0 mid floors built");

    // ---- Canon floor identities (the Wave-2 re-theme). ----
    {
        bool names = nameIs(mid.plan(SpireMidFloor::F3).name, "Genetics Lab") &&
                     nameIs(mid.plan(SpireMidFloor::F4).name, "Cybernetics Workshop") &&
                     nameIs(mid.plan(SpireMidFloor::F5).name, "Drone Manufacturing");
        check(names, "S1 canon floor identities (Genetics Lab / Cybernetics Workshop / Drone Manufacturing)");
    }

    // ---- Per-floor STANDARD-enemy placement COUNTS + ROLE split. ----
    {
        const SpireFloorPlan& f3 = mid.plan(SpireMidFloor::F3);
        uint32_t m3 = 0, r3 = 0; roleSplit(mid.enemies(SpireMidFloor::F3), m3, r3);
        check(mid.enemies(SpireMidFloor::F3).count() == 4 &&
              m3 == 3 && r3 == 1 && f3.meleeCount == 3 && f3.rangedCount == 1,
              "S2 F3 standard pack = 4 (3 melee + 1 ranged)");

        const SpireFloorPlan& f4 = mid.plan(SpireMidFloor::F4);
        uint32_t m4 = 0, r4 = 0; roleSplit(mid.enemies(SpireMidFloor::F4), m4, r4);
        check(mid.enemies(SpireMidFloor::F4).count() == 5 && f4.totalCount == 5 &&
              m4 == 3 && r4 == 2 && f4.meleeCount == 3 && f4.rangedCount == 2 && !f4.hasBoss,
              "S3 F4 = 5 enemies (3 melee + 2 ranged, no floor boss)");

        const SpireFloorPlan& f5 = mid.plan(SpireMidFloor::F5);
        uint32_t m5 = 0, r5 = 0; roleSplit(mid.enemies(SpireMidFloor::F5), m5, r5);
        check(mid.enemies(SpireMidFloor::F5).count() == 6 &&
              m5 == 1 && r5 == 5 && f5.meleeCount == 1 && f5.rangedCount == 5,
              "S4 F5 standard set = 6 (1 melee + 5 Swarm drones)");
    }

    // ---- F3 boss: Failed Experiment #7 (Marcus Webb), alive at load, Boss-type. ----
    {
        const SpireFloorPlan& f3 = mid.plan(SpireMidFloor::F3);
        bool f3Boss = f3.hasBoss && f3.bossCount == 1 && f3.totalCount == 5 &&
                      mid.f3Boss().count() == 1 &&
                      mid.f3Boss().at(0).type() == MonsterType::Boss &&
                      mid.f3Boss().at(0).alive() &&
                      mid.f3Boss().at(0).maxHp() == bossTuning(BossType::FailedExperiment7).hp;
        check(f3Boss, "S5 F3 carries the Failed Experiment #7 boss (live Boss-type)");
    }

    // ---- F5 boss: Swarm Controller AI, alive at load, Boss-type. ----
    {
        const SpireFloorPlan& f5 = mid.plan(SpireMidFloor::F5);
        bool swarm = f5.hasBoss && f5.bossCount == 1 && f5.totalCount == 7 &&
                     mid.swarmBoss().count() == 1 &&
                     mid.swarmBoss().at(0).type() == MonsterType::Boss &&
                     mid.swarmBoss().at(0).alive();
        check(swarm, "S6 F5 carries the Swarm Controller AI boss (live Boss-type)");
    }

    // ---- Difficulty escalates F3 < F4 < F5 (STANDARD-enemy totals only, so the
    // escalation curve of the regular waves still climbs even though F4 has no boss). ----
    {
        const uint32_t c3 = mid.enemies(SpireMidFloor::F3).count();   // 4
        const uint32_t c4 = mid.enemies(SpireMidFloor::F4).count();   // 5
        const uint32_t c5 = mid.enemies(SpireMidFloor::F5).count();   // 6
        check(c3 < c4 && c4 < c5, "S7 difficulty escalates F3(4) < F4(5) < F5(6) standard enemies");
    }

    // ---- All standard mid-floor enemies + the floor bosses alive at load. ----
    {
        bool allAlive = mid.enemies(SpireMidFloor::F3).aliveCount() == 4 &&
                        mid.enemies(SpireMidFloor::F4).aliveCount() == 5 &&
                        mid.enemies(SpireMidFloor::F5).aliveCount() == 6 &&
                        mid.f3Boss().aliveCount() == 1 &&
                        mid.swarmBoss().aliveCount() == 1;
        check(allAlive, "S8 all placed enemies + the F3/F5 bosses alive at load");
    }

    // ---- F4 Humanity meter: starts full, REFUSE keeps it, USE drops it; and the
    // F4 -> Floor 4.5 (Nexus) transition hook is exposed (Chorus NOT placed here). ----
    {
        check(mid.humanity() == kHumanityMax && !mid.augmentChairUsed(),
              "S9 F4 Humanity starts full (100), augment chair unused");

        // The F4 -> Floor 4.5 transition hook sits on the F4 plate (correct base Y),
        // and this module did NOT place a Cybernetics floor boss (that's the Nexus
        // agent's lane).
        bool hookOnF4 = std::fabs(mid.nexusTransition().y -
                          (layout.floorBaseY[(uint32_t)L1Floor::F4] + 0.05f)) < 1e-3f &&
                        !mid.plan(SpireMidFloor::F4).hasBoss;
        check(hookOnF4, "S10 F4 -> Floor 4.5 Nexus transition hook exposed; no Chorus placed here");

        // REFUSING the chair does not cost Humanity (but it does latch the choice).
        SpireMidFloors midR;
        Scene sc2; std::unique_ptr<x3::phys::IPhysicsWorld> ph2(x3::phys::createPhysicsWorld());
        ph2->init();
        Level1Layout ly2 = buildLevel1(sc2, device, *ph2);
        TriggerSystem tr2; midR.build(sc2, device, *ph2, ly2, tr2, rigged);
        bool refusedNoCost = midR.augmentChairChoice(false) &&
                             midR.humanity() == kHumanityMax && midR.augmentChairUsed() &&
                             !midR.augmentChairChoice(false);   // one-time
        ph2->shutdown();
        check(refusedNoCost, "S11 augment chair REFUSE keeps Humanity (one-time choice)");

        // USING the chair costs kAugmentHumanityCost.
        bool usedCost = mid.augmentChairChoice(true) &&
                        mid.humanity() == kHumanityMax - kAugmentHumanityCost &&
                        mid.augmentChairUsed() &&
                        !mid.augmentChairChoice(true);    // one-time
        check(usedCost, "S12 augment chair USE deducts Humanity (one-time choice)");
    }

    // ---- Sarah's master hack: GATED (not done at load); when run it strips the Swarm
    // AI's HP fraction + flips the F5 drone set to allied (their attack damage zeroed). ----
    {
        check(!mid.sarahHackDone(), "S13 Sarah's master hack NOT performed at load (gated)");

        const int swarmMax = mid.swarmBoss().at(0).maxHp();
        const int swarmBefore = mid.swarmBoss().at(0).hp();
        const uint32_t droneN = mid.f5DroneCount();
        // Drones hostile before the hack (their attack damage is non-zero, not allied).
        bool hostileBefore = droneN == 5;
        for (uint32_t i = 0; i < mid.enemies(SpireMidFloor::F5).count(); ++i) {
            const MonsterSystem& e = mid.enemies(SpireMidFloor::F5).at(i);
            if (e.ranged() && (e.attackDamage() <= 0 || e.isAllied())) hostileBefore = false;
        }
        check(hostileBefore && swarmBefore == swarmMax,
              "S14 F5 drones hostile + Swarm AI at full HP before the hack");

        ScriptedFightHook::Result res = mid.runSarahMasterHack();
        const int expectStrip = (int)(swarmMax * 0.75f + 0.5f);
        bool stripOk = res.hpStripped == expectStrip &&
                       mid.swarmBoss().at(0).hp() == swarmMax - expectStrip &&
                       mid.swarmBoss().at(0).hp() >= 1 &&     // never killed outright
                       mid.swarmBoss().at(0).alive();
        bool flipOk = res.dronesFlipped == droneN;
        bool alliedAfter = true;
        for (uint32_t i = 0; i < mid.enemies(SpireMidFloor::F5).count(); ++i) {
            const MonsterSystem& e = mid.enemies(SpireMidFloor::F5).at(i);
            if (e.ranged() && (!e.isAllied() || e.attackDamage() != 0)) alliedAfter = false;
        }
        check(mid.sarahHackDone() && stripOk && flipOk && alliedAfter,
              "S15 master hack strips ~75% Swarm AI HP + flips the drone army to allied");

        // Idempotent: a second call is a no-op (no further strip / flip).
        ScriptedFightHook::Result res2 = mid.runSarahMasterHack();
        check(res2.hpStripped == 0 && res2.dronesFlipped == 0,
              "S16 master hack is idempotent (second call is a no-op)");
    }

    // ---- F5 rescue victim PRESENT but NOT active at load. ----
    {
        bool present = mid.victimPresent();
        bool captive = mid.victimCaptive();
        bool timerStopped = !mid.victimTimerRunning();
        check(present && captive && timerStopped,
              "S17 F5 victim present + captive + timer NOT running at load");
    }

    // ---- The timer truly stays full until the hub fires: tick many frames at load
    // (no hub) and confirm the victim is STILL a captive (no expiry/transform). ----
    {
        for (int i = 0; i < 120; ++i) {
            mid.tick(kFixedDt, scene, *physics, layout.spawn, layout.spawn, nullptr, AttackFxFn{});
            physics->step(kFixedDt);
            scene.update(*physics);
        }
        bool stillCaptive = mid.victimCaptive();
        bool noBoss = mid.enemies(SpireMidFloor::F5).count() == 6;   // no extra spawn
        check(stillCaptive && noBoss,
              "S18 victim stays captive while hub unreached (no early expiry)");
    }

    // ---- Reaching the F5 hub starts the clock (mirrors RescueSystem::activate). ----
    {
        mid.onTrigger((uint32_t)SpireMidTrigger::F5Hub);
        check(mid.victimTimerRunning(), "S19 F5 hub trigger starts the rescue clock");
        // One tick now decrements the timer below its max.
        float before = mid.victimTimeLeft();
        mid.tick(kFixedDt, scene, *physics, layout.spawn, layout.spawn, nullptr, AttackFxFn{});
        check(mid.victimTimeLeft() < before, "S20 timer counts down after the hub");
    }

    // ---- Reachability via the elevator. The host builds one stop per floor
    // (kSpireFloorCount). Each mid floor's stop index must be inside that range, and
    // must equal its L1Floor index (so the ride lands on the right plate). ----
    {
        const uint32_t stops = kSpireFloorCount;   // one elevator stop per floor (B1..F7)
        bool reach = mid.reachableViaElevator(SpireMidFloor::F3, stops) &&
                     mid.reachableViaElevator(SpireMidFloor::F4, stops) &&
                     mid.reachableViaElevator(SpireMidFloor::F5, stops);
        bool stopsMatch = mid.plan(SpireMidFloor::F3).elevStop == (uint32_t)L1Floor::F3 &&
                          mid.plan(SpireMidFloor::F4).elevStop == (uint32_t)L1Floor::F4 &&
                          mid.plan(SpireMidFloor::F5).elevStop == (uint32_t)L1Floor::F5;
        check(reach && stopsMatch, "S21 F3/F4/F5 reachable via elevator (stop == floor index)");
    }

    // ---- Arrival positions land on the correct plate (Y matches the floor base Y). ----
    {
        bool yOk =
            std::fabs(mid.plan(SpireMidFloor::F3).arrival.y -
                      (layout.floorBaseY[(uint32_t)L1Floor::F3] + 0.05f)) < 1e-3f &&
            std::fabs(mid.plan(SpireMidFloor::F4).arrival.y -
                      (layout.floorBaseY[(uint32_t)L1Floor::F4] + 0.05f)) < 1e-3f &&
            std::fabs(mid.plan(SpireMidFloor::F5).arrival.y -
                      (layout.floorBaseY[(uint32_t)L1Floor::F5] + 0.05f)) < 1e-3f;
        check(yOk, "S22 arrival positions sit on each floor's plate (correct base Y)");
    }

    // ---- Keypad doors: one per floor, LOCKED with the authored code; the right code
    // in range unlocks, the wrong code does not. ----
    {
        check(mid.doors().count() == 3, "S23 three mid-floor keypad doors built");
        bool allLockedCoded = true;
        for (uint32_t i = 0; i < mid.doors().count(); ++i) {
            const Door& d = mid.doors().at(i);
            if (!d.locked || d.code == 0) allLockedCoded = false;
        }
        check(allLockedCoded, "S24 all three doors LOCKED + carry a keypad code");

        // Each floor's plan reports its code; assert the set matches what we authored.
        bool planCodes = mid.plan(SpireMidFloor::F3).doorCode == 3300 &&
                         mid.plan(SpireMidFloor::F4).doorCode == 4040 &&
                         mid.plan(SpireMidFloor::F5).doorCode == 5500;
        check(planCodes, "S25 per-floor door codes (3300/4040/5500) as authored");

        // Right code at the F3 door opens it; wrong code does not at the F3 door.
        const Door& d3 = mid.doors().at(0);   // F3 built first
        x3::phys::Vec3 atD3{ d3.closedPos.x, d3.closedPos.y + 1.0f, d3.closedPos.z };
        bool wrongRejected = !mid.tryDoorCode(atD3, 9999);   // wrong code: no open
        bool stillLocked   = mid.doors().at(0).locked;
        bool rightOpens    = mid.tryDoorCode(atD3, 3300);    // right code: opens
        check(wrongRejected && stillLocked && rightOpens,
              "S26 keypad: wrong code rejected, right code (3300) opens F3 door");
    }

    physics->shutdown();
    x3::logInfo(std::string("spiremid: ") + std::to_string(g_pass) + "/" +
                std::to_string(g_pass + g_fail) + " passed");
    return g_fail == 0;
}

// ===========================================================================
// Headless self-test (--test-dronehack) — the F5 Drone Manufacturing master hack.
// ===========================================================================
namespace {

int dh_pass = 0, dh_fail = 0;
void dhcheck(bool cond, const char* name) {
    if (cond) { ++dh_pass; x3::logInfo(std::string("[dronehack-test] PASS ") + name); }
    else      { ++dh_fail; x3::logError(std::string("[dronehack-test] FAIL ") + name); }
}

// Are ALL F5 ranged drones hostile (non-zero attack damage, not allied)?
bool allDronesHostile(const SpireMidFloors& mid) {
    const MonsterManager& f5 = mid.enemies(SpireMidFloor::F5);
    bool any = false;
    for (uint32_t i = 0; i < f5.count(); ++i) {
        if (!f5.at(i).ranged()) continue;
        any = true;
        if (f5.at(i).attackDamage() <= 0 || f5.at(i).isAllied()) return false;
    }
    return any;
}

// Are ALL F5 ranged drones allied (attack damage zeroed, isAllied)?
bool allDronesAllied(const SpireMidFloors& mid) {
    const MonsterManager& f5 = mid.enemies(SpireMidFloor::F5);
    bool any = false;
    for (uint32_t i = 0; i < f5.count(); ++i) {
        if (!f5.at(i).ranged()) continue;
        any = true;
        if (!f5.at(i).isAllied() || f5.at(i).attackDamage() != 0) return false;
    }
    return any;
}

} // namespace

bool runDroneHackSelfTest() {
    dh_pass = dh_fail = 0;

    std::unique_ptr<x3::phys::IPhysicsWorld> physics(x3::phys::createPhysicsWorld());
    physics->init();
    HeadlessDevice device;
    Scene scene;

    Level1Layout layout = buildLevel1(scene, device, *physics);
    TriggerSystem triggers;
    SpireMidFloors mid;
    mid.build(scene, device, *physics, layout, triggers, riggedGlbRoot());
    dhcheck(mid.built(), "D0 F5 Drone Manufacturing content built");

    // ---- The Swarm Controller AI boss + the hackable drone set exist. ----
    dhcheck(mid.swarmBoss().count() == 1 &&
            mid.swarmBoss().at(0).type() == MonsterType::Boss &&
            mid.swarmBoss().at(0).alive() &&
            mid.f5DroneCount() == 5,
            "D1 Swarm Controller AI boss + 5 hackable drones present");

    // ---- GATED: the hack is NOT performed at load; drones hostile + boss at full HP. ----
    const int swarmMax = mid.swarmBoss().at(0).maxHp();
    dhcheck(!mid.sarahHackDone(), "D2 master hack NOT performed at load (gated)");
    dhcheck(allDronesHostile(mid) && mid.swarmBoss().at(0).hp() == swarmMax,
            "D3 drones HOSTILE + Swarm AI at FULL HP before the hack");

    // ---- Run Sarah's master hack (the gated trigger/interact). ----
    ScriptedFightHook::Result res = mid.runSarahMasterHack();
    const int expectStrip = (int)(swarmMax * 0.75f + 0.5f);
    bool hpOk = res.hpStripped == expectStrip &&
                mid.swarmBoss().at(0).hp() == swarmMax - expectStrip &&
                mid.swarmBoss().at(0).hp() >= 1 &&        // never killed outright
                mid.swarmBoss().at(0).alive();
    dhcheck(mid.sarahHackDone() && hpOk,
            "D4 hack strips ~75% of the Swarm AI's HP (boss survives, still fights)");
    dhcheck(res.dronesFlipped == 5 && allDronesAllied(mid),
            "D5 hack flips the entire drone set to ALLIED (damage zeroed)");

    // ---- Idempotent: a second call is a no-op. ----
    ScriptedFightHook::Result res2 = mid.runSarahMasterHack();
    dhcheck(res2.hpStripped == 0 && res2.dronesFlipped == 0,
            "D6 master hack is idempotent (second call no-op)");

    physics->shutdown();
    x3::logInfo(std::string("dronehack: ") + std::to_string(dh_pass) + "/" +
                std::to_string(dh_pass + dh_fail) + " passed");
    return dh_fail == 0;
}

} // namespace x3::game
