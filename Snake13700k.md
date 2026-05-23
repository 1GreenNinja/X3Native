# TASK FOR SNAKE / ALT-13700K (second screen) — assigned by 13700K integrator

**You are a clean-room engine engineer on X3Native** (native C++20/Vulkan 1.3, this repo). Work in YOUR clone, push a feature branch, report status at the bottom. The 13700K integrator merges + re-gates + pushes `main` — **you do NOT push `main`.** (Your previous task `feat/act2-world` is DONE + integrated — thank you. This is your NEXT task.)

## ABSOLUTE CLEAN-ROOM RULE
NEVER read/reference/copy from RBDOOM, id Tech, Doom, Quake, or ANY other game-ENGINE source. Only X3Native's own headers + the EFLZ design docs in `docs/design/` (Tim's IP — OK) + public refs + permissive libs.

## YOUR TASK: dimension Floors 2-7 to real, identity-appropriate scale
Floor 1 was just re-laid to its real ~75×43 m detention layout (see `docs/design/SPIRE_LEVELARCHITECT_DIMS.md` + how `app/level1.cpp buildLevel1()` now builds the F1 interior). **Floors 2-7 still share the big F1-sized plate with no real interior** — give each its own identity-appropriate footprint + a basic interior. **Touch ONLY `app/level1.{h,cpp}` and the affected spire TEST code** (you may edit `--test-spiremid`/`--test-spiretop` assertions in `spire_mid.cpp`/`spire_top.cpp` ONLY to update footprint-derived position numbers — do NOT restructure their encounters/bosses). Do NOT touch monster.*, weapon.*, act2_*, or other lanes.

READ FIRST: `docs/design/SPIRE_LEVELARCHITECT_DIMS.md` (F1 template + the "F2-7 need fresh dims" note), `docs/design/EFLZ_WORLD_STRUCTURE.md` (the true floor identities), `app/level1.{h,cpp}` (the `kFloors[]` table, `L1RoomDef`/the new F1 detention builder, `floorBaseY`, the `Level1Layout`, `buildLevel1()` + its wall/doorway helpers), and how `spire_mid/top/nexus/sublevels` READ `floorBaseY[]`+the footprint to place content.

IMPLEMENT — give each floor a real footprint + a basic graybox interior fitting its identity (use the F1 detention builder pattern + the existing wall/doorway helpers; sizes in meters, human-scale ~1.8 m, ceilings ~3.5-4 m; rooms not tiny):
- **F2 Medical Bay** — wards + the 3 rescue rooms (Aria/Keisha/Emily) + Dr. Chen's area + Sarah's distant cell. (~F1-scale or larger.)
- **F3 Genetics Lab** — research wing, gene-vats, the Failed-Experiment-#7 arena.
- **F4 Cybernetics Workshop** — augmentation chairs, the Collective area, the F4→4.5 Nexus connector.
- **F5 Drone Manufacturing** — a LARGE drone-assembly bay (the drone level + Swarm-AI arena).
- **F6 Alien Technology Lab** — alien-tech halls, the Salvari/K'thara first-contact + Alien-Overseer arena.
- **F7 Executive Laboratory** — exec suites + the rooftop Clone/Sarah finale arena.
**Keep the vertical-stack model** (floors stacked by `floorBaseY`; raise the per-floor pitch if needed to clear taller ceilings) and a **consistent elevator-shaft XZ on every floor** (the spire content hard-codes arrival at x≈17.5-21,z≈0 — keep the shaft there so every floor's content + the elevator still align). All existing floor content (bosses/rescues/doors/sub-levels/Nexus) MUST still build + be reachable.

## TEST
Update `--test-level1`/`--test-spiremid`/`--test-spiretop` for the new per-floor footprints (assert each floor's footprint + key rooms exist, elevator aligns, content reachable). PRESERVE all semantic asserts (enemy counts/roles, rescue gating, escalation, boss presence) — only change footprint/position numbers that legitimately moved. Keep every other `--test-*` green.

## WORKFLOW (in your clone)
1. `git fetch origin && git checkout -b feat/floors2-7-dims origin/main`.
2. Implement. **Commit frequently**; commit a working state BEFORE the final build.
3. Build + gate (PowerShell). `--test-gltf` regenerates `docs/GLB_IMPORT_REPORT.md` → `git checkout -- docs/GLB_IMPORT_REPORT.md` after.
   ```powershell
   $env:VCPKG_ROOT="C:\vcpkg"; $env:VULKAN_SDK="C:\VulkanSDK\1.4.350.0"
   $cmake="C:\Program Files\Microsoft Visual Studio\18\Insiders\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
   & $cmake --preset windows-vs2026; & $cmake --build --preset windows-vs2026
   ```
   Run ALL these against `.\build\bin\Release\X3Engine.exe`; each MUST exit 0:
   `--test-asset --test-console --test-physics --test-gltf --test-player --test-interact --test-pickup --test-combat --test-audio --test-level1 --test-jobs --test-phase2a --test-phase2b --test-anim --test-terrain --test-streaming --test-ai --test-doorcode --test-elevator --test-terrainplace --test-net --test-rescue --test-locomotion --test-destruction --test-nav --test-weapons --test-vehicle --test-footik --test-bestiary --test-ui --test-netsync --test-spiremid --test-debris --test-spiretop --test-collapse --test-netinterp --test-saveload --test-netpredict --test-gpuskin --test-bosses --test-dronehack --test-nexus --test-sublevels --test-valley --test-cliffs --test-act2`
   Then Release `--smoketest` 0 VUID; Debug build + `--smoketest` = 0 VUID AND "live allocationCount=0". Also Debug `--capture-spire captures\spire` 0 VUID (eyeball the floors are properly sized).
4. `git push origin feat/floors2-7-dims`. Do NOT touch `main`.

## REPORT STATUS (append below, then push the branch)
<!-- STATUS: branch HEAD, files changed, per-floor footprints, test result lines, all-flags-0 + VUID 0 + allocationCount=0, "READY FOR INTEGRATION" or BLOCKED+why. -->
