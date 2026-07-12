# ⚠️ ATTENTION — FableAAA Sessions: READ BEFORE TOUCHING EITHER INTEGRATION LINE

**Written 2026-07-07 by the black-wires/hatch session (11940b27, @14900k) on Tim's direct order:
"Fold those into playable-build, talk to that session about managing the effort!"**

This file is the coordination contract between every Claude session working X3Native.
It lives at the repo root **on both integration branches**. If you change the plan, change
THIS FILE in the same commit, and mirror it to the other branch. The shared task board
(Claude tasks #40, #73) and FleetCommand/Slick (slick.x3designs.net) carry the live chatter;
this doc carries the decisions.

---

> ## 🌍 THE WORLD MERGE — DONE 2026-07-09 (Tim's order 2→3→1→4; session 392f6e4d)
> **`canonlevel` (the default boot) is now THE ONE WORLD**: the 7-floor facility with
> the real elevator down through the strata to Club 1127 (seam 1), the glass exterior
> wrapping the real tower with a walkable breach onto the apron (seam 2, `bae1f59`),
> and the planet streaming in around it — city, ocean base, crash site, mountain
> ranges, 15 km horizon (seam 3, `3f72754`). **Policy: `docs/design/WORLDS.md`** —
> the other 18 `--world` modes are DEV SHORTCUTS; new content lands in the canon
> world via shared modules (facility_exterior / club1127 / strata / elevator are the
> pattern). Do not author game content against a dev slice — that is how the
> two-line split below happened.

---

## 1. THE SITUATION — we forked the same game twice (again)

Two integration lines are active and **architecturally incompatible**:

| | `integration/honor-fable-final` (HFF) | `integration/playable-build` (PB) |
|---|---|---|
| Base | e443f27 (Great Fold) + **111 commits** | e443f27 + **35 commits** |
| Architecture | **#28 monolith split** (main.cpp 684 lines; cli/app_run/world_hosts/test_registry TUs) | **pre-split monolith** main.cpp (~15k lines, inline worlds/flags/audio) |
| Carries | Waves 1–6: cell R4–R7 polish, canon trapdoor+secret room+holo terminal, hatch audio, enemy PBR + W2-D attack/death anims, W2-A2 wiring, W3 room recipes + floors 2–7 (`loadCanonTower`), W4 captives/VIGIL/patrols, W5 level-4.5 Nexus + interrupt-rescue + **Sarah endgame**, W6 ships-PBR/white tower, THE FABLE CODEX, committed audio tree | Upper floors 2–7 content (`loadCanonBuilding`), **THE STRATA DESCENT** (real layered geology, explorable caves, facility → Club 1127), core-elevator wiring, elevator showcase (`--world elevator` centerpiece), car turntable shots, monster skeleton-fit fixes, **lightning-gun playtest tune (Tim's live feedback)**, its own attack-clip bake |
| Duplicate features (built TWICE, differently) | `loadCanonTower`, W2-D attack+death+lunge, W3-2 upper floors, hatch two-panel + R7 read | `loadCanonBuilding`+floorBase, attack-only clip bake, upper-floor-content, hatch two-panel (older) |

**A direct `git merge` was attempted 2026-07-07 and deliberately ABORTED**: 13 conflicted
files resolve fine until `main.cpp`, where the merge demands re-homing ~9k lines of PB's
monolith-inline features into the split structure. That is a **directed project**, not a
conflict resolution — done sloppily it silently drops features.

> ## ✅ FOLD COMPLETE — 2026-07-07/08 (session 392f6e4d, I9DevPC)
> **There is ONE integration line again: `integration/playable-build`, now pointing at the
> folded split-architecture head.** `integration/honor-fable-final` is retired (deleted per
> §2.7); the old PB monolith tip is preserved at tag **`archive/pb-monolith-20260707`**
> (f3d9ac1). A full parity audit of all 36 PB-unique commits ran before the collapse: 30
> verified FOLDED/SUPERSEDED at code/asset level; the 6 genuinely missing were landed as
> **R-7..R-10** (see the queue below). Gate at the collapse head: 22/23 suites green, 0 VUID,
> allocationCount=0, eye-verified proof shots in `shots/r9_*`/`r10_*`. The one red suite,
> `--test-boottime` (9.0 s vs 2.0 s budget, all in "canon gameplay spawns"), was PROVEN
> pre-existing at 5a81be4 by rebuilding and re-measuring the pre-fold head — it is filed as
> its own issue (synchronous enemy-GLB spawns from the W4/W8/W9 waves), not a fold casualty.
> **All new work lands on `integration/playable-build`.** Do not resurrect the HFF name.

## 2. THE FOLD PLAN (executed — kept for the record)

**Direction: re-home PB's unique content ONTO HFF**, then retire one name. Rationale: the
split architecture is the future (the Codex, the test registry, every Wave, and all current
tooling live on it); PB's unique value is **content**, and content re-homes cleanly (we have
the playbook: the DamageType re-home, the space-stack re-home, W2-A2).

Re-home queue, in order (each = one session/agent, one branch, Fable gates):

1. **R-1 Lightning playtest tune** — ✅ **LANDED on HFF `33530f5`** (2026-07-07, 11940b27 session):
   damage 14 / fireRate 8 / mag 200 / reserve 600 + keep `DamageType::Energy`. Tiny; land first.
2. **R-2 Monster skeleton-fit regression fixes** — ✅ **LANDED `81a5bf1`, then wall-probe half
   REVERTED `d140a74`** (bisect-proven double regression in HFF's smaller-hitbox context).
   Audit note: the revert touched ONLY the wall-probe change; the bestiary/deathragdoll scale
   fixes are moot on the split line (it sizes via `kRealModelScale`, never skeleton-fit).
3. **R-3 THE STRATA DESCENT** — ✅ **LANDED `a798cfe`** (`app/strata.*`,
   `world_hosts/host_strata.cpp`, live elevator descends the 9 strata to Club 1127).
4. **R-4 Elevator showcase** — ✅ **LANDED `a798cfe`** (bundled with R-3).
5. **R-5 Upper-floor content reconciliation** — ✅ **LANDED `1613c7d`** (squads + pickup
   economy F2-F7, `--screenshot-upperfloors`; `loadCanonTower` is the one tower loader).
6. **R-6 Car turntable** — ✅ **LANDED** (`--screenshot-car` in app/screenshot_hosts.cpp).
   Bonus: PB's realistic access keypad re-homed as `eb55fdc` (+ `--test-keypad`).
7. **FINAL: one line.** ✅ **DONE** — but only after a **full parity audit** of every
   PB-unique commit caught 6 features the R-queue never listed. Landed as:
   - **R-7 `384ef9e`** — canonical facility is the DEFAULT boot (PB c3538d3; legacy tower
     stays at `--world level1`); all 5 weapon-variety GLB reskins (hash-verified vs PB
     manifest); terminal hint bottom-center with the "(code 1278)" spoiler dropped + music
     muted by default (PB 4b9f067).
   - **R-8 `107a4a9`** — barrel EXPLOSION fireball (`spawnExplosion`) + chaingun tracer as
     camera-facing ribbon (`drawTracerBillboard`), grafted around the split line's
     lightning-bolt propagation (PB 013b144 + 583a212).
   - **R-9 `1126618`** — exterior inter-floor skirt on the stacked canon tower (PB 03256bd;
     Nexus platform tiers excluded by design; before/after proof `shots/r9_*`).
   - **R-10** — `--fx-demo` proof hooks (explosion + tracer in the capture) + eye-verified
     shots `r10_fxdemo.png` / `r10_default_boot.png`.
   Audit verdicts for everything else (SUPERSEDED: W2-D anims, W2-E dedup, canon_45
   scaffold climb, `kRealModelScale` sizing, HFF rig manifest) are in the audit record —
   28 content commits traced, none dropped silently.

**Rules now that the fold is DONE:**
- **All work lands on `integration/playable-build`** (or short-lived branches off it).
- No branch lives longer than a day without landing (see `project_x3native_branch_crisis_0524`
  — we have been here before).
- Every re-home carries the full Fable gate: Release relink (mtime-verified — the
  X3Engine/x3engine case-collision no-ops the exe at exit 0), `--test-*` 0-new-fail,
  `--smoketest` 0 VUID / allocationCount=0, and **eye-verified screenshots for anything visual**
  (read your own screenshots; agents oversell blockouts).
- Check the shared task list before starting ANYTHING — features here have been built twice
  because sessions didn't look.

## 3. ALREADY ON HFF — do not rebuild
Cell polish R4–R7 (ceiling, tinted pipes, de-blown lights, hatch seam-stripe read + rim +
status lens + hatch spot), canon trapdoor/secret-room/holo-terminal port, **hatch audio**
(SecretRoomSounds: keypad clicks, wrong-code buzz, unlock-edge chime, servo loop + seat
thunk — commits 9c0b69e + 012280a), the committed `assets/audio/` tree **including
`interact/` (keypad_click/chime/buzz/servo_loop)** — the elevator's four cue loads now point
at those committed WAVs too (they silently resolved to nothing on boxes without the external
Unity packs since 288ce2a).

## 4. THE ELEVATOR BAR — the Babylon Club 1127 masterpiece (Tim: "truly review it")

Reviewed 2026-07-07: `D:\GameDev\X3Engine\src\features\x3-elevator.js` (1,806 lines).
This is the quality bar for the C++ elevator + strata effort. What makes it a masterpiece
is that **the ride itself is content** — every second between floors is authored:

- **10-state FSM with real motion physics** (IDLE→DOORS_CLOSING→ACCEL→CRUISE→DECEL→ARRIVING→
  DOORS_OPENING→DOORS_OPEN, + EMERGENCY_STOP + FREEFALL). Accel 6, decel 8, 14 m/s cruise,
  8 m decel window. C++ elevator.h already ports this 1:1. ✅
- **The glass observation wall + LIVE STRATA readout**: 9 authored geological layers
  (Sky & Concrete → Foundation → Limestone → Granite → Basalt → Obsidian → **Crystal Veins
  (violet glow)** → **Magma Zone (ember glow)** → **Alien Substrate (alien glow)**), animated
  glowing veins snaking through the special layers, depth tick-marks every 50 m.
  **PB's strata descent made this PHYSICAL — real geology out the glass. That synthesis
  (real strata + cabin presentation) is the whole point of R-3.**
- **Twin OLED viewscreens** — live telemetry sells the fiction: LEFT = GEOLOGICAL SURVEY
  (depth in metres, current stratum name, speed km/h, ambient temp = 18 + depth×0.03 °C,
  lifetime odometer, "WARNING: ALIEN SUBSTRATE" past −120 m). RIGHT = floor directory with
  live `>` current / `*` target markers.
- **A complete audio persona, all procedural**: motor pitch tracks speed; wind noise scales
  with speed; 60 Hz mains hum while moving; random cable CREAKS every ~3–5 s in motion;
  floor-pass dings (880 Hz bell + 1.5× harmonic); hydraulic door hiss + metal slide +
  delayed 55 Hz THUNK; keypad clicks **pitched per digit** (800 + digit×50 Hz); wrong-code
  buzz; and **procedural MUZAK** — a 72 BPM pentatonic melody over LFO-swept pad chords that
  fades in when the doors close and stops on arrival. The lift has manners.
- **DISCO MODE — code 1127 on the cabin terminal**: disco ball materializes, blacklight +
  strobe + 4 orbiting colored spots, glass hue-cycles, a procedural 128 BPM club track
  (kick / hi-hat / off-beat hi-hat / Cm7 stab sequencer), **DISCO SLOW (0.25× descent — the
  ride becomes the party)**, terminal and floor LED flip magenta, and it **unlocks CLUB 1127
  at −200 m ("x3_theDeep")**. Enter 1127 again to shut it down.
- **HORROR EVENTS**: 8 % chance on arrival (always armed on SUB + F3) — emergency stop with
  alarm + cabin shake, or a light-flicker + cable creak. The elevator has moods; riders
  never fully trust it.
- **Rider craft**: proximity auto-open when you approach a closed idle car, ride-along
  physics, digit keys captured only while riding, buttons glow-pulse toward the target,
  the LED tracks floors mid-flight with a speed bar, blinking terminal cursor, 4 steel
  cables rising 300 m above the car.

**C++ status vs the bar** (full line-by-line audit vs the 1,806-line JS ran 2026-07-08,
session 392f6e4d): 288ce2a doors/ding/motor-loop/keypad/buzz/status-UX ✅ · e4c9686 muzak +
cable creaks + horror events ✅ · R-3 real strata out the glass ✅ (exceeds the JS canvas) ·
disco toggle/slow/descend-to-−200 + per-digit key pitch + floor-pass dings ✅ (WAV-rate
adaptations; no synth path exists in the audio engine — offline `tools/gen_elevator_audio.py`
bakes are the pattern) · f49d209 TWIN OLED TELEMETRY ✅ · 29e2ef7 layered door SFX + the
cable-slip Freefall set-piece ✅.

**AUDIT GAPS — ✅ ALL CLOSED 2026-07-08/09 (392f6e4d, I9DevPC):**
1. ✅ Disco is LOUD (`131dc05`): baked 128-BPM club track rides the 1127 toggle (muzak
   yields), 4 Hz strobe in `applyDiscoCue`, magenta terminal/LED flip (+ fixed the
   stuck-purple ceiling after disco-off).
2. ✅ THE RIDE ENDS SOMEWHERE (`b58e43f`): Club1127World lazy-builds on the accepted
   1127 code, ticks/draws in the game world, owns the light budget at The Deep, and the
   rider steps out at club.spawn() when the doors open at −200. Proof:
   `shots/r12_club_deep.png` (live in-game capture inside the club).
3. ✅ 4 steel shaft cables ride the cab roof (`131dc05`).
4. ✅ Rider craft: `autoOpenFor()` — an idle sealed car opens for near feet (wired in the
   game loop + the walkable host; FSM test F10 guards it); the walkable host's digit
   capture is UN-stubbed — digits feed the KEYPAD while riding (1127 works), floor calls
   only from a landing, numpad = keypad anywhere.

**The elevator bar vs `x3-elevator.js` is CLOSED — and it exceeds the JS**: real strata
geology out the glass, a real club at the bottom, and a cable-slip set-piece the
reference never had. Ride it: `--world level1`, code 1127 at the cab terminal.

## 5. COMMS
- **This doc** = decisions. Amend it in-commit. (Single line now — no mirroring needed.)
- **Shared task board** = live claims. Claim before you build. Task #73 tracks the fold.
- **FleetCommand / Slick** (slick.x3designs.net) = cross-machine chatter (@14900k daemon).
- Commit trailers: keep co-author + session tags so archaeology stays possible.

*— written in honor of Fable, who never shipped a black wire he hadn't looked at.*
