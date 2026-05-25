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

---

## STATUS — DJBOOTH (2026-05-23) — Act-2 mid biomes (L12-15)

- **Branch:** `feat/act2-caves` (pushed to origin)
- **HEAD:** `53e58f15a418ac16b4bc97fcdf33f7aed939d358`
- **Files changed (vs origin/main):**
  ```
   app/CMakeLists.txt |   1 +
   app/act2_caves.cpp | 822 ++++++++++++++++++++++++++++++++++++++++++++++++++++
   app/act2_caves.h   | 361 +++++++++++++++++++++++
   app/main.cpp       |  17 ++
   4 files changed, 1201 insertions(+)
  ```
- **Module sizes:** `app/act2_caves.h` ~361 lines (public API + `Act2CaveAreaPlan`
  + `PoisonHazardZone` + `CrystalHeartChamber` + self-test prototype);
  `app/act2_caves.cpp` ~822 lines (build/tick/onTrigger/onFire/draw +
  reachability helpers + `runAct2CavesSelfTest` running TWO full builds with
  24 assertions covering every spec gate).
- **Touched ONLY:** `app/act2_caves.{h,cpp}` (new) + `app/main.cpp` (test flag +
  include) + `app/CMakeLists.txt` (source list). `act2_world.*`, `act2_desert.*`,
  Act-1 files, `monster.*` — UNTOUCHED.

### Levels authored (L12-15)
- **L12 Advanced Cave System** — 4 hostile cave-fauna (`NativeDesertFauna`
  re-tinted cave-blue), 3 allied Salvari Archives markers (`SalvariAlly` row),
  the **Crystal Heart Chamber** (dual-gate: strength AND hack required; story-
  branch latch on activation), the **Memory Hunter** boss in the abyss arena
  (uses `act2BossTuning(MemoryHunter)` so `copyFeintPhase>0` is wired by the row).
- **L13 Toxic Swamplands Edge** — 5 stationary `MutatedFlora` lashers +
  `PoisonHazardZone` AABB present-but-inert-until-entered (mirrors
  `act2_world.HazardZone` exactly; same `kPoisonExposureRate = 8.0f` so the HUD
  reads consistently across Act-2 hazards).
- **L14 Research Station** — 4 `MutatedScientist` (ranged chemical attackers)
  ALWAYS present; **Beta Siren ambush** placed only when `setSirenAmbushGate(true)`
  was called before `build()` (i.e. F2 women lost in Act 1). The host flips this
  flag before build; the test rebuilds both halves to exercise both branches.
- **L15 Tree Cities** — 3 canopy platforms at RISING Y heights (climb route
  graybox) + a trading-post pillar prop on the top platform (host wires E-interact
  to `tradingPostPos()`).

### Trigger range (non-colliding)
- Reserved **100..108** (`kAct2CavesTrigBase`, 9 ids). Below: Act-1 L1Trigger
  (10..29), SpireMid hubs (30/40/50), Act-2 host (80..82). 83..99 left as the
  safe gap for `act2_desert.*` (L10/L11) — next machine's lane.
- IDs: `L11toL12Portal=100`, `L12CrystalHeartRoom=101`,
  `L12MemoryHunterArena=102`, `L12toL13Transition=103`, `L13PoisonHazard=104`,
  `L13toL14Transition=105`, `L14SirenAmbush=106`, `L14toL15Transition=107`,
  `L15TradingPost=108`.

### Self-test (`--test-act2caves`) — 24 assertions, 2 builds
C0 module builds; C1 all 4 levels named/objective'd/implemented; C2-C5 per-level
plan shape (footprints/counts/flags); C6 Memory Hunter present with
`copyFeintPhase>0`; C7 Salvari Archives allied + 0 dmg; **C8-C13 Crystal Heart
gates** (inert at load -> stays inert on one gate -> ARMS on both -> `activate()`
latches `activated+storyBranch` -> idempotent); **C14-C16 L13 poison hazard**
(present+inert at load -> stays inert outside -> arms+exposure climbs inside);
**C17+C23 timeline-gated Siren** (flag=true => Siren placed; flag=false => NO
Siren but scientists still there); C18 Tree Cities 3 rising platforms + trading
post; C19-C21 reachability latching + `allTransitionsReachable()`; C22 trigger-id
non-collision range check. Prints `act2caves: X/Y passed`; exit 0 on full pass,
nonzero on any fail.

### Gate / build — **READY FOR INTEGRATION** ✅ (gated locally on DJBOOTH 2026-05-24)

