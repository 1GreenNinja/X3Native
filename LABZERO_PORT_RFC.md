# LABZERO_PORT_RFC.md — Escape from Lab Zero on X3Native
**Written 2026-07-31 by the Fable chat session (claude.ai, Lab Zero project) on Tim's order:
"Write it! I can save it and have a CLI session here pick it up! Playable by 10 am."**

Save this file at the X3Native repo root. It follows the ATTENTION_FableAAA.md contract:
if you change the plan, change THIS FILE in the same commit.

> **REDIRECT (2026-07-31, later the same morning):** the render host is now the
> 3D rail — see `LABZERO_3D_ADDENDUM.md` (P-slices) before executing S0's host
> steps. The SIM SPEC (SS2.3), TEST SPINE (SS-T), BUG LEDGER (SS-B), FLASH PASS
> (SS2.5), art-direction bar, and every gate in this file remain LAW and are
> consumed by the addendum. S0's HUD-layer host survives only as a fallback if
> P0 hits an unexpected wall — decision and reasoning go in the addendum.

---

## SESSION KICKOFF (paste this into the CLI session)

> Read `LABZERO_PORT_RFC.md` at the repo root, then execute Slice S0 exactly as
> specified. Machine is the MI box: i7-7700K / 32GB / GTX 1060 3GB — Pascal, so
> the engine's RT falls back to raster automatically; that is expected and fine.
> Branch: `feature/labzero-s0`. Do not touch `engine/` in S0. Follow CLAUDE.md
> run discipline (bounded runs with timeout, never `--smoketest`, check
> `Get-Process X3Engine` before launching). Target: `--world labzero` playable
> AND `--test-labzero` green. The test suite is part of S0, not a follow-up --
> we do not like bugs. Read SS-T (test spine) and SS-B (bug ledger) before coding.
> SLOP IS BANNED: no untextured flat rectangles reach the screen -- S0 ships the
> NEON LAB procedural look (SS2.3b) and the G4 screenshot gate. X3_WORLD_RULES.md
> material laws apply in spirit: everything glows correctly or it doesn't ship.
> After S0's gates are green, continue SAME-DAY into S0.5 FLASH PASS (SS2.5):
> the flashiest, most polished version is the goal -- built ON green tests, in
> that order, because juice stacked on a bug is just a flashier bug.

---

## 0. MISSION

Port the C# WinForms game **Escape from Lab Zero** onto X3Native as a title in the
portfolio (X3, TTT 1995, Pin-Pull-Tomb, **Lab Zero**). 2D side-scrolling action
platformer: Jake & Sarah, 12-weapon arsenal, 10 enemy types + bosses, combo/style
system, 50-level 3-act campaign. Visual bar: **Fieldrunners 2** — painted, lush,
juicy. Art comes from SD FORGE on the 13700K/3090 Ti box; sim comes from the C#
codebase, which is the **behavioral spec**.

The C# game already has: fixed-timestep deterministic physics (1/60s), px/sec
units, single-source enums, a verified API surface, and a StyleSystem
(combos/ranks/announcer/hitstop). The port is mechanical *because* of that work.

**S0 is procedural, not placeholder.** No image assets yet -- but NO GRAY BOXES
EITHER. Every visible element uses boot-baked textures (gradients, radial glows,
stars) exactly the way the C# game built its look from pure GDI+ math. The
Fieldrunners-2 painted bar arrives with the S2 atlases; S0 must already read as
a deliberate neon-lab aesthetic in a screenshot -- and by end of day S0.5
(SS2.5) must FEEL polished in a motion clip. The target at every slice is the
flashiest, most polished version buildable; the correctness gates are what make
that safe to chase. Playable this morning, juicy by tonight.

---

## 1. GROUND RULES (repo protocol)

1. One branch per slice: `feature/labzero-s0`, `-s1`, etc. Push when green.
2. **Do not modify `engine/`** except in S3 (which has its own gate). S0–S2 are
   pure `app/` additions. The canon world and every existing `--world` mode must
   be untouched — Lab Zero is additive.
3. CLAUDE.md discipline applies: bounded engine runs (`timeout` + kill zombies),
   never `--smoketest`, check for a running X3Engine (the owner may be playing).
4. **Worktree hygiene is CRITICAL on the MI box** (small drive): run
   `./tools/clean_worktrees.ps1` before starting and after any sub-agent work.
5. Build note for this box (4C/8T): the engine is already split — build the
   `X3Engine` target incrementally; never delete `build/` unless forced.
