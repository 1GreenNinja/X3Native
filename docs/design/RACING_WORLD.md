# RACING WORLD — street/freeway racing mode

**Status: DESIGN ONLY. Nothing in this document is built.**
Internal design doc, private repo. Written 2026-08-01 from a read-only sweep of `dev-main` + `origin/echotropolis`.
Companions: `VEHICLE_UPGRADES.md` (the upgrade system), `VEHPARTS_FORMAT.md` (the parts data format), `X3_WORLD_BLUEPRINT.md` §1 (the world gazetteer).

Every claim about what exists carries a `file:line`. Claims about `origin/echotropolis` are cited as
`origin/echotropolis:path:line` — that branch is **not** merged into `dev-main`.

---

## 0. THE VISION, AND THE ONE UNRESOLVED WORD

Tim's brief:

> racing takes place *"on the streets of the planet where the freeways go in tunnels over rivers and under mountains — the BL version"*

### 0.1 "the BL version" — ANSWERED BY TIM (2026-08-13)

**BL = BABYLON. It is the X3BabylonJS version — the world X3Babylon was built in.**

Tim, verbatim: *"BL BL is Babylon3d Version, wichi s what X3Babylon's world had been
made in!!"* and *"Yeah, you may even fin screeenshots of it.. thgey looked good, but
were a slide show, which is why we made X3Native."*

So "the BL version" of the freeway network means THE BABYLON WORLD'S ROAD LAYOUT —
the authored ring and its tunnels/bridges — not a code lineage. That world looked
right and ran like a slide show; X3Native exists to run it properly. This also
resolves the `BL_WORLD_PORT.md` provenance notes, which cite BL as the source of
the corridor-depression technique and the tunnel cross-sections.

DECIDED STRATEGY (Tim, same session): port the AUTHORED ROAD RING, REGENERATE the
buildings. Do not port BL's building placement.

KNOWN BL-ERA DEFECTS not to reproduce: a huge white tank popping in and out near
the city centre; interiors existed but were lit badly; the elevator shaft never
rendered correctly.

The search below is left in place as the record of how the question stood open,
and as a caution: the answer was never discoverable from the repo. It had to come
from Tim.

<details><summary>Original investigation (nothing in the repo could answer it)</summary>

**I could not determine what BL refers to. Do not let anyone guess it into the design.**

What I searched and what I found:

| Search | Result |
|---|---|
| `BL` as a whole word, every tracked text file (`.md .json .py .txt .bat .h .cpp`) | 4 hits, all **"bottom-left" vertex comments**: `app/club1127.cpp:929`, `app/club1127.cpp:1955`, `app/intro_orchestrator.cpp:1681`, `app/world_hosts/host_showroom.cpp:403` |
| `BL` as an identifier | `app/holo_terminal.cpp:355` — `const Ink BL = kBlue;` (a local colour alias) |
| `BL` on `origin/echotropolis` | `origin/echotropolis:app/world_hosts/echo_region_builders.cpp:1022` — `struct BL { float sx,sz,dx,dz,len,speed; int n; };`, a local **Boat Lane** struct for the harbour fleet |
| All 1615 commit subjects **and bodies**, every ref (`git log --all --format='%H|%s|%b'`) | **zero** occurrences of `BL` as a word |
| All branch names and tags | zero (`feat/world-blueprint` and `origin/docs/public-fleetcommand` match only as substrings) |

</details>
| Level / world / region / asset names (`assets/levels/`, `assets/world/`, `app/world_hosts/`) | zero |

The nearest-looking artefact is **not** a world: `Escape48BLN`, a folder in Tim's OneDrive that holds the Task9D Spire model (`docs/design/X3_WORLD_BLUEPRINT.md:14`, `app/level_loader.cpp:1032`, `docs/KNOWN_BUGS.md:18`). `BLN` ≠ `BL`, and it names a *directory*, not a place in the game.

**Working hypotheses, none confirmed, listed only so Tim can say "yes, that one" or "none of those":**
1. A variant of one of Tim's own Babylon.js source modules (`x3-city-roads.js`, `x3-freeway-tunnels.js`, `x3-world-city.js`) that lives in `…\OneDrive\GameDev\Q3Engine\src\` and is not in this repo — `app/city.h:6-7` names those files as the content reference.
2. A real-world place. Precedent exists: Club 1127 **is** Tim's real Miami auto shop (`app/club1127.cpp:631`, `:773`, `:1287` — "Late Night Speed"), and `docs/design/narrative/chat_trees/club1127_vesper.md:8` calls it "Tim's real Miami club, ported".
3. A build/level revision tag from outside this repo.

**Action: ask Tim before anyone authors a route.** Everything below is written so that the *route data* is swappable — if BL turns out to be a specific city or a specific JS module, the route table changes and nothing else does.

### 0.2 What the vision maps onto that DOES exist

The three signature features are each already named in the canon, and each is currently a stub:

| Vision element | Canon reference | Reality today |
|---|---|---|
| "freeways go in tunnels … under mountains" | `docs/design/X3_WORLD_BLUEPRINT.md:48` — "Freeway Tunnels \| through mountains \| 4 named bores \| ✅" | **Fake.** 4 named plans exist (`app/city.cpp:53-57`) but each renders as a portal frame box + an 18 m sunken throat box (`app/city.cpp:386-411`). The code admits it: *"the old full-length 200 m surface bore rendered as a giant wall across the map; the logical bore length stays in the plan for gameplay/tests"* (`app/city.cpp:400-402`). You cannot drive into one. |
| "over rivers" | THE RIVER — a real authored spline that **carves the heightfield** (`app/terrain.cpp:527-534`, carve at `app/terrain.cpp:664-701`), 34 m half-width (`app/terrain.h:130`), bed 3.2 m below water (`app/terrain.h:131`) | **No bridge exists, anywhere in the repo.** `viaduct` and `culvert` return zero hits. The single `bridge` mention in the terrain is `app/terrain.cpp:448`: *"coast-spur road z=500 >=320 m; NO road crossing => NO bridge needed."* The river was routed to **avoid** needing one. |
| "the streets of the planet" | Scrapyard City / New District / Industrial Zone at (−600,500)/(200,500)/(−200,350) (`app/city.cpp:395-405` pads, `app/city.h:15`) | 52 authored road segments, each an **axis-aligned box** (`app/city.cpp:138-145`), no collision (`app/city.cpp:73`), 0.08 m proud of a **midpoint-sampled** terrain height. |

So the vision is 100% consistent with the design canon and 0% built.

---

## 1. INVENTORY — VEHICLE / DRIVING

### 1.1 The physics car — real, and better than expected

`DriveDemo` (`app/vehicle.h:43-139`) wraps a genuine Jolt `VehicleConstraint` + `WheeledVehicleController` (`engine/physics/JoltVehicle.cpp:174`, controller cast `:180`), registered as both a constraint and a step listener (`:178-179`). Wheels are **raycast** contacts (`JPH::VehicleCollisionTesterRay`, `engine/physics/JoltVehicle.cpp:176`), cast against the Dynamic layer so they hit static terrain (rationale `engine/physics/IVehicle.h:111-117`). Chassis is 1.68 × 1.0 × 3.9 m, sized to the hero GLB (`app/vehicle.h:117`). Roll protection: `mMaxPitchRollAngle = 60°` (`engine/physics/JoltVehicle.cpp:100`).

Layout: front steered, rear powered + handbraked (`app/vehicle.cpp:107-108`). Handbrake torque is `maxBrakeTorque × 2.5` on handbraked wheels (`engine/physics/JoltVehicle.cpp:121`), ratio preserved through live retune (`:311-312`).

**Traction control is the only game-layer handling logic** (`app/vehicle.cpp:254-277`): takes max longitudinal slip across wheels (`:267-268`), and above `kSlipTarget = 0.10` trims throttle by `1 − 4.0(slip − 0.10)` clamped to `[0.15, 1.0]` (`:269-274`). The comment explains why (`:257-263`): without it a 700 Nm RWD car sits in a torque-independent burnout plateau and **every power upgrade would be invisible**. TC off is documented as "full burnout mode" (`app/vehicle.h:97`).

**Live retune with no constraint rebuild** — `applyTuning(WheeledTuning)` (`app/vehicle.h:91-93` → `engine/physics/JoltVehicle.cpp:270-318`): torque, redline, torque curve (≤8 points, `:281-287`), mass via `ScaleToMass` (`:290`), grip scale (`:296-301`), suspension freq/damp, ride height (clamped `min ≥ 0.03`, `:305`), brake torque. Idempotent — everything scales from baselines captured at build (`:131-142`).

### 1.2 What the physics car does NOT have

| Missing | Evidence |
|---|---|
| Any drift model — no slip *angle* (only longitudinal slip, `engine/physics/JoltVehicle.cpp:258-265`), no rear-grip cut, no counter-steer assist, no drift scoring | `VEHICLE_UPGRADES.md:124` names it as the single biggest unbuilt feel change |
| Downforce / aero | `VEHICLE_UPGRADES.md:64` — "the one physics param the current system lacks entirely" |
| Gear ratios / shift RPM / switch time / manual shift / clutch input — only `mClutchStrength` is set | `engine/physics/JoltVehicle.cpp:150`; zero hits for `mGearRatios`/`mShiftUpRPM` in `engine/physics/` |
| `gear()` has an implementation and **zero callers** | `engine/physics/JoltVehicle.cpp:254-256` |
| LSD / torque split / AWD — one hardcoded open differential over the first two powered wheels | `engine/physics/JoltVehicle.cpp:155-165` |
| Anti-roll bars (Jolt supports `mAntiRollBars`; never populated) | zero `antiroll` hits in `engine/physics/` |
| **Per-surface friction of any kind.** No asphalt/gravel/wet distinction at any layer | zero friction API in `engine/physics/IPhysicsWorld.h` or `JoltPhysicsWorld.cpp`; grip is one scalar `gripScale` on Jolt's default tyre curves (`engine/physics/JoltVehicle.cpp:124-127`) |
| Per-wheel differential grip — baselines are cached from **wheel 0 only** | `engine/physics/JoltVehicle.cpp:139-142` |
| Vehicle respawn / reset-to-road / flip recovery | no such path exists; a flipped car's only recourse is the 60° clamp |
| Speedometer, tacho, gear indicator, boost gauge, nitrous bar | zero hits repo-wide; `HudModel` (`app/ui.h:260-338`) has no vehicle fields |

### 1.3 `app/world_cars.{h,cpp}` — the in-world car, not traffic

