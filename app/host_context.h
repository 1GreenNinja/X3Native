#pragma once
// ============================================================================
// HostContext — the shared live state threaded through the --world hosts and
// the interactive render loop (#28 deep monolith split).
//
// main() builds the device/window + parses all flags into locals, then populates
// ONE of these and dispatches to a host function. Each host body was lifted
// VERBATIM out of main()'s giant body; the ONLY edits are the mechanics of
// reaching the shared state through `hc.` instead of as a bare main()-local. So
// populating this struct in main() stays a near-1:1 assignment list (mirrors the
// local names), exactly like TestFlags did for the headless test ladder.
//
// Ownership note: HostContext holds NON-owning pointers/refs to objects main()
// still owns (device, window). The hosts tear those down themselves on their
// exit path (device->shutdown(); glfwDestroyWindow(window); glfwTerminate()),
// byte-identical to the inline bodies — so main() must NOT double-free after a
// host returns (the host dispatch returns the program exit code directly).
// ============================================================================

#include <cstdint>
#include <string>
#include <vector>
#include <utility>
#include <future>

#include "boot_audio.h"   // BootAudio (the boot-time async audio future the default host joins)

struct GLFWwindow;
namespace x3 { namespace rhi { class IRenderDevice; } }
namespace x3 { namespace audio { class IAudioSystem; } }

namespace x3 { namespace apphost {

struct HostContext {
    // ---- Live objects (non-owning; main() owns them) ----
    x3::rhi::IRenderDevice* device = nullptr;   // the live render device
    GLFWwindow*             window = nullptr;   // null in headless mode
    // The live audio system (non-owning; app_run owns the unique_ptr). Threaded so
    // the Intro Orchestrator's cinematic beats get music/SFX (Phase 5 audio restore):
    // P4 passed nullptr audio to the beats and the intro went SILENT. null = silent
    // (headless / no audio device) — every cue plays graceful-silent then.
    x3::audio::IAudioSystem* audio = nullptr;

    // ---- Mode / routing ----
    std::string worldMode = "level1";
    bool        headless  = false;

    // ---- W-MENU: RUNTIME WORLD LOAD (the world menu's "LOADS WORLD" rows) --------
    // A host sets these and RETURNS 0. main() then tears the host down, swaps
    // worldMode, and re-dispatches — the SAME window and the SAME render device, so
    // this is a real in-engine world load, not a process relaunch. `switchDestKey`
    // (optional) is a destination-registry key the newly-built world should place the
    // player at once it is standing (load-AND-place); "" = that world's own spawn.
    //
    // Only the DEFAULT host (app_run.cpp) honours switchDestKey today; every host can
    // REQUEST a switch. main() clears both before each dispatch.
    std::string switchWorldTo;
    std::string switchDestKey;
    // --test-worldswitch <flag>: the default host's smoketest path REQUESTS this
    // switch after its render frames, so the world-load handoff is exercised
    // headlessly (see cli.h). "" = normal smoketest (no switch).
    std::string worldSwitchTest;
    // Set by main()'s world-load loop on the NEW world's dispatch: a destination
    // key the freshly-built world should stand the player at, instead of its own
    // spawn. "" = use the world's normal spawn (every existing launch path).
    std::string spawnAtKey;

    // ---- Resolution (headless = fixed 1280x720; else the windowed size) ----
    uint32_t W = 1280;
    uint32_t H = 720;

    // ---- Screenshot / capture state ----
    bool        screenshot       = false;
    std::string screenshotPath;
    int         screenshotSettle = 16;
    bool        shotCamOverride  = false;
    float       shotCam[5]       = { 8.0f, 1.75f, -0.4f, 0.06f, -0.16f };
    // Start with the flashlight OFF (--flashlight-off): the honest-lighting review
    // gate. Redundant since the torch defaults OFF (2026-08-18) but still honored.
    bool        flashlightOff    = false;
    // Start with the flashlight ON (--flashlight-on). Also raises r_flashlight to 1
    // so the cvar and the live state agree from frame one.
    bool        flashlightOn     = false;

    // ---- Stress / benchmark ----
    uint32_t stressCount = 0;
    bool     bench       = false;
    uint32_t benchFrames = 600;

