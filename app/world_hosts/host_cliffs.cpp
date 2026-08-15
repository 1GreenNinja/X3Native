// --world cliffs host — lifted VERBATIM from main() (#28 deep split).
#include "world_host_common.h"
#include "host_shell.h"                 // console (~), menu (ESC), FPS (F3)
#include "../scene.h"
#include "../cliffs.h"

namespace x3 { namespace apphost {

int hostCliffs(HostContext& hc) {
    auto* device = hc.device;
    GLFWwindow* window = hc.window;
    const std::string& worldMode = hc.worldMode;
    const bool headless = hc.headless;
    const bool screenshot = hc.screenshot;
    const std::string& screenshotPath = hc.screenshotPath;
    const bool shotCamOverride = hc.shotCamOverride;
    const float* shotCam = hc.shotCam;
    const uint32_t W = hc.W;
    const uint32_t H = hc.H;

    // ==== VERBATIM host body ====
    if (worldMode == "cliffs") {
        x3::logInfo("--world cliffs: building the above-ground Salvari cliffs finale");
        std::unique_ptr<x3::jobs::IJobSystem> cjobs(x3::jobs::createJobSystem());
        cjobs->init(0);
        std::unique_ptr<x3::phys::IPhysicsWorld> cphys(x3::phys::createPhysicsWorld());
        if (!cphys->init()) {
            x3::logError("--world cliffs: physics init failed");
            device->shutdown();
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return 1;
        }
        x3::game::Scene cscene;
        x3::game::CliffsArea cliffs;
        cliffs.build(cscene, *device, *cphys, cjobs.get());

        // CONTENT WIRING: cascades for the outdoor view depth. `r_csm` was only
        // ever pushed to the device from runDefaultHost, which a --world host
        // replaces -- so this world had cascaded shadows compiled in and
        // unreachable. `--set r_csm 0` restores the legacy single 45 m box.
        applyOutdoorCsm(hc, *device, 400.0f, "cliffs");

        const float dt = 1.0f / 60.0f;

        // ===== Headless capture: warm the ring + waves, pose the vantage, grab. ==
        if (headless) {
            const std::string outPath = screenshot ? screenshotPath
                                                   : std::string("w_cliffs.png");
            float eye[3]; float camYaw = 0.0f, camPitch = 0.0f;
            cliffs.suggestCamera(eye, camYaw, camPitch);
            if (shotCamOverride) {
                eye[0]=shotCam[0]; eye[1]=shotCam[1]; eye[2]=shotCam[2];
                camYaw=shotCam[3]; camPitch=shotCam[4];
            }
            const float focusX = cliffs.padCenter()[0], focusZ = cliffs.padCenter()[2];
            const int kFrames = 220;
            for (int i = 0; i < kFrames; ++i) {
                glfwPollEvents();
                cphys->step(dt);
                // The streamer only enqueues the FULL ring on a focus-tile boundary
                // cross (init seeds the 3x3). Nudge the focus across a tile on frame 1
                // to trigger the ring request, then hold it at the pad so the wide
                // resident set drains in over the warmup window.
                const float fX = (i == 1) ? (focusX + 40.0f) : focusX;
                cliffs.update(cscene, *device, *cphys, dt, fX, focusZ);
                device->setCamera(eye[0], eye[1], eye[2], camYaw, camPitch, 70.0f);
                if (i == kFrames - 1) device->armCapture(outPath.c_str());
                auto frame = device->beginFrame();
                if (frame.valid) cliffs.render(*device, frame, cscene);
                device->endFrame(frame);
            }
            const bool wrote = device->captureFrame(outPath.c_str());
            if (wrote) {
                const x3::rhi::RenderStats st = device->stats();
                char rb[256];
                std::snprintf(rb, sizeof(rb),
                    "--world cliffs: wrote %s | seaLevel=%.1f padY=%.1f actors=%u "
                    "resident=%u draws=%u tris=%u ship=%s",
                    outPath.c_str(), cliffs.seaLevel(), cliffs.padCenter()[1],
                    cliffs.actorCount(), cliffs.residentTiles(), st.drawCalls,
                    st.triangles, cliffs.shipReal() ? "REAL" : "fallback");
                x3::logInfo(rb);
            } else x3::logError("--world cliffs: capture FAILED");

            cliffs.shutdown(cscene, *device, *cphys);
            cjobs->shutdown();
            cphys->shutdown();
            device->shutdown();
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return wrote ? 0 : 1;
        }

        // ===== Walkable windowed path: fly-cam over the cliffs. =================
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        double lastMX, lastMY; glfwGetCursorPos(window, &lastMX, &lastMY);
        double prevTime = glfwGetTime();
        float ceye[3]; float fyaw = 0.0f, fpitch = 0.0f;
        cliffs.suggestCamera(ceye, fyaw, fpitch);
        float fx = ceye[0], fy = ceye[1], fz = ceye[2];
        x3::logInfo("--world cliffs: fly with WASD + mouse, Space/Ctrl up-down, Shift sprint, Esc to quit");
        int lastWc = (int)W, lastHc = (int)H;
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
            float fdt = (float)(now - prevTime); prevTime = now;
            if (fdt > 0.1f) fdt = 0.1f;
            double mx, my; glfwGetCursorPos(window, &mx, &my);
            const float look = shell.inputEnabled() ? 1.0f : 0.0f;   // no mouse-look while typing
            float ddx = (float)(mx - lastMX) * look, ddy = (float)(my - lastMY) * look;
            lastMX = mx; lastMY = my;
            auto kd = [&](int k){ return shell.key(k); };   // false while the console/menu owns input
            const float sens = 0.0025f;
            fyaw += ddx * sens; fpitch -= ddy * sens;
            if (fpitch >  1.55f) fpitch =  1.55f;
            if (fpitch < -1.55f) fpitch = -1.55f;
            float dx = std::cos(fpitch)*std::cos(fyaw), dy = std::sin(fpitch), dz = std::cos(fpitch)*std::sin(fyaw);
            float rl = std::sqrt(dx*dx + dz*dz); if (rl < 1e-4f) rl = 1e-4f;
            float rx = -dz/rl, rz = dx/rl;
            float spd = 8.0f * fdt; if (kd(GLFW_KEY_LEFT_SHIFT)) spd *= 3.0f;
            if (kd(GLFW_KEY_W)) { fx += dx*spd; fy += dy*spd; fz += dz*spd; }
            if (kd(GLFW_KEY_S)) { fx -= dx*spd; fy -= dy*spd; fz -= dz*spd; }
            if (kd(GLFW_KEY_D)) { fx += rx*spd; fz += rz*spd; }
            if (kd(GLFW_KEY_A)) { fx -= rx*spd; fz -= rz*spd; }
            if (kd(GLFW_KEY_SPACE)) fy += spd;
            if (kd(GLFW_KEY_LEFT_CONTROL)) fy -= spd;

            cphys->step(fdt);
            cliffs.update(cscene, *device, *cphys, fdt, fx, fz);

            int cw, ch; glfwGetFramebufferSize(window, &cw, &ch);
            if (cw != lastWc || ch != lastHc) { lastWc=cw; lastHc=ch; if (cw>0&&ch>0) device->onResize((uint32_t)cw,(uint32_t)ch); }
            device->setCamera(fx, fy, fz, fyaw, fpitch, 70.0f);
            auto frame = device->beginFrame();
            if (frame.valid) cliffs.render(*device, frame, cscene);
            shell.draw(frame);
            device->endFrame(frame);
        }
        cliffs.shutdown(cscene, *device, *cphys);
        cjobs->shutdown();
        cphys->shutdown();
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return 0;
    }
}

}} // namespace x3::apphost
