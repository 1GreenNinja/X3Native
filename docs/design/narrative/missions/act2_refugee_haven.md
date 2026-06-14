# Mission — Refugee Haven (`act2_refugee_haven`)

**Act 2 · Level 11 · Salvari Camp — a hidden cave settlement of bioluminescent crystal**

> The last of a genocided species shelters in a cave that sings. Earn their alliance, learn the cure, and meet the steward who will teach you to fight a war you didn't start.

**Offered when:** `salvari.allied` set

**Starts at:** `o0`

## Beats (objective graph)

### `o0` — Enter the camp. Let them see your hands before your weapons.
- *on enter:* fire `dialog_open`
- *complete when:* `kthara.met` set
- *on complete:* set `salvari.met`
- *then:* → `o1`

### `o1` — Hear K'thara out. Thirty survivors from thirty billion is not a number you argue with.
- *complete when:* `kthara.briefed` set
- *on complete:* set `cradle.known`
- *branch:* if karma_lte -20 → `o2_wary`
- *then:* → `o2`

### `o2` — The Nordic Steward at the upgrade station offers to teach. Accept the lesson, or take only the gear.
- *on enter:* fire `dialog_open`
- *complete when:* any
- *on complete:* give `alien_upgrade`
- *branch:* if `steward.mentored` set → `o3` — give `alien_upgrade`, trust +3
- *then:* → `o3`

### `o2_wary` — They don't trust you. Prove the camp is safe in your hands before they open anything.
- *on enter:* fire `objective_banner`
- *complete when:* `salvari.trust_earned` set
- *on complete:* trust +5, karma +2
- *then:* → `o2`

### `o3` — K'thara offers to join you — and offers the cure synthesis. Decide what the alliance is built on.
- *on enter:* fire `dialog_open`
- *complete when:* `kthara.decided` set
- *on complete:* set `salvari.cure_known`
- *branch:* if `kthara.joined` set → `o_done` — +1 alliance, set `salvari.cure_known`, give `salvari_cure_kit`
- *then:* → `o_done`

### `o_done` — Refuge, for now. The war is still out there, but you have a door that closes and people behind it.
- *on complete:* karma +5, set `salvari.haven`, fire `hub_register`
- *then:* → `end`

## Hooks into canon / existing branches

Wraps act2-desert's L11 Salvari Camp + the nordic-mentor branch (Nordic Steward at the upgrade station) into the alliance quest. K'thara recruited here (ally:true) is the canon Salvari leader / Beta-romance option; first_contact tree authored in chat_trees/kthara.json. salvari.cure_known is the cure thread the Carrier arc (act2_the_quiet_channel) reads. Registers the camp as a re-enterable hub (like Club 1127). A low-karma player must earn trust first (o2_wary) — a non-blocking detour.

## Voice note

K'thara is grief carried with dignity, never self-pity. The Steward is dry, practical, second-species outsider wisdom ("You fight like a man who only just learned he's allowed to.").
