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
// Opaque constraint handle. Backs BOTH a two-body joint (physics props / ragdoll
// chains) AND a Physics §1 single-body-to-world joint (point/distance to a fixed
// world anchor). 0 == invalid. Maps to a JPH::Ref<JPH::Constraint> kept inside
// JoltPhysicsWorld.cpp (no JPH:: types leak here).
struct ConstraintId { uint32_t id = 0; bool valid() const { return id != 0; } };

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

    // (Per-body damping is declared once, below, with the §1 constraint API.)

    // -----------------------------------------------------------------------
    // Two-body JOINTS (physics props §1 "hanging cubes" + ragdoll §2 chains).
    // Opaque ConstraintId; no JPH:: types leak. A point (ball) joint pins the two
    // bodies together at one world anchor — they can swing freely about it. Pass an
    // invalid BodyId for `a` to pin `b` to the WORLD at `worldAnchor` (a fixed
    // point), which is how a cube "hangs from above".
    // -----------------------------------------------------------------------
    virtual ConstraintId addPointConstraint(BodyId a, BodyId b, Vec3 worldAnchor) = 0;
    // (removeConstraint is declared once, below, with the §1 constraint API — it
    //  tears down BOTH this two-body joint form and the single-body §1 joints.)

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
    // Physics §1 — suspended / constrained bodies (swinging cubes). Hang a
    // dynamic body from a FIXED world-space anchor so it swings like a pendulum
    // under gravity and settles via damping. Opaque; NO JPH:: types leak here.
    // Built on Jolt's PointConstraint / DistanceConstraint against the implicit
    // world body (JPH::Body::sFixedToWorld) — no fake anchor body needed.
    // -----------------------------------------------------------------------

    // Attach a dynamic `body` to a fixed world-space point `anchorWorld` with a
    // BALL/POINT joint: the body's current attach point (a world-space offset
    // `bodyAttachWorld`) is pinned to `anchorWorld`, leaving all 3 rotational
    // DOF free so it swings + spins like a hung cube. `bodyAttachWorld` is the
    // world-space point ON the body that gets pinned (usually a top corner/edge
    // of the cube) — at constraint time it should coincide with the body's
    // current pose. Returns invalid on a bad/static body. The joint is owned by
    // the world and torn down at shutdown() or removeConstraint().
    virtual ConstraintId addPointConstraint(BodyId body, Vec3 anchorWorld,
                                            Vec3 bodyAttachWorld) = 0;

    // Attach a dynamic `body` to a fixed world-space point `anchorWorld` with a
    // DISTANCE joint (a rope/rod of length [minLen,maxLen] between the body's
    // current attach point and the anchor). With minLen==maxLen it is a rigid
    // rod; with minLen<maxLen it is a rope that lets the body fall to maxLen then
    // swing. Returns invalid on a bad/static body.
    virtual ConstraintId addDistanceConstraint(BodyId body, Vec3 anchorWorld,
                                               Vec3 bodyAttachWorld,
                                               float minLen, float maxLen) = 0;

    // Remove + destroy a previously-created constraint. Safe on an invalid/stale
    // id (no-op). The two bodies are left free.
    virtual void removeConstraint(ConstraintId) = 0;

    // Set a dynamic body's linear + angular damping (dv/dt = -c*v, dw/dt = -c*w;
    // both >= 0, usually small e.g. 0.05). Higher values settle a swing faster.
    // No-op for a static/character/invalid body.
    virtual void setBodyDamping(BodyId, float linear, float angular) = 0;

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

// Physics §1 self-test (--test-physjoint): create a dynamic body on a point
// constraint, step the sim, and assert it (1) hangs + swings under gravity
// (the swing angle oscillates about the anchor), (2) settles with damping, and
// (3) re-settles after an impulse displaces it; no NaNs; leak-clean. Prints
// "physjoint: X/Y passed" and returns true iff all pass. Implemented in
// JoltPhysicsWorld.cpp (where JPH:: types are available).
bool runPhysJointSelfTest();

} // namespace x3::phys
