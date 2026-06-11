# DR. FELIX REYES — the neutral/ambiguous NPC (F1 Research Lab)

**One line:** A collaborator by cowardice and mortgage — junior geneticist who ran assays on
"tubes with barcodes instead of faces," holds real currency (codes, timer intel, the word
"Cradle"), and resolves four ways: ally, flee, betray, or dead.

## Design intent
The pack's morality instrument. The girls test compassion, Martinez tests righteousness;
Reyes tests **how you treat the complicit-but-pathetic** — and he's built so the
*instrumentally optimal* play (threaten him; fastest intel) is the one that can detonate
later, while mercy compounds. His placement (F1 Research Lab — an open room in the
HIDDEN_AREAS layout) puts him on the natural route before the Medical Bay he holds the
code for.

## What he's worth (the currency)
- **Medical Bay keypad code 4119** — the F1 locked-door system (HIDDEN_AREAS §2: Medical
  Bay = code only) gets its diegetic source. (Placeholder code per that doc's open item;
  trivially re-tuned.)
- **Ward assignments + live procedure windows** (`rescue_intel` event) — host can surface
  real timer numbers on the rescue HUD, a concrete mechanical reward.
- **"Cradle"** — independently corroborates Emily (two unconnected NPCs naming the same
  buried memo is how the conspiracy is made to feel real).
- **Ally branch bonus:** the "Bride suite" line — Jake's cell and one other room share a
  separate power loop. Seeds Sarah/F7 and expansion (a)'s "you were an investment" thread
  *and* the Gamma "The Bride" naming, fifteen floors early, as a chill rather than a reveal.

## The four outcomes
| Branch | How | Consequence |
|---|---|---|
| **ALLY** (`reyes.deal`) | take the escort deal | full intel + Bride-suite lore + `ally` +1; he pledges testimony (Act-4 collaborator-trials hook) |
| **FLEE** (`reyes.fled`) | give him an exit | full intel + karma; he resurfaces at **Club 1127** with the batch-records guilt-gift (the `club_reunion` tree — evidence item usable in expansion-(f) ending variants) |
| **BETRAY** (`reyes.betrayed`) | threaten him, then leave with karma < 0 | he sells you to the duty station — pre-staged elite team at the F3 landing. The node text spells the mechanism: *you made yourself the scarier unknown* |
| **DEAD** | kill him any time | karma hit; badge yields the code but no schedule, no Cradle corroboration, no club thread |

The betrayal is deliberately **conditional on karma**, not on the threat alone — cowards
pick the safest master available; if your reputation (karma ≥ 0) reads safer than the
program, he stares at the alarm key and doesn't press it. "Coward cuts both ways."

## Voice keys
Org-chart hedging ("Civilian — well, contractor — well—"), transaction grammar ("a buyer or
a tax"), the everyone-has-a-thing confession (the humming man on Five), and the thesis line
at the club: *"the worst part of cowardice as a career? The benefits are real. You DO
survive. You just arrive places like this, alive, with nobody to drink with."*

## Flags: `reyes.met / .threatened / .deal / .ally / .fled / .gave_under_threat / .betrayed / .will_testify` · Items: `reyes_badge`, `reyes_batch_records` · Globals: `cradle.known`
