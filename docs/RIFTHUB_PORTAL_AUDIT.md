# RIFT HUB — location, reachability, portal test matrix, shared-tech audit

*Lane: `audit/rifthub-portals` off `origin/integration/complete` @ d9fcb934, 2026-08-16.*
*Tim's ask: "look into the Rift Hub... where it is, how to get to it from the main game..
and test all its portals" + "We need the same technology in all the worlds."*

---

## 1. WHERE IT IS, AND THE IN-GAME PATH (no gap — it is reachable by playing)

The hub is **SUB-LEVEL R1 of the canon world**, 78 m under the tower
(`app/app_run.cpp:1375` `kRiftFloorY = -78.0f`). It is NOT `--world rifthub`-only;
the W-RIFT fold made it a real region of canonlevel
(`app/app_run.cpp:1341-1347`: "It was reachable only via `--world rifthub`. It is
now SUB-LEVEL R1"). The dev flag remains a supported shortcut per
`docs/design/WORLDS.md` §DEV SHORTCUTS.

**The player's path, all in-world:**

1. **Learn the code.** A maintenance HoloTerminal in the Security room (fallback
   Research → Main Hall) carries LOG 41: *"CABIN OVERRIDE: 4790. RIDE DOWN. TAKE
   THE HALL WEST, THEN LEFT."* — `app/app_run.cpp:2404-2437` (riftLore build +
   text). The code is FOUND, not handed over (the cell-terminal 1278 rule).
2. **Unlock R1 in the elevator.** The cab panel hides the R1 row until 4790 is
   keyed: `app/elevator.h:253-264` (`kRiftAccessCode = "4790"`, `setRiftStop`,
   `unlockRift`), `app/elevator.cpp:793-802` (code accepted → row unlocks),
   `app/elevator.cpp:349` ("sub-level R1 (RIFT) is now a selectable floor").
3. **Ride down, walk the approach.** A bored landing at the shaft + industrial
   corridor that turns a corner into the hub's doorway (`RiftDepths`,
   `app/app_run.cpp:2356-2366`); the hub itself is the SAME `Rifthub` module the
   dev world builds, authored at a region origin (`app/app_run.cpp:2341-2354`) —
   one build path, no copy.
4. **Walk into an open gate and it TAKES you** — live traversal in the canon loop:
   `app/app_run.cpp:9216-9271` (trigger fires the kawoosh; `traversalPortal` →
   `rifthub.destination()` → `riftDestination()` resolver → `setBodyPosition`,
   HUD "RIFT TRAVERSED -> <place>", 3 s cooldown). Gates aimed at a *separate
   world* (console-retargeted) do a real world LOAD (`hc.switchWorldTo`,
   `app/app_run.cpp:9250-9260`) — same path as the world menu; nothing lies.

Headless capture hooks for all of it: `X3_RIFT_OPEN`, `X3_RIFT_TARGET=<g>=<dest>`,
`X3_RIFT_TRAVERSE=<g>`, `X3_RIFT_LOOK` (`app/app_run.cpp:2440-2447, 4858-4922`).

## 2. THE REGISTRY (destinations), AND WHAT THE 8 GATES POINT AT

The old "44-world registry" is now the **destination registry**:
`app/destinations.cpp:71-136` — **50 rows** in 6 groups (Hub 1, Facility 8,
Underworld 6, Planet 4, Echo Harbor 1, Dev Worlds 30). It is self-testing against
the LIVE dispatch tables (D0-D11, `runDestinationsSelfTest`): every `worldFlag`
must be dispatched (D2), every dispatchable world must have a row or a reasoned
exclusion (D7, negative-controlled by D10), product floor of six worlds pinned
(D11). **No dead entries exist on this branch** — `--test-rifthub` runs it live:
`destinations: 12/12`, inside `rifthub: 33/33`.

The physical hub has **8 Stargate gates on a 14 m ring** (`app/rifthub.cpp:614-621`
seeds; keys `club crystal crash city river ridge f1 f7`), re-aimed in canonlevel at
the same 8 canon anchors (`kCanonDest`, `app/app_run.cpp:2391-2400`). The rift
console (walk up, [E]) can retarget any gate at ANY registry row
(`Rifthub::setDestination`, `app/rifthub.cpp:3263` — canonicalises to registry keys,
refuses junk out loud).

