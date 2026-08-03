# Lane 4 — city block / lot / frontage generator, before & after

Headless captures (`--world echotropolis --screenshot ... --shot-cam ...`), one
binary, one camera per pair. `ECHO_CITY_LEGACY=1` selects the pre-V8 placement
(the four polar hash rings + the `corridorHits` veto that cleaned up after
them); unset selects the lot/frontage placement. Everything else — terrain,
time of day, road network, camera — is identical across each pair.

| pair | camera (`--shot-cam`) | what it shows |
|---|---|---|
| `aerial_crown_{before,after}.png` | `-20,700,760,0.0,-1.5707` | the crown mesa. BEFORE: four concentric arcs of houses scattered across bare ground, no road within 300 m of most of them. AFTER: nothing out on the mesa; every building is on the street grid at frame left. |
| `aerial_harbor_{before,after}.png` | `350,420,520,0.0,-1.5707` | the harbour grid. BEFORE: bare streets — the rings are 500 m away at the crown, so the actual city grid had no buildings on it at all. AFTER: both sides of every street are built, block interiors are open. |
| `street_{before,after}.png` | `296,72,500,0.0,-0.10` | street level. BEFORE: empty road, the 20 hand-traced waterfront towers marching off to the left. AFTER: a street wall — every building square to the kerb, `yaw = lot.frontYaw`, zero jitter. |

## Two capture-only caveats — read these before judging the images

**1. The buildings are BLOCKOUT MASSES, not the real art.** The building packs
(`HouseForge`, `Urban Night City`) live under `D:/Assets/_glb/...` on the
authoring box and are not in the repo. `ECHO_CITY_PROXY=1` (off by default)
draws a box at each building's exact footprint, yaw and terrain seat instead.
So the images are honest about *where* and *which way* buildings go, and say
nothing about how they look.

**2. The terrain is a STAND-IN.** `assets/island_mesa/island_{height_,}20260530`
are Git-LFS pointers whose blobs live only on the fleet's self-hosted Gitea
(`192.168.7.23:3000`), which needs an access token this machine does not have
(`git lfs pull` -> `{"Message":"Unauthorized"}`; GitHub's mirror 404s the oid).
`EchoRoads::build()` refuses to run without a heightfield, so there would
otherwise be no road network and no capture at all. The stand-in reproduces the
shape the road module was authored against — a crown mesa at `(-20,760)` and a
shoreline through the nine `kShoreSeed` points — so the boulevard probe, the
rim probe and the harbour grid all find something real. Generators:
`scratchpad/mkhf.py` / `mkglb.py` (not committed; capture-only). **Both sides of
every pair use it**, so the comparison is valid; absolute terrain shapes are
not.

## Honest read of the AFTER images

* The harbour pair is the strongest evidence: the streets that the road module
  had been drawing since V2 finally have a city on them.
* The crown pair reads as a **loss of content** as much as a gain, and that is
  real: the frontage walk only builds where there is a road, and most of the
  crown mesa has none. The rings were filling that space with houses facing an
  imaginary circular road. Emptier and correct beats full and wrong, but
  somebody has to author crown streets before that area reads as a city.
* In `street_after.png` several buildings on the right still visibly overhang
  their slope. The four-corner seat stops them sinking and the plinth (the
  darker base under each mass) fills most of the drop, but the plinth only
  reaches the lowest of the five probes — on a steep slope the ground keeps
  falling away outside the footprint. Unfinished.
