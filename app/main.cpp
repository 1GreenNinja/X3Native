// X3Engine host — opens a window, brings up the render device + physics, builds
// the S2 graybox test level, and runs the loop with a fly camera. Walking is S3.

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#include "engine/core/x3_log.h"
#include "engine/core/x3_boot.h"   // [boot] timeline (boot-to-interactive instrumentation + --test-boottime)
#include "engine/core/IConsole.h"
#include "engine/core/IJobSystem.h"
#include "engine/rhi/IRenderDevice.h"
#include "engine/rhi/FrustumCull.h"          // CPU per-object frustum cull (--test-frustumcull)
#include "engine/rhi/GpuCull.h"           // D15 GPU culling — meshlet builder self-test (--test-meshlet)
#include <glm/gtc/matrix_transform.hpp>       // glm::perspective/lookAt/translate/scale (--test-frustumcull)
#include "engine/asset/IAssetSource.h"
#include "engine/physics/IPhysicsWorld.h"
#include "engine/physics/Destruction.h"   // K-T0/T1 destructibles + --test-destruction
#include "engine/physics/StructuralCollapse.h" // K-T3 support-graph collapse + --test-collapse
#include "engine/physics/Ragdoll.h"       // Physics §2 ragdoll+blend: --test-ragdoll + --world ragdoll
#include "engine/physics/IVehicle.h"      // vehicle framework: --test-vehicle + --world drive/boat/fly
#include "engine/asset/IModelLoader.h"
#include "engine/audio/IAudioSystem.h"
#include "engine/audio/RtAcoustics.h"   // RT ACOUSTICS: audio rays through the render TLAS (+ --test-acoustics)
#include "engine/net/INetworkSystem.h"   // netcode Phase 0: --test-net + SimClock
#include "engine/net/SimClock.h"         // deterministic fixed-step accumulator
#include "engine/net/ISnapshotInterpolator.h"  // netcode Phase 0c: --test-netinterp
#include "engine/net/IClientPredictor.h"        // netcode Phase 1: --test-netpredict
#include "engine/ai/INavigation.h"       // GENERAL navigation: nav grid + A* + --test-nav
#include "engine/script/IScriptSystem.h" // D14 Lua scripting: pak-shipped behavior + --test-script
#include "engine/llm/ILlmSystem.h"       // in-engine LLM (living NPC minds) + --test-llm

#include "scene.h"
#include "mesh_prims.h"
#include "asset_root.h"                    // portable assetRoot() (assets-LFS)
#include "asset_manifest_check.h"          // fleet asset-store manifest boot check (Phase A)
#include "audio_root.h"                    // portable resolveAudio() (D: mirror / G: packs)
#include "anim.h"                          // Skinner + --list-clips clip check
#include "thirdperson.h"                    // FP/3P toggle + Jake avatar + held weapon (--test-thirdperson)
#include "level1.h"
#include "level_loader.h"                   // data-driven canonical level loader + per-room PVS cull (--test-canonlevel)
#include "leveldoc_world.h"                  // EDITOR LevelDoc loader: --world fromdoc + hot reload (--test-loader)
#include "player.h"
#include "monster.h"
#include "level1_game.h"
#include "canon_play.h"                     // --world canonlevel gameplay (sidearm + animated enemies + Martinez + girls)
#include "intro_coldopen.h"                  // --world intro / default lead-in cold-open (shot-down -> captured)
#include "cutscene.h"                        // x3.cutscene/1 data-driven cutscene system (the COLD OPEN film)
#include "npc_dialog.h"                     // rescued-NPC talk/dialog -> companion (the captive girl)
#include "chat_tree.h"                      // x3.chattree/1 data-driven dialog runner (--test-chattree)
#include "mission.h"                        // x3.mission/1 data-driven mission runner (--test-mission, g_missiondoc)
#include "physprops.h"                      // FEATURE_GOALS §1: hanging cubes / joints (ragdoll foundation)
#include "ragdoll.h"                        // FEATURE_GOALS §2: physics death ragdoll
#include "editor/editor.h"                  // native Level Editor E1 (brain + self-test)
#include "editor/editor_host.h"             // Level Architect editor host (shell + blockout)
#include "barrels.h"                        // explosive barrels (shoot -> chain explosion)
#include "glass_test.h"                      // translucent-glass material (--test-glass)
#include "holo_terminal.h"                  // Jake's cell holographic terminal (text + input)
#include "secret_room.h"                    // code-locked trapdoor -> stocked secret room
#include "headless_device.h"                // shared no-op IRenderDevice (--test-hatch)
#include "engine/ecs/Ecs.h"                 // sparse-set ECS core (10k+ entities)
#include "ecs_render.h"                     // ECS -> GPU-driven render feed
#include "spire_mid.h"                      // EFLZ Spire F3/F4/F5 mid-floor content
#include "spire_top.h"                      // EFLZ Spire F6/F7 top-floor content (Act-1 finale)
#include "spire_nexus.h"                    // EFLZ Floor 4.5 Nexus Chamber / The Chorus (off-elevator boss)
#include "spire_sublevels.h"                // EFLZ hidden Floor-7 sub-levels + Dr. Chen Return Mission
#include "timeline.h"                        // EFLZ morality/timeline backbone for the 12 endings (--test-timeline)
#include "act2_world.h"                      // EFLZ Act-2 open-world surface host + L8/L9 (--test-act2)
#include "act2_desert.h"                     // EFLZ Act-2 desert depths + Salvari camp L10/L11 (--test-act2desert)
#include "act2_caves.h"                      // EFLZ Act-2 mid biomes L12-15 (--test-act2caves)
#include "tod.h"                             // EFLZ Time-of-Day cycle (sky/sun via SkyParams — --test-tod)
#include "weather.h"                         // EFLZ Weather (7 states, biome-gated, hazard — --test-weather)
#include "world_regions.h"                   // EFLZ open-world surrounding regions + 4 mountain ranges (--test-worldregions)
#include "world_stream.h"                    // SEAMLESS region-graph streaming (--world streamed / --test-worldstream)
#include "world_map.h"                       // INTERACTIVE WORLD MAP (M key / --test-worldmap)
#include "city.h"                            // EFLZ open-world metropolis: districts + roads + freeway tunnels (--test-city)
#include "ocean_base.h"                      // EFLZ open-world ocean + undersea base + submarine combat (--test-oceanbase)
#include "elevator.h"
#include "club1127.h"
#include "env_art.h"                       // EnvArtSystem::buildFromGlb (--screenshot-showroom)
#include "valley.h"                          // Crystal Valleys (Act 2, L15 — --world valley)
#include "cliffs.h"                          // Salvari cliffs finale (--world cliffs)
#include "terrain.h"
#include "fx.h"
#include "hud.h"
#include "ui.h"                              // GENERAL game-UI: menus + production HUD + --test-ui
#include "loading_screen.h"                  // EFLZ boot/world-load screen + --test-loading (Task #49)
#include "save.h"                            // GENERAL versioned checkpoint save/load + --test-saveload
#include "dialog.h"                          // AI-dialog + TTS voice on skinned NPCs (§3) + --test-dialog
#include "stress.h"
#include "destruct_demo.h"                 // K-T1 destruction demo (--world destruct)
#include "ragdoll_demo.h"                  // Physics §2 ragdoll demo (--world ragdoll) + blend check
#include "vehicle.h"                       // vehicle demo worlds (--world drive/boat/fly)
#include "vehparts.h"                      // performance-parts catalog + build composition (--test-vehparts)
#include "perfshop.h"                      // the drive-in performance shop (--world drive)
#include "ecology.h"                       // AMBIENT ECOLOGY: grazers/predators/patrols (--test-ecology)
#include "crowd.h"                         // CROWDS: club dancers + facility civilians (--test-crowd)
#include "alert.h"                         // FACILITY ALERT LEVEL: the wanted system (--test-alert)
#include "host_context.h"                  // #28 deep split: shared live-state struct for the --world hosts
#include "world_hosts.h"                   // #28 deep split: dispatchWorldHost() + the extracted host TUs
#include "screenshot_hosts.h"              // #28 deep split: dispatchScreenshotHosts() (headless capture handlers)
#include "surface_library.h"               // ART_BIBLE §4: curated texture-set preview (--screenshot-matlib)
#include "app_run.h"                       // #28 deep split: runDefaultHost() (the interactive render loop)
#include "cli.h"                           // #28 deep split: CliOptions + parseCli() (the arg-parse loop)
#include "settings_io.h"                   // #28 deep split: window/audio settings persistence (shared)
#include "input_globals.h"                 // #28 deep split: g_weaponScroll + scrollCallback (shared w/ streamed host)

