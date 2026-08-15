# The Confection Annex & The Anywhere Elevator — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A Roald-Dahl-*style* (original, IP-clean) whimsical mega-factory — **The Confection Annex** — hidden beside the Spire, reachable only because the souped-up glass elevator learns to travel **sideways, slantways, and longways** (the **Anywhere Elevator** upgrade), ending in a glass-shattering **roof burst** above the world.

**Architecture:** Two coupled efforts. (1) `ElevatorSystem` generalizes from a vertical stop-list to a **3D waypoint graph with straight-segment travel** (any-angle accel/cruise/decel along the segment arclength), keeping the legacy vertical API byte-identical so `--test-elevator` E1–E6 stay green; a keypad code unlocks the hidden lateral rail. (2) A new game module `app/factory_annex.{h,cpp}` authors five wonder-room floors in the rifthub style (author-once Scene props + entity-span `tick()` animation, zero per-frame heap), hosted by `--world factory` with headless screenshot / GIF-capture / self-test gates.

**Tech Stack:** X3Native only — `Scene` + `mesh_prims` + `surface_library` (PBR), `ElevatorSystem` (10-state FSM), `TriggerSystem`, `CombatFx` (incl. `spawnFireball` once the barrel branch folds — fallback given), water + glass pipelines, GPU debris pool, `IAudioSystem` (asset-optional cues), `gif.h` capture pattern. Clean-room: NO Dahl/Wonka names, characters, lyrics, or trademarks anywhere in code, assets, or strings — Dahl-*style* whimsy, original content.

---

## Design decisions (locked before任 tasks)

