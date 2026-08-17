# HANDOFF — W-WEAPONS lane (Jake's guns in --world tunnel)

*Lane COMPLETE. Base commit d6950f55 on top of origin/integration/complete
8645406a. DO NOT PUSH — session lead merges. Successors: read CLAUDE.md,
docs/NO_SLOP.md, docs/ENGINE_GOTCHAS.md first.*

## THE ASK
Owner (twice): "Does he have his weapons?" A rifle in the driving world, wired
THROUGH app/character_anim (AnimatedCharacter) — never a host-local rig hack
(owner directive: "THIS ENGINE NEEDS CONSISTENT APPLICATION OF MODEL
ANIMATION"). States draw/holster (1/Q), aim (RMB + fine look), fire (LMB,
muzzle flash + tracer + light pulse), reload (R), grenade (G, arc + boom).
Ammo HUD + crosshair. THE CONTACT LAW in every capture.

## WHAT ALREADY EXISTED (grep-first — REUSED, not rebuilt)
- **app/weapon.{h,cpp}** `Arsenal`: data-driven roster, ammo/mag/reload/
  cooldown/spread, hitscan/projectile resolution (`--test-weapons` 30/30),
  viewmodel GLB loading, `drawCurrentAt(model16)` for third-person hand
  socketing, `currentMuzzleLocal()` = MEASURED barrel tip per GLB.
- **app/fx.{h,cpp}** `CombatFx`: tracers, muzzle bursts, impact sparks +
  scorch decals, projectile bolt visuals, `spawnExplosion` — all through
  `submitParticles`.
- **app/thirdperson.{h,cpp}**: campaign 3P avatar, `handSocketWorld` =
  avatarDraw x boneGlobal(`mixamorigRightHand`) x grip, `kTpGripTable`.
- **Jake_44_actions.glb** (store-served): Firingrifle 0.25 s,
  Rifleaimingidle 3.04 s, Riflerun 0.71 s, Riflejump 0.58 s, Reloading
  3.29 s, Tossgrenade 2.96 s. Hand bone `mixamorigRightHand`, chain scale 1.0.
- **WeaponRailgun.glb** (assets/rigged_glb, store-served) — the rifle read.

## WEAPON ASSET PROVENANCE (no new art was authored)
- Rifle model: **assets/rigged_glb/WeaponRailgun.glb**, store-served
  (tools/asset_store.py managed dir), materialized by `fetch --all`. NOT
  committed and must never be (gotcha 2.5). No draco extension
  (`extensionsUsed: null`) so no `@gltf-transform/cli copy` decode is needed —
  it renders. 5 LOD nodes model_LOD0..4, all with the same X+90 deg node
  rotation; `drawCurrentAt` draws all five stacked, exactly as the campaign
  does. MEASURED material: baseColor 2048^2 mean RGB **(23, 33, 45)** — the
  gun is a genuinely dark blue-grey tactical rifle, NOT an untextured
  full-metal black (X3_WORLD_RULES rule 5 checked and cleared); mrTex
  G=rough 0.40 / B=metal 0.08. That measurement is why the fix for the "the
  gun looks black" read was a CAMERA, not a material hack.
- Armory (localhost:8787) was searched: only medieval weapon racks, nothing
  usable. Nothing procedural was generated.
- UNMERGED sibling lane `origin/feat/weapons-on-complete` (087b716d) carries a
  12-weapon campaign roster — reference only, deliberately not merged here.

## THE MODULE WIRING SHAPE (the owner directive, concretely)
Hosts own KEYS and FX. The module owns the RIG. Nothing reaches into the
skinner from a host.
1. **app/character_anim.{h,cpp}** — `CharacterClipTable`/`jakeClipTable()`
   gained rifleIdle/rifleFire/rifleReload/rifleGrenade/rifleRun/rifleJump
   (exact clip names, measured durations in the comment block).
   `setArmed(bool)` re-registers the locomotion clips so the armed run is
   Riflerun, the armed stationary idle is Rifleaimingidle and the armed jump
   is Riflejump. `setAiming(bool)` makes the body face the camera even while
   moving. `playOneShot()` + fire/reload/grenade one-shots.
   `boneWorld(bone, player, yawTrim, yTrim, out16)` is the ONLY way a host
   reads a bone — cached node resolve, draw matrix x skinner boneGlobal.
