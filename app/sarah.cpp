// SARAH COMPANION COMBAT (LANE B). See app/sarah.h.
//
// Clean-room: built from Scene/MonsterSystem + the engine interfaces (IRenderDevice /
// IPhysicsWorld / IModelLoader / IAssetSource) only. Mirrors rescue.cpp (the #48
// companion path) for load/skin/follow, and the monster.cpp drone ranged path for the
// LOS-gated hitscan — retargeted from "shoot Jake" to "shoot the nearest hostile".
#include "sarah.h"
#include "mesh_prims.h"
#include "headless_device.h"
#include "asset_root.h"

#include "engine/core/x3_log.h"

#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace x3::game {

namespace {

// Sarah's collision box half-extents (a standing humanoid ~1.8 m), matching rescue.
constexpr x3::phys::Vec3 kSarahHalf{ 0.4f, 0.9f, 0.4f };
constexpr float kSarahScale = 1.0f;

// Column-major 4x4 from a 3x3 basis (columns bx,by,bz), uniform scale s, translation t.
// Identical to rescue.cpp / monster.cpp composeTRS.
void composeTRS(float m[16],
                const x3::phys::Vec3& bx, const x3::phys::Vec3& by, const x3::phys::Vec3& bz,
                float s, const x3::phys::Vec3& t) {
    m[0]  = bx.x * s; m[1]  = bx.y * s; m[2]  = bx.z * s; m[3]  = 0.0f;
    m[4]  = by.x * s; m[5]  = by.y * s; m[6]  = by.z * s; m[7]  = 0.0f;
    m[8]  = bz.x * s; m[9]  = bz.y * s; m[10] = bz.z * s; m[11] = 0.0f;
    m[12] = t.x;      m[13] = t.y;      m[14] = t.z;      m[15] = 1.0f;
}

// Facing law (CONVENTIONS.md / rescue.cpp): to point a model's local -Z along planar
// (dirX,dirZ), yaw = atan2(-dirX,-dirZ).
float headingToFace(float dirX, float dirZ) {
    if (dirX * dirX + dirZ * dirZ < 1e-12f) return 0.0f;
    return std::atan2(-dirX, -dirZ);
}

} // namespace

