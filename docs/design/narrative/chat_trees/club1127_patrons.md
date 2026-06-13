# CLUB 1127 PATRONS — Hollow Pete & Static

Two fixtures of the bottom bar (one JSON pack, two NPCs — they share the club's spawn/cull
cell). Together with Vesper they make Club 1127 a functioning rumor economy: Vesper sells
the big picture, Pete sells the warnings, Static sells the map.

---

## HOLLOW PETE (Officer Pellerin) — the Humanity-meter made flesh

**One line:** Ex-guard who pulled the lines out of his own arm and rabbited off the Floor-4
table a third of the way through — the walking, drinking consequence of the augmentation
system.

**Why he exists mechanically:** The F4 Humanity meter (canon: augments cost Humanity, too
many lock out good endings) needs a *diegetic warning* the player meets before the chairs
do their consent theater. Pete is that warning with a serial number filed off: "the chrome's
just the receipt"; "after that you're negotiating with your own missing pieces." Every
branch fires `rumor: humanity_cost / augment_warning` for the Lua layer to surface when the
player first sees an augmentation chair.

**Payload nodes:**
- `hp1b` — the port that's homesick for the thing that would have erased him. The pack's
  bleakest image; karma-rewarded for asking with kindness.
- `hp1c` — the collaborator's inventory, no absolution: and the **Martinez cross-light**
  ("Marty signs the transfer orders... he's got the same knee and a better view") — a
  second, independent witness to Martinez's engineered blindness, making the talk-down
  path's premise discoverable before the fight.
- `hp_intel` (return visit) — **the queue reveal**: when Pete fled, the next name on the
  augmentation board was *7-Alpha*. Jake was scheduled before he ever woke. Free of charge:
  "some things a man shouldn't have to buy." Feeds expansion (a)'s investment thread.

---

## STATIC (Nadia) — tunnel-runner, map-broker, Lena's old trainer

**One line:** The fastest courier on three levels "now that the little ghost's off the
board" — sells the deep maps that seed Act 2's caves, flooded levels, and ocean, and has
heard the Leviathan through three hundred meters of black water.

**Why she exists mechanically:** Act-2 and hidden-area seeding as merchandise. Her three
map tubes are pre-knowledge of canon spaces: the **flood level** (deep tunnels / maze
water), the **cave lines** (the −178 m Salvari-crystal caves — her "they're mail, waiting
for the right reader" is the crystals' story beat told as runner superstition), and the
**seafloor line** (the undersea base "the lab pretends it never built" + the
Leviathan — canon submarine-combat boss — rendered as a confession: "every fish-light for a
kilometer went dark out of RESPECT").

**Relationship logic:** Her opening assumes Lena is dead. If Lena is saved and the player
relays *"she's holding her own marker,"* the booth opens and the marker/ledger system
(tunnel law as emotional bookkeeping) becomes the pack's connective tissue. If the player
knows about Marco (`lena.marco_known`), Static gives the flood map free and opens the
**marco_marker** side quest — "somebody has to swim it home someday or the ledger never
closes. Take her with you. Don't let her say no." (A late-game closure scene for Lena that
the engine's water/swim systems can cash.) Without trust, the flood tube stays shut:
*"bring me proof you take care of your people."*

**The hum is wrong** (`st_loop`): her returning-visit greeting drifts — "two ticks fast,
like something turning over in its sleep" — a slow-burn escalation channel the host can key
to act progress, pointing at expansion (e).

## Flags: `pete.met / .warning_given / .martinez_known / .queue_reveal`, `static.met / .vouched / .marco_quest`, `rumor.leviathan / .crystals` · Items: `seafloor_chart`, `flood_map`
