# THE SMALL MOUNTAIN TOWN — placement manifest (W-TOWN, Lane 4)

*What stands where, from which pack, and every number that decides it. Written
2026-08-17 alongside `app/town.{h,cpp}`; REVISED the same day when the first
kit turned out to be a ruined-village kit and the pedestrians turned out to
include a monster (sections 2 and 6 carry both receipts). Companion law:
`docs/NO_SLOP.md`, `docs/design/X3_WORLD_RULES.md`,
`.claude/skills/x3native-environments/SKILL.md`.*

---

## 1. THE SITE — why here

`docs/design/ROAD_NETWORK_SKETCH_V2.png` labels a brown **"Small Mountain
Town"** centre-east, hanging off a **yellow ladder-switchback road** between the
Large Mountain and the river.

The network already contains that ladder, and the plan already named the site:

| source | says |
|---|---|
| `app/road_network.h` `registerSummitSpur` | the ONLY switchback climb in the network |
| `app/forest.cpp:384-395` | hard-codes the spur peak `(393, 6752)` as *"the sketch's spiral-road mountain family"* |
| `docs/design/ROAD_NETWORK_PLAN.md:701` | *"Town 2 — the climb foot, where the inner tour meets the summit road."* |

So the town is **laid along the summit spur's lowest, gentlest reach** and is
handed the spur's registered `RoadSpec` + graded datum by the host — it is
never given hard-coded coordinates, so it follows the spur wherever the
hill-climb puts it.

**As built (boot log, 2026-08-17):**

```
summit spur: 1.44 miles UP - climbs 247 ft to a 349 ft peak at (393, 6752),
             junction at (-101, 4439) | max grade 14.0% | 53 nodes
town: 17 buildings, 23 props, 10 parked cars, 34 lit windows, 10 lamps
      along 690 m of main street (u 70..760);
      centre (-20.6, 15.7, 4817.5); 3 lots rejected (slope/overlap)
town: 6 pedestrians on a 52-node sidewalk loop
```

**TOWN CENTRE / MapPoi anchor: `x = -20.6, y = 15.7, z = 4817.5`.**
Main street runs from about `(-95, 4460)` to `(60, 5130)`.

---

## 2. THE PACK — swapped, and the two traps in it

**Round one's kit was a ruin, and this is the fix.** The town was first built
from the armory's **HouseForge** prefabs. Eyes-on at full res they read as
DERELICT — dark, spiky, broken silhouettes down the whole street — because that
kit is authored as **collapsed ruins**. `docs/design/TOWN_ASSET_SCOUT.md`
reached the same verdict independently, and the armory's own thumbnail bakes
render them the same way. Geometry, textures, scale and grounding were all
correct; the *kit* was wrong. Section 8 predicted the swap would be "an edit to
two tables", and that is what it turned out to be.

Everything structural now comes from **ONE** licensed pack,
`Complete Racing Game URP All in One`
(`\\p13700\G\Assets\...\Racing_Game\Models\Level_Design\Models`). One pack means
one register, and that pack was authored for a **driving game** — so the town
belongs to this world's road network instead of being a medieval village a
freeway happens to pass.

| role | source | notes |
|---|---|---|
| house shells | `Red_House/HighPoly/House_1..4.fbx` | 15–37 m wide, 9–25.7 m tall, pitched roofs, dormers, chimneys, porches |
| street lamp | `Light 2/Light_2.fbx` | 7.16 m highway standard; replaces round one's medieval torch |
| roadside signs | `Billboards/Billboard_1..2.fbx` | 2.3–2.4 m |
| fence | `Red_House/HighPoly/Wood_Fence.fbx` | 5.6 m picket run |
| benches | `nature/SM_WoodBench_01a`, `nature/SM_Bench` | already in-tree, unchanged |
| parked cars | `Vehicles/` converted fleet | unchanged |

