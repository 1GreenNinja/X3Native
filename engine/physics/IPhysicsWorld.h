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
// Opaque shape handle (K-T0). A pre-built collision shape (convex hull / compound)
// the world keeps cached; a body can be created from it. 0 == invalid. Maps to a
// JPH::ShapeRefC kept inside JoltPhysicsWorld.cpp (no JPH:: types leak here).
struct ShapeId { uint32_t id = 0; bool valid() const { return id != 0; } };

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

    // Body orientation (D-phys, K-T0 subset). Quaternion in glTF/CONVENTIONS.md
    // order (x, y, z, w) — w LAST. Jolt's JPH::Quat is also xyzw (GetX/Y/Z/W),
    // so the boundary is a 1:1 component copy; see JoltPhysicsWorld.cpp.
    virtual void   getBodyRotation(BodyId, float outQuat[4]) const = 0; // -> (x,y,z,w)
    virtual void   setBodyRotation(BodyId, const float quat[4]) = 0;    // (x,y,z,w)

    // Linear / angular velocity (D-phys). Vectors are world-space (m/s, rad/s).
    virtual void   setBodyLinearVelocity(BodyId, const float v[3]) = 0;
    virtual void   getBodyLinearVelocity(BodyId, float out[3]) const = 0;
    virtual void   setBodyAngularVelocity(BodyId, const float v[3]) = 0;
    virtual void   getBodyAngularVelocity(BodyId, float out[3]) const = 0;

    // Per-body user tag (D-phys). Fast opaque uint64 the game can stamp on a body
    // (AI: "is this an enemy?"; later destruction: "is this destructible?"). 0 by
    // default. Maps to JPH::Body::SetUserData / GetUserData.
    virtual void     setBodyUserData(BodyId, uint64_t) = 0;
    virtual uint64_t getBodyUserData(BodyId) const = 0;

    // Character controller (capsule)
    virtual BodyId createCharacter(float radius, float height, Vec3 pos) = 0;
    virtual void   moveCharacter(BodyId, Vec3 desiredVelocity, float dt) = 0;
    virtual bool   characterGrounded(BodyId) const = 0;

    // Queries
    virtual RayHit rayCast(Vec3 origin, Vec3 dir, float maxDist, Layer mask) = 0;

    // Trigger callbacks (overlap-only sensors)
    using TriggerFn = void(*)(BodyId trigger, BodyId other, bool entered, void* user);
    virtual void setTriggerCallback(TriggerFn, void* user) = 0;

    // -----------------------------------------------------------------------
    // K-T0 destruction foundation (opaque; NO JPH:: types leak through here).
    // -----------------------------------------------------------------------

    // Build a convex-hull SHAPE from `n` points (`pts` = n*3 tightly-packed xyz
    // floats, LOCAL space). Returns an invalid ShapeId on a degenerate hull
    // (too few / coplanar points) — callers should fall back to a box AABB.
    // Maps to JPH::ConvexHullShape. The shape is cached and can back many bodies.
    virtual ShapeId addConvexHull(const float* pts, uint32_t n) = 0;

    // Build a COMPOUND shape from `n` previously-created child shapes, each placed
    // by a column-major 4x4 LOCAL transform (`localXforms4x4` = n*16 floats; only
    // the rotation + translation are honored — no scale/shear). Returns invalid on
    // empty/invalid input. Maps to JPH::StaticCompoundShape (fast tree).
    virtual ShapeId addCompound(const ShapeId* parts, const float* localXforms4x4,
                                uint32_t n) = 0;

    // Create a DYNAMIC body from a cached ShapeId at `pos` with `mass`. Returns an
    // invalid BodyId on a bad shape. The body participates in the sim + contact
    // callback exactly like addBox/addSphere bodies (this is how destruction spawns
    // the intact compound parent and the convex child chunks).
    virtual BodyId addBodyFromShape(ShapeId, Vec3 pos, float mass, Layer) = 0;

    // Queue-based contact callback (K-T0 / spec §4b). The function is invoked once
    // per contact POST-step (drained AFTER PhysicsSystem::Update returns) — NEVER
    // inside Jolt's locked OnContactAdded. So it is SAFE to mutate the world (add /
    // remove bodies, fracture) from inside this callback. `impulse` is an estimated
    // collision impulse magnitude (approachSpeed * min(mass1,mass2) along the
    // manifold normal). `point`/`normal` are world-space. Pass nullptr to disable.
    using ContactFn = void(*)(BodyId a, BodyId b, const float point[3],
                              const float normal[3], float impulse, void* user);
    virtual void setContactCallback(ContactFn, void* user) = 0;

    // Rebuild/optimize the broadphase quadtree after a big churn of body add/remove
    // (e.g. a large fracture). Maps to JPH::PhysicsSystem::OptimizeBroadPhase.
    virtual void optimizeBroadphase() = 0;

    // -----------------------------------------------------------------------
    // Native-handle escape hatch (vehicle framework). Returns the underlying
    // Jolt objects as void* so the type stays out of this header. This is ONLY
    // for in-engine subsystems that MUST integrate with Jolt at a level the POD
    // API can't express — specifically the WHEELED vehicle controller, which has
    // to construct a JPH::VehicleConstraint on the chassis JPH::Body and register
    // it as a step listener on the JPH::PhysicsSystem. Game/app code must NOT use
    // these; it talks to IVehicleController instead.
    //   nativeSystem()      -> JPH::PhysicsSystem*  (never null after init()).
    //   nativeBody(BodyId)  -> JPH::Body*           (null for an invalid/stale id
    //                          or a character-controller id).
    // The pointers are owned by the world and valid until shutdown()/removeBody().
    virtual void* nativeSystem() = 0;
    virtual void* nativeBody(BodyId) = 0;
};

IPhysicsWorld* createPhysicsWorld();

// Runs the M3 acceptance tests (T1-T8) in-process and returns true iff all
// pass. Logs each as "PASS T# <name>" / "FAIL T# ...". Implemented in
// JoltPhysicsWorld.cpp (where JPH:: types are available). Mirrors
// runAssetSelfTest()/runConsoleSelfTest().
bool runPhysicsSelfTest();

} // namespace x3::phys
