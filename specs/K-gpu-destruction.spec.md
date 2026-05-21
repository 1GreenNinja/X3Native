# Spec: GPU Destruction Physics + Hybrid Jolt Handoff (K) — SEED

> **Status: SEED** (captured 2026-05-20 from Tim's reference dumps). Finalize when K is reached — it is **downstream of the renderer core** (needs compute pipelines, SSBOs, a buffer/texture API, and indirect draw) and is **priority #4** (after the fiber job system, the GPU-driven renderer, and streaming). Captured now so the design + references aren't lost.
>
> Clean-room: built from public references (Jorrit Rouwe's GDC 2022 "Architecting Jolt Physics for Horizon Forbidden West", Erin Catto's Sequential-Impulse GDC talks, Harada "Real-Time Rigid Body Simulation on GPUs" GPU Gems 3, Tiago Sousa SIGGRAPH 2025, the Vulkan spec) + the permissive Jolt library. No RBDOOM/id Tech source.

## 1. Architecture — hybrid CPU(Jolt) + GPU(compute), per roadmap **D-PHYS**

Two worlds, one-way coupling:
- **Jolt (CPU, authoritative):** gameplay-critical bodies — player, AI-relevant bodies, anything raycast/queried, **and the destruction *event*** (the intact object + its fracture into convex child pieces). Deterministic, correct.
- **GPU compute (visual debris):** the thousands of small/resting fragments that result. Lives entirely in SSBOs, simulated in compute, drawn via indirect draw.
- **Coupling:** gameplay/explosions push impulses *in*; large pieces stay in Jolt; **small/sleeping pieces hand off to the GPU pool**; a big GPU-detected event can wake a piece back into Jolt. Debris does **not** feed gameplay queries back (or only via cheap coarse readback).

## 2. Jolt (CPU) side

### 2a. DestructibleManager
- `DestructibleObject { parentBodyID; vector<FracturePiece>; isBroken; health/material; }` where `FracturePiece { shapeSettings (ConvexHull); localOffset; localRotation; mass; comOverride; }`.
- **Intact body** = one `Dynamic` body with a `StaticCompoundShape` built from the pieces (fast tree). `MutableCompoundShape` only if progressive add/remove is needed.
- Lookup `GetDestructible(BodyID)` via `unordered_map<BodyID, DestructibleObject>`; prefer `Body::mUserData` marker for fast "is destructible?" checks.

### 2b. Break detection — `ContactListener`
- `OnContactAdded` / `OnContactPersisted`: estimate impulse (`approachSpeed * min(mass1,mass2)` along the manifold normal); compare to break thresholds (impulse + relative-velocity).
- **CRITICAL:** never mutate physics state (add/remove/fracture) inside the contact callback — bodies are locked. **Queue** fracture requests (thread-safe), then `ProcessPendingFractures()` once per step **after** `PhysicsSystem::Update()`.
- Also support **non-contact breaks**: weapon raycast/shapecast hits + explosion radius → enqueue fracture directly.

### 2c. Fracture spawning
- On break: read parent transform + linear/angular velocity, **remove the parent**, spawn each child as a `Dynamic` body at `parentPos + parentRot*localOffset`. Child velocity = parent linear + impact·k + radial-from-impact·(breakImpulse/mass); child angular = parentAng + (chunkPos−parentPos)×impactVel·k. Cap concurrent active pieces per object.

### 2d. Solver + broadphase (tuning that matters at high counts)
- Jolt = **Sequential-Impulse (PGS)**: velocity solver (warm-started, ~4–10 iters) then a Baumgarte position solver (~2–4 iters), **island-based** (independent islands → parallel). Fractured children join islands automatically.
- Settings exposed at init: `mNumVelocitySteps`, `mNumPositionSteps`, `mBaumgarte`, `mPenetrationSlop`, `mSpeculativeContactDistance`. Raise velocity steps temporarily under heavy destruction.
- Broadphase = **quadtree**, double-buffered/lock-free (queries run parallel to Update). **Batch** body add/remove (`AddBodiesPrepare/Finalize`); call `OptimizeBroadPhase()` after big streaming/destruction churn.
- `6DOF` constraints suit **progressive breaking** (start limited, relax limits on damage).

## 3. GPU (compute) side
- **SoA SSBOs**, `DEVICE_LOCAL`: position+invMass, quat rotation, linear/angular velocity, inertia, flags, sleepCounter, materialID. Batch like-material debris per dispatch for warp coherence.
- **Pipeline per fixed step (barriers between):** (1) broadphase — uniform grid + spatial hash, two-phase insert (count `atomicAdd` → prefix-sum → scatter), pair-gen tests cell+neighbors with `idA<idB` dedup; (2) narrow-phase + impulse resolution; (3) integrate (semi-implicit Euler + quaternion); (4) sleep/wake (sleepCounter; below-threshold N frames → sleep, keep in broadphase; wake on impulse/contact).
- **Render:** indirect draw + instancing; GPU LOD to bound VRAM.
- **Sync:** async-compute queue + timeline semaphores (compute → vertex/indirect read). **Pascal (1080 Ti) caveat:** compute works but async *overlap* is weak; full overlap benefit on the RTX 50-series.

## 4. CPU↔GPU handoff
- After Jolt `Update()`: small/sleeping fragments → upload to the GPU pool (pre-allocated "dead" slots; CPU writes via a staging buffer), remove from Jolt to keep island/solver cost bounded.
- On a high-impact GPU event near gameplay → coarse readback → re-spawn a Jolt body.

## 5. Required `IPhysicsWorld` extensions (not present today)
The current clean interface only has box/sphere/static-mesh + position get/set + raycast + simple triggers. K needs:
- Shapes: `addConvexHull(points...)`, `addCompound(parts...)` (+ a fracture-asset representation).
- Motion: kinematic motion type; `setBodyVelocity` / `getBodyLinearVelocity` / `getBodyAngularVelocity`; `getBodyRotation` / `setBodyRotation` (also fixes the door-rotation + monster-facing gaps elsewhere).
- Events: a clean **contact/break callback** (queue-based, fired post-step), and body **user-data**.
- Maintenance: `optimizeBroadphase()`, batched add/remove, and solver-tuning fields on the init settings.
- All additions keep `JPH::` types out of the header (opaque handles + plain structs), per the existing convention.

## 6. Acceptance tests (future)
1. Authored fracture asset: on impact above threshold, parent removed + N convex children spawned with split linear/angular velocity; below threshold, no break.
2. Contact-callback safety: fractures are queued and applied post-step (no mutation in the locked callback).
3. GPU debris: thousands of fragments simulate + sleep/wake at the open-world target FPS on the RTX 50-series.
4. Handoff: small Jolt pieces transfer to the GPU pool (Jolt island count drops); a big event re-spawns a Jolt body.
5. Determinism of the Jolt side (fixed-step) preserved.

## 7. Tuning knobs (start values, tune to mass scale)
Break impulse threshold ~15, break velocity ~8, child impact factor ~0.4, radial impulse ÷mass, angular ×~8; velocity steps 10 / position steps 4 / baumgarte 0.2 / slop 0.02 under heavy load; GPU cell size ≈ largest debris AABB (aim 5–20 objects/cell); sleep velocity threshold + N frames.