**Eight facades from four shells.** Four meshes is thin for a 20-plot street,
and repeating a mesh is the uniform lattice the x3native-environments skill
forbids. The pack's UVs are authored to **tile** (`Wall` spans u −6.7…7.7), so
each shell is baked in two real photographic paints — RED clapboard
(`Wall.tif`) with scalloped shingle (`Roof_2.tif`), and WHITE clapboard
(`Wood.jpg`) with plank roof (`Roof_1.tif`), both over a `Brick.tif` base.
Variety by **material**, which is how a real street of one builder's houses
actually looks. The whole kit is **6.5 MB**, down from HouseForge's ~120 MB.

### The two traps `tools/town_assets.py` exists to defuse

1. **The pack ships NO Unity material metadata.** `find … -name '*.mat'` returns
   **0** across the entire pack, and there are no `.meta` files either — so
   `tools/convert_unity_pack.py`'s GUID→texture resolution has nothing to
   resolve, and FBX2glTF emits **1×1 white placeholder PNGs** for every slot
   (`Warning: could not find a image file for texture`). Shipping that is a
   flat-grey town, NO_SLOP rule 3. The tool injects the pack's real albedos
   **by MATERIAL NAME** instead; the artist's own naming makes it unambiguous
   (`Wall`→`Wall.tif`, `Base`→`Brick.tif`, `Roof`→`Roof_*.tif`).
2. **stb_image cannot read TIFF.** `engine/asset/ModelLoader.cpp:639` decodes
   embedded images with `stbi_load_from_memory`, and three of the four
   wall/roof albedos are `.tif`. They are transcoded to JPEG at bake time.
   (Same class as round one's WebP: there is no WebP decoder anywhere in the
   tree either.)

`python tools/town_assets.py verify` asserts **all** of it — no
`extensionsRequired`, every bound image PNG/JPEG in a bufferView, no bound
image smaller than 2×2 (the placeholder test), no untextured material at
metallic ≥ 0.9 (X3_WORLD_RULES rule 5, the black-prop law). **GREEN on all 12
assets.** Eleven `MaterialOverride` entries that round one needed are simply
gone: every material of every asset is painted from a photograph, so there is
no gap left to patch.

### Rejected, with reasons (measured, not assumed)

The scout recommended this pack's `Medium_Building_*` / `Tower_*` for shops.
**Measuring them says no**, and the correction is worth recording:

| asset | measured | verdict |
|---|---|---|
| `Medium_Building_2/3/4/…` | 45–76 m tall | 15–25 storey office blocks, not main-street shops |
| `Tower_1/2/5` | **464–618 m tall** | skyscrapers |
| `Buildings _Night/Building_1..7` | **0 × 0 × 0 — empty meshes** | no geometry at all; the scout's list had these as usable |
| `Parking_2` | 46 × 20 × 46 m | plausible, but no town lot is big enough |

Their `Base` (main wall) material also has **no texture anywhere in the pack** —
only the window and shopfront atlases ship — so even at the right scale they
would have needed an invented wall. `Shop.jpg` / `Shop_2.jpg` are genuinely
excellent lit-storefront photographs and remain available if a future pass wants
a commercial building; nothing in this kit uses them yet.

---

## 3. THE ASSET TABLE — measured, per X3_WORLD_RULES rule 0/3/4

Regenerate with `python tools/town_assets.py report`. These are the numbers
`kAssets[]` in `app/town.cpp` carries; **if the kit is re-baked, re-run it and
update both in the same commit** (NO_SLOP rule 4 — paired values are one value).

| asset | W × H × D (m) | bbox centre (x,z) | loY | **front (engine yaw)** | front measured from |
|---|---|---|---|---|---|
| House_1_Red / _White | 23.00 × 15.75 × 18.48 | −0.95, −0.13 | 0.00 | **+101.9°** | `Door_Garage` centroid |
| House_2_Red / _White | 27.50 × 9.00 × 14.88 | −0.05, −0.41 | 0.00 | **180.0°** | **eyes-on** (see below) |
| House_3_Red / _White | 14.98 × 15.86 × 25.00 | −0.60, +1.25 | 0.00 | **−0.6°** | `Door_Garage` centroid |
| House_4_Red / _White | 37.52 × 25.72 × 25.11 | −3.73, +0.50 | −0.08 | **−179.5°** | `Door_2` centroid |
| Light_2 (lamp) | 1.14 × 7.16 × 1.14 | +0.12, 0.00 | 0.00 | symmetric | — |
| Wood_Fence | 5.56 × 2.32 × 0.30 | +0.06, −0.14 | 0.00 | symmetric | — |
| Billboard_1 / _2 | ≈0.3 × 2.3–2.5 × 1.7–2.1 | — | −0.4 / −0.6 | symmetric | — |

