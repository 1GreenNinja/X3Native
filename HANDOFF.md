# W-HANDLING — lane handoff (live document, keep current)

Worktree: `.claude/worktrees/agent-a962cd94f9604eaa6`, branch
`worktree-agent-a962cd94f9604eaa6`, based on `7c94e869` (integration/complete).
Build dir: `build/` **inside this worktree** (already configured with the vcpkg
toolchain — ENGINE_GOTCHAS 1.4/5.3). Thin `X3Engine.exe` over `x3app.dll`.
**NEVER push** — the session lead merges.

## THE LANE (owner quotes are SPEC — NO_SLOP rule 8)
- "The car is a LOT harder to control now than it was in NFS.. Can we
  substantially increase the 'stick on the road' idea? It should be harder to
  flip the car upside down and spin it just driving"
- "Not TOOO much heavier.. and spoilers for downforce."
- "it shouldnt peg redline the whole time you drive... most cars dont do that"
- "make the final gear a 0.50:1"
- "will we need to increase engine power as we increase weight to make it fun!"

## STATUS — all lane items landed, all gates green
| item | state |
|---|---|
| 0.50:1 sixth gear | DONE — `vehicle.cpp` `gearRatios[5] = 0.50f` (exact) |
| 5.2 final drive (paired with the overdrive) | DONE |
| Throttle-adaptive shift band + kickdown detent | DONE (`JoltVehicle.cpp` preStep) |
| Aero downforce F=k·v², rear-biased + its induced DRAG | DONE |
| `car_downforce` / `car_rolldamp` cvars | DONE — via `HostShell::addFloatCommand`, which reads **args[0]** (IConsole.h ARG CONVENTION) |
| Roll-rate damping, gated >=3 wheels grounded | DONE |
| **MASS** | **UNCHANGED at 1083.2 kg (+0%)** — downforce delivered the planted feel, so the +10% allowance was not spent and engine torque did not need rescaling |
| H1-H5 measured A/Bs | DONE, 37/37 |

## GATES
| gate | result |
|---|---|
| Build | green (ALL_BUILD, `x3app.dll` mtime advanced every time — gotcha 1.1) |
| `--test-vehicle` | **37 passed, 0 failed** (+ vehcam 10/10, vehicle 4/4) |
| `--test-roadnetwork` | 58 passed, 0 failed |
| `--test-console` | 8 passed, 0 failed |
| `--test-vehparts` | 23 passed, 0 failed |
| Boot | `--screenshot-tunnel` on the real driving world: **zero [ERROR]** |
| Eyes-on | `shots_handling/` 9 full-res stills, read: road, lit bore, portal, LNSS garage all intact |

## MEASURED — baseline -> shipped
| metric | baseline | shipped |
|---|---|---|
| Cruise 70 mph | 5th @ ~4800 rpm forever (**the "pegged tach"**) | **6th, tach 2416, engine NOTE 2348, peak 2486** |
| Top speed | 153 mph **AT the 7500 limiter** in 6th | **181.2 mph in 6th @ 6527 rpm — drag-limited, off the limiter** |
| NOS ×1.6 | — | **209.9 mph** (6th geartop at redline 223 mph, so 220 stays gear-reachable) |
| 0-60 | 3.05 s | 3.00 s |
| 0-100 | 5.28 s | 6.13 s (the honest cost of the wing at speed) |
| Skidpad | — | 1.58 g flat, 0.07 deg roll, no wheel lift |
| **Slalom ~96 mph (H4)** | maxSlip **88.6 deg — SPINS OUT** | maxSlip **2.7 deg**, maxRoll 0.13 deg |
| **Curb strike 98 mph (H5)** | maxRoll **61.4 deg — onto Jolt's 60-deg limiter** | maxRoll **15.6 deg**, upright |

Mass before/after: **1083.2 kg -> 1083.2 kg. Engine torque unchanged (800 Nm
base).** Power-to-weight is untouched; the grip came from aero, not ballast.

