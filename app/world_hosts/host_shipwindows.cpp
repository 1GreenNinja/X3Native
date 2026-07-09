// ============================================================================
// host_shipwindows — S5 ship-interior + S6 ship-windows FOLD (integration
// feast, 2026-07-09): the walkable small-cockpit interior (app/space/
// ship_interior.*) with TRUE-PORTAL moving space out every manifest window
// (app/space/ship_windows.*, star/nebula UVs panned by envYaw) + per-window
// light-bleed. Ported from the stranded feat/ship-windows branch's monolith
// main.cpp host into the world_hosts structure (the branch predates the
// cli/test_registry/world_hosts split); logic byte-faithful, only the
// mechanics of reaching shared state through `hc.` changed.
//
//   * WALKABLE (windowed): --world ship-windows — WASD + mouse, walls collide.
//   * SCREENSHOT (headless): --world ship-windows --screenshot [--shot-cam].
//   * GATES: --test-shipinterior / --test-shipwindows (self-tests live in the
//     lane TUs; registered in test_registry.cpp).
// ============================================================================

#include "../world_hosts.h"
#include "../host_context.h"
#include "../scene.h"
#include "../player.h"
#include "../space/ship_interior.h"
#include "../space/ship_windows.h"
#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"
#include "engine/core/x3_log.h"

#include <GLFW/glfw3.h>
#include <memory>
#include <string>
#include <vector>

