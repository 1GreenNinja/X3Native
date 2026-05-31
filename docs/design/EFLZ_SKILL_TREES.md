# EFLZ — Skill Trees + Economy Spec
> Synthesized 2026-05-31 from TASK_11 PARTS 1-4 (~190 KB total).
> Authority: Tim's design corpus at G:\Unity_Projects\PARALLEL_TASKS_8_CHATS\.
> **No code changes; design doc only.**

---

## 1. Overview + Sources

### 1.1 Source documents

| File | Size | Coverage |
|------|------|----------|
| `TASK_11_SKILL_TREES_PART1_MAIN_COMPANIONS.md` | 70 KB | Jake (3 trees full), Sarah (full), Aria + Keisha overviews, Emily Tier-1 only |
| `TASK_11_SKILL_TREES_PART2_EMILY_KTHARA.md` | 43 KB | Emily Tier 2-4 + Analyst tree, K'thara both trees full |
| `TASK_11_SKILL_TREES_PART3_SUPPORT_COMPANIONS.md` | 44 KB | Dr. Chen, SEEKER, Foreman Grox, Maria Santos full |
| `TASK_11_SKILL_TREES_PART4_ECONOMY_IMPLEMENTATION.md` | 33 KB | Economy, costs, respec, archetypes, totals |

Total system: **200 skills / 17 skill trees / 17 ultimates / ~1000 skill levels** across 10 characters [PART4 §"Complete Skill Summary"].

### 1.2 Universal structure

Every standard tree has **4 tiers** and **12 skills** (3 per tier T1-T3, 1 ultimate at T4):

| Tier | Unlock Level | Skills | Slot cost per upgrade level |
|------|--------------|--------|------------------------------|
| 1 | Level 1 | 3 | 1,1,2,2,3 (9 total per skill) [PART4] |
| 2 | Level 10 | 3 | 1,1,2,2,3 (9 total) [PART4] |
| 3 | Level 20 | 3 | 2,2,2,3,3 (12 total) [PART4] |
| 4 (Ultimate) | Level 35 | 1 | 3,3,3,4,4 (17 total) [PART4] |

**Full tree cost = 107 SP.** A 50-level main companion earns 100 SP — *cannot* max even a single tree. **Specialization is mandatory.** [PART4 §"Skill Cost Breakdown" note]

### 1.3 Skill types

- **Active** — 70 % of all skills (140/200). Cooldown-gated abilities triggered by player/AI input. [PART4 §"Skills by Type"]
- **Passive** — 21.5 % (43/200). Auto-applied stat/effect modifiers on unlock.
- **Ultimate** — 8.5 % (17/200). Tier-4 only, 1 per tree, ~180 s cooldown, transformative effects + music + camera + post-process.

### 1.4 Cross-cutting design rules

- **5 upgrade levels per skill.** Each tier of upgrade typically adds: +damage, +duration/-cooldown, then a *qualitative* level-5 modifier (armor pierce, chain, immunity, etc.). Example: `bf_heavy_blow` L5 grants 50 % armor pierce. [PART1 lines 84-89]
- **Synergies** are explicit string lists per skill: e.g. `bf_heavy_blow.synergies = ["bf_momentum", "bf_execute"]`. These are *suggested combos*, not hard-coded prereqs. They drive UI highlighting and AI hint text.
- **Prerequisites** are typed expressions of the form `"<skill_id> >= <level>"` combined with `OR`/`AND` (e.g. `["bf_execute >= 3 AND bf_berserker >= 3"]`). Tier-2 needs `Tier-1 skill ≥ 2`, Tier-3 needs `Tier-2 ≥ 3`, Tier-4 (Ultimate) needs *two* Tier-3 skills `≥ 3`. [PART1 lines 143-145, 222-224, 306-307]
- **ai_hint string** is a one-line plain-English hint stored on every skill, intended for the LLM companion brain and the codex tooltip ("Use against single tough enemies, combo starter"). [PART1 line 91 etc.]

---

## 2. Jake Hunter — Player Character (3 trees, 36 skills)

### 2.1 Character sheet [PART1 lines 16-39]

| Field | Value |
|-------|-------|
| character_id | `jake_hunter` |
| Role | Player Character / Enhanced Soldier |
| Timeline availability | All (protagonist, always playable) |
| Base HP | 150 |
| Base stamina | 100 |
| Strength | 400 (the trademark "400 % enhanced strength") |
| Speed | 8 |
| Armor | 15 |
| Crit chance / damage | 10 % / 150 % |
| Unique trait | 400 % Enhanced Strength — superhuman feats |
| Skill points per level | **2** |
| Max level | 50 |
| Total SP earned | **100** |
| Trees | 3 (Brute Force, Tactical, Survivor) |

Voice direction: *"Determined, protective. Military background shows in speech patterns."*

### 2.2 Tree 1 — Brute Force (Strength / red `#DC143C`) [PART1 lines 44-340]

Ultimate: `bf_titan` ("Titan's Might")

| Tier | Skill ID | Name | Type | Headline |
|------|----------|------|------|----------|
| 1 | bf_heavy_blow | Heavy Blow | Active | 150 dmg + 1.5 s stagger; L5 = +50 % armor pierce |
| 1 | bf_iron_grip | Iron Grip | Active | Grab enemy 5 s as shield (50 % DR); L5 grabs elites, throws for 200 dmg |
| 1 | bf_power_strike | Power Strike | Passive | +15 → +40 % melee dmg, +10 % crit at L5 |
| 2 | bf_ground_pound | Ground Pound | Active | 100 dmg / 8 m AoE + 2 s knockdown; L5 = shockwave 50 DPS / 3 s |
| 2 | bf_unstoppable | Unstoppable | Active | 15 m charge, 75 dmg + knockback; L5 = immune during charge |
| 2 | bf_momentum | Momentum | Passive | +3 → +5 % dmg per hit, up to 10-15 stacks; L5 = no decay in combat |
| 3 | bf_execute | Execute | Active | Instant kill ≤ 20 % HP (L5 = elites); 300-400 dmg if above threshold |
| 3 | bf_berserker | Berserker Rage | Active | +50 → +75 % dmg / -25 % def for 15 s; L5 = kills extend duration |
| 3 | bf_armor_shatter | Armor Shatter | Active | 100 dmg + permanent -50 → -100 armor on enemy |
| 4 | **bf_titan** | Titan's Might | Ultimate | 20-30 s: +100 % dmg, -50 % dmg taken, 1.5× size, KB/stagger immune; L5 = AoE attacks |

### 2.3 Tree 2 — Tactical (Combat / blue `#4169E1`) [PART1 lines 344-633]

Ultimate: `tac_predator` ("Apex Predator")

