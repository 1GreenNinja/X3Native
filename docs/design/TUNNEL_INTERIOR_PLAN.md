# Plan — TUNNELS AS INFRASTRUCTURE: the interior program at network scale

Status: DRAFT v2, pre-execution. v1 specified the interior of ONE showcase bore.
v2 re-reads the whole program in the light of `ROAD_NETWORK_PLAN.md`: the ring
tours cross mountain ranges, Tim ruled *"We CAN drive through a mountain!!!!
we have TUNNELS!!!!"*, and the tunnel therefore stops being a set piece and
becomes a REPEATABLE ELEMENT the road network places wherever it meets a range.
Conditions still written before any code — that discipline caught the city
bores' three portal defects as named conditions instead of capture-and-squint
iterations, and it stays.

Companions: `ROAD_NETWORK_PLAN.md` (the network this now serves),
`CITY_BORES_PLAN.md` (the four city bores), `TUNNEL_NEXT.md` (open work; see
the superseded-items note below), `TUNNEL_MOUTH_LOD.md` (the two LOD bugs).

UNITS: **feet, miles, mph** for everything Tim reads. Engine data stays SI;
where a CODE CONSTANT is quoted it is quoted in metres with feet alongside, so
nobody mis-edits a source file from this doc.

## Sharpening pass (2026-08-15) — tunnels as INFRASTRUCTURE

What changed: the road-network plan puts ~62 miles of authored road on the map,
and its own sharpening pass proved the outer tour cannot circle the country
without meeting the N and W ranges. Tim's answer makes the tunnel the crossing
mechanism. That means **4–8+ bores** (the demo, four city freeway bores, and
one bore per authored range crossing), not one showcase — a different
engineering problem the v1 spec had never been read against. Findings, several
of which are corrections of things v1 states as fact:

1. **v1's light number is WRONG — the bore spends 8 real lights, not 6.**
   `kTcMaxBoreLights` (6) is only the mid-bore spread; the builder then adds
   one PORTAL light per mouth (`tunnel_corridor.cpp` ~1683, and its own comment
   says "8 … in total for the whole tunnel"). Consequence: the four city bores
   alone spend 32 — **exactly** `CITY_BORES_PLAN.md` B1's cap, zero headroom —
   and 8 network bores = 64 = the ENTIRE legacy pooled-light budget. Static
   per-bore budgets cannot scale; lights must become a per-frame nearest-K
   merged pool (condition I3), which is ALSO the fix for the already-diagnosed
   interactive black-bore bug (`TUNNEL_NEXT.md` §6: lights are submitted once
   at boot and overwritten on frame 2).
2. **CITY_BORES_PLAN B3 (shared SurfaceLibrary) was NEVER DONE — verified.**
   `TunnelCorridorWorld` owns `SurfaceLibrary m_surf` per instance and
   `build()` mounts and loads its 3 surface sets (9 texture maps) per call
   (`tunnel_corridor.cpp:945–948`). Eight bores = eight uploads of the same
   concrete and asphalt. Promoted from "do first when scaling" to a hard
   BLOCKER (condition I2) before any second dressed bore.
3. **v1's "free volume behind the wall" claim (constraint 3) is now only
   conditionally true.** It was written for the OLD bored build, where the
   heightfield was the roof and everything under it was empty. Under
   cut-and-cover the corridor is cut to road level for its whole length and
   the hillside over the tube is a reconstructed LID MESH. Rooms are still
   nearly free — but only under the roofed span, only below the lid's
   underside, and only where they stay inside the rising cut batter. On the
   approach cuttings there is no lid and a room would punch through the
   visible cut face into open air. Constraint 3 is rewritten below with the
   actual envelope.
