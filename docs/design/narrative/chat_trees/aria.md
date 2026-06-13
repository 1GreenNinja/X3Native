# ARIA — Ward A, Floor 2 (Medical Bay)

**One line:** The medic who has nursed the aftermath of this program on other women — her
horror is knowledge, her armor is caretaking, her arc is learning to be carried.

## Who she is
Night-rotation medical technician, Lab Zero staff. She noticed patients in the med system
with no admission records — forty-one of them — and filed one honest report up the chain.
The report became her transfer order to Ward A. Canon basis: "Medical Technician Aria,"
Floor-2 Room A captive → companion (healer/medic) or boss "The Siren" (EFLZ_NARRATIVE §3–4);
voice seeded from `staging/girls_dialog.json` ("soft, frightened civilian") but aged up from
victim to *witness* — she's soft, not weak.

**Voice keys:** clinical numbers used as emotional armor ("I knew my own odds. To the
percent."); night-shift idiom; says thank-you by taking on work (grabs the trauma kit
without asking). Never raises her voice — her "calm" is the tell that she's at her limit.

## Trees (script form, default path)

### first_meeting — the interrupted-rescue variant (flag `aria.interrupted`)
She was saved mid-attack. What she says first is *"Is it off me. Yes or no."* — she will not
look at the dead thing, and she polices the language ("don't describe it"). The
uninterrupted variant (`fm0_alt`) is wary instead of raw: she's been listening to footsteps
for an hour deciding what you were. Both routes converge on fm2, the triage beat — she
forces the game's own moral mechanic into the dialog: *"If your math says you can't reach
everyone... I've done that math for other people. Don't ask me to do it for you."* The
player's three answers (promise everyone / honest triage / "you're the one in front of me")
set karma and the `aria.heard_promise` flag, which the host can pay off brutally if a girl
is later lost. Spine ends at fm4 — `follow: true` — with her thesis line: *"Stay where I can
see your back. That's the part of you I can keep alive."*

### banter — companion pool
Caretaking observations (she reloads the med kit when your breathing changes), morgue-log
grief held at arm's length, cross-companion lines if Keisha/Emily are saved, and one
world-building dagger about the elevator muzak (*"Someone chose the key it's in."*).

### sidequest — THE MANIFEST
Recover her original 41-name log from morgue-office terminal three, Floor 1 medical wing
(fits the HIDDEN_AREAS F1 layout: morgue is in the Medical Wing Corridor's side rooms).
Reward: the `aria_manifest` **item** — it doubles as the *evidence* that enables Martinez's
talk-down path (see martinez.json), rel → 2, karma +5. Refusing it ("it's paper") is
permitted and costs quietly: love −5 and a silence the .json renders as stage direction.

### trust → bond (rel 3)
The recognition scene: the worst part of the table wasn't fear, it was *recognition* — she'd
held hands on that table and lied kindly at known odds. All three player responses land; the
best one ("And then I broke your odds") earns her keeping you "as a rounding error. The
good kind."

### romance (rel 4, opt-in, declinable)
The bandage scene. She confesses by clinical confession ("more often than is clinically
defensible") and breaks her own night-shift rule with both eyes open. Declining is written
with full dignity — *"I'm not made of glass — I'm made of night shifts"* — keeps Bond, sets
`aria.romance_declined`. Canon note: in Omega she pairs with Dr. David Chang
(EFLZ_NARRATIVE §3 support-tier); Jake-romance is the Alpha-family lane, which this gate
respects (Alpha/pre-lock only).

### desire — the off-the-clock scene (heat 4/5, gate: `aria.romance`)
The exam that stops being an exam: her hands keep charting after medicine is done, and she
diagnoses *herself* out loud ("Resting heart rate: elevated... That was my diagnosis of me").
Two accept branches — "stop counting" and "the recommended treatment" — both end on the same
invitation: *find me a locked door.* Declining ("Not tonight, Aria") costs nothing, sets no
flag, and the scene re-offers later; her exit line is pure her: "The night shift is patient.
It's the whole job description."

### night — the culmination (heat 5/5, gate: rel ≥ 3 + `aria.romance` + `aria.desire` + host-set `loc.private`)
She throws the bolt herself and puts the trauma kit out of reach — the scene's whole thesis
in one stage direction. Explicit threshold consent ("say yes like you mean it, or say
nothing and I'll hold your watch instead — both answers keep you"). The scene carries
through the act in her register — she memorizes him *whole* instead of injured, and the
counting motif pays off at the peak: she loses count, on purpose, for the first time in her
careful life. In-scene aftermath (n3) sets `aria.night`, +10 love, fires
`romance_consummated`. The decline branch (hold my watch) is written as its own small
intimacy — "Near is its own medicine" — zero cost, re-offers.

### afterglow — the new chart (gate: `aria.night`, once)
Morning: she's started "a new chart — one patient, lifetime care plan," and the 41 names
motif closes its loop: *"I carry forty-one names out of this place. Yours is the one I get
to keep saying."* She hums while packing the kit now. Aria hums now.

### banter (romance-gated additions)
Six new lines once romanced: the two-beats-too-long pulse check, the belt-hook in passing
("Withdrawn — I'm not sorry"), greedy-about-your-heartbeat, the steady-hands confession,
and two remembered-night lines (the kissed scars chart; "I plan to go on being the reason
you don't" rest).

### infected_lost — the Siren seed
If her timer expires, her boss-intro lines invert her caretaking: *"Come here — let me take
care of you. I was always... so good... at taking care of people."* Hooks the existing
victim→boss machine; fires `boss_intro` for Lua.

## Flags: `aria.met / .interrupted / .heard_promise / .sq_active / .sq_done / .sq_refused / .romance / .romance_declined / .desire / .night / .afterglow` · Item: `aria_manifest` · Host flag read: `loc.private`
