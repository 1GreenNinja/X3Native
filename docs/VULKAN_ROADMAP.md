# X3Native — Newer-Vulkan Evaluation

**Status:** decision document. Authored 2026-08-04 against `origin/main` @ `21894259`.
**Scope:** what newer Vulkan (1.4, Roadmap 2024/2026, and the post-1.3 extension set) offers *this* renderer, grounded in this repo's code and this repo's measurements.
**Verdict summary: three things are worth doing. Five are not. None of the three are new Vulkan features.**

---

## 0. THE PASCAL CONSTRAINT — read before anything else

The fleet's declared minimum-spec floor is the **GTX 1080 Ti (Pascal)**, on four machines
(`docs/BLENDER_FLEET.md:20-24`), and it is a *locked architectural decision*, not an accident:
**D-RT** in `docs/IDTECH8_ROADMAP.md:3` — "GPU-driven raster path is the foundation (runs
everywhere incl. 1080 Ti) … RT becomes a high-end *tier*, not the foundation." The RT stack
already honours this via `m_rtSupported` feature detection with a full raster fallback
(`engine/rhi/VulkanRenderDevice.cpp:118-160`). **That pattern is the precedent every proposal
in this document must follow.**

Two facts sharpen the constraint, and one of them has a clock on it:

1. **Pascal's feature set is now FROZEN.** NVIDIA's **R580** driver branch is the *last* to
   support Maxwell/Pascal/Volta; the R590 series drops them. Whatever a 1080 Ti reports on
   R580 is its **permanent ceiling** — no future driver will add an extension to these boxes.
   So "Pascal will get it eventually" is never a valid argument in this document.
2. **Pascal can never conform to Roadmap 2026.** Roadmap 2026 requires Vulkan 1.4 *plus*
   Roadmap 2024, and mandates `VK_KHR_fragment_shading_rate`, `VK_KHR_cooperative_matrix`,
   `VK_KHR_shader_untyped_pointers`, `maintenance7/8/9`, and `hostImageCopy`. VRS and
   cooperative matrix are Turing+ in hardware. Roadmap 2026 is **not a target for this
   engine** while the Pascal floor holds.

**Rule for every candidate below:** anything that raises `require_api_version` beyond
**1.3** (`engine/rhi/VulkanRenderDevice.cpp:37`), or that needs post-Pascal hardware, is
called out explicitly and must be **opt-in behind feature detection**, exactly as
`m_rtSupported` and `m_rtPosFetch` already are. **No candidate in this document recommends
a baseline bump.**

> **Open governance question this doc does not decide** (it is already logged at
> `docs/BEYOND_IDTECH8.md:238`): *how long do we hold the no-RT-required floor?* With R580
> being terminal, holding it past ~2027 means shipping against a driver branch that receives
> no security or correctness fixes. That is a product call, not a rendering call.

---

## 1. WHERE THIS RENDERER IS ACTUALLY BOUND

The brief warned against repeating the vertex-compression mistake — a real 37% memory win
that produced **zero** frame-time change. Here is why that happened and what it implies.

**The vertex-compression result** (commit `89c4061f`, RTX 5070 Ti, 21,618,714 triangles):

| stride | `r_csm 0` | `r_csm 1` (4 cascades) | mesh VRAM |
|---|---|---|---|
| 32 B | 3.179 ms | 5.817 ms | 3,762,304 B |
| 24 B | 3.177 ms | 5.823 ms | 2,821,728 B (-25%) |
| 20 B | 3.177 ms | 5.829 ms | 2,351,440 B (-37.5%) |

0.2% spread — noise. The author's stated conclusion was "triangle-setup bound, not
vertex-fetch-bandwidth bound." **Treat that as a hypothesis, not a measurement**: there is no
primitive-rate counter and no pipeline-statistics query in this engine to confirm it. What
*was* measured is only the negative: narrowing the stride changed nothing.

### 1.1 The evidence that does exist says CPU, not GPU

Two independent datapoints, both on the RTX 5090:

| Source | CPU | GPU | Read |
|---|---|---|---|
| `docs/ZERO_STUTTER.md:166` — `--test-framepacing`, 553 frames, full RT+DDGI+TAA+post stack | **p50 72.91 ms** | p50 45.05 ms | CPU is **1.62×** GPU; ~28 ms GPU idle at the median |
| `docs/screenshots/gpucull/RESULTS.md` — 108,567 instances, 4 cull paths | **4.12–4.61 ms** | 1.41–2.64 ms | CPU ~flat across all four paths |

And the `gpucull` doc names the cost itself (`RESULTS.md:46-54`):

