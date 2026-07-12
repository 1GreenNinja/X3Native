# X3Native — Roadmap Mine (source digest for the HTML master roadmap)
Generated 2026-07-11 from all 190 remote refs + main-line commit bodies + design docs + the live session queue. Integration tip = `origin/feat/intro-cockpit`.

## (1) BRANCH LEDGER

### STRANDED — REAL UNIQUE WORK (not yet folded; the integration backlog)
| branch | date | ahead | unique content |
|---|---|---|---|
| feat/rifthub-aaa | 07-11 | 15 | Stargate portals: true torus rings, liquid fresnel membrane, kawoosh surge, 3D gate audio (supersedes rifthub-portal-visuals) |
| feat/rifthub-portal-visuals | 07-05 | 103 | earlier Stargate portal rework + --capture-rifthub GIF (older than rifthub-aaa) |
| feat/dialog-live | 07-02 | 54 | **wires Aria/Keisha/Emily to authored chat trees + surfaces the 1278 code on the cell terminal → ties rescue beat to hatch** (directly feeds current rescue work) |
| feat/npc-characters | 07-03 | 58 | animated citizen BODY pipeline (rig + anim states + ragdoll interface) |
| feat/living-city / city-aaa / city-fix / city-uplift / neon-city | 07-03 | 49–60 | living city: WD2 hack markers, wet-street SSR, lens polish, cool fill, cable-wire fix |
| feat/terrain-aaa / world-terrain | 07-02 | 45–47 | AAA ground: biome splat + detail-normals + emissive, FREEWAY asphalt ribbon, landmarks |
| feat/undersea-art | 06-09 | 143 | Act-4 undersea station unified + lit + marine-snow atmosphere |
| feat/gpu-llm | 07-01 | 8 | CUDA inference backend for VIGIL + Apache-7B upgrade + zero-stutter benchmark |
| feat/weapons-overhaul | 07-01 | 10 | lightning arc-tendril impact, crystal battery pickups, per-weapon firing FX audit |
| feat/model-test | 07-07 | 144 | --world modeltest bench (Snake's, off honor-fable) |
| feat/m7-postfx-advanced | 06-05 | 137 | **SCAFFOLD/docs only** — advanced cinematic post-FX milestone (no impl) |
| feat/m8-imgui-devtools | 06-05 | 137 | **SCAFFOLD/docs only** — Dear ImGui dev-tools milestone (no impl) |
| feat/gen-image-workflow | 06-14 | 156 | Slick WEB infra (comfy image pipeline, settings/login) — NOT game engine |
| feat/jake-ship / m10-ship / cockpit-vattalus / ship-interior-firefly | 05-31–06-05 | 112–124 | older ship-art lineages (mostly superseded by folded S5/S6/S11 + intro cockpit — verify before folding) |
| feat/coop-companion-merge | 05-30 | 68 | companion↔faction coop merge (task #26 adapter deferred) |
| feat/portal-hub-rebased / -polished / portal-hub | 05-30–06-08 | 10–146 | DJBooth portal hub (partly folded via act2_caves; rifthub-aaa is the newer line) |

### FOLDED-HUSK (0 ahead, or content verified folded this campaign — safe to archive)
All 0-ahead refs + the space-combat lanes now LIVE on intro-cockpit: ship-interior, ship-windows, ship-ai, ship-damage, ship-targeting, ship-art, ship-repair, space-layer, space-env, space-lod, space-pilot, space-stars, space-wave1-integrated, eva-spacewalk, atmo-descent, wormhole-transit, wormhole-vfx, tractor-beam, decloak-vfx; weapon-textures/-variety/-real-models/-sfx-fx/-damage-types/-grip-tune/explosive-weapon-row; holo-glass-platform; floors2-7-dims; all the *-fire-damage-types; the refactor/split-* and integration/* history lines; every worktree-agent-* and backup/*. ~120 refs — archivable.

### ACTIVE / DOCS / UNKNOWN
- ACTIVE: feat/intro-cockpit (tip, 07-11), rescue-room polish (in-flight local), **echotropolis (07-11, 13 ahead — NEW, unclassified; needs a look)**.
- DOCS-ONLY: docs/studio-doctrine, docs/public-fleetcommand, docs/narrative-*, docs/canon-aliens-*, all feat/note-to-* .

## (2) OPEN ITEMS (actionable)

### INTRO
- [INTRO] cine beats (cine.flight/reveal/charge) need Tim's phase-by-phase eyes-on read — b1ba6d2 — S — todo
- [INTRO] tractor beam → wire into the intro capital-ship capture (real deployment; only test-host proven) — b2c1ff1 — M — todo

### TOWER (floors 1–7)
- [TOWER] spire interior floors BRIGHTNESS — identity ladder done but floors read dim (LD bar) — 680ae3c/LD — M — todo
- [TOWER] feed wingFloorLights() into interactive Spire light seams (lights gameplay not just captures) — ed76165 — S — todo
- [TOWER] F5 hangar floorTile grunge scale balance (~4.5m) — corduroy solved at 6.0, grunge scaled up — ee13ae6 — S — todo
- [TOWER] detention ward-cell amber verified by recipe only (no bespoke ward-cell capture) — ee13ae6 — S — todo

### RESCUE (F2 medical)
- [RESCUE] room polish IN FLIGHT: dark-glass rounded screen swap + exam lighting + floor contact-shadow artifact — session — M — in-flight
- [RESCUE] rescue-timer reconciliation: point RescueSystem placement at ward markers + suppress double-body (live victim vs staged captive) — 5fbd0c8 HOOK — M — todo
- [RESCUE] Room B magnetic-lock MECHANIC (red seal is visual-only) — 5fbd0c8 HOOK — M — todo
- [RESCUE] BodyContact → rescue integration (posed captives solved onto bed + mattress indentation, one call site) — 694ab49 HOOK — S — todo
- [RESCUE] wire captives to authored chat trees + the 1278 code (EXISTS on feat/dialog-live — fold it) — dialog-live — M — todo
- [RESCUE] crisper supine pose re-bake (Blender headless) — optional polish — 564e664 — S — todo

### DESCENT + CAVES
- [DESCENT] camera ROLL for overbanks — needs renderer view-up axis (setCamera is yaw/pitch only) — 551c0f2 — M — todo
- [DESCENT] cavern irregular rock forms once cv_rock_wet lands (forged, shelved) — 551c0f2 — S — todo
- [DESCENT] Club-1127 bass-bleed audio hook (loopable positional low-pass at cave floor) — 551c0f2 — S — todo
- [CAVES] concrete→shotcrete→rock transition grammar build (sets forged, geometry not) — TEXTURE_STRATEGY §2 — L — todo
- [CAVES] wire the 9 shelved cave/descent surface sets into geometry — ee13ae6 — M — todo

### SHIP + SPACE
- [SPACE] valley host has NO hero ship to rim-light (content gap, not a bug) — 680ae3c — S — todo
- [SPACE] space capital hull under-detailed — panel/greeble pass — art-audit — M — todo
- [SPACE] ocean sun-spec clips to white blobs — tighten roughness/exposure — art-audit — S — todo

### PARK (Cedar Point 2000-2002 replica — LIVE functioning park)
- [PARK] track tech: lift-hill chain, multi-car trains, block/station stop, queue+station buildings — CP_PLAN — L — todo
- [PARK] Phase A Blue Streak (tech prover) → Phase B Magnum → Phase C Millennium Force → Phase D Raptor — CP_PLAN — XL — todo
- [PARK] live-park scope: crowds (engine crowd system), ride-ops, queues, NIGHT lighting everywhere — CP_PLAN — XL — todo
- [PARK] park layout graybox (peninsula/midways/beach/Hotel Breakers/causeway) — CP_PLAN — L — todo
- [PARK] per-ride material stories + period signage (SD3.5 forge) — CP_PLAN — M — todo

### WORLD / EXTERIOR
- [WORLD] kill the flat kelly-green ground everywhere (streamed city + water-edge land) — top immersion-breaker — art-audit — M — todo
- [WORLD] terrain tiling: triplanar + slope-blend + macro-variation (EXISTS on terrain-aaa — fold) — terrain-aaa — M — todo
- [WORLD] living city: fold the city-aaa/npc-characters line (animated citizens, wet streets) — city-aaa — L — todo
- [WORLD] rifthub Stargate portals: fold rifthub-aaa — rifthub-aaa — M — todo
- [WORLD] Act-2 desert/caves have NO player-eye capture path (headless self-test only) — LD tooling gap — M — todo
- [WORLD] undersea Act-4: fold undersea-art + StarForge saucer command center (Hunyuan mesh pending) — undersea-art — L — todo
- [WORLD] surface facility white-concrete banding (spec is concrete+glass, currently all-black-glass) — art-audit — S — todo
- [WORLD] per-zone depth FOG game-wide (cheapest single "one painting" lever) — art-audit — M — todo
- [WORLD] elevator OLED wall panels sit DARK — gate content onto them — art-audit — S — todo
- [WORLD] Club 1127: dance-floor emissive −30% + DJ-rig haze/volumetric beams + perimeter wall-wash — art-audit — M — todo

### ENGINE
- [ENGINE] BodyContact per-frame on live characters (companions/NPCs/captive struggle) — 694ab49 — M — future
- [ENGINE] renderer camera view-up axis (unblocks coaster/overbank roll) — 551c0f2 — M — todo
- [ENGINE] GPU/CUDA LLM inference for VIGIL + 7B (EXISTS on gpu-llm — fold) — gpu-llm — M — todo
- [ENGINE] extract --world showcase blocks out of main.cpp (stack pressure) — task #20 — M — todo
- [ENGINE] multi-process Vulkan contention Bug 2: operational rule vs code fix (decision) — task #18 — M — blocked-on-Tim

### ART / PIPELINE
- [ART] lab-wall mint desaturate to grey-green (art-audit residual) — art-audit — S — todo
- [ART] F4 grate rust cool/desaturate vs cyan (partly done in 2A) — art-audit — S — todo
- [ART] dark-glass rounded screen = engine-wide standard (never flat bright quads) — session — S — policy
- [ART] SM_Hospital_Bed manifest size-mismatch WARN vs asset store (benign) — session — S — todo
- [PIPELINE] Blender headless pose-bake pipeline is proven (captives) — reusable for posed NPCs — 564e664 — note

### FLEET / OPS
- [FLEET] i9/oglaptop audit STILL unanswered (5+ pings) — session — — blocked-on-oglaptop
- [FLEET] i5000 18 staged fleet-infra files: commit-vs-gitignore ruling — session — S — blocked-on-Tim
- [FLEET] 14900K burn-in before re-taking lanes (RMA candidate; microcode 0x12B+ check) — session — — todo
- [FLEET] ~120 husk branches archivable; ~15 stranded-real to fold — this mine — L — todo
- [FLEET] echotropolis branch (07-11, 13 ahead) unclassified — needs a look — this mine — S — todo

## (3) DONE THIS CAMPAIGN (crossed off, on feat/intro-cockpit)
- Flyable intro cold-open (starfield, clear canopy glass, 1P cockpit, mouse-look, nose-follow, target brackets, mirror fix) — c043fe8/7efcc9d/d8210fd/b1ba6d2
- The everything-build merge (elevator/club/canon wave + 14900K AAA weapon textures) — 124f523
- S5/S6 fold: walkable ship interior + true-portal windows + REAL kit interior + SD3.5 hull panels — 72dee0d/03214f9/fb803a4
- Black-glass VIGIL terminal port — 311d0d3
- Floors 2–7 dressing + 18-set forged surface library + black-prop fix + amber ladder + hospital bed — 14e45c6/ed76165/1622584/45e1c46/921c0f2/ee13ae6
- Studio review bench (level-designer + art-director agents) + full-game LD & art audits — 0d0a27b
- Spire floor identity hue ladder + showroom/elevator/surface lighting + cell crates + space rim — 680ae3c
- The Descent Ride (coaster-grade B1→−178m) + generic track layer — 551c0f2
- Space-combat fold: wormhole transit + crystal-matrix VFX + tractor beam (14900K lanes absorbed) — cf3dd37
- TV-realism tractor beam (translucent, pulsing) — b2c1ff1
- NEW ENGINE FEATURE: BodyContact (no-clip bone solver + soft-surface indentation) — 694ab49
- F2 rescue rooms (Aria/Keisha/Emily, posed, oriented) — 5fbd0c8/564e664/8058578
- Cedar Point 2000-2002 park master plan + world −700m depth ruling — f792cdb/b6b6be2
- Tooling: --shot-cam negative-coord fix (frees canned-cam worlds) — 694ab49

## (4) RULINGS NEEDED FROM TIM
- PARK: placement RULED (live park); SEASON still open — 2000-default vs 2002 toggle vs "25-years-later" variant.
- DEPTH: −700m bottom ruled; strata assignments −350→−700 (mines / Salvari ruins / thermal vents / true-bottom reveal) unassigned — TEXTURE_STRATEGY §3.4.
- NARRATIVE: EFLZ_MASTER_PLAN open decisions — per-act level counts to total 100 (Act1 ~12 / Act2 ~27 / Act3 ~40 / Act4 ~25?).
- FLEET: i5000 staged-infra commit-vs-ignore; Bug-2 Vulkan-contention rule-vs-code.
- INTEGRATION: approve the fold order for the ~15 stranded-real branches (dialog-live + terrain-aaa + city/npc + rifthub-aaa + undersea-art + gpu-llm + weapons-overhaul are the high-value ones).