// ===========================================================================
// SarahCompanion
// ===========================================================================
void SarahCompanion::build(Scene& scene, x3::rhi::IRenderDevice& device,
                           x3::phys::IPhysicsWorld& physics,
                           std::string_view modelDir, const x3::phys::Vec3& pos) {
    m_pos    = pos;
    m_state  = SarahState::Restrained;
    m_hp     = kSarahHp;
    m_device = &device;
    // Warm friendly tint so she reads distinct from the magenta/red hostiles.
    m_tint[0] = 0.85f; m_tint[1] = 0.95f; m_tint[2] = 1.0f; m_tint[3] = 1.0f;

    // ---- Load the AnnaTactical rig via a mounted loose-dir asset source (same path
    // as RescueVictim::build). Rigged humanoids are Y-up. Box fallback on failure. ----
    const std::string file = "AnnaTactical.glb";
    m_assets.reset(x3::asset::createAssetSource());
    if (m_assets->mountDir(std::string(modelDir), 0)) {
        m_loader.reset(x3::asset::createModelLoader(&device, m_assets.get()));
        m_model = m_loader->load(file);
        if (m_model.ok) m_drawables = x3::asset::makeDrawables(m_model);
    } else {
        x3::logWarn("[sarah] mountDir failed: " + std::string(modelDir));
    }

    if (!m_drawables.empty()) {
        m_usingReal  = true;
        m_modelScale = kSarahScale;
        x3::logInfo("[sarah] loaded " + file + " — " +
                    std::to_string(m_drawables.size()) + " primitive(s)");
        // Bind the skeletal-animation runtime so she idles/walks/aims instead of
        // freezing at bind/T-pose (mirrors RescueVictim / MonsterSystem).
        if (m_model.ok && m_skinner.bind(m_model)) {
            const bool gpuSkin = m_skinner.enableGpuSkinning(device, m_model);
            m_idleClip = m_skinner.findClip({ "idle", "stand", "breath", "loop" });
            m_walkClip = m_skinner.findClip({ "walk" });
            m_runClip  = m_skinner.findClip({ "run", "sprint", "jog" });
            m_aimClip  = m_skinner.findClip({ "aim", "fire", "shoot", "rifle", "shot", "attack" });
            m_deathClip= m_skinner.findClip({ "death", "die", "collapse", "down", "fall" });
            if (m_walkClip < 0) m_walkClip = m_skinner.findClip({ "move", "jog", "run" });
            if (m_idleClip < 0) m_idleClip = 0;   // fall back to the first clip
            m_animActive   = (m_idleClip >= 0);
            m_useLocoBlend = m_animActive && (m_walkClip >= 0 || m_runClip >= 0);
            if (m_useLocoBlend)
                m_skinner.setLocomotionClips(m_idleClip, m_walkClip, m_runClip, 0.2f, 2.0f);
            // Pose into idle up front so the FIRST rendered frame is the animated pose.
            if (m_animActive && m_device) {
                if (m_useLocoBlend) { m_skinner.setLocomotionSpeed(0.0f);
                                      m_skinner.applyLocomotion(m_model, *m_device, 0.0f); }
                else                m_skinner.apply(m_model, *m_device, (uint32_t)m_idleClip, 0.0f);
            }
            x3::logInfo(std::string("[sarah] animated (") + (gpuSkin ? "GPU" : "CPU") +
                        " skin) — idle=" + std::to_string(m_idleClip) + " walk=" +
                        std::to_string(m_walkClip) + " aim=" + std::to_string(m_aimClip) +
                        " death=" + std::to_string(m_deathClip) + " locoBlend=" +
                        (m_useLocoBlend ? "1" : "0"));
        } else {
            x3::logInfo("[sarah] " + file + " not skinnable — static draw");
        }
    } else {
        // Fallback box so Sarah still exists + is testable headlessly.
        m_usingReal  = false;
        m_modelScale = kSarahScale;
        x3::logWarn("[sarah] " + file + " load failed; using fallback box");
        x3::prims::PrimMesh geo = x3::prims::makeBox(kSarahHalf.x, kSarahHalf.y, kSarahHalf.z,
                                                     0.0f, 0.0f, 0.0f, 1.0f);
        x3::rhi::MeshHandle mesh = device.createMesh(
            geo.verts.data(), (uint32_t)geo.verts.size(),
            geo.index.data(), (uint32_t)geo.index.size());
        x3::asset::ModelDrawable d;
        d.meshId = mesh.id;
        d.baseColorTexId = 0;
        d.baseColorFactor[0] = 0.7f; d.baseColorFactor[1] = 0.9f;
        d.baseColorFactor[2] = 1.0f; d.baseColorFactor[3] = 1.0f;   // friendly blue
        m_drawables.push_back(d);
    }

    // ---- Static-by-mass Enemy-layer box: lets her move via setBodyPosition (the
    // teleport trick the monster chase / S4 door use). Mass 0 so she stays put while
    // restrained. NOTE: Enemy layer keeps her out of the player's own shot ray while
    // still being a solid presence — she is friendly, so the player never targets her. -
    m_body = physics.addBox(kSarahHalf, m_pos, 0.0f, x3::phys::Layer::Enemy);

    // ---- Tag::Prop entity: bookkeeping only; render mesh left invalid so draw() owns
    // the multi-primitive draw (mirrors RescueVictim). ----
    Entity e;
    e.tag     = (uint32_t)Tag::Prop;
    e.visible = true;
    e.body    = m_body;
    composeTRS(e.transform,
               x3::phys::Vec3{1,0,0}, x3::phys::Vec3{0,1,0}, x3::phys::Vec3{0,0,1},
               m_modelScale, m_pos);
    m_entity = scene.add(e);
    m_built  = true;

    x3::logInfo("[sarah] restrained at (" + std::to_string(pos.x) + ", " +
                std::to_string(pos.y) + ", " + std::to_string(pos.z) + ")" +
                (m_usingReal ? " [real GLB]" : " [fallback box]"));
}

