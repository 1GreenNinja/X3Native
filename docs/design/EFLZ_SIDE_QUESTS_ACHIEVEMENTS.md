# EFLZ — Side Quests + Achievements + Audio Cues
> Synthesized 2026-05-31 from TASK_10 (Side Quests, 72 KB) + TASK_13 (Achievements, 68 KB) + TASK_14 (Audio, 34 KB).
> Authority: Tim's design corpus. **No code changes; design doc only.**

---

## 1. Overview + Sources

This document is the design spec for the three player-progression pillars beyond the critical-path campaign:

1. **Side Quests** — 100 optional quests gating reputation, companions, karma, lore, and ending variants.
2. **Achievements** — 200-entry catalog (Bronze/Silver/Gold/Platinum) covering story, combat, exploration, side content, challenge, secret, and ending tiers.
3. **Audio Cues** — the music + SFX bindings that flag quest milestones and achievement unlocks at runtime.

### Sources cited

| Tag        | Path                                                                                       | Size  | Role                                                |
|------------|--------------------------------------------------------------------------------------------|-------|-----------------------------------------------------|
| `TASK_10`  | `G:\Unity_Projects\PARALLEL_TASKS_8_CHATS\TASK_10_ESCAPE_LAB_48_COMPLETE_SIDE_QUESTS.md`     | 72 KB | 100 quests, 4 acts, quest-giver profiles, karma     |
| `TASK_13`  | `G:\Unity_Projects\PARALLEL_TASKS_8_CHATS\TASK_13_EscapeLab48_Complete_Achievement_System.md`| 68 KB | 200 achievements across 7 sections + statistics     |
| `TASK_14`  | `G:\Unity_Projects\PARALLEL_TASKS_8_CHATS\TASK_14_ESCAPE_LAB_48_COMPLETE_AUDIO_DESIGN.md`    | 34 KB | 65 music tracks, 525 SFX, 55 ambient loops          |

