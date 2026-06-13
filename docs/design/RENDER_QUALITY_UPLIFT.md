# X3Native Render-Quality Uplift Plan — Matching Unity 6 HDRP (ShowRoom_Vol30)

> **Goal:** Close the ENGINE-LEVEL gap between X3Native (custom C++20/Vulkan 1.3, GPU-driven,
> HDR R16G16B16A16, ACES composite, 5-mip bloom, half-res SSAO/SSGI, single 2048 PCF sun shadow,
> analytic sky) and Unity 6 HDRP's high-key, reflective, soft-GI "smooth white sci-fi interior"
> look. The owner is tired of per-scene tweaks. **Every item here is a permanent engine capability,
> not a scene knob.** Reference target: `docs/showroom_interior_reference.md`.
>
> Synthesized from 17 per-dimension investigations of the live code (`engine/rhi/VulkanRenderDevice.cpp`,
> `shaders/*.frag`, `engine/asset/ModelLoader.cpp`, `app/mesh_prims.h`, `app/main.cpp`). Key code
> anchors verified live: dead exposure at `VulkanRenderDevice.cpp:3174` (`cp.exposure = 1.0f`),
> fake flat IBL at `mesh.frag:341` (`Lo += (ambient*3.4)*Fr*...`), the diffuse-only branch at
> `mesh.frag:309` (`if (vMrTexIndex == 0u)`), and the wedge "stair" at `mesh_prims.h:120` (`makeRamp`).

---

## 0. The root cause (why per-scene tweaks never fix it)

The single recurring theme across all 17 dimensions is the same: **the look is reconstructed by hand
in every scene, and the physical machinery HDRP relies on does not exist in the engine.**

- There is **no environment map / cubemap infrastructure anywhere** (grep for cubemap / IBL / prefilter
  returns zero render-side hits). All "reflection" is a directionless `ambient*3.4*Fresnel` constant
  (`mesh.frag:341`), applied **only** to PBR-metal meshes — dielectric white cladding (the bulk of the
  showroom) gets no specular at all (`mesh.frag:309` diffuse-only branch).
- There is **no SSR, no G-buffer** (single forward HDR color + D32 depth only). Floors reflect nothing.
- The **showroom runs with GI/SSAO OFF** (`main.cpp` `setGiParams{enabled=false}`) because the depth
  prepass has no fragment shader and would punch sky-holes through alpha-cutout foliage under the
  EQUAL depth test (see `POSTMORTEM_showroom_black_to_beautiful.md:148`). So the exact scene we want
  to match currently has **zero indirect bounce** — only a flat constant ambient.
- **Exposure is dead** (`cp.exposure = 1.0f`), there is **no grade stage** (one hardcoded Narkowicz ACES),
  **no white balance, no auto-exposure, no TAA** in a live window (SSAA is headless-only).
- The "look" is **~50 inline `SkyParams` blocks** of magic numbers scattered across `main.cpp` /
  `cliffs.cpp` / `act2_world.cpp`, plus `setAmbient`/`setBloom` poking. There is **no first-class look
  object** and the data-driven level JSON has **no lighting/material/look section at all.**

The fix is therefore three-pronged: (A) add the missing physical machinery (IBL, SSR, stable GI, soft
shadows, TAA, grade, auto-exposure), (B) run the real PBR/material path on everything, and (C) make the
"look" a single default-applied first-class object enforced by a style guide + Claude skill.

---

## 1. TOP gaps to Unity HDRP, ranked (biggest wins first)

Ranked by `impact / effort` — high impact + low effort floats to the top. Effort: **L** = low (clone an
existing pass / one shader + small device hook), **M** = medium (new subsystem or vertex/format change),
all assume one experienced engine dev.

