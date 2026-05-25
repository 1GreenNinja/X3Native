# TASK FOR DJBOOTH (garage 4790K / 1080Ti) — assigned by 13700K integrator

**You are a clean-room gameplay/content engineer on X3Native** (native C++20/Vulkan 1.3, this repo). Work in YOUR clone, push a feature branch, report status at the bottom. The 13700K integrator merges + re-gates + pushes `main` — **you do NOT push `main`.** Your previous task `feat/act2-roster` is DONE + integrated — thank you. THIS is your next task.

## ABSOLUTE CLEAN-ROOM RULE
NEVER read/reference/copy from RBDOOM, id Tech, actual Quake/Doom ENGINE source, or any OTHER third-party engine. You MAY read Tim's OWN design docs + his OWN game code. Build native impl from X3Native's interfaces + the EFLZ design.

## YOUR TASK: Act 2 mid biomes — L12 Advanced Caves (+ Memory Hunter) + L13–15 Toxic Swamplands (+ Siren)
Act 2's opening (L8/L9 in `act2_world.*`) + L10/L11 desert (I5000's `act2_desert.*`, landing soon) exist. Build the MID biomes. **Create a NEW module `app/act2_caves.{h,cpp}`** (or `act2_mid.*`) + minimal `main.cpp`/`CMakeLists` wiring. Do NOT edit `act2_world.*`/`act2_desert.*` (read them for the framework + transition pattern), Act-1 floor files, monster.*, or other lanes.

READ FIRST:
- `G:\Unity_Projects\PARALLEL_TASKS_8_CHATS\TASK_3_LEVELS_8-20_OPENWORLD.md` (L12 Advanced Cave System ⭐ — bioluminescent caves, Salvari Archives, **Crystal Heart Chamber** [needs Jake-strength + Sarah-hack; major story branch], boss **Memory Hunter**; L13 Toxic Swamplands Edge — mutated flora, env-suit, poison; L14 Research Station — mutated scientists, Beta: **Siren/Aria** ambush; L15 Tree Cities — vertical, trading, resistance).
- `docs/design/X3_WORLD_BLUEPRINT.md` §4.2 + `EFLZ_BESTIARY.md`.
- `app/act2_world.{h,cpp}` (framework: `Act2Level`/`Act2AreaPlan`/`HazardZone`/`Act2Trigger` + the `--test-act2` style — MIRROR it).
- `app/monster.h` — the Act-2 roster already on main: `MemoryHunter`, `TheSiren`, `MutatedScientist`, `MutatedFlora`, `SalvariAlly` (use them; the boss machine + `bossDef`/`Act2BossType`).

IMPLEMENT `app/act2_caves.{h,cpp}` (mirror `act2_world`/`spire_mid` authoring): L12 bioluminescent cave system (multi-layer: upper caves/lake → Salvari Archives → Crystal Heart Chamber [a strength+hack gated interactable, story-branch flag] → abyss boss **Memory Hunter**); L13 toxic swamp edge (mutated flora hazards + a poison/exposure HazardZone); L14 research station (mutated scientists + a gated Beta **Siren/Aria** ambush — present only if the F2 women weren't saved, per a timeline flag; otherwise a normal encounter); L15 tree cities (vertical graybox + a trading/upgrade interact). Reachable L11→L12→…→L15 via triggers (fresh id range). Existing-systems only.

## TEST
`--test-act2caves`: headless — L12-15 build with expected areas/footprints; Memory Hunter present at L12 + the Crystal Heart is strength+hack gated (inert until both); the L13 poison hazard is present-but-inert-until-entered; the L14 Siren ambush is gated on the timeline flag; L11→L15 reachable. Print `act2caves: X/Y passed`, exit nonzero on fail.

## WORKFLOW (in your clone)
1. `git fetch origin && git checkout -b feat/act2-caves origin/main`.
2. Implement. **Commit frequently**; commit working state BEFORE the final build.
3. Build + gate if your toolchain is ready (else push code + note "gate pending — integrator gates"). `--test-gltf` regenerates `docs/GLB_IMPORT_REPORT.md` → `git checkout -- docs/GLB_IMPORT_REPORT.md` after.
   ```powershell
   $env:VCPKG_ROOT="C:\vcpkg"; $env:VULKAN_SDK="C:\VulkanSDK\1.4.350.0"
   $cmake="C:\Program Files\Microsoft Visual Studio\18\Insiders\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
   & $cmake --preset windows-vs2026; & $cmake --build --preset windows-vs2026
   ```
   Run EVERY `--test-*` flag parsed in app/main.cpp against `.\build\bin\Release\X3Engine.exe` (each exit 0) + your new `--test-act2caves`; then Release `--smoketest` 0 VUID; Debug build + `--smoketest` = 0 VUID + "live allocationCount=0".
4. `git push origin feat/act2-caves`. Do NOT touch `main`.

## REPORT STATUS (append below, then push the branch)
<!-- STATUS: branch HEAD, files added/changed, "act2caves: X/Y passed" (or "gate pending"), VUID/leak if gated, "READY FOR INTEGRATION" or BLOCKED+why. -->
