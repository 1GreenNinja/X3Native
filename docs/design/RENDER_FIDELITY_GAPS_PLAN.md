# Render Fidelity — Closing the Unity HDRP Gap

**Goal:** take X3Native's PBR/IBL from "Unity-HDRP-ish" to a faithful match of the
Unity HDRP reference (the ShowRoom_Vol30 interior is the litmus test). Written
2026-06-06 after the showroom material/emissive/probe pass landed.

**Quality bar:** Riftforged-in-UE5 ([[project_realm_remake]]) — soft GI + crisp local
reflections + detailed surfaces. The showroom DAY render is the right *vibe* today; this
doc closes the measurable gaps.

> Lane note: most of this is the **13700K's renderer lane** (`engine/rhi`, `shaders/`).
> The 14900K (this machine) owns the converter side (DetailMap import, atlas res) + content
> tuning. Each phase is its own `feat/` branch, headless-gated, integrator re-gates.

---

## Where we are (baseline, committed/working)
- PBR metallic-roughness (normal + MR + AO + emissive), correct sRGB/linear split.
- IBL: analytic-sky env **or** a single interior reflection probe → irradiance + prefilter
  + BRDF LUT (split-sum), with a specular energy ceiling.
- SSAO (+ gated hardware RT-AO `r_rtao`), CSM sun shadows, HDR + bloom + ACES, `r_exposure`.
- HDRP material import: MaskMap→ORM, `_BaseColor` tints, localized emissive maps.

## The gaps (Tim's list) + the plan to close each

### GAP 1 — Local reflections: SSR + multi-probe  ★ highest visual impact
**Current:** one interior probe (whole-scene cube from the camera) + sky IBL. Reflections
are coarse, single-viewpoint, and miss nearby geometry (a panel doesn't reflect the panel
next to it).
**Unity:** screen-space reflections (sharp, contact-accurate) + multiple reflection probes
blended by proximity, with SSR falling back to probes off-screen.
**Plan:**
1. **SSR pass** (P1a): a compute/full-screen pass marching the HDR color + depth buffers
   along the reflection ray; output a half-res reflection RT, denoise + upsample. Composite
   into `mesh.frag`'s specular IBL: `spec = mix(prefilteredProbe, ssrColor, ssrConfidence)`.
   Reuse the existing depth pre-pass + HDR target. Biggest single win for glossy panels.
2. **Multi-probe** (P1b): N interior probes placed per zone (atrium / mezzanine / gallery),
   baked into a small cube **array**; `mesh.frag` picks/blends the nearest probe(s) by world
   pos. Extends the current `regenIblFromSky` scene bake to multiple probe points.
**Effort:** SSR = medium-large (new pass + denoise). Multi-probe = medium (cube array +
blend). **Order:** SSR first (helps every world), then multi-probe.

### GAP 2 — Global illumination (indirect bounce)
**Current:** ambient term + IBL irradiance (sky/probe). No true light bounce; SSGI/GI hooks
exist in the device (per the postmortem) but aren't a full solution.
**Unity:** HDRP screen-space GI + (baked/APV) probe volumes → soft colored bounce.
**Plan:**
1. **SSGI** (P2a): finish/enable the screen-space GI pass (gather indirect from the HDR
   buffer using depth+normals), temporally accumulate (reuse the GI temporal reproject the
   device already snapshots depth for). Gate `r_ssgi`, default off until tuned.
2. **Irradiance-volume bounce** (P2b, later): sample the multi-probe irradiance for diffuse
   GI (cheap, static) — naturally falls out of GAP 1b's probes.
**Effort:** SSGI medium; volume bounce small once probes exist. **Risk:** temporal ghosting
— needs care. Renderer lane.

### GAP 3 — DetailMaps (surface micro-detail)  ★ converter-side, my lane, low risk
**Current:** the converter drops HDRP `_DetailMap`; surfaces are smoother/plainer than Unity.
**Unity HDRP DetailMap packing:** R = desaturated detail albedo, G = detail normal Y,
B = detail smoothness, A = detail normal X; tiled by `_DetailAlbedoScale`/`_DetailNormalScale`
+ a detail UV scale.
**Plan:**
1. **Converter** (P3a): read `_DetailMap` + the detail scales in `convert_unity_pack.py`;
   emit it as a glTF extra / second texture + a detail-UV-scale material param (glTF has no
   native detail slot → use `KHR_materials_*` extras or a custom channel the loader reads).
2. **Loader + shader** (P3b): `ModelLoader` carries the detail map + scale; `mesh.frag`
   samples it at the tiled UV and blends: detail-normal into the normal (partial-derivative
   blend), detail-albedo overlay (overlay/2x), detail-smoothness into roughness.
**Effort:** small-medium, mostly mechanical. **This is the cleanest win I can do solo.**

### GAP 4 — Texture fidelity: atlas res + anisotropy + filtering
**Current:** `MAX_TEX = 1024` cap in the converter; anisotropy 8 + full mip chain already on
(per the postmortem). The 1024 cap softens the 2K–8K Unity atlases.
**Plan:**
1. **Higher atlas res** (P4a): raise the assembled-scene `repack-glb` cap to 2048 (dedup
   means shared atlases embed once — measure the GLB size; KTX2/BC7 compress if needed via
   the existing KTX2 bake tool to keep VRAM/size sane).
2. **Aniso/mip audit** (P4b): confirm aniso 16 on the showroom, trilinear, correct mip bias;
   add a sharpening/contrast-adaptive pass only if still soft.
3. **KTX2/BC7** (P4c, optional): bake the showroom textures to BC7 + mips so 2K costs less
   VRAM than the current uncompressed embed.
**Effort:** P4a trivial (one constant + size check); KTX2 medium (pipeline exists).

---

## Beyond the four (HDRP parity stretch)
- **Contact shadows** (screen-space, sharpens contact AO) — cheap, high impact indoors.
- **Area lights** (the showroom's strip/panel lights are area emitters in HDRP) — LTC area
  lights for soft specular highlights.
- **Volumetric fog / light shafts** — HDRP local volumetrics; big mood win at night.
- **Specular AA** (geometric + Toksvig from the normal map) — kills shimmer on the glossy panels.
- **DLAA/DLSS** (NVIDIA Streamline) — later; SSAA-stills only today.

## Suggested sequencing (impact ÷ effort)
1. **P3 DetailMaps** — solo, low-risk, immediate surface richness. *(start here)*
2. **P4a atlas 2048** — one constant, measure size.
3. **P1a SSR** — the big reflection win (renderer lane).
4. **Contact shadows** — cheap indoor depth.
5. **P1b multi-probe** + **P2 SSGI** — the heavier GI/reflection lift.
6. KTX2, area lights, volumetrics — polish tier.

## Verification per phase
Headless `--screenshot-showroom*` (DAY + NIGHT) A/B before/after, `--smoketest` +
`--test-canonlevel 16/16`, 0 VUID, allocationCount=0. Visual sign-off = Tim's eye
(use the `r_exposure` dial + the planned per-feature `r_ssr`/`r_ssgi` toggles for A/B).
A real Unity reference screenshot (still missing) would let us do a true side-by-side —
**get one if possible.**