| # | Gap | Why it's the look | Impact | Effort |
|---|-----|-------------------|--------|--------|
| 1 | **IBL & environment reflections** (prefiltered cubemap + irradiance + BRDF LUT, fed from a reflection probe) | The *single biggest* "CG-flat vs HDRP" gap. No env map exists; reflections are a flat grey lift regardless of view/surface/surroundings. Gives the blue pad the dome, the metal mullions the snow, the glass the sky. | High | L |
| 2 | **Screen-space reflections (SSR)** (clone of the SSGI 4-pass chain, Fresnel-composited) | Sharp near-field detail on the glossy floor/glass that the prefiltered cube can't resolve. The SSGI chain is a proven, exact template (gather→temporal→bilateral→additive apply). | High | L |
| 3 | **GI fill: SSGI on + DDGI-lite irradiance probes** | The showroom literally renders with GI OFF today. Probes give screen-INDEPENDENT, stable, snow-tinted soft fill (no dead-color crevices, no flicker) and capture off-screen snow SSGI can never see. | High | L |
| 4 | **Soft shadows: CSM + PCSS contact-hardening + screen-space contact shadows** | One 2048 map smeared over a 60–120m scene = blocky edges + a single fixed-width penumbra. HDRP widens penumbra with occluder height and grounds thin geometry. | High | L |
| 5 | **GTAO + contact shadows + bent normals** | Replaces binary Crytek SSAO (over-darkens, halos, faceted dFdx normal). Bent normal drives specular occlusion so reflections darken in recesses; a tiny G-buffer normal target fixes SSGI too. | High | L |
| 6 | **Anti-aliasing: TAA / DLAA (+ DLSS via Streamline on RTX)** | No live AA at all (SSAA is headless-only). The high-key reflective scene crawls/sparkles on the floor, mullions, shadow edges — exactly where HDRP TAA cleans up. SSGI temporal scaffold + jitter is the template. | High | L |
| 7 | **Material discipline: dielectric PBR everywhere + scalar roughness + clearcoat/sheen + ban garish procedurals** | Dielectrics (metallic=0, the white panels/floor) take the no-specular path — physically cannot show a highlight or reflection. No scalar roughness reaches the GPU. Bolted-panel `makeSciFiPanel` clashes with the smooth aesthetic. | High | M |
| 8 | **Tonemapping & color grading** (tunable filmic + WB + contrast/sat + lift/gamma/gain + 3D LUT) | One hardcoded ACES, dead exposure. Cannot hit HDRP's shaped high-key near-clip whites or its cool neutral cast; cannot re-grade per scene without recompiling. | High | M |
| 9 | **Auto-exposure / eye adaptation** (GPU histogram) | Fixed exposure=1.0 means a dim cell and 50k-lux snow tonemap identically. This is the treadmill: brightness consistency must be a measured engine feature, not authored ambient. | High | M |
| 10 | **Sky / atmosphere / volumetrics** (PBR sky → sky-derived IBL → froxel fog god-rays) | Flat analytic gradient with dead air; sky color and ambient fill are two unrelated hand-tuned numbers. Sky-derived IBL kills the manual `setAmbient` sync; froxel fog gives shafts through the dome. | High | L |
| 11 | **Glass / transparency / refraction** (SSR-bg + Beer-Lambert tint + sky reflection + IOR) | Flat constant-opacity milky decal with a faint rim. No bg read, no env reflection, no thickness tint. `water.frag` already implements the exact recipe glass needs — glass reuses none of it. | High | M |
| 12 | **Bloom: energy-normalized single-knob (scatter+intensity)** | Per-mip additive `*0.85` AND a separate `+bloom*0.06` interact non-linearly → look drifts per scene (the literal cause of 4 different `setBloom` calls). Threshold 1.10 blocks the soft white-panel wash HDRP wants. | High | M |
| 13 | **Lighting workflow & units** (physical lux/lumens + EV + area/soft lights) | Hardcoded `0.75 * kSunColor`; sun strength/tint is a shader edit, not data. No area lights → tiny hard GGX dots instead of broad soft sheen. | High | L |
| 14 | **Geometry discipline + the "angled monstrosity"** (bevel/chamfer + real stairs + smoothing + tangents) | Hard 0-radius edges show a black-then-blown seam under high-key+bloom; the showroom "stair" is a bare wedge (see §4). No tangent in `MeshVertex` → no normal maps possible. | High | M |
| 15 | **Texture sampling quality** (16x aniso + neg LOD bias + KTX2/BC7 runtime loader) | Aniso hardcapped at 8x (reflective floor blurs at grazing); RGBA8 burns 4x VRAM vs BC7. Offline `ktx2bake` encoder exists but the loader half was never built. | Med | M |
| 16 | **Post-processing stack** (sharpen/CAS + 8-bit dither + vignette + grain + CA) | Final image is clean-but-soft, bands on the big smooth white gradients, looks "digital-flat." Dither alone should be on by default. The 2 dead push pads are free room. | Med | S |
| 17 | **PERMANENT style system & enforcement** (RenderLook preset + style guide + Claude skill) | Without a default-applied look object + an enforcement gate, every fix above silently rots the moment someone authors a new scene. This is what makes the bar STICK. | High | M |

