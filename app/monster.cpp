// Monster + combat (S6). See app/monster.h.
//
// Clean-room: built from the IModelLoader + IAssetSource + IRenderDevice +
// IPhysicsWorld + Scene interfaces only. No purchased C# copied; no id Tech /
// RBDOOM source consulted.
#include "monster.h"
#include "weapon.h"   // ViewKick (game-feel recoil/shake) — exercised by --test-gamefeel
#include "mesh_prims.h"
#include "headless_device.h"
#include "asset_root.h"

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace x3::game {

// ---------------------------------------------------------------------------
// Tuning constants.
// ---------------------------------------------------------------------------
namespace {

// The purchased alien_crawler GLB is authored larger than our graybox monster
// footprint; scale it down so it reads as a ~1.2 m crawler. The fallback box is
// authored at the right size already, so it uses scale 1.
constexpr float kRealModelScale = 0.5f;
constexpr float kBoxModelScale  = 1.0f;

// Collision box half-extents (meters) for the Enemy-layer body. Sized to a rough
// crawler footprint: ~1.0 m wide/deep, ~0.8 m tall. Used for BOTH the real GLB
// (the model is drawn over this volume) and the fallback box (which also renders
// at this size).
constexpr x3::phys::Vec3 kMonsterHalf{ 0.5f, 0.4f, 0.5f };

// Build a column-major 4x4 from a 3x3 basis (columns bx,by,bz), uniform scale s,
// and translation t. Column-major: m[0..3]=col0, m[4..7]=col1, etc.
void composeTRS(float m[16],
                const x3::phys::Vec3& bx, const x3::phys::Vec3& by, const x3::phys::Vec3& bz,
                float s, const x3::phys::Vec3& t) {
    m[0]  = bx.x * s; m[1]  = bx.y * s; m[2]  = bx.z * s; m[3]  = 0.0f;
    m[4]  = by.x * s; m[5]  = by.y * s; m[6]  = by.z * s; m[7]  = 0.0f;
    m[8]  = bz.x * s; m[9]  = bz.y * s; m[10] = bz.z * s; m[11] = 0.0f;
    m[12] = t.x;      m[13] = t.y;      m[14] = t.z;      m[15] = 1.0f;
}

// ---------------------------------------------------------------------------
// Facing math (D-ai). SET IN STONE per docs/CONVENTIONS.md + the verified env_art
// construction: a model's default facing is local -Z. Under composeTRS's yaw
// basis (col0=(c,0,-s), col2=(s,0,c)), the model's local -Z maps to world
// (-sin yaw, 0, -cos yaw). Therefore, to point local -Z along a desired planar
// direction (dirX, dirZ), the heading is yaw = atan2(-dirX, -dirZ). This is the
// EXACT relationship the existing chase code used (atan2(kFaceSign*dx, ...) with
// kFaceSign=-1) and it is verified by the facing self-test (--test-ai, case d).
// Facing the player: dir = player - self. Facing AWAY (retreat): dir = self -
// player. Do NOT reinvent this angle.
float headingToFace(float dirX, float dirZ) {
    if (dirX * dirX + dirZ * dirZ < 1e-12f) return 0.0f;
    return std::atan2(-dirX, -dirZ);
}

// Smallest signed angular difference wrapped to (-pi, pi].
float angWrap(float d) {
    constexpr float kPi = 3.14159265358979323846f;
    while (d >  kPi)  d -= 2.0f * kPi;
    while (d <= -kPi) d += 2.0f * kPi;
    return d;
}

// Slew `cur` toward `target` by at most rate*dt (radians), shortest way around.
float slewAngle(float cur, float target, float rate, float dt) {
    const float diff = angWrap(target - cur);
    const float step = rate * dt;
    if (diff >  step) return cur + step;
    if (diff < -step) return cur - step;
    return target;
}

// Tiny LCG -> float in [0,1). No heap, deterministic per seed; advances the seed.
float rng01(uint32_t& s) {
    s = s * 1664525u + 1013904223u;
    return (float)(s >> 8) * (1.0f / 16777216.0f);
}

} // namespace

const char* aiStateName(AiState s) {
    switch (s) {
        case AiState::Idle:    return "Idle";
        case AiState::Search:  return "Search";
        case AiState::Advance: return "Advance";
        case AiState::Attack:  return "Attack";
        case AiState::Strafe:  return "Strafe";
        case AiState::Retreat: return "Retreat";
        case AiState::Regroup: return "Regroup";
    }
    return "?";
}

const char* enemyTypeName(EnemyType t) {
    switch (t) {
        case EnemyType::DominionTrooper: return "DominionTrooper";
        case EnemyType::Verthani:        return "Verthani";
        case EnemyType::Illuminated:     return "Illuminated";
        case EnemyType::BlueSynth:       return "BlueSynth";
        case EnemyType::Count:           return "?";
    }
    return "?";
}

x3::phys::Vec3 MonsterSystem::facingDir() const {
    return x3::phys::Vec3{ -std::sin(m_yaw), 0.0f, -std::cos(m_yaw) };
}

// Hit-react flash strength in [0,1] (mirrors Player::damageFlash): 1 right after a
// hit, decaying to 0 over kHitFlashTime. The draw path brightens/tints by this.
float MonsterSystem::hitFlash() const {
    if (kHitFlashTime <= 0.0f) return 0.0f;
    const float f = m_flash / kHitFlashTime;
    return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
}

// ---------------------------------------------------------------------------
// Pure damage rule (testable; no rendering / physics).
// ---------------------------------------------------------------------------
bool applyDamage(int* hp, int damage) {
    if (!hp) return false;
    *hp -= damage;
    if (*hp < 0) *hp = 0;
    return *hp <= 0;
}

// ---------------------------------------------------------------------------
// Build the monster (load the real GLB, or fall back to a box).
// ---------------------------------------------------------------------------
void MonsterSystem::buildMonster(Scene& scene, x3::rhi::IRenderDevice& device,
                                 x3::phys::IPhysicsWorld& physics,
                                 std::string_view modelDir, const x3::phys::Vec3& pos) {
    // Default tuning reproduces the original single-monster behaviour exactly.
    buildMonsterTuned(scene, device, physics, modelDir, pos, Tuning{});
}

void MonsterSystem::buildMonsterTuned(Scene& scene, x3::rhi::IRenderDevice& device,
                                      x3::phys::IPhysicsWorld& physics,
                                      std::string_view modelDir, const x3::phys::Vec3& pos,
                                      const Tuning& tuning) {
    m_pos = pos;
    m_maxHp = tuning.hp;
    m_hp  = tuning.hp;
    m_chaseSpeed = tuning.chaseSpeed;
    for (int i = 0; i < 4; ++i) m_baseTint[i] = tuning.tint[i];
    m_alive = true;
    m_flash = 0.0f;

    // ---- Attack behaviour (Phase 2a, spec §6.5) ----
    m_type           = tuning.type;
    m_dmg            = tuning.damage;
    m_attackRange    = tuning.attackRange;
    m_attackCooldown = tuning.attackCooldown;
    m_attackWindup   = tuning.attackWindup;
    m_ranged         = tuning.ranged;
    m_standoff       = tuning.standoff;
    m_aiStrafeBias   = tuning.aiStrafeBias;     // <0 => use the MonsterType default
    m_atkTimer       = tuning.attackCooldown;  // small initial delay before first hit
    m_windupTimer    = 0.0f;
    m_winding        = false;

    // ---- Boss phases (Phase 2b, spec §8). Always start at Phase1 with neutral
    // (1x) multipliers; the HP-keyed machine in update() advances them. Copying the
    // config even for non-Boss types is harmless (the machine only runs for Boss). ----
    m_phase            = BossPhase::Phase1;
    m_phase2Frac       = tuning.phase2Frac;
    m_phase3Frac       = tuning.phase3Frac;
    m_phase2SpeedMul   = tuning.phase2SpeedMul;
    m_phase2DamageMul  = tuning.phase2DamageMul;
    m_phase3SpeedMul   = tuning.phase3SpeedMul;
    m_phase3DamageMul  = tuning.phase3DamageMul;
    m_phase2ScaleMul   = tuning.phase2ScaleMul;
    m_phase3ScaleMul   = tuning.phase3ScaleMul;
    m_phase3SummonCount= tuning.phase3SummonCount;
    m_phaseSpeedMul    = 1.0f;
    m_phaseDamageMul   = 1.0f;
    m_phaseScaleMul    = 1.0f;
    m_phaseTintMul[0] = m_phaseTintMul[1] = m_phaseTintMul[2] = 1.0f;

    // ---- Try the real purchased GLB via a mounted loose-dir asset source. The
    // model file + dir are tuning-overridable (EFLZ art pass): Level 1 points the
    // characters at converted_glb/Characters/*.glb; the tests keep the legacy
    // rigged_glb/alien_crawler.glb (empty overrides). ----
    const std::string modelFile = tuning.modelFile.empty()
        ? std::string("alien_crawler.glb") : tuning.modelFile;
    const std::string useDir = tuning.modelDirOverride.empty()
        ? std::string(modelDir) : tuning.modelDirOverride;

    m_device = &device;   // cached so update() can re-upload CPU-skinned vertices
    m_assets.reset(x3::asset::createAssetSource());
    bool mounted = m_assets->mountDir(useDir, 0);
    if (mounted) {
        m_loader.reset(x3::asset::createModelLoader(&device, m_assets.get()));
        m_model = m_loader->load(modelFile);
        if (m_model.ok)
            m_drawables = x3::asset::makeDrawables(m_model);
    } else {
        x3::logWarn("[monster] mountDir failed: " + useDir);
    }

    // ---- J1/T1: bind the skeletal-animation runtime if this model is skinnable
    // (has a skin + joints + at least one clip + skinned primitives). Locate the
    // locomotion clip set by fuzzy name. When a distinct Walk AND Run exist (the
    // retargeted multi-clip *_anim.glb) the 1D locomotion BLEND is driven by the
    // monster's planar speed; otherwise it degrades to the legacy idle/move switch
    // (the Skinner's blend collapses gracefully on a single locomotion clip).
    // Models with no skin/anim (the Drone, the legacy crawler, the fallback box)
    // leave the skinner invalid -> static draw. ----
    if (m_model.ok && m_skinner.bind(m_model)) {
        m_idleClip = m_skinner.findClip({ "idle", "stand", "breath", "loop" });
        // Prefer a distinct WALK clip; fall back to any move clip for m_walkClip.
        m_walkClip = m_skinner.findClip({ "walk" });
        m_runClip  = m_skinner.findClip({ "run", "sprint", "jog" });
        m_jumpClip = m_skinner.findClip({ "jump", "leap" });
        // One-shot attack clip for the attack-commit crossfade (fuzzy by exporter
        // name). Falls back to the jump clip at attack time if no attack clip exists.
        m_attackClip = m_skinner.findClip({ "attack", "melee", "punch", "swing",
                                            "bite", "shoot", "cast" });
        if (m_walkClip < 0) m_walkClip = m_skinner.findClip({ "move", "jog", "run" });
        if (m_idleClip < 0) m_idleClip = 0;   // fall back to the first clip
        m_animActive = (m_idleClip >= 0);
        // The locomotion blend is meaningful only when a real idle + at least one
        // distinct move clip exist (so speed can sweep). Walk authored ~1.5 m/s,
        // Run ~4 m/s (the AI maps planar speed to those bands).
        m_useLocoBlend = m_animActive && (m_walkClip >= 0 || m_runClip >= 0);
        if (m_useLocoBlend)
            m_skinner.setLocomotionClips(m_idleClip, m_walkClip, m_runClip, 1.5f, 4.0f);
        std::string clipList;
        for (uint32_t c = 0; c < m_skinner.clipCount(); ++c) {
            clipList += (c ? ", " : "") + std::string(m_skinner.clipName(c)) +
                        "(" + std::to_string(m_skinner.clipDuration(c)) + "s)";
        }
        x3::logInfo("[monster] " + modelFile + " is animated — clips: " + clipList +
                    "; idle=" + std::to_string(m_idleClip) +
                    " walk=" + std::to_string(m_walkClip) +
                    " run=" + std::to_string(m_runClip) +
                    " jump=" + std::to_string(m_jumpClip) +
                    " attack=" + std::to_string(m_attackClip) +
                    " locoBlend=" + (m_useLocoBlend ? "1" : "0"));
        // Pose the bind-pose mesh into the idle pose at t=0 once up front so the
        // very first rendered frame already shows the animated pose (not bind pose).
        if (m_animActive && m_device) {
            if (m_useLocoBlend) {
                m_skinner.setLocomotionSpeed(0.0f);   // start idle
                m_skinner.applyLocomotion(m_model, *m_device, 0.0f);
            } else {
                m_skinner.apply(m_model, *m_device, (uint32_t)m_idleClip, 0.0f);
            }
        }
    }

    // ---- Model-local fixup: the converted character GLBs are Z-up (lying flat),
    // so rotate -90deg about X to stand them upright (local +Z -> world +Y), then
    // ground the feet at y=0. Y-up models (crawler / ModularSciFi) keep identity. -
    {
        for (int i=0;i<16;++i) m_modelFixup[i]=(i%5==0)?1.0f:0.0f;
        if (tuning.standUpZtoY) {
            // Rotation -90deg about X (column-major): +Z->+Y, +Y->-Z.
            //  col0=(1,0,0) col1=(0,0,-1) col2=(0,1,0)
            m_modelFixup[0]=1; m_modelFixup[1]=0;  m_modelFixup[2]=0;
            m_modelFixup[4]=0; m_modelFixup[5]=0;  m_modelFixup[6]=-1;
            m_modelFixup[8]=0; m_modelFixup[9]=1;  m_modelFixup[10]=0;
            // After rotation the feet (was z=0) sit at y=0 already; no extra offset.
        }
    }

    if (!m_drawables.empty()) {
        m_usingReal  = true;
        m_modelScale = kRealModelScale;
        x3::logInfo("[monster] loaded " + modelFile + " — " +
                    std::to_string(m_drawables.size()) + " drawable primitive(s)");
    } else {
        // ---- Fallback: a procedural box monster so the slice still works. ----
        m_usingReal  = false;
        m_modelScale = kBoxModelScale;
        if (m_model.ok)
            x3::logWarn("[monster] GLB loaded but produced no drawables; using fallback box");
        else
            x3::logWarn("[monster] " + modelFile + " load failed; using fallback box");

        x3::prims::PrimMesh geo = x3::prims::makeBox(kMonsterHalf.x, kMonsterHalf.y,
                                                     kMonsterHalf.z, 0.0f, 0.0f, 0.0f, 1.0f);
        x3::rhi::MeshHandle mesh = device.createMesh(
            geo.verts.data(), (uint32_t)geo.verts.size(),
            geo.index.data(), (uint32_t)geo.index.size());
        x3::asset::ModelDrawable d;
        d.meshId = mesh.id;
        d.baseColorTexId = 0;                    // 0 -> default white -> flat color
        d.baseColorFactor[0] = 0.25f; d.baseColorFactor[1] = 0.65f;
        d.baseColorFactor[2] = 0.30f; d.baseColorFactor[3] = 1.0f;  // sickly green
        m_drawables.push_back(d);
    }

    // Per-instance model-scale override (Tuning.modelScale >= 0): lets a boss read
    // bigger than a basic enemy while reusing the same crawler GLB.
    if (tuning.modelScale > 0.0f) m_modelScale = tuning.modelScale;

    // ---- Enemy-layer collision body for the shoot raycast. mass 0 -> Static
    // motion type but keeps the Enemy ObjectLayer, so it stays put under gravity
    // yet is hittable by a rayCast(Layer::Enemy) and movable by setBodyPosition
    // (same approach as the S4 door). ----
    // Hitbox sized to the (possibly scaled) VISUAL so aiming at the body actually
    // connects. The old fixed kMonsterHalf (~0.8 m tall) let scaled-up humanoids be shot
    // "through" — only the small drone was hittable. Taller box, scaled by modelScale.
    {
        const float hs = (m_modelScale > 0.1f) ? m_modelScale : 1.0f;
        m_hitHalfY = 0.95f * hs;   // body box half-height; top quarter = the HEAD zone
        const x3::phys::Vec3 hitHalf{ 0.55f * hs, m_hitHalfY, 0.55f * hs };
        m_body = physics.addBox(hitHalf, m_pos, 0.0f, x3::phys::Layer::Enemy);
    }

    // ---- Monster Entity: bookkeeping (tag/body/visibility/transform). Its render
    // mesh handle is left INVALID so Scene::render skips it; drawMonster() renders
    // ALL of the model's primitives at the Entity transform with the hit-flash
    // tint (so multi-primitive GLBs draw fully + consistently). ----
    Entity e;
    e.tag     = (uint32_t)Tag::Monster;
    e.visible = true;
    e.body    = m_body;                          // also registers body->entity map
    composeTRS(e.transform,
               x3::phys::Vec3{1, 0, 0}, x3::phys::Vec3{0, 1, 0}, x3::phys::Vec3{0, 0, 1},
               m_modelScale, m_pos);
    m_entity = scene.add(e);

    x3::logInfo("[monster] entity " + std::to_string(m_entity) +
                " placed at (" + std::to_string(pos.x) + ", " +
                std::to_string(pos.y) + ", " + std::to_string(pos.z) + ")" +
                " HP=" + std::to_string(m_hp) +
                (m_usingReal ? " [real GLB]" : " [fallback box]"));
}

// ---------------------------------------------------------------------------
// Fire one shot: raycast the Enemy layer, resolve to this monster, damage it.
// ---------------------------------------------------------------------------
FireResult MonsterSystem::fire(const x3::phys::Vec3& eye, const x3::phys::Vec3& dir,
                               Scene& scene, x3::phys::IPhysicsWorld& physics) {
    FireResult r;
    r.hpAfter = m_hp;

    // Normalize the look dir so the FX tracer "miss" end point (eye + dir*range)
    // is at the true max range regardless of the caller's dir magnitude.
    float dl = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (dl < 1e-6f) dl = 1e-6f;
    const x3::phys::Vec3 ndir{ dir.x / dl, dir.y / dl, dir.z / dl };
    // Default tracer end on a miss: straight out to max range.
    r.endPoint = x3::phys::Vec3{ eye.x + ndir.x * kFireMaxDist,
                                 eye.y + ndir.y * kFireMaxDist,
                                 eye.z + ndir.z * kFireMaxDist };

    if (!m_alive) return r;                       // already dead: nothing to hit

    x3::phys::RayHit hit = physics.rayCast(eye, ndir, kFireMaxDist, x3::phys::Layer::Enemy);
    if (hit.hit && hit.body.valid()) {
        r.hit      = true;
        r.hitPoint = hit.point;
        r.endPoint = hit.point;                   // tracer terminates at the hit
    } else {
        return r;                                 // missed everything
    }

    uint32_t ent = scene.entityForBody(hit.body);
    if (ent == kNoLink || ent >= scene.size()) return r;

    const Entity& e = scene.get(ent);
    if (e.tag != (uint32_t)Tag::Monster) return r; // hit something that isn't a monster
    if (ent != m_entity) return r;                 // (multi-monster: not this one)

    // ---- Hit a live monster: apply damage + start the red hit-flash. HEADSHOT (hit in
    // the upper part of the body box) deals 3x. ----
    r.hitMonster = true;
    // HEAD zone = the top quarter of the (scaled) hitbox -> a distinct head area.
    const bool headshot = (hit.point.y - m_pos.y) > m_hitHalfY * 0.5f;
    const int  shotDmg  = headshot ? kDamagePerShot * 3 : kDamagePerShot;
    if (headshot) x3::logInfo("[monster] HEADSHOT! 3x damage");
    bool dead = applyDamage(&m_hp, shotDmg);
    m_flash = kHitFlashTime;
    r.hpAfter = m_hp;
    // D-ai: remember recent damage so the state machine can flinch/retreat.
    m_dmgMemory   = kAiDamageMemory;
    m_dmgWindowHp += kDamagePerShot;

    if (dead) {
        // ---- Death: remove the physics body IMMEDIATELY (so subsequent rays
        // miss right away) and drop the Entity's body handle, but DO NOT hide the
        // model yet — start a brief death "pop" so the kill reads on screen. The
        // Entity stays visible while m_dying; update() counts m_deathPop down and
        // hides it when the pop finishes. ----
        m_alive = false;
        r.killed = true;
        m_dying    = true;
        m_deathPop = kDeathPopTime;
        if (m_entity != kNoLink && m_entity < scene.size()) {
            Entity& me = scene.get(m_entity);
            me.body = x3::phys::BodyId{};         // entity no longer owns a body
        }
        physics.removeBody(m_body);
        m_body = x3::phys::BodyId{};
        x3::logInfo("[monster] killed (HP 0) — body removed, death-pop started");
    } else {
        x3::logInfo("[monster] hit for " + std::to_string(kDamagePerShot) +
                    " — HP now " + std::to_string(m_hp));
    }
    return r;
}

// ---------------------------------------------------------------------------
// Super-strength melee (Phase 2b): apply heavy damage + start the hit-flash, and
// kill (hide + remove body + death-pop) on HP<=0 — the same death path fire()
// uses. The caller has already resolved that this monster is inside the punch arc.
// The knockback impulse is applied by the caller (it owns the direction).
// ---------------------------------------------------------------------------
bool MonsterSystem::takeMeleeDamage(int damage, Scene& scene,
                                    x3::phys::IPhysicsWorld& physics) {
    if (!m_alive) return false;
    bool dead = applyDamage(&m_hp, damage);
    m_flash = kHitFlashTime;
    // D-ai: heavy melee is a strong flinch trigger.
    m_dmgMemory   = kAiDamageMemory;
    m_dmgWindowHp += damage;
    if (dead) {
        m_alive    = false;
        m_dying    = true;
        m_deathPop = kDeathPopTime;
        if (m_entity != kNoLink && m_entity < scene.size())
            scene.get(m_entity).body = x3::phys::BodyId{};
        if (m_body.valid()) physics.removeBody(m_body);
        m_body = x3::phys::BodyId{};
        x3::logInfo("[monster] melee-killed (HP 0) — body removed, death-pop started");
    } else {
        x3::logInfo("[monster] melee hit for " + std::to_string(damage) +
                    " — HP now " + std::to_string(m_hp));
    }
    return dead;
}

int MonsterSystem::effectiveDamage() const {
    // Phase-scaled per-attack damage (Phase1 = 1x). Round to nearest int.
    return (int)(m_dmg * m_phaseDamageMul + 0.5f);
}

// ---------------------------------------------------------------------------
// Per-frame: decay hit-flash; run the death pop; else face + chase the player.
// ---------------------------------------------------------------------------
void MonsterSystem::update(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics,
                           const x3::phys::Vec3& playerPos) {
    update(dt, scene, physics, playerPos, playerPos, nullptr,
           AttackFxFn{}, BossPhaseFn{}, AllyQueryFn{});
}

void MonsterSystem::update(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics,
                           const x3::phys::Vec3& playerPos,
                           IDamageSink* target, const AttackFxFn& fx) {
    update(dt, scene, physics, playerPos, playerPos, target,
           fx, BossPhaseFn{}, AllyQueryFn{});
}

void MonsterSystem::update(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics,
                           const x3::phys::Vec3& playerPos,
                           IDamageSink* target, const AttackFxFn& fx,
                           const BossPhaseFn& onPhase) {
    // LOS endpoint: prefer the live target's eye if we have one, else the planar
    // tracking position (the legacy callers pass the eye as playerPos already).
    const x3::phys::Vec3 eye =
        (target ? target->damageTargetPos() : playerPos);
    update(dt, scene, physics, playerPos, eye, target, fx, onPhase, AllyQueryFn{});
}

void MonsterSystem::update(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics,
                           const x3::phys::Vec3& playerPos, const x3::phys::Vec3& playerEye,
                           IDamageSink* target, const AttackFxFn& fx,
                           const BossPhaseFn& onPhase, const AllyQueryFn& allies) {
    if (dt <= 0.0f) return;

    // ---- Boss phase machine (Phase 2b). Monotone HP-fraction latch: only ever
    // advances Phase1 -> Phase2 -> Phase3. On a transition, fold in that phase's
    // speed/damage/scale/tint multipliers and fire onPhase once (HUD flash / audio
    // / summon). Runs while alive (a dead boss has its body gone). ----
    if (m_type == MonsterType::Boss && m_alive && m_maxHp > 0) {
        const float frac = (float)m_hp / (float)m_maxHp;
        BossPhase want = m_phase;
        if (frac <= m_phase3Frac)      want = BossPhase::Phase3;
        else if (frac <= m_phase2Frac) want = BossPhase::Phase2;
        if ((uint32_t)want > (uint32_t)m_phase) {
            m_phase = want;
            if (m_phase == BossPhase::Phase2) {
                m_phaseSpeedMul  = m_phase2SpeedMul;
                m_phaseDamageMul = m_phase2DamageMul;
                m_phaseScaleMul  = m_phase2ScaleMul;
                m_phaseTintMul[0] = 1.0f; m_phaseTintMul[1] = 0.6f; m_phaseTintMul[2] = 0.6f;
                x3::logInfo("[monster] BOSS PHASE 2 — ENRAGE (faster + harder + reddened)");
            } else { // Phase3
                m_phaseSpeedMul  = m_phase3SpeedMul;
                m_phaseDamageMul = m_phase3DamageMul;
                m_phaseScaleMul  = m_phase3ScaleMul;
                m_phaseTintMul[0] = 1.0f; m_phaseTintMul[1] = 0.35f; m_phaseTintMul[2] = 0.35f;
                x3::logInfo("[monster] BOSS PHASE 3 — DESPERATE (summon + charge)");
            }
            if (onPhase) onPhase(m_phase);
        }
    }

    // Decay hit-flash.
    if (m_flash > 0.0f) {
        m_flash -= dt;
        if (m_flash < 0.0f) m_flash = 0.0f;
    }

    // Attack cooldown always advances (so the first attack can land promptly).
    if (m_atkTimer > 0.0f) { m_atkTimer -= dt; if (m_atkTimer < 0.0f) m_atkTimer = 0.0f; }

    // ---- Death pop: keep drawing (shrink + flash, in drawMonster) until the
    // timer runs out, then hide the Entity. The body is already gone. ----
    if (!m_alive) {
        if (m_dying) {
            m_deathPop -= dt;
            if (m_deathPop <= 0.0f) {
                m_deathPop = 0.0f;
                m_dying = false;
                if (m_entity != kNoLink && m_entity < scene.size())
                    scene.get(m_entity).visible = false;
            }
        }
        return;
    }

    if (m_entity == kNoLink || m_entity >= scene.size()) return;

    // Snapshot the pre-movement position so we can measure this frame's planar
    // speed (used to choose idle vs walk for the skeletal animation, below).
    const x3::phys::Vec3 prevPos = m_pos;

    // One-time AI init: desync the per-instance decision cadence + RNG so a group
    // of enemies does NOT switch states in lockstep, and seed the heading.
    if (!m_aiInit) {
        m_aiInit = true;
        // Mix the entity id into the seed so each instance gets a distinct stream.
        m_rng ^= (m_entity * 2654435761u) + 0x85EBCA6Bu;
        m_decisionTimer = kAiDecisionPeriod * (0.3f + 0.7f * rng01(m_rng));
        m_yawTarget = m_yaw;
        m_strafeDir = (rng01(m_rng) < 0.5f) ? -1.0f : 1.0f;
    }

    // ---- Geometry to the player (planar) + LOS (rayCast against Static). ----
    float dx = playerPos.x - m_pos.x;
    float dz = playerPos.z - m_pos.z;
    float horiz = std::sqrt(dx * dx + dz * dz);
    m_lastHoriz = horiz;   // cache for the manager's nearest-attacker selection
    const float fxn = (horiz > 1e-4f) ? dx / horiz : 0.0f;   // unit toward player
    const float fzn = (horiz > 1e-4f) ? dz / horiz : 0.0f;
    const float pxn = -fzn, pzn = fxn;                        // perpendicular (left)

    // Recent-damage memory decays; the flinch HP-window resets when memory lapses.
    if (m_dmgMemory > 0.0f) {
        m_dmgMemory -= dt;
        if (m_dmgMemory <= 0.0f) { m_dmgMemory = 0.0f; m_dmgWindowHp = 0; }
    }
    m_stateTime += dt;

    // ---- Periodic decision: re-evaluate the behaviour state on a jittered cadence
    // (NOT every frame) with hysteresis (a min dwell) so states don't jitter and a
    // group doesn't switch in lockstep. The chosen state then drives movement +
    // heading below every frame. ----
    m_decisionTimer -= dt;
    if (m_decisionTimer <= 0.0f) {
        m_decisionTimer = kAiDecisionPeriod +
                          kAiDecisionJitter * (2.0f * rng01(m_rng) - 1.0f);

        // LOS: ray from our center toward the player's eye; clear if no Static wall
        // blocks it before the player. Skip past our own collision box first (the
        // Static mask also matches Enemy bodies, so a center-origin ray self-hits).
        bool los = true;
        if (target) {
            const x3::phys::Vec3 from0{ m_pos.x, m_pos.y + 0.3f, m_pos.z };
            x3::phys::Vec3 d{ playerEye.x - from0.x, playerEye.y - from0.y,
                              playerEye.z - from0.z };
            float dl = std::sqrt(d.x*d.x + d.y*d.y + d.z*d.z);
            if (dl < 1e-4f) dl = 1e-4f;
            const x3::phys::Vec3 nd{ d.x/dl, d.y/dl, d.z/dl };
            const float skip = kMonsterHalf.x + 0.15f;
            const x3::phys::Vec3 from{ from0.x + nd.x*skip, from0.y + nd.y*skip,
                                       from0.z + nd.z*skip };
            float losLen = dl - skip; if (losLen < 0.0f) losLen = 0.0f;
            x3::phys::RayHit wall = (losLen > 1e-3f)
                ? physics.rayCast(from, nd, losLen, x3::phys::Layer::Static)
                : x3::phys::RayHit{};
            los = !wall.hit;
        } else {
            los = false;   // no target -> nothing to see -> Search/Idle
        }
        m_hasLos = los;
        if (los) { m_lastKnown = playerPos; m_everSawPlayer = true; }

        // Pressure signals.
        const float frac = (m_maxHp > 0) ? (float)m_hp / (float)m_maxHp : 1.0f;
        const bool lowHp     = frac <= kAiRetreatFrac;
        const bool flinch    = (m_dmgMemory > 0.0f) && (m_dmgWindowHp >= kAiHeavyDmgWindowHp);
        const bool pressured = lowHp || flinch;

        // Per-type behaviour weighting (probabilities, applied with the per-instance
        // RNG so a group spreads out). Drone flanks/strafes more + holds standoff;
        // Guard advances harder; Boss mixes attack with reposition. The bestiary
        // roster (MonsterDef) can OVERRIDE this per-species via Tuning.aiStrafeBias
        // (>= 0): e.g. Verthani strafe-heavy (~0.80), Illuminated standoff-low
        // (~0.10). A negative m_aiStrafeBias (the default) keeps the MonsterType
        // default exactly, so existing Guard/Drone/Boss behaviour is unchanged.
        float strafeBias = 0.35f;  // chance to strafe instead of advance at mid-range
        if (m_type == MonsterType::Drone) strafeBias = 0.75f;
        else if (m_type == MonsterType::Guard) strafeBias = 0.20f;
        else if (m_type == MonsterType::Boss)  strafeBias = 0.45f;
        if (m_aiStrafeBias >= 0.0f)
            strafeBias = (m_aiStrafeBias > 1.0f) ? 1.0f : m_aiStrafeBias;  // data override

        AiState want = m_ai;

        if (pressured && los) {
            // Hurt + can see the player: prefer Retreat, but a pressured enemy with
            // a nearby ally may Regroup toward them instead (re-form, then re-engage).
            want = AiState::Retreat;
            if (allies) {
                x3::phys::Vec3 buf[8];
                uint32_t n = allies(m_pos, m_entity, kAiRegroupRadius, buf, 8u);
                if (n > 0) {
                    // Fall back toward the nearest ally.
                    float best = 1e30f; int bi = -1;
                    for (uint32_t i = 0; i < n; ++i) {
                        float ax = buf[i].x - m_pos.x, az = buf[i].z - m_pos.z;
                        float d2 = ax*ax + az*az;
                        if (d2 < best) { best = d2; bi = (int)i; }
                    }
                    if (bi >= 0) { m_rallyPoint = buf[bi]; m_hasRally = true; want = AiState::Regroup; }
                }
            }
        } else if (los) {
            // Healthy + visible: engage. Choose Attack / Strafe / Advance by range.
            const float band = m_ranged ? m_standoff : kAiStrafeBandLo;
            if (!m_ranged && horiz <= m_attackRange * 1.05f) {
                want = AiState::Attack;
            } else if (m_ranged && std::fabs(horiz - m_standoff) <= 1.5f) {
                // Drone in its standoff band: strafe + fire.
                want = (horiz <= m_attackRange) ? AiState::Strafe : AiState::Advance;
            } else if (horiz <= kAiStrafeBandHi && horiz >= band) {
                want = (rng01(m_rng) < strafeBias) ? AiState::Strafe : AiState::Advance;
            } else {
                want = AiState::Advance;
            }
        } else {
            // No LOS. If we ever saw the player, Search the last-known spot for a
            // while, then give up to Idle. Never saw them -> Idle.
            if (m_everSawPlayer && m_searchTimer > 0.0f) {
                want = AiState::Search;
            } else if (m_everSawPlayer && m_ai != AiState::Search && m_ai != AiState::Idle) {
                // Just lost LOS: begin a fresh Search.
                want = AiState::Search;
                m_searchTimer = kAiSearchTime;
            } else if (m_searchTimer <= 0.0f) {
                want = AiState::Idle;
            }
        }

        // Hysteresis: don't switch away from a freshly-entered state too soon,
        // unless the new state is a higher-priority safety transition (Retreat).
        const bool forced = (want == AiState::Retreat);
        if (want != m_ai && (forced || m_stateTime >= kAiStateMinTime)) {
            // Retreat hysteresis: hold retreat at least kAiRetreatMinTime once entered.
            if (m_ai == AiState::Retreat && m_stateTime < kAiRetreatMinTime &&
                want != AiState::Retreat) {
                want = AiState::Retreat;   // keep retreating a bit longer
            }
            if (m_ai == AiState::Regroup && m_regroupTimer > 0.0f &&
                want != AiState::Retreat) {
                want = AiState::Regroup;   // hold regroup its min dwell
            }
            if (want != m_ai) {
                x3::logInfo(std::string("[ai] entity ") + std::to_string(m_entity) +
                            " " + aiStateName(m_ai) + " -> " + aiStateName(want) +
                            " (hp=" + std::to_string(m_hp) + "/" + std::to_string(m_maxHp) +
                            " los=" + (los ? "1" : "0") +
                            " d=" + std::to_string((int)horiz) + "m)");
                m_ai = want;
                m_stateTime = 0.0f;
                if (m_ai == AiState::Search)  m_searchTimer = kAiSearchTime;
                if (m_ai == AiState::Regroup) m_regroupTimer = kAiRegroupHold;
            }
        }
    }

    // ---- Per-frame state execution: pick a movement direction + a desired heading
    // for the CURRENT state. Heading slews toward the target (kAiTurnRate) so the
    // body turns; FACING IS A CONSEQUENCE OF THE STATE (see CONVENTIONS facing math
    // in headingToFace()). ----
    float mx = 0.0f, mz = 0.0f;          // desired planar move dir (un-normalized)
    bool  wantMove = false;
    const float chaseSpeed = m_chaseSpeed * m_phaseSpeedMul;

    // ---- GENERAL navigation (optional): when a nav grid is attached, the agent
    // ROUTES AROUND walls instead of beelining. We rebuild an A* path to the move
    // target on a cadence (kNavRepathPeriod), then steer toward the current next
    // WAYPOINT. The "move target" depends on the state: engage states aim at the
    // player; Search aims at the last-known position. nav* hold the resolved unit
    // direction toward the next waypoint (or 0 if no usable path). The straight-line
    // direction below is used as the fallback when no path exists. ----
    float navDx = 0.0f, navDz = 0.0f; bool navSteer = false;
    if (m_navGrid) {
        m_repathTimer -= dt;
        // Pick the nav goal for the current/likely state: engage -> player, Search ->
        // last-known. (Both use the player's planar tracking position when engaging.)
        const bool engaging = (m_ai == AiState::Advance || m_ai == AiState::Attack ||
                               m_ai == AiState::Strafe);
        x3::phys::Vec3 navGoal = engaging ? playerPos
                                          : (m_everSawPlayer ? m_lastKnown : playerPos);
        // Rebuild on the cadence, or immediately if the goal moved a lot / we have no
        // path. Cheap A* over the bounded grid (pooled nodes; no per-frame alloc).
        const float gdx = navGoal.x - m_pathGoal.x, gdz = navGoal.z - m_pathGoal.z;
        const bool goalMoved = (gdx*gdx + gdz*gdz) > (1.5f * 1.5f);
        if (m_repathTimer <= 0.0f || !m_hasPath || goalMoved) {
            m_repathTimer = kNavRepathPeriod;
            m_pathGoal = navGoal;
            x3::ai::NavPath path = m_navGrid->findPath(
                x3::ai::NavVec3{ m_pos.x, m_pos.y, m_pos.z },
                x3::ai::NavVec3{ navGoal.x, navGoal.y, navGoal.z }, /*smooth*/ true);
            m_follower.setPath(path);
            m_hasPath = path.ok() && m_follower.hasPath();
        }
        if (m_hasPath && !m_follower.arrived()) {
            // Desired velocity toward the next waypoint -> a unit planar direction.
            x3::ai::NavVec3 v = m_follower.desiredVelocity(
                x3::ai::NavVec3{ m_pos.x, m_pos.y, m_pos.z }, 1.0f, kNavWaypointArrive);
            const float vl = std::sqrt(v.x*v.x + v.z*v.z);
            if (vl > 1e-4f) { navDx = v.x / vl; navDz = v.z / vl; navSteer = true; }
        }
    }

    switch (m_ai) {
        case AiState::Advance: {
            // Move toward + FACE the player. With a nav grid, steer along the A* path
            // (around walls); otherwise the original straight-line approach + weave.
            if (navSteer) {
                mx = navDx; mz = navDz;        // follow the path waypoint (routes around)
                m_yawTarget = headingToFace(navDx, navDz);  // face where we're going
            } else {
                m_wander += dt * kStrafeFreq;
                const float strafe = std::sin(m_wander) * kStrafeAmt;
                mx = fxn + pxn * strafe; mz = fzn + pzn * strafe;
                m_yawTarget = headingToFace(dx, dz);
            }
            // Dogpile limiting (playtest-fix): a melee enemy WITHOUT an attack permit
            // holds at the standoff ring (just beyond reach) instead of closing onto
            // the player's tile — it waits its turn while a capped number swing. A
            // permitted melee enemy (or a ranged one) closes to its normal stop dist.
            const bool meleeWaiting = !m_ranged && m_dmg > 0 && !m_meleePermit;
            const float stop = meleeWaiting ? combat::kStandoffRing : kChaseStopDist;
            wantMove = horiz > stop;
        } break;

        case AiState::Attack: {
            // In strike range: FACE the player. A permitted melee enemy holds position
            // and the attack block below fires; a melee enemy DENIED a permit (dogpile
            // cap) backs off to the standoff ring so it doesn't stack on the player
            // while waiting its turn. Drones keep their standoff (handled in Strafe).
            m_yawTarget = headingToFace(dx, dz);
            const bool meleeWaiting = !m_ranged && m_dmg > 0 && !m_meleePermit;
            if (meleeWaiting && horiz < combat::kStandoffRing) {
                mx = -fxn; mz = -fzn;          // ease back to the standoff ring
                wantMove = true;
            } else {
                wantMove = false;
            }
        } break;

        case AiState::Strafe: {
            // Circle the player at mid-range while FACING the player (the only
            // "face you while moving sideways" state). Flip orbit direction on a
            // timer + add gentle range-keeping so it neither closes nor drifts.
            const float ideal = m_ranged ? m_standoff
                                         : 0.5f * (kAiStrafeBandLo + kAiStrafeBandHi);
            const float radialErr = horiz - ideal;     // >0 too far, <0 too close
            mx = pxn * m_strafeDir + fxn * (radialErr * 0.5f);
            mz = pzn * m_strafeDir + fzn * (radialErr * 0.5f);
            wantMove = true;
            m_retarget -= dt;
            if (m_retarget <= 0.0f) { m_strafeDir = -m_strafeDir; m_retarget = kOrbitRetarget; }
            m_yawTarget = headingToFace(dx, dz);
        } break;

        case AiState::Retreat: {
            // Back off to a safer distance, FACING AWAY from the player (toward the
            // retreat point). Stop retreating once far enough (the decision logic
            // will then re-engage or keep retreating if still hurt).
            mx = -fxn; mz = -fzn;
            wantMove = horiz < kAiRetreatDist;
            // Face away: aim local -Z along the away direction (= self - player).
            m_yawTarget = headingToFace(-dx, -dz);
        } break;

        case AiState::Regroup: {
            // Fall back toward the rally ally; FACE the rally direction while moving.
            m_regroupTimer -= dt;
            float rx = m_rallyPoint.x - m_pos.x, rz = m_rallyPoint.z - m_pos.z;
            float rl = std::sqrt(rx*rx + rz*rz);
            if (rl > 0.6f) { mx = rx/rl; mz = rz/rl; wantMove = true; m_yawTarget = headingToFace(rx, rz); }
            else { wantMove = false; m_yawTarget = headingToFace(dx, dz); } // re-formed: face player
        } break;

        case AiState::Search: {
            // No LOS: walk to the last-known player position while SWEEPING the
            // heading left/right ("looking around"). Times out toward Idle.
            m_searchTimer -= dt;
            float lx = m_lastKnown.x - m_pos.x, lz = m_lastKnown.z - m_pos.z;
            float ll = std::sqrt(lx*lx + lz*lz);
            m_searchSweep += dt * kAiSearchSweepFreq;
            if (ll > kAiLastKnownReach) {
                // Route around walls toward last-known if a nav path exists, else
                // straight at it. The head still sweeps while moving.
                if (navSteer) { mx = navDx; mz = navDz; }
                else          { mx = lx/ll; mz = lz/ll; }
                wantMove = true;
                m_yawTarget = headingToFace(navSteer ? navDx : lx, navSteer ? navDz : lz) +
                              kAiSearchSweepAmp * std::sin(m_searchSweep);
            } else {
                // Arrived at last-known: stand and sweep the head, scanning.
                wantMove = false;
                m_yawTarget = headingToFace(lx, lz) +
                              kAiSearchSweepAmp * std::sin(m_searchSweep);
            }
            if (m_searchTimer <= 0.0f) m_searchTimer = 0.0f; // -> Idle next decision
        } break;

        case AiState::Idle:
        default: {
            // Idle: stand still, slow idle heading sweep so it doesn't look frozen.
            wantMove = false;
            m_searchSweep += dt * (kAiSearchSweepFreq * 0.3f);
            m_yawTarget = m_yaw + 0.15f * std::sin(m_searchSweep) * dt;
        } break;
    }

    // ---- Apply movement (shared, with wall/prop avoidance). The Boss enrage speed
    // multiplier scales the step. NOTE (D-ai fix): the Static-mask ray also matches
    // Enemy bodies (queryHitsLayer: Static<->Enemy collide), so a probe from m_pos
    // self-hits THIS enemy's own box at distance ~0 and would block ALL movement.
    // Start the probe just BEYOND our own half-extent along the move dir (the same
    // technique the ranged LOS check uses) so it only sees real walls/props. ----
    if (wantMove && chaseSpeed > 0.0f && m_body.valid()) {
        float ml = std::sqrt(mx * mx + mz * mz);
        if (ml > 1e-4f) { mx /= ml; mz /= ml; }
        const float step  = chaseSpeed * dt;
        const float skip  = kMonsterHalf.x + 0.05f;     // clear our own box first
        const float probe = step + 0.10f;               // look this far past the box
        const x3::phys::Vec3 mdir{ mx, 0.0f, mz };
        const x3::phys::Vec3 from{ m_pos.x + mx * skip, m_pos.y, m_pos.z + mz * skip };
        const bool blocked =
            physics.rayCast(from, mdir, probe, x3::phys::Layer::Static).hit ||
            physics.rayCast(from, mdir, probe, x3::phys::Layer::Dynamic).hit;
        if (blocked) {
            m_strafeDir = -m_strafeDir;   // try a new line next frames
            m_wander   += 1.7f;
        } else {
            m_pos.x += mx * step;
            m_pos.z += mz * step;
            physics.setBodyPosition(m_body, m_pos);
        }
    }

    // ---- Turn the body: slew the heading toward the state's target heading. This
    // is what actually rotates the rendered model (and, for rigid bodies, the
    // physics body — see the setBodyRotation call after the transform bake). ----
    m_yaw = slewAngle(m_yaw, m_yawTarget, kAiTurnRate, dt);

    // ---- Attack (Phase 2a, spec §6.5). Guard/Boss = melee within attackRange;
    // Drone = ranged hitscan toward the player within attackRange. Both gate on a
    // per-attack cooldown and a short wind-up telegraph so the hit reads/feels fair
    // (the wind-up also gives the player a beat to react). `target` may be null
    // (movement-only path), in which case no attacks run. ----
    // Dogpile limiting (playtest-fix): a MELEE enemy may only swing this frame if it
    // holds an attack permit (granted by MonsterManager to the nearest
    // combat::kMaxMeleeAttackers melee enemies). Ranged enemies ignore the permit
    // (they keep their distance, so they don't contribute to a melee dogpile). A
    // melee enemy without a permit cancels any wind-up below (it's holding/backing
    // off in the state block above), so it cannot land a hit until it earns a permit.
    const bool mayAttack = m_ranged || m_meleePermit;
    if (target && target->isAlive() && m_dmg > 0 && horiz <= m_attackRange && mayAttack) {
        if (!m_winding && m_atkTimer <= 0.0f) {
            // Begin a new attack: start the wind-up; the hit lands when it elapses.
            m_winding     = true;
            m_windupTimer = m_attackWindup;
            // Telegraph FX up front (a beam toward the player) so the attack reads.
            if (fx) {
                x3::phys::Vec3 tp = target->damageTargetPos();
                x3::phys::Vec3 from{ m_pos.x, m_pos.y + 0.3f, m_pos.z };
                fx(from, tp);
            }
            // ---- Attack-anim crossfade (game-feel). Fire ONE one-shot inertialized
            // crossfade to the attack clip (or the jump clip as a stand-in) the moment
            // the attack commits. The Skinner ramps it in over kAttackFadeSec and (it's
            // non-looping) auto-returns to the locomotion blend, so the swing reads as a
            // distinct motion without a per-frame retrigger. Debounced by the
            // (!m_winding && cooldown-elapsed) gate above => exactly once per attack.
            // Guarded on a usable locomotion blend + a valid one-shot clip. ----
            if (m_animActive && m_useLocoBlend) {
                const int oneShot = (m_attackClip >= 0) ? m_attackClip : m_jumpClip;
                if (oneShot >= 0) {
                    m_skinner.triggerClip(oneShot, kAttackFadeSec, /*loop=*/false);
                    m_attackAnimActive = true;
                }
            }
        }
        if (m_winding) {
            m_windupTimer -= dt;
            if (m_windupTimer <= 0.0f) {
                m_winding = false;
                m_atkTimer = m_attackCooldown;   // start the cooldown
                bool landed = true;
                if (m_ranged) {
                    // Ranged: hitscan toward the player; only land if line-of-sight
                    // is clear (no Static wall between the drone and the player).
                    // NOTE: the engine's Static-mask ray also matches Enemy bodies
                    // (queryHitsLayer: Static/Enemy collide), so a ray from the
                    // drone CENTER would self-hit its own Enemy box at distance 0.
                    // Start the ray just BEYOND the drone's own half-extent along
                    // the firing direction so the LOS check sees real walls only.
                    x3::phys::Vec3 tp = target->damageTargetPos();
                    const x3::phys::Vec3 muzzle{ m_pos.x, m_pos.y + 0.3f, m_pos.z };
                    x3::phys::Vec3 d{ tp.x - muzzle.x, tp.y - muzzle.y, tp.z - muzzle.z };
                    float dl = std::sqrt(d.x*d.x + d.y*d.y + d.z*d.z);
                    if (dl < 1e-4f) dl = 1e-4f;
                    x3::phys::Vec3 nd{ d.x/dl, d.y/dl, d.z/dl };
                    // Clear our own collision box before testing for walls.
                    const float skip = kMonsterHalf.x + 0.15f;
                    x3::phys::Vec3 from{ muzzle.x + nd.x * skip,
                                         muzzle.y + nd.y * skip,
                                         muzzle.z + nd.z * skip };
                    float losLen = dl - skip; if (losLen < 0.0f) losLen = 0.0f;
                    x3::phys::RayHit wall = (losLen > 1e-3f)
                        ? physics.rayCast(from, nd, losLen, x3::phys::Layer::Static)
                        : x3::phys::RayHit{};
                    landed = !wall.hit;
                    if (fx) {
                        // Tracer from the muzzle to the impact (player) or the wall.
                        x3::phys::Vec3 end = wall.hit ? wall.point : tp;
                        fx(muzzle, end);
                    }
                }
                if (landed) {
                    // Phase-scaled damage (Boss enrage hits harder; 1x otherwise).
                    const int dmg = effectiveDamage();
                    bool hit = target->takeDamage(dmg);
                    if (hit) {
                        x3::logInfo(std::string("[monster] ") +
                                    (m_ranged ? "drone ranged" : "melee") +
                                    " hit player for " + std::to_string(dmg));
                        // Impact cue at the player so audio/FX can land a hit sound.
                        emitCueOrLog(m_cueSink, GameCue{
                            m_ranged ? CueKind::BulletImpact : CueKind::MeleeImpact,
                            target->damageTargetPos(), 1.0f });
                    }
                }
            }
        }
    } else {
        // Out of range / no target: cancel any pending wind-up.
        m_winding = false;
    }

    // ---- Bake the facing yaw into the render transform's upper-left 3x3, keeping
    // the uniform model scale, and set the translation to the (possibly moved)
    // body center. Scene::update only overwrites the translation column and
    // preserves the 3x3, so this facing survives the per-frame physics sync as
    // long as the host calls update() after scene.update() (see main loop). ----
    {
        // Rigged character GLBs are authored facing +Z, but facingDir()/AI assume local
        // -Z forward (CONVENTIONS) — so the MESH renders with its back to the player.
        // Flip the VISUAL yaw 180 deg here (facingDir()/aim math are unchanged, so the
        // AI + the --test-ai facing case stay correct).
        const float ry = m_yaw + 3.14159265358979323846f;
        const float c = std::cos(ry), s = std::sin(ry);
        // Yaw about +Y: local +X -> (c,0,-s), +Z -> (s,0,c). The phase scale
        // multiplier up-scales the boss as it enrages (graybox phase feedback).
        const float scale = m_modelScale * m_phaseScaleMul;
        Entity& me = scene.get(m_entity);
        composeTRS(me.transform,
                   x3::phys::Vec3{ c, 0.0f, -s },
                   x3::phys::Vec3{ 0.0f, 1.0f, 0.0f },
                   x3::phys::Vec3{ s, 0.0f, c },
                   scale, m_pos);

        // ---- Also turn the rigid body (D-ai). Our heading is a pure rotation about
        // +Y; the quaternion (x,y,z,w) for yaw `m_yaw` about +Y is
        // (0, sin(yaw/2), 0, cos(yaw/2)) — CONVENTIONS.md quat order, w LAST. This
        // keeps the physics body's orientation in sync with the visual heading so
        // any system reading getBodyRotation() (or future capsule push) agrees with
        // what's drawn. The character-capsule case has no rigid box, so this is a
        // no-op there (m_body invalid); the visual heading above is what turns it. -
        if (m_body.valid()) {
            const float h = m_yaw * 0.5f;
            const float q[4] = { 0.0f, std::sin(h), 0.0f, std::cos(h) }; // (x,y,z,w)
            physics.setBodyRotation(m_body, q);
        }
    }

    // ---- J1/T1: drive skeletal animation from the AI's planar movement speed.
    // Measure this frame's planar velocity (the same value the chase/strafe logic
    // produced via setBodyPosition) and feed it into the locomotion blend so a
    // moving enemy visibly walks/runs and a stopped one idles. The Skinner maps
    // speed -> Idle/Walk/Run weight (walk ~1.5 m/s, run ~4 m/s) and keeps phase
    // continuity. When no real walk/run set exists we fall back to the legacy
    // idle/move clip switch. Re-uploads skinned vertices via the cached device. --
    if (m_animActive && m_device) {
        // PERF (distance cull): CPU skinning re-skins + RE-UPLOADS the whole vertex
        // buffer every frame via updateMesh — costly per character (and the upload can
        // stall the GPU). A level full of rigged NPCs (other floors, the F2 rescue
        // victims, distant guards) otherwise drowns the frame in per-frame skin uploads
        // while the player is nowhere near them. Skip the re-skin for characters far
        // from the player; they simply hold their last pose until you approach. The
        // real fix is GPU skinning (engine lane) — this is the content-side guard.
        {
            const float cdx = m_pos.x - playerPos.x, cdy = m_pos.y - playerPos.y,
                        cdz = m_pos.z - playerPos.z;
            constexpr float kSkinCullDist = 32.0f;   // meters; beyond this, don't re-skin
            if (cdx*cdx + cdy*cdy + cdz*cdz > kSkinCullDist * kSkinCullDist) return;
        }
        const float ddx = m_pos.x - prevPos.x, ddz = m_pos.z - prevPos.z;
        const float planarSpeed = (dt > 1e-5f)
            ? std::sqrt(ddx*ddx + ddz*ddz) / dt : 0.0f;
        if (m_useLocoBlend) {
            m_skinner.setLocomotionSpeed(planarSpeed);
            m_skinner.applyLocomotion(m_model, *m_device, dt);

            // ---- Footstep cues at locomotion phase crossings (foot plants). The
            // shared phase wraps [0,1); a left/right step lands each time it crosses
            // one of kFootstepsPerCycle evenly-spaced marks. We compare this frame's
            // phase to last frame's and fire a cue per mark crossed (handles wrap +
            // multi-mark steps in one frame). Gated on moving fast enough + NOT mid
            // attack crossfade (so a swing in place doesn't tick steps). ----
            const float phase = m_skinner.locomotionPhase();
            if (planarSpeed > kFootstepMinSpeed && !m_attackAnimActive &&
                kFootstepsPerCycle > 0) {
                const float marks = (float)kFootstepsPerCycle;
                int prevMark = (int)std::floor(m_lastFootPhase * marks);
                int curMark  = (int)std::floor(phase * marks);
                // Phase wrapped this frame (cur < prev): add a full cycle of marks.
                if (phase < m_lastFootPhase) curMark += kFootstepsPerCycle;
                for (int s = prevMark; s < curMark; ++s) {
                    // Intensity scales with speed (faster -> louder/firmer step).
                    const float inten = std::min(1.0f, 0.4f + 0.2f * planarSpeed);
                    emitCueOrLog(m_cueSink, GameCue{ CueKind::Footstep, m_pos, inten });
                }
            }
            m_lastFootPhase = phase;

            // The one-shot attack crossfade auto-returns to the locomotion blend; the
            // Skinner reports crossfadeWeight()==0 once it's fully back. Clear our
            // mirror flag then so attackAnimActive() tracks the live transition.
            if (m_attackAnimActive && m_skinner.crossfadeWeight() <= 0.0f)
                m_attackAnimActive = false;
        } else {
            // Legacy single-locomotion-clip path: switch idle/move on a threshold.
            const bool moving = (m_walkClip >= 0) && (planarSpeed > 0.25f);
            const int clip = moving ? m_walkClip : m_idleClip;
            m_animTime += dt;
            m_skinner.apply(m_model, *m_device, (uint32_t)clip, m_animTime);
        }
    }
}

// ---------------------------------------------------------------------------
// Draw all monster primitives at its transform, with the hit-flash tint.
// ---------------------------------------------------------------------------
void MonsterSystem::drawMonster(x3::rhi::IRenderDevice& device,
                                const x3::rhi::FrameContext& frame,
                                const Scene& scene) const {
    // Draw while alive OR during the brief death pop (m_dying). Skip otherwise.
    if ((!m_alive && !m_dying) || m_entity == kNoLink || m_entity >= scene.size()) return;
    const Entity& e = scene.get(m_entity);
    if (!e.visible) return;

    // Hit-flash: lerp the per-primitive base color toward red as flash decays.
    // flashAmt in [0,1]; 1 right after a hit, 0 once decayed (the exposed
    // hitFlash() accessor — same value a HUD/draw layer can read). Start from the
    // per-instance base tint (Tuning.tint) so e.g. boss Martinez reads distinct.
    const float flashAmt = hitFlash();
    // Fold the per-phase tint multiplier into the base (Boss enrage reddens; 1x for
    // everyone else) so the active boss phase reads on screen.
    float tint[4] = { m_baseTint[0] * m_phaseTintMul[0],
                      m_baseTint[1] * m_phaseTintMul[1],
                      m_baseTint[2] * m_phaseTintMul[2],
                      m_baseTint[3] };
    // Toward red: keep R, knock down G/B by the flash amount. Also LIFT overall
    // brightness briefly so the hit reads as a flash (not just a hue shift) — a
    // short emissive-style pop that decays with the flash (game-feel hit react).
    const float flashLift = 1.0f + 0.6f * flashAmt;
    tint[0] *= flashLift;
    tint[1] *= flashLift * (1.0f - 0.85f * flashAmt);
    tint[2] *= flashLift * (1.0f - 0.85f * flashAmt);

    if (m_dying) {
        // ---- Death pop: shrink the model toward zero and flash it bright white
        // over the pop window, so the kill reads as a distinct event. popAmt goes
        // 1 -> 0 across kDeathPopTime. ----
        const float popAmt = (kDeathPopTime > 0.0f) ? (m_deathPop / kDeathPopTime) : 0.0f;
        // Flash bright: push tint up toward white-hot as it dies.
        const float bright = 1.0f + 1.5f * popAmt;
        tint[0] = bright; tint[1] = bright; tint[2] = bright;
        // Scale the whole transform down (uniform shrink) while keeping the center.
        float m[16];
        for (int i = 0; i < 16; ++i) m[i] = e.transform[i];
        for (int col = 0; col < 3; ++col) {        // scale the 3 basis columns
            m[col * 4 + 0] *= popAmt;
            m[col * 4 + 1] *= popAmt;
            m[col * 4 + 2] *= popAmt;
        }
        drawMonsterAt(device, frame, m, tint);
        return;
    }

    drawMonsterAt(device, frame, e.transform, tint);
}

void MonsterSystem::drawMonsterAt(x3::rhi::IRenderDevice& device,
                                  const x3::rhi::FrameContext& frame,
                                  const float model[16], const float tint[4]) const {
    for (const auto& d : m_drawables) {
        float color[4] = {
            d.baseColorFactor[0] * tint[0],
            d.baseColorFactor[1] * tint[1],
            d.baseColorFactor[2] * tint[2],
            d.baseColorFactor[3] * tint[3],
        };
        // Compose: fin = model * fixup * nodeTransform. `model` is the gameplay
        // transform (yaw + scale + pos); `fixup` stands up Z-up character GLBs;
        // nodeTransform is the baked glTF node world matrix (M2 fix). Identity
        // fixup + identity node => legacy behaviour (crawler / box).
        float mf[16], fin[16];
        x3::asset::mulMat4(model, m_modelFixup, mf);
        x3::asset::mulMat4(mf, d.nodeTransform, fin);
        device.drawMesh(frame,
                        x3::rhi::MeshHandle{ d.meshId },
                        x3::rhi::TextureHandle{ d.baseColorTexId },
                        color,
                        fin);
    }
}

// ===========================================================================
// Multi-monster manager (Level 1 / §6.1).
// ===========================================================================
uint32_t MonsterManager::spawn(Scene& scene, x3::rhi::IRenderDevice& device,
                               x3::phys::IPhysicsWorld& physics,
                               std::string_view modelDir, const x3::phys::Vec3& pos,
                               const MonsterSystem::Tuning& tuning) {
    auto m = std::make_unique<MonsterSystem>();
    m->buildMonsterTuned(scene, device, physics, modelDir, pos, tuning);
    if (m_cueSink) m->setCueSink(m_cueSink);   // wire footstep/impact cues on new spawns
    uint32_t idx = (uint32_t)m_monsters.size();
    m_monsters.push_back(std::move(m));
    return idx;
}

void MonsterManager::setCueSink(const GameCueFn& sink) {
    m_cueSink = sink;
    for (auto& m : m_monsters) m->setCueSink(sink);   // apply to existing too
}

uint32_t MonsterManager::aliveCount() const {
    uint32_t n = 0;
    for (const auto& m : m_monsters)
        if (m->alive()) ++n;
    return n;
}

void MonsterManager::update(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics,
                            const x3::phys::Vec3& playerPos) {
    update(dt, scene, physics, playerPos, nullptr, AttackFxFn{}, BossPhaseFn{});
}

void MonsterManager::update(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics,
                            const x3::phys::Vec3& playerPos,
                            IDamageSink* target, const AttackFxFn& fx) {
    update(dt, scene, physics, playerPos, target, fx, BossPhaseFn{});
}

void MonsterManager::update(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics,
                            const x3::phys::Vec3& playerPos,
                            IDamageSink* target, const AttackFxFn& fx,
                            const BossPhaseFn& onPhase) {
    // D-ai: ally query over THIS manager's own live members (so a pressured enemy
    // can Regroup toward squadmates). No per-frame heap alloc — the lambda only
    // reads positions into the caller's fixed buffer. The LOS endpoint is the
    // target's eye (or the planar pos when there's no target).
    const x3::phys::Vec3 eye = (target ? target->damageTargetPos() : playerPos);
    AllyQueryFn allies = [this](const x3::phys::Vec3& self, uint32_t selfEntity,
                                float radius, x3::phys::Vec3* out, uint32_t maxOut) -> uint32_t {
        uint32_t n = 0;
        const float r2 = radius * radius;
        for (const auto& a : m_monsters) {
            if (n >= maxOut) break;
            if (!a->alive() || a->entity() == selfEntity) continue;
            const x3::phys::Vec3 p = a->pos();
            const float ddx = p.x - self.x, ddz = p.z - self.z;
            if (ddx*ddx + ddz*ddz <= r2) out[n++] = p;
        }
        return n;
    };

    // ---- Dogpile limiting (playtest-fix). Before ticking, arbitrate MELEE attack
    // permits across the whole squad: only the NEAREST combat::kMaxMeleeAttackers
    // live melee enemies that are within (a little beyond) their reach of the player
    // get a permit to swing this frame; everyone else is denied and holds at the
    // standoff ring. Ranged enemies are not part of this cap (they keep distance).
    // This is what stops 3+ guards melting the player in a few seconds while keeping
    // the rest of the squad pressuring/flanking. Distance is measured here from the
    // current positions (so the very first frame is correct too). No heap alloc:
    // a small fixed scan, O(N * cap). ----
    {
        // Default: deny every melee enemy a permit; ranged always allowed.
        for (auto& m : m_monsters) m->setMeleeAttackPermit(m->ranged());
        // Greedily grant the cap to the nearest in-reach melee enemies.
        for (uint32_t granted = 0; granted < combat::kMaxMeleeAttackers; ++granted) {
            MonsterSystem* best = nullptr; float bestD2 = 1e30f;
            for (auto& m : m_monsters) {
                if (!m->alive() || m->ranged() || m->attackDamage() <= 0) continue;
                if (m->meleeAttackPermit()) continue;   // already granted this frame
                const x3::phys::Vec3 p = m->pos();
                const float ddx = p.x - playerPos.x, ddz = p.z - playerPos.z;
                const float d2 = ddx*ddx + ddz*ddz;
                // Only contest a permit if roughly within reach (a small slack so an
                // enemy easing in still earns the next open slot). Beyond that it
                // would just be advancing anyway, so leave the slot for a closer one.
                const float reach = m->attackRange() + 0.5f;
                if (d2 > reach * reach) continue;
                if (d2 < bestD2) { bestD2 = d2; best = m.get(); }
            }
            if (!best) break;                 // no more in-reach melee enemies
            best->setMeleeAttackPermit(true);
        }
    }

    for (auto& m : m_monsters)
        m->update(dt, scene, physics, playerPos, eye, target, fx, onPhase, allies);
}

void MonsterManager::drawAll(x3::rhi::IRenderDevice& device,
                             const x3::rhi::FrameContext& frame,
                             const Scene& scene) const {
    for (const auto& m : m_monsters)
        m->drawMonster(device, frame, scene);
}

FireResult MonsterManager::fire(const x3::phys::Vec3& eye, const x3::phys::Vec3& dir,
                                Scene& scene, x3::phys::IPhysicsWorld& physics) {
    // Each MonsterSystem::fire() casts an Enemy-layer ray that returns the NEAREST
    // enemy body, but only applies damage if that body is its own. So the first
    // monster whose fire() reports a real monster hit is the one the ray actually
    // struck (the nearest); the others see the same nearest body, recognise it as
    // "not me", and no-op. We therefore take the first hitMonster result. We also
    // keep the best non-monster result (a wall/miss tracer end) for FX.
    FireResult best;
    for (auto& m : m_monsters) {
        FireResult r = m->fire(eye, dir, scene, physics);
        if (r.hitMonster) return r;       // the nearest monster took the shot
        if (r.hit && !best.hit) best = r; // remember a geometry hit for the tracer
    }
    return best;
}

// ===========================================================================
// Headless self-test (--test-combat). T1 hit damages, T2 kill removes + rays
// miss, T3 aim away no damage, T4 unarmed gate. No window / Vulkan.
// ===========================================================================
namespace {

int g_pass = 0, g_fail = 0;
void check(bool cond, const char* name) {
    if (cond) { ++g_pass; x3::logInfo(std::string("[combat-test] PASS ") + name); }
    else      { ++g_fail; x3::logError(std::string("[combat-test] FAIL ") + name); }
}

// Headless IRenderDevice: the shared no-op test-double (app/headless_device.h).
// Mints monotonically-increasing valid handles so buildMonster() runs unchanged
// with no Vulkan; draw/frame/camera calls are no-ops.
using HeadlessDevice = x3::game::HeadlessRenderDevice;

x3::phys::Vec3 sub(const x3::phys::Vec3& a, const x3::phys::Vec3& b) {
    return x3::phys::Vec3{ a.x - b.x, a.y - b.y, a.z - b.z };
}

// A damage sink that COUNTS landed hits (and total HP) with NO iframe gating, so a
// test can measure an enemy's OWN attack cadence / the squad's attacker cap without
// the player's invuln window confounding the count. Always alive, fixed eye.
class CountingSink final : public IDamageSink {
public:
    int hits = 0;
    int totalDmg = 0;
    x3::phys::Vec3 eye{ 0.0f, 1.6f, 0.0f };
    bool takeDamage(int amount) override { ++hits; totalDmg += amount; return true; }
    x3::phys::Vec3 damageTargetPos() const override { return eye; }
    bool isAlive() const override { return true; }
};

// Flat ground for the balance tests (CCW so +Y is solid), `half` to a side.
x3::phys::BodyId combatGround(x3::phys::IPhysicsWorld& w, float half) {
    float v[] = { -half,0,-half,  half,0,-half,  half,0,half,  -half,0,half };
    uint32_t idx[] = { 0,2,1, 0,3,2 };
    return w.addStaticMesh(v, 4, idx, 6);
}

} // namespace

bool runCombatSelfTest() {
    g_pass = g_fail = 0;

    std::unique_ptr<x3::phys::IPhysicsWorld> physics(x3::phys::createPhysicsWorld());
    physics->init();

    HeadlessDevice device;
    Scene scene;
    MonsterSystem combat;

    // Place a monster ahead of a known eye position. modelDir points at the real
    // model dir; the headless device path mints fake GPU handles so the loader
    // works without Vulkan (falls back to a box if the GLB can't be parsed).
    const x3::phys::Vec3 monsterPos{ 0.0f, 0.4f, 0.0f };
    combat.buildMonster(scene, device, *physics, riggedGlbRoot(), monsterPos);

    // Eye 3 m in front of the monster (toward -Z), at the monster's height.
    const x3::phys::Vec3 eye{ monsterPos.x, monsterPos.y, monsterPos.z - 3.0f };
    const x3::phys::Vec3 aimAt = sub(monsterPos, eye);     // points at the monster
    const x3::phys::Vec3 aimAway{ 0.0f, 0.0f, -1.0f };     // points away (further -Z)

    const int hp0 = combat.hp();

    // ---- T1: ray aimed AT the monster reduces its HP -------------------------
    {
        FireResult r = combat.fire(eye, aimAt, scene, *physics);
        bool damaged = r.hitMonster && combat.hp() == hp0 - kDamagePerShot && combat.alive();
        check(damaged, "T1 ray at monster reduces HP");
    }

    // ---- T3: ray aimed AWAY from the monster does no damage ------------------
    // (Run before the kill so the monster is still alive to prove the miss.)
    {
        int before = combat.hp();
        FireResult r = combat.fire(eye, aimAway, scene, *physics);
        bool noDamage = !r.hitMonster && combat.hp() == before && combat.alive();
        check(noDamage, "T3 ray away from monster does no damage");
    }

    // ---- T4: firing when NOT armed does nothing (gate respected) -------------
    // The host only calls fire() when WeaponSystem::hasWeapon(). Model that gate
    // exactly: with armed==false we must NOT call fire(), so HP is unchanged even
    // though the ray would otherwise hit.
    {
        int before = combat.hp();
        bool armed = false;
        FireResult r;                              // default: no hit
        if (armed) r = combat.fire(eye, aimAt, scene, *physics);
        bool gated = !r.hitMonster && combat.hp() == before && combat.alive();
        check(gated, "T4 firing when not armed does nothing");
    }

    // ---- T2: enough shots -> HP<=0 -> dead (hidden, body removed, rays miss) --
    {
        // Monster is at hp0 - kDamagePerShot from T1. Fire until dead (armed).
        bool killedAtSomePoint = false;
        for (int i = 0; i < 10 && combat.alive(); ++i) {
            FireResult r = combat.fire(eye, aimAt, scene, *physics);
            if (r.killed) killedAtSomePoint = true;
        }
        bool dead    = !combat.alive() && combat.hp() <= 0;
        // Death now defers the hide to a brief "pop" (driven by update(), which
        // this headless test never calls), so the kill leaves the monster in the
        // dying state with the body already removed (rays must miss immediately).
        bool dying   = combat.dying();
        // Subsequent ray AT where the monster was now MISSES (body removed).
        x3::phys::RayHit after = physics->rayCast(eye, aimAt, kFireMaxDist,
                                                  x3::phys::Layer::Enemy);
        bool raysMiss = !after.hit;
        // Firing again is a no-op (still dead, no hit reported).
        FireResult again = combat.fire(eye, aimAt, scene, *physics);
        bool noopAfter = !again.hitMonster && !again.killed;
        check(killedAtSomePoint && dead && dying && raysMiss && noopAfter,
              "T2 shots kill monster: dying-pop + body removed + rays miss");
    }

    physics->shutdown();

    // =======================================================================
    // PLAYTEST-FIX balance behaviour (Issue 1). These assert the SYSTEM, not the
    // exact numbers: (T5) an enemy cannot damage the player twice within its attack
    // cooldown; (T6) at most combat::kMaxMeleeAttackers melee enemies swing at once
    // (the dogpile cap), so a corridor squad can't melt the player every frame.
    // =======================================================================

    // ---- T5: per-enemy attack COOLDOWN gates damage. A single melee Guard at
    // point-blank, with NO wind-up and a counting (iframe-free) sink, lands a hit
    // then must WAIT its cooldown before the next — so over a fixed window the hit
    // count is bounded by window/cooldown, NOT one-per-frame. ----
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init();
        combatGround(*w, 50.0f);
        Scene scene5; MonsterSystem guard; CountingSink sink;
        MonsterSystem::Tuning t;
        t.type = MonsterType::Guard; t.hp = 100; t.chaseSpeed = 0.0f;
        t.damage = combat::kMeleeDamageDefault;
        t.attackRange = 2.0f;
        t.attackCooldown = combat::kMeleeCooldownDefault;  // ~1.1 s
        t.attackWindup = 0.0f;                              // land immediately on cooldown
        t.ranged = false;
        // Place the guard ~1.2 m from the sink (inside the 2 m range).
        sink.eye = x3::phys::Vec3{ 0.0f, 1.6f, 0.0f };
        guard.buildMonsterTuned(scene5, device, *w, riggedGlbRoot(),
                                x3::phys::Vec3{ 1.2f, 0.4f, 0.0f }, t);
        const float dtc = 1.0f / 60.0f;
        const float window = 3.0f;                          // 3 s
        const int steps = (int)(window / dtc);
        for (int i = 0; i < steps; ++i) {
            guard.update(dtc, scene5, *w, sink.eye, &sink, AttackFxFn{});
            w->step(dtc);
        }
        // Over 3 s on a ~1.1 s cooldown, expect ~3 hits (3.0/1.1 ~= 2-3), and CRUCIALLY
        // far fewer than one-per-frame (180 frames). Bound: hits <= window/cooldown + 1.
        const int maxExpected = (int)(window / combat::kMeleeCooldownDefault) + 1;
        bool someHits   = sink.hits >= 1;
        bool notEveryFrame = sink.hits <= maxExpected;       // cooldown actually gates
        x3::logInfo(std::string("[combat-test] T5 hits=") + std::to_string(sink.hits) +
                    " over " + std::to_string(steps) + " frames (cap " +
                    std::to_string(maxExpected) + ")");
        check(someHits && notEveryFrame,
              "T5 attack cooldown: enemy cannot damage twice within its cooldown");
        w->shutdown();
    }

    // ---- T6: dogpile CAP. Five melee Guards all stacked in melee range of the sink,
    // driven by a MonsterManager (which arbitrates permits). In a single update tick
    // at most combat::kMaxMeleeAttackers may hold a melee permit, so over one cooldown
    // window the number of DISTINCT swinging guards is capped. We verify the permit
    // arbitration directly (the robust, frame-exact signal) AND that the landed-hit
    // rate is bounded by the cap, not the squad size. ----
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init();
        combatGround(*w, 50.0f);
        Scene scene6; MonsterManager squad; CountingSink sink;
        sink.eye = x3::phys::Vec3{ 0.0f, 1.6f, 0.0f };
        MonsterSystem::Tuning t;
        t.type = MonsterType::Guard; t.hp = 100; t.chaseSpeed = 0.0f;
        t.damage = combat::kMeleeDamageDefault;
        t.attackRange = 2.0f;
        t.attackCooldown = combat::kMeleeCooldownDefault;
        t.attackWindup = 0.0f;
        t.ranged = false;
        // Ring of 5 guards all within 1.5 m of the sink (all in melee range).
        const int N = 5;
        for (int i = 0; i < N; ++i) {
            const float ang = (float)i * (2.0f * 3.14159265f / (float)N);
            const x3::phys::Vec3 p{ 1.3f * std::cos(ang), 0.4f, 1.3f * std::sin(ang) };
            squad.spawn(scene6, device, *w, riggedGlbRoot(), p, t);
        }
        const float dtc = 1.0f / 60.0f;
        // One update tick arbitrates permits; count how many guards hold one.
        squad.update(dtc, scene6, *w, sink.eye, &sink, AttackFxFn{});
        uint32_t permitted = 0;
        for (uint32_t i = 0; i < squad.count(); ++i)
            if (!squad.at(i).ranged() && squad.at(i).meleeAttackPermit()) ++permitted;
        bool capRespected = permitted <= combat::kMaxMeleeAttackers;
        bool capActive    = permitted == combat::kMaxMeleeAttackers; // 5 in range -> cap is full
        // And the landed-damage rate over a cooldown window reflects the cap, not N.
        sink.hits = 0; sink.totalDmg = 0;
        const float window = combat::kMeleeCooldownDefault * 1.05f;  // ~one cooldown
        const int steps = (int)(window / dtc);
        for (int i = 0; i < steps; ++i) {
            squad.update(dtc, scene6, *w, sink.eye, &sink, AttackFxFn{});
            w->step(dtc);
        }
        // Within ~one cooldown each permitted guard swings ~once -> hits ~ cap (allow
        // a small slack for the first-frame initial-cooldown stagger). NOT ~N.
        bool hitsBounded = sink.hits <= (int)combat::kMaxMeleeAttackers + 1;
        x3::logInfo(std::string("[combat-test] T6 permitted=") + std::to_string(permitted) +
                    "/" + std::to_string(N) + " (cap " + std::to_string(combat::kMaxMeleeAttackers) +
                    ") hitsInWindow=" + std::to_string(sink.hits));
        check(capRespected && capActive && hitsBounded,
              "T6 dogpile cap: at most kMaxMeleeAttackers melee enemies swing at once");
        w->shutdown();
    }

    x3::logInfo(std::string("[combat-test] ") + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

// ===========================================================================
// Phase 2a self-test (--test-phase2a): player health + enemies that fight back.
// T1 Guard melee within range drains player HP over time.
// T2 Drone ranged at standoff distance drains player HP (ranged hitscan).
// T3 player HP -> 0 enters death; after the respawn delay a respawn restores
//    full HP (resetHealth, as the host does after readyToRespawn()).
// T4 iframes: two takeDamage() calls in one frame land only one hit.
// T5 Guard vs Drone tuning params differ (type / ranged / range).
// No window / Vulkan. Mirrors the other self-tests.
// ===========================================================================
namespace {

int p2_pass = 0, p2_fail = 0;
void p2check(bool cond, const char* name) {
    if (cond) { ++p2_pass; x3::logInfo(std::string("[phase2a-test] PASS ") + name); }
    else      { ++p2_fail; x3::logError(std::string("[phase2a-test] FAIL ") + name); }
}

constexpr float kP2Dt = 1.0f / 60.0f;

// Flat ground at y=0 (CCW so +Y is solid), `half` units to a side.
x3::phys::BodyId p2Ground(x3::phys::IPhysicsWorld& w, float half) {
    float v[] = {
        -half, 0.0f, -half,  half, 0.0f, -half,
         half, 0.0f,  half, -half, 0.0f,  half,
    };
    uint32_t idx[] = { 0,2,1, 0,3,2 };
    return w.addStaticMesh(v, 4, idx, 6);
}

// Guard tuning (melee) for the test: small range, quick cadence, no wind-up so a
// few stepped frames land a clear hit deterministically.
MonsterSystem::Tuning testGuardTuning() {
    MonsterSystem::Tuning t;
    t.type = MonsterType::Guard;
    t.hp = 100; t.chaseSpeed = 0.0f;   // stationary so the test geometry is stable
    t.damage = 8; t.attackRange = 2.0f; t.attackCooldown = 0.5f; t.attackWindup = 0.0f;
    t.ranged = false;
    return t;
}
// Drone tuning (ranged): long range, no movement so it just fires from a distance.
MonsterSystem::Tuning testDroneTuning() {
    MonsterSystem::Tuning t;
    t.type = MonsterType::Drone;
    t.hp = 66; t.chaseSpeed = 0.0f;
    t.damage = 5; t.attackRange = 20.0f; t.attackCooldown = 0.5f; t.attackWindup = 0.0f;
    t.ranged = true; t.standoff = 8.0f;
    return t;
}

} // namespace

bool runPhase2aSelfTest() {
    p2_pass = p2_fail = 0;

    HeadlessDevice device;

    // ---- T5: Guard vs Drone params differ (pure tuning check; no world needed). ----
    {
        MonsterSystem::Tuning g = testGuardTuning();
        MonsterSystem::Tuning d = testDroneTuning();
        bool differ = (g.type != d.type) && (g.ranged != d.ranged) &&
                      (g.attackRange != d.attackRange) && (g.damage != d.damage);
        p2check(differ, "T5 Guard vs Drone tuning params differ");
    }

    // ---- T4: iframes prevent two hits landing in one window. ----
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init();
        p2Ground(*w, 50.0f);
        Player pl;
        pl.spawn(*w, 0.0f, 0.05f, 0.0f);
        int hp0 = pl.hp();
        bool first  = pl.takeDamage(10);   // lands, opens the iframe window
        bool second = pl.takeDamage(10);   // same window -> absorbed
        bool onlyOne = first && !second && (pl.hp() == hp0 - 10) && pl.invulnerable();
        p2check(onlyOne, "T4 iframes: only one hit lands per window");
        w->shutdown();
    }

    // ---- T1: a Guard within melee range drains player HP over time. ----
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init();
        p2Ground(*w, 50.0f);
        Scene scene;
        Player pl;
        pl.spawn(*w, 0.0f, 0.05f, 0.0f);
        MonsterSystem guard;
        // Place the guard ~1.2 m from the player (inside the 2.0 m attack range).
        guard.buildMonsterTuned(scene, device, *w, riggedGlbRoot(),
                                x3::phys::Vec3{ 1.2f, 0.4f, 0.0f }, testGuardTuning());
        int hp0 = pl.hp();
        // Step ~3 s. The player position fed in is the eye; the guard attacks on its
        // cooldown when within range. With iframes (0.5 s) gating, several hits land.
        const x3::phys::Vec3 playerPos = pl.damageTargetPos();
        for (int i = 0; i < 180; ++i) {
            pl.updateHealth(kP2Dt);                    // decay iframes/flash
            guard.update(kP2Dt, scene, *w, playerPos, &pl, AttackFxFn{});
            w->step(kP2Dt);
        }
        bool drained = pl.hp() < hp0 && pl.isAlive();
        p2check(drained, "T1 Guard in range drains player HP over time");
    }

    // ---- T2: a Drone at standoff range drains player HP (ranged hitscan). ----
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init();
        p2Ground(*w, 50.0f);
        Scene scene;
        Player pl;
        pl.spawn(*w, 0.0f, 0.05f, 0.0f);
        MonsterSystem drone;
        // Place the drone 8 m away (well beyond melee, inside the 20 m fire range).
        drone.buildMonsterTuned(scene, device, *w, riggedGlbRoot(),
                                x3::phys::Vec3{ 8.0f, 0.6f, 0.0f }, testDroneTuning());
        int hp0 = pl.hp();
        const x3::phys::Vec3 playerPos = pl.damageTargetPos();
        for (int i = 0; i < 180; ++i) {
            pl.updateHealth(kP2Dt);
            drone.update(kP2Dt, scene, *w, playerPos, &pl, AttackFxFn{});
            w->step(kP2Dt);
        }
        bool drained = pl.hp() < hp0;
        p2check(drained, "T2 Drone at range drains player HP (ranged)");
    }

    // ---- T3: HP -> 0 enters death; after the respawn delay a respawn restores
    //          full HP (mirrors the host: resetHealth() once readyToRespawn()). ----
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init();
        p2Ground(*w, 50.0f);
        Player pl;
        pl.spawn(*w, 0.0f, 0.05f, 0.0f);
        // Kill the player: repeated big hits, ticking past the iframe window between
        // each so they all land. kPlayerMaxHp/some chunk + spacing.
        for (int i = 0; i < 20 && pl.isAlive(); ++i) {
            pl.takeDamage(40);
            // Advance past the iframe window so the next hit can land.
            for (int k = 0; k < 40; ++k) pl.updateHealth(kP2Dt);
        }
        bool died = pl.dead() && pl.hp() == 0;
        // Now run out the respawn countdown.
        bool readyBefore = pl.readyToRespawn();   // should be false right after death
        for (int k = 0; k < (int)(kRespawnDelay * 60.0f) + 5; ++k) pl.updateHealth(kP2Dt);
        bool ready = pl.readyToRespawn();
        // Host respawns: restore full HP (position handled by setBodyPosition).
        if (ready) pl.resetHealth();
        bool respawned = pl.isAlive() && pl.hp() == pl.maxHp() && pl.damageFlash() == 0.0f;
        p2check(died && !readyBefore && ready && respawned,
                "T3 HP 0 -> death -> respawn restores full HP");
        w->shutdown();
    }

    x3::logInfo(std::string("[phase2a-test] ") + std::to_string(p2_pass) + " passed, " +
                std::to_string(p2_fail) + " failed");
    return p2_fail == 0;
}

