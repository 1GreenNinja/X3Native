# Mission — Many Hands (`act2_many_hands`)

**Act 2 · Level 18 · Underground Resistance HQ — a multi-species holdout beneath the ruined city**

> Salvari aren't the only ones the Overlord tried to file away. A resistance is forming underground from a dozen species who agree on exactly one thing. Make them agree on a second.

**Offered when:** `salvari.allied` set, `act2.heart_done` set

**Starts at:** `o0`

## Beats (objective graph)

### `o0` — Reach the resistance HQ. The Nordic Steward vouches for you; that gets you in the door, not the room.
- *on enter:* fire `dialog_open`
- *complete when:* `act2.resistance_met` set
- *then:* → `o1`

### `o1` — Three factions, three grievances. Hear them all before you pick a fight for them.
- *complete when:* counter `evidence.faction_brief` >= 3
- *on complete:* set `act2.factions_heard`
- *then:* → `o2`

### `o2` — They'll only unite behind someone who's bled for more than one of them. Settle the dispute that's keeping them apart.
- *on enter:* fire `objective_banner`
- *complete when:* `act2.dispute_settled` set
- *branch:* if karma_gte +20 → `o3_unity` — trust +4
- *branch:* if karma_lte -20 → `o3_strongarm`
- *then:* → `o3`

### `o3_unity` — You give them a reason instead of an order. Three species stand up at once.
- *on complete:* +1 alliance, +1 alliance, redemption +3
- *then:* → `o4`

### `o3_strongarm` — You make them an offer they're too scared to refuse. They follow. They don't love you for it.
- *on complete:* +1 alliance, trust -3, karma -2
- *then:* → `o4`

### `o3` — The alliance forms, ragged but real.
- *on complete:* +1 alliance
- *then:* → `o4`

### `o4` — An Overlord push hits the HQ while the ink's still wet. Hold the line together.
- *on enter:* fire `objective_banner`
- *complete when:* counter `kills.any` >= 12
- *on complete:* karma +5
- *fail:* when `act2.hq_overrun` set → `o4` — karma -4, trust -5
- *then:* → `o_done`

### `o_done` — The HQ holds. For the first time, the war has a 'we' in it that isn't just you and the people you carried out of a basement.
- *on complete:* set `act2.alliance_formed`, fire `hub_register`, fire `mission_start`
- *then:* → `end`

## Hooks into canon / existing branches

Canon L18 Underground Resistance — the multi-species alliance / companion-arc beat. give-gated on salvari.allied + act2.heart_done (so it lands after the player has proven themselves to the Salvari and seen the Biomesh truth). Reuses the Nordic Steward as the player's in (mentor tree, m_resistance node). The dispute resolution branches on karma (unity vs strong-arm) and grants alliances via the existing onAllyJoined (ally:true). act2.alliance_formed gates the Storm Runner climax and the late-game ending math (ally count scales L100). Chains into act2_storm_runner.

## Voice note

Grief politics done with restraint. 'Nobody here agrees on whose loss counts most' is the whole tragedy. The unity path costs nothing; the strong-arm path works but is quietly poisoned (trust down).
