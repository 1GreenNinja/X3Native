// screenshot_hosts — headless screenshot/capture handlers lifted VERBATIM from
// main() (#28 deep split, Phase B). See screenshot_hosts.h. The only edits are
// the HostContext alias prelude + the 9 `device.get()` -> `device` (the host
// takes a raw IRenderDevice*). This TU now owns the SOLE non-inline gif.h
// include (moved from main.cpp) + its own file-local stb_image copy.
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#include "engine/core/x3_log.h"
#include "engine/core/x3_boot.h"
#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"
#include "engine/asset/IAssetSource.h"
#include "engine/asset/IModelLoader.h"

#include "host_context.h"
#include "screenshot_hosts.h"
#include "showroom_tod.h"
#include "cinematic.h"
#include "intro_orchestrator.h"   // x3::intro::runIntroCombatShots (--screenshot-introcombat)
#include "scene.h"
#include "mesh_prims.h"
#include "env_art.h"
#include "monster.h"
#include "anim.h"
#include "terrain.h"
#include "cutscene.h"
#include "leveldoc_world.h"
#include "level_loader.h"
#include "editor/editor_host.h"
#include "asset_root.h"

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

int dispatchScreenshotHosts(HostContext& hc) {
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
    const bool rtshShot = hc.rtshShot;            const std::string& rtshShotDir = hc.rtshShotDir;
    const bool showroomShot = hc.showroomShot;    const std::string& showroomShotPath = hc.showroomShotPath;
    const bool carShot = hc.carShot;              const std::string& carShotDir = hc.carShotDir;
    const bool planetShot = hc.planetShot;        const std::string& planetShotPath = hc.planetShotPath;
    const bool nightskyShot = hc.nightskyShot;    const std::string& nightskyShotPath = hc.nightskyShotPath;
    const bool cutsceneShot = hc.cutsceneShot;    const std::string& cutsceneShotPath = hc.cutsceneShotPath;
    const bool introCombatShot = hc.introCombatShot; const std::string& introCombatShotPath = hc.introCombatShotPath;
    const bool terrainShot = hc.terrainShot;      const std::string& terrainShotPath = hc.terrainShotPath;
    const bool oceanShot = hc.oceanShot;          const std::string& oceanShotPath = hc.oceanShotPath;
    const bool captureAi = hc.captureAi;          const std::string& captureAiDir = hc.captureAiDir;
    const bool captureWalk = hc.captureWalk;      const std::string& captureWalkPath = hc.captureWalkPath;
    const bool captureFootIk = hc.captureFootIk;  const std::string& captureFootIkPath = hc.captureFootIkPath;

    // ==== VERBATIM handler bodies (device.get() -> device) ====
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
            float p0[3] = { 0.0f, 0.0f, 0.0f }, s0[3] = { 8.0f, 0.5f, 8.0f };   // floor plate
            float p1[3] = { -2.0f, 1.0f, 1.0f }, s1[3] = { 2.0f, 2.0f, 2.0f };  // a box
            float p2[3] = { 3.0f, 1.0f, 1.0f }, s2[3] = { 3.0f, 2.0f, 4.0f };   // a ramp
            proofHost.placeBrush(0u, p0, s0, *device, proofScene, *proofPhys);
            proofHost.placeBrush(0u, p1, s1, *device, proofScene, *proofPhys);
            proofHost.placeBrush(1u, p2, s2, *device, proofScene, *proofPhys);
            // Feature 3 proof: place a GLB prop (renders via renderModels each frame).
            proofHost.placeModel("SciFi_Warehouse_Kit/Barrel.glb", *device);
        }
        // A pleasant 3/4 vantage on the brushes; a touch of ambient so the grey reads.
        device->setCamera(8.0f, 6.5f, 11.0f, -2.35f, -0.45f, 60.0f);
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
        applyShowroomTimeOfDay(device, gShowroomDay, /*interiorLights*/nullptr);
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
        device->setShadowBounds(cx, cy, cz, 150.0f);

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
        const bool wrote = device->captureFrame(showroomShotPath.c_str());
        if (wrote) x3::logInfo("--screenshot-showroom: wrote " + showroomShotPath);
        else       x3::logError("--screenshot-showroom: capture FAILED");

        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return wrote ? 0 : 1;
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
            x3::rhi::IRenderDevice::ReflectionParams rf{};
            rf.ssr = true; rf.rtFallback = true; rf.fullRes = true; rf.intensity = 1.0f;
            device->setReflectionParams(rf);
            x3::logInfo(std::string("--screenshot-car: reflections ON; rayTracingSupported=") +
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

        x3::cut::CutscenePlayer player(cs);
        player.onEvent([&](const x3::cut::Event& e, bool) { cin.onEvent(e.name, cs, e.t); });
        player.seek(cueTime);
        cin.update(cs, cueTime);   // backfill the trail/puff state up to the scrub point

        const float t = player.time();
        const x3::cut::CamPose cam = x3::cut::evalCamera(cs, t);
        device->setCamera(cam.pos.x, cam.pos.y, cam.pos.z, cam.yaw, cam.pitch, cam.fov);
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

    // ---- Interactive space-combat proof stills (--screenshot-introcombat [base]) ----
    // Job B visual verification: build the LIVE combat scene (capital + fighters +
    // bolts + HUD) and capture <base>_takecontrol.png (the hand-over) + <base>_fight
    // .png (the dogfight) via the shared LiveCombatView::drawScene.
    if (introCombatShot) {
        x3::logInfo("--screenshot-introcombat: capturing interactive-combat proof stills -> " +
                    introCombatShotPath + "_{takecontrol,fight}.png");
        const bool ok = x3::intro::runIntroCombatShots(*device, window, introCombatShotPath);
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return ok ? 0 : 1;
    }

    // ---- Terrain vantage mode (--screenshot-terrain [path.png]) ------------
    // Build the B2 tiled procedural terrain world (terrain meshes + the analytic
    // sky lit by the existing sun), pose a camera up on the hills looking toward
    // the sun so the lit rolling terrain, cast shadows, and sky all read, settle a
    // few frames so the shadow map + LOD register, and capture a PNG. Built
    // entirely through the public render API + a local Jolt world (so the terrain
    // collision path is exercised too) — no game/audio stack. EFLZ Level 1 is an
    // enclosed interior; this is how to SEE + verify the outdoor terrain.
    if (terrainShot) {
        x3::logInfo("--screenshot-terrain: rendering STREAMED terrain world to " + terrainShotPath);

        // B3: this path now exercises the STREAMER under validation. A job system
        // generates tiles async; the focus is SWEPT across the world during the
        // frame loop so stream-IN (createMesh + addStaticMesh) AND stream-OUT
        // (destroyMesh + removeBody) both run inside validated frames, proving the
        // async upload + teardown barriers are validation-clean. The camera trails
        // the swept focus so the final capture is a lit terrain vista.
        std::unique_ptr<x3::jobs::IJobSystem> tjobs(x3::jobs::createJobSystem());
        tjobs->init(0);
        std::unique_ptr<x3::phys::IPhysicsWorld> tphys(x3::phys::createPhysicsWorld());
        tphys->init();
        x3::game::Scene tscene;
        x3::game::TerrainStreamer streamer;
        x3::game::TerrainConfig tcfg;   // 32 m tiles; unbounded (streamed)

        // Turn ON the analytic sky with the SAME sun the shadow pass + mesh.frag
        // use (normalize(0.4,1,0.3)) so the disk sits where the world is lit from.
        x3::rhi::IRenderDevice::SkyParams sp{};
        sp.enabled = true;
        sp.sunDir[0] = 0.4f; sp.sunDir[1] = 1.0f; sp.sunDir[2] = 0.3f;
        sp.sunColor[0] = 1.0f; sp.sunColor[1] = 0.97f; sp.sunColor[2] = 0.92f;
        sp.sunIntensity = 1.0f; sp.haze = 0.5f; sp.exposure = 1.0f;
        device->setSkyParams(sp);

        // Start the focus well away from the origin (proves unbounded coords) and
        // bring up the ring there.
        float fx = -90.0f, fz = -120.0f;
        streamer.init(tscene, *device, *tphys, tjobs.get(), tcfg, fx, fz, /*radius=*/8);

        const float sunYaw   = std::atan2(0.3f, 0.4f);  // toward the sun in XZ
        const float camPitch = -0.16f;                  // ~9deg down: hills + shadows + sky

        const float dt = 1.0f / 60.0f;
        // Render a measured window of frames; report the averaged GPU-pass time
        // (vsync-independent). Sweep the focus +X so tiles stream in/out during the
        // validated loop. The capture is armed on the final frame.
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
        const bool wrote = device->captureFrame(terrainShotPath.c_str());
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
        wt.modelFile = pickAnimGlb(x3::game::riggedGlbRoot(), "marcus_webb.glb");
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
        // a clear mid-stride; capture the final frame.
        const float dt = 1.0f / 60.0f;
        x3::game::AttackFxFn noFx{}; x3::game::BossPhaseFn noPhase{}; x3::game::AllyQueryFn noAllies{};
        const int steps = 90;
        bool wrote = false;
        for (int step = 0; step <= steps; ++step) {
            glfwPollEvents();
            guard.update(dt, wscene, *wphys, tgt.eye /*planar*/, tgt.eye /*eye*/,
                         &tgt, noFx, noPhase, noAllies);
            wphys->step(dt);
            const bool last = (step == steps);
            if (last) device->armCapture(captureWalkPath.c_str());
            auto frame = device->beginFrame();
            if (frame.valid) {
                device->drawMesh(frame, groundMesh, groundTex, whiteTint, modelGround);
                wscene.render(*device, frame);
                guard.drawMonster(*device, frame, wscene);
            }
            device->endFrame(frame);
            if (last) wrote = device->captureFrame(captureWalkPath.c_str());
        }
        x3::logInfo(std::string("--capture-walk: aiState=") +
                    x3::game::aiStateName(guard.aiState()) +
                    (wrote ? "  wrote " + captureWalkPath : "  CAPTURE FAILED"));

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

}} // namespace x3::apphost