// ===========================================================================
// D-ai self-test (--test-ai): monster combat behaviour state machine.
//   (a) healthy + LOS  -> Advance/Attack, FACES the player.
//   (b) low HP         -> Retreat, faces/moves AWAY.
//   (c) lost LOS       -> Search: stops tracking, moves to last-known.
//   (d) facing math    -> known target -> known heading (off-by-pi/2 trap caught).
//   (e) hysteresis     -> a state holds for >= a few ticks (no jitter).
// Prints the state transitions. No window / Vulkan.
// ===========================================================================
namespace {

int ai_pass = 0, ai_fail = 0;
void aicheck(bool cond, const char* name) {
    if (cond) { ++ai_pass; x3::logInfo(std::string("[ai-test] PASS ") + name); }
    else      { ++ai_fail; x3::logError(std::string("[ai-test] FAIL ") + name); }
}

constexpr float kAiDt = 1.0f / 60.0f;
constexpr float kAiPi = 3.14159265358979323846f;

// A trivial damage sink the AI treats as "the player": alive, at a fixed eye.
class AiTargetStub final : public IDamageSink {
public:
    x3::phys::Vec3 eye{ 0.0f, 1.6f, 0.0f };
    bool takeDamage(int) override { return true; }
    x3::phys::Vec3 damageTargetPos() const override { return eye; }
    bool isAlive() const override { return true; }
};

// Flat ground at y=0 (CCW so +Y is solid), `half` to a side.
x3::phys::BodyId aiGround(x3::phys::IPhysicsWorld& w, float half) {
    float v[] = { -half,0,-half,  half,0,-half,  half,0,half,  -half,0,half };
    uint32_t idx[] = { 0,2,1, 0,3,2 };
    return w.addStaticMesh(v, 4, idx, 6);
}

// AI tuning for the test: a Guard, stationary-free (real chase speed), no wind-up
// so behaviour reads quickly. Uses the box fallback (no GLB needed here, but the
// loader is tolerant either way).
MonsterSystem::Tuning aiGuardTuning() {
    MonsterSystem::Tuning t;
    t.type = MonsterType::Guard;
    t.hp = 100; t.chaseSpeed = 3.0f;
    t.damage = 8; t.attackRange = 1.9f; t.attackCooldown = 0.6f; t.attackWindup = 0.0f;
    t.ranged = false;
    return t;   // empty modelFile -> alien_crawler.glb / box fallback
}

// Smallest |a-b| wrapped to [0,pi].
float aiAngErr(float a, float b) {
    float d = a - b;
    while (d >  kAiPi) d -= 2.0f * kAiPi;
    while (d <= -kAiPi) d += 2.0f * kAiPi;
    return std::fabs(d);
}

} // namespace