void SarahCompanion::bakeTransform(Scene& scene) {
    if (m_entity == kNoLink || m_entity >= scene.size()) return;
    // FACING FLIP (matches rescue.cpp): the rigged GLBs are authored facing +Z, but
    // headingToFace()/m_yaw assume local -Z forward — so flip the VISUAL yaw 180deg here.
    const float ry = m_yaw + 3.14159265358979323846f;
    const float c = std::cos(ry), s = std::sin(ry);
    Entity& me = scene.get(m_entity);
    composeTRS(me.transform,
               x3::phys::Vec3{ c, 0.0f, -s },
               x3::phys::Vec3{ 0.0f, 1.0f, 0.0f },
               x3::phys::Vec3{ s, 0.0f, c },
               m_modelScale, m_pos);
}

void SarahCompanion::driveAnim(float dt, float planarSpeed) {
    if (!m_animActive || !m_device) return;
    // Downed: hold the death/collapse pose (frozen at its end); else idle frozen.
    if (m_state == SarahState::Incapacitated) {
        const int clip = (m_deathClip >= 0) ? m_deathClip : m_idleClip;
        m_skinner.apply(m_model, *m_device, (uint32_t)clip, m_animTime);
        return;
    }
    // Firing: the aim/fire pose preempts locomotion so the shot reads (grounded-anim:
    // upright, rifle up). If the rig has no aim clip, fall through to locomotion.
    if (m_fireAnimT >= 0.0f && m_aimClip >= 0) {
        m_skinner.apply(m_model, *m_device, (uint32_t)m_aimClip, m_fireAnimT);
        return;
    }
    if (m_useLocoBlend) {
        m_skinner.setLocomotionSpeed(planarSpeed);
        m_skinner.applyLocomotion(m_model, *m_device, dt);
    } else {
        const bool hasWalk = (m_walkClip >= 0);
        const bool moving  = planarSpeed > 0.25f;
        const int  clip    = (hasWalk && moving) ? m_walkClip : m_idleClip;
        const float rate   = (!hasWalk && moving)
            ? (1.0f + (planarSpeed < 4.0f ? planarSpeed : 4.0f) * 0.35f) : 1.0f;
        m_animTime += dt * rate;
        m_skinner.apply(m_model, *m_device, (uint32_t)clip, m_animTime);
    }
}

void SarahCompanion::onFreed() {
    if (m_state != SarahState::Restrained) return;   // idempotent / no-op if downed
    m_state    = SarahState::Awake;
    m_wasFreed = true;
    x3::logInfo("[sarah] FREED — waking to fight beside Jake");
    bark("Collar's off. I've got your back — let's END him.");
}

void SarahCompanion::onCloneDown() {
    if (m_cloneDownBarked) return;
    m_cloneDownBarked = true;
    bark("That thing wore your face. Good riddance.");
}

bool SarahCompanion::takeDamage(int dmg, Scene& scene, x3::phys::IPhysicsWorld& physics) {
    // She can only be hurt once in the fight (never while restrained / already down).
    if (m_state != SarahState::Awake || dmg <= 0) return false;
    m_hp -= dmg;
    if (m_hp > 0) return false;
    // ---- INCAPACITATED (the emotional beat): NOT deleted. Drop her combat state,
    // remove the standing collision body (so a corpse can't block the fight), but KEEP
    // her entity + model visible in a downed pose. ----
    m_hp    = 0;
    m_state = SarahState::Incapacitated;
    m_target = nullptr;
    m_fireAnimT = -1.0f;
    m_animTime  = 0.0f;   // restart the downed clip from its head
    if (m_body.valid()) { physics.removeBody(m_body); m_body = x3::phys::BodyId{}; }
    if (m_entity != kNoLink && m_entity < scene.size()) {
        Entity& me = scene.get(m_entity);
        me.body = x3::phys::BodyId{};   // freeze: Scene::update() no longer moves her
        // me.visible stays TRUE — she lies there, she is not gone.
    }
    x3::logInfo("[sarah] DOWN — incapacitated (not deleted)");
    bark("...go. I'll... hold here.");
    return true;
}

