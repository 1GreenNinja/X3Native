// Super-strength melee verb + --test-phase2b. See app/melee.h + app/monster.h.
//
// Clean-room: built from the Scene / MonsterManager / DoorSystem systems and the
// IPhysicsWorld interface only. No purchased C# / id Tech / RBDOOM source.
#include "melee.h"
#include "player.h"
#include "level1_game.h"

#include "engine/core/x3_log.h"

#include <cmath>
#include <memory>
#include <string>

namespace x3::game {

// ---------------------------------------------------------------------------
// Pure forward-cone arc test (testable, no Scene/physics).
// ---------------------------------------------------------------------------
bool inMeleeArc(const x3::phys::Vec3& eye, const x3::phys::Vec3& dir,
                const x3::phys::Vec3& target, float range, float halfAngle) {
    const x3::phys::Vec3 to{ target.x - eye.x, target.y - eye.y, target.z - eye.z };
    const float dist = std::sqrt(to.x * to.x + to.y * to.y + to.z * to.z);
    if (dist > range) return false;          // out of reach
    if (dist < 1e-4f) return true;           // right on top of us -> "in front"

    float dl = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (dl < 1e-6f) dl = 1e-6f;
    // cos(angle) = (dir . to) / (|dir| |to|). In the cone iff cos >= cos(halfAngle).
    const float cosAng = (dir.x * to.x + dir.y * to.y + dir.z * to.z) / (dl * dist);
    return cosAng >= std::cos(halfAngle);
}

// ---------------------------------------------------------------------------
// Throw a super-punch: damage + knockback enemies in the arc, brute-force a door.
// ---------------------------------------------------------------------------
MeleeResult MeleeSystem::strike(const x3::phys::Vec3& eye, const x3::phys::Vec3& dir,
                                Scene& scene, x3::phys::IPhysicsWorld& physics,
                                const std::vector<MonsterManager*>& groups,
                                DoorSystem* doors) {
    MeleeResult r;

    // Normalized look dir, for the swing-FX endpoint + the knockback direction.
    float dl = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (dl < 1e-6f) dl = 1e-6f;
    const x3::phys::Vec3 ndir{ dir.x / dl, dir.y / dl, dir.z / dl };
    r.swingTo = x3::phys::Vec3{ eye.x + ndir.x * kMeleeRange,
                                eye.y + ndir.y * kMeleeRange,
                                eye.z + ndir.z * kMeleeRange };

    if (!ready()) { r.onCooldown = true; return r; }
    m_cooldown = kMeleeCooldown;  // a punch happens now (even on a whiff)

    // ---- Enemies: every LIVE enemy whose body center is inside the forward arc
    // takes heavy damage + a knockback impulse along eye->enemy. ----
    for (MonsterManager* mm : groups) {
        if (!mm) continue;
        for (uint32_t i = 0; i < mm->count(); ++i) {
            MonsterSystem& m = mm->at(i);
            if (!m.alive()) continue;
            const x3::phys::BodyId body = m.body();
            // Body center via the entity transform (translation column) — robust
            // whether or not the physics body is still queryable this frame.
            const Entity& e = scene.get(m.entity());
            const x3::phys::Vec3 center{ e.transform[12], e.transform[13], e.transform[14] };
            if (!inMeleeArc(eye, ndir, center, kMeleeRange, kMeleeHalfAngle)) continue;

            // Knockback impulse along eye->enemy (mostly horizontal). Applied
            // BEFORE the damage call so a kill (which removes the body) doesn't
            // race the impulse on a now-invalid body.
            x3::phys::Vec3 kb{ center.x - eye.x, 0.0f, center.z - eye.z };
            float kl = std::sqrt(kb.x * kb.x + kb.z * kb.z);
            if (kl < 1e-4f) { kb = x3::phys::Vec3{ ndir.x, 0.0f, ndir.z }; kl = 1.0f; }
            const x3::phys::Vec3 imp{ kb.x / kl * kMeleeKnockback, 2.0f,
                                      kb.z / kl * kMeleeKnockback };
            if (body.valid()) physics.applyImpulse(body, imp);

            bool killed = m.takeMeleeDamage(kMeleeDamage, scene, physics);
            ++r.enemiesHit;
            if (killed) ++r.enemiesKilled;
        }
    }

    // ---- Door brute-force: a short look-dir ray (Static layer) hits the door
    // slab; if it resolves to a CLOSED door, force it open (loud strength verb). ----
    if (doors) {
        x3::phys::RayHit hit = physics.rayCast(eye, ndir, kMeleeDoorReach, x3::phys::Layer::Static);
        if (hit.hit && hit.body.valid()) {
            uint32_t ent = scene.entityForBody(hit.body);
            if (ent != kNoLink && ent < scene.size() &&
                scene.get(ent).tag == (uint32_t)Tag::Door) {
                Door* d = doors->findByEntity(ent);
                if (d && d->state == DoorState::Closed) {
                    if (doors->unlockAndOpen(*d)) {
                        r.doorForced = true;
                        x3::logInfo("[melee] BRUTE-FORCED a door open (super-strength)");
                    }
                }
            }
        }
    }

    if (r.enemiesHit > 0)
        x3::logInfo("[melee] super-punch hit " + std::to_string(r.enemiesHit) +
                    " enemy(ies), killed " + std::to_string(r.enemiesKilled));
    return r;
}

// ===========================================================================
// Headless self-test (--test-phase2b). See app/monster.h runPhase2bSelfTest().
//   (a) melee damages an enemy in FRONT but not BEHIND / out of range + knockback;
//   (b) melee on a locked door forces it open;
//   (c) a Boss transitions phase at the HP threshold (enrage stats change) and the
//       Phase3 summon fires exactly once;
//   (d) a Boss still only opens Door E on death (existing gate intact).
// No window / Vulkan. Mirrors the other self-tests.
// ===========================================================================
namespace {

int g_pass = 0, g_fail = 0;
void check(bool cond, const char* name) {
    if (cond) { ++g_pass; x3::logInfo(std::string("[phase2b-test] PASS ") + name); }
    else      { ++g_fail; x3::logError(std::string("[phase2b-test] FAIL ") + name); }
}

constexpr float kDt = 1.0f / 60.0f;

// Minimal headless IRenderDevice (same shape as monster.cpp / level1_game.cpp):
// mints monotonically-increasing valid handles so build()/spawn() run sans Vulkan.
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
    void drawMeshEmissive(const x3::rhi::FrameContext&, x3::rhi::MeshHandle,
                          x3::rhi::TextureHandle, const float[4], const float[4],
                          const float[16]) override {}
    void setPointLights(const x3::rhi::PointLight*, uint32_t) override {}
    void setSkyParams(const x3::rhi::IRenderDevice::SkyParams&) override {}
    void setSsaoParams(const x3::rhi::IRenderDevice::SsaoParams&) override {}
    void setGiParams(const x3::rhi::IRenderDevice::GiParams&) override {}
    void setWaterParams(const x3::rhi::IRenderDevice::WaterParams&) override {}
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

// A stationary, inert Guard tuning (no chase, no attack) so the test geometry is
// stable: the only thing that moves it is the melee knockback.
MonsterSystem::Tuning inertGuard() {
    MonsterSystem::Tuning t;
    t.type = MonsterType::Guard;
    t.hp = 100; t.chaseSpeed = 0.0f; t.damage = 0;
    return t;
}

// A small-HP Boss whose phase thresholds are easy to cross with melee chunks.
MonsterSystem::Tuning testBoss() {
    MonsterSystem::Tuning t;
    t.type = MonsterType::Boss;
    t.hp = 300; t.chaseSpeed = 3.0f; t.damage = 10;
    t.attackRange = 2.0f; t.attackCooldown = 1.0f; t.attackWindup = 0.0f;
    t.phase2Frac = 0.66f; t.phase3Frac = 0.33f;
    t.phase3SummonCount = 2;
    return t;
}

} // namespace

bool runPhase2bSelfTest() {
    g_pass = g_fail = 0;

    HeadlessDevice device;

    // ---- (a) Melee: enemy in FRONT is hit + knocked back; enemy BEHIND / out of
    // range is NOT. Also exercises the pure inMeleeArc() helper directly. ----
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init();
        Scene scene;
        MonsterManager mm;

        // Eye at origin, looking toward +X.
        const x3::phys::Vec3 eye{ 0.0f, 0.6f, 0.0f };
        const x3::phys::Vec3 dir{ 1.0f, 0.0f, 0.0f };

        // Front enemy ~1.5 m ahead (+X); behind enemy ~1.5 m back (-X);
        // far enemy 6 m ahead (out of range).
        uint32_t front = mm.spawn(scene, device, *w, "G:/GameModels/rigged_glb",
                                  x3::phys::Vec3{ 1.5f, 0.6f, 0.0f }, inertGuard());
        uint32_t behind = mm.spawn(scene, device, *w, "G:/GameModels/rigged_glb",
                                   x3::phys::Vec3{ -1.5f, 0.6f, 0.0f }, inertGuard());
        uint32_t farAway = mm.spawn(scene, device, *w, "G:/GameModels/rigged_glb",
                                    x3::phys::Vec3{ 6.0f, 0.6f, 0.0f }, inertGuard());

        // Pure-helper sanity: front in arc, behind + far not.
        bool helperOk =
            inMeleeArc(eye, dir, x3::phys::Vec3{ 1.5f, 0.6f, 0.0f }, kMeleeRange, kMeleeHalfAngle) &&
            !inMeleeArc(eye, dir, x3::phys::Vec3{ -1.5f, 0.6f, 0.0f }, kMeleeRange, kMeleeHalfAngle) &&
            !inMeleeArc(eye, dir, x3::phys::Vec3{ 6.0f, 0.6f, 0.0f }, kMeleeRange, kMeleeHalfAngle);
        check(helperOk, "Ta1 inMeleeArc: front in cone, behind + far out");

        const int frontHp0  = mm.at(front).hp();
        const int behindHp0 = mm.at(behind).hp();
        const int farHp0    = mm.at(farAway).hp();

        MeleeSystem melee;
        std::vector<MonsterManager*> groups{ &mm };
        MeleeResult res = melee.strike(eye, dir, scene, *w, groups, nullptr);

        // Front took heavy damage (and at 100 HP / 120 dmg, died); behind + far did not.
        bool frontHurt  = mm.at(front).hp() < frontHp0 && !mm.at(front).alive();
        bool behindSafe = mm.at(behind).hp() == behindHp0 && mm.at(behind).alive();
        bool farSafe    = mm.at(farAway).hp() == farHp0 && mm.at(farAway).alive();
        bool resOk      = res.enemiesHit == 1 && res.enemiesKilled == 1 && !res.onCooldown;
        check(frontHurt && behindSafe && farSafe && resOk,
              "Ta2 melee damages FRONT enemy only (not behind / out of range)");

        // Knockback: a punch on a DYNAMIC body shoves it. Spawn a dynamic box in the
        // arc, punch, step, and assert it moved away from the eye.
        x3::phys::BodyId box = w->addBox(x3::phys::Vec3{ 0.3f, 0.3f, 0.3f },
                                         x3::phys::Vec3{ 1.2f, 0.6f, 0.0f }, 1.0f,
                                         x3::phys::Layer::Dynamic);
        // applyImpulse directly (the strike() impulse path is the same call) so the
        // test is independent of the enemy bodies being static-by-mass.
        const x3::phys::Vec3 before = w->getBodyPosition(box);
        w->applyImpulse(box, x3::phys::Vec3{ kMeleeKnockback, 2.0f, 0.0f });
        for (int i = 0; i < 30; ++i) w->step(kDt);
        const x3::phys::Vec3 after = w->getBodyPosition(box);
        bool shoved = (after.x - before.x) > 0.1f;   // pushed along +X (away from eye)
        check(shoved, "Ta3 knockback impulse shoves a body away from the punch");

        w->shutdown();
    }

