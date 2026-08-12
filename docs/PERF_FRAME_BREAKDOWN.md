# Echo Harbor — THE NAMED FRAME COST BREAKDOWN (re-measured on main)

**Lane 6, replay.** Measured 2026-08-11 against **`origin/main` @ `9d19cb51`** (plus the
two fixes named in §1), branch `lane6/perf-timers-main`. Box: I9DEVPC, 14900K + RTX 5090,
Release build, `--world echotropolis`, windowed, boot camera (crown), 130 s runs,
`X3_PASSDUMP=2` → 65 two-second windows per run.

> ## ⛔ THE PREVIOUS EDITION OF THIS FILE MEASURED AN ENGINE THAT NO LONGER EXISTS
>
> The 2026-08-04 numbers (`CPU 46.42 ms / 21.5 FPS`) were taken on `echotropolis`
> @ `0bc0d482`. Verified by merge-base, that base contains **none** of the engine
> tier: the 132-fold (`21894259`), clustered lights (`badd3a3d`), CSM (`304a6067`),
> geo-LOD (`4292c6d1`) or reflection denoise (`cb948f0c`) — and it is **377 commits
> behind main**. Every millisecond it published is superseded by this file. The old
> table is kept in §4 for the diff and **for no other purpose**.

---

## 0. The one-line answer

> The frame is **CPU-bound by 20.8 ms**. The GPU finishes in **14.7 ms** and then waits —
> `cpu.beginframe`, which *is* the GPU fence wait, is **0.10 ms**. The engine tier bought
> a real **~12.5 ms/frame** versus the old base while the scene got *bigger*
> (35,967 objects drawn, up from 32,524). What it did **not** touch is the shape of the
> problem: the top two costs are still **15.2 ms of host game/AI simulation** and
> **6.8 ms of ray-tracing acceleration-structure rebuild**, and the AS rebuild still
> exists to feed **DDGI alone** — forcing DDGI off takes the frame from **35.3 ms to
> 28.3 ms (28.3 → 35.3 FPS)**.
>
> **Do not kill DDGI.** DDGI's own GPU cost is 0.30 ms. The 7 ms is the engine rebuilding
> a **96,076-instance TLAS every single frame** to feed it. Fix the TLAS, keep the GI. §8.

---

## 1. What this branch is, and two things that had to be fixed to measure at all

This branch is the `lane6/perf-timers` instrumentation (`931c1a2c`, cherry-picked)
replayed onto `origin/main`, plus:

**(a) `origin/main`'s tip does not compile.** `9d19cb51` ("living-city audio") added a
drone-flyby block written against a host-local `drones` vector that the earlier TIER-2
region split (`a99b6ed9`) had already moved into `echo_region_builders.cpp` as the
function-local `dronePoses`. ~16 errors on a pristine checkout. **Only the
`host_echotropolis.cpp` repair hunk** from the sync-validation lane's `772bf11b` was
cherry-picked, so both branches carry the identical fix. **None of `772bf11b`'s
barrier work was taken** — added pipeline-barrier ordering moves frame timings, and
importing it would have made engine-tier effects indistinguishable from barrier effects.

**(b) `kMaxTimedPasses` 48 → 96.** main records more graph passes than the old base and
the clamp in `vk_graph.cpp` was **silent** — an over-cap pass would have vanished from
the breakdown while the "pass sum == frame bracket" invariant appeared to hold. A
one-shot `[perf] TIMED-PASS OVERFLOW` warning now fires if it is ever hit. It was not
hit: 25 passes recorded, and the pass sum still matches the frame bracket to 0.01 ms.

---

## 2. THE BREAKDOWN — echotropolis, crown, steady state, on main

`scene: drawMesh submitted 96,174 · distinct meshes 5,930 · objects drawn 35,967 ·
TLAS instances 96,076 · BLAS 5,858`

All figures are the **p10 across 63 windows** — see §6 on why the median is not
trustworthy on this box.

### CPU — 35.5 ms/frame (28.1 FPS)

