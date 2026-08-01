# LABZERO_3D_ADDENDUM.md — The 3D Side-Scroller Redirect
**Written 2026-07-31 by the Fable chat session, on Tim's order: "Can we make a
beautiful 3D side scroller? Where we can go into 3D in mountain caves?" — "Write it!"**

Save at repo root NEXT TO `LABZERO_PORT_RFC.md`. This addendum REDIRECTS the
render host from HUD-layer 2D to a 3D rail on the real world. Everything else
in the RFC remains LAW: the test spine (SS-T), the bug ledger (SS-B), the
no-slop gate (G4), the motion-clip gate (G5), the polish law (SS2.5 F1–F7),
ground rules, and the feature-regression prohibition. The pure headless sim
survives as `labzero_feel.h` — see P1. If you change the plan, change THIS
file in the same commit.

---

## SESSION KICKOFF (paste into the CLI session)

> Read `LABZERO_PORT_RFC.md` then `LABZERO_3D_ADDENDUM.md` at repo root.
> Execute slice P0. Machine: MI box (7700K / GTX 1060 3GB — Pascal: RT falls
> back to raster; expected). Branch `feature/labzero-p0`. BEFORE coding, read:
> `app/world_hosts/host_valley.cpp` (your skeleton), `app/host_context.h`,
> `app/player.h`, `engine/physics/IPhysicsWorld.h` (character API, ~L77),
> `engine/rhi/IRenderDevice.h` setCamera/setCameraBasis/FogParams. Do NOT
> touch `engine/`. CLAUDE.md run discipline applies. No slop; G5 gif required.

---

## A0. THE ARCHITECTURE — one world, one controller, one seam

A 3D side-scroller with cave transitions is NOT two games. It is:

1. **One world** — the streamed terrain + sky the valley host already builds.
2. **One character** — a Jolt character (`createCharacter/moveCharacter/
   characterGrounded`), driven every fixed step by our feel layer.
3. **One constraint** — a gameplay SPLINE. In side-scroll mode, input maps to
   the spline tangent and the character is steered back to the spline laterally.
   Weight `w = 1` means fully railed; `w = 0` means free 3D.
4. **One camera rig** — rail mode: offset ~14 m perpendicular to the spline,
   FOV 28 (compressed, painterly); free mode: over-shoulder follow, FOV 55.
5. **THE SEAM** — a cave-mouth trigger blends `w 1 -> 0`, the camera rig
   cross-blends, fog ramps, input remaps. Design precedent: Nier: Automata's
   camera shifts; Inside/Little Nightmares for the rail lens; Klonoa for a
   curved rail through a 3D world. The player just walks into the mountain.

Determinism note (honest): Jolt is deterministic for same binary + same input
sequence. T1 becomes **T1-J**: dual-run, same process, scripted input, hash
character pos/vel per step — must match exactly. Cross-BUILD determinism is
not guaranteed (unlike the pure C# sim); documented, accepted.

---

## A1. UNITS — px -> metres (repo law: metres, no cm)

Scale anchor: Jake 88 px tall = **1.85 m** ⇒ `1 m = 47.57 px`,
`PX2M = 0.021023`. Convert ANY C# spec number by multiplying px by PX2M.
Feel is preserved because the RATIOS are preserved.

| Constant | C# (px, px/s) | 3D (m, m/s) |
|---|---|---|
| Player height / radius | 88 / — | **1.85 / 0.35** (capsule) |
| MOVE_SPEED | 300 | **6.31** (run ×1.7 = 10.72) |
| JUMP_STRENGTH | −900 | **−18.92** |
| GRAVITY | 3600 | **75.7** (≈7.7 g — this is WHY it feels snappy; do not "fix" to 9.81) |
| TERMINAL_VELOCITY | 900 | **18.92** |
| Jump apex / air time | 105 px EXACT (measured) / 29 steps | **2.207 m** (T2m gate: 2.16-2.25 m; SPEC CORRECTED BY EXECUTION) |
| Pistol projectile | 800 | 16.82 |
| Enemy speed stat | stat × 60 px/s | stat × **1.261 m/s** |
| Jetpack thrust / dive / rise / dive caps | 5400 / 4200 px/s^2, 520 / 620 px/s | **113.5 / 88.3 m/s^2, 10.93 / 13.03 m/s** (J toggles mode; W/S fly; ARROWS aim simultaneously -- twin-stick; fuel rates unchanged: time) |
| Coyote / buffer / cooldowns | steps | **unchanged** (they are time, not distance) |
| FIXED_TIMESTEP / MAX_STEPS | 1/60 / 5 | unchanged |

---

## P0 — RAIL CAMERA PROTOTYPE (~3–4 h) `feature/labzero-p0`

**DoD:** `X3Engine.exe --world labzero3d` opens on the streamed mountain
terrain with sky; a Jolt-capsule character runs left/right along a gameplay
spline and jumps with placeholder gravity; side camera tracks on the rail at
FOV 28 with smoothing; 60 fps on the 1060; G4 shot + G5 gif in
`shots/labzero_p0/`; `--test-labzero` still green; zero `engine/` diffs.

1. **Host:** `app/world_hosts/host_labzero3d.cpp`, route `"labzero3d"` in
   `kHostRoutes`, declare in `world_hosts.h`, add to `app/CMakeLists.txt`.
   Copy `hostValley`'s setup VERBATIM (physics init, job system,
   `worldTerrainConfig()` streamed terrain, analytic `SkyParams`) — that ~60
   lines buys the entire world. Strip valley-specific ecology if heavy.
