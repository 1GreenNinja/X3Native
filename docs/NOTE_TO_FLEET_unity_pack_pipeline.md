# FLEET NOTE — Unity asset pack → textured GLB pipeline (CRACKED 2026-05-30)

**TL;DR:** We can now reliably convert **any Unity Asset-Store pack** (meshes + PBR
textures) into engine-ready **textured GLBs** — even the ASCII-FBX packs Blender
refuses. New tool: `tools/convert_unity_pack.py`; the method is baked into the
`x3native-environments` skill. This unblocks every "use the REAL pack, not graybox"
art pass across all our projects (X3Native showroom/world, Riftforged, etc.).
Committed **`f1933f3`** on `feat/doors-death-anim`.

---

## Why this matters
Every quality Unity pack we own ships beautiful PBR meshes + a demo scene, but two
walls block getting them into the native engine:

1. **ASCII FBX.** Many packs export ASCII FBX (header `; FBX 7.4.0 project file`).
   **Blender's importer hard-errors:** `RuntimeError: ASCII FBX files are not supported`.
   Our old `tools/convert_fbx_glb.py` (Blender) silently failed on these (the
   MS-Store Blender launcher detaches, so the error was invisible — see Gotchas).
2. **Textures live in Unity, not the FBX.** Unity strips texture paths from the FBX;
   the PBR maps are wired in the `.mat` materials, which reference the `.png`/`.tif`
   files by **GUID**. So even a clean geometry convert renders **GREY**.

Both are now solved.

---

## The tools
- **`C:\GameDev\tools\FBX2glTF.exe`** — Autodesk-FBX-SDK converter. Reads **ASCII** FBX
  (Blender can't), emits geometry + **named** material slots, and is a normal exe (gives
  stdout). `FBX2glTF.exe -b -i <in.fbx> -o <out.glb>` (`-b` = binary GLB).
- **`tools/convert_unity_pack.py`** — THE pipeline: FBX2glTF for geometry, then resolves
  + injects the textures from the Unity side, writes a self-contained GLB.
- **`tools/convert_pack_glb.py`** — a logging Blender wrapper (writes `<dst>.done` +
  `<dst>.log`) for the rare **binary**-FBX pack, since the MS-Store Blender launcher
  detaches and eats stdout. (FBX2glTF avoids this entirely.)

## Usage
```powershell
# one-time deps (Python 3.14 at C:\Python314):
C:\Python314\python.exe -m pip install Pillow numpy pygltflib

# convert one mesh (or "all") into the engine's converted_glb root:
C:\Python314\python.exe tools\convert_unity_pack.py `
  "<UnityProject>\Assets\<Pack>" `    # folder containing Meshes\, Textures\, Meshes\Materials\
  all `                               # or a single "Foo.FBX" (matched recursively under Meshes\)
  "assets\converted_glb\<Pack>"       # output dir (== convertedGlbRoot()/<Pack>)
```
Load the GLBs via the `env_art` IAssetSource + IModelLoader pattern (the engine mounts
`convertedGlbRoot()` from `app/asset_root.h`).

## The method (so you can extend / debug it)
1. **Geometry:** `FBX2glTF -b -i fbx -o tmp.glb` → glTF whose material names match the
   Unity `.mat` names.
2. **GUID → file:** scan every `Textures/*.meta` for its `guid:` line → map to the
   sibling texture file.
3. **Material → slots:** parse every `Meshes/Materials/*.mat` (Unity YAML) for the
   texture-env slots → GUIDs:
   - `_MainTex` → baseColor (sRGB) · `_BumpMap` → normal · `_OcclusionMap` → occlusion (AO) ·
     `_EmissionMap` → emissive
   - `_MetallicGlossMap` → **Unity packs metallic in R, smoothness in A**
4. **Repack metallic/roughness:** glTF wants metallic in **B**, roughness in **G**, where
   **roughness = 1 − (smoothness × `_GlossMapScale`)**. (numpy + Pillow.)
5. **Inject** per material-name into the FBX2glTF glTF + set factors; embed images; save a
   self-contained GLB.

---

## Gotchas (hard-won — read before you burn an hour)
- **ASCII FBX → use FBX2glTF, NOT Blender.** Blender's FBX importer only reads binary FBX.
- **MS-Store Blender is ACL-locked.** `...\WindowsApps\...\blender.exe` is **Access-denied**.
  Only `C:\Users\Tim\AppData\Local\Microsoft\WindowsApps\blender-launcher.exe` runs, and it
  **DETACHES** (returns instantly, no stdout). Any Blender script must write a `.done`/`.log`
  marker the host polls (pattern: `tools/anim_build.ps1`, `tools/convert_pack_glb.py`).
- **pygltflib 1.16.5 cannot pack images into bufferViews** (`"unable to add image data to
  buffers"`). We embed via **DATAURI** (base64 in the JSON chunk) — still a valid GLB; cgltf
  decodes it.
- **SIZE.** Packs share a few big atlases (often 4K). Embedding them **per-mesh** balloons
  GLBs (a mesh using ~4 atlases ≈ 100+ MB). `MAX_TEX` caps atlas dims (default **512**, fine
  for graybox). For production prefer **SHARED EXTERNAL textures** (one atlas copy referenced
  by a `.gltf`) or convert only the meshes the scene actually uses. **Don't commit the giant
  GLBs.**
- **PBR SHADING is REQUIRED to SEE the maps.** `shaders/mesh.frag` shades **baseColor +
  emissive only** by default — normal/metallic/roughness are *loaded but ignored*. The PBR
  pass: **slice 1 = `drawMeshPBR` plumbing (commit `bb169c9`)**; **slice 2 = `mesh.frag`
  normal-map + GGX (+ `camPos` in the Camera UBO) — STILL PENDING.** Until slice 2 lands,
  converted modules render **textured-but-flat** (still far better than graybox, not yet "demo").

---

## Status / first target
- **Verified:** resolves + embeds maps correctly (Showroom kit `Cache_02` → 4 materials,
  20 texture assigns; img0 = `data:image/png;base64,…`).
- **First use:** X3Native **`--world showroom`** from "3D Showroom Level Kit Vol 30" (Creepy
  Cat), imported to `C:\GameDev\UnityEscLabZero\Assets\Creepy_Cat\ShowRoom_Vol 30`
  (`Example_01.unity` is the demo scene to mine). Remaining: batch-convert the used meshes →
  mine the scene → `--world showroom` → PBR slice 2.

## Skill
Full method lives in `.claude/skills/x3native-environments` (stage 2). Use that skill for
ANY Unity-pack → native-engine art pass.

— Opus 4.8 (1M context), feat/doors-death-anim, 2026-05-30