| bucket | ms | % frame | what it is |
|---|---:|---:|---|
| **`cpu.host_sim`** | **15.22** | **42.8 %** | **host game simulation: crowd + npcLife schedules + skinned pose-follow + city alert + miners + LLM/ambient chatter poll + tod/atmosphere + ocean + camera + audio listener** |
| **`cpu.rt_as_build`** | **6.75** | **19.0 %** | **BLAS refit + TLAS build, incl. their blocking fence waits** |
| `cpu.preparedata` | 3.94 | 11.1 % | per-frame object SSBO / indirect fill over 96 k records |
| `cpu.host_drawfan` | 3.10 | 8.7 % | host work *between* two `drawMesh` calls (the fan walk) |
| `cpu.endframe_rest` | 2.32 | 6.5 % | endFrame minus its named children |
| `cpu.drawmesh` | 2.07 | 5.8 % | the device's own submission walk, 96,174 calls |
| `cpu.graph_record` | 1.33 | 3.7 % | graph build + all pass record callbacks |
| `cpu.submit_present` | 0.25 | 0.7 % | `vkQueueSubmit2` + `vkQueuePresentKHR` |
| `cpu.beginframe` | 0.10 | 0.3 % | **the GPU fence wait — this is how you know the GPU is not the limiter** |
| `cpu.host_drawfans` | 0.024 | 0.1 % | draw-fan section *residual* (see note) |
| `cpu.host_input` | 0.026 | 0.1 % | poll + console/menu/mode keys + camera rig |
| `cpu.skinpalette` | 0.025 | 0.1 % | bone palette upload, 75 calls |
| `cpu.host_framecap` | 0.005 | 0.0 % | `r_maxfps` sleep — **must be ~0 or the measurement is a lie** |
| `cpu.host_physics` | 0.000 | 0.0 % | `phys->step` — **never runs at the orbit camera** (0 calls/frame) |
| `cpu.host_stream` | 0.000 | 0.0 % | Tier-2 streamer tick — free (M-A boots force-resident) |
| **`cpu.host_outside`** | **0.008** | **0.02 %** | **unattributed residual — was 41 % before this branch** |

Inside `cpu.rt_as_build`: `as.blas_refit` **3.53** · `as.instance_pack` **1.26** ·
`as.tlas_build` **1.94** (sum 6.73 ≈ the 6.75 bucket).

*Note on `cpu.host_drawfans` reading ~0.02 ms:* that is correct, not broken. The whole
draw-fan section costs `drawmesh 2.07 + host_drawfan 3.10 + beginframe 0.10 ≈ 5.3 ms`,
and `DrawScope`'s sub-50 µs gap rule already attributes essentially all of it. The span
zone only reports what the gap rule missed. It is proof the two mechanisms agree.

### GPU — 14.7 ms/frame, and it is idle

| pass | ms | % GPU |
|---|---:|---:|
| main-color | 3.45 | 23.5 % |
| glass | 3.15 | 21.4 % |
| shadow-depth | 3.07 | 20.9 % |
| depth-prepass | 3.06 | 20.8 % |
| skin-compute | 1.49 | 10.2 % |
| ddgi-update | 0.197 | 1.3 % |
| ddgi-rays | 0.104 | 0.7 % |
| *all 18 others* | <0.03 each | <0.5 % total |

`pass gpu sum 14.75 ms vs frame bracket 14.75 ms (delta 0.00)` — that agreement is the
proof the timers are sane.

---

## 3. `cpu.host_outside` IS SPLIT — 41 % → 0.02 %

The old edition's single biggest hole was a 19.05 ms (41 %) unattributed residual. It is
now attributed. The mechanism is **`HostScope`** in `engine/core/x3_cpuzones.h`: an
*exclusive* span that charges `span − (everything already attributed inside it)`, so a
host span wrapping 96,174 `drawMesh` calls does not double-count them and host spans
nest correctly. That is what lets these rows join the leaf partition directly.

**The answer is unglamorous and it is one row: `cpu.host_sim`, 15.2 ms, 43 % of the
frame.** Input is 0.026 ms. Physics is *zero* — it never steps at the orbit camera.
Streaming is *zero*. The freeway of host cost is the living city itself: `residents.update`,
`npcLife.update`, `npcSkin.update`, `residentsSkin.update`, `miners.update`,
`minersSkin.update`, `cityAlert.update`, plus tod/atmosphere/ocean/audio. **The next
perf lane should split `cpu.host_sim` further — the same way this one split
`host_outside` — because at 43 % it is now the whole game.**

