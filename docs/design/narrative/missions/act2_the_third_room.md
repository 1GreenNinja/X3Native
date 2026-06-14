# Mission — The Third Room (`act2_the_third_room`)

**Act 2 · Level 11/12 · Below Club 1127 — the under-halls down to the Alien Substrate boundary (Y=-400)**

> Eleven twenty-seven was his door. Twelve seventy-eight was his cell. There's a third number, further down than either, with his real work in it. The bio-mesh scan reads his markings now — follow them to the bottom of the world.

**Offered when:** `rumor.third_room` set, `biomesh.scan` set

**Starts at:** `o0`

## Beats (objective graph)

### `o0` — Earn the floor it's on. Vesper sells the third-room rumor only when you've gone deep enough to deserve it.
- *on enter:* fire `dialog_open`
- *complete when:* `architect.third_room_known` set
- *then:* → `o1`

### `o1` — Descend the under-halls. The bio-mesh scan picks up his markings on the rock — follow them down past where the Illuminated refuse to go.
- *on enter:* fire `map_ping`
- *complete when:* `act2.past_illuminated_line` set
- *on complete:* set `biomesh.deep`
- *then:* → `o2`

### `o2` — The substrate hum is loud here, and the drones drift drunk. Reach the Architect's listening station at the boundary.
- *on enter:* fire `objective_banner`
- *complete when:* `act2.station_reached` set
- *then:* → `o3`

### `o3` — Read his drawings. He wasn't hiding from the thing below. Find out what he was doing instead.
- *on enter:* fire `cutscene`
- *complete when:* `act2.drawings_read` set
- *on complete:* set `architect.station`, set `biomesh.lullaby_known`, redemption +3
- *then:* → `o4`

### `o4` — The station still runs. Decide what to do with a lullaby pointed at something sixty-five million years asleep.
- *complete when:* any
- *on complete:* karma +3, mercy +2
- *branch:* if `architect.station_silenced` set → `o_done_silenced` — karma -2, set `act2.hum_rising`
- *then:* → `o_done`

### `o_done` — You leave the lullaby playing. One builder kept the world asleep his whole life; the least you can do is not be the one who woke it.
- *on complete:* set `act2.third_room_done`, give `architect_drawings`
- *then:* → `end`

### `o_done_silenced` — You cut the music. The rock is quieter now. You'll learn what that means later — everyone always does.
- *on complete:* set `act2.third_room_done`, give `architect_drawings`
- *then:* → `end`

## Hooks into canon / existing branches

The Architect arc (STORYLINE_EXPANSIONS (d)) + the deep-tunnel/substrate boundary (-400, canon per X3_WORLD_BLUEPRINT and HIDDEN_AREAS). give-gated on rumor.third_room (Vesper, set in her existing r_codes node) + biomesh.scan (earned in act2_the_root_remembers — so the scan's markings being the Architect's pays off mechanically). Binds the three numbers (1127/1278/the third) into one man's addresses; gives the bio-mesh scan a narrative father. The kept/silenced choice ties forward to act2_the_drowned_disc (the ocean is the awake part) and Act-3's husk-world. Vesper's act2_architect tree lives in chat_trees/club1127_vesper_act2.json.

## Voice note

A human ghost story under an alien horror. The Architect is homesick genius, not a villain. The reveal is that he was a caretaker, not a coward — 'he was keeping it asleep.' Quiet, reverent, a little sad.