    // ---- Per-host screenshot proof flags + paths (showroom family etc.) ----
    bool        carShot = false;            std::string carShotDir;
    // --car <id>: which app/car_roster.h CarSpec the player drives and the
    // showcase poses. Default "gbx" (see CliOptions::carId — PAIRED, change
    // both). Hosts resolve it with x3::game::carSpecById(carId.c_str()).
    std::string carId = "gbx";
    bool        upperShot = false;          std::string upperShotDir;   // R-5: floors 2-7 proof
    // door-mesh swap: the repeatable DOOR visual gate (per-floor leaf/frame/signage,
    // closed + mid-slide + open, head-on at a real cut doorway on each floor).
    bool        doorShot = false;           std::string doorShotDir;
    bool        rescueShot = false;         std::string rescueShotDir;  // F2 rescue-room closeups
    bool        showroomFpShot = false;     std::string showroomFpShotPath;
    bool        showroomRagdollShot = false;std::string showroomRagdollShotPath;
    bool        showroomDeckShot = false;   std::string showroomDeckShotPath;
    bool        showroomElevShot = false;   std::string showroomElevShotPath;
    bool        showroomStairShot = false;  std::string showroomStairShotPath;
    bool        showroomFloor2Shot = false; std::string showroomFloor2ShotPath;
    bool        showroomDoorShot = false;   std::string showroomDoorShotPath;
    bool        showroomStrutsShot = false; std::string showroomStrutsShotPath;
    bool        showroomGalleryShot = false;std::string showroomGalleryShotPath;
    bool        showroomCivShot = false;    std::string showroomCivShotPath;

    bool        destructShot = false;       std::string destructShotPath;
    // R-4 fold: --screenshot-elevator beauty set (interior/exterior/strata trio)
    bool        elevShot = false;           std::string elevShotDir = "docs/screenshots/elevator";
    bool        perfshopShot = false;       std::string perfshopShotDir;
    bool        ecologyShot = false;        std::string ecologyShotPath;
    bool        crowdShot = false;          std::string crowdShotPath;
    bool        complexShot = false;        std::string complexShotDir = "docs/screenshots/complex";
    bool        tunnelShot = false;         std::string tunnelShotDir = "docs/screenshots/tunnel";
    // --screenshot-jake: the on-foot AnimatedCharacter proof set (see cli.h).
    bool        jakeShot = false;           std::string jakeShotDir = "docs/screenshots/jake";
    // --screenshot-town: the W-TOWN mountain-town proof set (see cli.h).
    bool        townShot = false;           std::string townShotDir = "docs/screenshots/town";
    // --screenshot-cutaway: the LEVEL ARCHITECT CUTAWAY proof set (stacked
    // floors / upper floors hidden / hover card). Runs through --world cutaway.
    bool        cutawayShot = false;        std::string cutawayShotDir = "docs/screenshots/cutaway";

    // ---- A/B / GI force switches consulted by some hosts' settle logic ----
    bool        ddgiForce = false;          // --ddgi: longer probe-convergence settle

    // ---- --world streamed tuning + the headless world-map shot ----
    bool        shotWorldMap  = false;      // --screenshot-worldmap (headless map shot sequence)
    float       wsBudgetMs    = 6.0f;       // --ws-budget   streaming upload budget (ms/frame)
    float       wsLookaheadS  = 2.5f;       // --ws-lookahead prefetch horizon (s)

    // ---- Headless SCREENSHOT/CAPTURE handlers (Phase B) — flags + paths the
    // pre-host capture handlers consult (dispatchScreenshotHosts) ----
    bool        editorShot = false;     std::string editorShotPath;
    bool        loaderShot = false;     std::string loaderShotPath;
    bool        skyShot = false;        std::string skyShotPath;
    bool        ddgiShot = false;       std::string ddgiShotDir;
    bool        reflVerifyShot = false; std::string reflVerifyShotDir;
    bool        rtMatVerifyShot = false; std::string rtMatVerifyShotDir;
    bool        rtshShot = false;       std::string rtshShotDir;
    bool        showroomShot = false;   std::string showroomShotPath;
    bool        planetShot = false;     std::string planetShotPath;
    bool        nightskyShot = false;   std::string nightskyShotPath;
    bool        cutsceneShot = false;   std::string cutsceneShotPath;
    bool        terrainShot = false;    std::string terrainShotPath;
    // Lane 3 CSM proof suite (--screenshot-csm <dir>): A/B pillar rows at 3
    // distances, a camera-pan sequence, and a cascade-boundary framing.
    bool        csmShot = false;        std::string csmShotDir;
    bool        oceanShot = false;      std::string oceanShotPath;
    bool        oceanBaseShot = false;  std::string oceanBaseShotPath;   // W3-4 undersea base
    bool        cityShot = false;       std::string cityShotPath;        // W8-3 city vantage
    bool        captureAi = false;      std::string captureAiDir;
    bool        captureCrowdSpread = false; std::string captureCrowdSpreadDir;
    bool        captureWalk = false;    std::string captureWalkPath;
    std::string captureWalkRig;         // --capture-walk optional rig GLB
    bool        captureFootIk = false;  std::string captureFootIkPath;
    std::string cutsceneFile;           // --cutscene <file>
    float       cueTime = 0.0f;         // --cuetime <s>
    bool        noFilmic = false;       // --nofilmic: cutscene filmic post OFF (A/B stills)

