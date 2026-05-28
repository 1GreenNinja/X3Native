#include "companion_controller.h"
#include "monster.h"
#include "scene.h"
#include "asset_root.h"
#include "headless_device.h"
#include "engine/core/x3_log.h"

#include <cmath>
#include <string>

namespace x3::game {

// Revive tuning (mirrors CompanionSquad's constants so behavior is consistent).
namespace {
    constexpr float kCtrlReviveTime = 2.0f;    // s a teammate must hold to revive
    constexpr float kCtrlReviveHp   = 0.40f;   // fraction of maxHp restored on revive
    constexpr float kCtrlPi         = 3.14159265358979f;
}

// ---------------------------------------------------------------------------
// spawn
// ---------------------------------------------------------------------------

void CompanionController::spawn(x3::phys::IPhysicsWorld& phys,
                                x3::rhi::IRenderDevice* dev, Scene* scene,
                                const x3::phys::Vec3& pos, float yaw,
                                const Identity& id) {
    m_id = id;
    m_player.spawn(phys, pos.x, pos.y, pos.z);
    m_player.setLook(yaw, 0.0f);
    m_spawned = true;
    m_downed  = false;
    m_scene   = scene;   // bound for the fire raycast (null in pure-headless)

    // Optional visual body: an inert MonsterSystem (chaseSpeed=0, damage=0) loaded
    // from the identity's rigged GLB so drawBody() can render it. Mirrors the
    // `--world companion` visual-proxy pattern. Skipped in headless tests.
    if (dev && scene && !m_id.modelFile.empty()) {
        m_viz = std::make_unique<MonsterSystem>();
        MonsterSystem::Tuning vt;
        vt.hp              = 100;
        vt.chaseSpeed      = 0.0f;   // inert: the Player capsule owns movement
        vt.damage          = 0;
        vt.modelFile       = m_id.modelFile;
        vt.modelDirOverride = riggedGlbRoot();
        vt.modelScale      = 1.0f;
        vt.standUpZtoY     = false;
        m_viz->buildMonsterTuned(*scene, *dev, phys, riggedGlbRoot(), pos, vt);
    }
}

// ---------------------------------------------------------------------------
// tick
// ---------------------------------------------------------------------------

void CompanionController::tick(float dt, x3::phys::IPhysicsWorld& phys,
                               const x3::phys::Vec3& playerPos, float playerHpFrac,
                               bool playerDowned,
                               MonsterSystem* const* threats, uint32_t threatCount) {
    if (!m_spawned) return;

    // ---- Downed: stop acting; tick only health timers. ----
    if (m_downed) {
        m_player.updateHealth(dt);
        return;
    }

    // ---- Detect a fresh death (HP just hit 0 -> enter Downed). ----
    if (!m_player.isAlive()) {
        m_downed = true;
        m_player.updateHealth(dt);
        x3::logInfo("[companion-ctrl] " + m_id.name + " went DOWNED");
        return;
    }

    // ---- Build the tactical context from the live world. ----
    CompanionContext ctx;
    const x3::phys::Vec3 fp = m_player.feet();
    ctx.selfPos      = { fp.x, fp.y + 1.6f, fp.z };
    ctx.playerPos    = playerPos;
    ctx.selfHpFrac   = m_player.isAlive()
                           ? (float)m_player.hp() / (float)m_player.maxHp()
                           : 0.0f;
    ctx.ammoInMag    = m_ammoInMag;
    ctx.playerDowned = playerDowned;
    ctx.anyAllyDowned = playerDowned;
    ctx.downedAllyPos = playerPos;
    ctx.suggestion   = m_suggestion;   // Slice-C cognitive bias (soft nudge)

    // Threat scratch (stack-local, max 16). Single-companion -> no shared buffer.
    CompanionThreat tBuf[16];
    int tc = 0;
    for (uint32_t t = 0; t < threatCount && tc < 16; ++t) {
        if (!threats[t] || !threats[t]->alive()) continue;
        const x3::phys::Vec3 tp = threats[t]->pos();
        const float dx = tp.x - ctx.selfPos.x;
        const float dy = tp.y - ctx.selfPos.y;
        const float dz = tp.z - ctx.selfPos.z;
        const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        const float pdx = tp.x - playerPos.x;
        const float pdz = tp.z - playerPos.z;
        tBuf[tc].pos       = tp;
        tBuf[tc].dist      = dist;
        tBuf[tc].toPlayer  = std::sqrt(pdx * pdx + pdz * pdz);
        tBuf[tc].losToSelf = true;   // LOS deferred (rayCast would go here)
        ++tc;
    }
    ctx.threats     = (tc > 0) ? tBuf : nullptr;
    ctx.threatCount = tc;
    ctx.nearCover          = false;
    ctx.inPlayerLineOfFire = false;
    (void)playerHpFrac;   // reserved for future context use (parity with squad)

    // ---- Run the reflex brain. ----
    const CompanionCommand cmd = m_brain.tick(ctx);
    m_lastBehavior = cmd.chosen;

    // ---- Fire: raycast from the eye toward the aim, damage matching monsters. ----
    // Requires a live Scene (set at spawn) to register impacts. Mirrors
    // CompanionSquad::doCompanionFire. No-op when no scene is bound (pure-headless).
    if (cmd.fire && m_scene) {
        float ex, ey, ez, fyaw, fpitch;
        m_player.camera(ex, ey, ez, fyaw, fpitch);
        const x3::phys::Vec3 eye{ ex, ey, ez };
        const float cp = std::cos(fpitch), sp = std::sin(fpitch);
        const float cy = std::cos(fyaw),   sy = std::sin(fyaw);
        const x3::phys::Vec3 dir{ cp * cy, sp, cp * sy };
        for (uint32_t t = 0; t < threatCount; ++t) {
            if (!threats[t] || !threats[t]->alive()) continue;
            threats[t]->fire(eye, dir, *m_scene, phys);
        }
    }

    // ---- Orient the companion toward its behavior target. ----
    float camX, camY, camZ, selfYaw, selfPitch;
    m_player.camera(camX, camY, camZ, selfYaw, selfPitch);
    float targetYaw   = selfYaw;
    float targetPitch = 0.0f;
    switch (cmd.chosen) {
    case CompanionBehavior::Follow:
    case CompanionBehavior::Hold: {
        const float dx = playerPos.x - camX, dz = playerPos.z - camZ;
        if (std::sqrt(dx * dx + dz * dz) > 0.5f) targetYaw = std::atan2(dz, dx);
        break;
    }
    case CompanionBehavior::Engage:
    case CompanionBehavior::TakeCover:
        targetYaw   = cmd.aimYaw;
        targetPitch = cmd.aimPitch;
        break;
    case CompanionBehavior::Retreat:
        targetYaw = cmd.aimYaw + kCtrlPi;
        break;
    case CompanionBehavior::Revive: {
        const float dx = ctx.downedAllyPos.x - camX, dz = ctx.downedAllyPos.z - camZ;
        if (std::sqrt(dx * dx + dz * dz) > 0.3f) targetYaw = std::atan2(dz, dx);
        break;
    }
    case CompanionBehavior::Reload:
    default:
        break;
    }
    m_player.setLook(targetYaw, targetPitch);

    // ---- Drive movement. ----
    PlayerInput in;
    in.moveFwd     = cmd.moveFwd;
    in.moveStrafe  = cmd.moveStrafe;
    in.sprint      = cmd.sprint;
    in.jumpPressed = cmd.jumpPressed;
    m_player.update(in, dt, phys);
}

// ---------------------------------------------------------------------------
// takeDamage / reviveProgress
// ---------------------------------------------------------------------------

void CompanionController::takeDamage(int amount) {
    if (m_downed) return;
    m_player.takeDamage(amount);
    if (!m_player.isAlive()) {
        m_downed = true;
        x3::logInfo("[companion-ctrl] " + m_id.name + " DOWNED by damage");
    }
}

bool CompanionController::reviveProgress(float dt) {
    if (!m_downed) return false;
    m_reviveTimer += dt;
    if (m_reviveTimer >= kCtrlReviveTime) {
        const int restoreHp = std::max(1, (int)((float)m_player.maxHp() * kCtrlReviveHp));
        m_player.setHp(restoreHp);
        m_downed      = false;
        m_reviveTimer = 0.0f;
        x3::logInfo("[companion-ctrl] " + m_id.name + " REVIVED (hp=" +
                    std::to_string(m_player.hp()) + ")");
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// drawBody / shutdown
// ---------------------------------------------------------------------------

void CompanionController::drawBody(x3::rhi::IRenderDevice& dev,
                                   const x3::rhi::FrameContext& fr,
                                   Scene& scene) {
    if (!m_viz) return;
    // Sync the visual proxy's entity transform to the capsule pose, then draw.
    const uint32_t eid = m_viz->entity();
    if (eid != kNoLink && eid < scene.size()) {
        const x3::phys::Vec3 f = m_player.feet();
        // Rigged GLBs authored facing +Z; flip by +pi to match the game convention
        // (same fix as MonsterSystem's facing bake).
        const float ry = m_player.yaw() + kCtrlPi;
        const float c = std::cos(ry), s = std::sin(ry);
        Entity& ve = scene.get(eid);
        ve.transform[0]=c;  ve.transform[1]=0.0f; ve.transform[2]=-s; ve.transform[3]=0.0f;
        ve.transform[4]=0.0f; ve.transform[5]=1.0f; ve.transform[6]=0.0f; ve.transform[7]=0.0f;
        ve.transform[8]=s;  ve.transform[9]=0.0f; ve.transform[10]=c; ve.transform[11]=0.0f;
        ve.transform[12]=f.x; ve.transform[13]=f.y; ve.transform[14]=f.z; ve.transform[15]=1.0f;
    }
    m_viz->drawMonster(dev, fr, scene);
}

void CompanionController::shutdown(x3::phys::IPhysicsWorld& phys) {
    (void)phys;   // character body removed when the physics world shuts down
    m_viz.reset();
    m_spawned = false;
}

// ---------------------------------------------------------------------------
// runCompanionControllerSelfTest (--test-companion-controller)
// ---------------------------------------------------------------------------

bool runCompanionControllerSelfTest() {
    x3::logInfo("[companion-ctrl-test] running companion controller self-test...");
    int pass = 0, total = 0;

    std::unique_ptr<x3::phys::IPhysicsWorld> phys(x3::phys::createPhysicsWorld());
    if (!phys->init()) {
        x3::logError("[companion-ctrl-test] physics init failed");
        return false;
    }
    phys->addBox({ 50.0f, 0.5f, 50.0f }, { 0.0f, -0.5f, 0.0f },
                 0.0f, x3::phys::Layer::Static);

    const x3::phys::Vec3 playerPos{ 0.0f, 1.6f, -5.0f };

    // ------------------------------------------------------------------
    // A1: spawn creates a live companion carrying its identity.
    // ------------------------------------------------------------------
    CompanionController anna;   // female -> Grok
    {
        total++;
        CompanionController::Identity id;
        id.name = "Anna"; id.female = true; id.modelFile = "";  // headless: no visual
        anna.spawn(*phys, nullptr, nullptr, { 2.0f, 0.5f, 0.0f }, 0.0f, id);
        const bool ok = anna.isAlive() && !anna.isDowned() &&
                        anna.identity().name == "Anna" && anna.hp() > 0;
        if (ok) { pass++; x3::logInfo("[companion-ctrl-test] A1 PASS (spawn + identity)"); }
        else x3::logError("[companion-ctrl-test] A1 FAIL (spawn/identity)");
    }

    // ------------------------------------------------------------------
    // A2: Provider routing -- female -> Grok, male -> Claude.
    // ------------------------------------------------------------------
    CompanionController marcus;  // male -> Claude
    {
        total++;
        CompanionController::Identity mid;
        mid.name = "Marcus"; mid.female = false; mid.modelFile = "";
        marcus.spawn(*phys, nullptr, nullptr, { -2.0f, 0.5f, 0.0f }, 0.0f, mid);
        const bool ok = (anna.provider()   == CompanionController::Provider::Grok) &&
                        (marcus.provider() == CompanionController::Provider::Claude);
        if (ok) { pass++; x3::logInfo("[companion-ctrl-test] A2 PASS (provider routing)"); }
        else x3::logError("[companion-ctrl-test] A2 FAIL (provider routing)");
    }

    // Settle both companions onto the floor.
    for (int i = 0; i < 10; ++i) {
        anna.tick(1.0f / 60.0f, *phys, playerPos, 1.0f, false, nullptr, 0);
        marcus.tick(1.0f / 60.0f, *phys, playerPos, 1.0f, false, nullptr, 0);
        phys->step(1.0f / 60.0f);
    }

    // ------------------------------------------------------------------
    // A3: tick() with a threat present makes the brain choose Engage.
    //     Build a CompanionThreat at ~10m in LOS and tick the controller.
    // ------------------------------------------------------------------
    {
        total++;
        // Build one inert enemy ~10m ahead of Anna and tick the controller; the
        // brain should choose Engage. Needs a headless device + scene for the
        // MonsterSystem build (tick() takes MonsterSystem* threats).
        HeadlessRenderDevice dev;
        x3::rhi::DeviceDesc dsc; dsc.width = 64; dsc.height = 64; dsc.headless = true;
        dev.init(dsc);
        Scene scene;
        MonsterSystem enemy;
        MonsterSystem::Tuning t;
        t.hp = 100; t.chaseSpeed = 0.0f; t.damage = 0;
        const x3::phys::Vec3 ap = anna.feet();
        enemy.buildMonsterTuned(scene, dev, *phys, "",
                                { ap.x, ap.y + 1.6f, ap.z + 10.0f }, t);
        MonsterSystem* threats[1] = { &enemy };
        anna.clearSuggestion();
        anna.tick(1.0f / 60.0f, *phys, playerPos, 1.0f, false, threats, 1);
        const bool ok = (anna.lastBehavior() == CompanionBehavior::Engage);
        if (ok) { pass++; x3::logInfo("[companion-ctrl-test] A3 PASS (engage threat)"); }
        else x3::logError("[companion-ctrl-test] A3 FAIL (expected Engage, got " +
                          std::to_string((int)anna.lastBehavior()) + ")");
        dev.shutdown();
    }

    // ------------------------------------------------------------------
    // A4: suggest() biases the chosen behavior vs. the no-suggestion baseline.
    //     Baseline (no threat, player 8m away) -> Follow; a Hold suggestion flips it.
    // ------------------------------------------------------------------
    {
        total++;
        const x3::phys::Vec3 farPlayer{ anna.feet().x, 1.6f, anna.feet().z + 8.0f };
        // Baseline tick (no suggestion).
        anna.clearSuggestion();
        anna.tick(1.0f / 60.0f, *phys, farPlayer, 1.0f, false, nullptr, 0);
        const CompanionBehavior baseline = anna.lastBehavior();
        // Biased tick (Hold suggestion).
        CompanionSuggestion sug; sug.prefer = CompanionBehavior::Hold;
        anna.suggest(sug);
        anna.tick(1.0f / 60.0f, *phys, farPlayer, 1.0f, false, nullptr, 0);
        const CompanionBehavior biased = anna.lastBehavior();
        const bool ok = (baseline == CompanionBehavior::Follow) &&
                        (biased == CompanionBehavior::Hold) &&
                        (baseline != biased);
        if (ok) { pass++; x3::logInfo("[companion-ctrl-test] A4 PASS (suggestion bias)"); }
        else x3::logError("[companion-ctrl-test] A4 FAIL (baseline=" +
                          std::to_string((int)baseline) + " biased=" +
                          std::to_string((int)biased) + ")");
        anna.clearSuggestion();
    }

    // ------------------------------------------------------------------
    // A5: say() / pendingSpeech() round-trip (the Slice-C speech buffer).
    // ------------------------------------------------------------------
    {
        total++;
        anna.say("Contact, two o'clock!");
        const bool got = (anna.pendingSpeech() == "Contact, two o'clock!");
        anna.clearSpeech();
        const bool cleared = anna.pendingSpeech().empty();
        if (got && cleared) { pass++; x3::logInfo("[companion-ctrl-test] A5 PASS (speech round-trip)"); }
        else x3::logError("[companion-ctrl-test] A5 FAIL (speech round-trip)");
    }

    // ------------------------------------------------------------------
    // A6: downed -> reviveProgress -> alive lifecycle.
    // ------------------------------------------------------------------
    {
        total++;
        // Knock Marcus down via takeDamage (drains all HP).
        marcus.takeDamage(marcus.maxHp() + 50);
        const bool downedOK = marcus.isDowned() && !marcus.isAlive();
        // Drive the revive: progress should NOT complete before the hold time.
        bool earlyComplete = marcus.reviveProgress(0.5f);
        // Finish the revive.
        bool completed = false;
        for (int i = 0; i < 200 && !completed; ++i) {
            completed = marcus.reviveProgress(1.0f / 60.0f);
        }
        const bool aliveOK = completed && marcus.isAlive() && !marcus.isDowned() &&
                             marcus.hp() > 0;
        const bool ok = downedOK && !earlyComplete && aliveOK;
        if (ok) { pass++; x3::logInfo("[companion-ctrl-test] A6 PASS (downed->revive->alive)"); }
        else x3::logError("[companion-ctrl-test] A6 FAIL (downed=" +
                          std::to_string(downedOK) + " early=" +
                          std::to_string(earlyComplete) + " alive=" +
                          std::to_string(aliveOK) + ")");
    }

    // ------------------------------------------------------------------
    // A7: shutdown clean (no crash; controller marked not spawned).
    // ------------------------------------------------------------------
    {
        total++;
        anna.shutdown(*phys);
        marcus.shutdown(*phys);
        // A tick after shutdown is a safe no-op.
        anna.tick(1.0f / 60.0f, *phys, playerPos, 1.0f, false, nullptr, 0);
        pass++;   // reaching here without crashing == pass
        x3::logInfo("[companion-ctrl-test] A7 PASS (clean shutdown)");
    }

    phys->shutdown();
    x3::logInfo("[companion-ctrl-test] " + std::to_string(pass) + "/" +
                std::to_string(total) + " passed");
    return pass == total;
}

} // namespace x3::game
