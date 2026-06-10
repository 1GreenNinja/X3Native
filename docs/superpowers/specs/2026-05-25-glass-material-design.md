# Engine Glass / Transparency Material — Design Spec

**Date:** 2026-05-25
**Status:** Design (approved verbally; pending written review)
**Author:** integrator (13700K / P13700)
**Target hardware note:** all techniques here are **raster, no ray tracing** — they run on the integrator's GTX 1080 Ti (Pascal). RT-AO from the doors-death-anim merge is unrelated and unavailable on this box.

## 1. Goal

A **general, reusable translucent-glass material** for the engine — UE-translucent-like — so any mesh can be glass: from **crystal-clear** (an almost-invisible pane that only shimmers + bends light) through tinted/frosted up to **fully opaque**. Replaces the holo-terminal's current "fake translucency by darkening an opaque texture" hack, and becomes the engine's standard transparency path (canopies, windows, holograms, display glass, future water rework).

Non-goal (v1): order-independent transparency, planar/SSR mirror reflections, multi-bounce refraction (glass-behind-glass refracting each other). Listed in §8.

## 2. Requirements

A glass surface is described by a small **material**, all values **runtime-tunable**:

| Param | Range | Meaning |
|---|---|---|
| `opacity` | 0..1 | 0 = crystal clear (body invisible; only shimmer + fresnel + refraction read), 1 = fully opaque. Primary dial. |
| `refraction` (IOR-ish) | 0..~0.1 | screen-space distortion strength of the scene behind. 0 = flat clear pane. |
| `roughness` | 0..1 | 0 = polished/clear (sharp glints, undistorted refraction), 1 = frosted (broad sheen, blurred refraction). |
| `tint` | rgb | glass color; white = colorless. |
| `specular` | 0..1 | strength of the shimmer: specular highlights from scene lights + animated glint. |
| `emissive` | rgb·strength | existing Entity emissive; kept (holo glow). |
| (fresnel) | auto | Schlick reflectance: glass brightens/reflects at grazing angles + edges. Always on; makes clear glass still read as a surface. |

