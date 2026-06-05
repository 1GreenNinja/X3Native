# X3Native — Branch Feature Report

**Generated:** 2026-06-05 · **Repo:** `1GreenNinja/X3Native` · **Refs surveyed:** 166 (local + `origin/*`)
**Current branch:** `feat/cull-combined` · **Default:** `main`

This report groups every active feature branch by lane, using each branch's
latest commit subject as its feature label. Local + `origin/` pairs of the same
name are shown once. Ephemeral `worktree-agent-*` scratch branches are
summarized as a group at the end rather than listed individually.

---

## At a glance

| Lane | Branches | Highlights |
|---|---|---|
| 🚀 Space / Act-3 (S0–S12 + VFX) | ~19 | Full ship-combat + interstellar-transit slice stack |
| 🛸 Ships · cockpits · vehicles | 4 | Jake's Minerva, Vattalus bridge, Firefly cockpit, heavy-mech |
| 🤝 Companion / co-op AI | 5 | Two-brain companion squad + non-combatant NPC system |
| 👾 Enemies · canon-aliens · monsters | 7 | 4 canon species, Adaptive-Hide, Saurian Warlord boss |
| 🗺️ World / levels | ~10 | Open-world, Act-2 desert/caves, floors 2–7, Spire cull |
| 💎 Glass · materials · holo | 5 | UE5 Cook-Torrance glass, holo-terminal kiosks |
| 🔫 Weapons | 5 | Railgun #10, real PBR viewmodels, per-weapon SFX/FX |
| 🔥 Damage-type thread (Jun 1) | 5 | `DamageType` threaded through every `onFire` dispatch |
| ⚙️ Rendering / engine infra | ~10 | Frustum + occlusion cull, RT AO, capture fix, versioning |
| 🌀 Portal hub · swim · misc gameplay | 4 | Rift hub, Act-4 undersea swim, healthbars, AI fixes |
| 📝 Docs · fleet · specs | ~14 | Fleet notes, clean-room license, level-editor lane |
| 🔗 Integration / merge | 5 | `main`, culling-glass, 14900k-test, cull-doors-death-anim |

---

## 🚀 Space / Act-3 — ship combat & interstellar transit

The largest lane: a numbered slice stack (S0–S12) plus standalone VFX.

| Branch | Slice | Feature |
|---|---|---|
| `feat/space-layer` | S0 | SpaceLayer spine — context FSM + env transform + proxy registry |
| `feat/space-env` | S1 | Space environment — nebula/star dome + proxy planets + sun |
| `feat/space-lod` | S2 | Distance-LOD system + Blender LOD-chain generator |
| `feat/wormhole-transit` | S3 | Wormhole transit — crystal-matrix interstellar jump |
| `feat/atmo-descent` | S4 | Cinematic atmospheric descent (AtmoDescent) |
| `feat/ship-interior` | S5 | Walkable, data-driven static ship interior |
| `feat/ship-windows` | S6 | True-portal ship windows — moving space outside the glass |
| `feat/ship-repair` | S7 | Ship repair — open panels + connect wires (transit gameplay) |
| `feat/ship-ai` | S8 | Enemy ship AI — dogfight pillar enemies |
| `feat/ship-targeting` | S9 | Targeting / radar / lock-on dogfight HUD layer |
| `feat/ship-damage` | S10 | Ship damage model — shield/hull/destructible subsystems |
| `feat/ship-art` | S11 | Ship art + node-transform animation (Lane D) |
| `feat/eva-spacewalk` | S12 | EVA spacewalk — zero-G hull-repair controller |
| `feat/space-pilot` | — | SpacePilotController 6DOF + `--test-space` + `--world space` |
| `feat/space-stars` | — | Procedural starfield system + Act-3 art plan |
| `feat/decloak-vfx` | — | Decloak shimmer VFX (intro ship phasing in) |
| `feat/tractor-beam` | — | Tractor-beam VFX for the intro capture beat |
| `feat/wormhole-vfx` | — | Salvari crystal-matrix wormhole VFX (jump transition) |
| `feat/space-wave1-integrated` | — | Wave-1 space integration branch |

---

## 🛸 Ships · cockpits · vehicles