---

## 2. Tiered ROADMAP (P0 highest-impact-lowest-effort → P2)

### P0 — Do first. Highest impact, lowest effort, mostly clone existing passes.

**P0.1 — Reflection-probe IBL (prefiltered cubemap + irradiance + BRDF LUT)** — *impact High, effort L*
- **What:** A real split-sum specular IBL path + diffuse irradiance, fed from a probe capture.
- **Approach:** Add `captureEnvProbe(worldPos)` + a `VK_IMAGE_VIEW_TYPE_CUBE` R16G16B16A16 RT (~128–256px/face,
  reuse the main mesh+sky pass for 6 faces; one probe near the atrium centre to start). New compute shaders:
  `ibl_prefilter.comp` (Karis/Epic GGX importance-sample → roughness-mipped radiance cube),
  `ibl_irradiance.comp` (cosine-convolved 16px cube **or** SH9), `ibl_brdf_lut.comp` (one-time 256² RG16F).
  Add `set4 = {samplerCube prefiltered, irradiance, sampler2D brdfLUT}` to the mesh pipeline. **Replace
  `mesh.frag:340-341`** with `R=reflect(-V,N); spec = textureLod(env,R,rough*maxMip)*(F0*lut.x+lut.y)`,
  plus diffuse `irradiance(N)*albedo*ao`. **Light the dielectric path too** (run the env term for
  `vMrTexIndex==0`), so plain white cladding finally gets the sheen.
- **Why first:** It is the largest visual gap and a pure capability gap — which is *exactly why ambient
  tweaks never fix it.* Also unblocks #8 glass and the sky-derived IBL in P1.

**P0.2 — SSR (screen-space reflections)** — *impact High, effort L*
- **What:** Sharp local floor/glass reflections on top of the IBL cube fallback.
- **Approach:** Clone the SSGI chain (the exact template). `ssr_trace.frag` (half-res: reconstruct view
  pos/normal as `ssgi_gather` does, `R=reflect(normalize(P),N)`, ~32-step linear screen march first; HiZ
  over a depth min-pyramid as a follow-up; output rgb=radiance, a=confidence faded by edge/length/roughness).
  Reuse `ssgi_temporal` + `ssgi_blur` **verbatim**. `ssr_apply.frag` composites **Fresnel-weighted (NOT pure
  additive)** by `confidence*fresnel*glossMask`. Add a small **R8G8 material MRT** (roughness, reflectivity)
  written beside `outColor` so SSR knows where/how-blurry to reflect (future-proofs the engine). Slot
  trace→temporal→denoise→apply **after gi-apply, before bloom**. **Fallback to the P0.1 cube** on miss/off-screen.
- **Why:** Layers directly on P0.1 (same machinery), produces the signature wet-floor sheen.

