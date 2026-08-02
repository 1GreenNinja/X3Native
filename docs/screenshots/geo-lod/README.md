# Discrete mesh LOD — A/B capture (Lane 5, `inspx/geo-lod`)

Regenerate with:

```
build-ninja\bin\X3Engine.exe --screenshot-geolod docs/screenshots/geo-lod
```

## The rig

Real art, not boxes — LOD pop is a perceptual claim and a box has nothing to pop.

* 36 stations (`assets/converted_glb/Undersea/abyssal_station.glb`, 4 material
  groups each, ~90 m across) on a 6x6 / 220 m grid
* 207 cars (`assets/converted_glb/Vehicles/CTR.glb`, 94,714 tris each, ~4.8 m long)
  scattered across the plaza
* 352 entities, 5 LOD chains, **21,618,714 triangles at LOD0**
* 1280x720, 70 deg vertical FOV, `r_meshlod_err 1.5` px, `r_meshlod_hyst 0.15`
* Each capture is frame 100 of a settled run; frame time is the mean of frames
  40-99. `Scene::resetLodState()` runs between the A and B pass so no hysteresis
  state leaks across.

## The generated chains

| mesh | LOD0 | LOD1 | LOD2 | LOD3 | error at LOD3 |
|---|---|---|---|---|---|
| abyssal_station / Storage | 5,808 | 2,904 | 1,452 | 580 | 0.423 m |
| abyssal_station / Hangars | 2,314 | 1,156 | 578 | 230 | 0.354 m |
| abyssal_station / Cyl_3 | 21,584 | 10,792 | 5,396 | 2,158 | 0.232 m |
| abyssal_station / Cyl_5 | 26,208 | 13,104 | 6,552 | 2,620 | 0.493 m |
| CTR (car) | 94,714 | 47,345 | 23,671 | 9,467 | 0.197 m |

Errors are model-space surface deviation measured by the decimator
(`app/mesh_decimate.cpp`), not guessed ratios.

## Results

| distance | triangles OFF | triangles ON | cut | GPU ms OFF | GPU ms ON | A/B PSNR | px differing >32 |
|---|---|---|---|---|---|---|---|
| near | 16,839,794 | 2,041,836 | **-87.9%** | 2.679 | 0.594 | 31.5 dB | 0.940% |
| mid  | 21,618,714 | 2,302,351 | **-89.4%** | 3.253 | 0.608 | 38.2 dB | 0.242% |
| far  | 21,618,714 | 2,160,849 | **-90.0%** | 3.126 | 0.587 | 44.0 dB | 0.056% |

`*_diff_x8.png` is the per-pixel A/B difference amplified 8x. The differences are
a 1-2 px band on silhouettes and shadow edges — there are no filled regions, no
missing objects and no shifted surfaces, which is the signature of a
screen-space-error budget that is actually being respected.

Draw calls go UP slightly with LOD on (6 -> 14 at `near`, 6 -> 6 at `far`): the
renderer groups by mesh id, so a chain whose instances land on several different
levels emits one indirect draw per level in use. That is the intended trade —
8 extra draw calls for 14.8 M fewer triangles.

---

# Vertex compression (piece 2)

`--vtxfmt N` picks the mesh vertex layout at DEVICE INIT (the vertex input is
baked into every PSO, so it cannot be a runtime cvar). See
`engine/rhi/VertexPack.h`.

| N | layout | stride | vs legacy |
|---|---|---|---|
| 0 | pos f32x3 / nrm f32x3 / uv f32x2 | 32 B | — (today, bit-exact) |
| 1 | pos f32x3 / nrm A2B10G10R10_SNORM / uv f32x2 | 24 B | **-25%** |
| 2 | pos f32x3 / nrm A2B10G10R10_SNORM / uv f16x2 | 20 B | **-37.5%** |

There is no shader-side unpack: these are real Vulkan vertex-buffer formats, so
the fixed-function fetch converts them and `mesh.vert` / `depth.vert` /
`shadow.vert` / `velocity.vert` / the cutout verts / `mesh_probe.vert` are all
UNCHANGED (which also keeps this lane out of `shaders/inc/*`).

## Measured, on the rig above (RTX 5070 Ti, 1280x720)

**Memory — the guaranteed win, exactly as advertised:**

| stride | mesh vertex buffers |
|---|---|
| 32 B | 3,762,304 B (3.59 MB) |
| 24 B | 2,821,728 B (2.69 MB) — **-25.0%** |
| 20 B | 2,351,440 B (2.24 MB) — **-37.5%** |

**Frame time — NO measurable win. This is the honest result:**

| stride | 21.6 M tris, r_csm 0 | 21.6 M tris, r_csm 1 (4 cascades) |
|---|---|---|
| 32 B | 3.179 ms | 5.817 ms |
| 24 B | 3.177 ms | 5.823 ms |
| 20 B | 3.177 ms | 5.829 ms |

Spread 0.2%, i.e. run-to-run noise. Even with the depth pre-pass plus four CSM
cascades re-fetching all 21.6 M triangles, this GPU is triangle-setup bound, not
vertex-fetch-bandwidth bound, so narrowing the vertex buys nothing on the clock.
The win may still be real on a bandwidth-starved part (Pascal / integrated /
handheld) — untested here.

**Image cost — effectively free:**

| format | max per-channel delta vs `--vtxfmt 0` | pixels differing >2/255 |
|---|---|---|
| 1 (24 B) | 1 / 255 | 0.000% |
| 2 (20 B) | 9 / 255 | 0.023% |

`vtxfmt2/near_lod_off.png` is the 20 B capture; the 32 B capture is
`near_lod_off.png` in this directory, and the two are indistinguishable.

**Fallback: `--vtxfmt 0` reproduces today BIT-EXACTLY.** Every one of the six
captures above rendered with `--vtxfmt 0` is MD5-identical to the same capture
taken before the compression commit existed.

## Known limitation, measured not hand-waved

`--vtxfmt 2` stores UV as binary16, which loses absolute precision as the
exponent grows. `--test-geolod` V4 measures it: over a 0..64 tiled UV range the
worst error is 0.0155 uv units = **63.5 texels of a 4096 map**. Format 1 (full
precision UV, still -25%) exists exactly for tiled surfaces, and is why format 2
is opt-in rather than the default.

`--vtxfmt` != 0 also disables GPU skinning for the run: `shaders/skin.comp`
writes a 32 B `MeshVertex`-layout output buffer the draw passes bind directly,
which no longer matches the PSO's vertex input. `supportsGpuSkinning()` returns
false and callers transparently take the CPU skinning path (which packs, because
it goes through `updateMesh`). Packing skin.comp's output is a follow-up.