### The span boundaries are load-bearing — do not move them casually

`beginFrame()` calls `accumulateCpuZones()` + `frameAccum().reset()`
(`VulkanRenderDevice.cpp:1049`). **That is the accumulator's frame edge.** A host span
that straddles it sees `attributed` snap back to zero, its exclusive subtraction
underflows, and the span silently reports **0.000 ms forever**. The first cut of this
wiring did exactly that and `cpu.host_drawfans` read 0.000 in every window of every run.
`zSim` therefore ends *before* the reset and `zFans` begins *after* it.

Known and accepted: the console / skill-tree / pause-menu modal paths call their own
`beginFrame`+`endFrame` and `continue`, so on those frames `cpu.host_input` straddles a
reset and reports 0. Those frames are a frozen world with a menu open and are never part
of a measurement.

---

## 4. VERSUS THE OLD BASE — what the engine tier actually did

| | 0bc0d482 (2026-08-04) | main (2026-08-11) | delta |
|---|---:|---:|---|
| **frame CPU** | 46.42 ms (21.5 FPS) | **35.5 ms (28.1 FPS)** | **−10.9 ms, +6.6 FPS** |
| frame CPU, instrumentation off | ~45.3 ms | **32.8 ms (30.5 FPS)** | **−12.5 ms** |
| frame GPU | 12.44 ms | **14.7 ms** | **+2.3 ms** |
| host outside the renderer | 19.05 ms | **15.2 ms** | −3.8 ms |
| `cpu.rt_as_build` | 8.20 ms | **6.75 ms** | −1.45 ms |
|  ↳ `as.blas_refit` | 3.78 | 3.53 | −0.25 |
|  ↳ `as.instance_pack` | 2.07 | 1.26 | −0.81 |
|  ↳ `as.tlas_build` | 2.34 | 1.94 | −0.40 |
| `cpu.preparedata` | 6.88 ms | **3.94 ms** | **−2.94 ms** |
| `drawmesh` + `host_drawfan` | 6.50 ms | **5.17 ms** | −1.33 ms |
| `graph_record`+`submit`+`beginframe` | 5.76 ms | **1.68 ms** | **−4.08 ms** |
| drawMesh submitted | 92,583 | **96,174** | **+3,591** |
| objects drawn | 32,524 | **35,967** | **+3,443** |
| TLAS instances | 92,485 | **96,076** | **+3,591** |
| distinct meshes | 6,285 | 5,930 | −355 |

**The engine tier is real and it is worth ~12.5 ms/frame** — and it delivered that
while the scene grew by 3,443 drawn objects. The largest single win is not where anyone
was looking: **`graph_record`+`submit`+`beginframe` fell 5.76 → 1.68 ms** and
**`preparedata` nearly halved**, on *more* records.

The GPU got **more expensive** (+2.3 ms) — CSM and clustered lighting are not free, and
`shadow-depth` (3.07) and `main-color` (3.45) are where it went. This is a *good* trade
that cost nothing real: the GPU had 34 ms of headroom and now has 21.

⚠️ The two columns are separate measurement sessions on the same box weeks apart, so
sub-millisecond deltas (`blas_refit`, `tlas_build`) are inside the noise floor and should
not be over-read. The multi-millisecond ones are far outside it.

---

## 5. THE HYPOTHESIS UNDER TEST — mostly held, with one clean miss

> *"geo-LOD cuts TRIANGLES, not INSTANCE COUNT — so the 8.2 ms AS path (driven by
> 92,485 instances) and the 19.05 ms host_outside should be largely untouched, and the
> frame should still be CPU-bound."*

| claim | verdict | evidence |
|---|---|---|
| geo-LOD cuts triangles, not instances | ✅ **CONFIRMED** | TLAS instances **rose** 92,485 → 96,076; drawMesh calls rose 92,583 → 96,174. Nothing reduced instance count. |
| AS path largely untouched | ✅ **HELD** | 8.20 → 6.75 ms. Still the largest cost inside the renderer, still ~19 % of the frame, still rebuilding the whole TLAS every frame. |
| host time largely untouched | ✅ **HELD** | 19.05 → 15.2 ms. Still the single largest bucket in the game at 43 %. |
| frame still CPU-bound | ✅ **CONFIRMED, emphatically** | CPU 35.5 vs GPU 14.7. `cpu.beginframe` = **0.10 ms**. The GPU has never been the limiter and is further from being it than before. |
| *(implicit)* the engine tier wouldn't move the frame much | ❌ **REFUTED** | −12.5 ms/frame, 21.5 → 30.5 FPS clean. The tier paid — just **not** in the two buckets the hypothesis named. It paid in `graph_record`/`submit` (−4.1 ms) and `preparedata` (−2.9 ms), which nobody was watching. |

