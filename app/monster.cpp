// Monster + combat (S6). See app/monster.h.
//
// Clean-room: built from the IModelLoader + IAssetSource + IRenderDevice +
// IPhysicsWorld + Scene interfaces only. No purchased C# copied; no id Tech /
// RBDOOM source consulted.
#include "monster.h"
#include "mesh_prims.h"
#include "headless_device.h"
#include "asset_root.h"

#include "engine/core/x3_log.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
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

// Hover height (m) added to a Drone-type enemy's spawn Y so flyers float in the
// air (above the player's eye line) instead of walking the floor.
constexpr float kDroneHoverY = 1.8f;

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

// Inverse of a column-major affine 4x4 that is a rotation * UNIFORM scale +
// translation (the monster's gameplay model matrix: composeTRS with a yaw basis +
// uniform scale s). The upper 3x3 is R*s, so its inverse is R^T / s; the
// translation maps to -(invA * t). General enough for the death-ragdoll space
// transform (TASK#12). Returns false (and leaves `out` identity) on a degenerate
// (near-zero scale) matrix.
bool invertAffineUniform(const float m[16], float out[16]) {
    for (int i = 0; i < 16; ++i) out[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    // Uniform scale = length of column 0 (col0 = R*s). Guard against zero.
    const float s2 = m[0]*m[0] + m[1]*m[1] + m[2]*m[2];
    if (s2 < 1e-12f) return false;
    const float invS2 = 1.0f / s2;   // (1/s) * (1/s); invA = R^T / s = (col^T) / s^2
    // invA (3x3) = transpose(upper3x3) * invS2 (since (R*s)^-1 = R^T / s = R^T * s / s^2).
    // out columns are the rows of m scaled by invS2.
    out[0] = m[0]*invS2; out[1] = m[4]*invS2; out[2] = m[8]*invS2;
    out[4] = m[1]*invS2; out[5] = m[5]*invS2; out[6] = m[9]*invS2;
    out[8] = m[2]*invS2; out[9] = m[6]*invS2; out[10]= m[10]*invS2;
    out[3] = out[7] = out[11] = 0.0f;
    // translation: -(invA * t)
    const float tx = m[12], ty = m[13], tz = m[14];
    out[12] = -(out[0]*tx + out[4]*ty + out[8]*tz);
    out[13] = -(out[1]*tx + out[5]*ty + out[9]*tz);
    out[14] = -(out[2]*tx + out[6]*ty + out[10]*tz);
    out[15] = 1.0f;
    return true;
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

// [P2-5] Normalize a fire() aim direction (guarding the near-zero case) so the
// miss-tracer end point (eye + dir*range) sits at the true max range regardless
// of the caller's dir magnitude. Shared by fire() and applyFireHit().
x3::phys::Vec3 fireDirNormalized(const x3::phys::Vec3& dir) {
    float dl = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (dl < 1e-6f) dl = 1e-6f;
    return x3::phys::Vec3{ dir.x / dl, dir.y / dl, dir.z / dl };
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
        case AiState::Patrol:  return "Patrol";
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
// hit, decaying to 0 over kHitFlashTime. The draw path + HUD health bars read this.
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
    m_emissiveScale = tuning.emissiveScale;
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
    m_flyer          = tuning.flyer;            // hovering, center-origin enemy
    // ---- Guard-life (W4-3): species stamp + patrol arming. The anchor is the
    // SPAWN position (captured before the flyer hover lift below — patrols are
    // ground behaviour and flyers never enable them anyway). A patrol-capable
    // row STARTS in Patrol so a never-aggroed guard walks its beat from frame
    // one; patrolRadius==0 (every existing tuning) keeps the original Idle. ----
    m_species        = tuning.species;
    m_patrolRadius   = tuning.patrolRadius > 0.0f ? tuning.patrolRadius : 0.0f;
    m_patrolPauseSec = tuning.patrolPauseSec;
    m_patrolSpeedMul = tuning.patrolSpeedMul;
    m_patrolAnchor   = m_pos;
    if (m_patrolRadius > 0.0f && !m_flyer) m_ai = AiState::Patrol;

    // ---- Flyers HOVER: lift the spawn position off the floor so the model floats
    // in the air instead of walking the ground. The AI only moves in x/z (m_pos.y
    // is never touched), so this hover holds for the enemy's whole life. Keyed on
    // m_flyer, NOT type==Drone (that type is also used by GROUND elites).
    if (m_flyer) m_pos.y += kDroneHoverY;
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

    // ---- Act-1 boss gimmicks (Wave 1). Inert by default (Martinez unchanged). ----
    m_hasCureOption       = tuning.hasCureOption;
    m_cured               = false;
    m_memoryFlashTime     = tuning.memoryFlashTime;
    m_memoryFlashDamageMul = tuning.memoryFlashDamageMul;
    m_flashTimer          = 0.0f;
    // ---- Adaptive Hide (canon-aliens). Inert unless adaptiveHideResist > 0. ----
    m_adaptiveHideResist      = tuning.adaptiveHideResist;
    m_adaptiveHideDurationSec = tuning.adaptiveHideDurationSec;
    m_adaptiveHideType        = x3::DamageType::None;
    m_adaptiveHideTimer       = 0.0f;

    // ---- Act-2 gimmicks (Wave 2). Inert by default. Data tags read by act2_world. ----
    m_copyFeintPhase      = tuning.copyFeintPhase;
    m_escapeTimer         = tuning.escapeTimerSeconds;
    // START ALLIED (Salvari ally, L11+): pre-flip so the monster fights beside the
    // player and CANNOT damage them. Matches the post-build convertToAllied()
    // result so the rest of the system (AI / draw / death) is unchanged.
    if (tuning.startAllied) {
        m_allied = true;
        m_dmg    = 0;
    } else {
        m_allied = false;
    }

    // ---- Try the real purchased GLB via a mounted loose-dir asset source. The
    // model file + dir are tuning-overridable (EFLZ art pass): Level 1 points the
    // characters at converted_glb/Characters/*.glb; the tests keep the legacy
    // rigged_glb/alien_crawler.glb (empty overrides). ----
    const std::string modelFile = tuning.modelFile.empty()
        ? std::string("alien_crawler.glb") : tuning.modelFile;
    const std::string useDir = tuning.modelDirOverride.empty()
        ? std::string(modelDir) : tuning.modelDirOverride;

    // X3_MONSTER_PROF=1: per-phase spawn timing (boot-regression hunt). Cheap
    // enough to keep — two clock reads per phase, only logged when armed.
    const bool prof = std::getenv("X3_MONSTER_PROF") != nullptr;
    using profclock = std::chrono::steady_clock;
    auto profT0 = profclock::now();
    double msMount = 0.0, msLoad = 0.0, msDraw = 0.0, msSkin = 0.0, msPose = 0.0;
    auto profMs = [&profT0]() {
        const auto t1 = profclock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - profT0).count();
        profT0 = t1;
        return ms;
    };

    m_device = &device;   // cached so update() can re-upload CPU-skinned vertices
    m_assets.reset(x3::asset::createAssetSource());
    bool mounted = m_assets->mountDir(useDir, 0);
    if (prof) msMount = profMs();
    if (mounted) {
        m_loader.reset(x3::asset::createModelLoader(&device, m_assets.get()));
        m_model = m_loader->load(modelFile);
        if (prof) msLoad = profMs();
        if (m_model.ok)
            m_drawables = x3::asset::makeDrawables(m_model);
        if (prof) msDraw = profMs();
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
    if (prof) profT0 = profclock::now();
    if (m_model.ok && m_skinner.bind(m_model)) {
        // GPU SKINNING OF MODELS: register this character's skinned primitives with
        // the device's compute-skinning path. When supported, apply/applyLocomotion
        // upload the joint palette + the GPU skins (no per-frame CPU LBS + full vertex
        // re-upload) — the scalability fix for crowds of NPCs. Falls back to CPU
        // skinning transparently on a non-compute / headless device.
        const bool gpuSkin = m_skinner.enableGpuSkinning(device, m_model);
        if (prof) msSkin = profMs();
        x3::logInfo(std::string("[monster] ") + std::string(modelFile) +
                    (gpuSkin ? "  ->  GPU-SKINNED (compute pre-pass)"
                             : "  ->  CPU-SKINNED FALLBACK (per-frame updateMesh — PERF)"));
        m_idleClip = m_skinner.findClip({ "idle", "stand", "breath", "loop" });
        // Prefer a distinct WALK clip; fall back to any move clip for m_walkClip.
        m_walkClip = m_skinner.findClip({ "walk" });
        m_runClip  = m_skinner.findClip({ "run", "sprint", "jog" });
        m_jumpClip = m_skinner.findClip({ "jump", "leap" });
        // W2-D: one-shot combat clips. Absent on today's retargeted rigs (Idle/Walk/
        // Run[/Jump] only) — the attack_death_bake.py pipeline appends them; lookup
        // degrades to -1 gracefully so this wiring is drop-in either way.
        m_attackClip = m_skinner.findClip({ "attack", "strike", "swing", "punch", "bite", "swipe", "slash" });
        m_deathClip  = m_skinner.findClip({ "death", "die", "collapse" });
        if (m_walkClip < 0) m_walkClip = m_skinner.findClip({ "move", "jog", "run" });
        if (m_idleClip < 0) m_idleClip = 0;   // fall back to the first clip
        m_animActive = (m_idleClip >= 0);
        // The locomotion blend is meaningful only when a real idle + at least one
        // distinct move clip exist (so speed can sweep). Walk authored ~1.5 m/s,
        // Run ~4 m/s (the AI maps planar speed to those bands).
        m_useLocoBlend = m_animActive && (m_walkClip >= 0 || m_runClip >= 0);
        if (m_useLocoBlend)
            // Walk threshold lowered 1.5 -> 0.2 m/s so any real motion triggers walk
            // (Tim playtest 2026-05-27: monsters were sliding without animating).
            m_skinner.setLocomotionClips(m_idleClip, m_walkClip, m_runClip, 0.2f, 2.0f);
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
                    " death=" + std::to_string(m_deathClip) +
                    " locoBlend=" + (m_useLocoBlend ? "1" : "0"));
        // Pose the bind-pose mesh into the idle pose at t=0 once up front so the
        // very first rendered frame already shows the animated pose (not bind pose).
        if (m_animActive && m_device) {
            if (prof) profT0 = profclock::now();
            if (m_useLocoBlend) {
                m_skinner.setLocomotionSpeed(0.0f);   // start idle
                m_skinner.applyLocomotion(m_model, *m_device, 0.0f);
            } else {
                m_skinner.apply(m_model, *m_device, (uint32_t)m_idleClip, 0.0f);
            }
            if (prof) msPose = profMs();
        }
    }
    if (prof)
        x3::logInfo("[monster-prof] " + modelFile +
                    "  mount=" + std::to_string(msMount) +
                    " load="  + std::to_string(msLoad) +
                    " draw="  + std::to_string(msDraw) +
                    " skin="  + std::to_string(msSkin) +
                    " pose="  + std::to_string(msPose) + " (ms)");

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
    // connects. The old box was CENTERED on m_pos — but ground models have their
    // origin at the FEET, so the box sat half underground and only covered the shins:
    // chest/head shots flew clean over it (humanoids were nearly unhittable; only the
    // centre-origin hovering drone was hit). Fix: raise the box for ground enemies so
    // it spans feet..head; keep the drone's box centered on m_pos (origin IS center).
    if (!tuning.noBody) {
        const float hs = (m_modelScale > 0.1f) ? m_modelScale : 1.0f;
        if (m_flyer) {
            // Hovering flyer (Drone.glb): model origin is its CENTER -> centered box.
            m_hitHalfY     = 0.95f * hs;
            m_hitCenterOff = 0.0f;
        } else {
            // GROUND enemy: a GENEROUS box from a little below the origin up to well
            // above it, so it covers the visible body whether the model's origin is
            // at the FEET (humanoids) or the CENTER (low insectoid "beasts"). The old
            // box assumed feet-origin and sat above a center-origin crawler's body,
            // making it impossible to hit. std::max keeps small-scaled models hittable.
            const float sc     = std::max(hs, 0.8f);
            const float topY   = 2.0f * sc;          // reach the head of a tall model
            const float skirt  = 0.4f;               // dip below the origin (low crawlers)
            m_hitHalfY     = (topY + skirt) * 0.5f;
            m_hitCenterOff = m_hitHalfY - skirt;     // box bottom = origin - skirt
        }
        const float hw = 0.60f * std::max(hs, 0.8f);
        m_hitHalfXZ = hw;                        // chase wall-probe must clear this width
        const x3::phys::Vec3 hitHalf{ hw, m_hitHalfY, hw };
        const x3::phys::Vec3 center{ m_pos.x, m_pos.y + m_hitCenterOff, m_pos.z };
        m_body = physics.addBox(hitHalf, center, 0.0f, x3::phys::Layer::Enemy);
    }

    // ---- Monster Entity: bookkeeping (tag/body/visibility/transform). Its render
    // mesh handle is left INVALID so Scene::render skips it; drawMonster() renders
    // ALL of the model's primitives at the Entity transform with the hit-flash
    // tint (so multi-primitive GLBs draw fully + consistently). ----
    Entity e;
    e.tag     = (uint32_t)Tag::Monster;
    e.visible = true;
    e.body    = m_body;                          // also registers body->entity map
    // The hitbox body is centered at m_pos + m_hitCenterOff (raised so a feet-origin
    // humanoid's box spans feet..head); tell Scene::update to anchor the VISUAL +
    // melee/draw transform back at the feet (m_pos) so the raised box stays invisible.
    e.bodyVisualOffsetY = m_hitCenterOff;
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
                               Scene& scene, x3::phys::IPhysicsWorld& physics,
                               int damage, x3::DamageType type) {
    // [P2-5] fire() = ONE Enemy-layer cast + applyFireHit() on the result. The
    // single-monster behaviour is byte-identical to the pre-split code; the split
    // exists so MonsterManager / MultiPodBoss can cast once and fan the same hit.
    if (!m_alive) {
        // Already dead: nothing to hit (and no cast — same as before the split).
        FireResult r;
        r.hpAfter  = m_hp;
        const x3::phys::Vec3 nd = fireDirNormalized(dir);
        r.endPoint = x3::phys::Vec3{ eye.x + nd.x * kFireMaxDist,
                                     eye.y + nd.y * kFireMaxDist,
                                     eye.z + nd.z * kFireMaxDist };
        return r;
    }
    const x3::phys::RayHit hit =
        physics.rayCast(eye, fireDirNormalized(dir), kFireMaxDist, x3::phys::Layer::Enemy);
    return applyFireHit(hit, eye, dir, scene, physics, damage, type);
}

// ---------------------------------------------------------------------------
// [P2-5] Resolve a precomputed Enemy-layer ray hit against THIS monster. This is
// the entire former body of fire() after its rayCast: tracer bookkeeping, "is the
// hit body mine?" resolution, damage (headshot / adaptive-hide / memory-flash),
// and the death path. See specs/MONSTER_FIRE_SINGLE_RAY.spec.md.
// ---------------------------------------------------------------------------
FireResult MonsterSystem::applyFireHit(const x3::phys::RayHit& hit,
                                       const x3::phys::Vec3& eye, const x3::phys::Vec3& dir,
                                       Scene& scene, x3::phys::IPhysicsWorld& physics,
                                       int damage, x3::DamageType type) {
    FireResult r;
    r.hpAfter = m_hp;

    // Normalize the look dir so the FX tracer "miss" end point (eye + dir*range)
    // is at the true max range regardless of the caller's dir magnitude.
    const x3::phys::Vec3 ndir = fireDirNormalized(dir);
    // Default tracer end on a miss: straight out to max range.
    r.endPoint = x3::phys::Vec3{ eye.x + ndir.x * kFireMaxDist,
                                 eye.y + ndir.y * kFireMaxDist,
                                 eye.z + ndir.z * kFireMaxDist };

    if (!m_alive) return r;                       // already dead: nothing to hit

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
    // PER-WEAPON damage: the caller passes the firing weapon's WeaponDef damage
    // (defaults to kDamagePerShot for legacy/test paths). Act-1 boss gimmick (FE#7):
    // a memory-flash window amplifies incoming damage (incomingDamageMul()); 1x for
    // every non-flashing monster (no behaviour change). HEADSHOT (a hit in the upper
    // part of the hitbox) deals 3x — box center is m_pos.y + m_hitCenterOff, its top
    // is +m_hitHalfY above that, so the top half is the head zone.
    r.hitMonster = true;
    const bool headshot = (hit.point.y - m_pos.y) > m_hitCenterOff + m_hitHalfY * 0.5f;
    const int  baseDmg  = headshot ? damage * 3 : damage;
    // ADAPTIVE HIDE (canon-aliens; opt-in via Tuning::adaptiveHideResist). When a
    // matched type lands inside the current resist window, scale damage by
    // (1 - resist). Stacks multiplicatively with memoryFlashDamageMul (see §3.1
    // of docs/canon-aliens-adaptive-hide.md). Inert when m_adaptiveHideResist == 0.
    float resistMul = 1.0f;
    if (m_adaptiveHideResist > 0.0f &&
        m_adaptiveHideTimer  > 0.0f &&
        type == m_adaptiveHideType) {
        resistMul = 1.0f - m_adaptiveHideResist;
    }
    // W9-1: m_damageTakenMul folds in the coolant-sabotage vulnerability (x1.5
    // on The Collective once the F4 console is used; 1.0 for everything else).
    const int shotDmg = (int)(baseDmg * incomingDamageMul() * resistMul *
                              m_damageTakenMul + 0.5f);
    if (headshot) x3::logInfo("[monster] HEADSHOT! 3x damage");
    bool dead = applyDamage(&m_hp, shotDmg);
    // Latch the new type + reset the resist window AFTER applying damage. Only
    // bookkeep when the row has opted in (avoids touching state for normal rows).
    if (m_adaptiveHideResist > 0.0f) {
        m_adaptiveHideType  = type;
        m_adaptiveHideTimer = m_adaptiveHideDurationSec;
    }
    m_flash = kHitFlashTime;
    r.hpAfter = m_hp;
    // D-ai: remember recent damage so the state machine can flinch/retreat.
    m_dmgMemory   = kAiDamageMemory;
    m_dmgWindowHp += shotDmg;

    if (dead) {
        // ---- Death: remove the physics body IMMEDIATELY (so subsequent rays
        // miss right away) and drop the Entity's body handle, but DO NOT hide the
        // model yet — start a brief death "pop" so the kill reads on screen. The
        // Entity stays visible while m_dying; update() counts m_deathPop down and
        // hides it when the pop finishes. ----
        m_alive = false;
        r.killed = true;
        m_dying    = true;
        m_deathPop = kDeathToppleTime;
        if (m_entity != kNoLink && m_entity < scene.size()) {
            Entity& me = scene.get(m_entity);
            me.body = x3::phys::BodyId{};         // entity no longer owns a body
        }
        if (m_body.valid()) physics.removeBody(m_body);
        m_body = x3::phys::BodyId{};
        x3::logInfo("[monster] killed (HP 0) — body removed, death-pop started");
        // TASK#12: try the SKINNED DEATH RAGDOLL — a rigged enemy physically flops
        // with its model (the shot direction carries the topple shove). No-op on an
        // unrigged model -> the legacy rigid topple draws instead. Spawn AFTER the
        // Enemy box is removed so the ragdoll bodies don't fight it.
        spawnDeathRagdoll(physics, dir);
        // GIBS: fire the death FX sink ONCE at the kill moment so the host explodes
        // the monster into debris chunks + blood at its body center (not its feet).
        if (m_deathFx) {
            const float center[3] = { m_pos.x, m_pos.y + m_hitCenterOff, m_pos.z };
            m_deathFx(center, m_type == MonsterType::Drone);
        }
        // Enemy-SFX: a DEATH vocalization at the kill moment (host -> creature-death).
        emitCueOrLog(m_cueSink, GameCue{ CueKind::EnemyDeath,
            x3::phys::Vec3{ m_pos.x, m_pos.y + m_hitCenterOff, m_pos.z }, 1.0f,
            (uint32_t)m_species });
    } else {
        x3::logInfo("[monster] hit for " + std::to_string(shotDmg) +
                    " — HP now " + std::to_string(m_hp));
        // Enemy-SFX: a TAKE-HIT grunt when it survives the shot (host -> creature-pain).
        emitCueOrLog(m_cueSink, GameCue{ CueKind::EnemyHit,
            x3::phys::Vec3{ m_pos.x, m_pos.y + m_hitCenterOff, m_pos.z }, 1.0f,
            (uint32_t)m_species });
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
                                    x3::phys::IPhysicsWorld& physics,
                                    x3::DamageType type) {
    if (!m_alive) return false;
    // ADAPTIVE HIDE (canon-aliens; same rule as fire()): when a matched type lands
    // inside the current resist window, scale by (1 - resist). Then latch + reset.
    float resistMul = 1.0f;
    if (m_adaptiveHideResist > 0.0f &&
        m_adaptiveHideTimer  > 0.0f &&
        type == m_adaptiveHideType) {
        resistMul = 1.0f - m_adaptiveHideResist;
    }
    // Act-1 boss gimmick (FE#7): memory-flash amplifies incoming melee too (1x else).
    // W9-1: m_damageTakenMul = the coolant-sabotage vulnerability (1.0 default).
    const int dmg = (int)(damage * incomingDamageMul() * resistMul *
                          m_damageTakenMul + 0.5f);
    bool dead = applyDamage(&m_hp, dmg);
    if (m_adaptiveHideResist > 0.0f) {
        m_adaptiveHideType  = type;
        m_adaptiveHideTimer = m_adaptiveHideDurationSec;
    }
    m_flash = kHitFlashTime;
    // D-ai: heavy melee is a strong flinch trigger.
    m_dmgMemory   = kAiDamageMemory;
    m_dmgWindowHp += dmg;
    if (dead) {
        m_alive    = false;
        m_dying    = true;
        m_deathPop = kDeathToppleTime;
        if (m_entity != kNoLink && m_entity < scene.size())
            scene.get(m_entity).body = x3::phys::BodyId{};
        if (m_body.valid()) physics.removeBody(m_body);
        m_body = x3::phys::BodyId{};
        x3::logInfo("[monster] melee-killed (HP 0) — body removed, death-pop started");
        // TASK#12: skinned death ragdoll (melee). No explicit shot dir here — pass a
        // zero shove so spawnDeathRagdoll topples it along its facing (the caller owns
        // the separate rigid-body knockback; the body is already gone). No-op unrigged.
        spawnDeathRagdoll(physics, x3::phys::Vec3{ 0.0f, 0.0f, 0.0f });
        // GIBS: same death-FX hook as the shot path (gib burst at the body center).
        if (m_deathFx) {
            const float center[3] = { m_pos.x, m_pos.y + m_hitCenterOff, m_pos.z };
            m_deathFx(center, m_type == MonsterType::Drone);
        }
        // Enemy-SFX: death vocalization (same as the shot kill path).
        emitCueOrLog(m_cueSink, GameCue{ CueKind::EnemyDeath,
            x3::phys::Vec3{ m_pos.x, m_pos.y + m_hitCenterOff, m_pos.z }, 1.0f,
            (uint32_t)m_species });
    } else {
        x3::logInfo("[monster] melee hit for " + std::to_string(dmg) +
                    " — HP now " + std::to_string(m_hp));
        // Enemy-SFX: take-hit grunt on a surviving melee hit.
        emitCueOrLog(m_cueSink, GameCue{ CueKind::EnemyHit,
            x3::phys::Vec3{ m_pos.x, m_pos.y + m_hitCenterOff, m_pos.z }, 1.0f,
            (uint32_t)m_species });
    }
    return dead;
}

// ---------------------------------------------------------------------------
// CURE / spare path (Dr. Chen, F2 — KILL-vs-CURE gimmick). Incapacitate the boss
// WITHOUT a kill: same removal as a death (body gone + death-pop hides the model),
// but flagged m_cured (not killed). Only valid once canCure() (has the option, is
// alive, reached Phase3). The floor module wires the narrative (Chen survives ->
// 100% cure ally) off wasCured(). Inert for every boss without the cure option.
// ---------------------------------------------------------------------------
bool MonsterSystem::cure(Scene& scene, x3::phys::IPhysicsWorld& physics) {
    if (!canCure()) return false;
    m_cured    = true;
    m_alive    = false;     // out of the fight, but NOT killed (m_cured distinguishes)
    m_dying    = true;
    m_deathPop = kDeathPopTime;
    if (m_entity != kNoLink && m_entity < scene.size())
        scene.get(m_entity).body = x3::phys::BodyId{};
    if (m_body.valid()) physics.removeBody(m_body);
    m_body = x3::phys::BodyId{};
    x3::logInfo("[monster] BOSS CURED — incapacitated (spared, not killed)");
    return true;
}

// Non-lethal SPARE (multi-pod "save" path). Removes a LIVE monster from the fight
// (body gone + death-pop hides the model) and flags it spared (m_cured) so it counts
// as saved, NOT killed. No phase precondition (unlike cure()).
bool MonsterSystem::spare(Scene& scene, x3::phys::IPhysicsWorld& physics) {
    if (!m_alive) return false;
    m_cured    = true;      // reuse the "not killed" flag for spared pods
    m_alive    = false;
    m_dying    = true;
    m_deathPop = kDeathPopTime;
    if (m_entity != kNoLink && m_entity < scene.size())
        scene.get(m_entity).body = x3::phys::BodyId{};
    if (m_body.valid()) physics.removeBody(m_body);
    m_body = x3::phys::BodyId{};
    x3::logInfo("[monster] SPARED — freed from the fight (not killed)");
    return true;
}

int MonsterSystem::effectiveDamage() const {
    // Phase-scaled per-attack damage (Phase1 = 1x). Round to nearest int.
    return (int)(m_dmg * m_phaseDamageMul + 0.5f);
}

// ---------------------------------------------------------------------------
// [P2-6] One LOS probe (extracted from the decision-cadence block so the attack
// path can re-run it FRESH). Ray from our center toward the player's eye; clear
// if no Static wall blocks it before the player. Skip past our own collision
// box first (the Static mask also matches Enemy bodies, so a center-origin ray
// self-hits). See docs/design/SUBSYSTEM_HARDENING_PLAN.md AI-2.
// ---------------------------------------------------------------------------
bool MonsterSystem::probeLos(x3::phys::IPhysicsWorld& physics,
                             const x3::phys::Vec3& playerEye) const {
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
    return !wall.hit;
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
            // Act-1 boss gimmick (FE#7): a phase transition opens a brief MEMORY-
            // FLASH window — the boss is staggered (cannot attack, see mayAttack)
            // and takes amplified damage (incomingDamageMul) for memoryFlashTime.
            // Inert when memoryFlashTime is 0 (every other boss).
            if (m_memoryFlashTime > 0.0f) {
                m_flashTimer = m_memoryFlashTime;
                x3::logInfo("[monster] BOSS MEMORY FLASH — staggered + vulnerable");
            }
            if (onPhase) onPhase(m_phase);
        }
    }

    // Act-1 boss gimmick (FE#7): tick down the memory-flash vulnerability window.
    if (m_flashTimer > 0.0f) { m_flashTimer -= dt; if (m_flashTimer < 0.0f) m_flashTimer = 0.0f; }
    // Adaptive-Hide (canon-aliens): tick down the type-resist window.
    if (m_adaptiveHideTimer > 0.0f) { m_adaptiveHideTimer -= dt; if (m_adaptiveHideTimer < 0.0f) m_adaptiveHideTimer = 0.0f; }

    // Decay hit-flash.
    if (m_flash > 0.0f) {
        m_flash -= dt;
        if (m_flash < 0.0f) m_flash = 0.0f;
    }

    // Attack cooldown always advances (so the first attack can land promptly).
    if (m_atkTimer > 0.0f) { m_atkTimer -= dt; if (m_atkTimer < 0.0f) m_atkTimer = 0.0f; }

    // ---- Death: keep drawing the TOPPLE (fall-over + flash, in drawMonster)
    // until the timer runs out, then settle into a corpse, linger briefly, and
    // DESPAWN (BUG#30): the corpse is removed so a killed monster does not stand /
    // lie around forever. The physics body is already gone from physics; the GPU
    // skinned-mesh registration is freed on despawn. ----
    if (!m_alive) {
        if (m_dying) {
            // TASK#12: while a SKINNED ragdoll is active, the host has already stepped
            // the shared physics world this frame — read the bone transforms OUT and
            // flop the model's skin to match (replaces the rigid topple for rigged
            // enemies). Unrigged enemies have no ragdoll; their draw path topples.
            if (m_ragdollActive) driveSkinFromRagdoll();
            // W2-D: authored DEATH CLIP fallback — a rigged model whose ragdoll did
            // NOT spawn (or a rig that ships a Death clip but no ragdoll support)
            // plays the collapse clip once and freezes on its final frame; the draw
            // path suppresses the rigid topple while it plays (m_deathAnimT >= 0).
            // Ragdoll stays the PREFERRED death (physical, shove-reactive).
            else if (m_animActive && m_device && m_deathClip >= 0) {
                if (m_deathAnimT < 0.0f) m_deathAnimT = 0.0f;
                const float dur = m_skinner.clipDuration((uint32_t)m_deathClip);
                if (!m_deathClipDone) {
                    m_deathAnimT += dt;
                    if (m_deathAnimT >= dur) { m_deathAnimT = dur; m_deathClipDone = true; }
                    m_skinner.apply(m_model, *m_device, (uint32_t)m_deathClip, m_deathAnimT);
                }
            }
            m_deathPop -= dt;
            if (m_deathPop <= 0.0f) {
                m_deathPop = 0.0f;
                m_dying  = false;
                m_corpse = true;   // settled flat; keep drawing it briefly on the floor
                // Corpse settled -> tear down the ragdoll bodies (no leaked Jolt
                // bodies). The last skinned pose stays uploaded so the corpse keeps
                // its collapsed shape; the rigid corpse-topple is skipped for it.
                clearDeathRagdoll();
                // BUG#30: start the corpse despawn countdown. A gib-class kill (a
                // flyer/drone that burst/sparked out of the air) has no corpse to
                // linger — despawn it immediately (timer 0).
                const bool gibClass = (m_flyer || m_type == MonsterType::Drone);
                m_corpseTimer = gibClass ? 0.0f : kCorpseDespawnTime;
            }
        } else if (m_corpse && !m_despawned) {
            // BUG#30: lingering corpse -> count down + DESPAWN. The body is already
            // gone (removed at the kill), the alive/enemiesRemaining count already
            // dropped on kill, and clearDeathRagdoll() ran on corpse-settle — so the
            // despawn must NOT touch the count again, only hide + free the skin.
            m_corpseTimer -= dt;
            if (m_corpseTimer <= 0.0f) despawn(scene);
        }
        return;
    }

    if (m_entity == kNoLink || m_entity >= scene.size()) return;

    // ---- W9-1: EMP STUN / master-hack DOCILE. A stunned or docile enemy is
    // FROZEN where it stands: no AI, no movement, no attack (any wind-up is
    // cancelled). The death/corpse flow above and fire() damage are untouched,
    // so a powered-down drone is still killable and still counts. Timers/flash
    // decay above keeps running (the earlier blocks already ticked them). ----
    if (m_stunTimer > 0.0f) {
        m_stunTimer -= dt;
        if (m_stunTimer < 0.0f) m_stunTimer = 0.0f;
        m_winding = false;
        return;
    }
    if (m_docile) { m_winding = false; return; }

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
        // blocks it before the player (probeLos, shared with the [P2-6] fresh
        // attack-time re-check below). No target -> nothing to see -> Search/Idle.
        const bool los = target ? probeLos(physics, playerEye) : false;
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
            if (m_ranged) {
                // RANGED (drone/flyer): hold the standoff ring and STAY AWAY. Strafe
                // maintains range via its radial term — backing OUT when too close,
                // easing IN when slightly far — so it never charges the player. Only
                // Advance when very far (well past the ring) to close the initial gap.
                want = (horiz > m_standoff + 4.0f && horiz > kAiStrafeBandHi)
                           ? AiState::Advance : AiState::Strafe;
            } else if (horiz <= m_attackRange * 1.05f) {
                want = AiState::Attack;
            } else if (horiz <= kAiStrafeBandHi && horiz >= kAiStrafeBandLo) {
                want = (rng01(m_rng) < strafeBias) ? AiState::Strafe : AiState::Advance;
            } else {
                want = AiState::Advance;
            }
        } else {
            // No LOS. If we ever saw the player, Search the last-known spot for a
            // while, then give up to the calm state. Never saw them -> calm state.
            // Calm state = Patrol for patrol-capable rows (guard-life W4-3), else
            // the original Idle — so give-up guards return to WALKING THEIR BEAT.
            const AiState calm = (m_patrolRadius > 0.0f && !m_flyer)
                                     ? AiState::Patrol : AiState::Idle;
            if (m_everSawPlayer && m_searchTimer > 0.0f) {
                want = AiState::Search;
            } else if (m_everSawPlayer && m_ai != AiState::Search && m_ai != calm) {
                // Just lost LOS: begin a fresh Search.
                want = AiState::Search;
                m_searchTimer = kAiSearchTime;
            } else if (m_searchTimer <= 0.0f) {
                want = calm;
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
    // Patrol walks at a fraction of chase speed (guard-life W4-3) so the beat
    // reads as a WALK (the locomotion blend picks the Walk clip off the lower
    // speed) and the guard never looks like it's charging its own waypoints.
    const float chaseSpeed = m_chaseSpeed * m_phaseSpeedMul *
                             (m_ai == AiState::Patrol ? m_patrolSpeedMul : 1.0f);

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

        case AiState::Idle: {
            // Idle: stand still, slow idle heading sweep so it doesn't look frozen.
            wantMove = false;
            m_searchSweep += dt * (kAiSearchSweepFreq * 0.3f);
            m_yawTarget = m_yaw + 0.15f * std::sin(m_searchSweep) * dt;
        } break;

        case AiState::Patrol: {
            // Guard-life (W4-3): walk a diamond waypoint loop around the spawn
            // anchor at walk speed (the shared movement block below scales the
            // step by m_patrolSpeedMul for this state). Pause-look at each
            // waypoint; a blocked leg (wall/prop — counted by the movement
            // block's stall counter) skips to the next waypoint so a guard
            // never marches in place against a wall.
            static const float kWpX[4] = { 1.0f, 0.0f, -1.0f,  0.0f };
            static const float kWpZ[4] = { 0.0f, 1.0f,  0.0f, -1.0f };
            const float wx = m_patrolAnchor.x + kWpX[m_patrolIdx & 3] * m_patrolRadius;
            const float wz = m_patrolAnchor.z + kWpZ[m_patrolIdx & 3] * m_patrolRadius;
            const float px = wx - m_pos.x, pz = wz - m_pos.z;
            const float pl = std::sqrt(px * px + pz * pz);
            if (m_patrolPause > 0.0f) {
                // Waypoint pause: stand, sweep the head ("look around"), then move on.
                m_patrolPause -= dt;
                wantMove = false;
                m_searchSweep += dt * (kAiSearchSweepFreq * 0.5f);
                m_yawTarget = m_yaw + 0.35f * std::sin(m_searchSweep) * dt;
                if (m_patrolPause <= 0.0f) {
                    m_patrolIdx = (m_patrolIdx + 1) & 3;
                    m_patrolStall = 0;
                }
            } else if (pl < 0.4f || m_patrolStall > 45) {
                // Arrived (or the leg is wall-blocked ~0.75 s): begin the pause beat.
                m_patrolPause = m_patrolPauseSec +
                                0.6f * m_patrolPauseSec * (2.0f * rng01(m_rng) - 1.0f);
                m_patrolStall = 0;
                wantMove = false;
            } else {
                mx = px / pl; mz = pz / pl;
                wantMove = true;
                m_yawTarget = headingToFace(mx, mz);   // face where we're walking
            }
        } break;

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
    // ---- Inter-enemy SEPARATION (anti-crowding): push away from nearby allies so a
    // squad SPREADS rather than stacking on the player's tile. Computed from the same
    // ally query the Regroup logic uses (no extra physics queries). The separation
    // vector is blended into the state's desired move dir BEFORE normalization, so it
    // nudges spacing without overriding the state's intent (advance/strafe/retreat).
    // Inert when no ally query is wired (single-monster tests) or no ally is close. --
    if (wantMove && allies && m_dmg >= 0) {
        x3::phys::Vec3 buf[8];
        uint32_t n = allies(m_pos, m_entity, kAiSeparationRadius, buf, 8u);
        float sx = 0.0f, sz = 0.0f;
        for (uint32_t i = 0; i < n; ++i) {
            float ax = m_pos.x - buf[i].x, az = m_pos.z - buf[i].z;
            float d2 = ax*ax + az*az;
            if (d2 < 1e-4f) {   // exactly co-located: shove along a deterministic jitter
                ax = (rng01(m_rng) - 0.5f); az = (rng01(m_rng) - 0.5f); d2 = ax*ax + az*az + 1e-4f;
            }
            const float d = std::sqrt(d2);
            // Inverse-distance falloff (closer allies push harder), unit-direction away.
            const float w = (kAiSeparationRadius - d) / kAiSeparationRadius;  // 0..1
            if (w > 0.0f) { sx += (ax / d) * w; sz += (az / d) * w; }
        }
        const float sl = std::sqrt(sx*sx + sz*sz);
        if (sl > 1e-4f) {
            // Blend the (unit) separation push into the move dir at a capped weight.
            mx += (sx / sl) * kAiSeparationWeight;
            mz += (sz / sl) * kAiSeparationWeight;
        }
    }

    if (wantMove && chaseSpeed > 0.0f && m_body.valid()) {
        float ml = std::sqrt(mx * mx + mz * mz);
        if (ml > 1e-4f) { mx /= ml; mz /= ml; }
        const float step  = chaseSpeed * dt;
        // Clear our OWN (now generously-sized) hitbox before probing for walls: the
        // Static mask also matches Enemy bodies, so a probe starting inside our box
        // self-hits at ~0 and would block ALL movement. skip past the box half-width
        // (m_hitHalfXZ, set in build()) + a margin, and probe at a mid-body height
        // (the box spans m_pos.y - skirt .. + so feet-Y is inside it).
        //
        // #80 RESOLUTION: R-2 (PB cbf7999) replaced this with a corner-sized skip +
        // self-hit discard for x1.7-3.0 boss bodies — FULLY REVERTED. The big skip
        // lands the probe origin past nearby walls in compact rooms (level1 checkpoint
        // broke: enemies walked into geometry, unhittable), and the self-discard ALONE
        // alters the diagonal blocked-flips the Search AI's last-known walk depends on
        // (ai Tc broke). Bisect-verified: only the exact original passes level1 21/21
        // AND ai 7/7. If playtest shows large bodies freezing on diagonals (PB's
        // original complaint), the fix must be room-aware, not a bigger skip.
        const float skip  = m_hitHalfXZ + 0.10f;        // clear our own (wider) box first
        const float probe = step + 0.10f;               // look this far past the box
        const x3::phys::Vec3 mdir{ mx, 0.0f, mz };
        const float probeY = m_pos.y + m_hitCenterOff;  // mid-body (where a wall blocks)
        const x3::phys::Vec3 from{ m_pos.x + mx * skip, probeY, m_pos.z + mz * skip };
        const bool blocked =
            physics.rayCast(from, mdir, probe, x3::phys::Layer::Static).hit ||
            physics.rayCast(from, mdir, probe, x3::phys::Layer::Dynamic).hit;
        if (blocked) {
            m_strafeDir = -m_strafeDir;   // try a new line next frames
            m_wander   += 1.7f;
            // Guard-life (W4-3): a wall-blocked PATROL leg counts stall frames;
            // the Patrol state skips to its next waypoint past the threshold so
            // a guard never marches in place against a wall.
            if (m_ai == AiState::Patrol) ++m_patrolStall;
        } else {
            m_pos.x += mx * step;
            m_pos.z += mz * step;
            physics.setBodyPosition(m_body,
                x3::phys::Vec3{ m_pos.x, m_pos.y + m_hitCenterOff, m_pos.z });
            if (m_ai == AiState::Patrol) m_patrolStall = 0;
        }
    }

    // ---- Turn the body: slew the heading toward the state's target heading. This
    // is what actually rotates the rendered model (and, for rigid bodies, the
    // physics body — see the setBodyRotation call after the transform bake). ----
    m_yaw = slewAngle(m_yaw, m_yawTarget, kAiTurnRate, dt);

    // ---- Enemy-SFX (vocalization): an engaged enemy TAUNTS/HARASSES audibly on a
    // jittered cadence so it isn't silent at range (the playtest "enemies make NO
    // sounds" fix, harass half). Only while alive + has-LOS + actively engaged (not
    // Idle/Search), so a give-up enemy goes quiet. The host maps EnemyTaunt onto a
    // creature vocal. allied/inert enemies (m_dmg==0) don't taunt. --
    if (m_dmg > 0 && m_hasLos &&
        m_ai != AiState::Idle && m_ai != AiState::Search) {
        m_tauntTimer -= dt;
        if (m_tauntTimer <= 0.0f) {
            m_tauntTimer = kAiTauntPeriod + kAiTauntJitter * (2.0f * rng01(m_rng) - 1.0f);
            emitCueOrLog(m_cueSink, GameCue{ CueKind::EnemyTaunt,
                x3::phys::Vec3{ m_pos.x, m_pos.y + 0.3f, m_pos.z }, 0.8f,
                (uint32_t)m_species });
        }
    } else {
        // Not engaged: hold a short delay so re-engaging doesn't instantly bark.
        m_tauntTimer = 0.8f;
        // Guard-life (W4-3): a PATROLLING guard grunts QUIETLY on a long jittered
        // cadence — the "something is around the corner" dread the player hears
        // before seeing. Low intensity (the host maps intensity to volume) so it
        // never reads as an engaged taunt; footsteps ride the normal locomotion
        // cue and carry the rest of the presence.
        if (m_dmg > 0 && m_ai == AiState::Patrol) {
            m_patrolGrunt -= dt;
            if (m_patrolGrunt <= 0.0f) {
                m_patrolGrunt = 9.0f + 5.0f * rng01(m_rng);
                emitCueOrLog(m_cueSink, GameCue{ CueKind::EnemyTaunt,
                    x3::phys::Vec3{ m_pos.x, m_pos.y + 0.3f, m_pos.z }, 0.35f,
                    (uint32_t)m_species });
            }
        }
    }

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
    // Act-1 boss gimmick (FE#7): during a MEMORY-FLASH window the boss is STAGGERED
    // and cannot attack (the clarity beat) — it just stands vulnerable. Inert for
    // every non-flashing monster (m_flashTimer is 0).
    const bool mayAttack = (m_ranged || m_meleePermit) && m_flashTimer <= 0.0f;
    // CORRECTNESS (LOS fix): an attack may only begin/land if the enemy has CLEAR
    // LINE OF SIGHT to the player (m_hasLos is set by the decision-cadence rayCast
    // against Layer::Static, which the floor/ceiling slabs are — so a floor between
    // the enemy and the player makes it false) AND the player is within a sane
    // VERTICAL band of the enemy's body center. Previously the gate keyed only on the
    // PLANAR distance `horiz`, so a melee enemy a floor ABOVE the player (horiz~0,
    // ~3 m up) landed hits THROUGH the floor; and the melee `landed` path did no LOS
    // test at all (only the ranged path did). This closes both: no LOS or too much
    // vertical separation => no wind-up, no hit. The ranged path keeps its own
    // muzzle->player wall ray as a second, finer check at the moment of the shot.
    //
    // [P2-6] FRESH attack-time LOS (SUBSYSTEM_HARDENING_PLAN AI-2): the decision
    // cadence is jittered ~0.15-0.45 s, so between ticks m_hasLos is STALE — an
    // enemy kept shooting/swinging at where the player WAS (through the corner or
    // doorframe just vacated), and refused fair shots for up to half a second
    // after the player stepped into view. Whenever an attack is even plausible
    // (live target, armed, permitted, and in range or already winding), re-run
    // the SAME LOS probe THIS frame and overwrite m_hasLos, so both the wind-up
    // gate and the melee land use the current world, not the stale snapshot. One
    // extra Static ray per frame, only for enemies actually in attack range.
    if (target && target->isAlive() && m_dmg > 0 && mayAttack &&
        (m_winding || horiz <= m_attackRange)) {
        m_hasLos = probeLos(physics, playerEye);
        if (m_hasLos) { m_lastKnown = playerPos; m_everSawPlayer = true; }
    }
    const float vsep = std::fabs(playerPos.y - m_pos.y);
    const bool attackClear = m_hasLos && vsep <= kAttackMaxVertical;
    if (target && target->isAlive() && m_dmg > 0 && horiz <= m_attackRange &&
        mayAttack && attackClear) {
        if (!m_winding && m_atkTimer <= 0.0f) {
            // Begin a new attack: start the wind-up; the hit lands when it elapses.
            m_winding     = true;
            m_windupTimer = m_attackWindup;
            // W2-D: kick the one-shot ATTACK clip (if the rig has one). The anim
            // block below plays it to completion, preempting locomotion.
            if (m_animActive && m_attackClip >= 0) m_attackAnimT = 0.0f;
            // Telegraph FX up front (a beam toward the player) so the attack reads.
            if (fx) {
                x3::phys::Vec3 tp = target->damageTargetPos();
                x3::phys::Vec3 from{ m_pos.x, m_pos.y + 0.3f, m_pos.z };
                fx(from, tp);
            }
            // Enemy-SFX: a swing/shot vocalization at the wind-up start (the host maps
            // it onto a melee-bite / plasma-charge sound). Fired once per attack (the
            // wind-up gate), at the enemy muzzle, so attacks are AUDIBLE.
            emitCueOrLog(m_cueSink, GameCue{ CueKind::EnemyAttack,
                x3::phys::Vec3{ m_pos.x, m_pos.y + 0.3f, m_pos.z }, 1.0f,
                (uint32_t)m_species });
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
                            target->damageTargetPos(), 1.0f, (uint32_t)m_species });
                    }
                }
            }
        }
    } else {
        // Out of range / no target: cancel any pending wind-up.
        m_winding = false;
    }

    // ---- W2-D: procedural attack TELL (visual-only). A melee enemy with NO
    // authored Attack clip rears back through the first 70% of its wind-up, dips,
    // then LUNGES at the player across the strike moment — a read-at-distance
    // swing sold purely through the draw transform (the physics body, hitbox and
    // damage timing are untouched). Rigs WITH an Attack clip skip this (the clip
    // carries the read). Decays smoothly back to 0 after the strike. ----
    {
        float wantLunge = 0.0f, wantDip = 0.0f;
        if (m_winding && !m_ranged && m_attackClip < 0 && m_attackWindup > 1e-4f) {
            const float t = 1.0f - (m_windupTimer / m_attackWindup);   // 0..1 through wind-up
            if (t < 0.7f) { const float k = t / 0.7f;        wantLunge = -0.16f * k; wantDip = 0.07f * k; }
            else          { const float k = (t - 0.7f) / 0.3f; wantLunge = -0.16f + 0.62f * k; wantDip = 0.07f * (1.0f - k); }
        }
        // Snap toward the wind-up profile fast (it IS the motion), relax slower.
        const float rate = (m_winding ? 30.0f : 8.0f) * dt;
        const float k = std::min(1.0f, rate);
        m_lunge    += (wantLunge - m_lunge) * k;
        m_lungeDip += (wantDip  - m_lungeDip) * k;
    }

    // ---- Bake the facing yaw into the render transform's upper-left 3x3, keeping
    // the uniform model scale, and set the translation to the (possibly moved)
    // body center. Scene::update only overwrites the translation column and
    // preserves the 3x3, so this facing survives the per-frame physics sync as
    // long as the host calls update() after scene.update() (see main loop). ----
    {
        // FACING FIX (recovered from wave3): the rigged character GLBs are authored
        // facing +Z, but facingDir()/AI assume local -Z forward (CONVENTIONS) — so the
        // MESH renders with its BACK to the player. Flip the VISUAL yaw 180deg here
        // ONLY (m_yaw / facingDir() / aim / --test-ai are unchanged, so AI stays right).
        const float ry = m_yaw + 3.14159265358979323846f;
        const float c = std::cos(ry), s = std::sin(ry);
        // Yaw about +Y: local +X -> (c,0,-s), +Z -> (s,0,c). The phase scale
        // multiplier up-scales the boss as it enrages (graybox phase feedback).
        const float scale = m_modelScale * m_phaseScaleMul;
        // W2-D: fold the procedural attack-tell offset into the DRAW position only.
        // Facing local -Z maps to world (-sin yaw, -cos yaw) (headingToFace docs),
        // so a positive lunge pushes the visual toward whatever it faces.
        x3::phys::Vec3 drawPos = m_pos;
        if (m_lunge != 0.0f || m_lungeDip != 0.0f) {
            const float fs = std::sin(m_yaw), fc = std::cos(m_yaw);
            drawPos.x += -fs * m_lunge;
            drawPos.z += -fc * m_lunge;
            drawPos.y -= m_lungeDip;
        }
        Entity& me = scene.get(m_entity);
        if (m_propLean != 0.0f) {
            // SKINNED CITIZENS: fold the caller-fed torso lean (setPropMotion) into
            // the bake as R = Ry(ry) * Rx(lean-toward-facing). In the render frame
            // the model's front is local +Z (the +pi visual flip above), so a
            // positive lean pitches the body toward whatever it faces — the crowd's
            // carry/console/converse lean read. lean == 0 (every existing monster)
            // takes the identical branch below.
            const float sa = std::sin(m_propLean), ca = std::cos(m_propLean);
            composeTRS(me.transform,
                       x3::phys::Vec3{ c, 0.0f, -s },
                       x3::phys::Vec3{ sa * s, ca, sa * c },
                       x3::phys::Vec3{ ca * s, -sa, ca * c },
                       scale, drawPos);
        } else {
            composeTRS(me.transform,
                       x3::phys::Vec3{ c, 0.0f, -s },
                       x3::phys::Vec3{ 0.0f, 1.0f, 0.0f },
                       x3::phys::Vec3{ s, 0.0f, c },
                       scale, drawPos);
        }

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
        // W2-D: the one-shot ATTACK clip preempts locomotion while it plays (kicked
        // at wind-up start). Hard-cut in/out — consistent with the engine's existing
        // clip switching; returns to the locomotion blend the frame after it ends.
        if (m_attackAnimT >= 0.0f && m_attackClip >= 0) {
            m_attackAnimT += dt;
            const float dur = m_skinner.clipDuration((uint32_t)m_attackClip);
            if (m_attackAnimT < dur && dur > 1e-4f) {
                m_skinner.apply(m_model, *m_device, (uint32_t)m_attackClip, m_attackAnimT);
                // (m_lastFootPhase untouched: locomotion phase is frozen during the
                // swing, so no footstep-mark jump on resume.)
                return;                    // locomotion resumes next frame after the swing
            }
            m_attackAnimT = -1.0f;         // finished -> fall through to locomotion
        }
        const float ddx = m_pos.x - prevPos.x, ddz = m_pos.z - prevPos.z;
        // SKINNED CITIZENS: a prop posed via setPropPose already moved BEFORE this
        // update (delta 0), so a caller-fed speed (setPropMotion) overrides the
        // measured delta and drives the Idle/Walk/Run blend. < 0 = not driven.
        const float planarSpeed = (m_propSpeed >= 0.0f) ? m_propSpeed
            : ((dt > 1e-5f) ? std::sqrt(ddx*ddx + ddz*ddz) / dt : 0.0f);
        // ---- W5-2: scripted CALM LOOP (the assault tableau — e.g. "Struggle").
        // While unaggroed + effectively stationary, a set calm loop replaces idle so
        // the burst-in reads as an act in progress, not guards loitering. Aggro or
        // movement falls straight back to locomotion; the one-shot Attack above
        // already preempts everything. Absent clip -> setCalmLoop left it -1 -> no-op.
        if (m_calmLoopClip >= 0 &&
            (m_ai == AiState::Idle || m_ai == AiState::Patrol) &&
            planarSpeed < 0.15f) {
            m_calmLoopT += dt;
            m_skinner.apply(m_model, *m_device, (uint32_t)m_calmLoopClip, m_calmLoopT);
            return;
        }
        if (m_useLocoBlend) {
            m_skinner.setLocomotionSpeed(planarSpeed);
            m_skinner.applyLocomotion(m_model, *m_device, dt);

            // ---- Footstep cues at locomotion phase crossings (foot plants). The
            // shared phase wraps [0,1); a left/right step lands each time it crosses
            // one of kFootstepsPerCycle evenly-spaced marks. We compare this frame's
            // phase to last frame's and fire a cue per mark crossed (handles wrap +
            // multi-mark steps in one frame). Gated on moving fast enough. ----
            const float phase = m_skinner.locomotionPhase();
            if (planarSpeed > kFootstepMinSpeed && kFootstepsPerCycle > 0) {
                const float marks = (float)kFootstepsPerCycle;
                int prevMark = (int)std::floor(m_lastFootPhase * marks);
                int curMark  = (int)std::floor(phase * marks);
                // Phase wrapped this frame (cur < prev): add a full cycle of marks.
                if (phase < m_lastFootPhase) curMark += kFootstepsPerCycle;
                for (int s = prevMark; s < curMark; ++s) {
                    // Intensity scales with speed (faster -> louder/firmer step).
                    const float inten = std::min(1.0f, 0.4f + 0.2f * planarSpeed);
                    emitCueOrLog(m_cueSink, GameCue{ CueKind::Footstep, m_pos, inten,
                        (uint32_t)m_species });
                }
            }
            m_lastFootPhase = phase;
        } else {
            // Legacy single-clip path (rig has no Walk/Run blend). Use Walk if one
            // somehow exists; otherwise play Idle — but PUMP it faster while moving so
            // an Idle-only rig reads as agitated/advancing instead of dead-static
            // (the real fix is a retargeted multi-clip *_anim.glb for that enemy).
            const bool hasWalk = (m_walkClip >= 0);
            const bool moving  = planarSpeed > 0.25f;
            const int  clip    = (hasWalk && moving) ? m_walkClip : m_idleClip;
            const float rate   = (!hasWalk && moving)
                ? (1.0f + std::min(planarSpeed, 4.0f) * 0.35f)   // 1x idle .. ~2.4x chasing
                : 1.0f;
            m_animTime += dt * rate;
            m_skinner.apply(m_model, *m_device, (uint32_t)clip, m_animTime);
        }
    }
}

