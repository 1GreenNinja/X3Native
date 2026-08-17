# HANDOFF — W-WEAPONS lane (Jake's guns in --world tunnel)

*Written by the Fable session at cap. Successor: read this, then CLAUDE.md,
docs/NO_SLOP.md, docs/ENGINE_GOTCHAS.md. Base commit d6950f55 on top of
origin/integration/complete 8645406a. DO NOT PUSH — session lead merges.*

## THE ASK
Owner (twice): "Does he have his weapons?" Rifle in the driving world, wired
THROUGH app/character_anim (AnimatedCharacter) — states draw/holster (1/Q),
aim (RMB + fine look), fire (LMB, muzzle flash + tracer + light pulse),
reload (R), grenade (G, arc + boom). Ammo HUD + crosshair. Contact law in
every capture. Gates: build green, suites green, boot zero [ERROR], EYES-ON
full-res captures read by the agent.

## WHAT ALREADY EXISTED (grep-first — REUSE, do not rebuild)
- **app/weapon.{h,cpp}**: `Arsenal` — data-driven roster, ammo/mag/reload/
  cooldown/spread, hitscan/projectile resolution (headless-tested,
  `--test-weapons` 30/30), viewmodel GLB loading, `drawCurrentAt(model16)`
  for third-person hand-socketing, `currentMuzzleLocal()` = MEASURED barrel
  tip per GLB (tools/weapon_muzzle_probe.py).
- **app/fx.{h,cpp}**: `CombatFx` — tracers (`addTracer`), muzzle bursts
  (`spawnMuzzleFlash`), impact sparks + scorch decals (`spawnImpact`),
  projectile bolt visuals (`boltFx`, Rocket kind = hot core + smoke trail),
  `spawnExplosion`, all via `submitParticles`.
- **app/thirdperson.{h,cpp}**: campaign 3P avatar with `handSocketWorld` =
  avatarDraw × boneGlobal(`mixamorigRightHand`) × grip. `kTpGripTable`
  (values were chosen BLIND per its own comment — never eyeballed in 3P!).
- **Jake_44_actions.glb** (store-served, fetched): clips Firingrifle(0.25s),
  Rifleaimingidle(3.04s), Riflerun(0.71s), Riflejump(0.58s),
  Reloading(3.29s), Tossgrenade(2.96s), Block3. Hand bone
  `mixamorigRightHand`, chain scale = 1.0 (verified numerically).
- **Weapon GLBs** in assets/rigged_glb (manifest, store-served):
  WeaponRailgun.glb = the rifle read (5 nodes model_LOD0..4, ALL five drawn
  stacked by drawCurrentAt — campaign does the same). Scene-space: 1.9 m
  long (Z −0.95..0.95 after the X+90° node rotation), 0.68 m tall, mat:
  metallic=1.0 rough=1.0 WITH baseColor+MR textures. vmMuzzle measured
  {0, 0.494, 0.909}.
- **Armory (localhost:8787)**: only medieval weapon racks — nothing usable.
- **UNMERGED other lane**: `origin/feat/weapons-on-complete` (087b716d) —
  12-weapon campaign roster + armory models. NOT merged into
  integration/complete; do not merge it yourself, but its weapon.cpp deltas
  are a reference.

## WHAT THIS LANE BUILT (commit d6950f55)
1. **app/character_anim.{h,cpp}** (the module — owner directive says weapons
   wire THROUGH here):
   - `CharacterClipTable` + jakeClipTable(): rifleIdle/rifleFire/rifleReload/
     rifleGrenade/rifleRun/rifleJump entries (exact names, measured durations
     in the comment block).
   - `setArmed(bool)` — swaps locomotion run clip to Riflerun (re-registers
     setLocomotionClips), armed stationary idle = Rifleaimingidle, armed jump
     = Riflejump (`m_jumpClip` selection).
   - `setAiming(bool)` — faces the CAMERA even while moving forward.
   - `playOneShot(name, restart)` + `fireOneShot/reloadOneShot/
     grenadeOneShot/grenadeOneShotActive/oneShotTime`.
   - `boneWorld(boneName, player, yawTrim, yTrim, out16)` — draw matrix ×
     skinner boneGlobal (cached node resolve).
2. **app/thirdperson.{h,cpp}**: grip TRS extracted to shared inline
   `tpComposeGrip()` (header), handSocketWorld refactored onto it; added
   `"rifle"` grip row (copy of smg row).
3. **app/tunnel_corridor.{h,cpp}**: `uploadTunnelLights(dev, cam, extra,
   extraCount)` — transient lights merged IN FRONT of the pooled bore lights
   (muzzle/boom pulses), still one setPointLights per frame.
