// screenshot_hosts — headless screenshot/capture handlers lifted VERBATIM from
// main() (#28 deep split, Phase B). See screenshot_hosts.h. The only edits are
// the HostContext alias prelude + the 9 `device.get()` -> `device` (the host
// takes a raw IRenderDevice*). This TU now owns the SOLE non-inline gif.h
// include (moved from main.cpp) + its own file-local stb_image copy.
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#include "engine/core/x3_log.h"
#include "engine/core/IConsole.h"   // Lane 3: --screenshot-csm drives the r_csm cvar
#include "engine/core/x3_boot.h"
#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"
#include "engine/asset/IAssetSource.h"
#include "engine/asset/IModelLoader.h"

#include "host_context.h"
#include "screenshot_hosts.h"
#include "showroom_tod.h"
#include "cinematic.h"
#include "scene.h"
#include "mesh_prims.h"
#include "env_art.h"
#include "monster.h"
#include "anim.h"
#include "terrain.h"
#include "app_run.h"        // applyRtaoCVarsForTest: pushes r_csm to the device
#include "engine/rhi/Csm.h" // kNumCascades (perf receipt line)
#include "ocean_base.h"        // W3-4: --screenshot-oceanbase undersea vantage
#include "city.h"              // W8-3: --screenshot-city district vantage
#include "street_lights.h"     // content wiring: --screenshot-city night lamp grid
#include "engine/rhi/ClusterLights.h"  // kMaxSceneLights
#include "cutscene.h"
#include "leveldoc_world.h"
#include "level_loader.h"
#include "canon_play.h"        // R-5: --screenshot-upperfloors content proof
#include "level1.h"            // F2 rescue rooms: buildLevel1 / Level1ArtMask (--screenshot-rescuerooms)
#include "wing_dressing.h"     // F2-F7 wing dressing (--screenshot-rescuerooms)
#include "editor/editor_host.h"
#include "factory_annex.h"     // --capture-factory: the Confection Annex bore ride
#include "elevator.h"          // --capture-factory: the Anywhere Elevator cab
#include "trigger.h"           // --capture-factory: the annex trigger volumes
#include "world_hosts/world_host_common.h"   // applyHostRenderCVars / reportUnappliedHostCVars
#include "asset_root.h"
#include "intro_orchestrator.h"   // X3_INTRO_CAPTURE headless combat-evidence run

#include <memory>
#include <string>
#include <vector>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <filesystem>

// Public-domain single-header GIF encoder (Charlie Tangora) — vendored under
// third_party/gif_h. Used ONLY by the headless --capture-ai handler to assemble
// the captured PNG frame sequence into an animated GIF. This is the SOLE
// translation unit that includes gif.h, so its (non-inline) functions link cleanly.
#if defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning(disable : 4334)
#endif
#include "../third_party/gif_h/gif.h"
#if defined(_MSC_VER)
#  pragma warning(pop)
#endif
// stb_image (read the captured PNGs back for GIF assembly) — file-local copy.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#if defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning(disable : 4244 4456 4457)
#endif
#include <stb_image.h>
#if defined(_MSC_VER)
#  pragma warning(pop)
#endif

namespace x3 { namespace apphost {

// The capture handlers themselves, verbatim. Renamed to *Impl and made static so
// dispatchScreenshotHosts below can wrap ALL of them at once — see the note there
// for why a per-handler call would not have worked.
static int dispatchScreenshotHostsImpl(HostContext& hc) {
    auto* device = hc.device;
    GLFWwindow* window = hc.window;
    const bool headless = hc.headless;
    const bool screenshot = hc.screenshot;
    const uint32_t W = hc.W;
    const uint32_t H = hc.H;
    const bool ddgiForce = hc.ddgiForce;
    const float cueTime = hc.cueTime;
    const std::string& cutsceneFile = hc.cutsceneFile;
    const bool editorShot = hc.editorShot;        const std::string& editorShotPath = hc.editorShotPath;
    const bool loaderShot = hc.loaderShot;        const std::string& loaderShotPath = hc.loaderShotPath;
    const bool skyShot = hc.skyShot;              const std::string& skyShotPath = hc.skyShotPath;
    const bool ddgiShot = hc.ddgiShot;            const std::string& ddgiShotDir = hc.ddgiShotDir;
    const bool reflVerifyShot = hc.reflVerifyShot; const std::string& reflVerifyShotDir = hc.reflVerifyShotDir;
    const bool rtMatVerifyShot = hc.rtMatVerifyShot; const std::string& rtMatVerifyShotDir = hc.rtMatVerifyShotDir;
    const bool rtshShot = hc.rtshShot;            const std::string& rtshShotDir = hc.rtshShotDir;
    const bool showroomShot = hc.showroomShot;    const std::string& showroomShotPath = hc.showroomShotPath;
    const bool carShot = hc.carShot;              const std::string& carShotDir = hc.carShotDir;
    const bool upperShot = hc.upperShot;          const std::string& upperShotDir = hc.upperShotDir;
    const bool doorShot  = hc.doorShot;           const std::string& doorShotDir  = hc.doorShotDir;
    const bool rescueShot = hc.rescueShot;        const std::string& rescueShotDir = hc.rescueShotDir;
    const bool planetShot = hc.planetShot;        const std::string& planetShotPath = hc.planetShotPath;
    const bool nightskyShot = hc.nightskyShot;    const std::string& nightskyShotPath = hc.nightskyShotPath;
    const bool cutsceneShot = hc.cutsceneShot;    const std::string& cutsceneShotPath = hc.cutsceneShotPath;
    const bool terrainShot = hc.terrainShot;      const std::string& terrainShotPath = hc.terrainShotPath;
    const bool csmShot = hc.csmShot;              const std::string& csmShotDir = hc.csmShotDir;
    const bool oceanShot = hc.oceanShot;          const std::string& oceanShotPath = hc.oceanShotPath;
    const bool oceanBaseShot = hc.oceanBaseShot;  const std::string& oceanBaseShotPath = hc.oceanBaseShotPath;
    const bool cityShot = hc.cityShot;            const std::string& cityShotPath = hc.cityShotPath;
    const bool captureAi = hc.captureAi;          const std::string& captureAiDir = hc.captureAiDir;
    const bool captureFactory = hc.captureFactory; const std::string& captureFactoryPath = hc.captureFactoryPath;
    const bool captureCrowdSpread = hc.captureCrowdSpread; const std::string& captureCrowdSpreadDir = hc.captureCrowdSpreadDir;
    const bool captureWalk = hc.captureWalk;      const std::string& captureWalkPath = hc.captureWalkPath;
    const bool captureFootIk = hc.captureFootIk;  const std::string& captureFootIkPath = hc.captureFootIkPath;

    // ==== VERBATIM handler bodies (device.get() -> device) ====
    // ---- Headless INTRO-COMBAT evidence capture (X3_INTRO_CAPTURE=<dir>) ---------
    // Renders the interactive space-combat cold-open beats OFFSCREEN through the
    // intro orchestrator's built-in captureMode staging (scripted flight + staged
    // enemy damage + fire windows), writing live_<beat>_s<step>.png frames into
    // <dir>. The per-enemy DAMAGE METER HUD, the ship-death DISINTEGRATION blast +
    // GPU debris, and the wing fire all render into these PNGs — the eyeball gate
    // for the space power-fantasy pass. Env-gated + headless-only: inert in every
    // normal run (nothing sets X3_INTRO_CAPTURE). runInteractiveIntro is self-
    // contained (own physics; null-audio safe) and skips the cinematic clips when
    // there is no window, so only the combat beats render.
    if (const char* introCapDir = std::getenv("X3_INTRO_CAPTURE");
        introCapDir && *introCapDir && headless && device) {
        x3::logInfo(std::string("[intro-capture]: rendering intro combat evidence -> ") + introCapDir);
        std::error_code ec;
        std::filesystem::create_directories(introCapDir, ec);
        (void)x3::intro::runInteractiveIntro(hc);
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return 0;
    }
    // ---- Headless editor PROOF (--screenshot-editor [path.png]) ------------------
    // Inits ImGui in the headless device (a hidden GLFW window backs the GLFW backend;
    // rendering goes into the offscreen color image), renders ONE frame with the
    // dockspace + demo window, and captures a PNG so the Phase-0 integration can be
    // verified without a display. Offscreen + one-shot, like --screenshot.
    if (editorShot) {
        x3::logInfo("--screenshot-editor: ImGui Phase-0 proof -> " + editorShotPath);
        // Ensure the output directory exists (build/proof/).
        {
            std::error_code ec;
            std::filesystem::path outp(editorShotPath);
            if (outp.has_parent_path())
                std::filesystem::create_directories(outp.parent_path(), ec);
        }
        // A HIDDEN GLFW window backs ImGui's GLFW backend (no surface is used — the
        // device is headless and renders into its offscreen target).
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        GLFWwindow* proofWin = glfwCreateWindow(static_cast<int>(W), static_cast<int>(H),
                                                "X3Engine-editor-proof", nullptr, nullptr);
        glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);  // restore the default for any later window
        if (!proofWin) {
            x3::logError("--screenshot-editor: hidden GLFW window create failed");
            device->shutdown();
            glfwTerminate();
            return 1;
        }
        device->initEditorUI(proofWin);
        if (!device->editorUIActive()) {
            x3::logError("--screenshot-editor: ImGui init failed");
            glfwDestroyWindow(proofWin);
            device->shutdown();
            glfwTerminate();
            return 1;
        }
        // Phase 1/2 PROOF: stand up a real EditorHost over a minimal Scene + physics,
        // place a couple of blockout BOXES + a RAMP (clean grid material), pose the
        // camera at them, and render one frame -> PNG showing the DOCKSPACE + the
        // editor panels + the grid-material brushes in the viewport.
        x3::game::Scene proofScene;
        x3::phys::IPhysicsWorld* proofPhys = x3::phys::createPhysicsWorld();
        proofPhys->init();
        x3::editor::EditorHost proofHost;
        proofHost.init(*device, proofScene, *proofPhys, proofWin);
        {
            float p0[3] = { 0.0f, 0.0f, 0.0f }, s0[3] = { 10.0f, 0.5f, 10.0f }; // floor plate
            float p1[3] = { -3.5f, 1.0f, 1.0f }, s1[3] = { 2.0f, 2.0f, 2.0f };  // a box
            float p2[3] = { 3.5f, 1.0f, 1.5f }, s2[3] = { 3.0f, 2.0f, 4.0f };   // a ramp
            float p3[3] = { -3.5f, 1.5f, -3.5f }, s3[3] = { 2.0f, 3.0f, 2.0f }; // a CYLINDER
            float p4[3] = { 0.5f, 1.0f, -3.0f }, s4[3] = { 3.0f, 2.0f, 4.0f };  // STAIRS
            proofHost.placeBrush(0u, p0, s0, *device, proofScene, *proofPhys);
            proofHost.placeBrush(0u, p1, s1, *device, proofScene, *proofPhys);
            proofHost.placeBrush(1u, p2, s2, *device, proofScene, *proofPhys);
            proofHost.placeBrush(2u, p3, s3, *device, proofScene, *proofPhys);
            proofHost.placeBrush(3u, p4, s4, *device, proofScene, *proofPhys);
            // Feature 3 proof: place a GLB prop (renders via renderModels each frame).
            proofHost.placeModel("SciFi_Warehouse_Kit/Barrel.glb", *device);
        }
        // X3_EDITOR_CAM=orbit|walk|fly — a headless proof cannot click a menu, so this is
        // how the Status panel's camera-mode readout gets exercised for the visual gate.
        if (const char* cm = std::getenv("X3_EDITOR_CAM")) {
            const std::string c = cm;
            if (c == "orbit") proofHost.dispatchCmd(x3::editor::Cmd::CamOrbit);
            else if (c == "walk") proofHost.dispatchCmd(x3::editor::Cmd::CamFpsWalk);
            else proofHost.dispatchCmd(x3::editor::Cmd::CamFly);
        }
        // A pleasant 3/4 vantage on the brushes; a touch of ambient so the grey reads.
        device->setCamera(9.0f, 7.5f, 12.0f, -2.30f, -0.42f, 60.0f);
        device->setAmbient(0.55f, 0.56f, 0.58f);
        // Render a few settle frames (font upload + draw-data), capturing the last one.
        bool ok = false;
        const int kProofFrames = 4;
        for (int f = 0; f < kProofFrames; ++f) {
            const bool lastFrame = (f == kProofFrames - 1);
            if (lastFrame) device->armCapture(editorShotPath.c_str());
            glfwPollEvents();
            auto frame = device->beginFrame();
            if (frame.valid) {
                proofScene.render(*device, frame);          // the grid-material brushes
                proofHost.renderModels(*device, frame);     // Feature 3 GLB props
                device->beginEditorUI();                    // dockspace root (device)
                proofHost.draw(*device, proofScene, *proofPhys, 1.0f/60.0f);  // panels (host)
                device->endEditorUI();                      // ImGui::Render + stash draw data
                device->endFrame(frame);
            }
            if (lastFrame) ok = device->captureFrame(editorShotPath.c_str());
        }
        device->shutdownEditorUI();
        proofPhys->shutdown();
        delete proofPhys;
        glfwDestroyWindow(proofWin);
        device->shutdown();
        glfwTerminate();
        x3::logInfo(ok ? "--screenshot-editor: wrote " + editorShotPath
                       : "--screenshot-editor: capture FAILED");
        return ok ? 0 : 1;
    }

    // ---- F2 RESCUE-ROOM closeups (--screenshot-rescuerooms [dir]) ----------------
    // The owner's signature Floor-2 medical rescue rooms. Build the Spire (buildLevel1)
    // with the graybox surfaces suppressed where the GLB art covers, run the F2-F7 wing
    // dressing, then pose the camera INSIDE each of the three side-by-side rescue rooms
    // (Aria / Keisha / Emily) and capture w2d_rescue_{a,b,c}.png — the centered bed, the
    // restrained captive, the monitors, and (Room B) the RED magnetic-seal door. Mirrors
    // the --capture-wings lighting recipe (interior ambient + IBL probe + the dressing's
    // own motivated keys) so the white clinical props read. Headless one-shot; NO HUD /
    // weapon viewmodel (unlike the gameplay --screenshot), and lit for the F2 plate the
    // camera stands on. Exits after.
    if (rescueShot) {
        namespace fs = std::filesystem;
        std::error_code mkec; fs::create_directories(rescueShotDir, mkec);
        x3::logInfo("--screenshot-rescuerooms: building Spire + F2 wing dressing -> " + rescueShotDir);

        std::unique_ptr<x3::phys::IPhysicsWorld> rphys(x3::phys::createPhysicsWorld());
        rphys->init();
        x3::game::Scene rscene;
        x3::game::Level1ArtMask mask; mask.walls = true; mask.floors = true;   // dressing owns surfaces
        x3::game::Level1Layout L = x3::game::buildLevel1(rscene, *device, *rphys, mask);
        (void)L;
        x3::game::WingDressing wd;
        const bool built = wd.build(*device, x3::game::assetRoot() + "/surface_library",
                                    x3::game::convertedGlbRoot());
        if (!built) x3::logWarn("--screenshot-rescuerooms: wing dressing did not build (bare graybox)");

        // Owner note #2c: the capture rig was over-lifting the room (exposure 1.35 +
        // ambient 0.34 + huge 4-5 fill/key point lights) — everything washed out flat.
        // Pull the exposure + ambient DOWN toward a moody clinical read so the corners
        // fall into shadow and the softened pendant + cool wall coves shape the space.
        device->setAmbient(0.19f, 0.20f, 0.24f);
        device->setIblProbe(true);
        device->setExposure(1.05f);
        // Owner note #3 (the floor "jagged rounded shadow"): the DEFAULT RT-shadow tier
        // is 2 (sun + POINT LIGHTS cast). Every fill/cove/pendant point light throws a
        // ray-traced shadow of the bed FRAME, and in this short headless capture the
        // jittered penumbra hasn't TAA-converged — that noise reads as pixelated radial
        // SPOKES on the floor. The soft contact-shadow discs already ground the props, so
        // drop to tier 1 (sun-only) for a clean clinical still. (Live gameplay keeps tier
        // 2; its penumbra converges over many frames and reads soft, not jagged.)
        { x3::rhi::IRenderDevice::RtShadowParams rss; rss.tier = 0; device->setRtShadowParams(rss); }
        // SSGI is a TEMPORAL screen-space gather (EMA history, 16 noisy taps/frame). This
        // headless still renders only ~18 frames, so its history never converges and the
        // raw noise streaks into pixelated radial SPOKES on the floor at grazing angles —
        // the true source of the "jagged shadow" residual. Disable the GI chain for the
        // still (SSAO stays on for the corner-contact grounding). Live gameplay keeps SSGI.
        { x3::rhi::IRenderDevice::GiParams gi; gi.enabled = false; device->setGiParams(gi); }
        // The REAL "jagged floor shadow" (owner note #3): the capture never repositions
        // the sun, so the engine's DEFAULT directional sun (normalize(0.4,1,0.3)) both
        // FLOODS the room and casts a hard, low-res CSM shadow of the bed's SLATTED frame
        // — parallel slat shadows that perspective-converge into a pixelated radial FAN.
        // A windowless clinical room has no sun: park it BELOW THE HORIZON (the cell-
        // capture technique) so N.L<=0 everywhere — no sun flood, no sun shadow. The room
        // is then lit purely by ambient + IBL + the motivated pendant/cove lamps (moody
        // clinical read) and the props ground on their soft contact discs alone.
        {
            x3::rhi::IRenderDevice::SkyParams sky;
            sky.enabled = false;                                  // indoor: no sky visual
            sky.sunDir[0] = 0.0f; sky.sunDir[1] = -1.0f; sky.sunDir[2] = 0.01f;  // below horizon
            sky.sunIntensity = 0.0f;
            device->setSkyParams(sky);
        }

        struct RShot { const char* tag; float cx, cz; };
        const float fY = 10.0f;                 // F2 plate floor Y
        const RShot rooms[3] = {
            { "a", -27.0f, 6.75f },   // Rescue Room A (Aria)
            { "b", -35.0f, 6.75f },   // Rescue Room B (Keisha) — magnetic seal
            { "c", -43.0f, 6.75f },   // Rescue Room C (Emily)
        };
        bool allOk = true;
        // One capture from a given eye + look-at, tagged w2f_<room>_<suffix>.png. The
        // dressing's motivated floor keys + a soft camera fill + a warm surgical key over
        // the bed so the captive + straps read on this dark clinical plate.
        auto shoot = [&](const RShot& s, const char* suffix,
                         float ex, float ey, float ez,
                         float ax, float ay, float az) -> bool {
            const float dx = ax - ex, dy = ay - ey, dz = az - ez;
            const float yaw = std::atan2(dz, dx);
            const float pitch = std::atan2(dy, std::sqrt(dx * dx + dz * dz));
            const x3::phys::Vec3 eye{ ex, ey, ez };
            std::vector<x3::rhi::PointLight> pls;
            wd.collectFloorLights(eye, pls);
            // Soft COOL camera fill (was a 4.4-5.0 flood that blew the room out): a low
            // cool bounce so the near side of the captive reads without erasing shadow.
            x3::rhi::PointLight fill;
            fill.pos[0] = ex; fill.pos[1] = fY + 2.4f; fill.pos[2] = ez;
            fill.range = 11.0f; fill.color[0] = 1.35f; fill.color[1] = 1.45f; fill.color[2] = 1.65f;
            pls.push_back(fill);
            // A modest WARM over-bed key (was 5.4 near-white blowout) — the recipe's own
            // softened pendant now carries the room, this just guarantees the bed reads.
            x3::rhi::PointLight key;
            key.pos[0] = s.cx; key.pos[1] = fY + 2.9f; key.pos[2] = s.cz;
            key.range = 7.0f; key.color[0] = 2.45f; key.color[1] = 2.35f; key.color[2] = 2.05f;
            pls.push_back(key);
            device->setPointLights(pls.data(), (uint32_t)pls.size());

            const std::string outPath =
                rescueShotDir + "/w2f_" + std::string(s.tag) + "_" + suffix + ".png";
            const int kSettle = 18;
            for (int i = 0; i < kSettle; ++i) {
                glfwPollEvents();
                device->setCamera(ex, ey, ez, yaw, pitch, 78.0f);
                if (i == kSettle - 1) device->armCapture(outPath.c_str());
                auto frame = device->beginFrame();
                if (frame.valid) {
                    rscene.render(*device, frame);
                    wd.draw(*device, frame, eye);
                }
                device->endFrame(frame);
            }
            const bool wrote = device->captureFrame(outPath.c_str());
            if (wrote) x3::logInfo("--screenshot-rescuerooms: wrote " + outPath);
            else x3::logError("--screenshot-rescuerooms: FAILED " + outPath);
            return wrote;
        };
        for (const RShot& s : rooms) {
            // (1) DOOR: a 3/4 vantage from the door corner, across the bed toward the captive.
            allOk &= shoot(s, "door", s.cx + 2.3f, fY + 1.95f, 3.9f,
                                      s.cx - 0.2f, fY + 0.75f, s.cz + 0.6f);
            // (2) FOOT: bedside, ~1.4 m up at the FOOT/door end (low z), looking straight
            // along the bed toward the +Z pillow. This makes head/foot orientation
            // UNAMBIGUOUS: the raised knees read nearest the camera, the torso recedes,
            // and the head lands on the FAR pillow — a reversed body would show her face
            // filling the near frame instead.
            allOk &= shoot(s, "foot", s.cx + 0.9f, fY + 1.40f, s.cz - 2.40f,
                                      s.cx,        fY + 0.70f, s.cz + 1.00f);
        }
        rphys->shutdown();
        device->shutdown();
        glfwTerminate();
        x3::logInfo(std::string("--screenshot-rescuerooms: ") +
                    (allOk ? "all three rescue rooms captured" : "one or more captures FAILED"));
        return allOk ? 0 : 1;
    }

    // ---- Headless LOADER proof (--screenshot-loader [path.png]) ----------------
    // The data-driven LevelDoc loader's render proof: author the sample LevelDoc,
    // SAVE it to disk, LOAD it back through the REAL loader (file -> parse -> brushes
    // + materials + props + lights + trigger -> live device meshes + Jolt bodies),
    // pose the camera inside the built room, capture a PNG, tear everything down.
    // Offscreen + one-shot, like --screenshot.
    if (loaderShot) {
        x3::logInfo("--screenshot-loader: LevelDoc loader proof -> " + loaderShotPath);
        {
            std::error_code ec;
            std::filesystem::path outp(loaderShotPath);
            if (outp.has_parent_path())
                std::filesystem::create_directories(outp.parent_path(), ec);
            std::filesystem::create_directories("build/proof", ec);
        }
        // Author + save the sample doc, then load it through the real file path so
        // the proof exercises the EXACT pipeline `--world fromdoc` boots.
        const char* kDocPath = "build/proof/loader_sample_room.json";
        x3::editor::LevelDoc sample = x3::game::makeSampleLevelDoc();
        if (!sample.saveJson(kDocPath)) {
            x3::logError("--screenshot-loader: could not write " + std::string(kDocPath));
            device->shutdown(); glfwTerminate(); return 1;
        }
        x3::game::Scene proofScene;
        x3::phys::IPhysicsWorld* proofPhys = x3::phys::createPhysicsWorld();
        proofPhys->init();
        x3::game::LevelDocWorld proofDoc;
        bool built = proofDoc.loadFromFile(kDocPath, proofScene, *device, *proofPhys);
        bool ok = false;
        if (built) {
            // A 3/4 vantage inside the room: ramp + ledge + hazard pillar in frame.
            float ps[3]; proofDoc.playerStart(ps);
            device->setCamera(ps[0] + 4.0f, ps[1] + 2.6f, ps[2] + 1.5f, -2.05f, -0.30f, 65.0f);
            device->setAmbient(0.42f, 0.43f, 0.47f);
            std::vector<x3::rhi::PointLight> pls;
            proofDoc.selectLights(ps[0], ps[1], ps[2], pls, 16);
            device->setPointLights(pls.data(), (uint32_t)pls.size());
            const int kFrames = 8;   // settle so shadows/SSAO register
            for (int f = 0; f < kFrames; ++f) {
                const bool last = (f == kFrames - 1);
                if (last) device->armCapture(loaderShotPath.c_str());
                glfwPollEvents();
                auto frame = device->beginFrame();
                if (frame.valid) {
                    proofScene.render(*device, frame);
                    device->endFrame(frame);
                }
                if (last) ok = device->captureFrame(loaderShotPath.c_str());
            }
        } else {
            x3::logError("--screenshot-loader: loader BUILD failed");
        }
        proofDoc.shutdown(proofScene, *device, *proofPhys);
        proofPhys->shutdown();
        delete proofPhys;
        device->shutdown();
        glfwTerminate();
        x3::logInfo(ok ? "--screenshot-loader: wrote " + loaderShotPath
                       : "--screenshot-loader: capture FAILED");
        return ok ? 0 : 1;
    }