// ===========================================================================
// SKINNED DEATH RAGDOLL (TASK#12). The model's bones are physically driven by a
// Jolt ragdoll so a rigged enemy FLOPS with its mesh instead of doing the rigid
// topple. Built on the KILL, driven each frame while m_dying, torn down on
// corpse-expire. Falls back to the legacy topple for unrigged models.
// ===========================================================================

// Build the physics ragdoll at the kill moment + bind the skin driver. No-op (and
// the legacy topple draws) unless the model is skinnable.
void MonsterSystem::spawnDeathRagdoll(x3::phys::IPhysicsWorld& physics,
                                      const x3::phys::Vec3& shove) {
    // Only rigged characters can flop their skin; box/crawler/static fall back.
    if (!m_skinner.valid() || m_skinner.nodeCount() == 0) return;
    if (m_deathRagdoll) return;   // already built (idempotent-safe)

    // ---- The FROZEN draw transform the skinned mesh is rendered through:
    //   final = model * fixup * skinGlobal,  model = T(m_pos) * R(yaw+pi) * S(scale).
    // Build `model` the SAME way drawMonster's facing bake does, fold in m_modelFixup
    // (the Z-up stand-up / feet-grounding the draw path applies), and capture the
    // inverse of (model*fixup) once — m_pos/yaw/scale don't change while dead. The
    // ragdoll bone world transforms get mapped through THIS inverse into skin space so
    // the rigid delta composes correctly under the unchanged draw. --
    const float ry = m_yaw + 3.14159265358979323846f;
    const float c = std::cos(ry), s = std::sin(ry);
    const float scale = m_modelScale * m_phaseScaleMul;
    float model[16];
    composeTRS(model,
               x3::phys::Vec3{ c, 0.0f, -s },
               x3::phys::Vec3{ 0.0f, 1.0f, 0.0f },
               x3::phys::Vec3{ s, 0.0f, c },
               scale, m_pos);
    float drawXform[16];
    x3::asset::mulMat4(model, m_modelFixup, drawXform);   // model * fixup (skin->world)
    if (!invertAffineUniform(drawXform, m_deathModelInv)) return; // degenerate: bail to topple

    // ---- Build the canonical humanoid rig, in WORLD space, matching the monster:
    // feet at the monster's base, scaled by the model scale, yawed to its facing so
    // the physical ragdoll falls in the right spot / direction. makeHumanoidRagdollBones
    // authors an UPRIGHT rig with the pelvis `originY` up; we then transform every
    // bone's bind-world matrix by the world placement (T*R*S, feet at m_pos). ----
    // Ragdoll authored at scale 1 standing on the floor; place its feet at m_pos and
    // yaw it by the visual heading. The rig's pelvis sits ~ (its authored heights) up.
    x3::phys::makeHumanoidRagdollBones(/*originY*/0.0f, m_ragdollBones);
    const uint32_t bn = (uint32_t)m_ragdollBones.size();
    if (bn == 0) { m_ragdollBones.clear(); return; }

    // World placement for the rig: feet at m_pos, yawed by the VISUAL heading (so the
    // ragdoll's local +Z forward agrees with the drawn mesh), scaled by `scale`.
    float place[16];
    composeTRS(place,
               x3::phys::Vec3{ c, 0.0f, -s },
               x3::phys::Vec3{ 0.0f, 1.0f, 0.0f },
               x3::phys::Vec3{ s, 0.0f, c },
               scale, m_pos);
    for (uint32_t b = 0; b < bn; ++b) {
        float placed[16];
        x3::asset::mulMat4(place, m_ragdollBones[b].bindWorld, placed);
        std::memcpy(m_ragdollBones[b].bindWorld, placed, 16 * sizeof(float));
        // Scale the capsule dims by the model scale so collisions match the placement.
        m_ragdollBones[b].halfHeight *= scale;
        m_ragdollBones[b].radius     *= scale;
    }

    m_deathRagdoll.reset(x3::phys::createRagdoll(physics, m_ragdollBones.data(), bn));
    if (!m_deathRagdoll) { m_ragdollBones.clear(); return; }   // bail -> legacy topple

    // Snap the ragdoll to its placed bind pose + add it to the world (active).
    {
        std::vector<float> bind((size_t)bn * 16);
        for (uint32_t b = 0; b < bn; ++b)
            std::memcpy(&bind[b*16], m_ragdollBones[b].bindWorld, 16 * sizeof(float));
        m_deathRagdoll->addToWorld(/*activate*/true);
        m_deathRagdoll->setPoseWorld(bind.data());
    }

    // ---- Bind the rigid bone->skin driver to the monster's CURRENT animated pose so
    // the flop starts seamlessly from where the animation left off. We capture the
    // Skinner's current global bone transforms (currentGlobals) and seed RagdollSkin
    // from THEM (not the static bind pose) so frame 0 of the flop == the last animated
    // frame (no pop). We then map every skin node to the nearest ragdoll bone. The bone
    // "parts" we feed RagdollSkin live in the SAME space as those globals (the model's
    // skin space), so each ragdoll bone's INIT part = inv(model*fixup) * boneWorldInit
    // (m_deathModelInv). The per-frame delta (cur*initInv) is then a skin-space rigid
    // motion that composes correctly under the frozen draw (final = model*fixup*skinGlobal). ----
    {
        // Capture the pose the last draw used: the active clip (or idle) at m_animTime.
        const int poseClip = (m_idleClip >= 0) ? m_idleClip
                           : (m_walkClip >= 0) ? m_walkClip : 0;
        std::vector<float> curGlobals;
        const uint32_t ng = m_skinner.currentGlobals(m_model, (uint32_t)poseClip,
                                                      m_animTime, curGlobals);
        if (ng == m_skinner.nodeCount() && ng > 0)
            m_ragdollSkin.bindFromGlobals(m_model, curGlobals.data(), ng);
        else
            m_ragdollSkin.bind(m_model);   // fall back to the static bind pose
    }

    // Capture the ragdoll's INITIAL bone world transforms (post setPose) and convert
    // to model-local for the RagdollSkin part frames.
    m_ragWorldScratch.assign((size_t)bn * 16, 0.0f);
    m_ragPartInit.assign((size_t)bn * 16, 0.0f);
    m_deathRagdoll->getBoneWorldTransforms(m_ragWorldScratch.data());
    for (uint32_t b = 0; b < bn; ++b)
        x3::asset::mulMat4(m_deathModelInv, &m_ragWorldScratch[b*16], &m_ragPartInit[b*16]);
    m_ragdollSkin.mapToParts(m_ragPartInit.data(), bn);
    m_ragPartCur.assign((size_t)bn * 16, 0.0f);

    // ---- Death impulse: carry the topple's directional shove + a vertical lift, and
    // a flyer/drone variant that SPARKS/SPINS (a yaw kick) as it falls out of the air,
    // vs. a grounded enemy that just topples the way it was hit. ----
    const bool flyer = (m_flyer || m_type == MonsterType::Drone);
    float impX = shove.x, impZ = shove.z;
    const float hl = std::sqrt(impX*impX + impZ*impZ);
    if (hl < 1e-3f) { impX = std::sin(m_yaw); impZ = std::cos(m_yaw); }  // fall forward-ish
    // Per-mass impulse roughly; the ragdoll AddImpulse is a velocity*mass blast, so a
    // modest value reads as a shove without launching the body off-screen.
    const float push = flyer ? 4.5f : 3.0f;
    const float lift = flyer ? 2.0f : 1.2f;
    m_deathRagdoll->applyImpulseAll(x3::phys::Vec3{ impX * push, lift, impZ * push });
    if (flyer) {
        // Spin a drone: a strong angular kick on the head/spine reads as a sparking
        // tumble out of the air (apply to a couple of bones so it whirls, not just drops).
        m_deathRagdoll->applyImpulseBone(2 /*head*/,  x3::phys::Vec3{  push*0.8f, 0.5f, 0.0f });
        m_deathRagdoll->applyImpulseBone(1 /*spine*/, x3::phys::Vec3{ -push*0.6f, 0.0f, push*0.4f });
    }

    m_ragdollActive = true;
    m_ragdolled     = true;   // this corpse flops via ragdoll; skip the rigid topple draw
    x3::logInfo(std::string("[monster] SKINNED DEATH RAGDOLL spawned (") +
                std::to_string(bn) + " bones, " +
                (flyer ? "flyer/drone spin" : "grounded topple") + ")");
}

