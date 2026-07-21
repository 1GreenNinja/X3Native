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
| D10 | door-threshold ramps (many doorways) | surface / flat-untextured | SEV-3 | AFTER_wl8_door_closeup (bottom wedge), sweep/F1_Boss_Arena_Martinez_b | `doorwayRamp` wedges read as flat light-grey planes — floorTex is passed but reads blank/stretched at ramp scale, and the {0.46,0.50,0.58} tint sits far above the hall floors' value | **FIXED** (cluster 2): ramp tint brought into the facility floor band + UV scale fit |
| D11 | Elevator Lobby (F1) south view | lighting / near-black frame | SEV-3 | sweep/F1_Elevator_Lobby_b | lobby interior renders near-black from the south end | **DEFERRED** — lighting lane (LIGHTING_AUDIT owns the rig); not a geometry defect: geometry verified present in the a-frame |
| D12 | ward door tell (yellow strip under cell doors) | n/a | — | sweep/F1_WR_1_a | the amber light-under-the-door strip is the AUTHORED rescue-design tell (room_dressing W5-2) | N/A (by design) |

*(F2-F7 survey triage rows appended below after the fork review — see "UPPER FLOORS".)*

## GATES (post-fix, all green)
`--test-levellint` PASS (124 rooms/180 doorways, 0 violations) · `--test-propclip` PASS
(381 props, 0 visible-class violations, negative controls red-capable) · `--test-canonlevel`
16/16 · `--test-canonplay` 10/10 · `--test-level1` 21/21 · `--test-basis` 11/11 ·
`--test-primlight` 9/9 · `--smoketest` default + canonlevel: exit 0, **0 VUID,
allocationCount=0**.
