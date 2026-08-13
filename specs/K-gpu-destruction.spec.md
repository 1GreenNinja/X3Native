# Spec: GPU Destruction Physics + Hybrid Jolt Handoff (K)

> Written by the SPEC TEAM. Implemented by the CLEAN-ROOM TEAM from THIS FILE + public refs ONLY.
> ❌ No GPL source, no transcribed function bodies, no RBDOOM identifiers/paths below this line.
>
> **Clean-room basis:** Rouwe "Architecting Jolt Physics for Horizon Forbidden West" (GDC 2022), Catto sequential-impulse GDC talks, Harada "Real-Time Rigid Body Simulation on GPUs" (GPU Gems 3), Sousa "Fast as Hell" (SIGGRAPH 2025), Müller et al. position-based dynamics papers, the Vulkan spec, and the permissive Jolt library. No id Tech/RBDOOM source.

- **Ledger ID:** subsystem **K** (roadmap) — extends `IPhysicsWorld` + adds `IDestructionSystem` / `IGpuDebrisWorld`
- **Implements interface:** `IDestructionSystem` (`engine/physics/IDestructionSystem.h`) + `IPhysicsWorld` extensions + a compute path in the renderer
- **Status:** SEED→SPEC. Downstream of the renderer core (needs compute pipelines, SSBOs, buffer/texture API, indirect draw) and the job system. Per `X3NATIVE_ROADMAP.md` decision **D-PHYS**, priority after A (jobs), C/D (GPU-driven renderer), and H (streaming).
- **Spec author machine:** 14900K · **Clean-room target machine:** 13700K (compute OK on 1080 Ti; async *overlap* weak on Pascal — full win on the 5090)

## 0. Feature tiers (idTech 8 pillar #4 parity → beyond)

| Tier | Scope |
|---|---|
| **T0 — foundation** | `addConvexHull`/`addCompound` shapes + body user-data + queue-based contact callback in `IPhysicsWorld` (also fixes door-rotation/monster-facing gaps) |
| **T1 — Jolt destruction** | authored pre-fractured assets; impact/explosion/weapon breaks; parent→convex children with split impulse; concurrency caps; deterministic CPU sim |
| **T2 — idTech 8 parity** | **GPU compute debris world** (thousands of fragments in SSBOs, indirect-drawn), CPU→GPU **handoff** of small/sleeping pieces, smart **sleep/wake persistence**, async-compute + timeline-semaphore sync, FX coupling (craters→decals, dust→particles) |
| **T3 — beyond** | **structural-connectivity** destruction (support graph → unsupported chunks fall, progressive collapse, Teardown/Red-Faction-tier), nested/hierarchical fracture, GPU-side narrow-phase scaling to **1M+** debris on the 5090, optional deterministic GPU sim, two-way coarse readback to gameplay |

All additions keep `JPH::`/Vulkan types out of public headers (opaque handles + POD structs).

---

## 1. Architecture — hybrid CPU(Jolt authoritative) + GPU(compute visual), per D-PHYS
Two worlds, **one-way coupling**:
- **Jolt (CPU, authoritative):** gameplay-critical bodies — player, AI-relevant, anything raycast/queried, **and the destruction *event*** (intact object + its fracture into convex child pieces). Deterministic, correct.
- **GPU compute (visual debris):** the thousands of resulting small/resting fragments. Live entirely in SSBOs, simulated in compute, drawn via indirect draw.
- **Coupling:** gameplay/explosions push impulses *in*; large pieces stay in Jolt; **small/sleeping pieces hand off to the GPU pool**; a big GPU event can wake a piece back into Jolt. Debris does **not** feed gameplay queries (or only via cheap coarse readback). We do **not** move authoritative sim to the GPU.

---

## 2. Interface contract (clean, authored fresh)