> "CPU cost is ~flat across paths (4.1–4.6 ms @ 100k) and is dominated by the immediate-mode
> `drawMesh()` submission walk itself, NOT by culling."

The shipping-world hunt agrees. Commit `498b6f62` ("busts the perf myth") **disproved by
experiment** the theory that the residual ~33 ms was district/woodland draw records: gating
off far districts gave 28.5 FPS; skipping the **entire crown** (37 towers, houses, condos,
metro) gave 26.9 FPS — *identical*. The remaining cost is **unattributed**, with four
never-bisected suspects: 72 skinned characters + TLAS refits, island GLB, ocean, post stack.

### 1.2 The measurement gap is the real blocker

**This engine has exactly two GPU timestamps: one at frame start, one at frame end.**
`engine/rhi/vk/vk_resources.cpp:1132` — `qci.queryCount = 2;`. There is **no per-pass GPU
timing anywhere**. The fields exist and are dead: `m_cullCpuMs`, `m_cullGpuMs`, `m_hzbGpuMs`
are declared at `engine/rhi/vk/VulkanRenderDevice_internal.h:3379-3381` with the comment
`(0 = not measured)`, are **never assigned**, and the HUD prints `cull 0.00/0.00` on every
machine. My own smoketest run reproduces it: `pvs 0.02 cull 0.00/0.00 hzb 0.00 ms`.

Every per-pass millisecond figure in this repo is a **whole-frame delta** taken on a rig
built so the pass under test dominates — `docs/screenshots/ddgi/README.md:56-59` says so
outright ("whole-frame timestamp delta 9.66 → 12.10 ms in adjacent runs").

`docs/RENDERING_SPEED.md:62-64` listed "GPU timestamp queries per pass" and Tracy as
**non-negotiable**. Neither shipped. `docs/plans/SESSION_LANES.md:57` re-ordered them for the
33 ms hunt. They were never built.

**Consequence for this document: no GPU-side candidate below can currently be validated on
this codebase.** That is not a reason to adopt speculatively; it is the reason the #1
recommendation in §5 is instrumentation.

### 1.3 There is not one measured 1080 Ti number in this repository

Searched: docs, commit messages, captures. The Pascal floor is referenced only as
*predictions* — "the 1080 Ti box, where vertex work is the bottleneck"
(`gpucull/RESULTS.md:53`) and "may still be real on a bandwidth-starved part (Pascal…);
that is **untested here**" (the vertex-compression commit). The one loose figure
(`FLEET.md:25`, "~680 FPS" on dual-1080-Ti SLI) carries no scene, build, resolution or date
and is not usable as evidence.

**So the hardware constraint that governs this entire document has never been measured.**
That is what the probe shipped on this branch (§6) exists to fix.

---

## 2. QUESTION A — the RT acceleration-structure deprecation. **CLOSED. No action.**

