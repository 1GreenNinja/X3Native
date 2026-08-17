# W-CLOUDS v2 HANDOFF — session cap imminent (2026-08-16)

## STATE: code COMPLETE + committed. Eyes-on capture round NOT done (GPU contention all evening).

Branch `worktree-agent-ac78e9cb58a88573a`, based on 7345a929. Commits (all local, do NOT push):
- 576180df clouds: kill the shard sky at its hash + ground shadow (task #27)
- c4c14f3b clouds: storm deck crushes the sun disk (occlude by cloud alpha)
- cd93b5a8 clouds: normalize fBm by live weight sum + recalibrate threshold
- (this commit) receipts + handoff

## WHAT WAS BUILT (details in commit messages)
- Root cause of the "AWWFUL" shard sky: `fract(sin(x)*43758.5)` hash — GPU sin
  loses fractional precision at large |x| -> constant per cell -> flat
  rectangles. Receipt: shots_clouds/repro_cloud.py (+ repro_*.png pairs),
  before-shot shots_clouds/base_sky_042.png.
- shaders/inc/sky_clouds.glsl: ONE shared density field (sin-free hash12,
  rotated fBm NORMALIZED by weight sum, quadratic coverage threshold
  lo = 0.66 - 0.20c - 0.29c^2, cloudShadowFactor()).
- shaders/sky.frag: soft cumulus composite, gloom->near-black storm base at
  cover 0.94, optical alpha, sun disk/glow attenuated by deck opacity.
- CLOUD SHADOWS (task #27): mesh.frag multiplies the direct-sun `shadow` term
  by cloudShadowFactor; new SsaoControl::cloudShadow lane (strength 0.85,
  cover, skyTime) filled device-side from cached sky params in vk_passes.cpp.
  No host API; gate shut when cover 0/sky off.
- host_tunnel.cpp: setSkyTime(riverWaterClock) both loops (wind drift),
  X3_CLOUD env override (no-weather cover; A/B knob), [cloud-perf] gpu-ms
  receipt in settleAndGrab, resolved-cam log line.

## GATES ALREADY GREEN
- Build green (Release). SPVs fresh in build/bin/Release/shaders/.
- Suites: roadnetwork 58/58, terraincorridor 11/11, tunnelmouth 8/8,
  riverbridge 9/9 (headless CPU, safe under contention).
- Boot `--world tunnel` zero [ERROR]: shots_clouds/log_perf_base.txt.
- CPU receipts: shots_clouds/verify_new_field.py — sky-fraction ladder
  0.37 @ cover 0.42 / 0.71 @ 0.66 / 0.98 @ 0.94 (±40 km window, time-stable);
  3-oct shadow field == 5-oct sky field within 1%.

## CAPTURE ROUND — 0 of 9 clean (the only completed run was GPU-contended)
Run when no foreign X3Engine.exe (script self-guards):
    cd <worktree> && sh shots_clouds/run_captures.sh
Produces + logs (grep each log for "[ERROR]" and "[cloud-perf]"):
  perf_base_cloud0.png  (X3_CLOUD=0 baseline, spawn cam)   <- perf A
  fair_01_spawn.png     (cover 0.42, same cam)             <- perf B; budget
                        gate: (B-A)/B < 10% gpu ms, uncontended
  fair_02_sky.png / fair_03_up.png (sky view + straight up; NO polygon edges)
  shadows_01.png / shadows_02.png  (elevated; dapple on open ground)
  overcast_01_sky.png   (X3_CLOUD=0.70 broken overcast)
  storm_01_sky.png / storm_02_ground.png (X3_WEATHER=storm; near-black deck,
                        sun crushed, dark ground)
EYES-ON each PNG full-res before shipping (NO_SLOP 2). If fair sky reads too
sparse, nudge only the lo fit in sky_clouds.glsl + verify_new_field.py (pair).

## PERF SO FAR
Only contended garbage: perf_base gpu=358 ms avg while owner played (ignore).
Redo both A/B numbers from a clean window via the script.

## HAZARDS
- Owner + other lanes launch X3Engine.exe at random — NEVER capture alongside
  (coordinator directive); script guard handles it.
- Untracked leftovers in worktree NOT mine/committed: assets/converted_glb/
  Vehicles/*.glb, modified shots_wmap/*.png, cc_porous_cement/normal.png —
  predecessor-era; left untouched deliberately.
