# Chat tree — STEWARD (`nordic_steward`)

> THE NORDIC STEWARD — a second-species ally seated at the L11 Salvari-camp upgrade station (canon-aliens: the Nordic Steward, a tall, calm, human-passing species the lore casts as old-hand caretakers). Not a refugee — a long-timer who has watched this war on many worlds and stayed useful instead of bitter. He mentors Jake (gear + technique) and later vouches for him to the Underground Resistance. Voice: dry, economical, faintly amused, the patience of someone who has buried his own urgency.

## Tree: `mentor`
*Fired from act2_refugee_haven (m0) and act2_many_hands (m_resistance).*

_start:_ `m0`

**`m0` — STEWARD:**
  - "*he doesn't look up from the bench at first; he's seating a power cell into something that wasn't built for it and is about to be* You're the one who came down the cave singing of dead Reptilians. The camp likes you. The camp likes anyone who reduces the local predator population. I'm harder to impress. *now he looks up* I can hand you gear, or I can teach you to deserve it. The gear's the same either way. The teaching is free and worth more. Your choice."
    - **>** "Teach me. I'll take everything you've got." → `m_teach` → trust +2, set `steward.mentored`
    - **>** "Just the gear. I learn fast enough getting shot at." → `m_gear` → set `steward.gear_only`
    - **>** "Who are you, and why are you the one rationing alien weapons in a refugee camp?" → `m_who`

**`m_who` — STEWARD:**
  - "I'm the man who was here before the refugees and will be here after, which on a world this size makes me the closest thing to a road sign. My people keep. That's the whole of it — we outlast, we maintain, we hand the next desperate generation the tools and a little sense. I've done it on four worlds the Overlord has since erased. *a thin smile* I'm very good at it and it has never once been enough. Now. Teaching, or gear?"
    - **>** "Teach me." → `m_teach` → trust +2, set `steward.mentored`
    - **>** "Gear." → `m_gear` → set `steward.gear_only`

**`m_teach` — STEWARD:**
  - "*he hands you the over-charged thing off the bench; it hums wrong and hits right* Good. First lesson, which is also the only lesson, repeated: you fight like a man who only just learned he's allowed to. There's a flinch before every kill where you ask permission. Lose it. Not the conscience — keep that, it's the expensive part — lose the *flinch*. The conscience you spend later, in the quiet. The flinch just gets people behind you killed."
  - _fx:_ give `alien_upgrade`
  - → `m_end`

**`m_gear` — STEWARD:**
  - "*he passes it over without ceremony* Don't point it at anything you'd regret on a world that's running out of things to regret. It'll do the rest. Come back if you change your mind about the teaching. The bench is always here. So, increasingly, am I."
  - _fx:_ give `alien_upgrade`
  - → `m_end`

**`m_resistance` — STEWARD:**
  - _gate:_ `act2.alliance_formed` (else → `m_resistance_vouch`)
  - "*he's already there when you arrive, of course he is* They'll let you in because I said your name and I don't waste names. That gets you through the door. The room you earn yourself. A dozen species down there, every one of them certain their grief is the heaviest. Don't take a side in the grief contest. Take a side in the *war*. It's the only argument that ends."
  - _fx:_ set `act2.resistance_met`
  - → `m_end`

**`m_resistance_vouch` — STEWARD:**
  - "*he's already there when you arrive* I sent ahead. They'll hear you out — that's all I can buy you, and it cost more than you'd think. A dozen species, one agreement: they hate the Overlord. Your job is to find the second thing they agree on. There always is one. Usually it's that they're tired. Tired is a foundation. Build on tired."
  - _fx:_ set `act2.resistance_met`
  - → `m_end`

**`m_end` — STEWARD:**
  - "*back to the bench, the conversation already filed away* Mm. Go be useful. It's the only thing that helps, and it never helps enough. Both true. Get used to both."
  - → `end`

## Tree: `banter`

Banter pool (weighted, `if`-filtered):

- "You're collecting people. I noticed. It's a good instinct and a heavy one. I stopped doing it three worlds ago. Don't ask me if I was right to."  _[`steward.mentored`]_
- "The Salvari sing their dead. My people just keep the lights on for them. Different liturgies, same church."  _[`salvari.met`]_
- "You lost the flinch. I can see it. Now mind you don't lose the thing the flinch was guarding. That part doesn't come back when you go looking."  _[`steward.mentored`, karma_lte -10]_
- "Four worlds, and I've never seen one with two suns get a happy ending. *a beat* I'd be glad to be wrong. I'm so rarely glad. Give me the chance."