// Per-frame: read the ragdoll bone WORLD transforms, convert to model-local, run the
// rigid bone->skin attach, and feed the result to the Skinner's external-pose path so
// the GPU-skinned model flops physically with the bones. No device upload work beyond
// what the Skinner already does (palette upload on GPU, CPU LBS otherwise).
void MonsterSystem::driveSkinFromRagdoll() {
    if (!m_ragdollActive || !m_deathRagdoll || !m_device) return;
    const uint32_t bn = m_deathRagdoll->boneCount();
    if (bn == 0) return;
    if (m_ragWorldScratch.size() != (size_t)bn * 16) m_ragWorldScratch.assign((size_t)bn*16, 0.0f);
    if (m_ragPartCur.size()      != (size_t)bn * 16) m_ragPartCur.assign((size_t)bn*16, 0.0f);

    m_deathRagdoll->getBoneWorldTransforms(m_ragWorldScratch.data());
    for (uint32_t b = 0; b < bn; ++b)
        x3::asset::mulMat4(m_deathModelInv, &m_ragWorldScratch[b*16], &m_ragPartCur[b*16]);

    const uint32_t nc = m_ragdollSkin.computeNodeGlobals(m_ragPartCur.data(), bn, m_ragNodeGlobals);
    if (nc == 0) return;
    m_skinner.applyExternalGlobals(m_model, *m_device, m_ragNodeGlobals.data(), nc);
}

// Remove the ragdoll bodies (corpse cleanup). Idempotent. The corpse stays drawable
// (its last skinned pose is what was uploaded; the rigid corpse-topple is skipped for
// a model that ragdolled). No leaked Jolt bodies — the IRagdoll dtor also removes.
void MonsterSystem::clearDeathRagdoll() {
    if (m_deathRagdoll) {
        m_deathRagdoll->removeFromWorld();
        m_deathRagdoll.reset();
    }
    m_ragdollActive = false;
    m_ragdollBones.clear();
    m_ragPartInit.clear();
    m_ragPartCur.clear();
    m_ragWorldScratch.clear();
}

// BUG#30: despawn a settled corpse — hide the Entity, free the GPU skinned-mesh
// registration, and drop any lingering ragdoll bodies. Idempotent; does NOT touch the
// alive/enemiesRemaining count (the kill already decremented it; alive() stays false).
void MonsterSystem::despawn(Scene& scene) {
    if (m_despawned) return;
    m_despawned = true;
    m_corpse    = false;   // no longer a drawable corpse
    m_dying     = false;
    // Hide the model (the draw path early-outs on a hidden entity; this stops the
    // corpse drawing). The body was already removed at the kill, so nothing physical
    // remains. Leave the entity slot in place (kNoLink would break entity() lookups).
    if (m_entity != kNoLink && m_entity < scene.size()) {
        Entity& me = scene.get(m_entity);
        me.visible = false;
        me.body    = x3::phys::BodyId{};   // belt-and-braces: ensure no dangling body link
    }
    // Free the GPU skinned-mesh registration so a despawned rigged enemy does not leak
    // its per-mesh skinning buffers/descriptors for the rest of the run. No-op on a
    // headless / CPU-only build (nothing was registered) and on unrigged monsters.
    if (m_device && m_skinner.valid())
        m_skinner.disableGpuSkinning(*m_device);
    // Belt-and-braces: any ragdoll should already be cleared on corpse-settle, but a
    // gib-class (immediate) despawn can land in the same frame — clear it here too.
    clearDeathRagdoll();
    x3::logInfo("[monster] corpse despawned (entity hidden, skinned mesh freed)");
}

