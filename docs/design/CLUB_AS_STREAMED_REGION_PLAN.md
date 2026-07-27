# Club 1127 as a Streamed Region — "One Continuous Earth" Design Plan

**Status:** PLAN ONLY. Execution is **gated** on Tim seeing the earth foundation first
(see GATE 0). No game code was changed to produce this document.

**Branch investigated:** `integration/unified` (clone at `D:/GameDev/x3-plan-region`).

**Goal (step 2 of the underground vision):** make the underground Club 1127 a
**region inside the seamless streamed region-graph world** — the same world as the
surface city — so the earth is ONE continuous body from the surface city down to the
deep club, enabling walk/dig surface → strata → club and a tunnel network carved
through the earth.

---

## 0. Reality check — what actually exists today (read before anything else)

Three premises in the vision do **not** yet exist in the code, and the plan is shaped
around that:

1. **There is no `club_bedrock` earth.** A repo-wide pickaxe over all ~190 branches
   (`git log --all -S'club_bedrock'` / `-S'bedrock'` over `app/ engine/`) finds
   nothing. "bedrock" appears only as dialogue text in
   `docs/design/narrative/chat_trees/danny.json`. The club is a **thin floor plate +
   wall shell**: `club1127.cpp:998` builds a 0.2 m floor slab at world Y = −200
   (`box(0, 0.0f, 0, CW/2, 0.1f, CL/2, ...)`), ceiling at −190.86, walls around the
   perimeter — **nothing solid below or around it.** So GATE 0 (below) is the
   "earth foundation" Tim must see first; this plan *designs* it because it isn't there.

2. **The world "earth" is a surface skin, not a solid volume.** The ground is a
   value-noise/fBm heightfield streamed as camera-centered tiles by `TerrainStreamer`
   (`terrain.h:262`); `TerrainConfig` (`terrain.h:59`) is `tileSize=32`, `heightScale=55`.
   Each tile is the **top surface + a non-colliding LOD "skirt"** (`skirtDepth ≈ 55 m`,
   `terrain.cpp:185`; collision is "LOD0 top surface only, skirts excluded",
   `terrain.cpp:1006`). There is **no subsurface / bedrock / depth** concept. Surface
   grade near the city (~XZ −200, 425) is the flattened **Industrial Zone pad
   `kPads[3] = {-200,350,r150,padY=17}`** → surface **Y ≈ +17 m** (`terrain.cpp:394-405`).

3. **The engine has no CSG / boolean / voxel / SDF geometry.** A search of `engine/`
   for `CSG|boolean|meshSubtract|voxel|SDF|marching` finds only a Lua `boolean` type
   and unrelated GI/ray comments. **Runtime "boolean-subtract a tunnel out of solid
   earth" is not a capability that exists.** All world/club geometry is *additive box
   primitives*. This is the single most important constraint in the plan: "carving"
   here means **authored negative space + keep-out volumes**, not booleans.

The vertical stack today (verified):

```
   Y ≈ +17 m   surface city grade (Industrial Zone pad)
   Y =    0 m   facility base = elevator-shaft top = strata top (strata.h:64 kStrataTopY)
       │        ┌─ StrataWorld shaft (radius 14–16 m) around the elevator column:
       │        │  canted rock-slab rings + 5 offshoot tunnels + a 26-ledge on-foot
   ~217 m of    │  spiral, bands compressed into 0..−200 (strata.cpp:298-388).
   EMPTY scene  │  Today this is the ONLY "earth" between surface and club.
   space today  │
   Y = −200 m   └─ strata bottom cap = club ceiling line (strata.cpp:327-333)
                   Club 1127 floor (club1127.h:77 kClubY = −200)
```

---

## 1. How the streamed region-graph world works (the system we're joining)

**Data model** — `assets/world/regions.json` (dev, `--world streamed`) and
`assets/world/regions.canon.json` (the real game, `worldRegionsCanonJsonPath()`,
`world_stream.h:94`). A region (`WorldRegionDesc`, `world_stream.h:64`) is:

