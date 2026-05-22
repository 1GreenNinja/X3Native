# X3Native — idTech 8-Aligned Architecture Roadmap

**Status:** living document. Started 2026-05-20.
**Purpose:** X3Native targets the architectural philosophy of **id Tech 8** (Doom: The Dark Ages) — same stack as us (**C++ + Vulkan**) — as its north star. This doc records the decisions, the subsystem decomposition, the build sequence, and the per-machine hardware constraints so every session (and every parallel agent) shares one plan.

> **Clean-room note:** every idTech 8 reference here comes from **public** material — id Software interviews, Tiago Sousa's SIGGRAPH 2025 "Fast as Hell" GI talk, GDC talks, the Vulkan spec, public papers. None of it is RBDOOM/GPL source. Studying public architecture talks is exactly what the clean-room protocol (`specs/README.md`) permits.

---

## 1. The north star (idTech 8 pillars)

1. **Performance above all — predictable frame times.** Stable ≥60 FPS. No main thread; *everything* is jobs/tasks. Homogeneous per-frame workloads to avoid bubbles/variance. Minimal shader variants. Cache-friendly data layouts. Near-zero stutter; 2–4 s loads.
2. **Fully ray-traced lighting (RT-first).** Real-time RT GI + reflections as the foundation; path tracing post-launch. Sousa's hybrid "Fast as Hell" GI (cascaded light grids + irradiance volumes) hits cinematic quality at 60 Hz — WYSIWYG, no bake times.
3. **Virtualized / highly scalable geometry + streaming.** Auto vista LODs, contribution culling, GPU grass, heavy background streaming. Levels 5–10× larger than Eternal.
4. **Destructible GPU physics.** Scene graph on the GPU frees the CPU for AI/gameplay. Reactive destruction (craters, splits, debris) with smart sleep/wake persistence. Massive on-screen counts via GPU animation/ragdolls.
5. **Vulkan leveraged hard.** Low driver overhead, explicit command/memory control, async compute, one renderer scaling console→PC.

See `RENDERING_SPEED.md` for the concrete frame-rate technique stack (bindless, multidraw-indirect, GPU-driven culling, etc.) — it already converges with pillars 1/3/5.

---

## 2. Hardware reality (this is why we diverge from idTech 8 in two places)

| Rig | CPU | GPU | RT cores | Role |
|---|---|---|---|---|
| **13700K** (this clean-room box) | i7-13700K, 128GB | 2× GTX 1080 Ti (Pascal) | **none** | Clean-room engine build + GPU **compute** work |
| **14900K** | i9-14900K | RTX 5090 (Blackwell) | yes | RT/PT high-end tier + spec authoring + verification |
| A2000 laptop | — | RTX A2000 | yes | Original D1 verification |

**Two consequences:**
- **RT is NOT a hard requirement for us.** The 1080 Ti has zero hardware ray tracing (no `VK_KHR_acceleration_structure`). Copying idTech 8's "RT required, no fallback" stance would orphan this dev box and most players. → **RT becomes a high-end *tier*, not the foundation** (see Decision D-RT).
- **GPU *compute* is fully available on the 1080 Ti** (~11 TFLOPS FP32, SSBO atomics). So GPU destruction physics (pillar 4) *can* be developed and tested here. **Caveat:** Pascal's async-compute *overlap* is weak (preemption-based) — design the async path but expect the real overlap win only on the 5090.

---

## 3. Locked decisions (ADR-style)

- **D-RT — Lighting is hybrid, gated by hardware.** GPU-driven *raster* path is the foundation (runs everywhere incl. 1080 Ti). GI is hybrid: a **compute-based irradiance/probe GI** (Sousa-style cascaded grids — runs on the 1080 Ti, no RT cores) + a **hardware-RT reflections/PT tier** enabled only on RT-capable GPUs (5090). Not RT-required.
- **D-JOB — One engine-wide job system; hand-rolled fiber scheduler.** idTech 8's "everything is tasks." Physics, render command recording, and streaming all submit to ONE scheduler (no competing pools → no frame bubbles). Implementation: hand-rolled **fiber-based** scheduler (Naughty Dog "Parallelizing the Naughty Dog Engine," GDC 2015), behind `IJobSystem` so it's swappable. *(Chosen over a plain work-stealing pool and over adopting enkiTS/Taskflow. Highest-risk option — firewalled behind the interface; fall back to a pool if it fights us.)*
- **D-PHYS — Hybrid physics: Jolt (CPU, authoritative) + GPU compute (visual debris).** Jolt handles gameplay-critical bodies (player, AI-relevant, anything raycast/queried). A separate GPU-compute world handles high-volume visual-only debris. **One-way coupling:** gameplay/explosions push impulses *in*; debris does not feed gameplay queries back (or only via cheap coarse readback). We do NOT try to make Jolt do mega-scale destruction, and we do NOT move authoritative sim to the GPU.
- **D-GEO — CSG authoring → mesh bake → GPU-culled runtime; no runtime BSP.** (Already recorded in `LEVEL_GEOMETRY.md`.)
- **D-FAST — The fast path is the default path.** Bindless + multidraw-indirect + GPU-driven culling are built in from the start, not bolted on. (Already in `RENDERING_SPEED.md`.)

---

## 4. Subsystem decomposition

Each is its own spec → plan → build cycle. Status: ✅ done · 🔜 next · ⛔ blocked (dependency).

