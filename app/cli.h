#pragma once
// ============================================================================
// cli — CliOptions (all CLI flag state) + parseCli() (the arg-parse loop), lifted
// VERBATIM out of main() (#28 deep split, Phase D). The struct body is the exact
// flag-declaration block main() had (same names + defaults); parseCli runs the
// exact arg loop, reaching each flag through `o.`. main() shrinks to: parse ->
// dispatch -> run.
// ============================================================================
#include <string>
#include <vector>
#include <utility>
#include <cstdint>
#include <climits>

#include "leveldoc_world.h"   // x3::game::defaultLevelDocPath() (docWorldPath default)

namespace x3 { namespace apphost {

struct CliOptions {
    bool smoketest = false, testAsset = false, testConsole = false, testPhysics = false,
         testGltf = false, testPlayer = false, testInteract = false, testPickup = false,
         testPhysprops = false, testRagdoll = false, testRagdollSkin = false, testEditor = false,
         testBlockout = false,
         testBarrels = false, testGlass = false, testHoloterm = false, testLlm = false, testEcs = false, testEcsRender = false,
         testFrustumCull = false,
         testCombat = false, testAudio = false, testAcoustics = false, testLevel1 = false, testJobs = false,
         testPhase2a = false, testPhase2b = false, testAnim = false, testTerrain = false,
         testStreaming = false, testWorldStream = false, testWorldMap = false, testAi = false, testDoorCode = false, testElevator = false,
         testElevatorFsm = false,
         testTerrainPlace = false, testNet = false, testRescue = false, testDestruction = false,
         testNav = false, testWeapons = false, testVehicle = false, testVehParts = false,
         testFootIk = false,
         testScript = false,
         testNetSync = false, testNetInterp = false, testNetPredict = false, testNpcTalk = false,
         testChatTree = false,   // --test-chattree: x3.chattree/1 parse/validate + the lena walk
         testMission = false,    // --test-mission: x3.mission/1 docs + runner + the Level-1 equivalence walk
         testDeathRagdoll = false, testCanonLevel = false, testLevelLint = false, testCanonPlay = false,
         testGoldenPath = false,   // --test-goldenpath: W5-3 endgame spine (cell -> Sarah -> Helipad win)
         testThirdPerson = false, testHatchCode = false,
         // --test-hatch: END-TO-END secret-hatch chain (terminal_code fire ->
         // boot-loaded secret_room.lua -> registerGameBindings openTrapdoor ->
         // REAL Level1Game DoorSystem hatch opens + objective line set; plus the
         // keypad submit link via the real HoloTerminal). See runHatchChainSelfTest.
         testHatch = false,
         testEcology = false, testCrowd = false, testAlert = false;
    // --test-loader (EDITOR LevelDoc data-driven loader): author a doc in memory ->
    // save -> LOAD through the real loader -> assert the built world matches; then
    // modify + hot-reload -> assert the delta applied and the create/destroy ledgers
    // balance to zero (the no-leak gate). Additive flag.
    bool        testLoader = false;
    // --world fromdoc [path]: boot the engine DIRECTLY into a LevelDoc JSON (the
    // editor's save format) — playable (walk/collide/shoot) + HOT-RELOADABLE (mtime
    // poll / `level_reload` console cmd). Default path == the editor's File>Save
    // target so the edit -> save -> reload loop closes out of the box.
    std::string docWorldPath = x3::game::defaultLevelDocPath();
    // --screenshot-loader [path.png]: headless proof — build the sample LevelDoc
    // through the REAL loader on the live device, render the room, capture a PNG.
    bool        loaderShot = false;
    std::string loaderShotPath = "build/proof/loader_room.png";
    // --set <cvar> <value> pairs, applied right after console cvar registration.
    std::vector<std::pair<std::string, std::string>> cliCVars;
    // --screenshot-perfshop [dir]: headless PERFORMANCE-SHOP proofs — boot the
    // drive world, build the shop, set the car on the lift, capture the bay
    // (car on lift + neon sign), the PARTS terminal, and the DYNO mid-pull into
    // <dir>/perfshop_{bay,parts,dyno}.png. Implies --world drive.
    bool        perfshopShot = false;
    std::string perfshopShotDir = "docs/screenshots/perfshop";
    // --screenshot-ecology [path]: LIVING-WORLD proof shot. Builds the valley
    // open biome + the ambient ecology, stages the predator-strike moment at the
    // grazer herd, settles, and captures from a herd vantage. Implies
    // --world valley + headless.
    bool        ecologyShot = false;
    std::string ecologyShotPath = "docs/screenshots/livingworld/ecology_herd_predator.png";
    // --screenshot-crowd [path]: LIVING-WORLD proof shot #2 — the Club 1127 dance
    // floor crowd (idle clusters bobbing under the blacklights). Implies
    // --world club + headless.
    bool        crowdShot = false;
    std::string crowdShotPath = "docs/screenshots/livingworld/club_crowd.png";
    // --screenshot-alert [path]: LIVING-WORLD proof shot #3 — Level 1 under a
    // forced ALERT 3 LOCKDOWN (zone doors locked, red-shifted lights, the alert
    // HUD pips + LOCKDOWN banner + pulsing red frame). Rides the --screenshot
    // production-HUD corridor path.
    bool        alertShot = false;
    std::string alertShotPath = "docs/screenshots/livingworld/alert3_lockdown.png";
    // --test-rt (hardware ray-tracing RT AO): runs the headless smoketest render
    // path with r_rtao forced ON so the BLAS/TLAS build + ray-query AO compute +
    // apply passes are exercised under Vulkan validation on an RT-capable device.
    bool        testRt = false;
    bool        testReflections = false;   // --test-reflections: SSR + ray-query refl under validation
    bool        noRefl = false;            // --norefl: reflections off, TAA on (refl A/B isolate)
    // --test-ddgi (DDGI probe-grid GI): headless smoketest with r_ddgi forced ON so
    // the BLAS/TLAS build + ddgi_rays/ddgi_update compute + mesh.frag sampling run
    // under Vulkan validation. No-op on non-RT / no-position-fetch devices.
    bool        testDdgi = false;
    bool        ddgiForce = false;         // --ddgi: force r_ddgi 1 (screenshot/showroom A/B)
    // --test-bestiary (bestiary pass): the data-driven enemy roster. Additive flag.
    bool        testBestiary = false;
    // --test-bosses (Act-1 bosses, Wave 1): the 5 mid-boss defs + the multi-pod
    // machine + the scripted pre-fight hook + the Martinez regression guard. Additive.
    bool        testBosses = false;
    // --test-adaptive-hide (canon-aliens engine ext.): the type-keyed rotate-damage
    // rhythm on a Boss-type monster — full first hit, reduced same-type repeat,
    // type-rotation re-opens, window expires, opt-out (resist==0) is dead-code.
    bool        testAdaptiveHide = false;
    // --test-act2bosses (Act-2 roster, Wave 2): the 5 alien-planet-surface enemy
    // defs + 4 single-body bosses (Memory Hunter / Siren / Breeder Queen / Garrison
    // Commander) + the Wave-2 Tuning tags (startAllied / copyFeintPhase /
    // escapeTimerSeconds) + the Act-1 + Martinez regression guard. Additive.
    bool        testAct2Bosses = false;
    // --test-canonaliens (canon-alien roster — the four "most reported" species:
    // Mantis/Grey/Reptilian/Nordic, per the Davis-Puthoff visualisation). Builds
    // each of the 5 Tuning rows (SaurianSoldier/Warlord, GreyTasked, NordicSteward,
    // MantisArbiter) on a HeadlessDevice + Jolt world; asserts the roster is
    // complete + ordered, each row builds, and per-species stat invariants hold.
    bool        testCanonAliens = false;
    // --test-ui (UI pass): general game-UI layer (menus + HUD). Additive flag.
    bool        testUi = false;
    // --test-loading (loading-screen pass, Task #49): asserts progress is monotonic
    // 0->1 over the load steps + the tip line rotates. Headless. Additive flag.
    bool        testLoading = false;
    // --test-saveload (save/load pass): versioned checkpoint serialization. Additive.
    bool        testSaveLoad = false;
    // --test-dialog (AI-dialog + TTS pass, §3): the authored dialogue TREE advances
    // through nodes + player-choice branches OFFLINE; a stub AI provider hook is used
    // when set (else falls back to the tree); a stub TTS hook drives the NPC into/out
    // of the SPEAKING state. Fully offline + leak-clean; no network. Additive.
    bool        testDialog = false;
    // --demo-dialog [glb] (headless, offline): run the sample Sarah conversation
    // through a REAL skinned-NPC adapter (SpeakingMonster) that drives an anim
    // Skinner talk-bob (read-only) over a character GLB, with an offline stub TTS
    // hook pacing the speaking state. Prints each subtitle + the speaking
    // transitions + asserts the talk-bob actually animated the skeleton. Defaults
    // to chief_martinez_anim.glb. No window / Vulkan / network. Additive.
    bool        demoDialog = false;
    std::string demoDialogPath;
    // --test-valley (Crystal Valleys Act-2 L15) + --test-cliffs (Salvari cliffs finale).
    bool        testValley = false, testCliffs = false;
    // Space-combat stack (folded from feat/cockpit-vattalus): the Act-3 6DOF
    // space pilot (--test-space; energy-gated lasers + shield/hull two-pool +
    // 6DOF inertia), enemy ship-AI dogfight FSM (--test-ship-ai), targeting/
    // radar/lock-on (--test-targeting), the ship-damage model (--test-ship-damage),
    // and EVA zero-G spacewalk (--test-eva). All headless / deterministic. The
    // playable showcase is `--world space`. Additive flags.
    bool        testSpace = false, testShipAi = false, testTargeting = false;
    bool        testShipDamage = false, testEva = false;
    // --test-secretroom (code-locked trapdoor -> secret room): the cell HoloTerminal
    // override code opens a floor-hatch to a stocked secret room below. Additive flag.
    bool        testSecretRoom = false;
    // --test-club (the full Club 1127 "THE DEEP" at Y=-200): build headless + assert
    // the key fixtures (DJ booth, ORB, bars, 12-step stair, PA rig, 28 blacklights,
    // 6 TVs, the 50x100x30 ft room footprint/Y) + leak-clean. Additive flag.
    bool        testClub = false;
    // --test-spiremid (Spire mid-floor content): F3/F4/F5 encounter authoring. Additive.
    bool        testSpireMid = false;
    // --test-nexus (Floor 4.5 Nexus / The Chorus): off-elevator multi-pod boss. Additive.
    bool        testNexus = false;
    // --test-debris (K-T2 GPU-compute debris): spawn a burst, step the compute sim
    // through the live headless device, assert fall+settle+expiry+no-leak. Additive.
    bool        testDebris = false;
    // --test-gpuskin (GPU SKINNING OF MODELS): register a skinned mesh on the live
    // headless device, set a known palette, run the compute skinning pass, read back
    // the skinned output, and assert it matches a CPU LBS reference. Additive.
    bool        testGpuSkin = false;
    // --test-meshlet (D15 Tier-2 CPU meshlet builder): runs runMeshletSelfTest() —
    // builds meshlets from a generated grid mesh and asserts budgets/locality/sphere
    // containment/cone tightness/triangle conservation/degenerate input. Pure CPU,
    // no device needed. Additive.
    bool        testMeshlet = false;
    // --test-gpucull (D15 Tier-0 GPU cull): the EQUIVALENCE acceptance test — the
    // real device headless (validation on), GPU cull active, the CPU evaluating the
    // identical predicate per frame; asserts statDrawn == expected over a pose
    // sweep + conservation (drawn+culled==tested) + bypass + path-toggle. Additive.
    bool        testGpuCull = false;
    // --test-visunify (vis-unify acceptance gate): the ONE culling brain — policy
    // table + conservation across r_vis levels on a still camera + alias-cvar
    // mapping + the TLAS-mutation ZERO-sync-wait proof (double-buffer base). Additive.
    bool        testVisUnify = false;
    // --cullpath <n> / --hzb: seed the r_cullpath / r_hzb cvars from the CLI so the
    // smoketest/screenshot/bench paths exercise the D15 GPU cull (INT_MIN = unset).
    int         cullPathArg = INT_MIN;
    int         hzbArg = 0;
    // --vis <n>: seed THE unified r_vis cvar from the CLI (vis-unify; -1 auto, 0 cpu,
    // 1 +pvs, 2 pvs+gpu, 3 pvs+gpu+hzb). INT_MIN = unset (default 1). Wins over the
    // legacy --cullpath/--hzb seeds when both are given.
    int         visArg = INT_MIN;
    // --test-spiretop (Spire top-floor content): F6/F7 (Act-1 finale) encounter authoring. Additive.
    bool        testSpireTop = false;
    // --test-timeline (EFLZ morality/timeline backbone): infection 4-stage timers + cure
    // rates, the Omega/Alpha/Beta/Gamma timeline selector, the morality axes, and the
    // 12-ending eligibility map. Additive.
    bool        testTimeline = false;
    // --test-dronehack (F5 Drone Manufacturing): Sarah's master hack strips the Swarm
    // Controller AI's HP fraction + flips the drone set to allied (gated, not at load). Additive.
    bool        testDroneHack = false;
    // --test-sublevels (hidden Floor-7 sub-levels + Dr. Chen Return Mission): asserts the
    // descent is HIDDEN/inert until the F7-complete gate (Clone fallen + Sarah saved), then
    // SL1/SL2/SL3 build with the Frozen Collective mini-boss + a rescuable Dr. Chen. Additive.
    bool        testSubLevels = false;
    // --test-act2 (EFLZ Act-2 open-world surface): the alien-planet host + L8 Surface
    // Emergence (lab-exit gauntlet -> Emergence Point safe zone) + L9 Crystalline Desert
    // Edge (crystal props + neutral fauna + an inert-until-entered hazard zone). Additive.
    bool        testAct2 = false;
    // --test-act2desert (EFLZ Act-2 desert depths): L10 Crystalline Desert Depths
    // (deeper desert, first-contact allied Salvari + an injured-Salvari side-quest,
    // a hidden crystal-cave camp entrance, a light Overlord patrol) + L11 Salvari
    // Camp "Refugee Haven" (cave settlement, survivor markers incl. K'thara, an
    // upgrade-station interact + cultural-exchange beat). Reachable L9->L10->L11. Additive.
    bool        testAct2Desert = false;
    // --test-act2caves (EFLZ Act-2 mid biomes L12-15): the bioluminescent Advanced Cave
    // System (Crystal Heart dual-gated interactable + Memory Hunter abyss boss) + the
    // Toxic Swamplands edge (poison hazard zone, inert at load) + the Research Station
    // (timeline-gated Siren ambush) + the Tree Cities (vertical canopy + trading-post
    // interactable). Asserts the gates, the hazard, the timeline gate, reachability
    // L11->L12->L13->L14->L15, and trigger-id non-collision. Additive flag.
    bool        testAct2Caves = false;
    // --test-tod (EFLZ Time-of-Day): a 4-phase day cycle (dawn/day/dusk/night) that
    // drives the analytic sky/sun (dir/color/intensity/haze + ambient) via SkyParams.
    // Asserts the cycle visits all phases + wraps, the sun arc + intensity vary
    // monotonically across the day, city-lights/aurora gate on night, deterministic. Additive.
    bool        testTod = false;
    // --test-weather (EFLZ Weather): 7 states (clear/cloudy/rain/storm/fog/sandstorm/snow)
    // with smooth 30 s transitions, biome-gated, each nudging sky/fog/ambient + a hazard
    // flag. Asserts gating, interpolated transitions, hazard set only in hazardous states
    // (incl. swamp poison-fog), midpoint hazard flip, and determinism. Additive.
    bool        testWeather = false;
    bool        testWorldRegions = false;   // --test-worldregions (open-world surface regions + mountains)
    bool        testCity = false;           // --test-city (open-world metropolis: districts + roads + tunnels)
    bool        testOceanBase = false;      // --test-oceanbase (ocean + undersea base + submarine combat)
    // --test-collapse (K-T3 structural collapse): build a small structure (column /
    // beam on two supports), destroy a support, step the sim, and assert the
    // unsupported pieces fall (static->dynamic), anchored pieces stay stable, the
    // rubble settles bounded/NaN-free, GPU debris fires, and it's leak-clean. Additive.
    bool        testCollapse = false;
    // --test-physjoint (Physics §1): create a dynamic body on a point/distance
    // constraint, step the sim, and assert it hangs + swings under gravity then
    // settles with damping, and re-settles after an impulse; no NaNs; leak-clean.
    // Additive.
    bool        testPhysJoint = false;
    // --test-ragdoll (Physics §2): build a ragdoll from a synthetic skeleton, step,
    // assert it falls + settles (bounded, no NaN), the constraint chain holds (bone
    // lengths preserved), and the anim<->ragdoll blend 0->1 interpolates the palette
    // monotonically. Additive. (testRagdoll is declared in the block above.)
    // Clip-listing check (--list-clips <glb>): load a skinned GLB headless and
    // report its animation clip count + names, then sample Walk at t=0 vs t=0.5
    // and confirm the joint palette changes. Asset-pipeline verification for the
    // retargeted multi-clip character GLBs; no window / Vulkan. Additive — does
    // not affect the existing self-test gate.
    bool        listClips = false;
    std::string listClipsPath;
    // Locomotion-blend check (--test-locomotion [glb]): load a multi-clip GLB and
    // exercise the 1D idle/walk/run blend + Jump crossfade headless. Defaults to
    // chief_martinez_anim.glb if no path is given. No window / Vulkan. Additive.
    bool        testLocomotion = false;
    std::string testLocomotionPath;
    // --test-intro (intro cold-open): the prologue phase machine (Jake's last flight -> enemy
    // pulse -> white-out crash -> "6 MONTHS LATER" -> handoff to the cell) advances in order and
    // is skippable. No window / Vulkan. Additive — does not affect the existing gate.
    bool        testIntro = false;
    // --test-introorch (Phase 3 INTRO ORCHESTRATOR): the interactive branching
    // cold-open's beat state machine + skill->p->deterministic-outcome core. Asserts
    // beat sequencing, the skill->p mapping bounds, the deterministic chanceRoll gate,
    // the StoryFlags["intro.outcome"] write, and input-cleared/deterministic headless
    // interactive windows. No window / Vulkan. Additive — distinct from --test-intro
    // (the LEGACY cold-open phase-machine test).
    bool        testIntroOrch = false;
    // --test-introbranch (Phase 4 BRANCH WIRING): the app_run branch-selection
    // contract — intro.outcome flag round-trip, the --intro-force dev override,
    // the per-save seed thread, and the canon default. No window / Vulkan.
    bool        testIntroBranch = false;
    // --test-surfacestart (Phase 7): the ESCAPED-branch surface-landing Act-1 — the
    // cell-vs-surface branch selection + the surface scene standing up headlessly
    // (glass facility, player outside + armed, Sarah rescue target). No window/Vulkan.
    bool        testSurfaceStart = false;
    // Stress test: add N procedural cubes to the scene at startup (--stress N).
    // Default 0 = OFF; Level 1 is unaffected unless requested.
    uint32_t stressCount = 0;
    // Benchmark mode (--bench N [frames]): spawn N cubes, point the camera at the
    // field, run `frames` frames with vsync OFF, and report averaged FPS/CPU/GPU
    // ms. Headless of gameplay (no input); used to produce the perf baseline.
    bool     bench = false;
    uint32_t benchFrames = 600;
    // Screenshot mode (--screenshot [path.png]): build EFLZ Level 1, pose the
    // camera at a representative corridor vantage, render a few frames so shadows
    // + art settle, read the color image back to CPU, write a PNG, and exit 0.
    // Used to judge how the game looks without being at the keyboard. Default path
    // when omitted: G:\X3Native\screenshot.png.
    bool        screenshot = false;
    std::string screenshotPath = "G:/X3Native/screenshot.png";
    // Level Architect EDITOR mode (--editor): boot the live canon world with the Dear
    // ImGui (docking) editor overlay enabled. With no --editor flag ImGui never
    // initializes (zero cost — byte-for-byte the shipping game). Phase 0 draws only a
    // dockspace + the ImGui demo window (proof the integration renders); Phase 1 hosts
    // the real panels.
    bool        editorMode = false;
    // Headless editor proof (--screenshot-editor [path.png]): init ImGui in a headless
    // device, render ONE frame with the dockspace + demo window, and capture a PNG that
    // shows the ImGui window — proves the Phase-0 integration actually rasterizes.
    // NOTE: ImGui normally inits only in windowed --editor; this proof path is the
    // single exception (a forced headless ImGui init) so the render can be verified
    // without a display. Default path: build/proof/editor_p0.png.
    bool        editorShot = false;
    std::string editorShotPath = "build/proof/editor_p0.png";
    // UI-demo capture (--ui-demo [path.png] / --screenshot-menu): build EFLZ Level 1,
    // pose the gate-standard corridor camera, then draw the GENERAL game-UI MAIN MENU
    // (title + START / QUIT, the START button focused/hot) over the rendered scene and
    // capture a PNG — so the menu layer can be SEEN headlessly without being at the
    // keyboard. Additive + offscreen, like --screenshot. Default path:
    // G:/X3Native/captures/ui_menu.png.
    bool        uiDemo = false;
    std::string uiDemoPath = "G:/X3Native/captures/ui_menu.png";
    // Chat-tree dialog capture (--screenshot-dialog [path.png]): the --screenshot
    // path with the camera posed at the F5 captive (Lena) and her first_meeting
    // chat tree OPEN, so the choice UI can be judged headlessly. Additive.
    bool        dialogShot = false;
    std::string dialogShotPath = "docs/screenshots/dialog/lena_dialog.png";
    // W4-2: --screenshot-vigil [path.png] — the cell HoloTerminal captured with a
    // VIGIL chat-tree conversation live on the glass (orange ink). Additive.
    bool        vigilShot = false;
    // Which UI screen the --ui-demo capture shows: "main" (default), "pause", or
    // "settings". Lets one flag document all three menu screens.
    std::string uiDemoScreen = "main";
    // Sky vantage mode (--screenshot-sky [path.png]): build a minimal OUTDOOR test
    // scene (ground plane + the analytic sky lit by the existing sun), pose the
    // camera at the horizon looking slightly up toward the sun, render a few
    // settle frames, and capture a PNG that shows the sky gradient + sun disk +
    // horizon. EFLZ Level 1 is an enclosed interior, so this is the way to SEE the
    // open-world sky. Default path when omitted: G:\X3Native\sky.png.
    bool        skyShot = false;
    std::string skyShotPath = "G:/X3Native/sky.png";
    // --legacypost A/B: 1 = auto-exposure off (the pre-post-stack look);
    // 2 = also bloom off + tonemap passthrough (raw HDR clamp debugging).
    // BOTH levels also force TAA off — "legacy" means the pre-strike renderer,
    // and the bit-identical guarantee predates the TAA jitter.
    int         legacyPost = 0;
    // --notaa A/B: disable TAA only (jitter fully off + resolve skipped) so
    // before/after screenshots isolate exactly the TAA contribution.
    bool        noTaa = false;
    // Showroom preview (--screenshot-showroom [path.png]): load the baked Unity scene
    // export (assets/converted_glb/ShowRoom_Vol30/Example_01.glb), frame the camera on
    // the building cluster, capture a PBR-shaded PNG. Headless, like --screenshot.
    bool        showroomShot = false;
    std::string showroomShotPath = "G:/X3Native/showroom.png";
    // HERO CAR showcase (--screenshot-car [outDir]): load the converted hero-car
    // GLB (assets/converted_glb/Vehicles/CTR.glb — clearcoat paint + emissive
    // lights), pose it INSIDE the Unity showroom on a polished reflector slab
    // under emissive light panels (SSR/RT reflections sweep the body), and
    // capture a TURNTABLE set: 4 day-interior angles + 2 night-interior angles +
    // 2 night-EXTERIOR angles under the planet sky on wet asphalt. Headless,
    // 4x SSAA. Writes <outDir>/car_*.png (default docs/screenshots/vehicles).
    bool        carShot = false;
    std::string carShotDir = "docs/screenshots/vehicles";
    // FIRST-PERSON showroom proof (--screenshot-showroom-fp [path.png]): run the SAME
    // interactive `--world showroom` setup (walkable floor slab + companion Aria + the
    // wheeling night sky) but render ONE headless frame from the PLAYER SPAWN eye and
    // capture a PNG. This is the headless proof that the walkable content is correct
    // (interactive WASD/mouse can't be exercised headlessly). Default: G:\X3Native\showroom_fp.png.
    bool        showroomFpShot = false;
    std::string showroomFpShotPath = "G:/X3Native/showroom_fp.png";
    // RAGDOLL PROOF (--screenshot-showroom-ragdoll [path.png]): same headless showroom-FP
    // setup, but call girl.ragdoll() and step the physics world ~45 frames so Aria
    // COLLAPSES, then capture one frame. The frame must show her in a physics heap
    // (driven by applyExternalGlobals from the ragdoll), NOT the standing idle pose —
    // the headless proof the ragdoll drives the skin. Default: G:\X3Native\showroom_ragdoll.png.
    bool        showroomRagdollShot = false;
    std::string showroomRagdollShotPath = "G:/X3Native/showroom_ragdoll.png";
    // GLASS-DECK / ELEVATOR / HIDDEN-STAIR proofs (additive spire-top experience).
    // Each forces --world showroom on, then captures ONE headless frame from a
    // vantage that proves the new feature, and exits:
    //   --screenshot-showroom-deck  [path] — camera ON the glass deck at the spire
    //       top (~y=90), looking out at the night sky (deck glass + rails + planets).
    //   --screenshot-showroom-floor2 [path] — standing ON the 2nd floor (y=3) having
    //       climbed the synthesized stair; proves the climb collision + 2nd-floor slab.
    //   --screenshot-showroom-door  [path] — the hidden 2nd-floor WALL DOOR, shown
    //       OPEN (slid aside) revealing the entry passage behind it (set X3_SHOWROOM_
    //       DOORCLOSED=1 to instead capture it CLOSED/concealed flush in the wall).
    //   --screenshot-showroom-elevator [path] — the glass car in the ELEVATOR ATRIUM
    //       (the white room above the 2nd floor where the lift boards), camera inside.
    //   --screenshot-showroom-stair [path] — the entry PASSAGE + 90 deg TURN + the
    //       FLIGHT OF STAIRS climbing up to the elevator atrium.
    bool        showroomDeckShot = false;
    std::string showroomDeckShotPath = "C:/GameDev/X3Native-engine/build/proof/showroom_deck.png";
    bool        showroomElevShot = false;
    std::string showroomElevShotPath = "C:/GameDev/X3Native-engine/build/proof/showroom_elevator.png";
    bool        showroomStairShot = false;
    std::string showroomStairShotPath = "C:/GameDev/X3Native-engine/build/proof/showroom_stair.png";
    bool        showroomFloor2Shot = false;
    std::string showroomFloor2ShotPath = "C:/GameDev/X3Native-engine/build/proof/showroom_floor2.png";
    bool        showroomDoorShot = false;
    std::string showroomDoorShotPath = "C:/GameDev/X3Native-engine/build/proof/showroom_door.png";
    //   --screenshot-showroom-struts [path] — EXTERIOR shot from outside the building
    //       looking back at it, framing the SYMMETRIC radial set of thickened "/"
    //       strut legs (all four matched). Default build/proof/showroom_struts.png.
    bool        showroomStrutsShot = false;
    std::string showroomStrutsShotPath = "C:/GameDev/X3Native-engine/build/proof/showroom_struts.png";
    // HIDDEN ANALYST GALLERY proofs (--screenshot-showroom-gallery [path]): the secret
    // surveillance level ringing the central void at the elevator level (~Y10-13). The
    // flag captures TWO frames in one run: (a) the gallery itself — terminals glowing +
    // analyst figures around the ring — to <path>; (b) the view from the gallery looking
    // DOWN through the dark one-way glass onto the civilian floor/pad to <path>_down.png.
    // X3_SHOWROOM_GALLERY_UP=1 instead captures the civilian-floor view looking UP at the
    // dark-glass ceiling band (proving the analysts read dark/hidden from below).
    bool        showroomGalleryShot = false;
    std::string showroomGalleryShotPath = "C:/GameDev/X3Native-engine/build/proof/showroom_gallery.png";
    // CIVILIAN-FLOOR proof (--screenshot-showroom-civilians [path]): a wide DAY view
    // looking across the GROUND floor (blue pad + lounge) so the civilian crowd reads,
    // then a second frame from the 2nd-floor mezzanine deck (<path>_mezz.png). Proves
    // the civilians populate both floors naturally, day-lit, standing on the floor.
    bool        showroomCivShot = false;
    std::string showroomCivShotPath = "C:/GameDev/X3Native-engine/build/proof/showroom_civilians.png";
    // Planet preview (--screenshot-planet [path.png]): build a UV-sphere Moon body,
    // load the 5 FORGE3D Moon textures, light it from the side so a day/night
    // terminator reads, hang it against a dark space backdrop, and capture a PNG.
    // Headless, 4x SSAA, like --screenshot-showroom. Default path: G:\X3Native\planet.png.
    bool        planetShot = false;
    std::string planetShotPath = "G:/X3Native/planet.png";
    // Night-sky preview (--screenshot-nightsky [path.png]): build ONE UV-sphere and
    // hang ~6 VARIED planet TYPES (Moon, Ice, Gas, Lava, Terrestrial, Sun) staggered
    // across a dark, star-flecked dome, each shaded by its own per-type pipeline, and
    // capture a PNG. Headless, 4x SSAA, like --screenshot-planet. Default path:
    // G:\X3Native\nightsky.png.
    bool        nightskyShot = false;
    std::string nightskyShotPath = "G:/X3Native/nightsky.png";
    // ---- x3.cutscene/1 CLI (docs/design/CUTSCENE_FORMAT.md) ----
    // --test-cutscene          : headless format/eval/player self-test.
    // --skipintro              : never play the cold open this run.
    // --cutscene <file>        : play THAT cutscene at boot regardless of StoryFlags
    //                            (authoring loop; default = the shipped cold open).
    // --cuetime <s>            : start the played cutscene scrubbed to s seconds.
    // --cutscene-shot [path]   : HEADLESS film still — build the cinematic scene,
    //                            seek to --cuetime, capture one frame, exit. 4x SSAA.
    bool        testCutscene = false;
    bool        skipIntro    = false;
    // --intro-force <shot_down|escaped> : DEV override for the interactive intro's
    // outcome (QA/tests). -1 = none (roll the skill-biased {chance}); 0 = force the
    // canon SHOT_DOWN cell start; 1 = force the ESCAPED surface-landing start.
    int         introForce   = -1;
    std::string cutsceneFile;                  // empty = the shipped cold open
    float       cueTime      = 0.0f;
    bool        cutsceneShot = false;
    std::string cutsceneShotPath = "G:/X3Native/cutscene.png";
    // DDGI gate-shot proof (--screenshot-ddgi [outDir]): build a minimal sealed
    // two-room rig (room A holds a point light + an emissive ceiling panel; room B
    // is connected only through a doorway; room C is fully SEALED next to A — the
    // leak canary), render OFF/ON/debug captures headless and exit. Probes converge
    // over ~120 settle frames before each ON capture.
    bool        ddgiShot = false;
    std::string ddgiShotDir = "docs/screenshots/ddgi";
    // RT soft-shadow gate-shot proof (--screenshot-rtshadows [outDir]): build a
    // detention-cell rig (a single ceiling lamp + occluders at two distances from
    // the wall), capture lamp-shadow OFF/ON A/Bs (tier 0 vs 2), a sun CSM-vs-RT
    // A/B (contact hardening), and a 3-frame motion burst (TAA sizzle check);
    // logs the GPU-ms cost delta. Headless; exits after the captures.
    bool        rtshShot = false;
    std::string rtshShotDir = "docs/screenshots/rtshadows";
    // --test-rtshadows: headless smoketest with r_rtshadows forced to tier 2 so
    // the mesh_rt pipelines + TLAS path run under Vulkan validation.
    bool        testRtShadows = false;
    bool        noRtShadows = false;       // --nortshadows: pin r_rtshadows 0 (CSM-only A/B)
    // --test-framepacing (ZERO-STUTTER GUARANTEE, docs/ZERO_STUTTER.md): headless
    // 600-frame scripted camera flythrough of the built world (default Level 1)
    // that asserts, post-warmup: ZERO spike frames (> r_fpace_spikex * rolling
    // median + r_fpace_floor), ZERO pipelines/shader modules created after frame
    // 1, ZERO descriptor-pool growth. Prints the CPU+GPU p50/p95/p99/p999/max.
    bool        testFramePacing = false;
    // --test-boottime [budgetMs] (BOOT-TO-GAMEPLAY gate, docs/BOOT_TIME.md): boot the
    // REAL windowed interactive path (window + swapchain + full world build), skip the
    // intro cold-open (content, not boot work), run exactly ONE main-loop frame (the
    // first interactive frame: world built, menu live, player controllable on START),
    // print the [boot] phase table, and exit 0 iff total < the budget. The budget
    // defaults to the boot_budget_ms cvar default (2000 ms) and can be loosened for
    // weaker machines via the optional CLI arg or `boot_budget_ms` in the console cfg.
    bool        testBootTime = false;
    double      bootBudgetMs = 0.0;    // 0 = use the boot_budget_ms cvar (default 2000)
    // Terrain vantage mode (--screenshot-terrain [path.png]): build the tiled
    // procedural terrain world (terrain + sky + sun), pose a camera up on the
    // hills looking toward the sun so the lit rolling terrain + cast shadows +
    // sky read clearly, settle a few frames, and capture a PNG. Default path when
    // omitted: G:\X3Native-wt-terrain\terrain.png.
    bool        terrainShot = false;
    std::string terrainShotPath = "G:/X3Native-wt-terrain/terrain.png";
    // Surface-library material preview (--screenshot-matlib [outDir]): headless
    // contact-sheet renders of every assets/surface_library set (ART_BIBLE §4
    // realism mandate; sets curated by tools/tex_curate.py). FLAT output folder.
    bool        matlibShot = false;
    std::string matlibShotDir = "D:/GameDev/matlib_review";
    // Ocean vantage mode (--screenshot-ocean [path.png]): build the procedural
    // terrain world + an animated ocean at sea level under the sky/sun, pose a
    // camera on the shore looking out across the water toward the sun so the lit
    // animated waves, sun glint, depth-based shallow/deep color, and the
    // terrain->water shoreline all read, settle a few frames so the waves animate
    // + the shadow map registers, and capture a PNG. EFLZ Level 1 is interior;
    // this is the way to SEE the ocean. Default path: G:\X3Native-wt-water\ocean.png.
    bool        oceanShot = false;
    std::string oceanShotPath = "G:/X3Native-wt-water/ocean.png";
    // Undersea base vantage (--screenshot-oceanbase [path.png]) — W3-4: builds the
    // ocean_base zone (textured hull + practicals) under deep-water fog and captures
    // an approach shot plus a dock closeup (shot 2 path gets a _dock suffix).
    bool        oceanBaseShot = false;
    std::string oceanBaseShotPath = "oceanbase.png";
    // Destruction shatter capture (--screenshot-destruct [path.png]): build the
    // destruction demo world (lit ground + a row of destructible crates), shoot +
    // explode the crates so they shatter into tumbling convex chunks, settle a few
    // frames, and capture a PNG showing the intact->broken transition + scattered
    // chunks. Headless / offscreen, like --screenshot. Default path:
    // G:/X3Native/captures/destruct.png.
    bool        destructShot = false;
    std::string destructShotPath = "G:/X3Native/captures/destruct.png";
    // AI-action capture mode (--capture-ai [outDir]): build a clearly-lit demo arena
    // (lit ground + sky + point/sun fill) with a fixed player reference and a small
    // squad of enemies driven by the REAL combat-AI state machine (a Guard advances,
    // a Drone strafes, one enemy is damaged mid-run -> Retreat, one loses LOS ->
    // Search), pose a fixed 3/4 elevated camera, step the sim at fixed dt for ~6 s,
    // capture a numbered PNG frame every ~0.2 s into outDir, assemble an animated
    // GIF (G:\X3Native\ai_action.gif), and print a per-phase state log. Headless /
    // offscreen (no window), like --screenshot. Default outDir: G:\X3Native\ai_action.
    bool        captureAi    = false;
    std::string captureAiDir = "G:/X3Native/ai_action";
    // Walk-capture mode (--capture-walk [outPath]): build ONE close-up animated
    // guard (the multi-clip *_anim.glb when present), drive the T1 locomotion blend
    // toward a steady WALK, settle the blend a fraction of a second, then capture a
    // single PNG at a clearly mid-stride moment. Verifies the locomotion blend
    // visibly in-engine. Headless / offscreen. Default outPath: build/walk_pose.png.
    bool        captureWalk     = false;
    std::string captureWalkPath = "G:/X3Native-wt-animt1/build/walk_pose.png";
    // Foot-IK capture (--screenshot-footik [outPath]): build ONE animated character
    // standing on a SLOPED + STEPPED surface with foot-IK ON, drive a slow idle/walk
    // blend, plant the feet on the surface (raycast down via the local physics world),
    // adjust the pelvis, settle, and capture a single PNG showing the feet grounded on
    // the slope/step (vs floating). Headless / offscreen. Default: build/footik_pose.png.
    bool        captureFootIk     = false;
    std::string captureFootIkPath = "G:/X3Native-wt-footik/build/footik_pose.png";
    // Spire per-floor capture (--capture-spire [outDir]): build the FULL Act-1 host
    // (Level1Game + SpireMidFloors + SpireTopFloors, the same real lit scene the game
    // builds), then for EACH Spire floor B1,F1,F2,F3,F4,F5,F6,F7 pose the camera at
    // that floor's arrival/hub vantage looking across the main room, light the plate,
    // settle a few frames, and capture <outDir>/spire_<floor>.png. A dev/playtest
    // tool: it CHANGES NO gameplay/balance — it only renders + reads back the scene
    // each floor already builds. Headless / offscreen (no window), like --screenshot.
    // Prints one line per floor (path + that floor's enemy count). 0 VUID under Debug.
    bool        captureSpire    = false;
    std::string captureSpireDir = "captures/spire";
    // World selector (--world terrain): launch the playable OUTDOOR terrain world
    // (walk the hills) instead of the default interior Level 1. Anything else (or
    // omitted) keeps Level 1 as the default, unchanged.
    std::string worldMode = "level1";
    bool        worldExplicit = false;   // --world was passed (vs the default)
    bool shotWorldMap = false;   // --screenshot-worldmap (headless map shot sequence)
    // Seamless world streaming tunables (--world streamed; see app/world_stream.*):
    // per-frame stream-work budget (ms) + velocity lookahead (s). Cvar-style CLI
    // overrides: --ws-budget <ms> / --ws-lookahead <s>.
    float wsBudgetMs   = 6.0f;
    float wsLookaheadS = 2.5f;
    // Optional settle-frame count for --screenshot (default 16 = unchanged
    // behavior). Larger values advance the world (and the characters' skeletal
    // animation) further before the capture, so two shots at different counts show
    // different animated poses — used to prove J1 animation is live.
    int         screenshotSettle = 16;
    // Optional --screenshot camera override (--shot-cam x,y,z,yaw,pitch). When set,
    // the screenshot uses this vantage instead of the default corridor pose — used
    // to capture the tall arena / elevator shaft (the default corridor pose stays
    // the gate-standard view). Does NOT change any default behavior when omitted.
    bool        shotCamOverride = false;
    float       shotCam[5] = { 8.0f, 1.75f, -0.4f, 0.06f, -0.16f };
    // FX demo (--fx-demo): in --screenshot mode, spawn a combat particle/decal burst
    // (muzzle flash + impact sparks + dust + a scorch decal) a couple meters in front
    // of the screenshot camera each settle frame so the capture clearly shows the new
    // GPU particles (glowing via bloom, soft against depth) + a bullet decal on the
    // surface. Off by default — the standard --screenshot gate view is unchanged.
    bool        fxDemo = false;
    // Windowed-mode resolution (--width <px> / --height <px>). Defaults to the
    // historical 1280x720 so the dev box + every headless/offscreen path are
    // UNCHANGED. A high-DPI box can pass e.g. --width 2560 --height 1440. These
    // affect ONLY the on-screen window: headless capture/screenshot resolution is
    // forced back to 1280x720 below regardless of these flags.
    uint32_t    winW = 1600, winH = 900;   // bigger windowed default (NOT maximized)
    bool loadedWinSize = false;   // a saved "SET AS DEFAULT" window size was found
};

// Parse argv into o. Mirrors main()'s old inline loop byte-for-byte (o.-prefixed).
void parseCli(int argc, char** argv, CliOptions& o);

}} // namespace x3::apphost