- `id`, `name`, `neighbors[]` (graph edges);
- **content ref** — EITHER `builder` (a code-built world: `"city"|"oceanbase"|"worldregions"`,
  `world_stream.cpp:266-271`) OR `leveldoc` (a project-JSON path + `floor`);
- `anchor[3]` world meters, `radius` (footprint), `loadRadius`/`unloadRadius`
  (hysteresis pair measured from the footprint EDGE).

**Residency** — `WorldStreamer` (`world_stream.h:114`):

- `buildStartRegions(x,y,z)` (`world_stream.cpp:373`) synchronously builds every region
  whose footprint contains the spawn; neighbors stream in afterward.
- `update(px,py,pz, vx,vy,vz, budgetMs, ...)` (`world_stream.cpp:389`): computes wants
  from position + velocity·lookahead vs each region's load/unload radii; kicks async
  LevelDoc parses; realizes **at most one** region/frame and advances chunked
  evictions under the per-frame ms budget; maintains a soft proxy collision floor.
- `realize()` (`world_stream.cpp:236`) brackets the builder call in
  `Scene::beginEntityCapture` so an **ownership ledger** records every entity/mesh/
  texture/body; `evictSlice()` tears exactly those down and returns Scene::size to
  baseline. Neighbor-warm preloading (×1.5 loadRadius) at `:405-414`.
- **Region hooks** (`setRegionHooks`, `world_stream.h:180`): `onBuild` fires *inside*
  the capture window after the content builder (host-owned systems join the ledger);
  `onTeardown` fires before any slot is released (host abandons its ids). This is how
  the city street crowds live inside the `city` region (`app_run.cpp:3331`).

**⚠ THE decisive property — residency is XZ-only. Y is ignored everywhere:**
`distToFootprint()` uses only `dx,dz` (`world_stream.cpp:159-163`); `buildStartRegions`
does `(void)y;` (`:376`); `update` does `(void)py;(void)vy;` (`:394`). The proxy floor
is dropped at `anchor[1]` (`:350`).

**The canon game already runs this AND already fights the underground:**
`app_run.cpp:1418` instantiates `canonWstream`; `:3320-3331` inits it against
`regions.canon.json`, sets the exterior room tag (`kStreamedExteriorRoom`), a shared
surface library, and the city-crowd region hooks; `:9641-9663` ticks it — **but only
while `camY > kStreamSuppressBelowY` (−20 m)** (`:1424`, `:9642`). The in-code comment
is the exact problem statement for this plan:

> *"Risk 3 (XZ-only residency vs the underground): suppress ALL residency work while
> the eye is below this Y — the elevator/strata/Club-1127 descent must never see a
> region teardown or the streamer's Y=0 proxy floor."* (`app_run.cpp:1421-1424`,
> `:9636-9640`).

So the streamer and the descent already coexist in one process; they're just kept
apart by a Y cutoff. Making the club a region means **retiring that cutoff properly.**

**Adding a new region takes:** (a) a JSON entry in the canon graph; (b) if code-built,
a `builder` branch in `realize()`; (c) for anything stateful/animated, a host-owned
system driven each frame + region hooks — nothing more. No new host, no `--world`.

---

## 2. The surface city region (where we hang the club under)

`regions.canon.json` region **`city`** (builder `"city"`, `City::build`, dispatched at
`world_stream.cpp:267`). In `regions.json` it is anchored `[-200, 0, 425] r750`
(`regions.json:18-26`) — "Scrapyard City / New District / Industrial Zone + freeway
tunnels". Surface grade over that footprint is the pad system, **Y ≈ +17 m** near
(−200, 425). The facility/spire tower is at the canon tower footprint
(x[−3..47], z[−34.5..55.5]) with the elevator shaft inside it; the club shell today is
built at **XZ origin (0,0)** (every `box(0,...)` in `club1127.cpp`). **Coordinate audit
needed** (Risk R7): the club build-XZ and the descent-shaft XZ must be reconciled so
the region anchor, the descent, and `club.spawn()` all line up.

