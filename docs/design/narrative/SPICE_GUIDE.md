# SPICE_GUIDE — heat authoring rules for EFLZ chat trees

One page for anyone extending the romance content. The dial for the romance arcs is **4–5
chilis**; everything else in the game stays where the original pack put it.

## The heat ladder (what gates each chili level)

| 🌶 | What it reads like | Where it's allowed | Gate (exact conditions) |
|---|---|---|---|
| 1 | Warmth, gratitude, charged glances | first_meeting tails, early banter | `rel >= 1` |
| 2 | Flirt with intent, charged touches, innuendo | banter, trust scenes | `rel >= 2` |
| 3 | Declarations, first kiss, named desire | `romance` trees, Vesper's whole thread | `rel >= 3` + axis gate (girl-specific) + **explicit player choice** |
| 4 | Slow-burn escalation: hands, breath, want on the page; ends on an invitation, not an act | `desire` trees, romance-gated banter | `<girl>.romance` flag + explicit choice |
| 5 | Private culmination: carries into the act and through its aftermath; explicit-adjacent, anatomical poetry over clinical terms | `night` trees ONLY | `rel >= 3` + `<girl>.romance` + `<girl>.desire` + **`loc.private`** (host-set flag, safe rooms / quarters only) + explicit threshold choice |

After a `night` scene: one `afterglow` scene (warm, character-deepening, references the
night **specifically** — pay off a motif, don't gesture vaguely), then banter lines gated on
`<girl>.night` keep the romance alive in moment-to-moment play.

## The consent grammar (non-negotiable)

- **Consent IS the erotics.** Every escalation is invited, asked for, or enthusiastically
  met. These women choose Jake; their agency is the heat. Each `night` scene contains an
  explicit threshold beat, in the character's own voice (Aria's "last consult," Emily's
  "formally administered checkpoint," Keisha's "last call to object," Lena's "last chance
  to reroute").
- **Declining costs NOTHING.** No rel loss, no love loss, no soured banter, no flags that
  punish. Decline branches are written as their own small intimacies (hold my watch / talk
  until we sleep / hold the room) and the scene re-offers later. Respect reads as strength.
- **Every step is a player choice.** No auto-advancing into heat. The spine never routes
  through a 4–5 chili node.
- Heat is **earned deep in the arcs**: never in first meetings, rescue beats, or before the
  romance is explicitly accepted.

## The trauma-stays-restrained rule

The captivity/assault/menace material keeps the original pack's restraint — implication,
aftermath, voice — and that dial **never moves**. No spice anywhere near the wards, the
straps, the interrupted rescues, or the infected_lost trees. The two registers must never
blur: the building takes by force; everything Jake is given is given freely. That contrast
is the whole point of the game's intimacy.

## Per-girl voice notes (heat must be character-true, never interchangeable)

- **ARIA** — clinical language melting into tenderness. She flirts in vitals and dosage,
  loves in charts, and the consummation beat is her *losing count on purpose*. Her hands
  are the POV. Motifs: the count, the night shift, the trauma kit set out of reach, 41 names.
- **KEISHA** — fierce, physical, commanding; the Marine takes point. Sparring cadence,
  orders with plans in them, teeth in the promises. Off-duty is her intimacy: the
  watch-the-door posture standing down IS the undressing. Motifs: stay loud, the cardamom
  kitchen, ninety-minute sleep, dog tags. (Lane closes with grace if Torres is found alive.)
- **EMILY** — cerebral, narrating her own undoing, wry mid-passion. She consents in
  protocol language and combusts when the vocabulary fails. Never let her stop being funny.
  Motifs: the inch she kept, hypotheses/replication, item four, glasses off = naked.
- **LENA** — transactional armor cracking; touch is the thing she never lets herself buy.
  Fewest words, highest price per word. She comes apart *quiet* (screaming's spending). Heat
  lives in the stage directions, not speeches. Motifs: tolls/ledgers, maps (she charts him),
  Marco's tags, runners leave before light — she stays.
- **VESPER** — the exception that proves the ladder: 3–4 chilis, all sizzle and dangerous
  wit, **never consummated**. The tease is her currency; the day she pours it, it's gone.
  Escalate the dance, never the destination. (`vesper.flirt1` → `flirt2` → the standing f2.)

## Technical notes

- Flags per girl: `.romance` (arc accepted) → `.desire` (escalation accepted) → `.night`
  (consummated) → `.afterglow` (morning scene played). All set by accept-branch `fx` only.
- `loc.private` is an ordinary story flag the **host** sets/clears when the player is in a
  safe room or quarters. No new condition/effect ops exist or are needed.
- `night` completion fires `{"fire": "romance_consummated", "args": {"npc": "<girl>"}}` for
  the Lua layer (ambience, achievements, companion mood).
- Write normal prose; the dialog runner ASCII-folds smart punctuation.
- Validate with `tools/check_chattrees.py` — every file must parse and every ref must resolve.