6. C# source of truth: Tim's Lab Zero project directory (the 10 rebuilt files
   listed in §6 are authoritative). If the C# tree is not on this machine, S0
   does not need it — §2 embeds the complete S0 spec. Copy the C# tree into
   `staging/labzero_csharp_reference/` before starting S1.

---

## 2. SLICE S0 -- FIRST PLAYABLE (time-box: ~3h, target 10:00 AM EDT; the neon-lab
bake adds ~30-45 min over bare quads and is NOT cuttable -- slop is banned)

**Definition of done:** `X3Engine.exe --world labzero` opens a window on the
NEON LAB look (SS2.3b): parallax starfield, glowing Jake silhouette running and
jumping with coyote-time feel, three aura-ringed enemies chasing and dying in
glow bursts, streak projectiles, HUD text; `--test-labzero` green; 60 fps on the
GTX 1060; a proof shot in `shots/labzero_s0/`; clean exit. No image assets, no
audio, no engine changes -- and no untextured flat rectangles anywhere.

### 2.1 Files to create

```
app/labzero/labzero_sim.h        // constants + structs + sim class
app/labzero/labzero_sim.cpp      // sim implementation (~350 lines)
app/labzero/labzero_tests.cpp    // bool runLabZeroSimSelfTest() — SS-T; headless, no device
app/world_hosts/host_labzero.cpp // int hostLabZero(HostContext&) — loop + render + input
```

**SIM PURITY RULE (enforced by construction):** `labzero_sim.*` may include ONLY
the C++ standard library. No `IRenderDevice.h`, no GLFW, no engine headers.
The sim exposes `step(const LzInput&)` and a read-only state the host renders
from. This is what makes the whole game headless-testable and deterministic --
it mirrors the C#'s StepPhysics/render split exactly. Any PR that adds an
engine include to labzero_sim is wrong by definition.

### 2.2 Integration points (exact)

1. `app/world_hosts/host_labzero.cpp` — copy the skeleton of the SMALLEST
   existing host. **Read `app/world_hosts/host_valley.cpp` and
   `app/host_context.h` first (10 min)** — `HostContext` carries the device,
   window, and frame plumbing; follow the established pattern exactly.
2. `app/world_hosts/world_hosts.cpp` — add to the table-driven `kHostRoutes`:
   `{ "labzero", hostLabZero },`  and declare `hostLabZero` in
   `app/world_hosts/world_hosts.h` alongside the others.
3. `app/CMakeLists.txt` — add the two .cpp files to the `X3Engine` source list
   (grouped with a `# labzero — Escape from Lab Zero title (LABZERO_PORT_RFC)` comment).
4. Rendering: **HUD layer only** in S0. Pixel space via `hudSize(w,h)`, draw with
   `drawHudQuad(fc, x, y, w, h, rgba)` and `drawHudText(fc, ...)` from
   `engine/rhi/IRenderDevice.h` (quad at line ~1148, text ~1157). The 2D game
   IS a HUD in S0 — no camera, no meshes, no 3D.
5. Input: GLFW polling (`glfwGetKey`) — GLFW is already in
   `world_host_common.h`. No engine input work.

### 2.3 EMBEDDED S0 SPEC (self-contained — no C# tree needed)

All units px and px/sec. Y grows DOWNWARD (screen space). Integer positions are
fine internally as floats; render rounds.

**Fixed timestep (non-negotiable — this is the determinism contract):**
```
FIXED_TIMESTEP  = 1/60 s
MAX_STEPS       = 5          // spiral-of-death guard; drop banked time past this
accumulator += frameTime; while (acc >= STEP && n < MAX) { step(STEP); acc -= STEP; n++; }
Render every frame; sim only inside step(). Nothing gameplay-visible outside step().
```

**World:**
```
GROUND_Y     = floor(windowH * 0.85) + PLAYER_H   // the line feet stand on
PLAYER_W/H   = 63 x 88
Left bound 0, right bound windowW - PLAYER_W.
```

**Player physics (semi-implicit Euler: v += a*dt; p += v*dt):**
```
GRAVITY            = 3600  px/s^2
TERMINAL_VELOCITY  = 900   px/s
JUMP_STRENGTH      = -900  px/s
MOVE_SPEED         = 300   px/s      RUN_MULT = 1.7 (Shift)
COYOTE_STEPS       = 6               // grace after leaving ground
JUMP_BUFFER_STEPS  = 5               // early-press memory
Jump consumes on rising edge only (track prevJumpKey). Land: clamp feet to
GROUND_Y, vy=0, onGround=true. Apex = 105 px EXACT in 15 steps up (SPEC
CORRECTED BY EXECUTION, C++ suite 2026-07-31: discrete truth, not continuous).
```