**Scale is real, and was checked rather than assumed.** House_4 is 25.7 m tall,
which looks like a unit error until you measure the glazing: the individual
window panes run 0.9–1.6 m wide and 1.1–3.0 m tall — ordinary windows. The kit
is genuinely in metres (X3_WORLD_RULES rule 1); House_4 is simply a big lodge,
and it is used **once**, as the square's hero.

**Four things this table exists to prevent:**

1. **The shells are not centred on their origin.** `Town::build` addresses lots
   by **bbox centre** and backs the offset out at placement.
2. **`loY ≈ 0` on this kit** — unlike HouseForge, which modelled a plinth below
   the origin. The placer's `−loY` lift is therefore a no-op here; it is kept
   because the placer must stay correct for any kit.
3. **The front is measured from the DOOR material, in tiers.** `House_3`'s slot
   named `Door` is *not* a door — its UVs run u −5.15…6.15, i.e. a tiling clad
   wall — and averaging it dragged the measured front round to the blank flank.
   Only `Door_Garage` / `Door_2` (0…1 UVs, real single openings) are trusted
   first. **Caught by rendering the result and looking at it**, not by reading
   the name.
4. **`House_2` has no door material at all.** Its entrance was found by
   rendering the mesh at 0/90/180/270 with `tools/glb_contact_sheet.py` and
   looking: a centred door with a porch step on the 180° elevation, while the
   `Glass` fallback had pointed at −121°, a blank gable. The override is
   recorded in `FRONT_OVERRIDE` in `tools/town_assets.py` **with the reason**,
   so the measurement stays honest about what overrode it.

### The lit windows are measured per WINDOW

`kAssets[].win[3]` carries, for each of up to three real windows on the front
elevation, `{height above bbox bottom, offset along the wall, DEPTH from bbox
centre, half width, half height}` — obtained by clustering the shell's own
front-facing `Glass` triangles into connected components.

Three things had to be got right, and each was got wrong first:

* **Per-window, not per-storey.** Averaging a storey's glass puts the pane on
  the blank wall *between* two windows — the capture showed exactly that, grey
  cards stuck to the clapboard.
* **ε = 1.0 m.** The kit's windows are **mullioned**, glazed as grids of
  ~0.74 × 0.22 m sub-panes about 0.8 m apart, so a tighter epsilon returns 67
  single mullions instead of 5 windows.
* **Depth is measured, not the bbox support.** These houses have deep eaves; on
  House_1 the wall sits **6.9 m inside** the bbox front, so panes placed at the
  support plane hung in mid-air. The glass knows where the wall is.

Sized to the opening and set 6 cm in, an **unlit** pane is invisible against the
dark glass behind it — which is what makes the day shots clean.

---

## 4. THE LAYOUT — curated, not a lattice

`kLots[]` / `kProps[]` / `kParks[]` in `app/town.cpp` are hand-authored tables
keyed on **arc length `u` along the street**, side (±1), lateral setback, and a
per-plot yaw jitter. The tables are written against `u` 70…690 and rescaled onto
whatever reach the host gives, so the town stretches or compresses with the spur
rather than falling off the end of it.

| reach | character | plots |
|---|---|---|
| lower town (u 78–158) | loose and rural, deep setbacks (24–28 m) | House_2_White, House_1_White, House_3_Red, House_2_Red |
| main street (u 172–286) | fronting the pavement at 18.5–20 m, 18–26 m of clear ground between neighbours | House_1_Red, House_2_Red, House_3_White ×2, House_2_White, House_1_Red, House_3_Red |
| **the square** (u≈306) | House_4_Red as the hero, set back at 24 m | House_4_Red + House_2_White + House_1_White |
| upper street (u 366–490) | climbing and tightening (19–21.5 m) | House_3_White, House_3_Red, House_2_Red, House_1_Red, House_3_Red, House_2_Red |
| outskirts (u 520–630) | thinning toward the switchbacks (25–30 m) | House_1_White, House_3_White, House_2_White, House_1_Red |

