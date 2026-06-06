# Black → Beautiful: Rendering the Unity ShowRoom in X3Native

**Engine:** X3Native (native C++20 / Vulkan 1.3, GPU-driven: bindless + multidraw-indirect + compute culling)
**Subject:** Getting the Unity "3D Showroom Level Kit Vol 30" (`Example_01.glb`) from a blank/black frame to a beautiful textured + lit twilight hero render via `--screenshot-showroom`.
**Date:** 2026-05-31 · machine: 14900K + RTX 5090 (gameplay/content/showcase lane)
**Audience:** engine + rendering engineers. Assumes Vulkan, std430 SSBO layout, glTF/PBR, and a depth-prepass + SSAO pipeline.

---

## 0. TL;DR

We imported a heavily-instanced Unity asset pack (`1150` drawables, `~29` unique meshes, `25` materials, embedded jpeg/png maps) and tried to render it in our GPU-driven renderer. The first frame was **black**. The headline bug was not in the importer or the asset — it was a **std430 SSBO stride mismatch** between shader stages (`mesh.vert`/C++ at **128 B**, `depth.vert` at **112 B**, `shadow.vert` at **96 B**) that, combined with the depth pre-pass's `VK_COMPARE_OP_EQUAL` color-pass test, silently discarded **every instanced fragment except SSBO row 0**. That single bug masqueraded as a "draw-count cap" and ate the whole scene. After fixing it, a cascade of import-fidelity issues (near-black PBR metals, an sRGB-everywhere gamma bug, blur, missing alpha-cutout, glass transparency, mis-framed shadows, aliasing) each got chased to root cause and fixed — all as real **engine** fixes that also improve our shipping Level 1 (which stayed `16/16` on `--test-canonlevel` throughout). The capstone gotcha was a Windows **case-insensitive build-target collision** that silently rebuilt the wrong target.

The artifact KB count tells the whole arc: `showroom.png` = **36 KB blank** → **60 KB dark/not-compositing** → **957 KB lit + textured** → dark-twilight hero.

---

## 1. The blank/black showroom (and two false leads)

`--screenshot-showroom` loaded the GLB, framed the camera on the building bounds, and captured. The PNG came back **36 KB** — a blank frame. Nothing recognizable rendered.

We chased two false leads first, both plausible and both wrong:

- **False lead A — far-plane clipping.** The GLB world-AABB tool (`tools/glb_scene_info.py`) showed the scene was large; with the camera framed on the building we suspected geometry was being clipped beyond the **200 m far plane**. We confirmed the far plane and framing were fine — geometry was inside the frustum.
- **False lead B — an apparent `~480` draw-count cap.** When we forced a debug magenta color, magenta appeared on the **ground plane** (drawn via `drawMesh`, a single instance = SSBO **row 0**) but **never** on the building. Pushing the drawable count up, the *entire frame* would blank above roughly **480 instanced draws**. This looked exactly like a hard renderer limit, and we even capped the showroom at `448` drawables to work around it. (Note: there genuinely is a *separate* secondary "blank above ~480 cumulative instanced draws" symptom that turned out to be the same root cause — it vanished after the real fix; no mesh-merge was ever needed.)

Both leads were symptoms. The magenta-on-row-0-only clue was the real tell, but it took a structured isolation pass to read it correctly.

**LESSON:** A "draw cap" that always spares draw call #0 / instance index 0 is not a cap — it is an *indexing* bug. When a symptom is suspiciously correlated with "the zeroth element survives," suspect a per-instance data-addressing problem, not a throughput limit.

---

## 2. THE headline bug — std430 SSBO `ObjectData` stride mismatch × depth-prepass EQUAL

This is the one worth tattooing on the wall.

### The architecture that made it possible

X3Native is GPU-driven. Every drawable's per-object data (model matrix, base-color factor, emissive, texture indices, terrain pack) lives in **one shared per-object SSBO**, set `0`/binding `0`, indexed by `gl_InstanceIndex`. Three vertex stages read that *same* buffer:

- `shaders/mesh.vert` — the color pass
- `shaders/depth.vert` — the camera **depth pre-pass** (feeds SSAO/GI)
- `shaders/shadow.vert` — the sun shadow map pass

Because SSAO/GI default ON, the color pass runs **`VK_COMPARE_OP_EQUAL`** against the depth the pre-pass already wrote (depth-write off in the color pass). The contract is: the pre-pass must rasterize *exactly* the same geometry to *exactly* the same depth as the color pass, or the EQUAL test fails and the color fragment is discarded.

### The root cause

A prior PBR slice grew the `ObjectData` struct to **128 B** in `shaders/mesh.vert` and in the C++ definition — guarded by `static_assert(sizeof(ObjectData) == 128, ...)` (`engine/rhi/VulkanRenderDevice.cpp:1441`). But **`depth.vert` (112 B) and `shadow.vert` (96 B) were never updated.**

With a smaller std430 stride, `depth.vert` computed the byte offset of row *N* wrong for **every** instance index > 0 — it read a *different* object's `model` matrix than `mesh.vert` did. So the pre-pass wrote a different depth than the color pass for every instance except row 0. Under EQUAL, every instanced fragment except SSBO row 0 **failed the depth test and its color was discarded.**

That is *exactly* why forced magenta showed on the sky/ground (a single `drawMesh` instance = row 0) but never on the heavily-instanced building (256 records → 29 unique meshes, all instance index ≥ 1). And it's why it looked like a `~480` draw cap: as more instanced geometry piled up, more of the frame fell to discarded fragments.

This was a **real engine bug affecting ALL instanced rendering**, not a showroom quirk — Level 1's instanced depth and shadows were subtly wrong too.

### How we found it (debug workflow)

A multi-agent debug workflow (9 agents, converging at ~0.9 confidence) plus a **forced-magenta isolation** pass cracked it. Magenta-on-row-0-only pointed straight at per-instance addressing; diffing the three vertex stages' `ObjectData` declarations surfaced the 128/112/96 mismatch immediately once we were looking at *addressing* rather than *throughput*. (Worth noting: in a sibling session, three "high-confidence" agents had each named the *wrong* cause for an unrelated animation bug — so claims were always re-verified against source. Agents accelerate hypothesis generation; they do not replace reading the bytes.)

### The fix (shader-only, no C++ change)

Padded `depth.vert` and `shadow.vert`'s `ObjectData` to **128 B** to match `mesh.vert`/C++, and grouped the transform identically as `viewProj * (model * pos)` so the floating-point depth is **bit-invariant** with the color pass and survives EQUAL. `depth.vert` only uses `o.model`; the trailing 8 uints exist *solely* to force the stride to 128 B. The in-file comment now reads as a permanent warning (`shaders/depth.vert:17-22`):

> "a smaller stride here reads the WRONG row for every instance index > 0 → depth.vert writes a different depth than mesh.vert → the color pass's EQUAL depth test discards every instance except SSBO row 0."