---

## 3. The club world (what we're relocating into the graph)

`Club1127World` (`club1127.h:74`) builds "THE DEEP" at `kClubY = −200`, footprint
`kCW=30.48` (X) × `kCL=13.106` (Z) × `kCH=9.14` (Y); world bounds X∈[−15.24,+15.24],
Z∈[−6.55,+6.55], Y∈[−200,−190.86]. It is a self-contained, **stateful, animated**
object: the ORB spins, spotlights orbit, blacklights pulse, dancers are choreographed,
OLED panels shimmer, sub cones pump — all in `update()` which **re-pushes the point
lights every frame** (`club1127.h:128-130`). It owns GLB GPU handles for the app
lifetime (`m_chars`, `club1127.h:301`). It also carries the Jukebox, 3 canon dialogue
NPCs (Danny/Amara/Emma) + chat trees, and E-to-talk.

**What `--world club` (`host_club.cpp`) assumes that a region CANNOT provide:**

- **Its own physics world, device teardown, camera, spawn, and main loop**
  (`host_club.cpp:39-46`, `:225-227`, `:285-421`). A region has none of these — it
  realizes into the *surviving* streamed scene/device.
- **Global render/env state set once for the whole device:** sky **disabled**, IBL
  probe ON, violet ambient, `iblIntensity 0.20`, `iblSpecular 1.30`, `bloom 0.16`,
  `exposure 1.0` (`host_club.cpp:110-123`). In the streamed world these are the SAME
  global states the sunny surface uses — the club must not paint the surface violet.
- **Per-frame `club.update()` + `drawCharacters()` + `setPointLights()` + jukebox +
  chat** (`host_club.cpp:186-197`, `:307-418`). `realize()` is fire-and-forget; it has
  no per-frame hook for a living object.

The good news: the canon game **already** solved most of this for the manual path.
`app_run.cpp:1511` owns a `Club1127World club1127`; `:8675` lazy-builds it on the 1127
disco code; `:8987`/`:9079` tick it; `:9081-9092` teleport the rider to
`club1127.spawn()`; and `clubAtmoOn` (`:1516`) latches the interior IBL/ambient ONCE on
entry and restores it ONCE on exit. The region version reuses these same mechanisms,
driven by "club region resident / eye inside the club" instead of the lazy-build latch.

---

## 4. The descent / strata connection (where the tunnel region sits)

- **StrataWorld** (`strata.h`, `strata.cpp`) is the geological shaft built AROUND the
  elevator column: `kStrataTopY=0` … `kStrataClubY=−200` (`strata.h:64-65`), radius
  14 m standalone (`host_strata.cpp:45`) / 16 m live (`app_run.cpp:1687`). It has
  canted rock-slab rings per band (`strata.cpp:75-137`), a **cap floor at Y=−200 that
  IS the club-ceiling seam** (`strata.cpp:327-333`), 5 walkable offshoot tunnels at band
  boundaries (`strata.cpp:338-344`), a continuous **on-foot spiral (26 ledges, 0..−200)**
  (`strata.cpp:364-388`), and a `ClubArrival` trigger (id 207) at the bottom
  (`strata.cpp:397-399`). Crucially it already has **`setKeepOut()`** (`strata.h:154-163`)
  which carves rock away from the rift corridor + deep rooms so slabs don't intrude —
  **this is the existing "dig" primitive** (authored keep-out, not CSG).
- **Elevator** — `kDiscoCode="1127"` (`elevator.cpp:135`) drives the cab to
  `m_clubStopY = −200 + halfY` (`elevator.cpp:334`, overridden to −199.85 at
  `app_run.cpp:1673`). The elevator does NOT itself spawn into the club; the live loop
  teleports the rider to `club1127.spawn()` when the cab lands, doors >90% open, rider
  aboard (`app_run.cpp:9081-9092`).