| Tier | Skill ID | Name | Type | Headline |
|------|----------|------|------|----------|
| 1 | tac_quick_shot | Quick Shot | Active | 80 dmg precision shot, crit on weakpoints; L5 = pierce |
| 1 | tac_combat_roll | Combat Roll | Active | 5 m roll + 0.5 s i-frames; L5 = +50 % dmg on next attack |
| 1 | tac_steady_aim | Steady Aim | Passive | +15 → +25 % accuracy, +25 % ranged dmg, +10 % crit at L5 |
| 2 | tac_marked_death | Marked for Death | Active | +25-40 % dmg taken for 15-25 s; L5 = chains on kill |
| 2 | tac_weapon_master | Weapon Master | Passive | +15-35 % weapon dmg, +20-50 % reload speed |
| 2 | tac_suppressing_fire | Suppressing Fire | Active | -50-70 % enemy accuracy in 15-20 m AoE for 8 s |
| 3 | tac_bullet_time | Bullet Time | Active | 70 % time slow, +100 % acc, +50 % crit for 5-10 s |
| 3 | tac_dead_eye | Dead Eye | Active | 400-700 dmg charged shot (2 → 1 s charge), 100 % armor pierce, L5 = wall pierce |
| 3 | tac_tactical_reload | Tactical Reload | Passive | +30-50 % reload, 25-35 % DR while reloading, L5 = guaranteed crit on first shot |
| 4 | **tac_predator** | Apex Predator | Ultimate | 20-30 s: wallhack, all crit, +100 % crit dmg, +30 % speed, silent |

### 2.4 Tree 3 — Survivor (Defense / green `#228B22`) [PART1 lines 637-907]

Ultimate: `sur_last_stand` ("Last Stand")

| Tier | Skill ID | Name | Type | Headline |
|------|----------|------|------|----------|
| 1 | sur_regen | Enhanced Regen | Passive | 2 → 5 HPS out-of-combat (1-2 HPS in combat at L3-L5) |
| 1 | sur_thick_skin | Thick Skin | Passive | 10-25 % DR + 10-30 armor |
| 1 | sur_combat_instincts | Combat Instincts | Passive | 10-30 % dodge; L5 = 50-dmg counter on dodge |
| 2 | sur_adrenaline | Adrenaline Rush | Active | +20-30 % all stats + 25 % speed 10-15 s; L5 = auto-trigger ≤ 25 % HP |
| 2 | sur_resilience | Resilience | Passive | 30-50 % status resist, 30-40 % CC reduction; L5 = stun/knockdown immune |
| 2 | sur_emergency_med | Emergency Med | Active | Heal 30-50 % HP; L5 = cleanses debuffs |
| 3 | sur_second_wind | Second Wind | Passive | Survive fatal damage once + 3-5 s invuln; L5 = heal to 25 % HP |
| 3 | sur_fortify | Fortify | Active | 60-80 % DR + KB immune 8-12 s; L5 = reflect 25 % |
| 3 | sur_battle_hardened | Battle Hardened | Passive | +25-100 max HP, +10-20 % DR |
| 4 | **sur_last_stand** | Last Stand | Ultimate | 10-18 s immortality + 50 % dmg + heal 50-100 % at end; L5 = allies get 50 % DR |

### 2.5 Cross-tree synergies (Jake)

Documented synergy links inside Jake (selected) [PART1 inline `synergies` arrays]:

- `bf_heavy_blow ↔ bf_momentum ↔ bf_execute` — melee combo chain
- `tac_bullet_time ↔ tac_dead_eye ↔ tac_predator` — precision chain
- `tac_combat_roll ↔ sur_combat_instincts` — mobility/dodge crossover (Tactical ↔ Survivor)
- `bf_iron_grip ↔ sur_human_shield` — grapple/tank crossover
- `sur_second_wind ↔ sur_last_stand ↔ sur_emergency_med` — defensive death-prevention chain

---

## 3. Sarah Chen — Hacker / Tech Specialist (2 trees, 24 skills)

### 3.1 Character sheet [PART1 lines 916-942]

| Field | Value |
|-------|-------|
| character_id | `sarah_chen` |
| Role | Hacker / Tech Specialist |
| Timeline | Omega (primary), Alpha/Beta secondary |
| Relationship | **Primary romance**, Dr. Chen's daughter |
| Base HP / Stamina | 80 / 120 |
| Intelligence | 180 |
| Speed / Armor | 10 / 5 |
| Crit chance / damage | 15 % / 175 % |
| Unique trait | Master Hacker — interfaces with any electronic system |
| SP/level / Max / Total | 2 / 50 / 100 |
| Trees | Hacker, Support |

Voice: *"Brilliant but grounded. Sarcastic humor masking deep care. Tech genius."*

### 3.2 Tree 1 — Hacker (cyan `#00CED1`) [PART1 lines 946-1234]

Ultimate: `hak_ghost` ("Ghost in the Machine")

| Tier | Skill ID | Name | Type | Headline |
|------|----------|------|------|----------|
| 1 | hak_quick_hack | Quick Hack | Active | 50-130 tech dmg + 2-3 s disable; L5 = spreads 5 m |
| 1 | hak_drone_hijack | Drone Hijack | Active | Convert drone 30-60 s; L5 = permanent control |
| 1 | hak_system_scan | System Scan | Active | Reveal electronics + weakpoints in 30-40 m, 20-30 s |
| 2 | hak_system_shock | System Shock | Active | 120-260 dmg + 3-5 s stun; L5 = chains 2 enemies |
| 2 | hak_firewall | Personal Firewall | Passive | 25-50 % tech DR + hack immune; L5 = reflect 30 % tech dmg |
| 2 | hak_turret_override | Turret Override | Active | Convert turret 45-95 s; L5 = up to 3 turrets simultaneous |
| 3 | hak_mass_shutdown | Mass Shutdown | Active | Disable all electronics in 20-35 m for 8-12 s; L5 = 200 dmg |
| 3 | hak_data_vampire | Data Vampire | Passive | 20-40 % lifesteal vs tech enemies; L5 = -3 s CD on kill |
| 3 | hak_virus | Combat Virus | Active | 30-50 DPS / 10-15 s, spreads in 8 m; L5 = 30 % slow |
| 4 | **hak_ghost** | Ghost in the Machine | Ultimate | 15-25 s: control all electronics 40 m, invuln digital form, instant hacks |

### 3.3 Tree 2 — Support (purple `#9370DB`) [PART1 line 1238 → end + PART1 cont. lines 1265-1537]

Ultimate: `sup_overwatch` ("Overwatch Protocol")

