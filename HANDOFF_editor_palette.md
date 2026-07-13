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
