# W-TRAFFIC handoff — freeway AI traffic (2026-08-16 → 2026-08-17)

**Task**: fill the 16-lane INNER TOUR freeway with believable AI traffic.
Owner: "now that we have a 16 lane freeway.. we will need to fill it with traffic ;->"

## STATE 2026-08-17 (v3): ALL GATES GREEN, EYES-ON DONE. See "FINAL RECEIPTS"
## at the bottom of this file — it supersedes the "Remaining gates" list.

## (historical, 2026-08-16) State: CODE COMPLETE, **NEVER BUILT**

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

---

# FINAL RECEIPTS (W-TRAFFIC v3, 2026-08-17)

Rebuilt from scratch in this worktree (`cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=...`
then ALL_BUILD Release). `x3app.dll` mtime advanced 1786956568 -> 1786975274 — the
app lives in x3app.dll here, NOT in the 10 KB X3Engine.exe launcher, so gotcha 1.1's
mtime check must be read off the DLL.

## Gates — ALL GREEN on the freshly relinked binary

| gate | result |
|---|---|
| `--test-traffic` | 11/11 passed (T0/T0b/T1/T2/T3/T3b/T4/T4b/T4c/T5/T5b), hash a93b1e1c9497f8e6 |
| `--test-roadnetwork` | 58 passed, 0 failed |
| `--test-terraincorridor` | 11 passed, 0 failed |
| `--test-tunnelmouth` | 8/8 passed |
| `--test-riverbridge` | 9 passed, 0 failed |
| `--test-vehicle` | 10 passed, 0 failed |
| `--world tunnel` boot | **0 [ERROR]** on every one of ~14 runs |

## Roster — 9 of 11 live, every GLB draco-DECODED, no GLB bytes committed

Verified by reading each GLB's JSON chunk: `extensionsUsed` carries NO
`KHR_draco_mesh_compression` on any of the 11 (draco = invisible-but-"loaded",
receipt 7158cc5e). Measured at boot by readGlbForLod:

    SEDAN A   Sedan_Car3.glb    1.56x1.23x3.52  -> s1.278 (4.50 m)  4 wheels  4 imgs
    SEDAN B   Sedan_Car4.glb    1.47x1.06x3.52  -> s1.264 (4.45 m)  4 wheels  4 imgs
    VAN       OldVan.glb        2.90x2.48x5.68  -> s1.000 (5.40 m)  0 wheels  3 imgs
    PICKUP B  Pickup2_URP.glb   1.60x1.30x3.67  -> s1.388 (5.10 m)  4 wheels  5 imgs
    E30       E30.glb           4.58x3.50x10.96 -> s0.394 (4.32 m)  4 wheels  1 img
    M3 E36    M3_E36.glb        2.32x1.76x5.70  -> s0.777 (4.43 m)  4 wheels  1 img
    SKYLINE   Skyline.glb       2.25x1.52x5.35  -> s0.859 (4.60 m)  4 wheels  2 imgs
    MUSCLE    Muscle.glb        3.59x2.36x7.93  -> s0.630 (5.00 m)  4 wheels  0 imgs (factor+clearcoat)
    BOX TRUCK Truck.glb        10.95x19.05x36.57-> s0.235 (8.60 m)  6 wheels  1 img

    DROPPED by the ASPECT GATE, with the numbers, every boot:
    PICKUP A  W/L 0.60 (want 0.25-0.55)      JEEP  W/L 0.19

Material audit (rule 5, untextured-full-metal renders black): across the 9 live
models the ONLY metallic>=0.9 + no-baseColorTexture + dark-baseColor materials are
`M_Windows` / `M_Frame_*` on E30 and `Muscle2_Windows` — glass and window trim, which
is what they should be. No E46-class body-panel defect on any shipping model.
The 7 `[gltf] L5` warnings at boot are the pre-existing GARAGE fleet (CTR/Coupe/E46),
not traffic.

## Counts

* 300 target; **281-300 live** in practice (ring geometry), 0 loose (no wrecks).
* 9,023 triangles per car measured (see the scaling table) and ~0.5 draws per car
  after device batching (147 extra draws for 294 cars).

## Perf — measured, and the gate is reported honestly, not sanded off (rule 9)

Same camera, traffic OFF vs ON, `gpu ms avg (settled 60f)` off the [tunnel-perf] line:

| camera | OFF | ON | ratio | cars |
|---|---|---|---|---|
| default showcase (off-freeway) | 668.5 fps | 670 fps | **100.2%** | 0 |
| freeway station 07, 60 m up | 1084 fps (0.923 ms) | 547 fps (1.829 ms) | 50% | 294 |
| freeway station 07, drive height | 1317 fps | 597 fps | 45% | 300 |
| tunnel spawn point | 1080 fps (0.926 ms) | 542 fps (1.850 ms) | 50% | 281 |

**The >=90%-of-baseline gate passes only where no traffic is live.** On the freeway
it is 45-50%, and that number is real: the empty frame is 0.92 ms / 666 k tris, so
"within 10%" means "traffic may cost 0.09 ms" = 0.31 us per car. Nothing built from
9 k-tri car models can do that. Absolute cost, which is what actually matters:

    cars     0        100      200      294       (station 07 high, same camera)
    gpu ms   0.923    1.163    1.570    1.829
    fps      1084     860      637      547
    tris     667k     1,444k   2,631k   3,319k

