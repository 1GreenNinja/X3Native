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
#include "host_shell.h"                 // console (~), menu (ESC), FPS (F3)
#include "../scene.h"
#include "../player.h"
#include "../space/ship_interior.h"
#include "../space/ship_windows.h"
#include "../space/ship_interior_art.h"
#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"
#include "engine/core/x3_log.h"

#include <GLFW/glfw3.h>
#include <memory>
#include <string>
#include <vector>

namespace x3 { namespace apphost {

int hostShipWindows(HostContext& hc) {
    // Serves TWO worlds — BOTH now render the PURE procedural glassy-neon cockpit
    // (dark plating + cyan/magenta neon strips + holo consoles). The licensed retro
    // Scifi-Kit GLB overlay is DROPPED per owner directive (2026-07-09: "do NOT want
    // 80s in the Space Ship" — the flyable ship-windows must be sleek neon). The
    // difference is only the entry point; the true-portal moving-space windows are
    // anchored to the procedural cockpit's window openings (interior.manifest()), so
    // they render over the neon interior in both.
    const bool pureInterior = true;   // no GLB art overlay in either world
    if (hc.worldMode != "ship-windows" && hc.worldMode != "ship-interior") return -1;
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

    // THE REAL INTERIOR (integration feast): drape the licensed Scifi Kit Vol 3
    // pieces over the graybox (env_art overlay pattern — graybox keeps collision
    // + is the per-piece fallback). Stations become interactable below.
    // GLB Scifi-Kit art overlay ONLY for the S6 ship-windows world. The pure
    // ship-interior world shows the procedural glassy-neon reskin with no cladding.
    x3::space::ShipInteriorArt art;
    if (!pureInterior && art.build(*device, sscene, interior.manifest()) > 0)
        interior.hideStationMarkers(sscene);   // real consoles replace the graybox cubes

    // No sky (inside the hull). SSAO/GI raster fallback off so a no-RT capture
    // is not black (lane brief; note: on the 3090 Ti RT paths are now live but
    // the fold keeps the branch's proven look byte-faithful — RTAO polish is a
    // separate feast course). Interior ceiling fill + light-bleed uploaded together.
    { x3::rhi::IRenderDevice::SkyParams sp{}; sp.enabled = false; device->setSkyParams(sp); }
    { x3::rhi::IRenderDevice::SsaoParams ao{}; ao.enabled = false; device->setSsaoParams(ao); }
    { x3::rhi::IRenderDevice::GiParams gi{}; gi.enabled = false; device->setGiParams(gi); }
    // NEON POINT-LIGHTS (glassy-neon cyberpunk reskin): a cool CYAN key + a MAGENTA
    // accent in the cockpit (replacing the neutral fill), a cyan helm underglow, and
    // a cool cyan corridor fill. Kept bright enough to read the space without washing
    // out the emissive neon strips/screens.
    std::vector<x3::rhi::PointLight> lights;
    // Cockpit cyan key (from the ceiling).
    { x3::rhi::PointLight pl{}; pl.pos[0]=0.0f; pl.pos[1]=2.7f; pl.pos[2]=0.0f;
      pl.range=10.0f; pl.color[0]=2.4f; pl.color[1]=4.6f; pl.color[2]=5.4f; lights.push_back(pl); }
    // Cockpit magenta accent (off to the side, lower).
    { x3::rhi::PointLight pl{}; pl.pos[0]=1.6f; pl.pos[1]=1.5f; pl.pos[2]=-0.8f;
      pl.range=6.0f; pl.color[0]=4.6f; pl.color[1]=1.2f; pl.color[2]=3.6f; lights.push_back(pl); }
    // Cyan helm underglow at the forward console.
    { x3::rhi::PointLight pl{}; pl.pos[0]=0.0f; pl.pos[1]=0.9f; pl.pos[2]=-2.2f;
      pl.range=4.0f; pl.color[0]=1.6f; pl.color[1]=3.8f; pl.color[2]=4.8f; lights.push_back(pl); }
    // Cool-cyan corridor ceiling fill (the aft room had NO light of its own).
    { x3::rhi::PointLight pl{}; pl.pos[0]=0.0f; pl.pos[1]=2.6f; pl.pos[2]=5.5f;
      pl.range=8.0f; pl.color[0]=2.4f; pl.color[1]=3.2f; pl.color[2]=3.8f; lights.push_back(pl); }
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
        art.shutdown();
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
    x3::logInfo("--world ship-windows: WASD walk, mouse look, Space jump, LeftShift sprint, E interact, Esc to quit");
    bool prevE = false;
    int  activeStation = -1;         // station in range this frame (-1 none)
    std::vector<float> stationPulse(art.stations().size(), 0.0f);

    // Console (~), ESC menu and the FPS/stats overlay. See host_shell.h:
    // the engine has had all three for a long time and 28 of ~31 hosts
    // wired none of them, so the worlds you could actually launch and play
    // were the one place in the engine with no developer tools at all.
    HostShell shell;
    shell.attach(hc);

    while (!glfwWindowShouldClose(window) && !shell.wantQuit()) {
        glfwPollEvents();
        shell.beginFrame();   // ESC opens the menu now; SHIFT+ESC quits

        double now = glfwGetTime();
        float dt = (float)(now - prevTime); prevTime = now;
        if (dt > 0.1f) dt = 0.1f;
        const float t = (float)(now - startTime);

        double mx, my; glfwGetCursorPos(window, &mx, &my);
        const float look = shell.inputEnabled() ? 1.0f : 0.0f;   // no mouse-look while typing
        float ddx = (float)(mx - lastMX) * look, ddy = (float)(my - lastMY) * look;
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
        // ---- Interact: nearest art station within reach gets the E prompt;
        //      pressing E pulses its console screen (and logs the action). ----
        activeStation = -1;
        {
            float best = 2.2f * 2.2f;
            const auto& sts = art.stations();
            for (size_t i = 0; i < sts.size(); ++i) {
                const float dx = sts[i].pos[0] - camX, dz = sts[i].pos[2] - camZ;
                const float d2 = dx*dx + dz*dz;
                if (d2 < best) { best = d2; activeStation = (int)i; }
            }
        }
        const bool eNow = kd(GLFW_KEY_E);
        if (eNow && !prevE && activeStation >= 0) {
            stationPulse[(size_t)activeStation] = 1.0f;
            x3::logInfo("[shipart] station used: " + art.stations()[(size_t)activeStation].kind);
        }
        prevE = eNow;
        // Decay pulses + drive the console screen emissive (base 1.1 + pulse).
        {
            const auto& sts = art.stations();
            for (size_t i = 0; i < sts.size(); ++i) {
                stationPulse[i] = std::max(0.0f, stationPulse[i] - dt * 1.4f);
                const uint32_t id = sts[i].screenEntity;
                if (id != UINT32_MAX && id < sscene.size())
                    sscene.get(id).emissive[3] = 1.1f + 2.4f * stationPulse[i];
            }
        }

        auto frame = device->beginFrame();
        if (frame.valid) {
            interior.render(*device, frame, sscene);
            windows.setCamera(camX, camY, camZ);
            windows.render(*device, frame, /*viewProj16=*/nullptr, t, envYaw, 0.0f);
            if (activeStation >= 0) {
                char prompt[64];
                std::snprintf(prompt, sizeof(prompt), "[E]  %s CONSOLE",
                              art.stations()[(size_t)activeStation].kind.c_str());
                const float cy2[4] = { 0.55f, 0.9f, 1.0f, 0.9f };
                device->drawHudTextF(frame, x3::rhi::FontRole::HudMono, prompt,
                                     (float)lastWs * 0.5f - 90.0f,
                                     (float)lastHs * 0.62f, 20.0f, cy2);
            }
        }
        shell.draw(frame);
        device->endFrame(frame);
    }

    art.shutdown();
    windows.shutdown(*device);
    interior.shutdown(*sphys);
    sphys->shutdown();
    device->shutdown();
    if (window) glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

}} // namespace x3::apphost
