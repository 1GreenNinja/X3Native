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
    bool testJobs = false, testAsset = false, testConsole = false, testPhysics = false;
    bool testPhysJoint = false, testRagdoll = false, testGltf = false, testPlayer = false;
    bool testInteract = false, testPhysprops = false, testRagdollSkin = false;
    bool testEditor = false, testBlockout = false, testLoader = false, testBarrels = false;
    bool testEditorAi = false;   // --test-editor-ai (AI Architect plan validator)
    bool testCanonEdit = false;  // --test-canonedit (the editor edits THE GAME's canon level)
    bool testGlass = false, testFrustumCull = false, testHoloterm = false;
    bool testSecretRoom = false, testHatch = false, testLlm = false, testEcs = false;
    bool testEcsRender = false, testPickup = false, testCombat = false, testDeathRagdoll = false;
    bool testAudio = false, testAcoustics = false, testLevel1 = false, testCanonLevel = false;
    bool testKeypad = false;      // --test-keypad (realistic high-poly access keypad, KP1-KP6)
    bool testLevelLint = false;   // --test-levellint (GATE A: door-seat / junction / cut-span / reach)
    bool testCanonPlay = false, testIntro = false, testCutscene = false, testPhase2a = false;
    bool testGoldenPath = false;
    bool testDescMech = false;    // --test-descmech (W9-1: desc-field Tier-A mechanics)
    bool testInventory = false;    // W9-3 RPG: backpack/item-db (--test-inventory)
    bool testProgression = false;  // W9-3 RPG: XP/levels (--test-progression)
    bool testSkillTree = false;    // W9-3 RPG: skill tree (--test-skilltree)
    bool testStrata = false;            // R-3 fold: THE DESCENT (strata module chain)
    bool testElevatorShowcase = false;  // R-4 fold: THE CENTERPIECE showcase  // --test-goldenpath (W5-3: the endgame spine, Gate-C foundation)
    bool testIntroOrch = false;   // --test-introorch (Phase 3 Intro Orchestrator core)
    bool testIntroBranch = false; // --test-introbranch (Phase 4 app_run branch wiring)
    bool testIntroCockpit = false;// --test-introcockpit (intro cockpit Scene-entity rig)
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
    bool testPhase2b = false, testAnim = false, testLocomotion = false;
    bool listClips = false;
    bool testTerrain = false, testTerrainPlace = false, testStreaming = false;
    bool testWorldStream = false, testWorldMap = false, testAi = false, testBestiary = false;
    bool testBosses = false, testAct2Bosses = false, testSpireMid = false, testNexus = false;
    bool testAdaptiveHide = false;   // --test-adaptive-hide (canon-aliens rotate-damage rhythm)
    bool testCanonAliens = false;    // --test-canonaliens (canon-alien roster: Mantis/Grey/Reptilian/Nordic)
    bool testSpireTop = false, testTimeline = false, testDroneHack = false, testSubLevels = false;
    bool testTod = false, testWeather = false, testAct2 = false, testAct2Desert = false;
    bool testAct2Caves = false, testWorldRegions = false, testCity = false, testOceanBase = false;
    bool testRifthub = false;   // --test-rifthub (Stargate portal hub self-test)
    bool testBasis = false;     // --test-basis (KNOWN_BUGS R3: the MIRROR determinant invariant)
    bool testDoorCode = false, testHatchCode = false, testElevator = false, testElevatorFsm = false;
    bool testNet = false, testNetSync = false, testNetInterp = false, testNetPredict = false;
    bool testRescue = false, testThirdPerson = false, testNpcTalk = false, testChatTree = false;
    bool testMission = false, testDestruction = false, testDebris = false, testGpuSkin = false;
    bool testMeshlet = false, testGpuCull = false, testCollapse = false, testNav = false;
    bool testVisUnify = false;   // --test-visunify (vis-unify acceptance gate)
    bool testWeapons = false, testScript = false, testVehicle = false, testVehParts = false;
    bool testCanonVehicle = false;   // --test-canonvehicle (WORLD CARS enter/drive/exit/hack)
    bool testLightningCharge = false;  // --test-lightning-charge (Lightning Gun CHARGE model)
    bool testEcology = false, testCrowd = false, testAlert = false, testFootIk = false;
    bool testWaterZap = false;   // --test-waterzap (FISH schools + the lightning WATER ZAP)
    bool testUi = false, testLoading = false, testSaveLoad = false, testDialog = false;
    bool demoDialog = false, testValley = false, testCliffs = false, testClub = false;
    bool testPerfshop = false;
    // Space-combat stack (folded from feat/cockpit-vattalus): the Act-3 6DOF
    // space pilot (--test-space) + enemy ship-AI (--test-ship-ai) + targeting/
    // radar/lock (--test-targeting) + ship-damage model (--test-ship-damage) +
    // EVA zero-G spacewalk (--test-eva). All headless / deterministic.
    bool testSpace = false, testShipAi = false, testTargeting = false;
    bool testShipDamage = false, testEva = false;

    // --- path/string args used by a few handlers ---
    std::string testLocomotionPath;
    std::string listClipsPath;
    std::string demoDialogPath;
};

// Run the headless test ladder. Returns 0/1 (the program exit code) if a test
// flag matched, or -1 if none matched (main() should continue normal boot).
int dispatchTests(const TestFlags& tf);

} // namespace x3::apphost
