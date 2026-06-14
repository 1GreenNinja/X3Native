# Mission — The Drowned Disc (`act2_the_drowned_disc`)

**Act 2 · Level 13/18 · The Undersea Disc Base — the lab's first attempt to tap the root, on the trench floor**

> There's a base under the sea the lab pretends never existed. It was their first attempt to tap the root directly. The root responded. Whatever's still down there is on patrol, and has been for sixty-five million years.

**Offered when:** `salvari.cure_known` set, `biomesh.lore` set

**Starts at:** `o0`

## Beats (objective graph)

### `o0` — Find a way down to the trench. The Salvari have a route they don't like to talk about.
- *on enter:* fire `dialog_open`
- *complete when:* `act2.trench_route` set
- *then:* → `o1`

### `o1` — Descend to the Drowned Disc. Every depth-meter is closer to the part of this world that's still awake.
- *on enter:* fire `map_ping`
- *complete when:* `disc.found` set
- *on complete:* set `disc.found`
- *then:* → `o2`

### `o2` — Get inside the base. The flood took it, but the records survived in the dark.
- *complete when:* counter `evidence.disc_log` >= 2
- *on complete:* set `act2.disc_logs`
- *then:* → `o3`

### `o3` — Read what they did and what answered. The Leviathan on the canon submarine-combat boards isn't a native animal.
- *on enter:* fire `cutscene`
- *complete when:* `act2.leviathan_understood` set
- *on complete:* set `disc.root_tapped`
- *then:* → `o4`

### `o4` — The base's old tap is still live, and it has called the patrol. Survive the Leviathan — or shut the tap and let it go back to sleep.
- *on enter:* fire `spawn_boss`, fire `objective_banner`
- *complete when:* any
- *on complete:* karma +2
- *branch:* if `act2.tap_sealed` set → `o_done_seal` — mercy +5, karma +4, redemption +3
- *then:* → `o_done`

### `o_done_seal` — You close the wound the lab left open. The Leviathan turns away into the dark, still on patrol, no longer at war with you.
- *on complete:* set `act2.disc_done`, set `biomesh.root_living`
- *then:* → `end`

### `o_done` — It's dead, or it's gone. The trench keeps its silence either way, and you understand now what the silence is for.
- *on complete:* set `act2.disc_done`, set `biomesh.root_living`
- *then:* → `end`

## Hooks into canon / existing branches

The Biomesh-sea escalation (STORYLINE_EXPANSIONS (e) reveal #2: 'the ocean is the living part'). give-gated on salvari.cure_known + biomesh.lore (both earned earlier), so it lands after the player has the framework to understand it. Uses the canon Undersea Base + the Leviathan submarine-combat boss (systems catalog) and reframes the boss as the root's immune cell, not a monster. The seal path (mercy/redemption) sets biomesh.root_living, feeding Act-3's husk-world contrast (the same root, fully harvested) and sharpening Act-4's stakes (Earth's root is the next freshest). disc.root_tapped corroborates the Cradle bioreactor logic (thread b).

## Voice note

Awe and dread, underwater-slow. The Leviathan is tragic, not evil — 'it isn't hunting you.' The reveal recontextualizes Act 1 (the building was a tick on something's back) without a single line of lecture in the objective text itself.