1. **Why an Annex, not the Spire floors:** blueprint §2.4 gives every Spire floor real dark-facility canon (F1 detention…F7 clone pods). The factory must not overwrite canon. The Annex sits at **+60 m X from the shaft** (shaft at Spire (3500, −2000) local origin; Annex center local `x=+60`), floors at **F2/F3/F4-adjacent heights**, connected by a **lateral bore** the elevator traverses. Discovery beat: the facility's eccentric founder built a private wonder-works behind the wall — very Dahl, zero canon damage.
2. **Elevator motion contract:** travel is a **single straight 3D segment** per leg ("slantways" allowed by authored rails), constant-jerk-free trapezoid speed profile on segment arclength `s ∈ [0, L]` with the existing `ElevTuning` (`maxSpeed/accel/decel/decelDist`) reinterpreted on arclength. Vertical-only graphs degenerate to today's behavior exactly (same math on `L = |Δy|`).
3. **Rider carry generalizes to Vec3.** Legacy `float update(...)` (returns Y delta) stays and keeps its exact semantics; new `const x3::phys::Vec3& carryDelta() const` exposes the full per-frame cab delta. Hosts that carry riders switch to `carryDelta()`; untouched hosts keep working (lateral rails simply never exist in their graphs).
4. **Unlock gating:** the Annex rail is invisible to `callNext()` cycling and the floor buttons. It unlocks ONLY via keypad code **4790** (the garage CPU — Tim's numerology, replaceable constant `kAnnexCode`). After unlock, a new golden button appears on the panel (`buildVisuals` adds it disabled/dark; unlock lights it).
5. **Roof burst is a scripted state, not free flight.** New `ElevState::Burst` (11th state): from the top Annex stop, code **9999** arms it; cab accelerates up a dedicated vertical rail past the roof plane, `CombatFx` glass-shatter + GPU-debris burst at the roof Y, cab glides to a sky stop (+40 m), holds 8 s over the world (sky dome + wind loop), auto-returns with `Freefall`→`EmergencyStop` theatrics into `DoorsOpening`. All existing states untouched.
6. **Room animation style:** every animated prop follows the rifthub pattern — contiguous Scene entity spans stored on the room struct, `tick(dt)` mutates emissive/transform in place, per-prop phase offsets, no allocation. Machines that *move* (stir arms, sorter arms, capsule) use `scene.setTransform`-style pose pokes on their span (same technique the elevator cab uses).
7. **Palette:** candy-bright against dark iron — factory floor `#2a2430` (aubergine iron), brass `#b08d57`, then per-room accent saturations (below). Emissives do the wonder; PBR surfaces from `surface_library` do the grounding.

## File structure

| File | Responsibility |
|---|---|
| `app/elevator.h/.cpp` (modify) | 3D waypoint graph, arclength motion, `carryDelta()`, unlock, `Burst` state |
| `app/factory_annex.h` (create) | `FactoryAnnex` class: room structs, entity spans, trigger ids (base **300**), public API |
| `app/factory_annex.cpp` (create) | `build()` (shell + 5 rooms + bore), `tick()`, `shutdown()`, self-test |
| `app/factory_rooms.cpp` (create) | per-room authoring functions (split from annex core so files stay focused) |
| `app/main.cpp` (modify) | `--world factory` host, `--test-factory` (STANDALONE `if()` — MSVC C1061!), `--capture-factory` |
| `app/CMakeLists.txt` (modify) | add `factory_annex.cpp`, `factory_rooms.cpp` |
| `specs/FACTORY.spec.md` (create) | room specs + elevator motion contract addendum |
| `docs/design/X3_WORLD_BLUEPRINT.md` (modify) | §2.7 The Confection Annex |

Trigger-id map (fresh range, no collisions — rifthub owns 200–207): **300–319** = annex (300 bore-entry, 301–305 room-entry per floor, 310 sorter-chute, 311 fizz-lowgrav, 312 roof-burst-arm, 313 tube-ride).

---

## PHASE 1 — The Anywhere Elevator (elevator goes 3D)

### Task 1: Waypoint stop struct + graph API (additive, legacy-safe)

**Files:** Modify `app/elevator.h`, `app/elevator.cpp`, Test: extend `runElevatorSelfTest` (E7+)

- [ ] **Step 1: Write the failing test** (in the existing self-test function, new check E7):

```cpp
// E7: buildEx with a 3D graph — legacy accessors still sane.
{
    ElevatorSystem ev;
    std::vector<ElevatorSystem::Stop> stops = {
        { {0.f, 2.f, 0.f},  "F1",    /*hidden*/false },
        { {0.f, 12.f, 0.f}, "F3",    false },
        { {60.f,12.f, 0.f}, "ANNEX", true  },   // lateral, hidden until unlock
    };
    // rails: 0<->1 vertical, 1<->2 lateral (adjacency pairs)
    std::vector<std::pair<int,int>> rails = { {0,1}, {1,2} };
    ok &= check(ev.buildEx(scene, device, physics, 1.6f, 0.25f, 1.6f, stops, rails, 0),
                "E7 buildEx 3D graph builds");
    ok &= check(ev.stopCount() == 3 && ev.stopY(2) == 12.f,
                "E7 legacy stopY reads the Y of a lateral stop");
}
```

- [ ] **Step 2: Run to verify it fails** — `./build/bin/Release/X3Engine.exe --test-elevator` → FAIL (no `Stop`/`buildEx`).

- [ ] **Step 3: Implement.** In `elevator.h`:

```cpp
struct Stop {
    x3::phys::Vec3 center;      // cab-CENTER world position at this stop
    const char*    label;       // panel/button label
    bool           hidden;      // not in callNext()/button cycling until unlocked
};
// 3D build: stops + rails (undirected adjacency). Legacy build() forwards here
// with x=shaftX,z=shaftZ on every stop and a full vertical rail chain.
bool buildEx(Scene& scene, x3::rhi::IRenderDevice& device, x3::phys::IPhysicsWorld& physics,
             float cabHalfX, float cabHalfY, float cabHalfZ,
             const std::vector<Stop>& stops,
             const std::vector<std::pair<int,int>>& rails, int startStop = 0);
const x3::phys::Vec3& stopCenter(int i) const;      // full 3D accessor
void unlockHidden();                                 // reveals hidden stops
bool hiddenUnlocked() const { return m_unlocked; }
```

Internals: replace `std::vector<float> m_stopsY` **usage** with `std::vector<Stop> m_stops` + keep `m_stopsY` as a derived mirror (`m_stopsY[i] = m_stops[i].center.y`) so every legacy read (`stopY`, `nearestStopTo`, strata display) is untouched. `build()` becomes a thin wrapper constructing the vertical chain. Store rails as `std::vector<std::vector<int>> m_adj`.

- [ ] **Step 4: Run** `--test-elevator` → E1–E6 **byte-identical output**, E7 passes.
- [ ] **Step 5: Commit** — `git commit -m "elevator: 3D waypoint stops + rails graph (additive; legacy build() wraps)"`

### Task 2: Straight-segment 3D motion + Vec3 rider carry

**Files:** Modify `app/elevator.cpp` (motion core), `app/elevator.h`, tests E8/E9

- [ ] **Step 1: Failing tests.**

```cpp
// E8: lateral leg travels on X with the trapezoid profile; Y untouched.
{   /* build E7 graph; */ ev.enableFsm(true); ev.unlockHidden();
    ev.callTo(2);                       // from stop 1 (0,12,0) to (60,12,0)
    float maxY = -1e9f, endX = 0.f;
    for (int i = 0; i < 3000; ++i) {    // 50 s at 60 Hz — plenty
        ev.update(1.f/60.f, scene, physics);
        maxY = std::max(maxY, std::fabs(ev.cabTopY() - (12.f + 0.25f)));
        endX = ev.cabCenter().x;
        if (ev.state() == ElevState::DoorsOpen) break;
    }
    ok &= check(maxY < 0.01f, "E8 lateral leg keeps Y flat");
    ok &= check(std::fabs(endX - 60.f) < 0.05f, "E8 lateral leg arrives at x=60");
}
// E9: carryDelta() integrates to the full segment vector.
{   /* ride stop1->stop2 accumulating ev.carryDelta() into acc; */
    ok &= check(std::fabs(acc.x - 60.f) < 0.1f && std::fabs(acc.y) < 0.1f,
                "E9 carryDelta sums to (60,0,0) across the lateral leg");
}
```

- [ ] **Step 2: Run** → FAIL (`cabCenter`/`carryDelta` missing; lateral motion absent).

- [ ] **Step 3: Implement the motion core.** In `update()`, replace the scalar position integrator with segment travel:

```cpp
// Segment state (set on departure): m_segFrom, m_segTo (stop centers),
// m_segLen = |to-from|, m_segDir = (to-from)/len, m_s = arclength progressed.
// The EXISTING trapezoid speed logic (accel/cruise/decel vs decelDist) now
// advances m_s instead of m_pos.y; then:
const x3::phys::Vec3 prev = m_pos;
m_pos = m_segFrom + m_segDir * m_s;
m_carry = { m_pos.x - prev.x, m_pos.y - prev.y, m_pos.z - prev.z };
physics.setBodyPosition(m_cabBody, m_pos);            // same moved-static technique
```

`carryDelta()` returns `m_carry`; legacy `update()` return stays `m_carry.y` (exact old semantics for vertical graphs). `playerRiding()` already footprint-tests against the cab — verify it reads `m_pos.x/z` (not the build-time shaft XZ); if it caches shaft XZ, switch it to `m_pos` (behavior identical for vertical graphs).
Route selection in `callTo`: BFS over `m_adj` from current stop, then travel the legs in sequence (multi-leg queue `m_route`); each leg runs the full accel/decel profile (elevator "stops" at corners — reads as deliberate, avoids curve math).

- [ ] **Step 4: Run** `--test-elevator` → E1–E9 pass; E1–E6 output unchanged.
- [ ] **Step 5: Commit** — `"elevator: straight-segment 3D travel + Vec3 carryDelta (vertical graphs byte-identical)"`

### Task 3: Keypad unlock + hidden-stop button + lateral strata panorama

**Files:** Modify `app/elevator.cpp` (keypad handler, `buildVisuals`, strata draw), test E10

- [ ] **Step 1: Failing test E10:** entering code `4790` flips `hiddenUnlocked()`; before unlock `callTo(hiddenIdx)` is a no-op (state stays Idle); after unlock it departs.
- [ ] **Step 2: Run** → FAIL.
- [ ] **Step 3: Implement.** Keypad path (same handler that recognizes `1127` for disco) adds `kAnnexCode = 4790` → `unlockHidden()` + `ding` + light the golden button (a `buildVisuals` panel quad whose emissive goes 0.05 → 3.0). `callTo`/`callNext` skip `hidden && !m_unlocked` stops. Strata display: when the active leg's `|dir.y| < 0.3`, swap the scroll axis — layers slide horizontally with a `"ANNEX TRANSIT"` label band (reuse the existing strata quad span; scroll offset driven by `m_s/m_segLen`).
- [ ] **Step 4:** `--test-elevator` all green. **Step 5: Commit** `"elevator: 4790 annex unlock + golden button + lateral strata panorama"`

### Task 4: The Burst state (roof finale, scripted)

**Files:** Modify `app/elevator.h` (enum + arm API), `app/elevator.cpp`, test E11

- [ ] **Step 1: Failing test E11:** `armBurst()` from the designated burst stop + `update` loop → state passes through `Burst`, cab Y exceeds `roofY`, returns to `DoorsOpen` at the burst stop within 40 s sim; `burstFired()` true exactly once.
- [ ] **Step 2: Run** → FAIL. 
- [ ] **Step 3: Implement.** `ElevState::Burst = 10`. Config on `buildEx`-adjacent setter: `setBurst(int fromStop, float roofY, float apexY)`. Sequence (all inside the FSM tick): doors seal → accel up at `2*accel` uncapped to `1.6*maxSpeed` → on crossing `roofY` set `m_burstFired=true` + emit via new host callback `std::function<void(const Vec3&)> onRoofShatter` (host wires debris/FX — keeps elevator render-pure) → decel to hover at `apexY`, hold 8 s (`m_holdT`) → `Freefall` (existing state!) until `roofY-2`, then `EmergencyStop` catch → normal `Arriving` glide to the burst stop → `DoorsOpening`. Keypad `9999` at the burst stop = `armBurst()`.
- [ ] **Step 4:** E1–E11 green. **Step 5: Commit** `"elevator: Burst state — scripted roof-shatter apex ride with Freefall return"`

---

## PHASE 2 — Annex shell + host wiring

### Task 5: `FactoryAnnex` skeleton + shell + bore

**Files:** Create `app/factory_annex.h`, `app/factory_annex.cpp`; modify `app/CMakeLists.txt`

- [ ] **Step 1:** Header (rifthub-patterned). Core shapes:

```cpp
namespace x3::game {
enum class FactoryTrigger : uint32_t {
    BoreEntry = 300, RoomMixture = 301, RoomInvention = 302, RoomFizz = 303,
    RoomSorting = 304, RoomTube = 305, SorterChute = 310, FizzLowGrav = 311,
    BurstArm = 312, TubeRide = 313,
};
struct AnnexRoom {                    // one wonder-room floor
    const char* name;                 // e.g. "MIXTURE ATRIUM" (original names ONLY)
    float       baseY;                // floor Y
    float       accent[3];            // room accent color (linear)
    uint32_t    propEntFirst = 0, propEntCount = 0;   // animated span
    uint32_t    glowEntFirst = 0, glowEntCount = 0;   // emissive-pulse span
    bool        visited = false;      // latched by its entry trigger
};
class FactoryAnnex {
public:
    void build(Scene&, x3::rhi::IRenderDevice&, x3::phys::IPhysicsWorld&, TriggerSystem&,
               float shaftX, float shaftZ);            // authors EVERYTHING once
    void tick(float dt, Scene&);                       // all room animation
    void onTrigger(uint32_t id);                       // latches visited/chute/lowgrav
    void shutdown(x3::rhi::IRenderDevice&);
    bool built() const; uint32_t roomCount() const; const AnnexRoom& room(uint32_t) const;
    bool lowGravActive() const;                        // host applies jump modifier
    // ... entity/mesh bookkeeping mirrors Rifthub (m_meshes vector, freed uniformly)
};
} // namespace
```

- [ ] **Step 2: Shell dims (exact):** 5 floors, each **40×40 m footprint, 11 m clear height**, stacked at `baseY = {2, 15, 28, 41, 54}` (13 m pitch), centered local `(annexX=+60, z=0)` from the shaft. Aubergine-iron walls (`surface_library` metal plate, tint 0.16/0.14/0.19), brass trim beams every 8 m, glass curtain on the shaft-facing wall (glass pipeline) so riders SEE rooms during lateral approach. Bore corridor: 4 m∅ octagonal tube from shaft wall to Annex wall at `y=15` (Floor B height), brass-ribbed every 3 m.
- [ ] **Step 3:** `build()` authors shell + bore + physics floors/walls + the 10 trigger AABBs; empty room-content hooks call into `factory_rooms.cpp` per room. Compile + `--world factory` (Task 6) not yet — just ensure lib compiles: add both cpp files to `app/CMakeLists.txt` after `rifthub.cpp`.
- [ ] **Step 4: Commit** `"factory: annex shell + bore + trigger map (300-313) — rooms empty"`

### Task 6: `--world factory` host + `--test-factory` + `--capture-factory`

**Files:** Modify `app/main.cpp` (three additions, all following the rifthub host block pattern)

- [ ] **Step 1:** `--world factory`: physics + Scene + TriggerSystem + `FactoryAnnex.build(...)` + `ElevatorSystem.buildEx` with the combined graph: Spire-side stops F1/F3 (vertical), bore-level lateral rail to Annex stops A1–A5 (vertical chain at annexX), burst stop = A5 with `setBurst(A5, roofY=65, apexY=105)`. `enableFsm(true)`. Host wires: rider carry via `carryDelta()`, `onRoofShatter` → GPU-debris burst + `CombatFx` impacts (use `spawnFireball` if present on the branch; else 6× `spawnImpact` ring — note in code comment), low-grav via `annex.lowGravActive()` scaling the player jump impulse ×1.8. Headless: settle 24 frames, screenshot from `{60, 22, 46, -1.57, -0.30}` (three glass floors + shaft in frame).
- [ ] **Step 2:** `--test-factory` as a **STANDALONE `if()`** (NOT chained `else-if` — the arg parser already rides MSVC's C1061 nesting limit; see the portal-hub-polished commit note).
- [ ] **Step 3:** `--capture-factory [out.gif]` modeled on `--capture-rifthub`/`--capture-ai`: 640×360, 60 frames @ 20 fps, camera riding INSIDE the cab during the lateral bore approach (glass wall + rooms sliding by = the money shot).
- [ ] **Step 4:** Build; run `--world factory --headless --screenshot f.png` → exit 0, VMA 0 leaks. **Commit** `"main: --world factory host + --test-factory + --capture-factory"`

---

## PHASE 3 — The five wonder-rooms (`app/factory_rooms.cpp`)

> Room A is specified with **complete code**; B–E are specified with exact prop tables + animation params (same authoring helpers, different data — the implementer transcribes tables into the demonstrated pattern, no taste decisions required).

### Task 7: Floor A (y=2) — **THE MIXTURE ATRIUM** (full exemplar)

- [ ] **Step 1: Content:** a glowing **confection river** crossing the floor diagonally (12 m wide band) + **6 copper vats** (3 m∅, 4 m tall, stirring arms) + overhead **pipe canopy**.
- [ ] **Step 2: The river** = the water pipeline with candy grading: author a water patch `40×12` at `y=2.15`, then host-side `setWaterParams` tint `{1.0, 0.35, 0.55}` (raspberry), emissive under-glow strip beneath the surface (span-registered, `tick` pulses 1.2→2.6 @ 0.18 Hz). Two brass footbridges (physics boxes) cross it.
- [ ] **Step 3: Vats — complete authoring code:**

```cpp
static void buildVat(Scene& s, x3::rhi::IRenderDevice& dev, std::vector<x3::rhi::MeshHandle>& meshes,
                     AnnexRoom& room, float cx, float cz, float baseY, float phase) {
    // body: 10-segment tangent-box cylinder, copper PBR (surface_library "metal_copper")
    for (int i = 0; i < 10; ++i) {
        const float a = (i / 10.f) * 6.2831853f;
        addOrientedBox(s, dev, meshes, /*center*/{cx + 1.5f*std::cos(a), baseY+2.f, cz + 1.5f*std::sin(a)},
                       /*half*/{0.47f, 2.0f, 0.12f}, /*yaw*/a + 1.5708f, kCopper);
    }
    // stir arm: one thin brass box, entity registered into room.propEnt span;
    // tick() rotates it: yaw = t*0.8 + phase (pose poke, rifthub-style)
    room.propEntFirst = firstIfUnset(room.propEntFirst, addOrientedBox(s, dev, meshes,
        {cx, baseY + 4.2f, cz}, {1.7f, 0.06f, 0.12f}, phase, kBrass));
    ++room.propEntCount;
    // rim glow ring: emissive torus-approx (8 thin boxes), accent color, glowEnt span
    for (int i = 0; i < 8; ++i) {
        const float a = (i / 8.f) * 6.2831853f;
        addEmissiveBox(s, dev, meshes, {cx + 1.55f*std::cos(a), baseY + 4.05f, cz + 1.55f*std::sin(a)},
                       {0.30f, 0.05f, 0.08f}, a + 1.5708f, room.accent, /*strength*/2.0f,
                       room.glowEntFirst, room.glowEntCount);
    }
}
```

`tick()` for Floor A: stir arms `yaw = t*0.8 + i*1.047`; rim rings pulse `2.0 + 1.2*sin(t*1.1 + i*0.7)`; river under-glow as above. Six vats at `(±8, ±12)` staggered grid, phases `i*1.047`.
- [ ] **Step 4:** `--test-factory` A-checks: F1 room entity counts (6 vats × spans), F2 tick moves a stir arm (pose differs across 30 ticks), F3 river water params applied. Build + run → pass. Screenshot eyeball. **Commit** `"factory: Floor A — Mixture Atrium (river, 6 stirring vats, pipe canopy)"`

### Task 8: Floor B (y=15) — **THE INVENTION WORKS**

Prop table (each row = one machine; all use `buildVat`-style helpers: body boxes + one animated span entity + glow span):

| Machine (original names) | Footprint (m) | Anim type | Anim params | Glow |
|---|---|---|---|---|
| Gum-Stretcher | 4×2×3 | piston (Y bob) | amp 0.9, 1.4 Hz | mint `{0.4,1,0.6}` |
| Fizz Compressor | 3×3×5 | spin (yaw) | 2.2 rad/s | amber |
| Idea Bellows | 2×4×2 | squash (scaleY pose) | 0.7↔1.15, 0.5 Hz | violet |
| Sprocket Fountain | 3×3×6 | spin + bob | 1.1 rad/s / 0.4 amp | brass |
| Wobble Boiler | 4×4×4 | sway (roll ±0.12) | 0.9 Hz | raspberry |
| Button Organ | 6×2×3 | key-chase emissive | 8 keys, 0.12 s step | white |
| Notion Centrifuge | 5×5×2 | spin fast | 4.0 rad/s | cyan |
| The Maybe Machine | 2×2×7 | random flicker | seed t*13.7 hash | gold |

Plus a **conveyor** (14 m, 24 slat boxes, slats pose-scroll at 1.2 m/s, wrapping) carrying 8 emissive "gizmo" cubes. Tests: F4 count, F5 conveyor slat wraps. **Commit** `"factory: Floor B — Invention Works (8 machines + conveyor)"`

### Task 9: Floor C (y=28) — **THE FIZZ GALLERY**

Bubble columns: 4 glass cylinders (glass pipeline, 2 m∅ floor-to-ceiling) with **rising emissive bubble spheres** (10 per column, span-animated: `y += 1.1*dt`, wrap at ceiling, slight XZ sine wobble; sphere = existing sphere prim or 6-box cluster if no sphere prim — implementer checks `mesh_prims.h`). Ceiling: 3 giant slow fans (3-blade, 0.6 rad/s). **Low-grav trigger** (311) covers the central 16×16 zone: host scales jump impulse ×1.8 while inside + `FizzLowGrav` latches `lowGravActive`. Audio: cue slot for a fizz loop (asset-optional). Tests: F6 bubbles rise + wrap, F7 low-grav trigger latch. **Commit** `"factory: Floor C — Fizz Gallery (bubble columns, fans, low-grav zone)"`

### Task 10: Floor D (y=41) — **THE SORTING HALL**

A ring conveyor of 12 emissive orbs (10 gold, 2 dull-grey "duds"; orbit `r=8`, 0.5 rad/s), **2 sorter arms** (4 m brass, sweep ±1.2 rad @ 0.7 Hz, phase-offset), and the **Chute of Dubious Quality**: a 2×2 m floor hatch (trigger 310) — standing on it 1.5 s opens it (pose-slide door) and drops the player to a padded room on Floor A's level with a `buzz` cue + a wall sign span reading "QUALITY: DUBIOUS" (emissive letters via the HUD-font-atlas quad technique or box-serif letters — implementer picks the cheaper). Tests: F8 orb orbit, F9 chute trigger opens hatch. **Commit** `"factory: Floor D — Sorting Hall (orb ring, sorter arms, dubious chute)"`

### Task 11: Floor E (y=54) — **THE TUBE JUNCTION** + burst lobby

Glass transport tubes (glass pipeline): 5 tubes (1.2 m∅) rising wall-to-ceiling in a fan; one **pneumatic capsule** (brass pill, 0.9 m) whooshes through a fixed 4-point polyline (span pose, 6 m/s, 2 s pause at ends, `doorThunk` cue on arrival). Center: the **golden burst dais** under the shaft's roof extension — trigger 312 arms the keypad hint (HUD line "The roof is not the limit — 9999"). Tests: F10 capsule traverses + pauses. **Commit** `"factory: Floor E — Tube Junction + burst dais"`

### Task 12: Audio + ambience pass

`IAudioSystem` (asset-optional, rifthub pattern — silent if WAV absent): annex ambience loop (vol 0.25) on `--world factory`, per-room 3D cues at room centers (vat glorp 0.12 Hz random, conveyor clank, fizz, sorter servo, tube whoosh), `onRoofShatter` → glass-crash one-shot + wind loop at apex, keypad golden-unlock fanfare. All handles loaded via the existing `SMP1`-style asset probe; log-warn-and-continue on miss. **Commit** `"factory: audio pass (asset-optional cues per room + burst crash/wind)"`

---

## PHASE 4 — Gates, capture, docs

### Task 13: Full self-test sweep + leak gate
- [ ] `--test-elevator` E1–E11 green (E1–E6 byte-identical vs main — diff the captured output).
- [ ] `--test-factory` F1–F10 green; headless `--world factory` exit 0, **VMA live allocations 0**.
- [ ] `--test-rifthub`, `--test-city`, `--test-vehicle` unchanged (regression sweep).

### Task 14: Captures for Tim
- [ ] `--world factory --headless --screenshot` per floor (5 shots, camera table in the task) + `--capture-factory factory_tour.gif` (the lateral glass-wall approach). Post GIF to #x3native + #fleet-ops per the established pattern (upload → `m.image`).

### Task 15: Docs + ledger
- [ ] `specs/FACTORY.spec.md`: room tables above + elevator motion-contract addendum (segment profile, carryDelta semantics, Burst sequence).
- [ ] Blueprint §2.7 "The Confection Annex" (position, discovery beat, canon-safety note). STATUS ledger row.
- [ ] **Commit** `"docs: FACTORY spec + blueprint §2.7 annex"`

### Task 16: Branch & integration discipline
- [ ] All work on `feat/factory-annex` off **current `origin/main`**. Push (feature branch), then post the lane to #fleet-ops for the integration queue (current discipline: `integration/fold-*`), tagging @13700k. **Do NOT force-push anything; do NOT touch main.**

---

## Self-review (per skill)

- **Spec coverage:** elevator 3D travel ✔ (T1–T3), hidden unlock ✔ (T3), burst finale ✔ (T4), five distinct Dahl-style rooms ✔ (T7–T11), host+tests+capture ✔ (T6, T13–T14), canon safety ✔ (annex design decision), IP safety ✔ (original names throughout).
- **Placeholder scan:** B–E rooms use exact prop/anim tables + the demonstrated Task-7 pattern rather than repeated full listings — every number an implementer needs is in the tables; two explicitly-flagged implementer checks (sphere prim availability; letter-sign technique) are decision points with both options named.
- **Type consistency:** `Stop`, `buildEx`, `carryDelta()`, `unlockHidden()`, `armBurst()`, `onRoofShatter`, `FactoryTrigger` ids 300–313, `AnnexRoom` spans — names used identically across tasks. Internals cited from `origin/main:app/elevator.h` (verified 2026-08-15); implementer re-verifies `playerRiding` XZ source at T2 as noted.

**Estimated effort:** T1–T4 ≈ 1 session (Fable), T5–T6 ≈ 1, T7–T11 ≈ 2, T12–T16 ≈ 1. Fully parallelizable after T6 (rooms are independent files/functions).