**Where the data disagrees, the data wins:** the frame is 27 % faster than the brief
assumed, and the biggest single improvement came from render-graph/submit overhead — a
bucket the old document did not even flag as interesting.

---

## 6. A/B MEASUREMENTS ON THE REAL ENGINE

Every arm: 130 s, 65 windows, identical camera, same binary, instrumentation ON for all
four so the comparison is like-for-like.

| configuration | frame CPU (p10) | GPU | FPS | delta |
|---|---:|---:|---:|---|
| **baseline** | **35.33 ms** | 14.72 | **28.3** | — |
| `X3_FORCE_DDGI=0` | **28.29 ms** | 14.33 | **35.3** | **−7.04 ms · +7.0 FPS** |
| `X3_FORCE_SKINNEDRT=0` | **32.40 ms** | 14.73 | **30.9** | **−2.93 ms · +2.6 FPS** |
| `X3_PASSTIMERS=0` | **32.83 ms** | 14.74 | **30.5** | **−2.50 ms** = the instrumentation |

Confirmation run after the drone repair swap: **35.54 ms** — inside noise of the 35.33
baseline, so the repair changed nothing measurable.

**`X3_FORCE_DDGI=0`.** `cpu.rt_as_build` → **0.00**, TLAS instances → **0**, BLAS → **0**.
DDGI is the only RT consumer enabled in this world, so this measures the whole RT AS path:
**7.0 ms of CPU per frame, 20 % of the frame, for DDGI alone.** The GPU cost of DDGI
itself is `ddgi-update 0.197 + ddgi-rays 0.104 = 0.30 ms`. **The cost is 23× more CPU
bookkeeping than GPU ray tracing.**

**`X3_FORCE_SKINNEDRT=0`.** `as.blas_refit` 3.53 → **0.08 ms**. But `as.instance_pack`
climbs 1.26 → **2.54** (the forced rebuild is gone, so the 96 k-instance signature is
computed again) and `as.tlas_build` holds at 1.86 — **the 96,076-instance TLAS still
rebuilds every frame**, because moving cars/boats/drones change the signature anyway.
Net −2.93 ms (the old base measured −1.6 ms here; the refit got relatively more expensive
as everything around it got cheaper).

**Instrumentation overhead: 2.50 ms (7.1 % of the frame), fully removable** with
`X3_PASSTIMERS=0` or `r_passtimers 0`. It is up from the old base's 1.13 ms because the
frame is now 11 ms shorter while the dominant cost — 2 × `__rdtsc` per `drawMesh`, i.e.
~192,000 reads/frame — is unchanged and there are 3,591 more draws. **Every number in
§2 and §6 is measured *with* the 2.50 ms present**; the honest "shipping" frame time is
**32.8 ms / 30.5 FPS**.

---

## 7. HOW TO USE IT

---

## 7b. M-D FAST BOOT — measured, and it is not what the plan assumed

Nothing was printing a boot total for echotropolis: the `[boot]` marks stop after
device init (~0.96 s) and **every world host's build blocks are unmarked**, so the
"~19 s" figure was folklore. This branch adds one mark on the device's first
`beginFrame`, which is the honest process-start → first-rendered-frame number.

```
[boot] FIRST FRAME (world build complete)   + 19242.9 ms  (t= 20120.5 ms)
```

**19.24 s confirmed.** And:

| finding | number |
|---|---|
| device init (all `[boot]` marks that existed before) | 0.96 s (5 %) |
| **glTF loading** | **17.02 s over 1,294 loads (88 % of boot)** |
| — of which *first* loads | 16.09 s over **877 distinct files**, mean **18.3 ms** |
| — of which redundant re-loads | 0.27 s over 367 loads (trees re-parsed 34–41× each) |
| crowd/npc skinning pools | ~3.3 s (overlaps the above) |
| `ECHO_STREAM=0` vs default | **19.42 s vs 19.24 s — no difference** |

