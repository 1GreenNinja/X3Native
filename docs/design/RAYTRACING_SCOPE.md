# X3Native — Hardware Ray Tracing Scope (RT reflections / GI / shadows)
*Grounded in the current Vulkan backend. Authored 2026-05-24. The single biggest visual leap available on the 5090 — the GPU's RT cores are currently idle.*

## Goal
Use the 5090's hardware RT cores for **RT shadows, RT reflections (wet streets / metal — the CP2077 look), and RT AO/GI** — replacing/augmenting the screen-space approximations (SSAO, cubemap-less reflections) with ground-truth ray-traced results.

## Current state (what we build on)
- **Vulkan 1.3** via vk-bootstrap; `PhysicalDeviceSelector` at `engine/rhi/VulkanRenderDevice.cpp:151-154` (`set_minimum_version(1,3)`, features 1.0/1.2/1.3). VMA at api 1.3 (`:188`). SDK 1.4.341 installed (full RT).
- Forward shading + **CSM shadows, SSAO, HDR+bloom, ACES** (`shaders/mesh.frag`, `ssao.frag`, `composite.frag`).
- **GPU-driven multidraw-indirect**: `endFrame()` already gathers every draw's instance (mesh + transform) into an SSBO. **This is the TLAS input** — big synergy (below).
- No RT today (no `VK_KHR_*ray*`, no acceleration structures).

## Recommended approach: RAY QUERY (inline RT), not an RT pipeline
Two ways to do hardware RT in Vulkan:
- **RT pipeline** (`VK_KHR_ray_tracing_pipeline`): rgen/rmiss/rchit shaders + a Shader Binding Table. Powerful (full path tracing) but a whole new pipeline type + SBT management — heavy, invasive.
- **Ray query** (`VK_KHR_ray_query`): cast rays *inline* inside the EXISTING fragment/compute shaders (`rayQueryEXT` in GLSL). RT shadows/reflections/AO drop straight into the current lighting pass. **Far less invasive — this is the path.** (Path-traced GI can come later via the RT pipeline if wanted.)

## Implementation plan (ordered; each phase shippable + visible)
**Phase 0 — Enable RT (small, ~1 file).** In the vk-bootstrap selector (`VulkanRenderDevice.cpp:151`):
- Require `VK_KHR_acceleration_structure`, `VK_KHR_ray_query`, `VK_KHR_deferred_host_operations`, and `bufferDeviceAddress` (a 1.2 feature — verify `f12.bufferDeviceAddress=VK_TRUE`).
- Chain `VkPhysicalDeviceAccelerationStructureFeaturesKHR` + `VkPhysicalDeviceRayQueryFeaturesKHR` into device creation.
- **Capability probe**: enumerate device extensions, log `RT: ray-query SUPPORTED` (graceful fallback to SSAO/CSM if absent — so non-RT GPUs still run). *Concrete first step.*

**Phase 1 — Acceleration structures (the foundation).**
- **BLAS per mesh**: build a bottom-level AS from each mesh's existing vertex+index buffers (needs `bufferDeviceAddress` on those buffers). Built once per mesh, cached by `MeshHandle`.
- **TLAS per frame**: top-level AS of instances. **Feed it from the SAME instance list `endFrame()` already builds for multidraw-indirect** — each instance = (BLAS for its mesh, its transform). Rebuild/refit once per frame (refit is cheap for moving objects). This is the key synergy: the GPU-driven path already has the data RT needs.
- Bind the TLAS to the lighting descriptor set.

**Phase 2 — RT shadows (cheapest win, do first).** In `mesh.frag`, replace the CSM shadow lookup with a `rayQueryEXT` shadow ray from the surface to each light: any-hit → in shadow. Crisp contact shadows, no cascade seams/peter-panning. Keep CSM as the fallback.

**Phase 3 — RT reflections (the CP2077 money shot).** A reflection pass (or inline in `mesh.frag` for metal/wet surfaces): reflect the view ray off the surface normal, `rayQuery` the TLAS, shade the hit (re-use the material/lighting). Gate by roughness (mirror→glossy). This + wet emissive-neon streets = the cyberpunk look. Pairs with the Riftforged Neon Sprawl art direction.

**Phase 4 — RT AO / GI (replace SSAO).** RT ambient occlusion via short `rayQuery` cosine-hemisphere rays (ground-truth AO, no SSAO haloing). Optional 1-bounce diffuse GI: gather irradiance from ray hits. Denoise with a temporal/spatial pass (or start noisy + TAA).

## Integration notes / gotchas
- `bufferDeviceAddress` must be enabled AND vertex/index buffers created with the address-usage flag → check VMA buffer creation flags.
- TLAS rebuild belongs in/after the instance-gather in `endFrame()` (reuse that data; don't duplicate the scene walk).
- Ray query needs `GL_EXT_ray_query` in GLSL + SPIR-V 1.4+ (the SDK's glslang supports it).
- Keep every RT path behind a `r_raytracing` cvar with the SSAO/CSM fallback, so it degrades on non-RT GPUs and is A/B-comparable.
- DX12/DXR is NOT relevant — this is the Vulkan backend; ray query is the Vulkan-native equivalent.

## Effort / risk
- Phase 0: ~half a day, low risk (additive, gated).
- Phase 1 (BLAS/TLAS): the real work — 1-2 days; medium risk (new AS management, buffer-address plumbing).
- Phases 2-4: ~half a day each on top of Phase 1; low-medium (shader edits + a descriptor binding).
- **Total: a focused multi-day feature.** Ray query keeps it bounded (no SBT/RT-pipeline). Biggest unknown: TLAS rebuild cost at 10k+ instances — mitigate with refit + only-rebuild-on-topology-change.

## Recommended first move
Phase 0 (enable + probe) + Phase 1 BLAS/TLAS wired to the existing instance gather, then Phase 2 RT shadows as the first visible result. That order de-risks the AS plumbing before spending shader effort.
