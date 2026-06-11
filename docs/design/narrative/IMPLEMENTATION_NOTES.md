# Implementation Notes — honest state of the wiring

## What exists TODAY (read from the repo, 2026-06-11)

| System | File | What it already does |
|---|---|---|
| Linear rescue exchange | `app/npc_dialog.h/.cpp` | One active exchange; 5-line shared script (`makeRescueDialog`); E advances; completion fires `onComplete` → `RescueSystem::tryRescue` → Companion follow. Headless-tested. **No branching, no conditions, shared lines.** |
| Branching tree runner | `app/dialog.h/.cpp` | Full `Tree{Node{id,speaker,voice,line,choices[]}}` runner with validation, player choices, Tree/Ai/Hybrid modes (Claude/Grok provider hook), TTS hook, speaking-NPC seam, self-test. **Trees are authored in C++ (`sampleSarahTree()`), int node ids, NO conditions/effects, NO JSON loader.** |
| Per-girl lines by lifecycle | `app/canon_play.h/.cpp` + `staging/girls_dialog.json` | `GirlsDialog` loads per-girl line pools for 4 states (frantic/grateful/amorous/lost) from JSON with baked fallback; distinct-voice self-test. **Pools, not trees.** |
| Morality/timeline backbone | `app/timeline.h/.cpp` | Karma/Humanity/Trust/Mercy/Love/Redemption (clamped), Omega/Alpha/Beta/Gamma lock, per-captive `InfectionTimer` with stages + cure rates `[0,90,60,30,0]`, ally count, 12-ending eligibility, serialize/deserialize, global `notify*` bridges. **Everything the trees' conditions/effects need on the moral axis already exists.** |
| Rescue/companion | `app/rescue.h/.cpp` | VictimState Captive→Companion→Expired, follow AI, victim→boss transform. |
| Lua data path | `engine/script/IScriptSystem.h` + `scripts/secret_room.lua` | Sandboxed Lua from pak; `onEvent`/`x3.fire`; `registerFunction` host bindings; hot reload. The 1278 trapdoor beat already ships as pure data. |

## What needs EXTENDING (the actual gap, smallest honest list)

1. **JSON tree loader** → `x3::dialog::Tree`. New: parse `chat_trees/*.json` (string ids →
   interned int32), per-NPC multi-tree container, `validate()` reuse. ~1 file, mirrors
   `GirlsDialog::load`'s pattern (including baked fallback for clean checkouts).
