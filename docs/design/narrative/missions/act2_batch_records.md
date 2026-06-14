# Mission — Batch Records (`act2_batch_records`)

**Act 2 · Level 11 · Club 1127 (Y=-200) — the empty barstool at the end of the bar**

> Dr. Reyes ran, the way Reyes does. He resurfaced at the bottom of the world with a guilt-gift he can't bring himself to hand over sober: the gestation-cohort batch records. Get them. They name names, and one of the names is his.

**Offered when:** `reyes.fled` set, `club.found` set, not

**Starts at:** `o0`

## Beats (objective graph)

### `o0` — Vesper says the barcode man walked off the floor and onto her tab. Find Reyes at the club.
- *on enter:* fire `dialog_open`
- *complete when:* `act2.reyes_located` set
- *on complete:* set `vesper.reyes_paid`
- *then:* → `o1`

### `o1` — Talk to Reyes. He'll rationalize for an hour if you let him. Decide how much rope to give a man hanging himself.
- *on enter:* fire `dialog_open`
- *complete when:* `act2.reyes_talked` set
- *branch:* if `reyes.will_testify` set → `o2_witness`
- *branch:* if karma_lte -10 → `o2_extort`
- *then:* → `o2`

### `o2_witness` — He doesn't want money or mercy. He wants to be useful for once. Let him give you the records as testimony, not payment.
- *on complete:* give `batch_records`, set `reyes.testified`, redemption +3, karma +4
- *then:* → `o_done`

### `o2_extort` — He's terrified and you're not in a forgiving mood. Take the records the ugly way.
- *on complete:* give `batch_records`, set `reyes.coerced`, karma -2, mercy -2
- *then:* → `o_done`

### `o2` — Make the trade. The records for whatever he thinks buys him peace.
- *on complete:* give `batch_records`, set `reyes.dealt`
- *then:* → `o_done`

### `o_done` — The batch records are yours. Every cohort, every date, every authorizing signature. Including one that reads 'F. Reyes, reluctantly.'
- *on complete:* set `act2.batch_records_held`, fire `evidence_register`
- *then:* → `end`

## Hooks into canon / existing branches

Club 1127 hub side-quest paying off the Reyes thread from the existing pack (dr_reyes.json's FLEE outcome: reyes.fled + the club_door rumor). give-gated on reyes.fled + club.found. The batch records are the THIRD piece of the collaborator-trials evidence package (with Aria's manifest + the Quartermaster's ledger) — together they power STORYLINE_EXPANSIONS (f1) LANTERNS (Martinez prosecuting, 'First name Felix. Middle name reluctantly.'). The witness path (reyes.will_testify, set in his existing tree) is the redemption route; the extort path is the dark route. Needs a NEW act2_reyes node-set on Vesper + a club_hideout tree on Reyes (chat_trees/act2_reyes_club.json).

## Voice note

Reyes is 'rationalization mid-collapse' (his bio). The mercy here is letting a coward be briefly brave. The records' signature line is the whole tragedy of the collaborator arc in five words.
