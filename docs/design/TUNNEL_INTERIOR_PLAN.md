# Plan — the tunnel INTERIOR program (walkways, doors, rooms, dead LEDs, screens)

Status: DRAFT, pre-execution. Conditions written BEFORE any code, the way
`CITY_BORES_PLAN.md` did it — that discipline caught its three portal defects
as named conditions instead of capture-and-squint iterations.

Companion to `TUNNEL_NEXT.md` §3 (the raw request) and `CITY_BORES_PLAN.md`
(the four city bores this program must eventually scale to).

## The program (Tim, 2026-08-13, verbatim intent)

"sidewalks, access points, doors with keypads and rooms and stairs and
underground complex access, lighting — some LED, some burned out, some video
screens like CP2077".

## Verified constraints (measured or read from code, not assumed)

1. **There is exactly 1.0 m of floor per side for a walkway.** The road ribbon
   is ±`kTcRoadHalfWidth` (6.0 m); the tube wall springs at ±`kTcTubeHalfWidth`
   (7.0 m). A raised maintenance walkway therefore fits ONLY as a 6.0→7.0 m
   band — real bore walkways are 0.8–1.2 m, so this is authentic, but there is
   zero slack: any kerb/railing thickness comes out of the 1.0 m.
2. **Wall height before the arch is 3.6 m** (`kTcTubeWallH`); the crown is
   7.6 m (`kTcTubeCrownH`). Doors (≈2.1 m) and screens fit on the vertical
   wall band without touching the arch.
3. **The shell is 0.9 m thick** (`kTcShellThick`). A door niche recessed into
   the wall may go at most ~0.55 m deep before it breaches the outer skin.
   Anything deeper (rooms, stairs) hangs OFF the niche *behind* the wall —
   and that volume is FREE: the terrain is only a surface; below/inside the
   hill there is nothing to excavate, no corridor to register, no portal hole
   to cut. Rooms need no terrain interaction at all.
4. **Light budget.** The engine has 64 forward point lights. Each bore already
   spends `kTcMaxBoreLights` = 6 REAL lights; the 26–30 strips are emissive
   geometry (free). Four city bores = 24 lights before a single interior is
   lit; `CITY_BORES_PLAN.md` B1 caps all-tunnel spend at 32. Interiors are
   therefore EMISSIVE-FIRST: the interior program for a bore must fit inside
   the SAME 6 lights that bore already spends, by re-aiming, not adding.
5. **Mechanisms that already exist and MUST be reused, not reinvented:**
   * `KeypadEntry` state machine + `buildKeypad()` realistic keypad geometry
     (`app/keypad.h`, `--test-keypad` KP1-KP6). It already ships the three
     status colours — Locked red / Unlocked green / **Denied amber** with the
     "SERVICE VOID — NO ATMOSPHERE" lore beat, which is exactly the flavour a
     maintenance bore wants on most of its doors.
   * The code-locked door chain (`--test-hatch`: terminal → fire(terminal_code)
     → secret_room.lua → openTrapdoor → real DoorSystem;
     `runSecretRoomSelfTest`).
   * `holo_terminal` for any in-wall info panel interaction.
   * The flipbook-atlas loader pattern (`rifthub.cpp loadFlipbookAtlas`, 8×6
     atlas, `tools/make_membrane_flipbook.py`) for ANIMATED screen content.
   * `SurfaceLibrary` sets: `mw_metal_grate` (walkway deck), `sr_metal_b` /
     `mw_metal_panels_a` (doors), `mw_concrete_panels_b` (niche reveals),
     `sr_floorstripes` (kerb edge marking).
6. **Determinism is a hard rule.** Burned-out / flickering strip selection must
   be a hash of quantised position (strip index × route identity), NEVER
   `rand()` — or captures stop being reproducible and every screenshot diff in
   this lane dies. Flicker may animate with time interactively, but captures
   must evaluate at t = 0.
