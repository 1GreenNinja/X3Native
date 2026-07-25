# Animation Enrichment — POLISH pass screenshots

Grounded pose QA for the two fixes in this polish pass. Every Blender shot uses
the orchestrator's GROUNDED method: floor plane at the feet, a near-level camera
(~1 m eye, ~3.4 m out), the clip's own action set, read at a REPRESENTATIVE
frame of the motion arc (peak for a flinch, not the midpoint). Judge each AFTER
against the same rig's Idle rendered the same way.

## FIX 1 — enemy hit-reactions read as hits

### Humanoid biped (chief_martinez / marcus_webb, 19-bone rig)
- `chief_hitreact_BEFORE_nearknockdown.png` — the shipped bake at its peak. The
  head/torso are thrown ~55 deg back, chin pointing at the ceiling — a backbend /
  near-backward-fall, NOT a hit recoil. This is the over-pitch the orchestrator
  flagged.
- `chief_hitreact_AFTER_peak.png` — retuned peak. A CONTAINED ~20 deg cumulative
  snap-back with the shoulders/forearms hitched up into a guard; feet planted.
  Reads unambiguously as "just took a hit," not a fall. What the pixels show: head
  tipped back a little, chin up slightly, shoulders hunched forward/in, weight
  still over both boots.
- `chief_hitreact_AFTER_settle.png` — same clip at t0.70. Back to a near-neutral
  upright stance (arms lowering, head almost level), so the runtime hard-cut back
  to locomotion has no pop. Honest read: fully recovered, essentially Idle.
- `marcus_hitreact_AFTER_peak.png` — the SAME biped baker on marcus's rig. Subtler
  than chief because marcus's long-clawed arms don't hunch the same way, but the
  head clearly snaps back (chin up) vs marcus's level-headed Idle, chest opens,
  feet planted. Honest read: a legible flinch, weaker than chief's but distinct
  from Idle and definitely not a knockdown. (Cheer/Drink stay deferred — rig
  shoulder skinning, per the prior wave.)

### Single-Root aliens (canon_grey / mantis / nordic / saurian)
A one-bone rig can only move the whole body — accepted; the goal was to make the
whole-body motion READ.
- `alien_grey_hitreact_BEFORE_subtle.png` — shipped bake at peak: a tiny backward
  lean, nearly indistinguishable from Idle.
- `alien_grey_hitreact_AFTER_peak.png` — punched to a sharp whole-body recoil
  (hard base-pivot tip back + upward hitch, peak ~frame 2). Pixels: the alien is
  plainly staggered backward, head tilted back, clearly different from Idle. Feet
  stay grounded (tail/heels anchor it).
- `alien_saurian_hitreact_AFTER_peak.png` — same core baker on the tall,
  tailed saurian: a clean backward recoil, the heavy tail counter-balancing so it
  reads as "stung," not toppling. Spot-check that the core bake generalizes.
- `alien_grey_attack_BEFORE_weakbow.png` — shipped Attack at strike: a shallow
  forward bow, barely a reach.
- `alien_grey_attack_AFTER_lunge.png` — retuned strike: a committed ~40 deg
  forward lunge with a downward weight-drop, returning to rest. Pixels: the alien
  drives its whole body forward, arms leading — reads as a strike. (In-engine
  monster.cpp adds a procedural forward draw-offset ON TOP, so it lunges even
  further at runtime.) At this magnitude it is aggressive but stops short of a
  face-plant.

## FIX 2 — a seat under seated crowd agents

- `crowd_sit_seat_blender.png` — AnnaCasual's Sit pose with a 0.44 m crate placed
  the way the engine places it (feet on floor, crate resting on the same floor,
  centered under the agent). The crate top meets her hips: she sits ON it, feet
  planted, torso upright. This is how the 0.44 m height was chosen (grounded
  seat-fit; 0.38/0.44/0.50 compared).
- `crowd_sit_seat_INENGINE.png` — the real thing: `--screenshot-crowd`, cropped to
  the seated citizen under the blue spotlight. She sits on the dark crate — butt
  on the top, boots on the checker floor, no floating gap. (The crate reads dark
  in the dim club, but the contact is correct.) Full frame:
  `docs/screenshots/livingworld/club_crowd.png`. 0 VUID on that capture.

## Verification summary
- `--test-anim` T6 (each new clip's palette != Idle): all green after the re-bake.
- ~~Pre-existing fails left as-is (not this wave): `--test-anim` T5,
  `--test-locomotion` L3/L5/L6.~~ **FIXED in `feat/alien-locomotion` (guard-loco
  wave): those fails were the guard "glide" itself — dead 2-key locomotion clips
  inside chief/marcus `_anim.glb` (+ store-drift reinstalling them on every
  fetch). See `docs/KNOWN_BUGS.md` L12. Both tests fully green now; do NOT
  re-add an ignore label if they regress — they gate real walking.**
- `--smoketest`: 0 VUID / 0 validation / 0 VK_ERROR.
