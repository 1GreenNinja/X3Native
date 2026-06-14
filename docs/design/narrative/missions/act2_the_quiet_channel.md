# Mission — The Quiet Channel (`act2_the_quiet_channel`)

**Act 2 · Level 11/12 · Salvari Camp and the cave galleries — wherever the squad makes camp**

> A companion you saved at the last second carries something the program never finished. Not a traitor — a compromised channel. She offers you her knife, grip-first, and asks you to read the map she can't trust herself to read.

**Offered when:** `mole.suspected` set, any

**Starts at:** `o0`

## Beats (objective graph)

### `o0` — The drones drift wrong around her. A door opens she was never taught. Decide whether to say what you've noticed.
- *complete when:* `carrier.confront_chosen` set
- *fail:* when `carrier.left_buried` set → `o_buried` — set `carrier.buried`
- *then:* → `o1`

### `o1` — Confront her. Camp, quiet, no one else in earshot.
- *on enter:* fire `dialog_open`
- *complete when:* any
- *branch:* if `lena.cure_path` set → `o_cure`
- *branch:* if `mole.weaponized` set → `o_weaponize`
- *branch:* if `lena.final_choice` set → `o_knife`
- *then:* → `o_buried`

### `o_cure` — There's a way to purge a dormant strand — Salvari cure tech, the synthesis lab. Get it, and defend her while it runs.
- *gate:* `salvari.cure_known` set (else → `o_cure_blocked`)
- *on enter:* fire `objective_banner`
- *complete when:* `act2.purge_complete` set
- *on complete:* mercy +6, redemption +4, karma +6, clear `lena.carrier`, set `carrier.cured`, rel ['lena', 3]
- *then:* → `o_done`

### `o_cure_blocked` — You don't have the cure yet. Ally with the Salvari first; she'll hold until you do.
- *on enter:* fire `objective_banner`
- *complete when:* `salvari.cure_known` set
- *then:* → `o_cure`

### `o_weaponize` — Keep the tapped wire and feed it lies. It works — and the strand advances every time you use it. She set the toll herself; honor it.
- *on enter:* fire `objective_banner`
- *complete when:* `act2.false_ambush_turned` set
- *on complete:* set `carrier.weaponized`, karma -3, trust -2
- *then:* → `o_done`

### `o_knife` — She offered it. On camera, in your hand, irreversible. Nobody will make you. She won't even flinch.
- *complete when:* any
- *on complete:* karma -8, mercy -6, set `carrier.killed`, set `lena.lost_confirmed`, fire `companion_lost`
- *branch:* if `carrier.spared_at_edge` set → `o_cure_blocked` — mercy +4, set `lena.cure_path`
- *then:* → `o_done`

### `o_buried` — You say nothing. The ambushes keep feeling unlucky. Some things you only learn at the very end, if at all.
- *on complete:* set `carrier.buried`
- *then:* → `end`

### `o_done` — The channel is closed, one way or another. What it cost is yours to carry.
- *then:* → `end`

## Hooks into canon / existing branches

The Carrier/mole arc as a mission (STORYLINE_EXPANSIONS (c)). It DRIVES THE EXISTING lena.json `carrier_confront` tree (node c0) and reads its REAL outcome flags — lena.cure_path / mole.weaponized / lena.final_choice — rather than inventing new ones (no duplicate tree authored). give-gated on mole.suspected (set by act2_the_root_remembers' whisper) + a <girl>.carrier flag (set silently by the host when a girl was rescued at InfectionStage::Critical — the existing timeline.h [0,90,60,30,0] stage model, NO new infection state). Three priced resolutions: cure (needs salvari.cure_known from act2_refugee_haven), double-agent (mole.weaponized → STORYLINE_EXPANSIONS (f3) final-whisper), knife (irreversible, with a last spare-at-the-edge off-ramp routing back to the cure path). The whole arc is MISSABLE — never-noticed routes to o_buried (a player learns the truth only in an ending slide). Generic over which girl carries (lena default; aria/keisha/emily.carrier accepted at the offer gate, each driving her own infected_lost pool).

## Voice note

Per the pack and SPICE_GUIDE: menace stays restrained. She offers the knife grip-first; the cost surfaces gently. No resolution is free, and declining to act is itself a (costed) choice. The objective text quotes her own conditions back, never narrates harm.