**P0.3 — Turn on GI in the showroom: depth alpha-test prepass + SSGI quality wins** — *impact High, effort L*
- **What:** The showroom's indirect bounce currently does not run.
- **Approach:** Add a `depth.frag` alpha-test prepass (the postmortem's deferred fix) so the SSAO/GI prepass
  can `discard` cutout foliage, then flip `main.cpp` `setGiParams{enabled=true}`. While there, the cheap
  SSGI quality wins: (1) **frame-rotate the gather noise** by golden angle using `FrameConstants.frameIndex`
  (`VulkanRenderDevice.cpp:546`) so temporal accumulation integrates *new* directions; (2) **multi-radius
  gather** (0.5m contact + 3m room); (3) **thickness-aware** occlusion test; (4) **analytic/probe fallback**
  when a sample leaves screen (so window-ward surfaces still receive snow fill).
- **Why:** Cheapest possible "turn the lights on" — the scene we are matching runs with GI off today.

**P0.4 — GTAO + bent normals + contact shadows + tiny normal G-buffer** — *impact High, effort L*
- **What:** Replace binary Crytek SSAO with horizon-based GTAO; add specular occlusion + contact shadows.
- **Approach:** Rewrite `ssao.frag` as a horizon-search integrator (4–8 slices, arc-integral; drops the
  kernel/noise table). Output **bent normal** (avg unoccluded dir) — widen `kSsaoFormat` R8 → RGBA8 (AO in
  `.a`, view-space bent normal in `.rgb`). In `mesh.frag` use the bent normal for the ambient/IBL lookup +
  a specular-occlusion factor `so = saturate(dot(bentN,R)+ao)` on the env term. Add a shared **half-res
  view-space normal G-buffer** (R10G10B10A2 / RG16 octahedral) in the depth prepass so GTAO **and** SSGI
  read a real normal instead of `cross(dFdx,dFdy)`. New `contact_shadow.frag`: ~16-step screen march along
  `cam.sunDir`, multiply into the **direct sun term only**. EMA-temporal the AO via the SSGI machinery.
- **Why:** Fixes halos/over-darkening, grounds thin geometry, and the normal G-buffer is shared infrastructure.

**P0.5 — TAA / DLAA resolve (DLSS via Streamline as RTX tier)** — *impact High, effort L*
- **What:** The engine's default live AA (none exists today in a window).
- **Approach:** (1) **Jitter** projection with Halton(2,3) in `prepareFrameData` (carry cur/prev jitter in the
  UBO). (2) **Motion vectors:** add `prevModel` to ObjectData + `prevViewProj` to the camera UBO, write RG16F
  velocity from `mesh.frag` to a new attachment (imported like `gi.raw`); this also upgrades SSGI from
  camera-only to per-object (fixes its documented ghosting). (3) **`taa_resolve.frag`** between the last HDR
  write and composite, **on the HDR target before tonemap**: reproject history via velocity, YCoCg 3×3
  neighborhood-clamp, ~0.9 history blend, ping-pong `m_taaHistImg[2]` (mirror `m_giAccumImg[2]` + a
  `m_taaHistoryValid` invalidated on resize). DLAA = TAA at native res. (4) **Streamline DLSS/DLAA** gated
  behind `m_rtSupported` (same RTX detection that enables ray_query): feed pre-tonemap HDR + motion + depth +
  jitter; render internal scene smaller for upscale to reclaim budget. Expose `r_aa` via `setAaParams`.
- **Why:** TAA is what makes HDRP read "clean" on reflective/high-contrast content; cheap, reuses SSGI scaffold.

### P1 — Do next. High impact, M effort (new subsystem / format change). Several depend on P0.

**P1.1 — Tonemapping & full color grade** — *impact High, effort M*
- New cached `GradeParams{enabled; exposureEV; tonemapMode(0=ACES,1=hable); toe/shoulder/whitePoint;
  wbTemp/wbTint; contrast/saturation; lift/gamma/gain[3]; lutStrength}` + `setGradeParams` (mirror the
  existing `setXxxParams` idiom). In `composite.frag` after hdr+bloom: exposure → white-balance (CIE
  temp→LMS adaptation) → selected tonemap (keep ACES mode 0, add parameterized Hable mode 1) →
  ASC-CDL color adjust → **3D-LUT** (add a 3rd `sampler3D` binding to `m_postSetLayout2`, load a 32³ `.cube`
  via a small `LutLoader`, lerp by `lutStrength`). Default `enabled=false`/identity so all other scenes are
  byte-identical. Showroom DAY grade lives in the preset (§3).

**P1.2 — Auto-exposure (GPU histogram eye adaptation)** — *impact High, effort M*
- `luminance_histogram.comp` (16×16 groups → 256-bin uint SSBO atomicAdd of log-luminance) +
  `luminance_average.comp` (parallel reduction, clip 40%/90% tails, weighted avg, EV map, frame-rate-
  independent EMA `adapted += (target-adapted)*(1-exp(-dt*speed))` with separate up/down speeds, persists in
  a tiny adaptation SSBO). Schedule as compute passes after the HDR pass, before bloom (reuse the skin/rtao
  compute scaffold). Composite reads the GPU-adapted value instead of pushing `1.0f`. Expose min/max EV, key
  value, speeds as cvars so the high-key target is set **once engine-wide.**

**P1.3 — Material discipline: dielectric PBR everywhere + clearcoat/sheen + clean library** — *impact High, effort M*
- Pack scalar `roughness/metallic/clearcoat/sheen` into a spare vec4 of the 128B `ObjectData` row; plumb
  through ModelDrawable/DrawRecord/`drawMeshPBR`. **Delete the `vMrTexIndex==0` diffuse-only branch** — run
  Cook-Torrance unconditionally (F0=0.04 dielectric when no MR map). Add a 2nd thin GGX clearcoat lobe +
  Charlie sheen; parse `KHR_materials_clearcoat`/`_sheen` in `ModelLoader::buildMaterials`. Add
  `engine/asset/MaterialLibrary.h` presets (WhitePanelSatin, BlueFloorPolished, BrushedMetalMullion,
  TintedGlass, LightGreyCarpet) and have the level/canon-floor builder consume them **instead of**
  `makeSciFiPanelRGBA` (gate the bolted-panel procedural behind a debug flag).

**P1.4 — Sky-driven IBL + PBR sky + froxel volumetrics** — *impact High, effort L–M* (specular path lands on P0.1)
- Replace the ad-hoc gradient in `sky.frag` with Hosek-Wilkie / Bruneton single-scatter (param: sunDir +
  turbidity); bake once/frame into a small lat-long/cube RGBA16F. Feed **that** cube into the P0.1 prefilter +
  SH chain so reflective floors/glass reflect the *real* winter sky+snow and the GI fill **is** the sky — this
  kills the manual `setAmbient`/sky-color sync. Add a small lower-hemisphere "snow-bounce" SH boost. Add a
  **froxel fog pass** (160×90×64 RGBA16F, HG phase, sample the existing sun shadow map per froxel → god-rays
  through the dome) + a composite apply by scene depth. New `SkyParams` fields: turbidity, fogDensity,
  fogHeight, anisotropy, cloudCoverage.

**P1.5 — Glass / refraction** — *impact High, effort M* (reuses `water.frag` recipe + P0.1/P0.2)
- Blit opaque HDR into `m_sceneColorOpaque` (with mips) after the opaque pass; bind it + depth to the
  transparent pipeline. Parse `KHR_materials_transmission/_ior/_volume`; pack into the SSBO row. New BLEND
  branch in `mesh.frag`: screen-space refraction offset (`refract`, scaled by thickness, mip=roughness) →
  Beer-Lambert tint `exp(-(1-attenColor)*thickness)` → reflection via P0.1 env cube (or shared `skyColor()`)
  → Fresnel-blend, spec as HDR so it feeds bloom. "Dark analyst glass" = transmission~0.1 + heavy tint.

**P1.6 — Energy-normalized single-knob bloom** — *impact High, effort M*
- Switch `bloom_up` blend from `ONE,ONE` to a scatter lerp (`dst = mix(dst, up, scatter)` via
  SRC_ALPHA/1-SRC_ALPHA) so energy is conserved regardless of mip count. Scale mip count to resolution
  (7–8 mips ≥1080p). Lower threshold to ~0.0–0.3 with a wide knee; control glow via the **scatter+intensity
  pair only.** Promote `kBloomThreshold/Knee/Scatter/Intensity` to cvars; make `setBloom` a thin wrapper,
  add `setBloomScatter`. Optional lens-dirt 3rd binding (default off).

**P1.7 — Soft shadows: CSM + PCSS + screen-space contact** — *impact High, effort L–M*
- (A) Shadow image → `2D_ARRAY` 4 layers, split frustum into 4 texel-snapped cascades, `mat4 lightViewProj[4]`
  + splits in FrameUBO, cascade select by view depth with a blend band. (B) PCSS in `sampleShadow()`:
  blocker search → `penumbra=(receiver-blocker)/blocker*lightSize` → Poisson PCF radius scaled by penumbra.
  (C) Reuse the P0.4 `contact_shadow.frag`. Optional RT upgrade: a `rayQueryEXT` sun-visibility ray reusing
  the RTAO TLAS, gated behind `m_rtSupported`.

**P1.8 — Physical lighting units + area/soft lights** — *impact High, effort L*
- Sun in lux (`sunIlluminanceLux`), point lights in lumens (`I = lumens/(4π·683)`), camera EV in composite
  before ACES — drop the hardcoded `0.75/kSunColor` and feed `cam.sunColor/sunIlluminance` via FrameUBO.
  Add lightType+size (Rect/Tube/Disc) and the Karis representative-point GGX area approximation in the
  forward loop (no new passes). Upgrade flat hemispheric ambient to a tinted zenith/horizon/ground 2-band.

### P2 — Polish & infrastructure.

**P2.1 — Post-processing stack** — *impact Med, effort S*
- Grow `CompositePush` into the dead 2 pads (vignette, CA, sharpen, grain, invRes…) + `setPostParams`.
  After ACES: CA (edge-only) → **CAS sharpen** (highest-value, pairs with SSAA) → vignette → grain →
  **8×8 Bayer dither** (the one piece worth a tiny default-ON to kill banding on the smooth white gradients).
  Each effect gated by `strength==0`.

**P2.2 — Texture sampling quality + KTX2/BC7 runtime loader** — *impact Med, effort M*
- One-line wins: query `maxSamplerAnisotropy`, clamp to `min(16, limit)` (currently hardcoded 8.0 at
  `VulkanRenderDevice.cpp:6857`); add `mipLodBias ~ -0.5`. Structural: build the missing
  `createCompressedTexture()` (BC7_SRGB/UNORM, BC5) that copies pre-baked KTX2 mips
  (`vkCmdCopyBufferToImage` per level, no blit), gate on `textureCompressionBC`, resolve `.png→.ktx2` via
  `manifest.json` in `resolveTexture()`. (Pairs with the already-built `tools/ktx2bake`.)

**P2.3 — Geometry discipline layer** — *impact High, effort M* (see §4)

**P2.4 — PERMANENT style system & enforcement** — *impact High, effort M* (see §3)

---

## 3. PERMANENT STYLE SYSTEM (so this is permanent, not per-scene)

Three reinforcing layers — this is the deliverable that makes the bar **stick.**

**(A) Engine — one first-class look object.**
Add a POD `RenderLook` that bundles every look knob (`SkyParams` + ambient rgb + bloom + `SsaoParams` +
`GiParams` + the new `GradeParams`) and a single `device->applyLook(const RenderLook&)` that fans out to the
existing cached `set*Params` calls; the device holds an active look and **re-applies it every frame** (same
caching pattern already proven by `setSkyParams`). Ship `engine/render/LookPresets.h` with constexpr presets,
the hero being **`Looks::ShowroomDay`** populated EXACTLY from the proven `applyShowroomTimeOfDay` DAY values
(sunDir −0.0595/0.9355/−0.3483, sunIntensity 3.4, exposure 0.92, zenith 0.20/0.34/0.62, horizon
0.72/0.80/0.92, ambient 0.48/0.52/0.62, bloom 0.12) **plus** SSAO/SSGI on, cool white balance, mild contrast,
the four material presets, IBL on. **Critical default-on hook:** the `level_loader` and every `--world` path
apply `Looks::ShowroomDay` at startup *before* scene code runs, so a scene that sets nothing already renders
at the bar; scenes override **deltas** only. Then delete the ~50 inline `SkyParams` blocks and replace each
with `applyLook(...)` so the magic numbers live in exactly one place.

**Material presets (the reusable library, `engine/asset/MaterialLibrary.h`):**
- `WhitePanelSatin` — albedo 0.86 off-white, roughness 0.35, clearcoat 0.4 / cc-rough 0.12, metallic 0.
- `BlueFloorPolished` — roughness 0.18, clearcoat 0.6.
- `BrushedMetalMullion` — metallic 1, roughness 0.45.
- `TintedGlass` — transmission ~0.25–0.40, IOR 1.5, tint + thickness.
- `LightGreyCarpet` — sheen on.

**(B) Data/spine — the look travels with the level.**
Add an optional top-level `"look"` object to the level JSON (preset name + override fields) and one
parse+apply path in `level_loader.cpp`. Let materials reference **named material presets** instead of the
hardcoded type→RGB switch (`level_loader.cpp:859-867`), so the data-driven loader carries the look forward
with zero C++ edits.

**(C) Enforcement — style guide + Claude skill + optional CI gate.**
- Write **`docs/RENDER_STYLE_GUIDE.md`**: the AAA target (high-key cool grade values, the four material
  presets, clean-BSP-geometry + chamfer/stairs rules from §4, the import gamma/alpha formula already in the
  postmortem §11), and a **10-point AAA checklist** — default look applied? exposure/grade set? SSAO+SSGI on?
  bloom subtle not blown? metals/dielectrics have an env (IBL) term? glass uses the TintedGlass preset +
  Fresnel? no hard black shadows? cool white balance? reflective-floor preset? no procedural grey boxes /
  raw hard edges?
- Add Claude skill **`.claude/skills/x3native-look/SKILL.md`**: codifies the discipline — *every new scene
  starts from a `Looks::` preset via `applyLook`, never hand-rolls `SkyParams`; pick a material preset per
  surface; chamfer + smooth + real stairs; run the AAA checklist; gate with a headless screenshot diff vs the
  showroom reference.* (Complements the existing `x3native-environments` geometry/placement skill, which today
  only says "tune the sky dark/violet" and has no look preset.)
- Optional **`--audit-look`** headless gate: fail CI if a scene renders with the default-sentinel (no look
  applied) so the discipline can't silently rot.

---

## 4. The "angled monstrosity" + geometry-discipline rules

**Identified:** the showroom "stair" is **`x3::prims::makeRamp`** (`app/mesh_prims.h:120`) — a **bare
triangular prism**: one sloped top quad + one vertical riser quad + two flat side triangles
(`mesh_prims.h:152-171`). It is used as a *climbable stair* at `main.cpp:4756` (STAGE1 +Z, full 12m rise) and
`main.cpp:5523` (the "grand white ramp" +X, 6m rise). A comment even claims it is "capped with a white tread
plate so it reads as a clad stair" (`main.cpp:4735`) — **but no tread plate is actually built**; it ships as
a plain white wedge with a single hard top edge. Next to the authored ShowRoom GLB it reads as crude graybox.

Contributing engine-level defects: every `prims` builder emits **hard per-face normals** with **0-radius
sharp 90°/acute corners** (no bevel, no smoothing groups), and `MeshVertex = {pos, normal, uv}`
(`IRenderDevice.h:66`) has **no tangent** — so normal maps are impossible and the canted struts / 48 stacked
stair-riser blocks facet sharply instead of reading as one clad blade. UV scale is set per-call by eye
(0.25/0.4/0.5) so texel density drifts between adjacent surfaces.

**Permanent geometry-discipline rules (enforced via the §3 style guide + skill):**
1. **No player-visible procedural surface ships with raw 0-radius edges.** Add `makeBevelBox` / a beveled
   variant of every primitive (chamfer 0.02–0.04m) so silhouettes/corners catch a soft highlight instead of a
   black-then-blown specular seam under high-key + bloom. *(Biggest crude→clean win.)*
2. **Stairs are stairs, not wedges.** Replace `makeRamp` usages with `makeStair(run, rise, nSteps, halfW)` —
   real treads + risers + 2–3cm bullnose nosing + a continuous side stringer, proper per-tread normals.
   Keep `makeRamp` **only** for genuine ADA-style ramps.
3. **Smooth-shade by crease angle.** Add `weldAndSmooth(PrimMesh, angleThresholdDeg≈40)` that merges
   coincident verts and averages normals below the crease angle (keeps hard edges only where the dihedral is
   genuinely sharp) — fixes the faceted struts and riser stacks.
4. **Tangents in the vertex format.** Extend `MeshVertex` → `{pos, normal, tangent, uv}` (and the
   `createMesh` path + `mesh.vert/.frag`); emit a tangent from the UV gradient so cladding can carry a subtle
   tiling normal map (fine panel bevels/seams) and match GLB wall fidelity.
5. **Consistent texel density.** A `uvScale` helper derived from world size (not per-call magic numbers) so
   all built surfaces share one panel grid that aligns at seams.

---

## 5. Recommended FIRST 3 to implement

1. **P0.1 — Reflection-probe IBL** (prefiltered cubemap + irradiance + BRDF LUT). The single biggest "flat vs
   HDRP" gap, a pure capability gap (so ambient tweaks can't fix it), and it unblocks SSR fallback, glass, and
   sky-driven IBL. Lights the dielectric path too, so white cladding finally gets the sheen.
2. **P0.3 — Turn GI on in the showroom** (depth alpha-test prepass → `setGiParams{enabled=true}` + the cheap
   SSGI quality wins). The scene we are matching currently renders with **zero indirect bounce**; this is the
   lowest-effort "turn the lights on" and immediately removes the dead-color crevices.
3. **P0.5 — TAA/DLAA resolve** (jitter + motion vectors + `taa_resolve.frag`, reusing the SSGI temporal
   scaffold). Makes the high-key reflective output read **clean** instead of crawling/sparkling on floors,
   mullions, and shadow edges — and the motion vectors simultaneously fix SSGI's documented ghosting.

Together these three deliver reflective surfaces (IBL), soft stable fill (GI), and a clean stable image (TAA)
— the three things that most separate the current render from the HDRP reference — and they are all P0
(low-effort, mostly cloning proven existing passes). Land the **`RenderLook` preset + style guide + skill**
(§3 / P2.4) immediately alongside so the gains are applied by default to every future scene and never rot.
