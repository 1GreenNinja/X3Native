#include "companion_squad.h"
#include "headless_device.h"
#include "engine/core/x3_log.h"
#include <cmath>
#include <string>

namespace x3::game {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static float squadLen2(float dx, float dz) { return std::sqrt(dx * dx + dz * dz); }

// ---------------------------------------------------------------------------
// CompanionSquad::addCompanion
// ---------------------------------------------------------------------------

uint32_t CompanionSquad::addCompanion(x3::phys::IPhysicsWorld& physics,
                                      float x, float y, float z,
                                      float startYaw) {
    if (m_slots.size() >= kSquadMaxSize) return ~0u;
    m_slots.emplace_back();
    const uint32_t idx = (uint32_t)m_slots.size() - 1u;
    CompanionSlot& s = m_slots[idx];
    s.player.spawn(physics, x, y, z);
    s.player.setLook(startYaw, 0.0f);
    s.spawned = true;
    return idx;
}

// ---------------------------------------------------------------------------
// CompanionSquad::shutdown
// ---------------------------------------------------------------------------

void CompanionSquad::shutdown(x3::phys::IPhysicsWorld&) {
    // Character physics bodies are removed when the physics world shuts down.
    // Clear slot list so the squad is logically empty.
    m_slots.clear();
}

// ---------------------------------------------------------------------------
// CompanionSquad::buildContext
// ---------------------------------------------------------------------------

CompanionContext CompanionSquad::buildContext(uint32_t i,
                                             const x3::phys::Vec3& playerPos,
                                             float /*playerHpFrac*/,
                                             bool  playerDowned,
                                             MonsterSystem* const* threats,
                                             uint32_t threatCount) const {
    CompanionContext ctx;
    const CompanionSlot& s = m_slots[i];

    // selfPos: feet + eye-height approximation.
    const x3::phys::Vec3 fp = s.player.feet();
    ctx.selfPos   = { fp.x, fp.y + 1.6f, fp.z };
    ctx.playerPos = playerPos;
    ctx.selfHpFrac = s.player.isAlive()
                         ? (float)s.player.hp() / (float)s.player.maxHp()
                         : 0.0f;
    ctx.ammoInMag  = s.ammoInMag;
    ctx.playerDowned = playerDowned;

    // anyAllyDowned: true if any OTHER slot is downed, or the player is downed.
    ctx.anyAllyDowned = playerDowned;
    ctx.downedAllyPos = playerPos;
    for (uint32_t j = 0; j < (uint32_t)m_slots.size(); ++j) {
        if (j == i) continue;
        if (m_slots[j].downed == DownedState::Downed) {
            ctx.anyAllyDowned = true;
            const x3::phys::Vec3 ap = m_slots[j].player.feet();
            ctx.downedAllyPos = { ap.x, ap.y + 0.5f, ap.z };
            break;  // closest downed ally (first found in v1)
        }
    }

    // Build threat scratch array (stack-local, max 16).
    // See header note: sequential per-companion processing makes this safe in v1.
    static thread_local CompanionThreat tBuf[16];
    int tc = 0;
    for (uint32_t t = 0; t < threatCount && tc < 16; ++t) {
        if (!threats[t] || !threats[t]->alive()) continue;
        const x3::phys::Vec3 tp = threats[t]->pos();
        const float dx = tp.x - ctx.selfPos.x;
        const float dy = tp.y - ctx.selfPos.y;
        const float dz = tp.z - ctx.selfPos.z;
        const float dist     = std::sqrt(dx * dx + dy * dy + dz * dz);
        const float pdx      = tp.x - playerPos.x;
        const float pdz      = tp.z - playerPos.z;
        const float toPlayer = std::sqrt(pdx * pdx + pdz * pdz);
        tBuf[tc].pos       = tp;
        tBuf[tc].dist      = dist;
        tBuf[tc].toPlayer  = toPlayer;
        tBuf[tc].losToSelf = true;   // LOS deferred to v2 (rayCast would go here)
        ++tc;
    }
    ctx.threats     = (tc > 0) ? tBuf : nullptr;
    ctx.threatCount = tc;

    ctx.nearCover          = false;  // cover system deferred
    ctx.inPlayerLineOfFire = false;
    return ctx;
}

// ---------------------------------------------------------------------------
// CompanionSquad::commandToInput
// ---------------------------------------------------------------------------

PlayerInput CompanionSquad::commandToInput(const CompanionCommand& cmd,
                                           float /*selfYaw*/, float targetYaw) {
    PlayerInput in;
    in.moveFwd    = cmd.moveFwd;
    in.moveStrafe = cmd.moveStrafe;
    in.sprint     = cmd.sprint;
    in.jumpPressed = cmd.jumpPressed;

    // Convert the TARGET yaw into a lookDX delta so Player::update rotates the
    // companion's body toward the aim direction each frame (proportional nudge).
    // Player sensitivity ~= 0.0025 rad/px (from player.cpp kLookSens).
    // We don't know the exact current yaw here -- the caller sets it via setLook
    // before update(), so lookDX is zeroed (orientation handled by setLook directly).
    (void)targetYaw;
    in.lookDX = 0.0f;
    in.lookDY = 0.0f;
    return in;
}

// ---------------------------------------------------------------------------
// CompanionSquad::doCompanionFire
// ---------------------------------------------------------------------------

void CompanionSquad::doCompanionFire(uint32_t slotIdx,
                                     x3::phys::IPhysicsWorld& physics,
                                     Scene& scene,
                                     MonsterSystem* const* threats,
                                     uint32_t threatCount) {
    if (slotIdx >= m_slots.size()) return;
    const CompanionSlot& s = m_slots[slotIdx];
    float ex, ey, ez, yaw, pitch;
    s.player.camera(ex, ey, ez, yaw, pitch);

    const x3::phys::Vec3 eye{ ex, ey, ez };
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const float cy = std::cos(yaw),   sy = std::sin(yaw);
    const x3::phys::Vec3 dir{ cp * cy, sp, cp * sy };

    // Try all live threats with this ray -- fire() returns early for non-matching bodies.
    for (uint32_t t = 0; t < threatCount; ++t) {
        if (!threats[t] || !threats[t]->alive()) continue;
        threats[t]->fire(eye, dir, scene, physics);
    }
}

// ---------------------------------------------------------------------------
// CompanionSquad::tick
// ---------------------------------------------------------------------------

void CompanionSquad::tick(float dt,
                          x3::phys::IPhysicsWorld& physics,
                          const x3::phys::Vec3& playerPos,
                          float playerHpFrac,
                          bool  playerDowned,
                          MonsterSystem* const* threats,
                          uint32_t threatCount) {
    for (uint32_t i = 0; i < (uint32_t)m_slots.size(); ++i) {
        CompanionSlot& s = m_slots[i];
        if (!s.spawned) continue;

        // ---- Downed: stop acting; tick health timers only. ----
        if (s.downed == DownedState::Downed) {
            s.player.updateHealth(dt);
            continue;
        }

        // ---- Detect newly dead companion (HP just hit 0). ----
        if (!s.player.isAlive() && s.downed == DownedState::Alive) {
            s.downed = DownedState::Downed;
            x3::logInfo("[squad] companion " + std::to_string(i) + " went DOWNED");
            s.player.updateHealth(dt);
            continue;
        }

        // ---- Build context + tick brain. ----
        const CompanionContext ctx = buildContext(i, playerPos, playerHpFrac,
                                                  playerDowned, threats, threatCount);
        const CompanionCommand cmd = s.brain.tick(ctx);

        // ---- Revive: advance revive timer toward a nearby downed ally. ----
        if (cmd.chosen == CompanionBehavior::Revive && cmd.reviveAction) {
            // Find the nearest downed slot (may be the player -- for simplicity
            // we only revive other companion slots in v1; player respawns on its own).
            int target = -1;
            float best = 1e30f;
            for (uint32_t j = 0; j < (uint32_t)m_slots.size(); ++j) {
                if (j == i) continue;
                if (m_slots[j].downed != DownedState::Downed) continue;
                const x3::phys::Vec3 dp = m_slots[j].player.feet();
                const float dx = dp.x - ctx.selfPos.x;
                const float dz = dp.z - ctx.selfPos.z;
                const float d  = std::sqrt(dx * dx + dz * dz);
                if (d < best) { best = d; target = (int)j; }
            }
            if (target >= 0) {
                m_slots[target].reviveTimer += dt;
                if (m_slots[target].reviveTimer >= kSquadReviveTime) {
                    const int restoreHp = std::max(1,
                        (int)((float)m_slots[target].player.maxHp() * kSquadReviveHp));
                    m_slots[target].player.setHp(restoreHp);
                    m_slots[target].downed      = DownedState::Alive;
                    m_slots[target].reviveTimer = 0.0f;
                    x3::logInfo("[squad] companion " + std::to_string(i) +
                                " revived companion " + std::to_string(target));
                }
            }
        }

        // ---- Fire: raycast from companion's eye toward aim, damage monsters. ----
        if (cmd.fire && m_scene) {
            doCompanionFire(i, physics, *m_scene, threats, threatCount);
        }

        // ---- Determine the orientation the companion should face this frame. ----
        float camX, camY, camZ, selfYaw, selfPitch;
        s.player.camera(camX, camY, camZ, selfYaw, selfPitch);

        float targetYaw   = selfYaw;
        float targetPitch = 0.0f;

        switch (cmd.chosen) {
        case CompanionBehavior::Follow:
        case CompanionBehavior::Hold: {
            // Face the player.
            const float dx = playerPos.x - camX, dz = playerPos.z - camZ;
            if (squadLen2(dx, dz) > 0.5f) targetYaw = std::atan2(dz, dx);
            break;
        }
        case CompanionBehavior::Engage: {
            // Face the threat (brain's aimYaw).
            targetYaw   = cmd.aimYaw;
            targetPitch = cmd.aimPitch;
            break;
        }
        case CompanionBehavior::TakeCover: {
            // Face toward cover point (brain sets aimYaw toward cover in Slice A).
            targetYaw = cmd.aimYaw;
            break;
        }
        case CompanionBehavior::Retreat: {
            // Move away from threat: flip the aim yaw (brain aimed at the threat).
            targetYaw = cmd.aimYaw + 3.14159265f;
            break;
        }
        case CompanionBehavior::Revive: {
            // Face the downed ally.
            const float dx = ctx.downedAllyPos.x - camX;
            const float dz = ctx.downedAllyPos.z - camZ;
            if (squadLen2(dx, dz) > 0.3f) targetYaw = std::atan2(dz, dx);
            break;
        }
        case CompanionBehavior::Reload:
        default:
            break;
        }

        // Apply look direction directly (no lookDX integration needed; setLook
        // teleports the yaw/pitch so the companion instantly faces its target).
        s.player.setLook(targetYaw, targetPitch);

        // Build and apply the PlayerInput.
        const PlayerInput in = commandToInput(cmd, selfYaw, targetYaw);
        s.player.update(in, dt, physics);
    }
}

// ---------------------------------------------------------------------------
// runCompanionSquadSelfTest
// ---------------------------------------------------------------------------

bool runCompanionSquadSelfTest() {
    x3::logInfo("[companion-squad-test] running companion squad integration self-test...");
    int pass = 0, total = 0;

    // Build a minimal flat-arena physics world.
    std::unique_ptr<x3::phys::IPhysicsWorld> phys(x3::phys::createPhysicsWorld());
    if (!phys->init()) {
        x3::logError("[companion-squad-test] physics init failed");
        return false;
    }

    // Static ground plane at y=0 (mass=0 => static body).
    phys->addBox({ 50.0f, 0.5f, 50.0f }, { 0.0f, -0.5f, 0.0f },
                 0.0f, x3::phys::Layer::Static);

    // Headless render device + scene (needed by MonsterSystem::buildMonsterTuned).
    HeadlessRenderDevice dev;
    x3::rhi::DeviceDesc dsc;
    dsc.width = 64; dsc.height = 64; dsc.headless = true;
    dev.init(dsc);
    Scene scene;

    // Spawn squad (3 companions in a small triangle near the origin).
    CompanionSquad squad;
    squad.setScene(&scene);
    squad.addCompanion(*phys,  2.0f, 0.5f,  0.0f);   // slot 0
    squad.addCompanion(*phys, -2.0f, 0.5f,  0.0f);   // slot 1
    squad.addCompanion(*phys,  0.0f, 0.5f, -2.0f);   // slot 2

    // Spawn one enemy (guard tuning) at z=12 -- ~12 m in front of the squad.
    MonsterSystem enemy;
    {
        MonsterSystem::Tuning t;
        t.hp = 100; t.chaseSpeed = 0.0f;  // stationary so it doesn't interfere
        t.damage = 0;
        enemy.buildMonsterTuned(scene, dev, *phys, "",
                                { 0.0f, 0.5f, 12.0f }, t);
    }
    MonsterSystem* threats[1] = { &enemy };

    const x3::phys::Vec3 playerPos{ 0.0f, 1.6f, -5.0f };

    // Settle: let characters land on the floor.
    for (int i = 0; i < 10; ++i) {
        squad.tick(1.0f / 60.0f, *phys, playerPos, 1.0f, false, threats, 1);
        phys->step(1.0f / 60.0f);
        scene.update(*phys);
    }

    // ------------------------------------------------------------------
    // T1: With a visible threat 12m away, companion brain picks Engage.
    // ------------------------------------------------------------------
    {
        total++;
        const CompanionSlot& s0 = squad.slot(0);
        const x3::phys::Vec3 fp = s0.player.feet();
        CompanionContext ctx;
        ctx.selfPos    = { fp.x, fp.y + 1.6f, fp.z };
        ctx.playerPos  = playerPos;
        ctx.selfHpFrac = 1.0f;
        ctx.ammoInMag  = 30;
        CompanionThreat th;
        th.pos       = enemy.pos();
        th.dist      = 12.0f;
        th.losToSelf = true;
        ctx.threats     = &th;
        ctx.threatCount = 1;
        const CompanionCommand cmd = s0.brain.tick(ctx);
        const bool ok = (cmd.chosen == CompanionBehavior::Engage);
        if (ok) { pass++; x3::logInfo("[companion-squad-test] T1 PASS (Engage threat)"); }
        else x3::logError("[companion-squad-test] T1 FAIL (expected Engage, got " +
                          std::to_string((int)cmd.chosen) + ")");
    }

    // ------------------------------------------------------------------
    // T2: Zero companion 0's HP; one tick should set it to Downed.
    // ------------------------------------------------------------------
    {
        total++;
        squad.slot(0).player.setHp(0);
        // One tick for the squad machine to detect the death transition.
        squad.tick(1.0f / 60.0f, *phys, playerPos, 1.0f, false, threats, 1);
        const bool ok = (squad.slot(0).downed == DownedState::Downed);
        if (ok) { pass++; x3::logInfo("[companion-squad-test] T2 PASS (companion Downed)"); }
        else x3::logError("[companion-squad-test] T2 FAIL (expected Downed, got " +
                          std::to_string((int)squad.slot(0).downed) + ")");
    }

    // ------------------------------------------------------------------
    // T3: With slot 0 downed and no active threats, slot 1's brain should
    //     pick Revive + reviveAction when the downed ally is within kReviveRange.
    // ------------------------------------------------------------------
    {
        total++;
        // Place the downed ally 1.0m from slot 1 (inside kSquadReviveRange=1.5m).
        const x3::phys::Vec3 fp1 = squad.slot(1).player.feet();
        CompanionBrain brain;
        CompanionContext ctx;
        ctx.selfPos       = { fp1.x, fp1.y + 1.6f, fp1.z };
        ctx.playerPos     = playerPos;
        ctx.selfHpFrac    = 1.0f;
        ctx.ammoInMag     = 30;
        ctx.anyAllyDowned = true;
        // Downed ally 1.0m away (within kReviveRange).
        ctx.downedAllyPos = { fp1.x + 1.0f, fp1.y + 0.5f, fp1.z };
        ctx.threats     = nullptr;
        ctx.threatCount = 0;
        const CompanionCommand cmd = brain.tick(ctx);
        const bool ok = (cmd.chosen == CompanionBehavior::Revive) && cmd.reviveAction;
        if (ok) { pass++; x3::logInfo("[companion-squad-test] T3 PASS (Revive + reviveAction)"); }
        else x3::logError("[companion-squad-test] T3 FAIL (expected Revive+reviveAction, chose=" +
                          std::to_string((int)cmd.chosen) +
                          " reviveAction=" + std::to_string(cmd.reviveAction) + ")");
    }

    // ------------------------------------------------------------------
    // T4: Drive the squad revive timer until slot 0 is restored to Alive.
    //     Position slot 1 next to slot 0, run no threats, tick until done.
    // ------------------------------------------------------------------
    {
        total++;
        // Ensure slot 0 is Downed with a fresh revive timer.
        squad.slot(0).player.setHp(0);
        squad.slot(0).downed      = DownedState::Downed;
        squad.slot(0).reviveTimer = 0.0f;

        // Move slot 1 adjacent to slot 0 (within kSquadReviveRange).
        const x3::phys::Vec3 fp0 = squad.slot(0).player.feet();
        squad.slot(1).player.setFeetPosition(*phys,
            { fp0.x + 0.5f, fp0.y, fp0.z });

        // Tick long enough to complete the revive (kSquadReviveTime + margin).
        const int kMaxTicks = (int)(kSquadReviveTime * 60.0f) + 10;
        for (int i = 0; i < kMaxTicks; ++i) {
            squad.tick(1.0f / 60.0f, *phys, playerPos, 1.0f, false, nullptr, 0);
            phys->step(1.0f / 60.0f);
            scene.update(*phys);
            if (squad.slot(0).downed == DownedState::Alive) break;
        }
        const bool ok = (squad.slot(0).downed == DownedState::Alive) &&
                        (squad.slot(0).player.hp() > 0);
        if (ok) { pass++; x3::logInfo("[companion-squad-test] T4 PASS (revive completed, hp=" +
                          std::to_string(squad.slot(0).player.hp()) + ")"); }
        else x3::logError("[companion-squad-test] T4 FAIL (downed=" +
                          std::to_string((int)squad.slot(0).downed) +
                          " hp=" + std::to_string(squad.slot(0).player.hp()) + ")");
    }

    squad.shutdown(*phys);
    phys->shutdown();
    dev.shutdown();

    x3::logInfo("[companion-squad-test] " + std::to_string(pass) + "/" +
                std::to_string(total) + " passed");
    return pass == total;
}

} // namespace x3::game
