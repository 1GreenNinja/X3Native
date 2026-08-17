# W-HANDLING — lane handoff (live document, keep current)

Worktree: `.claude/worktrees/agent-a962cd94f9604eaa6`, branch
`worktree-agent-a962cd94f9604eaa6`, based on `7c94e869` (integration/complete).
Build dir: `build/` **inside this worktree** (already configured with the vcpkg
toolchain — ENGINE_GOTCHAS 1.4/5.3). Thin `X3Engine.exe` over `x3app.dll`.

## THE LANE (owner quotes are SPEC — NO_SLOP rule 8)
- "The car is a LOT harder to control now than it was in NFS.. Can we
  substantially increase the 'stick on the road' idea? It should be harder to
  flip the car upside down and spin it just driving"
- "Not TOOO much heavier.. and spoilers for downforce."
- "it shouldnt peg redline the whole time you drive... most cars dont do that"
- "make the final gear a 0.50:1"
- "will we need to increase engine power as we increase weight to make it fun!"

## STATUS
| item | state |
|---|---|
| 0.50:1 sixth gear | DONE (vehicle.cpp `gearRatios[5]`) |
| 5.2 final drive (paired) | DONE |
| Throttle-adaptive shift band | DONE (JoltVehicle.cpp preStep) |
| Aero downforce F=k·v² + `car_downforce` | DONE |
| Roll-rate damping + `car_rolldamp` | DONE |
| Mass increase | NOT NEEDED so far (downforce carried it) — stayed 1083.2 kg |
| H4 curb-strike A/B | **OPEN** — see below |
| Boot gate / suites / eyes-on captures | pending re-run after H4 |

## HISTORY OF THIS WORKTREE
1. W-HANDLING2 committed `a2b4dd6e` (checkpoint) — good code, but it swept in
   10 store-served `assets/converted_glb/Vehicles/*.glb`, two re-fetched nature
   GLBs and their 27 MB `*.pre-fetch.bak` blobs. **ENGINE_GOTCHAS 2.5 violation.**
2. W-HANDLING3 rewrote that commit as `1e11a1c8` — code byte-identical
   (`git diff a2b4dd6e HEAD -- app engine` = empty), all asset paths dropped,
   `.bak` files deleted from disk. `git log --name-only` over the branch shows
   **zero** `.glb` paths. Do not re-add them: the store + manifest carries them
   (`python tools/asset_store.py fetch --all`).

## MEASURED (from --test-vehicle, W-HANDLING2's run)
| metric | baseline | now |
|---|---|---|
| 0-60 | 3.05 s | 2.98 s |
| 0-100 | 5.28 s | 5.35 s |
| Top speed | 153 mph AT the 7500 limiter in 6th | 198 mph in 6th @ 7056 rpm (drag-limited) |
| Cruise 70 mph | 5th @ ~4800 rpm (the "pegged tach") | 6th @ 2372 rpm |
| NOS ×1.6 | — | 216.5 mph; 6th geartop 223 mph |
| Skidpad | — | 1.58 g, 0.07 deg roll |

## THE OPEN DEFECT (H4)
`--test-vehicle` section H4 drives a violent 0.55 s-flick slalom at ~96 mph
into two staggered 12 cm curbs (left at z=7450, right at z=7442) on a 16 km
slab. The **shipped** (downforce 1, rolldamp 2000) car trips to the 60 deg
limiter at the curbs and ends on its side. The trace proves the slalom itself
is flat (0.13 deg roll through full-lock flicks) — it is curb-only. The no-aero
car never even reaches the curbs: it spins out at the first flick (59 deg slip)
— i.e. downforce demonstrably cures the owner's spin complaint.
Next steps: curb-only A/B (no slalom), then roll-damping gate/strength; the
gate currently requires >=3 wheels grounded, which a curb strike may break.

## TRAPS PAID FOR
- Headless slab tests MUST call `car.setTerrainContactLaw(false)` — the
  procedural terrain field is 37 m up at z=-381 after the W-MOUNTAIN merge and
  hoists the test car mid-run.
- Do NOT raise anti-roll bars (>= 15 kN/m the Jolt solver pumps roll and flips
  the car at 60 Hz) and do NOT touch WheeledTuning suspension spring/damper —
  another session owns suspension.
- NEVER `--smoketest`. Check `tasklist //FI "IMAGENAME eq X3Engine.exe"` before
  every launch — the owner plays at night, ~13 sibling lanes are running.
- NEVER push. The session lead merges.
