# #4 — Velocity Buffer + DLSS (feat/dlss-velocity)

Render-chain item 3, built on top of `feat/tlas-doublebuffer`. Touches only
`engine/` + `shaders/` (no `app/` edits — see the app follow-up at the bottom).

## PART 1 — Velocity buffer (SHIPPED, live)

### What it does
A per-object screen-space **motion-vector** buffer (`RG16_SFLOAT`, full-res) that
the TAA resolve uses to reproject history. This replaces camera-only
reprojection for moving geometry, fixing the soft/ghosted look of fast
**dynamic + skinned** objects (drone, monsters, quick turns). It is also the
required input for DLSS (PART 2).

### Pipeline / data flow
1. **Velocity target** `m_velImg` (RG16F) — created alongside the TAA targets in
   `createBloomTargets()` (`vk_targets.cpp`); tracks the swapchain extent.
2. **Velocity pre-pass** (`shaders/velocity.vert` + `velocity.frag`) — runs right
   after the depth pre-pass, re-rasterizing the **same opaque indirect draws**
   with depth-test **EQUAL** (no depth write), writing `(prevUV - curUV)` per
   pixel. Wired into the render graph in `vk_graph.cpp` between the depth
   pre-pass and the TAA resolve. Recorded by `recordVelocityPassBody()`
   (`vk_gi_rt.cpp`), which mirrors `recordDepthPrePassBody()`'s draw loop.
3. **TAA resolve** (`shaders/taa_resolve.frag`) — new sampler binding 4 (the
   velocity buffer). When `velocityValid` (TaaUBO `params1.z`) is set it samples
   the MV at the **closest-depth neighbor** (depth dilation for clean
   silhouettes) and reprojects `prevUV = vUV + mv`. Otherwise it takes the
   original camera-only fallback (byte-identical to the pre-velocity path).

### Dynamic + skinned motion (the hard part)
- **Camera + instance motion**: the velocity vertex shader transforms each vertex
  by the current model (`objBuf`) and the **previous-frame model**
  (`m_prevModelBuf`, a per-row `mat4` SSBO double-buffered per frame). Per-row
  model history is tracked CPU-side (`m_velPrevModels`) in `prepareFrameData`'s
  grouped SSBO fill, so a row maps to the same object across frames (a topology
  change costs one frame of stale MV for a few rows, contained by the TAA
  neighborhood clamp).
- **Skinning deformation**: the GPU skinning compute (`skin.comp`) already writes
  this frame's deformed verts into `dynVbo[frameIdx]` and last frame's into
  `dynVbo[prevSlot]`. The velocity pass binds **two vertex streams** — current
  verts (binding 0) + previous verts (binding 1, consumed only as `inPrevPos` at
  location 3) — so per-vertex skinning deformation contributes to the MV, not
  just the instance transform. For static meshes both streams point at the same
  VBO, so the skinning term vanishes (only model/camera motion remains).
- **Jitter handling**: the MV endpoints are computed with the **UNJITTERED**
  current + previous viewProj (passed in the velocity UBO), while `gl_Position`
  uses the **JITTERED** camera viewProj so the depth EQUAL test keeps exactly the
  depth-prepass fragments. The per-frame jitter (NDC) is also passed and
  subtracted defensively. Result: the MV is true surface motion, free of the
  per-frame jitter wobble.

### Guards / graceful degradation
- `r_velocity` (`PostFXParams::velocity`) gates the whole feature.
- The pass only runs when: TAA is on, the depth pre-pass runs (SSAO/GI/RT-AO/
  reflections enabled), `r_velocity` is set, and the pipeline + target exist.
