# MARTINEZ — Floor 1 Boss, pre-fight confrontation + spare scene

**One line:** The honest door of a dishonest building — eight years of perfect compliance
kept deliberately "outside the briefing tier," whose talk-down is possible but priced like
one: karma + evidence + three correct beats, no threats, no lies.

## Canon honored
- **Floor 1, locked** (Tim 2026-05-22; EFLZ_BESTIARY conflict ledger → F1).
- Canon profile kept: ex-special-forces, cybernetic augments (the red eye + servo arm are
  his phase-2 tells, pre-seeded here), "believes he's guarding legitimate research,"
  3-phase arc Professional → Doubt → Revelation, **can be killed or spared**, drops
  Elevator Keycard / Journal / Family-photo-adjacent humanity / Custom Pistol.
- This tree does NOT replace the boss machine. It is the doorway: most outcomes `end:
  "fight"` into the existing `BossPhase` fight. The talk-down is the rare ending.

## The talk-down path (hard by design)
Three gates, all must hold:
1. **Engagement** — open with truth or a question (p0 choices 1/3); any taunt line is an
   immediate `p_fight_taunt` collapse, in-character ("aggression indices elevated — first
   thing this facility's told me all day that checks out").
2. **Evidence item** — `aria_manifest` (Aria's sidequest reward) or `chen_video_log`
   (findable in Chen's F1 office per TASK_9's hidden-keycard room). Testimony without paper
   fails sympathetically (`p_almost`): "Without paper, you're just the most articulate
   escapee I've ever processed."
3. **Karma ≥ 20 + the right final beat** — "Come WITH me. Check the wards yourself" is the
   only line that gives him a *role* instead of a surrender. "Walk away" fails even with
   evidence — the tree's thesis: you cannot ask this man to walk out of his own skeleton;
   you can only re-task him.

**Talk-down payoff:** keycard + journal, +1 ally, every F1 door released (his command
override), `martinez.talked_down` — and a downstream thread: he goes hunting the names for
his "noose," which STORYLINE_EXPANSIONS (a) pays off (he surfaces evidence pointing at the
Quartermaster; optional Act-4 resistance cameo honoring the canon "dies learning the
truth" as *dies acting on it* if the writer wants the tragic close).

## Texture choices worth keeping
- He knows Jake's file: *pilot, shot down, salvaged* — the first NPC to confirm the cold
  open diegetically, six minutes into the game.
- The evidence scenes use one name ("Mercer," also on Aria's manifest — the two quests
  cross-corroborate) and the signature motif: *"They used MY signature as the lid on this."*
- `p_fight_judged`: telling him the hard truth ("you wore the uniform for eight years") is
  morally honest and STILL gets the fight — "guilt doesn't make us stand down. It makes us
  need the trial." Choices differ in tone and consequence, not correctness theater.

## Spare scene (post-fight, canon P3 Revelation)
He asks for the slate — "a man should read the whole charge sheet against himself." The s1
branch hands the pack its connective tissue: his last request is **for Aria** — "tell her
somebody finally read them" (`martinez.aria_message`, which unlocks a unique Aria banter
line the host can add: the report that got her taken, finally answered).

## Flags: `martinez.engaged / .shown_evidence / .talked_down / .spared / .aria_message` · Items in: `aria_manifest`, `chen_video_log` · Items out: `martinez_keycard`, `martinez_journal`, `martinez_pistol`
