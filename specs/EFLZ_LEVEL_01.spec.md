# Spec: EFLZ Level 1 — "Awakening" (first playable level)

> A **buildable** first-level spec for the X3Native engine. Translates the EFLZ opening
> (Floor 1 "Awakening", source `Floor1_AwakeningManager.cs`) into what we can graybox **now**
> with existing engine primitives, plus an explicit list of what's missing to build next.
>
> This is a **clean-room** target: it distills EFLZ *intent and structure* only. No Unity C#,
> no id Tech / RBDOOM source. Build with our own primitives.

- **Status:** SPEC
- **Design source (READ-ONLY):** `G:\Unity_Projects\PARALLEL_TASKS_8_CHATS\Floor1_AwakeningManager.cs` (+ `TASK_7_DIALOGUE_CUTSCENES.md`, bestiary, `docs/ASSET_INVENTORY.md`)
- **Engine primitives targeted:** `app/scene.h` (Entity/Tag/Scene), `app/level.h` (graybox builder), `app/door.h` (DoorSystem/buildDoorAndButton/tryUse), `app/weapon.h` (WeaponSystem), `app/monster.h` (MonsterSystem), `app/player.h` (Player), `app/hud.h` (Hud), Jolt physics (`engine/physics/IPhysicsWorld.h`), M2 model loader + D5 asset source.
- **Scope guard:** this is the *first playable EFLZ slice grown into real content* — the detention/escape opening. Not the whole 7-floor act, not the timed-rescue branching (that is Floor 2).

---

## 1. Purpose (one paragraph)

Build the opening of EFLZ as a playable level: Jake **wakes in a detention cell**, **discovers super-strength**
(crushes equipment, bends bars, rips the cell door), the facility goes to **alarm** and spawns guards/drones, he
fights to a small **armory** to pick up the energy pistol, pushes through a **security checkpoint** to a **boss
arena** for a short fight against **Chief Martinez**, then steps into the **elevator** to complete the level. It is
deliberately the engine's existing vertical-slice spine — *walk → button/door → weapon pickup → shoot monster* —
dressed as real EFLZ content, so it is buildable today and grows naturally as systems land.

---

## 2. Room / space layout (graybox-buildable)

Single connected sequence of boxy rooms joined by doorway gaps + sliding doors (exactly the `buildTestLevel` +
`buildDoorAndButton` pattern, repeated). Coordinates are illustrative meters, +Y up, room floors at y=0, walls 3 m
tall (matches the S2/S4 graybox convention; a fully-open door pokes slightly above 3 m — acceptable graybox).
Player spawns at feet height in the cell.

```
                                                 [ELEVATOR]  (win trigger)
                                                     |  Door E (opens on boss death)
   +------------+   Door A   +-------------+  Door B  +-----------+  Door C  +--------------+
   |  CELL      |==(bent)===>|  CORRIDOR   |=========>|  ARMORY   |=========>| CHECKPOINT   |
   | (spawn)    |  ripped    |  (guards +  | button   | (pistol   | locked   | (guards)     |
   | pod, equip |  cell door |   drone)    |          |  pickup)  | until    |              |
   +------------+            +-------------+          +-----------+ armed    +------+-------+
       strength                                                                    | Door D (auto-trigger)
       discovery                                                                   v
                                                                          +-----------------+
                                                                          |  BOSS ARENA     |
                                                                          |  (Martinez)     |
                                                                          +-----------------+
```

| Room | Approx size | Contents |
|---|---|---|
| **Cell** | 6×3×6 m | Player spawn; "medical pod" prop; "medical equipment" prop (strength-discovery target); cell-bars prop forming Door A's barrier. |
| **Corridor** | 16×3×6 m | 2 Security Guard monsters + 1 Surveillance Drone monster spawn on alarm. Door B + button at the far end. |
| **Armory** | 8×3×8 m | Energy-pistol pickup (Tag::Weapon). Door C (to checkpoint) is **locked until armed**. |
| **Checkpoint** | 12×3×8 m | 1-2 more guards. Door D leads down/across to the boss arena (auto-trigger volume). |
| **Boss Arena** | 14×3×14 m | Chief Martinez (a tougher monster). Door E (to elevator) opens only on his death. |
| **Elevator** | 3×3×3 m | Trigger volume = level complete. |

All geometry = procedural boxes with distinct graybox tints per surface (per `docs/LEVEL_GEOMETRY.md`), each with a
static Jolt collision mesh, as `buildTestLevel` already does. Reuse `buildTestLevel` for the cell, then append rooms.

---

## 3. Beat-by-beat objective sequence (spawn → complete)

