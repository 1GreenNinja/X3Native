# TASK: CYBERPUNK-2077 METROPOLIS ART — assigned by 13700K integrator
*(Lane brief — assign to a free machine; the 14900K+5090 is the natural fit, GPU + asset/editor-adjacent. Rename to `<machine>.md` or have a freed-up box pick it up.)*

**You are a clean-room ART-PIPELINE + content engineer on X3Native** (native C++20/Vulkan 1.3). Work in YOUR clone, push a feature branch, report status at the bottom. The 13700K integrator merges + re-gates + pushes `main` — **you do NOT push `main`.**

## CLEAN-ROOM / LICENSING
NEVER read/reference/copy from RBDOOM, id Tech, Doom, Quake, or ANY other game-ENGINE source. The Cyberpunk City packs in `G:\Assets` are Tim's **licensed Unity Asset Store** content — fine to convert + use (like the ModularSciFi kit that `env_art.cpp` already uses). Build the native impl from X3Native's own interfaces.

## YOUR TASK: make the open-world metropolis look like CYBERPUNK 2077
The graybox metropolis STRUCTURE shipped in `app/city.{h,cpp}` (Scrapyard City / New District / Industrial Zone + a road grid + 4 freeway tunnels — districts at Scrapyard (-600,500), New District (200,500), Industrial (-200,350)). Dress it with the real Cyberpunk assets — exactly the way `app/env_art.cpp` drapes converted GLBs over the Level-1 graybox (graybox stays as collision/fallback; the art is a visual overlay, per-piece fallback if a GLB is missing).

**Two parts:**

### A) ASSET PIPELINE — FBX → GLB
Convert the Cyberpunk City packs in `G:\Assets` to GLB into the engine's converted-GLB dir (the dir `env_art` mounts — see `app/asset_root.h` `convertedGlbRoot()`), reusing the project's existing FBX→GLB path (FBX2glTF / Blender — whatever produced the current `converted_glb`; see `tools/`). Source packs (pick the best buildings/props/signage):
- `G:\Assets\Cyberpunk City Cyberpunk Cyberpunk City Sci-Fi City\` (primary)
- `G:\Assets\SciFi Neon Buildings\`, `G:\Assets\Cyberpunk Rooftop Market\`
- `G:\Assets\Road System - Modular Roads with Street and Traffic Lights\`
- `G:\Assets\Modular Cyber Racing Cars - Low Poly 3D Models\` (street vehicles)
Bring textures across (the converted-GLB pipeline embeds/points to them); keep names stable. Add a `docs/` note listing what converted OK.

### B) CityArt OVERLAY — `app/city_art.{h,cpp}`
A new `CityArtSystem` modelled on `env_art.{h,cpp}`: load the converted Cyberpunk GLBs + place instances over `city.cpp`'s district footprints + road/tunnel positions (read `City`'s `CityZonePlan` / `FreewayTunnelPlan` queries — do NOT edit `city.*`). Tower buildings on the district blocks, neon signage + emissive strips (HDR bloom — `Entity::emissive`), street props + parked cyber-cars along the roads, tunnel facades. Aim for a night-time **neon Cyberpunk-2077** read (lean on emissive + the existing bloom). Per-piece fallback (missing GLB → graybox stays). Do NOT touch `city.*`, `world_regions.*`, `act2_*`, `monster.*`.

READ FIRST: `app/env_art.{h,cpp}` (the overlay pattern to mirror), `app/city.h` (the graybox + its queries), `app/asset_root.h` (`convertedGlbRoot`), `engine/asset/IModelLoader.h` + `engine/rhi/IRenderDevice.h`, `docs/design/X3_WORLD_BLUEPRINT.md` §1.

TEST: `--test-cityart` headless (assert the converted GLBs load — or the graybox-fallback path is clean when absent — and instances place over the city footprints; report assetsLoaded/instanceCount). Keep every other `--test-*` green.

## WORKFLOW
1. `git fetch origin && git checkout -b feat/cityart origin/main`.
2. Implement (pipeline first, then the overlay). Commit frequently; commit a working state before the final build.
3. Build + gate (standard `windows-vs2026` preset + full `--test-*` + `--test-cityart`; Release `--smoketest` 0 VUID; Debug `--smoketest` 0 VUID + `allocationCount=0`). Add a Debug `--world city`-style capture if there's a city showcase, to eyeball the neon look.
4. `git push origin feat/cityart`. Do NOT touch `main`.

## REPORT STATUS (append below, then push the branch)
<!-- STATUS: branch HEAD, packs converted, files added, --test-cityart result, all-flags-0 + VUID 0 + allocationCount=0, "READY FOR INTEGRATION" or BLOCKED+why. -->