MonsterSystem* SarahCompanion::acquireNearest(const std::vector<MonsterSystem*>& hostiles) const {
    MonsterSystem* best = nullptr;
    float bestD2 = kSarahEngageRange * kSarahEngageRange;
    for (MonsterSystem* m : hostiles) {
        if (!m || !m->alive() || m->isAllied()) continue;   // skip dead / friendly
        const x3::phys::Vec3 mp = m->pos();
        const float dx = mp.x - m_pos.x, dz = mp.z - m_pos.z;
        const float d2 = dx * dx + dz * dz;
        if (d2 <= bestD2) { bestD2 = d2; best = m; }
    }
    return best;
}

bool SarahCompanion::losClear(x3::phys::IPhysicsWorld& physics, const x3::phys::Vec3& p) const {
    // Muzzle just above her center; skip her own half-extent so the ray doesn't self-hit
    // her Enemy box (same trick the drone ranged path uses).
    const x3::phys::Vec3 muzzle{ m_pos.x, m_pos.y + 0.5f, m_pos.z };
    x3::phys::Vec3 d{ p.x - muzzle.x, p.y + 0.4f - muzzle.y, p.z - muzzle.z };
    float dl = std::sqrt(d.x*d.x + d.y*d.y + d.z*d.z);
    if (dl < 1e-4f) return true;
    const x3::phys::Vec3 nd{ d.x/dl, d.y/dl, d.z/dl };
    const float skip = kSarahHalf.x + 0.15f;
    const x3::phys::Vec3 from{ muzzle.x + nd.x*skip, muzzle.y + nd.y*skip, muzzle.z + nd.z*skip };
    float losLen = dl - skip; if (losLen < 1e-3f) return true;
    x3::phys::RayHit wall = physics.rayCast(from, nd, losLen, x3::phys::Layer::Static);
    return !wall.hit;
}