**Two ways to change the variable (UE-like):**
- **Per-material / per-entity:** each glass mesh stores its own params (material-instance style) — a window, a canopy, the holo-terminal all differ.
- **Live dev cvar:** `r_glass_opacity`, `r_glass_refract`, `r_glass_rough`, `r_glass_spec` override/scale the glass the player is looking at, tunable in the console in real time (scrub it like a slider until it's right).

**Presets** (just param combos, not special cases):
- *Clear glass:* opacity ≈ 0.08, roughness 0, refraction low, tint white, specular high → invisible-ish pane that shimmers + bends.
- *Frosted:* roughness ↑.
- *Holo-terminal:* tinted blue, lightly frosted, emissive, mid opacity.

## 3. Architecture

### 3.1 Render-graph change (the core)
Today: depth pre-pass → opaque color pass (depth-EQUAL) → bloom/post. There is **no transparent mesh path**.

Add, **after the opaque HDR color pass and before bloom/tonemap**:

1. **Scene-color capture.** Copy (or alias) the opaque HDR color into a sampled texture `sceneColorCopy` (full-res). Build a **small blurred mip chain** of it (reuse the bloom downsample machinery) for the **frost** lookup.
2. **Transparent pass.** Draw transparent entities, **sorted back-to-front by view depth**, into the same HDR target:
   - Pipeline: `depthTest = LESS_OR_EQUAL` against opaque depth, **`depthWrite = OFF`**, **alpha blend** (`SRC_ALPHA` / `ONE_MINUS_SRC_ALPHA`), `cull = NONE` (double-sided glass).
   - Fragment shader (`glass.frag`):
     - `screenUV` from gl_FragCoord; refraction offset = view-space normal.xy × `refraction` (× IOR factor). Sample `sceneColorCopy` at `screenUV + offset`; pick mip from `roughness` (frost blur).
     - Combine: `refractedScene × (1 - tintStrength) + tint` for the body; add **GGX specular** from the scene's primary lights (sharpness from `roughness`) + a subtle **animated glint** (time-scrolled along the surface) scaled by `specular`; add **Schlick fresnel** reflectance that also lifts alpha at grazing angles; add `emissive`.
     - `outAlpha = clamp(opacity + fresnel·(1-opacity), 0, 1)` — clear bodies, lit edges.

### 3.2 Data + API
- **`GlassMaterial`** struct (opacity, refraction, roughness, tint[3], specular) on `Entity` (or a side table keyed by entity). Passed to the shader via push-constant / per-object UBO already used by the mesh pass.
- **Routing:** `Entity.transparent` bool (set explicitly, or auto when `baseColor[3] < 1`). The scene submit splits entities into **opaque list** and **transparent list**; opaque renders as today, transparent goes through §3.1.
- **Cvars:** registered in the existing console; when set, they override the focused glass entity's params (raycast from camera to find it) or scale globally for dev.

### 3.3 Files touched
- `engine/rhi/IRenderDevice.h` — `GlassMaterial` + transparent flag in the entity/draw API.
- `engine/rhi/VulkanRenderDevice.cpp` — sceneColorCopy target + frost mip chain, transparent pipeline, back-to-front sort, transparent draw between opaque and bloom; descriptor for the scene-color sampler.
- `shaders/glass.vert`, `shaders/glass.frag` — new.
- `app/scene.*` — opaque/transparent split in submission.
- `app/holo_terminal.cpp` — make the panel + rim glass-material instances; delete the "fake by darkening the texture" workaround.
- `app/main.cpp` — register `r_glass_*` cvars; a `--world glass` demo + `--test-glass`.

## 4. Data flow
opaque entities → depth pre-pass → opaque HDR color → **copy HDR → sceneColorCopy (+blur mips)** → sort transparent entities back-to-front → **glass pass** (samples sceneColorCopy for refraction/frost, blends into HDR) → bloom/tonemap → present.

## 5. Error handling / robustness
- Font/RT-style graceful fallback: if the transparent pipeline or scene-copy target fails to create, log once and **skip the transparent pass** (entities render nothing rather than crash) — the build must never break the opaque path.
- Resize: sceneColorCopy + frost mips recreated with the swapchain extent (alongside the existing bloom targets), destroyed on shutdown (must keep `allocationCount=0`).
- No transparent entities this frame → skip the copy + pass entirely (zero cost).

## 6. Testing
- **`--test-glass`** (headless): GlassMaterial param plumbing, opaque/transparent split puts a glass entity in the transparent list, back-to-front sort orders correctly, cvar override mutates the focused material. Print `glass: X/Y passed`, nonzero exit on fail.
- **Gate:** full `--test-*` sweep stays green; **Release `--smoketest` 0 VUID**; **Debug `--smoketest` 0 VUID + `allocationCount=0`** (new targets/pipeline must not leak or trip validation).
- **Visual:** `--world glass` demo (panes at varying opacity/roughness/refraction behind moving geometry) + the holo-terminal in the Spire.

## 7. Performance (1080 Ti target)
One full-res HDR copy + a short blur chain (shared with bloom) + one extra geometry pass over transparent meshes only. Cheap relative to the scene; far cheaper than the unavailable RT path. Frost blur cost scales with mip levels generated (cap at ~4).

## 8. Known limitations (v1, acceptable)
- **Back-to-front per-object sort**, not OIT → interpenetrating/overlapping translucent surfaces can sort wrong.
- Refraction samples the **pre-transparent** scene → glass seen *through* other glass won't refract the nearer glass. Fine for windows/panels.
- Fresnel reflectance is analytic (no real environment reflection / SSR) in v1.

## 9. Migration / payoff
The holo-terminal becomes a tinted, lightly-frosted, emissive glass instance — real see-through glass, mirror + typing fixes already in. Every future glass surface (canopies, windows, display panels) uses the same material with different params, tunable live via `r_glass_*`.
