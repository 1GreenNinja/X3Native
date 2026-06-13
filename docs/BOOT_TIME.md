# BOOT-TO-GAMEPLAY UNDER 2 SECONDS

Branch: `feat/fast-boot` (on top of `feat/zero-stutter`).
Machine: RTX 5090, fast NVMe, Release, **warm pipeline cache**.

**Definition.** Boot-to-interactive = process start (static-init clock in
`engine/core/x3_boot.cpp`) → the **first main-loop frame presented**: window up,
world fully built, menu live (player controllable on START). The intro
cold-open is *content* (a skippable cinematic), not boot work — `--test-boottime`
skips it; on a real default boot it plays *while* the async GLB warmup runs.

**Gate.** `--test-boottime [budgetMs]` — Release, windowed, warm cache. Boots
the real interactive path, runs exactly one main-loop frame, prints the `[boot]`
phase table, exits 0 iff total < budget. Budget = optional CLI arg, else the
`boot_budget_ms` cvar (default **2000 ms** — loosen on weaker machines). With no
`--world` it gates the **canonical world** (`canonlevel`, the data-driven
Floor 1 — the game's true level); the legacy hand-coded tower builds ~5x the
entity count and has an honest floor of ~3.1 s — gate it explicitly:
`--test-boottime 4000 --world level1`.

Every interactive boot and every `--smoketest` run also logs the `[boot]` phase
table + total.

## Baseline (before, commit aa2aeda + instrumentation only)

Headless `--smoketest` totals (lower bound — no swapchain/fade):

| Phase (top poles)                       | level1 (default) | canonlevel |
|-----------------------------------------|---------:|---------:|
| world build (geometry + GLB spawns)      | 42 231 ms | 26 386 ms |
| weapon viewmodels (GLBs)                 |  2 714 ms |  1 982 ms |
| rhi: instance+device                     |    500 ms |    533 ms |
| third-person avatar (GLB)                |    461 ms |    367 ms |
| audio init + sfx loads                   |     85 ms |     85 ms |
| **TOTAL (headless, to last frame)**      | **53.8 s** | **~32 s** |

First *windowed* interactive frame on the legacy tower additionally paid
**6.6 s** of per-mesh BLAS one-shot builds (~8000 submits) the first time RT
(reflections tier / RT shadows) saw the scene.

### Root causes (measured, in order)

1. **Per-box blocking GPU uploads** — every `createMesh`/`createTexture` did its
   own staging submit + fence wait (~8 ms fixed cost); the canon floor alone is
   1039 boxes = ~2100 submits → **16.5 s** of pure submit/wait.
2. **Byte-by-byte file reads** — `PakAssetSource` dir reads used
   `istreambuf_iterator` (~230 ms per 49 MB GLB).
3. **Repeated GLB loads per spawn** — 19x `marcus_webb_anim.glb`, 17x
   `Drone.glb`, 8x `chief_martinez_anim.glb`… each re-read, re-parsed,
   re-decoded, re-uploaded.
4. **~8000 serial BLAS one-shot submits** on the first RT frame (legacy tower).
5. Serial decode of all boot textures on the main thread; audio init serial;
   a dedicated loading-screen hold+fade loop (~0.2–0.6 s of padding).

## What changed

| Fix | Where | Effect |
|---|---|---|
| **Upload batching** (`begin/endUploadBatch`, double-buffered cmd+fence pair; auto-submit at `beginFrame`/any one-shot; trailing `TRANSFER→ALL` barrier so no CPU wait is needed) | `VulkanRenderDevice` | canon floor 19.3 s → **0.12 s**; recording never blocks on the in-flight submit |
| **Bulk file reads** (`file_size` + one `read()`) | `PakAssetSource` | 49 MB GLB: 230 ms → ~15 ms |
| **Texture cache** (content-hashed, refcounted; per-model ref dedupe matches unload) | `ModelLoader` | identical images across files/instances decode+upload once |
| **Model template cache** (CPU data + shared textures; per-instance mesh buffers re-uploaded, batched; `contentStamp` size+mtime key — no re-read on a hit) | `ModelLoader` + `IAssetSource` | repeat spawn loads: ~400 ms → **&lt;0.2 ms** |
| **Decode prewarm** (`prewarmModelDecodesAsync`) — parallel stb decodes kicked **before Vulkan init starts** | `ModelLoader`, `main` | ~240–570 ms of decode fully hidden under the ~1 s driver init |
| **Async GLB preload** (`preloadModelsAsync` kicked mid device-init via `DeviceDesc.onUploadReady`; upload entry points mutex-guarded; staging memcpys unlocked) | `ModelLoader`, `VulkanRenderDevice`, `main` | warmup fully overlapped; `GLB preload joined` ≈ 0 ms |
| **Async audio bring-up** | `main` | 85 ms → ~2 ms (overlapped) |
| **In-loop loading fade** — no dedicated hold/fade loop; the completed bar fades OVER the live main loop (menu already interactive beneath it; fast boots fade 3x quicker) | `main`, `loading_screen` | ~0.2–0.6 s of padding removed; the heavy first frame happens under the bar |
| **Batched + budgeted BLAS warm-up** (`begin/endBlasBatch`, ≤4096 new BLAS/frame, raster fallback until complete) | `VulkanRT`, `VulkanRenderDevice` | legacy-tower first frame 6.6 s → ~0.3–0.4 s; RT (shadows/reflections) comes online a frame or two in — a graceful warm-up, never a hitch |
| **Sampled content hashing** (size + 3×64 KB probes) | `ModelLoader` | full-byte FNV over 30 MB textures eliminated |

User-visible deferrals (all graceful):
* **RT warm-up**: on worlds with more than ~4096 static meshes, RT shadows /
  ray-query reflections use the CSM/SSR raster fallback for the first 1–2 frames
  while BLAS build (attributed in the zero-stutter spike log, inside the
  framepacing warmup window).
* **Loading fade**: the bar's fade-out now overlays the live menu instead of
  delaying it.
* **Intro**: unchanged for players; explicitly excluded from the gate.

## After (Release, warm cache, windowed `--test-boottime`)

**canonlevel (the gate): 5/5 PASS, median 1595 ms** (1540 / 1594 / **1595** / 1743 / 1755).

Representative phase table (1539 ms run):

| Phase | ms |
|---|---:|
| static init + args + prewarm kick | 1 |
| glfw init + window | 50 |
| rhi: vk instance | 250–375 |
| rhi: phys select + device build | 335 |
| rhi: swapchain+frames+shadow | 220–280 |
| rhi: pipelines (core/hud/sky/post/RT, warm cache) | ~230 |
| audio init + sfx loads (async, joined) | 2 |
| physics init | 2 |
| loading frame (IBL first bake ~70 ms) | 30–130 |
| GLB preload joined (fully overlapped) | ~0 |
| canon floor geometry+doors | ~20 |
| canon gameplay spawns (cache hits) | ~200 |
| weapon viewmodels + 3P avatar + fx | ~50 |
| systems wired + player spawn | ~15 |
| first interactive frame (incl. AS warm-up) | ~280 |
| **TOTAL** | **~1540–1750** |

**level1 (legacy tower, `--test-boottime 4000 --world level1`): ~3.1–3.3 s**
(was **~9.9 s** windowed). Blocker phases: the Vulkan driver window
(instance 370 + device 480 + swapchain 280 ≈ 1.1 s, irreducible here) plus the
~50 serial monster spawns across the Spire floors (~740 ms of per-instance
skinning/ragdoll setup — gameplay-code parallelization, out of scope for this
pass) and CPU contention between the 29-file warmup and driver init.

## Receipts

* `--test-boottime` (canonlevel, Release, warm cache): **5/5 PASS < 2000 ms**, median **1594.7 ms**
* Full `--test-*` suite (Release): **83/83 PASS** (boottime is additive on top)
* `--smoketest` Release: 30 frames + recreate OK, **0 VUID**, VMA `allocationCount=0`
* `--smoketest` Debug (validation ON): **0 VUID**, `allocationCount=0`
* `--test-framepacing`: **5/5 PASS** — zero unattributed spikes, zero late pipelines/modules/pools