#include <memory>
#include <string_view>
#include <string>
#include <cmath>
#include <vector>
#include <unordered_map>   // per-weapon fire-sound cache (name -> SoundHandle)
#include <cstdint>
#include <cstdlib>
#include <cstring>    // std::strcmp (showroom planet name match)
#include <algorithm>
#include <filesystem>
#include <cstdio>
#include <thread>     // r_maxfps frame limiter
#include <future>     // boot-time async audio bring-up (docs/BOOT_TIME.md)
#include <chrono>
#include <fstream>    // window-size settings persistence (SET AS DEFAULT)

// (The vendored gif.h + a file-local stb_image copy moved with the --capture-ai
//  handler to app/screenshot_hosts.cpp — that TU is now the SOLE non-inline gif.h
//  includer; #28 split Phase B. main.cpp no longer reads PNGs back / writes GIFs.)

namespace {
// Approximate the viewmodel muzzle in world space from the eye + look angles, so
// the FX tracer starts near the gun barrel (lower-right of the view) rather than
// dead center. Mirrors the camera-basis offsets used by WeaponSystem; tuned to
// sit just in front of and below/right of the eye.

// COLD-OPEN CINEMATIC + NIGHT-SKY PLANETS — moved VERBATIM to app/cinematic.
// {h,cpp} (#28 monolith split). using-declarations keep every call site in the
// world hosts below unqualified (byte-identical bodies).
} // close the anon namespace briefly to include the cinematic header
#include "cinematic.h"
#include "showroom_tod.h"   // SHOWROOM DAY/NIGHT helpers (shared with the --world showroom host)
namespace {
using x3::apphost::applyShowroomTimeOfDay;   // moved to showroom_tod.h (#28 split)
using x3::apphost::showroomDayDefault;       // moved to showroom_tod.h (#28 split)
using x3::apphost::NightSkyPlanet;
using x3::apphost::kNightSkyDist;
using x3::apphost::loadNightSkyPlanets;
using x3::apphost::drawNightSkyPlanets;
using x3::apphost::CinActorState;
using x3::apphost::CinematicScene;
using x3::apphost::CinAudioMap;
using x3::apphost::runCutsceneWindowed;

// ---- SHOWROOM DAY<->NIGHT lighting STATES (one helper, two looks) -----------
// The --world showroom (and its headless proofs) drive their sky/sun/ambient/
// bloom/interior-point-lights through this ONE helper so DAY and NIGHT are a
// single switch.  ADDITIVE: NIGHT reproduces the exact values the showroom has
// always used (dark planet sky + dim cool moon + full interior point lights);
// the planet draw + setSkyTime wheeling are gated to NIGHT by the CALLER.
//
//   DAY  — match the Unity ShowRoom_Vol30 interior (bright, cool, high-key white):
//     * Sun from the Unity HDRP directional light Rotation X=69.31, Y=9.7, Z=0.
//       Unity light forward = R_y(9.7)*R_x(69.31)*(0,0,1) = (0.0595,-0.9355,0.3483);
//       sunDir (TOWARD the sun) = -forward = (-0.0595, 0.9355, -0.3483) — a high
//       winter-midday sun, ~69 deg elevation, azimuth ~10 deg.  Short soft shadows.
//     * Bright sky: pale winter-blue zenith, warm-grey horizon haze, exposure ~1.
//     * setAmbient BRIGHT cool snow-bounce — the DOMINANT fill (high-key, no hard
//       blacks); this is what makes the interior read bright/cool like Unity 013904.
//     * setBloom low (~0.12).  Interior point lights DIMMED (x0.3) — snow-bounce
//       carries the room by day, not the fixtures.
//
// `interiorLights` (nullable) holds the FULL-intensity NIGHT point lights (color
// already pre-multiplied by intensity).  DAY pushes a x0.3-scaled copy; NIGHT
// pushes them unchanged.  Pass nullptr for paths with no interior lights (the
// exterior --screenshot-showroom).  Returns nothing; SSAO/GI are untouched.
// (applyShowroomTimeOfDay + showroomDayDefault moved VERBATIM to showroom_tod.h
//  for the #28 split — shared with app/world_hosts/host_showroom.cpp; main()'s
//  call sites stay unqualified via the using-declarations above.)

// ---- ON-GLASS HOLO-TERMINAL readout (large, high-contrast, fit to the panel) ----
// Project the cell HoloTerminal panel center + top edge, then lay out the boot
// readout (and, while typing, the input line) as LARGE proportional-font text sized
// to fit WITHIN the glass: the body size auto-shrinks so the widest line spans ~92%
// of the projected panel width, so it never overflows the bezel yet stays as big as
// possible. Bright cyan-white over the glowing blue glass with a dark drop shadow.
// Purely additive 2D HUD draws; safe to call from both the interactive loop and the
// --screenshot capture. `anchor` is the panel center; `showInput` adds the prompt.

// ---- Settings persistence in %LOCALAPPDATA%\x3native_settings.cfg ----
// readWindowSize() overrides winW/winH at startup (returns true if a saved size exists,
// so the host skips maximize-by-default); readAudioSettings() seeds the music/SFX
// state; writeSettings() (the menu "SET AS DEFAULT" action) persists window size +
// audio together. Plain key=value text; a missing/garbled file/key is simply ignored.
// (x3SettingsPath / readWindowSize / readAudioSettings / writeSettings moved to
//  settings_io.h for the #28 split — shared with app/app_run.cpp. main()'s
//  prelude still uses readWindowSize via the using-declarations below.)
using x3::apphost::x3SettingsPath;
using x3::apphost::readWindowSize;
using x3::apphost::readAudioSettings;
using x3::apphost::writeSettings;

// Bundle passed to GLFW via the window user-pointer so the char/key callbacks
// can route text input into the on-screen console.

// ---------------------------------------------------------------------------
// SpeakingMonster — the HOST dialog->skinned-NPC adapter — moved VERBATIM to
// app/speaking_monster.h (#28 monolith split). Declared in x3::apphost; the
// `using` below keeps the --demo-dialog dispatch AND the --world host call
// sites in main() unqualified so the bodies are byte-identical.
// ---------------------------------------------------------------------------
} // namespace
#include "speaking_monster.h"
using x3::apphost::SpeakingMonster;
#include "test_registry.h"   // x3::apphost::TestFlags + dispatchTests (#28 split)

