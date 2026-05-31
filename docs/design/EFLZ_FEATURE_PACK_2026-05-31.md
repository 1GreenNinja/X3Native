# EFLZ — Feature Pack 2026-05-31 (Tim's Wife's Design Notes)

> **Source:** design notes from Tim's wife, relayed via Tim 2026-05-31 (late-night session).
> **Status:** design spec — not implemented yet. Engine impact ranges from medium
> (vehicles + drivable cars, gas-station infra, weather sliding) to small (Sarah's
> Clone variant, fitness-center room art, autumn-leaves shader).
> **Authority:** Tim's wife's contributions to the EFLZ design corpus; this doc is the
> first capture of her input into the repo and supersedes any verbal-only notes.
> **Related:** [`MASTER_GAME_PLAN.md`](../MASTER_GAME_PLAN.md), [`EFLZ_WORLD_STRUCTURE.md`](EFLZ_WORLD_STRUCTURE.md), [`EFLZ_BESTIARY.md`](EFLZ_BESTIARY.md), the in-flight overnight spec set ([`EFLZ_TECH_SYSTEMS.md`](EFLZ_TECH_SYSTEMS.md), [`EFLZ_BESTIARY_RECONCILE.md`](EFLZ_BESTIARY_RECONCILE.md), [`EFLZ_CRAFTING_INVENTORY.md`](EFLZ_CRAFTING_INVENTORY.md), [`EFLZ_ACTS_2_4_GAPS.md`](EFLZ_ACTS_2_4_GAPS.md)).

---

## Why this exists

