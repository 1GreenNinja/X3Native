# Escape From Lab Zero (EFLZ) — Distilled Design Overview

> A navigable **map of the ~940-page EFLZ production bible**, distilled for the X3Native engine team.
> Purpose: tell the engine team *what game we are building* so engine work (slices) can be aimed at real content.
>
> **Source bible:** `G:\Unity_Projects\PARALLEL_TASKS_8_CHATS\` (READ-ONLY). This doc paraphrases
> intent, mechanics, and structure — no Unity C# is reproduced. Each section cites its bible source(s).
> Working title in the bible alternates between **"Escape From Lab Zero"** and **"Escape Lab 48"**;
> they are the same project. Treat **EFLZ** as canonical.
>
> Genre: single-player first-person sci-fi **horror-action** with light RPG systems (skills, crafting,
> companions, branching timelines). 50 levels across 4 acts. Rated mature.

---

## 1. Premise & Story

*Sources: `Floor1_AwakeningManager.cs`, `Floor2_MedicalBayController.cs`, `TASK_7_DIALOGUE_CUTSCENES.md`, `TASK_7_ESCAPE_LAB_48_COMPLETE_DIALOGUE_SCRIPTS.md`, `TASK_6_ESCAPE_LAB_48_COMPLETE_ENEMY_BESTIARY.md`.*

**Jake Hunter** wakes on a slab in a detention cell of an underground facility — **Lab Zero** — with no memory of
how he got there. A monitor reads *"PHASE 1 COMPLETE — Subject 7-Alpha: 400% strength increase achieved."* He has
been experimented on: he is now superhumanly strong. He crushes the medical equipment by accident, bends the cell
bars, and rips the door off its hinges. An alarm sounds: *"Subject 7-Alpha awakening confirmed. Termination squad
en route."* His one goal: find **Sarah**, his girlfriend, a brilliant hacker also held in the facility.

The facility is a front for an alien program. A collective hive intelligence — **the Overlord**, which has consumed
47+ civilizations — is using Lab Zero to perfect a human breeding/infection program before invading Earth. **Dr. Chen**,
a corrupted (and partly remorseful) scientist, is the on-site architect/documentarian of the horror. Jake fights up
through the facility's floors, can rescue infected captives (Aria, Keisha, Emily) against live transformation timers,
finds Sarah on Floor 7 guarded by **Jake's Clone**, then escapes onto an alien surface, builds a galactic alliance,
and returns to liberate an occupied Earth.

**Key characters**
- **Jake Hunter** — protagonist, 400% enhanced strength, protective, military bearing. Base HP 150, speed 8.
- **Sarah** — Jake's partner; genius hacker; held on Floor 7; becomes a companion (her hacking unlocks doors/drones).
- **Aria, Keisha, Emily** — Floor 2 captives. Saved → companions (healer / warrior / tech). Lost → return as bosses.
- **K'thara** — Salvari (alien) commander; alliance leader and possible romance; captains the ship *Storm Runner*.
- **Dr. Chen** — corrupted scientist seeking redemption; boss and/or ally depending on choices.
- **Jake's Clone** — cold, "optimized" copy loyal to the Overlord; Act 1 climax boss; holds Sarah.
- **The Overlord** — collective consciousness across thousands of worlds; final antagonist (4-phase finale).

**The timeline / branching spine.** Floor 2's triple timed rescue (plus Sarah's background timer) sets a
**timeline** that colors the rest of the 50-level campaign:
- **OMEGA** — perfect run, everyone saved; Jake + Sarah (strongest bond).
- **ALPHA** — 2-3 of the women saved, Sarah lost/barely saved; polyamorous "family" forms.
- **BETA** — Sarah saved first, the three women transform into recurring bosses (The Siren / Breeder Queen / The Oracle).

The game advertises **12 endings** ranging from "Perfect Liberation" to "Jake becomes the new Overlord" to "Total Failure."

---

## 2. Act / Level Structure (all ~50 levels)

*Sources: `Floor1_AwakeningManager.cs`, `Floor2_MedicalBayController.cs`, `TASK_1_FLOORS_3-4_IMPLEMENTATION.md`,
`TASK_2_FLOORS_5-7_IMPLEMENTATION.md`, `Level_08..Level_20_*.json`, `TASK_3_LEVELS_8-20_OPENWORLD.md`,
`TASK_4_LEVELS_21-35_SPACE.md`, `TASK_5_LEVELS_36-50_EARTH.md`.*

> ⚠️ **Numbering note.** Act 1 is described as **7 "Floors"** (the facility) but the bible counts them as
> **Levels 1-7** in some files. There is also a "Floor 4.5 Nexus / Chorus" mentioned as already-existing.
> Floors map to Levels roughly 1:1. See §7 *Inconsistencies* for the Martinez/Chen boss-placement conflict.

### Act 1 — Lab Zero (the facility), Floors / Levels 1-7
| # | Floor / Level | One line |
|---|---|---|
| 1 | **Awakening — Detention** | Wake as Subject 7-Alpha, discover strength, escape the cell, fight to an elevator. Boss: **Chief Martinez** (security). |
| 2 | **Medical Bay** | The triple **timed rescue** (Aria/Keisha/Emily) + Sarah's background timer — sets the timeline. Dr. Chen broadcasts taunts; infection timer + breeding chambers introduced. |
| 3 | **Genetics Lab** | Hybrid-horror specimens, breeding pods, environmental storytelling. Boss: **Failed Experiment #7** (Jake's tragic predecessor). |
| 4 | **Cybernetics Workshop** | Human-machine fusion; optional augments cost "humanity." Boss: **The Collective / The Chorus** (5 merged scientists). Unlocks **Chaingun**. |
| 5 | **Drone Manufacturing (DroneStation)** | Wireframe drones, swarm AI. **Sarah's 90-second master hack** converts enemy drones to allies. Boss: **Swarm Controller AI**. |
| 6 | **Alien Technology Lab** | First contact with **Salvari** refugees; learn the infection **cure**; meet **K'thara**. Unlocks **Lightning Gun**. |
| 7 | **Executive Lab** | Rescue **Sarah** against a recombination timer. Boss: **Jake's Clone**. Timeline (Alpha/Beta/Omega) locks here. |

### Act 2 — Alien World (open-world, Levels 8-20)
*Source `.json` files exist for Levels 8-10, 12-20.*
| # | Level | One line |
|---|---|---|
| 8 | **Surface Emergence** | Escape the collapsing facility into alien daylight (two suns, purple sky). |
| 9 | **Crystalline Desert — Edge** | Harsh survival; singing crystals; first native creatures. |
| 10 | **Crystalline Desert — Depths** | Find planet-side Salvari refugee camps; learn of the planetary invasion. |
| 11 | **Crystalline Desert — Salvari Camp** | Integrate with 200 refugees; alien equipment upgrades. |
| 12 | **Advanced Cave System** ⭐ | Multi-layer ruins; deep lore (47 conquered worlds); Crystal Heart chamber; boss **Memory Hunter**. |
| 13 | **Toxic Swamplands — Edge** | Failed-terraforming mutation horror; poison/suit mechanics. |
| 14 | **Research Station** | What went wrong; mutated scientists. *Beta:* **The Siren (Aria)** ambush. |
| 15 | **Tree Cities** | Vertical traversal; trading post; resistance contact; upgrades. |
| 16 | **Ruined Metropolis — Outskirts** | Fallen alien city ruins; scavengers. *Beta:* **Breeder Queen (Keisha)** territory. |
| 17 | **Downtown** | Skyscraper exploration; central AI lore dump; heavy combat. |
| 18 | **Underground Resistance** | Multi-species alliance forms; companion development. |
| 19 | **Spaceport Assault / Approach** | Large-scale + vehicle combat; Defense Commander mini-boss. |
| 20 | **The Spaceport** ⭐ (Act 2 finale) | Capture a ship and leave the planet; boss **Garrison Commander**. Ship choice affects Act 3. |

### Act 3 — Space Journey (Levels 21-35)
*Hub ship: the **Storm Runner** (Salvari vessel). Source: `TASK_4_LEVELS_21-35_SPACE.md`.*
| # | Level | One line |
|---|---|---|
| 21 | Departure | First spaceflight; orbital battle; ship-hub tutorial. |
| 22 | Asteroid Field | Fuel mining; harmless "Void Rays"; pirate cache. |
| 23-25 | Salvari Prime trilogy | K'thara's devastated homeworld; archives (Overlord's true nature); boss **Memory Hunter** in Memorial City. |
| 26 | Mining Colony Liberation | Free 200+ enslaved workers; recruit specialists. |
| 27-28 | Space Casino "Fortune's End" | Espionage + gambling for fleet intel. *Beta:* **The Oracle (Emily)** assassination attempt. |
| 29-31 | Rebel Base trilogy | Build the alliance; best upgrades; Crystal Heart install. |
| 32 | Fleet Assembly | Prepare the liberation fleet. |
| 33 | Warp Gate Assault | Capture the warp gate to Earth. |
| 34 | En Route to Earth | Relationship resolution; *Omega:* the wedding scene. |
| 35 | **Sol System Arrival** ⭐ (Act 3 finale) | Earth in sight; invasion already in progress; choose landing zone. |

### Act 4 — Earth Liberation (Levels 36-50, the climax)
*Source: `TASK_5_LEVELS_36-50_EARTH.md` (+ expanded production guides). Earth invaded 3 months prior.*
| # | Level | One line |
|---|---|---|
| 36 | Orbital Assault | Space battle to break the blockade; boss **Overlord Dreadnought**. |
| 37 | Atmospheric Entry | Fiery descent; choose a landing continent. |
| 38 | Base Establishment | First boots on Earth; contact fragmented resistance. |
| 39-41 | Regional Liberation (NA / Europe / Asia) | Liberate first major regions; civilian rescue. |
| 42 | **New York Reclamation** ⭐ | Iconic urban warfare; boss **Sector Commander**. |
| 43 | London Underground | Tunnel warfare through the Tube network. |
| 44 | Tokyo Neon Nightmare | Cyberpunk-horror vertical combat; cyber-collaborators. |
| 45 | **Return to Lab Zero** ⭐⭐ | Full circle — descend Floors 7→1, destroy the original infection source. |
| 46 | Global Counter-Offensive | All allies attack at once; strategic command. |
| 47-48 | Overlord Mothership (approach + infiltration) | Board and sabotage the orbital command ship. |
| 49 | Proto-Overlord Gauntlet | Boss rush of corrupted Earth leaders (+ Beta finale: Siren/Queen/Oracle). |
| 50 | **The Overlord Core** ⭐⭐⭐ (finale) | 4-phase final fight (Avatar → Swarm → Truth → Choice); branches into **12 endings**. |

---

## 3. Core Gameplay Loop

*Sources: `Floor1_AwakeningManager.cs`, `TASK_6_*_BESTIARY.md`, `TASK_11_*` skill trees, `TASK_12_*` crafting,
`DoorOverridePuzzle.cs`, `MagneticLockPuzzle.cs`, `VentilationPuzzle.cs`, `RescueVictim.cs`, `RescueHUD.cs`.*

Moment-to-moment, the player:
1. **Explores** sci-fi/alien spaces (corridors, labs, open biomes, ships) — environmental storytelling everywhere
   (logs, photos, video footage, drawings) feeds the lore.
2. **Fights** with melee (super-strength punches, charged "super punch", grab-and-throw) and ranged weapons
   (pistol → chaingun → alien guns), hitscan + projectile. Headshots, weak points, hit-flash feedback, death pops.
3. **Uses super-strength** as a verb: bend bars, rip doors, throw props, **brute-force locked doors** (loud — alerts enemies),
   stagger heavies.
4. **Solves puzzles**: **door-override** (4-digit rotating code *or* strength brute-force), **magnetic-lock**,
   **ventilation routing**, and **Sarah's timed hacks** (multi-phase, defend-while-hacking).
5. **Rescues** captives against **live countdown timers** (the defining Floor-2 mechanic) — success = companion,
   failure = the victim becomes a boss later. The **RescueHUD** shows simultaneous timers.
6. **Manages timers/infection**: medical drones can inject a 9-minute transformation timer; cure stations reset it.
7. **Grows the character**: skill trees (Jake: *Brute Force / Tactical / Survivor*; 2 pts/level, cap 50, 100 pts),
   companion skill trees, **crafting** (150 recipes; melee, ranged, armor, consumables, ammo, upgrades, ship parts),
   an **economy** (credits + ~12 resource types, vendors, crafting stations), achievements, and side quests.
8. **Makes choices** that move the **timeline** (Omega/Alpha/Beta) and feed toward one of 12 endings.

The **vertical-slice spine** the engine is already chasing — *walk → press button → open door → pick up weapon →
shoot monster* — is literally the opening minutes of EFLZ Level 1.

---

## 4. Enemy Roster (from the bestiary)

*Source: `TASK_6_ESCAPE_LAB_48_COMPLETE_ENEMY_BESTIARY.md` (the full ~2,100-line version; the short
`TASK_6_ENEMY_BESTIARY.md` is the brief). Stats below are the bible's; the engine should treat them as tuning targets.*

### Act 1 — Lab Zero
- **Security Guard (Basic)** — patrolling human; pistol bursts + baton; can flee/surrender; calls backup. *First enemy.*
- **Security Guard (Elite)** — ex-military; squad tactics; no surrender.
- **Security Guard (Riot Shield)** — heavy front shield (destructible 500 HP); legs exposed.
- **Lab Scientist (Panicked)** — non-combatant; can be ignored/threatened/rescued/interrogated.
- **Lab Scientist (Corrupted)** — hostile; syringes, scalpels, chemical throws; clarity interrupts.
- **Surveillance Drone** — recon quadcopter; taser + alert broadcast; hackable.
- **Combat Drone** — twin plasma cannons; flanks, kamikazes; hackable.
- **Medical Drone** — *critical threat:* nanite injection starts the 9-min infection timer; hackable into a healer.
- **Breeder Drone** — heavy cargo drone; captures (esp. female) targets; 30s to rescue; hackable.
- **Hacker Drone** — Sarah's converted ally only; disables turrets/doors/other drones.
- **Infected Human (Stage 1/2/3)** — escalating transformation: erratic clawers → wall-climbing ambushers → telepathic "hybrid" commanders. Stages 1-2 curable.

### Act 2 — Alien World
- **Crystal Stalker** — burrowing crystalline ambush predator; pack hunter.
- **Sand Burrower** — giant 50-ft worm; attracted to vibration; weak underbelly.
- **Swamp Stalker** — mutated amphibian; toxic attacks; regenerates in water.
- **Toxic Hulk** — mini-boss apex swamp predator; gas emissions.
- **Salvari Scavenger (Hostile)** — corrupted/desperate aliens; can sometimes be converted (if K'thara present).
- **Overlord Overseer** — psychic commander; barriers, mind shock, summons.
- **Salvari Scout / K'thara** — potential allies (K'thara becomes a full companion).

### Act 3 — Space Journey
- **Pirate Raider** / **Captain Vex** — zero-G human criminals; can be bribed/recruited.
- **Overlord Shock Trooper / Elite Warrior** — disciplined alien soldiers; never surrender; buff auras.
- **Overlord Fighter Craft** — vehicle enemy fought from ship turrets.
- **Corrupted Crew / Station Commander** — infected station personnel; operate hazards.

### Act 4 — Earth
- **Collaborator Militia / Enforcer** — human traitors; militia can surrender/convert, enforcers cannot.
- **Infected Soldier** — trained military + feral switching; curable.
- **Infected Tank** — 2,500 HP armored vehicle; treads/rear/hatch weak points.
- **Overlord Heavy Trooper / Assassin** — slow devastating heavy / cloaking stealth killer.

### Major bosses (by act)
- **Act 1:** Chief Martinez · Dr. Chen · Failed Experiment #7 · The Collective/Chorus · Swarm Controller AI · Jake's Clone.
- **Act 2 (Beta):** The Siren (Aria) · Breeder Queen (Keisha) · The Oracle (Emily).
- **Act 3:** Memory Hunter (psychological-warfare boss).
- **Act 4:** Proto-Overlords (The General / Scientist / Priest / Executive / Prophet) → **The Overlord** (4-phase finale).

---

## 5. Weapon & Item Roster

*Sources: `docs/ASSET_INVENTORY.md` (S5 weapon stats), `TASK_12_*_CRAFTING_SYSTEM.md`, bestiary boss drops.*

**Weapons (acquisition roughly by act):**
| Weapon | Stats (bible) | Notes |
|---|---|---|
| **Super-strength melee** | punch / charged super-punch / grab-throw | Always available; Jake's signature verb. |
| **Energy Pistol** | 15 dmg, 3/s, mag 12, 50 m, hitscan | Starting firearm (Floor 1 armory). GLB exists. |
| **Shotgun** | 20/pellet ×8, 1/s, 15 m | Close-range. GLB exists. |
| **Chaingun** | tier-4 high-ROF auto | Drops from The Collective (Floor 4). |
| **Lightning Gun** | continuous beam, chains 3 targets | Salvari tech (Floor 6). |
| **Bazooka / Rocket Launcher** | 150 dmg + 100 AOE r=5 m, 200 m | Heavy; GLB exists. |
| **Railgun / BFG** | (high-tier) | GLBs exist; later-act/crafted. |
| **Plasma Pistol/Rifle, Plasma Blade** | craftable | Tiered crafting recipes. |

**Items / consumables / economy:** health kits, medkits, ammo types, antidotes/cure items, infection cure stations,
keycards (Blue/Floor keycards gate progress), evidence/lore collectibles, **credits** + ~12 crafting resources
(scrap, precision parts, plasma canisters, fusion cores, bio-crystal, Salvari alloy, etc.), crafting stations and
vendors, ship-upgrade parts (Act 3+).

---

## 6. Key Mechanics & Systems (high level)

*Sources: `DoorOverridePuzzle.cs`, `MagneticLockPuzzle.cs`, `VentilationPuzzle.cs`, `RescueVictim.cs`,
`RescueHUD.cs`, `TASK_8_TECHNICAL_SYSTEMS*.md`, `TASK_10..14` (side quests / skills / crafting / achievements / audio).*

- **Super-strength interactions** — bend/rip/throw; **brute-force** locks (fast but loud → alerts).
- **Puzzles** — *Door-override* (4-digit rotating code, limited attempts, wrong attempt alerts patrols, OR brute-force);
  *Magnetic-lock* (power-routing/sequence); *Ventilation* (route airflow/path). All gate doors or rescue rooms.
- **Timed rescue system** — multiple simultaneous countdowns (RescueHUD); outcome branches the timeline; failed
  rescues respawn as bosses.
- **Infection timer** — drone injection starts a 9-minute clock with escalating debuffs; cure stations reset it.
- **Hacking (Sarah)** — multi-phase timed hacks; convert enemy drones to allies; open doors; extract data.
- **Companions** — Sarah, Aria, Keisha, Emily, K'thara, etc.; each has a **skill tree**, combat barks, and arcs.
- **Skill trees & leveling** — Jake (Brute Force / Tactical / Survivor), 2 pts/level, max level 50.
- **Crafting & economy** — ~150 recipes across 9 categories; resources + credits; crafting stations; vendors.
- **Branching narrative** — Alpha/Beta/Omega timeline + romance options → **12 endings**.
- **Side content** — side quests (TASK_10), achievements (TASK_13), full audio/music design (TASK_14).
- **Ship hub (Act 3+)** — Storm Runner with decks for command/crew/ops/Salvari-tech; space combat.

---

## 7. Where the bible is huge / inconsistent (read before building)

- **Scale.** ~940 pages across ~50 files; some Markdown files are 100K-390K chars. I **read fully**: the master index,
  Floor 1 & Floor 2 managers, all five structural TASK files (1-5), the full bestiary, the dialogue/cutscene brief,
  the missing-scripts spec, the asset inventory, and the door-override puzzle. I **sampled**: the giant
  `TASk_2_FLOORS_5-7_COMPLETE_IMPLEMENTATION.md` (391K), the complete dialogue scripts (134K), TASK_8 technical
  parts, and TASK_10-14 (side quests / skill trees / crafting / achievements / audio) — enough for breadth, not line-by-line.
- **Boss-placement conflict (Act 1).** The implemented **`Floor1_AwakeningManager.cs` puts Chief Martinez on Floor 1**
  and the bestiary agrees (Martinez = Floor 1 boss, Dr. Chen = Floor 2 boss). But **`TASK_9_MISSING_SCRIPTS_SPEC.md`
  re-specs Floor 1 with *no* boss (a Dr. Chen *trust choice* instead) and moves **Martinez to Floor 2**, while
  `Floor2_MedicalBayController.cs` makes Floor 2 the triple-rescue with Dr. Chen as a *broadcast* villain (Chen boss
  fight elsewhere). **For Level 1 we follow the implemented `Floor1_AwakeningManager.cs`** (Martinez on Floor 1) and flag this.
- **Rescue-timer values disagree.** Cutscene 2 shows Aria 5:00 / Keisha 7:00 / Emily 4:00; `Floor2_MedicalBayController.cs`
  uses 15:00 / 20:00 / 9:00 with Sarah 30:00. Treat these as tuning placeholders.
- **Naming drift.** "Escape From Lab Zero" vs "Escape Lab 48"; "Floors 1-7" vs "Levels 1-7"; a "Floor 4.5" exists.
- **Format mix.** Act 1 is Unity C# behavior scripts; Acts 2-4 are JSON/Markdown level specs + screenplay dialogue —
  i.e., later acts are *design intent*, not implemented logic. The opening (Act 1) is the most concrete and is the
  right place to start building.
- **AI-generated provenance.** The bible was produced by an "8 parallel Claude chats" workflow (see `00_MASTER_INDEX.md`),
  so cross-file consistency is imperfect by construction. Models/textures/audio are purchased/AI-generated and owned by
  the project (see `docs/ASSET_INVENTORY.md` §7 provenance).

---

## 8. Source-file index (quick map)

| Topic | Bible file(s) |
|---|---|
| Whole-bible map / workflow | `00_MASTER_INDEX.md` |
| Floor 1 opening (Level 1 basis) | `Floor1_AwakeningManager.cs` |
| Floor 2 rescue/timeline | `Floor2_MedicalBayController.cs`, `BreedingChamberRoom.cs`, `RescueVictim.cs`, `RescueHUD.cs` |
| Puzzles | `DoorOverridePuzzle.cs`, `MagneticLockPuzzle.cs`, `VentilationPuzzle.cs` |
| Floors 3-7 design | `TASK_1_FLOORS_3-4_IMPLEMENTATION.md`, `TASK_2_FLOORS_5-7_IMPLEMENTATION.md`, `TASk_2_FLOORS_5-7_COMPLETE_IMPLEMENTATION.md` |
| Act 2 levels (8-20) | `Level_08..Level_20_*.json`, `TASK_3_LEVELS_8-20_OPENWORLD.md` |
| Act 3 levels (21-35) | `TASK_4_LEVELS_21-35_SPACE.md` |
| Act 4 levels (36-50) + endings | `TASK_5_LEVELS_36-50_EARTH.md`, `TASK_5_ESCAPE_LAB_48_ACT4_*.md` |
| Enemies & bosses | `TASK_6_ESCAPE_LAB_48_COMPLETE_ENEMY_BESTIARY.md` |
| Dialogue & cutscenes | `TASK_7_DIALOGUE_CUTSCENES.md`, `TASK_7_ESCAPE_LAB_48_COMPLETE_DIALOGUE_SCRIPTS.md` |
| Tech / shaders / QA | `TASK_8_TECHNICAL_SYSTEMS*.md` |
| Missing-scripts spec | `TASK_9_MISSING_SCRIPTS_SPEC.md` |
| Side quests / skills / crafting / achievements / audio | `TASK_10..TASK_14_*.md` |
| Boss specifics | `DrChenBoss.cs`, `TransformedEnemies.cs` |
| Engine asset mapping | `docs/ASSET_INVENTORY.md` (this repo) |

---

*This overview is a map, not the territory. For the buildable first slice, see `specs/EFLZ_LEVEL_01.spec.md`.*