// ===========================================================================
// D14 SCRIPT BOOT + GAME BINDINGS — moved VERBATIM to app/bindings.{h,cpp}
// (#28 monolith split). Declared in x3::apphost; the call sites below stay
// unqualified via the using-declarations so the bodies are byte-identical.
// ===========================================================================
#include "bindings.h"
using x3::apphost::loadBootScripts;
using x3::apphost::registerGameBindings;
using x3::apphost::submitTerminalToScripts;

// HOST SELF-TESTS — moved VERBATIM to app/self_tests.{h,cpp} (#28 monolith
// split). using-declarations keep the --test-* dispatch call sites unqualified
// (byte-identical bodies).
#include "self_tests.h"
using x3::apphost::runFrustumCullSelfTest;
using x3::apphost::runGpuCullSelfTest;
using x3::apphost::runDebrisSelfTest;
using x3::apphost::runGpuSkinSelfTest;
using x3::apphost::runHatchChainSelfTest;


int main(int argc, char** argv) {
    // ---- CLI: parse argv into CliOptions (app/cli.{h,cpp}; #28 Phase D) ----
    x3::apphost::CliOptions o;
    x3::apphost::parseCli(argc, argv, o);

    // Fleet asset-store manifest check (Phase A, docs/ASSET_DISTRIBUTION.md):
    // auto-fetch any manifest asset missing locally (D: cache -> G: share), or
    // log ONE line telling the dev to run `python tools/asset_store.py fetch
    // --all`. No-op when assets/manifest.json is absent. Never blocks boot.
    x3::game::checkAssetManifest();

    // ---- HEADLESS --test-*/--demo-*/--list-clips DISPATCH ----------------------
    // The long `if (testX) return runX()?0:1;` ladder was moved VERBATIM to
    // app/test_registry.cpp (#28 monolith split). Populate the flag struct from
    // the locals above (1:1 names) and dispatch; a non-negative result is this
    // program's exit code, -1 means "no test flag set" so boot continues.
    {
        x3::apphost::TestFlags _tf;
        _tf.testJobs = o.testJobs;
        _tf.testAsset = o.testAsset;
        _tf.testConsole = o.testConsole;
        _tf.testPhysics = o.testPhysics;
        _tf.testPhysJoint = o.testPhysJoint;
        _tf.testRagdoll = o.testRagdoll;
        _tf.testGltf = o.testGltf;
        _tf.testPlayer = o.testPlayer;
        _tf.testInteract = o.testInteract;
        _tf.testPhysprops = o.testPhysprops;
        _tf.testRagdollSkin = o.testRagdollSkin;
        _tf.testEditor = o.testEditor;
        _tf.testBlockout = o.testBlockout;
        _tf.testLoader = o.testLoader;
        _tf.testBarrels = o.testBarrels;
        _tf.testGlass = o.testGlass;
        _tf.testFrustumCull = o.testFrustumCull;
        _tf.testHoloterm = o.testHoloterm;
        _tf.testSecretRoom = o.testSecretRoom;
        _tf.testHatch = o.testHatch;
        _tf.testLlm = o.testLlm;
        _tf.testEcs = o.testEcs;
        _tf.testEcsRender = o.testEcsRender;
        _tf.testPickup = o.testPickup;
        _tf.testCombat = o.testCombat;
        _tf.testDeathRagdoll = o.testDeathRagdoll;
        _tf.testAudio = o.testAudio;
        _tf.testAcoustics = o.testAcoustics;
        _tf.testLevel1 = o.testLevel1;
        _tf.testCanonLevel = o.testCanonLevel;
        _tf.testKeypad = o.testKeypad;
        _tf.testLevelLint = o.testLevelLint;
        _tf.testCanonPlay = o.testCanonPlay;
        _tf.testGoldenPath = o.testGoldenPath;
        _tf.testStrata = o.testStrata;
        _tf.testElevatorShowcase = o.testElevatorShowcase;
        _tf.testIntro = o.testIntro;
        _tf.testIntroOrch = o.testIntroOrch;
        _tf.testIntroBranch = o.testIntroBranch;
        _tf.testSurfaceStart = o.testSurfaceStart;
        _tf.testCutscene = o.testCutscene;
        _tf.testPhase2a = o.testPhase2a;
        _tf.testPhase2b = o.testPhase2b;
        _tf.testAnim = o.testAnim;
        _tf.testLocomotion = o.testLocomotion;
        _tf.listClips = o.listClips;
        _tf.testTerrain = o.testTerrain;
        _tf.testTerrainPlace = o.testTerrainPlace;
        _tf.testStreaming = o.testStreaming;
        _tf.testWorldStream = o.testWorldStream;
        _tf.testWorldMap = o.testWorldMap;
        _tf.testAi = o.testAi;
        _tf.testBestiary = o.testBestiary;
        _tf.testBosses = o.testBosses;
        _tf.testAdaptiveHide = o.testAdaptiveHide;
        _tf.testAct2Bosses = o.testAct2Bosses;
        _tf.testCanonAliens = o.testCanonAliens;
        _tf.testSpireMid = o.testSpireMid;
        _tf.testNexus = o.testNexus;
        _tf.testSpireTop = o.testSpireTop;
        _tf.testTimeline = o.testTimeline;
        _tf.testDroneHack = o.testDroneHack;
        _tf.testSubLevels = o.testSubLevels;
        _tf.testTod = o.testTod;
        _tf.testWeather = o.testWeather;
        _tf.testAct2 = o.testAct2;
        _tf.testAct2Desert = o.testAct2Desert;
        _tf.testAct2Caves = o.testAct2Caves;
        _tf.testWorldRegions = o.testWorldRegions;
        _tf.testCity = o.testCity;
        _tf.testOceanBase = o.testOceanBase;
        _tf.testDoorCode = o.testDoorCode;
        _tf.testHatchCode = o.testHatchCode;
        _tf.testElevator = o.testElevator;
        _tf.testElevatorFsm = o.testElevatorFsm;
        _tf.testNet = o.testNet;
        _tf.testNetSync = o.testNetSync;
        _tf.testNetInterp = o.testNetInterp;
        _tf.testNetPredict = o.testNetPredict;
        _tf.testRescue = o.testRescue;
        _tf.testThirdPerson = o.testThirdPerson;
        _tf.testNpcTalk = o.testNpcTalk;
        _tf.testChatTree = o.testChatTree;
        _tf.testMission = o.testMission;
        _tf.testDestruction = o.testDestruction;
        _tf.testDebris = o.testDebris;
        _tf.testGpuSkin = o.testGpuSkin;
        _tf.testMeshlet = o.testMeshlet;
        _tf.testGpuCull = o.testGpuCull;
        _tf.testVisUnify = o.testVisUnify;
        _tf.testCollapse = o.testCollapse;
        _tf.testNav = o.testNav;
        _tf.testWeapons = o.testWeapons;
        _tf.testScript = o.testScript;
        _tf.testVehicle = o.testVehicle;
        _tf.testVehParts = o.testVehParts;
        _tf.testEcology = o.testEcology;
        _tf.testCrowd = o.testCrowd;
        _tf.testAlert = o.testAlert;
        _tf.testFootIk = o.testFootIk;
        _tf.testUi = o.testUi;
        _tf.testLoading = o.testLoading;
        _tf.testSaveLoad = o.testSaveLoad;
        _tf.testDialog = o.testDialog;
        _tf.demoDialog = o.demoDialog;
        _tf.testValley = o.testValley;
        _tf.testCliffs = o.testCliffs;
        _tf.testClub = o.testClub;
        _tf.testSpace = o.testSpace;
        _tf.testEva = o.testEva;
        _tf.testShipAi = o.testShipAi;
        _tf.testTargeting = o.testTargeting;
        _tf.testShipDamage = o.testShipDamage;
        _tf.testLocomotionPath = o.testLocomotionPath;
        _tf.listClipsPath = o.listClipsPath;
        _tf.demoDialogPath = o.demoDialogPath;
        int _rc = x3::apphost::dispatchTests(_tf);
        if (_rc >= 0) return _rc;
    }

    x3::logInfo("X3Engine starting...");
    x3::boot::mark("static init + args");

    // --test-boottime gates the CANONICAL world (canonlevel — the data-driven
    // Floor 1, the game's true level) unless a --world was given explicitly. The
    // legacy hand-coded tower (--world level1) builds 5x the entity count and has
    // an honest boot floor of ~3.2 s — gate it explicitly with a budget arg, e.g.
    // `--test-boottime 4000 --world level1` (see docs/BOOT_TIME.md).
    if (o.testBootTime && !o.worldExplicit) {
        o.worldMode = "canonlevel";
        x3::logInfo("boottime: no --world given — gating the canonical world (canonlevel)");
    }

    // ---- BOOT MANIFEST (docs/BOOT_TIME.md): everything the cell worlds (default
    // Level 1 / elevator / canonlevel / intro) load at build time. Built once,
    // used twice: (1) prewarmModelDecodesAsync RIGHT NOW — pure-CPU stb decodes on
    // background threads, fully overlapped with the ~1 s Vulkan driver init below;
    // (2) preloadModelsAsync after the device exists — parallel full loads that
    // consume those decodes and warm the model/texture caches, so the serial world
    // build takes cache hits. Missing files are skipped silently (superset).
    std::vector<std::pair<std::string, std::string>> bootManifest;
    {
        const bool legacyCell = (o.worldMode == "level1") || (o.worldMode == "elevator");
        const bool canonCell  = (o.worldMode == "canonlevel") || (o.worldMode == "intro");
        if (legacyCell || canonCell) {
            const std::string rig = x3::game::riggedGlbRoot();
            const std::string cvt = x3::game::convertedGlbRoot();
            // Common to BOTH cell builds (canon Floor 1 + the legacy tower).
            for (const char* f : { "marcus_webb_anim.glb", "alien_crawler_anim.glb",
                                   "chief_martinez.glb",
                                   "AnnaCasual.glb", "AnnaBodySuit.glb", "AnnaTactical.glb",
                                   "Jake_22_actions.glb",
                                   "WeaponEnergyPistol.glb", "WeaponEnergyPistol2.glb",
                                   "WeaponRailgun.glb", "WeaponShotgun2.glb",
                                   "WeaponBFG.glb", "WeaponRocketLauncher.glb" })
                bootManifest.emplace_back(rig, f);
            bootManifest.emplace_back(cvt, "Characters/Drone.glb");
            bootManifest.emplace_back(cvt, "ModularSciFi_Interior/SM_Door_A.glb");
            // Legacy tower only (Spire floors + env art + warehouse props).
            if (legacyCell) {
                for (const char* f : { "chief_martinez_anim.glb", "Oracle.glb" })
                    bootManifest.emplace_back(rig, f);
                for (const char* f : { "ModularSciFi_Interior/SM_DoorFrame_A.glb",
                                       "ModularSciFi_Interior/SM_Wall_A.glb",
                                       "ModularSciFi_Interior/SM_Floor_A.glb",
                                       "ModularSciFi_Interior/SM_Ceiling_A.glb",
                                       "ModularSciFi_Interior/SM_Light_A.glb",
                                       "ModularSciFi_Interior/SM_Pipes_A.glb",
                                       "ModularSciFi_Interior/SM_Console.glb",
                                       "SciFi_Warehouse_Kit/Barrel.glb",
                                       "SciFi_Warehouse_Kit/Crate Long.glb",
                                       "SciFi_Warehouse_Kit/Crate Short.glb",
                                       "SciFi_Warehouse_Kit/Fusebox 01.glb",
                                       "SciFi_Warehouse_Kit/Pallet.glb" })
                    bootManifest.emplace_back(cvt, f);
            }
            x3::asset::prewarmModelDecodesAsync(bootManifest);
            x3::boot::mark("decode prewarm kicked (async)");
        }
    }

    // HEADLESS / OFFSCREEN routing: the non-interactive verification + screenshot
    // paths (--smoketest, --screenshot, --screenshot-sky, --screenshot-terrain)
    // render fully offscreen — NO GLFW window, NO surface, NO swapchain, nothing
    // shown on screen. Everything a human actually watches (no-arg game,
    // --world terrain, --bench) keeps a real window + swapchain exactly as before.
    if (o.perfshopShot) o.worldMode = "drive";   // the shop lives in the drive world
    if (o.ecologyShot)  o.worldMode = "valley";  // the ambient ecology rides the valley biome
    if (o.crowdShot)    o.worldMode = "club";    // the crowd proof lives on the club floor
    if (o.alertShot) { o.screenshot = true; o.screenshotPath = o.alertShotPath; }   // rides --screenshot
    const bool headless = o.smoketest || o.testFramePacing || o.screenshot || o.skyShot || o.ddgiShot || o.showroomShot || o.carShot || o.upperShot || o.showroomFpShot || o.showroomRagdollShot || o.showroomDeckShot || o.showroomElevShot || o.showroomStairShot || o.showroomFloor2Shot || o.showroomDoorShot || o.showroomStrutsShot || o.showroomGalleryShot || o.showroomCivShot || o.planetShot || o.nightskyShot || o.cutsceneShot || o.terrainShot || o.oceanShot || o.oceanBaseShot || o.matlibShot || o.captureAi || o.captureWalk || o.destructShot || o.captureFootIk || o.uiDemo || o.captureSpire || o.editorShot || o.loaderShot || o.perfshopShot || o.ecologyShot || o.crowdShot;

    if (!glfwInit()) {
        x3::logError("glfwInit failed");
        return 1;
    }
    // No OpenGL/GLES context — we drive Vulkan ourselves.
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

    // Headless capture/screenshot resolution is FIXED at 1280x720 (so offscreen
    // output stays byte-stable regardless of any --width/--height flags). The
    // visible window uses the configurable winW/winH (default 1280x720), guarded
    // to a sane minimum so a typo can't create a 0-size surface.
    constexpr uint32_t kHeadlessW = 1280, kHeadlessH = 720;
    if (o.winW < 320)  o.winW = 320;
    if (o.winH < 240)  o.winH = 240;
    const uint32_t W = headless ? kHeadlessW : o.winW;
    const uint32_t H = headless ? kHeadlessH : o.winH;
    // In headless mode we create NO window (no glfwCreateWindow at all). GLFW is
    // still initialized (cheap; some paths poll events) but never opens a surface.
    GLFWwindow* window = nullptr;
    if (!headless) {
        // NO maximize-by-default (per Tim): open windowed at winW x H (or the saved
        // "SET AS DEFAULT" size). Fullscreen is opt-in via the settings checkbox.
        window = glfwCreateWindow(static_cast<int>(W), static_cast<int>(H),
                                  "X3Engine", nullptr, nullptr);
        if (!window) {
            x3::logError("glfwCreateWindow failed");
            glfwTerminate();
            return 1;
        }
        x3::logInfo("window: " + std::to_string(W) + "x" + std::to_string(H));
    } else {
        x3::logInfo("headless mode: rendering offscreen (no window / no swapchain) at "
                    + std::to_string(W) + "x" + std::to_string(H));
    }
    x3::boot::mark("glfw init + window");

    // BOOT-TIME: overlap the audio bring-up (miniaudio device + the WAV loads,
    // ~80-100 ms) with the Vulkan device init below. Launched ONLY for the
    // windowed worlds that actually reach the shared audio block (the headless
    // capture paths and the self-contained demo worlds early-return before it and
    // must not leak a live audio device). Joined at the original audio spot.
    // BootAudio struct + makeBootAudio() moved to boot_audio.h (#28 split) so the
    // default host (app/app_run.cpp) shares the same definition; main() launches
    // the async here and hands the future to the host.
    using x3::apphost::BootAudio;
    using x3::apphost::makeBootAudio;
    std::future<BootAudio> bootAudioFut;
    {
        const bool sharedAudioWorld =
            (o.worldMode == "level1") || (o.worldMode == "elevator") ||
            (o.worldMode == "canonlevel") || (o.worldMode == "intro") ||
            (o.worldMode == "terrain") || (o.worldMode == "ocean");
        if (!headless && sharedAudioWorld)
            bootAudioFut = std::async(std::launch::async, makeBootAudio);
    }

    // ---- Render device ----
    std::unique_ptr<x3::rhi::IRenderDevice> device(x3::rhi::createRenderDevice());

    x3::rhi::DeviceDesc desc{};
    // BOOT-TIME: kick the async GLB warmup the moment the device's upload path is
    // live (mid-init, right after the core graphics objects) so it overlaps the
    // remaining ~300 ms of device init. Joined before the world build.
    struct PreloadCtx {
        x3::rhi::IRenderDevice* dev;
        const std::vector<std::pair<std::string, std::string>>* manifest;
    } preloadCtx{ device.get(), &bootManifest };
    if (!bootManifest.empty()) {
        desc.onUploadReady = [](void* u) {
            auto* c = static_cast<PreloadCtx*>(u);
            // Open the upload-batch window NOW so the preload threads record into
            // the shared batch (the later world-build beginUploadBatch is a no-op).
            c->dev->beginUploadBatch();
            x3::asset::preloadModelsAsync(c->dev, *c->manifest);
            x3::logInfo("[boot] GLB preload kicked (mid device-init, async)");
        };
        desc.onUploadReadyUser = &preloadCtx;
    }
    desc.nativeWindowHandle = window ? glfwGetWin32Window(window) : nullptr;
    desc.width  = W;
    desc.height = H;
    desc.headless = headless;
    desc.ssaa = (o.showroomShot || o.carShot || o.showroomFpShot || o.showroomRagdollShot || o.showroomDeckShot || o.showroomElevShot || o.showroomStairShot || o.showroomFloor2Shot || o.showroomDoorShot || o.showroomStrutsShot || o.showroomGalleryShot || o.showroomCivShot || o.planetShot || o.nightskyShot || o.cutsceneShot) ? 4u : 1u;   // 4x supersample the showroom / planet / nightsky still (5090 headless: ~16 samples/px, pristine)
    // Benchmark mode runs with vsync OFF so it measures the true frame ceiling,
    // not the display refresh cap.
    desc.vsync  = !o.bench;
#ifdef _DEBUG
    desc.validation = true;
#else
    desc.validation = false;
#endif

    if (!device->init(desc)) {
        x3::logError("render device init failed");
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    // --legacypost: A/B switch — disable the post-stack additions (auto-exposure +
    // TAA; and with --legacypost2 also bloom + ACES->passthrough) so any path,
    // headless screenshots included, can be compared against the pre-post-stack
    // renderer. --notaa: disable ONLY TAA (jitter fully off + resolve skipped) so
    // before/after captures isolate the TAA contribution. Both also pin the cvars
    // so the interactive per-frame cvar sync doesn't re-enable the feature.
    if (o.legacyPost || o.noTaa) {
        x3::rhi::IRenderDevice::PostFXParams px{};
        if (o.legacyPost) {
            px.autoExposure = false;             // legacy = no eye adaptation
            px.taa = false;                      // legacy = no TAA jitter/resolve
            if (o.legacyPost > 1) { px.bloomEnabled = false; px.tonemapMode = 0; }
        }
        if (o.noTaa) px.taa = false;
        device->setPostFX(px);
        // Reflections ride the TAA history, so TAA-off already disables them in
        // the device; push an explicit OFF too so the A/B state is unambiguous.
        device->setReflectionParams(x3::rhi::IRenderDevice::ReflectionParams{});
        // (The interactive path additionally pins the matching cvars right after
        // the console exists, so the per-frame cvar sync can't re-enable these.)
    }
    // --norefl: A/B switch — reflections OFF with TAA (and everything else) left
    // at defaults, so before/after captures isolate exactly the SSR/RT-reflection
    // contribution (the post stack + TAA stay identical).
    if (o.noRefl) device->setReflectionParams(x3::rhi::IRenderDevice::ReflectionParams{});

    // --ddgi: A/B switch the other way — force DDGI probe-grid GI ON for any
    // path (headless showroom/level screenshots included) so before/after
    // captures isolate exactly the DDGI ambient-diffuse contribution. No-op on
    // hardware without ray query + position fetch (the device tier gate).
    if (o.ddgiForce) {
        x3::rhi::IRenderDevice::DdgiParams dp{};
        dp.enabled = true;
        device->setDdgiParams(dp);
        x3::logInfo(std::string("--ddgi: DDGI requested; device rayTracingSupported=") +
                    (device->rayTracingSupported() ? "YES" : "NO"));
    }

    // ---- --screenshot-matlib: surface-library preview (ART_BIBLE §4). Fully
    // self-contained (its own meshes/textures/lights, no world build) — render
    // the contact sheet and exit before any host machinery spins up. ----
    if (o.matlibShot) {
        const int rc = x3::game::runMatlibShot(*device, o.matlibShotDir);
        device.reset();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return rc;
    }

    // ---- Level Architect EDITOR overlay init (--editor / --screenshot-editor) ----
    // ImGui initializes ONLY here, ONLY when --editor (windowed) or --screenshot-editor
    // (headless proof) is set. Without either flag initEditorUI is never called and the
    // device allocates nothing for ImGui (the shipping game path is byte-for-byte
    // unchanged). The windowed --editor begin/end wrap lands in the interactive loop.
    if (o.editorMode && window) {
        device->initEditorUI(window);
        x3::logInfo(device->editorUIActive()
            ? "--editor: Dear ImGui (docking) editor overlay ACTIVE"
            : "--editor: editor overlay FAILED to init");
    }

    // ======================================================================
    // ---- HOST DISPATCH (#28 deep monolith split) --------------------------
    // Populate ONE HostContext (a near-1:1 mirror of main()'s shared live-state
    // locals) and run, in the SAME order the inline code did:
    //   1. dispatchScreenshotHosts  — the headless editor/loader/sky/ddgi/
    //      rtshadows/showroom/car/planet/nightsky/cutscene/terrain/ocean +
    //      capture-ai/-walk/-footik capture handlers (app/screenshot_hosts.cpp).
    //   2. dispatchWorldHost        — the self-contained --world hosts
    //      (destruct/physjoint/ragdoll/drive|boat|fly/club/showroom/valley/
    //      cliffs/streamed; app/world_hosts/*.cpp).
    // A >=0 result from either is this program's exit code; both return -1 to
    // fall through to the default host (the shared interactive render loop).
    // ======================================================================
    {
        x3::apphost::HostContext _hc;
        _hc.device           = device.get();
        _hc.window           = window;
        _hc.worldMode        = o.worldMode;
        _hc.headless         = headless;
        _hc.W                = W;
        _hc.H                = H;
        _hc.screenshot       = o.screenshot;
        _hc.screenshotPath   = o.screenshotPath;
        _hc.screenshotSettle = o.screenshotSettle;
        _hc.shotCamOverride  = o.shotCamOverride;
        for (int _k = 0; _k < 5; ++_k) _hc.shotCam[_k] = o.shotCam[_k];
        _hc.stressCount      = o.stressCount;
        _hc.bench            = o.bench;
        _hc.benchFrames      = o.benchFrames;
        _hc.ddgiForce        = o.ddgiForce;
        _hc.shotWorldMap     = o.shotWorldMap;
        _hc.wsBudgetMs       = o.wsBudgetMs;
        _hc.wsLookaheadS     = o.wsLookaheadS;
        _hc.cueTime          = o.cueTime;
        _hc.cutsceneFile     = o.cutsceneFile;
        _hc.editorShot       = o.editorShot;       _hc.editorShotPath   = o.editorShotPath;
        _hc.loaderShot       = o.loaderShot;       _hc.loaderShotPath   = o.loaderShotPath;
        _hc.skyShot          = o.skyShot;          _hc.skyShotPath      = o.skyShotPath;
        _hc.ddgiShot         = o.ddgiShot;         _hc.ddgiShotDir      = o.ddgiShotDir;
        _hc.rtshShot         = o.rtshShot;         _hc.rtshShotDir      = o.rtshShotDir;
        _hc.showroomShot     = o.showroomShot;     _hc.showroomShotPath = o.showroomShotPath;
        _hc.planetShot       = o.planetShot;       _hc.planetShotPath   = o.planetShotPath;
        _hc.nightskyShot     = o.nightskyShot;     _hc.nightskyShotPath = o.nightskyShotPath;
        _hc.cutsceneShot     = o.cutsceneShot;     _hc.cutsceneShotPath = o.cutsceneShotPath;
        _hc.terrainShot      = o.terrainShot;      _hc.terrainShotPath  = o.terrainShotPath;
        _hc.oceanShot        = o.oceanShot;        _hc.oceanShotPath    = o.oceanShotPath;
        _hc.oceanBaseShot    = o.oceanBaseShot;    _hc.oceanBaseShotPath = o.oceanBaseShotPath;
        _hc.captureAi        = o.captureAi;        _hc.captureAiDir     = o.captureAiDir;
        _hc.captureWalk      = o.captureWalk;      _hc.captureWalkPath  = o.captureWalkPath;
        _hc.captureFootIk    = o.captureFootIk;    _hc.captureFootIkPath = o.captureFootIkPath;
        _hc.destructShot     = o.destructShot;     _hc.destructShotPath = o.destructShotPath;
        _hc.elevShot         = o.elevShot;         _hc.elevShotDir      = o.elevShotDir;
        _hc.carShot          = o.carShot;          _hc.carShotDir       = o.carShotDir;
        _hc.upperShot        = o.upperShot;        _hc.upperShotDir     = o.upperShotDir;
        _hc.showroomFpShot   = o.showroomFpShot;   _hc.showroomFpShotPath = o.showroomFpShotPath;
        _hc.showroomRagdollShot = o.showroomRagdollShot; _hc.showroomRagdollShotPath = o.showroomRagdollShotPath;
        _hc.showroomDeckShot = o.showroomDeckShot; _hc.showroomDeckShotPath = o.showroomDeckShotPath;
        _hc.showroomElevShot = o.showroomElevShot; _hc.showroomElevShotPath = o.showroomElevShotPath;
        _hc.showroomStairShot = o.showroomStairShot; _hc.showroomStairShotPath = o.showroomStairShotPath;
        _hc.showroomFloor2Shot = o.showroomFloor2Shot; _hc.showroomFloor2ShotPath = o.showroomFloor2ShotPath;
        _hc.showroomDoorShot = o.showroomDoorShot; _hc.showroomDoorShotPath = o.showroomDoorShotPath;
        _hc.showroomStrutsShot = o.showroomStrutsShot; _hc.showroomStrutsShotPath = o.showroomStrutsShotPath;
        _hc.showroomGalleryShot = o.showroomGalleryShot; _hc.showroomGalleryShotPath = o.showroomGalleryShotPath;
        _hc.showroomCivShot  = o.showroomCivShot;  _hc.showroomCivShotPath = o.showroomCivShotPath;
        _hc.perfshopShot     = o.perfshopShot;     _hc.perfshopShotDir  = o.perfshopShotDir;
        _hc.ecologyShot      = o.ecologyShot;      _hc.ecologyShotPath  = o.ecologyShotPath;
        _hc.crowdShot        = o.crowdShot;        _hc.crowdShotPath    = o.crowdShotPath;

        int _shotRc = x3::apphost::dispatchScreenshotHosts(_hc);
        if (_shotRc >= 0) return _shotRc;
        int _hostRc = x3::apphost::dispatchWorldHost(_hc);
        if (_hostRc >= 0) return _hostRc;

    // ======================================================================
    // ---- DEFAULT HOST (#28 deep split, Phase C) ---------------------------
    // Neither a screenshot nor a --world host matched: run the shared
    // interactive render loop + world build (app/app_run.cpp). It joins the
    // boot-time audio future, builds the cell/terrain/canon/fromdoc world, runs
    // the loop (or the headless smoketest/screenshot/bench paths), and returns
    // bootTestExit. Thread the few extra prelude objects through the context.
    // ======================================================================
    _hc.descVsync       = desc.vsync;
    _hc.cliCVars        = o.cliCVars;
    _hc.bootAudioFut    = &bootAudioFut;
    _hc.smoketest       = o.smoketest;
    _hc.testBootTime    = o.testBootTime;
    _hc.testFramePacing = o.testFramePacing;
    _hc.testRt          = o.testRt;
    _hc.testReflections = o.testReflections;
    _hc.testDdgi        = o.testDdgi;
    _hc.testRtShadows   = o.testRtShadows;
    _hc.noRtShadows     = o.noRtShadows;
    _hc.legacyPost      = o.legacyPost;
    _hc.noTaa           = o.noTaa;
    _hc.noRefl          = o.noRefl;
    _hc.skipIntro       = o.skipIntro;
    _hc.introForce      = o.introForce;        // DEV --intro-force outcome override
    _hc.editorMode      = o.editorMode;
    _hc.fxDemo          = o.fxDemo;
    _hc.uiDemo          = o.uiDemo;            _hc.uiDemoPath = o.uiDemoPath; _hc.uiDemoScreen = o.uiDemoScreen;
    _hc.dialogShot      = o.dialogShot;
    _hc.vigilShot       = o.vigilShot;
    _hc.alertShot       = o.alertShot;
    _hc.captureSpire    = o.captureSpire;      _hc.captureSpireDir = o.captureSpireDir;
    _hc.docWorldPath    = o.docWorldPath;
    _hc.cullPathArg     = o.cullPathArg;
    _hc.hzbArg          = o.hzbArg;
    _hc.visArg          = o.visArg;
    _hc.bootBudgetMs    = o.bootBudgetMs;
    return x3::apphost::runDefaultHost(_hc);
    }   // close the host-dispatch block (its _hc reaches the default host)
}