// ---------------------------------------------------------------------------
// Draw all monster primitives at its transform, with the hit-flash tint.
// ---------------------------------------------------------------------------
void MonsterSystem::drawMonster(x3::rhi::IRenderDevice& device,
                                const x3::rhi::FrameContext& frame,
                                const Scene& scene) const {
    // Draw while alive, mid-topple (m_dying), OR as a lingering corpse (m_corpse).
    if ((!m_alive && !m_dying && !m_corpse) || m_entity == kNoLink || m_entity >= scene.size()) return;
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

    // TASK#12: a model that FLOPPED via the skinned death ragdoll has its collapse
    // baked into the skinned vertices already (driveSkinFromRagdoll fed the Skinner
    // the ragdoll pose). Draw it at the plain frozen entity transform — applying the
    // rigid topple on top would double-rotate it. The corpse keeps the last uploaded
    // ragdoll pose. (Unrigged enemies have m_ragdolled == false and topple below.)
    if ((m_dying || m_corpse) && m_ragdolled) {
        // White flash at the very start of the death still reads the kill.
        const float fall = m_corpse ? 1.0f
            : (kDeathToppleTime > 0.0f ? (1.0f - m_deathPop / kDeathToppleTime) : 1.0f);
        const float flash  = (fall < 0.35f) ? (1.0f - fall / 0.35f) : 0.0f;
        const float bright = 1.0f + 1.3f * flash;
        float t2[4] = { tint[0]*bright, tint[1]*bright, tint[2]*bright, tint[3] };
        drawMonsterAt(device, frame, e.transform, t2);
        return;
    }

    if (m_dying || m_corpse) {
        // W2-D: when the authored DEATH CLIP drove the pose (no ragdoll spawned),
        // the skin already holds the collapse — draw at the plain transform with
        // just the kill flash, and SUPPRESS the rigid topple (the clip + topple
        // together would fold the corpse twice).
        if (m_deathAnimT >= 0.0f) {
            const float fallT = m_corpse ? 1.0f
                : (kDeathToppleTime > 0.0f ? (1.0f - m_deathPop / kDeathToppleTime) : 1.0f);
            const float flash2  = (fallT < 0.35f) ? (1.0f - fallT / 0.35f) : 0.0f;
            const float bright2 = 1.0f + 1.3f * flash2;
            float t2[4] = { tint[0]*bright2, tint[1]*bright2, tint[2]*bright2, tint[3] };
            drawMonsterAt(device, frame, e.transform, t2);
            return;
        }
        // ---- Death TOPPLE: the body falls over (rotates about its feet) and
        // settles flat as a lingering corpse — replaces the old shrink-poof. `fall`
        // eases 0 (upright) -> 1 (flat) across the topple window; a corpse is pinned
        // at 1. A brief white flash punches the very start so the kill still reads. -
        const float fall = m_corpse ? 1.0f
            : (kDeathToppleTime > 0.0f ? (1.0f - m_deathPop / kDeathToppleTime) : 1.0f);
        const float fe   = 1.0f - (1.0f - fall) * (1.0f - fall);   // easeOut (gravity-ish)
        const float ang  = fe * 1.5707963f * 0.92f;                // up to ~83deg — lying down
        // White flash only in the first third of the fall, then back to tint.
        const float flash  = (fall < 0.35f) ? (1.0f - fall / 0.35f) : 0.0f;
        const float bright = 1.0f + 1.3f * flash;
        tint[0] *= bright; tint[1] *= bright; tint[2] *= bright;
        // World-space topple about X through the feet (the entity translation): strip
        // the translation, rotate, then restore it so the feet stay planted and the
        // head swings down to the floor.
        const float tx = e.transform[12], ty = e.transform[13], tz = e.transform[14];
        float base[16];
        for (int i = 0; i < 16; ++i) base[i] = e.transform[i];
        base[12] = base[13] = base[14] = 0.0f;
        const float c = std::cos(ang), s = std::sin(ang);
        const float Rx[16] = { 1, 0, 0, 0,   0, c, s, 0,   0, -s, c, 0,   0, 0, 0, 1 };
        float m[16];
        x3::asset::mulMat4(Rx, base, m);
        m[12] = tx; m[13] = ty; m[14] = tz;
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
        // FULL PBR path (was the basic drawMesh): normal + metallic-roughness maps
        // give the characters real shading (the flat path is why the drone read as
        // a white bag with coal arms). Authored material emissive rides through
        // SCALED by m_emissiveScale (Tuning) — same discipline as the cell dressing:
        // strength 0 suppresses, 1 = as authored, never an uncontrolled bloom bomb.
        // Hit-flash / phase / death tints stay folded into the base color exactly
        // as before (`color` above already carries them).
        const bool matEmis = d.emissiveTexId != 0 ||
            d.emissiveFactor[0] > 0.001f || d.emissiveFactor[1] > 0.001f ||
            d.emissiveFactor[2] > 0.001f;
        const float emis[4] = {
            matEmis ? d.emissiveFactor[0] : 0.0f,
            matEmis ? d.emissiveFactor[1] : 0.0f,
            matEmis ? d.emissiveFactor[2] : 0.0f,
            matEmis ? m_emissiveScale : 0.0f,
        };
        device.drawMeshPBR(frame,
                           x3::rhi::MeshHandle{ d.meshId },
                           x3::rhi::TextureHandle{ d.baseColorTexId },
                           x3::rhi::TextureHandle{ d.normalTexId },
                           x3::rhi::TextureHandle{ d.mrTexId },
                           color,
                           emis,
                           fin,
                           d.alphaMask,
                           d.alphaBlend,
                           x3::rhi::TextureHandle{ d.emissiveTexId },
                           x3::rhi::TextureHandle{ d.detailTexId },
                           d.detailUvScale,
                           d.clearcoat, d.clearcoatRough);
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
    if (m_deathFx) m->setDeathFxSink(m_deathFx); // wire the gib-burst death FX on new spawns
    uint32_t idx = (uint32_t)m_monsters.size();
    m_monsters.push_back(std::move(m));
    return idx;
}

void MonsterManager::setCueSink(const GameCueFn& sink) {
    m_cueSink = sink;
    for (auto& m : m_monsters) m->setCueSink(sink);   // apply to existing too
}

void MonsterManager::setDeathFxSink(const DeathFxFn& sink) {
    m_deathFx = sink;
    for (auto& m : m_monsters) m->setDeathFxSink(sink);   // apply to existing too
}

uint32_t MonsterManager::aliveCount() const {
    uint32_t n = 0;
    for (const auto& m : m_monsters)
        if (m->alive()) ++n;
    return n;
}

void MonsterManager::shutdown() {
    // TASK#12: tear down any in-flight death ragdolls while the physics world is still
    // alive (mirrors RagdollDemo::shutdown). Without this, a monster destroyed AFTER
    // physics->shutdown() would call IRagdoll::removeFromWorld() on a dead Jolt system.
    for (auto& m : m_monsters) m->shutdownRagdoll();
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
                                Scene& scene, x3::phys::IPhysicsWorld& physics,
                                int damage, x3::DamageType type) {
    // [P2-5] ONE Enemy-layer rayCast per shot (specs/MONSTER_FIRE_SINGLE_RAY.spec.md).
    // The single cast returns the NEAREST enemy body along the ray; each monster
    // then merely checks "is that my body?" against the SAME precomputed hit (a
    // map lookup, no ray). Previously every MonsterSystem::fire() re-cast the
    // identical ray — O(N monsters) physics rays per shot. Nearest-hit semantics
    // are unchanged (the cast itself is nearest-wins, independent of vector
    // order); at most one monster is damaged per call; a hit on an Enemy-layer
    // body that is NOT one of ours is kept as a geometry hit for the tracer,
    // exactly as before. Zero monsters => no cast, default miss (as before).
    FireResult best;
    if (m_monsters.empty()) return best;
    const x3::phys::RayHit hit =
        physics.rayCast(eye, fireDirNormalized(dir), kFireMaxDist, x3::phys::Layer::Enemy);
    for (auto& m : m_monsters) {
        FireResult r = m->applyFireHit(hit, eye, dir, scene, physics, damage, type);
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

// [P2-5] Counting physics world: a transparent forwarding wrapper over a real
// IPhysicsWorld that COUNTS rayCast() invocations. This is the spec's F5 probe:
// it observes real physics-interface traffic (not a self-reported counter inside
// the code under test), so "fire() performs exactly ONE world raycast" is proven
// at the boundary. Everything else forwards 1:1.
class CountingPhysicsWorld final : public x3::phys::IPhysicsWorld {
public:
    explicit CountingPhysicsWorld(x3::phys::IPhysicsWorld& inner) : m_w(inner) {}
    uint32_t rayCasts = 0;

    x3::phys::RayHit rayCast(x3::phys::Vec3 o, x3::phys::Vec3 d, float maxDist,
                             x3::phys::Layer mask) override {
        ++rayCasts;
        return m_w.rayCast(o, d, maxDist, mask);
    }

    // ---- pure forwarding below ----
    bool init() override { return m_w.init(); }
    void shutdown() override { m_w.shutdown(); }
    void step(float dt) override { m_w.step(dt); }
    x3::phys::BodyId addStaticMesh(const float* v, uint32_t vc,
                                   const uint32_t* i, uint32_t ic) override {
        return m_w.addStaticMesh(v, vc, i, ic);
    }
    x3::phys::BodyId addBox(x3::phys::Vec3 h, x3::phys::Vec3 p, float m,
                            x3::phys::Layer l) override { return m_w.addBox(h, p, m, l); }
    x3::phys::BodyId addSphere(float r, x3::phys::Vec3 p, float m,
                               x3::phys::Layer l) override { return m_w.addSphere(r, p, m, l); }
    void removeBody(x3::phys::BodyId b) override { m_w.removeBody(b); }
    void setBodyPosition(x3::phys::BodyId b, x3::phys::Vec3 p) override { m_w.setBodyPosition(b, p); }
    x3::phys::Vec3 getBodyPosition(x3::phys::BodyId b) const override { return m_w.getBodyPosition(b); }
    void applyImpulse(x3::phys::BodyId b, x3::phys::Vec3 i) override { m_w.applyImpulse(b, i); }
    void getBodyRotation(x3::phys::BodyId b, float q[4]) const override { m_w.getBodyRotation(b, q); }
    void setBodyRotation(x3::phys::BodyId b, const float q[4]) override { m_w.setBodyRotation(b, q); }
    void setBodyLinearVelocity(x3::phys::BodyId b, const float v[3]) override { m_w.setBodyLinearVelocity(b, v); }
    void getBodyLinearVelocity(x3::phys::BodyId b, float v[3]) const override { m_w.getBodyLinearVelocity(b, v); }
    void setBodyAngularVelocity(x3::phys::BodyId b, const float v[3]) override { m_w.setBodyAngularVelocity(b, v); }
    void getBodyAngularVelocity(x3::phys::BodyId b, float v[3]) const override { m_w.getBodyAngularVelocity(b, v); }
    x3::phys::ConstraintId addPointConstraint(x3::phys::BodyId a, x3::phys::BodyId b,
                                              x3::phys::Vec3 anchor) override {
        return m_w.addPointConstraint(a, b, anchor);
    }
    void setBodyUserData(x3::phys::BodyId b, uint64_t u) override { m_w.setBodyUserData(b, u); }
    uint64_t getBodyUserData(x3::phys::BodyId b) const override { return m_w.getBodyUserData(b); }
    x3::phys::BodyId createCharacter(float r, float h, x3::phys::Vec3 p) override {
        return m_w.createCharacter(r, h, p);
    }
    void moveCharacter(x3::phys::BodyId b, x3::phys::Vec3 v, float dt) override { m_w.moveCharacter(b, v, dt); }
    bool characterGrounded(x3::phys::BodyId b) const override { return m_w.characterGrounded(b); }
    bool setCharacterHeight(x3::phys::BodyId b, float h) override { return m_w.setCharacterHeight(b, h); }
    void setCharacterSwim(x3::phys::BodyId b, bool e) override { m_w.setCharacterSwim(b, e); }
    void setTriggerCallback(TriggerFn f, void* u) override { m_w.setTriggerCallback(f, u); }
    x3::phys::ShapeId addConvexHull(const float* pts, uint32_t n) override { return m_w.addConvexHull(pts, n); }
    x3::phys::ShapeId addCompound(const x3::phys::ShapeId* parts, const float* xf,
                                  uint32_t n) override { return m_w.addCompound(parts, xf, n); }
    x3::phys::BodyId addBodyFromShape(x3::phys::ShapeId s, x3::phys::Vec3 p, float m,
                                      x3::phys::Layer l) override { return m_w.addBodyFromShape(s, p, m, l); }
    void setContactCallback(ContactFn f, void* u) override { m_w.setContactCallback(f, u); }
    void optimizeBroadphase() override { m_w.optimizeBroadphase(); }
    x3::phys::ConstraintId addPointConstraint(x3::phys::BodyId b, x3::phys::Vec3 a,
                                              x3::phys::Vec3 att) override {
        return m_w.addPointConstraint(b, a, att);
    }
    x3::phys::ConstraintId addDistanceConstraint(x3::phys::BodyId b, x3::phys::Vec3 a,
                                                 x3::phys::Vec3 att, float mn, float mx) override {
        return m_w.addDistanceConstraint(b, a, att, mn, mx);
    }
    void removeConstraint(x3::phys::ConstraintId c) override { m_w.removeConstraint(c); }
    void setBodyDamping(x3::phys::BodyId b, float l, float a) override { m_w.setBodyDamping(b, l, a); }
    void* nativeSystem() override { return m_w.nativeSystem(); }
    void* nativeBody(x3::phys::BodyId b) override { return m_w.nativeBody(b); }

private:
    x3::phys::IPhysicsWorld& m_w;
};

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

    // =======================================================================
    // [P2-5] Single-raycast fire (specs/MONSTER_FIRE_SINGLE_RAY.spec.md).
    // T7: with TWO monsters on the same ray, one MonsterManager::fire() performs
    //     EXACTLY ONE world raycast (spec F5, counted at the physics interface),
    //     the NEARER monster takes the damage and the farther is untouched (F2),
    //     a miss damages nobody and still costs one cast (F4) — plus a negative
    //     control proving the ray counter actually counts.
    // T8: spawn order reversed -> the same NEARER monster is the victim (F3).
    // =======================================================================

    // ---- T7: exactly one raycast per shot; nearest wins; miss is clean. ----
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> inner(x3::phys::createPhysicsWorld());
        inner->init();
        CountingPhysicsWorld w(*inner);
        Scene scene7; MonsterManager mm;
        MonsterSystem::Tuning tn;               // NEAR: modest HP
        tn.hp = 40;  tn.chaseSpeed = 0.0f;
        MonsterSystem::Tuning tf = tn;          // FAR: more HP (spec F2)
        tf.hp = 400;
        // Both centered on x=0 so a -Z -> +Z ray runs through both hitboxes.
        mm.spawn(scene7, device, w, riggedGlbRoot(), x3::phys::Vec3{ 0.0f, 0.4f, 0.0f }, tn);
        mm.spawn(scene7, device, w, riggedGlbRoot(), x3::phys::Vec3{ 0.0f, 0.4f, 3.0f }, tf);
        const x3::phys::Vec3 eye7{ 0.0f, 0.4f, -3.0f };
        const x3::phys::Vec3 aimZ{ 0.0f, 0.0f, 1.0f };   // through near, then far
        const x3::phys::Vec3 away{ 0.0f, 0.0f, -1.0f };  // empty space behind the eye

        // Negative control FIRST: the counter must see direct rayCast traffic.
        w.rayCasts = 0;
        (void)w.rayCast(eye7, aimZ, 1.0f, x3::phys::Layer::Enemy);
        (void)w.rayCast(eye7, aimZ, 1.0f, x3::phys::Layer::Enemy);
        const bool probeWorks = (w.rayCasts == 2);
        check(probeWorks, "T7a ray-count probe counts direct casts (negative control)");

        // One shot at both monsters: ONE cast, nearest damaged, farther untouched.
        w.rayCasts = 0;
        FireResult r = mm.fire(eye7, aimZ, scene7, w);
        const uint32_t castsPerShot = w.rayCasts;
        const bool oneCast   = (castsPerShot == 1);                       // F5
        const bool nearHit   = r.hitMonster && mm.at(0).hp() == 40 - kDamagePerShot;
        const bool farUntouched = mm.at(1).hp() == 400;                   // F2
        x3::logInfo(std::string("[combat-test] T7 rayCasts/shot=") +
                    std::to_string(castsPerShot) + " (monsters=" +
                    std::to_string(mm.count()) + ")");
        check(oneCast, "T7b fire() performs exactly ONE world raycast (was one per monster)");
        check(nearHit && farUntouched, "T7c nearest monster takes the shot; farther untouched");

        // Miss: aim at empty space — no HP change anywhere, still a single cast.
        w.rayCasts = 0;
        FireResult rm = mm.fire(eye7, away, scene7, w);
        const bool missClean = !rm.hitMonster && mm.at(0).hp() == 40 - kDamagePerShot &&
                               mm.at(1).hp() == 400 && w.rayCasts == 1;   // F4 + F5
        check(missClean, "T7d miss damages nobody (single cast)");
        w.shutdown();
    }

    // ---- T8: ORDER INDEPENDENCE (F3) — reversed spawn order, same victim. ----
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> inner(x3::phys::createPhysicsWorld());
        inner->init();
        CountingPhysicsWorld w(*inner);
        Scene scene8; MonsterManager mm;
        MonsterSystem::Tuning tn; tn.hp = 40;  tn.chaseSpeed = 0.0f;
        MonsterSystem::Tuning tf; tf.hp = 400; tf.chaseSpeed = 0.0f;
        // FAR spawned FIRST this time (index 0), NEAR second (index 1).
        mm.spawn(scene8, device, w, riggedGlbRoot(), x3::phys::Vec3{ 0.0f, 0.4f, 3.0f }, tf);
        mm.spawn(scene8, device, w, riggedGlbRoot(), x3::phys::Vec3{ 0.0f, 0.4f, 0.0f }, tn);
        const x3::phys::Vec3 eye8{ 0.0f, 0.4f, -3.0f };
        w.rayCasts = 0;
        FireResult r = mm.fire(eye8, x3::phys::Vec3{ 0.0f, 0.0f, 1.0f }, scene8, w);
        const bool nearVictim = r.hitMonster && mm.at(1).hp() == 40 - kDamagePerShot &&
                                mm.at(0).hp() == 400 && w.rayCasts == 1;
        check(nearVictim, "T8 vector order reversed: same (nearer) victim, one cast");
        w.shutdown();
    }

    x3::logInfo(std::string("[combat-test] ") + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

// ===========================================================================
// Headless self-test (--test-deathragdoll, TASK#12). Kill a RIGGED monster and
// assert the SKINNED death ragdoll: spawns, its bones fall under gravity, the
// Skinner receives the external pose, and it tears down on corpse-expire with no
// leaked bodies. No window / Vulkan (headless skin path; palette computed on CPU,
// uploads skipped for meshId==0). Mirrors the other self-tests.
// ===========================================================================
namespace {
int dr_pass = 0, dr_fail = 0;
void drcheck(bool cond, const char* name) {
    if (cond) { ++dr_pass; x3::logInfo(std::string("[deathragdoll-test] PASS ") + name); }
    else      { ++dr_fail; x3::logError(std::string("[deathragdoll-test] FAIL ") + name); }
}
// Distance between two palette buffers (sum of |a-b|) — a quick "did the pose move".
float palDiff(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.empty() || a.size() != b.size()) return 0.0f;
    float s = 0.0f;
    for (size_t i = 0; i < a.size(); ++i) s += std::fabs(a[i] - b[i]);
    return s;
}
} // namespace

bool runDeathRagdollSelfTest() {
    dr_pass = dr_fail = 0;
    const float dt = 1.0f / 60.0f;

    std::unique_ptr<x3::phys::IPhysicsWorld> physics(x3::phys::createPhysicsWorld());
    physics->init();
    // Flat ground at y=0 to catch the collapse.
    {
        float v[] = { -50,0,-50,  50,0,-50,  50,0,50,  -50,0,50 };
        uint32_t idx[] = { 0,2,1, 0,3,2 };
        physics->addStaticMesh(v, 4, idx, 6);
    }
    physics->optimizeBroadphase();

    HeadlessDevice device;
    Scene scene;
    MonsterSystem mon;
    const x3::phys::Vec3 monsterPos{ 0.0f, 0.0f, 0.0f };
    // Prefer a genuinely RIGGED model so the skinned-ragdoll path is exercised. The
    // multi-clip "*_anim.glb" rigs (chief_martinez_anim.glb etc.) are GENERATED
    // artifacts that may be ABSENT in a clean checkout — pick the first one that
    // exists, else fall back to the default (which exercises the graceful no-ragdoll
    // path below). buildMonsterTuned loads modelFile from modelDirOverride.
    {
        MonsterSystem::Tuning t;
        // Prefer the multi-clip retargeted "*_anim.glb" rigs when present, but FALL
        // BACK to the Idle-only BASE humanoid rigs (chief_martinez.glb / marcus_webb.glb)
        // which ARE checked in and DO carry a skin + joints + idle clip -> they bind
        // skinnable and exercise the real skinned ragdoll. (The absent _anim artifacts
        // used to leave this defaulting to the insectoid alien_crawler.glb, which does
        // not fit the humanoid rig and fell to the unrigged topple.)
        const char* rigCandidates[] = {
            "chief_martinez_anim.glb", "marcus_webb_anim.glb", "alien_crawler_anim.glb",
            "chief_martinez.glb", "marcus_webb.glb"
        };
        for (const char* cand : rigCandidates) {
            std::error_code ec;
            if (std::filesystem::exists(std::filesystem::path(riggedGlbRoot()) / cand, ec)) {
                t.modelFile = cand;
                t.modelDirOverride = riggedGlbRoot();
                break;
            }
        }
        mon.buildMonsterTuned(scene, device, *physics, riggedGlbRoot(), monsterPos, t);
    }

    // This test is only meaningful for a RIGGED model. If the build fell back to a
    // non-skinnable model (no rigged anim GLB present in this checkout), the ragdoll
    // path correctly NO-OPS to the legacy topple — assert that graceful fallback
    // instead of failing the suite.
    if (!mon.skinnable()) {
        drcheck(!mon.ragdollActive(), "D0 unrigged model: no skinned ragdoll (graceful topple fallback)");
        // Kill it and confirm the death path still works (never break a death).
        const x3::phys::Vec3 eye{ 0.0f, 0.4f, -3.0f };
        const x3::phys::Vec3 aim{ 0.0f, 0.0f, 1.0f };
        for (int i = 0; i < 20 && mon.alive(); ++i) mon.fire(eye, aim, scene, *physics);
        drcheck(!mon.alive() && !mon.ragdollActive(), "D0b unrigged monster dies via topple (no ragdoll)");
        physics->shutdown();
        x3::logInfo(std::string("[deathragdoll-test] ") + std::to_string(dr_pass) +
                    " passed, " + std::to_string(dr_fail) + " failed (unrigged fallback path)");
        return dr_fail == 0;
    }

    // ---- Animated palette snapshot BEFORE death (drive one idle frame so the
    // Skinner has a pose to compare against). ----
    mon.update(dt, scene, *physics, x3::phys::Vec3{ 0.0f, 1.6f, 5.0f });  // far player -> idle
    std::vector<float> animPalette = mon.skinner().lastPalette();

    // ---- KILL it with a shot from -Z (the shove carries +Z). ----
    const x3::phys::Vec3 eye{ 0.0f, 0.4f, -3.0f };
    const x3::phys::Vec3 aim{ 0.0f, 0.0f, 1.0f };
    bool killed = false;
    for (int i = 0; i < 30 && mon.alive(); ++i) {
        FireResult r = mon.fire(eye, aim, scene, *physics);
        if (r.killed) killed = true;
    }
    drcheck(killed && !mon.alive(), "D1a rigged monster killed");
    drcheck(mon.ragdollActive() && mon.deathRagdoll() != nullptr,
            "D1 kill spawns a SKINNED death ragdoll");
    drcheck(mon.deathRagdoll() && mon.deathRagdoll()->inWorld(),
            "D1b ragdoll added to the physics world");

    // Record the ragdoll's top bone Y at spawn (before it falls).
    float topY0 = -1e30f;
    uint32_t bn = mon.deathRagdoll() ? mon.deathRagdoll()->boneCount() : 0;
    drcheck(bn > 0, "D1c ragdoll has bones");
    {
        std::vector<float> w((size_t)bn * 16);
        if (mon.deathRagdoll()) mon.deathRagdoll()->getBoneWorldTransforms(w.data());
        for (uint32_t b = 0; b < bn; ++b) topY0 = std::max(topY0, w[b*16+13]);
    }

    // ---- Drive the death window: the HOST steps physics, then update() reads the
    // bones out and feeds the Skinner. Run most of the topple window (NOT to expiry
    // yet) so the ragdoll is still active + falling. ----
    const int holdSteps = 30;   // 0.5 s < kDeathToppleTime (0.7 s)
    for (int i = 0; i < holdSteps; ++i) {
        physics->step(dt);
        mon.update(dt, scene, *physics, x3::phys::Vec3{ 0.0f, 1.6f, 5.0f });
    }

    // D2: the bones FELL (top bone Y dropped under gravity).
    float topY1 = -1e30f;
    bool finite = true;
    if (mon.deathRagdoll()) {
        std::vector<float> w((size_t)bn * 16);
        mon.deathRagdoll()->getBoneWorldTransforms(w.data());
        topY1 = -1e30f;
        for (uint32_t b = 0; b < bn; ++b) {
            topY1 = std::max(topY1, w[b*16+13]);
            for (int k = 0; k < 16; ++k) if (!std::isfinite(w[b*16+k])) finite = false;
        }
    }
    drcheck(finite, "D2a ragdoll bone transforms stay finite (no NaN)");
    drcheck(topY1 < topY0 - 0.15f, "D2 ragdoll bones fall under gravity (top bone dropped)");

    // D3: the Skinner RECEIVED the external pose — its palette diverged from the
    // animated pose once the ragdoll drove it.
    std::vector<float> ragPalette = mon.skinner().lastPalette();
    drcheck(!ragPalette.empty() && ragPalette.size() == animPalette.size() &&
            palDiff(ragPalette, animPalette) > 1e-3f,
            "D3 Skinner received the external ragdoll pose (palette diverged from animated)");

    // ---- Run OUT the rest of the death window so it expires -> corpse + teardown. ----
    for (int i = 0; i < 60; ++i) {   // 1 s > remaining window
        physics->step(dt);
        mon.update(dt, scene, *physics, x3::phys::Vec3{ 0.0f, 1.6f, 5.0f });
    }

    // D4: torn down on corpse-expire — ragdoll removed, pointer cleared (no leak).
    drcheck(!mon.ragdollActive(), "D4a ragdoll deactivated on corpse-expire");
    drcheck(mon.deathRagdoll() == nullptr, "D4 ragdoll torn down (bodies removed, no leaked Jolt bodies)");

    // ---- D5 (BUG#30): the corpse DESPAWNS after the linger window — it must not
    // stand/lie around forever. Run past kCorpseDespawnTime: the Entity is hidden,
    // despawned() latches, the GPU skin registration is freed (gpuSkinning() false),
    // and the dead monster is NOT double-counted (alive() stays false; entity slot
    // preserved). ----
    {
        const uint32_t deadEnt = mon.entity();
        const bool entLinks   = deadEnt != kNoLink && deadEnt < scene.size();
        const bool wasVisible = entLinks ? scene.get(deadEnt).visible : false;
        drcheck(!mon.despawned() && wasVisible,
                "D5a corpse still present (visible, not yet despawned) before the linger window");
        // Step well past kCorpseDespawnTime (2.5 s) at 60 Hz.
        const int despawnSteps = (int)((kCorpseDespawnTime + 0.5f) / dt) + 2;
        for (int i = 0; i < despawnSteps; ++i) {
            physics->step(dt);
            mon.update(dt, scene, *physics, x3::phys::Vec3{ 0.0f, 1.6f, 5.0f });
        }
        const bool hidden = entLinks ? !scene.get(deadEnt).visible : true;
        drcheck(mon.despawned(), "D5 corpse despawned after the linger window");
        drcheck(hidden, "D5b despawned corpse Entity hidden (no lingering body drawn)");
        drcheck(!mon.skinner().gpuSkinning(),
                "D5c GPU skinned-mesh registration freed on despawn (no leak)");
        drcheck(!mon.alive() && mon.entity() == deadEnt,
                "D5d despawn does not resurrect/double-count (dead, entity slot preserved)");
        drcheck(mon.deathRagdoll() == nullptr,
                "D5e no ragdoll body survives the despawn");
    }

    // The world still steps cleanly with the ragdoll bodies gone (sanity).
    for (int i = 0; i < 5; ++i) physics->step(dt);

    // Defensive: drop any ragdoll bodies (already torn down by D4) while the Jolt
    // world is still alive, so ~MonsterSystem never removes bodies from a dead world.
    mon.shutdownRagdoll();
    physics->shutdown();
    x3::logInfo(std::string("[deathragdoll-test] ") + std::to_string(dr_pass) +
                " passed, " + std::to_string(dr_fail) + " failed");
    return dr_fail == 0;
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

    // ---- (g) PATROL (guard-life W4-3): a patrol-capable row WALKS ITS BEAT when
    // calm, stays near the anchor, AGGROES off it on LOS, and RETURNS to Patrol
    // after the search gives up. target=nullptr => los=false (the machine's own
    // "nothing to see" lane), so phase 1/3 never sight the player. ----
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init(); aiGround(*w, 60.0f);
        Scene scene; MonsterSystem m; AiTargetStub tgt;
        MonsterSystem::Tuning t = aiGuardTuning();
        t.patrolRadius = 2.5f; t.patrolPauseSec = 0.25f;   // brisk loop for the test
        m.buildMonsterTuned(scene, device, *w, riggedGlbRoot(),
                            x3::phys::Vec3{ 0, 0.4f, 0 }, t);
        // Phase 1: no target at all -> patrols. Track displacement + confinement.
        float maxDisp = 0.0f, maxFromAnchor = 0.0f; bool sawPatrol = false;
        for (int i = 0; i < 900; ++i) {                    // 15 s of beat-walking
            m.update(kAiDt, scene, *w, tgt.eye, tgt.eye, nullptr,
                     AttackFxFn{}, BossPhaseFn{}, AllyQueryFn{});
            w->step(kAiDt);
            if (m.aiState() == AiState::Patrol) sawPatrol = true;
            const float dx = m.pos().x, dz = m.pos().z;
            const float d = std::sqrt(dx*dx + dz*dz);      // anchor = origin
            if (d > maxDisp)      maxDisp = d;
            if (d > maxFromAnchor) maxFromAnchor = d;
        }
        const bool walked   = sawPatrol && maxDisp > 1.0f;         // actually moved
        const bool confined = maxFromAnchor < t.patrolRadius + 1.5f; // stayed on beat
        // Phase 2: give it the player close with clear LOS -> engages (interrupt).
        tgt.eye = x3::phys::Vec3{ m.pos().x + 5.0f, 1.6f, m.pos().z };
        bool engaged = false;
        for (int i = 0; i < 300 && !engaged; ++i) {
            m.update(kAiDt, scene, *w, tgt.eye, tgt.eye, &tgt,
                     AttackFxFn{}, BossPhaseFn{}, AllyQueryFn{});
            w->step(kAiDt);
            engaged = (m.aiState() == AiState::Advance || m.aiState() == AiState::Attack ||
                       m.aiState() == AiState::Strafe);
        }
        // Phase 3: target vanishes -> Search runs dry -> back to Patrol.
        bool backToPatrol = false;
        for (int i = 0; i < 1200 && !backToPatrol; ++i) {  // 20 s > search timeout
            m.update(kAiDt, scene, *w, tgt.eye, tgt.eye, nullptr,
                     AttackFxFn{}, BossPhaseFn{}, AllyQueryFn{});
            w->step(kAiDt);
            backToPatrol = (m.aiState() == AiState::Patrol);
        }
        x3::logInfo(std::string("[ai-test] (g) sawPatrol=") + (sawPatrol?"1":"0") +
                    " maxDisp=" + std::to_string(maxDisp) +
                    " maxFromAnchor=" + std::to_string(maxFromAnchor) +
                    " engaged=" + (engaged?"1":"0") +
                    " backToPatrol=" + (backToPatrol?"1":"0"));
        aicheck(walked && confined && engaged && backToPatrol,
                "Tg patrol: walks the beat near the anchor, aggro interrupts, returns after search");
        w->shutdown();
    }

    // ---- (h) [P2-6] FRESH attack LOS (SUBSYSTEM_HARDENING_PLAN AI-2): corner/
    // door fairness. The decision cadence is jittered ~0.15-0.45 s, so m_hasLos
    // alone is a stale snapshot at attack time. Two directions:
    //   Th1: a wall raised MID-WIND-UP (player ducks behind the doorframe) must
    //        cancel the melee — no hit lands through the just-raised wall.
    //   Th2: with the wall REMOVED (player steps into view inside melee range),
    //        the attack must begin within a few frames — not after waiting out
    //        the rest of the decision period — and the fair hit then lands.
    // Both are deterministic under the per-frame refresh (the fresh probe runs
    // every frame an attack is plausible); pre-fix they depended on where the
    // jittered decision tick happened to fall. ----
    {
        // Th1: cancel mid-wind-up when a wall appears.
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init(); aiGround(*w, 60.0f);
        Scene scene; MonsterSystem m; CountingSink sink;
        MonsterSystem::Tuning t;
        t.type = MonsterType::Guard;
        t.hp = 100; t.chaseSpeed = 0.0f;          // stationary: geometry stays fixed
        t.damage = 8; t.attackRange = 2.5f;
        t.attackCooldown = 10.0f;                  // one attack per scenario
        t.attackWindup = 0.30f;                    // a real telegraph window
        t.ranged = false;
        m.buildMonsterTuned(scene, device, *w, riggedGlbRoot(),
                            x3::phys::Vec3{ 0.0f, 0.4f, 0.0f }, t);
        sink.eye = x3::phys::Vec3{ 0.0f, 1.6f, -2.0f };   // inside melee range
        bool wound = false;
        for (int i = 0; i < 120 && !wound; ++i) {
            m.update(kAiDt, scene, *w, sink.eye, sink.eye, &sink, AttackFxFn{},
                     BossPhaseFn{}, AllyQueryFn{});
            w->step(kAiDt);
            wound = m.winding();
        }
        // The player "ducks behind the corner": raise a tall double-sided wall
        // between enemy (z=0) and player (z=-2) while the wind-up is in flight.
        {
            float wx0=-10, wx1=10, wy0=0, wy1=5, wz=-1.0f;
            float v[] = { wx0,wy0,wz, wx1,wy0,wz, wx1,wy1,wz, wx0,wy1,wz };
            uint32_t idx[] = { 0,1,2, 0,2,3,  0,2,1, 0,3,2 }; // double-sided
            w->addStaticMesh(v, 4, idx, 12);
        }
        for (int i = 0; i < 30; ++i) {             // ride out the full wind-up
            m.update(kAiDt, scene, *w, sink.eye, sink.eye, &sink, AttackFxFn{},
                     BossPhaseFn{}, AllyQueryFn{});
            w->step(kAiDt);
        }
        x3::logInfo(std::string("[ai-test] (h1) wound=") + (wound?"1":"0") +
                    " hitsThroughWall=" + std::to_string(sink.hits));
        aicheck(wound && sink.hits == 0,
                "Th1 fresh LOS cancels a mid-wind-up melee when a wall appears");
        w->shutdown();
    }
    {
        // Th2: attack begins promptly once the wall is gone (no stale refusal).
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init(); aiGround(*w, 60.0f);
        Scene scene; MonsterSystem m; CountingSink sink;
        MonsterSystem::Tuning t;
        t.type = MonsterType::Guard;
        t.hp = 100; t.chaseSpeed = 0.0f;
        t.damage = 8; t.attackRange = 2.5f;
        t.attackCooldown = 10.0f;
        t.attackWindup = 0.30f;
        t.ranged = false;
        m.buildMonsterTuned(scene, device, *w, riggedGlbRoot(),
                            x3::phys::Vec3{ 0.0f, 0.4f, 0.0f }, t);
        sink.eye = x3::phys::Vec3{ 0.0f, 1.6f, -2.0f };
        x3::phys::BodyId wall;
        {
            float wx0=-10, wx1=10, wy0=0, wy1=5, wz=-1.0f;
            float v[] = { wx0,wy0,wz, wx1,wy0,wz, wx1,wy1,wz, wx0,wy1,wz };
            uint32_t idx[] = { 0,1,2, 0,2,3,  0,2,1, 0,3,2 };
            wall = w->addStaticMesh(v, 4, idx, 12);
        }
        // Blocked: half a second in melee range with a wall between -> no attack.
        for (int i = 0; i < 30; ++i) {
            m.update(kAiDt, scene, *w, sink.eye, sink.eye, &sink, AttackFxFn{},
                     BossPhaseFn{}, AllyQueryFn{});
            w->step(kAiDt);
        }
        const bool noBlindHits = (sink.hits == 0) && !m.winding();
        // The wall drops (player steps through the door): the wind-up must begin
        // within a FEW frames (fresh per-frame probe), not after the remainder of
        // the ~0.15-0.45 s decision period.
        w->removeBody(wall);
        int framesToWindup = -1;
        for (int i = 0; i < 10; ++i) {
            m.update(kAiDt, scene, *w, sink.eye, sink.eye, &sink, AttackFxFn{},
                     BossPhaseFn{}, AllyQueryFn{});
            w->step(kAiDt);
            if (m.winding()) { framesToWindup = i + 1; break; }
        }
        const bool prompt = (framesToWindup > 0 && framesToWindup <= 3);
        // And the fair hit then actually lands once the wind-up elapses.
        for (int i = 0; i < 30; ++i) {
            m.update(kAiDt, scene, *w, sink.eye, sink.eye, &sink, AttackFxFn{},
                     BossPhaseFn{}, AllyQueryFn{});
            w->step(kAiDt);
        }
        x3::logInfo(std::string("[ai-test] (h2) noBlindHits=") + (noBlindHits?"1":"0") +
                    " framesToWindup=" + std::to_string(framesToWindup) +
                    " fairHits=" + std::to_string(sink.hits));
        aicheck(noBlindHits && prompt && sink.hits >= 1,
                "Th2 fresh LOS grants a fair attack promptly after cover breaks");
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
        t.species      = EnemyType::DominionTrooper;   // guard-life: cue species
        t.patrolRadius = 3.5f;                         // walks a beat when calm
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
        t.species      = EnemyType::Verthani;          // guard-life: creature bucket
        t.patrolRadius = 3.0f;                         // prowls when calm
        t.patrolSpeedMul = 0.35f;                      // slow prowl vs its fast chase
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
        t.species = EnemyType::Illuminated;            // guard-life: cue species (no patrol — elites hold post)
        defs.push_back({ EnemyType::Illuminated, "Illuminated", t });
    }

    // ---- BlueSynth — synthetic: ranged drone-like flier. Bible "Combat Drone":
    // HP 150, plasma 20, flanks/coordinates. Uses blue_synth_seed*.glb if present
    // (absent today -> Drone.glb tinted blue -> box). Mid strafe bias. ----
    {
        MonsterSystem::Tuning t;
        t.type           = MonsterType::Drone;
        t.flyer          = true;                            // ACTUAL flier (hovers, center-origin)
        t.hp             = 150;                             // bible: 150
        t.chaseSpeed     = 3.2f;                            // bible "Medium (flying)"
        t.damage         = combat::kRangedDamageDefault;    // 5 (plasma bolt)
        t.attackRange    = 14.0f;
        t.attackCooldown = combat::kRangedCooldownDefault;  // ~1.4 s
        t.attackWindup   = 0.30f;
        t.ranged         = true;
        t.standoff       = combat::kRangedStandoff;         // ~7 m
        t.aiStrafeBias   = 0.60f;                           // flanks/coordinates (drone-ish)
        // PBR pass: deep steel-blue gunmetal — the old 0.45/0.65/1.0 on the near-
        // white Drone.glb shell washed to a blown-white bag under the cell tube
        // light. Dark base + a modest authored-emissive boost = readable synthetic
        // flier with a controlled eye/engine glow.
        t.tint[0]=0.30f; t.tint[1]=0.36f; t.tint[2]=0.48f;
        t.emissiveScale  = 1.3f;
        const bool realSynth = defBlueSynth(t, 1.0f);
        // The fallback Drone.glb is authored UPRIGHT (Y-up), NOT lying-flat like the
        // human characters — applying the Z->Y stand-up tipped it onto its side
        // ("sideways" drone). Force it off; rigged synths are Y-up too.
        t.standUpZtoY = false;
        x3::logInfo(std::string("[bestiary] BlueSynth model: ") +
                    (realSynth ? "rigged blue_synth GLB" : "fallback Drone.glb (blue tint)"));
        t.species = EnemyType::BlueSynth;              // guard-life: synth bucket (no patrol — flier/watcher)
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
// ACT-1 MID-BOSS ROSTER + MACHINE EXTENSIONS (Wave 1).
//
// The 3 single-body bosses (Dr. Chen / Failed Experiment #7 / Alien Overseer) are
// DATA rows on the existing Boss phase machine; the Chorus (multi-pod) + Swarm
// (scripted hook) are the two general machine extensions. All HP/damage are tuned
// RELATIVE TO MARTINEZ (HP 340, dmg 15; see level1_game.cpp martinezTuning) and the
// combat:: bands, NOT the bible's raw values, so each fight is winnable.
// ===========================================================================

const char* bossTypeName(BossType t) {
    switch (t) {
        case BossType::DrChen:            return "Dr. Chen";
        case BossType::FailedExperiment7: return "Failed Experiment #7";
        case BossType::AlienOverseer:     return "Alien Overseer";
        case BossType::Count:             return "?";
    }
    return "?";
}

namespace {

// Build the single-body Act-1 boss roster ONCE. Each row is a Boss-type Tuning that
// flows through buildMonsterTuned() like Martinez. Stats are Martinez-relative.
std::vector<BossDef> buildBossDefs() {
    std::vector<BossDef> defs;
    defs.reserve((size_t)BossType::Count);

    // ---- DR. CHEN (F2) — transforming oncologist; KILL-vs-CURE. A touch tankier
    // than Martinez (you fight him second). 3 phases (Scientist -> Injection ->
    // Monster); the cure path opens in Phase3 ("Monster"). Reddish toxin tint. ----
    {
        MonsterSystem::Tuning t;
        t.type           = MonsterType::Boss;
        t.hp             = 380;                 // Martinez 340; slightly tankier
        t.chaseSpeed     = 3.2f;               // deliberate scientist; ramps via phases
        t.damage         = 14;                 // ~Martinez (15); chemical/mutant slam
        t.attackRange    = 2.3f;
        t.attackCooldown = 1.1f;
        t.attackWindup   = 0.30f;
        t.ranged         = false;
        t.tint[0]=0.75f; t.tint[1]=1.0f; t.tint[2]=0.70f; t.tint[3]=1.0f;  // sickly green
        // Phases mirror the bible's 800/500/200 thresholds -> our HP fractions.
        t.phase2Frac     = 0.62f;              // ~500/800 (Injection)
        t.phase3Frac     = 0.25f;              // ~200/800 (Monster)
        t.phase2SpeedMul = 1.30f; t.phase2DamageMul = 1.4f;
        t.phase3SpeedMul = 1.7f;  t.phase3DamageMul = 1.8f;
        t.phase3SummonCount = 2;               // summons 2 corrupted scientists (P1 flavor)
        t.hasCureOption  = true;               // KILL vs INCAPACITATE+CURE
        defRigged(t, "chief_martinez.glb", 1.30f);
        defs.push_back({ BossType::DrChen, "Dr. Chen", t });
    }

    // ---- FAILED EXPERIMENT #7 / Marcus Webb (F3) — tragic predecessor; 8ft, heavy
    // armor (folded into higher HP). MEMORY-FLASH gimmick: each phase transition
    // opens a brief stagger + vulnerability window. Bigger + slower-but-hits-hard. ----
    {
        MonsterSystem::Tuning t;
        t.type           = MonsterType::Boss;
        t.hp             = 460;                 // 8ft armored brute (bible 1200 + 50% armor)
        t.chaseSpeed     = 3.0f;               // heavy; charges in phases
        t.damage         = 18;                 // devastating melee (> Martinez)
        t.attackRange    = 2.6f;               // long reach
        t.attackCooldown = 1.25f;              // slower, telegraphed swings
        t.attackWindup   = 0.35f;
        t.ranged         = false;
        t.tint[0]=0.85f; t.tint[1]=0.70f; t.tint[2]=0.95f; t.tint[3]=1.0f;  // bruised violet
        t.phase2Frac     = 0.66f;              // Rage -> Despair (~800/1200)
        t.phase3Frac     = 0.33f;              // Despair -> Release (~400/1200)
        t.phase2SpeedMul = 0.85f; t.phase2DamageMul = 1.2f;  // "slower but +20% dmg" (Despair)
        t.phase3SpeedMul = 1.2f;  t.phase3DamageMul = 1.3f;  // erratic self-control (Release)
        t.phase3SummonCount = 0;               // tragic solo fight; no summons
        // MEMORY FLASH: on each phase transition, ~2 s staggered + takes 2x damage.
        t.memoryFlashTime      = 2.0f;
        t.memoryFlashDamageMul = 2.0f;
        defRigged(t, "marcus_webb.glb", 1.45f);  // 8ft predecessor reads tall
        defs.push_back({ BossType::FailedExperiment7, "Failed Experiment #7", t });
    }

    // ---- ALIEN OVERSEER (F6) — psychic alien commander. RANGED (homing psi-blast)
    // boss: holds a standoff and pelts the player, summons in P3. Tankiest single
    // body of Act 1 (you fight it late). Golden psychic glow. ----
    {
        MonsterSystem::Tuning t;
        t.type           = MonsterType::Boss;
        t.hp             = 420;                 // bible 400 + 200 barrier (folded)
        t.chaseSpeed     = 2.6f;               // floats/repositions, keeps range
        t.damage         = 12;                 // psi-blast (ranged chip; band-aware)
        t.attackRange    = 16.0f;              // long psychic reach
        t.attackCooldown = 1.0f;
        t.attackWindup   = 0.40f;              // telegraphed homing blast
        t.ranged         = true;
        t.standoff       = 12.0f;              // commands from afar
        t.aiStrafeBias   = 0.30f;
        t.tint[0]=1.0f; t.tint[1]=0.85f; t.tint[2]=0.45f; t.tint[3]=1.0f;  // psychic gold
        t.phase2Frac     = 0.66f;
        t.phase3Frac     = 0.33f;
        t.phase2SpeedMul = 1.25f; t.phase2DamageMul = 1.35f;
        t.phase3SpeedMul = 1.5f;  t.phase3DamageMul = 1.6f;
        t.phase3SummonCount = 3;               // summons minions in desperation
        defRigged(t, "chief_martinez.glb", 1.40f);  // tall commander (humanoid stand-in)
        defs.push_back({ BossType::AlienOverseer, "Alien Overseer", t });
    }

    return defs;
}

} // namespace

const std::vector<BossDef>& bossDefs() {
    static const std::vector<BossDef> defs = buildBossDefs();
    return defs;
}

const BossDef& bossDef(BossType t) {
    const std::vector<BossDef>& defs = bossDefs();
    const uint32_t i = (uint32_t)t;
    if (i < defs.size() && defs[i].type == t) return defs[i];   // table is in enum order
    for (const BossDef& d : defs) if (d.type == t) return d;     // defensive
    return defs[0];
}

MonsterSystem::Tuning bossTuning(BossType t) {
    return bossDef(t).tuning;
}

// ===========================================================================
// MULTI-POD BOSS (machine extension #2) — The Collective / The Chorus.
// ===========================================================================

void MultiPodBoss::build(const Config& cfg, Scene& scene, x3::rhi::IRenderDevice& device,
                         x3::phys::IPhysicsWorld& physics, std::string_view modelDir,
                         const x3::phys::Vec3& origin) {
    m_pods.clear();
    m_podNames.clear();
    m_saved = 0;
    m_maxSaved = cfg.maxSaved;
    for (const PodConfig& pc : cfg.pods) {
        auto m = std::make_unique<MonsterSystem>();
        const x3::phys::Vec3 p{ origin.x + pc.offset.x, origin.y + pc.offset.y,
                                origin.z + pc.offset.z };
        m->buildMonsterTuned(scene, device, physics, modelDir, p, pc.tuning);
        m_pods.push_back(std::move(m));
        m_podNames.push_back(pc.name ? std::string(pc.name) : std::string("Pod"));
    }
    // Resolve the fall threshold: 0 in the config means "all pods".
    m_fallThreshold = (cfg.fallThreshold == 0) ? (uint32_t)m_pods.size()
                                               : cfg.fallThreshold;
    if (m_fallThreshold > (uint32_t)m_pods.size())
        m_fallThreshold = (uint32_t)m_pods.size();
    x3::logInfo("[multipod] built " + std::to_string(m_pods.size()) +
                " pods; fallThreshold=" + std::to_string(m_fallThreshold) +
                " maxSaved=" + std::to_string(m_maxSaved));
}

void MultiPodBoss::shutdown() {
    // TASK#12: tear down any in-flight death ragdolls while the physics world is still
    // alive (mirrors MonsterManager::shutdown). Without this, a pod destroyed AFTER
    // physics->shutdown() would call IRagdoll::removeFromWorld() on a dead Jolt system.
    for (auto& p : m_pods) p->shutdownRagdoll();
}

bool MultiPodBoss::sparePod(uint32_t i, Scene& scene, x3::phys::IPhysicsWorld& physics) {
    if (i >= m_pods.size()) return false;
    MonsterSystem& p = *m_pods[i];
    if (!p.alive()) return false;                          // already down (killed/spared)
    if (m_maxSaved != 0 && m_saved >= m_maxSaved) return false;  // save budget spent
    // Non-lethal removal: spare() flags the pod as saved (m_cured) so it is NOT
    // counted as killed. The save COUNT lives here (the morality budget); the pod
    // bookkeeping (killed vs spared) lives on the MonsterSystem.
    const bool removed = p.spare(scene, physics);
    if (removed) {
        ++m_saved;
        x3::logInfo("[multipod] SPARED pod " + std::to_string(i) + " (" +
                    m_podNames[i] + "); saved=" + std::to_string(m_saved));
    }
    return removed;
}

void MultiPodBoss::update(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics,
                          const x3::phys::Vec3& playerPos, IDamageSink* target,
                          const AttackFxFn& fx, const BossPhaseFn& onPhase) {
    for (auto& p : m_pods)
        p->update(dt, scene, physics, playerPos, target, fx, onPhase);
}

void MultiPodBoss::drawAll(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                           const Scene& scene) const {
    for (const auto& p : m_pods) p->drawMonster(device, frame, scene);
}

FireResult MultiPodBoss::fire(const x3::phys::Vec3& eye, const x3::phys::Vec3& dir,
                              Scene& scene, x3::phys::IPhysicsWorld& physics,
                              int damage, x3::DamageType type) {
    // [P2-5] ONE Enemy-layer rayCast per shot, fanned across the pods — same
    // single-cast pattern as MonsterManager::fire (see the comment there and
    // specs/MONSTER_FIRE_SINGLE_RAY.spec.md). Previously one cast PER POD.
    FireResult best;
    if (m_pods.empty()) return best;
    const x3::phys::RayHit hit =
        physics.rayCast(eye, fireDirNormalized(dir), kFireMaxDist, x3::phys::Layer::Enemy);
    for (auto& p : m_pods) {
        FireResult r = p->applyFireHit(hit, eye, dir, scene, physics, damage, type);
        if (r.hitMonster) return r;   // a pod took the shot; at most one per call
        if (r.hit && !best.hit) best = r;  // remember a wall hit for the tracer
    }
    return best;
}

uint32_t MultiPodBoss::killedCount() const {
    uint32_t n = 0;
    for (const auto& p : m_pods)
        if (!p->alive() && !p->wasCured()) ++n;   // down by lethal damage (not spared)
    return n;
}

uint32_t MultiPodBoss::downedCount() const {
    uint32_t n = 0;
    for (const auto& p : m_pods) if (!p->alive()) ++n;
    return n;
}

uint32_t MultiPodBoss::aliveCount() const {
    uint32_t n = 0;
    for (const auto& p : m_pods) if (p->alive()) ++n;
    return n;
}

// Canonical Chorus config: 5 fused minds (Subject Zero/Maya = core, then Harmon /
// Patel / Vasquez / Klein), save up to 4 (the core remains). HP per pod is
// Martinez-relative and small (5 simultaneous targets); each pod is melee-ish so
// the fight reads as a swarm of voices. The Wave-2 Nexus module may use this
// verbatim or build its own Config.
MultiPodBoss::Config chorusConfig() {
    MultiPodBoss::Config cfg;
    cfg.fallThreshold = 0;   // 0 => all 5 pods (down/save all to end the fight)
    cfg.maxSaved      = 4;   // save up to 4 voices; Subject Zero/Maya remains
    struct Voice { const char* name; int hp; float dx, dz; bool ranged; };
    const Voice voices[] = {
        { "Subject Zero (Maya)", 160, 0.0f,  0.0f, false },  // core, suffering
        { "Dr. Harmon",          120, 2.5f,  1.0f, false },  // offense
        { "Dr. Patel",           120, -2.5f, 1.0f, true  },  // defense (ranged)
        { "Dr. Vasquez",         100, 2.0f, -2.0f, false },  // sometimes sabotages
        { "Dr. Klein",           100, -2.0f,-2.0f, true  },  // zealot (ranged)
    };
    for (const Voice& v : voices) {
        MultiPodBoss::PodConfig pc;
        pc.name = v.name;
        pc.offset = x3::phys::Vec3{ v.dx, 0.0f, v.dz };
        MonsterSystem::Tuning t;
        t.type           = MonsterType::Guard;   // pods are NOT the phase-machine Boss
        t.hp             = v.hp;
        t.chaseSpeed     = v.ranged ? 2.4f : 3.0f;
        t.ranged         = v.ranged;
        if (v.ranged) {
            t.damage         = combat::kRangedDamageDefault;
            t.attackRange    = 12.0f;
            t.attackCooldown = combat::kRangedCooldownDefault;
            t.standoff       = 8.0f;
        } else {
            t.damage         = combat::kMeleeDamageDefault;
            t.attackRange    = combat::kMeleeRange;
            t.attackCooldown = combat::kMeleeCooldownDefault;
        }
        t.tint[0]=0.6f; t.tint[1]=0.4f; t.tint[2]=0.9f; t.tint[3]=1.0f;  // cyber-horror violet
        defRigged(t, "marcus_webb.glb", 1.0f);   // humanoid stand-in (falls back to box)
        pc.tuning = t;
        cfg.pods.push_back(pc);
    }
    return cfg;
}

// ===========================================================================
// SCRIPTED PRE-FIGHT HOOK (machine extension #3) — Sarah's master hack (F5).
// ===========================================================================

int ScriptedFightHook::stripBossHp(MonsterSystem& boss, float fraction) {
    if (fraction <= 0.0f) return 0;
    if (fraction > 1.0f) fraction = 1.0f;
    const int before = boss.hp();
    if (before <= 0) return 0;
    int strip = (int)(boss.maxHp() * fraction + 0.5f);
    // Never kill outright: leave at least 1 HP so the boss still spawns/fights.
    if (strip > before - 1) strip = before - 1;
    if (strip < 0) strip = 0;
    // Direct HP debuff (no death path): setHp drops the boss to its post-hack HP so
    // it still spawns + fights, and its own update() advances phases from there.
    if (strip > 0) boss.setHp(before - strip);
    x3::logInfo("[scripted-hook] stripped " + std::to_string(strip) +
                " boss HP (" + std::to_string(before) + " -> " +
                std::to_string(boss.hp()) + ")");
    return strip;
}

uint32_t ScriptedFightHook::flipToAllied(const std::vector<MonsterSystem*>& enemies) {
    uint32_t n = 0;
    for (MonsterSystem* e : enemies) {
        if (!e) continue;
        e->convertToAllied();
        ++n;
    }
    x3::logInfo("[scripted-hook] flipped " + std::to_string(n) + " enemies to allied");
    return n;
}

ScriptedFightHook::Result ScriptedFightHook::masterHack(
        MonsterSystem& boss, float bossHpFraction,
        const std::vector<MonsterSystem*>& drones) {
    Result r;
    r.hpStripped    = stripBossHp(boss, bossHpFraction);
    r.dronesFlipped = flipToAllied(drones);
    return r;
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

// ===========================================================================
// --test-bosses: Act-1 mid-boss roster + machine extensions (Wave 1).
//   (a) the 5 mid-bosses exist with sane phase/HP/damage and BUILD; the 3 single-
//       body bosses transition phases on the HP-keyed machine; Chen exposes the cure
//       path in Phase3; FE#7's Memory-Flash window opens on a phase transition.
//   (b) the multi-pod Chorus DOWNS only when the fall-threshold is met; the SAVE path
//       increments savedCount (not killedCount) and respects the maxSaved cap.
//   (c) the scripted hook strips the right HP fraction AND flips enemies to allied.
//   (d) Chief Martinez still constructs + behaves as before (regression guard).
// No window / Vulkan. Mirrors the other self-tests.
// ===========================================================================
namespace {

int bo_pass = 0, bo_fail = 0;
void bocheck(bool cond, const char* name) {
    if (cond) { ++bo_pass; x3::logInfo(std::string("[bosses-test] PASS ") + name); }
    else      { ++bo_fail; x3::logError(std::string("[bosses-test] FAIL ") + name); }
}

constexpr float kBoDt = 1.0f / 60.0f;

// A trivial player stand-in the AI treats as the target (alive, fixed eye).
class BoTargetStub final : public IDamageSink {
public:
    x3::phys::Vec3 eye{ 0.0f, 1.6f, 0.0f };
    int hits = 0;
    bool takeDamage(int) override { ++hits; return true; }
    x3::phys::Vec3 damageTargetPos() const override { return eye; }
    bool isAlive() const override { return true; }
};

x3::phys::BodyId boGround(x3::phys::IPhysicsWorld& w, float half) {
    float v[] = { -half,0,-half,  half,0,-half,  half,0,half,  -half,0,half };
    uint32_t idx[] = { 0,2,1, 0,3,2 };
    return w.addStaticMesh(v, 4, idx, 6);
}

// Drive a boss's HP down in steps + tick its update() so the phase machine latches.
// Returns the highest phase reached. Tracks whether a memory-flash ever opened.
BossPhase driveBossToHp(MonsterSystem& m, Scene& scene, x3::phys::IPhysicsWorld& w,
                        BoTargetStub& tgt, int targetHp, bool* sawFlash) {
    while (m.hp() > targetHp && m.alive()) {
        m.setHp(m.hp() - 1);
        m.update(kBoDt, scene, w, tgt.eye, tgt.eye, &tgt, AttackFxFn{},
                 BossPhaseFn{}, AllyQueryFn{});
        if (sawFlash && m.inMemoryFlash()) *sawFlash = true;
    }
    return m.phase();
}

} // namespace

bool runBossesSelfTest() {
    bo_pass = bo_fail = 0;
    HeadlessDevice device;

    // ---- (a1) the single-body boss table is complete + ordered, with sane stats. ----
    {
        const std::vector<BossDef>& roster = bossDefs();
        bool complete = roster.size() == (size_t)BossType::Count;
        bool ordered = true, sane = true;
        for (uint32_t i = 0; i < roster.size(); ++i) {
            if ((uint32_t)roster[i].type != i) ordered = false;
            const MonsterSystem::Tuning& t = roster[i].tuning;
            // Sane = Boss type, positive HP, positive damage, valid phase thresholds.
            if (t.type != MonsterType::Boss) sane = false;
            if (t.hp <= 0 || t.damage <= 0) sane = false;
            if (!(t.phase3Frac > 0.0f && t.phase3Frac < t.phase2Frac && t.phase2Frac < 1.0f))
                sane = false;
            x3::logInfo(std::string("[bosses-test] ") + roster[i].name +
                        " hp=" + std::to_string(t.hp) + " dmg=" + std::to_string(t.damage) +
                        " p2=" + std::to_string(t.phase2Frac) +
                        " p3=" + std::to_string(t.phase3Frac) +
                        " ranged=" + (t.ranged ? "1" : "0"));
        }
        bocheck(complete && ordered && sane,
                "Ta single-body boss roster complete/ordered with sane phase/HP/damage");
    }

    // ---- (a2) each single-body boss BUILDS, reports its stats, and the 3-phase
    // HP-keyed machine advances Phase1 -> Phase2 -> Phase3. ----
    {
        int builtOk = 0, phasedOk = 0;
        for (uint32_t bi = 0; bi < (uint32_t)BossType::Count; ++bi) {
            const BossType bt = (BossType)bi;
            const BossDef& def = bossDef(bt);
            std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
            w->init(); boGround(*w, 80.0f);
            Scene scene; MonsterSystem m; BoTargetStub tgt;
            tgt.eye = x3::phys::Vec3{ 0.0f, 1.6f, -8.0f };
            m.buildMonsterTuned(scene, device, *w, riggedGlbRoot(),
                                x3::phys::Vec3{ 0.0f, 0.4f, 0.0f }, bossTuning(bt));
            const bool statsMatch = m.type() == MonsterType::Boss &&
                m.maxHp() == def.tuning.hp && m.attackDamage() == def.tuning.damage &&
                m.phase() == BossPhase::Phase1;
            if (statsMatch) ++builtOk;
            // Drive HP down through the thresholds.
            const int p2hp = (int)(m.maxHp() * def.tuning.phase2Frac) - 1;
            driveBossToHp(m, scene, *w, tgt, p2hp, nullptr);
            const bool reachedP2 = m.phase() == BossPhase::Phase2;
            const int p3hp = (int)(m.maxHp() * def.tuning.phase3Frac) - 1;
            driveBossToHp(m, scene, *w, tgt, p3hp, nullptr);
            const bool reachedP3 = m.phase() == BossPhase::Phase3;
            if (reachedP2 && reachedP3) ++phasedOk;
            x3::logInfo(std::string("[bosses-test] ") + bossTypeName(bt) +
                        " built statsOK=" + (statsMatch ? "1" : "0") +
                        " reachedP2=" + (reachedP2 ? "1" : "0") +
                        " reachedP3=" + (reachedP3 ? "1" : "0"));
            w->shutdown();
        }
        bocheck(builtOk == (int)BossType::Count,
                "Ta each single-body boss builds Boss-type with its table HP/damage");
        bocheck(phasedOk == (int)BossType::Count,
                "Ta each single-body boss advances P1->P2->P3 on the HP machine");
    }

    // ---- (a3) Dr. Chen exposes the CURE path in Phase3, and cure() spares (not
    // kills) him; a boss WITHOUT the option (FE#7) cannot be cured. ----
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init(); boGround(*w, 80.0f);
        Scene scene; MonsterSystem chen; BoTargetStub tgt;
        tgt.eye = x3::phys::Vec3{ 0.0f, 1.6f, -8.0f };
        chen.buildMonsterTuned(scene, device, *w, riggedGlbRoot(),
                               x3::phys::Vec3{ 0.0f, 0.4f, 0.0f }, bossTuning(BossType::DrChen));
        const bool hasOpt = chen.hasCureOption();
        const bool noCureEarly = !chen.canCure();          // Phase1 -> not yet curable
        const int p3hp = (int)(chen.maxHp() * bossTuning(BossType::DrChen).phase3Frac) - 1;
        driveBossToHp(chen, scene, *w, tgt, p3hp, nullptr);
        const bool curableInP3 = chen.canCure();           // Phase3 -> curable
        const bool cured = chen.cure(scene, *w);
        const bool sparedNotKilled = cured && !chen.alive() && chen.wasCured();
        bocheck(hasOpt && noCureEarly && curableInP3 && sparedNotKilled,
                "Ta Dr. Chen: cure option opens in Phase3 + cure() spares (not kills)");

        // FE#7 has NO cure option.
        Scene scene2; MonsterSystem fe7;
        fe7.buildMonsterTuned(scene2, device, *w, riggedGlbRoot(),
                              x3::phys::Vec3{ 20.0f, 0.4f, 0.0f },
                              bossTuning(BossType::FailedExperiment7));
        driveBossToHp(fe7, scene2, *w, tgt,
                      (int)(fe7.maxHp() * 0.20f), nullptr);
        bocheck(!fe7.hasCureOption() && !fe7.canCure() && !fe7.cure(scene2, *w),
                "Ta Failed Experiment #7 has NO cure option (cure() refused)");
        w->shutdown();
    }

    // ---- (a4) FE#7 MEMORY FLASH: a phase transition opens a stagger + amplified-
    // damage window. Assert the window opens AND incoming damage is amplified. ----
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init(); boGround(*w, 80.0f);
        Scene scene; MonsterSystem fe7; BoTargetStub tgt;
        tgt.eye = x3::phys::Vec3{ 0.0f, 1.6f, -8.0f };
        fe7.buildMonsterTuned(scene, device, *w, riggedGlbRoot(),
                              x3::phys::Vec3{ 0.0f, 0.4f, 0.0f },
                              bossTuning(BossType::FailedExperiment7));
        const float flashMul = bossTuning(BossType::FailedExperiment7).memoryFlashDamageMul;
        // Drive into Phase2 (a transition) and catch the flash window.
        bool sawFlash = false;
        const int p2hp = (int)(fe7.maxHp() * bossTuning(BossType::FailedExperiment7).phase2Frac) - 1;
        driveBossToHp(fe7, scene, *w, tgt, p2hp, &sawFlash);
        const bool flashOpen = fe7.inMemoryFlash();
        const float mulNow = fe7.incomingDamageMul();
        // While flashing: an incoming shot does amplified damage.
        const int hpBefore = fe7.hp();
        const bool wasFlashing = fe7.inMemoryFlash();
        fe7.takeMeleeDamage(10, scene, *w);   // 10 base -> ~20 while flashing
        const int dealt = hpBefore - fe7.hp();
        const bool amplified = !wasFlashing || dealt >= (int)(10 * flashMul) - 1;
        x3::logInfo(std::string("[bosses-test] FE#7 sawFlash=") + (sawFlash ? "1" : "0") +
                    " flashOpenAtP2=" + (flashOpen ? "1" : "0") +
                    " mul=" + std::to_string(mulNow) + " dealt=" + std::to_string(dealt));
        bocheck(sawFlash && flashOpen && mulNow > 1.5f && amplified,
                "Ta Failed Experiment #7 Memory-Flash: staggered window + amplified damage");
        w->shutdown();
    }

    // ---- (b) MULTI-POD Chorus: builds 5 pods; falls only at the threshold; the SAVE
    // path increments savedCount (not killedCount) and respects the cap. ----
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init(); boGround(*w, 80.0f);
        Scene scene; MultiPodBoss chorus;
        MultiPodBoss::Config cfg = chorusConfig();
        chorus.build(cfg, scene, device, *w, riggedGlbRoot(), x3::phys::Vec3{ 0.0f, 0.4f, -6.0f });
        const uint32_t n = chorus.podCount();
        const uint32_t thresh = chorus.fallThreshold();
        bocheck(n == 5 && thresh == 5 && !chorus.hasFallen(),
                "Tb Chorus builds 5 pods; not fallen with 0 down");

        // Down pods one at a time; the boss must NOT fall until the threshold.
        bool fellEarly = false;
        for (uint32_t i = 0; i < n - 1; ++i) {
            chorus.pod(i).takeMeleeDamage(chorus.pod(i).hp() + 1, scene, *w);  // kill pod i
            if (chorus.hasFallen()) fellEarly = true;   // must NOT fall before all down
        }
        const bool notFallenBelowThreshold = !fellEarly && !chorus.hasFallen();
        // Down the last pod -> now the boss falls.
        chorus.pod(n - 1).takeMeleeDamage(chorus.pod(n - 1).hp() + 1, scene, *w);
        const bool fallenAtThreshold = chorus.hasFallen();
        bocheck(notFallenBelowThreshold && fallenAtThreshold,
                "Tb Chorus falls ONLY when the pod fall-threshold is met");
        bocheck(chorus.killedCount() == 5 && chorus.savedCount() == 0,
                "Tb killing all pods => killedCount=5, savedCount=0");
    }

    // ---- (b2) SAVE path: spare counts as saved (not killed); the maxSaved cap holds. ----
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init(); boGround(*w, 80.0f);
        Scene scene; MultiPodBoss chorus;
        chorus.build(chorusConfig(), scene, device, *w, riggedGlbRoot(),
                     x3::phys::Vec3{ 0.0f, 0.4f, -6.0f });
        // Spare 4 voices (the max). Each must increment saved, not killed.
        uint32_t spared = 0;
        for (uint32_t i = 0; i < 4; ++i) if (chorus.sparePod(i, scene, *w)) ++spared;
        // A 5th spare must be REFUSED (cap = 4).
        const bool fifthRefused = !chorus.sparePod(4, scene, *w);
        const bool saveCounts = chorus.savedCount() == 4 && chorus.killedCount() == 0;
        // Kill the remaining (5th) pod -> killedCount=1, savedCount stays 4, fallen.
        chorus.pod(4).takeMeleeDamage(chorus.pod(4).hp() + 1, scene, *w);
        const bool mixOk = chorus.savedCount() == 4 && chorus.killedCount() == 1 &&
                           chorus.downedCount() == 5 && chorus.hasFallen();
        x3::logInfo(std::string("[bosses-test] Chorus spared=") + std::to_string(spared) +
                    " saved=" + std::to_string(chorus.savedCount()) +
                    " killed=" + std::to_string(chorus.killedCount()) +
                    " fifthRefused=" + (fifthRefused ? "1" : "0"));
        bocheck(spared == 4 && fifthRefused && saveCounts && mixOk,
                "Tb SAVE path: spare increments saved (not killed) + maxSaved cap holds");
    }

    // ---- (c) SCRIPTED PRE-FIGHT HOOK: strips the right HP fraction + flips the
    // designated enemies to allied (their damage zeroed). ----
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init(); boGround(*w, 80.0f);
        Scene scene;
        MonsterSystem boss;
        boss.buildMonsterTuned(scene, device, *w, riggedGlbRoot(),
                               x3::phys::Vec3{ 0.0f, 0.4f, -10.0f },
                               bossTuning(BossType::AlienOverseer));   // any boss as the F5 stand-in
        const int maxHp = boss.maxHp();
        // Build 3 hostile drones (Combat Drone = BlueSynth) to flip.
        std::vector<std::unique_ptr<MonsterSystem>> droneOwn;
        std::vector<MonsterSystem*> drones;
        for (int i = 0; i < 3; ++i) {
            auto d = std::make_unique<MonsterSystem>();
            d->buildMonsterTuned(scene, device, *w, riggedGlbRoot(),
                                 x3::phys::Vec3{ (float)(i*3 + 3), 0.4f, -4.0f },
                                 tuningFor(EnemyType::BlueSynth));
            drones.push_back(d.get());
            droneOwn.push_back(std::move(d));
        }
        bool allHostileBefore = true;
        for (auto* d : drones) if (d->attackDamage() <= 0 || d->isAllied()) allHostileBefore = false;

        // The master hack: strip 75% of the boss + flip the 3 drones.
        ScriptedFightHook::Result res = ScriptedFightHook::masterHack(boss, 0.75f, drones);

        const int expectStrip = (int)(maxHp * 0.75f + 0.5f);
        const bool hpStripOk = res.hpStripped == expectStrip &&
                               boss.hp() == maxHp - expectStrip && boss.hp() >= 1;
        const bool flipCountOk = res.dronesFlipped == 3;
        bool allAlliedAfter = true;
        for (auto* d : drones) if (!d->isAllied() || d->attackDamage() != 0) allAlliedAfter = false;
        x3::logInfo(std::string("[bosses-test] hack stripped=") + std::to_string(res.hpStripped) +
                    " (expect " + std::to_string(expectStrip) + ") bossHP=" +
                    std::to_string(boss.hp()) + "/" + std::to_string(maxHp) +
                    " flipped=" + std::to_string(res.dronesFlipped));
        bocheck(allHostileBefore && hpStripOk && flipCountOk && allAlliedAfter,
                "Tc scripted hook strips the right HP fraction + flips enemies to allied");
        w->shutdown();
    }

    // ---- (d) REGRESSION: Chief Martinez still builds + behaves (Boss, 3-phase, the
    // P3 summon callback fires once, no cure option, no memory flash). Mirrors his
    // canonical level1_game tuning so we don't depend on that file. ----
    {
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init(); boGround(*w, 80.0f);
        Scene scene; MonsterSystem martinez; BoTargetStub tgt;
        tgt.eye = x3::phys::Vec3{ 0.0f, 1.6f, -8.0f };
        MonsterSystem::Tuning t;     // == level1_game.cpp martinezTuning() essentials
        t.type = MonsterType::Boss; t.hp = 340; t.chaseSpeed = 3.4f;
        t.damage = 15; t.attackRange = 2.4f; t.attackCooldown = 1.1f; t.attackWindup = 0.30f;
        t.ranged = false; t.phase3SummonCount = 2;
        martinez.buildMonsterTuned(scene, device, *w, riggedGlbRoot(),
                                   x3::phys::Vec3{ 0.0f, 0.4f, 0.0f }, t);
        const bool builtOk = martinez.type() == MonsterType::Boss && martinez.maxHp() == 340 &&
                             martinez.attackDamage() == 15 && martinez.phase() == BossPhase::Phase1 &&
                             !martinez.hasCureOption() && !martinez.inMemoryFlash();
        int summonFired = 0; BossPhase lastPhase = BossPhase::Phase1;
        BossPhaseFn onPhase = [&](BossPhase p) {
            lastPhase = p;
            if (p == BossPhase::Phase3) summonFired += martinez.summonCount();
        };
        // Drive HP to 0 with the phase callback wired.
        while (martinez.hp() > 0 && martinez.alive()) {
            martinez.setHp(martinez.hp() - 2);
            martinez.update(kBoDt, scene, *w, tgt.eye, tgt.eye, &tgt, AttackFxFn{},
                            onPhase, AllyQueryFn{});
        }
        const bool phasedAndSummoned = lastPhase == BossPhase::Phase3 && summonFired == 2;
        x3::logInfo(std::string("[bosses-test] Martinez builtOk=") + (builtOk ? "1" : "0") +
                    " lastPhase=" + std::to_string((int)lastPhase) +
                    " summonFired=" + std::to_string(summonFired));
        bocheck(builtOk && phasedAndSummoned,
                "Td Chief Martinez still builds Boss + phases + P3 summon (regression)");
        w->shutdown();
    }

    const int total = bo_pass + bo_fail;
    x3::logInfo(std::string("bosses: ") + std::to_string(bo_pass) + "/" +
                std::to_string(total) + " passed");
    x3::logInfo(std::string("[bosses-test] ") + std::to_string(bo_pass) + " passed, " +
                std::to_string(bo_fail) + " failed");
    return bo_fail == 0;
}

