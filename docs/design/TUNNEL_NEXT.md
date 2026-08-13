# Tunnels — open work, research, and the mountain diagnosis

Companion to `CITY_BORES_PLAN.md`. Written to survive a context reset: everything
here was in a conversation and would otherwise be lost.

---

## 1. THE MOUNTAIN — why it is inert, and how to fix it

A fifth `RangeDef` (the "tunnel ridge") is committed in `terrain.cpp` and has NO
effect on the corridor route. The report stays byte-identical: `road 12.3..16.4 m`,
`max cut 34.6 m`, `worst soil cover 2.46 m at s=218`.

### Already ruled out (do not re-check these)
* Geometry is right — the route centre (-592,-352) lies EXACTLY on the ridge
  spine, `segDist` = 0.
* The loop bound really is `kRangeCount` now. `mountainHeight()` hardcoded
  `i < 4`, so a 5th range WOULD have been silently ignored. That was a real bug
  and is fixed. (The first attempt to fix it silently missed on a line-ending
  mismatch — it was redone positionally and verified by grep.)
* `terrain.cpp` recompiled (obj timestamp newer than src).
* Only one `terrainHeightAt` definition exists (terrain.cpp:922).
* `host_tunnel.cpp` does use `worldTerrainConfig()`.
* **The TU is live**: the rock-splat change in the SAME FILE is plainly visible
  in the captures. So this is not a build/staleness problem.

### The leading hypothesis, untested
`app/terrain.h` says "With features on, MOUNTAIN heights intentionally exceed
heightScale". That wording implies a **features flag**. I only inspected
terrain.cpp lines ~928-940 around the `h += mountainHeight(...)` call and saw no
gate there — but a gate could enclose a much larger block higher up in
`terrainHeightAt`.

NEXT STEP, in order:
1. Read `terrainHeightAt` (terrain.cpp:922) **in full**, looking for a
   `cfg.features` / `cfg.bigFeatures` conditional enclosing the mountain call.
2. If found, check what `worldTerrainConfig()` sets it to, and what
   `--world tunnel` gets.
3. Fastest decisive probe regardless: log `mountainHeight(-592,-352,seed)`
   directly at boot. If it returns ~0, the range is being masked; if it returns
   60-200, then the height IS there and the corridor route is reading a
   different field than we think.

### If the range route stays blocked
Do not fight it. The corridor system already has a local, authored primitive
(`TerrainCorridor`) that modifies the field along a polyline. The same shape of
mechanism — an authored local UPLIFT registered at boot — would raise the hill
over the bore without touching the global range table, and would have a much
smaller blast radius. Ranges are 8-9 km features; a tunnel hill is a ~500 m one.

---

## 2. NFS RESEARCH (1994) — what they did, what we still lack

Tim: "if NFS could do it in 1992/1994/1996, research HOW and do the same or
better." Researched 2026-08-13. Sources at the bottom.

### What The Need for Speed (1994, 3DO) actually did
* **Scaled 2D bitmaps instead of geometry.** The road was drawn "out to infinity"
  using 2D bitmaps that scale as they approach, rather than fully polygonal
  environments — deliberately cutting polygon work so the 3DO's budget went to
  car rendering and physics.
* **Photo-digitised textures** wrapped over a low-poly skeleton. Their surfaces
  read as real asphalt/concrete because they WERE photographs, not authored
  patterns.
* **Several subtly different variants of one road texture**, cycled along the
  track — the era's standard anti-tiling trick. Enough detail to read as a
  surface, not so much that the repeat announces itself.
* **Guardrails as a single repeated texture** on trivial geometry, and roadside
  furniture placed on a track-relative rail.

### The uncomfortable finding
Our approach shot has **no roadside furniture at all** — no guardrail, no marker
post, no reflector, no signage, no distance marker. A 1994 game on 32-bit
hardware had all of it. Our cutting walls and shoulder are more physically
correct than anything they shipped and the frame still reads emptier, because
correctness is not presence.

### Where we can genuinely beat them
They baked everything; their roads were fixed at one time of day with painted-in
lighting. We have CSM, DDGI, RT reflections and a wetness model (on
`inspx/wetness`). A wet tunnel mouth at dusk reflecting its own light strips is
beyond any 1994 pipeline — but only once there is furniture and texture variation
THERE to reflect.

### Three items, in cost order (own lane, do not stall tunnel dressing)
1. **Roadside furniture on a route-relative rail.** We already have the spine and
   `posAt(s)`; placing guardrail segments, marker posts and reflectors at
   intervals along `s` is exactly their trick, and the corridor gives it free.
2. **Road texture variation.** Two or three `rd_asphalt_*` variants selected by a
   hash of `s` so the ribbon stops repeating. Direct port of the 1990s technique.
3. **Rock splat.** DONE (2d488fd2) — was `sr_concrete_01`, now `fw_rock_cliff`.

---

## 3. TUNNEL INTERIOR PROGRAM (Tim, 2026-08-13)

Requested: "sidewalks, access points, doors with keypads and rooms and stairs and
underground complex access, lighting — some LED, some burned out, some video
screens like CP2077".

This is a SUBSYSTEM, not dressing, and needs its own acceptance conditions before
any code. Captured here so the request is not lost.

### Scope sketch
* **Maintenance walkway** both sides of the bore, kerb height, with railing.
* **Access doors** in the bore wall at intervals, with working keypads. The
  engine already has `holo_terminal` and a keypad (`--test-keypad`, KP1-KP6) and
  a code-locked door chain (`secret_room`, `--test-hatch`) — REUSE, do not
  reinvent.
* **Rooms behind the doors**: plant rooms, cross-passages between bores, stairs
  down to an underground complex. Ties into the existing elevator//free-flight
  easter-egg work (task #9).
* **Lighting**: the bore already has 20 emissive strips + 6 real point lights.
  Add per-strip STATE — a deterministic hash of `s` marks some strips dead, some
  flickering. Dead strips cost nothing and read as maintenance neglect.
* **Video screens**: emissive panels with scrolling content. The engine has
  `membrane_flipbook*` sets in surface_library that could drive animated panels.

### Conditions to write before coding
* Lights: the interior must stay inside the point-light budget. Four bores with
  interiors could blow the 64-light cap outright — see `CITY_BORES_PLAN.md` B1.
* Doors/keypads must reuse the existing hatch/keypad chain and its self-tests.
* Every room must be reachable AND escapable (no soft-locks) — assert in a test.
* Burned-out strips must be DETERMINISTIC (hash of position), never random, or
  captures stop being reproducible.

---

## Sources
* The Need for Speed — https://en.wikipedia.org/wiki/The_Need_for_Speed
* Old School Gamer Magazine — https://www.oldschoolgamermagazine.com/the-need-for-speed-3do/
* Texturing a race track (Polycount) — https://polycount.com/discussion/48473/texturing-a-race-track
* Lou's Pseudo 3D Page — https://www.extentofthejam.com/pseudo/
