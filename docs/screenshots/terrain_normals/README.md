# Terrain normal relief — before/after

`shaders/mesh.frag` gated its ENTIRE normal-map path behind
`(vFlags & FLAG_TERRAIN) == 0u`. Every terrain fragment in every world therefore
shaded from the geometry normal alone: the splat picked a rock *colour* for a cut
face and then lit it like polished plaster. `terrainNormal()` in
`shaders/inc/mesh_terrain.glsl` now samples each layer's normal map through the
same stochastic-lattice taps, the same `textureGrad` gradients and the same
triplanar plane weights as its albedo, and blends them with the same mix chain.

## How these were captured

All pairs come from ONE binary. `X3_TERRAIN_NORMALS=0` loads no normal maps at
all (bindless index 0 -> the geometry-normal fallback), so the only difference
between a `_before` and its `_after` is the relief — no recompile in between to
smuggle anything else in.

```powershell
$env:X3_TERRAIN_NORMALS="0"   # or "1"
.\build\bin\Release\X3Engine.exe --screenshot-tunnel docs/screenshots/terrain_normals/tunnel_before
.\build\bin\Release\X3Engine.exe --world cliffs --screenshot docs/screenshots/terrain_normals/cliffs_before/01_vantage.png
# echotropolis (ECHO_CITY_PROXY=1 ECHO_TOD=noon, canon cams from ../island-regen/README.md)
.\build\bin\Release\X3Engine.exe --world echotropolis --screenshot <png> --shot-cam "-1750,160,-880,0.35,-0.06"
```

`--screenshot-tunnel` and `--world cliffs` use deterministic built-in cameras, so
there is no `--shot-cam` to get wrong. The echotropolis cams are passed with a
SPACE (`--shot-cam "x,y,z,yaw,pitch"`); with `=` the flag is silently ignored.

## What the frames actually show — read them, don't take my word for it

### `tunnel_before/` vs `tunnel_after/` — 8 viewpoints, the strongest evidence

| shot | honest verdict |
|---|---|
| `08_exit_portal` | **The big win.** The right-hand cut face goes from smooth pale plaster to legible rock: cracks read recessed, ribs raised. The grass gains blade texture instead of a flat green wash. |
| `04_saddle` | Clear win on open ground — the field is no longer a flat colour. |
| `06_mouth_headon` | Real but **modest**. The flanking cuttings gain striation; they still read high-key and chalky. |
| `01_approach` | **Subtle.** The near cuttings are at a grazing angle to the camera and the sun; relief is present but does not carry the frame. |
| `05_portal_detail` | **Weakest.** See the finding below — most of the "plaster" in this frame is NOT terrain. |
| `07_inside_looking_out` | **Essentially unchanged**, correctly: the tunnel lining is a mesh, not terrain. |

### `dbg/n_before` vs `dbg/n_after` — `--set r_debugview 1` (shading normals)

This is the proof, and it is also the disappointment. In `n_after` every terrain
surface is finely perturbed and every non-terrain surface is flat. That second
half matters: the **portal headwall, the cut-and-cover lid, the apron and the
road** stay perfectly flat because they are corridor MESHES, not `FLAG_TERRAIN`.
They are a large fraction of the pale surface in `05_portal_detail`, and this
change cannot touch them. Giving the lid a real material + normal map is a
separate job.

### `echo_before/` vs `echo_after/` — Echo Harbor: **NO EFFECT, and that is the finding**

All three pairs are **byte-identical**, across two independently rebuilt binaries
and six separate process launches. Two code facts explain it:

1. The landform is `EnvArtSystem island` — a GLB loaded by
   `host_echotropolis.cpp`, **not** a terrain mesh, so it never enters the splat.
2. Its glTF material `land` has a `baseColorTexture` and **no `normalTexture` at
   all**, so the ordinary (non-terrain) `perturbNormal` path does not run either.

So "the landform sells; the materials don't" in Echo Harbor is a **different
bug** from the one fixed here, and it is still open. `gorge_inlet.png` shows it:
kilometre-scale cliffs with no per-texel signal of any kind. Two ways out —
flag the island `FLAG_TERRAIN` so it inherits the whole splat (large look change,
and the height bands are tuned to a different world field), or author real
albedo+normal materials for it (safer). Neither is in this lane.

### `cliffs_before/` vs `cliffs_after/` — the second world that does use terrain

`--world cliffs` streams the real terrain, so it is the honest second world.
The flat "golf course" turf gains grass-blade relief and directional shading.

## Known limits, stated plainly

* **The remaining flatness at the tunnel portal is not a normal-map problem.**
  It is (a) non-terrain lid/headwall meshes and (b) a near-white cliff albedo
  under a high ambient/IBL wash, which leaves shading almost nothing to modulate.
* **Distance falloff.** The detail tile is 5.56 m (`kDetailScale` 0.18) on a 2K
  map, i.e. ~368 px/m. Past ~50 m that is sub-pixel and mips average it toward
  flat, which is correct but means relief carries the near field, not the vista.
* **Shimmer was not tested in motion.** Every frame here is a still. The taps go
  through `textureGrad` with unoffset derivatives, which is the right mip/aniso
  selection, but a moving-camera pass is still owed.
* **Strength is deliberately 1.0** (`kTerrainNormalStrength`) — the assets'
  authored relief, not a dialled-up version chosen to make these shots louder.
