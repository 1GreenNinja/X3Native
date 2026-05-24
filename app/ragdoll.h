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

#include <vector>
#include <cstdint>

namespace x3::game {

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

} // namespace x3::game
