# KEISHA — Ward B, Floor 2 (Medical Bay)

**One line:** Ex-Marine facility guard taken FOR her combat genes — rage where others have
fear, command cadence as armor, and one unfinished thing named Torres.

## Who she is
Staff Sergeant Keisha Williams, ex-Corps, hired onto Lab Zero security. During the
"evacuation drill" that was actually a harvest sweep she dropped two guards before the gas
took her — which moved her to the top of the acquisition list (they wanted the genes that
fight back). Canon basis: "Security Specialist Keisha (Williams)," Floor-2 Room B,
companion (warrior) or boss "Breeder Queen"; James Torres = her canon Omega partner
(EFLZ_NARRATIVE §3). Voice seeded from `girls_dialog.json` ("fierce, defiant, swagger") and
given a spine: the swagger is *doctrine* — she converts fear to procedure in real time.

**Voice keys:** military brevity, inventory-speak ("a weapon, a direction, and one truthful
answer"), profanity-adjacent heat, jokes deployed as gifts. Her tell: identity recital
(name, service number, mother's kitchen, cardamom) — the thing she was saying goodbye to on
the table, and the thing the Breeder Queen variant has lost ("There was a kitchen. It's
quiet in there now.").

## Trees (highlights)

### first_meeting
Interrupted variant: she's still fighting the straps and her first demand is about the
*enemy* — "Tell me it felt something when it died." Uninterrupted variant: she's chewed a
strap half loose over forty minutes. The fm1c branch ("First tell me you're still you") is
the keystone: she answers with the identity recital and extracts the put-me-down promise
(`keisha.promise_asked`) — which the host can invoke devastatingly in the infected_lost
path or the mole arc. fm2 makes her the player's conscience with teeth: tell her you're
beelining to Sarah (`keisha.knows_sarah`) and she names her price — *"if a door screams when
we pass it, we are OPENING that door."* Spine ends `follow: true`.

### banter
Drill-instructor care (fixes your reload), the clean-nails-and-wedding-ring doc she's
saving for later, the 0300 strap-walks, cross-lines for Aria/Emily, and the thesis bark:
*"Stay loud, Jake. Quiet's what they make here."*

### sidequest — TORRES
Find Officer James Torres, "reassigned: sublevel storage" — which routes the player at the
canon Cryo Storage sub-level (SL2, wake-the-pods choice; EFLZ_WORLD_STRUCTURE §3b). Two
host-fired resolutions: `sq_found_alive` (Torres wakes; +ally; **gracefully closes her
romance lane** — canon Omega pairing honored, the `r_torres` node is one of the pack's best
scenes) or `sq_found_dead` (the folded manifest over her sternum; her romance lane stays
open and deepens). Asking "Was he just your partner?" earns the truth: *"He was the guy I
never got around to."*

### trust → bond
The 0300 debrief, delivered as a tactical briefing because that's the only container she
has: what they almost took wasn't her body, it was her *Sunday*. The t1c branch answers why
she chose Jake over Aria for it: "Aria would CARRY it... You'll just stand next to it with
me."

### romance
Field-stripping weapons, she delivers a "tactical assessment" that ends in *"You're my
everything-word."* The r1b branch ("Say it plain, Sergeant") gets the plainest declaration
in the pack. Declining is clean — "you get the second-best thing I've got, which is
everything minus one" — no soured banter. Gate: rel 3, love ≥ 70, **not** `torres_alive`.

### infected_lost — Breeder Queen seed
The horror is perfected cadence with no breath behind it, and the empty pocket where the
kitchen was.

## Flags: `keisha.met / .interrupted / .promise_asked / .knows_sarah / .sq_active / .sq_done / .torres_truth / .torres_alive / .torres_dead / .romance / .romance_declined / .romance_torres`