| Branch | Feature |
|---|---|
| `feat/jake-ship` | Baked game-weight Minerva staged as Jake's hero ship |
| `feat/cockpit-vattalus` | Vattalus-style sci-fi bridge for `--world ship-interior` |
| `feat/ship-interior-firefly` | `--world ship-interior` rebuilt as a Firefly cockpit |
| `feat/mech-pilot` | Rideable heavy-mech pilot controller + `--test-mech` + `--world mech` |

---

## 🤝 Companion / co-op AI

| Branch | Feature |
|---|---|
| `feat/companion-ai` | Companion squad — visible companion + threat models in showcase |
| `feat/companion-controller` | Single-companion facade (Slice C seam) |
| `feat/coop-companion-merge` | Co-op companion merge — spec locked, Wave-1 incoming |
| `feat/npc-controller` | NPCSystem controller for non-combatant world NPCs |
| `origin/feat/coop-npcs` | Co-op NPC integration — INavGrid + allyStateName fixes |

---

## 👾 Enemies · canon-aliens · monsters

| Branch | Feature |
|---|---|
| `feat/enemy-artpass` | Wire the real Synth character as `blue_synth_seed1.glb` (18.6→8.6 MB) |
| `origin/feat/canon-aliens` | Port the four "most reported" species into `MonsterSystem::Tuning` |
| `origin/feat/canon-aliens-warlord-adaptive` | Light up Adaptive-Hide on the Saurian Warlord row |
| `origin/feat/adaptive-hide` | Adaptive-Hide engine extension (rotate damage-type resist) |
| `origin/feat/act2-desert-warlord` | Saurian Warlord boss wired into L10 (gated arena) |
| `origin/feat/act2-desert-mantis-ambush` | Mantis Arbiter wildcard ambush gated by side-quest |
| `origin/feat/act2-desert-grey-patrol` | L10 patrol's ranged unit swapped to canon GreyTasked drone |

---

## 🗺️ World / levels

| Branch | Feature · Status |
|---|---|
| `feat/openworld` | Open-world — gated (Release+Debug, 0 VUID, alloc=0) · **READY FOR INTEGRATION** |
| `feat/floors2-7-dims` | Floors 2–7 dims · **READY FOR INTEGRATION** (46/46 gate) |
| `feat/act2-world` | Act-2 world · **READY FOR INTEGRATION** (44/44 gate) |
| `origin/feat/act2-caves` | Act-2 caves (DJBOOTH) · gate PASSED |
| `origin/feat/act2-desert` | Act-2 desert base world |
| `origin/feat/act2-desert-nordic-mentor` | Nordic Steward mentor seated in the L11 Salvari camp |
| `origin/feat/act2-roster` | Act-2 roster + env install |
| `origin/feat/14900k-content` | Club 1127 + flooded cave/tunnel network (`--world club`) |
| `feat/world-blueprint` | Resolve the 4 open design decisions (Tim, 2026-05-23) |
| `feat/spire-cull` | Game-wide per-floor occlusion cull for the hand-coded Spire |
| `origin/feat/door-code-keypad` | Functional doors + 7-story elevator + gun aim |

---

## 💎 Glass · materials · holo-terminals

| Branch | Feature |
|---|---|
| `feat/glass-shiny-transparent` | Glass rework — UE5-style shiny + transparent (Filament Cook-Torrance) |
| `feat/glass-lounge-sit` | Glass lounge (clear plate-glass table + chairs) + sit-at-chair interaction |
| `feat/holo-terminal-kiosks` | Clear-glass HoloTerminal redo + placeable kiosk system + 6 B1 terminals |
| `origin/feat/holoterm-hologram` | Cell terminal as an actual glowing blue hologram |

---

## 🔫 Weapons

