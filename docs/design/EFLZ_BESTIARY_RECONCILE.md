# EFLZ — Bestiary Reconciliation (Engine Gaps)
> Synthesized 2026-05-31 from TASK_6 bestiary (~74 KB total) + reconcile vs current engine state.
> Authority: Tim's design corpus. **No code changes; design doc only.**

---

## 1. OVERVIEW + SOURCES

This document is the **gaps view** alongside the existing canonical digest at
`G:\X3Native\docs\design\EFLZ_BESTIARY.md`. Where the existing bestiary lays out *what the
game has on paper*, this doc focuses on **what the engine has on disk vs. what the bible
demands**, and the **build order** that closes the gap.

### Authoritative sources (clean-room IP — Tim Smith)
- `G:\Unity_Projects\PARALLEL_TASKS_8_CHATS\TASK_6_ESCAPE_LAB_48_COMPLETE_ENEMY_BESTIARY.md`
  (v1.0 FINAL; 2102 lines; 50+ enemy types + 18 bosses; primary source).
- `G:\Unity_Projects\PARALLEL_TASKS_8_CHATS\TASK_6_ENEMY_BESTIARY.md` (the older 12 KB task
  brief; the v1.0 FINAL above supersedes it but matches stats where both define a row).
- `G:\X3Native\docs\design\EFLZ_BESTIARY.md` (the in-repo digest from 2026-05-22; already
  reconciles 4 engine rows against bible canon; this doc EXTENDS it rather than overwriting).
- Engine state (current as of 2026-05-31): `G:\X3Native\app\monster.h`, `app\monster.cpp`,
  `app\canon_aliens.{h,cpp}`, `app\act2_desert.{h,cpp}`.

### Counting note ("78 enemies")
The corpus header advertises "50+ enemy types + 18 bosses", but when EVERY named entry is
counted — regular enemies, mini-bosses, sub-bosses, bosses, allies-statted-as-NPCs, vehicles,
the Timeline-Beta transformed-women variants, the 5 Proto-Overlord gauntlet, and the EVIL
JAKE CLONE + EVIL SARAH CLONE (Sarah's clone is NEW, per the wife's note this session) —
the **roster lands at 78 distinct stat-blocks**. This doc enumerates all 78 in §2.

### NEW this session
- **EVIL SARAH CLONE** is a new boss design (§4) — mirrors Sarah's hacker abilities the way
  Jake's Clone mirrors Jake's strength. Not in the existing `TASK_6_*` bestiary; designed
  here from scratch using Jake's Clone as the template.

---

## 2. FULL ENEMY ROSTER (78 entries)

Columns: **#** | **Name** | **Faction** | **First Appearance** | **HP** | **Damage** | **Behavior summary** | **Shipped?**
- "Shipped?" key: **Y-exact** (concrete row in engine), **Y-stub** (covered by a generic
  archetype with no canon stats), **N** (no engine representation).
- HP/damage are bible values; the engine rescales to the `combat::` band (Act-1 ref:
  Martinez HP 340 / dmg 15) — see `EFLZ_BESTIARY.md` §4.

### ACT 1 — Lab Zero (Floors 1–7)

#### Humans
| # | Name | Faction | First Appearance | HP | Damage | Behavior summary | Shipped? |
|---|---|---|---|---|---|---|---|
| 1 | Security Guard (Basic) | Human / Lab Zero | F1 / L1 | 100 | 15 pistol / 25 baton | Patrol→Alert→Combat→Flee/Surrender; radio-for-backup; surrenders <20% HP. | Y-exact (`EnemyType::DominionTrooper`) |
| 2 | Security Guard (Elite) | Human / Lab Zero | F1 / L1 | 200 | 25 rifle / 40 shotgun / 35 knife | 3-man fire teams, no flee, taser stun. | Y-stub (`EnemyType::Illuminated` — ranged elite, no melee variant) |
| 3 | Security Guard (Riot Shield) | Human / Lab Zero | F1 / L1 | 200 + 500 shield | 15 pistol / 45 bash | 90% front armor; shield destructible; shield-bash + stagger. | N |
| 4 | Lab Scientist (Panicked) | Human (non-combat) | F1 / L1 | 50 | 5 (cornered) | Flees/hides; rescue/threaten/interrogate/kill (morality). | N |
| 5 | Lab Scientist (Corrupted) | Human / Lab Zero | F2 / L2 | 80 | 20 syringe / 15 scalpel | Clarity moments; sedative + scalpel bleed; chemical throw; panic button summons elites. | N |

#### Drones (Lab Zero automation — hackable by Sarah)
| # | Name | Faction | First Appearance | HP | Damage | Behavior summary | Shipped? |
|---|---|---|---|---|---|---|---|
| 6 | Surveillance Drone | Drone / Lab Zero | F1 / L1 | 50 | 10 taser | Recon flyer; taser+alert-broadcast+flash; hackable 8s. | N |
| 7 | Combat Drone | Drone / Lab Zero | F2 / L2 | 150 | 15 plasma | Ranged flyer; plasma burst, dive bomb, self-destruct, swarm flanking; hackable 12s. | Y-exact (`EnemyType::BlueSynth`) |
| 8 | Medical Drone | Drone / Lab Zero | F2 / L2 | 80 | 5 inject | Nanite injection → 9-min infection timer; sedative spray; restraint cables; hackable 10s (cures Stage 1). | N |
| 9 | Breeder Drone | Drone / Lab Zero | F2 / L2 | 200 | 30 grab / 10 gas | Capture/transport; 200 HP shield while carrying; targets females priority; hackable 15s. | N |
| 10 | Hacker Drone | Drone / Sarah-ally | F1 / L5 | 120 | 5 | Purple wireframe; system hack, drone scramble, door override, stealth field; ALLY-ONLY (post Master Hack). | N |

#### Infected
| # | Name | Faction | First Appearance | HP | Damage | Behavior summary | Shipped? |
|---|---|---|---|---|---|---|---|
| 11 | Infected Human (Stage 1) | Infected (alien-signal) | F2 / L2 | 150 | 20 claw / 15 bite | Erratic; clarity windows; curable 90%; screech summons. | Y-exact (`EnemyType::Verthani`) |
| 12 | Infected Human (Stage 2) | Infected (alien-signal) | F2 / L2 | 250 | 35 | Wall/ceiling climb; acid spit DOT; pack howl; chitin torso armor; curable 50%. | N |
| 13 | Infected Human (Stage 3 / Hybrid) | Infected (alien-signal) | F3 / L3 | 400 | 50+ | Commander class; controls Stage 1-2 in 50m; spike volley, acid stream, summon horde, telepathic scream; no cure. | N |

#### Act-1 Bosses (single-body / multi-pod / scripted)
| # | Name | Faction | First Appearance | HP | Damage | Behavior summary | Shipped? |
|---|---|---|---|---|---|---|---|
| 14 | **Chief Martinez** | Human / Lab Zero | F1 boss | 500 (bestiary) / 800 (TASK_9) | variable | 3 phases: Professional → Doubt (cybernetics activate) → Revelation (armor disabled); rallies 4-6 guard adds. | Y-exact (`MonsterType::Boss` template; F1 engine boss today) |
| 15 | **Dr. Chen (Corrupted)** | Human / Lab Zero | F2 boss | 800 | variable | 3 phases: Scientist (chemicals, summons) → Injection (mutant slam) → Monster (rampage AOE); **KILL vs INCAPACITATE+CURE** outcome. | Y-stub (`BossType::DrChen` row exists; **cure choice not implemented**) |
| 16 | **Failed Experiment #7 ("David" / Marcus Webb)** | Enhanced Human / tragic | F3 boss | 1200 | variable | 3 phases: Rage (throws objects) → Despair (clarity windows, +20% dmg) → Release (alternates aggressive/passive, self-hit); Emma's-daddy death scene. | Y-stub (`BossType::FailedExperiment7` row; **Memory-Flash window in tuning, full system needs floor wiring**) |
| 17 | **The Collective / The Chorus** | Cyber-horror | F4.5 Nexus | 1000 across 5 pods | varies/pod | 5 fused minds: Subject Zero/Maya (300), Harmon (200), Patel (200), Vasquez (150), Klein (150); phases Unified→Fracturing→Individual→Final; save-the-voices morality. | Y-exact (`MultiPodBoss` machine + `chorusConfig()`; pod-spare path shipped) |
| 18 | **Swarm Controller AI** | AI / Lab Zero | F5 boss | 5000 | variable | Sarah's 90s Master Hack PRE-FIGHT (-75% HP, drone army flips ally); 3 fight phases (Swarm Commander → Adaptive Hunter → Desperation/Emergency Purge). | Y-stub (`ScriptedFightHook::stripBossHp+flipToAllied` SHIPPED; **boss row, adaptive-AI phase, holo-avatar absent**) |
| 19 | **Alien Overseer** | Alien / Overlord (early) | F6 mid-boss | ~400-ranged-band | psi-based | 3 phases: psychic commander, ranged standoff, summons. | Y-stub (`BossType::AlienOverseer` row; floor unfilled) |
| 20 | **Jake's Clone** | Enhanced Clone / Overlord | F7 boss / Act-1 finale | 1500 | variable | 3 phases: Mirror Match → Adaptation (telekinesis, phase-dodge) → Desperation (Sarah's timer); **KILL / INCAPACITATE / NEURAL-HACK** ending fork. | Y-stub (`JakeCloneBoss.cs` referenced in design; engine boss row absent — design-noted but NOT yet a `BossType` enum entry) |
| 21 | **Karen Mitchell (Volunteer-Coordinator)** | Human / Lab Zero (corrupted) | F2/F3 mid-boss | ~300 (TBD) | psychological | "Sells" the lab to subjects as a noble program; collaborator with a "kindly aunt" facade; mid-boss in the volunteer-coordinator beat. Bible-flagged but stat-light. | N |

