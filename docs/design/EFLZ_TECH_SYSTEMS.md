# EFLZ — Tech Systems + Bio-Integration Lab Spec
> Synthesized 2026-05-31 from TASK_8 (Technical Systems, 3 parts ~185 KB total).
> Authority: Tim's design corpus at G:\Unity_Projects\PARALLEL_TASKS_8_CHATS\.
> Clean-room safe — Tim's own IP. **No code changes; design doc only.**

---

## 1. Overview + Sources

### 1.1 What this document is
A design specification synthesized from the EFLZ (Escape From Lab Zero) "Technical Systems" task corpus, mapped onto the X3Native C++/Vulkan engine. It describes the **gameplay systems that the rendering/sim code will need to call into**: the Bio-Integration Enhancement Lab (Floor 4 Augmentation Bay), the Augmentation + Humanity meter system, the Infection / Cure plumbing, the Drone economy, the Player Inventory, and the shader/VFX features required to render their feedback.

It is **NOT** a re-spec of the rendering engine (PBR, bindless, glass pass, holo terminals, F2 rescue, F4.5 Nexus/Chorus, F7 sub-levels, companion controller — all already shipped on X3Native and listed in section 8 as "current baseline"). It calls out only the **gaps** between what TASK_8 expects and what X3Native currently provides.

### 1.2 Three input files actually read

| File | Size | What's in it | What's NOT in it |
|------|------|--------------|------------------|
| `TASK_8_TECHNICAL_SYSTEMS.md` | 27 KB | High-level outline: shader stubs (WireframeDrone, InfectionSpread, AlienTech), level streaming manager, object pool, LOD/culling presets, SD texture pipeline overview, NUnit test framework outline, manual QA checklist. | Bio-Integration Lab spec, augmentation rules, humanity thresholds, infection/cure plumbing, drone economy, inventory plumbing. |
| `TASK_8_TECHNICAL_SYSTEMS_PART1_SHADERS.md` | 49 KB | 10 complete HLSL shader implementations (HDRP). | Same as above — pure rendering. |
| `TASK_8_TECHNICAL_SYSTEMS_PART2.md` | 106 KB | Full object pool, LOD/culling manager, performance profiler, Python SD pipeline w/ 50+ texture definitions, automated NUnit test suite, manual QA checklist, build & deployment guide. | Same as above — engine plumbing, not gameplay systems. |

### 1.3 Key finding — the named "Bio-Integration Enhancement Lab"
The literal phrase "Bio-Integration Enhancement Lab" **does not appear** in any TASK_8 file or in the wider `PARALLEL_TASKS_8_CHATS` corpus (verified by grep across 50+ files, 2026-05-31). The room Tim is referring to is the **Floor 4 Forced Augmentation Bay** — the canonical name in the design corpus is "Augmentation Bay" inside the "Cybernetics Workshop." Its full spec lives in:

- `G:\Unity_Projects\PARALLEL_TASKS_8_CHATS\Floors3-4_Complete\RoomLayouts\Floor4_CyberneticsWorkshop_Layouts.md` — room geometry, station layout, lighting, audio, enemy spawns.
- `G:\Unity_Projects\PARALLEL_TASKS_8_CHATS\Floors3-4_Complete\Scripts\Floor4\AugmentationSystem.cs` — the gameplay code that runs the room.
- `G:\Unity_Projects\PARALLEL_TASKS_8_CHATS\Floors3-4_Complete\Scripts\Floor4\HumanityMeter.cs` — the moral cost meter the room writes to.
- `G:\Unity_Projects\PARALLEL_TASKS_8_CHATS\Floors3-4_Complete\Scripts\Floor4\Floor4_CyberneticsManager.cs` — orchestrator.

