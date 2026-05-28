# Adaptive Hide — engine-extension proposal (monster.* lane)

**Status:** PROPOSAL · drafted by **i5000** (canon-aliens lane) for the monster.* /
engine lane owner · 2026-05-27

**Companion to:** `feat/canon-aliens` (shipped) — the `SaurianWarlord` Tuning row in
`app/canon_aliens.cpp` already documents Adaptive Hide as TODO and uses the
existing **memory-flash** machinery as a temporary stand-in for the "rotate damage
type" rhythm. This proposal specifies the *real* mechanic + a minimal, additive
engine extension that ships it cleanly.

---

## 1 · Motivation

The Saurian Warlord boss design (canon-aliens) hinges on a single fight-defining
behavior: **resist whatever damage type just hit you for ~8 s, then re-evaluate.**
A player who spams one weapon stalls out; a player who *rotates* (kinetic →
energy → bio → …) keeps the damage flowing. This is the "build diversity by
encounter" lesson EFLZ wants from its bosses.

Today's engine has no concept of damage type — `MonsterSystem::fire()` and
`takeMeleeDamage()` take a plain `int damage`, and `applyDamage()` is purely
HP-arithmetic. Memory-flash (`Tuning::memoryFlashTime` + `m_flashTimer`) gives a
brief post-phase amplified-vulnerability *window*, but doesn't gate on the damage
*type*, so it doesn't enforce weapon rotation.

This proposal threads the *type* through the existing damage hooks and adds the
adaptive-resist machinery as **additive** Tuning + per-instance state. Existing
call sites stay valid; the feature is **off by default** (`adaptiveHideResist =
0.0f`); only the Warlord (and any future row that opts in) gets the behavior.

---

## 2 · Design — public API

### 2.1 New enum: `DamageType`

```cpp
// engine/core/x3_damage.h  (NEW, tiny header — pure data, no deps)
namespace x3 {
enum class DamageType : uint32_t {
    None      = 0,   // sentinel — "no last-damage" (initial state)
    Kinetic   = 1,   // pistol, shotgun, melee-blade equivalents
    Energy    = 2,   // plasma, BFG, railgun, laser
    Explosive = 3,   // rocket, grenade
    Bio       = 4,   // chemical, toxic
    Melee     = 5,   // player melee (Phase-2b super-strength punch, etc.)
    Count     = 6
};
} // namespace x3
```

*Six is generous; the player's arsenal today (pistol/shotgun/bfg/railgun/rocket +
melee) maps cleanly onto the first 5 non-None values. `WeaponSystem` declares the
mapping per weapon — one switch statement, no per-weapon files touched.*

### 2.2 Tuning fields (additive, default off)

In `app/monster.h`, **`MonsterSystem::Tuning`** struct (currently ends with
`escapeTimerSeconds` at ~line 465). Append:

```cpp
        // ---- Adaptive Hide (canon-aliens SaurianWarlord, L?? boss). Engine-
        // extension Wave 3. Boss-style "rotate damage type" mechanic: after taking
        // damage of type T, gain `adaptiveHideResist` reduction to ALL further
        // damage of type T for `adaptiveHideDurationSec` seconds. Inert by default
        // (resist == 0) so every existing row is unchanged.
        float adaptiveHideResist      = 0.0f;   // 0..1; e.g. 0.6 = 60% reduction
        float adaptiveHideDurationSec = 8.0f;   // window length; ignored if resist==0
```

### 2.3 Damage entry-points get a `DamageType` parameter (optional, defaulted)

In `app/monster.h`, `MonsterSystem::fire(...)` and `takeMeleeDamage(...)` get a
trailing optional arg. Defaults make every existing call site valid:

```cpp
    FireResult fire(const x3::phys::Vec3& eye, const x3::phys::Vec3& dir,
                    Scene& scene, x3::phys::IPhysicsWorld& physics,
                    int damage = kDamagePerShot,
                    x3::DamageType type = x3::DamageType::Kinetic);

    bool takeMeleeDamage(int damage, Scene& scene, x3::phys::IPhysicsWorld& physics,
                         x3::DamageType type = x3::DamageType::Melee);
```

`MonsterManager::fire()` mirrors the new arg + default; no other public surface
changes. `applyDamage(int*, int)` stays a pure HP rule — the *type* logic lives
one level up.

