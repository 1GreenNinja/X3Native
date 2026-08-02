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