Tim's wife dropped a feature batch the night of 2026-05-31 that adds real-world
texture to the game (weather, vehicles, trees, fitness centers) and one substantial
new antagonist (Evil Sarah Clone). These are not in the 940-page bible or any of
the TASK_* design files in `G:\Unity_Projects\PARALLEL_TASKS_8_CHATS\` — they're
fresh additions that should be folded into the master plan but live here as a
single capture point until they are.

---

## 1. WEATHER SYSTEM — rain, clouds, thunderstorms

The single highest-impact addition. Touches: VFX, audio, gameplay physics, AI behavior,
roof/cover queries, and a new "lightning damage" event channel.

### 1.1 Weather state machine

A global `WeatherSystem` tracks the current weather state per level/biome:

| State | Visual | Audio | Gameplay effects |
|---|---|---|---|
| `CLEAR` | Default sky | Ambient | None |
| `OVERCAST` | Grey scrolling clouds; lower sky luminance | Wind | Subtle: -10% ambient outdoors |
| `RAIN` | Particle sheet rain + screen-space wet shader on surfaces | Constant rain hiss + drips | **Wet ground = sliding** (see §1.2); enemies + player both slide |
| `THUNDERSTORM` | RAIN + intermittent lightning flashes + heavier downpour + low cloud ceiling | RAIN + thunder claps + 4-Hz wind | Sliding **+ lightning strikes** (see §1.3) |

State transitions are level-scripted (cutscene-driven for set-pieces like F7 Clone
arrival — the bible's Floor 7 already specifies *"thunderstorm outside"*) and/or
randomly cycled in Act-2/Act-4 outdoor levels with a per-biome bias (Crystalline
Desert = rare; Toxic Swamplands = frequent; Earth liberation cities = variable
per-region).

### 1.2 Sliding physics (RAIN + THUNDERSTORM)

When weather state is RAIN or THUNDERSTORM and an entity (player or NPC monster) is
**OUTDOORS** (see §1.4 cover check), apply a sliding modifier to its locomotion:

- **Friction reduction:** ground friction multiplier drops from `1.0` → `~0.4`.
- **Acceleration cap:** lateral input acceleration drops to `~50%`.
- **Stop time:** when input is released or AI is in "brake," the velocity decay
  curve is ~2.5× slower (the *slide-to-a-stop* sensation).
- **Cornering:** sharp turns over-shoot — the entity's velocity vector
  re-aligns more slowly to its facing.

This applies equally to **monsters** (their pathing nodes need a slip-tolerance
factor) and to the **player** — so combat in storms becomes a high-skill ballet of
predicting where you and your enemies are *actually* going to end up. Bosses can be
exempted by tag (boss arenas should not turn into slippery-floor jokes — `Tag::Boss`
opt-out per encounter).

**Engine integration:**
- New: `app/weather.{h,cpp}` (`WeatherState`, `WeatherSystem::tick`, hooks to
  `player.cpp` ground-friction + `monster.cpp` locomotion).
- Existing: there is already a `app/weather.cpp` per the CMakeLists source list —
  CHECK IT FIRST and extend; this spec assumes that file is currently a stub
  (audit before implementing).
- Player + monster need a `bool m_sliding` flag + scaled friction in their
  ground-contact path; existing `app/physprops.cpp` is the right home for the
  friction multiplier.

### 1.3 Random lightning strikes (THUNDERSTORM only)

Per thunderstorm period (suggest: every `8-15 s` window), each **outdoor non-covered
entity** rolls a **9% chance** to be struck by a lightning bolt:

| Target | Effect |
|---|---|
| Enemy / monster | **Instant kill** (apply lethal damage event with `lightning` damage type — drives unique death VFX: blackened silhouette + crackling residue + smoke for ~3 s) |
| Player | **Heavy damage** (~`40 HP` of `lightning` damage — enough to threaten but not always-kill; tuneable per difficulty). At low HP, **lethal.** |
| Companion (Sarah, K'thara, etc.) | Same as player — heavy damage, can kill |
| Vehicles (see §2) | EMP burst — engine stalls for 5-10 s; if "key on" → can restart, if tank-hit while refueling → catastrophic explosion (see §2.4) |

**VFX:** a `LightningStrike` VFX class — pre-strike "tracer" flash on the ground
~250 ms before the strike (gives the player a chance to scramble), then the bolt
itself (vertical line of additive-blended light + screen flash + bloom), thunder
clap audio + camera shake.

**Strike count cap:** at most 3 simultaneous strikes per frame across all entities
(visual overload + perf). Strikes are spaced ≥150 ms apart on the audio side to keep
thunder claps individually readable.

**Engine integration:**
- New: `WeatherSystem` owns a per-frame lightning roller; iterates the entity list
  filtered by `IsOutdoor(e) && !IsCovered(e)`; rolls the 9% and emits a
  `LightningStrikeEvent` on hit.
- New: `app/fx.cpp` (already exists) gets a `LightningStrike` particle type.
- Damage: route through the existing damage path with a new `DamageType::Lightning`
  tag so future immunities/resistances can branch (e.g., a Salvari-tech armor mod
  could grant -50% lightning damage).

### 1.4 Cover check (the "9% if not in a building or under a platform")

Critical piece. An entity is **covered** iff:

1. **Indoor:** the player/NPC's current `roomId` is non-`kNoRoom` (the existing
   per-room PVS already tracks this — see `app/scene.h::Entity::roomId`); rooms
   are by definition indoor unless flagged `outdoor`.
2. **Under a platform:** a vertical raycast up from the entity's center hits any
   static geometry within `8 m` overhead.

If either is true → exempt from lightning AND from sliding (indoor floors aren't
wet). Otherwise → exposed.

**Engine integration:**
- `WeatherSystem::isCovered(Entity&)` — cheap test (room-id flag first, then a
  cached upward raycast updated every ~250 ms to avoid per-frame raycasts).
- Level data: every `Room` definition needs an `outdoor: bool` flag (default
  `false`); the existing JSON canonlevel loader is the place to plumb it.

### 1.5 Audio + visual polish

- **Rain particles:** screen-space rain shader (existing engine has particle.{vert,frag});
  add a `kRainParticleCount = 4096` particle pool driven by camera position.
- **Wet shader:** surface darkening + reflection boost when a fragment is below
  ceiling (could use the existing SSGI or a cheap downward raycast).
- **Thunder audio:** procedural — random-amplitude low-frequency rumble +
  high-frequency crack envelope; existing `cues.cpp` is the right home.
- **Lightning sky flash:** raise sky albedo to ~3.0× for 80-150 ms, then decay.

### 1.6 Test gates

- `--test-weather` — drives WeatherSystem through all 4 states; asserts
  sliding modifier applies/clears; asserts 9% strike rate over a 10,000-tick
  Monte-Carlo with cover and exposure mix; asserts cover check matches manual
  raycasts.
- `--world weather` — visual showcase: rain → thunderstorm → strikes on dummies.

---

## 2. VEHICLES + GAS STATIONS

A new vehicular gameplay layer with a dedicated unlock path and a high-impact
gas-station explosion mechanic.

### 2.1 Drivable vehicles

A new `app/vehicle.{h,cpp}` (already partially exists — engine has a `vehicle.cpp`
in CMakeLists; AUDIT first) hosting:

- **Mount/dismount:** `E` interact when standing within ~`1.5 m` of a vehicle.
  Player root attaches to the driver seat bone; camera switches to a
  vehicle-relative chase cam (existing thirdperson camera reused, offset tuned).
- **Driving model:** simple arcade physics — forward throttle, brake, steering, +
  optional handbrake. Not a sim. Jolt physics handles the body; new vehicle
  controller adds wheel raycasts + torque.
- **Vehicle types (initial 3):**
  - **Civilian sedan** (Earth Act-4 only): fast on roads, fragile.
  - **Armored truck** (Earth Act-4 + mountain base): durable, mounts a turret.
  - **Salvari hover-buggy** (Keth'zar Act-2): hovers ~1 m above ground, no wheels,
    glides over terrain — unique driving model (uses vertical thrust to hold height).
- **Damage:** vehicles take damage from collisions + weapons. Below 25% HP → black
  smoke trail. Below 5% → tagged-for-explosion (timer 5 s) — drive away or eject.
  At 0 → explode.
- **Combat:** the armored truck has a roof turret the player or a companion can
  man (chaingun, plasma rifle, or lightning gun based on weapon ladder progress).

### 2.2 Gas stations

A new world-object class `GasStation` that bundles:

- **Pumps** (2-4 per station, each `interact: refuel` if vehicle adjacent).
- **Mini-mart store** (one room inside the station structure) stocked with:
  - **Health powerups** (3-5 small, occasionally 1 large) — see `EFLZ_CRAFTING_INVENTORY.md` for inventory spec
  - **Candy bars** (consumable, +10 HP each, lightweight; placeholder name —
    final naming TBD; could be `Sarah's Granola Bar` for in-universe flavor)
  - **Weapons** (rare — 5-10% chance per station to carry a lower-tier weapon: pistol clip, shotgun)