### 2.4 Per-instance state (private)

In `MonsterSystem`'s private members (`app/monster.h` ~ line 770+ block), alongside
`m_flashTimer`:

```cpp
    x3::DamageType m_adaptiveHideType   = x3::DamageType::None;
    float          m_adaptiveHideTimer  = 0.0f;   // seconds remaining
```

---

## 3 · Behavior — exact rules

On a damage event with incoming `(damage, type)`:

```
if (Tuning.adaptiveHideResist > 0 &&
    m_adaptiveHideTimer > 0 &&
    type == m_adaptiveHideType)
    damage *= (1 - Tuning.adaptiveHideResist);     // round to nearest int

apply damage (existing applyDamage path, hit-flash, etc.)

if (Tuning.adaptiveHideResist > 0):
    m_adaptiveHideType  = type;                    // latch the new type
    m_adaptiveHideTimer = Tuning.adaptiveHideDurationSec   // reset window
```

In `MonsterSystem::update(dt, ...)`:

```
if (m_adaptiveHideTimer > 0) m_adaptiveHideTimer = max(0, m_adaptiveHideTimer - dt);
```

**Read in plain English:**

| Sequence | Incoming | Apply | New state |
|---|---|---|---|
| 1st shot | Kinetic / 10 | 10 (no resist; last was None) | type=Kinetic, t=8.0 |
| 2nd shot, immediate | Kinetic / 10 | **4** (60% resist) | type=Kinetic, t=8.0 (reset) |
| 3rd shot, immediate | Energy / 10 | 10 (different type) | type=Energy, t=8.0 |
| Wait 9 s, no hits |   |   | type=Energy, t=0 |
| 4th shot | Energy / 10 | 10 (timer expired) | type=Energy, t=8.0 |
| 5th shot, immediate | Energy / 10 | **4** | type=Energy, t=8.0 |

That's the rhythm: *the player must rotate weapons (or pause) to keep damage flowing.*

### 3.1 Interaction with `memoryFlashDamageMul`

Memory-flash (`incomingDamageMul()`, ~1.5×) and Adaptive-Hide stack
**multiplicatively** — the order is irrelevant:

```
finalDamage = round(damage
                    * incomingDamageMul()            // memory-flash >1 when active
                    * (resistMatches ? (1-resist) : 1));
```

Net effect during a flash window with a matched type: `1.5 × 0.4 = 0.6` → still
reduced but less harshly, which is exactly the design intent (flash is the
"break the pattern" reward — players who *also* rotate during a flash multiply
the bonus).

---

## 4 · Implementation touch points