This is the **canon-world** car layer: 9 authored parked cars (`app/app_run.cpp:3365-3389`), direct-draw visuals + one static Jolt box each, deliberately not scene entities so streaming ledgers can't destroy the shared GLB (`app/world_cars.h:10-18`). **Exactly one live drivable rig**, built lazily on first entry and parked in a limbo slab at (4000,−600,4000) on exit (`app/world_cars.h:19-25`, `app/world_cars.cpp:313`). Hold-E hack for locked cars, 3.0 s, 2.6 m reach (`app/world_cars.h:64-65`), unlock persists across region rebuilds (`:191`). Deep water kills the engine and force-exits into swim (`app/world_cars.h:40-42`, `app/world_cars.cpp:356-367`).

API: `interact` (`:115`), `driveInput`/`preStep`/`postStep` (`:127-129`), `driverCamera` (`:132`), `forwardSpeed`/`engineRPM` (`:135-136`), `nearestCar(x,z)` (`:170`).

### 1.4 `app/vehparts.{h,cpp}` + `app/perfshop.{h,cpp}` — the economy already exists

11 categories / 36 parts in `assets/vehicles/parts.json` (`x3.vehparts/1`), parsed by `Catalog` (`app/vehparts.h:102-104`), composed to `x3::phys::WheeledTuning` and lowered onto the live Jolt car. Full category table at `VEHICLE_UPGRADES.md:14-26`. Baseline: 700 Nm / 6500 rpm / 1300 kg / 2200 Nm brakes (`app/vehparts.h:88-96`).

- **Wallet**: `int credits = 12000` on `VehicleBuild` (`app/vehparts.h:139`).
- **Spending works**: purchase debit (`app/perfshop.cpp:546`), 70% trade-in on the replaced part (`:549`), insufficient-funds check (`:543`), repair charge (`:595-596`), nitrous refill `max(50, refillCost)` (`:604-612`).
- **Nothing awards credits.** Grep of `app/*.cpp` for `credits` finds only spends and an unrelated ATM hack that accumulates into a local test int (`app/hackables.cpp:230,239`). This is the single cleanest hook a race mode plugs into.
- **Persistence**: `VehicleBuild::toJson/fromJson/saveFile/loadFile` (`app/vehparts.h:147-150`) → `vehbuild.json` beside the checkpoint save (`:145-146`, path `:154`); host saves on `consumeNeedSave()` (`app/perfshop.h:95`).
- **Knock model + dyno**: ECU boost/fuel/timing sliders, knock index → `knockLimit` → LIMIT-POP → ×0.85 power until `repairCost` is paid (`app/vehparts.cpp:405-426`, `app/vehparts.h:194`).

`VEHICLE_UPGRADES.md:3` — *"performance layer SHIPPED, cosmetic layer NOT BUILT."* §5 of that doc is the handling-feel list; item 3 is *"Pursuit — cop AI, heat levels, roadblocks, spike strips. Nothing exists; this is the biggest net-new system."* (`VEHICLE_UPGRADES.md:126`).

### 1.5 `--world drive` (`app/world_hosts/host_drive.cpp`, 731 lines) — the racing prototype that already runs

This host is much closer to a racing game than anything else in the repo:

| Feature | Line |
|---|---|
| Streamed terrain, residency radius **6** tiles, upload budget 64 | `app/world_hosts/host_drive.cpp:71-72` |
| Hero-car GLB skin (`Vehicles/CTR.glb`) with graybox fallback | `:116` |
| Late Night Speed perf shop: parts catalog + persisted `VehicleBuild` + lift-pad detection + dyno + tune sliders | `:128-139`, `:440-477` |
| **Nitrous on LEFT SHIFT**, draining the tank by `fdt` | `:491-499` |
| **Speed FOV**: 70° → 84° approaching 33 m/s (~120 km/h), dt-smoothed at 6/s | `:548-555` |
| **Banking chase cam** via `setCameraBasis` — `kBank = 0.30` rad gain, `kMaxBank = 0.20` rad (~11.5°), `kBankLerp = 5/s`, scaled by `min(|v|/20, 1)` | `:614-657` |
| Engine audio: pitch `0.65 + 1.15·rpmFrac + 0.15·throttle + exhaustPitchOffset`, volume shaped by exhaust timbre | `:574-582` |
| SC whine layer; turbo spool + whistle + **blowoff "psshh" on lift above 55% spool** | `:583-603` |
| E to step out to an on-foot walker and back | `:415-438` |

**The architectural problem:** `host_drive.cpp` (the feel) and `app/world_cars.cpp` (the world you actually play) share only `DriveDemo`. The canon world has **no** nitrous binding, **no** perf shop (`PerfShop` is never constructed in `app_run.cpp`), **never calls `applyTuning`** (so parts have zero effect on the car you drive in the game), no banking cam, no speed FOV, and normalises RPM by a hardcoded 6500 (`app/world_cars.cpp:401`) instead of the tuned redline.

**A racing mode should be built on the `host_drive` lane and should merge that lane back into `world_cars`, not fork a third one.**

---

## 2. INVENTORY — ROAD / WORLD GENERATION

### 2.1 `dev-main` has no road graph

`app/city.{h,cpp}` is immediate-mode geometry with zero retained topology. The entire public road API is a counter: `roadSegmentCount()` (`app/city.h:72`, backing field `:84`). A road is one axis-aligned box:

```cpp
// app/city.cpp:138-145
auto addRoad = [&](float x0, float z0, float x1, float z1, float halfW) {
    const float cx = (x0 + x1) * 0.5f, cz = (z0 + z1) * 0.5f;
    const float hx = std::fabs(x1 - x0) * 0.5f + ((x1 == x0) ? halfW : 0.0f);
    const float hz = std::fabs(z1 - z0) * 0.5f + ((z1 == z0) ? halfW : 0.0f);
    float s[3]; placeOnTerrain(cx, cz, s);
    addBoxProp(cx, s[1] + 0.08f, cz, hx, 0.09f, hz, roadCol, nullptr);
    ++m_roadSegments;
};
```

Consequences: terrain is sampled **once at the segment midpoint**; conformance is faked by chopping runs into ≤36 m pieces (`app/city.cpp:146-154`); a diagonal road would degenerate (no `halfW` on either axis) — none are authored, so it never fires; **roads have no collision** (`app/city.cpp:73` — `(void)physics; // blockout-plus city is visual-only this pass`). Cars drive on the terrain heightfield *under* the road and will hit a 0.08 m lip at every 36 m segment boundary.

52 segments total, spanning X ∈ [−775, +650], Z ∈ [+80, +580]. Junctions are not computed — 8 traffic lights are hand-placed at fudged +5.5/+8.5 offsets (`app/city.cpp:289-292`) and store no position.

**This is a decoration layer, not a route source.**

### 2.2 `origin/echotropolis` — `EchoRoads` is the real spine

**Not merged.** Files: `app/world_hosts/echo_roads.{h,cpp}` (220 + 1986 lines), plus `echo_regions`, `echo_region_builders`, `echo_heightfield`, `echo_water`, `echo_woodlands`, `host_echotropolis.cpp` (4643 lines). This branch was **never fetched into the local clone** until this analysis; `git fetch origin echotropolis:refs/remotes/origin/echotropolis` is required before anyone can read it.

**Header self-description** (`origin/echotropolis:app/world_hosts/echo_roads.h:2-7`): *"Tim's order: 'Curving freeway aerial structures... interchanges... streets that flow around the harbor in nicely angled grid sections'. Target bar: GTA5-class freeway GEOMETRY (curved banked deck, barriers, lane paint, pillar rows, trumpet interchanges)."*

**The data model** (`echo_roads.h:120-153`):

```cpp
enum class RoadClass : uint8_t { Freeway = 0, Ramp, Avenue, HarborStreet };

struct RoadSample {
    float x, y, z;      // centerline point (world; y = finished deck/tarmac)
    float tx, tz;       // unit tangent (XZ)
    float bank;         // radians; + = right edge dips (superelevation)
};
struct RoadNode { float x, z; };   // graph endpoints (POSITIONAL joins)
struct RoadEdge {
    RoadClass cls; float width; int lanesF, lanesB;
    uint32_t a, b;                      // a==b for the closed ring
    std::vector<RoadSample> center;     // arc-length-even samples (~4 m; 2 m ramps)
    float length;
};
struct RoadGraph {
    std::vector<RoadNode> nodes; std::vector<RoadEdge> edges;
    static float laneCenterOffset(const RoadEdge&, int lane, bool forward);
};
```

`laneCenterOffset` (`echo_roads.cpp:538-542`) is `((lane + 0.5) * kLaneWidth)` signed by direction, `kLaneWidth = 3.4` (`:57`). It **ignores `e`** — no clamping against `lanesF`/`lanesB`; the caller must clamp. Consumers apply it along the right-perp `(tz, −tx)`.

**Per-class recipe** (constants `echo_roads.cpp:50-124`):

| Class | width | lanes F/B | banked | barriers | piers | resample | max turn |
|---|---|---|---|---|---|---|---|
| Freeway | 14 m | 2 / 2 | yes | yes | pair, ±4.8 m | 4 m | 0.8 °/m (r ≈ 72 m) |
| Ramp | 7 m | 1 / 0 (spur 1/1) | on-ramp only | yes | single | **2 m** | 1.2 °/m (r ≈ 48 m) |
| Avenue | 9 m (blvd 12.5) | 1 / 1 (blvd 2/2) | no (`bank = 0`) | no | none | 4 m | 2.5 °/m (r ≈ 23 m) |
| HarborStreet | 9 m | 1 / 1 | no | no | none | 4 m | 2.5 °/m |

**THE ZIGZAG LAW** (`echo_roads.cpp:1306-1455`) — Tim's ruling, 2026-07-27: *"No roads can ever be zig zag."* A universal final geometry pass that runs after every authoring/probe/inset pass and before junctions read the centrelines.

- Metric: worst per-sample heading change per metre, `acos(t_i · t_{i+1}) · 57.29578 / step` (`:1337-1350`), tangents from central differences (`:1327-1336`).
- Cure ladder per edge: 2× moving-average smooth over a ~40 m window with the first/last 2 samples **pinned** (junction handoffs must not move) (`:1356-1372`); ground classes **re-seat** onto the heightfield afterwards, decks keep their graded `y` (`:1374-1377`); escalation doubles the window up to **320 m** (`:1394-1400`); then one decimate-refit (every 4th sample, piecewise-linear, deliberately not a spline) to kill sample-scale oscillation (`:1401-1428`).
- Failure: with a 15% grace, the edge is **DELETED** (`:1429-1437`) — *"a missing road beats a zigzag"*.
- `lawExempt` marks intentional tight loops; only one site uses it on this branch, the 300° mine cul-de-sac (`:1128`). The retired trumpet curl used to need it; the replacement CA sweep on-ramp passes the law unaided (`:944`).
- Logs: `[roads] curvature: worst <w> deg/m on class <c> edge <ei>` per edge, then `[roads] zigzag law: PASS|FAIL (N edges dropped)` (`:1442-1454`).

