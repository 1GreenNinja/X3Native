# EFLZ Golden Path Audit — P1-3 (Freeze the dual spine)

**Date:** 2026-08-17
**Lane:** X3Native CANON CAMPAIGN
**Status:** AUDIT (this document is the P1-3 deliverable; it gates the P1-4 mission-spine work)
**Basis:** `docs/design/EFLZ_GOLDEN_PATH_PLAN.md` §3 target table, cross-checked against the
live code (`app/canon_play.*`, `app/app_run.cpp`, `app/intro_orchestrator.*`, `app/spire_*`,
`app/canon_45.*`, `app/act2_world.*`, `app/mission.*`, `app/rescue.*`).

## Classification legend

| Status | Meaning |
|---|---|
| **ON CANON** | The flag/code path lives in `CanonPlay` / room data / the canon host (`--world canonlevel`). No port needed. |
| **CUT** | Legacy-only (`Spire*` / `Level1Game`) and NOT wanted on the product path. One-line design note given. |
| **PORT NEEDED** | Legacy-only or partially-on-canon, but STILL WANTED on the product path. Seed of future work. |

---

## The Act-1 beat table

| # | Beat (plan §3) | Status | Evidence / note |
|---|---|---|---|
| 0 | Cold-open beats + skill roll | **ON CANON** | `app/intro_orchestrator.*` writes `intro.outcome=<shot_down\|escaped\|capital_killed>` (suffix-flag encoding, `writeOutcomeFlag`); `app/intro_coldopen.*` ships the lead-in (`--world intro` forces it; default `canonlevel` skips to cell). |
| 1A | ShotDown → cell wake | **ON CANON** | Canon cell spawn (`--world canonlevel`); `--test-opening` (O1-O5) asserts the wake-in-cell contract (unarmed, sidearm out of reach, spawns dormant, calm alert). |
| 1B | Escaped → surface → facility interior | **ON CANON** | `app/world_hosts/host_surface_start.cpp` + the handoff in `app_run.cpp` (~L4441: `importEscapedIntroFlags` → `cheatArm` → spawn at `entrance` → objective `REACH SARAH`); gated by `--test-surfacehandoff`. [P0-1 landed; the interior hunt is real, not the old DEAD END.] |
| 2 | F1 arm + hall combat + Martinez | **ON CANON** | `CanonPlay`: sidearm pickup in Jake's Cell (`pickupRoom`), Main-Hall + cell-guard squads (`m_mainHall`/`m_cellGuards`), Martinez boss in the Boss Arena (`m_martinez`). `--test-canonplay` asserts the spawn anchoring. |
| 3 | Elevator F1→F7 + RIFT optional | **PORT NEEDED** | The `ElevatorSystem` FSM (cabin/keypad/disco/RIFT stop) is built ONLY on the legacy `--world level1`/elevator hosts, not on canon (`app_run` gates it behind `!canonWorld`; canon has no `elevator.build`). Canon traverses via the **stairwell** (`app/canon_stairs.*` + `StairNavChain`). RIFT = `app/rifthub.*` (out of scope for this lane). **Port**: a functional elevator/RIFT ride on canon is still wanted. |
| 4 | F2 triage (3 girls) | **ON CANON** | `CanonPlay::m_rescue` — Aria/Keisha/Emily in Medical Bay + Ward A/B/C, each guarded; `--test-rescue` (R0-R5) + `--test-canonplay` (distinct per-girl dialog) green. Medical-bay arm gate lives in the canon host (room-name reach). |
| 5 | Extract companions to F2 lobby | **ON CANON** | Extraction point = `F2: Elevator Lobby` (`canon_play.cpp` ~L539); host sets `girl.extracted.<aria\|keisha\|emily>` + the goodbye bark (`app_run.cpp` ~L10489). |
| 6 | F3–F6 squads / desc verbs / bosses | **ON CANON** | Squads = `m_upperEnemies` (F2-F7 themed rooms); bosses = `m_floorBosses` ladder (F2 Dr. Chen, F3 Exp#7, F4 Collective, F5 Swarm, F6 Overseer, F7 Clone); desc verbs = `app/desc_mechanics.*` (W9-1) keyed on `f4.coolant_sabotaged` / `f5.hacked`. |
| 7 | F4.5 optional Nexus | **ON CANON** (reduced) | `app/canon_45.*` (the Chorus Nexus: whispers/name-call/apex wake/cavern creatures) is wired into the canon host. **Richness gap**: the legacy `--test-nexus` off-elevator **multi-pod** boss is NOT ported — see "Chorus multipod" below. |
| 8 | F7 Clone → Sarah → Helipad WIN | **ON CANON** | `cloneDefeated()` (F7 "Jake's Clone" ladder boss) → `trySarahRescue` gate → `sarahExtractedThisFrame()` win latch; `--test-goldenpath` (G1-G9) green. |
| 9 | Act-2 handoff (L8 emergence stub) | **PORT NEEDED** | `app/act2_world.*` (L8/L9) EXISTS and `--test-act2` is green, but the post-win handoff is NOT wired — `app_run.cpp` ends at the win card + `TO BE CONTINUED`. Phase 5 owns this. |

---

## The named suspects (plan §2 "Port candidates")

| Suspect | Status | Note |
|---|---|---|
| **Lena chat / mid-floor scripts** | **CUT** | `Lena` lives only in `SpireMidFloors` (`app/spire_mid.*`, F5 synth-bay captive), built on the legacy `--world level1` host (`app_run.cpp` ~L3087). Canon has NO Lena — the canon rescue dialog is Aria/Keisha/Emily (F2) + Sarah (F7). Cut from Act-1 canon; **PORT NEEDED** only if a F5 scavenger beat is still wanted on product (then re-home her tree + `onRescue` onto `CanonPlay`). |
| **Chen return / sub-levels** | **CUT** (sub-levels); **ON CANON** (Chen boss) | `app/spire_sublevels.*` ("hidden Floor-7 sub-levels + Dr. Chen Return Mission", `subLevels.onRescue` payoff) is legacy-only. The identity itself is NOT cut: Dr. Chen IS the F2 ladder boss "Mutated Dr. Chen" on canon (`canon_play.cpp` ~L649). The **Return Mission** (SL3 rescue payoff) is the cut part. |
| **Chorus multipod richness vs Canon45** | **PORT NEEDED** | `Canon45` is the reduced single-apex Nexus (one "The Chorus" apex + creature-bucket vocals). The legacy nexus (`--test-nexus`) is an off-elevator **multi-pod** boss fight. The richer multi-pod form is still wanted; not blocking the spine. |
| **Mission flag polling tied to Level1Game** | **PORT NEEDED** | `pollLevel1MissionFlags` (`app/mission.cpp`) reads `Level1Game` public queries only; canon has no poll adapter, so `g_missiondoc` only drives the legacy level. **This is exactly P1-4** — a `pollCanonMissionFlags` adapter is the port. |

---

## StoryFlags that survive the whole path (the "one world" contract)

The mission spine (P1-4) must advance on the SAME flags the chat trees and canon host
already write — never a second, divergent lane:

- `intro.outcome=shot_down` / `intro.outcome=escaped` / `intro.landed` — intro branch (intro_orchestrator).
- `girl.freed.aria` / `girl.freed.keisha` / `girl.freed.emily` — a girl became a Companion (E-rescue, `app_run.cpp` ~L10481).
- `girl.extracted.aria` / `girl.extracted.keisha` / `girl.extracted.emily` — reached the F2 lobby (`~L10491`).
- `<girl>.interrupted` — rescue resolved WOUNDED (raw-vs-composed tree variants).
- `clone.defeated` — F7 clone down, Sarah's field key invalid (`~L10518`).
- `sarah.freed` / `sarah.freed.sync` — Sarah is a Companion (`~L10531`).
- `sarah.extracted` — the WIN latch (`~L10539`).
- Desc-mech flags `f4.coolant_sabotaged`, `f5.hacked` — the W9-1 verbs.

**New canon-only poll flags (added by P1-4, `pollCanonMissionFlags`)** — these mirror
what `pollLevel1MissionFlags` already does for Level 1, and are the ONLY new keys:
`canon.leftCell`, `canon.armed`, `canon.martinez.dead`, `canon.floor.<n>` (player reached
floor n via `StairNavChain::floorForY`), plus kill-counter milestones.

---

## Net result

- The **dual spine is frozen**: `canonlevel` (JSON `EscapeLab48_AllFloors_v2` + `CanonPlay`)
  owns every Act-1 beat 0-8. `--world level1` remains a regression/art reference only.
- **CUT** from Act-1 canon: Lena mid-floor chat, the Chen Return Mission sub-levels.
- **PORT NEEDED** (future-work seed, non-blocking): functional canon elevator + RIFT ride
  (beat 3), Chorus multi-pod richness (beat 7), the post-win Act-2 handoff (beat 9), and —
  as this audit's own enabler — the canon mission poll (beat "spine", P1-4).
- **P1-4 is unblocked**: every minimum beat the mission spine needs
  (wake → arm → Martinez → F2 triage → floor climb → clone → Sarah → helipad) is ON CANON
  with a flag or CanonPlay query to advance on.