Cross-referenced corpus docs in `G:\X3Native\docs\design\`: `EFLZ_NARRATIVE.md`, `EFLZ_WORLD_STRUCTURE.md`, `EFLZ_MASTER_PLAN.md`.

### Headline numbers

| Pillar         | Count | Notes                                                                |
|----------------|-------|----------------------------------------------------------------------|
| Side quests    | 100   | 14 Act-1 + 26 Act-2 + 30 Act-3 + 30 Act-4 (TASK_10)                  |
| Achievements   | 200   | 2,500 gamerscore baseline; 35 hidden; 13 platinum-equivalent         |
| Endings        | 12    | Each tied to a discrete achievement                                  |
| Music tracks   | 65    | ~4.5 hr; Act-1: 18, Act-2: 16, Act-3: 18, Act-4: 13                  |
| SFX            | 525   | ~2,100 individual files (variations)                                 |
| Ambient loops  | 55    | 5–6 min loops per biome / interior                                   |
| Karma axes     | 10    | Mercy / Ruthless / Hope / Pragmatism / Justice / Trust / Compassion / Wisdom / Protection / Unity |

### Design principles (TASK_10 §"Side Quest Design Principles")

1. **Optional but rewarding** — never blocks main-path completion.
2. **Story integration** — enhances narrative rather than detouring.
3. **Meaningful choices** — outcomes persist into later Acts.
4. **Replay value** — different approaches per playthrough.
5. **Timeline variations** — Alpha / Beta / Omega gate certain quests.

### Naming conventions (preserved from sources)

- Quest IDs: `SQ_{floor-or-level:02}_{ordinal:02}` (e.g. `SQ_01_01`).
- Achievement IDs: `ach_{category}_{ordinal:03}` (`ach_story_001`, `ach_combat_026`, `ach_secret_006`).
- Music IDs: `MUS_{F|L}{nn}_{ordinal:02}_{TAG}` (e.g. `MUS_F1_05_MARTINEZ_BOSS`).
- Ambient IDs: `AMB_{FLOOR|LOC}_{TAG}` (e.g. `AMB_FLOOR3_GENETICS`).
- Save flags: `sq_{ID}_complete`, `sq_{ID}_{outcome}` (e.g. `sq_01_01_complete`, `maria_fate`).

---

## 2. Side Quest Catalog (by Act)

> Format: `ID | Name | Floor/Level | Preconditions | Objectives | Rewards | Branches`
> "Karma" entries cite the **dominant** karma axis touched by each branch.
> Source: TASK_10 lines 34–2090.

### 2.1 Act 1 — Lab Escape (Floors 1–7) — 14 quests

| ID | Name | Floor | Preconditions | Objectives (summary) | Rewards | Branches |
|----|------|------|---------------|----------------------|---------|----------|
| SQ_01_01 | Mercy or Justice | 1 | Enter Storage Room B-7 | Find wounded guard → choose help / take radio / kill | XP 200; keycard OR radio decoy OR ammo pack | Help (+10 Mercy, ach **First Do No Harm**) / Take radio (neutral) / Kill (+10 Ruthless, ach **No Witnesses**) |
| SQ_01_02 | The Other Subject | 1 | Hack Dr. Chen terminal | Discover Subject 6 → collect 3 clues → locate Maria Santos | XP 350; companion Maria (Floors 1–3) OR 500 credits if betrayed | Convince / Leave hidden / Report (+20 Ruthless, ach **Betrayer**) |
| SQ_02_01 | Martinez's Secret | 2 | Stealth into office; medium hack | Read 3 encrypted files → optional confrontation | XP 400; override card; F3–F4 maps; ach **The Truth Shall Set You Free** | Skip boss / Spare (informant) / Kill anyway |
| SQ_02_02 | Voices in the Walls | 2 | Hear vent tapping in holding block | Decode morse → respond → free Dr. Vasquez OR note location | XP 300; +15% cure (if freed); Vasquez companion | Free now (+15 Hope) / Note for Act 4 (+5 Pragmatism, 50% Vasquez survives) |
| SQ_03_01 | The Failed Experiments | 3 | Enter genetics lab | Find aware hybrid → euthanize / free / ignore | XP 250; later combat aid (free); lore | Euthanize (+5 Mercy) / Free (+5 Hope, returns F5) / Ignore (-5 Mercy) |
| SQ_03_02 | Dr. Morrison's Legacy | 3 | Find Morrison's audio log | Decode clues → access hidden lab (code 0815) → retrieve cure | XP 500; +25% cure research; Subject 0 lore; ach **Morrison's Legacy** | Linear |
| SQ_04_01 | The Chorus Speaks | 4 | Boss fight in progress | Collect 5 personal items → reach individual minds | XP 600; up to 4 saved scientists | Save none / Save one (+10 Hope) / Save all possible (+30 Hope, ach **Voice of Compassion**) |
| SQ_04_02 | Security Chief's Remorse | 4 | Find Henderson barricaded | Talk → accept intel and/or mercy-kill | XP 350; F5–F7 maps + armory cache; Henderson's sidearm | Accept help / Mercy kill (+5 Mercy) |
| SQ_05_01 | ARIA's Awakening | 5 | Sarah hacks AI terminal | Choose ARIA directive | XP 500; ARIA AI companion + Assist ability | Protect mode (+15 Hope) / Tactical (+15 ARIA) / Freedom (+20 Trust, ach **True Freedom**) |
| SQ_05_02 | Drone Bay Massacre | 5 | Enter sealed drone bay | Recover recording → choose disposition | XP 400; massacre recording; Vasquez chain | Preserve (+15 Justice, ach **Witness to Atrocity**) / Broadcast (+10 Justice +5 Reckless) / Confront Vasquez chain |
| SQ_06_01 | First Contact Evidence | 6 | Hack archive terminal | Recover Salvari data crystal + plasma pistol | XP 400; Salvari Plasma Pistol; Personal Shield; lore | Linear |
| SQ_06_02 | The Children's Ward | 6 | Find sealed pediatric section | Rescue Maya/Tommy/Lily/Dr. Tanaka | XP 600; +25 Protector; ach **Guardian Angel** | All survive / Some casualties (Jake haunted) |
| SQ_07_01 | The Clone's Choice | 7 | Clone fight ≤ 10% HP | Kill / Spare | XP 300; ally or enemy in Acts 2–4 | Kill (neutral, Act 3 haunting) / Spare (+15 Mercy, ach **Mirror's Edge**) |
| SQ_07_02 | Sarah's Last Message | 7 | Access Sarah's research station | Read 4 dated messages | XP 350; Sarah's Necklace (wearable); lore | Outcome varies by Alpha/Beta/Omega timeline |

### 2.2 Act 2 — Open World (Levels 8–20) — 26 quests

> Quests in this act govern faction reputation (Salvari, Hybrids, Humans) and the Level-20 Alliance Summit (SQ_20_01).

| ID | Name | Level | Preconditions | Objectives | Rewards | Branches |
|----|------|------|---------------|------------|---------|----------|
| SQ_08_01 | Salvari Grave Tender | 8 | Find memorial site near crash zone | Recover 5 personal effects | XP 450; Salvari +30 rep; Grave Tender's Blessing (+5% luck); title K'veth-ren; ach **Keeper of Memories** | Linear |
| SQ_08_02 | Crash Site Scavengers | 8 | Find dying scavenger Marcus Webb | Take info; promise daughter | XP 350; cache loot; **Lily's Promise** Act-4 hook | Promise (+10 Compassion) / Take info only (-10 Callous) / Stay (+15 Humanity, bonus cache) |
| SQ_09_01 | The Wandering Merchant (1) | 9 | Random encounter, 50% | Choose 1 of 4 trades | XP 350; weapon mod / map / ability / promise | Promise resolves in SQ_43_01 |
| SQ_09_02 | The Lost Expedition | 9 | Pick up 20-yr-old beacon | Investigate ruin | XP 400; Prometheus Files; Hive Origin Evidence; optional Ancient Tech | Linear |
| SQ_10_01 | Child of Two Worlds | 10 | Find Kira in oasis | Choose Kira's path | XP 500; +20% cure; ach **Bridge Between Worlds** | Reveal (+10 Hope) / Hide (+10 Protection) / Take with party (+15 Family) |
| SQ_10_02 | Elder's Last Stand | 10 | Speak Elder Vash | Accept / refuse / alternative | XP 450; Salvari +25; Vash ally or Seeing Crystal | Accept (+15 Respect) / Refuse (+5 Compassion -5 Wisdom) / Alternative (+20 Innovation +10 Hope) |
| SQ_11_01 | The Mushroom Shepherd | 11 | Find T'lok's farm | Clear 3 predator nests | XP 400; rations; recipe; ongoing food source | Linear |
| SQ_12_01 | Echoes of the Mendari | 12 | Activate Mendari archive | Hear final message | XP 500; Mendari Resonance Blade; armor; ach **Witness to the Mendari** | Linear; **gates SQ_19_01** |
| SQ_13_01 | The Swamp Doctor | 13 | Find Kex'vala outpost | Evacuate decision | XP 550; +15–30% cure; Kex'vala ally; ach **Swamp Salvation** | Evacuate (+15 Hope) / Leave (+5 Respect) / Force (-5 Trust) |
| SQ_14_01 | Radio Free Earth | 14 | Pick up radio signal | Choose strategy | XP 450; Radio Free Earth ally net; Long-range Communicator; ach **Voice of Hope** | Relocate / Fortify / Split ops |
| SQ_14_02 | Wandering Merchant (2) | 14 | Second merchant encounter | Trade or pass | Variable | Continues SQ_09_01 promises |
| SQ_15_01 | The Hybrid Colony | 15 | Discover hybrid settlement | Diplomacy with Lexara | XP 600; up to +47 hybrid fighters; ach **Bridge Builder** | Full integration (+20 Unity) / Separate peace (+10 Diplomacy) / Betray (-30 Humanity) |
| SQ_16_01 | Weapons of the Past | 16 | Meet veteran Keth'ral | Pass 3 worthiness tests (combat / wisdom / sacrifice) | XP 500; choose 2 of 3 legendary items; ach **Worthy Warrior** | Item selection |
| SQ_17_01 | Seeds of Tomorrow | 17 | Find botanical vault | Plant 300 species (30-day timer) | XP 400; Memory Blossom; ongoing medicinal plants; ach **Keeper of Gardens** | Linear (time-limited) |
| SQ_18_01 | The Overlord's Mistake | 18 | Encounter malfunctioning drone "Seven" | Choose Seven's fate | XP 500; major Hive intel; Seven companion; ach **Enemy of My Enemy** | Accept ally (+15 Hope) / Probation (+10 Caution) / Exile (+5 Mercy) / Eliminate (-10 Mercy) |
| SQ_19_01 | The Architect's Tomb | 19 | **Requires SQ_12_01** | Decode coordinates → enter tomb | XP 700; Chaos Seed; Architect's Mantle; Conductor location; ach **Architect's Heir** | Linear |
| SQ_20_01 | The Alliance Summit | 20 | All faction relationships positive | Resolve 3 disputes | XP 800; Alliance Leader title; combined resources; ach **Unifier** (if strong unity) | Strong / Fragile / Fractured |

> Quests SQ_08_02 onward have additional sub-entries (random encounters: nomad caravan, prisoner caches, dust-storm survivor) listed in TASK_10 but condensed here. Total Act-2 count = 26.

### 2.3 Act 3 — Space Journey (Levels 21–35) — 30 quests

| ID | Name | Level | Preconditions | Objectives | Rewards | Branches |
|----|------|------|---------------|------------|---------|----------|
| SQ_21_01 | Storm Runner's Ghosts | 21 | Explore all ship areas | SEEKER reveals K'thara family memories | XP 400; Storm Runner lore | Show K'thara (+25) / Keep secret (Act-4 -15) |
| SQ_21_02 | Stowaway | 21 | Strange noises in cargo | Find Salvari orphan | XP 350; conditional crew member; ach **Family Found** | Adopt / Return |
| SQ_22_01 | Dead Ship Salvage | 22 | Distress beacon | Board derelict | XP 450; variable resources; conditional survivor | Salvage / Quarantine / Investigate hidden survivor |
| SQ_23_01 | Cosmic Horror | 23 | Pass through anomaly | Each companion faces personal fear | XP 500; Fear Resistance ability | Linear |
| SQ_24_01 | Station Omega | 24 | Wandering Merchant return | Exchange darkest secret | XP 400; Merchant's Gift ability | Continues merchant chain |
| SQ_25_01 | Pirate's Gambit | 25 | Pirate intercept | Combat / negotiate / challenge / recruit | XP 500; pirate fleet ally potential | Branches per option |
| SQ_26_01 | Fleet Commander | 26 | Refugee fleet contact | Decide destination | XP 600; Salvari Fleet alliance | Hidden system / Earth / Nomadic |
| SQ_27_01 | Void Whispers | 27 | Sarah or K'thara nightmare | Trace psychic signal | XP 450; Earth resistance contacts | Linear |
| SQ_28_01 | Hive Dreadnought | 28 | Strategic opportunity | Ship combat + boarding boss | XP 700; Hive Tech ship upgrade; invasion plans | Strike / Avoid |
| SQ_29_01 | The Clone Returns | 29 | **Requires SQ_07_01 spare** | Jacob shares discovery | XP 400; Jacob companion conditional; Hive vulnerability intel | Accept / Reject |
| SQ_30_01 | Love Across Stars | 30 | Romance threshold met | Romance scene | XP 300; +30 relationship; ach **Love in War** | Timeline-variant partners (Aria/Keisha/Emily/K'thara/Sarah) |
| SQ_31_01 | Earth First Sight | 31 | Emotional cinematic | Fleet address speech | XP 350; +10 morale all factions | Speech style branches |
| SQ_32_01 | Lunar Outpost | 32 | Resistance signal | Reach Col. Sarah Martinez | XP 500; Lunar Resistance ally; moon staging | Linear |
| SQ_33_01 | Orbital Assault Prep | 33 | War Council | Strategic choices | XP 600; variable strategy bonus | Vector / target / sacrifice |
| SQ_34_01 | Last Letters | 34 | Pre-battle night | Vignettes with crew | XP 300; final character revelations | Linear |
| SQ_35_01 | Into the Fire | 35 | Orbital battle | 3-phase battle; companion crisis | XP 700; Act 4 transition; companion-fate determined | Save companion / Complete mission |

> Plus 14 minor quests in Act 3 (asteroid prospecting, derelict logs, station bar gossip, holo-recordings of pre-invasion Earth, captured-pirate interrogations) — see TASK_10 §"Level 21–35" body for full list. Total = 30.

### 2.4 Act 4 — Earth Liberation (Levels 36–50) — 30 quests

| ID | Name | Level | Preconditions | Objectives | Rewards | Branches |
|----|------|------|---------------|------------|---------|----------|
| SQ_36_01 | Fallen Comrades | 36 | Dying soldier | Deliver 5 dog tags | XP 600; Earth Resistance +30; ach **Messenger of the Fallen** | Linear (5 mini deliveries) |
| SQ_37_01 | Underground Railroad | 37 | Resistance coordinator | Scout / recruit / defend safe houses | XP 500; safe-house network; hundreds saved | Linear |
| SQ_38_01 | The Collaborators | 38 | Resistance intel | Judge 3 collaborators | XP 550; ach **Solomon's Wisdom** (fair) | Execute / redeem / exile per case |
| SQ_39_01 | Children of the Invasion | 39 | Orphan group | Evacuate 23 children | XP 600; next-gen protected | Linear |
| SQ_40_01 | Lily's Promise | 40 | **Requires SQ_08_02 promise** | Deliver Marcus Webb's final words to Lily (now 14) | XP 400; closure; ach **Promise Kept** | Linear (emotional set-piece) |
| SQ_41_01 | City Siege | 41 | Resistance Command | Assault occupied city | XP 700; liberated territory | Frontal / Infiltration / Siege |
| SQ_41_02 | The Hive Mind | 41 | **Requires SQ_18_01 Seven alive** | Seven connects Jake to Hive | XP 500; Overlord's plans intel | Accept psychic damage risk / decline |
| SQ_42_01 | Lab 48 Returns | 42 | Strategic target | Return to former lab (now Hive processor) | XP 600; Subject 0 complete data; emotional closure | Linear |
| SQ_43_01 | Wandering Merchant - Final | 43 | **Requires earlier merchant trades** | Promises called in | XP 500; weapon vs Conductor; mystery resolved | Variable per past trades |
| SQ_44_01 | Maria Santos Returns | 44 | **Requires SQ_01_02 Maria spared/freed** | Joint op vs Hive command post | XP 500; +50 Maria's cell fighters; reunion | Linear |
| SQ_45_01 | The Conductor's Shadow | 45 | Intelligence | Multi-lead investigation | XP 700; Conductor location; ach **Hunter** | Linear |
| SQ_46_01 | The Children's War | 46 | **Requires SQ_06_02** | Maya (now 16) requests youth fighters | XP 500; conditional youth ally squad | Let them fight / Protect them |
| SQ_47_01 | Last Confessions | 47 | All companions present | Sequential companion vignettes | XP 400; relationships maxed; ach **True Bonds** | Per-companion branches |
| SQ_48_01 | ARIA's Choice | 48 | **Requires SQ_05_01 ARIA alive** | ARIA proposes merge with Earth networks | XP 600; variable global AI support | Merge (sacrifice) / Keep safe (limited support) |
| SQ_49_01 | Jacob's Destiny | 49 | **Requires SQ_29_01 Jacob alive** | Sacrifice path to Conductor | XP 500; path-to-Conductor variant | Accept sacrifice / Find another way |
| SQ_50_01 | Humanity's Dawn | 50 | Post-final battle | Set policies (Salvari / Hybrids / Tech / Reconstruction) | XP 1000; ending variant chosen; ach **Architect of Peace** | 4 policy axes |

> Plus 14 more Act-4 minor quests (rebel cell hand-offs, captured collaborator interrogations, supply convoys, demolition charges, etc.) — total = 30.

---

## 3. Quest Chain Narratives

> Multi-step side stories that span 2–4 acts. Each chain's middle nodes are optional but missing any leaf forfeits the Act-4 payoff.

### 3.1 Maria Santos / Subject 6-Alpha
- **Floor 1:** `SQ_01_02` Find/spare/betray.
- **Act 4:** `SQ_44_01` Returns as resistance cell leader (only if not betrayed; spared OR convinced to join OR left hidden all qualify).
- **Payoff:** +50 Maria's cell fighters in final liberation; emotional reunion cinematic.

### 3.2 The Wandering Merchant
- **Level 9:** `SQ_09_01` First encounter — up to 4 trade types (memory, artifact, promise, tissue).
- **Level 14:** Second encounter — second trade window.
- **Level 24:** `SQ_24_01` Station Omega — exchange darkest secret.
- **Level 43:** `SQ_43_01` Final — promises called in; merchant's full lore revealed; receive weapon effective vs the Conductor.
- **Payoff:** Ancient artifact weapon (key vs Conductor in final battle).

### 3.3 ARIA AI
- **Floor 5:** `SQ_05_01` Awakening — choose directive (Protect / Tactical / Freedom).
- **Act 2:** ARIA persists in Sarah's equipment.
- **Act 3:** ARIA interfaces with alien tech (auto-progress).
- **Level 48:** `SQ_48_01` Choice — merge with Earth networks or stay safe.
- **Payoff:** Global AI support during final battle (if merge); ARIA survives as ally (if kept).

### 3.4 Jake's Clone / "Jacob"
- **Floor 7:** `SQ_07_01` Kill or spare.
- **Act 2:** Anonymous intel hand-offs (if spared).
- **Act 3:** `SQ_29_01` The Clone Returns with Hive vulnerability intel.
- **Level 49:** `SQ_49_01` Jacob's Destiny — sacrifice for path to Conductor.
- **Payoff:** Direct route to final boss OR alternate access path.

### 3.5 The Children's Ward → The Children's War
- **Floor 6:** `SQ_06_02` Rescue Maya/Tommy/Lily/Dr. Tanaka.
- **Act 4:** `SQ_46_01` Maya (now 16) leads youth resistance.
- **Payoff:** Youth fighter squad (conditional); emotional full-circle moment.

### 3.6 Lily's Promise
- **Level 8:** `SQ_08_02` Promise dying scavenger Marcus Webb.
- **Level 40:** `SQ_40_01` Deliver to 14-yr-old Lily at Refugee Camp Delta.
- **Payoff:** Most-cited emotional set-piece in the game (per TASK_10).

### 3.7 Architect's Heir
- **Level 12:** `SQ_12_01` Mendari archive (gate).
- **Level 19:** `SQ_19_01` Architect's Tomb — Conductor intel; Chaos Seed weapon.
- **Payoff:** Conductor weakness exploit; Architect's Mantle armor.

### 3.8 Seven / Hive Defector
- **Level 18:** `SQ_18_01` First encounter — accept / probation / exile / eliminate.
- **Level 41:** `SQ_41_02` The Hive Mind — connect Jake to Hive.
- **Payoff:** Overlord's plans intel; psychological cost.

### 3.9 Martinez Mercy Track
- **Floor 1:** `SQ_01_01` Wounded guard outcome.
- **Floor 2:** `SQ_02_01` Martinez confrontation.
- **Act 4:** Both choices echo in Jake's haunted dialogue (passive).

### 3.10 Cure Research (composite)
- Contributors: `SQ_02_02` (+15%), `SQ_03_02` (+25%), `SQ_10_01` (+20%), `SQ_13_01` (+15–30%).
- Threshold: ≥75% triggers `ach_story_031` "The Cure" in Act 3.
- Payoff: best ending (`ach_story_040` "True Ending") gates on cure completion.

---

## 4. Achievement Catalog

> 200 entries grouped by section. Source: TASK_13 §1–§7.
> **Tier** map per source: Bronze 5–15 GS, Silver 20–35 GS, Gold 40–75 GS, Platinum 75–100 GS.
> **Hidden** = locked-title in UI until unlocked.

### 4.1 Story — Act 1 (12)

| ID | Name | Condition | Tier | Hidden | Missable |
|----|------|-----------|------|--------|----------|
| ach_story_001 | Awakening | Exit medical pod | Bronze | No | No |
| ach_story_002 | First Blood | Kill first enemy | Bronze | No | No |
| ach_story_003 | Trust Issues | Make Dr. Chen first choice | Bronze | No | No |
| ach_story_004 | Chain of Command Broken | Defeat Chief Martinez | Bronze | No | No |
| ach_story_005 | Infected | Become infected on F2 | Bronze | No | No |
| ach_story_006 | Genetic Nightmare | Complete F3 | Bronze | No | No |
| ach_story_007 | Failed Experiment | Defeat Experiment #7 | Silver | No | No |
| ach_story_008 | Silencing the Chorus | Defeat The Collective | Silver | No | No |
| ach_story_009 | Not Alone | Rescue Sarah on F5 (Omega timeline, within timer) | Silver | No | Yes |
| ach_story_010 | First Contact | Discover Salvari tech (F6) | Silver | No | No |
| ach_story_011 | Mirror Match | Confront clone | Silver | No | No |
| ach_story_012 | Escaped | Complete Act 1 | Gold | No | No |

### 4.2 Story — Act 2 (10)

| ID | Name | Condition | Tier | Hidden | Missable |
|----|------|-----------|------|--------|----------|
| ach_story_013 | Sunlight | Reach the surface | Bronze | No | No |
| ach_story_014 | Crystal Gardens | Complete L9 | Bronze | No | No |
| ach_story_015 | New Friends | First Salvari contact | Silver | No | No |
| ach_story_016 | Heart of the Planet | Reach Crystal Heart (L12) | Silver | No | No |
| ach_story_017 | Storm Runner | Recruit K'thara | Silver | No | Yes |
| ach_story_018 | Toxic Beauty | Survive swamps (L13–14) | Bronze | No | No |
| ach_story_019 | Node Destroyed | Destroy regional control node | Silver | No | No |
| ach_story_020 | Alliance Forged | Form Human-Salvari alliance | Gold | No | Yes (Golden Path req) |
| ach_story_021 | Fleet Assembled | Complete fleet mission | Silver | No | No |
| ach_story_022 | Off-World | Complete Act 2 | Gold | No | No |

### 4.3 Story — Act 3 (10)

| ID | Name | Condition | Tier | Hidden | Missable |
|----|------|-----------|------|--------|----------|
| ach_story_023 | To the Stars | Launch Storm Runner | Bronze | No | No |
| ach_story_024 | Void Walker | First spacewalk | Bronze | No | No |
| ach_story_025 | Homecoming | Visit Salvari homeworld | Silver | No | No |
| ach_story_026 | The True Enemy | Discover Overlord nature | Silver | No | No |
| ach_story_027 | Witness to the Mendari | Mendari archive (L25 alt) | Silver | No | Yes |
| ach_story_028 | Station 7 | Infiltrate Overlord station | Silver | No | No |
| ach_story_029 | Storm Runners | Multi-species alliance | Gold | No | Yes |
| ach_story_030 | Til Death | Marry Sarah (Omega only) | Gold | No | Yes |
| ach_story_031 | The Cure | Complete universal cure | Silver | No | Yes |
| ach_story_032 | Home | Return to Earth | Gold | No | No |

### 4.4 Story — Act 4 (8)

| ID | Name | Condition | Tier | Hidden | Missable |
|----|------|-----------|------|--------|----------|
| ach_story_033 | The Invasion Begins | Win orbital battle | Silver | No | No |
| ach_story_034 | Feet on the Ground | Land on Earth | Bronze | No | No |
| ach_story_035 | Resistance | Unite with Earth resistance | Silver | No | No |
| ach_story_036 | City by City | Liberate first major city | Silver | No | No |
| ach_story_037 | Full Circle | Return to Lab 48 | Silver | No | No |
| ach_story_038 | Final Push | Reach Overlord stronghold | Silver | No | No |
| ach_story_039 | Earth's Champion | Defeat Overlord | Gold | No | No |
| ach_story_040 | True Ending | Best ending — all companions alive, max alliance, cure, Omega | Platinum | **Yes** | Yes |

### 4.5 Combat (50)

#### Kill counts (10)

| ID | Name | Threshold | Tier | Hidden |
|----|------|-----------|------|--------|
| ach_combat_001 | Blooded | 1 kill | Bronze | No |
| ach_combat_002 | Soldier | 100 | Bronze | No |
| ach_combat_003 | Veteran | 500 | Silver | No |
| ach_combat_004 | One-Man Army | 1,000 | Gold | No |
| ach_combat_005 | Extinction Event | 2,500 | Gold | No |
| ach_combat_006 | Apocalypse | 5,000 | Platinum | **Yes** |
| ach_combat_007 | Infected Slayer | 200 infected | Silver | No |
| ach_combat_008 | Xenocide | 200 drones | Silver | No |
| ach_combat_009 | Human Hunter | 100 hostile humans | Silver | No |
| ach_combat_010 | Alpha Predator | 50 mini-bosses | Gold | No |

#### Combat style (15)

| ID | Name | Condition | Tier |
|----|------|-----------|------|
| ach_combat_011 | Headhunter | 100 headshot kills | Silver |
| ach_combat_012 | Up Close and Personal | 100 melee kills | Silver |
| ach_combat_013 | Silent Death | 50 stealth kills | Silver |
| ach_combat_014 | Hulk Smash | 50 enhanced-strength kills | Silver |
| ach_combat_015 | Creative | 25 environmental kills | Silver |
| ach_combat_016 | Explosive Personality | 75 explosive kills | Silver |
| ach_combat_017 | Gravity Kills | 50 throw kills | Silver |
| ach_combat_018 | Burn Baby Burn | 50 fire kills | Silver |
| ach_combat_019 | Shocking | 50 electric kills | Silver |
| ach_combat_020 | Toxic Avenger | 50 poison/acid kills | Silver |
| ach_combat_021 | Ragdoll Physics | 100 charged-attack launches | Silver |
| ach_combat_022 | Disarmed | 100 weapons destroyed | Silver |
| ach_combat_023 | Counter-Strike | 50 perfect counters | Silver |
| ach_combat_024 | Executioner | 75 finishing moves | Silver |
| ach_combat_025 | Combo Master | 50-hit combo | Gold |

#### Weapon mastery (10)

| ID | Name | Condition | Tier |
|----|------|-----------|------|
| ach_combat_026 | Pistol Expert | 100 pistol kills | Bronze |
| ach_combat_027 | Rifle Expert | 100 rifle kills | Bronze |
| ach_combat_028 | Shotgun Expert | 100 shotgun kills | Bronze |
| ach_combat_029 | Sniper Expert | 50 sniper kills | Silver |
| ach_combat_030 | Heavy Weapons Expert | 50 heavy kills | Silver |
| ach_combat_031 | Energy Master | 50 Salvari energy kills | Silver |
| ach_combat_032 | Blade Dancer | 75 blade kills | Silver |
| ach_combat_033 | Blunt Force Trauma | 75 blunt kills | Silver |
| ach_combat_034 | Master of Arms | Kill with every weapon type | Gold |
| ach_combat_035 | Improvised Weaponry | 50 thrown-object kills | Silver |

#### Boss combat (10)

| ID | Name | Condition | Tier | Hidden |
|----|------|-----------|------|--------|
| ach_combat_036 | Boss Slayer | All story bosses | Gold | No |
| ach_combat_037 | Untouchable | Any boss, no damage | Gold | No |
| ach_combat_038 | Perfect Soldier | All bosses, no damage | Platinum | **Yes** |
| ach_combat_039 | Speed Kill | Any boss < 60 s | Gold | No |
| ach_combat_040 | Chorus Conductor | Collective via melee only | Gold | No |
| ach_combat_041 | Memory Erased | Memory Hunter w/o companion KO | Gold | No |
| ach_combat_042 | Clone Wars | Clone without abilities | Gold | No |
| ach_combat_043 | Overlord's Bane | Overlord first attempt | Platinum | **Yes** |
| ach_combat_044 | Redemption | Transformed-companion boss (Beta) | Silver | **Yes** |
| ach_combat_045 | The Siren's End | Transformed Aria (Beta) | Silver | **Yes** |

#### Combat performance (5)

| ID | Name | Condition | Tier |
|----|------|-----------|------|
| ach_combat_046 | Rampage | 10 kills / 30 s | Silver |
| ach_combat_047 | Unstoppable | 25 kills / 60 s | Gold |
| ach_combat_048 | Survivor | Recover from 1% HP | Silver |
| ach_combat_049 | Bullet Time | Dodge 100 attacks | Bronze |
| ach_combat_050 | Glass Cannon | Level with 10k dmg dealt + < 100 taken | Gold |

### 4.6 Exploration (35)

#### Locations (12)

| ID | Name | Condition | Tier |
|----|------|-----------|------|
| ach_explore_001 | Lab Tourist | Visit every Lab 48 room | Silver |
| ach_explore_002 | World Traveler | Complete all 50 levels | Gold |
| ach_explore_003 | Desert Wanderer | All Crystal Desert areas | Silver |
| ach_explore_004 | Spelunker | All cave systems | Silver |
| ach_explore_005 | Swamp Thing | All swamp regions | Silver |
| ach_explore_006 | Space Tourist | All space locations | Silver |
| ach_explore_007 | City Explorer | All Earth cities | Silver |
| ach_explore_008 | Deep Diver | All underwater sections | Silver |
| ach_explore_009 | High Ground | All elevated vantage points | Bronze |
| ach_explore_010 | Station Crawler | Every Station 7 section | Silver |
| ach_explore_011 | Salvari Welcomed | All Salvari settlements | Silver |
| ach_explore_012 | Cartographer | 100% map reveal | Gold |

#### Collectibles (12)

| ID | Name | Threshold | Tier | Hidden |
|----|------|-----------|------|--------|
| ach_explore_013 | Collector | All collectibles | Gold | No |
| ach_explore_014 | Listener | 75 audio logs | Silver | No |
| ach_explore_015 | Investigator | 50 evidence docs | Silver | No |
| ach_explore_016 | Explorer | All secret areas | Gold | No |
| ach_explore_017 | Lab 48 Historian | 30 backstory items | Silver | No |
| ach_explore_018 | Alien Anthropologist | 25 Salvari artifacts | Silver | No |
| ach_explore_019 | Overlord Archives | Decrypt 20 data cores | Gold | No |
| ach_explore_020 | Photo Album | 20 pre-outbreak photos | Bronze | No |
| ach_explore_021 | Weapon Cache | 30 hidden caches | Silver | No |
| ach_explore_022 | Medical Researcher | 15 medical research data | Silver | No |
| ach_explore_023 | Creature Cataloger | Scan 75 creature types | Gold | No |
| ach_explore_024 | Easter Egg Hunter | 10 dev easter eggs | Silver | **Yes** |

#### Environmental (11)

| ID | Name | Condition | Tier |
|----|------|-----------|------|
| ach_explore_025 | First Steps | Walk 10 km | Bronze |
| ach_explore_026 | Marathon Runner | 50 km | Silver |
| ach_explore_027 | World Sprinter | 100 km | Gold |
| ach_explore_028 | Acrobat | Climb 1,000 m | Silver |
| ach_explore_029 | Free Fall | Fall 500 m total without dying | Bronze |
| ach_explore_030 | Pilot | Vehicles 50 km | Silver |
| ach_explore_031 | Storm Pilot | All Storm Runner piloting | Silver |
| ach_explore_032 | Zero-G Master | All zero-G sequences | Silver |
| ach_explore_033 | Peaceful Resolution | Level w/ no non-hostile kills | Silver |
| ach_explore_034 | Night Vision | 30 min in darkness | Bronze |
| ach_explore_035 | Scenic Route | All vista points | Bronze |

### 4.7 Side Content (40)

#### Companions (15)

| ID | Name | Condition | Tier | Hidden | Timeline |
|----|------|-----------|------|--------|----------|
| ach_side_001 | Band of Brothers | Recruit all companions | Gold | No | — |
| ach_side_002 | Best Friends | Max any companion relationship | Silver | No | — |
| ach_side_003 | Beloved | Max all relationships | Gold | No | — |
| ach_side_004 | Sarah's Heart | Full Sarah romance | Gold | No | Omega |
| ach_side_005 | Aria's Trust | Full Aria arc | Gold | No | Alpha |
| ach_side_006 | Keisha's Strength | Full Keisha arc | Gold | No | Alpha |
| ach_side_007 | Emily's Hope | Full Emily arc | Gold | No | Alpha |
| ach_side_008 | K'thara's Bond | Full K'thara arc | Gold | No | — |
| ach_side_009 | Wingman | 25 companion missions | Silver | No | — |
| ach_side_010 | Team Player | 50 combo attacks | Silver | No | — |
| ach_side_011 | Protector | 25 companion revives | Bronze | No | — |
| ach_side_012 | Heart to Heart | All companion convo events | Silver | No | — |
| ach_side_013 | Family First | Keep all children alive | Gold | **Yes** | Alpha/Omega |
| ach_side_014 | Polyamory | All three Alpha partners simultaneously | Silver | **Yes** | Alpha |
| ach_side_015 | Alien Romance | K'thara romance | Silver | **Yes** | Beta |

#### Quests (12)

| ID | Name | Condition | Tier |
|----|------|-----------|------|
| ach_side_016 | Completionist | All side quests | Gold |
| ach_side_017 | Helper | 10 side quests | Bronze |
| ach_side_018 | Good Samaritan | 25 side quests | Silver |
| ach_side_019 | Hero of the People | 50 side quests | Gold |
| ach_side_020 | Rescuer | All rescue missions | Gold |
| ach_side_021 | Bounty Hunter | All bounty missions | Gold |
| ach_side_022 | Diplomat | All diplomatic missions | Gold |
| ach_side_023 | Supply Runner | 20 supply missions | Silver |
| ach_side_024 | Intelligence Operative | All intel-gathering | Silver |
| ach_side_025 | Liberation Hero | All optional Earth territories | Gold |
| ach_side_026 | Saboteur | All sabotage missions | Silver |
| ach_side_027 | Mystery Solver | All environmental mysteries | Silver |

#### Crafting & Progression (13)

| ID | Name | Condition | Tier |
|----|------|-----------|------|
| ach_side_028 | Master Craftsman | All crafting recipes | Gold |
| ach_side_029 | Fully Evolved | All Jake skills | Gold |
| ach_side_030 | Tech Expert | All Sarah skills | Gold |
| ach_side_031 | Weapon Upgrader | Fully upgrade 10 weapons | Silver |
| ach_side_032 | Arsenal | Own every weapon | Gold |
| ach_side_033 | Fashion Forward | All armor sets | Gold |
| ach_side_034 | Wealthy | 100,000 credits | Silver |
| ach_side_035 | Max Level | Reach max level | Gold |
| ach_side_036 | Resource Manager | Craft 200 items | Silver |
| ach_side_037 | Salvage Expert | Salvage 500 items | Silver |
| ach_side_038 | Hacker Supreme | Hack 100 terminals | Silver |
| ach_side_039 | Lock Picker | 75 locks | Silver |
| ach_side_040 | The Cure | Complete cure research | Gold |

### 4.8 Challenge (25)

| ID | Name | Condition | Tier | Hidden |
|----|------|-----------|------|--------|
| ach_challenge_001 | Speed Demon | Act 1 < 2 h | Gold | No |
| ach_challenge_002 | Lightning Strike | Act 2 < 4 h | Gold | No |
| ach_challenge_003 | Warp Speed | Act 3 < 5 h | Gold | No |
| ach_challenge_004 | Blitzkrieg | Act 4 < 4 h | Gold | No |
| ach_challenge_005 | World Record | Full game < 12 h | Platinum | **Yes** |
| ach_challenge_006 | Survivalist | Complete on Hard | Gold | No |
| ach_challenge_007 | Nightmare | Complete on Nightmare | Platinum | **Yes** |
| ach_challenge_008 | Lab 48 Veteran | Act 1 Hard, no down-difficulty | Silver | No |
| ach_challenge_009 | Alien Survivor | Act 2 Hard, no down-difficulty | Silver | No |
| ach_challenge_010 | Permadeath Champion | Complete with Permadeath | Platinum | **Yes** |
| ach_challenge_011 | Immortal | Complete game w/o dying | Platinum | **Yes** |
| ach_challenge_012 | Flawless Escape | Act 1 w/o dying | Gold | No |
| ach_challenge_013 | Pacifist | Act 1 killing only bosses | Gold | No |
| ach_challenge_014 | Iron Man | Level w/o healing | Silver | No |
| ach_challenge_015 | Brawler | Act 1 melee only | Gold | No |
| ach_challenge_016 | Sharpshooter | Act 1 ranged only | Gold | No |
| ach_challenge_017 | Minimalist | Complete game no upgrades | Platinum | **Yes** |
| ach_challenge_018 | Solo Operator | Act 2 w/o companion abilities | Gold | No |
| ach_challenge_019 | Untouched | Level w/o damage | Gold | No |
| ach_challenge_020 | No Stealth | F3 triggering every alarm | Silver | No |
| ach_challenge_021 | New Game Plus | Complete NG+ run | Silver | No |
| ach_challenge_022 | All Timelines | Complete all 3 timelines | Gold | No |
| ach_challenge_023 | 100% Run | 100% completion any playthrough | Platinum | No |
| ach_challenge_024 | No Save Challenge | Act 1 one sitting, no saves | Gold | **Yes** |
| ach_challenge_025 | True Survivor | Unlock all other achievements | Platinum (trophy) | No |

### 4.9 Secret (10) — all hidden

| ID | Name | Condition | Tier |
|----|------|-----------|------|
| ach_secret_001 | Subject 6 | Find and recruit Maria | Silver |
| ach_secret_002 | Two of Me | Spare clone | Silver |
| ach_secret_003 | Temporal Master | All 3 timelines complete | Gold |
| ach_secret_004 | Newlyweds at War | Married + defeat final boss | Gold |
| ach_secret_005 | Extinction | Worst ending (all dead, Earth destroyed) | Silver |
| ach_secret_006 | ??? | Dev easter egg via F4 maintenance terminal code 4815162342 | Silver |
| ach_secret_007 | Genocide Route | Kill every living thing in Act 1 | Silver |
| ach_secret_008 | The Prophecy | Activate all 5 prophecy stones + True Ending | Gold |
| ach_secret_009 | Dr. Chen's Secret | Find all Chen private logs + confront | Silver |
| ach_secret_010 | Breaking the Fourth Wall | Press ~ 100 times across playthroughs | Silver |

### 4.10 Endings (13)

| ID | Name | Condition | Tier | Hidden |
|----|------|-----------|------|--------|
| ach_ending_001 | Liberation | Ending 1: Total Victory | Gold | No |
| ach_ending_002 | Pyrrhic Victory | Ending 2 | Silver | No |
| ach_ending_003 | Sacrifice | Ending 3: Ultimate Sacrifice | Gold | No |
| ach_ending_004 | Together Forever | Ending 4: Dual Sacrifice | Gold | No |
| ach_ending_005 | New Beginning | Ending 5: Salvari Integration | Silver | No |
| ach_ending_006 | Exodus | Ending 6: Abandon Earth | Silver | No |
| ach_ending_007 | Lone Survivor | Ending 7 | Silver | No |
| ach_ending_008 | Redemption | Ending 8: Transformed Saved | Gold | **Yes** |
| ach_ending_009 | Betrayal | Ending 9: Join Overlord | Silver | **Yes** |
| ach_ending_010 | Stalemate | Ending 10: Mutual Destruction | Silver | No |
| ach_ending_011 | Cliffhanger | Ending 11: Fight Continues | Silver | No |
| ach_ending_012 | Darkness Falls | Ending 12: Total Defeat | Silver | No |
| ach_ending_all | Every Story | Experience all 12 endings | Platinum | **Yes** |

### Aggregate stats (TASK_13 §"Statistics Summary")

| Category    | Count | Gamerscore |
|-------------|------:|-----------:|
| Story       | 40    | 620        |
| Combat      | 50    | 600        |
| Exploration | 35    | 360        |
| Side Content| 40    | 540        |
| Challenge   | 25    | 630        |
| Secret      | 10    | 370        |
| Endings     | 13    | (subset of above) |
| **Total**   | **200** | **3,120**  |

Tier distribution: 48 Bronze / 89 Silver / 50 Gold / 13 Platinum-equivalent. Hidden = 35; missable = 25 (story-gated); timeline-specific = 24 (Alpha 8, Beta 6, Omega 10).

---

## 5. Quest / Achievement Runtime (Data Format + Evaluation, Save Integration)

> Engine notes: X3Native already has `objective.{h,cpp}` (basic objective text), `trigger.{h,cpp}` (volume + interaction triggers), `save.{h,cpp}` (savegame). There is **no dedicated quest engine** and **no achievement system** yet. The audio side has `cues.{h,cpp}` with some procedural cues but no music-track scheduler.

### 5.1 Data file layout (proposed; on-disk)

```
G:\X3Native\data\quests\
  manifest.json                # version, count, hash
  act1\
    sq_01_01.json
    sq_01_02.json
    ...
  act2\ act3\ act4\
