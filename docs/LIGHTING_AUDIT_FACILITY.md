# LIGHTING AUDIT — THE FACILITY (F1 Detention → F7 Executive)

**Branch:** `light/audit-facility` (based on `fix/honest-lighting-rooms` @ `e06ee05`, which is
NOT yet in `integration/playable-build` — this branch carries it).
**Method:** walk every room with the **flashlight OFF** (`--flashlight-off`), screenshot from
player eye height (y = 1.7), and **measure**: mean luminance, p05/p95 (contrast!), spread,
% pixels clipped to white, % pixels at or below 6/255 ("void"). Numbers are Rec.709 luma on the
final display-referred frame. Tool: `tools/lum.py`.
**Shots:** `docs/screenshots/lighting_audit/facility/`, BEFORE + AFTER, flashlight OFF.

> **The headline:** the facility had **four separate instances of the same disease** —
> *the lights were not where the player is, or could not reach him.* Every one of them was
> invisible until the 0.42 ambient wash died. **THE PATTERN held, four more times.**

---

## THE SCOREBOARD (flashlight OFF — measured, not eyeballed)

| Room | mean | p05 | p95 | spread | %void (≤6) | %clip | verdict |
|---|---|---|---|---|---|---|---|
| **Level 1 — corridor z=0** | 20.5 → 20.0 | 1.9 | 118 | 116 | 43% | 0.00 | unchanged **by design** (no regression on the tuned corridor) |
| **Level 1 — Cell 4, z=−22** | **5.9 → 27.3** | 1.1 | 121 | 18 → **120** | **83% → 44%** | 0.08 | a VOID became a room |
| **Level 1 — z=−25** | **6.8 → 30.3** | 1.1 | 143 | 19 → **142** | **82% → 32%** | 0.11 | a VOID became a room |
| **Level 1 — z=+30** | **8.4 → 40.0** | 1.0 | 155 | 24 → **154** | **70% → 30%** | 0.13 | a VOID became a room |
| **Canon — Jake's Cell** | 28.6 → 28.6 | 0.0 | 129 | 129 | 52% | 0.00 | **untouched** (ZNone / CellDressing owns it — correct) |
| **Canon — East Cell Hall** | **7.9 → 16.2** | 2.0 | 47 | 20 → **45** | **67% → 41%** | 0.02 | flat+dark → real pools |
| **Canon — W Service Corridor** | **7.3 → 11.9** | 1.0 | 37 | 20 → **36** | **70% → 52%** | 0.01 | improved, still the weakest room |
| **Canon — Main Hall** | **13.2 → 19.3** | 1.8 | 125 | 64 → **123** | 68% → 66% | 0.07 | real contrast now |
| **Canon — Security Station** | **8.3 → 10.9** | 1.0 | 32 | 20 → **31** | 68% → 56% | 0.00 | improved, still dim |
| **Canon — Ward corridor** | **23.6 → 40.6** | 0.0 | 161 | 99 → **161** | 63% → 59% | 0.00 | good |
| **Canon — Boss Arena** | **20.1 → 27.9** | 1.2 | 93 | 62 → **92** | 50% → 51% | 0.00 | good |
| **Rift Hub** | 67.0 | 5.0 | 179 | **174** | 6.6% | 0.13 | **already excellent — no change needed** |

**Nothing clips.** Max %clipped anywhere after the pass is **0.13%**. The wins are all in
*spread* (contrast), which is the thing that actually distinguishes lighting from a wash.

---

## THE FIVE DEFECTS FOUND (and fixed)

### 1. Level 1 — the ceiling rows still missed most of the plate (`app/env_art.cpp`)
`e06ee05` rescued the z≈0 play corridor by anchoring a light row on it and clamping the two
flanking rows to `z = ±min(zHalf*0.5, 7)`. **That clamp does not scale with depth**, so:

* **B1 / F1**: `zHalf` 38, rows at z = −7/0/+7, range 9.0 → **lit only to z = ±16**.
  A **22 m void on EACH SIDE — 58% of the plate depth.** The detention cells run down −Z to
  **z = −22 (Cell 4 "Skeleton")**, so whole authored ROOMS sat outside every light in the building.