```cpp
// engine/physics/IDestructionSystem.h — opaque ids; no JPH/Vulkan types
using DestructibleId = uint32_t;
using FractureAssetId = uint32_t;

struct FracturePieceDesc { const float* hullPoints; uint32_t pointCount;
                           float localOffset[3]; float localRot[4]; float mass; };
struct FractureAssetDesc { const FracturePieceDesc* pieces; uint32_t pieceCount;
                           const int32_t* adjacency; uint32_t adjacencyCount;   // connectivity graph (T3)
                           float breakImpulse; float breakRelVel; uint8_t fractureDepth; };

struct BreakEvent { DestructibleId id; float worldPos[3]; float impulse; uint32_t childCount; };

class IDestructionSystem {
public:
    virtual ~IDestructionSystem() = default;

    virtual FractureAssetId loadFractureAsset(const FractureAssetDesc&) = 0;     // offline-authored (Voronoi)
    virtual DestructibleId  spawnDestructible(FractureAssetId, const float xform[16]) = 0;
    virtual void            despawn(DestructibleId) = 0;

    // explicit (non-contact) breaks
    virtual void applyRadialImpulse(const float center[3], float radius, float strength) = 0; // explosion
    virtual void applyHit(const float point[3], const float dir[3], float strength) = 0;      // weapon ray/shape hit

    // per-frame: called AFTER IPhysicsWorld::step() (never inside a contact callback)
    virtual void update(float dt) = 0;

    // events out (queued, drained post-step)
    virtual uint32_t          drainBreakEvents(BreakEvent* out, uint32_t maxOut) = 0;

    // GPU debris world (T2) — opaque buffer ids handed to the renderer for indirect draw
    virtual uint32_t debrisInstanceSSBO() const = 0;
    virtual uint32_t debrisIndirectDrawBuffer() const = 0;
    virtual uint32_t activeDebrisCount() const = 0;

    // tuning (start values §15)
    virtual void setSolverTuning(int velSteps, int posSteps, float baumgarte, float slop) = 0;
    virtual void setDebrisBudget(uint32_t maxGpuDebris, uint32_t maxJoltChildren) = 0;
};
```

```cpp
// engine/physics/IPhysicsWorld.h — additions K depends on (T0; opaque, no JPH types)
virtual ShapeId addConvexHull(const float* pts, uint32_t n) = 0;
virtual ShapeId addCompound(const ShapeId* parts, const float* localXforms, uint32_t n) = 0;
virtual void    setBodyVelocity(BodyId, const float lin[3], const float ang[3]) = 0;
virtual void    getBodyLinearVelocity(BodyId, float out[3]) = 0;
virtual void    getBodyAngularVelocity(BodyId, float out[3]) = 0;
virtual void    getBodyRotation(BodyId, float outQuat[4]) = 0;            // also fixes door/monster-facing
virtual void    setBodyRotation(BodyId, const float quat[4]) = 0;
virtual void    setBodyUserData(BodyId, uint64_t) = 0;                    // fast "is destructible?" marker
virtual uint64_t getBodyUserData(BodyId) = 0;
// queue-based contact callback (fired POST-step, never inside the locked callback):
using ContactFn = void(*)(BodyId a, BodyId b, const float point[3], const float normal[3], float impulse, void* user);
virtual void setContactCallback(ContactFn, void* user) = 0;
virtual void optimizeBroadphase() = 0;
virtual void addBodiesBatched(const BodyDesc* descs, uint32_t n, BodyId* outIds) = 0;
```

---

## 3. Fracture authoring pipeline (offline)
- Pre-fracture meshes **offline** (Blender Cell Fracture / Houdini Voronoi): an intact mesh → N convex chunks with local offsets, masses, and a **connectivity/adjacency graph** (which chunks touch — feeds T3 structural integrity). Bake to a `FractureAsset` (chunks as convex hulls).
- **Nested/hierarchical fracture (T3):** `fractureDepth` allows a chunk to itself carry a sub-fracture asset, so a large piece can shatter again on a second strong impact (depth-limited to bound cost).
- **Material patterns:** fracture style per material (glass = many shards + radial pattern; concrete = chunky + dust; wood = splinters along grain). Stored on the asset; drives child count, child size distribution, and the FX coupling (§7).

## 4. Jolt (CPU) side
### 4a. DestructibleManager
- `DestructibleObject { parentBodyID; pieces[]; isBroken; material; }`. **Intact body** = one `Dynamic` body with a `StaticCompoundShape` (fast tree) of the pieces; `MutableCompoundShape` only if progressive add/remove is needed. Fast "is destructible?" via `Body::mUserData` marker; full lookup via `unordered_map<BodyID,DestructibleObject>`.
### 4b. Break detection — contact listener
- `OnContactAdded`/`OnContactPersisted`: estimate impulse (`approachSpeed * min(mass1,mass2)` along the manifold normal); compare to thresholds (impulse + relative-velocity).
- **CRITICAL:** never mutate physics (add/remove/fracture) inside the contact callback — bodies are locked. **Queue** fracture requests (thread-safe), then `ProcessPendingFractures()` once per step **after** `PhysicsSystem::Update()`. Non-contact breaks (weapon ray/shape hits, explosion radius) enqueue directly.
### 4c. Fracture spawning
- On break: read parent transform + linear/angular velocity, **remove the parent**, spawn each child `Dynamic` at `parentPos + parentRot*localOffset`. Child linear = parentLin + impact·k + radialFromImpact·(breakImpulse/mass); child angular = parentAng + (chunkPos−parentPos)×impactVel·k. Cap concurrent active pieces per object (§15); excess/small/quiet pieces route to the GPU pool (§6).
### 4d. Solver + broadphase (matters at high counts)
- Jolt = sequential-impulse (PGS): warm-started velocity solver (~4–10 iters) + Baumgarte position solver (~2–4 iters), **island-based** (parallel). Settings exposed at init: `mNumVelocitySteps`, `mNumPositionSteps`, `mBaumgarte`, `mPenetrationSlop`, `mSpeculativeContactDistance`. Raise velocity steps temporarily under heavy destruction.
- Broadphase = quadtree, double-buffered/lock-free. **Batch** add/remove (`AddBodiesPrepare/Finalize`); `OptimizeBroadPhase()` after big churn. 6DOF constraints suit progressive breaking (relax limits on damage).

