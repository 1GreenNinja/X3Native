# QA MAINLEVEL SWEEP — clipping + surface aberrations, canon facility

**Branch:** `fix/mainlevel-clipping` (worktree off `integration/playable-build` @ 7f3fee8).
**Scope:** the canon facility (`--world canonlevel` == the default boot world), F1-F7,
124 rooms / 180 doorways. Two defect families hunted: **A. clipping** (props vs
architecture, player sightline leaks, camera near-plane) and **B. surface aberrations**
(z-fighting, texture seams/stretching, leftover material bugs).

**Method.**
* Per-room eye-height screenshot survey: `tools/qa_room_sweep.py` (NEW) — 2 shots per
  room down the long axis, generic cells deduped to one per wing; 196 frames, all floors.
  The full survey (~96 MB) is regenerable and NOT committed; the evidence pairs are in
  `docs/screenshots/qa_mainlevel/` (BEFORE_*/AFTER_* + diag_* debug-view frames).
* Targeted `r_debugview 1/2/3` diagnostics on every suspect (the albedo/point-term/normals
  triage cracked D1 in four frames).
* **NEW GATE — the prop-clip lint** (`--test-propclip`, `app/qa_propclip.{h,cpp}`): builds
  CellDressing + RoomDressing headless (381 prop instances), computes each prop's world
  AABB (headless meshBounds x instance x nodeTransform) and audits it against its room's
  box. Wall/floor/ceiling penetration classified **visible** (lands inside a
  non-overlapping foreign room -> FAIL) vs **hidden** (dies inside slab/void -> warn);
  doorway cut spans and canon Overlap throats are exempt; prop-pair interpenetration > 50%
  reported as warnings. Ships 4 negative controls (void-warn / foreign-fail / clean-pass /
  doorway-exempt) proving it can go red.

## THE HEADLINE

> **Every "mystery tan slab" in the facility was one defect.** SM_Door_A is a single
> PENTAGON leaf: its authored silhouette covers only ~72% of its bounding rectangle
> (the top-right triangle is open air — rasterized from the GLB to prove it), and the
> height-fit scale leaves ~10 cm side margins against the 1.6 m wall cut. A CLOSED door
> never sealed its opening — and because the PVS flood correctly treats a closed door
> as opaque, the room behind it was CULLED. The gaps looked straight into unrendered
> void, painted tan by the amber zone fog. Cell halls, wards, lab side-doors — dozens
> of "flat cream slabs" across all seven floors were this one LAW 1 violation.

## DEFECT TABLE

