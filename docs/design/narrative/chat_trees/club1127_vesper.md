# VESPER — Club 1127 bartender / info-broker (Y = −200, the bottom of the world)

**One line:** The fixer who has poured at the bottom of the world since before the lab had
a number — never lies, only prices; her rumor menu is the game's word-of-mouth quest board.

## Canon honored
- Club 1127 at the **bottom** of the facility, Y=−200, reached by elevator code **1127**
  (X3_WORLD_BLUEPRINT §2.3, §2.6 — Tim's real Miami club, ported). The "fixer NPC" from
  MASTER_GAME_PLAN's secrets list, given a name and a bar.
- The club pre-dates the lab in her telling ("the lab grew over us like coral over a
  wreck") — consistent with the alien tunnels being older than the facility (§2.6: the lab
  plugged into them) and with HIDDEN_AREAS' under-hall architecture. Why the Dominion
  tolerates it = expansion (d).

## How she works (mechanics)
The `hub` tree is **re-enterable** (first-visit v0 → returning v_loop) and the `v_menu` node
is the rumor system: choices appear as the player earns their prerequisites elsewhere —
rumors are *gated by flags other NPCs set*, so the club rewards every conversation had
upstairs. Every rumor fires `x3.fire("rumor", {topic})` for the Lua/objective layer.

| Menu item | Gate | Seeds |
|---|---|---|
| The Architect | heard of him (Lena's code scene or rumor) | hidden-areas meta-lore: the under-halls, furniture-doors, and the sealed room under Detention are one man's life work |
| The codes 1127/1278 | `code.1278.known` | **the code lore**: 1127 = his door, 1278 = his cell — he numbered hiding places after his addresses; plus the "third room, further down" hook (`rumor.third_room`) |
| What's below | — | the **substrate hum**; the club's PA rig is a lullaby pointed DOWN (re-reads the canon speaker inventory as story); expansion (e) seed |
| The surface | — | **the not-Earth tease**: the two-suns coaster sketch, staff contracts that auto-renew — pre-figures the canon L8 reveal without spoiling its impact |
| The big ship | `harvest.known` (Emily's ledger) | **the Quartermaster** — the human broker who targeted Jake's ship; opens the `quartermaster_ledger` quest (expansion a) |
| The barcode man | `reyes.fled` | pays off Reyes's flee branch; comps a free rumor — kindness upstairs is currency downstairs |

## Voice keys
"Honey" as punctuation; barkeep economics ("I only sell the question"); her two tells are
authored and consistent — *full attention* (stops polishing) for things she cares about,
*polishing the clean glass* for the one thing that scares her (the Quartermaster). She
checks the door exactly once in the tree. Players who read tells get told the truth twice.

## Flags set: `vesper.met`, `club.found`, `code.1127.known`, `rumor.architect_full / .third_room / .substrate_hum / .not_earth`, `quartermaster.known`, `vesper.reyes_paid`
