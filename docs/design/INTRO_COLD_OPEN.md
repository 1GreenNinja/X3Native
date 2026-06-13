# EFLZ — Cold-Open / Prologue (Tim, 2026-05-26)

> **Status:** design intent, NOT yet built. Recovered after being lost to compaction —
> capture so it isn't lost again. Folds into `docs/design/EFLZ_NARRATIVE.md` (currently
> starts at the detention cell; this is the beat *before* it).

## The intro sequence
1. **Spawn in the spaceship.** Jake starts at the controls of a small ship in flight (not
   the cell). The player **actually flies it** — player-controlled flight, ~**45 seconds**.
2. **The hit.** A **larger ship** fires an **energy pulse** that strikes Jake's ship. The
   ship is crippled / crashes.
3. **Fade to black.**
4. **"Six months later."** Jake **wakes up in his detention cell** — which is exactly where
   Level 1 currently begins (Jake's Cell, the canonlevel spawn). The crash + capture is how
   he ended up imprisoned in the facility.

## Why it matters / hooks
- Gives the player agency + a set-piece in the first minute (flight), then the title/time
  card, then the grounded survival-horror open we already have.
- Ties the cold open directly to the existing Level-1 spawn (no wasted geometry — it hands
  off to Jake's Cell).
- Leverages engine tech we already have: a flyable-vehicle mode exists in the codebase
  (`--world fly`/drive/boat paths in main.cpp), the analytic sky, and the fast renderer —
  so the flight + the bigger ship + the pulse are achievable.

## Build sketch (later)
- A `--world intro` (or a front-end "New Game" → intro) flight scene: ship cockpit/3rd-person,
  WASD/mouse flight for ~45s, a scripted bigger-ship flyby that fires the energy-pulse
  projectile → impact → screen fade → "6 MONTHS LATER" card → load `--world canonlevel`
  spawned in Jake's Cell.
- Skippable after first view. Keep it short (the 45s is the whole point — tight + dramatic).
