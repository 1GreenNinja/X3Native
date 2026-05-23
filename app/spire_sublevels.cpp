// EFLZ Act 1 "The Spire" — HIDDEN Floor-7 sub-levels + the Dr. Chen Return Mission.
// See spire_sublevels.h.
//
// Clean-room: built ONLY from the existing Scene/monster/rescue/door/trigger systems +
// the engine interfaces, plus the EFLZ DESIGN docs (Tim's IP). No purchased C# / id Tech
// / RBDOOM source consulted. CONTENT/LEVEL-SCRIPT ONLY — no renderer or core-engine
// changes; this composes the data-driven roster (monster.*) + rescue/door/trigger onto
// NEW graybox plates stacked below B1. Mirrors spire_top.cpp's authoring style exactly
// (the just-shipped F6/F7 content), including the stacked-keypad-door 3D (include-Y)
// proximity fix.
#include "spire_sublevels.h"
#include "spire_top.h"     // self-test composes the F7 finale to derive the descent gate
#include "asset_root.h"
#include "headless_device.h"

#include "engine/core/x3_log.h"

#include <cmath>
#include <memory>
#include <string>

namespace x3::game {

namespace {

// Enemy body-center Y above a plate's floor (same offset SpireMidFloors/SpireTopFloors
// use) — added to the sub-level base Y so a placement lands on the plate, not the ground.
constexpr float kEnemyYOff = 0.4f;

// The three sub-levels stack DOWNWARD below B1 (whose base Y is 0), 5 m apart like the
// Spire floors (kFloorSpacing). SL1 sits one floor below B1, SL2 two below, SL3 three
// below. They are NOT elevator stops (the hidden descent is off the main floor list).
constexpr float kSubBaseY[(uint32_t)SpireSubLevel::Count] = {
    -1.0f * kFloorSpacing,   // SL1 Waste Disposal      (y = -5)
    -2.0f * kFloorSpacing,   // SL2 Cryogenic Storage   (y = -10)
    -3.0f * kFloorSpacing,   // SL3 Enhanced Interrogation (y = -15)
};

// Portable converted-GLB root (same lazy resolve SpireTopFloors uses for the door mesh
// swap). The roster tunings carry their own model dir; the Chen captive build needs the
// rigged dir (passed in as modelDir).
const std::string& convertedDir() { static const std::string d = convertedGlbRoot(); return d; }

// ---- The SL2 mini-boss: "The Frozen Collective" (merged cryo-subjects; WORLD_STRUCTURE
// §3b). Per the task lane, this is a SINGLE-BODY Boss configured LOCALLY here (NOT a new
// monster.* row): we take the Alien-Overseer phase-boss tuning as the base (a tough
// single-body phase boss) and re-skin it for the cryo theme — ice-cyan tint, a touch
// more HP than the F5/F7 victim mini-bosses but below the F7 Clone finale, and a slower,
// heavier swing. Reuses the Oracle rescue-boss mesh (an existing asset); falls back to a
// tinted box if the GLB is absent (the level never breaks). Runs the SAME HP-keyed phase
// machine as Martinez / the Clone.
MonsterSystem::Tuning frozenCollectiveTuning(std::string_view modelDir) {
    // Base off an existing single-body phase boss (Alien Overseer) so the cryo mini-boss
    // inherits sane phase thresholds + multipliers without re-deriving them.
    MonsterSystem::Tuning bt = bossTuning(BossType::AlienOverseer);
    bt.type           = MonsterType::Boss;
    bt.hp             = 540;          // cryo mini-boss: above the F5 (460)/F7 (500) victim bosses, below the Clone (620)
    bt.chaseSpeed     = 2.9f;         // heavy/lumbering frozen mass
    bt.damage         = 13;
    bt.attackRange    = 2.4f;
    bt.attackCooldown = 1.2f;
    bt.attackWindup   = 0.32f;
    bt.ranged         = false;        // a merged frozen MELEE mass (override the Overseer's ranged psychic)
    bt.standoff       = 0.0f;
    bt.tint[0] = 0.55f; bt.tint[1] = 0.85f; bt.tint[2] = 1.0f; bt.tint[3] = 1.0f; // ice-cyan
    bt.modelFile        = "Oracle.glb";   // existing rescue-boss mesh; box fallback if absent
    bt.modelDirOverride = std::string(modelDir);
    bt.standUpZtoY      = false;      // rigged boss authored Y-up
    bt.modelScale       = 1.5f;       // a big frozen mass
    bt.phase3SummonCount = 2;         // desperate: shed a couple of cryo-thralls (the "summons" beat)
    return bt;
}

// The fallback boss tuning Dr. Chen's captive carries for API symmetry with the other
// RescueVictims (every RescueVictim::build takes a bossTuning applied IF the timer
// expires). Chen's clock is gated on the SL3 hub and the Return Mission is to FREE him,
// so in normal play this is never reached; we derive it from the canon Dr. Chen boss def
// (a transforming, KILL-vs-CURE single-body boss) so an expiry reads as "Chen lost to the
// corruption." Tinted Chen-amber. Box fallback if the mesh is absent.
MonsterSystem::Tuning chenLostBossTuning(std::string_view modelDir) {
    MonsterSystem::Tuning bt = bossTuning(BossType::DrChen);
    bt.type           = MonsterType::Boss;
    bt.hp             = 480;
    bt.chaseSpeed     = 3.1f;
    bt.damage         = 12;
    bt.attackRange    = 2.3f;
    bt.attackCooldown = 1.1f;
    bt.attackWindup   = 0.30f;
    bt.ranged         = false;
    bt.tint[0] = 1.0f; bt.tint[1] = 0.72f; bt.tint[2] = 0.35f; bt.tint[3] = 1.0f; // chen-amber
    bt.modelFile        = "BossTheSiren.glb";   // existing rescue-boss mesh; box fallback if absent
    bt.modelDirOverride = std::string(modelDir);
    bt.standUpZtoY      = false;
    bt.modelScale       = 1.35f;
    return bt;
}

} // namespace

void SpireSubLevels::build(Scene& scene, x3::rhi::IRenderDevice& device,
                           x3::phys::IPhysicsWorld& physics, const Level1Layout& layout,
                           TriggerSystem& triggers, std::string_view modelDir) {
    m_modelDir  = std::string(modelDir);
    m_device    = &device;
    m_triggers  = &triggers;

    // The canonical floor table — we reuse B1's XZ footprint for the sub-level plates so a
    // hidden lift column lines up vertically below the basement (the sub-levels share the
    // tower's footprint, descending in -Y). We do NOT add new rows to level1Rooms() (not
    // this module's lane) — we just borrow B1's bounds for placement.
    const L1RoomDef* tbl = level1Rooms();
    const L1RoomDef& b1  = tbl[(uint32_t)L1Floor::B1];

    // The sub-levels descend from B1's ACTUAL base Y (read from the layout — the single
    // source of truth for floor heights), so they line up under the basement regardless of
    // where B1 sits. kSubBaseY[] are the relative offsets (-5/-10/-15 below B1).
    const float b1BaseY = layout.floorBaseY[(uint32_t)L1Floor::B1];
    auto subBaseY = [&](SpireSubLevel s) { return b1BaseY + kSubBaseY[(uint32_t)s]; };

    // Convenience: a world point on a given sub-level at (x, baseY+yOff, z).
    auto at = [&](SpireSubLevel s, float x, float yOff, float z) {
        return x3::phys::Vec3{ x, subBaseY(s) + yOff, z };
    };

    // ===================================================================
    // SUB-LEVEL 1 — WASTE DISPOSAL & FAILED EXPERIMENTS (WORLD_STRUCTURE §3b). A HAZARD
    // floor: incinerator chambers + toxic-gas vents, with Disposal Drones + Corrupted
    // Janitors. We read the roster onto this: a melee-led pack (the shambling Corrupted
    // Janitors = 3 melee DominionTrooper/Verthani) + 1 ranged Disposal Drone (BlueSynth)
    // = 4, plus a STANDING HAZARD volume (the toxic incinerator zone) that drains HP while
    // the player stands in it. A keypad door (incinerator override, code 8100) gates the
    // inner disposal chute. Placed off the hidden-lift arrival spine.
    // ===================================================================
    {
        const uint32_t si = (uint32_t)SpireSubLevel::SL1;
        const SpireSubLevel s = SpireSubLevel::SL1;
        SpireSubPlan& p = m_plan[si];
        p.sub      = s;
        p.baseY    = subBaseY(s);
        p.arrival  = at(s, 17.5f, 0.05f, 0.0f);   // step off the hidden lift onto the plate

        // Corrupted Janitors (melee) shamble forward; a Disposal Drone snipes from the back.
        m_enemies[si].spawn(scene, device, physics, m_modelDir,
                            at(s, 12.0f, kEnemyYOff, -3.0f), tuningFor(EnemyType::DominionTrooper));
        m_enemies[si].spawn(scene, device, physics, m_modelDir,
                            at(s, 12.0f, kEnemyYOff,  3.0f), tuningFor(EnemyType::DominionTrooper));
        m_enemies[si].spawn(scene, device, physics, m_modelDir,
                            at(s,  9.0f, kEnemyYOff,  0.0f), tuningFor(EnemyType::Verthani));
        m_enemies[si].spawn(scene, device, physics, m_modelDir,
                            at(s,  4.0f, kEnemyYOff, -4.0f), tuningFor(EnemyType::BlueSynth));
        p.meleeCount  = 3;   // 2 DominionTrooper + 1 Verthani (Corrupted Janitors)
        p.rangedCount = 1;   // 1 BlueSynth (Disposal Drone)
        p.bossCount   = 0;
        p.totalCount  = p.meleeCount + p.rangedCount + p.bossCount;

        // The standing HAZARD volume: a toxic incinerator zone in the -X half of the
        // plate. A simple host-evaluated AABB (see tick()). Present-but-inert until the
        // descent opens (m_descentOpen gates hazardActive()).
        m_hazardPresent = true;
        m_hazardMin = x3::phys::Vec3{ b1.x0 + 2.0f, p.baseY - 0.5f, -b1.zHalf + 1.0f };
        m_hazardMax = x3::phys::Vec3{ b1.x0 + 8.0f, p.baseY + 3.0f,  b1.zHalf - 1.0f };
        p.hasHazard = true;

        // Incinerator-override keypad door: locked, code 8100. Partition at x=8.
        DoorSpec d; d.doorwayCenter = at(s, 8.0f, 0.0f, 0.0f); d.axis = DoorAxis::AlongZ;
        d.withButton = false; d.locked = true; d.code = 8100;   // incinerator override
        d.tint[0]=0.55f; d.tint[1]=0.40f; d.tint[2]=0.30f;      // grimy disposal brown
        buildLevelDoor(scene, m_doors, device, physics, d);
        p.doorCode = 8100;

        // Hub trigger where the rider arrives (hazard/encounter alarm hook). Registered up
        // front but only DISPATCHED post-descent (onTrigger ignores ids while closed); the
        // host won't reach this volume until the hidden lift drops the player here anyway.
        triggers.add(x3::phys::Vec3{ b1.x1 - 8.0f, p.baseY,        -6.0f },
                     x3::phys::Vec3{ b1.x1,         p.baseY + 3.0f,  6.0f },
                     (uint32_t)SpireSubTrigger::SL1Hub, true);
    }

    // ===================================================================
    // SUB-LEVEL 2 — CRYOGENIC STORAGE (WORLD_STRUCTURE §3b). Hundreds in stasis; the
    // mini-boss THE FROZEN COLLECTIVE (merged cryo-subjects) anchors the storage hall.
    // Configured locally as a single-body Boss (frozenCollectiveTuning). A light cryo
    // escort (2 melee Verthani thralls + 1 ranged Illuminated overseer = 3) so the floor
    // is the mini-boss's, not a dogpile. A keypad door (cryo-control, code 8200) gates the
    // stasis-pod control room.
    // ===================================================================
    {
        const uint32_t si = (uint32_t)SpireSubLevel::SL2;
        const SpireSubLevel s = SpireSubLevel::SL2;
        SpireSubPlan& p = m_plan[si];
        p.sub      = s;
        p.baseY    = subBaseY(s);
        p.arrival  = at(s, 17.5f, 0.05f, 0.0f);

        // The Frozen Collective mini-boss anchors the hall center (off the lift spine).
        m_miniBoss.spawn(scene, device, physics, m_modelDir,
                         at(s, 9.0f, kEnemyYOff, 0.0f), frozenCollectiveTuning(m_modelDir));
        p.bossCount = 1;

        // A small cryo-thrall escort: two Verthani (frozen subjects) press melee + one
        // Illuminated holds a ranged standoff.
        m_enemies[si].spawn(scene, device, physics, m_modelDir,
                            at(s, 13.0f, kEnemyYOff, -3.0f), tuningFor(EnemyType::Verthani));
        m_enemies[si].spawn(scene, device, physics, m_modelDir,
                            at(s, 13.0f, kEnemyYOff,  3.0f), tuningFor(EnemyType::Verthani));
        m_enemies[si].spawn(scene, device, physics, m_modelDir,
                            at(s,  4.0f, kEnemyYOff,  4.0f), tuningFor(EnemyType::Illuminated));
        p.meleeCount  = 2;   // 2 Verthani cryo-thralls
        p.rangedCount = 1;   // 1 Illuminated cryo-overseer
        p.totalCount  = p.meleeCount + p.rangedCount + p.bossCount;   // 4 combatants (3 escort + boss)
        p.hasMiniBoss = true;

        // Cryo-control keypad door: locked, code 8200. Partition at x=8.
        DoorSpec d; d.doorwayCenter = at(s, 8.0f, 0.0f, 0.0f); d.axis = DoorAxis::AlongZ;
        d.withButton = false; d.locked = true; d.code = 8200;   // cryo-control code
        d.tint[0]=0.50f; d.tint[1]=0.72f; d.tint[2]=0.90f;      // frosted cyan
        buildLevelDoor(scene, m_doors, device, physics, d);
        p.doorCode = 8200;

        triggers.add(x3::phys::Vec3{ b1.x1 - 8.0f, p.baseY,        -6.0f },
                     x3::phys::Vec3{ b1.x1,         p.baseY + 3.0f,  6.0f },
                     (uint32_t)SpireSubTrigger::SL2Hub, true);
    }

    // ===================================================================
    // SUB-LEVEL 3 — ENHANCED INTERROGATION (WORLD_STRUCTURE §3b). Dr. Chen's torture
    // chamber: a regeneration device forcing endless pain. A light guard detail (1 melee
    // DominionTrooper jailer + 2 ranged BlueSynth interrogation drones = 3) so the floor's
    // beat is the RESCUE, not a fight. Dr. CHEN is held CAPTIVE — a rescue (E in range,
    // mirrors RescueSystem gating) that FREES him: the Return-Mission payoff. His clock is
    // GATED on the SL3 hub (m_sl3HubReached, default FALSE) AND on the descent being open,
    // so it can never expire at load. A keypad door (interrogation lockdown, code 8300)
    // gates the chamber.
    // ===================================================================
    {
        const uint32_t si = (uint32_t)SpireSubLevel::SL3;
        const SpireSubLevel s = SpireSubLevel::SL3;
        SpireSubPlan& p = m_plan[si];
        p.sub      = s;
        p.baseY    = subBaseY(s);
        p.arrival  = at(s, 17.5f, 0.05f, 0.0f);

        m_enemies[si].spawn(scene, device, physics, m_modelDir,
                            at(s, 12.0f, kEnemyYOff,  0.0f), tuningFor(EnemyType::DominionTrooper));
        m_enemies[si].spawn(scene, device, physics, m_modelDir,
                            at(s,  9.0f, kEnemyYOff, -4.0f), tuningFor(EnemyType::BlueSynth));
        m_enemies[si].spawn(scene, device, physics, m_modelDir,
                            at(s,  9.0f, kEnemyYOff,  4.0f), tuningFor(EnemyType::BlueSynth));
        p.meleeCount  = 1;   // 1 DominionTrooper jailer
        p.rangedCount = 2;   // 2 BlueSynth interrogation drones
        p.bossCount   = 0;
        p.totalCount  = p.meleeCount + p.rangedCount + p.bossCount;

        // Interrogation-lockdown keypad door: locked, code 8300. Partition at x=8.
        DoorSpec d; d.doorwayCenter = at(s, 8.0f, 0.0f, 0.0f); d.axis = DoorAxis::AlongZ;
        d.withButton = false; d.locked = true; d.code = 8300;   // interrogation lockdown
        d.tint[0]=0.45f; d.tint[1]=0.30f; d.tint[2]=0.35f;      // grim interrogation maroon
        buildLevelDoor(scene, m_doors, device, physics, d);
        p.doorCode = 8300;

        // ---- Dr. CHEN captive (the Return-Mission target). Held strapped to the
        // regeneration device in the -Z corner. The timer is GATED on the SL3 hub being
        // reached (m_sl3HubReached, default FALSE) AND on the descent being open, so it
        // cannot start at load. Reuses the AnnaCasual rigged mesh (an existing asset);
        // box fallback if absent. He carries chenLostBossTuning for API symmetry (only
        // reached if his clock expires — the Return is to free him before then). ----
        const x3::phys::Vec3 chenPos = at(s, 3.0f, kEnemyYOff, -5.5f);  // -Z interrogation corner
        m_chen = std::make_unique<RescueVictim>();
        m_chen->build(scene, device, physics, m_modelDir, chenPos,
                      VictimId::Emily, "Dr. Chen", "AnnaCasual.glb",
                      kRescueTimer, chenLostBossTuning(m_modelDir));
        p.hasCaptive = true;

        triggers.add(x3::phys::Vec3{ b1.x1 - 8.0f, p.baseY,        -6.0f },
                     x3::phys::Vec3{ b1.x1,         p.baseY + 3.0f,  6.0f },
                     (uint32_t)SpireSubTrigger::SL3Hub, true);
    }

    // The hidden-lift DESCENT trigger volume — at B1's elevator/spine arrival, sized to
    // catch a player who steps into the hidden lift behind the executive desk. We REMEMBER
    // its bounds here but DO NOT register it yet: it is added to the host's TriggerSystem
    // only in openDescent(), so it can never fire while the descent is hidden.
    m_descentVolMin = x3::phys::Vec3{ b1.x1 - 6.0f, b1BaseY - 1.0f, -4.0f };
    m_descentVolMax = x3::phys::Vec3{ b1.x1,        b1BaseY + 3.0f,  4.0f };

    // Try to load the shared real-door GLB so the keypad doors render as meshes (the
    // collision box stays). No-op/harmless if the GLB is absent (graybox fallback).
    m_doors.loadDoorMesh(device, convertedDir());

    m_built = true;
    x3::logInfo("SpireSubLevels::build complete (HIDDEN/inert) — SL1 Waste Disposal "
                "(4 enemies + hazard, code 8100), SL2 Cryo Storage (3 escort + 'The Frozen "
                "Collective' mini-boss, code 8200), SL3 Enhanced Interrogation (3 guards + "
                "Dr. Chen captive [gated], code 8300). Descent NOT armed until openDescent().");
}

bool SpireSubLevels::openDescent(bool cloneFallen, bool sarahSaved) {
    if (!m_built) return false;
    if (m_descentOpen) return false;           // idempotent / one-way
    // THE GATE: both the F7 Clone must have fallen AND Sarah must be saved. With either
    // false this is a no-op and the descent stays hidden (at load both are false).
    if (!(cloneFallen && sarahSaved)) return false;

    m_descentOpen = true;
    // Arm the hidden lift behind the executive desk NOW (it was never registered before).
    if (m_triggers)
        m_triggers->add(m_descentVolMin, m_descentVolMax,
                        (uint32_t)SpireSubTrigger::Descent, true);
    x3::logInfo("SpireSubLevels: HIDDEN DESCENT OPENED — the lift behind the executive "
                "desk activates (Clone fallen + Sarah saved). The Return Mission begins: "
                "descend SL1 Waste Disposal -> SL2 Cryo (Frozen Collective) -> SL3 to free Dr. Chen.");
    return true;
}

void SpireSubLevels::tick(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics,
                          const x3::phys::Vec3& eye, const x3::phys::Vec3& playerPos,
                          IDamageSink* player, const AttackFxFn& attackFx) {
    if (!m_built) return;
    // HIDDEN/INERT until the descent is opened: nothing in the sub-levels moves, the
    // hazard does not bite, and Chen's clock cannot run. This is the load-time state.
    if (!m_descentOpen) return;

    // Keypad doors animate (only meaningful once the player is down here).
    m_doors.update(dt, scene, physics);

    // Enemies + the mini-boss attack only while the player is alive (matches the others).
    IDamageSink* atkTarget = (player && player->isAlive()) ? player : nullptr;
    for (uint32_t i = 0; i < (uint32_t)SpireSubLevel::Count; ++i)
        m_enemies[i].update(dt, scene, physics, eye, atkTarget, attackFx);
    m_miniBoss.update(dt, scene, physics, eye, atkTarget, attackFx);

    // SL1 standing HAZARD: drain HP while the player stands in the toxic incinerator zone,
    // on a fixed cadence (so it can't melt the player every frame). Only when a live player
    // is provided and inside the volume.
    if (m_hazardPresent && atkTarget) {
        if (pointInBox(playerPos, m_hazardMin, m_hazardMax)) {
            m_hazardTick += dt;
            while (m_hazardTick >= kSubHazardInterval) {
                m_hazardTick -= kSubHazardInterval;
                atkTarget->takeDamage(kSubHazardDamage);
            }
        } else {
            m_hazardTick = 0.0f;   // reset cadence when out of the hazard
        }
    }

    // SL3 Dr. Chen captive: tick the timer (gated on m_sl3HubReached) + companion follow,
    // and spawn the fallback mini-boss the FRAME the timer expires (mirrors the F5/F7
    // captives). In normal play the Return is to FREE him before this.
    if (m_chen) {
        const bool expiredNow =
            m_chen->tick(dt, m_sl3HubReached, scene, physics, playerPos);
        if (expiredNow && m_device) {
            const x3::phys::Vec3 bossAt{ m_chen->pos().x, kEnemyYOff, m_chen->pos().z };
            m_chenBoss.spawn(scene, *m_device, physics, m_modelDir, bossAt,
                             m_chen->bossTuning());
            x3::logInfo("[sublevels] SL3 Dr. Chen lost to the corruption — interrogation mini-boss spawned");
        }
    }
    m_chenBoss.update(dt, scene, physics, eye, atkTarget, attackFx);
}

void SpireSubLevels::onTrigger(uint32_t triggerId) {
    // Defensive: ignore ALL sub-level triggers while the descent is hidden (the descent
    // trigger isn't even registered yet, and the hub triggers are unreachable).
    if (!m_descentOpen) return;
    switch ((SpireSubTrigger)triggerId) {
        case SpireSubTrigger::Descent:
            if (!m_descentReached) {
                m_descentReached = true;
                x3::logInfo("SubLevels: hidden lift used — descending into the Floor-7 sub-levels");
            }
            break;
        case SpireSubTrigger::SL1Hub:
            if (!m_sl1HubReached) {
                m_sl1HubReached = true;
                x3::logInfo("SubLevels: SL1 WASTE DISPOSAL hub reached — hazard + Corrupted Janitors armed");
            }
            break;
        case SpireSubTrigger::SL2Hub:
            if (!m_sl2HubReached) {
                m_sl2HubReached = true;
                x3::logInfo("SubLevels: SL2 CRYOGENIC STORAGE hub reached — 'The Frozen Collective' engages");
            }
            break;
        case SpireSubTrigger::SL3Hub:
            // The player reached the interrogation chamber — START Chen's rescue clock NOW
            // (not at load). Idempotent.
            if (!m_sl3HubReached) {
                m_sl3HubReached = true;
                x3::logInfo("SubLevels: SL3 ENHANCED INTERROGATION hub reached — Dr. Chen's rescue clock started");
            }
            break;
    }
}

bool SpireSubLevels::onRescue(const x3::phys::Vec3& playerPos, float range) {
    if (!m_descentOpen || !m_chen) return false;
    return m_chen->tryRescue(playerPos, range);
}

FireResult SpireSubLevels::onFire(const x3::phys::Vec3& eye, const x3::phys::Vec3& dir,
                                  Scene& scene, x3::phys::IPhysicsWorld& physics,
                                  int damage) {
    FireResult r;
    if (!m_descentOpen) return r;     // the sub-levels aren't in play yet — a clean miss
    for (uint32_t i = 0; i < (uint32_t)SpireSubLevel::Count; ++i) {
        FireResult ri = m_enemies[i].fire(eye, dir, scene, physics, damage);
        if (ri.hitMonster) return ri;          // a live enemy took it — done
        if (!r.hit && ri.hit) r = ri;          // remember the nearest geometry hit
    }
    // The Frozen Collective mini-boss.
    FireResult rboss = m_miniBoss.fire(eye, dir, scene, physics, damage);
    if (rboss.hitMonster) return rboss;
    if (!r.hit && rboss.hit) r = rboss;
    // The (rare) Chen-lost mini-boss, if any.
    FireResult rb = m_chenBoss.fire(eye, dir, scene, physics, damage);
    if (rb.hitMonster) return rb;
    if (!r.hit && rb.hit) r = rb;
    return r;
}

void SpireSubLevels::draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                          const Scene& scene) const {
    if (!m_descentOpen) return;       // hidden — nothing to draw until the descent opens
    for (uint32_t i = 0; i < (uint32_t)SpireSubLevel::Count; ++i)
        m_enemies[i].drawAll(device, frame, scene);
    m_miniBoss.drawAll(device, frame, scene);
    if (m_chen) m_chen->draw(device, frame, scene);
    m_chenBoss.drawAll(device, frame, scene);
}