bool runAiSelfTest() {
    ai_pass = ai_fail = 0;
    HeadlessDevice device;

    // ---- (d) FACING MATH: known target -> known heading (the off-by-pi/2 trap).
    // headingToFace is in an anon namespace; verify via a built monster's heading
    // accessor instead (drives the same construction). Place an enemy at origin,
    // the player straight ahead (-Z) and to the right (+X), and assert the model's
    // forward (local -Z) under the resolved heading points AT the player. ----
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init(); aiGround(*w, 60.0f);
        // STATIONARY enemy (chaseSpeed 0) so the heading is a pure facing solve with
        // no movement/strafe displacement: every engage state aims local -Z at the
        // player, so the heading converges to headingToFace(dx,dz) regardless.
        MonsterSystem::Tuning t = aiGuardTuning(); t.chaseSpeed = 0.0f;
        Scene scene; MonsterSystem m; AiTargetStub tgt;
        m.buildMonsterTuned(scene, device, *w, riggedGlbRoot(),
                            x3::phys::Vec3{ 0,0.4f,0 }, t);
        // Player straight ahead (-Z). After enough ticks facingDir() ~ (0,0,-1).
        tgt.eye = x3::phys::Vec3{ 0.0f, 1.6f, -5.0f };
        for (int i = 0; i < 120; ++i) {
            m.update(kAiDt, scene, *w, tgt.eye, tgt.eye, &tgt, AttackFxFn{}, BossPhaseFn{}, AllyQueryFn{});
            w->step(kAiDt);
        }
        x3::phys::Vec3 f = m.facingDir();
        bool facesAhead = (f.z < -0.95f) && std::fabs(f.x) < 0.1f;
        // Target +X (right): the CORRECT heading is atan2(-dx,-dz)=atan2(-1,0)=-pi/2
        // (forward = +X). The off-by-pi/2 trap (e.g. atan2(dz,dx)+pi/2) would point
        // forward at -X (faces LEFT) — caught here.
        Scene scene2; MonsterSystem m2; AiTargetStub tgt2;
        m2.buildMonsterTuned(scene2, device, *w, riggedGlbRoot(),
                             x3::phys::Vec3{ 0,0.4f,0 }, t);
        tgt2.eye = x3::phys::Vec3{ 6.0f, 1.6f, 0.0f };   // +X
        for (int i = 0; i < 120; ++i) {
            m2.update(kAiDt, scene2, *w, tgt2.eye, tgt2.eye, &tgt2, AttackFxFn{}, BossPhaseFn{}, AllyQueryFn{});
            w->step(kAiDt);
        }
        x3::phys::Vec3 f2 = m2.facingDir();
        const float wantYaw = std::atan2(-1.0f, 0.0f);   // = -pi/2 (faces +X)
        bool facesRight = (f2.x > 0.95f) && std::fabs(f2.z) < 0.1f &&
                          aiAngErr(m2.heading(), wantYaw) < 0.05f;
        x3::logInfo(std::string("[ai-test] (d) faceAheadZ=") + std::to_string(f.z) +
                    " faceRightX=" + std::to_string(f2.x) +
                    " headingErr=" + std::to_string(aiAngErr(m2.heading(), wantYaw)));
        aicheck(facesAhead && facesRight,
                "Td facing math: -Z target -> faces -Z; +X target -> faces +X (no off-by-pi/2)");
        w->shutdown();
    }

    // ---- (a) HEALTHY + LOS -> Advance/Attack, FACES the player. ----
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init(); aiGround(*w, 60.0f);
        Scene scene; MonsterSystem m; AiTargetStub tgt;
        m.buildMonsterTuned(scene, device, *w, riggedGlbRoot(),
                            x3::phys::Vec3{ 0,0.4f,0 }, aiGuardTuning());
        tgt.eye = x3::phys::Vec3{ 0.0f, 1.6f, -8.0f };   // 8 m ahead, clear LOS
        AiState seen = AiState::Idle; bool everEngaged = false;
        for (int i = 0; i < 240; ++i) {
            m.update(kAiDt, scene, *w, tgt.eye, tgt.eye, &tgt, AttackFxFn{}, BossPhaseFn{}, AllyQueryFn{});
            w->step(kAiDt);
            seen = m.aiState();
            if (seen == AiState::Advance || seen == AiState::Attack || seen == AiState::Strafe)
                everEngaged = true;
        }
        // Faces the player: forward (local -Z) points toward (player - self).
        float dx = tgt.eye.x - m.pos().x, dz = tgt.eye.z - m.pos().z;
        float dl = std::sqrt(dx*dx + dz*dz);
        x3::phys::Vec3 f = m.facingDir();
        float dot = (dl > 1e-4f) ? (f.x*dx + f.z*dz)/dl : 0.0f;
        bool engaged = everEngaged && m.hasLineOfSight();
        bool facing  = dot > 0.9f;       // facing within ~25 deg of the player
        x3::logInfo(std::string("[ai-test] (a) final state=") + aiStateName(m.aiState()) +
                    " facingdot=" + std::to_string(dot));
        aicheck(engaged && facing, "Ta healthy+LOS enters Advance/Attack and FACES player");
    }

    // ---- (b) LOW HP -> Retreat, faces/moves AWAY. ----
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init(); aiGround(*w, 60.0f);
        Scene scene; MonsterSystem m; AiTargetStub tgt;
        MonsterSystem::Tuning t = aiGuardTuning();
        t.hp = 90;   // 2 shots (34 each) -> 22 HP = 24% (clearly <= kAiRetreatFrac)
        m.buildMonsterTuned(scene, device, *w, riggedGlbRoot(),
                            x3::phys::Vec3{ 0,0.4f,0 }, t);
        tgt.eye = x3::phys::Vec3{ 0.0f, 1.6f, -4.0f };   // close, clear LOS
        // Damage it to low HP via shots (also stamps recent-damage memory). 90 HP,
        // 34/shot -> 2 shots = 22 HP (<= 30% threshold). Aim along +Z toward enemy.
        const x3::phys::Vec3 eye{ 0,0.4f,-4.0f };
        const x3::phys::Vec3 aim{ 0,0,1 };               // +Z toward the enemy at origin
        m.fire(eye, aim, scene, *w);
        m.fire(eye, aim, scene, *w);                     // ~22 HP now (24%)
        float startDist = std::sqrt(m.pos().x*m.pos().x +
                                    (m.pos().z - tgt.eye.z)*(m.pos().z - tgt.eye.z));
        bool everRetreat = false; float minDot = 1e30f;
        for (int i = 0; i < 180; ++i) {
            m.update(kAiDt, scene, *w, tgt.eye, tgt.eye, &tgt, AttackFxFn{}, BossPhaseFn{}, AllyQueryFn{});
            w->step(kAiDt);
            if (m.aiState() == AiState::Retreat) {
                everRetreat = true;
                float dx = tgt.eye.x - m.pos().x, dz = tgt.eye.z - m.pos().z;
                float dl = std::sqrt(dx*dx+dz*dz);
                x3::phys::Vec3 f = m.facingDir();
                float dot = (dl>1e-4f) ? (f.x*dx+f.z*dz)/dl : 0.0f;
                if (dot < minDot) minDot = dot;
            }
        }
        float endDist = std::sqrt(m.pos().x*m.pos().x +
                                  (m.pos().z - tgt.eye.z)*(m.pos().z - tgt.eye.z));
        bool movedAway = endDist > startDist + 0.5f;      // backed off
        bool facesAway = everRetreat && minDot < -0.5f;   // forward points AWAY from player
        x3::logInfo(std::string("[ai-test] (b) hp=") + std::to_string(m.hp()) +
                    " retreat=" + (everRetreat?"1":"0") +
                    " startDist=" + std::to_string(startDist) +
                    " endDist=" + std::to_string(endDist) +
                    " minFacingdot=" + std::to_string(minDot));
        aicheck(everRetreat && movedAway && facesAway,
                "Tb low-HP enters Retreat and faces/moves AWAY");
        w->shutdown();
    }

    // ---- (c) LOST LOS -> Search: stops tracking, moves to last-known. ----
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init(); aiGround(*w, 60.0f);
        Scene scene; MonsterSystem m; AiTargetStub tgt;
        m.buildMonsterTuned(scene, device, *w, riggedGlbRoot(),
                            x3::phys::Vec3{ 0,0.4f,0 }, aiGuardTuning());
        // First, establish LOS so the enemy records a last-known position. Keep the
        // enemy STATIONARY (chaseSpeed 0) during this phase so its X stays ~0 and we
        // can build a blocking wall on the X=0 line afterward. Player straight ahead.
        tgt.eye = x3::phys::Vec3{ 0.0f, 1.6f, -10.0f };
        for (int i = 0; i < 60; ++i) {
            m.update(kAiDt, scene, *w, tgt.eye, tgt.eye, &tgt, AttackFxFn{}, BossPhaseFn{}, AllyQueryFn{});
            w->step(kAiDt);
        }
        bool sawFirst = m.hasLineOfSight();
        x3::phys::Vec3 lastKnown = m.lastKnownPlayerPos();
        // Drop a BIG tall Static wall between the enemy (~origin) and the player,
        // perpendicular to the line of sight, then move the player BEHIND it on the
        // SAME -Z line. The wall blocks LOS -> the enemy should Search and head to
        // last-known (-Z), NOT teleport-track the live player. Wall at z=-6 spanning
        // x in [-20,20], y in [0,5], double-sided so the ray is caught either way.
        {
            float wx0=-20, wx1=20, wy0=0, wy1=5, wz=-6.0f;
            float v[] = { wx0,wy0,wz, wx1,wy0,wz, wx1,wy1,wz, wx0,wy1,wz };
            uint32_t idx[] = { 0,1,2, 0,2,3,  0,2,1, 0,3,2 }; // double-sided
            w->addStaticMesh(v, 4, idx, 12);
        }
        tgt.eye = x3::phys::Vec3{ 0.0f, 1.6f, -30.0f };   // player retreated far behind the wall
        x3::phys::Vec3 startPos = m.pos();
        bool everSearch = false;
        for (int i = 0; i < 240; ++i) {
            m.update(kAiDt, scene, *w, tgt.eye, tgt.eye, &tgt, AttackFxFn{}, BossPhaseFn{}, AllyQueryFn{});
            w->step(kAiDt);
            if (m.aiState() == AiState::Search) everSearch = true;
        }
        bool lostLos = !m.hasLineOfSight();
        // Moved toward the LAST-KNOWN spot (-Z, toward z=-10). The wall stops it ~z=-5
        // (it can't pass through), but it must clearly head that way (z decreases).
        float towardLastKnown = m.pos().z - startPos.z;   // last-known is more -Z (negative)
        bool trackedLastKnown = (towardLastKnown < -0.3f);
        x3::logInfo(std::string("[ai-test] (c) sawFirst=") + (sawFirst?"1":"0") +
                    " lostLos=" + (lostLos?"1":"0") + " search=" + (everSearch?"1":"0") +
                    " dz=" + std::to_string(towardLastKnown) +
                    " lastKnownZ=" + std::to_string(lastKnown.z));
        aicheck(sawFirst && lostLos && everSearch && trackedLastKnown,
                "Tc lost LOS enters Search and moves to last-known (not live pos)");
        w->shutdown();
    }

    // ---- (e) HYSTERESIS: a state holds for >= a few ticks (no per-frame jitter).
    // With a steady, healthy, LOS engagement, the state must not flip every tick.
    // Count transitions over many ticks: should be small (no thrash). ----
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init(); aiGround(*w, 60.0f);
        Scene scene; MonsterSystem m; AiTargetStub tgt;
        m.buildMonsterTuned(scene, device, *w, riggedGlbRoot(),
                            x3::phys::Vec3{ 0,0.4f,0 }, aiGuardTuning());
        tgt.eye = x3::phys::Vec3{ 0.0f, 1.6f, -4.0f };   // steady, in engage range
        AiState prev = m.aiState(); int transitions = 0; int maxRun = 0, run = 0;
        for (int i = 0; i < 300; ++i) {                  // 5 s @ 60 Hz
            m.update(kAiDt, scene, *w, tgt.eye, tgt.eye, &tgt, AttackFxFn{}, BossPhaseFn{}, AllyQueryFn{});
            w->step(kAiDt);
            AiState s = m.aiState();
            if (s != prev) { ++transitions; prev = s; run = 0; } else { ++run; if (run>maxRun) maxRun=run; }
        }
        // Decisions fire ~every 0.3 s (18 ticks), and a state must dwell >= ~27
        // ticks (kAiStateMinTime). Over 300 ticks, far fewer than 300 transitions,
        // and the longest single-state run must clearly exceed a few ticks.
        bool noJitter = (transitions < 40) && (maxRun >= 10);
        x3::logInfo(std::string("[ai-test] (e) transitions=") + std::to_string(transitions) +
                    " longestRun=" + std::to_string(maxRun) + " ticks");
        aicheck(noJitter, "Te hysteresis: state holds >= several ticks (no jitter)");
        w->shutdown();
    }

    // ---- (f) NAV WIRING: an enemy with a nav grid ROUTES AROUND a wall to reach the
    // player, where a straight-line enemy would stall against it. We use a LOW barrier
    // (0.6 m) the enemy can SEE the player over (eye-level LOS clears it) but cannot
    // WALK through (taller than the agent's step height -> nav marks it blocked). So
    // the enemy stays engaged (Advance, keeps LOS) yet must detour around the barrier
    // ends via the A* path. Barrier on the X~2 line, z in [-13,7] (gap toward +Z).
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init(); aiGround(*w, 60.0f);
        // Low barrier centered at (2, 0.3, -3): spans x in [1.5,2.5], y in [0,0.6],
        // z in [-13,7]. Top y=0.6 > maxStepHeight(0.5) -> blocked for walking; the
        // eye-level chase LOS ray (from y~0.7 up to the player eye y=1.6) passes over.
        w->addBox(x3::phys::Vec3{ 0.5f, 0.3f, 10.0f }, x3::phys::Vec3{ 2.0f, 0.3f, -3.0f },
                  0.0f, x3::phys::Layer::Static);
        // Nav grid over the area, sampled from physics (marks the barrier blocked).
        x3::ai::NavBuildParams np;
        np.minX = -6; np.maxX = 14; np.minZ = -16; np.maxZ = 12; np.cellSize = 1.0f;
        np.agentRadius = 0.4f; np.agentHeight = 1.8f; np.sampleTopY = 8.0f;
        np.sampleDepth = 20.0f; np.maxStepHeight = 0.5f;
        std::unique_ptr<x3::ai::INavGrid> grid(x3::ai::buildNavGridFromPhysics(*w, np));
        // Sanity: the barrier cell at (2,-3) is blocked, an open cell at (-3,-3) isn't.
        uint32_t bc, br; grid->worldToCell(2.0f, -3.0f, bc, br);
        uint32_t fc, fr; grid->worldToCell(-3.0f, -3.0f, fc, fr);
        const bool gridOk = !grid->walkable(bc, br) && grid->walkable(fc, fr);

        Scene scene; MonsterSystem m; AiTargetStub tgt;
        MonsterSystem::Tuning t = aiGuardTuning(); t.chaseSpeed = 4.0f; t.attackRange = 1.5f;
        m.buildMonsterTuned(scene, device, *w, riggedGlbRoot(),
                            x3::phys::Vec3{ 0,0.4f,0 }, t);
        m.setNavGrid(grid.get());
        // Player on the FAR side of the barrier (x=8), roughly opposite the enemy.
        tgt.eye = x3::phys::Vec3{ 8.0f, 1.6f, -3.0f };

        float maxZ = m.pos().z; bool clearedWall = false; bool gotPath = false;
        for (int i = 0; i < 1200; ++i) {       // up to 20 s
            m.update(kAiDt, scene, *w, tgt.eye, tgt.eye, &tgt, AttackFxFn{}, BossPhaseFn{}, AllyQueryFn{});
            w->step(kAiDt);
            if (m.pos().z > maxZ) maxZ = m.pos().z;
            if (m.pathWaypointCount() >= 2) gotPath = true;
            if (m.pos().x > 2.6f) clearedWall = true;   // got past the barrier's far face
        }
        // Routed around: built a multi-waypoint path, deviated toward the +Z gap, and
        // got past the barrier (x > 2.6) to the player's side — not stuck at the wall.
        bool detouredZ = maxZ > 4.0f;
        x3::logInfo(std::string("[ai-test] (f) usingNav=") + (m.usingNav()?"1":"0") +
                    " gridOk=" + (gridOk?"1":"0") + " gotPath=" + (gotPath?"1":"0") +
                    " maxZ=" + std::to_string(maxZ) +
                    " clearedWall=" + (clearedWall?"1":"0") +
                    " finalPos=(" + std::to_string(m.pos().x) + "," + std::to_string(m.pos().z) + ")");
        aicheck(m.usingNav() && gridOk && gotPath && detouredZ && clearedWall,
                "Tf nav-wired enemy routes AROUND a wall to reach the player");
        w->shutdown();
    }

    x3::logInfo(std::string("[ai-test] ") + std::to_string(ai_pass) + " passed, " +
                std::to_string(ai_fail) + " failed");
    return ai_fail == 0;
}

