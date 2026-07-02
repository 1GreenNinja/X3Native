# LEVEL LINT BASELINE — the honest violation count

Gate A of the x3-level-authoring doctrine (`.claude/skills/x3-level-authoring/SKILL.md`).
Tool: `app/level_lint.{h,cpp}`; run with `X3Engine --test-levellint`.

This is the **before** snapshot — the quantified state of Tim's "abysmal" verdict on the
level architecture. Every count below corresponds 1:1 to geometry the canonical builder
(`buildCanonFloor`) emits, because the lint reads the same `CanonFloor` data + the shared
`kCanon*` constants the builder generates from (`level_loader.h`).

## Baseline (first green build, exe 2026-07-01 20:04)

| World | door-seat | seam | height | reach | contain | TOTAL |
|-------|----------:|-----:|-------:|------:|--------:|------:|
| **Floor 1 (Detention Level)** | 23 | 7 | 61 | 0 | 0 | **91** |
| **Whole building (7 floors fused)** | 44 | 16 | 82 | 31 | 11 | **184** |

`--test-levellint: FAIL`.

## Root causes by category (the repair targets — fix in the loader, not room-by-room)

### HEIGHT — 61 (F1) / 82 (bldg) — the biggest bucket
- **RAMP_TOO_STEEP** (majority): the builder's threshold ramp targets slope
  `kCanonRampSlope = 0.70` (≈35°). LAW 3 caps ramps at **30°** (tan 0.5774). Every
  floor-delta transition (hall→cell 0.75 m, hall→room 0.50 m, boss tiers 1.5–3 m) is
  therefore built too steep. A tiny-rise variant reads 32° where the min-run clamp
  (`kCanonWallT+0.6 = 0.8 m`) binds before the slope does.
  **Category fix:** lower `kCanonRampSlope` so the *built* slope is ≤30° for every rise
  whose run isn't clamped, and lengthen the min run so clamped rises also clear 30°.
- **HEIGHT_NO_TRANSITION**: gap-bridge corridors that span a floor delta (0.25–2.25 m)
  drop the player onto a **bare ledge** — the bridge builder lays a flat deck with no
  step/ramp at the lower mouth. **Category fix:** ramp (or step, for ≤0.25 m) the
  gap-bridge mouth to the lower room's floor.
- **RAMP_DOESNT_FIT** (1): Elevator Lobby→Elevator Shaft is modelled as a ramped
  adjacency but it is the elevator spine — should be `CrossLevel`, not a ramp.

### DOOR-SEAT — 23 (F1) / 44 (bldg)
- **DOOR_UNSEATED** (0.25/0.25 m off both wall planes): rooms authored with a ~0.5 m
  air gap between them, classified *Adjacent*, so the cut door lands centred in the void
  0.25 m off each wall. Correlates 1:1 with the GAP_SEAM entries. Also every stacked
  "Elevator Lobby → corridor" door on F1..F7.
- **BRIDGE_MOUTH_OFF_WALL**: the gap-bridge mouth's cross-axis coordinate is taken from
  the corridor, not clamped into the **overlap of both rooms' facing-wall spans**, so the
  1.6 m opening is punched past the partner room's wall (e.g. corridor z-span 11..42 vs
  cell z-span 35..41, mouth at 32.2). **Category fix:** clamp the mouth centre to the
  shared span midpoint.
- **DOOR_PAST_EDGE** (2, bldg): opening pokes past a room corner (Cold Room/Decon → F3
  boss).
- **TUBE_MISSES_ROOM** (7, bldg): the hidden F4.5 spire vertical links drop descent tubes
  at XZ columns that don't overlap one of the two rooms' footprints.

### SEAM — 7 (F1) / 16 (bldg)
- **GAP_SEAM** (4): the ~0.5 m void bands under Main Hall ↔ Entrance/Admin/IT/Network —
  same authoring gap as DOOR_UNSEATED; you can see the void through the threshold.
  **Category fix:** snap the two rooms' facing walls flush (close the gap) OR bridge it.
- **DOUBLED_WALL / DOUBLED_FLOOR**: two coplanar overlapping faces at a room boundary →
  z-fight shimmer (Boss Approach/Arena shared Z=-7 wall; cell-hall/bottom-hall coplanar
  floors; several F2/F4/F7 corridor↔room shared planes). **Category fix:** extend the
  builder's coplanar dedup to these boundary pairs.