For the duration of this doc we use the name "**Bio-Integration Enhancement Lab**" (Tim's branding) interchangeably with "Forced Augmentation Bay" (corpus name). Where parameters are cited, the source line is in the cybernetics file at the path above.

### 1.4 Why TASK_8 alone is not enough
TASK_8 is the **engine-and-pipeline** task. The gameplay rules Tim wants wired up belong to TASK_1 (Floors 3-4 implementation), TASK_2 (Floors 5-7), TASK_9 (missing scripts spec), TASK_11 (skill trees + economy), TASK_12 (crafting system). This doc cross-references those wherever a TASK_8 hook would need a contract from elsewhere.

---

## 2. Bio-Integration Enhancement Lab — High-Priority Spec

> **Canonical room file:** `Floors3-4_Complete\RoomLayouts\Floor4_CyberneticsWorkshop_Layouts.md` ROOM 1: AUGMENTATION BAY, lines 10–194.
> **Canonical script:** `Floors3-4_Complete\Scripts\Floor4\AugmentationSystem.cs`, lines 1–531.
> **Status in X3Native:** Floor exists (spire_top includes F5/F6/F7; F4 is between Nexus/Chorus area at F4.5 and the Cybernetics Workshop above); the *room*, the *stations*, and the *system* have **not** been ported. This is the top gap to close after the F7 sub-levels currently in progress.

### 2.1 What the room *is*
A 45 m × 35 m × 8 m sterile surgical bay on Floor 4 of the lab spire, six **Augmentation Stations** arrayed in two rows of three (positions in F4 layout file lines 22–62). It is the gameplay room where the player chooses to trade **humanity** for **stat upgrades** — the lab's one major character-builder room, and the only point in the main spire where the Humanity meter goes down by player choice (not by being forced).

Sarah (the companion / second playable) can additionally hack the central console (line 67) to disable security and unlock alternate paths through the room.

### 2.2 The six stations (gameplay states)

| # | World pos (corpus coord) | State | Player interaction | Humanity cost |
|---|--------------------------|-------|--------------------|---------------|
| 1 | (-10, 36, -12) | ACTIVE forced surgery on NPC, drill arm descending | **Rescue minigame**: 30 s timer to hit interrupt button at (-12, 37, -12). Success: +150 XP + future ally. Failure: NPC becomes Augmented_Guard enemy. | n/a (rescue) |
| 2 | (0, 36, -12) | Dead subject, augments harvested | **Loot**: drops tech parts (drone economy input). | n/a |
| 3 | (10, 36, -12) | Empty, powered down | **Player-usable**: install Strength +15% OR Speed +20% | **10%** |
| 4 | (-10, 36, 12) | Malfunctioning, subject alive begging | **Moral choice**: mercy-kill (+humanity) OR leave alive (+infection torment, no humanity) | n/a (+humanity gain on mercy) |
| 5 | (0, 36, 12) | Training hologram playing | **Cosmetic only**: "Making Better People, One Part at a Time" cheerful tutorial — environmental storytelling beat | n/a |
| 6 | (10, 36, 12) | Calibrated for "400% enhanced subjects" (Jake-only) | **Jake-only premium**: Reinforced Skeleton (+armor) OR Neural Boost (+reaction) | **15%** |

(Source: F4_CyberneticsWorkshop_Layouts.md lines 22–62.)

### 2.3 Central Control Console (0, 36, 0)
- Holographic interface (uses existing **holo-terminal** material; already shipped).
- Lists hundreds of "volunteer" names — environmental storytelling.
- **Sarah hack** disables surgical-arm hostility, unlocking quieter run through. Without Sarah, if the player attempts to brute-hack, **all 8 surgical arms become hostile** and **all doors lock for 60 s**. (Layout file lines 117–119.)

### 2.4 Surgical Robot Arms (ceiling)
- 8 arms, ceiling-mounted, 200 HP each. One arm always active over Station 1 (the rescue victim).
- On alarm: ALL arms detach from rails and become hostile flying combatants. (Lines 69–74.)
- Two arms "roam" the ceiling rails as ambient threat.

### 2.5 The observation window beat (story trigger)
At world pos (20, 38, ±15) — a row of windows. Walking into the trigger volume reveals the **alien armada outside** for the first time. Jake line: *"Jesus... that's not a research vessel. That's an armada."* (Lines 75–80.) Triggers `Dramatic_Revelation_Sting` music cue and spawns one **Heavy_Cyborg** ("WITNESS PROTECTION PROTOCOL ACTIVE") through the ceiling (line 121).

This beat establishes Act-1 → Act-2 stakes — confirms the invasion is bigger than the lab.

### 2.6 Environmental storytelling props
- "Employee of the Month" board (line 82): photo wall of Dr. Rodriguez, Dr. Tanaka, others. Note: "Most Successful Integrations - July." Disturbing reframing of corporate normalcy.
- Reject Bin (line 87): Failed augmentation parts, some with tissue. **Lootable** — drops Drone Parts (see §5).
- Patient Restraint Chair (line 92): Broken free, drag marks toward a vent, fingernails embedded in armrest. Vent leads to a hidden side-room (TBD layout, not in F4 file but implied).
- Children's Drawing at Station 4 (line 96): "Get well soon Daddy" with robot arms. Established that subject in Station 4 is a parent — emotional weight for the mercy-kill choice.

### 2.7 Lighting (already-supported features only)
- Surgical spotlights at each station: 2000 lumens focused cones (use existing point-light system).
- Station 1: red warning strobe (animated emissive on existing PBR material).
- Floor circuit lines: cyan glow (RGB 0,200,255), emissive stripe along floor tile geometry.
- Console blue holographic light reflecting on floor — already supported via existing holo glass shader.

(Lines 136–162. No new shader features required for the room itself.)

### 2.8 Audio bed (gameplay-mixed)

| Loop layer | Source | Trigger |
|-----------|--------|---------|
| `Surgical_Hum` | Ambient bed | Room enter |
| `Ventilation_Clean` | Ambient bed | Room enter |
| `Heartbeat_Monitor` | Per-active-station | Stations 1, 4 |
| `Distant_Screaming` | Ambient bed (-20 dB) | Room enter |
| `Drill_Whine` | Station 1 specific | While timer running |
| `Flatline` | Station 2 | Continuous |
| `Cheerful_Tutorial` | Station 5 | Looped |
| `Augmentation_Complete` | One-shot | Player uses Station 3 or 6 |
| `Robot_Arm_Activate` | Per-arm | Alarm raised |
| `Fleet_Rumble` | Sub-bass | At observation window |
| `Tension_Countdown` | Music stinger | Station 1 timer entered |
| `Dramatic_Revelation_Sting` | Music stinger | Observation window |
| `Combat_Augmented_Enemies` | Music | Combat triggered |

Reverb: clinical, 0.8 s decay. (Lines 166–193.)

### 2.9 Gating — what this room gates
1. **Story gate:** observation window beat unlocks Act-2 (alien-fleet awareness) → required for elevator to Floor 5.
2. **Build gate:** all six stations are reachable in one run; player typically gets 1-2 augments here before being forced into combat. With Sarah hacking, all six are reachable peacefully — the **only** safe place in the spire to install augments without an alarm.
3. **Humanity branching gate:** if the player installs 4+ augs across the spire, the Humanity meter drops below the 50% "Good Ending" threshold (see §3). This room is where most players first cross that line.
4. **Ally gate:** rescuing the Station 1 victim recruits a future companion (NPC name not yet bound in the corpus — TBD).

### 2.10 Engine inputs the room needs (call-out to gameplay code)
This is the contract — what the X3Native gameplay layer must provide for the room to function:

- `IAugmentationStation::Interact()` — applies an `AugmentationData` and decrements humanity.
- `IHumanityMeter::Current()`, `::Reduce(amount, source)`, `::IsGoodEndingLocked()` — see §3.
- `IRescueTimer` — already shipped (`rescue.{h,cpp}`); but Station 1 needs a *non-companion* rescue variant (Floor 2 rescue is companion-coupled). Gap noted in §8.
- `IInventory::Add(itemId, qty)` — for the Reject Bin loot, Station 2 organ-harvest tech parts. See §6.
- `INpcController::SwitchToHostile()` — on alarm, surgical arms switch.
- `IDoorLock::Lock(60s)` — for the "hack failed" lockdown beat.
- `IObservationWindowTrigger::OnEnter()` — fires armada reveal, spawns Heavy_Cyborg via ceiling-break VFX.

---

## 3. Augmentation System + Humanity Meter

> **Source files:**
> - `Floors3-4_Complete\Scripts\Floor4\AugmentationSystem.cs` (full, lines 1–531).
> - `Floors3-4_Complete\Scripts\Floor4\HumanityMeter.cs` (full, lines 1–752).

### 3.1 AugmentationData — the payload

Each augmentation is a `ScriptableObject`-style record (AugmentationSystem.cs lines 426–449):

```
augmentationName   : string
description        : string (multi-line)
icon               : Sprite
type               : AugmentationType {Strength, Speed, Armor, Hacking, Regeneration, Perception}
bonusValue         : float
humanityCost       : float
visualPrefab       : GameObject (attaches to player rig)
attachPoint        : string (bone name)
playerReaction     : string (dialogue line shown at install)
loreText           : string
```

The six **types** map 1:1 to PlayerStatType (line 416). Each type accumulates via `augmentationBonuses[type] += bonusValue` (line 153) so a player who installs two Strength augs gets the sum.

### 3.2 Catalog (canonical list from corpus)

Confirmed augs from F4 layout file + AugmentationSystem.cs catalog implications:

| Aug | Type | Bonus | Humanity cost | Available at |
|-----|------|-------|---------------|--------------|
| Reflex Boost | Speed | +20% | 10% | Station 3 |
| Strength Enhancer | Strength | +15% | 10% | Station 3 |
| Reinforced Skeleton | Armor | +X% | 15% | Station 6 (Jake-only) |
| Neural Boost | Perception | +X% (reaction) | 15% | Station 6 (Jake-only) |

Additional augs (Hacking, Regeneration) implied by AugmentationType enum but not yet bound to a station — likely earned from F5 (Drone Station — Hacking aug) and F6 (Alien Tech — Regeneration via Salvari nanite). Confirm in TASK_2 (`TASK_2_FLOORS_5-7_IMPLEMENTATION.md`).

### 3.3 Install sequence (AugmentationSystem.cs lines 96–185)

1. Validate: aug not already installed; under max-installed cap (5, line 42); humanity ≥ cost.
2. Show UI panel (`augmentationUIPanel`), play `augmentationSound`, play `augmentationEffect` particles.
3. **2-second dramatic pause** (line 147).
4. Add to `appliedAugmentations`, accumulate `augmentationBonuses[type]`.
5. Call `humanityMeter.ReduceHumanity(cost)` → cascades to UI flash + threshold checks.
6. **Visual changes** — `cyberneticSkinBlend = count / maxAugs` (line 219), set as `_CyberneticBlend` shader float on player SkinnedMeshRenderer (lines 222–233). This drives the gradual visual transformation. Also instantiates `visualPrefab` at `attachPoint` bone — physical prosthetic attaches.
7. Player reaction dialogue, scaled by current humanity level (lines 263–285): >50% humanity = aug's flavor line; <50% = "I can feel it changing me... but the power..."; <25% = "It doesn't even hurt anymore. Is that bad?"
8. `OnAugmentationApplied` event fires for listeners (achievements, story flags, etc.)

### 3.4 Humanity Meter — the moral cost dial

**Range:** 0–100. **Default start:** 100. (HumanityMeter.cs lines 47–52.)

#### 3.4.1 State buckets (lines 21–28, 429–440)

| State | Range | UI color | Vignette α | Endings locked |
|-------|-------|----------|------------|----------------|
| Full | 80–100 | green | 0 | none |
| Slight | 60–80 | yellow-green | 0 | none |
| Moderate | 40–60 | yellow | 0 | best-ending pending |
| Severe | 20–40 | orange | 0.10 | best ending LOCKED |
| Critical | 10–20 | red | 0.20 | good ending LOCKED |
| Lost | 0–10 | purple | 0.35 | only dark endings |

#### 3.4.2 Ending thresholds (lines 99–102)

- `goodEndingMinimum = 50f` — drop below and the "good" ending bucket locks (warning + event `OnGoodEndingLocked`).
- `bestEndingMinimum = 80f` — drop below and the "best" ending locks (event `OnBestEndingLocked`).
- `romanceMinimum = 60f` — drop below and **romance options become unavailable** (`CanRomance` returns false, line 143).
- `lostThreshold = 10f` — `IsHumanityLost` true; only dark endings; some dialogue trees swap to "dissolved identity" variants.

#### 3.4.3 State-transition dialogues (lines 449–489)

Hooked into `CharacterSwitcher` to pull "Jake" vs "Sarah" name. Sample lines:
- → Slight: *"These upgrades feel... strange. But I can handle it."*
- → Moderate: *"I'm starting to feel different. Less... me."*
- → Severe: *"When I look in the mirror... I barely recognize myself."*
- → Critical: *"What have I become? Where does the machine end and I begin?"*
- → Lost: System line *"{name}'s humanity has been lost to cybernetic integration."*

#### 3.4.4 Visual feedback (HumanityMeter.cs lines 65–95, 562–597)

- 6 portrait sprites (one per state) cycle automatically.
- **Player material gets `_CyberneticBlend = 1 − (humanity / max)`** set every frame the meter updates (line 577). This is the SAME shader float the AugmentationSystem writes on install (§3.3, step 6) — they reconcile (the meter override is canonical because it's recomputed from humanity, the install pulse is just the per-aug spike).
- Screen vignette ramps from 0 → 0.35 alpha through Severe → Lost states.
- Per-state UI gradient (lines 76–83) tints fill bar.
- Modification history log (lines 121, 211–219) — every aug/event keeps a stamped record of *what* changed humanity and *when*. Used in save data + end-of-run UI ("Choices that shaped you").

#### 3.4.5 Save/load (lines 361–385)

Save struct (HumanitySaveData):
```
currentHumanity     : float
maxHumanity         : float
goodEndingLocked    : bool
bestEndingLocked    : bool
modificationCount   : int
```

### 3.5 What X3Native needs to plumb for this

The C++ engine surfaces required:
- `Humanity` global singleton (one per save slot). Default 100.0, persistable.
- `PlayerMaterial::SetCyberneticBlend(float 0..1)` — must drive a shader uniform on the player mesh material. (Need new shader feature — see §7.4.)
- `PlayerStats::AddBonus(stat, amount)` — already implied by core gameplay but worth defining as a stable surface.
- `DialogueManager::Show(speaker, line)` — needed for the meter's state-transition dialogue + augmentation install reactions.
- `GameFlags::Set(name, bool)` / `GameStats::Set(name, int)` — for `humanity_depleted`, `good_endings_locked`, `best_ending_locked`, `current_humanity` (AugmentationSystem.cs lines 322–328, HumanityMeter.cs lines 617–624).
- `Audio::PlayOneShot(clip)` — for the meter's per-transition stingers (warning, critical, lost).
- `UI::ShowVignette(alpha)` — full-screen vignette overlay (lines 581–597).

---

## 4. Infection / Cure Plumbing

> **Source files:**
> - `RescueVictim.cs` (lines 1–~280) — infection-level accumulator on rescued NPCs.
> - `TASK_12_EscapeLab48_COMPLETE_CRAFTING_SYSTEM.md` — cure recipes and rates.
> - `TASK_8_TECHNICAL_SYSTEMS_PART1_SHADERS.md` lines 243–415 — `InfectionSpread.shader` (5-stage visual).
> - `TASK_8_TECHNICAL_SYSTEMS.md` lines 2517–2526 — Level 7 timer outcome bands.

### 4.1 Three different infection systems exist

Tim's design has **three distinct infection plumbings**, often confused:

1. **Per-NPC rescue infection** (Floor 2, Floor 7 Sarah rescue, F4 station 1, F4.5 Chorus victims). A timer counts down; if exceeded, the NPC transforms. *Already partially shipped in X3Native via* `rescue.{h,cpp}` *(F2 only).*
2. **Player infection** (Floor 6 alien-tech exposure, late-act bites). Persistent character state; cured by a 3-ingredient consumable.
3. **World-level infection spread** (later acts) — environmental visual + AI-priority modifier as the lab loses containment. Less plumbing, mostly cosmetic.

This section covers all three.

### 4.2 Per-NPC rescue infection (the "common" case)

#### 4.2.1 RescueVictim parameters (RescueVictim.cs lines 30–50)

```
infectionRate     : float    (default 0.1f per second → 100% in 1000 s — but per-victim override)
infectionLevel    : float    (0–1, grows over time)
maxRescueTime     : float    (per-victim; F2 Aria=180s, F2 Keisha=120s, F2 Emily=240s, F7 Sarah=540s)
stage             : enum     (Stage0_Clean .. Stage5_FullAlien)
canBeRescued      : bool
```

Update loop accumulates `infectionLevel += infectionRate * dt` (line 256). At stage thresholds, transforms visual + AI.

#### 4.2.2 Stage bands (corpus consensus, InfectionSpread.shader lines 285–289 + RescueVictim.cs)

| Stage | Threshold | Visual (shader) | Gameplay |
|-------|-----------|-----------------|----------|
| 0 (Clean) | 0.0 | normal skin | rescuable, friendly |
| 1 (Veins) | 0.1 | dark purple veins appear, pulse animation | rescuable, frightened |
| 2 (Discolor) | 0.3 | skin desaturation, gray patches | rescuable, paranoid |
| 3 (Chitin) | 0.5 | chitin plates breaking through, FBM displacement +0.02 on normals | last-chance rescue (Antidote stage 1-2 still works) |
| 4 (Transform) | 0.7 | mostly alien chitin, partial flesh remaining | **NO rescue** — combat only or Full Cure (stage-3 hybrid) |
| 5 (Full Alien) | 0.9 | full insectoid carapace, emissive vein glow | combat-only enemy |

#### 4.2.3 The L7 Sarah rescue outcome bands (TASK_8 manual QA, lines 2517–2526)

Sarah starts at 9-min (540 s) timer. Outcome bands:
- `9+ min remaining` → **Perfect rescue** + bonus content (impossible to hit unless skipping content)
- `6–9 min` → Sarah healthy (Stage 0)
- `2–6 min` → Sarah minor infection (Stage 1)
- `0–2 min` → Sarah partially infected (Stage 2-3)
- `expired` → Sarah transformed (combat boss instead of ally)

Each band determines a save flag that the Act-2 opening reads to pick Sarah's dialogue + appearance.

### 4.3 Cure components (TASK_12 crafting)

> **Authoritative source for recipes:** `TASK_12_EscapeLab48_COMPLETE_CRAFTING_SYSTEM.md` lines 79, 596–597, 695, 700.

#### 4.3.1 Cure-related items in the inventory

| Item ID | Display | Tier | Stack | Source |
|---------|---------|------|-------|--------|
| `cure_component` | Cure Component | Epic | 10 | Boss drops, Research data |
| `antidote` | Antidote | T2 | 5 | Crafted: 5 Toxic Residue + 3 Purified |
| `full_cure` | Full Cure | T4 | 3 | Crafted: 3 Cure Component + 5 Purified + 2 Hybrid Gland |
| `cure_prototype` | Cure Prototype | T3 | (timed quest item) | Crafted: 1 Cure Component + 3 Purified + 10 Medical + 5 Toxic — **saves Dr. Chen on F2** |
| `hybrid_stabilizer` | Hybrid Stabilizer | T4 | (one-shot) | 3 Hybrid Gland + 2 Cure Component + 5 Nano Components — stabilizes transformed allies |
| `advanced_medkit` | Advanced Medkit | T2 | 10 | 10 Medical + 2 Purified — heals 100 HP + cures *poison* (not infection) |

#### 4.3.2 Cure-success matrix

| Cure | Stage 0-1 | Stage 2 | Stage 3 | Stage 4-5 |
|------|-----------|---------|---------|-----------|
| Antidote | 100% | 100% | 0% | 0% |
| Cure Prototype | 100% | 100% | 80% (timed Chen-only) | 0% |
| Full Cure | 100% | 100% | 100% | 0% |
| Hybrid Stabilizer | n/a | n/a | n/a | 100% (locks transformed ally as combat-capable but non-hostile companion) |

Hybrid Stabilizer is the ONLY way to recover a Stage-5 NPC, and only as a hybrid (not human) — used for "best ending" content gates.

### 4.4 Cure plumbing — engine contract

For the X3Native side to wire this:
- `Infection` component on every infectable actor: `level: float`, `rate: float`, `stage: enum`. Tick in fixed sim.
- `Infection::ApplyCure(cureType)` returns success bool, consults the matrix above.
- `Infection::OnStageChanged(prev, next)` event → drives material `_InfectionProgress` (already in shader spec, see §7.1) + AI state swap.
- `Inventory::HasItems(recipe[])` / `Inventory::ConsumeItems(recipe[])` for cure-application flow.
- The cure-success matrix is **data-driven** (load from `data/cures.json` or similar) so designers can tune without code changes.

### 4.5 Player infection (variant 2 — late acts)

Less plumbed in the corpus, but key beats:
- Floor 6 Salvari power core (TASK_8 part1 line 1532) can infect the player with **Salvari hybrid markers** — *not* the same as the F2 alien infection. Visually different shader (uses `AlienTech.shader` emissive veins, NOT `InfectionSpread.shader`).
- Late-act bites from L8+ open-world enemies can apply rate-based player infection. Untreated reaches Stage 3 in ~30 min real-time, locking out specific dialogue trees.
- Cure path: Full Cure (works on player Stage 0–3) OR Salvari Bio-Crystal (works on Salvari-marker variant only, post-Floor-6).

---

## 5. Drone Economy

> **Sources:**
> - `TASK_12_EscapeLab48_COMPLETE_CRAFTING_SYSTEM.md` lines 64–96 (drone-input materials), 719 (Salvari Combat Drone recipe).
> - `TASK_11_SKILL_TREES_PART4_ECONOMY_IMPLEMENTATION.md` lines 178, 191, 204 (drone hacking skills, credit costs).

### 5.1 Currency
Single in-world currency: **Credits** (no factions, no split currencies — confirmed via grep across TASK_11). Skill respec is 500 credits per point, full respec 1000 credits per level (TASK_11 lines 204–210).

### 5.2 Drone materials (crafting inputs)

| Material | Tier | Stack | Drops from |
|----------|------|-------|------------|
| `scrap_metal` | Common | 99 | Drones, containers, environment |
| `circuit_board` | Common | 50 | Electronics, terminals, **drones** |
| `power_cell` | Common | 50 | Equipment, **drones**, vehicles |
| `salvari_alloy` | Rare (Act 3+) | (varies) | Salvari-tech crates, late-act drone husks |
| `bio_crystal` | Rare (Act 3+) | (varies) | Salvari power conduits |

(TASK_12 lines 64, 65, 96, 102.)

### 5.3 Player-summon drone economy (the "Salvari Combat Drone" example)

Recipe (TASK_12 line 719):
- 20 Salvari Alloy + 6 Bio-Crystal + 3 Adv Processor + 1 Crystal Energy
- Summons a drone with 100 HP, 25 DPS, 60-second duration
- Max stack: 2 (carry 2 at a time)

This is the player's deployable; the *enemy* drones use the Wireframe shader and follow standard combat AI rules (no economy from the player's side, they just drop materials on kill).

### 5.4 Sarah's hacked-drone economy (TASK_11 lines 178, 191)

Sarah's "Electronic warfare, drone control" playstyle:
- `hak_drone_hijack` — convert hostile drone into ally for the encounter
- Caps at **5 simultaneous hacked drones** (TASK_8 part2 line 2550 manual QA check)
- Master hack (Floor 5, the Drone Station) **unlocks all drones on the floor at once** — one-shot story ability, used in the F5 climax (lines 2549–2551).

Hacked drones don't cost credits or materials — they're an in-encounter resource. They DO consume Sarah's "drone control slots" (1 per drone), capped at her skill tree's current max.

### 5.5 Repair / Recharge (the gap)

**Tim's prompt called these out; the corpus barely defines them.** The state-of-the-design is:
- Hacked drones can be ordered to "self-repair" at a charging station (Floor 5 corpus implies, no scripts shipped).
- Drones do NOT take wear-and-tear in the strict sense; they have HP and either survive an encounter or are destroyed. There is no maintenance grind.
- Player-summon drones (Salvari) self-expire at 60 s — no recharge, just re-craft.
- **Floor 5 Drone Charging Station** is in the texture-prompts list (TASK_8 part 2 line 1507: "drone charging dock surface, electrical contacts, status LED arrays") but the gameplay loop around recharging is **not designed yet**.

**Recommendation for X3Native design:** treat drone "repair/recharge" as a Floor-5-room mechanic that restores hacked-drone HP for a credits cost (1 credit per HP, max 5 drones at once, takes 10 s real-time). This is **net new design** — confirm with Tim before implementing.

---

## 6. Player Inventory Plumbing References

> **No standalone inventory spec file exists in TASK_8.** References scattered.

### 6.1 What the corpus assumes exists

Cross-references in TASK_12 crafting script (lines 811, 825, 855):
```
InventoryManager.Instance
.GetItemCount(itemId)
.AddItem(item, quantity)
.RemoveItem(itemId, quantity)
```

Plus the QA checklist (TASK_8 part 2 line 2603: "Load restores inventory").

### 6.2 Implied interface

The X3Native engine needs an Inventory service with at minimum:
```
add(itemId, qty)       → bool
remove(itemId, qty)    → bool
count(itemId)          → uint32
hasAll(recipe[])       → bool
consumeAll(recipe[])   → bool (atomic)
list() → ItemStack[]
serialize() / deserialize()
```

### 6.3 Item taxonomy (from TASK_12)

32 materials across 6 categories:
1. **Basic** (scrap_metal, circuit_board, etc.) — common crafting inputs.
2. **Medical** (medical, purified, toxic_residue, etc.) — cure inputs.
3. **Biological** (hybrid_gland, organic_matter) — boss drops.
4. **Electronics** (power_cell, advanced_processor, nano_components) — drone/tech crafting.
5. **Salvari Materials** (salvari_alloy, bio_crystal, crystal_energy) — Act 3+ alien tech.
6. **Special** (cure_component, etc.) — story items.

Plus crafted consumables: medkits, antidotes, cures, deployables, weapon upgrades.

### 6.4 Stack sizes
Mostly defined: common = 99, medical = 50, boss-drops = 5–10, story items = 1–3. Lookup is per-item in the recipe table.

### 6.5 Crafting station scopes (TASK_12 lines 129–130)
- Chemistry Lab (Floor 2): Consumables, chemical weapons, antidotes — **1.0× efficiency**.
- Med Bay (Floor 2): Consumables, medical equipment, cures — **0.9× efficiency**.

Efficiency multiplier likely affects yield or ingredient cost — TBD which.

### 6.6 Engine surface needed
- C++ `Inventory` class scoped per-player-character (Jake and Sarah share, per QA spec "inventory persists" line 2603 — not split).
- Serializable as JSON blob for save/load.
- UI integration with already-shipped holo-terminal aesthetic for the crafting bench UI.
- Drag-drop or hotbar-style interaction (not specified in corpus — pick what fits X3Native UI conventions).

---

## 7. Shader / Particle / VFX Requirements

> Cross-reference with shipped X3Native shaders (PBR `mesh.frag` w/ normals + GGX, glass material, holo-terminal — all present per Tim's "current baseline" note). This section is **gap-only**.

### 7.1 InfectionSpread (NEW — required)

**Source spec:** TASK_8 Part 1 lines 243–415.

Features the X3Native PBR shader does NOT do:
1. **Per-vertex infection mask** seeded from origin point (3D distance + FBM noise offset), animated `_InfectionProgress` 0–1.
2. **5-stage progressive lerp** — vein color, skin discolor, chitin pattern, chitin displacement, final alien.
3. **Vertex displacement** along normal scaled by FBM noise + chitin factor (Stage 3+) — needs vertex-stage modification, not just frag.
4. **Vein pulse** = `sin(time * pulse_speed + worldPos.y * 10)`, drives veinColor emissive.
5. **Sampled vein-mask texture** + `step(thickness, veinMask)` for vein routing.

Implementation in X3Native:
- Add a new fragment+vertex pipeline variant: `mesh_infection.{vert,frag}`.
- Reuse bindless texture slots for `_InfectionTex`, `_ChitinTex`, `_VeinMask`.
- Push constants for `_InfectionProgress`, `_InfectionOrigin (vec3)`, stage thresholds.
- Per-actor descriptor set holding the infection state — written by C++ Infection component each tick.

### 7.2 BodyHorror (NEW — required for transformed enemies)

**Source:** TASK_8 Part 1 lines 533–633.

Distinct from InfectionSpread — used post-transformation for enemy variants:
- Vertex displacement via FBM "mass growth" along normal × per-vertex blue color channel.
- Flesh-tear mask using vertex-color red channel + FBM step.
- Blood rim color on tear edges.

Lower priority than InfectionSpread (only needed for Stage-5+ enemies; can fall back to opaque chitin material as a placeholder).

### 7.3 WireframeDrone (NEW — required for all drones)

**Source:** TASK_8 Part 1 lines 13–230. Highest-priority new shader after Infection.

Features:
- Geometry-shader-based barycentric wireframe (or compute equivalent on Vulkan if geometry-shader avoidance is desired — geometry shaders are slow on AMD/NVIDIA both).
- Per-state color (5 colors: default, hacked, alert, disabled, overloading).
- Scanline = `step(0.5, frac(worldPos.y * count - time * speed))`.
- Fresnel rim + per-drone pulse.
- Glitch displacement on vertex.x when `_GlitchIntensity > 0`.
- Damage flicker when `_DamageAmount > 0`.

**Tim's drone colors:**
- Surveillance: blue (#00AAFF)
- Combat: red (#FF3333)
- Medical: green (#33FF66)
- Breeder: orange (#FF9933)
- Sarah's hacker drone: cyan (#00FFCC)

Implementation note: prefer **mesh shader** or **vertex-encoded barycentrics** over geometry shader on Vulkan. The geometry-shader fallback is fine for development but should be replaced before ship.

### 7.4 CyberneticBlend material parameter (NEW — required for Bio-Integration Lab feedback)

**Source:** AugmentationSystem.cs line 228 (`material.SetFloat("_CyberneticBlend", ...)`), HumanityMeter.cs line 577.

Needed: the player's body material must expose a `_CyberneticBlend` float (0–1). At 0 = pure flesh; at 1 = mostly chrome/circuit. This is **net-new** to the X3Native PBR shader and the simplest way to add it is:

- Add a `cybernetic_overlay` slot in the bindless descriptor — a tileable chrome/circuit albedo + normal pair.
- In `mesh.frag`, blend `final_albedo = lerp(skin_albedo, cyber_albedo, _CyberneticBlend)` and similarly for normal/metallic/roughness.
- Drive from a per-actor uniform.

This is the **single most important new shader feature** for the Bio-Integration Lab to read as gameplay-meaningful.

### 7.5 AlienTech / Salvari emissive (NEW — required for Floor 6 and Salvari weapons/drones)

**Source:** TASK_8 Part 1 lines 419–529.

Features:
- Animated glyph map (UV scroll).
- Energy-flow texture (UV scroll, different speed).
- Crystal refraction (fresnel-driven inner-color glow).
- Power level multiplier (0 to 1).

Lower priority — only matters from Act 3 onwards.

### 7.6 EnergyShield (NEW — required for boss fights)

**Source:** Part 1 lines 637–724.

Features:
- Hex pattern UV-scrolled.
- Impact ripple emanating from `_ImpactPoint` over time.
- Fresnel rim + pulse animation.

Used by: F4.5 Chorus boss segments, Salvari elite enemies, Sarah's combat ability (later acts).

### 7.7 BloodSplatter decal (NEW — required for combat feedback)

**Source:** Part 1 lines 813–881.

Features:
- Wet/dry lerp (`_Age` 0→1).
- Specular highlight scaled by `(1 − Age) * _Wetness`.
- Standard decal projection.

X3Native needs a decal-projector system — confirm if one exists; if not, this is a system-level feature, not just a shader.

### 7.8 HolographicUI (ALREADY SHIPPED — verify)

**Source:** Part 1 lines 885–968.

Tim's notes say "holo terminals + glass material" already exist. Verify the existing X3Native holo material has:
- Scanlines (`_ScanlineCount`, `_ScanlineIntensity`)
- Edge glow / fresnel
- Random noise overlay
- Flicker (rare hash-based dimming)

If yes, no work. If not, extend existing.

### 7.9 PortalEffect (NEW — required for Act-3+ travel)

**Source:** Part 1 lines 728–809. Used for dimensional portals in the late game. Low priority until Floor 6 / Act 2 transitions are designed.

### 7.10 CrystalFormation (NEW — required for Floor 6 / Salvari)

**Source:** Part 1 lines 1083–1175. Internal glow + growth animation. Used for alien crystals in Floor 6 and beyond.

### 7.11 SpaceSkybox (NEW — required for Act 2)

**Source:** Part 1 lines 974–1077. Procedural stars + nebula + sun + Earth. Toggles for armada display. Required when the Floor-4 observation-window beat happens (§2.5).

### 7.12 Particle systems (gameplay-driven, NEW)

- **Augmentation install** — sparks + electrical arc particles, plays in the Bio-Integration Lab on station use.
- **Humanity gain/loss** — particle stream up/down from player, color-coded by direction.
- **Infection emissive vein pulse** — particle-system per-actor that pulses on infected NPCs.
- **Cure application** — green wash + healing motes.
- **Drone disable / hack-success** — electrical surge sparking burst.

These are gameplay-tied, not shader-shader. Need a particle system that supports world-space + screen-space + actor-attached emitters.

---

## 8. Implementation Gaps vs Current X3Native Engine

### 8.1 Already shipped (do not duplicate)

Per Tim's note:
- 7-floor spire (level1 → spire_mid → spire_top)
- F2 rescue system (`rescue.{h,cpp}`)
- Floor-4.5 Nexus / Chorus (`spire_nexus.{h,cpp}`)
- F7 sub-levels (`spire_sublevels.{h,cpp}`)
- Companion controller (`companion_controller.{h,cpp}`)
- Holo terminals + glass material
- PBR shading (mesh.frag w/ normal + GGX)
- Vulkan render device with bindless textures + glass pass

### 8.2 Gap matrix — what's missing

| Need | Where it goes (proposed) | Source spec | Priority |
|------|--------------------------|-------------|----------|
| `Humanity` singleton (C++ class) | `humanity.{h,cpp}` (engine root or `gameplay/`) | HumanityMeter.cs lines 17–752 | **P0** — needed for Bio-Integration Lab |
| `AugmentationSystem` (C++ class) | `augmentation.{h,cpp}` | AugmentationSystem.cs lines 15–531 | **P0** |
| `AugmentationStation` (room component) | extend existing room actor system | AugmentationSystem.cs lines 463–530 | **P0** |
| Bio-Integration Lab room (geometry, lighting, audio) | new room module `spire_f4_aug_bay.{h,cpp}` | F4_CyberneticsWorkshop_Layouts.md lines 10–194 | **P0** |
| Surgical robot arm AI (ceiling-mounted) | `npc_surgical_arm.{h,cpp}` | F4 layout lines 69–74 | **P1** |
| Observation-window armada trigger | level script in Bio-Integration room | F4 layout lines 75–80 | **P1** |
| `Infection` component (per-actor) | `infection.{h,cpp}` (extend `rescue.{h,cpp}` for the per-NPC variant; new class for player-infection) | RescueVictim.cs + InfectionSpread shader spec | **P0** |
| `Inventory` service | `inventory.{h,cpp}` | TASK_12 implied surface | **P0** |
| Crafting recipe DB (data-driven) | `data/recipes.json` + `crafting.{h,cpp}` | TASK_12 full table | **P1** |
| Cure-application matrix (data-driven) | `data/cures.json` | §4.3.2 above | **P1** |
| Credits as currency | extend Inventory or separate `wallet.{h,cpp}` | TASK_11 line 204 | **P1** |
| Drone-summon system (player-side) | `drone_summon.{h,cpp}` | TASK_12 line 719 | **P2** |
| Drone-hack system (Sarah-side) | `drone_hack.{h,cpp}` (extend companion_controller?) | TASK_11 line 178 | **P2** |
| `InfectionSpread.shader` (Vulkan port) | new pipeline variant | TASK_8 Part 1 lines 243–415 | **P0** |
| `WireframeDrone.shader` (Vulkan port, no geometry shader) | new pipeline variant | Part 1 lines 13–230 | **P1** |
| `_CyberneticBlend` material slot in PBR | extend `mesh.frag` | this doc §7.4 | **P0** |
| `BodyHorror.shader` (Vulkan port) | new pipeline variant | Part 1 lines 533–633 | **P2** |
| `AlienTech.shader` (Vulkan port) | new pipeline variant | Part 1 lines 419–529 | **P2** |
| `EnergyShield.shader` (Vulkan port) | new pipeline variant | Part 1 lines 637–724 | **P2** |
| `BloodSplatter` decal system | new decal projector | Part 1 lines 813–881 + need projector framework | **P2** |
| Particle system (actor-attached) | `particles.{h,cpp}` (if not already present) | this doc §7.12 | **P1** |
| Vignette overlay (full-screen) | post-process effect, single uniform | HumanityMeter.cs lines 581–597 | **P1** |
| Object pool (engine-level) | `object_pool.{h,cpp}` — but X3Native likely has its own allocator | TASK_8 part 2 lines 6–453 | **P3** (engine-internal, may already exist) |
| LOD manager | `lod_manager.{h,cpp}` | TASK_8 part 2 lines 460–769 | **P3** (engine-internal) |
| Level streaming | `streaming.{h,cpp}` | TASK_8 part 2 indirectly | **P3** |
| Performance overlay (F3 toggle) | `perf_hud.{h,cpp}` | TASK_8 part 2 lines 776–1107 | **P2** (nice-to-have for dev) |
| Save/load schema for: humanity, augs, inventory, infection, drone state, credits | extend existing save system | per-section save structs above | **P0** (when any P0 system lands) |
| `GameFlags` / `GameStats` global key-value store | `game_flags.{h,cpp}` | referenced by AugmentationSystem.cs lines 322–328 | **P1** |
| `DialogueManager` runtime | `dialogue.{h,cpp}` (likely exists in some form for companion lines) | referenced widely | **P1** (verify what's already there) |

### 8.3 What's IN TASK_8 but not relevant to X3Native

- Unity HDRP-specific shader includes (`Packages/com.unity.render-pipelines.high-definition/...`) — port concepts only, ignore code.
- C# `MonoBehaviour` / `Coroutine` patterns — translate to C++ tick-based update loops.
- Unity Editor scripts (`AssetPostprocessor`, menu items) — irrelevant; X3Native has its own asset pipeline.
- Stable Diffusion automation in Python — useful as an offline asset workflow but not engine code. (See `EFLZ_ASSET_SOURCES` memory note.)
- Steam/GOG/Epic deployment configs — premature; not relevant until release.
- NUnit test framework (C#) — X3Native should use its own native test framework (likely GoogleTest or a custom one). The *test cases* in TASK_8 part 2 lines 2049–2470 ARE useful as design specs (e.g., "Sarah rescue timer determines correct outcome at 540s / 480s / 300s / 60s remaining" — line 2243).

### 8.4 Reuse opportunities

- The existing `rescue.{h,cpp}` (F2 rescue) is the closest template for the per-NPC Infection plumbing. The Station-1 victim in the Bio-Integration Lab can almost certainly use `rescue.{h,cpp}` as-is with a different timer constant and a different transform-target (Augmented_Guard enemy spawn instead of full-alien spawn).
- The holo-terminal material already shipped is what the central control console in the lab uses — no new shader needed for that interactable.
- The companion controller (Sarah) is where the drone-hack ability should live (extend, don't fork).
- The spire room layout system that already loads F2, F4.5, F7 is what the Bio-Integration Lab plugs into — add `spire_f4_aug_bay.{h,cpp}` alongside the others.

---

## 9. Recommended Build Order

This is the path I recommend to maximize Tim's "wired up" win:

### 9.1 Sprint A — Bio-Integration Lab MVP (P0)

**Goal:** the room exists, the player can walk in, walk to Station 3, install one strength augmentation, see their humanity drop, see the cybernetic blend appear on their body. Single end-to-end feedback loop.

1. **`humanity.{h,cpp}`** — singleton, 0–100 range, state buckets, ending thresholds, save/load. Pure data + events; no UI yet.
2. **Add `_CyberneticBlend` to `mesh.frag`** — single uniform on the player material that lerps to a chrome/circuit overlay. Test by manually setting from 0→1 in dev console.
3. **`augmentation.{h,cpp}`** — AugmentationData payload, install sequence, ties to humanity.
4. **`spire_f4_aug_bay.{h,cpp}`** — minimal room geometry (place 6 station markers, central console, observation window trigger volume). Reuse existing room infrastructure.
5. **`AugmentationStation` interactable** — only Station 3 needs to work for MVP (Strength +15%, 10% humanity cost). Other 5 can be cosmetic placeholders.
6. **Humanity UI** — fill bar + percent + state text. Reuse holo-terminal aesthetic.
7. **Save extensions** for humanity + applied augs list.

Acceptance: player walks in, presses E on Station 3, plays the 2-second install animation, humanity drops from 100→90, player material visibly shifts slightly toward chrome, save+reload preserves state.

### 9.2 Sprint B — Bio-Integration Lab story beats (P1)

1. **Station 1 rescue** — extend `rescue.{h,cpp}` with a 30-s variant, surgical-arm hostility, success spawns NPC ally (ally bookkeeping for later).
2. **Surgical robot arms (ceiling)** — `npc_surgical_arm.{h,cpp}`. Dormant by default; on alarm, become hostile flying enemies (200 HP each). Reuse existing flying-enemy AI if possible.
3. **Central console** — interactable; without Sarah, hacking attempts trigger alarm; with Sarah, opens "volunteer list" UI + disables arms.
4. **Observation window trigger** — fires armada-reveal cinematic moment. Spawn Heavy_Cyborg through ceiling break (VFX + audio sting).
5. **Stations 2, 4, 5, 6** wired up — Station 2 lootable (drops Drone Parts → Inventory), Station 4 mercy-kill choice (+humanity), Station 5 cosmetic, Station 6 Jake-only premium aug.
6. **Audio bed** for the room (12 cues per §2.8).
7. **`SpaceSkybox` shader** + Earth/armada toggles for the observation window beat.

### 9.3 Sprint C — Infection + Cure plumbing (P0 because gameplay-blocking elsewhere)

1. **`infection.{h,cpp}`** — Infection component, 5-stage progression, `OnStageChanged` event.
2. **`InfectionSpread.shader`** — Vulkan port, 5-stage visual.
3. **Update `rescue.{h,cpp}`** — feed Infection component instead of internal `infectionLevel` (consolidate).
4. **`Inventory` service** — minimal: add/remove/count/save.
5. **`crafting.{h,cpp}` + recipe JSON** — Antidote + Full Cure + Cure Prototype recipes.
6. **Cure application matrix (data JSON)** — `Infection::ApplyCure(cureType)` consults it.
7. **L7 Sarah rescue outcome bands** — wire the 540s/480s/300s/60s thresholds to save flags.

### 9.4 Sprint D — Drone economy (P1/P2)

1. **Credits as a tracked resource** in Inventory or separate Wallet.
2. **Drone-kill loot tables** — drones drop scrap_metal + circuit_board + power_cell on death.
3. **Reject Bin lootable** in Bio-Integration Lab — drops Drone Parts.
4. **`WireframeDrone.shader`** — Vulkan port, per-drone color, state colors.
5. **Sarah's `drone_hack` ability** — extend companion_controller. 5-drone cap. One-shot Master Hack.
6. **Player drone-summon** (Salvari Combat Drone craftable) — Sprint D2 if needed.

### 9.5 Sprint E — Visual polish (P2)

1. **`BodyHorror.shader`** for transformed enemies.
2. **`EnergyShield.shader`** for bosses.
3. **`AlienTech.shader`** for Salvari content.
4. **Vignette post-process** for low-humanity feedback.
5. **Blood splatter decals** if decal system exists.

### 9.6 Cross-cutting concerns (do continuously)

- **GameFlags / GameStats** key-value store — small service, builds up as systems land.
- **DialogueManager** — verify what's there; extend for Jake/Sarah state-transition lines.
- **Performance HUD** (`perf_hud.{h,cpp}`) — useful for dev throughout, ship-toggleable F3 overlay.
- **Save schema extensions** — every new persistent system extends the save format. Bump version on each.

### 9.7 What NOT to do yet

- Don't port the Object Pool, LOD Manager, Level Streaming, Performance Profiler from TASK_8 part 2. X3Native's render/sim already handles those at a lower level. These were Unity-engine-specific concerns and X3Native has equivalents already.
- Don't port the Python Stable Diffusion pipeline as engine code — it's offline asset production, not runtime.
- Don't port the Unity Editor asset-postprocessor — irrelevant to a native engine.
- Don't port the NUnit test cases as-is — translate the *behavioral assertions* into X3Native's test framework, but don't try to mimic Unity Test Framework.
- Don't write build/deployment automation for Steam/GOG/Epic yet. Way premature.

---

## 10. Open Questions for Tim

These came up during synthesis and need a one-line decision:

1. **Single Inventory or split (Jake/Sarah)?** TASK_8 QA line 2603 ("Load restores inventory") + Sarah's distinct skill tree implies *shared* inventory, but worth confirming.
2. **Drone "repair/recharge" mechanics** — corpus has a charging station environment art but no gameplay loop. Recommendation in §5.5 is 1-credit/HP at a Floor-5 station; needs Tim's sign-off.
3. **`AugmentationType.Hacking` and `.Regeneration`** are in the enum but not assigned to any station. Are these (a) Sarah-only augs you install at the F5 Drone Station, (b) Floor-6 Salvari augs, or (c) reserved for DLC?
4. **Bio-Integration Lab name** — should the X3Native room file be `spire_f4_aug_bay.{h,cpp}` (match corpus) or `bio_integration_lab.{h,cpp}` (match Tim's verbal branding)? My recommendation: file = `spire_f4_aug_bay`, display name in-game = "Bio-Integration Enhancement Lab" or similar — keeps engine grep-friendly while honoring narrative branding.
5. **`_CyberneticBlend` shader feature** — should this live in the base `mesh.frag` (every actor pays the cost of an extra texture fetch even if blend=0) or as a material variant? Recommendation: material variant, only the player body uses it.
6. **Particle system** — does X3Native have one already? The corpus assumes one exists. If not, Sprint A needs a P0 particle service before the augmentation install effect works.
7. **Vignette / post-process** — is there a post-process pipeline in X3Native or only the forward pass? Vignette needs a screen-space pass.
8. **Decal projection** — same question for blood splatter and any future floor-decals.

---

## Appendix A — Source line citations cheat-sheet

For every claim in this doc, the canonical source:

- AugmentationSystem behaviour: `Floors3-4_Complete\Scripts\Floor4\AugmentationSystem.cs:1-531`
- HumanityMeter behaviour: `Floors3-4_Complete\Scripts\Floor4\HumanityMeter.cs:1-752`
- Bio-Integration Lab room layout: `Floors3-4_Complete\RoomLayouts\Floor4_CyberneticsWorkshop_Layouts.md:1-194`
- InfectionSpread shader: `TASK_8_TECHNICAL_SYSTEMS_PART1_SHADERS.md:243-415`
- WireframeDrone shader: `TASK_8_TECHNICAL_SYSTEMS_PART1_SHADERS.md:13-230`
- BodyHorror shader: `TASK_8_TECHNICAL_SYSTEMS_PART1_SHADERS.md:533-633`
- AlienTech shader: `TASK_8_TECHNICAL_SYSTEMS_PART1_SHADERS.md:419-529`
- EnergyShield shader: `TASK_8_TECHNICAL_SYSTEMS_PART1_SHADERS.md:637-724`
- PortalEffect shader: `TASK_8_TECHNICAL_SYSTEMS_PART1_SHADERS.md:728-809`
- BloodSplatter shader: `TASK_8_TECHNICAL_SYSTEMS_PART1_SHADERS.md:813-881`
- HolographicUI shader: `TASK_8_TECHNICAL_SYSTEMS_PART1_SHADERS.md:885-968`
- SpaceSkybox shader: `TASK_8_TECHNICAL_SYSTEMS_PART1_SHADERS.md:974-1077`
- CrystalFormation shader: `TASK_8_TECHNICAL_SYSTEMS_PART1_SHADERS.md:1083-1175`
- RescueVictim infection: `Task9_Extracted\RescueVictim.cs:30-50, 256`
- Cure recipes + costs: `TASK_12_EscapeLab48_COMPLETE_CRAFTING_SYSTEM.md:79, 596-597, 695, 700`
- Drone materials: `TASK_12_EscapeLab48_COMPLETE_CRAFTING_SYSTEM.md:64-96, 719`
- Skill tree drone-hack: `TASK_11_SKILL_TREES_PART4_ECONOMY_IMPLEMENTATION.md:178, 191`
- Credits cost (respec): `TASK_11_SKILL_TREES_PART4_ECONOMY_IMPLEMENTATION.md:204-210`
- L7 Sarah rescue timer bands: `TASK_8_TECHNICAL_SYSTEMS.md:2517-2526`
- Inventory implied interface: `TASK_12_EscapeLab48_COMPLETE_CRAFTING_SYSTEM.md:811, 825, 855`
- QA inventory persistence: `TASK_8_TECHNICAL_SYSTEMS_PART2.md:2603`
- Sarah max-5 hacked drones: `TASK_8_TECHNICAL_SYSTEMS.md:2550`
- F5 Master Hack story beat: `TASK_8_TECHNICAL_SYSTEMS.md:2551`

---

*End of EFLZ_TECH_SYSTEMS.md.*