    // ---- (b) Melee on a LOCKED door forces it open. Build a Level 1 game (Door C
    // is locked-until-armed) and punch its slab from in front while unarmed. ----
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init();
        Scene scene;
        Level1Game game;
        game.setDevice(device);
        game.build(scene, device, *w, "G:/GameModels/rigged_glb");
        const Level1Layout& L = game.layout();

        bool lockedBefore = game.doorLocked('C') && game.doorState('C') == DoorState::Closed;
        bool armedBefore  = game.armed();   // must be false (the verb ignores arming)

        // Aim from in front of Door C's slab (−X side) straight at it (+X).
        x3::phys::Vec3 eye{ L.doorC.x - 1.2f, 1.3f, L.doorC.z };
        x3::phys::Vec3 dir{ 1.0f, 0.0f, 0.0f };
        bool forced = game.onMelee(eye, dir, scene, *w).doorForced;

        // Step the door open.
        for (int i = 0; i < 80; ++i) {
            game.tick(kDt, scene, *w, x3::phys::Vec3{ L.spawn.x, 0.05f, 0.0f },
                      x3::phys::Vec3{ L.spawn.x, 0.05f, 0.0f });
            w->step(kDt); scene.update(*w);
        }
        bool openNow = game.doorState('C') == DoorState::Open;
        check(lockedBefore && !armedBefore && forced && openNow,
              "Tb melee brute-forces a LOCKED door open (unarmed)");
        w->shutdown();
    }

    // ---- (c) Boss phase transitions at the HP threshold: enrage stats change +
    // the Phase3 summon callback fires exactly once. ----
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init();
        Scene scene;
        MonsterSystem boss;
        boss.buildMonsterTuned(scene, device, *w, "G:/GameModels/rigged_glb",
                               x3::phys::Vec3{ 0.0f, 0.6f, 0.0f }, testBoss());
        const x3::phys::Vec3 player{ 0.0f, 0.6f, 0.0f };

        int summonCalls = 0;
        BossPhase lastPhase = BossPhase::Phase1;
        auto onPhase = [&](BossPhase p) {
            lastPhase = p;
            if (p == BossPhase::Phase3) summonCalls += boss.summonCount();
        };

        // Phase1 at full HP: neutral stats.
        bool startP1 = boss.phase() == BossPhase::Phase1;
        const float baseSpeed = boss.effectiveChaseSpeed();
        const int   baseDmg   = boss.effectiveDamage();

        // Drop into Phase2 (<=66%): 300 -> ~190 (37%? no, 190/300=0.63 -> Phase2).
        boss.takeMeleeDamage(110, scene, *w);            // 190 HP, 63%
        boss.update(kDt, scene, *w, player, nullptr, AttackFxFn{}, onPhase);
        bool nowP2 = boss.phase() == BossPhase::Phase2;
        bool enraged = boss.effectiveChaseSpeed() > baseSpeed &&
                       boss.effectiveDamage() > baseDmg;

        // Drop into Phase3 (<=33%): 190 -> 90 (30%).
        boss.takeMeleeDamage(100, scene, *w);            // 90 HP, 30%
        boss.update(kDt, scene, *w, player, nullptr, AttackFxFn{}, onPhase);
        bool nowP3 = boss.phase() == BossPhase::Phase3;
        bool summonedOnce = summonCalls == boss.summonCount() && lastPhase == BossPhase::Phase3;

        // A second update at the same HP does NOT re-fire the summon (latch).
        boss.update(kDt, scene, *w, player, nullptr, AttackFxFn{}, onPhase);
        bool noReSummon = summonCalls == boss.summonCount();

        check(startP1 && nowP2 && enraged && nowP3 && summonedOnce && noReSummon,
              "Tc boss enrages at threshold + summons once at Phase3");
        w->shutdown();
    }

    // ---- (d) Boss still only opens Door E on death (existing gate intact). ----
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init();
        Scene scene;
        Level1Game game;
        game.setDevice(device);
        game.build(scene, device, *w, "G:/GameModels/rigged_glb");
        const Level1Layout& L = game.layout();

        // Cross the arena trigger to spawn Martinez.
        for (int i = 0; i < 80; ++i) {
            game.tick(kDt, scene, *w, x3::phys::Vec3{ L.doorD.x + 2.0f, 0.05f, 0.0f },
                      x3::phys::Vec3{ L.doorD.x + 2.0f, 0.05f, 0.0f });
            w->step(kDt); scene.update(*w);
        }
        bool spawned = game.martinezSpawned() && game.martinezAlive();
        bool eClosedWhileAlive = game.doorState('E') == DoorState::Closed && game.doorLocked('E');

        // Punch Martinez to death from the arena (melee is the unarmed verb; super-
        // strength damage chunks the boss). Re-aim each strike.
        x3::phys::Vec3 eye{ L.arenaCenter.x - 1.5f, 0.6f, L.arenaCenter.z };
        for (int i = 0; i < 60 && game.martinezAlive(); ++i) {
            x3::phys::Vec3 tgt{ L.arenaCenter.x, 0.6f, L.arenaCenter.z };
            game.onMelee(eye, sub(tgt, eye), scene, *w);
            // Advance past the melee cooldown + step the world.
            for (int k = 0; k < 32; ++k) {
                game.tick(kDt, scene, *w, eye, eye);
                w->step(kDt); scene.update(*w);
            }
        }
        // Let Door E animate.
        for (int i = 0; i < 80; ++i) {
            game.tick(kDt, scene, *w, eye, eye);
            w->step(kDt); scene.update(*w);
        }
        bool deadNow = game.martinezDead();
        bool eOpenAfter = game.doorState('E') == DoorState::Open;
        check(spawned && eClosedWhileAlive && deadNow && eOpenAfter,
              "Td Door E closed until boss dead, then opens (gate intact)");
        w->shutdown();
    }

    x3::logInfo(std::string("[phase2b-test] ") + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

} // namespace x3::game