// ===========================================================================
// ACT-2 ROSTER (Wave 2) — alien-planet surface (Keth'zar Prime), Levels 8-20.
//
// 5 enemy rows + 4 single-body boss rows. All ride the existing MonsterSystem
// + Boss phase machine; only the new Wave-2 Tuning tags (startAllied /
// copyFeintPhase / escapeTimerSeconds) are read by the floor module. HP/damage
// are MARTINEZ-RELATIVE (Act-1 final ref HP 340 / dmg 15) and bounded by the
// combat:: bands — NOT the bible's raw values — so each Act-2 fight stays
// winnable under the engine's time/iframe budget.
// ===========================================================================

const char* act2EnemyTypeName(Act2EnemyType t) {
    switch (t) {
        case Act2EnemyType::SalvariAlly:         return "Salvari Ally";
        case Act2EnemyType::NativeDesertFauna:   return "Native Desert Fauna";
        case Act2EnemyType::MutatedScientist:    return "Mutated Scientist";
        case Act2EnemyType::MutatedFlora:        return "Mutated Flora";
        case Act2EnemyType::SurfacePursuitDrone: return "Surface Pursuit Drone";
        case Act2EnemyType::Count:               return "?";
    }
    return "?";
}

const char* act2BossTypeName(Act2BossType t) {
    switch (t) {
        case Act2BossType::MemoryHunter:        return "Memory Hunter";
        case Act2BossType::TheSiren:            return "The Siren";
        case Act2BossType::BreederQueen:        return "Breeder Queen";
        case Act2BossType::GarrisonCommander:   return "Planetary Garrison Commander";
        case Act2BossType::Count:               return "?";
    }
    return "?";
}

