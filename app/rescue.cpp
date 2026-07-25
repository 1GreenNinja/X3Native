// F2 RESCUE SYSTEM (EFLZ Spire spec §5). See app/rescue.h.
//
// Clean-room: built from Scene/MonsterManager + the engine interfaces
// (IRenderDevice / IPhysicsWorld / IModelLoader / IAssetSource) only. No
// purchased C# / id Tech / RBDOOM source consulted. Mirrors monster.cpp.
#include "rescue.h"
#include "mesh_prims.h"
#include "headless_device.h"
#include "asset_root.h"

#include "engine/core/x3_log.h"

#include <cmath>
#include <cstdio>    // std::snprintf (R8 root-Y negative-control GLB builder)
#include <cstring>   // std::memcpy (ragdoll bone bind-pose copies)
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace x3::game {

namespace {

// Victim collision box half-extents (a standing humanoid ~1.8 m tall).
constexpr x3::phys::Vec3 kVictimHalf{ 0.4f, 0.9f, 0.4f };

// Default live-character scale (rigged humanoid GLBs read ~1.8 m at scale 1).
constexpr float kVictimScale = 1.0f;
constexpr float kBoxScale    = 1.0f;

// Build a column-major 4x4 from a 3x3 basis (columns bx,by,bz), uniform scale s,
// and translation t. Identical to monster.cpp's composeTRS.
void composeTRS(float m[16],
                const x3::phys::Vec3& bx, const x3::phys::Vec3& by, const x3::phys::Vec3& bz,
                float s, const x3::phys::Vec3& t) {
    m[0]  = bx.x * s; m[1]  = bx.y * s; m[2]  = bx.z * s; m[3]  = 0.0f;
    m[4]  = by.x * s; m[5]  = by.y * s; m[6]  = by.z * s; m[7]  = 0.0f;
    m[8]  = bz.x * s; m[9]  = bz.y * s; m[10] = bz.z * s; m[11] = 0.0f;
    m[12] = t.x;      m[13] = t.y;      m[14] = t.z;      m[15] = 1.0f;
}

// Facing law (CONVENTIONS.md / monster.cpp headingToFace): to point a model's
// local -Z along planar (dirX,dirZ), yaw = atan2(-dirX,-dirZ). Companion faces the
// player (dir = player - self).
float headingToFace(float dirX, float dirZ) {
    if (dirX * dirX + dirZ * dirZ < 1e-12f) return 0.0f;
    return std::atan2(-dirX, -dirZ);
}

// Inverse of a column-major affine 4x4 that is rotation * UNIFORM scale + translation
// (the gameplay draw matrix: composeTRS with a yaw basis + uniform scale). Identical
// to monster.cpp's invertAffineUniform — used by the collapse ragdoll to map bone
// world transforms into skin space. Returns false (out=identity) on a degenerate
// (near-zero scale) matrix.
bool invertAffineUniform(const float m[16], float out[16]) {
    for (int i = 0; i < 16; ++i) out[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    const float s2 = m[0]*m[0] + m[1]*m[1] + m[2]*m[2];
    if (s2 < 1e-12f) return false;
    const float invS2 = 1.0f / s2;   // invA = R^T / s = (col^T) / s^2
    out[0] = m[0]*invS2; out[1] = m[4]*invS2; out[2] = m[8]*invS2;
    out[4] = m[1]*invS2; out[5] = m[5]*invS2; out[6] = m[9]*invS2;
    out[8] = m[2]*invS2; out[9] = m[6]*invS2; out[10]= m[10]*invS2;
    out[3] = out[7] = out[11] = 0.0f;
    const float tx = m[12], ty = m[13], tz = m[14];
    out[12] = -(out[0]*tx + out[4]*ty + out[8]*tz);
    out[13] = -(out[1]*tx + out[5]*ty + out[9]*tz);
    out[14] = -(out[2]*tx + out[6]*ty + out[10]*tz);
    out[15] = 1.0f;
    return true;
}

} // namespace

// ===========================================================================
// RescueVictim
// ===========================================================================
void RescueVictim::build(Scene& scene, x3::rhi::IRenderDevice& device,
                         x3::phys::IPhysicsWorld& physics,
                         std::string_view modelDir, const x3::phys::Vec3& pos,
                         VictimId id, std::string_view name, std::string_view liveModel,
                         float timer, const MonsterSystem::Tuning& bossTuning) {
    m_id        = id;
    m_name      = std::string(name);
    m_pos       = pos;
    m_state     = VictimState::Captive;
    m_timerMax  = timer;
    m_timeLeft  = timer;
    m_bossTuning= bossTuning;
    // A friendly cyan-ish tint so a captive/companion reads distinct from enemies.
    m_tint[0] = 0.7f; m_tint[1] = 0.9f; m_tint[2] = 1.0f; m_tint[3] = 1.0f;

    // ---- Load the live character GLB via a mounted loose-dir asset source (same
    // path as MonsterSystem::buildMonsterTuned). The rigged humanoids are Y-up, so
    // no Z->Y stand-up. On any failure a procedural box stands in. ----
    const std::string file = std::string(liveModel);
    m_device = &device;   // cached so tick() can re-upload CPU-skinned vertices
    m_assets.reset(x3::asset::createAssetSource());
    bool mounted = m_assets->mountDir(std::string(modelDir), 0);
    if (mounted) {
        m_loader.reset(x3::asset::createModelLoader(&device, m_assets.get()));
        m_model = m_loader->load(file);
        if (m_model.ok)
            m_drawables = x3::asset::makeDrawables(m_model);
    } else {
        x3::logWarn("[rescue] mountDir failed: " + std::string(modelDir));
    }

    for (int i = 0; i < 16; ++i) m_modelFixup[i] = (i % 5 == 0) ? 1.0f : 0.0f;

    if (!m_drawables.empty()) {
        m_usingReal  = true;
        m_modelScale = kVictimScale;
        x3::logInfo("[rescue] " + m_name + ": loaded " + file + " — " +
                    std::to_string(m_drawables.size()) + " primitive(s)");

        // ---- BUG #48: bind the skeletal-animation runtime so the girl breathes/
        // idles (and a companion walks) instead of freezing at bind/T-pose. Mirrors
        // MonsterSystem::buildMonsterTuned exactly: bind(), resolve idle/walk/run by
        // fuzzy name, drive the 1D locomotion blend by planar speed when a walk/run
        // set exists (else degrade to the single Idle clip). Per-girl PHASE OFFSET so
        // the three captives don't breathe in lockstep. Skip when the model isn't
        // skinnable (no skin/clips) -> static draw, no regression. ----
        if (m_model.ok && m_skinner.bind(m_model)) {
            // ROOT-Y LOCK (buried/bouncing guard): a victim's world Y is OWNED by
            // her physics spawn (m_pos) — any clip with a broken baked root Y
            // (the retarget pipeline's Jake -0.9488 armature-offset family, see
            // anim.h setRootYLock) must never sink or bob her through the floor.
            // Idle sway/lean is untouched (X/Z + rotations pass through).
            m_skinner.setRootYLock(true);
            // GPU compute-skin when the device supports it (crowd-scalable); CPU LBS
            // fallback otherwise (headless / non-compute) — transparent to tick().
            const bool gpuSkin = m_skinner.enableGpuSkinning(device, m_model);
            m_idleClip = m_skinner.findClip({ "idle", "stand", "breath", "loop" });
            m_walkClip = m_skinner.findClip({ "walk" });
            m_runClip  = m_skinner.findClip({ "run", "sprint", "jog" });
            if (m_walkClip < 0) m_walkClip = m_skinner.findClip({ "move", "jog", "run" });
            if (m_idleClip < 0) m_idleClip = 0;   // fall back to the first clip
            m_animActive   = (m_idleClip >= 0);
            m_useLocoBlend = m_animActive && (m_walkClip >= 0 || m_runClip >= 0);
            if (m_useLocoBlend)
                m_skinner.setLocomotionClips(m_idleClip, m_walkClip, m_runClip, 0.2f, 2.0f);
            // Per-girl phase offset (seconds) so the captives idle out of sync. Spread
            // them across ~0.8 s by the victim index (Aria 0, Keisha .27, Emily .53).
            m_phaseOffset = 0.27f * (float)(uint32_t)id;
            m_animTime    = m_phaseOffset;
            // Pose the bind-pose mesh into the idle pose up front so the FIRST rendered
            // frame already shows the animated pose (not bind/T-pose) — the #48 fix.
            if (m_animActive && m_device) {
                if (m_useLocoBlend) {
                    m_skinner.setLocomotionSpeed(0.0f);                       // start idle
                    m_skinner.applyLocomotion(m_model, *m_device, m_phaseOffset);
                } else {
                    m_skinner.apply(m_model, *m_device, (uint32_t)m_idleClip, m_animTime);
                }
            }
            x3::logInfo("[rescue] " + m_name + " is animated (" +
                        (gpuSkin ? "GPU" : "CPU") + " skin) — idle=" +
                        std::to_string(m_idleClip) + " walk=" + std::to_string(m_walkClip) +
                        " run=" + std::to_string(m_runClip) + " locoBlend=" +
                        (m_useLocoBlend ? "1" : "0") + " phaseOff=" +
                        std::to_string(m_phaseOffset));
        } else {
            x3::logInfo("[rescue] " + m_name + ": " + file +
                        " not skinnable (no skin/clips) — static draw");
        }
    } else {
        // Fallback box so the victim still exists + is rescuable headlessly.
        m_usingReal  = false;
        m_modelScale = kBoxScale;
        x3::logWarn("[rescue] " + m_name + ": " + file + " load failed; using fallback box");
        x3::prims::PrimMesh geo = x3::prims::makeBox(kVictimHalf.x, kVictimHalf.y,
                                                     kVictimHalf.z, 0.0f, 0.0f, 0.0f, 1.0f);
        x3::rhi::MeshHandle mesh = device.createMesh(
            geo.verts.data(), (uint32_t)geo.verts.size(),
            geo.index.data(), (uint32_t)geo.index.size());
        x3::asset::ModelDrawable d;
        d.meshId = mesh.id;
        d.baseColorTexId = 0;
        d.baseColorFactor[0] = 0.6f; d.baseColorFactor[1] = 0.85f;
        d.baseColorFactor[2] = 1.0f; d.baseColorFactor[3] = 1.0f;  // friendly blue
        m_drawables.push_back(d);
    }

    // ---- Static-by-mass Enemy-layer box: lets the host (future) raycast it and
    // lets the companion move via setBodyPosition (same teleport trick the monster
    // chase + S4 door use). Mass 0 so it stays put in the ward. ----
    m_body = physics.addBox(kVictimHalf, m_pos, 0.0f, x3::phys::Layer::Enemy);

    // ---- Tag::Prop entity: bookkeeping only; render mesh left invalid so
    // Scene::render skips it and draw() owns the multi-primitive draw. ----
    Entity e;
    e.tag     = (uint32_t)Tag::Prop;
    e.visible = true;
    e.body    = m_body;
    composeTRS(e.transform,
               x3::phys::Vec3{1,0,0}, x3::phys::Vec3{0,1,0}, x3::phys::Vec3{0,0,1},
               m_modelScale, m_pos);
    m_entity = scene.add(e);

    x3::logInfo("[rescue] victim " + m_name + " (entity " + std::to_string(m_entity) +
                ") placed at (" + std::to_string(pos.x) + ", " + std::to_string(pos.y) +
                ", " + std::to_string(pos.z) + ") timer=" +
                std::to_string((int)timer) + "s" +
                (m_usingReal ? " [real GLB]" : " [fallback box]"));
}

void RescueVictim::bakeTransform(Scene& scene) {
    if (m_entity == kNoLink || m_entity >= scene.size()) return;
    // FACING FIX (matches monster.cpp): the rigged character GLBs are authored facing
    // +Z, but headingToFace()/m_yaw assume local -Z forward (CONVENTIONS) — so the mesh
    // renders with its BACK to its target. Flip the VISUAL yaw 180deg here ONLY (m_yaw
    // and the follow/face-player logic are unchanged, so --test-rescue stays correct).
    const float ry = m_yaw + 3.14159265358979323846f;
    const float c = std::cos(ry), s = std::sin(ry);
    Entity& me = scene.get(m_entity);
    composeTRS(me.transform,
               x3::phys::Vec3{ c, 0.0f, -s },
               x3::phys::Vec3{ 0.0f, 1.0f, 0.0f },
               x3::phys::Vec3{ s, 0.0f, c },
               m_modelScale, m_pos);
}

void RescueVictim::driveAnim(float dt, float planarSpeed) {
    if (!m_animActive || !m_device) return;
    if (m_useLocoBlend) {
        // 1D Idle/Walk/Run blend driven by planar speed (stationary captive -> idle).
        m_skinner.setLocomotionSpeed(planarSpeed);
        m_skinner.applyLocomotion(m_model, *m_device, dt);
    } else {
        // Single-clip path (rig has only Idle, or no distinct walk/run). Play Walk
        // when one exists + moving; else play Idle, pumped faster while moving so an
        // Idle-only rig reads as advancing instead of dead-static. Mirrors monster.cpp.
        const bool hasWalk = (m_walkClip >= 0);
        const bool moving  = planarSpeed > 0.25f;
        const int  clip    = (hasWalk && moving) ? m_walkClip : m_idleClip;
        const float rate   = (!hasWalk && moving)
            ? (1.0f + (planarSpeed < 4.0f ? planarSpeed : 4.0f) * 0.35f)
            : 1.0f;
        m_animTime += dt * rate;
        m_skinner.apply(m_model, *m_device, (uint32_t)clip, m_animTime);
    }
}

// ===========================================================================
// PHYSICS RAGDOLL (collapse). SAME machinery as MonsterSystem::spawnDeathRagdoll:
// build a Jolt humanoid ragdoll from the canonical rig, placed/yawed/scaled to
// match Aria, seeded from her CURRENT animated bone globals (seamless flop start),
// added to the SHARED physics world. tick() then reads the bones back and drives
// the skin via applyExternalGlobals. No-op unless the model is skinnable. Idempotent.
// ===========================================================================
void RescueVictim::ragdoll(Scene& scene, x3::phys::IPhysicsWorld& physics) {
    if (m_ragdolled || m_deathRagdoll) return;          // already collapsed (idempotent)
    if (!m_animActive || !m_skinner.valid() || m_skinner.nodeCount() == 0) {
        x3::logInfo("[rescue] " + m_name + ": ragdoll() skipped — model not skinnable");
        return;                                          // unrigged -> stays standing (no regression)
    }

    // Remove her STANDING collision box (Layer::Enemy, mass 0) so it can't fight the
    // falling ragdoll bodies (also Layer::Enemy) — mirrors the monster dropping its
    // body at the kill. CRITICALLY also DROP the entity's body LINK: otherwise
    // Scene::update() keeps reading the (now-removed) body and corrupts the frozen
    // collapse transform, teleporting the skinned mesh away. The entity transform is
    // left frozen at the collapse-time bake; the skin palette carries the world placement.
    if (m_entity != kNoLink && m_entity < scene.size()) {
        Entity& me = scene.get(m_entity);
        me.body = x3::phys::BodyId{};   // freeze: Scene::update() now skips this entity
    }
    if (m_body.valid()) { physics.removeBody(m_body); m_body = x3::phys::BodyId{}; }

    // ---- The FROZEN draw transform the mesh renders through at collapse:
    //   final = model * fixup * skinGlobal,  model = T(m_pos) * R(yaw+pi) * S(scale).
    // Build `model` the SAME way bakeTransform()'s facing bake does (the +pi VISUAL
    // flip — the rigged GLBs face +Z), fold in m_modelFixup (identity here), and
    // capture inv(model*fixup) once. Ragdoll bone world transforms map through this
    // inverse into skin space so the rigid delta composes under the unchanged draw. --
    const float ry = m_yaw + 3.14159265358979323846f;
    const float c = std::cos(ry), s = std::sin(ry);
    const float scale = m_modelScale;
    float model[16];
    composeTRS(model,
               x3::phys::Vec3{ c, 0.0f, -s },
               x3::phys::Vec3{ 0.0f, 1.0f, 0.0f },
               x3::phys::Vec3{ s, 0.0f, c },
               scale, m_pos);
    float drawXform[16];
    x3::asset::mulMat4(model, m_modelFixup, drawXform);   // model * fixup (skin->world)
    if (!invertAffineUniform(drawXform, m_deathModelInv)) {
        x3::logWarn("[rescue] " + m_name + ": ragdoll() degenerate draw matrix — skipped");
        return;
    }

    // ---- Canonical humanoid rig in WORLD space, matching Aria: the rig is authored
    // with the PELVIS at originY and the legs hanging below (shins reach ~0.58 m down).
    // m_pos is Aria's FEET on the floor, so lift the placement by the pelvis height so
    // the rig spawns standing on the floor (feet at m_pos) and topples ONTO it — rather
    // than spawning half-buried in the floor slab. (mirrors monster.cpp's grounded rig). -
    constexpr float kRigPelvisH = 0.9f;   // pelvis height above the feet in the canonical rig
    x3::phys::makeHumanoidRagdollBones(/*originY*/0.0f, m_ragdollBones);
    const uint32_t bn = (uint32_t)m_ragdollBones.size();
    if (bn == 0) { m_ragdollBones.clear(); return; }

    const x3::phys::Vec3 footPos{ m_pos.x, m_pos.y + kRigPelvisH * scale, m_pos.z };
    float place[16];
    composeTRS(place,
               x3::phys::Vec3{ c, 0.0f, -s },
               x3::phys::Vec3{ 0.0f, 1.0f, 0.0f },
               x3::phys::Vec3{ s, 0.0f, c },
               scale, footPos);
    for (uint32_t b = 0; b < bn; ++b) {
        float placed[16];
        x3::asset::mulMat4(place, m_ragdollBones[b].bindWorld, placed);
        std::memcpy(m_ragdollBones[b].bindWorld, placed, 16 * sizeof(float));
        m_ragdollBones[b].halfHeight *= scale;
        m_ragdollBones[b].radius     *= scale;
    }

    m_deathRagdoll.reset(x3::phys::createRagdoll(physics, m_ragdollBones.data(), bn));
    if (!m_deathRagdoll) { m_ragdollBones.clear(); return; }   // bail -> stays standing

    // Snap to the placed bind pose + add to the world (active).
    {
        std::vector<float> bind((size_t)bn * 16);
        for (uint32_t b = 0; b < bn; ++b)
            std::memcpy(&bind[b*16], m_ragdollBones[b].bindWorld, 16 * sizeof(float));
        m_deathRagdoll->addToWorld(/*activate*/true);
        m_deathRagdoll->setPoseWorld(bind.data());
    }

    // ---- Seed the rigid bone->skin driver from Aria's CURRENT animated pose so the
    // collapse starts seamlessly from where the idle/loco left off (no pop). Capture
    // the Skinner's current global bone transforms and bind RagdollSkin from THEM. The
    // pose clip is the one driveAnim() last played (idle, or the loco set at idle). ----
    {
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

    // Capture the ragdoll's INITIAL bone world transforms (post setPose) -> model-local
    // for the RagdollSkin part frames, then map every skin node to its nearest bone.
    m_ragWorldScratch.assign((size_t)bn * 16, 0.0f);
    m_ragPartInit.assign((size_t)bn * 16, 0.0f);
    m_deathRagdoll->getBoneWorldTransforms(m_ragWorldScratch.data());
    for (uint32_t b = 0; b < bn; ++b)
        x3::asset::mulMat4(m_deathModelInv, &m_ragWorldScratch[b*16], &m_ragPartInit[b*16]);
    m_ragdollSkin.mapToParts(m_ragPartInit.data(), bn);
    m_ragPartCur.assign((size_t)bn * 16, 0.0f);

    // Collapse impulse: a forward + downward shove so she folds/sprawls to the floor
    // (she's not "killed" by a hit here, so just topple her over).
    const float fx = std::sin(m_yaw), fz = std::cos(m_yaw);
    m_deathRagdoll->applyImpulseAll(x3::phys::Vec3{ fx * 2.5f, -0.5f, fz * 2.5f });

    m_ragdolled = true;
    x3::logInfo("[rescue] " + m_name + ": COLLAPSED into a physics ragdoll (" +
                std::to_string(bn) + " bones)");
}

// Per-frame while ragdolled: read the ragdoll bone WORLD transforms, map to skin
// space, run the rigid bone->skin attach, feed the result to applyExternalGlobals so
// the GPU-skinned mesh flops with the bodies. Mirrors MonsterSystem::driveSkinFromRagdoll.
void RescueVictim::driveSkinFromRagdoll() {
    if (!m_ragdolled || !m_deathRagdoll || !m_device) return;
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

// W5-2: shared expiry — the 5-min timer and the interrupt hard cutoff end the same
// way (RESCUE_SETPIECE_DESIGN.md tier 3 reuses the existing transform path).
void RescueVictim::expire(Scene& scene, x3::phys::IPhysicsWorld& physics) {
    m_timeLeft = 0.0f;
    m_state = VictimState::Expired;
    m_assaultActive = false;
    if (m_entity != kNoLink && m_entity < scene.size()) {
        Entity& me = scene.get(m_entity);
        me.visible = false;
        me.body = x3::phys::BodyId{};
    }
    if (m_body.valid()) { physics.removeBody(m_body); m_body = x3::phys::BodyId{}; }
}

void RescueVictim::resolveTier(RescueTier t) {
    m_tier = t;
    m_assaultActive = false;
    m_assaultArmed  = false;   // the tell is over either way
    if (t == RescueTier::Wounded) {
        // Aftermath table (§2): subtle blood-multiply — she reads hurt, not gory —
        // and the run clip is suppressed (walk-pace only) until a story beat clears it.
        setTint(0.85f, 0.52f, 0.48f, 1.0f);
        m_runCap = true;
        x3::logInfo("[rescue] " + m_name + ": interrupt WOUNDED (window " +
                    std::to_string(m_windowT) + "s) — she is hurt but saved");
    } else {
        x3::logInfo("[rescue] " + m_name + ": interrupt CLEAN (window " +
                    std::to_string(m_windowT) + "s)");
    }
}

bool RescueVictim::tick(float dt, bool hubReached, Scene& scene,
                        x3::phys::IPhysicsWorld& physics,
                        const x3::phys::Vec3& playerPos,
                        uint32_t aliveAttackers) {
    if (m_state == VictimState::Expired) return false;

    // ---- RAGDOLLED: she's collapsed — the host already stepped the shared physics
    // world this frame, so just read the bone transforms OUT and flop the skin to
    // match (replaces the animation clip + follow/face logic). The entity transform is
    // FROZEN at the collapse pose (the skin globals carry the world placement now). --
    if (m_ragdolled) {
        driveSkinFromRagdoll();
        return false;
    }

    // ---- Companion: light follow AI (mirror the monster chase movement). Hold a
    // standoff so it trails the player rather than crowding it; snap-catch up if
    // left way behind (e.g. after an elevator ride). ----
    if (m_state == VictimState::Companion) {
        const x3::phys::Vec3 prevPos = m_pos;   // for the locomotion-speed measure
        const float dx = playerPos.x - m_pos.x, dz = playerPos.z - m_pos.z;
        const float horiz = std::sqrt(dx * dx + dz * dz);
        if (horiz > kCompanionTeleport) {
            // Lost too far behind — snap just behind the player.
            m_pos.x = playerPos.x; m_pos.z = playerPos.z;
        } else if (horiz > kCompanionStop) {
            const float inv = (horiz > 1e-4f) ? 1.0f / horiz : 0.0f;
            const float mx = dx * inv, mz = dz * inv;
            const float step = kCompanionSpeed * dt;
            // Don't overshoot past the standoff ring.
            const float travel = (horiz - kCompanionStop < step) ? (horiz - kCompanionStop) : step;
            m_pos.x += mx * travel; m_pos.z += mz * travel;
            m_yaw = headingToFace(dx, dz);   // face the player while following
        }
        if (m_body.valid()) physics.setBodyPosition(m_body, m_pos);
        bakeTransform(scene);
        // BUG #48: drive the walk/idle blend from this frame's planar movement (a
        // teleport-snap is ignored — it's not real locomotion) so a following
        // companion visibly walks and a held one idles.
        const float ddx = m_pos.x - prevPos.x, ddz = m_pos.z - prevPos.z;
        float planarSpeed = (dt > 1e-5f) ? std::sqrt(ddx*ddx + ddz*ddz) / dt : 0.0f;
        if (planarSpeed > kCompanionTeleport) planarSpeed = 0.0f;   // ignore snap jumps
        // W5-2 Wounded aftermath: the run clip is suppressed — cap the locomotion
        // input under the walk->run blend threshold so she never breaks into a run.
        if (m_runCap && planarSpeed > 1.9f) planarSpeed = 1.9f;
        driveAnim(dt, planarSpeed);
        return false;
    }

    // ---- Captive: breathe/idle EVERY frame (BUG #48), whether or not the hub
    // timer is running, so a held captive isn't a frozen mannequin. Stationary ->
    // speed 0 -> the Idle clip (or locomotion blend collapsed to idle). Bake the
    // transform too (mirrors the Companion path) so the captive gets the 180deg
    // facing-FLIP — without it she keeps the identity-basis init transform from
    // build() and renders facing the wrong way (back to the player). ----
    bakeTransform(scene);
    driveAnim(dt, 0.0f);

    // ---- Captive: run the countdown once the hub is reached. ----
    if (!hubReached) return false;

    // ---- W5-2: the interrupt window (only when configured; legacy paths — and the
    // pre-existing tests — see identical behavior when configureAssault was never
    // called). Arm on approach (the door tell), open the window on close approach
    // (the burst-in), resolve on last-attacker-death, hard-cutoff to Lost. ----
    if (m_assaultConfigured && m_tier == RescueTier::None) {
        const float pdx = playerPos.x - m_pos.x, pdz = playerPos.z - m_pos.z;
        const float pd2 = pdx * pdx + pdz * pdz;
        if (!m_assaultArmed && pd2 <= m_armR2) {
            m_assaultArmed = true;
            x3::logInfo("[rescue] " + m_name + ": assault ARMED — the door tell begins");
        }
        if (m_assaultArmed && !m_assaultActive && pd2 <= m_activeR2) {
            m_assaultActive = true;
            m_windowT = 0.0f;
            x3::logInfo("[rescue] " + m_name + ": interrupt WINDOW OPEN (clean<=" +
                        std::to_string(m_cleanS) + "s cutoff=" + std::to_string(m_woundedS) + "s)");
        }
        if (m_assaultActive) {
            m_windowT += dt;
            if (aliveAttackers == 0) {
                resolveTier(m_windowT <= m_cleanS ? RescueTier::Clean : RescueTier::Wounded);
            } else if (m_windowT > m_woundedS) {
                resolveTier(RescueTier::Lost);
                x3::logInfo("[rescue] " + m_name +
                            ": interrupt LOST — the window closed with attackers alive");
                expire(scene, physics);
                return true;   // host spawns her boss this frame (same as timer expiry)
            }
        }
    }

    if (m_timeLeft > 0.0f) {
        m_timeLeft -= dt;
        if (m_timeLeft <= 0.0f) {
            // ---- Timer expired: the captive transforms into a boss. Hide the
            // captive (its entity + body go away; the host spawns the boss via the
            // bossTuning getter so the new boss owns its own mesh/body). ----
            if (m_tier == RescueTier::None) m_tier = RescueTier::Lost;
            expire(scene, physics);
            x3::logInfo("[rescue] " + m_name + ": TIMER EXPIRED — transforming into a boss");
            return true;   // signal the host to spawn the boss this frame
        }
    }
    return false;
}

bool RescueVictim::tryRescue(const x3::phys::Vec3& playerPos, float reach) {
    if (m_state != VictimState::Captive) return false;
    const float dx = playerPos.x - m_pos.x, dz = playerPos.z - m_pos.z;
    const float horiz = std::sqrt(dx * dx + dz * dz);
    if (horiz > reach) return false;
    m_state = VictimState::Companion;
    m_timeLeft = m_timerMax;   // freeze the displayed time (timer stops on rescue)
    m_yaw = headingToFace(dx, dz);
    x3::logInfo("[rescue] " + m_name + ": RESCUED — now a friendly companion");
    return true;
}

void RescueVictim::extract(Scene& scene, x3::phys::IPhysicsWorld& physics) {
    if (m_state != VictimState::Companion) return;
    m_state = VictimState::Extracted;
    // The Expired vanish pattern: hide the entity, drop the collision body. The
    // loaded model stays owned (app lifetime) like every other character system.
    if (m_entity != kNoLink && m_entity < scene.size()) {
        Entity& me = scene.get(m_entity);
        me.visible = false;
        me.body = x3::phys::BodyId{};
    }
    if (m_body.valid()) { physics.removeBody(m_body); m_body = x3::phys::BodyId{}; }
    x3::logInfo("[rescue] " + m_name + ": EXTRACTED — safe at the elevator, leaving the level");
}

void RescueVictim::draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                        const Scene& scene) const {
    if (m_state == VictimState::Expired) return;     // the captive is gone (a boss now)
    if (m_state == VictimState::Extracted) return;   // she left the level (W4-1)
    if (m_entity == kNoLink || m_entity >= scene.size()) return;
    const Entity& e = scene.get(m_entity);
    if (!e.visible) return;
    drawAt(device, frame, e.transform);
}

void RescueVictim::drawAt(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                          const float model[16]) const {
    for (const auto& d : m_drawables) {
        float color[4] = {
            d.baseColorFactor[0] * m_tint[0],
            d.baseColorFactor[1] * m_tint[1],
            d.baseColorFactor[2] * m_tint[2],
            d.baseColorFactor[3] * m_tint[3],
        };
        float mf[16], fin[16];
        x3::asset::mulMat4(model, m_modelFixup, mf);
        x3::asset::mulMat4(mf, d.nodeTransform, fin);
        device.drawMesh(frame,
                        x3::rhi::MeshHandle{ d.meshId },
                        x3::rhi::TextureHandle{ d.baseColorTexId },
                        color, fin);
    }
}

// ===========================================================================
// RescueSystem
// ===========================================================================
void RescueSystem::build(Scene& scene, x3::rhi::IRenderDevice& device,
                         x3::phys::IPhysicsWorld& physics, std::string_view modelDir,
                         const x3::phys::Vec3& wardA, const x3::phys::Vec3& wardB,
                         const x3::phys::Vec3& wardC, float timer) {
    m_modelDir = std::string(modelDir);
    m_device   = &device;

    struct Spec { VictimId id; const char* name; const char* live; const char* boss;
                  float bossHp; x3::phys::Vec3 pos; };
    // Live victims map to Anna/character GLBs (spec §7); each transforms into its
    // boss on expiry: Aria->The Siren, Keisha->Breeder Queen, Emily->Oracle (§5).
    const Spec specs[3] = {
        { VictimId::Aria,   "Aria",   "AnnaCasual.glb",   "BossTheSiren.glb",     520.0f, wardA },
        { VictimId::Keisha, "Keisha", "AnnaBodySuit_anim.glb", "BossBreederQueen_anim.glb", 600.0f, wardB },
        { VictimId::Emily,  "Emily",  "AnnaTactical.glb", "Oracle_anim.glb",           480.0f, wardC },
    };

    for (const Spec& sp : specs) {
        // Boss tuning the victim becomes on expiry — a Boss-type so it runs the same
        // phase machine as Martinez. Rigged sources are Y-up (standUpZtoY=false).
        MonsterSystem::Tuning bt;
        bt.type           = MonsterType::Boss;
        bt.hp             = (int)sp.bossHp;
        bt.chaseSpeed     = 3.2f;
        bt.damage         = 14;
        bt.attackRange    = 2.3f;
        bt.attackCooldown = 1.1f;
        bt.attackWindup   = 0.30f;
        bt.ranged         = false;
        bt.tint[0] = 1.0f; bt.tint[1] = 0.45f; bt.tint[2] = 0.60f; bt.tint[3] = 1.0f; // sinister magenta
        bt.modelFile        = sp.boss;
        bt.modelDirOverride = m_modelDir;
        bt.standUpZtoY      = false;     // rigged bosses authored Y-up
        bt.modelScale       = 1.4f;      // bosses read taller

        auto v = std::make_unique<RescueVictim>();
        v->build(scene, device, physics, m_modelDir, sp.pos,
                 sp.id, sp.name, sp.live, timer, bt);
        m_victims.push_back(std::move(v));
    }

    m_built = true;

    // ---- W5-2: per-ward interrupt windows (RESCUE_SETPIECE_DESIGN.md §5 pacing).
    // A (Aria, 2 attackers, the teaching ward): a generous first window. B (Keisha,
    // 1 attacker): tight solo — her defiance rewards aggression. C (Emily, 2
    // attackers): the hardest count, longest clock. Timings resolve on TIME-TO-KILL;
    // the diegetic ring makes the clock visible (Tim's countdown hedge, rescue_ring).
    configureAssault((uint32_t)VictimId::Aria,   20.0f, 38.0f);
    configureAssault((uint32_t)VictimId::Keisha, 16.0f, 30.0f);
    configureAssault((uint32_t)VictimId::Emily,  26.0f, 48.0f);

    x3::logInfo("RescueSystem::build complete — 3 victims (Aria/Keisha/Emily) on " +
                std::to_string((int)timer) + "s timers (run once the F2 hub is reached)"
                "; interrupt windows armed (A 20/38, K 16/30, E 26/48)");
}

// W5-2: scene-truth attacker liveness. A dead monster drops its physics body AND
// clears its entity's body link (monster.cpp kill path), so a LIVING attacker near
// the victim is: Tag::Enemy entity, valid body link, within `radius` in XZ.
uint32_t RescueSystem::aliveAttackersNear(const Scene& scene, const x3::phys::Vec3& pos,
                                          float radius) {
    const float r2 = radius * radius;
    uint32_t n = 0;
    for (uint32_t e = 0; e < scene.size(); ++e) {
        const Entity& en = scene.get(e);
        if (en.tag != (uint32_t)Tag::Monster) continue;
        if (!en.body.valid()) continue;               // corpse: body link dropped at the kill
        const float dx = en.transform[12] - pos.x, dz = en.transform[14] - pos.z;
        if (dx * dx + dz * dz <= r2) ++n;
    }
    return n;
}

void RescueSystem::escalationTick(float dt) {
    m_ringClock += dt;
    // Most urgent active window (frac 1 -> 0). Escalate under a third remaining.
    float worst = 1.0f;
    for (const auto& v : m_victims)
        if (v->assaultActive()) worst = std::min(worst, v->windowFrac());
    const bool urgent = (worst < 0.33f);
    if (m_escAudio && m_heartbeat.valid()) {
        if (urgent && !m_heartLoop.valid()) {
            m_heartLoop = m_escAudio->startLoop(m_heartbeat, 0.55f, 1.0f);
        } else if (urgent && m_heartLoop.valid()) {
            // Pitch + volume climb as the window closes (frac 0.33 -> 0).
            const float k = 1.0f - worst / 0.33f;                 // 0 -> 1
            m_escAudio->setLoopParams(m_heartLoop, 0.55f + 0.35f * k, 1.0f + 0.22f * k);
        } else if (!urgent && m_heartLoop.valid()) {
            m_escAudio->stopLoop(m_heartLoop);
            m_heartLoop = {};
        }
    }
}

void RescueSystem::tick(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics,
                        const x3::phys::Vec3& playerPos) {
    if (!m_built) return;

    m_extractedThisFrame = UINT32_MAX;
    m_tierResolvedThisFrame = UINT32_MAX;
    // W5-2: lazily build the countdown-ring pip on the first frame any window runs.
    if (m_ringEnabled && !m_ringPip.valid() && m_device) {
        bool anyActive = false;
        for (const auto& v : m_victims) if (v->assaultActive()) { anyActive = true; break; }
        if (anyActive) {
            x3::prims::PrimMesh pip = x3::prims::makeBox(0.055f, 0.012f, 0.028f,
                                                         0.0f, 0.0f, 0.0f, 1.0f);
            m_ringPip = m_device->createMesh(pip.verts.data(), (uint32_t)pip.verts.size(),
                                             pip.index.data(), (uint32_t)pip.index.size());
        }
    }
    for (size_t vi = 0; vi < m_victims.size(); ++vi) {
        auto& v = m_victims[vi];
        // W5-2: scene-truth attacker count near this victim (7 m ward bubble); only
        // meaningful while her window is armed/active — cheap either way (3 victims).
        const uint32_t alive = aliveAttackersNear(scene, v->pos(), 7.0f);
        const RescueTier tierBefore = v->tier();
        const bool expiredNow = v->tick(dt, m_hubReached, scene, physics, playerPos, alive);
        if (tierBefore == RescueTier::None &&
            (v->tier() == RescueTier::Clean || v->tier() == RescueTier::Wounded))
            m_tierResolvedThisFrame = (uint32_t)vi;   // host: flag + bark on Wounded
        if (expiredNow && m_device) {
            // Spawn the boss the victim transforms into, at the victim's ward spot.
            const x3::phys::Vec3 at{ v->pos().x, 0.4f, v->pos().z };
            m_bosses.spawn(scene, *m_device, physics, m_modelDir, at, v->bossTuning());
            x3::logInfo("[rescue] " + v->name() + " transformed — boss spawned (" +
                        std::to_string(m_bosses.count()) + " rescue boss(es) active)");
        }
        // W4-1: a following companion that reaches the extraction point leaves the
        // level. Checked AFTER the follow step so arriving-this-frame extracts now.
        if (m_extractSet && v->companion()) {
            const float ex = v->pos().x - m_extractPos.x, ez = v->pos().z - m_extractPos.z;
            if (ex * ex + ez * ez <= m_extractR2) {
                v->extract(scene, physics);
                m_extractedThisFrame = (uint32_t)vi;   // host reads for bark + flag
            }
        }
    }

    // Advance the transformed-victim bosses (movement-only here; the host wires the
    // attack target/FX/phase callbacks when it folds these into the combat loop —
    // for now they chase, exactly like the legacy single-monster path).
    m_bosses.update(dt, scene, physics, playerPos);
}

bool RescueSystem::tryRescue(const x3::phys::Vec3& playerPos, float reach) {
    if (!m_built) return false;
    // Rescue the NEAREST in-range captive (so an E press near two wards is unambiguous).
    RescueVictim* best = nullptr;
    float bestD = reach * reach;
    for (auto& v : m_victims) {
        if (!v->captive()) continue;
        const float dx = playerPos.x - v->pos().x, dz = playerPos.z - v->pos().z;
        const float d2 = dx * dx + dz * dz;
        if (d2 <= bestD) { bestD = d2; best = v.get(); }
    }
    if (!best) return false;
    return best->tryRescue(playerPos, reach);
}

void RescueSystem::draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                        const Scene& scene) const {
    for (const auto& v : m_victims) v->draw(device, frame, scene);
    m_bosses.drawAll(device, frame, scene);

    // ---- W5-2: the DIEGETIC countdown ring — a circle of emissive pips around an
    // active tableau, pips dying as the window drains, amber -> red. In-world (not a
    // HUD arc) per the bible's instrument law: the countdown is a thing in the room.
    if (!m_ringEnabled || !m_ringPip.valid()) return;
    constexpr int   kPips   = 24;
    constexpr float kRingR  = 1.15f;
    constexpr float kPi2    = 6.28318530718f;
    const x3::rhi::TextureHandle white{ 0 };
    for (const auto& v : m_victims) {
        if (!v->assaultActive()) continue;
        const float frac = v->windowFrac();               // 1 -> 0 as time drains
        const int   lit  = (int)(frac * kPips + 0.5f);
        const x3::phys::Vec3 p = v->pos();
        // Amber early, blood-red as the window closes (lerp by remaining fraction).
        float cr = 1.0f, cg = 0.15f + 0.55f * frac, cb = 0.08f * frac;
        // HYBRID ESCALATION: under a third remaining the ring goes full blood-red
        // and THROBS — pips swell + flare on a fast pulse (with the heartbeat).
        float pipScale = 1.0f, emGain = 1.35f;
        if (frac < 0.33f) {
            const float pulse = std::max(0.0f, std::sin(m_ringClock * 9.0f));
            cg = 0.06f; cb = 0.03f;
            pipScale = 1.0f + 0.45f * pulse;
            emGain   = 1.35f + 1.3f * pulse;
        }
        for (int i = 0; i < lit; ++i) {
            const float a = kPi2 * (float)i / (float)kPips;
            const float c = std::cos(a), s = std::sin(a);
            float t[16] = { c*pipScale,0,-s*pipScale,0,  0,pipScale,0,0,  s*pipScale,0,c*pipScale,0,
                            p.x + c * kRingR, p.y + 0.18f, p.z + s * kRingR, 1 };
            const float col[4]  = { cr, cg, cb, 1.0f };
            const float emis[4] = { cr, cg, cb, emGain };
            device.drawMeshEmissive(frame, m_ringPip, white, col, emis, t);
        }
    }
}

std::vector<RescueTimerHud> RescueSystem::hudTimers() const {
    std::vector<RescueTimerHud> rows;
    for (const auto& v : m_victims) {
        if (!v->captive()) continue;          // only running countdowns show
        RescueTimerHud r;
        r.name    = v->name();
        r.seconds = v->timeLeft();
        r.urgent  = (v->timeLeft() < 60.0f);
        rows.push_back(std::move(r));
    }
    return rows;
}

uint32_t RescueSystem::rescuedCount() const {
    uint32_t n = 0;
    for (const auto& v : m_victims) if (v->companion()) ++n;
    return n;
}

uint32_t RescueSystem::expiredCount() const {
    uint32_t n = 0;
    for (const auto& v : m_victims) if (v->expired()) ++n;
    return n;
}

RescueSystem::SaveState RescueSystem::serialize() const {
    SaveState s;
    s.hubReached = m_hubReached;
    s.victims.reserve(m_victims.size());
    for (const auto& v : m_victims) s.victims.push_back(v->save());
    return s;
}

void RescueSystem::deserialize(const SaveState& s) {
    m_hubReached = s.hubReached;
    const size_t n = (s.victims.size() < m_victims.size()) ? s.victims.size() : m_victims.size();
    for (size_t i = 0; i < n; ++i) m_victims[i]->load(s.victims[i]);
}

// ===========================================================================
// Headless self-test (--test-rescue). No window / Vulkan.
// ===========================================================================
namespace {

int g_rpass = 0, g_rfail = 0;
void rcheck(bool cond, const char* name) {
    if (cond) { ++g_rpass; x3::logInfo(std::string("[rescue-test] PASS ") + name); }
    else      { ++g_rfail; x3::logError(std::string("[rescue-test] FAIL ") + name); }
}

// Headless IRenderDevice: the shared no-op test-double (app/headless_device.h).
// Mints valid handles so build() runs without Vulkan; draw/frame calls are
// no-ops.
using HeadlessDevice = x3::game::HeadlessRenderDevice;

// ---- R8c negative-control asset: a tiny skinned GLB whose ONE clip carries a
// BROKEN baked root Y (the retarget bug family: hips rest at +0.9 but the clip
// translates them to ~-0.05..-0.25 — a metre buried, bouncing 0.2 m), plus a
// rotation channel so the lock's "rotations still animate" contract is provable.
// Structure: node0 = skinned mesh, node1 = "Armature" (identity), node2 = "Hips"
// joint (rest T = [0, 0.9, 0]). Mirrors --test-anim's makeSkinnedGlb().
void appendU32(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back(uint8_t(v));       b.push_back(uint8_t(v >> 8));
    b.push_back(uint8_t(v >> 16)); b.push_back(uint8_t(v >> 24));
}
std::vector<uint8_t> makeBuriedRootGlb() {
    struct V { float p[3]; float n[3]; float uv[2]; uint16_t j[4]; float w[4]; };
    std::vector<V> v = {
        {{-0.1f, 0.0f, 0}, {0,0,1}, {0,0}, {0,0,0,0}, {1,0,0,0}},
        {{ 0.1f, 0.0f, 0}, {0,0,1}, {1,0}, {0,0,0,0}, {1,0,0,0}},
        {{-0.1f, 1.8f, 0}, {0,0,1}, {0,1}, {0,0,0,0}, {1,0,0,0}},
        {{ 0.1f, 1.8f, 0}, {0,0,1}, {1,1}, {0,0,0,0}, {1,0,0,0}},
    };
    std::vector<uint16_t> idx = { 0,1,2, 2,1,3 };

    std::vector<uint8_t> bin;
    auto put = [&](const void* d, size_t n) {
        const uint8_t* p = (const uint8_t*)d; bin.insert(bin.end(), p, p + n);
    };
    auto align4 = [&]{ while (bin.size() % 4 != 0) bin.push_back(0); };

    const size_t nv = v.size();
    size_t posOfs = bin.size(); for (auto& vv : v) put(vv.p, 12);
    size_t nrmOfs = bin.size(); for (auto& vv : v) put(vv.n, 12);
    size_t uvOfs  = bin.size(); for (auto& vv : v) put(vv.uv, 8);
    size_t jOfs   = bin.size(); for (auto& vv : v) put(vv.j, 8);
    size_t wOfs   = bin.size(); for (auto& vv : v) put(vv.w, 16);
    size_t idxOfs = bin.size(); put(idx.data(), idx.size()*2); align4();
    size_t ibmOfs = bin.size();
    float ibm[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}; put(ibm, 64);
    // Shared key times [0, 0.5, 1].
    size_t timeOfs = bin.size(); float times[3] = {0.0f, 0.5f, 1.0f}; put(times, 12);
    // BROKEN translation: rest hips Y is 0.9 but the clip buries them ~1 m down
    // AND bounces 0.2 m — the exact live-bug signature.
    size_t trnOfs = bin.size();
    float trns[9] = { 0,-0.05f,0,  0,-0.25f,0,  0,-0.05f,0 }; put(trns, 36);
    // Rotation: identity -> 30 deg about Z -> identity (sway that must SURVIVE
    // the root-Y lock).
    size_t rotOfs = bin.size();
    const float s15 = std::sin(0.2617994f), c15 = std::cos(0.2617994f);
    float quats[12] = { 0,0,0,1,  0,0,s15,c15,  0,0,0,1 }; put(quats, 48);

    char buf[2048];
    std::string j = "{\"asset\":{\"version\":\"2.0\"},";
    j += "\"scene\":0,\"scenes\":[{\"nodes\":[0,1]}],";
    j += "\"nodes\":[{\"mesh\":0,\"skin\":0},"
         "{\"name\":\"Armature\",\"children\":[2]},"
         "{\"name\":\"Hips\",\"translation\":[0,0.9,0]}],";
    j += "\"meshes\":[{\"primitives\":[{\"attributes\":{";
    j += "\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2,\"JOINTS_0\":3,\"WEIGHTS_0\":4},";
    j += "\"indices\":5}]}],";
    j += "\"skins\":[{\"joints\":[2],\"inverseBindMatrices\":6}],";
    j += "\"animations\":[{\"name\":\"BuriedWalk\",\"channels\":["
         "{\"sampler\":0,\"target\":{\"node\":2,\"path\":\"translation\"}},"
         "{\"sampler\":1,\"target\":{\"node\":2,\"path\":\"rotation\"}}],";
    j += "\"samplers\":[{\"input\":7,\"output\":8,\"interpolation\":\"LINEAR\"},"
         "{\"input\":7,\"output\":9,\"interpolation\":\"LINEAR\"}]}],";
    std::snprintf(buf, sizeof buf,
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":%zu,\"type\":\"VEC3\",\"min\":[-0.1,0,0],\"max\":[0.1,1.8,0]},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":%zu,\"type\":\"VEC3\"},"
        "{\"bufferView\":2,\"componentType\":5126,\"count\":%zu,\"type\":\"VEC2\"},"
        "{\"bufferView\":3,\"componentType\":5123,\"count\":%zu,\"type\":\"VEC4\"},"
        "{\"bufferView\":4,\"componentType\":5126,\"count\":%zu,\"type\":\"VEC4\"},"
        "{\"bufferView\":5,\"componentType\":5123,\"count\":%zu,\"type\":\"SCALAR\"},"
        "{\"bufferView\":6,\"componentType\":5126,\"count\":1,\"type\":\"MAT4\"},"
        "{\"bufferView\":7,\"componentType\":5126,\"count\":3,\"type\":\"SCALAR\",\"min\":[0],\"max\":[1]},"
        "{\"bufferView\":8,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":9,\"componentType\":5126,\"count\":3,\"type\":\"VEC4\"}],",
        nv, nv, nv, nv, nv, idx.size());
    j += buf;
    std::snprintf(buf, sizeof buf,
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu},"
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu},"
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu},"
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu},"
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu},"
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu},"
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":64},"
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":12},"
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":48}],",
        posOfs, nv*12, nrmOfs, nv*12, uvOfs, nv*8, jOfs, nv*8, wOfs, nv*16,
        idxOfs, idx.size()*2, ibmOfs, timeOfs, trnOfs, rotOfs);
    j += buf;
    std::snprintf(buf, sizeof buf, "\"buffers\":[{\"byteLength\":%zu}]}", bin.size());
    j += buf;

    while (j.size() % 4 != 0) j.push_back(' ');
    std::vector<uint8_t> binPad = bin;
    while (binPad.size() % 4 != 0) binPad.push_back(0);
    std::vector<uint8_t> glb;
    const uint32_t total = 12 + 8 + uint32_t(j.size()) + 8 + uint32_t(binPad.size());
    appendU32(glb, 0x46546C67); appendU32(glb, 2); appendU32(glb, total);
    appendU32(glb, uint32_t(j.size())); appendU32(glb, 0x4E4F534A);
    glb.insert(glb.end(), j.begin(), j.end());
    appendU32(glb, uint32_t(binPad.size())); appendU32(glb, 0x004E4942);
    glb.insert(glb.end(), binPad.begin(), binPad.end());
    return glb;
}

} // namespace

