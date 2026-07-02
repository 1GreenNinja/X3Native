# Interactive Branching Cold-Open — Design Spec

**Status:** Approved 2026-06-18 (Tim). Author: Opus 4.8 continuing the X3Native push.
**Feature:** Turn the passive cold-open film into a *hybrid-cinematic, skill-influenced, branching* space-combat intro whose outcome changes the game's starting state.

---

## 1. North star

Today's cold-open is a 67.5s passive film (`assets/cutscenes/cold_open.cutscene.json`): Jake flies, a capital ship looms, he's shot down, "SIX MONTHS LATER", he wakes in the cell → Level 1 (prisoner). We make it **playable** and **branching**:

- **Hybrid cinematic** — scripted cinematic beats interleaved with bounded *interactive combat windows* (not a free-flight sim).
- **Skill-influenced ~7% success** — a `{chance}` floor of ~7% that climbs with fight performance (up to ~35–40% for a flawless run).
- **Outcome changes the game start:**
  - **Fail / shot-down (~93%, canon):** smash-to-black → "SIX MONTHS LATER" → **wake in the cell** (existing Level 1, prisoner). Unchanged player experience.
  - **Win / escape (skill-earned):** Jake cripples/evades the ship → **a glancing hit drains his antimatter fuel** → he uses the **ion-pulse drive to coast down to the planet** → **lands outside the huge glass facility where Sarah is imprisoned** → a new **free, armed *rescuer* Act-1** (break IN, instead of escaping from within).

## 2. What we reuse (it's mostly wiring, not invention)

On `integration/honor-fable-final` today:
- **Cinematic system** (`app/cutscene.{h,cpp}`, `app/cinematic.{h,cpp}`) — mature, deterministic: Catmull-Rom camera spline, actor tracks, FX prims, audio cues, title cards, `x3.fire` events, K-to-skip. **Passive-only** (no mid-film input). Renders ships against the FORGE3D planet backdrop already.
- **Authored content + assets** — `cold_open.cutscene.json`; `JakeFighterShip.glb`; **the capital ship is already a real 200m `SpaceShip4.glb`** (the "black blob" Tim sees is the **legacy 2D fallback** `app/intro_coldopen.cpp`, used only when the JSON film fails to load — see §7 risk).
- **`{chance}` op** — `ChatCondKind::Chance` + deterministic `chanceRoll` (`app/story_ops.{h,cpp}`): the skill-biased 7% gate.
- **`StoryFlags`** (`app/story_ops.h`) — persistent branch state.

On **un-folded** `feat/space-*` / `feat/ship-*` branches (`feat/cockpit-vattalus` is the superset), all self-tested in C++:
- **`space_pilot.{h,cpp}`** — 6-DOF flight, shield→hull damage, energy-gated `fireLaser`, 1P/3P chase cam, a playable `--world space` loop.
- **`ship_ai`** (enemy fighter FSM), **`targeting`** (lock/lead/radar), **`ship_damage`** (shield/hull + destructible capital-ship subsystems), **`space_layer`** (a `Context` FSM spine: DeepSpace/WormholeTransit/EVA/AtmoDescent/Surface), **`atmo-descent`** (`descent.cpp`, on-rails orbit→ground — the **ion-pulse coast-down**), `space-env`/`stars`/`lod`.

## 3. Architecture — the Intro Orchestrator (chosen approach)

Do **not** bolt interactivity into the passive cutscene loop. Add a thin **Intro Orchestrator** that owns the intro as a beat state machine and hands off between the (untouched, pure) `CutscenePlayer` for cinematic clips and the space-combat gameplay (`SpacePilotController` + `EnemyShipManager`) for interactive windows. This keeps `cutscene.cpp`/`cinematic.cpp` clean and concentrates all new logic in one new unit.

**New unit:** `app/intro_orchestrator.{h,cpp}` — `runInteractiveIntro(HostContext&) -> IntroOutcome`.
- Sequences beats; each beat is either a `CutsceneClip` (play a named cinematic span, blocking, K-skippable) or an `InteractiveWindow` (hand control to `SpacePilotController` + `EnemyShipManager` + `TargetingSystem` for a bounded encounter with a clear exit condition + timer).
- Tracks **skill metrics** during interactive windows (hits dodged, hull remaining, subsystems downed, accuracy, time).
- At the climax, computes the **skill-biased outcome** and returns `IntroOutcome { ShotDown | Escaped }`.
- The orchestrator, not the cutscene system, draws the space scene during interactive windows (reuse the `--world space` setup: deep-space lighting, `CombatFx` tracers, the FORGE3D backdrop).

