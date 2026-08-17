# W-TRAFFIC handoff — freeway AI traffic (2026-08-16, mid-task cap)

**Task**: fill the 16-lane INNER TOUR freeway with believable AI traffic.
Owner: "now that we have a 16 lane freeway.. we will need to fill it with traffic ;->"

## State: CODE COMPLETE, **NEVER BUILT** — next step is the first compile.

```
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake   # FRESH worktree dir (gotcha 1.4)
cmake --build build --config Release          # ALL_BUILD, never --target X3Engine (gotcha 1.1)
# verify build/bin/Release/X3Engine.exe mtime ADVANCED
```
Expect first-compile errors in app/traffic.cpp (written blind, never compiled).

## What exists (all committed in this worktree)

* **engine/physics/IPhysicsWorld.h + JoltPhysicsWorld.cpp** — NEW:
  `addKinematicBox` / `moveKinematic` / `makeBodyDynamic` (default-implemented
  virtuals so monster.cpp's CountingPhysicsWorld keeps compiling; Jolt
  overrides). Kinematic boxes are created `mAllowDynamicOrKinematic` on the
  Dynamic layer; `makeBodyDynamic` rescales shape mass props + SetMotionType.
* **app/traffic.h / app/traffic.cpp** — `FreewayTraffic`:
  - Lane geometry from `buildRoadRenderPath` + `computeMedianPlan` (NEVER
    re-derived). lat>0 = right of +u travel = (-tz,+tx) (road_network P()).
  - DIRECTION LAW: right cw (lat>0) travels +u, left cw travels -u → median
    on driver's LEFT both ways. Lane 0 = median-side fast lane, 7 = outer.
    laneLat() = sgn*(medianHalf + kFwyPavedHalfM - kFwyRunningHalfM + (lane+.5)*3.6576).
  - Constant-time-gap follower (7 m + 1.6 s), accel +2.5/-6.5, hard
    no-overlap invariant pass after integration.
  - Spawn ring 300-1500 m, cull 1600, target 60, xorshift-deterministic.
  - Kinematic box per car, moveKinematic pre-step; contact callback (host
    owns it) converts to dynamic at impulse >= 4000 (drum pattern); loose
    cars get a rule-11 push-UP clamp vs terrainHeightAtWorld.
  - Models MEASURED at load via readGlbForLod (CPU, hierarchy-correct bbox);
    uniform rescale to per-model real-car target length; groundLift = -minY*scale
    (contact plane on lane); DROP + logWarn on draco/degenerate/X-oriented.
  - Wheels: drawables under Wheel_*/wheel_*/Tire nodes (not Steer*) spin about
    model-X at their own hub; radius = hubY - minY (measured); theta = fmod
    over circumference; models without wheel nodes don't spin (by rule).
  - PAIRED: kTrafficPaveProud 0.02 ↔ road_network.cpp kPaveProud (:2531).
* **--test-traffic** wired (cli.h/cli.cpp/test_registry.cpp): T0 register,
  T1 no head-on (measured displacement), T2 median-on-left (measured, via
  CarState cx/cz), T3 gap never negative + follower yields, T4 ring fill/cull
  + determinism hash, T5 heavy trucks lanes>=5. Headless (no device/phys).
* **host_tunnel.cpp** wired: build after riverLife (X3_TRAFFIC default ON,
  =0 off; needs ringOn); update before phys->step in BOTH interactive + the
  settleAndGrab capture loop (gotcha 4.1b analog); render in interactive,
  pause, and headless frames; shutdown + contact-callback clear on both exits.
* **app/CMakeLists.txt**: traffic.cpp added.

## Vehicle roster (audited — tools/traffic_roster_audit.py prints all of this)

SHIPPED (assets/converted_glb/Vehicles/Traffic/, store-published, manifest
committed; draco-decoded by tools/traffic_decode_batch.py):
* Sedan_Car3.glb / Sedan_Car4.glb — textured body (doors/wheels are factor
  mats — EYES-ON pending, doors may read primer-grey; if so drop or repaint)
* OldVan.glb — fully textured, metre-scale, NO wheel nodes (no spin, allowed)
* Pickup2_URP.glb — textured body; wheel nodes lowercase `wheel_BL(.001)`

From the proven RCC fleet (already store-served): E30, M3_E36,
Skyline_by_BUMSTRUM (near-perfect scale), Muscle, Pickup, Jeep, Truck (6-wheel
box truck, heavy class). All factor+clearcoat materials, proven in-engine.

EXCLUDED with receipts: Coupe (exports 13 cm long — toy scale), E46_New
(black-panel full-metal mats, the 7 L5 clamp warnings), F1 (open-wheeler),
CTR (hero + 155k tris), CompactCar1993/MiniCargoTruck/BankTruck/GarbageTruck
(armory: flat-grey default materials or 1/40 scale — rule 3),
TruckLevo (skinned + no textures), all lowpoly/toon packs.

NOTE: RCC GLBs measure oversized in naive scans (M3 5.7 m, Muscle 7.9 m…) —
that's why per-model targetLenM rescale exists in kTrafficModels. Skyline/CTR
measure correct. VERIFY the readGlbForLod measurements in the boot log
("traffic: <label> measured ...") against the table on first run.

## Remaining gates (NONE done yet)
1. Build green; fix compile errors.
2. `--test-traffic` green; also roadnetwork/terraincorridor/tunnelmouth/riverbridge.
3. Boot `--world tunnel` zero [ERROR]; check `tasklist //FI "IMAGENAME eq X3Engine.exe"` FIRST.
4. Captures via `--world tunnel --screenshot t.png --shot-cam "x,y,z,yaw,pitch"`
   (settle loop already ticks+renders traffic). Camera positions: pick freeway
   stations from the boot log / ringSpec nodes; drive-height shot = station y+1.2.
   EYES-ON: both carriageways populated CORRECT DIRECTIONS, truck keeping right,
   junction not driven through fillets, per-model close-ups (esp. the 4 armory
   models' materials + orientation — OldVan nose direction is UNVERIFIED).
5. Perf: [tunnel-perf] lines print per capture; measure X3_TRAFFIC=0 vs 1 at
   spawn; gate >= 90% baseline fps.
6. Commit receipts; DO NOT push (session lead merges).

## Gotchas rediscovered this session
* Armory galleries: http://localhost:8787/galleries.json (22 MB, slow) →
  root D:\Assets\_glb is directly readable on disk.
* ALL armory vehicle GLBs are draco → decode or the loader silently draws
  nothing (tools/traffic_decode_batch.py is resumable).
* Sandbox: git commands mentioning `origin/integration/complete` are blocked
  (string filter) — use the SHA (tip was 8645406a).
* assets_staging/traffic/ holds the decoded-but-not-shipped candidates
  (BankTruck, MiniCargoTruck, CompactCar1993, ModernSports5, Car7/12/23,
  GarbageTruck) — untracked, keep out of git.
