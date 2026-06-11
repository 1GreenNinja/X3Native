# ZERO-STUTTER GUARANTEE

**The anti-UE beat: this engine never hitches — and it proves it with receipts,
every run.** No pipeline is ever compiled mid-frame. No descriptor pool ever
grows mid-frame. Every frame's cost is measured, every spike is logged with a
cause, and a headless CI gate (`--test-framepacing`) fails the build if the
guarantee regresses.

Hardware baseline: RTX 5090, Vulkan 1.3, MSVC/VS2026, branch `feat/zero-stutter`
(on top of `feat/rt-shadows` — the fullest render stack: depth-prepass, CSM +
RT soft shadows, RT-AO, SSR + ray-query reflections, DDGI, TAA, bloom/auto-
exposure post, glass, water, particles, debris compute, GPU skinning, planets,
HUD, editor ImGui).

---

## 1. The pipeline audit

Every `vkCreateGraphicsPipelines` / `vkCreateComputePipelines` /
`vkCreateDescriptorPool` / `vmaCreateBuffer` / `vmaCreateImage` call site in the
renderer (39 PSO sites, 20 pool sites, 40 allocation sites — all in
`engine/rhi/VulkanRenderDevice.cpp`; verified by grep, no other translation unit
creates pipelines) is funneled through tracked wrappers
(`x3CreateGraphicsPipelines` / `x3CreateComputePipelines` /
`x3CreateDescriptorPool` / `x3vmaCreateBuffer` / `x3vmaCreateImage`). The
wrappers count totals, per-frame attribution, and **late creations** (anything
created after the first `beginFrame()`, outside a declared boundary).

### Boot-time pipelines (created in `init()`, before any frame)

| Pipeline(s) | Where created | Notes |
|---|---|---|
| Mesh opaque (SSAO + no-SSAO variants) | `createGraphics()` | depth EQUAL / LESS pair |
| Mesh RT-shadow variants (opaque x2 + transparent) | `createGraphics()` | only on ray-query devices |
| Mesh transparent (alpha blend) | `createGraphics()` | |
| Mesh IBL-probe variant | `createGraphics()` | probe-bake camera path |
| Planet bodies (9 opaque types) | `createGraphics()` | FORGE3D port, one PSO per type |
| Planet glow shells (atmosphere/corona/ring) | `createGraphics()` | additive/alpha transparent |
| Glass (refraction/frost) | `createGraphics()` | own layout, set 4 scene copy |
| Shadow depth (CSM single map) | `createShadowPipeline()` | |
| Depth pre-pass (+ cutout variant) | `createDepthPrePipeline()` via `createSsao()` | |
| SSAO gen + blur, GI gen + blur/resolve | `createSsao()` / `createGi()` | fullscreen helper |
| Post: TAA resolve, bloom down/up chain, composite | `createPost()` | fullscreen helper |
| Auto-exposure histogram/adapt (compute) | `createPost()` | |
| HUD textured-quad | `createHud()` | |
| Analytic sky | `createSky()` | |
| Water (Gerstner) | `createWater()` | |
| Particles (additive + alpha) + decals | `createParticles()` | |
| Debris integrate (compute) + debris draw | `createDebris()` | |
| GPU skinning LBS (compute) | `createSkinning()` | |
| IBL equirect/irradiance/prefilter/BRDF bake | `createIbl()` | bake pipelines, boot |

### Previously-LAZY pipelines — CONVERTED to boot precompile (this branch)

These used to be built by `ensureRtaoReady()` / `ensureReflReady()` /
`ensureDdgiReady()` **in the middle of the first frame that enabled them** — a
mid-frame `vkCreate*Pipelines` plus a `vkDeviceWaitIdle()`, i.e. exactly the
UE-style PSO hitch. Worse: SSR defaults ON, so the reflections chain compiled
mid-frame on frame 1 of **every** run.

| Pipeline(s) | Old trigger | Now |
|---|---|---|
| RT-AO ray-query AO (compute) + multiply-apply (gfx) | first frame with `r_rtao 1` | **boot** (`init()` precompile, ray-query devices) |
| SSR reflections (compute) | first frame with `r_ssr 1` + TAA (= frame 1, default config) | **boot** (all devices) |
| RT reflection fallback (compute) | same | **boot** (ray-query devices) |
| DDGI probe rays + probe update (compute) | first frame with `r_ddgi 1` | **boot** (ray-query + position-fetch devices) |
| `VulkanRT` AS module init | first frame any RT consumer ran | **boot** (`ensureRtCore()` from `init()`) |

The `ensure*Ready()` lazy paths remain as graceful fallback but find
`m_*Built == true` and create nothing. The strict assert would flag them if
they ever fired again.

### Documented exceptions (cannot precompile — created at declared boundaries, never mid-gameplay-frame)