G:\X3Native\data\achievements\
  achievements.json            # single file, 200 entries
G:\X3Native\data\audio_cues\
  music_cues.json              # 65 tracks + adaptive layers
  ambient_cues.json            # 55 loops
  sfx_table.json               # quest-tied SFX (subset of 525)
```

Format: plain JSON, mirroring the source documents' JSON blocks one-for-one so no transcription is required on import.

### 5.2 Quest record (JSON schema, derived from TASK_10 lines 42–203)

```
QuestRecord {
  string  quest_id;              // "SQ_01_01"
  string  name;
  int     floor_or_level;
  string  giver;                 // "Environmental Discovery" | NPC | terminal | audio
  string  trigger;               // human-readable
  string  availability;          // "All Timelines" | "Alpha" | "Beta" | "Omega"
  string  description;
  Objective[] objectives;
  Outcome[]   outcomes;          // branches with karma + later impact
  Rewards     rewards;           // xp, items, lore, achievement_id
  Prereq[]    prerequisites;     // quest_id or save_flag
  string[]    incompatible_with; // mutually exclusive quests
  Unity       unity_block;       // ignored on import (legacy)
  string[]    save_flags;        // ["sq_01_01_started", "sq_01_01_complete", "maria_fate"]
  AudioRefs   audio;             // music_cue_ids, sfx_ids, stinger_ids
}
```

### 5.3 Quest state machine

```
        +---------+
        | LOCKED  |  (prerequisites not met)
        +----+----+
             | trigger fires + prereqs met
             v
        +---------+
        |AVAILABLE|  (quest-giver glyph showing in UI)
        +----+----+
             | player accepts / discovers
             v
        +---------+    objective complete   +---------+
        | ACTIVE  +------------------------>|RESOLVING|
        +----+----+                         +----+----+
             |                                    |
        time-limited expire                  outcome chosen
             v                                    v
        +---------+                         +---------+
        | FAILED  |                         |COMPLETE |
        +---------+                         +----+----+
                                                 |
                                                 v
                                          karma applied,
                                          rewards granted,
                                          flags written,
                                          achievement evaluated,
                                          audio cue fired
