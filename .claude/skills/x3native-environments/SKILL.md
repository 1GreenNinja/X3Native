---
name: x3native-environments
description: Build REALISTIC environments/cities in the NATIVE X3Native engine (C++20 / Vulkan 1.3, GLB assets) from Unity asset packs on G:\Assets — Cyberpunk-2077 / GTA quality, NOT a grey graybox grid. The native counterpart to the UE-targeted `realistic-environments` skill. Use whenever dressing a district/city/interior/world in X3Native from imported FBX packs and it must look curated + properly textured + neon-lit. Triggers on: "make the metropolis look like CP2077", "use the textures / the demo scene / the Cyberpunk pack", building a CityArt / env_art overlay, importing FBX packs into the native engine, "not square blocks", neon-sprawl / Act-2 city / Keth'zar surface art passes.
---

# Realistic environments in X3Native (native Vulkan + GLB)

X3Native is **not Unreal** — there are no UE materials, `MaterialEditingLibrary`, or `ExponentialHeightFog`. The realism principles are identical to the UE `realistic-environments` skill, but the *mechanism* is the engine's own GLB + PBR + `env_art` overlay pipeline. Two failure modes to avoid — BOTH:

1. **Grey / untextured meshes** — converting an FBX without carrying its texture maps gives flat grey. You MUST bring BaseColor/Normal/Metallic/Roughness/AO/**Emissive** through into the GLB's glTF-PBR material.
2. **Procedural square-block grids** — random boxes on a uniform lattice reads fake. Real cities (and the pack demo scenes) are *curated*: varied massing, hero landmarks, alleys, density gradients, broken alignment.

## The native pipeline (4 stages)

### 1 — Mine the pack DEMO SCENE → a placement manifest (ground truth)
Every quality Unity pack on `G:\Assets` ships a demo/showcase the artist arranged with correct composition. Treat it as the reference; don't reinvent.
- Find it: `Demo*/`, `Showcase*/`, `Scenes/`, `_Layouts/` + the `.unity` scene + `.prefab` files (e.g. the Cyberpunk pack: 2 `.unity` + 331 `.prefab` under `_Layouts\`).
- Unity `.unity`/`.prefab` are **YAML** → parse to JSON. For each `GameObject`: read its `Transform` (localPosition/Rotation/Scale) + the mesh ref, resolve the mesh via the `.meta` **GUID → FBX** map, compose nested prefab/child transforms into a world transform.
- **Axis fix:** Unity is Y-up **left-handed**, X3Native is Y-up **right-handed** (`docs/CONVENTIONS.md`: +X right, +Y up, −Z forward); 1 unit = 1 m. Negate Z on positions + fix rotation handedness consistently (verify against a known landmark in the demo).
- Emit a **placement manifest** (JSON / `.x3lvl`): `[{ glb, pos[3], rotQuat[4], scale[3] }]` — the designer's exact city.

### 2 — Convert FBX → GLB **with textures** (the realism unlock) — use `tools/convert_unity_pack.py`
THE cracked pipeline (2026-05-30; works on ASCII packs Blender rejects). Output to `convertedGlbRoot()` (`app/asset_root.h`) = `assets/converted_glb/<Pack>/`.
- **Don't import via Blender if the pack ships ASCII FBX** (header `; FBX 7.4.0`): Blender hard-errors *"ASCII FBX files are not supported"*. (Also: the MS-Store Blender `blender.exe` is ACL-denied — only `blender-launcher.exe` runs, and it DETACHES, so any Blender script must write a `.done`/`.log` marker to be observable, e.g. `tools/convert_pack_glb.py`.)
- **Use `C:\GameDev\tools\FBX2glTF.exe`** (Autodesk FBX SDK; reads ASCII; normal exe w/ stdout): `FBX2glTF -b -i <in.fbx> -o <out.glb>` → geometry + NAMED material slots. But Unity strips texture paths from the FBX → this alone is GREY.
- **Resolve textures from Unity**: each `Meshes/Materials/*.mat` lists slots with texture GUIDs (`_MainTex`=albedo, `_BumpMap`=normal, `_MetallicGlossMap`=metallic(R)+smoothness(A), `_OcclusionMap`=AO, `_EmissionMap`=emissive); each `Textures/<file>.png.meta` holds that file's GUID. Build GUID→file from all `.meta`, resolve per-material, **repack `_MetallicGlossMap` (Unity R=metallic/A=smoothness → glTF metallicRoughness B=metallic / G=roughness=1−smoothness)**, `.tif`→png, inject per material-name. `tools/convert_unity_pack.py <packAssetsDir> <fbx|all> <outDir>` does all of this.
- Deps: Python + `pip install Pillow numpy pygltflib`. **pygltflib 1.16.5 can't pack images into bufferViews** → it embeds as DATAURI (base64 in the JSON chunk; cgltf reads it). **SIZE CAVEAT**: shared 4K atlases embedded PER-MESH balloon GLBs (a mesh using ~4 atlases ≈ 100+ MB). `MAX_TEX` caps atlas dims (default 512). Production: prefer **shared external textures** (one atlas copy via `.gltf`) or convert only the meshes the scene uses.
- **PBR shading is REQUIRED to see the maps**: `shaders/mesh.frag` shades baseColor+emissive ONLY by default — normal/metallic/roughness need the PBR pass (slice 1 = `drawMeshPBR` plumbing, commit `bb169c9`; slice 2 = `mesh.frag` normal-map + GGX). Without it, converted modules render textured-but-flat.

### 3 — CityArt OVERLAY (`app/city_art.*`, mirror `app/env_art.{h,cpp}`)
The graybox (`city.cpp` / `world_regions.cpp` / `level1.cpp`) stays as **collision + per-piece fallback**; the art is a *visual overlay drawn on top* (exactly how `env_art` drapes the ModularSciFi kit over Level 1).
- Load the converted GLBs (the `env_art` IAssetSource + IModelLoader pattern) and place instances **at the manifest transforms (stage 1)**, anchored onto the graybox region (read `City::CityZonePlan` center/footprint — do NOT edit `city.*`).
- **Drive `Entity::emissive` HARD** on neon signage/windows/strips: `emissive = {r,g,b, strength}` with strength > 1 so they bloom — the engine has a real bloom chain (`IRenderDevice` bloom), and **emissive neon is THE cyberpunk look** in this engine.
- Per-piece fallback: a missing GLB → the graybox stays visible (level never breaks).

### 4 — Curate the layout + neon-noir lighting
- The stage-1 manifest gives curation for free (it's the artist's arrangement). When procedurally filling gaps: jitter, rotate off-axis, vary height (hero towers vs low shops), carve alleys — **never a uniform lattice of one merged box**.
- Detail layers that read as "real": props (AC units, pipes, dumpsters, cables, market stalls), emissive billboards, parked cyber-cars (the Modular Cyber Racing Cars pack), wet/road-marked streets (Road System pack).
- **Lighting (native):** tune the analytic **sky** dark/violet (`IRenderDevice::SkyParams`), place **Forward+ point lights** at the neon practicals (cool magenta/cyan/amber), rely on the **bloom** chain for glow + **SSGI/SSAO** for grounding; if water is in frame, use it for wet-street reflection.

## Gate (every environment lane)
Headless `--test-<name>` (assert: converted GLBs load — or the graybox-fallback path is clean when absent — and instances place at the manifest transforms; report assetsLoaded / instanceCount). Then Release + Debug `--smoketest` = **0 VUID + `allocationCount=0`**. Note: GPU material/texture verification runs in the normal windowed path; the headless test asserts placement/counts, not pixels.

## Clean-room
The Unity packs on `G:\Assets` are Tim's **licensed** Asset-Store content — fine to convert + ship (like the ModularSciFi kit `env_art` already uses). NEVER read RBDOOM / id Tech / Doom / Quake or any other third-party **engine** source.

## Key references
`app/env_art.{h,cpp}` (overlay pattern) · `app/asset_root.h` (`convertedGlbRoot`) · `tools/convert_fbx_glb.py` (FBX→GLB) · `app/city.h` / `world_regions.h` (graybox + queries) · `engine/rhi/IRenderDevice.h` (`SkyParams`, bloom, `createMesh`/`createTexture`) · `app/scene.h` (`Entity::emissive`) · `docs/CONVENTIONS.md` (axes) · `docs/design/X3_WORLD_BLUEPRINT.md`. Companion (UE side): `realistic-environments`.