7. **Teardown.** `TunnelCorridorWorld` owns everything it builds
   (m_meshes/m_bodies/m_lights); interior geometry joins those ledgers so the
   streamed city bores still tear down cleanly (`CITY_BORES_PLAN.md` N1).

## Acceptance conditions

Execution iterates until ALL hold. Each is checkable — by a test, a log line,
or a named capture. No condition is "looks good".

### Walkways
- [ ] W1. Raised walkway BOTH sides, kerb height 0.30 m, deck width ≥ 0.85 m,
      running the full shell span; no gap in deck > 0.1 m of arc length.
- [ ] W2. Walkways COLLIDE: a character capsule walks one walkway end to end
      without falling onto the carriageway (assert in test, not by eye), and a
      kerb-face exists so a car brushing it is deflected, not launched.
- [ ] W3. Headroom ≥ 2.0 m over the deck along its whole run: no strip, screen,
      door frame or prop intrudes. Assert geometrically.
- [ ] W4. The walkway does not breach the shell: deck outer edge ≤ the wall
      springing line (7.0 m) everywhere.

### Doors + rooms
- [ ] D1. Access niches every 100–140 m of bore, alternating sides, recessed
      ≤ 0.55 m (constraint 3); each carries a door (≈1.0 × 2.1 m) + a
      `buildKeypad()` unit at 1.4 m height beside it.
- [ ] D2. Doors run the EXISTING KeypadEntry/DoorSystem chain: wrong code →
      stays locked (negative control), right code → opens, and at least one
      door is the amber DENIED service-void variant that never opens (also a
      negative control — a door system that cannot refuse is not a lock).
