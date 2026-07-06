# Waves 3 & 4 — World Flesh-Out, then NPC Flesh-Out

Sources: ONE_GAME_REVIEW_MANIFEST_2026-07-01 (unlanded: STFC intro stack, companion/ally
system, ship interiors, car roster), the rescue-storyline intent (captive girls, per-girl
dialog, companion arc), the canonical level truth (EscapeLab48_AllFloors_v2 = 53-room F1,
wide hall, cells both sides), WORLD_AND_EDITOR_PLAN.md, the hidden-level-4.5 / facility
tower spec, and Wave 1/2 state (charter + WAVE2_PUNCHLIST). Law: the AAA Studio Charter.

## Gate zero (in flight now)
- W2-E: canon DOOR SEATING + `--test-levellint` (door-seat, containment; flood-fill next).
  Nothing in Wave 3 builds on a floor that violates Law 1.
- W2-A merge + the director's app_run wiring round (footsteps→real WAVs, player pain/land
  sink, reload/dry-fire playback, viewmodel in screenshots). Closes Wave 2.
- Tim playtest of Wave 2 (audio + animation motion are un-judgeable in stills).

## WAVE 3 — WORLD: make Floor 1 a PLACE (scale the calibrated treatment)
The cell is the calibration; the recipe is proven (palette, inset law, pools-not-floods,
ceiling panels, glazing, contact shadows). Scale it by ROOM TYPE, not room-by-room hand work:
- 3.1 `RoomDressing` recipes keyed on canon room names/types: MainHall (priority 1 — it is
  the dark-blue tunnel today), DetentionCell row (reuse cell recipe with variation seeds),
  SecurityStation, ResearchLab, Mess/Storage, BossArena. Each = kit tiling + 2-4 motivated
  lights + 1-2 hero props + ambient emitter. One fork per 2-3 room types, disjoint recipe
  files, cell_dressing stays frozen as the reference.
- 3.2 Signage + wayfinding: exit signs, cell numbers, painted floor lines to objectives
  (the canon data knows room names — render them into the world, AAA facilities are labeled).
- 3.3 Per-room-type ambience (assets committed in Wave 2; hall needs its own loop + the
  lab a machine hum — source from packs like W2-B did).
- 3.4 Neighbor-room visibility: rooms seen through cell glass must at least get the
  ceiling/wall treatment (they are the current immersion leak).
- 3.5 Floor-1 completeness playtest trace (Gate C): spawn → hall → security → research →
  boss → elevator, no noclip, lint green throughout.
DELIVERABLE: Tim walks Floor 1 and every room reads dressed. THEN the same recipes scale to
Floors 2-4 + the level-4.5 monster section + the surface facility (the escape-branch Act-1)
almost for free — that is why recipes, not hand passes.

## WAVE 4 — NPC: make it PEOPLED (the rescue storyline starts here)
- 4.1 THE CAPTIVES: girls in the ward cells (models on hand: Anna variants, Lena; 9 chat
  trees already load from docs/design/narrative/chat_trees). Per-girl cell dressing +
  idle/sit poses (W2-D's bake pipeline extends), E-interact dialog, FREE-HER beat →
  follow-to-safe-room escort (first slice of the companion/ally system from the ONE-GAME
  manifest — land the smallest real slice, not the whole fold).
- 4.2 MARTINEZ presence: boss-arena entrance beat (door + bark + music sting) using the
  new Attack/Death clips + boss chat tree.
- 4.3 VIGIL on the holo terminals: the cell terminal (F1's port) + hall terminals get the
  hybrid NPC brain (task #9: chat-tree spine + LLM color, offline-safe fallback).
- 4.4 Guard LIFE: patrol routes on the canon room graph, per-species barks (extend GameCue
  with EnemyType — punch-list P2), taunt-at-distance behavior already verified OK.
- 4.5 The L2 interrupt-rescue set-piece (kill mid-impregnation, per-girl aftermath dialog)
  — designed in Wave 4, BUILT after Floor 2 gets its Wave-3 recipe pass.

## Wave 5+ (Tim prioritizes when Waves 3-4 land)
STFC intro stack fold · ship-interior cluster · 25-car roster + LATE NIGHT SPEED shops ·
facility exterior to the tower spec (white concrete, black glass bands) · level 4.5 reveal.

## RED-LINE (AD-2 whole-game survey, 66 shots, 2026-07-05 — director-verified)
The renderer already produces bible-grade frames (20_planet, 06_showroom exterior,
23_car, 26_ddgi). The gap is CONTENT-side, concentrated exactly where Wave 3 aims:
1. Canon cell + hall: walls run bright/even (violates §2 darks-own-the-frame); hall is
   a flat monochrome blue wash with no key, no warm-vs-cold, no accent discipline.
   -> Wave 3 recipes + AD-1 fog/grade + AD-3 real materials. Priority ONE.
2. --world space: NO STARFIELD (flat navy) behind an otherwise good capital-ship
   composition — wire the existing nightsky starfield in. Cheap, do in Wave 3.
3. --world surface: pure blockout (black plane + glass slab). Full dressing to the
   white-concrete/black-glass tower spec = Wave 5 scope, noted.
4. 25_alert is the closest interior to the bible — treat it as the internal reference;
   fix: red becomes lenses/pools not a full wall wash; stop the door blowing pink-white.
BUG LIST (filed, not art): --screenshot-showroom-ragdoll segfaults at shutdown after
writing; main menu prints RESOLUTION: 0 x 0; worldmap POI labels overlap at center;
RT lamp shots show black speckle at emitters; showroom-fp camera renders ~black;
survey cameras outside the shell see kit backfaces (sightline containment).