- **Underground supply tanks** — 2-4 fuel tanks visible behind the building.
  These are the "blow up" target (see §2.4).
- **Terminal** — optional puzzle interaction (some stations are locked, requiring
  a hack OR force entry).

**Placement:** required on every Act-4 Earth-liberation level with vehicle gameplay
(L37 atmospheric-entry onwards). Optional accents in Act-2 industrial zones
(L17 Downtown, L19 Spaceport).

### 2.3 Refuel

When the player drives up to a pump, `E` triggers:

1. 5-second hold to refuel (visual: gas-pump bar fills + audio "pump click")
2. Fuel restored to 100% on the vehicle's HUD
3. Optional cost: refueling consumes 1 "fuel credit" (a craftable/lootable item
   per the inventory spec) — limits unlimited refueling

If the player attacks the pump or shoots near it while another player is
refueling: spark → small fire → escalates to §2.4 if not extinguished within ~3 s.

### 2.4 Blow up the gas station — the centerpiece mechanic

A **massive area-of-effect explosion** that kills everything inside a **1.5
camera-width radius**. (Tim's wife was very specific about this.)

**Trigger paths:**
- Direct shot at the underground supply tanks (visible behind the building)
- Sustained fire / explosive damage to a pump
- Lightning strike on the station during a thunderstorm
- Player explicitly arms a charge (Sarah-skill: place C4)

**Radius math:** at the player's default FOV (60°) and standard 1080p framing,
"1.5 camera widths" at the player's depth ≈ `35-45 m`. Implement as a configurable
`kGasStationBlastRadius = 40.0f` constant (per the source spec; tuneable).

**Damage:** lethal to all unsealed entities in radius. Vehicles in radius are
destroyed; structures in radius take heavy damage; the gas station itself becomes
a flaming crater.

**Pre-blast warning:** 1.5-second telegraph — pumps spark, klaxon sounds, color
shifts on lights — so the player can sprint clear. This gives the strategic option
of *deliberately* using the explosion as a weapon (kite enemies onto the lot, snipe
the tank, sprint to cover).

**VFX:** central fireball (use existing fx particle pool), shockwave ring,
secondary debris (chunks of pumps + station roof), 4-second smoke pillar afterward.
Audio: deep low-frequency boom + flame crackle + debris-tinkle layer.

**Engine integration:**
- New: `app/gas_station.{h,cpp}` (place-able world object with attached entities:
  pumps, mini-mart shelves, fuel tanks).
- Hooks into the existing weapon/damage path for tank HP + cascade.
- The blast = a `BlastEvent` reusing the existing explosion code path used by
  barrels (`app/barrels.cpp`) — just much larger.

### 2.5 Unlock path — Sarah hacks the underground mountain garage

Vehicles are **NOT available at game start.** They unlock via a specific story
beat:

1. After some Act-2 progression (e.g., L11 Salvari Camp or L18 Underground
   Resistance — call this `kVehicleUnlockLevel` and align to the Acts-2-4 gap doc
   when the agent finishes)…
2. The player reaches an **underground mountain base** with a **secured garage
   door**.
3. **Sarah hacks the garage terminal** (a 30-90 second hack-defense beat,
   similar to her F5 master-hack but scoped smaller).
4. Garage opens → 3-5 vehicles inside, all driveable from this point forward
   (the player can call them in via a hub at safe-houses).

**Required for certain levels:** the wife's note says vehicles are *required* for
specific levels. Suggested gating:
- **L19 Spaceport Assault:** vehicle approach required (existing JSON already hints
  at "vehicle combat" — see `Level_19_SpaceportAssault.json`).
- **L37 Atmospheric Entry "Falling Home"** (Earth landing): vehicle escape from
  landing zone required.
- **L39-41 Regional Liberation** trilogy: vehicles for traversal across cities.
- **L42 New York Reclamation:** urban warfare with vehicles.
- **L45 Return to Lab Zero:** approach in armored convoy.

**Engine integration:**
- New: `app/garage.{h,cpp}` — the unlockable garage hub.
- Hook into existing `app/save.{h,cpp}` for the unlock flag (`kVehiclesUnlocked`).
- New objective + dialogue beats (see `EFLZ_DIALOGUE_CATALOG.md` from Agent 4).

### 2.6 Vehicle-required level enforcement

For "required for certain levels" enforcement: the level's spawn point places the
player inside a vehicle, OR the level fails-out if the player attempts to enter
without a vehicle (door blocked, NPC says "You'll need wheels for this one").

### 2.7 Test gates

- `--test-vehicle` — drive/mount/dismount, damage/destruction, refuel cycle.
- `--test-gasstation` — blast radius math, kill confirmation, telegraph timing.
- `--world vehicle` — drive a sedan around a small test arena with 2 gas stations
  and 6 enemies; verify blast kills the enemies in radius.

---

## 3. EVIL SARAH CLONE (NEW antagonist)

Tim's wife has explicitly flagged the **Evil Sarah Clone** as a new antagonist —
the natural counterpart to Jake's Clone (F7 boss, already shipped). Sarah's Clone
fills the same narrative role: a corrupted dark mirror of the partner.

### 3.1 When does Sarah's Clone appear?

The narrative trigger depends on the Act-1 Floor-7 outcome (the timeline lock):

- **Omega timeline** (Sarah saved <2 min, full clean save): Evil Sarah Clone
  appears in **Act 4** (Earth liberation) as a **recurring mini-boss** — the
  Overlord's attempt to recreate the breeding program using a hacker-clone
  template. Appears in L42 (NYC), L45 (Return to Lab Zero), L49 (Proto-Overlord
  Gauntlet, replacing one of the 5 corrupted Earth leaders).
- **Alpha timeline** (3 women saved, Sarah lost): Evil Sarah Clone is in fact the
  *original* Sarah turned via the F7 breeding completion → becomes the **Breeder
  Queen** primary boss line in Act 2 (recurring) + Act 4 finale.
- **Beta timeline** (Sarah saved first, women lost): Evil Sarah Clone is **a
  duplicate manufactured later** — appears as a single Act-3 set-piece, Sarah
  must fight her own mirror, identity-crisis dialogue beat.

This means Evil Sarah Clone has **3 distinct manifestations** depending on the
player's Floor-7 outcome — leverages the existing timeline branching (see
`app/timeline.{h,cpp}`).