**Beat flow (default):**
1. **Cinematic** — Jake launches/flies, maneuvers (camera spline over `JakeFighterShip`).
2. **Cinematic reveal** — capital ship resolves **blob→detailed** (`SpaceShip4.glb` scale/distance ramp; ensure the 3D path runs — see §7).
3. **Interactive window 1** — dodge the opening salvo (evade bolts; skill = hits avoided).
4. **Cinematic** — the ship charges its main weapon / launches fighters.
5. **Interactive window 2** — the dogfight (`space_pilot` + `ship_ai` + `targeting`; damage the capital ship's subsystems via `ship_damage`; skill = subsystems downed + hull left + accuracy + survival time).
6. **Outcome roll** — skill-biased `{chance}` (≥7% floor, ≤~40% flawless).
   - **Escaped** → cinematic "you slip the kill-box" → **glancing hit drains antimatter** (scripted) → **ion-pulse atmo-descent** (reuse `descent.cpp`, on-rails or light-interactive) → **land outside the glass facility**.
   - **ShotDown** → cinematic killing hits → smash-to-black → "SIX MONTHS LATER".

## 4. Skill → odds

The orchestrator accumulates a normalized `skillScore ∈ [0,1]` from the interactive windows. Effective success probability `p = 0.07 + skillScore * (0.40 - 0.07)`, clamped. The roll uses the existing deterministic `chanceRoll(seed, key)` against `p` (so it's save-seed-deterministic, not frame-RNG). Skill inputs (tunable): subsystems destroyed, final hull %, salvos dodged, hit accuracy, time-to-cripple.

## 5. Branch-state wiring

- Orchestrator returns `IntroOutcome`; it writes a `StoryFlags` key `intro.outcome = "shot_down" | "escaped"` (persisted beside the save).
- **`app/app_run.cpp`** (today it unconditionally builds the cell after the film) reads `intro.outcome` and selects the Act-1 build:
  - `shot_down` → existing `level1_game` cell start (canon).
  - `escaped` → the **new surface-landing start** (§6).
- The flag also lets later content acknowledge the path (dialog/objective variants), and feeds the broader timeline system.

## 6. New content: the alternate Act-1 (escape path)

- **Ion-pulse descent beat** — reuse `atmo-descent` (`descent.cpp`): orbit→ground coast-down, fuel-drained framing, the glass facility growing below.
- **Surface landing start** — a new start scene **outside the huge glass facility**: Jake's landed ship, Jake free + armed, the facility (where **Sarah** is imprisoned) ahead. Hands to a *rescuer* Act-1 (break in) rather than the prisoner cell. This is the principal new authored content; it can reuse facility exterior/material assets and the existing rescue/objective systems.

## 7. Risks / open items

1. **The "blob" = 2D fallback.** Tim sees the legacy 2D intro (`intro_coldopen.cpp`), meaning the 3D `cold_open.cutscene.json` is failing to load on his machine (or the 200m ship renders dark). **First implementation task:** diagnose why the 3D film isn't playing and fix it — the whole feature rides the 3D cinematic path.
2. **Folding the space stack** (`cockpit-vattalus` superset) onto `honor-fable-final` is a real merge (each branch's `main.cpp` diff is ~1000–1400 lines; that code is now split into `app_run.cpp`/`world_hosts/*`). Must be re-homed onto the split + gated to the Fable bar (build green, suite, 0 VUID, alloc=0).
3. **Interactive↔cinematic seam** — clear input state on hand-off (no lingering key presses); deterministic outcome capture.
4. **Perf in combat windows** — cap enemy count + prefer emissive FX over dynamic lights (WebGL/Vulkan light caps); respect the zero-stutter discipline.
5. **Scope of the escape Act-1** — the surface-landing start is new content; keep its first slice focused (landing + approach + entry), expand later.

## 8. Components (isolation)

| Unit | Responsibility | Depends on |
|---|---|---|
| `intro_orchestrator.{h,cpp}` (NEW) | Beat state machine, interactive-window hosting, skill tracking, outcome roll, flag write | cutscene player, space_pilot, ship_ai, targeting, ship_damage, story_ops |
| space-combat stack (FOLD) | Flight, enemy AI, targeting, damage, space FSM, atmo-descent | engine RHI/physics |
| `app_run.cpp` (EDIT) | Call orchestrator; branch Act-1 build on `intro.outcome` | intro_orchestrator, level1_game, new surface start |
| surface-landing Act-1 (NEW) | The escape-path Act-1 outside the glass facility | level/loader, rescue/objective systems |
| `cold_open.cutscene.json` (EDIT) | Cinematic clips referenced by the orchestrator + blob→detailed reveal | cutscene format |

## 9. Testing / gates (Fable bar)

- The fold + each new unit: build Release+Debug green (ALL_BUILD + **verify exe relinked by mtime** — the X3Engine/x3engine case-collision no-ops the app exe), full `--test-*` suite 0-new-fail, `--smoketest` 0 VUID / allocationCount=0, screenshots in baseline basin where output shouldn't change.
- New self-tests: `--test-intro` (beat sequencing, outcome capture, skill→p mapping, deterministic roll, flag write), space-combat tests already exist on the folded branches (`--test-space`, ship AI/targeting/damage).
- Headless-deterministic where possible; interactive windows verified via scripted-input or state assertions.

## 10. Build order (for the plan)

1. **Fix the 3D cold-open path** (stop the 2D-fallback blob) — small, unblocks seeing the real film.
2. **Fold the space-combat stack** onto `honor-fable-final` (gated) — foundation.
3. **Intro Orchestrator** + skill tracking + outcome roll + `--test-intro`.
4. **Branch wiring** in `app_run.cpp` (`intro.outcome` → cell vs surface).
5. **Cinematic beats** (blob→detailed reveal; interactive-window cinematic bookends).
6. **Ion-pulse descent beat** (reuse atmo-descent).
7. **Surface-landing Act-1** (escape path; first focused slice).