- If `velocity.*.spv` is missing (e.g. a build that didn't compile them) the
  pipeline stays null and TAA silently falls back to camera-only reprojection —
  **byte-identical** to the pre-velocity engine. (The shaders are compiled by
  `engine/CMakeLists.txt` into the shared `shaders_spv` dir, so they ship in
  every build including clean CI, with no `app/CMakeLists.txt` edit.)
- `--notaa` is completely unaffected (velocity has no consumer without TAA).

### DEFAULT = OFF (determinism basins)
`r_velocity` defaults **OFF** so the A/B determinism screenshot basins
(`default` / `notaa` / `legacypost` / `norefl`) stay byte-identical to the
pre-velocity build (verified — see the report). Enable with `r_velocity 1`.
When ON, the TAA paths (`default`, `norefl`) shift by exactly the velocity
reprojection (proven: toggling `r_velocity 0` returns both to their baseline
basin value-for-value), which is the intended fast-motion improvement.

## PART 2 — DLSS (SCAFFOLDED — no SDK present)

### Path taken: SCAFFOLD
The NVIDIA DLSS / NGX / Streamline SDK is **not vendored and not fetchable** in
this repo:
- Not in `vcpkg.json` (vcpkg does not carry the gated DLSS SDK).
- Not in `third_party/`, `vendor/`, or any CMake `FetchContent` / `ExternalProject`.
- No `dlss` / `nvngx` / `streamline` / `sl.interposer` symbol anywhere in the tree.

DLSS is a license-gated NVIDIA download, so per the brief it was **not faked**.
PART 1 (velocity) ships green and standalone; the DLSS seam is wired with the
now-ready inputs and the exact drop-in is documented below.

### The clean seam (already in place after PART 1)
DLSS Super Resolution needs four inputs — **all now produced by this branch**:
- **color** — the HDR scene (`m_hdrImg`, pre-tonemap, linear) — the TAA resolve's
  input. DLSS would consume this instead of (or alongside) the TAA resolve.
- **motion vectors** — `m_velImg` (RG16F) — NEW in PART 1. DLSS wants MVs in
  pixels (or a documented scale); `velocity.frag` currently writes UV-space
  `(prevUV - curUV)`; multiply by render extent for the pixel-space convention.
- **depth** — `m_depthImg` (D32F) — already produced by the depth pre-pass.
- **jitter** — the Halton(2,3) sub-pixel offset already computed per frame in
  `prepareFrameData` (`jit`, pixels) and stashed in the TaaUBO/VelUBO.

### Exact remaining SDK step (when the SDK is obtained)
1. **Get the SDK.** Either:
   - **NVIDIA DLSS SDK** (`https://developer.nvidia.com/rtx/dlss`, gated) — drop
     `nvngx_dlss` + `nvsdk_ngx` headers/libs into `third_party/dlss/`, or
   - **NVIDIA Streamline** (`https://github.com/NVIDIAGameWorks/Streamline`) —
     the higher-level interposer (`sl.interposer`, `sl.dlss` plugins).
2. **CMake**: add the include dir + link the import lib in `engine/CMakeLists.txt`
   (PRIVATE to `x3engine`, like the other RHI deps). Gate on a
   `find_path`/`-DX3_WITH_DLSS=ON` so non-NVIDIA builds skip it.
3. **Init** (Vulkan): after device creation call `NVSDK_NGX_VULKAN_Init` (pass
   `m_inst.instance`, `m_dev.physical_device`, `m_dev.device`), then
   `NGX_VULKAN_CREATE_DLSS_EXT` to create the DLSS feature sized to the render
   extent + a chosen `quality` cvar (Performance/Balanced/Quality/UltraPerf →
   render-scale). Store the `NVSDK_NGX_Handle*`.
4. **Per-frame evaluate**: add a graph pass AFTER the velocity pass + main color
   pass (replacing the TAA resolve when `r_dlss` is on). Fill
   `NVSDK_NGX_VK_DLSS_Eval_Params` with:
   - `Color` = `m_hdrImg`, `Output` = a new full-res upscaled target,
   - `MotionVectors` = `m_velImg` (set `InMVScaleX/Y` = render extent if keeping
     the UV-space convention, or change `velocity.frag` to pixel space),
   - `Depth` = `m_depthImg`,
   - `JitterOffsetX/Y` = the per-frame Halton jitter (pixels),
   - `InRenderSubrectDimensions` = render extent.
   Call `NGX_VULKAN_EVALUATE_DLSS_EXT`. Then composite/tonemap the upscaled
   output exactly where the TAA output feeds today (the AE/bloom/composite
   wiring already has a TAA-vs-raw switch — add a DLSS branch).
5. **cvar**: expose `r_dlss` (0=off, 1..4=quality) via `PostFXParams` + a
   `setPostFX` field, mirroring `r_velocity`. The string-binding goes in
   `app/main.cpp` (see the app follow-up).
6. **Shutdown**: `NVSDK_NGX_VULKAN_ReleaseFeature` + `NVSDK_NGX_VULKAN_Shutdown`.

The velocity/depth/jitter inputs are the long pole and are done; integrating DLSS
is the steps above plus the SDK.

## APP FOLLOW-UP (not done here — `app/` is owned by parallel agents)
The engine exposes everything; the app needs a ~5-line cvar string-binding (same
pattern skinned-TLAS used). In `app/main.cpp`:
- Parse `--velocity` / `--novelocity` (and later `--dlss N`) and/or an
  `r_velocity` console cvar.
- Set `px.velocity = <value>` on the `PostFXParams` before `device->setPostFX(px)`
  (the two existing sites ~line 629 and ~line 1973).
- Optionally surface `device->velocityAvailable()` / `velocityEnabled()` in the
  perf HUD.
No engine change is required for that — `PostFXParams::velocity`,
`velocityEnabled()`, and `velocityAvailable()` already exist.
