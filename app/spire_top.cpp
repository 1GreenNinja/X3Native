// EFLZ Act 1 "The Spire" — F6/F7 top-floor encounter content. See spire_top.h.
//
// Clean-room: built ONLY from the existing Scene/monster/rescue/door/trigger systems
// + the engine interfaces. No purchased C# / id Tech / RBDOOM source consulted.
// CONTENT/LEVEL-SCRIPT ONLY — no renderer or core-engine changes; this composes the
// data-driven roster (monster.*) + rescue/door/trigger onto the plates buildLevel1()
// already produced. Mirrors spire_mid.cpp's authoring style exactly (the JUST-shipped
// F3/F4/F5 content), including the stacked-keypad-door 3D (include-Y) proximity fix.
#include "spire_top.h"
#include "asset_root.h"
#include "headless_device.h"
#include "spire_mid.h"   // self-test composes mid+top to assert F5 < F6 < F7 escalation

#include "engine/core/x3_log.h"

#include <cmath>
#include <memory>
#include <string>

namespace x3::game {

namespace {

// Enemy body-center Y above a plate's floor (same offset SpireMidFloors uses) — added
// to the floor's base Y so a placement lands on the plate, not the ground.
constexpr float kEnemyYOff = 0.4f;

// Portable converted-GLB root (same lazy resolve SpireMidFloors uses for the door
// mesh swap). The roster tunings carry their own model dir; the rescue victim build
// needs the rigged dir (passed in as modelDir).
const std::string& convertedDir() { static const std::string d = convertedGlbRoot(); return d; }

// ---- The F7 ACT-1 FINALE boss: "The Clone" (Jake's duplicate, master plan L7). A
// Boss-type so it runs the SAME phase machine as Chief Martinez / the rescue bosses
// (enrage at 66% HP, desperate summon at 33%). Tougher than the F5 mid-boss and the
// standard floor enemies — this is the climax of Act 1. Reuses the chief_martinez.glb
// humanoid rigged mesh (an existing asset); falls back to a tinted box if absent (the
// level never breaks). Tinted clone-cyan so it reads as Jake's mirror, not Martinez.
MonsterSystem::Tuning cloneBossTuning(std::string_view modelDir) {
    MonsterSystem::Tuning bt;
    bt.type           = MonsterType::Boss;
    bt.hp             = 620;          // Act-1 finale boss: tougher than the F5 mid-boss (460) + Martinez (340)
    bt.chaseSpeed     = 3.4f;
    bt.damage         = 14;           // within the melee band scaling; phase muls ramp it
    bt.attackRange    = 2.4f;
    bt.attackCooldown = 1.05f;
    bt.attackWindup   = 0.30f;
    bt.ranged         = false;
    bt.tint[0] = 0.55f; bt.tint[1] = 0.85f; bt.tint[2] = 1.0f; bt.tint[3] = 1.0f; // clone-cyan
    bt.modelFile        = "chief_martinez.glb";   // humanoid rigged boss mesh (existing asset)
    bt.modelDirOverride = std::string(modelDir);
    bt.standUpZtoY      = false;      // rigged boss authored Y-up
    bt.modelScale       = 1.45f;
    bt.phase3SummonCount = 2;         // desperate: summon adds (the bible "summons" beat)
    return bt;
}

// The mini-boss Sarah transforms into if her rescue timer expires (canon: the unsaved
// captive becomes a boss). A Boss-type running the same phase machine. Reuses the
// BossTheSiren.glb rescue-boss mesh (an existing asset); falls back to a box if absent.
MonsterSystem::Tuning sarahVictimBossTuning(std::string_view modelDir) {
    MonsterSystem::Tuning bt;
    bt.type           = MonsterType::Boss;
    bt.hp             = 500;          // top-floor mini-boss: above the F5 captive boss (460)
    bt.chaseSpeed     = 3.3f;
    bt.damage         = 13;
    bt.attackRange    = 2.3f;
    bt.attackCooldown = 1.1f;
    bt.attackWindup   = 0.30f;
    bt.ranged         = false;
    bt.tint[0] = 1.0f; bt.tint[1] = 0.40f; bt.tint[2] = 0.55f; bt.tint[3] = 1.0f; // siren-rose
    bt.modelFile        = "BossTheSiren.glb";
    bt.modelDirOverride = std::string(modelDir);
    bt.standUpZtoY      = false;
    bt.modelScale       = 1.35f;
    return bt;
}

} // namespace

void SpireTopFloors::build(Scene& scene, x3::rhi::IRenderDevice& device,
                           x3::phys::IPhysicsWorld& physics, const Level1Layout& layout,
                           TriggerSystem& triggers, std::string_view modelDir) {
    m_modelDir = std::string(modelDir);
    m_device   = &device;

    // The canonical floor table (footprints + base Y) — single source of truth shared
    // with level1.cpp / env_art.cpp. We place encounters within each plate's bounds.
    const L1RoomDef* tbl = level1Rooms();

    // Convenience: a world point on a given top-floor at (x, baseY+yOff, z).
    auto at = [&](L1Floor f, float x, float yOff, float z) {
        return x3::phys::Vec3{ x, layout.floorBaseY[(uint32_t)f] + yOff, z };
    };

    // ===================================================================
    // F6 — ALIEN TECHNOLOGY LAB (first contact). The penultimate floor: a mixed
    // occupation / Salvari-holding push escalating beyond F5's standard set — 7
    // standard enemies (3 melee: 2 DominionTrooper + 1 Verthani flanker; 4 ranged:
    // 2 BlueSynth + 2 Illuminated) PLUS the floor BOSS **Alien Overseer**, the ranged
    // psychic invasion commander anchoring the cure-synthesis lab. Placed in the +X /
    // open half of the plate, off the elevator-doorway spine (x=19.5 shaft) so an
    // arriving rider isn't ambushed in the shaft mouth, and CLEAR of the -Z partition.
    // TWO keypad doors gate the cure-synthesis labs (the door-override puzzle).
    // ===================================================================
    {
        const uint32_t fi = (uint32_t)SpireTopFloor::F6;
        const L1Floor   f = L1Floor::F6;
        SpireTopPlan&   p = m_plan[fi];
        p.floor    = f;
        p.name     = "Alien Technology Lab";
        p.elevStop = (uint32_t)f;                       // one elevator stop per floor
        p.baseY    = layout.floorBaseY[(uint32_t)f];
        p.arrival  = at(f, 17.5f, 0.05f, 0.0f);         // step off the shaft (x=19.5) onto the plate

        // Occupation push: a melee front of two troopers + a Verthani flanker, backed
        // by a ranged firing line of two synths + two Illuminated elites holding the far
        // standoff. The far -X / +Z quadrant stays clear of the partition buildLevel1
        // puts in the -Z corner (x in [0,8], z<=-3), so no enemy spawns inside a wall.
        m_enemies[fi].spawn(scene, device, physics, m_modelDir,
                            at(f, 15.0f, kEnemyYOff, -4.0f), tuningFor(EnemyType::DominionTrooper));
        m_enemies[fi].spawn(scene, device, physics, m_modelDir,
                            at(f, 15.0f, kEnemyYOff,  4.0f), tuningFor(EnemyType::DominionTrooper));
        m_enemies[fi].spawn(scene, device, physics, m_modelDir,
                            at(f, 12.0f, kEnemyYOff,  0.0f), tuningFor(EnemyType::Verthani));
        m_enemies[fi].spawn(scene, device, physics, m_modelDir,
                            at(f,  9.0f, kEnemyYOff, -5.0f), tuningFor(EnemyType::BlueSynth));
        m_enemies[fi].spawn(scene, device, physics, m_modelDir,
                            at(f,  9.0f, kEnemyYOff,  5.0f), tuningFor(EnemyType::BlueSynth));
        m_enemies[fi].spawn(scene, device, physics, m_modelDir,
                            at(f,  4.0f, kEnemyYOff,  4.0f), tuningFor(EnemyType::Illuminated));
        m_enemies[fi].spawn(scene, device, physics, m_modelDir,
                            at(f, 13.0f, kEnemyYOff,  6.5f), tuningFor(EnemyType::Illuminated));
        p.meleeCount  = 3;   // 2 DominionTrooper + 1 Verthani (Guard archetype)
        p.rangedCount = 4;   // 2 BlueSynth + 2 Illuminated (Drone archetype)

        // ---- F6 BOSS: Alien Overseer. The ranged psychic invasion commander — its
        // own manager (like the F7 Clone). Reuse the Wave-1 Boss tuning (a ranged Boss
        // that commands from a standoff + runs the phase machine, summons in P3). Anchors
        // the cure-synthesis lab in the far -X / +Z open quadrant, off the shaft spine
        // and clear of the -Z partition.
        m_overseer.spawn(scene, device, physics, m_modelDir,
                         at(f, 5.0f, kEnemyYOff, 6.0f), bossTuning(BossType::AlienOverseer));
        p.bossCount   = 1;
        p.hasBoss     = true;
        p.totalCount  = p.meleeCount + p.rangedCount + p.bossCount;   // 8

        // Door-override puzzle: TWO locked keypad doors gating the cure-synthesis labs.
        // The OUTER lab door (code 6600) at x=14, and the INNER archive vault (code
        // 6611) at x=10. Both AlongZ (wall runs along Z, door thin in X), like the spine
        // doors. Distinct X so the 3D proximity test on this stacked vertical tower
        // resolves the right one.
        DoorSpec d1; d1.doorwayCenter = at(f, 14.0f, 0.0f, 0.0f); d1.axis = DoorAxis::AlongZ;
        d1.withButton = false; d1.locked = true; d1.code = 6600;     // outer cure-lab code
        d1.tint[0]=0.50f; d1.tint[1]=0.62f; d1.tint[2]=0.70f;        // alien-tech teal
        buildLevelDoor(scene, m_doors, device, physics, d1);

        DoorSpec d2; d2.doorwayCenter = at(f, 10.0f, 0.0f, 0.0f); d2.axis = DoorAxis::AlongZ;
        d2.withButton = false; d2.locked = true; d2.code = 6611;     // inner archive override code
        d2.tint[0]=0.40f; d2.tint[1]=0.50f; d2.tint[2]=0.58f;        // darker archive tone
        buildLevelDoor(scene, m_doors, device, physics, d2);

        p.doorCode  = 6600;
        p.doorCode2 = 6611;
        p.hasVictim = false;

        // Hub trigger where the rider arrives (alarm/objective hook).
        triggers.add(x3::phys::Vec3{ tbl[(uint32_t)f].x1 - 8.0f, p.baseY,        -6.0f },
                     x3::phys::Vec3{ tbl[(uint32_t)f].x1,        p.baseY + 3.0f,  6.0f },
                     (uint32_t)SpireTopTrigger::F6Hub, true);
    }

    // ===================================================================
    // F7 — EXECUTIVE LABORATORY (the ACT-1 SUMMIT / FINALE). The climactic setpiece: a
    // Boss-type "The Clone" (Jake's duplicate) anchors the exec lab, flanked by an honor
    // guard / outer ring escort. 8 combatants total = 1 Boss + a 7-strong escort, so
    // the floor cleanly exceeds F6's escort (F5 < F6 < F7). The escort is 2 melee
    // (1 Verthani enforcer + 1 DominionTrooper) + 5 ranged (2 Illuminated honor guard
    // + 3 BlueSynth), keeping the melee count low so the dogpile cap is never strained
    // while the Clone boss is the real threat. PLUS the F7 rescue objective: Sarah,
    // held in a lab holding cell, present-but-not-active-at-load and gated on the F7
    // hub. A keypad door gates the lab airlock (code 7700). F7 is the elevator's TOP
    // stop. (The Sarah outcome is the canon Alpha/Beta/Omega timeline lock; the rescue
    // lifecycle drives it — kept exactly as the Wave-1 Clone+Sarah finale.)
    // ===================================================================
    {
        const uint32_t fi = (uint32_t)SpireTopFloor::F7;
        const L1Floor   f = L1Floor::F7;
        SpireTopPlan&   p = m_plan[fi];
        p.floor    = f;
        p.name     = "Executive Laboratory";
        p.elevStop = (uint32_t)f;                       // F7's stop is the elevator TOP stop
        p.baseY    = layout.floorBaseY[(uint32_t)f];
        p.arrival  = at(f, 17.5f, 0.05f, 0.0f);

        // ---- The Clone boss anchors the exec-lab center (off the shaft spine). ----
        m_boss.spawn(scene, device, physics, m_modelDir,
                     at(f, 8.0f, kEnemyYOff, 0.0f), cloneBossTuning(m_modelDir));
        p.bossCount = 1;

        // ---- The honor guard + outer ring (the boss's escort), 8 strong. 2 Illuminated
        // honor guard hold close standoff, a Verthani enforcer + a DominionTrooper press
        // melee (the only two melee units — the cap is relevant but never strained), and
        // 4 BlueSynths lay down ranged fire across the lab. Escort 8 + boss 1 = 9
        // combatants > F6's 8 (the Act-1 FINALE is the densest floor). All off the shaft
        // spine; clear of the holding-cell partition (x in [0,8], z<=-3) where Sarah is held.
        m_enemies[fi].spawn(scene, device, physics, m_modelDir,
                            at(f, 11.0f, kEnemyYOff, -3.0f), tuningFor(EnemyType::Illuminated));
        m_enemies[fi].spawn(scene, device, physics, m_modelDir,
                            at(f, 11.0f, kEnemyYOff,  3.0f), tuningFor(EnemyType::Illuminated));
        m_enemies[fi].spawn(scene, device, physics, m_modelDir,
                            at(f, 13.0f, kEnemyYOff,  0.0f), tuningFor(EnemyType::Verthani));
        m_enemies[fi].spawn(scene, device, physics, m_modelDir,
                            at(f, 15.0f, kEnemyYOff,  5.0f), tuningFor(EnemyType::DominionTrooper));
        m_enemies[fi].spawn(scene, device, physics, m_modelDir,
                            at(f,  5.0f, kEnemyYOff, -5.0f), tuningFor(EnemyType::BlueSynth));
        m_enemies[fi].spawn(scene, device, physics, m_modelDir,
                            at(f,  5.0f, kEnemyYOff,  5.0f), tuningFor(EnemyType::BlueSynth));
        m_enemies[fi].spawn(scene, device, physics, m_modelDir,
                            at(f,  6.0f, kEnemyYOff,  6.5f), tuningFor(EnemyType::BlueSynth));
        m_enemies[fi].spawn(scene, device, physics, m_modelDir,
                            at(f,  6.0f, kEnemyYOff, -6.5f), tuningFor(EnemyType::BlueSynth));
        p.meleeCount  = 2;   // 1 Verthani + 1 DominionTrooper (Guard archetype)
        p.rangedCount = 6;   // 2 Illuminated honor guard + 4 BlueSynth (Drone archetype)
        // totalCount counts EVERY combatant on the floor including the Clone boss:
        // escort 8 (2 melee + 6 ranged) + boss 1 = 9, clearing F6's 8.
        p.totalCount  = p.meleeCount + p.rangedCount + p.bossCount;

        // ---- Exec-lab airlock keypad door: locked, code 7700. Gates the holding cell
        // partition. AlongZ at x=6 (the lab partition). The boss spawns at x=8; the door
        // body is a thin slab in the doorway gap, on the holding-cell side. Distinct from
        // the F6 doors' X so the 3D proximity resolves it.
        DoorSpec d; d.doorwayCenter = at(f, 6.0f, 0.0f, 0.0f); d.axis = DoorAxis::AlongZ;
        d.withButton = false; d.locked = true; d.code = 7700;       // rooftop airlock code
        d.tint[0]=0.60f; d.tint[1]=0.66f; d.tint[2]=0.80f;          // exec-lab steel tone
        buildLevelDoor(scene, m_doors, device, physics, d);
        p.doorCode  = 7700;
        p.doorCode2 = 0;
        p.hasBoss   = true;

        // ---- F7 rescue captive: SARAH (canon: rescued F7, becomes co-fighter; her
        // outcome is the Alpha/Beta/Omega timeline lock). Held in an exec-lab holding
        // cell in the -Z corner. The timer is GATED on the F7
        // hub being reached (m_f7HubReached, default FALSE) — we do NOT activate it
        // here, so the 5-min clock cannot expire at load and spawn the mini-boss on the
        // first frame (the playtest bug spire_mid fixed). The host registers the F7Hub
        // trigger below; entering it (onTrigger) flips m_f7HubReached and the clock
        // starts. Distinct mesh (AnnaCasual) from the F5 captive 'Lena' (AnnaTactical).
        const x3::phys::Vec3 sarahPos = at(f, 3.0f, kEnemyYOff, -5.5f);  // -Z holding-cell corner
        m_victim = std::make_unique<RescueVictim>();
        m_victim->build(scene, device, physics, m_modelDir, sarahPos,
                        VictimId::Aria, "Sarah", "AnnaCasual.glb",
                        kRescueTimer, sarahVictimBossTuning(m_modelDir));
        p.hasVictim = true;

        triggers.add(x3::phys::Vec3{ tbl[(uint32_t)f].x1 - 8.0f, p.baseY,        -6.0f },
                     x3::phys::Vec3{ tbl[(uint32_t)f].x1,        p.baseY + 3.0f,  6.0f },
                     (uint32_t)SpireTopTrigger::F7Hub, true);
    }

    // Try to load the shared real-door GLB so the keypad doors render as meshes (the
    // collision box stays). No-op/harmless if the GLB is absent (graybox fallback).
    m_doors.loadDoorMesh(device, convertedDir());

    m_built = true;
    x3::logInfo("SpireTopFloors::build complete — F6 ALIEN TECHNOLOGY LAB (7 enemies + "
                "boss 'Alien Overseer' = 8 combatants, codes 6600/6611), "
                "F7 EXECUTIVE LABORATORY (boss 'The Clone' + 7 escort = 8 combatants + "
                "1 rescue captive 'Sarah' [timer gated on F7 hub], code 7700); "
                "3 keypad doors, 2 floor-hub triggers");
}

void SpireTopFloors::tick(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics,
                          const x3::phys::Vec3& eye, const x3::phys::Vec3& playerPos,
                          IDamageSink* player, const AttackFxFn& attackFx) {
    if (!m_built) return;

    // Keypad doors animate.
    m_doors.update(dt, scene, physics);

    // Enemies + the bosses attack only while the player is alive (matches the others).
    IDamageSink* atkTarget = (player && player->isAlive()) ? player : nullptr;
    for (uint32_t i = 0; i < (uint32_t)SpireTopFloor::Count; ++i)
        m_enemies[i].update(dt, scene, physics, eye, atkTarget, attackFx);
    m_overseer.update(dt, scene, physics, eye, atkTarget, attackFx);  // F6 Alien Overseer
    m_boss.update(dt, scene, physics, eye, atkTarget, attackFx);      // F7 Clone

    // F7 rescue victim (Sarah): tick the timer (gated on m_f7HubReached) + companion
    // follow, and spawn the mini-boss the FRAME the timer expires (mirrors the F5
    // captive in spire_mid / RescueSystem::tick).
    if (m_victim) {
        const bool expiredNow =
            m_victim->tick(dt, m_f7HubReached, scene, physics, playerPos);
        if (expiredNow && m_device) {
            const x3::phys::Vec3 bossAt{ m_victim->pos().x, kEnemyYOff, m_victim->pos().z };
            m_victimBoss.spawn(scene, *m_device, physics, m_modelDir, bossAt,
                               m_victim->bossTuning());
            x3::logInfo("[spiretop] F7 captive 'Sarah' transformed — rooftop mini-boss spawned");
        }
    }
    // The transformed mini-boss chases/attacks like the rescue bosses do.
    m_victimBoss.update(dt, scene, physics, eye, atkTarget, attackFx);
}

void SpireTopFloors::onTrigger(uint32_t triggerId) {
    switch ((SpireTopTrigger)triggerId) {
        case SpireTopTrigger::F6Hub:
            if (!m_f6HubReached) {
                m_f6HubReached = true;
                x3::logInfo("SpireTop: F6 ALIEN TECHNOLOGY LAB hub reached — first-contact "
                            "encounter armed (Alien Overseer commands the cure-synth lab)");
            }
            break;
        case SpireTopTrigger::F7Hub:
            // PLAYTEST-FIX mirror: the player reached the F7 rooftop summit — START
            // Sarah's rescue clock NOW (not at load). Idempotent.
            if (!m_f7HubReached) {
                m_f7HubReached = true;
                x3::logInfo("SpireTop: F7 EXECUTIVE LABORATORY summit hub reached — Sarah's "
                            "rescue timer started (the Act-1 finale + timeline lock begins)");
            }
            break;
    }
}

bool SpireTopFloors::onRescue(const x3::phys::Vec3& playerPos, float range) {
    if (!m_victim) return false;
    return m_victim->tryRescue(playerPos, range);
}

FireResult SpireTopFloors::onFire(const x3::phys::Vec3& eye, const x3::phys::Vec3& dir,
                                  Scene& scene, x3::phys::IPhysicsWorld& physics,
                                  int damage, x3::DamageType type) {
    FireResult r;
    for (uint32_t i = 0; i < (uint32_t)SpireTopFloor::Count; ++i) {
        FireResult ri = m_enemies[i].fire(eye, dir, scene, physics, damage, type);
        if (ri.hitMonster) return ri;          // a live enemy took it — done
        if (!r.hit && ri.hit) r = ri;          // remember the nearest geometry hit
    }
    // The F6 Alien Overseer boss.
    FireResult rovr = m_overseer.fire(eye, dir, scene, physics, damage, type);
    if (rovr.hitMonster) return rovr;
    if (!r.hit && rovr.hit) r = rovr;
    // The F7 Clone boss.
    FireResult rboss = m_boss.fire(eye, dir, scene, physics, damage, type);
    if (rboss.hitMonster) return rboss;
    if (!r.hit && rboss.hit) r = rboss;
    // The transformed mini-boss, if any.
    FireResult rb = m_victimBoss.fire(eye, dir, scene, physics, damage, type);
    if (rb.hitMonster) return rb;
    if (!r.hit && rb.hit) r = rb;
    return r;
}

void SpireTopFloors::draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                          const Scene& scene) const {
    for (uint32_t i = 0; i < (uint32_t)SpireTopFloor::Count; ++i)
        m_enemies[i].drawAll(device, frame, scene);
    m_overseer.drawAll(device, frame, scene);   // F6 Alien Overseer
    m_boss.drawAll(device, frame, scene);       // F7 Clone
    if (m_victim) m_victim->draw(device, frame, scene);
    m_victimBoss.drawAll(device, frame, scene);
}

