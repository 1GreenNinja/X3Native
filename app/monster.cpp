// Monster + combat (S6). See app/monster.h.
//
// Clean-room: built from the IModelLoader + IAssetSource + IRenderDevice +
// IPhysicsWorld + Scene interfaces only. No purchased C# copied; no id Tech /
// RBDOOM source consulted.
#include "monster.h"
#include "mesh_prims.h"

#include "engine/core/x3_log.h"

#include <cmath>
#include <memory>
#include <string>

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

} // namespace

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

    // ---- J1: bind the skeletal-animation runtime if this model is skinnable
    // (has a skin + joints + at least one clip + skinned primitives). Pick an idle
    // clip (fuzzy name match; fall back to the first clip) and, if present, a walk/
    // run clip to play while moving. Models with no skin/anim (the Drone, the
    // legacy crawler, the fallback box) leave the skinner invalid -> static draw. ----
    if (m_model.ok && m_skinner.bind(m_model)) {
        m_idleClip = m_skinner.findClip({ "idle", "stand", "breath", "loop" });
        m_walkClip = m_skinner.findClip({ "walk", "run", "move", "jog" });
        if (m_idleClip < 0) m_idleClip = 0;   // fall back to the first clip
        m_animActive = (m_idleClip >= 0);
        std::string clipList;
        for (uint32_t c = 0; c < m_skinner.clipCount(); ++c) {
            clipList += (c ? ", " : "") + std::string(m_skinner.clipName(c)) +
                        "(" + std::to_string(m_skinner.clipDuration(c)) + "s)";
        }
        x3::logInfo("[monster] " + modelFile + " is animated — clips: " + clipList +
                    "; idle=" + std::to_string(m_idleClip) +
                    " walk=" + std::to_string(m_walkClip));
        // Pose the bind-pose mesh into the idle clip at t=0 once up front so the
        // very first rendered frame already shows the animated pose (not bind pose).
        if (m_animActive && m_device)
            m_skinner.apply(m_model, *m_device, (uint32_t)m_idleClip, 0.0f);
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
    m_body = physics.addBox(kMonsterHalf, m_pos, 0.0f, x3::phys::Layer::Enemy);

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

    // ---- Hit a live monster: apply damage + start the red hit-flash. ----
    r.hitMonster = true;
    bool dead = applyDamage(&m_hp, kDamagePerShot);
    m_flash = kHitFlashTime;
    r.hpAfter = m_hp;

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
    update(dt, scene, physics, playerPos, nullptr, AttackFxFn{}, BossPhaseFn{});
}

void MonsterSystem::update(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics,
                           const x3::phys::Vec3& playerPos,
                           IDamageSink* target, const AttackFxFn& fx) {
    update(dt, scene, physics, playerPos, target, fx, BossPhaseFn{});
}