    // ---- Sky vantage mode (--screenshot-sky [path.png]) --------------------
    // The open-world sky's first verification gate. EFLZ Level 1 is an enclosed
    // interior, so the sky never shows there; this builds a MINIMAL outdoor scene
    // (a large checkered ground plane + a couple of boxes for ground-shadow proof)
    // entirely through the public render API — no game/physics/audio stack — turns
    // ON the analytic sky with the engine's existing sun direction + color, poses
    // the camera at the horizon looking slightly up toward the sun, settles a few
    // frames so the shadow map + sky register, captures a PNG, and exits.
    if (skyShot) {
        x3::logInfo("--screenshot-sky: rendering outdoor sky vantage to " + skyShotPath);

        // Ground plane (large XZ quad) with a tiled checker so the horizon + ground
        // shadows read clearly. A neutral mid-grey/green checker reads as terrain.
        std::vector<x3::rhi::MeshVertex> gv; std::vector<uint32_t> gi;
        x3::prims::makeGroundQuad(/*half=*/400.0f, /*tiles=*/200.0f, gv, gi);
        x3::rhi::MeshHandle ground = device->createMesh(gv.data(), (uint32_t)gv.size(),
                                                        gi.data(), (uint32_t)gi.size());
        auto checker = x3::prims::makeCheckerRGBA(64, 8, 150, 165, 150, 70, 90, 75);
        x3::rhi::TextureHandle groundTex = device->createTexture(checker.data(), 64, 64, /*srgb=*/true);

        // A few boxes sitting on the ground so the sun casts visible shadows (proves
        // the sky's sun direction matches the lighting/shadow pass).
        x3::prims::PrimMesh boxM = x3::prims::makeBox(1.5f, 1.5f, 1.5f, 0, 1.5f, 0, 0.5f);
        x3::rhi::MeshHandle box = device->createMesh(boxM.verts.data(), (uint32_t)boxM.verts.size(),
                                                     boxM.index.data(), (uint32_t)boxM.index.size());
        auto boxPx = x3::prims::makeSolidRGBA(4, 200, 200, 205);
        x3::rhi::TextureHandle boxTex = device->createTexture(boxPx.data(), 4, 4, /*srgb=*/true);

        // Turn ON the analytic sky with the SAME sun the shadow pass + mesh.frag use
        // (normalize(0.4,1,0.3)) and a sun color matching mesh.frag's kSunColor, so
        // the disk in the sky sits exactly where the world is lit from.
        x3::rhi::IRenderDevice::SkyParams sp{};
        sp.enabled = true;
        sp.sunDir[0] = 0.4f; sp.sunDir[1] = 1.0f; sp.sunDir[2] = 0.3f;
        sp.sunColor[0] = 1.0f; sp.sunColor[1] = 0.97f; sp.sunColor[2] = 0.92f;
        sp.sunIntensity = 1.0f; sp.haze = 0.5f; sp.exposure = 1.0f;
        device->setSkyParams(sp);

        // Camera: stand at eye height aimed toward the sun's azimuth and pitched UP
        // toward its elevation so the sun disk + glow land in-frame with the
        // gradient above and the horizon + ground shadows at the bottom. The sun
        // dir is normalize(0.4,1,0.3): azimuth = atan2(0.3,0.4) in XZ, elevation =
        // asin(0.898) ~ 64deg. Aim the yaw at the azimuth and pitch partway up to
        // its elevation (a touch lower than the sun so the horizon stays visible).
        const float sunYaw   = std::atan2(0.3f, 0.4f);  // toward the sun in XZ
        const float camPitch = 0.52f;                   // ~30deg up: sun upper frame, horizon at bottom
        device->setCamera(0.0f, 1.7f, 16.0f, sunYaw, camPitch, 72.0f);

        const float modelGround[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        const float modelBoxA[16]   = { 1,0,0,0, 0,1,0,0, 0,0,1,0,  -6.0f, 0.0f,  2.0f, 1 };
        const float modelBoxB[16]   = { 1,0,0,0, 0,1,0,0, 0,0,1,0,   5.0f, 0.0f, -3.0f, 1 };
        const float white[4]  = { 1, 1, 1, 1 };
        const float gtint[4]  = { 1, 1, 1, 1 };

        const int kSettle = 12;
        for (int i = 0; i < kSettle; ++i) {
            glfwPollEvents();
            if (i == kSettle - 1) device->armCapture(skyShotPath.c_str());
            auto frame = device->beginFrame();
            if (frame.valid) {
                device->drawMesh(frame, ground, groundTex, gtint, modelGround);
                device->drawMesh(frame, box, boxTex, white, modelBoxA);
                device->drawMesh(frame, box, boxTex, white, modelBoxB);
            }
            device->endFrame(frame);
        }
        const bool wrote = device->captureFrame(skyShotPath.c_str());
        if (wrote) x3::logInfo("--screenshot-sky: wrote " + skyShotPath);
        else       x3::logError("--screenshot-sky: capture FAILED");

        device->shutdown();
        glfwDestroyWindow(window);
        glfwTerminate();
        return wrote ? 0 : 1;
    }

    // ---- DDGI gate-shot proof (--screenshot-ddgi [outDir]) ------------------
    // THE GATE SHOT for r_ddgi: a sealed two-room rig where room A holds the only
    // light sources (a warm point light + an emissive ceiling panel) and room B
    // connects to it ONLY through a doorway. With r_ddgi 0, B is near-black (flat
    // ambient only); with r_ddgi 1, the probe field carries the bounce through the
    // doorway and B reads warm — honest traced GI, no screen-space dependence.
    // Room C sits SEALED beside A (full walls): the leak canary — Chebyshev
    // visibility weighting must keep it black with DDGI on. Captures:
    //   ddgi_corridor_off/on.png  — camera in room B (the money A/B)
    //   ddgi_leak_off/on.png      — camera in sealed room C (must stay dark)
    //   ddgi_probes_debug.png     — r_ddgi_debug 1 irradiance-field view (room B)
    // Headless, like --screenshot-sky; exits after the captures.
    if (ddgiShot) {
        namespace fs = std::filesystem;
        std::error_code mkec; fs::create_directories(ddgiShotDir, mkec);
        x3::logInfo("--screenshot-ddgi: rendering DDGI gate shots to " + ddgiShotDir);

        // ---- Geometry: floor/ceiling shell + room walls from solid boxes (their
        // outward faces are the room interiors; back-face culling + DDGI backface
        // handling are both happy with closed slabs). Floor top at y=0. ----
        std::vector<x3::prims::PrimMesh> parts;
        parts.push_back(x3::prims::makeBox(9.5f, 0.5f, 8.5f,  0.0f, -0.5f, 3.5f)); // floor slab
        parts.push_back(x3::prims::makeBox(9.5f, 0.5f, 8.5f,  0.0f,  4.5f, 3.5f)); // ceiling slab
        // Walls run y -0.15..4.15 (half 2.15) so they OVERLAP the floor +
        // ceiling slabs — no coplanar seam for the shadow map's PCF bias to
        // leak a sunlit strip through at the junction.
        parts.push_back(x3::prims::makeBox(0.5f, 2.15f, 8.5f, -9.0f,  2.0f, 3.5f)); // west shell
        parts.push_back(x3::prims::makeBox(0.5f, 2.15f, 8.5f,  9.0f,  2.0f, 3.5f)); // east shell
        parts.push_back(x3::prims::makeBox(9.5f, 2.15f, 0.5f,  0.0f,  2.0f, -4.5f)); // south shell
        parts.push_back(x3::prims::makeBox(9.5f, 2.15f, 0.5f,  0.0f,  2.0f, 11.5f)); // north shell
        // A|B divider (x=0) with a 2 m doorway at z in [-1,1], ~3 m tall:
        parts.push_back(x3::prims::makeBox(0.5f, 2.15f, 1.5f,  0.0f,  2.0f, -2.5f)); // divider south seg
        parts.push_back(x3::prims::makeBox(0.5f, 2.15f, 1.5f,  0.0f,  2.0f,  2.5f)); // divider north seg
        parts.push_back(x3::prims::makeBox(0.5f, 0.65f, 1.0f,  0.0f,  3.55f, 0.0f)); // doorway lintel (2.9..4.2)
        // A|C separator (z=4..5, FULL span — room C is sealed; the leak canary):
        parts.push_back(x3::prims::makeBox(9.5f, 2.15f, 0.5f,  0.0f,  2.0f,  4.5f));
        // C | east-void divider (x=0, z 5..11) so C is a closed room:
        parts.push_back(x3::prims::makeBox(0.5f, 2.15f, 3.0f,  0.0f,  2.0f,  8.0f));
        // RED accent wall inside room A (color-bleed proof: B's spill reads warm-red):
        x3::prims::PrimMesh redPanel = x3::prims::makeBox(0.1f, 1.8f, 3.5f, -8.3f, 1.9f, 0.0f);

        std::vector<x3::rhi::MeshHandle> partMesh;
        for (auto& p : parts)
            partMesh.push_back(device->createMesh(p.verts.data(), (uint32_t)p.verts.size(),
                                                  p.index.data(), (uint32_t)p.index.size()));
        x3::rhi::MeshHandle redMesh = device->createMesh(redPanel.verts.data(), (uint32_t)redPanel.verts.size(),
                                                         redPanel.index.data(), (uint32_t)redPanel.index.size());
        // Emissive ceiling panel in room A:
        x3::prims::PrimMesh panel = x3::prims::makeBox(1.5f, 0.05f, 1.5f, -4.5f, 3.9f, 0.0f);
        x3::rhi::MeshHandle panelMesh = device->createMesh(panel.verts.data(), (uint32_t)panel.verts.size(),
                                                           panel.index.data(), (uint32_t)panel.index.size());

        auto greyPx = x3::prims::makeSolidRGBA(4, 200, 200, 200);
        x3::rhi::TextureHandle greyTex = device->createTexture(greyPx.data(), 4, 4, /*srgb=*/true);

        // Lights: room A only. The point light gives A its direct look; the
        // emissive panel is the DYNAMIC GI source the probes must pick up.
        x3::rhi::PointLight pl{};
        pl.pos[0] = -4.5f; pl.pos[1] = 3.2f; pl.pos[2] = 0.0f; pl.range = 10.0f;
        pl.color[0] = 3.0f; pl.color[1] = 2.6f; pl.color[2] = 2.0f;
        device->setPointLights(&pl, 1);
        device->setAmbient(0.015f, 0.016f, 0.020f);          // near-black base ambient
        device->setShadowBounds(0.0f, 2.0f, 3.5f, 25.0f);
        // Sun BELOW the horizon (sky stays disabled): every surface's sun N.L is
        // <= 0, so the only light in the rig is the room-A point light + the
        // emissive panel — the purest possible bounce-only A/B (this also avoids
        // the engine's shadow-bias seam at wall/ceiling junctions muddying the
        // leak canary; that seam is a raster artifact identical OFF and ON).
        {
            x3::rhi::IRenderDevice::SkyParams sp{};
            sp.enabled = false;
            sp.sunDir[0] = 0.0f; sp.sunDir[1] = -1.0f; sp.sunDir[2] = 0.01f;
            device->setSkyParams(sp);
        }

        const float identity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        const float white[4] = { 1, 1, 1, 1 };
        const float red[4]   = { 0.85f, 0.08f, 0.06f, 1.0f };
        const float panelTint[4] = { 1.0f, 0.9f, 0.7f, 1.0f };
        const float panelEmissive[4] = { 1.0f, 0.85f, 0.6f, 10.0f };  // HDR glow (GI source)

        auto drawScene = [&](const x3::rhi::FrameContext& f) {
            for (auto m : partMesh) device->drawMesh(f, m, greyTex, white, identity);
            device->drawMesh(f, redMesh, greyTex, red, identity);
            device->drawMeshEmissive(f, panelMesh, greyTex, panelTint, panelEmissive, identity);
        };
        float gpuMsAccum = 0.0f; int gpuMsN = 0;
        auto renderFrames = [&](int n, const std::string& capturePath) -> bool {
            for (int i = 0; i < n; ++i) {
                glfwPollEvents();
                if (!capturePath.empty() && i == n - 1) device->armCapture(capturePath.c_str());
                auto f = device->beginFrame();
                if (f.valid) drawScene(f);
                device->endFrame(f);
                gpuMsAccum += device->stats().gpuFrameMs; ++gpuMsN;
            }
            if (capturePath.empty()) return true;
            const bool ok = device->captureFrame(capturePath.c_str());
            x3::logInfo(std::string(ok ? "--screenshot-ddgi: wrote " : "--screenshot-ddgi: FAILED ") + capturePath);
            return ok;
        };
        // Camera poses: room B looking through at the doorway wall; sealed room C.
        auto camRoomB = [&]() { device->setCamera(7.2f, 1.7f, 3.0f, std::atan2(-3.0f, -7.2f), -0.03f, 72.0f); };
        auto camRoomC = [&]() { device->setCamera(-4.5f, 1.7f, 9.8f, std::atan2(-4.8f, 0.0f), -0.03f, 72.0f); };

        bool ok = true;
        // ---- OFF baselines (r_ddgi 0 — the device default). ----
        camRoomB(); ok &= renderFrames(20, ddgiShotDir + "/ddgi_corridor_off.png");
        gpuMsAccum = 0.0f; gpuMsN = 0;
        camRoomB(); ok &= renderFrames(40, "");
        const float gpuOff = gpuMsAccum / std::max(1, gpuMsN);
        camRoomC(); ok &= renderFrames(10, ddgiShotDir + "/ddgi_leak_off.png");

        // ---- ON: explicit probe volume over the rig (the auto-fit AABB is
        // origin-based and this rig bakes its boxes at identity), 20x6x20. ----
        x3::rhi::IRenderDevice::DdgiParams dp{};
        dp.enabled = true;
        dp.countX = 20; dp.countY = 6; dp.countZ = 20;
        dp.originX = -9.5f; dp.originY = -1.0f; dp.originZ = -5.0f;
        dp.sizeX = 19.0f; dp.sizeY = 6.0f; dp.sizeZ = 17.0f;
        dp.raysPerProbe = 128;
        device->setDdgiParams(dp);
        camRoomB(); ok &= renderFrames(150, ddgiShotDir + "/ddgi_corridor_on.png");
        gpuMsAccum = 0.0f; gpuMsN = 0;
        camRoomB(); ok &= renderFrames(40, "");
        const float gpuOn = gpuMsAccum / std::max(1, gpuMsN);
        // Probe-field debug visualization (r_ddgi_debug 1):
        dp.debug = 1; device->setDdgiParams(dp);
        camRoomB(); ok &= renderFrames(4, ddgiShotDir + "/ddgi_probes_debug.png");
        dp.debug = 0; device->setDdgiParams(dp);
        // The no-leak canary: sealed room C must STAY dark with DDGI on.
        camRoomC(); ok &= renderFrames(30, ddgiShotDir + "/ddgi_leak_on.png");
        // DYNAMIC proof: remove the point light mid-run — the probes re-converge
        // (hysteresis, ~1-2 s) to the EMISSIVE PANEL as the only GI source. The
        // doorway spill must survive (dimmer, panel-toned) purely from emissive.
        device->setPointLights(nullptr, 0);
        camRoomB(); ok &= renderFrames(180, ddgiShotDir + "/ddgi_emissive_only.png");

        x3::logInfo("--screenshot-ddgi: GPU frame avg " + std::to_string(gpuOff) +
                    " ms (off) vs " + std::to_string(gpuOn) + " ms (on) -> DDGI cost ~" +
                    std::to_string(gpuOn - gpuOff) + " ms (rays+update, 20x6x20 probes, 128 rays)");

        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return ok ? 0 : 1;
    }

    // ---- REFLECTION VERIFY rig (--screenshot-reflverify [outDir]) -----------
    // A purpose-built A/B bench for the two rt-refl shader changes. Every other
    // reflective scene in this repo (--screenshot-car, showroom, drive) needs
    // GLB assets; this rig is 100% procedural so it runs on a bare checkout.
    //
    // THE RIG: the camera is pitched DOWN at a metal floor. The floor is a ramp
    // of 8 strips whose ROUGHNESS climbs 0.02 -> 0.85 left-to-right (metallic 1,
    // so the specular lobe is the whole look). Three wide bars hang ABOVE and
    // AHEAD, deliberately OUTSIDE the vertical frustum: they are never on screen,
    // so the SSR march always walks off the top edge and the RT ray-query
    // fallback is the ONLY thing that can shade their reflection. The bars are a
    // saturated RED, a saturated GREEN and a bright EMISSIVE CYAN.
    //
    // WHAT EACH CHANGE SHOWS UP AS:
    //   bc8c52a0 (per-object material): with the OLD refl.comp every bar
    //     reflects as the SAME grey; with the new one the reflection must carry
    //     red / green / cyan-glow.
    //   de12fbb5 (glossy blur): left strips stay mirror-sharp; the reflection
    //     must blur progressively to the right instead of vanishing by rough 0.6.
    if (reflVerifyShot) {
        namespace fs = std::filesystem;
        std::error_code mkec; fs::create_directories(reflVerifyShotDir, mkec);
        x3::logInfo("--screenshot-reflverify: writing to " + reflVerifyShotDir);
        x3::logInfo(std::string("--screenshot-reflverify: rayTracingSupported=") +
                    (device->rayTracingSupported() ? "YES" : "NO"));

        auto makeMesh = [&](const x3::prims::PrimMesh& pm) {
            return device->createMesh(pm.verts.data(), (uint32_t)pm.verts.size(),
                                      pm.index.data(), (uint32_t)pm.index.size());
        };
        auto greyPx = x3::prims::makeSolidRGBA(4, 220, 220, 220);
        x3::rhi::TextureHandle greyTex = device->createTexture(greyPx.data(), 4, 4, /*srgb=*/true);

        // ---- The roughness ramp floor: 8 strips, metallic = 1.0. -------------
        // glTF MR packing: G = roughness, B = metallic.
        const int   kStrips = 8;
        const float kRough[kStrips] = { 0.02f, 0.08f, 0.15f, 0.25f, 0.35f, 0.50f, 0.68f, 0.85f };
        x3::rhi::MeshHandle    stripMesh[kStrips];
        x3::rhi::TextureHandle stripMr[kStrips];
        for (int i = 0; i < kStrips; ++i) {
            const float xc = -7.0f + 2.0f * (float)i;      // 1.9 m wide strips, x -7..+7
            stripMesh[i] = makeMesh(x3::prims::makeBox(0.95f, 0.05f, 16.0f, xc, -0.05f, -4.0f, 0.25f));
            const uint8_t mr[4] = { 0, (uint8_t)(kRough[i] * 255.0f + 0.5f), 255, 255 };
            stripMr[i] = device->createTexture(mr, 1, 1, false);
        }
        // A dark surround so the strips read as the only reflector.
        x3::rhi::MeshHandle surroundMesh = makeMesh(x3::prims::makeBox(40.0f, 0.04f, 40.0f, 0.0f, -0.12f, -4.0f, 0.5f));
        const uint8_t surroundMrPx[4] = { 0, 230, 0, 255 };   // rough .9, dielectric
        x3::rhi::TextureHandle surroundMr = device->createTexture(surroundMrPx, 1, 1, false);

        // ---- The OFF-SCREEN bars (the RT-fallback subjects). -----------------
        // y = 7 m, well above the top of a -20 deg pitched 60 deg frustum.
        x3::rhi::MeshHandle barRed   = makeMesh(x3::prims::makeBox(14.0f, 0.45f, 1.1f, 0.0f, 7.0f,  -2.0f, 1.0f));
        x3::rhi::MeshHandle barGreen = makeMesh(x3::prims::makeBox(14.0f, 0.45f, 1.1f, 0.0f, 7.0f, -10.0f, 1.0f));
        x3::rhi::MeshHandle barCyan  = makeMesh(x3::prims::makeBox(14.0f, 0.45f, 1.1f, 0.0f, 7.0f, -18.0f, 1.0f));
        const float kRedAlb[4]   = { 0.90f, 0.04f, 0.03f, 1.0f };
        const float kGreenAlb[4] = { 0.05f, 0.80f, 0.08f, 1.0f };
        const float kCyanAlb[4]  = { 0.10f, 0.85f, 0.95f, 1.0f };
        const float kNoEmis[4]   = { 0, 0, 0, 0 };
        const float kCyanEmis[4] = { 0.10f, 0.85f, 1.00f, 8.0f };   // HDR glow
        const float kWhite[4]    = { 1, 1, 1, 1 };
        const float kSurround[4] = { 0.06f, 0.06f, 0.07f, 1.0f };
        const float identity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };

        // ---- DEPTH-DISCONTINUITY probes. sampleReflGlossy() widens its disc in
        // SCREEN space with no depth/normal rejection, so the real risk of a
        // larger kReflBlurPx is reflected radiance SMEARING across a silhouette.
        // These matte pillars stand ON the reflective ramp and give it hard
        // internal depth edges to smear across; without them this rig is a
        // single flat plane and cannot see that failure mode at all.
        x3::rhi::MeshHandle pillarMesh = makeMesh(x3::prims::makeBox(0.55f, 1.30f, 0.55f, 0.0f, 1.30f, 0.0f, 1.0f));
        const uint8_t pillarMrPx[4] = { 0, 235, 0, 255 };    // rough .92, dielectric (not a reflector)
        x3::rhi::TextureHandle pillarMr = device->createTexture(pillarMrPx, 1, 1, false);
        const float kPillarAlb[4] = { 0.55f, 0.53f, 0.50f, 1.0f };

        // `allMirror` forces EVERY strip to the rough-0.02 material. de12fbb5
        // claims a single tap (bit-identical to the pre-change path) at
        // rough <= 0.05, so an all-mirror frame must diff to ZERO old vs new.
        bool allMirror = false;
        bool withPillars = false;
        auto drawScene = [&](const x3::rhi::FrameContext& f) {
            device->drawMeshPBR(f, surroundMesh, greyTex, x3::rhi::TextureHandle{}, surroundMr,
                                kSurround, kNoEmis, identity);
            for (int i = 0; i < kStrips; ++i)
                device->drawMeshPBR(f, stripMesh[i], greyTex, x3::rhi::TextureHandle{},
                                    allMirror ? stripMr[0] : stripMr[i],
                                    kWhite, kNoEmis, identity);
            if (withPillars) {
                float m[16];
                for (int i = 0; i < 4; ++i) {
                    const float px = -6.0f + 4.0f * (float)i;
                    const float mm[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, px, 0.0f, 1.0f, 1 };
                    for (int k = 0; k < 16; ++k) m[k] = mm[k];
                    device->drawMeshPBR(f, pillarMesh, greyTex, x3::rhi::TextureHandle{}, pillarMr,
                                        kPillarAlb, kNoEmis, m);
                }
            }
            device->drawMesh(f, barRed,   greyTex, kRedAlb,   identity);
            device->drawMesh(f, barGreen, greyTex, kGreenAlb, identity);
            device->drawMeshEmissive(f, barCyan, greyTex, kCyanAlb, kCyanEmis, identity);
        };

        // ---- Lighting: modest ambient so the per-object ALBEDO term in the RT
        // fallback is clearly readable, sun low and to the side. ---------------
        device->setAmbient(0.22f, 0.22f, 0.26f);
        {
            x3::rhi::IRenderDevice::SkyParams sp{};
            sp.enabled = true;
            sp.sunDir[0] = 0.35f; sp.sunDir[1] = 0.55f; sp.sunDir[2] = 0.75f;
            device->setSkyParams(sp);
        }
        device->setShadowBounds(0.0f, 2.0f, -4.0f, 40.0f);
        { x3::rhi::IRenderDevice::SsaoParams s{}; s.enabled = false; device->setSsaoParams(s); }
        { x3::rhi::IRenderDevice::GiParams   g{}; g.enabled = false; device->setGiParams(g); }

        // Camera: high and pitched steeply DOWN so the 8 roughness strips read as
        // 8 broad bands instead of converging to a point, and so the top of the
        // frustum sits near the horizon (every bar at y=7 is safely off-screen).
        auto poseMain = [&]() {
            device->setCamera(0.0f, 5.0f, 9.0f, std::atan2(-1.0f, 0.0f), -0.55f, 60.0f);
        };
        // A tighter pose for judging the blur kernel / tap pattern up close.
        auto poseClose = [&]() {
            device->setCamera(0.0f, 5.0f, 5.0f, std::atan2(-1.0f, 0.0f), -0.72f, 38.0f);
        };

        auto shoot = [&](const std::string& name, int settle) -> bool {
            const std::string path = reflVerifyShotDir + "/" + name + ".png";
            for (int i = 0; i < settle; ++i) {
                glfwPollEvents();
                if (i == settle - 1) device->armCapture(path.c_str());
                auto f = device->beginFrame();
                if (f.valid) drawScene(f);
                device->endFrame(f);
            }
            const bool ok = device->captureFrame(path.c_str());
            x3::logInfo(std::string(ok ? "--screenshot-reflverify: wrote "
                                       : "--screenshot-reflverify: FAILED ") + path);
            return ok;
        };

        auto setRefl = [&](bool on, bool rt, bool fullRes) {
            x3::rhi::IRenderDevice::ReflectionParams rf{};
            rf.ssr = on; rf.rtFallback = rt; rf.fullRes = fullRes; rf.intensity = 1.0f;
            device->setReflectionParams(rf);
        };

        bool ok = true;
        const int kSettle = 90;   // TAA history + SSR + auto-exposure + IBL probe

        // (1) reflections OFF — the baseline every other shot is read against.
        setRefl(false, false, false); poseMain();  ok &= shoot("ramp_ssr_off", kSettle);
        // (2) SSR only, RT fallback OFF — the off-screen bars CANNOT appear.
        setRefl(true, false, false);  poseMain();  ok &= shoot("ramp_ssr_on_rt_off", kSettle);
        // (3) SSR + RT fallback — the money shot for both changes (half-res, the
        //     shipping default) and (4) the same at full-res.
        setRefl(true, true, false);   poseMain();  ok &= shoot("ramp_ssr_on_rt_on_halfres", kSettle);
        setRefl(true, true, true);    poseMain();  ok &= shoot("ramp_ssr_on_rt_on_fullres", kSettle);
        // (5) close pose, RT on, half-res — for judging tap pattern / ringing.
        setRefl(true, true, false);   poseClose(); ok &= shoot("close_ssr_on_rt_on_halfres", kSettle);
        setRefl(false, false, false); poseClose(); ok &= shoot("close_ssr_off", kSettle);
        // (6) ALL-MIRROR frame (every strip rough 0.02): the invariance gate for
        //     de12fbb5 — this frame must be identical old-vs-new mesh.frag.
        allMirror = true;
        setRefl(true, true, false);   poseMain();  ok &= shoot("mirror_all_ssr_on_rt_on", kSettle);
        allMirror = false;
        // (7) DEPTH-EDGE probe: matte pillars standing on the ramp. The blur disc
        //     has no depth rejection, so this is where a too-large kReflBlurPx
        //     shows up as a halo bleeding across each pillar's silhouette.
        withPillars = true;
        setRefl(true, true, false);   poseMain();  ok &= shoot("edges_ssr_on_rt_on", kSettle);
        setRefl(false, false, false); poseMain();  ok &= shoot("edges_ssr_off", kSettle);
        withPillars = false;

        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return ok ? 0 : 1;
    }

    // ---- OFF-SCREEN MATERIAL VERIFY (--screenshot-rtmatverify [outDir]) -----
    //
    // THE QUESTION THIS RIG ANSWERS: when a DDGI or reflection ray hits geometry
    // the CAMERA CANNOT SEE, does it shade with that geometry's OWN material?
    //
    // Why the existing --screenshot-reflverify rig cannot answer it: its bars
    // hang at y=7 with a ~14 m bounding RADIUS, so although they are off SCREEN
    // their bounding SPHERE still crosses the frustum and the conservative
    // sphere test keeps them. Measured: 0 of its 12 TLAS instances carry a bad
    // instanceCustomIndex. It is a null case for this bug, which is exactly why
    // the bug survived that verification pass. Here every coloured object is
    // COMPACT and placed so its whole sphere clears a frustum plane by metres.
    //
    // THE ROW-0 BAIT. Group order follows first-draw order, so the FIRST object
    // drawn owns object-SSBO row 0 — the row a stale instanceCustomIndex of 0
    // resolves to. In both arms that first object is a saturated ORANGE matte
    // surface with ZERO emissive. A broken build therefore paints every
    // off-screen contribution orange-and-unlit; a correct one paints it red /
    // green / cyan and lets the emissive panels actually glow.
    if (rtMatVerifyShot) {
        namespace fs = std::filesystem;
        std::error_code mkec; fs::create_directories(rtMatVerifyShotDir, mkec);
        x3::logInfo("--screenshot-rtmatverify: writing to " + rtMatVerifyShotDir);
        x3::logInfo(std::string("--screenshot-rtmatverify: rayTracingSupported=") +
                    (device->rayTracingSupported() ? "YES" : "NO"));

        auto makeMesh = [&](const x3::prims::PrimMesh& pm) {
            return device->createMesh(pm.verts.data(), (uint32_t)pm.verts.size(),
                                      pm.index.data(), (uint32_t)pm.index.size());
        };
        auto greyPx = x3::prims::makeSolidRGBA(4, 220, 220, 220);
        x3::rhi::TextureHandle greyTex = device->createTexture(greyPx.data(), 4, 4, /*srgb=*/true);
        const uint8_t mirrorMrPx[4] = { 0, 5,   255, 255 };  // rough .02, metal 1
        const uint8_t matteMrPx[4]  = { 0, 235, 0,   255 };  // rough .92, dielectric
        x3::rhi::TextureHandle mirrorMr = device->createTexture(mirrorMrPx, 1, 1, false);
        x3::rhi::TextureHandle matteMr  = device->createTexture(matteMrPx,  1, 1, false);

        const float identity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        const float kOrange[4]  = { 0.95f, 0.42f, 0.04f, 1.0f };   // <- object row 0
        const float kWhite[4]   = { 0.92f, 0.92f, 0.92f, 1.0f };
        const float kRedAlb[4]  = { 0.92f, 0.03f, 0.03f, 1.0f };
        const float kGrnAlb[4]  = { 0.03f, 0.90f, 0.06f, 1.0f };
        const float kCyanAlb[4] = { 0.05f, 0.85f, 0.95f, 1.0f };
        const float kNoEmis[4]  = { 0, 0, 0, 0 };
        const float kRedEmis[4] = { 1.00f, 0.03f, 0.02f, 14.0f };
        const float kGrnEmis[4] = { 0.04f, 1.00f, 0.08f, 14.0f };
        const float kCyanEmis[4]= { 0.06f, 0.85f, 1.00f, 14.0f };

        // ===================== ARM 1 — RT REFLECTION =========================
        // refl.comp fades out any reflection ray heading back TOWARD the camera
        // (`backFade`, refl.comp:172), so a head-on mirror wall is discarded by
        // construction and cannot be used here. A GRAZING mirror FLOOR is the
        // geometry the shader actually supports: pitched 45 deg down, the
        // reflection ray leaves the floor at 45 deg FORWARD-AND-UP, and
        // dot(R,V) = 0 keeps backFade at 1.
        //
        // So the subjects hang HIGH and AHEAD, where that ray goes. At 45 deg
        // pitch with a 60 deg vertical fov the frustum's top plane points 15 deg
        // DOWN, so at the panels' distance (16 m) it has already fallen to
        // y = 1.7 — while the panels sit at y = 10 with a 5.2 m bounding radius,
        // clearing it by over 3 m. They are frustum-culled every single frame,
        // yet the mirror floor must still show them.
        x3::rhi::MeshHandle mirrorFloor = makeMesh(x3::prims::makeBox(14.0f, 0.25f, 18.0f, 0.0f, -0.25f, -4.0f, 0.5f));
        // Row-0 bait: a matte ORANGE slab lying on the floor in the near field.
        x3::rhi::MeshHandle baitSlab    = makeMesh(x3::prims::makeBox(14.0f, 0.03f, 2.0f, 0.0f, 0.03f, 8.5f, 0.5f));
        // The three off-screen emissive panels (half-extents 5 x 1.5 x 0.3 ->
        // bounding radius 5.24 m, centres at y = 10, z = -6).
        x3::rhi::MeshHandle panelR = makeMesh(x3::prims::makeBox(5.0f, 1.5f, 0.3f, -12.0f, 10.0f, -6.0f, 1.0f));
        x3::rhi::MeshHandle panelG = makeMesh(x3::prims::makeBox(5.0f, 1.5f, 0.3f,   0.0f, 10.0f, -6.0f, 1.0f));
        x3::rhi::MeshHandle panelC = makeMesh(x3::prims::makeBox(5.0f, 1.5f, 0.3f,  12.0f, 10.0f, -6.0f, 1.0f));

        auto drawMirrorScene = [&](const x3::rhi::FrameContext& f) {
            // FIRST -> owns SSBO row 0. Every wrong lookup reads THIS orange.
            device->drawMeshPBR(f, baitSlab, greyTex, x3::rhi::TextureHandle{}, matteMr,
                                kOrange, kNoEmis, identity);
            device->drawMeshPBR(f, mirrorFloor, greyTex, x3::rhi::TextureHandle{}, mirrorMr,
                                kWhite, kNoEmis, identity);
            device->drawMeshEmissive(f, panelR, greyTex, kRedAlb,  kRedEmis,  identity);
            device->drawMeshEmissive(f, panelG, greyTex, kGrnAlb,  kGrnEmis,  identity);
            device->drawMeshEmissive(f, panelC, greyTex, kCyanAlb, kCyanEmis, identity);
        };

        // ===================== ARM 2 — DDGI COLOUR BLEED ======================
        // DDGI is the cleaner statement of the same bug: probes trace in EVERY
        // direction, so off-screen geometry contributes unconditionally — no
        // grazing angles, no SSR interplay. A saturated green emissive panel
        // sits BEHIND the camera (bounding radius 4.7 m, centre z = +6, so the
        // whole sphere is behind the near plane) and bleeds onto a white wall
        // the camera is looking at. Emissive is read through the SAME material
        // lookup, so a broken build gets row 0's material — orange albedo with
        // ZERO emissive, i.e. no glow and no green at all.
        x3::rhi::MeshHandle roomFloor = makeMesh(x3::prims::makeBox(12.0f, 0.25f, 14.0f, 0.0f, -0.25f, -2.0f, 0.5f));
        x3::rhi::MeshHandle farWall   = makeMesh(x3::prims::makeBox(8.0f, 4.0f, 0.3f, 0.0f, 4.0f, -10.0f, 1.0f));
        x3::rhi::MeshHandle sideWall  = makeMesh(x3::prims::makeBox(0.3f, 4.0f, 8.0f, -7.0f, 4.0f, -2.0f, 1.0f));
        // Row-0 bait for this arm: an orange post standing in view.
        x3::rhi::MeshHandle baitPost  = makeMesh(x3::prims::makeBox(0.5f, 1.2f, 0.5f, 4.5f, 1.2f, -4.0f, 1.0f));
        // The off-screen emissive source, behind the camera. Half-extents
        // 5 x 3 x 0.3 -> bounding radius 5.83 m, centre z = +8 against a camera
        // at z = -2: a 10 m gap, so the whole sphere clears the near plane by
        // over 4 m and the frustum test drops it every frame.
        x3::rhi::MeshHandle bleedPanel = makeMesh(x3::prims::makeBox(5.0f, 3.0f, 0.3f, 0.0f, 3.2f, 8.0f, 1.0f));

        auto drawBleedScene = [&](const x3::rhi::FrameContext& f) {
            device->drawMeshPBR(f, baitPost, greyTex, x3::rhi::TextureHandle{}, matteMr,
                                kOrange, kNoEmis, identity);   // FIRST -> row 0
            device->drawMeshPBR(f, roomFloor, greyTex, x3::rhi::TextureHandle{}, matteMr,
                                kWhite, kNoEmis, identity);
            device->drawMeshPBR(f, farWall, greyTex, x3::rhi::TextureHandle{}, matteMr,
                                kWhite, kNoEmis, identity);
            device->drawMeshPBR(f, sideWall, greyTex, x3::rhi::TextureHandle{}, matteMr,
                                kWhite, kNoEmis, identity);
            device->drawMeshEmissive(f, bleedPanel, greyTex, kGrnAlb, kGrnEmis, identity);
        };

        bool bleedArm = false;
        auto drawScene = [&](const x3::rhi::FrameContext& f) {
            if (bleedArm) drawBleedScene(f); else drawMirrorScene(f);
        };

        // Dim ambient + a weak sun so the emissive panels, not the sky, decide
        // what colour the off-screen contribution has.
        device->setAmbient(0.13f, 0.13f, 0.16f);
        {
            x3::rhi::IRenderDevice::SkyParams sp{};
            sp.enabled = true;
            sp.sunDir[0] = 0.30f; sp.sunDir[1] = 0.80f; sp.sunDir[2] = 0.52f;
            device->setSkyParams(sp);
        }
        device->setShadowBounds(0.0f, 2.0f, -4.0f, 40.0f);
        { x3::rhi::IRenderDevice::SsaoParams s{}; s.enabled = false; device->setSsaoParams(s); }
        { x3::rhi::IRenderDevice::GiParams   g{}; g.enabled = false; device->setGiParams(g); }

        // 45 deg down the -Z axis (yaw atan2(-1,0) is the engine's -Z forward).
        auto poseMirror = [&]() {
            device->setCamera(0.0f, 6.0f, 10.0f, std::atan2(-1.0f, 0.0f), -0.785f, 60.0f);
        };
        auto poseBleed = [&]() {
            device->setCamera(0.0f, 2.4f, -2.0f, std::atan2(-1.0f, 0.0f), -0.20f, 62.0f);
        };
        // ARM 2 lighting: the bounce has to be the brightest thing in the room,
        // or auto-exposure buries it under sun + ambient.
        auto lightBleedArm = [&]() {
            device->setAmbient(0.03f, 0.03f, 0.04f);
            x3::rhi::IRenderDevice::SkyParams sp{};
            sp.enabled = true;
            sp.sunDir[0] = 0.30f; sp.sunDir[1] = 0.80f; sp.sunDir[2] = 0.52f;
            sp.sunIntensity = 0.15f;
            sp.sunLight     = 0.10f;
            device->setSkyParams(sp);
        };

        auto shoot = [&](const std::string& name, int settle) -> bool {
            const std::string path = rtMatVerifyShotDir + "/" + name + ".png";
            for (int i = 0; i < settle; ++i) {
                glfwPollEvents();
                if (i == settle - 1) device->armCapture(path.c_str());
                auto f = device->beginFrame();
                if (f.valid) drawScene(f);
                device->endFrame(f);
            }
            const bool ok = device->captureFrame(path.c_str());
            x3::logInfo(std::string(ok ? "--screenshot-rtmatverify: wrote "
                                       : "--screenshot-rtmatverify: FAILED ") + path);
            return ok;
        };
        auto setRefl = [&](bool on, bool rt) {
            x3::rhi::IRenderDevice::ReflectionParams rf{};
            rf.ssr = on; rf.rtFallback = rt; rf.fullRes = true; rf.intensity = 1.0f;
            device->setReflectionParams(rf);
        };
        auto setDdgi = [&](bool on) {
            x3::rhi::IRenderDevice::DdgiParams dg{};
            dg.enabled = on;
            if (on) {
                dg.countX = 16; dg.countY = 8; dg.countZ = 20;
                dg.originX = -10.0f; dg.originY = -0.5f; dg.originZ = -12.0f;
                dg.sizeX   =  20.0f; dg.sizeY   =  8.0f; dg.sizeZ   =  24.0f;
                dg.raysPerProbe = 128;
                dg.hysteresis   = 0.90f;   // converge faster than the 0.97 default
                dg.intensity    = 2.0f;    // read the bounce clearly
            }
            device->setDdgiParams(dg);
        };

        bool ok = true;
        const int kSettle = 90;

        // ---- ARM 1 shots ----------------------------------------------------
        bleedArm = false; setDdgi(false);
        // (1) CONTROL: reflections off entirely.
        setRefl(false, false); poseMirror(); ok &= shoot("mirror_refl_off", kSettle);
        // (2) CONTROL: SSR on, RT fallback OFF. The panels are off-screen, so
        //     the screen-space march has nothing to find and MUST show nothing.
        //     This proves shot (3)'s colour can only have come from the RT path.
        setRefl(true, false);  poseMirror(); ok &= shoot("mirror_ssr_only", kSettle);
        // (3) THE GATE SHOT: SSR + RT fallback. Correct = three distinct glowing
        //     bands, red / green / cyan. Broken = ORANGE, unlit (row 0).
        setRefl(true, true);   poseMirror(); ok &= shoot("mirror_rt_on", kSettle);

        // ---- ARM 2 shots ----------------------------------------------------
        bleedArm = true; setRefl(false, false); lightBleedArm();
        setDdgi(false); poseBleed(); ok &= shoot("bleed_ddgi_off", kSettle);
        // Probes converge over ~1-2 s of hysteresis -> settle long.
        setDdgi(true);  poseBleed(); ok &= shoot("bleed_ddgi_on", 420);

        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return ok ? 0 : 1;
    }