## 5. GPU (compute) debris world (T2)
- **SoA SSBOs**, `DEVICE_LOCAL`: position+invMass, quat rotation, linear/angular velocity, inertia, flags, sleepCounter, materialID. Batch like-material debris per dispatch for warp coherence. Pre-allocated pool of "dead" slots; CPU writes new debris via a staging buffer.
- **Pipeline per fixed step (barriers between):**
  1. **Broadphase:** uniform grid + spatial hash (cell ≈ largest debris AABB; 5–20 objects/cell). Two-phase insert: count (`atomicAdd`) → prefix-sum → scatter. Pair-gen tests cell+neighbors, `idA<idB` dedup.
  2. **Narrow-phase + impulse resolution:** sphere/box/convex contacts; sequential-impulse or small-iteration PBD on the GPU.
  3. **Integrate:** semi-implicit Euler + quaternion angular update, gravity/damping.
  4. **Sleep/wake:** per-body sleep counter; below vel-threshold N frames → sleep (skip integration, keep in broadphase); wake on impulse/contact. *This keeps persistent debris cheap — the idTech 8 "smart sleep/wake persistence."*
- **Determinism (T3, optional):** fixed iteration counts + deterministic atomics ordering for replay; off by default (visual-only).

## 6. CPU↔GPU handoff
- After Jolt `Update()`: small/sleeping fragments → upload to the GPU pool (staging buffer → "dead" slots), remove from Jolt to bound island/solver cost.
- On a high-impact GPU event near gameplay → coarse readback → re-spawn a Jolt body.
- Budget-driven: `setDebrisBudget(maxGpuDebris, maxJoltChildren)`; overflow recycles oldest sleeping debris (ring/LRU).