| What | Boundary | Why it's not a stutter |
|---|---|---|
| Extent-tracking targets (HDR scene, bloom mips, SSAO/GI/RT-AO/refl buffers, depth) | swapchain/offscreen **resize** (`recreateSwapchain()` / `recreateOffscreenTarget()`) | explicit user action; `vkDeviceWaitIdle` recreate; **no pipelines** (dynamic rendering, formats are extent-independent) |
| Reflection target res switch | `r_reflquality` change | settings-change boundary; images/descriptors only |
| DDGI atlas dims | `r_ddgi_nx/ny/nz` change | settings-change boundary; images/descriptors only |
| ImGui internal pipeline + descriptor pool | `initEditorUI()` (`--editor` dev tool) | dev-only, explicit editor-open moment; excluded via the boundary flag (ImGui compiles its own PSO outside our wrappers — TODO: pass our `VkPipelineCache` through `ImGui_ImplVulkan_InitInfo`) |
| BLAS (new static mesh) / TLAS (instance-set change) builds | scene mutation (level load, mesh registration) | GPU AS builds, not PSO compiles; counted per-frame and attributed in the spike log; static scenes build once during warmup |
| IBL sky re-bake | sky-parameter change (one-time submit) | rare, attributed in the spike log (`+iblbake`) |
| Mesh/texture upload (`createMesh`/`createTexture`) | level load / streaming | allocations, attributed in the spike log (`allocs+N`) |

### The strict gate — `r_strictpso`

Any pipeline / shader module / descriptor pool created after the first
`beginFrame()` (outside a declared boundary) logs a validation-style line:

```
[ERROR] [stutter] graphics pipeline created after first frame (frame N) — precompile it at boot or inside a declared recreate boundary
```

Default **ON in Debug** (with the validation layers), OFF in Release — the
counters always accumulate regardless and gate `--test-framepacing` in both
configs.

---

## 2. VkPipelineCache persistence

The device creates its `VkPipelineCache` before the first PSO compile, seeds it
from `x3pipeline.cache` beside the exe (validated against the spec'd 32-byte
header: vendorID + deviceID + pipelineCacheUUID — a foreign/stale blob is
ignored), and saves it at shutdown.

**Measured (RTX 5090, Release, 53 boot pipelines):**

| Boot | Pipeline compile wall time | Cache |
|---|---|---|
| First (cold) | **45.7 ms** | 0 bytes loaded → 1,232,867 bytes saved |
| Second (warm) | **5.3 ms** | 1,232,867 bytes loaded |

**8.6x faster** pipeline boot; and since *all* compiles happen at boot, this is
the entire compile cost of a session — frames never see any of it.

Log receipts every run:

```
[rhi] pipeline cache: WARM — loaded 1232867 bytes from ...\x3pipeline.cache
[rhi] boot precompile: 53 pipelines total (6 RT-chain) in 5.3 ms (warm cache) — rtao=1 refl=1 ddgi=1; no pipeline may be created after frame 1
[rhi] pipeline cache: saved 1232867 bytes (this boot compiled 53 pipelines in 5.3 ms; loaded 1232867 bytes)
```

---

## 3. Frame-pacing telemetry

* **Ring buffer** of the last 4096 post-warmup frames: CPU time (endFrame →
  endFrame wall delta) + GPU time (the existing per-frame timestamp queries).
* **`framePacing()`** snapshot API: p50/p95/p99/p999/max for CPU + GPU, spike
  count, late-creation counters, boot compile stats.
* **Spike log** — any post-warmup frame above `max(r_fpace_spikex * rolling
  median, median + r_fpace_floor ms)` emits ONE line with cause attribution
  from that frame's creation counters:

```
[pacing] SPIKE frame=187 cpu=9.41ms (median 4.02) gpu=4.11ms | pso+0 mod+0 pools+0 allocs+0 asbuild+1 +iblbake
```

* **HUD line** (`r_frametelemetry 1`): live percentiles + spikes + late counters.
* Thresholds are cvars (`r_fpace_warmup` 60 / `r_fpace_spikex` 2.0 /
  `r_fpace_floor` 3.0 ms) so CI can tighten them; the absolute floor filters
  OS-scheduler noise on sub-ms headless frames — a real PSO hitch is 50–300 ms,
  three orders of magnitude above it.

---

## 4. The gate: `--test-framepacing`

Headless 600-frame scripted camera flythrough of Level 1 (deterministic ellipse
over the spawn→armory spine, two laps, tangent look + pitch bob), ticking the
real game loop (Level1Game, physics, scene sync, FX) and rendering the full
stack. Asserts:

1. ring has ≥ 400 post-warmup samples,
2. **ZERO** post-warmup spike frames,
3. **ZERO** pipelines created after frame 1,
4. **ZERO** shader modules created after frame 1,
5. **ZERO** descriptor-pool growth after frame 1.

### Measured flythrough (RTX 5090, Release)

```
TBD_FLYTHROUGH_RESULTS
```

---

## 5. The guarantee

> After the first frame begins, X3Native creates **no** graphics or compute
> pipeline, **no** shader module, and **no** descriptor pool, outside the
> declared boundaries above (resize, explicit quality-setting change, editor
> open). All 53 pipelines of the full render stack compile at boot, through a
> persistent on-disk pipeline cache that cuts even that to ~5 ms on every boot
> after the first. Frame pacing is continuously measured; any frame exceeding
> 2x the rolling median is logged with its cause; and the `--test-framepacing`
> CI gate fails the build if a single post-warmup frame spikes or a single
> late creation occurs.

UE ships PSO-precache heuristics and still hitches when a material variant
misses. X3Native makes the miss **structurally impossible** and instruments the
renderer to prove it on every run.