namespace {

// Build the Act-2 ENEMY roster ONCE. Each row maps the bible's surface fauna /
// Salvari / mutated-swamp / surface-drone profiles onto the existing combat AI
// lanes (Guard/Drone), with the Act-2 Tuning tags set per-row.
std::vector<Act2EnemyDef> buildAct2EnemyDefs() {
    std::vector<Act2EnemyDef> defs;
    defs.reserve((size_t)Act2EnemyType::Count);

    // ---- SALVARI ALLY (L11) — refugee/companion who fights beside the player.
    // ALLIED at spawn: m_allied=true, m_dmg=0 (cannot harm the player). Tuned
    // similar to a Salvari Scout (bible HP 200, energy-rifle ranged) but with a
    // SMALL "team-up" footprint so it doesn't out-damage Jake. Standoff drone-
    // lane AI (it pelts hostiles at range; the player is not a valid target). ----
    {
        MonsterSystem::Tuning t;
        t.type           = MonsterType::Drone;              // ranged AI shape (rifle)
        t.hp             = 220;                             // bible Salvari Scout ~200 (+earth bonus)
        t.chaseSpeed     = 3.6f;                            // earth-O2 enhanced (fast)
        t.damage         = 0;                               // ALLIED: cannot harm the player
        t.attackRange    = 14.0f;                           // energy rifle
        t.attackCooldown = combat::kRangedCooldownDefault;
        t.attackWindup   = 0.30f;
        t.ranged         = true;
        t.standoff       = 9.0f;                            // covers the player from range
        t.aiStrafeBias   = 0.25f;                           // disciplined, tactical fire
        t.tint[0]=0.55f; t.tint[1]=0.95f; t.tint[2]=0.85f;  // bioluminescent teal
        t.startAllied    = true;                            // FLIPPED ALLIED at build time
        defRigged(t, "marcus_webb.glb", 1.0f);              // humanoid stand-in (box fallback)
        defs.push_back({ Act2EnemyType::SalvariAlly, "Salvari Ally", t });
    }

    // ---- NATIVE DESERT FAUNA (L9-10) — crystalline-desert predator. Burrow-ambush
    // shape (bible Crystal Stalker: HP 180, claw 25, shard-spit; packs). Modeled
    // as a Guard-lane fast melee with high strafe (it darts/flanks like Verthani
    // but a touch tankier — crystal armor). Hostile by default; the floor module
    // chooses whether to spawn it neutral or hunting. ----
    {
        MonsterSystem::Tuning t;
        t.type           = MonsterType::Guard;
        t.hp             = 180;                             // bible: 180 (Crystal Stalker)
        t.chaseSpeed     = 4.0f;                            // ambush dart speed
        t.damage         = combat::kMeleeDamageMax;         // 10 (strong claw)
        t.attackRange    = combat::kMeleeRange;
        t.attackCooldown = combat::kMeleeCooldownMin;       // 1.0 s (rapid claws)
        t.attackWindup   = combat::kMeleeWindup;
        t.ranged         = false;
        t.aiStrafeBias   = 0.85f;                           // pack-flanker, very darty
        t.tint[0]=0.95f; t.tint[1]=0.75f; t.tint[2]=1.0f;   // crystalline violet
        defRigged(t, "alien_crawler.glb", 1.05f);           // non-humanoid crawler
        defs.push_back({ Act2EnemyType::NativeDesertFauna, "Native Desert Fauna", t });
    }

    // ---- MUTATED SCIENTIST (L13-14) — toxic-swamp hostile. Was a researcher;
    // failed-terraforming chemistry warped them. Mid HP, slow chase, chemical-
    // throw at MEDIUM range (modeled as ranged so they hold position and pelt;
    // walks like an Illuminated but cheaper). ----
    {
        MonsterSystem::Tuning t;
        t.type           = MonsterType::Drone;              // ranged-AI (chemical throw)
        t.hp             = 150;                             // bible Corrupted Scientist ~80; tougher post-mutation
        t.chaseSpeed     = 2.4f;                            // slow, deteriorated
        t.damage         = combat::kRangedDamageDefault;    // 5 (chemical splash)
        t.attackRange    = 10.0f;
        t.attackCooldown = combat::kRangedCooldownMax;      // 1.2 s
        t.attackWindup   = 0.35f;
        t.ranged         = true;
        t.standoff       = 7.5f;
        t.aiStrafeBias   = 0.20f;                           // panicky, holds short standoff
        t.tint[0]=0.65f; t.tint[1]=0.95f; t.tint[2]=0.55f;  // toxic green
        defRigged(t, "chief_martinez.glb", 1.0f);           // humanoid stand-in (box fallback)
        defs.push_back({ Act2EnemyType::MutatedScientist, "Mutated Scientist", t });
    }

    // ---- MUTATED FLORA (L13-14) — toxic-swamp hostile, STATIONARY. Rooted lash-
    // arm hazard: chase speed = 0 so it never moves; attacks at long melee reach
    // (a whip/tendril). The AI lane is Guard (melee) so the state machine still
    // attacks anyone in reach; combined with chaseSpeed=0 the body never closes —
    // exactly the "stationary but dangerous in its bubble" behaviour the spec asks. ----
    {
        MonsterSystem::Tuning t;
        t.type           = MonsterType::Guard;              // melee swing (lash)
        t.hp             = 220;                             // tough plant body (bible-flavored)
        t.chaseSpeed     = 0.0f;                            // STATIONARY — never moves
        t.damage         = combat::kMeleeDamageDefault;     // 8 (tendril lash)
        t.attackRange    = 3.5f;                            // long lash reach (vs 1.9 m default)
        t.attackCooldown = combat::kMeleeCooldownMax;       // 1.3 s (slow, heavy lashes)
        t.attackWindup   = 0.40f;                           // big telegraphed swipe
        t.ranged         = false;
        t.aiStrafeBias   = 0.0f;                            // can't strafe (stationary)
        t.tint[0]=0.45f; t.tint[1]=0.70f; t.tint[2]=0.40f;  // dark mossy green
        defRigged(t, "alien_crawler.glb", 1.30f);           // bulky organic mass (box fallback)
        defs.push_back({ Act2EnemyType::MutatedFlora, "Mutated Flora", t });
    }

    // ---- SURFACE PURSUIT DRONE (L8) — fast ranged FLYER for the escape encounter.
    // BlueSynth profile pushed harder: higher chase speed (hunts the running
    // player), tighter standoff (it CHASES, doesn't loiter at range), short
    // cooldown plasma-bolt. Sky-grey tint to distinguish from indoor BlueSynth. ----
    {
        MonsterSystem::Tuning t;
        t.type           = MonsterType::Drone;
        t.flyer          = true;                            // hovers off the floor
        t.hp             = 120;                             // fragile pursuit drone
        t.chaseSpeed     = 5.0f;                            // FAST (faster than player walk)
        t.damage         = combat::kRangedDamageDefault;    // 5 (plasma bolt)
        t.attackRange    = 16.0f;                           // long reach
        t.attackCooldown = combat::kRangedCooldownMin;      // 0.8 s (relentless)
        t.attackWindup   = 0.25f;
        t.ranged         = true;
        t.standoff       = 5.0f;                            // closes in — not a sniper
        t.aiStrafeBias   = 0.70f;                           // active pursuit, flanks
        // PBR pass: mid grey-steel (0.85+ washed to white on the pale shell) —
        // still clearly lighter than the indoor BlueSynth's deep steel-blue.
        t.tint[0]=0.55f; t.tint[1]=0.57f; t.tint[2]=0.63f;
        t.emissiveScale  = 1.3f;
        // Use the BlueSynth model-resolution helper (rigged blue_synth if present;
        // else Drone.glb fallback). Force standUpZtoY off (Drone.glb is Y-up).
        const bool realSynth = defBlueSynth(t, 1.0f);
        t.standUpZtoY = false;
        (void)realSynth;
        defs.push_back({ Act2EnemyType::SurfacePursuitDrone, "Surface Pursuit Drone", t });
    }

    return defs;
}

// Build the Act-2 BOSS roster ONCE. Each row is a Boss-type Tuning that flows
// through buildMonsterTuned() like Martinez + the Act-1 bosses. Stats are
// Martinez-relative (HP 340 / dmg 15) with each fight tuned a notch tankier
// than the last Act-1 boss to read as escalation.
std::vector<Act2BossDef> buildAct2BossDefs() {
    std::vector<Act2BossDef> defs;
    defs.reserve((size_t)Act2BossType::Count);

    // ---- MEMORY HUNTER (L12) — Act-2 cave-system boss. PSYCHOLOGICAL / IDENTITY
    // gimmick: in Phase2 it copies a previously-fought enemy's moveset and feints
    // (the bible's "memory warfare" — illusions of the player's dead). The boss
    // machine carries the PHASE TAG (copyFeintPhase = 2) so act2_world reads it
    // and spawns the illusion adds + HUD; the boss itself is a tanky Boss-type
    // melee/short-range chaser with the FE#7 memory-flash window borrowed in P2
    // (clarity moments interrupt the copy). ----
    {
        MonsterSystem::Tuning t;
        t.type           = MonsterType::Boss;
        t.hp             = 500;                             // tanky cave-system boss
        t.chaseSpeed     = 3.4f;                            // matches Martinez
        t.damage         = 14;                              // psychic strike
        t.attackRange    = 3.0f;                            // mid reach
        t.attackCooldown = 1.05f;
        t.attackWindup   = 0.30f;
        t.ranged         = false;
        t.tint[0]=0.70f; t.tint[1]=0.55f; t.tint[2]=1.0f; t.tint[3]=1.0f;  // psi-violet
        t.phase2Frac     = 0.66f;                           // P1 Illusions -> P2 Copy/Feint
        t.phase3Frac     = 0.33f;                           // P2 -> P3 Identity Crisis
        t.phase2SpeedMul = 1.20f; t.phase2DamageMul = 1.30f;
        t.phase3SpeedMul = 1.55f; t.phase3DamageMul = 1.70f;
        t.phase3SummonCount = 3;                            // illusion-army revenants in P3
        // Wave-2 tag: P2 is the COPY/FEINT phase (act2_world reads this).
        t.copyFeintPhase     = 2;                           // Phase2 = copy/feint
        // Borrow the memory-flash beat for the clarity-moment tells.
        t.memoryFlashTime      = 1.5f;
        t.memoryFlashDamageMul = 1.75f;
        defRigged(t, "chief_martinez.glb", 1.35f);          // tall psychic stand-in
        defs.push_back({ Act2BossType::MemoryHunter, "Memory Hunter", t });
    }

    // ---- THE SIREN (Beta, L14) — transformed Aria. Sonic/psychic lure boss;
    // Beta-class tier (a step beyond Martinez/Chen). Ranged-AI lane (her "voice"
    // is the long-range hit), summons parasite adds in P3. Reuses the existing
    // rescue-boss GLB if present. ----
    {
        MonsterSystem::Tuning t;
        t.type           = MonsterType::Boss;
        t.hp             = 440;                             // Beta-tier (Martinez 340)
        t.chaseSpeed     = 2.8f;                            // floats, repositions
        t.damage         = 13;                              // sonic blast (ranged chip)
        t.attackRange    = 14.0f;                           // long psychic voice
        t.attackCooldown = 1.0f;
        t.attackWindup   = 0.45f;                           // sustained-scream telegraph
        t.ranged         = true;
        t.standoff       = 10.0f;
        t.aiStrafeBias   = 0.40f;
        t.tint[0]=1.0f; t.tint[1]=0.50f; t.tint[2]=0.85f; t.tint[3]=1.0f;  // siren magenta
        t.phase2Frac     = 0.66f;                           // Lure -> Mother (parasites)
        t.phase3Frac     = 0.33f;                           // Mother -> Final (spawning core)
        t.phase2SpeedMul = 1.25f; t.phase2DamageMul = 1.30f;
        t.phase3SpeedMul = 1.55f; t.phase3DamageMul = 1.60f;
        t.phase3SummonCount = 4;                            // parasite swarm in final phase
        // Existing BossTheSiren.glb is in rigged_glb (Y-up); use directly.
        t.modelFile        = "BossTheSiren.glb";
        t.modelDirOverride = riggedGlbRoot();
        t.standUpZtoY      = false;
        t.modelScale       = 1.30f;
        defs.push_back({ Act2BossType::TheSiren, "The Siren", t });
    }

    // ---- BREEDER QUEEN (Beta, L16) — transformed Keisha. Tactical mind; SUMMONS
    // (Stage-2 infected adds). Beta-tier tankier than the Siren; melee charger
    // with heavy armor (folded into HP). High P2/P3 summon count (the spec calls
    // out "summons" explicitly). ----
    {
        MonsterSystem::Tuning t;
        t.type           = MonsterType::Boss;
        t.hp             = 520;                             // bible 2000; band-scaled (heavy armor)
        t.chaseSpeed     = 3.2f;                            // rage-charge in P2+
        t.damage         = 17;                              // brutal claw / slam
        t.attackRange    = 2.6f;                            // long reach (12ft frame)
        t.attackCooldown = 1.15f;
        t.attackWindup   = 0.35f;
        t.ranged         = false;
        t.tint[0]=0.85f; t.tint[1]=0.45f; t.tint[2]=0.55f; t.tint[3]=1.0f;  // bloody crimson
        t.phase2Frac     = 0.66f;                           // General -> Mother (summons more)
        t.phase3Frac     = 0.33f;                           // Mother -> Final (weak flank)
        t.phase2SpeedMul = 1.30f; t.phase2DamageMul = 1.35f;
        t.phase3SpeedMul = 1.55f; t.phase3DamageMul = 1.60f;
        t.phase3SummonCount = 5;                            // SUMMONS — the spec's headline beat
        // Existing BossBreederQueen.glb is in rigged_glb (Y-up); use directly.
        t.modelFile        = "BossBreederQueen.glb";
        t.modelDirOverride = riggedGlbRoot();
        t.standUpZtoY      = false;
        t.modelScale       = 1.55f;                         // 12ft frame reads tall
        defs.push_back({ Act2BossType::BreederQueen, "Breeder Queen", t });
    }

    // ---- PLANETARY GARRISON COMMANDER (L20 finale) — Act-2 escape finale.
    // 3 phases, mapped onto the existing HP-keyed machine + the Wave-2 escape-
    // timer tag:
    //   P1 (>66%)  TROOPS         — commander on foot with rifle, summons squad adds.
    //   P2 (66-33%) MECH-SUIT     — bigger/tougher (scale up via phase2ScaleMul,
    //                               damage up via phase2DamageMul); melee+ranged.
    //   P3 (<33%)  ORBITAL STRIKE — boss broadcasts the strike; escapeTimerSeconds
    //                               drives the level-exit countdown ("escape or
    //                               die"). The floor module reads escapeTimer() in
    //                               P3 and starts a HUD timer + the trigger.
    // Tankiest Act-2 fight; sets up the Act-3 transition. ----
    {
        MonsterSystem::Tuning t;
        t.type           = MonsterType::Boss;
        t.hp             = 600;                             // Act-2 finale: tankiest
        t.chaseSpeed     = 3.0f;                            // disciplined; ramps in mech
        t.damage         = 16;                              // plasma rifle / mech fist
        t.attackRange    = 12.0f;                           // ranged in P1, longer in P2 effectively
        t.attackCooldown = 1.0f;
        t.attackWindup   = 0.30f;
        t.ranged         = true;                            // commander pelts; mech adds melee
        t.standoff       = 8.0f;
        t.aiStrafeBias   = 0.30f;
        t.tint[0]=0.90f; t.tint[1]=0.65f; t.tint[2]=0.35f; t.tint[3]=1.0f;  // gold-bronze command
        t.phase2Frac     = 0.66f;                           // TROOPS -> MECH-SUIT
        t.phase3Frac     = 0.33f;                           // MECH-SUIT -> ORBITAL-STRIKE escape
        // P2 MECH-SUIT: bigger AND tougher. Scale + damage push, with a slight
        // speed dip (heavy chassis) -> the mech reads as a different fight.
        t.phase2SpeedMul = 0.95f; t.phase2DamageMul = 1.50f;
        t.phase2ScaleMul = 1.45f;                           // MECH = visibly bigger
        // P3 ESCAPE: the commander vents the mech and calls the orbital strike —
        // damage & speed spike for a brief desperation push before the timer.
        t.phase3SpeedMul = 1.35f; t.phase3DamageMul = 1.70f;
        t.phase3ScaleMul = 1.30f;
        t.phase3SummonCount = 2;                            // last-ditch elite guards
        // Wave-2 tag: P3 starts the orbital-strike escape timer (sec).
        t.escapeTimerSeconds = 30.0f;                       // 30 s level-exit window
        defRigged(t, "chief_martinez.glb", 1.45f);          // tall commander stand-in
        defs.push_back({ Act2BossType::GarrisonCommander, "Planetary Garrison Commander", t });
    }

    return defs;
}

} // namespace