**`lat` is the setback of the FRONT FACE**, not the distance to the bbox centre,
and that is a bug fix rather than a rename. Under the old semantics a plot at
lat 18.4 holding an asset with a 9.2 m front support put its facade **9.2 m off
the centreline — inside `kPavedHalfM` (14.63 m)**, i.e. a building standing in
the road. The placer now adds each asset's own measured front support, so the
keep-out is enforced on the facade, which is what a keep-out is for.

The square's hero is keyed on a **lot INDEX** (`kHeroLot`), not an asset id:
with eight facades drawn from four shells every asset repeats, and an id match
would silently aim the eye gate at whichever plot came last.

Curation rules the tables encode by hand: **varied massing** (nothing repeats
twice in a row), **varied setback** (shops 19–21 m, houses 26–34 m, stalls
18.5 m), **varied gaps** (26–62 m, wide at the ends, tight through the middle),
**broken alignment** (every plot carries its own yaw jitter, so no two facades
are parallel).

**Street furniture** (`kProps[]`): 10 of the pack's 7.16 m highway lamp
standards alternating sides at 17.8 m (round one used medieval torches — the
ruined kit's register), 3 armory benches, 3 picket-fence runs closing the gaps
between houses, and 2 roadside billboards at the approaches. **Parked cars** (`kParks[]`): 10 of the
converted fleet (`Vehicles/E30, Pickup, Jeep, Coupe, Muscle, Truck, M3_E36,
E46_New`), angle-parked nose-in at 17.9 m with ±58–63° skew off the street
tangent.

---

## 5. THE LAWS THE PLACER ENFORCES

**KEEP-OUT (the road is never built on).** `kStreetKeepOutM = 17.5 m` from the
centreline. The spur is a base-profile road: `kPavedHalfM` 14.63 m of pavement +
shoulder + apron, and `RoadSpec::halfWidth` carves a metre past that. 17.5 m
clears the carve edge and leaves the verge for the sidewalk. Nothing — building,
prop, cart, car — is ever placed inside it. The junction mouth at the spur's
foot is a second keep-out disc of `kJunctionSetbackM`.

**THE TOWNSITE PLANE.** A main street is *graded*. Each plot's ground is
`clamp(min-of-5-terrain-samples, datum − 1.6 m, datum + 1.2 m)`. Both bounds
were paid for:

* *Upper bound* — corridors only ever CUT (`road_network.h`), so beside a
  climbing spur the natural ground sits metres **above** the datum. Round one
  grounded on raw terrain and the whole town vanished behind its own bank; only
  roof spikes showed above the grass.
* *Lower bound* — round two clamped only the upper side and the square's hero
  landed in a **9 m hollow** below its own street.

**CONTACT (rule 4).** Five terrain samples per plot — four rotated footprint
corners plus the centre. The plot sits on the **lowest**, so nothing floats; a
plot whose corners disagree by more than 4.5 m is **rejected** (3 were). The
origin is then lifted by `−loY` so the modelled plinth rests on the ground, and
sunk 0.5 m so a slope shows no gap under the sill.

**COLLISION.** One yaw-rotated static box per building (`addBox` + `setBodyRotation`,
the `app/world_cars.cpp` precedent), footprint inset 0.35 m, spanning the visible
body only — so the car cannot drive through a house. Anything with a
half-footprint under 1.6 m — lamps, benches, fences, signs — gets **no** collider: an invisible wall on a sidewalk is worse than
driving through a barrel. Parked cars get the same 0.85 × 0.62 × 1.95 box
`world_cars.cpp` uses.

**NIGHT (rule 5).** Up to three lit panes per house, each cut to a MEASURED
window and set 6 cm into the MEASURED wall depth (see section 3 — the bbox front
is not the wall; these houses have up to 6.9 m of eaves). Flat emissive **0.42**
over a near-black albedo —
the ACES law says anything past ~0.5 clips to a white slab — with a warm
practical behind each pane, and one at the head of every 7.16 m lamp standard (range scales off the measured lamp height, so a different lamp cannot silently under-light the street). `Town::setNight(k)`
rebuilds each light from its **authored** colour, never from its current one, so
repeated calls cannot compound.

---

## 6. THE LIVES — and the monster that was walking Main Street

Six pedestrians on a 52-node closed sidewalk loop (up the −side apron at
`kSidewalkLatM`, back down the +side). Each is an **`AnimatedCharacter`**
(`app/character_anim.h`, the one character-rig runtime) driven by its own
`Player` capsule.

### The cast was wrong, and only the eye gate could show it

The town used to spawn from `CrowdSkin::defaultRigs()` — `AnnaCasual_anim`,
`marcus_webb_anim`, `chief_martinez_anim`. Rendering that roster
(`python tools/glb_contact_sheet.py assets/rigged_glb/<rig>.glb`) shows what it
actually is:

| rig | what it is |
|---|---|
| `AnnaCasual_anim` | a woman in a crop top and shorts — a civilian |
| `chief_martinez_anim` | a black-clad SWAT / tactical operator |
| `marcus_webb_anim` | **a clawed, green-veined mutant** |

Two of the three people strolling Main Street were a special-forces officer and
a monster. `crowd_skin.cpp:35` selects purely on "carries Idle/Walk/Run" and was
cast for the **club** scene; nothing in it asks whether a character belongs in
the world using it. It survived because no capture had ever framed a walker
close enough to see one — the moment the pedestrian gate did, it was obvious
(NO_SLOP rule 2).

**crowd_skin's roster is left alone** — other worlds legitimately want those
rigs. The town casts its own.

### The townspeople

`tools/town_people.py` builds six civilians from the licensed
**`City People FREE Samples`** pack (Denys Almaral), found with
`python tools/unitypackage_index.py --search "City People"` — it was among the
~700 packages that had never been extracted to the share, which is exactly what
that index exists for.

`CityPerson_ManCasual`, `CityPerson_WomanCasual`, `CityPerson_ManJacket`,
`CityPerson_WomanCoat`, `CityPerson_Elder`, `CityPerson_Boy` — 0.9 m to 1.8 m
tall, cycled so the street is not six clones.

The pipeline, each step reusing something that already existed:

1. read the `.unitypackage` directly (a gzipped tar of `<guid>/asset` +
   `<guid>/pathname`) and pull only the meshes, clips and atlas needed — no
   Unity, no full extraction;
2. `FBX2glTF` the mesh and each single-clip animation FBX (the pack ships one
   shared 33-node skeleton across all of them);
3. inject `people_pal.png` on the `peopleColors` material — without it every
   pedestrian ships on FBX2glTF's 1×1 white placeholder, the same trap the
   buildings hit (NO_SLOP rule 3);
4. **`node tools/glb-merge-anims.mjs`** fuses mesh + clips into one GLB and
   names the clips. **That tool already existed in the tree for exactly this
   shape of pack** (`tools/README-glb-merge.md`) — NO_SLOP rule 1.

**`townPedClipTable()` exists because labels are untrusted**, and the clip names
are PAIRED with it (rule 4): the rigs are baked as `Idle` / `Walk` / `Run` /
`LookAround` because that is what the table asks for and what
`AnimatedCharacter` resolves by EXACT name. `jakeClipTable()`'s names are
`Walking` / `Running`; the wrong table yields a sliding idle-only statue and no
error. `python tools/town_people.py verify` asserts the clips, the skin and the
bound texture on every rig — **GREEN**, and the boot log confirms it per rig
(`idle=0 walk=1 run=2`).

**Honest note on register:** these are low-poly, palette-shaded people against
photographic clapboard houses. That is a style seam, and it is a deliberate
trade — plainly-dressed townspeople at the right scale beat a photoreal monster.
A future pass wanting photoreal civilians should mine the library again.

**THE CONTACT LAW** is enforced inside `AnimatedCharacter::update` — feet clamp
to `max(terrain height, downward static raycast)` every frame; no host can ship
a buried walker. The town's part of the bargain is the **`kPedActiveM` = 320 m
gate**: beyond it the terrain tiles the walkers stand on are not resident, and
ticking them is a free-fall, a raycast and a log line each, every frame, for
something nobody can see. The gate lives inside `Town::update`, and the capture
settle loop calls it too — an unticked `AnimatedCharacter` is a bind-pose statue,
which is the T-pose defect NO_SLOP rule 1 catalogues.

### Where the pavement is

`kSidewalkLatM` is **13.6 m**, and it was 16.4 m. `RoadSpec::halfWidth` is
`kPavedHalfM + 1` = **15.63 m**, and the corridor only carves the height field
out to there — so at 16.4 m the loop ran on the **raw hillside**, which beside a
road that cuts 34 ft is the batter slope. The walkers climbed a mud bank, and
the gate camera, grounded on the same terrain, was buried in it. 13.6 m is on
the apron (`kShoulderHalfM` 8.53 m … `kPavedHalfM` 14.63 m), so the ground under
it is the graded road surface, with 5 m still between a walker and the running
lanes. **PAIRED with `kPavedHalfM` / `RoadSpec::halfWidth`.**

---

## 7. THE GATE

`X3Engine.exe --screenshot-town <dir>` — five stills, cameras **derived from the
town's own placement data** (`Town::showcaseCamera`), never typed in, because
ENGINE_GOTCHAS 4.1 is explicit that hand-picked cameras end up inside walls.
Latest set: `shots_town/`, boot clean, **0 `[ERROR]`**, 19 buildings, 22 props,
10 parked cars, 21 lit windows, 10 lamps, 6 pedestrians.

| # | shot |
|---|---|
| 01 | main street from the road, at driver eye height |
| 02 | the square's hero facade, close enough to judge the textures |
| 03 | the pedestrians, on the pavement |
| 04 | the lit windows at dusk (the sun is dropped to the horizon for this frame only) |
| 05 | the town from across the valley |

### Deriving a camera is not enough — it has to be derived from EMPTY ground

Three of the five were wrong on the first pass, all the same way, and the fixes
are the useful part:

* **04 stood inside a house.** It sat 30 m off the centreline on the +side
  normal, and +side lots sit at a bbox centre of 26.9 m — the whole frame was
  the underside of somebody's roof. It now stands **on the roadway**, the only
  ground in this town guaranteed clear: `kStreetKeepOutM` exists to keep it so.
* **03 photographed a field, twice.** First it grounded itself on raw terrain at
  the old 16.4 m sidewalk and ended up in the cut bank. Then, re-derived from a
  real pedestrian, it stood **8.5 m** in front of one — and the settle loop runs
  60 frames at a 1.35 m/s gait, so the walker covers ~8.5 m and walks straight
  past the camera before the shutter opens. It now stands **20 m** down the
  walker's **own heading** (bearing to its next waypoint — half the loop runs
  against the street tangent) and looks back, so the settle walks the subject
  *into* frame. The 20 m is derived from that measured 8.5 m, not chosen.