| id | room / where | class | sev | evidence (all read by me) | root cause | status |
|----|--------------|-------|-----|---------------------------|------------|--------|
| D1 | EVERY slab-bearing doorway, F1-F7 (~100 doors) | clip / sightline-into-void | **SEV-1** | BEFORE_wch_doors, BEFORE_wl8_door_closeup, BEFORE_cell_wl8_lookdoor, diag_wch_{albedo,pointterm,normals} vs AFTER_* | SM_Door_A pentagon leaf covers ~72% of the wall cut; closed door leaks a sightline into the PVS-culled neighbour (amber fog void) | **FIXED** `door.{h,cpp}`: opaque backing slab, full cut + bezel, 0.04 m thin, slides with the leaf, hides inside the leaf's 0.13 m body; matte dielectric 1x1 MR texel (reusing the leaf's MR map measured blown-white — L5's mirror case) |
| D2 | 3P mode, any room | clip / camera | SEV-2 | code inspection (boom 3.6 m; halls 3 m wide) | `ThirdPersonView::camera` had NO wall clamp — the orbit boom went through walls, near plane opened the culled void | **FIXED** `app_run.cpp`: static raycast head->camera, pull-in to hit − 0.25 m |
| D3 | Hidden Sub-Level (y=-174) | clip / scenery-through-room + L7-class magenta | SEV-2 | BEFORE_sublevel_pink + diag_sublevel_pink_albedo vs AFTER_sublevel_pink | TWO causes: strata Crystal-Veins band (emissive {0.3,0.1,0.6}) builds violet slabs through/around the room (shaft bore r=16 m vs room 5 m away), AND the room's ceiling lid was collision-only INVISIBLE with no dressing — it stared straight up into the shaft scenery | **FIXED** `strata.h` keep-out is now a LIST + `app_run.cpp` registers every deep room's AABB+1 m; `level_loader.cpp` renders the lid for deep (cy<-50) rooms |
| D4 | Cave System (y=-178) | clip / sky-through-world | SEV-2 | BEFORE_cave_sky vs AFTER_cave_sky | same invisible-lid hole: an underground cave 178 m down showed the analytic SUNSET SKY + stars | **FIXED** (same deep-lid change). Honest note: the cave now reads as a paneled room, not a cave — rock dressing for the deep rooms is a follow-up art pass (P1 in LIGHTING_AUDIT already owns "deep rooms need a real rig") |
| D5 | Main Hall mouth -> Entrance corner | clip / prop-through-wall | SEV-3 | propclip lint (`Barrel.glb ... 1.14 m through the +Z wall, VISIBLE inside 'Entrance'`) | cell_dressing placed the hall clutter drum at `hz+3.2` — the hall is only 5 m deep (z1=47, drum at z=47.7), so the (explodable!) barrel sat through the wall in the Entrance's corner | **FIXED** `cell_dressing.cpp`: drum at `hz+1.1` beside the crate stack, both sink + static branches |
| D6 | Jake's Cell ceiling / West Cell Hall throat | clip / panel-overhang | SEV-3 | propclip lint (SM_Ceiling_A 2x2 raw 4x3 m grid spanning 8 m over a 7 m cell) | east ceiling tile overhung the cell by 1 m and hung visibly in the West Cell Hall airspace | **FIXED** `cell_dressing.cpp`: tiles scaled 0.875 -> X fits exactly; 0.375 m Z end borders read as shadow gaps |
| D7 | wards/decon/server walls (6x Duct Vent), door frames, rooftop consoles | clip / hidden-void penetrations | SEV-4 | propclip lint warnings | vent bodies deliberately recess INTO walls (grate flush); frame jambs sink into floor slabs; rooftop consoles cross an open-roof "ceiling" plane | **ACCEPTED** — reclassified as warnings (not player-visible); the lint distinguishes visible vs hidden penetration so regressions in the visible class still fail |
| D8 | Elevator Shaft room (sweep frame) | n/a | — | sweep/F1_Elevator_Shaft_a | survey camera lands inside the elevator machinery — a sweep artifact, not a defect | N/A |
| D9 | WR-1 (and all cells): magenta lens dots on ceiling kit + frame trim | material / L7-family | SEV-4 | sweep/F1_WR_1_b | ModularSciFi emission keys convert to magenta LED lenses — a **judged decision** in `tools/convert_modular_scifi.py` ("read as magenta LEDs with a hot core. Judged on the render") | **ACCEPTED** (canon converter behavior; not re-litigated here) |
| D10 | door-threshold ramps (67 ramps, all floors) | surface / flat-untextured + Lambert-blown | SEV-3 | AFTER_wl8_door_closeup (bottom wedge), BEFORE_ramp_wl8_door vs AFTER_ramp_wl8_door, AFTER_ramp_wch_hall, AFTER_ramp_bossapproach | `doorwayRamp` wedges wore graybox floorTex + a bright tint on the UNNORMALIZED Lambert prim route (~pi x brighter than every dressed PBR surface beside them — R1's mirror case) while every floor the player sees is the dressing's surface-library deck | **FIXED** e12620a+8fc603b: ramps wear the SAME hh_floor_01a deck set (albedo+normal+mr, deck tint 0.40, matching 2 m tile density) with a matte-MR graybox fallback. *Session-2 correction: the earlier "FIXED (cluster 2) tint+UV" row was WRONG — that change never landed (the AFTER2/AFTER3 ramp evidence was three byte-identical copies of one stale frame); status re-verified against fresh renders this session.* |
| D11 | Elevator Lobby (F1) south view | lighting / near-black frame | SEV-3 | sweep/F1_Elevator_Lobby_b | lobby interior renders near-black from the south end | **DEFERRED** — lighting lane (LIGHTING_AUDIT owns the rig); not a geometry defect: geometry verified present in the a-frame |
| D12 | ward door tell (yellow strip under cell doors) | n/a | — | sweep/F1_WR_1_a | the amber light-under-the-door strip is the AUTHORED rescue-design tell (room_dressing W5-2) | N/A (by design) |
| D13 | every `makeRamp` wedge (ramps + showroom stairs ramps + strata) | geometry / mixed winding parity (LATENT until D10's PBR routing) | **SEV-2** | BEFORE_ramp_wl8_door + BEFORE_ramp_wl8_side (the "bright tan quad" = fog-void through the CULLED ramp top, magenta-tint diagnostic proved the quad was not the ramp) vs AFTER_ramp_wl8_door | `makeRamp` emitted faces in a FIXED vertex order, so geometric winding flips with `dir` and differs per axis — per-face front/back parity was mixed (axis-0 top correct only for dir=+1; riser/sides only for dir=-1; axis-1 inverted). The no-cull emissive route HID this for the mesh's whole life; the backface-culling PBR route exposed it: wrong-parity ramp TOPS vanished from above, opening fog-void windows at thresholds | **FIXED** 8fc603b `mesh_prims.h`: every triangle is oriented against its authored outward vertex normal at build; side-triangle normals made axis-aware (old hardcoded ±X was wrong for axis-0 runs). NEW LINT: `--test-levellint` now builds all four (axis,dir) ramp variants and fails on any backward triangle, with a flipped-tri negative control |
| D14 | W2-E doorway-reveal floor strips (throat floors) | surface / Lambert-blown | SEV-4 | scratchpad down_48 diagnostic (bright tan throat floor beyond the WL-8 plane) | the reveal's interstice floor strip used wallTexA + room tint on the Lambert route — the same D10 brightness class, visible when a stepped door stands open | **FIXED** (this session): both reveal floor strips now wear the ramp's deck set (albedo+normal+mr + deck tint) |

*(F2-F7 survey triage rows appended below after the fork review — see "UPPER FLOORS".)*

## SESSION 2 (2026-07-22) — recovered sweep continuation

The first session died at its limit mid-D10; a second agent's F6/F7 triage notes died
WITH it (never appended here — the "UPPER FLOORS" rows below are re-derived from a fresh
post-fix survey, `sweep2`, 196 frames, regenerable + not committed). Session-2 additions:
D10 status corrected + root-fixed, D13 (makeRamp winding, the real monster under D10),
D14 (reveal strips), D15 (bridge interiors), D16 (Medical Bay doors), and the WINDING
lint gate. F1 re-verified frame-by-frame post-fix: D1 seals hold in every cell/hall frame
read (leaf notch shows dark backing, not fog), D3/D4 deep rooms stay sealed (no sky, no
violet), D5/D6 hold, D12 amber tells present.

| id | room / where | class | sev | evidence | root cause | status |
|----|--------------|-------|-----|----------|------------|--------|
| D15 | gap-bridge corridor interiors (secured-room mouths: Security/Medical/Armory + every GapBridge) | surface / Lambert-blown graybox | SEV-3 | sweep2/F1_Armory_a (glowing cream mouth rectangle); normals debugview from the room cam confirms geometry present in the mouth | bridge floor/walls are raw graybox (floorTex/wallTexA + tint) on the unnormalized Lambert route — the one graybox shell the player looks INTO from dressed rooms | **FIXED** (session 2): bridge floors wear the deck set, bridge walls the hh_wall_01a set, same fallback law as ramps. Residual: the mouth still reads bright-ish from across the room — that residue is the D17 fog-wash class (lighting lane), not graybox |
| D16 | door leaves under bright rigs (Medical Bay worst; every honest key) | surface / blown-white leaves | SEV-3 | BEFORE_medbay_doors vs AFTER_medbay_doors; albedo debugview proved the leaves textured + geometry sound — the ALBEDO VALUE was the defect | SM_Door_A leaf albedo means ~0.78 LINEAR — double the facility value-band ceiling (0.40) every dressed wall is clamped into, so any honest key made the leaves the one blown-white thing in the room | **FIXED** `door.cpp`: hue-preserving value tint 0.51 on the leaf draw (the surface library's own normalization law) |
| D17 | F2-F7 main corridors/halls read as a bright cream floor expanse at eye level | lighting / zone-fog saturation (NOT geometry) | SEV-3 | sweep2/F2_F2_Main_Corridor_a, F3_F3_Specimen_Hall_a, F4_F4_Augmentation_Corridor_a; diagnosis: nadir view shows a DARK dressed tiled floor + guide strip; immune to --norefl; absent in debugview (which skips fog) | the tower-floor zone fog washes accumulate to their cream tint over long eye-level sight lines — the floor is present, dressed, in-band | **DEFERRED to the lighting lane** with the diagnosis written down (the next agent must NOT chase "missing floor"). Session-2 side fix landed anyway: the dressing's CrossLevel skip no longer throws away WHOLE floors (spine lobbies/linked corridors dressed no floor at all); floors now lay AROUND the 3x3 tube mouth, upper room only |
| D18 | F1 Elevator Lobby a-frame | n/a (survey artifact) | — | sweep2 + sweep F1_Elevator_Lobby_a identical grey wash | survey camera lands inside lobby geometry (D8 class); session-1's D11 note "geometry verified present in the a-frame" was WRONG — the a-frame is blank | N/A (sweep artifact; D11 stays with the lighting lane) |

### Session-2 verdict

Geometry + surface state of the canon facility after both sessions: **clean**. Every
defect class in families A (clipping/sightline) and B (surface/material) that was found
is either FIXED with before/after evidence or reclassified with a written diagnosis to
the lane that owns it (lighting/atmosphere: D11, D17-symptom, F5's under-lit floor,
cell-rig hotspots seen through opened doors). The remaining visible uglies on the upper
floors are LIGHT and FOG calibration, not holes, not clips, not graybox. Three lint
gates (levellint + winding, propclip) hold the fixed classes down.

## UPPER FLOORS (session-2 triage, post-fix sweep2 + pre-fix sweep compared)

F2: wards (Keisha/Emily/Aria) + theaters dressed and coherent; corridor/lobby = D17 fog
class. F3: labs/tanks dressed (green goo aprons + biomesh = authored); Specimen Hall =
D17. F4: wing rooms dressed; Augmentation Corridor = D17; the F4.5 Nexus tiers are
near-black BY DESIGN (ZCave fog-only, silhouettes-over-detail). F5: whole floor reads
near-black — the known under-authored floor; geometry present; LIGHTING lane owns it
(same lane as D11). F6: Portal Chamber / organic rooms authored (green portal, biolume);
corridors dark. F7: exec/rooftop rooms read as intended (night rooftop). NO new clipping
or void-leak classes found on F2-F7 in 196 post-fix frames; the D1 backing-slab seal
holds on every floor's doors that were read. Survey-camera artifacts (D8/D18 class):
F1/F2 Elevator Lobby a-frames land inside lobby machinery — sweep tool improvement, not
world defects.

## GATES (session 2 final, all green)
`--test-levellint` PASS (124 rooms/180 doorways, 0 violations, **+ prim-winding gate:
0 backward tris across all four (axis,dir) ramp variants, negative control red-capable**)
· `--test-propclip` PASS (0 visible-class violations, negative controls red-capable) ·
`--test-canonlevel` 16/16 · `--test-canonplay` 10/10 · `--test-level1` 21/21 ·
`--test-basis` 11/11 · `--test-primlight` 9/9 · `--smoketest` default + canonlevel:
exit 0, 0 VUID, allocationCount=0.

## SESSION 3 (2026-07-27, `fix/qa-upper-floors`) — the UPPER FLOORS of the SPIRE

Sessions 1-2 swept the **canon facility** (`--world canonlevel`, the LevelDoc JSON) and
left it lint-green. This session finished the F2-F7 pass on the OTHER upper-floor
tower: the **Spire** (`--world level1`, `app/level1.cpp`) — the code-generated B1->F7
stack whose F2-F7 plates carry the Medical / Genetics / Cybernetics / Drone / Alien /
Executive wings.

### THE HEADLINE — GATE A could not see half the game

`--test-levellint` loaded the canonical JSON and lints **only** what the loader
resolves. The Spire is generated in C++ and was therefore **invisible to the geometric
lint for its whole life** — precisely the failure the x3-level-authoring doctrine's
LAW 5 warns about ("if you must generate geometry in code, generate it INTO a form the
lint can see"). The gate reported PASS while an unwalkable, self-z-fighting mass stood
in the middle of every upper floor.

**NEW: the SPIRE lint block** (`app/level_lint.cpp`) reads the builder's own tables and
the SHARED stairwell layout (`spireStair()`, new in `level1.h` — one source, consumed by
both the builder and the gate, so the lint can never go blind) and applies the doctrine
to the Spire: CONTAIN / OVERLAP / KEEPOUT / DOOR-PROBE (LAW 1), ZFIGHT / PIERCE / WELL
(LAW 2), RISER / TREAD / SLOPE / LANDING / HEAD (LAW 3), REACH, and VALUE (the
surface_library reflectance band). Ships negative controls.

**Objective before -> after** (`--test-levellint`, SPIRE counters):

| probe | before | after |
|-------|--------|-------|
| riser (LAW 3) | 182 | 0 |
| tread (LAW 3) | 162 | 0 |
| landing (LAW 3) | 7 | 0 |
| zfight (LAW 2) | 920 pairs | 0 |
| pierce (LAW 2) | 1423 crossings | 0 |
| well | 1 | 0 |
| value band | 8 | 0 |
| contain / overlap / keepout / door | 0 | 0 |
| **violation lines** | **24** | **0 — PASS** |

| id | room / where | class | sev | evidence | root cause | status |
|----|--------------|-------|-----|----------|------------|--------|
| D19 | the Spire emergency stairwell (x 10-14, z 15) — visible on **every** floor B1-F7 | geometry / doubled + through-solid | **SEV-1** | SPIRE lint: 920 interpenetrating pairs, 1423 slab/lid crossings; AFTER_F2/F4/F6/F7_stairwell_from_plate.png, AFTER_F3_stairwell_apron.png | each "step" was a solid COLUMN from y=0 to its own top and every floor transition reused the SAME 4 m of X, so 182 boxes nested inside one another; no well was ever cut, so the whole mass was driven through every floor slab and ceiling lid on F1-F6 | **FIXED (ROOT)** `level1.cpp`: the stair is now a shared `SpireStair` layout — a switchback of ramp flights + landings inside a **well cut out of every slab and lid it crosses** (`addSlabMinusHoles`, generalised from the B1 trapdoor carve) |
| D20 | same | geometry / illegal height transition | **SEV-1** | SPIRE lint: 182 risers at 0.500 m, 162 treads down to **0.057 m** (the 35 m F4->F5 gap) | the run split a whole floor-to-floor gap into fixed ~0.5 m rises over a fixed 4 m X footprint — at 35 m that is a 70-step vertical wall. The stair was not climbable on ANY floor, and the uncut ceiling lids blocked it even if it had been | **FIXED (ROOT)** rebuilt in the doctrine's LAW 3 vocabulary: <= 30 deg ramp flights (measured 21-27 deg), a level landing every <= 2.5 m of rise, >= 1.9 m head clearance under the return leg, even flight counts so every floor is entered off the same landing through a 1.2 m doorway in the enclosure (LAW 1) |
| D21 | the well vs the env_art GLB overlay, F1-F7 | art / walk-through-able floor | **SEV-1** (introduced by D19's cut, caught + fixed in-session) | probe_f3_well_down.png (pre-fix: a GLB floor panel painted straight across the open shaft) vs AFTER_stair_shaft_lookdown.png | `env_art::tileSurface` tiles GLB floor/ceiling panels across each WHOLE plate and knew nothing about the opening — solid-looking art over a hole with no collision under it | **FIXED (ROOT)** ONE shared rect, `spireWellTileSpan()`: env_art SKIPS every tile the well touches; buildLevel1 lays a graybox APRON over exactly those tiles minus the well (render-only; the slab already carries collision) so the skip leaves no void ring |
| D22 | **every plate deck B1-F7** + the stair's own walking surfaces | surface / value band | SEV-2 | SPIRE-VALUE probe: measured map albedo **0.032 LINEAR**, effective 0.018-0.026 after the per-floor tints, against the interior band 0.08-0.40 | `makeFloorGrateRGBA`'s deck face is sRGB (52,55,62) = a **3.2% reflector, darker than asphalt** — the exact sr_rubberfloor pathology `surface_library.h` was written about — and the per-floor identity tints (0.55-0.78) only ever darken it further. A light lands on the Spire's floors and nothing comes back | **FIXED** a neutral hue-preserving lift on the MAP at generation time (`level1DeckMapLift()`, kept in sRGB bytes so no baseColor exceeds 1): map 0.032 -> **0.223**, every plate deck now 0.12-0.18 in band. Applied to the B1 trapdoor panels too, which are authored to sit ~1.3x brighter than the deck (`secret_room.cpp`). **Scope honesty:** the plate decks are only DRAWN when the GLB floor art is absent (`artMask.floors`); with art on, this fix shows on the stairwell surfaces, the aprons and the hatch |

### Session-3 verdict + what is NOT claimed

Geometry state of the Spire's F2-F7 after this session: the wing-room tables
(CONTAIN / OVERLAP / KEEPOUT / DOOR-PROBE) were **already clean and were verified so,
not assumed** — every defect found was in the stairwell, the well's art contract, and
the deck value. All are root fixes: one layout, one shared rect, one map lift, each
fixing all seven floors at once rather than per-instance.

Headless gates prove no-crash / no-leak / lint-clean. They do NOT prove it looks right.
Read `docs/screenshots/qa_upperfloors/` — the stairwell reads as a solid panelled tower
with a doorway on each floor and a legible ramp inside, no void, no shimmer. The one
thing that wants the owner's eye is the **graybox apron**: it is a visibly different
(flatter) deck material from the GLB floor around the stairwell base. It is correct and
seamless, but it is graybox next to art.

## GATES (session 3)
`--test-levellint` PASS (**incl. the new SPIRE block, 24 -> 0 violations**) ·
`--test-propclip` PASS · `--test-level1` 21/21 · `--test-canonlevel` 16/16 ·
`--test-canonplay` 12/12 · `--test-spiretop` 20/20 · `--test-spiremid` 27/27 ·
`--test-nexus` 11/11 · `--test-basis` 11/11 · `--test-primlight` 9/9 ·
`--test-secretroom` 8/8 · `--test-hatch` 8/8 · `--test-elevator` 6/6 ·
`--test-wingdressing` 6/6 · `--test-sublevels` 22/22 · `--test-goldenpath` 9/9 ·
`--test-strata` 10/10 · `--test-thirdperson` 19/19.

## GAMMA-RECAL CLOSURE (2026-07-25, fix/gamma-recal)

The deferred lighting-lane rows above are closed by the gamma recalibration
(the sRGB swapchain fix 5951890b exposed every value tuned on the bent curve;
retuned from the honest baseline — see docs/screenshots/gamma_recal/):

| row | verdict |
|-----|---------|
| D17 F2-F7 cream wash | **CURED** — the corridor-zone recal (near-black warm-neutral fog at real extinction + dense warm pendant rhythm) removed the cream accumulation entirely. Verified: F2 Main Corridor / F3 Specimen Hall / F4 Augmentation Corridor re-shot (zones_before//zones_after) — dark moody runs, floor-identity strips carrying each floor, zero cream. Corridor b-frames run 30-69% void by design (atmosphere, not soup; the a-frames show the lit run). |
| F5 under-lit floor (+ D11 class) | **RE-JUDGED under the honest curve: largely cured for free.** F5 Main Corridor now rides the corridor recipe (amber hazard band + guide strip read; dim but navigable); Drone Bay Alpha mean ~18 dim-moody with its key. No bespoke F5 work done this pass; the floor remains the least-authored (LIGHTING_AUDIT still owns 'deep rooms need a real rig'). |
| D8/D18 survey-cam artifacts | still artifacts (F2 corridor a-frame lands in the CrossLevel tube mouth — same class). |

