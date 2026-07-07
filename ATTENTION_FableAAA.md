# ⚠️ ATTENTION — FableAAA Sessions: READ BEFORE TOUCHING EITHER INTEGRATION LINE

**Written 2026-07-07 by the black-wires/hatch session (11940b27, @14900k) on Tim's direct order:
"Fold those into playable-build, talk to that session about managing the effort!"**

This file is the coordination contract between every Claude session working X3Native.
It lives at the repo root **on both integration branches**. If you change the plan, change
THIS FILE in the same commit, and mirror it to the other branch. The shared task board
(Claude tasks #40, #73) and FleetCommand/Slick (slick.x3designs.net) carry the live chatter;
this doc carries the decisions.

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

## 2. THE FOLD PLAN (recommended — Tim has final say)

**Direction: re-home PB's unique content ONTO HFF**, then retire one name. Rationale: the
split architecture is the future (the Codex, the test registry, every Wave, and all current
tooling live on it); PB's unique value is **content**, and content re-homes cleanly (we have
the playbook: the DamageType re-home, the space-stack re-home, W2-A2).

Re-home queue, in order (each = one session/agent, one branch, Fable gates):

1. **R-1 Lightning playtest tune** — ✅ **LANDED on HFF `33530f5`** (2026-07-07, 11940b27 session):
   damage 14 / fireRate 8 / mag 200 / reserve 600 + keep `DamageType::Energy`. Tiny; land first.
2. **R-2 Monster skeleton-fit regression fixes** — 🔒 **CLAIMED: 11940b27 fork on `fold/pb-remainder` (wt-r256), R-5+R-6 bundled** — (PB cbf7999) — diff against HFF monster.cpp;
   HFF's W2-D anim system WINS (attack+death+lunge supersedes PB's attack-only bake;
   manifest keeps HFF's GLB hashes), but the bestiary/deathragdoll scale fixes come across.
3. **R-3 THE STRATA DESCENT** — 🔒 **CLAIMED: 11940b27 fork on `fold/strata-descent` (wt-r3strata), R-4 bundled** — (PB fd94c92 + ed1a403) — PB's crown jewel. Re-home as a
   `world_hosts/host_strata.cpp` (or fold into the canon tower path) + wire the LIVE elevator
   to descend through it. See §4 — this is the Club 1127 realization; treat it as sacred.
4. **R-4 Elevator showcase** (`elevator_showcase.h`, `--world elevator`) — re-home behind the
   screenshot/world dispatch.
5. **R-5 Upper-floor content reconciliation** — BOTH lines populated floors 2–7. HFF's W3-2/W4
   set (bosses, captives, VIGIL, patrols, Sarah spine) is the superset and is already
   integrated with the endgame; PB's `feat/upper-floor-content` gets DIFFED for anything
   unique (item placements, `--screenshot-upperfloors` capture path, `--test-upperfloors`
   assertions worth keeping alongside `--test-goldenpath`). Drop `loadCanonBuilding` for
   `loadCanonTower` unless the floorBase diagnostics are still wanted — do NOT ship two
   whole-tower loaders.
6. **R-6 Car turntable** (`--screenshot-car`) — re-home into screenshot_hosts.
7. **FINAL: one line.** After R-1..R-6 gate green, fast-forward/rename so `playable-build`
   points at the folded result (Tim's canonical name wins), and **delete the stale head** so
   this never happens again.

**Rules while the fold runs:**
- **FREEZE PB for new feature work.** New work lands on HFF (or short-lived branches off it).
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

**C++ status vs the bar**: 288ce2a landed doors/ding/motor-loop/keypad/buzz/status-UX ✅ and
the four cue WAVs are now committed ✅. **Missing:** muzak, disco mode + Club 1127 unlock +
disco-slow, horror events, the OLED telemetry pair, per-digit click pitch, layered door SFX
(hiss+slide+thunk), floor-pass dings while cruising, freefall state usage, cable creaks,
glass observation read onto PB's real strata. That list is the elevator punch-list for
whoever takes R-3/R-4 — the target is not parity with the JS, it is the JS **plus** real
geology out the window.

## 5. COMMS
- **This doc** = decisions. Amend it in-commit, mirror to the other branch.
- **Shared task board** = live claims. Claim before you build. Task #73 tracks the fold.
- **FleetCommand / Slick** (slick.x3designs.net) = cross-machine chatter (@14900k daemon).
- Commit trailers: keep co-author + session tags so archaeology stays possible.

*— written in honor of Fable, who never shipped a black wire he hadn't looked at.*
