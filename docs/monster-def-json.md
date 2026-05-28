# `monster_def.json` — data-driven roster loader (proposal)

**Status:** PROPOSAL · drafted by **i5000** (canon-aliens lane) for the monster.* /
engine lane owner · 2026-05-27

**Goal:** migrate the four static C++ roster tables — `monsterDefs()` (Act-1
EnemyType), `bossDefs()` (Act-1 BossType), `act2EnemyDefs()` (Act-2 enemies +
bosses), `canonAlienDefs()` (Davis-Puthoff canon — just shipped on `feat/canon-aliens`) —
into a single, hot-editable **`data/monster_def.json`** that maps cleanly to the
existing `MonsterSystem::Tuning` struct. Existing C++ tables and the loader
**coexist** until the loader is gate-green; nothing breaks.

---

## 1 · Motivation

Today's roster is **C++ source data**: every stat tweak (HP, damage, strafe bias)
is a code edit + full rebuild + re-gate. With ~25 active rows across four tables
and more landing per-act, that's high friction for a content-iteration loop.

A JSON manifest lets us:

- **Hot-iterate** stats without recompiling (the headless `--test-*` flags still
  validate the loader → identical behaviour).
- **Single source of truth** — one file to grep for "what HP does the Saurian
  Warlord have today" instead of three.
- **Modder + tool surface** — designer GUIs / spreadsheets can emit/edit JSON.
- **Save-format friendliness** — the loader is the path to versioned content
  packs later (`monster_def.v2.json`, with a small migration shim).

Today's static tables remain valid; this is an **additive load path**, not a
breaking change.

---

## 2 · JSON schema

One file at the repo root: **`data/monster_def.json`**. A top-level object with a
schema version and a flat array of rows; each row carries its Act-bucket as a
tag (so a single file replaces the four tables without losing the per-act
namespacing). Field names mirror the C++ `MonsterSystem::Tuning` struct exactly
(camelCase, identical types) — the loader can use a generated `from_json` shim
and the schema is reflective.

```jsonc
{
  "schemaVersion": "1.0",
  "rosters": {
    "act1Enemy":    { "ids": ["DominionTrooper","Verthani","Illuminated","BlueSynth"] },
    "act1Boss":     { "ids": ["DrChen","FailedExperiment7","AlienOverseer"] },
    "act2Enemy":    { "ids": ["SalvariAlly","NativeDesertFauna","MutatedScientist",
                              "MutatedFlora","SurfacePursuitDrone"] },
    "act2Boss":     { "ids": ["MemoryHunter","TheSiren","BreederQueen",
                              "GarrisonCommander"] },
    "canonAlien":   { "ids": ["SaurianSoldier","SaurianWarlord","GreyTasked",
                              "NordicSteward","MantisArbiter"] }
  },
  "rows": [
    {
      "roster":  "act1Enemy",
      "id":      "DominionTrooper",
      "name":    "Dominion Trooper",
      "tuning": {
        "hp": 100,
        "chaseSpeed": 2.5,
        "modelScale": -1.0,
        "tint":            [1.0, 1.0, 1.0, 1.0],
        "modelFile":       "",
        "modelDirOverride":"",
        "standUpZtoY":     false,
        "type":            "Guard",
        "damage":          8,
        "attackRange":     1.9,
        "attackCooldown":  1.1,
        "attackWindup":    0.25,
        "ranged":          false,
        "standoff":        6.0,
        "flyer":           false,
        "aiStrafeBias":    -1.0,
        "phase2Frac":      0.66,
        "phase3Frac":      0.33,
        "phase2SpeedMul":  1.35,
        "phase2DamageMul": 1.4,
        "phase3SpeedMul":  1.7,
        "phase3DamageMul": 1.8,
        "phase2ScaleMul":  1.15,
        "phase3ScaleMul":  1.3,
        "phase3SummonCount": 2,
        "hasCureOption":   false,
        "memoryFlashTime": 0.0,
        "memoryFlashDamageMul": 1.0,
        "startAllied":     false,
        "copyFeintPhase":  0,
        "escapeTimerSeconds": 0.0
      }
    },
    {
      "roster": "canonAlien",
      "id":     "SaurianWarlord",
      "name":   "Saurian Warlord",
      "tuning": {
        "hp": 540,
        "type": "Boss",
        "damage": 18,
        "attackRange": 2.3,
        "chaseSpeed": 2.75,
        "aiStrafeBias": 0.10,
        "modelScale": 1.20,
        "tint": [0.45, 0.40, 0.25, 1.0],
        "phase2Frac": 0.66,
        "phase3Frac": 0.33,
        "phase2SpeedMul": 1.30,
        "phase2DamageMul": 1.40,
        "phase3SpeedMul": 1.60,
        "phase3DamageMul": 1.70,
        "phase3SummonCount": 2,
        "memoryFlashTime": 1.2,
        "memoryFlashDamageMul": 1.5
        // adaptiveHideResist + adaptiveHideDurationSec land once the engine
        // extension PR lands (see docs/canon-aliens-adaptive-hide.md).
      }
    }
    // ... ~25 rows total when all four tables migrate.
  ]
}
```

