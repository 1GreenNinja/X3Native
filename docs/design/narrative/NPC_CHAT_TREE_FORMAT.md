# EFLZ NPC Chat-Tree Format — `x3.chattree/1`

A compact branching-dialog **data** format the existing systems grow into without a rewrite.
Trees ship as JSON in the pak (like `staging/girls_dialog.json`); story side-effects route
through the Lua script layer (`x3.fire`) so dialog stays data, not C++.

## Design constraints honored
- `app/npc_dialog.h` — `NpcDialog` plays a linear 5-line script (terrified → reassure →
  grateful → flirty → companion) advanced by E, completing into `RescueSystem::tryRescue`.
  **Every first-meeting tree in this pack has a "spine"**: the default path (always choice 0,
  or the no-choice chain) is exactly 4–6 lines and lands on a `"follow": true` effect — so the
  current engine can play any tree TODAY by walking the spine and ignoring branches.
- `app/dialog.h` — `x3::dialog::Tree { Node{id,speaker,voice,line,choices[]}, startNode }` is
  already a branching runner with validation, AI/Hybrid line rewrite, and TTS. This format is
  that struct **plus two optional fields per node/choice: `if` (conditions) and `fx`
  (effects)**, and string ids instead of int ids (a loader interns them).
- `app/canon_play.h` — `GirlsDialog` / `GirlDialogState` (CaptiveFrantic / RescuedGrateful /
  CompanionAmorous / InfectedLost) maps onto this format's **tree names** (see §4).
- `app/timeline.h` — `TimelineState` already owns karma/humanity/love/trust/mercy/redemption,
  the Omega/Alpha/Beta/Gamma timeline, captive fates, and ally counts. Conditions read it;
  effects write it through its existing `adjust*`/`notify*` API. **No new moral state.**
- `engine/script/IScriptSystem.h` + `scripts/secret_room.lua` — effects may broadcast
  `x3.fire(event, args)`; Lua scripts subscribe via `onEvent` and call registered host
  functions (`x3.openTrapdoor()` pattern). Story logic that isn't an axis-adjust lives there.

## 1. File shape

```json
{
  "format": "x3.chattree/1",
  "npc": "aria",                  // stable id (flag namespace + save key)
  "display": "ARIA",              // HUD speaker label
  "voice": "Aria",                // VoiceId name for TTS (extend dialog.h enum)
  "trees": {
    "first_meeting": { "start": "fm0", "nodes": [ ...Node ] },
    "banter":        { "pool":  [ ...BanterLine ] },
    "...":           { }
  }
}
```

`trees` is an open map. Conventional tree names (the loader/host triggers them):