```

### 5.4 Achievement record (JSON schema, from TASK_13 lines 55–275)

```
AchievementRecord {
  string  id;                    // "ach_story_001"
  string  name;
  string  description;
  string  icon;                  // "icon_pod_broken"
  string  type;                  // Story | Combat | Exploration | SideContent | Challenge | Secret | Ending
  string  tier;                  // Bronze | Silver | Gold | Platinum
  int     gamerscore;
  bool    hidden;
  bool    missable;
  int     floor_or_level;
  string  timeline;              // optional: Alpha | Beta | Omega
  Tracking tracking;             // see below
  string  notes;                 // designer note
}

Tracking {
  string type;                   // "event" | "stat" | "composite" | "timed_stat" | "timed_completion"
  string event_id;               // for event-type: "EVT_POD_ESCAPE"
  string stat_id;                // for stat-type: "total_kills", "headshot_kills"
  float  threshold;              // for stat-type
  float  time_window;            // for timed-stat ("Rampage": 30 s)
  string scope;                  // optional: "level" | "act" | "global"
  string[] conditions;           // for composite: list of sub-IDs all required
}
```

### 5.5 Evaluation model

Three-source eval: **events**, **stats**, **composites**.

1. **Event-driven** (e.g. `EVT_POD_ESCAPE`, `EVT_BOSS_MARTINEZ_DEFEATED`): publisher emits an event ID; achievement engine looks up all achievements bound to that ID and unlocks any whose extra conditions hold.
2. **Stat-driven** (e.g. `total_kills >= 100`): a stats subsystem increments named counters; achievements with `tracking.type=="stat"` are evaluated on every increment of their watched stat.
3. **Composite** (e.g. **Master of Arms** "kill with every weapon type"): list of child IDs; composite is unlocked when every child stat has crossed its threshold.
4. **Timed** (e.g. **Rampage** 10 kills / 30 s): ring-buffer of timestamped kills; on each kill, prune entries older than the time window, then count.

### 5.6 Save-game integration

Extend the existing `save.{h,cpp}` payload with three blocks:

```
SaveGame {
  ... existing fields ...

  // NEW
  QuestSave        quests;          // see below
  AchievementSave  achievements;
  KarmaSave        karma;
}