4. **The interior plan's own regression gate is stale.** v1's B4 says
   "`--test-tunneldrive` still 11/11". The test is currently **6/12**: A2/A3/B1
   assert the earth ramp and the portal holes that the cut-and-cover rebuild
   deleted (`--test-tunnelmouth` is 7/7 precisely because the defect is gone by
   construction). Rewriting those three assertions is therefore no longer
   "separate debt" (as `ROAD_NETWORK_PLAN.md`'s NOT-list has it) — it is step 0
   of THIS plan, because every interior condition that says "the drive test
   still passes" is meaningless while the baseline is red.
5. **The two LOD plans collide, and the reconciliation matters at 62 miles.**
   `TUNNEL_MOUTH_LOD.md` fix (a) pins corridor tiles to FULL (fine for one
   640 m demo: ~40 tiles). `ROAD_NETWORK_PLAN.md` B3 caps corridor tiles at
   HALF. At network scope, pin-to-Full is ~5,000 tiles ≈ 5M vertices held at
   full density — untenable. Reconciliation, stated here so neither doc wins
   by accident: **Full within ~250 ft of a portal** (the seam the player
   stares at), **Half elsewhere on corridors** (the road plan's rule), plus
   the hysteresis fix (b) which is scale-independent. The mouth-seam assert
   from TUNNEL_MOUTH_LOD survives unchanged.
6. **One bore's program is not eight bores' program.** Walkways, railings,
   niches, strips and screens scale; the rooms/halls/stairs/underground-complex
   story does NOT — eight copies of the same secret command room is exactly
   the procedural slop this project refuses. v2 splits the program into a
   repeatable KIT and an authored IDENTITY (per-bore table) plus a TIER system
   (below), and draws the anti-slop line explicitly: which axes may be seeded,
   which must be authored, and which must not vary at all.
7. **Two elements of Tim's brief had NO acceptance conditions at all**: the
   metal railings ("what makes a tunnel read as infrastructure rather than a
   tube" — his emphasis) and the pull-off shoulders (the one element that
   changes the tube profile). Both get real geometry in feet and real
   conditions below. Command consoles and "down halls" were named in the brief
   but absent from the conditions; fixed.
8. **Length is now a variable.** The demo route is ~2,100 ft end to end; a
   range crossing on the outer tour can want a 0.5–1 mile bore. A LONG-BORE
   section below specifies what a mile of tunnel needs that 500 ft does not:
   a travelling light window, distance furniture, lay-by spacing, mesh
   chunking for culling, drainage grade, and an audio flag.
9. **The bore must learn to be PLACED, not to place itself.** Today
   `TunnelSpec` is (centre, heading, halfLen) and the module samples the hill,
   grades its own road and picks its own datum. Ring roads arrive with their
   own polyline, their own grading and their own corridor chain (P1). The
   integration contract — tunnel-as-a-reach-of-the-route, with datum
   continuity at the hand-offs — is specified below (N-group). This is the
   single biggest thing the tunnel owes the network.

**`TUNNEL_NEXT.md` status notes** (recorded here per protocol, that file is
not edited):
* §1 (the inert mountain diagnosis + NEXT-STEP list) — **dead/resolved**: the
  tunnel ridge exists and is real enough that `ROAD_NETWORK_PLAN.md` routes a
  935 ft hillclimb over it and `TUNNEL_MOUTH_LOD.md` bug 1 is about it
  VANISHING at LOD, not about it being absent.
* §3 (the interior sketch) — already marked superseded by this plan.
* §7 (portal plugs/holes, the 11/11 outcome) — **superseded by the
  cut-and-cover rebuild**; its machinery is what A2/A3/B1 still assert (see
  finding 4). Its lesson ("a number the code volunteers about itself is not
  automatically benign") stays canon.
* §6 (lights submitted once, overwritten on frame 2) — **not dead: promoted.**
  It is condition I3 here; infrastructure cannot ship on a boot-once light
  submission.
* §2 (NFS roadside-furniture research) — alive and now serves the whole
  62-mile network, not just the tunnel approach.

## THE BORE CENSUS + TIERS

The network's tunnels, as currently decided or foreseeable:

| bore | where | length class | tier |
|---|---|---|---|
| demo / showcase | the tunnel ridge (−592,−352), under the hillclimb | ~2,100 ft route | **A** |
| 4 city freeway bores | `registerCityFreewayTunnels()` | short–medium | **B** |
| outer-tour range crossings | 1–2 authored crossings (N and/or W range), IF Tim routes through rather than around | up to ~1 mile | **B** (long-bore rules apply) |
| future underpasses / spur cuts | wherever a spoke meets a ridge | < 500 ft | **C** |

**Tier A — the showcase (exactly ONE).** Full program: walkways, railings,
niches, keypad doors, plant room, command-console room, halls, stairhead to
the underground complex (the task #9 elevator tie-in). The STORY lives here
and only here.

**Tier B — infrastructure bores.** Walkways + railings + kerbs, strip state
(dead/flicker per wear tier), niches with doors — **most of them the amber
DENIED service-void variant** (a locked door is one mesh + one keypad; a room
is forty entities), at most ONE openable plant room per bore, screens per the
identity table, lay-bys per the length table, maintenance section if > 2,000 ft.
No stairs, no underground access, no console rooms — the story does not repeat.

**Tier C — underpasses.** Strips + the concrete verge band + portal treatment.
No doors, no walkway kerb (the band stays flat), no screens. A 400 ft
underpass dressed like the showcase reads as a theme-park ride.

## Verified constraints (updated — measured or read from code, not assumed)

1. **There is 3.3 ft of floor per side for a walkway.** The road ribbon is
   ±19.7 ft (`kTcRoadHalfWidth` 6.0 m); the tube wall springs at ±23.0 ft
   (`kTcTubeHalfWidth` 7.0 m). Real bore walkways are 2.6–3.9 ft, so the
   3.3 ft band is authentic — and there is zero slack: kerb and railing
   thickness come out of it. NOTE: this band ALREADY EXISTS as a flat,
   colliding concrete verge (`walk` mesh, `tunnel_corridor.cpp` ~1101–1201,
   dressed in the portal concrete set). The walkway program RAISES and kerbs
   an existing mesh; it is not a green field.
2. **Wall height before the arch is 11.8 ft** (`kTcTubeWallH` 3.6 m); the
   crown is 24.9 ft (`kTcTubeCrownH` 7.6 m). Doors (~6.9 ft) and screens fit
   on the vertical wall band without touching the arch.
3. **The room envelope (REWRITTEN for cut-and-cover).** The shell is 3.0 ft
   thick (`kTcShellThick` 0.9 m); a niche may recess ~1.8 ft before breaching
   the outer skin. Volumes deeper than that hang OFF the niche behind the
   wall, and they are free ONLY inside this envelope:
   * **Under the ROOFED span only.** There, the heightfield is cut to road
     level, the hillside above is the backfill LID mesh, and the space between
     the shell's outer skin and the cut batter is genuinely empty — no terrain
     collision, no excavation, no portal hole needed.
   * **Below the lid.** Room ceilings stay ≥ 1 ft under the lid's underside.
     `tunnelLidHeightAt()` is public precisely so a test can ask the real
     surface — assert it, per room, the way M3 asserts the lid clears the
     shell.
   * **Inside the batter.** The corridor's flat floor ends at 28.9 ft off
     centre (`kTcCorridorHalfW` 8.8 m) and the ground then rises over a 46 ft
     falloff. A room extending laterally must stay under that rising surface
     or it pokes out of the hillside. Practical depth for a 8 ft-tall room:
     roughly 10–15 ft beyond the wall before the check is needed at all;
     beyond that, assert against `terrainHeightAtWorld`.
   * **NEVER on the approach cuttings.** No lid there; the cut face is
     visible from above and a room would breach it. Approach cuttings get no
     doors — the retaining walls are only 1.8 ft thick and hold back real
     ground.
4. **Light budget (CORRECTED).** Each dressed bore spends **8 real pooled
   lights today**: 6 spread through the bore (`kTcMaxBoreLights`, range
   112 ft) + 1 per mouth (range 98 ft). The legacy path pools 64 per frame;
   the clustered path (`r_clusterlights 1`, 1024) exists but every md5 gate
   pins legacy, so the work must read on BOTH (same doctrine as
   `ROAD_NETWORK_PLAN.md`'s bridge). Consequences:
   * Interiors add ZERO pooled lights — emissive-first, re-aim the 8, never
     add (unchanged from v1, now with the right number).
   * At network scale even 8/bore is untenable statically (8 bores = the
     whole budget). The spend becomes a per-frame merged pool: each bore
     CONTRIBUTES its 8 to a host-side nearest-K selection alongside street
     lamps and region lights, ≤ 16 tunnel lights submitted per frame
     regardless of bore count (I3). This is also the fix for the black-bore
     bug (`TUNNEL_NEXT.md` §6) — one lane, two defects.
5. **Mechanisms that already exist and MUST be reused, not reinvented:**
   * `KeypadEntry` + `buildKeypad()` (`--test-keypad` KP1–KP6), with the
     Locked red / Unlocked green / **Denied amber** "SERVICE VOID — NO
     ATMOSPHERE" beat — the default state for Tier-B doors.
   * The code-locked door chain (`--test-hatch`: terminal → fire(code) →
     openTrapdoor → real DoorSystem).
   * `holo_terminal` for the command consoles.
   * The flipbook-atlas loader (`rifthub.cpp loadFlipbookAtlas`, 8×6 atlas,
     `tools/make_membrane_flipbook.py`) for animated screens.
   * `SurfaceLibrary` sets: `mw_metal_grate` (walkway deck), `sr_metal_b` /
     `mw_metal_panels_a` (doors), `mw_concrete_panels_b` (niche reveals),
     `sr_floorstripes` (kerb edge marking).
   * `StreetLights`' deterministic dead/flicker variance and nearest-K
     `selectLights()` lane — the merge pattern I3 adopts.
6. **Determinism is a hard rule.** Per-strip and per-fixture state is a hash
   of (strip index × route-name hash), NEVER `rand()`. Flicker may animate
   interactively; captures evaluate at t = 0.
7. **Teardown.** `TunnelCorridorWorld` owns everything it builds
   (m_meshes/m_bodies/m_lights, 10 uploads and ~8 static bodies per bore
   today); interior geometry joins those ledgers so streamed bores tear down
   cleanly at any count (`CITY_BORES_PLAN.md` N1).

## PER-TUNNEL IDENTITY vs THE SHARED KIT — the anti-slop line

Eight bores must not read as eight copies of one corridor. They also must not
read as eight rolls of a dice. The split:

**The KIT (shared, identical everywhere, by design):** cross-section, walkway
and railing profiles, kerb height, niche/door/keypad geometry, strip fixture
mesh, screen frames, signage plate geometry, the deterministic-hash plumbing.
Real highway authorities build identical fixtures into every tunnel they own;
uniformity of HARDWARE is realism, not slop.

**The IDENTITY (authored, one hand-written row per bore).** A small table,
checked in as data, that a human wrote and can defend:

| field | values | example |
|---|---|---|
| name | authored string — **Tim writes these** | "CINDERCONE PASS TUNNEL — ELEV 2,340 FT" on the portal plate |
| lining set | one of 2–3 surface families (`cc_cement_white`, `mw_concrete_panels_a/b`) | city bores panelled, range bores raw cement |
| light temperature | warm / neutral / cool (strip emissive tint + real-light colour) | old city bore sodium-warm; new range bore LED-cool |
| wear tier | pristine (≤ 2 % dead strips) / worn (~10 %) / neglected (~20 %, more flicker, grime decals) | the showcase bore is WORN — that is its mood |
| screen content set | ads / status boards / none | CP2077 ads in the city, "LANE 1 CLOSED" boards in the range, NONE in a neglected bore (a working billboard in a dead tunnel reads wrong — v1's S2 rule, kept) |
| program tier | A / B / C | exactly one A |

**Seeded variation is allowed ONLY below the identity line:** which strips are
dead (hash), flicker phases, grime decal placement, which side each niche
alternates to. **NEVER seeded:** any dimension, any signage or screen text,
room layouts, door codes, the identity fields themselves. **NEVER copied:**
the Tier-A story. If a variation axis cannot be named in the table, it does
not vary. A procedurally-smeared tunnel is worse than a plain one.

## COST AT MULTIPLES — the budget, measured then multiplied

Per dressed bore TODAY (read from `tunnel_corridor.cpp`, before interiors):

| resource | per bore | 8 bores | verdict |
|---|---|---|---|
| meshes / draws | 10 (slab, paint, verge, walk band, shell, lid, portals, walls, berm, strips) | 80 | fine IF chunked for culling on long bores (see LONG BORE) |
| static physics bodies | ~8 | ~64 | fine for Jolt; teardown ledger must hold (I4) |
| real pooled lights | **8** (6 + 2 portal) | **64 = the whole legacy budget** | BLOCKER → I3 merged pool |
| texture uploads | 9 maps (3 sets × alb/nrm/mr) **per instance** | 72 uploads of the same 9 files | BLOCKER → I2 shared library (`CITY_BORES_PLAN` B3, verified NOT done, `tunnel_corridor.cpp:945`) |
| lid triangles | ~8 k | ~64 k | fine |
| interior entities (this plan) | ≤ 40 (Tier A) / ≤ 24 (Tier B) / ≤ 8 (Tier C) | ~150–200 across the census | fine, budgeted per tier |

Budget per bore, binding: **Tier A ≤ 40 interior entities, Tier B ≤ 24,
Tier C ≤ 8; zero added pooled lights at any tier; zero added texture SETS
beyond the shared library's existing families** (a new set needs a named
reason in the identity table). Counts logged at build — a budget nobody logs
is a wish (B1).

## THE LONG BORE — what a mile needs that 500 ft does not

The demo route is ~2,100 ft; a range crossing can want 0.5–1 mile. At
5,280 ft, taking the current builder as-is:

* **Lighting breaks first.** 6 mid-bore lights spread evenly = one pool of
  light every 880 ft with 112 ft range → ~660 ft of pooled-light darkness
  between pools; the car receives no dynamic light for most of the drive even
  though the emissive strips (107 of them at the 49 ft interval) still glow.
  **Fix: the LIGHT WINDOW** — the bore's 6 contributed lights sit at the 6
  strip stations nearest the player, positions quantised to strip stations so
  they are deterministic, selection by proximity in the same I3 merge. A
  travelling pool of real light inside an emissive tube. Captures pin the
  window to the camera station.
* **Progress sense.** A mile of identical strips is a treadmill. Real long
  tunnels are legible by furniture, all cheap and all authored:
  * **chainage plates** every 500 ft (white-on-blue, authored numerals);
  * **EXIT distance signs** both directions every ~1,000 ft ("EXIT 2400 FT" —
    green, emissive, and they double as the strongest wayfinding cue);
  * **section bands** — the lining tint or strip temperature steps subtly at
    maintenance-section boundaries, the way real bores change per contract
    section;
  * **jet fans** (paired ceiling cylinders, static props) in bores > 3,000 ft
    — the single most recognisable long-tunnel silhouette;
  * screens as EVENTS (one per ~1,500 ft max), never wallpaper.
* **Lay-bys** (Tim's pull-off shoulders): real practice for long bidirectional
  tunnels is a lay-by roughly every 0.6 mi, alternating sides. Table: bores
  < 1,500 ft — none; 1,500–3,000 ft — one; per mile — two, alternating.
  Geometry in the SH-group below.
* **Maintenance section**: one 150–250 ft section per bore > 2,000 ft
  (MS-group below); on a mile bore, two, and they are where the section bands
  step.
* **Streaming / culling.** The bore is built by the host at boot as one
  resident object; terrain streams around it. A mile-long shell as ONE mesh
  defeats frustum culling (its bbox is always in view somewhere) and makes a
  future streaming story impossible. **Chunk every mesh at ~500 ft station
  boundaries** (shell, walkways, strips, walls) — draw count rises to ~10 per
  chunk but off-screen chunks cull, and a chunk is a natural future streaming
  unit. Chunks share station vertices — the weld discipline, no butt joints.
* **Grade + drainage.** A dead-flat mile of road in a mountain reads wrong and
  real bores forbid it (water). Minimum longitudinal grade **0.3 %** through
  any bore, max stays 4.5 % (`kTcMaxGrade`); profile either continuous fall or
  a mid-bore high point. Kerbside gully slots (visual only, kit geometry)
  every ~165 ft.
* **Audio (flagged OPEN, one line).** Nothing anywhere specifies tunnel
  acoustics; a reverb/lowpass zone per bore is the single strongest "you are
  inside a mountain" cue and belongs to whichever lane owns audio. Named here
  so it is not lost; not conditioned here.

## WHAT THE TUNNEL OWES THE ROAD NETWORK

Today the module SITES ITSELF: `TunnelSpec` is centre + heading + halfLen, it
samples the hill, grades its own road, picks its own datum. Infrastructure is
placed by the network instead. The contract:

1. **Tunnel-as-a-reach.** `TunnelSpec` (or a sibling) accepts a POLYLINE REACH
   of the ring route — the route's own stations — instead of inventing one.
   The bore's corridor chains with the road's ≤ 32-node corridors sharing
   endpoint nodes (the P1 pattern), so the carve union is seamless by
   construction.
2. **Datum continuity.** |tunnel roadY − road roadY| ≤ **0.2 ft** at both
   hand-off stations (the same weld number as the bridge's B4), and grade is
   continuous across the joint (no crease > 0.5 % between adjacent stations).
   The tunnel takes the ring's grading through the hill; it does not re-grade.
3. **The bore on a CURVE — newly possible (P1), completely unspecified until
   now.**
   * Minimum curve radius INSIDE a bore: **1,200 ft** (the road plan's
     sweeper class — a bore should never be the tightest thing on the route).
   * **Portals sit on tangent**: the last 150 ft at each end straight, or
     R ≥ 2,000 ft — real practice, and it means the mouth is seen square
     before it is entered, which is also what keeps the headwall/wingwall
     builder's square-to-tangent assumption honest.
   * **No superelevation through a bore** (extends the road plan's G4
     bank-to-zero-on-decks rule): the walkway band, kerb heights and niche
     floors all assume a level cross-section; bank runoff completes before
     boreS0.
   * Sweep fidelity: station spacing already gives chord error ≪ 0.1 ft at
     R = 1,200 ft — no change needed, but the CURVED variants of the mouth
     gates must actually run (C1): M1/M3/M4 on a route with a deliberate 30°
     bend, because the lid-seam exactness proof has only ever run straight.
4. **Grade through the bore** per the LONG BORE rules (0.3–4.5 %), supplied by
   the network's profile, not chosen locally.
5. **Portals and the dark country.** The road plan decreed rural rings stay
   DARK between settlements. A tunnel portal is therefore an ARRIVAL: the only
   exterior light is the threshold treatment (the existing per-mouth light +
   emissive headwall wash), no approach lamps. At night a range bore reads as
   a lit arch floating in black country — that is the shot, and it costs
   nothing we do not already spend.
6. **Portal identity**: the name plate (identity table) on the headwall, and
   at range crossings the portal is the range's nameplate too — the outer
   tour's legibility comes from knowing WHICH mountain you are entering.
7. **Emergency crossovers: explicitly N/A for now.** Our bores are single
   bidirectional tubes (39 ft roadway); cross-passages exist between TWIN
   tubes, which nothing in the network plans. If a range crossing ever gets
   twin bores, cross-passages every ~1,000 ft become a condition; until then
   the SOS niches (D-group) are the emergency story. Recorded so nobody
   invents corridor-connecting rooms ad hoc.

## Acceptance conditions

Execution iterates until ALL hold. Each is checkable — by a test, a log line,
or a named capture. No condition is "looks good". (Letter groups: W walkways +
railings, SH shoulders, D doors/rooms, MS maintenance, L lighting, S screens,
I infrastructure/scale, C curve, N network hand-off, B budget, E evidence.)

### Step 0 — the baseline (blocks everything)
- [ ] Z1. `--test-tunneldrive` A2/A3/B1 rewritten for cut-and-cover: the
      negative control becomes X3_TUNNEL_PORTAL_CUT=0 restoring the OLD field
      exactly (assert the fallback, not the deleted ramp), and the hole-count
      assertions die with the holes. Test green at its new count BEFORE any
      interior work lands on top of it.

### Walkways + railings
- [ ] W1. Raised walkway BOTH sides: kerb height 1.0 ft, deck width ≥ 2.8 ft,
      full roofed-span run; no deck gap > 0.33 ft of arc. (Rebuilds the
      existing flat `walk` band — see constraint 1.)
- [ ] W2. Walkways COLLIDE: a character capsule walks each walkway end to end
      without falling onto the carriageway (asserted, not eyeballed); the
      kerb face exists and is vertical.
- [ ] W3. Headroom ≥ 6.6 ft over the deck along its whole run: no strip,
      screen, door frame, sign, fan or prop intrudes. Assert geometrically.
- [ ] W4. The walkway does not breach the shell: deck outer edge ≤ the
      springing line (23.0 ft) everywhere, including through niches and bays.
- [ ] W5. **METAL RAILINGS** (new — the element Tim called out, previously
      unconditioned): posts every ~6.5 ft, top rail at 3.5 ft and mid rail at
      1.75 ft above the deck, along the deck's carriageway edge, breaking only
      at niches and lay-bys; `sr_metal_b`-family material; COLLIDES.
- [ ] W6. The kerb+railing deflect, not launch: a scripted car brushing the
      kerb at 60 mph / 15° stays on the carriageway with vertical
      acceleration spike < 0.5 g (the N1 discipline applied to the kerb).
      Negative control: same run with railings/kerb collision disabled must
      mount the deck — a barrier that cannot be missed is not being tested.

### Pull-off shoulders (Tim's lay-bys — the profile-changing element)
- [ ] SH1. Lay-by geometry: local widening of the tube on ONE side from
      23.0 ft to ~35 ft half-width at road level — bay length 110 ft plus
      40 ft entry/exit tapers; walkway, railing and wall follow the widened
      profile; the corridor's flat floor widens with it (per-station
      half-width) and the LID covers the bulge. Counts per the length table
      (LONG BORE); alternating sides.
- [ ] SH2. The bay is drivable: scripted pull-in at 30 mph, full stop clear
      of the running lane (≥ 10 ft from the white line), rejoin — no
      collision, no step > 0.2 ft anywhere in the manoeuvre.
- [ ] SH3. The M-suite holds THROUGH a bay: M1 (no ground above road), M3/M4
      (lid clearance + seam exactness) pass on a route with at least one
      lay-by — the profile transition is exactly where the lid and corridor
      derivations can silently disagree.
- [ ] SH4. Profile transition rate ≤ 1 ft of half-width per 10 ft of arc
      (the taper reads as built, not morphed).

### Doors + rooms (Tier A fully; Tier B per its reduced program)
- [ ] D1. Access niches every 330–460 ft of bore, alternating sides, recessed
      ≤ 1.8 ft; each carries a door (3.3 × 6.9 ft) + `buildKeypad()` at
      4.6 ft beside it; niche floor flush with the walkway deck ± 0.1 ft and
      door frames flush with the niche reveal (a floating niche passes v1's
      wording and looks broken — this closes that).
- [ ] D2. Doors run the EXISTING KeypadEntry/DoorSystem chain: wrong code
      stays locked (negative control), right code opens; on Tier B the
      DEFAULT door is the amber DENIED service-void variant that never opens.
- [ ] D3. Tier A only: behind one door a PLANT ROOM (pumps/vents/pipes,
      emissive-lit or one re-aimed light); behind another a HALL ≥ 6.5 ft
      wide × 8 ft tall running ≥ 20 ft before opening into its room (the
      brief's "down halls" — depth behind the door, not a box against the
      wall); the hall route ends at a STAIRHEAD with a real descending stair
      to the underground-complex landing (task #9 tie-in). All inside the
      constraint-3 envelope, each room's ceiling asserted against
      `tunnelLidHeightAt()`.
- [ ] D4. Every enterable space is reachable AND escapable: the self-test
      walks a character in and back out of each room THROUGH its hall; no
      soft-locks.
- [ ] D5. **COMMAND CONSOLE** (new — in the brief, absent from v1's
      conditions): ≥ 1 Tier-A room contains a `holo_terminal` console that is
      interactable, gated behind a working keypad door; the chain test drives
      it end to end (keypad → door → console fires).

### Maintenance sections
- [ ] MS1. One 150–250 ft maintenance section per bore > 2,000 ft (two per
      mile): walkway widens to ~6 ft on one side, cable trays at 8 ft, wall
      pipe runs, ≥ 2 equipment cabinets — all outside the W3 headroom
      envelope and the vehicle clearance envelope (assert both).
- [ ] MS2. Section boundaries are where the identity's section bands step
      (lining tint / strip temperature) — the seam is at a bulkhead frame,
      never mid-panel.

### Lighting
- [ ] L1. Per-strip STATE (lit / dead / flickering) = pure function of
      (strip index, route-name hash). Same route twice ⇒ byte-identical;
      different route ⇒ different (negative control).
- [ ] L2. Dead/flicker FRACTIONS come from the identity table's wear tier
      (pristine ≤ 2 %, worn ~10 %, neglected ~20 % dead; flicker 2–5 %),
      not from a global constant. Captures at t = 0.
- [ ] L3. Interior adds ZERO pooled lights at any tier: per-bore contribution
      stays exactly the existing 8 (6 bore + 2 portal — the CORRECTED
      number), re-aimed as needed. Logged per bore.

### Screens
- [ ] S1. Screens per the identity table's content set: emissive geometry,
      flipbook-animated, ≥ 2 per Tier-A/B bore that carries them; captures
      pin frame 0.
- [ ] S2. Screens sit ≥ 7.2 ft over the deck, never overlap a strip, niche,
      sign or door, and are absent from any dead-strip stretch longer than
      148 ft AND from any bore whose wear tier is "neglected" (the aesthetic
      rule, stated so it is checkable).

### Infrastructure / scale (all new)
- [ ] I1. The IDENTITY TABLE exists as data; every dressed bore names its
      row; no two bores share the same (lining, temperature, wear) triple;
      names are authored strings (Tim's), never generated.
- [ ] I2. ONE SurfaceLibrary serves all bores: total tunnel texture uploads
      with N bores == with 1 bore (log the count; this is `CITY_BORES_PLAN`
      B3, verified undone at `tunnel_corridor.cpp:945`, promoted to blocker).
- [ ] I3. Lights are a PER-FRAME MERGED POOL: every bore contributes; the
      host submits ≤ 16 tunnel lights per frame regardless of bore count via
      the nearest-K lane street lamps already use; driving through any bore
      at night is LIT interactively (this closes `TUNNEL_NEXT.md` §6's
      black-bore bug — the boot-once submission dies here). Long bores place
      their contribution by the LIGHT WINDOW rule.
- [ ] I4. Build/teardown ledger balance at N bores: build 8, tear down 8,
      zero leaked meshes/bodies/textures/lights (`--test-worldstream`
      ledgers not worse than baseline).
- [ ] I5. Long-bore meshes are CHUNKED at ~500 ft station boundaries with
      welded shared verts; a capture-frame draw count from inside a mile bore
      is < 50 % of the bore's total chunk draws (culling demonstrably works).

### Curve (all new — the P1 consequence)
- [ ] C1. The full M-suite (M1–M6) passes on a route with a deliberate 30°
      bend; the lid seam (M4) is exact on the curve, not just straight.
- [ ] C2. Portal tangency: the 150 ft approaching each portal is straight or
      R ≥ 2,000 ft; the headwall is square to the LOCAL tangent (asserted
      from route data at build).
- [ ] C3. Zero superelevation inside boreS0..boreS1; bank runoff completes
      outside. Walkway deck, kerb and niche floors are level across the
      section through the whole bore.
- [ ] C4. Strips, screens, niches, railings and signs all place via the LOCAL
      frame (`frameAt`) — on the curved test route, every fixture's lateral
      offset is perpendicular to the road at its station (asserted, since
      "should be free" is not a property until proven).

### Network hand-off (all new)
- [ ] N1. A bore can be registered FROM A ROUTE REACH: spec carries the ring
      route's stations; the bore's corridor chains with the road's corridors
      sharing endpoint nodes; ONE carve union, no double-cut at the joints.
- [ ] N2. Datum weld: |tunnel roadY − road roadY| ≤ 0.2 ft at both hand-offs;
      grade crease ≤ 0.5 % between adjacent stations across the joint; a
      scripted 60 mph drive across both joints shows vertical spike < 0.5 g.
- [ ] N3. Bore grade obeys 0.3 % ≤ |g| ≤ 4.5 % through the roofed span
      (drainage floor, motorway ceiling), supplied by the network profile.

### Budget / regression
- [ ] B1. Interior adds ≤ 40 (A) / 24 (B) / 8 (C) entities per bore, count
      logged per bore at build; renegotiate with a measurement, not silently.
- [ ] B2. `X3_TUNNEL_INTERIOR=0` restores the bare bore EXACTLY (fallback
      doctrine, same class as X3_TUNNEL_PORTAL_CUT).
- [ ] B3. The rewritten `--test-tunneldrive` (Z1) stays green through every
      interior change: the walkway/kerb/railing/bay geometry must not narrow
      the drivable envelope below what the rig needs.
- [ ] B4. Mouth-LOD reconciliation holds: corridor tiles cap at Half
      (`ROAD_NETWORK_PLAN` B3), EXCEPT tiles within 250 ft of a portal which
      pin Full; the mouth-seam |dY| assert from `TUNNEL_MOUTH_LOD.md` runs in
      the drive test at every camera distance it visits.

### Evidence
- [ ] E1. Captures per dressed bore, from the SAME `showcaseCamera` poses so
      bores are comparable: walkway run with railings, niche + keypad
      close-up, a dead-strip stretch, a lit screen, a lay-by, the portal
      name plate. Committed alongside the change.
- [ ] E2. `--test-tunnelinterior` headless self-test covering W1/W2/W6/SH2/
      SH3/D2/D4/D5/L1/L2/L3/C4/N2, with ≥ 1 negative control per group (a
      check that cannot fail is not a check).
- [ ] E3. The two-bore proof: captures of TWO Tier-B bores side by side that
      a stranger can tell apart (identity working) while every fixture is
      recognisably the same hardware (kit working). This is the anti-slop
      gate made visible.

## Execution order

0. **Z1** — rewrite the three dead drive-test assertions. The lane's own gate
   must be green before anything stacks on it.
1. **I2 + I3** — shared SurfaceLibrary and the per-frame merged light pool.
   The two scaling blockers, and I3 also fixes the black-bore bug Tim already
   hit. Do these BEFORE dressing anything twice.
2. **Decide the lay-by profile on paper** (SH1's geometry) — Tim's own brief
   note: the shoulders change the tube profile, so the cross-section is
   settled before walkways are built against a profile about to change. No
   code yet; the per-station-width machinery lands with SH later.
3. Walkways + kerbs + railings (W1–W6) on the demo bore.
4. Strip STATE + wear tiers (L1–L2) — smallest change, biggest mood shift,
   forces the hash plumbing everything reuses.
5. Niches + doors + keypads (D1–D2) on the demo bore.
6. Plant room, hall, console room, stairhead (D3–D5) — Tier A only, ever.
7. Screens (S1–S2).
8. Identity table + Tier-B rollout to the four city bores (I1, E3), under
   CITY_BORES_PLAN's own conditions.
9. Lay-bys + maintenance sections + long-bore furniture (SH, MS, I5, LIGHT
   WINDOW) — when the first bore > 2,000 ft is routed, i.e. with the ring
   work, alongside N1–N3.
10. C-group lands with P1's curved-route capability as soon as a curved bore
    is authored — C1 (the curved M-suite) can and should run earlier, the
    moment the route layer supports it, because it is pure maths.

## Open questions for Tim

1. Screen CONTENT: ad loops, status boards, or both alternating? (2–3
   flipbook atlases either way, bakeable with
   tools/make_membrane_flipbook.py.)
2. Door codes: discoverable in-world (holo_terminal note in the plant room?)
   or a new canon code? (kShowroomHatchCode 2742 is taken.)
3. Underground complex depth before it meets the task #9 elevator work — one
   landing, or a real shaft?
4. **The outer tour's range crossings: how many bores?** Each is an authored
   decision (a pass OVER is also legitimate — the road plan's summit climb
   proves it). One N-range bore + the pass over the W range? Two bores? This
   sets the census and the long-bore workload.
5. **Tunnel NAMES** — the identity table wants your strings, not generated
   ones. A name per bore, whenever; the plates build around them.
6. Twin tubes: ever? (Decides whether emergency cross-passages ever exist —
   recorded as N/A above until you say otherwise.)

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