| ID | Subsystem | Status | Depends on | Hardware notes |
|---|---|---|---|---|
| **A** | **Job system** (`IJobSystem`, fiber) | 🔜 **designing now** | — | CPU-only; the spine. Bridges Jolt's pool (Slice 41). |
| **B** | Frame/render graph (declarative passes, auto-barriers, async-compute) | ⛔ after A + render core | A | — |
| **C** | Mesh + PBR material pipeline + VMA buffers (D2) | 🔜 (current renderer next-step) | — | runs on 1080 Ti |
| **D** | Bindless + multidraw-indirect + GPU-driven culling | ⛔ after C | C, A | descriptorIndexing already on (D1) |
| **E** | Shadows (D3) + depth pre-pass | ⛔ after C/D | C | — |
| **F** | Compute irradiance/probe GI (Sousa hybrid) | ⛔ after C/D | C, D | **runs on 1080 Ti (compute, no RT)** |
| **G** | Hardware-RT reflections / path-tracing tier | ⛔ after F | F | **5090 only** (no 1080 Ti) |
| **H** | Streaming + LOD (vista LODs, contribution culling) | ⛔ after D | D, A | I/O on the job system's I/O pool |
| **I** | CSG brush + patch editor → mesh/collision bake (M8) | ⛔ later | C, M3 | — |
| **J** | GPU-skinned animation + ragdolls (D8) | ⛔ after C/D | C, D | glTF skins from M2 feed this |
| **K** | **GPU destruction physics** (compute debris world) | ⛔ after render core | C, D, A, M3 | **compute on 1080 Ti OK; async overlap weak on Pascal** |
| — | M3 Jolt physics (CPU authoritative) | ✅ done | — | single-precision port (see Open Decisions) |
| — | M2 glTF/GLB loader | ✅ done | D5 | GPU upload deferred (opaque-handle seam) |
| — | KTX2 bake tool (`tools/ktx2bake`) | ✅ done | — | toktx v4.4.2 |

---

## 5. Subsystem K — GPU destruction physics (seed spec)

Captures the design fed in 2026-05-20 (idTech 8 GPU offload). Detailed compute-shader code goes into `specs/K-gpu-destruction.spec.md` when K is reached; this is the architecture so nothing is lost.

- **Two-world hybrid (per D-PHYS):** Jolt = authoritative; GPU = visual debris pool. Pre-allocated SSBO pool of "dead" bodies; CPU writes new debris via a staging buffer.
- **Data layout:** Structure-of-Arrays in `STORAGE_BUFFER` / `DEVICE_LOCAL` SSBOs (position+invMass, quat rotation, linear/angular velocity, inertia, flags, sleepCounter, materialID). Batch like-material debris into the same dispatch for warp coherence.
- **Compute pipeline (per fixed step, barriers between):**
  1. **Broad-phase:** uniform grid + spatial hash (cell ≈ largest debris AABB; aim 5–20 objects/cell). Two-phase insert: count (atomicAdd) → prefix-sum → scatter. Pair gen tests cell+neighbors, `idA<idB` dedup.
  2. **Narrow-phase + impulse resolution** (sphere/box/convex contacts).
  3. **Integration:** semi-implicit Euler, quaternion angular update, gravity/damping.
  4. **Sleep/wake:** per-body sleep counter; below vel threshold for N frames → sleep (skip integration, keep in broad-phase); wake on impulse/contact. *This is how persistent debris stays cheap.*
- **Fracture spawn:** pre-fractured meshes authored offline (Blender/Houdini) → convex chunks + local offsets/mass. On break: Jolt parent deactivates; spawn child bodies (Jolt for large pieces, GPU pool for small) with split linear/angular impulse from the impact point.
- **Render coupling:** debris rendered via **indirect draw** + instancing; GPU LOD switching to limit VRAM.
- **Sync:** async-compute queue + **timeline semaphores** (compute physics → vertex/indirect read). On Pascal expect limited overlap; full benefit on the 5090.

---

## 6. Comparison findings — references vs. shipped M3 (2026-05-20)

The pasted Jolt/GPU references checked against the M3 implementation (`d830dfe`):

- ✅ **Matches best practice:** init sequence (`RegisterDefaultAllocator`→`Factory`→`RegisterTypes`→`PhysicsSystem::Init`→`JobSystemThreadPool`), custom broadphase-layer interface + filters + collision matrix, fixed 1/60 accumulator (encapsulated inside `step()`), `CharacterVirtual`, static-mesh/box/sphere, layer-masked raycasts, `ContactListener` triggers. 8/8 tests pass.
- ⚠️ **Gaps / decisions** (tracked in §7): single vs double precision; Jolt uses its own pool today (must bridge to A); fracture needs compound + convex-hull shapes added to `IPhysicsWorld`.

---

## 7. Open decisions (need a call before the relevant subsystem)

1. **Jolt precision (before large-world content).** vcpkg `joltphysics` is **single-precision**; idTech-scale worlds (~30,000 units; Babylon ran `WORLD_RADIUS=15000`) jitter far from origin. Options: (a) switch port to `USE_DOUBLE_PRECISION` (perf cost), or (b) keep single + **camera-relative origin rebasing** (cheaper; renderer wants it anyway). *Lean: (b).*
2. **`IPhysicsWorld` shape additions (before destruction).** Add `addConvexHull` + compound-shape support so pre-fractured destruction (and richer collision) works. Currently only box/sphere/static-mesh.
3. **Job-system bridge timing (Slice 41).** When A lands, bridge `JPH::JobSystem` onto it so physics stops running its own pool.

---

## 8. Build sequence (near-term)

1. **A — Job system** (designing now) → spec → plan → build. Bridge Jolt (Slice 41).
2. **C — Mesh + PBR + VMA buffers** (renderer core) — also unblocks M2's real GPU upload.
3. **D — Bindless + multidraw + GPU culling** on top of C.
4. **B — Frame graph** to formalize passes + async compute.
5. Then the multipliers: **E** shadows, **F** compute GI, **H** streaming, **K** GPU destruction, **G** RT tier (5090), **J** animation, **I** editor.