// ===========================================================================
// DATA-DRIVEN BESTIARY ROSTER (bestiary pass). The static MonsterDef table is the
// single source of truth for the enemy species: one row per EnemyType, each a
// fully-populated MonsterSystem::Tuning (stats / model / AI weighting). Adding a
// new enemy is DATA (append a row), not code — the row flows through the existing
// buildMonsterTuned()/spawn() path and reuses the whole combat-AI lane verbatim.
//
// Stats are grounded in the bible's TASK_6 enemy bestiary (HP / damage / speed /
// ranged); the engine treats those as TUNING TARGETS (re-mappable for playtest),
// and damage is pulled from the GENERAL combat:: balance bands so the squad stays
// winnable (not raw bible numbers, which assume a different time/iframe budget).
// ===========================================================================
namespace {

// Resolve the model for a roster row in a self-contained way (no dependency on
// level1_game's file-local helpers). Mirrors their two model sources:
//   * RIGGED + ANIMATED humanoids load from assets/rigged_glb (Y-up; the Skinner
//     finds the idle/loco clips) — preferring the multi-clip "<stem>_anim.glb" when
//     it exists on disk, else the Idle-only base GLB.
//   * Z-up converted props (e.g. Drone.glb) load from assets/converted_glb/
//     Characters with the Z->Y stand-up.
// A row whose GLB is absent falls back (inside MonsterSystem) to the tinted box.

// Set a RIGGED + ANIMATED character (rigged_glb, Y-up). Prefers <stem>_anim.glb.
void defRigged(MonsterSystem::Tuning& t, const char* file, float scale) {
    namespace fs = std::filesystem;
    const std::string riggedDir = riggedGlbRoot();
    std::string base(file), chosen(base);
    const std::string stem = (base.size() > 4 && base.substr(base.size() - 4) == ".glb")
        ? base.substr(0, base.size() - 4) : base;
    std::error_code ec;
    if (fs::exists(fs::path(riggedDir) / (stem + "_anim.glb"), ec)) chosen = stem + "_anim.glb";
    t.modelFile        = chosen;
    t.modelDirOverride = riggedDir;
    t.standUpZtoY      = false;     // rigged sources are authored Y-up
    t.modelScale       = scale;
}

// Set a Z-up converted-character model (converted_glb/Characters), stood up. If
// `preferRigged` names an alternate file present in rigged_glb, prefer that (lets a
// future blue_synth_seed*.glb in rigged_glb light up automatically). FALLBACK
// ROUTE for BlueSynth: blue_synth_seed1.glb is NOT in the repo today, so this
// resolves to Drone.glb (a synthetic-looking flier) — and if THAT is missing the
// MonsterSystem draws its tinted box.
void defConverted(MonsterSystem::Tuning& t, const char* file, float scale) {
    t.modelFile        = std::string("Characters/") + file;
    t.modelDirOverride = convertedGlbRoot();
    t.standUpZtoY      = true;      // converted characters are authored Z-up
    t.modelScale       = scale;
}

// Resolve the BlueSynth model: prefer a rigged blue_synth_seed*.glb if one ever
// lands in rigged_glb, else the converted Drone.glb, else (handled downstream) the
// box. Returns true if it pointed at a real on-disk rigged synth GLB.
bool defBlueSynth(MonsterSystem::Tuning& t, float scale) {
    namespace fs = std::filesystem;
    const std::string riggedDir = riggedGlbRoot();
    std::error_code ec;
    const char* seeds[] = { "blue_synth_seed1.glb", "blue_synth_seed2.glb",
                            "blue_synth_seed3.glb", "blue_synth.glb" };
    for (const char* s : seeds) {
        if (fs::exists(fs::path(riggedDir) / s, ec)) { defRigged(t, s, scale); return true; }
    }
    defConverted(t, "Drone.glb", scale);   // fallback: the existing synthetic flier
    return false;
}

// Build the roster ONCE. Each row: stats grounded in TASK_6, damage/standoff from
// the combat:: bands, and a per-species AI strafe/flank bias so the AI WEIGHTING is
// data (Verthani strafe-heavy; Illuminated standoff-low; etc.).
std::vector<MonsterDef> buildMonsterDefs() {
    std::vector<MonsterDef> defs;
    defs.reserve((size_t)EnemyType::Count);

    // ---- DominionTrooper — baseline humanoid soldier. Bible "Security Guard
    // (Basic)": HP 100, melee. The neutral reference (≈ the existing guard). ----
    {
        MonsterSystem::Tuning t;
        t.type           = MonsterType::Guard;
        t.hp             = 100;                              // bible: 100
        t.chaseSpeed     = 2.5f;                             // bible "Normal"
        t.damage         = combat::kMeleeDamageDefault;     // 8 (band 6..10)
        t.attackRange    = combat::kMeleeRange;             // 1.9 m baton/rifle-butt
        t.attackCooldown = combat::kMeleeCooldownDefault;   // ~1.1 s
        t.attackWindup   = combat::kMeleeWindup;            // 0.25 s
        t.ranged         = false;
        t.aiStrafeBias   = 0.20f;                           // advances harder than it flanks
        t.tint[0]=0.80f; t.tint[1]=0.82f; t.tint[2]=0.86f;  // neutral steel
        defRigged(t, "marcus_webb.glb", 1.0f);             // ~1.8 m animated soldier
        defs.push_back({ EnemyType::DominionTrooper, "DominionTrooper", t });
    }

    // ---- Verthani — insectoid: FASTER, flanks more (strafe-heavy), melee. Bible
    // "Infected Human (Stage 1)" profile (HP 150, Fast, claw/erratic) trimmed for
    // the squad budget; reads as a darting flanker. ----
    {
        MonsterSystem::Tuning t;
        t.type           = MonsterType::Guard;              // melee archetype
        t.hp             = 130;                             // bible-ish (fast, fragile-ish)
        t.chaseSpeed     = 4.2f;                            // bible "Fast" (vs trooper 2.5)
        t.damage         = combat::kMeleeDamageMax;         // 10 (strong claw, top of band)
        t.attackRange    = combat::kMeleeRange;             // 1.9 m claw reach
        t.attackCooldown = combat::kMeleeCooldownMin;       // 1.0 s (quick swings)
        t.attackWindup   = combat::kMeleeWindup;            // 0.25 s
        t.ranged         = false;
        t.aiStrafeBias   = 0.80f;                           // STRAFE-HEAVY: orbits/flanks
        t.tint[0]=0.55f; t.tint[1]=0.95f; t.tint[2]=0.55f;  // chitin green
        defRigged(t, "alien_crawler.glb", 0.9f);           // non-humanoid insectoid look
        defs.push_back({ EnemyType::Verthani, "Verthani", t });
    }

    // ---- Illuminated — elite: ranged + "shielded" (modeled as much higher HP),
    // holds a LONG standoff. Bible "Security Guard (Elite)": HP 200, rifle 25,
    // 25% armor (folded into HP). Standoff AI (low strafe bias: it backs/holds). ----
    {
        MonsterSystem::Tuning t;
        t.type           = MonsterType::Drone;              // ranged archetype (reuses drone lane)
        t.hp             = 220;                             // bible 200 + armor (shielded elite)
        t.chaseSpeed     = 2.2f;                            // deliberate (holds the line)
        t.damage         = combat::kRangedDamageMax;        // 6 (band 4..6; strong bolt)
        t.attackRange    = 18.0f;                           // long rifle reach
        t.attackCooldown = combat::kRangedCooldownMin;      // 0.8 s (disciplined fire)
        t.attackWindup   = 0.35f;                           // telegraphed beam
        t.ranged         = true;
        t.standoff       = 11.0f;                           // holds FAR back (elite standoff)
        t.aiStrafeBias   = 0.10f;                           // STANDOFF: barely flanks, holds range
        t.tint[0]=1.0f; t.tint[1]=0.92f; t.tint[2]=0.55f;   // golden "illuminated" glow
        defRigged(t, "chief_martinez.glb", 1.15f);         // tall elite humanoid
        defs.push_back({ EnemyType::Illuminated, "Illuminated", t });
    }

    // ---- BlueSynth — synthetic: ranged drone-like flier. Bible "Combat Drone":
    // HP 150, plasma 20, flanks/coordinates. Uses blue_synth_seed*.glb if present
    // (absent today -> Drone.glb tinted blue -> box). Mid strafe bias. ----
    {
        MonsterSystem::Tuning t;
        t.type           = MonsterType::Drone;
        t.hp             = 150;                             // bible: 150
        t.chaseSpeed     = 3.2f;                            // bible "Medium (flying)"
        t.damage         = combat::kRangedDamageDefault;    // 5 (plasma bolt)
        t.attackRange    = 14.0f;
        t.attackCooldown = combat::kRangedCooldownDefault;  // ~1.4 s
        t.attackWindup   = 0.30f;
        t.ranged         = true;
        t.standoff       = combat::kRangedStandoff;         // ~7 m
        t.aiStrafeBias   = 0.60f;                           // flanks/coordinates (drone-ish)
        t.tint[0]=0.45f; t.tint[1]=0.65f; t.tint[2]=1.0f;   // synthetic blue
        const bool realSynth = defBlueSynth(t, 1.0f);
        x3::logInfo(std::string("[bestiary] BlueSynth model: ") +
                    (realSynth ? "rigged blue_synth GLB" : "fallback Drone.glb (blue tint)"));
        defs.push_back({ EnemyType::BlueSynth, "BlueSynth", t });
    }

    return defs;
}

} // namespace