- [ ] D3. Behind at least one door: a plant room (pumps/vents/pipes props, one
      re-aimed real light or emissive-only). Behind another: a stairhead with a
      real descending stair (reused stair prims) to an underground-complex
      landing — the tie-in point for the elevator/free-flight easter egg
      (task #9). Both live in the free volume behind the wall (constraint 3).
- [ ] D4. Every enterable space is reachable AND escapable: the self-test walks
      a character in and back out of each room; no soft-locks.

### Lighting
- [ ] L1. Per-strip STATE (lit / dead / flickering) = pure function of
      (strip index, route name hash). Two runs produce byte-identical strip
      states; the self-test asserts the same seed twice == same states and a
      different route != (negative control).
- [ ] L2. 8–20 % of strips DEAD (zero emissive — they read as neglect and cost
      nothing), 2–5 % FLICKERING (emissive modulated by a hash-derived phase;
      t = 0 in captures).
- [ ] L3. REAL point-light spend per bore INCLUDING interiors stays exactly
      `kTcMaxBoreLights` (6) — interiors re-aim or borrow, never add. Log the
      count; the city-wide cap stays `CITY_BORES_PLAN.md` B1's 32.

### Screens (the CP2077 beat)
- [ ] S1. ≥ 2 wall screens per bore on the vertical band, emissive GEOMETRY
      (no light cost), flipbook-animated via the existing atlas loader;
      captures pin the flipbook to frame 0.
- [ ] S2. Screens sit ≥ 2.2 m over the deck (respects W3), never overlap a
      strip, niche or door, and are absent from any DEAD-strip dark stretch
      longer than 45 m (a working billboard in a dead zone reads wrong — and
      this is the one aesthetic rule in the plan, stated so it is checkable).

### Budget / regression
- [ ] B1. Interior adds ≤ 40 entities per bore (count logged; renegotiate with
      a measurement, not silently).
- [ ] B2. `--test-worldstream` ledgers not worse than the pre-existing 16/2
      (texture c/d 76/112). If interiors add to the double-free, say so.
- [ ] B3. `X3_TUNNEL_INTERIOR=0` restores the bare bore EXACTLY (fallback
      doctrine — same class as X3_TUNNEL_PORTAL_CUT).
- [ ] B4. `--test-tunneldrive` still 11/11: the walkway/kerb must not narrow
      the drivable envelope below what the rig needs.

### Evidence
- [ ] E1. Captures: walkway run, door niche + keypad close-up, a dead-strip
      stretch, a lit screen — committed alongside the change.
- [ ] E2. `--test-tunnelinterior` headless self-test covering W1/W2/D2/D4/L1/
      L2/L3, with at least one negative control per group (a check that cannot
      fail is not a check).

## Execution order (when this plan is picked up)

1. Walkways + kerbs (W1-W4) — they are also the collision that keeps cars off
   the deck, and everything else mounts relative to them.
2. Strip STATE (L1-L2) — smallest change with the biggest mood shift, and it
   forces the deterministic-hash plumbing everything else reuses.
3. Niches + doors + keypads (D1-D2) on the demo bore only.
4. One plant room + one stairhead (D3-D4).
5. Screens (S1-S2).
6. Only then scale to the four city bores, under CITY_BORES_PLAN's own
   conditions (shared SurfaceLibrary, light cap, teardown ledgers).

## Open questions for Tim

1. Screen CONTENT: ad loops (the CP2077 vibe), tunnel status boards
   ("LANE 1 CLOSED"), or both alternating? Needs 2-3 flipbook atlases either
   way — candidates can be baked with tools/make_membrane_flipbook.py.
2. Door codes: discoverable in-world (holo_terminal note in the plant room?)
   or reuse an existing canon code? (kShowroomHatchCode 2742 is taken.)
3. How deep does the underground complex go before it links to the task #9
   elevator work — one landing, or a real shaft?

---

## TIM'S INTERIOR BRIEF — verbatim, 2026-08-13

Captured directly. The plan above was written from a second-hand summary; this is
the source. Where the two disagree, THIS wins.

> "sidewalks, access points, doors with keypads and rooms and stairs and
>  underground complex access.. lighting.. some LED, some burned out... some
>  video screens like CP2077"

and, expanded later the same day:

> "Sidewalks... metal railings.. doors... command consoles in rooms behind keypad
>  access doors... down halls.. shoulders you can pull off on.. maintenance
>  sections"

### The full element list, as given
* **Sidewalks** — a walkway, both sides.
* **Metal railings** — along the walkway. Not decorative: this is what makes a
  tunnel read as infrastructure rather than a tube.
* **Doors**, with **keypad access**.
* **Command consoles IN THE ROOMS behind those doors.** The rooms are not empty
  volume — they have a purpose and something to interact with. Reuse the existing
  HoloTerminal + KeypadEntry chain (`--test-keypad` KP1-KP6, `--test-hatch`).
* **Halls** — the rooms lead DOWN HALLS, not straight into a single box. There is
  depth behind the door.
* **Stairs**, and **underground complex access** — the halls go somewhere. This is
  where the tunnel meets the elevator/free-flight easter-egg work (GAME_BACKLOG §3).
* **SHOULDERS YOU CAN PULL OFF ON** — breakdown/lay-by bays wide enough to leave
  the running lane and park. NOT in the earlier summary and easy to miss: this is
  a DRIVING feature, not set dressing. It needs real width in the bore
  cross-section and drivable collision, so it affects the tube profile itself —
  decide it before the shell geometry is finalised, not after.
* **Maintenance sections** — stretches that read as service areas rather than
  running tunnel: plant, cabling, equipment bays.
* **Lighting: some LED, some BURNED OUT.** Deterministic hash of position, never
  random, or captures stop being reproducible.
* **Video screens, CP2077-style.** Emissive panels with content; the
  `membrane_flipbook*` surface_library sets can drive animation.

### The one that changes the geometry
Everything else is dressing inside the existing tube. **The pull-off shoulders are
not** — they widen the bore. Size them against the vehicle (the drive test rig is
the measure) and settle the cross-section BEFORE building walkways and railings
against a profile that is about to change.
