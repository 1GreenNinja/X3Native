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

> STATUS 2026-08-13 (later): the acceptance conditions now EXIST, written
> before any code as required — see `TUNNEL_INTERIOR_PLAN.md` (verified
> constraints incl. the exact 1.0 m walkway band and the <=0.55 m niche depth,
> W/D/L/S/B/E conditions with negative controls, execution order, and the open
> questions for Tim). The sketch below is superseded by that plan.

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

## 4. TIM'S DEFECT LIST (2026-08-13, approach_mountain.png)

Seven called out. Status after 8537fbed + the follow-up:

- [x] **Strips up the slope — DELETED.** Not tile skirts (my earlier guess) and not
      the summit trench. It was MY OWN embankment from e0edbfcd: the batter clamps
      each step with `max(y, ground)` so it cannot sink below grade, which is right
      for a downhill FILL and catastrophic in a CUT. Under the mountain the ground
      beside the road is ~100 m higher, so that max() drove the shoulder strip
      straight up the hillside — two rails, two strips. Now guarded: no batter
      where the ground is already at or above the road, because a cutting face IS
      the terrain.
- [x] **Road-shaped cutout across the mountain top — FIXED.** The bored branch took
      `min(coverAvail - kBoreCut, kTcMaxScar)`. Fine for a 55 m hummock; under a
      124 m mountain coverAvail is enormous so it pinned at kTcMaxScar and gouged a
      trench along the summit. A tunnel does not carve the mountain above it —
      depth is now 0 on a bored reach.
- [x] **Dark angular wedge on the upper-left slope — NOT A DEFECT.** Tim identified
      it as the cliff face where the cutting wraps around the hill. My headwall
      guess was wrong; the terrain is behaving.
- [ ] **Car is too small and faces the wrong way.** Showcase prop in host_tunnel.
- [ ] **Mountain is too smooth** — needs jagged peaks, not a dune. `ridgeExp` and
      `jagAmp` on the tunnel-ridge RangeDef are the levers (currently 1.7 / 26).
- [x] **Mountain has no MOUNTAIN COLOURS** — DONE (f33daf0e) except trees. The
      splat gained an optional FIFTH texture slot: `terrain_rock_dark`
      (UniStorm Rock_2, dark blue-grey craggy slate) crossfades in over
      70..150 m with the blue-vein luminance remap on top, and snow was
      retuned 180/265 -> 118/185 because the ridge peaks ~162 m — UNDER the
      old snow floor. Saddle capture shows dark craggy summit stone with snow
      pockets. The stale WARNING that the altitude tint "changes ZERO pixels"
      was disproven by a garish-red probe: the x3shaders copy-fix had already
      cured the stale-spv trap. Trees still open (vegetation scatter).
- [ ] **Strange non-concrete artifact at the roadside** — may be resolved by the
      fill-only guard above; needs a fresh look.
- [x] **Grass or rock ON the road** — CONFIRMED and FIXED: it was §7's earth
      ramp (the portal-sweep residual). Cut by the portal plugs + mesh portal
      holes; see §7's outcome note.

## Sources
* The Need for Speed — https://en.wikipedia.org/wiki/The_Need_for_Speed
* Old School Gamer Magazine — https://www.oldschoolgamermagazine.com/the-need-for-speed-3do/
* Texturing a race track (Polycount) — https://polycount.com/discussion/48473/texturing-a-race-track
* Lou's Pseudo 3D Page — https://www.extentofthejam.com/pseudo/

---

## 5. THE CAR — gyroscope wheels + facing left (2026-08-13, Tim driving)

Tim drove it in `--world tunnel`: "the car moves, the wheels spin, but they spin
SIDEWAYS, lookin like a gyroscope" and "it moves forward when I accelerate".

THAT LAST PART IS THE DIAGNOSIS. Physics forward is CORRECT — the rig drives the
way it faces. So nothing is wrong with the vehicle sim or the tuning; this is
purely the SKIN transform, i.e. two hard-coded constants in app/vehicle.cpp that
encode assumptions about how the GLB was authored:

    // "Mesh-local wheel axis is +-X (car lateral); the physics wheel pose maps a
    //  unit Y-cylinder (axis = axle). Rotate mesh X onto pose Y (Rz +90deg)."
    kWheelAxisFix[16] = { 0,1,0,0, -1,0,0,0, 0,0,1,0, 0,0,0,1 };
    kBodySkin[16]     = { -1,0,0,0, 0,1,0,0, 0,0,-1,0, 0,kBodyDropY,0,1 };  // 180 deg nose flip

If THIS car's wheels are not authored with the axle on +-X, that Rz+90 rotates
them OFF the axle rather than onto it — a wheel spinning about an axis 90 deg
from its axle is precisely a gyroscope. Same class for the body: the 180 deg flip
assumes a nose direction this model may not share, which is the "pointed left".

NEXT STEPS, cheapest first:
1. Inspect the car GLB's node axes directly (which local axis is the wheel
   cylinder on, which way does the body nose point). Do NOT guess — the whole bug
   is a guess about authoring that turned out wrong for this asset.
2. Try kWheelAxisFix = identity. If the wheels then spin correctly, the GLB's
   wheels are already axle-aligned and the "fix" was the defect.
3. The body flip and the wheel fix are INDEPENDENT — verify them separately or a
   wrong pair can look right from one angle.

Verification needs a human at the wheel or an animated capture; a still cannot
show a spin axis. Do not claim this fixed from a screenshot.