    // ---- RT soft-shadow gate shots (--screenshot-rtshadows [outDir]) --------
    // THE GATE SHOT for r_rtshadows: a detention-cell rig lit by ONE ceiling
    // lamp (sun parked below the horizon). A bunk sits low near a wall (tight
    // contact shadow) and a tall pillar stands mid-room (its lamp shadow's
    // penumbra must WIDEN with distance from its base). Tier 0 = today's path:
    // the lamp casts NOTHING (attenuation only); tier 2 = the lamp finally
    // casts. Also: an outdoor plate A/Bs the sun CSM (tier 0) vs RT (tier 1) —
    // contact hardening at the pole base, soft tip — and a 3-frame motion
    // burst checks the 1-spp penumbra noise stays TAA-stable. Headless; logs
    // the GPU-ms delta; exits after the captures.
    if (rtshShot) {
        namespace fs = std::filesystem;
        std::error_code mkec; fs::create_directories(rtshShotDir, mkec);
        x3::logInfo("--screenshot-rtshadows: rendering RT shadow gate shots to " + rtshShotDir);

        // ---- CELL rig: 8x4x8 m room, floor top at y=0 (the ddgi-rig pattern;
        // walls overlap the slabs so no coplanar seams). ----
        std::vector<x3::prims::PrimMesh> parts;
        parts.push_back(x3::prims::makeBox(4.5f, 0.5f, 4.5f,  0.0f, -0.5f, 0.0f)); // floor slab
        parts.push_back(x3::prims::makeBox(4.5f, 0.5f, 4.5f,  0.0f,  4.5f, 0.0f)); // ceiling slab
        parts.push_back(x3::prims::makeBox(0.5f, 2.15f, 4.5f, -4.0f,  2.0f, 0.0f)); // west wall
        parts.push_back(x3::prims::makeBox(0.5f, 2.15f, 4.5f,  4.0f,  2.0f, 0.0f)); // east wall
        parts.push_back(x3::prims::makeBox(4.5f, 2.15f, 0.5f,  0.0f,  2.0f, -4.0f)); // south wall
        parts.push_back(x3::prims::makeBox(4.5f, 2.15f, 0.5f,  0.0f,  2.0f,  4.0f)); // north wall
        // Occluders: a low bunk near the north wall + a tall pillar mid-room.
        parts.push_back(x3::prims::makeBox(0.9f, 0.25f, 0.5f, -2.0f, 0.45f, 2.6f)); // bunk (low -> tight shadow)
        parts.push_back(x3::prims::makeBox(0.22f, 1.5f, 0.22f, 1.2f, 1.5f, 0.6f));  // pillar (tall -> widening penumbra)
        std::vector<x3::rhi::MeshHandle> partMesh;
        for (auto& p : parts)
            partMesh.push_back(device->createMesh(p.verts.data(), (uint32_t)p.verts.size(),
                                                  p.index.data(), (uint32_t)p.index.size()));
        // Emissive lamp fixture just ABOVE the light position (outside every
        // shadow segment — rays stop a clearance short of the source).
        x3::prims::PrimMesh fixture = x3::prims::makeBox(0.35f, 0.06f, 0.35f, 0.0f, 3.85f, 0.0f);
        x3::rhi::MeshHandle fixtureMesh = device->createMesh(fixture.verts.data(), (uint32_t)fixture.verts.size(),
                                                             fixture.index.data(), (uint32_t)fixture.index.size());
        // ---- SUN plate: open ground + a cube + a tall thin pole. ----
        x3::prims::PrimMesh ground = x3::prims::makeBox(10.0f, 0.5f, 10.0f, 0.0f, -0.5f, 0.0f);
        x3::prims::PrimMesh cube   = x3::prims::makeBox(0.5f, 0.5f, 0.5f, -1.5f, 0.5f, 0.5f);
        x3::prims::PrimMesh pole   = x3::prims::makeBox(0.08f, 2.0f, 0.08f, 1.5f, 2.0f, -0.5f);
        x3::rhi::MeshHandle groundMesh = device->createMesh(ground.verts.data(), (uint32_t)ground.verts.size(),
                                                            ground.index.data(), (uint32_t)ground.index.size());
        x3::rhi::MeshHandle cubeMesh   = device->createMesh(cube.verts.data(), (uint32_t)cube.verts.size(),
                                                            cube.index.data(), (uint32_t)cube.index.size());
        x3::rhi::MeshHandle poleMesh   = device->createMesh(pole.verts.data(), (uint32_t)pole.verts.size(),
                                                            pole.index.data(), (uint32_t)pole.index.size());

        auto greyPx = x3::prims::makeSolidRGBA(4, 195, 195, 195);
        x3::rhi::TextureHandle greyTex = device->createTexture(greyPx.data(), 4, 4, /*srgb=*/true);

        const float identity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        const float white[4] = { 1, 1, 1, 1 };
        const float fixtureTint[4]     = { 1.0f, 0.95f, 0.85f, 1.0f };
        const float fixtureEmissive[4] = { 1.0f, 0.9f, 0.7f, 8.0f };

        auto drawCell = [&](const x3::rhi::FrameContext& f) {
            for (auto m : partMesh) device->drawMesh(f, m, greyTex, white, identity);
            device->drawMeshEmissive(f, fixtureMesh, greyTex, fixtureTint, fixtureEmissive, identity);
        };
        auto drawSunPlate = [&](const x3::rhi::FrameContext& f) {
            device->drawMesh(f, groundMesh, greyTex, white, identity);
            device->drawMesh(f, cubeMesh,   greyTex, white, identity);
            device->drawMesh(f, poleMesh,   greyTex, white, identity);
        };

        float gpuMsAccum = 0.0f; int gpuMsN = 0;
        auto renderFrames = [&](int n, const std::string& capturePath, auto&& draw) -> bool {
            for (int i = 0; i < n; ++i) {
                glfwPollEvents();
                if (!capturePath.empty() && i == n - 1) device->armCapture(capturePath.c_str());
                auto f = device->beginFrame();
                if (f.valid) draw(f);
                device->endFrame(f);
                gpuMsAccum += device->stats().gpuFrameMs; ++gpuMsN;
            }
            if (capturePath.empty()) return true;
            const bool ok = device->captureFrame(capturePath.c_str());
            x3::logInfo(std::string(ok ? "--screenshot-rtshadows: wrote " : "--screenshot-rtshadows: FAILED ") + capturePath);
            return ok;
        };
        auto setTier = [&](int tier) {
            x3::rhi::IRenderDevice::RtShadowParams rp{};
            rp.tier = tier;
            device->setRtShadowParams(rp);
        };

        bool ok = true;

        // ===== 1) LAMP gate shot (cell rig, lamp-only) =====
        x3::rhi::PointLight pl{};
        pl.pos[0] = 0.0f; pl.pos[1] = 3.6f; pl.pos[2] = 0.0f; pl.range = 14.0f;
        pl.color[0] = 3.2f; pl.color[1] = 2.9f; pl.color[2] = 2.4f;
        device->setPointLights(&pl, 1);
        device->setAmbient(0.015f, 0.016f, 0.020f);
        device->setShadowBounds(0.0f, 2.0f, 0.0f, 20.0f);
        {   // sun below the horizon: the lamp is the only direct light.
            x3::rhi::IRenderDevice::SkyParams sp{};
            sp.enabled = false;
            sp.sunDir[0] = 0.0f; sp.sunDir[1] = -1.0f; sp.sunDir[2] = 0.01f;
            device->setSkyParams(sp);
        }
        // Camera: SW corner, looking across the pillar toward the NE walls.
        device->setCamera(-3.1f, 1.7f, -3.1f, std::atan2(3.7f, 4.3f), -0.10f, 72.0f);

        setTier(0);
        ok &= renderFrames(30, rtshShotDir + "/lamp_rtshadows_off.png", drawCell);
        gpuMsAccum = 0.0f; gpuMsN = 0;
        ok &= renderFrames(40, "", drawCell);
        const float gpuLampOff = gpuMsAccum / std::max(1, gpuMsN);

        setTier(2);
        ok &= renderFrames(90, rtshShotDir + "/lamp_rtshadows_on.png", drawCell);   // TAA settles the penumbra
        gpuMsAccum = 0.0f; gpuMsN = 0;
        ok &= renderFrames(40, "", drawCell);
        const float gpuLampOn = gpuMsAccum / std::max(1, gpuMsN);

        // Motion burst (tier 2): 3 consecutive frames while the camera slides —
        // the 1-spp penumbra must stay TAA-stable (no sizzle/ghost trails).
        for (int mf = 0; mf < 3; ++mf) {
            device->setCamera(-3.1f + 0.06f * (float)(mf + 1), 1.7f, -3.1f,
                              std::atan2(3.7f, 4.3f), -0.10f, 72.0f);
            ok &= renderFrames(1, rtshShotDir + "/lamp_motion_f" + std::to_string(mf) + ".png", drawCell);
        }

        // ===== 2) SUN A/B (outdoor plate): CSM (tier 0) vs RT (tier 1) =====
        // LOW sun (elev ~23 deg) -> the 4 m pole throws a ~9 m shadow, so the
        // RT penumbra growth (sharp at the base, ~8 cm soft at the tip with the
        // default 0.5 deg sun) reads against CSM's constant 3x3 PCF blur.
        device->setPointLights(nullptr, 0);
        device->setAmbient(0.10f, 0.11f, 0.13f);
        device->setShadowBounds(0.0f, 0.0f, 0.0f, 30.0f);
        {
            x3::rhi::IRenderDevice::SkyParams sp{};
            sp.enabled = true;
            sp.sunDir[0] = 0.70f; sp.sunDir[1] = 0.30f; sp.sunDir[2] = 0.15f;
            device->setSkyParams(sp);
        }
        // Camera low over the pole's shadow line, looking down its length.
        device->setCamera(0.5f, 1.9f, 3.4f, std::atan2(-4.6f, -4.5f), -0.30f, 72.0f);

        setTier(0);
        ok &= renderFrames(40, rtshShotDir + "/sun_csm.png", drawSunPlate);
        setTier(1);
        gpuMsAccum = 0.0f; gpuMsN = 0;
        ok &= renderFrames(90, rtshShotDir + "/sun_rt.png", drawSunPlate);
        const float gpuSunOn = gpuMsAccum / std::max(1, gpuMsN);

        // ===== 3) COST plate (no capture): a 24x24 box field + 8 overlapping
        // lamps + sun — every pixel pays the sun ray AND saturates the K=4
        // point-ray budget. 60-frame GPU-ms averages per tier (the single-frame
        // smoketest stat is useless under GPU contention). =====
        std::vector<x3::rhi::MeshHandle> fieldMesh;
        {
            x3::prims::PrimMesh fb = x3::prims::makeBox(0.45f, 0.9f, 0.45f, 0.0f, 0.9f, 0.0f);
            fieldMesh.push_back(device->createMesh(fb.verts.data(), (uint32_t)fb.verts.size(),
                                                   fb.index.data(), (uint32_t)fb.index.size()));
        }
        std::vector<float> fieldXf;                 // 16 floats per instance (row-major flat)
        for (int gz = 0; gz < 24; ++gz)
            for (int gx = 0; gx < 24; ++gx) {
                const float m[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0,
                                      -9.2f + 0.8f * (float)gx, 0.0f, -9.2f + 0.8f * (float)gz, 1 };
                fieldXf.insert(fieldXf.end(), m, m + 16);
            }
        x3::rhi::PointLight lamps[8]{};
        for (int li = 0; li < 8; ++li) {
            lamps[li].pos[0] = -6.0f + 4.0f * (float)(li % 4);
            lamps[li].pos[1] = 3.0f;
            lamps[li].pos[2] = (li < 4) ? -3.0f : 3.0f;
            lamps[li].range = 16.0f;             // every lamp covers most pixels
            lamps[li].color[0] = 2.2f; lamps[li].color[1] = 2.0f; lamps[li].color[2] = 1.7f;
        }
        device->setPointLights(lamps, 8);
        auto drawField = [&](const x3::rhi::FrameContext& f) {
            device->drawMesh(f, groundMesh, greyTex, white, identity);
            for (size_t mi = 0; mi + 16 <= fieldXf.size(); mi += 16)
                device->drawMesh(f, fieldMesh[0], greyTex, white, &fieldXf[mi]);
        };
        device->setCamera(0.0f, 6.5f, 13.0f, std::atan2(-13.0f, 0.0f), -0.42f, 72.0f);
        auto costAvg = [&](int tier) -> float {
            setTier(tier);
            renderFrames(20, "", drawField);     // settle (pipeline swap, TLAS)
            gpuMsAccum = 0.0f; gpuMsN = 0;
            renderFrames(60, "", drawField);
            return gpuMsAccum / std::max(1, gpuMsN);
        };
        const float costT0 = costAvg(0);
        const float costT1 = costAvg(1);
        const float costT2 = costAvg(2);

        x3::logInfo("--screenshot-rtshadows: GPU frame avg lamp " + std::to_string(gpuLampOff) +
                    " ms (tier 0) vs " + std::to_string(gpuLampOn) + " ms (tier 2) -> point-shadow cost ~" +
                    std::to_string(gpuLampOn - gpuLampOff) + " ms; sun-RT plate avg " +
                    std::to_string(gpuSunOn) + " ms (full-res, 1 spp)");
        x3::logInfo("--screenshot-rtshadows: COST plate (576 boxes, 8 lamps, K=4 saturated, 60-frame avg): tier0 " +
                    std::to_string(costT0) + " ms, tier1 " + std::to_string(costT1) + " ms (+" +
                    std::to_string(costT1 - costT0) + " sun), tier2 " + std::to_string(costT2) + " ms (+" +
                    std::to_string(costT2 - costT1) + " points; full-res inline)");

        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return ok ? 0 : 1;
    }

    // ---- Showroom preview (--screenshot-showroom [path.png]) ---------------
    // Load the baked Unity scene export (the "3D Showroom Level Kit" Example_01),
    // frame the camera on the BUILDING cluster (the surrounding km of decorative
    // scatter sits off-frame), and capture a PBR-shaded PNG. Headless, like the rest.
    if (showroomShot) {
        x3::logInfo("--screenshot-showroom: rendering the Unity showroom export to " + showroomShotPath);
        x3::game::EnvArtSystem showroom;
        const bool ok = showroom.buildFromGlb(*device, x3::game::convertedGlbRoot(),
                                              "ShowRoom_Vol30/Example_01.glb");
        if (!ok) x3::logError("--screenshot-showroom: scene GLB failed to load");


        // DAY<->NIGHT state (default NIGHT; X3_SHOWROOM_DAY=1 -> DAY). The helper sets
        // sky/sun/ambient/bloom for the chosen state (no interior point lights on the
        // exterior shot). DAY = Unity-match bright cool sky; NIGHT = the original recipe.
        const bool gShowroomDay = showroomDayDefault();
        // EXTERIOR hero shot: SKY reflection probe (interiorProbe = false) — the tower's
        // metal panels reflect the sky, which is where the reference's sheen comes from.
        // NIGHT keeps its original interior probe (a black sky has nothing to give a
        // metal); DAY takes the SKY probe.
        applyShowroomTimeOfDay(device, gShowroomDay, /*interiorLights*/nullptr,
                               /*interiorProbe*/!gShowroomDay);
        // Alpha-cutout SHADOWS: the firs are MASK billboards; the depth-only shadow pass
        // was casting their full QUADS as black rectangles all over the snow.
        device->setShadowCutout(true);
        // ...and the RT soft-shadow path (tier 2 by default on ray-query hardware) is
        // OPAQUE-ONLY by design — "alpha-cutout occludes as the full quad" (see
        // IRenderDevice::RtShadowParams). On a 5090 it is what actually shades the
        // scene, so it re-draws the same black crosses no matter what the raster
        // shadow map does. The showroom's forest is ALL cutout billboards, so this
        // world takes the CSM path (tier 0), where the cutout pipeline above applies.
        { x3::rhi::IRenderDevice::RtShadowParams rt{}; rt.tier = 0;
          if (const char* rte = std::getenv("X3_SHOWROOM_RT")) rt.tier = std::atoi(rte);
          device->setRtShadowParams(rt); }
        x3::logInfo(std::string("--screenshot-showroom: time-of-day = ") + (gShowroomDay ? "DAY" : "NIGHT"));
        // Disable the SSAO/GI depth PRE-PASS for the showroom: it makes the color pass use an
        // EQUAL depth test vs full-quad pre-pass depth, which would punch sky holes through
        // alpha-cutout foliage (the pre-pass has no fragment shader to discard). Without it the
        // color pass uses LESS + depth-write, so cutout sprites composite correctly.
        { x3::rhi::IRenderDevice::SsaoParams s{}; s.enabled = false; device->setSsaoParams(s); }
        { x3::rhi::IRenderDevice::GiParams   g{}; g.enabled = false; device->setGiParams(g); }

        // --- NIGHT-SKY planets: load the 6 FORGE3D bodies via the SHARED helper (same
        // files / slot order / srgb as --screenshot-nightsky). CELESTIAL placement:
        // each body is a fixed sky DIRECTION (az/el) + angular diameter, anchored on
        // the camera eye at draw time — no per-scene repositioning needed.
        int nPlanetTexFail = 0;
        x3::rhi::MeshHandle planetMesh{};
        x3::rhi::MeshHandle ringMesh{};
        std::vector<NightSkyPlanet> planets =
            loadNightSkyPlanets(device, planetMesh, nPlanetTexFail, "--screenshot-showroom", &ringMesh);
        if (nPlanetTexFail > 0)
            x3::logError("--screenshot-showroom: " + std::to_string(nPlanetTexFail) +
                         " planet texture(s) missing — some bodies may render flat");
        // Non-zero sky-animation time so the starfield (+ any future time-driven sky)
        // is captured in a settled, animated state. Corona uTime flows via drawPlanet.
        // NIGHT only — the starfield/wheeling is a night feature (auto-hidden on the
        // bright DAY sky, and the planets are not drawn in DAY).
        if (!gShowroomDay) device->setSkyTime(10.0f);

        // Frame on the BUILDING using ENGINE-space bounds (the engine's node-transform
        // composition differs from the Python analysis, so trust the engine). namedBounds
        // filters to structural/furniture meshes, ignoring the km of decorative scatter.
        const std::vector<std::string> kBuild = {
            "room", "pilar", "plateform", "platform", "stair", "window", "showcase",
            "table", "chair", "carpet", "tube", "halogen", "cache", "tv_screen" };
        float bmn[3], bmx[3];
        const uint32_t nb = showroom.namedBounds(kBuild, bmn, bmx);
        if (nb == 0) { showroom.worldBounds(bmn, bmx); x3::logWarn("--screenshot-showroom: 0 named building nodes; framing the full scene"); }
        x3::logInfo("--screenshot-showroom: building bounds (" + std::to_string(nb) + " nodes) min(" +
            std::to_string(bmn[0]) + "," + std::to_string(bmn[1]) + "," + std::to_string(bmn[2]) +
            ") max(" + std::to_string(bmx[0]) + "," + std::to_string(bmx[1]) + "," + std::to_string(bmx[2]) + ")");

        // Center + extent -> stand back along +Z, kept within the 200 m far plane.
        const float cx = (bmn[0] + bmx[0]) * 0.5f, cy = (bmn[1] + bmx[1]) * 0.5f, cz = (bmn[2] + bmx[2]) * 0.5f;

        // FOREST DENSITY: the pack ships ~166 conifer billboards; the Unity reference
        // reads as a DENSE snow forest. Clone them around themselves — each clone
        // inherits a source tree's real on-ground position, then takes a fresh yaw, a
        // 0.7-1.35x scale and a 3-14 m offset. Clusters, not a lattice. The keep-out
        // disc holds them off the hero building's apron.
        {
            uint32_t treeAdd = 260;
            if (const char* te = std::getenv("X3_SHOWROOM_TREES")) treeAdd = (uint32_t)std::strtoul(te, nullptr, 10);
            const float keepOut[3] = { cx, cz, 46.0f };
            if (treeAdd)
                showroom.densifyFoliage({ "sapin" }, treeAdd, /*seed*/20260712u,
                                        /*minR*/3.0f, /*maxR*/14.0f,
                                        /*scaleMin*/0.70f, /*scaleMax*/1.35f, /*sink*/0.5f, keepOut);
        }
        const float ex = bmx[0] - bmn[0], ey = bmx[1] - bmn[1];
        float span = ex > ey ? ex : ey;
        float dist = span * 0.75f + 12.0f;
        if (dist > 175.0f) dist = 175.0f;
        if (dist < 18.0f)  dist = 18.0f;
        float camx = cx, camy = cy + ey * 0.12f, camz = bmx[2] + dist;         // in front (+Z face)
        float dx = cx - camx, dy = cy - camy, dz = cz - camz;                  // look toward center
        float len = std::sqrt(dx * dx + dy * dy + dz * dz); if (len < 1e-3f) len = 1e-3f;
        const float pitch = std::asin(dy / len);
        const float yaw   = std::atan2(dz, dx);
        // X3_SHOWROOM_CAMOFF="dx,dy,dz" TRANSLATES the eye (yaw/pitch unchanged) —
        // the no-parallax proof for the celestial planets: the foreground shifts,
        // the sky composition must NOT (the bodies re-anchor on the moved eye).
        if (const char* off = std::getenv("X3_SHOWROOM_CAMOFF")) {
            float ox = 0, oy = 0, oz = 0;
            if (std::sscanf(off, "%f,%f,%f", &ox, &oy, &oz) == 3) {
                camx += ox; camy += oy; camz += oz;
                x3::logInfo("--screenshot-showroom: CAMOFF applied (" + std::to_string(ox) + "," +
                            std::to_string(oy) + "," + std::to_string(oz) + ")");
            }
        }
        x3::logInfo("--screenshot-showroom: cam(" + std::to_string(camx) + "," + std::to_string(camy) + "," +
            std::to_string(camz) + ") yaw=" + std::to_string(yaw) + " pitch=" + std::to_string(pitch));
        device->setCamera(camx, camy, camz, yaw, pitch, 72.0f);
        // Frame the sun's shadow box on the building (+ surrounding firs) so they cast shadows
        // (the default ~45 m camera-following box sits 100+ m short of the building).
        float showroomShadowBox = 150.0f;
        if (const char* sb = std::getenv("X3_SHOWROOM_SHADOWBOX")) showroomShadowBox = (float)std::atof(sb);
        device->setShadowBounds(cx, cy, cz, showroomShadowBox);
        // MOUNTAINS: the Unity pack's OWN terrain mesh spans ~6.6 km and reaches
        // +453 m — real snowy peaks. They were never missing: the engine's DEFAULT
        // 200 m far plane was clipping every one of them. Push the far plane out to
        // the GLB's world AABB (X3_SHOWROOM_FAR overrides for tuning).
        float showroomFar = 8000.0f;
        if (const char* fe = std::getenv("X3_SHOWROOM_FAR")) showroomFar = (float)std::atof(fe);
        device->setCameraFar(showroomFar);

        // (Planet placement is CELESTIAL — fixed world-space sky directions anchored
        // on the camera eye inside drawNightSkyPlanets — so there is NO per-scene
        // repositioning here anymore. The bodies hang out in the night sky at their
        // az/el table defaults, occluded by the spire/terrain via the depth test.)

        // Draw the WHOLE scene (all 1150 drawables). The earlier "~480 draws blanks the frame"
        // ceiling was a SYMPTOM of the depth.vert/shadow.vert SSBO-stride bug (garbage depths at
        // high instance indices compounded with draw count) — fixed, so the full scene composites.
        uint32_t showroomMaxDraw = 0xFFFFFFFFu;
        if (const char* e = std::getenv("X3_SHOWROOM_MAXDRAW")) showroomMaxDraw = (uint32_t)std::strtoul(e, nullptr, 10);
        x3::logInfo("--screenshot-showroom: maxDraw=" + std::to_string(showroomMaxDraw));

        // --ddgi: give the probe field time to converge (hysteresis warm-up ramp
        // + a few multibounce generations) before the still is captured.
        const int kSettle = ddgiForce ? 120 : 16;
        for (int i = 0; i < kSettle; ++i) {
            glfwPollEvents();
            if (i == kSettle - 1) device->armCapture(showroomShotPath.c_str());
            auto frame = device->beginFrame();
            if (frame.valid) {
                const uint32_t nDrawn = showroom.draw(*device, frame, showroomMaxDraw, nullptr, nullptr);
                if (i == 0) x3::logInfo("--screenshot-showroom: drew " + std::to_string(nDrawn) + " drawables (of 1150)");
                // Hang the night-sky planets over the spire (AFTER the env so depth occludes correctly).
                // Ring mesh enables the gas giant's alpha ring; the device composites the
                // transparent glow shells (atmosphere/corona/ring) AFTER the opaque bodies.
                // NIGHT only — DAY has no planets (the bright sky carries the exterior).
                if (!gShowroomDay)
                    drawNightSkyPlanets(device, frame, planetMesh, planets, 10.0f,
                                        camx, camy, camz, ringMesh);
            }
            device->endFrame(frame);
        }
        {   // Perf of the hero frame (NOTE: this path renders at 4x SSAA = 5120x2880).
            const x3::rhi::RenderStats st = device->stats();
            x3::logInfo("--screenshot-showroom: PERF gpuFrameMs=" + std::to_string(st.gpuFrameMs) +
                        " (" + std::to_string(st.gpuFrameMs > 0.0f ? 1000.0f / st.gpuFrameMs : 0.0f) +
                        " fps @ 4x SSAA 5120x2880) drawCalls=" + std::to_string(st.drawCalls));
        }
        const bool wrote = device->captureFrame(showroomShotPath.c_str());
        if (wrote) x3::logInfo("--screenshot-showroom: wrote " + showroomShotPath);
        else       x3::logError("--screenshot-showroom: capture FAILED");

        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return wrote ? 0 : 1;
    }

