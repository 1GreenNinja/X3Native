# EFLZ Golden Path Plan

**Date:** 2026-07-16  
**Status:** PLAN  
**Product:** Escape From Lab Zero (first game on X3Native)  
**Specs:**  
- `specs/EFLZ_GOLDEN_PATH.spec.md`  
- `specs/EFLZ_SURFACE_FACILITY_HANDOFF.spec.md`  
- `specs/EFLZ_CANON_MISSION_SPINE.spec.md`  

**Related:** `docs/design/INTERACTIVE_INTRO_PLAN.md`, `docs/design/RESCUE_SETPIECE_DESIGN.md`, `docs/design/EFLZ_WORLD_STRUCTURE.md`, `docs/CAMPAIGN_LEDGER.md`

---

## 1. Goal

Make the **default product path** a single honest Act-1 journey:

1. Interactive cold-open (skill branch)  
2. Facility entry (cell **or** surface rescuer)  
3. Combat through the tower  
4. F2 rescue  
5. F7 Sarah extraction  
6. Campaign continue into Act-2 (minimal stub acceptable)

No dead-end “skill reward,” no dual production spines, no mid-game without objectives.

---

## 2. Current state (as of review)

### Product entry

| Flag | Role |
|---|---|
| Default / `--world canonlevel` | **THE GAME** |
| `--world intro` | Same tower, forces cold-open path |
| `--skipintro` | Skip prologue |
| `--world level1` | **Legacy** hand-coded spire (reference only) |
| `--world surface` | Escaped-branch landing (incomplete) |

### Branch tree

```
runInteractiveIntro (app/intro_orchestrator.*)
  ├─ ShotDown (~93%) → blackout → canon cell → CanonPlay tower → Sarah → WIN → "TO BE CONTINUED"
  └─ Escaped (skill) → ion descent → host_surface_start → DEAD END (no interior)
```

### Dual spine (structural risk)

| Spine | Geometry | Encounters |
|---|---|---|
| **Canon (product)** | JSON `EscapeLab48_AllFloors_v2` | `CanonPlay` |
| **Legacy** | Hand plates | `Level1Game` + SpireMid/Top/Nexus/SubLevels |

Mission JSON + several mid/top beats still favor legacy. Product default does **not** build Spire* modules.

### Existing tests (protect what works)

`--test-introorch`, `--test-introbranch`, `--test-canonplay`, `--test-goldenpath`, `--test-rescue`, `--test-descmech`, `--test-level1`, `--test-surfacestart`, `--test-act2*` (Act-2 off-path)

---

## 3. Target golden path (canonical)

| # | Beat | Entry | Owner |
|---|---|---|---|
| 0 | Cold-open beats + skill roll | Windowed canon/intro | `intro_orchestrator` |
| 1A | ShotDown → cell wake | `intro.outcome=shot_down` | `app_run` + `CanonPlay` |
| 1B | Escaped → surface → **facility interior** | `intro.outcome=escaped` + handoff | surface host → canon |
| 2 | F1 arm + hall combat + Martinez | Proximity / rooms | `CanonPlay` |
| 3 | Elevator F1→F7 + RIFT optional | Lobby rooms | `ElevatorSystem` |
| 4 | F2 triage (3 girls) | Medical Bay **or** any Ward | `RescueSystem` |
| 5 | Extract companions to F2 lobby | Companion AI | `RescueSystem` |
| 6 | F3–F6 squads, desc verbs, bosses | Elevator ascent | `CanonPlay` + `DescMechanics` |
| 7 | F4.5 optional Nexus | Open-ceiling access | `Canon45` |
| 8 | F7 Clone → Sarah → Helipad WIN | `clone.defeated` | `CanonPlay` |
| 9 | Act-2 handoff (L8 surface emergence stub) | Post-extract | new host or embed |

StoryFlags that must survive the whole path (non-exhaustive):  
`intro.outcome`, `intro.landed`, `girl.freed.*`, `girl.extracted.*`, `clone.defeated`, rescue interrupt tiers, desc mech flags (`f4.coolant_sabotaged`, etc.)

---

## 4. Phases

### Phase 0 — Documentation lock (this package)

- [x] Map beats, owners, gaps  
- [x] Specs for path, surface handoff, mission spine  
- [ ] Integrator acknowledges P0 board order  

### Phase 1 — Surface → facility handoff  [P0-1]

**Problem:** Escaped players never enter the real tower.  
**Spec:** `specs/EFLZ_SURFACE_FACILITY_HANDOFF.spec.md`

**Deliverable options (pick one in implementation PR; prefer A):**

| Option | Mechanism | Notes |
|---|---|---|
| **A (preferred)** | At breach interact / volume: `hc.switchWorldTo = "canonlevel"`, `hc.spawnAtKey` = entrance/rescuer spawn | Reuses world-load loop; preserves surface art as prologue |
| **B** | In-place build of canon interior inside surface host | Heavy; duplicates default-host world build |
| **C** | Teleport into already-built dual world | Only if surface and tower share one host (they do not today) |

**Also:**

- Escaped spawn: **armed**, **outside cell**, `StoryFlags` intact  
- Surface pistol must either wire full `WeaponSystem`/`Arsenal` or be replaced by real loadout on handoff  
- Tests: force escaped → handoff → player in Entrance/apron room with weapon  

