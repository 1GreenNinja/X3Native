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

struct GLFWwindow;
namespace x3 { namespace rhi { class IRenderDevice; } }

namespace x3 { namespace apphost {

struct HostContext {
    // ---- Live objects (non-owning; main() owns them) ----
    x3::rhi::IRenderDevice* device = nullptr;   // the live render device
    GLFWwindow*             window = nullptr;   // null in headless mode

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
    bool        captureAi = false;      std::string captureAiDir;
    bool        captureWalk = false;    std::string captureWalkPath;
    bool        captureFootIk = false;  std::string captureFootIkPath;
    std::string cutsceneFile;           // --cutscene <file>
    float       cueTime = 0.0f;         // --cuetime <s>
};

}} // namespace x3::apphost