## 3. PORTAL TEST MATRIX (Release, headless, X3_MUTE=1)

Traversal = real code path via `X3_RIFT_TRAVERSE=<g>` on `--smoketest --world
canonlevel --screenshot` (boot canonlevel, fire gate, settle OPEN, traverse,
resolve, land the camera). All runs: **exit 0, allocationCount=0**.

| Gate | Destination (registry row) | Resolver leg | Result | Evidence (log line) |
|---|---|---|---|---|
| 1 | Club 1127 (`club`) | lazy-build club + spawn | **PASS** | `TRAVERSED gate 1 -> Club 1127 at (13.6, -798.8, 3.8)` |
| 2 | The Crystal Veins (`crystal`) | strata offshoot pocket | **PASS** | `TRAVERSED gate 2 -> The Crystal Veins at (22.0, -153.8, -59.0)` |
| 3 | The Crash Site (`crash`) | streamed terrain anchor | **PASS** | `TRAVERSED gate 3 -> The Crash Site at (140.0, 0.4, 205.0)` |
| 4 | The City (`city`) | streamed city region | **PASS** | `TRAVERSED gate 4 -> The City at (-200.0, 18.2, 425.0)` |
| 5 | The River Valley (`river`) | river-spline bank | **PASS** | `TRAVERSED gate 5 -> The River Valley at (433.6, 1.9, -344.9)` |
| 6 | The Cliff Ridge (`ridge`) | terrain-height ring search | **PASS** | `TRAVERSED gate 6 -> The Cliff Ridge at (-608.1, 174.3, -121.0)` |
| 7 | Facility F1 (`f1`) | floor lobby | **PASS** | `TRAVERSED gate 7 -> Facility F1 - Detention at (22.0, -1.0, -25.0)` |
| 8 | Facility F7 (`f7`) | floor lobby | **PASS** | `TRAVERSED gate 8 -> Facility F7 - Executive at (22.0, 89.5, -26.0)` |

**Separate-world leg** (console retarget → world LOAD), `--test-worldswitch <flag>`
(headless canonlevel boot → live teardown → target world boot):

| Target | Result |
|---|---|
| `rifthub` (dev hub world) | **PASS** exit 0 |
| `echotropolis` | **PASS** exit 0 |
| `space` | **PASS** exit 0 |
| `strata` | **PASS after fix** — world booted fine but exited 1 on a dead-clone absolute fallback screenshot path (`host_strata.cpp:64`, `C:/GameDev/X3Native-engine/...`); same bug class `host_club.cpp:325` already fixed. Fixed this lane. |
| `streamed` | **PASS** exit 0 |
| `gallery` | **PASS** exit 0 |

**Refusal honesty:** `X3_RIFT_TARGET=1=gallery` + traverse in the capture path →
`gate 1 -> 'gallery': a separate world - it must be LOADED, not walked to — the
gate holds` (exit 0). In live play the same gate performs the world load.

**Dead entries: none.** Rows `crash`/`city` have no `--world` flag by design
(canon anchors only); D3/D7 police the rest.

## 4. THE LOOK (frames read by a human-proxy, not relayed blind)

`--smoketest --world rifthub --screenshot` (idle) and `X3_RIFTHUB_OPEN=1` (open):
riveted gunmetal torus gates, blue plasma membranes (nebula idle vs spiral throat
open — clearly different states), heat-shimmer columns off every gate, hanging
holo consoles with legible multi-color readouts, polished plank floor carrying
real specular reflections of the membranes, cool striplit industrial ceiling.
**The July "we made it look GOOD" build survived the folds intact.** Frames
committed: `docs/screenshots/rifthub/hub_idle.png` + `hub_open.png` (joining the
July art-pass series already in that directory). Arrival frames spot-read:
river-valley terrain+sky good; club arrival is a dark interior frame (streamed
lights not in frame budget — logs are the truth per the capture-blindness rule).

## 5. "SAME TECHNOLOGY IN ALL THE WORLDS" — the madness check

