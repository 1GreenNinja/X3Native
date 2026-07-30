# iPAD QUESTION (Tim 2026-07-30): native M5 iPad build = FEASIBLE, weeks-class
# future lane: MoltenVK (Vulkan->Metal, mature), Jolt+llama.cpp run great on
# Apple silicon (llama METAL backend = faster citizens), GLFW must be replaced
# (iOS windowing + TOUCH control design), RT tier-gates off gracefully. Sequence
# AFTER perf hunt. NEAR-TERM ANSWER: Moonlight/Sunshine streaming from the 5090
# box = Echo Harbor on the iPad tonight, zero port. Fleet GPU correction: NO
# 1080 Tis anywhere (13700K + Predator both regunned, GPUs unknown) — Michigan
# 1060 box is the only min-spec bench.

# MEMORY LEAK HUNT (Tim 2026-07-30, Lane 6 first move): (1) 30-60min soak logging
# RSS + VRAM (vmaCalculateStatistics) per 60s — monotonic growth = leak; (2)
# Vulkan validation layer shutdown audit; (3) re-check DELIBERATE leaks under
# M-C evictions: lampScene/StreetLights per-rebuild, EnvArt shared textures,
# model-cache refs; (4) crowd/npc re-attach cycles must not grow pools.
# ART_WAVE_AUDIT.md = Lane 5 burn-down, top-down, blocks first.

# ALSO UNDER THE NO-REFILING RULE (Tim, laughing, 2026-07-30): (a) BOAT LANES
# still cross dry land (galleon through the village) — depth-gate routes vs
# active hf, filed twice/built zero, fix WITH #34's wave. (b) HOUSES ON WATER
# on the fjord world: authored-position content ignores the changed coastline —
# fjord prerequisite: a boot-time COASTLINE AUDIT (every authored placement
# re-validates vs active hf; relocate or skip+log). Same class as the
# gorge-arcing highway.

# ★★ STANDING ACCOUNTABILITY (Tim, 2026-07-30): task #34 CITY BLOCKS/STREETS was
# filed FOUR times (spec, agenda, punchlists, lane 5) and built ZERO times —
# named 'well-documented procrastination'. RULE: #34 is Lane 5's FIRST commit,
# before any other art item. Scope: crown street grid + blocks, towers/houses
# re-seated on street-aligned lots (RoadGraph tangents), avenues tied to the
# ring via interchanges. ALSO: freeway STRUCTURAL pass (box-girder underdeck,
# pier caps, haunches, CA curve-follow supports, larger texture tiles) — the
# piers are bare box columns; V7 did surface only. No more refiling either one.

# LANE 7 ORDERS — TIM'S iPAD ANNOTATIONS (2026-07-30, on fjord captures):
# 1. BLUE: GOLDEN GATE-STYLE SUSPENSION BRIDGE spanning the gorge mouth (city
#    side) — towers + catenary main cables + hangers + deck tied into the road
#    graph. New module (echo_bridge or a Bridge edge class in EchoRoads).
# 2. YELLOW: EXPAND THE CLIFF BLUFF — the channel headland grows per his traced
#    footprint (carve v4 target zone: the isolated bluff + adjoining walls).
# 3. RED: SEA-CAVE WATERWAY -> LARGE UNDERGROUND CAVERN behind the cliff face.
#    Heightfield is 2.5D — cavern = authored cave geometry (engine has
#    act2_caves.cpp + cave kits) + carved entrance notch + interior water plane;
#    perfect streaming Lane-A cell + future hidden-mine/smuggler set piece.
# 4. WAVE ARTIFACT on fjord world: honest answer — the white-sheet fixes (ring
#    repaint + trough margin) landed on island_mesa's bake; assets/island_fjord
#    is an OLDER bake without WATER_V2. Fjord needs a WATER_V2 rebake; ALSO the
#    surface-level patch-vs-ring contrast at eye<140m remains open everywhere
#    (Lane 5 water tier 2).