### 3.2 Stats + abilities

Evil Sarah Clone mirrors Sarah's strengths (hacker + tactical mind) twisted into a
combat antagonist:

| Property | Value | Note |
|---|---|---|
| HP | 4500 | Mid-tier boss (Jake's Clone = ~5000) |
| Speed | High | Sarah is fast and nimble — moreso than Jake |
| Primary weapon | Plasma pistol (Salvari-tech variant) | Sarah's signature in shipped game |
| Special: Drone Summon | Spawns 3 hacker drones every 20 s | Mirrors Sarah's master-hack ability |
| Special: System Override | Disables player's HUD for 5 s every 45 s | Crushing — player must fight blind |
| Special: Console Lockout | Locks doors / pumps / vehicles in arena for 8 s every 60 s | Strategic — denies player escape routes |
| Phases | 3 | (1) ranged + drones, (2) +System Override, (3) +Console Lockout + melee |
| Weakness | EMP grenades (interrupt her hack channels) | Player can craft these (per `EFLZ_CRAFTING_INVENTORY.md`) |

### 3.3 Visual + character design

- Same body model as Sarah (reuse the rig)
- Cyber-augmented + Overlord-corrupted: glowing alien-blue eye implants, neural-jack ports along jawline, slightly metallic skin tone
- Wears a corrupted version of Sarah's outfit (darker palette, exposed cybernetics)
- Voice: Sarah's voice with a layered "alien chorus" undertone (same processing as Aria's `Siren` if shipped — reuse if available)

