---
name: realistic-environments
description: Build REALISTIC game environments/cities in Unreal Engine from Unity asset packs (e.g. on \\p13700\G\Assets) when realism matters (Cyberpunk 2077 / GTA level). Use whenever assembling a city, district, town, interior, or any environment from imported FBX packs and the result must look curated and properly textured — NOT a procedural grid of grey square blocks. Triggers on: "make it look realistic", "CP2077/GTA quality", "not square blocks", "use the textures", "use the demo scene", building Neon Sprawl / any Riftforged world, or any UE environment pass that scatters pack meshes.
---

# Realistic environments from Unity asset packs (UE5)

The two failure modes that make pack-built scenes look fake — avoid BOTH:

1. **Grey / flat buildings** — Unity (HDRP/URP) materials DO NOT import into UE. A raw FBX
   import brings the mesh and maybe the texture *files*, but the working material is lost.
   You MUST rebuild materials in UE from the texture maps. This is the #1 realism unlock.
2. **Procedural square-block grids** — random buildings on a uniform grid reads as fake
   sprawl. Real cities (and the pack demo scenes) are *curated*: varied massing, hero
   landmarks, alleys, props, signage, density gradients, broken alignment.

## Step 1 — Mine the pack's DEMO SCENE first (ground truth)
Every quality pack ships a demo/showcase scene the artist arranged with correct materials
and composition. Treat it as the reference, don't reinvent.
- Find it: `Demo*/`, `Showcase*/`, `Example*/`, `Scenes/`, `DemoScene/` + the `.unity`
  scene and `.prefab` files in the pack on G:.
- The demo tells you: which meshes are hero vs background, street widths, prop density,
  signage placement, and the exact texture set per surface.
- If practical, parse the `.prefab` / `.unity` YAML (GameObject transforms + mesh refs) to
  reconstruct EXACT placements. Otherwise replicate the demo's composition by eye.
- Many Leartes/HIVEMIND packs include a pre-merged "demo scene" mesh — importing that one
  mesh gives the whole arranged block instantly (then texture it per Step 2).

## Step 2 — Rebuild materials from the texture maps (the realism unlock)
Raw import = grey. Fix it:
- Locate the pack's textures: BaseColor/Albedo, Normal, Metallic, Roughness (or Smoothness),
  AO, **Emissive/Emission**, and any packed Mask/ORM/MRAO texture.
- Build a UE master material (+ instances per surface) wiring: BaseColor → BaseColor,
  Normal → Normal (flip G if it looks inverted), Metallic, Roughness (1-x if the map is
  *Smoothness*), AO, and **Emissive driven strongly** (neon signage/windows must GLOW and
  bloom — this is what sells cyberpunk).
- Unity frequently channel-PACKS Metallic+AO+Smoothness into one texture. Split R/G/B/A into
  the correct UE inputs (use a Mask/ComponentMask node), don't plug the packed texture in raw.
- Assign materials to each mesh material slot, matching by name. After the pass, verify NO
  mesh has a default grey slot left.
- This material work MUST run in the GUI editor (Slate up), never `-nullrhi`
  (MaterialEditingLibrary + texture import crash headless).

## Step 3 — Curate the layout, don't scatter
- **Massing**: dramatic height variation (hero towers beside low shops); density gradient
  (dense core, sparser edges); carve alleys, plazas, setbacks.
- **Detail layers** (what actually reads as "real"): props (AC units, pipes, dumpsters,
  cables, vents, market stalls), emissive signage/billboards, decals (grime, posters,
  cracks, graffiti), street clutter.
- **Streets**: wet reflective asphalt (high spec + puddle normals), road markings, curbs,
  crossings — use a road kit (Road System pack), not a flat dark plane.
- Break the grid: jitter, rotate off-axis, insert diagonals + landmarks. No two blocks
  identical. NEVER a uniform lattice of the same merged box.

## Step 4 — Lighting + atmosphere (sells the mood)
- Night neon-noir: dim cool moonlight, volumetric `ExponentialHeightFog`
  (`enable_volumetric_fog=True`), MANY emissive neon practicals (magenta/cyan/amber).
- On DX12 + hardware-RT (the 5090): wet streets + RT reflections + emissive neon is the
  whole CP2077 look — lean on it.
- PostProcess: strong bloom (neon glow), cool color grade, vignette, subtle chromatic
  aberration; lens flares on hero signs.

## Step 5 — Verify like a human
- Take a screenshot / have the user eyeball it.
- Grey buildings → materials not rebuilt (back to Step 2).
- "Looks like a parking lot of identical blocks" → curation failed (Step 3).
- Flat lighting → Step 4.

## Riftforged / Tim's setup specifics
- Neon Sprawl = `L_RiftModernWorld`. Cyberpunk packs on `\\p13700\G\Assets`: **Cyberpunk City
  (HIVEMIND)**, **Cyberpunk Rooftop Market**, **Cyberpunk City Recife**, **SciFi Neon
  Buildings**, **Miami Vice City**, **POLYGON Nightclubs** — inspect each pack's demo scene.
- G: import is network-bound (~940 Mbps); meshes cache locally on C: after first import.
- Import buildings with **Nanite** for density; build materials per Step 2.
- The `SM_MERGED_BP_*` meshes are *pre-merged background* buildings — fine for backdrop, but
  the foreground/playable streets need the modular kit + props + rebuilt materials, arranged
  like the demo, to hit realism. Don't build the whole city from merged background boxes.
