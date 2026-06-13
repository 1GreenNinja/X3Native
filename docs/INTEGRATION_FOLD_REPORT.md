# THE GREAT FOLD — Integration Report

**Branch:** `integration/empire-fold`
**Base:** `origin/main` @ `d1e8a1b` (untouched; never checked out, never pushed)
**Final pushed commit:** `7b40843`
**Commits ahead of main:** 169 (26 of them `fold(...)` merge/fix commits)
**Author:** Claude Opus 4.8 (1M context) — automated empire-run fold
**Date:** 2026-06-13

This branch folds the ~29 "empire run" branches into one reviewable, **building, gate-green**
integration branch for the Commander (13700K) to later promote to `main`. `main` was NOT touched.
No force-push, no history rewrite, no branch deletion. The LFS/size pre-commit guard was respected
(no new >50MB files, no `.gguf` committed — the gitignore gguf-ban was kept as the stronger superset).

---

## Per-branch merge result

### Group 1 — INFRA / CULL
| Branch | Result |
|---|---|
| `feat/asset-guard` | **Folded via descendant.** Ancestor of `feat/asset-store`; the single asset-store merge contains it. |
| `feat/asset-store` | **CLEAN** (merge `23a2022`). Content-addressed asset store + boot manifest check. |
| `feat/cpu-frustum-cull` | **Folded via descendant.** `git merge-base --is-ancestor` confirms it IS the merge-base of gpu-cull (tip `e2e205e`); merging gpu-cull alone suffices. |
| `feat/gpu-cull` | **CLEAN** (merge `942e83b`). GPU compute frustum cull + HZB + meshlet shaders. Grew `ObjectData` 144→160 B (carried the PBR-map slice). |

### Group 2 — RENDER STACK (merged in energy-conserving order)
| Branch | Result |
|---|---|
| `fix/metal-ambient` [1/7] | **CONFLICTED → resolved** (`7921348`). Additive unions in main.cpp / IRenderDevice.h / VulkanRenderDevice.cpp (kept cull members + `m_metalAmbient`/`setMetalAmbient`). |
| `feat/post-stack` [2/7] | **CONFLICTED → resolved** (`8980e69`). Kept all r_tonemap/r_bloom*/r_autoexposure/r_ae* cvars + `PostFXParams`/`setPostFX` alongside metal-ambient. |
| `feat/reflections` [3/7] | **CONFLICTED → resolved** (`b96e8a2`). `mesh.frag` SsaoControl UBO: kept `ibl.w` (metal) AND added new `refl` vec4 lane (composes into IBL specular, gated by `ssao.refl.x` — **not** naive-additive). 6 VulkanRenderDevice hunks unioned (cull/HZB + TAA-resolve + refl-activation); `sc.ibl/sc.refl` took the reflections side (already carries both) to avoid a duplicate. |
| `feat/taa` [4/7] | **ALREADY CONTAINED.** `taa` is an ancestor of `reflections` (reflections require the TAA history). The [4/7] merge was a no-op (`Already up to date`). |
| `feat/ddgi` [5/7] | **CONFLICTED → resolved** (`0bac2c8`). `mesh.frag` DDGI UBO lanes merged clean (compose into ambient, gated by `ddgiCtrl.x`). VulkanRenderDevice prepareFrameData + recreateSwapchain unioned (DDGI builds on RT-AO). |
| `feat/rt-shadows` [6/7] | **CLEAN** (merge `16c03aa`). |
| `fix/planets-sky` [7/7] | **CLEAN** (merge `4c9589f`). Note: this branch reworked `NightSkyPlanet` (worldPos/radius → azimuth/elevation/angularDiameter); see Build-fix #3 below. |