- **descent_slide** is a SEPARATE coaster (`descent_slide.cpp`) bottoming in a crystal
  cavern at **Y=−178** (`kBowlY`, `descent_slide.cpp:57`), ~22 m *above* the club — not
  the club route. Leave it out of scope.

**A "descent/tunnel region" therefore has a canonical, pre-existing footprint:** a
cylinder of radius ~16–18 m on the elevator-shaft XZ spanning **Y ∈ [−200, 0]** — i.e.
`StrataWorld` itself, wrapped as a streamed region.

---

## 5. Coordinate strategy

Because builders build at their **own hard-coded internal coordinates** (City/OceanBase
ignore `anchor`; the club builds at its local origin offset to Y=−200), the region
`anchor` in JSON is used **only** for footprint distance + the proxy floor. Two
consequences drive the whole coordinate plan:

1. **`anchor` XZ MUST equal the club's true world build-XZ** (currently (0,0); pending
   the R7 audit it should be the descent-shaft XZ). Otherwise residency triggers in the
   wrong place.
2. **Set `anchor[1] = −200` (the club floor), NOT 0.** This is the single most important
   number: the soft proxy floor is dropped at `anchor[1]` (`world_stream.cpp:350`), so a
   Y=−200 anchor puts any fallback plane *at the club floor* instead of the "Y=0 plane
   over The Deep" the current code explicitly fears (`app_run.cpp:9640`). The club's own
   local origin already maps to Y=−200 via its `oy=kClubY` offset (`club1127.cpp:991`),
   so no content moves.

Proposed placement (canon graph):

| region   | builder | anchor (x, y, z)        | radius | loadR | unloadR | yMin..yMax (new) |
|----------|---------|-------------------------|--------|-------|---------|------------------|
| `club`   | `club`  | `[shaftX, -200, shaftZ]`| ~22    | ~90   | ~150    | −210 .. +5       |
| `descent`| `strata`| `[shaftX, -100, shaftZ]`| ~18    | ~120  | ~200    | −205 .. +5       |

`radius 22` covers the club shell (15.24 × 6.55) plus the engine-room addition with
margin. `loadRadius 90` gives the club time to realize during the slow disco descent.
The `yMin..yMax` band is the proposed new field (Section 6 / step 1); with it, `club`
is wanted only when the eye is in the deep band, so standing in the surface city at
Y=+17 over the same XZ does NOT stream the club (and vice-versa).

---

## 6. Region registration + streaming-budget impact

1. **JSON:** add the two rows above to `regions.canon.json` (the real game). Optionally
   mirror into `regions.json` for the dev `--world streamed` harness. Neighbors:
   `club ↔ descent`, `descent ↔ city`/spire, so neighbor-warm preload
   (`world_stream.cpp:405`) pulls the club in as you approach the shaft.
