# LENA — Floor 5 (Drone Manufacturing) captive

**One line:** The street-smart scavenger — a deep-tunnel courier who came UP to steal meds
and got caught; the only captive who *chose* to be in the building, pays every debt in
codes and maps, and is the default candidate for the carrier/mole arc.

## Who she is
Canon basis: the unnamed-then-named F5 captive ("Lena," `staging/girls_dialog.json`: "quiet,
tough survivor — few words, deep loyalty once saved") + the engine-spec deep tunnels / alien
tubes / Club 1127 vertical canon (X3_WORLD_BLUEPRINT §2.6). Expanded: she's a tunnel-runner
from the under-world economy — antibiotics move like currency down there — whose brother
Marco drowned on the flooded level (his death is why her maps are good: "every line on them
is paid for"). She is the dialog system's *world-navigation* voice the way Emily is its
lore voice: tunnels, liar panels, the bottom door with music behind it.

**Voice keys:** transactional grammar (everything costs; screaming's spending; questions
have prices), sentences cut to the bone, "bottom questions cost the most." Her tells: when
something costs HER to say, it's the most expensive thing in the scene — and the
infected/carrier corruption manifests as *spending words freely*, the precise inversion.

## Trees (highlights)

### first_meeting
Interrupted variant: no scream — she's checking the corpse like a live wire, then "Knife.
Left ankle. They never check the left ankle." Uninterrupted: she's half through picking her
own cuffs with drone filament. fm2 is the keystone *reverse interview*: she owes you one
free answer and the question you pick is the test. "What's under this building?" buys the
tunnel cosmology (tubes older than the lab — *the lab plugged INTO them*); "How do I get to
Floor 7 fast?" buys route intel; **"Are you okay?"** spends your free question on *her* —
the highest-value choice (karma, love, `lena.asked_her`), because down-tunnel nobody ever
asks. Spine ends `follow: true`: "You're the gun, I'm the map."

### banter
Navigation gifts (dead cameras, warm-cold-warm vents), the Static name-drop that pre-seeds
Club 1127, the cell-terminal breadcrumb toward code 1278, Marco at rel 3 — and **one
carrier-gated line** ("They drift wrong lately. Like they're listening to something further
down.") that doubles as a mole-arc breadcrumb hiding in plain sight.

### sidequest — THE CACHE
Routes the player into the F1 under-hall (the hidden yellow route, bookshelf elevator,
bio-lab survivors — all from HIDDEN_AREAS_AND_BIOMESH.md, surfaced as dialog instead of
stumble-upon). The cache holds meds, Marco's tags, and the hand-drawn tunnel map item.
The sq1c branch explicitly seeds the substrate-scan gate ("it wants a signal I don't
carry... something under the skin") — the bio-mesh system, foreshadowed in-fiction.

### trust — THE CODE SCENE (rel 3)
Gated on the **mercy** axis (she watches how you treat surrendering guards and hiding
scientists — that's her ledger). She settles her debt with the most expensive thing she
carries: **1278**, drawn in dust and wiped, plus the sighting that explains it — a man who
came out of the floor of a locked cell, never on any roster: *the Architect* (Club 1127's
builder; see STORYLINE_EXPANSIONS d). Fires `dialog_hint {code:1278}` — the Lua side can
set the objective line exactly like `secret_room.lua` does today.

### romance — the settle-up problem (gate: rel 3 + love ≥ 70, declinable at zero cost)
Her loyalty arc now opens into a full romance lane, on her terms: she lays her knife down
like an opening bid and reports the bug in tunnel law — *"the ledger's clean... and I still
want my hands on you. No trade attached. There's no WORD for that down-tunnel. So you say
it."* Jake supplies the word; she tests "mine" like testing a rope, full weight. The r1b
branch (move the knife, take her hands) is wordless — she just doesn't let go for an hour,
"and Lena has never once held anything she wasn't ready to drop." Declining: she priced the
branch before she sat down; nothing sours, nothing is lost — "a night where nothing's lost
is a RICH night."

### desire — pricing his hands (heat 4/5, gate: `lena.romance`)
She catches herself watching openly — watching is information given away free, so from her
it's a siren. His hands are the one thing in the building she can't put a number on; one
finger on his wrist is a confession at full volume. The d1 branch is twelve years of
never-touching spending itself at once — "You're WARM. Nobody told me you get to just...
HAVE this." The d1b branch ("Take what's yours, runner") is the theft-kiss with his zipper
down as "down payment — I've already scouted three" locked doors. Decline: she reroutes
without drama and leaves the offer open, "first thing I ever put on the table without a
clock on it."

### night — load-bearing (heat 5/5, gate: rel ≥ 3 + `lena.romance` + `lena.desire` + `loc.private`)
Three rules at the threshold (never uncounted before; not counting tonight; "if I forget how
to stop holding on, you don't tell anyone"). She comes apart *quiet* — screaming's spending —
and the silence is louder than anything in the building; his name said once against his
throat is "the most expensive word she owns, paid in full." Aftermath: she's drawing on his
back, and it's a map — *of him* — "best terrain I ever charted. No tolls anywhere on it."
Marco gets his laugh; the cache line gets its echo: "Worth more than what it cost. You.
Always." Sets `lena.night`, fires `romance_consummated`. Decline: she drags her bedroll
flush against his for the first time ever — zero cost.

### afterglow — runners leave before light (gate: `lena.night`, once)
The whole scene is one fact: runners leave before light, and Lena is still there. New rule,
free: the route runs two-way now. The a1 branch ends on her leaving her knife inside
someone else's reach — "down-tunnel, that's the whole wedding." The a1b branch reveals the
cloth map's new marker by the under-hall: a small star and the word HOME.

### banter (romance-gated additions)
Six new lines: the empty-palm toll ("Pay it." — just to touch him), the locked-room
"trade intel" that is not trade intel, sharing heat because it's GOOD, pricing the hours his
hands aren't on her, the zero-length route ("Stand closer"), and Marco's tags that don't get
cold anymore.

### carrier_confront — the mole arc (player-discoverable)
If she was rescued at `InfectionStage::Critical`, the host silently sets `lena.carrier`.
Breadcrumbs accumulate (drones don't target her; she knows a door she shouldn't; a flat
second voice in her sleep). When the player confronts: she offers you her own knife,
grip-first, and asks you to read the one map she can't. Three resolutions — cure quest
(mercy path), **double agent** ("Runners carry packages without reading them... I carry
this one BACK at them" — with her own non-negotiable kill conditions), or the knife. The c3
node is written so the act is on the player, on camera, with her correcting your grip as a
last lesson. No resolution is free.

### infected_lost
The economy inverts: words spent freely, tolls abolished, "we weren't moving THROUGH
something, we were moving INSIDE something" — which is also a true statement about the
biomesh world (expansion e), making her lost-state a lore source players will reload to hear.

## Flags: `lena.met / .interrupted / .asked_tunnels / .asked_her / .told_tunnels / .sq_active / .sq_done / .marco_known / .carrier / .cure_path / .double_agent / .final_choice / .romance / .romance_declined / .desire / .night / .afterglow` · Items: `lena_tunnel_map` · Globals: `code.1278.known`, `mole.suspected`, `mole.weaponized` · Host flag read: `loc.private`
