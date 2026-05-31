# EFLZ — Master Task List (2026-05-31)

> **Single source of truth for "what's next" on Escape From Lab Zero.**
>
> Consolidated 2026-05-31 from the overnight 7-agent corpus digest +
> Tim's wife's feature notes + reconciliation against the current
> engine state. Replaces ad-hoc TODO files scattered across `docs/`
> and the (mostly empty) GitHub issues list.
>
> **Read order if you only have time for one doc:** start here, then
> drill into the specific spec for whatever task you pick up.

---

## How to read this doc

Every task below has:

- **ID** — stable handle used to reference it in commits and GitHub issues.
- **Title** — what the task delivers.
- **Source doc(s)** — the design spec(s) under `docs/design/` that define the work.
- **Engine files** — concrete paths to add/extend.
- **Test gate** — `--test-*` name (or `--world *` for visual gates).
- **Effort** — rough order of magnitude (light = <1 wk · medium = 1-3 wk · heavy = 3+ wk).
- **Blocked by** — tasks that must land first.
- **Why it matters** — one-sentence rationale.

Tasks are grouped by **phase** (the recommended landing order). Within a phase, tasks are independently shippable.

---

## Currently shipped (DON'T re-do)

Before picking any task, know what's already on `feat/cull-combined @ 3f2c3b1`:

