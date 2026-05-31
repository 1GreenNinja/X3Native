# EFLZ — Crafting + Inventory Spec
> Synthesized 2026-05-31 from TASK_12 (Crafting, 55 KB) + bible references.
> Authority: Tim's design corpus. **No code changes; design doc only.**

---

## 1. Overview + Sources

### 1.1 What this document is

A native-engine design port of the EFLZ ("Escape From Lab Zero" / "Escape Lab 48")
crafting and inventory systems, derived from Tim's own clean-room IP. Nothing in
here references RBDOOM/idTech/Doom/Quake/Unreal source. The system covers:

- A **player inventory** (slot model, weight, categories, UI surfaces).
- **150 recipes** spanning melee, ranged, armor, consumables, ammo, weapon
  upgrades, ship parts, quest items, and Salvari technology.
- **5 crafting stations** distributed across acts/floors with first-available
  gates.
- The **cure-synthesis quest** (Jake DNA + Salvari base + 3-biome xenoflora +
  ancient-temple purification).
- **Weapon mods** and **drone-upgrade integration**.
- **Consumables** (health, candy bars / rations, stims, grenades) that the wife's
  gas-station-restocking notes require.
- **Hybrid weapons** (Nanite Swarm, Bio-Cannon, Mind Shredder) and **Salvari
  tech weapons** (Lightning Gun, Nullifier, Dimensional Rifle).

### 1.2 Primary sources

| Source | Path | Used for |
|--------|------|----------|
| TASK_12 Crafting | `G:\Unity_Projects\PARALLEL_TASKS_8_CHATS\TASK_12_EscapeLab48_COMPLETE_CRAFTING_SYSTEM.md` | 150-recipe catalog, 32 materials, 5 stations, 5 skill levels, station/level gates, stats, craft times, sell values |
| EFLZ Narrative Bible | `G:\Unity_Projects\# 🎮 ESCAPE FROM LAB ZERO - ULTIMATE Narrative Design.txt` (1143 lines) | Cure quest components (lines 626-633), floor weapon unlocks (lines 982-996), hybrid weapons (lines 993-996), Salvari weapons (lines 988-991), Sarah/Jake/Salvari ally framework |
| EFLZ Master Plan | `G:\X3Native\docs\design\EFLZ_MASTER_PLAN.md` | Act/floor structure, integration with shipped X3Native systems |
| EFLZ World Structure | `G:\X3Native\docs\design\EFLZ_WORLD_STRUCTURE.md` | Floor list, biome layout for xenoflora sourcing |
| Bestiary | `G:\X3Native\docs\design\EFLZ_BESTIARY.md` | Material drop sources (drones, infected, hybrids, Overlord, Chorus) |

### 1.3 What's already shipped (treat as constraints)

| System | File(s) | Status |
|--------|---------|--------|
| Weapons | `G:\X3Native\app\weapon.h`, `weapon.cpp` | Pickup + first-person viewmodel for a single energy-pistol GLB. NO ammo, NO mag, NO mod slots. Re-use the pickup/arming flow as the seed for crafted-weapon equipping. |
| Player HP | `G:\X3Native\app\player.h` (m_hp/m_maxHp at lines 197-198) | HP, iframes, death/respawn. NO healing items wired in. |
| Health drops | scattered level cpps; ad-hoc | Pickup-able health crystals/orbs in a few rooms. NOT formalized as inventory consumables. |
| Drone shop credits | referenced in `companion_squad.cpp`, design only | Mentions a credits/shop economy in drone-upgrade design docs. NOT implemented. |
| Inventory UI | none | Does NOT exist. HUD only shows HP + weapon-state. |
| Crafting | none | Does NOT exist. |
| Recipe registry | none | Does NOT exist. |

The spec below is intentionally additive: new engine modules (`inventory`,
`crafting`, `recipe_db`, `consumables`, `weapon_mods`) plug in alongside the
existing `weapon.h` viewmodel and `player.h` HP, without rewriting either.

### 1.4 Design intent

- **Diegetic, lightweight.** EFLZ is a co-op action shooter, not Tarkov. The
  inventory is grid-lite with category tabs, not a Tetris simulator.
- **Survival-tinged.** The wife's gas-station note (player can stock up on
  candy bars / sodas / first-aid) means the inventory MUST hold consumables in
  meaningful stacks (5-25), not single-shot use items.
- **Crafting gates story.** Salvari Forge unlocks only after the Salvari
  alliance choice; Cure synthesis is its own multi-act quest; hybrid weapons
  cost humanity. Crafting is woven into the narrative, not parallel to it.
- **Companions help.** Per `companion-coop-roadmap.md`, allies/companions can
  scavenge/return materials and pre-stock the workbench during downtime — see
  Section 8.6.

---

## 2. Inventory Model

### 2.1 Top-level data model

```
InventorySlot {
    ItemId    itemId        // FK -> ItemDb; empty slot has Invalid
    uint16_t  count         // current stack size
    uint16_t  durabilityCur // for weapons/armor; 0 for stackables
    uint16_t  modMask       // bitfield -> WeaponMod slots populated
    uint8_t   tag           // hotbar position, locked-by-quest, etc.
}

Inventory {
    PlayerId               owner
    InventorySlot[CAP]     slots           // CAP = 40 (see 2.3)
    uint32_t               credits         // currency (drone-shop / vendors)
    uint32_t               carriedWeight   // computed; cached for HUD
    uint32_t               maxWeight       // see 2.4
    Hotbar                 hotbar          // see 2.5
    Stash                  stash           // shared per-act locker, optional
}
```

`ItemId` is a stable handle into the **ItemDb** ScriptableObject-equivalent
registry. Each entry carries:

```
ItemDef {
    ItemId     id
    string     displayName
    string     iconPath
    Category   category        // see 2.2
    Rarity     rarity          // Common .. Legendary
    uint16_t   maxStack
    uint16_t   maxDurability   // 0 = non-durable (stackable)
    uint16_t   weightGrams     // see 2.4
    int32_t    sellValue       // credits (shop / vendor)
    ItemFlags  flags           // QuestLocked, NoDrop, NoSell, Consumable, etc.
    StatBlock  stats           // damage/armor/effect; for weapons/armor
    EffectId   onUse           // for consumables -> EffectDb entry
    AmmoType   ammoType        // for ranged weapons + ammo
}
```

### 2.2 Categories (six top tabs in the UI)

The TASK_12 doc enumerates 9 craftable categories; the player-facing inventory
folds them into **6 tabs** that match the way players actually think about
things:

| Tab | Source categories (TASK_12) | Example items |
|-----|-----------------------------|---------------|
| **Weapons** | Melee Weapons, Ranged Weapons | Combat Knife, Service Pistol, Lightning Gun, Salvari Crystal Sword |
| **Armor** | Armor & Equipment | Tactical Combat Armor, Hazmat Suit, Salvari Bio-Armor |
| **Consumables** | Consumables, Salvari Tech (med variants) | Medkit, Combat Stim, Candy Bar, Salvari Healing Crystal |
| **Ammo + Mods** | Ammunition, Weapon Upgrades | Rifle Rounds, Damage Enhancer Mk2, Suppressor, Salvari Crystal Enhancement |
| **Materials** | Mechanical, Chemical, Biological, Energy, Salvari, Special | Scrap Metal, Bio-Crystal, Hybrid Gland, Echo Fragment |
| **Quest / Keys** | Quest Items, key cards, story items | Security Keycard, Cure Prototype, Mothership Virus, Aria's Tracking Device |

Tabs do NOT change the slot count — they're just filters over the same 40 slots.

### 2.3 Slot count and grid

- **40 slots base.** A practical number for an Act-1 / Act-2 player who's
  carrying 3-4 weapons, 1 armor, 6-10 stacks of materials, 4-6 consumables, and
  a handful of quest items.