## 7. Render + FX coupling
- Debris drawn via **indirect draw + instancing** from `debrisInstanceSSBO`/`debrisIndirectDrawBuffer`; integrate with the GPU-driven culling (subsystem D) and **GPU LOD** to bound VRAM/overdraw (distant debris → impostor/cull).
- **Crater/scar decals:** on break, project a decal at the impact (deferred decal or mesh decal) — persistent surface damage.
- **Dust/spark particles:** material pattern (§3) spawns a GPU particle burst (ties to subsystem K's sibling particle system); glass=shards+sparkle, concrete=dust plume, wood=splinters.
- **Audio:** break event → notify → audio M9 (material-keyed break sound).

## 8. Structural connectivity / progressive collapse (T3 — beyond idTech 8's published scope)
- Use the adjacency graph (§3): when chunks break, run a GPU/CPU **connected-components** pass over the remaining graph; any sub-graph no longer connected to a "support" anchor (ground/static) **wakes and falls** under gravity — buildings sag and collapse rather than floating. Throttle the connectivity pass (only on break events, amortized). This is the Red Faction Geo-Mod / Teardown-class behavior; gated as a high-end feature.

## 9. Async compute + sync
- Run the debris compute on an **async-compute queue**; **timeline semaphores** order compute→(vertex/indirect read). On Pascal (1080 Ti) async *overlap* is weak (preemption-based) — design the async path; expect the real overlap win on the 5090. Provide a synchronous fallback path so it runs correctly (just not overlapped) on Pascal.

## 10. Edge cases & error handling
- Fracture requested in a contact callback → must be queued (test T2 enforces this); direct mutation is a defect.
- Debris pool exhausted → recycle oldest sleeping (no alloc, no hitch); never block the frame.
- Convex hull degenerate (coplanar/too few points) → reject at `loadFractureAsset`, log, treat piece as a box AABB fallback.
- Single-precision far-from-origin jitter (Open Decision #1) → camera-relative origin rebasing applies to debris too.
- Device loss → debris world reinitializes empty (visual-only, acceptable); Jolt authoritative state unaffected.
- GPU readback latency → gameplay never *waits* on debris readback (1–2 frame stale tolerated; coupling is one-way).

## 11. Performance targets
- **Jolt destruction:** a 200-chunk building fracture stays within the physics frame budget; ≤ 256 active Jolt children before handoff to GPU.
- **GPU debris:** **50k+** simultaneous fragments simulate + sleep/wake at ≥60 FPS on the 1080 Ti (compute); **1M+** on the 5090. ≤ ~1.5 ms compute on the 5090 for 1M sleeping/light-load debris.
- **Handoff:** moving 1k pieces CPU→GPU in a frame causes no hitch (staged, amortized).
- **No per-frame heap alloc** on CPU; GPU pool is fixed-size. Stable frame time under sustained destruction (variance is a gate, per idTech 8 pillar #1).

## 12. Acceptance tests (clean-room team implements these)
1. **T1 — authored fracture:** impact above threshold → parent removed + N convex children with split linear/angular velocity; below threshold → no break.
2. **T2 — callback safety:** all fractures are queued and applied post-step; zero physics mutation inside the locked contact callback (assert).
3. **T3 — explosion/weapon breaks:** radial impulse and ray/shape hit each enqueue a fracture without a contact.
4. **T4 — GPU debris sim:** N=50k fragments integrate, collide coarsely, and sleep; FPS ≥ target on the 1080 Ti.
5. **T5 — sleep/wake persistence:** sleeping debris costs ~0 integration; an impulse wakes only the affected neighborhood.
6. **T6 — handoff:** small Jolt pieces transfer to the GPU pool (Jolt island count drops); a big GPU event re-spawns a Jolt body.
7. **T7 — determinism (Jolt side):** fixed-step CPU destruction reproduces identically across two runs.
8. **T8 — budget/recycle:** exceeding `maxGpuDebris` recycles oldest sleeping debris with no hitch and no alloc.
9. **T9 — FX coupling:** a break spawns a decal + material-appropriate particle burst + a break sound notify.
10. **T10 — structural collapse (T3):** breaking the supports of a connected structure causes the unsupported sub-graph to wake and fall (connectivity pass correct).
11. **T11 — async sync:** debris compute on the async queue is correctly ordered before the indirect draw read (no read-before-write); synchronous fallback also correct on Pascal.

## 13. Public references
- Rouwe, "Architecting Jolt Physics for Horizon Forbidden West," GDC 2022; Jolt Physics docs/samples.
- Catto, "Soft Constraints" / "Understanding Constraints" / sequential-impulse GDC talks.
- Harada, "Real-Time Rigid Body Simulation on GPUs," GPU Gems 3, ch. 29.
- Müller et al., "Position Based Dynamics" / "Detailed Rigid Body Simulation with XPBD."
- Sousa, "Fast as Hell" (SIGGRAPH 2025) — GPU scene-graph offload context.
- Parker & O'Brien, "Real-Time Deformation and Fracture in a Game Environment" (Star Wars: Force Unleashed) — fracture/structural ideas.
- Vulkan spec — compute pipelines, SSBOs, atomics, indirect draw, async-compute queues, timeline semaphores.
- Public Red Faction "Geo-Mod" and Teardown voxel-destruction talks — structural-integrity tier (T3).

## 14. Suggested permissive libraries (clean IP)
- Jolt Physics (MIT) — authoritative CPU world, convex hulls, compound shapes, contact listeners.
- Voronoi/convex tooling offline (Houdini/Blender) — authoring only, not shipped code.
- glm (MIT) — math. miniz (MIT) — pack fracture assets into `.x3pak`.

## 15. Tuning knobs (start values; tune to mass scale)
Break impulse ~15, break velocity ~8, child impact factor ~0.4, radial impulse ÷mass, angular ×~8; under heavy load velocity steps 10 / position steps 4 / baumgarte 0.2 / slop 0.02; GPU cell size ≈ largest debris AABB (5–20 objects/cell); sleep velocity threshold + N frames; per-object active-children cap ~64 before GPU handoff; `maxGpuDebris` sized to VRAM (e.g. 256k on 1080 Ti, 1M+ on 5090).

## 16. Build order & dependencies
- **T0** lands with the renderer/physics maturing: add `addConvexHull`/`addCompound`, body user-data, velocity/rotation getters, and the queue-based contact callback to `IPhysicsWorld` (these also fix the door-rotation + monster-facing gaps today).
- **T1** (Jolt destruction) can ship before the GPU path — pure CPU, deterministic, immediately useful for gameplay.
- **T2** (GPU debris) requires: subsystem **A** (jobs), **C** (mesh/VMA buffers), **D** (bindless + multidraw-indirect + GPU culling), and compute-pipeline + SSBO + indirect-draw support. Build the async path but validate the synchronous fallback on the 1080 Ti.
- **T3** (structural connectivity, nested fracture, 1M debris, deterministic GPU sim) after T2 is stable; primarily a 5090 showcase.
