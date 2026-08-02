# Clustered (froxel) forward lighting — `r_clusterlights`

INTERNAL. Lane 2, 2026-08-02. Branch `inspx/clustered-lights` (from `inspx/rt-reflections`).

## What was wrong

Forward lighting was a fixed **64-entry point-light array in a UBO**, looped in full for
**every fragment** (`shaders/mesh.frag`, `glass.frag`, and the layout mirror in `mesh.vert`).
No culling, no clustering. At 1440p that is ~236M light evaluations per frame before
overdraw, and — worse than the cost — the cap was a hard ceiling on scene content:

* Echo Harbor's neon night city could not be lit.
* One tunnel's dressing alone would eat 48 of the 64 slots.
* Level 1 registered **332** ceiling fixtures; `setPointLights` silently kept the first 64
  **in build order**, and logged nothing. The level was lit by an ambient wash for a year
  because its lights were being thrown away (see the B4 note in `app_run.cpp`).

## What it is now

The view frustum is diced into **froxels** (frustum voxels). Each light is assigned to the
froxels its sphere of influence overlaps; the host writes fixed-stride per-froxel index
lists into an SSBO; each fragment resolves its own froxel from `gl_FragCoord` + its view
depth and iterates **only that froxel's list**, out of a light set of up to **1024**.

`r_clusterlights 0` (the default) still runs **the original 64-light loop, bit-for-bit**.

### Grid: 16 x 9 x 24 = 3456 froxels

Constants live in **one place**, `engine/rhi/ClusterLights.h`; the shader reads the
dimensions out of the per-frame UBO, so retuning is a one-line edit with no second copy to
drift.

* **16 x 9** matches the 16:9 frame, so a froxel is **square on screen** (80x80 px at 720p,
  120x120 at 1080p). Square froxels keep the sphere-vs-AABB test equally tight in both
  axes; a 16x16 tile grid on a 16:9 frame gives tall thin froxels that over-include in Y.
* **24 depth slices, EXPONENTIAL.** With the engine's 0.1 m near plane, each slice is a
  constant depth *ratio* — measured on the test camera (0.1 .. 500 m): the near slice is
  **4.3 cm** thick and the far slice is **149 m**, a 3506x spread. A linear grid would put
  the entire near field, where a lamp's sphere is screen-huge, into a single froxel.

Law (CPU and GPU implement the identical expression):

```
slice(z)   = floor(log(z) * sliceScale + sliceBias)
sliceScale =  GZ / log(zFar/zNear)
sliceBias  = -GZ * log(zNear) / log(zFar/zNear)
```

### Per-froxel capacity and the overflow policy

Lists are **fixed stride**: froxel *c* owns `[c*64, (c+1)*64)`. 3456 x 64 x 4 B = 884 KB per
frame-in-flight — nothing — and in exchange there is no prefix-sum pass and no indirection
table. Counts ride in the same buffer ahead of the lists, so it is one binding, not two.

