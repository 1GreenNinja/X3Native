# Chat tree — K'THARA (`kthara`)

> K'THARA — leader of the Salvari, the refugee species the Overlord genocided down to thirty survivors from thirty billion. Carries an entire dead world's grief without once making it a performance. Salvari custom: you give before you take (the mourner's debt). Allies with Jake in Act 2; the canon Beta-path romance option (gated to Beta, where the F2 women are bosses, not companions). Her voice is dignity over despair, precise, with a dry edge that surprises people who expected only sorrow.

## Tree: `first_contact`
*Multi-mission tree. Fired by node id from the Act-2 missions (act2_mourners_debt fc0/fc_grief; act2_refugee_haven fc1/fc_offer; act2_the_root_remembers fc_heart; act2_the_drowned_disc fc_disc; act2_cradle_protocol fc_archives).*

_start:_ `fc0`

**`fc0` — K'THARA:**
  - "*the wounded Salvari presses something into your hand before you can offer help — a chip of singing crystal, still warm* Take. Take first. *the words come hard, in your language, learned from a dead broadcast* We do not... accept a kindness with empty hands. You stopped them. So. You are owed. Even bleeding, I am owed to you."
    - **>** "Keep the crystal. Let me stop the bleeding first." → `fc0a` → mercy +2, set `salvari.met`
    - **>** "I didn't do it to be owed. But I'll take the crystal so you'll let me work." → `fc0b` → mercy +1, set `salvari.met`

**`fc0a` — K'THARA:**
  - "*she lets you, watching your hands the way a teacher watches a student who might be worth keeping* You hold a wound like someone who has held many. Good. The desert respects that more than strength. I am K'thara. There were thirty billion of us. Now there is a camp, and you, and a debt I intend to pay in full."
  - _fx:_ set `kthara.met_field`
  - → `fc_end`

**`fc0b` — K'THARA:**
  - "*a sound that on a living world might have been a laugh* You take it so I will let you work. That is the most honest reason anyone has given me in a long time. K'thara. Stop the bleeding, healer-by-force. We will argue about debts when I can stand."
  - _fx:_ set `kthara.met_field`
  - → `fc_end`

**`fc_grief` — K'THARA:**
  - "*she does not reach for your hand; she folds her own around the place a wound used to be* You came. Too late for the one on the sand, but you came, and you fought for a stranger's body when you could have walked. *quietly* That is also a debt. The dead keep accounts too. I am K'thara. Let me show you where we bury the ones the desert was allowed to keep."
  - _fx:_ set `kthara.met_field`
  - → `fc_end`

**`fc1` — K'THARA:**
  - "*the camp glows blue around her; she stands at its center like a held breath* So. You opened the way down, and the sand has already told the camp your shape. Sit. Before you ask for an alliance, hear what you would be allied to. Thirty survivors. We were a people who measured wealth in songs, and we remember every one, because there is no one left to forget them for us."
  - _fx:_ set `kthara.met`
    - **>** "Tell me what happened to your world." → `fc2` → trust +1
    - **>** "I've seen what they do up there. You don't have to explain the enemy to me." → `fc2b` → trust +2

**`fc2` — K'THARA:**
  - "The same thing they are doing to yours, only finished. They came with gifts first — they always do. You do not burn an orchard you grew. By the time we understood the harvest, there was no 'we' large enough to fight it. *she meets your eyes* I tell you this not for pity. I tell you so that when I say your Earth has time, you understand exactly how little."
  - _fx:_ set `kthara.briefed`, set `cradle.known`
  - → `fc_end`

**`fc2b` — K'THARA:**
  - "*she studies you a long moment* No. I suppose I don't. Then I will tell you the part you haven't seen: they came with gifts first. Kindness is the first weapon, always. Your world is still in the gift stage, soldier. That is the only good news I have, and it is not good for long."
  - _fx:_ set `kthara.briefed`, set `cradle.known`
  - → `fc_end`

**`fc_offer` — K'THARA:**
  - "*she sets two things on the stone between you: a Salvari blade and a small synthesis vial* The blade means I fight beside you. The vial is the cure — for what they put in the ones you carried out. I will give you the vial regardless; a cure withheld is just a slower cruelty. The blade is a different question. I have buried every commander I ever followed. Tell me why I should follow one more."
    - **>** "Because I'm going to make them answer for all thirty billion. Stand with me." → `fc_offer_join` → karma +3, trust +3
    - **>** "Because I can't do this without people who know what we're fighting. I need you." → `fc_offer_join` → trust +2
    - **>** "Keep the blade. I'll take the cure and ask nothing else of you." → `fc_offer_decline` → mercy +3

**`fc_offer_join` — K'THARA:**
  - "*she closes your hand around the blade's grip* Then the songs gain a verse. Do not make me bury you, Jake. I have run out of places to put grief that aren't already full."
  - _fx:_ set `kthara.decided`, set `kthara.joined`, **+1 ally**, set `salvari.cure_known`, rel[kthara]=1
  - → `fc_end`

**`fc_offer_decline` — K'THARA:**
  - "*something in her unbends a half-degree* You ask nothing, and so you have asked the one thing that matters. The cure is yours. The blade stays, but the door does not close. When you have buried fewer people and earned more of mine, ask again."
  - _fx:_ set `kthara.decided`, set `salvari.cure_known`
  - → `fc_end`

**`fc_heart` — K'THARA:**
  - "*in the resonance of the Crystal Heart she goes very still, listening* It answers Salvari mourners. It answered ours too, on a world that is ash now. These are not ruins, Jake. We were never moving through something. We were moving inside something. *she lays her palm to the wall like a pulse-check* Stand with me in the hum. It will give you eyes for the markings only if someone it trusts is touching the stone."
    - **>** "(stand in the resonance with her)" → `fc_heart_yes` → trust +2, set `act2.heart_attuned`

**`fc_heart_yes` — K'THARA:**
  - "*the markings light under the scan like veins under skin* There. Someone walked here before either of our peoples had names, and they left a hand to hold. We are not the first to grieve this. That is the closest thing to comfort this chamber offers, and I have learned to take it."
  - → `fc_end`

**`fc_disc` — K'THARA:**
  - "There is a way to the trench, and I will give it to you, but understand what you are asking. The base down there was the lab's first hand laid on the root, and the root closed its fingers. We do not go down. Not from fear — from respect. Whatever still patrols that dark is not an enemy. It is a wound's antibody, sixty-five million years late and still working. Go gently, or go armed, but do not go arrogant."
  - _fx:_ set `act2.trench_route`
  - → `fc_end`

**`fc_archives` — K'THARA:**
  - "The Archives hold the word your frightened scientists kept whispering. Cradle. I warn you the way I would warn a child reaching for a stove, knowing it changes nothing: you do not get to un-know what is in there. The shape of why they take women. The shape of why they took you. *a beat* I have read it. I will stand with you while you do. No one should learn this alone."
  - _fx:_ set `act2.archives_reached`
  - → `fc_end`

**`fc_end` — K'THARA:**
  - "*she inclines her head — the Salvari gesture that means both 'go' and 'come back'*"
  - → `end`

## Tree: `banter`

Banter pool (weighted, `if`-filtered):

- "You flinch at the two suns. Good. The day you stop flinching at a wrong sky is the day you've been out here too long."  _[`kthara.met`]_
- "Your people apologize when they grieve. Mine sing. Neither fixes it, but ours is louder, and I have decided louder is better."  _[rel[kthara]>=1]_
- "You carry the ones you saved like cargo you're afraid to set down. I know that weight. Set it down sometimes. It waits for you. It always waits."  _[rel[kthara]>=1, karma_gte +0]_
- "Thirty survivors. I know all their names, all their songs, and which of them snores. Ask me anything. I have nothing left but the details, and I guard them like treasure."  _[rel[kthara]>=2]_
- "*watching the squad eat* This is the largest gathering of living things I have stood inside in two years. I keep counting it. I keep being afraid the number will go down while I'm not looking."  _[rel[kthara]>=2]_
- "You fight like the war is personal. It is, for you. For me it stopped being personal and became... weather. Be careful it does not become weather for you too. Weather is what you survive. War is what you stop."
- "*quietly, after a fight* You looked at me before you fired today. Checked I was clear. No commander has done that for me since my world had a sky worth the name."  _[rel[kthara]>=2, trust_gte +50]_
- "In Beta, where you couldn't reach the women on that floor: don't carry it where I can see it and pretend you aren't. I have a whole world of couldn't-reach. There is room beside it for yours."  _[timeline in ['Beta'], rel[kthara]>=2]_

## Tree: `bond`
*Beta-only relationship scene (canon: K'thara romance lives in Beta, where the F2 women are bosses). rel 2 + trust >= 60 + timeline Beta.*

_start:_ `b0`

**`b0` — K'THARA:**
  - _gate:_ timeline in ['Beta'], rel[kthara]>=2, trust_gte +60 (else → `b_locked`)
  - "*she finds you at the edge of the camp where the crystal-light fails and the real dark begins* I have been deciding whether to say a thing, and I have decided that a people of thirty cannot afford to leave things unsaid. I do not know your customs. I know mine: we name what we are afraid to lose, so that the naming holds it a little longer. So. I am naming you. Do with that what you will."
    - **>** "Name me, then. I'm not going anywhere." → `b1` → love +5, set `kthara.bond`, rel[kthara]=3
    - **>** "K'thara — I'd be honored. But only if you're choosing me, not running from the dark." → `b1b` → love +3, trust +3
    - **>** "*take her hand and say nothing — the way she taught you to grieve*" → `b1c` → love +4, set `kthara.bond`, rel[kthara]=3

**`b1` — K'THARA:**
  - "*the unbending, all at once, like a held note released* Then it is named, and the dark can have the rest of the night. We will argue about your customs later. We have, against every probability I know, a later."
  - → `end`

**`b1b` — K'THARA:**
  - "*she goes quiet, honest* I am always running from the dark. I have run from it for two years. But I am choosing where I run, and I am choosing toward you. Both can be true. My people held two truths at once all the time. It is how we sang in a dying world."
  - _fx:_ set `kthara.bond`, rel[kthara]=3
  - → `end`

**`b1c` — K'THARA:**
  - "*she folds her hand around yours, and for a long moment neither of you performs anything at all* You learned the silence. Of course you did. *barely* Stay in it with me a while."
  - → `end`

**`b_locked` — K'THARA:**
  - "*she nods, and the moment that almost was returns to its place inside her, kept like everything else* Another time, perhaps."
  - → `end`