| Tree name        | Triggered by                                                   |
|------------------|----------------------------------------------------------------|
| `first_meeting`  | E on a live captive (replaces `makeRescueDialog`)              |
| `banter`         | Companion idle pool — pick by weight among passing `if`s       |
| `sidequest`      | E on the companion after `rel >= 2` (the personal quest hook)  |
| `trust` / `bond` / `romance` | Relationship-arc scenes, gated by `rel` + axes     |
| `prefight` / `spare` | Boss confrontation trees (Martinez)                       |
| `hub`            | Re-enterable hub conversation (Vesper's bar loop)              |
| `infected_lost`  | The girl transformed — her boss-intro lines                    |

## 2. Node

```json
{
  "id": "fm0",
  "speaker": "ARIA",            // omit => npc display; "YOU" = Jake
  "line": "You're... not one of them.",
  "if":   [ {"flag": "aria.interrupted"} ],   // ALL must pass to show this node
  "else": "fm0_alt",            // node to show instead when `if` fails
  "fx":   [ {"set": "aria.met"} ],            // applied when the line is delivered
  "choices": [
    { "text": "Easy. I'm getting you out of here.",
      "next": "fm1",
      "if":  [ {"karma_gte": 0} ],            // hide the choice unless it passes
      "fx":  [ {"karma": 2} ] }
  ]
}
```

- A node with **no choices** auto-advances to `"next"` if present, else ends the tree.
- `"next": "end"` ends the conversation (maps to `kEndNode`).
- Choices are **filtered, never greyed**: a failed `if` hides the option (no "locked" tease
  unless an explicit alternate choice is authored).

### BanterLine (pool entries)
```json
{ "line": "Stay close to me?", "if": [ {"rel_gte": ["aria", 1]} ], "weight": 2, "once": false }
```

## 3. Condition + effect vocabulary

**Conditions** (`if` arrays; AND semantics; `{"any":[...]}` for OR; `{"not": {...}}` negates):

| Op | Meaning | Backed by |
|---|---|---|
| `{"flag": "name"}` | story flag set | new `StoryFlags` set (string→bool, save-serialized) |
| `{"karma_gte": n}` / `{"karma_lte": n}` | karma window | `TimelineState::axes().karma` |
| `{"humanity_gte": n}`, `{"love_gte": n}`, `{"trust_gte": n}`, `{"mercy_gte": n}`, `{"redemption_gte": n}` | axis gates | `MoralityAxes` |
| `{"timeline": ["Alpha","Omega"]}` | locked timeline ∈ set | `TimelineState::timeline()` |
| `{"girl_saved": "keisha"}` / `{"girl_lost": "emily"}` | captive fate | `TimelineState::fate(Woman)` |
| `{"item": "chen_video_log"}` | inventory holds item | host inventory (string ids) |
| `{"rel_gte": ["aria", 2]}` | relationship stage ≥ n | new per-NPC `rel` int (0–4, see §5) |
| `{"chance": 0.25}` | weighted variety roll | deterministic per-(save,node) hash |
| `{"lua": "fn_name"}` | escape hatch: registered Lua fn returns truthy | `IScriptSystem` NativeFn |

**Effects** (`fx` arrays, applied in order):

| Op | Meaning | Backed by |
|---|---|---|
| `{"karma": ±n}` etc. (any axis) | adjust axis | `TimelineState::adjust*` |
| `{"set": "flag"}` / `{"clear": "flag"}` | story flags | `StoryFlags` |
| `{"fire": "event", "args": {…}}` | broadcast to Lua | `IScriptSystem::fire` — story logic lives in `scripts/*.lua` |
| `{"give": "item"}` / `{"take": "item"}` | inventory | host |
| `{"follow": true}` | she becomes a Companion | `RescueSystem::tryRescue` (exactly what `NpcDialog::onComplete` does today) |
| `{"rel": ["aria", 2]}` | set relationship stage (only raises) | per-NPC rel int |
| `{"ally": true}` | +1 alliance, +trust | `TimelineState::onAllyJoined` |
| `{"end": "fight"}` | end conversation with a host verb (`fight`, `flee`, `shop`…) | host switch / `x3.fire("dialog_end", {npc, verb})` |

## 4. Mapping onto the existing 5-stage / 4-state system

`GirlDialogState` ↔ trees:
- `CaptiveFrantic`   → the captive's pre-interaction barks = `first_meeting` nodes tagged
  before the spine's reassure beat (or the existing `girls_dialog.json` pool — both kept).
- `RescuedGrateful`  → the spine's tail nodes (`fm3`+) and the immediate post-`follow` bark.
- `CompanionAmorous` → `banter` pool entries with `rel_gte` ≥ 1 (warm) / ≥ 3 (devoted).
- `InfectedLost`     → the `infected_lost` tree.

`NpcDialog` compatibility: a shim `makeRescueDialogFromTree(npc)` walks the spine (choice 0
at every branch) and emits the same `std::vector<DialogLine>` it builds today. Zero risk path.

## 5. Relationship stages (shared across all companion NPCs)

| rel | Name | Typical gate to reach it |
|---|---|---|
| 0 | Stranger | — |
| 1 | Rescued | `first_meeting` completes with `follow` |
| 2 | Trust | her `sidequest` resolved + karma ≥ 10 |
| 3 | Bond | `trust` scene played + axis gate (girl-specific: love/mercy/trust) |
| 4 | Romance | `romance` scene accepted + timeline permits (Alpha/Omega per canon; Beta locks the F2 girls out because they're bosses) |

Romance is always **opt-in via an explicit choice** and can be declined without losing
rel 3 (declining never costs anything, at any stage). Heat authoring rules — the 1–5 chili
ladder, the consent grammar, and the trauma-stays-restrained rule — live in
`SPICE_GUIDE.md`. Culmination (`night`) scenes gate on `rel >= 3` + the npc's `.romance`
and `.desire` flags + the host-set `loc.private` flag; the captivity/menace material keeps
its original restraint regardless.

## 6. Lua data path (how a tree ships with zero C++)

1. Pak carries `scripts/eflz_dialog.lua` + `dialog/<npc>.chattree.json`.
2. Host loads trees at boot (same loader slot as `GirlsDialog::load`).
3. The tree runner (small C++: ~filter choices by `if`, apply `fx`) emits every `fire` effect
   into `IScriptSystem::fire`. Example: Lena's tree fires
   `x3.fire("dialog_hint", {code="1278"})`; `eflz_dialog.lua` catches it and calls
   `x3.setObjective("Lena mentioned a cell terminal code: 1278")` — exactly the
   `secret_room.lua` pattern already proven in-engine.
4. `{"lua": "fn"}` conditions let a script answer dynamic questions ("is the trapdoor open?")
   via `registerFunction` without widening the C++ condition enum.

## 7. Flag glossary used by this pack (namespaced `npc.flag`)

`<girl>.met / .interrupted / .rescued / .sq_done / .romance`,
`martinez.talked_down / .spared`, `reyes.deal / .fled / .threatened / .dead / .betrayed`,
`club.found / vesper.met / vesper.rumor_<n>`, `lena.carrier` (the mole arc — set silently by
the host when a girl is rescued at `InfectionStage::Critical`), `quartermaster.known`,
`cradle.known` (the program's name has been learned), `code.1278.known`, `code.1127.known`.
