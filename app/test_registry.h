#pragma once
// ===========================================================================
// TEST REGISTRY — the headless `--test-*` / `--demo-*` / `--list-clips`
// dispatch ladder, factored out of app/main.cpp's main() (#28 monolith split).
//
// main() parses ~100 CLI flags into local bools, then runs a long chain of
//   if (testX) { log; return runX() ? 0 : 1; }
// blocks BEFORE the engine boots (these tests need no window / Vulkan, except
// the few that drive the headless device via the x3::apphost self-tests). That
// whole chain reads ONLY the flag bools (+ a few path strings) and returns an
// exit code; it touches none of the render/world/camera state. So it lifts out
// cleanly: main() fills a TestFlags struct from its locals and calls
// dispatchTests(); a non-negative return is main()'s exit code, -1 means "no
// test flag set, continue normal boot".
//
// NOTE: the smoketest-style GPU A/B flags (--test-rt/-reflections/-rtshadows/
// -ddgi/-framepacing) are NOT here — they set smoketest=true and flow through
// main()'s real render path, not this headless ladder.
//
// The existing C1061 chain-breaks (the ladder is plain `if`s, each returning,
// not one giant else-if) are PRESERVED: every block still independently returns.
// ===========================================================================

#include <string>

namespace x3::apphost {

// The flag/path state the headless dispatch ladder reads. Mirrors the main()
// locals 1:1 (same names) so population is a verbatim assignment list.
struct TestFlags {
    // --- self-tests (return runX()?0:1) ---
    bool testJobs = false, testAsset = false, testConsole = false, testEngineConsole = false, testPhysics = false;
    bool testPhysJoint = false, testRagdoll = false, testGltf = false, testPlayer = false;
    bool testInteract = false, testPhysprops = false, testRagdollSkin = false;
    bool testDoors = false;       // --test-doors (door-mesh swap: ease/dt/variants/passability/audio)
    bool testEditor = false, testBlockout = false, testLoader = false, testBarrels = false;
    bool testEditorAi = false;   // --test-editor-ai (AI Architect plan validator)
    bool testGlass = false, testFrustumCull = false, testHoloterm = false;
    bool testSecretRoom = false, testHatch = false, testLlm = false, testEcs = false;
    bool testDescentFall = false;   // --test-descentfall (fall shaft + dark room + keypad + elevator)
    bool testCaveAtmos = false;     // --test-caveatmos (crystal-only + beat pulse + fog cave atmosphere)
    bool testEcsRender = false, testPickup = false, testCombat = false, testDeathRagdoll = false;
    bool testAudio = false, testAcoustics = false, testLevel1 = false, testCanonLevel = false;
    bool testKeypad = false;      // --test-keypad (realistic high-poly access keypad, KP1-KP6)
    bool testLevelLint = false;   // --test-levellint (GATE A: door-seat / junction / cut-span / reach)
    bool testPropClip = false;    // --test-propclip (GATE A ext: dressing prop AABB vs room bounds)
    bool testCanonPlay = false, testIntro = false, testCutscene = false, testPhase2a = false;
    bool testGrounding = false;   // --test-grounding (character feet vs support surface, app/grounding.h)
    bool testStairNav = false;   // feat/stair-nav: enemy stairwell pathing (S1-S5)
    bool testFilmic = false;      // --test-filmic (composite filmic block: CPU-mirror identity probe)
    bool testGoldenPath = false;
    bool testOpening = false;     // --test-opening (opening-flow: the wake-in-cell contract)
    bool testDescMech = false;    // --test-descmech (W9-1: desc-field Tier-A mechanics)
    bool testInventory = false;    // W9-3 RPG: backpack/item-db (--test-inventory)
    bool testProgression = false;  // W9-3 RPG: XP/levels (--test-progression)
    bool testSkillTree = false;    // W9-3 RPG: skill tree (--test-skilltree)
    bool testStrata = false;            // R-3 fold: THE DESCENT (strata module chain)
    bool testCityBlocks = false;        // --test-cityblocks (city block/lot/frontage generator)
    bool testSeaLevel = false;          // --test-sealevel (sea-datum consistency: one sea, derived offsets)
    bool testElevatorShowcase = false;  // R-4 fold: THE CENTERPIECE showcase  // --test-goldenpath (W5-3: the endgame spine, Gate-C foundation)
    bool testIntroOrch = false;   // --test-introorch (Phase 3 Intro Orchestrator core)
    bool testIntroBranch = false; // --test-introbranch (Phase 4 app_run branch wiring)
    bool testIntroCockpit = false;// --test-introcockpit (intro cockpit Scene-entity rig)
    bool testStarsystems = false; // --test-starsystems (x3.starsys/1 registry integrity)
    bool testShipInterior = false;// --test-shipinterior (S5 walkable interior, headless)
    bool testShipWindows  = false;// --test-shipwindows (S6 true-portal windows, headless)
    // Space-combat feast fold (14900K lanes, headless):
    bool testBodyContact = false;     // --test-bodycontact (bone-surface contact + soft indent)
    bool testWormhole = false;        // --test-wormhole (Salvari crystal-matrix VFX)
    bool testWormholeTransit = false; // --test-wormhole-transit (S3 autopilot jump ride)
    bool testTractor = false;         // --test-tractor (intro capital-ship capture beam)
    bool testDescentSlide = false;// --test-descentslide (Wave 2C track spec + rider sim, headless)
    bool testWingDressing = false;// --test-wingdressing (F2-F7 wing recipe dressing, headless)
    bool testSurfaceStart = false;// --test-surfacestart (Phase 7 ESCAPED-branch surface Act-1)
    bool testSurfaceHandoff = false;// --test-surfacehandoff ([P0-1] surface -> facility handoff)
    bool testApronLanding = false;// --test-apronlanding (ONE WORLD landing: intro -> canon apron)
    bool testPhase2b = false, testAnim = false, testLocomotion = false;
    bool listClips = false;
    bool testTerrain = false, testTerrainPlace = false, testStreaming = false;
    bool testTerrainCorridor = false;  // --test-terraincorridor (corridor heightfield depression)
    bool testRouteFrame = false;
    bool testRoadNetwork = false;     // --test-roadnetwork (the ring tours)      // --test-routeframe (polyline frame, P1)
    bool testRiverBridge = false;     // --test-riverbridge (valley road + Bridge No.1)
    bool testTraffic = false;         // --test-traffic (freeway AI traffic)
    bool testGasStation = false;      // --test-gasstation (W-STATIONS forecourts + fuel stub)
    bool testFactory     = false;     // --test-factory (the works siting + the tickets)
    bool testInterchange = false;     // --test-interchange (the diamond grade split)
    bool testTunnelDrive = false;      // --test-tunneldrive (drive-through the demo bore, negative-controlled)
    bool testSummitLot   = false;      // --test-summitlot (the pad at the top of the summit spur)
    bool testRidgeRoad   = false;      // --test-ridgeroad (the dirt road along the tops)
    bool testTunnelMouth = false;     // --test-tunnelmouth (THE tunnel-mouth defect gate)
    bool testWorldStream = false, testWorldMap = false, testAi = false, testBestiary = false;
    bool testEnemyScale = false;     // --test-enemy-scale (THE BODY-SIZE SWEEP: every live enemy row renders body-sized)
    bool testBosses = false, testAct2Bosses = false, testSpireMid = false, testNexus = false;
    bool testAdaptiveHide = false;   // --test-adaptive-hide (canon-aliens rotate-damage rhythm)
    bool testCanonAliens = false;    // --test-canonaliens (canon-alien roster: Mantis/Grey/Reptilian/Nordic)
    bool testPackSpiders = false;    // --test-packspiders (pack-harvest arachnids: Lab Skitterer / Venom Brood)
    bool testClone = false;          // --test-clone (THE CLONE: Act-1 finale 3-phase boss + neural collar)
    bool testGallery = false;        // --test-gallery (character-gallery cast + clip cycle)
    bool testCutaway = false;        // --test-cutaway (Level Architect cutaway model)
    bool testSpireTop = false, testTimeline = false, testDroneHack = false, testSubLevels = false;
    bool testTod = false, testWeather = false, testAct2 = false, testAct2Desert = false;
    bool testAct2Caves = false, testWorldRegions = false, testCity = false, testOceanBase = false;
    bool testRifthub = false;   // --test-rifthub (Stargate portal hub self-test)
    bool testBasis = false;     // --test-basis (KNOWN_BUGS R3: the MIRROR determinant invariant)
    bool testDoorCode = false, testHatchCode = false, testElevator = false, testElevatorFsm = false;
    bool testNet = false, testNetSync = false, testNetInterp = false, testNetPredict = false;
    bool testRescue = false, testThirdPerson = false, testNpcTalk = false, testChatTree = false;
    bool testCompanionCombat = false;   // --test-companion-combat (Sarah ally combat, Lane B)
    bool testVigil = false;
    bool testMission = false, testDestruction = false, testDebris = false, testGpuSkin = false;
    bool testMeshlet = false, testGpuCull = false, testCollapse = false, testNav = false;
    bool testVisUnify = false;   // --test-visunify (vis-unify acceptance gate)
    bool testWeapons = false, testScript = false, testVehicle = false, testVehParts = false;
    bool testCsm = false;   // Lane 3: cascaded shadow maps (splits, snapping, rotation invariance)
    bool testGeoLod = false;// Lane 5: mesh LOD (screen-space error, hysteresis) + vertex compression
    bool testMineFx = false;// mines lane: Armory mine entrance census + glow bake
    bool testWetness = false;// wetness lane: rain soak/dry asymmetry, ice hysteresis, tire grip
    bool testStorm   = false;// storm lane: lightning flash shape + thunder travel delay
    bool testPrecip  = false;// precip lane: falling snow/rain, camera-local volume
    bool testTunnelFitout = false;// tunnel lane: bore interior program placement
    bool testTunnelRooms = false;// tunnel lane: rooms/halls/stairs behind the service doors
    bool testCanonVehicle = false;   // --test-canonvehicle (WORLD CARS enter/drive/exit/hack)
    bool testReflDenoise = false;    // --test-refldenoise (edge-aware a-trous reflection DENOISE; pure CPU)
    bool testEchoRoads = false;      // --test-echoroads (LIFT A: EchoRoads road-graph checksum; no GPU, no assets)
    bool testLightningCharge = false;  // --test-lightning-charge (Lightning Gun CHARGE model)
    bool testEcology = false, testCrowd = false, testAlert = false, testFootIk = false;
    bool testNpcLife = false;    // --test-npclife (LIVING CITY daily-life system)
    bool testHacking = false;    // --test-hacking (WD2 scan/hack registry)
    bool testWaterZap = false;   // --test-waterzap (FISH schools + the lightning WATER ZAP)
    bool testSealife = false;    // --test-sealife (great white hunt, bite-once, zap, abyss)
    bool testUi = false, testLoading = false, testSaveLoad = false, testDialog = false;
    bool demoDialog = false, testValley = false, testCliffs = false, testClub = false;
    bool testComplex = false;
    bool testClubNpcs = false;   // --test-clubnpcs (feat/club-npcs: 3 canon NPCs + trees)
    bool testJukebox = false;   // --test-jukebox (Club Jukebox pipeline)
    bool testListen = false;   // --test-listen (CLUB LISTEN MODE beat-detector self-test)
    bool testPerfshop = false;
    bool testGamma = false;    // --test-gamma (LINEAR-vs-GAMMA acceptance-gate byte measurement)
    // Space-combat stack (folded from feat/cockpit-vattalus): the Act-3 6DOF
    // space pilot (--test-space) + enemy ship-AI (--test-ship-ai) + targeting/
    // radar/lock (--test-targeting) + ship-damage model (--test-ship-damage) +
    // EVA zero-G spacewalk (--test-eva). All headless / deterministic.
    bool testSpace = false, testShipAi = false, testTargeting = false;
    bool testSpaceHud = false, testCockpitSway = false;
    bool testShipDamage = false, testEva = false;

    // --- path/string args used by a few handlers ---
    std::string testLocomotionPath;
    std::string listClipsPath;
    std::string demoDialogPath;
};

// ---------------------------------------------------------------------------
// --test-grounding — THE CHARACTER-GROUNDING GATE (app/grounding.h).
//
// The 0cbe3f89 wiring: parseCli() sets this inline global directly and
// dispatchTests() reads it first (it predates the EXE split's TestFlags copy
// and stayed additive/zero-conflict for that reason). Merge 58eb79b3
// (inspx/la-exe -> integration/playtest-0814) dropped the cli.cpp + this block
// in its KEEP-BOTH resolution, leaving runGroundingSelfTest() with NO caller —
// the gate existed but could not be run. Restored 2026-08-16
// (audit/rifthub-portals). Folds into TestFlags in one line whenever a lane
// that owns main.cpp wants it.
// ---------------------------------------------------------------------------
inline bool g_testGrounding = false;

// Run the headless test ladder. Returns 0/1 (the program exit code) if a test
// flag matched, or -1 if none matched (main() should continue normal boot).
int dispatchTests(const TestFlags& tf);

} // namespace x3::apphost