**Khronos, 2026-07-08** ("Vulkan Ray Tracing: Deprecating Host-Side Acceleration Structure
Builds") deprecates the feature flags `accelerationStructureHostCommands` and
`micromapHostCommands`, and these entry points: `vkBuildAccelerationStructuresKHR`,
`vkCopyAccelerationStructureKHR`, `vkCopyAccelerationStructureToMemoryKHR`,
`vkCopyMemoryToAccelerationStructureKHR`, `vkWriteAccelerationStructuresPropertiesKHR`, plus
the five micromap equivalents. Deprecated, **not removed**; validation layers will begin
emitting legacy warnings over time.

**X3Native's skinned-TLAS path does not use host-side builds anywhere.** Verified
exhaustively in `engine/rhi/VulkanRT.h` (the entire AS manager, 779 lines):

| Check | Finding |
|---|---|
| Entry points resolved (`VulkanRT.h:69-73`) | `vkCreateAccelerationStructureKHR`, `vkDestroyAccelerationStructureKHR`, `vkGetAccelerationStructureBuildSizesKHR`, **`vkCmdBuildAccelerationStructuresKHR`**, `vkGetAccelerationStructureDeviceAddressKHR`. **Zero host-command pointers are loaded** — none of the ten deprecated functions appears anywhere in `engine/`. |
| Build-type argument | All three `vkGetAccelerationStructureBuildSizesKHR` calls pass `VK_ACCELERATION_STRUCTURE_BUILD_TYPE_**DEVICE**_KHR` — `VulkanRT.h:154` (static BLAS), `:359` (skinned BLAS), `:515` (TLAS). No `BUILD_TYPE_HOST` in the repo. |
| Feature requested | `VulkanRenderDevice.cpp:132-134` sets **only** `asf.accelerationStructure = VK_TRUE`. `accelerationStructureHostCommands` is never requested. |
| Geometry inputs | Device addresses throughout — `vbAddr`/`ibAddr` (`VulkanRT.h:133,137`), instance buffer (`:503`), scratch (`:183, 342, 388, 546`). Already the "single device-address-based path" the deprecation steers toward. |
| Skinned refit (`ensureSkinnedBlas`, `VulkanRT.h:297-401`) | `MODE_UPDATE` recorded via `m_pfnCmdBuildAS` into the batch command buffer. Device-side. |

`VK_KHR_deferred_host_operations` **is** enabled (`VulkanRenderDevice.cpp:129`), but that is a
required dependency of `VK_KHR_acceleration_structure`, and Khronos states explicitly that
the deprecation "does not affect `VK_KHR_deferred_host_operations`."

**Verdict: NO DEPRECATION CLOCK ON THIS ENGINE.** This item is closed and needs no
roadmap slot. The engine was already on the path Khronos is consolidating toward.

### 2.1 …but the AS path *does* have a real cost, and it is not an API-version problem

While verifying the above I found a genuine per-frame serialization point that no new
Vulkan feature addresses:

- `VulkanRT::buildTlas` ends in `oneTimeSubmit` (`VulkanRT.h:552`), which does
  `vkQueueSubmit2` **+ a blocking `vkWaitForFences(…, UINT64_MAX)`** (`VulkanRT.h:704-708`)
  on the graphics queue.
- `endBlasBatch` (`VulkanRT.h:235-253`) does the same blocking submit+wait.
- With `r_skinnedrt` on and characters moving, `buildRtSceneAS` rebuilds the TLAS **every
  frame** (`vk_passes.cpp:338`, because a refit BLAS moves its bounds), so **both** blocking
  waits are paid every frame.
- The in-code measurement (`vk_passes.cpp:264-267`): "steady state = ~2.0–2.6 ms for 33
  REFITS **incl. the fence wait**."

The team already did the hard correctness work here — the TLAS ring (`kTlasSlots = 3`) killed
the per-frame `vkDeviceWaitIdle`, and `--test-framepacing` proves `tlasSyncWaits` settles at 1
(the boot build). **The remaining cost is the module's own private fence wait, not a device
idle.** Given §1.1 says the frame is CPU-bound, ~2–2.6 ms of blocking CPU wait per frame is a
credible slice of the unattributed ~33 ms and is one of the four never-bisected suspects.
See §5, item 2.

---

## 3. QUESTION B — descriptor heap / descriptor buffer. **SKIP. Lateral move, real cost, zero measurable gain.**

Asked to be skeptical. The audit supports the skeptical answer decisively.

**These extensions optimize exactly what this engine has already eliminated.** Descriptor
buffer/heap target three costs: descriptor-set *allocation*, descriptor *writes*, and
*per-draw binding*. Measured against this codebase:

| Cost they target | X3Native steady-state | Evidence |
|---|---|---|
| Per-draw descriptor binding | **ZERO** | Main color pass binds all **5 sets in ONE call** at `vk_passes.cpp:2482-2483`, *before* the draw loop at `:2484`. Loop body is `BindVertexBuffers` + `BindIndexBuffer` + `DrawIndexedIndirect` only. Shadow (`:2351`), depth-pre (`:2412`), glass (`:38`) all bind once. |
| `vkUpdateDescriptorSets` / frame | **0–7 calls** | 3 for the GI ping-pong (`vk_gi_rt.cpp:1928,1939,1953`, only when `r_gi` on), ≤4 for the TLAS re-point (`vk_passes.cpp:163,169,175` + `vk_gi_rt.cpp:597`), plus one per HUD record. All 47 other sites are boot/resize/toggle-time. |
| `vkAllocateDescriptorSets` / frame | **= HUD record count** (typically <20, capped 256) | One site: `vk_passes.cpp:2563`, from a pool wholesale-reset at `VulkanRenderDevice.cpp:965`. |
| Descriptor-set thrash | **None found** | Architecture is "write once, bind once": pre-baked alternate sets for TAA on/off (`vk_passes.cpp:751`), 4 pre-baked ping-pong sets for the a-trous denoiser (`vk_graph.cpp:864-867`) explicitly to avoid per-frame rewrites. |

`docs/ZERO_STUTTER.md:161` already codifies "ZERO descriptor-pool growth after frame 1" as a
**shipped, tested invariant**. The bindless array is a single 4096-slot
`COMBINED_IMAGE_SAMPLER` binding, `UPDATE_AFTER_BIND | PARTIALLY_BOUND`, allocated as
**exactly one set** (`vk_pipelines.cpp:389-421`), with per-object data in an SSBO indexed by
`gl_InstanceIndex` rather than in descriptors. There are **no push constants at all** in the
main mesh path (`vk_pipelines.cpp:811`).

**What migration would cost:** 38 descriptor set layouts, 26 pools, and 5-set/6-set pipeline
layouts across ~50 pipelines would have to move **together**. `VK_EXT_descriptor_buffer` is
mutually exclusive with the `UPDATE_AFTER_BIND` pool path this engine depends on, so the
bindless write path (`vk_resources.cpp:1498-1515`) and the documented
`vkDeviceWaitIdle`-free texture-streaming guarantee (`vk_resources.cpp:404-410`) would both
need reworking.

**Pascal makes it worse, not neutral.** `VK_EXT_descriptor_buffer` on Pascal is
*known-broken*: it triggers **Xid 69 GPU faults** in VKD3D on Pascal cards from driver
**R535 onward**, and the standard mitigation is to *mask the extension off* on Pascal. Since
R580 is Pascal's terminal branch, that bug is permanent on our floor.

**`VK_EXT_descriptor_heap` is even further out of reach.** Released 2026-01-23 in Vulkan
1.4.340; NVIDIA support begins at **driver 610** — Pascal stops at R580, so these machines
will **never** see it. It also requires `VK_KHR_shader_untyped_pointers`, which NVIDIA's own
write-up calls "new and still maturing across shader languages and toolchains." Khronos
positions it as "a full replacement for [descriptor_buffer] **in newer hardware**," and the
motivating audience in the release coverage is D3D12 translation layers (Proton/VKD3D) —
where per-draw descriptor-table churn is real. **We are not a translation layer, and we do
not have that churn.**

**Verdict: SKIP.** Revisit only if a future feature introduces genuinely dynamic per-draw
descriptor traffic. The only two remaining descriptor-traffic targets — the per-HUD-record
alloc+write loop (`vk_passes.cpp:2555-2581`) and the 3-call GI ping-pong — are **trivially
fixable inside the current API** (pre-baked HUD sets keyed by texture; pre-baked GI ping-pong
pairs, exactly the pattern `m_reflDnSet[4]` already uses). Neither justifies a descriptor
rewrite; both are worth roughly an afternoon if profiling ever implicates them.

---

## 4. CANDIDATE TABLE

Hardware column: **P** = works on Pascal/1080 Ti · **T+** = Turing or newer.

| # | Candidate | Our code that would change | Expected win | HW | Verdict |
|---|---|---|---|---|---|
| A | **RT host-build deprecation** | `engine/rhi/VulkanRT.h` | **N/A — already compliant** | P/T+ | **CLOSED** (§2) |
| B | **`VK_KHR_ray_tracing_position_fetch`** | `VulkanRenderDevice.cpp:142-154` | **Already shipped** — gates DDGI hit normals | T+ (opt-in ✅) | **DONE** |
| C | **`VK_EXT_descriptor_heap` / `_buffer`** | 38 layouts, 26 pools, ~50 PSOs | **ZERO** — optimizes costs we already have at ~0 | heap: driver 610 (never Pascal); buffer: **broken on Pascal** | **SKIP** (§3) |
| D | **Vulkan Video (H.264/265/AV1 decode)** | *nothing* — no decoder exists | **ZERO** — there is no video to decode | — | **SKIP** (§4.1) |
| E | **`VK_KHR_cooperative_matrix`** | *nothing* | **ZERO** — no neural component; denoiser is hand-written a-trous | T+ | **SKIP** (§4.2) |
| F | **Vulkan 1.4 baseline bump** | `VulkanRenderDevice.cpp:37` | **ZERO** — nothing we need is 1.4-only | would break nothing *today*, but buys nothing | **SKIP** (§4.3) |
| G | **Roadmap 2026 as a target** | device init | **ZERO** — mandates VRS + coop-matrix + untyped pointers | Pascal can **never** conform | **SKIP** (§0) |
| H | **`VK_EXT_host_image_copy`** | `vk_resources.cpp:1312` upload path | **Probably zero-to-negative on discrete GPUs** | T+ / 1.4 | **SKIP** (§4.4) |
| I | **`VK_EXT_mesh_shader`** (Tier 2) | `GpuCull.{h,cpp}`, `meshlet.task/.mesh`, all 7 draw loops | **Unproven; measured draw count says wrong lever** | T+ (opt-in) | **ADOPT LATER** (§4.5) |
| J | **`VK_KHR_fragment_shading_rate` (VRS)** | post/forward pass PSOs | Plausible when fragment-bound — **not established** | T+ (opt-in) | **ADOPT LATER** (§4.6) |
| K | **Swapchain: `present_wait2` / `present_id2` / `fifo_latest_ready`** | `vk_targets.cpp:24`, `app_run.cpp:8279` sleep-spin limiter | Better pacing, replaces a spin-loop | T+ mostly (opt-in) | **ADOPT LATER** (§4.7) |
| L | **`VK_KHR_maintenance5/6`** (1.4 core) | scattered ergonomics | ~ZERO perf; small cleanliness | P (likely) | **SKIP for now** |
| — | **Per-pass GPU timestamps** | `vk_resources.cpp:1132`, dead `m_*GpuMs` fields | **Unblocks every other decision** | P/T+ | **ADOPT NOW** (§5.1) |
| — | **Un-block the per-frame AS fence waits** | `VulkanRT.h:235-253, 552` | Up to ~2–2.6 ms CPU/frame in a **CPU-bound** frame | P/T+ | **ADOPT NOW** (§5.2) |
| — | **`DrawIndexedIndirectCount` + shared VB/IB arena** | `vk_passes.cpp` ×7 loops, `vk_resources.cpp` | Attacks the **best-evidenced** cost | **P** (1.2 core) | **ADOPT NEXT** (§5.3) |

---

### 4.1 Vulkan Video — SKIP

**There is no video playback in this engine at all.** No decode library in `vcpkg.json`
(deps are vk-bootstrap, VMA, glfw3, miniz, glm, Jolt, cgltf, draco, stb, miniaudio, lua,
sol2, imgui). No `VK_KHR_video_*` / `VkVideoSessionKHR` / `vkCmdDecodeVideo` in `engine/` or
`app/`. `engine/asset/` has exactly two loaders: glTF and pak. No video asset type exists.

**The cold open is not a video and structurally cannot be one.** It is a data-driven
real-time sequence: `assets/cutscenes/cold_open.cutscene.json` (17.5 KB, format
`x3.cutscene/1`), played by `app/cutscene.cpp` + `app/intro_orchestrator.cpp` (2,999 lines),
which **interleaves playable dogfight windows** into the timeline and **selects the ending
from player performance** (`ShotDown` / `Escaped` / `CapitalKilled` span-branches). A
pre-rendered file cannot express a branch chosen at runtime.

The only video assets on disk are `docs/reference/PortalAnimated.mp4` (10.2 MB, an *offline*
art reference consumed by `tools/make_membrane_flipbook.py`) and a documentation GIF.
`assets/` contains **zero** video files.

The one place decoded footage genuinely ships is the rift-hub membrane flipbook —
~144 frames baked into three atlases, 15.8 MB (`app/rifthub.cpp:975-1010`). Replacing that
with H.264 trades ~10 MB of *disk* for a codec dependency, a decode queue, DPB management and
YCbCr sampler conversion, on a texture that is already resident and free at runtime.
`app/elevator_showcase.cpp:68` reaches the same conclusion in a code comment: *"Real video
decode isn't required."*

**Verdict: SKIP.** Reopen only if pre-rendered branching-free cinematics are ever added.

### 4.2 Cooperative matrix — SKIP

Turing+, and aimed at neural inference (low-precision large matrix multiplies). This engine
has no neural component: the reflection denoiser is a hand-written a-trous filter
(`engine/rhi/ReflDenoise.cpp`), and the DLSS work (`docs/VELOCITY_DLSS_REPORT.md`) goes
through NVIDIA's NGX SDK, which does not consume this extension from our side. **Zero
surface area today.** Reopen only if a first-party neural denoiser/upscaler is ever built.

### 4.3 Vulkan 1.4 baseline bump — SKIP

Vulkan 1.4 promoted `dynamic_rendering_local_read`, `maintenance5/6`, `push_descriptor`,
`map_memory2`, `index_type_uint8`, `line_rasterization`, `host_image_copy`,
`vertex_attribute_divisor`, `shader_expect_assume`, `shader_float_controls2`,
`shader_subgroup_rotate`, and more into core.

**Nothing in that list is something this engine needs and cannot already get.** Specifically:
`push_descriptor` is irrelevant (we bind once per pass, not per draw — §3);
`dynamic_rendering_local_read` targets subpass-style G-buffer merging and we are **Forward+**,
not deferred; `host_image_copy` is separately assessed as negative (§4.4).

Bumping `require_api_version(1,3,0)` → `(1,4,0)` would likely *not* break the fleet today
(Pascal reached Vulkan 1.4 in NVIDIA's R570 branch, and R580 is available), but it **buys
nothing**, narrows the supported driver range for zero return, and forecloses non-NVIDIA
Pascal-era hardware. Adopt individual 1.4 features via `enable_extensions_if_present` if a
concrete need appears; do not move the floor.

### 4.4 `VK_EXT_host_image_copy` — SKIP

Current upload is staging-buffer → `vkCmdCopyBufferToImage` (`vk_resources.cpp:1312`), with an
async boot-time warmup already overlapped (`VulkanRenderDevice.cpp:251`). The extension
removes the staging buffer and lets the CPU write optimal-layout images directly.

**The Khronos proposal itself warns this is often a loss on discrete GPUs**: the CPU-side
swizzle "may indeed be slower than the double-copy through a buffer," and it is "not
generally recommended for applications to perform all image copies through this extension"
without profiling. Its stated sweet spot is embedded/UMA and peak-memory pressure during bulk
init. Our boxes are discrete NVIDIA parts, texture upload is already off the critical path,
and boot time is already tracked and healthy (`docs/BOOT_TIME.md`). **Expected win: zero to
negative.** Turing+/1.4 anyway.

### 4.5 `VK_EXT_mesh_shader` — ADOPT LATER, and measure first

**This is the closest call in the document, and the honest answer is "not yet."**

*What already exists:* a Tier 2 mesh-shader path is designed and partly built.
`engine/rhi/GpuCull.h:17-20` declares it; `shaders/meshlet.task` (92 lines) and
`shaders/meshlet.mesh` (58 lines) exist; the CPU meshlet builder is **written and passing
7/7 self-tests** (`GpuCull.cpp:296-450`, `--test-meshlet`).

*What does not exist:* `meshlet.task` / `meshlet.mesh` appear in **no CMakeLists** — they have
never been compiled. `VK_EXT_mesh_shader` is **probed but never enabled** at device creation
(`GpuCull.cpp:36-41` calls `vkGetPhysicalDeviceFeatures2` only; the extension is not in the
device list). Meshlets are never baked into any asset — `buildMeshlets` is called *only* from
its own self-test. `meshlet.mesh` emits **3 varyings**; `mesh.vert` needs **14 locations**.

*Why not now — the measurement.* I captured the number the repo was missing.
`--smoketest --world canonlevel` on this branch:

```
smoketest: stats draws=232 tris=1438246 objs=502/3587 gpu=1.244 ms
```

**232 draw calls.** Per-mesh instancing already collapses instances into one indirect command
per distinct mesh (`vk_passes.cpp:2110-2120, 2240-2246`), and room PVS does the heavy lifting
(6,242 candidates → 502 drawn). Mesh/meshlet culling pays at 100k+-triangle meshes and
thousands of groups; the measured geometry here is bimodal — a mass of ~12-tri procedural
graybox room shells plus a few hand-authored meshes (one 73,728-tri hero tube). **The
submission profile does not look like the one meshlets fix.**

*Why it is genuinely risky.* The depth pre-pass runs and the color pass then tests
`EQUAL` with depth-write off (`vk_graph.cpp:522-527`). If the prepass rasterizes non-meshlet
geometry and the color pass rasterizes meshlet geometry, tiny FP differences produce
**EQUAL-test holes**. So Tier 2 cannot be adopted incrementally — the prepass, **all four CSM
cascades**, the velocity pass and the 6-face probe pass must convert *together*, or the engine
carries two divergent geometry paths.

*The cheaper alternative that comes first.* Every draw in this engine is
`vkCmdDrawIndexedIndirect` with **`drawCount == 1`** — see the explicit note at
`VulkanRenderDevice.cpp:97` ("multiDrawIndirect not needed: drawCount == 1 per call"). Seven
loops re-record 3 commands per group, and the CSM loop does it **four times**. Consolidating
into a shared vertex/index arena (already proven viable by `createMeshLodChain`,
`vk_resources.cpp:102-111`) would collapse each loop into **one**
`vkCmdDrawIndexedIndirectCount` — attacking the best-evidenced cost (§1.1, the immediate-mode
submission walk), on **Pascal-compatible 1.2-core functionality**, and removing the
`kMaxDrawMeshes = 4096` silent-truncation bug (`vk_passes.cpp:2148`) as a side effect.

**Verdict: ADOPT LATER.** Gate on: (a) per-pass GPU timers landing, (b) §5.3 shipping first,
(c) a level whose draw count is in the thousands, not 232. It must stay **opt-in behind
`m_rtSupported`-style detection** — Pascal has no mesh shaders and never will.

### 4.6 Variable rate shading — ADOPT LATER

Legitimate lever *if* the frame is fragment-bound; the post stack and Forward+ shading are
plausible candidates. But §1.1's evidence points at CPU, and §1.2 means **we cannot currently
tell which pass is fragment-bound.** Turing+; must be opt-in. Revisit after §5.1.

### 4.7 Swapchain / presentation — ADOPT LATER

The frame limiter today is a hand-rolled sleep-then-spin loop
(`app/app_run.cpp:8279-8284`: "Sleep most of the wait, spin the last ~1 ms for accuracy") —
i.e. it *burns a core* to hit `r_maxfps`, on an engine that §1.1 shows is **CPU-bound**.
`VK_KHR_present_wait2` / `present_id2` let the app block on actual present completion instead,
and `VK_KHR_present_mode_fifo_latest_ready` reduces latency without the spin.
`VK_EXT_swapchain_maintenance1` also cleans up the resize path (`vk_targets.cpp:93,210`
currently use `vkDeviceWaitIdle`).

Small, contained, and it touches a real measured cost — but it is presentation-layer polish,
not the ~33 ms, and it needs the capability probe's fleet data first (present_wait's Pascal
status is untested here). Opt-in behind detection.

---

## 5. RECOMMENDED ORDER OF WORK

**None of the top three are new Vulkan features.** That is the honest finding: this renderer's
problems are its own, and newer Vulkan does not address them.

### 5.1 — FIRST: per-pass GPU timestamps *(unblocks everything else)*

Raise `queryCount` from 2 (`vk_resources.cpp:1132`) to a per-pass ring, stamp each
`RenderGraph` pass, and **assign the three dead fields** `m_cullCpuMs` / `m_cullGpuMs` /
`m_hzbGpuMs` (`VulkanRenderDevice_internal.h:3379-3381`) that the HUD already prints as
`0.00`. Surface through the existing `r_speeds` path (`app/hud.cpp:195-220`).

*Why first:* it was promised as non-negotiable in `docs/RENDERING_SPEED.md:62-64`, ordered
again in `docs/plans/SESSION_LANES.md:57`, and never built. Until it lands, **every verdict in
§4 marked ADOPT LATER is unfalsifiable**, and the unattributed ~33 ms cannot be bisected. It
is also fully Pascal-compatible. Cost: contained to the RHI, no shader or content changes.

### 5.2 — SECOND: un-block the per-frame AS fence waits

`VulkanRT::endBlasBatch` and `VulkanRT::buildTlas` each do a blocking
`vkQueueSubmit2` + `vkWaitForFences(UINT64_MAX)` **every frame** when `r_skinnedrt` is on
(`VulkanRT.h:235-253`, `:552`, `:704-708`). In-code measurement: ~2.0–2.6 ms for 33 refits
*including the fence wait*, on a frame that §1.1 shows is CPU-starved by 28 ms.

Record the AS builds into the frame's own command buffer (or a dedicated compute queue —
`m_computeQueue` already exists, `VulkanRenderDevice.cpp:177-186`) and synchronize with a
timeline semaphore instead of a CPU stall. **The correctness groundwork is already done:** the
`kTlasSlots = 3` ring removed the cross-frame WAR hazard, so the remaining wait is
conservatism, not a requirement. This also directly tests one of the four never-bisected
suspects from commit `498b6f62` ("skinned characters + TLAS refits"). Pascal-safe (the whole
path is already RT-gated and skipped there).