Recompile the `.spv`; Level 1 stayed `16/16` (the fix also corrected Level 1's instanced depth/shadows).

**LESSON — shared-SSBO stride discipline:** If N shader stages bind the *same* SSBO and index it by `gl_InstanceIndex`, the std430 stride **must be byte-identical in every stage and in the C++ struct**, even in stages that read only one field. Enforce it: (a) a single source-of-truth comment citing the byte count in every stage, (b) `static_assert` on the C++ side, and ideally (c) pad unused stages with explicit trailing scalars and *say why*. Any depth pre-pass that drives an EQUAL color test makes a stride bug *invisible until rendering is silently wrong* — there is no validation-layer error, just a black frame.

---

## 3. Near-black PBR metals → IBL ambient-specular + synthetic MR textures

With the stride fixed, the building drew — but the metal panels were nearly **black**. Two causes stacked:

1. **Dark Unity albedo tints.** The pack's metallic materials carry very dark base-color factors. A pure metal with a dark albedo and *no environment to reflect* reflects nothing → black.
2. **No environment map.** Our metallic-roughness branch had direct sun + point lights but no ambient specular, so smooth metals had nothing to mirror.

**Fix:** in `mesh.frag`'s PBR branch we added an **IBL ambient-specular approximation** — metals reflect the scene ambient (≈ `ambient * 3.4` in linear HDR) through `F0`/Fresnel instead of going black (`shaders/mesh.frag:340-341`):

```glsl
vec3 Fr = F0 + (max(vec3(1.0 - a), F0) - F0) * pow(1.0 - NoV, 5.0);
Lo += (ambient * 3.4) * Fr * mix(0.55, 1.1, up) * ao;
```

Because it scales with `setAmbient(...)`, the bright showroom lifts metals while Level 1 stays moderate.

We also made sure metallic materials actually *take* the PBR branch: in `ModelLoader.cpp::buildMaterials`, a material that has a `metallic_factor` but **no** MR texture gets a **synthetic 1×1 metallic-roughness texture** (`shaders/...` data, not color) so `vMrTexIndex != 0` and the shader runs Cook-Torrance (`ModelLoader.cpp:402-406`). MR texture count went `523 → 665`.

> Historical note: an early stop-gap also floored the base-color factor to `max(vFactor.rgb, 0.32)` so dark-metallic albedos didn't read black. That clamp was **later removed** — it washed Glass (0.03) and dark-metal F0 to a flat grey 0.32 and destroyed material accuracy. The IBL term is the *correct* fix; the floor was a crutch.

**LESSON:** A physically-based metal with no environment term is black by construction. Either supply a real IBL/env probe or, at minimum, a cheap ambient-specular Fresnel term — and never paper over dark albedos with a base-color floor (it destroys glass and any genuinely dark material).

---

## 4. The gamma bug — sRGB everywhere → THE import formula

Even lit, surfaces looked subtly wrong — normals read mushy, PBR response was off. Root cause in `ModelLoader.cpp::resolveTexture`: it uploaded **every** texture as **sRGB** (`uploadTexture`'s `srgb` param defaulted `true`; `isNormal` only chose the *fallback default* on decode failure, it never picked the *format*).

sRGB-decoding a **normal map** or a **metallic-roughness** map is corruption: those are *data*, not color. The sampler was applying an sRGB→linear curve to tangent-space normals and to metallic/roughness/AO channels — the core "import carnage."

**Fix:** an explicit `srgb` parameter on `resolveTexture`, set per map type (`ModelLoader.cpp:396, 397, 413, 414, 415`):

- `baseColorTex` → **sRGB**
- `emissiveTex` → **sRGB**
- `metallic_roughness_texture` → **LINEAR** (data)
- `normal_texture` → **LINEAR** (data)
- `occlusion_texture` → **LINEAR** (data)

This maps to the Vulkan format choice in `createSampledTexture`: `VK_FORMAT_R8G8B8A8_SRGB` vs `VK_FORMAT_R8G8B8A8_UNORM` (`VulkanRenderDevice.cpp:6676`).

### THE FORMULA

> **COLOR maps (baseColor, emissive) = sRGB. DATA maps (normal, metallic-roughness, occlusion/AO) = LINEAR.**

Get this wrong and every imported asset has corrupted normals and wrong PBR; get it right and assets import without visual carnage.

---

## 5. Blur — missing mipmaps + anisotropy

Surfaces were **blurry at grazing angles** while camera-facing sprites stayed sharp. Root cause: `createSampledTexture` created images with **`mipLevels = 1`** (no mip chain) and **no anisotropic filtering** — but the sampler asked for `VK_SAMPLER_MIPMAP_MODE_LINEAR`, i.e. it requested mips that didn't exist. Minified/oblique surfaces fell back to a single full-res level and smeared.

**Fix** (`VulkanRenderDevice.cpp:6674-6779`):

- Generate a **full mip chain**: `mipLevels = floor(log2(max(w,h))) + 1`, built by successive `vkCmdBlitImage` 2× linear downscales (image gains `TRANSFER_SRC` usage; per-mip sync2 layout barriers).
- Enable the device feature `f10.samplerAnisotropy = VK_TRUE` (`:162`) and set `sci.anisotropyEnable = VK_TRUE; sci.maxAnisotropy = 8.0f` (`:6773-6774`, well under the RTX limit of 16).

Now grazing surfaces are crisp, not blurry — and minified geometry stops aliasing.

**LESSON:** A `LINEAR` mipmap sampler with `mipLevels = 1` is a silent contradiction — no validation error, just blur. Texture upload should generate mips by default and turn on anisotropy; both are nearly free and fix grazing-angle quality.

---

## 6. Foliage — alpha-cutout (glTF `MASK`)

Tim's catch: the snow firs rendered as **opaque blue quads**. The loader only ever handled `BLEND`; it never read glTF `MASK`. Foliage and people billboards depend on alpha-cutout: keep the opaque pixels, `discard` the transparent atlas background.

**Fix — plumbed end to end via a `texIndex` bit:**

- `ModelLoader.cpp:418-419` reads `cgltf_alpha_mode_mask` → `Material.alphaMask` + `Material.alphaCutoff`.
- `ModelDrawable.alphaMask` carried through; `drawMeshPBR` gained a `bool alphaMask`; `env_art.cpp:523` passes `d.alphaMask`.
- Encoded into **bit 31** of `ObjectData.texIndex`. In `mesh.frag` the base index is masked off (`vTexIndex & 0x3FFFFFFFu`) and the cutout flag decoded (`vTexIndex & 0x80000000u`), then `if (alphaCutout && albedo.a < 0.5) discard;` (`shaders/mesh.frag:283-293`).

Foliage and people now render as proper cutout sprites.

> Pre-pass caveat: the depth pre-pass has **no fragment shader**, so it can't `discard`. Under SSAO/GI's EQUAL test, the pre-pass would punch sky-holes through cutout foliage. The showroom therefore **disables SSAO/GI** (`setSsaoParams{enabled=false}`, `setGiParams{enabled=false}`, `main.cpp:1895-1896`). The proper long-term fix is a depth-prepass alpha-test (`depth.frag`); deferred.

---

## 7. BLEND glass transparency (and "the windows turned white")

`OPAQUE` ✓ and `MASK` ✓ left `BLEND` — real see-through glass (desk, windows). This needs a sorted/partitioned transparent pass.

**Fix (a 3rd debug workflow planned it, ~0.88 conf):**

- `Material.alphaBlend → ModelDrawable → DrawRecord`, encoded into **bit 30** of `texIndex`.
- `prepareFrameData` **partitions** mesh groups OPAQUE-first / BLEND-last around a boundary `m_frameCmdOpaque` (`VulkanRenderDevice.cpp:1862-1869`): two emit passes, opaque groups then blend groups.
- The **shadow** and **depth-prepass** loops are capped at `m_frameCmdOpaque` — glass is excluded from both (`:1896` and the shadow body), so glass never writes shadow/pre-pass depth.
- A **second color-pass loop** draws the blend tail `[m_frameCmdOpaque, m_frameCmdCount)` with `m_meshPipelineTransparent` (`:1971-1973`): src-alpha OVER, depth-test `LEQUAL`, **no depth-write**, cull `NONE`.
- `mesh.frag` decodes bit 30 and applies a glass treatment.

v1 is **unsorted** — fine for the showroom's mostly co-planar panes; proper back-to-front sort is v2.

### Follow-up: "the windows turned white"

Smooth-glass IBL reflection plus too-high an alpha floor made the windows read as opaque white. The Unity `Glass` material has `baseColorFactor.a = 0` (it would be invisible under straight alpha-over), so `mesh.frag` *floors* the opacity and adds a Fresnel rim. The first floor was too aggressive. **Fix** (`shaders/mesh.frag:353-358`):

- Lowered the alpha floor `mix(0.28, 0.6) + fres*0.5` → **`mix(0.10, 0.32) + fres*0.35`** (mostly see-through).
- Lowered the grazing rim contribution `0.15 → 0.06`.
- Gated on `vFactor.a < 0.99` so a fully-opaque (`a = 1`) `BLEND` overlay (e.g. screens) stays solid.

Glass now reads as a translucent, faintly reflective pane instead of a white slab.

**LESSON:** Transparency is a *pipeline partition*, not a shader toggle — blend geometry must be excluded from shadow/depth-prepass writes and drawn last with depth-write off. And imported glass often ships `alpha = 0`; floor the opacity but keep it low, or you trade "invisible" for "opaque white."

---

## 8. Sun shadows + per-scene sun/sky

**Shadows missing.** `computeLightViewProj` framed the shadow ortho on `m_camPos` with a ±45 m box (Level-1-sized) — **100 m short** of the showroom building, so it cast no shadows.

**Fix:** a new device API `setShadowBounds(cx, cy, cz, halfExtent)` (`VulkanRenderDevice.cpp:345-350`) — a non-pure `IRenderDevice` method + Vulkan override that sets `m_shadowOverride` and a fixed center/ortho/depth-half (depth-half = `halfExtent * 1.6` for tall geometry + sun setback). The showroom frames it on the building center with `halfExtent = 150` (`main.cpp:1928`). With no override, shadows stay camera-following → Level 1 unchanged. Firs + building now cast shadows.

**Per-scene sun + sky.** The sun direction and sky gradient were *global daytime constants* baked into the shaders. To get a showroom **twilight** without disturbing Level 1, we plumbed them **per-scene through the UBO**:

- `cam.sunDir` (`shaders/mesh.frag:54`) — the lighting + shadow sun direction comes from `SkyParams.sunDir`, no longer a hardcoded const. The showroom sets a **low raking** sun `(0.6, 0.42, -0.2)` (`main.cpp:1877`) vs Level 1's high `(0.4, 1.0, 0.3)`.
- `sky.frag` gained per-scene `zenith` and `horizon` colors in `SkyUBO` (`shaders/sky.frag:30-31`) so the showroom gets a **Unity-matched dark-zenith / bright-horizon** gradient.

This per-scene plumbing is what lets one renderer serve both a midday lab and a dusk showroom from the same shaders.

---

## 9. Anti-aliasing — 4× SSAA for stills

The "glowing edge strips" Tim saw on the panels were **aliasing**, not emissive. For headless hero stills we added supersampling: `DeviceDesc.ssaa` (headless-only). The device renders at `width*ssaa × height*ssaa` and box-downscales in `writeCapturePng` (`VulkanRenderDevice.cpp:91-94, 798-818`). The showroom uses **`ssaa = 4`** (`main.cpp:1769`) → 5120×2880 down to 1280×720 = **16 samples/px**, pristine.

We deliberately did **not** use FXAA — it softens, and we'd just fixed blur. For the *walkable game* path, the plan is **MSAA** (sharp; ~2–5% on the 5090 per Tim's guidance), resolving into the HDR target with SSAO on the 1× pre-pass depth — deferred. DLAA/DLSS would need NVIDIA Streamline, later.

---

## 10. The build-system gotcha — `X3Engine` vs `x3engine`

The capstone facepalm. The **exe** target is `add_executable(X3Engine ...)` (`app/CMakeLists.txt:1`); the **static lib** target is `add_library(x3engine STATIC ...)` (`engine/CMakeLists.txt:1`). On Windows, target names collide **case-insensitively**.

Consequence: `cmake --build build --target X3Engine` silently resolved to the **lib** and rebuilt only `x3engine` — never the app, never the shader compile (`x3shaders`), and never the POST_BUILD step that copies the freshly-built `.spv` from `build/shaders_spv` to `<exedir>/shaders/` (`app/CMakeLists.txt:134-139`). So shader edits *looked* applied but the runtime kept loading **stale** `.spv`. Hours of "my fix didn't take" was a build that never ran.

**Fix:** **don't target by name — build everything.**

```
cmake --build build --config Release      # ALL_BUILD; runs x3shaders + X3Engine POST_BUILD copy
```

(And always kill `X3Engine.exe` before building — a held exe blocks the link.)

**LESSON:** On a case-insensitive filesystem, never give an exe and a lib names that differ only in case. If you must, never build by `--target` — build `ALL_BUILD`, and remember the shader-copy lives on the *exe* target's POST_BUILD, so skipping it ships stale shaders silently.

---

## 11. Lessons / Reusable Import Formula

The rules that make **any** glTF / Unity asset import without visual carnage:

1. **GAMMA — color vs data.**
   `baseColor`, `emissive` → **sRGB**. `normal`, `metallicRoughness`, `occlusion/AO` → **LINEAR**.
   (Wrong = corrupted normals + wrong PBR. This is the single most important rule.)

2. **ALPHA — honor all three glTF modes.**
   `OPAQUE` → ignore alpha. `MASK` → alpha-cutout `discard` (carry a flag, e.g. `texIndex` bit 31; foliage/people). `BLEND` → real transparency: partition opaque-first/blend-last, **exclude blend from shadow + depth-prepass writes**, draw last with src-alpha OVER + `LEQUAL` + no depth-write. Imported glass often ships `alpha = 0` — floor the opacity *low* + add a Fresnel rim.

3. **SHARED-SSBO STRIDE — identical in every stage.**
   Any per-object SSBO indexed by `gl_InstanceIndex` must have a **byte-identical std430 stride in every shader stage that binds it AND in the C++ struct** — even stages that read one field. Pad unused stages explicitly and document the byte count. A depth-prepass EQUAL test turns a stride mismatch into a silent black frame.

4. **TEXTURE UPLOAD — mips + anisotropy by default.**
   Generate a full mip chain and enable anisotropy, or `LINEAR` mip sampling silently blurs grazing surfaces.

5. **PBR METALS — give them an environment.**
   A metal with no env term is black. Add an IBL/ambient-specular Fresnel term that scales with scene ambient; never floor the base color to fake it.

6. **PER-SCENE LIGHTING via UBO, not shader constants.**
   Sun direction, sky zenith/horizon, ambient, bloom, shadow bounds should be per-scene device state so one renderer serves midday and dusk without forking shaders or regressing other levels.

7. **BUILD — beware case-insensitive target collisions; build `ALL_BUILD`.**
   The shader-copy POST_BUILD rides the exe target; targeting the wrong (lib) name ships stale `.spv`.

Every one of these landed as a **real engine fix**: Level 1 (`--test-canonlevel`) held `16/16` across the entire effort, and the smoketests stayed clean (0 VUID, allocationCount = 0).

---

## 12. What's next

- **Push the dark-twilight / bloom hero tone** further (lower exposure, darker/bluer sky, stronger bloom on emissive). Open question: confirm the spire's window-strip is an HDR-*emissive* material in the GLB — if it isn't, bloom won't halo it (the glow is currently weak/unverified). Options: verify emissive in the GLB, push `setBloom` higher, or lower `kBloomThreshold` (1.10).
- **MSAA for the walkable game path** (the SSAA route is headless-stills-only).
- **Depth-prepass alpha-test (`depth.frag`)** so foliage renders correctly *with* SSAO/GI enabled (today the showroom disables them to avoid sky-holes through cutout foliage).
- **Proper back-to-front sort** for the BLEND pass (v2; v1 is unsorted).
- **Build a walkable `--world showroom`** and place the RescueSystem NPCs (Aria / Keisha / Emily + a guy NPC) inside — the original goal; polish first, then people.
- **Flag the depth-stride + gamma + alpha + mipmap fixes to the fleet** (`docs/NOTE_TO_13700K_pbr_pass.md`) to complete the PBR slice — these are engine-wide wins, not showroom-only.