2. **Builder branch:** in `realize()` (`world_stream.cpp:266-275`) add
   `else if (r.desc.builder == "club") { /* build via the host-owned instance, below */ }`
   and `else if (r.desc.builder == "strata") { StrataWorld ... }`. Because the club is
   **stateful and app-lifetime-owned**, do NOT construct a throwaway `Club1127World`
   inside `realize()`. Instead build it through the **region hook** against a host-owned
   instance (mirrors the city crowds, `app_run.cpp:3331`): the `onBuild` lambda for
   `id=="club"` calls `club.build(scene,device,physics,...)` inside the capture window
   so its shell entities join the region ledger and get the room tag; `onTeardown`
   calls a new `Club1127World::abandon()` that forgets the scene ids **without freeing
   the GLB GPU handles** (so re-entry doesn't re-decode). The living systems
   (lights/jukebox/dancers/NPCs/`update()`) are host-owned and gated on residency.
3. **Budget impact:** the club is heavy (~hundreds of emissive boxes, ~28 blacklights,
   PA rig, dancers, many point lights). Two mitigations: (a) **pin the club** — mark the
   region non-evicting once resident (footprint is tiny and `kCullDist=80` already
   distance-culls its draw), so you pay the realize **once** and never thrash GLB loads
   (the "city 2 s realize hitch on re-stream" precedent, `world_stream.h:270-276`);
   (b) keep the single-realize-per-frame budget gate — the club is one monolithic
   builder call (the documented atomicity floor, `world_stream.cpp:475-479`), so expect
   one budget-overshoot log line on the entry frame, masked by the disco descent.
   Watch the **point-light cap** and HUD record coalescing (descriptor-pool exhaustion,
   `bac85083`) while club + surface are briefly co-resident at the band boundary.

---

## 7. What breaks running as a region instead of `--world club`, and the fix

**Top 3 (the load-bearing ones):**

1. **XZ-only residency underground + the Y=0 proxy floor.** The streamer can't tell
   "surface city" from "club" — they share XZ — and its fallback floor is at `anchor[1]`.
   *Fix:* (a) set the club anchor `y=−200` (proxy lands at the floor); (b) add an
   **optional Y-band** (`yMin/yMax`) to `WorldRegionDesc` and factor it into the want
   computation in `update()` (keep XZ distance as the horizontal cull; gate `wanted` on
   `py`+`vy·lookahead` ∈ band). This retires `kStreamSuppressBelowY` (`app_run.cpp:1424`)
   — the club/descent regions stream by depth, the surface regions use an infinite
   default band and are unaffected. Small, surgical change to one file + the JSON schema.

2. **Club lifecycle vs. fire-and-forget `realize()`.** The club must be `update()`d and
   drawn every frame and must keep its GLB handles alive; `realize()` gives neither.
   *Fix:* host-owned `Club1127World` + region hooks (build in the capture window,
   `abandon()` on teardown) + a per-frame **resident-gated tick** in the streamed host's
   loop: `if (clubRegionResident) { club.update(...); club.drawCharacters(...);
   device.setPointLights(club.pointLights()...); jukebox.update(...); /* E-to-talk */ }`.
   Add `Club1127World::abandon()`. This is the exact city-crowds pattern generalized.

3. **Global env/sky/IBL state bleed.** The club sets device-global sky-off + violet IBL
   + bloom; the surface wants the sun sky. *Fix:* drive the club atmosphere by **camera
   location**, not by region build — reuse the existing `clubAtmoOn` latch
   (`app_run.cpp:1516`): apply the club IBL/ambient/exposure/bloom ONCE when the eye
   enters the club region/band, restore the world sky ONCE on exit. Never per-frame,
   never per-build.

**Also (secondary):**

- **No `--world` switch at all** → this **entirely avoids** the shared-device teardown
  segfault class fixed in `a865a525` (a host destroying the shared `VkDevice`/window
  before main() re-dispatched into the next host → use-after-free in
  `TerrainStreamer::init`) **and** the club-transition descriptor-pool follow-up. A
  streamed region realizes into the *surviving* device/scene; there is no second host,
  no device shutdown, no re-dispatch. **Call this out as the headline win of the region
  approach.**
- **Player entry** is already solved and stays: elevator 1127 → cab to −199.85 →
  teleport to `club.spawn()` (`app_run.cpp:9081-9092`). The only new requirement is that
  the club region be **resident by arrival** — guaranteed by loadRadius 90 + kicking the
  want on the accepted 1127 code (pre-stream during the slow ride).
- **Physics:** the club shell's static bodies join the region ledger and are removed on
  teardown like any region content — no separate physics world (drop `cphys`).
- **Headless screenshot paths** (`--world club`, `--screenshot-crowd`) should be KEPT as
  a standalone host for authoring/QA; the region path is additive, not a replacement.

---

## 8. The continuous earth — how the club's earth joins the world's earth

Neither a subsurface solid nor a carve capability exists (Section 0), so a single global
solid earth is **not** on the table for this sprint. Two options:

- **Option A — one unified solid earth (NOT recommended now):** replace the terrain
  surface-skin with a true volumetric earth (voxel/SDF) down to −200+. This is a
  multi-week engine epic (new meshing, new collision, new streaming) and is the ONLY
  path that supports literal runtime boolean digging. Defer to a future epic.

- **Option B — region-carried earth SLICES seamed into one body (RECOMMENDED):** the
  "continuous earth" is an **illusion assembled from adjacent authored slices** that meet
  at shared Y planes, exactly matching the engine's additive-primitive reality:
  - the **surface** is the heightfield skin (top at Y≈+17; its ~55 m visual skirt
    already reaches toward Y≈−38);
  - the **descent region** carries `StrataWorld`'s rock shell for Y ∈ [−200, 0] (canted
    rings + bores) — this is *already built earth*, just needs region-wrapping;
  - the **club region** carries a NEW thick **`club_bedrock`** shell — rock boxes
    beneath and around the club room (floor slab down to, say, −212; a surrounding rind
    outside the walls) that **seams to the strata cap at Y=−200** (the existing
    club-ceiling seam plane, `strata.cpp:327-333`).
  Seams are shared Y planes (surface-skirt bottom → strata top at 0; strata cap →
  club_bedrock top at −200), so the player reads one continuous mass while each region
  owns and tears down only its own slice. **This is GATE 0 — the earth foundation Tim
  must see first.**

