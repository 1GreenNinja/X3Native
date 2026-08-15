# Spec: The Confection Annex + The Anywhere Elevator (feat/factory-annex)

> Game-layer (`app/factory_annex.{h,cpp}`, `app/factory_rooms.cpp`, `app/elevator.{h,cpp}` additive), engine/ stays pure. Plan: `docs/superpowers/plans/2026-08-15-dahl-factory-annex-plan.md`. Blueprint: §2.7.
>
> **CLEAN-ROOM / IP-SAFE:** Roald-Dahl-*style* whimsy, 100% original content. NO Dahl/Wonka names, characters, lyrics, or trademarks anywhere in code, assets, or strings.

## 1. Purpose
A hidden five-floor wonder-works — **The Confection Annex** — beside the Spire shaft, reachable only because the glass elevator learns to travel *sideways, slantways, and longways* (the **Anywhere Elevator** upgrade). Ends in the scripted **roof burst** finale over the world. Zero canon damage: the Spire floors (blueprint §2.4) are untouched; the Annex is the founder's private works behind the wall.

## 2. Placement & shell (one source of numbers: `FactoryAnnex` constants)

| Constant | Value | Meaning |
|---|---|---|
| `kAnnexXOff` | +60 m | annex center +X from the shaft (`annexX = shaftX + 60`) |
| `kFloorHalf` | 20 m | 40x40 m footprint per floor |
| `kClearH` | 11 m | clear height per floor |
| `kFloorCount` | 5 | floors A..E |
| `kFloorBaseY[]` | {2, 15, 28, 41, 54} | walking-floor Y per room (13 m pitch, `kFloorPitch`) |
| `kBoreY` | 15 m | bore corridor centerline == floor B baseY |
| `kBoreRadius` | 2 m | 4 m octagonal bore, brass-ribbed every 3 m |
| `kRoofY` | 65 m | roof plane (54 + 11 clear) — the burst shatter plane |
| `kCabHalf{X,Y,Z}` | 1.6 / 0.25 / 1.6 | cab half-extents (match elevator tests E7-E11) |

Dressing: aubergine iron (tint 0.16/0.14/0.19) + brass trim; the shaft-facing wall is a **glass curtain** so riders see the rooms during the lateral approach. Trigger ids **300-313** (`FactoryTrigger`): 300 bore entry, 301-305 room entry A..E, 310 sorter chute, 311 fizz low-grav, 312 burst dais, 313 tube ride. Rifthub owns 200-207; no collisions.

### 2.1 Phase-3 adaptations (documented deviations from the plan)
1. **Straight-Z river** (Floor A): the plan sketches the confection river "diagonally"; slab segmentation and `IPhysicsWorld::addBox` are axis-aligned (no yaw), so the channel is a straight sunken cut along the full Z extent at local x `[kRiverX0, kRiverX1]` = `[-16, -4]` from the annex center. Water surface `kRiverSurfY = 1.55` (below every walkable deck — the water plane is engine-global), channel bed `kRiverBedY = 0.65` (wading floor). The raspberry Gerstner params live on the annex (`riverWater()`); the **host** pushes them per frame with a host-advanced clock (deterministic captures).
2. **Span-major authoring**: every animated prop group is a *contiguous* Scene entity span recorded on its `AnnexRoom` (`propEntFirst/Count`, `glowEntFirst/Count`); `tick(dt)` pokes transforms/emissives in place — no per-frame heap, motion deterministic in `t` (bubbles/orbs/capsule rebuild their pose from formula, zero per-prop state).

### 2.2 The Chute of Dubious Quality (Floor D constants)
`kChuteX = 8, kChuteZ = 8` (local offsets), `kChuteHoleHalf = 1.1`. Floors B/C/D slabs carry the drop-shaft hole (7-segment slab variant); Floor A keeps its slab as the padded room's floor. The hatch is a **moved-static** brass plate (elevator-cab technique — its physics body slides with the visual). Trigger 310 latches; the annex room clock opens the hatch 1.5 s later; the drop lands in a padded room (`mw_thermal_padding`) with the box-serif emissive verdict sign "QUALITY: DUBIOUS". The Fizz low-grav zone is trimmed to ±7.5 so a falling player never clips trigger 311.