* **02 gave 45 % of the frame to blurred tarmac.** Raised to a standing 3.1 m
  and pitched slightly **up** at the building, which is the subject.

### The night dial had no driver

`Town::setNight` defaulted to **1** and nothing ever called it, so every window
in the town burned at full strength under a noon sun — eyes-on, the panes read
as pale tan cards glued to the clapboard. The default is now **0 (day)**, and
`Town::setNightFromSun(sunDir.y)` pairs the dial to the sky (full day above
0.25, full night at or below 0.05) everywhere the host pushes `SkyParams`. Sun
elevation and window glow are one value (NO_SLOP rule 4).

---

## 7b. THE FPS GATE — measured, and told straight

A/B on a **quiet GPU** (every sibling lane's `X3Engine.exe` confirmed gone
first), `X3_TOWN=0` vs `X3_TOWN=1`, identical `--shot-cam` poses, 2 reps each,
60 settled frames per rep, 1280×720. The numbers are the mean of the two reps.

| camera | off (ms) | on (ms) | Δ | off fps | on fps | **fps as % of baseline** | Δ as % of a 165 fps frame |
|---|---|---|---|---|---|---|---|
| 01 main street, driver eye | 0.719 | 0.829 | +0.110 | 1391 | 1206 | **86.7 %** | 1.8 % |
| 04 the square at dusk | 2.092 | 2.161 | +0.069 | 478 | 463 | **96.8 %** | 1.1 % |
| 05 the town from the valley | 0.961 | 1.146 | +0.185 | 1041 | 873 | **83.9 %** | 3.1 % |

