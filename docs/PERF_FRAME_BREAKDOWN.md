# Echo Harbor — THE NAMED FRAME COST BREAKDOWN

**Lane 6 (streaming & perf).** Authored 2026-08-04 against `echotropolis` @ `0bc0d482`,
branch `lane6/perf-timers`. Box: I9DEVPC, 14900K + RTX 5090, Release build,
`--world echotropolis`, windowed 1280×720, boot camera (crown), ~55 s runs,
each number an average over a 6-second window of frames.

**Every millisecond below is measured on this machine by this branch's
instrumentation.** Where a number contradicts an existing doc, the doc is wrong and
this file says so explicitly.

---

## 0. The one-line answer

> The frame is **CPU-bound by 34 ms**. The GPU finishes in **12.4 ms** and then waits.
> The single largest named cost inside the renderer is the **ray-tracing acceleration
> structure rebuild at 8.2 ms/frame**, which exists to feed **DDGI** — turning DDGI off
> takes the frame from **46.4 ms to 38.1 ms** (21.5 → 26.3 FPS). The next largest is
> **19 ms of host-side game/AI/physics time that never enters the render device at all**.
> Nothing in the draw-submission path is a top-three cost.

---

## 1. Why no one could find the 35 ms before

`engine/rhi/vk/vk_resources.cpp` created the frame's timestamp query pool with
`queryCount = 2` — **one stamp at frame start, one at frame end, and nothing else in
the entire engine.** `m_cullGpuMs`, `m_hzbGpuMs` and `m_cullCpuMs` were declared with
the comment `(0 = not measured)` and were **never assigned**; the HUD has printed
`cull 0.00/0.00` on every machine that has ever run this engine.

The consequence, first stated in `docs/VULKAN_ROADMAP.md` §1.2 and confirmed here:
**every per-pass millisecond quoted anywhere in this repository is a whole-frame
delta, not a pass cost.** There was no instrument capable of finding the 35 ms, so it
was never found. `docs/RENDERING_SPEED.md:62-64` called per-pass GPU timestamps
"non-negotiable"; `docs/plans/SESSION_LANES.md:57` re-ordered them. They had never
been built until this branch.

---

## 2. What this branch measures

### 2.1 GPU — real per-pass timestamp pairs
The query pool is now `2 + 2 × kMaxTimedPasses` (98) queries. `RenderGraph::execute`
brackets **every graph pass** with `vkCmdWriteTimestamp2(ALL_COMMANDS)` pairs. Results
are read back when the ring slot's fence retires (`kFramesInFlight` frames later) —
**no stall**, and read with `WITH_AVAILABILITY` so one unresolved query cannot poison
the batch. `m_cullGpuMs` / `m_hzbGpuMs` are assigned from the `gpu-cull` / `hzb-build`
pass pairs.

*Honest caveat:* both stamps use `ALL_COMMANDS`, which makes the per-pass durations
non-overlapping (their sum equals the frame bracket to 0.01 ms — see the tables below,
that agreement is the proof the timers are sane) at the cost of discouraging some
inter-pass overlap. `X3_PASSTIMERS=0` removes the whole apparatus for the A/B; see §6.

### 2.2 CPU — rdtsc zones, and a trick to see host cost without touching host files
`engine/core/x3_cpuzones.h` (new, header-only) buckets CPU time with `__rdtsc`
(~10 cycles, vs ~25 ns for QueryPerformanceCounter — which matters when the hot zone
fires 92,583 times a frame). Zones cover every render-device entry point.

The interesting one is `cpu.host_drawfan`. The device cannot see what the host does
*between* two `drawMesh` calls — but the **gap** between one returning and the next
entering measures it exactly. Gaps under 50 µs are charged to the draw fan; longer
gaps are not draw-fan work and fall into the residual `cpu.host_outside`. This
attributes host cost **without editing a single host file** (Lane 7 owns
`host_echotropolis.cpp`).