2. **app/thirdperson.{h,cpp}** — the grip TRS was extracted to a shared
   inline `tpComposeGrip()`; the campaign's `handSocketWorld` was refactored
   ONTO it, so the campaign 3P frame and the tunnel frame cannot drift.
   `kTpGripTable` gained a "rifle" row (scaleMul 1.35 -> a 0.62 m held read).
3. **app/tunnel_corridor.{h,cpp}** — `uploadTunnelLights(dev, cam, extra,
   extraCount)`: transient muzzle/boom lights merged IN FRONT of the pooled
   bore lights, still one `setPointLights` per frame.
4. **app/world_hosts/host_tunnel.cpp** — `tunnelRifleRoster()` (one WeaponDef,
   reloadTime 3.29 s PAIRED with the Reloading clip per NO_SLOP rule 4),
   `heldRifleWorld` (module hand socket x shared grip x scale),
   `heldRifleMuzzle` (the MEASURED vmMuzzle under the same matrix the gun
   draws with), `fireRifleOnce`, `releaseGrenade`/`tickGrenades` (real Jolt
   spheres, 2.2 s fuse, Rocket boltFx arc, spawnExplosion 3.2 + car shove),
   `weaponLights`, `setRifleArmed`. Live loop: 1/Q draw-holster, RMB aim
   (ThirdNear + FOV 62 + 0.45x look gain + crosshair), LMB auto-fire, R
   reload, G grenade; unarmed punch/kick unchanged while holstered.

## WHAT THIS SESSION (v2) ADDED — and why
The v1 proof set was captured from the 12 m front camera and the F1 chase
cameras. Eyes-on at full res showed the held rifle was ~20 px wide from 12 m
and fully OCCLUDED BY JAKE'S OWN BACK from the chase cams: the shots could
not prove he was holding a rifle rather than a brick, and 24_reload_mid could
not show hands on a receiver at all. That is NO_SLOP rule 2 (eyes on, full
res, against a real reference) failing at the capture stage, so the capture
stage is what got fixed:
- **`kCamGunRig` (camMode 3, capture-only)** in `jakeSeq` — 3.4 m off Jake's
  FRONT-RIGHT (the rifle hand is +X of a -Z facing) at chest height,
  FOLLOWING the capsule so it works for moving shots. Framed so the boots
  stay in frame (at 3.4 m the 74 deg lens shows 2.88 m of height, aimed at
  feet+1.30 -> covers -0.14..2.74 m): a weapon shot must still be able to
  prove THE CONTACT LAW. Shots 20, 21, 23b, 24, 27 use it.
- **`groundedTail` contact-frame picker** in `jakeSeq` — MEASURED defect: the
  70-frame cut of 21_rifle_run landed in the run's flight phase with both
  boots 0.15 m over the tarmac, which is exactly the floating read rule 11
  exists to prevent. The first half of the window (longer than the 0.71 s
  Riflerun cycle, so a foot-strike is guaranteed inside it) only measures the
  lower toe bone's clearance over the capsule feet plane via the module's
  `boneWorld`; the second half arms the shutter on the first frame back
  within 1.5 cm of that minimum, then stops. No magic frame numbers. Receipt
  in the run log: `21_rifle_run armed on a FOOT-CONTACT frame (118/160, min
  toe clearance 0.134 m)`.
- **New shot 23b_fire_close** — shot 23 (over the shoulder) proves the tracer
  runs to the crosshair but cannot prove the flash leaves the BARREL TIP.
  23b, from the gun rig, does.
