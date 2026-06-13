# D15 GPU-Driven Culling — bring-up results (RTX 5090 / i9-14900K)

Branch `feat/gpu-cull`. All numbers measured 2026-06-12 on the 14900K + RTX 5090,
driver-current, Release build. Bench = `--bench 100000 --cullpath N [--hzb]`
(1600x900 windowed, vsync OFF, 600 frames / 60 warmup, fixed camera at the
Level-1 spawn cube field). Stills = headless 1280x720 `--screenshot` with the
stats overlay forced (`--stress 100000`).

## Equivalence (the acceptance gate)

The GPU predicate is bit-equivalent to the CPU `r_frustumcull` test
(`precise`-qualified plane expression matching glm::dot association; same
normalized planes; same `worldSphere`; same ALWAYS_VISIBLE bypass).

| Check | Result |
|---|---|
| `--test-gpucull` (real device, validation ON, pose sweep + Tier-1 sweep) | **40/40** |
| Level 1 reference (`--smoketest --cullpath 1`) | GPU `drawn=1192 tested=8568` == CPU `objs 1192/8568` **EXACT** |
| Level 1 on Tier 1 async (`--cullpath 2`) | `drawn=1192/8568` **EXACT** |
| Level 1 + HZB (`--cullpath 1 --hzb`) | `drawn=717 + hzb=475 = 1192` — conservation exact |
| Pixel diff CPU vs Tier 0 vs Tier 0+HZB (Level-1 still) | bbox `None` — **byte-identical images** |
| Debug validation (`--smoketest`, `--test-gpucull`, all paths) | **0 VUID**, `allocationCount=0` |

## 100k-object density bench (10x demo)

100,000 instanced cubes + the full Level 1 (108,567 total submitted instances).

| Path | FPS | CPU ms | GPU ms (graphics queue) | drawn / tested | frustum | hzb |
|---|---|---|---|---|---|---|
| `r_cullpath 0` (CPU cull, baseline) | 242.6 | 4.122 | 2.266 | 80,727 (CPU-side) | — | — |
| `r_cullpath 1` (Tier 0, gfx-queue compute) | 216.9 | 4.611 | 2.638 | 80,727 / 100,558 | 19,831 | 0 |
| `r_cullpath 2` (Tier 1, async compute) | 221.6 | 4.513 | 2.284 | 80,727 / 100,558 | 19,831 | 0 |
| `r_cullpath 1 --hzb` (Tier 0 + occlusion) | 221.9 | 4.506 | **1.406** | **18,226** / 100,558 | 19,831 | **62,501** |

Honest read:

* **All three GPU paths draw the exact CPU survivor set** (80,727) — equivalence
  holds at 100k scale.
* **HZB is the headline win**: 62,501 of the 80,727 frustum survivors are
  occluded inside the field → graphics-queue GPU time drops 2.27 → **1.41 ms**
  (-38% vs the CPU baseline) while producing the identical image.
* **Tier 1 moves the cull off the graphics timeline**: Tier 0's graphics queue
  pays 2.638 ms (raster + cull dispatch); Tier 1 pays 2.284 ms — i.e. the
  ~0.35 ms cull cost vanished onto the dedicated compute queue (graphics time
  returns to the no-cull-on-queue baseline of 2.266 ms).
* **CPU cost is ~flat across paths (4.1–4.6 ms @ 100k)** and is dominated by
  the immediate-mode `drawMesh()` submission walk itself, NOT by culling — on
  this 14900K the CPU sphere test was never the bottleneck. The GPU path pays
  ~0.4 ms more CPU than the baseline because it must write ALL 100k ObjectData
  rows + 100k CullInstanceGpu rows (the GPU decides visibility), while the CPU
  path writes survivors only. As the handoff predicted for the 5090 box: the
  big *raster* win is HZB; the Tier-1 win is the graphics-timeline reclaim.
  (The 1080 Ti box, where vertex work is the bottleneck, is where HZB's
  drawn-count collapse pays the most.)
* 242/217/222/222 FPS at 100k objects = far past interactive on all paths.

## Stills (stats overlay = part of the evidence)

* `demo_100k_cpu.png` — CPU path baseline.
* `demo_100k_tier0.png` — Tier 0, overview: `tested 108567 drawn 105412 frustum 3134`.
* `demo_100k_tier0_hzb.png` — same vantage + HZB (`hzb 21` — open vantage, almost nothing occluded: honest negative case).
* `demo_100k_tier1.png` — Tier 1 async, same counters as Tier 0.
* `demo_100k_hzb_inside.png` — ground-level inside the field: `tested 108567 drawn 276 frustum 33849 hzb 74442` — the near cube wall occludes essentially the whole world.

## Tier map shipped

| Tier | cvar | Status |
|---|---|---|
| Tier 0 — frustum compute on the graphics queue | `r_cullpath 1` | GREEN (equivalence exact, 0 VUID) |
| HZB occlusion phase | `r_hzb 1` | GREEN (standard-Z verified; conservation exact; pixel-identical stills) |
| Tier 1 — async on the dedicated compute queue | `r_cullpath 2` (also `-1` auto on 5090) | GREEN (CONCURRENT buffers + timeline semaphore; validation silent) |
| Tier 2 — mesh-shader meshlets | `r_cullpath 3` | NOT WIRED (meshlet builder 7/7 tested; task/mesh interface match vs mesh.frag outstanding) |

Notes / known limits:
* HZB has one frame of latency (pyramid = last frame's depth): a freshly
  disoccluded object can be missed for exactly 1 frame. Still camera = zero
  artifacts (pixel-diff proof above).
* HZB + Tier 1 demotes the cull to the graphics queue for that frame (the
  reduce samples the graphics-owned depth image; cross-queue image sharing is
  future work).
* A sky-change frame (IBL probe rebake) falls back to the CPU cull for that one
  frame (the probe bake replays the indirect commands before the cull runs).