### 5.3 — THIRD: `DrawIndexedIndirectCount` + a shared vertex/index arena

Attacks the best-evidenced cost in the repo — the immediate-mode `drawMesh()` submission walk
named at `gpucull/RESULTS.md:46-54`. Collapses seven per-group loops (one ×4 for cascades)
into one indirect call each; removes the `kMaxDrawMeshes = 4096` truncation. **Vulkan 1.2
core, Pascal-compatible** — but note `GpuCull.h:9` currently *asserts* Pascal lacks
`drawIndirectCount` and that assertion has never been checked. **Verify with the probe
(§6) before committing to this.**

### Then, and only with §5.1 data in hand
4. §4.7 swapchain/presentation (small, contained, opt-in)
5. §4.5 mesh shaders **or** §4.6 VRS — whichever the per-pass timers actually implicate
6. Never: §3 descriptor heap/buffer, §4.1 Vulkan Video, §4.2 cooperative matrix,
   §4.3 the 1.4 bump, §4.4 host image copy

---

## 6. WHAT SHIPPED ON THIS BRANCH

One change, deliberately minimal: **a boot-time roadmap capability probe.**

`engine/rhi/VulkanRenderDevice.cpp` — `logRoadmapCaps()`, called once after device creation.
Two `vkEnumerateDeviceExtensionProperties` calls plus `vkGetPhysicalDeviceProperties` against
the already-created device, emitting one log line. **It enables nothing, changes no behavior,
and costs nothing per frame.** The probe itself is core Vulkan 1.0/1.1, so it runs on Pascal.