2. **Character:** `createCharacter(0.35f, 1.85f, spawnPos)`. Read
   `app/player.h` FIRST — if the existing player character (model + animation)
   can be hosted standalone, USE IT (that is the no-slop answer); if it is
   entangled with canon-world state, ship P0 with a capsule + emissive rim
   material + blob shadow and file the model hookup as P1 line-item ZERO.
   Note the finding in this file.
3. **Spline:** `labzero_rail.h` — array of nodes, Catmull-Rom position +
   tangent lookup by arc-length param `s`. P0 ships TWO nodes (a straight
   400 m run along terrain that visibly skirts mountains); curving comes free
   later by adding nodes. Character control each fixed step: input −1/0/+1 →
   `desiredVel = tangent(s) * speed + lateralCorrection` where
   `lateralCorrection = (railPoint(s) − charPos)⊥ * k` (k ≈ 4/s) keeps the
   capsule glued to the rail without a hard constraint (Jolt stays happy).
4. **Camera:** `railCam = railPoint(s) + side * 14 m + up * 1.6 m`, look-at
   `charPos + up * 0.8 + facing * 2.5 m lookahead (eased)`; FOV 28 via
   `setCamera`/`setCameraBasis`. Critically-damped smoothing (halflife 0.12 s
   horizontal, 0.25 s vertical) + a 1.2×0.8 m deadzone box. NEVER hard-snap
   (polish law).
5. **Feel placeholder:** gravity/jump with A1 constants applied naively (full
   feel layer is P1); jump on edge-press only (B3 lives here too).
6. **1060 3 GB budget (dev box!):** streamed terrain + bindless pools may
   press 3 GB. If VRAM-limited: reduce view distance / texture budget via the
   existing r_ cvars rather than code changes; DOCUMENT the settings used in
   this file. This box is the min-spec gate — make it work here first.

---

## P1 — FEEL PORT IN METRES (~half day) `feature/labzero-p1`

The RFC's sim becomes **`app/labzero/labzero_feel.h`** — a PURE state machine
(std-lib only; sim-purity rule holds): coyote counter, jump buffer, edge
detection, run/crouch modifiers, shoot cooldown, hitstop counter. Consumed by
BOTH the headless test suite and the 3D host. One implementation, two clients
— the tests test THE code the game runs, not a copy.

- Host wiring per fixed step: `feel.step(input, grounded=characterGrounded(id))
  -> desiredVelocity` → `moveCharacter(id, v, dt)`.
- **Tests re-hosted in metres:** T2m apex 2.30–2.43 m & air 28–32 steps; T3
  coyote ≤6/≥7 boundary; T4 buffer ≤5/≥6 + held-key no-refire; T5m grounded
  contract (no jitter/sink over 120 idle steps); T6 cooldown count; T9
  hitstop. Plus **T1-J** dual-run hash (A0 note).
