# EMILY — Ward C, Floor 2 (Medical Bay)

**One line:** The conspiracy-aware scientist who traced the Cradle program for a year,
filed her concern to the desk that owned it, and survives terror by narrating it — her arc
is deciding whether the narration was bravery or the moment she became her own lab notes.

## Who she is
Dr. Emily Watson, research assistant, gene-integration team. Canon basis: Floor-2 Room C
captive → companion (tech/intel: alien translation, weakness ID) or boss "The Oracle"
(EFLZ_NARRATIVE §3–4; the Oracle's canon line "You could have saved all four with 87%
probability... you simply chose" is paid off verbatim in her infected_lost tree). Voice
extends the `girls_dialog.json` draft ("narrates the horror in clinical real-time") into a
named coping architecture: *observe yourself and you keep one inch of yourself.*

**Voice keys:** Bayesian idiom, footnotes herself, ranks everything ("category five"),
memo-drafting as intrusive thought. Distinct from Aria's clinical voice: Aria's numbers are
*caretaker's odds* (other people's), Emily's are *instruments* (her own armor).

## Trees (highlights)

### first_meeting
The interrupted variant opens mid-self-narration — she has been clinically describing her
own assault for hours to keep the inch, and her voice cracks only when the data changes:
*"Say something so I know which side of the data I'm on."* The uninterrupted variant has
her warning you off the door sensor before introducing herself. fm2 is the pack's biggest
single lore drop, gated to dialogue not cutscene: the program is named **Cradle**
(sets `cradle.known`), the funding has no origin, and — fm3 — the mothers are the
manufacturing line and *Jake is an investment, not an escapee*. The fm3c branch ("you
stayed?") is her wound: she'd decided understanding was protection. "I was the next girl in
Ward C. The understanding was not protection. Updated belief. Hard way."

### sidequest — THE MISSING VARIABLE
Three encrypted Cradle-ledger terminals: Genetics (F3), the contract room (F4), the
hive-mind chamber (F5) — all canon rooms (EFLZ_WORLD_STRUCTURE §2). Mechanical reward per
terminal: `bestiary_intel` events ("a bestiary written by the devil's own QA department" —
hooks the MonsterDef weakness data). Narrative reward at completion: **the harvest reveal**
(`harvest.known`) — the six months were not imprisonment, they were genome harvesting; the
program breeds *officers*, command-capable hybrids, and command requires Jake's
free-will-retaining anomaly. Everything they build has a Jake-shaped flaw. This is the
dialog-side payoff of STORYLINE_EXPANSIONS thread (a)+(b) and recontextualizes the Clone.

### trust → bond
The peer-review scene: "does it read as brave or as broken?" All three answers are honest;
even "Honestly? It unsettles me sometimes" *raises* trust, because the flinch is the data
and yours never came.

### romance — the disclosure (now a full arc)
Her declaration stays a conflict-of-interest disclosure, not a demand: *"I'm not asking for
a result. I'm disclosing a conflict of interest."* Accepting starts the long study;
declining files itself under good results and the collaboration genuinely stands. What was
an opened door now continues through desire/night/afterglow — in HER shape (cerebral,
self-narrating, wry mid-passion), never Aria's or Keisha's.

### desire — reporting the variable (heat 4/5, gate: `emily.romance`)
Nine re-reads of the same terminal line, tripled error rate, and a formal anomaly report:
"the confounding variable is your hands. Hypothesis: they would be significantly better
deployed on me." The d1b branch ("Describe the simulation, Dr. Watson") gets the phase-one/
phase-two/phase-three breakdown ending in "I've simulated phase three two hundred times and
I still can't predict your face." Decline is a null session, no penalty to the model — "good
hypotheses always keep."

### night — controlled conditions (heat 5/5, gate: rel ≥ 3 + `emily.romance` + `emily.desire` + `loc.private`)
She locks the door, checks it twice, then removes her glasses — "the most naked thing you
have ever watched anyone do." Consent is *formally administered*, in character, as a final
checkpoint. The scene's engine is her narration failing: she annotates right up to the point
where vocabulary goes nonverbal, and the woman who survived Ward C by never losing the
thread of her own sentence loses it gladly, on purpose. The Ward C inch-she-kept motif
turns over at the peak: *"the inch I kept — I'm giving it to you."* Aftermath delivers her
finding ("effect size: frankly embarrassing; replication: required, extensively"). Sets
`emily.night`, fires `romance_consummated`. Decline branch: the longitudinal conversational
study, falling asleep mid-sentence — zero cost.

### afterglow — day one of the long study (gate: `emily.night`, once)
0400, his shirt, her slate: she's writing it up. One visible line — "Day one of the long
study" — and a formal amendment: lifetime, subject to his review. Accepting it makes her
close the slate *without saving*, the most out-of-character act of trust she has. The other
branch births "item four," a running private code the banter pool then pays off.

### infected_lost — Oracle seed
The narration with the inch gone: "the subject understood the mechanism completely, and the
understanding changed nothing. Isn't that the cruelest finding in the file?"

## Flags: `emily.met / .interrupted / .told_cradle / .sq_active / .sq_solo_risk / .sq_done / .romance / .romance_declined / .desire / .night / .afterglow` · Globals set: `cradle.known`, `harvest.known` · Host flag read: `loc.private`

## Banter (romance-gated additions)
Six new lines: the eleven-seconds-of-blind-spot line (she does not elaborate; her ears
elaborate), hand-holding-improves-my-aim, the observer-effect walk complaint, the
fudged-odds-left-the-error-in confession, "Item four," and the unexplained-anomalies list
("you're on it four times — twice for last night alone").