    // ---- Upper-floor CONTENT proof shots (--screenshot-upperfloors [outDir]) ----
    // R-5 (re-homed from playable-build eab7ff4, adapted to HFF's loadCanonTower).
    // Build the WHOLE canon tower + CanonPlay's full content (floor-1 spine, W4
    // wards/bosses, the R-5 upper-floor squads + pickups), then pose the camera at
    // a few POPULATED upper rooms and capture: an enemy/item room, a captive ward,
    // a drone bay, and the F7 executive corridor (PB's 4th shot was its spire apex
    // encounter — dropped with PB's F4.5 content; HFF's Nexus is W5-1's design).
    // Proves the floors are designed, populated spaces. Headless; exits after.
    if (upperShot) {
        namespace fs = std::filesystem;
        std::error_code mkec; fs::create_directories(upperShotDir, mkec);
        x3::logInfo("--screenshot-upperfloors: building canon tower + content, capturing to " + upperShotDir);

        std::unique_ptr<x3::phys::IPhysicsWorld> uphys(x3::phys::createPhysicsWorld());
        uphys->init();
        x3::game::Scene uscene;
        x3::game::CanonFloor ufloor = x3::game::loadCanonTower(x3::game::canonProjectJsonPath());
        bool ok = true;
        if (!ufloor.valid()) {
            x3::logError("--screenshot-upperfloors: canonical JSON absent — cannot capture");
            ok = false;
        } else {
            x3::game::buildCanonFloor(ufloor, uscene, *device, *uphys);
            x3::game::CanonPlay uplay;
            uplay.build(ufloor, uscene, *device, *uphys,
                        x3::game::riggedGlbRoot(), x3::game::canonGirlsDialogPath());

            // Bright-enough lighting for a content read (per-shot point lights at the room).
            device->setAmbient(0.34f, 0.34f, 0.38f);
            { x3::rhi::IRenderDevice::SkyParams sp{}; sp.enabled = false;
              sp.sunDir[0]=0.2f; sp.sunDir[1]=-1.0f; sp.sunDir[2]=0.1f; device->setSkyParams(sp); }

            // Pose a 3/4 vantage from the room's near-low corner, looking at the content
            // standing near the room center (enemies/items sit ~0.4-1.0 m off the floor).
            auto litShot = [&](const char* roomName, const std::string& outPath) -> bool {
                const uint32_t r = ufloor.roomByName(roomName);
                if (r == x3::game::kNoRoom) { x3::logError(std::string("  room absent: ") + roomName); return false; }
                const x3::game::CanonRoom& R = ufloor.rooms[r];
                // Three point lights across the room for an even, readable interior.
                x3::rhi::PointLight pl{};
                pl.pos[0]=R.cx; pl.pos[1]=R.y0()+std::min(3.0f, R.h*0.6f); pl.pos[2]=R.cz;
                pl.range=std::max(R.w,R.d)+10.0f; pl.color[0]=4.0f; pl.color[1]=3.8f; pl.color[2]=3.4f;
                x3::rhi::PointLight l0=pl, l1=pl, l2=pl;
                l1.pos[0]=R.cx - R.w*0.30f; l2.pos[0]=R.cx + R.w*0.30f;
                x3::rhi::PointLight lights[3] = { l0, l1, l2 };
                device->setPointLights(lights, 3);
                // Eye: INSIDE the room near a back corner (inset from the walls so we never
                // look at an exterior face), ~1.7 m off the floor, aimed diagonally across to
                // the content cluster near the room center (content sits ~0.9 m off the floor).
                const float ex = R.cx - R.w*0.38f;
                const float ey = R.y0() + 1.7f;
                const float ez = R.cz - R.d*0.38f;
                const float aimX = R.cx + R.w*0.12f, aimY = R.y0() + 0.9f, aimZ = R.cz + R.d*0.18f;
                const float dx = aimX-ex, dy = aimY-ey, dz = aimZ-ez;
                const float yaw = std::atan2(dz, dx);
                const float horiz = std::sqrt(dx*dx + dz*dz);
                const float pitch = std::atan2(dy, std::max(0.01f, horiz));
                device->setCamera(ex, ey, ez, yaw, pitch, 70.0f);
                // Feed the whole tower visible (no cull) so content draws in the shot.
                std::vector<uint32_t> vis; vis.reserve(ufloor.rooms.size());
                for (uint32_t i=0;i<(uint32_t)ufloor.rooms.size();++i) vis.push_back(i);
                uscene.setVisibleRooms(vis.data(), (uint32_t)vis.size());
                const int kSettle = 6;
                for (int i=0;i<kSettle;++i) {
                    glfwPollEvents();
                    if (i==kSettle-1) device->armCapture(outPath.c_str());
                    auto f = device->beginFrame();
                    if (f.valid) {
                        uscene.render(*device, f);
                        uplay.draw(*device, f, uscene);
                    }
                    device->endFrame(f);
                }
                const bool wrote = device->captureFrame(outPath.c_str());
                x3::logInfo(std::string(wrote ? "  wrote " : "  FAILED ") + outPath +
                            " (room '" + R.name + "')");
                return wrote;
            };

            // 1) F2 Operating Theater B — enemies + a medkit in a real room.
            ok &= litShot("Operating Theater B", upperShotDir + "/f2_operating_theater.png");
            // 2) F2 Ward C: Aria — a captive in her cell with her attacker.
            ok &= litShot("Ward C: Aria", upperShotDir + "/f2_captive_ward.png");
            // 2b) GROUNDED-QA bounce check: advance the LIVE sim 0.5 s (the girls'
            // idle drive runs in CanonPlay::tick) and re-shoot the same ward from
            // the same camera. Her feet must stay planted at deck level across the
            // two clip times — a sunken/bouncing captive shows as a vertical shift.
            {
                const uint32_t wr = ufloor.roomByName("Ward C: Aria");
                if (wr != x3::game::kNoRoom) {
                    const x3::game::CanonRoom& WR = ufloor.rooms[wr];
                    const x3::phys::Vec3 tickEye{ WR.cx - WR.w*0.38f, WR.y0() + 1.7f,
                                                  WR.cz - WR.d*0.38f };
                    x3::game::AttackFxFn noFx{};
                    for (int i = 0; i < 30; ++i) {
                        uphys->step(1.0f / 60.0f);
                        uplay.tick(1.0f / 60.0f, uscene, *uphys, tickEye, nullptr, noFx);
                    }
                }
                ok &= litShot("Ward C: Aria", upperShotDir + "/f2_captive_ward_t2.png");
            }
            // 3) F5 Drone Bay Beta — heavy combat drones + the weapons locker floor.
            ok &= litShot("Drone Bay Beta", upperShotDir + "/f5_drone_bay.png");
            // 4) F7 Executive Corridor — the exec-guard gauntlet before Sarah.
            ok &= litShot("F7: Executive Corridor", upperShotDir + "/f7_executive_corridor.png");

            // 5) R-9: EXTERIOR tower proof — the whole stacked tower from a 3/4 ground
            // vantage, so the inter-floor skirt bands (PB 03256bd re-home) are eye-
            // checkable: the building must read as one continuous shell, not floating
            // plates. Frames the tower AABB (deep zone excluded) with all rooms visible.
            {
                float bx0=1e9f,bx1=-1e9f,by0=1e9f,by1=-1e9f,bz0=1e9f,bz1=-1e9f;
                for (const x3::game::CanonRoom& r : ufloor.rooms) {
                    if (r.cy < -50.0f) continue;   // deep cave/sub-level: not the tower
                    bx0=std::min(bx0,r.x0()); bx1=std::max(bx1,r.x1());
                    by0=std::min(by0,r.y0()); by1=std::max(by1,r.y1());
                    bz0=std::min(bz0,r.z0()); bz1=std::max(bz1,r.z1());
                }
                const float cx=(bx0+bx1)*0.5f, cy=(by0+by1)*0.5f, cz=(bz0+bz1)*0.5f;
                const float span = std::max({bx1-bx0, by1-by0, bz1-bz0});
                device->setCameraFar(600.0f);   // whole tower in frame from ~1.5 spans out
                device->setAmbient(1.1f,1.1f,1.2f);
                { x3::rhi::IRenderDevice::SkyParams sp{}; sp.enabled = true;
                  sp.sunDir[0]=-0.5f; sp.sunDir[1]=0.75f; sp.sunDir[2]=-0.4f;
                  device->setSkyParams(sp); }
                std::vector<uint32_t> vis; vis.reserve(ufloor.rooms.size());
                for (uint32_t i=0;i<(uint32_t)ufloor.rooms.size();++i) vis.push_back(i);
                uscene.setVisibleRooms(vis.data(), (uint32_t)vis.size());
                auto extShot = [&](float ex, float ey, float ez,
                                   float ax, float ay, float az,
                                   const x3::rhi::PointLight* ls, uint32_t nl,
                                   const std::string& outPath, const char* what) -> bool {
                    const float dx=ax-ex, dy=ay-ey, dz=az-ez;
                    const float yaw = std::atan2(dz,dx);
                    const float pitch = std::atan2(dy, std::sqrt(dx*dx+dz*dz));
                    device->setCamera(ex,ey,ez,yaw,pitch,60.0f);
                    device->setPointLights(ls, nl);
                    const int kSettle = 6;
                    for (int i=0;i<kSettle;++i) {
                        glfwPollEvents();
                        if (i==kSettle-1) device->armCapture(outPath.c_str());
                        auto f = device->beginFrame();
                        if (f.valid) uscene.render(*device, f);
                        device->endFrame(f);
                    }
                    const bool wrote = device->captureFrame(outPath.c_str());
                    x3::logInfo(std::string(wrote ? "  wrote " : "  FAILED ") + outPath +
                                std::string(" (") + what + ")");
                    return wrote;
                };
                // 5a) Whole-tower silhouette from a 3/4 ground vantage: any UNSEALED
                // inter-floor gap reads as a bright sky slit through the stack.
                {
                    x3::rhi::PointLight fl{};
                    fl.range = span*6.0f;
                    fl.color[0]=900.0f; fl.color[1]=870.0f; fl.color[2]=820.0f;
                    x3::rhi::PointLight f0=fl, f1=fl;
                    f0.pos[0]=cx-span*0.9f; f0.pos[1]=by0+(by1-by0)*0.85f; f0.pos[2]=cz-span*0.7f;
                    f1.pos[0]=cx-span*0.9f; f1.pos[1]=by0+(by1-by0)*0.30f; f1.pos[2]=cz-span*0.7f;
                    x3::rhi::PointLight fills[2] = { f0, f1 };
                    ok &= extShot(cx - span*1.15f, by0 + (by1-by0)*0.35f, cz - span*0.95f,
                                  cx, cy, cz, fills, 2,
                                  upperShotDir + "/tower_exterior.png",
                                  "whole-tower exterior — skirt-band silhouette proof");
                }
                // 5b) CLOSE-UP of the lower floor-band seams on the -X facade, lit at
                // the ~10 m distances this forward pipeline provably reads (the interior
                // shots): the skirt bands between the floor plates must be closed.
                {
                    const float ay = by0 + (by1-by0)*0.30f;   // a couple of band seams up
                    x3::rhi::PointLight fl{};
                    fl.range = 200.0f;
                    fl.color[0]=180.0f; fl.color[1]=174.0f; fl.color[2]=164.0f;
                    x3::rhi::PointLight c0=fl, c1=fl;
                    c0.pos[0]=bx0-9.0f;  c0.pos[1]=ay+9.0f;  c0.pos[2]=cz-8.0f;
                    c1.pos[0]=bx0-11.0f; c1.pos[1]=ay-7.0f;  c1.pos[2]=cz+9.0f;
                    x3::rhi::PointLight closes[2] = { c0, c1 };
                    ok &= extShot(bx0 - 26.0f, ay + 3.0f, cz - 18.0f,
                                  bx0, ay, cz, closes, 2,
                                  upperShotDir + "/tower_exterior_close.png",
                                  "close-up — inter-floor skirt seam proof");
                }
            }

            uplay.shutdown();
        }
        uphys->shutdown();
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return ok ? 0 : 1;
    }

    // ======================================================================
    // DOOR-MESH-SWAP visual gate (--screenshot-doors [outDir]).
    //
    // Every other canon capture path builds with CanonBuildOpts::doors == nullptr,
    // so NO capture in this repo has ever actually drawn a door. This one builds
    // the canon tower WITH a DoorSystem, then shoots a real cut doorway on each
    // floor that has one — head-on from the approach side at standing eye height —
    // at CLOSED, MID-SLIDE and OPEN. That is what GATE B (the visual review) needs
    // to judge: per-floor leaf tint + signage colour, the frame seated in the cut,
    // and that the OPEN leaf is genuinely clear of the opening.
    // ======================================================================
    if (doorShot) {
        namespace fs = std::filesystem;
        std::error_code mkec; fs::create_directories(doorShotDir, mkec);
        x3::logInfo("--screenshot-doors: building canon tower + doors, capturing to " + doorShotDir);

        std::unique_ptr<x3::phys::IPhysicsWorld> dphys(x3::phys::createPhysicsWorld());
        dphys->init();
        x3::game::Scene dscene;
        x3::game::DoorSystem ddoors;
        x3::game::CanonFloor dfloor = x3::game::loadCanonTower(x3::game::canonProjectJsonPath());
        bool ok = true;
        if (!dfloor.valid()) {
            x3::logError("--screenshot-doors: canonical JSON absent — cannot capture");
            ok = false;
        } else {
            x3::game::CanonBuildOpts dopts;
            dopts.doors = &ddoors;
            dopts.lockSecuredRooms = true;   // so a LOCKED door's red signage is in the set
            x3::game::buildCanonFloor(dfloor, dscene, *device, *dphys, dopts);

            device->setAmbient(0.30f, 0.30f, 0.34f);
            { x3::rhi::IRenderDevice::SkyParams sp{}; sp.enabled = false; device->setSkyParams(sp); }
            std::vector<uint32_t> vis; vis.reserve(dfloor.rooms.size());
            for (uint32_t i = 0; i < (uint32_t)dfloor.rooms.size(); ++i) vis.push_back(i);
            dscene.setVisibleRooms(vis.data(), (uint32_t)vis.size());

            // Shoot one door head-on. `back` is how far the eye stands off the slab
            // along the wall normal (the approach room), so the leaf fills the frame.
            auto doorShotAt = [&](const x3::game::Door& d, const std::string& outPath,
                                  const char* what) -> bool {
                const float bottomY = d.closedPos.y - d.height * 0.5f;
                // WHICH SIDE TO STAND ON. A fixed side put the camera inside wall meat
                // (or in a 2 m closet staring at a blank face) on half the floors, which
                // is exactly the sort of thing that makes a "visual gate" a lie. Probe
                // both flanking sides, take whichever room is DEEPER along the wall
                // normal, and pull the eye in so it stays inside that room.
                const float ux = (d.axis == 0) ? 1.0f : 0.0f;   // axis 0: wall plane X=const
                const float uz = (d.axis == 0) ? 0.0f : 1.0f;
                const float probeY = bottomY + 1.0f;
                float side = -1.0f, bestDepth = 0.0f;
                for (int sgn = -1; sgn <= 1; sgn += 2) {
                    const float px = d.closedPos.x + ux * 1.2f * (float)sgn;
                    const float pz = d.closedPos.z + uz * 1.2f * (float)sgn;
                    const uint32_t r = dfloor.roomAt(px, probeY, pz);
                    if (r == x3::game::kNoRoom) continue;
                    const x3::game::CanonRoom& R = dfloor.rooms[r];
                    const float depth = (d.axis == 0)
                        ? ((sgn > 0) ? (R.x1() - d.closedPos.x) : (d.closedPos.x - R.x0()))
                        : ((sgn > 0) ? (R.z1() - d.closedPos.z) : (d.closedPos.z - R.z0()));
                    if (depth > bestDepth) { bestDepth = depth; side = (float)sgn; }
                }
                if (bestDepth <= 0.0f) bestDepth = 3.4f;        // no room resolved: fall back
                const float back = std::min(3.4f, std::max(1.6f, bestDepth - 0.6f));
                const float nx = ux * side, nz = uz * side;     // outward toward the eye
                const float ex = d.closedPos.x + nx * back;
                const float ey = bottomY + 1.65f;               // standing eye height
                const float ez = d.closedPos.z + nz * back;
                const float ax = d.closedPos.x, ay = bottomY + 1.20f, az = d.closedPos.z;
                const float dx = ax - ex, dy = ay - ey, dz = az - ez;
                device->setCamera(ex, ey, ez, std::atan2(dz, dx),
                                  std::atan2(dy, std::sqrt(dx*dx + dz*dz)), 70.0f);
                // Two practicals bracketing the doorway so the trim + signage read.
                x3::rhi::PointLight pl{};
                pl.range = 12.0f; pl.color[0]=5.0f; pl.color[1]=4.8f; pl.color[2]=4.4f;
                x3::rhi::PointLight l0=pl, l1=pl;
                l0.pos[0]=ex; l0.pos[1]=bottomY+2.6f; l0.pos[2]=ez;
                l1.pos[0]=d.closedPos.x + nx*1.2f; l1.pos[1]=bottomY+2.6f;
                l1.pos[2]=d.closedPos.z + nz*1.2f;
                x3::rhi::PointLight ls[2] = { l0, l1 };
                device->setPointLights(ls, 2);
                const int kSettle = 6;
                for (int i = 0; i < kSettle; ++i) {
                    glfwPollEvents();
                    if (i == kSettle-1) device->armCapture(outPath.c_str());
                    auto f = device->beginFrame();
                    if (f.valid) { dscene.render(*device, f); ddoors.drawMeshes(*device, f); }
                    device->endFrame(f);
                }
                const bool wrote = device->captureFrame(outPath.c_str());
                x3::logInfo(std::string(wrote ? "  wrote " : "  FAILED ") + outPath +
                            std::string(" (") + what + ")");
                return wrote;
            };

            // Pick the FIRST ordinary (unlocked, non-hatch) door on each floor index —
            // one representative per floor identity.
            uint32_t pick[x3::game::kDoorFloorCount];
            for (uint32_t i = 0; i < x3::game::kDoorFloorCount; ++i) pick[i] = UINT32_MAX;
            uint32_t lockedPick = UINT32_MAX;
            for (uint32_t i = 0; i < ddoors.count(); ++i) {
                const x3::game::Door& d = ddoors.at(i);
                if (d.floorHatch) continue;
                if (d.locked) { if (lockedPick == UINT32_MAX) lockedPick = i; continue; }
                if (d.floorStyle < x3::game::kDoorFloorCount && pick[d.floorStyle] == UINT32_MAX)
                    pick[d.floorStyle] = i;
            }
            for (uint32_t f = 0; f < x3::game::kDoorFloorCount; ++f) {
                if (pick[f] == UINT32_MAX) {
                    x3::logInfo(std::string("  (no door on ") + x3::game::doorStyleFor(f).name + ")");
                    continue;
                }
                const std::string tag = "f" + std::to_string(f);
                ok &= doorShotAt(ddoors.at(pick[f]), doorShotDir + "/" + tag + "_closed.png",
                                 x3::game::doorStyleFor(f).name);
            }
            // MOTION + CLEARANCE on one representative door: closed -> ~40% -> open.
            // The OPEN frame is the passability eyeball (headless D4 proves collision;
            // this proves the LEAF is visually clear of the opening too).
            {
                uint32_t hero = UINT32_MAX;
                for (uint32_t f = 0; f < x3::game::kDoorFloorCount && hero == UINT32_MAX; ++f)
                    if (pick[f] != UINT32_MAX) hero = pick[f];
                if (hero != UINT32_MAX) {
                    x3::game::Door& d = ddoors.at(hero);
                    ddoors.startOpening(d);
                    for (int i = 0; i < 24; ++i) ddoors.update(1.0f/60.0f, dscene, *dphys);
                    ok &= doorShotAt(d, doorShotDir + "/motion_mid.png", "mid-slide (eased)");
                    for (int i = 0; i < 90; ++i) ddoors.update(1.0f/60.0f, dscene, *dphys);
                    ok &= doorShotAt(d, doorShotDir + "/motion_open.png", "OPEN — clearance eyeball");
                }
            }
            // A LOCKED door: its signage must burn RED regardless of floor.
            if (lockedPick != UINT32_MAX)
                ok &= doorShotAt(ddoors.at(lockedPick), doorShotDir + "/locked_red.png",
                                 "LOCKED — red signage over the floor colour");
            ddoors.stopAllMotors();
        }
        dphys->shutdown();
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return ok ? 0 : 1;
    }

    // ======================================================================
    // HERO CAR showcase (--screenshot-car [outDir]) — the NFS beauty pass.
    //   * INTERIOR: the Unity showroom GLB as surroundings; the car posed on a
    //     POLISHED reflector slab (the feat/reflections floor material dial)
    //     under EMISSIVE light panels, so SSR/RT reflections visibly sweep the
    //     clearcoat paint. DAY (4 turntable angles) + NIGHT (2 angles, lights
    //     glowing + bloom).
    //   * EXTERIOR NIGHT: the car alone on a wet-asphalt slab under the FORGE3D
    //     planet night sky (2 angles, headlights on).
    // Headless + 4x SSAA; each still settles ~90 frames so TAA history, SSR,
    // auto-exposure and the IBL probe converge before capture.
    // ======================================================================
    if (carShot) {
        namespace fs = std::filesystem;
        std::error_code mkec; fs::create_directories(carShotDir, mkec);
        x3::logInfo("--screenshot-car: writing turntable set to " + carShotDir);

        // --- The showroom surroundings (identity, baked node transforms). ---
        x3::game::EnvArtSystem showroom;
        const bool roomOk = showroom.buildFromGlb(*device, x3::game::convertedGlbRoot(),
                                                  "ShowRoom_Vol30/Example_01.glb");
        if (!roomOk) x3::logWarn("--screenshot-car: showroom GLB missing — interior shots get the studio slab only");

        // Anchor the car on the showroom's GROUND floor: bounds of the building
        // cluster (same name filter as --screenshot-showroom), car at the center
        // of the X span, on the floor (min Y of the named nodes).
        float bmn[3] = {0,0,0}, bmx[3] = {0,0,0};
        bool haveRoom = false;
        if (roomOk) {
            const std::vector<std::string> kBuild = {
                "room", "pilar", "plateform", "platform", "stair", "window", "showcase",
                "table", "chair", "carpet", "tube", "halogen", "cache", "tv_screen" };
            haveRoom = showroom.namedBounds(kBuild, bmn, bmx) > 0;
        }
        // Anchor in the OPEN west bay (the building-center has the round podium +
        // NPC mannequins; +21 m from the west wall is clean polished hall).
        float carX = haveRoom ? (bmn[0] + 21.0f) : 0.0f;
        float carY = haveRoom ? bmn[1] : 0.0f;
        float carZ = haveRoom ? (bmn[2] + bmx[2]) * 0.5f : 0.0f;
        if (const char* e = std::getenv("X3_CAR_POS")) {   // manual override: "x,y,z"
            float px2, py2, pz2;
            if (std::sscanf(e, "%f,%f,%f", &px2, &py2, &pz2) == 3) { carX = px2; carY = py2; carZ = pz2; }
        }
        x3::logInfo("--screenshot-car: car anchor (" + std::to_string(carX) + "," +
                    std::to_string(carY) + "," + std::to_string(carZ) + ") haveRoom=" +
                    (haveRoom ? "1" : "0"));

        // --- The hero car (CTR.glb: clearcoat paint + emissive lights). The GLB
        // sits on y=0 with +Z = nose, so the instance transform is yaw+translate.
        const float kSlabTop = 0.02f;                  // polished slab top above the floor
        auto carXform = [&](float yawRad, float ox, float oy, float oz, float out[16]) {
            const float c = std::cos(yawRad), s = std::sin(yawRad);
            const float m[16] = { c,0,-s,0,  0,1,0,0,  s,0,c,0,  ox,oy,oz,1 };
            for (int i = 0; i < 16; ++i) out[i] = m[i];
        };
        float carT[16];
        carXform(0.6f, carX, carY + kSlabTop, carZ, carT);
        x3::game::EnvArtSystem car;
        const bool carOk = car.buildFromGlbAt(*device, x3::game::convertedGlbRoot(),
                                              "Vehicles/CTR.glb", carT);
        if (!carOk) {
            x3::logError("--screenshot-car: Vehicles/CTR.glb FAILED to load — aborting");
            device->shutdown();
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return 1;
        }

        // --- Studio dressing: a polished reflector slab (the feat/reflections
        // floor dial: rough 0.08 / metal 0.5 on a white dielectric) + emissive
        // panels flanking + above the car (the reflections that sweep the body).
        x3::rhi::TextureHandle polishedMr{}, asphaltMr{};
        { const uint8_t mr[4] = { 0,  20, 128, 255 }; polishedMr = device->createTexture(mr, 1, 1, false); }
        { const uint8_t mr[4] = { 0,  56,  26, 255 }; asphaltMr  = device->createTexture(mr, 1, 1, false); } // wet asphalt: rough .22, metal .10
        auto makeMesh = [&](const x3::prims::PrimMesh& pm) {
            return device->createMesh(pm.verts.data(), (uint32_t)pm.verts.size(),
                                      pm.index.data(), (uint32_t)pm.index.size());
        };
        x3::rhi::MeshHandle slabMesh  = makeMesh(x3::prims::makeBox(5.5f, 0.01f, 5.5f, 0, 0, 0, 0.25f));
        x3::rhi::MeshHandle padMesh   = makeMesh(x3::prims::makeBox(60.0f, 0.01f, 60.0f, 0, 0, 0, 0.25f));
        x3::rhi::MeshHandle stripMesh = makeMesh(x3::prims::makeBox(3.2f, 0.04f, 0.30f, 0, 0, 0, 1.0f));
        x3::rhi::MeshHandle panelMesh = makeMesh(x3::prims::makeBox(0.05f, 1.1f, 2.6f, 0, 0, 0, 1.0f));
        const float kWhite[4]   = { 0.97f, 0.97f, 0.98f, 1.0f };
        const float kAsphalt[4] = { 0.045f, 0.045f, 0.05f, 1.0f };
        const float kNoEmis[4]  = { 0, 0, 0, 0 };
        const float kStripEmis[4] = { 1.0f, 0.99f, 0.95f, 6.0f };   // cool-white HDR strips
        const float kPanelEmisL[4] = { 0.35f, 0.65f, 1.0f, 2.2f };  // cyan panel (left)
        const float kPanelEmisR[4] = { 1.0f, 0.45f, 0.20f, 2.2f };  // amber panel (right)
        auto at = [&](float x, float y, float z, float out[16]) {
            const float m[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, x,y,z,1 };
            for (int i = 0; i < 16; ++i) out[i] = m[i];
        };

        // Per-still draw: env + car + dressing. `interior` gates the showroom +
        // studio strips; exterior swaps the slab material for wet asphalt.
        auto drawScene = [&](const x3::rhi::FrameContext& fr, bool interior) {
            float m[16];
            if (interior && roomOk) showroom.draw(*device, fr);
            car.draw(*device, fr);
            if (interior) {
                at(carX, carY + 0.01f, carZ, m);
                device->drawMeshPBR(fr, slabMesh, x3::rhi::TextureHandle{}, x3::rhi::TextureHandle{},
                                    polishedMr, kWhite, kNoEmis, m);
                // Ceiling light strips (3, across the car) + side emissive panels.
                for (int i = -1; i <= 1; ++i) {
                    at(carX, carY + 3.4f, carZ + (float)i * 2.2f, m);
                    device->drawMeshEmissive(fr, stripMesh, x3::rhi::TextureHandle{}, kWhite, kStripEmis, m);
                }
                at(carX - 5.6f, carY + 1.15f, carZ, m);
                device->drawMeshEmissive(fr, panelMesh, x3::rhi::TextureHandle{}, kWhite, kPanelEmisL, m);
                at(carX + 5.6f, carY + 1.15f, carZ, m);
                device->drawMeshEmissive(fr, panelMesh, x3::rhi::TextureHandle{}, kWhite, kPanelEmisR, m);
            } else {
                at(carX, carY + 0.01f, carZ, m);
                device->drawMeshPBR(fr, padMesh, x3::rhi::TextureHandle{}, x3::rhi::TextureHandle{},
                                    asphaltMr, kAsphalt, kNoEmis, m);
            }
        };

        // --- Pipeline state: SSAO/GI off (the showroom foliage cutout rule),
        // TAA on (default), SSR + RT reflections ON full-res (the money dial). ---
        { x3::rhi::IRenderDevice::SsaoParams s{}; s.enabled = false; device->setSsaoParams(s); }
        { x3::rhi::IRenderDevice::GiParams   g{}; g.enabled = false; device->setGiParams(g); }
        {
            // --norefl HAS TO REACH HERE. main.cpp pushes an OFF ReflectionParams
            // for --norefl at startup, but this block then overwrote it
            // unconditionally, so `--norefl --screenshot-car` still captured with
            // reflections ON — i.e. the one rig that shows CLEARCOAT CAR PAINT had
            // no A/B at all. Honouring hc.noRefl here is what makes the off-side of
            // the clearcoat comparison capturable.
            x3::rhi::IRenderDevice::ReflectionParams rf{};
            const bool on = !hc.noRefl;
            rf.ssr = on; rf.rtFallback = on; rf.fullRes = on; rf.intensity = 1.0f;
            // DENOISE STAGE A/B (--refldn N, --refldn-disc S), threaded for
            // exactly the same reason as --norefl above: this host never runs the
            // per-frame cvar sync, so the CLI is the only way the knob can reach
            // it. `--refldn 0` disables the stage and is BIT-EXACT to the
            // pre-denoise renderer — that is the "before" side of the door-skin
            // blotch measurement this lane exists to move.
            if (hc.reflDenoise  >= 0)    rf.denoiseIters      = hc.reflDenoise;
            if (hc.reflDnDisc   >= 0.0f) rf.denoiseDiscScale  = hc.reflDnDisc;
            if (hc.reflDnNormal >= 0.0f) rf.denoiseNormalPow  = hc.reflDnNormal;
            if (hc.reflDnDepth  >= 0.0f) rf.denoiseDepthSigma = hc.reflDnDepth;
            device->setReflectionParams(rf);
            x3::logInfo(std::string("--screenshot-car: reflections ") + (on ? "ON" : "OFF (--norefl)") +
                        "; denoise iters=" + std::to_string(rf.denoiseIters) +
                        " disc=" + std::to_string(rf.denoiseDiscScale) +
                        "; rayTracingSupported=" +
                        (device->rayTracingSupported() ? "YES" : "NO"));
        }
        device->setShadowBounds(carX, carY, carZ, 40.0f);

        // Point lights for the car bay (key + fills; pre-multiplied color*intensity).
        std::vector<x3::rhi::PointLight> bay;
        auto addLight = [&](float x, float y, float z, float r, float cr, float cg, float cb) {
            x3::rhi::PointLight pl{}; pl.pos[0]=x; pl.pos[1]=y; pl.pos[2]=z; pl.range=r;
            pl.color[0]=cr; pl.color[1]=cg; pl.color[2]=cb; bay.push_back(pl);
        };
        addLight(carX,        carY + 3.2f, carZ,        14.0f, 6.0f, 5.9f, 5.6f);   // overhead key
        addLight(carX - 4.4f, carY + 1.6f, carZ + 1.5f, 10.0f, 1.2f, 2.2f, 3.4f);   // cool fill (cyan side)
        addLight(carX + 4.4f, carY + 1.6f, carZ - 1.5f, 10.0f, 3.4f, 1.6f, 0.7f);   // warm fill (amber side)

        // Night-sky planets for the EXTERIOR stills (the shared nightsky kit).
        int nPlanetTexFail = 0;
        x3::rhi::MeshHandle planetMesh{}, ringMesh{};
        std::vector<NightSkyPlanet> planets =
            loadNightSkyPlanets(device, planetMesh, nPlanetTexFail, "--screenshot-car", &ringMesh);

        // --- One settled still: pose, settle, capture. ---
        int shotFails = 0;
        auto still = [&](const std::string& name, bool interior, bool day,
                         float camYaw, float camDist, float camHeight, float fov) {
            applyShowroomTimeOfDay(device, day, &bay);
            if (!day) device->setSkyTime(10.0f);
            // Orbit the camera around the car anchor at `camYaw` (0 = looking from +Z).
            const float cx = carX + std::sin(camYaw) * camDist;
            const float cz = carZ + std::cos(camYaw) * camDist;
            const float cy = carY + camHeight;
            const float lx = carX - cx, ly = (carY + 0.55f) - cy, lz = carZ - cz;
            const float len = std::max(std::sqrt(lx*lx + ly*ly + lz*lz), 1e-3f);
            device->setCamera(cx, cy, cz, std::atan2(lz, lx), std::asin(ly / len), fov);
            const std::string path = carShotDir + "/" + name + ".png";
            const int kSettle = 90;   // TAA history + SSR + auto-exposure + IBL probe
            for (int i = 0; i < kSettle; ++i) {
                glfwPollEvents();
                if (i == kSettle - 1) device->armCapture(path.c_str());
                auto fr = device->beginFrame();
                if (fr.valid) {
                    drawScene(fr, interior);
                    if (!interior && !day)
                        drawNightSkyPlanets(device, fr, planetMesh, planets, 10.0f,
                                            cx, cy, cz, ringMesh);   // FOLD FIX: eye-anchored API
                }
                device->endFrame(fr);
            }
            const bool ok = device->captureFrame(path.c_str());
            if (ok) x3::logInfo("--screenshot-car: wrote " + path);
            else  { x3::logError("--screenshot-car: capture FAILED " + path); ++shotFails; }
        };

        // The turntable. Interior DAY (4 angles), interior NIGHT (2), then move
        // the car to the EXTERIOR pad for the planet-sky night pair.
        still("car_day_front34",  true, true,  0.65f, 5.0f, 1.25f, 48.0f);
        still("car_day_rear34",   true, true,  2.60f, 5.2f, 1.35f, 48.0f);
        still("car_day_profile",  true, true, -1.50f, 5.4f, 1.00f, 48.0f);
        still("car_day_frontlow", true, true,  0.10f, 4.8f, 0.60f, 52.0f);
        still("car_night_front34", true, false, 0.80f, 5.0f, 1.15f, 48.0f);
        still("car_night_rear34",  true, false, 2.45f, 5.2f, 1.05f, 48.0f);
        // EXTERIOR: re-pose the car on the asphalt pad away from the building.
        carX += 0.0f; carY = haveRoom ? carY : 0.0f; carZ = haveRoom ? (bmx[2] + 30.0f) : 0.0f;
        carXform(2.85f, carX, carY + kSlabTop, carZ, carT);
        car.setInstanceTransform(0, carT);
        device->setShadowBounds(carX, carY, carZ, 40.0f);
        // Re-aim the planets around the new pad (high in the back of frame).
        // FOLD FIX: fix/planets-sky moved NightSkyPlanet to az/el/angularDiameter
        // (eye-anchored in drawNightSkyPlanets) — convert the old worldPos/radius
        // fan (planets ~120 m out, 45+ m high, radius 13/26 m) to angles.
        {
            constexpr float kRadToDeg = 180.0f / 3.14159265358979323846f;
            constexpr float kDist = 120.0f;
            int pi = 0;
            for (NightSkyPlanet& b : planets) {
                const float az = -1.2f + 0.6f * (float)pi;     // fan behind the car (-Z side)
                const float height = 45.0f + 6.0f * (float)pi;
                const float radius = (pi == 1 || pi == 4) ? 26.0f : 13.0f;
                b.azimuthDeg         = az * kRadToDeg;
                b.elevationDeg       = std::atan2(height, kDist) * kRadToDeg;
                b.angularDiameterDeg = 2.0f * std::atan2(radius, kDist) * kRadToDeg;
                ++pi;
            }
        }
        still("car_extnight_front34", false, false, 2.95f, 6.4f, 1.25f, 50.0f);
        still("car_extnight_rear",    false, false, 0.35f, 7.0f, 0.85f, 48.0f);

        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return shotFails == 0 ? 0 : 1;
    }