ALSO FROM THE SAME SESSION: physics feel got a thumbs-up — "Physics FEELS kind of
FUN though!!!! I feel gravity!" Whatever the drift lane does, do not regress it.

## 6. YOU CANNOT SEE IN THE TUNNEL (interactive only) — 2026-08-13

Headless captures of the bore are lit; DRIVING through it is dark. That split is
the clue: the difference is not the tunnel, it is the frame loop.

host_tunnel.cpp:92 submits the bore's 6 real point lights EXACTLY ONCE, at boot,
before the `if (headless)` branch:

    device->setPointLights(tunnel.lights().data(), (uint32_t)tunnel.lights().size());

A headless capture renders a handful of frames with nothing else touching the
light set, so they survive and the bore is lit. The interactive loop re-submits
its OWN light list every frame (street lamps, region lights, etc.), which
overwrites those 6 on frame 2 and leaves the bore black from then on.

FIX DIRECTION: the tunnel's lights must be re-submitted every frame along with
whatever else the host is drawing, or appended to the host's per-frame list
rather than set once. Check how host_echotropolis merges streetLamps.selectLights
+ regionSet.appendNearLights into one array per frame — the same pattern applies.

Note the 20 emissive strips are GEOMETRY, not lights, so they should still glow
even with the point lights gone; if the bore is TOTALLY black, check whether the
strips are being drawn at all in the interactive path as well.

Related and worth having anyway: the car has no HEADLIGHTS. In a 450 m bore that
is the difference between a tunnel and a cave.

## 7. THE TUNNEL IS PLUGGED WITH EARTH — the real "can't see in" (2026-08-13)

Tim drove to the mouth and found it PACKED WITH GRASS: "The road... doesnt go IN",
"we need to cut out the tunnel". The bore is not dark because of lighting. It is
dark because there is a dirt ramp across the carriageway and nothing to see past.

I CHASED LIGHTING TWICE BEFORE LOOKING AT THIS, and the log had been telling me
every single run:

    18 m of road inside the shell still carries an earth ramp (the portal-sweep residual)

I read that line repeatedly and classified it as an accepted limitation, because
the module reports it honestly and its own comment calls it "the technique's
irreducible residual". It is not cosmetic. It WALLS THE TUNNEL OFF. Lesson: a
number the code volunteers about itself is not automatically benign.

WHY IT HAPPENS. A single-valued heightfield cannot step vertically, so at each
mouth the ground must sweep continuously from road level up past the crown. Where
that sweep happens, it crosses the bore.

WHAT DID NOT WORK (tried, measured, keep for the record): requiring surplus cover
(kBoreCutMargin = 14 m) before excavation stops, so the transition sits deeper
under the hill. Residual only moved 18 m -> 16 m. The ramp is not caused by WHERE
the depth profile switches; it is caused by the corridor FIELD being smoothed by
the union-of-capsules end caps over ~halfWidth. Moving the switch just moves the
gradient, it does not steepen it.

WHAT TO TRY NEXT, in order:
1. FORCE A FULL CUT for a fixed run past each portal — depth = latMax - roadY
   (the open-cutting formula) for the first ~40 m inside the shell regardless of
   cover, so the carriageway is guaranteed clear and the sweep is pushed out
   beyond the tube. Cheapest, most likely to work.
2. SHARPEN THE FIELD near the mouth: a smaller corridor halfWidth/falloff there
   makes the union's end cap tighter, so the transition happens over metres
   instead of tens of metres.
3. If neither is enough, the tube's floor is the honest fallback: emit the road
   ribbon INSIDE the shell at the tube floor and let the shell's own geometry
   exclude the terrain, rather than relying on the heightfield to get out of the
   way.

DO NOT ship a tunnel you cannot drive through and call the lane done.

### OUTCOME (2026-08-13, a7f83138) — CUT, and PROVEN by driving

Options 1+2 combined, plus a third mechanism the list above missed:

* PORTAL PLUG per mouth — a short 4-node full-cut corridor, falloff 2.5 m,
  face pushed past the main corridor's 18.8 m end-cap reach. Key measured
  find: the capsule end cap FREEZES the last node's depth for halfWidth
  (8.8 m) while the mountain keeps rising ~1.2-2 m/m, so a locally-sampled
  final depth still left a 5 m ramp at the natural slope. The final node now
  samples its depth one cap-length ahead (over-cutting a scoop just outside
  the portal), which collapses the rise into the 2.5 m falloff band.
* PORTAL HOLE (new, terrain.h) — the part NO field change can ever fix: the
  tile mesher joins road-level verts to lid verts with a continuous CURTAIN
  of triangles that has collision, however steep the field steps. The mesher
  now drops surface triangles whose centroid is in the mouth prism and whose
  lowest vertex dips into the tube envelope — render AND collision. Skirts
  get the same predicate.
* Fallback: X3_TUNNEL_PORTAL_CUT=0 restores the old field + mesh exactly.

MEASURED: field residual 16 m -> 0 m. PROVEN by `--test-tunneldrive` (11
checks): the real Jolt rig drives the real streamed-tile collision end to
end — negative control (cut disabled) stalls at the ramp at s=107, enabled
run exits past the far portal at road level (worst |dY| 1.0 m). The same
test also surveys the ditch-to-road mount step (Lane 7's skirt-wall defect,
fixed by the ~19 deg scree fillet in f33daf0e): worst step 0.14 m vs the
0.45 m chassis clearance.

Honest debts: the mouth cliff faces render with stretched terrain triangles
and a few floating shard triangles (torn hole rim / coarse LOD); the
headwall is small against the new ~40 m rock face. Cosmetic — the drive is
clean.
