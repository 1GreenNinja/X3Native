# Plan Audit: X3_NATIVE_SLICES.md
**Verdict: GO**
Date: 2026-06-05  ·  Plan: `G:\X3Native\X3_NATIVE_SLICES.md`

## Verdict rationale
This is a forward-looking 100-slice backlog, so pending work is expected and correct. The plan's foundation claims (clean-room render device, pak/VFS, console, glTF/PBR, Jolt physics + CharacterVirtual, navigation, miniaudio) all verify against real source — the engine is far more built-out than the milestone numbering implies. The only implementation-blocking class of defect — instructions that reference forbidden clean-room (RBDOOM/id Tech) source as if it were the substrate — appeared in three *active* feature slices (23, 80, 91). All three were provably wrong (the engine has its own PBR, bloom, and audio; there is no RBDOOM/id sound to map-to or replace) and have been auto-fixed in place with source evidence. The remaining RBDOOM language is confined to the correction box and the explicitly-OBSOLETE / "Not applicable" M0 section (Slices 1–8), which is fenced as historical and should not be acted on; purging vs. keeping it as history is a design judgment, so it is flagged, not edited. No remaining High blocks implementation → GO.

## Auto-fixes applied (3)
- [Slice 23 — PBR metallic-roughness materials from glTF] Removed "RBDOOM's PBR" (forbidden-source reference; no RBDOOM in repo)
  - Was: `**Goal**: Map glTF PBR (baseColor/metallic/roughness/normal/emissive/occlusion) to RBDOOM's PBR.`
  - Now: `**Goal**: Map glTF PBR (...) to the engine's own clean-room PBR (shaders/mesh.frag; importer in engine/asset/ModelLoader.cpp). *(Note: much of this is already implemented — see audit.)*`
  - Evidence: `shaders/mesh.frag:79-82` (bindless PBR map indices, metallic-roughness); `engine/asset/ModelLoader.cpp:387-393` (cgltf metallic-roughness/baseColor/normal/emissive/occlusion import); `PROVENANCE.md:52` ("glTF/GLB loader + PBR ... DONE; cgltf-based; metallic-roughness PBR import"). No RBDOOM source exists in the repo.

