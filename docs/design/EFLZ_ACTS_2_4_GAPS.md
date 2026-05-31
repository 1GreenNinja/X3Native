# EFLZ — Acts 2/3/4 Reconciliation (Engine Gaps)
> Synthesized 2026-05-31 from TASK_3 + TASK_4 + TASK_5 (all 3 parts) + Level_*.json data files (~90 KB total).
> Authority: Tim's design corpus. **No code changes; design doc only.**

---

## 1. Overview + Sources

This document reconciles the EFLZ (Escape From Lab Zero) design corpus for Acts 2–4 (Levels 8–50, "Surface Emergence" through "The Overlord Core") against what X3Native has already shipped in `app/act2_*` and `app/space/`. The goal is to make the engine-side gaps obvious: what's authored in design but absent in code, what's coded but under-utilized, and what build sequence finishes Acts 2–4 plus the 12-ending finale.

### Source corpus (all read in full)

| File | Bytes | Role |
|------|-------|------|
| `G:\Unity_Projects\PARALLEL_TASKS_8_CHATS\TASK_3_LEVELS_8-20_OPENWORLD.md` | ~8.7 KB | Act 2 master spec — L8 to L20 outline |
| `G:\Unity_Projects\PARALLEL_TASKS_8_CHATS\TASK_4_LEVELS_21-35_SPACE.md` | ~9.5 KB | Act 3 master spec — Storm Runner hub, departure → Sol arrival |
| `G:\Unity_Projects\PARALLEL_TASKS_8_CHATS\TASK_5_ACT4_EXPANDED_PRODUCTION_GUIDE.md` | ~22 KB | Act 4 production: L36/L45/L50 detailed mechanics, boss phases, choice graph |
| `G:\Unity_Projects\PARALLEL_TASKS_8_CHATS\TASK_5_ESCAPE_LAB_48_ACT4_LEVELS_36-50_COMPLETE.md` | ~27 KB | Act 4 complete: every L36–L50, AP system, 12 endings full requirements matrix |
| `G:\Unity_Projects\PARALLEL_TASKS_8_CHATS\TASK_5_LEVELS_36-50_EARTH.md` | ~11 KB | Act 4 outline (the "still needed" preview that TASK_5_ESCAPE_LAB_48 expanded) |
| `Level_08_SurfaceEmergence.json` | full level data |
| `Level_09_CrystallineDesertEdge.json` | full level data |
| `Level_10_CrystallineDesertDepths.json` | full level data |
| `Level_12_AdvancedCaveSystem.json` | full level data (major level) |
| `Level_15_TreeCities.json` | full level data |
| `Level_17_Downtown.json` | full level data |
| `Level_19_SpaceportAssault.json` | full level data |
| `Level_20_EscapeFromKethzarPrime.json` | full level data (Act 2 finale) |

### Shipped engine code (cross-referenced)

| File / Module | What it owns | Header path |
|---------------|--------------|-------------|
| Act 2 host + L8/L9 | `Act2World` — alien-surface terrain, L8 lab-exit gauntlet, L9 desert + hazard | `app/act2_world.h` |
| Act 2 desert | `Act2Desert` — L10/L11, Saurian Warlord boss, Mantis Arbiter wildcard, Nordic mentor, side-quest interact | `app/act2_desert.h` |
| Act 2 caves | `Act2Caves` — L12/L13/L14/L15: Memory Hunter, Crystal Heart Chamber (dual-gated), poison hazard, Siren ambush timeline-gate, Tree-City platforms | `app/act2_caves.h` |
| Canon-aliens roster | `canon_aliens.{h,cpp}` — Saurian (×2), Grey, Nordic, Mantis | `app/canon_aliens.h` |
| Act 3 space engine | S0–S12, listed in §3.2 below | `app/space/*.h`, `app/space_pilot.h` |

> All space subsystems live in `feat/cull-combined`-derived branches; they implement S0 SpaceLayer (spine), S1 SpaceEnv (deep-space backdrop), S2 LOD, S3 WormholeTransit, S4 AtmoDescent, S5 ShipInterior, S6 ShipWindows, S7 ShipRepair, S8 ShipAi (dogfight enemies), S9 Targeting (radar/lock-on), S10 ShipDamage (shield+hull+subsystems), S11 ShipAnim (node-transform), S12 EVA spacewalk.

### Authoring conventions to preserve

- Module lanes own a level range (`act2_world` = L8/L9, `act2_desert` = L10/L11, `act2_caves` = L12–L15). New modules below pick up where these leave off (`act2_metropolis` = L16–L18, `act2_spaceport` = L19/L20, `act3_*` = L21–L35, `act4_*` = L36–L50).
- Each module exposes `build()`, `tick()`, `onTrigger()`, `draw()`, `plan()`, and a headless self-test (`runActNNXSelfTest()`).
- Trigger event IDs use non-colliding ranges: Act-1 uses 10/30/40/50, `act2_world` owns 80–82, `act2_desert` owns 90–95, `act2_caves` owns 100–108. New ranges below continue this scheme.
- Hazards (heat, sandstorm, poison, radiation): `HazardZone` pattern — AABB + tracked exposure, INERT at load, ARMS on entry or trigger.
- Boss arenas: present at build but inert until an `ArenaArm` trigger fires (`m_warlordSpawned`, `m_mantisSpawned` pattern).
- Interactables: present-at-load but inert until `onInteract()` flips them once (Crystal Heart dual-gate, upgrade station, etc.).
- Timeline branches (Alpha/Beta/Omega) read a single boolean (`m_sirenGate` style) at `build()` time; the headless self-test flips it and rebuilds to assert both halves.

---

## 2. Act 2 (L8–L20) Reconciliation

### 2.1 Per-level table

