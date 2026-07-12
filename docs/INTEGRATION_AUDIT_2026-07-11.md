# X3Native — Integration Audit, 2026-07-11

**Auditor:** OG Dell_I9 (read-only pass — nothing merged, rebased, or deleted)
**Scope:** 227 remote branches + 7 local, audited against the line the owner actually plays.
**Status:** AUDIT ONLY. Every command below is a *proposal*. Owner + 13700K integrator approve before execution.

---

## 1. Executive summary

### How bad is it?

**Not as bad as 190 branches sounds — but the single most owner-visible bug in the game is fixed on a branch nobody merged.**

Of 227 remote branches:

| Bucket | Count | Meaning |
|---|---:|---|
| **ALREADY IN** | 97 | Zero unique patches vs the canonical line. Safe to delete today. |
| **STALE/DEAD** | 103 | Superseded ancestors, duplicates, sibling-slice branches. Delete after their one head lands. |
| **LAND NOW** | 13 | Finished, valuable, owner-visible work that is in **neither** `main` nor `playable-build`. |
| **NEEDS WORK** | 12 | Real value, unfinished/stranded/non-game (fleet tooling, `slick` chat app). |
| **ACTIVE** | 2 | In-flight today. Do not touch. |

So **200 of 227 branches are noise**. The real problem is a **13-branch backlog** — and it is concentrated: *two* branches (`feat/intro-cockpit`, `feat/city-fix`) carry most of the value.

### The canonical line — stated plainly

> **`integration/playable-build` IS the game. `origin/main` is a stale ancestor of it, nothing more.**

The relationship is simpler than the branch count suggests:

```
merge-base(origin/main, origin/integration/playable-build) == e443f27 == origin/main's own tip
```

`origin/main` is a **direct, strict ancestor** of `integration/playable-build`. It has **0** commits playable-build lacks and sits **217 behind**. THE GREAT FOLD (`e443f27`, `7b40843`, the g4/g5 merges) landed on main on **2026-06-13** — and then all development *continued on playable-build*, which already contained the fold. Main was simply never moved forward again.

**Consequence:** `origin/main` can be **fast-forwarded** to `playable-build` with **zero merge risk and zero conflicts** — it is a pure pointer move. There is no divergence to reconcile. Main being "behind" is a bookkeeping failure, not an integration failure.

Everything else in this audit is measured against `integration/playable-build` (tip `f967213`, 2026-07-11).

### The motivating example — CONFIRMED

The owner still sees the **camo pistol**. Verified:

- `ba3ce7a` *"weapons: restore per-weapon PBR textures (fix shared-atlas skin bug)"* — `git merge-base --is-ancestor` says **NOT in `main`, NOT in `playable-build`**. It lives only on `feat/weapon-textures`.
- The root cause per the commit body: every weapon viewmodel GLB was built against **one shared texture atlas**, then tinted via `baseColorFactor`, so 4 of 5 weapons wore a foreign skin sampled through their own UVs = the camo splotch.
- The **fixed** pistol GLB (LFS oid `28a84e3d…`) exists on `feat/weapon-textures` **and** on `feat/intro-cockpit`. `playable-build` still carries the **broken** oid `f9a64a66…`.

The owner is playing the broken oid. The fix has been sitting on two unmerged branches for **9 days**.

### THE BLOCKER YOU MUST KNOW ABOUT — Git LFS is dead

**GitHub LFS budget is EXHAUSTED. Asset fetches hard-fail fleet-wide.** Verified live:

```
batch response: This repository exceeded its LFS budget.
error: failed to fetch some objects from 'https://github.com/1GreenNinja/X3Native.git/info/lfs'
```

Of the **129 asset binaries** that `feat/intro-cockpit` adds or changes vs playable-build, **129 are missing locally and 0 are fetchable.** Cross-checking the content-addressed asset store (`D:\Assets\X3AssetStore` = 154 objects, `\\p13700\G\X3AssetStore` = 81 objects) against that branch's `assets/manifest.json`: **84 of 122 manifest entries are in NEITHER store.** That includes all 5 fixed weapon GLBs.

**This means: merging `feat/intro-cockpit` on any machine that did not forge those assets produces a build with dangling LFS pointers.** Landing is a *code + asset* operation, not a git operation.

**The escape hatch (this is the good news):** the camo-pistol fix is **a pipeline fix, not a binary.** `tools/rebind_weapon_textures.py` regenerates the correct GLBs from Rodin source, which is **present on this machine**:

```
D:\GameDev\EscapeLab3D-RBDOOM\base\models\x3\rodin\
  WeaponEnergyPistol\  texture_diffuse.png texture_normal.png texture_metallic.png texture_roughness.png weaponenergypistol.obj
  WeaponBFG\           (same set)
  WeaponRailGun\       (same set)
  WeaponRocketLauncher\(same set)
  WeaponShotGun\       weaponshotgun.obj + .mtl — NO textures (matches ba3ce7a's note: needs real art)
```

So the weapons can be fixed **without LFS, without the 14900K**: land the code, re-run the tool, publish to the asset store. See Step 2 of the landing plan.

### Top 10 to land, by owner-visible impact

| # | What he'd SEE | Branch | Effort |
|---:|---|---|---|
| 1 | **Camo pistol / wrong weapon skins** — 5 guns wearing each other's textures | `feat/intro-cockpit` (+ tool from `feat/weapon-textures`) | Merge: 2 conflicts. Assets: regenerate. |
| 2 | **Held-weapon texture + terminal UI + cell scale** — 3 defects he personally reported (`6574e07`) | `feat/intro-cockpit` | same merge |
| 3 | **Terminal is dull, text too small** — glossy dark glass + readable text, his lock-down (`8c6e7c1`) | `feat/intro-cockpit` | same merge |
| 4 | **Console scrollback broken + VIGIL digit-choice misroutes** (`b81f884`) | `feat/intro-cockpit` | same merge |
| 5 | **Black-silhouette props in F2–F7** (`45e1c46`) — ⚠️ *see §5, this may be a band-aid* | `feat/intro-cockpit` | same merge |
| 6 | **The city is procedural grey boxes** — real GLB cyberpunk facades + 14 prop types exist and are unlanded | `feat/city-fix` | **HARD — port, don't merge** |
| 7 | **Weapon FX are flat** — lightning CHARGE model, sharp zigzag bolt, arc-tendril impacts, battery pickups | `feat/weapons-overhaul` | cherry-pick |
| 8 | **Input lockup (P0)** — `InputCaptureManager` + ESC failsafe + watchdog, absent from PB | `fix/input-capture-lockup` | clean cherry-pick |
| 9 | **Monsters can't see you (P1)** — self-intersecting LOS-ray root-cause fix + regression gate | `fix/monster-perception` | clean cherry-pick |
| 10 | **The HoloPanel platform** — the black-glass/round-pipe fixture he explicitly asked to reuse game-wide; 4 variants never landed | `feat/holo-glass-platform` | clean cherry-pick (`holo_panel.*` is new-file only) |

**Bonus (not owner-visible but load-bearing):** the entire **portal hub / rifthub** is in the game on **zero** branches — `app/rifthub.cpp` does not exist in `playable-build`. `feat/rifthub-aaa` is 49 ahead / 4 behind and is the *only* home of the portal work. If it doesn't land, the portal hub ships in no build.

---

## 2. Full branch table

Legend: **New** = commits with no patch-equivalent in `playable-build` (`git cherry`). **Behind** = commits playable-build has that the branch lacks.

Local-only branches not in the remote list: `fix/engine-glb-lighting` (**ACTIVE**, worktree `D:/GameDev/X3Native-glblight`, 49 ahead / 4 behind — the engine-wide GLB lighting fix + rifthub R8), `wt/polish` (**ACTIVE**, worktree `D:/GameDev/X3Native-polish`), `integration/honor-fable-final` (**ALREADY IN** — the pre-split fold, superseded).

