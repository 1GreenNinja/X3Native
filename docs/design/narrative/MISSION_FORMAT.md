# EFLZ Mission Format — `x3.mission/1`

A compact, branching **mission/quest data** format for Act-2 open-world content. It is the
sibling of `x3.chattree/1` (see `NPC_CHAT_TREE_FORMAT.md`) and **shares its exact
condition/effect vocabulary** (the `story_ops` set in §3 of that doc). A mission is just an
*objective graph* gated by the same `if`/`fx` that gate dialog nodes and choices, so the
engine's mission runner reuses the chat-tree's `app/story_ops` evaluator wholesale — no new
op enum, no new moral state.

> **Why this format exists.** The chat-tree format already proved that *story side-effects*
> (karma, flags, `follow`, `fire→Lua`, items, `rel`) can ship as data. Missions are the same
> idea one level up: a list of **objectives** with **gates** and **branch/fail/retry** edges,
> plus **kill-counter bridges** to the existing combat AI (drone/enemy kills the host already
> counts). Default-inert: a mission does nothing until the host registers it and fires its
> `start` trigger.

## Design constraints honored (same spine philosophy as chat trees)
- **A mission always has a linear "spine."** Walking the default edge of every objective
  (the first `next` / the unconditional `then`) yields a playable 4–8 beat quest the engine
  can run today by ignoring the optional branch/fail edges. Branches only *add*.
- **Every gate is a real `story_op`.** `if`/`fx` arrays use only the ops documented in
  `NPC_CHAT_TREE_FORMAT.md §3`. No mission-only ops are invented. The two purely-structural
  additions below (`counter`, `objectives`) are mission *structure*, not story state — they
  carry no moral meaning and serialize as plain ints/strings.
- **Counters bridge to combat, not new C++.** A `counter` reads a host-tracked integer the
  engine already increments (enemy kills, drones converted, crystals scanned, pods
  destroyed). The mission system polls `{"counter": ["id", n]}` exactly like the chat-tree
  `{"chance": x}` op polls a hashed roll — a read, never a write to combat code.
- **All world wiring is `fire`.** Spawning the boss arena, opening a gate, marking a map
  ping, granting a vehicle — every world verb leaves through `{"fire": "event", "args":{…}}`
  into `IScriptSystem::fire`, the proven `secret_room.lua` path. The mission file names the
  event; the Lua/host owns the verb. This keeps missions pure data.

## 1. File shape

```json
{
  "format": "x3.mission/1",
  "id": "act2_help_the_injured_salvari",   // stable id (flag namespace + save key)
  "title": "The Mourner's Debt",            // HUD/journal title
  "act": 2,
  "level": 10,                              // canon level number (Act2Level), or a list
  "location": "Crystalline Desert Depths",  // human label, matches WORLD_STRUCTURE
  "summary": "One sentence for the journal.",
  "give":  [ {"flag": "..."} ],             // OPTIONAL: requirements to OFFER the mission
  "start": "o0",                            // first objective id
  "objectives": [ ...Objective ],
  "_hooks": "Free prose: which canon threads / branches this hooks into.",
  "_voice": "Free prose: tone notes."
}
```

`level` may be a single int or an array (a mission that spans levels, e.g. a Club 1127 hub
quest that resolves on the surface). `give` (mission-level) is the offer gate — when ALL its
conditions pass, the mission becomes available; omit for always-available.

## 2. Objective

```json
{
  "id": "o1",
  "text": "Reach the Salvari camp entrance.",   // journal line shown while active
  "if":   [ {"flag": "..."} ],                   // gate: objective is SKIPPED if it fails
  "else": "o2",                                  // where to go when `if` fails (skip-to)
  "on_enter": [ {"fire": "map_ping", "args": {"id": "salvari_camp"}} ],  // fx when it activates
  "complete_when": [ {"counter": ["salvari_freed", 3]} ],  // conditions to auto-complete
  "fx":   [ {"karma": 3}, {"set": "salvari.met"} ],        // fx applied ON completion
  "next": "o2",                                  // next objective (the spine)
  "branch": [                                    // OPTIONAL conditional re-routes
    { "if": [ {"flag": "mole.suspected"} ], "next": "o2_mole" }
  ],
  "fail": {                                       // OPTIONAL fail edge
    "when": [ {"flag": "salvari.camp_overrun"} ],
    "fx":   [ {"karma": -2} ],
    "next": "o_fail",                             // or "retry" to re-arm this objective
    "retryable": true
  },
  "timer": { "seconds": 90, "on_expire": "o_fail" }  // OPTIONAL countdown (tuning placeholder)
}
```

### Objective edge semantics (the runner's order of evaluation each tick)
1. If the objective's `if` fails → jump to `else` (or `next` if no `else`); its `fx` do NOT fire.
2. Else the objective is **active**: apply `on_enter` once.
3. If any `fail.when` passes → apply `fail.fx`, then go to `fail.next` (`"retry"` re-arms
   this objective; `retryable:true` allows the player to re-trigger after a fail node).