**Schema notes:**

- **Field-by-field parity with `MonsterSystem::Tuning`.** No JSON-specific renames; a missing field defaults to the struct's in-source default (so most rows are short — only override what differs).
- **`type`** is a string enum mirroring `MonsterType` (`"Guard"`, `"Drone"`, `"Boss"`).
- **`tint`** is `[r,g,b,a]` (4 floats).
- **`roster` + `id`** together are the primary key; loader builds `std::unordered_map<std::string, MonsterSystem::Tuning>` keyed by `"<roster>:<id>"`.
- **`schemaVersion`** is a plain string the loader pins; an older binary refusing a newer schema is the bug-prevention. v1.0 = "fields match the v1.0 Tuning struct snapshot."

---

## 3 · C++ loader sketch

A thin reader producing identical `Tuning` instances. Lives in `engine/asset/`
(engine TU — JSON parsing is engine-side, not app-side).

```cpp
// engine/asset/MonsterDefLoader.h  (NEW)
#pragma once
#include "monster.h"     // MonsterSystem::Tuning
#include <string>
#include <unordered_map>

namespace x3::asset {
struct MonsterDefLoader {
    // Load + parse data/monster_def.json (path is repo-relative; resolved by
    // asset_root.h's repoRoot()). Returns a map keyed by "<roster>:<id>" —
    // e.g. "canonAlien:SaurianWarlord" -> populated Tuning.
    // Empty map + a logged warning on parse failure (loader never throws).
    std::unordered_map<std::string, x3::game::MonsterSystem::Tuning>
    load(const std::string& path);

    // Convenience: look up a single row. nullopt iff missing/parse failed.
    std::optional<x3::game::MonsterSystem::Tuning>
    lookup(const std::string& roster, const std::string& id);
};
} // namespace x3::asset
```

**Dependency:** add **`nlohmann-json`** to `vcpkg.json` — header-only, well-vetted,
~1 LoC build cost. It's the standard for "thin JSON in modern C++ + Vulkan
projects." Alternative: a hand-rolled flat-keys parser if the lane is allergic
to a new dep (~150 lines, but rejected here because every future data file
benefits from a real JSON parser).

```cpp
// engine/asset/MonsterDefLoader.cpp  (NEW, ~120 lines)
//   * read file -> nlohmann::json
//   * pin schemaVersion == "1.0"
//   * iterate rows[]; for each:
//       - build a default Tuning (defaults preserved on missing fields)
//       - read each field via j.contains("hp") ? j["hp"].get<int>() : keep default
//       - map "type" string -> MonsterType enum
//       - map "tint" array -> float[4]
//   * key = roster + ":" + id; emplace into the result map
//   * on any field type mismatch, log + skip the row (don't abort the whole load)
```

**Caller usage in monster.cpp**:

```cpp
// Current path (still works after this lands):
const MonsterDef& d = monsterDef(EnemyType::DominionTrooper);
MonsterSystem::Tuning t = d.tuning;

// New path (additive — opt-in by call site):
auto loader = x3::asset::MonsterDefLoader{};
auto rows   = loader.load(x3::asset::repoRoot() + "/data/monster_def.json");
auto tuning = rows.at("act1Enemy:DominionTrooper");
```

The static `monsterDefs()` / `bossDefs()` / `act2EnemyDefs()` / `canonAlienDefs()`
tables stay byte-identical for one release cycle (back-compat). New code can opt
into the loader; old code keeps working.

---

## 4 · Backward-compatibility plan

The loader is **additive — both paths coexist** until the loader proves out.

**Phase A (this PR — loader lands):**

1. Add `engine/asset/MonsterDefLoader.{h,cpp}` + `data/monster_def.json` (one row
   per existing static-table row — initially **machine-generated from the static
   tables** to guarantee parity).