---

## 9. The tunnel network — the repeatable "dig" recipe (no CSG)

Because there are no booleans, a tunnel is **authored negative space + a keep-out
volume**, and `StrataWorld` already ships the primitives (offshoot tunnels
`strata.cpp:338-344`; `setKeepOut()` `strata.h:154-163`). Repeatable recipe:

1. **Path:** pick waypoints, e.g. city (XZ, Y0) → shaft (XZ, −100) → club (XZ, −200).
2. **Bore geometry:** sweep a corridor of box segments (floor + two walls + ceiling)
   along the path — reuse the strata offshoot-tunnel + spiral-ledge builders.
3. **Keep-out:** register the corridor interior as a keep-out on the surrounding
   earth-slice builders (`setKeepOut`) so the rock slabs don't poke through the walkable
   bore. This is the "subtract," done at author time, not by CSG.
4. **Wrap:** put the corridor in a streamed region (or fold it into `descent`) with a
   Y-band footprint; seam its ends to the neighbor regions at shared Y planes.
5. **Dress:** collision from the box walls; lighting from placed fixtures (no global
   sky); a room tag if it draws near an interior PVS boundary.

Doing this at author time (a tool/script emitting the corridor + keep-out) IS the "dig"
until/unless Option A voxel earth lands. If Tim wants *runtime, in-game* digging, that
is explicitly Option A and out of this sprint's scope.

---

## 10. Step-by-step execution plan (each step independently gate-able)

