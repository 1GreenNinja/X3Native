# Chat tree — QUARTERMASTER (`quartermaster`)

> THE QUARTERMASTER — the program's human procurement broker. Well-dressed, cold-metal-and-good-cologne (Vesper's tell). Sells the Dominion freight line its groceries: fuel, parts, acquisitions. He flagged Jake's ship for collection — and the order was pre-dated, cut before the flight plan was filed (STORYLINE_EXPANSIONS a). In Act 2 he is NEVER a boss; he is a ledger, a cologne, an empty barstool, and — at the Spaceport, cornered — a face. His voice is unbothered, civilized menace: the banality of a man who never once thought of his cargo as people. The seed of Act-4's Human Collaborators.

## Tree: `confront`
*Fired from act2_storm_runner o2_qm (only if quartermaster.ledger_held). The in-person spaceport confrontation. He is cornered, not fought.*

_start:_ `qc0`

**`qc0` — QUARTERMASTER:**
  - _gate:_ `quartermaster.ledger_held`
  - "*he sees you and, more to the point, sees what you're holding. He does not run. Men like him have never had to.* Ah. Subject Seven-Alpha. *he says the designation the way one reads a SKU* You have my ledger. I wondered who'd lifted it off the run. I'd have priced the theft higher if I'd known it was inventory walking off with the inventory list. *the cologne reaches you before the next sentence does* You're going to want to make a speech now. Please. I do enjoy them."
    - **>** "You flagged my ship before I ever filed a flight plan. Why me?" → `qc1`
    - **>** "Forty-one names in a manifest. Forty-seven thousand in the core. You signed for all of it." → `qc1b` → karma +1
    - **>** "(say nothing — let him fill the silence)" → `qc1c` → trust +1

**`qc1` — QUARTERMASTER:**
  - "Why you. *he sounds almost fond of the question* Because you were a line item that retained its will through integration — the only one on file. The program had been shopping for the carrier of that genome for years. I didn't find your ship, Seven-Alpha. I found your bloodwork, in a medical database a continent away, and I cut the order. The ship was just where the bloodwork was sitting at the time. *a small shrug* You weren't a target. You were a requisition that happened to have a pilot's license."
  - → `qc2`

**`qc1b` — QUARTERMASTER:**
  - "*he doesn't even pretend to weigh it* I signed for inventory. I have never signed for a person in my life — that would be sentimental, and sentiment is shrinkage. *he adjusts a cuff* You're describing a moral catastrophe. I'm describing a supply chain. We're both correct. The difference is I sleep, and you've come a very long way to a very bad ship to tell me I shouldn't."
  - → `qc2`

**`qc1c` — QUARTERMASTER:**
  - "*the silence works; it always does on men who mistake composure for safety* ...You're doing the quiet thing. The frightening men do the quiet thing. *the first hairline crack in the cologne* I move product, Seven-Alpha. I don't fight it. You've established you can take a ledger off a moving freight line — I'm not interested in establishing what else you can take. So. Let's be transactional. It's the only language I'm fluent in."
  - → `qc2`

**`qc2` — QUARTERMASTER:**
  - "Here's my offer, and it's a good one because I don't make bad ones: that book in your hand is worthless as a weapon and priceless as a witness. Kill me and you have a ledger with no one to read it. Let me walk, and someday — when there are rooms where this is judged — I will sit in one and name every name above mine. And there are names above mine. There are always names above mine. *he spreads his hands, an honest casino* What's it to be?"
    - **>** "You'll testify. Every name. Or the deal's off and so are you." → `qc_spare` → set `quartermaster.confronted`, set `quartermaster.spared`, set `quartermaster.witness`, mercy +3
    - **>** "You don't get to outlive the people in that book. (end it)" → `qc_kill` → set `quartermaster.confronted`, set `quartermaster.killed`, karma -3
    - **>** "I'm taking you in. Cuffs, not a coffin and not a handshake." _[karma_gte +10]_ → `qc_take` → set `quartermaster.confronted`, set `quartermaster.spared`, set `quartermaster.witness`, karma +4

**`qc_spare` — QUARTERMASTER:**
  - "*he exhales, and it's relief dressed as boredom* A pragmatist. Thank God. I was worried you were a hero. *he straightens his jacket* I'll be where the runners can find me, and when the room comes, I'll fill it with names. It's the only brave thing I'll ever do, and I'll do it to save my skin, and the people in your book won't care WHY their murderers got named. Neither should you. *he steps aside from the ramp.*"
  - → `end`

**`qc_kill` — QUARTERMASTER:**
  - "*for one instant the civilization drops and there's just a man who finally understood his cargo could look back* That's — that's not — *and then it's done, on camera, in your hand, the way these things are. The ledger in your other hand is just a book now. A book with no one left to read the names above his.* "
  - _fx:_ clear `quartermaster.witness`
  - → `end`

**`qc_take` — QUARTERMASTER:**
  - "*he looks at the cuffs like a man being handed his own obituary, neatly typed* You're going to make me LIVE through it. *a thin, awful laugh* Worse than the other thing, and you know it, and you did it anyway. *he offers his wrists* Fine. I'll name them all from a chair instead of a barstool. Lead on, Seven-Alpha. I do hope your war has good catering."
  - → `end`
