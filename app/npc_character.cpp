// NPC CHARACTER — animated citizen bodies for the living city. See npc_character.h.
//
// Clean-room: built from Scene + the engine interfaces (IRenderDevice /
// IPhysicsWorld / IModelLoader / IAssetSource) + x3::anim::Skinner + x3::game::
// RagdollSkin + x3::phys::IRagdoll only. No purchased C# / id Tech / RBDOOM source
// consulted. Mirrors monster.cpp / rescue.cpp deliberately (the same body machinery,
// none of the combat/rescue lifecycle) so this branch folds cleanly with them.
#include "npc_character.h"
#include "mesh_prims.h"
#include "headless_device.h"
#include "asset_root.h"

#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace x3::game {

namespace {

constexpr float kPi = 3.14159265358979323846f;

// A standing citizen is fitted to ~1.8 m; this is the target the skeleton-fit maps to.
constexpr float kCitizenTargetHeight = 1.8f;
// Collision box half-extents (a standing humanoid).
constexpr x3::phys::Vec3 kCitizenHalf{ 0.4f, 0.9f, 0.4f };
// Pelvis height above the feet in the canonical humanoid ragdoll rig.
constexpr float kRigPelvisH = 0.9f;
// Default locomotion pace floors for the Walk / Run|Flee states (m/s).
constexpr float kWalkPace = 1.2f;
constexpr float kRunPace  = 3.6f;

// ---- The archetype table: shared rigged-humanoid bodies + a look. Six visually
// distinct citizens mixing three animated bodies with tint + size variety. The
// *_anim.glb carry the shared Idle/Walk/Run(/Talk) clip set (retarget_from_jake.py);
// clip-less bodies (a clean checkout) degrade to a static idle. ----
const NpcArchetypeDef kArchetypes[] = {
    { "marcus_webb_anim.glb",    1.00f, { 1.00f, 0.96f, 0.90f, 1.0f }, "MaleCasual"    },
    { "AnnaCasual_anim.glb",     1.00f, { 1.00f, 1.00f, 1.00f, 1.0f }, "FemaleCasual"  },
    { "chief_martinez_anim.glb", 1.00f, { 0.85f, 0.90f, 1.00f, 1.0f }, "Worker"        },
    { "marcus_webb_anim.glb",    1.05f, { 0.90f, 0.84f, 0.78f, 1.0f }, "MaleCasual-B"  },
    { "AnnaCasual_anim.glb",     0.97f, { 1.00f, 0.90f, 0.92f, 1.0f }, "FemaleCasual-B"},
    { "chief_martinez_anim.glb", 0.98f, { 0.95f, 0.95f, 0.86f, 1.0f }, "Worker-B"      },
};

// Column-major 4x4 from a 3x3 basis (columns bx,by,bz), uniform scale s, translation
// t. Identical to monster.cpp / rescue.cpp composeTRS.
void composeTRS(float m[16],
                const x3::phys::Vec3& bx, const x3::phys::Vec3& by, const x3::phys::Vec3& bz,
                float s, const x3::phys::Vec3& t) {
    m[0]  = bx.x * s; m[1]  = bx.y * s; m[2]  = bx.z * s; m[3]  = 0.0f;
    m[4]  = by.x * s; m[5]  = by.y * s; m[6]  = by.z * s; m[7]  = 0.0f;
    m[8]  = bz.x * s; m[9]  = bz.y * s; m[10] = bz.z * s; m[11] = 0.0f;
    m[12] = t.x;      m[13] = t.y;      m[14] = t.z;      m[15] = 1.0f;
}

// Facing law (CONVENTIONS.md): point local -Z along (dirX,dirZ) -> yaw=atan2(-dirX,-dirZ).
float headingToFace(float dirX, float dirZ) {
    if (dirX * dirX + dirZ * dirZ < 1e-12f) return 0.0f;
    return std::atan2(-dirX, -dirZ);
}

// Inverse of a rotation*uniform-scale+translation affine 4x4 (the gameplay draw
// matrix). out=identity on a degenerate matrix. Identical to rescue.cpp.
bool invertAffineUniform(const float m[16], float out[16]) {
    for (int i = 0; i < 16; ++i) out[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    const float s2 = m[0]*m[0] + m[1]*m[1] + m[2]*m[2];
    if (s2 < 1e-12f) return false;
    const float invS2 = 1.0f / s2;
    out[0] = m[0]*invS2; out[1] = m[4]*invS2; out[2] = m[8]*invS2;
    out[4] = m[1]*invS2; out[5] = m[5]*invS2; out[6] = m[9]*invS2;
    out[8] = m[2]*invS2; out[9] = m[6]*invS2; out[10]= m[10]*invS2;
    const float tx = m[12], ty = m[13], tz = m[14];
    out[12] = -(out[0]*tx + out[4]*ty + out[8]*tz);
    out[13] = -(out[1]*tx + out[5]*ty + out[9]*tz);
    out[14] = -(out[2]*tx + out[6]*ty + out[10]*tz);
    out[15] = 1.0f;
    return true;
}

} // namespace

const char* npcAnimStateName(NpcAnimState s) {
    switch (s) {
        case NpcAnimState::Idle:     return "Idle";
        case NpcAnimState::Walk:     return "Walk";
        case NpcAnimState::Run:      return "Run";
        case NpcAnimState::Flee:     return "Flee";
        case NpcAnimState::Talk:     return "Talk";
        case NpcAnimState::HitReact: return "HitReact";
        case NpcAnimState::Death:    return "Death";
    }
    return "?";
}

uint32_t npcArchetypeCount() { return (uint32_t)(sizeof(kArchetypes) / sizeof(kArchetypes[0])); }
const NpcArchetypeDef& npcArchetype(uint32_t id) {
    return kArchetypes[id < npcArchetypeCount() ? id : 0];
}

// ===========================================================================
// NpcCharacter
// ===========================================================================
void NpcCharacter::spawn(uint32_t archetypeId, Scene& scene, x3::rhi::IRenderDevice& device,
                         x3::phys::IPhysicsWorld& physics, std::string_view modelDir,
                         const x3::phys::Vec3& pos, float yaw) {
    if (m_spawned) return;   // idempotent
    m_archetype = (archetypeId < npcArchetypeCount()) ? archetypeId : 0;
    const NpcArchetypeDef& def = npcArchetype(m_archetype);
    m_pos = pos;
    m_prevMovePos = pos;
    m_yaw = yaw;
    for (int i = 0; i < 4; ++i) m_tint[i] = def.tint[i];
    for (int i = 0; i < 16; ++i) m_modelFixup[i] = (i % 5 == 0) ? 1.0f : 0.0f;

    // ---- Ground raycast (per-instance settle reference — NOT a hardwired y=0). Cast
    // DOWN from just above the spawn to find the static floor beneath THIS citizen; the
    // ragdoll then falls onto real geometry wherever the body stands. ----
    m_groundY = pos.y;
    {
        x3::phys::RayHit h = physics.rayCast(
            x3::phys::Vec3{ pos.x, pos.y + 2.0f, pos.z },
            x3::phys::Vec3{ 0.0f, -1.0f, 0.0f }, 8.0f, x3::phys::Layer::Static);
        if (h.hit) m_groundY = h.point.y;
    }

    // ---- Load the body GLB via a mounted loose-dir asset source (monster/rescue path). ----
    const std::string dir  = modelDir.empty() ? riggedGlbRoot() : std::string(modelDir);
    const std::string file = def.modelFile;
    m_device = &device;
    m_assets.reset(x3::asset::createAssetSource());
    bool mounted = m_assets->mountDir(dir, 0);
    if (mounted) {
        m_loader.reset(x3::asset::createModelLoader(&device, m_assets.get()));
        m_model = m_loader->load(file);
        if (m_model.ok)
            m_drawables = x3::asset::makeDrawables(m_model);
    } else {
        x3::logWarn("[npc] mountDir failed: " + dir);
    }

    if (!m_drawables.empty()) {
        m_usingReal = true;
        // ---- Bind the Skinner (breathe/walk). Mirrors MonsterSystem exactly. ----
        if (m_model.ok && m_skinner.bind(m_model)) {
            const bool gpuSkin = m_skinner.enableGpuSkinning(device, m_model);
            m_idleClip = m_skinner.findClip({ "idle", "stand", "breath", "loop" });
            m_walkClip = m_skinner.findClip({ "walk" });
            m_runClip  = m_skinner.findClip({ "run", "sprint", "jog" });
            m_talkClip = m_skinner.findClip({ "talk", "speak", "gesture", "converse" });
            if (m_walkClip < 0) m_walkClip = m_skinner.findClip({ "move", "jog", "run" });
            if (m_idleClip < 0) m_idleClip = 0;
            m_animActive   = (m_idleClip >= 0);
            m_useLocoBlend = m_animActive && (m_walkClip >= 0 || m_runClip >= 0);
            if (m_useLocoBlend)
                m_skinner.setLocomotionClips(m_idleClip, m_walkClip, m_runClip, 0.2f, 2.0f);
            // Per-body phase so a crowd doesn't breathe in lockstep (spread by archetype
            // id + a hash of the position so identical archetypes still desync).
            const uint32_t h = (uint32_t)((int)(pos.x * 7.0f) * 73856093) ^
                               (uint32_t)((int)(pos.z * 7.0f) * 19349663) ^ (m_archetype * 2654435761u);
            m_phaseOffset = (float)(h & 1023u) / 1023.0f * 1.3f;
            m_animTime    = m_phaseOffset;
            // Fit the scale to ~1.8 m BEFORE the first pose so the collision box + draw match.
            fitScale();
            // Prime frame 0 into the idle pose so the first drawn frame isn't bind/T-pose.
            if (m_animActive && m_device) {
                if (m_useLocoBlend) {
                    m_skinner.setLocomotionSpeed(0.0f);
                    m_skinner.applyLocomotion(m_model, *m_device, m_phaseOffset);
                } else {
                    m_skinner.apply(m_model, *m_device, (uint32_t)m_idleClip, m_animTime);
                }
            }
            x3::logInfo(std::string("[npc] ") + def.label + " (" + file + ") animated (" +
                        (gpuSkin ? "GPU" : "CPU") + " skin) idle=" + std::to_string(m_idleClip) +
                        " walk=" + std::to_string(m_walkClip) + " run=" + std::to_string(m_runClip) +
                        " talk=" + std::to_string(m_talkClip) + " loco=" + (m_useLocoBlend ? "1" : "0"));
        } else {
            x3::logInfo(std::string("[npc] ") + def.label + " (" + file +
                        ") not skinnable — static draw");
        }
        m_modelScale = (m_fittedScale > 0.0f ? m_fittedScale : 1.0f) * def.scaleMul;
    } else {
        // Fallback box so a citizen still exists (headless / clean checkout).
        m_usingReal  = false;
        m_modelScale = def.scaleMul;
        x3::logWarn(std::string("[npc] ") + file + " load failed; using fallback box");
        x3::prims::PrimMesh geo = x3::prims::makeBox(kCitizenHalf.x, kCitizenHalf.y,
                                                     kCitizenHalf.z, 0.0f, 0.0f, 0.0f, 1.0f);
        x3::rhi::MeshHandle mesh = device.createMesh(
            geo.verts.data(), (uint32_t)geo.verts.size(),
            geo.index.data(), (uint32_t)geo.index.size());
        x3::asset::ModelDrawable d;
        d.meshId = mesh.id;
        d.baseColorTexId = 0;
        d.baseColorFactor[0] = 0.55f; d.baseColorFactor[1] = 0.55f;
        d.baseColorFactor[2] = 0.60f; d.baseColorFactor[3] = 1.0f;
        m_drawables.push_back(d);
    }

    // ---- Static-by-mass Enemy-layer collision box (hittable + movable via
    // setBodyPosition, the teleport idiom). Mass 0 keeps it put under gravity. ----
    m_body = physics.addBox(kCitizenHalf, m_pos, 0.0f, x3::phys::Layer::Enemy);

    Entity e;
    e.tag     = (uint32_t)Tag::Prop;   // bookkeeping only; draw() owns the render
    e.visible = true;
    e.body    = m_body;
    composeTRS(e.transform,
               x3::phys::Vec3{1,0,0}, x3::phys::Vec3{0,1,0}, x3::phys::Vec3{0,0,1},
               m_modelScale, m_pos);
    m_entity = scene.add(e);
    bakeTransform(scene);   // apply the facing flip immediately

    m_spawned   = true;
    m_despawned = false;
    x3::logInfo(std::string("[npc] spawned ") + def.label + " entity " +
                std::to_string(m_entity) + " at (" + std::to_string(pos.x) + "," +
                std::to_string(pos.y) + "," + std::to_string(pos.z) + ") groundY=" +
                std::to_string(m_groundY) + (m_usingReal ? " [GLB]" : " [box]"));
}

void NpcCharacter::fitScale() {
    if (!m_skinner.valid() || m_skinner.nodeCount() == 0) return;
    float minY = 1e30f, maxY = -1e30f;
    auto tryNamed = [&](const char* a, const char* b, float& lo, float& hi) -> bool {
        int na = m_skinner.resolveNodeByName(m_model, a);
        int nb = m_skinner.resolveNodeByName(m_model, b);
        float ma[16], mb[16];
        if (na >= 0 && nb >= 0 && m_skinner.boneGlobal((uint32_t)na, ma) &&
            m_skinner.boneGlobal((uint32_t)nb, mb)) {
            lo = std::min(ma[13], mb[13]); hi = std::max(ma[13], mb[13]); return true;
        }
        return false;
    };
    float lo = 0.0f, hi = 0.0f;
    bool named = tryNamed("mixamorigLeftToeBase", "mixamorigHead", lo, hi)
              || tryNamed("LeftToeBase", "Head", lo, hi)
              || tryNamed("toe", "head", lo, hi)
              || tryNamed("foot", "head", lo, hi);
    if (named) { minY = lo; maxY = hi; }
    else {
        float bm[16];
        for (uint32_t n = 0; n < m_skinner.nodeCount(); ++n)
            if (m_skinner.boneGlobal(n, bm)) { minY = std::min(minY, bm[13]); maxY = std::max(maxY, bm[13]); }
    }
    const float H = maxY - minY;
    const float Hfit = named ? H : H * 1.15f;
    if (Hfit > 0.2f && Hfit < 100.0f) {
        m_fittedScale = kCitizenTargetHeight / Hfit;
        x3::logInfo("[npc] skeleton-fit H=" + std::to_string(Hfit) + "m -> base scale=" +
                    std::to_string(m_fittedScale) + (named ? " (named)" : " (scan)"));
    }
}

void NpcCharacter::bakeTransform(Scene& scene) {
    if (m_entity == kNoLink || m_entity >= scene.size()) return;
    // The rigged GLBs are authored facing +Z; m_yaw is a -Z-forward heading — flip the
    // VISUAL yaw 180deg (m_yaw + logic unchanged), matching monster.cpp / rescue.cpp.
    const float ry = m_yaw + kPi;
    const float c = std::cos(ry), s = std::sin(ry);
    Entity& me = scene.get(m_entity);
    composeTRS(me.transform,
               x3::phys::Vec3{ c, 0.0f, -s },
               x3::phys::Vec3{ 0.0f, 1.0f, 0.0f },
               x3::phys::Vec3{ s, 0.0f, c },
               m_modelScale, m_pos);
}

void NpcCharacter::triggerRagdoll(const x3::phys::Vec3& impulse) {
    if (m_ragdolled || m_pendingRagdoll) return;   // idempotent
    m_pendingRagdoll = true;
    m_pendingImpulse = impulse;
}

void NpcCharacter::setAnimState(NpcAnimState state, float moveSpeed) {
    if (state == NpcAnimState::HitReact) {
        // A hit knocks the body over in the facing-back direction (physical hit react).
        const float fx = std::sin(m_yaw), fz = std::cos(m_yaw);
        triggerRagdoll(x3::phys::Vec3{ -fx * 3.0f, 0.5f, -fz * 3.0f });
        return;
    }
    if (state == NpcAnimState::Death) {
        const float fx = std::sin(m_yaw), fz = std::cos(m_yaw);
        triggerRagdoll(x3::phys::Vec3{ fx * 2.0f, -0.3f, fz * 2.0f });
        return;
    }
    m_animState = state;
    // Record an explicit locomotion floor for this state; update() blends with the
    // measured movement speed (whichever is larger drives the walk/run cycle).
    switch (state) {
        case NpcAnimState::Idle: m_locoSpeed = 0.0f; break;
        case NpcAnimState::Walk: m_locoSpeed = (moveSpeed > 0.0f) ? moveSpeed : kWalkPace; break;
        case NpcAnimState::Run:
        case NpcAnimState::Flee: m_locoSpeed = (moveSpeed > 0.0f) ? moveSpeed : kRunPace; break;
        case NpcAnimState::Talk: m_locoSpeed = 0.0f; break;
        default: break;
    }
}

void NpcCharacter::moveTo(Scene& scene, x3::phys::IPhysicsWorld& physics,
                          const x3::phys::Vec3& pos) {
    if (m_ragdolled) return;
    const float dx = pos.x - m_pos.x, dz = pos.z - m_pos.z;
    if (dx * dx + dz * dz > 1e-9f) m_yaw = headingToFace(dx, dz);   // face travel
    m_pos = pos;
    if (m_body.valid()) physics.setBodyPosition(m_body, m_pos);
    bakeTransform(scene);
}

void NpcCharacter::driveAnim(float dt) {
    if (!m_animActive || !m_device) return;
    // Talk: play the gesture clip regardless of movement (dialog / scan-card).
    if (m_animState == NpcAnimState::Talk && m_talkClip >= 0) {
        m_animTime += dt;
        m_skinner.apply(m_model, *m_device, (uint32_t)m_talkClip, m_animTime);
        return;
    }
    if (m_useLocoBlend) {
        m_skinner.setLocomotionSpeed(m_locoSpeed);
        m_skinner.applyLocomotion(m_model, *m_device, dt);
    } else {
        const bool hasWalk = (m_walkClip >= 0);
        const bool moving  = m_locoSpeed > 0.25f;
        const int  clip    = (hasWalk && moving) ? m_walkClip : m_idleClip;
        const float rate   = (!hasWalk && moving)
            ? (1.0f + std::min(m_locoSpeed, 4.0f) * 0.35f) : 1.0f;
        m_animTime += dt * rate;
        m_skinner.apply(m_model, *m_device, (uint32_t)clip, m_animTime);
    }
}

void NpcCharacter::update(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics) {
    if (!m_spawned || m_despawned) return;

    // A triggerRagdoll() was latched — build the ragdoll now (we hold the Scene + world).
    if (m_pendingRagdoll && !m_ragdolled) {
        m_pendingRagdoll = false;
        buildRagdoll(scene, physics, m_pendingImpulse);
    }

    // Ragdolled: the host already stepped the shared world — read the bones back out
    // and flop the skin to match (the entity transform is frozen at collapse).
    if (m_ragdolled) { driveSkinFromRagdoll(); return; }

    // Measure this frame's planar movement speed (moveTo drives citizens by teleport).
    // Ignore large snaps (teleports) so a catch-up jump doesn't read as a sprint.
    float measured = 0.0f;
    if (dt > 1e-5f) {
        const float mdx = m_pos.x - m_prevMovePos.x, mdz = m_pos.z - m_prevMovePos.z;
        measured = std::sqrt(mdx * mdx + mdz * mdz) / dt;
        if (measured > 20.0f) measured = 0.0f;
    }
    m_prevMovePos = m_pos;

    // The effective locomotion speed = max(explicit state floor, measured movement), so
    // a stationary "Flee" still animates a run AND a moved-but-Idle citizen walks.
    const float effective = std::max(m_locoSpeed, measured);
    // Talk holds its own path; otherwise drive locomotion at `effective`.
    if (m_animState != NpcAnimState::Talk) m_locoSpeed = effective;

    bakeTransform(scene);
    driveAnim(dt);
}

// ===========================================================================
// RAGDOLL (hit-react / death flop). SAME machinery as MonsterSystem::spawnDeathRagdoll
// / RescueVictim::ragdoll: build a Jolt humanoid ragdoll from the canonical rig,
// placed/yawed/scaled to match this body, seeded from the CURRENT animated pose (so
// the flop starts seamlessly), added to the SHARED world, and kicked by `impulse`.
// From then on update() reads the bones back and flops the skin. Settles on whatever
// static floor is under the body (per-instance — not hardwired y=0). Idempotent.
// ===========================================================================
void NpcCharacter::buildRagdoll(Scene& scene, x3::phys::IPhysicsWorld& physics,
                                const x3::phys::Vec3& impulse) {
    if (m_ragdolled || m_deathRagdoll) return;
    if (!m_animActive || !m_skinner.valid() || m_skinner.nodeCount() == 0) {
        // Unrigged / clip-less: can't drive a skinned ragdoll — mark it "down" via the
        // state but leave it standing (no regression; a box has no skeleton to flop).
        m_animState = NpcAnimState::Death;
        return;
    }

    // Bake the collapse-time transform ONCE, then FREEZE the entity: drop its body link
    // so Scene::update() stops overwriting the frozen collapse transform, and remove the
    // standing collision box so it can't fight the falling ragdoll bodies (mirrors
    // rescue.cpp). The skin globals carry the world placement from here on.
    bakeTransform(scene);
    if (m_entity != kNoLink && m_entity < scene.size())
        scene.get(m_entity).body = x3::phys::BodyId{};
    if (m_body.valid()) { physics.removeBody(m_body); m_body = x3::phys::BodyId{}; }

    // The frozen draw transform: model = T(pos) * R(yaw+pi) * S(scale); capture inv(model).
    const float ry = m_yaw + kPi;
    const float c = std::cos(ry), s = std::sin(ry);
    const float scale = m_modelScale;
    float model[16];
    composeTRS(model, x3::phys::Vec3{ c, 0.0f, -s }, x3::phys::Vec3{ 0.0f, 1.0f, 0.0f },
               x3::phys::Vec3{ s, 0.0f, c }, scale, m_pos);
    float drawXform[16];
    x3::asset::mulMat4(model, m_modelFixup, drawXform);
    if (!invertAffineUniform(drawXform, m_deathModelInv)) return;

    // Canonical humanoid rig in WORLD space, lifted so the FEET sit at m_pos (topples
    // onto the floor rather than half-buried).
    x3::phys::makeHumanoidRagdollBones(0.0f, m_ragdollBones);
    const uint32_t bn = (uint32_t)m_ragdollBones.size();
    if (bn == 0) { m_ragdollBones.clear(); return; }
    const x3::phys::Vec3 footPos{ m_pos.x, m_pos.y + kRigPelvisH * scale, m_pos.z };
    float place[16];
    composeTRS(place, x3::phys::Vec3{ c, 0.0f, -s }, x3::phys::Vec3{ 0.0f, 1.0f, 0.0f },
               x3::phys::Vec3{ s, 0.0f, c }, scale, footPos);
    for (uint32_t b = 0; b < bn; ++b) {
        float placed[16];
        x3::asset::mulMat4(place, m_ragdollBones[b].bindWorld, placed);
        std::memcpy(m_ragdollBones[b].bindWorld, placed, 16 * sizeof(float));
        m_ragdollBones[b].halfHeight *= scale;
        m_ragdollBones[b].radius     *= scale;
    }

    m_deathRagdoll.reset(x3::phys::createRagdoll(physics, m_ragdollBones.data(), bn));
    if (!m_deathRagdoll) { m_ragdollBones.clear(); return; }

    {
        std::vector<float> bind((size_t)bn * 16);
        for (uint32_t b = 0; b < bn; ++b)
            std::memcpy(&bind[b*16], m_ragdollBones[b].bindWorld, 16 * sizeof(float));
        m_deathRagdoll->addToWorld(true);
        m_deathRagdoll->setPoseWorld(bind.data());
    }

    // Seed the rigid bone->skin driver from the CURRENT animated pose (seamless flop).
    {
        const int poseClip = (m_idleClip >= 0) ? m_idleClip : (m_walkClip >= 0) ? m_walkClip : 0;
        std::vector<float> curGlobals;
        const uint32_t ng = m_skinner.currentGlobals(m_model, (uint32_t)poseClip, m_animTime, curGlobals);
        if (ng == m_skinner.nodeCount() && ng > 0) m_ragdollSkin.bindFromGlobals(m_model, curGlobals.data(), ng);
        else                                       m_ragdollSkin.bind(m_model);
    }

    m_ragWorldScratch.assign((size_t)bn * 16, 0.0f);
    m_ragPartInit.assign((size_t)bn * 16, 0.0f);
    m_deathRagdoll->getBoneWorldTransforms(m_ragWorldScratch.data());
    for (uint32_t b = 0; b < bn; ++b)
        x3::asset::mulMat4(m_deathModelInv, &m_ragWorldScratch[b*16], &m_ragPartInit[b*16]);
    m_ragdollSkin.mapToParts(m_ragPartInit.data(), bn);
    m_ragPartCur.assign((size_t)bn * 16, 0.0f);

    // Kick the collapse. A near-zero impulse -> a gentle forward topple.
    x3::phys::Vec3 kick = impulse;
    if (std::fabs(kick.x) + std::fabs(kick.y) + std::fabs(kick.z) < 1e-3f) {
        const float fx = std::sin(m_yaw), fz = std::cos(m_yaw);
        kick = x3::phys::Vec3{ fx * 2.0f, -0.4f, fz * 2.0f };
    }
    m_deathRagdoll->applyImpulseAll(kick);

    m_ragdolled = true;
    m_animState = NpcAnimState::Death;
    x3::logInfo("[npc] " + std::string(npcArchetype(m_archetype).label) +
                " COLLAPSED into a ragdoll (" + std::to_string(bn) + " bones)");
}

void NpcCharacter::driveSkinFromRagdoll() {
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

bool NpcCharacter::ragdollSettled() const {
    return m_deathRagdoll && !m_deathRagdoll->isActive();
}

float NpcCharacter::ragdollLowestY() const {
    if (!m_deathRagdoll) return m_pos.y;
    const uint32_t bn = m_deathRagdoll->boneCount();
    if (bn == 0) return m_pos.y;
    std::vector<float> w((size_t)bn * 16);
    m_deathRagdoll->getBoneWorldTransforms(w.data());
    float lo = 1e30f;
    for (uint32_t b = 0; b < bn; ++b) lo = std::min(lo, w[b*16+13]);
    return lo;
}

void NpcCharacter::draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                        const Scene& scene) const {
    if (m_despawned || m_entity == kNoLink || m_entity >= scene.size()) return;
    const Entity& e = scene.get(m_entity);
    if (!e.visible) return;
    for (const auto& d : m_drawables) {
        float color[4] = {
            d.baseColorFactor[0] * m_tint[0], d.baseColorFactor[1] * m_tint[1],
            d.baseColorFactor[2] * m_tint[2], d.baseColorFactor[3] * m_tint[3],
        };
        float mf[16], fin[16];
        x3::asset::mulMat4(e.transform, m_modelFixup, mf);
        x3::asset::mulMat4(mf, d.nodeTransform, fin);
        device.drawMesh(frame, x3::rhi::MeshHandle{ d.meshId },
                        x3::rhi::TextureHandle{ d.baseColorTexId }, color, fin);
    }
}

void NpcCharacter::despawn(Scene& scene, x3::phys::IPhysicsWorld& physics) {
    if (m_despawned) return;
    if (m_deathRagdoll) { m_deathRagdoll->removeFromWorld(); m_deathRagdoll.reset(); }
    m_ragdollBones.clear();
    m_ragdolled = false;
    if (m_body.valid()) { physics.removeBody(m_body); m_body = x3::phys::BodyId{}; }
    if (m_device && m_skinner.valid()) m_skinner.disableGpuSkinning(*m_device);
    if (m_entity != kNoLink && m_entity < scene.size()) {
        Entity& me = scene.get(m_entity);
        me.visible = false;
        me.body = x3::phys::BodyId{};
    }
    m_despawned = true;
    x3::logInfo("[npc] despawned " + std::string(npcArchetype(m_archetype).label) +
                " entity " + std::to_string(m_entity));
}

// ===========================================================================
// NpcCrowd
// ===========================================================================
uint32_t NpcCrowd::spawn(uint32_t archetypeId, Scene& scene, x3::rhi::IRenderDevice& device,
                         x3::phys::IPhysicsWorld& physics, std::string_view modelDir,
                         const x3::phys::Vec3& pos, float yaw) {
    auto body = std::make_unique<NpcCharacter>();
    body->spawn(archetypeId, scene, device, physics, modelDir, pos, yaw);
    m_bodies.push_back(std::move(body));
    return (uint32_t)m_bodies.size() - 1;
}

uint32_t NpcCrowd::spawnRing(uint32_t count, Scene& scene, x3::rhi::IRenderDevice& device,
                             x3::phys::IPhysicsWorld& physics, std::string_view modelDir,
                             float centerX, float groundY, float centerZ, float radius) {
    for (uint32_t i = 0; i < count; ++i) {
        const float a = (float)i / (float)std::max(1u, count) * 2.0f * kPi;
        const x3::phys::Vec3 p{ centerX + std::cos(a) * radius, groundY, centerZ + std::sin(a) * radius };
        // Face inward toward the center.
        const float yaw = headingToFace(centerX - p.x, centerZ - p.z);
        spawn(i % npcArchetypeCount(), scene, device, physics, modelDir, p, yaw);
    }
    return (uint32_t)m_bodies.size();
}

void NpcCrowd::update(float dt, Scene& scene, x3::phys::IPhysicsWorld& physics) {
    for (auto& b : m_bodies) b->update(dt, scene, physics);
}
void NpcCrowd::draw(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                    const Scene& scene) const {
    for (const auto& b : m_bodies) b->draw(device, frame, scene);
}
void NpcCrowd::despawnAll(Scene& scene, x3::phys::IPhysicsWorld& physics) {
    for (auto& b : m_bodies) b->despawn(scene, physics);
}

// ===========================================================================
// Headless self-test (--test-npcchar). No window / Vulkan.
// ===========================================================================
namespace {

int g_npass = 0, g_nfail = 0;
void ncheck(bool cond, const char* name) {
    if (cond) { ++g_npass; x3::logInfo(std::string("[npcchar-test] PASS ") + name); }
    else      { ++g_nfail; x3::logError(std::string("[npcchar-test] FAIL ") + name); }
}
using HeadlessDevice = x3::game::HeadlessRenderDevice;

// Mean absolute difference between two equal-length joint palettes.
float palDiff(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.empty() || a.size() != b.size()) return 0.0f;
    double s = 0.0; for (size_t i = 0; i < a.size(); ++i) s += std::fabs(a[i] - b[i]);
    return (float)(s / (double)a.size());
}

} // namespace