- **GATE 0 — the earth foundation (Tim's prerequisite).** Author `club_bedrock`: a thick
  rock shell beneath + around the club room, seamed to the strata cap at Y=−200, and
  render it under `--world club` so **Tim can SEE the continuous earth** before any
  streaming work proceeds. *(Everything below is gated on Tim approving this.)*
- **Step 1 — Y-band residency.** Add optional `yMin/yMax` to `WorldRegionDesc` + JSON
  schema; factor into the `update()` want computation; default = infinite band (surface
  regions unchanged). Retire `kStreamSuppressBelowY`. Extend `--test-worldstream` with an
  underground band case (surface region NOT resident when deep; deep region NOT resident
  at surface; no proxy over The Deep).
- **Step 2 — club as a streamed builder.** Add the `"club"` (and `"strata"`) branch to
  `realize()`; host-owned `Club1127World` via region hooks; add `Club1127World::abandon()`;
  per-frame resident-gated tick for update/draw/lights/jukebox/NPCs; pin the club region
  (non-evicting).
- **Step 3 — atmosphere handoff.** Drive club IBL/sky/ambient/bloom/exposure by camera
  location via the existing `clubAtmoOn` latch — once on entry, restore on exit.
- **Step 4 — register + entry test.** Add the `club` row to `regions.canon.json`
  (anchor `[shaftX,−200,shaftZ]`, r22, band). Descend via elevator 1127; confirm the club
  streams in during the ride (no `--world` switch, no device teardown), and the
  teleport to `club.spawn()` lands on resident geometry.
- **Step 5 — descent region.** Wrap `StrataWorld` as the `descent` region spanning
  Y[−200,0]; seam top to surface, bottom cap to `club_bedrock`. Confirm a continuous
  walk surface → strata spiral → club with no loading screen.
- **Step 6 — tunnel network.** Author 1–2 tunnels (city↔strata, strata↔club) via the
  keep-out corridor recipe (Section 9); fold into `descent` or their own regions.
- **Step 7 (FUTURE / optional) — true carvable earth.** Voxel/SDF terrain epic; only if
  runtime digging is wanted. Weeks of engine work; separate plan.

---

## 11. Risks / gotchas

- **R1 — XZ-only residency is load-bearing** across the streamer, the world map tile
  bake, and the canon host. The Y-band add must default to "infinite band" so the 4
  surface regions are byte-for-byte unchanged; guard with the existing self-tests.
- **R2 — GLB-handle thrash / realize hitch.** The club owns app-lifetime GLB handles;
  naive evict/re-realize re-decodes them (the city 2 s hitch precedent). Pin the club +
  make `abandon()` cheap (forget ids, keep handles).
- **R3 — global render-state bleed.** Sky/IBL/ambient/bloom/exposure are device-global;
  the club's violet interior must be strictly camera-location-gated and restored on
  exit, or the surface goes dark/violet at the band boundary.
- **R4 — no CSG.** "Continuous carvable earth" = authored slices + keep-out, NOT
  booleans. Setting this expectation is essential — the "boolean-subtract" framing is not
  supported by the engine today (Section 0.3).
- **R5 — proxy floor.** With `anchor.y=−200` the fallback plane sits at the club floor;
  verify the band residency keeps the club resident through the whole descent so the
  proxy never engages mid-ride (`proxyEngageCount()==0` in the entry test).
- **R6 — descriptor-pool / point-light spikes** when club + surface are briefly
  co-resident at the boundary (HUD coalescing `bac85083`; the club adds many point
  lights + emissive entities). Budget-check the entry frame.
- **R7 — coordinate reconciliation.** The club shell builds at XZ (0,0); the canon
  elevator shaft is not at (0,0). Either relocate the club build to the shaft XZ or set
  the region anchor to the true build XZ and land the descent there. Audit before Step 4.
- **R8 — graph choice.** Register in `regions.canon.json` (THE game; no `spire_f1`,
  tower self-built), not just the dev `regions.json`.

---

## 12. Effort estimate

| Step | Scope | Effort |
|------|-------|--------|
| GATE 0 | author `club_bedrock` earth + view under `--world club` | S–M (~0.5–1 day) |
| 1 | Y-band residency + tests | S (~0.5 day) |
| 2 | club-as-builder + hooks + lifecycle + `abandon()` | M (~1–2 days) |
| 3 | atmosphere handoff (pattern exists) | S (~0.5 day) |
| 4 | region registration + descent stream test | S (~0.5 day) |
| 5 | strata/descent region wrap + seams | M (~1 day) |
| 6 | tunnel network (keep-out recipe) | M (~1 day per tunnel) |
| 7 | FUTURE voxel/SDF earth (runtime dig) | L (weeks — separate epic) |

**Core (GATE 0 → Step 5): ~4–6 focused days.** Tunnels + polish are additive. The
region approach is *lower risk than the current manual lazy-build* because it deletes an
entire failure class (the `--world` switch / shared-device teardown), reuses proven
patterns (city crowds, `clubAtmoOn`, `setKeepOut`, the existing strata shaft), and its
one genuinely new engine change (Y-band residency) is small and self-contained.
