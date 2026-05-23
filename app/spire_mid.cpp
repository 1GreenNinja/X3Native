// EFLZ Act 1 "The Spire" — F3/F4/F5 mid-floor encounter content. See spire_mid.h.
//
// Clean-room: built ONLY from the existing Scene/monster/rescue/door/trigger
// systems + the engine interfaces. No purchased C# / id Tech / RBDOOM source
// consulted. CONTENT/LEVEL-SCRIPT ONLY — no renderer or core-engine changes; this
// composes the data-driven roster (monster.*) + rescue/door/trigger onto the plates
// buildLevel1() already produced. Mirrors level1_game.cpp's authoring style exactly.
#include "spire_mid.h"
#include "asset_root.h"
#include "headless_device.h"

#include "engine/core/x3_log.h"

#include <cmath>
#include <memory>
#include <string>

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
    // F3 — LABS (research wing). Difficulty FLOOR (entry). "Infected enemies" read
    // as a melee-led pack: 3 melee (2 DominionTroopers + 1 Verthani flanker) + 1
    // ranged BlueSynth covering from the back. Total 4. Placed in the +X half of the
    // plate, off the elevator-doorway spine (z=0 near the shaft) so an arriving rider
    // isn't ambushed in the shaft mouth. A keypad door (lab keycode) gates the inner
    // lab partition.
    // ===================================================================
    {
        const uint32_t fi = (uint32_t)SpireMidFloor::F3;
        const L1Floor   f = L1Floor::F3;
        SpireFloorPlan& p = m_plan[fi];
        p.floor    = f;
        p.elevStop = (uint32_t)f;                       // one elevator stop per floor
        p.baseY    = layout.floorBaseY[(uint32_t)f];
        p.arrival  = at(f, 17.5f, 0.05f, 0.0f);         // step off the shaft (x=19.5) onto the plate

        // Infected research pack — front melee pair advances; a Verthani darts/flanks;
        // a BlueSynth snipes from the far -X end. Counts/roles from the roster.
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
        p.totalCount  = p.meleeCount + p.rangedCount;

        // Lab keypad door: locked, code 3300 (research wing). Sits in a partition in
        // the middle of the plate (x=8) so the inner lab is gated. AlongZ (wall runs
        // along Z, door thin in X), same as the B1 spine doors.
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
    // F4 — OFFICES (cubicle combat sprawl). Escalates F3: 5 enemies, a ranged ELITE
    // added. "Occupation troopers; cover; door-override puzzle." 3 melee
    // (DominionTroopers) holding cover lines + 1 BlueSynth + 1 Illuminated elite
    // holding a long standoff. Total 5. A door-override keypad (code 4040).
    // ===================================================================
    {
        const uint32_t fi = (uint32_t)SpireMidFloor::F4;
        const L1Floor   f = L1Floor::F4;
        SpireFloorPlan& p = m_plan[fi];
        p.floor    = f;
        p.elevStop = (uint32_t)f;
        p.baseY    = layout.floorBaseY[(uint32_t)f];
        p.arrival  = at(f, 17.5f, 0.05f, 0.0f);

        // Three troopers across the cubicle floor (cover lines), a synth flanker, and
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
        p.totalCount  = p.meleeCount + p.rangedCount;

        // Door-override keypad: locked, code 4040. Partition at x=8.
        DoorSpec d; d.doorwayCenter = at(f, 8.0f, 0.0f, 0.0f); d.axis = DoorAxis::AlongZ;
        d.withButton = false; d.locked = true; d.code = 4040;   // door-override code
        d.tint[0]=0.70f; d.tint[1]=0.66f; d.tint[2]=0.50f;      // office tan
        buildLevelDoor(scene, m_doors, device, physics, d);
        p.doorCode  = 4040;
        p.hasVictim = false;

        triggers.add(x3::phys::Vec3{ tbl[(uint32_t)f].x1 - 8.0f, p.baseY,        -6.0f },
                     x3::phys::Vec3{ tbl[(uint32_t)f].x1,        p.baseY + 3.0f,  6.0f },
                     (uint32_t)SpireMidTrigger::F4Hub, true);
    }

    // ===================================================================
    // F5 — R&D / SYNTH BAY (high-bay). Hardest mid floor: 6 enemies, RANGED-led synth
    // waves + a 2nd Illuminated elite. 1 melee Verthani (a darting infected) + 5
    // ranged (3 BlueSynth + 2 Illuminated). Total 6. PLUS one rescue captive (a lab
    // tech) on a timer GATED on the F5 hub (never armed at load); if it expires the
    // victim transforms into a mini-boss. A synth-bay keypad door (code 5500).
    // ===================================================================
    {
        const uint32_t fi = (uint32_t)SpireMidFloor::F5;
        const L1Floor   f = L1Floor::F5;
        SpireFloorPlan& p = m_plan[fi];
        p.floor    = f;
        p.elevStop = (uint32_t)f;
        p.baseY    = layout.floorBaseY[(uint32_t)f];
        p.arrival  = at(f, 17.5f, 0.05f, 0.0f);

        // Synth wave: a lone Verthani melee harrier weaving in, three BlueSynths
        // laying down ranged fire across the bay, and two Illuminated elites anchoring
        // the far standoff. The melee cap (combat::kMaxMeleeAttackers) is irrelevant
        // with one melee unit, and the ranged units hold their distance, so the floor
        // is dense but winnable (no dogpile) — escalation via ranged pressure, not a
        // pile of melee.
        m_enemies[fi].spawn(scene, device, physics, m_modelDir,
                            at(f, 13.0f, kEnemyYOff,  0.0f), tuningFor(EnemyType::Verthani));
        m_enemies[fi].spawn(scene, device, physics, m_modelDir,
                            at(f, 10.0f, kEnemyYOff, -4.0f), tuningFor(EnemyType::BlueSynth));
        m_enemies[fi].spawn(scene, device, physics, m_modelDir,
                            at(f, 10.0f, kEnemyYOff,  4.0f), tuningFor(EnemyType::BlueSynth));
        m_enemies[fi].spawn(scene, device, physics, m_modelDir,
                            at(f,  6.0f, kEnemyYOff,  0.0f), tuningFor(EnemyType::BlueSynth));
        m_enemies[fi].spawn(scene, device, physics, m_modelDir,
                            at(f,  3.0f, kEnemyYOff, -4.0f), tuningFor(EnemyType::Illuminated));
        m_enemies[fi].spawn(scene, device, physics, m_modelDir,
                            at(f,  3.0f, kEnemyYOff,  4.0f), tuningFor(EnemyType::Illuminated));
        p.meleeCount  = 1;   // 1 Verthani (Guard archetype)
        p.rangedCount = 5;   // 3 BlueSynth + 2 Illuminated (Drone archetype)
        p.totalCount  = p.meleeCount + p.rangedCount;

        // Synth-bay keypad door: locked, code 5500. Partition at x=8.
        DoorSpec d; d.doorwayCenter = at(f, 8.0f, 0.0f, 0.0f); d.axis = DoorAxis::AlongZ;
        d.withButton = false; d.locked = true; d.code = 5500;   // synth-bay code
        d.tint[0]=0.45f; d.tint[1]=0.55f; d.tint[2]=0.85f;      // synth blue
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
    x3::logInfo("SpireMidFloors::build complete — F3(4 enemies, code 3300), "
                "F4(5 enemies, code 4040), F5(6 enemies + 1 rescue captive 'Lena' "
                "[timer gated on F5 hub], code 5500); 3 keypad doors, 3 floor-hub triggers");
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

    // F5 rescue victim: tick the timer (gated on m_f5HubReached) + companion follow,
    // and spawn the mini-boss the FRAME the timer expires (mirrors RescueSystem::tick).
    if (m_victim) {
        const bool expiredNow =
            m_victim->tick(dt, m_f5HubReached, scene, physics, playerPos);
        if (expiredNow && m_device) {
            const x3::phys::Vec3 bossAt{ m_victim->pos().x, kEnemyYOff, m_victim->pos().z };
            m_victimBoss.spawn(scene, *m_device, physics, m_modelDir, bossAt,
                               m_victim->bossTuning());
            x3::logInfo("[spiremid] F5 captive 'Lena' transformed — synth-bay mini-boss spawned");
        }
    }
    // The transformed mini-boss chases/attacks like the rescue bosses do.
    m_victimBoss.update(dt, scene, physics, eye, atkTarget, attackFx);
}

void SpireMidFloors::onTrigger(uint32_t triggerId) {
    switch ((SpireMidTrigger)triggerId) {
        case SpireMidTrigger::F3Hub:
            if (!m_f3HubReached) {
                m_f3HubReached = true;
                x3::logInfo("SpireMid: F3 LABS hub reached — research-wing encounter armed");
            }
            break;
        case SpireMidTrigger::F4Hub:
            if (!m_f4HubReached) {
                m_f4HubReached = true;
                x3::logInfo("SpireMid: F4 OFFICES hub reached — cubicle encounter armed");
            }
            break;
        case SpireMidTrigger::F5Hub:
            // PLAYTEST-FIX mirror: the player reached the F5 synth bay — START the
            // rescue clock NOW (not at load). Idempotent.
            if (!m_f5HubReached) {
                m_f5HubReached = true;
                x3::logInfo("SpireMid: F5 SYNTH BAY hub reached — rescue timer started "
                            "('Lena' now counting down)");
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
                                  int damage) {
    FireResult r;
    for (uint32_t i = 0; i < (uint32_t)SpireMidFloor::Count; ++i) {
        FireResult ri = m_enemies[i].fire(eye, dir, scene, physics, damage);
        if (ri.hitMonster) return ri;          // a live enemy took it — done
        if (!r.hit && ri.hit) r = ri;          // remember the nearest geometry hit
    }
    // The transformed mini-boss, if any.
    FireResult rb = m_victimBoss.fire(eye, dir, scene, physics, damage);
    if (rb.hitMonster) return rb;
    if (!r.hit && rb.hit) r = rb;
    return r;
}

void SpireMidFloors::draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                          const Scene& scene) const {
    for (uint32_t i = 0; i < (uint32_t)SpireMidFloor::Count; ++i)
        m_enemies[i].drawAll(device, frame, scene);
    if (m_victim) m_victim->draw(device, frame, scene);
    m_victimBoss.drawAll(device, frame, scene);
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

    // ---- Per-floor placement COUNTS + ROLE split. ----
    {
        const SpireFloorPlan& f3 = mid.plan(SpireMidFloor::F3);
        uint32_t m3 = 0, r3 = 0; roleSplit(mid.enemies(SpireMidFloor::F3), m3, r3);
        check(mid.enemies(SpireMidFloor::F3).count() == 4 && f3.totalCount == 4 &&
              m3 == 3 && r3 == 1 && f3.meleeCount == 3 && f3.rangedCount == 1,
              "S1 F3 = 4 enemies (3 melee + 1 ranged)");

        const SpireFloorPlan& f4 = mid.plan(SpireMidFloor::F4);
        uint32_t m4 = 0, r4 = 0; roleSplit(mid.enemies(SpireMidFloor::F4), m4, r4);
        check(mid.enemies(SpireMidFloor::F4).count() == 5 && f4.totalCount == 5 &&
              m4 == 3 && r4 == 2 && f4.meleeCount == 3 && f4.rangedCount == 2,
              "S2 F4 = 5 enemies (3 melee + 2 ranged)");

        const SpireFloorPlan& f5 = mid.plan(SpireMidFloor::F5);
        uint32_t m5 = 0, r5 = 0; roleSplit(mid.enemies(SpireMidFloor::F5), m5, r5);
        check(mid.enemies(SpireMidFloor::F5).count() == 6 && f5.totalCount == 6 &&
              m5 == 1 && r5 == 5 && f5.meleeCount == 1 && f5.rangedCount == 5,
              "S3 F5 = 6 enemies (1 melee + 5 ranged)");
    }

    // ---- Difficulty escalates F3 < F4 < F5 (total counts). ----
    {
        const uint32_t c3 = mid.plan(SpireMidFloor::F3).totalCount;
        const uint32_t c4 = mid.plan(SpireMidFloor::F4).totalCount;
        const uint32_t c5 = mid.plan(SpireMidFloor::F5).totalCount;
        check(c3 < c4 && c4 < c5, "S4 difficulty escalates F3 < F4 < F5");
    }

    // ---- All mid-floor enemies are alive at load (placed, not pre-killed). ----
    {
        bool allAlive = mid.enemies(SpireMidFloor::F3).aliveCount() == 4 &&
                        mid.enemies(SpireMidFloor::F4).aliveCount() == 5 &&
                        mid.enemies(SpireMidFloor::F5).aliveCount() == 6;
        check(allAlive, "S5 all placed enemies alive at load");
    }

    // ---- F5 rescue victim PRESENT but NOT active at load. ----
    {
        bool present = mid.victimPresent();
        bool captive = mid.victimCaptive();
        bool timerStopped = !mid.victimTimerRunning();
        check(present && captive && timerStopped,
              "S6 F5 victim present + captive + timer NOT running at load");
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
              "S7 victim stays captive while hub unreached (no early expiry)");
    }

    // ---- Reaching the F5 hub starts the clock (mirrors RescueSystem::activate). ----
    {
        mid.onTrigger((uint32_t)SpireMidTrigger::F5Hub);
        check(mid.victimTimerRunning(), "S8 F5 hub trigger starts the rescue clock");
        // One tick now decrements the timer below its max.
        float before = mid.victimTimeLeft();
        mid.tick(kFixedDt, scene, *physics, layout.spawn, layout.spawn, nullptr, AttackFxFn{});
        check(mid.victimTimeLeft() < before, "S9 timer counts down after the hub");
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
        check(reach && stopsMatch, "S10 F3/F4/F5 reachable via elevator (stop == floor index)");
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
        check(yOk, "S11 arrival positions sit on each floor's plate (correct base Y)");
    }

    // ---- Keypad doors: one per floor, LOCKED with the authored code; the right code
    // in range unlocks, the wrong code does not. ----
    {
        check(mid.doors().count() == 3, "S12 three mid-floor keypad doors built");
        // Build a fresh probe at each door's closed position. (closedPos is the
        // doorway center; nearLockedCodedDoor uses XZ within range.)
        bool allLockedCoded = true;
        int codes[3] = { 3300, 4040, 5500 };
        for (uint32_t i = 0; i < mid.doors().count(); ++i) {
            const Door& d = mid.doors().at(i);
            if (!d.locked || d.code == 0) allLockedCoded = false;
        }
        check(allLockedCoded, "S13 all three doors LOCKED + carry a keypad code");

        // Each floor's plan reports its code; assert the set matches what we authored.
        bool planCodes = mid.plan(SpireMidFloor::F3).doorCode == 3300 &&
                         mid.plan(SpireMidFloor::F4).doorCode == 4040 &&
                         mid.plan(SpireMidFloor::F5).doorCode == 5500;
        check(planCodes, "S14 per-floor door codes (3300/4040/5500) as authored");
        (void)codes;

        // Right code at the F3 door opens it; wrong code does not at the F4 door.
        const Door& d3 = mid.doors().at(0);   // F3 built first
        x3::phys::Vec3 atD3{ d3.closedPos.x, d3.closedPos.y + 1.0f, d3.closedPos.z };
        bool wrongRejected = !mid.tryDoorCode(atD3, 9999);   // wrong code: no open
        bool stillLocked   = mid.doors().at(0).locked;
        bool rightOpens    = mid.tryDoorCode(atD3, 3300);    // right code: opens
        check(wrongRejected && stillLocked && rightOpens,
              "S15 keypad: wrong code rejected, right code (3300) opens F3 door");
    }

    physics->shutdown();
    x3::logInfo(std::string("spiremid: ") + std::to_string(g_pass) + "/" +
                std::to_string(g_pass + g_fail) + " passed");
    return g_fail == 0;
}

} // namespace x3::game
