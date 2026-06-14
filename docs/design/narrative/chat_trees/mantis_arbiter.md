# Chat tree — ARBITER (`mantis_arbiter`)

> THE MANTIS ARBITER — a lone, neutral wildcard of the Mantis species (canon-aliens), an itinerant 'arbiter' that trades in the one currency it values: information about who is compromised. It is drawn to the suspicion the player carries (the Carrier arc's mole.suspected) like a scavenger to a wound. Not an enemy and not a friend — a price. Its voice is patient, geometric, unsettlingly courteous, never raising itself, never lying, never giving anything away free.

## Tree: `wildcard`
*Fired from act2_mourners_debt o3_mantis (only appears when mole.suspected is set).*

_start:_ `w0`

**`w0` — ARBITER:**
  - _gate:_ `mole.suspected` (else → `w_none`)
  - "*it unfolds from the crystal shade with no sound at all, forelimbs steepled in something that is almost a bow* You carry a question you have not asked aloud. I can smell it on the one walking behind you. *the head tilts, precise* I am an Arbiter. I settle the question of who is still wholly themselves. You have such a question. I have the answer. The exchange writes itself."
    - **>** "Name your price." → `w1`
    - **>** "I settle my own questions about my own people." → `w_refuse` → trust +1
    - **>** "What makes you think there's a question at all?" → `w_probe`

**`w_probe` — ARBITER:**
  - "*a slow, courteous closing and opening of the forelimbs* Because the drones near her walk wrong, and you have noticed, and you have told no one, and the not-telling has a weight you carry in the left shoulder. I do not guess, soldier. I read. The reading is free. The answer is not."
    - **>** "Then name your price." → `w1`
    - **>** "Keep your answer. I'll find it myself." → `w_refuse` → trust +1

**`w1` — ARBITER:**
  - "Small. Almost a courtesy. When you settle her — and you will, one way or another — you will tell me how. Not which way. Only that it was settled, and the shape of it. I collect endings. They are the only honest part of any story. Agree, and I tell you precisely what was done to her, and how deep it went."
    - **>** "Deal. Tell me how deep it went." → `w_bargain` → set `mantis.bargain`, set `carrier.diagnosis_known`
    - **>** "No. Her ending isn't yours to collect." → `w_refuse` → karma +2, set `mantis.refused`, trust +2

**`w_bargain` — ARBITER:**
  - "*it gives you the truth flatly, without relish, which is somehow worse* The strand in her never finished. She cannot be commanded. She can be heard through. The closing of it is possible and it is not cheap. *it folds itself away* I will find you when there is an ending to collect. There always is. Walk well, soldier. Carry the left shoulder a little higher; the weight has a name now."
  - _fx:_ fire `rumor`
  - → `end`

**`w_refuse` — ARBITER:**
  - "*no offense registers; perhaps none is possible* As you wish. I do not buy what is not for sale, and I do not press. But the question will ripen, and questions that ripen unanswered tend to answer themselves at the worst hour. I will be in the desert. I am patient the way stone is patient. *it is already gone.*"
  - _fx:_ set `mantis.refused`
  - → `end`

**`w_none` — ARBITER:**
  - "*it regards you, finds no wound to trade on, and withdraws* No question. Then no exchange. A clean soldier is a boring one. Walk well."
  - → `end`