*Why this and not a feature:* §1.3 — the hardware constraint governing this entire document
has **never been measured**, and §0 — Pascal's feature set is now permanently frozen at R580.
Every ADOPT LATER verdict above is gated on the same unknown. This turns it into one command
per fleet machine.

**Measured on this branch (RTX 5090, `--smoketest`):**

```
[rhi/caps] device Vulkan 1.4.341 | drawIndirectCount=1 meshShader=1 descriptorBuffer=1
descriptorHeap=1 untypedPointers=1 hostImageCopy=1 fragShadingRate=1 coopMatrix=1
videoDecodeH265=1 presentWait=1 swapchainMaint1=1 maintenance5=1 maintenance6=1
rtPositionFetch=1 asHostCommands=1
```

Note the engine still requests only 1.3 — the device reports 1.4.341 and we correctly do not
use it (§4.3).

**Gate results:** Release `--smoketest` exit **0**, `VMA live allocationCount=0`, 30 frames +
swapchain recreate OK, no VUID output. Verified no sibling `X3Engine` process was live before
the run.

### ACTION REQUIRED — run this on a 1080 Ti

The probe's whole purpose is the Pascal row, and I could not produce it (this box is a 5090).
On any 1080 Ti fleet machine (13700K, Snake13700k, DJBOOTH, i5000), build this branch and grep
one line. **Specifically settle `drawIndirectCount`** — §5.3 depends on it and `GpuCull.h:9`
asserts it is absent without evidence.