### Group 3 — CORE SYSTEMS
| Branch | Result |
|---|---|
| `fix/stability` | **CLEAN** (merge `cc2fd11`). Introduced the consolidated `shutdownGameSystems()` teardown helper. |
| `feat/level-loader` | **CONFLICTED → resolved** (`21a1dce`). headless flag unioned `ddgiShot`+`loaderShot`; kept cull seeds + `level_reload` cmd; 2 shutdown sites use `shutdownGameSystems()` + appended `docLevel.shutdown()`. |
| `feat/lua-scripting` | **CONFLICTED → resolved** (`9ac06ed`). D14 Lua system init unioned with CLI seeds. |
| `feat/lua-trigger-binding` | **CONFLICTED → resolved** (`73cd3cd`). Merged the flag comma-list (`testHatch` + level-loader flags). |
| `feat/zero-stutter` | **CONFLICTED → resolved** (`ce74d39`). `testFramePacing` unioned into headless flag; one-time `PacingParams` push kept. |
| `feat/fast-boot` | **CONFLICTED → resolved** (`3b14fcd`). Combined `worldExplicit` + fromdoc-path parse; kept `shutdownGameSystems` + `joinModelPreload`; init kept GPU-cull create + boot marks + onUploadReady; mesh upload kept `computeLocalSphere` then the registry lock. |
| `feat/world-streaming` | **CONFLICTED → resolved** (`59ccdfd`). ⚠️ **Upload-batching divergence:** the SAME subsystem had two implementations. fast-boot's **double-buffered** batch (`m_batchCmds[2]`/`m_batchFences[2]`, async retire) **supersedes** world-streaming's older **single-buffer** (`m_batchCmd`/`m_uploadFence`); took HEAD (fast-boot) for all 6 hunks — the single-buffer path is fully dropped (verified no live refs). main.cpp unioned (this branch also carried mission/dialog content — see Group 4). |
| `feat/vis-unify` | **SKIPPED — see "Skipped branches" below.** |

