# Mission — Queen's Territory (`act2_queens_territory`)

**Act 2 · Level 16 · Ruined Metropolis — Outskirts of a fallen alien city**

> The scavengers in the dead city answer to something that holds ground like a soldier and thinks like a tactician. If Keisha didn't make it off the medical floor, this is who kept her promise the only way the program left her.

**Offered when:** girl_lost keisha

**Starts at:** `o0`

## Beats (objective graph)

### `o0` — Move through the ruined outskirts. Her territory has a perimeter and the perimeter has rules.
- *on enter:* fire `map_ping`
- *complete when:* `act2.queen_perimeter` set
- *then:* → `o1`

### `o1` — She summons before she swings. Break her adds or break her line of sight.
- *on enter:* fire `objective_banner`
- *complete when:* counter `kills.queen_adds` >= 4
- *then:* → `o2`

### `o2` — Confront the Breeder Queen. There's a security specialist's discipline in how she holds the room. Find out if there's anything else left.
- *on enter:* fire `spawn_boss`, fire `dialog_open`
- *complete when:* any
- *on complete:* karma -1
- *branch:* if `queen.released` set → `o_done_mercy` — mercy +6, redemption +4, set `keisha.laid_to_rest`
- *then:* → `o_done`

### `o_done_mercy` — She stops guarding the door at last. You hope, somewhere, someone learned to be loud.
- *on complete:* set `act2.queen_done`
- *then:* → `end`

### `o_done` — The plaza goes quiet. The scavengers scatter without their captain. You don't look back at the model number on the wall.
- *on complete:* set `act2.queen_done`, set `keisha.lost_confirmed`
- *then:* → `end`

## Hooks into canon / existing branches

Beta-path sibling of act2_the_siren_call: the Breeder Queen (transformed Keisha, canon L16 territory — act2-roster ships BossBreederQueen.glb + the BreederQueen boss def with phase-3 summons). give:{girl_lost:keisha} gates the OFFER, so only a Beta run sees it. Keisha's character (a promise-keeper, 'guard the door') is honored even in her lost-state line; the release path reuses her infected_lost tree. Both Beta-boss missions feed STORYLINE_EXPANSIONS (f3) Hollow Crown / Chen's-Redemption overlays.

## Voice note

Keisha's whole self is in 'she fights like she's still guarding a door.' The horror is the persistence of who she was, bent to the program's use. No gore, no spectacle of her body — the menace is tactical and the grief is quiet.
