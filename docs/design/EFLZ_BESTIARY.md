# EFLZ — Canonical Bestiary + Boss Digest

*"Escape From Lab Zero" (a.k.a. "Escape Lab 48"). Tim Smith's IP. Distilled from the design
bible in `G:\Unity_Projects\PARALLEL_TASKS_8_CHATS\` for the X3Native engine roster pass.*

**Primary source (authoritative, latest/most complete):**
`TASK_6_ESCAPE_LAB_48_COMPLETE_ENEMY_BESTIARY.md` (v1.0 FINAL; 50+ enemy types, 18 bosses).
Floor-implementation docs (`TASK_9_FLOORS_1-2`, `TASK_1_FLOORS_3-4`, `TASk_2_FLOORS_5-7_COMPLETE`,
side-quests `TASK_10`, audio `TASK_14`) corroborate boss mechanics and are cited where they add
detail or conflict. Act overviews (`TASK_3/4/5`) are thinner and used only for cross-checking names.

> **Naming note.** The game is titled "Escape From Lab Zero" in newer docs and "Escape Lab 48" in
> the bestiary; same game. Acts span **Floors 1-7 (Act 1)** then **Levels 8-50 (Acts 2-4)**.
> Numbers below are the **bible's design values**; the engine deliberately re-scales them to its
> own time/iframe budget (see §4 Reconciliation).

---

## 1. ENEMY ROSTER (regular enemies, grouped by faction)

HP/damage are bible values. "Act/Lvl" = first appearance. Src = bestiary unless noted.

### ACT 1 — Lab Zero (Floors 1-7)

#### Faction: Humans (Lab Zero security & staff)
| Name | Role | HP | Damage | Attack / Range | Special | Act/Lvl |
|---|---|---|---|---|---|---|
| Security Guard (Basic) | Human melee/ranged | 100 | 15 pistol / 25 baton | pistol burst, baton, flashbang, radio-for-backup | Surrenders <20% HP; flees; low morale | A1 / L1 |
| Security Guard (Elite) | Human elite | 200 | 25 rifle / 40 shotgun / 35 knife | rifle, shotgun, taser (stun), grenades; 3-man fire teams | No flee/surrender; squad coordination | A1 / L1 |
| Security Guard (Riot Shield) | Human shield/tank | 200 (+500 shield) | 15 pistol / 45 bash | shield bash + knockback/stagger | 90% front armor; legs exposed; shield destructible | A1 / L1 |
| Lab Scientist (Panicked) | Non-combatant | 50 | 5 (cornered) | flees/hides | Can ignore/threaten/rescue/interrogate; killing = morality hit | A1 / L1 |
| Lab Scientist (Corrupted) | Human hostile | 80 | 20 syringe / 15 scalpel | sedative, scalpel-bleed, chemical throw, panic button | Clarity moments; drawn to wounded; summons elites | A1 / L2 |

#### Faction: Drones (Lab Zero automation — most are HACKABLE by Sarah)
| Name | Role | HP | Damage | Attack / Range | Special | Act/Lvl |
|---|---|---|---|---|---|---|
| Surveillance Drone | Recon flyer | 50 | 10 taser | taser, alert broadcast, flash burst | Hackable 8s; EMP-instakill; marks enemies | A1 / L1 |
| Combat Drone | Ranged flyer | 150 | 15/plasma shot | plasma burst (25m), dive bomb, self-destruct, suppress | Hackable 12s; swarm flanking; **engine BlueSynth basis** | A1 / L2 |
| Medical Drone | Infector (critical) | 80 | 5 inject | nanite injection -> 9-min infection timer; sedative; cables | Hackable 10s (heals, cures Stage 1); targets females/wounded | A1 / L2 |
| Breeder Drone | Capture/transport | 200 | 30 grab / 10 gas | capture-grab, sedative gas, 200HP shield, shock | Hackable 15s; captures Sarah = critical objective | A1 / L2 |
| Hacker Drone | Ally only (Sarah's) | 120 | 5 | disables turrets/drones, door override, stealth field | Purple wireframe; spawns L5+ after Master Hack | A1 / L5 |
| **CrazyDrone** (Grok proto) | Pilot-controlled / rage | n/a (proto) | n/a | scan beam, glitch barrel-rolls, "rage mode" swarm spawn | Neural-linked to **Pilot Zero**; "sanity" drains -> RAGE; wireframe shader. *Source: `Grok/CrazyDrone.cs` + `CrazyDroneController.cs` (prototype, not in bestiary)* | A1 region |

#### Faction: Infected (the alien-signal transformation line)
| Name | Role | HP | Damage | Attack / Range | Special | Act/Lvl |
|---|---|---|---|---|---|---|
| Infected Human (Stage 1) | Fast melee | 150 | 20 claw / 15 bite | clawing combo, infectious bite, tackle, screech | Clarity windows; curable (90%); **engine Verthani basis** | A1 / L2 |
| Infected Human (Stage 2) | Climber/ambush | 250 | 35 | arm swipe (4m), acid spit (DOT), wall pounce, pack howl | Wall/ceiling climb; chitin armor on torso; curable 50% | A1 / L2 |
| Infected Human (Stage 3 / Hybrid) | Commander | 400 | 50+ | melee combo, acid stream, spike volley, pounce, summon horde, telepathic scream | Controls Stage 1-2 in 50m; no cure; Floor 3 spawn | A1 / L3 |

### ACT 2 — Alien World / Keth'zar (Levels 8-20)

#### Faction: Alien Fauna (Crystal Desert + Toxic Swamp)
| Name | Role | HP | Damage | Attack / Range | Special | Act/Lvl |
|---|---|---|---|---|---|---|
| Crystal Stalker | Burrow-ambush predator | 180 | 25 claw / 35 ambush | underground ambush, claws, shard spit | 2x ambush dmg; fire 2x; packs of 3-5 | A2 / L8 |
| Sand Burrower | Large worm | 500 | 50 bite / 80 swallow | emergence strike, bite-grab, swallow (instakill), seismic slam | Unarmored underbelly; lured by sound | A2 / L8 |
| Swamp Stalker | Amphibious | 200 | 20 claw / 30 bite | toxic claw/bite (poison DOT), spit, water pull | Regens 5HP/s in water; fire stops regen | A2 / L11 |

#### Faction: Salvari (alien humanoids — allies OR corrupted)
| Name | Role | HP | Damage | Attack / Range | Special | Act/Lvl |
|---|---|---|---|---|---|---|
| Salvari Scavenger (Hostile) | Corrupted humanoid | 150 | 25 weapon / 20 melee | salvaged guns, traps, ambush | Diplomacy via K'thara (50% convert) | A2 / L14 |
| Salvari Scout | Potential ally | 200 | 30 rifle / 25 blade | energy rifle, blade | Earth-O2 enhanced (fast); friendship arc | A2 / L9 |
| K'thara (Salvari Commander) | Ally / companion | 350 | 40 / 35 | energy weapons, melee | Earth bonuses; romance (Timeline Beta) | A2 / L8-10 |

#### Faction: Overlord (the galactic conqueror — see Act 3/4 for soldiers)
| Name | Role | HP | Damage | Attack / Range | Special | Act/Lvl |
|---|---|---|---|---|---|---|
| Overlord Overseer | Psychic commander | 400 | 40 psi / 50 strike | homing psi-blast, command strike, 200HP barrier, mind shock, summon | Kill -> local forces scatter; psi-amp hackable | A2 / L15 |

### ACT 3 — Space Journey (Levels 21-35)

#### Faction: Void Pirates (human criminals)
| Name | Role | HP | Damage | Attack / Range | Special | Act/Lvl |
|---|---|---|---|---|---|---|
| Pirate Raider | Zero-G human | 120 | 20 rifle / 25 axe | makeshift rifle, boarding axe (armor-pen), thruster charge | 3D zero-G movement; bribable/recruitable | A3 / L21 |

#### Faction: Overlord Space Forces (alien soldiers)
| Name | Role | HP | Damage | Attack / Range | Special | Act/Lvl |
|---|---|---|---|---|---|---|
| Overlord Shock Trooper | Disciplined soldier | 200 | 30 plasma / 40 blade | plasma rifle, energy blade, sticky grenade, squad push | Never surrenders; squad coordination; **engine DominionTrooper analogue** | A3 / L24 |
| Overlord Elite Warrior | Veteran | 400 | 45 plasma / 60 blade | heavy plasma, blocking power blade, domination aura, executioner | +25% dmg aura to allies | A3 / L27 |
| Overlord Fighter Craft | Vehicle | 300 | 40 cannons | plasma cannons | Wings of 3-5; fought via Storm Runner turrets | A3 / L25 |

#### Faction: Corrupted Station Personnel (infected humans, space)
| Name | Role | HP | Damage | Attack / Range | Special | Act/Lvl |
|---|---|---|---|---|---|---|
| Corrupted Crew Member | Infected human | 130 | 20 / 15 bite | improvised weapons, infection bite, operates airlocks/traps | Curable 80% | A3 / L22 |

### ACT 4 — Earth Liberation (Levels 36-50)

#### Faction: Human Collaborators (traitors)
| Name | Role | HP | Damage | Attack / Range | Special | Act/Lvl |
|---|---|---|---|---|---|---|
| Collaborator Militia | Human traitor | 100 | 20 rifle / 25 melee | rifle, baton-stun, call reinforcements | Some surrender/convertible (30-70%) | A4 / L36 |
| Collaborator Enforcer | Traitor elite | 250 | 35 heavy / 50 execution | Overlord plasma, execution strike, buff militia, Overlord beacon | True believers; no surrender | A4 / L38 |

#### Faction: Converted Military (infected humans + vehicles)
| Name | Role | HP | Damage | Attack / Range | Special | Act/Lvl |
|---|---|---|---|---|---|---|
| Infected Soldier | Infected military | 180 | 30 rifle / 25 melee | military rifle, infected claw, grenade, drives vehicles | Switches military/feral mode; curable 70% | A4 / L37 |
| Infected Tank | Vehicle (infected crew) | 2500 | 100 cannon / 40 MG | cannon, MG | 70% armor vs small arms; weak treads/rear/hatch | A4 / L40 |

#### Faction: Overlord Ground Forces (alien soldiers, Earth)
| Name | Role | HP | Damage | Attack / Range | Special | Act/Lvl |
|---|---|---|---|---|---|---|
| Overlord Heavy Trooper | Heavy weapons | 500 | 60 plasma / 80 hammer | heavy plasma cannon, power hammer (AOE), suppression, breach cover | Very slow, long wind-ups; weak back armor | A4 / L42 |
| Overlord Assassin | Stealth specialist | 200 | 80 assassinate / 40 std | stealth strike, dual energy blades, cloak, smoke bomb | Hunts Jake & Sarah; revealed by drones/Sarah | A4 / L44 |

---

## 2. BOSS CATALOG

One entry per boss. Phases/HP are bible values. **The CHORUS and the drone boss are answered in §3.**

### ACT 1 (Floors 1-7)

**CHIEF MARTINEZ — Floor 1 boss (Human).** *Src: bestiary + `TASK_9_FLOORS_1-2` + implemented `Floor1_AwakeningManager.cs`.*
HP **500** (bestiary, phased) / **800** (TASK_9 impl) — conflict noted. Arena: security hub / "Martinez Arena" (30x25x5m). Head of Lab Zero security, ex-special-forces, unaware of the lab's true purpose.
- **P1 The Professional (500-300):** tactical cover, custom pistol, flashbang barrage, rallies 4-6 guards every 30s.
- **P2 The Doubt (300-100):** cybernetic enhancements activate (red eye, arm-blade); cyberblade strike, enhanced aim, EMP grenade; speed up.
- **P3 The Revelation (100-0):** sees the truth on a terminal; armor disabled by shock; reckless.
- **Gimmick:** moral arc — drops Elevator Keycard, Martinez's Journal, Family Photo, Custom Pistol. *This is the F1 boss the engine already implements as the "Boss" archetype.*

**DR. CHEN (CORRUPTED) — Floor 2 boss (Human, transforming).** *Src: bestiary + `DrChenBoss.cs`.*
HP **800** (phased). Oncologist corrupted by the alien signal; orchestrated the breeding program.
- **P1 Scientist (800-500):** lab-equipment + chemical attacks (acid/toxin/paralytic); summons 2 corrupted scientists.
- **P2 Injection (500-200):** injects serum, mutations begin; mutant arm slam.
- **P3 Monster (200-0):** full mutation; rampage + mutation burst AOE.
- **Gimmick — DEFEAT CHOICE:** KILL (50% cure formula) **or** INCAPACITATE+CURE (Chen survives, gives 100% cure, becomes ally).

**FAILED EXPERIMENT #7 ("David") — Floor 3 boss (Enhanced human, tragic).** *Src: bestiary + `FailedExperiment7_Boss.cs` (3 phases).*
HP **1200**, heavy armor 50%, 8ft tall. Jake's predecessor — same 400% enhancement, failed stability. Could-have-been-Jake.
- **P1 Rage (1200-800):** devastating melee, throws objects, charging tackle.
- **P2 Despair (800-400):** clarity moments interrupt; slower but +20% dmg.
- **P3 Release (400-0):** fights for self-control, alternates aggressive/passive, sometimes hits himself.
- **Gimmick — Memory Flash:** `MemoryFlashSystem.cs` shows human memories when pods are destroyed. Death: begs Jake to find his daughter Emma. Drops keycard, journal, Enhancement Stabilizer.

**THE COLLECTIVE / THE CHORUS — Floor 4 (a.k.a. "Floor 4.5 Nexus Chamber") boss (Cyber-horror).** *Src: bestiary + `TASK_1_FLOORS_3-4` + side-quest `SQ_04_01` + audio `MUS_F4_04_COLLECTIVE_BOSS "The Chorus"`. Script: `TheChorusBoss.cs` (pre-existing).*
HP **1000** distributed across **5 pods**. Five scientists merged into one cyber-organism; each personality fights for control.
- **Five voices:** Subject Zero/Maya (300, core, suffering), Dr. Harmon (200, offense), Dr. Patel (200, defense), Dr. Vasquez (150, sometimes sabotages), Dr. Klein (150, zealot). *(Side-quest SQ_04_01 names a savable variant set: Park/Okonkwo/Reznov/Lisa Chen, with Dr. James Whitmore the un-savable traitor — a parallel/alt cast for "The Chorus Speaks.")*
- **Phases:** P1 Unified -> P2 Fracturing (pods attack each other) -> P3 Individual (player picks pods) -> P4 Final (only Subject Zero remains).
- **Gimmick:** save-the-victims morality quest (save one / none / all-possible -> up to 4 allies, "Voice of Compassion" achievement). Death: Maya asks to be remembered; victim names displayed. Drops Neural Interface, Collective Memory Core, **Chaingun**.

**SWARM CONTROLLER AI — Floor 5 boss (AI).** *Src: bestiary + `TASk_2_FLOORS_5-7_COMPLETE` (`SarahMasterHack.cs`). Script: `SwarmControllerAI.cs`.*
HP **5000**, 20% armor + adaptive shielding. AI commanding all Lab Zero drones; holographic avatar.
- **Gimmick — Sarah's 90-second Master Hack (pre-fight):** P1 (0-30s) network access -> Surveillance Drones; P2 (30-75s) override -> Combat Drones; P3 (75-90s) reprogram -> all drones ally **and the boss spawns at -75% HP**. Sarah is vulnerable during the hack (fails if she's downed).
- **Fight P1 Swarm Commander (100-70%):** drone waves, electric grid, turrets.
- **P2 Adaptive Hunter (70-40%):** learns player patterns, avatar fights directly.
- **P3 Desperation (40-0%):** Emergency Purge (30s self-destruct), System Overload AOE; "THIS UNIT... IS... afraid?".

**JAKE'S CLONE — Floor 7 boss = ACT 1 FINALE (Enhanced clone).** *Src: bestiary + `EFLZ_DESIGN.md`. Script: `JakeCloneBoss.cs`.*
HP **1500**, medium armor 30%. A perfect genetic copy with extra alien enhancements, loyal to the Overlord; holds Sarah restrained against a recombination timer. **This is "the Clone" Act-1 finale boss.**
- **P1 Mirror Match (1500-1000):** identical moveset to Jake; counters player attacks.
- **P2 Adaptation (1000-500):** telekinesis, lab-equipment integration, phase-dodge (briefly intangible).
- **P3 Desperation (500-0):** Sarah's timer pressure; tries to finish her process; reckless/baitable.
- **Gimmick — VICTORY CHOICE:** KILL / INCAPACITATE (late-game consequences) / NEURAL HACK (Sarah turns the Clone into a controlled ally — rarest, best-ending path). Locks Timeline Alpha/Beta/Omega. Drops Exit Keycard.

### ACT 2 — Timeline Beta bosses (only if the women were NOT saved on Floor 2)
- **THE SIREN (ARIA) — Beta L18.** HP 1500. Lures with human voice; spawns parasites; acid. P1 Lure / P2 Mother (2x spawns) / P3 Final (clarity points to spawning core). *Script: `SirenAriaBoss.cs`, `TransformedEnemies.cs`.*
- **BREEDER QUEEN (KEISHA) — Beta L19.** HP 2000, 12ft, heavy armor. Tactical mind; spawns Stage-2 infected; rage charge. P1 General / P2 Mother (Stage-3 joins) / P3 Final (clarity exposes a weak flank).
- **THE ORACLE (EMILY) — Beta L20.** HP 1200, psychic. Future sight (predicts/counters), psychic projections, mind control of an ally. P1 Seer / P2 Vision Flood / P3 Final (helps you, transfers a plot vision). Death: "Finally... I can stop seeing..."
- *(Act-2 Toxic Hulk, L12, is a mini-boss: HP 800, slam + toxic-gas enrage — guards the Salvari refugees.)*

### ACT 3
- **MEMORY HUNTER — Act 3 boss, Level 30 (Alien construct).** *Src: bestiary + `TASK_4`. Script: `MemoryHunterBoss.cs`.* HP **3000**. Hunts by reading memories; speaks with the voices of the player's dead (Martinez, David, Chen). **Gimmick — Memory warfare:** P1 illusions of lost characters + memory-spike/prison; P2 summons 50%-stat "memory army" of past enemies; P3 Identity Crisis — the player must **assert identity against all memories** (lose = Game Over). Death: "Because the pain means they mattered." Drops a **Memory Core that resurrects one lost companion.** *(Act overview `TASK_4` flavors it as a "corrupted Salvari AI.")*
- *(Act-3 mini-bosses: Pirate Captain Vex, L23, HP 500, negotiable; Corrupted Station Commander, L28, HP 600.)*

### ACT 4 — Proto-Overlord gauntlet (sub-bosses, L45-49) + finale
- **THE GENERAL — L45.** HP 4000. P1 Strategist (artillery, prediction) / P2 Direct Command / P3 Warrior's End. Death: salutes — "Well fought, human."
- **THE SCIENTIST — L46.** HP 3000. Prototype weapons, specimen summons, capture/forced-evolution; destroys research data if losing. (Dr. Chen unknowingly worked for this entity.)
- **THE PRIEST — L47.** HP 3500. Psychic sermon; **human-shield followers who genuinely believe** (moral challenge); faith breaks or renews in P3.
- **THE ENFORCER — L48.** HP 5000, 12ft. Execution Hammer (100), brutal charge; pure violence; P3 is Jake's 400%-strength power-vs-power showcase.
- **THE VOICE — L49.** HP 2500. Propagandist; demoralization debuffs, signal hijack; P3 choice: destroy the center or hijack it to broadcast the truth globally.
- **THE OVERLORD — FINAL BOSS, Level 50 (Ultimate).** *Script: `TheOverlordFinalBoss.cs`.* **15,000+ HP** across phases; a collective consciousness spanning 2,847 worlds; the Earth avatar is one manifestation. **Fight scales with how many allies you saved.**
  - **P1 The Avatar (4000):** 50ft humanoid; cosmic strike, element barrage, minion waves, assimilation beam; reforms at 0 HP.
  - **P2 The Consciousness (3500):** destroy **5 neural nodes (700 each)** the avatar defends; memory/identity attacks; assimilation temptation.
  - **P3 The Truth (4000):** true eldritch form; reveals it's a survivor of a dead universe consuming worlds to delay entropy; Entropy Beam (150), dimensional shift, existential horror.
  - **P4 The Choice (not combat):** Destroy / Negotiate / Sacrifice / **Full Alliance (Golden Ending)** — determines the ending (Golden/Good/Standard/Sacrifice/Bad/Nightmare).

---

## 3. PER-FLOOR / PER-ACT BOSS MAP

| Act | Floor / Level | Boss | One-line mechanic |
|---|---|---|---|
| 1 | Floor 1 | **Chief Martinez** | 3-phase moral arc; cybernetics in P2; rallies guard adds (the engine's Boss type) |
| 1 | Floor 2 | **Dr. Chen (Corrupted)** | KILL vs CURE choice; transforms across phases |
| 1 | Floor 3 | **Failed Experiment #7 (David)** | Tragic predecessor; Memory-Flash gimmick; clarity windows |
| 1 | Floor 4 / "4.5 Nexus" | **The Collective / The Chorus** | 5 merged scientists in 5 pods; save-the-voices quest; drops Chaingun |
| 1 | Floor 5 | **Swarm Controller AI** | Sarah's 90s Master Hack pre-fight strips 75% HP, converts the drone army |
| 1 | Floor 6 | *(none — security hub / traversal & Chen's logs)* | — |
| 1 | Floor 7 | **Jake's Clone** *(ACT 1 FINALE)* | Mirror-match; rescue Sarah vs timer; KILL/INCAP/NEURAL-HACK ending fork |
| 2 | L12 | Toxic Hulk *(mini)* | Toxic-gas enrage; guards Salvari refugees |
| 2 | L15 | Overlord Overseer *(elite/mini)* | Psychic commander; kill scatters local forces |
| 2 | L18-20 (Beta only) | **Siren (Aria) / Breeder Queen (Keisha) / Oracle (Emily)** | Transformed-women bosses if not saved on Floor 2 |
| 3 | L23 | Pirate Captain Vex *(mini)* | Negotiable boss-lite *(act doc calls a fleet boss "Admiral Vex" — see conflicts)* |
| 3 | L28 | Corrupted Station Commander *(mini)* | Uses station defenses (turrets, venting) |
| 3 | **L30** | **Memory Hunter** *(ACT 3 boss)* | Psychological warfare; assert identity in P3; resurrect a companion |
| 4 | L45 | Proto-Overlord: The General | Strategist -> warrior's-honor death |
| 4 | L46 | Proto-Overlord: The Scientist | Prototype weapons; destroys data if losing |
| 4 | L47 | Proto-Overlord: The Priest | Believer human-shields (moral) |
| 4 | L48 | Proto-Overlord: The Enforcer | 12ft; power-vs-power finale showcase |
| 4 | L49 | Proto-Overlord: The Voice | Propaganda; hijack-the-broadcast choice |
| 4 | **L50** | **The Overlord** *(FINAL)* | 4 phases incl. neural-node phase; ending-choice P4; scales with allies |

---

## 4. RECONCILIATION vs THE CURRENT ENGINE ROSTER

### What the engine has today (`app/monster.{h,cpp}`)
- **Archetypes** (`MonsterType`): `Guard` (melee), `Drone` (ranged), `Boss` (melee + HP-keyed phase machine = Chief Martinez on F1).
- **Data-driven species** (`EnemyType` + `MonsterDef` table in `monster.cpp` ~L1946): exactly **4** rows —
  - `DominionTrooper` (Guard, HP 100, melee, strafeBias 0.20; model `marcus_webb.glb`) — "baseline humanoid soldier," mapped to bible **Security Guard (Basic)**.
  - `Verthani` (Guard, HP 130, fast 4.2 m/s, claw, strafeBias 0.80; model `alien_crawler.glb`) — mapped to bible **Infected Human Stage 1** profile.
  - `Illuminated` (Drone/ranged, HP 220, long 18m standoff, strafeBias 0.10; model `chief_martinez.glb`) — mapped to bible **Security Guard (Elite)** (armor folded into HP).
  - `BlueSynth` (Drone/ranged, HP 150, plasma, strafeBias 0.60; model `Drone.glb` fallback, wants `blue_synth_seed*.glb`) — mapped to bible **Combat Drone**.
- **Boss phase machine** (`BossPhase` P1/P2/P3 by HP frac, enrage speed/dmg/scale/tint, P3 summon) — generic, tuned for **Chief Martinez** only.
- **Combat tunables** (`namespace combat`): melee 6-10 dmg / ~1.0-1.3s cooldown / 1.9m reach / 0.25s windup; ranged 4-6 dmg / 0.8-1.4s / ~7m standoff; **dogpile cap `kMaxMeleeAttackers = 2`**; AI bands (retreat <30% HP, strafe 2.5-6m, search 4s, regroup 12m). **Engine deliberately uses these bands, NOT raw bible HP/damage** (bible numbers assume a different time/iframe budget).

### The 4 engine "species" — CANON faction names mapped onto 4 AI shapes (CORRECTION 2026-05-22)
**CORRECTION:** an earlier draft of this digest called these "engine-invented." That was WRONG. **`Dominion`, `Verthani`, `Illuminated` are real EFLZ bible factions** — `TASK_6_..._ENEMY_BESTIARY.md` references them **78 times** and `docs/MASTER_GAME_PLAN.md` defines them: **Dominion** = multi-armed greys who *built* Lab Zero / run the breeding program; **Verthani** = insectoid warriors; **The Illuminated** = energy-being elite; **Salvari** = the bioluminescent refugee allies — all subject races/forces of the **Overlord** hive. The engine roster correctly adopts these names and currently maps them onto the 4 AI behavior shapes (baseline-melee / fast-flanker / ranged-standoff-elite / ranged-drone). **`BlueSynth`** is the one engine-side coinage (the synth/drone type). **Tim confirmed (2026-05-22): keep these faction names** — they're canon. So the gap is not the *names* but the missing per-faction enemy variety + the bosses (below); add canon rows under these factions rather than renaming.

### MISSING from the engine (designed but unimplemented)
**Regular enemies (Act 1, the immediate need):** Security Guard Elite + Riot-Shield variants; both Lab Scientist types; the full **drone family** (Surveillance/Medical/Breeder/Hacker — only a generic ranged "drone" shape exists, and the hack-to-ally mechanic is absent); Infected **Stage 2** (wall-climb) and **Stage 3 Hybrid** (commander/summoner).
**Bosses:** every boss except Chief Martinez and the (design-noted) Clone — i.e. **Dr. Chen, Failed Experiment #7, The Collective/Chorus, Swarm Controller AI**, all Act 2-4 bosses (Memory Hunter, the 5 Proto-Overlords, the final Overlord, the 3 Beta transformed-women bosses).
**Whole factions:** Salvari, Void Pirates, Overlord soldiers (Shock/Elite/Heavy/Assassin), Collaborators, Converted Military, Alien Fauna — none represented.
**Systems behind enemies:** infection timer + cure, drone hacking/conversion, multi-pod/multi-target bosses (Chorus 5 pods, Overlord 5 nodes), non-combat boss phases (Overlord P4 choice, Memory Hunter identity check), morality/choice outcomes.

### Recommended mapping / expansion (data-first, reuse the existing lane)
The roster is **data-driven** — new enemies are new `MonsterDef` rows, no new code, as long as the behavior fits Guard/Drone/Boss. Recommended renames + additions:

1. **Rename the 4 stand-ins to canon Act-1 species** (or add canon rows alongside) so the roster reads as EFLZ:
   - `DominionTrooper` -> **`SecurityGuard`** (keep stats; it already *is* Security Guard Basic).
   - `Illuminated` -> **`EliteGuard`** (already Security Guard Elite — but it is currently `ranged`; the bible elite is *rifle* ranged, so this is acceptable; consider a separate melee `RiotShield` row).
   - `BlueSynth` -> **`CombatDrone`** (already Combat Drone; add the real `blue_synth`/drone art).
   - `Verthani` -> **`Infected1`** (already the Stage-1 profile).
2. **Add Act-1 rows** that fit existing archetypes with zero new code:
   - `SurveillanceDrone` (Drone, low HP 50, weak taser, short range), `MedicalDrone`/`BreederDrone` (Drone shells until infection/capture systems exist).
   - `Infected2` (Guard, HP 250, higher dmg, high strafe — approximate wall-climb as aggressive flanking for now).
3. **Add boss rows reusing the Boss phase machine** for the melee-ish bosses that fit 3 HP-phases: **Dr. Chen**, **Failed Experiment #7**, and **Jake's Clone** (Clone is already design-noted for F7). Give each distinct HP/tint/scale + the P3 summon count. These need *no* new code — only the **choice/cure/mirror gimmicks** are unmodeled.
4. **Flag as "needs new systems"** (beyond the current phase latch): **The Collective/Chorus** (5 simultaneous damageable pods), **Swarm Controller** (pre-fight hack + adaptive AI), **Memory Hunter** (summon-army + identity phase), **The Overlord** (neural-node phase + non-combat choice phase). The current single-body, HP-fraction Boss machine cannot express multi-target or non-combat phases without extension.
5. **Stage later acts** as their own roster batches (Salvari, Overlord soldiers, etc.) once Act 1 is fully represented — they all fit Guard/Drone archetypes; only the Overlord psychic/aura buffs and vehicle (Tank/Fighter) need new behavior.

### Conflicts / notes (cite-and-flag)
- **Martinez HP:** bestiary **500** vs `TASK_9` impl spec **800**. Engine uses its own band anyway; design canon (`EFLZ_DESIGN.md`) keeps Martinez as the **Floor 1** boss (matching `Floor1_AwakeningManager.cs`). A separate spec (`TASK_9_MISSING_SCRIPTS_SPEC.md`) re-specs Floor 1 as boss-less and moves Martinez to Floor 2 — **not followed**; F1 = Martinez.
- **Chorus floor:** "Floor 4" (bestiary) vs "Floor 4.5 Nexus Chamber" (`TASK_1`, audio, side-quests). Same boss; it sits between Floors 4 and 5.
- **Chorus cast:** bestiary main fight = Subject Zero(Maya)/Harmon/Patel/Vasquez/Klein; side-quest "The Chorus Speaks" lists Park/Okonkwo/Reznov/Lisa Chen + traitor Whitmore. Treat the side-quest set as the **savable-victim** layer of the same boss.
- **Pirate boss name:** bestiary **Pirate Captain Vex** (L23, "Void Jackals"); act overview `TASK_4` mentions **Admiral Vex** (fleet commander). Likely same character or a promoted variant; bestiary value used.
- **CrazyDrone (Grok prototype)** is NOT in the bible bestiary — it's an experimental pilot-controlled/"rage-mode" drone tied to a **Pilot Zero** neural link (`Grok/CrazyDrone.cs`, `CrazyDroneController.cs`), plus a "Biolumeniscent leviathon" video asset in that folder. Treat as concept/proto art, not canon roster.

---

## SOURCE INDEX
- `TASK_6_ESCAPE_LAB_48_COMPLETE_ENEMY_BESTIARY.md` — master bestiary + all 18 bosses (authoritative).
- `Tasks_9_14/TASK_9_FLOORS_1-2_IMPLEMENTATION.md` — Floors 1-2, Chief Martinez (HP 800 spec).
- `TASK_1_FLOORS_3-4_IMPLEMENTATION.md` — Failed Experiment #7 (Memory Flash), The Collective (Humanity Meter), Floor 4.5 Chorus.
- `TASk_2_FLOORS_5-7_COMPLETE_IMPLEMENTATION.md` — Sarah's Master Hack, Swarm Controller, Jake's Clone (Act-1 finale).
- `TASK_10_..._SIDE_QUESTS.md` (SQ_04_01) — "The Chorus Speaks" savable scientists.
- `TASK_14_..._AUDIO_DESIGN.md` — `MUS_F4_04_COLLECTIVE_BOSS` named "The Chorus".
- `TASK_3/4/5_*` — Act 2/3/4 overviews (Memory Hunter, Proto-Overlords, name cross-checks).
- `Grok/CrazyDrone.cs`, `Grok/CrazyDroneController.cs` — prototype drone (Pilot Zero / rage mode).
- Engine: `G:\X3Native\app\monster.h`, `app\monster.cpp` (MonsterType, EnemyType/MonsterDef roster, BossPhase, namespace combat); cross-ref `docs\EFLZ_DESIGN.md`.