* **F5** (drone high-bay): a 10 m void each side. **F6**: 4 m each side.

**Fix:** tile the z rows across the **full depth** at an 8 m pitch, phase-locked to z = 0 so the
corridor row stays exactly on the spine. 8 m pitch under a ≥9 m range ⇒ pools always overlap, at
any depth, on any plate. Fixtures 498 → **1140** (instanced meshes; the point lights were already
selected nearest-to-eye at K=44, so **GPU cost is unchanged**).

### 2. Canonlevel — the recipe keys **could not reach their own floor** (`app/room_dressing.cpp`)
`Recipe::keyRange` is a fixed constant that knows nothing about room height — but the key hangs on
the **ceiling**. Attenuation is `(1 − d/range)²`, so:

* **ZCorridor**: range 5.0, ceiling 4.5 → key at y = 4.15, floor 4.15 m below →
  `(1 − 4.15/5.0)² = 0.029`. **The floor receives 3% of the key.**
* **ZHall**: range 6.0 → 5% at the floor.
* **ZSecurity**: range 3.2 under a 4.5 m ceiling → **the key dies 0.9 m ABOVE the floor.**

The rooms were not under-lit; **they were lit by lamps that stopped in mid-air.** `env_art` already
had this right (`range = max(9, ceil + 4.5)`); RoomDressing never did.
**Fix:** `keyReach = max(rec.keyRange, dropToFloor + 4)`. Reach, not lumens.