## WHAT THE EYE COULD NOT CONFIRM (ENGINE_GOTCHAS 4.4, stated plainly)
Handling is a MOTION claim and no still can prove it. The H1-H5 A/Bs are the
evidence. Two specific things a capture could NOT show:
- The tach/HUD is drawn only in the live loop; `settleAndGrab` renders scene +
  car and no HUD, so there is no headless still of the settled needle.
- A parked-car beauty shot was attempted twice and abandoned: the spawn
  (3279.3, 10.2, -278.3) sits in a road CUT, so any camera placed beside it is
  buried in the embankment. Not worth more engine launches; it proves nothing
  this lane claims. Do NOT read the absence as a defect.

## THE THREE DEFECTS THIS SESSION FIXED (receipts, rule 10)
1. **The GLB sweep.** W-HANDLING2's checkpoint `a2b4dd6e` committed 10
   store-served `assets/converted_glb/Vehicles/*.glb`, two re-fetched nature
   GLBs and their 27 MB `*.pre-fetch.bak` blobs — ENGINE_GOTCHAS 2.5. Rewritten
   as `1e11a1c8`: code byte-identical (`git diff a2b4dd6e HEAD -- app engine`
   empty), all asset paths dropped, `.bak` deleted. `git log --name-only` over
   the branch shows **zero** `.glb` paths. The store carries them
   (`python tools/asset_store.py fetch --all`) — never re-add them.
2. **H4 was two tests in a trench coat, and the curb was never struck.** The
   slalom ran THROUGH the curbs, so the no-aero arm spun out before reaching
   them and the "downforce trips the car" conclusion compared a curb strike
   against a spin. AND the curb was 0.30 m long while a 60 Hz step at 100 mph
   advances the wheel 0.745 m — the raycast stepped clean over it and returned
   7e-6 deg of roll, a green meaning "no test ran". Split into H4 (spin) and
   H5 (flip); curbs are 3 m; H5 gates on >5 cm suspension asymmetry so a
   skipped bump can never pass again. **Any bump test in this engine must be
   longer than v_max/60.**
3. **The wing was free lunch and the shift band had 72 rpm of margin.**
   Downforce with no induced drag put top speed at 198 mph (45 above the car's
   own ~155) and made NOS worth only 18 mph. Added D_wing = L/(L/D), L/D 3.0.
   That raised cruise throttle a few points, which exposed the real fragility:
   `load = thr^2` put the 5->6 upshift at 4750 rpm while 5th at 70 mph sits at
   4822, and the box began hunting 5-6-5-6 to 5890 rpm — self-driven, because
   the overdrive needs more pedal and more pedal demands the downshift.
   Replaced with a **kickdown detent** at 0.55 pedal (load 0 below it,
   smoothstep to 1 at the floor). WOT untouched.

## TRAPS PAID FOR
- Headless slab tests MUST call `car.setTerrainContactLaw(false)` — the
  procedural terrain field is 37 m up at z=-381 after the W-MOUNTAIN merge and
  hoists the test car mid-run.
- Do NOT raise anti-roll bars (>= 15 kN/m the Jolt solver pumps roll and flips
  the car at 60 Hz) and do NOT touch WheeledTuning suspension spring/damper —
  another session owns suspension.
- `--test-vehicle` and friends dispatch at `main.cpp:462`, BEFORE any Vulkan
  device is created — they are CPU-only and safe to run while a sibling lane's
  engine is up. Only real `--world` / `--screenshot` launches need the
  `tasklist //FI "IMAGENAME eq X3Engine.exe"` gate. NEVER `--smoketest`.
- `--world tunnel --screenshot` ALSO rewrites the tracked `shots_wmap/*.png`
  (an always-on map/HUD proof set that belongs to the W-MAP lane).
  `git checkout -- shots_wmap/` after any such run.

## PAIRED VALUES (rule 4 — change one, change all)
`gearRatios[5]=0.50` · `finalDrive=5.2` · wheel r=0.33 · redline 7500 ·
`kShiftUpFrac/kShiftDownFrac` + their light-end pair + `kDetentFrac` ·
`kDownforceFrac70`/`kDownforceCap`/`kWingLoverD` · `kAeroDrag` ·
`host_tunnel`'s `car_reset` block and every cvar help string.
These together set cruise rpm, top speed and the NOS ceiling.