| # | Beat | Trigger | Effect | Engine primitive(s) |
|---|---|---|---|---|
| 0 | **Spawn / awaken** | level load | Player capsule spawns in Cell; HUD shows objective *"Escape the detention cell"*. Brief intro text. | `Player::spawn`, `Hud` (text), `buildTestLevel` |
| 1 | **Discover strength** | player near "medical equipment" prop (radius test) | Prop hidden/"destroyed"; objective text + (later) dialogue line. | distance check like `shouldArm`; Entity `visible=false` |
| 2 | **Bend bars / rip cell door** | player presses *use* aimed at the cell-bars/Door A button | **Door A opens** (portcullis slide), passage to Corridor clears. | `tryUse` → `DoorSystem::startOpening` (`door.h`) |
| 3 | **Alarm + enemies spawn** | Door A reaches Open | Spawn 2 Guards + 1 Drone in Corridor; objective → *"Find weapons in the armory"*; (later) alarm SFX + emergency tint. | `MonsterSystem` (multi-instance), HUD text |
| 4 | **Fight through corridor** | combat | Player must be armed to shoot — but isn't yet, so **melee/avoid** to the armory, OR design Door B to be a strength/button door reachable without killing. | `MonsterSystem::fire` gated by `WeaponSystem::hasWeapon()` |
| 5 | **Open Door B → Armory** | *use* on Corridor button | Door B opens. | `buildDoorAndButton` + `tryUse` |
| 6 | **Pick up pistol** | walk within `kPickupRadius` of pickup | `hasWeapon=true`; viewmodel appears; pickup hidden; objective → *"Reach the elevator to Floor 2"*. | `WeaponSystem::buildWeaponPickup` / `update` / `drawViewmodel` |
| 7 | **Unlock + open Door C** | armed (or *use*) | Door C (locked-until-armed) opens to Checkpoint. | door logic + a *locked* flag (**missing**, see §6) |
| 8 | **Clear checkpoint** | combat | Shoot 1-2 guards with the pistol. | `MonsterSystem::fire` |
| 9 | **Enter boss arena (Door D)** | player crosses arena trigger volume | Spawn **Martinez**; (later) lock the arena behind the player; boss music. | trigger volume (**missing**, see §6) + `MonsterSystem` |
| 10 | **Defeat Martinez** | boss HP ≤ 0 | Martinez dies (death pop); **Door E unlocks + opens** toward elevator; objective → *"Take the elevator"*. | `MonsterSystem` death → open door E |
| 11 | **Reach elevator (WIN)** | player enters elevator trigger | **Level complete** (log + fade/quit-to-next stub). | trigger volume (**missing**) + completion hook |

**Win condition:** player enters the elevator trigger volume **after** Martinez is dead. (Minimum viable win for the
first build can be: *Door E open → step through* even before a full elevator transition exists.)

---

## 4. Doors / buttons / pickups / enemies (concrete entity list)

- **Doors (DoorSystem):** A (cell, strength/button), B (corridor→armory, button), C (armory→checkpoint, locked-until-armed),
  D (checkpoint→arena, auto-trigger), E (arena→elevator, opens on boss death). All built via the `buildDoorAndButton`
  pattern (door box fills a doorway gap; rises to open).
- **Buttons (Tag::Button):** one per manually-opened door (A, B). C/D/E open by game state, not a wall button.
- **Pickups (Tag::Weapon):** Energy Pistol pickup in the Armory (`WeaponEnergyPistol.glb`, already loadable).
- **Enemies (Tag::Monster):** Corridor = 2 Security Guards + 1 Surveillance Drone; Checkpoint = 1-2 Guards;
  Arena = **Chief Martinez** (boss-tier: more HP, faster). For the first build, all use the existing single-monster
  pattern instanced N times; visuals can reuse `alien_crawler.glb` / `EnemyOccupationTrooper777.glb` until guard/drone
  models are converted.

---

## 5. Mapping each beat to existing primitives (what works today)

| Need | Existing primitive | Notes |
|---|---|---|
| Boxy rooms + collision | `buildTestLevel` (`level.h`) | Reuse for the cell; extend to append more rooms. |
| First-person move/look/jump | `Player` (`player.h`) | Walk speed 5 m/s, sprint 8 (per `ASSET_INVENTORY.md`). |
| Press button → open door | `tryUse` + `DoorSystem` + `buildDoorAndButton` (`door.h`) | Exactly beats 2, 5. |
| Weapon pickup + viewmodel | `WeaponSystem` (`weapon.h`) | Beat 6; `WeaponEnergyPistol.glb` ready. |
| Shoot enemy, damage, death | `MonsterSystem` (`monster.h`) | Beats 4/8/10; `fire()` gated on `hasWeapon()`. |
| Crosshair / FPS / console | `Hud` (`hud.h`) | Crosshair + dev console exist today. |
| Physics, raycasts, triggers-by-distance | Jolt via `IPhysicsWorld` | Distance tests already used by `shouldArm`. |

Roughly **60-70%** of Level 1 is buildable with today's primitives (the slice spine). The rest is the gap list below.

---

## 6. What's missing (build-next list, in priority order)