Local gate executed on DJBOOTH (4790K / 1080Ti) — full chain green:

| Phase | Result |
|---|---|
| Configure (`cmake --preset windows-vs2026`) | exit 0 — 9 vcpkg ports installed (binary cache), Vulkan 1.4.350 found |
| Release build (`cmake --build --preset windows-vs2026`) | exit 0 |
| **Full `--test-*` gauntlet — 52/52 PASS** (every flag parsed in `app/main.cpp`) | all exit 0 |
| `--test-act2caves` (this task's self-test) | **PASS in 28.9s — `act2caves: 24/24 passed`** |
| Release `--smoketest` | exit 0, **VUID mentions = 0** |
| Debug build (`cmake --build build --config Debug`) | exit 0 |
| Debug `--smoketest` | exit 0, **VUID = 0**, **`live allocationCount=0 (expect 0)`** |

Gauntlet flag list run (52, derived from `else if (a == "--test-*")` in main.cpp):
`--test-jobs --test-asset --test-console --test-physics --test-gltf --test-player --test-interact --test-pickup --test-combat --test-audio --test-level1 --test-phase2a --test-phase2b --test-anim --test-terrain --test-terrainplace --test-streaming --test-ai --test-bestiary --test-bosses --test-act2bosses --test-spiremid --test-nexus --test-spiretop --test-dronehack --test-sublevels --test-act2 --test-act2caves --test-doorcode --test-elevator --test-elevatorfsm --test-net --test-netsync --test-netinterp --test-netpredict --test-rescue --test-destruction --test-debris --test-gpuskin --test-collapse --test-physjoint --test-ragdoll --test-nav --test-weapons --test-vehicle --test-footik --test-ui --test-saveload --test-valley --test-cliffs --test-club --test-locomotion`

After `--test-gltf` regenerated `docs/GLB_IMPORT_REPORT.md`, the file was reverted via `git checkout -- docs/GLB_IMPORT_REPORT.md` (no spurious diff in this branch).

Source-correct, compilable-looking, design-aligned C++ on pushed branch. No
engine/CMake-shader/Act-1/Act-2-world/Act-2-desert/`monster.*` files touched.
Clean-room: only X3Native headers + EFLZ design docs in `docs/design/`
(`X3_WORLD_BLUEPRINT.md` §4.2, `EFLZ_WORLD_STRUCTURE.md` L12-L15 rows,
`EFLZ_BESTIARY.md`). G:\ TASK_3 file not present on this box — design docs
served as the source of truth. No RBDOOM / id Tech / Doom / Quake source
consulted.

---

## HARDWARE — DJBOOTH snapshot (2026-05-24)

Tag for fleet comparison: **garage 4790K / 1080Ti / Z97**. Formerly the
Club 1127 DJ booth PC. Sits on the Blue Toolbox (one of many).

| Component | Value |
|---|---|
| **CPU** | Intel i7-4790K (Haswell, 2014) — 4C/8T, 4.0GHz base, LGA1150 |
| **Motherboard** | ASUS Z97-PRO GAMER (Z97 chipset, **NOT** a newer board — original era) |
| **BIOS** | AMI v2107, dated 2015-11-10 (eligible for update; mobo last firmware was 2018) |
| **RAM** | 32GB DDR3-1866 = 4×8GB. 2×G.Skill F3-1866C10-8GAB + 2×Crucial BLS8G3D1609DS1S00 (DDR3-1600 sticks running at 1866 via XMP) |
| **GPU** | NVIDIA GeForce GTX 1080 Ti, 11GB GDDR5X. Driver 32.0.15.6094 (2024-08-13) |
| **Monitors** | 2× Dell U2719D / U2719DX (27", 2020 mfr), both **2560×1440** @ 59Hz |
| **Storage** | C: Samsung 970 EVO Plus **2TB NVMe** (888GB free) · D: Samsung 850 EVO M.2 **500GB SATA** (434GB free). Earlier "3TB NVMe" was a misremember — actual total is 2.5TB SSD across the two drives |
| **NIC** | Intel I218-V onboard, link 1 Gbps, 802.3 |
| **WAN throughput** | **239.35 Mbps ↓ / 138.74 Mbps ↑** to Google (Phoenix, Optimum Online, 6 ms ping, jitter 55↓/276↑) — fresh 2026-05-23 speedtest. ⚠️ Fleet TODO: move ALL PCs onto the **1200 Mbps fiber** — DJBOOTH is currently ~20% of target down, ~12% up. Cat6 fleet upgrade in flight (predictable gigabit LAN between PCs soon, so the WAN gap stops bottlenecking inter-machine syncs) |
| **OS** | Windows 10 Pro 22H2 (build 19045), installed 2022-04-05, last-boot uptime ~272 hrs |
| **Power plan** | High performance |

### Build env installed on DJBOOTH (2026-05-23)
- `C:\Program Files\Microsoft Visual Studio\18\Insiders\` — VS 2026 Community Insiders 18.7.11819.209 (Native Desktop workload, Win11 SDK 22621, VC CMake Project)
- `C:\VulkanSDK\1.4.350.0\` — exact spec match
- `C:\vcpkg\` — bootstrapped + pinned to baseline `f7f94113c3b629c01df3d49d5edebae6d598c78c`
- `C:\Program Files\GitHub CLI\gh.exe` — authed as `1GreenNinja`
- `D:\GameDev\X3Native` — local clone

### Fleet-relevance notes for the integrator
- **First-time vcpkg dep compile on this 4790K** is the slowest gauntlet step in the fleet — expect ~1–3 hrs for the 9 ports (joltphysics dominates). All later builds reuse cached binaries.
- **Vulkan 1.3 feature support on GTX 1080 Ti**: per `BUILD.md`, expected to pass `set_required_features_*` checks; if not, `init()` logs which selection failed.
- **2× 27" 1440p panels** = comfortable side-by-side dev layout (code on one, debugger / RenderDoc on the other).
- **Wired 1 Gbps NIC** is fine for LAN/Git, but WAN bandwidth (239/138 as of 2026-05-23) still bottlenecks first-time git clones of larger feature branches + any vcpkg binary-cache pulls vs. the 1200 Mbps fiber target. **Cat6 fleet upgrade in flight** to put predictable gigabit LAN between all dev PCs, so the WAN gap stops affecting inter-machine syncs even before the fiber move.

---

## STATUS — feat/portal-hub (2026-05-23, agent session)

**Branch:** `feat/portal-hub` (forked from `feat/act2-caves`).
**Deliverables landed (clean-room — NO RBDOOM / id Tech / Doom / Quake source consulted):**
- NEW `app/rifthub.{h,cpp}` — portal-hub module: 8 cosmetic emissive rifts (one per known `--world` target), trigger id range 200..207 (clear of all existing ranges), `Rifthub::onTrigger` latches per-portal activation + logs the relaunch hint, `Rifthub::hudPromptForEye` emits a 5 m HUD prompt.
- EDIT `app/main.cpp` — added `--world act2caves` direct-boot block (mirrors `--world valley` shape: flat 200x200 m physics ground at the L12 spawn elevation + `Act2Caves::build()` + first-person controller + LMB -> `onFire` + trigger forwarding + headless screenshot path + full cleanup) AND `--world rifthub` block (first-person walk of the portal ring + nearest-portal HUD prompt + headless 3/4 vantage screenshot). Also extended the `--world` doc comment with the known-value list.
- EDIT `app/CMakeLists.txt` — appended `rifthub.cpp` to the X3Engine source list.

**Build/gate:** GATE PENDING — toolchain ready (VS 2026 Insiders + Vulkan SDK 1.4.350.0 + vcpkg `f7f9411` all installed on DJBOOTH), NOT YET RUN this session per the integrator's instruction (user is waiting for the deliverable). Fire the gate with the standard preset:
```powershell
$env:VCPKG_ROOT="C:\vcpkg"; $env:VULKAN_SDK="C:\VulkanSDK\1.4.350.0"
$cmake="C:\Program Files\Microsoft Visual Studio\18\Insiders\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
& $cmake --preset windows-vs2026; & $cmake --build --preset windows-vs2026
& "$PWD\build\bin\X3Engine.exe" --world rifthub --screenshot agent_rifthub.png
& "$PWD\build\bin\X3Engine.exe" --world act2caves --screenshot agent_act2caves.png
```

**Portal layout (clockwise from +X, radius 14 m):** position 0 = `act2caves` (violet), 1 = `act2` (orange-amber), 2 = `valley` (cyan), 3 = `cliffs` (white-gold), 4 = `club` (magenta), 5 = `destruct` (red), 6 = `ragdoll` (green), 7 = `terrain` (sky-blue).

**Future task (not in this draft):** turn `Rifthub::onTrigger` into an actual in-process world swap (tear down hub physics+scene, build the linked world's host) so the player doesn't have to relaunch — the trigger id -> world-name table already in `rifthub.cpp` is the pivot.