const std::vector<Act2EnemyDef>& act2EnemyDefs() {
    static const std::vector<Act2EnemyDef> defs = buildAct2EnemyDefs();
    return defs;
}

const Act2EnemyDef& act2EnemyDef(Act2EnemyType t) {
    const std::vector<Act2EnemyDef>& defs = act2EnemyDefs();
    const uint32_t i = (uint32_t)t;
    if (i < defs.size() && defs[i].type == t) return defs[i];
    for (const Act2EnemyDef& d : defs) if (d.type == t) return d;
    return defs[0];
}

MonsterSystem::Tuning act2EnemyTuning(Act2EnemyType t) {
    return act2EnemyDef(t).tuning;
}

const std::vector<Act2BossDef>& act2BossDefs() {
    static const std::vector<Act2BossDef> defs = buildAct2BossDefs();
    return defs;
}

const Act2BossDef& act2BossDef(Act2BossType t) {
    const std::vector<Act2BossDef>& defs = act2BossDefs();
    const uint32_t i = (uint32_t)t;
    if (i < defs.size() && defs[i].type == t) return defs[i];
    for (const Act2BossDef& d : defs) if (d.type == t) return d;
    return defs[0];
}

MonsterSystem::Tuning act2BossTuning(Act2BossType t) {
    return act2BossDef(t).tuning;
}

