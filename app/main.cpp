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
#include "app_run.h"                       // #28 deep split: runDefaultHost() (the interactive render loop)
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
         testDeathRagdoll = false, testCanonLevel = false, testCanonPlay = false,
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
    // --test-act2bosses (Act-2 roster, Wave 2): the 5 alien-planet-surface enemy
    // defs + 4 single-body bosses (Memory Hunter / Siren / Breeder Queen / Garrison
    // Commander) + the Wave-2 Tuning tags (startAllied / copyFeintPhase /
    // escapeTimerSeconds) + the Act-1 + Martinez regression guard. Additive.
    bool        testAct2Bosses = false;
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
    // --cullpath <n> / --hzb: seed the r_cullpath / r_hzb cvars from the CLI so the
    // smoketest/screenshot/bench paths exercise the D15 GPU cull (INT_MIN = unset).
    int         cullPathArg = INT_MIN;
    int         hzbArg = 0;
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
    // Ocean vantage mode (--screenshot-ocean [path.png]): build the procedural
    // terrain world + an animated ocean at sea level under the sky/sun, pose a
    // camera on the shore looking out across the water toward the sun so the lit
    // animated waves, sun glint, depth-based shallow/deep color, and the
    // terrain->water shoreline all read, settle a few frames so the waves animate
    // + the shadow map registers, and capture a PNG. EFLZ Level 1 is interior;
    // this is the way to SEE the ocean. Default path: G:\X3Native-wt-water\ocean.png.
    bool        oceanShot = false;
    std::string oceanShotPath = "G:/X3Native-wt-water/ocean.png";
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
    const bool  loadedWinSize = readWindowSize(winW, winH);   // saved "SET AS DEFAULT" size
    (void)loadedWinSize;
    for (int i = 1; i < argc; ++i) {
        std::string_view a(argv[i]);
        // DDGI flags handled OUTSIDE the big else-if chain below (MSVC C1061:
        // every `else if` nests a block; the chain is at the compiler's limit).
        if (a == "--test-ddgi") { smoketest = true; testDdgi = true; continue; }
        if (a == "--ddgi") { ddgiForce = true; continue; }
        if (a == "--screenshot-ddgi") {
            ddgiShot = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') ddgiShotDir = argv[++i];
            continue;
        }
        // RT soft-shadow flags — handled OUTSIDE the chain (same C1061 reason).
        if (a == "--test-rtshadows") { smoketest = true; testRtShadows = true; continue; }
        // Zero-stutter flythrough — handled OUTSIDE the chain (same C1061 reason).
        if (a == "--test-framepacing") { testFramePacing = true; continue; }
        if (a == "--test-boottime") {
            testBootTime = true;
            if (i + 1 < argc && argv[i + 1][0] != '-')
                bootBudgetMs = std::atof(argv[++i]);
            continue;
        }
        if (a == "--nortshadows") { noRtShadows = true; continue; }   // A/B: pin tier 0 (CSM-only)
        if (a == "--screenshot-rtshadows") {
            rtshShot = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') rtshShotDir = argv[++i];
            continue;
        }
        // World-streaming flags handled OUTSIDE the big else-if chain (which sits at
        // MSVC's C1061 block-nesting limit — adding to it breaks the build).
        if (a == "--test-worldstream") { testWorldStream = true; continue; }
        if (a == "--test-worldmap")    { testWorldMap    = true; continue; }
        if (a == "--screenshot-worldmap") {   // headless world-map shot sequence
            shotWorldMap = true; worldMode = "streamed"; screenshot = true; continue;
        }
        if (a == "--ws-budget") {   // per-frame world-stream budget, ms (cvar-style tunable)
            if (i + 1 < argc && argv[i + 1][0] != '-') wsBudgetMs = std::strtof(argv[++i], nullptr);
            continue;
        }
        if (a == "--ws-lookahead") { // velocity lookahead, seconds
            if (i + 1 < argc && argv[i + 1][0] != '-') wsLookaheadS = std::strtof(argv[++i], nullptr);
            continue;
        }
        if (a == "--smoketest") smoketest = true;
        else if (a == "--legacypost")  legacyPost = 1;   // A/B: auto-exposure OFF (pre-strike look)
        else if (a == "--legacypost2") legacyPost = 2;   // A/B: + bloom OFF + tonemap passthrough
        else if (a == "--notaa")       noTaa = true;     // A/B: TAA off (jitter + resolve disabled)
        else if (a == "--norefl")      noRefl = true;    // A/B: reflections off (TAA stays on)
        else if (a == "--test-rt") { smoketest = true; testRt = true; }
        else if (a == "--test-reflections") { smoketest = true; testReflections = true; }
        else if (a == "--test-jobs") testJobs = true;
        else if (a == "--test-asset") testAsset = true;
        else if (a == "--test-console") testConsole = true;
        else if (a == "--test-physics") testPhysics = true;
        else if (a == "--test-gltf") testGltf = true;
        else if (a == "--test-player") testPlayer = true;
        else if (a == "--test-interact") testInteract = true;
        else if (a == "--test-physprops") testPhysprops = true;
        else if (a == "--test-ragdoll") testRagdoll = true;
        else if (a == "--test-ragdollskin") testRagdollSkin = true;
        else if (a == "--test-editor") testEditor = true;
        else if (a == "--test-loader") testLoader = true;
        else if (a == "--test-blockout") testBlockout = true;
        else if (a == "--test-barrels") testBarrels = true;
        else if (a == "--test-glass") testGlass = true;
        else if (a == "--test-frustumcull") testFrustumCull = true;
        else if (a == "--test-holoterm") testHoloterm = true;
        else if (a == "--test-llm") testLlm = true;
        else if (a == "--test-secretroom") testSecretRoom = true;
        else if (a == "--test-ecs") testEcs = true;
        else if (a == "--test-ecsrender") testEcsRender = true;
        else if (a == "--test-pickup") testPickup = true;
        else if (a == "--test-combat") testCombat = true;
        else if (a == "--test-deathragdoll") testDeathRagdoll = true;
        else if (a == "--test-audio") testAudio = true;
        else if (a == "--test-acoustics") testAcoustics = true;
        else if (a == "--test-level1") testLevel1 = true;
        else if (a == "--test-canonlevel") testCanonLevel = true;
        else if (a == "--test-canonplay") testCanonPlay = true;
        else if (a == "--test-phase2a") testPhase2a = true;
        else if (a == "--test-phase2b") testPhase2b = true;
        else if (a == "--test-anim") testAnim = true;
        else if (a == "--test-terrain") testTerrain = true;
        else if (a == "--test-terrainplace") testTerrainPlace = true;
        else if (a == "--test-streaming") testStreaming = true;
        else if (a == "--test-ai") testAi = true;
        else if (a == "--test-bestiary") testBestiary = true;
        else if (a == "--test-bosses") testBosses = true;
        // (chain break — restart the if/else-if ladder so MSVC stays under the
        // C1061 block-nesting limit; flags are exact == matches, all unique, so a
        // matched arg simply falls through the second ladder without re-matching)
        if (false) {}
        else if (a == "--test-act2bosses") testAct2Bosses = true;
        else if (a == "--test-spiremid") testSpireMid = true;
        else if (a == "--test-nexus") testNexus = true;
        else if (a == "--test-spiretop") testSpireTop = true;
        else if (a == "--test-timeline") testTimeline = true;
        else if (a == "--test-dronehack") testDroneHack = true;
        else if (a == "--test-sublevels") testSubLevels = true;
        else if (a == "--test-act2") testAct2 = true;
        else if (a == "--test-act2desert") testAct2Desert = true;
        else if (a == "--test-act2caves") testAct2Caves = true;
        else if (a == "--test-tod") testTod = true;
        else if (a == "--test-weather") testWeather = true;
        else if (a == "--test-worldregions") testWorldRegions = true;
        else if (a == "--test-city") testCity = true;
        else if (a == "--test-oceanbase") testOceanBase = true;
        else if (a == "--test-doorcode") testDoorCode = true;
        else if (a == "--test-hatchcode") testHatchCode = true;
        else if (a == "--test-hatch") testHatch = true;
        else if (a == "--test-elevator") testElevator = true;
        else if (a == "--test-elevatorfsm") testElevatorFsm = true;
        else if (a == "--test-net") testNet = true;
        else if (a == "--test-netsync") testNetSync = true;
        else if (a == "--test-netinterp") testNetInterp = true;
        else if (a == "--test-netpredict") testNetPredict = true;
        else if (a == "--test-rescue") testRescue = true;
        else if (a == "--test-thirdperson") testThirdPerson = true;
        else if (a == "--test-npctalk") testNpcTalk = true;
        else if (a == "--test-chattree") testChatTree = true;
        else if (a == "--test-mission") testMission = true;
        else if (a == "--test-destruction") testDestruction = true;
        else if (a == "--test-debris") testDebris = true;
        else if (a == "--test-gpuskin") testGpuSkin = true;
        else if (a == "--test-meshlet") testMeshlet = true;
        else if (a == "--test-gpucull") testGpuCull = true;
        else if (a == "--cullpath" && i + 1 < argc) cullPathArg = std::atoi(argv[++i]);
        else if (a == "--hzb") hzbArg = 1;
        else if (a == "--test-collapse") testCollapse = true;
        else if (a == "--test-physjoint") testPhysJoint = true;
        else if (a == "--test-nav") testNav = true;
        else if (a == "--test-script") testScript = true;
        else if (a == "--test-weapons") testWeapons = true;
        else if (a == "--test-vehicle") testVehicle = true;
        else if (a == "--test-vehparts") testVehParts = true;
        else if (a == "--test-ecology") testEcology = true;
        else if (a == "--test-crowd") testCrowd = true;
        else if (a == "--test-alert") testAlert = true;
        else if (a == "--screenshot-ecology") {
            ecologyShot = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') ecologyShotPath = argv[++i];
        }
        else if (a == "--screenshot-crowd") {
            crowdShot = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') crowdShotPath = argv[++i];
        }
        else if (a == "--screenshot-alert") {
            alertShot = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') alertShotPath = argv[++i];
        }
        else if (a == "--screenshot-perfshop") {
            perfshopShot = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') perfshopShotDir = argv[++i];
        }
        else if (a == "--set") {
            // Generic CLI cvar override: --set <cvar> <value> (repeatable).
            // Applied right after the console registers its cvars — the headless
            // A/B debugging workhorse (e.g. --set r_rtreflections 0).
            if (i + 2 < argc) { cliCVars.emplace_back(argv[i+1], argv[i+2]); i += 2; }
        }
        else if (a == "--test-footik") testFootIk = true;
        else if (a == "--test-ui") testUi = true;
        else if (a == "--test-loading") testLoading = true;
        else if (a == "--test-saveload") testSaveLoad = true;
        else if (a == "--test-dialog") testDialog = true;
        // (chain break #2 — see the note above)
        if (false) {}
        else if (a == "--demo-dialog") {
            demoDialog = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') demoDialogPath = argv[++i];
        }
        else if (a == "--test-valley") testValley = true;
        else if (a == "--test-cliffs") testCliffs = true;
        else if (a == "--test-club") testClub = true;
        else if (a == "--width") {
            if (i + 1 < argc) { winW = (uint32_t)std::strtoul(argv[++i], nullptr, 10); }
        }
        else if (a == "--height") {
            if (i + 1 < argc) { winH = (uint32_t)std::strtoul(argv[++i], nullptr, 10); }
        }
        else if (a == "--world") {
            if (i + 1 < argc && argv[i + 1][0] != '-') { worldMode = argv[++i]; worldExplicit = true; }
            // `--world fromdoc <path.json>`: an optional second positional token is
            // the LevelDoc to boot (default = the editor's File>Save target).
            if (worldMode == "fromdoc" && i + 1 < argc && argv[i + 1][0] != '-')
                docWorldPath = argv[++i];
        }
        else if (a == "--stress") {
            if (i + 1 < argc) { stressCount = (uint32_t)std::strtoul(argv[++i], nullptr, 10); }
        }
        else if (a == "--bench") {
            bench = true;
            if (i + 1 < argc) { stressCount = (uint32_t)std::strtoul(argv[++i], nullptr, 10); }
            // Optional second positional arg = frame count.
            if (i + 1 < argc && argv[i + 1][0] != '-')
                benchFrames = (uint32_t)std::strtoul(argv[++i], nullptr, 10);
        }
        else if (a == "--editor") editorMode = true;
        else if (a == "--screenshot-editor") {
            editorShot = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') editorShotPath = argv[++i];
        }
        else if (a == "--screenshot-loader") {
            loaderShot = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') loaderShotPath = argv[++i];
        }
        else if (a == "--screenshot-dialog") {
            screenshot = true; dialogShot = true;
            screenshotPath = dialogShotPath;
            if (i + 1 < argc && argv[i + 1][0] != '-') { dialogShotPath = argv[++i]; screenshotPath = dialogShotPath; }
        }
        else if (a == "--screenshot") {
            screenshot = true;
            // Optional path arg (next token, if it isn't another flag).
            if (i + 1 < argc && argv[i + 1][0] != '-') screenshotPath = argv[++i];
            // Optional settle-frame count (second positional, if numeric).
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9')
                screenshotSettle = (int)std::strtol(argv[++i], nullptr, 10);
        }
        else if (a == "--shot-cam") {
            // Parse "x,y,z,yaw,pitch" into shotCam[]; enables the override.
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                const char* s = argv[++i];
                int n = 0; char* end = nullptr;
                while (n < 5 && *s) {
                    shotCam[n++] = std::strtof(s, &end);
                    s = (end && *end == ',') ? end + 1 : end;
                    if (!end || (*end != ',' && *end != '\0')) break;
                }
                shotCamOverride = (n == 5);
            }
        }
        else if (a == "--ui-demo" || a == "--screenshot-menu") {
            uiDemo = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') uiDemoPath = argv[++i];
            // Optional screen keyword: main | pause | settings.
            if (i + 1 < argc && argv[i + 1][0] != '-') uiDemoScreen = argv[++i];
        }
        else if (a == "--fx-demo") fxDemo = true;
        else if (a == "--screenshot-sky") {
            skyShot = true;
            // Optional output path arg (next token, if it isn't another flag).
            if (i + 1 < argc && argv[i + 1][0] != '-') skyShotPath = argv[++i];
        }
        else if (a == "--screenshot-showroom") {
            showroomShot = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') showroomShotPath = argv[++i];
        }
        else if (a == "--screenshot-car") {
            carShot = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') carShotDir = argv[++i];
        }
        else if (a == "--screenshot-showroom-fp") {
            // Headless first-person proof of the walkable --world showroom. Forces the
            // showroom world on so the SAME build path runs, then renders one frame from
            // the player spawn eye and exits.
            showroomFpShot = true;
            worldMode = "showroom";
            if (i + 1 < argc && argv[i + 1][0] != '-') showroomFpShotPath = argv[++i];
        }
        else if (a == "--screenshot-showroom-ragdoll") {
            // Headless proof of Aria's physics RAGDOLL: same showroom-FP setup, but
            // collapse her + step the world so she falls, then capture one frame.
            showroomRagdollShot = true;
            worldMode = "showroom";
            if (i + 1 < argc && argv[i + 1][0] != '-') showroomRagdollShotPath = argv[++i];
        }
        else if (a == "--screenshot-showroom-deck") {
            // Headless proof: stand on the spire-top glass deck, look out at the night sky.
            showroomDeckShot = true;
            worldMode = "showroom";
            if (i + 1 < argc && argv[i + 1][0] != '-') showroomDeckShotPath = argv[++i];
        }
        else if (a == "--screenshot-showroom-elevator") {
            // Headless proof: glass elevator car parked mid-shaft, camera inside it.
            showroomElevShot = true;
            worldMode = "showroom";
            if (i + 1 < argc && argv[i + 1][0] != '-') showroomElevShotPath = argv[++i];
        }
        else if (a == "--screenshot-showroom-stair") {
            // Headless proof: the entry passage + 90 deg turn + the stairs up to the atrium.
            showroomStairShot = true;
            worldMode = "showroom";
            if (i + 1 < argc && argv[i + 1][0] != '-') showroomStairShotPath = argv[++i];
        }
        else if (a == "--screenshot-showroom-floor2") {
            // Headless proof: standing on the 2nd floor having climbed the synthesized stair.
            showroomFloor2Shot = true;
            worldMode = "showroom";
            if (i + 1 < argc && argv[i + 1][0] != '-') showroomFloor2ShotPath = argv[++i];
        }
        else if (a == "--screenshot-showroom-door") {
            // Headless proof: the hidden STRUT-FACE door (open by default; closed via env).
            showroomDoorShot = true;
            worldMode = "showroom";
            if (i + 1 < argc && argv[i + 1][0] != '-') showroomDoorShotPath = argv[++i];
        }
        else if (a == "--screenshot-showroom-struts") {
            // Headless EXTERIOR proof: frame the symmetric set of thickened "/" struts.
            showroomStrutsShot = true;
            worldMode = "showroom";
            if (i + 1 < argc && argv[i + 1][0] != '-') showroomStrutsShotPath = argv[++i];
        }
        else if (a == "--screenshot-showroom-gallery") {
            // Headless proof of the HIDDEN ANALYST GALLERY: captures the gallery (terminals
            // + analyst figures) AND a down-through-the-dark-glass view in one run (and an
            // up-from-the-civilian-floor view under X3_SHOWROOM_GALLERY_UP=1).
            showroomGalleryShot = true;
            worldMode = "showroom";
            if (i + 1 < argc && argv[i + 1][0] != '-') showroomGalleryShotPath = argv[++i];
        }
        else if (a == "--screenshot-showroom-civilians") {
            // Headless DAY proof of the CIVILIAN crowd on the ground + 2nd floors:
            // captures a wide ground-floor view (<path>) + a mezzanine view (<path>_mezz.png).
            showroomCivShot = true;
            worldMode = "showroom";
            if (i + 1 < argc && argv[i + 1][0] != '-') showroomCivShotPath = argv[++i];
        }
        else if (a == "--screenshot-planet") {
            planetShot = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') planetShotPath = argv[++i];
        }
        else if (a == "--screenshot-nightsky") {
            nightskyShot = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') nightskyShotPath = argv[++i];
        }
        // ---- x3.cutscene/1 args: a FRESH if-chain (not chained onto the giant
        // else-if ladder above — MSVC C1061 nesting limit). Disjoint exact matches,
        // so re-starting the chain is behavior-identical.
        if (a == "--test-cutscene") testCutscene = true;
        else if (a == "--skipintro") skipIntro = true;
        else if (a == "--cutscene") {
            if (i + 1 < argc && argv[i + 1][0] != '-') cutsceneFile = argv[++i];
        }
        else if (a == "--cuetime") {
            if (i + 1 < argc) cueTime = (float)std::strtod(argv[++i], nullptr);
        }
        else if (a == "--cutscene-shot") {
            cutsceneShot = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') cutsceneShotPath = argv[++i];
        }
        else if (a == "--screenshot-terrain") {
            terrainShot = true;
            // Optional output path arg (next token, if it isn't another flag).
            if (i + 1 < argc && argv[i + 1][0] != '-') terrainShotPath = argv[++i];
        }
        else if (a == "--screenshot-ocean") {
            oceanShot = true;
            // Optional output path arg (next token, if it isn't another flag).
            if (i + 1 < argc && argv[i + 1][0] != '-') oceanShotPath = argv[++i];
        }
        else if (a == "--screenshot-destruct") {
            destructShot = true;
            // Optional output path arg (next token, if it isn't another flag).
            if (i + 1 < argc && argv[i + 1][0] != '-') destructShotPath = argv[++i];
        }
        else if (a == "--capture-ai") {
            captureAi = true;
            // Optional output directory arg (next token, if it isn't another flag).
            if (i + 1 < argc && argv[i + 1][0] != '-') captureAiDir = argv[++i];
        }
        else if (a == "--capture-walk") {
            captureWalk = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') captureWalkPath = argv[++i];
        }
        else if (a == "--screenshot-footik") {
            captureFootIk = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') captureFootIkPath = argv[++i];
        }
        else if (a == "--capture-spire") {
            captureSpire = true;
            // Optional output directory arg (next token, if it isn't another flag).
            if (i + 1 < argc && argv[i + 1][0] != '-') captureSpireDir = argv[++i];
        }
        else if (a == "--list-clips") {
            listClips = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') listClipsPath = argv[++i];
        }
        else if (a == "--test-locomotion") {
            testLocomotion = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') testLocomotionPath = argv[++i];
        }
        else if (a == "--test-intro") testIntro = true;
    }

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
        _tf.testJobs = testJobs;
        _tf.testAsset = testAsset;
        _tf.testConsole = testConsole;
        _tf.testPhysics = testPhysics;
        _tf.testPhysJoint = testPhysJoint;
        _tf.testRagdoll = testRagdoll;
        _tf.testGltf = testGltf;
        _tf.testPlayer = testPlayer;
        _tf.testInteract = testInteract;
        _tf.testPhysprops = testPhysprops;
        _tf.testRagdollSkin = testRagdollSkin;
        _tf.testEditor = testEditor;
        _tf.testBlockout = testBlockout;
        _tf.testLoader = testLoader;
        _tf.testBarrels = testBarrels;
        _tf.testGlass = testGlass;
        _tf.testFrustumCull = testFrustumCull;
        _tf.testHoloterm = testHoloterm;
        _tf.testSecretRoom = testSecretRoom;
        _tf.testHatch = testHatch;
        _tf.testLlm = testLlm;
        _tf.testEcs = testEcs;
        _tf.testEcsRender = testEcsRender;
        _tf.testPickup = testPickup;
        _tf.testCombat = testCombat;
        _tf.testDeathRagdoll = testDeathRagdoll;
        _tf.testAudio = testAudio;
        _tf.testAcoustics = testAcoustics;
        _tf.testLevel1 = testLevel1;
        _tf.testCanonLevel = testCanonLevel;
        _tf.testCanonPlay = testCanonPlay;
        _tf.testIntro = testIntro;
        _tf.testCutscene = testCutscene;
        _tf.testPhase2a = testPhase2a;
        _tf.testPhase2b = testPhase2b;
        _tf.testAnim = testAnim;
        _tf.testLocomotion = testLocomotion;
        _tf.listClips = listClips;
        _tf.testTerrain = testTerrain;
        _tf.testTerrainPlace = testTerrainPlace;
        _tf.testStreaming = testStreaming;
        _tf.testWorldStream = testWorldStream;
        _tf.testWorldMap = testWorldMap;
        _tf.testAi = testAi;
        _tf.testBestiary = testBestiary;
        _tf.testBosses = testBosses;
        _tf.testAct2Bosses = testAct2Bosses;
        _tf.testSpireMid = testSpireMid;
        _tf.testNexus = testNexus;
        _tf.testSpireTop = testSpireTop;
        _tf.testTimeline = testTimeline;
        _tf.testDroneHack = testDroneHack;
        _tf.testSubLevels = testSubLevels;
        _tf.testTod = testTod;
        _tf.testWeather = testWeather;
        _tf.testAct2 = testAct2;
        _tf.testAct2Desert = testAct2Desert;
        _tf.testAct2Caves = testAct2Caves;
        _tf.testWorldRegions = testWorldRegions;
        _tf.testCity = testCity;
        _tf.testOceanBase = testOceanBase;
        _tf.testDoorCode = testDoorCode;
        _tf.testHatchCode = testHatchCode;
        _tf.testElevator = testElevator;
        _tf.testElevatorFsm = testElevatorFsm;
        _tf.testNet = testNet;
        _tf.testNetSync = testNetSync;
        _tf.testNetInterp = testNetInterp;
        _tf.testNetPredict = testNetPredict;
        _tf.testRescue = testRescue;
        _tf.testThirdPerson = testThirdPerson;
        _tf.testNpcTalk = testNpcTalk;
        _tf.testChatTree = testChatTree;
        _tf.testMission = testMission;
        _tf.testDestruction = testDestruction;
        _tf.testDebris = testDebris;
        _tf.testGpuSkin = testGpuSkin;
        _tf.testMeshlet = testMeshlet;
        _tf.testGpuCull = testGpuCull;
        _tf.testCollapse = testCollapse;
        _tf.testNav = testNav;
        _tf.testWeapons = testWeapons;
        _tf.testScript = testScript;
        _tf.testVehicle = testVehicle;
        _tf.testVehParts = testVehParts;
        _tf.testEcology = testEcology;
        _tf.testCrowd = testCrowd;
        _tf.testAlert = testAlert;
        _tf.testFootIk = testFootIk;
        _tf.testUi = testUi;
        _tf.testLoading = testLoading;
        _tf.testSaveLoad = testSaveLoad;
        _tf.testDialog = testDialog;
        _tf.demoDialog = demoDialog;
        _tf.testValley = testValley;
        _tf.testCliffs = testCliffs;
        _tf.testClub = testClub;
        _tf.testLocomotionPath = testLocomotionPath;
        _tf.listClipsPath = listClipsPath;
        _tf.demoDialogPath = demoDialogPath;
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
    if (testBootTime && !worldExplicit) {
        worldMode = "canonlevel";
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
        const bool legacyCell = (worldMode == "level1") || (worldMode == "elevator");
        const bool canonCell  = (worldMode == "canonlevel") || (worldMode == "intro");
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
    if (perfshopShot) worldMode = "drive";   // the shop lives in the drive world
    if (ecologyShot)  worldMode = "valley";  // the ambient ecology rides the valley biome
    if (crowdShot)    worldMode = "club";    // the crowd proof lives on the club floor
    if (alertShot) { screenshot = true; screenshotPath = alertShotPath; }   // rides --screenshot
    const bool headless = smoketest || testFramePacing || screenshot || skyShot || ddgiShot || showroomShot || carShot || showroomFpShot || showroomRagdollShot || showroomDeckShot || showroomElevShot || showroomStairShot || showroomFloor2Shot || showroomDoorShot || showroomStrutsShot || showroomGalleryShot || showroomCivShot || planetShot || nightskyShot || cutsceneShot || terrainShot || oceanShot || captureAi || captureWalk || destructShot || captureFootIk || uiDemo || captureSpire || editorShot || loaderShot || perfshopShot || ecologyShot || crowdShot;

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
    if (winW < 320)  winW = 320;
    if (winH < 240)  winH = 240;
    const uint32_t W = headless ? kHeadlessW : winW;
    const uint32_t H = headless ? kHeadlessH : winH;
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
            (worldMode == "level1") || (worldMode == "elevator") ||
            (worldMode == "canonlevel") || (worldMode == "intro") ||
            (worldMode == "terrain") || (worldMode == "ocean");
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
    desc.ssaa = (showroomShot || carShot || showroomFpShot || showroomRagdollShot || showroomDeckShot || showroomElevShot || showroomStairShot || showroomFloor2Shot || showroomDoorShot || showroomStrutsShot || showroomGalleryShot || showroomCivShot || planetShot || nightskyShot || cutsceneShot) ? 4u : 1u;   // 4x supersample the showroom / planet / nightsky still (5090 headless: ~16 samples/px, pristine)
    // Benchmark mode runs with vsync OFF so it measures the true frame ceiling,
    // not the display refresh cap.
    desc.vsync  = !bench;
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
    if (legacyPost || noTaa) {
        x3::rhi::IRenderDevice::PostFXParams px{};
        if (legacyPost) {
            px.autoExposure = false;             // legacy = no eye adaptation
            px.taa = false;                      // legacy = no TAA jitter/resolve
            if (legacyPost > 1) { px.bloomEnabled = false; px.tonemapMode = 0; }
        }
        if (noTaa) px.taa = false;
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
    if (noRefl) device->setReflectionParams(x3::rhi::IRenderDevice::ReflectionParams{});

    // --ddgi: A/B switch the other way — force DDGI probe-grid GI ON for any
    // path (headless showroom/level screenshots included) so before/after
    // captures isolate exactly the DDGI ambient-diffuse contribution. No-op on
    // hardware without ray query + position fetch (the device tier gate).
    if (ddgiForce) {
        x3::rhi::IRenderDevice::DdgiParams dp{};
        dp.enabled = true;
        device->setDdgiParams(dp);
        x3::logInfo(std::string("--ddgi: DDGI requested; device rayTracingSupported=") +
                    (device->rayTracingSupported() ? "YES" : "NO"));
    }

    // ---- Level Architect EDITOR overlay init (--editor / --screenshot-editor) ----
    // ImGui initializes ONLY here, ONLY when --editor (windowed) or --screenshot-editor
    // (headless proof) is set. Without either flag initEditorUI is never called and the
    // device allocates nothing for ImGui (the shipping game path is byte-for-byte
    // unchanged). The windowed --editor begin/end wrap lands in the interactive loop.
    if (editorMode && window) {
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
        _hc.worldMode        = worldMode;
        _hc.headless         = headless;
        _hc.W                = W;
        _hc.H                = H;
        _hc.screenshot       = screenshot;
        _hc.screenshotPath   = screenshotPath;
        _hc.screenshotSettle = screenshotSettle;
        _hc.shotCamOverride  = shotCamOverride;
        for (int _k = 0; _k < 5; ++_k) _hc.shotCam[_k] = shotCam[_k];
        _hc.stressCount      = stressCount;
        _hc.bench            = bench;
        _hc.benchFrames      = benchFrames;
        _hc.ddgiForce        = ddgiForce;
        _hc.shotWorldMap     = shotWorldMap;
        _hc.wsBudgetMs       = wsBudgetMs;
        _hc.wsLookaheadS     = wsLookaheadS;
        _hc.cueTime          = cueTime;
        _hc.cutsceneFile     = cutsceneFile;
        _hc.editorShot       = editorShot;       _hc.editorShotPath   = editorShotPath;
        _hc.loaderShot       = loaderShot;       _hc.loaderShotPath   = loaderShotPath;
        _hc.skyShot          = skyShot;          _hc.skyShotPath      = skyShotPath;
        _hc.ddgiShot         = ddgiShot;         _hc.ddgiShotDir      = ddgiShotDir;
        _hc.rtshShot         = rtshShot;         _hc.rtshShotDir      = rtshShotDir;
        _hc.showroomShot     = showroomShot;     _hc.showroomShotPath = showroomShotPath;
        _hc.planetShot       = planetShot;       _hc.planetShotPath   = planetShotPath;
        _hc.nightskyShot     = nightskyShot;     _hc.nightskyShotPath = nightskyShotPath;
        _hc.cutsceneShot     = cutsceneShot;     _hc.cutsceneShotPath = cutsceneShotPath;
        _hc.terrainShot      = terrainShot;      _hc.terrainShotPath  = terrainShotPath;
        _hc.oceanShot        = oceanShot;        _hc.oceanShotPath    = oceanShotPath;
        _hc.captureAi        = captureAi;        _hc.captureAiDir     = captureAiDir;
        _hc.captureWalk      = captureWalk;      _hc.captureWalkPath  = captureWalkPath;
        _hc.captureFootIk    = captureFootIk;    _hc.captureFootIkPath = captureFootIkPath;
        _hc.destructShot     = destructShot;     _hc.destructShotPath = destructShotPath;
        _hc.carShot          = carShot;          _hc.carShotDir       = carShotDir;
        _hc.showroomFpShot   = showroomFpShot;   _hc.showroomFpShotPath = showroomFpShotPath;
        _hc.showroomRagdollShot = showroomRagdollShot; _hc.showroomRagdollShotPath = showroomRagdollShotPath;
        _hc.showroomDeckShot = showroomDeckShot; _hc.showroomDeckShotPath = showroomDeckShotPath;
        _hc.showroomElevShot = showroomElevShot; _hc.showroomElevShotPath = showroomElevShotPath;
        _hc.showroomStairShot = showroomStairShot; _hc.showroomStairShotPath = showroomStairShotPath;
        _hc.showroomFloor2Shot = showroomFloor2Shot; _hc.showroomFloor2ShotPath = showroomFloor2ShotPath;
        _hc.showroomDoorShot = showroomDoorShot; _hc.showroomDoorShotPath = showroomDoorShotPath;
        _hc.showroomStrutsShot = showroomStrutsShot; _hc.showroomStrutsShotPath = showroomStrutsShotPath;
        _hc.showroomGalleryShot = showroomGalleryShot; _hc.showroomGalleryShotPath = showroomGalleryShotPath;
        _hc.showroomCivShot  = showroomCivShot;  _hc.showroomCivShotPath = showroomCivShotPath;
        _hc.perfshopShot     = perfshopShot;     _hc.perfshopShotDir  = perfshopShotDir;
        _hc.ecologyShot      = ecologyShot;      _hc.ecologyShotPath  = ecologyShotPath;
        _hc.crowdShot        = crowdShot;        _hc.crowdShotPath    = crowdShotPath;

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
    _hc.cliCVars        = cliCVars;
    _hc.bootAudioFut    = &bootAudioFut;
    _hc.smoketest       = smoketest;
    _hc.testBootTime    = testBootTime;
    _hc.testFramePacing = testFramePacing;
    _hc.testRt          = testRt;
    _hc.testReflections = testReflections;
    _hc.testDdgi        = testDdgi;
    _hc.testRtShadows   = testRtShadows;
    _hc.noRtShadows     = noRtShadows;
    _hc.legacyPost      = legacyPost;
    _hc.noTaa           = noTaa;
    _hc.noRefl          = noRefl;
    _hc.skipIntro       = skipIntro;
    _hc.editorMode      = editorMode;
    _hc.fxDemo          = fxDemo;
    _hc.uiDemo          = uiDemo;            _hc.uiDemoPath = uiDemoPath; _hc.uiDemoScreen = uiDemoScreen;
    _hc.dialogShot      = dialogShot;
    _hc.alertShot       = alertShot;
    _hc.captureSpire    = captureSpire;      _hc.captureSpireDir = captureSpireDir;
    _hc.docWorldPath    = docWorldPath;
    _hc.cullPathArg     = cullPathArg;
    _hc.hzbArg          = hzbArg;
    _hc.bootBudgetMs    = bootBudgetMs;
    return x3::apphost::runDefaultHost(_hc);
    }   // close the host-dispatch block (its _hc reaches the default host)
}