**Controls (GLFW, v2 2026-07-31 PM -- Space jumps):** A/D or Left/Right move ·
SPACE or J: jump (grounded) / jetpack toggle (airborne) · F / Numpad0 / L-Alt
shoot (all additive; WinForms build swallows SC_KEYMENU so Alt+Space cannot pop
the system menu -- GLFW hosts have no such hazard, bind Alt freely) · Shift run · Up/Down ARROWS aim -- always,
including in flight · W/S fly when jetpack mode is active (fold into aim when
it is not) · Esc exits the host cleanly. TWIN-STICK LAW: flight (W/S) and aim
(arrows) are simultaneous, independent channels; any port that collapses them
onto shared keys breaks the jetpack's combat identity.

**Smooth aiming (S1 requirement; embed now so nobody invents different feel):**
```
AIM_SPEED       = 1.8 rad/s    // sweep rate while a key is held
AIM_LIMIT       = +-PI/2       // FULL +-90 deg. Straight UP and straight DOWN
                               // are required states (platform play: overhead
                               // drones, under-platform turrets). The old
                               // +-1.2 rad clamp was a design bug -- do not
                               // reintroduce it.
AIM_MAGNET_ZONE = 0.12 rad     // inside the last ~7 deg while held...
AIM_MAGNET_PULL = 0.45/step    // ...pull to EXACT vertical and land there,
                               // so straight-up is stable, never a 88-deg wobble
Ease-back       : no key held -> aimAngle *= 0.85 per step; snap 0 below 0.01 rad
Fire direction  : (cos(aim), sin(aim)) * projectileSpeed, mirrored by facing.
Muzzle          : rotates with aim around the SHOULDER (shoulder + aimDir*armLen)
                  -- straight-up shots leave above the head, never the hip.
Full deflection ~= 0.75 s with magnetism (52 raw steps, magnet closes the tail).
The ease-back is THE feel: never snap aim to zero (polish law applies to aim).
F1 interpolation note: interpolate aimAngle at render time like any position.
```

**Shooting:** cooldown 10 steps. Projectile: 8x4 px, speed 800 px/s horizontal in
facing direction, damage 10, dies off-screen. Muzzle at (facing? x+W : x, y+H/2).

**Enemies (2 types for S0, 3 spawned):**
```
Biped  40x60  HP 60  speed 120 px/s  — gravity applies, chases player X
Drone  34x34  HP 40  speed 180 px/s  — no gravity, hovers toward player X and Y
                                       + sin(t*3.2)*bob
Contact with player: player takes 10 damage, 45-step invulnerability flicker
(skip player draw when (invulnSteps/3)%2==0). Enemy killed -> +100 score,
respawn a random type at a random X after 90 steps. Player HP 2500 (yes, 2500 —
matches the C# spec; do not "fix" it).
```

### 2.3b S0 ART DIRECTION -- "NEON LAB" (procedural; SLOP IS BANNED)

No image files. At host boot, `bakeTextures()` generates small RGBA8 buffers on
the CPU and uploads each via `createTexture(rgba8, w, h, srgb=true)` (~120
lines total, pure math). Bake once, draw with `drawHudImage` + tint everywhere:

```
texGlow      128x128  radial falloff a = (1-d)^2, white -- THE workhorse:
                      tinted for auras, muzzle flashes, projectile heads,
                      death bursts, power pips. Never draw a bare quad where
                      a tinted glow belongs.
texVGrad     8x256    vertical gradient (a: 1 -> 0) -- sky, ground shading,
                      building faces
texStar      16x16    tiny 4-point star (cross falloff) -- starfield layers
texStreak    64x8     horizontal glow streak -- projectile bodies, speed lines
texVignette  256x256  corner-darkening -- one full-screen pass, instant mood
```

**Layers, back to front (all drawHudImage unless noted):**
1. Sky: full-screen texVGrad tinted deep lab-blue (0.03,0.05,0.12 -> 0.10,0.13,0.22).
2. Starfield: 3 parallax layers of texStar (60/40/20 stars, alpha .35/.6/.9),
   drifting at 0.2x/0.5x/1.0x of a slow scroll -- seeded ONCE (fixed seed, not
   per-frame) so the sky is stable and deterministic.
3. Ground: texVGrad slab from GROUND_Y down (dark steel 0.10,0.11,0.15) + a 2px
   cyan edge line (texStreak stretched, tint 0.3,0.9,1.0, alpha 0.6) -- the
   drawn line IS the physics line (B12).