| Tier | Skill ID | Name | Type | Headline |
|------|----------|------|------|----------|
| 1 | sup_tactical_scan | Tactical Scan | Active | Reveal enemies in 40-50 m for 15-25 s; L5 = +15 % dmg vs revealed |
| 1 | sup_shield_boost | Shield Boost | Active | 50-100 shield for 10-15 s; L5 = 2 allies |
| 1 | sup_weakness_analysis | Weak Point Analysis | Active | +25-50 % team crit vs target; L5 = +100 % crit dmg |
| 2 | sup_combat_stim | Combat Stim | Active | +25-40 % dmg, +20 % atk speed 15-20 s; L5 = self+ally |
| 2 | sup_hologram | Holographic Decoy | Active | Decoy taunts, 15-25 s; L5 = explodes 150 dmg on death |
| 2 | sup_energy_transfer | Energy Transfer | Active | Restore 50-80 % stamina + -5-8 s CDs; L5 = full + reset random CD |
| 3 | sup_emergency_protocol | Emergency Protocol | Active | Heal 40-60 % + shield 50-75 + cleanse; L5 = revive downed ally |
| 3 | sup_jamming_field | Jamming Field | Active | -40-60 % enemy accuracy, +50 % enemy CD, no reinforcements at L5 |
| 3 | sup_neural_link | Neural Link | Active | Share dmg with ally 20-30 s; L5 = ally inherits 50 % of Sarah's abilities |
| 4 | **sup_overwatch** | Overwatch Protocol | Ultimate | 20-30 s: +30 % team dmg, -20 % team DR, +30 % team CDR, full map vision; L5 = team auto-revive once |

### 3.4 Sarah cross-character synergies

- `sup_tactical_scan ↔ hak_system_scan` — combined intel
- `sup_weakness_analysis ↔ hak_quick_hack` — debuff stack
- `sup_neural_link ↔ sup_combat_stim` — buff link chain
- `sup_shield_boost → hak_firewall` — protection stack
- `hak_data_vampire ↔ sup_energy_transfer` — sustain loop

---

## 4. Emily Watson — Stealth / Scout / Analyst (2 trees, 24 skills)

### 4.1 Character sheet [PART1 lines 1758-1783]

| Field | Value |
|-------|-------|
| character_id | `emily_watson` |
| Role | Stealth / Scout / Analyst |
| Timeline | Alpha (full), Beta/Omega limited |
| Availability | **Timed rescue, Floor 5, 3-minute window** |
| If not rescued | Transforms into **"The Oracle"** boss |
| Base HP / Stamina | 70 / 130 |
| Intelligence | 170 |
| Speed | 12 (fastest companion) |
| Crit / Crit dmg | 20 % / 200 % (highest in cast) |
| Unique trait | Analytical Mind — auto-reveals enemy weak points |
| SP/level / Max / Total | 2 / 50 / 100 |
| Trees | Infiltrator, Analyst |

### 4.2 Tree 1 — Infiltrator (Stealth / black `#2F2F2F`) [PART1 lines 1787-1875 + PART2 lines 1-208]

Ultimate: `inf_shadow_strike` ("Shadow Strike")

