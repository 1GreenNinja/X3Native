# Cell interior-shadow fix — evidence (fix/interior-shadows, 2026-08-17)

Tim, live in the canonlevel starting cell: shadows "flash incessantly and look
like an aberrated artifact"; stills showed a hard-edged blocky blob under the
bed + dithered/stippled shadow edges + wall speckle.

All frames: 720p headless canonlevel, cell vantages
(`--shot-cam "1.2,1.2,39.6,3.36,-0.58"` and `"1.0,1.6,40.6,3.9,-0.75"`).

## What drew the bad shadows (diagnosed by cvar ablation, frames diffed + read)

| capture | finding |
|---|---|
| default (`r_rtshadows 2`) | giant black stipple sprays on the wall behind the bed |
| `r_rtshadows 1` (sun only) | sprays GONE → the sprays are the POINT-light RT shadows (the ceiling-pipe/bed penumbra from the flickering tube) rendered at 1 spp binary |
| `r_rtshadows 0` | cell floods with leaked sun (the legacy raster cascade misses the interior occluders the TLAS has) — RT sun rays are what keep the cell dark, so tier >= 1 must stay |
| bed skirt | the ROUND-3 contact "shadow blob" discs render their radial fade into uv.y which the glass pass never reads → uniform-alpha hard-edged 24-gon |

The FLASHING: rtsh1.x was a per-frame counter while TAA ran — the 1-spp binary
pattern re-rolled every frame, TAA's neighborhood clamp cannot converge a
full-contrast 0/1 flip, and the cell's key light is a deliberately flickering
fluorescent re-scaling the noise field on top. Screenshot settle partially
averaged it, which is why stills under-reported live play.

## Fix

* `shaders/inc/mesh_shadows.glsl`: Vogel-disk sample spiral rotated per pixel
  by Interleaved Gradient Noise, purely SPATIAL seed. 2-ray consensus probe
  (lit/umbra → done), up to 16 stratified rays in penumbra.
* `engine/rhi/vk/vk_passes.cpp`: rtsh1.x pinned 0 unconditionally.
* `app/cell_dressing.cpp`: the three fake blob discs cut (real RT point
  shadows ground the props; SSAO covers non-RT hardware).

## Measured

* Consecutive-frame shadow flip (settle N vs N+1, wall crop 550x300, luma>6):
  **1374 px before → 61 px after (22x)** — and the before number is flattered
  by TAA settle; live motion re-rolled the full field every frame.
* Same-camera captures bit-stable (max delta 1 LSB).
* GPU at the cell: 2.75 ms (RT off) → 2.94–3.4 ms (multi-ray sun+point RT).
  Whole RT shadow stack ≈ 0.5 ms; live frame at the cell was 7.7 ms.

## Frames

* `before_1spp_sprays.png` / `after_vogel_ign_8spp.png` — wall vantage
* `after_16spp_wall.png` — final 16-spp penumbra
* `before_blob_disc.png` / `after_blob_cut.png` — bed skirt
* `crop_before_gamma.png` / `crop_after16_gamma.png` — gamma-lifted 2x crops

No in-harness pixel-assertion path exists for GPU captures (the --test-*
suites are CPU-headless), so these frames + the numbers above are the gate
evidence. Follow-up filed in the lane report: a proper shadow denoiser and
texel-snapping for the camera-locked legacy sun box.