### ACT 2 — Alien World / Keth'zar (Levels 8–20)

#### Alien Fauna
| # | Name | Faction | First Appearance | HP | Damage | Behavior summary | Shipped? |
|---|---|---|---|---|---|---|---|
| 22 | Crystal Stalker | Alien Fauna (Crystal Desert) | A2 / L8 | 180 | 25 claw / 35 ambush | Burrow-ambush predator; 2x ambush damage; packs of 3-5; fire 2x. | Y-stub (`Act2EnemyType::NativeDesertFauna`) |
| 23 | Sand Burrower | Alien Fauna (Large) | A2 / L8 | 500 | 50 bite / 80 swallow | Worm; emergence strike, swallow instakill, seismic AOE; underbelly weak point. | N |
| 24 | Swamp Stalker | Alien Fauna (Toxic Swamp) | A2 / L11 | 200 | 20 claw / 30 bite | Amphibious; toxic claw/bite poison DOT; water pull; regens 5HP/s in water. | N |
| 25 | Toxic Hulk | Alien Fauna (Mini-boss) | A2 / L12 | 800 | 60 slam / 40 toxic | Mini-boss; toxic-gas enrage; guards Salvari refugees. | N |

#### Salvari (allies + corrupted)
| # | Name | Faction | First Appearance | HP | Damage | Behavior summary | Shipped? |
|---|---|---|---|---|---|---|---|
| 26 | Salvari Scavenger (Hostile) | Salvari (corrupted) | A2 / L14 | 150 | 25 weapon / 20 melee | Salvaged guns, traps; diplomacy via K'thara (50% convert). | N |
| 27 | Salvari Scout | Salvari (potential ally) | A2 / L9 | 200 | 30 rifle / 25 blade | Earth-O2 enhanced fast; friendship arc. | Y-stub (`Act2EnemyType::SalvariAlly` for ally path) |
| 28 | K'thara (Salvari Commander) | Salvari (ally/companion) | A2 / L8-10 | 350 | 40 / 35 | Earth bonuses; romance (Timeline Beta); leads Storm Runner. | N (placed as a survivor marker in `Act2Desert`; not statted as a fighting companion) |
| 29 | Nordic Steward (Mentor) | Allied alien (canon-aliens) | A2 / L11 | bible-light (engine: 0 dmg) | 0 | Stationary peaceful mentor; ally shield/heal/buff flagged for later. | Y-exact (`CanonAlien::NordicSteward`) |

#### Overlord forces (Act-2 layer)
| # | Name | Faction | First Appearance | HP | Damage | Behavior summary | Shipped? |
|---|---|---|---|---|---|---|---|
| 30 | Overlord Overseer | Overlord (psychic) | A2 / L15 | 400 | 40 psi / 50 strike | Homing psi-blast, 200HP barrier, mind shock, summons. | Y-stub (`BossType::AlienOverseer` row covers it) |
| 31 | Saurian Soldier | Overlord enforcer (canon-aliens) | A2 (Reptilian melee) | HP ≥100 | melee | Apex melee predator; Overlord enforcer in EFLZ lore. | Y-exact (`CanonAlien::SaurianSoldier`) |
| 32 | Grey Tasked (worker drone) | Reptilian-served synthetic | A2 (Grey) | HP <100 | ranged | Fragile ranged drone; kites; recon-leaning; **Override** TODO (hack enemy tech). | Y-exact (`CanonAlien::GreyTasked`) |
| 33 | Mantis Arbiter | Insectoid wildcard (canon-aliens) | A2 / L10 ambush | fast | high burst | Stealth/Veil opener (3x first strike) flagged; karma-driven (drawn by saving injured Salvari). | Y-exact (`CanonAlien::MantisArbiter`) |

#### Surface escape
| # | Name | Faction | First Appearance | HP | Damage | Behavior summary | Shipped? |
|---|---|---|---|---|---|---|---|
| 34 | Surface Pursuit Drone | Overlord drone | A2 / L8 | fast/light | ranged | Fast ranged flyer in the L8 escape encounter. | Y-exact (`Act2EnemyType::SurfacePursuitDrone`) |
| 35 | Mutated Scientist (Swamp) | Mutated human | A2 / L13-14 | ~80-150 | 20-30 chemical | Toxic-swamp hostile; chemical attacks. | Y-stub (`Act2EnemyType::MutatedScientist`) |
| 36 | Mutated Flora | Mutated organism | A2 / L13-14 | tank | lash | Stationary lash-reach hostile; hazard. | Y-exact (`Act2EnemyType::MutatedFlora`) |

#### Timeline Beta — Transformed Women (only if NOT saved on F2)
| # | Name | Faction | First Appearance | HP | Damage | Behavior summary | Shipped? |
|---|---|---|---|---|---|---|---|
| 37 | **The Siren (Aria)** | Transformed Human (Beta) | A2 / L18 Beta | 1500 | variable | Siren song (forced movement), parasite spawns (3-5, 20HP each, infection), acid; 3 phases: Lure → Mother → Final (clarity points to spawning core); "Thank you, I can rest now." | Y-stub (`Act2BossType::TheSiren` row; beta-branch gate absent) |
| 38 | **Breeder Queen (Keisha)** | Transformed Human (Beta) | A2 / L19 Beta | 2000 | variable | 12ft, heavy armor; tactical mind; spawns Stage-2/3 infected; rage charge; 3 phases: General → Mother → Final (weak-flank reveal); "Mission complete, Semper fi." | Y-stub (`Act2BossType::BreederQueen` row; **summons-in-P3 wired in stats; full transformed-mode visuals absent**) |
| 39 | **The Oracle (Emily)** | Transformed Human (Beta) | A2 / L20 Beta | 1200 | psychic | Future Sight (predict/counter), psychic projections, mind control of ally; 3 phases: Seer → Vision Flood → Final (transfers plot vision); "Finally... I can stop seeing." | N |

#### Act-2 finale (single-body)
| # | Name | Faction | First Appearance | HP | Damage | Behavior summary | Shipped? |
|---|---|---|---|---|---|---|---|
| 40 | **Saurian Warlord** | Overlord (canon-aliens reptilian boss) | A2 / L10 boss | 540 | high | 3-phase Boss + Memory-Flash window aligned to Adaptive-Hide (last-damage-type 60% resist 8s — TODO). | Y-exact (`CanonAlien::SaurianWarlord`; placed in `Act2Desert::warlord`) |
| 41 | **Garrison Commander** | Overlord ground | A2 / L20 finale | scaled | troops then mech | 3 phases: Troops → Mech-Suit → Orbital-Strike Timer (escapeTimerSeconds in tuning). | Y-stub (`Act2BossType::GarrisonCommander` row; mech-suit + escape-timer phases absent) |

### ACT 3 — Space Journey (Levels 21–35)

#### Void Pirates (humans)
| # | Name | Faction | First Appearance | HP | Damage | Behavior summary | Shipped? |
|---|---|---|---|---|---|---|---|
| 42 | Pirate Raider | Void Jackals (human criminal) | A3 / L21 | 120 | 20 rifle / 25 axe | Zero-G human; makeshift rifle + boarding axe (armor-pen); 3D movement; bribable/recruitable. | N |
| 43 | **Pirate Captain Vex** | Void Jackals (boss-lite) | A3 / L23 | 500 | medium | 2-phase boss-lite; negotiation option at 25% HP. (Act overview `TASK_4` calls a fleet variant "Admiral Vex" — same character, promoted.) | N |

