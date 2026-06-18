# Interactive Branching Cold-Open — Implementation Plan

> **For agentic workers:** each Phase is a self-contained, gated workstream. Implement TDD against the REAL code in the named files (read them first — do not fabricate). Every phase ends at the Fable gate. Spec: `docs/design/INTERACTIVE_INTRO_DESIGN.md`.

**Goal:** Make the cold-open a hybrid-cinematic, skill-influenced (~7%→~40%) branching space fight whose outcome forks the game start (canon cell vs. land-outside-the-glass-facility rescuer Act-1).

**Architecture:** A thin Intro Orchestrator sequences cinematic clips (pure `CutscenePlayer`) and interactive space-combat windows (`space_pilot`+`ship_ai`+`targeting`+`ship_damage`), captures a skill score, rolls a skill-biased `{chance}`, writes `intro.outcome` to `StoryFlags`, and `app_run.cpp` branches the Act-1 build on it.

**Tech stack:** C++/Vulkan X3Native; base branch `integration/honor-fable-final`. Build = `cmake --build build --config Release` (ALL_BUILD; VERIFY exe mtime advanced — X3Engine/x3engine case-collision no-ops the app exe; else msbuild `build/app/X3Engine.vcxproj`).

---

## GLOBAL GATE (every phase)
Build Release+Debug green; **verify exe relinked (mtime)**; relevant `--test-*` 0-new-fail vs baseline; `--smoketest` Release 0 VUID + allocationCount=0; basin-identical where output shouldn't change; branch off the prior phase's tip (serial deps below); never touch `main`/`empire-fold`; push the phase branch; co-author trailer. Report on a freshly-relinked exe only.

---

## Phase 1 — Fix the 3D cold-open path (the "blob")  [no deps]
**Branch:** `fix/coldopen-3d-path` off `integration/honor-fable-final`.
**Problem:** Tim sees the legacy 2D fallback (`app/intro_coldopen.cpp`, the "blob") — the 3D film (`cold_open.cutscene.json` → `SpaceShip4.glb`) is failing to load or render dark.
**Steps (agent):** Read `app/app_run.cpp` cold-open block (~the `runCutsceneWindowed` call + the `ranFilm`/fallback at the `intro_coldopen` path) and `app/cinematic.cpp` `loadCutsceneFile`. Reproduce: run `--world intro` / default, log WHY it falls back (JSON parse error? missing asset path? `SpaceShip4.glb` load fail? lighting making the 200m ship read black?). Fix the root cause so the 3D film plays with a visible, lit, detailed capital ship.
**Deliverable:** the 3D cold-open plays (no 2D-fallback) with the detailed capital ship visible.
**Gate:** global gate + a visible-capital-ship confirmation (log "[cutscene] cold open 3D film playing" + the ship GLB loaded; screenshot if a host allows).

## Phase 2 — Fold the space-combat stack  [no deps; FOUNDATION for 3-7]
**Branch:** `integration/space-stack-folded` off `integration/honor-fable-final`.
**Task:** Bring the space-combat pillar onto the split base, gated. Source: `feat/cockpit-vattalus` (superset) — it carries `space_pilot`, `app/space/ship_ai`, `targeting`, `ship_damage`, `space_layer`, `descent` (atmo), `eva`, `space-lod`, `decloak_vfx`. These branched PRE-split (their `main.cpp` host loops are 1000-1400 lines) → **re-home** the `--world space` host loop + CLI/test plumbing into the split structure (`app/world_hosts/*`, `app/cli.*`, `app/test_registry.*`, `app/app_run.cpp`), like the DamageType re-home. Combat gameplay files (`app/space/*`, `space_pilot.*`) mostly aren't relocated → 3-way apply.
**Steps (agent):** map the branch lineage; create `app/space/` + `space_pilot.{h,cpp}`; re-home `--world space` as `app/world_hosts/host_space.cpp`; wire `--test-space` + ship AI/targeting/damage tests through the split CLI/test plumbing; add new TUs to `app/CMakeLists.txt`. SCOPE-REVIEW: pull ONLY the space-combat stack, exclude unrelated churn (companion/glass_lounge churn the branches carry).
**Deliverable:** `--world space` playable on the base; space tests pass.
**Gate:** global gate + `--test-space` + ship-ai/targeting/damage tests green; `--world space` smoketest 0 VUID/alloc=0.