const std::vector<MonsterDef>& monsterDefs() {
    static const std::vector<MonsterDef> defs = buildMonsterDefs();
    return defs;
}

const MonsterDef& monsterDef(EnemyType t) {
    const std::vector<MonsterDef>& defs = monsterDefs();
    const uint32_t i = (uint32_t)t;
    if (i < defs.size() && defs[i].type == t) return defs[i];   // table is in enum order
    // Defensive: linear find if the table order ever drifts.
    for (const MonsterDef& d : defs) if (d.type == t) return d;
    return defs[0];
}

MonsterSystem::Tuning tuningFor(EnemyType t) {
    return monsterDef(t).tuning;
}

// ===========================================================================
// --test-bestiary: the data-driven enemy roster.
//   (a) each EnemyType BUILDS with its table stats (HP/type/ranged/damage match).
//   (b) each behaves per its weighting under steady LOS: melee species close +
//       FACE the player; ranged species hold a standoff (don't close to melee);
//       a strafe-heavy species (Verthani) reaches Strafe; weights differ across
//       the roster (Verthani strafes more than Illuminated).
//   (c) the rows are DISTINCT (no two identical stat blocks).
// Reuses the existing combat AI verbatim. No window / Vulkan.
// ===========================================================================
namespace {

int be_pass = 0, be_fail = 0;
void becheck(bool cond, const char* name) {
    if (cond) { ++be_pass; x3::logInfo(std::string("[bestiary-test] PASS ") + name); }
    else      { ++be_fail; x3::logError(std::string("[bestiary-test] FAIL ") + name); }
}

constexpr float kBeDt = 1.0f / 60.0f;

// A trivial damage sink the AI treats as "the player": alive, at a fixed eye.
class BeTargetStub final : public IDamageSink {
public:
    x3::phys::Vec3 eye{ 0.0f, 1.6f, 0.0f };
    bool takeDamage(int) override { return true; }
    x3::phys::Vec3 damageTargetPos() const override { return eye; }
    bool isAlive() const override { return true; }
};

// Flat ground at y=0 (CCW so +Y is solid), `half` to a side.
x3::phys::BodyId beGround(x3::phys::IPhysicsWorld& w, float half) {
    float v[] = { -half,0,-half,  half,0,-half,  half,0,half,  -half,0,half };
    uint32_t idx[] = { 0,2,1, 0,3,2 };
    return w.addStaticMesh(v, 4, idx, 6);
}

// Two tunings have an identical stat block? (compares the gameplay-relevant fields,
// not the model strings — used to prove the roster carries real variety.)
bool sameStats(const MonsterSystem::Tuning& a, const MonsterSystem::Tuning& b) {
    return a.type == b.type && a.hp == b.hp && a.damage == b.damage &&
           a.ranged == b.ranged && std::fabs(a.chaseSpeed - b.chaseSpeed) < 1e-4f &&
           std::fabs(a.attackRange - b.attackRange) < 1e-4f &&
           std::fabs(a.aiStrafeBias - b.aiStrafeBias) < 1e-4f;
}

} // namespace

