// ============================================================================
// host_descentslide — THE DESCENT RIDE host (feast Wave 2C). Walkable mouth
// platform in the facility basement (B1, y=0) -> step into the chute mouth to
// ride the coaster-grade descent to the -178 m crystal cavern -> walk the
// cavern. Headless --screenshot honors --shot-cam and logs four SUGGESTED
// feature cams (mouth/overbank/window/cavern, computed from the built spec) so
// capture passes never guess coordinates.
//   * WALK+RIDE (windowed): --world descentslide — WASD walk, ride auto-starts
//     at the chute mouth (A/D steer on-track), Esc quits.
//   * SCREENSHOT (headless): --world descentslide --screenshot [--shot-cam].
//   * GATE: --test-descentslide (headless spec + rider sim; test_registry).
// ============================================================================

#include "../world_hosts.h"
#include "../host_context.h"
#include "host_shell.h"                 // console (~), menu (ESC), FPS (F3)
#include "../scene.h"
#include "../player.h"
#include "../descent_slide.h"
#include "engine/rhi/IRenderDevice.h"
#include "engine/physics/IPhysicsWorld.h"
#include "engine/core/x3_log.h"

#include <GLFW/glfw3.h>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

namespace x3 { namespace apphost {

int hostDescentSlide(HostContext& hc) {
    if (hc.worldMode != "descentslide") return -1;
    auto* device = hc.device;
    GLFWwindow* window = hc.window;

    x3::logInfo("--world descentslide: THE DESCENT — B1 chute mouth to the -178 m crystal cavern");

    std::unique_ptr<x3::phys::IPhysicsWorld> sphys(x3::phys::createPhysicsWorld());
    if (!sphys->init()) {
        x3::logError("--world descentslide: physics init failed");
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    x3::game::Scene sscene;
    x3::game::DescentSlide ride;
    if (!ride.build(*device, sscene, *sphys)) {
        x3::logError("--world descentslide: track build failed");
        sphys->shutdown(); device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }
    const auto& spec = ride.spec();

    // Underground: no sky; moderate ambient (the black-props lesson: props here
    // are matte textured prims, so ambient + the ride's own lights carry them).
    { x3::rhi::IRenderDevice::SkyParams sp{}; sp.enabled = false; device->setSkyParams(sp); }
    { x3::rhi::IRenderDevice::SsaoParams ao{}; ao.enabled = false; device->setSsaoParams(ao); }
    { x3::rhi::IRenderDevice::GiParams gi{}; gi.enabled = false; device->setGiParams(gi); }
    device->setAmbient(0.30f, 0.32f, 0.38f);
    device->setPointLights(ride.lights().data(), (uint32_t)ride.lights().size());

    // CLUB BASS-BLEED HOOK (canon: Club 1127 sits below the cavern floor). Needs a
    // loopable positional one-shot; IAudioSystem music is a single global bed. Log
    // so the beat is never silently dropped.
    x3::logInfo("[descentslide] TODO(audio): muffled 128 BPM bass-bleed at the cavern floor "
                "(Club 1127 below) — needs loopable playSound3D");

    // Suggested feature cams (copy into --shot-cam "x,y,z,yaw,pitch").
    {
        auto camAt = [&](size_t idx, float back, float upO) {
            const auto& f = spec.frames[std::min(idx, spec.frames.size() - 1)];
            const float yaw = std::atan2(f.tan.z, f.tan.x);
            const float pitch = std::asin(std::clamp(f.tan.y, -1.0f, 1.0f));
            std::printf("[descentslide] cam: %.1f,%.1f,%.1f,%.3f,%.3f\n",
                        f.pos.x - f.tan.x * back, f.pos.y + upO, f.pos.z - f.tan.z * back,
                        yaw, pitch * 0.6f);
        };
        size_t overbank = 0, window2 = 0, cavern = spec.frames.size() - 6;
        for (size_t i = 0; i < spec.frames.size(); ++i) {
            if (!overbank && std::fabs(spec.frames[i].bankDeg) > 90.0f) overbank = i;
            if (!window2 && spec.frames[i].type == x3::game::TrackSegType::Burst &&
                spec.frames[i].pos.y < -50.0f) window2 = i;
        }
        std::printf("[descentslide] suggested cams (mouth/overbank/burst/cavern):\n");
        camAt(2, 4.0f, 1.6f);
        camAt(overbank ? overbank : 40, 6.0f, 1.8f);
        camAt(window2 ? window2 : 80, 5.0f, 1.4f);
        camAt(cavern, 8.0f, 2.2f);
    }

    // ===== Headless screenshot =====
    if (hc.headless) {
        // Default: the crest view — down the first drop, depth yawning.
        float cam[5] = { 0.0f, 1.6f, 0.0f, 0.0f, -0.35f };
        {
            const auto& f0 = spec.frames[2];
            cam[0] = f0.pos.x - f0.tan.x * 4.0f; cam[1] = f0.pos.y + 1.6f;
            cam[2] = f0.pos.z - f0.tan.z * 4.0f;
            cam[3] = std::atan2(f0.tan.z, f0.tan.x); cam[4] = -0.4f;
        }
        if (hc.shotCamOverride) for (int k = 0; k < 5; ++k) cam[k] = hc.shotCam[k];
        const std::string outPath = hc.screenshot ? hc.screenshotPath
                                                  : std::string("captures/descentslide.png");
        const float dt = 1.0f / 60.0f;
        const int kSettle = 16;
        for (int i = 0; i < kSettle; ++i) {
            glfwPollEvents();
            sphys->step(dt);
            sscene.update(*sphys);
            device->setCamera(cam[0], cam[1], cam[2], cam[3], cam[4], 74.0f);
            if (i == kSettle - 1) device->armCapture(outPath.c_str());
            auto frame = device->beginFrame();
            if (frame.valid) sscene.render(*device, frame);
            device->endFrame(frame);
        }
        const bool wrote = device->captureFrame(outPath.c_str());
        if (wrote) x3::logInfo("--world descentslide: wrote " + outPath);
        else       x3::logError("--world descentslide: capture FAILED");
        ride.shutdown(*sphys);
        sphys->shutdown(); device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return wrote ? 0 : 1;
    }

    // ===== Windowed: walk the mouth -> ride -> walk the cavern =====
    x3::game::Player splayer;
    const x3::phys::Vec3 mouth = ride.mouth();
    splayer.spawn(*sphys, mouth.x, mouth.y + 0.2f, mouth.z);

    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
    double lastMX, lastMY; glfwGetCursorPos(window, &lastMX, &lastMY);
    double prevTime = glfwGetTime();
    bool prevSpace = false, ridden = false;
    int lastW = 0, lastH = 0; glfwGetFramebufferSize(window, &lastW, &lastH);
    enum class Mode { Walk, Ride } mode = Mode::Walk;
    x3::game::TrackRider rider;
    x3::logInfo("--world descentslide: WASD walk, step into the chute mouth to RIDE (A/D steer), Esc quits");

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

        int cw, ch; glfwGetFramebufferSize(window, &cw, &ch);
        if (cw != lastW || ch != lastH) { lastW = cw; lastH = ch; if (cw>0&&ch>0) device->onResize((uint32_t)cw,(uint32_t)ch); }

        auto kd = [&](int k) { return glfwGetKey(window, k) == GLFW_PRESS; };
        float camX, camY, camZ, camYaw, camPitch, fov = 70.0f;

        if (mode == Mode::Walk) {
            double mx, my; glfwGetCursorPos(window, &mx, &my);
            const float look = shell.inputEnabled() ? 1.0f : 0.0f;   // no mouse-look while typing
            float ddx = (float)(mx - lastMX) * look, ddy = (float)(my - lastMY) * look;
            lastMX = mx; lastMY = my;
            bool spaceNow = kd(GLFW_KEY_SPACE);
            x3::game::PlayerInput in;
            if (kd(GLFW_KEY_W)) in.moveFwd    += 1.0f;
            if (kd(GLFW_KEY_S)) in.moveFwd    -= 1.0f;
            if (kd(GLFW_KEY_D)) in.moveStrafe += 1.0f;
            if (kd(GLFW_KEY_A)) in.moveStrafe -= 1.0f;
            in.sprint = kd(GLFW_KEY_LEFT_SHIFT);
            in.jumpPressed = spaceNow && !prevSpace;
            in.lookDX = ddx; in.lookDY = ddy;
            prevSpace = spaceNow;
            splayer.update(in, dt, *sphys);
            sphys->step(dt);
            sscene.update(*sphys);
            splayer.camera(camX, camY, camZ, camYaw, camPitch);
            // Entry trigger: within 1.6 m of the chute mouth -> RIDE (once).
            const auto& f0 = spec.frames.front();
            const float dxm = camX - f0.pos.x, dzm = camZ - f0.pos.z;
            if (!ridden && dxm*dxm + dzm*dzm < 1.6f*1.6f && std::fabs(camY - f0.pos.y) < 3.0f) {
                mode = Mode::Ride;
                rider = x3::game::TrackRider{};
                x3::logInfo("[descentslide] RIDE START");
            }
        } else {
            const float steer = (kd(GLFW_KEY_D) ? 1.0f : 0.0f) - (kd(GLFW_KEY_A) ? 1.0f : 0.0f);
            float cam3[3];
            rider.tick(spec, dt, steer, cam3, camYaw, camPitch, fov);
            camX = cam3[0]; camY = cam3[1]; camZ = cam3[2];
            sphys->step(dt);
            sscene.update(*sphys);
            if (rider.done) {
                mode = Mode::Walk; ridden = true;
                const x3::phys::Vec3 b = ride.bowl();
                splayer.setFeetPosition(*sphys, b);      // teleport into the cavern
                glfwGetCursorPos(window, &lastMX, &lastMY);
                x3::logInfo("[descentslide] RIDE COMPLETE — walking the crystal cavern");
            }
        }

        device->setCamera(camX, camY, camZ, camYaw, camPitch, fov);
        auto frame = device->beginFrame();
        if (frame.valid) sscene.render(*device, frame);
        shell.draw(frame);
        device->endFrame(frame);
    }

    ride.shutdown(*sphys);
    sphys->shutdown();
    device->shutdown();
    if (window) glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

}} // namespace x3::apphost
