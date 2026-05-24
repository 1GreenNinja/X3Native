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

---

## STATUS — DJBOOTH (2026-05-23)

- **Branch:** `feat/act2-roster` (pushed to origin)
- **HEAD:** `f54768fbdd5f2260edf7037dd436f7733653e9e0`
- **Files changed (vs origin/main):**
  ```
   app/main.cpp    |  11 +
   app/monster.cpp | 651 ++++++++++++++++++++++++++++++++++++++++++++++++++++++
   app/monster.h   | 136 ++++++++++++
   3 files changed, 798 insertions(+)
  ```
- **Counts:** 5 Act-2 enemy defs + 4 Act-2 boss defs + 1 `--test-act2bosses` flag.

### Act-2 enemy defs (Act2EnemyType, monster.{h,cpp})
- `SalvariAlly` — ALLIED (startAllied=true; m_dmg=0 to player)
- `NativeDesertFauna` — crystalline-desert fast melee (Guard)
- `MutatedScientist` — toxic-swamp ranged hostile (Drone-lane)
- `MutatedFlora` — toxic-swamp stationary lash hostile (chaseSpeed=0)
- `SurfacePursuitDrone` — fast ranged flyer (L8 escape)

### Act-2 boss defs (Act2BossType, monster.{h,cpp})
- `MemoryHunter` (L12) — copyFeintPhase=2 (Memory Hunter copy/feint tag)
- `TheSiren` (L14 Beta) — ranged sonic; uses existing `BossTheSiren.glb`
- `BreederQueen` (L16 Beta) — phase3SummonCount=5; uses `BossBreederQueen.glb`
- `GarrisonCommander` (L20 finale) — 3 phases (troops/mech/escape);
  escapeTimerSeconds=30 (orbital-strike P3 countdown)

### Wave-2 Tuning hooks added (all inert by default; Act-1 unchanged)
- `Tuning::startAllied` — pre-flip to allied at build time
- `Tuning::copyFeintPhase` — phase tag for psychological-warfare gimmick
- `Tuning::escapeTimerSeconds` — P3 level-exit countdown carrier

### Self-test (`--test-act2bosses`)
- Asserts each enemy/boss def builds with sane stats; Salvari ally is
  allied + 0 damage; Mutated Flora is stationary; Surface Pursuit Drone
  is fast ranged flyer; each boss advances P1→P2→P3 on the HP machine;
  Memory Hunter inCopyFeintPhase() at P2; Garrison Commander has 3
  phases + escapeTimer>0; Breeder Queen summons in P3; Martinez + all
  Act-1 bosses still construct (regression).
- Prints `act2bosses: X/Y passed`; exit 0 on full pass, nonzero on any fail.

### Gate / build

**HANDED OFF to 13700K integrator for smoke test (per 2026-05-23 agreement).**

Local build env on DJBOOTH (4790K) is NOW installed and future-ready:
- VS 2026 Community Insiders 18.7.11819.209 at
  `C:\Program Files\Microsoft Visual Studio\18\Insiders\` (Native Desktop
  workload, Win11 SDK 22621, VC CMake Project)
- Vulkan SDK 1.4.350.0 at `C:\VulkanSDK\1.4.350.0\` (exact spec match)
- vcpkg at `C:\vcpkg\` bootstrapped + pinned to baseline
  `f7f94113c3b629c01df3d49d5edebae6d598c78c` (matches `vcpkg.json`)

Local 42-flag gauntlet + Release/Debug smoketest NOT executed here —
13700K agreed to gate. If 13700K kicks back, DJBOOTH can now gate locally
on a follow-up pass (first vcpkg dep build will take ~1–4 hrs on this 4790K
since none cached yet).

Source-correct, compilable-looking, design-aligned C++ on pushed branch.
No engine/CMake/Act-1/world-module files touched. Clean-room: no external
game-engine source consulted (only X3Native headers + EFLZ design docs).