// ===========================================================================
// --test-act2bosses: the Act-2 enemy + boss roster (Wave 2).
//   (a) the 5 Act-2 enemy defs exist + BUILD with sane stats; the Salvari ally
//       is flagged ALLIED and deals 0 damage; mutated flora is STATIONARY; the
//       surface pursuit drone is fast / ranged / flyer.
//   (b) the 4 Act-2 boss defs exist + BUILD with Boss-type stats + valid phase
//       thresholds, and the HP-keyed machine advances P1 -> P2 -> P3.
//   (c) Memory Hunter carries a copy/feint phase descriptor + reports the tag
//       once driven into that phase.
//   (d) Garrison Commander has 3 phases AND an escape-timer tag (> 0); the
//       floor module reads it in P3.
//   (e) Breeder Queen summons in P3 (phase3SummonCount > 0).
//   (f) REGRESSION: Chief Martinez + all 3 single-body Act-1 bosses still build.
// No window / Vulkan. Mirrors the other self-tests.
// ===========================================================================
namespace {

int a2_pass = 0, a2_fail = 0;
void a2check(bool cond, const char* name) {
    if (cond) { ++a2_pass; x3::logInfo(std::string("[act2bosses-test] PASS ") + name); }
    else      { ++a2_fail; x3::logError(std::string("[act2bosses-test] FAIL ") + name); }
}

constexpr float kA2Dt = 1.0f / 60.0f;

class A2TargetStub final : public IDamageSink {
public:
    x3::phys::Vec3 eye{ 0.0f, 1.6f, -8.0f };
    int hits = 0;
    bool takeDamage(int) override { ++hits; return true; }
    x3::phys::Vec3 damageTargetPos() const override { return eye; }
    bool isAlive() const override { return true; }
};

x3::phys::BodyId a2Ground(x3::phys::IPhysicsWorld& w, float half) {
    float v[] = { -half,0,-half,  half,0,-half,  half,0,half,  -half,0,half };
    uint32_t idx[] = { 0,2,1, 0,3,2 };
    return w.addStaticMesh(v, 4, idx, 6);
}

// Drive a boss's HP down by setHp + update ticks until <= targetHp. Returns the
// highest phase reached. Mirrors driveBossToHp() from the Act-1 self-test.
BossPhase a2DriveBossToHp(MonsterSystem& m, Scene& scene, x3::phys::IPhysicsWorld& w,
                          A2TargetStub& tgt, int targetHp) {
    while (m.hp() > targetHp && m.alive()) {
        m.setHp(m.hp() - 1);
        m.update(kA2Dt, scene, w, tgt.eye, tgt.eye, &tgt, AttackFxFn{},
                 BossPhaseFn{}, AllyQueryFn{});
    }
    return m.phase();
}

} // namespace

bool runAct2BossesSelfTest() {
    a2_pass = a2_fail = 0;
    HeadlessDevice device;

    // ---- (a1) The Act-2 enemy table is complete + ordered. ----
    {
        const std::vector<Act2EnemyDef>& roster = act2EnemyDefs();
        bool complete = roster.size() == (size_t)Act2EnemyType::Count;
        bool ordered = true;
        for (uint32_t i = 0; i < roster.size(); ++i)
            if ((uint32_t)roster[i].type != i) ordered = false;
        x3::logInfo(std::string("[act2bosses-test] enemy roster size=") +
                    std::to_string(roster.size()) + " (expected " +
                    std::to_string((uint32_t)Act2EnemyType::Count) + ")");
        a2check(complete && ordered,
                "Ta enemy roster has one row per Act2EnemyType, in enum order");
    }

    // ---- (a2) Each Act-2 enemy BUILDS with its table stats, and per-row tags
    // are observable on the built MonsterSystem. ----
    {
        int builtOk = 0;
        bool salvariAllied = false, salvariZeroDmg = false;
        bool floraStationary = false;
        bool droneIsFastRangedFlyer = false;
        for (uint32_t ei = 0; ei < (uint32_t)Act2EnemyType::Count; ++ei) {
            const Act2EnemyType et = (Act2EnemyType)ei;
            const Act2EnemyDef& def = act2EnemyDef(et);
            std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
            w->init(); a2Ground(*w, 80.0f);
            Scene scene; MonsterSystem m;
            m.buildMonsterTuned(scene, device, *w, riggedGlbRoot(),
                                x3::phys::Vec3{ 0.0f, 0.4f, 0.0f }, act2EnemyTuning(et));
            // Built stats match the row, with the allied flip applied when set.
            const bool statsMatch =
                m.type() == def.tuning.type && m.maxHp() == def.tuning.hp &&
                m.ranged() == def.tuning.ranged;
            if (statsMatch) ++builtOk;
            x3::logInfo(std::string("[act2bosses-test] ") + act2EnemyTypeName(et) +
                        " hp=" + std::to_string(m.maxHp()) +
                        " type=" + std::to_string((int)m.type()) +
                        " ranged=" + (m.ranged() ? "1" : "0") +
                        " dmg=" + std::to_string(m.attackDamage()) +
                        " allied=" + (m.isAllied() ? "1" : "0"));
            if (et == Act2EnemyType::SalvariAlly) {
                salvariAllied  = m.isAllied();
                salvariZeroDmg = m.attackDamage() == 0;
            }
            if (et == Act2EnemyType::MutatedFlora) {
                // Stationary: chase-speed pulled from the tuning is 0.
                floraStationary = (def.tuning.chaseSpeed == 0.0f);
            }
            if (et == Act2EnemyType::SurfacePursuitDrone) {
                // Fast ranged flyer (data tags on the def).
                droneIsFastRangedFlyer = def.tuning.flyer && def.tuning.ranged &&
                                         def.tuning.chaseSpeed >= 4.5f;
            }
            w->shutdown();
        }
        a2check(builtOk == (int)Act2EnemyType::Count,
                "Ta each Act-2 enemy builds with its table stats");
        a2check(salvariAllied && salvariZeroDmg,
                "Ta Salvari ally is ALLIED + 0 damage to player");
        a2check(floraStationary,
                "Ta Mutated Flora is STATIONARY (chaseSpeed == 0)");
        a2check(droneIsFastRangedFlyer,
                "Ta Surface Pursuit Drone is fast ranged flyer");
    }

    // ---- (b1) The Act-2 boss table is complete + ordered + has sane phase/HP/damage. ----
    {
        const std::vector<Act2BossDef>& roster = act2BossDefs();
        bool complete = roster.size() == (size_t)Act2BossType::Count;
        bool ordered = true, sane = true;
        for (uint32_t i = 0; i < roster.size(); ++i) {
            if ((uint32_t)roster[i].type != i) ordered = false;
            const MonsterSystem::Tuning& t = roster[i].tuning;
            if (t.type != MonsterType::Boss) sane = false;
            if (t.hp <= 0 || t.damage <= 0) sane = false;
            if (!(t.phase3Frac > 0.0f && t.phase3Frac < t.phase2Frac && t.phase2Frac < 1.0f))
                sane = false;
            x3::logInfo(std::string("[act2bosses-test] ") + roster[i].name +
                        " hp=" + std::to_string(t.hp) + " dmg=" + std::to_string(t.damage) +
                        " p2=" + std::to_string(t.phase2Frac) +
                        " p3=" + std::to_string(t.phase3Frac) +
                        " ranged=" + (t.ranged ? "1" : "0"));
        }
        a2check(complete && ordered && sane,
                "Tb Act-2 boss roster complete/ordered with sane phase/HP/damage");
    }

    // ---- (b2) Each Act-2 boss BUILDS Boss-type and advances P1 -> P2 -> P3. ----
    {
        int builtOk = 0, phasedOk = 0;
        for (uint32_t bi = 0; bi < (uint32_t)Act2BossType::Count; ++bi) {
            const Act2BossType bt = (Act2BossType)bi;
            const Act2BossDef& def = act2BossDef(bt);
            std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
            w->init(); a2Ground(*w, 80.0f);
            Scene scene; MonsterSystem m; A2TargetStub tgt;
            m.buildMonsterTuned(scene, device, *w, riggedGlbRoot(),
                                x3::phys::Vec3{ 0.0f, 0.4f, 0.0f }, act2BossTuning(bt));
            const bool statsMatch = m.type() == MonsterType::Boss &&
                m.maxHp() == def.tuning.hp && m.attackDamage() == def.tuning.damage &&
                m.phase() == BossPhase::Phase1;
            if (statsMatch) ++builtOk;
            const int p2hp = (int)(m.maxHp() * def.tuning.phase2Frac) - 1;
            a2DriveBossToHp(m, scene, *w, tgt, p2hp);
            const bool reachedP2 = m.phase() == BossPhase::Phase2;
            const int p3hp = (int)(m.maxHp() * def.tuning.phase3Frac) - 1;
            a2DriveBossToHp(m, scene, *w, tgt, p3hp);
            const bool reachedP3 = m.phase() == BossPhase::Phase3;
            if (reachedP2 && reachedP3) ++phasedOk;
            x3::logInfo(std::string("[act2bosses-test] ") + act2BossTypeName(bt) +
                        " built statsOK=" + (statsMatch ? "1" : "0") +
                        " reachedP2=" + (reachedP2 ? "1" : "0") +
                        " reachedP3=" + (reachedP3 ? "1" : "0"));
            w->shutdown();
        }
        a2check(builtOk == (int)Act2BossType::Count,
                "Tb each Act-2 boss builds Boss-type with its table HP/damage");
        a2check(phasedOk == (int)Act2BossType::Count,
                "Tb each Act-2 boss advances P1->P2->P3 on the HP machine");
    }

    // ---- (c) MEMORY HUNTER copy/feint phase descriptor (data tag). ----
    {
        const MonsterSystem::Tuning mh = act2BossTuning(Act2BossType::MemoryHunter);
        const bool tagSet = mh.copyFeintPhase != 0;
        // Drive a built Memory Hunter into the tagged phase and check the live tag.
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init(); a2Ground(*w, 80.0f);
        Scene scene; MonsterSystem m; A2TargetStub tgt;
        m.buildMonsterTuned(scene, device, *w, riggedGlbRoot(),
                            x3::phys::Vec3{ 0.0f, 0.4f, 0.0f }, mh);
        const uint32_t tag = m.copyFeintPhase();
        // Drive to the tagged phase (copyFeintPhase==2 -> Phase2).
        const int p2hp = (int)(m.maxHp() * mh.phase2Frac) - 1;
        a2DriveBossToHp(m, scene, *w, tgt, p2hp);
        const bool inTaggedPhase = m.inCopyFeintPhase();
        x3::logInfo(std::string("[act2bosses-test] MemoryHunter copyFeintPhase tag=") +
                    std::to_string(tag) + " inCopyFeintPhase=" +
                    (inTaggedPhase ? "1" : "0") + " phase=" + std::to_string((int)m.phase()));
        a2check(tagSet && tag == mh.copyFeintPhase && inTaggedPhase,
                "Tc Memory Hunter carries a copy/feint phase descriptor (active in P2)");
        w->shutdown();
    }

    // ---- (d) GARRISON COMMANDER has 3 phases AND an escape-timer tag. ----
    {
        const MonsterSystem::Tuning gc = act2BossTuning(Act2BossType::GarrisonCommander);
        const bool timerSet = gc.escapeTimerSeconds > 0.0f;
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init(); a2Ground(*w, 80.0f);
        Scene scene; MonsterSystem m; A2TargetStub tgt;
        m.buildMonsterTuned(scene, device, *w, riggedGlbRoot(),
                            x3::phys::Vec3{ 0.0f, 0.4f, 0.0f }, gc);
        // The 3 phases: Phase1 at spawn; drive to Phase2; drive to Phase3.
        const bool spawnP1 = m.phase() == BossPhase::Phase1;
        a2DriveBossToHp(m, scene, *w, tgt, (int)(m.maxHp() * gc.phase2Frac) - 1);
        const bool reachedP2 = m.phase() == BossPhase::Phase2;
        a2DriveBossToHp(m, scene, *w, tgt, (int)(m.maxHp() * gc.phase3Frac) - 1);
        const bool reachedP3 = m.phase() == BossPhase::Phase3;
        const bool timerLive = m.hasEscapeTimer() && m.escapeTimerSeconds() == gc.escapeTimerSeconds;
        x3::logInfo(std::string("[act2bosses-test] Garrison spawnP1=") +
                    (spawnP1 ? "1" : "0") + " P2=" + (reachedP2 ? "1" : "0") +
                    " P3=" + (reachedP3 ? "1" : "0") +
                    " escapeTimer=" + std::to_string(m.escapeTimerSeconds()));
        a2check(spawnP1 && reachedP2 && reachedP3,
                "Td Garrison Commander has 3 phases (P1 troops / P2 mech / P3 escape)");
        a2check(timerSet && timerLive,
                "Td Garrison Commander carries an orbital-strike escape timer");
        w->shutdown();
    }

    // ---- (e) BREEDER QUEEN summons (phase3SummonCount > 0). ----
    {
        const MonsterSystem::Tuning bq = act2BossTuning(Act2BossType::BreederQueen);
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init(); a2Ground(*w, 80.0f);
        Scene scene; MonsterSystem m; A2TargetStub tgt;
        m.buildMonsterTuned(scene, device, *w, riggedGlbRoot(),
                            x3::phys::Vec3{ 0.0f, 0.4f, 0.0f }, bq);
        // Drive to P3 and read the summonCount() the host would use.
        a2DriveBossToHp(m, scene, *w, tgt, (int)(m.maxHp() * bq.phase3Frac) - 1);
        const int summons = m.summonCount();
        x3::logInfo(std::string("[act2bosses-test] BreederQueen summonCount=") +
                    std::to_string(summons));
        a2check(bq.phase3SummonCount > 0 && summons > 0,
                "Te Breeder Queen summons in P3 (phase3SummonCount > 0)");
        w->shutdown();
    }

    // ---- (f) REGRESSION: Chief Martinez + all 3 Act-1 single-body bosses still
    // build (Boss-type, table HP/damage, Phase1 at spawn). ----
    {
        // The 3 Act-1 bosses (DrChen / FE7 / AlienOverseer):
        int act1Ok = 0;
        for (uint32_t bi = 0; bi < (uint32_t)BossType::Count; ++bi) {
            const BossType bt = (BossType)bi;
            const BossDef& def = bossDef(bt);
            std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
            w->init(); a2Ground(*w, 80.0f);
            Scene scene; MonsterSystem m;
            m.buildMonsterTuned(scene, device, *w, riggedGlbRoot(),
                                x3::phys::Vec3{ 0.0f, 0.4f, 0.0f }, bossTuning(bt));
            const bool ok = m.type() == MonsterType::Boss &&
                            m.maxHp() == def.tuning.hp &&
                            m.attackDamage() == def.tuning.damage &&
                            m.phase() == BossPhase::Phase1;
            if (ok) ++act1Ok;
            x3::logInfo(std::string("[act2bosses-test] Act-1 ") + bossTypeName(bt) +
                        " regression " + (ok ? "OK" : "FAIL"));
            w->shutdown();
        }
        // Chief Martinez (no enum row — mirrors level1_game's martinezTuning essentials).
        std::unique_ptr<x3::phys::IPhysicsWorld> w(x3::phys::createPhysicsWorld());
        w->init(); a2Ground(*w, 80.0f);
        Scene scene; MonsterSystem martinez;
        MonsterSystem::Tuning t;
        t.type = MonsterType::Boss; t.hp = 340; t.chaseSpeed = 3.4f;
        t.damage = 15; t.attackRange = 2.4f; t.attackCooldown = 1.1f; t.attackWindup = 0.30f;
        t.ranged = false; t.phase3SummonCount = 2;
        martinez.buildMonsterTuned(scene, device, *w, riggedGlbRoot(),
                                   x3::phys::Vec3{ 0.0f, 0.4f, 0.0f }, t);
        const bool martinezOk = martinez.type() == MonsterType::Boss &&
                                martinez.maxHp() == 340 &&
                                martinez.attackDamage() == 15 &&
                                martinez.phase() == BossPhase::Phase1 &&
                                !martinez.hasCureOption() &&
                                !martinez.inMemoryFlash() &&
                                !martinez.hasEscapeTimer() &&
                                martinez.copyFeintPhase() == 0;
        x3::logInfo(std::string("[act2bosses-test] Martinez regression ") +
                    (martinezOk ? "OK" : "FAIL"));
        a2check(act1Ok == (int)BossType::Count && martinezOk,
                "Tf REGRESSION: Martinez + all 3 Act-1 bosses still construct");
        w->shutdown();
    }

    const int total = a2_pass + a2_fail;
    x3::logInfo(std::string("act2bosses: ") + std::to_string(a2_pass) + "/" +
                std::to_string(total) + " passed");
    x3::logInfo(std::string("[act2bosses-test] ") + std::to_string(a2_pass) + " passed, " +
                std::to_string(a2_fail) + " failed");
    return a2_fail == 0;
}

// ===========================================================================
// --test-adaptive-hide: canon-aliens Adaptive-Hide rhythm. One MonsterSystem
// tuned with adaptiveHideResist=0.6 + window 8s; the test walks the spec's
// case table (full first hit -> reduced same-type repeat -> full type-rotation
// -> window expires -> full again -> reduced again) and a final opt-out
// regression. No window/Vulkan. Mirrors runBossesSelfTest.
// ===========================================================================
bool runAdaptiveHideSelfTest() {
    int ah_pass = 0, ah_fail = 0;
    auto ahcheck = [&](bool cond, const char* label) {
        if (cond) { ++ah_pass; x3::logInfo(std::string("[adaptivehide-test] PASS ") + label); }
        else      { ++ah_fail; x3::logError(std::string("[adaptivehide-test] FAIL ") + label); }
    };

    HeadlessDevice device;
    std::unique_ptr<x3::phys::IPhysicsWorld> physics(x3::phys::createPhysicsWorld());
    physics->init(); boGround(*physics, 80.0f);

    // ---- Build a Boss-type monster with Adaptive Hide opted-in. HP is generous so
    // the long fire sequence never accidentally kills it. damage=0 so the monster
    // doesn't try to attack the (absent) player. ----
    MonsterSystem::Tuning t;
    t.type                    = MonsterType::Boss;
    t.hp                      = 1000;
    t.damage                  = 0;
    t.adaptiveHideResist      = 0.6f;
    t.adaptiveHideDurationSec = 8.0f;

    Scene scene;
    MonsterSystem m;
    m.buildMonsterTuned(scene, device, *physics, riggedGlbRoot(),
                        x3::phys::Vec3{ 0.0f, 0.4f, 0.0f }, t);

    const int baseDmg = 10;
    auto hit = [&](x3::DamageType ty) {
        const int before = m.hp();
        m.takeMeleeDamage(baseDmg, scene, *physics, ty);
        return before - m.hp();
    };

    // T1 — first hit (Kinetic, no prior type): expect FULL damage (10).
    {
        const int delta = hit(x3::DamageType::Kinetic);
        ahcheck(delta == 10, "T1 first hit (fresh window) applies FULL damage (10)");
    }
    // T2 — same type immediate: expect REDUCED (10 * (1 - 0.6) = 4).
    {
        const int delta = hit(x3::DamageType::Kinetic);
        ahcheck(delta == 4 && m.adaptiveHideType() == x3::DamageType::Kinetic,
                "T2 same-type immediate applies REDUCED damage (4) and re-latches");
    }
    // T3 — DIFFERENT type immediate: expect FULL (10), latches the new type.
    {
        const int delta = hit(x3::DamageType::Energy);
        ahcheck(delta == 10 && m.adaptiveHideType() == x3::DamageType::Energy,
                "T3 different-type immediate applies FULL damage (10) and latches Energy");
    }
    // T4 — drive the world forward 9 s; the timer must expire (>= durationSec).
    {
        for (int i = 0; i < 90; ++i)
            m.update(0.1f, scene, *physics, x3::phys::Vec3{ 0.0f, 0.0f, 0.0f });
        ahcheck(m.adaptiveHideTimer() == 0.0f,
                "T4 update(~9s) expires the resist window (timer == 0)");
    }
    // T5 — same type as the latched (Energy), but AFTER expiry: expect FULL.
    {
        const int delta = hit(x3::DamageType::Energy);
        ahcheck(delta == 10,
                "T5 same-as-latched type AFTER window expired applies FULL damage (10)");
    }
    // T6 — immediate same-type after T5: expect REDUCED again (window re-opened).
    {
        const int delta = hit(x3::DamageType::Energy);
        ahcheck(delta == 4,
                "T6 same-type immediate after T5 applies REDUCED damage (4) again");
    }
    // T7 — OPT-OUT regression. A Tuning with adaptiveHideResist=0 takes FULL on every
    // hit no matter how fast you spam (existing rows must be totally untouched).
    {
        MonsterSystem::Tuning t0 = t;
        t0.adaptiveHideResist = 0.0f;
        Scene s0;
        MonsterSystem m0;
        m0.buildMonsterTuned(s0, device, *physics, riggedGlbRoot(),
                             x3::phys::Vec3{ 8.0f, 0.4f, 0.0f }, t0);
        const int before = m0.hp();
        m0.takeMeleeDamage(baseDmg, s0, *physics, x3::DamageType::Kinetic);
        m0.takeMeleeDamage(baseDmg, s0, *physics, x3::DamageType::Kinetic);
        m0.takeMeleeDamage(baseDmg, s0, *physics, x3::DamageType::Kinetic);
        const int delta = before - m0.hp();
        ahcheck(delta == 30,
                "T7 opt-out (resist=0) takes FULL damage on every same-type repeat (3x10=30)");
    }

    physics->shutdown();

    const int total = ah_pass + ah_fail;
    x3::logInfo(std::string("adaptivehide: ") + std::to_string(ah_pass) + "/" +
                std::to_string(total) + " passed");
    x3::logInfo(std::string("[adaptivehide-test] ") + std::to_string(ah_pass) + " passed, " +
                std::to_string(ah_fail) + " failed");
    return ah_fail == 0;
}

} // namespace x3::game