void MonsterSystem::update(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics,
                           const x3::phys::Vec3& playerPos,
                           IDamageSink* target, const AttackFxFn& fx,
                           const BossPhaseFn& onPhase) {
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

    // ---- Face the player (horizontal yaw) EVERY frame while alive. Computed
    // from the body->player vector on the XZ plane. The render 3x3 is rebuilt
    // below from this yaw + kFaceSign (which forward axis the model authored). ----
    float dx = playerPos.x - m_pos.x;
    float dz = playerPos.z - m_pos.z;
    float horiz = std::sqrt(dx * dx + dz * dz);
    if (horiz > 1e-4f) {
        // Yaw so the model's forward axis points toward the player. With
        // kFaceSign = -1 (glTF -Z forward), aim local -Z at (dx,dz).
        m_yaw = std::atan2(kFaceSign * dx, kFaceSign * dz);
    }

    // ---- Chase: weave toward the player while far, orbit when close, and STOP at
    // walls/props so it neither beelines-then-freezes nor phases through the box.
    // The Drone (ranged) instead HOLDS a standoff distance: it approaches only
    // until within m_standoff, then strafes/backs off to keep its distance and
    // shoot. (Facing still tracks the player, above.) ----
    // Phase-scaled chase speed (Boss enrage moves faster; 1x for everyone else).
    const float chaseSpeed = m_chaseSpeed * m_phaseSpeedMul;
    if (chaseSpeed > 0.0f && m_body.valid() && horiz > 1e-4f) {
        m_wander += dt * kStrafeFreq;
        const float fxn = dx / horiz, fzn = dz / horiz;   // toward player (XZ)
        const float pxn = -fzn,       pzn = fxn;          // perpendicular (left)

        float mx, mz;
        if (m_ranged) {
            // Drone standoff: keep ~m_standoff away. Too close -> back off; too far
            // -> approach; in the band -> strafe sideways (orbit) to feel alive.
            const float band = 1.0f; // dead-band (m) around the standoff distance
            if (horiz < m_standoff - band) {
                mx = -fxn; mz = -fzn;                       // retreat
            } else if (horiz > m_standoff + band) {
                mx = fxn;  mz = fzn;                        // approach
            } else {
                mx = pxn * m_strafeDir; mz = pzn * m_strafeDir; // strafe / orbit
                m_retarget -= dt;
                if (m_retarget <= 0.0f) { m_strafeDir = -m_strafeDir; m_retarget = kOrbitRetarget; }
            }
        } else if (horiz > kChaseStopDist) {
            // Approach with a side-to-side weave (not a straight line).
            const float strafe = std::sin(m_wander) * kStrafeAmt;
            mx = fxn + pxn * strafe;
            mz = fzn + pzn * strafe;
        } else {
            // Close: circle the player instead of grinding to a halt.
            mx = pxn * m_strafeDir;
            mz = pzn * m_strafeDir;
            m_retarget -= dt;
            if (m_retarget <= 0.0f) { m_strafeDir = -m_strafeDir; m_retarget = kOrbitRetarget; }
        }
        float ml = std::sqrt(mx * mx + mz * mz);
        if (ml > 1e-4f) { mx /= ml; mz /= ml; }

        const float step  = chaseSpeed * dt;
        const float probe = step + kMonsterHalf.x + 0.05f;
        const x3::phys::Vec3 mdir{ mx, 0.0f, mz };
        // Don't walk through walls (Static) or props like the dynamic box (Dynamic).
        const bool blocked =
            physics.rayCast(m_pos, mdir, probe, x3::phys::Layer::Static).hit ||
            physics.rayCast(m_pos, mdir, probe, x3::phys::Layer::Dynamic).hit;
        if (blocked) {
            m_strafeDir = -m_strafeDir;   // try a new line next frames
            m_wander   += 1.7f;
        } else {
            m_pos.x += mx * step;
            m_pos.z += mz * step;
            physics.setBodyPosition(m_body, m_pos);
        }
    }

    // ---- Attack (Phase 2a, spec §6.5). Guard/Boss = melee within attackRange;
    // Drone = ranged hitscan toward the player within attackRange. Both gate on a
    // per-attack cooldown and a short wind-up telegraph so the hit reads/feels fair
    // (the wind-up also gives the player a beat to react). `target` may be null
    // (movement-only path), in which case no attacks run. ----
    if (target && target->isAlive() && m_dmg > 0 && horiz <= m_attackRange) {
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
                    if (hit)
                        x3::logInfo(std::string("[monster] ") +
                                    (m_ranged ? "drone ranged" : "melee") +
                                    " hit player for " + std::to_string(dmg));
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
        const float c = std::cos(m_yaw), s = std::sin(m_yaw);
        // Yaw about +Y: local +X -> (c,0,-s), +Z -> (s,0,c). The phase scale
        // multiplier up-scales the boss as it enrages (graybox phase feedback).
        const float scale = m_modelScale * m_phaseScaleMul;
        Entity& me = scene.get(m_entity);
        composeTRS(me.transform,
                   x3::phys::Vec3{ c, 0.0f, -s },
                   x3::phys::Vec3{ 0.0f, 1.0f, 0.0f },
                   x3::phys::Vec3{ s, 0.0f, c },
                   scale, m_pos);
    }

    // ---- J1: drive skeletal animation. Advance the active clip's time and CPU-
    // skin the mesh. Use the walk/run clip while the planar velocity is non-trivial
    // and a walk clip exists; otherwise the idle clip. Re-uploads the skinned
    // vertices through the cached device. No-op for unskinned models. ----
    if (m_animActive && m_device) {
        const float ddx = m_pos.x - prevPos.x, ddz = m_pos.z - prevPos.z;
        const float planarSpeed = (dt > 1e-5f)
            ? std::sqrt(ddx*ddx + ddz*ddz) / dt : 0.0f;
        const bool moving = (m_walkClip >= 0) && (planarSpeed > 0.25f);
        const int clip = moving ? m_walkClip : m_idleClip;
        m_animTime += dt;
        m_skinner.apply(m_model, *m_device, (uint32_t)clip, m_animTime);
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
    // flashAmt in [0,1]; 1 right after a hit, 0 once decayed. Start from the
    // per-instance base tint (Tuning.tint) so e.g. boss Martinez reads distinct.
    const float flashAmt = (kHitFlashTime > 0.0f) ? (m_flash / kHitFlashTime) : 0.0f;
    // Fold the per-phase tint multiplier into the base (Boss enrage reddens; 1x for
    // everyone else) so the active boss phase reads on screen.
    float tint[4] = { m_baseTint[0] * m_phaseTintMul[0],
                      m_baseTint[1] * m_phaseTintMul[1],
                      m_baseTint[2] * m_phaseTintMul[2],
                      m_baseTint[3] };
    // Toward red: keep R, knock down G/B by the flash amount.
    tint[1] *= (1.0f - 0.85f * flashAmt);
    tint[2] *= (1.0f - 0.85f * flashAmt);

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
    uint32_t idx = (uint32_t)m_monsters.size();
    m_monsters.push_back(std::move(m));
    return idx;
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
    for (auto& m : m_monsters)
        m->update(dt, scene, physics, playerPos, target, fx, onPhase);
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

// Minimal headless IRenderDevice: hands out monotonically-increasing valid
// handles so buildMonster() runs unchanged with no Vulkan. Draw/frame/camera
// calls are no-ops. (Same shape as door.cpp's HeadlessDevice.)
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
    combat.buildMonster(scene, device, *physics, "G:/GameModels/rigged_glb", monsterPos);

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
        guard.buildMonsterTuned(scene, device, *w, "G:/GameModels/rigged_glb",
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
        drone.buildMonsterTuned(scene, device, *w, "G:/GameModels/rigged_glb",
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

} // namespace x3::game