**Superelevation** (`echo_roads.cpp:829-836`, recomputed post-law by `rebank` `:1378-1392`):
```cpp
const float cross = p.tx*q.tz - p.tz*q.tx;
ring[i].bank = clamp((cross / (2*step)) * kBankPerKappa, -kBankMax, kBankMax);
```
`kBankPerKappa = 55`, `kBankMax = 0.10 rad` (~5.7°) (`:78-79`), then two passes of a 3-tap box smooth.

**Elevation independence — the freeway deck already leaves the terrain.** This is the most important existing capability for the vision.

```cpp
// echo_roads.cpp:216-222
inline float deckFloor(const Heightfield& hf, float x, float z) {
    float g = hf.heightAt(x, z);
    for (int s = 0; s < 4; ++s)                       // 4-tap max over ±9 m
        g = std::max(g, hf.heightAt(x + (s%2 ? kRidgeProbe : -kRidgeProbe),
                                    z + (s<2 ? kRidgeProbe : -kRidgeProbe)));
    return std::max(g, 2.0f) + kDeckClearance;        // ground + 11 m, never below y = 13
}
```
Then a **bidirectional grade-limited relaxation** — "a rubber band under tension" (`:682-722`): a forward sweep caps descent, a backward sweep caps ascent, both at `kDeckMaxGrade × kSampleStep = 0.22 × 4 = 0.88 m` per sample, each wrapped twice so the result is start-sample independent. Both sweeps only ever *raise*, so the clearance invariant holds by construction. Explicitly no box smoothing — *"it re-inflated the valleys; profile smoothness comes from the grade bound itself"* (`:717-719`).

Plus a **V5 rim-edge inset** (`:723-820`): deck samples hanging more than 25 m over air inside the 620 m rim zone migrate inboard in 6 m hops (cap 120 m), and the displacements are **low-passed along arc length** (±80 m, 2 iterations) before being applied — raw independent hops were the mid-air right-angle switchbacks Tim outlawed (`:735-739`).

Piers (`:1800-1820`): every 70 m, skipped where deck-to-ground ≤ 3 m, a **pair** ±4.8 m off centreline for freeway and a single for ramps. `pillar()` (`:500-524`) emits a footing pad, a **tapered shaft** in 1–3 sections by height, and a **hammerhead cap beam** transverse to the deck. Deck gets a real box section — fascia walls + a soffit 1.8 m below the top (`deckFascia`, `:434-479`).

**Junctions** (`:1471-1560`): Pass A captures stop-short endpoints against ground-edge corridors (`d < 0.70 × mean width`) and **extends** the stub into the target corridor in 4 m steps; Pass B finds interior crossings by global closest approach (`< 0.55 × mean width`); candidates within 14 m merge. A junction is a filled **12-gon patch** at the highest participant's `y` (`:1873-1908`), ribbons trim to `r − 1.2 m`, stop bars and zebra crosswalks are emitted at the keep-transitions (`:1666-1704`). **Freeway edges are skipped — grade-separated** (`:1473`).

**Collision export** (`RoadCollisionMesh`, `echo_roads.h:166-169`): asphalt/concrete tops of every class + freeway shoulders + junction patches (`:1715`, `:1730-1732`, `:1889-1907`). **Not** included: barriers, curbs, sidewalks, piers, paint. *Nothing stops a car leaving the deck laterally.*

**Determinism**: zero `rand` — one hash `h01()` keyed on edge index and arc bucket (`:184-187`). The whole network is ~45–50 edges from hardcoded tables: a 6-waypoint fixed arc (`:170-174`) + ≤10 radially-probed rim waypoints (bearings 320°→140° in 20° steps, `:164-169`), 2 gates (`:840-845`), 9 shore seeds (`:1150-1154`), 5 harbour blocks (`:1246-1250`), 2 spur avenues, a mine spur.

**TUNNELS: absent, categorically.** `grep -ciE "tunnel|bore|portal|underground|subway"` over `echo_roads.cpp` returns **0**. The module's entire vertical vocabulary is *conform to ground* (Avenue/HarborStreet) or *elevate on piers* (Freeway/Ramp). Where terrain rises, the deck rides `terrain + 11 m` — it goes **over**, never through.

**BRIDGES OVER WATER: absent.** Water is only an avoidance predicate (`kWaterMinLand = 1.5`). The boulevard sets back 42 m from a bisection-located waterline (`:1156-1174`) and nudges inland in coves (`:1195-1199`); harbour streets are **truncated** at the first wet sample (`:1286-1290`); the Urban gate ground node walks up the terrain gradient so ramps don't descend into the sea (`:862-874`). **The network retreats from water; it never crosses it.**

### 2.3 Traffic AI on `origin/echotropolis` — the racing-line substrate