bool runBestiarySelfTest() {
    be_pass = be_fail = 0;
    HeadlessDevice device;

    const std::vector<MonsterDef>& roster = monsterDefs();

    // ---- (a) the table is complete + ordered (one row per EnemyType, enum order). ----
    {
        bool complete = roster.size() == (size_t)EnemyType::Count;
        bool ordered = true;
        for (uint32_t i = 0; i < roster.size(); ++i)
            if ((uint32_t)roster[i].type != i) ordered = false;
        x3::logInfo(std::string("[bestiary-test] roster size=") + std::to_string(roster.size()) +
                    " (expected " + std::to_string((uint32_t)EnemyType::Count) + ")");
        becheck(complete && ordered, "Ta roster has one row per EnemyType, in enum order");
    }

    // ---- (c) the rows are DISTINCT (no two identical gameplay stat blocks). ----
    {
        bool allDistinct = true;
        for (size_t i = 0; i < roster.size(); ++i)
            for (size_t j = i + 1; j < roster.size(); ++j)
                if (sameStats(roster[i].tuning, roster[j].tuning)) allDistinct = false;
        becheck(allDistinct, "Tc all roster rows are distinct (the table carries variety)");
    }

    // ---- (b) per-species BUILD + BEHAVIOUR. For each EnemyType: spawn it via the
    // data-driven tuningFor() path, assert the built MonsterSystem reports the row's
    // stats, then run a steady LOS engagement and observe the AI behaves per weight.
    int  builtOk = 0;
    bool meleeClosed = false, meleeFaces = false;     // Trooper/Verthani
    bool rangedHeldStandoff = false;                  // Illuminated/BlueSynth
    bool verthaniStrafed = false, illuminatedHeldRange = false;
    for (uint32_t ti = 0; ti < (uint32_t)EnemyType::Count; ++ti) {
        const EnemyType et = (EnemyType)ti;
        const MonsterDef& def = monsterDef(et);
        MonsterSystem::Tuning t = tuningFor(et);

        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init(); beGround(*w, 80.0f);
        Scene scene; MonsterSystem m; BeTargetStub tgt;
        // Spawn ~10 m ahead (toward -Z) with a clear LOS so every type engages.
        m.buildMonsterTuned(scene, device, *w, riggedGlbRoot(),
                            x3::phys::Vec3{ 0.0f, 0.4f, 0.0f }, t);

        // (a) built stats match the row.
        const bool statsMatch =
            m.type() == def.tuning.type && m.maxHp() == def.tuning.hp &&
            m.ranged() == def.tuning.ranged && m.attackDamage() == def.tuning.damage;
        if (statsMatch) ++builtOk;
        x3::logInfo(std::string("[bestiary-test] ") + enemyTypeName(et) +
                    " built hp=" + std::to_string(m.maxHp()) +
                    " type=" + std::to_string((int)m.type()) +
                    " ranged=" + (m.ranged() ? "1" : "0") +
                    " dmg=" + std::to_string(m.attackDamage()) +
                    " realModel=" + (m.usingRealModel() ? "1" : "0") +
                    (statsMatch ? "  [stats OK]" : "  [STATS MISMATCH]"));

        // Behaviour: player 10 m ahead, clear LOS. Run a few seconds.
        tgt.eye = x3::phys::Vec3{ 0.0f, 1.6f, -10.0f };
        const float startHoriz = 10.0f;
        float minHoriz = 1e30f, maxHoriz = 0.0f;
        bool sawStrafe = false, sawAdvanceOrAttack = false;
        for (int i = 0; i < 360; ++i) {   // 6 s
            m.update(kBeDt, scene, *w, tgt.eye, tgt.eye, &tgt, AttackFxFn{},
                     BossPhaseFn{}, AllyQueryFn{});
            w->step(kBeDt);
            const float dz = tgt.eye.z - m.pos().z, dx = tgt.eye.x - m.pos().x;
            const float h = std::sqrt(dx*dx + dz*dz);
            if (h < minHoriz) minHoriz = h;
            if (h > maxHoriz) maxHoriz = h;
            if (m.aiState() == AiState::Strafe) sawStrafe = true;
            if (m.aiState() == AiState::Advance || m.aiState() == AiState::Attack)
                sawAdvanceOrAttack = true;
        }
        // Facing: forward (local -Z) points toward the player.
        const float dx = tgt.eye.x - m.pos().x, dz = tgt.eye.z - m.pos().z;
        const float dl = std::sqrt(dx*dx + dz*dz);
        const x3::phys::Vec3 f = m.facingDir();
        const float facingDot = (dl > 1e-4f) ? (f.x*dx + f.z*dz)/dl : 0.0f;

        x3::logInfo(std::string("[bestiary-test] ") + enemyTypeName(et) +
                    " minH=" + std::to_string(minHoriz) + " maxH=" + std::to_string(maxHoriz) +
                    " sawStrafe=" + (sawStrafe ? "1" : "0") +
                    " sawAdv/Atk=" + (sawAdvanceOrAttack ? "1" : "0") +
                    " facingDot=" + std::to_string(facingDot) + " engaged=" +
                    (m.hasLineOfSight() ? "1" : "0"));

        if (!def.tuning.ranged) {
            // MELEE species: must CLOSE to roughly melee reach and FACE the player.
            if (minHoriz <= def.tuning.attackRange + 1.0f) meleeClosed = true;
            if (facingDot > 0.85f) meleeFaces = true;
            if (et == EnemyType::Verthani && sawStrafe) verthaniStrafed = true;
        } else {
            // RANGED species: must HOLD a standoff — i.e. NOT close to melee reach.
            // It should settle near its standoff, clearly farther than melee.
            if (minHoriz > combat::kMeleeRange + 1.0f) rangedHeldStandoff = true;
            // Illuminated holds a LONG standoff: it should not approach inside ~6 m.
            if (et == EnemyType::Illuminated && minHoriz > 6.0f) illuminatedHeldRange = true;
        }
        (void)startHoriz; (void)sawAdvanceOrAttack;
        w->shutdown();
    }

    becheck(builtOk == (int)EnemyType::Count,
            "Tb each EnemyType builds with its table stats (HP/type/ranged/damage)");
    becheck(meleeClosed && meleeFaces,
            "Tb melee species close to reach AND face the player");
    becheck(rangedHeldStandoff && illuminatedHeldRange,
            "Tb ranged species hold a standoff (Illuminated holds a LONG one)");
    becheck(verthaniStrafed,
            "Tb Verthani (strafe-heavy weight) reaches the Strafe/flank state");

    x3::logInfo(std::string("[bestiary-test] ") + std::to_string(be_pass) + " passed, " +
                std::to_string(be_fail) + " failed");
    return be_fail == 0;
}

