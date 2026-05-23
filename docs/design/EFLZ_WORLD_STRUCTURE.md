# EFLZ — Canonical World / Level-Structure Digest

> **Escape From Lab Zero** (a.k.a. "Escape Lab 48") — Tim Smith's 50-level sci-fi
> horror-action game. This is the single canonical map of the WORLD/LEVEL structure,
> synthesized from the full design corpus so the X3Native engine team can build to the
> real game (not a guess).
>
> **Authoring note (provenance & conflicts).** The corpus was produced by an "8 parallel
> Claude chats" workflow plus a shipped web build, so files disagree in places. This doc
> prefers the **most complete / latest** source and **flags conflicts inline**. Every fact
> is cited to its source file. Sources live READ-ONLY on `G:`:
> - Engine-side specs: `docs/MASTER_GAME_PLAN.md`, `docs/EFLZ_DESIGN.md`,
>   `specs/EFLZ_SPIRE_7FLOOR.spec.md`, `specs/EFLZ_LEVEL_01.spec.md`, `specs/ELEVATOR.spec.md`.
> - Design corpus: `G:\Unity_Projects\PARALLEL_TASKS_8_CHATS\` (TASK_1…TASK_9, the Floors3-4
>   RoomLayouts, the 391 KB `TASk_2_FLOORS_5-7_COMPLETE_IMPLEMENTATION.md`, the Act 2/3/4 docs,
>   the Level_08…Level_20 JSON, and `Grok\` drone materials).
> - Merged narrative (latest, most authoritative single prose): `G:\GameDev\Web Escape Lab 48
>   versions\escape-lab-48-v9.881\ESCAPE_FROM_LAB_ZERO_COMPLETE_NARRATIVE_MERGED.md`
>   and `UNITY_CSHARP_ANALYSIS.md` (which lists the actually-shipped C# files, incl.
>   `Floor4_5_NexusChamber_MasterController.cs` and `TheChorusBoss.cs`).

---

## 1. Overview — the 4-act / 50-level spine

The game is canonically **50 levels in 4 acts** (the engine-side `MASTER_GAME_PLAN.md`
proposes a *separate* 100-level expansion in 3 "acts" — that is an aspirational reframe, NOT
the source design; the corpus, the merged narrative, and every TASK file agree on **50/4**).

| Act | Levels | Identity (one line) |
|---|---|---|
| **Act 1 — Lab Zero (the facility)** | "Floors" 1–7 (+ Floor 4.5, + sub-levels) | Vertical breakout up a 7-floor breeding-program lab; ends at Jake's Clone + Sarah rescue. *(Sources: `ESCAPE_..._MERGED.md` §Seven-Floor Structure; `TASK_9_FLOORS_1-2`, `TASK_1_FLOORS_3-4`, `TASK_2_FLOORS_5-7`.)* |
| **Act 2 — Alien World (open world)** | 8–20 | Emerge onto alien planet **Keth'zar Prime**; survive biomes, find the **Salvari** refugees + **K'thara**, build an alliance, capture a ship. *(Sources: `TASK_3_LEVELS_8-20_OPENWORLD.md`; `Level_08…Level_20_*.json`.)* |
| **Act 3 — Space Journey** | 21–35 | Galactic liberation tour aboard the hub-ship **Storm Runner**; raise a fleet; reach Sol. *(Source: `TASK_4_LEVELS_21-35_SPACE.md`.)* |
| **Act 4 — Earth Liberation** | 36–50 | Break the orbital blockade, retake Earth city-by-city, **Return to Lab Zero**, board the Mothership, fight the Overlord → **12 endings**. *(Sources: `TASK_5_ESCAPE_LAB_48_ACT4_LEVELS_36-50_COMPLETE.md`, `TASK_5_LEVELS_36-50_EARTH.md`, `TASK_5_ACT4_EXPANDED_PRODUCTION_GUIDE.md`.)* |

**Premise (canon).** Earth Year 2157. The hive-mind **Overlord** (a collective consciousness
that has consumed 47 worlds; Earth = "Target World 48") runs **Lab Zero** to perfect a human
breeding/infection program. **Jake Hunter** wakes as **Subject 7-Alpha** (+400% strength,
free will intact via a genetic anomaly). His girlfriend **Sarah** (genius hacker) is held on
the Executive Floor for forced breeding with **Jake's Clone**. *(Source: `ESCAPE_..._MERGED.md`
§Core Narrative Premise.)*

**Branching timeline** (set mostly by Act-1 Floor-2 rescues): **OMEGA** (perfect; Jake+Sarah) /
**ALPHA** (the 3 women saved, Sarah lost → poly family) / **BETA** (Sarah saved first, the 3
women transform into recurring bosses Siren/Breeder Queen/Oracle). *(Sources: `EFLZ_DESIGN.md`
§1; `TASK_2`/`TASK_3` timeline context.)*

---

## 2. Act 1 — Floor-by-floor (the facility / "The Spire")

> Act 1 is the most concrete, fully-authored act and the engine's current target. The
> facility is **7 main Floors + an off-elevator Floor 4.5 + late-game sub-levels** (see §3).
> Floors connect by a **central elevator** + emergency stairwells. Vertical heights from the
> Floors3-4 RoomLayouts: F3 at Y=24–36, F4 at Y=36–50 (each floor ~12–14 m tall). The merged
> narrative numbers the breakout as "Floors 1–7"; some TASK files number them "Levels 1–7" 1:1.

### Floor 1 — Detention / Awakening
- **Identity / theme:** Prison break; discovering super-strength. *(`ESCAPE_..._MERGED.md` Floor 1; `TASK_9_FLOORS_1-2`.)*
- **Key rooms:** Medical Pod Chamber (Jake's pod + 6 others, 3 failed subjects), Medical Wing Corridor (4 side rooms incl. morgue), **Dr. Chen's Office** (hidden keycard, family photo), Security Checkpoint (weapon locker = first pistol). *(`TASK_9`.)*
- **Objective:** Escape the medical/detention wing; reach the elevator/stairwell. Find the **pistol** (security locker); a **Bazooka** is in the armory. *(merged + `TASK_9`.)*
- **Boss:** **Security Chief (Marcus) Martinez** — ex-military, basic cyber-augments, "just doing his job"; 2 phases (pistol+flashbang → shotgun); can be killed or spared. *(`ESCAPE_..._MERGED.md`; `ChiefMartinezBoss.cs` profile in `TASK_9`: HP 800.)*
  - ⚠️ **CONFLICT (Martinez floor):** The merged narrative + `Floor1_AwakeningManager.cs` + bestiary put **Martinez on Floor 1**. But `TASK_9_FLOORS_1-2` (the prompt) places the **Martinez boss arena on Floor 2** and gives Floor 1 a Dr. Chen *trust choice* instead, and `TASK_9_MISSING_SCRIPTS_SPEC.md` re-specs Floor 1 with no boss. **Prefer the merged narrative + implemented script: Martinez = Floor 1.** The engine already does this.
- **Dr. Chen beat:** Floor 1 introduces **Dr. Chen** (corrupted scientist, partly remorseful). Trust/distrust choice gates an optional Floor-2 Chen rescue and later story. *(`TASK_9`: `DrChenEncounter.cs`/`DrChenRescue.cs`.)*
- **Notable mechanics:** strength-discovery (crush equipment, bend bars, rip door); tutorials; first guards + early **wireframe surveillance drones**; first save checkpoint.

### Floor 2 — Medical Bay (the signature triage)
- **Identity / theme:** Body horror; **timed moral-choice rescues**; sets the timeline. *(`ESCAPE_..._MERGED.md` Floor 2; `Floor2_MedicalBayController.cs`; `TASK_9`.)*
- **The 3 timed rescues (the game's signature beat):**
  - **Room A — Aria** (medical tech). If lost → boss **"The Siren."**
  - **Room B — Keisha** (security specialist; magnetic-lock puzzle). If lost → boss **"Breeder Queen."**
  - **Room C — Emily Watson** (research assistant; ventilation puzzle). If lost → boss **"The Oracle."**
  - Plus **Sarah's** background timer running the whole act. *(merged §Three Rescue Scenarios.)*
- ⚠️ **CONFLICT (timer values):** merged narrative / cutscenes use **Aria 5:00 / Keisha 7:00 / Emily 4:00**; `Floor2_MedicalBayController.cs` uses **15:00 / 20:00 / Emily 9:00, Sarah 30:00**; `TASK_2` integration note lists **Aria 5 / Keisha 7 / Emily 4 / Sarah 9**. Treat as tuning placeholders; **the 5/7/4 + Sarah-9 set is the most-repeated.**
- **Objective:** Reach Ward A/B/C, save who you can (only 1–2 reachable in time → forced triage), optionally rescue **Dr. Chen** if trusted, reach the stairwell up.
- **Boss:** ⚠️ **CONFLICT.** Merged narrative + `EFLZ_DESIGN.md` bestiary = **Dr. Chen (Corrupted), 3 phases** is the Floor-2 boss ("I only wanted to cure cancer… forgive me"). `TASK_9` instead routes the **Martinez** arena here. **Prefer Dr. Chen = Floor 2 boss** (consistent with Martinez=F1). `DrChenBoss.cs` exists (36 KB).
- **Notable mechanics:** **door-override** (4-digit code OR strength brute-force), **magnetic-lock**, **ventilation** puzzles; **9-minute infection timer** introduced (medical-drone injection; cure stations reset it); **RescueHUD** with simultaneous countdowns; evidence/lore collectibles.

### Floor 3 — Genetics Lab
- **Identity / theme:** Hybrid horror; mass-experimentation evidence; tragedy. *(`TASK_1_FLOORS_3-4`; `Floor3_GeneticsLab_Layouts.md`.)*
- **Key rooms (with coords, from the RoomLayouts):** **Specimen Tank Chamber** (12 hybrid tanks), **Video Log Archives** (Dr. Chen's corruption timeline, keycard-gated, "Maya, daddy's sorry"), **The Nursery** (failed hybrid offspring; music box; strength/hack blast door), **Spawning Chamber** (8 active breeding pods — destroy 3 to trigger boss; central genetic sequencer Sarah can sabotage), **Boss Arena**.
- **Objective:** Push through the genetics wing, destroy breeding pods (else infinite spawns; each pod destroyed = a victim "memory flash"), reach the boss.
- **Boss:** **Failed Experiment #7 = Marcus Webb** — Jake's predecessor, the **first** 400% subject, now feral but retains love for his daughters (Emma/Lily). 3 phases (Rage → Lucidity, chest weak-point exposed → self-destructive Deterioration). Drops **Marcus's ID badge → unlocks Floor 4.** *(`Floor3_GeneticsLab_Layouts.md` boss section; merged Floor 3.)*
- **Notable mechanics:** Memory-flash system; environmental storytelling (children's drawings, teddy bear "For Baby Marcus"); enemies = Stage 1–4 Hybrids.

### Floor 4 — Cybernetics Workshop
- **Identity / theme:** Human-machine fusion; **losing humanity** to augmentation. *(`TASK_1_FLOORS_3-4`; `Floor4_CyberneticsWorkshop_Layouts.md`.)*
- **Key rooms:** **Augmentation Bay** (6 surgical stations — interrupt forced surgery to save a victim; optional Jake augments at a **Humanity cost**; observation window = **first sight of the alien fleet/armada**), **Brain Upload Chamber** (consciousness-transfer server holding 4,728 minds; next subject queued = **JAKE**), **Contract Room** ("volunteer" cubicles; mini-boss **Karen Mitchell**, the Volunteer Coordinator), **The Collective Arena** (dome boss).
- **Objective:** Cross the cybernetics wing, optionally interrupt surgeries / free uploaded minds, beat the boss → **Neural Access Key unlocks Floor 5.**
- **Boss:** **The Collective / The Chorus** — **five scientists merged into one cyber-organism** (Rodriguez/Tanaka/Chen-tech-officer/Lancaster + **Subject Zero** at center). Kill the outer 4 to expose Subject Zero; 4 phases (Unified → Fracturing → Individual → Final Core). Drops **ChainGun** blueprint. *(`Floor4_CyberneticsWorkshop_Layouts.md`; merged Floor 4 "Boss: The Collective"; dialogue scripts call it **"The Chorus"** in the **Nexus Chamber, Floor 4** — see §3 for the Floor-4-vs-4.5 conflict.)*
- **Notable mechanics:** **Augmentation system** + **Humanity meter** (too many augments lock out good endings); brain-upload "personality overwrite"; **Chaingun** unlock.

### Floor 5 — Drone Manufacturing & Control ("DroneStation")
- **Identity / theme:** Wireframe aesthetic; swarm intelligence; **hackable** systems. *(`TASK_2_FLOORS_5-7`; merged Floor 5; the **391 KB** `TASk_2_FLOORS_5-7_COMPLETE_IMPLEMENTATION.md`.)*
- **Key rooms:** Central Command Station, Manufacturing/Repair/Recharge depots, Security (turret) stations, **The Hive Mind Chamber** (central AI holding recorded footage of EVERY prior floor's horrors; optional "broadcast evidence to the world" choice; Sarah's primary hack target).
- **Drone variants (the canonical roster):** Surveillance (blue), Combat (red), **Medical** (green — starts the 9-min infection timer), **Breeder** (orange — captures subjects), Hacker (purple — Sarah's converted allies). *(merged Floor 5.)*
- **Objective / signature beat:** **Sarah's 90-second 3-phase master hack** (Network Access → Override Swarm Controller → Reprogram Allegiance) — succeed and **all drones become permanent allies** ("Drone army"), Sarah gains **Drone Commander**. *(`TASK_2`; merged §Sarah's Master Hack.)*
- **Boss:** **Swarm Controller AI** (Phase 1 Jake-alone destroys 5 nodes; Phase 2 the full hack defense). *(`TASK_2`.)*
- **Notable mechanics:** post-hack **drone shop** (credits/parts economy, drone upgrades); turret networks; weapon unlock **Plasma Rifle**. **This is the engine's "drone level" anchor** (see §4).

### Floor 6 — Alien Technology Lab
- **Identity / theme:** First contact; true scope of the invasion revealed. *(`TASK_2`; merged Floor 6.)*
- **Key rooms:** Salvari Holding Cells, Ancient Technology Archive (lore: 47 conquered worlds), **Cure Synthesis Lab**, **K'thara's Quarters**, Escape Pod Bay (surface transition seed).
- **Objective:** Free / ally with the **Salvari** refugees (**30 survivors from 30 billion**), learn the **infection cure**, meet **K'thara** (Salvari leader; romance option in Beta). First-contact choice: **Trust** (gain cure + allies + weapons) vs **Attack** (lose cure path).
- **Boss:** **Alien Overseer** — true form of the invasion commander ("Your species shows promise for our armies"; reveals the 47-world scope). *(merged Floor 6.)*
- **Notable mechanics:** **Lightning Gun** unlock (Salvari tech, if allied); cure synthesis; Salvari history terminals.

### Floor 7 — Executive Laboratory (the Clone confrontation)
- **Identity / theme:** The emotional core of Act 1; **timed Sarah rescue**; timeline locks here. *(`TASK_2`; merged Floor 7.)*
- **Key rooms:** Executive Suite (Sarah on the table; clone pods/backup bodies; city-view thunderstorm), executive terminal (reveals the **sub-levels** + Chen torture — see §3).
- **Objective:** Stop the Clone, **rescue Sarah** against a **7–9 minute genetic-recombination timer** (outcome tiers: <2 min clean save → 9 min full failure = Sarah becomes Breeder Queen).
- **Boss:** **Jake's Clone** — 3 phases (Phase 1 mirror-match of Jake's 400% moveset → Phase 2 nanite self-injection → Phase 3 desperation, activates other clones). Can be killed or incapacitated (affects ending). *(`TASK_2`; merged.)*
- **Timeline lock:** the save/loss of Sarah (vs the Floor-2 women) finalizes **Alpha / Beta / Omega.**

**Act-1 weapon ladder:** Pistol + Bazooka (F1) → ChainGun (F4 Collective/Chorus) → Plasma
Rifle (F5) → Lightning Gun (F6, Salvari). *(merged per-floor "Weapon Unlock" lines.)*

---

## 3. The off-elevator / in-between / sub-level architecture  ⭐ (what Tim flagged)

This is the crux. There are **three distinct kinds** of "extra" Act-1 space, and they are
NOT the same thing:

### 3a. **Floor 4.5 — the Nexus Chamber (the CHORUS boss floor)**  ← the "Level 4.5" / in-between floor
- **What it is:** A **half-floor / mezzanine "Nexus Chamber"** sitting **between Floor 4 and
  Floor 5**, housing **The Chorus** boss (the five-scientist plural-mind merger). It is an
  **actual shipped, working level** — the v9.881 web build ships
  **`Floor4_5_NexusChamber_MasterController.cs` (605 lines, "Cinematic sequence controller")**
  and **`TheChorusBoss.cs` (1,572 lines)** with a 1,503-line consciousness-rescue minigame.
  *(Source: `UNITY_CSHARP_ANALYSIS.md` lines 273, 449; `TASK_1`/`TASK_2` both list "Floor 4.5
  (Nexus Chamber with Chorus boss) — WORKING / EXISTS" as a do-not-break existing system.)*
- **How it's reached / what gates it:** It is the transition stage between F4 and F5 — you pass
  THROUGH it after the Cybernetics wing (the Nexus is the cybernetics network's core). It is the
  **"in-between" floor the elevator's main floor-list does not advertise as a numbered stop** —
  conceptually "Floor 4.5," accessed off the F4→F5 path rather than as a normal lobby stop.
- **What it contains:** the **Chorus** plural-mind boss fight (`INT. NEXUS CHAMBER - FLOOR 4`,
  five voices in unison; Subject Zero screams "TRAPPED! I DON'T WANT TO BE PLURAL ANYMORE!"),
  blue holographic interfaces, observation deck onto the assembling alien fleet, and a
  **consciousness-rescue minigame**. *(Source: `TASK_7_..._DIALOGUE_SCRIPTS.md` lines 892–943.)*
- ⚠️ **CONFLICT — is the Chorus on Floor 4 or Floor 4.5?** Two readings exist and the corpus
  uses BOTH names for the **same** five-mind boss:
  - **Floor 4 reading:** `Floor4_CyberneticsWorkshop_Layouts.md` + merged narrative name the
    Floor-4 dome boss **"The Collective,"** and the dialogue header literally says
    `THE CHORUS BOSS - FLOOR 4 … Nexus Chamber`.
  - **Floor 4.5 reading:** the **shipped code** isolates it into
    `Floor4_5_NexusChamber_MasterController.cs`, and TASK_1/TASK_2 explicitly say "**Floor 4.5
    (Nexus Chamber with Chorus boss)**."
  - **Reconciliation (recommended canon):** **The Chorus / The Collective is one boss, staged in
    the Nexus Chamber, treated as the half-step "Floor 4.5" that bridges F4 (Cybernetics) → F5
    (DroneStation).** Build it as a discrete "4.5" encounter floor reached off the F4→F5 path,
    NOT as a normal numbered elevator lobby. (This matches Tim's "Level 4.5 the elevator does not
    stop at.")

### 3b. **The Floor-7 SUB-LEVELS (the Return Mission, behind the executive desk)**  ← the "sub-levels"
- **What they are:** A **hidden vertical descent** of **3 sub-levels** revealed only **after the
  Clone is defeated**, when **Sarah** checks the executive terminal and discovers **Dr. Chen is
  alive**, being tortured. *(Source: `ESCAPE_..._MERGED.md` lines 405–510 "THE RETURN MISSION:
  SUB-LEVELS & DR. CHEN'S RESCUE".)*
- **How reached / what gates them:** A **"previously hidden elevator behind the executive desk"
  activates** — gated on **(a) defeating the Clone and (b) having saved Sarah** in time (only
  triggers if Sarah is saved before the clone's completion). You descend, then **fight back UP
  through F7→F1** (reinforcements have arrived; this is also where **Sarah's drone-hack** can be
  performed on the return through F5).
- **The 3 sub-levels:**
  - **Sub-Level 1 — Waste Disposal & Failed Experiments:** incinerator chambers, still-living
    failed hybrids begging for death, toxic-gas vents; Disposal Drones + Corrupted Janitors;
    horror "children's section."
  - **Sub-Level 2 — Cryogenic Storage:** hundreds of subjects in stasis; **choice: wake them
    (gain allies) or leave frozen**; boss **The Frozen Collective** (merged cryo-subjects); Sarah
    can selectively wake pods.
  - **Sub-Level 3 — Enhanced Interrogation:** **Dr. Chen's torture chamber** (regeneration device
    forcing endless pain); the Overlord's true experiments revealed; Chen rescue ("Earth has 72
    hours…"). *(merged lines 423–509.)*

### 3c. **Spec-team "in-between layers" + deep tunnels (engine-side reframe)**
- The engine spec `EFLZ_SPIRE_7FLOOR.spec.md` adds its OWN traversal layers that are a *design
  reframe*, not from the original corpus: **deep underground tunnels below B1** (flooded
  service tunnels → power core → hidden boss → ties to the Act-2 underground), **in-between
  mezzanines/vents/service voids** between every floor (the strength-and-traversal layer:
  pry vents, punch through weak floors to skip a front-door fight), and the **"Club 1127"**
  hidden hub (keypad code **1127**) from `MASTER_GAME_PLAN.md`. *(Sources:
  `EFLZ_SPIRE_7FLOOR.spec.md` §2b–2d; `MASTER_GAME_PLAN.md` §Secrets.)*
- **Note:** Club 1127 and the B1 deep tunnels are **Tim's engine-side additions**, not present in
  the original 50-level corpus. Keep them clearly separated from the canon **Floor 4.5** and the
  **Floor-7 sub-levels**, which ARE in the original design.

### 3d. The "Return to Lab Zero" callback (Act 4, Level 45) — sub-levels reappear
- Late-game **Level 45 "Return to Lab Zero"** descends the mutated facility **F7→F1** and adds
  **3 NEW sub-levels**: **Sub-Level 1 Breeding Program Core** (47,000 humans processed),
  **Sub-Level 2 Chen's True Laboratory** (complete cure if Chen was rescued), **Sub-Level 3
  Original Artifact** (a **65-million-year-old stasis pod** — the asteroid that killed the
  dinosaurs was an Overlord seeding pod). Floor 4 there is listed as **"The Chorus remains
  (expanded organic structure)"** — confirming the Chorus = the Floor-4/4.5 boss. Boss: **The
  First.** *(Source: `TASK_5_ESCAPE_LAB_48_ACT4_LEVELS_36-50_COMPLETE.md` Level 45.)*

---

## 4. Acts 2–4 level list (8–50)

> Names/biomes from `TASK_3` / `TASK_4` / `TASK_5` and the `Level_08…Level_20` JSON. Where the
> shipped JSON disagrees with the prompt, the JSON is noted (it is the more authoritative
> per-level data). **The DRONE LEVEL and the CHORUS BOSS are called out explicitly below and in §3.**

### Act 2 — Alien World (open world), Levels 8–20 — planet **Keth'zar Prime**
| # | Name | Biome | Boss / objective |
|---|---|---|---|
| 8 | Surface Emergence | Underground→Surface transition | Escape collapsing facility into alien daylight (two suns, purple sky). No boss. *(`Level_08_SurfaceEmergence.json`.)* |
| 9 | Crystalline Desert — Edge | Crystal desert | Survival; singing crystals; first native creatures. *(`Level_09…json`.)* |
| 10 | Crystalline Desert — Depths | Crystal caves | Find planet-side **Salvari** camps; learn of the invasion. *(`Level_10…json`.)* |
| 11 | **Salvari Camp** ("Refugee Haven") | Refugee outpost | Hub: 500+ Salvari; recruit **K'thara**; trading/crafting; faction-alignment choice; **camp holds prototype drone hacks (ties to the Crazy Drone)**. *(`Grok\Level_11_SalvariCamp.json`.)* |
| 12 | **Advanced Cave System** ⭐ | Ancient ruins | Multi-layer ruins; 47-conquered-worlds lore; **Crystal Heart** chamber (Jake-strength + Sarah-hack); boss **Memory Hunter**. *(`Level_12…json`; `TASK_3`.)* |
| 13 | Toxic Swamplands — Edge | Toxic swamp | Failed-terraforming mutation horror; poison/suit mechanics. *(`Level_13…json`.)* |
| 14 | Research Station | Swamp station | What went wrong; mutated scientists. **Beta:** **The Siren (Aria)** ambush. *(`Level_14_ResearchStation.json`.)* |
| 15 | Tree Cities | Canopy city | Vertical traversal; trading post; resistance contact. *(`Level_15_TreeCities.json`.)* |
| 16 | Ruined Metropolis — Outskirts | Fallen alien city | Scavengers. **Beta:** **Breeder Queen (Keisha)** territory. *(`Level_16…json`.)* |
| 17 | Downtown | Skyscrapers | Central-AI lore dump; heavy combat. *(`Level_17_Downtown.json`.)* |
| 18 | Underground Resistance | Resistance HQ | Multi-species alliance forms; companion arcs. *(`Level_18…json`.)* |
| 19 | Spaceport Assault / Approach | Spaceport exterior | Vehicle combat; Defense Commander mini-boss. *(`Level_19_SpaceportAssault.json`.)* |
| 20 | **The Spaceport** ⭐ (Act 2 finale) | Spaceport | Capture a ship (Storm Runner); boss **Garrison Commander**. ⚠️ JSON titles it **"Escape From Kethzar Prime"** (`Level_20_EscapeFromKethzarPrime.json`) — same level. Ship choice affects Act 3. |

### Act 3 — Space Journey, Levels 21–35 (hub: **Storm Runner**) — *(`TASK_4`)*
| # | Name | Setting | Boss / objective |
|---|---|---|---|
| 21 | Departure | Orbital escape | First spaceflight; orbital battle; ship-hub tutorial. |
| 22 | Asteroid Field | Asteroids | Fuel mining; harmless beautiful **Void Rays**; pirate cache. |
| 23–25 | Salvari Prime trilogy | K'thara's dead homeworld | Surface → Archives (Overlord's true nature) → Memorial City; boss **Memory Hunter**. |
| 26 | Mining Colony Liberation | Colony | Free 200+ enslaved workers; recruit specialists. |
| 27–28 | Space Casino "Fortune's End" | Station | Espionage/gambling for fleet intel. **Beta:** **The Oracle (Emily)** assassination attempt. |
| 29–31 | Rebel Base trilogy | Asteroid base | Build the alliance; best upgrades; Crystal Heart install. |
| 32 | Fleet Assembly | Base | Prepare the liberation fleet. |
| 33 | Warp Gate Assault | Space | Capture the warp gate to Earth. |
| 34 | En Route to Earth | Storm Runner | Relationship resolution; **Omega:** wedding scene. |
| 35 | **Sol System Arrival** ⭐ (Act 3 finale) | Sol | Earth in sight; invasion in progress; choose landing zone. |

### Act 4 — Earth Liberation, Levels 36–50 — *(`TASK_5_..._ACT4_..._COMPLETE.md`)*
| # | Name | Biome | Boss / objective |
|---|---|---|---|
| 36 | Orbital Assault "The Return" | Earth orbit | Break the blockade; boss **Overlord Dreadnought "Extinction"** (HP 50,000). |
| 37 | Atmospheric Entry "Falling Home" | Descent | Fiery descent; choose landing continent (5 options). |
| 38 | Base Establishment "First Foothold" | Landing zone | Base building; boss **The Broadcaster** (HP 3,000). |
| 39–41 | Regional Liberation trilogy | NA/EU/Asia/Africa/SA | Player-ordered; each = Urban Warfare / Breeding-Center / Military-Base; regional commander bosses. |
| 42 | **New York Reclamation** ⭐ | Occupied NYC | Iconic urban warfare; boss **PRIME-7 (Sector Commander)** (HP 10,000). |
| 43 | London Underground | Tube tunnels | Tunnel warfare (outlined). |
| 44 | Tokyo Neon Nightmare | Cyberpunk Tokyo | Vertical cyber-horror; cyber-collaborators (outlined). |
| 45 | **Return to Lab Zero** ⭐⭐ | Mutated Lab Zero | Descend F7→F1 + **3 new sub-levels** (see §3d); boss **The First** (HP 12,000); the 65-Myr revelation. |
| 46 | Global Counter-Offensive "United Earth" | Global HQ | RTS multi-front; all allies attack at once. |
| 47–48 | Mothership Assault | Overlord Mothership | Approach (boss **Defense Fleet Admiral**) → Infiltration (boss **Ship's Heart**). |
| 49 | Proto-Overlord Gauntlet "The Puppets" | Mothership | Boss rush of 5 corrupted Earth leaders. **Beta:** one replaced by Siren/Breeder Queen/Oracle. |
| 50 | **The Overlord Core** ⭐⭐⭐ (finale) | Dimensional nexus | 4-phase final fight (Avatar → Swarm → Truth → Choice) → **12 endings**. |

### The DRONE LEVEL — precise answer
There are **two related "drone" things**, do not confuse them:
1. **Floor 5 — Drone Manufacturing / "DroneStation"** is the canonical **drone level** in the
   main 50-level spine (wireframe drones, the 5-drone roster, Sarah's 90-sec master hack, the
   Swarm Controller AI boss, the drone-army payoff, the post-hack drone shop). *(merged Floor 5;
   `TASK_2`; the 391 KB `TASk_2_FLOORS_5-7_COMPLETE_IMPLEMENTATION.md` is the deep impl.)*
2. **The "Crazy Drone" / playable-drone material** in `G:\Unity_Projects\PARALLEL_TASKS_8_CHATS\
   Grok\` is a **separate, experimental drone-piloting prototype** (a hackable/pilotable drone
   with a sanity/rage meter and a helmet-HUD). Files: **`CrazyDrone.cs`** (neural-link to
   "Pilot Zero"; `rageMode`; `WireframeDrone.shader`), **`CrazyDroneController.cs`** (WASD+mouse
   hover, glitch/barrel-rolls, **sanity drain → RAGE MODE at <20%**, scan-beam raycast to hack
   doors/explode drones), plus art refs (`Drones on Stormy Ocean.jpg`, `Biolumeniscent
   leviathon.mp4`). **`Level_11_SalvariCamp.json` explicitly ties the camp's "Drone Hack
   Defense" minigame to the Crazy Drone** ("Crazy Drone assists"). So the Crazy Drone is a
   **drone-piloting mechanic seeded at Floor 5 and reused as a Level-11 minigame**, not its own
   numbered level. *(Sources: `Grok\CrazyDrone.cs`, `Grok\CrazyDroneController.cs`,
   `Grok\Level_11_SalvariCamp.json`.)*

### The CHORUS BOSS — precise answer
**The Chorus = the five-mind merged-scientist boss in the Nexus Chamber, the "Floor 4.5"
half-step between Cybernetics (F4) and DroneStation (F5).** Same entity the layouts call "The
Collective." Shipped as `Floor4_5_NexusChamber_MasterController.cs` + `TheChorusBoss.cs` with a
consciousness-rescue minigame. See §3a for the F4-vs-4.5 conflict and the recommended canon.

---

## 5. Reconciliation vs the current engine build (the GAPS)

**What the engine actually builds today** (read from `app/level1.{h,cpp}`,
`app/level1_game.{h,cpp}`, `app/spire_mid.{h,cpp}`, `app/spire_top.{h,cpp}`,
`app/elevator.{h,cpp}`, `app/rescue.{h,cpp}`):

- A **single vertical graybox "Spire"** of **8 plates** (`L1Floor` enum): **B1, F1, F2, F3, F4,
  F5, F6, F7**, spaced 5 m apart, connected by one central elevator (one stop per plate) +
  a stairwell. *(`level1.h` lines 31–48.)*
- **Engine floor identities (the enum comments):** B1 = basement security, F1 = atrium/lobby,
  F2 = medical wards (3 rescue rooms), F3 = labs, F4 = offices, F5 = synth bay, F6 = executive,
  F7 = rooftop helipad. *(`level1.h`.)*
- **B1 content** = the full Level-1 beat chain (cell → corridor → armory pistol → checkpoint →
  arena → elevator) with a **Martinez** boss + Door A–E logic. *(`level1_game.h`.)*
- **F2** = the rescue hub (Aria/Keisha/Emily via `RescueSystem`). *(`level1_game.h` line 255.)*
- **F3/F4/F5** (`spire_mid`) = **generic enemy escalation only** — counts climb 4→5→6, species
  pulled from the engine's generic roster **DominionTrooper / Verthani / Illuminated /
  BlueSynth**, one keypad door each, plus **one anonymous "captured lab tech"** rescue victim on
  F5. **No Genetics boss, no Cybernetics/Chorus boss, no DroneStation/Swarm boss.** *(`spire_mid.h`.)*
- **F6/F7** (`spire_top`) = generic escalation 7→8; F6 = "occupation strongpoint" with two keypad
  doors; **F7 = a Boss-type "The Clone" + an Illuminated honor-guard pair + Sarah rescue.**
  *(`spire_top.h`.)*

**Itemized GAPS (engine build vs real design):**

| # | Engine floor | Engine reality | Real design | Gap to fix |
|---|---|---|---|---|
| G1 | **F1 Atrium/Lobby** | Glass-atrium breather | **No standalone atrium floor** in the corpus — the canon Floor 1 IS the detention/medical-pod awakening; the lobby is a B1/F1 sub-zone at most. | Re-cast: fold "atrium" into the Detention floor; the 7 floors should be Detention/Medical/Genetics/Cybernetics/Drone/AlienTech/Executive, NOT lobby/wards/labs/offices/synth/exec/rooftop. **Off-by-one floor identities.** |
| G2 | **F3 "Labs" (generic)** | Generic melee escalation | **Genetics Lab** + boss **Failed Experiment #7 (Marcus Webb)**, breeding pods, nursery, video logs. | Add the Genetics identity + the Exp-#7 3-phase boss + pod-destruction loop. **Missing designed boss.** |
| G3 | **F4 "Offices" (generic)** | Cubicle generic escalation + keypad puzzle | **Cybernetics Workshop** + Augmentation/Humanity system + Brain-Upload + mini-boss Karen + the **Collective/Chorus** dome. | Re-identify as Cybernetics; add Humanity mechanic; this floor is also where the **fleet reveal** happens. **Missing identity + systems.** |
| G4 | **(none) Floor 4.5** | **Does not exist** | **Nexus Chamber / The CHORUS boss** (the in-between "4.5" floor the elevator doesn't stop at) + consciousness-rescue minigame. | **Add an off-elevator Floor 4.5 boss arena** between F4 and F5. **Entirely missing.** |
| G5 | **F5 "Synth bay" (generic)** | BlueSynth waves + 1 anonymous victim | **DroneStation** + 5-drone roster + **Sarah's 90-sec master hack** + **Swarm Controller AI** boss + drone-army payoff. | Re-identify as the **drone level**; add the hack sequence + Swarm boss + drone allies. Optionally seed the **Crazy Drone** pilot mechanic. **Missing the signature drone content.** |
| G6 | **F6 "Executive" (generic)** | Occupation strongpoint, 2 keypad doors | **Alien Technology Lab** — Salvari first contact, **K'thara**, the **cure**, boss **Alien Overseer**, Lightning Gun. | Re-identify as Alien Tech; add Salvari/K'thara/cure + Overseer boss. **Wrong floor entirely** (real "Executive" is F7). |
| G7 | **F7 Rooftop + Clone** | Clone boss + honor guard + Sarah rescue (✓ closest match) | **Executive Laboratory** — Clone boss + **timed** Sarah rescue + **timeline lock** (Alpha/Beta/Omega). | Keep the Clone+Sarah, but it's the **Executive Lab interior**, not a rooftop helipad; add the timeline-defining outcome tiers + the 7–9 min recombination timer. The "rooftop/cliffs/Salvari ship" is an engine-spec invention (the canon transition to surface is **Level 8**, a new level). |
| G8 | **F1 boss** | Martinez is on **B1** (engine) | Martinez = **Floor 1** boss (correct), Dr. Chen = **Floor 2** boss. | Engine has Martinez but **no Dr. Chen boss on the medical floor.** Add Chen (3-phase) + the Chen trust/rescue thread. |
| G9 | **F2 rescues** | Aria/Keisha/Emily present (✓) | Plus **Sarah's background timer**, infection-timer tutorial, transformed-victim bosses (Siren/Breeder Queen/Oracle). | Victim→boss transform exists generically; **wire the named transforms + the infection timer.** |
| G10 | **Sub-levels** | **Do not exist** | **Floor-7 hidden sub-levels** (Waste Disposal / Cryo Storage / Enhanced Interrogation) gated on Clone-dead + Sarah-saved → **Dr. Chen rescue**, with the **return fight F7→F1** + Sarah's drone hack. | **Entirely missing** — add the hidden-elevator descent + 3 sub-levels + return-up loop. |
| G11 | **Enemy roster** | Generic Dominion/Verthani/Illuminated/BlueSynth | Floor-specific bestiary (Security Guards/Drones on F1–2; Stage 1–4 Hybrids F3; Augmented/Surgical/Cyborg F4; the 5 wireframe drone types F5; Salvari/Overseer F6; Clone F7). | Replace generic escalation with the **designed per-floor encounter rosters**. |
| G12 | **Weapon ladder** | (pistol baseline) | Pistol+Bazooka → ChainGun (F4) → Plasma Rifle (F5) → Lightning Gun (F6). | Wire the per-floor weapon unlocks. |

**Net:** the engine has a faithful **B1 (Martinez) + F2 (3-rescue) + F7 (Clone+Sarah)** spine,
but **F1/F3/F4/F5/F6 carry the WRONG identities** (lobby/labs/offices/synth/exec instead of
Detention-merge/Genetics/Cybernetics/DroneStation/AlienTech), **all five designed mid bosses are
missing** (Chen, Failed Experiment #7, the Chorus/Collective, Swarm Controller AI, Alien
Overseer), **Floor 4.5/the Chorus is absent**, and the **Floor-7 sub-levels + Chen-rescue return
mission are absent.** The current F3–F6 is "graybox enemy escalation in EFLZ clothing."

---

## 6. Recommended build-order (highest-value corrections first)

Bring Act 1 in line with the real design, biggest narrative ROI first; each step reuses the
engine's existing systems (`monster.*`, `rescue.*`, `door.*`, `trigger.*`, `elevator.*`):

1. **Fix the floor IDENTITIES (cheap, unblocks everything).** Re-label the `L1Floor` enum +
   `level1Rooms()` to the canon: **F1 Detention/Awakening, F2 Medical Bay, F3 Genetics Lab,
   F4 Cybernetics, F5 DroneStation, F6 Alien Tech, F7 Executive Lab** (drop the standalone
   "atrium/offices/synth/rooftop" framing; merge B1+lobby into Detention). Pure relabel +
   re-theme of existing plates. *(Fixes G1, G3, G5, G6, G7.)*
2. **Add the designed mid bosses** onto the existing per-floor managers (they already support a
   Boss-type leader, as F7's Clone proves): **Failed Experiment #7** (F3, 3-phase, drops the
   key), **The Collective/Chorus** (F4 or the new 4.5), **Swarm Controller AI** (F5),
   **Alien Overseer** (F6), and **Dr. Chen** (F2). *(Fixes G2, G3, G5, G6, G8.)*
3. **Floor 5 DroneStation signature content:** Sarah's **90-second 3-phase master hack**
   (defend-while-hacking) + the **drone-army** ally payoff + the 5-drone roster. Highest
   gameplay-identity ROI after identities/bosses. Optionally fold in the **Crazy Drone** pilot
   mechanic (`Grok\CrazyDrone*.cs`). *(Fixes G5, partially G11.)*
4. **Floor 4.5 — the Nexus / Chorus arena:** add a discrete off-elevator boss floor between F4
   and F5 (not a numbered lobby stop) using the existing boss/trigger pattern + the
   consciousness-rescue minigame. *(Fixes G4.)*
5. **Floor 2 depth:** wire the **named** victim→boss transforms (Aria→Siren / Keisha→Breeder
   Queen / Emily→Oracle), the **Sarah background timer**, the **9-min infection timer** + cure
   stations, and the **Dr. Chen trust/rescue** thread. *(Fixes G9, part of G8.)*
6. **Floor 7 Executive correctness:** convert the rooftop to the **Executive Lab interior**, add
   the **7–9 min recombination timer** outcome tiers and the **Alpha/Beta/Omega timeline lock**
   on the Sarah outcome. *(Fixes G7.)*
7. **The hidden Floor-7 SUB-LEVELS + Return Mission:** hidden-elevator descent (gated on
   Clone-dead + Sarah-saved) → Waste Disposal / Cryo Storage (Frozen Collective) / Enhanced
   Interrogation (**Dr. Chen rescue**) → the **return fight F7→F1** (where Sarah's drone hack
   can also live). *(Fixes G10.)*
8. **Per-floor rosters + weapon ladder:** replace generic escalation with the designed enemy
   sets and wire ChainGun (F4) / Plasma Rifle (F5) / Lightning Gun (F6) unlocks. *(Fixes G11, G12.)*

> Items 1–2 alone convert the Spire from "generic graybox" to "the real EFLZ Act 1 skeleton";
> items 3–4 restore the two pieces Tim specifically flagged (the drone level + the Chorus/4.5);
> item 7 restores the sub-levels.

---

### Appendix — quick conflict ledger (prefer the LEFT)
- **Martinez floor:** Floor 1 (merged + impl + bestiary) **vs** Floor 2 (`TASK_9` prompt). → **F1.**
- **F2 boss:** Dr. Chen 3-phase (merged + bestiary) **vs** Martinez (`TASK_9`). → **Dr. Chen.**
- **Chorus location:** "Floor 4.5 Nexus" (shipped code + TASK_1/2) **vs** "Floor 4 Collective"
  (layouts + merged + dialogue header). → treat as **one boss staged as the 4.5 half-step.**
- **Rescue timers:** 5/7/4 + Sarah 9 (cutscenes/`TASK_2`) **vs** 15/20/9 + Sarah 30
  (`Floor2_MedicalBayController.cs`). → **5/7/4/9 placeholder.**
- **Act/level count:** **50 levels / 4 acts** (entire corpus) **vs** 100 levels / 3 acts
  (`MASTER_GAME_PLAN.md` aspirational reframe). → **50/4 is the design;** 100/3 is Tim's stretch plan.
- **Level 20 title:** "The Spaceport" (`TASK_3`) **vs** "Escape From Kethzar Prime" (the JSON). → same level.