All in `app/monster.*` (this is the monster.* lane's call). No other files need
edits.

| File | Symbol | Approx. line | Edit |
|---|---|---|---|
| `engine/core/x3_damage.h` *(new)* | `enum class DamageType` | — | tiny new header, pure data |
| `app/monster.h` | `MonsterSystem::Tuning` close-brace | 466 | add the 2 new fields (§2.2) before `};` |
| `app/monster.h` | `MonsterSystem::fire(...)` decl | ~485 | trailing `DamageType type = Kinetic` |
| `app/monster.h` | `MonsterSystem::takeMeleeDamage(...)` decl | ~525 | trailing `DamageType type = Melee` |
| `app/monster.h` | `MonsterManager::fire(...)` decl | ~890 | trailing `DamageType type = Kinetic` |
| `app/monster.h` | `MonsterSystem` private members block | ~770 | `m_adaptiveHideType` + `m_adaptiveHideTimer` |
| `app/monster.cpp` | `FireResult MonsterSystem::fire(...)` | 432 | accept + apply resist rule + latch type/timer (§3) |
| `app/monster.cpp` | `bool MonsterSystem::takeMeleeDamage(...)` | 526 | same rule |
| `app/monster.cpp` | `MonsterSystem::update(...)` body | (existing) | tick `m_adaptiveHideTimer -= dt` |
| `app/monster.cpp` | `MonsterManager::fire(...)` | (existing) | forward the new `type` arg |
| `app/main.cpp` | the player-fire call site | (existing) | pass weapon's `DamageType` (see §4.1) |

### 4.1 WeaponSystem → DamageType mapping

In `app/weapon.h` / `weapon.cpp`, add a per-weapon `DamageType type` field on the
weapon's spec table (one line per weapon). Player's `onFire()` reads it and passes
to `MonsterManager::fire(..., damage, weapon.type)`. Suggested defaults:

| Weapon | DamageType |
|---|---|
| pistol | Kinetic |
| shotgun | Kinetic |
| plasma / BFG | Energy |
| railgun | Energy |
| rocket / grenade | Explosive |
| (any bio/poison weapon, future) | Bio |
| melee (`MeleeSystem`) | Melee |

---

## 5 · Self-test — `--test-adaptive-hide`

Add to `monster.cpp` next to `runBossesSelfTest()`. Headless (no window/Vulkan).

```cpp
bool runAdaptiveHideSelfTest() {
    // Build a HeadlessDevice + Jolt world.
    // Spawn a MonsterSystem with the SaurianWarlord tuning + adaptiveHideResist=0.6
    // + adaptiveHideDurationSec=8.0 (canon-aliens row plus the new fields once
    // the engine extension lands).

    // Helper: fire the same eye+dir at the monster's body; reuse the FireResult to
    // verify the damage actually applied (hpAfter delta).

    // Case A — first shot (no last-type): expect FULL damage.
    //   m.maxHp() - m.hp() == kDamagePerShot

    // Case B — second shot, same type, immediate: expect REDUCED.
    //   delta == round(kDamagePerShot * 0.4)

    // Case C — third shot, DIFFERENT type, immediate: expect FULL.
    //   delta == kDamagePerShot

    // Case D — fast-forward update(9.0s) (10x update(0.9s) etc.); timer expires.
    //   m.adaptiveHideTimer() == 0  (or equivalent introspection)

    // Case E — fourth shot, same type as latched: expect FULL again (window gone).

    // Case F — fifth shot, immediate same type: expect REDUCED again.

    // Case G — memory-flash interaction: if memoryFlashTime > 0 active +
    // matched-type incoming, finalDamage == round(damage * flashMul * (1-resist)).
}
```

Self-test exit non-zero on any FAIL. Wire into `main.cpp` as `--test-adaptive-hide`
(mirror `--test-bosses` pattern).

---

## 6 · Backward compatibility

- **Existing roster rows untouched.** All current `Tuning` initialisers set
  `adaptiveHideResist = 0`, so the rule is dead code for them.
- **Existing call sites untouched.** The new `DamageType` arg is *defaulted* on
  every public declaration; current `fire(damage)` and `takeMeleeDamage(damage)`
  calls keep working and behave exactly as before (the type defaults silently
  populate `Kinetic` / `Melee`, but no row resists those by default).
- **Save/load.** `m_adaptiveHideTimer` is per-instance transient runtime state,
  fine to default to 0 on load.

---

## 7 · Risks / open questions

- **Damage-type proliferation.** 6 types should be enough; if the design wants
  more (cryo, EMP, void) just extend the enum + `Count`. The map is data-driven
  in `WeaponSystem`, so adding a type is one line in the table.
- **Visual telegraph.** The original brainstorm describes the boss's scales
  *colour-shifting* to telegraph the current resist (red=kinetic, blue=energy,
  etc.). That's an *art* asset task — out of scope here; spec the *behaviour*
  + a `m_adaptiveHideType` query so the HUD can wire a colour to it later.
- **Stacking with phase damage multipliers.** Already handled — phase-mul stacks
  multiplicatively just like memory-flash (see §3.1).
- **Multiple instances.** Each `MonsterSystem` has its own `m_adaptiveHideType /
  m_adaptiveHideTimer`, so two Warlords in the same arena don't share resists.

---

## 8 · Phasing

This is a **single, small PR** for the monster.* lane (estimated ~80 lines
across `monster.h` + `monster.cpp` + the new `x3_damage.h` + the self-test +
the `WeaponSystem` per-weapon mapping). Once it lands, **i5000 updates the
`SaurianWarlord` row in `app/canon_aliens.cpp`** to set:

```cpp
    t.adaptiveHideResist      = 0.60f;   // 60% resist to last-damage type
    t.adaptiveHideDurationSec = 8.0f;    // 8-second window
```

…and the boss's "rotate damage type" rhythm is live in-engine. The encounter
content (the arena, the Grey-add summon at Phase-3, the visual colour-tint per
type) follows in subsequent PRs.

---

— *i5000 (desert lane); pinging the engine channel.*
