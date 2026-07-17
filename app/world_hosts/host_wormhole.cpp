// ============================================================================
// host_wormhole — S3 WORMHOLE TRANSIT + Salvari crystal-matrix WORMHOLE VFX +
// capital-ship TRACTOR BEAM FOLD (integration feast, 2026-07-11): the stranded
// 14900K space-combat lanes (feat/wormhole-transit, feat/wormhole-vfx,
// feat/tractor-beam — parked 05-30, cut from the dead cull-combined lineage)
// folded onto the current playable line.
//
// Ported from the stranded branches' monolith main.cpp `--world` blocks into
// the world_hosts structure (the branches predate the cli/test_registry/
// world_hosts split); logic byte-faithful, only the mechanics of reaching
// shared state through `hc.` changed. Three discrete `--world` targets:
//
//   * --world wormhole-transit : the autopilot crystal-matrix jump ride (the S3
//     WormholeTransit runner drives the WormholeVfx tunnel bloom to convergence).
//   * --world wormhole         : the standalone crystal-matrix tunnel showcase
//     (fly straight down +Z; roll + white-hot core).
//   * --world tractor          : the intro capture beat — a capital ship reels
//     Jake's fighter in on a tapered energy cone.
//
// Each is windowed (Esc to quit) or headless (--screenshot <path> [--shot-cam]).
// GATES: --test-wormhole / --test-wormhole-transit / --test-tractor (self-tests
// live in the lane TUs; registered in test_registry.cpp).
// ============================================================================

#include "../world_hosts.h"
#include "../host_context.h"
#include "../mesh_prims.h"                 // x3::prims box/checker builders (tractor scene)
#include "../space/space_layer.h"
#include "../space/wormhole_vfx.h"
#include "../space/wormhole_transit.h"
#include "../space/tractor_beam.h"
#include "engine/rhi/IRenderDevice.h"
#include "engine/core/x3_log.h"

#include <GLFW/glfw3.h>
#include <cmath>
#include <string>

namespace x3 { namespace apphost {

// ---- S3 wormhole transit showcase (--world wormhole-transit) ---------------
int hostWormholeTransit(HostContext& hc) {
    if (hc.worldMode != "wormhole-transit") return -1;
    auto* device = hc.device;
    GLFWwindow* window = hc.window;

    x3::logInfo("--world wormhole-transit: showcasing the S3 crystal-matrix jump");

    { x3::rhi::IRenderDevice::SsaoParams sp{}; sp.enabled = false; device->setSsaoParams(sp); }
    { x3::rhi::IRenderDevice::GiParams   gp{}; gp.enabled   = false; device->setGiParams(gp); }
    { x3::rhi::IRenderDevice::SkyParams  sp{}; sp.enabled   = false; device->setSkyParams(sp); }

    x3::space::SpaceLayer L;
    L.init();
    x3::space::WormholeTransit wt;
    const float kDuration = 6.0f;
    wt.init(*device, L, kDuration);
    L.requestWormhole(/*destSystemId=*/42u);

    if (hc.headless) {
        const std::string outPath = hc.screenshot ? hc.screenshotPath
                                                  : std::string("agent_wormhole_transit.png");
        const int kFrames = 36;
        const float dt = (kDuration * 0.8f) / (float)kFrames;
        float t = 0.0f;
        for (int i = 0; i < kFrames; ++i) {
            glfwPollEvents();
            L.update(dt);
            t += dt;
            float camZ = 4.0f + t * 6.0f;
            device->setCamera(0.0f, 0.0f, camZ, 0.0f, 0.0f, 75.0f);
            if (hc.shotCamOverride) {
                device->setCamera(hc.shotCam[0], hc.shotCam[1], hc.shotCam[2],
                                  hc.shotCam[3], hc.shotCam[4], 75.0f);
            }
            if (i == kFrames - 1) device->armCapture(outPath.c_str());
            auto frame = device->beginFrame();
            if (frame.valid) wt.render(*device, frame, nullptr, t);
            device->endFrame(frame);
        }
        const bool wrote = device->captureFrame(outPath.c_str());
        if (wrote) x3::logInfo("--world wormhole-transit: wrote " + outPath +
                               " (progress=" + std::to_string(wt.progress()) + ")");
        else       x3::logError("--world wormhole-transit: capture FAILED");
        wt.shutdown(*device);
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return wrote ? 0 : 1;
    }

    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
    double startTimeWT = glfwGetTime();
    double prevTimeWT  = startTimeWT;
    x3::logInfo("--world wormhole-transit: the jump plays then holds; Esc to quit");
    int lastWsWT = (int)hc.W, lastHsWT = (int)hc.H;
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;
        double now = glfwGetTime();
        float dt = (float)(now - prevTimeWT); prevTimeWT = now;
        float t = (float)(now - startTimeWT);
        L.update(dt);
        int cw, chh; glfwGetFramebufferSize(window, &cw, &chh);
        if (cw != lastWsWT || chh != lastHsWT) { lastWsWT = cw; lastHsWT = chh; if (cw>0&&chh>0) device->onResize((uint32_t)cw,(uint32_t)chh); }
        float camZ = 4.0f + t * 6.0f;
        device->setCamera(0.0f, 0.0f, camZ, 0.0f, 0.0f, 75.0f);
        auto frame = device->beginFrame();
        if (frame.valid) wt.render(*device, frame, nullptr, t);
        device->endFrame(frame);
    }

