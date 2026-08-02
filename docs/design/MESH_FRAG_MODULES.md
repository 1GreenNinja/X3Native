# mesh.frag modules — per-concern file ownership for parallel lanes

INTERNAL. Written 2026-08-02 (Lane 2, clustered lighting).

## Why

`shaders/mesh.frag` was a 1180-line single file that **three lanes wanted at once**
(see `docs/design/LANE_DISPATCH_PLAN.md`): Lane 1 reflections, Lane 2 lighting,
Lane 3 shadows — different regions, same file, guaranteed merge conflict in the
most delicate shader in the engine.

The fleet already solves this for `FLEET-STATUS.md` with **per-lane files plus an
assembler**. This applies the same doctrine to the mesh shader: `mesh.frag` is now
a thin **orchestrator** (version/extensions, every `layout` declaration, the
varyings, and `main()`), and each shading concern lives in its own
`shaders/inc/*.glsl`, pulled in with `#include`.

**One owner per module. Two lanes never open the same file.**

## The module map

| File | Contents | Owner |
|---|---|---|
| `shaders/mesh.frag` | version/extensions, ALL `layout` decls, varyings, flag consts, `main()` | orchestrator — coordinate before editing |
| `inc/mesh_reflections.glsl` | `sampleReflGlossy` (roughness-aware reflection tap) | **Lane 1** |
| `inc/mesh_ibl.glsl` | `iblAmbient` — split-sum IBL + the SSR/RT reflection blend | **Lane 1** |
| `inc/mesh_shadows.glsl` | ray-query shadow rays (`rtsh*`) + 3x3 PCF `sampleShadow` | **Lane 3** |
| `inc/mesh_lighting.glsl` | `pointAtten`, the froxel cluster lookup, the point-light loops | **Lane 2** |
| `inc/mesh_material.glsl` | `PI`, `D_GGX`, `V_SmithGGX`, `F_Schlick`, `F_SchlickRoughness` | shared |
| `inc/mesh_brdf.glsl` | `brdf()` + the sun/point diffuse weights | shared |
| `inc/mesh_terrain.glsl` | procedural height/slope terrain splat | terrain lanes |
| `inc/mesh_ddgi.glsl` | DDGI probe-field irradiance sampling | GI lane |
| `inc/mesh_caustics.glsl` | underwater caustics modulation | — |
| `inc/mesh_normalmap.glsl` | `perturbNormal` (derivative TBN) | — |

`shaders/glass.frag` includes `inc/mesh_shadows.glsl` and `inc/mesh_lighting.glsl`.
It used to carry hand-kept COPIES of both; they had already drifted (in comments,
so far). Glass and the opaque path can no longer disagree about the falloff curve,
the shadow bias, or the cluster lookup.

## THE BIT-EXACTNESS RULE — read before you move anything

glslc's default (non-`-g`) output embeds **no source filenames**, so hoisting a
block **verbatim** into an `-I` include produces **byte-identical SPIR-V**.

That is not true if you **reorder**. SPIR-V function/variable order follows source
order, so swapping two independent functions changes the module bytes. Measured:

```
$ glslc c.frag -o c.spv   # helper() then g()
$ glslc d.frag -o d.spv   # g() then helper()
c.spv d.spv differ: char 203
```

**Therefore: each `#include` sits exactly where its block used to sit, and the
blocks stay in their original order.** That is the only reason there are ten small
modules instead of four tidy ones — `brdf()` and the GGX primitives are separated
in the original by ~230 lines of DDGI and IBL, so merging them would have meant
moving code, which would have meant giving up the byte-exact proof.

If you need to reorder, you are no longer doing a no-op refactor. Say so, and fall
back to the rendered-output gate below.

## The gates this split was proved against

Both run from the repo root.

**1. SPIR-V byte-identity** (the strong gate) — all three variants compiled from
`mesh.frag`/`glass.frag`, before vs after:

| artifact | md5 (before == after) |
|---|---|
| `mesh.frag.spv` | `68f00a2b50de907a3ea5161178b31c84` |
| `mesh_rt.frag.spv` (`-DRT_SHADOWS=1`) | `7ff7fcda27b0ad07ccb65e0f0dafd448` |
| `glass.frag.spv` | `0e17dab63cba5f0f8ec8fc01277a1754` |
| `mesh.vert.spv` | `e36c82b9f110635511ac6eea622203b7` |

**2. Rendered-output md5** (the end-to-end gate). Both captures were first shown
to be bit-reproducible across two runs of the SAME binary, then compared across
the refactor:

| capture | md5 (before == after) |
|---|---|
| `--test-primlight` | `13d21a6340405c9f9e0976fed98128c1` |
| `--screenshot-city` (establishing) | `ef8c893efbf82ebc51b745e11e52a839` |
| `--screenshot-city` (street) | `7b6a9681c6b1ea921d35202052ca257a` |
| `--screenshot-city` (scrapyard) | `8ecdfd44fdaff25c58802f69fc63e52f` |

`--test-primlight` also stayed 9/9 including its negative control.

## Build wiring

`app/CMakeLists.txt` passes `-I${CMAKE_SOURCE_DIR}/shaders` to every glslc
invocation, plus `-MD -MF <out>.d` with CMake `DEPFILE` so **editing an include
rebuilds the shaders that use it** (verified: touching `inc/mesh_lighting.glsl`
rebuilds `glass.frag`, `mesh.frag` and `mesh_rt.frag`).

`#extension GL_GOOGLE_include_directive` is deliberately **not** declared — shaderc
resolves `#include` from `-I` without it, and declaring it would emit an
`OpSourceExtension` and break the byte-identity above.