- [Slice 80 — Bloom tuning + lens dirt] Removed "RBDOOM's bloom" (forbidden-source reference)
  - Was: `**Goal**: Tune RBDOOM's bloom; optional lens-dirt overlay.`
  - Now: `**Goal**: Tune the engine's own bloom (shaders/bloom_down.frag / shaders/bloom_up.frag); optional lens-dirt overlay.`
  - Evidence: `shaders/bloom_down.frag`, `shaders/bloom_up.frag` exist (engine's own bloom pyramid). No RBDOOM bloom in repo.

- [Slice 91 — miniaudio integration] Removed "Replace id sound" (assumes a nonexistent id-derived audio subsystem)
  - Was: `**Goal**: Replace id sound with miniaudio; play a 2D sound.`
  - Now: `**Goal**: Add the miniaudio audio backend (clean-room; there is no prior "id sound" to replace); play a 2D sound. *(Note: engine/audio/MiniaudioSystem.cpp already exists — IN PROGRESS per PROVENANCE.)*`
  - Evidence: `engine/audio/MiniaudioSystem.cpp:1-5` ("Clean-room ... No RBDOOM / id Tech source."); grep for `idSound`/`id sound`/`snd_` across engine/ + app/ returns no id-sound subsystem; `PROVENANCE.md:57` (Audio = miniaudio, IN PROGRESS).

## Remaining High findings (block implementation) (0)
None. (See Medium #1 for the OBSOLETE M0 RBDOOM section — flagged, not blocking, because it is explicitly fenced as "Not applicable / historical context".)

## Medium findings (5)
- OBSOLETE M0 still contains live-looking forbidden-source instructions — Slices 1–8 (lines 35–69). Goals like "Find the RBDOOM source tree on the 14900K", "Generate a VS2022 solution from RBDOOM's CMake", "RBDoom3BFG.exe compiles", "the existing id physics", "`.pk4` mounting". Neutralized by the correction box (lines 6–14) + the "OBSOLETE / Not applicable" banner (lines 29–33), so not High — but a skimming agent could mistake a slice for live. Fix (human design call): either delete Slices 1–8 outright or collapse them to a one-line "see PROVENANCE.md" pointer. — Evidence: `PROVENANCE.md:25-28` (no RBDOOM/id source ever read), `PROVENANCE.md:77`.
- Cross-cutting "Profiler-first: Tracy from Slice 8" depends on an OBSOLETE slice — line 495 (and Slice 8, line 66) assert Tracy as an in-place foundation, but Tracy is NOT integrated anywhere and Slice 8 sits under OBSOLETE M0. — Fix: add a real, non-obsolete "integrate Tracy" slice or downgrade the "every perf claim is measured" guarantee until it exists. — Evidence: grep for `Tracy`/`FrameMark` across engine/, app/, CMakeLists.txt, vcpkg.json, third_party/ returns nothing.
- Slice 57 names "Recast/Detour" for the navmesh, conflicting with the shipped clean-room nav — the engine deliberately built a grid+A* navigator with "no Recast source read." Adopting Recast/Detour now is a real design decision (and a clean-room/licensing consideration), not a drop-in. — Fix: decide keep-clean-room-nav vs. add-Recast; if keeping, reword Slice 57. — Evidence: `engine/ai/Navigation.cpp:1-7` ("built from public pathfinding refs ... No third-party nav source read"); `PROVENANCE.md:56` ("nav grid + 8-connected A* + string-pull ... no Recast source; DONE").
- Status drift: several slices marked planned are already DONE — e.g. Slice 23 PBR import (ModelLoader.cpp:387-393), Slice 35 CharacterVirtual (`engine/physics/JoltPhysicsWorld.cpp:271,594,604`), Slice 65/67 `.x3pak` mount + priority override with acceptance tests (`engine/asset/PakAssetSource.cpp:55,186-211`), Slices 9–15 Vulkan 1.3 / dynamic rendering / sync2 / descriptor-indexing / timeline-semaphores (`engine/rhi/VulkanRenderDevice.cpp:150-155,261,1699`). The plan has no ✅/🚧 status markers despite defining the legend (line 18). — Fix: pass the legend over the slices so agents don't re-implement done work. — Evidence: cited file:line above.
- Slice 21 "Files: renderer/gltf/" path does not match reality — the glTF loader lives at `engine/asset/ModelLoader.cpp` / `engine/asset/IModelLoader.h`, not `renderer/gltf/`. (Forward "Files:" hints throughout the plan are aspirational; this one is contradicted by an already-shipped file.) — Evidence: `engine/asset/ModelLoader.cpp:1`, `PROVENANCE.md:52`. No `renderer/` dir exists at repo root.

## Low findings (3)
- `+game` launch flag (Slice 68) is currently a stub — `app/main.cpp:9641` mounts `base.x3pak` with "stub: logs not-implemented for now". Consistent with the slice being planned; noted so it isn't assumed working.
- Exe-name consistency confirmed (not a defect): plan's `X3Engine.exe` (M6/M10) matches `app/CMakeLists.txt:1` `add_executable(X3Engine ...)`; project is `X3Native` (`CMakeLists.txt:8`).
- "Babylon X3" references (Slices 36/60/79/82, cross-cutting) are the author's own prior project used as a tuning/feel reference — legitimate, not forbidden source; no action.

## Coverage
Claims checked: ~24  ·  Verified against source: 24  ·  Files inspected: `X3_NATIVE_SLICES.md`, `PROVENANCE.md`, `engine/rhi/VulkanRenderDevice.cpp`, `engine/asset/ModelLoader.cpp`, `engine/asset/PakAssetSource.cpp`, `engine/asset/IModelLoader.h`, `engine/physics/JoltPhysicsWorld.cpp`, `engine/audio/MiniaudioSystem.cpp`, `engine/ai/Navigation.cpp`, `shaders/mesh.frag`, `shaders/bloom_down.frag`, `shaders/bloom_up.frag`, `app/main.cpp`, `app/CMakeLists.txt`, `engine/CMakeLists.txt`, `CMakeLists.txt`, `vcpkg.json`, `tools/` listing.