    // ---- Planet preview (--screenshot-planet [path.png]) -------------------
    // Build a UV-sphere Moon body, load the 5 FORGE3D Moon textures from the pack,
    // shade it with the dedicated planet pipeline (object-space triplanar PBR +
    // scatter), hang it against a dark space backdrop lit from the side so a clear
    // day/night terminator shows, settle one headless frame, and capture a PNG.
    if (planetShot) {
        x3::logInfo("--screenshot-planet: rendering procedural Moon to " + planetShotPath);

        // --- Load the 5 Moon textures straight from the FORGE3D pack (no repo copy).
        const std::string kPack = "C:/Users/Tim/X3/Assets/FORGE3D/Planets/Moon/Textures/";
        const std::string kAtmo = "C:/Users/Tim/X3/Assets/FORGE3D/Planets/Atmosphere/";
        // albedo, normal, detail, spec, scatter — sRGB for color/albedo/scatter, linear for normal/spec.
        const std::string paths[5] = {
            kPack + "moon_01.png", kPack + "moon_01_normal.png", kPack + "moon_01_detail.png",
            kPack + "moon_01_spec.png", kAtmo + "sunset_yellow_05.png",
        };
        const bool srgbFlag[5] = { true, false, true, false, true };
        x3::rhi::TextureHandle tex[5] = {};
        bool allLoaded = true;
        for (int t = 0; t < 5; ++t) {
            int w = 0, h = 0, comp = 0;
            stbi_uc* px = stbi_load(paths[t].c_str(), &w, &h, &comp, 4);   // force RGBA8
            if (!px) {
                x3::logError("--screenshot-planet: FAILED to load " + paths[t]);
                allLoaded = false;
                continue;
            }
            tex[t] = device->createTexture(px, (uint32_t)w, (uint32_t)h, srgbFlag[t]);
            x3::logInfo("--screenshot-planet: loaded " + paths[t] + " (" + std::to_string(w) + "x" +
                        std::to_string(h) + (srgbFlag[t] ? ", srgb)" : ", linear)"));
            stbi_image_free(px);
        }
        if (!allLoaded) x3::logError("--screenshot-planet: one or more Moon textures missing — render may be flat");

        // --- UV-sphere Moon mesh (unit radius). pos == normal for the triplanar.
        x3::prims::PrimMesh sphere = x3::prims::makeUVSphere(64, 128);
        x3::rhi::MeshHandle moon = device->createMesh(sphere.verts.data(), (uint32_t)sphere.verts.size(),
                                                      sphere.index.data(), (uint32_t)sphere.index.size());

        // --- Model: unit sphere at the origin (radius 1). Column-major identity*scale.
        const float kRadius = 1.0f;
        float model[16] = {
            kRadius, 0, 0, 0,
            0, kRadius, 0, 0,
            0, 0, kRadius, 0,
            0, 0, 0, 1,
        };

        // --- Dark space backdrop: near-black zenith, faint horizon, a side sun so the
        // moon shows a clear day/night terminator. The Camera UBO sun direction the
        // planet frag reads is sourced from SkyParams.sunDir, so set it here.
        // Sun mostly TOWARD the camera (+Z dominant) and to the upper-side (+X,+Y) so
        // the camera-facing hemisphere reads as a bright gibbous Moon with the day/night
        // terminator sweeping across the LEFT third of the visible disc (a clear, lit
        // hero shot rather than a thin night-side crescent).
        const float sunDir[3] = { 0.32f, 0.26f, 1.0f };   // normalized internally
        x3::rhi::IRenderDevice::SkyParams sp{};
        sp.enabled = true;
        sp.sunDir[0] = sunDir[0]; sp.sunDir[1] = sunDir[1]; sp.sunDir[2] = sunDir[2];
        sp.sunColor[0] = 1.0f; sp.sunColor[1] = 0.96f; sp.sunColor[2] = 0.90f;
        sp.sunIntensity = 0.25f;   // low — this is deep space, not a daylit horizon
        sp.haze = 0.0f; sp.exposure = 1.0f;
        sp.zenith[0]  = 0.01f; sp.zenith[1]  = 0.01f; sp.zenith[2]  = 0.02f;  // near-black space
        sp.horizon[0] = 0.02f; sp.horizon[1] = 0.02f; sp.horizon[2] = 0.04f;  // slightly lighter
        device->setSkyParams(sp);
        device->setAmbient(0.07f, 0.07f, 0.09f);   // small cool fill so the night side reads as dark rock, not pure black
        device->setBloom(0.10f);
        // No SSAO/GI pre-pass: the planet uses the opaque depth LESS+write pipeline.
        { x3::rhi::IRenderDevice::SsaoParams s{}; s.enabled = false; device->setSsaoParams(s); }
        { x3::rhi::IRenderDevice::GiParams   g{}; g.enabled = false; device->setGiParams(g); }

        // --- Camera ~3 units back, looking at the origin, modest FOV so the unit
        // sphere fills ~70% of the frame.
        const float camx = 0.0f, camy = 0.0f, camz = 3.0f;
        const float yaw   = std::atan2(0.0f - camz, 0.0f - camx);   // toward origin in XZ
        const float pitch = 0.0f;                                   // level
        const float fovDeg = 40.0f;
        device->setCamera(camx, camy, camz, yaw, pitch, fovDeg);

        x3::logInfo("--screenshot-planet: cam(" + std::to_string(camx) + "," + std::to_string(camy) + "," +
            std::to_string(camz) + ") yaw=" + std::to_string(yaw) + " pitch=" + std::to_string(pitch) +
            " sun(" + std::to_string(sunDir[0]) + "," + std::to_string(sunDir[1]) + "," + std::to_string(sunDir[2]) +
            ") out=" + planetShotPath);

        const int kSettle = 8;
        bool wrote = false;
        for (int i = 0; i < kSettle; ++i) {
            glfwPollEvents();
            if (i == kSettle - 1) device->armCapture(planetShotPath.c_str());
            auto frame = device->beginFrame();
            if (frame.valid) {
                // typeIndex 0 = Moon; its 5 maps in pc.tex[0..4] order.
                device->drawPlanet(frame, moon, model, 0u /*Moon*/, tex, 5u, 0.0f);
            }
            device->endFrame(frame);
        }
        wrote = device->captureFrame(planetShotPath.c_str());
        if (wrote) x3::logInfo("--screenshot-planet: wrote " + planetShotPath);
        else       x3::logError("--screenshot-planet: capture FAILED");

        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return wrote ? 0 : 1;
    }

    // ---- Night-sky preview (--screenshot-nightsky [path.png]) --------------
    // Build ONE UV-sphere mesh and hang 6 VARIED planet TYPES staggered across a
    // dark, star-flecked dome — each shaded by its OWN per-type pipeline. Loads each
    // type's documented texture set from the FORGE3D pack (slot order per the frag
    // header / TEXTURE_MANIFEST.md), places them at varied azimuth/elevation/distance/
    // radius so they read as distinct sky bodies of different apparent sizes, and
    // captures a single 4x-SSAA PNG. The procedural starfield in sky.frag appears on
    // the dark sky automatically. (Atmosphere/suncorona/ring shells are DEFERRED.)
    if (nightskyShot) {
        x3::logInfo("--screenshot-nightsky: rendering staggered multi-planet sky to " + nightskyShotPath);

        // --- Build the UV-sphere + load the 6 FORGE3D planet types via the shared
        // helper (same files / slot order / srgb / default positions as before).
        int nTexFail = 0;
        x3::rhi::MeshHandle planetMesh{};
        x3::rhi::MeshHandle ringMesh{};
        std::vector<NightSkyPlanet> bodies =
            loadNightSkyPlanets(device, planetMesh, nTexFail, "--screenshot-nightsky", &ringMesh);

        if (nTexFail > 0)
            x3::logError("--screenshot-nightsky: " + std::to_string(nTexFail) +
                         " texture(s) missing — some bodies may render flat");

        // --- DARK NIGHT sky: near-black zenith, faint horizon, very low sun intensity,
        // no haze. The procedural starfield in sky.frag paints onto the dark dome.
        const float sunDir[3] = { 0.90f, 0.42f, 0.08f };  // az ~95 el ~25: half/gibbous phase on BOTH sky clusters (normalized internally)
        x3::rhi::IRenderDevice::SkyParams sp{};
        sp.enabled = true;
        sp.sunDir[0] = sunDir[0]; sp.sunDir[1] = sunDir[1]; sp.sunDir[2] = sunDir[2];
        sp.sunColor[0] = 1.0f; sp.sunColor[1] = 0.97f; sp.sunColor[2] = 0.92f;
        sp.sunIntensity = 0.04f;     // deep night — the bodies are the heroes, not the sky
        sp.haze = 0.0f; sp.exposure = 1.0f;
        sp.zenith[0]  = 0.005f; sp.zenith[1]  = 0.005f; sp.zenith[2]  = 0.012f;  // near-black
        sp.horizon[0] = 0.010f; sp.horizon[1] = 0.012f; sp.horizon[2] = 0.025f;  // slightly lighter
        device->setSkyParams(sp);
        device->setAmbient(0.06f, 0.06f, 0.10f);   // cool low fill
        device->setBloom(0.15f);
        device->setSkyTime(10.0f);   // non-zero sky-animation time (starfield rotation)
        { x3::rhi::IRenderDevice::SsaoParams s{}; s.enabled = false; device->setSsaoParams(s); }
        { x3::rhi::IRenderDevice::GiParams   g{}; g.enabled = false; device->setGiParams(g); }

        // --- Camera near origin, aimed at the HERO cluster of the celestial layout
        // (sun az +28 / terrestrial az -22 / moon az -44 / lava az +47 — az/el table in
        // loadNightSkyPlanets), tilted up so the bodies ride the upper frame. The
        // gas giant (az -147) / ice are out of this frame by design (the sky wraps
        // the full horizon); X3_NIGHTSKY_AZDEG aims the camera at any azimuth
        // (e.g. -147 for the ringed-gas-giant proof).
        const float camx = 0.0f, camy = 6.0f, camz = 18.0f;
        float aimAzDeg = 0.0f;
        if (const char* e = std::getenv("X3_NIGHTSKY_AZDEG")) aimAzDeg = (float)std::atof(e);
        const float yaw   = (aimAzDeg - 90.0f) * 3.14159265f / 180.0f;  // az 0 = -Z -> yaw = az - 90deg
        const float pitch = 0.45f;      // ~26deg up (bodies sit at 16..45deg elevation)
        const float fovDeg = 75.0f;
        device->setCamera(camx, camy, camz, yaw, pitch, fovDeg);

        x3::logInfo("--screenshot-nightsky: cam(" + std::to_string(camx) + "," + std::to_string(camy) + "," +
            std::to_string(camz) + ") yaw=" + std::to_string(yaw) + " pitch=" + std::to_string(pitch) +
            " fov=" + std::to_string(fovDeg) + " bodies=" + std::to_string(bodies.size()) +
            " out=" + nightskyShotPath);

        const float kUTime = 10.0f;   // fixed animation time for the still (animated types)
        const int kSettle = 8;
        bool wrote = false;
        for (int i = 0; i < kSettle; ++i) {
            glfwPollEvents();
            if (i == kSettle - 1) device->armCapture(nightskyShotPath.c_str());
            auto frame = device->beginFrame();
            if (frame.valid) {
                drawNightSkyPlanets(device, frame, planetMesh, bodies, kUTime,
                                    camx, camy, camz, ringMesh);
            }
            device->endFrame(frame);
        }
        wrote = device->captureFrame(nightskyShotPath.c_str());
        if (wrote) x3::logInfo("--screenshot-nightsky: wrote " + nightskyShotPath);
        else       x3::logError("--screenshot-nightsky: capture FAILED");

        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return wrote ? 0 : 1;
    }

    // ---- Cutscene film still (--cutscene-shot [path.png] --cuetime <s>) ----
    // HEADLESS: load the cutscene (default = the shipped cold open), build the full
    // cinematic scene (planets + ships + FX), seek the deterministic player to
    // --cuetime (events re-fire as seeked so trails/impacts are state-correct),
    // render + capture ONE supersampled frame, exit. The film-strip pipeline behind
    // docs/screenshots/coldopen/.
    if (cutsceneShot) {
        const std::string csPath = !cutsceneFile.empty()
            ? cutsceneFile
            : x3::game::assetRoot() + "/cutscenes/cold_open.cutscene.json";
        x3::cut::Cutscene cs;
        std::vector<std::string> errs;
        if (!x3::cut::loadCutsceneFile(csPath, cs, errs)) {
            for (const auto& e : errs) x3::logError("--cutscene-shot: " + e);
            device->shutdown();
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return 1;
        }
        x3::logInfo("--cutscene-shot: '" + cs.name + "' t=" + std::to_string(cueTime) +
                    " -> " + cutsceneShotPath);

        CinematicScene cin;
        device->beginUploadBatch();
        cin.load(*device, cs);
        device->endUploadBatch();
        cin.applyLook(*device);
        // --nofilmic: strip the film look (vignette/grain/split-tone) applyLook
        // just enabled — the A/B lever for the film-strip pipeline (OFF frames).
        if (hc.noFilmic) device->setFilmic(x3::rhi::IRenderDevice::FilmicParams{});

        x3::cut::CutscenePlayer player(cs);
        player.onEvent([&](const x3::cut::Event& e, bool) { cin.onEvent(e.name, cs, e.t); });
        player.seek(cueTime);
        cin.update(cs, cueTime);   // backfill the trail/puff state up to the scrub point

        const float t = player.time();
        const x3::cut::CamPose cam = x3::cut::evalCamera(cs, t);
        // ROLL-CAPABLE still: full basis so a keyed dutch angle banks the frame.
        float camFwd[3], camUp[3];
        x3::cut::camBasis(cam, camFwd, camUp);
        device->setCameraBasis(cam.pos.x, cam.pos.y, cam.pos.z, camFwd, camUp, cam.fov);
        // Per-shot sun lane (no sun keys -> re-applies the applyLook baseline).
        cin.applyShotSun(*device, cam);
        device->setSkyTime(10.0f + t * 0.02f);

        const int kSettle = 8;   // TAA/auto-exposure settle, like the other stills
        for (int i = 0; i < kSettle; ++i) {
            glfwPollEvents();
            if (i == kSettle - 1) device->armCapture(cutsceneShotPath.c_str());
            auto frame = device->beginFrame();
            if (frame.valid) {
                cin.drawWorld(*device, frame, cs, t);
                cin.drawOverlay(*device, frame, cs, t);
            }
            device->endFrame(frame);
        }
        const bool wrote = device->captureFrame(cutsceneShotPath.c_str());
        if (wrote) x3::logInfo("--cutscene-shot: wrote " + cutsceneShotPath);
        else       x3::logError("--cutscene-shot: capture FAILED");

        cin.destroy(*device);
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return wrote ? 0 : 1;
    }

    // ---- Terrain vantage mode (--screenshot-terrain [path.png]) ------------
    // Build the B2 tiled procedural terrain world (terrain meshes + the analytic
    // sky lit by the existing sun), pose a camera up on the hills looking toward
    // the sun so the lit rolling terrain, cast shadows, and sky all read, settle a
    // few frames so the shadow map + LOD register, and capture a PNG. Built
    // entirely through the public render API + a local Jolt world (so the terrain
    // collision path is exercised too) — no game/audio stack. EFLZ Level 1 is an
    // enclosed interior; this is how to SEE + verify the outdoor terrain.
    // ---- Lane 3: CASCADED SHADOW MAPS proof suite (--screenshot-csm <dir>) ----
    // Clean-room, original work; composes the engine's own TerrainStreamer + the
    // r_csm cvar. No id Tech / RBDOOM source consulted.
    //
    // The legacy sun cast from ONE 45 m camera-locked ortho box, so shadows
    // simply stopped ~45 m out. To make that visible (and to make the fix
    // visible) this host plants a row of PILLARS at 12 / 30 / 55 / 95 / 160 /
    // 230 m from a fixed camera on real streamed terrain, then captures:
    //   ab_<tag>_csm0/1.png   the same framing with r_csm 0 vs 1 at 3 vantages
    //   pan_csm1_<i>.png      a camera-PAN sequence from one spot (the money shot
    //                         for texel snapping: edges must not crawl)
    //   boundary_csm1.png     a cascade-boundary framing (blend band, not a line)
    //   boundary_csm1_noblend.png   the same with r_csm_blend 0, for comparison
    if (csmShot) {
        x3::logInfo("--screenshot-csm: cascaded shadow map proof suite -> " + csmShotDir);

        // A local console with the REAL cvar defaults registered, so the r_csm
        // A/B below flips exactly the cvar a player would type — and every other
        // render cvar keeps its shipped default instead of parsing as 0.
        std::unique_ptr<x3::con::IConsole> console(x3::con::createConsole());
        x3::apphost::registerViewmodelCVarsForTest(*console);
        // A flat apron viewed at a grazing angle is exactly the case where SSAO,
        // SSGI and TAA all leave banding. None of them are under test here and all
        // of them muddy a shadow-RANGE read, so this rig runs with them off.
        console->set("r_ssao", "0");
        console->set("r_ssgi", "0");
        console->set("r_taa",  "0");

        std::unique_ptr<x3::jobs::IJobSystem> tjobs(x3::jobs::createJobSystem());
        tjobs->init(0);
        std::unique_ptr<x3::phys::IPhysicsWorld> tphys(x3::phys::createPhysicsWorld());
        tphys->init();
        x3::game::Scene tscene;
        x3::game::TerrainStreamer streamer;
        const x3::game::TerrainConfig& tcfg = x3::game::worldTerrainConfig();

        // A LOW afternoon sun (~27 deg elevation). The engine default
        // normalize(0.4, 1, 0.3) is ~63 deg up, so a 12 m pillar casts only a 6 m
        // shadow — far too short to read at 200 m. Lowering it stretches shadows
        // to ~24 m, so the RANGE of the shadowed region is what the image shows.
        // The shadow pass reads this same m_sky.sunDir, so lighting, the sky disk
        // and the cascades all stay consistent.
        x3::rhi::IRenderDevice::SkyParams sp{};
        sp.enabled = true;
        sp.sunDir[0] = 0.62f; sp.sunDir[1] = 0.40f; sp.sunDir[2] = 0.47f;
        sp.sunColor[0] = 1.0f; sp.sunColor[1] = 0.97f; sp.sunColor[2] = 0.92f;
        // NO haze. The analytic sky's haze term is applied in DEPTH BANDS, and on
        // a flat apron viewed at a grazing angle those bands read as evenly spaced
        // dark stripes that look exactly like shadows — they are present with sun
        // shadowing forcibly disabled, which is how this was caught. A shadow-range
        // measurement needs a frame where a dark pixel can only be a shadow.
        sp.sunIntensity = 1.0f; sp.haze = 0.0f; sp.exposure = 1.0f;
        device->setSkyParams(sp);
        device->setCameraFar(15000.0f);
        device->setAmbient(0.30f, 0.32f, 0.38f);

        // ---- DELIBERATELY A CONTROLLED RIG, NOT A DRESSED WORLD ---------------
        // This host originally ran on the streamed terrain with a horizon ring.
        // That scene has tens of metres of relief plus 140 concentric ring bands,
        // and BOTH read as large dark shapes on the ground that are visually
        // indistinguishable from shadows — verified the hard way: with sun
        // shadowing forcibly disabled the "shadows" in those captures were still
        // there, byte-identical. A shadow-RANGE measurement needs a receiver where
        // a dark pixel can only mean a shadow, so the scene here is exactly: an
        // analytic sky, one flat apron, and the pillar ruler. Nothing else.
        const float dt = 1.0f / 60.0f;

        // ---- The rulers: pillars marching away from the camera ---------------
        // Placed along +Z (the camera looks that way), each seated on the terrain.
        // Tall and thin, so the CAST SHADOW is the readable feature.
        // Unit pillar; each instance is SCALED so it subtends a similar angle at
        // its distance and its shadow stays readable all the way out. Unscaled,
        // the 235 m pillar is a few pixels tall.
        x3::prims::PrimMesh box = x3::prims::makeBox(1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f);
        x3::rhi::MeshHandle pillarMesh = device->createMesh(
            box.verts.data(), (uint32_t)box.verts.size(),
            box.index.data(), (uint32_t)box.index.size());
        auto pillarTexels = x3::prims::makeCheckerRGBA(64, 8, 228, 224, 214, 206, 202, 194);
        x3::rhi::TextureHandle pillarTex = device->createTexture(pillarTexels.data(), 64, 64, true);

        // ---- WHY THE VIEW POINTS WHERE IT DOES (this matters for honesty) ----
        // The legacy box is 2*45 m in LIGHT space, i.e. in the plane PERPENDICULAR
        // to the sun. Its footprint on the GROUND is therefore ~90 m across the sun
        // azimuth but stretched ALONG it (by 1/sin(elevation), capped by the +-80 m
        // depth range). Aim the camera down-sun and even the legacy box reaches
        // ~140 m, which flatters it. Aiming ACROSS the sun azimuth is where the
        // 45 m radius actually bites — the honest framing, and also the racing case.
        //
        // The camera also stays LOW (8 m). The legacy box is centred on the CAMERA
        // in 3D, so a high camera lifts it off the ground entirely and the A/B
        // becomes a rigged "no shadows at all" comparison. 8 m is a chase-cam
        // height: the box genuinely covers a ~45 m radius of ground around it.
        const float sunAzX = 0.62f, sunAzZ = 0.47f;                   // sun XZ heading
        const float azLen  = std::sqrt(sunAzX * sunAzX + sunAzZ * sunAzZ);
        const float sunUX  = sunAzX / azLen,  sunUZ = sunAzZ / azLen;  // unit sun azimuth
        const float viewX  = -sunUZ,          viewZ = sunUX;           // perpendicular to it
        const float kYawView = std::atan2(viewZ, viewX);               // yaw: 0 = +X

        // A RULER of pillars marching away along the view axis, but OFFSET ~26 m
        // to one side (up-sun) so they never occlude the ground the camera is
        // looking at. Each pillar's shadow is cast DOWN-sun, i.e. across the view
        // centreline, so every shadow lands in clear view at a known distance.
        // 45 m — the legacy cascade's half-extent — falls between the 4th and 5th
        // station, so with r_csm 0 the stations past the 4th cast NOTHING.
        const float kPillarD[] = { 15.0f, 26.0f, 38.0f, 52.0f, 72.0f, 100.0f,
                                   135.0f, 180.0f, 235.0f };
        const float camX = 0.0f, camZ = 0.0f;
        const float groundY = 0.0f;
        // The apron IS the ground: y = 0, so a dark pixel can only be a shadow.
        const float kSideOffset = 26.0f;
        for (float pd : kPillarD) {
            // Mild growth only (keeps the far pillars visible without letting the
            // near ones fill the frame and hide the very shadows we are judging).
            const float sc = 1.0f + pd * 0.006f;
            const float hw = 1.6f * sc;                  // half-width
            const float hy = 7.0f * sc;                  // half-height
            const float px = camX + viewX * pd + sunUX * kSideOffset;
            const float pz = camZ + viewZ * pd + sunUZ * kSideOffset;
            const float py = groundY + 0.40f;                // stand on the apron
            x3::game::Entity e{};
            e.mesh = pillarMesh; e.tex = pillarTex;
            // Column-major: scale on the diagonal, translation in column 3.
            e.transform[0]  = hw; e.transform[5] = hy; e.transform[10] = hw;
            e.transform[12] = px; e.transform[13] = py + hy; e.transform[14] = pz;
            tscene.add(e);
        }
        // ---- A FLAT RECEIVING APRON ------------------------------------------
        // The streamed terrain has relief, so at 8 m eye height the ground past
        // ~120 m is neither flat nor continuous — a missing far shadow would be
        // indistinguishable from missing ground. This 300 m apron, laid a few cm
        // proud of the terrain, guarantees an unambiguous shadow-RECEIVING surface
        // across the whole 15..235 m ruler. It is test-rig geometry; the only
        // thing it changes is what the shadows land ON.
        {
            x3::prims::PrimMesh ap = x3::prims::makeBox(300.0f, 0.4f, 300.0f, 0.0f, 0.0f, 0.0f, 0.02f);
            x3::rhi::MeshHandle apMesh = device->createMesh(
                ap.verts.data(), (uint32_t)ap.verts.size(),
                ap.index.data(), (uint32_t)ap.index.size());
            // FLAT, UNIFORM colour on purpose. A checker here reads as evenly
            // spaced dark bands and is indistinguishable from shadows at a glance
            // — it fooled this lane for an hour. The apron must contribute NO
            // value variation, so the only dark thing in frame is a shadow.
            auto apTexels = x3::prims::makeCheckerRGBA(64, 32, 128, 140, 106, 128, 140, 106);
            x3::rhi::TextureHandle apTex = device->createTexture(apTexels.data(), 64, 64, true);
            x3::game::Entity a{};
            a.mesh = apMesh; a.tex = apTex;
            a.transform[12] = camX + viewX * 110.0f;
            a.transform[13] = groundY;
            a.transform[14] = camZ + viewZ * 110.0f;
            tscene.add(a);
        }

        x3::logInfo("--screenshot-csm: planted 9 pillars at 15..235 m ACROSS the sun azimuth "
                    "(the legacy 45 m cutoff is tightest on this axis) on a 300 m flat apron");

        const float camY = groundY + 0.40f + 8.0f;
        const float kYawPlusZ = kYawView;

        // One capture: set the cvars, settle N frames, arm on the last, write.
        auto shot = [&](const std::string& file, int csmOn, float blend,
                        float camx, float camy, float camz,
                        float yaw, float pitch, float fov, int frames) -> bool {
            console->set("r_csm", csmOn ? "1" : "0");
            console->set("r_csm_blend", std::to_string(blend));
            const std::string path = csmShotDir + "/" + file;
            for (int i = 0; i < frames; ++i) {
                glfwPollEvents();
                // The cvar sync hub — without this r_csm never reaches the device.
                x3::apphost::applyRtaoCVarsForTest(*console, *device);
                device->setCamera(camx, camy, camz, yaw, pitch, fov);
                if (i == frames - 1) device->armCapture(path.c_str());
                auto frame = device->beginFrame();
                if (frame.valid) tscene.render(*device, frame);
                device->endFrame(frame);
            }
            const bool ok = device->captureFrame(path.c_str());
            if (ok) x3::logInfo("--screenshot-csm: wrote " + path);
            else    x3::logError("--screenshot-csm: capture FAILED for " + path);
            return ok;
        };

        // Mean GPU frame time at a given cascade setting (warmup frames dropped).
        auto measure = [&](int csmOn, int frames, int warmup) -> double {
            console->set("r_csm", csmOn ? "1" : "0");
            double sum = 0.0; int n = 0;
            for (int i = 0; i < frames; ++i) {
                glfwPollEvents();
                x3::apphost::applyRtaoCVarsForTest(*console, *device);
                device->setCamera(camX, camY, camZ, kYawPlusZ, -0.15f, 70.0f);
                auto frame = device->beginFrame();
                if (frame.valid) tscene.render(*device, frame);
                device->endFrame(frame);
                if (i >= warmup) { sum += device->stats().gpuFrameMs; ++n; }
            }
            return (n > 0) ? sum / (double)n : 0.0;
        };

        // ---- (a) A/B at three framings, r_csm 0 vs 1 -------------------------
        struct Vantage { const char* tag; float pitch; float fov; };
        const Vantage kV[] = {
            { "near", -0.30f, 70.0f },   // the 15-52 m stations fill the frame
            { "mid",  -0.15f, 70.0f },   // the legacy 45 m cutoff crosses the frame
            { "far",  -0.075f, 45.0f },  // narrow FOV, out to the 235 m station
        };
        for (const Vantage& v : kV) {
            shot(std::string("ab_") + v.tag + "_csm0.png", 0, 0.12f,
                 camX, camY, camZ, kYawPlusZ, v.pitch, v.fov, 24);
            shot(std::string("ab_") + v.tag + "_csm1.png", 1, 0.12f,
                 camX, camY, camZ, kYawPlusZ, v.pitch, v.fov, 24);
        }

        // ---- (a2) THE RANGE SHOT — a telephoto framing of the disputed band ----
        // At 8 m eye height the ground from 60 m to 250 m subtends only ~6 deg, so
        // a 70 deg lens buries it in a few dozen scanlines and the A/B looks like
        // nothing changed. A 10 deg lens puts EXACTLY the band the legacy 45 m box
        // cannot reach across the whole frame. This is the honest money shot for
        // "shadows persist well past 45 m".
        shot("ab_range_csm0.png", 0, 0.12f, camX, camY, camZ, kYawPlusZ, -0.082f, 10.0f, 24);
        shot("ab_range_csm1.png", 1, 0.12f, camX, camY, camZ, kYawPlusZ, -0.082f, 10.0f, 24);

        // ---- (b) CAMERA PAN, r_csm 1 — the texel-snapping money shot ---------
        // Same spot, yaw swept in small steps. With an unsnapped origin (or a
        // rotation-dependent extent) shadow edges crawl and shimmer frame to
        // frame; with both fixes the edges hold still against the terrain.
        for (int i = 0; i < 6; ++i) {
            const float yaw = kYawPlusZ - 0.10f + (float)i * 0.04f;
            char nm[64]; std::snprintf(nm, sizeof(nm), "pan_csm1_%d.png", i);
            shot(nm, 1, 0.12f, camX, camY, camZ, yaw, -0.22f, 70.0f, 14);
        }

        // ---- (c) CASCADE BOUNDARY: blend band vs a hard line ------------------
        // A shallow, high framing so the cascade-0/1 split (~17 m) and 1/2 (~40 m)
        // both cross the frame. The no-blend variant is the control.
        shot("boundary_csm1.png",         1, 0.35f, camX, camY, camZ, kYawPlusZ, -0.13f, 70.0f, 24);
        shot("boundary_csm1_noblend.png", 1, 0.0f,  camX, camY, camZ, kYawPlusZ, -0.13f, 70.0f, 24);

        // ---- (c2) CASCADE DEBUG: which cascade is each pixel in? --------------
        // Steps shadow visibility per cascade, so the split rings are visible.
        // This is the diagnostic that answers "is the far cascade even selected".
        console->set("r_csm_debug", "1");
        shot("cascades_debug.png", 1, 0.0f, camX, camY, camZ, kYawPlusZ, -0.15f, 70.0f, 24);
        shot("legacy_footprint_debug.png", 0, 0.0f, camX, camY, camZ, kYawPlusZ, -0.075f, 45.0f, 24);
        shot("legacy_footprint_range_debug.png", 0, 0.0f, camX, camY, camZ, kYawPlusZ, -0.082f, 10.0f, 24);
        console->set("r_csm_debug", "0");

        // ---- (d) The r_shadowforward INTERIM, as an A/B reference -------------
        // Slides the LEGACY box forward along the camera axis. It does not add
        // range, it MOVES the range — useful for a car, useless for a wide view.
        console->set("r_shadowforward", "30");
        shot("interim_forward30_csm0.png", 0, 0.12f, camX, camY, camZ, kYawPlusZ, -0.15f, 70.0f, 24);
        console->set("r_shadowforward", "0");

        // ---- Perf: N cascades vs the single cascade ---------------------------
        console->set("r_csm_blend", "0.12");
        const double ms0 = measure(0, 140, 60);
        const double ms1 = measure(1, 140, 60);
        char perf[256];
        std::snprintf(perf, sizeof(perf),
            "--screenshot-csm: GPU frame time  r_csm 0 (1 cascade) = %.3f ms | "
            "r_csm 1 (%d cascades) = %.3f ms | delta = %+.3f ms (%+.1f%%)",
            ms0, (int)x3::csm::kNumCascades, ms1, ms1 - ms0,
            (ms0 > 0.0) ? (ms1 - ms0) / ms0 * 100.0 : 0.0);
        x3::logInfo(perf);
        {
            const x3::rhi::RenderStats st = device->stats();
            x3::logInfo("--screenshot-csm: draws = " + std::to_string(st.drawCalls));
        }

        console->set("r_csm", "0");
        tphys->shutdown();
        tjobs->shutdown();
        return 0;
    }