**Dev shell (console ~ / ESC pause / F3 FPS, `HostShell`):** 23 of 24 world hosts
in `app/world_hosts/` wire it (`shell.attach(hc)`), incl. `host_rifthub.cpp:216`.
The default host (canonlevel/intro/level1/elevator/terrain/ocean/fromdoc/
spacestation) registers the SAME shared engine-console set + campaign hooks
(`app/app_run.cpp:7246-7289`). **The one hold-out is `host_echotropolis.cpp`** —
it re-implemented the whole shell by hand: own `IConsole` + Hud front-end + its
own command set (`amb/haze/todspeed/bloom/speed/sens/sun/vol/...`), own ESC pause
menu, zero calls to `registerEngineConsole` — so Echo Harbor lacks the standard
`noclip/god/help/flightmode` set and every future shell improvement forks. This
is exactly the re-make-madness Tim named.

**Grounding (`0cbe3f89`, `app/grounding.h` + `surface_type.h`):** the shared rule
lives in the CHARACTER MODULES (`monster.cpp`, `rescue.cpp`, `sarah.cpp`), so any
world that spawns cast through Monster/Rescue/Sarah inherits it — which covers
canonlevel and every portal-arrival anchor (they are all inside canonlevel).
NOT covered: `crowd.cpp` (club + Echo Harbor civilians — flat `m_cfg.groundY`,
fine on flat interiors, wrong the day a crowd stands on terrain) and
`host_gallery.cpp` (empirical per-model floor offsets, its own comment admits the
+1.03 hack).

**Facing/pose-release (`2058bc8a`):** landed in `monster.cpp`/`canon_play.cpp`
shared paths — same inheritance story as grounding: all Monster consumers get it,
crowd characters have their own separate animation path.

**The gate itself was broken:** `--test-grounding` (the 38-check gate from
`0cbe3f89`) was UNWIRED on this branch — merge `58eb79b3` (inspx/la-exe →
playtest-0814) dropped the `cli.cpp` + `test_registry.h` wiring in its KEEP-BOTH
resolution, leaving `runGroundingSelfTest()` with zero callers. The convention
survived; the *enforcement* did not — the exact drift class this repo keeps
hitting. Restored this lane.

### Gap list, ranked by fix cost

| # | World / module | Missing tech | Cost | Status |
|---|---|---|---|---|
| 1 | test harness | `--test-grounding` gate unwired (merge 58eb79b3) | 3 files, ~15 lines | **FIXED this lane** |
| 2 | `host_strata.cpp:64` | dead absolute fallback screenshot path → healthy world exits 1 | 1 line | **FIXED this lane** |
| 3 | `destinations.cpp` club row | said "Y=-200"; club moved to Y=-800 in July (`club1127.h:82`) | 1 string | **FIXED this lane** |
| 4 | `host_echotropolis.cpp` | bespoke console/pause/FPS; no `registerEngineConsole` → no noclip/god/help; forks every shell improvement | ~1-2 h (register shared set on its existing console, or port to HostShell) | **FILED** — big host, own input loop, not trivial wiring |
| 5 | `crowd.cpp` | flat `groundY`, ignores `groundCharacter()` | ~1 h (probe at spawn/waypoint assign) | **FILED** — behavior-neutral on today's flat floors, so not urgent |
| 6 | `host_gallery.cpp` | empirical floor offsets instead of the shared grounding probe | ~30 min | **FILED** |
| 7 | `level_loader.cpp:1037`, `world_map.cpp:163`, `world_stream.cpp:108/123` | more `C:\GameDev\X3Native-engine` legacy absolute paths (these look like ordered-fallback lists, so likely benign; verify before touching) | audit ~30 min | **FILED** |

## 6. Gate results (wiped dir, Release + Debug, X3_MUTE=1)

Release: `--smoketest` (default = canonlevel) exit 0 · `--smoketest --world
canonlevel` exit 0 · `--smoketest --world rifthub` exit 0 — all
`allocationCount=0`. Suites: level1 **21/0**, grounding **38/0** (gate restored
this lane), canonplay **13/0**, canonlevel **16/0**, rifthub **33/33** (incl.
destinations 12/12 + riftconsole 6/6). `--test-worldswitch strata` exit **0**
after the host_strata path fix (was 1). Pre-existing failures verified BY NAME,
unchanged: `--test-ai` 12/1 `Tf nav-wired enemy routes AROUND a wall`,
`--test-phase2b` 5/1 `Td Door E closed until boss dead`. Debug: smoketest
default + rifthub + grounding — exit 0, 0 VUID, allocationCount=0.