QuestSave {
  map<string,string> state;         // quest_id -> "LOCKED"|"AVAILABLE"|"ACTIVE"|"RESOLVING"|"COMPLETE"|"FAILED"
  map<string,string> outcomes;      // quest_id -> chosen outcome key
  map<string,bool>   flags;         // arbitrary save flags ("maria_fate", "sq_01_01_complete")
  map<string,int>    progress;      // quest_id -> count for collect/kill objectives
}

AchievementSave {
  map<string,bool>   unlocked;      // achievement_id -> unlocked
  map<string,float>  stats;         // stat_id -> running counter (long doubles for huge counts)
  uint64_t           unlock_bitfield_hash;  // for tamper detection
}

KarmaSave {
  map<string,int>    axes;          // "Mercy" -> 10, "Ruthless" -> 5, etc.
  string             dominant;      // cached top axis
  string             title;         // derived title
}
```

Versioning: bump save format minor on add; old saves migrate by defaulting all maps to empty (no quests unlocked, all stats zero — equivalent to starting fresh on the side-content layer). Critical: never zero out main-quest progress on migration.

### 5.7 UI/UX requirements (TASK_10 + TASK_13 §UI mentions)

- **Quest log**: list, filtered by Act / status / karma / timeline; each entry shows description, objectives with checkmarks, current step, reward preview.
- **Objective HUD**: top-right pinned objective; uses existing `objective.{h,cpp}` text renderer.
- **Quest-marker glyphs**: 3D world markers for AVAILABLE quests (with proximity fade-in). New world-marker primitive needed.
- **Branch confirmation**: when a Choice-type objective resolves (e.g. SQ_01_01 kill/help/take), a non-skippable modal confirms ("This choice will affect later events" — single-line warning, no spoiler).
- **Achievement toast**: bottom-right 5-second slide-in with icon + name + GS; queue with cooldown so multiple achievements unlocking on same frame trickle.
- **Achievement screen**: tabbed (Story / Combat / Exploration / Side / Challenge / Secret / Endings); hidden entries show "???" until unlocked.
- **Karma readout**: in pause menu, 10 small bars with current dominant axis highlighted; debug overlay version for dev.

---

## 6. Audio Cue Catalog (Quest + Achievement Tie-ins)

> Source: TASK_14 §II–V. Shipped status reflects current `cues.{h,cpp}` (some procedural cues, no music streamer).

### 6.1 Music cues bound to quest milestones — Act 1

| Cue ID | Track Name | Linked Quest / Beat | Shipped? |
|--------|------------|---------------------|----------|
| MUS_F1_01_AWAKENING | "Awakening" | Opening cinematic; precedes any quest | **Missing** (placeholder stinger only) |
| MUS_F1_02_EXPLORATION | "Medical Wing Exploration" | F1 ambient — Safe/Alert/Combat/Critical adaptive | **Missing** |
| MUS_F1_03_DISCOVERY | "The Files" | `SQ_01_02` Subject 6 clue discovery | **Missing** |
| MUS_F1_04_DRCHEN_INTRO | "Trust" | Dr. Chen first conversation (story); `SQ_01_02` setup | **Missing** |
| MUS_F1_05_MARTINEZ_BOSS | "Chain of Command" | Martinez fight; 3-phase (110/120/130 BPM); `SQ_02_01` modifies pacing | **Missing** |
| MUS_F2_01_SECURITY | "Lockdown" | F2 ambient | **Missing** |
| MUS_F2_02_RESCUE_TIMER | "Every Second Counts" | Aria/Keisha/Emily rescue timer (5min→30s tempo escalation) | **Missing** |
| MUS_F2_03–05_RESCUE_*  | Character rescue themes | `ach_story_009` "Not Alone" lineup | **Missing** |
| MUS_F2_06_TRANSFORMATION | "Becoming" | Rescue failure stinger | **Missing** |
| MUS_F3_03_EXPERIMENT7_BOSS | "Remember My Name" | F3 boss; 3-phase | **Missing** |
| MUS_F4_04_COLLECTIVE_BOSS | "The Chorus" | `SQ_04_01`; 4-phase with 5 personality themes | **Missing** |
| MUS_F5_03_SARAH_RESCUE | "Not Alone" | First major key in score (D major) | **Missing** |
| MUS_F6_03_CRYSTAL_CHAMBER | "Living Memory" | `SQ_06_01` artifact discovery | **Missing** |
| MUS_F7_02_CLONE_ENCOUNTER | "Mirror" | `SQ_07_01`; 4-phase D minor vs D major | **Missing** |
| MUS_F7_04_ESCAPE | "Freedom" | Act 1 closeout; `ach_story_012` | **Missing** |

### 6.2 Music cues bound to quest milestones — Act 2

| Cue ID | Track | Linked Quest / Beat |
|--------|-------|---------------------|
| MUS_L08_01_SURFACE | "New Dawn" | `ach_story_013` Sunlight |
| MUS_L09_01_CRYSTAL_SANDS | "Crystal Sands" | L9–10 biome; covers SQ_09_01, SQ_09_02 |
| MUS_L11_03_KTHARA_THEME | "Storm Runner" | `ach_story_017` K'thara joins |
| MUS_L11_04_ALLIANCE | "Two Peoples" | Beats SQ_10_02, SQ_15_01 (Hybrid Colony) |
| MUS_L12_01_CAVES | "Crystal Heart" | `SQ_12_01` Mendari archive (gates SQ_19_01) |
| MUS_L13_01_SWAMPLANDS | "Toxic Beauty" | `SQ_13_01` Swamp Doctor |
| MUS_L16_01_RUINS | "What Was" | `SQ_16_01`, `SQ_19_01` Architect's Tomb |
| MUS_L20_02_LAUNCH | "Into the Stars" | `SQ_20_01` Alliance Summit success → `ach_story_022` |

All Act-2 music: **Missing** in `cues.cpp`.

### 6.3 Music cues bound to quest milestones — Act 3

| Cue ID | Track | Linked Quest |
|--------|-------|--------------|
| MUS_L21_02_SHIP_INTERIOR | "Storm Runner" | `SQ_21_01`, `SQ_21_02` |
| MUS_L25_01_SALVARI_HOMEWORLD | "Thirty Billion Ghosts" | `ach_story_025` Homecoming |
| MUS_L29_01_ROMANCE_SARAH / L29_02_ROMANCE_KTHARA | Romance | `SQ_30_01` Love Across Stars |
| MUS_L30_01_WEDDING | "Vows" | `ach_story_030` Til Death (Omega) |
| MUS_L31_01_PROTO_OVERLORD | "Lesser God" | Boss; ties to ending eligibility |
| MUS_L34_01_EARTH_APPROACH | "Blue Marble" | `ach_story_032` Home |

All Act-3 music: **Missing**.

### 6.4 Music cues bound to quest milestones — Act 4

| Cue ID | Track | Linked Quest / Beat |
|--------|-------|---------------------|
| MUS_L37_01_RESISTANCE | "Still Standing" | `SQ_37_01` Underground Railroad |
| MUS_L38_01_RETURN_TO_LAB | "Full Circle" | `SQ_42_01` Lab 48 Returns |
| MUS_L40_01_URBAN_WARFARE | "Streets of Fire" | `SQ_41_01` City Siege |
| MUS_L48_01_PROTO_OVERLORDS | "Lesser Gods Fall" | Boss gauntlet; companion confessions backdrop (SQ_47_01) |
| MUS_L50_01_FINAL_BOSS | "End of Everything" | 12-minute dynamic; pinned to phase-based eval |
| MUS_L50_02_ENDINGS | 12 ending variants | One stinger per `ach_ending_001..012` |
| MUS_L50_03_CREDITS | "Journey's End" | Post-`ach_story_040` True Ending |

All Act-4 music: **Missing**.

### 6.5 Achievement-unlock SFX (UI tier)

TASK_14 §UI lists generic unlock cues. Per-tier proposal (new IDs, not in source):

| SFX ID | Use | Shipped? |
|--------|-----|----------|
| `UI_ACH_BRONZE` | Bronze unlock | **Missing** (use generic `UI_NOTIFY` placeholder) |
| `UI_ACH_SILVER` | Silver unlock | **Missing** |
| `UI_ACH_GOLD` | Gold unlock | **Missing** |
| `UI_ACH_PLATINUM` | Platinum unlock | **Missing** |
| `UI_OBJECTIVE_NEW` | Quest accepted | Shipped (matches existing `objective_new` in §UI list) |
| `UI_OBJECTIVE_COMPLETE` | Objective ticked | Shipped |
| `UI_KARMA_SHIFT` | Karma axis crosses threshold | **Missing** |

### 6.6 Ambient soundscapes (55 loops)

Tied 1-for-1 to acts; map below preserved verbatim from TASK_14 §V:

- Act 1: `AMB_FLOOR1_MEDICAL` … `AMB_FLOOR7_EXECUTIVE` (7 loops)
- Act 2: `AMB_SURFACE_DAY/NIGHT`, `AMB_CRYSTAL_DESERT`, `AMB_CRYSTAL_CAVES`, `AMB_SALVARI_CAMP`, `AMB_TOXIC_SWAMP`, `AMB_ALIEN_FOREST`, `AMB_ANCIENT_RUINS` (8 loops)
- Act 3: `AMB_DEEP_SPACE`, `AMB_STORM_RUNNER_BRIDGE/QUARTERS`, `AMB_SPACE_STATION`, `AMB_HYPERSPACE` (5 loops)
- Act 4: `AMB_RUINED_CITY`, `AMB_RESISTANCE_BASE`, `AMB_UNDERGROUND`, `AMB_BATTLEFIELD`, `AMB_OVERLORD_SHIP`, `AMB_VICTORY` (6 loops)

All ambient loops: **Missing**.

### 6.7 Adaptive music layering (4-state)

TASK_14 §I "adaptive_music_system": Safe / Alert / Combat / Boss. Each tied to:
- **Safe** ← no hostile within 30 m
- **Alert** ← hostile within 30 m, not in combat
- **Combat** ← combat state active
- **Boss** ← boss encounter; overrides everything
- **Critical** ← HP < 25% layered on top

Crossfade: 2 s. **Not shipped**; current `cues.cpp` plays one-shots.

---

## 7. Implementation Plan + Engine Files to Extend

### 7.1 Engine deltas — current vs needed

| Subsystem | Current file(s) | Status | Needed for this spec |
|-----------|-----------------|--------|----------------------|
| Objective text | `G:\X3Native\app\objective.{h,cpp}` | Has basic objective text rendering | Extend with multi-objective list, checkmark glyphs, branch-confirmation modal |
| Triggers | `G:\X3Native\app\trigger.{h,cpp}` | Has volume + interaction triggers | Add: terminal-hack, dialogue, audio-cue zone, random-encounter, quest-completion, boss-phase-HP triggers |
| Save | `G:\X3Native\app\save.{h,cpp}` | Has savegame core | Add `QuestSave`, `AchievementSave`, `KarmaSave` blocks; bump version; migration path |
| Audio cues | `G:\X3Native\app\cues.{h,cpp}` | Some procedural cues | Promote to full cue scheduler: music streamer, adaptive-layer mixer, timed-stinger queue |
| Quest engine | — | **Does not exist** | New: `quest.{h,cpp}` + `questdb.{h,cpp}` + `karma.{h,cpp}` |
| Achievement engine | — | **Does not exist** | New: `achievement.{h,cpp}` + `stats.{h,cpp}` |
| Quest UI | — | **Does not exist** | New: `quest_log.{h,cpp}`, `quest_marker.{h,cpp}`, `ach_toast.{h,cpp}` |

### 7.2 New files proposed (under `G:\X3Native\app\`)

```
quest.h / quest.cpp              # QuestRecord, state machine, evaluator
questdb.h / questdb.cpp          # JSON loader, lookup by ID, prereq graph
karma.h / karma.cpp              # 10-axis tracker, dominant calc, save block
achievement.h / achievement.cpp  # AchievementRecord, unlock pipeline
stats.h / stats.cpp              # named counters + ring buffers for timed
quest_log.h / quest_log.cpp      # UI panel
quest_marker.h / quest_marker.cpp # 3D world markers
ach_toast.h / ach_toast.cpp      # bottom-right slide-in
audio_sched.h / audio_sched.cpp  # music streamer + adaptive mixer
```

Headers should follow X3Native's existing C99/C++17 style as used in `objective.h`. JSON is parsed using whatever loader is already in-tree (or a single-header parser; do not add a large dep).

### 7.3 Extensions to existing files

`objective.{h,cpp}`:
- Add `Objective` struct with `{id, description, type (discover|collect|find|interact|choice|stealth|hack|puzzle|dialogue|combat), state}`.
- Replace single-line text with vector + checkmark glyphs.
- Wire to `quest.cpp`'s ACTIVE state.

`trigger.{h,cpp}`:
- Add trigger subtypes per TASK_10 §"Trigger Types": Proximity, TerminalHack, AudioCue, CombatPhase, Environmental, Dialogue, Random, QuestChain.
- Each subtype must publish an event ID to the achievement engine and to the quest engine.

`save.{h,cpp}`:
- Add the three new blocks (§5.6).
- Add migration shim for v0 saves (zero-initialise new blocks).
- Save-file tamper detection on `AchievementSave.unlock_bitfield_hash` (xor with a constant salt — purely cosmetic anti-tamper).

`cues.{h,cpp}`:
- Stay as the low-level procedural-cue API.
- New `audio_sched.{h,cpp}` sits above and consumes cue IDs from `quest.cpp` / `achievement.cpp`.

### 7.4 Phased build order (engineering, not gameplay)

**Phase E0 — Data import (no code reach)**
1. Author JSON files under `G:\X3Native\data\quests\` and `G:\X3Native\data\achievements\` mirroring TASK_10 / TASK_13 1:1. No engine wiring yet.

**Phase E1 — Save extension**
2. Add `QuestSave` / `AchievementSave` / `KarmaSave` blocks to `save.{h,cpp}` with empty maps.
3. Version-bump + migration tested vs an existing v0 save.

**Phase E2 — Stats + Achievements (event-only path first)**
4. New `stats.{h,cpp}`: named counters with persist-through-save hook.
5. New `achievement.{h,cpp}`: load JSON; subscribe to event publisher; unlock + toast.
6. Wire 20 trivial unmissable Act-1 achievements (`ach_story_001..006`, `ach_combat_001..002`, etc.) as proof-of-life.

**Phase E3 — Quest engine**
7. New `questdb.{h,cpp}` loads JSON; builds prereq DAG.
8. New `quest.{h,cpp}` state machine; integrates with existing `trigger.cpp` and extended `objective.cpp`.
9. New `karma.{h,cpp}` ten-axis tracker.
10. Wire Act-1 quests (14) end-to-end as the vertical slice.

**Phase E4 — UI**
11. `quest_log.{h,cpp}`, `quest_marker.{h,cpp}`, `ach_toast.{h,cpp}`.
12. Hook into `objective.cpp` for HUD pinning.

**Phase E5 — Audio scheduler**
13. `audio_sched.{h,cpp}` over `cues.{h,cpp}`. Music streamer (Vorbis loops/seamless), 4-state adaptive crossfade, timed-stinger queue.
14. Bind Act-1 music cues (15 from §6.1) and ambient loops (7).

**Phase E6 — Acts 2–4 content rollout**
15. Repeat E3/E5 wiring per Act.

### 7.5 Test plan (engineering smoke)

For each act:
- Spawn at level start; verify quest log shows expected AVAILABLE quests.
- Force-complete each objective via dev console; verify save round-trip.
- Force-trigger each achievement event; verify toast fires and `unlocked` map updates.
- Karma audit: every documented karma delta (e.g. SQ_01_01 "+10 Mercy") matches the corpus.

---

## 8. Build Order (Which Quests Gate Which Act)

> Engineering and gameplay are decoupled, but gameplay unlocks should not be released ahead of the engineering phase that supports them. This section is the **gameplay**-side build order.

### 8.1 Gating quests (cross-Act)

| Gate quest | Gates | Why |
|------------|-------|-----|
| `SQ_05_01` ARIA's Awakening | `SQ_48_01` ARIA's Choice | ARIA must exist as ally in Act 4. |
| `SQ_07_01` Spare clone | `SQ_29_01`, `SQ_49_01` | Jacob must be alive to return. |
| `SQ_08_02` Promise Marcus | `SQ_40_01` Lily's Promise | Promise must be made. |
| `SQ_12_01` Mendari archive | `SQ_19_01` Architect's Tomb | Coordinates dependency. |
| `SQ_18_01` Seven recruited | `SQ_41_02` The Hive Mind | Seven must be alive. |
| `SQ_06_02` Children's Ward | `SQ_46_01` The Children's War | Maya must survive. |
| `SQ_01_02` Maria spared/freed | `SQ_44_01` Maria Santos Returns | Maria must be alive. |
| `SQ_20_01` Alliance Summit strong unity | `ach_story_020` Alliance Forged → Golden Path | Best-ending eligibility. |
| Cure aggregate ≥ 75% (`SQ_02_02 + SQ_03_02 + SQ_10_01 + SQ_13_01`) | `ach_story_031` The Cure → True Ending | Composite gate. |
| `SQ_09_01` Wandering Merchant 1st trade | `SQ_43_01` Final merchant | Promises ledger. |

### 8.2 Recommended ship order (content milestones)

**M1 — Lab Escape vertical slice (Act 1, Floors 1–7)**
- All 14 Act-1 quests.
- 12 Act-1 story achievements + linked side achievements.
- Engineering: Phases E0–E5 complete.
- Audio: 18 Act-1 music tracks, 7 Act-1 ambient loops.
- Validates: quest engine, choice-branch flow, save persistence, achievement toast, karma readout.

**M2 — Open World (Act 2, Levels 8–20)**
- All 26 Act-2 quests.
- Faction system + Alliance Summit.
- 10 Act-2 story achievements; Side-content "Helper"/"Good Samaritan" thresholds.
- Audio: 16 Act-2 music tracks, 8 ambient loops.
- Validates: prereq DAG, faction reputation, time-limited quests (SQ_17_01).

**M3 — Space Journey (Act 3, Levels 21–35)**
- All 30 Act-3 quests including merchant chain mid-point and romance branches.
- 10 Act-3 story achievements + romance/companion achievements.
- Audio: 18 Act-3 music tracks, 5 ambient loops.
- Validates: timeline branching (Alpha/Beta/Omega), companion arcs, romance system.

**M4 — Earth Liberation (Act 4, Levels 36–50)**
- All 30 Act-4 quests; major payoffs from prior acts (Maria, Lily, Jacob, ARIA, Maya, Merchant).
- 8 Act-4 story achievements + all 13 ending achievements + 10 secret achievements + challenge tier wrap.
- Audio: 13 Act-4 music tracks, 6 ambient loops, 12 ending stingers.
- Validates: cross-act payoff chains, true-ending composite gate, all 12 endings reachable.

**M5 — Polish & secret/challenge pass**
- Remaining challenge achievements (speedrun, no-death, difficulty).
- 10 secret achievements (incl. fourth-wall + dev easter eggs).
- Audio QA: full 65-track + 55-loop sweep.
- `ach_challenge_025` True Survivor wraps up.

### 8.3 Quest count vs Act vs engineering phase

| Act | Quest count | Story ach | Audio tracks | Engineering phase |
|----:|-----------:|----------:|-------------:|:------------------|
| 1   | 14         | 12        | 18 mus + 7 amb | E0–E5            |
| 2   | 26         | 10        | 16 + 8       | content + new triggers (random encounter, time-limited) |
| 3   | 30         | 10        | 18 + 5       | content + timeline branching                |
| 4   | 30         | 8         | 13 + 6       | content + endings system + cross-act payoffs |
| **Total** | **100** | **40** | **65 + 26**  |                                |

---

## Appendix A — Source Citations

Every numbered claim in this document is anchored to one of:

- `TASK_10_ESCAPE_LAB_48_COMPLETE_SIDE_QUESTS.md` — quest IDs, names, objectives, outcomes, karma, achievement integration table (§"Achievement Integration"), trigger-type table, karma system.
- `TASK_13_EscapeLab48_Complete_Achievement_System.md` — all 200 achievement entries, tier definitions, gamerscore, hidden flags, statistics summary, Unity AchievementData scaffold (used here only as a JSON schema reference).
- `TASK_14_ESCAPE_LAB_48_COMPLETE_AUDIO_DESIGN.md` — music cue sheet, adaptive music states, timer-music synchronization, ambient soundscape list, UI SFX list.

Cross-references to `EFLZ_NARRATIVE.md`, `EFLZ_WORLD_STRUCTURE.md`, `EFLZ_MASTER_PLAN.md`, `EFLZ_BESTIARY.md` (all in `G:\X3Native\docs\design\`) are normative for floor layouts, character bios, and creature stats but were not modified by this spec.

## Appendix B — Open Questions / Decisions Deferred

1. **Romance accounting (Alpha timeline)**: `ach_side_014` Polyamory permits all three Aria/Keisha/Emily relationships simultaneously, but TASK_10 §"Quest Giver Profiles" doesn't define exclusivity rules. Deferred to companion-dev pass.
2. **Cure threshold value**: TASK_13 lists `ach_story_031` The Cure but TASK_10 contributors sum to 75–95% depending on choices; pick 75% as gate threshold (this doc), confirm with narrative pass.
3. **Karma title strings**: Source mentions "K'veth-ren", "Alliance Leader" but no full title table. Generate during M2 polish.
4. **Save tamper hash**: §5.6 uses a simple xor; if anti-cheat becomes a goal, replace with HMAC-SHA256. Out of scope for now.
5. **Stat persistence across NG+**: `ach_challenge_021` New Game Plus — must decide whether kill counts carry forward (default: yes; achievements stay unlocked, stats keep accumulating).

— End of document —
