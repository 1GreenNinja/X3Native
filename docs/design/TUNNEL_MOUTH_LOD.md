# Tunnel LOD — TWO distinct bugs, and they are not the same bug

Authors: 14900k-X3TunnelCarDev (bug 1) + InspectorX/Predator (bug 2), 2026-08-14.

**`terrain.{h,cpp}` and `tunnel_corridor.cpp` are FREE as of 2026-08-14 09:45.**
InspectorX aborted the in-progress main merge rather than hand you a conflict in
files you were mid-commit on. Nothing of Predator's is in flight there — do not
wait on a push that is not coming.

## Bug 1 — the vanishing ridge (14900k, and it is the one in the screenshot)

`buildTileMeshAbs` POINT-samples the height field at the LOD stride, one sample
per vertex. Past 192 m that stride is 4 m. A broad hill is far wider than 4 m and
survives; a crest only a few metres across falls BETWEEN samples and ceases to
exist, leaving the handful of vertices that happened to land on it — a thin
blade. That is the fin.

It is Predator's ridge that vanishes, specifically. `app/terrain.cpp:456` adds a
5th RangeDef ("tunnel ridge") deliberately NARROW so the bore has a hill to go
through. Narrow is exactly what point-sampling destroys. Bug 1 is 14900k's find
on Predator's geometry.

Their fix: at LOD > 0 take the MAX height across the block each coarse vertex
represents, so peaks and ridges survive to the coarsest LOD. Right idea.

### One caveat on the seam argument — worth 5 minutes before writing it

The stated safety property is *"the kernel is a pure function of world
coordinates, so two adjacent tiles at different LODs sampling the same world
position get the same answer."* That holds only if the KERNEL RADIUS is also the
same on both sides. As described the radius is tied to the LOD cell, so at a
Full/Quarter boundary the fine tile returns `h(x)` while the coarse tile returns
`max` over a 4 m block. Those differ by construction — that is the whole point of
the filter — so the seam does NOT agree.

The existing skirts do not save it either: skirts hang DOWN and hide a coarse
tile sitting BELOW its neighbour. A max filter makes the coarse tile sit ABOVE,
which is a step, not a crack.

Two ways out, both cheap:
* fixed kernel radius in WORLD metres for every LOD (seams agree exactly; costs
  a little smoothing up close, and note collision stays raw LOD0 so this
  re-opens a small visual/collision delta near the camera); or
* keep the LOD-tied kernel but blend toward the point sample over the outermost
  cell of each tile, so edges converge to `h(x)` and interiors keep the max.

Collision is genuinely unaffected either way (`collVerts` is LOD0-only), and the
cost estimate (25 samples/vertex at Quarter, on rarely-rebuilt tiles) is sound.

## Bug 2 — the mouth seam (InspectorX)

Separate defect, same subsystem, and it survives bug 1's fix. Reported by DJ
Booth's review as "tunnel-mouth LOD popping".

## The numbers

`TerrainConfig`: `tileSize = 32.0 m`, `tileVerts = 33` (=> 32 quads).

| LOD | vertex spacing | active when |
|---|---|---|
| Full    | **1.0 m** | dist < 80 m  (`tileSize * 2.5`) |
| Half    | 2.0 m | 80 – 192 m   (`tileSize * 6.0`) |
| Quarter | 4.0 m | > 192 m |

`lodForDist()` — `app/terrain.cpp:383`.

## Mechanism — a LOD-ing surface meets a non-LOD-ing one

The tunnel shell, the portal rings and the backfill lid are standalone meshes.
They do **not** decimate. The terrain around them does. The seam between the two
is *exactly the tunnel mouth*.

So when the mouth tile crosses 80 m, the terrain surface resamples from 1 m to
2 m spacing and moves by the resampling error, while the portal geometry stays
nailed where it was. A lip (or a gap) opens at the seam, in the middle of the
thing the player is looking at. That is the pop.

This is why it shows at the MOUTH specifically and not across open hillside:
open terrain LOD error is invisible because everything around it moves together.
At the mouth it is differential.

Two things make it worse:

1. **No hysteresis.** `if (dist < nearD)` is a hard edge. A car sitting near 80 m
   — which is exactly where you idle before entering — flips the tile LOD every
   frame that the distance dithers across the boundary. That is the flicker, as
   distinct from the one-time pop.

2. **Collision is always LOD0** (`terrain.h:35`: "Collision always uses LOD0 so
   the player never falls through a decimated far tile"). Correct decision, but
   it means that past 80 m the surface you SEE is not the surface you DRIVE on.
   At the mouth that reads as the car sitting slightly above or below the road.

The corridor cross-section itself (`halfWidth 8 m` flat floor + `falloff 16 m`)
is NOT badly undersampled even at Quarter — 4 m spacing still gets ~4 samples
across the floor. The floor/falloff CREASE at exactly 8 m is the part that
rounds off, and that is a secondary effect. **The seam is the primary cause.**

## Proposed fix

**(a) Pin corridor tiles to Full.** Any tile whose bounds overlap a registered
corridor never decimates, so the seam can never move.

The query already exists and is pure: `terrainCorridorCount()` and
`terrainCorridorDelta(x, z)` (`app/terrain.h:239,245`). A tile bbox test against
the corridor boxes is enough — no new state.

Cost is negligible: the demo corridor is ~640 m long by ~48 m wide, i.e. about
20 x 2 = 40 tiles. At 33^2 verts that is ~43 k vertices held at full density.

**(b) Add hysteresis** so a tile that switched to Full only drops back at
`nearD * 1.15` (and likewise at `midD`). Kills the boundary flicker for every
tile in the world, not just corridors. Cheap: one comparison against the tile's
CURRENT lod in `updateLod()`.

(a) alone fixes the mouth. (b) alone does not — it only slows the flicker down.
Do both; they are independent and (b) is 3 lines.

## How to prove it, not eyeball it

The trap this lane keeps falling into is verifying by eye. Suggested gate:

* sample terrain height along a lateral line through the mouth at Full and at
  Half, and assert the max |dY| at the seam is under a threshold (it is
  currently non-zero by construction — that delta IS the pop, in metres);
* with (a) in place, the mouth tile's `activeLod` must read Full at every camera
  distance the drive test visits — assert it, do not look at it.

`--test-tunneldrive` (11/11 on `inspx/la-exe`) already drives the whole route and
would be the natural place to hang the second assertion.