bool SpireSubLevels::nearLockedCodedDoor(const x3::phys::Vec3& playerPos, float range) const {
    if (!m_descentOpen) return false;
    const float r2 = range * range;
    for (uint32_t i = 0; i < m_doors.count(); ++i) {
        const Door& d = m_doors.at(i);
        if (!d.locked || d.code == 0) continue;
        // 3D distance: the sub-level doors STACK at the same XZ on the vertical tower
        // (SL1/SL2/SL3 doors share x=8, differing only in Y, BELOW B1), so Y must
        // distinguish them — an XZ-only test would treat all three as the same spot.
        // (Mirrors the spire_mid/spire_top stacked-door fix.)
        const float dx = playerPos.x - d.closedPos.x;
        const float dy = playerPos.y - d.closedPos.y;
        const float dz = playerPos.z - d.closedPos.z;
        if (dx * dx + dy * dy + dz * dz <= r2) return true;
    }
    return false;
}

bool SpireSubLevels::tryDoorCode(const x3::phys::Vec3& playerPos, int code, float range) {
    if (!m_descentOpen) return false;
    const float r2 = range * range;
    int best = -1; float bestD2 = r2;
    for (uint32_t i = 0; i < m_doors.count(); ++i) {
        const Door& d = m_doors.at(i);
        if (!d.locked || d.code == 0) continue;
        // 3D distance (include Y) so the nearest LOCKED coded door is the one on the
        // player's CURRENT sub-level, not a stacked-floor tie.
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

bool SpireSubLevels::chenCaptive() const {
    return m_chen && m_chen->captive();
}
bool SpireSubLevels::chenRescued() const {
    return m_chen && m_chen->companion();
}
float SpireSubLevels::chenTimeLeft() const {
    return m_chen ? m_chen->timeLeft() : 0.0f;
}

// ===========================================================================
// Headless self-test (--test-sublevels).
// ===========================================================================
namespace {

int g_pass = 0, g_fail = 0;
void check(bool cond, const char* name) {
    if (cond) { ++g_pass; x3::logInfo(std::string("[sublevels-test] PASS ") + name); }
    else      { ++g_fail; x3::logError(std::string("[sublevels-test] FAIL ") + name); }
}

constexpr float kFixedDt = 1.0f / 60.0f;

using HeadlessDevice = x3::game::HeadlessRenderDevice;

// Count melee (Guard-archetype, non-ranged) vs ranged (ranged==true) enemies in a manager.
void roleSplit(const MonsterManager& m, uint32_t& melee, uint32_t& ranged) {
    melee = ranged = 0;
    for (uint32_t i = 0; i < m.count(); ++i) {
        if (m.at(i).ranged()) ++ranged; else ++melee;
    }
}

// A minimal IDamageSink so the hazard test can observe HP drain headlessly (the real
// Player needs a physics body / camera; we only need the takeDamage/isAlive contract).
struct TestSink : public IDamageSink {
    int hp = 100;
    x3::phys::Vec3 p{};
    bool takeDamage(int amount) override {
        if (hp <= 0) return false;
        hp -= amount; if (hp < 0) hp = 0; return hp <= 0;
    }
    x3::phys::Vec3 damageTargetPos() const override { return p; }
    bool isAlive() const override { return hp > 0; }
};

} // namespace

bool runSubLevelsSelfTest() {
    g_pass = g_fail = 0;

    std::unique_ptr<x3::phys::IPhysicsWorld> physics(x3::phys::createPhysicsWorld());
    physics->init();

    HeadlessDevice device;
    Scene scene;

    // Build the Spire geometry (gives floorBaseY[] + the plate footprints), then the top
    // floors (so we can derive the F7-complete gate from spire_top's PUBLIC queries the
    // way the host does), then the sub-levels under B1. Share a TriggerSystem like the host.
    Level1Layout layout = buildLevel1(scene, device, *physics);
    TriggerSystem triggers;
    const std::string rigged = riggedGlbRoot();

    SpireTopFloors top;
    top.build(scene, device, *physics, layout, triggers, rigged);

    SpireSubLevels sub;
    sub.build(scene, device, *physics, layout, triggers, rigged);

    check(sub.built(), "S0 sub-levels built");

    // ---- HIDDEN at load: the descent is closed + inert. ----
    {
        bool hidden = !sub.descentOpen() &&
                      sub.hazardPresent() && !sub.hazardActive() &&
                      sub.chenPresent() && sub.chenCaptive() && !sub.chenRescued() &&
                      !sub.chenTimerRunning();
        // The descent trigger must NOT be registered at load (only the 3 hub triggers from
        // this module, plus the top floors' triggers). Confirm no Descent id exists yet.
        bool noDescentTrigger = (triggers.findById((uint32_t)SpireSubTrigger::Descent) == nullptr);
        check(hidden && noDescentTrigger,
              "S1 descent HIDDEN at load (closed, hazard inert, Chen captive, no descent trigger armed)");
    }

    // ---- Inert ticking at load: many frames change nothing; Chen never starts/ends. ----
    {
        TestSink sink; sink.hp = 100;
        // Stand the (would-be) player INSIDE the SL1 hazard footprint — at load it must NOT
        // bite (the descent is closed). The hazard spans the -X half of the SL1 plate.
        const float hazY = sub.plan(SpireSubLevel::SL1).baseY;
        x3::phys::Vec3 inHazard{ level1Rooms()[(uint32_t)L1Floor::B1].x0 + 4.0f, hazY + 0.5f, 0.0f };
        sink.p = inHazard;
        for (int i = 0; i < 120; ++i) {
            sub.tick(kFixedDt, scene, *physics, inHazard, inHazard, &sink, AttackFxFn{});
            physics->step(kFixedDt);
            scene.update(*physics);
        }
        bool noHazardDamage = (sink.hp == 100);
        bool chenStillCaptive = sub.chenCaptive() && !sub.chenTimerRunning();
        check(noHazardDamage && chenStillCaptive,
              "S2 closed descent is INERT (no hazard damage, Chen clock never starts)");
    }

    // ---- The GATE: openDescent() is a NO-OP unless BOTH conditions hold. ----
    {
        bool n1 = !sub.openDescent(false, false) && !sub.descentOpen();   // neither
        bool n2 = !sub.openDescent(true,  false) && !sub.descentOpen();   // clone only
        bool n3 = !sub.openDescent(false, true ) && !sub.descentOpen();   // sarah only
        check(n1 && n2 && n3,
              "S3 openDescent GATED — no-op unless (Clone fallen AND Sarah saved)");
    }

    // ---- Derive the gate from spire_top's PUBLIC queries the way the host does, and
    // confirm that at the F7-complete state the gate opens (and arms the descent). At load
    // the Clone is alive + Sarah captive, so the host-side gate is naturally closed. ----
    {
        // Host-side gate inputs (read-only from spire_top): Clone fallen == boss all dead;
        // Sarah saved == she is present + no longer a captive (rescued, not expired). At
        // load: boss alive, Sarah captive => gate closed.
        bool cloneFallenAtLoad = (top.boss().aliveCount() == 0);
        bool sarahSavedAtLoad   = top.victimPresent() && !top.victimCaptive();
        bool gateClosedAtLoad   = !(cloneFallenAtLoad && sarahSavedAtLoad);
        check(gateClosedAtLoad && !sub.descentOpen(),
              "S4 host-derived F7 gate is CLOSED at load (Clone alive + Sarah captive)");

        // Now satisfy the gate (the post-F7 state) and open the descent.
        bool opened = sub.openDescent(/*cloneFallen*/true, /*sarahSaved*/true);
        bool armed  = (triggers.findById((uint32_t)SpireSubTrigger::Descent) != nullptr);
        check(opened && sub.descentOpen() && armed,
              "S5 gate satisfied -> descent OPENS + hidden-lift trigger ARMS");

        // Idempotent: a second open is a no-op (still open).
        check(!sub.openDescent(true, true) && sub.descentOpen(),
              "S6 openDescent is idempotent (second call no-op)");
    }

    // ---- Once OPEN: SL1/SL2/SL3 built with expected counts + role split. ----
    {
        const SpireSubPlan& s1 = sub.plan(SpireSubLevel::SL1);
        uint32_t m1=0, r1=0; roleSplit(sub.enemies(SpireSubLevel::SL1), m1, r1);
        check(sub.enemies(SpireSubLevel::SL1).count() == 4 && s1.totalCount == 4 &&
              m1 == 3 && r1 == 1 && s1.meleeCount == 3 && s1.rangedCount == 1 &&
              s1.hasHazard,
              "S7 SL1 Waste Disposal = 4 enemies (3 melee + 1 ranged) + hazard");

        const SpireSubPlan& s2 = sub.plan(SpireSubLevel::SL2);
        uint32_t m2=0, r2=0; roleSplit(sub.enemies(SpireSubLevel::SL2), m2, r2);
        check(sub.enemies(SpireSubLevel::SL2).count() == 3 &&
              m2 == 2 && r2 == 1 && s2.meleeCount == 2 && s2.rangedCount == 1 &&
              s2.bossCount == 1 && s2.hasMiniBoss && s2.totalCount == 4,
              "S8 SL2 Cryo Storage = 3 escort (2 melee + 1 ranged) + 1 mini-boss");

        const SpireSubPlan& s3 = sub.plan(SpireSubLevel::SL3);
        uint32_t m3=0, r3=0; roleSplit(sub.enemies(SpireSubLevel::SL3), m3, r3);
        check(sub.enemies(SpireSubLevel::SL3).count() == 3 &&
              m3 == 1 && r3 == 2 && s3.meleeCount == 1 && s3.rangedCount == 2 &&
              s3.hasCaptive,
              "S9 SL3 Enhanced Interrogation = 3 guards (1 melee + 2 ranged) + Dr. Chen captive");
    }

    // ---- The Frozen Collective mini-boss is a live Boss-type leader. ----
    {
        bool ok = sub.plan(SpireSubLevel::SL2).hasMiniBoss &&
                  sub.miniBoss().count() == 1 &&
                  sub.miniBoss().at(0).type() == MonsterType::Boss &&
                  sub.miniBoss().at(0).alive() &&
                  sub.miniBoss().aliveCount() == 1;
        check(ok, "S10 SL2 'The Frozen Collective' is a live Boss-type mini-boss");
    }

    // ---- Dr. Chen present-but-captive (not active/rescued) after the descent opens. ----
    {
        check(sub.chenPresent() && sub.chenCaptive() && !sub.chenRescued() &&
              !sub.chenTimerRunning(),
              "S11 Dr. Chen present + captive + clock NOT running right after open");
    }

    // ---- The SL1 hazard now BITES a player standing in it (descent is open). ----
    {
        TestSink sink; sink.hp = 100;
        const float hazY = sub.plan(SpireSubLevel::SL1).baseY;
        x3::phys::Vec3 inHazard{ level1Rooms()[(uint32_t)L1Floor::B1].x0 + 4.0f, hazY + 0.5f, 0.0f };
        sink.p = inHazard;
        for (int i = 0; i < 120; ++i)      // 2 s -> several hazard ticks
            sub.tick(kFixedDt, scene, *physics, inHazard, inHazard, &sink, AttackFxFn{});
        bool drained = sink.hp < 100 && sink.isAlive();   // lost HP but not instakilled
        bool hazardActive = sub.hazardActive();
        bool insideVol = sub.inHazardVolume(inHazard);     // the drained spot IS in the volume
        check(drained && hazardActive && insideVol,
              "S12 SL1 hazard drains a player standing in it once the descent is open");

        // The hazard VOLUME is BOUNDED: a point OUTSIDE the incinerator footprint is not
        // "in hazard" (so it takes no hazard damage). Tested via the boundary query so the
        // enemies-chasing-a-live-target confound can't muddy the assertion.
        x3::phys::Vec3 outside{ level1Rooms()[(uint32_t)L1Floor::B1].x1 - 2.0f, hazY + 0.5f, 0.0f };
        check(!sub.inHazardVolume(outside),
              "S13 the hazard volume is BOUNDED — a point outside the incinerator is not in hazard");
    }

    // ---- The SL3 hub starts Chen's clock; the rescue (E in range) FREES him. ----
    {
        // Before the hub: still captive, clock stopped.
        bool before = sub.chenCaptive() && !sub.chenTimerRunning();

        sub.onTrigger((uint32_t)SpireSubTrigger::SL3Hub);
        bool clockRunning = sub.chenTimerRunning();
        float t0 = sub.chenTimeLeft();
        sub.tick(kFixedDt, scene, *physics, layout.spawn, layout.spawn, nullptr, AttackFxFn{});
        bool countsDown = sub.chenTimeLeft() < t0;
        check(before && clockRunning && countsDown,
              "S14 SL3 hub starts Dr. Chen's clock (counts down after)");

        // The rescue: out of range fails; in range frees Chen (companion).
        const SpireSubPlan& s3 = sub.plan(SpireSubLevel::SL3);
        x3::phys::Vec3 farPos{ s3.arrival.x, s3.baseY + 0.5f, s3.arrival.z };  // arrival is far from the cell
        bool farFails = !sub.onRescue(farPos, kRescueReach);
        // Chen's cell is at (x=3.0, baseY, -5.5) — the SL3 build places him with
        // at(s, 3.0f, kEnemyYOff, -5.5f) (an ABSOLUTE plate X, not B1.x0-relative).
        // Walk right up to him. (FLOOR-1 RELAY: was B1.x0+3, which only matched when the
        // old placeholder B1.x0 was 0; the grown plate moved B1.x0 to -24, so pin the
        // literal build position.)
        x3::phys::Vec3 nearChen{ 3.0f, s3.baseY + kEnemyYOff, -5.5f };
        bool freed = sub.onRescue(nearChen, kRescueReach);
        check(farFails && freed && sub.chenRescued() && !sub.chenCaptive(),
              "S15 Dr. Chen rescue: out-of-range fails, in-range FREES him (Return-Mission payoff)");
    }

    // ---- Keypad doors: one per sub-level, LOCKED with the authored code; right code in
    // range unlocks, wrong code does not. ----
    {
        check(sub.doors().count() == 3, "S16 three sub-level keypad doors built");

        bool allLockedCoded = true;
        for (uint32_t i = 0; i < sub.doors().count(); ++i) {
            const Door& d = sub.doors().at(i);
            if (!d.locked || d.code == 0) allLockedCoded = false;
        }
        bool planCodes = sub.plan(SpireSubLevel::SL1).doorCode == 8100 &&
                         sub.plan(SpireSubLevel::SL2).doorCode == 8200 &&
                         sub.plan(SpireSubLevel::SL3).doorCode == 8300;
        check(allLockedCoded && planCodes,
              "S17 all three doors LOCKED with the authored codes (8100/8200/8300)");

        // Right code at the SL1 door opens it; wrong code does not. Probe at its 3D closed
        // position (the stacked-tower fix means Y matters).
        const Door& d1 = sub.doors().at(0);   // SL1 built first
        x3::phys::Vec3 atD1{ d1.closedPos.x, d1.closedPos.y + 1.0f, d1.closedPos.z };
        bool wrongRejected = !sub.tryDoorCode(atD1, 9999);
        bool stillLocked   = sub.doors().at(0).locked;
        bool rightOpens    = sub.tryDoorCode(atD1, 8100);
        check(wrongRejected && stillLocked && rightOpens,
              "S18 keypad: wrong code rejected, right code (8100) opens the SL1 door");
    }

    physics->shutdown();
    x3::logInfo(std::string("sublevels: ") + std::to_string(g_pass) + "/" +
                std::to_string(g_pass + g_fail) + " passed");
    return g_fail == 0;
}

} // namespace x3::game