4. Enemies: body quad in the C# behavior palette (Biped 0.70,0.35,0.35 / Drone
   0.35,0.67,0.82) UNDER a texGlow aura at 1.6x body size pulsing with
   sin(step*0.12) -- the C# behavior-indicator look, day one. Death: 6-8 texGlow
   bursts scaling out + fading over ~25 steps.
5. Projectiles: texStreak body + texGlow head, weapon-tinted (pistol
   1.0,0.94,0.6); 3-position fading trail (store last 3 positions).
6. Jake: NOT one rectangle -- a 5-quad silhouette (head / torso / 2 legs with a
   2-quad walk swing / arm) in Jake's palette (skin 0.88,0.67,0.52, shirt
   0.23,0.29,0.24, pants 0.24,0.24,0.27) over a faint white texGlow rim
   (alpha 0.15; 0.5 while invulnerable-flickering). Muzzle texGlow flash 4
   steps on fire. ~30 lines more than a box; completely different screenshot.
7. HUD: "HP <n>   SCORE <n>   LAB ZERO" top-left 16px via drawHudTextF
   (proportional role), HP also as a texStreak bar tinted green->red by pct.
8. texVignette full-screen, alpha 0.35. Last.
```

### 2.4 S0 gates
- **G0 tests:** `X3Engine.exe --test-labzero` exits 0 with every SS-T sub-test
  green. Register the flag in `app/test_registry.cpp`'s dispatch ladder exactly
  like the existing `--test-*` entries (include `labzero/labzero_tests.h`,
  add the `if (tf.testLabzero) return runLabZeroSimSelfTest() ? 0 : 1;` rung,
  add the flag parse alongside its peers in the TestFlags path). Headless --
  runs on any box, no GPU work.
- **G1 determinism:** SS-T/T1 in the suite, PLUS a live log of jump apex in the
  host (105 px EXACT -- measured by execution: discrete semi-implicit Euler,
  not the continuous 112.5 estimate; identical across runs and frame-rate jitter).
- **G2 perf:** 60 fps sustained on the 1060 (this scene cannot miss; if it does,
  something is wrong -- investigate, don't ship).
- **G3 hygiene:** 0 Vulkan validation errors (repo standard), clean Esc exit,
  no engine/ diffs, existing worlds unaffected (spot-boot `--world valley`).
- **G4 LOOK (the no-slop gate):** save a PNG proof shot to `shots/labzero_s0/`
  (repo screenshot capture) mid-gameplay and eyeball it against this checklist:
  zero untextured flat rectangles visible; every luminous thing sits on a
  texGlow; parallax layers visibly separate; the ground edge line matches where
  feet stand; vignette present. If the shot looks like programmer art, it is
  not done. Repo precedent: eye-verified proof shots are the standard
  (shots/r9_*, r10_*).
- Commit message: `labzero S0: first playable, neon-lab look + test spine (LABZERO_PORT_RFC)`.

---

## 2.5 SLICE S0.5 -- FLASH PASS (same day, starts ONLY after all S0 gates green)

Time-box ~2.5-3h. Branch `feature/labzero-s0.5`. Still zero image assets, still
zero `engine/` changes. This is where "correct" becomes "feels incredible."
Order matters -- items are sorted by feel-per-line; do them top to bottom.

**F1. RENDER INTERPOLATION (the single biggest polish upgrade, ~20 lines).**
Keep the previous sim state; every draw uses
`lerp(prev, curr, accumulator / FIXED_TIMESTEP)`. Motion becomes butter at any
refresh rate while the sim stays bit-deterministic (T1 unaffected -- this is
render-only). Nothing else on this list looks right without it. This is the
same render/sim split the C# architecture was built for; do not let any
interpolation value leak back into sim state.

**F2. SQUASH & STRETCH + EASING.** Jump launch: scale Jake (0.90x, 1.15y) easing
back over 8 steps; landing: (1.25x, 0.80y) easing out over 6; walk cycle gets a
2px bob. HUD numbers punch-scale 1.3->1.0 (easeOutCubic) on change. RULE OF
POLISH: nothing on screen ever snaps -- every visual value moves through an
easing curve (easeOutCubic/easeOutBack, 4-10 steps). Sim values are exempt;
this law is render-side only.

**F3. IMPACT STACK (ported early from StyleSystem, sim-side, add T9 now).**
Hitstop: 3 skipped sim steps on kill, 8 on multi-kill (render keeps running --
the accumulator makes this free). Screen shake: on kill/damage add decaying
random offset (start 6px, -1/step) to ALL layers except vignette+HUD. Kill
flash: full-screen texGlow white pop, alpha 0.25 -> 0 over 5 steps. Damage:
red vignette pulse + 2-step 1px chromatic-style double-draw of Jake (offset
red/cyan copies, alpha 0.3) -- fake chromatic aberration until S3 does it real.

**F4. REAL BLOOM PARTICLES (the engine's gift).** Route muzzle flashes, kill
bursts, and projectile spark trails through `submitParticles` ADDITIVE
(IRenderDevice ~line 966): in-scene instanced quads that FEED THE BLOOM PASS --
actual glow, not painted glow. Verify visually that in-scene particles
composite acceptably under the HUD-layer game; if pass ordering fights the 2D
look, keep the HUD texGlow versions and move this item to S3 -- note the
finding in this file either way (it IS the S3 evidence).

**F5. DEATH & LANDING CEREMONY.** Enemy death: expanding texGlow ring (quad
scaling 1->3x, alpha ->0, 20 steps) + 6 additive sparks + score popup text
drifting up 30px fading (easeOut). Landing from any fall: 4 alpha dust puffs
sliding outward. Enemy spawn: reverse-ring materialize.

**F6. LIVING BACKGROUND.** Starfield to 5 layers; add a slow full-screen
nebula tint drift (two texVGrad passes, hue-offset, 60s period); a shooting
star (texStreak, ~1/10s chance per second, seeded); ground edge line gets a
soft secondary glow line. All deterministic-seeded.

**F7. PRESENTATION FRAME.** Level intro card: "SECTOR 1 -- CRYO BAY 7" centered,
fade in/out (90 steps) on boot; HP bar gets a delayed "damage ghost" (white
chip that drains 20 steps behind the real bar -- the classic fighting-game
read); subtle idle breathing on Jake (1% scaleY sine).

### 2.5.1 S0.5 gates
- All S0 gates re-run green (T1 PROVES interpolation leaked nothing into sim).
- T9 (hitstop) added to the suite and green.
- **G5 MOTION PROOF (upgraded G4):** capture a 3-5 s gameplay GIF via
  `third_party/gif_h` to `shots/labzero_s05/` showing: a jump (squash/stretch
  visible), a kill (hitstop + shake + ring + sparks readable), parallax depth.
  Eyeball law: if the clip could be mistaken for a static engine test, it is
  not done. A screenshot cannot pass G5 -- polish is motion.
- Commit: `labzero S0.5: flash pass -- interpolation, juice stack, bloom particles`.

---

## SS-T. TEST SPINE -- `--test-labzero` (part of S0, grows every slice)

One suite function, repo pattern: `bool runLabZeroSimSelfTest()` in
`app/labzero/labzero_tests.cpp`. Pure-sim, headless, fixed seed, prints one
PASS/FAIL line per sub-test. S0 ships T1-T7; S1 extends through T14.

- **T1 DETERMINISM-HASH:** run two independent sims, identical input script
  (idle/run/jump/shoot mix), 3600 steps. FNV-1a hash every entity's position,
  velocity, HP, and RNG state each step; final hashes must be EQUAL. This is
  the master test -- it catches uninitialized fields, iteration-order bugs,
  stray frame-time dependence, and hidden randomness in one assertion.
- **T2 JUMP-APEX:** scripted jump from ground; apex 103-107 px (measured:
  105.00 EXACT -- discrete semi-implicit Euler; the continuous estimate 112.5
  was wrong, SPEC CORRECTED BY EXECUTION); airborne 28-32 steps.
- **T3 COYOTE:** walk off a ledge; press succeeds through the 5th airborne
  step, blocked from the 6th (COYOTE_STEPS=6 with decrement-BEFORE-read
  ordering yields a 5-late-step window -- measured in C++ AND C#; SPEC
  CORRECTED BY EXECUTION). Exact boundary, both sides.
- **T4 JUMP-BUFFER:** press jump N steps BEFORE landing; fires on landing for
  N<=5, ignored for N>=6. Also: holding jump across a landing must NOT re-jump
  (edge detection -- see B3).
- **T5 GROUND-CONTRACT:** after any drop, feet == GROUND_Y exactly (not +-1);
  onGround true; standing still 120 steps stays bit-identical (no jitter,
  no sinking). Guards B1/B2.
- **T6 SHOOT-COOLDOWN:** hold fire 120 steps -> exactly 12 projectiles
  (cooldown 10); each spawns at the muzzle, correct facing, dies off-screen.
- **T7 CONTACT-DAMAGE:** overlap enemy -> exactly one 10-damage hit, then 45
  invulnerable steps with zero further damage, then vulnerable again.
- *(S1 additions)* **T8** weapon-table integrity: 12 weapons, enum order ==
  key order 1-9,0, every stat > 0 where required. **T9** hitstop: request 3 ->
  exactly 3 skipped steps, counter zero after, no double-consume (B10).
  **T10** stomp-bounce chain velocities -900*min(1.6,1+0.15n) and chain reset
  on ground touch. **T11** combo decay window + hit-halving. **T12** enemy
  behavior split over 10k rolls: 40/35/25 +-3%. **T13** enemy projectiles spawn
  at ENEMY muzzle, tagged enemy, CAN hit player (B4). **T14** DamageBoost and
  LastStand multipliers actually applied to dealt damage (B5). **T17** jetpack (FLIGHT MODE):
  grounded JUMP (Space or J) ALWAYS jumps, jetpack owned or not; airborne JUMP-edge with fuel >= 15
  toggles JetpackActive (and consumes the press: no buffered double-jump may
  fire on the same edge); airborne JUMP-edge with fuel < 15 and mode OFF does NOT
  toggle (re-arm threshold). While active-airborne: W thrusts (drain 40/s,
  rise clamped 520), S dives (drain 6/s, clamped 620), neither = HOVER
  (drain 12/s, |velocityY| < 5 px/s within 20 steps, altitude drift < 2 px
  over 120 hover steps). Fuel 0 -> mode drops the SAME step. Mode persists
  across landings; grounded regen 26/s runs armed or not; armed + W performs
  a takeoff. While active-airborne, flight reads W/S ONLY and the ARROWS
  keep aiming: T15's full aim contract (sweep, +-PI/2 magnetism, ease-back)
  must pass DURING hover, and a scripted hover + arrow-sweep + fire must
  produce vertical shots while altitude holds (twin-stick proof). W/S fold
  back into aim whenever flight is not claiming them (mode off or grounded).
  Constants: THRUST 5400, DESCEND 4200 px/s^2.
  **T16** effect saturation:
  fire 1000 aberration/flash/shake/blur triggers in one step storm -> every
  amount <= its hard cap; 30 idle steps later all read 0 (decay beats any
  trigger rate). Port rule: post-FX intensity clamps at the SOURCE; clamp with
  min(), never modulo -- wrap is flicker, not safety. **T15** aim:
  hold aim-up 60 steps -> angle == -PI/2 EXACTLY (magnet must land it, not
  hover at 88 deg); same down -> +PI/2; at +-PI/2 fired projectile is
  perfectly vertical (|vx| < 1e-3) and spawns above head / below feet line
  respectively (muzzle rotation); release -> below 0.01 rad within 30 steps,
  monotonically; facing flip mirrors, never resets, the angle. Straight-down
  while standing on a PLATFORM must be legal and hit an enemy below it.

---

## SS-B. THE C# BUG LEDGER -- every bug we fixed becomes a regression test

These were real defects found and fixed during the C# rebuild. They are the
port's most likely failure modes because they live in the SPEC's subtle corners.
Each is pinned by the test noted. Porting session: read this list BEFORE
writing the corresponding code, not after a test fails.

| # | Bug (as it existed in C#) | Root cause | Pinned by |
|---|---|---|---|
| B1 | Player floated one body-height above the floor | Two meanings of GROUND_LEVEL (top-of-player vs feet); fix clamped to the line the renderer actually draws | T5 |
| B2 | Jumping died after coyote expired while standing on the floor | Ground re-check used `>` where standing-exactly-on-line needs `>=` after OnGround was cleared | T3/T5 |
| B3 | Holding jump re-fired on every landing | Missing rising-edge detection on the jump key | T4 |
| B4 | Enemy fire could never hit the player | Helper ignored its x/y/isPlayer args: every shot spawned at the player, tagged as player-owned | T13 |
| B5 | DamageBoost power-up did nothing | Multiplier existed on Player but engine never applied it to projectile damage | T14 |
| B6 | Shift/Ctrl combos ate movement keys | Modifier BITS vs key CODES confusion; required normalize (key & KeyCode). GLFW analog: poll physical keys, never require exact mods==0 | T1 (script uses Shift) |
| B7 | Whole game ran slow-motion under load | deltaTime clamped to 16 ms instead of accumulating; fixed-step accumulator + MAX_STEPS is the cure | design: SS2.3; T1 |
| B8 | Recurring CS0101 duplicate-type storms | Enums re-declared per file; single-header authority is the cure | build itself; one `labzero_types.h` |
| B9 | (Latent) catch-up frame could load several levels at once | Level-clear check inside the step loop | S1: clear check once per frame, outside step() |
| B10 | (Design risk) hitstop double-consumption | Freeze request consumed at two sites | T9 |
| B11 | Stomping gave no bounce (feature gap, felt like a bug) | Kill handler never set VelocityY | T10 |
| B12 | Ground drawn at a different line than physics stood on | Renderer used GROUND_LEVEL + PLAYER_H; physics must match the drawn line | T5 + eyeball in host |
| B13 | (Aug-Oct 2025) Past level 7, entering the LAB, the screen became an unplayable smear of aberration and garbage | Compound: unclamped level-scaled Color.FromArgb crossing 255 at L8 (mid-Paint failure -> garbage frames; one site even used % 256, WRAPPING bright colors to near-black flicker); 19-enemy kill density re-triggered the 5-effect stack faster than decay under the capped-deltaTime loop -> permanent aberration. Cure: clamp every color param at the source (Math.Min, NEVER modulo); effect triggers ASSIGN with hard caps, never accumulate; decay decoupled from load | T16 |

**Defensive standards (all slices):** sim state fully initialized at
construction (no "set later" fields -- T1 catches these); `std::clamp` at every
boundary; fixed pools / `std::vector` reuse, zero per-step heap allocation in
steady state (matches the engine's allocationCount=0 bar); one seeded
`std::mt19937`, NEVER `rand()` or a second generator; asserts on enum ranges at
switch defaults; every timer is an integer step count.

**Scope discipline:** 10:00 is the target, correctness is the requirement.
If the clock presses, the order of what ships NEVER changes: sim + tests first,
then rendering, then enemies. A smaller S0 that passes every test beats a fuller
one that "mostly works" -- a bug shipped at 10 costs the whole afternoon.
Nothing merges with a red test or a skipped gate. We do not like bugs.

---

## 3. SLICE S1 — FULL SIM PORT (the real game)

Copy the C# tree to `staging/labzero_csharp_reference/` first. Then port
file-by-file into `app/labzero/`. The C# is the spec — behavior parity, not
line-by-line beauty.

**COORDINATE FIRST -- EFLZ content already exists in this repo.** The fleet has
been building Lab Zero universe content natively: `app/canon_aliens.*` ("EFLZ
canon-alien roster: Mantis/Grey/Reptilian/Nordic", `--test-canonaliens`) and
`app/clone_boss.*` ("THE CLONE -- Act-1 finale 3-phase boss", `--test-clone`).
Before porting Enemy.cs bosses/rosters, read both, check the task board, and
decide the mapping (2D archetypes vs 3D roster) in an ATTENTION-doc update.
Building the same roster twice is the exact failure mode ATTENTION_FableAAA.md
documents. Do not repeat it in this project.

### 3.1 File map (C# -> C++)
| C# (authoritative) | C++ target | Notes |
|---|---|---|
| GameTypes.cs | labzero_types.h | `enum class` per enum; KEEP member order (weapon keys 1-9,0 depend on it) |
| GameConstants.cs | labzero_constants.h | constexpr; px/sec values are already correct |
| WeaponSystem.cs | labzero_weapons.h/.cpp | stat table for all 12 weapons verbatim |
| Player.cs | labzero_player.h/.cpp | physics, power-ups, coyote/buffer, aim; rendering stubbed to quads until S2 |
| Enemy.cs | labzero_enemy.h/.cpp | AI v3.0 (2026-07-31 PM): v2 behavior split PLUS awareness/alert '!' telegraphs, per-species signatures (drone dive-bombs, swarm boids, assassin cloak cycle, titan stomp ceremony, healer tether, biped platform-jumps), predictive turret aim + aimed engine fire (360 px/s lead), enrage, flinch squash, 10 distinct renderers |
| Projectile.cs / Particle.cs | labzero_projectile.*, labzero_fx.* | particles route to `submitParticles` (ADDITIVE for sparks/plasma — free bloom feed) |
| Level.cs | labzero_level.h/.cpp | 3-act 50-level tables + weighted enemy pools |
| StyleSystem.cs | labzero_style.h/.cpp | combos/ranks/announcer/hitstop/LastStand; hitstop = skip N sim steps while rendering |
| InputManager.cs | folded into host | GLFW mapping; keep J-jump / Space-shoot / arrow-aim contract |
| GameEngine.cs (2652 lines) | labzero_game.h/.cpp + host | orchestration only; engine-owned concerns (window/timing) drop out |

### 3.2 Translation rules
- C# properties -> public fields or trivial inline accessors. No GC: entities in
  `std::vector`, fixed pools for projectiles/particles (the engine's own
  particle ring is the model).
- `Random` -> one `std::mt19937` seeded per level (determinism!); `Math.Clamp`
  patterns -> `std::clamp`; `Rectangle` -> tiny POD `LzRect{x,y,w,h}` +
  `intersects()`.
- Frame counters stay integer STEP counters (they are exact time at 60 Hz).
- Rendering calls in C# classes become draw-list entries the host submits;
  sim never touches the device directly (mirrors the C# sim/render split).

### 3.3 S1 gates
Feature-parity checklist vs C# (12 weapons switchable, all 10 enemy types +
4 bosses, stomp-bounce chains, LastStand, sector-clear slow-mo, PERFECT CLEAR
bonus), G1 determinism re-run, 60 fps on the 1060 with 20 enemies + heavy fire.

---

## 4. SLICE S2 — ART PIPELINE (Fieldrunners bar begins)

- FORGE runs on the 13700K/3090 Ti (24GB: SDXL/SD3.5 native, no CPU offload —
  the old `device_map="balanced"` configs are obsolete). Expose the FORGE API
  over Tailscale; MI box requests PNGs remotely, never renders SD locally.
- Atlases: per-act parallax backgrounds (Act palettes from Level.cs: lab blue /
  alien amber / war crimson), Jake & Sarah pose sheets, enemy sheets, weapon
  pickups. PNG -> `stb` load -> `createTexture(rgba8, w, h, srgb=true)` ->
  `drawHudImage(fc, tex, x,y,w,h, rgba, u0,v0,u1,v1)` with sub-rect UVs.
  (.x3pak/KTX2 packaging is S2.5, after the look is proven.)
- Parallax = N image layers drawn back-to-front with camera-scaled offsets.

## 5. SLICE S3 — BLOOM ON SPRITES (the one engine decision)

HUD likely composites AFTER tonemap/bloom, so `drawHudImage` sprites may not
glow. **Verify pass order in `engine/rhi/vk/vk_passes.cpp` first.** Then pick:
(a) route glow-needing sprites through `submitParticles` ADDITIVE (in-scene,
feeds bloom, zero engine changes), or (b) add a world-space sprite pass before
tonemap (small engine PR, own gate, coordinate via ATTENTION doc). Prefer (a)
until it visibly limits the art.

## 6. SOURCE-OF-TRUTH MANIFEST (C# authoritative set)

Rebuilt + verified this month, single-convention (fixed step, px/sec, enum
authority): `GameEngine.cs (2652)`, `GameConstants.cs`, `GameTypes.cs`,
`WeaponSystem.cs`, `Player.cs (894)`, `Enemy.cs`, `Projectile.cs`,
`Particle.cs`, `InputManager.cs`, `Level.cs`, `StyleSystem.cs (430)`.
Supporting behavior worth mining later: DialogueSystem, Building/Platform/
PowerUp/WeaponPickup, EnemyDeathEffects, UpgradeSystem, CheckpointData.

## 7. HARDWARE / VALIDATION MATRIX

| Box | Role | Gate |
|---|---|---|
| 7700K / 1060 3GB (MI, dev) | port dev + **min-spec gate** | 60 fps, raster fallback path |
| 4790K / 1080 Ti (AZ) | verified-GPU tier | boots, parity |
| 14900K / 5070 Ti | mid-tier + SD overflow | 60 fps @1440p |
| 13700K / 3090 Ti | **art factory** (FORGE) + heavy dev | — |
| 14900K / 5090 / 128GB | fleet flagship, RT/DDGI paths, integration | full-feature |

## 8. STANDING GATES (every slice)
0 VUID · determinism G1 · 60 fps on min-spec · no regression of existing worlds
· **G4 proof shot per slice -- NO SLOP: no untextured flat rectangles, no
default-look placeholder art, every glow on a real falloff** (X3_WORLD_RULES.md
material-law culture applies to Lab Zero from S0 onward)
· **G5 motion clip (gif_h) per slice from S0.5 onward -- THE POLISH LAW: nothing
snaps, every visual value eases, sim stays interpolated at render time; flash
is built on green tests, never instead of them**
· **never reduce Lab Zero features vs the C# spec -- feature regression is the
cardinal sin** (it is written in the project's memory for a reason).