2. Add `--test-monster-def-json` headless self-test — load the JSON, compare each
   row byte-for-byte against the matching static-table row (a small `Tuning`
   equality helper). Gate must be green for any future change.
3. **No call-site changes.** Static tables remain canonical; loader is dormant
   except in the new test.

**Phase B (consumer migration, separate PR per consumer):**

- Switch one consumer at a time to the loader. Existing self-tests (`--test-bestiary`,
  `--test-bosses`, `--test-act2bosses`, `--test-canonaliens`) continue to pass —
  they read from whichever table the consumer chose.

**Phase C (decommission, eventual):**

- Once every consumer reads from the loader and `--test-monster-def-json` is
  green for ~2 release cycles, delete the static-table `build*Defs()` functions
  and update `MonsterDef` / `BossDef` / `Act2EnemyDef` / `CanonAlienDef` to be
  pure aliases of the loader's result.

---

## 5 · Self-test — `--test-monster-def-json`

Lives in `engine/asset/MonsterDefLoader.cpp` next to the loader (or in
`monster.cpp` to share `Tuning` equality logic). Headless, no Vulkan.

Asserts:

1. `data/monster_def.json` loads (no parse error; `schemaVersion == "1.0"`).
2. **Row count matches** the union of static tables (`monsterDefs.size() +
   bossDefs.size() + act2EnemyDefs.size() + canonAlienDefs.size()`).
3. **For every static-table row**, the loader has a matching key
   (`"<roster>:<id>"`).
4. **Field-by-field equality** between the loader's Tuning and the static-table
   Tuning — uses a `tuningEq(a, b)` helper covering every field (HP, damage,
   type, ranged, chaseSpeed, attackRange, attackCooldown, attackWindup, standoff,
   flyer, aiStrafeBias, all phase fields, memoryFlash fields, startAllied,
   copyFeintPhase, escapeTimerSeconds, tint[4], modelScale, modelFile,
   modelDirOverride, standUpZtoY). Float compare with epsilon 1e-6.
5. **Unknown-field tolerance.** Add a row with a junk field (`"foo": 42`) — the
   loader skips the junk + still loads the row with sensible defaults.
6. **Missing-field defaults.** A minimal row (just `id` + `roster` + `hp`)
   produces a Tuning identical to a default-constructed `Tuning` with `.hp`
   overridden.

Wire into `main.cpp` as `--test-monster-def-json` (mirror `--test-bestiary`
pattern). Gate exit non-zero on any FAIL.

---

## 6 · Risks / open questions

- **Schema versioning.** Bump `schemaVersion` whenever a new Tuning field
  arrives (e.g., when Adaptive-Hide lands, jump to `"1.1"` + a one-line loader
  migration: read the new field with a default if absent).
- **JSON parse cost.** Loading ~25 rows is microseconds; called once at app
  start. Not a hot path.
- **File location.** `data/monster_def.json` lives at the repo root next to
  `vcpkg.json` (similar role — a manifest, not source code). Resolved via
  `asset_root.h::repoRoot()`.
- **Editor support.** With nlohmann-json the loader emits readable parse errors
  on a malformed file; the data file should pass a CI lint (a tiny pre-commit
  hook running `jq . data/monster_def.json` would catch typos).
- **Hot-reload?** Out of scope here; that'd be a separate `loader.reload()` +
  a file-watcher hook on debug builds. Easy follow-up once the static load lands.

---

## 7 · Phasing summary

| Phase | Owner | Scope | Gate |
|---|---|---|---|
| **A — Loader + JSON file + test** | monster.* / engine lane | `engine/asset/MonsterDefLoader.{h,cpp}` + `data/monster_def.json` + `--test-monster-def-json` + nlohmann-json vcpkg dep | `monster-def-json: N/N passed`, all existing `--test-*` still green |
| **B — Migration** | per-consumer (multiple lanes) | flip each callsite over to the loader; static tables stay byte-identical | every existing self-test still green |
| **C — Decommission** | engine lane | delete the `build*Defs()` functions; `MonsterDef` etc. become thin wrappers over the loader | full sweep gate green |

Estimated Phase A size: ~250 lines new code (loader 120, test 80, JSON gen ~25, vcpkg.json + CMake wiring ~5), zero existing-file edits beyond CMake + vcpkg.json. Mergeable in one PR.

---

— *i5000 (desert lane); paired with `docs/canon-aliens-adaptive-hide.md`. Pinging the engine channel.*
