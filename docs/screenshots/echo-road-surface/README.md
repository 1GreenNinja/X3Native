# Echo Harbor road-surface lane — `fix/echo-road-surface`

Tim, flying `--world echotropolis` on `dc9757fa`: *"freeways look GREAT! Buildings are
still ON them, and neon roads look rough.. intersections just cross"*.

All three reported defects were REGRESSIONS/desync, not missing features. Diagnosis and
proof below; every frame is `--world echotropolis --screenshot <out> 30` at the exact
same `--shot-cam` before and after, same binary path, same TOD.

| camera | `--shot-cam` |
|---|---|
| `crown_vista` | ` -250,250,860,-0.37,-0.10` |
| `crown_hi` | ` -250,330,620,0.95,-0.35` |
| `crown_lane` | ` -320,230,742,0.0,-0.18` |
| `harbor_grid` | ` 480,45,700,3.35,-0.15` |
| `freeway_streets` | ` 640,95,430,2.55,-0.14` |

## Root cause 1 — TERRAIN/CONTENT DESYNC (defects 1 and 2)

`tools/echo_terrain_gen.py` writes the heightmap at `N_PNG` but MESHES the land at
`N_MESH = 513` (`sub = px[::step, ::step]`). The player therefore sees a 513² triangle
mesh, while `Heightfield::heightAt` — which seats every road ribbon, junction patch and
building — read a BILINEAR patch of the FULL-RESOLUTION PNG. Two different surfaces.
They agree only at shared vertices.

Worse, the host prefers the out-of-repo bake `D:/GameDev/EchoHarbor/assets/island_mesa`
(2048² PNG → 4× decimation) over the committed regen bake (1025² → 2×), so the running
game got the worse of the two.

Measured on the loaded bake, over the 1111 exported road corridor samples:

```
BEFORE (raw-PNG sampler): p50 -0.161  p90 +0.020  max +18.42 m | BURIED 10.4%  >0.5m 4.7%
AFTER  (mesh-matched)   : p50 -0.150  p90 -0.150  max  -0.15 m | BURIED  0.0%  >0.5m 0.0%
```

`kGroundLift` is 15 cm and the junction patch was `+0.02` — both far below the metres of
decimation error, so paving vanished into the grass in patches while the higher-lifted
paint/curbs survived. That is the "bare strips on grass" report, and the same mechanism is
what put houses in the bay and towers off the crown rim in the terrain lane's captures.

**Fix:** `echo_heightfield.h` gains mesh-matched sampling — `heightAt` evaluates the
RENDERED 513² grid with the generator's own triangle split. AFTER, the ribbon sits exactly
`kGroundLift` proud of the rendered ground at every one of 1483 samples. `ECHO_RAW_HF=1`
restores the old sampler as an A/B lever.

## Root cause 2 — TWO ROAD SYSTEMS ON THE CROWN (defect 2)

`buildCrown` placed the five crown car lanes as single FLAT `road_asphalt.glb` slabs
620×15 m at ONE probe height (`kCarY = heightAt(-20,760)`). Measured against the rendered
land, 78% / 43% / 95% / 89% / 78% of each lane hung more than 2 m in the air (up to 190 m
off the cliff). Their six mutual crossings had no junction geometry at all — EchoRoads had
never heard of them.

**Fix:** the same five centrelines are seeded into EchoRoads as Avenues
(`echo_roads.cpp`, "1d-bis. THE CROWN GRID"); the GLB slabs are gone. They are now draped,
curbed, painted, lamped, and swept by the junction pass. Junction patches went 37 → 47.

The patch itself was also a flat disc at `j.y + 0.02` — 13 cm BELOW the ribbons it joins,
so on any grade half of it sank. It now drapes on the terrain and rides `kPatchLift` proud
of the highest ribbon at the node.

## Root cause 3 — THE CORRIDOR GUARANTEE NEVER COVERED THESE BUILDINGS (defect 3)

`corridorHits()` / `corridorHitsAABB()` were DELETED in V8 on the rationale that "a
building now comes from a LOT or from a FRONTAGE POINT, both of which are outside every
road corridor by construction." Two things that rationale does not cover:

1. The 36-tower DOWNTOWN SKYLINE comes from neither — it is one baked Unity layout dropped
   through a single scene transform, and the veto was removed at that exact site
   (`echo_region_builders.cpp`). The freeway passes within 37 m of the cluster centre.
2. Even for lots, `buildCityPlan` is called with `kRcGround`, so **Freeway and Ramp are not
   in the block-forming graph at all** — the lot system cannot know the freeway exists.

`--test-cityblocks` property 4, "no building intersects a road corridor", is proven over a
SYNTHETIC `RoadGraph` inside the test. It never runs against the real world, which is why
it stayed green while the real city broke.

**Fix:** the AABB veto is back on the skyline path and tests the FULL road set, Freeway and
Ramp included. `6 REFUSED — footprint in a road/freeway corridor`.

## Honest verdict on the result

Defects 1 and 2 are materially fixed and the fix is measurable, not cosmetic. What ships is
a solid BLOCKOUT, not AAA:

- Junction patches still read as **round black coins** against straight ribbons. There is
  junction geometry, crosswalks and stop bars now — but a real intersection is a squared
  box with corner radii, not a 12-gon disc.
- Asphalt-to-grass is a **hard edge**; no verge, no shoulder blend on ground streets.
- The crown grid now has street frontage but almost **no buildings on it** — it reads as a
  road grid in a lawn.
- Some crossings still get no patch (they fall under the `kJuncCross` capture threshold).
- Defect 3 is fixed only at the path that lost its veto. Towers that merely stand *close*
  to the deck are untouched, and the `kRcGround` mask on `buildCityPlan` is unchanged —
  the lot system still cannot see the freeway.
