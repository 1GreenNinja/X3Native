// cli — parseCli() (the arg-parse loop) lifted VERBATIM from main() (#28 Phase D).
#include "cli.h"
#include "settings_io.h"   // readWindowSize (saved window default)
#include <string_view>
#include <cstdlib>
#include <cstring>

namespace x3 { namespace apphost {

void parseCli(int argc, char** argv, CliOptions& o) {
    o.loadedWinSize = readWindowSize(o.winW, o.winH);   // saved "SET AS DEFAULT" size
    (void)o.loadedWinSize;
    for (int i = 1; i < argc; ++i) {
        std::string_view a(argv[i]);
        // DDGI flags handled OUTSIDE the big else-if chain below (MSVC C1061:
        // every `else if` nests a block; the chain is at the compiler's limit).
        if (a == "--test-ddgi") { o.smoketest = true; o.testDdgi = true; continue; }
        if (a == "--ddgi") { o.ddgiForce = true; continue; }
        if (a == "--screenshot-ddgi") {
            o.ddgiShot = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') o.ddgiShotDir = argv[++i];
            continue;
        }
        if (a == "--screenshot-reflverify") {
            o.reflVerifyShot = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') o.reflVerifyShotDir = argv[++i];
            continue;
        }
        // RT soft-shadow flags — handled OUTSIDE the chain (same C1061 reason).
        if (a == "--test-rtshadows") { o.smoketest = true; o.testRtShadows = true; continue; }
        // Zero-stutter flythrough — handled OUTSIDE the chain (same C1061 reason).
        if (a == "--test-framepacing") { o.testFramePacing = true; continue; }
        if (a == "--test-boottime") {
            o.testBootTime = true;
            if (i + 1 < argc && argv[i + 1][0] != '-')
                o.bootBudgetMs = std::atof(argv[++i]);
            continue;
        }
        if (a == "--nortshadows") { o.noRtShadows = true; continue; }   // A/B: pin tier 0 (CSM-only)
        if (a == "--screenshot-rtshadows") {
            o.rtshShot = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') o.rtshShotDir = argv[++i];
            continue;
        }
        // World-streaming flags handled OUTSIDE the big else-if chain (which sits at
        // MSVC's C1061 block-nesting limit — adding to it breaks the build).
        if (a == "--test-worldstream") { o.testWorldStream = true; continue; }
        if (a == "--test-worldmap")    { o.testWorldMap    = true; continue; }
        if (a == "--screenshot-worldmap") {   // headless world-map shot sequence
            o.shotWorldMap = true; o.worldMode = "streamed"; o.screenshot = true; continue;
        }
        if (a == "--ws-budget") {   // per-frame world-stream budget, ms (cvar-style tunable)
            if (i + 1 < argc && argv[i + 1][0] != '-') o.wsBudgetMs = std::strtof(argv[++i], nullptr);
            continue;
        }
        if (a == "--ws-lookahead") { // velocity lookahead, seconds
            if (i + 1 < argc && argv[i + 1][0] != '-') o.wsLookaheadS = std::strtof(argv[++i], nullptr);
            continue;
        }
        if (a == "--smoketest") o.smoketest = true;
        else if (a == "--legacypost")  o.legacyPost = 1;   // A/B: auto-exposure OFF (pre-strike look)
        else if (a == "--legacypost2") o.legacyPost = 2;   // A/B: + bloom OFF + tonemap passthrough
        else if (a == "--notaa")       o.noTaa = true;     // A/B: TAA off (jitter + resolve disabled)
        else if (a == "--norefl")      o.noRefl = true;    // A/B: reflections off (TAA stays on)
        else if (a == "--test-rt") { o.smoketest = true; o.testRt = true; }
        else if (a == "--test-reflections") { o.smoketest = true; o.testReflections = true; }
        else if (a == "--test-jobs") o.testJobs = true;
        else if (a == "--test-asset") o.testAsset = true;
        else if (a == "--test-console") o.testConsole = true;
        else if (a == "--test-physics") o.testPhysics = true;
        else if (a == "--test-gltf") o.testGltf = true;
        else if (a == "--test-player") o.testPlayer = true;
        else if (a == "--test-interact") o.testInteract = true;
        else if (a == "--test-doors") o.testDoors = true;   // door-mesh-swap polish gate (D1-D6)
        else if (a == "--test-physprops") o.testPhysprops = true;
        else if (a == "--test-ragdoll") o.testRagdoll = true;
        else if (a == "--test-ragdollskin") o.testRagdollSkin = true;
        else if (a == "--test-editor") o.testEditor = true;
        else if (a == "--test-loader") o.testLoader = true;
        else if (a == "--test-blockout") o.testBlockout = true;
        else if (a == "--test-barrels") o.testBarrels = true;
        else if (a == "--test-glass") o.testGlass = true;
        else if (a == "--test-frustumcull") o.testFrustumCull = true;
        else if (a == "--test-holoterm") o.testHoloterm = true;
        else if (a == "--test-llm") o.testLlm = true;
        else if (a == "--test-editor-ai") o.testEditorAi = true;
        else if (a == "--test-secretroom") o.testSecretRoom = true;
        else if (a == "--test-descentfall") o.testDescentFall = true;
        else if (a == "--test-caveatmos") o.testCaveAtmos = true;
        else if (a == "--test-ecs") o.testEcs = true;
        else if (a == "--test-ecsrender") o.testEcsRender = true;
        else if (a == "--test-pickup") o.testPickup = true;
        else if (a == "--test-combat") o.testCombat = true;
        else if (a == "--test-deathragdoll") o.testDeathRagdoll = true;
        else if (a == "--test-audio") o.testAudio = true;
        else if (a == "--test-acoustics") o.testAcoustics = true;
        else if (a == "--test-level1") o.testLevel1 = true;
        else if (a == "--test-canonlevel") o.testCanonLevel = true;
        else if (a == "--test-keypad") o.testKeypad = true;   // realistic keypad geometry (KP1-KP6)
        else if (a == "--test-levellint") o.testLevelLint = true;   // GATE A geometric lint
        else if (a == "--test-propclip") o.testPropClip = true;     // GATE A ext: dressing prop clip audit
        else if (a == "--test-canonplay") o.testCanonPlay = true;
        else if (a == "--test-worldswitch") {   // headless canonlevel->flag world-load repro/regression
            if (i + 1 < argc && argv[i + 1][0] != '-') o.worldSwitchTest = argv[++i];
            else o.worldSwitchTest = "streamed";
            o.smoketest = true;   // the harness drives the loop through the smoketest host
        }
        else if (a == "--test-stairnav") o.testStairNav = true;   // feat/stair-nav: enemies path floors via the stairwell
        else if (a == "--test-goldenpath") o.testGoldenPath = true;
        else if (a == "--test-opening") o.testOpening = true;   // opening-flow wake-in-cell contract
        else if (a == "--test-descmech") o.testDescMech = true;   // W9-1 desc-field mechanics (Tier A)
        else if (a == "--test-inventory") o.testInventory = true;       // W9-3 RPG backpack
        else if (a == "--test-progression") o.testProgression = true;   // W9-3 XP/levels
        else if (a == "--test-skilltree") o.testSkillTree = true;       // W9-3 skill tree
        else if (a == "--test-strata") o.testStrata = true;                       // R-3 fold
        else if (a == "--test-elevator-showcase") o.testElevatorShowcase = true;  // R-4 fold
        else if (a == "--screenshot-elevator") {                                  // R-4 beauty trio
            o.elevShot = true; o.worldMode = "elevator-showcase"; o.screenshot = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') o.elevShotDir = argv[++i];
        }   // W5-3 endgame spine
        else if (a == "--test-phase2a") o.testPhase2a = true;
        else if (a == "--test-phase2b") o.testPhase2b = true;
        else if (a == "--test-anim") o.testAnim = true;
        else if (a == "--test-terrain") o.testTerrain = true;
        else if (a == "--test-terrainplace") o.testTerrainPlace = true;
        else if (a == "--test-streaming") o.testStreaming = true;
        else if (a == "--test-ai") o.testAi = true;
        else if (a == "--test-bestiary") o.testBestiary = true;
        else if (a == "--test-bosses") o.testBosses = true;
        else if (a == "--test-adaptive-hide") o.testAdaptiveHide = true;
        else if (a == "--test-canonaliens") o.testCanonAliens = true;
        else if (a == "--test-packspiders") o.testPackSpiders = true;
        else if (a == "--test-clone") o.testClone = true;
        else if (a == "--test-gallery") o.testGallery = true;
        // (chain break — restart the if/else-if ladder so MSVC stays under the
        // C1061 block-nesting limit; flags are exact == matches, all unique, so a
        // matched arg simply falls through the second ladder without re-matching)
        if (false) {}
        else if (a == "--test-act2bosses") o.testAct2Bosses = true;
        else if (a == "--test-spiremid") o.testSpireMid = true;
        else if (a == "--test-nexus") o.testNexus = true;
        else if (a == "--test-spiretop") o.testSpireTop = true;
        else if (a == "--test-timeline") o.testTimeline = true;
        else if (a == "--test-dronehack") o.testDroneHack = true;
        else if (a == "--test-sublevels") o.testSubLevels = true;
        else if (a == "--test-act2") o.testAct2 = true;
        else if (a == "--test-act2desert") o.testAct2Desert = true;
        else if (a == "--test-act2caves") o.testAct2Caves = true;
        else if (a == "--test-rifthub") o.testRifthub = true;
        else if (a == "--test-basis") o.testBasis = true;
        else if (a == "--test-tod") o.testTod = true;
        else if (a == "--test-weather") o.testWeather = true;
        else if (a == "--test-worldregions") o.testWorldRegions = true;
        else if (a == "--test-city") o.testCity = true;
        else if (a == "--test-oceanbase") o.testOceanBase = true;
        else if (a == "--test-doorcode") o.testDoorCode = true;
        else if (a == "--test-hatchcode") o.testHatchCode = true;
        else if (a == "--test-hatch") o.testHatch = true;
        else if (a == "--test-elevator") o.testElevator = true;
        else if (a == "--test-elevatorfsm") o.testElevatorFsm = true;
        else if (a == "--test-net") o.testNet = true;
        else if (a == "--test-netsync") o.testNetSync = true;
        else if (a == "--test-netinterp") o.testNetInterp = true;
        else if (a == "--test-netpredict") o.testNetPredict = true;
        else if (a == "--test-rescue") o.testRescue = true;
        else if (a == "--test-companion-combat") o.testCompanionCombat = true;
        else if (a == "--test-thirdperson") o.testThirdPerson = true;
        else if (a == "--test-npctalk") o.testNpcTalk = true;
        else if (a == "--test-chattree") o.testChatTree = true;
        else if (a == "--test-vigil") o.testVigil = true;
        else if (a == "--test-mission") o.testMission = true;
        else if (a == "--test-destruction") o.testDestruction = true;
        else if (a == "--test-debris") o.testDebris = true;
        else if (a == "--test-gpuskin") o.testGpuSkin = true;
        else if (a == "--test-meshlet") o.testMeshlet = true;
        else if (a == "--test-gpucull") o.testGpuCull = true;
        else if (a == "--test-visunify") o.testVisUnify = true;
        else if (a == "--cullpath" && i + 1 < argc) o.cullPathArg = std::atoi(argv[++i]);
        else if (a == "--hzb") o.hzbArg = 1;
        else if (a == "--vis" && i + 1 < argc) o.visArg = std::atoi(argv[++i]);
        // --velocity: seed r_velocity 1 (per-object motion vectors for TAA/DLSS;
        // default off so the determinism basins stay byte-identical). Equivalent to
        // `--set r_velocity 1`; provided as a first-class flag (deferred cvar #4).
        else if (a == "--velocity") o.cliCVars.emplace_back("r_velocity", "1");
        else if (a == "--test-collapse") o.testCollapse = true;
        else if (a == "--test-physjoint") o.testPhysJoint = true;
        else if (a == "--test-nav") o.testNav = true;
        else if (a == "--test-script") o.testScript = true;
        else if (a == "--test-weapons") o.testWeapons = true;
        else if (a == "--test-lightning-charge") o.testLightningCharge = true;
        else if (a == "--test-vehicle") o.testVehicle = true;
        else if (a == "--test-canonvehicle") o.testCanonVehicle = true;
        else if (a == "--shot-drive") o.shotDrive = true;
        else if (a == "--flashlight-off") o.flashlightOff = true;
        else if (a == "--dusk") o.duskSky = true;
        else if (a == "--day") o.daySky = true;
        else if (a == "--shot-chatter") {
            o.shotChatter = 1;
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9')
                o.shotChatter = (int)std::strtol(argv[++i], nullptr, 10);
        }
        else if (a == "--test-vehparts") o.testVehParts = true;
        else if (a == "--test-ecology") o.testEcology = true;
        else if (a == "--test-crowd") o.testCrowd = true;
        else if (a == "--test-npclife") o.testNpcLife = true;
        else if (a == "--test-hacking") o.testHacking = true;
        else if (a == "--test-waterzap") o.testWaterZap = true;
        else if (a == "--test-sealife") o.testSealife = true;
        else if (a == "--test-alert") o.testAlert = true;
        else if (a == "--screenshot-ecology") {
            o.ecologyShot = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') o.ecologyShotPath = argv[++i];
        }
        else if (a == "--screenshot-crowd") {
            o.crowdShot = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') o.crowdShotPath = argv[++i];
        }
        else if (a == "--screenshot-alert") {
            o.alertShot = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') o.alertShotPath = argv[++i];
        }
        else if (a == "--screenshot-perfshop") {
            o.perfshopShot = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') o.perfshopShotDir = argv[++i];
        }
        else if (a == "--set") {
            // Generic CLI cvar override: --set <cvar> <value> (repeatable).
            // Applied right after the console registers its cvars — the headless
            // A/B debugging workhorse (e.g. --set r_rtreflections 0).
            if (i + 2 < argc) { o.cliCVars.emplace_back(argv[i+1], argv[i+2]); i += 2; }
        }
        else if (a == "--test-footik") o.testFootIk = true;
        else if (a == "--test-ui") o.testUi = true;
        else if (a == "--test-loading") o.testLoading = true;
        else if (a == "--test-saveload") o.testSaveLoad = true;
        else if (a == "--test-dialog") o.testDialog = true;
        // (chain break #2 — see the note above)
        if (false) {}
        else if (a == "--demo-dialog") {
            o.demoDialog = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') o.demoDialogPath = argv[++i];
        }
        else if (a == "--test-valley") o.testValley = true;
        else if (a == "--test-cliffs") o.testCliffs = true;
        else if (a == "--test-club") o.testClub = true;
        else if (a == "--test-gamma") o.testGamma = true;
        else if (a == "--test-complex") o.testComplex = true;
        else if (a == "--screenshot-complex") {                                   // 7-level Complex beauty set
            o.complexShot = true; o.worldMode = "complex"; o.screenshot = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') o.complexShotDir = argv[++i];
        }
        else if (a == "--test-clubnpcs") o.testClubNpcs = true;
        else if (a == "--test-jukebox") o.testJukebox = true;
        else if (a == "--test-listen") o.testListen = true;
        else if (a == "--test-perfshop") o.testPerfshop = true;
        else if (a == "--test-space") o.testSpace = true;
        else if (a == "--test-eva") o.testEva = true;
        else if (a == "--test-ship-ai") o.testShipAi = true;
        else if (a == "--test-targeting") o.testTargeting = true;
        else if (a == "--test-ship-damage") o.testShipDamage = true;
        else if (a == "--width") {
            if (i + 1 < argc) { o.winW = (uint32_t)std::strtoul(argv[++i], nullptr, 10); }
        }
        else if (a == "--height") {
            if (i + 1 < argc) { o.winH = (uint32_t)std::strtoul(argv[++i], nullptr, 10); }
        }
        else if (a == "--world") {
            if (i + 1 < argc && argv[i + 1][0] != '-') { o.worldMode = argv[++i]; o.worldExplicit = true; }
            // `--world fromdoc <path.json>`: an optional second positional token is
            // the LevelDoc to boot (default = the editor's File>Save target).
            if (o.worldMode == "fromdoc" && i + 1 < argc && argv[i + 1][0] != '-')
                o.docWorldPath = argv[++i];
            // `--world spacestation`: the rescued deep-space station scene. A thin
            // alias over the LevelDoc loader — it boots the committed station doc
            // (whose biome "space" drives the deep-space sky + distant-Sol bodies)
            // unless an explicit doc path is given after it.
            if (o.worldMode == "spacestation") {
                if (i + 1 < argc && argv[i + 1][0] != '-') o.docWorldPath = argv[++i];
                else o.docWorldPath = "assets/levels/space_station.leveldoc.json";
            }
        }
        else if (a == "--stress") {
            if (i + 1 < argc) { o.stressCount = (uint32_t)std::strtoul(argv[++i], nullptr, 10); }
        }
        else if (a == "--bench") {
            o.bench = true;
            if (i + 1 < argc) { o.stressCount = (uint32_t)std::strtoul(argv[++i], nullptr, 10); }
            // Optional second positional arg = frame count.
            if (i + 1 < argc && argv[i + 1][0] != '-')
                o.benchFrames = (uint32_t)std::strtoul(argv[++i], nullptr, 10);
        }
        else if (a == "--editor") o.editorMode = true;
        else if (a == "--screenshot-editor") {
            o.editorShot = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') o.editorShotPath = argv[++i];
        }
        else if (a == "--screenshot-loader") {
            o.loaderShot = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') o.loaderShotPath = argv[++i];
        }
        else if (a == "--screenshot-dialog") {
            o.screenshot = true; o.dialogShot = true;
            o.screenshotPath = o.dialogShotPath;
            if (i + 1 < argc && argv[i + 1][0] != '-') { o.dialogShotPath = argv[++i]; o.screenshotPath = o.dialogShotPath; }
        }
        else if (a == "--screenshot-vigil") {
            // W4-2: capture the cell HoloTerminal with a VIGIL conversation live
            // on the glass (scripted tree, orange ink) — the AI's review host.
            o.screenshot = true; o.vigilShot = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') o.screenshotPath = argv[++i];
        }
        else if (a == "--screenshot") {
            o.screenshot = true;
            // Optional path arg (next token, if it isn't another flag).
            if (i + 1 < argc && argv[i + 1][0] != '-') o.screenshotPath = argv[++i];
            // Optional settle-frame count (second positional, if numeric).
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9')
                o.screenshotSettle = (int)std::strtol(argv[++i], nullptr, 10);
        }
        else if (a == "--shot-cam") {
            // Parse "x,y,z,yaw,pitch" into shotCam[]; enables the override.
            // BUGFIX: the guard used to be a bare `argv[i+1][0] != '-'`, which rejected
            // every NEGATIVE camera position — you could not frame a shot anywhere at
            // -X/-Y/-Z (the whole rift chamber, half the tower, most of the planet), and
            // the flag silently fell back to the hero camera instead of saying so. A
            // leading '-' followed by a digit or '.' is a NUMBER, not a flag.
            // (Both integration lines hit this independently — the fold found it via the
            // bodycontact host's rigid-side cam. Same fix, kept as the shared lambda.)
            auto isNumArg = [](const char* s) {
                return s[0] != '-' ||
                       (s[1] >= '0' && s[1] <= '9') || s[1] == '.';
            };
            if (i + 1 < argc && isNumArg(argv[i + 1])) {
                const char* s = argv[++i];
                int n = 0; char* end = nullptr;
                while (n < 5 && *s) {
                    o.shotCam[n++] = std::strtof(s, &end);
                    s = (end && *end == ',') ? end + 1 : end;
                    if (!end || (*end != ',' && *end != '\0')) break;
                }
                o.shotCamOverride = (n == 5);
            }
        }
        else if (a == "--ui-demo" || a == "--screenshot-menu") {
            o.uiDemo = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') o.uiDemoPath = argv[++i];
            // Optional screen keyword: main | pause | settings.
            if (i + 1 < argc && argv[i + 1][0] != '-') o.uiDemoScreen = argv[++i];
        }
        else if (a == "--fx-demo") o.fxDemo = true;
        else if (a == "--fx-lightning") { o.fxDemo = true; o.fxLightning = true; }
        else if (a == "--screenshot-sky") {
            o.skyShot = true;
            // Optional output path arg (next token, if it isn't another flag).
            if (i + 1 < argc && argv[i + 1][0] != '-') o.skyShotPath = argv[++i];
        }
        else if (a == "--screenshot-showroom") {
            o.showroomShot = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') o.showroomShotPath = argv[++i];
        }
        else if (a == "--screenshot-car") {
            o.carShot = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') o.carShotDir = argv[++i];
        }
        else if (a == "--screenshot-upperfloors") {   // R-5: floors 2-7 content proof
            o.upperShot = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') o.upperShotDir = argv[++i];
        }
        else if (a == "--screenshot-rescuerooms") {    // F2 three-captive rescue-room closeups
            o.rescueShot = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') o.rescueShotDir = argv[++i];
        }
        else if (a == "--screenshot-showroom-fp") {
            // Headless first-person proof of the walkable --world showroom. Forces the
            // showroom world on so the SAME build path runs, then renders one frame from
            // the player spawn eye and exits.
            o.showroomFpShot = true;
            o.worldMode = "showroom";
            if (i + 1 < argc && argv[i + 1][0] != '-') o.showroomFpShotPath = argv[++i];
        }
        else if (a == "--screenshot-showroom-ragdoll") {
            // Headless proof of Aria's physics RAGDOLL: same showroom-FP setup, but
            // collapse her + step the world so she falls, then capture one frame.
            o.showroomRagdollShot = true;
            o.worldMode = "showroom";
            if (i + 1 < argc && argv[i + 1][0] != '-') o.showroomRagdollShotPath = argv[++i];
        }
        else if (a == "--screenshot-showroom-deck") {
            // Headless proof: stand on the spire-top glass deck, look out at the night sky.
            o.showroomDeckShot = true;
            o.worldMode = "showroom";
            if (i + 1 < argc && argv[i + 1][0] != '-') o.showroomDeckShotPath = argv[++i];
        }
        else if (a == "--screenshot-showroom-elevator") {
            // Headless proof: glass elevator car parked mid-shaft, camera inside it.
            o.showroomElevShot = true;
            o.worldMode = "showroom";
            if (i + 1 < argc && argv[i + 1][0] != '-') o.showroomElevShotPath = argv[++i];
        }
        else if (a == "--screenshot-showroom-stair") {
            // Headless proof: the entry passage + 90 deg turn + the stairs up to the atrium.
            o.showroomStairShot = true;
            o.worldMode = "showroom";
            if (i + 1 < argc && argv[i + 1][0] != '-') o.showroomStairShotPath = argv[++i];
        }
        else if (a == "--screenshot-showroom-floor2") {
            // Headless proof: standing on the 2nd floor having climbed the synthesized stair.
            o.showroomFloor2Shot = true;
            o.worldMode = "showroom";
            if (i + 1 < argc && argv[i + 1][0] != '-') o.showroomFloor2ShotPath = argv[++i];
        }
        else if (a == "--screenshot-doors") {   // door-mesh-swap visual gate (per floor)
            o.doorShot = true;
            o.worldMode = "canonlevel";
            if (i + 1 < argc && argv[i + 1][0] != '-') o.doorShotDir = argv[++i];
        }
        else if (a == "--screenshot-showroom-door") {
            // Headless proof: the hidden STRUT-FACE door (open by default; closed via env).
            o.showroomDoorShot = true;
            o.worldMode = "showroom";
            if (i + 1 < argc && argv[i + 1][0] != '-') o.showroomDoorShotPath = argv[++i];
        }
        else if (a == "--screenshot-showroom-struts") {
            // Headless EXTERIOR proof: frame the symmetric set of thickened "/" struts.
            o.showroomStrutsShot = true;
            o.worldMode = "showroom";
            if (i + 1 < argc && argv[i + 1][0] != '-') o.showroomStrutsShotPath = argv[++i];
        }
        else if (a == "--screenshot-showroom-gallery") {
            // Headless proof of the HIDDEN ANALYST GALLERY: captures the gallery (terminals
            // + analyst figures) AND a down-through-the-dark-glass view in one run (and an
            // up-from-the-civilian-floor view under X3_SHOWROOM_GALLERY_UP=1).
            o.showroomGalleryShot = true;
            o.worldMode = "showroom";
            if (i + 1 < argc && argv[i + 1][0] != '-') o.showroomGalleryShotPath = argv[++i];
        }
        else if (a == "--screenshot-showroom-civilians") {
            // Headless DAY proof of the CIVILIAN crowd on the ground + 2nd floors:
            // captures a wide ground-floor view (<path>) + a mezzanine view (<path>_mezz.png).
            o.showroomCivShot = true;
            o.worldMode = "showroom";
            if (i + 1 < argc && argv[i + 1][0] != '-') o.showroomCivShotPath = argv[++i];
        }
        else if (a == "--screenshot-planet") {
            o.planetShot = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') o.planetShotPath = argv[++i];
        }
        else if (a == "--screenshot-nightsky") {
            o.nightskyShot = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') o.nightskyShotPath = argv[++i];
        }
        // ---- x3.cutscene/1 args: a FRESH if-chain (not chained onto the giant
        // else-if ladder above — MSVC C1061 nesting limit). Disjoint exact matches,
        // so re-starting the chain is behavior-identical.
        if (a == "--test-cutscene") o.testCutscene = true;
        else if (a == "--test-filmic") o.testFilmic = true;
        else if (a == "--nofilmic") o.noFilmic = true;
        else if (a == "--skipintro") o.skipIntro = true;
        else if (a == "--cutscene") {
            if (i + 1 < argc && argv[i + 1][0] != '-') o.cutsceneFile = argv[++i];
        }
        else if (a == "--cuetime") {
            if (i + 1 < argc) o.cueTime = (float)std::strtod(argv[++i], nullptr);
        }
        else if (a == "--cutscene-shot") {
            o.cutsceneShot = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') o.cutsceneShotPath = argv[++i];
        }
        else if (a == "--screenshot-terrain") {
            o.terrainShot = true;
            // Optional output path arg (next token, if it isn't another flag).
            if (i + 1 < argc && argv[i + 1][0] != '-') o.terrainShotPath = argv[++i];
        }
        else if (a == "--screenshot-ocean") {
            o.oceanShot = true;
            // Optional output path arg (next token, if it isn't another flag).
            if (i + 1 < argc && argv[i + 1][0] != '-') o.oceanShotPath = argv[++i];
        }
        else if (a == "--screenshot-oceanbase") {   // W3-4 undersea base vantage
            o.oceanBaseShot = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') o.oceanBaseShotPath = argv[++i];
        }
        else if (a == "--screenshot-city") {        // W8-3 city establishing shots
            o.cityShot = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') o.cityShotPath = argv[++i];
        }
        else if (a == "--screenshot-matlib") {
            // Surface-library preview: one bay per curated texture set, a closeup
            // per set + overview rows into a FLAT output dir.
            o.matlibShot = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') o.matlibShotDir = argv[++i];
        }
        else if (a == "--test-primlight") {
            // ONE LIGHTING PATH: prim (dielectric) vs GLB (Cook-Torrance) radiance
            // parity on the real device, with a negative control. Optional out-PNG.
            o.testPrimLight = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') o.primLightShotPath = argv[++i];
        }
        else if (a == "--screenshot-destruct") {
            o.destructShot = true;
            // Optional output path arg (next token, if it isn't another flag).
            if (i + 1 < argc && argv[i + 1][0] != '-') o.destructShotPath = argv[++i];
        }
        else if (a == "--capture-ai") {
            o.captureAi = true;
            // Optional output directory arg (next token, if it isn't another flag).
            if (i + 1 < argc && argv[i + 1][0] != '-') o.captureAiDir = argv[++i];
        }
        else if (a == "--capture-crowd-spread") {
            o.captureCrowdSpread = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') o.captureCrowdSpreadDir = argv[++i];
        }
        else if (a == "--capture-walk") {
            o.captureWalk = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') o.captureWalkPath = argv[++i];
            if (i + 1 < argc && argv[i + 1][0] != '-') o.captureWalkRig  = argv[++i];
        }
        else if (a == "--screenshot-footik") {
            o.captureFootIk = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') o.captureFootIkPath = argv[++i];
        }
        else if (a == "--capture-spire") {
            o.captureSpire = true;
            // Optional output directory arg (next token, if it isn't another flag).
            if (i + 1 < argc && argv[i + 1][0] != '-') o.captureSpireDir = argv[++i];
        }
        else if (a == "--capture-wings") {
            o.captureWings = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') o.captureWingsDir = argv[++i];
        }
        else if (a == "--list-clips") {
            o.listClips = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') o.listClipsPath = argv[++i];
        }
        else if (a == "--test-locomotion") {
            o.testLocomotion = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') o.testLocomotionPath = argv[++i];
        }
        else if (a == "--test-intro") o.testIntro = true;
        else if (a == "--test-introorch") o.testIntroOrch = true;
        else if (a == "--test-introcockpit") o.testIntroCockpit = true;
        else if (a == "--test-shipinterior") o.testShipInterior = true;
        else if (a == "--test-shipwindows") o.testShipWindows = true;
        else if (a == "--test-bodycontact") o.testBodyContact = true;
        else if (a == "--test-wormhole") o.testWormhole = true;
        else if (a == "--test-wormhole-transit") o.testWormholeTransit = true;
        else if (a == "--test-tractor") o.testTractor = true;
        else if (a == "--test-descentslide") o.testDescentSlide = true;
        else if (a == "--test-wingdressing") o.testWingDressing = true;
        else if (a == "--test-introbranch") o.testIntroBranch = true;
        else if (a == "--test-surfacestart") o.testSurfaceStart = true;
        // [P0-1] both spellings accepted (the plan doc names --test-surface-handoff).
        else if (a == "--test-surfacehandoff" || a == "--test-surface-handoff")
            o.testSurfaceHandoff = true;
        else if (a == "--test-starsystems") o.testStarsystems = true;
        else if (a == "--intro-force") {
            // DEV: force the interactive-intro outcome branch.
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                const std::string v = argv[++i];
                if      (v == "escaped"   || v == "escape") o.introForce = 1;
                else if (v == "shot_down" || v == "shotdown" || v == "cell") o.introForce = 0;
                // The earned third branch: kill the dreadnought -> it crashes ->
                // Act-1 starts at the wreck (salvage, prisoners, Lab Zero breach).
                else if (v == "capital_killed" || v == "kill" || v == "wreck")
                    o.introForce = 2;
            }
        }
    }
}

}} // namespace x3::apphost