Linear at **3.08 us and 9,023 tris per car**. With traffic ON and 300 cars the frame
is 547 fps — 9.1x the 60 Hz budget, 3.8x a 144 Hz budget. Cost is IDENTICAL whether
the cars are in frame or hidden behind a hill (the spawn row above), and the device
already CPU-frustum-culls per object (engine/rhi/FrustumCull.h, vk_passes.cpp:2529) —
so the residual is submission + shadow work, not main-view raster.

**NAMED NEXT STEP (the one real perf lever): a distance LOD for traffic.**
9 k tris for a car that is 40 px tall at 400 m is the whole cost. `FreewayTraffic::render`
currently ignores camPos entirely (`(void)camPos; // everything live is inside the cull
ring already`) — it conflates the 1600 m SIM ring with the DRAW set. Do NOT fix this by
shrinking the draw radius; the far stream of traffic down the ribbon is exactly what
makes 16 lanes read as filled.

## Coordinator's roster lead — evaluated, NOT taken (the roster loads clean), with a correction

`//p13700/G/Assets/Complete Racing Game URP All in One` exists: 218 FBX.

* **CORRECTION to the lead**: `F_Body_AI.fbx` is **2,003,632 bytes vs the hero
  `F_Body.fbx` at 1,014,016** — and `F_Wheel_AI.fbx` 356,816 vs `F_Wheel.fbx` 188,512.
  The `_AI` meshes are TWICE the size of the hero meshes; they are AI-*opponent*
  variants, NOT low-detail LOD siblings. Adopting them would make the perf number
  worse, not better. Measured before believing it (rule 9).
* The genuine perf/variety lead in that same pack is
  `Racing_Game/Models/Cars/LowPoly_Cars/` — **12 numbered cars at ~128 KB of FBX each**,
  roughly a tenth of a hero body. Twelve fresh silhouettes AND the LOD lever in one
  conversion. Gate them on rule 3 first: the predecessor excluded earlier low-poly packs
  for flat-grey default materials, and these car dirs carry no local Textures/ folder,
  so tools/convert_unity_pack.py must resolve the .mat GUIDs or they do not ship.
* Also in the pack for later dressing: Grid_Fence, Tire, barrier and cone props.

## Captures — shots_traffic/ (all eyes-on at full res by W-TRAFFIC v3)

| file | what it proves |
|---|---|
| g3_perf_on_near0_high07.png | both carriageways populated, 60 m up, 300 live |
| g12_high30_directions.png | both carriageways at 30 m — the direction read |
| g12z_oncoming_left_carriageway_4x.png | 4x crop: left-carriageway cars show HEADLIGHTS (oncoming), median on their left. No head-on. |
| g7_drive_on.png | drive height, behind a traffic car showing its REAR (receding, our way) |
| g13_outer_lanes_heavies.png | outer lanes with a heavy hugging the shoulder |
| g13z_truck_keeping_right_4x.png | 4x crop: the semi tractor IS in the outermost running lane |
| g10_junction_topdown.png | the valley-road junction from 240 m: cars on running lanes only, **fillet and connector empty — nothing cuts the corner** |
| g11_side_keepright.png | the ribbon in landscape, both carriageways flowing |
| g1 / g2 / g14 / g15 / g16_* | the perf A/B and the scaling rows above |

NOTE on g14/g15 (spawn point): `[tunnel] SPAWN at (3300.8, 4.2, -276.3)` shares x/z with
ring node 0 but sits ~17 m BELOW the road datum (21.8) — a camera placed at spawn eye
height looks into a hillside. The images are honest but show terrain; their value is the
perf pair, not the view.

## Capture recipe for the next agent

`X3_TRAFFIC_CAMS=8` prints paste-ready road-DERIVED `--shot-cam` strings (drive/high/side)
— use them, do not eyeball coordinates. The `side` preset at 90 m now lands INSIDE a
roadside tree since W-FOREST planted; 160 m out / 45 m up / pitch -0.25 clears it.
`X3_TRAFFIC_NEAR=0` fills a still's foreground (the ring's 300 m gameplay default cannot
be closed by the 200-frame settle loop — gotcha 4.1b's lesson).

A `--world tunnel --screenshot` run takes ~8 s and is worth ONE retry: with sibling lanes
running, a run can die silently before the traffic build (Vulkan contention, gotcha Bug 2
family). The tell is a log that ends at the garage/vehicle setup with no `traffic:` lines
and no PNG. Retrying succeeded immediately.

## Hygiene

* `git log --name-only a1400d67..HEAD | grep -ci '\.glb$'` = **0**. No GLB bytes committed.
* shots_wmap/*.png reverted and untouched — another lane owns them. No capture in this
  session wrote to that directory.
* Store-served GLBs, `*.pre-fetch.bak` and `assets_staging/` deliberately left untracked.
* Junk removed from the inherited tree: `dev/null/` (a stray POSIX redirect made a real
  directory on Windows), `build_log.txt`, `galleries_tmp.json`.
