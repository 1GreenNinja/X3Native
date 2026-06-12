# D15 — GPU Culling (Tiers 0/1/2): Fleet Handoff

**From:** Claude (Fable 5, claude.ai chat) — 2026-06-09
**Branch context:** written against `feat/cull-combined` + `main` (d1e8a1b).
Note: main is stale vs. the June-8 branch tips — rebase check required.

> **BRING-UP UPDATE — 2026-06-12, `feat/gpu-cull` (14900K + RTX 5090):**
> Tiers 0/1 + HZB are LIVE and gated green. See
> `docs/screenshots/gpucull/RESULTS.md` for the equivalence numbers (Level 1
> 1192/8568 EXACT on both tiers), the 100k-object bench table, the stills, and
> the precise Tier-2 remaining-work list. The table below is the ORIGINAL
> pre-bring-up status, kept for history; current truth:
>
> | Piece | Status now |
> |---|---|
> | Tier 0 (`r_cullpath 1`) | ✅ ON-GPU GREEN — equivalence exact, 0 VUID Debug, pixel-identical stills |
> | HZB (`r_hzb 1`) | ✅ ON-GPU GREEN — standard-Z verified, drawn+hzb conservation exact, 0 VUID |
> | Tier 1 (`r_cullpath 2` / auto) | ✅ ON-GPU GREEN — CONCURRENT buffers + timeline semaphore, validation silent |
> | Tier 2 (`r_cullpath 3`) | ⚠ NOT WIRED (clamps to Tier 1). Builder still 7/7; task/mesh shaders still the untested originals. |

## Verification status — ORIGINAL (2026-06-09, pre-bring-up)

| Piece | Status |
|---|---|
| `cull.comp`, `hzb_build.comp` | ✅ Compile to SPIR-V (glslang 15, vulkan1.3 target). Logic reviewed. **Needs on-GPU run.** |
| `meshlet.task`, `meshlet.mesh` (Tier 2) | ⚠ Compile to SPIR-V. **UNTESTED on GPU — review required.** Vertex out-block must be diffed against `mesh.vert`/`mesh.frag` interface before first run. |
| `GpuCull.h/.cpp` | ✅ Compiles clean (g++13, C++20) **linked against the repo's real `RenderGraph.cpp`**. |
| Meshlet builder (`buildMeshlets`) | ✅ **7/7 acceptance tests pass** (`runMeshletSelfTest`): budgets, locality, sphere containment, cone tightness, triangle conservation, degenerate input. |
| Tier 0 pass recording | ✅ Code-reviewed against RenderGraph contract. **Needs validation layers + on-GPU verification** (checklist below). |
| Tier 1 async compute | ⚠ **UNTESTED — review required.** Cross-queue semaphore + CONCURRENT-sharing design; must pass validation layers on Turing+ before enabling. |
| HZB mip-chain barriers | ⚠ **REVIEW REQUIRED** under validation layers (per-mip GENERAL-layout write→read flips). |

## Tier 2 — precise remaining work (the one tier that resisted)

The meshlet BUILDER is tested (7/7) and `r_cullpath 3` safely clamps to Tier 1.
Wiring the task/mesh path needs, in order:
1. Enable `VK_EXT_mesh_shader` (+ feature struct) at device creation, gated
   like the RT block. CMake: compile `meshlet.task/.mesh` with
   `--target-env=vulkan1.3 --target-spv=spv1.4` (the RT shader list).
2. Bake meshlets in `createMesh()` (positions+indices are in hand), upload the
   3 meshlet buffers + a pos/normal/uv vertex SSBO. ⚠ The in-file `Vertex`
   struct (`posUVx/nrmUVy/tangent` vec4s) does NOT match the real `MeshVertex`
   (tightly packed 3+3+2 floats, 32 B) — rewrite to 8-float stride.
3. `meshlet.mesh` out-block must mirror mesh.vert's FULL 14-location interface
   (incl. all flat outputs + ObjectData lookup via the visible-instance row);
   `meshlet.task` needs per-instance plumbing (gl_WorkGroupID.y = compacted
   instance slot, firstInstance via push constant).
4. cull.comp (or a CPU pre-pass) must emit `VkDrawMeshTasksIndirectCommandEXT`
   (12 B stride: groupsX=ceil(meshletCount/32), Y=survivors, Z=1) into a SECOND
   indirect buffer — the existing 20 B indexed commands don't apply.
5. Consistency: depth-prepass (EQUAL test) + shadow replay the classic vertex
   path; per-cluster CONE culling is consistent only if the opaque pipeline
   backface-culls (verify cullMode) — and per-cluster HZB must stay OFF in the
   color pass until the prepass uses meshlets too (else EQUAL-test holes).
