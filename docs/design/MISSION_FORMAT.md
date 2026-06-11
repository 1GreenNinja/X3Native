# x3.mission/1 — Missions as Data

The GTA mission loop, authored as JSON and executed by a runner
(`app/mission.{h,cpp}`). A mission is an ordered/branching list of **stages**;
each stage owns the HUD objective line, effects on entry/completion, and a
condition list that advances it. The 100-level arc becomes 100 docs the fleet
can author in parallel — no C++ per mission.

Proof doc: `missions/level1.mission.json` replicates the live Level-1 beat
progression exactly (`--test-mission` asserts the equivalence). Runtime gate:
cvar **`g_missiondoc`** (default `0` = the doc is not even loaded; `1` = the
doc drives the objective line through the `ObjectiveSystem` free-text lane).

## One ops vocabulary (shared with x3.chattree/1)

Conditions (`advance_when` / `fail_when` / `branch.if`) and effects
(`on_enter` / `on_complete` / `on_fail`) are **the same ops the chat trees
use** — `ChatCond` / `ChatFx` in `app/story_ops.h`, evaluated by
`evalChatConds()` and applied by `applyChatFx()`. There is deliberately no
second condition system. Anything the dialog format can test or do, a mission
can too:

| Conditions (`if`-ops)                 | Effects (`fx`-ops)                  |
|---------------------------------------|-------------------------------------|
| `{"flag": "name"}`                    | `{"set"/"clear": "flag"}`           |
| `{"karma_gte"/"karma_lte": n}`        | `{"karma"/"humanity"/...: ±n}`      |
| `{"humanity_gte"/"trust_gte"/...: n}` | `{"fire": "event", "args": {...}}`  |
| `{"timeline": ["hero", ...]}`         | `{"give"/"take": "item"}`           |
| `{"girl_saved"/"girl_lost": "aria"}`  | `{"rel": ["npc", stage]}`           |
| `{"item": "id"}` / `{"rel_gte": [..]}`| `{"follow": true}` / `{"ally": true}`|
| `{"chance": 0.3}` / `{"lua": "fn"}`   | `{"end": "verb"}`                   |
| `{"any": [...]}` / `{"not": {...}}`   |                                     |

Effects route through the existing sinks: `TimelineState::adjust*`,
`StoryFlags`, `IScriptSystem::fire` (the D14 Lua layer — doors, spawns,
trapdoors, objectives are reachable via `x3.*` bindings from a fired script),
the host follow callback. Rewards are just fx: `{"give": "keycard_red"}`,
`{"karma": 5}`, `{"set": "l1.bonus.earned"}`.

## Document shape

```json
{
  "format": "x3.mission/1",
  "id": "level1",                  // stable id — the flag namespace
  "title": "Display Title",        // ASCII-folded at load
  "start": "stage_id",             // optional; default = first stage
  "stages": [ { ...stage... } ]
}
```

### Stage

```json
{
  "id": "boss",                          // unique; "end" is reserved
  "objective": "Defeat Chief Martinez",  // HUD line on entry ("" = keep previous)
  "on_enter":     [ fx... ],             // applied when the stage is entered
  "advance_when": [ cond... ],           // AND; EMPTY = advance immediately (fx-only stage)
  "fail_when":    [ cond... ],           // optional; checked BEFORE advance_when
  "on_complete":  [ fx... ],             // applied when advance_when passes
  "on_fail":      [ fx... ],             // applied when fail_when passes
  "next": "elevator",                    // stage id or "end" (default "end")
  "branch": {                            // used INSTEAD of `next` when present
    "if":   [ cond... ],                 // AND; empty passes
    "then": "hero_end",
    "else": "coward_end"
  },
  "fail_to": "retry_stage"               // on fail: jump here; ABSENT = mission FAILED
}
```

Runner semantics (`MissionRunner::tick()`, called once per frame — a handful
of hash lookups when nothing changed):

1. `fail_when` (if non-empty) — on pass: `on_fail` fx, then `fail_to` (or the
   mission becomes **Failed**).
2. `advance_when` — on pass: `on_complete` fx, then `branch` (if present) or
   `next`. `"end"` ⇒ **Complete**.
3. Cascades: consecutive already-satisfied stages chain in one tick (bounded).

Validation (`validateMission`): unique non-empty ids, start resolves, every
`next`/`branch.*`/`fail_to` is a real stage or `"end"`, every stage reachable
from start. Unknown cond/fx kinds are **loader errors** (closed contract,
same as chattree).

## §4 The flag bridge — how gameplay advances stages

Stages advance on **StoryFlags** (plus axes/items/Lua). Gameplay events become
flags via small adapters (`MissionEventBridge` + the Level-1 poll adapter) —
the conditions never poll game objects directly:

| Source                          | Flag set                                  |
|---------------------------------|-------------------------------------------|
| any `x3.fire` event forwarded   | `ev.<event>` (latched)                    |
| `trigger_enter {zone}`          | `trigger.<zone>` (zone = `L1Trigger` id)  |
| `terminal_code {code}`          | `code.<code>.entered`                     |
| kill counters (`onKill(type)`)  | `kill.<type>.<n>` for n = 1..count (monotonic milestones — `{"flag":"kill.guard.3"}` means "3+ kills") |
| rescues (`onRescue(who)`)       | `<who>.rescued` (the chattree glossary key) |
| item pickups                    | `{"item": "id"}` reads the StoryFlags inventory (`give` fx / host `give`) |
| reach-location                  | a trigger zone (`trigger.<id>`)           |

Level-1 poll adapter (`pollLevel1MissionFlags`, pure reads of `Level1Game`):
`l1.doorA.open`, `l1.armed`, `l1.checkpoint.clear`, `l1.martinez.dead`,
`l1.complete`, `trigger.<id>` for every fired volume, and the `hostile` /
`martinez` kill counters.

## Persistence / resume

The runner records its position **in StoryFlags** — `mission.<id>.at.<stage>`
(one marker, moved on every entry), `mission.<id>.done`, `mission.<id>.failed`.
Mission progress therefore rides the existing flags save lane
(`<checkpoint>.flags.txt`, saved/loaded next to the binary checkpoint).
`MissionRunner::resume(doc)` re-enters the recorded stage **without re-firing
its `on_enter` fx** (effects are not idempotent; the save already carries their
results), re-emits the objective text, and cascades. A `done`/`failed` mission
resumes straight to that state; no marker means a fresh `start()`.

## Authoring checklist (for fleet-parallel mission docs)

1. One file per mission: `missions/<id>.mission.json`.
2. Namespace your flags by mission/level (`l2.*`, `m_rescue.*`).
3. Advance on flags the bridges already emit where possible; new beats want a
   tiny poll/event adapter, not a new condition kind.
4. `--test-mission` must stay green; add the doc to a parse/validate sweep.
5. Side-effects beyond flags/axes/items go through `{"fire": ...}` into the
   D14 Lua layer — never a new C++ sink.