### 3. Canonlevel — the whole zone table was keyed at **half** the building's own standard
Every practical that *works* in this building runs **3.2–3.3** (the cell tube 3.30 from `3f7e6d0`;
env_art's ceiling fixtures 3.2). The recipe table was authored at means of **0.7–2.4**. The rooms
weren't "moody" — they were **half-lit by authoring**, and the 0.42 wash hid it.
**Fix:** every zone key scaled so its **mean lands on 3.2, HUE RATIO PRESERVED EXACTLY** (a corridor
key is still the same cool blue-white; a ward key still the same warm amber — only the *level* moves).
Boost capped at 2.2×, which deliberately leaves **ZStorage (2.46), ZSalvari (1.94), ZOrganic (1.59)**
below standard: those rooms are *meant* to be darker, and the cap preserves that intent.

### 4. Canonlevel — a **black floor under a white ceiling** (`app/surface_library.{h,cpp}`)
`SurfaceLibrary::drawPanel` drew **every** surface at a hard-coded `baseColor = {1,1,1,1}` — no value
normalization at all. The library ships albedos spanning **0.041 → 0.618 linear, a 15× spread**:

| surface | linear albedo | used as |
|---|---|---|
| `sr_rubberfloor` | **0.041** | the ZHall / ZCorridor **FLOOR** — 4% reflectance, darker than asphalt |
| `mw_metal_grate` | **0.067** | the ZSecurity floor |
| `mw_metal_panels_a` | **0.618** | the corridor **CEILING** — nearly snow |

That is a black floor under a white ceiling, and it is exactly what the corridors rendered: the key
light *was* arriving, but **only the ceiling ever reported back**. Measured tell: a corridor whose
brightest 5% of pixels sat at sRGB 22/255 while its ceiling pools bloomed.
**Fix:** measure each albedo's mean linear reflectance at load and apply a **hue-preserving** value
tint that lands it in the band real interior materials occupy, `[0.08, 0.40]`. A surface already in
band returns exactly 1.0 and is **bit-identical**.

> **I got this wrong once, and the measurement caught me.** My first cut clamped **both** ends and
> made every corridor **measurably worse** (E Cell Hall 9.1 → 8.4, W Service 7.9 → 6.8, Security
> 9.3 → 7.7, all with *more* void). Reason: in a room whose key can't reach the floor, **the bright
> ceiling is the only surface reporting any light back to the player.** Dimming it removed the one
> thing carrying the frame. The ceiling clamp is only correct **together with** the key raise (#3).
> They now ship as one change. *This is why you measure instead of shipping the theory.*

### 5. The flashlight **did not work in the Rift Hub** (`app/app_run.cpp`)
`riftLights()` opens with `out.clear()` — the hub's rig "owns the budget". But `fl` at that point
**already held the flashlight** (inserted at the front). So walking into sub-level R1 **silently
deleted the player's torch every frame** — pressing `L` did nothing at all. The one room you reach
down a 33 m unlit approach corridor is the one room where your flashlight isn't connected to anything.
**Fix:** the torch is kept aside and re-inserted at the front after any takeover. Because
`riftLights()` returns a nearest-first list, the 2 entries the 64-cap now trims off the tail are the
2 *farthest* hub lights — exactly what we'd have chosen to drop.

### 6. (bonus) The elevator **capture** path was still first-in-build-order
`e06ee05` fed the live loop and the plain capture path nearest-to-eye but **missed the elevator
screenshot path**, which still did `= game.lightFixtures()` (all ~1.1k tower fixtures in **room
order**) then `resize(64)`. Every elevator capture photographed a correctly-lit cab against a shaft
and lobby lit by fixtures from unrelated floors — i.e. by nothing. **Fix:** `nearestFixtures(...)`.

---

## LIGHT-LIST AUDIT (authored vs. reaching the GPU)

| System | authored | reaches GPU | selection | verdict |
|---|---|---|---|---|
| `env_art` (Level 1, B1–F7) | 498 → **1140** | 44 | **nearest-to-eye** (K=44) | OK after fix #1 |
| `buildCanonLights` (generic per-room) | ~124 → **277** | ≤36 | nearest-to-eye | **mostly inert** — see note ▼ |
| `RoomDressing` (recipe rooms) | per-zone | ≤36 | nearest-to-eye, room-gated | **this is the real canon rig** |
| `CellDressing` (Jake's Cell) | 8 motivated | all (front-inserted) | — | **good; deliberately untouched** |
| `Elevator` cab | 1 practical + 4 disco | front-inserted | — | OK (disco-ball bug already fixed) |
| `Rifthub` + `RiftDepths` | ~57 + strips | ≤64 | nearest-to-eye | **excellent** |
| `SpireSubLevels` / `spire_mid` / `spire_top` / `secret_room` | **ZERO point lights** | — | — | ⚠️ **see P1 below** |

**Note on `buildCanonLights`:** I removed its arbitrary `min(3,…)` grid clamp (a 40 m cell hall got
3 lights spaced 16 m apart) — but then measured **zero change**, and found why: `app_run.cpp:1793`
**erases the generic light of every room that has a RoomDressing recipe.** So for nearly every room
that matters, `buildCanonLights` is dead code and **RoomDressing is the actual rig**. The de-clamp is
still correct for the non-recipe rooms it does serve, and it now can't be a trap for the next person.
Also raised the per-frame canon feed **16 → 36** (budget-checked against the 64-light device cap with
every other claimant paid first).

**Verified clean:** the elevator's disco-ball bug (the key sealed inside the ball) is properly fixed —
key at −0.40, ball shrunk to 0.36 m and hung at −0.75, with clear air. No sealed lights found elsewhere
in my region.

---

## WHAT I DID **NOT** FIX (and why) — PRIORITIZED

### P1 — 🔴 `secret_room`, `spire_mid` (F3/F4/F5), `spire_top` (F7), `spire_sublevels` (SL1–3) author **ZERO point lights**
Not one `PointLight` between them. They are lit **only** by:
* `env_art`'s plate ceiling fixtures — which the spire floors *do* inherit (they sit on the same
  `level1Rooms()` plates), so F3–F7 are now covered by fix #1; **but**
* the **sub-levels are NEW plates below B1** and the **secret room is carved below the cell** — neither
  is in the `level1Rooms()` table, so **neither gets a single env_art fixture**. They are currently lit
  by *flat self-emissive props and ambient 0.034*, i.e. **a fake self-emissive crutch and nothing else**.
  That is precisely the crutch the doctrine says to kill. **These rooms need a real light rig authored.**
  I did not do it because it is authoring new content, not fixing a defect — and it is a real art pass.

### P2 — 🔴 Level 1's graybox **prim walls take ZERO point light** (shared renderer — NOT mine to fix)
Measured in one frame, under the same fixture:

| surface | sRGB | luma | hue |
|---|---|---|---|
| ceiling (GLB) | (60.4, 53.3, 29.9) | **53.1** | warm — R>G>B ✅ lit by the tungsten key |
| floor (GLB) | (47.7, 43.0, 31.3) | **43.1** | warm ✅ |
| **wall (prim)** | (3.1, 5.0, 8.9) | **4.9** | **BLUE-dominant — B > G > R** ❌ |

**The hue is the tell** (the same lesson that solved the pink door, run backwards): a surface lit by a
warm fixture reads warm. These read **pure ambient blue**. They receive **no point-light diffuse at
all** — not dim, *zero*. Ruled out: it is **not** RT shadows (bit-identical with `--nortshadows`), not
metallic (the GLBs declare `metallicFactor = 0`), and not albedo (`SM_Wall_A` measures a healthy 0.355
linear). Root cause is in the **prim / dielectric shading path or the entity submit** — shared renderer
code owned by the `fix/engine-glb-lighting` agent. **Filed, not touched, to avoid a collision.**
This is the single biggest remaining facility defect: every interior partition wall in the legacy tower
is unlit.

### P3 — 🟠 The ModularSciFi emission-key bug is now **far more visible** (owned by another agent)
B11 / L7: the pack bakes its emission masks **into the albedo** as `(242,0,242)` magenta. My fix #1 lit
rooms that were previously black — and **the bug walked straight out into the light.** The Level 1
interior now reads as a **yellow-and-magenta checkerboard** across ceilings and floors. See
`AFTER_L1d_cell4_z-22.png`. **The lighting there is correct; the textures are not.**
**Level 1 will look WORSE, not better, until that convert-time fix lands.** This is expected and is not
a regression in my work — but Tim should know it before he judges the shot.

### P4 — 🟠 `sr_rubberfloor` is still the weakest surface in the game
Even lifted to the 0.08 floor of the band, the hall/corridor floor reads near-black at grazing
incidence (`p05 = 2.0` in the East Cell Hall). It is a genuinely bad choice of material for a floor
that a ceiling key has to graze. **Recommend replacing the surface**, not tinting it further.

### P5 — 🟡 The **club** (`camY < −150`) eats the flashlight exactly like the rift did
`club1127` does the identical `fl.clear()` takeover. **Not touched — that is the Descent/Club owner's
region (peer B).** Same one-line shape of fix as #5 above.

### P6 — 🟡 W Service Corridor + Security Station are still dim
11.9 and 10.9 mean, ~52–56% void. Improved but not *good*. They want practicals with visible housings
(P4's floor is half the problem).

---

## HONEST VERDICT — what I verified vs. what Tim must judge

**I read every PNG in this report myself.** What I actually saw:

* **Level 1's dark band is genuinely fixed.** z = −22 went from an unreadable black void to a room with
  a ceiling, a floor, walls, depth and falloff. **But it is now visibly drowning in the magenta/yellow
  emission-key bug (P3).** It reads *lit and wrong* rather than *dark and wrong*. That is progress, and
  it is not pretty yet.
* **The canon detention corridors are genuinely better.** The East Cell Hall now shows concrete grain,
  scratches, receding cell doors, real light pools and honest falloff — a moody, navigable prison
  corridor. **Its floor is still too dark.**
* **The Rift Hub was already excellent** and I changed nothing about its look (only that your flashlight
  now works in it).
* **Jake's Cell is bit-identical.** It was hand-calibrated in `3f7e6d0` and I deliberately did not touch it.
* **The Main Hall, Security Station and W Service Corridor are still darker than I'd like.** I improved
  them and I am not going to call them done.

**Still bad, plainly stated:** the legacy tower's prim walls are unlit (P2) and that is the biggest
remaining hole; the sub-levels and the secret room have **no lights at all** (P1); and Level 1's
textures will keep it ugly until the emission-key convert lands (P3).

---

## GATES
`--test-level1 · -canonlevel · -canonplay · -elevatorfsm · -rifthub · -secretroom · -holoterm · -glass · -ui`
— **all exit 0, 0 failures.**
Release `--smoketest` on **default + canonlevel + level1 + rifthub** — **all exit 0, 0 VUID,
allocationCount = 0.**