    wt.shutdown(*device);
    device->shutdown();
    if (window) glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

// ---- Salvari crystal-matrix wormhole showcase (--world wormhole) -----------
int hostWormhole(HostContext& hc) {
    if (hc.worldMode != "wormhole") return -1;
    auto* device = hc.device;
    GLFWwindow* window = hc.window;

    x3::logInfo("--world wormhole: showcasing the Salvari crystal-matrix wormhole");

    { x3::rhi::IRenderDevice::SsaoParams sp{}; sp.enabled = false; device->setSsaoParams(sp); }
    { x3::rhi::IRenderDevice::GiParams   gp{}; gp.enabled = false; device->setGiParams(gp); }
    { x3::rhi::IRenderDevice::SkyParams  sp{}; sp.enabled = false; device->setSkyParams(sp); }

    x3::space::WormholeVfx wh;
    x3::space::WormholeVfx::Tuning whT;
    wh.init(*device, whT);
    if (!wh.initialized()) {
        x3::logError("--world wormhole: WormholeVfx::init() failed");
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }
    wh.setOrigin(0.0f, 0.0f, 0.0f);
    const float kFlyZ0 = 6.0f;
    const float kFlyZ1 = whT.length * 0.9f;
    const float kAxisYaw = 1.57079633f;

    if (hc.headless) {
        const std::string outPath = hc.screenshot ? hc.screenshotPath
                                                  : std::string("agent_wormhole.png");
        const int kFrames = 48;
        const int kShotFrame = 2;
        const float kShotProgress = 0.55f;
        for (int i = 0; i < kFrames; ++i) {
            glfwPollEvents();
            float t = (float)i * (1.0f / 30.0f);
            float u = (float)i / (float)(kFrames - 1);
            bool isShot = (i == kShotFrame);
            float camZ = isShot ? kFlyZ0 : (kFlyZ0 + (kFlyZ1 - kFlyZ0) * u);
            float prog = isShot ? kShotProgress : u;
            float roll = 0.25f * t;
            if (isShot) {
                // Level for the gated screenshot (unchanged framing).
                device->setCamera(0.0f, 0.0f, camZ, kAxisYaw, 0.0f, 80.0f);
            } else {
                // REAL barrel roll (setCameraBasis): forward straight down the tunnel
                // (+Z), up rotated around forward by `roll`. Replaces the faked
                // 0.04-rad yaw/pitch wobble that stood in for roll before the engine
                // had a roll-capable camera. THIS is what the "roll + white-hot core"
                // comment at the top of the file always wanted.
                const float fwd[3] = { 0.0f, 0.0f, 1.0f };
                const float up[3]  = { -std::sin(roll), std::cos(roll), 0.0f };
                device->setCameraBasis(0.0f, 0.0f, camZ, fwd, up, 80.0f);
            }
            if (hc.shotCamOverride) {
                device->setCamera(hc.shotCam[0], hc.shotCam[1], hc.shotCam[2],
                                  hc.shotCam[3], hc.shotCam[4], 80.0f);
            }
            if (isShot) device->armCapture(outPath.c_str());
            auto frame = device->beginFrame();
            if (frame.valid) {
                wh.render(*device, frame, nullptr, t, prog, whT);
            }
            device->endFrame(frame);
        }
        const bool wrote = device->captureFrame(outPath.c_str());
        if (wrote) x3::logInfo("--world wormhole: wrote " + outPath);
        else       x3::logError("--world wormhole: capture FAILED");
        wh.shutdown(*device);
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return wrote ? 0 : 1;
    }

    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
    double startTimeW = glfwGetTime();
    x3::logInfo("--world wormhole: flying through the crystal-matrix jump, Esc to quit");
    int lastWsW = (int)hc.W, lastHsW = (int)hc.H;
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;
        double now = glfwGetTime();
        float t = (float)(now - startTimeW);
        float u = std::fmod(t, 6.0f) / 6.0f;
        float camZ = kFlyZ0 + (kFlyZ1 - kFlyZ0) * u;
        float roll = 0.25f * t;
        int cw, chh; glfwGetFramebufferSize(window, &cw, &chh);
        if (cw != lastWsW || chh != lastHsW) { lastWsW = cw; lastHsW = chh; if (cw>0&&chh>0) device->onResize((uint32_t)cw,(uint32_t)chh); }
        // REAL barrel roll down the tunnel (roll-capable camera) — a true spiral,
        // not the faked yaw/pitch wobble the roll-less setCamera forced before.
        const float fwd[3] = { 0.0f, 0.0f, 1.0f };
        const float up[3]  = { -std::sin(roll), std::cos(roll), 0.0f };
        device->setCameraBasis(0.0f, 0.0f, camZ, fwd, up, 80.0f);
        auto frame = device->beginFrame();
        if (frame.valid) {
            wh.render(*device, frame, nullptr, t, u, whT);
        }
        device->endFrame(frame);
    }

