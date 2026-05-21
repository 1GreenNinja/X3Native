#pragma once
// Physics World + Character Controller interface — M3.
// Spec: specs/M3-physics-world.spec.md
//
// Clean interface: plain structs + opaque BodyId. NO JPH:: types leak here;
// the Jolt implementation lives entirely in JoltPhysicsWorld.cpp.
#include <cstdint>

namespace x3::phys {

struct Vec3 { float x = 0, y = 0, z = 0; };
struct BodyId { uint32_t id = 0; bool valid() const { return id != 0; } };

enum class Layer : uint8_t { Static, Dynamic, Player, Enemy, Projectile, Trigger };

struct RayHit { bool hit = false; BodyId body; Vec3 point; Vec3 normal; float distance = 0; };

class IPhysicsWorld {
public:
    virtual ~IPhysicsWorld() = default;

    virtual bool init() = 0;
    virtual void shutdown() = 0;
    virtual void step(float dtSeconds) = 0;          // accumulator -> fixed 1/60 internally

    // Bodies
    virtual BodyId addStaticMesh(const float* verts, uint32_t vcount,
                                 const uint32_t* indices, uint32_t icount) = 0;
    virtual BodyId addBox(Vec3 halfExtents, Vec3 pos, float mass, Layer) = 0;
    virtual BodyId addSphere(float radius, Vec3 pos, float mass, Layer) = 0;
    virtual void   removeBody(BodyId) = 0;
    virtual void   setBodyPosition(BodyId, Vec3) = 0;
    virtual Vec3   getBodyPosition(BodyId) const = 0;
    virtual void   applyImpulse(BodyId, Vec3) = 0;

    // Character controller (capsule)
    virtual BodyId createCharacter(float radius, float height, Vec3 pos) = 0;
    virtual void   moveCharacter(BodyId, Vec3 desiredVelocity, float dt) = 0;
    virtual bool   characterGrounded(BodyId) const = 0;

    // Queries
    virtual RayHit rayCast(Vec3 origin, Vec3 dir, float maxDist, Layer mask) = 0;

    // Trigger callbacks (overlap-only sensors)
    using TriggerFn = void(*)(BodyId trigger, BodyId other, bool entered, void* user);
    virtual void setTriggerCallback(TriggerFn, void* user) = 0;
};

IPhysicsWorld* createPhysicsWorld();

// Runs the M3 acceptance tests (T1-T8) in-process and returns true iff all
// pass. Logs each as "PASS T# <name>" / "FAIL T# ...". Implemented in
// JoltPhysicsWorld.cpp (where JPH:: types are available). Mirrors
// runAssetSelfTest()/runConsoleSelfTest().
bool runPhysicsSelfTest();

} // namespace x3::phys