`host_echotropolis.cpp` wires `EchoRoads` fully (the header's "UNWIRED" note is stale): `build()` at `:984-990` (hoisted above the region boot so placements can audit corridors), `collisionMesh() → addStaticMesh` at `:2240-2249` (*"Tim: 'you can fall through' the bridge"*), `lights()` merged night-gated into the 64-light cap at `:4164-4176`, `draw()`/`drawNightGlow()` at `:4227-4228`.

"LANE 1 TRAFFIC v1 — whole-graph kinematic routing" (`:1287-1470`):

```cpp
struct TrafficLeg   { uint32_t edge; bool fwd; float len; bool exitSignalled; };
struct TrafficRoute { std::vector<TrafficLeg> legs; float total; };
```

- **Adjacency is derived, not stored** (`:1400-1421`): because `RoadNode`s are positional joins with no shared indices, nodes within 10 m are clustered into supernodes, `adj[]` is built from those, and `juncNodes` = supernodes of degree ≥ 3.
- **Routes are seeded random walks** (`:1429-1459`): xorshift keyed `ci * 2654435761u | 1`, up to 10 legs / 1500 m, avoiding immediate backtrack.
- **`poseCar`** (`:1328-1391`) is kinematic — no physics body. Distance is integrator **state, not `f(t)`**, *"because a red signal genuinely holds the car"*. Class speed multipliers: **Freeway 1.0, Ramp 0.55, street 0.45** (`:1352-1354`). Signals hold within 14 m of a signalled exit on a global 10 s cycle (even edges green 0–5 s, odd 5–10 s), and a hack forces all-red gridlock (`:1355-1360`). Lateral placement is `laneCenterOffset` applied along `(tz, −tx)` (`:1373-1375`).
- Gaps: cars have **no collision** with each other or the barriers; signals are a global clock, not per-junction; lane index is `(ci & 1)` so at most 2 lanes are ever used; traffic never consults `corridorHits`.

### 2.4 The substrate mismatch — a first-class integration risk

`EchoRoads::build()` takes an `x3::game::Heightfield` (`echo_roads.h:184`), which is a **baked 16-bit PNG island**: 4096 m extent, 320 m height scale, sea at 0.20 normalized (`origin/echotropolis:app/world_hosts/echo_heightfield.h:26-31`, bilinear sample `:51-56`), loaded from `islandDir + "/island_height_20260530.png"` (`host_echotropolis.cpp:900`).

The canonical X3 world is a **procedural fBm heightfield** with a completely different API (`float terrainHeightAt(const TerrainConfig&, float, float)`, `app/terrain.h:193`).

`EchoRoads` only ever calls `hf.heightAt(x, z)`. **Porting it to the canonical world is a one-function adapter** — but it must be done deliberately, and the two worlds' coordinate frames, scales and sea levels do not match. Note also that `assets/island_mesa/*` and `assets/roads/*` on that branch are Git-LFS pointers (132–133 bytes), and the host's default asset path `D:/GameDev/EchoHarbor/assets/island_mesa` does not exist on this machine — `ECHO_ISLAND_DIR` must point at the in-repo copies.

### 2.5 The other track tech nobody remembers: `app/descent_slide.{h,cpp}`

The coaster/chute module contains a **generic, ride-agnostic, arc-length-sampled banked track layer**, explicitly built for reuse (`app/descent_slide.h:3-5` — *"the spline/rider tech seeds the Cedar Point 2000 coaster system, so the track definition + rider controller are DATA-DRIVEN and ride-agnostic"*).

```cpp
// app/descent_slide.h:48-56
struct TrackFrame {
    x3::phys::Vec3 pos, tan, right, up;   // right/up are the BANKED lateral frame
    float bankDeg, cumLen;
    TrackSegType type;
};
```
`TrackSegType` (`:36-44`): `Crest / Drop / Curve / Airtime / Bore / Burst / Brake` — a per-range behaviour tag that already drives rider physics. `TrackSpec` (`:60-68`) is pure data (no GPU/physics), headless-simulable. Frames are built by central-difference tangent + Rodrigues rotation of the lateral basis by `bankDeg` about the tangent (`app/descent_slide.cpp:187-214`); `kSampleStep = 2.5 m` (`:52`). Authoring is a **turtle script** — `straightPitched / pitchRamp / helix` (`:63-112`), with a "makeup spiral" that absorbs authoring drift to hit an exact end elevation (`:169-176`) — directly reusable for closing a lap.

`TrackRider` (`descent_slide.h:73-84`, sim `:255-300`) is a vehicle-on-route controller with O(1) frame lookup by arc length, a `lateral` cross-track offset clamped to ±1.35 m (the analogue of a lane offset), and speed-driven FOV.

And critically for the vision: **`Burst` segments already emit trestle columns every 7 m** with real oriented static collision (`app/descent_slide.cpp:424-446`, verts CPU-transformed by the banked basis at `:381-388`). *The engine can already build an elevated deck on supports.* `Bore` segments already build a tight enclosed tube with a roof.

---

## 3. INVENTORY — TERRAIN / STREAMING

### 3.1 The heightfield

Self-implemented value noise + fBm, not Perlin/simplex, **no domain warp** (`app/terrain.cpp:71-111`). Canonical config: 32 m tiles, 33 verts/edge (1 m cells, 2048 tris/tile), `heightScale = 55 m`, `noiseFreq = 0.0042`, 5 octaves, `seed = 1337`, `worldFeatures = true` (`app/terrain.h:59-78`, `app/terrain.cpp:827-837`).

Layers composited in order (`app/terrain.cpp:739-778`): macro relief → 4 mountain ranges (peaks 400–500 m at 7–10 km, **deliberately exceeding `heightScale`**, `app/terrain.cpp:386-391`, `:705-725`) → ocean basin to −90 m → 4 flat pads (facility r=260 @ Y=−2; Scrapyard r=250 @16; New District r=190 @15; Industrial r=150 @17, `:395-405`, blend to `r × 1.7`, `:762-772`, *"applied last so streets win"*) → authored landforms (bluff, canyon pass, 2 ravines, **the river carve**).

Query API is pure and thread-safe: `terrainHeightAtWorld` / `terrainNormalAtWorld` / `placeOnTerrain` / `worldWaterLevelAt` (`app/terrain.h:97-149`, impl `app/terrain.cpp:839-867`). Normals use the same central-diff construction as the mesher, so query and rendered surface agree by construction.

**THE RIVER** (`app/terrain.cpp:421-448` design block, nodes `:527-534`, Chaikin chain `:566`, carve `:664-701`): 9 nodes from (780, 180) at waterY +3.5 to (900, −1120) at −9.9, monotonically descending. The carve is three-stage: a floodplain shelf out to ~130 m, an authored levee holding the bank crest, then a 24 m channel cut at `waterY − 3.2` with banks over 26 m. Water surface query is unified river-then-ocean (`worldWaterLevelAt`, `app/terrain.h:149`, `app/terrain.cpp:804-818`), sea level `kWorldSeaLevel = −10` (`app/terrain.h:147`).

### 3.2 The residency ring — purely radial, no hysteresis

```cpp
uint32_t TerrainStreamer::update(Scene&, IRenderDevice&, IPhysicsWorld&,
                                 float focusX, float focusZ);   // app/terrain.h:279-280
```

**There is no velocity parameter and no lookahead field anywhere in the class** (member list `app/terrain.h:377-416`). The stream-in loop is a symmetric expanding-shell Chebyshev scan (`app/terrain.cpp:1356-1364`); nearest-first is the only prioritisation. Eviction fires the instant `|Δgx| > radius` (`app/terrain.cpp:1329-1333`) — **no residency hysteresis** (only pending *requests* get a +1 slack, `:1344-1350`).

| Host | radius | ring |
|---|---|---|
| canon world / `--world terrain` | 8 tiles | 256 m, 17×17 = 289 tiles (`app/app_run.cpp:3113-3114`, `:3429-3431`) |
| **`--world drive`** | **6 tiles** | **192 m**, 13×13 = 169 tiles (`app/world_hosts/host_drive.cpp:71`) |
| act2 world | 2 tiles | 64 m (`app/act2_world.cpp:26`) |

Generation is async (`runIO`, `app/terrain.cpp:1218`) with a budgeted main-thread drain (`:1376-1416`), but the under-focus 3×3 is **forced synchronous** on init and on every boundary cross (`:1289-1293`, `:1360-1361`) and bypasses the upload budget (`:1399-1403`).

LOD: 3 levels, thresholds 80 m / 192 m (`app/terrain.cpp:325-331`), applied per-frame by centre distance (`:1418-1429`). **All three LOD meshes are built and uploaded for every tile** (`:1120-1123`, `:1230-1234`) — 3× VRAM and 3× generation cost.

Collision is **not** a Jolt heightfield shape (zero `HeightField` hits repo-wide) — it is one `JPH::MeshShapeSettings` triangle mesh per tile from the LOD0 top surface (`app/terrain.cpp:1125-1141` → `engine/physics/JoltPhysicsWorld.cpp:378-406`). At radius 8 that is **289 static bodies ≈ 592k collision triangles resident**, and `MeshShapeSettings::Create()` (the BVH build) runs on the **main thread** inside `upload()` (`app/terrain.cpp:1235`).

The streaming self-test only validates **walking speed** — 7 m/s over 4000 frames (`app/terrain.cpp:1717-1720`).

### 3.3 The region streamer already does what terrain doesn't

`WorldStreamer::update(..., px,py,pz, vx,vy,vz, budgetMs, ...)` (`app/world_stream.h:137-141`) computes wants from `pos + vel × lookahead` and takes `min(dNow, dAhead)` (`app/world_stream.cpp:398-420`). Real load/unload hysteresis pair per region (`:421-424`), neighbour warm-preload at ×1.5 (`:402-421`), and a soft collision-floor proxy instead of a loading screen if the player outruns streaming (`app/world_stream.h:38-42`, `:450-458`).

**Driving is already special-cased**: lookahead raised to ≥4.0 s and fed the real chassis velocity rather than the chase-cam delta (`app/app_run.cpp:9843-9850`). `assets/world/regions.json:4` states the load radii were tuned for *"~40 m/s drive => 8-12 s of stream lead time"*.

So the pattern is proven in-repo; it was simply never applied to the ground.

### 3.4 Tunnels / bridges / portals — the honest answer

- **No terrain carving of any kind.** `h(x, z)` is single-valued (`app/terrain.cpp:735`). No voxel, SDF, CSG, dual-contour, hole or mask. The only subtractive mechanism is the whole-tile **keep-out rect** (`app/terrain.h:299-310`, enforced `app/terrain.cpp:1185-1190`), used exactly once for the facility apron (`app/app_run.cpp:3427-3428`), and safe there only because the facility brings its own ground.
- A CSG carve is written up as an **unimplemented upgrade path** in `app/club_bedrock.h:107-113` — the underground club is a hand-authored six-slab cavity, not a boolean.
- Underground and surface coexist only by **suppression**: all residency work is skipped below `kStreamSuppressBelowY = −20 m` (`app/app_run.cpp:1481`, gate `:9835`), a workaround the code itself names *"Risk 3 (XZ-only residency vs the underground)"* (`:9829-9833`).
- **Portals** exist in exactly one useful form: the room/doorway PVS with a frustum-directional multi-hop flood fill (`app/level_loader.h:3`, `:173-180`, consumed via `Scene::setVisibleRooms`/`roomVisible`, `app/scene.h:225-256`). It **requires authored `CanonRoom`/`CanonDoorway` data** — there is no way to author a portal in open terrain. The exterior occlusion answer is HZB.

---

## 4. INVENTORY — RACE-ADJACENT SYSTEMS

| System | State | Citation |
|---|---|---|
| **Trigger volumes** | Exist. World-space AABB + id + `fired` latch + `enabled`. `update(point)` returns ids fired this call. Deliberately a per-frame point-in-box test — *"simple, deterministic, allocation-free, and trivially testable headless"* | `app/trigger.h:5-9`, `:25-31`, `:62` |
| **Mission system** | A real JSON scripting format `x3.mission/1` — ordered stages with `objective` HUD text, `on_enter`/`on_complete` effects, `advance_when`/`fail_when` condition arrays, branching. **Trigger integration is first-class**: `{"flag": "trigger.3"}` | `missions/level1.mission.json:2`, `:6-43`, `:36`; model `app/mission.h:55-69`; flag bridge `:111-136`; runner `:155-217`; spec `docs/design/MISSION_FORMAT.md` |
| **Objectives** | Ordered label list + cursor + free-text override, renders "OBJECTIVE: …" | `app/objective.h:28-86`, `:78` |
| **Timers** | Gameplay-specific only (rescue 300 s `app/rescue.h:71`, boss escape, respawn). **No generic stopwatch/lap timer class.** The mission format has **no** time condition kind | `app/mission.h:16-19` |
| **HUD** | Two layers: dev overlay (`app/hud.h:54-98`) and production `GameHud::draw(UiContext&, const HudModel&, float)` (`app/ui.h:428`, impl `app/ui.cpp:801`) driven by a plain push model `HudModel` (`app/ui.h:260-338`) — **zero vehicle fields** |
| **Minimap / radar** | Exists as a host-filled plain-array feed: player XZ + yaw as centre/rotation, 32 enemy blips, ally blips, 16 room outlines, and — most useful — a **waypoint marker with edge-clamped off-screen chevron + distance readout** | `app/ui.h:292-338`, chevron `:325-331` |
| **Fixed timestep** | Real. `kSimHz = 60`, `kSimDt = 1/60`, aligned to Jolt's internal step; spiral clamp `kMaxAccum = 0.25` | `engine/net/SimClock.h:17-25`, `:37-47`; wired `app/app_run.cpp:9146-9148`; vehicle input sampled inside the sub-step loop `:9162-9170` |
| **Replay / ghosts** | **None.** Determinism is named as the prerequisite for *"prediction re-sim, demo replay, and reproducible tests"* but no input-log recorder exists | `engine/net/SimClock.h:9-11` |
| **AI navigation** | Bounded XZ grid + 8-connected A* + string-pull + `PathFollower`, built from **physics raycasts** not render meshes. Defaults are pedestrian: 1.8 m height, 0.4 m radius, 0.5 m step, 1 m cells, ±16 m region, `arriveRadius = 0.35 m`. `desiredVelocity` returns a planar velocity — **no steer/throttle/brake conversion, no turning-radius constraint, no reverse** | `engine/ai/INavigation.h:11-19`, `:55-63`, `:78-119`, `:138-163` |
| — and it is **not built for any live world** | the only `buildNavGridFromPhysics` call outside the engine is in a self-test | `app/monster.cpp:3646-3650` |
| **Level authoring** | Three lanes: (a) **LevelDoc JSON** — the format the native editor saves; brushes Box/Ramp/Cylinder/Stairs, entities incl. a `"vehicle"` type documented as the drivable-car hook and a `"trigger"` type with a script id + zone size (`app/editor/editor.h:24-61`, `:69-101`, loader `app/leveldoc_world.h:48-169`, samples `assets/levels/perfshop.leveldoc.json`). (b) LevelArchitect `v2.project.json` for the facility. (c) **Pure code hosts** — still dominant, 22 of them |
| — LevelDoc gaps | **No road/spline/curve primitive**; trigger zones are AABB with **no yaw**; no route or checkpoint-sequence concept | `app/editor/editor.h:71`, `:44-49` |
| **Audio** | Full system: 3D one-shots, streamed music, **looping voices with live pitch/volume** (`startLoop`, `startLoop3D`, `setLoopParams`), occlusion provider, RT acoustics | `engine/audio/IAudioSystem.h:60-155` |
| — audio gaps | **No Doppler** (no velocity parameter on any call). **No `setLoopPosition`** — `startLoop3D` fixes the emitter position once at start (`:129-130`), so a *moving* rival's engine loop cannot be repositioned. Only one engine loop sample exists in-repo (`app/world_hosts/host_drive.cpp:369-372`); exhaust tiers are pitch/timbre variants. No tyre squeal, skid, shift or backfire |
| **Decals** | RHI-level `DecalInstance{center, halfSize, normal, angle, color}` + `submitDecals` (`engine/rhi/IRenderDevice.h:1001-1008`). App-side ring is 64 entries with a 12 s life (`app/fx.h:176-177`) — fine for bullet holes, far too small/short for skidmarks |
| **Particles** | Bounded pool, additive + alpha modes, `spawnSmoke` etc. | `app/fx.h:166-234` |
| **Save/persist** | Four independent lanes, no unified profile: binary checkpoint `'X3SV'` v1 (`app/save.h:36-37`, `:73-97`, explicitly excludes physics bodies `:10-12`), StoryFlags text file (`app/story_ops.h:52`, `app/cutscene.cpp:754-777`), `vehbuild.json`, settings |

---

## 5. THE PLAN

### A. ROUTE AUTHORING

**Decision: a race route is an ordered list of `(edgeIndex, forward)` legs over a `RoadGraph`, resolved at load into a flat arc-length-indexed `RaceLine`.** This is the same shape as `TrafficRoute` (`origin/echotropolis:app/world_hosts/host_echotropolis.cpp:1292-1293`) and deliberately so — traffic and racers should share one path type.

```cpp
// proposed: app/race_route.h
struct RouteLeg    { uint32_t edge; bool fwd; };
struct Checkpoint  { float arc;            // distance along the resolved line
                     float halfWidth;      // gate half-width (m), default edge.width*0.6
                     uint32_t flags; };    // Start / Finish / Split / Optional
struct RaceRoute {
    std::string id, name;
    std::vector<RouteLeg>   legs;
    std::vector<Checkpoint> checkpoints;
    bool  closed = false;                  // true => lap; false => point-to-point
    int   laps   = 1;
    float gridSpacingM = 6.0f;             // start-grid pitch along the line
};
// Resolved at load, cached, never serialized:
struct RaceLine {
    std::vector<RoadSample> samples;       // concatenated, direction-corrected, re-arc-indexed
    std::vector<float>      curvature;     // |dtangent/ds|, computed once
    std::vector<uint8_t>    cls;           // RoadClass per sample (drives AI speed)
    float totalLength;
};
```

Why legs-over-edges rather than a free spline:
- The centrelines have **already passed the zigzag law** (`echo_roads.cpp:1306-1455`). A hand-drawn spline has not, and a race line with a 12 m kink is worse than a missing road for exactly the reason Tim gave.
- Banking, lane counts and per-class speed come along for free (`RoadEdge`, `echo_roads.h:135-142`).
- The traffic router already proves graph traversal works (`host_echotropolis.cpp:1393-1466`).
- A route is then ~20 integers, trivially diffable and reviewable.

**Resolution details the implementer must not skip:**
1. **Adjacency is not stored.** `RoadNode`s are positional joins with no shared indices (`echo_roads.h:112-114`). Any route tool must reproduce the supernode clustering the traffic router does — nodes within 10 m merge (`host_echotropolis.cpp:1401-1413`). Factor that out of the host into `RoadGraph` as `buildAdjacency(float clusterM = 10.0f)` so route validation and traffic use one implementation.
2. **Direction correction.** `RaceLine` concatenates `e.center` forward or reversed per leg; tangents must be negated on reversal (the traffic router does this inline at `host_echotropolis.cpp:1370-1373`).
3. **Junction seams.** Junction patches sit at the *highest* participant's `y` (`echo_roads.cpp:1544-1560`) and ribbons trim to `r − 1.2 m` (`:1657-1665`). The resolved line will have a small gap and a possible y-step at every junction. Resolve by inserting a 12-gon-crossing bridge segment interpolated between the two edge ends.
4. **Length bug to fix first.** `re.length = kSampleStep * s.size()` uses 4 m even for ramps sampled at 2 m (`echo_roads.cpp:1576-1578`), so **ramp lengths are double-counted** and ramp traffic advances at half rate. A race route through a ramp will mis-measure. This is a one-line fix on that branch and should land before any route work.

**Authoring UX, in build order:**
- **v1 — procedural, from the graph.** A `--race-autoroute <seed>` that runs the existing seeded walk (`host_echotropolis.cpp:1429-1459`) with a closure constraint (return to the start supernode) and rejects routes shorter than N metres or with more than K reversals. Free: the code is written. Gets 20 playable circuits on day one.
- **v2 — JSON route docs.** `assets/races/*.race.json`, format tag `x3.race/1`, mirroring `x3.vehparts/1` and `x3.mission/1`. Human-editable leg list, checkpoint arcs, laps, payout. This is the shipping format.
- **v3 — Level Architect.** The editor already has a `"vehicle"` entity type documented as the drivable-car hook (`app/editor/editor.h:28-33`) and a `"trigger"` type (`:44-49`). Add a `"route_node"` entity: click points in the viewport, snap each to the nearest `RoadEdge` sample, emit the leg list. **Do not** add a spline primitive to LevelDoc — snapping to the validated graph is both cheaper and safer.

**Lap vs point-to-point** is a single `closed` flag. Closed routes require `legs.back()` to end at the supernode `legs.front()` starts from — validate at load and refuse otherwise, with a log in the zigzag-law house style.

### B. THE SIGNATURE TERRAIN — tunnels, river bridges, mountain bores

This is the largest net-new work in the whole plan and it is where the vision lives.

#### B.1 What is already true

`EchoRoads` **already builds a road whose elevation is independent of the terrain.** The deck profile is a grade-limited relaxation over a clearance floor (`echo_roads.cpp:677-722`), piers are generated wherever deck-to-ground exceeds 3 m (`:1800-1820`), and there is a real box-section deck with fascia and soffit (`:434-479`). Half of "roads must depart from the heightfield" is done.

#### B.2 What is missing: one concept, three uses

Introduce a **signed clearance profile** as the road's primary vertical authority, replacing the current "floor + relax" that can only ever raise:

```
clearance(s) = deck_y(s) − terrain_y(s)
   clearance >  +3 m   → ELEVATED    (piers, existing)
   clearance ∈ [−1, +3] → AT GRADE   (conform, existing)
   clearance <  −1 m   → SUBGRADE    (tunnel — new)
```

The current relaxation is deliberately raise-only (`echo_roads.cpp:684-716`) so the clearance invariant holds by construction. **Replace it with a two-sided profile solver:**

1. **Author a target profile independent of terrain.** Same grade cap (`kDeckMaxGrade = 0.22` for freeway, `kRampMaxGrade = 0.07` for ramps) and same bidirectional double-wrap sweep, but seeded from *authored control elevations at route waypoints* rather than from `deckFloor`. The sweep then enforces grade continuity in both directions without a one-sided floor.
2. **Classify each sample** by the clearance test above, with hysteresis (a tunnel must be at least ~60 m long or it reads as a pothole; a bridge shorter than ~25 m reads as a bump — merge short runs into their neighbours before emitting).
3. **Emit per run type.**

#### B.3 TUNNELS (the "under mountains" half)

The road **must not** try to carve the heightfield. `h(x,z)` is single-valued (`app/terrain.cpp:735`) and there is no CSG anywhere; adding voxel/SDF terrain to get four tunnels is the wrong trade by an order of magnitude.

**Approach: the tunnel is a solid tube that occludes the terrain, plus a per-tile terrain hole.**

- **Tube geometry** — sweep a closed cross-section along the banked frame, exactly the way `descent_slide.cpp` builds its `Bore` segments (roof + walls, oriented boxes in the banked basis, `app/descent_slide.cpp:381-441`). Reuse that code path; it already CPU-transforms verts by `right/up/tan` and hands real oriented static collision to Jolt.
- **The terrain hole** — extend `TerrainStreamer::setKeepOut` (today a single axis-aligned rect, `app/terrain.h:299-310`, `app/terrain.cpp:1185-1190`) into a **list of oriented rects** registered by the road builder at boot. Tiles fully inside a hole are never generated; tiles intersecting one still generate. That is exactly the facility-apron pattern (`app/app_run.cpp:3427-3428`) and is safe under the same condition: **the tunnel must supply its own ground and its own collision across the whole skipped area**, including a soil skirt beyond the tube so there is never a void.
  - Cost note: a 32 m tile granularity means a 14 m-wide tunnel punches a 32 m hole. Accept this — the portal massing (below) covers the seam, and the alternative (per-tile mesh boolean) is a large, risky project.
- **Portal volumes at the mouths** — this is the one place the existing PVS is directly reusable. The portal system is room/doorway based and needs authored `CanonRoom`/`CanonDoorway` data (`app/level_loader.h:173-180`), but a tunnel *is* a room with exactly two doorways. Generate a synthetic `CanonRoom` per tunnel run and a `CanonDoorway` at each mouth, register with `Scene::setVisibleRooms` (`app/scene.h:225-256`), and the existing frustum-directional flood fill culls the outside world while you are inside. This is worth doing for correctness (fog, lighting, exposure) even before it pays for itself in performance.
- **Lighting inside** — the tunnel is the single worst case for the 64-light forward cap (`shaders/mesh.frag:56`). Ration it: one emissive strip running the tube length as *geometry* (zero lights), plus at most 6–8 real point lights selected nearest-first, the same pattern `EchoRoads::lights()` uses (`echo_roads.h:196`, host merge `host_echotropolis.cpp:4164-4176`).
- **The four named bores already exist as data** (`app/city.cpp:53-57`) — North/East/South/West Freeway Tunnel, each with a mouth, a unit heading and a 200 m length. Keep the names and headings; replace the two placeholder boxes (`app/city.cpp:386-411`) with real bores. That is the smallest possible delta between "the canon says ✅" and "it is true".

#### B.4 BRIDGES / VIADUCTS (the "over rivers" half)

Already 80% there. A bridge run is an elevated run whose clearance test additionally consults water:

```
isSpan(s) = clearance(s) > 3 m AND worldWaterLevelAt(x, z) > terrain_y(s)
```

`worldWaterLevelAt` (`app/terrain.h:149`, `app/terrain.cpp:804-818`) already returns river-then-ocean water surface, and the river's own spline is exported (`worldRiverNodes()`, `app/terrain.h:128-129`) so a crossing can be detected and even *placed* analytically.

- **Deck**: the existing `deckFascia` box section (`echo_roads.cpp:434-479`) is the bridge deck. No new geometry needed.
- **Piers**: the existing `pillar()` (`echo_roads.cpp:500-524`) already does footing pad + tapered shaft + hammerhead cap. Two changes: (i) **do not found a pier in water** — bias the 70 m spacing so pier stations land on dry ground either side, which for a 68 m-wide river (`kWorldRiverHalfWidth = 34`, `app/terrain.h:130`) means a single clear span; (ii) add an abutment block at each bank.
- **Deliberately choose a crossing.** `app/terrain.cpp:448` records that the coast spur was routed to avoid the river. **Reverse that decision** for the racing route: the river reach around (400–620, −300…+180) is the natural bridge site and is the single most photogenic thing the vision asks for.
- **Barriers must become collidable.** Today barriers are cosmetic and are *excluded* from `RoadCollisionMesh` (`echo_roads.cpp` collision export covers only ribbons, shoulders and patches, `:1715`, `:1730-1732`, `:1889-1907`). On a 40 m viaduct over water, a car that clips a barrier currently falls through it. Add barriers to the collision export for `Freeway`/`Ramp` classes only — streets keep the open feel.

#### B.5 Ordering constraint

The zigzag law runs **before** junction geometry is appended (`echo_roads.cpp:1306` then `:1471`), so junction extensions are never re-validated. The new vertical passes must run **before** the law, and the law's `reseat()` (`:1374-1377`) — which re-seats ground classes onto the heightfield — must be taught to skip subgrade and elevated runs, or it will slam a tunnel back onto the surface.

### C. RACE MODES — ranked by what already exists

| Mode | Net-new work | Verdict |
|---|---|---|
| **Time trial** | Route + checkpoint gates + a clock. Nothing else. No opponents, no traffic, no cops. | **Ship first.** Every other mode is this plus something. It is also the only mode that is fully testable headless. |
| **Circuit** (closed, N laps) | Time trial + lap counting + a trigger re-arm. | **Second.** `TriggerVolume::fired` is a permanent latch (`app/trigger.h:29`) — laps need a re-arm; that is a 3-line change plus a test. |
| **Sprint** (point-to-point) | Time trial + `closed = false`. | Free once circuits work. Route validation is *easier* (no closure constraint). |
| **Rival race** (3–7 AI cars) | Opponent AI (§D), position tracking, 3D moving engine audio. | Third. The AI substrate exists (§2.3) but the audio API cannot move a loop emitter (`engine/audio/IAudioSystem.h:129-130`) — that needs a `setLoopPosition` addition. |
| **Drift event** | Requires the drift layer, which does not exist at all — no slip-angle computation anywhere (`engine/physics/JoltVehicle.cpp:258-265` is longitudinal only), no rear-grip cut, no counter-steer assist. `VEHICLE_UPGRADES.md:124` calls it "the single biggest feel change". | **Do not schedule this as a race mode.** Schedule the drift *layer* as a handling feature; the event is a thin scoring wrapper on top and costs almost nothing once the layer lands. |
| **Pursuit / cops** | `VEHICLE_UPGRADES.md:126`: *"Nothing exists; this is the biggest net-new system."* Needs: AI cars that are actual physics vehicles (not the kinematic traffic poser), pursuit steering, heat state, roadblocks, spike strips, a busted state. | **Last.** Highest fantasy value, highest cost, and it depends on everything above. |

One more mode worth noting because it is nearly free: **traffic-dodge / speed-trap**, using the existing kinematic traffic as moving obstacles. It needs traffic cars to have collision (they have none today, §2.3) — which the rival-race work needs anyway.

### D. OPPONENT AI

**Do not use `engine/ai/INavigation.h`.** It is a 1 m pedestrian walkability grid with a 0.35 m arrive radius (`engine/ai/INavigation.h:60-63`, `:148`) that emits a planar velocity with no steer/throttle conversion and no turning-radius constraint. At 55 m/s a car crosses that arrive radius in 6 ms. It is also not built for any live world (`app/monster.cpp:3650` is the only call site, inside a self-test).

**Use the `RaceLine` directly.** The whole point of authoring routes over the graph is that the racing line is the route.

#### D.1 Two tiers, and be honest about which one ships

- **Tier A — kinematic pace cars (cheap, ships first).** Extend `poseCar` (`host_echotropolis.cpp:1328-1391`): distance is integrator state, lateral offset from `laneCenterOffset`, speed from class. Add curvature-limited speed and a lateral racing-line offset. Zero physics bodies, zero collision, ~microseconds per car. Good enough for a first playable and for capture/screenshot work. **Cannot be crashed into, cannot block, cannot be overtaken meaningfully.**
- **Tier B — real Jolt vehicles (the actual feature).** Each rival is a `DriveDemo` with its own `WheeledTuning`, driven by a controller that converts a target point on the racing line into `VehicleInput{throttle, steer, brake, handBrake}`. This is what makes contact, drafting and overtaking real.

#### D.2 The speed law — the data already exists

Target speed at arc `s`:

```
v_target(s) = min( v_class(cls[s]),
                   sqrt( mu_eff(s) * g / max(kappa(s), eps) ),
                   v_brake(s) )
mu_eff(s) = mu_base * gripScale * (1 + tan(bank(s)))     // banking buys grip
v_brake(s) = sqrt( v_target(s')^2 + 2*a_brake*(s' - s) )  // backward pass from the slowest point ahead
```

- `kappa(s)` — compute once at `RaceLine` build, using **the same estimator the zigzag law uses** (`acos(t_i·t_j) / step`, `echo_roads.cpp:1337-1350`) so the AI and the road agree about what a corner is.
- `bank(s)` — already on every sample (`RoadSample::bank`, `echo_roads.h:130`), already curvature-derived and smoothed (`echo_roads.cpp:829-836`).
- `v_class` — reuse the traffic multipliers as the starting point: Freeway 1.0, Ramp 0.55, street 0.45 (`host_echotropolis.cpp:1352-1354`), scaled to racing pace.
- `v_brake` — a single backward pass over the line at build time. Because the line is arc-indexed this is O(n) and can be baked, which also gives you a free lap-time estimate for balancing payouts.
- Because the zigzag law guarantees a per-class curvature ceiling (0.8 / 1.2 / 2.5 °/m, `echo_roads.cpp:1317-1319`), `v_target` has a **provable floor per class**. That is a real gift: the AI can never be surprised by a corner the road generator would have deleted.

#### D.3 Racing line, not lane centre

Lane offset is a lateral scalar in exactly the same place `TrackRider::lateral` sits (`app/descent_slide.h:47`). Compute an offset track: out–in–out via a smoothed signed-curvature blend, clamped to `±(edge.width/2 − carHalfWidth − 0.4)`. Store it on the `RaceLine` at build; it is deterministic and free at runtime.

#### D.4 Overtaking

Minimal viable, in priority order:
1. **Longitudinal**: if a car ahead within 2 s of closing time, target `v_ahead` and pick an offset lane; if a lane is clear for the next 3 s of travel, commit.
2. **Lateral**: bias the racing-line offset toward the clear side, rate-limited to a plausible steering input (a real driver cannot teleport 3.4 m).
3. **Defence**: the leader may hold one lane offset toward the inside on corner entry, once per corner. More than that and it reads as cheating.

Note the existing traffic uses `lane = (ci & 1)` clamped (`host_echotropolis.cpp:1373`), so **at most 2 lanes are ever occupied** — that must generalize before overtaking is meaningful.

#### D.5 Rubber-banding — the argument, and the recommendation

**Recommendation: do not rubber-band the physics. Rubber-band the field composition instead.**

*For rubber-banding:* it keeps races close, it hides AI quality problems, and with a 36-part upgrade system the player's car performance varies over a huge range — a fixed AI is trivially beatable after two turbo upgrades and unbeatable before them.

*Against, and this is the case that wins here:* this game's entire identity is a **mechanical tuning simulator** (`VEHICLE_UPGRADES.md:30` — *"the mechanical half is deeper than any NFS"*). The player spends credits, watches a dyno pull draw a torque curve, creeps a timing slider toward `knockLimit` and risks an engine. **If the AI's speed silently tracks the player's, every one of those decisions is invalidated.** A player who cannot tell whether their new camshaft did anything has been robbed of the only thing this game does better than its references.

*The policy:*
- **No speed rubber-banding. Ever.** AI speed is a pure function of the line, the class, and that rival's own `WheeledTuning`.
- **Do balance the field by build.** Each event declares a target performance index (peak power / mass, computable from `ComposedBuild` — the dyno already samples peak torque and power over 65 points, `app/vehparts.cpp:461-468`). Rivals are generated with builds around that index, ±spread. A player who over-builds *should* win easily; that is the reward for building well.
- **Do gate entry by index.** An event can refuse a car that is 40% over its class — the "you brought a knife to a gun fight, in reverse" problem — which is honest and readable, unlike invisible AI scaling.
- **Do vary rival driver skill**, not rival car speed: a skill scalar that scales the *margin* on `v_target` (0.90 → 1.0) and adds line-following error. That produces a spread of finishing positions from identical cars and is legible to the player as "that guy brakes early".
- **One concession, opt-in:** an assist setting that adds a small catch-up allowance for the *last* AI only, so a race never becomes a one-car parade for a struggling player. Off by default, surfaced in settings (`app/ui.h:344-376` already persists settings), never applied to the leader.

### E. RACE FLOW / STATE

A single `RaceSession` state machine, ticked inside the fixed sub-step loop (`app/app_run.cpp:9146-9170`) so it is deterministic:

```
Idle → Staging → Countdown → Running → (Finished | DNF | Abandoned) → Results → Idle
```

**Staging.** Positions come from the `RaceLine`: grid slot `i` at `arc = −(i/2)*gridSpacingM`, lateral `±1.7 m`. Teleport via `setBodyPosition` + zero velocities, exactly as the perfshop lift proof does (`app/world_hosts/host_drive.cpp:187-188`), then settle for ~180 ticks on handbrake (`:189-197`).

**Countdown.** 3-2-1-GO on the fixed step, input gated to brake/handbrake only. Audio: the repo has no countdown SFX; reuse the low-pitched one-shot the dyno already borrows (`app/world_hosts/host_drive.cpp:380`) until real audio lands. A false start = 2 s throttle lockout, not a restart.

**Checkpoints.** Use `TriggerVolume` (`app/trigger.h:25-31`) fed the **chassis** position, not the camera. Two required changes:
1. **Re-arm.** `fired` is a permanent latch (`app/trigger.h:29`). Add `rearm(id)` / an `oneShot` flag so a lap can re-fire the same gate.
2. **Orientation.** Triggers are axis-aligned AABBs; a gate across a diagonal freeway becomes a very coarse box. Two options: (a) add a yaw to `TriggerVolume`, or (b) **skip trigger volumes for gates entirely** and test arc progress along the `RaceLine` instead — `arc` crossed a checkpoint's `arc` this step, with a lateral-distance sanity check. **(b) is strictly better**: it is exact, orientation-free, immune to tunnelling at 55 m/s (an AABB test at 60 Hz can miss a 4 m-deep gate above ~240 m/s but a car that clips a corner can still miss a *narrow* one), and it costs one float compare. Keep `TriggerVolume` for non-gate things like speed traps and shortcut detection.

**Progress and position.** Every racer carries `(lap, arcOnLine)`. Position = sort by `lap * totalLength + arc`, descending. Because the AI already integrates arc distance, this is free for rivals; the player's arc comes from a nearest-sample search seeded from last frame's index (O(1) amortised, the same walk `TrackRider::tick` uses, `app/descent_slide.cpp:259-261`).

**Off-route handling.** If the player's lateral distance from the line exceeds ~3× edge half-width for more than 3 s, show a "return to route" chevron (the minimap widget already exists, `app/ui.h:325-331`) and start a 10 s DNF timer. A **reset-to-route** action is mandatory and does not exist today (§1.2): teleport to the nearest line sample, orient to the tangent, keep 40% of speed, apply a 3 s penalty.

**DNF** on: route-abandon timeout, wrong-way for >5 s, or exceeding a per-event time limit.

**Results and payout.** This is the piece that makes the whole upgrade system come alive, and it is genuinely small:

```
payout = base(event) * positionMult[pos] * cleanBonus * eventDifficulty
credits += payout;   build.saveFile(defaultBuildSavePath());
```

`VehicleBuild::credits` (`app/vehparts.h:139`) is the wallet, `saveFile` (`:150`, path `:154`) is the persistence, and `PerfShop::consumeNeedSave()` (`app/perfshop.h:95`) is the existing save-on-change pattern. **Today nothing in the repo grants credits** — `app/perfshop.cpp:543-549` only debits. A one-function `awardCredits(VehicleBuild&, int)` closes the loop between racing and the 36 shipped parts and is arguably the single highest-value line of code in this plan.

Also add to the persisted state (new keys in the same JSON, no format bump needed): per-route best lap and best total, and an event-completed set for tier gating (`Part::tier` exists and is unused as a gate, `app/vehparts.h:41`).

**HUD.** `HudModel` (`app/ui.h:260-338`) has no vehicle fields; add a `RaceHudModel` sibling rather than bloating it — speed, RPM/redline, gear (`gear()` exists with zero callers, `engine/physics/JoltVehicle.cpp:254-256`), nitrous, lap `n/N`, position `p/F`, current/best/delta split, next-checkpoint chevron (reuse `app/ui.h:325-331`).

### F. STREAMING AT 200 km/h

**The numbers.** 200 km/h = 55.6 m/s. Tiles are 32 m (`app/terrain.h:62`).

| | radius 6 (`--world drive`, `host_drive.cpp:71`) | radius 8 (canon, `app_run.cpp:3113`) |
|---|---|---|
| Ring half-extent | 192 m | 256 m |
| **Lead time at 55.6 m/s** | **3.45 s** | **4.6 s** |
| Tiles crossed per second | 1.74 | 1.74 |
| Resident tiles | 169 | 289 |
| Resident collision tris | ~346k | ~592k |

3.45 s of lead is not enough, and **half of it is spent behind you** because the ring is symmetric (`app/terrain.cpp:1356-1364`).

**Four changes, in order of value:**

1. **Velocity lookahead on `TerrainStreamer` — the big one.** Change the signature to `update(..., focusX, focusZ, velX, velZ)` and compute wants from `min(dist(pos), dist(pos + vel*lookahead))`, exactly as `WorldStreamer` does (`app/world_stream.cpp:398-420`). The pattern is proven, the driving path already feeds real chassis velocity to the region streamer at ≥4 s lookahead (`app/app_run.cpp:9843-9850`), and `assets/world/regions.json:4` documents the intent: *"loadRadius is tuned generously for VEHICLE traversal (~40 m/s drive => 8-12 s of stream lead time)"*. With a 4 s lookahead at 55.6 m/s the effective forward horizon becomes 192 + 222 = **414 m ≈ 7.4 s** at radius 6, with no increase in resident tile count.
2. **Anisotropic ring.** Bias the shell scan by heading: request tiles whose direction from the focus has `dot(dir, vel) > 0` first, and allow the forward extent to reach `radius + 2` while the rear stays at `radius − 1`. Same total tile budget, roughly double the useful lead. This is a change to the loop at `app/terrain.cpp:1356-1364` only.
3. **Residency hysteresis.** Eviction currently fires the instant the Chebyshev distance exceeds the radius (`app/terrain.cpp:1329-1333`). A car tracking a road that runs along a tile boundary will thrash create/destroy of a 3-mesh triple **plus a Jolt `MeshShape` BVH build on the main thread** (`app/terrain.cpp:1235`). Add a `loadRadius`/`unloadRadius` pair matching `WorldStreamer`'s (`app/world_stream.cpp:421-424`); `unload = load + 2` is sufficient.
4. **Route prefetch.** Because a race route is known in advance, request tiles along `RaceLine` samples 8–12 s ahead — including *around the next corner*, which velocity extrapolation gets wrong precisely where it matters. This is strictly better than any generic heuristic and is unique to race mode: a `prefetchAlong(const RaceLine&, float arc0, float arc1)` that enqueues at low priority behind the normal ring.

**Also worth doing:**
- **Prewarm the whole route during the countdown.** 3 seconds of staging is enough to force-resident the first ~800 m at radius-6 quality. The upload-budget bypass for under-focus tiles already exists (`app/terrain.cpp:1399-1403`).
- **Stop building 3 LODs per tile** (`app/terrain.cpp:1120-1123`). At racing distances most tiles will only ever draw Quarter. Building LOD0 + the LOD the tile's spawn distance implies, and upgrading lazily, cuts generation cost and VRAM by roughly a third.
- **Raise the upload budget while racing.** `setUploadBudget` / `setMaxInFlight` are already exposed (`app/terrain.h:297-298`); `host_drive` sets 64 (`host_drive.cpp:72`), the canon world sets 512 in-flight for stills (`app/app_run.cpp:5355`).
- **The `WorldStreamer` side is already fine** and needs no work beyond confirming the drive-mode lookahead is applied on the racing path too.
- **Add a high-speed streaming gate.** The self-test drives 7 m/s (`app/terrain.cpp:1717-1720`). Add a 55 m/s variant asserting zero frames without ground under the car and zero tile rebuild thrash — this is the test that will actually catch regressions.

### G. PERFORMANCE

A high-speed camera changes the budget in four specific ways.

**1. Far plane and draw distance.** `--world drive` never calls `setCameraFar` and therefore runs at the **200 m default** (`engine/rhi/vk/VulkanRenderDevice_internal.h:2988`). At 55.6 m/s that is 3.6 s of visible world — unacceptable for racing. Outdoor hosts raise it manually to 15 km (`app/app_run.cpp:3586`) or 20 km (`origin/echotropolis:app/world_hosts/host_echotropolis.cpp:504`). Racing needs 4–8 km plus the terrain horizon ring (`addTerrainHorizonRing`, `app/terrain.h:165-176`, `rOuter = 13000`, visual only, no collision `:163`). More far plane means more candidates, which pushes directly on culling.

**2. Shadows — the hardest wall.** There is **no CSM**. The comment is explicit: *"Single map (no CSM): an ortho box of half-extent `kShadowOrtho` centered on the camera position … The box follows the camera so the visible ~60 m level is always covered"* (`engine/rhi/vk/VulkanRenderDevice_internal.h:257-261`), `kShadowDim = 2048` (`:654`), `kShadowOrtho = 45.0f` (`:658`). At 55.6 m/s the shadow boundary sweeps past the car in **0.8 seconds** — shadows will visibly pop in less than a car length ahead of the play space. CSM is PLANNED only (`PROVENANCE.md:59`, `X3_NATIVE_ENGINE_PLAN.md:196`).

> **Racing mode depends on cascaded shadow maps more than on any other queued lane.** Nothing else on the render roadmap is a hard blocker; this one is.

Interim mitigation if CSM slips: raise `kShadowOrtho` (blurrier but continuous), bias the ortho box **forward along the velocity vector** rather than centering it on the camera (a one-line change that buys ~2× useful coverage for free), or accept shadowed-nearfield-only and lean on the fog/AO for depth.

**3. Culling.** GPU cull Tier 0/1 + HZB are **implemented and green on GPU** (`D15_GPUCULL_HANDOFF.md:17-19`) but **off by default**: `r_vis` defaults to `"1"` = PVS + CPU frustum only (`app/app_run.cpp:461`, policy table `engine/rhi/Visibility.h:24-31`). Racing should ship on **`r_vis 3`** (PVS + GPU cull + HZB). HZB is explicitly the answer for *"open terrain, dense outdoor prop fields"* (`D15_GPUCULL_HANDOFF.md:70-72`) — which is exactly a freeway lined with lamps, barriers, piers and signage. Tier 2 (mesh shaders) is **not wired** and clamps to Tier 1 (`D15_GPUCULL_HANDOFF.md:20`); it is a nice-to-have, not a dependency.

**4. Lighting.** A flat 64-light forward loop, no clustering, no tiling (`shaders/mesh.frag:56`, `:906`, `:978`). `EchoRoads` places a lamp every 34 m on freeway and every 26 m on streets (`origin/echotropolis:app/world_hosts/echo_roads.cpp:108-109`) — a 2 km night lap has ~60 freeway lamps alone, and the host is already doing nearest-K selection against the cap (`host_echotropolis.cpp:4164-4176`). Night racing plus the underglow/headlight cosmetics from `VEHICLE_UPGRADES.md:48-54` (which that doc already marks *"Blocked on Lane 2 (clustered lighting)"*, `:54`) will hit the cap immediately. **Clustered lighting is the second-most-depended-on queued lane** — but unlike CSM it only gates *night* racing and the cosmetic lighting layer, so it can follow.

**Everything else on the roadmap is a multiplier, not a blocker:**

| Lane | Status | Racing dependency |
|---|---|---|
| CSM / cascades | **not implemented**, planned (`X3_NATIVE_ENGINE_PLAN.md:196`) | **HARD BLOCKER** |
| Clustered lighting | not implemented, 64-light forward cap | **Blocks night racing + cosmetic lights** |
| GPU cull T0/T1 + HZB | green, default OFF (`app/app_run.cpp:461`) | **Turn it on.** Free win. |
| Mesh LOD (props/buildings/cars) | **does not exist** — terrain is the only thing with LOD (backlog Slice 28, `X3_NATIVE_SLICES.md:160-162`) | Strongly wanted at 4 km far plane; roadside props are the worst case |
| Mesh-shader Tier 2 | not wired (`D15_GPUCULL_HANDOFF.md:20`) | Nice to have |
| SSR + RT reflections | implemented, default ON (`app/app_run.cpp:520`, `:522`) | Wanted for wet asphalt and clearcoat paint (`VEHICLE_UPGRADES.md:46`); **note the known intermittent black-frame with RT reflection fallback on the drive path**, documented at `app/world_hosts/host_drive.cpp:164-167` — race-shaped, pre-existing, sidestepped with `--legacypost`/`--notaa`/`--norefl`. Fix this before shipping a racing mode on the default post stack. |
| TAA | implemented, default ON (`app/app_run.cpp:505`) | Essential — SSR hard-gates on it (`vk_graph.cpp:256-259`) |
| Motion blur | **no shader exists** (`shaders/` has no motion-blur pass) | The one missing *speed-sensation* effect; `VEHICLE_UPGRADES.md:118` lists it |
| RT soft shadows | default tier 2, but alpha-cutout casts the **full quad** (`docs/KNOWN_BUGS.md:431`) | Roadside trees/crowds will cast black rectangles — pin tier 0 on race routes until fixed |

**Physics budget.** 289 resident static bodies at ~592k collision triangles (§3.2), plus a road collision mesh (`EchoRoads::collisionMesh()`), plus 4 raycast wheels per car × N racers. The `MeshShape` BVH build on the main thread (`app/terrain.cpp:1235`) is the spike to watch — moving it to the worker alongside vertex generation is a contained, high-value fix for exactly this mode.

### H. BUILD ORDER

Effort sizes: **S** ≈ ≤2 days, **M** ≈ ≈1 week, **L** ≈ 2–4 weeks, **XL** ≈ >1 month.
"Reuse" means the code exists and is called; "extend" means the code exists and needs new capability; "net-new" means it does not exist.

| # | Item | Effort | Reuse vs net-new | Why here |
|---|---|---|---|---|
| **1** | **Merge `origin/echotropolis` road tech onto a working lane** — fetch, port `EchoRoads` off the baked-PNG `Heightfield` onto `terrainHeightAtWorld`, fix the ramp `length` double-count (`echo_roads.cpp:1576-1578`) | **M** | **Reuse** — 2200 lines of validated, deterministic road generation. The `hf.heightAt` → `terrainHeightAtWorld` adapter is one function. | Everything else in this plan sits on `RoadGraph`. Nothing can start until it exists on a lane that builds. |
| **2** | **`awardCredits()` + race payout hook** | **S** | Net-new, ~1 function | Closes the loop on 36 shipped parts that currently cannot be earned toward. Highest value-per-line in the document. Can ship before any race exists (payout from any source). |
| **3** | **Race HUD** — speed, RPM/redline, gear, nitrous, lap, position, split | **S–M** | Extend `HudModel` (`app/ui.h:260-338`); `gear()` already implemented with zero callers | Racing is unplayable without a speedometer. Also unblocks tuning feedback for the parts system. |
| **4** | **`RaceRoute` / `RaceLine` + `x3.race/1` JSON + autoroute** | **M** | Extend — the seeded graph walk exists (`host_echotropolis.cpp:1429-1459`); adjacency clustering exists (`:1400-1421`) and should be hoisted into `RoadGraph` | The data model everything else consumes. |
| **5** | **`RaceSession` state machine + arc-based checkpoints + time trial** | **M** | Reuse `SimClock` (`engine/net/SimClock.h:20-21`), `Objective` (`app/objective.h`), minimap chevron (`app/ui.h:325-331`); net-new session logic | **First playable.** Ships as `--world race`. Fully headless-testable. |
| **6** | **Streaming for speed** — velocity lookahead + anisotropic ring + hysteresis + route prefetch + a 55 m/s gate | **M** | Extend `TerrainStreamer` (`app/terrain.h:279-280`); the pattern is copied verbatim from `WorldStreamer` (`app/world_stream.cpp:398-424`) | Without it a race at 200 km/h drives into unstreamed ground. Do it *with* item 5 so the gate has something to gate. |
| **7** | **Turn on `r_vis 3`, raise the far plane, kill the RT-reflection black frame** | **S** | Reuse — GPU cull + HZB are green (`D15_GPUCULL_HANDOFF.md:17-19`), just default-off (`app/app_run.cpp:461`) | Cheap performance headroom before adding rivals. The black-frame bug (`host_drive.cpp:164-167`) is a shipping blocker. |
| **8** | **Circuit + sprint modes** (laps, trigger re-arm, closure validation) | **S** | Extend item 5 | Nearly free once time trial works. |
| **9** | **Reset-to-route + flip recovery** | **S** | Net-new — no vehicle respawn exists anywhere | Currently a flipped car ends the session. Ship before anyone plays it twice. |
| **10** | **Tier-A kinematic rivals + position tracking** | **M** | **Extend** `poseCar` (`host_echotropolis.cpp:1328-1391`) — routing, lane offsets and class speeds already work | First "race against someone" build. Cheap, and it validates the whole session/position layer before the expensive AI. |
| **11** | **BRIDGES / VIADUCTS over the river** — two-sided elevation profile, span detection against `worldWaterLevelAt`, pier placement off water, abutments, **collidable barriers** | **L** | **Extend** — deck box section (`echo_roads.cpp:434-479`) and `pillar()` (`:500-524`) already exist; the grade-limited relaxation (`:682-722`) becomes two-sided | The cheaper half of the signature terrain, and the more photogenic one. |
| **12** | **CSM / cascaded shadows** *(engine lane, not owned by racing)* | **L** | Net-new (`X3_NATIVE_ENGINE_PLAN.md:196`, planned) | **Hard dependency.** A 45 m camera-locked shadow box (`VulkanRenderDevice_internal.h:658`) is a visible wall at racing speed. Interim: bias the ortho box forward along velocity (**S**, do this immediately). |
| **13** | **Tier-B physics rivals** — line-following → `VehicleInput`, curvature speed law, braking pass, overtaking, skill spread | **L** | Net-new controller; **reuse** `DriveDemo` + `applyTuning` + the baked curvature/bank data | The real racing experience. Also requires `setLoopPosition` on the audio API (`engine/audio/IAudioSystem.h:129-130`) so rival engines can move. |
| **14** | **TUNNELS / MOUNTAIN BORES** — subgrade profile runs, swept tube reusing the `Bore`/`Burst` sweep (`app/descent_slide.cpp:381-441`), oriented keep-out list on `TerrainStreamer`, synthetic portal rooms at the mouths, rationed interior lights | **L–XL** | **Extend** — tube sweep and PVS exist; oriented keep-out and portal synthesis are net-new | The most distinctive thing in the vision and the most expensive. Replaces the placeholder boxes at `app/city.cpp:386-411` and finally makes `X3_WORLD_BLUEPRINT.md:48`'s "✅" true. |
| **15** | **Drift layer** (rear-grip cut above slip angle + counter-steer assist + stabilisation), then drift events | **M** for the layer, **S** for the event | Net-new; `VEHICLE_UPGRADES.md:124` already specifies it | Independent of everything above — can run in parallel on its own branch from day one. |
| **16** | **Clustered lighting** *(engine lane)* → night racing, underglow, headlights | **L** | Net-new; `VEHICLE_UPGRADES.md:54` already blocks the cosmetic lighting tier on it | Gates night racing and the NFS-Underground signature look. Not a blocker for daytime racing. |
| **17** | **Skidmarks + tyre smoke + surface response** | **M** | Extend — `submitDecals` (`engine/rhi/IRenderDevice.h:1001-1008`) and the particle pool (`app/fx.h:166-234`) exist, but the decal ring is 64 entries / 12 s life (`app/fx.h:176-177`) and needs a dedicated long-lived road ring. Surface friction is **net-new at every layer** (zero friction API in `IPhysicsWorld.h`) | Big feel-per-effort once drift lands. |
| **18** | **Pursuit / cops / heat / roadblocks** | **XL** | Net-new — `VEHICLE_UPGRADES.md:126`: *"Nothing exists; this is the biggest net-new system"* | Highest fantasy value, and it needs items 4, 5, 10, 13 and preferably 14 first. |
| **19** | **Ghost / replay** | **M** | Net-new recorder; **reuse** the fixed step (`engine/net/SimClock.h:20-21`) and the rollback plumbing (`engine/net/IClientPredictor.h:20-25`), which name replay as the intended beneficiary (`SimClock.h:11`) | Big value for time trials. Deterministic sim makes this much cheaper than it looks. |

**Suggested first milestone (items 1–8, ~6–8 weeks):** `--world race` boots a route procedurally generated over a real `RoadGraph` on the canonical terrain, streams correctly at 200 km/h, counts laps against a clock, shows a speedometer, and pays credits into the shop you already drive into. That is a complete, testable, shippable loop with **no** net-new AI, **no** net-new terrain generation, and **no** engine-lane dependency except turning on culling that is already green.

---

## 6. OPEN QUESTIONS FOR TIM

1. **What is "the BL version"?** (§0.1.) Nothing in the repo resolves it. It changes which route table gets authored and possibly which world the racing lives in.
2. **Which world?** `EchoRoads` was built for the Echo Harbor island (a baked 4096 m PNG heightfield); the freeway-tunnels canon lives in the EFLZ planet (procedural fBm, 4 mountain ranges, the carved river). Racing can go on either, but the road generator must be ported to whichever one wins, and the four named bores + the river only exist on the EFLZ side.
3. **Day or night first?** Night racing is the better look and the stronger fantasy, but it is gated on clustered lighting (§G). Daytime racing is gated only on CSM.
4. **Do rivals share the player's garage?** i.e. are rival builds drawn from `parts.json` (which would make the performance index honest and let the player *see* what beat them), or are they hand-tuned numbers?
5. **Is a race a mission?** The `x3.mission/1` format (`missions/level1.mission.json:2`) could host race events as stages, which would give story gating and objective text for free — but it has no time condition (`app/mission.h:16-19`) and would need one.

---

*Everything in §1–§4 was verified against the tree on 2026-08-01. `origin/echotropolis` was fetched read-only for this analysis and is not merged; nothing in the repository was modified.*