void SarahCompanion::tick(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics,
                          const x3::phys::Vec3& playerPos,
                          const std::vector<MonsterSystem*>& hostiles,
                          const std::vector<x3::phys::Vec3>& allies) {
    if (!m_built) return;

    // ---- Restrained: idle in place (breathe), do not follow or fight. ----
    if (m_state == SarahState::Restrained) {
        bakeTransform(scene);
        driveAnim(dt, 0.0f);
        return;
    }
    // ---- Incapacitated: hold the downed pose (no follow / no fire). ----
    if (m_state == SarahState::Incapacitated) {
        m_animTime += dt;
        driveAnim(dt, 0.0f);
        return;
    }

    // ======================= AWAKE: follow + fight =========================
    const x3::phys::Vec3 prevPos = m_pos;

    // ---- FOLLOW Jake: close to the standoff ring (mirror the rescue companion). ----
    {
        const float dx = playerPos.x - m_pos.x, dz = playerPos.z - m_pos.z;
        const float horiz = std::sqrt(dx * dx + dz * dz);
        if (horiz > kSarahTeleport) {
            m_pos.x = playerPos.x; m_pos.z = playerPos.z;      // snap up if lost behind
        } else if (horiz > kSarahFollowStop) {
            const float inv = (horiz > 1e-4f) ? 1.0f / horiz : 0.0f;
            const float step = kSarahFollowSpeed * dt;
            const float travel = (horiz - kSarahFollowStop < step) ? (horiz - kSarahFollowStop) : step;
            m_pos.x += dx * inv * travel; m_pos.z += dz * inv * travel;
        }
    }

    // ---- SEPARATION (#25 / ecology): push off anything within the crowd radius — the
    // player first (never block Jake) then other allies — so she flanks, not stacks. ----
    {
        auto pushOff = [&](const x3::phys::Vec3& o) {
            const float dx = m_pos.x - o.x, dz = m_pos.z - o.z;
            const float d2 = dx * dx + dz * dz;
            if (d2 < kSarahSepRadius * kSarahSepRadius) {
                float d = std::sqrt(d2);
                float nx, nz;
                if (d > 1e-4f) { nx = dx / d; nz = dz / d; }
                else { nx = 1.0f; nz = 0.0f; }               // exact overlap: pick +X
                const float push = kSarahSepStrength * dt;
                m_pos.x += nx * push; m_pos.z += nz * push;
            }
        };
        pushOff(playerPos);
        for (const x3::phys::Vec3& a : allies) pushOff(a);
    }

    // ---- ACQUIRE the nearest hostile + FIRE on cooldown (LOS-gated). ----
    m_target = acquireNearest(hostiles);
    if (m_fireCooldown > 0.0f) m_fireCooldown -= dt;
    // Advance / clear the aim-pose timer.
    if (m_fireAnimT >= 0.0f) {
        m_fireAnimT += dt;
        if (m_fireAnimT > kSarahAimTime + 0.35f) m_fireAnimT = -1.0f;   // pose released
    }

    if (m_target) {
        const x3::phys::Vec3 tp = m_target->pos();
        const float dx = tp.x - m_pos.x, dz = tp.z - m_pos.z;
        const float dist = std::sqrt(dx * dx + dz * dz);
        // FACE the target while engaging (overrides the follow-facing).
        m_yaw = headingToFace(dx, dz);
        if (dist <= kSarahFireRange && m_fireCooldown <= 0.0f && losClear(physics, tp)) {
            // Enter the aim pose, then land the hitscan on the acquired hostile.
            if (m_fireAnimT < 0.0f) m_fireAnimT = 0.0f;
            m_fireCooldown = kSarahFireCooldown;
            const x3::phys::Vec3 muzzle{ m_pos.x, m_pos.y + 0.5f, m_pos.z };
            if (m_fx) m_fx(muzzle, x3::phys::Vec3{ tp.x, tp.y + 0.4f, tp.z });
            const bool killed = m_target->takeMeleeDamage(kSarahShotDamage, scene, physics,
                                                          x3::DamageType::Kinetic);
            if (killed) {
                ++m_kills;
                if (!m_firstKillBarked) { m_firstKillBarked = true;
                    bark("One down. Keep moving!"); }
            }
        }
    } else {
        // No hostile: face where we're heading (toward Jake).
        const float fdx = playerPos.x - m_pos.x, fdz = playerPos.z - m_pos.z;
        if (fdx*fdx + fdz*fdz > 1e-4f) m_yaw = headingToFace(fdx, fdz);
    }

    if (m_body.valid()) physics.setBodyPosition(m_body, m_pos);
    bakeTransform(scene);

    // Drive locomotion from this frame's real movement (ignore teleport snaps).
    const float ddx = m_pos.x - prevPos.x, ddz = m_pos.z - prevPos.z;
    float planarSpeed = (dt > 1e-5f) ? std::sqrt(ddx*ddx + ddz*ddz) / dt : 0.0f;
    if (planarSpeed > kSarahTeleport) planarSpeed = 0.0f;
    driveAnim(dt, planarSpeed);
}

void SarahCompanion::draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                          const Scene& scene) const {
    if (m_entity == kNoLink || m_entity >= scene.size()) return;
    const Entity& e = scene.get(m_entity);
    if (!e.visible) return;
    drawAt(device, frame, e.transform);
}

void SarahCompanion::drawAt(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                            const float model[16]) const {
    for (const auto& d : m_drawables) {
        float color[4] = {
            d.baseColorFactor[0] * m_tint[0], d.baseColorFactor[1] * m_tint[1],
            d.baseColorFactor[2] * m_tint[2], d.baseColorFactor[3] * m_tint[3],
        };
        float fin[16];
        x3::asset::mulMat4(model, d.nodeTransform, fin);
        device.drawMesh(frame, x3::rhi::MeshHandle{ d.meshId },
                        x3::rhi::TextureHandle{ d.baseColorTexId }, color, fin);
    }
}

void SarahCompanion::bark(const std::string& line) {
    if (m_bark) m_bark(line);
    else x3::logInfo("[sarah] (bark) " + line);
}

// ===========================================================================
// Headless self-test (--test-companion-combat). No window / Vulkan.
// ===========================================================================
namespace {
int g_cpass = 0, g_cfail = 0;
void ccheck(bool cond, const char* name) {
    if (cond) { ++g_cpass; x3::logInfo(std::string("[sarah-test] PASS ") + name); }
    else      { ++g_cfail; x3::logError(std::string("[sarah-test] FAIL ") + name); }
}
using HeadlessDevice = x3::game::HeadlessRenderDevice;

// Spawn one live hostile Guard (box fallback headless) at `at` via a MonsterManager.
} // namespace

