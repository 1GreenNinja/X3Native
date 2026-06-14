# Gap 6 — Point-Light Shadows (rasterized, atlas) — Implementation Plan

> Render-Gap Roadmap Gap #6. Lane: **predator (1080 Ti)**, validated by i5000
> (Maxwell floor-check). Branch: `feat/point-light-shadows` off
> `integration/empire-fold` (e443f27). 2026-06-14.

## Why rasterized (not RT)

Point-light shadows already exist via `r_rtshadows 2` (sun + point lights, RT
soft shadows — `shaders/mesh.frag:702-712`). But that path needs hardware ray
tracing. **Half the fleet has none:** GTX 980 / 980Ti (Maxwell) and 1080 Ti
(Pascal) have zero RT cores. This adds a **rasterized** point-shadow path so
local fixtures cast shadows on every non-RT GPU. The two paths are mutually
exclusive at runtime (RT where available, raster otherwise) and never overlap.

## Storage: budgeted cube-face atlas

- **One depth atlas** `m_pointShadowAtlas`: 4096×4096 `D32_SFLOAT` (~64 MB).
  Cheap enough for a 4 GB GTX 980. Hardware-compare sampler (`sampler2DShadow`),
  new descriptor **set=5, binding=0** (sun shadow stays on set=2, untouched).
- **Omni via 6 cube faces packed as tiles.** Each shadow-casting light claims a
  3×2 block of 512² tiles. 4096²/512² = 64 tiles ⇒ up to ~10 lit casters/frame.
- **Per-frame budget.** CPU picks the `N` nearest/brightest shadow-casting point
  lights (`r_pointshadow_lights`, default 8); only those get atlas blocks. Lights
  beyond the budget render unshadowed (today's behavior) — graceful, no popping
  cliff. Tile resolution is `r_pointshadow_res` (default 512).

Cube-face atlas chosen over dual-paraboloid: no seam distortion, simpler correct
sampling, and the VRAM math is already trivial at this tile size.

## GPU data

- Extend `GpuPointLight` (`VulkanRenderDevice.cpp:2199`) with a shadow handle —
  `int shadowSlot` (-1 = unshadowed) packed into the unused `.a`/`_pad` lanes so
  `FrameUBO` size/std140 layout is preserved where possible (verify offsets).
- Per-frame shadow table (small SSBO/UBO): for each budgeted slot, the atlas tile
  origin + the 6 face view-proj matrices (90° FOV, near=0.05, far=light.range).

## Build order (each step independently verifiable)

1. **Resources + cvars + binding.** Create the atlas image, compare sampler,
   set=5 layout, extend the pipeline layout 5→6 sets. Cvars: `r_pointshadows`
   (0/1), `r_pointshadow_lights` (N), `r_pointshadow_res`. No visual change yet
   (atlas cleared, never sampled). Gate: clean build, 0-VUID smoketest.
2. **Light selection + matrices (CPU).** Each frame: rank casters, assign slots,
   compute the 6 face matrices, upload the shadow table. Gate: HUD line shows
   `pointshadow casters: n/N`; no GPU sampling yet.
3. **Atlas render pass.** New `point_shadow.vert` (face matrix via push constant)
   + RenderGraph pass `"pointshadow-depth"` mirroring `"shadow-depth"`
   (`:3802`): for each slot × 6 faces, set the tile viewport, draw scene depth.
   Front-face cull + depth bias like the sun pass. Gate: RenderDoc shows the
   atlas populated; still not sampled.
4. **Sample in `mesh.frag`.** `samplePointShadow(i, fragPos, lightPos)`:
   dir = fragPos−lightPos → dominant axis → face → tile UV → 3×3 PCF compare.
   Multiply the visibility into both point-light loops (`:698` dielectric,
   `:734` PBR). Gate: **the acceptance test** — a fixture scene (point light +
   occluder + floor) shows a correct, stable shadow with `r_pointshadows 1`,
   and is byte-identical to today with `r_pointshadows 0`.
5. **Validate + tune.** Headless screenshot proof (shadow on/off), 0-VUID Debug
   smoketest, full `--test-*` suite green. Perf note on the 1080 Ti at 120 Hz
   (8.3 ms budget) before asking for promotion. i5000 confirms the 980/Maxwell
   floor.

## Files to touch

- `engine/rhi/VulkanRenderDevice.cpp` — atlas image, sampler, set=5, pass,
  light selection, shadow table upload, `GpuPointLight`/`FrameUBO` extension.
- `shaders/point_shadow.vert` (new) — depth-only, per-face matrix push constant.
- `shaders/mesh.frag` — `samplePointShadow()` + the two loop multiplies.
- `app/main.cpp` — register `r_pointshadows*` cvars; HUD caster count.

## Out of scope (follow-ups)

Soft/contact-hardening PCF, per-light shadow res by importance, shadow caching
for static lights (only re-render moved casters), spotlight (single-tile) fast
path. Land the correct hard-edged version first.