**Two of the three cameras miss a literal "≥ 90 % of baseline" reading, and that
is stated rather than hidden.** But the ratio is the wrong instrument here and
the absolute number says so: the town costs **0.07–0.19 ms of GPU per frame**.
The baseline frames it is being divided into are 0.7–2.1 ms — at 1391 fps, one
tenth of a millisecond *is* 13 %. Against the owner's actual benchmark
("Huge open world… 165 fps" = a 6.06 ms frame) the same 0.07–0.19 ms is
**1.1 %–3.1 % of the frame budget**, and the town never moves the frame time by
as much as a fifth of a millisecond in any pose measured.

The dusk/square camera — the one where the town is closest, largest, and
carrying all 21 lit panes plus 31 point lights — is the *cheapest* in relative
terms (96.8 %), because that frame is already doing real work. The worst ratio
is the valley shot, where the baseline frame is nearly empty and the town adds
280 draw calls of distant houses to it.

**Where the cost is: draw calls, not triangles.** Shot 05 adds 277 k triangles
(+31 %) but **+281 draws** (108 → 389), because the EnvArt overlay issues one
draw per placed instance. If this ever needs to come down, instancing or a
merged static batch per GLB is the lever — not fewer or smaller buildings.

Recorded honestly per NO_SLOP rule 9: every number above came from a
measurement on an idle machine, and the two sub-90 % readings are real.

