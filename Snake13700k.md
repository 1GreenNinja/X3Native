# TASK FOR SNAKE (second screen) — assigned by 13700K integrator

**You are a clean-room gameplay/content engineer on X3Native** (native C++20/Vulkan 1.3 game engine, this repo). Work this task in YOUR clone, push a feature branch, report status at the bottom. The 13700K integrator merges + re-gates + pushes `main` — **you do NOT push to `main`.**

## ABSOLUTE CLEAN-ROOM RULE (NON-NEGOTIABLE)
NEVER read/reference/copy from RBDOOM-3-BFG, id Tech, Doom, Quake, or ANY other game-ENGINE source. Build ONLY from X3Native's own headers + the EFLZ design docs in `docs/design/` (Tim's own IP — OK) + public references + permissive libs.

## YOUR TASK: Act 2 open-world surface HOST + the opening (L8 Surface Emergence + L9 Crystalline Desert Edge)
EFLZ Act 2 (Levels 8-20) is the alien-planet surface — open world. Build the Act-2 HOST + the first two levels. **Create a NEW module `app/act2_world.{h,cpp}`** + minimal `app/main.cpp` wiring + `app/CMakeLists.txt`. Do NOT edit Act-1 floor files or `monster.*` (other lanes own those).

READ FIRST:
- `docs/design/EFLZ_MASTER_PLAN.md` (Act 2 = L8-20) + `docs/design/EFLZ_WORLD_STRUCTURE.md` (Act-2 overview). The full level breakdown is `G:\Unity_Projects\PARALLEL_TASKS_8_CHATS\TASK_3_LEVELS_8-20_OPENWORLD.md` (read it — L8 Surface Emergence, L9 Crystalline Desert Edge).
- The engine systems you BUILD ON (study how Level 1 / the `--world terrain` and `--world ocean` paths use them in `app/main.cpp`): terrain (`app/terrain.*` — procedural heightmap + streaming + height/slope splat), the analytic sky/sun, water/ocean, the scene/trigger systems. Mirror the authoring pattern of `app/spire_mid.{h,cpp}` (build/tick/draw/onTrigger/queries + a headless self-test).

IMPLEMENT `app/act2_world.{h,cpp}` — an `Act2World` system:
- Stands up the alien-planet surface: terrain world + analytic sky tuned ALIEN (binary-sun / purple-atmosphere tint via the sky's sun-color/ambient params — approximate; do not add new engine tech) + water where needed.
- A simple **biome/area/level framework** (an enum of Act-2 levels L8-20 + per-level area descriptors: footprint, biome, spawn, objective) so later levels slot in. Implement the first two:
  - **L8 Surface Emergence**: a lab-exit tunnel (pursuit drones + a few infected — use EXISTING roster types; the Act-2-specific roster lands separately) → "The Emergence Point" open reveal/safe zone (the awe beat; companions present as markers/NPCs).
  - **L9 Crystalline Desert Edge**: open desert terrain biome with crystal-formation props, environmental-hazard hooks (heat/sandstorm as a tracked stat/zone), and a couple of neutral fauna placeholders.
- Reachability: L8 → L9 progression via a trigger/transition (mirror the elevator/hub pattern).
- Keep it data/level-script + existing-systems only — NO renderer/engine changes.

TEST: add `--test-act2` in main.cpp (pattern of `--test-spiremid`/`--test-terrainplace`). Headless: the Act-2 world builds; L8 + L9 areas load with expected footprints + spawn + objective; L8→L9 transition reachable; hazard zone present but inert until entered. Print `act2: X/Y passed`, exit nonzero on fail.

## WORKFLOW (in your clone)
1. `git fetch origin && git checkout -b feat/act2-world origin/main`.
2. Implement. **Commit frequently.** Commit a working state BEFORE your final build.
3. Build + gate (PowerShell):
   ```powershell
   $env:VCPKG_ROOT="C:\vcpkg"; $env:VULKAN_SDK="C:\VulkanSDK\1.4.350.0"
   $cmake="C:\Program Files\Microsoft Visual Studio\18\Insiders\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
   & $cmake --preset windows-vs2026; & $cmake --build --preset windows-vs2026
   ```
   Run ALL these against `.\build\bin\Release\X3Engine.exe`; each MUST exit 0:
   `--test-asset --test-console --test-physics --test-gltf --test-player --test-interact --test-pickup --test-combat --test-audio --test-level1 --test-jobs --test-phase2a --test-phase2b --test-anim --test-terrain --test-streaming --test-ai --test-doorcode --test-elevator --test-terrainplace --test-net --test-rescue --test-locomotion --test-destruction --test-nav --test-weapons --test-vehicle --test-footik --test-bestiary --test-ui --test-netsync --test-spiremid --test-debris --test-spiretop --test-collapse --test-netinterp --test-saveload --test-netpredict --test-gpuskin --test-bosses --test-dronehack --test-nexus --test-sublevels --test-act2`
   Then Release `--smoketest` 0 VUID; Debug build + `--smoketest` = 0 VUID AND "live allocationCount=0". (`--test-gltf` regenerates `docs/GLB_IMPORT_REPORT.md` — `git checkout -- docs/GLB_IMPORT_REPORT.md` after.)
4. `git push origin feat/act2-world`. Do NOT touch `main`.

## REPORT STATUS (append below, then push the branch)
<!-- STATUS: branch HEAD hash, files added/changed, "act2: X/Y passed", all-flags-0 + VUID 0 + allocationCount=0 confirmation, and "READY FOR INTEGRATION" (or BLOCKED + why). -->
