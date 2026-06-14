# Chat tree — DR. REYES (`reyes`)

> ADDITIVE Act-2 tree for DR. FELIX REYES (merged into dr_reyes.json by npc id — same loader slot, one new tree, no overwrite). The club_hideout scene: the fled collaborator (reyes.fled) resurfaces at Club 1127 and finally hands over the gestation-cohort batch records. His voice is unchanged from the base file — rationalization mid-collapse, a coward given one chance to be briefly useful.

## Tree: `club_hideout`
*Fired from act2_batch_records o1 (after Vesper locates him). Resolves into the witness / deal / extort branches the mission reads.*

_start:_ `rh0`

**`rh0` — DR. REYES:**
  - _gate:_ `reyes.fled`
  - "*he doesn't startle; he's been waiting for you the way you wait for a diagnosis you already suspect* You found the bar. Of course you did. *he turns the glass without drinking it* I keep telling myself I came down here to disappear. That's the lie I drink to. The truth is I came down here so somebody who'd do something would eventually walk in. I just didn't want to be sober when they did. Sit. Or don't. I've practiced this speech to an empty stool for three nights."
    - **>** "I'm not here for the speech, Felix. I'm here for what you've got." → `rh1`
    - **>** "Then say it sober. You owe the people on that list that much." → `rh1b` → karma +2
    - **>** "You sold us once. Why would I believe you've got anything but excuses?" → `rh1c` → trust -1

**`rh1` — DR. REYES:**
  - "*he slides a battered data-slate across, fast, like ripping a bandage* Batch records. Every cohort. Every date. Every name that authorized a... a 'procurement.' *the word costs him* Take it before I find a reason not to. I've found forty reasons. I'm out of new ones."
    - **>** "This is testimony, Felix. Not a payment. You're going to stand behind it." → `rh_witness` → redemption +3, karma +3, set `reyes.will_testify`
    - **>** "Good enough. We're done here." → `rh_deal` → set `reyes.dealt`

**`rh1b` — DR. REYES:**
  - "*he flinches, then — slowly — pushes the glass away from himself* ...Sober. Right. *he straightens half an inch, which on Reyes is a revolution* I cross-referenced the ward assignments against the admission logs for eight months and I told myself the discrepancy was a clerical error every single morning. It wasn't. I knew. Here. *the slate* The records. And my name, near the bottom, where it belongs."
    - **>** "Then you'll testify to all of it. That's the deal." → `rh_witness` → redemption +4, karma +4, set `reyes.will_testify`
    - **>** "Take the slate and go. We're done." → `rh_deal` → set `reyes.dealt`

**`rh1c` — DR. REYES:**
  - "*the dig lands; he doesn't dodge it* You shouldn't believe me. That's the only honest thing I can offer, so I'm offering it. I'm a coward with a mortgage, not a convert. But cowards keep good records — it's all we're brave enough to do. *he sets the slate down between you, not pushing it, just... letting it be there* The records don't care if you trust the man who kept them."
    - **>** "Then keep being useful where it counts. Testify." → `rh_witness` → redemption +3, set `reyes.will_testify`
    - **>** "You're right. I don't trust you. Hand it over anyway." _[karma_lte -10]_ → `rh_extort` → set `reyes.coerced`, mercy -2
    - **>** "Take it. Get out of the rock while you still can." → `rh_deal` → set `reyes.dealt`, mercy +1

**`rh_witness` — DR. REYES:**
  - "*something almost like relief, which is the most uncomfortable thing on him yet* Testify. *he says it like trying on a coat that doesn't fit but might, someday* Okay. Okay. Felix Reyes, for the record. First name Felix, middle name... *a broken laugh* ...'reluctantly,' apparently. Fine. If there's ever a room where this matters, I'll be in it. Tell whoever's keeping the list that I'll keep it too. It's the only thing I was ever good at."
  - _fx:_ give `batch_records`, set `act2.reyes_talked`, fire `evidence_register`
  - → `end`

**`rh_extort` — DR. REYES:**
  - "*he hands it over with shaking hands, and the small dignity he'd half-found folds back up* Right. No, you're right, of course. Take it. I won't pretend this is brave now. *quietly* I hope it's worth more to you than it cost me. Most things in this building were."
  - _fx:_ give `batch_records`, set `act2.reyes_talked`, fire `evidence_register`
  - → `end`

**`rh_deal` — DR. REYES:**
  - "*he pockets nothing; there's nothing left to pocket* Done. *he picks the glass back up* I'll finish this and decide whether to run further or just stop running. Don't tell me which I should do. I've had enough men in lab coats tell me what to do with the rest of my life. *a beat, almost a thank-you* You came down here. Most wouldn't have. That's... noted. By me. For whatever I'm worth, which we've established."
  - _fx:_ give `batch_records`, set `act2.reyes_talked`, fire `evidence_register`
  - → `end`
