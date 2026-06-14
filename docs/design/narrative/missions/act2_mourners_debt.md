# Mission — The Mourner's Debt (`act2_mourners_debt`)

**Act 2 · Level 10 · Crystalline Desert Depths — the crystal-cave approach**

> An injured Salvari near a hidden cave mouth needs help the desert won't give freely. A Reptilian patrol hunts the approach, and something older is watching the bargain.

**Offered when:** `act2.surfaced` set

**Starts at:** `o0`

## Beats (objective graph)

### `o0` — Cross the desert depths toward the singing-crystal cave mouth.
- *on enter:* fire `map_ping`
- *complete when:* `act2.found_salvari_wounded` set
- *then:* → `o1`

### `o1` — Drive off the Reptilian patrol before it reaches the wounded Salvari.
- *on enter:* fire `objective_banner`
- *complete when:* counter `kills.reptilian_patrol` >= 3
- *on complete:* karma +3
- *fail:* when `salvari.wounded_dead` set → `o_fail_grief` — karma -2, set `salvari.first_blood`
- *then:* → `o2`

### `o2` — Help the wounded Salvari. They keep trying to give you something instead of taking help.
- *complete when:* `act2.salvari_wounded_stable` set
- *on complete:* mercy +3, set `salvari.met`, set `salvari.owe_debt`, fire `dialog_open`
- *branch:* if `mole.suspected` set → `o3_mantis`
- *then:* → `o3`

### `o3_mantis` — A third party arrives — a lone Mantis Arbiter, drawn by the suspicion you carry. Hear its terms, or refuse them.
- *on enter:* fire `dialog_open`
- *complete when:* any
- *then:* → `o3`

### `o3` — Escort the Salvari down toward their camp.
- *complete when:* `act2.escorted_to_cavemouth` set
- *branch:* if `warlord.dead` set → `o_done`
- *branch:* if `warlord.spared` set → `o_done`
- *then:* → `o4_warlord`

### `o4_warlord` — The cave mouth is held. A Saurian Warlord bars the only way down.
- *on enter:* fire `spawn_boss`, fire `objective_banner`
- *complete when:* any
- *on complete:* karma +4
- *branch:* if `warlord.spared` set → `o_done` — mercy +5, karma +2
- *then:* → `o_done`

### `o_done` — The way down is open. The Salvari will tell their camp who opened it.
- *on complete:* set `salvari.allied`, trust +4, fire `mission_start`
- *then:* → `end`

### `o_fail_grief` — The Salvari is gone. The desert keeps its debts either way.
- *on complete:* set `salvari.met`, fire `dialog_open`
- *then:* → `o3`

## Hooks into canon / existing branches

Wraps act2-desert's L10 content into a quest: the injured-Salvari side-quest hook, the Reptilian-Trooper+Grey-Tasked patrol (grey-patrol branch), the Saurian Warlord gated arena (warlord branch), and the Mantis Arbiter wildcard (mantis-ambush branch, gated by mole.suspected so it ONLY appears for a player carrying the Carrier-arc suspicion — making the ambush feel earned, per STORYLINE_EXPANSIONS (c)). Sets warlord.dead/spared and salvari.allied which act2_refugee_haven (L11) reads. First on-screen K'thara line fires here.

## Voice note

The Salvari grammar of grief: they try to GIVE before they receive (the mourner's debt). Warlord can be spared (mercy line) — the canon spare-or-kill choice carried into Act 2.