    if (terrainShot) {
        x3::logInfo("--screenshot-terrain: rendering STREAMED terrain world to " + terrainShotPath);

        // B3: this path exercises the STREAMER under validation. A job system
        // generates tiles async; the focus is SWEPT across the world during the
        // frame loop so stream-IN (createMesh + addStaticMesh) AND stream-OUT
        // (destroyMesh + removeBody) both run inside validated frames, proving the
        // async upload + teardown barriers are validation-clean.
        //
        // W8-3: the host now builds from the CANONICAL world config (features on:
        // macro relief / mountain ranges / pads / basin), raises the far plane so
        // the horizon draws, adds a horizon ring (far-terrain stitch), and writes
        // a SUITE: the classic sweep vista (base path) + a variability pair
        // (_plains / _hills, auto-probed from the relief field) + the Northern
        // Range framed from its foothills (_range).
        std::unique_ptr<x3::jobs::IJobSystem> tjobs(x3::jobs::createJobSystem());
        tjobs->init(0);
        std::unique_ptr<x3::phys::IPhysicsWorld> tphys(x3::phys::createPhysicsWorld());
        tphys->init();
        x3::game::Scene tscene;
        x3::game::TerrainStreamer streamer;
        const x3::game::TerrainConfig& tcfg = x3::game::worldTerrainConfig();

        // Turn ON the analytic sky with the SAME sun the shadow pass + mesh.frag
        // use (normalize(0.4,1,0.3)) so the disk sits where the world is lit from.
        x3::rhi::IRenderDevice::SkyParams sp{};
        sp.enabled = true;
        sp.sunDir[0] = 0.4f; sp.sunDir[1] = 1.0f; sp.sunDir[2] = 0.3f;
        sp.sunColor[0] = 1.0f; sp.sunColor[1] = 0.97f; sp.sunColor[2] = 0.92f;
        sp.sunIntensity = 1.0f; sp.haze = 0.5f; sp.exposure = 1.0f;
        device->setSkyParams(sp);
        device->setCameraFar(15000.0f);   // W8-3: the ranges live 7-10 km out
        device->setAmbient(0.30f, 0.32f, 0.38f);   // daylight sky fill (shadow sides read)

        // Start the focus well away from the origin (proves unbounded coords) and
        // bring up the ring there.
        float fx = -90.0f, fz = -120.0f;
        streamer.init(tscene, *device, *tphys, tjobs.get(), tcfg, fx, fz, /*radius=*/8);
        streamer.setUploadBudget(64);

        // W8-3 horizon stitch: a world-wide ring from the same field (recessed
        // under the streamed tiles) so the mountain ranges read on the horizon,
        // plus a finer local ring around the Northern-Range vantage so the peaks
        // are not chunky in the _range framing.
        {
            x3::game::HorizonRingDesc hr{};
            hr.centerX = 0.0f; hr.centerZ = 0.0f;
            hr.rInner = 240.0f; hr.rOuter = 13000.0f;
            hr.rings = 140; hr.segments = 160; hr.yBias = -3.0f;
            x3::game::addTerrainHorizonRing(tscene, *device, streamer.groundTexture(), hr);
            x3::game::HorizonRingDesc nr{};
            nr.centerX = 1300.0f; nr.centerZ = 8900.0f;  // N-range vantage (sunlit NE side)
            nr.rInner = 60.0f; nr.rOuter = 3600.0f;
            nr.rings = 120; nr.segments = 140; nr.yBias = -1.5f;
            x3::game::addTerrainHorizonRing(tscene, *device, streamer.groundTexture(), nr);
        }

        const float sunYaw   = std::atan2(0.3f, 0.4f);  // toward the sun in XZ
        const float camPitch = -0.16f;                  // ~9deg down: hills + shadows + sky

        const float dt = 1.0f / 60.0f;
        // ---- Shot 1: the classic sweep vista (stream-in/out churn, measured). ----
        const int kFrames = 140, kWarmup = 40;
        double sumGpuMs = 0.0; int measured = 0;
        for (int i = 0; i < kFrames; ++i) {
            glfwPollEvents();
            // Sweep the streaming focus across tile boundaries (in/out churn).
            fx += 4.0f;   // ~4 m/frame => crosses a 32 m tile every ~8 frames
            tphys->step(dt);
            streamer.update(tscene, *device, *tphys, fx, fz);

            // Camera trails the focus, elevated + looking down-across toward the sun.
            const float surfY = streamer.heightAt(fx, fz);
            const float camY  = surfY + 18.0f;
            device->setCamera(fx, camY, fz, sunYaw, camPitch, 70.0f);

            if (i == kFrames - 1) device->armCapture(terrainShotPath.c_str());
            auto frame = device->beginFrame();
            if (frame.valid) tscene.render(*device, frame);
            device->endFrame(frame);
            const x3::rhi::RenderStats s = device->stats();
            if (i >= kWarmup) { sumGpuMs += s.gpuFrameMs; ++measured; }
        }
        bool wrote = device->captureFrame(terrainShotPath.c_str());
        if (wrote) {
            const x3::rhi::RenderStats st = device->stats();
            const double avgGpu = measured ? sumGpuMs / measured : 0.0;
            const double gpuFps = (avgGpu > 1e-6) ? (1000.0 / avgGpu) : 0.0;
            char rb[256];
            std::snprintf(rb, sizeof(rb),
                "--screenshot-terrain: wrote %s | resident=%u (max %u) created=%llu destroyed=%llu "
                "draws=%u tris=%u | GPU=%.3f ms (~%.0f fps GPU-bound)",
                terrainShotPath.c_str(), streamer.residentCount(),
                streamer.maxResidentForRadius(),
                (unsigned long long)streamer.tilesCreated(),
                (unsigned long long)streamer.tilesDestroyed(),
                st.drawCalls, st.triangles, avgGpu, gpuFps);
            x3::logInfo(rb);
        } else x3::logError("--screenshot-terrain: capture FAILED");

        // ---- W8-3 shots 2-4: relocate the focus + settle + capture. ------------
        auto suffixed = [&](const char* tag) -> std::string {
            std::string p = terrainShotPath;
            const size_t dot = p.find_last_of('.');
            return (dot == std::string::npos) ? p + tag : p.substr(0, dot) + tag + p.substr(dot);
        };
        auto shotAt = [&](float focusX, float focusZ, float camX, float camY, float camZ,
                          float yaw, float pitch, const std::string& path) -> bool {
            const int kF = 170;   // relocate + drain the new residency ring + settle
            for (int i = 0; i < kF; ++i) {
                glfwPollEvents();
                tphys->step(dt);
                // Nudge across a tile boundary once so the full ring re-enqueues.
                const float nfx = (i == 1) ? (focusX + 40.0f) : focusX;
                streamer.update(tscene, *device, *tphys, nfx, focusZ);
                device->setCamera(camX, camY, camZ, yaw, pitch, 70.0f);
                if (i == kF - 1) device->armCapture(path.c_str());
                auto frame = device->beginFrame();
                if (frame.valid) tscene.render(*device, frame);
                device->endFrame(frame);
            }
            const bool ok = device->captureFrame(path.c_str());
            if (ok) x3::logInfo("--screenshot-terrain: wrote " + path);
            else    x3::logError("--screenshot-terrain: capture FAILED for " + path);
            return ok;
        };

        // Variability pair: probe candidate spots for local relief (stddev-ish of
        // the field) and shoot the flattest as _plains, the hilliest as _hills.
        {
            const float cand[6][2] = { {-1500,-900}, {900,1400}, {-2300,1500},
                                       {2100,-600}, {-600,-2300}, {1600,2300} };
            int flat = 0, hilly = 0; float loVar = 1e30f, hiVar = -1e30f;
            for (int c = 0; c < 6; ++c) {
                float mn = 1e30f, mx = -1e30f;
                for (int j = -2; j <= 2; ++j) for (int k = -2; k <= 2; ++k) {
                    const float h = x3::game::terrainHeightAtWorld(
                        cand[c][0] + (float)j * 45.0f, cand[c][1] + (float)k * 45.0f);
                    mn = std::min(mn, h); mx = std::max(mx, h);
                }
                const float v = mx - mn;
                if (v < loVar) { loVar = v; flat = c; }
                if (v > hiVar) { hiVar = v; hilly = c; }
            }
            const float px = cand[flat][0],  pz = cand[flat][1];
            const float hx2 = cand[hilly][0], hz2 = cand[hilly][1];
            wrote &= shotAt(px, pz, px, x3::game::terrainHeightAtWorld(px, pz) + 16.0f, pz,
                            sunYaw, -0.12f, suffixed("_plains"));
            wrote &= shotAt(hx2, hz2, hx2, x3::game::terrainHeightAtWorld(hx2, hz2) + 20.0f, hz2,
                            sunYaw, -0.14f, suffixed("_hills"));
        }
        // The Northern Range from its NE foothills looking SW along the chain, so
        // the camera sees the SUNLIT faces (sun sits in the +X+Z sky): peaks with
        // rock-by-slope + snow-sub-by-height filling the frame, slight up-pitch.
        {
            const float vx = 2200.0f, vz = 9700.0f;
            const float vy = x3::game::terrainHeightAtWorld(vx, vz) + 110.0f;   // aerial
            const float vYaw = std::atan2(8300.0f - vz, 300.0f - vx);   // toward the spine mid
            wrote &= shotAt(vx, vz, vx, vy, vz, vYaw, -0.05f, suffixed("_range"));
        }

        // Tear down the streamer (destroys resident meshes + bodies) before the
        // device/physics, then stop the job system.
        streamer.shutdown(tscene, *device, *tphys);
        tjobs->shutdown();
        tphys->shutdown();
        device->shutdown();
        glfwDestroyWindow(window);
        glfwTerminate();
        return wrote ? 0 : 1;
    }

    // ---- Ocean vantage mode (--screenshot-ocean [path.png]) ----------------
    // Build the procedural terrain world (streamed) + turn ON the animated ocean
    // at a sea level part-way up the height range, so the lower terrain is
    // submerged and the hills rise out of the sea (a real shoreline). The sky +
    // sun are the same the rest of the engine uses, so the water's sky-reflection
    // + sun glint agree with the backdrop. Pose a camera on high ground looking
    // out across the water toward the sun, advance the wave clock a few frames so
    // the surface animates + the shadow map registers, and capture a PNG. Built
    // through the public render API + a local Jolt world, like --screenshot-terrain.
    if (oceanShot) {
        x3::logInfo("--screenshot-ocean: rendering terrain + animated ocean to " + oceanShotPath);

        std::unique_ptr<x3::jobs::IJobSystem> ojobs(x3::jobs::createJobSystem());
        ojobs->init(0);
        std::unique_ptr<x3::phys::IPhysicsWorld> ophys(x3::phys::createPhysicsWorld());
        ophys->init();
        x3::game::Scene oscene;
        x3::game::TerrainStreamer ostream;
        x3::game::TerrainConfig ocfg;   // 32 m tiles; heightScale ~55 m

        const float sunYaw = std::atan2(0.3f, 0.4f);  // toward the sun in XZ

        // Sky (matches the engine sun) so the water reflection + sky agree.
        x3::rhi::IRenderDevice::SkyParams sp{};
        sp.enabled = true;
        sp.sunDir[0] = 0.4f; sp.sunDir[1] = 1.0f; sp.sunDir[2] = 0.3f;
        sp.sunColor[0] = 1.0f; sp.sunColor[1] = 0.97f; sp.sunColor[2] = 0.92f;
        sp.sunIntensity = 1.0f; sp.haze = 0.5f; sp.exposure = 1.0f;
        device->setSkyParams(sp);

        // Ocean: sea level part-way up the terrain height range so valleys flood
        // and hills become shorelines. Tasteful Gerstner defaults.
        const float seaLevel = 14.0f;
        x3::rhi::IRenderDevice::WaterParams wp{};
        wp.enabled = true;
        wp.seaLevel = seaLevel;
        wp.amplitude = 0.6f; wp.steepness = 0.6f; wp.waveLength = 16.0f; wp.speed = 1.0f;
        wp.deepColor[0] = 0.015f; wp.deepColor[1] = 0.06f;  wp.deepColor[2] = 0.10f;
        wp.shallowColor[0] = 0.10f; wp.shallowColor[1] = 0.32f; wp.shallowColor[2] = 0.36f;
        wp.sunDir[0] = 0.4f; wp.sunDir[1] = 1.0f; wp.sunDir[2] = 0.3f;
        wp.specular = 14.0f; wp.fresnel = 0.02f;

        // Find a vantage on high ground: scan a few points for one well above sea
        // level, then back the camera up the sun azimuth so the water spans the
        // frame toward the sun. Bring up the residency ring around that focus.
        float fx = 40.0f, fz = -10.0f;
        ostream.init(oscene, *device, *ophys, ojobs.get(), ocfg, fx, fz, /*radius=*/8);
        // Fill the resident ring fast so the shoreline + a generous expanse of
        // terrain are visible in the single capture (interactive uses the default
        // budget; this is a headless still).
        ostream.setUploadBudget(64);

        const float surfY = ostream.heightAt(fx, fz);
        const float camY  = std::max(surfY, seaLevel) + 10.0f;
        const float camPitch = -0.14f;                  // ~8deg down: water + shore + sky

        const float dt = 1.0f / 60.0f;
        const int kFrames = 220, kWarmup = 120;
        double sumGpuMs = 0.0; int measured = 0;
        for (int i = 0; i < kFrames; ++i) {
            glfwPollEvents();
            ophys->step(dt);
            // The streamer only enqueues the FULL residency ring on a focus tile
            // boundary cross (init seeds just the 3x3). Nudge the focus across one
            // tile on frame 1 to trigger the ring request, then hold it at the
            // vantage so the wide resident set drains in over the warmup window.
            const float focusX = (i == 1) ? (fx + 40.0f) : fx;
            ostream.update(oscene, *device, *ophys, focusX, fz);
            wp.time = (float)i * dt;        // advance the wave animation clock
            device->setWaterParams(wp);
            device->setCamera(fx - 26.0f, camY, fz, sunYaw, camPitch, 70.0f);

            if (i == kFrames - 1) device->armCapture(oceanShotPath.c_str());
            auto frame = device->beginFrame();
            if (frame.valid) oscene.render(*device, frame);
            device->endFrame(frame);
            const x3::rhi::RenderStats s = device->stats();
            if (i >= kWarmup) { sumGpuMs += s.gpuFrameMs; ++measured; }
        }
        const bool wrote = device->captureFrame(oceanShotPath.c_str());
        if (wrote) {
            const x3::rhi::RenderStats st = device->stats();
            const double avgGpu = measured ? sumGpuMs / measured : 0.0;
            const double gpuFps = (avgGpu > 1e-6) ? (1000.0 / avgGpu) : 0.0;
            char rb[256];
            std::snprintf(rb, sizeof(rb),
                "--screenshot-ocean: wrote %s | seaLevel=%.1f resident=%u draws=%u tris=%u | "
                "GPU=%.3f ms (~%.0f fps GPU-bound)",
                oceanShotPath.c_str(), seaLevel, ostream.residentCount(),
                st.drawCalls, st.triangles, avgGpu, gpuFps);
            x3::logInfo(rb);
        } else x3::logError("--screenshot-ocean: capture FAILED");

        ostream.shutdown(oscene, *device, *ophys);
        ojobs->shutdown();
        ophys->shutdown();
        device->shutdown();
        glfwDestroyWindow(window);
        glfwTerminate();
        return wrote ? 0 : 1;
    }

    // ---- Undersea base vantage (--screenshot-oceanbase [path.png]) ---------
    // W3-4: build the ocean_base zone (textured hull + emissive practicals) and
    // capture (1) a submarine-approach shot and (2) a dock closeup, under DEEP-
    // WATER fog (ART_BIBLE: underwater reads DEPTH — light falls off, silhouettes
    // + practicals over detail; never flat blue). Fog rides AD-1's graph pass.
    if (oceanBaseShot) {
        x3::logInfo("--screenshot-oceanbase: rendering the undersea base to " + oceanBaseShotPath);

        std::unique_ptr<x3::phys::IPhysicsWorld> bphys(x3::phys::createPhysicsWorld());
        bphys->init();
        x3::game::Scene bscene;
        x3::game::OceanBase base;
        base.build(bscene, *device, *bphys);
        const x3::game::OceanBasePlan& plan = base.plan();

        // Deep-water column: a dim blue-green "surface glow" sun from above (what
        // little light survives 60+ m of water) — the emissive practicals carry
        // the composition; the sun only separates silhouettes.
        x3::rhi::IRenderDevice::SkyParams sp{};
        sp.enabled = true;
        sp.sunDir[0] = 0.05f; sp.sunDir[1] = 1.0f; sp.sunDir[2] = 0.10f;
        sp.sunColor[0] = 0.25f; sp.sunColor[1] = 0.55f; sp.sunColor[2] = 0.60f;
        sp.sunIntensity = 0.35f; sp.haze = 0.9f; sp.exposure = 1.0f;
        device->setSkyParams(sp);

        // The water column IS fog: teal-green, heavy, silhouette-preserving.
        x3::rhi::IRenderDevice::FogParams fog{};
        fog.enabled = true;
        fog.color[0] = 0.012f; fog.color[1] = 0.050f; fog.color[2] = 0.055f;
        fog.density = 0.016f;          // ~80% extinction at 100 m — depth, not soup
        fog.start = 2.0f;
        fog.maxOpacity = 0.94f;
        device->setFog(fog);

        // The abyssal station's cool key/rim lights (OceanBase::build sized + placed
        // them at the landmark) so its hull catches light in the dark deep — the blue
        // emissive windows carry the glow, these make it read in 3D, not a silhouette.
        const auto& slights = base.stationLights();
        if (!slights.empty()) device->setPointLights(slights.data(), (uint32_t)slights.size());

        auto renderShot = [&](float cx, float cy, float cz, float yaw, float pitch,
                              const std::string& path) -> bool {
            const int kFrames = 90;    // settle: shadows + TAA history + bloom
            for (int i = 0; i < kFrames; ++i) {
                glfwPollEvents();
                bphys->step(1.0f / 60.0f);
                device->setCamera(cx, cy, cz, yaw, pitch, 70.0f);
                if (i == kFrames - 1) device->armCapture(path.c_str());
                auto frame = device->beginFrame();
                if (frame.valid) bscene.render(*device, frame);
                device->endFrame(frame);
            }
            return device->captureFrame(path.c_str());
        };

        // Shot 1 — approach from the DOCK side, low in the water so the base LOOMS
        // (deck-level eye, slight up-pitch): dock mouth frame + window bands + the
        // reactor crown all in silhouette through the murk.
        const float ax = plan.cx + plan.radius + 110.0f, ay = plan.baseDeckY - 4.0f, az = plan.cz + 34.0f;
        const float aYaw = std::atan2(plan.cz - az, plan.cx - ax);
        const bool w1 = renderShot(ax, ay, az, aYaw, 0.04f, oceanBaseShotPath);

        // Shot 2 — dock closeup: the amber entry strip + panel steel + airlock.
        std::string dockPath = oceanBaseShotPath;
        const size_t dot = dockPath.find_last_of('.');
        dockPath = (dot == std::string::npos) ? dockPath + "_dock"
                                              : dockPath.substr(0, dot) + "_dock" + dockPath.substr(dot);
        const float dx = plan.cx + plan.radius + 34.0f, dy = plan.baseDeckY - 2.0f, dz = plan.cz + 26.0f;
        const float dYaw = std::atan2(plan.cz - dz, (plan.cx + plan.radius) - dx);
        const bool w2 = renderShot(dx, dy, dz, dYaw, -0.06f, dockPath);

        // Shot 3 — the HERO station shot: the abyssal station lit on the seabed out
        // in the deep, framed close so its blue-emissive windows + cool-lit hull read
        // as a POWERED structure through the murk, with seabed + water column context.
        std::string stationPath = oceanBaseShotPath;
        const size_t sdot = stationPath.find_last_of('.');
        stationPath = (sdot == std::string::npos) ? stationPath + "_station"
                                                  : stationPath.substr(0, sdot) + "_station" + stationPath.substr(sdot);
        float stx, sty, stz; base.stationPos(stx, sty, stz);
        const float svx = stx, svy = sty + 26.0f, svz = stz + 62.0f;   // above-front, close
        const float sYaw = std::atan2(stz - svz, stx - svx);
        const bool w3 = renderShot(svx, svy, svz, sYaw, -0.14f, stationPath);

        if (w1) x3::logInfo("--screenshot-oceanbase: wrote " + oceanBaseShotPath);
        if (w2) x3::logInfo("--screenshot-oceanbase: wrote " + dockPath);
        if (w3) x3::logInfo("--screenshot-oceanbase: wrote " + stationPath);
        if (!w1 || !w2 || !w3) x3::logError("--screenshot-oceanbase: capture FAILED");

        x3::rhi::IRenderDevice::FogParams off{};
        device->setFog(off);   // leave the device clean for whoever runs next
        device->setPointLights(nullptr, 0);   // clear the station lights for the next host
        bphys->shutdown();
        device->shutdown();
        glfwDestroyWindow(window);
        glfwTerminate();
        return (w1 && w2 && w3) ? 0 : 1;
    }

    // ---- City vantage (--screenshot-city [path.png]) — W8-3 -----------------
    // Build the CITY REGION (Scrapyard / New District / Industrial blockout-plus,
    // app/city.cpp) on the canonical world terrain and capture (1) a New-District
    // establishing shot (varied skyscraper massing + street grid), (2) a street-
    // level main-street shot (_street: shops + neon + towers), (3) the Scrapyard
    // core (_scrapyard: the named-building roster + helipad tower). Ground is a
    // full horizon ring (no streamer — the city anchors analytically via
    // placeOnTerrain onto the same field), so the mountain ranges read behind
    // the skyline.
    if (cityShot) {
        x3::logInfo("--screenshot-city: rendering the city districts to " + cityShotPath);

        std::unique_ptr<x3::phys::IPhysicsWorld> cphys(x3::phys::createPhysicsWorld());
        cphys->init();
        x3::game::Scene cscene;

        // Daylight sky (the canonical outdoor sun) + a long far plane for the
        // ranges behind the skyline.
        x3::rhi::IRenderDevice::SkyParams sp{};
        sp.enabled = true;
        // AFTERNOON sun (low-ish, from the +X+Z sky): vertical massing catches
        // real light — at noon every facade was a black silhouette.
        sp.sunDir[0] = 0.62f; sp.sunDir[1] = 0.52f; sp.sunDir[2] = 0.46f;
        sp.sunColor[0] = 1.0f; sp.sunColor[1] = 0.93f; sp.sunColor[2] = 0.82f;
        sp.sunIntensity = 1.5f; sp.haze = 0.5f; sp.exposure = 1.0f;
        device->setSkyParams(sp);
        device->setCameraFar(15000.0f);
        device->setAmbient(0.38f, 0.40f, 0.46f);   // daylight sky fill (shadow sides read)
        device->setExposure(1.25f);                // entity-PBR path runs dim outdoors

        // Ground: one fine-inner horizon ring centered between the districts —
        // the flat city pads live in the field, so buildings/roads seat cleanly.
        {
            x3::rhi::TextureHandle splat = x3::game::makeTerrainSplatMarker(*device);
            x3::game::HorizonRingDesc hr{};
            hr.centerX = -200.0f; hr.centerZ = 450.0f;   // between Scrapyard + District
            hr.rInner = 12.0f; hr.rOuter = 13000.0f;
            hr.rings = 150; hr.segments = 160; hr.yBias = -0.05f;
            x3::game::addTerrainHorizonRing(cscene, *device, splat, hr);
        }

        // ==== CONTENT WIRING (lane inspx/content-wiring) =====================
        // THE MONEY SHOT. `--screenshot-city` built the districts under an
        // afternoon sun and NOTHING ELSE -- no lamps, no window light, no sign
        // wash, because the predecessor renderer could not afford them and this
        // host was written against that world. Clustered forward lighting
        // raised the cap 64 -> 1024 and nothing had spent it.
        //
        //   --set r_citylights 1      -> NIGHT, every authored street lamped at
        //                                urban spacing (~240 lamps), plus a
        //                                pooled light per lit window band and
        //                                per neon sign.
        //   --set r_clusterlights 1   -> the froxel path that can actually
        //                                carry them (without it the frame still
        //                                truncates to the legacy 64).
        //   --set r_debugview 6       -> the froxel occupancy heatmap.
        // With neither flag this host renders EXACTLY what it always did.
        // r_citylights is a BOOT cvar: it decides GEOMETRY (which lamps get
        // built), not a device parameter, so it is read off the --set list here
        // rather than pushed through a setter. It still CLAIMS its name, so the
        // end-of-run audit does not report it as silently ignored.
        //
        // r_clusterlights / r_debugview used to be hand-read here too and pushed
        // with their own setClusterLights/setDebugView calls. They are device
        // params, so the dispatch wrapper now applies them through the ONE shared
        // mechanism (applyHostRenderCVars) and the latch keeps them — those hand
        // rolled lines are gone. r_clusterlights is still read for the receipt
        // line below, which reports the light budget it implies.
        bool cityDense = false, cityCluster = false;
        for (const auto& kv : hc.cliCVars) {
            if (kv.first == "r_citylights"    && kv.second != "0") cityDense   = true;
            if (kv.first == "r_clusterlights" && kv.second != "0") cityCluster = true;
        }
        if (cityDense) claimHostCVar("r_citylights");

        if (cityDense) {
            // NIGHT. The lamps are the subject, so the sun goes to a thin blue
            // moonlight and the ambient drops to a city-glow floor -- bright
            // enough that unlit massing is readable silhouette, dark enough that
            // a 16 m sodium pool actually reads as light.
            sp.sunDir[0] = -0.30f; sp.sunDir[1] = 0.62f; sp.sunDir[2] = -0.42f;
            sp.sunColor[0] = 0.40f; sp.sunColor[1] = 0.48f; sp.sunColor[2] = 0.74f;
            sp.sunIntensity = 0.10f;   // sky DISK/glow only
            sp.haze  = 0.20f;
            sp.exposure = 1.0f;
            // sunLight is the separate multiplier on mesh.frag's directional
            // key. sunIntensity alone leaves the whole world daylit -- that is
            // the trap: the sky looked dim and every facade stayed lit.
            sp.sunLight = 0.055f;      // thin moonlight, silhouette only
            sp.zenith[0]  = 0.010f; sp.zenith[1]  = 0.016f; sp.zenith[2]  = 0.040f;
            sp.horizon[0] = 0.055f; sp.horizon[1] = 0.055f; sp.horizon[2] = 0.090f;
            device->setSkyParams(sp);
            device->setAmbient(0.014f, 0.016f, 0.026f);   // city-glow floor
            device->setIblIntensity(0.10f);               // the sky IBL is what kept the grass green
            device->setExposure(1.10f);
        }

        x3::game::City city;
        std::vector<x3::game::StreetLights::Glow> cityGlows;
        city.build(cscene, *device, *cphys, nullptr, cityDense ? &cityGlows : nullptr);

        x3::game::StreetLights cityLamps;
        if (cityDense) {
            cityLamps.buildCityLamps(cscene, *device, /*dense*/true);
            cityLamps.adoptCityGlows(cityGlows);
            x3::logInfo("--screenshot-city: NIGHT + " + std::to_string(cityLamps.lampCount()) +
                        " city light sources (" + std::to_string(cityLamps.deadCount()) +
                        " dead, " + std::to_string(cityLamps.flickerCount()) + " flickering, " +
                        std::to_string(cityGlows.size()) + " window/sign glows); clustered=" +
                        (cityCluster ? "1" : "0"));
        }

        auto renderShot = [&](float cx, float cy, float cz, float yaw, float pitch,
                              const std::string& path) -> bool {
            const int kFrames = 90;    // settle: shadows + TAA history + bloom
            std::vector<x3::rhi::PointLight> lampPool;
            for (int i = 0; i < kFrames; ++i) {
                glfwPollEvents();
                cphys->step(1.0f / 60.0f);
                if (cityDense) {
                    // Feed the NEAREST lamps to the eye. The budget is the
                    // clustered scene cap when clustering is on, and the legacy
                    // 64 when it is not -- which is exactly the A/B this lane
                    // exists to make visible.
                    cityLamps.update(1.0f / 60.0f, cscene);
                    lampPool.clear();
                    const uint32_t k = cityCluster ? x3::rhi::kMaxSceneLights : 64u;
                    cityLamps.selectLights(cx, cy, cz, lampPool, k);
                    device->setPointLights(lampPool.data(), (uint32_t)lampPool.size());
                }
                device->setCamera(cx, cy, cz, yaw, pitch, 70.0f);
                if (i == kFrames - 1) device->armCapture(path.c_str());
                auto frame = device->beginFrame();
                if (frame.valid) cscene.render(*device, frame);
                device->endFrame(frame);
            }
            const bool ok = device->captureFrame(path.c_str());
            if (ok) {
                const auto st = device->stats();
                x3::logInfo("--screenshot-city: wrote " + path);
                x3::logInfo("[city-perf] " + path +
                            "  lights=" + std::to_string(lampPool.size()) +
                            "  tris=" + std::to_string(st.triangles) +
                            "  draws=" + std::to_string(st.drawCalls) +
                            "  gpu=" + std::to_string(st.gpuFrameMs) + " ms" +
                            "  clusterLights=" + std::to_string(st.clusterLights) +
                            "  clusterOverflow=" + std::to_string(st.clusterOverflows) +
                            "  maxFroxelLoad=" + std::to_string(st.clusterMaxLoad));
            }
            else    x3::logError("--screenshot-city: capture FAILED for " + path);
            return ok;
        };
        auto suffixed = [&](const char* tag) -> std::string {
            std::string p = cityShotPath;
            const size_t dot = p.find_last_of('.');
            return (dot == std::string::npos) ? p + tag : p.substr(0, dot) + tag + p.substr(dot);
        };

        // The sun sits in the +X+Z sky — shoot FROM the north-east looking
        // south-west so the camera sees lit faces, not the shadow sides.
        // Shot 1 — New District establishing: elevated NE vantage, the skyscraper
        // cluster + grid receding, ranges on the horizon.
        float g[3]; x3::game::placeOnTerrain(330.0f, 620.0f, g);
        const float eYaw = std::atan2(490.0f - 620.0f, 190.0f - 330.0f);
        const bool w1 = renderShot(330.0f, g[1] + 55.0f, 620.0f, eYaw, -0.20f, cityShotPath);

        // Shot 2 — street level on the District main street, looking west (lit
        // +X faces of the shops/towers toward the camera).
        float g2[3]; x3::game::placeOnTerrain(300.0f, 500.0f, g2);
        const bool w2 = renderShot(300.0f, g2[1] + 2.1f, 500.0f, 3.14159f, 0.02f, suffixed("_street"));

        // Shot 3 — the Scrapyard core from its NE corner: named roster + helipad
        // tower + water tower, lit sides toward the camera.
        float g3[3]; x3::game::placeOnTerrain(-480.0f, 580.0f, g3);
        const float sYaw = std::atan2(480.0f - 580.0f, -620.0f + 480.0f);
        const bool w3 = renderShot(-480.0f, g3[1] + 14.0f, 580.0f, sYaw, -0.08f, suffixed("_scrapyard"));

        cphys->shutdown();
        device->shutdown();
        glfwDestroyWindow(window);
        glfwTerminate();
        return (w1 && w2 && w3) ? 0 : 1;
    }