namespace x3 { namespace apphost {

int hostShipWindows(HostContext& hc) {
    if (hc.worldMode != "ship-windows") return -1;
    auto* device = hc.device;
    GLFWwindow* window = hc.window;

    x3::logInfo("--world ship-windows: building the cockpit with TRUE-PORTAL moving space");

    std::unique_ptr<x3::phys::IPhysicsWorld> sphys(x3::phys::createPhysicsWorld());
    if (!sphys->init()) {
        x3::logError("--world ship-windows: physics init failed");
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    x3::game::Scene sscene;
    x3::space::ShipInterior interior;
    interior.build(*device, sscene, *sphys, x3::space::ShipInterior::makeSmallCockpit());

    x3::space::ShipWindows windows;
    windows.init(*device, interior.manifest());

    // No sky (inside the hull). SSAO/GI raster fallback off so a no-RT capture
    // is not black (lane brief; note: on the 3090 Ti RT paths are now live but
    // the fold keeps the branch's proven look byte-faithful — RTAO polish is a
    // separate feast course). Interior ceiling fill + light-bleed uploaded together.
    { x3::rhi::IRenderDevice::SkyParams sp{}; sp.enabled = false; device->setSkyParams(sp); }
    { x3::rhi::IRenderDevice::SsaoParams ao{}; ao.enabled = false; device->setSsaoParams(ao); }
    { x3::rhi::IRenderDevice::GiParams gi{}; gi.enabled = false; device->setGiParams(gi); }
    std::vector<x3::rhi::PointLight> lights;
    { x3::rhi::PointLight pl{}; pl.pos[0]=0.0f; pl.pos[1]=2.7f; pl.pos[2]=0.0f;
      pl.range=10.0f; pl.color[0]=4.0f; pl.color[1]=4.2f; pl.color[2]=4.6f; lights.push_back(pl); }
    for (const auto& bl : windows.bleedLights()) lights.push_back(bl);
    device->setPointLights(lights.data(), (uint32_t)lights.size());

    // ===== Headless screenshot: look forward at the moving viewport. =====
    if (hc.headless) {
        // Stand at the aft of the cockpit looking forward (-Z) at the forward
        // window so the deck + console + the moving space fill the frame.
        float cam[5] = { 0.0f, 1.7f, 2.2f, -1.5708f, -0.04f };
        if (hc.shotCamOverride) for (int k = 0; k < 5; ++k) cam[k] = hc.shotCam[k];
        const float dt = 1.0f / 60.0f;
        const std::string outPath = hc.screenshot ? hc.screenshotPath
                                                  : std::string("captures/windows.png");
        const int kSettle = 16;
        for (int i = 0; i < kSettle; ++i) {
            glfwPollEvents();
            sphys->step(dt);
            sscene.update(*sphys);
            const float t = (float)i * dt;
            const float envYaw = 0.25f * t;          // the ship "flies" — space pans
            device->setCamera(cam[0], cam[1], cam[2], cam[3], cam[4], 70.0f);
            if (i == kSettle - 1) device->armCapture(outPath.c_str());
            auto frame = device->beginFrame();
            if (frame.valid) {
                interior.render(*device, frame, sscene);
                windows.setCamera(cam[0], cam[1], cam[2]);
                windows.render(*device, frame, /*viewProj16=*/nullptr, t, envYaw, 0.0f);
            }
            device->endFrame(frame);
        }
        const bool wrote = device->captureFrame(outPath.c_str());
        if (wrote) x3::logInfo("--world ship-windows: wrote " + outPath);
        else       x3::logError("--world ship-windows: capture FAILED");
        windows.shutdown(*device);
        interior.shutdown(*sphys);
        sphys->shutdown();
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return wrote ? 0 : 1;
    }

    // ===== Walkable windowed path: real first-person Player + the moving space. =====
    x3::game::Player splayer;
    const x3::phys::Vec3 spawn = interior.spawnPoint();
    splayer.spawn(*sphys, spawn.x, spawn.y, spawn.z);

    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
    double lastMX, lastMY; glfwGetCursorPos(window, &lastMX, &lastMY);
    double startTime = glfwGetTime();
    double prevTime = startTime;
    bool prevSpaceS = false;
    int lastWs = 0, lastHs = 0;
    glfwGetFramebufferSize(window, &lastWs, &lastHs);
    x3::logInfo("--world ship-windows: WASD walk, mouse look, Space jump, LeftShift sprint, Esc to quit");

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;

        double now = glfwGetTime();
        float dt = (float)(now - prevTime); prevTime = now;
        if (dt > 0.1f) dt = 0.1f;
        const float t = (float)(now - startTime);

        double mx, my; glfwGetCursorPos(window, &mx, &my);
        float ddx = (float)(mx - lastMX), ddy = (float)(my - lastMY);
        lastMX = mx; lastMY = my;

        auto kd = [&](int k) { return glfwGetKey(window, k) == GLFW_PRESS; };
        bool spaceNow = kd(GLFW_KEY_SPACE);

        x3::game::PlayerInput in;
        if (kd(GLFW_KEY_W)) in.moveFwd    += 1.0f;
        if (kd(GLFW_KEY_S)) in.moveFwd    -= 1.0f;
        if (kd(GLFW_KEY_D)) in.moveStrafe += 1.0f;
        if (kd(GLFW_KEY_A)) in.moveStrafe -= 1.0f;
        in.sprint      = kd(GLFW_KEY_LEFT_SHIFT);
        in.jumpPressed = spaceNow && !prevSpaceS;
        in.lookDX = ddx; in.lookDY = ddy;
        prevSpaceS = spaceNow;

        splayer.update(in, dt, *sphys);
        sphys->step(dt);
        sscene.update(*sphys);

        float camX, camY, camZ, camYaw, camPitch;
        splayer.camera(camX, camY, camZ, camYaw, camPitch);

        int cw, ch; glfwGetFramebufferSize(window, &cw, &ch);
        if (cw != lastWs || ch != lastHs) { lastWs = cw; lastHs = ch; if (cw>0&&ch>0) device->onResize((uint32_t)cw,(uint32_t)ch); }

        const float envYaw = 0.18f * t;   // slow pan: "flying through space"
        device->setCamera(camX, camY, camZ, camYaw, camPitch, 70.0f);
        auto frame = device->beginFrame();
        if (frame.valid) {
            interior.render(*device, frame, sscene);
            windows.setCamera(camX, camY, camZ);
            windows.render(*device, frame, /*viewProj16=*/nullptr, t, envYaw, 0.0f);
        }
        device->endFrame(frame);
    }

    windows.shutdown(*device);
    interior.shutdown(*sphys);
    sphys->shutdown();
    device->shutdown();
    if (window) glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

}} // namespace x3::apphost