Three consequences that change the M-D plan:

1. **Streaming is not the boot lever it was assumed to be.** The streamer boots
   `M-A force-resident, 18 regions (ECHO_STREAM=on/wired-not-ticking)` — every region
   is built at boot either way, so the rollback switch costs nothing and saves nothing.
   Spawn-region boot only pays off *after* the streamer actually gates residency (M-C),
   which needs the host tick.
2. **The cost is a long tail, not a few hero assets.** The 12 slowest files total 3.8 s;
   the other 865 total 12.3 s. 175 files ≥ 20 ms account for 12.46 s; 702 files < 20 ms
   account for 3.63 s. There is no single asset to fix — the fix has to be structural:
   **load fewer files (spawn-region gating), load them in parallel, or stop parsing
   glTF at boot at all (a baked mesh cache / pak).**
3. **Deduplicating loads is worth only 0.27 s** — do it for hygiene, not for boot time.
   The 6 tree GLBs re-parsed ~38× each are cheap files; the mine forest builds one
   `EnvArtSystem` per tree and each re-parses from disk.
```
X3_PASSDUMP=2        X3Engine.exe --world echotropolis   # log the breakdown every 2 s
X3_PASSTIMERS=0      X3Engine.exe --world echotropolis   # measurement fully off (A/B)
X3_FORCE_DDGI=0 / X3_FORCE_SKINNEDRT=0                   # pin an A/B past the cvar sync
ECHO_AUTOEXIT_SEC=130                                    # bounded run
```
In-game console: `r_passdump`, `r_passtimers 0|1`.

⚠️ `--set` **does not reach `--world` hosts** — the cvar sync hub lives in
`runDefaultHost`, which `--world X` never runs. Use the env vars above, or verify your
cvar actually applied. This is why `X3_FORCE_*` exists.

⚠️ **This box is shared.** Sibling worktrees' engines (`X3Native-tnorm`, `X3Native-wcvars`)
were live for much of this session and inflated the median by up to 60 %. External load
only *adds* frame time, so the **p10 across many short windows** is the undisturbed
estimate; `X3_PASSDUMP=2` over 130 s gives 65 of them. Always check for foreign
`X3Engine.exe` processes — **and never kill one, check its command line first.**

---

## 8. THE REMAINING WORK, IN MEASURED PRIORITY ORDER

1. ~~**`cpu.host_sim` — 15.2 ms (43 %).**~~ **SPLIT AND CLOSED 2026-08-12 —
   see `docs/PERF_HOST_SIM_SPLIT.md`.** It was **not** the living city. **94 % of it
   (14.77 ms) was `sim.opsscreen`**: the crown-plaza ops dashboard re-rasterising a
   1024×1024 RGBA image and creating + destroying a fresh mipped GPU texture **every
   frame** for text that changes a few times per in-game day. Every crowd / npcLife /
   schedule / alert / audio system **combined** costs **0.79 ms**. A one-line
   content-equality guard on `HoloTerminal::setLines` takes the shipping frame from
   **30.61 ms (32.7 FPS) to 21.63 ms (46.2 FPS)** and the frame **stops being
   CPU-bound** (CPU 21.5 vs GPU 15.0). The residual after the split is **0.001 ms**.
2. **The 96,076-instance TLAS rebuilt every frame — 6.8 ms, and it exists for DDGI
   alone.** The right fix is a **static/dynamic TLAS split** (or `MODE_UPDATE` refit) so
   the ~96 k static instances are built once and only the ~15 skinned characters and the
   moving vehicles refit. This is worth most of the 6.8 ms **without losing GI** and is
   the single biggest engine-side win available. Real AS surgery; needs its own gate.
