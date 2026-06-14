# Mission — Storm Runner (`act2_storm_runner`)

**Act 2 · Level 19/20 · Spaceport Approach and the Spaceport — the way off Keth'zar**

> There's a ship called the Storm Runner on the Spaceport apron, and there's a Garrison Commander who'd rather burn it than let it leave. Take the ship. Take the war with you. And if a well-dressed man with cold-metal cologne is on that apron — that's your call.

**Offered when:** `act2.alliance_formed` set

**Starts at:** `o0`

## Beats (objective graph)

### `o0` — Break the Spaceport approach. Vehicle combat — the Defense Commander has the outer perimeter.
- *on enter:* fire `map_ping`, fire `grant_vehicle`
- *complete when:* `act2.defense_commander_down` set
- *on complete:* karma +3
- *then:* → `o1`

### `o1` — Reach the apron. The Storm Runner is fueled at the far end — under the Garrison's guns.
- *complete when:* `act2.apron_reached` set
- *branch:* if `quartermaster.ledger_held` set → `o2_qm`
- *then:* → `o2`

### `o2_qm` — The Quartermaster is here in person, brokering the Garrison's resupply. He recognizes you. He has nowhere to run, and he knows it.
- *on enter:* fire `dialog_open`
- *complete when:* `quartermaster.confronted` set
- *branch:* if `quartermaster.spared` set → `o2` — mercy +4, set `quartermaster.witness`
- *branch:* if `quartermaster.killed` set → `o2` — karma -3
- *then:* → `o2`

### `o2` — Fuel and crew the Storm Runner while the alliance holds the apron.
- *on enter:* fire `objective_banner`
- *complete when:* `stormrunner.fueled` set, `stormrunner.crew` set
- *on complete:* set `stormrunner.fueled`
- *then:* → `o3`

### `o3` — The Garrison Commander makes a stand on the loading ramp. He won't let the ship leave whole. Prove him wrong.
- *on enter:* fire `spawn_boss`, fire `objective_banner`
- *complete when:* `garrison_commander.down` set
- *on complete:* karma +5
- *fail:* when `stormrunner.crippled` set → `o4` — karma -3, set `act2.rough_launch`
- *then:* → `o4`

### `o4` — Get the Storm Runner off the ground. Whoever's still standing comes with you. Whoever isn't, you carry differently.
- *on enter:* fire `cutscene`
- *complete when:* `stormrunner.launched` set
- *on complete:* set `stormrunner.launched`, set `act2.complete`, karma +4, fire `act_transition`
- *then:* → `end`

## Hooks into canon / existing branches

Act-2 climax — canon L19 Spaceport Approach (Defense Commander mini-boss, vehicle combat) + L20 The Spaceport / 'Escape From Kethzar Prime' (Garrison Commander boss; capture the Storm Runner; ship choice affects Act 3). give-gated on act2.alliance_formed (act2_many_hands) so the ally count earned across Act 2 literally crews the ship. Optional o2_qm pays off the Quartermaster heist (act2_quartermasters_ledger): if the ledger is held, he appears in person for the spare/kill confrontation the expansion (a) promised (sparing → quartermaster.witness, the Act-4 trial prosecutor's witness). act_transition fire hands off to Act 3. The Defense/Garrison Commanders are staged via spawn_boss for the engine's existing boss machine.

## Voice note

Momentum and cost. The launch line ('whoever isn't, you carry differently') keeps the loss restrained and personal. The Quartermaster confrontation is the only place his civilian menace gets a face — and even then he is bargained with, not boss-fought.