| capability | 5090 (measured) | 1080 Ti (**unknown — please fill in**) |
|---|---|---|
| API version | 1.4.341 | ? |
| `drawIndirectCount` | 1 | **?** ← blocks §5.3 |
| `meshShader` | 1 | expected 0 |
| `descriptorBuffer` | 1 | expected 1 but **Xid-69 broken** (§3) |
| `descriptorHeap` | 1 | expected 0 (needs driver 610) |
| `fragShadingRate` | 1 | expected 0 |
| `presentWait` | 1 | **?** ← informs §4.7 |

---

## 7. SOURCES

- [Vulkan Ray Tracing: Deprecating Host-Side Acceleration Structure Builds](https://www.khronos.org/blog/vulkan-ray-tracing-deprecating-host-side-acceleration-structure-builds) (Khronos, 2026-07-08)
- [Vulkan Introduces Roadmap 2026 and New Descriptor Heap Extension](https://www.khronos.org/blog/vulkan-introduces-roadmap-2026-and-new-descriptor-heap-extension) (Khronos)
- [VK_EXT_descriptor_heap proposal](https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_descriptor_heap.html)
- [Streamlining Resource Binding with End-to-End Support for Vulkan Descriptor Heaps](https://developer.nvidia.com/blog/streamlining-resource-binding-with-end-to-end-support-for-vulkan-descriptor-heaps/) (NVIDIA — driver 610 requirement)
- [VK_EXT_descriptor_buffer in VKD3D triggers Xid 69 on Pascal cards since driver 535](https://forums.developer.nvidia.com/t/vk-ext-descriptor-buffer-in-vkd3d-triggers-xid-69-on-pascal-cards-since-driver-535-series/291172)
- [Vulkan Roadmap Milestones](https://docs.vulkan.org/spec/latest/appendices/roadmap.html) (Roadmap 2024 / 2026 contents)
- [Vulkan 1.4 release](https://www.khronos.org/news/press/khronos-streamlines-development-and-deployment-of-gpu-accelerated-applications-with-vulkan-1.4)
- [VK_EXT_host_image_copy proposal](https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_host_image_copy.html)
- [NVIDIA confirms R580 is the last driver branch for Maxwell/Pascal/Volta](https://www.gamingonlinux.com/2025/07/nvidia-confirm-upcoming-driver-will-be-the-last-for-maxwell-pascal-and-volta/)
- [VK_KHR_cooperative_matrix proposal](https://docs.vulkan.org/features/latest/features/proposals/VK_KHR_cooperative_matrix.html)
- [Vulkan 1.4.340 Released With Descriptor Heap](https://www.phoronix.com/news/Vulkan-1.4.340-Descriptor-Heap)
