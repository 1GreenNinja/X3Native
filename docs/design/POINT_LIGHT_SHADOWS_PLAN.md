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

## Storage: gated quality tiers (`r_pointshadows`)

Quality is a setting, not a fixed approach — each box runs the tier its GPU can
afford, the same way `r_rtshadows`/`r_cullpath` tier. The two storage backends
share one sampling entry point (`samplePointShadow`) selected by a spec constant
(`POINT_SHADOW_MODE`), so each tier is its own compiled pipeline variant — no
runtime branch in the hot path.

| `r_pointshadows` | Backend | Atlas / faces | Lights | VRAM | Target |
|---|---|---|---|---|---|
| **0** | off (raster) | — | — | 0 | any (RT path via `r_rtshadows 2` is independent) |
| **1** Low | atlas **4096²** / 512² faces | N=8 | ~64 MB | GTX 980 (4 GB), min-spec |
| **2** High | atlas **8192²** / 1024² faces | N≈10 | ~256 MB | 980Ti / 1080 Ti |
| **3** Ultra | **per-light cube-map array** | 1024²+ | all 64 | ~1.6 GB | 5090 / high-VRAM |

> **Atlas capacity is `(atlasDim/faceDim)² ÷ 6` casters** — so the atlas dim MUST
> scale with the tier's face res, or high-res tiers starve. Step-5 bench proved
> this: a 4096² atlas at 1024² faces fits only **2** casters (16 tiles ÷ 6), not
> the N=16 first drafted here. Tier 1 (4096²/512²) correctly fits the N=8 budget.
> Tier 2 needs the **8192²** atlas (currently the atlas is hardcoded 4096² — the
> one open follow-up before tier 2 is real). The "many lights at high res" case
> is what tier 3's cube-array exists for; don't chase it with an ever-bigger atlas.

- **Atlas (tiers 1–2):** one `D32_SFLOAT` atlas, omni via 6 cube faces packed as
  a 3×2 tile block per caster; CPU budgets the `N` nearest/brightest casters,
  the rest render unshadowed (graceful, no popping cliff). Hardware-compare
  sampler, descriptor **set=5, binding=0**.
- **Cube array (tier 3 — highest quality):** a `samplerCubeArray` depth target,
  one cube per light, no budget, full directional precision (no atlas projection
  seams). Same set=5 binding slot, different descriptor type + pipeline variant.
- **Default tier** is auto-selected from a VRAM/arch probe (à la `detectCullCaps`)
  and always overridable by the cvar. Sun shadow (set=2) is untouched at every
  tier. Override knobs: `r_pointshadow_res`, `r_pointshadow_lights`.

## GPU data

- Extend `GpuPointLight` (`VulkanRenderDevice.cpp:2199`) with a shadow handle —
  `int shadowSlot` (-1 = unshadowed): an atlas-block index in tiers 1–2, a
  cube-array layer in tier 3. Pack into the unused `.a`/`_pad` lanes so the
  `FrameUBO` std140 layout is preserved where possible (verify offsets).
- Per-frame shadow table (small SSBO/UBO): per slot, the 6 face view-proj
  matrices (90° FOV, near=0.05, far=light.range) + (atlas tiers) the tile origin.

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
4. **Sample in `mesh.frag` (atlas, tiers 1–2).** `samplePointShadow(i, fragPos,
   lightPos)` behind spec const `POINT_SHADOW_MODE=0`: dir = fragPos−lightPos →
   dominant axis → face → tile UV → 3×3 PCF compare. Multiply visibility into
   both point-light loops (`:698` dielectric, `:734` PBR). Gate: **the
   acceptance test** — a fixture scene (point light + occluder + floor) shows a
   correct, stable shadow with `r_pointshadows 1`, and is byte-identical to today
   with `r_pointshadows 0`.
5. **Validate atlas + tune.** Headless screenshot proof (shadow on/off), 0-VUID
   Debug smoketest, full `--test-*` suite green. Perf note on the 1080 Ti at
   120 Hz (8.3 ms budget). i5000 confirms the 980/Maxwell floor at tier 1.
6. **Ultra tier (cube-map array, tier 3).** Add the `samplerCubeArray` target +
   `POINT_SHADOW_MODE=1` pipeline variant: one cube/light, no budget, render 6
   faces per light into its layer, sample by direction (no atlas projection).
   Gate: pixel-identical-or-better vs tier 2 on a still camera; runs within frame
   budget on a high-VRAM box (5090). Tier 1/2 unaffected (separate variant).

## Files to touch

- `engine/rhi/VulkanRenderDevice.cpp` — atlas image, sampler, set=5, pass,
  light selection, shadow table upload, `GpuPointLight`/`FrameUBO` extension.
- `shaders/point_shadow.vert` (new) — depth-only, per-face matrix push constant.
- `shaders/mesh.frag` — `samplePointShadow()` + the two loop multiplies.
- `app/main.cpp` — register `r_pointshadows*` cvars; HUD caster count.

## Status (verified on GTX 1080 Ti, no RT)

- **Steps 1–4 DONE** — tier-1 atlas path renders correct hard-edged omni shadows
  (`256511d`, `527842a`, `6ea7856`). Byte-identical when `r_pointshadows 0`.
- **Step 5 DONE** (`83dda12`) — `--test-pointshadows` 8/8, **0 VUID** at tier 1+2
  (real umbra readback: floor luma 35→15). Bench: `tier0 1.08 ms | tier1 1.38 ms
  (+0.30) | tier2 1.05 ms`; tier-1 cost **+0.30 ms**, well inside the 8.3 ms
  (120 Hz) budget. Full `--test-*` suite green.
- **Open before promotion:** (1) scale atlas dim per tier (4096²→8192² for tier 2)
  so tier 2 isn't capped at 2 casters; (2) tier 3 cube-array (Ultra) unbuilt;
  (3) i5000 / GTX 980 (4 GB) floor-check of tier 1; (4) soft PCF is a later polish.

## Out of scope (follow-ups)

Soft/contact-hardening PCF, per-light shadow res by importance, shadow caching
for static lights (only re-render moved casters), spotlight (single-tile) fast
path. Land the correct hard-edged version first.