### 2.3 How to use it
```
X3_PASSDUMP=6   EchoHarbor.exe --world echotropolis    # log the breakdown every 6 s
X3_PASSTIMERS=0 EchoHarbor.exe --world echotropolis    # measurement fully off (A/B)
X3_FORCE_SKINNEDRT=0 / X3_FORCE_DDGI=0                 # pin an A/B past the host cvar sync
```
In-game console: `r_passdump` (log it now), `r_passtimers 0|1`.

---

## 3. THE BREAKDOWN — echotropolis, crown, steady state

```
scene: drawMesh submitted 92583   distinct meshes 6285   objects drawn 32524
       TLAS instances 92485       BLAS 6213
```

### CPU — 46.42 ms/frame (21.5 FPS)

| bucket | ms | % | note |
|---|---:|---:|---|
| `cpu.host_outside` | **19.05** | 41.0 % | game tick / physics / AI / streaming — **never enters the render device** |
| `cpu.rt_as_build` | **8.20** | 17.7 % | RT acceleration structures (breakdown below) |
| `cpu.preparedata` | **6.88** | 14.8 % | camera/light UBO + object SSBO + indirect fill |
| `cpu.host_drawfan` | 3.71 | 8.0 % | host work *between* draw calls (92,581 gaps) |
| `cpu.drawmesh` | 2.79 | 6.0 % | the submission walk itself, 92,583 calls |
| `cpu.graph_record` | 2.75 | 5.9 % | graph build + all pass record callbacks |
| `cpu.endframe_rest` | 2.56 | 5.5 % | endFrame minus its named children |
| `cpu.submit_present` | 0.35 | 0.8 % | |
| `cpu.beginframe` | 0.10 | 0.2 % | ← **GPU back-pressure. It is ~zero. The GPU is never the limiter.** |
| `cpu.hud` / `cpu.skinpalette` | 0.04 | 0.1 % | |

