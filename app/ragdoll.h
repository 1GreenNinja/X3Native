#pragma once
// Ragdoll (FEATURE_GOALS §2): physics-driven death ragdoll. An articulated set of
// dynamic body PARTS (pelvis / torso / head / 2 arms / 2 legs) linked by point
// (ball) joints — the SAME joint + dynamic-body primitives the physics props (§1)
// use, so one Jolt solver drives both. On spawn the parts inherit a death impulse
// and tumble/collapse under gravity into a heap, then settle. Game/slice code only.
//
// This is the graybox ragdoll: each part is drawn as its own box at its live
// physics transform (a tumbling collection of limbs). Driving a skinned GLB
// skeleton from these bodies (per-bone matrices) is the natural next layer.
#include "scene.h"

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"
#include "engine/asset/IModelLoader.h"

#include <vector>
#include <cstdint>

namespace x3::game {

// ---------------------------------------------------------------------------
// RagdollSkin — drive a skinned model's bones from ragdoll parts (rigid attach).
//
// Each skeleton bone follows its nearest ragdoll part by a RIGID delta:
//   nodeGlobal[n] = (partCur[p] * partInit[p]^-1) * bindGlobal[n]
// where p = the part assigned to node n. All transforms are in the model's SKIN
// space (the same space the normal joint palette is built in), so the result feeds
// Skinner::applyExternalGlobals() and draws through the model's existing
// object*fixup*node transform unchanged. Pure math — no physics/render deps beyond
// the Model + the per-part transforms the caller supplies. Headless-testable.
// ---------------------------------------------------------------------------
class RagdollSkin {
public:
    // Compute + cache each node's BIND-pose global (model/skin space) by composing
    // node local TRS down the hierarchy. Returns false if the model has no nodes.
    bool bind(const x3::asset::Model& model);

    // As bind(), but seed the reference globals from a CALLER-supplied set of per-node
    // GLOBAL (model/skin space) matrices (`nodeGlobals` = nodeCount*16, column-major)
    // instead of the static bind pose — e.g. the animation's CURRENT pose, captured via
    // anim::Skinner::currentGlobals(). The rigid bone->skin delta is then identity at
    // frame 0, so a death ragdoll (TASK#12) flops SEAMLESSLY from exactly where the
    // animation left off (no one-frame pop to bind pose). `nodeCount` must match the
    // model's node count. Returns false on mismatch / empty model.
    bool bindFromGlobals(const x3::asset::Model& model,
                         const float* nodeGlobals, uint32_t nodeCount);

    // Assign every node to the nearest part by bind-pose node position. `partInit`
    // = each part's initial (bind-time) skin-space 4x4 (count*16, column-major) —
    // its center is used for the nearest test and as the rigid-delta reference.
    void mapToParts(const float* partInit, uint32_t partCount);

    // Produce per-node globals into `outNodeGlobals` (nodeCount*16) from each part's
    // CURRENT skin-space 4x4 (`partCur` = partCount*16). out[n] = delta[assign[n]] *
    // bindGlobal[n]. Reuses internal scratch. No-op (returns 0) if unbound/mismatch.
    uint32_t computeNodeGlobals(const float* partCur, uint32_t partCount,
                                std::vector<float>& outNodeGlobals) const;

    uint32_t nodeCount() const { return m_nodeCount; }
    bool     ready() const { return m_nodeCount > 0 && !m_assign.empty(); }

private:
    uint32_t              m_nodeCount = 0;
    uint32_t              m_partCount = 0;
    std::vector<float>    m_bindGlobal;   // nodeCount*16
    std::vector<float>    m_partInitInv;  // partCount*16 (inverse of each part's init)
    std::vector<int>      m_assign;       // node -> part index
};

// One ragdoll limb/segment: a dynamic box body drawn at its live transform.
struct RagdollPart {
    uint32_t          entity = kNoLink;
    x3::phys::BodyId  body;
    float             halfY  = 0.2f;   // for reference / debug
};

// An articulated ragdoll instance. Built once on death; ticked each frame to sync
// the render parts; lingers as a settled heap (cheap — Jolt sleeps it).
class RagdollSystem {
public:
    // Build a humanoid ragdoll standing at `footPos` (feet on the ground), facing
    // `yaw` (radians, about +Y), scaled by `scale` (1.0 == ~1.8 m). `tint` colors
    // the parts. An optional `impulse` (world m/s * mass, applied to the torso)
    // kicks the death tumble — e.g. away from the shot. Render boxes via `device`,
    // dynamic bodies + point joints via `physics`, Entities in `scene`.
    void build(Scene& scene, x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics,
               x3::phys::Vec3 footPos, float yaw, float scale,
               const float tint[4], x3::phys::Vec3 impulse);

    // Sync every part's Entity transform (rotation + translation) from its body so
    // the render boxes follow the tumble. Call once per frame after step().
    void update(Scene& scene, x3::phys::IPhysicsWorld& physics);

    // Remove all bodies + joints (e.g. corpse cleanup). Idempotent.
    void clear(x3::phys::IPhysicsWorld& physics);

    uint32_t partCount() const { return (uint32_t)m_parts.size(); }
    const RagdollPart& part(uint32_t i) const { return m_parts[i]; }
    bool built() const { return !m_parts.empty(); }

private:
    std::vector<RagdollPart>            m_parts;
    std::vector<x3::phys::ConstraintId> m_joints;
};

// Headless self-test (--test-ragdoll): a ragdoll built above a floor collapses
// (parts fall + settle on the floor), its joints HOLD it together (head stays near
// the torso — it doesn't explode), and it comes to rest. Asserts T0-T3; returns
// true iff all pass. No window/Vulkan.
bool runRagdollSelfTest();

// Headless self-test (--test-ragdollskin): exercises the rigid-attach math on a
// tiny synthetic node chain + parts — parts at their bind transform reproduce the
// bind globals (no drift), and translating/rotating a part rigidly moves its
// assigned nodes by the same delta. Asserts S0-S3. No window / Vulkan.
bool runRagdollSkinSelfTest();

} // namespace x3::game