    // ---- AI-action capture mode (--capture-ai [outDir]) --------------------
    // Render the REAL monster combat-AI state machine "in action" to a numbered
    // PNG sequence + an animated GIF, entirely headless/offscreen (no window).
    //
    // Scene: a flat lit ground under the analytic sky, with the engine's sun plus a
    // ring of point lights so the characters read clearly (no dark silhouettes). A
    // fixed player REFERENCE (the AI's target) sits at the arena center; a small
    // squad of enemies runs the actual game AI update (the same MonsterSystem the
    // level uses) so their states + facing are genuine:
    //   * Guard   — advances toward + faces the player (Advance/Attack).
    //   * Drone   — ranged: holds standoff + circles the player (Strafe), facing it.
    //   * Wounded — a Guard we DAMAGE mid-capture (takeMeleeDamage) so it drops below
    //               the retreat threshold and turns away + backs off (Retreat).
    //   * Scout   — a Guard that has LOS, then we CUT its target mid-capture so it
    //               loses sight and walks to the last-known spot, scanning (Search).
    // A fixed 3/4 elevated camera frames all four + the player so advance/circle/
    // retreat read across the sequence. Steps at fixed dt for ~6 s, captures a frame
    // every ~0.2 s, then assembles the GIF + prints a per-phase state log.
    if (captureAi) {
        namespace fs = std::filesystem;
        x3::logInfo("--capture-ai: rendering monster combat-AI demo to " + captureAiDir);
        std::error_code mkec;
        fs::create_directories(captureAiDir, mkec);

        // ---- Physics + scene + lit ground ---------------------------------
        std::unique_ptr<x3::phys::IPhysicsWorld> cphys(x3::phys::createPhysicsWorld());
        cphys->init();
        // Flat collision ground at y=0 (CCW so +Y is solid), large enough for the
        // whole fight (so the LOS / move probes never run off the edge).
        {
            const float h = 80.0f;
            float gv[] = { -h,0,-h,  h,0,-h,  h,0,h,  -h,0,h };
            uint32_t gidx[] = { 0,2,1, 0,3,2 };
            cphys->addStaticMesh(gv, 4, gidx, 6);
        }
        x3::game::Scene cscene;

        // Visible lit ground plane (render): a neutral checker so the floor + the
        // characters' contact shadows-on-flat read. Drawn directly each frame.
        std::vector<x3::rhi::MeshVertex> gvtx; std::vector<uint32_t> gixs;
        x3::prims::makeGroundQuad(/*half=*/80.0f, /*tiles=*/40.0f, gvtx, gixs);
        x3::rhi::MeshHandle groundMesh = device->createMesh(
            gvtx.data(), (uint32_t)gvtx.size(), gixs.data(), (uint32_t)gixs.size());
        auto groundPx = x3::prims::makeCheckerRGBA(64, 8, 120, 130, 120, 64, 72, 66);
        x3::rhi::TextureHandle groundTex = device->createTexture(groundPx.data(), 64, 64, true);
        const float modelGround[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        const float whiteTint[4] = { 1, 1, 1, 1 };

        // A small bright marker box at the player reference so the "target" is
        // visibly in-frame (the AI target itself is a logical stub).
        x3::prims::PrimMesh markerM = x3::prims::makeBox(0.35f, 0.9f, 0.35f, 0, 0.9f, 0, 0.5f);
        x3::rhi::MeshHandle markerMesh = device->createMesh(
            markerM.verts.data(), (uint32_t)markerM.verts.size(),
            markerM.index.data(), (uint32_t)markerM.index.size());
        auto markerPx = x3::prims::makeSolidRGBA(4, 250, 235, 120);   // warm yellow pillar
        x3::rhi::TextureHandle markerTex = device->createTexture(markerPx.data(), 4, 4, true);

        // ---- Sky + lighting: the engine sun PLUS a ring of point lights so the
        // characters are well-lit from several sides (avoid the dim-corner problem;
        // they must NOT read as dark silhouettes). ----
        x3::rhi::IRenderDevice::SkyParams sp{};
        sp.enabled = true;
        sp.sunDir[0] = 0.4f; sp.sunDir[1] = 1.0f; sp.sunDir[2] = 0.3f;
        sp.sunColor[0] = 1.0f; sp.sunColor[1] = 0.97f; sp.sunColor[2] = 0.92f;
        sp.sunIntensity = 1.0f; sp.haze = 0.45f; sp.exposure = 1.0f;
        device->setSkyParams(sp);
        {
            // Bright fill lights placed above + around the action so every enemy is
            // lit regardless of which way it faces. color[] is linear RGB * intensity.
            x3::rhi::PointLight pl[5];
            auto setL = [](x3::rhi::PointLight& l, float x, float y, float z,
                           float r, float g, float b, float range) {
                l.pos[0]=x; l.pos[1]=y; l.pos[2]=z; l.range=range;
                l.color[0]=r; l.color[1]=g; l.color[2]=b;
            };
            setL(pl[0],   0.0f, 7.0f,  4.0f,  5.5f, 5.4f, 5.0f, 44.0f); // overhead key over the action
            setL(pl[1],   9.0f, 4.0f, 10.0f,  4.0f, 3.8f, 3.4f, 40.0f); // +X+Z warm fill
            setL(pl[2],  -9.0f, 4.0f, 10.0f,  3.4f, 3.8f, 4.4f, 40.0f); // -X+Z cool fill
            setL(pl[3],   9.0f, 4.0f, -6.0f,  3.8f, 3.6f, 3.4f, 40.0f); // +X-Z fill
            setL(pl[4],  -9.0f, 4.0f, -6.0f,  3.4f, 3.8f, 4.4f, 40.0f); // -X-Z fill
            device->setPointLights(pl, 5);
        }

        // ---- Player reference (the AI target) at the arena center. ----
        // A trivial always-alive damage sink at a fixed eye/foot, exactly like the
        // --test-ai stub: the enemies track + face IT.
        struct CapTarget final : public x3::game::IDamageSink {
            x3::phys::Vec3 eye{ 0.0f, 1.6f, 0.0f };
            bool takeDamage(int) override { return true; }
            x3::phys::Vec3 damageTargetPos() const override { return eye; }
            bool isAlive() const override { return true; }
        };
        CapTarget player;
        player.eye = x3::phys::Vec3{ 0.0f, 1.6f, 0.0f };
        const x3::phys::Vec3 playerFoot{ 0.0f, 0.0f, 0.0f };

        // ---- The squad. Each is a self-contained MonsterSystem so we can drive its
        // target / LOS independently. We use the rigged animated GLBs when present
        // (marcus_webb for guards, Drone for the flanker); on load failure each
        // falls back to a procedural box, so the capture never breaks. ----
        using x3::game::MonsterSystem;
        using x3::game::MonsterType;
        using x3::game::AiState;
        using x3::game::aiStateName;

        // Prefer the MULTI-CLIP "<name>_anim.glb" (Idle/Walk/Run/Jump) when present
        // so the captured enemies actually WALK/RUN as they move (T1 locomotion
        // blend); fall back to the Idle-only base GLB otherwise (clean checkout).
        auto pickAnimGlb = [](const std::string& dir, const char* base) -> std::string {
            namespace fs = std::filesystem;
            std::string b(base);
            std::string stem = (b.size() > 4 && b.substr(b.size()-4) == ".glb")
                ? b.substr(0, b.size()-4) : b;
            std::string anim = stem + "_anim.glb";
            std::error_code ec;
            if (fs::exists(fs::path(dir) / anim, ec)) return anim;
            return b;
        };
        auto guardTune = [&](){
            MonsterSystem::Tuning t;
            t.type = MonsterType::Guard;
            t.hp = 100; t.chaseSpeed = 3.2f;
            // Brighten the (dark-material) guard mesh so it reads clearly under the
            // arena lights — a warm steel-green so it isn't confused with the others.
            t.tint[0]=1.6f; t.tint[1]=1.7f; t.tint[2]=1.4f; t.tint[3]=1.0f;
            t.damage = 8; t.attackRange = 1.9f; t.attackCooldown = 1.0f; t.attackWindup = 0.25f;
            t.ranged = false;
            t.modelFile = pickAnimGlb(x3::game::riggedGlbRoot(), "marcus_webb.glb");
            t.modelDirOverride = x3::game::riggedGlbRoot();
            t.standUpZtoY = false; t.modelScale = 1.0f;
            return t;
        };
        auto droneTune = [](){
            MonsterSystem::Tuning t;
            t.type = MonsterType::Drone;
            t.hp = 66; t.chaseSpeed = 3.6f;
            t.tint[0]=1.0f; t.tint[1]=1.4f; t.tint[2]=2.0f; t.tint[3]=1.0f;   // bright pale-blue flanker
            t.damage = 5; t.attackRange = 14.0f; t.attackCooldown = 1.4f; t.attackWindup = 0.35f;
            t.ranged = true; t.standoff = 7.0f;
            t.modelFile = "Characters/Drone.glb"; t.modelDirOverride = x3::game::convertedGlbRoot();
            t.standUpZtoY = true; t.modelScale = 1.0f;
            return t;
        };
        auto scoutTune = [&](){
            MonsterSystem::Tuning t;
            t.type = MonsterType::Guard;
            t.hp = 100; t.chaseSpeed = 3.2f;
            t.tint[0]=2.0f; t.tint[1]=1.2f; t.tint[2]=2.2f; t.tint[3]=1.0f; // bright violet scout
            t.damage = 8; t.attackRange = 1.9f; t.attackCooldown = 1.0f; t.attackWindup = 0.25f;
            t.ranged = false;
            t.modelFile = pickAnimGlb(x3::game::riggedGlbRoot(), "marcus_webb.glb");
            t.modelDirOverride = x3::game::riggedGlbRoot();
            t.standUpZtoY = false; t.modelScale = 1.0f;
            return t;
        };
        auto woundedTune = [&](){
            MonsterSystem::Tuning t;
            t.type = MonsterType::Guard;
            t.hp = 100; t.chaseSpeed = 3.0f;
            t.tint[0]=2.2f; t.tint[1]=1.0f; t.tint[2]=0.8f; t.tint[3]=1.0f; // bright wounded red
            t.damage = 8; t.attackRange = 1.9f; t.attackCooldown = 1.0f; t.attackWindup = 0.25f;
            t.ranged = false;
            t.modelFile = pickAnimGlb(x3::game::riggedGlbRoot(), "marcus_webb.glb");
            t.modelDirOverride = x3::game::riggedGlbRoot();
            t.standUpZtoY = false; t.modelScale = 1.0f;
            return t;
        };

        const std::string modelDir = x3::game::riggedGlbRoot();
        MonsterSystem guard, drone, wounded, scout;
        // Place each on its OWN lane around the player so the four behaviours stay
        // spatially distinct (they don't all bunch on the target): the Guard starts
        // FAR so it visibly advances the whole clip; the Drone holds a side standoff
        // and circles; the Wounded sits mid-range then backs away; the Scout sits to
        // the far side and walks off to search once its LOS is cut.
        guard.buildMonsterTuned  (cscene, *device, *cphys, modelDir, x3::phys::Vec3{  -2.0f, 0.0f,  13.0f }, guardTune());
        drone.buildMonsterTuned  (cscene, *device, *cphys, modelDir, x3::phys::Vec3{   7.5f, 0.0f,   2.0f }, droneTune());
        wounded.buildMonsterTuned(cscene, *device, *cphys, modelDir, x3::phys::Vec3{   3.0f, 0.0f,   7.0f }, woundedTune());
        scout.buildMonsterTuned  (cscene, *device, *cphys, modelDir, x3::phys::Vec3{  -7.5f, 0.0f,   4.0f }, scoutTune());

        // ---- Fixed 3/4 elevated camera framing the player + all enemies. ----
        // A true 3/4 view: stand back + offset to +X so depth separates the figures,
        // elevated and looking down-across at the arena center, so advance (toward
        // center), strafe (circling), retreat (away) + search (walking off) all read.
        device->setCamera(11.0f, 8.0f, 15.0f, /*yaw=*/ -2.20f /* look toward -Z, angled -X */,
                          /*pitch=*/ -0.40f, 60.0f);

        // ---- Run the scripted fight. ~6 s at fixed dt; capture every ~0.2 s. ----
        const float dt          = 1.0f / 60.0f;
        const float captureEvery = 0.20f;        // seconds between captured frames
        const float duration     = 6.0f;         // total seconds simulated
        const float woundAtT     = 1.8f;         // damage the "wounded" guard here
        const float loseLosAtT   = 2.6f;         // cut the "scout" target here
        const int   totalSteps   = (int)(duration / dt + 0.5f);
        const int   stepsPerCap  = (int)(captureEvery / dt + 0.5f);

        int   frameNo = 0;
        bool  wounded_done = false;
        // Per-phase log lines (printed all together at the end as the "phase log").
        std::vector<std::string> phaseLog;
        std::vector<std::string> framePaths;

        x3::game::AttackFxFn noFx{};            // no tracer FX needed for the capture
        x3::game::BossPhaseFn noPhase{};
        x3::game::AllyQueryFn noAllies{};

        for (int step = 0; step <= totalSteps; ++step) {
            const float t = step * dt;
            glfwPollEvents();

            // Scripted events: wound one enemy into Retreat; cut another's LOS into
            // Search. Both exercise the REAL damage / targeting paths.
            if (!wounded_done && t >= woundAtT) {
                // Drop it well below the 30% retreat threshold (100 -> 22) so it
                // enters Retreat and turns away. Mirrors a lethal-ish melee combo.
                wounded.takeMeleeDamage(78, cscene, *cphys);
                wounded_done = true;
                x3::logInfo("[capture-ai] t=" + std::to_string(t) +
                            "s: wounded guard takes 78 dmg (hp now " +
                            std::to_string(wounded.hp()) + ")");
            }
            const bool scoutHasTarget = (t < loseLosAtT);   // cut LOS after this

            // Advance the AI for each enemy with the SAME update the game uses.
            // playerFoot is the planar tracking target; player.eye is the LOS end.
            guard.update  (dt, cscene, *cphys, playerFoot, player.eye, &player, noFx, noPhase, noAllies);
            drone.update  (dt, cscene, *cphys, playerFoot, player.eye, &player, noFx, noPhase, noAllies);
            wounded.update(dt, cscene, *cphys, playerFoot, player.eye, &player, noFx, noPhase, noAllies);
            // Scout: pass a live target until loseLosAtT, then null so it loses LOS
            // and falls to Search (it already saw the player, so it searches the
            // last-known spot rather than going Idle immediately).
            if (scoutHasTarget)
                scout.update(dt, cscene, *cphys, playerFoot, player.eye, &player, noFx, noPhase, noAllies);
            else
                scout.update(dt, cscene, *cphys, playerFoot, player.eye, nullptr, noFx, noPhase, noAllies);

            cphys->step(dt);
            // Sync entity translations from physics, THEN re-bake AI facing: the
            // monster update already composed each transform this frame, but a
            // scene.update() would overwrite the 3x3 with identity-rot; the game
            // order is update()-after-scene.update(). Here we don't call
            // scene.update() at all (the AI fully owns each enemy transform), so the
            // baked facing stands.

            // Capture a frame on the cadence (and always the final step).
            const bool doCap = (step % stepsPerCap == 0) || (step == totalSteps);
            char fpath[512];
            if (doCap) {
                std::snprintf(fpath, sizeof(fpath), "%s/frame_%03d.png",
                              captureAiDir.c_str(), frameNo);
                device->armCapture(fpath);
            }
            auto frame = device->beginFrame();
            if (frame.valid) {
                // Lit ground + the player marker pillar.
                device->drawMesh(frame, groundMesh, groundTex, whiteTint, modelGround);
                const float modelMarker[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0,
                                                player.eye.x, 0.0f, player.eye.z, 1 };
                device->drawMesh(frame, markerMesh, markerTex, whiteTint, modelMarker);
                // Scene entities (the monster Entities carry invalid render meshes,
                // so this is mostly a no-op for them) + each monster's own model.
                cscene.render(*device, frame);
                guard.drawMonster(*device, frame, cscene);
                drone.drawMonster(*device, frame, cscene);
                wounded.drawMonster(*device, frame, cscene);
                scout.drawMonster(*device, frame, cscene);
            }
            device->endFrame(frame);

            if (doCap) {
                const bool wrote = device->captureFrame(fpath);
                if (wrote) framePaths.emplace_back(fpath);
                // Record the per-phase state line at this capture instant.
                char line[256];
                std::snprintf(line, sizeof(line),
                    "t=%4.1f  guard=%-7s drone=%-7s wounded=%-7s(hp=%d) scout=%-7s",
                    t, aiStateName(guard.aiState()), aiStateName(drone.aiState()),
                    aiStateName(wounded.aiState()), wounded.hp(),
                    aiStateName(scout.aiState()));
                phaseLog.emplace_back(line);
                ++frameNo;
            }
        }

        // ---- Assemble the animated GIF from the captured PNG frames. -----------
        // Read each PNG back (stb_image, already in the engine TU) and feed it to the
        // public-domain gif.h encoder at ~10 fps (delay = 10 hundredths/frame), looping.
        bool gifOk = false;
        const std::string gifPath = "G:/X3Native/ai_action.gif";
        if (!framePaths.empty()) {
            int gw = 0, gh = 0, gc = 0;
            unsigned char* first = stbi_load(framePaths.front().c_str(), &gw, &gh, &gc, 4);
            if (first && gw > 0 && gh > 0) {
                GifWriter gif{};
                const uint32_t delayCs = 10;   // 10/100 s per frame => ~10 fps, looping
                if (GifBegin(&gif, gifPath.c_str(), (uint32_t)gw, (uint32_t)gh, delayCs)) {
                    GifWriteFrame(&gif, first, (uint32_t)gw, (uint32_t)gh, delayCs);
                    for (size_t i = 1; i < framePaths.size(); ++i) {
                        int w = 0, h = 0, c = 0;
                        unsigned char* px = stbi_load(framePaths[i].c_str(), &w, &h, &c, 4);
                        if (px && w == gw && h == gh) {
                            GifWriteFrame(&gif, px, (uint32_t)gw, (uint32_t)gh, delayCs);
                            stbi_image_free(px);
                        } else if (px) {
                            stbi_image_free(px);
                        }
                    }
                    gifOk = GifEnd(&gif);
                }
                stbi_image_free(first);
            }
        }

        // ---- Print the per-phase state log so the behaviours can be described. ---
        x3::logInfo("================ --capture-ai per-phase AI state log ================");
        for (const auto& l : phaseLog) x3::logInfo(l);
        x3::logInfo("====================================================================");
        {
            char sb[256];
            std::error_code szec;
            uintmax_t gifBytes = gifOk ? fs::file_size(gifPath, szec) : 0;
            std::snprintf(sb, sizeof(sb),
                "--capture-ai: wrote %d PNG frames to %s | GIF %s (%llu bytes)",
                frameNo, captureAiDir.c_str(),
                gifOk ? gifPath.c_str() : "(FAILED)",
                (unsigned long long)gifBytes);
            x3::logInfo(sb);
        }

        device->destroyMesh(groundMesh);
        device->destroyTexture(groundTex);
        device->destroyMesh(markerMesh);
        device->destroyTexture(markerTex);
        cphys->shutdown();
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return (frameNo > 0 && gifOk) ? 0 : 1;
    }

    // ---- Factory bore-ride capture (--capture-factory [out.gif]) -----------
    // THE GLASS-CURTAIN MONEY SHOT (feat/factory-annex T6): the camera rides
    // INSIDE the Anywhere Elevator's cab down the lateral bore leg — brass ribs
    // whipping past, then the annex glass curtain + the lit wonder-room floors
    // sliding by as the cab crosses into the Annex. The ride is ARMED IN CODE
    // (unlockHidden + callTo the annex bore stop — no keypad walk needed).
    // 60 frames -> a 640x360 @ 20 fps looping GIF (headless renders at the
    // fixed 1280x720; each frame is 2x box-downsampled for the GIF). Modeled
    // on --capture-ai above (the SOLE gif.h TU).
    if (captureFactory) {
        namespace fs = std::filesystem;
        const std::string gifPath = captureFactoryPath;
        std::string framesDir = gifPath;
        {
            const size_t dot = framesDir.find_last_of('.');
            if (dot != std::string::npos) framesDir.resize(dot);
            framesDir += "_frames";
        }
        x3::logInfo("--capture-factory: riding the annex bore to " + gifPath);
        std::error_code mkec;
        fs::create_directories(framesDir, mkec);

        std::unique_ptr<x3::phys::IPhysicsWorld> fcphys(x3::phys::createPhysicsWorld());
        fcphys->init();
        x3::game::Scene fcscene;
        x3::game::TriggerSystem fctrig;
        x3::game::FactoryAnnex fcannex;
        fcannex.build(fcscene, *device, *fcphys, fctrig, /*shaftX*/0.0f, /*shaftZ*/0.0f);
        fcannex.applyAtmosphere(*device);

        x3::game::ElevatorSystem fcelev;
        const x3::game::FactoryAnnex::ElevatorGraph g =
            x3::game::FactoryAnnex::makeElevatorGraph(0.0f, 0.0f);
        // Start the cab AT F3 (the bore level) so the whole capture is the
        // lateral leg. NO buildVisuals: the cab's sealed door panels would fill
        // the forward view — the capture is the ride, the bore and the curtain.
        fcelev.buildEx(fcscene, *device, *fcphys,
                       x3::game::FactoryAnnex::kCabHalfX,
                       x3::game::FactoryAnnex::kCabHalfY,
                       x3::game::FactoryAnnex::kCabHalfZ,
                       g.stops, g.rails, /*startStop*/g.f3);
        fcelev.enableFsm(true);
        fcelev.setFloorLabels(g.labels);
        // ARM THE RIDE: the annex rail opens in code and the cab departs for
        // A2 — the F3 -> A2 leg IS the bore transit.
        fcelev.unlockHidden();
        fcelev.callTo(g.a2);

        // Let the annex glow settle a beat before frame 0.
        const float sdt = 1.0f / 60.0f;
        for (int i = 0; i < 12; ++i) {
            fcannex.tick(sdt, fcscene);
            fcelev.update(sdt, fcscene, *fcphys);
            fcphys->step(sdt);
        }

        const int   kFrames      = 60;    // plan: 60 frames @ 20 fps
        const int   kStepsPerCap = 10;    // 1/6 s sim per frame: doors + the full
                                          // 60 m leg + arrival fit the 60 frames
        float fcWaterClock = 0.0f;        // the confection river (T7): host-pushed
        int   fcFrameNo = 0;
        std::vector<std::string> fcFramePaths;
        for (int f = 0; f < kFrames; ++f) {
            glfwPollEvents();
            for (int s = 0; s < kStepsPerCap; ++s) {
                fcannex.tick(sdt, fcscene);
                fcelev.update(sdt, fcscene, *fcphys);
                fcphys->step(sdt);
            }
            {   // ONE merged light push: annex rig + elevator rig (host law).
                std::vector<x3::rhi::PointLight> lp(fcannex.pointLights());
                lp.insert(lp.end(), fcelev.pointLights().begin(),
                          fcelev.pointLights().end());
                device->setPointLights(lp.data(), (uint32_t)lp.size());
            }
            {   // The confection river (T7) — host-advanced water clock.
                fcWaterClock += sdt * (float)kStepsPerCap;
                x3::rhi::IRenderDevice::WaterParams wp = fcannex.riverWater();
                wp.time = fcWaterClock;
                device->setWaterParams(wp);
            }
            // Rider's eye INSIDE the cab, looking down the direction of travel
            // (+X — device forward at yaw 0): ribs, then the glass curtain.
            const x3::phys::Vec3 c = fcelev.cabCenter();
            device->setCamera(c.x + 0.3f, c.y + 1.35f, c.z,
                              /*yaw*/0.0f, /*pitch*/-0.06f, 68.0f);
            char fpath[512];
            std::snprintf(fpath, sizeof(fpath), "%s/frame_%03d.png",
                          framesDir.c_str(), fcFrameNo);
            device->armCapture(fpath);
            auto frame = device->beginFrame();
            if (frame.valid) fcscene.render(*device, frame);
            device->endFrame(frame);
            if (device->captureFrame(fpath)) fcFramePaths.emplace_back(fpath);
            ++fcFrameNo;
        }

        // ---- Assemble the 640x360 @ 20 fps GIF (2x box-downsample per frame).
        bool fcGifOk = false;
        if (!fcFramePaths.empty()) {
            GifWriter gif{};
            const uint32_t delayCs = 5;   // 5/100 s per frame == 20 fps, looping
            uint32_t gw = 0, gh = 0;
            std::vector<unsigned char> small;
            for (size_t i = 0; i < fcFramePaths.size(); ++i) {
                int w = 0, h = 0, ch4 = 0;
                unsigned char* px = stbi_load(fcFramePaths[i].c_str(), &w, &h, &ch4, 4);
                if (!px) continue;
                const uint32_t dw = (uint32_t)w / 2u, dh = (uint32_t)h / 2u;
                if (i == 0) {
                    gw = dw; gh = dh;
                    small.resize((size_t)gw * gh * 4u);
                    if (!GifBegin(&gif, gifPath.c_str(), gw, gh, delayCs)) {
                        stbi_image_free(px);
                        break;
                    }
                }
                if (dw == gw && dh == gh) {
                    for (uint32_t y = 0; y < gh; ++y)
                        for (uint32_t x = 0; x < gw; ++x)
                            for (uint32_t k = 0; k < 4; ++k) {
                                const uint32_t s00 = ((2*y    ) * (uint32_t)w + 2*x    ) * 4u + k;
                                const uint32_t s01 = ((2*y    ) * (uint32_t)w + 2*x + 1) * 4u + k;
                                const uint32_t s10 = ((2*y + 1) * (uint32_t)w + 2*x    ) * 4u + k;
                                const uint32_t s11 = ((2*y + 1) * (uint32_t)w + 2*x + 1) * 4u + k;
                                small[((size_t)y * gw + x) * 4u + k] = (unsigned char)
                                    (((uint32_t)px[s00] + px[s01] + px[s10] + px[s11]) / 4u);
                            }
                    GifWriteFrame(&gif, small.data(), gw, gh, delayCs);
                }
                stbi_image_free(px);
                if (i + 1 == fcFramePaths.size()) fcGifOk = GifEnd(&gif);
            }
        }
        {
            char sb[320];
            std::error_code szec;
            const uintmax_t gifBytes = fcGifOk ? fs::file_size(gifPath, szec) : 0;
            std::snprintf(sb, sizeof(sb),
                "--capture-factory: %d frames -> %s | GIF %s (%llu bytes) | cab ended at x=%.1f",
                fcFrameNo, framesDir.c_str(),
                fcGifOk ? gifPath.c_str() : "(FAILED)",
                (unsigned long long)gifBytes, fcelev.cabCenter().x);
            x3::logInfo(sb);
        }

        fcannex.shutdown(*device);
        fcphys->shutdown();
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return (fcFrameNo > 0 && fcGifOk) ? 0 : 1;
    }

    // ---- Crowd-spread capture (--capture-crowd-spread [outDir]) ------------
    // The ANTI-CROWDING proof (Tim: "characters must NEVER occupy the same space,
    // nor DESIRE to"). Spawn a CLUSTER of guards stacked on nearly one point beside a
    // player reference, then step them through the REAL MonsterManager::update (which
    // wires the separation STEERING and the hard DE-OVERLAP pass). Capture a top-down
    // sequence: the same run is its own before/after — t=0 is a stacked pile, and by
    // ~t=2.5 s the squad has fanned into a clean ring/arc with no two bodies clipping.
    // A per-frame MIN pairwise-distance readout quantifies the spread honestly.
    if (captureCrowdSpread) {
        namespace fs = std::filesystem;
        x3::logInfo("--capture-crowd-spread: rendering the anti-crowding proof to " +
                    captureCrowdSpreadDir);
        std::error_code mkec;
        fs::create_directories(captureCrowdSpreadDir, mkec);

        std::unique_ptr<x3::phys::IPhysicsWorld> cphys(x3::phys::createPhysicsWorld());
        cphys->init();
        {
            const float h = 80.0f;
            float gv[] = { -h,0,-h,  h,0,-h,  h,0,h,  -h,0,h };
            uint32_t gidx[] = { 0,2,1, 0,3,2 };
            cphys->addStaticMesh(gv, 4, gidx, 6);
        }
        x3::game::Scene cscene;

        std::vector<x3::rhi::MeshVertex> gvtx; std::vector<uint32_t> gixs;
        x3::prims::makeGroundQuad(/*half=*/80.0f, /*tiles=*/40.0f, gvtx, gixs);
        x3::rhi::MeshHandle groundMesh = device->createMesh(
            gvtx.data(), (uint32_t)gvtx.size(), gixs.data(), (uint32_t)gixs.size());
        auto groundPx = x3::prims::makeCheckerRGBA(64, 8, 120, 130, 120, 64, 72, 66);
        x3::rhi::TextureHandle groundTex = device->createTexture(groundPx.data(), 64, 64, true);
        const float modelGround[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        const float whiteTint[4] = { 1, 1, 1, 1 };

        x3::prims::PrimMesh markerM = x3::prims::makeBox(0.35f, 0.9f, 0.35f, 0, 0.9f, 0, 0.5f);
        x3::rhi::MeshHandle markerMesh = device->createMesh(
            markerM.verts.data(), (uint32_t)markerM.verts.size(),
            markerM.index.data(), (uint32_t)markerM.index.size());
        auto markerPx = x3::prims::makeSolidRGBA(4, 250, 235, 120);
        x3::rhi::TextureHandle markerTex = device->createTexture(markerPx.data(), 4, 4, true);

        x3::rhi::IRenderDevice::SkyParams sp{};
        sp.enabled = true;
        sp.sunDir[0] = 0.4f; sp.sunDir[1] = 1.0f; sp.sunDir[2] = 0.3f;
        sp.sunColor[0] = 1.0f; sp.sunColor[1] = 0.97f; sp.sunColor[2] = 0.92f;
        sp.sunIntensity = 1.0f; sp.haze = 0.45f; sp.exposure = 1.0f;
        device->setSkyParams(sp);
        {
            x3::rhi::PointLight pl[5];
            auto setL = [](x3::rhi::PointLight& l, float x, float y, float z,
                           float r, float g, float b, float range) {
                l.pos[0]=x; l.pos[1]=y; l.pos[2]=z; l.range=range;
                l.color[0]=r; l.color[1]=g; l.color[2]=b;
            };
            setL(pl[0],   0.0f, 10.0f,  0.0f,  6.0f, 5.9f, 5.5f, 60.0f); // overhead key
            setL(pl[1],   9.0f, 5.0f,  9.0f,  3.6f, 3.5f, 3.2f, 44.0f);
            setL(pl[2],  -9.0f, 5.0f,  9.0f,  3.2f, 3.5f, 4.0f, 44.0f);
            setL(pl[3],   9.0f, 5.0f, -9.0f,  3.5f, 3.4f, 3.2f, 44.0f);
            setL(pl[4],  -9.0f, 5.0f, -9.0f,  3.2f, 3.5f, 4.0f, 44.0f);
            device->setPointLights(pl, 5);
        }

        struct CapTarget final : public x3::game::IDamageSink {
            x3::phys::Vec3 eye{ 0.0f, 1.6f, 0.0f };
            bool takeDamage(int) override { return true; }
            x3::phys::Vec3 damageTargetPos() const override { return eye; }
            bool isAlive() const override { return true; }
        };
        CapTarget player;
        const x3::phys::Vec3 playerFoot{ 0.0f, 0.0f, 0.0f };

        // A squad of guards spawned in a TIGHT pile ~4 m from the player — deep body
        // overlap at t=0 (the worst-case "everyone on one tile"). They all chase the
        // same player, so separation + de-overlap must fan them into a ring/arc.
        auto pickAnimGlb = [](const std::string& dir, const char* base) -> std::string {
            namespace fs2 = std::filesystem;
            std::string b(base);
            std::string stem = (b.size() > 4 && b.substr(b.size()-4) == ".glb")
                ? b.substr(0, b.size()-4) : b;
            std::string anim = stem + "_anim.glb";
            std::error_code ec;
            if (fs2::exists(fs2::path(dir) / anim, ec)) return anim;
            return b;
        };
        const std::string modelDir = x3::game::riggedGlbRoot();
        x3::game::MonsterManager squad;
        const int kSquad = 8;
        for (int i = 0; i < kSquad; ++i) {
            x3::game::MonsterSystem::Tuning t;
            t.type = x3::game::MonsterType::Guard;
            t.hp = 100; t.chaseSpeed = 3.2f;
            // Alternate two tints so overlapping/adjacent bodies are visually separable.
            if (i & 1) { t.tint[0]=1.7f; t.tint[1]=1.5f; t.tint[2]=1.2f; }
            else       { t.tint[0]=1.2f; t.tint[1]=1.6f; t.tint[2]=2.0f; }
            t.tint[3]=1.0f;
            t.damage = 8; t.attackRange = 1.9f; t.attackCooldown = 1.0f; t.attackWindup = 0.25f;
            t.ranged = false;
            t.modelFile = pickAnimGlb(modelDir, "marcus_webb.glb");
            t.modelDirOverride = modelDir;
            t.standUpZtoY = false; t.modelScale = 1.0f;
            // A ~0.3 m jittered pile centered 4 m in +Z (deterministic offsets).
            const float ox = std::cos((float)i * 2.399963f) * 0.28f;
            const float oz = std::sin((float)i * 2.399963f) * 0.28f;
            squad.spawn(cscene, *device, *cphys, modelDir,
                        x3::phys::Vec3{ ox, 0.0f, 4.0f + oz }, t);
        }

        // TOP-DOWN-ish camera straight over the action so the ring/arc reads clearly.
        device->setCamera(0.0f, 15.0f, 6.5f, /*yaw=*/ -1.5708f, /*pitch=*/ -1.06f, 55.0f);

        auto minPairDist = [&]() -> float {
            float best = 1e30f;
            for (uint32_t i = 0; i < squad.count(); ++i) {
                if (!squad.at(i).alive()) continue;
                for (uint32_t j = i + 1; j < squad.count(); ++j) {
                    if (!squad.at(j).alive()) continue;
                    const x3::phys::Vec3 a = squad.at(i).pos(), b = squad.at(j).pos();
                    const float dx = a.x - b.x, dz = a.z - b.z;
                    const float d = std::sqrt(dx*dx + dz*dz);
                    if (d < best) best = d;
                }
            }
            return best;
        };

        const float dt = 1.0f / 60.0f;
        // Capture instants (s) -> filename tag. The first is the stacked "before".
        struct Cap { float t; const char* tag; };
        const Cap caps[] = {
            { 0.00f, "t00_stacked" }, { 0.40f, "t04" }, { 0.80f, "t08" },
            { 1.60f, "t16" }, { 3.00f, "t30_ring" },
        };
        const int nCaps = (int)(sizeof(caps) / sizeof(caps[0]));
        const float duration = 3.05f;
        const int totalSteps = (int)(duration / dt + 0.5f);
        x3::game::AttackFxFn noFx{};
        int nextCap = 0;
        int wrote = 0;

        auto renderAndMaybeCapture = [&](float t) {
            const bool doCap = (nextCap < nCaps) && (t + 1e-4f >= caps[nextCap].t);
            char fpath[512];
            if (doCap) {
                std::snprintf(fpath, sizeof(fpath), "%s/crowd_spread_%s.png",
                              captureCrowdSpreadDir.c_str(), caps[nextCap].tag);
                device->armCapture(fpath);
            }
            auto frame = device->beginFrame();
            if (frame.valid) {
                device->drawMesh(frame, groundMesh, groundTex, whiteTint, modelGround);
                const float modelMarker[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0,
                                                player.eye.x, 0.0f, player.eye.z, 1 };
                device->drawMesh(frame, markerMesh, markerTex, whiteTint, modelMarker);
                cscene.render(*device, frame);
                squad.drawAll(*device, frame, cscene);
            }
            device->endFrame(frame);
            if (doCap) {
                if (device->captureFrame(fpath)) { ++wrote; }
                char line[192];
                std::snprintf(line, sizeof(line),
                    "[crowd-spread] t=%4.2f  minPairDist=%.3f m  -> %s",
                    t, minPairDist(), fpath);
                x3::logInfo(line);
                ++nextCap;
            }
        };

        for (int step = 0; step <= totalSteps; ++step) {
            const float t = step * dt;
            glfwPollEvents();
            renderAndMaybeCapture(t);
            // Advance the squad with the REAL manager update (separation + de-overlap).
            squad.update(dt, cscene, *cphys, playerFoot, &player, noFx);
            cphys->step(dt);
        }

        x3::logInfo("--capture-crowd-spread: wrote " + std::to_string(wrote) +
                    " frames to " + captureCrowdSpreadDir);

        device->destroyMesh(groundMesh);
        device->destroyTexture(groundTex);
        device->destroyMesh(markerMesh);
        device->destroyTexture(markerTex);
        squad.shutdown();
        cphys->shutdown();
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return (wrote > 0) ? 0 : 1;
    }

    // ---- Walk-pose capture (--capture-walk [outPath]) ----------------------
    // A focused single-frame proof of the T1 locomotion blend: build ONE close-up
    // animated guard, drive the locomotion blend to a steady WALK speed, settle a
    // fraction of a second so the legs reach a clear mid-stride, capture a PNG.
    // Headless / offscreen, like --screenshot. Uses the multi-clip "*_anim.glb"
    // when present (Walk clip); on a clean checkout (asset absent) the base GLB
    // plays Idle and the capture still succeeds (it just shows the idle pose).
    if (captureWalk) {
        namespace fs = std::filesystem;
        x3::logInfo("--capture-walk: rendering a walking guard to " + captureWalkPath);
        {
            fs::path outp(captureWalkPath);
            std::error_code mkec;
            if (outp.has_parent_path()) fs::create_directories(outp.parent_path(), mkec);
        }

        std::unique_ptr<x3::phys::IPhysicsWorld> wphys(x3::phys::createPhysicsWorld());
        wphys->init();
        {
            const float h = 40.0f;
            float gv[] = { -h,0,-h,  h,0,-h,  h,0,h,  -h,0,h };
            uint32_t gidx[] = { 0,2,1, 0,3,2 };
            wphys->addStaticMesh(gv, 4, gidx, 6);
        }
        x3::game::Scene wscene;

        std::vector<x3::rhi::MeshVertex> gvtx; std::vector<uint32_t> gixs;
        x3::prims::makeGroundQuad(/*half=*/40.0f, /*tiles=*/20.0f, gvtx, gixs);
        x3::rhi::MeshHandle groundMesh = device->createMesh(
            gvtx.data(), (uint32_t)gvtx.size(), gixs.data(), (uint32_t)gixs.size());
        auto groundPx = x3::prims::makeCheckerRGBA(64, 8, 120, 130, 120, 64, 72, 66);
        x3::rhi::TextureHandle groundTex = device->createTexture(groundPx.data(), 64, 64, true);
        const float modelGround[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        const float whiteTint[4] = { 1, 1, 1, 1 };

        x3::rhi::IRenderDevice::SkyParams sp{};
        sp.enabled = true;
        sp.sunDir[0] = 0.4f; sp.sunDir[1] = 1.0f; sp.sunDir[2] = 0.3f;
        sp.sunColor[0] = 1.0f; sp.sunColor[1] = 0.97f; sp.sunColor[2] = 0.92f;
        sp.sunIntensity = 1.0f; sp.haze = 0.40f; sp.exposure = 1.0f;
        device->setSkyParams(sp);
        {
            x3::rhi::PointLight pl[3];
            auto setL = [](x3::rhi::PointLight& l, float x, float y, float z,
                           float r, float g, float b, float range) {
                l.pos[0]=x; l.pos[1]=y; l.pos[2]=z; l.range=range;
                l.color[0]=r; l.color[1]=g; l.color[2]=b;
            };
            setL(pl[0],  0.0f, 4.0f,  3.0f,  5.5f, 5.4f, 5.0f, 30.0f);  // key in front
            setL(pl[1],  3.0f, 3.0f, -2.0f,  3.0f, 3.2f, 3.6f, 30.0f);  // back-right rim
            setL(pl[2], -3.0f, 3.0f,  1.0f,  3.0f, 3.4f, 3.0f, 30.0f);  // left fill
            device->setPointLights(pl, 3);
        }

        // The target the guard advances toward: straight ahead in -Z so it walks
        // toward the camera-facing direction and the stride reads in profile.
        struct WalkTarget final : public x3::game::IDamageSink {
            x3::phys::Vec3 eye{ 0.0f, 1.6f, -12.0f };
            bool takeDamage(int) override { return true; }
            x3::phys::Vec3 damageTargetPos() const override { return eye; }
            bool isAlive() const override { return true; }
        };
        WalkTarget tgt;

        using x3::game::MonsterSystem;
        using x3::game::MonsterType;
        // Prefer the multi-clip animated GLB so the locomotion blend lights up.
        auto pickAnimGlb = [](const std::string& dir, const char* base) -> std::string {
            namespace fsx = std::filesystem;
            std::string b(base);
            std::string stem = (b.size() > 4 && b.substr(b.size()-4) == ".glb")
                ? b.substr(0, b.size()-4) : b;
            std::string anim = stem + "_anim.glb";
            std::error_code ec;
            if (fsx::exists(fsx::path(dir) / anim, ec)) return anim;
            return b;
        };
        MonsterSystem::Tuning wt;
        wt.type = MonsterType::Guard;
        wt.hp = 100; wt.chaseSpeed = 1.5f;   // ~walk speed: the blend lands on WALK
        wt.tint[0]=1.6f; wt.tint[1]=1.7f; wt.tint[2]=1.5f; wt.tint[3]=1.0f;
        wt.damage = 0; wt.attackRange = 0.5f; wt.attackWindup = 0.0f; wt.ranged = false;
        // Rig selectable via the 2nd --capture-walk arg (grounded-walk QA on any
        // character; default stays the marcus guard).
        const std::string walkRig = hc.captureWalkRig.empty() ? "marcus_webb.glb"
                                                              : hc.captureWalkRig;
        wt.modelFile = pickAnimGlb(x3::game::riggedGlbRoot(), walkRig.c_str());
        wt.modelDirOverride = x3::game::riggedGlbRoot();
        wt.standUpZtoY = false; wt.modelScale = 1.0f;

        MonsterSystem guard;
        // Start the guard a little back so it advances (walks) toward the target the
        // whole time, never reaching attack range (damage 0, tiny attackRange). It
        // ends near z~2.8 after the settle below; the camera frames that spot.
        const x3::phys::Vec3 guardStart{ 0.0f, 0.0f, 4.0f };
        guard.buildMonsterTuned(wscene, *device, *wphys, x3::game::riggedGlbRoot(),
                                guardStart, wt);
        x3::logInfo(std::string("--capture-walk: guard usingRealModel=") +
                    (guard.usingRealModel() ? "1" : "0"));

        // Close, slightly-elevated front-3/4 camera AIMED at the guard's expected
        // mid-capture torso (~(0, 0.9, 2.8)). Camera at (3, 1.6, 5.2) looking back
        // toward -X/-Z: yaw = atan2(dz,dx) of (look - cam), pitch from the rise.
        {
            const float cx = 3.0f, cy = 1.6f, cz = 5.4f;
            const float lx = 0.0f, ly = 0.95f, lz = 2.8f;
            const float ddx = lx - cx, ddy = ly - cy, ddz = lz - cz;
            const float dlen = std::sqrt(ddx*ddx + ddy*ddy + ddz*ddz);
            const float yaw = std::atan2(ddz, ddx);
            const float pitch = std::asin(dlen > 1e-4f ? (ddy / dlen) : 0.0f);
            device->setCamera(cx, cy, cz, yaw, pitch, 50.0f);
        }

        // Step ~1.5 s so the guard accelerates into a steady WALK and the legs reach
        // a clear mid-stride; capture that frame PLUS a second one 0.5 s later
        // (<out>_t2.png) so vertical root sink/bounce across clip times is
        // eye-checkable (the grounded-anim QA rule: floor in frame, two times).
        const float dt = 1.0f / 60.0f;
        x3::game::AttackFxFn noFx{}; x3::game::BossPhaseFn noPhase{}; x3::game::AllyQueryFn noAllies{};
        const int stepsA = 90, stepsB = 120;    // 0.5 s apart
        std::string pathB = captureWalkPath;
        {
            const size_t dot = pathB.rfind('.');
            if (dot != std::string::npos) pathB.insert(dot, "_t2");
            else pathB += "_t2.png";
        }
        bool wrote = false, wroteB = false;
        for (int step = 0; step <= stepsB; ++step) {
            glfwPollEvents();
            guard.update(dt, wscene, *wphys, tgt.eye /*planar*/, tgt.eye /*eye*/,
                         &tgt, noFx, noPhase, noAllies);
            wphys->step(dt);
            const bool capA = (step == stepsA), capB = (step == stepsB);
            if (capA) device->armCapture(captureWalkPath.c_str());
            if (capB) device->armCapture(pathB.c_str());
            auto frame = device->beginFrame();
            if (frame.valid) {
                device->drawMesh(frame, groundMesh, groundTex, whiteTint, modelGround);
                wscene.render(*device, frame);
                guard.drawMonster(*device, frame, wscene);
            }
            device->endFrame(frame);
            if (capA) wrote  = device->captureFrame(captureWalkPath.c_str());
            if (capB) wroteB = device->captureFrame(pathB.c_str());
        }
        x3::logInfo(std::string("--capture-walk: rig=") + walkRig +
                    " aiState=" + x3::game::aiStateName(guard.aiState()) +
                    (wrote ? "  wrote " + captureWalkPath : "  CAPTURE FAILED") +
                    (wroteB ? " + " + pathB : " (t2 FAILED)"));

        device->destroyMesh(groundMesh);
        device->destroyTexture(groundTex);
        wphys->shutdown();
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return wrote ? 0 : 1;
    }

    // ---- Foot-IK capture (--screenshot-footik [outPath]) -------------------
    // Stand ONE rigged character on a SLOPE + STEP with foot-IK ON: the feet
    // raycast down into the local physics world, plant on the surface (+ align to
    // the ground normal), the pelvis lowers so both feet reach, and we capture a
    // single PNG. A side-by-side reference (IK OFF) is also written so the planted
    // vs floating difference is obvious. Self-contained (no game stack): loads the
    // rigged GLB directly + drives a Skinner with the new setFootIk() hook.
    if (captureFootIk) {
        namespace fs = std::filesystem;
        x3::logInfo("--screenshot-footik: grounding a character on a slope/step -> " + captureFootIkPath);
        { fs::path outp(captureFootIkPath); std::error_code mkec;
          if (outp.has_parent_path()) fs::create_directories(outp.parent_path(), mkec); }

        std::unique_ptr<x3::phys::IPhysicsWorld> fphys(x3::phys::createPhysicsWorld());
        fphys->init();

        // Build a SLOPED + STEPPED ground as a static collision mesh AND a matching
        // render mesh, so the raycast hits exactly what we see. A gentle ~12deg ramp
        // descending toward -X with a small raised step block under one foot.
        std::vector<x3::rhi::MeshVertex> gv; std::vector<uint32_t> gi;
        auto pushQuad = [&](float a[3], float b[3], float c[3], float d[3]) {
            uint32_t base = (uint32_t)gv.size();
            float e1[3] = { b[0]-a[0], b[1]-a[1], b[2]-a[2] };
            float e2[3] = { c[0]-a[0], c[1]-a[1], c[2]-a[2] };
            float nx = e1[1]*e2[2]-e1[2]*e2[1], ny = e1[2]*e2[0]-e1[0]*e2[2], nz = e1[0]*e2[1]-e1[1]*e2[0];
            float nl = std::sqrt(nx*nx+ny*ny+nz*nz); if (nl>1e-6f){nx/=nl;ny/=nl;nz/=nl;}
            auto add = [&](float* p, float u, float v){ x3::rhi::MeshVertex mv{};
                mv.pos[0]=p[0];mv.pos[1]=p[1];mv.pos[2]=p[2]; mv.normal[0]=nx;mv.normal[1]=ny;mv.normal[2]=nz;
                mv.uv[0]=u;mv.uv[1]=v; gv.push_back(mv); };
            add(a,0,0); add(b,1,0); add(c,1,1); add(d,0,1);
            gi.push_back(base+0); gi.push_back(base+1); gi.push_back(base+2);
            gi.push_back(base+0); gi.push_back(base+2); gi.push_back(base+3);
        };
        // A SLOPE running along the Z axis (descends toward -Z, the camera side) so the
        // character — facing the camera with feet spread in Z — has its front foot on
        // lower ground than its back foot. slope ~tan(18deg)=0.33; spans X[-4,4], Z[-4,4].
        // The character's TWO feet then land at clearly different heights -> the leg
        // analytic solve + the lower-foot-governed pelvis drop both read in the capture.
        const float slope = 0.22f;
        auto groundY = [&](float /*x*/, float z){ return slope * z; };  // height under (x,z)
        {
            float a[3]={-4,groundY(-4,-4),-4}, b[3]={4,groundY(4,-4),-4},
                  c[3]={4,groundY(4, 4), 4}, d[3]={-4,groundY(-4,4), 4};
            pushQuad(a,b,c,d);
        }
        // A single raised STEP block straddling the character's BACK (+Z) foot so one
        // foot is on the step and the other on the slope below — the classic foot-IK
        // stair case. Top at +0.16 above the slope, X[-1.5,1.5], Z[0.15,2.5].
        const float stepZ0=0.15f, stepZ1=2.5f, stepX0=-1.5f, stepX1=1.5f, stepUp=0.16f;
        auto stepTopY = [&](float z){ return groundY(0,z) + stepUp; };
        {
            float a[3]={stepX0,stepTopY(stepZ0),stepZ0}, b[3]={stepX1,stepTopY(stepZ0),stepZ0},
                  c[3]={stepX1,stepTopY(stepZ1),stepZ1}, d[3]={stepX0,stepTopY(stepZ1),stepZ1};
            pushQuad(a,b,c,d);
            // step riser (facing -Z toward the camera) so the step reads in profile.
            float ra[3]={stepX0,groundY(0,stepZ0),stepZ0}, rb[3]={stepX1,groundY(0,stepZ0),stepZ0},
                  rc[3]={stepX1,stepTopY(stepZ0),stepZ0}, rd[3]={stepX0,stepTopY(stepZ0),stepZ0};
            pushQuad(ra,rb,rc,rd);
        }
        // Collide the combined ground: addStaticMesh wants tightly-packed xyz floats.
        std::vector<float> gpos(gv.size()*3);
        for (size_t i=0;i<gv.size();++i){ gpos[i*3]=gv[i].pos[0]; gpos[i*3+1]=gv[i].pos[1]; gpos[i*3+2]=gv[i].pos[2]; }
        fphys->addStaticMesh(gpos.data(), (uint32_t)gv.size(), gi.data(), (uint32_t)gi.size());
        x3::rhi::MeshHandle groundMesh = device->createMesh(gv.data(), (uint32_t)gv.size(), gi.data(), (uint32_t)gi.size());
        auto groundPx = x3::prims::makeCheckerRGBA(64, 8, 120, 130, 120, 64, 72, 66);
        x3::rhi::TextureHandle groundTex = device->createTexture(groundPx.data(), 64, 64, true);
        const float modelGround[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        const float whiteTint[4] = { 1, 1, 1, 1 };

        x3::rhi::IRenderDevice::SkyParams sp{};
        sp.enabled=true;
        // Sun toward the CAMERA side (-Z) + up, so the front of the character is lit
        // (a +Z sun backlights it into a silhouette from this -Z camera).
        sp.sunDir[0]=0.25f; sp.sunDir[1]=0.85f; sp.sunDir[2]=-0.5f;
        sp.sunColor[0]=1.0f; sp.sunColor[1]=0.97f; sp.sunColor[2]=0.92f;
        sp.sunIntensity=1.0f; sp.haze=0.35f; sp.exposure=1.2f;
        device->setSkyParams(sp);
        { x3::rhi::PointLight pl[3];
          auto setL=[](x3::rhi::PointLight& l,float x,float y,float z,float r,float g,float b,float rng){
            l.pos[0]=x;l.pos[1]=y;l.pos[2]=z;l.range=rng;l.color[0]=r;l.color[1]=g;l.color[2]=b; };
          // Key light on the CAMERA side (-Z) low so the front + the legs/feet read
          // (the camera looks toward +Z, so a +Z light would only backlight).
          setL(pl[0], 0.6f,2.2f,-3.0f, 7.0f,6.8f,6.4f, 30.0f);   // front key (camera side)
          setL(pl[1], 2.5f,2.5f,-1.0f, 3.0f,3.0f,3.4f, 30.0f);   // front-right fill
          setL(pl[2],-2.5f,2.5f,-1.0f, 3.0f,3.2f,3.0f, 30.0f);   // front-left fill
          device->setPointLights(pl, 3); }

        // Load the rigged character with the REAL device so its skinned meshes upload.
        auto pickAnimGlb = [](const std::string& dir, const char* base) -> std::string {
            namespace fsx = std::filesystem; std::string b(base);
            std::string stem = (b.size()>4 && b.substr(b.size()-4)==".glb") ? b.substr(0,b.size()-4) : b;
            std::error_code ec2; std::string anim = stem + "_anim.glb";
            if (fsx::exists(fsx::path(dir)/anim, ec2)) return anim;
            return b;
        };
        const std::string rigDir = x3::game::riggedGlbRoot();
        std::string rigFile = pickAnimGlb(rigDir, "chief_martinez.glb");
        std::unique_ptr<x3::asset::IAssetSource> asrc(x3::asset::createAssetSource());
        asrc->mountDir(rigDir, 0);
        std::unique_ptr<x3::asset::IModelLoader> mloader(x3::asset::createModelLoader(device, asrc.get()));
        x3::asset::Model cmodel = mloader->load(rigFile);
        auto drawables = x3::asset::makeDrawables(cmodel);

        x3::anim::Skinner sk;
        bool skinnable = cmodel.ok && sk.bind(cmodel);
        x3::logInfo(std::string("--screenshot-footik: rig=") + rigFile +
                    " ok=" + (cmodel.ok?"1":"0") + " skinnable=" + (skinnable?"1":"0") +
                    " footIkResolved=" + (sk.footIkResolved()?"1":"0"));
        if (skinnable) {
            x3::logInfo(std::string("--screenshot-footik: legs L=(") +
                std::string(sk.footIkBoneName(0,0)) + "," + std::string(sk.footIkBoneName(0,1)) + "," +
                std::string(sk.footIkBoneName(0,2)) + ") R=(" + std::string(sk.footIkBoneName(1,0)) + "," +
                std::string(sk.footIkBoneName(1,1)) + "," + std::string(sk.footIkBoneName(1,2)) +
                ") pelvis=" + std::string(sk.footIkBoneName(0,3)));
            int idle = sk.findClip({ "idle","stand","breath","loop" });
            int walk = sk.findClip({ "walk" });
            int run  = sk.findClip({ "run","jog","sprint" });
            sk.setLocomotionClips(idle, walk, run, 1.5f, 4.0f);
            sk.setLocomotion01(0.0f);   // standing -> idle, so the feet stay planted
        }

        // Character placement: stand at the foot of the step on the slope, facing the
        // camera (default -Z facing). The model origin sits at the slope height under
        // its position; foot-IK then conforms each foot to the slope/step it's over.
        // worldFromModel = this placement matrix.
        const float charX = 0.0f, charZ = -0.1f;
        const float charY = groundY(0, charZ);   // model origin on the slope
        float placement[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, charX, charY, charZ, 1 };

        // Build the broadphase + settle the static world so the foot rays hit (mirrors
        // the physics self-test, which steps twice before relying on rayCast).
        fphys->optimizeBroadphase();
        for (int i = 0; i < 2; ++i) fphys->step(1.0f/60.0f);

        // Ground raycast callback bridging the Skinner to the physics world.
        struct RayCtx { x3::phys::IPhysicsWorld* phys; };
        RayCtx rctx{ fphys.get() };
        x3::anim::Skinner::GroundRay gray;
        gray.user = &rctx;
        gray.fn = [](const float o[3], const float d[3], float maxD,
                     float hit[3], float n[3], void* u) -> bool {
            auto* c = (RayCtx*)u;
            x3::phys::Vec3 org{ o[0], o[1], o[2] };
            x3::phys::Vec3 dir{ d[0], d[1], d[2] };
            x3::phys::RayHit rh = c->phys->rayCast(org, dir, maxD, x3::phys::Layer::Static);
            if (!rh.hit) return false;
            hit[0]=rh.point.x; hit[1]=rh.point.y; hit[2]=rh.point.z;
            n[0]=rh.normal.x; n[1]=rh.normal.y; n[2]=rh.normal.z;
            return true;
        };

        // Camera: low + close 3/4 view from the front-right (-Z, +X) looking slightly
        // down at the legs/feet so the slope + step + planting all read in profile.
        {
            const float cx=2.4f, cy=1.0f, cz=-2.6f;     // front-right, low
            const float lx=0.0f, ly=0.30f, lz=0.4f;     // look at the lower legs/feet
            const float dx=lx-cx, dy=ly-cy, dz=lz-cz;
            const float dl=std::sqrt(dx*dx+dy*dy+dz*dz);
            device->setCamera(cx,cy,cz, std::atan2(dz,dx), std::asin(dl>1e-4f?dy/dl:0.0f), 45.0f);
        }

        const float dt = 1.0f/60.0f;
        // Render a still with IK either ON or OFF. Settle the blend + IK a moment so the
        // smoothed weights + pelvis converge, then capture the final frame.
        auto renderStill = [&](bool ikOn, const std::string& path) -> bool {
            if (skinnable) {
                if (ikOn) sk.setFootIk(true, gray, placement);
                else      { x3::anim::Skinner::GroundRay none{}; sk.setFootIk(false, none, placement); }
            }
            const int steps = 90;
            bool wrote=false;
            for (int s=0; s<=steps; ++s) {
                glfwPollEvents();
                if (skinnable) sk.applyLocomotion(cmodel, *device, dt);
                fphys->step(dt);
                const bool last = (s==steps);
                if (last) device->armCapture(path.c_str());
                auto frame = device->beginFrame();
                if (frame.valid) {
                    device->drawMesh(frame, groundMesh, groundTex, whiteTint, modelGround);
                    for (const auto& dr : drawables) {
                        float fin[16];
                        x3::asset::mulMat4(placement, dr.nodeTransform, fin);
                        // Brighten the (very dark tactical-gear) material so the legs/feet
                        // read against the ground in the capture (visual only).
                        float tint[4] = { dr.baseColorFactor[0]*2.2f + 0.25f,
                                          dr.baseColorFactor[1]*2.2f + 0.25f,
                                          dr.baseColorFactor[2]*2.2f + 0.25f, dr.baseColorFactor[3] };
                        device->drawMesh(frame, x3::rhi::MeshHandle{ dr.meshId },
                                         x3::rhi::TextureHandle{ dr.baseColorTexId },
                                         tint, fin);
                    }
                }
                device->endFrame(frame);
                if (last) wrote = device->captureFrame(path.c_str());
            }
            return wrote;
        };

        // OFF (reference) first, then ON last so the final logged weights reflect the
        // engaged IK. Reference path is "<stem>_noik.png".
        std::string offPath = captureFootIkPath;
        {
            auto dot = offPath.find_last_of('.');
            offPath = (dot==std::string::npos) ? offPath + "_noik" : offPath.substr(0,dot) + "_noik" + offPath.substr(dot);
        }
        bool wroteOff = renderStill(false, offPath);
        bool wroteOn  = renderStill(true,  captureFootIkPath);
        x3::logInfo(std::string("--screenshot-footik: IK-ON pelvisDrop=") + std::to_string(sk.footIkPelvisDrop()) +
                    " wL=" + std::to_string(sk.footIkLegWeight(0)) + " wR=" + std::to_string(sk.footIkLegWeight(1)));
        x3::logInfo(std::string("--screenshot-footik: ON -> ") + (wroteOn?captureFootIkPath:"FAILED") +
                    " | OFF -> " + (wroteOff?offPath:"FAILED"));

        mloader->unload(cmodel);
        device->destroyMesh(groundMesh);
        device->destroyTexture(groundTex);
        fphys->shutdown();
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return wroteOn ? 0 : 1;
    }

    return -1;   // no capture flag set — continue boot
}

// ===========================================================================
// `--set` WIRING FOR THE CAPTURE HOSTS — the SAME bug the --world hosts had.
//
// The per-frame cvar->device sync hub (app_run.cpp applyRtaoCVars) belongs to
// runDefaultHost. The `--screenshot-*` handlers run BEFORE it and return, so
// `--set` never reached them either: `--screenshot-city --set r_bloom 0` wrote
// a PNG with bloom still on, exited 0, and looked entirely plausible. That
// matters more here than anywhere else, because captures are how this project
// verifies ART — a silently-dropped flag voids the visual evidence itself.
//
// WHY A WRAPPER AND NOT THE TWO-LINE HOIST kHostRoutes got. The --world side is
// a TABLE (one route -> one function pointer), so the wiring slots cleanly
// around each dispatch. This side is ONE 3,700-line function with 25 top-level
// `return`s, one per capture family — there is no table and no single exit to
// hang the "after" audit on. Rather than edit 25 return sites (which is exactly
// the remember-me trap this whole change exists to kill), the body became a
// static *Impl and this thin wrapper owns entry and exit for every handler at
// once. Same by-construction property, no per-handler opt-in.
//
// hc.captureHost is the pre-dispatch signal that a capture owns this run — set
// by ONE prefix match in parseCli, so a `--screenshot-*` flag added later is
// covered with no edit here (see cli.h).
// ===========================================================================
int dispatchScreenshotHosts(HostContext& hc) {
    const bool capture = !hc.captureHost.empty();
    if (capture && hc.device) applyHostRenderCVars(hc, *hc.device, hc.captureHost);

    const int rc = dispatchScreenshotHostsImpl(hc);

    if (rc < 0) {
        // No handler HERE owned it (e.g. --screenshot-destruct / -tunnel /
        // -elevator are --world hosts, and -vigil / -dialog / -crowd belong to
        // the default host). Hand the device back with the latch DISARMED: the
        // --world dispatch re-arms it identically, and the default host must
        // keep its live console edits (it syncs every frame from a console
        // already seeded with --set). Reporting is left to whoever does own it.
        if (capture && hc.device)
            hc.device->setCVarOverrides(x3::rhi::IRenderDevice::RenderCVarOverrides{});
        return rc;
    }
    reportUnappliedHostCVars(hc, hc.captureHost);
    return rc;
}

}} // namespace x3::apphost