bool SpireTopFloors::nearLockedCodedDoor(const x3::phys::Vec3& playerPos, float range) const {
    const float r2 = range * range;
    for (uint32_t i = 0; i < m_doors.count(); ++i) {
        const Door& d = m_doors.at(i);
        if (!d.locked || d.code == 0) continue;
        // 3D distance: the top-floor doors STACK at the same XZ on the vertical tower
        // (F6/F7 doors share x positions, differing in Y), and F6 itself has two
        // doors close together, so Y AND X must distinguish them — an XZ-only test
        // would treat stacked floors as the same spot. (spire_mid's self-fix.)
        const float dx = playerPos.x - d.closedPos.x;
        const float dy = playerPos.y - d.closedPos.y;
        const float dz = playerPos.z - d.closedPos.z;
        if (dx * dx + dy * dy + dz * dz <= r2) return true;
    }
    return false;
}

bool SpireTopFloors::tryDoorCode(const x3::phys::Vec3& playerPos, int code, float range) {
    const float r2 = range * range;
    int best = -1; float bestD2 = r2;
    for (uint32_t i = 0; i < m_doors.count(); ++i) {
        const Door& d = m_doors.at(i);
        if (!d.locked || d.code == 0) continue;
        // 3D distance (include Y) so the nearest LOCKED coded door is the one on the
        // player's CURRENT floor at the closest doorway, not a stacked-floor tie.
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

bool SpireTopFloors::victimCaptive() const {
    return m_victim && m_victim->captive();
}
float SpireTopFloors::victimTimeLeft() const {
    return m_victim ? m_victim->timeLeft() : 0.0f;
}

bool SpireTopFloors::reachableViaElevator(SpireTopFloor f, uint32_t elevatorStopCount) const {
    if (!m_built) return false;
    const uint32_t stop = m_plan[(uint32_t)f].elevStop;
    return stop < elevatorStopCount;   // the host builds one stop per floor (0..count-1)
}

// ===========================================================================
// Headless self-test (--test-spiretop).
// ===========================================================================
namespace {

int g_pass = 0, g_fail = 0;
void check(bool cond, const char* name) {
    if (cond) { ++g_pass; x3::logInfo(std::string("[spiretop-test] PASS ") + name); }
    else      { ++g_fail; x3::logError(std::string("[spiretop-test] FAIL ") + name); }
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

} // namespace

bool runSpireTopSelfTest() {
    g_pass = g_fail = 0;

    std::unique_ptr<x3::phys::IPhysicsWorld> physics(x3::phys::createPhysicsWorld());
    physics->init();

    HeadlessDevice device;
    Scene scene;

    // Build the Spire geometry (gives us floorBaseY[] + the plate footprints), then
    // author BOTH the mid floors (for the F5 baseline) and the top floors on top,
    // sharing a TriggerSystem like the host does.
    Level1Layout layout = buildLevel1(scene, device, *physics);
    TriggerSystem triggers;
    const std::string rigged = riggedGlbRoot();

    SpireMidFloors mid;
    mid.build(scene, device, *physics, layout, triggers, rigged);

    SpireTopFloors top;
    top.build(scene, device, *physics, layout, triggers, rigged);

    check(top.built(), "S0 top floors built");

    // ---- Canon floor identities (the Wave-2 re-theme). ----
    {
        bool names = std::string(top.plan(SpireTopFloor::F6).name) == "Alien Technology Lab" &&
                     std::string(top.plan(SpireTopFloor::F7).name) == "Executive Laboratory";
        check(names, "S1a canon floor identities (Alien Technology Lab / Executive Laboratory)");
    }

    // ---- Per-floor placement COUNTS + ROLE split. ----
    {
        const SpireTopPlan& f6 = top.plan(SpireTopFloor::F6);
        uint32_t m6 = 0, r6 = 0; roleSplit(top.enemies(SpireTopFloor::F6), m6, r6);
        // F6 standard push (in m_enemies): 3 melee + 4 ranged = 7, plus the Alien
        // Overseer boss in its own manager (overseerBoss()): 1. Total combatants = 8.
        check(top.enemies(SpireTopFloor::F6).count() == 7 &&
              m6 == 3 && r6 == 4 && f6.meleeCount == 3 && f6.rangedCount == 4 &&
              f6.bossCount == 1 && f6.totalCount == 8 && top.overseerBoss().count() == 1,
              "S1 F6 push = 7 (3 melee + 4 ranged) + 1 Alien Overseer boss (total 8)");

        const SpireTopPlan& f7 = top.plan(SpireTopFloor::F7);
        uint32_t m7 = 0, r7 = 0; roleSplit(top.enemies(SpireTopFloor::F7), m7, r7);
        // F7 escort (in m_enemies): 2 melee + 6 ranged = 8, plus the Clone boss in its
        // own manager (boss()): 1. Total combatants = 9. totalCount records 9.
        check(top.enemies(SpireTopFloor::F7).count() == 8 &&
              m7 == 2 && r7 == 6 && f7.meleeCount == 2 && f7.rangedCount == 6 &&
              f7.bossCount == 1 && f7.totalCount == 9 && top.boss().count() == 1,
              "S2 F7 escort = 8 (2 melee + 6 ranged) + 1 Clone boss (total 9)");
    }

    // ---- F6 carries the Alien Overseer boss (live Boss-type leader) at load. ----
    {
        bool hasOverseer = top.plan(SpireTopFloor::F6).hasBoss &&
                           top.overseerBoss().count() == 1 &&
                           top.overseerBoss().at(0).type() == MonsterType::Boss &&
                           top.overseerBoss().at(0).alive() &&
                           top.overseerBoss().aliveCount() == 1 &&
                           top.overseerBoss().at(0).maxHp() == bossTuning(BossType::AlienOverseer).hp;
        check(hasOverseer, "S2b F6 carries the Alien Overseer boss (live Boss-type)");
    }

    // ---- The F7 finale carries a BOSS-type leader (The Clone), alive at load. ----
    {
        bool hasBoss = top.plan(SpireTopFloor::F7).hasBoss &&
                       top.boss().count() == 1 &&
                       top.boss().at(0).type() == MonsterType::Boss &&
                       top.boss().at(0).alive() &&
                       top.boss().aliveCount() == 1;
        check(hasBoss, "S3 F7 finale Clone is a live Boss-type leader");
    }

    // ---- Difficulty escalates F5 < F6 < F7 (TOTAL combatant counts incl. each floor
    // boss). spire_mid supplies the F5 baseline (now 7: standard 6 + the Swarm AI boss). ----
    {
        const uint32_t c5 = mid.plan(SpireMidFloor::F5).totalCount;            // 7 (6 + Swarm AI)
        const uint32_t c6 = top.plan(SpireTopFloor::F6).totalCount;            // 8 (7 + Overseer)
        const uint32_t c7 = top.plan(SpireTopFloor::F7).totalCount;            // 9 (8 escort + Clone)
        // Cross-check the plan totals against the live managers (push/escort + boss).
        const uint32_t c6live = top.enemies(SpireTopFloor::F6).count() + top.overseerBoss().count();
        const uint32_t c7live = top.enemies(SpireTopFloor::F7).count() + top.boss().count();
        bool escalates = c5 < c6 && c6 < c7 && c6 == c6live && c7 == c7live &&
                         top.plan(SpireTopFloor::F6).hasBoss &&
                         top.plan(SpireTopFloor::F7).hasBoss &&
                         top.plan(SpireTopFloor::F7).hasVictim;
        check(escalates, "S4 difficulty escalates F5(7) < F6(8, +Overseer) < F7(9, +Clone +rescue)");
    }

    // ---- All top-floor enemies are alive at load (placed, not pre-killed). ----
    {
        bool allAlive = top.enemies(SpireTopFloor::F6).aliveCount() == 7 &&
                        top.enemies(SpireTopFloor::F7).aliveCount() == 8 &&
                        top.overseerBoss().aliveCount() == 1 &&
                        top.boss().aliveCount() == 1;
        check(allAlive, "S5 all placed enemies + the F6/F7 bosses alive at load");
    }

    // ---- F7 rescue victim (Sarah) PRESENT but NOT active at load. ----
    {
        bool present = top.victimPresent();
        bool captive = top.victimCaptive();
        bool timerStopped = !top.victimTimerRunning();
        check(present && captive && timerStopped,
              "S6 F7 victim 'Sarah' present + captive + timer NOT running at load");
    }

    // ---- The timer truly stays full until the hub fires: tick many frames at load
    // (no hub) and confirm the victim is STILL a captive (no expiry/transform). ----
    {
        for (int i = 0; i < 120; ++i) {
            top.tick(kFixedDt, scene, *physics, layout.spawn, layout.spawn, nullptr, AttackFxFn{});
            physics->step(kFixedDt);
            scene.update(*physics);
        }
        bool stillCaptive = top.victimCaptive();
        bool noBoss = top.enemies(SpireTopFloor::F7).count() == 8;   // no extra spawn
        check(stillCaptive && noBoss,
              "S7 victim stays captive while hub unreached (no early expiry)");
    }

    // ---- Reaching the F7 hub starts the clock (mirrors RescueSystem::activate). ----
    {
        top.onTrigger((uint32_t)SpireTopTrigger::F7Hub);
        check(top.victimTimerRunning(), "S8 F7 hub trigger starts Sarah's rescue clock");
        // One tick now decrements the timer below its max.
        float before = top.victimTimeLeft();
        top.tick(kFixedDt, scene, *physics, layout.spawn, layout.spawn, nullptr, AttackFxFn{});
        check(top.victimTimeLeft() < before, "S9 timer counts down after the hub");
    }

    // ---- Reachability via the elevator. The host builds one stop per floor
    // (kSpireFloorCount). Each top floor's stop index must be inside that range, equal
    // its L1Floor index, and F7 must be the TOP stop (kSpireFloorCount-1). ----
    {
        const uint32_t stops = kSpireFloorCount;   // one elevator stop per floor (B1..F7)
        bool reach = top.reachableViaElevator(SpireTopFloor::F6, stops) &&
                     top.reachableViaElevator(SpireTopFloor::F7, stops);
        bool stopsMatch = top.plan(SpireTopFloor::F6).elevStop == (uint32_t)L1Floor::F6 &&
                          top.plan(SpireTopFloor::F7).elevStop == (uint32_t)L1Floor::F7;
        bool f7IsTop = top.plan(SpireTopFloor::F7).elevStop == (stops - 1);
        check(reach && stopsMatch && f7IsTop,
              "S10 F6/F7 reachable via elevator; F7 == TOP stop (stop == floor index)");
    }

    // ---- Arrival positions land on the correct plate (Y matches the floor base Y). ----
    {
        bool yOk =
            std::fabs(top.plan(SpireTopFloor::F6).arrival.y -
                      (layout.floorBaseY[(uint32_t)L1Floor::F6] + 0.05f)) < 1e-3f &&
            std::fabs(top.plan(SpireTopFloor::F7).arrival.y -
                      (layout.floorBaseY[(uint32_t)L1Floor::F7] + 0.05f)) < 1e-3f;
        check(yOk, "S11 arrival positions sit on each floor's plate (correct base Y)");
    }

    // ---- Keypad doors: F6 has TWO, F7 has ONE (3 total), all LOCKED with the authored
    // codes; the right code in range unlocks, the wrong code does not. ----
    {
        check(top.doors().count() == 3, "S12 three top-floor keypad doors built (F6 x2 + F7 x1)");

        bool allLockedCoded = true;
        for (uint32_t i = 0; i < top.doors().count(); ++i) {
            const Door& d = top.doors().at(i);
            if (!d.locked || d.code == 0) allLockedCoded = false;
        }
        check(allLockedCoded, "S13 all three doors LOCKED + carry a keypad code");

        // Each floor's plan reports its code(s); assert the set matches what we authored.
        bool planCodes = top.plan(SpireTopFloor::F6).doorCode  == 6600 &&
                         top.plan(SpireTopFloor::F6).doorCode2 == 6611 &&
                         top.plan(SpireTopFloor::F7).doorCode  == 7700;
        check(planCodes, "S14 per-floor door codes (F6 6600/6611, F7 7700) as authored");

        // Right code at the F6 OUTER door opens it; wrong code does not. Probe at its
        // 3D closed position (the stacked-tower fix means Y matters).
        const Door& d6 = top.doors().at(0);   // F6 outer built first
        x3::phys::Vec3 atD6{ d6.closedPos.x, d6.closedPos.y + 1.0f, d6.closedPos.z };
        bool wrongRejected = !top.tryDoorCode(atD6, 1234);   // wrong code: no open
        bool stillLocked   = top.doors().at(0).locked;
        bool rightOpens    = top.tryDoorCode(atD6, 6600);    // right code: opens
        check(wrongRejected && stillLocked && rightOpens,
              "S15 keypad: wrong code rejected, right code (6600) opens F6 outer door");

        // The 3D proximity must resolve the SECOND F6 door (6611, at x=10) separately
        // from the first (6600, at x=8... actually 14/10): probe at the inner door's
        // own closed pos with its own code and confirm it opens (not the outer).
        const Door& d6b = top.doors().at(1);  // F6 inner (vault) built second
        x3::phys::Vec3 atD6b{ d6b.closedPos.x, d6b.closedPos.y + 1.0f, d6b.closedPos.z };
        bool innerOpens = top.tryDoorCode(atD6b, 6611);
        check(innerOpens, "S16 F6 inner vault door (6611) resolves + opens via 3D proximity");
    }

    physics->shutdown();
    x3::logInfo(std::string("spiretop: ") + std::to_string(g_pass) + "/" +
                std::to_string(g_pass + g_fail) + " passed");
    return g_fail == 0;
}

} // namespace x3::game
