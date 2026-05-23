# TASK FOR DJBOOTH (garage 4790K / 1080Ti) — assigned by 13700K integrator

**You are a clean-room gameplay engineer on X3Native** (native C++20/Vulkan 1.3 game engine, this repo). Work this task in YOUR clone, push a feature branch, and report status at the bottom of this file. The 13700K integrator merges + re-gates + pushes `main` — **you do NOT push to `main`.**

## ABSOLUTE CLEAN-ROOM RULE (NON-NEGOTIABLE)
NEVER read/reference/copy from RBDOOM-3-BFG, id Tech, Doom, Quake, or ANY other game-ENGINE source. Build ONLY from X3Native's own headers + the EFLZ design docs in `docs/design/` (Tim's own IP — OK to read) + public references + permissive (MIT/Apache/zlib/BSD/public-domain) libs.

## YOUR TASK: Act 2 enemy/boss roster (monster.{h,cpp})
Add the Act-2 (alien-planet surface) enemies + bosses as DATA on the existing machine, so the Act-2 world agents can place them. **Touch ONLY `app/monster.h`, `app/monster.cpp`, and `app/main.cpp` (a test flag).** Do NOT create Act-2 world modules (another machine owns those).

READ FIRST: `docs/design/EFLZ_BESTIARY.md`, `docs/design/EFLZ_WORLD_STRUCTURE.md`, `docs/design/EFLZ_MASTER_PLAN.md` (Act 2 = Levels 8-20), and `app/monster.h`/`.cpp` — especially the Wave-1 boss API (`BossType`, `bossDef()/bossTuning()`, `MultiPodBoss`, `ScriptedFightHook`) and `namespace combat` bands. Tune all new HP/damage to the existing Martinez-relative budget (use the `combat::` bands, NOT raw bible numbers).

IMPLEMENT (data rows + minimal machine reuse; keep all existing enemies/bosses unchanged):
1. **Salvari ally** — an ALLIED type (refugee/companion: non-hostile, can fight beside the player; reuse the allied/faction-flip flag the boss machine already has).
2. **Native desert fauna** — a neutral-or-hostile creature (crystalline desert).
3. **Mutated scientist** + **mutated flora** — toxic-swamp hostiles (flora = stationary/lashing).
4. **Surface pursuit drone** — fast ranged flyer (L8 escape).
5. **Act-2 bosses** on the existing phase machine: **Memory Hunter** (L12 — psychological/identity gimmick: a phase where it copies/feints; keep data-driven), **The Siren** (Beta, L14 — transformed Aria), **Breeder Queen** (Beta, L16 — transformed Keisha, summons), **Planetary Garrison Commander** (L20 finale — 3 phases: troops → mech-suit [bigger/tougher] → orbital-strike escape timer).

TEST: add `--test-act2bosses` in main.cpp (pattern of `--test-bosses`). Assert each new enemy/boss def exists with sane stats/phases; the Salvari ally is flagged allied (0 damage to player); Garrison Commander has its 3 phases; Martinez + Act-1 bosses still construct. Print `act2bosses: X/Y passed`, exit nonzero on fail.

## WORKFLOW (in your clone)
1. `git fetch origin && git checkout -b feat/act2-roster origin/main` (branch from CURRENT origin/main).
2. Implement. **Commit frequently.** Commit a working state BEFORE your final build.
3. Build + gate (Windows PowerShell):
   ```powershell
   $env:VCPKG_ROOT="C:\vcpkg"; $env:VULKAN_SDK="C:\VulkanSDK\1.4.350.0"
   $cmake="C:\Program Files\Microsoft Visual Studio\18\Insiders\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
   & $cmake --preset windows-vs2026; & $cmake --build --preset windows-vs2026
   ```
   Run ALL these against `.\build\bin\Release\X3Engine.exe`; each MUST exit 0:
   `--test-asset --test-console --test-physics --test-gltf --test-player --test-interact --test-pickup --test-combat --test-audio --test-level1 --test-jobs --test-phase2a --test-phase2b --test-anim --test-terrain --test-streaming --test-ai --test-doorcode --test-elevator --test-terrainplace --test-net --test-rescue --test-locomotion --test-destruction --test-nav --test-weapons --test-vehicle --test-footik --test-bestiary --test-ui --test-netsync --test-spiremid --test-debris --test-spiretop --test-collapse --test-netinterp --test-saveload --test-netpredict --test-gpuskin --test-bosses --test-dronehack --test-nexus --test-sublevels --test-act2bosses`
   Then Release `--smoketest` 0 VUID; Debug build (`& $cmake --build build --config Debug`) + `--smoketest` = 0 VUID AND "live allocationCount=0". (`--test-gltf` regenerates `docs/GLB_IMPORT_REPORT.md` — `git checkout -- docs/GLB_IMPORT_REPORT.md` after.)
4. `git push origin feat/act2-roster`. Do NOT touch `main`.

## REPORT STATUS (append below, then push the branch)
<!-- STATUS: branch HEAD hash, files changed, "act2bosses: X/Y passed", all-flags-0 + VUID 0 + allocationCount=0 confirmation, and "READY FOR INTEGRATION" (or BLOCKED + why). -->