- **Engine baseline:** Vulkan render device, bindless textures, PBR slice 1+2 (mesh.frag normal-map + GGX metal-roughness), glass material (drawMeshGlass + transparent pass + glass.frag), shadow + sky + water + SSAO + SSGI + bloom + HDR.
- **Act 1 spine:** level1.{h,cpp} (B1 detention + Martinez), spire_mid.{h,cpp} (F3-F5 generic escalation — needs identity rework per gap docs), spire_top.{h,cpp} (F6+F7 + Jake's Clone boss), elevator.{h,cpp}, F2 rescue.{h,cpp} (Aria/Keisha/Emily timed triage), spire_nexus.{h,cpp} (Floor 4.5 Chorus / MultiPodBoss), spire_sublevels.{h,cpp} (F7 hidden descent — Waste Disposal / Cryo Storage / Enhanced Interrogation).
- **Act 2:** act2_world.{h,cpp} (L8-L9), act2_desert.{h,cpp} + canon_aliens.{h,cpp} (L10-L11 with Saurian Warlord + Grey Patrol + Nordic Mentor + Mantis Arbiter), act2_caves.{h,cpp} (L12-L15 per the portal-hub polish — Memory Hunter, toxic swamp, Siren ambush).
- **Act 3 substrate:** space subsystems S0-S12 in app/space/ (SpaceLayer, SpaceLOD, SpaceEnv, WormholeVfx, WormholeTransit, Descent, ShipInterior, ShipAI, Targeting, ShipDamage, EVA, ShipWindows, ShipRepair, ShipArt) — built on spec, NO level content uses them yet.
- **Intro VFX:** decloak shimmer + tractor beam (just landed for the intro capstone Task #42).
- **Companion AI:** companion_controller.{h,cpp} (LLM-driven two-brain — reflex + cognitive), ally.{h,cpp}, ally_ai.{h,cpp}, companion_squad.{h,cpp}.
- **Misc shipped:** doors-death-anim (mostly merged via cull-combined; full pull still pending), thirdperson, weapons, monster system, npc dialog (basic), terrain (canon), ocean/water, save, timeline.

**Anti-list (do NOT spend cycles porting these):** Unity object pool, Unity LOD manager, NUnit, Steam deploy, anything from `Q3Engine/src/` that's pure Babylon UI — these are noted in the gap docs for completeness but are NOT engine work.

---

## Open owner decisions (block multiple tasks)

These need Tim's explicit go/no-go before the tasks downstream can proceed:

| # | Decision | Source | Blocks |
|---|---|---|---|
| D1 | **Promote `feat/cull-combined` → `main`** (currently 121+ ahead, strict ff-ready) | This session | Everything (the integration spine) |
| D2 | **Bio-Integration Lab naming + floor placement** (F4 cybernetics? a new room?) | EFLZ_TECH_SYSTEMS §2 | T-1, T-2, T-7 (Humanity meter UI) |
| D3 | **Drone repair/recharge mechanics** (1-credit/HP loop suggested — net-new) | EFLZ_TECH_SYSTEMS §5 | T-9 drone economy MVP |
| D4 | **Player vs companion inventory** (single shared vs split-per-character) | EFLZ_CRAFTING_INVENTORY §2 | T-8 inventory MVP |
| D5 | **Memory Hunter level placement** (A2 L12 / A3 L30 / both?) | EFLZ_BESTIARY_RECONCILE §6 | T-13 act3 mid |
| D6 | **6th landing zone for L37** (corpus has 5; Oceania/Sydney suggested) | EFLZ_ACTS_2_4_GAPS §4 | T-23 Act 4 land |
| D7 | **AP system: turn-based or real-time-with-cooldowns?** | EFLZ_ACTS_2_4_GAPS §3 | T-14 Storm Runner Bridge |
| D8 | **Endings 8 / 11 / 12 full screenplays** | EFLZ_DIALOGUE_CATALOG §7 + EFLZ_ACTS_2_4_GAPS §5 | T-26 ending selector |
| D9 | **Live AI banter (post-launch?) or shipped?** | EFLZ_DIALOGUE_CATALOG §6 | T-12 dialogue runtime |
| D10 | **Music production track** — all 65 tracks are unmade; outsource? in-house? | EFLZ_SIDE_QUESTS_ACHIEVEMENTS §4 | T-30 audio scheduler |

---

## PHASE 0 — Test + promote `feat/cull-combined`

Highest priority. Until this lands on `main`, every downstream task starts from a non-canonical baseline.

| ID | Title | Source | Effort | Blocked by |
|---|---|---|---|---|
| T-0 | **Tim playtests `feat/cull-combined`** — visual PBR check, glass screens, intro VFX (decloak + tractor), Act-2 desert chain, holo terminals | This session | <1 day | D1 |
| T-PROMOTE | **Promote `feat/cull-combined` → `main`** (strict ff) | This session | minutes | T-0 |

**Notes:** if T-0 catches issues, fix them on `feat/cull-combined` and re-test before promoting. The 121 commits include landscape changes (PBR, glass, intro VFX, Act-2 desert) that need real-eye verification.

---

## PHASE 1 — Wife's feature pack (highest-ROI gameplay additions)

Tim's wife dropped 6 features the night of 2026-05-31. These are pure ROI — every one is a memorable gameplay moment players will brag about. Spec: `EFLZ_FEATURE_PACK_2026-05-31.md`.

| ID | Title | Engine files | Test gate | Effort |
|---|---|---|---|---|
| T-WEATHER | **Weather system + sliding + lightning** — rain/clouds/storms cycle per biome; wet ground = sliding for monsters AND player; 9% per-period lightning strike chance on outdoor non-covered entities (instant-kill enemies, heavy damage to player) | weather.{h,cpp} (audit existing — may be stub), player.cpp (ground friction), monster.cpp (slip tolerance), fx.cpp (LightningStrike VFX), scene.h (Room outdoor flag) | `--test-weather` + `--world weather` | medium (1-2 wk) |
| T-GAS-BLAST | **Gas station + 1.5-camera-width explosion** — gas stations with pumps + mini-mart + supply tanks; tank ignition triggers ~40 m radius blast killing everything unsealed in range; 1.5-sec telegraph | gas_station.{h,cpp}, barrels.cpp (cascade), fx.cpp (blast VFX) | `--test-gasstation` + `--world gasstation` | medium (~1 wk) |
| T-VEHICLES | **Drivable vehicles + Sarah's garage hack unlock** — sedan/armored truck/Salvari hover-buggy; mount/dismount; arcade physics; damage→smoke→explode; UNLOCKED via Sarah hacking an underground mountain base garage terminal | vehicle.{h,cpp} (audit existing), garage.{h,cpp}, save.h (unlock flag) | `--test-vehicle` + `--world vehicle` | heavy (2-3 wk) |
| T-EVIL-SARAH | **Evil Sarah Clone** — 1100 HP hacker-mirror boss with 3 phases (Interface / Override / Reprogram); 3 timeline manifestations (Omega/Alpha/Beta); reuses MultiPodBoss + Sarah's rig | monster.{h,cpp} (new row), spire_top.cpp pattern, dialogue scripts | `--test-evilsarah` | light-medium (~1 wk) |
| T-TREES | **Earth trees + autumn seasonal color** — Planet Earth biome only; 4 varieties (oak/pine/maple/birch); GPU-instanced; TimeOfYear drives leaf tint; reuses placeOnTerrain + just-landed PBR | trees.{h,cpp}, leaves shader (alpha-cutout), terrain.h anchor API | `--test-trees` + `--world trees` | medium (1-2 wk) |
| T-FITNESS | **Fitness Center room** — pre-escape mundane gym; L45 return-visit horror beat; treadmills/weights/lockers/mirror wall; dumbbell melee weapon | level data (F4 cybernetics? F1?), new mesh prims for fitness equipment, glass mirror (reuses just-landed glass) | `--world fitness` | light (2-4 days) |

**Phase 1 total effort:** ~6-10 weeks single-developer; can be parallelized across the fleet.

**Phase 1 ROI ranking** (best first): T-WEATHER, T-GAS-BLAST, T-VEHICLES, T-EVIL-SARAH, T-TREES, T-FITNESS.

---

## PHASE 2 — Core gameplay systems (the load-bearing infrastructure)

These systems unlock huge swaths of downstream content. Every subsequent feature assumes most of these exist.

| ID | Title | Source | Engine files | Test gate | Effort | Blocked by |
|---|---|---|---|---|---|---|
| T-1 | **Bio-Integration Enhancement Lab MVP** — 6 stations + Humanity meter wired through to interactions | EFLZ_TECH_SYSTEMS §2 | bio_lab.{h,cpp} (NEW) or augmentation.{h,cpp}, humanity.{h,cpp}, _CyberneticBlend shader uniform | `--test-bio` + `--world bio` | medium-heavy | D2 |
| T-2 | **Humanity meter** — 6-state bucket runtime + thresholds (Full/High/Mid/Low/Critical/Lost) + save plumbing | EFLZ_TECH_SYSTEMS §3 | humanity.{h,cpp}, save.h | `--test-humanity` | light | T-1 |
| T-3 | **Augmentation system** — 6 aug types + install scene + cyber-blend shader + history log | EFLZ_TECH_SYSTEMS §3 | augmentation.{h,cpp}, shaders/mesh.frag (extend) | `--test-augment` | medium | T-1, T-2 |
| T-4 | **Infection plumbing** — 5-stage timer for player + per-NPC rescue; cure success matrix | EFLZ_TECH_SYSTEMS §4 | infection.{h,cpp}, rescue.{h,cpp} (extend) | `--test-infection` | medium | T-8 (inventory for cure items) |
| T-5 | **F7 Sarah outcome bands** — 9+/6-9/2-6/0-2 minute timer + branching dialogue | EFLZ_TECH_SYSTEMS §4 + EFLZ_DIALOGUE_CATALOG §2 | spire_top.cpp (extend), dialogue runtime | `--test-sarahoutcome` | medium | T-12 |
| T-6 | **Cure quest 5-stage pipeline** — C1 DNA / C2 Salvari base / C3 xenoflora / C4 temple / C5 application | EFLZ_CRAFTING_INVENTORY §5 | quest plumbing, inventory plumbing | `--test-cure` | medium | T-8, T-10 |
| T-7 | **Cybernetic blend shader uniform** — drives visible body changes per aug; P0 shader | EFLZ_TECH_SYSTEMS §7 | mesh.frag, ObjectData struct extension | (visual via T-1 gate) | light | (none, but T-1 consumes it) |
| T-8 | **Inventory system MVP** — 40-slot grid + 8-slot hotbar + tabbed UI + weight + stash | EFLZ_CRAFTING_INVENTORY §2 | engine/inventory (NEW), app/ui_inventory.{h,cpp} | `--test-inventory` | medium | D4 |
| T-9 | **Crafting system MVP** — 5 station tiers + recipe DB + 150 recipes | EFLZ_CRAFTING_INVENTORY §3-4 | engine/crafting (NEW), app/ui_crafting.{h,cpp}, recipes.json | `--test-crafting` | medium-heavy | T-8 |
| T-10 | **Consumables runtime** — effect application + per-consumable behavior (heal, buff, ammo) | EFLZ_CRAFTING_INVENTORY §6-7 | engine/consumables (NEW) | `--test-consumables` | light-medium | T-8 |
| T-11 | **Pickup component refactor** — subsume weapon.h pickup into a shared PickupComponent | EFLZ_CRAFTING_INVENTORY §2 | engine/inventory, weapon.{h,cpp} | `--test-pickup` | light | T-8 |
| T-12 | **Dialogue tree runtime** — `.dlg` line-based format + evaluator + save integration | EFLZ_DIALOGUE_CATALOG §8 | dialogue.{h,cpp} (NEW), npc_dialog.{h,cpp} (extend), .dlg file format spec | `--test-dialogue` | medium-heavy | D9 |
| T-12a | **Companion live-AI banter (Layer E)** — Grok for women, Claude for men, prompt-cached, off-tick | EFLZ_DIALOGUE_CATALOG §6 + docs/superpowers/specs/2026-05-26-companion-ai-design.md | companion_controller.{h,cpp} (extend) | `--test-livebanter` | heavy | T-12, D9 |

---

## PHASE 3 — Skill trees + economy

Per the EFLZ_SKILL_TREES spec, this is best done as a 7-sub-phase rollout. Sub-phases land independently.

| ID | Title | Source | Engine files | Test gate | Effort |
|---|---|---|---|---|---|
| T-SKILL-A | **Skill data registry** — JSON schema for 200 skills; loader; static validation | EFLZ_SKILL_TREES §9 | engine/skill_data.{h,cpp} (NEW) | `--test-skilldata` | light |
| T-SKILL-B | **Skill state runtime** — per-character runtime + save plumbing | EFLZ_SKILL_TREES §9 | engine/skill_state.{h,cpp} | `--test-skillstate` | light-medium |
| T-SKILL-C | **XP + skill points** — earn + spend + respec | EFLZ_SKILL_TREES §7 | engine/xp_system.{h,cpp} | `--test-xp` | light |
| T-SKILL-D | **Skill UI** — tree visualization + spend interaction | EFLZ_SKILL_TREES §8 | app/ui_skills.{h,cpp} | `--world skill` | medium |
| T-SKILL-E | **Skill effects executor** — apply per-skill modifiers (passive stats + active abilities) | EFLZ_SKILL_TREES §9 | engine/skill_executor.{h,cpp}, engine/skill_effects.{h,cpp} | `--test-skilleffects` | medium |
| T-SKILL-F | **Jake's 3 trees full data** — Brute Force / Tactical / Survivor | EFLZ_SKILL_TREES §2 | data only (skills.json) | (via T-SKILL-D) | light |
| T-SKILL-G | **Sarah + Emily + K'thara + support full data** — 17 trees / 200 skills | EFLZ_SKILL_TREES §3-7 | data only | (via T-SKILL-D) | medium |

---

## PHASE 4 — Quest / achievement / audio runtime

Per EFLZ_SIDE_QUESTS_ACHIEVEMENTS. 6 phases (E0-E5) shippable in order.

| ID | Title | Source | Engine files | Test gate | Effort |
|---|---|---|---|---|---|
| T-QUEST-A | **Quest data + state machine** — LOCKED/AVAILABLE/ACTIVE/RESOLVING/COMPLETE/FAILED | §5 | engine/quest.{h,cpp}, engine/questdb.{h,cpp} | `--test-quest` | light-medium |
| T-QUEST-B | **Karma counter + thresholds** | §5 | engine/karma.{h,cpp} | `--test-karma` | light |
| T-QUEST-C | **Achievement system** — 200 achievements + tracker + unlock notifications | §3 | engine/achievement.{h,cpp}, app/ach_toast.{h,cpp}, engine/stats.{h,cpp} | `--test-achievements` | medium |
| T-QUEST-D | **Quest log UI + minimap markers** | §6 | app/quest_log.{h,cpp}, app/quest_marker.{h,cpp} | `--world questlog` | medium |
| T-QUEST-E | **Audio scheduler** — adaptive music + ambient bed swaps + scheduled cues | §4 | engine/audio_sched.{h,cpp}, cues.{h,cpp} (extend) | `--test-audiosched` | medium |
| T-QUEST-F | **Music + SFX production** — 65 tracks + 525 SFX + 55 ambient loops — currently ALL missing | §4 | assets/audio/* | (artist QA) | **heavy** (multi-month) | D10 |

---

## PHASE 5 — Bestiary completion (fix the "generic graybox" feel)

Per EFLZ_BESTIARY_RECONCILE. 5 waves (A-E).

| ID | Title | Source | Engine files | Test gate | Effort |
|---|---|---|---|---|---|
| T-MON-A | **Wave A — wire what is half-shipped** — Chen cure choice, FE#7 Memory Flash, Overseer F6 placement, Warlord Adaptive-Hide | §7 | monster.{h,cpp} (extend rows), spire_top.cpp, spire_mid.cpp | `--test-bosses` | light |
| T-MON-B | **Wave B — data-only adds** — Jake's Clone full reconstitution, Toxic Hulk, Vex | §7 | monster.{h,cpp} (new rows) | (extend `--test-bosses`) | light |
| T-MON-C | **Wave C — scripted multi-pod bosses** — Swarm Controller avatar, Siren parasites, Breeder Queen summons, Garrison mech-suit | §7 | monster.{h,cpp} + per-boss scripted controllers | per-boss `--test-*` | medium |
| T-MON-D | **Wave D — non-combat-phase primitive** — Memory Hunter (cannot-attack-with-bullets), Sarah's Clone, Karen Mitchell | §7 + EFLZ_FEATURE_PACK §3 | monster.{h,cpp}, dialogue plumbing | `--test-memhunter` + `--test-evilsarah` | medium |
| T-MON-E | **Wave E — Act 3/4 regulars + 5 Proto-Overlords + final Overlord** | §7 | monster.{h,cpp} (20+ new rows), endgame scripted phases | per-boss `--test-*` | **heavy** |

---

## PHASE 6 — Act 2 finish + Act 3 + Act 4 (the level content)

Per EFLZ_ACTS_2_4_GAPS. 6 phases (A-F).

| ID | Title | Source | Engine files | Effort |
|---|---|---|---|---|
| T-13 | **Finish Act 2 L16-L20** — Ruined Metropolis trilogy + Spaceport finale + vehicle_combat module | §2 | act2_metropolis.{h,cpp}, act2_spaceport.{h,cpp}, vehicle_combat.{h,cpp} | heavy |
| T-14 | **Storm Runner ship hub (Act 3 spine)** — 4-deck hub, Bridge mission-board, AP system | §3 | storm_runner.{h,cpp}, ap.{h,cpp}, app/space/* (extend) | heavy |
| T-15 | **Act 3 opener L21-L25** — Departure, Asteroid Field, Salvari Prime trilogy | §3 | act3_*.{h,cpp} per level | heavy |
| T-16 | **Act 3 mid L26-L33** — Mining Colony, Casino, Rebel Base trilogy + Crystal Heart Installation | §3 | act3_*.{h,cpp} | heavy |
| T-17 | **Act 3 finale L34-L35** — Storm Runner romance + Sol arrival | §3 | act3_finale.{h,cpp} | medium |
| T-18-T22 | **Act 4 main path L36-L48** — Orbital, atmospheric, base, regional liberation, NYC/London/Tokyo, RTS module, Mothership | §4 | act4_*.{h,cpp} per level + rts.{h,cpp} | **multi-month** |
| T-23 | **Act 4 landing zone selector (6 zones)** | §4 | landing.{h,cpp} | medium | D6 |
| T-24 | **L45 Return to Lab Zero (mutation overlay)** — reuses Act 1 spire with mutation pass | §4 | spire_mutated.{h,cpp} | medium |
| T-25 | **L50 4-phase Overlord finale** — reality distortion + alt-Jake mirror-AI + 4-choice fork | §4 | act4_finale.{h,cpp} | heavy |
| T-26 | **Ending selector + 12 cinematics** | §5 | endings.{h,cpp} + cinematic playback module | heavy | D8 |

---

## PHASE 7 — Polish + endgame

| ID | Title | Source | Effort |
|---|---|---|---|
| T-27 | **Intro capstone (Task #42)** — Jake's fighter → Jupiter → G6 decloak → tractor beam → "6 Months Later" → cell | This session + EFLZ_DIALOGUE_CATALOG | medium |
| T-28 | **Promote `feat/cull-combined` → `main` then full-spire smoketest** | This session | day |
| T-29 | **New Game Plus modes** — Nightmare / Savior Run / Clone Mode / Sarah's Story / Salvari Memories | bible §New Game Plus | heavy |
| T-30 | **Mature-content toggle + content warnings** — option to reduce graphic content while keeping story | bible §Content Warnings | medium |

---

## Source-doc index (committed this session)

Every task above references one or more of these specs. Find them under `docs/design/`:

| Spec | Lines | Topic |
|---|---|---|
| `EFLZ_FEATURE_PACK_2026-05-31.md` | 540 | Wife's notes — weather/storms/vehicles/clones/trees/fitness |
| `EFLZ_TECH_SYSTEMS.md` | 767 | Bio-Integration Lab + Humanity + augments + infection + drone economy + shader gaps |
| `EFLZ_SKILL_TREES.md` | 781 | 10 chars / 17 trees / 200 skills + economy + UI |
| `EFLZ_CRAFTING_INVENTORY.md` | 1036 | 40-slot grid + 150 recipes + cure quest + gas-station consumables |
| `EFLZ_DIALOGUE_CATALOG.md` | 865 | 60+ scenes + 5 romance FSMs + 15 boss-taunt sets + `.dlg` runtime spec |
| `EFLZ_BESTIARY_RECONCILE.md` | 671 | 78 enemies / 22 shipped / Evil Sarah Clone full spec |
| `EFLZ_SIDE_QUESTS_ACHIEVEMENTS.md` | 990 | 100 quests + 200 achievements + audio |
| `EFLZ_ACTS_2_4_GAPS.md` | 619 | Acts 2/3/4 reconcile + 12 endings matrix + 28-module build plan |

Plus the pre-existing in-repo design corpus (not new this session but referenced throughout):
- `EFLZ_WORLD_STRUCTURE.md` (37 KB) — canonical world/level structure digest
- `EFLZ_BESTIARY.md` (28 KB) — original bestiary (kept; reconcile lives separately)
- `EFLZ_NARRATIVE.md` (24 KB) — narrative spine
- `X3_WORLD_BLUEPRINT.md` (22 KB) — world blueprint
- `MASTER_GAME_PLAN.md` (5 KB) — old plan; this doc supersedes for ordering

---

## What to do RIGHT AFTER Tim wakes up

1. **Read this doc** (you're doing it now). Scan the open-decisions table (`D1` through `D10`).
2. **Resolve D1 first** — playtest `feat/cull-combined`, then promote to `main`. Everything downstream restarts from `main`.
3. **Pick ONE task from Phase 1** to scope tonight — likely T-WEATHER (highest ROI) or T-FITNESS (lightest effort + immediate gratification). Drop the rest of Phase 1 into GitHub issues for fleet pickup.
4. **Skim Phase 2** to decide on **D2-D4** (Bio-Lab placement, drone repair mechanics, inventory split) — these gate the next wave of work.
5. **Don't read the full corpus docs cold** — use this list as the index, drill in only for the specific task you're working on.

---

## Recommended fleet allocation

If the multi-machine fleet is back to coordinated work:

| Machine | Lane | Suggested tasks |
|---|---|---|
| 13700K (clean-room integrator — me) | Engine work + integration | T-WEATHER, T-FITNESS, T-EVIL-SARAH, then T-8 inventory + T-9 crafting |
| 14900K + 5090 (gameplay + content) | Act-1 floor identities + Act-2 finish | T-MON-A through T-MON-D, T-13 Act 2 metropolis/spaceport |
| DJBOOTH (1080 Ti) | Polish + small features | T-FITNESS, T-TREES, T-EVIL-SARAH |
| Snake (right-screen, 13700K + own 1080 Ti) | Act 3 ship hub + space content | T-14 Storm Runner, T-15 Act 3 opener |
| i5000 / i4400 / additional workers | Bestiary completion + skill tree data | T-MON-B/E, T-SKILL-F/G |

Lane independence is enforced by the existing `app/*` module boundaries; everyone pushes `feat/*` branches; 13700K integrates onto `main` after each gate.

---

## What's deliberately NOT in this task list

Out of scope for this consolidation pass:
- Tooling (build scripts, asset converters — handled separately)
- Engine R&D (RT path tracing, GI improvements — separate spec)
- The 14900K's in-flight `doors-death-anim` branch (full pull comes when the 14900K signals stable; this session surgically cherry-picked just the PBR slices)
- Marketing/distribution/store-page work
- Localization (NC-17 content + multi-language is a separate planning problem)

---

— Master task list captured 2026-05-31 by the 13700K (clean-room engine rig).
Consolidates the wife's overnight feature notes + 7-agent corpus digest into one
canonical "what's next" reference. Living doc — update as tasks land or scope shifts.