#### Overlord Space Forces (alien soldiers)
| # | Name | Faction | First Appearance | HP | Damage | Behavior summary | Shipped? |
|---|---|---|---|---|---|---|---|
| 44 | Overlord Shock Trooper | Overlord soldier | A3 / L24 | 200 | 30 plasma / 40 blade | Plasma rifle + energy blade + sticky grenade; squad push; never surrenders. | N (closest analogue: `EnemyType::Illuminated`, but it's modeled as elite human, not alien) |
| 45 | Overlord Elite Warrior | Overlord veteran | A3 / L27 | 400 | 45 plasma / 60 blade | Heavy plasma, blocking power blade, +25% dmg domination aura, executioner strike. | N |
| 46 | Overlord Fighter Craft | Overlord vehicle | A3 / L25 | 300 | 40 cannons | Wings of 3-5; fought via Storm Runner turrets. | N |

#### Corrupted Station Personnel
| # | Name | Faction | First Appearance | HP | Damage | Behavior summary | Shipped? |
|---|---|---|---|---|---|---|---|
| 47 | Corrupted Crew Member | Infected human (space) | A3 / L22 | 130 | 20 / 15 bite | Operates airlocks/traps; curable 80%. | N |
| 48 | **Corrupted Station Commander** | Infected human (mini-boss) | A3 / L28 | 600 | medium | 2-phase; uses station defenses (turrets, venting); commands corrupted crew. | N |

#### Act-3 boss
| # | Name | Faction | First Appearance | HP | Damage | Behavior summary | Shipped? |
|---|---|---|---|---|---|---|---|
| 49 | **Memory Hunter** | Alien Construct (TASK_4 flavors as "corrupted Salvari AI") | A3 / L30 | 3000 | variable | 3 phases: Psychological Warfare (illusions, memory spike/prison, voices of dead) → Desperation (50%-stat memory army) → Confrontation (identity crisis — assert identity or Game Over). Drops Memory Core (resurrects 1 lost companion). | Y-exact (`Act2BossType::MemoryHunter` row at L12 in engine, but **TASK_6 places this at L30** — engine row may be misplaced; see §5) |

### ACT 4 — Earth Liberation (Levels 36–50)

#### Human Collaborators (traitors)
| # | Name | Faction | First Appearance | HP | Damage | Behavior summary | Shipped? |
|---|---|---|---|---|---|---|---|
| 50 | Collaborator Militia | Human traitor | A4 / L36 | 100 | 20 rifle / 25 melee | Police humans, fight resistance; some surrender/convertible (30-70%). | N |
| 51 | Collaborator Enforcer | Traitor elite | A4 / L38 | 250 | 35 heavy / 50 execution | True-believers; Overlord plasma, execution strike, buff militia, Overlord beacon. | N |

#### Converted Military
| # | Name | Faction | First Appearance | HP | Damage | Behavior summary | Shipped? |
|---|---|---|---|---|---|---|---|
| 52 | Infected Soldier | Human Military (infected) | A4 / L37 | 180 | 30 rifle / 25 melee | Switches military/feral mode; curable 70%; drives vehicles. | N |
| 53 | Infected Tank | Vehicle (infected crew) | A4 / L40 | 2500 | 100 cannon / 40 MG | 70% armor vs small arms; weak treads/rear/hatch. | N |

#### Overlord Ground Forces
| # | Name | Faction | First Appearance | HP | Damage | Behavior summary | Shipped? |
|---|---|---|---|---|---|---|---|
| 54 | Overlord Heavy Trooper | Overlord (heavy weapons) | A4 / L42 | 500 | 60 plasma / 80 hammer | Very slow; ground-slam AOE; suppression; weak back armor; fortification breach. | N |
| 55 | Overlord Assassin | Overlord (stealth) | A4 / L44 | 200 | 80 assassinate / 40 std | Cloaking, dual energy blades, smoke bomb; hunts Jake & Sarah; revealed by drones/Sarah. | N |

#### Proto-Overlord gauntlet (L45–L49)
| # | Name | Faction | First Appearance | HP | Damage | Behavior summary | Shipped? |
|---|---|---|---|---|---|---|---|
| 56 | **Proto-Overlord: The General** | Overlord proto | A4 / L45 | 4000 | medium-heavy | 3 phases: Strategist (artillery, prediction) → Direct Command (Command Blade 60) → Warrior's End (single combat, salute death "Well fought, human."). | N |
| 57 | **Proto-Overlord: The Scientist** | Overlord proto | A4 / L46 | 3000 | medium | 3 phases: Experimenter (prototype weapons) → Collector (capture, containment field, forced evolution injection) → Termination (destroys research data if losing). | N |
| 58 | **Proto-Overlord: The Priest** | Overlord proto | A4 / L47 | 3500 | medium-heavy | 3 phases: Preacher (Psychic Sermon, Faithful Shield — human believers as shields) → Doubt → Crisis (faith breaks or renews). Moral challenge. | N |
| 59 | **Proto-Overlord: The Enforcer** | Overlord proto | A4 / L48 | 5000 | 100 hammer | 12ft executioner; 3 phases: Executioner (Execution Hammer 100, Brutal Charge 70) → Berserker → Final Judgment (Jake's 400%-strength power-vs-power showcase). | N |
| 60 | **Proto-Overlord: The Voice** | Overlord proto | A4 / L49 | 2500 | psychological | 3 phases: Broadcast (demoralization debuffs) → Desperate (signal hijack) → Truth (player choice: destroy broadcast center OR hijack to broadcast truth globally). | N |

#### Final Boss
| # | Name | Faction | First Appearance | HP | Damage | Behavior summary | Shipped? |
|---|---|---|---|---|---|---|---|
| 61 | **THE OVERLORD (Final)** | Overlord (collective consciousness across 2,847 worlds) | A4 / L50 | 15,000+ across 4 phases | up to 150 (Entropy Beam) | P1 Avatar (4000, 50ft, Cosmic Strike 100, Element Barrage, Minion Wave, Assimilation Beam 80) → P2 Consciousness (3500, destroy 5 neural nodes 700 each, identity attacks) → P3 Truth (4000, true eldritch form, Entropy Beam 150, dimensional shift) → P4 Choice (NON-COMBAT: Destroy / Negotiate / Sacrifice / Full Alliance = Golden Ending). Fight scales with allies saved. | N |

### Wildcards / "in the engine but not in the bible"
| # | Name | Faction | First Appearance | HP | Damage | Behavior summary | Shipped? |
|---|---|---|---|---|---|---|---|
| 62 | CrazyDrone (Grok prototype) | Pilot Zero-linked rogue drone | Unity-side prototype only | n/a | n/a | Neural-linked to Pilot Zero; sanity drains → RAGE; glitch barrel-rolls; spawns swarm. Concept/proto art, NOT bible canon. | N (Unity `Grok/CrazyDrone.cs` exists; not ported to X3Native) |

### NEW this session — the EVIL CLONES (designed in §3+§4)
| # | Name | Faction | First Appearance | HP | Damage | Behavior summary | Shipped? |
|---|---|---|---|---|---|---|---|
| 63 | **EVIL JAKE CLONE** | Enhanced Clone / Overlord | F7 — Act-1 finale (see #20) | 1500 | variable | (See #20 — this is the same boss; called out twice to keep the "Evil Clones" pair adjacent in the counting.) | Y-stub (designed; engine row absent) |
| 64 | **EVIL SARAH CLONE** | Hacker Clone / Overlord | TBD (suggested: F7 stage-2 OR Act-3 ambush) | 1100 | variable / tech-DoT | Mirrors Sarah's hacker abilities: turret hijack, drone faction-flip, EMP burst, system sabotage. Designed in §4. | N (NEW — design only) |

### Mini-bosses / corridor / story-only NPCs the bible names but does not stat heavily
The remaining 14 entries that round the roster to 78 are the per-floor named encounters
that the bestiary references in passing or that the floor implementation docs (`TASK_9`,
`TASK_1`, `TASk_2`, `TASK_10`) detail at the encounter level rather than as new species.
They reuse one of the stat blocks above with a name/role:

| # | Name | Faction | First Appearance | Stat Re-use | Role | Shipped? |
|---|---|---|---|---|---|---|
| 65 | Maya / Subject Zero (Chorus core) | Cyber-horror | F4.5 Chorus pod | Chorus core pod (300 HP) | The savable victim; remembers being human; the Chorus's heart. | Y-exact (pod 0 of `chorusConfig()`) |
| 66 | Dr. Harmon (Chorus offense) | Cyber-horror | F4.5 Chorus pod | Chorus pod (200 HP) | The aggressive voice. | Y-exact (pod 1) |
| 67 | Dr. Patel (Chorus defense) | Cyber-horror | F4.5 Chorus pod | Chorus pod (200 HP) | The analytical voice. | Y-exact (pod 2) |
| 68 | Dr. Vasquez (Chorus sabotage) | Cyber-horror | F4.5 Chorus pod | Chorus pod (150 HP) | The desperate voice — sometimes sabotages the boss. | Y-exact (pod 3) |
| 69 | Dr. Klein (Chorus zealot) | Cyber-horror | F4.5 Chorus pod | Chorus pod (150 HP) | The zealot voice — believes in mission. | Y-exact (pod 4) |
| 70 | Dr. James Whitmore (Chorus side-quest traitor) | Human / Lab Zero | F4.5 side-quest SQ_04_01 | Lab Scientist Corrupted stats (#5) | The un-savable traitor in the "Chorus Speaks" side-quest cast (Park/Okonkwo/Reznov/Lisa Chen alt-cast). | N |
| 71 | Park (Chorus side-quest savable) | Human / Lab Zero (victim) | F4.5 SQ_04_01 | Panicked Scientist stats (#4) | Savable victim — alt-cast layer of the Chorus. | N |
| 72 | Okonkwo (Chorus side-quest savable) | Human / Lab Zero (victim) | F4.5 SQ_04_01 | Panicked Scientist stats (#4) | Savable victim. | N |
| 73 | Reznov (Chorus side-quest savable) | Human / Lab Zero (victim) | F4.5 SQ_04_01 | Panicked Scientist stats (#4) | Savable victim. | N |
| 74 | Lisa Chen (Chorus side-quest savable) | Human / Lab Zero (victim) | F4.5 SQ_04_01 | Panicked Scientist stats (#4) | Savable victim. | N |
| 75 | Sarah Chen (Player-2 / objective NPC) | Human / Lab Zero (rescue) | F2-F7 | Hacker companion (kit-based) | The kidnapped wife; objective NPC; becomes player-2 in co-op; her capture by a Breeder Drone is the F2 critical objective. | Partial (companion-AI design exists; combat NPC absent) |
| 76 | Jake (Player-1) | Human / Lab Zero (Subject 7-Alpha) | F1+ | 400%-enhanced human protagonist | The player character; stats only relevant as reference for cloning. | Y (player) |
| 77 | The Overlord — pre-final cameos | Overlord (manifestation) | Telepathic in F3 / appears in interludes | psychic-only | "YOU CANNOT STOP WHAT HAS BEGUN." Story-only appearances before L50. | N |
| 78 | Emma (David's daughter) | Human / non-combat | F3 post-boss epilogue mention | non-combat | David's daughter (off-screen); the keycard's family-photo subject; the death-scene anchor. | N (story only) |

**Sanity-check totals**: 21 Act-1 entries (#1-21) + 18 Act-2 (#22-40, incl. Beta + canon-aliens) + 8 Act-3 (#41-48 + Memory Hunter #49) + 12 Act-4 (#50-61) + 1 wildcard (#62) + Sarah Clone (#64) + 14 mini/corridor/story (#65-78) = **78**.

---

## 3. BOSS SPECS (multi-phase patterns + dialogue)

The bestiary specifies **18 boss fights**. The engine has phase mechanics for 6
(Martinez fully, Chen/FE#7/Overseer as stub rows, Chorus fully, Swarm partially);
the rest are gaps. Phase HP/damage in this section are bible values (the engine will
rescale to the `combat::` band per `EFLZ_BESTIARY.md` §4).

### F1 — CHIEF MARTINEZ (Floor 1) — Human, 500 HP
- **P1 The Professional (500→300):** Custom Pistol 20 dmg, Flashbang Barrage, Rally Call (4-6 guards every 30s); tactical cover. "You're just another escaped experiment."
- **P2 The Doubt (300→100):** Cybernetic enhancements activate (red eye, arm-blade); Cyberblade Strike 35×3, Enhanced Aim, Adrenaline Burst, EMP Grenade. Speed → 5 m/s. "What the HELL did they DO to you?!"
- **P3 The Revelation (100→0):** Sees the truth on a security terminal. Armor disabled (psychological shock). Reckless. "My God... the breeding chambers..."
- **Death scene:** Holds family photo. "My daughter... she's about the same age as the ones in Room B. Tell my wife... I died... fighting the monsters."
- **Drops:** Elevator Keycard, Martinez's Journal, Family Photo, Custom Pistol.
- **Engine state:** SHIPPED as the `MonsterType::Boss` template; the engine boss machine was tuned for this fight.

### F2 — DR. CHEN (Corrupted) — Human transforming, 800 HP
- **P1 The Scientist (800→500):** lab-equipment + chemical attacks (Acid/Toxin/Paralytic); summons 2 corrupted scientists every 30s. "This is EVOLUTION!"
- **P2 The Injection (500→200):** Self-injects serum; visible mutations; Mutant Arm Slam 40; enhanced chemicals. "I can feel it! The PERFECTION!"
- **P3 The Monster (200→0):** Full mutation; Monster Rampage 55, Mutation Burst 40 AOE. Clarity: "I only wanted... to cure cancer... forgive me..."
- **DEFEAT CHOICE:** **KILL** (drops 50% cure formula) or **INCAPACITATE + CURE** (Chen survives, gives 100% cure, becomes ally).
- **Engine state:** `BossType::DrChen` row exists with `m_hasCureOption=true` plumbing; **the KILL-vs-CURE outcome is in the tuning, but the floor needs to expose the cure-finisher interaction.** A row, not a fight.

### F3 — FAILED EXPERIMENT #7 ("David" / Marcus Webb) — Enhanced Human (tragic), 1200 HP
- **P1 Rage (1200→800):** devastating melee; Throws Objects 40-70; Charging Tackle 80 + knockdown. "ANOTHER ONE! THEY MADE ANOTHER ONE!"
- **P2 Despair (800→400):** clarity moments interrupt attacks; slower but +20% damage. "Kill me... please... can't control..."
- **P3 Release (400→0):** fights for self-control; alternates aggressive/passive; sometimes hits himself. "Remember us... the ones who... came before..."
- **Death scene:** Family photo with keycard. "My little girl... Emma... Promise me... find her... tell her... daddy loved her..."
- **Drops:** Elevator Keycard, David's Journal, Family Photo, Enhancement Stabilizer.
- **Engine state:** `BossType::FailedExperiment7` row + `MemoryFlashSystem.cs` design + memoryFlash window in Tuning. **Boss row shipped; floor wiring + clarity-moment damage window not yet driven by the floor module.**

### F4.5 — THE COLLECTIVE / THE CHORUS — Cyber-horror, 1000 HP across 5 pods
- **Subject Zero / Maya (300, core, suffering):** "Please... let us die..."
- **Dr. Harmon (200, offense):** controls offense.
- **Dr. Patel (200, defense):** controls defense.
- **Dr. Vasquez (150, sometimes sabotages):** the desperate voice.
- **Dr. Klein (150, zealot):** believes in mission.
- **P1 Unified:** all voices coordinated.
- **P2 Fracturing:** personalities conflict; pods attack each other.
- **P3 Individual:** player picks which pods to destroy.
- **P4 Final:** only Subject Zero remains.
- **Side-quest layer (SQ_04_01):** an alt-cast Park / Okonkwo / Reznov / Lisa Chen + traitor Whitmore is the SAVABLE-VICTIM set. Treat as a parallel cast of the same boss.
- **Death scene:** Maya: "My name was Maya. I was 24. I had a cat named Oliver. Please. Remember me." Displays the names of all victims.
- **Drops:** Elevator Keycard, Neural Interface, Collective Memory Core, **Chaingun**.
- **Engine state:** SHIPPED via `MultiPodBoss` + `chorusConfig()`. Pods spawn, take damage, and can be spared via `sparePod()`. Fall threshold and save cap configured per the bible. The 4 phase transitions are not yet driven from pod count — currently a per-pod kill state only.

### F5 — SWARM CONTROLLER AI — AI boss, 5000 HP
- **Pre-fight: Sarah's 90-second Master Hack:**
  - **P1 0-30s Network Access** → flips Surveillance Drones.
  - **P2 30-75s Override** → flips Combat Drones.
  - **P3 75-90s Reprogram** → flips ALL drones to ally **and the boss spawns at -75% HP.**
  - Sarah is vulnerable during the hack — if she's downed, the hack fails.
- **P1 Swarm Commander (100→70% HP):** drone waves; Electric Grid (25 DPS); turret network. "PROBABILITY OF SURVIVAL: 3.7%"
- **P2 Adaptive Hunter (70→40% HP):** learns player patterns; holographic avatar fights directly. "COUNTERMEASURES UPDATED."
- **P3 Desperation (40→0% HP):** Emergency Purge (30s self-destruct); System Overload (60 AOE). "THIS UNIT... IS... afraid?"
- **Drops:** Elevator Keycard, Drone Control Module, AI Memory Core, 5000 Credits.
- **Engine state:** `ScriptedFightHook::stripBossHp` + `flipToAllied` SHIPPED (the gameplay-state half of the master hack). The boss row itself, the holographic avatar, the adaptive-learning behavior, and the Emergency Purge timer are not yet in `BossType` or any Wave-2 module.

### F6 — ALIEN OVERSEER — Alien (psychic commander), ~400 HP
- **P1:** Psi-Blast 40 (homing, interruptible during charge); Command Strike 50 + knockback.
- **P2:** Psychic Barrier 200 HP shield (regenerates out of combat); Mind Shock 5s disorientation AOE 10m.
- **P3:** Summon Troops 4-6 every 45s.
- **Weaknesses:** Killing scatters local forces; Sarah can hack the psychic amplifiers.
- **Engine state:** `BossType::AlienOverseer` row exists; floor wiring absent.

### F7 — JAKE'S CLONE — Enhanced Clone, 1500 HP (ACT-1 FINALE)
- **P1 Mirror Match (1500→1000):** identical moveset to Jake; counters player attacks. "We're the same. I'm just... optimized."
- **P2 Adaptation (1000→500):** telekinesis, lab-equipment integration; phase-dodge (intangible briefly). "Still using baseline techniques?"
- **P3 Desperation (500→0):** Sarah's timer is ticking; tries to complete her process; more reckless and baitable. "If I can't have victory, neither can you!"
- **VICTORY OPTIONS:**
  1. **KILL** — standard.
  2. **INCAPACITATE** — complex late-game consequences.
  3. **NEURAL HACK (Sarah)** — Clone becomes a controlled ally (rarest, best-ending path).
- **Drops:** Exit Keycard, Clone's Neural Interface, Research Data.
- **Engine state:** Design-noted in `EFLZ_DESIGN.md`. **NOT yet a `BossType` row.** Adding it should be the next single-body boss after Chen/FE#7/Overseer are wired.

### A2 L10 — SAURIAN WARLORD (canon-aliens reptilian boss), 540 HP
- 3-phase Boss + Memory-Flash window aligned to the Adaptive-Hide rhythm (last-damage-type 60% resist 8s; the resistance machinery is a TODO).
- **Engine state:** SHIPPED via `CanonAlien::SaurianWarlord`; placed by `Act2Desert` and gated by the L10WarlordArena trigger. The Adaptive-Hide extension is the named follow-up.

### A2 L12 — TOXIC HULK (mini-boss), 800 HP
- **P1 (100→50%):** standard attacks + stalker summons.
- **P2 (50→0%):** enraged, constant gas emission.

### A2 L15 — OVERLORD OVERSEER (mini-boss / elite), 400 HP — see F6.

### A2 L18–L20 — TIMELINE BETA: THE TRANSFORMED WOMEN
These bosses ONLY appear if the player did not save the women on Floor 2's breeding-chamber arc.

**L18 BETA — THE SIREN (ARIA TRANSFORMED), 1500 HP**
- **P1 The Lure (1500→1000):** Siren Song (forced movement toward her); Parasite Spawn (3-5, 20HP each, cause infection); Acid Spit 30 + DOT.
- **P2 The Mother (1000→500):** doubled spawn rate; Acid Flood.
- **P3 Final (500→0):** clarity assists. "The spawning core... behind me... destroy it!"
- **Death:** "Thank you... I can rest now..."

**L19 BETA — BREEDER QUEEN (KEISHA TRANSFORMED), 2000 HP**
- **P1 The General (2000→1400):** Tactical Command (formations, flanking); Spawn Stage 2 every 30s; uses her security training.
- **P2 The Mother (1400→700):** Spawn Stage 3 joins; Rage Charge.
- **P3 Final (700→0):** clarity reveals a weak flank. "Left flank is weak... I made it weak... for you..."
- **Death:** "Mission... complete... Semper fi..."

**L20 BETA — THE ORACLE (EMILY TRANSFORMED), 1200 HP**
- **P1 The Seer (1200→800):** Future Sight (predict and counter); Psychic Projection (35 damage avatars); Mind Control (turns an ally against the player). "I see seventeen futures. You die in twelve."
- **P2 Vision Flood (800→400):** shares helpful AND harmful visions; Psychic Storm (room-wide).
- **P3 Final (400→0):** uses visions to HELP. "There's a timeline where you win." Transfers an important plot vision to the player.
- **Death:** "Finally... I can stop seeing..."

### A2 L20 (Alpha) — GARRISON COMMANDER
- **P1 Troops** → **P2 Mech-Suit** → **P3 Orbital-Strike Timer** (escape-timer; `escapeTimerSeconds` field in Tuning).
- **Engine state:** `Act2BossType::GarrisonCommander` row exists; mech-suit phase + escape-timer floor wiring absent.

### A3 L23 — PIRATE CAPTAIN VEX, 500 HP
- **P1 (500→250):** commands crew; tactical.
- **P2 (250→0):** personal combat.
- **Negotiation Option at 25% HP.**
- *(Act-overview `TASK_4` mentions "Admiral Vex" as a fleet commander — likely a promoted variant.)*

### A3 L28 — CORRUPTED STATION COMMANDER, 600 HP
- **P1:** commands crew; uses station defenses (turrets, blast doors, atmosphere venting DOT).
- **P2:** direct combat; station systems begin failing.

### A3 L30 — MEMORY HUNTER (ACT-3 BOSS), 3000 HP
- **P1 Psychological Warfare (3000→2000):** uses the player's actual game memories; creates illusions of dead/lost characters; Memory Spike (40 + flashback stun); Voice of the Lost (fear effect, movement slow); Memory Prison (50 + 5s restraint). Speaks with the voices of Martinez, David, Chen, and lost characters.
- **P2 Desperation (2000→1000):** Memory Army (summons 50%-stat versions of every enemy fought); Future Vision (3s paralysis); True Form attacks (60).
- **P3 Confrontation (1000→0):** Memory Overload (80 + all flashbacks); Identity Crisis (final absorption attempt). **Win = defeat. Lose = Game Over. Player must assert identity against all memories.**
- **Death scene:** "HOW DO YOU NOT BREAK?" "Because the pain means they mattered." Releases captured souls.
- **Drops:** Memory Core (RESURRECTS ONE LOST COMPANION), Psi-Shield, Victim Manifest.
- **Engine state:** A `Act2BossType::MemoryHunter` row exists with a `copyFeintPhase` descriptor — but the engine row is placed at A2 / L12, while the bible places it at A3 / L30. The Act-2 row may be a misnamed early stub; either rename it (it covers a copy/feint mechanic, which is a Memory-Hunter motif), or move it to A3 and add a new A2 row for whatever the L12 fight should actually be.

### A4 L45 — PROTO-OVERLORD: THE GENERAL, 4000 HP
- **P1 Strategist (4000→2800):** artillery strikes, tactical prediction.
- **P2 Direct Command (2800→1400):** Command Blade (60), Strategic Retreat.
- **P3 Warrior's End (1400→0):** pure single combat, military honor.
- **Death:** salutes — "Well fought, human."

### A4 L46 — PROTO-OVERLORD: THE SCIENTIST, 3000 HP
- **P1 Experimenter (3000→2000):** prototype weapons with random effects; specimen summons.
- **P2 Collector (2000→1000):** attempts capture; containment field; forced-evolution injection.
- **P3 Termination (1000→0):** destroys research data if losing.

### A4 L47 — PROTO-OVERLORD: THE PRIEST, 3500 HP
- **P1 Preacher (3500→2400):** Psychic Sermon; Faithful Shield (HUMAN BELIEVERS as human shields — moral challenge).
- **P2 Doubt (2400→1200):** faith shakes as followers die.
- **P3 Crisis (1200→0):** either renewed faith (harder fight) or broken (easier, tragic).

### A4 L48 — PROTO-OVERLORD: THE ENFORCER, 5000 HP
- **P1 Executioner (5000→3500):** Execution Hammer (100); Brutal Charge (70).
- **P2 Berserker (3500→1500):** faster, more aggressive.
- **P3 Final Judgment (1500→0):** Jake's 400%-strength showcase — power vs. power.
- **Death:** "IMPOSSIBLE... NO ONE... RESISTS... JUDGMENT..."

### A4 L49 — PROTO-OVERLORD: THE VOICE, 2500 HP
- **P1 Broadcast (2500→1700):** demoralization debuffs; technology control; shows the player's failures.
- **P2 Desperate (1700→800):** signal hijack; rage broadcast.
- **P3 Truth (800→0):** **CHOICE — destroy the broadcast center OR hijack to broadcast the truth globally.**

### A4 L50 — THE OVERLORD (FINAL), 15,000+ HP across 4 phases
- **P1 The Avatar (4000):** 50ft humanoid composed of shifting organic matter. Cosmic Strike (100), Element Barrage (60× multiple — fire/ice/acid/electric cycles), Minion Wave (10-15 various aliens from conquered worlds), Assimilation Beam (80 + stat loss), World Memory (shows conquered civilizations — psychological). At 0 HP the avatar REFORMS: "DESTRUCTION IS MEANINGLESS. WE ARE INFINITE."
- **P2 The Consciousness (3500):** must destroy **5 neural nodes (700 HP each)** the avatar defends. Memory Absorption (uses player memories for weakness); Identity Attack ("You let [name] die."); Assimilation Offer (power for surrender — temptation). "YOUR MEMORIES ARE DELICIOUS." "JOIN US. YOUR POWER COULD SAVE WORLDS."
- **P3 The Truth (4000):** true eldritch form. The Overlord reveals it's a survivor of a dead universe consuming worlds to delay entropy. Entropy Beam (150), Dimensional Shift, Existential Horror (fear effect — face the death of everything), Final Absorption. "WE WERE LIKE YOU ONCE. A SINGLE WORLD. ALONE. OUR UNIVERSE DIED. WE SURVIVED BY BECOMING... THIS."
- **P4 The Choice (NON-COMBAT):**
  - **A) DESTROY THE AVATAR** — max damage output; all allies attack together; severs Overlord from Earth; Earth free but Overlord returns eventually.
  - **B) NEGOTIATE** — requires high diplomacy; possible with Salvari help; Earth free, Overlord seeks elsewhere.
  - **C) SACRIFICE** — one character volunteers; provides genetic template without conquest; Earth saved at personal cost.
  - **D) FULL ALLIANCE (Golden Ending)** — requires Sarah saved AND present, all companions rescued, Salvari allied, Dr. Chen redeemed. Humanity partners with the Overlord to find an alternative to entropy.
- **Fight scales with allies saved** (more allies = different phase difficulty + support options).
- **Endings:** Golden / Good / Standard / Sacrifice / Bad / Nightmare (last is the "player joins the Overlord" Game-Over-with-credits ending).
- **Engine state:** NONE. The current Boss machine is single-body + HP-fraction phases; no extension supports a 4-phase fight with neural-node phase + non-combat choice phase.

---

## 4. SARAH'S EVIL CLONE (NEW — DESIGN FROM SCRATCH)

> Designed this session in response to the wife's note. NOT in the existing
> `TASK_6_*` bestiary corpus. The template is Jake's Clone (§3 F7); the mirror
> applies to Sarah's HACKER abilities rather than Jake's 400%-strength.

### Concept
**Sarah's Evil Clone** is a perfect cognitive copy of Sarah grown by the Overlord
from samples taken during the F2 breeding-chamber arc. Where Jake's Clone is a
mirror of brute force, Sarah's Clone is a mirror of *cyber-warfare*: every system
Sarah can break, the Clone can break BACK. The Clone exists because the Overlord
recognized — late, in the F5 hack — that Sarah is the more dangerous half of the
pair. The Clone is the Overlord's answer to her existence.

### Identity & Voice
- Cold, precise, contemptuous of "biological inefficiencies." Calls the real
  Sarah "the legacy build."
- Speaks in clipped clinical sentences. No humor.
- Quietly delights in disabling things Sarah BUILT.

### Stats
- **Health:** 1,100 (lower than Jake's Clone — she's the brain, not the body).
- **Damage:** Variable (mostly system-damage / DOT — see attacks).
- **Speed:** Fast (5.5 m/s — Sarah-tier, not Jake-tier).
- **Armor:** Light (15%) — she's never been a frontline fighter.

### Suggested First Appearance
Two design options — pick during integration:
1. **F7 stage-2:** appears AFTER Jake's Clone falls, as the "second twist" of the Act-1 finale. The pair fights — Sarah vs Sarah Clone — using each other's terminals against each other. The player toggles between defending Jake (real Sarah's body) and helping the real Sarah out-hack the Clone.
2. **A3 ambush:** ambushes the Storm Runner in the space-journey arc. Best if Sarah Clone is meant to be a recurring rival rather than a one-shot finale. Lets the Clone come back AFTER F7 even if Jake's Clone died.

Default recommendation: **F7 stage-2** — keeps the "Evil Clones" pair as a single Act-1 finale beat and avoids spreading the morality fork across acts.

### Three-Phase Pattern (mirrors Sarah's hack progression)

**PHASE 1 — THE INTERFACE (1100→750 HP)**
- **Mechanic mirror:** Sarah's "Network Access" pre-hack.
- **Attacks:**
  1. **Turret Hijack** — periodically faction-flips any room turret to hostile against the player for 8s.
  2. **System Sabotage** — drops 30 dmg DOT zones (rectangular floor terminals overload and arc).
  3. **EMP Pulse** — 5m radius, 20 dmg + 2s player-HUD scramble.
  4. **Drone Recall** — if any allied hacked drones are present, attempts to flip them back (5s channel, interruptible).
- **Voice:** "I HAVE EVERY KEY YOU HAVE. SHOW ME WHAT YOU BUILT."

**PHASE 2 — THE OVERRIDE (750→400 HP)**
- **Mechanic mirror:** Sarah's "Override" mid-hack.
- New attacks layered on P1:
  5. **Mirror Hack** — if the player uses an interact, the Clone instantly counter-uses it (door closes behind the player, elevator locks).
  6. **Drone Swarm Summon** — pulls 2-3 BlueSynth/Combat-Drone adds from a cargo hatch (every 25s).
  7. **Identity Spoof** — for 3s, the in-world UI labels her as "Sarah Chen — Ally." Friendly-fire prompt is suppressed. If the player shoots, the real Sarah audibly winces ("Don't — wait — that's NOT —") for narrative weight. No damage penalty; this is a moral/visual gimmick.
- **Voice:** "EVERY DOOR YOU OPEN, I CLOSE. EVERY ALLY YOU MAKE, I TURN."

**PHASE 3 — THE REPROGRAM (400→0 HP)**
- **Mechanic mirror:** Sarah's "Reprogram" — the part that flipped the drone army in F5.
- The Clone is losing. She abandons subtlety:
  8. **Lockdown Pulse** — periodically locks ALL player HUD widgets (compass, ammo readout, objective marker) for 3s.
  9. **Suicidal Recompile** — at 200 HP, the Clone forces a 10-second "recompile" timer. If she survives the 10s, she resets to 600 HP. If interrupted (damage breaks the channel), she drops to 50 HP.
  10. **Final Override** — if she reaches 0 HP without being captured (see victory options), she crashes the F7 elevator system — the player loses the "easy exit" and must take a longer escape route.
- **Voice:** "YOU CANNOT CODE ME OUT. I AM HER. I AM BETTER."

### Victory Options (mirrors the Jake's Clone fork)
1. **KILL** — standard. Drops Encrypted Sarah-Clone Datacore (lore item: Overlord's psychometric scan of Sarah). Sets the "Clone died" world flag.
2. **INCAPACITATE** — Sarah's hacking minigame at the end (3-attempt rhythm match) instead of the killing blow. The Clone survives in a locked containment shell. **Late-game consequence:** appears in Act 3 or Act 4 as a reluctant ally — the Overlord cuts her loose after she sees Sarah's compassion. Highest narrative payoff.
3. **NEURAL HACK (the real Sarah)** — Sarah turns the Clone INTO HERSELF. The Clone becomes a second hacker in the party (one-of-a-kind ally). Rarest path; gated on the player saving the Floor-2 women AND letting Chen live in F2 (so Sarah has the cure formula she needs).

### Weaknesses
- Light armor — physical damage is highly effective if the player can close distance through the DOT zones.
- The **Suicidal Recompile** (10s channel) is a free DPS window — interrupting it costs her 350 HP.
- EMP grenades (carried by Elite Security Guards) interrupt all her active hacks.
- The real Sarah, if present in the party, can contest each Clone hack one-for-one (a "hack duel" minigame).

### Dialogue beats
- (On phase change) "I AM THE OPTIMIZED ITERATION."
- (Mirror Hack on player) "Did you THINK we hadn't WATCHED you?"
- (Drone faction-flip back to hostile) "BELONG TO ME. AGAIN."
- (Phase 3 desperation) "If I cannot have her, NO ONE HAS HER."
- (KILL ending) "Tell — her — she — was — first —"
- (INCAPACITATE ending, in containment) "...the noise stopped. Why is the noise stopped?"
- (NEURAL HACK ending) "...oh. I remember Oliver. The cat. We had a cat."

### Engine impact
- New row in a future `BossType` extension OR in a co-finale `Act1FinaleBoss` enum alongside Jake's Clone.
- Needs three new mechanic types not present in `MonsterSystem` today:
  - **Faction-flip-on-cooldown** (the Turret Hijack / Drone Recall) — extension of `ScriptedFightHook::flipToAllied` to be re-triggerable mid-fight, not just once.
  - **DOT floor zones** — new hazard primitive (could reuse a trigger volume with a periodic damage application).
  - **HUD scramble / lockdown** — a host-side HUD effect callback; pure Application-layer.
- Can SHARE most attack code with Jake's Clone if implemented as two `Tuning` rows of one "Clone" archetype.

---

## 5. STUFF MISSING FROM THE CURRENT ENGINE (concrete: which `monster.{h,cpp}` rows are missing)

### What's in `monster.{h,cpp}` today (verified 2026-05-31)
- `enum class MonsterType { Guard, Drone, Boss }` — the three AI shapes.
- `enum class EnemyType { DominionTrooper, Verthani, Illuminated, BlueSynth, Count=4 }` — Act-1 roster, 4 rows.
- `enum class BossType { DrChen, FailedExperiment7, AlienOverseer, Count=3 }` — Act-1 single-body mid-bosses.
- `MultiPodBoss` + `chorusConfig()` — F4.5 Chorus, 5 pods, save-up-to-4.
- `ScriptedFightHook::{stripBossHp, flipToAllied, performMasterHack}` — F5 Master Hack gameplay-state half.
- `enum class Act2EnemyType { SalvariAlly, NativeDesertFauna, MutatedScientist, MutatedFlora, SurfacePursuitDrone, Count=5 }` — Act-2 roster, 5 rows.
- `enum class Act2BossType { MemoryHunter, TheSiren, BreederQueen, GarrisonCommander, Count=4 }` — Act-2 bosses, 4 rows.
- `enum class CanonAlien { SaurianSoldier, SaurianWarlord, GreyTasked, NordicSteward, MantisArbiter, Count=5 }` — canon-aliens, 5 rows (includes 1 boss: SaurianWarlord at A2 L10).

**Total engine rows today:** 4 Act-1 regular + 3 Act-1 single-body bosses + 1 Act-1 multi-pod boss (Chorus) + 5 Act-2 regular + 4 Act-2 bosses + 5 canon-aliens = **22 stat blocks** for the 78-entry bible roster. **~28% coverage.**

### Missing rows the engine NEEDS (data-only, no new code)

These all fit the existing `Guard` / `Drone` / `Boss` archetypes and add as new `MonsterDef`/`Act2EnemyDef`/`Act2BossDef` rows with no new behavior:

#### Act-1 regular gaps (rows to add under existing `EnemyType` enum or beside it):
1. **Riot Shield Guard** (#3) — Guard archetype, high HP front-armor (model the 500 HP shield as an HP bump until destructible-shield primitive lands).
2. **Lab Scientist (Panicked)** (#4) — non-combat NPC; ally-tagged with 0 damage; needs a "flee" AI variant OR can reuse `Act2EnemyType::SalvariAlly`'s allied-statted-with-0-damage pattern.
3. **Lab Scientist (Corrupted)** (#5) — Guard, low HP, melee with bleed-DOT (engine needs only the bleed; can use existing damage).
4. **Surveillance Drone** (#6) — Drone, very low HP, weak taser ranged.
5. **Medical Drone** (#8) — Drone, low HP; **infection-injection** is a new system (see "Needs new systems" below).
6. **Breeder Drone** (#9) — Drone, capture mechanic is a new system.
7. **Hacker Drone** (#10) — Drone, **ally-only** (Sarah's; no hostile variant); needs ally-spawn machinery (some exists via `startAllied`).
8. **Infected Stage 2** (#12) — Guard, higher HP, wall-climb approximated as aggressive flanking (high strafeBias).
9. **Infected Stage 3 / Hybrid** (#13) — Guard, commander class; needs summon-on-cooldown (the existing Phase3 summon-once is too coarse).

#### Act-1 boss row gaps:
10. **Karen Mitchell (Volunteer Coordinator)** (#21) — needs a new `BossType` row for the mid-boss arc OR a `BossType::KarenMitchell`. Stats undefined in the corpus; design pass needed.
11. **Jake's Clone** (#20) — design-noted in `EFLZ_DESIGN.md` but NOT a `BossType` enum row. Add as `BossType::JakeClone`.
12. **Sarah's Evil Clone** (#64) — NEW this session. Add as `BossType::SarahClone` (paired with Jake's Clone).

#### Act-2 gaps:
13. **Sand Burrower** (#23) — needs new behavior (burrow/emerge cycle; underbelly weak-point).
14. **Swamp Stalker** (#24) — Guard, water regen is a new mechanic.
15. **Toxic Hulk** (#25) — Boss, fits the existing `Boss` archetype; add as Act-2 boss row.
16. **Salvari Scavenger (Hostile)** (#26) — Guard; convertible-via-K'thara needs a diplomacy hook.
17. **K'thara as combat companion** (#28) — currently placed as a survivor marker; needs combat-ally stats.

#### Memory Hunter location conflict (#49):
- Engine has `Act2BossType::MemoryHunter`; bible places Memory Hunter at A3 L30. Either:
  - **Move it:** rename current row to a fitting A2 boss (the copyFeintPhase descriptor is generic enough to apply to other identity-mimic mechanics), and create an `Act3BossType` enum.
  - **Keep it:** rule that the engine's A2 row is a "Memory Hunter early appearance" (recon variant); add the L30 fight as a separate Act-3 row.

#### Act-3 gaps (entirely missing — 8 rows):
18-25. Pirate Raider, Pirate Captain Vex, Overlord Shock Trooper, Overlord Elite Warrior, Overlord Fighter Craft (vehicle), Corrupted Crew Member, Corrupted Station Commander, **Memory Hunter at L30** (if not handled above).

#### Act-4 gaps (entirely missing — 12 rows):
26-37. Collaborator Militia, Collaborator Enforcer, Infected Soldier, Infected Tank (vehicle), Overlord Heavy Trooper, Overlord Assassin (stealth — needs new mechanic), Proto-Overlord ×5, THE OVERLORD final boss.

### Needs NEW systems (not just new data rows)

These cannot be expressed as a new `MonsterDef` row alone — they require code beyond the current archetypes:

1. **Infection timer + cure** — the 9-minute 4-stage transformation pipeline (Medical Drone → Stage 1 → 2 → 3 → Game Over). Player-side system + UI timer + cure-item interaction. The bible spec is at `TASK_6_..._BESTIARY` lines 1955-2003.
2. **Drone hacking / faction-flip on demand** — `ScriptedFightHook::flipToAllied` exists for the F5 one-shot; the bible wants it as a CONTINUOUS Sarah-player ability (each drone has an 8-15s hack time, the player Sarah-mode drives it). System spec at `TASK_6_..._BESTIARY` lines 2007-2041.
3. **Multi-target boss with non-Chorus geometry** — `MultiPodBoss` handles the Chorus's 5 simultaneous bodies. The Overlord P2 needs the same primitive but with 5 NEURAL NODES the avatar DEFENDS — a different placement layout (nodes are AROUND the avatar, not the avatar's body). Likely reuses `MultiPodBoss` with one "core" pod (the Avatar, HP 4000) + 5 "node" pods (700 HP each).
4. **Non-combat boss phase** — the Overlord's P4 is a dialogue choice, not combat. Needs a boss-completion hook that fires a host UI choice and reports the result back. Currently the Boss machine assumes Phase3 = combat-to-death.
5. **Adaptive AI** — Swarm Controller P2 "learns player patterns." At a minimum: a counter-pattern (e.g. choose the LEAST-used dodge direction). System is generic enough to live on the `MonsterSystem` as a Tuning toggle.
6. **Identity / memory phase** — Memory Hunter P3's "assert identity against all memories" is a host-UI minigame (likely a Simon-says of past-companion names), not combat. Same hook shape as the Overlord P4.
7. **Bullet-time / 50ft-scale fights** — The Overlord P1 (50ft avatar) needs scale-aware combat that doesn't break the existing 1.9m melee reach. Probably a separate `BossPhase::LargeForm` tag that scales reach + damage radii.
8. **HUD scramble / lockdown effect** — needed for Sarah's Clone (#64) and the Overlord's psychic attacks. Application-layer effect callback.
9. **Vehicle-type enemies** — Infected Tank (#53), Overlord Fighter Craft (#46), Overlord Heavy Trooper (#54)'s ground-slam — these need armor zones (treads/rear/hatch as separate damage proxies). A `DamageZone` extension to `MonsterSystem`.
10. **Stealth / cloaking** — Overlord Assassin (#55) and Mantis Arbiter's Veil. Needs visibility flag tied to LOS detection threshold and detection-via-utility (drones, Sarah-mode highlights).
11. **Karen Mitchell-style "psychological mid-boss"** — not combat-heavy; a dialogue gauntlet with HP-as-trust. The wife's-note flagged this; design pending.

---

## 6. FACTION OVERVIEW + CROSS-REFERENCES

EFLZ's lore (per `docs/MASTER_GAME_PLAN.md`, the existing `EFLZ_BESTIARY.md` correction §4, and the
TASK_6 corpus) organizes ~78 enemies into 7 high-level factions, all subordinate to the
Overlord hive except humans and Salvari refugees.

### THE OVERLORD (the umbrella)
- **Definition:** A collective consciousness spanning 2,847 conquered worlds (corpus line 1607); active for 100,000+ years.
- **Earth manifestation:** the avatar of P1/P2/P3 of the final boss (#61).
- **What it actually is (P3 reveal):** the last remnant of a dying universe absorbing worlds to delay entropy.
- **All other "Overlord X" factions below are SUBJECT RACES / SERVANT FORCES of this entity.**

### DOMINION (the Greys / builders of Lab Zero)
- **Per `EFLZ_BESTIARY.md` correction §4:** multi-armed greys who *built* Lab Zero and run the breeding program.
- **Engine name carry:** `EnemyType::DominionTrooper` (which is statted as Security Guard Basic — a stand-in until the canon Grey-tier soldier is added). Canon-aliens `GreyTasked` is the worker-drone variant of this faction (worker-drone serving the Reptilians, per `canon_aliens.h:14`).
- **Roster:** Security Guards (Basic/Elite/Riot Shield) are the Earth-side enforcers; Lab Scientists (Panicked/Corrupted) are the staff; Drones (Surveillance/Combat/Medical/Breeder) are the automation.

### VERTHANI (the Insectoid Warriors)
- **Per `EFLZ_BESTIARY.md` correction §4:** insectoid warriors. The engine name `EnemyType::Verthani` is statted as Infected Stage 1 (the fast claw enemy) — the bible's "infected" line IS the Verthani transformation in canon. The "infection" is alien-signal mediated and the end-state (Stage 3) IS Verthani-form.
- **Canon-alien parallel:** `CanonAlien::MantisArbiter` is the insectoid assassin tier — Verthani-adjacent in the lore mapping.
- **Roster:** Infected Stage 1/2/3, the transformed-women bosses (Aria/Keisha/Emily are Verthani-line transformations in Timeline Beta).

### THE ILLUMINATED (the Energy-Being Elite)
- **Per `EFLZ_BESTIARY.md` correction §4:** energy-being elite.
- **Engine name carry:** `EnemyType::Illuminated` (statted as Security Guard Elite — a stand-in).
- **Bible reading:** the Illuminated are likely the psychic/energy-fluent tier — the Alien Overseer (#30 = `BossType::AlienOverseer`) and The Oracle/Emily (#39) read as Illuminated-line. The Voice (Proto-Overlord, #60) is also Illuminated-tier propaganda-form.

### SALVARI (the bioluminescent refugee allies)
- **Per `EFLZ_BESTIARY.md` correction §4:** the bioluminescent refugee allies — a conquered race that escaped to Keth'zar and survives in hidden camps.
- **Roster:** Salvari Scout (#27 = `Act2EnemyType::SalvariAlly`), K'thara (#28), Salvari Scavenger Hostile (#26 — corrupted/desperate Salvari).
- **Canon-alien parallel:** `CanonAlien::NordicSteward` is the peaceful mentor, Salvari-adjacent.
- **Best ending requires Salvari alliance** (final boss P4 D requires Salvari allied).

### THE OVERLORD'S DIRECT SOLDIERS (Act 3+)
- The aliens that fight under the Overlord directly, distinct from the subject-race servants:
- Overlord Shock Trooper (#44), Overlord Elite Warrior (#45), Overlord Heavy Trooper (#54), Overlord Assassin (#55), Overlord Overseer (#30).
- **Canon-alien parallel:** `CanonAlien::SaurianSoldier` + `SaurianWarlord` — the Reptilian Overlord enforcers per `canon_aliens.h:14`. They're the Act-2 visible-soldier face of the Overlord.

### HUMANS (Earth)
- Three sub-factions:
  - **Lab Zero Security** (Acts 1) — Dominion-aligned employees.
  - **Lab Zero Staff** (Acts 1) — Scientists, mostly victims; Chen / FE#7's David are case studies.
  - **Collaborators** (Act 4) — Earthlings serving the Overlord post-invasion. Militia + Enforcer + Infected Soldier.

### HYBRIDS (the transformation line)
- The Verthani-line is the hybrid pipeline: Medical Drone → 9-min timer → Stage 1 → 2 → 3.
- **Timeline Beta bosses** (Aria/Keisha/Emily) are end-state hybrids of specific named NPCs.
- **Corrupted Crew Member** (#47) and **Infected Soldier** (#52) are space-side and Earth-side variants of the same line.

### Cross-references (where each faction appears in this doc)
- **Dominion:** rows #1-#10, #14-#15, #16-#18.
- **Verthani:** rows #11-#13, #37-#39, #47, #52.
- **Illuminated:** rows #19, #30, #58, #60.
- **Salvari:** rows #26-#29.
- **Overlord direct soldiers:** rows #30, #31-#33, #44-#46, #54-#55, #56-#61.
- **Humans / Collaborators:** rows #50-#53.
- **Hybrids:** rows #11-#13, #37-#39, #47, #52, #62 (CrazyDrone, in the broadest sense — Pilot Zero is "hybrid"-coded).
- **Wildcards / proto:** row #62 (CrazyDrone).

---

## 7. BUILD ORDER (which bosses fix the engine's "generic graybox" problem first)

The engine today has FOUR canon Act-1 enemy rows + Chief Martinez + the Chorus + ONE Act-2 boss (Saurian Warlord) actually placed in a level. That means most non-spire encounters read as a generic graybox of "Guard / Drone" with no canon name attached. The fastest way to fix THAT — the user-visible feeling that "this is just a tech demo" — is to land bosses (not regular-enemy expansion), because bosses do the heaviest narrative lifting per row added.

**Recommended order (rough effort low → high):**

### Wave A — Wire what's already half-shipped (smallest delta, highest perceived impact)
1. **Dr. Chen's KILL-vs-CURE choice** (#15) — the `BossType::DrChen` row + `m_hasCureOption` plumbing exists. Wiring this gives F2 a real morality fork on the SHIPPED boss row, which the playtest will notice immediately. Adds ~1 cutscene + 1 interact.
2. **Failed Experiment #7's Memory-Flash window** (#16) — `BossType::FailedExperiment7` row exists with memoryFlash window in tuning; needs the floor module to call the host-side flash hook + the +20% damage window in P2/P3.
3. **Alien Overseer (F6) placement** (#19) — the row exists; the floor needs to spawn it as the F6 boss with a basic psi-blast + summon-3 behavior. Validates the third single-body boss row.
4. **Saurian Warlord Adaptive-Hide** (#40) — already placed; add the last-damage-type 60% resist 8s mechanic flagged in `canon_aliens.h:25`. One Tuning field + a per-frame tracker.

### Wave B — Single-body bosses the engine can express today (data-only rows)
5. **Jake's Clone (F7)** (#20 / #63) — add `BossType::JakeClone` row, then add a single-body Mirror-Match phase machine (P2 telekinesis = ranged + AOE, P3 reckless = phase-dodge approximated as a 0.3s phase invulnerability). This finishes Act 1.
6. **Toxic Hulk (A2 L12)** (#25) — Boss, 2 phases, fits today's machine. One Act-2 boss row.
7. **Pirate Captain Vex (A3 L23)** (#43) — Boss-lite, 2 phases + negotiation option (negotiation reuses the F2 cure choice's interact hook).

### Wave C — Multi-pod / scripted bosses (extensions of existing machines)
8. **Swarm Controller AI (F5)** (#18) — the `ScriptedFightHook` exists; add the `BossType::SwarmController` row + the holographic avatar (a high-HP Drone-archetype body that PROJECTS from the controller's actual position) + the Emergency Purge timer as a P3 "must-flee" beat.
9. **The Siren / Breeder Queen (A2 Beta L18/L19)** (#37, #38) — `Act2BossType::TheSiren` + `BreederQueen` rows exist; both need parasite-spawn (Siren) and Stage-2 summon (Queen) behaviors. The Breeder Queen has `phase3SummonCount > 0` flagged in tuning — wire it.
10. **Garrison Commander (A2 L20)** (#41) — row exists; mech-suit P2 = bigger model + reach buff; orbital-strike P3 = the existing `escapeTimerSeconds` field needs floor-side wiring to a "GTFO timer" UI.

### Wave D — New systems (the bosses that require code, not just data)
11. **Memory Hunter (A3 L30)** (#49) — extend or move the existing engine row. The Identity Crisis P3 needs the non-combat-phase hook (which the final boss also needs — build it generically here).
12. **Sarah's Evil Clone (NEW)** (#64) — leverages the same Faction-flip + DOT-zone + HUD-scramble extensions Wave-D needs ANYWAY. Best to land alongside Jake's Clone in Wave B but with a follow-up patch when those primitives exist.
13. **Karen Mitchell (volunteer-coordinator mid-boss)** (#21) — design pass first (stat-light in corpus); then a dialogue-gauntlet boss using HP-as-trust.

### Wave E — Act 3 / Act 4 / Final
14. Act 3 regulars (Pirate Raider, Overlord Shock Trooper/Elite Warrior, Fighter Craft, Corrupted Crew, Corrupted Station Commander).
15. Act 4 regulars (Collaborator Militia/Enforcer, Infected Soldier/Tank, Overlord Heavy Trooper/Assassin).
16. **Proto-Overlords ×5 (L45–L49)** — five 3-phase boss rows; each can ride the existing single-body Boss machine if the morality gimmicks (The Priest's "human shields," The Voice's "destroy vs hijack" choice) are deferred to data-light versions.
17. **THE OVERLORD (L50)** — the engine-buster. Needs every primitive Wave D started: multi-target (`MultiPodBoss` reused for the 5 neural nodes), large-scale boss, ally-scaled difficulty, **non-combat P4 choice phase**, six endings. THIS is what every other wave is leading up to.

### Why this order de-graybox-es the engine fastest
- Wave A makes the FIRST THREE BOSSES the player actually fights (F1 Martinez, F2 Chen, F3 FE#7) feel canon-correct without any new code.
- Wave B doubles the boss count (adds F7, A2 L12, A3 L23) — turning "two bosses, then graybox" into "five bosses, then graybox."
- By the end of Wave C the player can fight a full Act-1 (F1→F7) + a meaningful slice of Act-2 (L10 Warlord, L12 Hulk, L18-L20 Beta) — that's nine bosses, half the bible's 18.
- Wave D unlocks Memory Hunter, which is the FIRST boss the bible considers "psychologically signature" — landing it validates the non-combat-phase primitive needed for the Overlord.

---

## SOURCE INDEX
- `G:\Unity_Projects\PARALLEL_TASKS_8_CHATS\TASK_6_ESCAPE_LAB_48_COMPLETE_ENEMY_BESTIARY.md` (2102 lines; the primary v1.0 FINAL bestiary).
- `G:\Unity_Projects\PARALLEL_TASKS_8_CHATS\TASK_6_ENEMY_BESTIARY.md` (12 KB earlier task brief; superseded by the above but consistent).
- `G:\X3Native\docs\design\EFLZ_BESTIARY.md` (the in-repo canonical digest from 2026-05-22; 265 lines; contains the faction-name correction notes and the engine-vs-bible reconciliation §4).
- `G:\X3Native\docs\design\EFLZ_MASTER_PLAN.md` / `EFLZ_NARRATIVE.md` (lore + plot).
- `G:\X3Native\app\monster.h` (lines 78-265 archetype + boss machine; lines 1075-1399 the data tables).
- `G:\X3Native\app\monster.cpp` (~line 1946: the `MonsterDef` table).
- `G:\X3Native\app\canon_aliens.h` (the 5-row canon-alien roster + flagged extensions).
- `G:\X3Native\app\act2_desert.{h,cpp}` (the A2 L10/L11 placement that hosts Saurian Warlord + Mantis Ambush + Nordic Mentor).
- Floor implementation docs (Unity-side, for context): `TASK_9_FLOORS_1-2`, `TASK_1_FLOORS_3-4`, `TASk_2_FLOORS_5-7_COMPLETE`, `TASK_10_..._SIDE_QUESTS`, `TASK_14_..._AUDIO_DESIGN`, `TASK_3/4/5_*` act overviews.