| Tier | Skill ID | Name | Type | Headline |
|------|----------|------|------|----------|
| 1 | inf_cloak | Optical Cloak | Active | Invisible 8-18 s; L5 = +100 % dmg first strike from stealth |
| 1 | inf_backstab | Backstab | Active | 200-450 dmg from behind, guaranteed crit; L5 = 3 s silence |
| 1 | inf_silent_movement | Silent Movement | Passive | -50-70 % detection range; L5 = silent sprint |
| 2 | inf_smoke_bomb | Smoke Bomb | Active | 8-11 m smoke 8-12 s + 3 s cloak; L5 = toxic 15 DPS |
| 2 | inf_marked_prey | Mark Prey | Active | +25-50 % dmg vs marked, track through walls 30 s; L5 = kill→5 s cloak |
| 2 | inf_distraction | Distraction Device | Active | Lure enemies 10-15 s; L5 = 100-dmg explosion on destroy |
| 3 | inf_assassinate | Assassinate | Active | Instant-kill standard enemies from stealth; L4 = silent kill (don't break cloak); L5 = mini-boss 500 dmg |
| 3 | inf_shadow_step | Shadow Step | Active | 15-20 m teleport + 2-3 s cloak; L5 = phase walls |
| 3 | inf_crippling_strike | Crippling Strike | Active | 60-80 % slow for 8 s + 75-150 dmg; L5 = no sprint/dodge |
| 4 | **inf_shadow_strike** | Shadow Strike | Ultimate | 10 s: chain-assassinate 5-10 targets, instant teleport between, 75 % time slow; L5 = +10 % HP heal per kill |

### 4.3 Tree 2 — Analyst (Intelligence / blue `#4169E1`) [PART2 lines 212-499]

Ultimate: `an_perfect_analysis` ("Perfect Analysis")

| Tier | Skill ID | Name | Type | Headline |
|------|----------|------|------|----------|
| 1 | an_weakness_analysis | Weakness Analysis | Active | +20-35 % team dmg vs analyzed target, 20 s; L5 = spreads 5 m |
| 1 | an_reconnaissance | Reconnaissance | Active | Reveal enemies/hazards/loot 40-50 m for 15-25 s; L5 = permanent shared vision in range |
| 1 | an_critical_insight | Critical Insight | Passive | +10-20 % crit chance, +25-60 % crit damage |
| 2 | an_probability_field | Probability Field | Active | +25-30 % team accuracy + 15-20 % evasion; L5 = 25 % chance to fully negate attacks |
| 2 | an_exploit_weakness | Exploit Weakness | Passive | +30-50 % weakpoint dmg; L3 = ignore armor on weakpoints |
| 2 | an_tactical_prediction | Tactical Prediction | Active | See attack telegraphs 1.5-2.5 s earlier; L5 = perfect dodges = 100-dmg counter |
| 3 | an_data_spike | Data Spike | Active | 100-200 tech dmg + 8-12 s confusion; L5 = confused enemies attack each other |
| 3 | an_predictive_model | Predictive Model | Passive | +10-20 % team dmg/defense; L3 = +10 % team crit; L5 = -15 % CD team |
| 3 | an_overload_analysis | Analysis Overload | Active | 5-10 s stun + 50-75 % vulnerability; L5 = affects mini-bosses |
| 4 | **an_perfect_analysis** | Perfect Analysis | Ultimate | 15-25 s: see all, all crit, +50 % team dmg, perfect evasion; L5 = team ignores armor |

---

## 5. K'thara — Salvari Warrior / Pilot (2 trees, 24 skills)

### 5.1 Character sheet [PART2 lines 506-535]

| Field | Value |
|-------|-------|
| character_id | `kthara` |
| Role | Salvari pilot / warrior |
| Timeline | All (joins in Act 2 via First Contact sequence) |
| Species | Salvari |
| Age equivalent | 32 |
| Ship | Storm Runner |
| Base HP / Stamina | 150 / 100 |
| Strength | 280 |
| Speed / Armor | 11 / 20 |
| Crit / Crit dmg | 12 % / 175 % |
| Unique trait | **Earth Enhancement** — +40 % str, +50 % jump, +30 % stamina from Earth's O₂-rich atmosphere |
| SP/level / Max / Total | 2 / 50 / 100 |
| Trees | Warrior, Pilot |

### 5.2 Tree 1 — Warrior (Combat / teal `#00CED1`) [PART2 lines 539-832]

Ultimate: `war_ancestral_fury` ("Ancestral Fury")

| Tier | Skill ID | Name | Type | Headline |
|------|----------|------|------|----------|
| 1 | war_enhanced_leap | Enhanced Leap | Active | 20-25 m leap + 100-200 dmg slam; L5 = 2 s stun in radius |
| 1 | war_bioluminescent_strike | Bioluminescent Strike | Active | 80-160 dmg + 3-4 s blind; L5 = blind spreads 5 m |
| 1 | war_salvari_resilience | Salvari Resilience | Passive | 50-75 % env resist, 40-60 % poison resist; L5 = immune poison/radiation |
| 2 | war_oxygen_rush | Oxygen Rush | Active | +50-75 % str, +30 % speed, +50 % jump for 12-17 s; L5 = 25 HPS regen |
| 2 | war_dual_blade | Dual Blade Mastery | Passive | +20-35 % melee dmg, +15-25 % atk speed; L5 = every 5th hit double dmg |
| 2 | war_whirlwind | Whirlwind Strike | Active | 4-6 hits × 40-60 dmg in 5 m; L5 = final hit KB |
| 3 | war_predator_senses | Predator Senses | Active | Sense life 50-60 m, +30-50 % crit vs sensed; L5 = reveal cloaked |
| 3 | war_bloodrage | Blood Rage | Passive | +1.5-2.5 % dmg per 1 % HP lost (cap 100 %); L5 = 15 % lifesteal below 50 % HP |
| 3 | war_ancestral_blade | Ancestral Blade Dance | Active | 100-150 DPS in 8 m for 6-8 s; L5 = invulnerable during dance |
| 4 | **war_ancestral_fury** | Ancestral Fury | Ultimate | 15-25 s: +150 % dmg, +100 % atk speed, -50 % dmg taken, 30 % lifesteal; L5 = kill→+3 s |

### 5.3 Tree 2 — Pilot (Support / steel `#4682B4`) [PART2 lines 836-1121]

Ultimate: `pil_storm_runner_strike` ("Storm Runner Strike")

| Tier | Skill ID | Name | Type | Headline |
|------|----------|------|------|----------|
| 1 | pil_seeker_link | Seeker Link | Active | Track enemies 60-70 m for 20-30 s; L5 = SEEKER marks priority for +25 % dmg |
| 1 | pil_emergency_beacon | Emergency Beacon | Active | +15-20 % team dmg, 5-10 HPS in 12-17 m for 15 s; L5 = mobile beacon |
| 1 | pil_pilot_reflexes | Pilot Reflexes | Passive | 10-20 % dodge, 15-25 % faster reactions; L5 = 50-dmg counter on perfect dodge |
| 2 | pil_emergency_maneuvers | Emergency Maneuvers | Active | 2-3 s invuln dash 10-15 m; L5 = leaves decoy |
| 2 | pil_salvari_tech | Salvari Technology | Passive | +10-20 % weapon dmg, +15-25 armor; L5 = can equip Salvari weapons |
| 2 | pil_target_lock | Target Lock | Active | Guaranteed hits + 30-50 % dmg vs target for 10-15 s |
| 3 | pil_orbital_scan | Orbital Scan | Active | Reveal 100-150 m for 30 s; L5 = revealed enemies take +20 % dmg |
| 3 | pil_supply_drop | Supply Drop | Active | 3-5 health + ammo packs; L5 = +1 Salvari weapon |
| 3 | pil_covering_fire | Storm Runner Covering Fire | Active | 75-125 DPS in 10 m strip for 8-12 s; L5 = -50 % enemy accuracy |
| 4 | **pil_storm_runner_strike** | Storm Runner Strike | Ultimate | 5-10 strikes × 300 dmg in 20 m, multiple strikes; L5 = final beam 2000 dmg |

---

## 6. Support Companions (5 characters)

### 6.1 Aria Chen — Medic / Support [PART1 lines 1545-1644]

| Field | Value |
|-------|-------|
| character_id | `aria_chen` |
| Timeline | Alpha full, Beta/Omega limited |
| Availability | **Timed rescue, Floor 3, 5-minute window** |
| If not rescued | Transforms into **"The Siren"** |
| Base HP / Stamina | 90 / 110 |
| Unique trait | Empathic Healing — healing is +25 % effective |
| SP/level | 2, Total 100 |
| Trees | Healer, Combat Medic |

**Healer tree** (12 skills, Ultimate `heal_miracle`): heal_first_aid, heal_triage, heal_med_kit, heal_regen_field, heal_purify, heal_protective_aura, heal_resuscitate, heal_empathic_bond, heal_mass_restoration, heal_miracle ["Full team heal, revive all, damage immunity"].

**Combat Medic tree** (Ultimate `cm_angel_death`): cm_training, cm_adrenaline, cm_poison, cm_antidote, cm_painkiller, cm_stimulant, cm_tranq, cm_drone, cm_transfusion, cm_angel_death ["Become combat angel — heal allies, damage enemies"].

*PART1 only provides skill-overview list for Aria; full per-skill data not yet authored — flag for future expansion.*

### 6.2 Keisha Williams — Heavy Weapons / Tank [PART1 lines 1652-1748]

| Field | Value |
|-------|-------|
| character_id | `keisha_williams` |
| Availability | **Timed rescue, Floor 4, 4-minute window** |
| If not rescued | Transforms into **"The Breeder Queen"** |
| Base HP | **200** (highest non-Keth'mar) |
| Strength | 250 |
| Speed | 6 (slowest non-Grox) |
| Armor | 30 |
| Unique trait | Heavy Weapons Expert — no movement penalty on heavy weapons |
| SP/level | 2, Total 100 |
| Trees | Heavy Weapons, Fortification |

**Heavy Weapons** (Ultimate `hw_orbital_strike`): hw_suppression, hw_explosive, hw_proficiency, hw_rocket, hw_minigun, hw_concussion, hw_airstrike, hw_ap_rounds, hw_artillery, hw_orbital_strike.

**Fortification** (Ultimate `fort_unstoppable`): fort_armor, fort_battlecry, fort_cover, fort_shield_wall, fort_endurance, fort_bodyslam, fort_rally, fort_position, fort_revenge, fort_unstoppable ["Become invulnerable, reflect damage, taunt all"].

### 6.3 Dr. Marcus Chen — Support / Intel [PART3 lines 1-322]

| Field | Value |
|-------|-------|
| character_id | `dr_chen` |
| Timeline | All (if rescued) |
| Availability | Act 1+, optional rescue Floor 6-7 |
| Relationship | Sarah's father |
| SP/level | **1** (half rate) |
| Max level / Total | 50 / **50 SP** |
| Trees | 1 — Scientist |
| Unique trait | Lab Zero Knowledge — knows facility secrets, enemy weaknesses |

Ultimate: `sci_experimental_cure` — **the canonical anti-transformation reverter**. Can restore humanity to The Siren (Aria), Breeder Queen (Keisha), and The Oracle (Emily) if used during their respective boss fights. [PART3 lines 305-309]

| Tier | Skill ID | Name | Type |
|------|----------|------|------|
| 1 | sci_lab_knowledge | Lab Zero Knowledge | Passive (+20-35 % vs Lab enemies, reveal weakpoints) |
| 1 | sci_medical_training | Medical Training | Active heal 50-100 |
| 1 | sci_chemical_analysis | Chemical Analysis | Active reveal resistances, +15-30 % dmg amp |
| 2 | sci_experimental_formula | Experimental Formula | Active random/chosen team buff |
| 2 | sci_antidote_synthesis | Antidote Synthesis | Active cleanse + 60-120 s status immunity |
| 2 | sci_research_notes | Research Notes | Passive +10-25 % team XP, +10-20 % loot quality, L5 = unique Lab Zero loot |
| 3 | sci_override_protocols | Override Protocols | Active disable security, friendly turrets 60-90 s |
| 3 | sci_weakness_exploit | Genetic Weakness | Active +100-150 % dmg vs Lab/mutated enemies for 10-15 s |
| 3 | sci_emergency_stimulant | Emergency Stimulant | Active revive at 75-100 % + 50-75 % dmg buff |
| 4 | **sci_experimental_cure** | Experimental Cure | Ultimate (5 min CD): cure infection, 100 % heal all, +30-50 % team stats, 500 dmg vs infected boss |

### 6.4 SEEKER — Ship AI / Passive Support [PART3 lines 326-552]

| Field | Value |
|-------|-------|
| character_id | `seeker` |
| Availability | Act 2+ (Storm Runner acquisition) |
| Combat stats | None — passive bonuses only |
| SP/level | 1, Max level 30, Total **30 SP** |
| Trees | 1 — Ship Systems (only 8 skills, not 12!) |
| Tier unlock levels | **1 / 8 / 16 / 24** (compressed) |

| Tier | Skill ID | Effect |
|------|----------|--------|
| 1 | sys_tactical_uplink | Passive enhanced minimap + enemy tracking 30-50 m |
| 1 | sys_medical_scanner | Passive team regen 2-5 HPS out-of-combat, 1-2 in-combat |
| 2 | sys_shield_boost | Passive team DR 5-20 % |
| 2 | sys_targeting_assist | Passive team accuracy +10-20 %, +5-10 % crit |
| 3 | sys_threat_analysis | Passive +15-25 % dmg vs Elites, +10-25 % vs Bosses |
| 3 | sys_resource_optimization | Passive 5-12 % CDR + 10-20 % stamina cost reduction |
| 4 | **sys_full_integration** | Passive: doubles all SEEKER passives, +25-40 % team XP/loot, 15-25 % team Ultimate CDR |

**SEEKER is the team's quiet force multiplier** — entirely passive, no active skills, no character to control.

### 6.5 Foreman Grox — Keth'mar Warrior [PART3 lines 559-880]

| Field | Value |
|-------|-------|
| character_id | `grox` |
| Timeline | Act 3+, Keth'zar alliance-dependent |
| Species | Keth'mar (mining caste) |
| Base HP | **250** (tankiest companion) |
| Strength | 350 |
| Speed | 5 (slowest) |
| Armor | 40 |
| Crit dmg | 200 % |
| Unique trait | Mining caste physiology — extremely tough |
| SP/level | 1, Max 40, Total **40 SP** |
| Trees | 1 — Foreman |
| Tier unlocks | 1 / 10 / 20 / 30 |

Ultimate: `fore_cave_in` — 800-1200 environmental destruction in 20 m radius, terrain change at L5.

Key skills: fore_crushing_blow, fore_stone_skin, fore_miner_endurance, fore_rockslide, fore_immovable, fore_work_song (team buff via Keth'mar work-songs), fore_seismic_slam, fore_ore_body, fore_tunnel_vision, fore_cave_in.

### 6.6 Maria Santos — Subject 6-Alpha (secret companion) [PART3 lines 887-1199]

| Field | Value |
|-------|-------|
| character_id | `maria_santos` |
| Availability | **Secret unlock** — Act 2+, hidden Lab 48 sublevel |
| Background | Lab Zero test subject who retained humanity |
| Base HP | 130 |
| SP/level | 1, Max 40, Total **40 SP** |
| Trees | 1 — Hybrid |
| Tier unlocks | 1 / 10 / 20 / 30 |

Ultimate: `hyb_human_triumph` — 20-30 s of enhanced hybrid form, immune to transformation, +25 % team dmg via inspiration. **L5 can cure one transformed companion mid-fight** (overlaps with Dr. Chen's ultimate, but with different conditions).

Tier skills: hyb_bio_claws (3-5 hits, L5 bleed), hyb_regeneration (4-8 HPS, doubled below 50 %), hyb_enhanced_senses (detect + dodge), hyb_pheromone_control (confuse enemies / calm allies), hyb_resilient_mind (mind-control immune), hyb_bio_armor (+50-100 armor temporary), hyb_bio_explosive (200-350 organic bomb + acid pool), hyb_adaptation (5-10 % resist per hit, cap 50-75 %), hyb_hive_sense (alien hive-mind intel).

---

## 7. Economy — XP, Credits, Skill Points

### 7.1 SP totals per character [PART4 lines 10-105]

| Character | SP/level | Max level | **Total SP** | Trees | Skills | Skill-levels |
|-----------|----------|-----------|--------------|-------|--------|--------------|
| Jake | 2 | 50 | 100 | 3 | 36 | 180 |
| Sarah | 2 | 50 | 100 | 2 | 24 | 120 |
| Aria | 2 | 50 | 100 | 2 | 24 | 120 |
| Keisha | 2 | 50 | 100 | 2 | 24 | 120 |
| Emily | 2 | 50 | 100 | 2 | 24 | 120 |
| K'thara | 2 | 50 | 100 | 2 | 24 | 120 |
| Dr. Chen | 1 | 50 | 50 | 1 | 12 | 60 |
| SEEKER | 1 | 30 | 30 | 1 | 8 | 40 |
| Grox | 1 | 40 | 40 | 1 | 12 | 60 |
| Maria | 1 | 40 | 40 | 1 | 12 | 60 |
| **TOTAL** | — | — | — | **17** | **200** | **1000** |

### 7.2 Cost curve per tier [PART4 lines 110-143]

```
Tier 1 skill: 1 + 1 + 2 + 2 + 3 = 9 SP (max)
Tier 2 skill: 1 + 1 + 2 + 2 + 3 = 9 SP (max)
Tier 3 skill: 2 + 2 + 2 + 3 + 3 = 12 SP (max)
Tier 4 skill: 3 + 3 + 3 + 4 + 4 = 17 SP (max)

Full tree (3 + 3 + 3 + 1 = 10 skills): 27 + 27 + 36 + 17 = 107 SP
```

A 50-level main companion earns **100 SP** — cannot quite max a single full tree, forcing the player to leave one or two L4-5 upgrades unbought even within a "pure" build.

### 7.3 Skill point sources [PART4 lines 220-226]

| Source | Yield |
|--------|-------|
| Leveling | Primary — 1 or 2 per level |
| Achievements | Bonus points for specific achievements |
| Quests | Some quests reward bonus SP |
| Secrets | Hidden areas may drop SP items |

XP curve, level cap details, and per-source quotas are not specified in the corpus — flag for future numerical balancing pass.

### 7.4 Credits & respec [PART4 lines 200-218]

| Respec mode | Cost | Where | Cooldown |
|-------------|------|-------|----------|
| **Full respec** | 1000 credits × character level | Any safe zone | None |
| **Partial respec** | 500 credits per SP refunded | Any safe zone | None |
| **Free respec** | 0 | After each Act boss kill | One-time per boss |

Credits are the universal soft currency. The corpus does not enumerate other credit sinks here (weapons/cosmetics likely elsewhere) — defer to broader EFLZ economy doc.

### 7.5 Build archetypes [PART4 lines 147-196]

The corpus documents pre-named recommended builds — useful for hint text, achievement names, and AI companion default loadouts:

**Jake builds:** `berserker` (Brute Force focus, core: bf_berserker / bf_titan / bf_execute), `tactician` (Tactical focus, core: tac_dead_eye / tac_predator / tac_bullet_time), `survivor` (Survivor focus, core: sur_last_stand / sur_second_wind / sur_fortify), `hybrid_warrior` (BF + Tactical 50/50).

**Sarah builds:** `pure_hacker` (Hacker focus, core: hak_ghost / hak_mass_shutdown / hak_virus), `combat_support` (Support focus, core: sup_overwatch / sup_combat_stim / sup_neural_link), `tech_commander` (both balanced).

Equivalent build names for other companions are not in PART4 — extend by analogy.

---

## 8. Skill UI Spec

### 8.1 Top-level information architecture [synthesized from PART4 §"Skill UI System"]

The skill menu is a per-character paged screen with this layout:

```
+---------------------------------------------------+
| <Char Portrait>  Jake Hunter  Level 17  SP: 4     |
|                                                   |
|  [Brute Force]   [Tactical]    [Survivor]         |
|  (tab)            (tab)         (tab)             |
|                                                   |
|   TIER 1  o------o------o                         |
|             |      |      |                       |
|   TIER 2  o-o    o-o    o-o   (locked @ L10)      |
|                                                   |
|   TIER 3  o-o    o-o    o-o   (locked @ L20)      |
|                                                   |
|   TIER 4         o            (locked @ L35)      |
|                                                   |
|  +------------- INFO PANEL ------------------+    |
|  | Heavy Blow  (Active) Level 2/5            |    |
|  | Cooldown 8s   Range 3   Damage 150        |    |
|  | "A devastating punch that..."             |    |
|  | <green> Current: 200 dmg, 1.5 s stagger   |    |
|  | <yellow> Next: +75 dmg (cost 2 SP)        |    |
|  | Synergies: bf_momentum, bf_execute        |    |
|  | [ Unlock Next Rank ]                       |    |
|  +-------------------------------------------+    |
+---------------------------------------------------+
```

### 8.2 Skill node states

| State | Visual |
|-------|--------|
| Locked (tier not yet unlocked) | Greyed out, padlock icon, "Requires level X" tooltip |
| Locked (prereq not met) | Greyed, "Requires <skill> ≥ N" tooltip |
| Available (can spend SP) | Glowing, tree color outline |
| Owned, upgradable | Tree color fill, level pip count (●○○○○ → ●●○○○ etc.) |
| Maxed | Solid tree color + small gold star or "MAX" badge |
| Currently selected | Bright outline, info panel mirrors |

### 8.3 Tree visualization

- **One tab per skill tree** (so Jake has 3 tabs, Sarah/Aria/etc have 2, Dr. Chen / Maria / Grox / SEEKER have 1).
- Nodes drawn in tiers (top → bottom), positions: x = tree-index × 400 px, y = (tier − 1) × −150 px; nodes within a tier laid out horizontally around center [PART4 lines 730-758].
- **Connection lines** drawn between every skill and any prereq it lists (parsed from `"skill_id >= level"` strings) [PART4 lines 760-798]. Lines drawn before nodes (z-order).
- Tree color scheme used for: tab title, line stroke, node outline, info-panel accent (see per-tree `color_primary` and `color_secondary` hex codes throughout PART1-3).

### 8.4 Info panel content (per selected skill)

Pulled directly from the skill JSON:

1. Skill name + icon + type badge (Active / Passive / Ultimate)
2. Cooldown / Duration / Range / Damage (whichever are non-zero)
3. Description string
4. **Current level effects** (green) — computed from `upgrades[currentLevel-1]`
5. **Next level effects** (yellow) — from `upgrades[currentLevel]`, with cost in SP
6. Synergies list — clickable navigation to those skills
7. AI hint (small italic line, helpful for the LLM companion brain)
8. [Unlock Next Rank] button — disabled if not enough SP / prereq fail / max level

### 8.5 Skill hotbar (in-game HUD)

- Player active skills bound to (recommended) hotbar slots **1-4** for Active skills + slot **Q** for Ultimate (X3 keybinding conventions).
- Each slot shows: icon, level pip count overlay, cooldown radial sweep, stamina cost.
- AI companions have **no visible hotbar** — their executor fires skills based on `ai_hint` text and combat heuristics (see §9 below).

### 8.6 Auxiliary screens

- **Build presets** — save up to 3 named builds per character; switching one = full respec for free if in safe zone.
- **Synergy view** — overlay highlighting all currently-active synergy links across companions when in squad menu.
- **Achievement bonus SP** — toast notification on award.

---

## 9. Engine Integration Points (X3Native)

### 9.1 Existing files this design extends

| File | Current role | Extension |
|------|--------------|-----------|
| `app/companion_controller.{h,cpp}` | LLM/scripted companion brain | Add `SkillExecutor` member; pipe `ai_hint` strings into LLM prompt context; expose `try_use_skill(skill_id, target)` for the reflex layer |
| `app/companion_squad.{h,cpp}` | Squad/formation manager | Track per-companion `SkillTreeState`; broadcast synergy aura effects (e.g. SEEKER's passive +CDR) across squad |
| `app/companion.{h,cpp}` | Per-companion data | Add `level`, `xp`, `skill_points`, `skill_levels` (id→int map), `companion_id` (`jake_hunter`, `sarah_chen`, ...) |
| `app/ally.{h,cpp}`, `app/ally_ai.{h,cpp}` | Friendly NPC root | Inherit/compose with skill executor; non-companion allies (faction soldiers, etc.) have **no** skill trees |
| `app/player.{h,cpp}` | Jake (PC) | Owns Jake's `SkillTreeState` directly (3 trees); reads input for hotbar 1-4 + Q |
| `app/monster.{h,cpp}`, `app/weapon.{h,cpp}` | Combat receivers | Damage path must honor armor-pierce, weakpoint amp, marked target multipliers, lifesteal callbacks, etc. — extend `damage_event` struct to carry these flags |
| `app/hud.{h,cpp}` | Combat HUD | Render hotbar slots + cooldown sweeps + ultimate gauge; show skill-related debuff icons on enemies (marked, analyzed, virus, etc.) |
| `app/rescue.{h,cpp}` | Timed rescue logic | Already handles the Aria/Keisha/Emily windows; on success → unlocks that companion's skill tree state; on failure → spawns transformation boss (no skill tree, this is a monster type) |

### 9.2 New systems to add

Names below are *suggested module names*, not yet present in `app/`:

| Module (new) | Responsibility |
|--------------|----------------|
| `skill_data.h` | Static schema: `SkillId`, `SkillType {Active, Passive, Ultimate}`, `SkillUpgradeLevel {cost, bonuses}`, `SkillDef`, `SkillTreeDef`. Loaded from JSON (one file per tree, namespace under `data/skills/`). |
| `skill_state.{h,cpp}` | Per-character runtime: `xp`, `level`, `available_sp`, `skill_levels[id]`, `cooldowns[id]`. Save-load hooks via `save.cpp`. |
| `skill_executor.{h,cpp}` | Resolves active-skill use: validates (CD, stamina, prereq, level), fires effects, applies cooldown. One per character (player or companion). |
| `skill_effects.{h,cpp}` | Strategy table mapping `EffectType` (Damage / Heal / Buff / Debuff / Stun / Knockback / Stealth / Reveal / ...) → handler. Extend over time. |
| `skill_tree_ui.{h,cpp}` | Menu screen (see §8). Built atop existing UI patterns in `hud.cpp` and editor UI. |
| `xp_system.{h,cpp}` | Awards XP on kills/quests/secrets, splits across active squad (per the "team XP" passives from SEEKER + Dr. Chen). |

### 9.3 Companion AI hooks

Per the user's existing companion architecture (deterministic reflex + Grok/Claude cognitive brain):

1. **Reflex layer (`companion_controller`)** — runs `skill_executor.try_use_skill()` directly when:
   - Health < threshold → use heal / shield / emergency skills.
   - Surrounded → use AoE / knockback / cloak skills.
   - High-priority target visible → mark / analyze / focus skills.
2. **Cognitive layer (LLM)** — fed each companion's full skill list with `ai_hint` strings as tool descriptions. The model returns `use_skill(<skill_id>, <target_id>)` calls. This is the canonical use of the `ai_hint` field.
3. **Squad-wide passives** — e.g. SEEKER's `sys_full_integration` doubles all SEEKER passive bonuses; `companion_squad` aggregates these into a `SquadAuraBuffs` struct queried by every executor.

### 9.4 Skills that **change AI behavior** (not just stats)

From the corpus, these skills materially alter how a companion *fights*, not just numbers, and must be hooked at the AI layer:

| Skill | Companion | Behavior change |
|-------|-----------|----------------|
| `hak_drone_hijack`, `hak_turret_override` | Sarah | Adds *converted enemy* to ally roster temporarily |
| `sup_hologram` | Sarah | Spawns AI-controlled decoy unit, draws aggro |
| `inf_cloak`, `inf_silent_movement` | Emily | Enables stealth-approach mode; companion pathfinder picks flanks |
| `inf_assassinate` | Emily | Companion AI will *seek* lone, unaware enemies first |
| `war_predator_senses` | K'thara | Enables tracking through walls — AI never loses target |
| `pil_seeker_link`, `pil_orbital_scan` | K'thara | Broadcast revealed enemies to whole squad's AI as known targets |
| `sci_override_protocols` | Dr. Chen | Flips faction of turrets/security NPCs in scene |
| `fort_battlecry`, `fort_rally` | Keisha | Tank stance — taunts enemies, becomes aggro magnet |
| `heal_triage` | Aria | Auto-prioritize lowest-HP ally for next healing skill |
| `hyb_pheromone_control` | Maria | Confused enemies attack each other (faction flip per-target) |
| Every Ultimate | All | Triggers music cue, post-process FX, camera effect; AI saves these for boss / desperate moments per `ai_hint` |

### 9.5 Tech tree vs skill tree distinction

The corpus does **not** differentiate "tech tree" from "skill tree" — there is only one progression system per character. SEEKER's `Ship Systems` and Sarah's `Hacker` tree are technology-themed but still consume the same SP currency and use the same UI. **Do not implement a separate tech tree.**

### 9.6 Save format additions

Per-character persistent state (extend `save.cpp`):

```
companion_state {
  string companion_id      // "jake_hunter", ...
  int    level             // 1..max_level
  int    xp                // toward next level
  int    available_sp      // unspent
  map<string,int> skill_levels   // skill_id → 0..5
  int    free_respecs_remaining  // 1 per Act boss defeated
}
```

---

## 10. Recommended Build Order (Jake-first, companions later)

The user has already shipped companion controller, ally, squad, weapons, monster. The skill system is best layered in stages so the engine stays bootable:

### Phase A — Data + UI scaffolding (Jake's tree only)

1. Define `skill_data.h` schema (no behavior yet).
2. Author `data/skills/jake_brute_force.json` from PART1 lines 44-340 verbatim.
3. Implement `skill_state.{h,cpp}` + `xp_system.{h,cpp}` for **the player only**.
4. Wire the skill tree screen (`skill_tree_ui`) — view + select + spend SP. No effects yet.
5. Award test XP via cheat console.
**Milestone:** Jake can open menu, see Brute Force tree, spend SP, see node states update.

### Phase B — Active skill execution (Jake's Brute Force tree)

6. Implement `skill_executor` + cooldown tracking.
7. Implement first 3 effect handlers: Damage, Stagger, Knockdown (covers bf_heavy_blow, bf_ground_pound, bf_unstoppable).
8. Hook player input to hotbar 1-4.
9. Add HUD cooldown indicators.
**Milestone:** Jake punches enemies with Heavy Blow, AoE-slams with Ground Pound.

### Phase C — Passives + remaining Jake trees

10. Implement passive-effect application path (stat mod stack).
11. Author Tactical and Survivor JSON. Implement remaining effect types (mark, suppress, dodge, regen, fortify, etc.).
12. Implement Jake's three Ultimates (titan, predator, last_stand) — including music trigger, post-process hook, camera shake.
**Milestone:** Jake's full 3-tree progression playable through Act 1.

### Phase D — Squad XP + first companion (Sarah)

13. Extend XP system to distribute XP to active squad members (per `sup_overwatch` / `sci_research_notes` patterns).
14. Implement `skill_executor` on `companion_controller`; expose `try_use_skill` to reflex layer.
15. Author Sarah's two trees and wire her hacker behavior changes (drone hijack, turret override → faction flip).
16. Inject `ai_hint` strings into Sarah's LLM tool descriptions.
**Milestone:** Sarah autonomously hacks drones in combat; quick-hacks visible to player.

### Phase E — Timed-rescue companions (Aria, Keisha, Emily)

17. Wire `rescue.cpp` to unlock corresponding companion's `companion_state` on successful rescue.
18. Author Aria (Healer + Combat Medic), Keisha (Heavy Weapons + Fortification), Emily (Infiltrator + Analyst).
19. Implement Emily's stealth AI mode (cloak + assassinate priority targets).
20. Implement the **un-rescued** path — failed rescues spawn transformation bosses (no skill tree, treat as monster type with unique mechanics).
**Milestone:** Three rescue branches functional, including the boss path.

### Phase F — Act-2/3 companions

21. K'thara (joins Act 2 via First Contact). Warrior + Pilot trees. Wire `pil_storm_runner_strike` to Storm Runner spawn + flyby visual.
22. Dr. Chen (optional rescue Floor 6-7). Scientist tree. **Critical: implement `sci_experimental_cure` as the cure-transformation hook** that can restore Aria/Keisha/Emily in their boss fights.
23. SEEKER (Act 2+, passive-only). Implement `SquadAuraBuffs` so SEEKER's bonuses pipe through every other companion's executor.
24. Grox (Act 3+, Keth'zar alliance dependent). Foreman tree, including `fore_cave_in` terrain change (level geometry mutation).
25. Maria Santos (secret unlock in hidden Lab 48 sublevel). Hybrid tree with overlapping cure ability (`hyb_human_triumph` L5).

### Phase G — Polish

26. Build presets, free post-boss respec, achievement-driven SP rewards.
27. Synergy highlighting in UI.
28. Balance pass (the corpus does not provide XP curves — derive from playtest).
29. Per-skill animation/SFX/VFX hookup (audio_root + fx + anim already exist).

### 10.1 Why this order

- Phase A-C gives the player a *working RPG layer* on Jake alone, without touching companion AI. Stops the engine from regressing.
- Phase D introduces the **companion-skill execution path** with a single, well-understood character (Sarah) before scaling to 9 more.
- Phase E unblocks the **rescue-or-boss** narrative branch, which is core to EFLZ's design.
- Phase F is the *additive* expansion — every later companion is a new JSON file + a few behavior hooks, not a new system.

---

## Appendix A — Skill totals by tier (system-wide)

| Tier | Unlock level | Skills (across all 17 trees) | Notes |
|------|-------------|----------|-------|
| Tier 1 | 1 | 51 (3 per tree × 17, but SEEKER has 2) → actually 50 ⁕ | SEEKER tree has only 8 skills total (PART4 line 1018 lists 51, treat as approximate) |
| Tier 2 | 10 (SEEKER: 8) | 51 ⁕ | |
| Tier 3 | 20 (SEEKER: 16) | 51 ⁕ | |
| Tier 4 (Ultimate) | 35 (SEEKER: 24; Grox/Maria: 30) | 17 | One ultimate per tree |
| **Total skills** | — | **200** | [PART4 lines 993-1023] |

⁕ Per PART4's own tier-summary table — minor inconsistency with the SEEKER 8-skill tree (which compresses to 2 + 2 + 2 + 1 + 1 nodes) is noted but unresolved in the corpus.

---

## Appendix B — Ultimate-skill catalog (full list, 17)

| Char | Tree | Ultimate ID | Name |
|------|------|-------------|------|
| Jake | Brute Force | bf_titan | Titan's Might |
| Jake | Tactical | tac_predator | Apex Predator |
| Jake | Survivor | sur_last_stand | Last Stand |
| Sarah | Hacker | hak_ghost | Ghost in the Machine |
| Sarah | Support | sup_overwatch | Overwatch Protocol |
| Aria | Healer | heal_miracle | Miracle |
| Aria | Combat Medic | cm_angel_death | Angel of Death |
| Keisha | Heavy Weapons | hw_orbital_strike | Orbital Strike |
| Keisha | Fortification | fort_unstoppable | Unstoppable Force |
| Emily | Infiltrator | inf_shadow_strike | Shadow Strike |
| Emily | Analyst | an_perfect_analysis | Perfect Analysis |
| K'thara | Warrior | war_ancestral_fury | Ancestral Fury |
| K'thara | Pilot | pil_storm_runner_strike | Storm Runner Strike |
| Dr. Chen | Scientist | sci_experimental_cure | Experimental Cure (cures transformations) |
| SEEKER | Ship Systems | sys_full_integration | Full System Integration |
| Grox | Foreman | fore_cave_in | Cave-In |
| Maria | Hybrid | hyb_human_triumph | Human Triumph (can cure transformations) |

---

## Appendix C — Open questions / gaps in the corpus

The following are NOT specified in PART1-4 and must be resolved during implementation:

1. **XP curve.** Per-level XP requirements never given. Suggest exponential: `xp_to_next = 100 × 1.15^(level-1)`.
2. **XP sources.** Per-enemy XP values, per-quest XP, per-secret SP yields — flag for balancing pass.
3. **Active stamina costs.** Default placeholder of 15 in PART4 line 636; per-skill stamina costs not authored.
4. **Aria full skill data.** PART1 gives only `skills_overview` lists for Aria — full per-skill upgrade tables need authoring matching the Jake/Sarah/Emily/K'thara pattern.
5. **Keisha full skill data.** Same — overview-only in PART1, needs authored upgrade tables.
6. **Companion auto-spend.** When a companion levels, does the player spend their SP (via menu) or do they spend it themselves per a default build template? Recommend: player chooses, with auto-build presets per archetype.
7. **Multi-target/projectile semantics.** "Pierces targets" / "Chains to N enemies" / "Bounces" mechanics need engine specification.
8. **Per-skill animation/VFX/SFX asset names.** Many skills reference `anim_*`, `vfx_*`, `sfx_*` IDs — content authoring deferred to art/audio pipeline.

---

*End of spec. Total 200 skills documented from 4 source files, ~190 KB → this single index.*