- The v1 "blade" defect (a 1.9 m sky-mirror slab hip-to-ground in 22/23) is
  GONE and did not return in any of the 19 re-captures.

## STATE AT HANDOFF — all gates green
- **Build**: `cmake --build build --config Release` (ALL_BUILD) exit 0, zero
  errors; x3app.dll relinked (X3Engine.exe is a 10 KB launcher — the DLL is
  the artifact whose mtime must advance).
- **Suites, all re-run on the FINAL binary — 241 assertions, 0 failed**:
  player 10, vehicle 10, roadnetwork 58, terraincorridor 11, tunnelmouth 8/8,
  riverbridge 9, weapons 30, pickup 4, thirdperson 19, anim 22, locomotion 7,
  gpuskin 8/8, footik 7, bodycontact 8/8, combat 11, targeting 19/19.
- **Boot**: `--world tunnel` live loop, bounded 45 s — **0 [ERROR]** in 546
  log lines. The proof-capture boot: 19 captures, **0 [ERROR]**.
- **Captures**: docs/screenshots/weapons/ 10..27 + 23b, all 19 READ AT FULL
  RES this session. Rifle in hand textured and correctly oriented (20, 21,
  24), aim pose + crosshair (22), muzzle flash at the barrel tip (23b) and
  tracer to the crosshair with an impact puff (23), reload with the
  "RELOADING..." HUD (24), grenade arc with glowing core + smoke trail (25),
  detonation fireball (26), holstered back to the melee layer with the SAME
  lens as 20 for a clean A/B (27). THE CONTACT LAW holds in every shot except
  15_jump_midair and 16_fall, which are deliberately airborne.

## KNOWN, PRE-EXISTING, OUT OF LANE
`--test-tunneldrive` is 8 passed / 4 failed (A2, A3, B1, B5b). NOT caused by
this lane and not fixed by it: A2/A3/B1 are documented obsolete assertions
(docs/design/TUNNEL_INTERIOR_PLAN.md records the suite at 6/12 and
ROAD_NETWORK_PLAN.md §952 schedules their retirement), and B5b is a
terrain/road-collision survey. Receipt that it cannot be ours: the suite lives
in app/tunnel_corridor.cpp from line 3422, this lane's only change to that
file is `uploadTunnelLights` at line ~828, and the self-test never calls it.

## NEXT STEPS (optional polish — nothing is blocking)
1. Fire SFX via IAudioSystem (roster `fireSfx` paths exist under assets/audio;
   the host already has `audio` + the resolveAudio pattern).
2. An E-prompt hint for the 1/Q bind; auto-holster when re-entering the car.
3. `drawCurrentAt` draws all five LOD meshes stacked (campaign behaviour too)
   — a real LOD pick would cut 4/5 of the held-weapon draw cost. Shared code:
   change it for both paths or neither (NO_SLOP rule 4).

## GOTCHAS THAT BIT THIS LANE
- `git add -A` stages store-served GLBs + `.pre-fetch.bak` files (gotcha 2.5).
  The tree still carries modified/untracked GLBs under assets/ from
  `fetch --all`. LEAVE THEM. Stage explicit paths only. Verified clean:
  `git diff --name-only 8645406a..HEAD | grep -c '\.glb$'` -> 0.
- The sandbox refuses compound bash commands mentioning
  origin/integration/complete — use `git for-each-ref` + SHAs.
- Build with plain `cmake --build build --config Release` (ALL_BUILD), never
  `--target X3Engine` (stale-exe trap 1.1). This worktree's build dir is
  already configured WITH the vcpkg toolchain — do not reconfigure plainly.
- Several lanes run the engine at once tonight; `tasklist //FI "IMAGENAME eq
  X3Engine.exe"` shows OTHER worktrees' processes. Check the process Path
  before concluding a run is yours, and retry rather than aborting. Bounded
  runs only. NEVER `--smoketest`.
