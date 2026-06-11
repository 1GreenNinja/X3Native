# SSR + ray-query reflections before/after proof (STRIKE 3)

All captures from the SAME `feat/reflections` Release build. "off" = `--norefl`
(reflection compute + mesh.frag blend disabled, TAA + post stack untouched),
"on" = default (`r_ssr 1`, `r_rtreflections 1`, half-res). Headless captures
are bit-reproducible (the march jitter seed is a pure frame counter).

| file | what it shows |
|---|---|
| `floor2_refl_on/off.png` | Showroom 2nd floor. ON: the glossy curved cladding mirrors the window-mullion grid, the light strip and Aria's silhouette; OFF: a featureless gradient. **The money shot** — see the `_crop` zooms. |
| `floor2_refl_on/off_crop.png` | NEAREST zoom on the mirrored region above the light strip. |
| `showroom_refl_on/off.png` | Spire exterior night still (4x SSAA). Reflection deltas concentrate on the spire's polished hull panels; trees/terrain untouched (cutout pre-pass fix). |
| `deck_refl_on/off.png` | Glass elevator deck at the spire top — sanity: glass itself routes through the dedicated glass pass (no SSR), surrounding panels pick up subtle env detail. |
| `gallery_refl_on/off.png` | Hidden analyst gallery overlook. Top-down floor angles are correctly FADED (backface gate) instead of lying — differences are subtle by design. |
| `level1_refl_on/off.png` | Level-1 spawn still — lab walls/floor pick up subtle SSR; gameplay readability unchanged. |
| `nightsky_refl_on/off.png` | Night-sky sanity: md5-IDENTICAL on/off (sky pixels are not reflectors — confidence 0 by construction). |

A/B verdicts (md5, Level-1 still, vs a fresh base build at 20db239 = feat/taa + metal-ambient):
- base default == feat/reflections `--norefl` -> **identical** (`fd0cce05...`, the exact hash documented in the TAA proof)
- base `--notaa` == feat/reflections `--notaa` -> **identical** (`9e0b3976...`)
- base `--legacypost` == feat/reflections `--legacypost` -> **identical** (`3d5c9dc3...`)
- feat/reflections default run twice -> **identical** (`316e1ec2...`, deterministic)

What shipped:
- `refl.comp`: half-res compute — 24-step world-space march vs the depth buffer
  (mild geometric step growth, 5-iteration binary refine, distance-scaled
  thickness, per-pixel frame-seeded start jitter that TAA integrates away),
  sampling LAST frame's lit scene (the TAA history) through the previous
  unjittered viewProj. Screen-edge + backface fades. Sky writes confidence 0.
- `refl_rt.comp.spv` (same source, `-DREFL_RT=1`): where the march leaves the
  screen or disoccludes, ONE inline ray query into the shared RT-AO TLAS;
  history reprojection of the hit first, flat-shade approximation
  (albedo*(ambient + dim sun), documented) as last resort, at reduced
  confidence. RT miss = sky = stays on IBL (correct by construction).
  Tier-gated: no ray-query support -> SSR-only automatically.
- `mesh.frag`: the reflection radiance REPLACES the prefiltered env radiance
  inside the split-sum IBL specular by confidence x intensity x roughness gate
  (full < 0.25, faded out by 0.6) — same F0/brdfLUT weighting applied exactly
  once; never additive on top of full IBL specular; metal-ambient floor stays
  the final fallback.
- Alpha-cutout depth pre-pass variant (`depth_cutout.vert/.frag`): the
  reflections-forced pre-pass alpha-tests billboard groups so trees/people
  don't become clear-color rectangles (engaged ONLY on reflections frames —
  SSAO/GI-only pre-passes stay bit-for-bit historical).
- Polished-floor material dial: showroom 2nd-floor slab + elevator-atrium pad
  carry a 1x1 metallic-roughness map (rough 0.08 / metal 0.5) via the new
  optional `Entity::mrTex` -> `drawMeshPBR` route.

Honest notes: reflections REQUIRE `r_taa 1` (the history is the color source
and TAA is the denoiser); with TAA off the chain is fully off. Reflected
content is one frame old (camera cuts fall back to IBL for one frame). The
ray-query fallback has no material access (ray-query tier: no SBT/vertex
fetch) — its flat-shade fill is low-confidence by design. Glass surfaces route
through the dedicated glass pass and do not receive SSR (documented next tier).