| Branch | Bucket | New | Behind | Last | Subject |
|---|---|---:|---:|---|---|
| `feat/tractor-beam` | LAND NOW | 71 | 380 | 2026-05-30 | feat(space): tractor-beam VFX for the intro capture beat |
| `feat/city-fix` | LAND NOW | 51 | 128 | 2026-07-03 | feat(city): near-facade cool fill light + clean hero shot |
| `feat/living-city` | LAND NOW | 49 | 128 | 2026-07-03 | feat(living-city): NpcLife — the LIVING CITY (W5 #41) — schedules + 12 archety |
| `feat/npc-characters` | LAND NOW | 49 | 128 | 2026-07-03 | feat(npc): animated citizen BODY pipeline (rig+anim states+ragdoll+interface) |
| `feat/dialog-live` | LAND NOW | 43 | 128 | 2026-07-02 | dialog: wire canonlevel captives (Aria/Keisha/Emily) to authored chat trees |
| `feat/intro-cockpit` | LAND NOW | 43 | 14 | 2026-07-11 | rt: default RT AO + DDGI GI ON for ray-tracing-capable devices (owner: 'Ray Tr |
| `feat/weapon-textures` | LAND NOW | 42 | 128 | 2026-07-02 | weapons: restore per-weapon PBR textures (fix shared-atlas skin bug) |
| `feat/holo-glass-platform` | LAND NOW | 35 | 128 | 2026-07-02 | feat(holo): HoloPanel platform + 4 variants (terminal / elevator / keypad / pl |
| `fix/level-architecture` | LAND NOW | 15 | 128 | 2026-07-01 | fix(level/lint): seal future-content upper clusters — building residuals 64->3 |
| `feat/weapons-overhaul` | LAND NOW | 9 | 128 | 2026-07-01 | proof: per-weapon firing FX audit (plasma blue / shotgun wide boom / chaingun) |
| `feat/texture-offensive` | LAND NOW | 7 | 128 | 2026-07-01 | texture offensive: THE AMBIENT DROP + HERO TERMINAL FINISH (canon detention) |
| `fix/input-capture-lockup` | LAND NOW | 7 | 128 | 2026-07-01 | fix(input): reconcile capture by priority instead of diffing transitions |
| `fix/monster-perception` | LAND NOW | 6 | 128 | 2026-07-01 | test(monster): add --test-monsterperception P1 regression gate |
| `feat/rifthub-aaa` | ACTIVE | 46 | 4 | 2026-07-11 | docs(rifthub): R8 addendum — console PARAMETERS with consequences (room warp / |
| `echotropolis` | ACTIVE | 18 | 2 | 2026-07-11 | [sibling] ESC opens a PAUSE MENU (never exits) + start paused at golden + doub |
| `feat/minerva-texture` | NEEDS WORK | 146 | 380 | 2026-07-11 | feat(assets): Minerva textured pass — JakeFighterShip_textured.glb + proof sho |
| `docs/public-fleetcommand` | NEEDS WORK | 145 | 380 | 2026-07-03 | docs: scoping — Public FleetCommand (private fleet chat -> public Matrix servi |
| `feat/cull-combined` | NEEDS WORK | 145 | 380 | 2026-07-07 | feat(slick): Slack-style People roster in the sidebar |
| `feat/gen-image-workflow` | NEEDS WORK | 124 | 380 | 2026-06-14 | feat(slick): bridge/comfy_workflows/image.json — the gen-worker's image pipeli |
| `feat/model-test` | NEEDS WORK | 112 | 380 | 2026-07-07 | feat(modeltest): route translucent drawables (glTF alpha<0.5) through the engi |
| `feat/undersea-art` | NEEDS WORK | 111 | 380 | 2026-06-09 | feat(act4): marine-snow particulate — cinematic deep-sea atmosphere |
| `feat/space-engine-spec` | NEEDS WORK | 28 | 380 | 2026-05-31 | docs(fleet): NOTE_TO_14900K.2 — doors-death-anim is the keystone (cherry-pick  |
| `feat/mech-pilot` | NEEDS WORK | 15 | 380 | 2026-05-28 | feat(mech): rideable heavy-mech pilot controller + --test-mech + --world mech |
| `feat/daemon-media-outbox` | NEEDS WORK | 14 | 388 | 2026-06-14 | feat(matrix-daemon): native m.image sends via the outbox pipe |
| `feat/fleet-messaging-design` | NEEDS WORK | 13 | 388 | 2026-06-01 | fleet: add fleet_image.py — post images to fleet chat |
| `feat/point-light-shadows` | NEEDS WORK | 9 | 217 | 2026-06-24 | feat(gap6): tier-3 Ultra — depth cube-map array point shadows |
| `feat/gpu-llm` | NEEDS WORK | 7 | 128 | 2026-07-01 | feat(llm): zero-stutter frame-impact benchmark + ai_threads mitigation |
| `feat/portal-hub-rebased` | STALE/DEAD | 114 | 380 | 2026-05-30 | rifthub: blue energy core + wormhole-generator housing + eerie audio |
| `feat/m10-ship` | STALE/DEAD | 105 | 380 | 2026-06-05 | docs(m10): scaffold Steamworks + ship milestone |
| `feat/m7-postfx-advanced` | STALE/DEAD | 105 | 380 | 2026-06-05 | docs(m7): scaffold advanced cinematic post-FX milestone |
| `feat/m8-imgui-devtools` | STALE/DEAD | 105 | 380 | 2026-06-05 | docs(m8): scaffold Dear ImGui dev-tools milestone |
| `feat/jake-ship` | STALE/DEAD | 93 | 380 | 2026-06-05 | feat(jakeship): add thin blue body-line glow strips to the Minerva |
| `feat/cockpit-vattalus` | STALE/DEAD | 91 | 380 | 2026-05-31 | feat(cockpit): Vattalus-style sci-fi bridge for --world ship-interior |
| `feat/ship-interior-firefly` | STALE/DEAD | 81 | 380 | 2026-05-31 | feat(ship-interior): rebuild --world ship-interior as a FIREFLY cockpit |
| `feat/rifthub-portal-visuals` | STALE/DEAD | 74 | 380 | 2026-07-05 | feat(rifthub): rework portals into Stargate-inspired thick stone gates |
| `feat/decloak-vfx` | STALE/DEAD | 71 | 380 | 2026-05-30 | feat(space): decloak shimmer VFX (intro ship phasing in) |
| `feat/portal-hub-polished` | STALE/DEAD | 71 | 380 | 2026-05-30 | feat(rifthub): port + polish DJBOOTH's portal hub onto cull-combined |
| `feat/coop-companion-merge` | STALE/DEAD | 53 | 380 | 2026-05-30 | docs(fleet): NOTE_TO_FARMBOSS — Snake reply (coop-merge done, spec locked, wor |
| `feat/ship-repair` | STALE/DEAD | 49 | 380 | 2026-05-30 | feat(space): S7 ship repair — open panels + connect wires (transit gameplay) |
| `feat/ship-windows` | STALE/DEAD | 49 | 380 | 2026-05-30 | feat(space): S6 true-portal ship windows — moving space outside the glass |
| `feat/atmo-descent` | STALE/DEAD | 48 | 380 | 2026-05-30 | feat(space): S4 cinematic atmospheric descent (AtmoDescent) |
| `feat/city-aaa` | STALE/DEAD | 48 | 128 | 2026-07-03 | docs(city-aaa): final Release night hero shot (0 VUID, allocationCount=0) |
| `feat/eva-spacewalk` | STALE/DEAD | 48 | 380 | 2026-05-30 | feat(space): S12 EVA spacewalk — zero-G hull-repair controller + --world eva |
| `feat/ship-ai` | STALE/DEAD | 48 | 380 | 2026-05-30 | feat(space): S8 enemy ship AI — dogfight pillar enemies (--test-ship-ai + --wo |
| `feat/ship-damage` | STALE/DEAD | 48 | 380 | 2026-05-30 | feat(space): S10 ship damage model — shield/hull/destructible subsystems |
| `feat/ship-interior` | STALE/DEAD | 48 | 380 | 2026-05-30 | feat(space): S5 ship interior — walkable, data-driven static frame |
| `feat/ship-targeting` | STALE/DEAD | 48 | 380 | 2026-05-30 | feat(space): S9 targeting / radar / lock-on — dogfight HUD layer |
| `feat/wormhole-transit` | STALE/DEAD | 48 | 380 | 2026-05-30 | feat(space): S3 wormhole transit — crystal-matrix interstellar jump |
| `feat/ship-art` | STALE/DEAD | 47 | 380 | 2026-05-30 | feat(space): S11 ship art + node-transform animation (Lane D) |
| `feat/space-env` | STALE/DEAD | 47 | 380 | 2026-05-30 | feat(space): S1 space environment — nebula/star dome + proxy planets + sun |
| `feat/space-layer` | STALE/DEAD | 47 | 380 | 2026-05-30 | feat(space): S0 SpaceLayer spine — context FSM + env transform + proxy registr |
| `feat/space-lod` | STALE/DEAD | 47 | 380 | 2026-05-30 | feat(space): S2 distance-LOD system + Blender LOD-chain generator |
| `feat/space-wave1-integrated` | STALE/DEAD | 47 | 380 | 2026-05-30 | feat(space): S0 SpaceLayer spine — context FSM + env transform + proxy registr |
| `feat/wormhole-vfx` | STALE/DEAD | 47 | 380 | 2026-05-30 | feat(space): Salvari crystal-matrix wormhole VFX (Act-3 jump transition) |
| `feat/city-uplift` | STALE/DEAD | 42 | 128 | 2026-07-03 | feat(city): REAL GLB cyberpunk facades replace the procedural boxes |
| `integration/v003` | STALE/DEAD | 41 | 128 | 2026-07-02 | Merge remote-tracking branch 'origin/feat/tower-white-concrete' into integrati |
| `feat/neon-city` | STALE/DEAD | 40 | 128 | 2026-07-03 | feat(city): --world city playable neon district + in-world WD2 hacking |
| `feat/terrain-aaa` | STALE/DEAD | 38 | 128 | 2026-07-02 | feat(terrain-aaa): world landmarks + biome ground identity + hero composition |
| `feat/world-terrain` | STALE/DEAD | 36 | 128 | 2026-07-02 | feat(terrain): FREEWAY asphalt ribbon — paved deck + lane lines along the grad |
| `feat/wave3-content` | STALE/DEAD | 32 | 595 | 2026-05-23 | tools: procedural creature locomotion baker + animate the Verthani |
| `integration/v002` | STALE/DEAD | 32 | 128 | 2026-07-01 | fix(weapons/v002): pistol viewmodel stays ANCHORED — clip guard no longer floa |
| `feat/intro-real-ships` | STALE/DEAD | 21 | 128 | 2026-07-03 | intro(real-ships): swap procedural box ships for the real GLB hero + dreadnoug |
| `feat/intro-cinematic-p1` | STALE/DEAD | 20 | 128 | 2026-07-02 | T6: dress the procedural capital as a SHIP (layered hull + running lights) |
| `feat/weapons-artpass-rebased` | STALE/DEAD | 20 | 380 | 2026-05-27 | weapons: add Railgun (#10) — high-power precision slug |
| `feat/intro-cinematic-playable` | STALE/DEAD | 17 | 128 | 2026-07-01 | rhi: keep god-rays/lens buffers VUID-clean when their pass is skipped |
| `feat/companion-controller` | STALE/DEAD | 16 | 391 | 2026-05-28 | companion-controller: single-companion facade (Slice C seam) |
| `feat/companion-ai` | STALE/DEAD | 15 | 391 | 2026-05-27 | companion-squad: render visible companion + threat models in the showcase (fix |
| `feat/smoketest-fix` | STALE/DEAD | 14 | 380 | 2026-05-28 | fix(smoketest): mirror live shutdown teardown (game.shutdown + nexus.shutdown) |
| `feat/tower-white-concrete` | STALE/DEAD | 11 | 128 | 2026-07-02 | feat(tower polish): 3/4 hero + WHITE concrete + substantial spandrel bands |
| `feat/explosive-weapon-row` | STALE/DEAD | 10 | 388 | 2026-06-16 | feat(weapons): add Rocket Launcher row (first Explosive DamageType weapon) |
| `feat/space-pilot` | STALE/DEAD | 10 | 380 | 2026-05-28 | feat(act3): SpacePilotController 6DOF + --test-space + --world space showcase |
| `feat/space-stars` | STALE/DEAD | 10 | 380 | 2026-05-28 | feat(space): procedural starfield system + Act-3 art plan |
| `feat/version-bump-0.4` | STALE/DEAD | 10 | 380 | 2026-05-27 | chore(release): 0.4 — controllers + asset wiring batch (SwimController, NPCSys |
| `integration/intro-composite` | STALE/DEAD | 10 | 217 | 2026-06-24 | docs(intro): composite fold report — union, cvars, all-off==base md5, gates |
| `feat/multipod-boss-damage-types` | STALE/DEAD | 9 | 388 | 2026-06-16 | feat(spire): thread DamageType through all four spire onFire dispatchers |
| `feat/portal-hub` | STALE/DEAD | 9 | 498 | 2026-05-30 | rifthub: blue energy core + wormhole-generator housing + eerie audio |
| `feat/projectile-damage-types` | STALE/DEAD | 8 | 388 | 2026-06-16 | feat(projectile): carry DamageType through LiveProjectile to impact dispatch |
| `feat/act2-extra-fire-damage-types` | STALE/DEAD | 7 | 388 | 2026-06-16 | feat(act2-desert): thread DamageType through Act2Desert::onFire |
| `feat/coop-npcs` | STALE/DEAD | 7 | 394 | 2026-05-29 | fix(coop-npcs): eliminate ally Engage<->Search oscillation (combatant-aware ra |
| `feat/intro-combat` | STALE/DEAD | 6 | 128 | 2026-07-01 | feat(intro): make the cold-open space segment INTERACTIVE — take the stick (Jo |
| `feat/spire-fire-damage-types` | STALE/DEAD | 6 | 388 | 2026-06-01 | feat(spire): thread DamageType through all four spire onFire dispatchers |
| `feat/weapons-artpass` | STALE/DEAD | 6 | 504 | 2026-05-27 | weapons: add Railgun (#10) — high-power precision slug |
| `feat/act2-desert-fire-damage-types` | STALE/DEAD | 5 | 388 | 2026-06-01 | feat(act2-desert): thread DamageType through Act2Desert::onFire |
| `feat/act2-desert-mantis-ambush` | STALE/DEAD | 5 | 388 | 2026-05-30 | feat(act2-desert): Mantis Arbiter wildcard ambush gated by the side-quest |
| `feat/spire-art` | STALE/DEAD | 5 | 504 | 2026-05-29 | feat(spire-art): Stage 3b - spire_art.cpp F3 Hospital dressing + --test-spirea |
| `feat/vis-unify` | STALE/DEAD | 5 | 326 | 2026-06-13 | docs(vis): finalize RESULTS.md acceptance — measured 2026-06-12 numbers |
| `feat/act2-content-reconciled` | STALE/DEAD | 4 | 217 | 2026-06-13 | act2(docs): reconcile ACT2_CONTENT_INDEX to the real x3.mission/1 |
| `feat/act2-desert` | STALE/DEAD | 4 | 508 | 2026-05-25 | docs(i5000): fix HW-table storage mapping (C:=980 PRO 2TB/OS, E:=960 EVO/repo) |
| `feat/act2-desert-nordic-mentor` | STALE/DEAD | 4 | 388 | 2026-05-29 | feat(act2-desert): seat a Nordic Steward mentor in the L11 Salvari camp |
| `feat/canon-play-fire-damage-types` | STALE/DEAD | 4 | 388 | 2026-06-01 | feat(canon-play): thread DamageType through CanonPlay::onFire + main dispatch |
| `feat/enemy-artpass` | STALE/DEAD | 4 | 391 | 2026-05-27 | enemy art: wire the REAL Synth character as blue_synth_seed1.glb (18.6->8.6MB) |
| `integration/unified-launch` | STALE/DEAD | 4 | 128 | 2026-07-01 | proof: STRATA descent bore — layered bands + glowing depths (unified set compl |
| `feat/act2-desert-grey-patrol` | STALE/DEAD | 3 | 388 | 2026-05-28 | feat(act2-desert): swap L10 patrol's ranged unit to canon GreyTasked drone |
| `feat/canon-aliens` | STALE/DEAD | 3 | 217 | 2026-07-03 | chore(fleet): restore fleet_inbox.py hook (never blocks prompt) |
| `feat/fable-d14-d15-script-gpucull` | STALE/DEAD | 3 | 387 | 2026-06-09 | build(fable-d14-d15): wire D14 script + D15 gpucull into build; MSVC sol2 fixe |
| `feat/i5000-fleet-specs` | STALE/DEAD | 3 | 496 | 2026-05-25 | docs(fleet): fix i5000 storage mapping + add GPU single-device note |
| `feat/intro-hero-assets` | STALE/DEAD | 3 | 217 | 2026-06-24 | intro-assets(A3): wire the station into the cold-open cutscene as a beauty bea |
| `feat/main-fire-damage-types` | STALE/DEAD | 3 | 388 | 2026-06-01 | feat(main): thread DamageType through Level1Game::onFire + player fire dispatc |
| `feat/wave2-content` | STALE/DEAD | 3 | 635 | 2026-05-22 | Merge branch 'worktree-agent-af2861a0907afb2ac' into feat/wave2-content |
| `docs/fng-intro` | STALE/DEAD | 2 | 504 | 2026-05-25 | FNG.md: build GREEN on 1080 Ti - smoketest + asset(7/7) + console(8/8) pass, a |
| `feat/act2-content-offensive` | STALE/DEAD | 2 | 670 | 2026-06-13 | content(act2): Act-2 Content Offensive — 14 missions + 6 new chat trees + inde |
| `feat/act2-desert-warlord` | STALE/DEAD | 2 | 388 | 2026-05-28 | feat(act2-desert): wire the Saurian Warlord boss into L10 (gated arena) |
| `feat/canon-aliens-warlord-adaptive` | STALE/DEAD | 2 | 388 | 2026-05-30 | feat(canon-aliens): light up Adaptive Hide on the SaurianWarlord row |
| `feat/car-roster` | STALE/DEAD | 2 | 217 | 2026-06-24 | feat(roster): +14 Modular Cyber Racing cars, --world carshow lineup, --car her |
| `feat/door-code-keypad` | STALE/DEAD | 2 | 719 | 2026-05-22 | feat: functional doors + 7-story elevator + see-through/shimmer fixes + gun ai |
| `feat/god-rays` | STALE/DEAD | 2 | 217 | 2026-06-24 | docs(godrays): showcase screenshots + A/B verification notes |
| `feat/lens-flare` | STALE/DEAD | 2 | 217 | 2026-06-21 | lens flare: headless A/B PostFX override + restraint tuning + proof screenshot |
| `feat/mantis-polish` | STALE/DEAD | 2 | 388 | 2026-06-16 | feat(canon-aliens): promote Mantis Arbiter to mini-boss with rage phase machin |
| `feat/openworld` | STALE/DEAD | 2 | 516 | 2026-05-25 | open-world: STATUS — feat/openworld gated (Release+Debug, 0 VUID, alloc=0), RE |
| `feat/weapon-damage-types` | STALE/DEAD | 2 | 388 | 2026-06-01 | feat(weapons): stamp DamageType on every WeaponDef + ResolvedFire shot |
| `backup/canon-aliens-local` | STALE/DEAD | 1 | 217 | 2026-05-27 | feat(canon-aliens): port the four 'most reported' species into MonsterSystem:: |
| `docs/canon-aliens-adaptive-hide` | STALE/DEAD | 1 | 388 | 2026-05-27 | docs(canon-aliens): Adaptive-Hide engine-extension proposal (monster.* lane) |
| `docs/fleet-localllm` | STALE/DEAD | 1 | 502 | 2026-05-26 | docs(fleet): local-LLM via LM Studio router guide (optional cost lever) |
| `docs/monster-def-json` | STALE/DEAD | 1 | 388 | 2026-05-27 | docs(roster): data-driven monster_def.json loader proposal |
| `docs/studio-doctrine` | STALE/DEAD | 1 | 217 | 2026-07-05 | docs: add studio doctrine — exec-producer operating model for fleet dev |
| `feat/adaptive-hide` | STALE/DEAD | 1 | 388 | 2026-05-30 | feat(monster): Adaptive-Hide engine extension (canon-aliens rotate-damage-type |
| `feat/coldopen-always` | STALE/DEAD | 1 | 206 | 2026-06-16 | feat(intro): cold open plays EVERY launch + skip on K only |
| `feat/fleet-hw-format` | STALE/DEAD | 1 | 505 | 2026-05-25 | docs: add NOTE_TO_FLEET — standard HW-snapshot format + probe script |
| `feat/headless-capture-fix` | STALE/DEAD | 1 | 388 | 2026-05-27 | engine: fix headless capture grabbing only clear color + add pixel-variance as |
| `feat/level-editor` | STALE/DEAD | 1 | 504 | 2026-05-26 | docs(editor): seed native Level Editor lane — FP walkaround + AI texture/asset |
| `feat/note-to-13700k` | STALE/DEAD | 1 | 504 | 2026-05-26 | docs(note): I5000 -> 13700K merge requests (fleet-specs row priority) |
| `feat/note-to-14900k-hwformat` | STALE/DEAD | 1 | 504 | 2026-05-25 | docs: NOTE_TO_14900K — please use the fleet HW-snapshot format |
| `feat/note-to-i5000-skills` | STALE/DEAD | 1 | 504 | 2026-05-25 | docs: NOTE_TO_I5000 — ask for skills/plugins manifest |
| `feat/npc-controller` | STALE/DEAD | 1 | 388 | 2026-05-27 | feat(npc): NPCSystem controller for non-combatant world NPCs |
| `feat/rotor-spin` | STALE/DEAD | 1 | 504 | 2026-05-25 | feat(rotor): procedural rotor/propeller spin prototype (RotorSpin) + --test-ro |
| `feat/swim-controller` | STALE/DEAD | 1 | 388 | 2026-05-27 | swim: Act 4 undersea SwimController + --test-swim (8/8) + --world swim showcas |
| `feat/versioning` | STALE/DEAD | 1 | 504 | 2026-05-25 | feat(versioning): build-time 0.3.<commit-count> version system |
| `feat/vis-unify-rebased` | STALE/DEAD | 1 | 217 | 2026-06-13 | feat(vis): port vis-unify r_vis policy onto folded multi-consumer RT base |
| `feat/wire-new-glbs` | STALE/DEAD | 1 | 388 | 2026-05-27 | feat(wire-new-glbs): wire 8 new rigged GLBs into engine |
| `integration/playtest-launch` | STALE/DEAD | 1 | 129 | 2026-07-01 | merge feat/strata-descent: STRATA geological descent (facility base -> Club 11 |
| `backup/visual-pass-good` | ALREADY IN | 0 | 526 | 2026-05-24 | merge: role-based multi-font HUD (Orbitron/Space Grotesk/Tektur/Space Mono + p |
| `docs/clean-room-license-correction` | ALREADY IN | 0 | 718 | 2026-05-21 | docs: correct license posture — engine is original clean-room work, not an RBD |
| `docs/feature-goals` | ALREADY IN | 0 | 559 | 2026-05-23 | docs: add X3Native feature goals (physics + ragdoll) with UE 5.7 reference scr |
| `docs/narrative-pack` | ALREADY IN | 0 | 334 | 2026-06-11 | docs(narrative): NPC chat-tree pack + storyline expansions |
| `docs/narrative-spice` | ALREADY IN | 0 | 330 | 2026-06-11 | narrative: SPICE_GUIDE (heat ladder + consent grammar) and chattree ref checke |
| `feat/14900k-content` | ALREADY IN | 0 | 670 | 2026-05-22 | Add Club 1127 + flooded cave/tunnel network (--world club) |
| `feat/act1-surface-start` | ALREADY IN | 0 | 167 | 2026-06-18 | intro(P7): ESCAPED-branch Act-1 surface-landing start (outside the glass facil |
| `feat/act2-caves` | ALREADY IN | 0 | 514 | 2026-05-24 | DJBOOTH: amend STATUS — gate PASSED, READY FOR INTEGRATION |
| `feat/act2-roster` | ALREADY IN | 0 | 552 | 2026-05-23 | DJBOOTH: amend STATUS — env now installed, handed off to 13700K |
| `feat/act2-world` | ALREADY IN | 0 | 566 | 2026-05-23 | Act-2 world: status report — READY FOR INTEGRATION (44/44 gate + smoketests gr |
| `feat/asset-guard` | ALREADY IN | 0 | 333 | 2026-06-12 | guard: mark hook + sh installer executable |
| `feat/asset-store` | ALREADY IN | 0 | 330 | 2026-06-12 | asset store Phase A: boot-time manifest check + docs PROPOSED->IMPLEMENTED |
| `feat/asset-store-d-primary` | ALREADY IN | 0 | 164 | 2026-06-24 | asset store: make local D: the PRIMARY tier (G: share = backup mirror) |
| `feat/building-cohesive` | ALREADY IN | 0 | 209 | 2026-06-24 | level: seal exterior inter-floor gaps + basic AAA structural pass (whole build |
| `feat/cold-open` | ALREADY IN | 0 | 306 | 2026-06-11 | docs(coldopen): THE FILM — 11-still strip of the cold open |
| `feat/core-elevator` | ALREADY IN | 0 | 214 | 2026-06-24 | feat(elevator): brighten cab key light + reframe interior beauty cam |
| `feat/cpu-frustum-cull` | ALREADY IN | 0 | 334 | 2026-06-09 | feat(render): CPU per-object frustum cull (r_frustumcull, D15 baseline) |
| `feat/ddgi` | ALREADY IN | 0 | 320 | 2026-06-11 | fix(rhi): headless offscreen recreate now rewires RT-AO targets/descriptors (V |
| `feat/dialog-runner` | ALREADY IN | 0 | 328 | 2026-06-11 | app(chattree): ASCII-fold dialog text + width-measured wrap; Lena UI proof sho |
| `feat/dlss-velocity` | ALREADY IN | 0 | 199 | 2026-06-16 | docs(dlss): velocity+DLSS report — PART 2 DLSS scaffolded (no SDK), exact drop |
| `feat/doors-death-anim` | ALREADY IN | 0 | 335 | 2026-06-09 | docs(14900K): SYNC done — branch reconciled, 0 behind main, ready to promote |
| `feat/editor-phase3b` | ALREADY IN | 0 | 392 | 2026-06-06 | feat(editor): P3 transform gizmo + Details panel + undo/redo |
| `feat/editor-phase4` | ALREADY IN | 0 | 386 | 2026-06-06 | feat(editor): F3 content/model browser — place GLB props into the level |
| `feat/editor-phase5` | ALREADY IN | 0 | 383 | 2026-06-06 | feat(editor): phase5 - Doom-Builder "Visual Mode" keyboard nudge editing |
| `feat/fast-boot` | ALREADY IN | 0 | 311 | 2026-06-11 | docs: BOOT_TIME.md final receipts — boottime 5/5 PASS median 1595 ms, suite 83 |
| `feat/floors2-7-dims` | ALREADY IN | 0 | 553 | 2026-05-23 | Floors 2-7 dims: status report — READY FOR INTEGRATION (46/46 gate + smoketest |
| `feat/gpu-cull` | ALREADY IN | 0 | 326 | 2026-06-12 | docs(cull): D15 handoff updated post-bring-up — Tiers 0/1+HZB GREEN, Tier 2 pr |
| `feat/gpu-cull-test` | ALREADY IN | 0 | 334 | 2026-06-09 | D15 GPU-culling test integration: shaders 4/4 SPIR-V, meshlet self-test 7/7 |
| `feat/holoterm-hologram` | ALREADY IN | 0 | 506 | 2026-05-24 | feat(holoterm): make the cell terminal an actual glowing blue hologram |
| `feat/intro-branch-wiring` | ALREADY IN | 0 | 172 | 2026-06-18 | intro(P4): branch the game start on intro.outcome in app_run + CLI/test plumbi |
| `feat/intro-cinematics` | ALREADY IN | 0 | 169 | 2026-06-18 | intro(P5): author the cold-open clip beats, blob->detailed reveal, two stinger |
| `feat/intro-descent` | ALREADY IN | 0 | 168 | 2026-06-18 | intro(P6): ion-pulse descent beat on the ESCAPED branch + surface hand-off |
| `feat/intro-orchestrator` | ALREADY IN | 0 | 174 | 2026-06-18 | feat(intro): Phase 3 Intro Orchestrator — beat machine + skill->outcome->flag |
| `feat/level-loader` | ALREADY IN | 0 | 334 | 2026-06-11 | feat: data-driven LevelDoc loader — --world fromdoc + hot reload + --test-load |
| `feat/living-world` | ALREADY IN | 0 | 305 | 2026-06-11 | docs(screenshots): livingworld proof README (3 pillars, flags, verdicts) |
| `feat/llm-npc` | ALREADY IN | 0 | 319 | 2026-06-11 | docs(llm): Qwen2.5-3B license corrected (Qwen Research, NON-commercial) + any- |
| `feat/lua-scripting` | ALREADY IN | 0 | 334 | 2026-06-09 | D14: integrate Fable's Lua script system (pak-shipped behavior) |
| `feat/lua-trigger-binding` | ALREADY IN | 0 | 331 | 2026-06-11 | app: --test-hatch end-to-end secret-hatch chain self-test (C1-C8) |
| `feat/mission-system` | ALREADY IN | 0 | 327 | 2026-06-11 | app: x3.mission/1 mission system as data — story_ops factor + MissionRunner +  |
| `feat/multi-font-roles` | ALREADY IN | 0 | 528 | 2026-05-24 | font: role-based multi-font HUD/UI system (proportional, done right) |
| `feat/perf-shops` | ALREADY IN | 0 | 310 | 2026-06-11 | chore: gitignore vehbuild.json (per-machine performance-shop save) |
| `feat/post-stack` | ALREADY IN | 0 | 332 | 2026-06-11 | docs(screenshots): post-stack before/after proof — AE lifts dark interiors, no |
| `feat/reflections` | ALREADY IN | 0 | 324 | 2026-06-11 | docs(screenshots): reflections motion-frame sizzle + README verdicts (suite 80 |
| `feat/rt-acoustics` | ALREADY IN | 0 | 315 | 2026-06-11 | docs(rta): suite verdicts — 9/9 acoustics, 11/11 audio, 81/82 sweep (collapse= |
| `feat/rt-shadows` | ALREADY IN | 0 | 319 | 2026-06-11 | feat(render): RT soft shadows (r_rtshadows) — sun + point-light inline ray-que |
| `feat/skinned-tlas` | ALREADY IN | 0 | 203 | 2026-06-16 | perf(rt): document measured skinned-AS refit cost; drop temp probe |
| `feat/strata-descent` | ALREADY IN | 0 | 186 | 2026-06-26 | feat(strata): THE DESCENT — scenic layered geology + explorable caves (facilit |
| `feat/taa` | ALREADY IN | 0 | 330 | 2026-06-11 | docs(screenshots): TAA before/after proof — checker crawl resolved, A/B bit-id |
| `feat/tlas-doublebuffer` | ALREADY IN | 0 | 201 | 2026-06-16 | docs(cull): defer Tier-2 meshlets (#5 PART 2) with the exact 6 remaining steps |
| `feat/upper-floor-content` | ALREADY IN | 0 | 185 | 2026-06-26 | feat(canon): --screenshot-upperfloors proof shots + capture path |
| `feat/vehicles` | ALREADY IN | 0 | 318 | 2026-06-11 | feat(vehicles): drive core — hero-car GLB skin on Jolt wheels, E enter/exit, e |
| `feat/weapon-grip-tune` | ALREADY IN | 0 | 391 | 2026-06-06 | feat(3p): held-weapon grip LIVE-TUNE tool (TASK#53) |
| `feat/weapon-real-models` | ALREADY IN | 0 | 499 | 2026-05-25 | feat(weapons): give all 7 FPS weapons real PBR-textured viewmodels |
| `feat/weapon-sfx-fx` | ALREADY IN | 0 | 499 | 2026-05-25 | weapons: distinct per-weapon fire SFX + per-weapon muzzle/impact FX |
| `feat/weapon-variety` | ALREADY IN | 0 | 212 | 2026-06-24 | weapon-variety: re-skin LIGHTNING GLB — dark body + electric-blue/cyan glow |
| `feat/world-blueprint` | ALREADY IN | 0 | 553 | 2026-05-23 | blueprint: resolve the 4 open design decisions (Tim, 2026-05-23) |
| `feat/world-map` | ALREADY IN | 0 | 321 | 2026-06-12 | chore: drop stray one-shot wiring script |
| `feat/world-streaming` | ALREADY IN | 0 | 324 | 2026-06-11 | test(worldstream): config-aware W4 budget gate — Release 33 ms bar unchanged,  |
| `feat/zero-stutter` | ALREADY IN | 0 | 316 | 2026-06-11 | docs: ZERO_STUTTER.md final receipts — 83/83 suite, 5/5 flythrough (0 spikes), |
| `fix/cell-hatch` | ALREADY IN | 0 | 188 | 2026-06-16 | fix(hatch): flush two-panel cell-floor hatch that parts from centre + clears c |
| `fix/coldopen-3d-path` | ALREADY IN | 0 | 176 | 2026-06-18 | fix(coldopen): light the 200m capital ship so the 3D film reads (no black blob |
| `fix/elevator` | ALREADY IN | 0 | 188 | 2026-06-16 | elevator: real audio, interior/disco lighting, sliding doors + status UX |
| `fix/enemy-overhaul` | ALREADY IN | 0 | 188 | 2026-06-16 | Enemy overhaul: LOS-gated attacks, boss anim, separation, enemy SFX |
| `fix/holo-glass` | ALREADY IN | 0 | 188 | 2026-06-16 | fix(holo): cell terminal screen reads as real glass + round-section trim |
| `fix/metal-ambient` | ALREADY IN | 0 | 334 | 2026-06-11 | fix(render): metal ambient-specular floor — metals no longer go black in dark  |
| `fix/planets-sky` | ALREADY IN | 0 | 334 | 2026-06-11 | fix(planets): celestial sky-layer placement — direction-anchored, parallax-fre |
| `fix/playtest-combat` | ALREADY IN | 0 | 208 | 2026-06-24 | fix(assets): update manifest sha256+size for baked Attack-clip rigs |
| `fix/stability` | ALREADY IN | 0 | 333 | 2026-06-11 | fix(phys): join engine job workers BEFORE destroying the Jolt job bridge (rare |
| `fix/weapons-polish` | ALREADY IN | 0 | 188 | 2026-06-16 | fix(weapons): lightning polish + arsenal tuning/SFX/visual pass |
| `integration/combat-consolidated` | ALREADY IN | 0 | 179 | 2026-06-16 | merge: reconcile weapons-polish onto DamageType+enemy base |
| `integration/culling-glass` | ALREADY IN | 0 | 394 | 2026-05-26 | Merge remote-tracking branch 'origin/feat/doors-death-anim' |
| `integration/damagetype-gated` | ALREADY IN | 0 | 187 | 2026-06-16 | feat(combat): gate + re-home fleet DamageType system onto split base |
| `integration/empire-fold` | ALREADY IN | 0 | 217 | 2026-06-13 | fold(report): THE GREAT FOLD integration report |
| `integration/honor-fable` | ALREADY IN | 0 | 193 | 2026-06-16 | Merge remote-tracking branch 'origin/feat/dlss-velocity' into integration/hono |
| `integration/honor-fable-final` | ALREADY IN | 0 | 23 | 2026-07-07 | FOLD COMPLETE: absorb the pre-split playable-build monolith line |
| `integration/main-plus-session` | ALREADY IN | 0 | 454 | 2026-05-24 | fix(physics): unify the two constraint APIs without a crash/regression |
| `integration/playable-build` | ALREADY IN | 0 | 0 | 2026-07-11 | feat(city): STREET LIGHT — real cones, pooled light, a color story, lived-in v |
| `integration/space-stack-folded` | ALREADY IN | 0 | 176 | 2026-06-18 | feat(space): fold the space-combat stack onto the split base (Intro P2) |
| `main` | ALREADY IN | 0 | 217 | 2026-06-13 | fold(report): THE GREAT FOLD integration report |
| `polish/opening-space` | ALREADY IN | 0 | 163 | 2026-06-20 | polish(opening) R2: AAA detention cell — PBR walls, hero detail, atmosphere |
| `polish/opening-space-r3` | ALREADY IN | 0 | 162 | 2026-06-24 | polish(opening) R3: close the hero corner — wall variety + dead-corner light + |
| `refactor/monolith-split` | ALREADY IN | 0 | 201 | 2026-06-16 | docs(#28): document the deep core split (Part 3) — HostContext + 4 phases |
| `refactor/split-device-core` | ALREADY IN | 0 | 209 | 2026-06-13 | refactor(#28): split vk_pipelines/vk_passes further — every TU under ~2500 lin |
| `refactor/split-main-core` | ALREADY IN | 0 | 210 | 2026-06-13 | docs(#28): report Part 2 — main() core split (test_registry extraction) |
| `refactor/split-main-deep` | ALREADY IN | 0 | 201 | 2026-06-16 | docs(#28): document the deep core split (Part 3) — HostContext + 4 phases |
| `specs/jk-idtech8-enhance` | ALREADY IN | 0 | 717 | 2026-05-21 | docs(roadmap): fold T3 "beyond idTech 8" features into the roadmap |
| `worktree-agent-a50c7fe1642240ac9` | ALREADY IN | 0 | 536 | 2026-05-24 | feat(secret): code-locked cell trapdoor -> stocked secret room |
| `worktree-agent-a891d6135e6fe6427` | ALREADY IN | 0 | 507 | 2026-05-24 | level1: replace flat checker graybox with richer procedural sci-fi textures |
| `worktree-agent-a99a8e8367e5fe52c` | ALREADY IN | 0 | 525 | 2026-05-24 | art: refine sci-fi level textures — deck floor, calm large-scale walls, 3 wall |
| `worktree-agent-acb444fb8fa8f56dc` | ALREADY IN | 0 | 525 | 2026-05-24 | feat(hud): enemy nameplates + real minimap radar |
| `worktree-agent-adc96d327cb9718fd` | ALREADY IN | 0 | 525 | 2026-05-24 | rt: hardware ray-query RT ambient occlusion (BLAS/TLAS + inline AO) |
| `worktree-agent-addcc700b734b9c24` | ALREADY IN | 0 | 508 | 2026-05-24 | feat(holoterm): make the cell terminal an actual glowing blue hologram |
| `worktree-agent-ae5ef32386836f5c1` | ALREADY IN | 0 | 507 | 2026-05-24 | Replace blocky 8x8 bitmap HUD/menu font with a real TTF (Roboto Mono) |
| `worktree-agent-aeff0496fb05b9a1a` | ALREADY IN | 0 | 515 | 2026-05-24 | gibs: monsters EXPLODE into chunks + blood when they die |
| `worktree-agent-af060330558559680` | ALREADY IN | 0 | 437 | 2026-05-25 | TASK#12: wire the SKINNED death ragdoll into monster death |
| `worktree-agent-af5096c6da2e5a915` | ALREADY IN | 0 | 503 | 2026-05-24 | feat(holoterm): render a real sci-fi SECURITY-CONSOLE HUD on the cell glass |
| `worktree-agent-aff29e6b389c7efad` | ALREADY IN | 0 | 515 | 2026-05-24 | feat: rescued NPC (captive girl) is now INTERACTABLE -> flirty companion |


---

## 3. LAND NOW — the crown jewels, in detail

### 3.1 `feat/intro-cockpit` — **THE crown jewel** (43 new, only 14 behind, 2026-07-11)

The single highest-value branch in the repo. It is essentially *playable-build + 43 commits of owner-visible fixes*, forked only 14 commits ago.

**What it fixes/adds:**
- `124f523` **THE EVERYTHING BUILD** — ports the 14900K's AAA weapon textures (the camo-pistol payload) onto the playable-build line.
- `6574e07` — three owner-reported live-play defects: **cell scale, held-weapon texture, terminal UI**.
- `8c6e7c1` — terminal: **glossy dark glass + bigger readable text** (owner lock-down).
- `b81f884` — console scrollback + VIGIL digit-choice routing (two live-play UX bugs).
- `03b77ff` — **RT AO + DDGI GI default ON** for RT-capable devices (owner: *"Ray Tracing default should be ON on the 3090 Ti"*).
- `5fbd0c8`, `564e664`, `8058578`, `162bb69` — **the F2 Medical Rescue Rooms** (Aria / Keisha / Emily), supine posing, head-at-pillow fix, dark-glass vitals monitors. This is the rescue storyline the owner has been asking for.
- `694ab49` — **engine(physics): BODY CONTACT** — bone-surface solver + soft-surface indentation. A genuinely new engine feature.
- `551c0f2` — **THE DESCENT RIDE** — coaster-grade B1 → −178 m chute + generic track layer.
- `cf3dd37`, `72dee0d` — folds **wormhole transit + crystal-matrix VFX + tractor beam** and **walkable ship interior + true-portal windows** onto the line.
- `03214f9`, `fb803a4`, `ed76165`, `14e45c6` — the real ship interior (Scifi Kit Vol 3), SD3.5 hull-panel set, Floors 2–7 beauty pass.
- `45e1c46` — fix black-silhouette metallic kit props (⚠️ **band-aid suspect** — see §5).
- `94506b4` — **X3_WORLD_RULES.md**, the model/asset constitution.

**Conflicts:** dry-run `git merge-tree` says **exactly 2**:
- `app/CMakeLists.txt` (both sides added files — trivial union resolve)
- `app/world_hosts/host_surface_start.cpp`

**Risk:** LOW on code, **HIGH on assets.** All 129 asset binaries are LFS-stranded and unfetchable (§1). The 18 SD3.5 surface sets from `1622584` are the ones FLEET.md line 152 is asking the forger to copy to `G:`.

**Command (after the LFS problem is solved — see landing plan):**
```bash
git checkout integration/playable-build && git pull
git merge origin/feat/intro-cockpit        # resolve 2 conflicts
```

### 3.2 `feat/weapon-textures` — the tool that unblocks the camo pistol

Superseded for *content* by `intro-cockpit` (same fixed GLB oid `28a84e3d…`), but it is the **only** branch carrying `tools/rebind_weapon_textures.py` and the `convert_obj_glb.py` `texdir` default fix — i.e. the only way to **regenerate** the assets now that LFS is dead.

**Cherry-pick just the tooling:**
```bash
git checkout origin/feat/weapon-textures -- tools/rebind_weapon_textures.py tools/convert_obj_glb.py
python tools/rebind_weapon_textures.py     # regenerates the 5 GLBs from Rodin
python tools/asset_store.py publish assets/rigged_glb/Weapon*.glb --note "per-weapon PBR rebind (ba3ce7a)"
```

**Note:** `vmLitPBR` (the hardcoded-gunmetal hack `ba3ce7a` removes) still has 2 hits in `intro-cockpit`'s `weapon.cpp`. `ba3ce7a` also cuts viewmodel brightness 2.6× → 1.4×. Verify the port kept both changes; if not, take them from `ba3ce7a`.

### 3.3 `feat/city-fix` — the city is grey boxes and shouldn't be (60 new, 128 behind)

**The head of a 12-branch city/terrain chain** (absorbs `city-uplift`, `city-aaa`, `neon-city`, `terrain-aaa`, `world-terrain`, `texture-offensive`, `weapons-overhaul`, `gpu-llm`, `input-capture-lockup`, `monster-perception`, `integration/v002`, `integration/unified-launch` as strict ancestors).

**What it adds that PB genuinely lacks:** its `app/city.cpp` (725 lines) loads `CyberpunkCity/SM_MERGED_BP_*.glb` (6 real facades) + 14 `CityProps/*.glb` (billboards, dumpsters, street lamps, trash, vents, AC condensers, pallets). **PB's `app/city.cpp` (569 lines) contains zero `.glb` references, and PB's tree has zero files under `assets/converted_glb/CyberpunkCity` or `CityProps`.** PB's city is procedural boxes.

**⚠️ DO NOT `git merge` THIS.** It is 128 behind and PB independently rewrote the same files. `ed1a403..city-fix` and `ed1a403..PB` overlap on **150 files** including `app/city.cpp`, `app/app_run.cpp`, `app/canon_play.cpp`, `app/cell_dressing.cpp`. A merge means reconciling two independent rewrites of the city by hand.

**Port the payload instead:**
```bash
# 1. assets + tooling (apply cleanly, no PB counterpart)
git checkout origin/feat/city-fix -- assets/converted_glb/CyberpunkCity assets/converted_glb/CityProps \
    tools/build_city_facades.py tools/convert_city_props.py tools/patch_facade_textures.py
# 2. graft the facade/prop placement tables from city-fix's app/city.cpp into PB's app/city.cpp BY HAND
git diff origin/integration/playable-build:app/city.cpp origin/feat/city-fix:app/city.cpp
```
**Risk: MEDIUM-HIGH.** This is the one item that needs real engineering, not a merge. Budget a session.

### 3.4 Clean cherry-picks — new files with no PB counterpart (LOW risk)

These add files that **do not exist in playable-build**, so they apply without conflict:

| Branch | New file(s) absent from PB | What it fixes |
|---|---|---|
| `fix/input-capture-lockup` | `app/input_capture.{cpp,h}`, `InputCaptureManager` | **P0** input lockup; ESC failsafe + watchdog. PB has only `app/input_globals.h`. |
| `fix/monster-perception` | `--test-monsterperception` gate | **P1** monster LOS blindness (self-intersecting ray). Zero hits in PB. |
| `feat/holo-glass-platform` | `app/holo_panel.{h,cpp}` | HoloPanel platform + 4 variants. PB's `holo_terminal.h` only *mentions* it in a comment. `placard` = 0 hits in PB. |
| `feat/living-city` | `app/npc_life.{cpp,h}` | Daily schedules, 12 archetypes, scan-card karma, bank-robbery set-piece. PB's `LIVING NPCs` added converse/work/play to `crowd.cpp` but **no daily loop, no robbery**. |
| `feat/texture-offensive` | `docs/design/ART_DIRECTION.md`, `docs/TEXTURE_CATALOG.md` | The art bible. Both absent from PB. |
| `feat/weapons-overhaul` | lightning `battery` / `arc-tendril` / `zigzag` in `weapon.cpp` | Weapon FX. **Zero** hits for all three in PB's `weapon.cpp`. Not a new file — small targeted diff. |
| `feat/npc-characters` | `triggerRagdoll` **only** | PB re-implemented this branch as `app/crowd_skin.cpp` (Skinned Citizens). **Only the ragdoll hit-react/death flop is stranded.** Harvest that, discard the rest. |
| `fix/level-architecture` | `--test-doorscan`, `docs/LEVEL_LINT_BASELINE.md` | PB *has* `app/level_lint.cpp` but not the doorscan gate or the seam/tube fixes. Partial. |

### 3.5 `feat/tractor-beam` — the space-wave superset (71 new, 380 behind)

**Correction to a natural assumption:** `feat/space-wave1-integrated` is **NOT** the roll-up of the S0–S12 space slices — **`feat/tractor-beam` is.** All 18 space siblings are ancestors of `tractor-beam`; `space-wave1-integrated` integrated the *base*, not the *slices*, and is missing ship_interior/windows/repair/anim, both wormhole modules, and tractor_beam. **Deleting the siblings on the assumption that `space-wave1-integrated` covers them would silently destroy real work.**

`feat/intro-cockpit` already folds *most* of this (wormhole_transit, wormhole_vfx, ship_interior, ship_windows, tractor_beam). After intro-cockpit lands, `tractor-beam` is still the **sole** carrier of:

`app/space/ship_repair.*`, `app/space/ship_anim.*`, `app/mech_pilot.*`, `shaders/wormhole.frag`, `tools/gen_lod.py`, `tools/ship_node_anim.py`

**Live bug it repairs:** `SpaceLayer::registerWormholeRunner()` exists in PB (`app/space/space_layer.cpp:202`) but **nothing ever calls it** — only `registerDescentRunner` is wired. `requestWormhole()` arms `Pending::Wormhole` and hangs forever. Wormhole transit is a dead stub in the shipping line.

**Risk: HIGH** (380 behind). Harvest the 6 files above rather than merging.

---

## 4. The landing plan (ordered, each step independently gated)

> **Gate after every step:** Release build green · `--smoketest` exit 0 · VUID = 0 · `allocationCount = 0`.
> **Push protocol (FLEET.md §Push protocol):** large promotions route through the **13700K integrator**. Back up with a tag before each risky step — the May-24 `backup/*` tags saved the project once.

### STEP 0 — Free, zero-risk, do it first: fast-forward `main`

`origin/main` is a strict ancestor of playable-build. This is a pointer move, not a merge.

```bash
git fetch origin
git checkout main
git merge --ff-only origin/integration/playable-build   # 217 commits, cannot conflict
git push origin main
```
**Verify:** `git rev-list --left-right --count origin/main...origin/integration/playable-build` → `0  0`.

### STEP 1 — Tag the world before touching anything

```bash
git tag backup/pre-integration-0711 origin/integration/playable-build
git push origin backup/pre-integration-0711
```

### STEP 2 — Kill the camo pistol (highest owner-visible impact, no LFS needed)

Do this **on playable-build directly** — it is small, self-contained, and does not wait on the big merge.

```bash
git checkout integration/playable-build
git checkout origin/feat/weapon-textures -- tools/rebind_weapon_textures.py tools/convert_obj_glb.py
python tools/rebind_weapon_textures.py            # regenerates 5 GLBs from D:\GameDev\EscapeLab3D-RBDOOM\...\rodin
python tools/asset_store.py publish assets/rigged_glb/Weapon*.glb --note "per-weapon PBR rebind (ba3ce7a)"
git cherry-pick -n ba3ce7a                        # take app/weapon.cpp: drop vmLitPBR hack, viewmodel 2.6x -> 1.4x
# resolve: keep the code hunks; the GLBs you just regenerated win over the LFS pointers
python tools/asset_store.py verify                # MUST be green
```
**Gate:** launch, look at all 5 weapons in FP. Owner-verifiable. **Screenshot and show him.**
**Note:** `WeaponShotgun2` has no source textures (dead `C:/` mtl refs) — `ba3ce7a` gives it a procedural brushed-gunmetal stand-in and flags it as needing real art. Expect that one to still look plain.

### STEP 3 — Solve the LFS/asset problem (blocks Step 4)

Before `feat/intro-cockpit` can land anywhere, its 129 binaries must reach a store. **This requires the machine that forged them (the 14900K).** Per FLEET.md line 152, this is already an open ask.

```bash
# ON THE 14900K (the forger):
python tools/asset_store.py publish assets/surface_library assets/converted_glb assets/rigged_glb assets/textures
# and per FLEET.md, copy the 18 intro-cockpit surface sets to G:\Assets\X3Native\surface_library\
# THEN, on the integrator:
python tools/asset_store.py fetch --all && python tools/asset_store.py verify   # must exit 0
```
**Blocked on:** 14900K. **This is the critical path for items 1–5 of the top 10.**

### STEP 4 — Land `feat/intro-cockpit` (the crown jewel)

```bash
git checkout integration/playable-build
git merge origin/feat/intro-cockpit
# resolve exactly 2: app/CMakeLists.txt (union), app/world_hosts/host_surface_start.cpp
python tools/asset_store.py verify        # must be green, else STOP — you have dangling pointers
```
**Gate:** build + smoketest + **walk the F2 rescue rooms, open the terminal, fire every weapon.**
**Delivers:** top-10 items 1–5 + the rescue storyline + BODY CONTACT physics + the descent ride.

### STEP 5 — The clean cherry-picks (LOW risk, big felt value)

Each is independently gateable. Do them one at a time.

```bash
git cherry-pick <sha>   # fix/input-capture-lockup  -> app/input_capture.{cpp,h}   (P0)
git cherry-pick <sha>   # fix/monster-perception    -> --test-monsterperception    (P1)
git cherry-pick <sha>   # feat/holo-glass-platform  -> app/holo_panel.{h,cpp}
git cherry-pick <sha>   # feat/living-city          -> app/npc_life.{cpp,h}
git checkout origin/feat/texture-offensive -- docs/design/ART_DIRECTION.md docs/TEXTURE_CATALOG.md
# feat/weapons-overhaul: targeted diff into app/weapon.cpp (lightning charge / zigzag / arc-tendril / battery)
# feat/npc-characters: harvest triggerRagdoll ONLY
```
**Gate after each.** Resolve exact SHAs with `git log --oneline origin/integration/playable-build..origin/<branch>`.

### STEP 6 — Land `feat/rifthub-aaa` (coordinate — it is ACTIVE)

The portal hub exists on **no** shipping branch. `rifthub-aaa` is 49 ahead / 4 behind and is its only home (`app/rifthub.cpp` = 176 KB). **Talk to whoever is driving it before merging** — and note the `fix/engine-glb-lighting` worktree has *already merged* rifthub R8 into the engine fix (`856b494`), so these two must land together or in a known order.

### STEP 7 — `feat/city-fix` payload port (the hard one — own session)

Per §3.3. Do not merge; port. Budget real time.

### STEP 8 — `feat/tractor-beam` residue

Harvest `ship_repair.*`, `ship_anim.*`, `mech_pilot.*`, `shaders/wormhole.frag`, `tools/gen_lod.py`, `tools/ship_node_anim.py`. Wire `registerWormholeRunner()` so `requestWormhole()` stops hanging.

### STEP 9 — Prune

Delete the 200 branches in §6 **only after** the heads above have landed and gated.

---

## 5. ⚠️ DUPLICATED / CONFLICTING WORK — the GLB lighting band-aids

**This is the most important thing in this document after the LFS blocker.**

An agent is fixing the **engine-wide GLB lighting bug** right now on `fix/engine-glb-lighting` (worktree `D:/GameDev/X3Native-glblight`):

> `0e837f5` — **fix(engine): GLB meshes light identically to prims — the 1/PI shading-path split**

That is the **root cause**: GLB-sourced meshes and procedural prims went down different shading paths, differing by a factor of 1/π. Every GLB prop therefore rendered **too dark — the "black silhouette" look.**

**Art-level band-aids were applied across the repo to compensate.** When the engine fix lands, each of these will now be *over*-bright or double-lit and must be **UN-hacked**. Suspects, in priority order:

| Commit | Branch | The band-aid | Action after engine fix |
|---|---|---|---|
| `45e1c46` | `feat/intro-cockpit` | *"Fix black-silhouette metallic kit props in the F2-F7 wing floors"* | **PRIME SUSPECT.** Directly compensating for the 1/π bug. Re-evaluate every prop it touched. |
| `ed76165` | `feat/intro-cockpit` | *"…**black-prop fix**, **tall-room light law**"* | Both halves are lighting compensation. Re-check. |
| `921c0f2` | `feat/intro-cockpit` | *"art-director pass: reforge 6 texture-consistency violations"* | Textures reforged to look right **under the broken lighting**. May now read wrong. |
| `d345291` / R5 notes | `feat/rifthub-aaa` | The branch's own doc says the gate *"was never lit (inside-out mesh + **fake self-emissive**)"* | **Fake self-emissive is exactly the hack.** Remove once N·L is correct. |
| `2e71f6c` | `feat/texture-offensive` | *"holo-terminal **emissive clamp** (kill the cyan bloom-blob: albedo 0.55, opacity 0.38, screen 2.6→1.4)"* | Hand-tuned emissive constants. Re-tune. |
| `606c3fa` / `d0d4e21` | `feat/weapon-textures` | *"WHITE-CONCRETE tower now READS white"* after *"material/lighting **reads dark**"* | The tower was re-albedoed to fight the darkness. Will now blow out. |
| `ba3ce7a` | `feat/weapon-textures` | *"cut viewmodel brightness **2.6× → 1.4×**"* | This one is *reducing* an over-brightening — direction is right, but the constant was tuned pre-fix. **Re-verify after the engine fix.** |

**Recommendation to the integrator:** land `fix/engine-glb-lighting` **before or immediately after** `feat/intro-cockpit`, then do a **single deliberate un-hack pass** over the table above with screenshots. Landing the art band-aids and the engine fix *independently, weeks apart* is how you get a build that looks worse than either.

**Other duplicated work found:**
- **Portal hub × 5.** `portal-hub`, `portal-hub-polished`, `portal-hub-rebased`, `rifthub-portal-visuals` all solved the same problem; `feat/rifthub-aaa` is a **from-scratch rewrite** (176 KB `rifthub.cpp` vs 28/34/47 KB) that supersedes all four. Every named feature of the old four — blue energy core, wormhole-generator housing, eerie audio, Stargate stone gates, event-horizon membrane — is present *and better* in `rifthub-aaa` (which ships 3 synthesized WAVs; `portal-hub` committed **no audio files at all**). **Nothing to harvest. Straight delete all 4.**
- **NPC bodies × 2.** `feat/npc-characters` (`npc_character.cpp`) vs PB's own `app/crowd_skin.cpp` — same GLB skinning pipeline, independently re-implemented. PB's wins; harvest only `triggerRagdoll`.
- **Terrain × 2.** `feat/terrain-aaa` / `feat/world-terrain` vs PB's `1ebe8e6 TERRAIN DRAMA`. PB's wins.
- **Tower concrete × 2.** `feat/tower-white-concrete` vs PB's `facility_exterior.cpp` (already has `cc_cement_white` + spandrel bands). PB's wins.
- **Damage types × 9 branches.** All nine were landed by the June-13 GREAT FOLD and are pure duplicates today.

---

## 6. DELETE list

### 6.1 Delete now — 97 branches, zero unique patches (`git cherry` says nothing to lose)

Every branch in the table bucketed **ALREADY IN**. These are fully contained in `playable-build`. Includes 8 branches that *appear* ahead but are 100% patch-equivalent (rebased/cherry-picked in already): `docs/feature-goals`, `feat/editor-phase3b`, `feat/editor-phase4`, `feat/editor-phase5`, `feat/weapon-grip-tune`, and 3 `worktree-agent-*`.

### 6.2 Delete after their head lands — 103 STALE/DEAD

| Group | Branches | Justification |
|---|---|---|
| **Space wave (19)** | `space-layer` `space-env` `space-lod` `wormhole-transit` `atmo-descent` `ship-interior` `ship-windows` `ship-repair` `ship-ai` `ship-targeting` `ship-damage` `ship-art` `eva-spacewalk` `space-wave1-integrated` `wormhole-vfx` `decloak-vfx` `space-pilot` `space-stars` `space-engine-spec` | All ancestors of / superseded by `feat/tractor-beam`. **DELETE ONLY AFTER tractor-beam lands** — `space-wave1-integrated` is NOT a superset. Harvest 3 design docs from `space-engine-spec` first. |
| **Portal hubs (4)** | `portal-hub` `portal-hub-polished` `portal-hub-rebased` `rifthub-portal-visuals` | Superseded by the `rifthub-aaa` rewrite. Nothing to harvest (verified). **Straight delete.** |
| **Damage types (9)** | `weapon-damage-types` `main-fire-damage-types` `spire-fire-damage-types` `canon-play-fire-damage-types` `act2-desert-fire-damage-types` `act2-extra-fire-damage-types` `multipod-boss-damage-types` `projectile-damage-types` `explosive-weapon-row` | All landed by the June-13 GREAT FOLD. `x3_damage.h` + the enum + Rocket Launcher all verified present in PB. **Delete now.** |
| **City chain (12)** | `city-uplift` `city-aaa` `neon-city` `terrain-aaa` `world-terrain` `integration/v002` `integration/v003` `integration/unified-launch` `integration/playtest-launch` `tower-white-concrete` + others | Strict ancestors of `feat/city-fix` / `feat/dialog-live`. Delete after those land. |
| **Fleet/docs/notes (~30)** | `note-to-*` `docs/fng-intro` `fleet-hw-format` `i5000-fleet-specs` `docs/fleet-localllm` etc. | One-commit doc branches, long since absorbed or irrelevant. |
| **Rest (~29)** | May-era feature branches 380–719 behind | Superseded by later work; see table. |

### 6.3 KEEP — do not delete

`integration/playable-build` (canonical) · `main` (after FF) · `feat/rifthub-aaa` (ACTIVE) · `fix/engine-glb-lighting` (ACTIVE) · `echotropolis` (ACTIVE — this is the **Echo Harbor** sibling game: island/RTS camera/day-night; a separate lane, not EFLZ) · `wt/polish` (ACTIVE worktree) · every LAND NOW branch until it has landed and gated · all `backup/*` tags.

---

## 7. Open questions for the 13700K integrator

1. **LFS is dead — what's the ruling?** 84 of 122 manifest entries are in neither asset store. Options: (a) 14900K publishes everything to `D:\Assets\X3AssetStore` + `G:` (FLEET.md already asks this); (b) buy LFS budget; (c) history-rewrite `§6` of `ASSET_DISTRIBUTION.md` (needs Tim's sign-off + all machines coordinated). **Nothing in the intro-cockpit cluster can land until this is answered.**
2. **Can `main` just be fast-forwarded?** It's a zero-risk pointer move (217 commits, 0 conflicts). Any reason not to, or is `playable-build` staying the de-facto trunk permanently? If the latter, say so in FLEET.md — right now the push protocol says "primaries push `main`", which is no longer what anyone does.
3. **Order of `fix/engine-glb-lighting` vs `feat/intro-cockpit`.** The glblight worktree already merged rifthub R8 into itself (`856b494`). intro-cockpit carries the black-prop **band-aids**. Which lands first, and who owns the un-hack pass (§5)?
4. **`feat/city-fix`: port or rewrite?** PB and city-fix independently rewrote `app/city.cpp`. The GLB facades + props are real, finished, and unlanded. Is it worth a session to graft them into PB's city, or is PB's procedural city going to be replaced wholesale anyway?
5. **Who owns `feat/rifthub-aaa`, and is it done?** It is the only home of the entire portal hub. It's 4 behind and active today. When can it land?
6. **`echotropolis` / Echo Harbor — is this a separate game?** It's the current local HEAD and has its own launcher (`Launch Echo Harbor.bat`), island, RTS camera. It's cherry-picking PB commits. Should it be a separate repo?
7. **`tools/slick` (the FleetCommand chat app)** rides along on `feat/cull-combined`, `docs/public-fleetcommand`, `feat/gen-image-workflow`, `feat/minerva-texture` — ~43k lines of TypeScript in the *game engine repo*, all 380 behind. Split it out or delete it?

---

*Audit generated 2026-07-11 against `origin/integration/playable-build` @ `f967213`. 227 remote + 7 local branches enumerated via `git cherry` patch-equivalence, ancestry checks, and tree-content verification. No branches were merged, rebased, or deleted.*