## 3. The five wonder-rooms (`app/factory_rooms.cpp`)

| Floor | baseY | Room (original names) | Accent (linear) | Content |
|---|---|---|---|---|
| A | 2 | **THE MIXTURE ATRIUM** | raspberry {1.00, 0.35, 0.55} | confection river (sunken channel, raspberry Gerstner water + 0.18 Hz under-glow), 6 copper vats ringing the center (rotating stir arms, 8 rim glow studs each), 2 stepped brass footbridges, pipe canopy |
| B | 15 | **THE INVENTION WORKS** | mint {0.40, 1.00, 0.60} | 8 machines (table below) + 14 m conveyor: 24 slats @ 1.2 m/s wrapping, carrying 8 emissive gizmo cubes; bore approach strip (x<0, \|z\|<3.2) kept clear |
| C | 28 | **THE FIZZ GALLERY** | amber {1.00, 0.72, 0.25} | 4 glass bubble columns at (±11, ±11), 10 rising emissive bubbles each (`makeUVSphere`, ONE shared unit sphere instanced 40x; 1.1 m/s rise, ceiling wrap, XZ sine wobble); 3 three-blade brass ceiling fans @ 0.6 rad/s; central low-grav zone (trigger 311, host jump ×1.8) |
| D | 41 | **THE SORTING HALL** | gold {1.00, 0.84, 0.30} | 12 orbs (10 gold, 2 dull duds) orbiting r=8 @ 0.5 rad/s over a brass torus track; 2 brass sorter arms sweeping ±1.2 rad @ 0.7 Hz (phase-offset) from posts at (0, ±11); the Chute of Dubious Quality (§2.2) |
| E | 54 | **THE TUBE JUNCTION** | cyan {0.35, 0.90, 1.00} | 5 glass transport tubes (1.2 m dia) fanning wall-to-ceiling; pneumatic brass capsule on a fixed 4-point polyline @ 6 m/s, 2 s dock pauses (ping-pong, deterministic in t; docking bumps `eventCount` for the host's thunk/whoosh cues); the golden **burst dais** (brass torus + 12-stud golden chase) under the roof extension, trigger 312 = HUD hint "the roof is not the limit - 9999", trigger 313 = boarding platform |

### 3.1 Invention Works machine table

| Machine | Footprint (m) | Anim | Params | Glow |
|---|---|---|---|---|
| Gum-Stretcher | 4×2×3 | piston (Y bob) | amp 0.9, 1.4 Hz | mint {0.4, 1, 0.6} |
| Fizz Compressor | 3×3×5 | spin (yaw) | 2.2 rad/s | amber |
| Idea Bellows | 2×4×2 | squash (scaleY) | 0.7↔1.15, 0.5 Hz | violet |
| Sprocket Fountain | 3×3×6 | spin + bob | 1.1 rad/s / 0.4 amp | brass |
| Wobble Boiler | 4×4×4 | sway (roll ±0.12) | 0.9 Hz | raspberry |
| Button Organ | 6×2×3 | key-chase emissive | 8 keys, 0.12 s step | white |
| Notion Centrifuge | 5×5×2 | spin fast | 4.0 rad/s | cyan |
| The Maybe Machine | 2×2×7 | random flicker | t*13.7 hash | gold |

## 4. Elevator motion-contract addendum (the Anywhere Elevator)

### 4.1 3D waypoint graph (additive; legacy `build()` is byte-identical)
- `ElevatorSystem::Stop { Vec3 center; const char* label; bool hidden; }` + `buildEx(..., stops, rails, startStop)` with **undirected adjacency-pair rails**. Legacy `build()` forwards here with x=shaftX/z=shaftZ on every stop and a full vertical rail chain; `m_stopsY` is kept as a derived mirror so every legacy read (`stopY`, `nearestStopTo`, strata) is untouched — `--test-elevator` E1-E6 output is byte-identical.
- Route selection in `callTo`: BFS over the adjacency, then travel the legs in sequence; **each leg runs the full accel/decel profile** (the cab pauses at corners — reads as deliberate; no curve math).
- **Hidden stops** stay off `callNext()`/button cycling until `unlockHidden()`; while locked they may still serve as route *waypoints* (the lock gates destination selection only).

### 4.2 Segment profile
Travel is a **single straight 3D segment per leg**: `m_segFrom/m_segTo` (stop centers), `m_segLen = |to−from|`, `m_segDir` unit, arclength `s ∈ [0, L]`. The existing `ElevTuning` trapezoid (maxSpeed 14, accel 6, decel 8, decelDist 8) advances **s** instead of `pos.y`; then `pos = segFrom + segDir·s`. Vertical graphs degenerate to today's math exactly (`L = |Δy|`).

### 4.3 `carryDelta()` semantics
- Legacy `float update(...)` **keeps its exact old semantics**: it returns the per-frame cab **ΔY** (which is `carryDelta().y`).
- `const Vec3& carryDelta() const` is the **full per-frame cab delta**. Hosts that ride lateral rails add the whole vector to every rider position (`playerRiding` footprint test reads the live `m_pos`, not build-time shaft XZ). Integrated over a leg it sums to the segment vector (test E9: (60, 0, 0) across the bore).

### 4.4 Unlock gating
Keypad code **`kAnnexCode` = 4790** (same handler as disco `1127`) → `unlockHidden()` + ding + the golden panel button lights (emissive 0.05 → 3.0). When the active leg's `|dir.y| < 0.3` the strata display swaps to a horizontal "ANNEX TRANSIT" panorama scroll driven by `s/L`.

### 4.5 The Burst sequence (`ElevState::Burst` = 10, the 11th state)
Config: `setBurst(fromStop, roofY, apexY)` — the factory host uses (A5, `kRoofY`=65, apex 105). Arm: keypad **`kBurstCode` = 9999** at the burst stop, or `armBurst()` (no-op unless FSM on, cab parked at the burst stop, state Idle/DoorsOpen). Sequence, all inside the FSM tick:
1. Doors seal (`DoorsClosing` with `m_burstPending`) → enter `Burst` phase 0.
2. Ascend at **2×accel**, uncapped to **1.6×maxSpeed**; decel window inside `decelDist` of the apex (floor 1.2 m/s).
3. On first crossing `roofY`: `m_burstFired` latches (once, ever) and **`onRoofShatter(cabPos)`** fires — the HOST wires FX/audio (GPU debris burst + `CombatFx::spawnExplosion` — this branch's fireball — + 6× `spawnImpact` glass ring; glass-crash one-shot + apex wind loop). The elevator stays render- and audio-pure.
4. Hover at `apexY`, hold **8 s** (phase 1).
5. Return = `Freefall` (the existing state), caught at `roofY − 2` by `emergencyStop()` (NOT the cable-slip's 14 m rule), then a normal `Arriving` glide back to the burst stop → `DoorsOpening`.

## 5. Host wiring (`app/world_hosts/host_factory.cpp`)
- Combined graph from **one builder**, `FactoryAnnex::makeElevatorGraph(shaftX, shaftZ)` (host, capture rig, and self-test all use it — no drift): stops `[F1, F3, A1..A5]`, rails F1↔F3, F3↔A2 (the lateral bore leg, flat at floor-B height), A1↔A2↔A3↔A4↔A5. A1-A5 hidden until 4790. Burst stop = A5.
- Rider carry via `carryDelta()`; low-grav ×1.8 jump while `lowGravActive()` AND standing in the Fizz zone; **one** merged `setPointLights` push per frame (annex rig + elevator rig); river water host-pushed; audio asset-optional (probe `assets/audio/factory/*`, fall back to committed cues, silent on double miss — windowed path only).

## 6. Gates
- `--test-elevator` E1-E11 (E1-E6 byte-identical vs main), `--test-elevatorfsm`, `--test-factory` F1-F15 + shell checks (24/24), `--test-rifthub` / `--test-city` / `--test-vehicle` regression-clean.
- Headless `--world factory --screenshot` exit 0, **VMA live allocations 0**.
- `--capture-factory [out.gif]` — the bore-ride money shot (rider's eye, F3→A2). `--capture-burst [out.gif]` — the burst finale (dais → ascent → roof shatter → apex hover). Both 640×360 @ 20 fps looping GIFs.
