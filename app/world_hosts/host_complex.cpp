// --world complex host (+ --screenshot-complex) — the 7-LEVEL SURVIVAL COMPLEX
// west of / beneath Club 1127. Builds the club (so L1/tunnel + the two entrance
// endpoints exist as one connected world) PLUS the L2-L7 Complex, then either
// walks it first-person or captures a per-level beauty set headlessly.
//
// Mirrors host_club.cpp's structure. The Complex manages its OWN point-light
// budget (reported at build) and is applied on its own for this world so it
// stays well under the device's 64-light cap.
#include "world_host_common.h"
#include "host_shell.h"                 // console (~), menu (ESC), FPS (F3)
#include "../scene.h"
#include "../club1127.h"
#include "../survival_complex.h"
#include "../player.h"
#include "../asset_root.h"
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>

namespace x3 { namespace apphost {

int hostComplex(HostContext& hc) {
    auto* device = hc.device;
    GLFWwindow* window = hc.window;
    const bool headless = hc.headless;
    const bool screenshot = hc.screenshot;
    const std::string& screenshotPath = hc.screenshotPath;
    const bool shotCamOverride = hc.shotCamOverride;
    const float* shotCam = hc.shotCam;

    x3::logInfo("--world complex: building Club 1127 + the 7-LEVEL SURVIVAL COMPLEX west of it");

    std::unique_ptr<x3::phys::IPhysicsWorld> cphys(x3::phys::createPhysicsWorld());
    if (!cphys->init()) {
        x3::logError("--world complex: physics init failed");
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    x3::game::Scene cscene;
    // Build the club (L1 Private Lounge + tunnel + the entrance endpoints) then
    // the descending Complex (L2-L7 + stairwell + elevator + Route-B hall).
    x3::game::Club1127World club;
    club.build(cscene, *device, *cphys, x3::game::riggedGlbRoot());
    x3::game::SurvivalComplex complex;
    const auto& cs = complex.build(cscene, *device, *cphys, x3::game::riggedGlbRoot());
    x3::logInfo("--world complex: Complex point-light budget = " + std::to_string(cs.pointLights) +
                " (device cap 64) — separate space from the club, managed on its own");

    // Apply the COMPLEX light set (we live underground; the club's own neon is far
    // away/culled). Keeps us well under the 64-light cap.
    const auto& lights = complex.pointLights();
    device->setPointLights(lights.data(), (uint32_t)lights.size());

    // Interior atmosphere recipe (same family the club/facility interiors use so
    // surfaces the point lights don't hit still read, not dead-black).
    { x3::rhi::IRenderDevice::SkyParams sp{}; sp.enabled = false; device->setSkyParams(sp); }
    device->setIblProbe(true);
    // GAMMA WALK-BACK (integration/gamma-fold, 2026-07-25): the survival-complex was
    // lit on the pre-sRGB (2x-dark) engine — its IBL fill / ambient / bloom were all
    // propping the bunker up out of the crushed void. The LINEAR-vs-GAMMA fix (5951890b)
    // now lifts those darks ~2.4x on its own; at the old values the corridors read flat
    // and over-bright (measured mean-luma ~90-143 vs the correctly-lit club's ~20-77).
    // Pull the fill/ambient/bloom back the same way the club was (IBL 0.52->0.22,
    // ambient ~halved, bloom 0.20->0.13) so real practical light does the work.
    device->setIblIntensity(0.22f);               // colored ambient fill — readable bunker (was 0.52 pre-gamma)
    device->setIblSpecular(1.0f);
    device->setMetalAmbient(1.0f);
    device->setAmbient(0.036f, 0.034f, 0.030f);   // warm bunker floor (concrete, not gray) — halved post-gamma
    device->setExposure(1.0f);
    device->setBloom(0.13f);                       // let emissives bloom without blowing milky (was 0.20)

    const x3::phys::Vec3 spawn = complex.spawn();

    // ===== Headless capture path =====
    if (headless) {
        const float dt = 1.0f / 60.0f;
        auto settleAndGrab = [&](const float cam[5], const std::string& out) -> bool {
            for (int i = 0; i < 20; ++i) {
                glfwPollEvents();
                complex.update(dt, cscene, *device);
                cphys->step(dt);
                cscene.update(*cphys);
                device->setCamera(cam[0], cam[1], cam[2], cam[3], cam[4], 60.0f);
                if (i == 19) device->armCapture(out.c_str());
                auto frame = device->beginFrame();
                if (frame.valid) {
                    cscene.render(*device, frame);
                    club.drawCharacters(*device, frame, cscene);
                }
                device->endFrame(frame);
            }
            bool wrote = device->captureFrame(out.c_str());
            if (wrote) x3::logInfo("--world complex: wrote " + out);
            else       x3::logError("--world complex: capture FAILED " + out);
            return wrote;
        };

        bool ok = true;
        if (hc.complexShot) {
            // Per-level beauty set → docs/screenshots/complex/.
            const std::string dir = hc.complexShotDir;
            struct Shot { int level; const char* name; };
            const Shot shots[] = {
                { 2, "L2_recreation" }, { 3, "L3_medical_security" },
                { 4, "L4_storage_armory" }, { 5, "L5_water_air" },
                { 6, "L6_power_generators" }, { 7, "L7_hydroponics" },
                { 0, "stairwell" }, { 8, "routeB_hall" },
            };
            for (const auto& sh : shots) {
                float cam[5]; complex.showcaseCamera(sh.level, cam);
                char path[512];
                std::snprintf(path, sizeof(path), "%s/%s.png", dir.c_str(), sh.name);
                ok = settleAndGrab(cam, path) && ok;
            }
        } else {
            // Single fallback still (e.g. --smoketest --world complex).
            float cam[5]; complex.showcaseCamera(7, cam);   // L7 hydroponics hero
            if (shotCamOverride) for (int k = 0; k < 5; ++k) cam[k] = shotCam[k];
            const std::string out = screenshot ? screenshotPath : std::string("agent_complex.png");
            ok = settleAndGrab(cam, out);
        }

        cphys->shutdown();
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return ok ? 0 : 1;
    }

    // ===== Walkable windowed path =====
    x3::game::Player cplayer;
    cplayer.spawn(*cphys, spawn.x, spawn.y, spawn.z);
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
    double lastMX, lastMY; glfwGetCursorPos(window, &lastMX, &lastMY);
    double prevTime = glfwGetTime();
    bool prevSpaceC = false, prevFC = false, noclipC = false;
    float flyXc = spawn.x, flyYc = spawn.y + 1.6f, flyZc = spawn.z, flyYawC = 0.0f, flyPitchC = -0.1f;
    x3::logInfo("--world complex: walk the survival Complex — WASD, mouse look, Space jump, "
                "LeftShift sprint, F noclip, Esc to quit");

    int lastWc = (int)hc.W, lastHc = (int)hc.H;
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
        double mx, my; glfwGetCursorPos(window, &mx, &my);
        const float look = shell.inputEnabled() ? 1.0f : 0.0f;   // no mouse-look while typing
        float ddx = (float)(mx - lastMX) * look, ddy = (float)(my - lastMY) * look;
        lastMX = mx; lastMY = my;
        auto kd = [&](int k) { return glfwGetKey(window, k) == GLFW_PRESS; };
        bool spaceNow = kd(GLFW_KEY_SPACE);
        bool fNow = kd(GLFW_KEY_F);
        if (fNow && !prevFC) {
            noclipC = !noclipC;
            if (noclipC) { float yy, pp; cplayer.camera(flyXc, flyYc, flyZc, yy, pp); flyYawC = yy; flyPitchC = pp; }
        }
        prevFC = fNow;
        float camX, camY, camZ, camYaw, camPitch;
        if (!noclipC) {
            x3::game::PlayerInput in;
            if (kd(GLFW_KEY_W)) in.moveFwd    += 1.0f;
            if (kd(GLFW_KEY_S)) in.moveFwd    -= 1.0f;
            if (kd(GLFW_KEY_D)) in.moveStrafe += 1.0f;
            if (kd(GLFW_KEY_A)) in.moveStrafe -= 1.0f;
            in.sprint      = kd(GLFW_KEY_LEFT_SHIFT);
            in.jumpPressed = spaceNow && !prevSpaceC;
            in.lookDX = ddx; in.lookDY = ddy;
            cplayer.update(in, dt, *cphys);
            complex.update(dt, cscene, *device);
            cphys->step(dt);
            cscene.update(*cphys);
            cplayer.camera(camX, camY, camZ, camYaw, camPitch);
        } else {
            const float sens = 0.0025f;
            flyYawC += ddx * sens; flyPitchC -= ddy * sens;
            if (flyPitchC >  1.55f) flyPitchC =  1.55f;
            if (flyPitchC < -1.55f) flyPitchC = -1.55f;
            float fx = std::cos(flyPitchC) * std::cos(flyYawC);
            float fy = std::sin(flyPitchC);
            float fz = std::cos(flyPitchC) * std::sin(flyYawC);
            float rl = std::sqrt(fx*fx + fz*fz); if (rl < 1e-4f) rl = 1e-4f;
            float rx = -fz/rl, rz = fx/rl;
            float spd = 6.0f * dt; if (kd(GLFW_KEY_LEFT_SHIFT)) spd *= 3.0f;
            if (kd(GLFW_KEY_W)) { flyXc += fx*spd; flyYc += fy*spd; flyZc += fz*spd; }
            if (kd(GLFW_KEY_S)) { flyXc -= fx*spd; flyYc -= fy*spd; flyZc -= fz*spd; }
            if (kd(GLFW_KEY_D)) { flyXc += rx*spd; flyZc += rz*spd; }
            if (kd(GLFW_KEY_A)) { flyXc -= rx*spd; flyZc -= rz*spd; }
            if (spaceNow) flyYc += spd;
            if (kd(GLFW_KEY_LEFT_CONTROL)) flyYc -= spd;
            complex.update(dt, cscene, *device);
            cphys->step(dt);
            cscene.update(*cphys);
            camX = flyXc; camY = flyYc; camZ = flyZc; camYaw = flyYawC; camPitch = flyPitchC;
        }
        prevSpaceC = spaceNow;
        int cw, ch; glfwGetFramebufferSize(window, &cw, &ch);
        if (cw != lastWc || ch != lastHc) { lastWc = cw; lastHc = ch; if (cw>0&&ch>0) device->onResize((uint32_t)cw,(uint32_t)ch); }
        device->setCamera(camX, camY, camZ, camYaw, camPitch, 60.0f);
        auto frame = device->beginFrame();
        if (frame.valid) {
            cscene.render(*device, frame);
            club.drawCharacters(*device, frame, cscene);
        }
        shell.draw(frame);
        device->endFrame(frame);
    }
    cphys->shutdown();
    device->shutdown();
    if (window) glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

}} // namespace x3::apphost
