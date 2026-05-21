# GPL_DEBT.md — RBDOOM-derived code ledger

> **Commercial-ship gate: this ledger must be EMPTY (all rows DONE-CLEAN) and `engine/_gpl_rbdoom/` deleted with the build still green (`USE_GPL_SCAFFOLD=OFF`).**
> Until then: engine stays on the public/GPL branch and is NOT sold. Game data (`.x3pak`) is unaffected and may be commercial at any time.

## Status legend
- `TODO` — still GPL; clean impl not started
- `SPEC` — spec team writing the behavioral spec
- `WIP` — clean-room team implementing from spec
- `VERIFY` — clean impl built; running acceptance tests / interface swap
- `DONE-CLEAN` — clean impl passes; GPL v0 impl removed for this module

## Ledger

| ID | GPL module (from RBDOOM) | Interface | Quarantine path | Spec file | Status | Owner | Notes |
|---|---|---|---|---|---|---|---|
| D1 | Vulkan device + swapchain + cmd buffers | `IRenderDevice` | (clean-from-start — no GPL v0) | `specs/D1-render-device.spec.md` | WIP | 13700K | skeleton landed (instance/surface/device via vk-bootstrap); swapchain+frame+VMA TODO |
| D2 | Material / shader pipeline (PBR) | `IMaterialSystem` | `engine/_gpl_rbdoom/material/` | `specs/D2-material-system.spec.md` | TODO | — | shaders authored fresh GLSL→SPIR-V |
| D3 | Cascaded shadow maps | `IShadowRenderer` | `engine/_gpl_rbdoom/shadow/` | `specs/D3-shadow-renderer.spec.md` | TODO | — | refs: RTR4 ch.7, GPU Gems |
| D4 | Scene submission + culling | `ISceneRenderer` | `engine/_gpl_rbdoom/scene/` | `specs/D4-scene-renderer.spec.md` | TODO | — | optionally GPU-driven |
| D5 | Pak / virtual filesystem | `IAssetSource` | (clean-from-start — no GPL v0) | `specs/D5-asset-source.spec.md` | WIP | 13700K | interface + stub landed; miniz impl TODO |
| D6 | Console + cvar system | `IConsole` | (clean-from-start — no GPL v0) | `specs/D6-console.spec.md` | SPEC | — | spec ready; trivial fresh rewrite, ports Babylon `quality` UX |
| D7 | Math + containers | n/a (header) | `engine/_gpl_rbdoom/math/` | — | TODO | — | swap to glm + STL; mostly delete |
| D8 | Animation (md5/skinning) | `IAnimSystem` | `engine/_gpl_rbdoom/anim/` | `specs/D8-anim-system.spec.md` | TODO | — | targeting glTF skins |

## Audit trail

When a row flips to DONE-CLEAN, record:
- commit SHA of the clean impl
- machine it was authored on (should be 13700K / cleanroom — NOT a machine with the GPL checkout)
- acceptance-test result link

| Date | ID | Event | SHA | Machine |
|---|---|---|---|---|
| 2026-05-20 | — | Ledger created (scaffold) | — | I9DevPC |
| 2026-05-20 | D1,D5 | Buildable skeleton + interfaces seeded (clean-from-start) | (this push) | I9DevPC |

> Note: D1 + D5 are being built **clean from the start** (no GPL v0 stage) — standard permissive-lib wrappers spec'd from public knowledge. They go WIP → DONE-CLEAN directly. The GPL scaffold path (`engine/_gpl_rbdoom/`) applies mainly to the bespoke renderer internals D2-D4.

## Non-module GPL touchpoints to also clear before ship
- [ ] Any RBDOOM headers `#include`d outside the quarantine dir
- [ ] Any RBDOOM assets (textures, shaders, sounds, fonts) shipped in `base.x3pak`
- [ ] Any RBDOOM strings/IDs in cvar names or console output
- [ ] License headers: confirm no GPL headers remain in clean files
- [ ] `THIRDPARTY_LICENSES.md` lists every permissive lib + its license