### REACH — 0 (F1) / 31 (bldg)
- Floor 1 is fully connected. In the fused building, **31 upper-floor rooms** (F2 boss,
  F3/F4/F5/F6/F7 wings, rooftop cluster) are unreachable from the F1 spawn: some hang off
  gap-bridges with an illegal height delta (marked not-walkable by the reach flood), and
  the rooftop/drone rooms are outright floating. **Category fix:** follows from the height
  + containment fixes.

### CONTAIN — 0 (F1) / 11 (bldg)
- **INTERPENETRATION** (4): F5 Main Corridor ↔ Central Control Hub; Rooftop ↔
  Helipad/Guard Posts — overlapping rooms with no doorway.
- **FLOATING_ROOM** (7): Drone Bay Alpha/Beta and the whole rooftop cluster (Elevator
  Exit, Rooftop, Helipad, Guard Post A/B) have **no doorway at all** — geometry detached
  from the structure.

## AFTER — repair results (same build, category fixes in the loader/resolver)

| World | door-seat | seam | height | reach | contain | TOTAL | vs baseline |
|-------|----------:|-----:|-------:|------:|--------:|------:|------------:|
| **Floor 1** | 2 | 3 | 0 | 0 | 0 | **5** | 91 → 5 (−95%) |
| **Whole building** | 15 | 12 | 0 | 26 | 11 | **64** | 184 → 64 (−65%) |

Per-category deltas (Floor 1 → building):
- **height**: 61→0 / 82→0. Ramp slope 0.70→0.55 (≤30°); gap-bridge mouths now
  ramped up to the deck. (Elevator Lobby↔Shaft's impossible ramp became CrossLevel.)
- **door-seat**: 23→2 / 44→15. Gap-bridge mouths seated on the wall-span overlap;
  0.5 m void adjacencies bridged; elevator shaft = vertical link.
- **seam**: 7→3 / 16→12. 0.5 m GAP_SEAM voids closed by bridging.
- **reach**: 0 / 31→26. Ramped gap-bridges reconnected 5 formerly-ledged rooms.

### Remaining residuals (documented, not yet fixed)
- **Floor 1 (5):** 2 BRIDGE_MOUTH_OFF_WALL at corner pairs whose facing-span overlap
  is narrower than a 1.6 m door (unfittable without wider rooms — data); 2
  DOUBLED_FLOOR (small overlap-corner z-fight, West/East Cell Hall ↔ Bottom Hall);
  1 DOUBLED_WALL (Boss Approach ↔ Boss Arena — adjacent rooms with differing floor
  AND ceiling heights, which the coplanar-wall dedup can't collapse).
- **Building (64):** the F1 residuals + upper-floor authoring defects on F2–F7:
  DOUBLED_WALL at height-delta corridor↔room boundaries, 7 TUBE_MISSES_ROOM in the
  hidden F4.5 spire vertical links, 26 UNREACHABLE (rooftop/drone/boss wings hung
  off illegal bridges or floating), 11 CONTAIN (4 INTERPENETRATION + 7 FLOATING_ROOM
  with no doorway at all). These are data/authoring fixes on floors off the Floor-1
  golden path; scoped as follow-up.

## GATE B — visual review (screenshots read by the author, docs/screenshots/architecture/)
`mainhall_entrance_seam` 7/10, `westcellhall_doored_ramps` 8/10,
`service_gapbridge_mouths` 7.5/10, `boss_approach_arena` 7.5/10. No sky/void through
seams, no floating slabs, doors seated in wall openings, floors continuous. Canon
lighting is dim (per-room lights only for the visible set) but geometry is legible.

## GATE C — golden-path trace (`--test-goldenpath`)
Completes 5/7 beats with collision on, no noclip: Jake's Cell → Main Hall → Security
→ Research → Medical → Armory. Blocked at Armory → Boss Arena on the Boss
Approach/Boss Arena boundary (the DOUBLED_WALL residual above). The reachability
flood (lint reach=0 on F1) confirms the rooms are graph-connected; the physical block
is that one height-delta shared wall, not a disconnection.

## Suite + smoketest
`--test-canonlevel` 16/16, `--test-building` 10/10, `--test-canonplay` 9/9.
Release `--smoketest`: exit 0, 30 frames + swapchain recreate OK, 0 VUID,
VMA `allocationCount=0`.

## Known lint blind spot (live-playtest finding, 2026-07-01)
Tim hit freestanding **ornate door/portal FRAMES** (elevator-portal design language:
white+black hex-cutout, magenta/yellow accents) standing mid-floor with open void behind
them — not seated in any wall. These are **placed Scene entities** from the elevator/portal
content code, NOT `CanonDoorway` records, so the data-level door-seat check above does not
yet see them. Tracked as a follow-up check (scan placed door/frame entities against wall
geometry). See the repair log for status.
