# Handoff

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
