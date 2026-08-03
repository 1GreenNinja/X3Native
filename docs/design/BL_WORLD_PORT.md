# BL WORLD PORT SPECIFICATION

**Status: SPECIFICATION ONLY. Nothing here is built in the native engine.**
Internal doc, private repo. Written 2026-08-01 from a read-only sweep of the Babylon.js ("BL") source tree.

**This document answers `RACING_WORLD.md:653` open question #1 — "What is the BL version?"**
BL = **Bab(y)lon** — Tim's Babylon.js WebGL predecessor engine at
`C:\Users\Tim\OneDrive\GameDev\Q3Engine\`. `RACING_WORLD.md:36` listed this as
hypothesis #1 and it is correct: `x3-city-roads.js`, `x3-freeway-tunnels.js` and
`x3-world-city.js` are all real files in `Q3Engine\src\world\`, and
`app/city.h:6-7` already names them as the native city's content reference.
The freeways, the four named mountain tunnels, the districts and the mountain
ranges the native engine has been half-porting all live there.

Every claim carries a `file:line`. BL paths are relative to
`C:\Users\Tim\OneDrive\GameDev\Q3Engine\src\`. Native paths are relative to
`D:\X3Native-dev\`. `origin/echotropolis` is cited as `origin/echotropolis:path:line`
and is **not merged** into `dev-main`.

---

## 0. PROVENANCE, AND FOUR THINGS TO KNOW BEFORE READING

### 0.1 Which tree is canonical

| Tree | Files in `world/` | Bytes | mtime span | Verdict |
|---|---|---|---|---|
| **A** `Q3Engine\src\world\` | 23 | 1,552,992 | 2026-03-08 → 2026-04-02 | **CANONICAL PORT SOURCE** |
| B `DellGameDev\Bckup 3626\src\world\` | 8 | 556,767 | all 2026-03-05 | historical snapshot only |

B is a strict subset of A. `x3-freeway-tunnels.js`, `x3-city-roads.js`,
`x3-mountains.js`, `x3-world-structures.js`, `x3-facility.js`, `x3-club1127.js`,
`x3-maze-entrances.js`, `x3-cave-aesthetics.js`, `x3-world-nature.js` and 6 more
**do not exist in B at all** — they are net-new after the 2026-03-05 snapshot.
Every file common to both is strictly grown in A (e.g. `x3-world-city.js` 1523 → 2934
lines; `x3-world-terrain.js` 953 → 1804).

The one substantive B-side difference worth naming is world scale: B built a
**7,200 × 7,200** ground at 320 subdivisions; A builds **30,000 × 30,000** at 512
(`world/x3-world-terrain.js:243-245`, annotated *"Agent 57: Terrain expanded"* at
`:242`). The subsurface mass went 7,600 → 32,000 (`world/x3-world-terrain.js:955`)
and the horizon plane 5,000 → 40,000. **Use A. Ignore B except as archaeology.**

### 0.2 The extracted tree does not boot

`Q3Engine\index.html:248` loads `<script type="module" src="src/x3-main.js">`, and
**`src/x3-main.js` does not exist in A** (nor do `systems/x3-game-loop.js` or
`core/x3-constants.js`). They survive only in `Bckup 3626\src\` (2026-03-05) and in
`Q3Engine\.claude\worktrees\agent-*\src\core\x3-constants.js` (newest 2026-05-25).
Consequence: **there is no call-order authority in the port source.** Nothing in A
imports `buildFreewayTunnels`, `buildCityRoads`, `buildSurfaceStructures`,
`buildMountains` or `buildTerrain` — a repo-wide grep for those identifiers finds
only their own `export function` lines. The runnable artefacts are the monolithic
HTML builds:

- `Q3Engine\q3dm17_017-304-codex-v17.42.30.html` (1.32 MB, 2026-03-07) — world
  constants at `:1506-1530`, region defs at `:1604-1616`
- `Q3Engine\x3_engine_v17.042.31.html` (1.84 MB, 2026-03-05) — `MAZE_ENTRANCES`
  at `:1774-1780`

**Prefer these two files over `Bckup 3626` for any constant the extracted tree is
missing** — they are the actual shipped builds. The newest of them contains
**zero** occurrences of `buildFreewayTunnels`.

Two independent consequences, both load-bearing:

1. **The four named freeway tunnels were written and, as far as this tree can
   prove, never wired into a running build.** They are a high-quality design
   artefact, not shipped content. Do not treat their coordinates as playtested.
2. **There is a hard build-order dependency the port must respect**: the shared
   surface material palette `G.ps3_mats` (22 frozen PBR materials) is created
   *inside* `buildCrashSite()` at `world/x3-world-surface.js:332-390`, and
   `x3-world-city.js:48`, `x3-world-underground.js:24` and
   `x3-world-surface.js:1200` each `console.error` and bail without it. **Crash
   site before city / caves / maze**, always.

### 0.3 There are FOUR independent road systems in BL, and they do not connect

This is the single most important structural fact and it is not obvious from the
filenames.

| # | Module | What it is | Extent |
|---|---|---|---|
| **A** | `entities/x3-economy-roads.js:296-305` | **The real spine.** 6 Catmull-Rom spline roads, ribbon geometry, **automatic terrain-driven tunnel generation** with a CSG2 arch bore | X ∈ [−880, 800], Z ∈ [−320, 640] |
| **B** | `world/x3-world-structures.js:496-501` | 4 hardcoded straight box "freeways" incl. one elevated overpass | X ∈ [−300, 500], Z ∈ [100, 700] |
| **C** | `world/x3-city-roads.js:286-319` | District street grid: 4 main streets, 5 side streets, 4 connectors | X ∈ [−420, 650], Z ∈ [150, 580] |
| **D** | `world/x3-freeway-tunnels.js` | The 4 named tunnels (North/East/South/West) | mouths at (350,−1620), (2600,400), (0,−2090), (−2310,200) |

**System D connects to nothing.** The North tunnel's true mouth is at
(350, −1620) (`world/x3-freeway-tunnels.js:532-533`); the nearest road end is
system B's `hw_ns` terminating at (350, 300) (`world/x3-world-structures.js:498`)
— **1,920 units away, and exactly collinear in X = 350.** Likewise the East tunnel
starts at (2600, 400) (`:603-604`) and system A's road 0 ends at (800, 400)
(`entities/x3-economy-roads.js:298`) — **1,800 units away, exactly collinear in
Z = 400.** The intent is unmistakable and the connecting road was never authored.

**Design implication for the port: do not port D's placement. Port D's *dressing*
and D's *cross-section*, and let EchoRoads decide where tunnels go.**

### 0.4 There is no river and no bridge in BL

A repo-wide grep of `world/` for `river|bridge|viaduct` returns only arena
walkways (`world/x3-arena.js:260`, `:1646-1679`) and facility catwalks
(`world/x3-facility.js:646-657`). BL has an **ocean ring**, no watercourse, and
**no road ever crosses water**.

Tim's brief — *"freeways go in tunnels over rivers and under mountains"*
(`RACING_WORLD.md:16`) — is therefore a **composite**: the tunnels are BL, the
river is native (`app/terrain.cpp:421-448`, `app/terrain.h:128-131`). The port must
supply the bridge half from scratch; there is no BL reference for it.

---

## 1. WORLD INVENTORY

### 1.1 Global frame and layer constants

BL is a **single flat datum plane with Y-stacked layers**, not a planet, despite
the naming.

| Constant | Value | Source |
|---|---|---|
| `PLANET_Y` (the surface datum) | **−300** | `core/x3-state.js:41`; `q3dm17_…v17.42.30.html:1513` |
| `PLANET_RADIUS` | 1600 | `q3dm17_…:1514` — legacy; used only to scatter subsurface geology (`world/x3-world-terrain.js:1020-1021`) and to scale the minimap. **Not a sphere radius** |
| `PLANET_THICKNESS` | 8 | `q3dm17_…:1515` |
| `ARENA_Y` (floating arena) | **+600** | `core/x3-state.js:40`; `q3dm17_…:1507` (localStorage-overridable) |
| `ATMOSPHERE_TOP` / `FLOOR` | −200 / −320 | `core/x3-state.js:44`; `q3dm17_…:1516-1517` |
| `MAZE_Y_FLOOR` / `MAZE_Y_CEILING` | −315 / −305 | `core/x3-state.js:42-43` |
| `MAZE_WALL_H` / `CORRIDOR_W` / `WIDE_W` | 10 / 6 / 10 | read at `world/x3-world-underground.js:19-21`; **never assigned in tree A** — values from inline comments there |
| `OCEAN_SURFACE_Y` | −308 (`PLANET_Y − 8`) | `q3dm17_…:1522`; hard-coded copy at `world/x3-seafloor-access.js:27` |
| `OCEAN_FLOOR_MIN` / `MAX` | −360 / −480 | `q3dm17_…:1523-1524` |
| `OCEAN_INNER_RADIUS` (shoreline) | **1200** | `q3dm17_…:1525` |
| `OCEAN_OUTER_RADIUS` / `MESH_RADIUS` / `SHORE_BLEND` | 2400 / 2600 / 200 | `q3dm17_…:1526-1528` |
| `VOID_FLOOR_Y` | **−500 or −520 — conflict** | `core/x3-state.js:190` says −500; `q3dm17_…:1529` says −520 |
| `WORLD_RADIUS` | 15000 | `world/x3-world-terrain.js:50`, mirrored `core/x3-state.js:196` |
| `BOUNDARY_START` / `DAMAGE_START` / `WALL` | **13000 / 14000 / 14500** | `world/x3-world-terrain.js:51-53`, exported to `G` at `:56-57`; `BOUNDARY_DAMAGE_DPS = 5` at `:54` |
| `MAZE_ENTRANCES` (5) | crash (0,−30), east (800,380), west (−880,−340), north (−200,970), south (320,−890) | `x3_engine_v17.042.31.html:1774-1780`; **read but never assigned in tree A** (`world/x3-maze-entrances.js:661` warns and returns) |
| Camera near / far / FOV | 0.1 / **5000** / 1.2 rad (≈ 69°) | `systems/x3-scene-init.js:284` |

**Caveat, flagged honestly:** every `OCEAN_*`, `PLANET_RADIUS`, `MAZE_*` and
`REGION_*` value above lives in `x3-constants.js`, which is **missing from tree A**
— the world modules read them off `G` where they are declared `null`
(`core/x3-state.js:191-193`, comment *"set from x3-constants.js at init"*).
**As shipped, tree A cannot boot.** The values are recovered from the two monolith
builds (§0.2) and cross-checked against the 2026-03-05 backup; **no module in A
overrides them** (grep for `OCEAN_INNER_RADIUS =` finds only reads at
`world/x3-world-ocean.js:21`, `world/x3-ocean-upgrade.js:97`).

So the ocean ring sits at r = 1200–2600 while the terrain spans 30 km and the
mountains sit at 8–10 km — **the ocean is an inner ring around the crash site and
the mountain ranges are outside it.** Internally inconsistent in BL. Do not port
it literally; see §5.4.

**Also note the far-plane mismatch: `camera.maxZ = 5000` against a 30 km world.**
Everything past 5 km is depth-clipped and merely *implied* by the horizon backdrop
mesh. If the native port uses a 4–8 km far plane (`RACING_WORLD.md:592`) the BL
horizon trick becomes unnecessary — but BL's own composition was authored under a
5 km horizon, so its distant mountain placement was never actually seen in the
round.

### 1.2 Terrain

- **One mesh, not a streamed field.** `BABYLON.MeshBuilder.CreateGround("ps1_terrain", { width: 30000, height: 30000, subdivisions: 512, updatable: true })`
  (`world/x3-world-terrain.js:243-245`), positioned at `y = PLANET_Y`
  (`:246`). **512 subdivisions over 30 km = 58.6 m cells** — coarser than the
  native engine's 1 m LOD0 cells (`app/terrain.h:62-63`).
  **513² = 263,169 verts / ~524k triangles, one draw, no chunking, no tiling, no
  quadtree.** The only LOD is `addLODLevel(30000, null)` (`:1217`) — i.e. hide the
  whole world past 30 km.
- **Displacement is CPU, once, at build.** Vertices are read, `positions[i+1] = ps2_heightAt(x, z)` for every vertex, written back, normals recomputed
  (`world/x3-world-terrain.js:641-649`).
- **"Planet curvature" is a vertex-shader lie.** A `MaterialPluginBase`
  (`world/x3-world-terrain.js:1171-1206`) drops `vPositionW.y` by
  `clamp((d − 8000)/6000, 0, 1.5)² · 600` beyond r = 8000 (constants bound at
  `:1184-1186`, formula `:1193-1196`), attached to both the terrain and horizon
  materials (`:1203-1204`). **Visual only — collision and `ps2_heightAt` do not
  see it.** Anything placed by height query beyond 8 km will visibly float. Do not
  port; the native engine has a real horizon ring
  (`addTerrainHorizonRing`, `app/terrain.h:165-176`).
- **The noise is hand-rolled 2D simplex**, not value noise and not Babylon's
  (`world/x3-world-terrain.js:492-514`): `ps2_SEED = 42` (`:475`),
  `F2 = ½(√3−1)` (`:476`), `G2 = (3−√3)/6` (`:477`), a 512-entry permutation
  Fisher–Yates-shuffled by the module LCG (`:480-490`), 8 gradients (`:479`),
  output × 70 (`:513`). LCG state `ps2_rngState = 42` (`:468-472`).
- **The height function** `ps2_heightAt(x, z)` (`world/x3-world-terrain.js:561-636`),
  published as `window.ps2_heightAt` (`:638`) — a module closure, never on `G`.
  Returns height **relative to `PLANET_Y`**: world Y = `PLANET_Y + ps2_heightAt(x,z)`.
  This is BL's `terrainHeightAtWorld`. Composition, in order:
  1. **Rolling hills**: `noise(x·0.0004, z·0.0004)·120 + noise(x·0.0008+7.3, z·0.0008+3.1)·70` (`:565-566`)
  2. **Detail octaves**: `·0.003 → 80`, `·0.008 → 45`, `·0.025 → 12`, `·0.08 → 3` (`:569-572`)
  3. **Distant ridges**, only beyond r = 3000, ramping to full over 4000 m:
     `noise(·0.015)·60·ridgeFactor + |noise(·0.006)|·50·ridgeFactor` — the `abs()`
     is a ridged-multifractal trick giving V-shaped valleys (`:576-582`)
  4. **16 craters** (`ps2_CRATERS`, `:516-534`) — `h −= depth·(1−t²)`, rim
     `h += rim·exp(−((t−0.9)/0.1)²)` (`:590-593`). Notable: (0,0) r60 d18 rim6 =
     the crash site; (8500,−7000) r160 d55 rim20 = the largest.
  5. **10 flat zones** (`ps2_FLAT_ZONES`, `:536-543`) — `h *= smoothstep(r·0.7, r, d)`
     inside the radius (`:602-605`), i.e. flattened toward `PLANET_Y`. Named
     regions with literals in `q3dm17_…v17.42.30.html:1604-1616`:
     crash site (0,0) r80 · Outpost East (800,400) r70 · Outpost West (−880,−320) r70 ·
     (−200,1000) r60 · (320,−920) r60 · Ruins NE (1100,900) r60 · Antenna Farm
     (−400,700) r50 · **Scrapyard (−600,500) r220** · Desert Jct (−400,200) r60 ·
     Airfield (−300,700) r80
  6. **8 elevated plateaus** at 200–400 m, r 400–700, all beyond 5 km
     (`ps2_PLATEAUS`, `:545-554`, applied `:609-624`)
  7. **Boundary roll-off**: beyond r = 13000, `h -= min(t²·40, 40)` — deliberately
     *not* a cliff, it blends into a storm wall (`:629-633`, with the comment at
     `:626-628` explaining the old `t²·350` edge drop was removed)
- **Peak base-field amplitude ≈ ±330 m** before craters/plateaus. Compare the
  native canonical `heightScale = 55 m` (`app/terrain.h:64`) — **BL's rolling
  country is ~6× more vertically aggressive than the native base field.** This
  matters enormously for tunnels (§2.4).
- **Physics — and a leak worth not copying.** A `PhysicsShapeType.BOX` aggregate is
  created at `world/x3-world-terrain.js:324` **before** displacement (a no-op that is
  then superseded), then a `MESH` aggregate at `:658`, then **rebuilt three more
  times** — after the CSG cave-mouth cut (`:861`), after the vertex-depression
  fallback (`:890`), and after the tunnel carve (`:946`) — with **no disposal of the
  previous body**. Five Havok bodies on one 263k-vert mesh. Do not replicate.
- **A second terrain-modification mechanism — cave mouths.** `CAVE_CUTTERS`
  (`world/x3-world-terrain.js:832-838`) is 5 entries `{name, x, z, w, d, h, rotX}`:
  crash (0,−40) 10×28×35 rotX −0.3 · east (800,365) 10×28×35 · west (−880,−348)
  9×25×35 · north (−200,965) 10×24×35 · south (320,−988) 9×25×35.
  Applied **via `BABYLON.CSG2` subtract when available** (`:841-867`), and when it is
  not, via **a vertex depression** that pushes Y down to `min(y, −20·edgeFade)`
  inside `(w/2 + 26) × (d/2 + 26)` (`:868-893`). Each mouth also gets an invisible
  walkable ramp box at `PLANET_Y − 2` with `rotation.x = −0.3` (`:896-905`).
  **BL therefore ships both a real CSG hole and a heightfield-depression fallback
  for the same feature, and both are considered acceptable.** That is direct
  precedent for §4.3b.
- **Also built by this module**: an altitude/slope vertex-colour biome system with
  11 named zones (`getAltitudeBiomeColor`, `world/x3-world-terrain.js:690-717`,
  zone table `:664-687`), a splat PBR terrain material with altitude bands
  sand −3 / grass 0 / rock 15 / snow 30 and slope thresholds 0.7 / 0.4
  (`:281-288`), a ground-clutter thin-instance pass in **3 concentric density rings**
  (0–5 km dense, 5–10 km medium, 10–15 km sparse — `:1419-1492`),
  `buildFacilityRoad(scene, PLANET_Y)` at `:1676`, a **32,000-wide subsurface mass
  box** 780 m deep with its top at `MAZE_Y_FLOOR − 10 = −325`, CSG2-carved for cave
  mouths and maze rooms (`:952-995`), and a **separate 40,000 × 40,000 / 64-subdiv
  horizon backdrop ground** at `PLANET_Y − 0.5` with its own simplified noise
  (`:1141-1159`).

### 1.3 Mountains — `world/x3-mountains.js` (1263 lines)

Four named ranges, **all built as stacked, vertex-displaced icospheres placed on
the datum plane — not as terrain displacement.** Header at `:3-6`. Seed 63637
(`:26-30`).

| Range | Style | Peak table | Peaks | Height range | Radius range |
|---|---|---|---|---|---|
| **Northern** | jagged snow-capped | `world/x3-mountains.js:377-391` | 12 | 200–480 m | 70–140 |
| **Eastern** | volcanic basalt, lava veins, steam vents, one caldera | `:442-451` | 8 | 280–500 m | 85–160 |
| **Southern** | mesa/plateau sandstone, 3 marked `ruins: true` | `:546-563` | 15 | 120–270 m | 70–150 |
| **Western** | rolling highlands, 3 marked `crystal: true` | `:638-652` | 12 | 95–350 m | 65–160 |

**47 peaks total.** Construction per peak (`buildPeak`, `:180-268`): `layers`
icospheres (5 if `h > 350` else 4 for N/E; 4 if `h > 200` else 3 for S/W), radius
`baseRadius·(1 − t·0.75)·(0.85 + rng·0.3)`, per-vertex sinusoidal displacement at
noise scale 0.35 (jagged) / 0.2 (`:200-211`), and a per-style Y squash —
mesa `0.35 + t·0.1` with XZ widening, volcanic `0.7 + t·0.4`, rolling `0.5 + t·0.3`,
jagged `0.8 + t·0.5` (`:215-228`). Snow caps on non-volcanic/non-mesa peaks above
150 m, at `PY + h·0.92`, `scaling.y = 0.25` (`:252-265`).

**Two things that will bite a porter:**

1. **Mountains are anchored to the flat datum, not to the terrain.**
   `ico.position.set(cx, PY + layerH, cz)` — `world/x3-mountains.js:230`. They
   ignore `ps2_heightAt` entirely, so where the noise dips they float and where it
   peaks their bases are swallowed. **Port the peak *tables*; re-anchor to
   `terrainHeightAtWorld`.**
2. **There is a second, unreconciled height model.**
   `estimateAltitude(x, z)` (`world/x3-mountains.js:1115-1159`) iterates a
   **hard-coded 21-peak subset** (`:1118-1144` — the smaller connectors are missing),
   influence radius `p.r · 3`, and returns `max(p.h · t² · 0.3)`. Mountain trees
   (`scatterMountainTrees`, `:978-1059`) and boulders (`:1064-1110`) are placed at
   `PLANET_Y + estimateAltitude(x,z)` — **so mountain vegetation does not sit on
   any real surface.** Discard this function; use one height query in the port.

Collision is **not** the visual mesh: `addBaseCollider` (`:276-309`) emits two
invisible boxes per peak — a body prism `2·r × h × 2·r` centred at `PY + h/2`
(`:284-292`) and a cap `0.9·r × 4 × 0.9·r` at `PY + h − 2` (`:297-304`). A square
prism around a cone, so the player is blocked well outside the silhouette.
Visual LOD: `addLODLevel(4000, null)` on every mountain mesh (`:1209`).

Placement (BL frame — note the Z sign, §5.2):

- Northern range at **z ≈ −7,400 … −9,000**, x ∈ [−2200, 2800]. Header at `:370`
  says *"z < −7000"*. Tallest: `{ cx: 300, cz: -9000, h: 480, r: 140 }` (`:380`).
- Eastern at **x ≈ +8,500 … +10,500**, z ∈ [−2000, 2500]. Tallest:
  `{ cx: 9000, cz: -1000, h: 500, r: 160, caldera: true }` (`:443`).
- Southern at **z ≈ +8,500 … +9,600**, x ∈ [−2800, 3500]. Tallest:
  `{ cx: -1400, cz: 9500, h: 270, r: 150, ruins: true }` (`:551`).
- Western at **x ≈ −7,800 … −9,500**, z ∈ [−2000, 1800]. Tallest:
  `{ cx: -8500, cz: -500, h: 350, r: 160, crystal: true }` (`:638`).

Range-specific dressing: **Eastern** gets 2–4 lava-vein boxes per peak above 300 m
(`:461-483`), 1–3 steam vents above 280 m (`:486-497`), and a caldera on
(9000, −1000) — torus rim at `PY + h·0.88`, lava-pool disc at `PY + h·0.82`
(`:500-532`). **Southern** mesas each get a **walkable flat top** (box
`0.8–1.2·r` square × 3 tall at `PY + h − 2`, with physics — `:572-583`) plus 3–6
decorative cliff panels (`:586-604`). **Western** crystal peaks get 4–8 shard
cylinders (h 4–14, ⌀ 0.6–1.8, tess 6) at `PY + h·0.85` (`:676-699`). **Northern**
peaks at or above 400 m get snow particle systems (`:424-429`).

### 1.3.1 Mountain openings — the corrected answer

**There are no CSG holes and no bored tunnel mouths cut into any range.** But it is
not true that nothing was authored, and the port should know about all three
mechanisms:

1. **Two walkable passes, Northern range only** — elevated floor slabs *through* the
   range, with physics:
   - `nth_pass`: box 30 × 2 × 80 at **(−150, PY + 60, −8250)** — `world/x3-mountains.js:402-410`
   - `nth_pass2`: box 25 × 2 × 60 at **(1500, PY + 45, −8400)** — `:413-421`

   These are the only authored *routes* through a mountain range in BL, and they are
   floors at altitude, not tunnels.
2. **Six decorative cave-entrance facades — solid, no interior.**
   `buildCaveEntrance(name, cx, cy, cz)` (`:801-876`) builds two side boxes, a top
   arch (`archW` 6–12, `archH` 5–9, `:806-807`), **a solid dark box standing in for
   the interior at `cz − 8`** (`:840-847`), 3–6 stalactites, and one warm `PointLight`
   (intensity 0.8, range 15). The geometry is **hard-coded to face −Z** with no
   rotation parameter. Placements: three in the Southern range at (−800, 9000),
   (400, 8800), (−1400, 9500) with ±20 jitter (`:613-625`), and three standalone at
   **(−1200, PY+80, −8100)**, **(1600, PY+60, −8400)**, **(−8700, PY+70, −300)**
   (`:1192-1194`).
3. **The real tunnels are made elsewhere, from the road**, not from the mountain —
   `generateMandatoryMountainTunnels()` in `entities/x3-economy-roads.js:516-563`,
   plus the terrain carve in `world/x3-world-terrain.js:911-949`. See §2.2.

**And the four *named* tunnels penetrate nothing at all**: their mouths sit at
r ≈ 1,500–2,600 while the nearest range begins at r ≈ 7,400 — **5–8 km short.**
They are free-standing box corridors on open ground.

### 1.4 The city

Two distinct city builds, both anchored on the same coordinates the native engine
already uses:

- `world/x3-world-city.js:15` — `const CITY_CX = -600, CITY_CZ = 500;`
  → **identical to native `Scrapyard City (-600, 500)`** (`app/city.cpp:46`,
  `app/terrain.cpp:399`). Header claims 400 × 400 (`:236`); the real built extent
  from the building anchors is **X ∈ [−760, −450], Z ∈ [380, 560]** (widest anchors
  at `:1234`/`:1339` and `:1079`/`:1183`; `:1254`/`:1263` and `:1275`/`:1320`).
  Road layout is a **CSG2 boolean**: a 360 × 150 sidewalk pad at (−600, 460) has 6
  road boxes subtracted from it (`CITY_ROADS` table `:241-248`, CSG loop `:250-283`,
  flat-box fallback when CSG2 is unavailable `:279-284`).
  Textures are real PBR scans (`textures/terrain/Road007_1K/*`, `:293-301`).
- `world/x3-city-roads.js:281-282` — `const DX = 200, DZ = 500;` (New District)
  and `const SCX = -600, SCZ = 500;` (Scrapyard City) → **identical to native
  `New District (200, 500)`** (`app/city.cpp:47`).

So the native `app/city.cpp` districts are already a faithful 1:1 BL port at the
gazetteer level. Native `Industrial Zone (-200, 350)` (`app/city.cpp:48`)
corresponds to BL's `indBlvd` south-edge boulevard (`world/x3-city-roads.js:293`).

**How the buildings are made — and why not to port them.**
`x3-world-city.js` is **entirely hand-authored**: every one of the **25 named
structures** is a literal sequence of `d_box(...)` / `d_cyl(...)` calls at hardcoded
offsets from a per-building anchor. **No procedural generator, no GLB, no
instancing** (`thinInstance` appears zero times in the file). 8 original structures
at `:324-467` (Robot Shop, Arena Bar, Hack Den, Fuel Depot, Scrapyard Lot, Armor
Works, Garage, Water Tower) plus 17 in the "H1" expansion at `:1055-1350` (the file
variously claims 12 at `:1056` and 16 at `:1350` — both are wrong). Size range
**5 × 5 × 4** (Telecom Hut, `:1312`) to **12 × 12 × 30** (Helipad Tower, `:1276`,
tallest element `PY + 34.2` at `:1298`).

Street spacing worth keeping: **E–W streets 50 units apart**, **N–S streets 80
apart** (`CITY_ROADS` `:242-247`), sidewalk tiles every 8 units (`:311-314`),
**street lights every 25 units**, each a `PointLight` intensity 0.4 range 18
(`:317-322`).

**The counterexample — `world/x3-world-structures.js` is the one procedural
building generator in BL** (seed 88421, `:30-37`). Four size-randomised generators:
skyscraper (w/d 5–8, **h 15–30**, `:295-321`), apartment (w 7–12, d 6–10, h 8–15,
`:324-359`), warehouse (w 12–18, d 10–15, h 4–6, `:362-393`), shop (w 4–7, d 4–6,
h 3–4.5, `:396-421`). `buildNewDistrict(PY)` (`:426-488`) places 6 skyscrapers,
8 apartments, 4 warehouses and 10 shops at (200, 500) from hardcoded position
arrays. Window grids cap at 5 rows × 4 cols per face, 70 % lit (`:255-291`).
**If any BL building logic is worth porting, it is this file, not `x3-world-city.js`.**

City population (all per-frame animated, all cheap): 20 crowd walkers on 6 routes
(`:807-827`, builder `:649-760`), 34 specialised NPCs — 5 traders, 6 bar patrons,
4 mechanics, 4 guards, 3 scientists, 4 dock workers, 8 civilians (`:1836-1939`),
9 vehicles (3 trucks, 2 bikes, 4 drones — `:2107-2143`), and 2 orbiting helicopters
(`:2259-2260`).

### 1.5 The four named tunnels

Two layers, built by two modules, stacked end-to-end:

**Layer 1 — the entrance stubs** (`world/x3-world-structures.js:662-801`), table at
`:665-694`:

| name | entrance (x, z) | dir | length | width | height | arch W×H |
|---|---|---|---|---|---|---|
| `tun_north` | (350, −1500) | (0, −1) | 120 | 14 | 8 | 12 × 7 |
| `tun_east` | (2500, 400) | (1, 0) | 100 | 14 | 8 | 12 × 7 |
| `tun_south` | (0, −2000) | (0, −1) | 90 | 14 | 8 | 12 × 7 |
| `tun_west` | (−2200, 200) | (−1, 0) | 110 | 14 | 8 | 12 × 7 |

**Layer 2 — the full bores** (`world/x3-freeway-tunnels.js`), each starting exactly
where layer 1 ends:

| Tunnel | start | dir | length | width | height | signature |
|---|---|---|---|---|---|---|
| **North** | (350, −1620) `:532-533` | (0, −1) | **350** | **16** | 9 | dual-lane, jersey centre barrier, 3 emergency pullover bays |
| **East** | (2600, 400) `:603-604` | (1, 0) | **300** (100 + 100 curve + 100) | 14 | 8 | 30° bend in 5 sub-segments, 6 ceiling vent-fan shafts |
| **South** | (0, −2090) `:679-680` | (0, −1) | **400** (longest) | **16** | 9 | breakdown lane + hatching, double-density lighting |
| **West** | (−2310, 200) `:747-748` | (−1, 0) | 300 | 14 | 8 | **descends 20 m** over 300 (6.7 % grade) in 6 tilted segments, drainage grates, wet-floor patches |

Total named-tunnel length: **1,350 units of bore + 420 of stub = 1,770.**

### 1.6 Everything else, in brief

| Feature | Where | Evidence |
|---|---|---|
| **Facility** | **(3500, −2000)**; footprint 48 × 60, **110 tall**, world Y **−475 … −192** | `world/x3-facility.js:11` `FACILITY_POS`; root TransformNode at `PLANET_Y` (`:2443-2445`) so all floor Y values are **local**. Bounds `BLDG_L/R/F/B = −2/46/−32/28`, `BASE/TOP = −2/108` (`:12-13`). 9 levels: SUB −170, F1 Detention 0, F2 Medical 10, F3 Genetics 20, F4 Cybernetics 30, F5 Drone 65, F6 Alien Tech 78, F7 Executive 91, Roof 104 (`:23-34`). Elevator shaft 4 × 4 at (22, −29.5), **−175 → 108 = 283 units** (`:14-15`). Collision is a separate invisible `alpha=0` layer (`:264-424`). Beacon: 90-unit pole at (3470, −255, −1965), `PointLight` intensity 3.0 **range 300**, deliberately unparented (`:2284-2352`) |
| **Arena** | floating; **X ±110, Z ±70, world Y 595 … 650.5** | `world/x3-arena.js:63` adds `ARENA_Y` inside `makePlatform`. Ground floor 80 × 6 × 60 (`:176`), twin 40 × 6 × 40 platforms at x = ±85 y = 12 (`:189`, `:193`), **bridge 110 × 4 × 7 at (0, 13, 0)** (`:262`), 4 corner towers 16 × 5 × 16 at (±50, 21, ±35) (`:280-287`), crown 13 × 5 × 13 at (0, 48, 0) (`:292`). 7 `CreateRibbon` curved surfaces. Arena underground at `UY = ARENA_Y − 26 = 574`, height 20, extents X ±88 / Z ±31 (`world/x3-arena-underground.js:40-42`, `:728`) |
| **Club 1127** | underground; **dimensions contradict — see note** | `world/x3-club1127.js:33-55`. Constants: `CW 15.24 × CL 30.48 × CH 9.14` = **50 ft × 100 ft × 30 ft**. The file header at `:6` says *"43ft × 100ft × 20ft / 13.1 × 30.5 × 6.1"* — **the header is wrong; trust the constants.** `D.Y = -200` default (`:50`) but the only call site is `G.buildClub1127(0, 0, G.PLANET_Y - 200)` → **world (0, −500, 0)** (`features/x3-console.js:411`), while the LOD zone band assumes −203…−190 (`systems/x3-lod-culling.js:54-58`). **Unresolved in BL.** Cull distance 80. It is Tim's real Miami club — same provenance as native `app/club1127.cpp` |
| **Underground / maze** | Y ∈ [−315, −305] | `world/x3-world-underground.js:12` `buildMaze()`. **Hand-authored artery graph**, not procedural: 4 radial arteries hub→outposts/caves (`:459-469`, `:491-501`, `:520-531`, `:572-583`), 1 shortcut (`:628-636`), 1 NW cross-link (`:561-569`), a **40 × 40 octagonal "Nexus" hub** at origin (`:245-377`), ~14 junction rooms, and **15 dead-end rooms** (`buildDeadEnd` `:169-244`, table `:649-671`) of type pickup/lore/hazard/collapsed. **6 biome material sets** (alien/transit/industrial/mining/organic/thermal, `:130-140`). ⚠️ **Decoration uses unseeded `Math.random()`** (e.g. `:175`, `:538`, `:543`) — non-deterministic across runs; the structure itself is deterministic |
| **Command centre** | **(0, 500)**, `CC_FLOOR_Y = MAZE_Y_FLOOR − 55 = −370`, height 12 → Y −370 … −358 | `world/x3-world-underground.js:1850` `buildCommandCenter()`, constants `:1863-1873`: radius 30, corridor spine 200 × 8 × 5 running Z 430 → 230, grand entrance 12 × 6 × 40, 4 branch corridors 25 × 6 × 4. 36 terminals (`:2311`), holographic globe, tactical table (`:2343`) |
| **Maze entrances** | **5** surface↔maze portals | crash (0, −30), east (800, 380), west (−880, −340), north (−200, 970), south (320, −890) — `x3_engine_v17.042.31.html:1774-1780`. Builder `world/x3-maze-entrances.js:657-681`; each gets an arch (6 × 5 × 3), stalactites/stalagmites, drip + mist particles, glow markers, **a descent ramp** (width 6, drop 15, length 33, `atan2(15,33)` pitch, `friction 0.8` — `:340-374`), a `DynamicTexture` warning sign and rubble. Seed 55913 |
| **Caves** | `world/x3-world-surface.js:1190` `buildCaves()`; dressed by `world/x3-cave-aesthetics.js:84` | 3 cave systems: Western Outpost (−880, −320) `:1274`, Northern (−200, 1000) `:1508`, Southern (320, −920) `:1757`, plus 20 wayfinding signs `:1941`. Aesthetics module adds ring rocks / overhangs / moss / bones / dust motes / a bat flock per entrance, activation radius 150 (`x3-cave-aesthetics.js:10`), seed 9173 |
| **Ocean / seafloor** | ring r = 1200 → 2600, surface Y = −308 | `world/x3-world-ocean.js:29-46` publishes `ps5_isOverWater` (`hypot > 1200`), `ps5_waterSurfaceAt` (**flat, returns −308**), `ps5_oceanFloorAt`. Surface is a 5200 × 5200 / 32-subdiv `WaterMaterial` ground (`:82-87`, waves: windForce −10, height 0.8, length 0.3, speed 0.3) with a disc fallback (`:156-163`). **Seafloor is a separate 3200 × 3200 / 32 mesh** at `ps5c_floorHeight` (`:955-968`), which shore-ramps 1200→1600, adds 3 sines × 12 amplitude, flattens 4 zones (sub dock, sunken outpost, coral reef, kelp forest — `:930-935`), cuts a V-trench at (1300, −1200) half-width 60, and clamps to **[−490, −311]** (`:918-951`) |
| **The one water-adjacent structure** | surface pier at **(1050, −307.5, −1250)** | `world/x3-seafloor-access.js:25-34`, `:209-227` — deck 12 × 0.4 × 4 on 4 pylons, heading an elevator shaft −308 → −346 at 8 units/s. **This is the closest BL gets to a bridge, and it is a jetty over the shoreline, not a span** |
| **Space** | `world/x3-space.js` (740 lines in A vs 163 in B) | **No planet body** — `const planet = null` at `:197`, disabled at `:199`. Skybox sphere ⌀2200 BACKSIDE `infiniteDistance` (`:28-35`), 1500 stars, 2 moons, sun ⌀50, a 200-rock asteroid belt (`:343`). 12 `thinInstance` sites |
| **Nature** | trees/rocks/grass | `world/x3-world-nature.js:574` `buildNature()`, seed 31337. **Split instancing**: trees (80), rocks (60), dead trees (15), fallen logs (10) and lakes (4) are **individual meshes**; grass (300), wildflowers (100), bushes (40 clusters) and mushrooms (20) are **thin instances** (`:783`, `:842`, `:912-923`, `:1127`). Placement is **random-rejection**, not Poisson: `_validPlacement(x, z, margin, maxH=150, minH=−3)` (`:605-612`) rejects inside flat zones, past `BOUNDARY_WALL`, or outside the height band. Reads `window.ps2_heightAt` at `:577`. Per-feature cull with hysteresis: trees 300/320, rocks-logs-lakes 200/220, small detail 120/140 (`:1152-1154`), batched 1/20 per frame |

---

## 2. THE FREEWAY SYSTEM IN DETAIL

### 2.1 The one that matters: `entities/x3-economy-roads.js`

This module — **not** `x3-freeway-tunnels.js`, and **not** in `world/` — is the
racing spine, and it is a genuine road-graph system. It also carries BL's vehicle
upgrade economy (`UPGRADE_TIERS` `:23`, `applyUpgrade` `:56`, shop UI `:81-158`,
`SHOP_LOCATIONS` `:160`), which is the direct ancestor of native
`app/vehparts.{h,cpp}` and `VEHICLE_UPGRADES.md`.

**Topology — six hand-authored spline roads** (`entities/x3-economy-roads.js:296-305`):

```js
export const ROAD_DEFS = [
    { points: [[0,0],   [180,40],   [420,230],  [800,400]],   lanes: 4 },  // E arc
    { points: [[0,0],   [-220,-40], [-520,-180],[-880,-320]], lanes: 4 },  // W arc
    { points: [[0,0],   [-120,120], [-280,380], [-500,600]],  lanes: 4 },  // SW arc
    { points: [[-500,600],[-620,340],[-760,20],[-880,-320]],  lanes: 4 },  // W link
    { points: [[-500,600],[-120,640],[280,610], [800,400]],   lanes: 4 },  // S link
    { points: [[0,0],   [36,90],    [0,200]],   lanes: 2, localStreet: true },
];
```

Read the endpoints and a **closed ring** falls out: roads 0/2/3/4 form a loop
(0,0) → (800,400) → (−500,600) → (−880,−320) → back, with road 1 as the west
radial from the crash site. That is a ~4 km lap. **This is the BL race circuit,
and it is 20 numbers.** Port it verbatim (§6).

**Generation pipeline** (`buildRoad(def, idx)`, `:372-497`):

1. **Centreline**: `BABYLON.Curve3.CreateCatmullRomSpline(pts, samples, false)`
   with `samples = max(24, (pts.length-1)·18)` (`:327-328`), sampled at ~28 m
   nominal spacing (`:379`).
2. **Widths** (`:374-378`):
   `laneCount = 4` freeway / `2` local; `laneWidth = 2.6` freeway / `2.0` local;
   `shoulder = 0.9` / `0.35`; `roadHalfWidth = lanes·laneWidth·0.5 + shoulder`
   → **freeway full width 11.8 m, local street 4.7 m.**
3. **Elevation — flat.** `roadCenterlines[idx] = center.map(p => new Vector3(p.x, PY + 0.2, p.z))`
   (`:384`). **Every road sample sits at Y = −299.8, regardless of terrain.**
4. **Surface**: left/right offset paths from the tangent's perpendicular
   (`:387-398`), then one `MeshBuilder.CreateRibbon({ pathArray:[left,right], sideOrientation: DOUBLESIDE })`
   (`:400-403`). One mesh per road, PBR asphalt (`:405-408`), `freezeWorldMatrix()`.
5. **Markings**: dashed yellow centre every 4th sample, ≤5 m dashes
   (`:412-437`); white shoulder edge lines every 6th sample, 4 m
   (`:440-463`); 7-stripe crosswalks at both ends (`:466-493`). All share three
   frozen `disableLighting` emissive materials (`_dashMat`/`_edgeMat`/`_cwMat`) —
   allocated lazily and cached on the function object.

**No barriers, no piers, no lamps, no junction geometry, no collision on the road
surface itself.** The ribbon gets no `PhysicsAggregate` — cars drive on the terrain
mesh beneath it. (The same class of bug native `EchoRoads` fixed in V3,
`origin/echotropolis:app/world_hosts/echo_roads.h` INTEGRATION note 8.)

### 2.2 How roads relate to terrain — the inversion that defines BL

**Native EchoRoads: the road floats above the terrain.**
`deckFloor()` = ground + 11 m clearance, never below y = 13
(`RACING_WORLD.md:204-214` quoting `echo_roads.cpp:216-222`), then a raise-only
grade-limited relaxation. Where the ground rises, the deck rises on piers.

**BL: the road is a flat datum and the TERRAIN is lowered to meet it.**
That is the entire trick, and it is 45 lines
(`world/x3-world-terrain.js:911-949`). After the roads are built:

1. `generateMandatoryMountainTunnels()` scans every road centreline sample and
   computes **cover** = `terrainY − roadY` (`entities/x3-economy-roads.js:534-536`).
2. Any run of ≥ 6 consecutive samples (≥ 4 for local streets) with
   **cover > 11 m** (> 9 m local) becomes a tunnel run
   (`:526-527`, `:536-548`), padded by ±3 % of arc length (`:528`, `:542-543`).
3. Each run calls `buildRoadTunnel(roadIdx, tStart, tEnd, { width: 72, height: 34, wall: 2.5, lightStep: 7 })`
   for freeway, `{ width: 42, height: 22, ... }` for local (`:553-558`).
4. The tunnel's world path is recorded in `roadTunnelSpecs` (`:657-662`).
5. **Then the terrain is deformed.** For every one of the ~263k ground vertices,
   for every tunnel spec, if the vertex is within `carveRadius = spec.width · 0.95`
   of any path point, the vertex Y is pulled down toward
   `ceilingWorldY = p.y + spec.height + 4.0`, smoothstep-blended by radial distance,
   and `min`-combined across overlapping tunnels
   (`world/x3-world-terrain.js:916-937`). Normals recomputed, bounding info
   refreshed, and only *then* is the Havok mesh body created (`:940-946`).

So a BL tunnel is not a hole in a mountain. **It is a swept arch tube, plus a
smoothstep depression stamped into the heightfield so the hill sits down onto the
tube's roof.** From the road you drive into an arch with a hill over it; from
above you see a low saddle. It costs one extra vertex pass at build and **zero
per-frame work**, and it needs no CSG on the terrain, no voxels, no keep-out
rects, and no per-tile holes.

**This is the highest-value idea in the entire BL tree and it maps directly onto
`app/terrain.h`. See §4.3 and §6.**

### 2.3 Tunnel geometry — how the bore is actually made

`buildRoadTunnel` (`entities/x3-economy-roads.js:604-687`):

- **Cross-section**: `makeArchShape(width, height, floorY, steps)` (`:588-601`) —
  a 28-step (default) semi-ellipse: `y = floorY + height·sqrt(1 − nx²)` for
  `nx = x / (width/2)`, capped with two floor corners. A polyline in the XY plane.
- **Sweep**: `MeshBuilder.ExtrudeShape({ shape, path: tunnelPath, cap: CAP_ALL, closeShape: true })`
  (`:626-638`) — **the sweep follows the sliced road centreline**, so the tube
  inherits the road's curvature exactly. Two are made: `outer` at
  (width, height, floorY = 0) and `inner` at (width − 2·wall, height − wall,
  floorY = wall) (`:623-624`), wall default 2.4–2.5 m.
- **Hollowing**: `BABYLON.CSG2.FromMesh(outer).subtract(CSG2.FromMesh(inner))`
  (`:641-650`), guarded by `G.csg2Available` (set in
  `systems/x3-scene-init.js:230-255`). **On CSG failure it logs and ships the
  solid outer shell** (`:647-649`) — a deliberate, honest degradation.
  `backFaceCulling = false` on the tunnel material (`:621`) so the un-carved
  fallback is still enterable.
- **Collision**: `new PhysicsAggregate(shell, PhysicsShapeType.MESH, { mass: 0 })`
  (`:655`) — a real Havok triangle mesh over the carved shell.
- **Lighting**: emissive lamp boxes 0.35 × 0.18 × 2.4 every `lightStep` samples
  (7 freeway / 10 local), at `PLANET_Y + 0.2 + height − 1.6`, oriented to the
  tangent, added to the glow layer (`:665-686`). The comment at `:664` states the
  reason explicitly: *"Emissive strips instead of point lights to avoid
  cross-geometry light bleed."* **Zero dynamic lights in an auto-generated
  tunnel.** Compare `RACING_WORLD.md:433`, which independently proposes exactly
  this rationing strategy for the native engine.
- **Slicing**: `slicePathByT(path, tStart, tEnd)` (`:337-370`) is a proper
  arc-length walk with endpoint interpolation, not an index slice.

Dimensions worth noting: the auto-tunnels are **72 m wide × 34 m tall** — far
larger than the named tunnels' 14–16 × 8–9. They are cavernous underpasses sized
to swallow a hill, not road tunnels. The named tunnels are the road-scale ones.

### 2.4 Freeway system B — the straight boxes

`world/x3-world-structures.js:493-657`. Four hardcoded segments (`:496-501`):

| name | from | to | elevated |
|---|---|---|---|
| `hw_ew` | (−300, 500) | (500, 500) | no |
| `hw_ns` | (350, 300) | (350, 700) | no |
| `hw_overpass` | (320, 470) | (380, 530) | **yes, +8 m** |
| `hw_south` | (50, 100) | (150, 450) | no |

`ROAD_WIDTH = 12`, `LANE_COUNT = 4`, `OVERPASS_HEIGHT = 8` (`:503-505`).
Each is one `CreateBox(width: 12, height: 0.3, depth: len)` yaw-rotated by
`atan2(dx, dz)` (`:518-522`) at `PY + 0.2` (or `PY + 8` if elevated, `:515`), with a
Havok box aggregate, friction 0.8 (`:526`). Lane markings: double dashed yellow at
±0.2 m every other 12 m segment (`:548-557`), white lane lines every 4th segment at
`(lane − 2)·3 m` (`:560-569`). Guardrails: 0.15 × 0.8 × len boxes at
±(6 + 0.075), **with collision** (`:575-581`). The overpass adds 1.2 × 8 × 1.2
concrete pillars every 20 m in pairs at ±3 m (`:584-597`) and 20 m ramps at both
ends, pitched by `atan2(8, 20)` = 21.8 ° (`:599-634`). Street lamps every 50 m,
6 m poles + 2 m arms + a 0.2-intensity range-10 point light (`:637-653`).

Note `hw_overpass` is a **60 m diagonal** crossing `hw_ew` at (350, 500) — the
only interchange in BL, and it is four boxes.

### 2.5 System C — the district street grid

`world/x3-city-roads.js`. Grid tables at `:286-319`: 4 E–W main streets (width 14)
at Z = 560/500/440/410, 5 N–S side streets (width 8) at X = 120/170/220/270/310,
4 connectors (widths 8–10) reaching to Scrapyard City, the crash site, the coast
and the facility. Intersections are **computed**, not hand-placed: the cross product
of `mainZs` × `sideXs` (`:322-339`), 20 total, with `major` flagged on the top-8 for
traffic lights (`:334`).

Per-segment build (`buildRoadSegment`, `:344-387`): one box at `PY + 0.15` with
physics (`:356`), centre dashes 5 m on / 6 m off (`:368-381`), sidewalks + curbs on
non-connector roads (`:384-386`). Plus traffic lights (`:510`), street signs
(`:558`), bus stops (`:600`), fire hydrants (`:646`), utility poles (`:664`),
parking lots (`:737`), median strips (`:780`), speed bumps (`:823`).

Traffic lights are **stateful and cycled per frame** — `_trafficLights` holds
`{ red, yellow, green, phase, lastState }` (`:16`) and the three emissive materials
are deliberately excluded from the material freeze (`:216` — *"NOT frozen — swapped
at runtime"*).

### 2.6 Zone culling and per-frame work

BL has **no portals and no PVS.** Visibility is three cheap independent layers:

**(a) The global system** — `systems/x3-lod-culling.js:1-40`. Header is candid:
*"Previous system caused 2 FPS due to per-mesh iteration"*, *"NO octree (broken in
this project)"*. What survives: zone visibility via **name-prefix sorted arrays**
rechecked every 6 frames, Babylon-native `mesh.addLODLevel(dist, null)` for
auto-cull, light culling every 15 frames at 200 m with 180 m show-hysteresis
(`:36-38`), particle culling every 20 frames. Zones are keyed on Y bands
(`ZONE_Y_SPACE`, `ZONE_Y_UNDERGROUND`, imported `:26-27`).

**(b) Per-module distance culling — every module rolls its own, all `setEnabled`-based:**

| Module | Cull / show (hysteresis) | Throttle | Cite |
|---|---|---|---|
| City roads | 400 structure / 200 detail | — | `world/x3-city-roads.js:14-15`, `:98` |
| City | 350 / 300 outer; 180 / 150 detail | every 6 frames | `world/x3-world-city.js:16-23`, `:2584`, `:2622-2632` |
| Surface structures | 600 | 1/10 per frame; Y-band early-out at ±50 | `world/x3-world-structures.js:924-934` |
| Facility | 400 whole; 150 per floor (3-D incl. Y) | every 500 ms | `world/x3-facility.js:46-53`, `:2522`, `:2544-2552` |
| Freeway tunnels | 500 mesh / zone radius + 100 | 1/8 per frame; Y-band early-out at ±60 | `world/x3-freeway-tunnels.js:1051`, `:1069`, `:1083-1118` |
| Maze entrances | 200 | 1/8 per frame; Y-band ±60 | `world/x3-maze-entrances.js:701-723` |
| Nature | 300/320 trees, 200/220 rocks, 120/140 detail | 1/20 per frame | `world/x3-world-nature.js:1152-1154`, `:1175` |
| Cave aesthetics | 150 activation | — | `world/x3-cave-aesthetics.js:10-11` |
| Arena underground | 120 | every frame, early-out on no change | `world/x3-arena-underground.js:732`, `:745-770` |
| Club 1127 | 80 | every frame | `world/x3-club1127.js:53-54`, `:883-894` |

Two implementation details the port should inherit as *lessons*, not code:
**merged meshes sit at the origin, so distance tests must use
`boundingBox.centerWorld`, not `mesh.position`** (documented and implemented at
`world/x3-world-structures.js:844-847`, `world/x3-maze-entrances.js:729-739`,
`world/x3-freeway-tunnels.js:1103-1112`); and **materials must be frozen only
*after* merging**, an explicit rule stated at `world/x3-facility.js:2404`,
`world/x3-world-structures.js:872` and `world/x3-cave-aesthetics.js:29` — with
flickering neon materials deliberately excluded so `emissiveColor` stays mutable
(`world/x3-world-city.js:2523-2530`, `world/x3-city-roads.js:216`).

**(c) The tunnel module's own culler** — `updateFreewayTunnels(dt)`
(`world/x3-freeway-tunnels.js:1038-1149`). Per frame:
1. **Y-band early-out**: `if (Math.abs(py - PLANET_Y) > 60) return;` (`:1051`) —
   no work at all unless the player is on the surface layer.
2. Compute `nearTunnel` against `_tunnelZones` (`{cx, cz, radius}` registered per
   segment at `radius = length/2 + 50`, `:522`, `:858`) with a +100 m slop (`:1061`).
3. **Zone activation** at `CULL_RADIUS = 100` (`:1069-1080`).
4. **Amortised mesh toggling — 1/8 of the mesh list per frame**, batch index
   `floor(_updateTime · 6) % 8` (`:1083-1086`). If no zone is active, everything in
   the batch is `setEnabled(false)` (`:1090-1094`); otherwise each mesh is enabled
   iff its centre is within **500 m** (`:1114-1118`). Merged meshes use
   `_boundingInfo.boundingBox.centerWorld` because their `position` is (0,0,0)
   (`:1103-1112`).
5. **Fan rotation** — only when `nearTunnel`: `unfreezeWorldMatrix()`,
   `rotation.y += speed·dt`, `freezeWorldMatrix()` per fan (`:1123-1131`).
6. **Light flicker** — once every ~2 s, one random light drops to 30 % intensity
   and is restored by a 100 ms `setTimeout` (`:1134-1148`).

**Build-time optimisation** (`buildFreewayTunnels`, `:889-912`):
- `mergeTunnelMeshes()` (`:917-1013`) — group by **material name**, skip meshes with
  `physicsBody`, skip rotating fans, skip glow-layer meshes (`:923-942`), require
  ≥ 4 meshes per group (`:960`), merge in batches of ≤ 64 (`:963-982`) via
  `Mesh.MergeMeshes(batch, true, true, undefined, false, true)`, restoring
  originals on exception (`:993-998`).
- `freezeTunnelMaterials()` (`:1018-1033`) — `.freeze()` on all 19 materials.

---

## 3. TUNNEL DRESSING — the detail layer worth preserving

Everything in this section is from `world/x3-freeway-tunnels.js`. This is the
"not slop" layer: **19 materials, 11 prop archetypes, 4 distinct tunnel
personalities, all procedurally placed at fixed metre intervals.** It is worth
porting almost verbatim because it is *specification*, not code — the intervals
and dimensions are the design.

### 3.1 Materials (`createMaterials`, `:50-194`)

| Material | Type | Key values | Line |
|---|---|---|---|
| `matAsphalt` | PBRMetallicRoughness | baseColor (0.07, 0.07, 0.08), rough 0.92, metal 0 | `:54-56` |
| `matConcreteWall` | PBR + **procedural DynamicTexture** | 256², fill `#5e5a56`, horizontal lines every 32 px + vertical every 64 px in `#4a4744`, lineWidth 2 → a running-bond tile pattern; baseColor forced to white once the texture lands | `:59-80` |
| `matCeiling` | PBR | (0.12, 0.12, 0.11), rough 0.9 | `:83-85` |
| `matLaneWhite` / `matLaneYellow` | Standard, `disableLighting = true` | emissive (0.85,0.85,0.8) / (0.85,0.7,0.1) | `:88-94` |
| `matBarrier` | PBR | (0.50, 0.48, 0.45), rough 0.8 | `:97-99` |
| `matLightBar` / `matLightBarOff` | Standard emissive | (0.95, 0.92, 0.75) / (0.15, 0.14, 0.12) | `:102-108` |
| `matFanBlade` / `matFanHub` | PBR metal | rough 0.5/0.4, metal 0.6/0.7 | `:111-117` |
| `matFireExtBox` | PBR | (0.8, 0.1, 0.05) red, metal 0.3 | `:120-122` |
| `matSignPost` | PBR | (0.5, 0.5, 0.52), metal 0.5 | `:125-127` |
| `matSignFace` | Standard + **DynamicTexture** | 128²: `#e8e8e8` field, red 6 px circle r 55, bold 36 px "60", 16 px "KPH" | `:130-149` |
| `matExitSign` | Standard emissive + **DynamicTexture** | (0.1, 0.8, 0.2); 128×64, `#0a6618` field, white bold 32 px "EXIT" | `:152-168` |
| `matTelephoneBox` | PBR | (0.1, 0.2, 0.6) blue | `:171-173` |
| `matDrainGrate` | PBR | (0.25, 0.25, 0.27), metal 0.5 | `:176-178` |
| `matWetFloor` | PBR | (0.05,0.05,0.06), **rough 0.2** — the only glossy surface in the set | `:181-183` |
| `matJetFanBody` | PBR | (0.4, 0.4, 0.42), metal 0.6 | `:186-188` |
| `matVentShaft` | PBR | (0.3, 0.3, 0.32), metal 0.4 | `:191-193` |

### 3.2 The shell (`buildTunnelShell`, `:390-429`)

Five boxes per segment, all yaw-rotated to the tunnel angle, all
`freezeWorldMatrix()`, all except the ceiling given Havok box collision:

| Part | Dimensions | Position |
|---|---|---|
| Floor | `width × 0.3 × length` | `PY + 0.15` — **physics** `:399` |
| Left wall | `0.8 × height × length` | `+perp · width/2` — **physics** `:404-405` |
| Right wall | `0.8 × height × length` | `−perp · width/2` — **physics** `:410-411` |
| Ceiling | `(width + 1.6) × 0.5 × length` | `PY + height + 0.25` — **physics** `:416-417` |

Plus an arch at each end (`buildArch`, `:431-448`): two 1.5 × (height+1) × 1.5
pillars at `±(archW/2 + 0.75)` where `archW = width − 2` (`:432-440`), and a
`(archW + 3) × 1.2 × 1.5` top beam rotated `angle + π/2` (`:442-447`).

**Note the shell is a rectangular box tunnel, not the semi-elliptical arch of
§2.3.** BL has two independent tunnel cross-sections. The arch is better; the box
is what the named tunnels use.

### 3.3 Common furniture (`addCommonFurniture`, `:454-523`)

Every named tunnel gets all of this, at these exact intervals:

| Prop | Interval | Placement | Geometry | Line |
|---|---|---|---|---|
| **Fluorescent light strip** | **every 15 m** | ceiling, `PY + height − 0.15`, `width·0.5` long | box `(w·0.6) × 0.08 × 0.3`, `matLightBar`, added to the **glow layer**; **a `PointLight` on every 3rd bar** — warm (1.0, 0.95, 0.75), intensity 0.5, range 18, offset 0.5 m below the bar, registered in `_flickerLights` | `:249-264`, called `:459-468` |
| **EXIT sign** | **every 100 m** | right wall, `PY + height − 1.0`, inset 0.1 m | box 1.2 × 0.5 × 0.06, emissive green, glow layer | `:267-272`, `:470-479` |
| **Fire extinguisher box** | **every 80 m** | left wall, `PY + 1.5`, inset 0.15 m | box 0.4 × 0.6 × 0.2, red PBR | `:275-279`, `:481-490` |
| **Jet fan** | **every 60 m** | ceiling, `PY + height − 0.6` | 1.8 m × 1.4 ⌀ cylinder (10-tess) rotated 90° about Z to lie horizontal, plus a 1.2 ⌀ 8-tess disc rotated 90° about X; the disc joins `_fanMeshes` with speed `0.8 + rng()·0.4` rad/s | `:293-304`, `:492-501` |
| **Emergency telephone alcove** | **every 120 m** | right wall, `PY + 1.5`, inset 0.2 m | a 1.2 × 1.8 × 0.5 recess box in ceiling material offset 0.25 m into the wall, plus a 0.3 × 0.4 × 0.15 blue phone box offset 0.1 m | `:307-318`, `:503-513` |
| **Speed-limit sign (60 KPH)** | **once, at the mouth** | offset `width/2 + 2` outside the right wall | 3.5 m × 0.1 ⌀ post + 1.0 × 1.0 × 0.05 face at `PY + 1.5 + 1.5` | `:321-328`, `:515-517` |
| **Zone registration** | per segment | — | `_tunnelZones.push({ cx, cz, radius: length/2 + 50 })` | `:519-522` |

### 3.4 Lane markings (`addLaneMarkings`, `:331-385`)

Segment length 5 m; marking plane at `PY + 0.32`.
- **Dashed yellow centre, every other segment** (`:345`). Dual-lane tunnels get a
  *pair* at ±0.4 m to straddle the barrier (`:346-355`); single-bore gets one
  (`:356-360`). Dash geometry `0.12 × 0.02 × 3.0` (= `segLen·0.6`).
- **Solid white edge lines every segment**, at `±(width/2 − 0.5)`,
  `0.1 × 0.02 × 4.5` (`:364-371`).
- **White dashed lane dividers every 3rd segment**, dual-lane only, at
  `±width/4`, skipped if `|offset| < 1` (`:373-383`).

### 3.5 The four personalities

**North — dual-lane with centre barrier and pullover bays** (`:528-594`)
- Jersey barrier: one box `0.6 × 1.0 × (length − 20)` at `PY + 0.8`, centred, **with
  physics** — the 20 m shortfall leaves a gap at each end (`:547-553`).
- **3 pullover bays** at 100 m spacing (`:555-585`), each: a 4 × 0.3 × 12 asphalt
  floor at `−perp·(width/2 + 2)` with physics; a 0.6 × height × 12 concrete wall at
  `−perp·(width/2 + 4)` with physics; a 4.5 × 0.5 × 12 ceiling; and an amber
  `PointLight` (1.0, 0.8, 0.4) intensity 0.3, range 10, at `PY + height − 1`.
- `addLaneMarkings(..., isDualLane = true)` (`:588`).

**East — single bore with a 30° curve and vent shafts** (`:599-670`)
- Three parts: 100 m straight → 100 m curve → 100 m straight (`:612-655`).
- The curve is **5 chorded sub-segments** of 20 m each, heading advanced by
  `30°/5 = 6°` per segment and evaluated at the segment midpoint
  (`bendSegs = 5`, `angleStep`, `segAngle = curAngleRad + angleStep·(bs + 0.5)`,
  `:621-640`). Effective radius ≈ 100 / (30° in rad) ≈ **191 m**.
- **A known bug worth not porting**: the curve sub-segments get
  `buildTunnelShell` only — **no lane markings and no furniture** (`:636`). Only
  segments 1 and 3 are dressed (`:618-619`, `:654-655`). The curve is bare.
- **6 ceiling vent fans** at 50 m spacing along a straight-line lerp from the start
  to the final exit — which does **not** follow the curve, so mid-tunnel fans are
  laterally offset from the actual bore (`:657-667`). A second bug; fix on port.
- `addVentFan` (`:282-290`): a 0.4 m × 2.0 ⌀ 12-tess housing at `y + 0.2`, plus a
  1.6 ⌀ disc rotated 90° about X, joining `_fanMeshes` at `1.5 + rng()·0.5` rad/s.

**South — long straight, well-lit, breakdown lane** (`:675-738`)
- 400 m, the longest; width 16 for the extra lane; `breakdownWidth = 3` (`:696`).
- **Diagonal hatching every 8 m** in the breakdown lane: `0.1 × 0.02 × 2.5` boxes
  rotated `angle + π/6` (`:697-707`).
- **Separation line every 5 m**: `0.12 × 0.02 × 4` at `−perp·(width/2 − 3)`
  (`:710-719`).
- **Double lighting**: a second row of strips at 8 m spacing, offset `+perp·3`,
  `width·0.3` long — on top of the standard 15 m row (`:721-731`).

**West — steep descent with drainage** (`:743-861`)
- Descends `descentTotal = 20` over `length = 300` → **6.67 % grade** (`:750-753`).
- Built as **6 discrete 50 m segments**, each 3.33 m lower than the last
  (`:761-805`). Each segment's floor is tilted by
  `tiltAngle = atan2(descentPerSeg, segLen)` = 3.81°, applied to `rotation.z` when
  the tunnel runs along X and `rotation.x` when it runs along Z (`:783-785`).
  **The walls and ceiling are NOT tilted** (`:789-804`) — they step. A visible
  seam every 50 m; fix on port with a true swept frame.
- Arches: entrance at `PY`, exit at `PY − descentTotal` (`:808-811`).
- **Drainage grates**: 3 per segment (18 total), both walls, `0.6 × 0.05 × 3`, at
  `±perp·(width/2 − 1)`, Y interpolated along the segment slope (`:814-833`).
- **8 wet-floor patches** at seeded-random `t`, size `(2 + rng·2) × 0.02 × (3 + rng·2)`,
  lateral offset `(rng − 0.5)·(width − 4)`, yaw jittered ±0.15 rad
  (`:836-847`). `matWetFloor` at roughness 0.2 is the reflective payoff.

### 3.6 Audio

`setupReverbZones()` (`:866-884`) pushes every tunnel zone onto
`G._tunnelReverbZones` as `{ cx, cz, radius, reverbTime: 2.5, wetDry: 0.35 }`.
Nothing in the extracted tree consumes it — it is a declared contract with an
audio system that is not in `world/`. **Port the numbers (2.5 s RT60, 35 % wet);
the native `IAudioSystem` has an RT-acoustics path (`engine/audio/IAudioSystem.h:60-155`).**

### 3.7 Determinism

Every module uses the same LCG: `_seed = (_seed·1664525 + 1013904223) & 0x7fffffff`
(`x3-freeway-tunnels.js:36-39` seed **77231**; `x3-city-roads.js:25-28` seed
**55917**). Deterministic, but **order-dependent** — the wet patches, fan speeds
and flicker selection all draw from one shared stream. Native equivalent should use
a position-hashed generator (the `h01()` discipline in
`origin/echotropolis:app/world_hosts/echo_roads.cpp:184-187`) so results are
independent of build order.

---

## 4. WHAT PORTS DIRECTLY vs WHAT MUST BE REBUILT

### 4.1 API-level mapping

| BL construct | Where | Native equivalent | Verdict |
|---|---|---|---|
| `MeshBuilder.CreateBox/Cylinder/Disc/Sphere` | throughout | `app/mesh_prims.h` + `Scene` entities | **Direct.** 1:1 |
| `MeshBuilder.CreateRibbon(pathArray:[left,right])` | `economy-roads.js:400` | EchoRoads' welded ribbon emitter (`echo_roads.cpp`, "weld: shared verts, smooth bank normals, continuous UVs") | **Superseded** — native is strictly better (welded, banked, UV'd) |
| `MeshBuilder.ExtrudeShape(shape, path, CAP_ALL)` | `economy-roads.js:626-638` | **Nothing.** Closest is `descent_slide.cpp:381-441` `Bore` — oriented boxes in a banked basis, not a true swept profile | **REBUILD.** This is the one genuinely missing primitive; see §4.3 |
| `Curve3.CreateCatmullRomSpline` | `economy-roads.js:328` | EchoRoads resamples to arc-length-even samples (4 m / 2 m ramps); `descent_slide.cpp:187-214` builds frames by central-difference tangent + Rodrigues bank | **Direct**, use either |
| `BABYLON.CSG2.subtract` | `economy-roads.js:641-650`; `world-city.js:263-273` | **Nothing.** `app/club_bedrock.h:107-113` names CSG as an unimplemented upgrade path | **REBUILD or avoid.** Emit the annulus directly (§4.3) |
| `PBRMetallicRoughnessMaterial` | ~19 in the tunnel module alone | `IRenderDevice::drawMeshPBR` + `SurfaceLibrary` (`app/surface_library.h`) | **Direct.** Colour/rough/metal map 1:1 |
| `StandardMaterial` + `disableLighting` + `emissiveColor` | all lane paint, EXIT signs, light bars | native has no unlit path; use a flat PBR colour + the `NightGlow` bucket pattern (`echo_roads.h` `kBucketNightGlow`) | **Adapt.** Do NOT bake emissive into the shared material — `echo_roads.h` calls that "Tim's *neon never sleeps* bug class" |
| `DynamicTexture` (canvas-drawn concrete tiles, "60 KPH", "EXIT") | `:63-80`, `:133-149`, `:156-167` | none — native loads PNGs via stb_image (`echo_roads.h` V7 note 7) | **Rebuild as assets.** 3 small PNGs: `concrete_tile`, `sign_60kph`, `sign_exit` |
| `Mesh.MergeMeshes` by material | `:917-1013`, `city-roads.js:116-162` | native already builds **one mesh per material bucket** by construction (`echo_roads.h` `Bucket`) | **Obsolete.** Native has no merge step because it never fragments |
| `freezeWorldMatrix()` / `material.freeze()` | everywhere | N/A — native draws with an identity model matrix per bucket | **Obsolete** |
| thin instances (`thinInstanceSetBuffer` / `thinInstanceAdd`) | `world-nature.js` (grass/flowers/bushes/mushrooms), `mountains.js:1053-1054`, `:1104-1105`, `world-terrain.js:1598-1599`, `seafloor-base.js`, `space.js`, and exactly **2 sites** in the whole facility (`facility.js:1187-1188`, 32 detention cots). **Zero `createInstance()` / `InstancedMesh` anywhere in `world/`** | `IRenderDevice` instanced draw path | **Direct** — but note BL barely uses it. The city, facility, arena, club and all four named tunnels are 100 % individual meshes |
| `SceneLoader.ImportMeshAsync` (GLB) | only `world/x3-warehouse-props.js:30` (23 props, `:98-119`) — and it is **not called from `x3-facility.js`** despite its header claim (`warehouse-props.js:3`) | native GLB loader | **Direct**, and barely used — BL's world is 99 % procedural primitives |
| Custom shaders | **none in the world modules** except the aurora `ShaderMaterial` (`world/x3-space.js:562`). No `NodeMaterial`, no `Effect.ShadersStore`, no `ProceduralTexture` | native shader pipeline | **Nothing to port** |
| `PhysicsAggregate(mesh, BOX/CYLINDER, {mass:0, friction:0.7})` | every structural mesh | Jolt `addStaticBox` / mesh shape | **Direct.** Note BL friction 0.7 (props) / 0.8 (freeway roads, `world-structures.js:526`) |
| `PhysicsAggregate(shell, MESH, {mass:0})` | tunnel shells `:655`, terrain `:946` | `phys->addStaticMesh(...)` — the same call `echo_roads.h` INTEGRATION note 8 specifies | **Direct** |
| `PointLight { diffuse, intensity, range }` | tunnel strips `:257-261`, bay lights `:580-584`, lamps | `x3::rhi::PointLight` + host nearest-K merge under the 64-light cap (`host_echotropolis.cpp:4164-4176`) | **Direct, but ration.** See §4.4 |
| `glowLayer.addIncludedOnlyMesh` | light bars, EXIT signs, tunnel lamps | bloom is in the post stack; use the `drawNightGlow` bucket pattern | **Adapt** |
| Name-prefix zone arrays + Y-band culling | `systems/x3-lod-culling.js:1-40` | HZB + GPU cull (`r_vis 3`), Scene PVS for interiors | **Obsolete.** Native is strictly better |
| `window.ps2_heightAt(x, z)` | `world-terrain.js:638` | `terrainHeightAtWorld(x, z)` (`app/terrain.h:97`) | **Direct 1:1 adapter** |

### 4.2 What ports as *data*, not code

These are the artefacts worth transcribing literally into native tables:

1. **`ROAD_DEFS`** — 6 splines, 20 control points (`economy-roads.js:296-305`). A closed ~4 km ring. **This is the race circuit.**
2. **The 4 tunnel definitions** — names, headings, lengths, widths, heights, and the four personality descriptors (`world-structures.js:665-694` + `freeway-tunnels.js:528-861`).
3. **The tunnel dressing interval table** — §3.3. 15 m lights, 100 m EXIT, 80 m extinguisher, 60 m jet fan, 120 m phone. These are the design.
4. **The 4 mountain range peak tables** — **47** peaks with (cx, cz, h, r) (`x3-mountains.js:377-391`, `:442-451`, `:546-563`, `:638-652`).
5. **`ps2_CRATERS` / `ps2_FLAT_ZONES` / `ps2_PLATEAUS`** (`world-terrain.js:516-554`) — **16 + 10 + 8** entries.
6. **District centres** — already ported (`app/city.cpp:46-48`).
7. **Reverb: 2.5 s / 35 % wet** (`freeway-tunnels.js:878-879`).
8. **Club 1127's real dimensions**: **15.24 × 30.48 × 9.14 m** (50 × 100 × 30 ft) — `club1127.js:35-37`, *not* the header's 13.1 × 30.5 × 6.1.
9. **The 5 maze-entrance coordinates** (`x3_engine_v17.042.31.html:1774-1780`) and the 5 `CAVE_CUTTERS` mouths (`world-terrain.js:832-838`).
10. **The 2 Northern-range walkable passes** (`x3-mountains.js:402-421`) — the only authored routes through a mountain in BL.
11. **The facility floor table** — 9 levels at local Y −170 … 104 (`x3-facility.js:23-34`).

### 4.3 The three things that must be genuinely built

**(a) A swept-tube emitter.**
BL gets its arch bore from `ExtrudeShape` + a CSG boolean. Native has neither.
But it does not need either: the correct native construction is to **emit the
annulus directly** — walk the arc-length frames, evaluate the outer arch polyline
and the inner arch polyline at each frame, and stitch quads between consecutive
frames for the inner surface, the outer surface, and the two end caps. That is a
plain ribbon sweep with a closed profile — **no boolean, no CSG library, and it
produces exactly the mesh CSG would have produced**, with correct UVs and welded
normals as a bonus.

The frame supply already exists twice over:
- `descent_slide.cpp:187-214` — central-difference tangent + Rodrigues rotation of
  the lateral basis by `bankDeg`, `kSampleStep = 2.5 m` (`:52`). `TrackFrame`
  carries `pos/tan/right/up/bankDeg/cumLen` (`app/descent_slide.h:48-56`) — exactly
  the frame a sweep needs, and `TrackSegType::Bore` (`:41`) already means "tight
  enclosed tube with a roof".
- `RoadSample` (`echo_roads.h:126-132`) — `x/y/z`, unit XZ tangent, `bank`. Arc-length
  even at 4 m. Add an up-vector from `bank` and it is the same frame.

**Recommendation: write `sweepProfile(frames, profile2D) -> MeshVertex/index arrays`
as a shared utility, and have both a new tunnel bucket and `descent_slide`'s `Bore`
call it.** `makeArchShape` (`economy-roads.js:588-601`) is 13 lines and ports as-is.

**(b) A terrain corridor clamp — the BL trick, natively.**
BL's tunnel is a tube + a smoothstep depression stamped into the ground vertices
(`world-terrain.js:911-949`, §2.2).

**And BL itself already treats the depression as an acceptable substitute for a
real hole.** Its cave mouths do the *same feature both ways*: `CAVE_CUTTERS` cuts a
genuine `BABYLON.CSG2` hole when CSG2 is available (`world-terrain.js:841-867`) and
falls back to **a vertex depression to `min(y, −20·edgeFade)`** when it is not
(`:868-893`), with an invisible walkable ramp under the mouth either way
(`:896-905`). The tunnel path went straight to the depression and never had a CSG
variant at all. That is the precedent: **in this world's own judgement, a
depression reads well enough that it shipped as the fallback for a feature that
had a boolean available.**

The native heightfield is a *pure function*
composed of exactly this kind of layer already: `app/terrain.cpp:739-778` composites
macro relief → 4 mountain ranges → ocean basin → **4 flat pads (blended to `r × 1.7`
by smoothstep, `:762-772`)** → authored landforms including the river carve
(`:664-701`).

**A tunnel corridor is structurally identical to a flat pad, except the blend
target is a per-sample ceiling height along a polyline instead of a constant
height inside a circle.** Adding it is the same shape of change as adding the
river:

```
// proposed, app/terrain.cpp, in the authored-landform stage:
//   for each registered tunnel corridor C:
//     d = distance from (x,z) to C's polyline
//     if (d < C.carveRadius):
//       t = smoothstep(1 - d / C.carveRadius)
//       ceilY = interp(C, x, z).y + C.height + kTunnelSoilCover
//       h = min(h, lerp(h, ceilY, t))
```

This is **strictly better than `RACING_WORLD.md:427-431`'s proposal** (oriented
keep-out rects + tiles never generated + the tunnel supplying its own ground). No
holes, no voids, no 32 m granularity problem, no "tunnel must supply its own
ground and collision" invariant to violate. The terrain stays single-valued
(`app/terrain.cpp:735`), stays pure, stays thread-safe, and stays streamable —
because a corridor clamp is just another term in `h(x, z)`.

**The cost is honest and should be stated: the hill above a tunnel gets a visible
saddle.** In BL, `carveRadius = width · 0.95` and the tube is 72 m wide, so the
saddle is ~140 m across and ~34 m deep. At native road width (14 m deck, so maybe
a 24 m bore) the saddle is ~46 m across — a shallow notch, cosmetically fine and
arguably reads as a real cut-and-cover. For a *deep* bore under a 400 m peak the
clamp would gut the mountain, so:
- **Depth budget**: only clamp when `cover < kMaxSoilCover` (BL implicitly never
  hits this because its roads are flat at datum and its hills are ≤ 330 m; native
  should cap it explicitly, e.g. 60 m).
- **Beyond that budget**, the road must not go through — reroute, or accept a
  genuine hole (which is a much larger project, correctly deferred).

Register corridors at boot, before the first tile generates, exactly as
`EchoRoads::build()` is hoisted above the region boot (`host_echotropolis.cpp:984-990`).

**(c) Everything the vision needs that BL simply does not have.**
- **Bridges over water** — BL has none (§0.4). Build from `RACING_WORLD.md:436-449`.
- **Banking** — every BL road has `bank = 0` implicitly; the geometry is a flat
  ribbon. Native `RoadSample::bank` + `kBankPerKappa = 55`, `kBankMax = 0.10 rad`
  (`echo_roads.cpp:78-79`) is net-new relative to BL and should be kept.
- **Piers / elevated deck** — BL's only elevated road is the 60 m `hw_overpass` on
  four box pillars (`world-structures.js:584-597`). Native `pillar()`
  (`echo_roads.cpp:500-524`, footing + tapered shaft + hammerhead) is far ahead.
- **Junctions** — BL computes intersection *positions* (`city-roads.js:322-339`)
  but emits no junction geometry. Native has 12-gon patches, stop bars and zebras
  (`echo_roads.cpp:1471-1560`, `:1666-1704`).
- **Road collision** — BL's spline roads have none at all (§2.1). Native
  `RoadCollisionMesh` already solves it.
- **Traffic** — BL cycles traffic-light materials (`city-roads.js:16`, `:216`) but
  has no vehicles on the roads. Native has kinematic `poseCar` traffic
  (`host_echotropolis.cpp:1328-1391`).

### 4.4 The light budget, concretely

A single BL named tunnel's lighting, costed against the native 64-light forward cap
(`shaders/mesh.frag:56`):

| Tunnel | length | strips @15 m | PointLights (every 3rd) | bay lights | total lights |
|---|---|---|---|---|---|
| North | 350 | 23 | **8** | 3 | **11** |
| East | 300 (only 200 dressed) | 12 | 4 | 0 | 4 |
| South | 400 | 26 + 50 extra | **9 + 17** | 0 | **26** |
| West | 300 | 20 | 7 | 0 | 7 |
| | | **131 strips** | | | **48** |

South alone would eat 41 % of the entire scene light budget, and all four together
would consume 75 % of it before a single street lamp, headlight or muzzle flash. **Port the *strips*
as emissive geometry (BL's own auto-tunnel already does exactly this —
`economy-roads.js:664`) and keep at most 6–8 real point lights per tunnel,
nearest-K selected**, which is both `RACING_WORLD.md:433`'s recommendation and
BL's own better half. Do not port the 3-strip point-light ratio literally.

---

## 5. COORDINATE / SCALE MAPPING

### 5.1 Frame

| Property | BL | Native | Conversion |
|---|---|---|---|
| Up axis | **+Y** | **+Y** | none |
| Handedness | **left-handed** — `scene.useRightHandedSystem` is **never set** anywhere in `Q3Engine\src\` (repo-wide grep: 0 hits), so Babylon's default LH applies | **right-handed** — `app/terrain.h:100` states *"RH, +Y up"* | **negate one horizontal axis** (see §5.2) |
| Yaw convention | `Math.atan2(dirX, dirZ)` — measured from **+Z**, e.g. `world-structures.js:514`, `freeway-tunnels.js:538` | standard `atan2` usage varies per site | verify per call site |
| Ground datum | `PLANET_Y = -300` (`core/x3-state.js:41`) | `y = 0` nominal; facility pad at `-2` (`app/terrain.cpp:397`) | **`y_native = y_BL + 300`** |
| Sea level | `-308` = datum − 8 | `kWorldSeaLevel = -10` (`app/terrain.h:147`) | −8 vs −10 — near enough; use native |

### 5.2 The Z flip — already half-applied in native, and inconsistently

`app/terrain.cpp:383-385` declares *"Native compass: +Z = north … Band placement +
peak heights follow the Babylon map (N z~8300 … S z~-9000)"*.

But **BL's north is −Z**:
- `x3-mountains.js:370` — *"NORTHERN RANGE … (z < −7000)"*, peaks at z ≈ −8000 (`:377-392`)
- Southern mesas at z ≈ **+9000** (`:546-560`)
- `tun_north` heads `(0, −1)` (`world-structures.js:669`); `tun_south` also heads `(0,−1)` from z = −2000 (`:683`)

Native `kRanges` (`app/terrain.cpp:386-391`) puts N snow at **z +8300** and S mesa
at **z −9000** — i.e. the mountain layer was ported with **`z_native = −z_BL`**.
Meanwhile native `kTunnels` (`app/city.cpp:53-57`) puts "North Freeway Tunnel" at
(0, 700) heading `(0, +1)` — consistent with the flip — but native district
centres (`app/city.cpp:46-48`) are **byte-identical** to BL's
(`world/x3-world-city.js:15`, `world/x3-city-roads.js:281-282`): Scrapyard
(−600, +500) in both, New District (200, +500) in both.

**So the existing native world already contains a coordinate inconsistency: the
mountains are Z-flipped from BL, the city is not.** Since BL's city is
symmetric-ish about Z = 500 and no BL road reaches the mountains, nothing has
broken yet. It will break the moment a road is authored from the city toward a
range.

**Decision required before any route is authored. Recommendation: adopt
`z_native = −z_BL` universally**, flip the district centres to (−600, −500) /
(200, −500) / (−200, −350), and keep the native `+Z = north` compass. That makes
the native handedness honest (LH→RH via a single axis negation) and makes every
BL table portable by one sign change. The alternative — keeping the city and
un-flipping the mountains — contradicts `app/terrain.cpp:383` and the
`regions.json` gazetteer.

Full transform, if adopted:

```
x_native =  x_BL
y_native =  y_BL + 300            // PLANET_Y datum -> 0
z_native = -z_BL                  // LH -> RH, and BL's -Z north -> native +Z north
yaw_native = -yaw_BL              // atan2(dx, dz) with dz negated
```

Scale factor: **1.0**. See §5.3.

### 5.3 Units and scale — 1:1, with one caveat

BL is **metric-intended and roughly metric-consistent**. Three independent
confirmations that **1 BL unit = 1 metre**:
- **Player capsule**: `CC_HEIGHT: 2.0`, `CC_RADIUS: 0.45` (`core/x3-state.js:181-182`)
  — a 2 m human.
- **Club 1127** converts a real building's imperial dimensions to units and the
  arithmetic is exact: `CW: 15.24` = 50 ft, `CL: 30.48` = 100 ft, `CH: 9.14` = 30 ft
  (`world/x3-club1127.js:35-37`).
- Speed signs read "60 KPH" (`world/x3-freeway-tunnels.js:144-146`).
- Tunnel heights 8–9 m, widths 14–16 m, lane width 2.6 m
  (`economy-roads.js:376`) — all real road figures, though 2.6 m is narrow
  (native `kLaneWidth = 3.4`, `echo_roads.cpp:57`; real motorway lanes are 3.5 m).
- Sidewalks, hydrants, jersey barriers (0.6 × 1.0) are all life-size.

**The caveat: gravity is −28 m/s²** (`systems/x3-scene-init.js:29`,
`:587` `G.CC_GRAVITY`), 2.85× real, with `CC_GROUND_SPEED: 18`, `CC_AIR_SPEED: 12`,
`CC_JUMP_HEIGHT: 3.5` (`core/x3-state.js:183-185`). That is a Quake-lineage arcade
feel choice (the project's ancestry is literally `q3dm17_*.html`), **not** evidence
of a scale factor — the world geometry is metric and the *character* is superhuman.
Port geometry at 1:1 and let the native Jolt gravity stay real.

**Recommended conversion: identity scale.**

### 5.4 World extents — the reconciliation

| Quantity | BL | Native | Note |
|---|---|---|---|
| Ground extent | 30,000 × 30,000, one mesh (`world-terrain.js:243-245`) | unbounded, 32 m streamed tiles (`app/terrain.h:59-62`) | native wins outright |
| Ground resolution | 58.6 m/cell | 1.0 m/cell at LOD0 | **native is 58× finer** |
| Playable boundary | r = 13,000 → storm wall at 14,500 (`world-terrain.js:51-52`) | none | adopt BL's radius as a soft bound if wanted |
| Base terrain amplitude | ≈ ±330 m | `heightScale = 55 m` (`app/terrain.h:64`) | **6× discrepancy — the single biggest mismatch** |
| Mountains | 42 peaks, 100–500 m, at 7.4–10.5 km | 4 range bands, 195–460 m, at 7–10 km (`app/terrain.cpp:386-391`) | already ported, already agrees |
| Ocean | ring r 1200–2600, surface −308 | offshore basin at (1100, −1350) to −90 m, sea at −10 (`app/terrain.cpp:398`, `app/terrain.h:147`) | **incompatible; keep native** |
| Vertical stack | arena +600 / surface −300 / maze −315 / seafloor −480 | facility B1 = 0, caves −178, world bottom −700 (`app/descent_slide.h:8-11`) | **incompatible; keep native** |

**The amplitude gap is the one number that decides whether tunnels happen at all.**
BL generates tunnels wherever terrain rises > 11 m above a flat road over ≥ 6
samples (`economy-roads.js:526`, `:536`). On BL's ±330 m country that fires
constantly. On the native ±55 m base field with a road that *follows* the ground,
it fires **never** — which is precisely why `RACING_WORLD.md:226` found zero
tunnel references in `echo_roads.cpp`: the native road never needs one.

**Two ways to make tunnels real in native, and the port plan should pick one
explicitly:**
1. **Raise the drama locally.** The native mountain layer already exceeds
   `heightScale` deliberately (`app/terrain.h:69-71`, peaks 400–500 m). Route the
   race circuit so it *approaches* a range and bore through a foothill. Requires no
   terrain change, but puts the circuit 7–10 km from the city.
2. **Adopt BL's flat-datum idea for the circuit only.** Author the race route's
   elevation profile independently (which `RACING_WORLD.md:419` already proposes),
   let it run level across a ridge, and let the corridor clamp (§4.3b) cut the ridge
   down onto the tube. This puts tunnels wherever you want them, near the city, at
   the cost of a saddle in the skyline.

**Recommendation: (2) for the first tunnel, (1) for the signature one.**

---

## 6. PORT PLAN, RANKED BY VALUE / EFFORT

Effort scale matches `RACING_WORLD.md:622`: **S** ≤ 2 days, **M** ≈ 1 week,
**L** 2–4 weeks, **XL** > 1 month.

Sequenced to reach **"drivable freeway + one tunnel"** as fast as honestly possible.

### Phase 1 — a drivable freeway (no new tech)

| # | Item | Effort | Kind | Notes |
|---|---|---|---|---|
| **1** | **Port `ROAD_DEFS` as a native route table.** 6 splines / 20 control points (`economy-roads.js:296-305`), Z-negated per §5.2, fed to `EchoRoads` as authored waypoints instead of (or alongside) its probed rim route | **S** | **PORT AS DATA** | This is the whole BL circuit, and it is a closed ~4 km ring. Everything downstream needs it. Blocked only on `RACING_WORLD.md:627` item 1 (getting `EchoRoads` onto `terrainHeightAtWorld`) |
| **2** | Fix the `EchoRoads` ramp `length` double-count (`echo_roads.cpp:1576-1578`) | **S** | fix | Already flagged at `RACING_WORLD.md:389`. One line. Do it before any arc-length race code exists |
| **3** | Decide and apply the **Z-flip** (§5.2) across `kRanges`, `kPads`, `kDistricts`, `kTunnels` | **S** | decision + mechanical | Must happen before route authoring or it becomes a migration |
| **4** | Ship a drivable lap: `EchoRoads` graph + `collisionMesh()` → `addStaticMesh`, on the `host_drive` lane | **M** | reuse | Per `RACING_WORLD.md:627`, `:632` |

**Milestone A: a car drives a closed 4 km BL-authored freeway on native terrain,
with collision.** No tunnels yet, no new rendering tech, no CSM dependency.

### Phase 2 — the first tunnel (the new tech, minimised)

| # | Item | Effort | Kind | Notes |
|---|---|---|---|---|
| **5** | **`sweepProfile(frames, profile2D)`** shared utility + port `makeArchShape` (`economy-roads.js:588-601`) | **M** | **REBUILD** (§4.3a) | Emit the annulus directly — no CSG. Frames from `RoadSample` or `TrackFrame`. Make `descent_slide`'s `Bore` a second caller so it earns its keep twice |
| **6** | **Terrain corridor clamp** in `terrainHeightAt` (§4.3b) — polyline + radius + ceiling, smoothstep, `min`-combined, registered at boot | **M** | **REBUILD**, ported *idea* from `world-terrain.js:911-949` | The pivotal item. Strictly cheaper and safer than the keep-out-rect plan at `RACING_WORLD.md:427-431`. Add a `kMaxSoilCover` budget and log rejections in the zigzag-law house style |
| **7** | **Tunnel run detection** — the clearance classifier at `RACING_WORLD.md:408-420`, seeded by BL's thresholds: cover > 11 m over ≥ 6 samples, ±3 % arc padding (`economy-roads.js:526-548`) | **S** | **PORT AS DATA** | Reuse BL's exact hysteresis constants; they are tuned |
| **8** | **One tunnel end-to-end** on the BL circuit — pick the ring's highest-cover run, bore it, dress it minimally (asphalt + concrete + emissive light strip + arch portals) | **M** | integrate | **Milestone B** |

**Milestone B: you can drive into a mountain and out the other side, at speed,
with collision, on the canonical terrain.** This is the vision's core claim, made
true for the first time — and it retires the placeholder boxes at
`app/city.cpp:386-411` and finally makes `X3_WORLD_BLUEPRINT.md:48`'s "✅" honest.

### Phase 3 — the dressing (high value per hour, zero risk)

| # | Item | Effort | Kind |
|---|---|---|---|
| **9** | **Port the §3.3 interval table verbatim**: light strips @15 m, EXIT @100 m, extinguisher @80 m, jet fan @60 m, phone alcove @120 m, speed sign at the mouth | **S** | **PORT AS DATA** |
| **10** | Port the §3.1 material table (19 materials, exact colour/rough/metal) into `SurfaceLibrary` entries; author 3 replacement PNGs for the DynamicTextures | **S** | port + small art task |
| **11** | Lane markings per §3.4 (5 m segments, ±0.4 m dual-yellow, `±(w/2 − 0.5)` white edges) as a `Paint` bucket | **S** | port as data |
| **12** | Jet fans + vent fans as the only animated props; rotation `0.8–1.2` / `1.5–2.0` rad/s, gated on player-near-zone exactly as `:1123-1131` | **S** | port |
| **13** | Reverb zone: 2.5 s RT60, 35 % wet, radius `length/2 + 50` (`:876-880`, `:522`) | **S** | port as data |
| **14** | Light flicker: one random strip to 30 % for 100 ms every ~2 s (`:1134-1148`) — but drive it off the fixed step, not `setTimeout` | **S** | port + fix |

### Phase 4 — the other three tunnels, as personalities

| # | Item | Effort | Notes |
|---|---|---|---|
| **15** | **North**: jersey centre barrier (0.6 × 1.0, ends 10 m short each side) + 3 pullover bays @100 m with amber lights | **S** | `:547-585`. Bays are 4 × 12 asphalt + wall + ceiling — trivial |
| **16** | **South**: breakdown lane, diagonal hatching @8 m at `+π/6`, separation line @5 m, double lighting | **S** | `:696-731` |
| **17** | **West**: descending grade. **Rebuild, do not port** — BL steps 6 rigid segments and only tilts the floor, leaving a seam every 50 m (`:783-804`). Use a true swept frame at 6.67 % and it is free | **S** | plus drainage grates @3/segment and 8 wet-floor patches at roughness 0.2 |
| **18** | **East**: 30° bend. **Rebuild, do not port** — BL chords it in 5 flat sub-segments and, worse, dresses only the two straights (`:636`) and lerps the vent-fan positions along a straight line the tunnel does not follow (`:663-664`). A swept frame fixes all three bugs at once | **S** | target radius ≈ 191 m |

### Phase 5 — the rest of the world, in value order

| # | Item | Effort | Kind | Why here |
|---|---|---|---|---|
| **19** | **REGENERATE the city procedurally.** Do **not** port `x3-world-city.js` (2934 lines) or `x3-city-roads.js` (1024 lines) | **M** | **REGENERATE** | BL's city is CSG pad-subtraction + hand-placed boxes with no topology. `EchoRoads`' `Avenue`/`HarborStreet` classes with curbs, sidewalks, junction patches and lamp slices are already better in every dimension. **Port only the 3 district centres (done) and the street-grid *spacings*** (main streets 60 m apart, side streets 50 m apart, widths 14/8 — `city-roads.js:289-304`) |
| **20** | **REGENERATE nature.** `world-nature.js` is 28 thin-instance sites keyed on `ps2_heightAt` | **S** | **REGENERATE** | The native woodlands scatter (`origin/echotropolis:echo_woodlands.*`) already does this deterministically with corridor keep-outs. Port only the per-range tree densities (`x3-mountains.js:983-1030`) |
| **21** | **Mountain ranges** — already ported into `app/terrain.cpp:386-391` as 4 bands. Optionally refine using BL's 47 individual peaks | **S** | port as data (optional) | Only worth it if a specific peak becomes a landmark on the circuit. **Re-anchor to `terrainHeightAtWorld`** — BL's float on the datum (`x3-mountains.js:230`) |
| **22** | Craters / flat zones / plateaus (16 + 10 + 8 entries, `world-terrain.js:516-554`) | **S** | port as data | Cheap world character. The crash-site crater at (0,0) r60 d18 is canon |
| **23** | **Facility exterior anchor** at (3500, −2000) → (3500, +2000) after the flip (`x3-facility.js:11`), plus the 9-level floor table (`:23-34`) | **S** | port as data | Native already has `app/facility_exterior.*`; this is the gazetteer coordinate and the vertical program |
| **24** | Club 1127 dimensional check: **15.24 × 30.48 × 9.14 m** against `app/club1127.cpp` | **S** | verify | Free correctness win on a space that is a real place. **Note BL's own header comment is wrong** (§1.6) — if `app/club1127.cpp` was built from the header rather than the constants, it is 14 % too narrow and a full storey too short |
| **25** | **Do NOT port**: the arena (`x3-arena.js`, 2934 lines at Y = +600), the maze/underground (`x3-world-underground.js`), the seafloor base, `x3-space.js` | — | **DROP** | Native has its own, better, incompatible versions of all four (`app/spire_*`, `app/act2_caves`, `app/ocean_base`, `app/space/`), and the vertical stacks are irreconcilable (§5.4) |

### 6.1 Summary: regenerate vs port

**REGENERATE procedurally in native (via EchoRoads + terrain), do not port:**
- All road *geometry* — ribbons, markings, guardrails, sidewalks, junctions
- The city street grid and buildings
- Nature scatter
- Terrain displacement itself
- All culling / merging / freezing machinery (native's is better by construction)
- The four named tunnels' *placement* (§0.3 — they connect to nothing)

**PORT AS AUTHORED DATA:**
- `ROAD_DEFS` — the 6-spline, 20-point closed circuit **← the single most valuable artefact**
- The tunnel-run detection thresholds (11 m cover, 6 samples, 3 % padding)
- The §3.3 dressing interval table and §3.1 material table
- The four tunnel personalities (barrier + bays / curve + vents / breakdown lane / descent + drainage)
- 42 mountain peaks, 17 craters, 10 flat zones, 8 plateaus, 3 district centres, 1 facility anchor
- Reverb 2.5 s / 35 % wet; speed limit 60 KPH; club dimensions

**PORT AS AN IDEA (rebuild the implementation):**
- **The terrain corridor clamp** (§4.3b) — BL's tube-plus-depression is the right
  answer to "tunnels without CSG", and it is a better fit for the native pure-function
  heightfield than anything currently on the plan.
- The swept arch profile (§4.3a) — `makeArchShape` is 13 lines of maths worth keeping;
  the `ExtrudeShape` + CSG2 machinery around it is not.
- Emissive light strips instead of point lights inside tunnels
  (`economy-roads.js:664`) — BL's own better half, and it beats BL's named tunnels.

---

## 7. OPEN ITEMS

1. **§5.2 — the Z-flip decision.** Native currently has the mountains flipped and
   the city not. This must be resolved before any route is authored. Cheap now,
   a migration later.
2. **§5.4 — terrain amplitude.** Native `heightScale = 55 m` vs BL's ≈ ±330 m. The
   race circuit's tunnels depend on which reconciliation (1 or 2) is chosen.
3. **§0.2 — no boot order.** `x3-main.js` is missing from the canonical tree. If the
   exact BL build order matters for anything, it must be recovered from
   `Q3Engine\.claude\worktrees\agent-*\src\x3-main.js` or the 2026-03-05 backup.
   Nothing in this spec depends on it.
4. **Ocean.** BL's r = 1200 ring and native's offshore basin are incompatible and
   BL's is internally inconsistent with its own 30 km terrain (§1.1). Keep native's;
   do not attempt a merge.
5. **`x3-freeway-tunnels.js` was likely never run** (§0.2). Every dimension in §3
   should be treated as authored intent, not validated content — expect to tune
   widths against the actual native car (chassis 1.68 × 1.0 × 3.9 m,
   `app/vehicle.h:117`). A 14 m bore is generous; a 16 m dual-lane with a 0.6 m
   barrier gives 7.7 m per direction, which is fine.
6. **Club 1127's dimensions are self-contradictory in BL** (§1.6): the header
   comment says 43 × 100 × 20 ft, the constants say 50 × 100 × 30 ft. **Ask Tim
   which matches the real Miami room**, since it is a real place and both numbers
   claim to be it. Its world Y is also unresolved (−200 by constant, −500 by the
   only call site, −203…−190 by the LOD zone band).
7. **BL's underground decoration is non-deterministic** — `x3-world-underground.js`
   uses bare `Math.random()` (`:175`, `:538`, `:543`) while every other module uses
   a seeded LCG. Any port must pick a seed discipline; recommend the
   position-hashed `h01()` pattern (`origin/echotropolis:echo_roads.cpp:184-187`).
8. **`VOID_FLOOR_Y` conflict**: −500 (`core/x3-state.js:190`) vs −520
   (`q3dm17_…v17.42.30.html:1529`). Irrelevant to racing; noted for completeness.
9. **Two BL height models coexist and disagree** — `ps2_heightAt`
   (`world-terrain.js:561`) and `estimateAltitude` (`x3-mountains.js:1115`).
   Everything the port takes from `x3-mountains.js` must be re-derived against a
   single native height query.

---

*Sources read read-only on 2026-08-01. Nothing in `Q3Engine\`, `DellGameDev\`, or
any native file outside this document was modified. `origin/echotropolis` was
inspected via `git show` only.*
