# THE CLONE — Act 1 Finale Boss + Sarah Companion (Plan)

_Written 2026-07-25 (crash-durable, saved BEFORE coding). The Act-1 climax on F7 Executive/
Rooftop. Grounded in existing systems — this EXTENDS scaffolding, it does not invent._

## The beat (canon, from MASTER_GAME_PLAN)
F7 Executive Lab is the Act-1 finale. **The Clone** — a clone of Jake — is the boss, **3 phases**:
1. **SEPARATION** — the Clone separates from Sarah (she's restrained/collared); fight the Clone + its escort.
2. **NEURAL COLLAR** — a minigame: destroy Sarah's neural collar (frees her).
3. **MUTATED HYBRID** — the Clone mutates into a hybrid final form; last stand.
**Sarah wakes, arms up, and FIGHTS BESIDE JAKE** once freed. Killing the Clone + the executive-desk
objective = the descent gate opens (Act 1 → Act 2).

## What ALREADY exists (extend these — verified 2026-07-25)
- **F7 finale scaffolding** — `app/app_run.cpp` ~1487-1575: "F6 Executive / F7 Rooftop = Act-1 finale;
  F7 the Clone boss + a 7-strong escort, a gated F7 rescue captive; executive desk ARMS ONLY after the
  F7 finale (Clone has fallen); top F6/F7 + Overseer/Clone/Sarah." So the encounter slot + escort +
  post-boss desk gate are already sketched — we fill in the ACTUAL 3-phase fight + Sarah combat.
- **Multi-phase boss machine** — `BossPhase` enum + phase machine on `MonsterSystem` (Martinez:
  `martinezPhase()`/`phase()`; `SaurianWarlord` in `canon_aliens.*` = "Boss + HP>=400 + phase machine +
  memory-flash"; `MultiPodBoss`; `MemoryHunter` L12). `BossPhaseFn` hook exists (dialog.h/monster.h).
  → The Clone reuses this pattern: HP-gated phase transitions (Phase1→2→3).
- **The Clone's MODEL = Jake's own rig** — `assets/rigged_glb/Jake_22_actions.glb` (Mixamo, 22 clips,
  incl. rifle set). It IS Jake's clone → same mesh, TINTED (sickly/pale or emissive-veined) via the
  material tint the monster path already supports. Zero new asset. Phase 3 "mutated hybrid" = swap to a
  mutated variant tint/scale + emissive, or a mutated GLB if one exists.
- **Sarah's MODEL** — reuse a rescue-girl rig (`AnnaCasual.glb` / `AnnaTactical.glb`, already skinned +
  animating via the #48 rescue Skinner path). If a distinct Sarah is wanted later, Meshy auto-rig
  (reference_meshy_autorig, proven 7/25). Start with the tactical Anna rig.
- **Companion system** — the rescue captives already FOLLOW + animate (`canon_play.cpp` / `rescue.cpp`,
  Skinner-driven idle/walk). Companion COMBAT (ally fires at enemies) is the new piece to add on top.
- **Neural-collar minigame** — reuse the HoloTerminal / desc-mechanics interactable E-chain (the
  `HoloTerminalSystem` + keypad/desc-mechanics framework already drive coded world-effects). The collar
  = an interactable with a short destroy sequence (hold-E / 3-hit / timed), gated to Phase 2.

## Build lanes (fan out — 2 parallel agents, then integrate)

### LANE A — The Clone 3-phase boss + neural-collar minigame (the FIGHT, F7)
- Spawn The Clone on F7 using Jake_22_actions.glb, TINTED (clone look), as a Boss MonsterSystem with the
  phase machine (mirror Martinez/SaurianWarlord). HP-gated transitions.
- **Phase 1 (Separation):** Clone + the existing 7-strong escort; Sarah present but restrained (collared,
  non-combat). Clone fights with the rifle clips (it has Jake's moveset).
- **Phase 2 (Neural collar):** at HP threshold, Clone staggers/retreats; the collar interactable becomes
  active — destroy it (HoloTerminal/desc-mechanics interactable) to free Sarah. On success → fire the
  "Sarah freed" event (Lane B hooks this).
- **Phase 3 (Mutated hybrid):** Clone mutates (tint/scale/emissive shift, or mutated GLB), buffed, final
  stand. On death → set the "Clone dead" flag that (with the desk objective) opens the descent gate.
- Boss HP/phase HUD (reuse the Martinez boss-phase HUD path in level1_game.h).
- Gate: `--test-clone` (spawn, 3 phase transitions fire at thresholds, collar-destroy advances P1→P2→P3,
  death sets the descent flag). Headless-safe.

### LANE B — Sarah companion combat (wake + fight beside Jake)
- Sarah spawns restrained on F7 (Anna tactical rig). On the "Sarah freed" event (Lane A Phase 2 success),
  she WAKES → becomes a combat companion: follows Jake (reuse rescue-companion follow), and FIRES at the
  nearest hostile (new: ally-combat — give her a hitscan weapon + target-nearest-enemy AI + fire on
  cooldown + the rifle-aim/fire clips). She should not block Jake / not stack (reuse the personal-space
  separation from #25/ecology). Barks (freed / in-combat / Clone-down) via the dialog system.
- Keep her ALIVE-gated (if downed, she's incapacitated not deleted — this is the emotional beat).
- Gate: `--test-companion-combat` (freed event wakes her, she acquires + fires on a hostile, follows,
  respects separation). Headless-safe.

### INTEGRATION (14900K, after both land)
- Wire Lane A's "Sarah freed" event → Lane B's wake; Lane A's "Clone dead" → the existing F7 desk/descent
  gate. Playtest the full 3-phase fight on F7 windowed (VISUAL review — the owner's eyeball, per the
  verify-art-visually rule). Tune HP thresholds, escort count, collar timing, Sarah's DPS so she helps
  but Jake still carries.

## Gotchas / discipline
- Clean-room; original work. Reuse OUR systems only.
- Kill stale X3Engine.exe before builds; run from repo root (asset paths).
- Boss/companion ANIM must be judged floor+level-cam (reference_grounded_anim_qa) — no-floor close-ups lie.
- Commit + push per milestone (chronic-crash box; new 14900K is stable but don't tempt it).
- Worktree agents branch off a main-based commit → parent CHERRY-PICKS the single feature commit back
  (merging drags the whole lineage in — learned on rifthub).
- Visual correctness = owner's eyeball; headless gates prove no-crash/no-leak/logic, not "looks AAA."

## Open question flagged
Is there a mutated/hybrid GLB for Phase 3, or do we tint+scale Jake's rig? Lane A: check assets first;
if none, tint+scale is the acceptable first pass (mutated GLB = a follow-on / Meshy job).