---

## 8. OPEN — what is still not done

The art call that section 8 used to carry is **CLOSED**: the derelict kit is
gone, every asset in the town is textured from a real photograph, and
`tools/town_assets.py verify` + `tools/town_people.py verify` are both GREEN.
What remains open:

1. **Two of three fps ratios are below 90 % of baseline** (section 7b). The
   absolute cost is 0.07–0.19 ms and the ratio is an artefact of a 0.7 ms
   baseline frame, but the literal gate is not met and the lever is known:
   the EnvArt overlay issues **one draw per instance** (+281 draws on the valley
   shot). Instancing or a merged static batch per GLB is the fix if it matters.
2. **Style seam on the pedestrians.** Low-poly palette-shaded civilians against
   photographic clapboard. Deliberate (section 6), but a photoreal civilian pack
   would close it — the 914-package index is now built and searchable.
3. **No commercial building.** The town is residential. The racing pack's
   `Shop.jpg` / `Shop_2.jpg` are excellent lit-storefront photographs with no
   suitable shell to sit on (its own buildings are 45–76 m towers, section 2).
   A shop needs either a right-sized shell from elsewhere in the library or the
   storefront atlas applied to a purpose-built low-rise.
4. **6 of 25 authored lots are rejected** at build time by the overlap/slope
   ledger. That is the ledger doing its job on a hillside, but the table could
   be re-tuned so the intent and the result match more closely.
5. **The lit panes are per-window but not pixel-perfect.** They sit on the
   measured centre of a real window at its measured depth; on a couple of
   facades they read slightly proud of the frame. Unlit they are invisible, so
   this only shows at dusk.

---

## 9. MapPoi

Lane 7 (W-MAP) had not landed a `MapPoi` header at this lane's close
(`grep -rn MapPoi` finds nothing outside `docs/plans/SEVEN_LANE_PLAN.md:122`).
When it lands, register:

```
MapPoi{ "Mountain Town", x = -20.6f, z = 4817.5f, icon = town }
```

The live value is `Town::centerX()` / `centerZ()` / `centerY()`, so the
registration should read those rather than copy the literals — the spur is
hill-climbed at boot and the centre moves with it.
