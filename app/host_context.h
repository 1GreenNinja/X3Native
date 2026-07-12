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

    // ---- Resolution (headless = fixed 1280x720; else the windowed size) ----
    uint32_t W = 1280;
    uint32_t H = 720;

    // ---- Screenshot / capture state ----
    bool        screenshot       = false;
    std::string screenshotPath;
    int         screenshotSettle = 16;
    bool        shotCamOverride  = false;
    float       shotCam[5]       = { 8.0f, 1.75f, -0.4f, 0.06f, -0.16f };

    // ---- Stress / benchmark ----
    uint32_t stressCount = 0;
    bool     bench       = false;
    uint32_t benchFrames = 600;

    // ---- Per-host screenshot proof flags + paths (showroom family etc.) ----
    bool        carShot = false;            std::string carShotDir;
    bool        upperShot = false;          std::string upperShotDir;   // R-5: floors 2-7 proof
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
    bool        rtshShot = false;       std::string rtshShotDir;
    bool        showroomShot = false;   std::string showroomShotPath;
    bool        planetShot = false;     std::string planetShotPath;
    bool        nightskyShot = false;   std::string nightskyShotPath;
    bool        cutsceneShot = false;   std::string cutsceneShotPath;
    bool        terrainShot = false;    std::string terrainShotPath;
    bool        oceanShot = false;      std::string oceanShotPath;
    bool        oceanBaseShot = false;  std::string oceanBaseShotPath;   // W3-4 undersea base
    bool        cityShot = false;       std::string cityShotPath;        // W8-3 city vantage
    bool        captureAi = false;      std::string captureAiDir;
    bool        captureWalk = false;    std::string captureWalkPath;
    bool        captureFootIk = false;  std::string captureFootIkPath;
    std::string cutsceneFile;           // --cutscene <file>
    float       cueTime = 0.0f;         // --cuetime <s>

    // ---- Default-host (interactive render loop) extra prelude state (Phase C) ----
    bool        descVsync = true;       // DeviceDesc.vsync (smoketest UBO mirror)
    // --set <cvar> <value> pairs applied after console cvar registration.
    std::vector<std::pair<std::string, std::string>> cliCVars;
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
    bool        uiDemo = false;              std::string uiDemoPath; std::string uiDemoScreen;
    bool        dialogShot = false;
    bool        vigilShot = false;    // W4-2: --screenshot-vigil (VIGIL chat on the glass)
    bool        alertShot = false;
    bool        shotDrive = false;    // WORLD CARS: drive the nearest car through the settle
    bool        duskSky = false;      // STREET LIGHT staging: late-dusk sky (lamps carry the scene)
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