## Phase 3 — Intro Orchestrator + skill + outcome  [deps: P2]
**Branch:** `feat/intro-orchestrator` off `integration/space-stack-folded`.
**Files:** Create `app/intro_orchestrator.{h,cpp}`; Test `app/self_tests.cpp` (`runIntroSelfTest`) + `--test-intro` via `app/test_registry.*`.
**Deliverable:** `IntroOutcome runInteractiveIntro(HostContext&)` — a beat state machine alternating `CutsceneClip` (blocking `CutscenePlayer` span, K-skip) and `InteractiveWindow` (hand control to `SpacePilotController`+`EnemyShipManager`+`TargetingSystem`, bounded by exit-condition/timer, clearing input on hand-off). Accumulates `skillScore∈[0,1]` (subsystems downed, hull%, salvos dodged, accuracy, time). Computes `p = clamp(0.07 + skill*(0.40-0.07))`, rolls deterministic `chanceRoll(seed, "intro.outcome")` < p → `Escaped|ShotDown`. Writes `StoryFlags["intro.outcome"]`.
**Steps (agent):** read `cutscene.{h,cpp}` (CutscenePlayer), `space_pilot.h`, `ship_ai.h`, `story_ops.h` (chanceRoll/StoryFlags). TDD `--test-intro`: beat sequencing, deterministic outcome for fixed seed+skill, skill→p mapping bounds, flag write, input cleared on hand-off.
**Gate:** global gate + `--test-intro` green.

## Phase 4 — Branch the game start on the outcome  [deps: P3]
**Branch:** `feat/intro-branch-wiring` off `feat/intro-orchestrator`.
**Files:** Modify `app/app_run.cpp` (the post-intro block that today unconditionally builds the cell).
**Deliverable:** call `runInteractiveIntro`; read `intro.outcome`; `shot_down`→existing `level1_game` cell; `escaped`→the surface-landing start (Phase 7; until then a stub host that logs + falls to cell so this phase is testable independently). `--skipintro`/dev cvar to force an outcome for testing.
**Steps (agent):** TDD: forcing `intro.outcome=escaped` selects the surface path; `shot_down` selects the cell; default canon path unchanged.
**Gate:** global gate + the branch-selection test; canon path basin-identical.

## Phase 5 — Cinematic beats + blob→detailed reveal  [deps: P3; soft-dep P1]
**Branch:** `feat/intro-cinematics` off `feat/intro-orchestrator`.
**Files:** Modify `assets/cutscenes/cold_open.cutscene.json` (split into the named clips the orchestrator plays: flight, reveal, charge, escape-stinger, shot-down-stinger); author the capital-ship **scale/distance reveal** (blob→detailed ramp on `capital_ship` actor).
**Deliverable:** orchestrator plays real authored clips between interactive windows; the reveal reads as a ship emerging, not a blob.
**Gate:** global gate + cutscene validation tests pass; the clips load + play via the orchestrator.

## Phase 6 — Ion-pulse descent beat (escape path)  [deps: P2, P4]
**Branch:** `feat/intro-descent` off `feat/intro-branch-wiring`.
**Files:** reuse `descent.cpp` (atmo-descent, folded in P2); orchestrator escape branch invokes it.
**Deliverable:** on `escaped`, after the antimatter-drain stinger, an ion-pulse coast-down (on-rails or light-interactive) ending at the surface, the glass facility growing below.
**Gate:** global gate + descent self-test (if present) + the escape sequence runs headless to the landing hand-off.

## Phase 7 — Surface-landing Act-1 (escape path)  [deps: P4, P6]
**Branch:** `feat/act1-surface-start` off `feat/intro-descent`.
**Files:** Create `app/world_hosts/host_surface_start.cpp` (or a LevelDoc-authored start) — Jake's landed ship + the glass facility exterior (where Sarah is imprisoned) + a free/armed rescuer entry; wire into the `escaped` branch from P4.
**Deliverable:** the escape branch lands the player outside the glass facility, free + armed, into a rescuer Act-1 (first focused slice: landing → approach → entry).
**Gate:** global gate + a smoketest of the surface start (0 VUID/alloc=0).

---

## CONSOLIDATION (after phases)
Reconcile P1+P3+P4+P5+P6+P7 onto `integration/space-stack-folded` (most touch the orchestrator/app_run/cutscene — serial deps keep this mostly linear), gate as a unit, fast-forward `honor-fable-final`, rebuild Tim's exe.

## Self-review (done)
Spec coverage: every spec §3-§6 item maps to a phase (orchestrator→P3, skill→odds→P3, branch wiring→P4, blob fix→P1+P5, ion descent→P6, surface Act-1→P7, fold→P2). No placeholders (each phase has concrete files/deliverable/gate). Type consistency: `IntroOutcome`/`runInteractiveIntro`/`intro.outcome` used consistently P3-P7. Dependency order is explicit and acyclic.