    wh.shutdown(*device);
    device->shutdown();
    if (window) glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

// ---- Capital-ship tractor-beam capture showcase (--world tractor) ----------
int hostTractor(HostContext& hc) {
    if (hc.worldMode != "tractor") return -1;
    auto* device = hc.device;
    GLFWwindow* window = hc.window;

    x3::logInfo("--world tractor: showcasing the capital-ship tractor beam capture");

    { x3::rhi::IRenderDevice::SsaoParams sp{}; sp.enabled = false; device->setSsaoParams(sp); }
    { x3::rhi::IRenderDevice::GiParams   gp{}; gp.enabled = false; device->setGiParams(gp); }
    { x3::rhi::IRenderDevice::SkyParams  kp{}; kp.enabled = false; device->setSkyParams(kp); }
    // Three-point rig so the metal boxes read with shape against black space.
    { x3::rhi::PointLight pl[3];
      pl[0].pos[0]= 10.0f; pl[0].pos[1]= 12.0f; pl[0].pos[2]= 12.0f; pl[0].range=120.0f;
      pl[0].color[0]=11.0f; pl[0].color[1]=10.5f; pl[0].color[2]=9.0f;
      pl[1].pos[0]=-14.0f; pl[1].pos[1]= 8.0f; pl[1].pos[2]= 4.0f; pl[1].range=100.0f;
      pl[1].color[0]=3.0f;  pl[1].color[1]=3.6f;  pl[1].color[2]=4.8f;
      pl[2].pos[0]= 6.0f; pl[2].pos[1]=-6.0f; pl[2].pos[2]=-10.0f; pl[2].range=90.0f;
      pl[2].color[0]=2.5f;  pl[2].color[1]=1.8f;  pl[2].color[2]=1.4f;
      device->setPointLights(pl, 3); }

    // Capital-ship emitter end (the beam apex). A big slab of hull.
    x3::prims::PrimMesh capGeo = x3::prims::makeBox(5.0f, 3.0f, 6.0f, 0,0,0, 0.4f);
    auto capMesh = device->createMesh(capGeo.verts.data(), (uint32_t)capGeo.verts.size(),
                                      capGeo.index.data(), (uint32_t)capGeo.index.size());
    auto capTexD = x3::prims::makeCheckerRGBA(64, 8, 150, 158, 175, 60, 66, 84);
    auto capTex  = device->createTexture(capTexD.data(), 64, 64, true);

    // Jake's fighter (the captured ship). A small box.
    x3::prims::PrimMesh figGeo = x3::prims::makeBox(1.0f, 0.5f, 1.6f, 0,0,0, 0.5f);
    auto figMesh = device->createMesh(figGeo.verts.data(), (uint32_t)figGeo.verts.size(),
                                      figGeo.index.data(), (uint32_t)figGeo.index.size());
    auto figTexD = x3::prims::makeCheckerRGBA(64, 8, 200, 150, 110, 90, 70, 60);
    auto figTex  = device->createTexture(figTexD.data(), 64, 64, true);

    x3::space::TractorBeam beam;
    x3::space::TractorBeam::Tuning beamT;
    beam.init(*device, beamT);
    if (!beam.initialized()) {
        x3::logError("--world tractor: TractorBeam::init() failed");
        device->destroyMesh(capMesh); device->destroyTexture(capTex);
        device->destroyMesh(figMesh); device->destroyTexture(figTex);
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    // Emitter sits on the front face of the capital ship at the origin; the
    // fighter starts far out along +X and is reeled IN toward the emitter.
    const float kEmitter[3] = { 6.0f, 0.0f, 0.0f };   // beam apex (capital-ship hull face)
    const float kCapStart   = 40.0f;                  // fighter start distance (X)
    const float kCapEnd     = 11.0f;                  // fighter held just off the hull

    // Compose the fighter transform + draw the full scene for a given fighter X
    // and beam intensity/time.
    auto drawTractorScene = [&](const x3::rhi::FrameContext& frame, float figX,
                                float intensity, float tSec) {
        // Capital ship at origin.
        const float capM[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        const float capTint[4] = { 1.4f, 1.45f, 1.6f, 1.0f };
        device->drawMesh(frame, capMesh, capTex, capTint, capM);
        // Fighter at (figX, slight bob, 0), with a gentle tumble as it's pulled.
        float bob = 0.6f * std::sin(tSec * 1.3f);
        float yaw = 0.3f * std::sin(tSec * 0.9f);
        float c = std::cos(yaw), s = std::sin(yaw);
        const float figM[16] = {
            c, 0, -s, 0,
            0, 1,  0, 0,
            s, 0,  c, 0,
            figX, bob, 0.0f, 1.0f
        };
        const float figTint[4] = { 1.6f, 1.35f, 1.1f, 1.0f };
        device->drawMesh(frame, figMesh, figTex, figTint, figM);
        // The beam: apex at the emitter, base at the fighter (to == fighter pos).
        const float to[3] = { figX, bob, 0.0f };
        beam.render(*device, frame, nullptr, kEmitter, to, intensity, tSec, beamT);
    };

    if (hc.headless) {
        const std::string outPath = hc.screenshot ? hc.screenshotPath
                                                  : std::string("agent_tractor.png");
        const int kFrames = 60;
        const int kShotFrame = 40;   // late: beam locked on, fighter half-reeled
        for (int i = 0; i < kFrames; ++i) {
            glfwPollEvents();
            float t = (float)i * (1.0f / 30.0f);
            float u = (float)i / (float)(kFrames - 1);
            // Intensity ramps up fast (lock-on) then holds; fighter pulled in.
            float intensity = std::min(1.0f, u * 2.2f);
            float figX = kCapStart + (kCapEnd - kCapStart) * u;
            // Camera off to the side + above so the WHOLE beam (emitter->fighter)
            // and both ships fill the frame.
            device->setCamera(20.0f, 14.0f, 34.0f, 3.95f, -0.28f, 70.0f);
            if (hc.shotCamOverride) {
                device->setCamera(hc.shotCam[0], hc.shotCam[1], hc.shotCam[2],
                                  hc.shotCam[3], hc.shotCam[4], 70.0f);
            }
            if (i == kShotFrame) device->armCapture(outPath.c_str());
            auto frame = device->beginFrame();
            if (frame.valid) {
                drawTractorScene(frame, figX, intensity, t);
            }
            device->endFrame(frame);
        }
        const bool wrote = device->captureFrame(outPath.c_str());
        if (wrote) x3::logInfo("--world tractor: wrote " + outPath);
        else       x3::logError("--world tractor: capture FAILED");
        beam.shutdown(*device);
        device->destroyMesh(capMesh); device->destroyTexture(capTex);
        device->destroyMesh(figMesh); device->destroyTexture(figTex);
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return wrote ? 0 : 1;
    }

    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
    double startTimeT = glfwGetTime();
    x3::logInfo("--world tractor: the capital ship reels Jake's fighter in; Esc to quit");
    int lastWsT = (int)hc.W, lastHsT = (int)hc.H;
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;
        double now = glfwGetTime();
        float t = (float)(now - startTimeT);
        // Loop the capture beat every 5s so the showcase repeats.
        float u = std::fmod(t, 5.0f) / 5.0f;
        float intensity = std::min(1.0f, u * 2.2f);
        float figX = kCapStart + (kCapEnd - kCapStart) * u;
        int cw, chh; glfwGetFramebufferSize(window, &cw, &chh);
        if (cw != lastWsT || chh != lastHsT) { lastWsT = cw; lastHsT = chh; if (cw>0&&chh>0) device->onResize((uint32_t)cw,(uint32_t)chh); }
        device->setCamera(20.0f, 14.0f, 34.0f, 3.95f, -0.28f, 70.0f);
        auto frame = device->beginFrame();
        if (frame.valid) {
            drawTractorScene(frame, figX, intensity, t);
        }
        device->endFrame(frame);
    }

    beam.shutdown(*device);
    device->destroyMesh(capMesh); device->destroyTexture(capTex);
    device->destroyMesh(figMesh); device->destroyTexture(figTex);
    device->shutdown();
    if (window) glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

}} // namespace x3::apphost