| # | Name | Biome | Boss / Major Encounter | Key Objective | Shipped? | Module |
|---|------|-------|------------------------|---------------|----------|--------|
| 8 | Surface Emergence | Lab Zero exit tunnel → Crystal valley → Bioluminescent grove → Salvari hidden outpost | None (gauntlet: 5 Pursuit Drones + 3 Infected) | Reach Emergence Point; first contact with K'thara | **YES** (`Act2World`) | `act2_world` |
| 9 | Crystalline Desert — Edge | Singing crystals, Salvari Archives surface, waystation | Overlord Sentinel guarding archive (mini) | Cross desert; download Archive data (47 worlds reveal); puzzle: crystal resonance pillars | PARTIAL — terrain + hazard shipped; **archive download flow + crystal-pillar puzzle MISSING** | `act2_world` |
| 10 | Crystalline Desert — Depths | Underground oasis (Salvari refugees) | Saurian Warlord (canon-aliens; 3-phase boss); Mantis Arbiter (side-quest-gated ambush) | First contact with Salvari refugees; help injured Salvari (interact) | **YES** (`Act2Desert`) — both bosses present + side-quest gate | `act2_desert` |
| 11 | Salvari Camp ("Refugee Haven") | Hidden cave settlement (200 survivors) | None — hub/upgrade level | Cultural exchange; alien-equipment upgrade station | **YES** (`Act2Desert`) — Nordic Steward mentor + upgrade interact | `act2_desert` |
| 12 | The Advanced Cave System ⭐ | Multi-layer: Upper caves → Mid (archives, 47 Worlds Hall, weapon blueprints) → Deep (sealed invasion portal, Crystal Heart Chamber) → Abyss (boss arena, Overlord origin chamber) | **Memory Hunter** (8000 HP, 3 phases; phase 2 summons Memory Echoes of dead companions) | Activate Crystal Heart (Jake-strength + Sarah-hack dual gate); defeat Memory Hunter; view Overlord origin revelation | PARTIAL — `Act2Caves` ships Memory Hunter + Crystal Heart dual-gate + storyBranch flag. **MISSING: multi-layer descent (Upper/Mid/Deep/Abyss as distinct rooms), Hall-of-47-Worlds memorial display, sealed-portal "dimensional bleed" VFX, Overlord-origin holographic recording, ancient-weapon blueprint download** | `act2_caves` |
| 13 | Toxic Swamplands — Edge | Mutated flora, abandoned research stations | None | Survive poison hazard; reach research station | PARTIAL — `Act2Caves` ships poison `HazardZone`. **MISSING: mutated-flora "lash" hostile, environmental-suit/mask mechanic, abandoned research-station prop set** | `act2_caves` |
| 14 | Toxic Swamplands — Research Station | Mutated scientists | **The Siren** (transformed Aria; Timeline Beta only) | Find terraforming-failure logs; save trapped survivors (optional); confront the Siren if Beta | YES — `Act2Caves` ships mutated scientists + Siren timeline-gated boss. **MISSING: terraforming-failure log audio entries, trapped-survivor rescue micro-encounter** | `act2_caves` |
| 15 | Toxic Swamplands — Tree Cities | Giant-tree canopy civilization (500m trees, 2000 Salvari) | None | Vertical traversal (climbing, zip-lines); trading post; resistance contact | PARTIAL — `Act2Caves` ships 3 platforms + trading-post prop. **MISSING: vertical traversal mechanics (climbing/zip-line), Sky Market shop, Healing Grove, Council Hall NPCs (High Elder Vex'tira), assault-defense climax wave (canon: tree cities are about to be assaulted)** | `act2_caves` |
| 16 | Ruined Metropolis — Outskirts | Alien city ruins, holographic records on loop | **Breeder Queen** (transformed Keisha; Timeline Beta territorial zone) | Scavenger encounters (neutral/hostile by choice); optional vehicle acquisition | **NOT SHIPPED** — needs new module `act2_metropolis` | NEW |
| 17 | Ruined Metropolis — Downtown ⭐ | Skyscrapers, central AI archive | **Overlord Enforcer** (regional control-node boss; 4-floor Parliament dungeon) | Infiltrate Parliament (3 approach routes: main / sewer / rooftop); destroy regional control node; download Earth-invasion data | **NOT SHIPPED** — `act2_metropolis` | NEW |
| 18 | Ruined Metropolis — Underground | Multi-species alliance resistance HQ | None — hub | War-room planning; equipment upgrades; companion bonding | **NOT SHIPPED** — `act2_metropolis` | NEW |
| 19 | Spaceport Approach | Large-scale combat, vehicle sections, anti-air | **Planetary Defense Commander** (mini-boss; 3 approach routes; Storm Runner is on Pad Gamma) | Breach spaceport perimeter; defend Storm Runner during 3 startup waves; vehicle combat | **NOT SHIPPED** — `act2_spaceport` | NEW |
| 20 | The Spaceport ⭐ (Act 2 finale) | Spaceport interior + orbital ascent | **Planetary Garrison Commander** (3-phase: ground / mech / orbital-strike escape) + **Overlord Adjutant** mini-boss in final wave | Defense decision (Salvari-defend / Jake-defends / coordinated retreat); 3-phase wave defense; 60-sec boarding sequence; turret-defense ascent | **NOT SHIPPED** — `act2_spaceport` | NEW |

### 2.2 Already-shipped Act 2 inventory

`Act2World` (`act2_world.h`):
- Procedural alien-planet TerrainStreamer + violet binary-sun sky.
- L8 gauntlet (3 InfectedSoldier melee + 5 PursuitDrone ranged, both from existing roster, mapped onto BlueSynth / DominionTrooper profiles).
- L8 Emergence Point: 4 companion markers (Sarah/Aria/Keisha/Emily, allied roster + `convertToAllied()`).
- L9 desert: 6 emissive crystal-formation Scene props, 2 neutral fauna placeholders, a heat/sandstorm `HazardZone` (AABB + tracked exposure, inert at load).
- L8→L9 transition trigger reachability assertion.

`Act2Desert` (`act2_desert.h`):
- L10 first-contact Salvari (3 contacts incl. one injured) + a light Overlord patrol (3 hostile).
- L10 **Saurian Warlord** boss (canon-aliens; 540 HP, Boss-typed, 3 phases + memory-flash window aligned to Adaptive-Hide rhythm; present-at-build but gated by `m_warlordSpawned` until the `L10WarlordArena` trigger fires).
- L10 **Mantis Arbiter** wildcard ambush (canon-aliens; PRESENT at build but DOUBLE-gated: `L10MantisAmbush` trigger AND `m_injuredSalvariRescued`. Saving the injured Salvari is what draws the wildcard — a genuine "your choice has consequences" beat without a karma system).
- L11 camp: 7 Salvari survivor markers incl. K'thara; 8 cave bioluminescent crystal props; cave graybox; alien-equipment upgrade station interact (`L11UpgradeStation`).
- L11 **Nordic Steward** mentor (canon-aliens; allied + stationary + golden tint; the upgrade-station mentor, second-species presence distinct from the cyan-white K'thara).
- Trigger range 90–95; interact range 1–2; chain reachability assertion (L9→L10→L11).

`Act2Caves` (`act2_caves.h`):
- L12 cave-system content: hostile pack + Salvari Archives reader allies + **Memory Hunter** boss (act2BossTuning(MemoryHunter)).
- L12 **Crystal Heart Chamber**: dual-gated interactable (strengthGate AND hackGate; canActivate() iff both true; activate() latches `activated` + `storyBranch` on first valid call, idempotent thereafter).
- L13 toxic-swamp `PoisonHazardZone` (AABB + tracked exposure, inert at load, arms on entry or trigger, 8 units/sec).
- L14 mutated scientists + **timeline-gated Siren ambush** (`setSirenAmbushGate(womenLostOnF2)` read at `build()` time; F2-women-saved → no Siren placed).
- L15 Tree-City: 3 platforms at rising heights + trading-post pillar prop.
- Trigger range 100–108; reachability assertion across L11→L12→L13→L14→L15.

`canon_aliens` (`canon_aliens.h`):
- SaurianSoldier (Reptilian melee bruiser, Overlord enforcer).
- SaurianWarlord (Reptilian boss, 3 phases + memory-flash, Adaptive-Hide TODO).
- GreyTasked (synthetic worker-drone; ranged fragile).
- NordicSteward (peaceful mentor ally, startAllied + 0 dmg, stationary).
- MantisArbiter (wildcard insectoid assassin, fast melee-burst, extreme strafe).

### 2.3 Act 2 GAPS (concrete)

**L9 — Singing Crystals puzzle missing.** Design (`Level_09_CrystallineDesertEdge.json`, lines 348–360): a 7-pillar resonance puzzle (`puzzle_crystal_sequence`, solution `[3, 1, 5, 2, 7, 4, 6]`). Engine has the 6 crystal Scene props but no puzzle-state machine, no per-pillar resonance/audio bind, no failure→`CrystalGuardian` spawn.

**L9 — Salvari Archive download flow missing.** Design (`Level_09`, lines 596–730): five chambers (entry hall / history wing / science wing / war room / archive core) and a 3-stage authentication puzzle (`puzzle_archive_access`). Engine has no archive interior, no data-crystal interact, no "47 worlds" lore unlock.

**L12 — Multi-layer descent missing.** Design (`Level_12_AdvancedCaveSystem.json`, lines 51–73): explicit four layers (Upper / Mid / Deep / Abyss) with depth_range and per-layer area sets. Engine ships Memory Hunter and Crystal Heart but as flat content — needs an `Act2Caves::Layer` enum + per-layer spawn anchors + per-layer ambient light tuning.

**L12 — Hall of the Conquered (47 Worlds memorial) missing.** Design (`Level_12`, lines 491–602): 47 holographic world-displays, Mendari final symphony audio asset, final-transmissions wall. This is the lore payload that unlocks the Overlord-origin chamber. **No code yet** — needs a `HallOfTheConquered` lore-room module.

**L12 — Sealed Invasion Portal missing.** Design (`Level_12`, area_07, lines 656–725): 50 m diameter portal ring, "dimensional bleeds" VFX, "psychic pressure" gameplay effect (sanity drain). Engine has no portal prop and no sanity stat.

**L12 — Overlord Origin chamber missing.** Design (`Level_12`, area_10, lines 945–1000): 5-minute holographic recording, 5-step revelation sequence (Overlord was originally a peaceful preservation collective; "reset signal" unlocked here). This is what `L50` finale needs as a precondition. **Critical — no code yet.**

**L13 — Mutated-flora hostile not in roster.** Design calls for a stationary lash-attack hostile (`MutatedFlora` row in `act2_caves.h` comment) and `MutatedScientist` for L14 — both should land in the data-driven monster roster but the roster file isn't in `act2_caves.cpp`'s `#include` list.

**L13 — Environmental-suit mechanic missing.** Design (`Level_13` outline): poison hazard requires a suit/mask item. Engine has the exposure model but no item-gated mitigation.

**L15 — Vertical traversal mechanics missing.** Design (`Level_15_TreeCities.json`, lines 36–45): explicit vertical_scale (ground/lower/mid/upper/crown at Y=0/50/150/300/400). Engine has 3 platforms at rising Y but **no climbing/zip-line locomotion** — needs an extension to Player or a new `CanopyTraversal` controller.

**L15 — Sky Market + Healing Grove + Learning Center + Council Hall missing.** Design (`Level_15`, lines 209–245): named hub districts. Engine ships a trading-post stub only.

**L15 — Imminent-assault climax missing.** Design narrative: "A major Overlord assault is imminent." Engine ships no wave-defense for L15.

**L16/L17/L18 — Ruined Metropolis trilogy NOT SHIPPED.** No `act2_metropolis` module exists. L17 is a story-critical major level (`major_level: true` in `Level_17_Downtown.json`) with a 4-floor Parliament dungeon, 3 approach routes, regional control-node boss. **L16 carries the Breeder Queen Timeline Beta boss** (parallel to L14 Siren) — this is the second of three transformed-women confrontations in Beta and must land alongside Siren and Oracle.

**L19/L20 — Spaceport NOT SHIPPED.** No `act2_spaceport` module. Both are story-critical, `act_finale: true` for L20. L19 introduces vehicle combat (Salvari transport + hijackable Overlord scout), 3 approach options, 3 defense waves with timer for Storm Runner startup (180 sec). L20 has the most complex multi-area flow in Act 2: secured spaceport hub → counterattack alarm → 3-phase final defense → Overlord Adjutant mini-boss → 60-sec boarding sequence → atmospheric ascent (turret-defense gameplay) → orbital pursuit → fold-space jump.

### 2.4 Boss inventory (Act 2)

| Level | Boss | Type | Phases | HP | Status |
|-------|------|------|--------|----|----|
| L10 | Saurian Warlord | Reptilian Boss (canon-aliens) | 3 + memory-flash | 540 | **SHIPPED** |
| L10 | Mantis Arbiter | Wildcard insectoid | — (ambush) | — | **SHIPPED** (double-gated) |
| L12 | Memory Hunter | Ancient horror | 3 (Physical / Absorption / Hunger) | 8000 | **SHIPPED** (base) — phase 2 Memory-Echo summons + identity-crisis control-swap missing |
| L14 | The Siren | Transformed Aria, Beta-only | — | — | **SHIPPED** (timeline-gated) |
| L16 | Breeder Queen | Transformed Keisha, Beta-only | — | — | **NOT SHIPPED** |
| L17 | Overlord Enforcer | Regional Control-Node Boss | 3 (4-floor Parliament) | — | **NOT SHIPPED** |
| L19 | Planetary Defense Commander | Spaceport mini-boss | 1 | — | **NOT SHIPPED** |
| L20 | Planetary Garrison Commander | Act 2 finale boss | 3 (ground / mech / orbital-strike escape) | — | **NOT SHIPPED** |
| L20 | Overlord Adjutant | Final-wave mini-boss | — | 4000 | **NOT SHIPPED** |

---

## 3. Act 3 (L21–L35) Reconciliation + Space-Engine Subsystem Map

### 3.1 Per-level table

| # | Name | Setting | Boss / Major Encounter | Storm Runner role | Engine subsystem mapping | Shipped? |
|---|------|---------|------------------------|-------------------|--------------------------|----------|
| 21 | Departure | Atmosphere escape → orbital battle → first FTL jump | Orbital patrol skirmish | Tutorial / hub intro | S4 (atmospheric ascent), S0 (DeepSpace context), S3 (wormhole), S5 (interior tutorial), S6 (windows) | **NO** (subsystems exist, level slice missing) |
| 22 | Asteroid Field | Mining mini-game, Void Rays (silicon manta rays), hidden pirate cache | Asteroid hazards | Refuel, exterior mining | S1 (proxy asteroids), S10 (collision damage), S12 (EVA mining), S0 proxies | **NO** |
| 23 | Salvari Prime Ruins — Surface | K'thara's destroyed homeworld, Memorial City | None | Mobile hub | S4 (descent to Salvari Prime), `--world` surface handoff | **NO** |
| 24 | Salvari Prime — Archives | Deep lore: 47 worlds, Overlord true nature, weakness analysis | Archive defenders | Stationary | New `act3_salvari_prime` module | **NO** |
| 25 | Salvari Prime — Memorial City | Hidden enclave (50 more Salvari), ancient weapon cache | **Memory Hunter** (corrupted Salvari AI variant) | Stationary | Reuses Memory Hunter boss from L12 — distinct instance, different lore framing | **NO** (boss type shipped, instance + arena not) |
| 26 | Mining Colony Liberation | Infiltration, 200+ prisoner rescue | Force-field disable | Departure | New module; reuses Act 2 stealth/combat | **NO** |
| 27 | Space Casino "Fortune's End" — Floor | Casino floor, bar, VIP lounge, security office | Information broker contact | Docked | New `act3_casino` module (economy + dialog) | **NO** |
| 28 | Space Casino — High Stakes | Poker-style mini-game, cheating mechanics (Sarah's hacking) | Double-cross | Docked | Continues `act3_casino` | **NO** |
| 28 (Beta) | Space Casino | **The Oracle** (transformed Emily; Timeline Beta) | — | — | Third of the three Beta transformed-women bosses (Siren=L14, Breeder Queen=L16, **Oracle=L28**) | **NOT SHIPPED** |
| 29 | Rebel Rendezvous Point | 5-world alliance meeting | None | Docked | New `act3_rebel_alliance` module (negotiation UI) | **NO** |
| 30 | Asteroid Rebel Base — Exterior | Approach through field, identification challenge | Docking sequence | Docked | S1 proxies + S4-style approach cinematic | **NO** |
| 31 | Asteroid Rebel Base — Interior ⭐ | Full alliance hub: command / military / civilian / Salvari embassy / **Crystal Heart Installation** | None | Stationary major hub | Mirrors Storm Runner hub structure (S5 ShipInterior with `ShipClass::Huge` manifest); **Crystal Heart installation here is the L12 storyBranch payoff** — if `m_storyBranch == true` (Crystal Heart activated at L12), an interior interactable shows the installed Heart and provides a planet-wide buff visible in Sol arrival | **NO** (S5 supports it; manifest not authored) |
| 32 | Fleet Assembly | Ship upgrades, crew assignments, strategic planning | None | Stationary | New `act3_fleet_assembly` module (fleet composition data) | **NO** |
| 33 | Warp Gate Assault | Space battle to capture warp gate; boarding action; race against reinforcements | Multiple Overlord cruisers | Combat | S8 (enemy ships), S9 (targeting), S10 (damage), S1 (warp-gate proxy as `Proxy::Wormhole`), S3 (gate activation = wormhole transit) | **NO** |
| 34 | En Route to Earth | Character resolution scenes; final upgrades; wedding (Omega) | None | Interior bonding | S5 (interior), wedding cinematic (Omega-only branch — `m_omegaTimeline`); needs companion dialog scene system | **NO** |
| 35 | Sol System Arrival ⭐ (Act 3 finale) | Exit warp near Mars; see Overlord fleet around Earth; receive distress signals; choose landing zone | Strategic-overview moment | Combat-staging | S0 (Context::DeepSpace), S1 (Earth + Sol planets), S6 windows show Earth | **NO** |

### 3.2 Space engine subsystems (shipped) and their Act 3 mapping

| Slot | Subsystem | File | What Act 3 needs from it |
|------|-----------|------|--------------------------|
| S0 | SpaceLayer | `app/space/space_layer.h` | Context state machine (DeepSpace / WormholeTransit / EVA / AtmoDescent / Surface). Every Act 3 level transitions through this. |
| S1 | SpaceEnv | `app/space/space_env.h` | Star/nebula dome, proxy planets (Salvari Prime, Earth, Mars, asteroid belts, casino station, rebel base), sun. Backdrop for L21, L22, L23, L30, L33, L35. |
| S2 | LOD | `app/space/lod.h` | Distance-LOD for ship/station/planet proxies. L22 asteroid field will exercise this hard. |
| S3 | WormholeTransit | `app/space/wormhole_transit.h` | Crystal-matrix interstellar jump. L21 first FTL jump; L33 warp gate; every system-to-system transition L21→L23→L26→L27→L29→L30→L33→L35. |
| S4 | AtmoDescent | `app/space/descent.h` | Orbit→ground cinematic. L23 (Salvari Prime), L26 (Mining Colony), L35→L37 (Earth descent — also feeds Act 4). |
| S5 | ShipInterior | `app/space/ship_interior.h` | Walkable Storm Runner. The 4-deck layout (Command / Crew / Operations / Salvari Tech) maps directly onto a `ShipManifest` with `ShipClass::Huge`. Rebel Base interior (L31) is a second Huge-class manifest. |
| S6 | ShipWindows | `app/space/ship_windows.h` | "Star Trek moving space outside the glass" — the signature interior tech. Drives the parallax sky in all interior segments while the ship is in DeepSpace or WormholeTransit. |
| S7 | ShipRepair | `app/space/ship_repair.h` | In-transit repair gameplay. Storm Runner damaged-panel wiring; exercised during L33 boarding action and post-Warp-Gate damage scene. |
| S8 | ShipAi | `app/space/ship_ai.h` | Enemy ships (Patrol / Engage / Strafe / Evade). L21 patrol skirmish; L33 Warp Gate assault; L35 Overlord fleet contact. |
| S9 | Targeting | `app/space/targeting.h` | Radar / lead-the-target / lock-on HUD. Every combat encounter L21, L33, L35. |
| S10 | ShipDamage | `app/space/ship_damage.h` | Shield/hull/subsystems (Engines / Turrets / ShieldGen / Sensors). Player ship AND enemy ships use it. Capital ships (L33 cruisers, L36 Dreadnought) need `hasSubsystems == true`. |
| S11 | ShipAnim | `app/space/ship_anim.h` | Node-transform animation: landing gear, hull panels, turrets. L21 takeoff, L22 mining-gear deploy, L30 docking, L31 fleet rotation. |
| S12 | EVA | `app/space/eva.h` | Zero-G spacewalk + mag-boots. L22 asteroid mining; L33 boarding action; L48 mothership infiltration (also Act 4). |

### 3.3 Storm Runner hub specification (TASK_4 lines 36–60)

The Storm Runner is a 4-deck Salvari vessel, the mobile hub for Acts 3 (and the player's seat for Act 4's space-combat sequences).

**Deck 1 — COMMAND**
- Bridge (mission selection terminal; navigation; FTL jump initiation)
- Captain's Quarters (Jake's room; relationship scenes; timeline-branching dialog)
- War Room (alliance status display; strategic planning UI)

**Deck 2 — CREW**
- Crew Quarters (one room per surviving companion; timeline-dependent occupancy)
- Medical Bay (healing; cure-research synthesis — feeds `cure_research_complete` story flag from L12)
- Mess Hall (dialog/morale)
- Training Room (ability upgrades)

**Deck 3 — OPERATIONS**
- Armory (weapon customization; ancient-weapon-blueprint blueprints download from L12 unlock here)
- Engineering (ship upgrades; uses S7 ShipRepair flow for damage)
- Cargo Bay (resources, recruit storage)
- Shuttle Bay (mission deployment — connects to S4 AtmoDescent)

**Deck 4 — SALVARI TECH**
- K'thara's Archive (lore terminal — replays L9/L12 archive content)
- Crystal Heart Interface (only if L12 storyBranch active; reuses `CrystalHeartChamber` interactable type)
- Alien Weapon Lab (unique weapons unlocked across Act 2)

**Engine mapping:**
- One `ShipManifest` with `ShipClass::Huge`, ~14 `Room` entries, ~20 `Door` entries, ~10 `Station` markers (one per interactable, e.g. Bridge has nav-station, Mess Hall has morale-station).
- Per-deck `windows` arrays so S6 ShipWindows shows moving stars on every deck (Bridge gets the largest panes).
- Cargo Bay needs an integration with the rescued-companion / recruit roster (the "recruits as crew" feature L26+ needs).

**Shipped status:** S5 ShipInterior + S6 ShipWindows fully support this. **MISSING:** the actual `ShipManifest` JSON / authoring is not in any shipped module. Needs a new `act3_storm_runner` module that builds the manifest, places mission-board station markers, and wires Bridge nav-station → `SpaceLayer::requestWormhole(destSystemId)`.

### 3.4 Act 3 GAPS (concrete)

**Storm Runner hub ShipManifest not authored.** S5 supports it; the manifest needs to be built.

**Mission-board / mission-selection UI missing.** TASK_4 expects a Bridge station that lets the player pick the next system to jump to. No code today.

**Space combat HUD (radar + lock-on overlay) not wired.** S9 TargetingSystem is headless logic only — needs a HUD pass on top.

**Capital-ship combat (cruisers, dreadnoughts) not authored.** S10 supports `hasSubsystems`; S8 ShipAi is fighter-scale; needs a `CapitalShipAi` extension that uses the subsystem-targeting hooks.

**Casino mini-games (L27/L28) not designed in engine.** Poker-style mini-game + cheating mechanic (Sarah's hacking). New module.

**Mining mini-game (L22) not designed.** New module + an asteroid-cluster `Proxy` extension.

**The Oracle boss (L28, transformed Emily, Timeline Beta) not specified in `canon_aliens` or any boss module.** Needs a new `act3_casino` boss row.

**Memory Hunter L25 variant** can reuse the L12 boss type — just a second instance with different lore framing.

**Crystal Heart Installation at Rebel Base (L31) is a payoff for L12's `m_storyBranch` flag.** No code reads that flag at L31 today. Needs an `act3_rebel_base` module that imports `CrystalHeartChamber` (or a `CrystalHeartInstalled` cousin).

**Wedding scene (L34, Timeline Omega only) not designed.** Needs a dialog-tree + companion-relationship system. Cross-cutting with Act 1's timeline gate.

**Fleet composition / alliance points (AP) data store missing.** AP is the central system that gates the 12 endings; needs to be a save-game persisted scalar with named contributors (Sarah rescued: +50, etc.) — see Appendix C of `TASK_5_ESCAPE_LAB_48`.

---

## 4. Act 4 (L36–L50) Reconciliation + Earth Liberation Flow

### 4.1 Per-level table

| # | Name | Setting | Boss | Duration | Status |
|---|------|---------|------|----------|--------|
| 36 | Orbital Assault | Earth orbit, L2 Lagrange → atmosphere edge | **Dreadnought "Extinction"** (50,000 HP, 3 phases; phase 3: ship falling to Earth, 90-sec timer to destroy reactor core) | 45–60 min | **NOT SHIPPED** |
| 37 | Atmospheric Entry | Upper atmosphere → ground | None (vehicle descent / survival) | 25–35 min | **NOT SHIPPED** |
| 38 | Base Establishment | Landing zone + surroundings | **The Broadcaster** (3,000 HP; 3 phases: drone defense / desperate cable-whip / surrender) | 30–40 min | **NOT SHIPPED** |
| 39–41 | Regional Liberation (Urban Warfare / Breeding Center / Military Base) | Player-chosen order across landing zone | Urban: Regional Commander (Sheriff/Inquisitor/Salaryman/Pharaoh/Warlord by zone). Breeding: **The Director** OR **The Siren** (Beta). Military: **The War Machine** (8,000 HP bio-tank) + **The General** (4,000 HP cyber). | 90–120 min total | **NOT SHIPPED** |
| 42 | New York Reclamation | Manhattan: Brooklyn Bridge / Wall St / Times Square / Empire State / Central Park | **PRIME-7** (10,000 HP, 3 phases: commander / collective reality-distortion / self-destruct QTE) | 50–60 min | **NOT SHIPPED** |
| 43 | London Underground | Tube network, Victorian tunnels, Big Ben | **The Warden** (6,000 HP) | — | **NOT SHIPPED** |
| 44 | Tokyo Neon Nightmare | Shibuya, Akihabara, Imperial Palace, Shinjuku (50+ floors) | **The Executive** (6,000 HP) | — | **NOT SHIPPED** |
| 45 | Return to Lab Zero ⭐⭐ | Lab Zero mutated, floor-by-floor descent + 3 sub-levels (Breeding Core / Chen's True Lab / Original Artifact) | **The First** (12,000 HP, 3 phases; phase 3 offers Destroy / Absorb / Negotiate choice) | 40–50 min | **NOT SHIPPED** |
| 46 | Global Counter-Offensive | Global command center, 11 fronts (Berlin / Moscow / Tokyo / Shanghai / Mumbai / Chicago / LA / São Paulo / Cairo / Nairobi / Sydney) | Real-time strategy (no traditional boss) | 35–45 min | **NOT SHIPPED** |
| 47 | Mothership Approach | Space elevator / shuttle / inside job | **Fleet Admiral** (5,000 HP) | — | **NOT SHIPPED** |
| 48 | Mothership Infiltration | Hangar / Quarters / Engineering / Command | **Ship's Heart** (7,000 HP, AI/Organic hybrid) | 50–60 min | **NOT SHIPPED** |
| 49 | Proto-Overlord Gauntlet | Boss rush of 5 corrupted Earth leaders | **The General / The Executive / The Scientist / The Prophet / The Monarch** (6,000 HP each; choice per: Kill / Deprogram (variant) / Imprison) — Timeline Beta replaces one with Siren/Breeder/Oracle | — | **NOT SHIPPED** |
| 50 | The Overlord Core ⭐⭐⭐ | Mothership core / dimensional nexus | **Overlord Collective** (24,000 HP total; 4 phases: Avatar 10000 / Swarm 8000 / Truth 5000 / Choice 1000) | 60–90 min | **NOT SHIPPED** |

### 4.2 Earth liberation flow (Acts 4 structure)

**Landing zone choice (L37) — Tim's "6 continents" question:** the corpus lists **5 zones**, not 6:

1. **North America** — Rocky Mountains; 20 soldiers, 3 vehicles, 1 helicopter; military structure, advanced weapons.
2. **Europe** — Swiss Alps; 15 soldiers, 2 motorcycles, medical supplies; tunnel network.
3. **Asia** — Rural Japan; 10 soldiers, 1 truck; high population, advanced tech.
4. **Africa** — Ethiopian Highlands; 8 soldiers, 4 horses, satellite phone; minimal occupation, fortress terrain.
5. **South America** — Amazon Basin; 12 soldiers, 2 boats, jungle gear; excellent concealment.

**Open question for Tim:** the prompt mentions "6 continents." A sixth zone is not in the design corpus. Reasonable options: (a) **Oceania / Sydney** (Sydney appears as a Global-Offensive front at L46 — promote to a landing zone), or (b) **Antarctica** (research-base aesthetic, alien-artifact tie-in). Default suggestion: **Oceania**, matching L46's 11-front list.

**Regional ordering (L39–L41) is player-choice:** the trilogy plays in any order; difficulty scales: First = base, Second = +20%, Third = +40%. The three mission TYPES are constant (Urban Warfare / Breeding Center / Military Base); the LOCATION of each type adapts to the landing zone (Urban = Denver/Prague/Osaka/Cairo/São Paulo).

**Major city sequence (L42–L44):** NYC is mandatory; London and Tokyo are listed as variants — design implies they're parallel content unlocked by the L46 RTS results. **Open question:** are L43 (London) and L44 (Tokyo) **always played** or are they branches off L42? Design corpus phrases L43/L44 as "outlined" (less detailed than L42), suggesting they are full levels played in sequence after L42. Default: **sequential** (L42 → L43 → L44).

**L45 Return to Lab Zero — emotional climax.** Floor-by-floor descent F7→F6→F5→F4→F3→F2→F1 → sub-levels. Floor 2 is the rescue-cells emotional confrontation (Beta: see what became of Aria/Keisha/Emily). Sub-level 3 is the **65-million-year-old stasis pod**: Earth was marked for 65 million years; the dinosaur-killing asteroid was an Overlord seeding pod.

**L46 Global Counter-Offensive — RTS layer.** Multi-front coordination across 11 cities, 4 command abilities (Focus Fire / Reinforce / Orbital Strike / Rally), commanders chosen by timeline (Aria=medical, Keisha=military, Emily=tech, Sarah=drones, K'thara=Salvari). This is a unique gameplay mode not covered by any existing space/surface engine — needs a new `act4_rts` module.

**L47/L48 Mothership** — 3 approach options at L47 (space elevator / shuttle / inside job), then 4-section infiltration at L48. **Mothership = 5 km diameter capital ship** — exercises S10 capital-ship subsystem damage AND S12 EVA boarding.

**L49 Proto-Overlord Gauntlet** — boss rush. Beta variant: one of the five bosses is replaced with the third transformed-women (Oracle) confrontation — note that this is the **Beta-finale** for the Siren/Breeder/Oracle arc only if the Oracle wasn't fought at L28 in Act 3. Cross-act gating is needed.

**L50 The Overlord Core finale** — 4-phase fight that ENDS in a Choice screen with 4 options (Destroy / Sever / Absorb / Negotiate; Negotiate requires 700+ AP). This is the ending fork.

### 4.3 Act 4 GAPS (full — nothing is shipped for Act 4)

**No `act4_*` module exists.** Every level above needs to be authored. Suggested module layout (mirrors Act 2 lane pattern):

- `act4_orbital` — L36 (uses S0/S1/S8/S9/S10/S11) + Dreadnought capital-ship boss.
- `act4_descent` — L37 atmospheric entry (uses S4 AtmoDescent; needs 5- (or 6-) zone landing-zone choice; introduces vehicle-descent gameplay).
- `act4_landing_zone` — L38 base establishment (base-building system + Broadcaster boss); resource economy (Materials/Personnel costs from `TASK_5_ESCAPE_LAB_48` Table 1).
- `act4_regional` — L39/L40/L41 (3 mission types × 5–6 zones; player-choice ordering; difficulty scaling).
- `act4_cities` — L42 NYC (4 phases), L43 London, L44 Tokyo.
- `act4_lab_zero` — L45 Return to Lab Zero (reuses the L1 spire structure with mutation overlay).
- `act4_rts` — L46 Global Counter-Offensive (NEW gameplay mode; multi-front RTS).
- `act4_mothership` — L47 + L48 (S12 EVA + S5 ShipInterior for the alien capital-ship interior).
- `act4_gauntlet` — L49 boss rush (5 bosses + Beta override).
- `act4_finale` — L50 Overlord Core (4-phase fight; 4-choice ending fork; reset-signal tie-in to L12 origin chamber).

**Alliance Points (AP) system missing.** Central to ending gating. Needs a persisted scalar across all save data with named contributors (see TASK_5_ESCAPE_LAB_48 Appendix C). All boss-fate choices, rescue completions, romance/wedding completions write to AP.

**Choice-graph system missing.** L36 (aggressive/tactical/sacrifice), L37 (landing zone), L38 (boss fate kill/cure/capture), L42 (boss self-destruct QTE), L45 (destroy/absorb/negotiate), L49 (kill/deprogram/imprison per boss), L50 (destroy/sever/absorb/negotiate) all feed AP and the ending fork. Needs a dedicated module.

**Cinematic system for endings missing.** Each of the 12 endings has a 6–8 minute cinematic spec (see TASK_5_ACT4_EXPANDED_PRODUCTION_GUIDE Part 4). Engine has no cutscene playback system at this scale.

**Real-time strategy (L46) gameplay loop missing.** Distinct from third-person action combat — needs commander selection, resource allocation, ability cooldowns (Focus Fire 60s, Reinforce 90s, Orbital Strike 180s, Rally 45s), multi-front status display.

**Base-building (L38) economy missing.** Materials + Personnel costs; 6 structure types (Armory / Research Lab / Hangar / Comm Tower / Defense Wall + one more); buffs (+25% damage / tech tree unlock / vehicle repair / etc.).

**Vehicle combat (Humvee / Tank / Helicopter / APC) at L41 (Military Base) missing.** L19/L20 already need vehicle combat for the spaceport — a `Vehicle` system should land in Act 2 and serve Act 4.

**Reality-distortion gameplay effects (PRIME-7 phase 2, The First phase 3, L50 phase 3) missing.** Gravity-shift, time-dilation zones, memory-attack (hallucination sequence). These are new movement/physics effects.

**Drone-army advantage (Omega Timeline + Master Hack) missing.** L50 phase 2 swarm calculations require a player-side drone-army count from Act 1 (Sarah's master-hack), persisted through all of Act 3 and Act 4.

**Alternate-Jake-summons (L50 phase 3) missing.** Four AI variants of Jake (Corrupted / Failed / Tyrant / Absorbed) with Jake's moveset. Needs a "mirror-match AI" mode.

**Mothership-collapse escape sequence (L50 post-choice) missing.** 4-minute timer to reach hangar; debris hazards.

---

## 5. The 12 Endings (Full Catalog)

The ending fork is determined by Phase 4 of L50 + Alliance Points + Timeline (Alpha/Beta/Omega) + cross-act flags (companions, rescues, Crystal Heart, wedding, K'thara romance, drone-army hack).

### Tier matrix

| Tier | Endings |
|------|---------|
| **GOLDEN** | 1. Perfect Liberation · 2. Galactic Alliance · 3. The Wedding |
| **GOOD** | 4. Pyrrhic Victory · 5. Timeline Alpha Family · 6. K'thara Romance |
| **NEUTRAL** | 7. Sole Survivor · 8. The Sacrifice |
| **BAD** | 9. Corruption · 10. Breeder Victory |
| **NIGHTMARE** | 11. The Emperor · 12. Total Failure |

### Per-ending requirements (canonical matrix from TASK_5_ESCAPE_LAB_48 Appendix E)

| # | Name | AP | Timeline | Final Choice | Cross-Act Preconditions | Post-credits content |
|---|------|----|----------|--------------|-------------------------|----------------------|
| 1 | Perfect Liberation | 700+ | Any | A (Destroy) or B (Sever) | All companions survive Acts 1–4; all rescues (Sarah, Aria, Keisha, Emily, Chen); Crystal Heart built (L12 storyBranch + L31 installation) | 8-min cinematic: Earth healing timelapse → Council Chamber → Jake's speech ("we evolved through resistance") → family dinner → Earth from orbit with Crystal Heart shield active |
| 2 | Galactic Alliance | 750+ | Any | D (Negotiate) | All Archive databases (L9 + L12 + Salvari Prime L24) | 5 years later: Human-Salvari-Overlord coalition; first joint expedition to other civilizations |
| 3 | The Wedding | 600+ | **Omega** | A or B | L34 wedding scene completed; Sarah alive | 3 years later: Jake & Sarah's farmhouse; adopted children; pregnant Sarah |
| 4 | Pyrrhic Victory | 400–599 | Any | A or B | High casualties (failed rescues; civilian losses) | 2 years later: 2.3 billion dead; Jake rebuilding alongside survivors |
| 5 | Timeline Alpha Family | 500+ | **Alpha** | A or B | All children survive (the polyamorous-family children born by Year 5) | Polyamorous family thrives: Aria, Keisha, Emily, Jake raising four children |
| 6 | K'thara Romance | 500+ | Any | B (Sever) or D (Negotiate) | K'thara romance completed | Human-Salvari hybrid child; moon colony |
| 7 | Sole Survivor | 200–399 | Any | A or B | All companions dead | Jake alone with rescued-survivor surrogate family |
| 8 | The Sacrifice | 500+ | Any | A or B | Jake dies in final battle (failed QTE during L50 escape) | Statue in NYC; legacy continues through saved survivors |
| 9 | Corruption | 400+ | Any | C (Absorb) | Resist corruption check delayed | 5 years later: Jake as benevolent dictator; peace at cost of freedom |
| 10 | Breeder Victory | Any | **Beta** | C or final-battle fail | Sarah captured and broken (Beta-only) | Sarah rules as Breeder Queen; Jake leads underground resistance |
| 11 | The Emperor | 0–200 | Any | C (Absorb) | Corruption embraced | 10 years later: Jake worse than Overlord; galactic tyrant |
| 12 | Total Failure | 0–100 | Any | Fail | Multiple failed QTEs during L50 | Earth absorbed; humanity's consciousness trapped in Collective forever |

### Cross-act precondition chains for endings

- **Crystal Heart** (gates Ending 1's bonus): L12 dual-gate puzzle activated → `m_storyBranch == true` → L31 Rebel Base interior installation → planet-wide buff at Sol Arrival → L50 Choice A/B with 700+ AP → Ending 1.
- **All databases** (gates Ending 2): L9 Salvari Archive + L12 deep archive + L24 Salvari Prime archive = `m_allDatabases == true` → L50 Choice D → Ending 2.
- **Wedding** (gates Ending 3): Omega timeline + L34 wedding cinematic completed + 600+ AP → L50 Choice A/B → Ending 3.
- **K'thara Romance** (gates Ending 6): K'thara recruited (Act 2 Beta or completed alliance quest) + per-Act bonding scenes + L34 romance dialog → `m_ktharaRomance == true` → Ending 6.
- **Drone Army** (massively reduces L50 Phase-2 difficulty): Sarah's master-hack in Act 1 (+75 AP); persists as `m_droneArmy == true`; at L50 Phase 2 the swarm-clear time drops from ~60 seconds to ~8 seconds.

### Post-credits content

The corpus only gives full 6–8 minute cinematic specs for **Ending 1** and **Ending 11** (TASK_5_ACT4_EXPANDED_PRODUCTION_GUIDE Part 4). For the other 10 endings, the corpus provides 2–3 line summaries (see table above) but expects the implementation pass to script the equivalent scenes. **This is an open content gap that should be tracked at implementation time.**

---

## 6. Cross-Act Unlock Chains (Which Content Gates Which)

```
ACT 1
├─ Sarah rescued (+50 AP) ─────────────────────────────┐
├─ Aria rescued (+50 AP)   ────────────────────────────┤
├─ Keisha rescued (+50 AP) ────────────────────────────┤
├─ Emily rescued (+50 AP)  ────────────────────────────┤
├─ Chen rescued (+50 AP)   ────────────────────────────┤
├─ Chorus spared (+15 AP)  ────────────────────────────┤
└─ Drone army hack (+75 AP) ─────► m_droneArmy ────────┐│
                                                       ││
ACT 2                                                  ││
├─ L9 Salvari Archive download (+25 AP/database) ──────┤│
├─ L12 Crystal Heart activated ──► m_crystalHeart ─────┤│
├─ L12 deep archive download ─────► m_overlordOrigin ──┤│
├─ L12 ancient weapon blueprints ─► m_blueprintsObtained
├─ L12 Memory Hunter killed ─────► m_memoryHunterKilled
├─ L10 injured Salvari rescued ──► m_injuredRescued (gates Mantis ambush)
├─ L10 boss mercy choice (+15 AP per merciful boss)
├─ Salvari full alliance (+100 AP)
├─ K'thara recruited ──► m_ktharaRecruited (gates Ending 6)
├─ L14 Siren fight (Beta only) ──► m_sirenDefeated
├─ L16 Breeder Queen fight (Beta only) ──► m_breederDefeated
├─ L18 multi-species alliance ──► m_multiSpeciesAlliance
└─ L20 Spaceport ship choice ──► m_shipType {Military|Civilian|StormRunner}
                                  └─ Storm Runner unlocks Salvari tech tree

ACT 3
├─ L21 first FTL jump ──► Storm Runner hub unlocked
├─ L23-L25 Salvari Prime trilogy ──► m_salvariPrimeArchive (+all-databases bit)
├─ L26 Mining Colony liberation ──► +recruits (10-25 AP each)
├─ L28 Oracle fight (Beta only) ──► m_oracleDefeated
├─ Rebel fleet assembled (L29-L31) ──► +50 AP
├─ L31 Crystal Heart installation (requires m_crystalHeart) ──► +planetary buff
├─ L34 Wedding (Omega only) (+25 AP) ──► m_wedding
└─ L33 Warp Gate captured ──► Act 4 unlocked

ACT 4
├─ L36 Orbital Assault choice ──► AP -10..+55
├─ L37 Landing zone choice ──► determines L39-L41 location names
├─ L38 Broadcaster fate ──► AP -5..+35
├─ L39-L41 trilogy ──► +85..+180 AP combined (+2 per civilian rescue, -2 per casualty)
├─ L42 PRIME-7 kill ──► NYC liberated; +50..+100 AP
├─ L43 London Warden kill
├─ L44 Tokyo Executive kill
├─ L45 The First choice ──► [Destroy +50AP / Absorb +75AP / Negotiate +100AP (700AP req)]
│   └─ Negotiate unlocks Ending 2 path
├─ L46 Global Offensive ──► +75..+150 AP based on front success
├─ L47-L48 Mothership infiltration
├─ L49 Gauntlet ──► 5 bosses × [Kill/Deprogram/Imprison] = -20..+20 AP each
└─ L50 ──► Final Choice [Destroy/Sever/Absorb/Negotiate]
           determines ending 1-12 by AP + timeline + cross-act flags
```

### Critical gating questions

**Q1: Can the L12 storyBranch (Crystal Heart) be skipped?** Yes — the chamber's dual-gate is currently `false` at load. Skipping it locks the player out of Ending 1's "Crystal Heart shield" bonus content but does not block any other ending.

**Q2: Are the three Beta transformed-women bosses (Siren L14 / Breeder Queen L16 / Oracle L28) mandatory in Beta?** YES per design intent — Beta is the "you didn't save the women in Act 1" timeline; meeting their transformed selves is the emotional core. **Open question:** what happens in Beta at L49 Proto-Overlord Gauntlet if the player has already killed all three by L28? Design says "One boss replaced by Siren/Breeder Queen/Oracle confrontation" — implies the L49 substitution uses ONE of them, but if all three are dead by L28, the substitution can't happen. Default: L49 substitution uses the most recent of the three to die, replayed as a Memory Echo (uses Memory Hunter phase-2 summons system).

**Q3: Is Negotiate (Ending 2) gated by all databases OR by AP?** Design says BOTH (750+ AP + all databases). The `m_allDatabases` bit requires L9 + L12 + L24 (Salvari Prime Archives).

**Q4: How does L37 landing zone affect L39–L41 difficulty?** Mission TYPES (Urban / Breeding / Military) are constant; LOCATIONS adapt (Urban in NA = Denver; Urban in EU = Prague; etc.). Difficulty scales with player-choice order (1st = base, 2nd = +20%, 3rd = +40%) NOT with landing zone.

---

## 7. Implementation Plan: Existing Files to Extend, New Files Needed

### 7.1 Extend existing files

| File | Extension |
|------|-----------|
| `app/act2_world.{h,cpp}` | Add L9 Singing-Crystals puzzle state machine; add L9 Salvari Archive interior (5 chambers) + 3-stage authentication; add an `Act2World::onPuzzleSolve()` and `Act2World::archive()` accessor. |
| `app/act2_caves.{h,cpp}` | Add L12 layer enum (Upper/Mid/Deep/Abyss); add `HallOfTheConquered` lore room (47 holographic world-displays + Mendari final-symphony audio); add `SealedPortalRoom` (dimensional-bleed VFX hook + `m_psychicPressure` exposure); add `OverlordOriginChamber` (5-minute hologram + `m_resetSignalObtained` flag); add Memory Hunter phase-2 `MemoryEcho` summons + phase-2 identity-crisis control-swap effect. Add L13 environmental-suit mechanic gating the poison hazard. Add L15 vertical-traversal + canopy assault wave. |
| `app/canon_aliens.{h,cpp}` | Add `MutatedFlora` (L13 stationary lash), `MutatedScientist` (L14 ranged), `BreederQueen` (L16 Beta boss), `Oracle` (L28 Beta boss), `OverlordEnforcer` (L17 boss), `PlanetaryDefenseCommander` (L19 mini-boss), `PlanetaryGarrisonCommander` (L20 finale boss), `OverlordAdjutant` (L20 mini-boss). |
| `app/space/space_layer.h` | (FROZEN — do not extend. If new contexts needed, add separate enum.) |
| `app/space/ship_ai.h` | Add capital-ship AI variant (subsystem targeting; standoff escort behavior). |
| `app/space/ship_interior.h` | (No extension needed — Storm Runner uses the existing manifest API.) |

### 7.2 New files needed (Act 2)

| New module | Levels owned | Responsibilities |
|------------|--------------|------------------|
| `app/act2_metropolis.{h,cpp}` | L16/L17/L18 | Ruined Metropolis trilogy: scavenger encounters, Parliament 4-floor dungeon (3 approach routes + control-node boss), underground resistance HQ hub, **Breeder Queen** Beta boss at L16. Trigger range 110–125. |
| `app/act2_spaceport.{h,cpp}` | L19/L20 | Spaceport assault + finale: 3 approach routes, vehicle combat intro, 3 defense-wave systems, **Planetary Defense Commander** mini-boss (L19), **Planetary Garrison Commander** 3-phase boss (L20), **Overlord Adjutant** mini-boss (L20 final wave), 60-sec boarding sequence, turret-defense ascent. Trigger range 126–145. |
| `app/vehicle_combat.{h,cpp}` | Cross-cutting (L19/L20/L41) | Drivable vehicles: Salvari transport (no weapons, light armor), Overlord scout (hijackable, light cannon, medium armor), Humvee/Tank/Helicopter/APC for L41. New Vehicle entity type. |

### 7.3 New files needed (Act 3)

| New module | Levels owned | Responsibilities |
|------------|--------------|------------------|
| `app/act3_storm_runner.{h,cpp}` | Hub (used across L21–L35) | 4-deck `ShipManifest` (Huge class); Bridge nav-station → `SpaceLayer::requestWormhole()`; Mess Hall morale; Crystal Heart Interface (gated by `m_crystalHeart`); K'thara's Archive replay-terminal; Alien Weapon Lab. |
| `app/act3_departure.{h,cpp}` | L21 | Atmospheric ascent (S4 reverse), orbital battle skirmish (S8 + S9 + S10), first FTL (S3). |
| `app/act3_asteroids.{h,cpp}` | L22 | Asteroid-field navigation (S1 proxy asteroids + S2 LOD), mining mini-game, Void Rays ambient-creature renderer, hidden pirate cache. Includes EVA mining (S12). |
| `app/act3_salvari_prime.{h,cpp}` | L23/L24/L25 | Salvari Prime trilogy: Memorial City surface, Archives (gates `m_allDatabases`), Memory Hunter variant boss at L25. |
| `app/act3_mining_colony.{h,cpp}` | L26 | Liberation infiltration; 200+ prisoner-rescue counter; recruit gain. |
| `app/act3_casino.{h,cpp}` | L27/L28 | Casino floor + bar + VIP lounge + security office + smuggler's den + intel broker + fighting pit; poker-style mini-game; **Oracle** (Beta-only) boss; cheating mechanic (Sarah's hacking). |
| `app/act3_rebel_alliance.{h,cpp}` | L29/L30/L31 | 5-world alliance meeting (negotiation UI); rebel-base exterior approach + docking; rebel-base interior `ShipManifest` (Huge class); Crystal Heart Installation (reads L12 `m_crystalHeart`). |
| `app/act3_fleet_assembly.{h,cpp}` | L32 | Fleet composition data; ship-upgrade UI; crew assignment from recruits. |
| `app/act3_warp_gate.{h,cpp}` | L33 | Warp-Gate-class capital-ship boarding; S12 EVA boarding; S3 gate activation. |
| `app/act3_en_route.{h,cpp}` | L34 | Companion resolution scenes; Omega wedding cinematic (gated). |
| `app/act3_sol_arrival.{h,cpp}` | L35 | Mars warp-exit; Earth + Overlord fleet contact; landing-zone-choice handoff. |
| `app/dialog_tree.{h,cpp}` | Cross-cutting | Dialog tree system (used by Storm Runner, casino, rebel base, en-route, all NPC interactions). Independent of existing `npc_dialog.{h,cpp}` if that is just a 1-line popup. |
| `app/alliance_points.{h,cpp}` | Cross-cutting | Persisted AP scalar + named contributors. Saved/loaded from save game. |

### 7.4 New files needed (Act 4)

| New module | Levels owned | Responsibilities |
|------------|--------------|------------------|
| `app/act4_orbital.{h,cpp}` | L36 | Fleet command UI + 3 waves + Dreadnought "Extinction" 3-phase boss (capital ship). |
| `app/act4_descent.{h,cpp}` | L37 | 5- (or 6-) zone landing-zone choice; ship-damage descent; setup for L38. |
| `app/act4_base_establish.{h,cpp}` | L38 | Base-building economy (6 structures, Materials/Personnel costs); **Broadcaster** 3-phase boss. |
| `app/act4_regional.{h,cpp}` | L39/L40/L41 | Player-chosen-order trilogy: Urban (5 regional commanders by zone), Breeding (**Director** OR **Siren** Beta), Military (**War Machine** + **General** two-stage). |
| `app/act4_cities.{h,cpp}` | L42/L43/L44 | NYC (4 phases + PRIME-7); London (Warden); Tokyo (Executive). Cyber-collaborator enemy variant for Tokyo. |
| `app/act4_lab_zero.{h,cpp}` | L45 | Floor-by-floor descent (F7→F1) + 3 sub-levels; emotional Floor-2 confrontation; **The First** 3-phase boss; Destroy/Absorb/Negotiate choice. |
| `app/act4_rts.{h,cpp}` | L46 | NEW gameplay mode: 11-front RTS; 4 abilities (Focus Fire / Reinforce / Orbital Strike / Rally); commander selection (timeline-dependent). |
| `app/act4_mothership.{h,cpp}` | L47/L48 | L47 3-approach + Fleet Admiral boss; L48 4-section infiltration + **Ship's Heart** boss. Uses S12 EVA. |
| `app/act4_gauntlet.{h,cpp}` | L49 | 5-boss rush (General/Executive/Scientist/Prophet/Monarch), each with Kill/Deprogram/Imprison choice; Beta substitution. |
| `app/act4_finale.{h,cpp}` | L50 | 4-phase Overlord fight (Avatar / Swarm / Truth / Choice); reality-distortion effects; alternate-Jake summons (4 variants); 4-choice ending fork. |
| `app/endings.{h,cpp}` | Cross-cutting | The 12 endings: requirement evaluation reads AP + timeline + cross-act flags; cinematic playback driver; post-credits scene scripts. |
| `app/cinematic.{h,cpp}` | Cross-cutting | Cutscene playback system at 6–8 minute scale (currently engine has none). |

### 7.5 Open content gaps (not just code) at implementation time

- **Ending cinematic scripts** for Endings 2–10 (only 1 and 11 are fully scripted in design).
- **L43 London Underground** detailed layout (only outlined).
- **L44 Tokyo Neon Nightmare** detailed layout (only outlined).
- **L47 Mothership Approach** detailed layout (only outlined).
- **L48 Mothership Infiltration** detailed layout (only outlined).
- **Sixth landing zone (L37)** — not in corpus; needs Tim's decision (suggested: Oceania).

---

## 8. Build Order

Ordered by dependency so each phase produces a playable slice and unblocks the next.

### Phase A — Finish Act 2 (3–4 modules)

**A1.** Extend `act2_world` with L9 Singing-Crystals puzzle + Salvari Archive interior + download flow + `puzzle_archive_access` 3-stage authentication. Adds the L9 lore payload that unlocks "47 worlds" knowledge.

**A2.** Extend `act2_caves` for L12 full content: multi-layer descent, Hall of the Conquered (47 holographic world-displays + Mendari final symphony), Sealed Invasion Portal room, Overlord Origin Chamber (reset signal). Add Memory Hunter phase-2 summons + control-swap. **This is the L12 content that L50 finale depends on for the Negotiate path; do not skip.**

**A3.** New module `act2_metropolis` for L16/L17/L18 (Breeder Queen Beta boss at L16; Parliament 4-floor dungeon at L17; resistance HQ hub at L18).

**A4.** New module `act2_spaceport` + `vehicle_combat` for L19/L20 (Planetary Defense Commander, Planetary Garrison Commander 3-phase, Overlord Adjutant; vehicle combat intro; boarding-sequence + ascent turret-defense). Closes Act 2.

### Phase B — Storm Runner hub + Act 3 opener (2 modules)

**B1.** New module `act3_storm_runner` — author the 4-deck ShipManifest, wire Bridge nav-station → `SpaceLayer::requestWormhole()`, install the Crystal Heart Interface (gated by L12 `m_crystalHeart`), wire Mess Hall morale + Captain's Quarters relationship scenes. This is the persistent hub for L21–L34, so build it first. Also exercises S5/S6/S7 in a real scenario for the first time.

**B2.** New module `act3_departure` for L21 — atmospheric ascent (S4 reverse), orbital skirmish (S8/S9/S10), first FTL (S3). Plus `act3_asteroids` for L22 (asteroid field + Void Rays + EVA mining). This proves the space-engine subsystems work end-to-end in real content.

### Phase C — Act 3 mid-arc (4 modules)

**C1.** `act3_salvari_prime` for L23/L24/L25 (Memory Hunter variant + Salvari Prime Archive unlocks `m_allDatabases` half).

**C2.** `act3_mining_colony` for L26.

**C3.** `act3_casino` for L27/L28 (poker mini-game + Oracle Beta boss).

**C4.** `act3_rebel_alliance` for L29/L30/L31 (rebel base interior = second Huge ShipManifest; Crystal Heart Installation reads L12 flag).

### Phase D — Act 3 finale (3 modules)

**D1.** `act3_fleet_assembly` (L32) + `act3_warp_gate` (L33) + `act3_en_route` (L34 with Omega wedding) + `act3_sol_arrival` (L35).

**D2.** `dialog_tree` cross-cutting module (now mandatory — all Act 3 NPC content needs it).

**D3.** `alliance_points` cross-cutting (now mandatory — Act 4 endings depend on it; backfill AP contributions for Acts 1–3).

### Phase E — Act 4 Earth liberation (5 modules)

**E1.** `act4_orbital` (L36 Dreadnought) + `act4_descent` (L37 landing-zone choice — close the 6th-continent question first).

**E2.** `act4_base_establish` (L38 base-building economy) + `act4_regional` (L39–L41 trilogy).

**E3.** `act4_cities` (L42–L44 NYC/London/Tokyo).

**E4.** `act4_lab_zero` (L45 — closes the L1/L45 narrative arc; The First boss).

**E5.** `act4_rts` (L46 RTS) + `act4_mothership` (L47/L48).

### Phase F — The Finale (2 modules + cinematic system)

**F1.** `cinematic` cross-cutting module (6–8 minute playback).

**F2.** `act4_gauntlet` (L49 boss rush; Beta substitution).

**F3.** `act4_finale` (L50 4-phase Overlord; reality-distortion effects; alternate-Jake summons; 4-choice ending fork).

**F4.** `endings` (the 12-ending playback driver, reads AP + timeline + cross-act flags + the L50 choice).

### Critical path summary

```
A1 → A2 → A3 → A4   (Act 2 done)
         ↓
B1 → B2             (Storm Runner + Act 3 opener)
         ↓
C1 → C2 → C3 → C4   (Act 3 mid)
         ↓
D1 + D2 + D3        (Act 3 finale + cross-cutting infrastructure)
         ↓
E1 → E2 → E3 → E4 → E5  (Act 4 Earth)
         ↓
F1 → F2 → F3 → F4   (Finale + endings)
```

Estimated module count: **~28 new module pairs + ~6 extensions to existing modules.** Each module carries a `--test-actNN-xxx` self-test in the existing X3Native pattern.

### Self-tests to land alongside

- `--test-act2-archive` (L9 archive download flow)
- `--test-act2-l12-full` (multi-layer descent, Hall of the Conquered, Overlord Origin)
- `--test-act2-metropolis` (Breeder Queen boss + Parliament 4-floor)
- `--test-act2-spaceport` (Garrison Commander 3-phase + boarding sequence)
- `--test-vehicle-combat`
- `--test-act3-storm-runner` (4-deck ShipManifest reachability)
- `--test-act3-departure`, `--test-act3-asteroids`, `--test-act3-salvari-prime`, ...
- `--test-act3-casino-oracle` (Beta-only — flip the gate, rebuild, assert Oracle present)
- `--test-act3-warp-gate`
- `--test-alliance-points` (AP accumulation across all Act 1/2/3 contributors)
- `--test-act4-orbital-dreadnought`, `--test-act4-rts`, ...
- `--test-act4-finale` (4-phase Overlord; choice graph; ending selection from AP+timeline+flags)
- `--test-endings` (all 12 endings reachable from valid precondition sets)

---

## Appendix: open questions for Tim

1. **6th landing zone (L37):** corpus has 5. Suggested: Oceania (Sydney). Confirm or specify a different continent.
2. **L43/L44:** sequential after L42 (current default) or branches gated by L46 RTS results?
3. **L49 Beta substitution:** if all three transformed women are dead by L28, does L49 reuse the most recent via Memory Echo, or is the substitution simply skipped (use the standard 5-boss roster)?
4. **L31 Rebel Base interior:** how identical should it be to the Storm Runner manifest? Is it a 4-deck mirror or a 6-deck expanded version (more rooms reflecting the alliance scale)?
5. **L34 wedding officiant:** Dr. Chen (if saved) or K'thara — confirm precondition order and that this is a single dialog branch.
6. **L46 RTS commanders:** all five listed (Aria/Keisha/Emily/Sarah/K'thara) — is each commander unlocked by surviving Act 1 OR by AP threshold? (Implied: surviving Act 1, but spec is ambiguous on Omega-timeline Aria/Keisha/Emily who weren't rescued.)
7. **L50 Phase-3 alternate-Jake summons:** four variants listed (Corrupted / Failed / Tyrant / Absorbed). Which variants appear conditionally on timeline (e.g. Absorbed only if Ending 11 path is live)? Default suggestion: always all four.

---

End of EFLZ_ACTS_2_4_GAPS.md