bool runRescueSelfTest() {
    g_rpass = g_rfail = 0;

    std::unique_ptr<x3::phys::IPhysicsWorld> physics(x3::phys::createPhysicsWorld());
    physics->init();

    HeadlessDevice device;
    Scene scene;
    RescueSystem rescue;

    // Three wards spread apart so range-tests are unambiguous.
    const x3::phys::Vec3 wA{  0.0f, 0.4f, 0.0f };
    const x3::phys::Vec3 wB{ 20.0f, 0.4f, 0.0f };
    const x3::phys::Vec3 wC{ 40.0f, 0.4f, 0.0f };
    // Short timer so the expiry test runs fast (the 5-min default is a build param).
    rescue.build(scene, device, *physics, riggedGlbRoot(), wA, wB, wC, /*timer*/2.0f);
    rcheck(rescue.victimCount() == 3, "R0 three victims built");

    // ---- R6 (playtest-fix): the timers are GATED — NO countdown / NO transform
    // before activate(). Right after build the hub is NOT reached (default off), so
    // ticking for well past the 2 s timer must NOT count any victim down or spawn a
    // boss. This is the bug we fixed: timers must not run from load. ----
    {
        const bool gatedOff = !rescue.hubReached() && !rescue.active();
        const float t0 = rescue.victim(0).timeLeft();
        const x3::phys::Vec3 farPlayer{ 999.0f, 0.4f, 999.0f };  // nowhere near a ward
        for (int i = 0; i < 240; ++i) rescue.tick(1.0f / 60.0f, scene, *physics, farPlayer); // 4 s
        const bool noCountdown = std::abs(rescue.victim(0).timeLeft() - t0) < 1e-3f;
        const bool noBoss      = rescue.bosses().count() == 0 && rescue.expiredCount() == 0;
        rcheck(gatedOff && noCountdown && noBoss,
               "R6 no countdown / no transform before activate() (timers gated at load)");
    }

    // Now reach the hub via the explicit activate() path: the timers start here.
    rescue.activate();
    rcheck(rescue.hubReached() && rescue.active(), "R7 activate() starts the rescue clocks");

    // R4a: cannot rescue an OUT-OF-RANGE captive (player far from ward A).
    {
        bool got = rescue.tryRescue(x3::phys::Vec3{ 0.0f, 0.4f, 20.0f }, kRescueReach);
        rcheck(!got, "R4 out-of-range rescue rejected");
    }

    // R1: rescuing an IN-RANGE captive (Aria) yields a companion.
    {
        bool got = rescue.tryRescue(x3::phys::Vec3{ wA.x + 1.0f, 0.4f, wA.z }, kRescueReach);
        rcheck(got && rescue.rescuedCount() == 1, "R1 in-range rescue -> companion");
    }

    // R2: the companion follows the player. Tick toward a far player; the companion
    // should close the XZ distance toward the standoff ring.
    {
        const RescueVictim& aria = rescue.victim(0);
        const x3::phys::Vec3 start = aria.pos();
        const x3::phys::Vec3 player{ wA.x + 15.0f, 0.4f, wA.z };
        for (int i = 0; i < 120; ++i) rescue.tick(1.0f / 60.0f, scene, *physics, player);
        const x3::phys::Vec3 end = aria.pos();
        const float d0 = std::abs(player.x - start.x);
        const float d1 = std::abs(player.x - end.x);
        rcheck(d1 < d0 - 1.0f, "R2 companion follows the player");
    }

    // R3: a captive whose timer expires transforms into a boss. Keisha (ward B) is
    // never rescued; tick past the 2 s timer with the player far away.
    {
        const uint32_t bossesBefore = rescue.bosses().count();
        const x3::phys::Vec3 player{ wA.x + 15.0f, 0.4f, wA.z };
        for (int i = 0; i < 240; ++i) rescue.tick(1.0f / 60.0f, scene, *physics, player); // 4 s
        const bool grew = rescue.bosses().count() > bossesBefore;
        const bool expired = rescue.expiredCount() >= 1;
        bool bossType = false;
        if (rescue.bosses().count() > 0)
            bossType = (rescue.bosses().at(0).type() == MonsterType::Boss);
        rcheck(grew && expired && bossType, "R3 expired victim transforms into a Boss");
    }

    // R4b: a now-EXPIRED victim cannot be rescued.
    {
        // Keisha is at ward B and now expired; standing on her spot must NOT rescue.
        bool got = rescue.tryRescue(x3::phys::Vec3{ wB.x, 0.4f, wB.z }, kRescueReach);
        rcheck(!got, "R4 expired victim not rescuable");
    }

    // R5: serialize/deserialize round-trips the lifecycle + timers + hub flag.
    {
        RescueSystem::SaveState snap = rescue.serialize();
        rcheck(snap.hubReached && snap.victims.size() == 3, "R5a serialize captured state");
        // Mutate Emily (still a captive) then restore from the snapshot.
        const float emilyBefore = rescue.victim(2).timeLeft();
        rescue.deserialize(snap);
        const float emilyAfter = rescue.victim(2).timeLeft();
        const bool ariaCompanion = rescue.victim(0).companion();   // restored companion
        const bool keishaExpired = rescue.victim(1).expired();     // restored expired
        rcheck(std::abs(emilyBefore - emilyAfter) < 1e-3f && ariaCompanion && keishaExpired,
               "R5 deserialize restores lifecycle + timers");
    }

    // ---- R8 (BUG #48): a girl with a skinnable model must actually ANIMATE —
    // her joint palette must DIFFER between two clip times (a frozen bind-pose
    // mannequin would yield an identical palette). If NO victim's model is
    // skinnable on this checkout (asset variance), the assert is vacuously skipped
    // (still PASS) — the per-frame drive is wired regardless (proven green above). -
    {
        const RescueVictim* animed = nullptr;
        for (uint32_t i = 0; i < rescue.victimCount(); ++i)
            if (rescue.victim(i).animActive()) { animed = &rescue.victim(i); break; }
        if (animed) {
            std::vector<float> pa, pb;
            const uint32_t ja = animed->skinner().computePalette(animed->model(), 0, 0.00f, pa);
            const uint32_t jb = animed->skinner().computePalette(animed->model(), 0, 0.50f, pb);
            bool differs = (ja > 0 && ja == jb && pa.size() == pb.size());
            if (differs) {
                bool any = false;
                for (size_t k = 0; k < pa.size(); ++k)
                    if (std::abs(pa[k] - pb[k]) > 1e-4f) { any = true; break; }
                differs = any;
            }
            rcheck(differs, "R8 animated victim's joint palette changes over time (not frozen)");
        } else {
            rcheck(true, "R8 (no skinnable victim model on this checkout — drive still wired)");
        }
    }

    // ---- R8b (BURIED-GIRL FIX): a victim's ROOT (hips) must stay AT SPAWN HEIGHT.
    // Her world Y is physics-owned (m_pos); with the root-Y lock enabled in build()
    // the hips' model-space global Y must stay within +-0.05 m of its rest Y at
    // t = 0 / 0.25 / 0.5 / 0.75 of EVERY clip (world Y = m_pos.y + model Y, scale 1,
    // so the model-space bound IS the world-space bound). This is the gate that
    // catches the "half-buried + bouncing" class no matter which clip is playing. --
    {
        const RescueVictim* animed = nullptr;
        for (uint32_t i = 0; i < rescue.victimCount(); ++i)
            if (rescue.victim(i).animActive()) { animed = &rescue.victim(i); break; }
        if (animed) {
            const x3::anim::Skinner& sk = animed->skinner();
            const int rootNode = sk.rootYLockNode();
            const float restY  = sk.rootYLockRestY();
            bool lockOn = sk.rootYLock() && rootNode >= 0;
            float maxDev = 0.0f;
            std::vector<float> gl;
            for (uint32_t c = 0; lockOn && c < sk.clipCount(); ++c) {
                const float dur = sk.clipDuration(c);
                for (float u : { 0.0f, 0.25f, 0.5f, 0.75f }) {
                    if (sk.currentGlobals(animed->model(), c, dur * u, gl) == 0) continue;
                    const float y = gl[(size_t)rootNode * 16 + 13];
                    const float dev = std::abs(y - restY);
                    if (dev > maxDev) maxDev = dev;
                }
            }
            rcheck(lockOn && maxDev <= 0.05f,
                   "R8b root-Y lock holds the victim's hips at spawn height (+-0.05 m, all clips)");
            x3::logInfo("[rescue-test] R8b max hips-Y deviation " + std::to_string(maxDev) + " m");
        } else {
            rcheck(true, "R8b (no skinnable victim model on this checkout)");
        }
    }

    // ---- R8c NEGATIVE CONTROL: a synthetic clip with a BROKEN baked root Y (hips
    // rest +0.9, animated to -0.05..-0.25 — a metre buried, bouncing 0.2 m). With
    // the lock DISABLED the R8b gate must FAIL (deviation ~0.95 m >> 0.05) — proving
    // the gate detects the live-bug class; with the lock ENABLED the hips must pin
    // to rest while the clip's ROTATION channel still animates (sway preserved). ----
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path tmp = fs::temp_directory_path() / "x3native_rescuetest";
        fs::remove_all(tmp, ec);
        fs::create_directories(tmp, ec);
        {
            std::vector<uint8_t> glb = makeBuriedRootGlb();
            std::ofstream f(tmp / "buried.glb", std::ios::binary);
            f.write((const char*)glb.data(), (std::streamsize)glb.size());
        }
        std::unique_ptr<x3::asset::IAssetSource> bsrc(x3::asset::createAssetSource());
        bsrc->mountDir(tmp.string(), 0);
        std::unique_ptr<x3::asset::IModelLoader> bloader(
            x3::asset::createModelLoader(nullptr, bsrc.get()));
        x3::asset::Model bmodel = bloader->load("buried.glb");
        x3::anim::Skinner bsk;
        if (bmodel.ok && bsk.bind(bmodel)) {
            const int root = bsk.rootYLockNode();
            std::vector<float> gl;
            auto hipsY = [&](float t) -> float {
                if (bsk.currentGlobals(bmodel, 0, t, gl) == 0 || root < 0) return 1e9f;
                return gl[(size_t)root * 16 + 13];
            };
            // Lock OFF (the bind default): the gate MUST detect the burial + bounce.
            const float y0off = hipsY(0.0f), yMidOff = hipsY(0.5f);
            const bool detects = std::abs(y0off - 0.9f) > 0.5f &&        // ~1 m buried
                                 std::abs(yMidOff - y0off) > 0.1f;       // bouncing
            rcheck(root >= 0 && detects,
                   "R8c NEGATIVE control: lock disabled -> the gate FAILS the buried clip");
            // Lock ON: hips pinned to rest at both times...
            bsk.setRootYLock(true);
            const float y0on = hipsY(0.0f), yMidOn = hipsY(0.5f);
            const bool pinned = std::abs(y0on - 0.9f) < 1e-3f &&
                                std::abs(yMidOn - 0.9f) < 1e-3f;
            // ...while the rotation channel still animates (the sway contract).
            std::vector<float> pa, pb;
            const uint32_t ja = bsk.computePalette(bmodel, 0, 0.0f, pa);
            const uint32_t jb = bsk.computePalette(bmodel, 0, 0.5f, pb);
            bool sways = (ja > 0 && ja == jb);
            if (sways) {
                bool any = false;
                for (size_t k = 0; k < pa.size(); ++k)
                    if (std::abs(pa[k] - pb[k]) > 1e-4f) { any = true; break; }
                sways = any;
            }
            rcheck(pinned, "R8c lock enabled -> hips pinned to rest height (no sink, no bounce)");
            rcheck(sways,  "R8c lock enabled -> rotations still animate (idle sway preserved)");
        } else {
            rcheck(false, "R8c synthetic buried-root GLB failed to load/bind");
            rcheck(false, "R8c (lock check skipped)");
            rcheck(false, "R8c (sway check skipped)");
        }
        bloader->unload(bmodel);
        fs::remove_all(tmp, ec);
    }

    // ---- R9 (W4-1): EXTRACTION — a Companion that reaches the extraction point
    // leaves the level: state flips to Extracted, extractedThisFrame reports its
    // index, and the girl stops drawing/following. Uses a fresh system so the
    // mixed states from the earlier asserts can't mask the transition.
    {
        RescueSystem ex;
        Scene exScene;
        ex.build(exScene, device, *physics, riggedGlbRoot(), wA, wB, wC, /*timer*/60.0f);
        ex.activate();
        const x3::phys::Vec3 player{ 0.5f, 0.4f, 0.5f };        // in reach of ward A
        rcheck(ex.tryRescue(player, kRescueReach), "R9a companion rescued for extraction");
        // Extraction point right where the companion will be after one follow tick.
        ex.setExtractionPoint(x3::phys::Vec3{ ex.victim(0).pos().x, 0.4f,
                                              ex.victim(0).pos().z }, 5.0f);
        ex.tick(0.1f, exScene, *physics, player);
        rcheck(ex.victim(0).extracted(), "R9b companion at the point EXTRACTS");
        rcheck(ex.extractedThisFrame() == 0, "R9c extractedThisFrame reports the index");
        ex.tick(0.1f, exScene, *physics, player);
        rcheck(ex.extractedThisFrame() == UINT32_MAX && ex.victim(0).extracted(),
               "R9d extraction is one-shot + terminal");
    }

    // =======================================================================
    // W5-2 — the INTERRUPT WINDOW (R10-R13). Fresh system per case; "attackers"
    // are Tag::Enemy entities with live physics bodies (a kill = drop the body,
    // exactly what monster.cpp does), so aliveAttackersNear sees scene truth.
    // =======================================================================
    auto spawnFakeAttacker = [&](Scene& sc, x3::phys::IPhysicsWorld& ph,
                                 const x3::phys::Vec3& at) -> uint32_t {
        Entity en;
        en.tag = (uint32_t)Tag::Monster;
        en.visible = true;
        en.body = ph.addBox(x3::phys::Vec3{0.4f,0.9f,0.4f}, at, 0.0f, x3::phys::Layer::Enemy);
        en.transform[12] = at.x; en.transform[13] = at.y; en.transform[14] = at.z;
        return sc.add(en);
    };
    auto killFakeAttacker = [&](Scene& sc, x3::phys::IPhysicsWorld& ph, uint32_t e) {
        Entity& en = sc.get(e);
        if (en.body.valid()) ph.removeBody(en.body);
        en.body = x3::phys::BodyId{};   // the monster kill path: body link dropped
    };
    const x3::phys::Vec3 farAway{ 999.0f, 0.4f, 999.0f };

    // R10: approach ARMS the assault (the tell), close approach OPENS the window;
    // killing the attacker inside the clean threshold resolves CLEAN (no tint change).
    {
        RescueSystem rs; Scene sc;
        rs.build(sc, device, *physics, riggedGlbRoot(), wA, wB, wC, /*timer*/300.0f);
        rs.activate();
        uint32_t atk = spawnFakeAttacker(sc, *physics, x3::phys::Vec3{ wA.x + 1.2f, 0.4f, wA.z });
        rs.tick(1.0f/60.0f, sc, *physics, farAway);
        rcheck(!rs.victim(0).assaultArmed(), "R10a far player: not armed");
        rs.tick(1.0f/60.0f, sc, *physics, x3::phys::Vec3{ wA.x + 10.0f, 0.4f, wA.z });
        rcheck(rs.victim(0).assaultArmed() && rs.victim(0).tellActive() &&
               !rs.victim(0).assaultActive(), "R10b approach arms the tell");
        const x3::phys::Vec3 close{ wA.x + 3.0f, 0.4f, wA.z };
        rs.tick(1.0f/60.0f, sc, *physics, close);
        rcheck(rs.victim(0).assaultActive() && rs.victim(0).windowFrac() > 0.9f,
               "R10c burst-in opens the window");
        for (int i = 0; i < 120; ++i) rs.tick(1.0f/60.0f, sc, *physics, close);  // 2 s pass
        rcheck(rs.victim(0).assaultActive() && rs.victim(0).tier() == RescueTier::None,
               "R10d window runs while the attacker lives");
        killFakeAttacker(sc, *physics, atk);
        rs.tick(1.0f/60.0f, sc, *physics, close);
        rcheck(rs.victim(0).tier() == RescueTier::Clean && !rs.victim(0).assaultActive(),
               "R10e fast kill resolves CLEAN");
        rcheck(rs.tierResolvedThisFrame() == 0, "R10f tierResolvedThisFrame reports it");
    }

    // R11: a LATE kill (past the clean threshold, inside the cutoff) resolves WOUNDED.
    {
        RescueSystem rs; Scene sc;
        rs.build(sc, device, *physics, riggedGlbRoot(), wA, wB, wC, 300.0f);
        rs.configureAssault(0, /*clean*/0.5f, /*cutoff*/30.0f);   // tight clean for the test
        rs.activate();
        uint32_t atk = spawnFakeAttacker(sc, *physics, x3::phys::Vec3{ wA.x + 1.2f, 0.4f, wA.z });
        const x3::phys::Vec3 close{ wA.x + 3.0f, 0.4f, wA.z };
        for (int i = 0; i < 90; ++i) rs.tick(1.0f/60.0f, sc, *physics, close);   // 1.5 s > clean
        killFakeAttacker(sc, *physics, atk);
        rs.tick(1.0f/60.0f, sc, *physics, close);
        rcheck(rs.victim(0).tier() == RescueTier::Wounded, "R11a late kill resolves WOUNDED");
        rcheck(rs.tierResolvedThisFrame() == 0, "R11b wounded resolution reported to host");
    }

    // R12: the HARD CUTOFF with attackers alive = LOST — immediate expiry + boss,
    // exactly like the 5-min timer (shared expire path).
    {
        RescueSystem rs; Scene sc;
        rs.build(sc, device, *physics, riggedGlbRoot(), wA, wB, wC, 300.0f);
        rs.configureAssault(0, 0.3f, /*cutoff*/0.8f);
        rs.activate();
        spawnFakeAttacker(sc, *physics, x3::phys::Vec3{ wA.x + 1.2f, 0.4f, wA.z });
        const x3::phys::Vec3 close{ wA.x + 3.0f, 0.4f, wA.z };
        const uint32_t bossesBefore = rs.bosses().count();
        for (int i = 0; i < 90; ++i) rs.tick(1.0f/60.0f, sc, *physics, close);   // 1.5 s > cutoff
        rcheck(rs.victim(0).expired() && rs.victim(0).tier() == RescueTier::Lost,
               "R12a cutoff with attackers alive = LOST (expired)");
        rcheck(rs.bosses().count() > bossesBefore, "R12b LOST spawns her boss");
    }

    // R13: window state does NOT break rescue/extraction — a Clean-resolved captive
    // rescues into a companion exactly as before (legacy flow preserved).
    {
        RescueSystem rs; Scene sc;
        rs.build(sc, device, *physics, riggedGlbRoot(), wA, wB, wC, 300.0f);
        rs.activate();
        uint32_t atk = spawnFakeAttacker(sc, *physics, x3::phys::Vec3{ wA.x + 1.2f, 0.4f, wA.z });
        const x3::phys::Vec3 close{ wA.x + 1.0f, 0.4f, wA.z };
        rs.tick(1.0f/60.0f, sc, *physics, close);
        killFakeAttacker(sc, *physics, atk);
        rs.tick(1.0f/60.0f, sc, *physics, close);
        rcheck(rs.victim(0).tier() == RescueTier::Clean && rs.tryRescue(close, kRescueReach) &&
               rs.victim(0).companion(), "R13 clean tier -> rescue -> companion (flow intact)");
    }

    physics->shutdown();
    x3::logInfo("[rescue-test] " + std::to_string(g_rpass) + " passed, " +
                std::to_string(g_rfail) + " failed");
    return g_rfail == 0;
}

} // namespace x3::game