### 3.4 Dialogue hooks

Evil Sarah's banter should hit Jake where it hurts. Sample lines (refine via
Agent 4's dialogue catalog):

- *"Did you really think she came back to you whole, Jake? They only let you have her so you'd stop fighting."*
- *"I've seen what's in your head. The version of her you imagine — she doesn't exist."*
- *"The original begged at the end. Did the real you ever hear her?"*
- *"You can't shoot me, Jake. Not really. You'll always see her face."* (during low-HP)

If the player kills Evil Sarah Clone, the **real** Sarah (if alive) provides
post-fight grief/relief dialogue depending on the timeline.

### 3.5 Engine integration

- Extend the existing `MultiPodBoss` / boss pattern in `monster.{h,cpp}` to add
  an "EvilSarahClone" row.
- Existing `app/spire_top.cpp` shows the Jake's-Clone setup — mirror that for
  Sarah's Clone but make her movable across Act 4 levels (not a fixed-arena
  boss).
- New `--test-evilsarah` gate.

---

## 4. EVIL JAKE CLONE (already shipped — note ONLY)

This is in the bible's Floor 7 spec and is already implemented as the F7 boss in
`app/spire_top.cpp` + the `MultiPodBoss` infrastructure. **No new work needed
here.** This bullet exists in the wife's notes for completeness next to Evil
Sarah Clone.

