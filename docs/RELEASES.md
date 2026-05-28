# X3Native — Releases

> Per-MINOR notes. BUILD/HASH auto-fill from git via `cmake/GitVersion.cmake`. See `docs/VERSIONING.md` for the scheme.

## 0.4 (2026-05-27) — Controllers + asset wiring batch

Shipped on the SNAKE/13700K right-screen lanes (5-agent parallel dispatch) + integrated into `feat/cull-combined` via FarmBoss (P13700 left-screen). Each lane gated independently: `--test-*` green + `--world <lane> --screenshot --headless` PNG verified non-blank via pixel-variance (std > 15, uniqColors > 100) — NOT just drawCalls/triangles (the prior Slice-B lesson about that false-pass mode informed the gate criteria).

### New engine systems
- **`SwimController`** (`app/swim_controller.{h,cpp}`, `feat/swim-controller @ c2eeb18`) — 3D underwater player controller. Buoyancy + oxygen + depth/pressure UI; swim / boost / ascend / descend; `--test-swim` 8/8 (Release + Debug); `--world swim` showcase places player at -5m below `surfaceY=0` with sea fauna (sea_giant_squid, sea_hammerhead, sea_humpback_whale, sea_manta_ray, GreatWhiteSharkGameReady) as static decor. Foundation stone for the Act-4 undersea tier.
- **`NPCSystem`** (`app/npc.{h,cpp}`, `feat/npc-controller @ 7bb9b61`) — world-NPC layer for non-combatants (bartender, captive, scientist, civilian, dock worker, mechanic). Modes: Idle / Patrol / LookAtPlayer / Interact / Captive. Patrol waypoint follow, fixed-pose facing, look-at-range trigger, dialogue-on-interact hook, captive→companion rescue handoff (generalizes the F2 mechanic). `--test-npc` 7/7 (Release + Debug); `--world npc` showcase. Sets up `--world modeltest` (Bar / Model-Test station) and unblocks the cognitive/conversation slices of the companion roadmap.

### Asset wiring (`feat/wire-new-glbs @ 6c5eaca`)
- 8 new GLBs into `assets/rigged_glb/`: **Pistol** (replaces `WeaponEnergyPistol.glb`, old saved `.bak`), **Sportscar** (full res, no downscale per design rule), **Mechanic / DrLabResearcher / DannyBartender** (NPCs), **MechSoldier / SynthModelActual / ArmoredFuturisticSoldier** (enemy variants).
- 3 new monster tunings in `app/monster.{h,cpp}`: `synthModelActualTuning()` (humanoid ground melee, **flyer=false** — distinct from BlueSynth which is the flyer), `mechSoldierTuning()` (heavy armored melee), `armoredFuturisticSoldierTuning()` (armored melee). Bestiary table unchanged.
- Sportscar wired into `--world drive` via new `DriveDemo::loadBodyGlb()` in `app/vehicle.{h,cpp}`; procedural chassis suppressed on GLB-load success.
- `--test-loadnewglbs` 8/8 (Release + Debug); regression tests `--test-bestiary` / `--test-vehicle` / `--test-combat` green.

### Engine reliability (in flight at version-bump time; folded in by integrator)
- **Headless screenshot non-blank assertion** (`feat/headless-capture-fix`) — `IRenderDevice::captureFrame()` now computes pixel std + unique-color count on the readback buffer and returns false (with `[rhi] captureFrame: BLANK RENDER DETECTED` log) if `std < 5.0` OR `uniqueColors < 10`. Closes the false-pass loophole where prior agents shipped blank screenshots based on drawCalls/triangles alone.
- **Smoketest baseline restore** (`feat/smoketest-fix`) — fixed the silent-crash regression in the Level-1 / Spire monster-build path that landed via the `integration/culling-glass` merge wave. `--smoketest` now deterministically prints `30 frames + recreate OK` (5/5 consecutive runs, Release + Debug, 0 VUID, `allocationCount=0`).
- **Weapons-artpass rebase** (`feat/weapons-artpass-rebased`) — rebased onto `feat/cull-combined` tip so the full **10-weapon `makeDefaultRoster()`** (pistol / smg(WeaponX) / shotgun / plasma / chaingun / plasma_rifle / lightning / bfg / rpg / railgun) + the **jagged white-blue forked lightning bolt FX** (`app/fx.{h,cpp}` `addBolt` / `drawBolt`) lands in the integration train. Lightning gun fire path: `if (arsenal.current().beam) combatFx.addBolt(muzzle, r.endPoint); else combatFx.addTracer(...)`.

### Coordination
- Fleet protocol formalized as `FLEET.md` (Commander / FarmBoss / SNAKE / DJBOOTH lanes, 13700K = `IntegratorCaptainCommanderInspector`). All lanes branch from current integration tip, gate locally, push `feat/*`, NEVER push `main`. Integrator merges.