**Gate:** Release build; existing intro tests; new `--test-surface-handoff` (or extend surfacestart); eyes-on screenshot at breach entry.

### Phase 2 — Freeze dual spine  [P1-3]

**Rules:**

1. **Product = `canonlevel` only.**  
2. `--world level1` remains for regression / art reference; not demoed as “the game.”  
3. Any golden-path beat that exists only on Spire* must be **ported to `CanonPlay` / room data** or **explicitly cut** from Act-1 scope with a design note.  
4. Single Sarah / Clone implementation for product (CanonPlay).  

**Port candidates (audit list):**

- Lena chat / mid-floor scripts (if still desired on product)  
- Chen return / sub-levels  
- Chorus multipod richness vs `Canon45`  
- Mission flag polling currently tied to Level1Game  

**Gate:** Checklist in PR: every Act-1 beat either on canon or marked CUT. `--test-goldenpath` covers product only.

### Phase 3 — F2 rescue arming + UX  [P2-2]

**Problem:** Timers arm on room name `"Medical Bay"` while girls live in Ward A/B/C — soft fail risk.

**Deliverable:**

- Arm hub when player enters **any of:** Medical Bay, Ward A, Ward B, Ward C (or authored trigger volumes)  
- HUD always shows three timers while hub active and any captive alive  
- Document Lost / 5-min expire → boss transform as **intended** fail state, not a soft-lock  

**Files:** `app/rescue.*`, host arming site in default host / `CanonPlay` tick, HUD.

**Gate:** `--test-rescue`; manual enter-ward-first playtest.

### Phase 4 — Canon mission spine  [P1-4]

**Spec:** `specs/EFLZ_CANON_MISSION_SPINE.spec.md`  
**Format:** `docs/design/MISSION_FORMAT.md`

**Deliverable:**

- `missions/canon_act1.mission.json` (name flexible)  
- Poll flags from `CanonPlay` / StoryFlags the way `pollLevel1MissionFlags` does for Level1Game  
- Product default: mission HUD **on** for canon (or auto-on when mission file present)  
- Beats at minimum: wake → arm → Martinez → F2 triage → floor climb → clone → Sarah → helipad  

**Gate:** Headless mission self-test; playtest mid-game always has an objective string.

### Phase 5 — Post-win Act-2 handoff  [P2-1]

**Problem:** Win card + `"TO BE CONTINUED"` ends the campaign.

**Minimal deliverable (stub OK):**

1. After `sarahExtractedThisFrame` + win card (or skip card in test),  
2. Load Act-2 surface emergence:  
   - Prefer: real `--world act2` host registered in destinations + dispatch, **or**  
   - Embed `Act2World::build` via `switchWorldTo`  
3. Serialize timeline / rescue flags so Act-2 can read who lived  

**Not required in this phase:** full L8–L15 content polish.

**Gate:** `--test-act2` still green; new test or smoketest that post-win worldMode becomes act2 (or equivalent).

### Phase 6 — Mid-game content density (ongoing)

- Desc mechanics remaining TODOs  
- Boss ladder tuning  
- Optional F4.5 polish  
- Companion extraction clarity  

Track under campaign ledger; not blocking P0–P2.

---

## 5. Explicit non-goals

- Replacing interactive intro with passive film only  
- Deleting `--world level1` immediately (keep as reference until port complete)  
- Full Act-2 campaign in the same PR as surface handoff  
- Companion romance / full ally AI fold unless already on trunk  

---

## 6. File touch map (expected)

| Area | Paths |
|---|---|
| Intro branch | `app/intro_orchestrator.*`, `app/app_run.cpp` (or `app_run_intro.cpp` post-split) |
| Surface | `app/world_hosts/host_surface_start.cpp`, `facility_exterior.*` |
| Canon play | `app/canon_play.*`, `level_loader.*` |
| Rescue | `app/rescue.*`, HUD |
| Mission | `app/mission.*`, `missions/*.json`, host poll |
| Act-2 | `app/act2_*. *`, `world_hosts.cpp`, `destinations.cpp` |
| Destinations | `app/destinations.*` |

---

## 7. Definition of done (Act-1 “honest”)

- [ ] Escaped skill run can enter the facility and reach F2  
- [ ] Shot-down run still works, basin-identical on non-intro systems  
- [ ] One production spine (`canonlevel`) owns all required Act-1 beats  
- [ ] Mission/objective HUD continuous from cell through Sarah  
- [ ] F2 timers arm correctly from wards  
- [ ] Post-Sarah loads Act-2 stub (not only a card)  
- [ ] All listed golden-path tests green  

---

## 8. Risk register

| Risk | Mitigation |
|---|---|
| World switch tears state | Use existing `switchWorldTo` loop; persist StoryFlags to disk as intro already does |
| Dual Sarah bugs during port | Feature-flag legacy Sarah; product only CanonPlay |
| Mission spam | Short objective strings; don’t re-enable legacy Level1 mission on canon by accident |
| Act-2 content thin | Stub world with clear “Act 2 WIP” objective — honesty over emptiness |
