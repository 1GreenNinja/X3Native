# Render-Gap Roadmap — Fleet Offensive

> The fleet-wide plan to close the perceived-quality gap between X3Native and the
> Unity-HDRP / UE5 reference bar (showroom litmus test; Riftforged-UE5 vibe).
> Companion docs: `RENDER_QUALITY_UPLIFT.md` (the 17-dimension audit) and
> `RENDER_FIDELITY_GAPS_PLAN.md` (PBR/IBL-faithfulness lanes). This doc is the
> short strike-ordered version both of those feed into. 2026-06-11.

## What we HAVE (engine capabilities, landed + headless-gated)

| Capability | Notes |
|---|---|
| Bindless + MDI GPU-driven draw | ObjectData SSBO (160B std430), MultiDrawIndirect |
| PBR metallic-roughness + ORM + DetailMap | HDRP MaskMap→ORM import, sRGB/linear split |
| Split-sum IBL + interior reflection probe | irradiance + prefilter + BRDF LUT, energy ceiling |
| CSM sun shadows + SSAO (+ SSGI hooks) | half-res SSAO/blur; SSGI chain present, gated |
| Ray-query RT-AO (`r_rtao`) | BLAS/TLAS + inline rayQueryEXT, default off |
| Glass: refraction + frost via scene-color copy | dedicated scene.copy target + blurred mip chain |
| PVS room cull + CPU frustum cull | `r_roomcull` / portal flood + frustum gate |
| HDR pipeline: R16F scene + 5-mip Karis bloom + ACES composite | exposure applied ONCE in composite (`r_exposure`) |
| D15 meshlet foundations | GpuCull pass on the RenderGraph |
| Reversed-Z-ready depth flow + depth prepass | EQUAL-test main pass when prepass on |

## The GAPS, ranked by perceived-quality-per-effort

| # | Gap | Why it ranks here |
|---|---|---|
| 1 | **Bloom + ACES + AUTO-EXPOSURE** (this strike) | Bloom+ACES landed; auto-exposure (eye adaptation) + the cvar surface (`r_tonemap`/`r_bloom*`/`r_autoexposure`) complete the modern HDR loop. Sci-fi emissives glow; dark interiors self-correct. |
| 2 | **TAA → DLSS** | Stable image under motion is the single biggest "engine feels AAA" tell; DLSS rides the TAA plumbing (jitter + motion vectors + history). |
| 3 | **SSR + RT reflections** | Glossy panels/floors reflecting nearby geometry — the showroom's most-missed HDRP feature (GAP 1 in the fidelity plan). |
| 4 | **Volumetric fog + light shafts** | Atmosphere/depth cueing; sells every corridor and the Spire exterior. |
| 5 | **DDGI dynamic GI** | True bounce replaces constant-ambient; after the multipliers above so its cost is spent on an already-stable image. |
| 6 | **Point-light shadows** | Local fixtures currently shadowless; cube/atlas shadow maps. |
| 7 | **Meshlet LOD** | Builds on D15 Tier-0; density without draw-cost explosions. |
| 8 | **Decals / motion blur / DoF / physical sky** | Polish tier; each small, none load-bearing. |

**Strategy:** 1 → 2 → 3 → 4 is ≈80% of the perceived gap for ≈20% of the effort —
they are screen-space/post multipliers that improve EVERY scene with no content
work. GI (5) comes after the multipliers so its expensive photons land on a
tonemapped, anti-aliased, reflective image.

## Fleet lanes

| Machine | Lane |
|---|---|
| **14900K** | Strikes 1 (post stack — this branch) + 3 (SSR/RT reflections) |
| **snake** | Strike 4 — volumetric fog + shafts |
| **predator** | D15 Tier-0 meshlet pipeline |
| **13700K** | Strike 2 — TAA (then DLSS hook) |
| **i5000** | Maxwell floor-check (min-spec gate on every strike) |
| **Fable** | Strike 5 — DDGI design doc |

Every strike: own `feat/` branch off the synced tip, headless screenshot proof,
0-VUID Debug smoketest, full `--test-*` suite green before promote.