bool runCompanionCombatSelfTest() {
    g_cpass = g_cfail = 0;

    std::unique_ptr<x3::phys::IPhysicsWorld> physics(x3::phys::createPhysicsWorld());
    physics->init();
    HeadlessDevice device;

    const x3::phys::Vec3 sarahPos{ 0.0f, 0.4f, 0.0f };
    const x3::phys::Vec3 hostilePos{ 6.0f, 0.4f, 0.0f };   // 6 m away, in engage+fire range

    // ---- C1: RESTRAINED at spawn — no follow, no fire even with a hostile present. ----
    {
        Scene scene;
        SarahCompanion sarah;
        sarah.build(scene, device, *physics, riggedGlbRoot(), sarahPos);
        MonsterManager mm;
        MonsterSystem::Tuning gt; gt.type = MonsterType::Guard; gt.hp = 100; gt.chaseSpeed = 0.0f;
        mm.spawn(scene, device, *physics, riggedGlbRoot(), hostilePos, gt);
        std::vector<MonsterSystem*> hostiles{ &mm.at(0) };
        const int hpBefore = mm.at(0).hp();
        const x3::phys::Vec3 player{ 12.0f, 0.4f, 0.0f };   // far, to tempt a follow
        const x3::phys::Vec3 start = sarah.pos();
        for (int i = 0; i < 120; ++i) sarah.tick(1.0f/60.0f, scene, *physics, player, hostiles);
        const bool stayed = std::abs(sarah.pos().x - start.x) < 1e-3f;
        const bool notShot = (mm.at(0).hp() == hpBefore);
        ccheck(sarah.restrained() && stayed && notShot,
               "C1 restrained: no follow / no fire before onFreed()");
        mm.shutdown();
    }

    // ---- C2: onFreed() WAKES her (Awake + freed), idempotent. ----
    {
        Scene scene;
        SarahCompanion sarah;
        sarah.build(scene, device, *physics, riggedGlbRoot(), sarahPos);
        ccheck(sarah.restrained() && !sarah.freed(), "C2a spawns restrained (not freed)");
        sarah.onFreed();
        ccheck(sarah.awake() && sarah.freed(), "C2b onFreed() -> awake + freed()");
        sarah.onFreed();   // idempotent
        ccheck(sarah.awake(), "C2c onFreed() is idempotent");
    }

    // ---- C3: freed, she ACQUIRES the nearest hostile and FIRES (its HP drops). ----
    {
        Scene scene;
        SarahCompanion sarah;
        sarah.build(scene, device, *physics, riggedGlbRoot(), sarahPos);
        sarah.onFreed();
        MonsterManager mm;
        MonsterSystem::Tuning gt; gt.type = MonsterType::Guard; gt.hp = 100; gt.chaseSpeed = 0.0f;
        mm.spawn(scene, device, *physics, riggedGlbRoot(), hostilePos, gt);
        std::vector<MonsterSystem*> hostiles{ &mm.at(0) };
        const int hpBefore = mm.at(0).hp();
        // Player stands right beside Sarah so she holds position and engages the hostile.
        const x3::phys::Vec3 player{ 0.0f, 0.4f, 1.0f };
        bool acquired = false, everFired = false;
        for (int i = 0; i < 180; ++i) {
            sarah.tick(1.0f/60.0f, scene, *physics, player, hostiles);
            if (sarah.hasTarget()) acquired = true;
            if (sarah.firing())    everFired = true;
        }
        ccheck(acquired && sarah.target() == &mm.at(0), "C3a acquires the nearest hostile");
        ccheck(mm.at(0).hp() < hpBefore, "C3b fires a hitscan — hostile HP drops");
        ccheck(everFired, "C3c plays the aim/fire pose while shooting");
        mm.shutdown();
    }

    // ---- C4: freed, she FOLLOWS the player toward the standoff ring. ----
    {
        Scene scene;
        SarahCompanion sarah;
        sarah.build(scene, device, *physics, riggedGlbRoot(), sarahPos);
        sarah.onFreed();
        std::vector<MonsterSystem*> none;
        const x3::phys::Vec3 player{ 15.0f, 0.4f, 0.0f };
        const float d0 = std::abs(player.x - sarah.pos().x);
        for (int i = 0; i < 180; ++i) sarah.tick(1.0f/60.0f, scene, *physics, player, none);
        const float d1 = std::abs(player.x - sarah.pos().x);
        ccheck(d1 < d0 - 1.0f, "C4 follows the player (closes toward the standoff ring)");
        // She should hold roughly the standoff ring, not pile onto Jake.
        ccheck(d1 >= kSarahFollowStop - 0.6f, "C4b holds a standoff (doesn't crowd Jake)");
    }

    // ---- C5: SEPARATION — spawned ON the player, she is pushed OUT (no stacking). ----
    {
        Scene scene;
        SarahCompanion sarah;
        sarah.build(scene, device, *physics, riggedGlbRoot(), x3::phys::Vec3{ 5.0f, 0.4f, 5.0f });
        sarah.onFreed();
        std::vector<MonsterSystem*> none;
        const x3::phys::Vec3 player{ 5.0f, 0.4f, 5.0f };   // exactly on top of her
        for (int i = 0; i < 120; ++i) sarah.tick(1.0f/60.0f, scene, *physics, player, none);
        const float dx = sarah.pos().x - player.x, dz = sarah.pos().z - player.z;
        const float sep = std::sqrt(dx*dx + dz*dz);
        ccheck(sep > 0.9f, "C5 separation pushes her off the player (no stacking/blocking)");
    }

    // ---- C6: takeDamage to 0 => INCAPACITATED, NOT deleted; stops fighting/following. ----
    {
        Scene scene;
        SarahCompanion sarah;
        sarah.build(scene, device, *physics, riggedGlbRoot(), sarahPos);
        sarah.onFreed();
        const uint32_t sceneSizeBefore = scene.size();
        // Restrained can't be hurt: (nothing here — she's freed). Knock her all the way down.
        bool went = false;
        went |= sarah.takeDamage(50, scene, *physics);
        went |= sarah.takeDamage(50, scene, *physics);
        ccheck(went && sarah.incapacitated() && sarah.hp() == 0, "C6a HP 0 -> incapacitated");
        // NOT deleted: her entity still exists (scene didn't shrink) and still draws.
        const bool present = (scene.size() == sceneSizeBefore) &&
                             (sarah.state() == SarahState::Incapacitated);
        ccheck(present, "C6b incapacitated, NOT deleted (entity persists)");
        // Down => no more firing/following even with a hostile right there.
        MonsterManager mm;
        MonsterSystem::Tuning gt; gt.type = MonsterType::Guard; gt.hp = 100; gt.chaseSpeed = 0.0f;
        mm.spawn(scene, device, *physics, riggedGlbRoot(), hostilePos, gt);
        std::vector<MonsterSystem*> hostiles{ &mm.at(0) };
        const int hpBefore = mm.at(0).hp();
        const x3::phys::Vec3 far{ 40.0f, 0.4f, 0.0f };
        const x3::phys::Vec3 downedAt = sarah.pos();
        for (int i = 0; i < 120; ++i) sarah.tick(1.0f/60.0f, scene, *physics, far, hostiles);
        const bool frozen = std::abs(sarah.pos().x - downedAt.x) < 1e-3f;
        ccheck(frozen && mm.at(0).hp() == hpBefore && !sarah.hasTarget(),
               "C6c downed: no follow, no fire");
        // A further hit is a no-op (already down, not re-killed / not deleted).
        ccheck(!sarah.takeDamage(50, scene, *physics), "C6d further damage is a no-op once down");
        mm.shutdown();
    }

    physics->shutdown();
    x3::logInfo("[sarah-test] " + std::to_string(g_cpass) + " passed, " +
                std::to_string(g_cfail) + " failed");
    return g_cfail == 0;
}

} // namespace x3::game