1. **Multi-entity content authoring.** Today's systems are largely single-door / single-weapon / single-monster.
   Need: a small **level descriptor** (or repeated builder calls) to place *N* doors, *N* monsters, *N* pickups, plus
   a `DoorSystem`/list of monsters the main loop iterates. *(Smallest, unblocks everything else.)*
2. **Objective system + HUD objective text.** A tiny ordered objective list (`set/complete`) and a HUD line ("Escape the
   detention cell" → …). `Hud` can draw text already; needs a state holder. *(Maps to the bible's `SetObjective`/`CompleteObjective`.)*
3. **Trigger volumes / interaction zones.** A box trigger that fires once when the player enters (strength-discovery,
   boss-arena entry, elevator win). Jolt trigger callbacks (`setTriggerCallback`, noted as the "proper future path" in
   `weapon.h`) or a per-frame AABB test on the player.
4. **Lockable doors + game-state door opens.** Doors C/E need a **locked** flag and "open on event" (armed / boss-dead),
   not just a wall button. Small extension to `Door`/`DoorSystem` (the bible's `Door.Lock()/Unlock()/Open()`).
5. **Multiple monster types + a boss variant.** Distinct stats/behaviors for Guard vs Drone vs Martinez (HP, speed,
   ranged-vs-melee, a 2-phase boss). Today `MonsterSystem` is one alien with fixed HP/chase. Needs per-type params and
   ideally a spawn-on-event call.
6. **Enemies that damage the player + player health/death.** No player HP yet. Need player health, enemy attacks, a
   health HUD element, and a death/respawn-to-checkpoint path.
7. **Super-strength melee verb.** Punch / charged super-punch / grab-throw, and **"brute-force a door"** (loud → alert).
   Beats 1-2 currently fake strength via a button; the real verb is a melee/interaction system.
8. **"Win / level complete" hook.** A clean level-complete state (fade, load-next, or quit stub). Today the slice just runs.
9. **Audio (deferred to M9).** Alarm, door SFX, gunshot, pickup chime, boss music — all assets already cataloged in
   `docs/ASSET_INVENTORY.md`; wiring waits on the M9 audio backend.
10. **Cosmetic later:** emergency lighting/flicker, the actual elevator ride/transition, real guard/drone GLBs (convert
    from FBX), dialogue/subtitle display.

> **Recommended first build (thinnest playable):** items 1-3 + reuse of everything in §5 = a real EFLZ Level 1 you can
> walk: spawn in cell → (trigger) strength → button opens Door A → guards spawn → button opens Door B → grab pistol →
> shoot through to an arena → kill "Martinez" monster → Door E opens → elevator trigger = win. Items 4-8 then upgrade
> it from "slice in EFLZ clothing" to genuine first level.

---

## 7. Acceptance tests (observable, no internal structure)

1. **T1 — Spawn & escape cell:** load Level 1; player spawns in the cell; using the cell button opens Door A and the
   player can walk into the corridor.
2. **T2 — Alarm spawn:** once Door A is open, ≥2 guard monsters + 1 drone monster exist in the corridor.
3. **T3 — Arm:** walking onto the armory pickup sets armed = true and shows the viewmodel; the pistol can now deal damage.
4. **T4 — Locked gate:** Door C does **not** open before the player is armed; opens after.
5. **T5 — Boss gate:** Door E stays closed until Martinez HP reaches 0, then opens.
6. **T6 — Win:** entering the elevator trigger after Martinez is dead reports "level complete" exactly once.
7. **T7 — Objective flow (once §6.2 lands):** the objective text advances cell → armory → elevator in order.

(T1-T3, T5, T6 are reachable with §5 primitives + the §6.1/§6.3 additions; T4/T7 need §6.4/§6.2.)

---

## 8. Notes for the implementer

- Keep the geometry as graybox boxes; do **not** block on converting FBX kit art. The ModularSciFi door/wall meshes
  (`docs/ASSET_INVENTORY.md` §6) are the *eventual* skin, not a prerequisite.
- Reuse the **headless self-test** pattern (`runInteractSelfTest`, `runPickupSelfTest`, `runCombatSelfTest`): add a
  `--test-level1` that asserts T1-T6 with synthetic input and no window/Vulkan.
- The boss is, mechanically, "a monster with more HP that opens a door on death." Phases (cover, summons, cyberblade)
  are the bible's design (`bestiary` Floor 1 Boss) and can be layered on once §6.5 lands — do not over-build first pass.
- Where the bible conflicts (Martinez on Floor 1 vs Floor 2), this spec follows the **implemented**
  `Floor1_AwakeningManager.cs` (Martinez = Floor 1 boss, elevator = level exit). See `docs/EFLZ_DESIGN.md` §7.
- Tuning targets (HP, damage, speeds) live in `docs/EFLZ_DESIGN.md` §4-5 and `ASSET_INVENTORY.md` §5; treat as placeholders.
