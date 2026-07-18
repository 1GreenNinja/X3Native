# Animation Enrichment Wave — screenshots

In-engine (Vulkan) captures + Blender close-up pose QA for the combat/gesture
clips added this wave. A static frame can't fully prove motion — the rigorous
proof is the headless palette test `--test-anim` T6 (each new clip's joint palette
differs from Idle). These images are the visual credibility layer.

## In-engine (rendered by X3Engine)
- `inengine_captureai_attack.png` — `--capture-ai`, frame 6. Four real monsters
  (violet scout, green guard, red wounded, blue drone) advancing/attacking the
  player marker on the arena. Proves the enriched marcus_webb rig skins + plays
  in-engine. Enemies are small/dark at the fixed 3/4 camera.
- `inengine_captureai_hitreact.png` — `--capture-ai`, frame 10 (t=2.0s, ~0.2s
  after the red "wounded" guard takes 78 melee dmg and SURVIVES at hp22). The red
  guard's head/torso is thrown back — the new Hitreaction flinch firing on the
  survive path. Subtle at this distance; see the close-up for the clear pose.
- `inengine_crowd_club.png` — `--screenshot-crowd`. Living crowd of skinned
  citizens on the club floor. Gesture states drive the new calm-loop clips; at
  this camera a specific gesture isn't isolatable, but the crowd renders alive.
  0 VUID.

## Blender close-up pose QA (tools/pose_render.py, single posed frame)
- `closeup_chief_hitreaction.png` — chief Hitreaction frame 3: contained
  head-back/spine-arch recoil, legs planted; returns to neutral by the end frame
  (verified) so it hard-cuts back to locomotion with no pop.
- `closeup_chief_attack2.png` — chief Attack2 frame 9: a distinct forward
  hook/cross (vs the right-hand haymaker of Attack) for attack variety.
- `closeup_alien_grey_attack.png` — grey alien Attack frame 10: whole-body lunge
  (single-Root rig; monster.cpp adds a procedural lunge draw-offset on top).
- `closeup_alien_grey_death.png` — grey alien Death frame 28: roll-over + sink
  collapse (the aliens had NO death clip before this wave).
- `closeup_anna_sit.png` — AnnaCasual Sit: thighs forward, knees bent, torso
  upright, hands to thighs — reads as seated.
- `closeup_anna_work.png` — AnnaCasual Work: forward lean over a task, head down.
- `closeup_anna_converse.png` — AnnaCasual Converse: relaxed talking sway
  (head nods + light hand gesture).

## Honest notes / deferred
- The pose_render baseline shows characters "floating" (no ground plane / no
  engine stand-up fixup); judge each pose RELATIVE to that baseline.
- Crowd Cheer + Drink were authored but DEFERRED: extreme shoulder abduction
  mangles this rig's skinning (Cheer); Drink didn't read reliably. The bakers
  retain both for future rig work. Shipped civilian set: LookAround, Converse,
  Work, Sit.
- A dedicated in-engine close-up of an alien mid-Attack was not captured (no
  screenshot mode drives canon-alien combat); the T6 palette test + the Blender
  close-up cover it.