4. Else if `timer` runs out → go to `timer.on_expire`.
5. Else if **all** `complete_when` pass (or there is no `complete_when`, in which case the
   objective completes when the host fires `mission_objective_done {mission,objective}`) →
   apply `fx`, evaluate `branch` top-to-bottom (first passing `if` wins → its `next`),
   else follow `next`. `"next": "end"` ends the mission (success).
6. `"next": "fail"` / a `fail` node ending the mission marks it failed.

A mission **ends** when an objective routes to `"end"` (success, applies any mission-tail fx
on that objective) or to a node that has `"end": "fail"` in its `fx` (failure). Missions are
**resumable** from the active objective via the save lane (the mission id + active objective
id + counters serialize alongside `StoryFlags`).

## 3. Counters (the only new, non-moral state)

`counter` is a host-tracked integer keyed by string id. The host already counts the things
that matter; the mission only **reads** them:

| counter id (convention) | incremented by | used for |
|---|---|---|
| `kills.<type>` | enemy death of that roster type | "thin the patrol" objectives |
| `kills.any` | any enemy death in the active mission's area | clear-the-zone beats |
| `salvari_freed` | a captive Salvari converted to allied | rescue tallies |
| `crystals_scanned` | bio-mesh scan interact on a singing-crystal node | survey beats |
| `drones_converted` | a drone flipped to allied (the Crazy-Drone/Sarah-hack verb) | drone-army beats |
| `evidence.<id>` | an evidence collectible taken | dossier assembly |

Conditions: `{"counter": ["id", n]}` = counter ≥ n. `{"counter_lt": ["id", n]}` = counter < n.
Effects: `{"counter_set": ["id", n]}` / `{"counter_add": ["id", n]}` (used sparingly — most
counters are owned by combat/interact, not by missions). **Counters carry no karma/moral
weight**; they are quest plumbing and serialize as plain ints.

## 4. Everything else is `story_ops`

All `if`, `complete_when`, `fail.when`, `branch[].if`, and `fx`/`on_enter` entries draw ONLY
from `NPC_CHAT_TREE_FORMAT.md §3`:

- **Conditions:** `flag`, `karma_gte/lte`, `humanity_gte`/`love_gte`/`trust_gte`/`mercy_gte`/
  `redemption_gte`, `timeline`, `girl_saved`/`girl_lost`, `item`, `rel_gte`, `chance`, `lua`,
  plus the structural `counter`/`counter_lt`. `{"any":[…]}`, `{"not":{…}}` compose them.
- **Effects:** `karma`/`humanity`/`love`/`trust`/`mercy`/`redemption` (±n), `set`/`clear`,
  `fire`, `give`/`take`, `follow`, `rel`, `ally`, `end` (host verb), plus structural
  `counter_set`/`counter_add`. **`follow`/`ally` reuse `RescueSystem`/`onAllyJoined` exactly
  as the chat trees do.**

## 5. Mission ↔ chat-tree handshake

Missions and trees talk through flags and fires, never through each other's internals:
- A mission's `on_enter` can `{"fire":"dialog_open", "args":{"npc":"vesper","tree":"hub"}}`
  to start a conversation; the tree's `fx` set a flag the mission's `complete_when` reads.
- A chat-tree `sidequest` node that `{"set":"aria.sq_active"}` is the **offer gate** of the
  matching mission (`"give":[{"flag":"aria.sq_active"}]`); the mission's success `fx` set
  `aria.sq_done`, which the tree's `sq_done` node reads. The pack already uses these flags
  (see `NPC_CHAT_TREE_FORMAT.md §7`); the missions here reuse them, never reinvent them.

## 6. Flag glossary additions (Act-2, namespaced)

Builds on `NPC_CHAT_TREE_FORMAT.md §7`. New Act-2 flags introduced by THIS pack (all set
through `fx`/dialog, never hard-coded):

`act2.surfaced` (emerged onto Keth'zar), `salvari.met` / `salvari.allied` / `kthara.met`,
`warlord.dead` / `warlord.spared`, `mantis.bargain` / `mantis.refused`,
`quartermaster.ledger_run` / `quartermaster.confronted`, `mole.suspected` (see Carrier arc),
`carrier.cured` / `carrier.weaponized` / `carrier.killed`,
`cradle.core_seen` (Act-2 deepening of `cradle.known`),
`disc.found` (the drowned undersea base), `disc.root_tapped`,
`biomesh.scan` (the substrate-scan upgrade earned), `architect.station` (third room found),
`stormrunner.fueled` / `stormrunner.crew` / `stormrunner.launched` (the Act-2 climax chain).

## 7. Validation

`tools/check_missions.py` parses every `missions/*.json`, asserts `format ==
"x3.mission/1"`, that `start` and every `next`/`else`/`branch[].next`/`fail.next`/
`timer.on_expire` resolves to an objective id in the file (or the keywords `end` / `fail` /
`retry`), that objective ids are unique, and that every op used in any `if`/`fx`/etc. is in
the **known story_ops + structural set** (so a typo'd op fails the gate, not silently). It is
the mission-side mirror of `tools/check_chattrees.py`.
