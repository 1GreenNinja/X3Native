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
    // `comOffset` shifts the body's CENTER OF MASS relative to the shape center,
    // in the body's local frame (meters, +Y up). Default {0,0,0} = shape center,
    // which is what every existing caller gets.
    //
    // WHY THIS EXISTS. For a vehicle the CoM height is THE handling parameter:
    // rollover threshold is roughly atan(halfTrack / comHeight). The hero car ran
    // with its CoM at the box center — 0.76 m up on a 0.677 m half-track, i.e.
    // ~42 deg, where a real sports car is nearer 58 — so it tipped over on
    // ordinary terrain. Tim, 2026-08-14: "the car still rolls" / "Can we ADD
    // center of mass control? That is a critical part."
    //
    // A negative Y drops the mass toward the floor pan (what a real car does with
    // engine, gearbox and battery low in the hull) and makes it resist roll
    // without touching track, springs or grip. Maps to Jolt's
    // OffsetCenterOfMassShape, which is the engine's own vehicle idiom.
    virtual BodyId addBox(Vec3 halfExtents, Vec3 pos, float mass, Layer,
                          Vec3 comOffset = Vec3{}) = 0;
    virtual BodyId addSphere(float radius, Vec3 pos, float mass, Layer) = 0;

    // -----------------------------------------------------------------------
    // KINEMATIC bodies (W-TRAFFIC). A kinematic body is game-driven — the sim
    // never integrates forces into it — but unlike a teleported static body it
    // carries REAL velocity through moveKinematic(), so a dynamic body it hits
    // (the player's car) receives a physically correct shove instead of a
    // zero-velocity wall. Created on the Dynamic broadphase layer so it
    // collides with Dynamic/Player/Enemy. Traffic cars are the first consumer:
    // each AI car is a kinematic box marched along its lane spline; on a hard
    // impact makeBodyDynamic() hands the SAME body to the solver (Jolt
    // SetMotionType, allowed at creation), and the car goes loose like the
    // work-zone drums. Default implementations are no-ops so facade/test
    // implementations of this interface (e.g. app/monster.cpp's
    // CountingPhysicsWorld) keep compiling; the Jolt world overrides all three.
    // -----------------------------------------------------------------------
    virtual BodyId addKinematicBox(Vec3 /*halfExtents*/, Vec3 /*pos*/, Layer /*layer*/) { return {}; }
    // Drive the kinematic body toward (targetPos, targetQuat) over dt seconds —
    // the implementation derives the linear/angular velocity that reaches the
    // target this step (Jolt MoveKinematic). No-op for non-kinematic/invalid.
    virtual void   moveKinematic(BodyId, Vec3 /*targetPos*/, const float /*targetQuat*/[4], float /*dt*/) {}
    // Convert a body created with addKinematicBox to a DYNAMIC body of `mass`
    // kg (inertia from the shape, scaled to that mass), activated. Returns
    // false for an invalid/never-kinematic body.
    virtual bool   makeBodyDynamic(BodyId, float /*mass*/) { return false; }

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
    // Resize a character's capsule to a new total `height` (radius unchanged), KEEPING the
    // feet anchored (the capsule still grows/shrinks upward from the feet, so crouch ducks
    // the head without popping the player up/down). Used for crouch/prone so the collision
    // capsule — not just the camera — actually shrinks to fit low gaps. Returns false and
    // leaves the capsule UNCHANGED if the taller shape would intersect surrounding geometry
    // (e.g. a ceiling above while crouched): the caller stays crouched rather than clip. A
    // no-op returning false for a non-character / invalid id.
    virtual bool   setCharacterHeight(BodyId, float height) = 0;
    // SWIM mode (W10). While enabled the character's velocity is taken VERBATIM
    // from moveCharacter's desiredVelocity — all three components, any sign — and
    // NO gravity is integrated (the caller owns buoyancy/strokes/dive). Collision
    // resolution (ExtendedUpdate slide) is unchanged, so a swimmer still slides
    // along banks/walls and finds ground. Disabling restores the normal
    // grounded/gravity/jump-impulse semantics with the vertical velocity reset to
    // 0 (the caller re-enters walking at rest — no stale fall speed). Default off;
    // a no-op for a non-character/invalid id.
    virtual void   setCharacterSwim(BodyId, bool enabled) = 0;

    // Queries
    //
    // ⚠ TRAP — `mask` is NOT an exclusive layer filter. It names the layer the
    // query TARGETS, and a body is hit when its layer EQUALS the mask *or* when
    // the two layers would collide per the body-vs-body matrix (see
    // queryHitsLayer/objectLayersCollide in JoltPhysicsWorld.cpp). Because
    // Static/Dynamic/Player/Enemy all collide with each other, a
    // rayCast(..., Layer::Static) "wall probe" ALSO reports Dynamic props,
    // Player bodies and — the one that bites — ENEMY bodies.
    //
    // That silently breaks the classic line-of-sight idiom "cast at my target,
    // masked to Static; if it hits, a wall is in the way": the ray hits the
    // TARGET'S OWN Enemy box (monster hitboxes are 0.6 m half-width, so even a
    // ray shortened by 0.3 m still lands inside it) and the caller concludes it
    // is blocked. Sarah's companion combat never fired a single shot for exactly
    // this reason; enemy health bars / nameplates / crowd chat bubbles were
    // culled for the same reason.
    //
    // For an LOS / wall probe use rayCastStrict(..., Layer::Static) below.
    // Some callers legitimately DEPEND on the permissive behavior — e.g.
    // engine/ai/Navigation.cpp floor-samples with a Dynamic mask and needs the
    // Static floor back, and monster fire casts an Enemy mask and needs walls to
    // stop the bullet. Do NOT "fix" this by making the mask exclusive.
    virtual RayHit rayCast(Vec3 origin, Vec3 dir, float maxDist, Layer mask) = 0;

    // STRICT layer query: hits ONLY bodies whose object layer EQUALS `mask` —
    // no collision-matrix fall-through. This is the primitive an LOS / wall
    // probe wants: rayCastStrict(from, dir, len, Layer::Static) reports walls,
    // floors and door slabs (all authored on Layer::Static) and passes cleanly
    // THROUGH characters, corpses and props. Identical to rayCast in every other
    // respect (same normalization, same zero-length/zero-dist guards, same
    // RayHit fields).
    virtual RayHit rayCastStrict(Vec3 origin, Vec3 dir, float maxDist, Layer mask) = 0;

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