# WRITER SEAT (2026-07-30): a NEW session is live on LANES 1-4 — it now owns
# host_echotropolis.cpp + CMakeLists + cmake builds + exe launches. The elder
# window (this file's last author) has STOOD DOWN from all repo writes.
# New session: read SESSION_LANES.md + your punchlists; honor capture-review,
# tasklist-check for Tim's exe, and per-milestone commits. Lanes 5-7 unclaimed.

# Handoff

## LANES 1-4 EXECUTED (2026-07-30, this window — commits 6d97692a..6711a20f, PUSHED)
Tribunal mandate landed, all "wire what exists": (1) WD2 STACK LIVE — npcLife.build
got &hax (scan-cards lit), 48 hackables (7 cams/4 jboxes/4 ATMs/4 signals/6 cars/23
citizens), H=nethack vision, aim+E hacks, K=skill tree (console-modal), XP from
hacks/DODOGs/driving, heat=new AlertSystem + karma=TimelineState + junction hack =
StreetLights::killNear 45s blackout; RPG persists at echotropolis.rpg.txt. (2) V7.1
per-gate on-ramps — zigzag law PASS (gate 1's V7 bow hooked post-smooth 11.6->8.3;
20-deg bow to 200m merge passes; NO local metric predicts the law — its pinned-end
smooth + central-diff tangents must be measured by running it). (3) LANE 1 TRAFFIC —
22 cars route the WHOLE RoadGraph (seeded multi-edge walks, positional 10m supernode
clustering — RoadNodes share no indices!), 10s signal cycle holds cars at junctions,
TrafficSignal hack = real 20s gridlock. (4) LANE 2 — crown drag/plaza + harbor lamp
rows (35->51; crown seats FLAT — the 3x3 max probe floated them), 6 SB_* boards +
billboard on the drag + 2 harbor. ✅ NIGHT-GROUND SOLVED (3fe89811, same window):
root cause was GLASS IN THE TLAS — the lamps' fake-volumetric glow cones entered
the RT accel structure as solid occluders, so every point-shadow ray from the
ground hit its own lamp's cone shell (vis=0) and the whole city floor read black.
One-line fix (kFlagGlass filter in the TLAS instance loop, vk_passes.cpp) — drag
+ plaza now read in warm pools, facades catch lamp light. Debug lane r_debugview 7
(point RT shadows forced lit) kept — it's the A/B that cracked it. Pool discs were
NEVER broken (foreshortened slivers at street level, by design; top-down proof).
⚠ lamps were tuned while shadow-dead — night may read HOT; ECHO_LAMP_INT_MUL is
Tim's knob. hf/PNG/mesh all AGREE (python probe) — seat everything via hf freely. (5) LANE 4 v1 — NpcLifeConfig::
leisureMagnet sends Electrician/Courier/Gardener/OffShiftDrone to the noodle bar on
schedule; [E] ENTER/LEAVE door portals (condo lobby + harbor shop). (6) NFS LAYER —
PerfShop OPEN at (-130,726): WorldCars::liveCar() accessor, treasury=wallet, lift-pad
flow, UP/DOWN/ENTER browse+buy, LEFT/RIGHT boost, P dyno, R repair, N nitrous.
TIM LIVE-VERIFY: H/E/K feel, marker/card visuals, bar patrons at leisure hours, door
feel, shop flow + a bought upgrade on a lap, signal-cycle feel (only 1 junction
supernode found — expected ~3, check clustering radius). Drift tuning deferred (needs
hands). Screenshot path exits BEFORE worldCars/npcLife/shop build — live-mode only.

## THE PILLAR BOARD (Tim: "keep track of these pillars!!!!")
| Pillar | Task | State |
|---|---|---|
| Drivable cars (GTA/WD feel) | #26 | world_cars.h stack complete, UNWIRED — next big move |
| Walkable interiors | #27 | waits on streaming Lane A (M-C) |
| Watch-Dogs citizen depth | #28 | strongest pillar (LLM talk live); deepening queued |
| Roads | #29 DONE | v6: welded, zigzag LAW (escalating smoother), junctions, collision |
| GTA5-quality housing | #34 | KIT FOUND: Mega Open World City Pack (109 GLBs: fuel station, shops, civic), House On A Hill (house+fence+garden, matches cliff family), Seaside Town; plan = street-aligned lots via RoadGraph tangents |
| Desert highway frontier | #33 | vision logged; Fuel_Station + JunkYard + signboards found; needs big terrain frame |
| Terrain/fjord | #30 | fjord island BAKED + staged assets/island_fjord; ECHO_ISLAND_DIR swap test pending |