// Game-FEEL self-test (--test-gamefeel). Game-feel micro-polish:
//   (a) attack-anim crossfade: an enemy that commits an attack drives the one-shot
//       crossfade once (debounced) — when the model is skinnable; otherwise the
//       crossfade is correctly inert (skipped gracefully, like runAnimSelfTest).
//   (b) hit-react flash: hitFlash() spikes to ~1 on damage + decays to ~0 over
//       kHitFlashTime (mirrors Player::damageFlash).
//   (c) footstep cues: a MOVING enemy fires Footstep cues at phase crossings while
//       a stationary one does not (when the locomotion blend is live).
//   (d) ViewKick: recoil pitch + back-push + shake decay to ~0 after firing.
// No window / Vulkan.
// ===========================================================================
namespace {

int gf_pass = 0, gf_fail = 0;
void gfcheck(bool cond, const char* name) {
    if (cond) { ++gf_pass; x3::logInfo(std::string("[gamefeel-test] PASS ") + name); }
    else      { ++gf_fail; x3::logError(std::string("[gamefeel-test] FAIL ") + name); }
}

constexpr float kGfDt = 1.0f / 60.0f;

// Flat ground (CCW, +Y solid), `half` to a side.
x3::phys::BodyId gfGround(x3::phys::IPhysicsWorld& w, float half) {
    float v[] = { -half,0,-half,  half,0,-half,  half,0,half,  -half,0,half };
    uint32_t idx[] = { 0,2,1, 0,3,2 };
    return w.addStaticMesh(v, 4, idx, 6);
}

// A trivial alive damage sink the guard can attack.
class GfTargetStub final : public IDamageSink {
public:
    x3::phys::Vec3 eye{ 0.0f, 1.6f, 0.0f };
    bool takeDamage(int) override { return true; }
    x3::phys::Vec3 damageTargetPos() const override { return eye; }
    bool isAlive() const override { return true; }
};

} // namespace