**Possible enhancement:** if Evil Sarah Clone is introduced in Acts 2-4 (above),
Evil Jake Clone could *also* return as a recurring Act-4 antagonist (paired with
Sarah's Clone) — the "Hybrid Commander couple" the Overlord originally intended.
Worth a follow-up design decision; not in the wife's note explicitly.

---

## 5. TREES — Earth biome, autumn-colored in fall

**Visible only on Planet Earth levels.** No trees in Keth'zar's crystalline desert
or space.

### 5.1 Tree mesh + foliage system

- **Mesh source:** convert a few SpeedTree-style GLBs (oak, pine, maple, birch — 4
  varieties to cover seasonal range) OR procedurally generate via a `tree_prim`
  generator (similar to existing `mesh_prims.h` box/sphere generators but for
  branched structures).
- **Leaf rendering:** instanced quads (billboards) for distant leaves; mesh
  patches for closer. Use the existing bindless texture array + a per-leaf-cluster
  color tint to drive the seasonal shift.
- **Instancing:** trees are heavy at count. Use the existing GPU-driven multidraw
  indirect path (the engine already supports this via `kMaxDrawsPerFrame`); a
  Earth city forest scene can be 100-500 trees, but they should batch by mesh.

### 5.2 Seasonal color shift

A `TimeOfYear` global (`SPRING / SUMMER / AUTUMN / WINTER`) drives a per-tree
leaf color tint:

| Season | Leaf color | Density | Notes |
|---|---|---|---|
| Spring | Bright green | High | Default |
| Summer | Deep green | High | Default+ |
| **Autumn** | **Red / orange / yellow blend (per-tree random)** | High → declining | Wife's specific callout |
| Winter | Bare branches | None | Leaves dropped (visual: leaves on ground) |

`TimeOfYear` advances when the player progresses through certain
levels/checkpoints (game-time, not real-time). Earth-liberation Act 4 is long
enough to span 1-2 seasons.

### 5.3 Engine integration

- New: `app/trees.{h,cpp}` — `Tree` prim, instance manager, seasonal tinter.
- Reuse: existing terrain `app/terrain.cpp` placement API (`placeOnTerrain`) to
  anchor trees to the heightfield.
- Reuse: bindless textures + PBR shader (just-landed slice 1+2) so trees can have
  bark normal maps + leaf alpha-cutout.
- New: `--test-trees` gate (verify mesh, instancing, color shift).
- New: `--world trees` showcase (a forest scene with 200 trees, drive the season
  through all 4 to confirm color shift).

### 5.4 Visual reference

Trees should look like **actual trees** — not stylized lollipops. Tim's wife was
specific. Branching structure should be irregular, leaf clusters should have some
depth (not flat planes), and at autumn each tree should pick a slightly different
color from the red/orange/yellow palette so a forest doesn't look uniform.

---

## 6. FITNESS CENTER (Lab Zero room)

A specific room in the Lab Zero facility (Jake's escape building) that he returns
to in **L45 Return to Lab Zero** (Act 4).

### 6.1 Why it matters narratively

- **First visit (Act 1):** Pre-escape, Jake passes through (or sees through a
  window) the corporate fitness center where Lab Zero employees worked out before
  the Overlord corruption. Adds humanity-before-horror texture — these were
  normal people. Could be a side-room off F1 atrium or F4 cybernetics (where the
  blurred line between "employee perks" and "augmentation surgery" is most
  poignant).
- **Return (L45):** Jake returns to find the fitness center either intact (eerie:
  treadmills still running with no one on them, locker room empty, the music from
  the speakers eerily cheerful) OR repurposed (Overlord-style horror — workout
  equipment now restraint devices for failed experiments). Wife's note implies the
  Return moment is the more important beat — *"Jake escapes from and eventually
  has to return to rescue Sarah"* — fitness center is a memorable, character-rich
  setting for one of those return moments.

### 6.2 Room contents (graybox spec)

A medium-sized room (~12 × 8 m) containing:
- Cardio area: 4 treadmills, 2 stationary bikes, 1 elliptical
- Weight area: free-weight rack, 2 weight benches, a squat rack
- Mirrors covering one wall (multi-pass reflection — fun engine tech demo: glass
  pass already supports this)
- Locker room (sub-room, ~4 × 6 m): rows of lockers + a shower area + restrooms
- Possible secret: one of the lockers contains a keycard or weapon stash (Tim's
  novel-canon stash hook)

### 6.3 Mechanics / loot

- Pickup-able **dumbbell** = melee weapon (medium damage, slow swing, satisfying
  thud)
- Lockers contain **fitness-themed loot**: protein bars (consumable, +5 HP),
  spare workout clothes (cosmetic / disguise option), wallet money (currency)
- The locker-room shower can be triggered as an environmental hazard (steam fog
  reduces visibility) or healing zone (clean water — minor HP regen)
- **Return-visit horror twist:** in L45 the treadmills are running on their own,
  the mirror shows reflections of dead employees, ghosts of former-self Sarah
  whispers. Use the existing companion-AI dialogue system to script the haunting.

### 6.4 Engine integration

- New room data in the level loader JSON (`Floor1` or `Floor4` of the spire — TBD
  which floor; depends on where it best fits narratively).
- Reuse: existing graybox room builder + asset pipeline.
- New "fitness equipment" mesh prims (treadmill, dumbbell, weight bench, locker
  bank). Could convert Unity Asset Store packs via the just-landed
  `tools/convert_unity_pack.py` pipeline.
- Glass mirror reuses the just-landed glass material (mirror = glass with
  `roughness = 0` + reflection sample).

### 6.5 Test gate

- `--world fitness` — show the room walkable in both Act-1 intact and Act-4
  haunted modes.

---

## 7. Build order (priority across all 6 features)

Ranked by ROI (gameplay impact ÷ engine cost). Each is **independent** — they can
land in any order.

| Rank | Feature | Why first | Approximate effort | Blocked by |
|---|---|---|---|---|
| 1 | **Weather sliding + lightning** | Single biggest gameplay-feel addition; affects every outdoor level once it lands. Atmospheric set-pieces (F7 storm) immediately benefit. | Medium (~1-2 weeks: VFX, audio, physics, cover-check, test gate) | None |
| 2 | **Gas-station explosion mechanic** | Huge "wow" moment — the player will brag about this. The 1.5-camera-width blast is unique. | Medium (gas station object + blast + cascade) | Vehicles (#3) ideally, but can ship standalone (stations without drivable cars) |
| 3 | **Drivable vehicles + Sarah's garage hack unlock** | New traversal pillar; gates 5+ level designs (L19, L37, L39-42, L45). | Heavy (~2-3 weeks: physics, AI, garage hub, vehicle art) | Sarah's drone-hack from F5 (already designed) |
| 4 | **Evil Sarah Clone** | Strong story beat; reuses existing boss patterns | Light-Medium (~1 week: extend MultiPodBoss row + visual variant + dialogue) | Floor-7 timeline lock (already implemented) |
| 5 | **Earth trees + autumn color** | Atmospheric — Act 4 will feel barren without them | Medium (~1-2 weeks: foliage system + seasonal tint + GPU instancing) | None |
| 6 | **Fitness Center room** | Polish / character moment; one-off room | Light (~2-4 days: assets + room + return-visit twist) | None |

---

## 8. GitHub issue candidates

When the GitHub todo list is cleaned up (see master task list), each of these
features could become a tracked issue:

1. **`#weather-system`** — rain/storms + sliding + lightning + cover check
2. **`#gas-station-blast`** — gas station object + 1.5-camera-width blast
3. **`#vehicles-garage`** — drivable vehicles + Sarah's underground garage hack unlock
4. **`#evil-sarah-clone`** — recurring antagonist; 3 timeline manifestations
5. **`#earth-trees`** — foliage system + seasonal autumn color
6. **`#fitness-center`** — Lab Zero room + return-visit horror beat

---

## 9. Cross-references with the rest of the design

When the in-flight overnight specs land (Agents 1-7), check for overlap:

- **`EFLZ_TECH_SYSTEMS.md`** (Agent 1) — the Bio-Integration Lab spec may interact
  with the fitness-center room (both are "body modification" spaces); also the
  weather VFX requires shader work that may overlap with Agent 1's shader notes.
- **`EFLZ_SKILL_TREES.md`** (Agent 2) — Sarah's "hack garage terminal" might be a
  trainable skill; vehicle driving could be a skill node.
- **`EFLZ_CRAFTING_INVENTORY.md`** (Agent 3) — fuel canisters, EMP grenades,
  candy bars (gas-station consumables) all flow through the inventory system.
- **`EFLZ_DIALOGUE_CATALOG.md`** (Agent 4) — Evil Sarah Clone's dialogue + the
  fitness-center haunting beat both need entries.
- **`EFLZ_BESTIARY_RECONCILE.md`** (Agent 5) — Evil Sarah Clone needs its own
  bestiary entry; lightning kills change every enemy's "death" event.
- **`EFLZ_SIDE_QUESTS_ACHIEVEMENTS.md`** (Agent 6) — achievements for "Strike One"
  (kill an enemy via lightning), "Pump and Run" (use a gas station explosion to
  kill 5+ enemies), "Rolling Thunder" (drive a vehicle through a storm and
  survive), "Mirror Match" (defeat Evil Sarah Clone).
- **`EFLZ_ACTS_2_4_GAPS.md`** (Agent 7) — vehicles needed for L19/L37/L39-42/L45;
  fitness-center revisit lives in L45; Evil Sarah Clone manifestation table
  needs to align with the per-level boss list.

---

— Captured 2026-05-31 by the 13700K (clean-room engine rig).
Author of original design notes: Tim's wife. ❤️
