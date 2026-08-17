# Lane briefs — ready for pickup (any model, any terminal)
*Written 2026-08-17 14:00 after the box rebooted. Every lane below has its work
COMMITTED in its own worktree; nothing is lost. Hand one of these to a fresh
agent (DeepSeek tab, another Claude session, whatever) and it can continue.*

## HOW TO PICK ONE UP
1. `cd <worktree>` (paths below) — the branch is already checked out there.
2. `git log --oneline -3` and `git show --stat HEAD` — the last commit is a
   session-lead checkpoint of interrupted work: **UNVERIFIED**, review it.
3. `git fetch origin && git merge origin/integration/complete` — mainline moved
   (tip **8b7f757c**: map v3, interchange, cutaway, factory, town, audio, mute).
4. Read `CLAUDE.md`, `docs/NO_SLOP.md`, `docs/ENGINE_GOTCHAS.md` before coding.
5. **COMMIT AT EVERY GREEN STEP** — this machine has now lost lanes to a usage
   cap twice and a reboot once. Uncommitted work is the only thing ever at risk.
6. **NEVER push.** The session lead merges to `integration/complete`.

## STANDING LAW (all lanes)
- Never `--smoketest`. Check `tasklist //FI "IMAGENAME eq X3Engine.exe"` before
  EVERY engine launch — the owner plays on this box and sibling lanes capture;
  retry later rather than aborting the lane.
- Eyes-on FULL-RES captures, read by the agent itself, before claiming done
  (NO_SLOP rule 2). Interactive-only HUD needs the `X3_SHOT_PUMP` staging
  pattern — never fake a still.
- No GLB/asset bytes committed (ENGINE_GOTCHAS gotcha 2.5); publish to the
  store and commit only `assets/manifest.json`.
- Armory/pack GLBs are frequently **draco** (the loader silently drops the
  geometry — the model renders INVISIBLE while logging as loaded; decode with
  `npx @gltf-transform/cli copy`) and sometimes **WebP** (the engine has NO
  WebP decoder — transcode; see `tools/town_assets.py`).
- Search the whole 914-package asset library with
  `python tools/unitypackage_index.py --search <terms>`.

---

## LANE A — WINGS / OVERDRIVE FLIGHT  (worktree `agent-af1da97b0cf6e9a0c`)
`D:\GameDev\X3Native\.claude\worktrees\agent-af1da97b0cf6e9a0c`

**The story:** the owner found a bug and fell in love with it. In the NOS block
`nosActive = wantNos && nosTank > 0.02f` has no hysteresis and the tank
recharges even while SHIFT is held, so at empty it oscillates across the
threshold and re-fires the 2600 N·s activation kick every ~3rd frame — raw
impulse bypassing the tires. The car reaches ~379 mph. He wants it CANONIZED,
not fixed:
1. **"NITROUS DEPLETED"** HUD flash + sound the instant the tank empties
   (`assets/audio/vehicles/engine_bank/bov_psssht.wav` is thematically perfect).
   This warning is the secret's camouflage — normal players release SHIFT.
2. **OVERDRIVE** (0–5 s held past empty): replace the accidental oscillation
   with an explicit state applying equivalent thrust (~40 m/s²), escalating FOV
   punch and audio. The 379-mph FEEL must survive — A/B it and say honestly if
   it differs.