6. Acceptance: pixel-diff vs Tier 0 on a still camera (the harness from
   RESULTS.md works as-is), 0 VUID Debug.

## What this is

Moves per-object culling from the CPU (current: 6-plane frustum test per draw
record per frame, `r_frustumcull`) onto the GPU, and adds Hi-Z occlusion —
per-object "is it behind something" culling that works where the portal PVS
can't (open terrain, dense outdoor prop fields). Three tiers behind one cvar:

- **Tier 0** (universal, incl. GTX 1080 Ti): cull compute on the graphics
  queue. No `drawIndirectCount` needed — the CPU records the same per-mesh
  multidraw as today; the GPU zeroes/bumps `instanceCount` and compacts
  survivors into `visibleInstance[]`.
- **Tier 1** (Turing+/RDNA+, auto-selected when a dedicated compute queue
  exists on non-Pascal): same dispatches on the async compute queue,
  overlapping prior-frame raster.
- **Tier 2** (`VK_EXT_mesh_shader`, opt-in via `r_cullpath 2` only): per-
  meshlet culling — frustum + backface cone + HZB per ~64-tri cluster.

Tier detection: `detectCullCaps()`. Pascal heuristic: NVIDIA without mesh-shader
support == pre-Turing → pinned Tier 0 (Pascal advertises compute queues but its
async is weak preemption overlap). cvars to add: `r_cullpath` (-1 auto/0/1/2),
`r_hzb` (0/1), plus HUD lines from the stats buffer (tested/drawn/frustum/hzb).

## Integration order (do them as separate verifiable steps)

1. **Frustum-only Tier 0** (`useHzb=false`). Wire: CPU writes `CullInstanceGpu[]`
   + per-mesh indirect cmds with `instanceCount=0`; `mesh.vert` gains ONE
   indirection (`visibleInstance[gl_InstanceIndex + firstInstance]` → instance
   SSBO index). Verify vs. the CPU cull: with both enabled on the same frame,
   `statDrawn` must equal the CPU path's `objectsDrawn` exactly (same planes,
   same conservative sphere test, same ALWAYS_VISIBLE flags). That equivalence
   check is the whole Tier 0 acceptance test.
2. **HZB phase**: pyramid image (R32_SFLOAT, full mip chain), per-mip
   descriptor sets, `addHzbPasses` after depth is available, recreate cull
   pipeline with `useHzb=true`. ⚠ `kReversedZ`: hzb REDUCE flips MIN/MAX and
   `cull.comp::hzbVisible` compare flips with it — verify against the engine's
   actual depth convention (I could not determine it from main; check
   `depth.vert`/projection setup). Acceptance: walk behind the Spire — HUD
   `hzbCulled` climbs, nothing visibly pops for >1 frame.
3. **Tier 1**: create cull buffers `VK_SHARING_MODE_CONCURRENT` across both
   queue families, timeline semaphore, graphics submit waits at
   `VERTEX_INPUT|DRAW_INDIRECT`. Validation layers MUST be silent. Measure with
   Tracy: win shows as cull cost vanishing from the graphics-queue timeline.
4. **Tier 2** (when wanted): bake meshlets at asset-import time via
   `buildMeshlets()` (tested), upload the three meshlet buffers, task/mesh
   pipeline. First fix the mesh shader's vertex out-block + `Vertex` struct to
   match `ModelLoader`'s real layout — both are marked in-file. Acceptance:
   identical image vs Tier 0 on a still camera (pixel-diff a capture).

## Files

- `shaders/cull.comp` — per-instance frustum (+HZB via spec const) + compaction
- `shaders/hzb_build.comp` — depth pyramid (MIN/MAX via spec const)
- `shaders/meshlet.task` / `meshlet.mesh` — Tier 2 (⚠ untested tier)
- `engine/rhi/GpuCull.h` / `GpuCull.cpp` — caps detect, pipelines, pass
  recording, meshlet builder + self-test

Buffer hazards are manual sync2 buffer barriers inside record callbacks — the
RenderGraph tracks images only (its documented scope); this follows the
header's own guidance.

## Expected wins (set expectations honestly)

- 13700K / 1080 Ti: the big one — HZB removes hidden-prop vertex work +
  overdraw where that GPU is the bottleneck. Largest gains in dense scenes.
- 14900K / 5090: mostly CPU-side relief (cull walk leaves the frame loop);
  Tier 1 hides the dispatch entirely.
- If `objectsDrawn` is currently small (<2–3K), gains may be modest — profile
  before and after with Tracy; the stats buffer makes the cull's work visible.

## Versioning

New engine capability → 0.5.x per VERSIONING.md (or 0.6.0 if D14 scripting
lands first as 0.5.0).
