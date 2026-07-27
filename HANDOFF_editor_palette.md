# HANDOFF — Data-Driven Editor GLB Palette

Branch: `feat/editor-glb-palette` (worktree `D:\GameDev\X3Native-dragdrop`, off main).
Status: **CODE + STAGING COMPLETE. NOT COMPILED.** Owner (Tim) reviews the diff, then builds.

## What changed (the committed code)

**`app/editor/editor.cpp` only** (engine/** untouched, no other worktree touched).

The hardcoded 8-item `editorModelCatalog()` is now a **runtime scan** of
`convertedGlbRoot()` (= `assetRoot() + "/converted_glb"`):

- Recursively enumerates every `*.glb` (case-insensitive) under that dir.
- For each, `relPath` = path relative to convertedGlbRoot with **forward slashes**;
  `displayName` = `"<TopFolder> / <Humanized filename>"` (e.g.
  `SpaceStationsCreator/AlienSpaceStation1.glb` → `"Space Stations Creator / Alien Space Station 1"`).
- Scan is cached once in a `static const ModelCatalogStore` (thread-safe function-local
  static init). The store owns the backing `std::string`s and is built **in place**, so the
  `const char*` in each `ModelCatalogItem` stays valid for process lifetime.
- `editorModelCatalog()` / `editorModelCatalogCount()` signatures unchanged — they now
  return `.data()` / `.size()` of the scanned vector. Existing caller
  (`editor_host.cpp` model-browser panel, iterates by count) works unchanged.
- Missing/empty dir → empty catalog (no crash). Graybox-safe behavior preserved (a GLB
  that fails to load places nothing — unchanged, that logic lives in editor_host.cpp).
- Self-test E15 in `runEditorSelfTest()` relaxed: it no longer asserts a fixed count of 8
  (count is now machine-dependent); it asserts the catalog *shape* (empty is valid; any
  present entry is a well-formed `.glb` relpath) and keeps the real round-trip assertion.

New includes in editor.cpp: `<algorithm> <cctype> <filesystem>` and `"../asset_root.h"`.

## Staging (machine-local, NOT committed)

- Script: **`tools/stage_editor_glb.ps1`** (committed). Flattens a pack's deep Unity GLB
  tree into `D:\Assets\_glb\_editor\<PackName>\<mesh>.glb` via **hardlinks**
  (`New-Item -ItemType HardLink` == `mklink /H`; same D: volume, instant, zero extra
  bytes). Then junctions it into the editor's content root and adds a `.git/info/exclude`
  entry so it never lands in git. **Resumable** — re-run to pick up newly-converted modules.
- Already run for **Sci-Fi Space Stations Creator** → staged **177 modules** into
  `D:\Assets\_glb\_editor\SpaceStationsCreator\`. (Conversion of this pack was still in
  progress — 119 → 177 during this session — so re-run the script to pick up the rest.)
- Junction created:
  `assets\converted_glb\SpaceStationsCreator` → `D:\Assets\_glb\_editor\SpaceStationsCreator`.
  NOTE: we junction a **subfolder** (not `converted_glb` itself) because `converted_glb`
  already holds 57 git-tracked LFS GLBs; junctioning the parent would delete them from the
  working tree. The recursive scanner picks up both the existing packs and the staged one.
- `git status` is clean: only `app/editor/editor.cpp` (M) + `tools/stage_editor_glb.ps1`
  (untracked) + this handoff. The 177-module junction is excluded via the common
  `.git/info/exclude`.

### To stage the next pack later
```powershell
pwsh tools\stage_editor_glb.ps1 -SourceRoot "D:\Assets\_glb\tech\<pack folder>" -PackName "<CleanName>"
```
Or just re-run the Space Stations line below to catch modules that finished converting after this session:
```powershell
pwsh tools\stage_editor_glb.ps1 -SourceRoot "D:\Assets\_glb\tech\Sci-Fi Space Stations Creator" -PackName "SpaceStationsCreator"
```

## BUILD (do this — I did NOT compile; one native build at a time)

From the worktree root `D:\GameDev\X3Native-dragdrop`, with `VCPKG_ROOT` set and Vulkan SDK installed:
```powershell
cmake --preset windows-vs2026          # configure (only needed if not already configured)
cmake --build --preset windows-vs2026  # Release x64 -> build\bin\Release\X3Engine.exe
```

## VERIFY

1. **Headless self-test** (fast, no window) — the editor logic incl. the relaxed E15:
   ```powershell
   .\build\bin\Release\X3Engine.exe --test-editor ; echo $LASTEXITCODE   # expect 0
   ```
   (Related: `--test-blockout`, `--test-editor-ai` should also stay 0.)
2. **Live palette** — launch the editor and confirm the drag-and-drop palette is data-driven:
   ```powershell
   .\build\bin\Release\X3Engine.exe --editor
   ```
   - In the **Models** panel (lower-left), the list should now show the
     **`Space Stations Creator / …`** modules (177 of them) alongside the existing props.
   - Click one (or drag it into the viewport) → a `model` entity spawns at the camera focus
     and its GLB renders (graybox-safe: if a specific GLB fails to load, nothing spawns —
     that's expected, not a regression).
   - Save the level and confirm the placed module's `model` relpath round-trips in the JSON.

## Notes
- The palette **code** is the committed change; the **junction + hardlink staging** are
  machine-local and intentionally not committed.
- No cmake/MSVC was run by this task (box CPU-saturated + concurrent Draco build; one native
  build at a time). Engine/** and all other worktrees were left untouched.

---

## FOLLOW-UP SESSION (2026-07-14): first real build + a real space-station scene + a load-bearing bug fix

Picked this branch up to build a real, connected, assembled space-station LevelDoc scene
from the staged palette. Summary of what changed, in the order it mattered:

### 1. First-ever compile of this worktree hit a real bug — fixed
`app/editor/editor.cpp` failed to compile: `bool near(float a, float b, float e = 1e-3f)`
(the self-test's float-compare helper) silently expands under MSVC/`<windows.h>` — `near`
is a leftover 16-bit segment-pointer macro that expands to nothing, mangling the function
signature into a syntax error. Renamed the 38 call sites to `feq` (matching the identical
helper already named that way in `app/leveldoc_world.cpp`). Build is now clean; `--test-editor`
(19/19) and `--test-loader` (8/8) both PASS, exit 0.

### 2. DISCOVERED: the staged palette never actually rendered real geometry — Draco
Every GLB in `3D Scifi Kit Vol 2`, `3D Scifi Kit Vol 3`, **and the already-staged
`Sci-Fi Space Stations Creator`** exports with `extensionsRequired: ["KHR_draco_mesh_compression"]`.
`engine/asset/ModelLoader.cpp` (`buildPrimitives()`) has **no Draco decoder** — it
unconditionally does `if (prim.has_draco_mesh_compression) { skip; }` — so every piece from
all three packs silently fell back to the small graybox marker. This was invisible in the
prior session because `--test-editor`/`--test-loader` never load real GLB content, and no
screenshot was taken. **The palette's "click to spawn, it renders" claim in the section
above was never actually verified — it wasn't true until this fix.**

Fix: `npx @gltf-transform/cli copy <src> <dst>` decodes Draco on read and (since `copy`
applies no compression on write) emits a plain GLB — verified byte-identical in content,
no `extensionsUsed/Required`, normal bufferView-backed POSITION/NORMAL/TEXCOORD_0. New
script **`tools/decode_draco_glb.py`** batch-runs this per staged pack into a sibling
`<PackName>Decoded` dir (resumable, same convention as `stage_editor_glb.ps1`). Only the
~24 pieces the space-station scene references were decoded so far (fast path, ~30s);
decoding the FULL Vol2 (192) / Vol3 (884) packs is still pending — budget ~20-25 min for
Vol3 via `python tools/decode_draco_glb.py --editor-root D:\Assets\_glb\_editor --pack
ScifiKitVol3 --all` (same for Vol2/SpaceStationsCreator) if you want the whole in-editor
palette to actually render instead of graybox.

Junctions added (machine-local, git-excluded, same pattern as the raw packs):
`assets/converted_glb/ScifiKitVol2Decoded` → `D:\Assets\_glb\_editor\ScifiKitVol2Decoded`,
and the Vol3 equivalent.

### 3. Built: a real assembled space-station LevelDoc
- **Bbox ground truth**: pure-stdlib GLB AABB scanner (parses the RIFF-style GLB container,
  composes the node hierarchy, reads POSITION accessor min/max — no non-stdlib deps) scanned
  all 1430 GLBs across Vol2/Vol3/SpaceStationsCreator, 0 skipped. Manifest at
  `D:\Assets\_glb\_host\scifi_station_bbox.json`.
- **Generator**: `tools/build_space_station_level.py` — reads the manifest, reverse-engineers
  each kit family's pivot convention from real min/max (corner-pivoted for the Vol2 interior
  kit + the Vol3 "Ext_*/Roof_01-04" 5m-story family; center-pivoted for `Platerform_Metal_*`
  and the `Stargate_*` family), and lays out one connected structure: a walled/roofed hangar
  pad (Plateform deck + Ext_Wall_Addon shell + Roof_01 caps + Wall_Big_Simple/Angle entrance
  pylons), an 8m corridor chain (`Corridor_Coin_Big` × 3) linking it to a second pad, and a
  Stargate ring centerpiece assembled from `Stargate_Warp_01` + `Part_A/B/C` **all placed at
  the identical pos/yaw/scale** — their raw (huge, off-center) local vertex data already
  encodes each part's true offset from a shared gate origin, confirmed by their bboxes sharing
  a common radius (~410-470 units) with the Warp disc — so one shared placement reassembles
  the ring correctly without needing per-part rotation math the LevelDoc format can't express
  (yaw is Y-axis only; ring segments would need Z-axis rotation to sweep, which isn't available).
  `Space_Base_Module_01` (Tim's "mountain-top base") anchors the far pad as a foundation mass.
- **Output**: `assets/levels/space_station.leveldoc.json` (95 entities, 4 collision brushes).

### 4. Verified
`--test-loader` and `--test-editor` both green post-fix. Screenshots taken and READ (not
just claimed) via `--world fromdoc assets\levels\space_station.leveldoc.json --screenshot
<path> --shot-cam <x,y,z,yaw,pitch>` — see `build/proof/space_station_*.png` (not committed,
build output). Confirmed: zero graybox fallbacks (all 24 referenced pieces load with real
primitives), a walled/roofed hangar interior with a visible railing down the corridor, a
connected corridor arc chain, and a large standing stargate ring with its Part A/B/C
framework. **Residual, not fixed**: the ring assembly reads slightly disconnected from its
pad in one angle (worth Tim's eye — likely one of the four co-located parts extending lower
or higher than expected; I could not inspect the actual mesh topology, only its bbox).

**Also discovered, unrelated to this station and NOT fixed** (documented for whoever picks
this up): `--shot-cam` renders correctly at true world scale/distance only for
axis-aligned yaw (0, π/2, π, 3π/2); any oblique/diagonal yaw produces a badly-mis-scaled
close-up of whatever's in the general view direction, regardless of the requested camera
distance. Root cause not found (verified it's not the CLI parser — that's fine — and not a
simple pitch-magnitude threshold). Every proof screenshot in this session was taken from an
axis-aligned vantage to work around it.

### To load it
```powershell
.\build\bin\Release\X3Engine.exe --world fromdoc assets\levels\space_station.leveldoc.json --editor
```
(`--editor` to browse/edit it live in F8; drop `--editor` to just fly around it in-game.)
