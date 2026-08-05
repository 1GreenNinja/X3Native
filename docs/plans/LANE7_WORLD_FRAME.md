# LANE 7 — WORLD FRAME DECISION (8–16 km?) + fjord triage

Session: 2026-08-04, branch `lane7/terrain-frontier` (worktree off `echotropolis` @ `0bc0d482`).
Required reading order for the next Lane 7 session: this file, then `SESSION_LANES.md` §LANE 7,
then `.remember/remember.md` (Tim's iPad orders: bridge / bluff / sea-cave cavern / water rebake).

---

## 1. TRIAGE — what the fjord regen actually delivered (and where it lives)

**The brief's premise was wrong in one load-bearing way.** It said commit `6b5e0b18`
("REGENERATE ECHO HARBOR — fjord inlet, two-city cliff canon, committed generator")
"already landed in the fold". It did not land on our branch:

```
$ git merge-base --is-ancestor 6b5e0b18 HEAD   ->  NOT ancestor
$ git merge-base --is-ancestor HEAD 6b5e0b18   ->  YES
```

`6b5e0b18` is a **descendant** of `echotropolis` HEAD. It lives on `main` /
`origin/integration/inspx-all`, which is **1279 files / +65976 lines** ahead of
`echotropolis`. So:

* the fjord landform, the committed generator, and the `kRimSeamClear` roads fix are on `main`;
* `echotropolis` — the branch Lanes 1–5 have been shipping onto — still renders the **old
  mesa bake**. The two lineages have diverged and nobody has reconciled them.

**Also on `main` only** (relevant to Lane 7's remaining items): `51c11190 fix(city): reject
cliff-edge lots`, `f6084b64 feat(mines): ARMORY REBUILD` (real-rock mine kit + `--world mines`
+ an engine double-free fix), `app/cave_atmosphere.*`, `app/cave_river.h`.

### 1a. `assets/island_fjord` is a DEAD END — do not swap to it

The lane brief says "`assets/island_fjord` is staged; drive it via `ECHO_ISLAND_DIR`".
Driving it would be a **regression**. Evidence (hillshade render of the raw 16-bit heightfield,
plus Tim's own note in `.remember/remember.md` item 4):

| bake | date | res | landform verdict |
|---|---|---|---|
| `assets/island_fjord` | Jul 26 | 2048² @2 m | **BROKEN.** A hard straight vertical seam cuts the ocean a third of the way across; a corrugated ridged band runs down the west flank; the "inlet" is a blobby amoeba lagoon, not a gorge. Pre-`WATER_V2` — no in-frame water sheet. |
| `assets/island_mesa` (on-disk, Jul 29) | Jul 29 | 2048² @2 m | the old mesa bake `echotropolis` currently renders |
| **regenerated from `tools/echo_terrain_gen.py`** | deterministic | 1025² @4 m | **Tim's canon.** Winding cliff-walled inlet, 160–220 m walls, harbor basin, 190 m wall, crown plateau. |

### 1b. The generator is bit-exact — the "lost asset" risk is retired

`tools/echo_terrain_gen.py` was ported onto this branch and run on this box. Both artifacts
regenerate to **main's exact LFS oids**, whose blobs are *not downloaded anywhere on this fleet*:

```
heightfield PNG  cb28a30b0b60b394e79db88d53f9d4c6dc377dc82a43512a217f5008b7742c94  (1276568 B)  MATCH
land GLB         7f64d3668c831d98b8f74b664fc65ee241b79cb7113ddd474d47350bacba91e9  (27759896 B) MATCH
```

That is the single most valuable thing in this commit: **the terrain can never be lost again,
and it does not need LFS.** `--surface 1` (the default) is pinned to these hashes; a regression
check is one `sha256sum`.

### 1c. Navigability — the "boat lanes cross dry land" bug does NOT reproduce here

`.remember/remember.md` carries a no-refiling item: *boat lanes still cross dry land (galleon
through the village)*. Flood-fill of the regenerated bake from the open-ocean west edge, at a
−1.5 m keel draft (`echo_roads.cpp kWaterMinLand`):

```
navigable-from-ocean cells: 119431 of 119760 wet (99.7%)
boat lane A/B/C  start+end ......... all wet, all REACHABLE-FROM-OCEAN
harbor basin, waterfront seeds 0+8 . all wet, all REACHABLE-FROM-OCEAN
```

Ocean → waterfront shortest navigable route = **3,868 m (2.4 mi)**, one connected body of water.
So on **this** bake the lanes are sound; the dry-land crossings are an artefact of the *old* bake
the lanes were authored against. **This is the coastline-audit prerequisite Tim named** — it does
not need a new runtime audit *for water*, it needs the bake swapped. (Authored **land** placements
are a separate matter — see §4 risk.)

---

## 2. THE FRAME DECISION — recommendation: **stay at 4096 m now; the blocker is not the frame**

The frame is one constant: `kMeters = 4096.0f` in `app/world_hosts/echo_heightfield.h`, consumed
only by `Heightfield::heightAt()`. Changing the *number* is trivial. Changing the *world* is not.

Measured on the actual shipped bake (single land mesh + single baked albedo, as the pipeline
exists today), holding 4 m/px heightfield and 1 m/texel albedo:

| frame | across | heightfield RAM | land mesh | **single-mesh GLB** | **single albedo** | float32 ULP at edge |
|---|---|---|---|---|---|---|
| **4096 m** (today) | 2.5 mi | 2.1 MB | 513² = 0.26 M v / 0.52 M tri | **27.8 MB** (measured) | 4096² = 16.8 MB BC7 | 0.244 mm |
| 8192 m | 5.1 mi | 8.4 MB | 1025² = 1.05 M v / 2.10 M tri | ~111 MB | 8192² = 67 MB BC7 / 268 MB RGBA8 | 0.488 mm |
| 16384 m | 10.2 mi | 33.6 MB | 2049² = 4.20 M v / 8.39 M tri | **~444 MB** | 16384² = 268 MB BC7 / **1.07 GB RGBA8** | 0.977 mm |

**What these numbers say:**

1. **Float precision is NOT the blocker.** At a 16 km frame the worst-case coordinate is ±8192 m,
   where float32 ULP is **0.98 mm**. Rendering, physics and road math are all fine there. (Precision
   pain starts one to two orders of magnitude further out.) Do not let "float precision at range"
   be the reason we say no — it is not a real constraint at 16 km.
2. **Heightfield cost is NOT the blocker.** 33.6 MB of uint16 at 16 km is nothing, and the sampler
   is already bilinear-clamped and resolution-agnostic (`w`/`h` are read from the PNG).
3. **The blocker is that terrain ships as ONE mesh and ONE texture.** A 444 MB single-mesh GLB is
   unshippable, and a 16384² albedo is *exactly at* the Vulkan `maxImageDimension2D` cap on NVIDIA
   (16384) — there is no headroom above it at all. Even 8 km doubles us into a ~111 MB GLB that must
   be fully resident because the land mesh is one draw with no LOD and no chunking.

**RECOMMENDATION (this is a recommendation, not a silent pick — Tim decides):**

> **Do not scale the frame yet.** Scaling it is 1 constant + 1 generator re-run, and it buys nothing
> until terrain is **chunked** (per-tile mesh + per-tile albedo/virtual texture, streamed by the
> Lane 6 / Tier-2 streamer). Sequence: **(A)** land the fjord bake on `echotropolis`; **(B)** Lane 6
> lands M-C evictions + the per-pass timers; **(C)** *then* chunk the terrain bake — the generator
> already emits from one height array, so tiling it is a writer change, not a landform change;
> **(D)** *then* raise `kMeters` and re-run. Everything in `echo_terrain_gen.py` is authored in
> **world metres**, not pixels, so the landform survives the frame change unaltered.

**Cost of deferring:** Tim's "7 miles of waterways" stays compressed to a 2.4 mi navigable route,
and the desert corridor is capped at the ~2.05 km strip found in §3. Both are *good enough for v1*
and neither is wasted work — a chunked rebake re-uses the same generator.

**If Tim wants the miles now**, the cheapest honest path is **8192 m with chunked terrain**
(4×4 tiles of the current mesh/texture size = zero per-tile cost change, 111 MB total but only
~7 MB resident per tile), *not* 16384 m — 16 km forces a virtual-texture solution because a single
albedo is at the hardware cap.

---

## 3. DESERT CORRIDOR — site survey (built: terrain bed only)

`origin/feat/act2-desert*` (6 branches) were checked: **all carry gameplay only** — canon-alien
rosters, a Saurian warlord, a Grey drone swap, a Nordic NPC, and DamageType plumbing. Every one
touches only `app/act2_desert.{cpp,h}`, `canon_aliens.*`, `monster.*`, `weapon.*`. **No terrain, no
level geometry, no highway, no desert assets.** Nothing to salvage for a world; build from scratch.
(`app/act2_desert.cpp` on `main` is an *alien crystalline* desert — reusable as a host *pattern*, not as art.)

**Site chosen** by free-land scan (256 m cells, dry `h>5 m`, local relief `<25 m`, ≥600 m from
crown / mine / URBAN / RECIFE / HIVEMIND): the **NE band, x ≈ 1430–1690, z ≈ −1900 → +150**.

* length **≈ 2,050 m (1.27 mi)** — clears the "drivable desert mile" gate;
* natural grade: **93 m down to 10 m** north→south, i.e. a real highway descent, no earthworks;
* nearest existing content 756 m away at the south end, 2.4 km at the north.

**Built this session:** the desert **terrain bed** only — `--surface 2` retints that band to
sand/caliche with a soft gradient edge so the corridor reads as desert. See the honest critique in
`docs/screenshots/lane7-fjord/README.md`: it is a **featureless empty plain**. Bed, not environment.

**NOT built (next session, in this order):**

1. **Highway spur** off the ring's east side (ring wp `(980,560)`) → `(1450,150)` → south to
   `(1500,−1700)`. Must go through `EchoRoads`' graph so it inherits the zigzag law, junctions and
   collision — do **not** hand-author a strip mesh. Watch the law: the ring's worst curvature is
   already **0.795 °/m** against a 0.8 limit.
2. **Gas station** — `D:\Assets\_glb\prefab_buildings\Mega Open World City Pack\Assets\Mega City Environment\Models\Fuel_Station_Model.glb`
   (+ `SignBoard\SB_GasStation.glb`). Same pack has `RoadBarrier_Model`, `Street_Light_Model`,
   `BillBoard_Model`, `JunkYard_Model`, `Refinery_Model`, `Windmill_Model`, `BusStand01..03`, palms.
3. **Hidden mine set piece** — cherry-pick `f6084b64` from `main` first (real-rock kit, recessed
   bore, glowing mouth, 6 pooled lights, LOD chains, + the `destroyGraphics()` double-free fix).
   `echo_region_builders.cpp:760-855` is a working placement template; `GoldMineWorld::buildMouthGlow()`
   grafts the arch glow onto any mesh.
4. **Scatter, curated not lattice** — `D:\Assets\Wild West Low Poly Pack\...\Models\` (`cactus_01..03`,
   `rocks_01..04`, `sign_01/02`, `water_tank`, `shed`, `tunnel`); `D:\Assets\Metal and Concrete Barriers\...`
   (guardrail); `D:\Assets\North American Speed Signs - FREE\...` (US signage);
   `D:\Assets\Post-apocalypse Models Pack Mobile\...\gasstation_dmg.FBX` (weathered alt).

---

## 4. OPEN RISKS / HANDOFF

* **BLOCKER — authored placements break on the fjord bake. Photographed, not predicted.** Water is
  clean (§1c) but *land* placements are not. `docs/screenshots/lane7-fjord/sea_06_cityreveal.png`
  shows **a row of houses standing in the bay**, half-submerged; `sea_09_wall_dusk.png` shows a
  **cluster of glass towers hanging in mid-air** off the crown rim. Lane 5's waterfront row, hero
  houses and crown lots were all seated against the *old* mesa bake. `main` already needed
  `51c11190 fix(city): reject cliff-edge lots` for the same class of failure.
  **Do not land the fjord bake on `echotropolis` until a boot-time coastline/placement audit
  re-validates every authored position against the active heightfield (relocate or skip+log).**
  This **is** Tim's coastline-audit item, now a measured blocker with evidence attached.
* **Water surface seam is the #1 visual defect in every sea-level capture** — a hard horizontal line
  where the near water patch meets the far ring. Tim already filed this (Lane 5 water tier 2,
  "patch-vs-ring contrast at eye<140 m"); Lane 7 cannot fix it in the terrain bake.
* **Terrain has no detail/tiling texture.** The bake is 1 m/texel, which is mush at boat range —
  cliffs read as flat untextured mass in every close shot. This needs a *material/shader* detail
  layer, not a bigger albedo. Biggest single art win available for the fjord.
* **Mesh-skirt artifact**: a black rounded "tube" intrudes at frame right in `sea_02_narrows*.png`
  — the `SKIRT_Y = -8.0` rim skirt seen from inside the frame at sea level. Needs the skirt hidden
  or pushed out when the camera is inside the frame.
* **`--shot-cam` gotcha (cost me a full capture round):** the flag is parsed as two argv entries.
  `--shot-cam=x,y,z,yaw,pitch` is **silently ignored** and you get the default hero camera — the
  captures look plausible and are simply the wrong world. Always `--shot-cam <values>` with a space.
  Yaw convention: **0 = +x (east), +π/2 = +z (north)**.
* Exe is **`EchoHarbor.exe`**, not `X3Engine.exe` (a running `X3Engine.exe` is some other worktree —
  do not kill it reflexively before an Echo Harbor build).