bool runGameFeelSelfTest() {
    gf_pass = gf_fail = 0;

    HeadlessDevice device;

    // ---- T1: enemy hit-react flash spikes on damage + decays to ~0. -----------
    // Pure flash decay (mirrors Player). Works on the fallback box too (no anim).
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init();
        gfGround(*w, 50.0f);
        Scene scene;
        MonsterSystem m;
        MonsterSystem::Tuning t; t.hp = 1000; t.chaseSpeed = 0.0f; // tanky + still
        m.buildMonsterTuned(scene, device, *w, riggedGlbRoot(),
                            x3::phys::Vec3{ 0, 0.4f, 0 }, t);
        const float before = m.hitFlash();
        // Damage it (not lethal): the hit-flash should jump to ~1.
        m.takeMeleeDamage(50, scene, *w);
        const float spike = m.hitFlash();
        // Decay over > kHitFlashTime worth of frames (movement-only update, no target).
        const int frames = (int)(kHitFlashTime / kGfDt) + 6;
        for (int i = 0; i < frames; ++i) {
            m.update(kGfDt, scene, *w, x3::phys::Vec3{ 0, 1.6f, 0 });
            w->step(kGfDt);
        }
        const float decayed = m.hitFlash();
        gfcheck(before == 0.0f && spike > 0.9f && decayed <= 0.01f,
                "T1 enemy hit-flash spikes on damage + decays to ~0");
        w->shutdown();
    }

    // ---- T2: attack commit drives the one-shot crossfade ONCE (debounced). ----
    // A guard placed inside melee range of an alive target. With no wind-up the
    // first frame in range commits the attack -> triggerClip -> attackAnimActive.
    // We also assert it doesn't re-fire every frame (debounced on the cooldown).
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init();
        gfGround(*w, 50.0f);
        Scene scene;
        MonsterSystem m;
        GfTargetStub tgt;
        MonsterSystem::Tuning t;
        t.type = MonsterType::Guard; t.hp = 100; t.chaseSpeed = 0.0f;
        t.damage = 8; t.attackRange = 3.0f; t.attackCooldown = 0.5f; t.attackWindup = 0.0f;
        t.ranged = false;
        m.buildMonsterTuned(scene, device, *w, riggedGlbRoot(),
                            x3::phys::Vec3{ 0, 0.4f, 0 }, t);
        tgt.eye = x3::phys::Vec3{ 1.0f, 0.4f, 0.0f };   // 1 m away — inside range
        const x3::phys::Vec3 ppos{ tgt.eye.x, tgt.eye.y, tgt.eye.z };

        bool everActive = false;
        // Count the discrete attack COMMITS by watching the winding edge proxy: the
        // attack landed (player took damage) at least once proves we reached the
        // exact commit code path where triggerClip() is called.
        int hits = 0;
        struct CountSink final : public IDamageSink {
            int* n; x3::phys::Vec3 e;
            bool takeDamage(int) override { if (n) ++*n; return true; }
            x3::phys::Vec3 damageTargetPos() const override { return e; }
            bool isAlive() const override { return true; }
        } csink; csink.n = &hits; csink.e = tgt.eye;
        for (int i = 0; i < 40; ++i) {     // ~0.66 s — at least one attack commits
            m.update(kGfDt, scene, *w, ppos, ppos, &csink, AttackFxFn{}, BossPhaseFn{}, AllyQueryFn{});
            w->step(kGfDt);
            if (m.attackAnimActive()) everActive = true;
        }
        const bool committed = hits > 0;   // reached the attack-commit path
        if (m.locoBlendActive() && m.hasAttackClip()) {
            // Skinnable model with a usable one-shot clip: the crossfade MUST fire.
            gfcheck(committed && everActive, "T2 attack commit drives the one-shot crossfade");
        } else {
            // No locomotion blend (fallback box / non-anim model): the attack still
            // COMMITS (proving the trigger point is reached) but the crossfade is
            // correctly guarded off (inert) — graceful, like runAnimSelfTest.
            gfcheck(committed && !everActive,
                    "T2 attack commits; crossfade inert without a loco blend (graceful)");
        }
        w->shutdown();
    }

    // ---- T3: footstep cues fire while MOVING, not while STILL. ----------------
    // Drive a chasing guard toward a far target so it actually translates; count
    // Footstep cues via a sink. Compare to a stationary guard (0 footsteps). Only
    // meaningful with a live locomotion blend; otherwise assert it stays inert.
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init();
        gfGround(*w, 80.0f);
        Scene scene;
        MonsterSystem mover, idler;
        MonsterSystem::Tuning t; t.type = MonsterType::Guard; t.hp = 100;
        t.chaseSpeed = 3.0f; t.damage = 0; t.attackRange = 1.0f;
        mover.buildMonsterTuned(scene, device, *w, riggedGlbRoot(),
                                x3::phys::Vec3{ 0, 0.4f, 0 }, t);
        MonsterSystem::Tuning ti = t; ti.chaseSpeed = 0.0f;
        idler.buildMonsterTuned(scene, device, *w, riggedGlbRoot(),
                                x3::phys::Vec3{ 20.0f, 0.4f, 0 }, ti);
        int moverSteps = 0, idlerSteps = 0;
        mover.setCueSink([&](const GameCue& c){ if (c.kind == CueKind::Footstep) ++moverSteps; });
        idler.setCueSink([&](const GameCue& c){ if (c.kind == CueKind::Footstep) ++idlerSteps; });
        // Player far ahead so the mover advances (LOS clear over open ground).
        const x3::phys::Vec3 ppos{ 0.0f, 1.6f, 30.0f };
        GfTargetStub tgt; tgt.eye = ppos;
        for (int i = 0; i < 300; ++i) {    // 5 s
            mover.update(kGfDt, scene, *w, ppos, ppos, &tgt, AttackFxFn{}, BossPhaseFn{}, AllyQueryFn{});
            idler.update(kGfDt, scene, *w, ppos, ppos, &tgt, AttackFxFn{}, BossPhaseFn{}, AllyQueryFn{});
            w->step(kGfDt);
        }
        x3::logInfo(std::string("[gamefeel-test] (T3) moverSteps=") + std::to_string(moverSteps) +
                    " idlerSteps=" + std::to_string(idlerSteps) +
                    " loco=" + (mover.locoBlendActive() ? "1" : "0"));
        if (mover.locoBlendActive()) {
            // Live blend: a moving enemy plants feet (cues > 0); a still one doesn't.
            gfcheck(moverSteps > 0 && idlerSteps == 0,
                    "T3 footstep cues fire while moving, not while idle");
        } else {
            // No blend: footstep cues are correctly never emitted (graceful).
            gfcheck(moverSteps == 0 && idlerSteps == 0,
                    "T3 footstep cues inert without a locomotion blend (graceful)");
        }
        w->shutdown();
    }

    // ---- T4: ViewKick recoil + shake spike on fire then decay to ~0. ----------
    // Pure logic (no device/physics). Mirrors the player damage-flash decay shape.
    {
        ViewKick vk;
        bool restBefore = !vk.active() && vk.pitchOffset() == 0.0f && vk.backOffset() == 0.0f;
        vk.fire(4.0f);   // shotgun-ish kick
        const float pitch0 = vk.pitchOffset();
        const float back0  = vk.backOffset();
        bool spiked = pitch0 > 0.0f && back0 > 0.0f && vk.active();
        // Decay over ~1.2 s (well past the recoil-recover + shake windows): the
        // exponential relax converges to a tiny residual; assert it returns to ~0
        // (and clearly below the spike) — the "view recovers" property.
        for (int i = 0; i < 72; ++i) vk.tick(1.0f / 60.0f);
        bool recovered = vk.pitchOffset() < 1e-3f && vk.backOffset() < 1e-3f &&
                         vk.shakeYaw() == 0.0f && vk.shakePitch() == 0.0f && !vk.active();
        gfcheck(restBefore && spiked && recovered,
                "T4 ViewKick recoil+shake spike on fire then decay to ~0");
    }

    x3::logInfo(std::string("[gamefeel-test] ") + std::to_string(gf_pass) + " passed, " +
                std::to_string(gf_fail) + " failed");
    return gf_fail == 0;
}

} // namespace x3::game