2. **Condition eval + effects on Node/Choice.** Extend `Node`/`Choice` with `conds`/`fx`
   vectors (POD tagged structs mirroring NPC_CHAT_TREE_FORMAT §3). Evaluator reads
   `TimelineState` (exists), `StoryFlags` (NEW: a `std::unordered_set<std::string>` +
   save blit — trivial), inventory (host already tracks keycards/items ad hoc; needs a
   string-id set), and per-NPC `rel` ints (NEW: small map, save blit). Effects call
   existing `TimelineState::adjust*`, `RescueSystem::tryRescue` (the `follow` effect ==
   today's `onComplete`), and `IScriptSystem::fire` for everything else. **No new moral
   state — timeline.h already owns it.**
3. **Choice filtering in `DialogSystem::choices()`** — return only passing choices;
   auto-advance nodes (`next` without choices); `else` node redirect. Small change to the
   existing runner.
4. **Save flags.** `TimelineState::SaveState` already blits; add `StoryFlags` + `rel` map
   + per-NPC seen-node sets (for `once` banter) to the same save lane.
5. **Banter pools** — not trees: a weighted pick over `if`-passing entries on a companion
   idle timer. Can reuse `GirlsDialog`'s pool shape; ship as the same JSON file's
   `banter` tree.
6. **Trigger plumbing (host):** entering talk range routes to the right tree by NPC state
   (captive → `first_meeting`; companion + quest pending → `sidequest`; etc.) — a small
   table; plus host-fired nodes (`sq_done`, `carrier_confront` gates) via
   `x3.fire("dialog_open", {npc, tree, node})` so Lua can drive story scenes.
7. **Voices:** `VoiceId` enum needs Aria/Keisha/Emily/Lena/Martinez/Reyes/Vesper/Pete/
   Static appended (stable values, per the header's own instruction).

**Explicitly NOT needed:** new boss machinery (Martinez tree exits into the existing
`BossPhase` fight via `end:"fight"`), new infection model (carrier arc = the existing
Critical-stage rescue + one flag), new ending enums (expansion f = epilogue overlays
reading existing state).

## Suggested first slice (one girl, full loop, data-driven)

**Lena** — smallest cast dependencies (no F2 triage coupling), biggest system coverage,
and her tree already cross-wires the live `secret_room.lua` beat:

1. Loader + conditions/effects (items 1–3 above), Tree-mode only (AI/TTS hooks untouched).
2. Load `chat_trees/lena.json`; spawn her as the existing F5 captive ("captured lab tech"
   slot in `spire_mid`, or the canonlevel Medical Bay for testing).
3. Wire: `first_meeting` spine → `follow` effect → existing companion AI. Banter pool on
   the idle timer. `trust` tree fires `dialog_hint {code:1278}` → a 5-line addition to a
   Lua script sets the objective — proving the dialog→Lua→world chain end-to-end against
   the already-working trapdoor.
4. Headless test mirroring `runNpcTalkSelfTest`: walk the spine (compat with the 5-line
   contract), branch on a karma-gated choice, assert flag/karma effects landed in a local
   `TimelineState`, assert the fired event reached a stub script system.

That slice exercises loader, conditions, effects, follow, banter, Lua, and save flags —
every other NPC in this pack is then pure content. Estimated engine surface: ~2 new files
(`dialog_tree_loader`, `story_flags`) + modest edits to `dialog.cpp` and the host talk
trigger.

## Canon conflicts found while writing (flagged, per instructions)

- **Aria's romance:** canon Omega pairs Aria with Dr. David Chang (and the bible roots her
  "weaponized feelings" on Chief Daniels); Tim's engine-side intent (girls_dialog.json,
  `companion_amorous`, "at least one full romance arc — Aria") points her at Jake. Resolved
  per EFLZ_NARRATIVE's own structure: **Jake-romance arcs live in the Alpha lane**
  (canon polyamorous-family timeline) and pre-lock play; Omega pair-offs stay untouched.
  Same logic applied to Keisha (Torres) — her tree *uses* the conflict as content (finding
  Torres alive gracefully closes her Jake lane).
- **Martinez "dies learning the truth" (bible §1) vs "can be killed or spared" (bestiary).**
  Kept both reachable: talk-down/spare paths exist; the tragic close is optional downstream.
- **Emily's surname** appears as Watson throughout (consistent); bestiary's Chorus
  side-quest cast lists a "Lisa Chen" — untouched here.
- **Medical Bay code 4119** (Reyes) is a placeholder per HIDDEN_AREAS §6's open item
  ("placeholder codes used in Phase 1") — retune freely; the tree only needs *a* code.
- **Cold open vs bible opening:** the bible wakes Jake with no capture backstory; Tim's
  cold open (INTRO_COLD_OPEN.md) is authoritative per instructions — Martinez's "pilot,
  shot down, salvaged" line and expansion (a) are built on it and do not contradict the
  bible's wake-on-slab beat (the slab is month six).
- **Lena and Sarah exist in `staging/girls_dialog.json` but not in the design-doc rescue
  canon** (which is Aria/Keisha/Emily + Sarah-on-F7). Treated girls_dialog as engine canon:
  Lena = F5 captive. Sarah's trees deliberately NOT authored here — she's the main-quest
  spine (F7 timed rescue + master hack) and should be written against the F7 scene flow,
  not the companion template.