## State (echotropolis @ c2539ed5, 2026-07-27)
Tasks 10-23 ALL DONE (see git log). #24 streaming: M-A + M-B COMMITTED (streamer ticks,
vista rule, draw gate; ECHO_STREAM=0 rollback); M-C evictions + M-D fast-boot remain.
v6 roads: law PASS (escalation before drop saved the ring), weld 40% verts, water wave 1
(patch altitude-gate >140m killed white square; trough margin killed shards). Sky day = 2h
default (Tim: 20-30min/phase), HUD clock reads sky. STABILITY: Tim's M-B session exit 139
(segfault) after long play — eviction path suspect, first incident, watch for recurrence.
Tim's active front: #25 art wave, #26 cars, #31 perf (35ms NOT in districts/woodlands/crown
— proven by region bisect; suspects: 72 skinned rigs/TLAS, island GLB, ocean, post).

## WAVE-1 LANDED (2026-07-29 late): V7 surfaces + interiors are IN (see last commit).
OPEN: (a) WATER V2 Sonnet agent still running when window closed — its report + files
(echo_water.*, island_to_glb WATER_V2 flag) need INTEGRATION: CMake, host hookups
(echoShipPose in poseBoat, splashes via submitParticles, swell preset), bake w/ WATER_V2=1
+ ring repaint, restage island_mesa. (b) V7.1: one CA on-ramp law-dropped (11.6 deg/m
hook) — retune the hermite bow so both gates have on-ramps. (c) Tim live-verify list:
cars E-enter + C views, vendor buy prompts (Tess $8 DODOG), condo shells by day (floaters
gone?), night glow heads, noodle bar corner. (d) M-C makes interior cells actually gate.

## THE NEXT WINDOW = "MATERIALS + INTERIORS" (Tim's orders 2026-07-29)
1. #35 ROAD V7 SURFACE PASS: textures (SD-gen tiles on 5090), shoulders, crosswalks,
   debris scatter, CA sweeping ramps, lamp emissive heads.
2. #36 WATER V2 LIVING BAY: wet-sand band + foam line + ring repaint (one bake), swell
   tuning, SHIPS ROCK on the Gerstner fn, bow/shore splash particles.
3. #27 INTERIORS (Tim: ALL enterable buildings textured, drawn when near/entered,
   streaming style): Lane-A sub-regions (~60m radii) — the machinery is wired+empty by
   design; condo shells fix doubles as first interiors; vendors -> textured stalls with
   buy-interaction. 4. Condo shells (#25A) if not folded into 3. 5. Tim's live verdicts
   from the cars/views/noon test drive.

## Next (Tim's priority order)
1. #26 CARS (feel transformer; road graph ready, lane data exposed). 2. #25 art wave
(houses->#34 first per Tim "GTA5 houses", trees roster, night fog amber, emissive day-gate,
mine rebuild, black slab, legacy street decks #32 = the REAL remaining zigzag). 3. #31 perf.
4. M-C/M-D. 5. #30 fjord swap captures. 6. #33 desert.

## Context
- 1M context per window (Tim's note); compaction-safe via this file + commits.
- Capture-review law: NOTHING visual reaches Tim unreviewed. Single host writer. Check
  tasklist for Tim's exe before builds/launches. claude.exe old — no Opus 5; Fable forks
  burn credits fast (spend limit hit twice); prefer main-loop work or tight Sonnet briefs.
- Meshy ~3151cr one-lane. Verification kit: scripts/echo_stream_ab.ps1 + ECHO_SKIP_REGIONS
  + ECHO_PLAYAS_DEMO. Legacy exe path = Tim's Desktop shortcut -> build/bin/Release.