    // ---- Default-host (interactive render loop) extra prelude state (Phase C) ----
    bool        descVsync = true;       // DeviceDesc.vsync (smoketest UBO mirror)
    // What the validation layers were ACTUALLY asked to check on this run (the
    // resolved DeviceDesc values, not the CLI request). The smoketest verdict
    // line restates them so a copied "30 frames + recreate OK" can never again be
    // mistaken for "0 VUID" on a run that had no layers loaded.
    bool        descValidation = false;
    bool        descSyncValidation = false;
    // --set <cvar> <value> pairs applied after console cvar registration.
    std::vector<std::pair<std::string, std::string>> cliCVars;
    // [--set AUDIT] The `--screenshot-*` flag this run is, verbatim ("" = none).
    // See CliOptions::captureHost — dispatchScreenshotHosts uses it to know a
    // capture host owns this run before any handler has run.
    std::string captureHost;
    // Boot-time async audio future (launched in main(); joined in the default host).
    // Non-owning pointer to main()'s local future.
    std::future<BootAudio>* bootAudioFut = nullptr;

    // ---- More default-host flags (consulted by the render loop / world build) ----
    bool        smoketest = false;
    bool        testBootTime = false;
    bool        testFramePacing = false;
    bool        testRt = false;
    bool        testReflections = false;
    bool        testDdgi = false;
    bool        testRtShadows = false;
    bool        noRtShadows = false;
    int         legacyPost = 0;
    bool        noTaa = false;
    bool        noRefl = false;
    // Reflection DENOISE A/B (--refldn N / --refldn-disc S). Negative = leave the
    // device default. 0 iterations = stage OFF, which is bit-exact to the
    // pre-denoise renderer and is the "before" side of the door-skin blotch
    // measurement. Threaded through HostContext because the screenshot hosts do
    // NOT run the per-frame cvar sync — the same reason --norefl lives here.
    int         reflDenoise = -1;
    float       reflDnDisc  = -1.0f;
    float       reflDnNormal = -1.0f;   // --refldn-normal: normal edge-stop exponent (<0 = default)
    float       reflDnDepth  = -1.0f;   // --refldn-depth: depth edge-stop sigma (<0 = default)
    bool        skipIntro = false;
    // ---- Interactive intro (Phase 4 branch wiring) ----
    // Per-save deterministic seed for the intro outcome roll. 0 = derive from the
    // persisted StoryFlags content (a fresh save vs a continued one rolls stably).
    // Threaded so the chance roll is reproducible per save, not a fixed default.
    uint32_t    introSeed = 0;
    // DEV outcome override (QA/tests): -1 = none (roll normally), 0 = force
    // shot_down (canon cell), 1 = force escaped (surface stub). Set via the
    // `--intro-force shot_down|escaped` CLI flag / `intro_force` cvar.
    int         introForce = -1;
    bool        editorMode = false;
    bool        fxDemo = false;
    bool        fxLightning = false;   // --fx-lightning: bolt + arc-impact ACROSS the view
    bool        uiDemo = false;              std::string uiDemoPath; std::string uiDemoScreen;
    bool        dialogShot = false;
    bool        vigilShot = false;    // W4-2: --screenshot-vigil (VIGIL chat on the glass)
    bool        alertShot = false;
    bool        shotDrive = false;    // WORLD CARS: drive the nearest car through the settle
    bool        duskSky = false;      // STREET LIGHT staging: late-dusk sky (lamps carry the scene)
    bool        daySky  = false;      // UNDERWATER staging: bright-midday sky (submerged world reads lit)
    int         shotChatter = 0;      // CHATTER staging: pre-tick until N bubbles near the shot cam
    bool        captureSpire = false;        std::string captureSpireDir;
    bool        captureWings = false;        std::string captureWingsDir;
    std::string docWorldPath;
    int         cullPathArg = 0x80000000;   // INT_MIN sentinel = unset
    int         hzbArg = 0;
    int         visArg = 0x80000000;        // --vis <n> seed (vis-unify); INT_MIN = unset
    double      bootBudgetMs = 0.0;
};

}} // namespace x3::apphost
