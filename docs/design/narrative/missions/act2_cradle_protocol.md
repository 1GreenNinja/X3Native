# Mission — The Cradle Protocol (`act2_cradle_protocol`)

**Act 2 · Level 12 · The Salvari Archives, reached through the Advanced Cave System**

> The Salvari kept records the lab never knew existed. In the Archives, the word Emily and Reyes both whispered finally has a definition. The program isn't breeding soldiers. It's breeding officers — and the womb is the only machine that can build one.

**Offered when:** `cradle.known` set, `act2.heart_done` set

**Starts at:** `o0`

## Beats (objective graph)

### `o0` — Reach the Salvari Archives. K'thara warns you the truth in there is the kind you don't get to un-know.
- *on enter:* fire `dialog_open`
- *complete when:* `act2.archives_reached` set
- *then:* → `o1`

### `o1` — Cross-reference the Archives against what you carry out of the lab. The caste map starts to line up.
- *complete when:* counter `evidence.cradle_record` >= 3
- *on complete:* set `act2.castes_understood`
- *branch:* if item batch_records → `o2_corroborated` — set `act2.cradle_corroborated`
- *then:* → `o2`

### `o2_corroborated` — Reyes's batch records and the Archives agree to the day. The vat tiers are trash; the carried tiers are the product. The womb is the bioreactor that builds the command lobe.
- *on complete:* set `cradle.core_seen`
- *then:* → `o3`

### `o2` — The pattern is undeniable even without the paperwork. Human gestation is the missing manufacturing step the vats can't replace.
- *on complete:* set `cradle.core_seen`
- *then:* → `o3`

### `o3` — The Archive's deepest record is the one that explains the Clone. The program's final assembly step needs an anomaly-bearing father. It already had one in inventory.
- *on enter:* fire `cutscene`
- *complete when:* `act2.officer_understood` set
- *branch:* if `harvest.known` set → `o4_harvest` — set `act2.clone_origin_known`
- *then:* → `o4`

### `o4_harvest` — You already knew they harvested you for six months. Now you know what they were building from it: the first true Officer, grown with your will surgically absent. Stopping the Clone on Floor 7 wasn't just saving Sarah. It was aborting the enemy's end-state.
- *on complete:* redemption +4, set `act2.cradle_full`
- *then:* → `o5`

### `o4` — The Clone is the program's final assembly step, not its cruelty. Whatever happened on Floor 7 mattered more than you knew.
- *on complete:* set `act2.cradle_full`
- *then:* → `o5`

### `o5` — The last record is the oldest. The Overlord has run Cradle-class programs since before mammals. Earth is a long-cycle cradle world. The dinosaur-killer wasn't an attack. It was a planting.
- *on enter:* fire `cutscene`
- *complete when:* `act2.orchard_seen` set
- *on complete:* set `act2.cradle_protocol_done`, set `biomesh.lore`, karma +3
- *then:* → `end`

## Hooks into canon / existing branches

The Cradle Protocol thread (STORYLINE_EXPANSIONS (b)) as a knowledge mission in the canon Salvari Archives (L12 region). give-gated on cradle.known (set by Emily/Reyes in the existing pack, or by act2_refugee_haven) + act2.heart_done. PURE DEEPENING — it adds no new world-state beyond knowledge flags; it gives the caste map, the Clone-as-final-assembly reveal, and the 65-Myr orchard one spine. Optional corroboration branches reward the player who did act2_batch_records (item:batch_records) and act2_quartermasters_ledger (harvest.known): the threads literally cross-check. Sets up Act-3's husk-world and Act-4's 'invasion is a harvest crew on schedule.'

## Voice note

The horror has cold industrial logic — 'the program literally mines autonomy' — which makes it worse, not lurid. Per the Cradle thread, the violation stays where Tim's docs put it (forced breeding as the ultimate violation of autonomy) and is NEVER depicted; it is explained, with grief, by the records. No explicitness. The trauma dial does not move.