3. **The blocking fence waits inside the AS path.** `as.blas_refit` (3.53) +
   `as.tlas_build` (1.94) = 5.5 ms of the 6.75, and the sync-validation lane has since
   localised the ~6.1 ms stall precisely: `oneTimeSubmit`'s `vkWaitForFences(UINT64_MAX)`
   at `VulkanRenderDevice_internal.h:1793`. **It is a command-buffer LIFETIME constraint**
   (`vkFreeCommandBuffers` follows immediately), not data ordering — so barrier work does
   not relax it. Relaxing it needs a retire-later ring like the upload batch already uses.
   ⚠️ **Replacing that CPU wait with same-queue submission-order assumptions creates a
   hazard class that is invisible without `--vksync`. Gate any such change on
   `--vksync`, never on a plain smoketest.**
4. **`cpu.preparedata` — 3.94 ms.** Nearly halved by the engine tier; still fourth.
5. **96,174 draw submissions for 35,967 drawn objects.** ~63 % of everything submitted is
   culled after the fact. Cutting submissions at the source attacks `host_drawfan` +
   `drawmesh` + `preparedata` + `instance_pack` simultaneously: ~10.4 ms of the frame
   scales with this one number.
6. ~~**GPU work is not a priority at 14.7 ms**~~ — **it is now.** With item 1 closed the
   CPU frame is 21.5 ms against a **15.0 ms GPU**, and the CPU has started *waiting*
   (`cpu.endframe_rest` 2.40 → 5.75 ms, `cpu.rt_as_wait` 0.03 → 0.70 ms). `main-color`,
   `glass`, `shadow-depth`, `depth-prepass` are 3.0–3.5 ms each and 87 % of GPU time
   between them. Everything else is under 0.2 ms.

### The DDGI ruling Tim asked for

**Keep DDGI. Fix the TLAS instead.** The A/B says DDGI costs 7.0 ms/frame — but the
breakdown says only **0.30 ms of that is DDGI**; the other **6.7 ms is the engine
rebuilding a 96 k-instance acceleration structure from scratch, every frame, because
~15 skinned characters moved.** Turning DDGI off buys 7 FPS by deleting the *symptom*
and the entire RT capability with it (shadows, reflections, AO and acoustics all lose
their TLAS the moment anything else wants it). Item 2 buys most of the same 7 FPS and
*keeps* the GI. Killing DDGI is only the right call if item 2 is proven infeasible —
and nothing measured here suggests it is.

---

## 9. INTEGRATION NOTE

Files changed vs `origin/main`: `engine/core/x3_cpuzones.h` (new + `HostScope`),
`engine/rhi/RenderGraph.{h,cpp}`, `engine/rhi/VulkanRT.h`,
`engine/rhi/VulkanRenderDevice.cpp`, `engine/rhi/IRenderDevice.h`,
`engine/rhi/vk/{VulkanRenderDevice_internal.h,vk_graph.cpp,vk_passes.cpp,vk_resources.cpp}`,
`app/hud.cpp` (two console commands), `app/world_hosts/host_echotropolis.cpp`
(the frame-loop spans **and** the `772bf11b` drone repair).

**Merge order note:** this branch and `fix/vk-sync-hazards` both carry the identical
`host_echotropolis.cpp` drone repair hunk; whichever lands second will see it already
applied. No other file overlaps except `vk_graph.cpp` and `VulkanRenderDevice.cpp`, where
this lane touches only timestamp/perf code and that lane touches only barriers.

**Rollbacks:** `X3_PASSTIMERS=0` (env) or `r_passtimers 0` (console) disables all
measurement at a cost of one predictable branch per zone.

---

## 10. GATE

```
Release  --smoketest : exit 0 | [ERROR] lines 0 | VMA live allocationCount=0
Debug    --smoketest : exit 0 | VUID hits 0     | VMA live allocationCount=0
```
**Debug is the only meaningful VUID gate** — `app/main.cpp:626-630` compiles validation
layers out of Release via `#ifdef _DEBUG`, so a Release "0 VUID" proves only that
validation was *absent*. (The sync-validation lane has since made this a runtime flag;
this branch predates that and gates in Debug.) The Debug run emits exactly one `[ERROR]`:
`[stutter] shader module created after first frame (frame 31)` — the pre-existing
`r_strictpso` zero-stutter audit, which defaults to 1 in Debug and 0 in Release. It is not
a VUID and cannot come from this diff, which creates no shader modules or pipelines.

No sibling engine process belonging to this worktree was left running; foreign engines
from other worktrees were checked by command line and **never killed**.
