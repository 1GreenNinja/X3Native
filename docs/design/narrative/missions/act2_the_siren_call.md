# Mission — The Siren's Call (`act2_the_siren_call`)

**Act 2 · Level 14 · Research Station — a swamp-bound outpost where a failed cure was tried**

> Something in the drowned station sings in a voice you almost recognize. If you couldn't reach Aria on the medical floor, this is what the program made of her.

**Offered when:** girl_lost aria

**Starts at:** `o0`

## Beats (objective graph)

### `o0` — Search the research station. The recordings here are about a cure that didn't take.
- *on enter:* fire `map_ping`
- *complete when:* counter `evidence.research_log` >= 2
- *on complete:* set `act2.research_logs`
- *then:* → `o1`

### `o1` — Follow the sound. Don't let it follow you first.
- *on enter:* fire `objective_banner`
- *complete when:* `act2.siren_found` set
- *then:* → `o2`

### `o2` — Face the Siren. There is a person somewhere under the sound. Decide whether to look for her.
- *on enter:* fire `spawn_boss`, fire `dialog_open`
- *complete when:* any
- *on complete:* karma -1
- *branch:* if `siren.released` set → `o_done_mercy` — mercy +6, redemption +4, set `aria.laid_to_rest`
- *then:* → `o_done`

### `o_done_mercy` — You gave her the ending the lab refused her. It costs you nothing the world can measure and everything you can.
- *on complete:* set `act2.siren_done`
- *then:* → `end`

### `o_done` — It stops singing. You leave before the silence can tell you whose it was.
- *on complete:* set `act2.siren_done`, set `aria.lost_confirmed`
- *then:* → `end`

## Hooks into canon / existing branches

The Beta-path payoff of the F2 triage: if Aria was LOST on the medical floor, the program's transform (The Siren, canon L14 ambush — act2-roster ships BossTheSiren.glb + the TheSiren boss def) surfaces here as a mission. give:{girl_lost:aria} means the mission is OFFERED only on a run that lost her, so an Alpha/Omega player never sees it. The 'release' path (mercy/redemption) reuses her infected_lost tree from the existing pack and lays the named victim->boss motif to rest. Feeds the dark/redemption ending overlays (STORYLINE_EXPANSIONS (f)).

## Voice note

Restraint is the whole point. The horror is recognition, not gore — 'a voice you almost recognize.' The mercy path is offered, never forced, and the loss is never described in detail. The trauma dial does not move.
