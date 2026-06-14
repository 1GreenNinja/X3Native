# Mission — The Root Remembers (`act2_the_root_remembers`)

**Act 2 · Level 12 · Advanced Cave System — the ancient ruins and the Crystal Heart chamber**

> The ruins predating the Overlord aren't ruins. Deep under the desert, the crystal teaches a thing the survey crews stopped saying out loud — and a companion says it in a voice that isn't quite hers.

**Offered when:** `salvari.haven` set

**Starts at:** `o0`

## Beats (objective graph)

### `o0` — Descend into the cave system. The crystals hum differently the deeper you go.
- *on enter:* fire `map_ping`
- *complete when:* counter `crystals_scanned` >= 3
- *then:* → `o1`

### `o1` — Read the ruin walls. They aren't masonry.
- *on enter:* fire `cutscene`
- *complete when:* `act2.ruin_read` set
- *on complete:* set `biomesh.lore`
- *then:* → `o2`

### `o2` — Reach the Crystal Heart chamber. It needs both halves of you: strength to open it, a hacker to listen.
- *complete when:* `act2.heart_opened` set
- *branch:* if girl_saved sarah → `o3` — set `act2.sarah_listened`
- *then:* → `o3`

### `o3` — The Heart will give you the substrate-scan — but only if a companion stands in the resonance with you. Watch who answers it.
- *on enter:* fire `dialog_open`
- *complete when:* `act2.heart_attuned` set
- *on complete:* give `biomesh_scanner`, set `biomesh.scan`
- *branch:* if any → `o4_whisper`
- *then:* → `o5`

### `o4_whisper` — One of them stands too still in the resonance. The crystal answers her in a register you've heard before — on a floor you'd rather forget.
- *on enter:* fire `carrier_whisper`
- *complete when:* `act2.whisper_heard` set
- *on complete:* set `mole.suspected`
- *then:* → `o5`

### `o5` — The Memory Hunter stirs in the deep gallery, baited by the Heart. Survive it — or starve it of what it wants.
- *on enter:* fire `spawn_boss`, fire `objective_banner`
- *complete when:* `memory_hunter.routed` set
- *on complete:* karma +4, redemption +2
- *then:* → `o_done`

### `o_done` — You leave knowing the world is bigger than the lab — and worse. The scan reads markings now. Someone left them for whoever came next.
- *on complete:* set `act2.heart_done`, set `architect.markings_readable`
- *then:* → `end`

## Hooks into canon / existing branches

Wraps act2-caves' L12 Crystal Heart into the Biomesh-thread reveal (STORYLINE_EXPANSIONS (e)): the ruins are anatomy, the crystals are memory nodes of the dead root, the bio-mesh substrate-scan (HIDDEN_AREAS) is earned here and its markings are the Architect's (thread (d)). The Memory Hunter (canon L12 boss, recurs L23-25) is staged but its defeat is framed as 'routed/starved' (psychological-warfare boss). The optional o4_whisper is the Carrier arc's reveal #1 (whisper at resonance) — fires the matching companion's carrier tree at whisper volume and sets mole.suspected, the flag act2_mourners_debt's Mantis branch and act2_the_quiet_channel both key on. Generic over which girl is the carrier (lena.carrier default, but any).

## Voice note

The chamber teaches by geometry, not lecture. The whisper is the only horror beat here and it stays restrained — one line, flat second register, no description of what it means. The player decides what they heard.
