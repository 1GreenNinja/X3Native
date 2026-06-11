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

## Flags: `lena.met / .interrupted / .asked_tunnels / .asked_her / .told_tunnels / .sq_active / .sq_done / .marco_known / .carrier / .cure_path / .double_agent / .final_choice` · Items: `lena_tunnel_map` · Globals: `code.1278.known`, `mole.suspected`, `mole.weaponized`