bool runNpcCharacterSelfTest() {
    g_npass = g_nfail = 0;
    const float dt = 1.0f / 60.0f;

    std::unique_ptr<x3::phys::IPhysicsWorld> physics(x3::phys::createPhysicsWorld());
    physics->init();

    // ---- A static floor at a NON-ZERO height (y=2.0) — the whole point: prove the
    // ragdoll settles on THIS floor (per-instance), never a hardwired y=0. ----
    const float kFloorY = 2.0f;
    {
        float v[] = { -50,kFloorY,-50,  50,kFloorY,-50,  50,kFloorY,50,  -50,kFloorY,50 };
        uint32_t idx[] = { 0,2,1, 0,3,2 };
        physics->addStaticMesh(v, 4, idx, 6);
    }
    physics->optimizeBroadphase();

    HeadlessDevice device;
    Scene scene;

    // ---- N1: spawn a citizen. It loads a rigged skinnable body when the rigged GLB is
    // present, else gracefully a static box (clean checkout) — both are a PASS. ----
    NpcCharacter npc;
    const x3::phys::Vec3 spawnPos{ 0.0f, kFloorY, 0.0f };
    npc.spawn(0, scene, device, *physics, riggedGlbRoot(), spawnPos, 0.0f);
    ncheck(npc.spawned() && npc.entity() != kNoLink, "N1 citizen spawned with an entity");
    const bool skinnable = npc.skinnable();
    // The ground raycast found the non-zero floor (per-instance ground reference).
    ncheck(std::fabs(npc.groundY() - kFloorY) < 0.5f,
           "N1b ground raycast found the non-zero floor (per-instance settle reference)");

    if (!skinnable) {
        // Clean checkout without the rigged GLB: assert the graceful static-box path and
        // that triggerRagdoll no-ops (nothing to flop) — still a PASS.
        npc.setAnimState(NpcAnimState::Walk, 1.2f);
        npc.moveTo(scene, *physics, x3::phys::Vec3{ 0.5f, kFloorY, 0.0f });
        npc.update(dt, scene, *physics);
        npc.triggerRagdoll(x3::phys::Vec3{ 2, 0, 0 });
        npc.update(dt, scene, *physics);
        ncheck(!npc.ragdolled(), "N0 unrigged citizen: graceful static (no skinned ragdoll)");
        npc.despawn(scene, *physics);
        ncheck(npc.despawned(), "N0b unrigged citizen despawns clean");
        physics->shutdown();
        x3::logInfo("[npcchar-test] " + std::to_string(g_npass) + " passed, " +
                    std::to_string(g_nfail) + " failed (unrigged fallback path)");
        return g_nfail == 0;
    }

    // ---- N2: anim STATES switch — Idle vs Walk yield DIFFERENT joint palettes (the
    // body actually animates, not frozen). ----
    npc.setAnimState(NpcAnimState::Idle);
    npc.update(dt, scene, *physics);
    npc.update(dt, scene, *physics);
    std::vector<float> idlePal = npc.skinner().lastPalette();

    // Drive a few walking frames (moved position -> measured speed -> walk blend).
    for (int i = 0; i < 20; ++i) {
        npc.setAnimState(NpcAnimState::Walk, 1.4f);
        npc.moveTo(scene, *physics, x3::phys::Vec3{ 0.02f * (i + 1), kFloorY, 0.0f });
        npc.update(dt, scene, *physics);
    }
    std::vector<float> walkPal = npc.skinner().lastPalette();
    ncheck(!idlePal.empty() && idlePal.size() == walkPal.size() &&
           palDiff(idlePal, walkPal) > 1e-4f,
           "N2 Idle vs Walk produce different palettes (body animates, not frozen)");

    // Talk resolves + drives when the rig has a Talk clip (AnnaCasual does; marcus may
    // not — either way driving Talk must not crash or freeze the palette to empty).
    npc.setAnimState(NpcAnimState::Talk);
    npc.update(dt, scene, *physics);
    ncheck(!npc.skinner().lastPalette().empty(), "N2b Talk state drives a valid pose");

    // ---- N3: ragdoll — triggerRagdoll spawns a live ragdoll that FALLS and SETTLES on
    // the NON-ZERO floor (lowest bone near kFloorY, NOT y=0, NOT floating, no NaN). ----
    npc.setAnimState(NpcAnimState::Idle);
    npc.update(dt, scene, *physics);
    std::vector<float> preRagPal = npc.skinner().lastPalette();

    npc.triggerRagdoll(x3::phys::Vec3{ 1.5f, 0.5f, 0.0f });
    npc.update(dt, scene, *physics);   // builds the ragdoll this frame
    ncheck(npc.ragdolled() && npc.ragdoll() != nullptr && npc.ragdoll()->inWorld(),
           "N3 triggerRagdoll spawns a live ragdoll in the world");
    const float topY0 = npc.ragdollLowestY();

    // Step the shared world + drive the skin until it settles (or a cap).
    bool settled = false;
    for (int i = 0; i < 600 && !settled; ++i) {
        physics->step(dt);
        npc.update(dt, scene, *physics);
        settled = npc.ragdollSettled();
    }
    const float restY = npc.ragdollLowestY();
    const bool finite = std::isfinite(restY) && std::isfinite(topY0);
    ncheck(finite, "N3a ragdoll bone transforms stay finite (no NaN)");
    ncheck(settled, "N3b ragdoll comes to rest (bodies sleep — settled)");
    // Settled on the floor: lowest bone within a bone-radius band of kFloorY, and
    // CLEARLY not hardwired to 0 (it's up at y~2) and not floating far above.
    ncheck(restY > kFloorY - 0.35f && restY < kFloorY + 0.6f,
           "N3c ragdoll settled ON the non-zero floor (not y=0, not floating)");
    ncheck(restY > 1.0f, "N3d settle is per-instance (well above a hardwired y=0)");

    // ---- N4: the ragdoll drove the SKIN (palette diverged from the animated pose). ----
    std::vector<float> ragPal = npc.skinner().lastPalette();
    ncheck(!ragPal.empty() && ragPal.size() == preRagPal.size() &&
           palDiff(ragPal, preRagPal) > 1e-3f,
           "N4 ragdoll drove the skin (palette diverged from the animated pose)");

    // ---- N5: despawn frees everything (bodies gone, GPU skin freed, entity hidden). ----
    const uint32_t ent = npc.entity();
    npc.despawn(scene, *physics);
    const bool hidden = (ent < scene.size()) ? !scene.get(ent).visible : true;
    ncheck(npc.despawned() && hidden && npc.ragdoll() == nullptr,
           "N5 despawn frees the ragdoll + hides the entity (no leak)");
    ncheck(!npc.skinner().gpuSkinning(), "N5b GPU skinned-mesh registration freed on despawn");

    // ---- N6: a small CROWD spawns, ticks, draws headlessly, and despawns clean. ----
    {
        NpcCrowd crowd;
        const uint32_t n = crowd.spawnRing(8, scene, device, *physics, riggedGlbRoot(),
                                           30.0f, kFloorY, 0.0f, 4.0f);
        ncheck(n == 8, "N6 crowd spawned 8 bodies");
        x3::rhi::FrameContext frame{};
        for (int i = 0; i < 30; ++i) {
            physics->step(dt);
            crowd.update(dt, scene, *physics);
            crowd.draw(device, frame, scene);   // headless no-op draw (exercises the path)
        }
        // Flop a couple of them and let them settle among the standing crowd.
        crowd.body(2).triggerRagdoll(x3::phys::Vec3{ 2, 0.5f, 1 });
        crowd.body(5).setAnimState(NpcAnimState::HitReact);
        for (int i = 0; i < 120; ++i) { physics->step(dt); crowd.update(dt, scene, *physics); }
        const bool bothDown = crowd.body(2).ragdolled() && crowd.body(5).ragdolled();
        ncheck(bothDown, "N6b crowd members flop via triggerRagdoll + HitReact");
        crowd.despawnAll(scene, *physics);
        bool allDown = true;
        for (uint32_t i = 0; i < crowd.count(); ++i) if (!crowd.body(i).despawned()) allDown = false;
        ncheck(allDown, "N6c crowd despawns clean (no leaked bodies)");
    }

    physics->shutdown();
    x3::logInfo("[npcchar-test] " + std::to_string(g_npass) + " passed, " +
                std::to_string(g_nfail) + " failed");
    return g_nfail == 0;
}

} // namespace x3::game