- **Up to 56 slots** with the *Engineering Work Suit* (recipe #47) equipped
  (+16 slots) or the *Cargo Bay Expansion* on the player rig (+10 slots).
- **Stash** (per-act locker) — unlocked at first safe room each act. Shared
  across the act. NOT carried into combat; pulls and deposits at any
  workbench. 120 slots; no weight cap.

### 2.4 Weight system

Weight is informational, not punitive — over-cap = slower sprint / no sprint,
NOT "you cannot pick this up." This keeps the gas-station candy-bar-grab feel
intact.

| Tier | Weight (grams) | Effect |
|------|----------------|--------|
| `<= maxWeight * 0.80` | normal | full sprint, full stamina |
| `0.80 - 1.00` | encumbered | -10% sprint, -20% stamina regen |
| `> 1.00 - 1.30` | overburdened | no sprint, -50% stamina regen, HUD warning |
| `> 1.30` | hard cap | cannot pick up new items (the only hard wall) |

Base `maxWeight = 60_000 g` (60 kg). Augments: Powered Exoskeleton +30 kg,
Salvari Bio-Armor +15 kg.

Material weights are intentionally low (Scrap Metal: 250 g; Bio-Crystal: 50 g;
Quantum Core: 1000 g) so the typical material backpack costs ~10-15 kg. Big
items (Chain Gun, Rocket Launcher) are the costly ones (8-12 kg each).

### 2.5 Hotbar

8-slot wheel (D-pad / mouse-wheel cycling), assignable from any inventory tab.
Holds:

- Up to **2 primary weapons** (slots 1-2)
- Up to **2 secondary / heavy weapons** (slots 3-4)
- Up to **4 consumables / grenades** (slots 5-8)

Quick-use bindings: `Q` cycles hotbar; `1-8` direct slot; `F` use selected
consumable. Throwable consumables (grenades, decoys) hold-to-aim, release to
throw.

### 2.6 UI surfaces

Three distinct surfaces, all driven by the same `Inventory` model:

1. **HUD strip** (always-on, lower-right). Active weapon icon + ammo, active
   consumable + count, credits, encumbrance bar. Roughly 220x140 px @ 1080p.
2. **Inventory screen** (TAB key). Full-screen, 6-tab category bar, 40-slot
   grid (8x5), right-pane item detail with stats / on-use effects / sell button.
   Drag-to-hotbar, drag-to-stash, drag-to-drop. Sort buttons: by category,
   rarity, weight, recent.
3. **Crafting screen** (overloaded on Inventory while standing at a Station).
   Adds a left-pane recipe list filtered by station + skill level + unlocked
   set. Right-pane shows ingredients (green check / red X), craft time, output
   preview, and a single big "Craft" button. Holding the button starts the
   craft; the craft progresses in real time (Section 3.3) and can be cancelled
   for full refund.

### 2.7 Pickups -> inventory flow

The existing `WeaponSystem` (in `weapon.h`) uses a distance test on the
player's feet to arm the pickup. The new flow generalises that:

- Any world entity tagged `Tag::Pickup` carries a `PickupComponent { ItemId,
  count, durability }`.
- On entering `kPickupRadius` (or pressing `E` if `PickupComponent.requiresInteract`),
  `Inventory::tryAdd(id, count)` is called. If it succeeds, the pickup is
  consumed; if the inventory is full (or weight-capped), a "INVENTORY FULL" hint
  appears and the pickup stays.
- Existing health-drop entities scattered in `level*.cpp` migrate to the
  pickup component pointing at the `medkit_basic` ItemId.

---

## 3. Crafting Stations

### 3.1 Station table

From TASK_12 §"Crafting Stations" + bible cross-references.

| Station | First Available | Categories crafted | Speed | Visual / placement |
|---------|-----------------|--------------------|-------|--------------------|
| **Workbench** | Floor 1 (Act 1) | Melee, Ranged, Ammo, Upgrades, basic Quest items | 1.0x | Garage / maintenance room. Standing-height bench + vice. |
| **Chemistry Lab** | Floor 2 (Act 1) | Consumables, chemical weapons, antidotes | 1.0x | Fume hood + glassware + centrifuge. Found in research wings. |
| **Med Bay** | Floor 2 (Act 1) | Medical consumables, cures, trauma kits | 0.9x (faster) | Hospital bay; biohazard signage. |
| **Salvari Forge** | Level 9+ (Act 2 deep) | Salvari Tech, hybrid weapons, crystal equipment | 0.8x (fastest) | Bio-crystal pillar + suspended forge plate. Requires Salvari alliance. |
| **Ship Workshop** | Level 8+ (Act 2 / Act 3 prep) | Ship parts, vehicle upgrades | 1.2x (slower) | Aboard the Prometheus and partner ships. Hangar bay station. |

### 3.2 Station placements per floor / act

The 50-level structure (see `EFLZ_WORLD_STRUCTURE.md`) gates stations like so:

| Act | Levels | Stations introduced | Notes |
|-----|--------|---------------------|-------|
| **Act 1 — Underground Lab** | F1-F10 | F1: Workbench (intro level garage). F2: Chemistry Lab + Med Bay (research wing on Floor 2). F4-F6: secondary Workbenches in armory + maintenance. F8: optional Workbench in security barracks. | All Act 1 crafting uses Floors 1-3 materials. Skill cap = level 3 by F10. |
| **Act 2 — Surface + Salvari Contact** | L11-L20 | L13: first Salvari Forge (in K'thara's refugee camp, only if alliance chosen). L15-L20: forward Workbench in valley cabin, Chem Lab in cave research outpost. | Salvari Forge ONLY exists if `flags.alliance == Salvari`. Hostile branch locks all 18 Salvari Tech recipes. |
| **Act 3 — Space Journey** | L21-L35 | L22: Prometheus ship Workshop (after commandeering). L25: salvaged Salvari Forge moved aboard. Each major hub (mining colony, Fortune's End, rebel base) carries a Workshop + Chem Lab. | Ship Workshop crafts upgrades that mount to the player ship, not the player. |
| **Act 4 — Earth's Last Stand** | L36-L50 | L37: forward-operating-base Workbench. L40: combined Med Bay + Chem Lab in rebel encampment. L45: Salvari Forge re-deployed in alien fleet command. L48-L50: no new stations (climax run). | Crafting tapers off in late Act 4 — the climax run is a load-out commitment. |

### 3.3 Craft time + progression mechanic

- Each recipe has a `craft_time` (10-200 seconds). Standing at the station,
  click Craft -> a progress bar fills in **real time** (not paused).
- The player can leave the station — the craft continues in the background up
  to the recipe's `craft_time + 30s` grace window before requiring re-confirm.
- Skill bonus: each crafting skill level (1-5) shaves 5% off craft time
  (max -20% at level 5).
- Companion accelerator: if a companion with the `Engineer` tag is in the
  party AND idle, they reduce craft time another -15%.

### 3.4 Skill level gates

Direct from TASK_12 system_overview:

| Level | Requirement | Recipes unlocked (cumulative) |
|-------|-------------|-------------------------------|
| 1 | Available from start | Recipes flagged `levelRequired:1` (~30 recipes) |
| 2 | Floor 3+ OR 50 items crafted | levelRequired:2 (~30 more) |
| 3 | Floor 5+ OR 150 items crafted | levelRequired:3 (~30 more) |
| 4 | Act 2+ OR 300 items crafted | levelRequired:4 (~30 more) |
| 5 | Act 3+ AND Salvari contact (alliance OR captured) | levelRequired:5 (final ~30, includes Salvari + endgame) |

Items-crafted counter advances on every successful craft, so a player who
grinds materials early can outpace floor progression. This is intentional —
crafting is a real progression axis.

### 3.5 Character-restricted recipes

Two recipes are character-restricted in TASK_12:

| Recipe | Character | Rationale |
|--------|-----------|-----------|
| #7 Stun Knuckles | Jake | Scales with Jake's 400% strength (canon bible stat). |
| #34 Neural Override Pistol | Sarah | Sarah's hacking is enhanced (canon bible: Sarah = best pilot/hacker). |

Other recipes are character-agnostic.

---

## 4. Recipe Catalog (full table)

Listed by category in TASK_12 order. Each row: # | Name | Level gate | Station |
Inputs (canonical material IDs from TASK_12 §Resource Types) | Output | Notes /
gate.

> **Format:** `Material x N` repeated, comma-separated. Acronyms used to save
> space: `SM`=Scrap Metal, `CB`=Circuit Board, `PP`=Precision Parts,
> `AP`=Advanced Processor, `QC`=Quantum Core, `NC`=Nano Components,
> `CC`=Chemical Compound, `MS`=Medical Supplies, `TR`=Toxic Residue,
> `PC`=Purified Compound, `CurC`=Cure Component, `EC`=Explosive Compound,
> `OT`=Organic Tissue, `IT`=Infected Tissue, `HG`=Hybrid Gland,
> `OS`=Overlord Sample, `NT`=Neural Tissue, `PowC`=Power Cell, `Pla`=Plasma
> Canister, `FC`=Fusion Core, `CE`=Crystal Energy, `UE`=Unstable Energy,
> `SA`=Salvari Alloy, `BC`=Bio-Crystal, `MC`=Memory Crystal, `QS`=Quantum Silk,
> `RG`=Resonance Gem, `AA`=Alien Artifact, `PS`=Prototype Schematic,
> `EF`=Echo Fragment, `OC`=Overlord Cipher, `HE`=Humanity Essence.

### 4.1 Melee (Recipes 1-15)

| # | Name | Lvl | Station | Inputs | Output | Notes |
|---|------|-----|---------|--------|--------|-------|
| 1 | Reinforced Pipe | 1 | Workbench | SM x5 | dmg 30, Fast, stagger 15% | tutorial weapon |
| 2 | Combat Knife | 1 | Workbench | SM x8, PP x2 | dmg 40, V.Fast, crit 25%, bleed 30% | |
| 3 | Shock Baton | 2 | Workbench | SM x10, CB x5, PowC x2 | dmg 45, stun 20% / 2s | |
| 4 | Plasma Blade | 3 | Workbench | PP x10, Pla x3, AP x1 | dmg 75, AP 30%, burn | |
| 5 | Titanium Wrench | 1 | Workbench | SM x12, PP x3 | dmg 55, slow, stagger 40% | door-breaker |
| 6 | Vibro Machete | 2 | Workbench | SM x15, PP x5, PowC x3 | dmg 60, AP 20%, dismember 15% | |
| 7 | Stun Knuckles | 2 | Workbench | SM x8, CB x4, PowC x2 | dmg 50, stun 35%, combo +10%/hit | **Jake only** |
| 8 | Toxic Blade | 3 | Workbench | SM x10, TR x8, CC x5 | dmg 45, poison 8/s for 5s | |
| 9 | Neural Whip | 4 | Workbench | PP x15, NT x5, AP x2, PowC x4 | dmg 35, long range, multi-hit 3, disable 25% | |
| 10 | Gravity Hammer | 4 | Workbench | SM x30, QC x1, FC x1, PP x10 | dmg 120, AoE 3m, knockdown 75% | |
| 11 | Salvari Crystal Sword | 5 | Salvari Forge | SA x20, BC x8, RG x1 | dmg 90, AP 40%, scales w/ Salvari trust | Salvari alliance gate |
| 12 | Enhanced Fire Axe | 2 | Workbench | SM x18, Pla x1, PP x4 | dmg 70, burn 3/s, door-breaker | |
| 13 | Hybrid Claw Gauntlet | 4 | Workbench | HG x3, IT x10, SM x15, MS x8 | dmg 85, life-steal 10% | **-5 humanity when equipped** |
| 14 | Riot Suppressor | 2 | Workbench | SM x12, CB x6, PowC x3 | dmg 25, stun 50% / 4s, non-lethal | capture utility |
| 15 | Overlord Shard Blade | 5 | Salvari Forge | OS x2, SA x15, QC x1, EF x1 | dmg 150, true-dmg 50%, fear aura 20% | **LEGENDARY — one per game** |

### 4.2 Ranged (Recipes 16-35)

| # | Name | Lvl | Station | Inputs | Output | Notes |
|---|------|-----|---------|--------|--------|-------|
| 16 | Makeshift Pistol | 1 | Workbench | SM x10, PP x3 | dmg 25, semi, 8 mag | starter |
| 17 | Service Pistol | 1 | Workbench | SM x15, PP x5, CB x2 | dmg 35, semi, 12 mag, high acc | |
| 18 | Compact SMG | 2 | Workbench | SM x20, PP x8, CB x4 | dmg 18, FA 900 RPM, 30 mag | |
| 19 | Assault Rifle | 2 | Workbench | SM x25, PP x10, CB x5, PowC x1 | dmg 28, FA 650 RPM, 30 mag | mid-Act-1 default |
| 20 | Pump Shotgun | 2 | Workbench | SM x22, PP x6 | 12x8 pellets, pump, 6 mag | Floor 2 unlock per bible |
| 21 | Marksman Rifle | 3 | Workbench | SM x30, PP x15, CB x8, AP x1 | dmg 120, bolt, 4x scope | |
| 22 | Plasma Pistol | 3 | Workbench | PP x12, Pla x2, AP x1, CB x6 | dmg 45, semi, hold to overcharge 3x | |
| 23 | Plasma Rifle | 4 | Workbench | PP x20, Pla x5, AP x2, FC x1 | dmg 55, FA, burn 3/s | Floor 5 unlock per bible |
| 24 | Grenade Launcher | 3 | Workbench | SM x35, PP x12, EC x5 | dmg 150, AoE 5m | |
| 25 | Chain Gun | 4 | Workbench | SM x50, PP x25, PowC x5, CB x10 | dmg 22, FA 1200 RPM, 200 mag, spin-up | Floor 4 unlock per bible |
| 26 | Rocket Launcher | 4 | Workbench | SM x40, PP x15, EC x10, AP x1 | dmg 400, AoE 8m, lock-on | bazooka analogue |
| 27 | **Lightning Gun** | 4 | Workbench | PP x18, PowC x8, AP x2, CB x12 | dmg 30, continuous beam, chain 3, stun 30% | **Floor 6 unlock + Salvari tech reference (bible line 986, 988)** |
| 28 | Cryo Cannon | 4 | Workbench | PP x20, CC x15, PC x5, PowC x4 | dmg 15, spray, freeze + 50% slow | |
| 29 | Flamethrower | 3 | Workbench | SM x25, CC x20, EC x3, PP x8 | dmg 20, burn 10/s, area-denial | |
| 30 | Auto Shotgun | 3 | Workbench | SM x30, PP x12, CB x6 | 10x8 pellets, FA 180 RPM, 12 mag | |
| 31 | Salvari Beam Rifle | 5 | Salvari Forge | SA x25, BC x10, CE x2, RG x1 | dmg 80, charge beam, penetration | |
| 32 | Acid Sprayer | 4 | Workbench | SM x20, HG x4, TR x15, PP x10 | dmg 25, armor-melt -20%/s | |
| 33 | Quantum Disruptor | 5 | Salvari Forge | QC x2, OS x1, SA x20, AP x5 | dmg 200, 5s CD, phase | endgame DPS |
| 34 | Neural Override Pistol | 3 | Workbench | PP x15, AP x2, NT x3, CB x10 | dmg 20, hack 40% vs mech, disable 8s | **Sarah only** |
| 35 | Gauss Rifle | 5 | Workbench | SM x40, PP x25, FC x2, QC x1, AP x3 | dmg 180, charge, all-material pen | |

#### 4.2.a Salvari weapons cross-reference (bible)

| Bible name | Closest recipe | Notes |
|------------|----------------|-------|
| Lightning Gun | #27 Lightning Gun | exact match — same name, Floor 6 alignment |
| Nullifier | new — see Section 4.6, recipe #N1 | bible line 989: "Removes infection" — built atop Cure Component + Salvari tech; missing from TASK_12 |
| Stasis Field | bible line 990 | NOT a personal weapon; treat as a Salvari Tech grenade variant. See Section 4.7 (recipe #N2). |
| Dimensional Rifle | new — see Section 4.6, recipe #N3 | bible line 991: "Teleports enemies" — close cousin of #33 Quantum Disruptor; reflavor or add as variant. |

#### 4.2.b Hybrid weapons cross-reference (bible lines 993-996)

| Bible name | Closest recipe | Notes |
|------------|----------------|-------|
| Nanite Swarm | new — see Section 4.6, recipe #N4 | "Converts enemies" — turns target into a temporary ally for 8s. **Costs humanity.** |
| Bio-Cannon | new — see Section 4.6, recipe #N5 | "Uses infection as weapon" — fires infected-tissue rounds; turns dead enemies into hybrids. **Costs humanity.** |
| Mind Shredder | new — see Section 4.6, recipe #N6 | "Destroys consciousness" — Sarah-only psionic weapon; uses Neural Tissue + Memory Crystal. |

These 6 weapons (N1-N6) are appended in Section 4.6 below as a hybrid /
Salvari extension set — they appear in the bible but not in TASK_12, so they
need first-class entries here.

### 4.3 Armor (Recipes 36-47)

| # | Name | Lvl | Station | Inputs | Armor | Special |
|---|------|-----|---------|--------|-------|---------|
| 36 | Makeshift Vest | 1 | Workbench | SM x15, OT x5 | 25 | -5% speed |
| 37 | Security Armor | 2 | Workbench | SM x30, PP x8, CB x4 | 45 | 1 mod slot |
| 38 | Tactical Combat Armor | 3 | Workbench | SM x40, PP x15, CB x8, PowC x2 | 60 | 2 mod slots, night vision |
| 39 | Hazmat Suit | 2 | Chem Lab | CC x20, MS x15, SM x10 | 15 | poison + infection immunity |
| 40 | Stealth Infiltration Suit | 3 | Workbench | PP x20, CB x12, AP x2, PowC x3 | 30 | active camo 10s, -50% detection |
| 41 | Powered Exoskeleton | 4 | Workbench | SM x75, PP x30, AP x5, FC x2 | 80 | no speed penalty, +20% str, +30 kg cap |
| 42 | Salvari Bio-Armor | 5 | Salvari Forge | SA x40, BC x15, OT x20 | 70 | regen 1 HP/s, self-repair |
| 43 | Riot Control Gear | 2 | Workbench | SM x35, PP x10 | 55 | -15% spd, melee reduction +40% |
| 44 | Medical Support Exosuit | 3 | Med Bay | SM x30, MS x25, PP x15, AP x1 | 40 | auto-heal at 25% HP |
| 45 | Hybrid Carapace Armor | 4 | Workbench | HG x5, IT x15, SM x25, OT x20 | 75 | life-steal 5%, **-10 humanity** |
| 46 | Quantum Phase Armor | 5 | Salvari Forge | QC x2, SA x30, AP x4, CE x3 | 65 | 25% phase through dmg, 3s invuln |
| 47 | Engineering Work Suit | 2 | Workbench | SM x25, CB x10, PP x8 | 35 | +25% craft spd, +30% repair, **+16 inventory slots** |

### 4.4 Consumables (Recipes 48-72)

Full 25 recipes from TASK_12; see Section 7 for category breakdown + the
gas-station candy bar / soda integration.

| # | Name | Lvl | Station | Inputs | Effect | Stack |
|---|------|-----|---------|--------|--------|-------|
| 48 | Basic Medkit | 1 | Med Bay | MS x5 | Heal 50 HP | 10 |
| 49 | Advanced Medkit | 2 | Med Bay | MS x10, PC x2 | Heal 100 HP + cure poison | 5 |
| 50 | Trauma Kit | 3 | Med Bay | MS x15, PC x4, OT x5 | Heal 200 HP, remove debuffs | 3 |
| 51 | Combat Stim | 2 | Chem Lab | CC x8, MS x5 | +25% dmg / 60s | 5 |
| 52 | Reflex Booster | 2 | Chem Lab | CC x10, NT x2 | +30% spd, +20% reload / 45s | 5 |
| 53 | Antidote | 2 | Chem Lab | TR x5, PC x3 | Cure infection stage 1-2 | 5 |
| 54 | Full Cure | 4 | Med Bay | CurC x3, PC x5, HG x2 | Cure stage 3 hybrid | 3 |
| 55 | Adrenaline Shot | 3 | Med Bay | CC x10, OT x5 | Revive from downed (50 HP) | 3 |
| 56 | Armor Repair Kit | 2 | Workbench | SM x10, PP x3 | +50 armor durability | 5 |
| 57 | EMP Grenade | 3 | Workbench | CB x8, PowC x3, SM x5 | Disable electronics 8m / 10s | 5 |
| 58 | Frag Grenade | 2 | Workbench | SM x8, EC x3 | 100 dmg, 6m radius | 5 |
| 59 | Smoke Grenade | 1 | Chem Lab | CC x6, SM x4 | 15s screen | 8 |
| 60 | Flashbang | 2 | Workbench | SM x6, CC x5, PowC x1 | Blind 5s, 10m | 5 |
| 61 | Incendiary Grenade | 3 | Chem Lab | CC x12, EC x2, SM x5 | Fire pool 20 DPS / 10s | 4 |
| 62 | Cryo Grenade | 3 | Chem Lab | CC x15, PC x2, SM x5 | Freeze 5s + 50% slow after | 4 |
| 63 | Toxic Gas Grenade | 3 | Chem Lab | TR x10, CC x8, SM x4 | Poison cloud 15 DPS / 12s | 4 |
| 64 | Fortification Stim | 3 | Chem Lab | CC x12, OT x8, PC x2 | +50 temp armor / 45s | 4 |
| 65 | Berserker Stim | 4 | Chem Lab | CC x15, IT x5, HG x1 | +50% dmg, +30% spd / 30s, **10% HP cost** | 3 |
| 66 | Night Vision Drops | 2 | Chem Lab | CC x8, OT x3 | NV / 5 min | 5 |
| 67 | Disposable Hacking Tool | 2 | Workbench | CB x5, AP x1 | Instant hack one terminal | 5 |
| 68 | Portable Oxygen Tank | 1 | Workbench | SM x8, CC x5 | 5 min O2 | 3 |
| 69 | Salvari Healing Crystal | 5 | Salvari Forge | BC x5, CE x1, OT x10 | Full HP + 20 HP regen / 30s | 2 |
| 70 | Infection Suppressant | 2 | Chem Lab | TR x3, MS x8, CC x5 | Pause infection 3 min | 5 |
| 71 | High-Energy Ration | 1 | Chem Lab | OT x3, CC x2 | +50 stamina, +25 hydration | 10 |
| 72 | Holographic Decoy | 3 | Workbench | CB x10, AP x1, PowC x2 | Decoy draws fire / 15s | 3 |

### 4.5 Ammunition + Weapon Upgrades + Ship Parts + Quest Items + Salvari Tech

For space, these 4 large groups are tabulated by name + level + key inputs.
Full stats live in TASK_12 §Recipes 73-150.

#### Ammunition (73-82)

| # | Name | Lvl | Key Inputs | Output |
|---|------|-----|------------|--------|
| 73 | Pistol Rounds | 1 | SM x2 | 20 |
| 74 | Rifle Rounds | 1 | SM x3 | 30 |
| 75 | Shotgun Shells | 1 | SM x4 | 10 |
| 76 | Sniper Rounds | 2 | SM x5, PP x1 | 10 |
| 77 | Plasma Cell | 3 | Pla x1 | 50 |
| 78 | Explosive Rounds | 4 | SM x10, EC x5 | 10 |
| 79 | Incendiary Rounds | 3 | SM x6, CC x4 | 15 |
| 80 | AP Rounds | 3 | SM x8, PP x3 | 15 |
| 81 | Cryo Fuel Canister | 4 | CC x10, PC x3 | 100 |
| 82 | Flamethrower Fuel | 3 | CC x12, EC x1 | 100 |

#### Weapon Upgrades — mods (83-102)

| # | Name | Lvl | Key Inputs | Effect | Applicable |
|---|------|-----|------------|--------|------------|
| 83 | Damage Enhancer Mk1 | 2 | PP x8, CB x4 | +10% dmg | All |
| 84 | Damage Enhancer Mk2 | 3 | PP x15, CB x8, AP x1 | +20% dmg | All |
| 85 | Targeting Module | 2 | CB x8, AP x1 | +20% accuracy | Ranged |
| 86 | Extended Magazine | 2 | SM x15, PP x5 | +50% mag | Firearms |
| 87 | Quick Loader | 2 | PP x10, CB x3 | -30% reload | Firearms |
| 88 | Basic Scope | 2 | PP x8, CB x4 | 2x zoom, +10% acc | Rifles, Pistols |
| 89 | Combat Scope | 3 | PP x15, CB x8, AP x2 | 4x zoom + enemy highlight | Rifles |
| 90 | Suppressor | 2 | SM x12, PP x6 | -80% sound, -10% dmg | Pistols, SMGs, Rifles |
| 91 | Fire Converter | 4 | Pla x3, CC x10, AP x2 | +15 fire, ignite | All |
| 92 | Cryo Converter | 4 | PC x5, CC x10, AP x2 | +15 cold, 20% slow | All |
| 93 | Shock Converter | 4 | PowC x5, CB x10, AP x2 | +15 shock, 15% stun | All |
| 94 | AP Enhancement | 3 | PP x12, SM x8 | +30% armor pen | All |
| 95 | Reinforced Frame | 2 | SM x20, PP x5 | +50% durability | All |
| 96 | Ergonomic Grip | 1 | SM x6, OT x3 | -15% recoil, faster swap | All |
| 97 | Laser Sight | 2 | CB x6, PowC x2, PP x4 | +25% hip-fire acc | Ranged |
| 98 | Bayonet | 2 | SM x10, PP x4 | +40 melee | Rifles |
| 99 | Salvari Crystal Enhancement | 5 | BC x5, SA x10, RG x1 | +25% dmg, never degrades | All |
| 100 | Overcharge Capacitor | 4 | PowC x6, AP x2, FC x1 | Hold for 3x dmg shot | Energy |
| 101 | Auto-Loader | 4 | PP x20, AP x3, CB x10 | Auto-reload on holster | Firearms |
| 102 | Precision Strike Module | 3 | PP x12, CB x6, AP x1 | +15% crit, +50% crit dmg | All |

#### Ship Parts (103-117) — Ship Workshop only

| # | Name | Lvl | Effect |
|---|------|-----|--------|
| 103 | Reinforced Hull Plating | 3 | +100 ship HP |
| 104 | Shield Generator Upgrade | 4 | +50 shield, +20% recharge |
| 105 | Engine Booster | 3 | +25% spd, +15% accel |
| 106 | Additional Weapon Mount | 4 | +1 ship weapon slot |
| 107 | Cargo Bay Expansion | 3 | +50 ship cargo + **+10 player slots while docked** |
| 108 | Advanced Sensor Array | 3 | +50% scan, detect hidden |
| 109 | Cloaking Device | 5 | 30s invis / 120s CD |
| 110 | FTL Drive Enhancement | 4 | -30% FTL charge, +20% range |
| 111 | Life Support Upgrade | 3 | +100% emergency O2, crew heal |
| 112 | Auto-Repair System | 4 | 5 HP/min combat, 20 HP/min idle |
| 113 | Point Defense Turret | 4 | Auto-targets missiles 50 DPS |
| 114 | Salvari Propulsion Core | 5 | +50% spd, -20% fuel |
| 115 | Mining Laser Array | 3 | Mine asteroids (material drops) |
| 116 | Tractor Beam | 4 | Grab objects up to 500m |
| 117 | Enhanced Docking System | 3 | Fast dock, breach enemies |

#### Quest Items (118-132)

| # | Name | Lvl | Station | Use |
|---|------|-----|---------|-----|
| 118 | Security Keycard | 1 | Workbench | Opens lvl 1-2 doors |
| 119 | Emergency Signal Beacon | 2 | Workbench | F7 — call rescue ship |
| 120 | **Cure Prototype** | 3 | Med Bay | Save Dr. Chen (timed) — see §5 |
| 121 | Neural Dampener | 4 | Chem Lab | Resist Overlord mind control |
| 122 | Large-Scale EMP Device | 3 | Workbench | F5 — disable drone network |
| 123 | Decryption Key | 2 | Workbench | Access research data + logs |
| 124 | Aria's Tracking Device | 2 | Workbench | 9-min rescue timer (Aria route) |
| 125 | Hybrid Stabilizer | 4 | Med Bay | Stabilize transformed allies |
| 126 | Salvari Translation Device | 4 | Salvari Forge | Communicate with Salvari |
| 127 | Overlord Containment Device | 5 | Salvari Forge | Final boss — capture ending |
| 128 | Clone Detection Scanner | 3 | Chem Lab | Reveal Sarah's clone imposter |
| 129 | Chorus Disruptor | 4 | Med Bay | F4.5 boss — separate scientists |
| 130 | Crystal Heart Container | 5 | Salvari Forge | Protect Crystal Heart transport |
| 131 | Salvari Refugee Supply Pack | 4 | Chem Lab | Improve Salvari alliance (repeatable) |
| 132 | Mothership Virus | 5 | Workbench | L49 — disable mothership |

#### Salvari Technology (133-150) — Salvari Forge only

| # | Name | Lvl | Effect | Stack |
|---|------|-----|--------|-------|
| 133 | Salvari Bio-Medkit | 5 | Heal 150 + 20 regen / 30s | 5 |
| 134 | Personal Phase Cloak | 5 | 20s invis / 90s CD | 3 |
| 135 | Crystal Shield Generator | 5 | Deployable shield 500 HP / 30s | 2 |
| 136 | Salvari Combat Drone | 5 | Summon drone 100 HP, 25 DPS / 60s | 2 |
| 137 | Resonance Bomb | 5 | 300 dmg, 10m, ignores shields | 2 |
| 138 | Salvari Bio-Enhancer | 5 | +30% all stats / 60s | 3 |
| 139 | Teleport Beacon | 5 | 50m teleport / 120s CD | 1 |
| 140 | Salvari Comm Device | 5 | Call reinforcements / 5 min CD | 1 |
| 141 | Gravity Well Generator | 5 | Pull enemies 8m for 5s | 2 |
| 142 | Crystal Resource Scanner | 5 | Highlight resources 100m | 1 |
| 143 | Nano-Repair Swarm | 5 | +100 durability all equipment | 3 |
| 144 | Psionic Amplifier | 5 | **Sarah only**: +100% hack speed | 1 |
| 145 | Crystal Energy Ammunition | 5 | 100 crystal ammo | ∞ |
| 146 | Bio-Crystal Grenade | 5 | 200 dmg + heal allies 50 HP | 3 |
| 147 | Temporal Anchor | 5 | **LEGENDARY**: rewind 10s / 180s CD | 1 |
| 148 | Salvari Crystal Turret | 5 | Auto-turret 200 HP, 40 DPS / 60s | 2 |
| 149 | Harmony Field Generator | 5 | 10m: +25% dmg, +10 HP/s / 45s | 1 |
| 150 | Salvari Masterwork Weapon | 5 | **LEGENDARY**: any weapon type, +50% all stats, unbreakable (**one per game**) | 1 |

### 4.6 Hybrid + Salvari Extension Set (N1-N6, NEW)

Six weapons referenced in the bible but absent from TASK_12. Added here so the
canon arsenal is complete.

| # | Name | Lvl | Station | Inputs | Output | Notes |
|---|------|-----|---------|--------|--------|-------|
| **N1** | Nullifier | 5 | Salvari Forge | CurC x4, BC x10, SA x20, AP x3 | Beam, dmg 40, **removes infection from target** (allies cured / enemies killed if hybrid) | Bible line 989. Cure pipeline output (§5) gates this. |
| **N2** | Stasis Field (grenade) | 5 | Salvari Forge | RG x1, QS x5, BC x6, QC x1 | Throwable; 6m sphere, **time stops inside for 5s** | Bible line 990. Treated as a Salvari grenade, NOT a personal weapon. |
| **N3** | Dimensional Rifle | 5 | Salvari Forge | QC x2, OS x1, SA x20, AP x5, MC x1 | Sniper, dmg 150, **teleports target 30m forward**; can phase enemies into geometry (instakill at edges) | Bible line 991. Distinct from #33 Quantum Disruptor (which damages); this *removes* enemies. |
| **N4** | Nanite Swarm | 4 | Workbench | NC x15, HG x5, IT x10, AP x2 | Beam, dmg 25 + 30% chance to **convert target to friendly for 8s** | Bible line 994. **-3 humanity per kill via conversion.** |
| **N5** | Bio-Cannon | 4 | Workbench | HG x6, IT x20, EC x5, PP x10 | Lobbed shot, dmg 80 + AoE 4m, **kills create new hybrid ally for 12s** | Bible line 995. **-5 humanity per hybrid spawned.** |
| **N6** | Mind Shredder | 5 | Salvari Forge | NT x12, MC x2, EF x1, RG x1, AP x3 | Beam, dmg 60, **silences target** (no abilities) + 50% chance to **destroy AI** | Bible line 996. **Sarah only.** |

### 4.7 Cross-references and notes

- **"Stage 3 hybrid"** in recipe #54 (Full Cure) maps to canon infection stage 3
  per bible §Infection Visual System (line 1066). Stage 1-2 use Antidote (#53);
  stage 3 needs Full Cure; stage 4 (fully transformed) needs Hybrid Stabilizer
  (#125) + Full Cure chain.
- **Cure Component** is itself an Epic material (drops from bosses + research
  data), not a craft output. It's THE bottleneck for the cure pipeline.
- **Humanity score** is a separate gauge already mentioned in bible Emperor /
  Sacrifice endings (lines 1040-1053). Crafting/equipping certain items
  modifies it; the spec treats `humanity` as an Inventory-adjacent player stat,
  not an inventory item.
- **Sell value / credits**: every recipe carries a sell value (TASK_12). Credits
  are stored in `Inventory.credits`. Vendors (drone shops, Fortune's End,
  rebel-base traders) buy/sell against this. NO crafting recipe consumes credits
  directly — they consume materials.

---

## 5. Cure Synthesis Quest

The cure quest is the spine of EFLZ. It's its own crafting pipeline atop the
generic recipe system, with timed sub-objectives and 4 distinct components.

### 5.1 Components (bible lines 626-633)

| Component | Source | Acquisition | Acquired by |
|-----------|--------|-------------|-------------|
| **Jake's original DNA** | Pre-infection blood sample | Story-gated: F1 lab room (cold storage). One-shot, fixed location. | Story trigger |
| **Salvari antidote base** | K'thara's clinic | Salvari alliance required; given on first major aid quest (Salvari Refugee Supply Pack #131 turn-in). | Quest reward |
| **Xenoflora — Biome 1 (Cave/Underground)** | Bioluminescent cave moss | Hand-pick in Act 2 cave biome (L11-L13). 3 nodes per playthrough. | Pickup |
| **Xenoflora — Biome 2 (Desert/Surface)** | Hardpan succulent | Hand-pick in Act 2 desert biome (L14-L17). 3 nodes. | Pickup |
| **Xenoflora — Biome 3 (Ocean/Coast)** | Salt-marsh anemone | Hand-pick in Act 2 ocean biome (L18-L20). 3 nodes. | Pickup |
| **Ancient temple purification** | Ritual site at end of Act 2 | Player carries all components to the temple in L20; activates altar; **30-real-second purification cutscene-with-controllable-camera**. | Story trigger |

Per bible: "Process takes 3 in-game hours, infected character suffers greatly
but emerges human again." The 3-IGT-hour scale is communicated as a Sarah
infection-stage countdown clock; the actual purification UI is 30 seconds.

### 5.2 Outputs (3 distinct cure tiers)

The cure pipeline produces three downstream items:

| Output | Recipe | Use |
|--------|--------|-----|
| **Cure Component (material)** | Drops from Act 2+ bosses AND from temple purification | Ingredient for #54 Full Cure, #120 Cure Prototype, #N1 Nullifier |
| **Cure Prototype** (#120) | Med Bay, 1 CurC + 3 PC + 10 MS + 5 TR | Saves Dr. Chen (Act 1, timed) — see Floor 7 |
| **Full Cure** (#54) | Med Bay, 3 CurC + 5 PC + 2 HG | Cures Sarah / allies from stage-3 hybridization |

### 5.3 Quest stages

| Stage | Trigger | Inventory deltas | UI |
|-------|---------|------------------|----|
| **C1 — Sample** | F1 cold-storage interaction | +1 `jake_dna_sample` quest item | Journal entry: "Find a way out + a cure" |
| **C2 — Base** | Complete first Salvari supply turn-in | +1 `salvari_antidote_base` | Journal: K'thara dialogue points to xenoflora |
| **C3 — Biomes** | Pick up at least 1 xenoflora per biome (3 total) | +1 each of 3 typed flora items | Map markers; companion comments per biome |
| **C4 — Purification** | Reach L20 temple with all 5 components | -all 5 components, +6 `cure_component` (material) | Cutscene + 30s ritual + journal: "Cure achievable" |
| **C5 — Application** | Craft #54 Full Cure at Med Bay, use on Sarah / ally | -3 CurC per use; ally cured | Per-ally cured flag set; ending branch flagged |

### 5.4 Branches

- **Save Sarah within 2 minutes of capture (bible Wedding Ending):** Cure
  Prototype (#120) usable mid-Act 1; Full Cure unlocks in late Act 2. Wedding
  ending requires full purification chain.
- **Salvari alliance refused (bible attack-Salvari branch, line 329):** No
  Salvari antidote base. Sarah must derive cure alone (bible line 553) — Cure
  Component drops still happen but at 50% rate; Full Cure success drops to 60%
  (bible line 550).
- **Stage 4 transformations (fully hybridized ally):** require #125 Hybrid
  Stabilizer before #54 Full Cure. Stabilizer must be applied within 30 seconds
  of full hybrid state or the ally is permanently lost.

### 5.5 Companion involvement

Per `companion-ai-design.md` (memory) the LLM-powered companion can flag cure
opportunities ("I see Biome 1 flora over there — should I grab one?"); the
deterministic reflex layer handles the actual pickup. Companions hold the
xenoflora components on the player's behalf if the player is full.

---

## 6. Weapon Mods System

### 6.1 Mod slot model

Each ranged/energy weapon has **1-3 mod slots** depending on rarity:

| Rarity | Slot count |
|--------|------------|
| Common | 1 |
| Uncommon / Rare | 2 |
| Epic / Legendary | 3 |

Slots are typed by weapon class:

| Slot type | Accepts |
|-----------|---------|
| `Optic` | Scopes, sights (#88 Basic Scope, #89 Combat Scope, #97 Laser Sight) |
| `Barrel` | Suppressor #90, Bayonet #98 |
| `Magazine` | Extended Mag #86, Quick Loader #87, Auto-Loader #101 |
| `Accuracy` | Targeting Module #85, Precision Strike #102 |
| `Damage` | Damage Mk1/Mk2 (#83/#84), AP Enhancement #94, Salvari Crystal #99 |
| `Element` | Fire/Cryo/Shock Converters (#91/#92/#93) |
| `Frame` | Reinforced Frame #95, Ergonomic Grip #96 |
| `Power` | Overcharge Capacitor #100 (energy only) |

Slots accept ONE mod each. Equipping an `Element` slot replaces any prior
element. Slot-type compatibility per weapon is in `WeaponDef.modSlots[]`.

### 6.2 Mod install / remove

- Install: at any Workbench. Cost: the mod item (consumed) + 10s craft time.
- Remove: at any Workbench. Cost: 5 SM + 5s; mod recovered with 80% chance.
  (At skill 5, 100% chance.)
- Mod stat changes are applied at install; weapon stat blocks are recomputed
  via `Weapon::recomputeStats(modMask)`.

### 6.3 Visual integration with existing weapon system

The existing `WeaponSystem` (`weapon.h`) carries a single GLB with no mod
geometry. The new system adds:

- `WeaponDef.attachmentPoints[]` — named bones in the GLB (`muzzle`, `scope`,
  `grip`, `magwell`).
- `ModDef.attachmentMesh` — optional GLB stub to attach when this mod is
  equipped (e.g. suppressor on muzzle, scope above receiver).
- Drawables for attachments are uploaded once at install and parented to the
  weapon's drawable transform in `drawViewmodel()`.

If the existing pistol GLB has no attachment points, attachments are skipped
silently — stats still apply. This keeps the spec implementable against a
minimal art asset.

### 6.4 Drone-upgrade integration

The drone-shop credits referenced in `companion_squad.cpp` design docs are
unified with `Inventory.credits`. Drone upgrades use the same mod model:

| Drone slot | Accepts | Example mods |
|------------|---------|--------------|
| `Drone-Optic` | Sensor mods | Advanced Sensor Array #108 (variant), targeting bug |
| `Drone-Weapon` | Drone-mounted weapons | Mini-Lightning Gun, Mini-Plasma |
| `Drone-Armor` | Plating | Reinforced Hull Plating #103 (variant) |
| `Drone-Power` | Power cells, fusion cores | PowC x N -> +max-HP; FC x 1 -> +duration |

Drone mods are crafted at the **Ship Workshop** (or a dedicated `Drone Bay`
sub-station if added later), using credits + materials. The unified credits
purse lets the player choose between "buy a drone upgrade" or "buy raw
materials" at hub vendors.

---

## 7. Consumables (Health, Candy Bars, Ammo, etc.)

### 7.1 The wife's gas-station note

Per Tim's memory, the wife (design collaborator) noted that gas stations
should be stocked with **candy bars, sodas, first-aid kits, and similar
consumables** that the player can stuff into their inventory. This translates
to inventory-readiness for many small-stack consumables, NOT just one-shot
heals.

### 7.2 Consumable taxonomy

Three layers of consumables exist:

| Layer | Source | Examples | Stack size |
|-------|--------|----------|------------|
| **Crafted (formal)** | Recipes 48-72 + Salvari 133-150 | Medkit, Combat Stim, Full Cure | 2-10 |
| **Looted (semi-formal)** | World pickups in gas stations, vending machines, lab break rooms | Candy bar, Soda, Energy drink, Pre-packaged first-aid kit | 5-25 |
| **Crafted-ammo** | Recipes 73-82, 145 | Pistol Rounds, Plasma Cell, Crystal Energy Ammo | 100-200 (per stack) |

### 7.3 Looted-consumable catalog (new entries)

Not in TASK_12 — added here to support the wife's note + bestiary loot tables.

| ID | Name | Source | Effect | Stack | Weight (g) |
|----|------|--------|--------|-------|------------|
| `candy_bar` | Candy Bar | Gas stations, vending machines | +15 HP, +20 stamina | 25 | 60 |
| `soda` | Soda Can | Vending machines | +10 HP, +30 stamina, +5 hydration | 20 | 350 |
| `energy_drink` | Energy Drink | Gas stations, breakroom | +25 stamina, +15% spd / 20s | 12 | 250 |
| `pre_packed_medkit` | Pre-Packed Medkit | Gas stations, first-aid boxes | Heal 30 HP (smaller than crafted #48) | 8 | 200 |
| `vending_burrito` | Microwave Burrito | Gas station hot-counter | +50 HP, +40 stamina (slow eat 4s) | 5 | 300 |
| `cigarettes` | Cigarette Pack | Gas stations | +10% steady-aim / 60s, **-3 HP/min during effect** | 10 | 30 |
| `whiskey_mini` | Whiskey Mini | Gas station liquor shelf | Heal 5 HP, -25% recoil / 60s, **-10% accuracy** | 6 | 200 |
| `bandages` | Bandages | Gas stations, abandoned cars | Stop bleeding, +10 HP | 15 | 50 |
| `painkillers` | Painkillers | First-aid boxes | +30% damage resist / 60s | 10 | 40 |
| `water_bottle` | Water Bottle | Vending machines, gas stations | +25 hydration | 15 | 500 |

### 7.4 Stamina / hydration model

Stims and rations imply a second player gauge: **stamina** (sprint /
ability budget) and **hydration** (long-term debuff if not maintained on
Act 2 surface biomes). Both are out of scope of this doc but the consumables
listed above match the gauges so the data is consistent when those systems
land.

### 7.5 Ammo as a consumable

Ammo IS inventory. Each weapon's ammo type (per TASK_12 ranged definitions:
`pistol_ammo`, `rifle_ammo`, `shotgun_shells`, `sniper_rounds`, `plasma_cell`,
`grenades`, `rockets`, `power_cell`, `cryo_fuel`, `fuel_canister`,
`acid_tank`, `neural_darts`, `ferromagnetic_slugs`, `quantum_core`,
`crystal_energy`) is a single stack in the Materials/Ammo tab. Stack caps:
100-300 (varies by type; small / heavy / large).

The HUD ammo readout pulls `inventory.countOf(activeWeapon.ammoType)`. Magazine
state is per-weapon-instance, stored on the weapon item; reload subtracts from
the inventory stack.

### 7.6 Health-drop migration

Existing pickup-able health crystals/orbs scattered in level files migrate to
`pre_packed_medkit` pickups (small heal) or, for boss rewards, `medkit_basic`
(#48, larger heal). World pickups are visually unchanged — just the
ItemDef ID they carry.

---

## 8. Implementation Plan (engine files + interfaces)

### 8.1 New engine modules

```
engine/inventory/
    IInventory.h        // pure interface (header-only)
    Inventory.h/.cpp    // canonical impl
    InventoryDef.h      // ItemDef struct
    ItemDb.h/.cpp       // load + lookup ItemDefs

engine/crafting/
    ICraftingSystem.h   // pure interface
    CraftingSystem.h/.cpp
    Recipe.h            // Recipe struct
    RecipeDb.h/.cpp     // load + lookup Recipes

engine/consumables/
    EffectDb.h/.cpp     // OnUse effect lookup (heal, stim, grenade, etc.)
    EffectApplier.h/.cpp// apply effects to player/world

app/
    hud.h/.cpp          // add HUD strip rendering (extend existing)
    inventory_ui.cpp/.h // new — full-screen inventory + crafting panel
    weapon_mods.h/.cpp  // mod install/remove + stat recompute
    pickup_component.h  // generic pickup tag (replaces ad-hoc health pickups)
```

`engine/` modules stay pure (no `app` includes). All UI lives in `app`.

### 8.2 Key interfaces (informational shapes only — NOT code)

```
class IInventory {
    virtual AddResult tryAdd(ItemId, count) = 0;
    virtual bool      remove(ItemId, count) = 0;
    virtual uint32_t  countOf(ItemId) const = 0;
    virtual SlotRef   findSlot(ItemId) const = 0;
    virtual void      iterate(Tab, callback) const = 0;
    virtual uint32_t  credits() const = 0;
    virtual void      addCredits(int32_t) = 0;
};

class ICraftingSystem {
    virtual bool   canCraft(RecipeId, StationType) const = 0;
    virtual Handle startCraft(RecipeId, StationType) = 0;
    virtual void   tick(float dt) = 0;
    virtual bool   cancelCraft(Handle) = 0;  // full refund
    virtual List<RecipeId> availableAt(StationType) const = 0;
};

struct Recipe {
    RecipeId id;
    const char* displayName;
    uint8_t levelRequired;
    StationType station;
    CharacterMask characterMask; // Any / Jake / Sarah
    Ingredient ingredients[8];   // up to 8 distinct
    OutputSpec output;           // ItemId + count
    uint16_t craftTimeSec;
    uint16_t xpReward;
};
```

### 8.3 Integration points with existing code

| Existing file | Touch |
|---------------|-------|
| `app/weapon.h/cpp` | Generalize `WeaponSystem` to support inventory-resident weapons. Pickup arming flow becomes `Inventory::tryAdd(weapon_id)`. Existing single-pistol code is preserved as the bootstrap path. |
| `app/player.h` (m_hp / m_maxHp) | Add `m_inventory` member + `m_stamina`, `m_hydration`, `m_humanity` gauges. Consumables call back into player via `EffectApplier`. |
| `app/hud.cpp` | Add HUD strip (220x140 lower-right). Pulls from `m_inventory`. |
| `app/ui.cpp` | Add full-screen inventory UI (TAB key) and crafting panel overlay. |
| `app/level*.cpp` | Replace ad-hoc health pickups with `pickup_component` entities. |
| `app/companion_squad.cpp` | Wire drone-shop credits into `Inventory::credits`. |
| `app/save.cpp` | Serialize Inventory + Crafting state to save file. |

### 8.4 Data files

- `assets/data/items.json` — all ItemDefs (1 file, ~200 entries).
- `assets/data/recipes.json` — all 156 recipes (150 from TASK_12 + 6 hybrid /
  Salvari extensions).
- `assets/data/effects.json` — consumable on-use effects.
- `assets/data/loot_tables.json` — per-enemy / per-container drop tables.

JSON is the source of truth. The DB modules parse at load + validate IDs.

### 8.5 UI binding (engine-agnostic notes)

Three new surfaces:

1. **HUD strip** — extend the existing immediate-mode HUD draw in
   `app/hud.cpp`. New widgets: ActiveWeaponIcon, AmmoReadout, ConsumableQuick,
   CreditsCounter, EncumbranceBar.
2. **Inventory screen** — new full-screen overlay. Tab bar (6 tabs), grid
   (8x5), right-pane detail. Input: TAB toggles open/closed; mouse + gamepad
   navigation.
3. **Crafting screen** — overlays Inventory while standing at a station. Adds
   left-pane recipe list, ingredients check, craft button.

### 8.6 Companion / co-op hooks

Per `companion-coop-roadmap.md`:

- Companions can **carry** items (their own small inventory, 8 slots) and
  transfer them to the player.
- LLM-driven dialogue may **suggest** crafts ("I see we have enough scrap for
  another medkit").
- During the Salvari alliance arc, K'thara can **gift** Salvari-tech recipes
  + bio-crystal samples, propagating into the player's recipe-unlocks set.

### 8.7 Saves

Save shape (delta over current save):

```
SaveFile {
    ...
    Inventory.slots[40]
    Inventory.credits
    Inventory.stash[120]
    Crafting.skillLevel
    Crafting.itemsCrafted
    Crafting.unlockedRecipes (bitset over RecipeId)
    Crafting.activeOperation (RecipeId, elapsedSec)
    Player.stamina, .hydration, .humanity
    Quest.cure_stage (C1..C5)
    Quest.cure_components_held
}
```

---

## 9. Build Order

Phased so each phase yields a playable, verifiable slice. No phase blocks on
art assets that don't exist yet.

### Phase B1 — Inventory backbone (1-2 weeks)

- `engine/inventory/` module: `IInventory`, `Inventory`, `ItemDb`,
  `InventoryDef.h`.
- `assets/data/items.json` seeded with ~30 starter items (materials + 5
  weapons + 8 consumables).
- HUD strip in `hud.cpp` shows ammo + active consumable count + credits.
- Pickup-component refactor: all existing health pickups switch to the new
  `pickup_component`.
- **Verifiable:** Walk over a scrap pile, see "+5 Scrap Metal" toast. Walk
  over a medkit, F-key heals.

### Phase B2 — Inventory UI (1 week)

- Full-screen inventory (TAB key), 6 tabs, drag-to-hotbar.
- Item-detail right pane with sell button (sells to nowhere; just exercises
  the credits flow).
- **Verifiable:** Pick up 10 scraps, see them stacked; TAB opens UI; drag
  to hotbar; close UI.

### Phase B3 — Crafting backbone (1-2 weeks)

- `engine/crafting/` module: `ICraftingSystem`, `CraftingSystem`,
  `RecipeDb`, `Recipe.h`.
- `assets/data/recipes.json` seeded with all 150 (+6) recipes.
- `CraftingStation` entity + interact prompt; Workbench placed in F1.
- Crafting UI panel overlaid on inventory.
- **Verifiable:** Stand at workbench, see recipe list, craft a Reinforced
  Pipe, find it in inventory.

### Phase B4 — All 5 stations + skill gates (1 week)

- Place Chemistry Lab, Med Bay, Salvari Forge, Ship Workshop in their
  canonical floors (per §3.2).
- Implement skill-level progression + the floor / act gates.
- **Verifiable:** Craft a medkit at Med Bay; can't see Salvari recipes until
  alliance flag is set.

### Phase B5 — Consumables + ammo (1 week)

- `engine/consumables/` module: `EffectDb` + `EffectApplier`.
- Wire 25 consumable recipes; add the looted-consumable catalog (§7.3) to
  ItemDb.
- Ammo system: each weapon checks `inventory.countOf(ammoType)` on reload.
- **Verifiable:** Craft a Combat Stim, use it, see +25% damage for 60s. Run
  out of ammo, reload pulls from inventory.

### Phase B6 — Weapon mods + drone integration (1-2 weeks)

- `app/weapon_mods.h/.cpp`: install/remove flow.
- Stat-recompute hook in `Weapon`.
- Visual attachment-point system (best-effort; skip if GLB has no anchors).
- Drone shop wired to `Inventory.credits`; drone mod slots added.
- **Verifiable:** Craft a Suppressor + Damage Mk1, install both on the
  pistol, fire shows muted sound + higher damage.

### Phase B7 — Cure quest pipeline (1 week)

- Quest stages C1-C5 wired (§5.3).
- F1 cold-storage interaction for Jake DNA.
- K'thara dialogue gating Salvari antidote base.
- 3 xenoflora pickup biomes (3 nodes each).
- L20 temple purification cutscene + 30s ritual.
- **Verifiable:** Full playthrough Act 1 -> Act 2 -> temple yields 6 Cure
  Component; craft Full Cure; apply to Sarah; ending flag set.

### Phase B8 — Hybrid / Salvari extension weapons (1 week)

- Add the 6 N-recipes (§4.6) to `recipes.json`.
- Hook humanity-score deltas on equip / use (Nanite Swarm, Bio-Cannon, Hybrid
  Claw, Hybrid Carapace).
- **Verifiable:** Craft Nullifier, fire at stage-3 hybrid ally -> ally cured.

### Phase B9 — Stash + save/load (3-5 days)

- 120-slot per-act stash at every safe room.
- Save/load delta extended; backward-compat for older saves (defaults to
  empty inventory).
- **Verifiable:** Deposit 20 scrap to stash, save, reload, scrap still there.

### Phase B10 — Companion carry + LLM suggestions (1 week)

- Companion 8-slot inventory.
- Transfer UI (open companion inventory from radial menu).
- LLM hook for craft-suggestion lines.
- **Verifiable:** Companion picks up a flora node, returns to player, dialogue
  acknowledges.

### Phase B11 — Polish + balance pass (ongoing)

- Tune material drop rates (`loot_tables.json`).
- Tune craft times and skill-level breakpoints.
- Encumbrance + stamina + hydration polish.

---

## 10. Open Questions / Risks

| # | Question | Default if unresolved |
|---|----------|-----------------------|
| Q1 | Should the inventory be slot-grid or pure-stacked-list? | Slot-grid (40 slots, 8x5). Already specced. |
| Q2 | Are companions' inventories visible to the player UI, or just driven by AI? | Both — show on radial-menu open. |
| Q3 | Does the player keep crafted items across acts, or reset at story beats? | Keep. Stash carries across acts, full inventory carries through. |
| Q4 | How does humanity-score interact with crafting? Does it gate any recipes? | Yes — recipes #13, #45, N4, N5 require humanity > 30 to even appear. |
| Q5 | Is there an in-game vendor that *buys* crafted items, or only consumables / mats? | Yes — Fortune's End Casino (L29-30) buys any sellable item. |
| Q6 | Should ammo crafting consume materials or be infinite via a "reloader" station? | Consume materials (per TASK_12); reloader station NOT in canonical recipes. |
| Q7 | Are weapon durability + repair a thing? | Yes — durability per TASK_12; #56 Armor Repair Kit + workbench-side repair. |
| Q8 | Cure-quest companion-carry: can a companion carry the Jake DNA sample? | No — story item, player-only. Other components: yes. |

---

## 11. Acceptance Checklist

For the spec to be considered complete enough to start B1:

- [x] Item categories enumerated (6 tabs).
- [x] Slot count + weight system specified.
- [x] All 5 stations placed with floor/act gates.
- [x] All 150 TASK_12 recipes tabulated.
- [x] 6 hybrid/Salvari extension recipes (N1-N6) added.
- [x] Cure quest 5-stage pipeline specified.
- [x] Weapon mod slot model + drone-integration model specified.
- [x] Consumables (crafted + looted + ammo) categorized; gas-station catalog
      added.
- [x] Engine module layout + interfaces sketched.
- [x] 11-phase build order with verifiable per-phase outcome.

---

## 12. References (file paths)

- `G:\Unity_Projects\PARALLEL_TASKS_8_CHATS\TASK_12_EscapeLab48_COMPLETE_CRAFTING_SYSTEM.md`
  — 150 recipes, 32 materials, 5 stations, 5 skill levels, full stat tables.
- `G:\Unity_Projects\# 🎮 ESCAPE FROM LAB ZERO - ULTIMATE Narrative Design.txt`
  — bible. Specifically:
  - lines 117, 325-329, 338, 382, 407-414, 543-557, 586, 620-633, 681,
    836-857: cure components, Salvari trust, ally management.
  - lines 982-996: weapon progression + Salvari + hybrid weapons.
  - lines 1040-1053: humanity / endings.
- `G:\X3Native\app\weapon.h`, `weapon.cpp` — existing pickup + viewmodel
  system to extend.
- `G:\X3Native\app\player.h` — m_hp / m_maxHp (lines 197-198).
- `G:\X3Native\docs\design\EFLZ_MASTER_PLAN.md` — act/floor structure.
- `G:\X3Native\docs\design\EFLZ_WORLD_STRUCTURE.md` — floor list + biomes.
- `G:\X3Native\docs\design\EFLZ_BESTIARY.md` — enemy material drops.

---

> **Spec status:** Ready for Phase B1 implementation. No code changes made by
> this document; all engine/app files referenced are pre-existing or new
> stubs to be created in build phases B1-B11.