| Branch | Feature |
|---|---|
| `feat/weapons-artpass` | Add Railgun (#10) — high-power precision slug |
| `feat/weapons-artpass-rebased` | Railgun #10, rebased variant |
| `origin/feat/weapon-real-models` | Real PBR-textured viewmodels for all 7 FPS weapons |
| `origin/feat/weapon-sfx-fx` | Distinct per-weapon fire SFX + muzzle/impact FX |
| `origin/feat/weapon-damage-types` | Stamp `DamageType` on every `WeaponDef` + `ResolvedFire` shot |

---

## 🔥 Damage-type thread (Jun 1 sweep)

A coordinated pass threading `DamageType` through every `onFire` dispatcher.

| Branch | Feature |
|---|---|
| `origin/feat/weapon-damage-types` | Source: `DamageType` on weapon defs + resolved shots |
| `origin/feat/main-fire-damage-types` | `Level1Game::onFire` + player fire dispatch |
| `origin/feat/spire-fire-damage-types` | All four Spire `onFire` dispatchers (folded into `cull-combined`) |
| `origin/feat/act2-desert-fire-damage-types` | `Act2Desert::onFire` |
| `origin/feat/canon-play-fire-damage-types` | `CanonPlay::onFire` + main dispatch |

---

## ⚙️ Rendering / engine infrastructure

| Branch | Feature |
|---|---|
| `feat/cull-combined` | **(current)** Combined cull chain — 7 branches collapsed into one |
| `feat/frustum-cull` | CPU per-object camera-frustum (POV) culling |
| `feat/headless-capture-fix` | Fix headless capture grabbing only clear color + pixel-variance assertion |
| `feat/smoketest-fix` | Mirror live shutdown teardown before `physics->shutdown` |
| `feat/versioning` | Build-time `0.3.<commit-count>` version system |
| `feat/version-bump-0.4` | 0.4 release — controllers + asset wiring batch (8 GLBs) |
| `feat/wire-new-glbs` | Wire 8 new rigged GLBs into the engine |
| `origin/feat/multi-font-roles` | Role-based multi-font HUD/UI system (proportional) |
| `origin/worktree-agent-adc96d327cb9718fd` | Hardware ray-query RT ambient occlusion (BLAS/TLAS + inline AO) |
| `origin/worktree-agent-af060330558559680` | Skinned death ragdoll wired into monster death |

---

## 🌀 Portal hub · swim · other gameplay

| Branch | Feature |
|---|---|
| `feat/portal-hub-polish` | Rift hub — blue energy core + wormhole-generator housing + eerie audio |
| `feat/portal-hub-polished` | Port + polish DJBOOTH's portal hub onto `cull-combined` |
| `feat/swim-controller` | Act-4 undersea SwimController + `--test-swim` (8/8) + `--world swim` |
| `feat/healthbar-polish` | Healthbars — thin + faded + LOS-occluded + on every enemy |
| `fix/upper-floor-ai` | Stop cross-floor enemies from shooting through the level |

---

## 📝 Docs · fleet · specs

| Branch | Feature |
|---|---|
| `origin/docs/clean-room-license-correction` | Correct license posture — original clean-room work, not an RBDOOM fork |
| `origin/feat/level-editor` | Seed native Level Editor lane — FP walkaround + AI asset suggestions |
| `origin/specs/jk-idtech8-enhance` | Fold "beyond idTech 8" features into the roadmap |
| `origin/feat/fleet-messaging-design` | `fleet_image.py` — post images to fleet chat |
| `origin/docs/fleet-localllm` | Local-LLM via LM Studio router guide |
| `origin/feat/rotor-spin` | Procedural rotor/propeller spin prototype (RotorSpin) |
| `origin/feat/wave3-content` | Procedural creature locomotion baker (Verthani) |

*(Plus ~7 more `docs/*` and `note-to-*` fleet-coordination branches.)*

---

## 🔗 Integration / merge branches

| Branch | Role |
|---|---|
| `main` | Default — last merge PR #8 (integration/culling-glass); 3 behind some feature work |
| `origin/integration/culling-glass` | Culling + glass integration line |
| `integration/14900k-test` | 14900K integration test line |
| `origin/integration/main-plus-session` | main + session unify (physics constraint API) |
| `merge/cull-doors-death-anim` | Merge `doors-death-anim` onto the cull line |

---

## 🧪 Ephemeral agent worktrees

~16 `worktree-agent-*` branches exist as parallel-agent scratch workspaces
(glass M1, RT AO, gibs, nameplates/minimap, TTF font, secret trapdoor room,
nexus ragdoll teardown fix, etc.). Most are already merged into the integration
lines above and are not tracked here as standalone features.

---

*Report generated by surveying all local and `origin/*` refs. Feature labels are
the latest commit subject per branch; "READY FOR INTEGRATION" tags are the
branch authors' own gate status, not re-verified here.*
