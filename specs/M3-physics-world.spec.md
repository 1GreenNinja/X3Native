# Spec: Physics World + Character Controller (M3)

> Clean-room — implement from THIS FILE + public refs ONLY. No RBDOOM source.
> Jolt is MIT — strictly clean. Zero 14900K dependency.

- **Implements interface:** `IPhysicsWorld` (`engine/physics/IPhysicsWorld.h`)
- **Status:** SPEC (ready)
- **Library:** Jolt Physics (MIT).

## 1. Purpose
A fixed-timestep physics world (Jolt) decoupled from render rate: static level collision, rigid-body dynamics, a capsule character controller for the player/bots, and ray/shape queries for weapons + AI line-of-sight.

## 2. Interface contract
```cpp
// engine/physics/IPhysicsWorld.h — clean, no JPH types leak
#include <cstdint>

namespace x3::phys {

struct Vec3 { float x=0,y=0,z=0; };
struct BodyId { uint32_t id = 0; bool valid() const { return id != 0; } };

enum class Layer : uint8_t { Static, Dynamic, Player, Enemy, Projectile, Trigger };

struct RayHit { bool hit=false; BodyId body; Vec3 point; Vec3 normal; float distance=0; };

class IPhysicsWorld {
public:
    virtual ~IPhysicsWorld() = default;

    virtual bool init() = 0;
    virtual void shutdown() = 0;
    virtual void step(float dtSeconds) = 0;          // accumulator → fixed 1/60 internally

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

} // namespace x3::phys
```

## 3. Behavior
- Fixed timestep: accumulate `dt`, step Jolt at 1/60 (configurable), interpolate render transforms between steps.
- Layers + collision matrix: Player↔World/Enemy/Trigger; Projectile↔World/Enemy (not its owner); Trigger = sensor only.
- Character controller: `JPH::CharacterVirtual` — walk, collide with walls, step up small ledges, slide on slopes, report grounded.
- Threading: Jolt has its own job system; point it at a thread pool. `step` is main-thread-driven but parallel internally.

## 4. Edge cases & error handling
- Body created with mass 0 + Dynamic layer → treat as static, log a warning.
- Character stuck (penetrating geometry) → Jolt's depenetration; cap max push to avoid teleport.
- Raycast with zero-length dir → no hit, no crash.
- removeBody on an invalid/stale id → no-op, log once.
- Very large static mesh → build a Jolt `MeshShape`; warn if vertex count is huge.

## 5. Performance targets
- 200 dynamic bodies + 1 character at ≤ 1.5 ms/step on the 13700K (Jolt scales across cores).
- Raycast ≤ 5 µs typical.
- No per-step heap allocation in the query path.

## 6. Acceptance tests
1. **T1 — Falling box:** add a static ground + a dynamic box above it; after N steps the box rests on the ground (y ≈ groundTop + halfExtent), sleeps.
2. **T2 — Stack settles:** 10 stacked boxes settle + sleep, no jitter.
3. **T3 — Character walk:** capsule on a heightfield walks horizontally, collides with a wall (stops), `characterGrounded()==true` on ground / false mid-jump.
4. **T4 — Step up:** character climbs a 0.3 m step without jumping.
5. **T5 — Raycast:** ray from above hits the ground, returns correct point + up normal + distance.
6. **T6 — Layer filter:** a projectile body passes through its owner (same-owner filter) but a ray with Enemy mask hits an enemy body.
7. **T7 — Trigger:** moving a body into a trigger fires `entered=true`; leaving fires `entered=false`.
8. **T8 — Determinism:** same inputs over 2 runs → same final positions (seeded, fixed-step).

## 7. Public references
- Jolt Physics documentation + samples (especially CharacterVirtual + the HelloWorld).
- Jolt's `PhysicsSystem`, `BodyInterface`, `NarrowPhaseQuery` API docs.

## 8. Suggested permissive libraries
- **Jolt Physics** (MIT) — the whole subsystem.

## 9. Notes for the clean-room implementer
- Keep all `JPH::` types in the .cpp; the interface is plain structs + opaque BodyId.
- Map `Layer` → Jolt object layers + broad-phase layers via a small static table.
- Movement feel (accel/air-control/jump) is tuned at the game layer (Lua), not here — this exposes the primitives. Match the Babylon X3 reference values during M5 Slice 36.
- Wire Jolt's debug renderer to an engine line-renderer later (M3 Slice 40) — out of scope here.