### Group 4 — GAMEPLAY / CONTENT
| Branch | Result |
|---|---|
| `feat/dialog-runner` | **ALREADY CONTAINED** (via world-streaming's base). No-op. |
| `feat/mission-system` | **ALREADY CONTAINED** (via world-streaming's base). No-op. |
| `feat/llm-npc` | **CONFLICTED → resolved** (`396806b`). Kept the stronger gguf-ban gitignore; unioned Lua+LLM CMake sources/libs; **3-way terminal-submit stitch**: adopted llm-npc's code-vs-freeform routing, but the all-digit branch uses HEAD's richer path (mission `terminal_code` event + `submitTerminalToScripts`), non-digit routes to VIGIL/LLM. |
| `feat/rt-acoustics` | **CONFLICTED → resolved** (`c0db748`). Unioned cvars/flags/IRenderDevice; **dropped rt-acoustics' duplicate `createAudioSystem()`/`init()`** (HEAD's fast-boot audio is move-constructed from `bootAudio`); kept its `RtAcoustics` tracer block; TLAS-want gate now fires for any consumer (`rtao||reflRt||ddgi||rtshadows||audio`). |
| `feat/vehicles` | **CONFLICTED → resolved** (`ddf6b75`). `carShot` unioned into headless flag. |
| `feat/perf-shops` | **CONFLICTED → resolved** (`bcae00f`). ⚠️ perf-shops was itself a **recursive merge commit**, so conflicts arrived with nested embedded markers (`Temporary merge branch 1/2`). Reconstructed the affected main.cpp spans by hand: merged `testVehParts`; unioned `perfshopShot` + `--set cliCVars` loop; **dropped a duplicated `level_reload`** (now appears exactly once). |
| `feat/living-world` | **CONFLICTED → resolved** (`9cddf67`). 6 additive hunks: kept triggers + `doors()` accessor; merged ecology/crowd/alert flags; crowd/alert build + update + alert HUD; screenshot camera made non-const (dialog repositions) with living-world's `alertCam` defaults. |
| `feat/cold-open` | **CONFLICTED → resolved** (`ab77dc9`). `cutsceneShot` injected into headless + ssaa-4x lists. |
| `feat/world-map` | **CONFLICTED → resolved** (`750d475`). Kept `worldExplicit`+`shotWorldMap`; term/LLM Esc path + worldMap-close Esc path; `worldMap.shutdown`. VulkanRenderDevice mesh upload kept BOTH the cull bounding **sphere** AND the world-map **AABB** (`bmin/bmax`) — separate fields, no collision. |

### Group 5 — NARRATIVE DOCS
| Branch | Result |
|---|---|
| `docs/narrative-pack` | **CLEAN** (merge `fa4c18c`). |
| `docs/narrative-spice` | **CLEAN** (merge `a268dc6`). |

---

## Skipped branches

### `feat/vis-unify` — SKIPPED (irreconcilable RT-architecture conflict)
**Why:** vis-unify's merge-base with the fold is `a0a80d2` (the gpu-cull tip) — it was branched
**before** the render stack existed and is 66 commits of divergence. It does two things:
1. A unified visibility policy (`r_vis` cvar + `Visibility.h/.cpp` + `resolveVisPolicy`) — its
   infrastructure actually merges in cleanly and is harmless.
2. A **competing, older RT acceleration-structure design** — an async-armed TLAS rebuild
   (`prepareRtSceneAS`, `m_tlasBuildArm`, `m_rtaoSetTlas[]`, `syncRtaoTlasBinding`) that only
   knows about RT **AO**.

The empire RT stack already folded (reflections + DDGI + rt-shadows + rt-acoustics) is built on
HEAD's **multi-consumer** `buildRtSceneAS()` (one TLAS, four+ consumers). vis-unify's RT changes
are NOT confined to conflict hunks — large portions merge "cleanly" yet reference its own members
and call `prepareRtSceneAS`/`m_tlasBuildArm` from clean-merged graph code, which directly
contradicts `buildRtSceneAS`. Reconciling the two TLAS architectures is deep, high-risk surgery
with a strong chance of silent corruption / VUID errors. Per the doctrine ("a partial fold that
builds beats a complete fold that's broken"), the merge was **aborted** and the branch left out.

**For the Commander:** vis-unify needs a manual, human-gated **rebase onto this folded RT stack**
(re-implement the `r_vis` policy layer on top of `buildRtSceneAS`, discard its async-TLAS rewrite).
Its `Visibility.h/.cpp` policy table is the salvageable part. This is the one feature not in the fold.

---

## ObjectData spare-lane audit (highest-risk per doctrine) — ✅ CLEAN

The per-instance GPU struct `ObjectData` (engine/rhi/VulkanRenderDevice.cpp) is the prime
silent-corruption risk (multiple branches claiming the same padding offset). **No collision found.**

Final layout (160 B, `static_assert(sizeof(ObjectData) == 160)` intact):
```
mat4 model; vec4 baseColorFactor; vec4 emissive;
uint texIndex; uint flags;                 // flags was _pad0 (TERRAIN/GLASS bits)
uint _pad1, _pad2;                          // terrain detail packs
uint normalTexIndex, mrTexIndex, emissiveTexIndex, detailPacked;  // PBR slice (gpu-cull)
vec4 glassParams; vec4 glassTint;           // glass material (distinct vec4s)
```
- The base reserved `_pad1/_pad2` (terrain packs); the PBR slice added **new** fields
  (`normalTexIndex`/`mrTexIndex`/`emissiveTexIndex`/`detailPacked`) — it did NOT re-purpose
  `_pad1/_pad2`. Glass fields are independent vec4s. **No two branches wrote the same offset for
  different purposes.**
- **Shader-side stride verified consistent across all 5 ObjectData definitions** (C++,
  `mesh.vert`, `mesh_probe.vert`, `depth.vert`, `shadow.vert`) — all agree on the 160 B std430
  stride. depth/shadow use 7 generic `_pad` uints (they only read `model`) mapping to the 7 real
  uints. This matches the known depth.vert stride-bug fix (112→128→160).
- world-map's per-mesh AABB (`bmin/bmax`) lives on the CPU-side `Mesh` struct, **not** ObjectData —
  no GPU-row contention with the cull bounding sphere (`boundsCenter/boundsRadius`).

---

## Alert ↔ fast-travel stitch (the one intentional cross-branch semantic stitch) — ✅ DONE

Commit `88c86c6`. Before the fold, `feat/world-map`'s `fastTravelGate()` keyed
`FastTravelGate::Alert` on the `"alert.active"` StoryFlag — a **placeholder hook** (its own comment:
"no alert system here") because the facility AlertSystem lived on `feat/living-world`.

With both folded, the Level-1 sim tick now mirrors the **real** alert level onto that flag,
immediately after `facilityAlert.update()`:
```cpp
if (facilityAlert.level() > 0) chatTrees.flags().set("alert.active");
else                           chatTrees.flags().clear("alert.active");
```
Result: fast travel is **blocked while the facility is alerted** ("CANNOT TRAVEL WHILE HUNTED")
and re-enabled on de-escalation. Surgical — reuses the existing gate + flag rather than changing
`fastTravelGate`'s signature.

---

## Post-merge build fixes (cross-branch issues the textual merge could not detect)

Commit `7b40843` fixed three compile errors:
1. **Duplicate `setMetalAmbient`** virtual (IRenderDevice.h) + override (VulkanRenderDevice.cpp) —
   re-emitted by the post-stack/rt-acoustics unions. Removed the second copy of each.
2. **Duplicate `Level1Game::doors()`** accessor (level1_game.h) — already existed pre-merge; the
   living-world union duplicated it. Removed the duplicate pair.
3. **`NightSkyPlanet` API mismatch** — `fix/planets-sky` reworked the struct (`worldPos/radius` →
   `azimuth/elevation/angularDiameter`, eye-anchored inside `drawNightSkyPlanets`). `feat/cold-open`
   and `feat/vehicles` still used the old fields + the 6-arg call. Converted both call sites
   (cold-open intro, `--screenshot-car`) to set angles and pass the camera eye to the new 8-arg API.

---

## Build + gate results — ✅ GREEN

Environment: Windows 11, **MSVC 14.51 (VS2026 Insiders / "Visual Studio Community 2026")**,
Vulkan SDK 1.4.341, vcpkg baseline `f7f9411`, real GPU present.

| Gate | Result |
|---|---|
| `cmake --preset windows-vs2026` (configure, vcpkg builds deps incl. llama.cpp/ggml) | **exit 0** |
| `cmake --build --preset windows-vs2026` (Release) → `build/bin/Release/X3Engine.exe` | **exit 0** (warnings only: getenv/sscanf/unused-param) |
| `--smoketest` | **exit 0**, boot 3.08 s, **`VMA allocationCount=0`**, 0 VUID |
| 20 focused feature self-tests (see below) | **20/20 PASS** |
| Vulkan validation (VUID) across all tests | **0 errors** |
| `allocationCount` across all tests | **0 (no leaks anywhere)** |

Self-tests run (all PASS): `--test-asset --test-gpucull --test-frustumcull --test-reflections
--test-ddgi --test-rtshadows --test-acoustics --test-llm --test-loader --test-framepacing
--test-mission --test-chattree --test-alert --test-cutscene --test-worldmap --test-boottime
--test-glass --test-jobs --test-console --test-physics`. Notable internal tallies: worldmap 38/38,
alert 7/7.

---

## For the Commander — remaining items
1. **`feat/vis-unify` is the only feature NOT folded.** It needs a human-gated rebase onto this
   folded multi-consumer RT stack (keep the `r_vis`/`Visibility.*` policy table; discard its
   async-TLAS rewrite which conflicts with `buildRtSceneAS`). Details above.
2. **Test coverage is a focused sweep, not exhaustive.** 20 of the ~90 `--test-*` flags were run
   (every empire-run headline feature). The remaining tests (Act-2 content, net, ECS, bestiary,
   etc.) were not run for time — they predate the empire run and are not at conflict risk, but a
   full `--test-*` sweep + the `--screenshot-*` visual captures on the 13700K (GTX 1080 Ti) would
   confirm parity on the target GPU. (This machine has an RT-capable GPU; the 1080 Ti will auto-0
   the RT tiers — verify the raster fallbacks still render correctly.)
3. **No interactive/windowed run** was performed (headless only). Recommend a manual playthrough of
   `--world fromdoc`, the cold-open intro, fast-travel-under-alert, and the VIGIL terminal.
4. Branch is pushed to `origin/integration/empire-fold` @ `7b40843`. `main` is untouched at `d1e8a1b`.