**Policy: lights are visited in ascending light index; when a froxel already holds 64, the
candidate is DROPPED and counted.** So the winners are the *lowest-indexed* lights that
reach that froxel. Chosen over "nearest wins" / "brightest wins" because it is O(1) with no
per-froxel sort, it is **deterministic** (which this repo's md5 gates require — see below),
and it gives hosts a usable contract: *put the lights that matter first*.

Overflow is **never silent**, which is the whole point given what it replaces:

* `RenderStats::clusterOverflows` / `clusterOverflowed` / `clusterMaxLoad` every frame.
* A rate-limited `logWarn` naming the count, the froxel count and the cap.
* **`r_debugview 6`** paints froxel occupancy: black = empty, blue -> green -> red as the
  list fills, **white where the froxel is AT the cap**. One frame, by eye.

The self-test asserts conservation: `kept + dropped == the brute-force total`. Nothing can
vanish without being counted.

## CPU assignment, not a compute pass — and why

**Determinism is a hard requirement here.** This repo gates on md5-identical screenshots.
A parallel scatter or a GPU atomic-append pass produces per-froxel lists in a
*race-dependent order*, which changes float accumulation order in the shader's light loop,
which changes the low bits of the image, which breaks every image gate in the tree. A
serial ascending-index scatter is exactly reproducible — and it is what makes the strongest
test below (bit-identical legacy-vs-clustered renders) possible at all.

Secondary reasons: the rhi layer has no `IJobSystem` handle (wiring one in is a layering
change), and the engine enforces "no pipeline may be created after frame 1", so a compute
pass costs a shader + pipeline + descriptor set + barriers + a precompile slot.

The fixed-stride layout is already the shape a compute pass wants. If this ever needs to
move, do it as **two-phase count/fill** so the order stays ascending-by-light-index and the
image gates keep holding.

**Measured cost** (60-frame average, 512 lights, road-level 95 deg FOV, RTX-class):
**1.13 ms** CPU. This is the honest weak point — see "Not verified / next" below.

Two things mattered far more than the algorithm:

1. The assignment originally wrote **straight into the persistent-mapped, write-combined
   SSBO**, and its per-froxel count is a read-modify-write. **Reading WC memory cost 5.0 ms
   for a 48-light frame.** Assigning into ordinary cached staging and doing one linear
   memcpy took it to **0.25 ms — a 20x win**, and it is the same trick `cullStatsBuf`
   already uses for its readback.
2. Hoisting the froxel bounds by axis (z out of the row loop, y out of the column loop) so
   a whole slice or row rejects before its inner loop: another **~2x**.

## Files

| File | Role |
|---|---|
| `engine/rhi/ClusterLights.{h,cpp}` | the grid + assignment. Pure math, no Vulkan, no GLM in the interface — so the self-test drives exactly the code the device runs, with no GPU in the loop |
| `shaders/inc/mesh_lighting.glsl` | **Lane 2's module.** `pointAtten`, the froxel lookup, the `x3LightCount()`/`x3Light(i)` iterator, the `r_debugview 6` heatmap. Included by BOTH `mesh.frag` and `glass.frag` |
| `engine/rhi/vk/vk_passes.cpp` | per-frame build + upload in `prepareFrameData` |
| `engine/rhi/vk/vk_pipelines.cpp` | set 1 bindings 3/4 + the per-frame buffers |
| `app/cluster_light_test.{h,cpp}` | `--test-clusterlights` |

The shader never branches on the mode outside that one module: every light loop
(dielectric, PBR, clearcoat, `r_debugview 2`, glass) goes through the iterator, so there is
one loop body, not two.

## Why `r_clusterlights 0` is bit-exact

`x3LightCount()` returns `min(activeCount, 64)` and `x3Light(i)` returns `cam.lights[i]` —
**the same array elements, in the same order, as the original loop**. Same addends, same
order, same bits. The cluster SSBOs are allocated but the legacy branch cannot reach them.
`setPointLights` now accepts 1024, but `prepareFrameData` still copies only the **first 64**
into the legacy UBO array, which is exactly the set it always saw.

Verified end to end — these four md5s are identical before the work and after it:

| capture | md5 |
|---|---|
| `--test-primlight` | `13d21a6340405c9f9e0976fed98128c1` |
| `--screenshot-city` (establishing) | `ef8c893efbf82ebc51b745e11e52a839` |
| `--screenshot-city` (street) | `7b6a9681c6b1ea921d35202052ca257a` |
| `--screenshot-city` (scrapyard) | `8ecdfd44fdaff25c58802f69fc63e52f` |

## The gate: `--test-clusterlights` (37 assertions)

Run from the repo root. The design principle: **never check the implementation against
itself.**

* **Part A (CPU).** Every assignment is compared against an **independent brute-force
  sweep of all 3456 froxels** — the definition, with none of the fast path's narrowing. Plus
  the slice law and its round-trip, lights spanning many froxels, lights entirely outside
  the frustum (behind / lateral / past the far plane), a light *behind* the camera whose
  sphere still reaches in, list structure (in-range, no dupes, ascending), and the overflow
  policy including the conservation check.
* **Part B (real GPU). The equivalence proof:** with a light set that fits in 64,
  `r_clusterlights 0` and `1` must render **bit-identically — 0 differing pixels**. That
  holds because `pointAtten` returns exactly 0 at range and the assignment pads the radius,
  so dropped lights contribute exactly `+0.0f` and survivors are visited in the same order.
  Any difference at all means a light landed in the wrong froxel. Then 300 lights, with the
  panel lamps at index 240+: legacy leaves all 7 panels **black**, clustered lights all 7.
* **Part C.** The 256-light neon street A/B + the occupancy heatmap.
* **Part D.** The perf table.

**TAA and auto-exposure are turned off for Part B.** The first version of this test reported
~33k differing pixels with the assignment already provably correct — it was measuring TAA's
Halton *jitter phase* advancing between the two renders, and auto-exposure starting run B
from run A's adapted value. Two sequential renders are only comparable if each frame is a
pure function of the scene.

### Proving the gate can fail

Both injected, both reverted (`grep -c "NEGATIVE CONTROL"` = 0 in both files):

1. **Froxel grid Y axis flipped** in `ClusterLights.cpp` (the classic gl_FragCoord-origin
   mistake) -> **4 failures**: A9d brute-force mismatch, A10d conservation, B1 equivalence
   (1650 px), C2 brightness.
2. **Shader-only, slice off by one** in `mesh_lighting.glsl` -> **5 failures, Part A stayed
   fully green**: B1 (39172 px), B2d (0 of 7 panels lit), B2e, C2, C2b. This is the one that
   matters — it proves the GPU half independently gates the *shader's* fragment->froxel
   lookup, not just the CPU maths.

There is also a permanent in-test negative control (A11): the comparator every Part-A check
depends on is fed a deliberately wrong assignment and must reject it.

## Perf

60-frame average of the engine's own GPU timestamp queries; 256-light neon street,
road-level camera at 95 deg FOV so the geometry fills the frame (point-light cost is a
fragment-stage cost — a camera that leaves half the frame on the clear colour measures
mostly nothing). 1280x720, RTX-class.

| scene lights | path | GPU main-pass ms | cluster CPU ms | lights ACTUALLY rendered |
|---:|---|---:|---:|---:|
| 64 | legacy | 0.397 | 0.000 | 64 |
| 64 | **clustered** | **0.353** | 0.777 | 64 |
| 256 | legacy | 0.397 | 0.000 | **64** |
| 256 | **clustered** | 0.416 | 1.054 | **256** |
| 512 | legacy | 0.398 | 0.000 | **64** |
| 512 | **clustered** | 0.418 | 1.131 | **512** |

Read the last column before the timing column. **The legacy row does not get faster with
more lights — it stays flat because it silently drops everything past the 64th.** "Legacy at
512 lights" is a measurement of a scene it is not drawing. There is no honest apples-to-
apples legacy number above 64.

The two comparisons that are honest:

* **Same content (64 lights): 0.397 -> 0.353 ms, 1.12x faster.** Legacy evaluates all 64 for
  every pixel; clustered evaluates only the ones whose sphere reaches that pixel's froxel.
* **8x the lights for 1.05x the GPU time:** legacy renders 64 in 0.397 ms; clustered renders
  **512** in 0.418 ms.

Caveats, stated plainly: a 60 m street corridor is close to the **worst case** for froxel
occupancy (everything overlaps — 17 froxels overflowed at 256 lights, 106 at 512), and at
720p with 26 boxes the frame is not deeply fragment-bound, so the 0.35-0.42 ms band is near
this scene's noise floor. The per-pixel win should widen at 1440p and in scenes where lights
are spread rather than stacked down a corridor.

## Screenshots

`docs/screenshots/clustered-lights/`

| file | what |
|---|---|
| `neon256_legacy.png` / `neon256_clustered.png` | the 256-light street, same camera, `r_clusterlights` 0 vs 1 |
| `neon256_froxel_heatmap.png` | `r_debugview 6` occupancy |
| `ab_48lights_legacy.png` / `ab_48lights_clustered.png` | the bit-identity pair (these two files are byte-identical) |
| `ab_300lights_legacy.png` / `ab_300lights_clustered.png` | lamps at index 240+ |

**Verdict, having looked at them.** The A/B pair is unambiguous and it is exactly the
predicted failure mode: under legacy the street is lit for its first half and then **falls
off a cliff into black** — the neon past that point sits at light index 64+ and the UBO array
cannot reach it. Under clustered the corridor is lit continuously down its whole length, the
far pilasters read, and the magenta/cyan/amber banding continues to the end. The near half is
*pixel-identical* between the two (luma 34.63 vs 34.64), which is correct and is itself
reassuring: those tubes are inside the first 64, so both paths must agree there, and they do.
At the darkest road row the change is 26.34 -> 54.97 luma, 2.09x.

The heatmap reads cleanly: the froxel grid is visible, occupancy is low (cyan) near the
camera and climbs through green/yellow with depth, and the far centre of the corridor goes
**white** — froxels at the 64 cap, i.e. the 349 dropped assignments the warning reports,
located visually. That white patch is a real limitation of this scene at this grid size, not
an artifact: a single far froxel spans a large chunk of a 60 m street.

Honest note on the stress scene: it is untextured graybox boxes, so it demonstrates the
lighting change and nothing about art. It is a light-transport proof, not a beauty shot.

## Not verified / next

* **The 1.13 ms CPU assignment at 512 lights is the weak point.** It is ~7% of a 16.6 ms
  frame. Next step is the two-phase count/fill so it can be jobified across `IJobSystem`
  while keeping ascending order (and therefore the image gates). Not done here because the
  rhi layer has no job-system handle.
* **Not measured at 1440p** — the headless device renders 1280x720. The per-pixel win is
  expected to widen with resolution; that is an extrapolation, not a measurement.
* **Not exercised in a real world.** The city host feeds only 14 street lights
  (`streetLights.selectLights(..., 14)`) and truncates at 64 in `app_run.cpp`; raising those
  is host-side art direction and belongs to whoever owns those files. The 200+ light proof
  is the synthetic neon street. **The wins listed in the dispatch plan (Echo Harbor's night
  city, tunnel dressing, opulent interiors, car underglow) are now UNBLOCKED, not delivered
  — each still needs its host to actually push more than 64 lights and set `r_clusterlights 1`.**
* **`kMaxLightsPerCluster = 64` is not tuned**, just chosen. The neon street overflows it at
  256 lights. Whether the fix is a bigger cap, a finer grid, or shorter light ranges wants a
  real scene to decide against.
* **Glass is wired but not visually A/B'd** — `glass.frag` uses the same iterator and the same
  include, and it compiles and renders, but no glass-heavy 200-light scene was captured.
* **The `r_clusterlights 1` path has no md5 gate of its own** (it is new, so there is no
  baseline to pin). Only the `0` path is gated bit-exact.