Inside `cpu.rt_as_build`: `as.blas_refit` **3.78**, `as.instance_pack` **2.07**,
`as.tlas_build` **2.34`.

### GPU — 12.44 ms/frame

| pass | ms | % |
|---|---:|---:|
| `main-color` | 2.95 | 23.0 % |
| `glass` | 2.66 | 20.8 % |
| `shadow-depth` | 2.60 | 20.3 % |
| `depth-prepass` | 2.46 | 19.9 % |
| `skin-compute` | 1.50 | 12.2 % |
| `ddgi-update` | 0.20 | 1.6 % |
| `ddgi-rays` | 0.10 | 0.8 % |
| everything else (16 passes) | < 0.03 each | ~1 % total |

`pass gpu sum 12.44 ms vs frame bracket 12.44 ms (delta 0.00)` — the passes account
for the whole frame; there is no hidden GPU time.

---

## 4. WHAT THE DOCS GOT WRONG

| claim | source | measured here |
|---|---|---|
| "steady state = **~2.0–2.6 ms** for 33 REFITS incl. the fence wait" | comment in `vk_passes.cpp` (buildRtSceneAS), quoted by `VULKAN_ROADMAP.md` §2.1 | **10.19 ms** for the whole AS path before this branch's fixes; **8.20 ms** after. The BLAS refit alone is 3.78 ms. **The comment understates the real cost by ~4×** — it measured only the refit sub-step, not the path. |
| CPU p50 **72.91 ms** vs GPU p50 **45.05 ms** (CPU 1.62× GPU) | `ZERO_STUTTER.md:166` | Direction confirmed, magnitude very different **in this world**: CPU 46.4 ms vs GPU **12.4 ms** — CPU is **3.7×** GPU. The GPU is not close to the limiter here. (ZERO_STUTTER's run was `canonlevel` with the full RT screen stack; it is not the same scene, and should not be quoted for echotropolis.) |
| "CPU cost … is **dominated by the immediate-mode `drawMesh()` submission walk**" | `docs/screenshots/gpucull/RESULTS.md:46-54` | **False for this world.** `drawMesh` is 2.79 ms = **6.0 %** of the frame. Even adding the host's between-draw work (3.71 ms) the whole draw fan is 6.5 ms = 14 %. It is the 4th-largest cost, not the dominant one. |
| The residual ~33 ms is districts / woodlands / the crown | disproved by commit `498b6f62` | Confirmed disproved, and now explained: those are *draw* costs, and drawing is not where the time goes. |

---

## 5. WHAT WAS FIXED (measured, this branch)

Two surgical CPU fixes in the AS path. Control: `as.blas_refit`, which neither fix
touches, moved 3.834 → 3.782 ms (−1.4 %, run-to-run noise) across the same two runs —
so the deltas below are attribution, not drift.

1. **Stop hashing 92,485 instances to answer a question already answered.**
   `tlasSignature()` (18 FNV mixes per instance) exists only to decide "can we skip the
   rebuild?". A skinned BLAS refit happens **every frame** with characters on screen,
   which already forces the rebuild — so the signature was pure waste. Now skipped when
   the rebuild is forced (0 stored as an "unknown" sentinel; the first genuinely static
   frame afterwards recomputes, rebuilds once, and settles back onto the fast path).
2. **Resolve each mesh's BLAS address once, not four times.** The instance repack did
   `hasBlas()` + `hasSkinnedBlas()` — four `unordered_map` probes — for each of ~92,000
   draw records, and `buildTlas` then repeated two of them. Draw records arrive in runs
   of the same mesh, so a one-entry memo collapses nearly all of it, and the resolved
   address now rides in `TlasInstance::blasAddr` so `buildTlas` never looks up again.

| bucket | before | after | delta |
|---|---:|---:|---:|
| `as.instance_pack` | 3.697 ms | **2.074 ms** | **−1.62 ms** |
| `as.tlas_build` | 2.653 ms | **2.338 ms** | −0.32 ms |
| `cpu.rt_as_build` | 10.188 ms | **8.198 ms** | **−1.99 ms** |
| frame CPU | 48.81 ms | **46.42 ms** | −2.39 ms (20.5 → 21.5 FPS) |

---

## 6. A/B MEASUREMENTS (now possible for the first time)

| configuration | frame CPU | GPU | FPS | what it tells us |
|---|---:|---:|---:|---|
| baseline (this branch) | 46.42 ms | 12.44 | 21.5 | |
| `X3_PASSTIMERS=0` | — | — | — | host frame-time median **45.15 vs 46.28 ms** → the entire instrumentation costs **1.13 ms (2.4 %)** and is switchable off |
| `X3_FORCE_SKINNEDRT=0` | 44.86 ms | 12.54 | 22.3 | `as.blas_refit` 3.78 → **0.17 ms**. But `as.instance_pack` climbs back to 3.63 (the forced rebuild is gone so the signature is computed again) and `as.tlas_build` stays 2.63 — **the 92 k-instance TLAS still rebuilds every frame** because moving cars/boats/drones change the signature. Net −1.6 ms only. |
| `X3_FORCE_DDGI=0` | **38.05 ms** | 12.02 | **26.3** | `cpu.rt_as_build` → **0.00 ms**, TLAS instances → 0. DDGI is the only RT consumer enabled in this world, so this measures **the entire RT AS path: 8.2 ms of CPU per frame, 18 % of the frame, for DDGI alone.** |

---

## 7. THE REMAINING WORK, IN MEASURED PRIORITY ORDER

1. **`cpu.host_outside` — 19.05 ms (41 %).** The largest single cost in the game and it
   is entirely outside the renderer: game tick, physics, npcLife/crowd AI, world cars,
   streaming. **Not attributable further without instrumenting the host loop**, which is
   `app/world_hosts/host_echotropolis.cpp` (Lane 7's file). `X3_CPU_ZONE` is ready for
   it — see the integration note. This is where the next perf lane must go.
2. **The 92,485-instance TLAS rebuilt every frame — 8.2 ms.** The engine rebuilds the
   *entire* top-level structure because ~15 skinned characters refit. The right fix is a
   **static/dynamic TLAS split** (or `MODE_UPDATE` refit) so the 92 k static instances are
   built once. This alone is worth most of the 8.2 ms and is the single biggest engine-side
   win available. Not attempted here — it is real AS surgery and needs its own gate.
3. **Un-block the two per-frame `vkWaitForFences(UINT64_MAX)`** (`VulkanRT.h`
   `endBlasBatch` + `oneTimeSubmit`). Now measured: the two blocking round-trips are
   ~6.1 ms of the 8.2 ms AS cost. `VULKAN_ROADMAP.md` §5.2 proposes a timeline semaphore;
   a safer intermediate is to record the BLAS batch and the TLAS build into **one**
   command buffer with a `vkCmdPipelineBarrier2` between them, halving the round-trips.
   **Measured, not fixed.**
4. **`cpu.preparedata` — 6.88 ms.** The per-frame object SSBO / indirect fill over 92 k
   records. Never named before. Untouched.
5. **92,583 draw submissions for 32,524 drawn objects.** ~65 % of everything submitted is
   culled after the fact. Cutting submissions at the source (the host's draw fans, or
   real streaming residency — M-C) attacks `cpu.host_drawfan` + `cpu.drawmesh` +
   `cpu.preparedata` + `as.instance_pack` **simultaneously**: ~15 ms of the frame scales
   with this one number.
6. GPU work is not a priority at 12.4 ms. If it ever becomes one, the four passes worth
   looking at are `main-color`, `glass`, `shadow-depth`, `depth-prepass` — 2.5–3 ms each,
   84 % of GPU time between them. Everything else is under 0.2 ms and not worth touching.

---

## 8. INTEGRATION NOTE (for Lane 7 / the integrator)

**This branch touches NO host file and NO CMakeLists.** `host_echotropolis.cpp` and
`app/CMakeLists.txt` are untouched; the one new file (`engine/core/x3_cpuzones.h`) is
**header-only**, so `engine/CMakeLists.txt` needs no change either. It merges clean.

Files changed: `engine/core/x3_cpuzones.h` (new), `engine/rhi/RenderGraph.{h,cpp}`,
`engine/rhi/VulkanRT.h`, `engine/rhi/VulkanRenderDevice.cpp`, `engine/rhi/IRenderDevice.h`,
`engine/rhi/vk/{VulkanRenderDevice_internal.h,vk_graph.cpp,vk_passes.cpp,vk_resources.cpp}`,
`app/hud.cpp` (two console commands only).

**Optional one-line wiring — the only thing that needs a host edit.** To split the
19 ms `cpu.host_outside` bucket, add zones to the echotropolis frame loop
(`host_echotropolis.cpp`, the `while (!glfwWindowShouldClose(...))` at ~line 3216):

```cpp
#include "engine/core/x3_cpuzones.h"
...
{ X3_CPU_ZONE(Z_HostGame);    game.tick(...); }
{ X3_CPU_ZONE(Z_HostPhysics); physics->step(...); }
{ X3_CPU_ZONE(Z_HostAi);      npcLife.update(...); worldCars.update(...); }
{ X3_CPU_ZONE(Z_HostDrawFans); /* the EnvArtSystem ->draw() fan block */ }
```
Add the matching ids to the `Zone` enum + `zoneName()` in `x3_cpuzones.h` and to the
`leaves[]` array in `VulkanRenderDevice::logPerfBreakdown`. Nothing else changes; the
residual row shrinks by exactly what the new zones capture.

**Rollbacks:** `X3_PASSTIMERS=0` (env) or `r_passtimers 0` (console) disables all
measurement at a cost of one predictable branch per zone. `ECHO_STREAM=0` and
`ECHO_ISLAND_DIR` are unaffected.

---

## 9. GATE

```
Release  --smoketest : exit 0 | VUID hits 0 | [ERROR] lines 0 | VMA live allocationCount=0
Debug    --smoketest : exit 0 | VUID hits 0 | VMA live allocationCount=0
```
Debug is the meaningful VUID gate — `app/main.cpp:598` compiles validation layers **out
of Release**, so a Release "0 VUID" proves nothing. The Debug run emits exactly one
`[ERROR]`: `[stutter] shader module created after first frame (frame 31)`. That is the
pre-existing `r_strictpso` zero-stutter audit, which **defaults to 1 in Debug and 0 in
Release** (`app/app_run.cpp:567-569`) — it is not a VUID and cannot come from this diff,
which creates no shader modules, pipelines or descriptor pools.

No sibling engine process was live during any build or run.
