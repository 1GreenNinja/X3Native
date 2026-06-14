# Mission — The Quartermaster's Ledger (`act2_quartermasters_ledger`)

**Act 2 · Level 11/19 · Club 1127 (Y=-200) and the freight pipeline up to the Spaceport**

> A big ship shot you down six months ago. Big ships don't dogfight — they collect. Vesper knows whose ledger flagged you for collection, and the ledger rides the freight pipe twice a year. The next run is soon.

**Offered when:** `harvest.known` set, `club.found` set

**Starts at:** `o0`

## Beats (objective graph)

### `o0` — Buy the thread from Vesper. Bring her something she doesn't already know.
- *on enter:* fire `dialog_open`
- *complete when:* `quartermaster.known` set
- *then:* → `o1`

### `o1` — The ledger physically rides the freight pipe. Find where the next run loads.
- *on enter:* fire `map_ping`
- *complete when:* `act2.freight_found` set
- *on complete:* set `quartermaster.ledger_run`
- *then:* → `o2`

### `o2` — Get aboard the freight line without tripping the manifest check. Loud or quiet — both have a bill.
- *complete when:* `act2.aboard_freight` set
- *branch:* if karma_gte +10 → `o3`
- *fail:* when `act2.manifest_alarm` set → `o3_hot` — karma -1, set `act2.freight_hot`
- *then:* → `o3`

### `o3` — Find the ledger. It's a physical book, because the man who keeps it doesn't trust networks any more than you do.
- *complete when:* counter `evidence.ledger_pages` >= 1
- *on complete:* give `quartermaster_ledger`, set `act2.ledger_taken`
- *then:* → `o4`

### `o3_hot` — The line's awake now. Fight to the ledger and out before the run seals.
- *on enter:* fire `objective_banner`
- *complete when:* counter `evidence.ledger_pages` >= 1
- *on complete:* give `quartermaster_ledger`, set `act2.ledger_taken`, karma +2
- *then:* → `o4`

### `o4` — Read what the ledger says about you. The entry is dated. Check when.
- *on enter:* fire `cutscene`
- *complete when:* `act2.ledger_read` set
- *on complete:* set `harvest.predated`
- *then:* → `o_done`

### `o_done` — The order was cut before your flight plan was filed. He didn't find your ship. He found your bloodwork. Keep the book — it's evidence now.
- *on complete:* karma +4, set `quartermaster.ledger_held`, fire `evidence_register`
- *then:* → `end`

## Hooks into canon / existing branches

The Quartermaster heist (STORYLINE_EXPANSIONS (a)). give-gated on harvest.known (Emily's reveal) + club.found (Vesper). Vesper opens the thread via a NEW act2_quartermaster tree appended to her file (authored in chat_trees/club1127_vesper_act2.json). The ledger item + harvest.predated reveal seed Act-4 Human Collaborators and the high-karma 'collaborator trials' ending overlay (f1 LANTERNS) — registered into the evidence package alongside Aria's manifest + Reyes's batch records. Uses the engine's existing descent-tube/freight-line space (per the expansion doc). The Quartermaster himself surfaces in person at the Spaceport — see act2_storm_runner.

## Voice note

He is never a boss in Act 2 — 'a ledger, a cologne, an empty barstool.' The horror is bureaucratic: you were a purchase order. Vesper prices the rumor; she never lies.
