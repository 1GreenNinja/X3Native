# Reflection DENOISE stage — verification evidence

Lane `inspx/refl-denoise`, cut from `inspx/rt-reflections`.

Everything here was captured with the engine's own headless rig
(`--screenshot-car`, 1280x720 from 4x SSAA, 90 settle frames per still) on the
real hero car, `assets/converted_glb/Vehicles/CTR.glb`.

## The three capture sets

| dir | command | what it is |
|---|---|---|
| `refl_off/` | `--screenshot-car ... --norefl` | reflections entirely off — the floor of the metric |
| `dn_off/`   | `--screenshot-car ... --refldn 0` | **the shipped pre-denoise renderer**, and md5-identical to it (see below) |
| `dn_on/`    | `--screenshot-car ...`            | the denoise stage at its shipped default, `r_refldenoise 4` |

## `r_refldenoise 0` IS BIT-EXACT — verified, not asserted

The fallback cvar was checked the only way that actually proves anything:

1. The lane's changes were stashed, the **pre-lane tree** was built and captured.
   All 8 stills came out **md5-identical** to the baseline committed in
   `docs/screenshots/rt-refl-verify/real-car/ssr_on/` — so the rig is
   deterministic across builds and is a trustworthy gate.
2. The lane build with `--refldn 0` was then captured and compared to that
   pre-lane capture: **8/8 md5-identical.**

The first cut of the lane FAILED this and it is worth recording why, because it
is not obvious. It folded the denoised path into `sampleReflGlossy` behind a
`* discScale` factor that is exactly 1.0 when the stage is off, and it published
the denoiser's aux G-buffer from ~10 lines inside `refl.comp`. Both are
arithmetic no-ops when off. The A/B still came back with **+-1 LSB on up to
0.17% of subpixels** — shifted FMA contraction from recompiling perturbed
shaders. Visually nothing; enough to flip an md5 gate.

Both were fixed structurally rather than by argument:

* `sampleReflGlossy()` is left **textually verbatim**; the denoised path is a
  separate `sampleReflGlossyDenoised()` selected by `ssao.refl.w`.
* the aux G-buffer moved into its own shader, `shaders/refl_aux.comp`, so
  `refl.comp`'s SPIR-V is byte-identical (checked directly:
  `refl.comp.spv` md5 `0D1AD94E…` and `refl_rt.comp.spv` md5 `DC4FA2C8…`
  before and after).

## The metric

`tools/refl_blotch_metric.py door <img>` — mean `|px - 9x9 local mean|` on the
flat door skin, the same measurement that found the defect. The original commit
(02146c10) quoted 5.53 / 7.69 but not the rectangle, so the `door` preset is a
reconstruction tuned against the committed baseline stills; it lands at
**5.306 / 7.453** where the original reported **5.53 / 7.69**, and it reproduces
the same flat blur sweep (7.451 / 7.786 / 7.703 / 7.799 at radii 0 / 6 / 14 / 24).

### `car_day_profile` — the shot the defect was reported on

| | blotch | vs reflections-off |
|---|---|---|
| reflections OFF | 5.306 | — |
| shipped (`r_refldenoise 0`) | 7.453 | **+40.5%** |
| **denoised (`r_refldenoise 4`)** | **4.852** | **-8.6%** |

### All eight stills

| still | refl off | shipped | denoised |
|---|---|---|---|
| car_day_front34    |  3.411 |  6.683 |  5.839 |
| car_day_frontlow   |  7.697 |  6.851 |  6.683 |
| car_day_profile    |  5.306 |  7.453 |  4.852 |
| car_day_rear34     |  7.799 | 10.526 |  9.580 |
| car_extnight_front34 | 2.826 | 3.968 | 3.808 |
| car_extnight_rear  | 11.961 | 13.472 | 13.011 |
| car_night_front34  |  5.287 |  7.579 |  6.824 |
| car_night_rear34   |  6.811 |  7.681 |  7.601 |

The `door` rectangle is fixed in screen space and only lands squarely on flat
door skin on `car_day_profile` (the shot the original measurement used). On the
3/4 angles it also samples glass, chrome and wheel, which is why those rows move
less — see the zoom sheets for what is actually happening there.

## Iteration count was chosen by looking, not by the metric

Sweep on `car_day_profile` (`--refldn N`), against off = 5.31 / shipped = 7.45:

| iterations | blotch | verdict |
|---|---|---|
| 3 | 6.87 | still visibly cloudy |
| **4** | **4.82** | clean, and the broad reflected highlight sweep is still there |
| 5 | 3.44 | **below the reflections-off floor** — on screen the panel has gone flat and featureless |

A metric-only tuning pass would have shipped 5. This is exactly why the standing
requirement says screenshots are verification.

## Zoom sheets (look at these)

* `zoom_door_profile.png` — refl_off / shipped / denoised, door skin at 3x.
  The dense speckle "dirt" of the shipped build is gone; the panel reads as
  smooth paint carrying a broad reflected highlight.
* `zoom_door_rear34.png` — the same at the rear 3/4. **Honest:** much improved
  on the flatter door, but visible mottling REMAINS on the strongly curved
  rear-quarter shoulder and around the wheel-arch lip.
* `zoom_floor_mirror.png` — the polished showroom floor (rough 0.08) reflecting
  the wheel. Its spokes survive the denoise, which is what the roughness ramp in
  `sampleReflGlossyDenoised` exists to protect. With a hard raw/denoised switch
  at 0.05 they were visibly lost.
* `zoom_silhouette_contribution.png` — `|with - without reflections| x3` at the
  sill/floor boundary, which is the only way to SEE the second defect (in the
  raw image it hides inside the floor's own mirror reflection). The hard blocky
  step edges are softened; they are **not gone**.

## Second defect (silhouette bleed) — partial

Lower-silhouette band, same metric:

| still | refl off | shipped | denoised |
|---|---|---|---|
| car_extnight_rear  | 2.204 | 5.426 (+146%) | 3.837 (+74%) |
| car_night_front34  | 4.502 | 5.271 (+17.1%) | 5.001 (+11.1%) |
| car_day_profile    | 7.543 | 8.535 (+13.2%) | 8.422 (+11.6%) |
| car_night_rear34   | 7.442 | 8.093 (+8.8%)  | 8.045 (+8.1%)  |

Roughly half the excess removed on the worst case, marginal on the others.
It is improved, not fixed, and the reason is structural: this lane made the
BUFFER depth- and normal-consistent, but the remaining stair-step comes from
`mesh.frag` bilinearly upsampling a half-res buffer with **no depth-aware
upsample** — the thing `ssgi_apply.frag` does for GI and `mesh.frag` cannot,
because set 3 has no depth sampler. That is the named follow-up.

## Perf

`--bench 200 600 --skipintro`, 1280x720, two runs each:

| | GPU ms |
|---|---|
| `r_refldenoise 0` | 1.492 / 1.487 |
| `r_refldenoise 4` | 1.599 / 1.598 |

**+0.11 ms GPU (+7.3%)** for 1 aux dispatch + 4 a-trous iterations at half res.
Zero when off — the passes are not added to the graph at all.
