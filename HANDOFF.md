# W-CLOUDS v4 HANDOFF (2026-08-17 morning)

Branch `worktree-agent-ac78e9cb58a88573a`. All local. **NEVER push.**

## STATE

Code COMPLETE and merged with `origin/integration/complete` (ce48e2b3). Build
green, all five gates green on the merged build, capture round re-shot on it,
measured, and eyeballed at full res. DONE unless the lead wants changes.

    git log --oneline -7

    444c4a27   clouds: the merged-build capture round, eyes-on, numbers green
    217f0045   clouds: measure_round.py + a correction to my own ladder diagnosis
    3a7679da   clouds: capture script cams / one-build ladder / [tunnel-perf] grep
    24c045fd   merge: origin/integration/complete (ce48e2b3)
    e698ddce   clouds: white-plane artifact is NOT the cloud lane - two receipts
    510ed561   clouds: capture round + one sky mapping across all three host sites
    23fbcfe8   clouds: receipts + HANDOFF (v2's last commit)

## 1. THE WHITE-PLANE ARTIFACT — SETTLED, NOT OURS

v3 died flagging "a non-cloud white-plane artifact" on two elevated cams. It is
TWO pre-existing artifacts, and A/B'd evidence is in commit e698ddce.

* **Hard-edged white QUAD, camera-locked** = the river's Gerstner water patch.
  A finite 480 m square centred on the camera (`kWaterPatchHalf = 240`,
  VulkanRenderDevice_internal.h:3052) at `riverRoad.plan.waterY`, fading to the
  ANALYTIC SKY at its rim because `applyRiverWater` never sets
  `WaterParams::horizonColor` — the exact seam IRenderDevice.h:1213 documents.
  Gone with `X3_RIVER_ROAD=0` (`diag_B_riveroff.png`). **W-RIVER's residual #2.**
* **Large pale FIELD** = the horizon ring's static inner hole. `rInner = 470 m`
  about ROUTE MID (host_tunnel.cpp) while the streamer gets ~90 tiles (~170 m)
  resident in a settle loop. `diag_E_topdown_void.png` shows it from 1500 m as a
  clean circle, 76,296 px of ONE flat colour (199,204,209), ~992 m across vs the
  coded 470 m radius; `diag_F` proves it world-static. **W-PERF's finding.**
* **Neither is cloud.** Same field with clouds fully off (`diag_C`, X3_CLOUD=0)
  reads 168,174,181 vs `diag_A`'s (0.42) 166,172,180 — 2/255 of sky tint and
  zero cloud structure. Under storm it goes 108,103,94: it tracks the sky
  because it IS the sky.

**DO NOT FIX EITHER.** Other lanes. The only cloud-lane consequence is cam
choice, already fixed in run_captures.sh (see its cam-choice block).

## 2. THE LADDER IS A STEP, AND THAT IS CORRECT

Do not "fix" it. A ground camera stands under ONE deck cell (~1.8 km features
vs ~100 m of visible ground): full sun at cover 0.00..0.75, then the cell
closes at 1.00. v4 initially misdiagnosed this as a build-straddle and
corrected it in the commit above. The real dimming curve is
`verify_new_field.py`'s landscape average of `cloudShadowFactor`:

    cover      0.00   0.25   0.50   0.75   1.00
    sun kept  1.000  0.934  0.763  0.470  0.219
    deck opac 0.000  0.061  0.266  0.645  0.942

## 3. PERF — INSIDE BUDGET (clean GPU, merged build, 07:57)

From the `[tunnel-perf]` line (W-FOREST's receipt; the cloud lane's duplicate
`[cloud-perf]` was deleted in the merge). Identical tris/draws/objs on both
sides, so the delta is shading only:

    spawn cam     1.482 -> 1.494 ms   clouds = +0.80% of frame
    sky cam       1.166 -> 1.220 ms   clouds = +4.43% of frame   <- worst
    straight up   0.577 -> 0.588 ms   clouds = +1.87% of frame

Worst case 4.43% against a 10% budget. `python shots_clouds/measure_round.py`
re-reads all of it (plus the one-build check and the flat-cell scan).

## 4. WHAT IS LEFT

Nothing blocking. The round is shot, measured, and eyeballed; gates are green
on the merged build (roadnetwork 58/58, terraincorridor 16/16, tunnelmouth
8/8, riverbridge 9/9, terrain 4/4). Commits are LOCAL — the session lead
reviews and pushes. NEVER push from this lane.

## 5. OPEN, HONEST, UNRESOLVED

* **The deck foreshortens into streaks toward the horizon.** Overhead
  (fair_03_up) it is proper puffy cumulus. Near the horizon (fair_02_sky,
  overcast_01_sky) a view ray crosses kilometres of the deck plane per pixel,
  so the cells compress into soft horizontal bands that read more cirrostratus
  than cumulus. It is SOFT — no edges, no shards — and it is inherent to a
  single flat 2D deck at 1400 m. Fixing it properly means a second layer at a
  different altitude, or a slab with thickness. Not attempted; flagged so the
  next reader does not think it was missed.
* **`X3_WEATHER=storm` renders SNOW, not rain.** `weather.cpp:298` turns
  precipitation to snow whenever `tempC <= 1.0` and the temperate storm is
  getting there (biome base 14 C). The owner asked for "rain 10", so
  storm_02_ground is a snow shot. WEATHER LANE, not this one — the deck, the
  crushed sun and the dark ground are all correct in it.
* **Bright specks in the sky.** ~130 of them in the straight-up frame. NOT
  ours: perf_base_up (X3_CLOUD=0) has 144 and fair_03_up (0.42) has 123 — more
  with the clouds OFF. Some other lane's particles/TAA fireflies.
* Coverage comment vs measurement drift is FIXED (sky_clouds.glsl now carries
  both the top-down and the as-rendered numbers, and why they differ).