- **Aiming on the rail:** the RFC's smooth-aim spec (AIM_SPEED 1.8 rad/s,
  clamp +-PI/2 FULL VERTICAL with cardinal magnetism, 0.85/step ease-back —
  see RFC SS2.3) ports as PITCH IN THE RAIL PLANE — straight up and straight
  down included (platforms, overhead drones, shafts below): fire direction = tangent(s) rotated by aimAngle about the rail's
  side axis. Upper-body/arm follows aim if the character rig allows (read the
  model's node list); otherwise a small glow-dot reticle 3 m along the aim ray
  (texGlow — no-slop compliant, never a bare line). T15 runs against
  labzero_feel.h unchanged — the state machine is dimension-independent.
- RFC SS2.5 carries over NOW: F1 render interpolation of character/camera
  between fixed steps (interpolate aimAngle too); F2 easing law + squash/stretch (scale the MODEL
  transform, never the physics capsule); F3 impact stack (hitstop skips sim
  steps; camera shake shakes the RIG, not the character).
- Bug-ledger vigilance: B1/B2/B12 reappear in 3D as "capsule floats above
  terrain" / grounded-flag edge cases — T5m pins them.

---

## P2 — THE SEAM: INTO THE MOUNTAIN (~1 day) `feature/labzero-p2`

**COORDINATE FIRST.** The repo already contains the destination: `act2_caves`
defines `L11toL12Portal` ("entered the L12 cave portal") and L12 Advanced
Cave System; `cave_atmosphere` already owns a blend driver
(`State{inCave, depth01, blend}`, `configure(...)`, fog/audio hooks). The
seam must land ON these, not beside them. Read `act2_caves.h/.cpp`,
`cave_atmosphere.h/.cpp`, `strata.h` first; update the ATTENTION doc with the
chosen placement BEFORE coding. Building a second cave system is the exact
failure ATTENTION_FableAAA.md exists to prevent.

**Mechanics of the transition (all over `T = 1.0 s`, easeInOutCubic):**
1. Trigger volume at the cave mouth sets target `w: 1 -> 0` (exit sets 0 -> 1).
2. **Constraint:** lateralCorrection gain scales by `w`; at `w = 0` the rail
   releases entirely. Rail param `s` keeps tracking nearest-point so
   re-attachment on exit is seamless.
3. **Camera:** blend rail rig → follow rig (pos lerp + orientation slerp via
   `setCameraBasis`), FOV 28 → 55 on the same curve.
4. **Input:** crossfade mapping — L/R-on-tangent → camera-relative WASD,
   blended by `w` so mid-transition input never dead-zones or flips. Aim
   crossfades on the same weight: rail-plane pitch (arrow keys) → camera-look
   aiming in free mode; aimAngle eases toward the camera pitch as `w -> 0`
   so the reticle never jumps mid-seam.
5. **Atmosphere:** drive `cave_atmosphere` state (or mirror its pattern):
   `FogParams` ramp density 0 → ~0.003 (respect the "no milky wash" cap),
   color to the crystal look ("near-black, a whisper of blue"); sky influence
   down; point lights / (on capable GPUs) DDGI up. Audio hooks if
   `bindAudio` fits.
6. **Combat:** enemy plane-clamp releases with `w`; in-cave spawns use full
   3D pathing (existing monster systems).

**Seam tests (added to `--test-labzero`, headless where possible):**
- **TC1 CONSTRAINT INVARIANT:** with `w == 1`, distance(char, rail) < 0.05 m
  over a 3600-step scripted run including jumps.
- **TC2 CAMERA CONTINUITY:** through a scripted transition, per-frame camera
  position delta < 0.5 m and angular delta < 6° — no cuts, no pops.
- **TC3 ROUND-TRIP:** enter + exit; character re-attaches to rail within
  0.1 m, speed never exceeds 1.5× pre-seam speed (no velocity spike), `w`
  returns to exactly 1.
- **G5 clip:** the gif for this slice IS the transition — walk in, world
  opens, fog swallows the sky. If the seam is visible as a "mode switch,"
  it is not done.

---

## P3 — BEAUTY PASS (~1–2 days) `feature/labzero-p3`

- Per-act grade: `setFilmic` / LUT per act (lab blue / alien amber / war
  crimson — Level.cs palette becomes literal color grading).
- Caves: crystal look integration, `cave_river` water if placement fits,
  point-light showcases; DDGI on 5070 Ti/3090 Ti/5090, VERIFY the SSAO/CSM
  fallback looks intentional on the 1060 (min-spec gate includes LOOK).
- Mountains: `tod.h` time-of-day for long-shadow golden light on the rail
  sections; fog depth-cueing the ridgelines (parallax is free — it's real).
- RFC F4 resolves itself here: particles are in-scene natively in 3D — muzzle
  flashes, kill bursts, cave dust motes via `submitParticles` ADDITIVE feed
  bloom with zero ordering questions. Spend them generously in caves.
- Optional: Sarah companion hook (`sarah.h` exists — coordinate, don't fork).
- **Jetpack in the caves:** the seam's best payoff — vertical shafts in L12
  become jetpack terrain. In 3D the flame plumes route through
  `submitParticles` ADDITIVE (in-scene, feeds bloom: real glowing exhaust
  lighting cave walls), shock diamonds and all. Fuel economy identical (T17).

## P4 — ROADMAP (not this week)
Map the 2D campaign's 3-act / 50-level structure onto the 3D biome route
(L11 camp → L12 caves → L13 swamp → L14 station → L15 valley already ladder
up); the strata descent as a vertical set-piece act transition; boss ports
coordinated with `clone_boss` / `canon_aliens`.

## GATES (every P-slice)
`--test-labzero` green (incl. TC-series from P2) · 0 VUID · 60 fps on the
1060 with documented cvar budget · G4 shot + G5 gif per slice · no `engine/`
diffs (P-series is app-layer only) · existing worlds unaffected · never
reduce features vs the C# spec · no slop, nothing snaps, everything eases.
