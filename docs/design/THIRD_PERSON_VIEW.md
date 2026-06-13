# Third-person view capability (Tim, 2026-05-26 — ORIGINAL intent)

> EFLZ was **meant to be third-person-view capable from the beginning**, not first-person
> only. The engine is FP today (FP camera + weapon viewmodel); add a 3P mode + a visible
> animated player avatar, toggleable with FP. Capturing so it isn't lost (like the cold-open
> nearly was).

## Why now: the XYZ conventions
This unblocked once the engine↔source coordinate differences were understood. 3P depends on
the SAME conventions: native is Y-up **right-handed**, **−Z forward** (`docs/CONVENTIONS.md`);
the character GLBs are authored facing **+Z**, so they need the **180° visual yaw flip** we
already apply to monsters (`monster.cpp`: `ry = m_yaw + π`, mirrored in `rescue.cpp`). The 3P
avatar's facing + the orbit camera must use exactly this — get the convention right and the
character faces + the camera orbits correctly.

## Jake rig — CONFIRMED (skel_dump 2026-05-26)
Source: `...\DellGameDev\EscL48BLN-ALT\assets\models\jake\Jake_22_actions.glb` (twin:
`...\Q3Engine\3D Models -Escape Lab 48\Jake 22 actions.glb`). **Mixamo skeleton, 34 bones**
(`mixamorig*`), ~1.7 m tall, real `model` mesh + a stray `Icosphere` to drop on import.
- **Right-hand weapon socket = `mixamorigRightHand`** (parent `mixamorigRightForeArm`). This
  is the bone to attach the carried weapon to.
- **22 clips** — locomotion: `Idle`, `Walking`, `Walkingbackwards`, `Runbackwards`,
  `Leftstrafewalking`/`Rightstrafewalking`/`Strafeleft`/`Straferight`/`Strafe`, `Jump`, turns
  (`Leftturn90`/`Rightturn90`/`Turnleft`/`Turningright45degrees`); **weapon-holding (hands
  already gripping):** `Rifleaimingidle`, `Riflerun`, `Riflejump`, `Firingrifle`, `Reloading`,
  `Tossgrenade`; `Hitreaction`. The rifle clips mean the socketed weapon lines up in-grip with
  no manual hand-posing. Mixamo rig ⇒ more clips retarget on via `tools/retarget_anims.py`.

## Design
- **Camera:** a follow/orbit camera behind + slightly above the player; mouse (or the
  arrow-key turn) orbits it around the player. Toggle FP⇄3P (a key, e.g. a settings option /
  a hotkey). FP keeps the current eye-cam + weapon viewmodel; 3P hides the viewmodel and
  shows the avatar.
- **Player avatar:** render the player as an animated character — **Jake.glb (16–22 anims)**
  from the Babylon/Q3Engine (`Q3Engine/.../Jake.glb`; the Task9D editor loads it with 16
  native animations). Bring it into `assets/rigged_glb/` (or convert) and drive it through
  the SAME skinning + locomotion-blend path the monsters/girls already use (idle/walk/run +
  jump/crouch). The GPU-skinning + loco blend are PROVEN (enemies animate), so the avatar is
  the same machinery pointed at the player.
- **Facing:** in 3P, the avatar faces the move direction (or the aim direction in combat,
  over-the-shoulder); the camera orbits behind. Crouch/prone (#42/#23) drive the avatar's
  crouch/crawl clips.
- **Combat in 3P:** over-the-shoulder aim — fire along the camera/aim ray (the hitscan +
  projectile paths already take an eye+dir, so feed them the 3P aim), with a reticle.
- **Held weapon — the avatar CARRIES the equipped model (Tim, 2026-05-26):** the avatar's
  hands hold the SAME weapon mesh the player is using, and it **swaps when the player switches
  weapons** (the 5 weapon models — pistol…chaingun — already loaded for the FP viewmodel; reuse
  those meshes, don't duplicate). Attach the weapon to a **right-hand bone/socket** on the
  avatar skeleton: each frame read that bone's world matrix from the animation pose (the
  `Skinner` already computes the bone palette — expose the hand bone's transform CPU-side) and
  place the weapon mesh at it (with a small per-weapon grip offset). The avatar's
  aim/idle/walk clips then move the gun naturally with the hand. FP = viewmodel in front of the
  camera (current); 3P = viewmodel hidden, weapon shown in the avatar's hand. (Stretch: a
  holstered weapon on the hip/back when not drawn.) Ties to per-weapon textures (#20).

## Why it's achievable on the foundation
The hard parts already work: rigged-character GPU skinning + locomotion blend (monsters/girls
animate), the player controller + camera, the convention/facing-flip (proven on monsters).
3P = a follow camera + showing the player's OWN animated avatar (Jake) + the FP/3P toggle +
routing aim from the 3P camera. It's an additive mode, not a rewrite.

Related: `docs/CONVENTIONS.md`, `app/monster.cpp`/`rescue.cpp` (the facing-flip + loco blend
to mirror for the avatar), `app/player.cpp` (controller/stance), `app/anim.*` (the Skinner).
