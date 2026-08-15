# PROVENANCE.md — Originality Record

> **This file replaces the former `GPL_DEBT.md`.** There is no GPL debt: no
> RBDOOM / id Tech / Doom 3 — or any other third-party engine — code was ever
> forked, copied, or consulted. This document is the affirmative record of how
> X3Native was created, kept as evidence of independent creation.

## Statement of originality

X3Native (`engine/`, `app/`, `shaders/`) is **original work**, written
**clean-room from scratch**. Each subsystem was implemented from:

1. **Behavioral specs** in `specs/` — interface contracts, observable behavior,
   inputs/outputs, edge cases, performance targets, and acceptance tests,
   authored as original prose (no transcribed third-party source).
2. **Public technical references** — the Vulkan 1.3 specification, *Real-Time
   Rendering 4th ed.*, GPU Gems, vkguide.dev, the glTF 2.0 spec, public GDC /
   SIGGRAPH talks (e.g. Tiago Sousa's 2025 "Fast as Hell" GI talk), and the
   documentation/samples of the permissive libraries used.
3. **The author's own research and design notes** — including ideas worked out
   via search and AI assistants. Conceptual hints and algorithms are not
   copyrightable; only the original expression in this repo is, and it is the
   author's.

No proprietary or copyleft engine source — RBDOOM, id Tech (any version), Doom 3
BFG, or otherwise — was forked, copied, transcribed, or read while writing this
code. Many source files carry an explicit in-file note to that effect (e.g.
`// No GPL / id Tech / RBDOOM source consulted.`).

## Third-party code

The only non-original code is the set of **permissively-licensed libraries**
listed in `THIRDPARTY_LICENSES.md` (MIT / Apache 2.0 / zlib / BSD /
public-domain). No GPL/LGPL/CC-BY-SA dependencies exist or are permitted.

## How it was built (machines)

- **13700K** (i7-13700K, 2x GTX 1080 Ti, 128GB DDR5) — primary clean-room build
  machine; wrote the engine from specs + public references.
- **A2000 laptop** — early verification (validation-clean render device).
- **14900K** (RTX 5090) — high-end verification + RT/PT tier work.

No machine used a third-party engine checkout as a source for this code.

## Subsystem record (original implementations)

| Subsystem | Interface | Status | Notes |
|---|---|---|---|
| Render device (Vulkan 1.3) | `IRenderDevice` | DONE | vk-bootstrap + VMA; dynamic rendering, bindless textures, multidraw-indirect; validation-clean |
| Pak / virtual filesystem | `IAssetSource` | DONE | miniz zip mount + priority override + zip-slip reject; 7/7 acceptance tests |
| Console + cvars | `IConsole` | DONE | tokenizer, commands, typed cvars, save/load; 8/8 acceptance tests |
| glTF/GLB loader + PBR | `IModelLoader` | DONE | cgltf-based; metallic-roughness PBR import |
| Physics world + controller | `IPhysicsWorld` | DONE | Jolt backend + character controller |
| Forward+ lighting | (in `rhi`) | DONE | 16 point lights + hemispheric ambient + ACES tonemap |
| Skeletal animation | (in `app`/`engine`) | DONE | CPU skinning, glTF skins, idle clip |
| Navigation + pathfinding | `INavigation` | DONE | nav grid (physics-sampled walkability) + 8-connected A* + string-pull + path-follow; from public A*/navmesh refs (no Recast source); 5/5 `--test-nav` |
| Audio | `IAudioSystem` | IN PROGRESS | miniaudio backend |
| Material/shader pipeline | `IMaterialSystem` | PLANNED | original GLSL→SPIR-V |
| Cascaded shadow maps | `IShadowRenderer` | PLANNED | from RTR4 / GPU Gems |
| GPU-driven scene/culling | `ISceneRenderer` | PLANNED | compute frustum culling |

## Build hygiene

- All third-party code arrives via vcpkg (`vcpkg.json`) or vendored permissive
  single-headers; licenses tracked in `THIRDPARTY_LICENSES.md`.
- New dependencies must be permissive (see the rule in `THIRDPARTY_LICENSES.md`).
- Keep the in-file "no foreign engine source consulted" notes when adding new
  modules — they are part of the provenance record.

## History

| Date | Event |
|---|---|
| 2026-05-19 | Engine architecture decided; stack locked (C++20 / Vulkan 1.3 / Jolt / Lua / miniaudio) |
| 2026-05-20 | Repo seeded with planning + specs |
| 2026-05-20 → | Clean-room engine built from scratch on the 13700K (render device, pak/VFS, console, glTF, physics, lighting, animation, Level 1 graybox) |
| 2026-08-15 | Thunder audio (`assets/audio/weather/thunder_{crack,roll}.wav`) sourced from **UniStorm Weather System**, a Unity Asset Store product licensed to Tim (on `Z:`). Two of its six samples, downmixed to mono; chosen by MEASUREMENT (attack time + zero-crossing brightness), not by filename. Third-party ART under an Asset Store license - not engine code, and not clean-room. If the project ever needs an unencumbered set, these are the two slots to re-record. |
| 2026-05-21 | Docs corrected: the engine was built clean-room from scratch; the earlier "fork RBDOOM then de-GPL" plan was **never executed**. `GPL_DEBT.md` retired in favor of this originality record. |