4. **app/world_hosts/host_tunnel.cpp**:
   - `tunnelRifleRoster()` (top of file): one "rifle" WeaponDef seeded from
     the campaign smg row (Railgun GLB, measured muzzle), reloadTime 3.29 s
     PAIRED with the Reloading clip.
   - State + lambdas after the wheel-spin FX block (~line 1150):
     `combatFx` (heap), `rifle` Arsenal, `heldRifleWorld` / `heldRifleMuzzle`
     / `fireRifleOnce` / `releaseGrenade` / `tickGrenades` / `weaponLights` /
     `setRifleArmed`. Grenade = phys addSphere r 0.08 m 0.4 kg, v = dir·13 +
     4.5 up, 2.2 s fuse, Rocket boltFx arc, spawnExplosion(3.2) + car shove.
   - Live loop on-foot block: weapon keys FIRST (1/Q toggle, RMB aim + 0.45×
     look gain, R reload begins Arsenal reload + clip, G toss with 1.15 s
     release schedule), LMB auto-fire; unarmed punch/kick unchanged when
     holstered. Timers (`rifle.tick`, `tickGrenades`, `combatFx.update`) run
     EVERY frame after the block.
   - Aim camera: ThirdNear + FOV 62 while aiming (unless first person).
   - Render: combatFx.draw+submit after riverLife in the main frame block;
     held rifle drawn after jake.draw; ammo HUD bottom-left + crosshair
     (Hud::drawCrosshair via a bare `wpnHud`) while aiming.
   - Captures: `--screenshot-jake <dir>` extended — jakeSeq got an `act(i)`
     hook + weapon draw/FX/HUD, new shots 20_rifle_idle, 21_rifle_run,
     22_aim_shoulder, 23_fire_muzzle, 24_reload_mid, 25_grenade_arc,
     26_grenade_boom, 27_holster_punch.

## STATE AT HANDOFF
- Build green (x3app.dll + X3Engine.exe relinked, zero errors).
- Suites green: roadnetwork 58/58, terraincorridor 11/11, tunnelmouth 8/8,
  riverbridge 9/9, player 10/10, weapons 30/30, pickup 4/4.
- Proof run wrote all 18 captures, zero [ERROR]
  (docs/screenshots/weapons/, committed).

## OPEN DEFECT — THE BLADE (eyes-on caught it; fix before shipping)
In 22_aim_shoulder + 23_fire_muzzle a ~1.9 m wide sky-mirror "blade" runs
from Jake's left-hip area to the ground. Facts measured so far:
- Appears ONLY when the rifle draws (not in unarmed shots 10–19).
- Its length ≈ the UNSCALED Railgun (1.9 m); my heldRifleWorld folds
  s = 0.24 × grip 1.0 into the matrix, so the gun should be 0.46 m.
- A small dark rifle-looking object IS also visible at the shoulder in
  22/23 (tracer leaves from ~there toward the crosshair, impact puff at the
  aim point — that part works).
- Jake's GLB has ONE mesh (verified) — the blade is not part of his rig.
- Hand-bone chain scale is 1.0 (verified).
- Railgun mat is metallic 1.0 / rough 1.0 (textures exist) — the sky-mirror
  read matches an unlit/env-reflecting metal path (drawMesh, not PBR).
Hypotheses to kill IN ORDER (rule 9 — measure, don't guess):
1. Two draws? Search for any OTHER drawCurrentAt/draw of the rifle per
   frame in jakeSeq (I believe there is exactly one — verify by commenting
   out the jakeSeq held-rifle draw and re-running shots 22/23: if the blade
   survives, it is NOT my draw; if both gun+blade vanish, my single draw is
   emitting both = look at the 5 stacked LOD drawables' nodeTransforms as
   makeDrawables actually baked them (dump dr.nodeTransform at runtime, one
   log line per drawable — cheap and decisive).
2. If one LOD drawable carries a different baked node transform than the
   glTF shows (loader quirk), strip to LOD0 only in drawCurrentAt callers
   or filter drawables by name "model_LOD0" for the held path.
3. Grip orientation: once the blade is gone, verify barrel points forward
   (muzzle toward crosshair) and dial the "rifle" TpGrip row (grip_* cvars
   exist in showroom; here just edit the row + recapture).
4. The 22/23 rifle-at-shoulder read is DARK — if it stays black after the
   blade fix, that is X3_WORLD_RULES rule 5 (full-metal + simple drawMesh
   path); consider drawMeshPBR for the held path or clamping metalness.
Also improve: 24_reload_mid uses the FAR front camera — switch to
(nullptr, 1) near mode so hands-on-receiver is readable; 25_grenade_arc:
grenade glow not clearly visible mid-air — verify boltFx core size at 7 m
camera distance, maybe brighten/enlarge for the capture or move camSide.

## NEXT STEPS (in order)
1. Fix the blade (above), recapture 20–27, EYES-ON each at full res.
2. Verify contact law in every shot (feet on road — held so far).
3. Re-run the five suites + boot zero-[ERROR]; rebuild green.
4. Commit receipts; leave for session lead — DO NOT push.
5. Optional polish if time: fire SFX via IAudioSystem (roster fireSfx paths
   exist under assets/audio; host has `audio` + resolveAudio pattern in
   weapon docs), E-prompt hint for the 1/Q bind, holster also when
   re-entering the car.

## GOTCHAS THAT ALREADY BIT THIS LANE
- `git add -A` staged store-served GLBs + .pre-fetch.bak files (gotcha 2.5)
  — assets/ was unstaged before committing. NEVER commit them; the tree
  still has modified/untracked GLBs under assets/ from `fetch --all`
  (materialized over LFS pointers). Leave them; do not "clean up" by
  committing. If a merge blocks: `git checkout -- <tracked glbs>` + rm the
  .bak files.
- The sandbox refuses compound bash commands mentioning
  origin/integration/complete ("runs a string through complete") — use
  `git for-each-ref` + SHAs, or plain single commands.
- Build: plain `cmake --build build --config Release` (ALL_BUILD), never
  --target X3Engine (stale-exe trap 1.1). Worktree build dir already
  configured WITH the vcpkg toolchain — do not reconfigure plainly.
- Engine runs: check `tasklist //FI "IMAGENAME eq X3Engine.exe"` first;
  bounded runs only; NEVER --smoketest.
