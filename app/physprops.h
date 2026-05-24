#pragma once
// Physics props (FEATURE_GOALS §1): interactive constrained dynamic bodies —
// "cubes hanging from a point above that swing when you walk into them, then
// settle." Built on the engine's dynamic bodies (addBox mass>0) + the new
// point-constraint joint API + per-body damping. The SAME Jolt solver carries the
// ragdoll constraint chains (§2), so this is the shared foundation. Game/slice
// code only; engine/ stays pure.
#include "scene.h"

#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"

#include <vector>

namespace x3::game {

// One hanging prop: a dynamic box drawn at its live physics transform, pinned to
// a fixed world anchor above by a point (ball) joint so it swings like a pendulum.
struct PhysProp {
    uint32_t               entity = kNoLink;
    x3::phys::BodyId       body;
    x3::phys::ConstraintId joint;
};

class PhysPropsSystem {
public:
    // Build `count` hanging cubes in a row centered on `origin` (origin.y is the
    // FLOOR), each suspended from an anchor `drop` metres above by a point joint
    // and lightly damped so a knock settles. Render boxes via `device`, dynamic
    // bodies + joints via `physics`, Entities in `scene`.
    void build(Scene& scene, x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics,
               x3::phys::Vec3 origin, int count = 4, float drop = 2.6f);

    // Sync each prop's Entity transform (rotation + translation) from its physics
    // body so the render box follows the swing. Call once per frame after step().
    void update(Scene& scene, x3::phys::IPhysicsWorld& physics);

    uint32_t count() const { return (uint32_t)m_props.size(); }

private:
    std::vector<PhysProp> m_props;
};

// Headless self-test (--test-physprops): a cube pinned by a point joint hangs
// (doesn't fall), swings when shoved then settles, and FALLS once the joint is
// removed. Asserts T0-T3; returns true iff all pass. No window/Vulkan.
bool runPhysPropsSelfTest();

} // namespace x3::game