3. **WINGS at 5 s**: wings deploy (catalog search `wing aircraft spoiler jet`;
   procedural acceptable if textured), and the car becomes a flying beast.
   - **ATMOSPHERIC flight, not space 6DOF**: bank to turn, pitch to climb,
     mild rudder. Full aerobatics — rolls, loops, inverted must all work.
   - **700 mph full thrust, 277 mph hands-off coast** (owner's numbers are
     spec — make them fall out of thrust/drag, don't clamp).
   - The **6DOF camera** an earlier session built for the space host is for the
     CAMERA only (free-look decoupled from flight) — find and reuse it.
   - **Crashing hurts**: violent tumble, damage state, overdrive lockout.
   - **P = PARACHUTE** (owner was explicit: not E, not SPACE). Jake ejects via
     the shared AnimatedCharacter module, canopy, steerable drift, CONTACT LAW
     landing; the car crashes on without him.
4. **THE WINGS MUST GLEAM.** Owner: "We want the wings to Gleam in the sun...
   like" + `docs/design/GLEAM_REFERENCE.png` — a low sun throwing a sharp
   specular glitter-track across a surface (that shot is terrain reading like
   sunlit water). So the wings want a low-roughness, slightly anisotropic
   metallic finish that catches a moving highlight as the car banks: the
   gleam should SWEEP along the wing as its angle to the sun changes, which
   means a real specular response, not an emissive cheat. Rule 5 applies —
   a full-metal material with no mrTex renders BLACK; give it a proper
   metallic-roughness map. Bloom already exists in the chain; a tight
   highlight will bloom on its own. Prove it with a capture at a low sun
   angle mid-bank, and a second at a different bank angle showing the
   highlight has MOVED.
5. **ALL OF THIS LIVES IN THE SHARED VEHICLE LAYER** (`app/vehicle.*`), not
   host_tunnel — the owner was explicit ("That's NOT going to be in host_tunnel
   lol"). Part of the job is MOVING the existing host-local NOS block down.
6. Gate the wheel-lift CONTACT LAW in `DriveDemo::postStep` on wings-retracted
   (rule 4 comment pairing), and measure terrain streaming at 313 m/s.

**Gates:** vehicle 37+, roadnetwork, terraincorridor, tunnelmouth, riverbridge,
traffic; normal NOS behaviour unchanged (15 s bottle / 20 s recharge / one
kick) and regression-gated; captures of the DEPLETED flash, wings mid-deploy,
flight, and a landing.

---

## LANE B — UNDERGROUND RIVER + BOUNDED WATER  (worktree `agent-ae504b2275ceb226d`)
`D:\GameDev\X3Native\.claude\worktrees\agent-ae504b2275ceb226d`

**JOB 1 FIRST — water only in bodies of water.** Owner, flying noclip: "we do
indeed have water underground.. which I do not like. We just want water IN
bodies of water and rivers. like real life."
- Diagnosis is DONE, don't re-derive: the water MODEL is correct
  (`worldWaterLevelAt` returns dry off-river/off-ocean, which is why swimming
  behaves). The DRAWN MESH is the bug — `host_tunnel.cpp:~1142` draws ONE FLAT
  UNBOUNDED Gerstner plane at a single waterY (its own comment admits it), and
  the patch is a finite 480 m camera-centred square that fades to raw analytic
  sky at its rim because `applyRiverWater` never sets
  `WaterParams::horizonColor`.
- Fix: BOUNDED drawn water, only where the model says wet, following the
  descending channel (drawn-vs-query already agrees to 0.0000 m — keep gate
  RB10 green). Two bounded water meshes already exist: `app/world_regions.cpp`
  and `app/cave_river.cpp`. Reuse one; never write a third.
- Gate: a selftest sampling points above/below terrain map-wide asserting no
  drawn water where the model says dry, plus an under-terrain capture.

**JOB 2 — the cavern.** Owner: "we want an underground river... with rock
beaches... movie grade.. rushing water" running from the NW lake through the
city per `docs/design/ROAD_NETWORK_SKETCH_V2.png`.
- Mechanism is settled (`terrain.h`): corridor depressions are "the mechanism
  that makes freeway tunnels possible WITHOUT CSG, voxels or holes in the
  heightfield" — cut-and-cover. **Do not attempt CSG/voxels/heightfield holes.**
- `app/cave_river.cpp` already builds an underground water ribbon with bank
  lights for the club — point that machinery at the open world.
- Rushing water with whitewater at drops, WALKABLE rock beaches (CONTACT LAW),
  rock shell/ceiling, real rock textures (`terrain_rock`, `cv_rock_flume`,
  `cv_rock_wet`, `fw_rock_cliff` — check what's published), depth + mist.

---

## LANE C — THE PHOENIX MEGA STACK  (fresh start; old worktree had nothing)
Base off `origin/integration/complete`. Read `app/interchange.{h,cpp}` top to
bottom first — the diamond interchange lane built the foundation and its report
explicitly teed this up.

Owner: "i-17 - i-10 phoenix Mega Stack as reference... High concrete barriers
swooping curving ramps... high speed arcs." He drives the real thing.
- **Levels:** L1 one freeway at grade; L2 the other on a long deck over it;
  L3/L4 four DIRECTIONAL flyover ramps over both, on TALL piers.
- **The one new mechanism:** ramp-over-ramp gap authoring — a ramp's spec
  declaring `Gap`s against multiple underlying routes. Everything else exists.
- **High-speed arcs:** 150–300 m radius sweeps (55–65 mph), NOT the diamond's
  48 m loops. Drive-height captures must read as one continuous arc.
- **High solid parapets** (~1.1–1.4 m) on every elevated edge, continuous,
  collision on — you must be able to lean on the wall at 60 mph.
- Clearances measured at EVERY crossing with the diamond's interpolating
  sampler (it caught a 4.3 m quantisation error); 16.5 ft minimum.
- `X3_SPAWN=stack`. Gates: a `--test-stack` suite; interchange 10/10 must
  survive; fps A/B (the diamond cost nothing: 768 vs 760).

---

## LANE D — SHADOW FADE UNDER MOTION  (fresh start; diagnosis lane)
Owner: "the fading shadow of the car and Jake when moving" — shadows wash out
while moving, recover when still. **Measure before touching anything** (rule 9).

Suspects in order:
1. **TAA accumulating the shadowed lit result.** This engine already fought the
   object-ghosting war and won it with per-object motion vectors (`r_velocity`).
   The GROUND under a moving caster cannot have a motion vector — the ground
   doesn't move — so its history says "lit" and the blend washes the shadow
   out. Fixes: keep the shadow term out of history, tighter neighbourhood clamp
   (YCoCg AABB), or a responsive-history flag where the shadow delta is large.
2. **CSM cascade update cadence** — a fast car outruns a reduced-rate cascade.
3. **Soft-shadow temporal filtering** with its own history lacking caster
   motion rejection.
4. **SSAO/contact darkening** mistaken for "the shadow" — verify WHICH term
   fades by toggling passes.

Method: reproduce with capture PAIRS (stationary vs at speed, same framing —
`--shot-drive` staging exists), MEASURE the shadow luminance delta, bisect with
cvars, and **report the convicted pass before fixing**. Fix only that pass; do
not retune lighting globally. Add a LIGHTING NOTES section for anything else
wrong (bias, peter-panning). fps within 2%.

---

## ALSO PENDING (session lead's queue, not agent briefs)
- **Merge `lane4/town-asset-swap`** (branch in the X3Native repo, tip
  `11f7fa84`; its worktree was lost to the reboot but the branch is intact):
  real houses replacing the HouseForge ruins, six civilians replacing the
  clawed mutant + SWAT operator, plus four bonus fixes. fps honest amber.
- Backlog: #42 dealership (build on inspx garage turntable), #43 tree impostor
  pop, #39 traffic LOD, #40 Jake sprint blend, #41 buried river crew, #38 the
  Biomechanical Mutant, #36 extract the ~700 unextracted packs.
