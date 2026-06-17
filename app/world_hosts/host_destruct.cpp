// --world destruct (+ --screenshot-destruct) host — lifted VERBATIM from main()
// (#28 deep monolith split). The body below is byte-identical to the inline
// block; the ONLY change is the alias prelude that binds the shared state out of
// the HostContext so the lifted code reaches it by the same local names.
#include "world_host_common.h"
#include "../destruct_demo.h"          // K-T1 destruction demo
#include "engine/physics/Destruction.h"

namespace x3 { namespace apphost {

int hostDestruct(HostContext& hc) {
    // ---- alias the shared state by its original main()-local names ----
    auto* device = hc.device;
    GLFWwindow* window = hc.window;
    const bool headless = hc.headless;
    const bool screenshot = hc.screenshot;
    const std::string& screenshotPath = hc.screenshotPath;
    const bool shotCamOverride = hc.shotCamOverride;
    const float* shotCam = hc.shotCam;
    const bool bench = hc.bench;
    const uint32_t benchFrames = hc.benchFrames;
    const uint32_t stressCount = hc.stressCount;
    const uint32_t W = hc.W;
    const uint32_t H = hc.H;
    const bool destructShot = hc.destructShot;
    const std::string& destructShotPath = hc.destructShotPath;

    // ==== VERBATIM host body ====
    {
        x3::logInfo("--world destruct: building the destructible-crate showcase");
        std::unique_ptr<x3::phys::IPhysicsWorld> dphys(x3::phys::createPhysicsWorld());
        if (!dphys->init()) {
            x3::logError("--world destruct: physics init failed");
            device->shutdown();
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return 1;
        }
        x3::game::DestructDemo demo;
        demo.build(*device, *dphys, /*numCrates*/4);

        // Outdoor lighting: turn the analytic sky on (backdrop + sun disk) and add
        // bright fill point lights ALONG the crate row + on the camera side so the
        // crate faces toward the vantage + the scattered chunks read clearly (the
        // built-in directional sun comes from +X+Y+Z, so the camera-side faces need
        // fill). Lights span the row at x = -3..4.5, z = 0.
        { x3::rhi::IRenderDevice::SkyParams sp{}; sp.enabled = true; sp.sunIntensity = 1.4f; sp.haze = 0.35f;
          device->setSkyParams(sp); }
        { x3::rhi::PointLight pl[4];
          // Two strong fills on the CAMERA side (-X/+Z) lighting the faces we see.
          pl[0].pos[0]=-2.0f; pl[0].pos[1]=2.5f; pl[0].pos[2]=3.0f; pl[0].range=14.0f;
          pl[0].color[0]=5.0f; pl[0].color[1]=5.0f; pl[0].color[2]=5.4f;
          pl[1].pos[0]= 3.0f; pl[1].pos[1]=2.5f; pl[1].pos[2]=3.0f; pl[1].range=14.0f;
          pl[1].color[0]=5.0f; pl[1].color[1]=4.6f; pl[1].color[2]=4.0f;
          // Two overhead lights so the tumbling chunks catch light from above.
          pl[2].pos[0]=-1.0f; pl[2].pos[1]=4.0f; pl[2].pos[2]=0.0f; pl[2].range=12.0f;
          pl[2].color[0]=3.5f; pl[2].color[1]=3.5f; pl[2].color[2]=3.5f;
          pl[3].pos[0]= 4.0f; pl[3].pos[1]=4.0f; pl[3].pos[2]=0.0f; pl[3].range=12.0f;
          pl[3].color[0]=3.5f; pl[3].color[1]=3.5f; pl[3].color[2]=3.5f;
          device->setPointLights(pl, 4); }

        const float dt = 1.0f / 60.0f;

        // ===== Headless capture: shoot + explode the crates, settle, grab. ======
        if (headless) {
            // Vantage: close + low, framing the crate row (crates span x=-3..4.5 at
            // z=0, y=0.5) from off to the camera side so the intact crates (left) and
            // the freshly shattered, mid-air tumbling chunks (right) both read big.
            float cam[5] = { -5.5f, 1.8f, 3.2f, -0.46f, -0.10f };
            if (shotCamOverride) for (int k = 0; k < 5; ++k) cam[k] = shotCam[k];
            device->setCamera(cam[0], cam[1], cam[2], cam[3], cam[4], 70.0f);
            const std::string outPath = destructShot ? destructShotPath
                                       : (screenshot ? screenshotPath : destructShotPath);

            // ADDITIVE GPU-compute debris layer (K-T2): wire the cheap, large-scale
            // GPU rubble onto the SAME fracture/explosion events that drive the Jolt
            // chunks (the Jolt chunk path is untouched). Each break ALSO emits a GPU
            // debris burst at the impact, simulated + drawn entirely on the GPU. This
            // proves the compute path in the real windowed/screenshot render loop.
            { x3::rhi::IRenderDevice::GpuDebrisParams gp{};
              gp.groundY = 0.0f; gp.restitution = 0.2f; gp.friction = 0.5f;
              gp.linearDamping = 0.3f; gp.sleepFrames = 16;
              device->gpuDebrisConfig(gp); }
            const float debrisTint[4] = { 0.78f, 0.55f, 0.36f, 1.0f };

            // Break the RIGHT crates (3rd + 4th) so the left two stay intact for the
            // before/after contrast, and capture while the chunks are still scattering
            // (modest kicks so the debris stays in frame, not launched to the horizon).
            const int kSettle = 30;       // ~0.5 s after the break: chunks mid-air, near the crates
            for (int i = 0; i < kSettle; ++i) {
                glfwPollEvents();
                // Frame 4: shoot the 3rd crate from the left along +X.
                if (i == 4 && demo.crates().size() >= 3) {
                    const auto& cr = demo.crates();
                    float eye[3] = { cr[2].center[0] - 3.0f, cr[2].center[1], cr[2].center[2] };
                    float dir[3] = { 1.0f, 0.0f, 0.0f };
                    demo.fire(eye, dir, 45.0f);
                    // Additive GPU rubble burst at the crate (hundreds of cheap fragments).
                    float bp[3] = { cr[2].center[0], cr[2].center[1], cr[2].center[2] };
                    device->gpuDebrisSpawnBurst(bp, 600, 4.0f, 6.0f, 0.06f, 0xC0FFEEu);
                }
                // Frame 8: blow up the rightmost crate with an explosion right under it.
                if (i == 8 && demo.crates().size() >= 4) {
                    const auto& cr = demo.crates();
                    float center[3] = { cr[cr.size()-1].center[0], 0.5f, 0.0f };
                    demo.explode(center, 3.0f, 28.0f);
                    float bp[3] = { center[0], 0.6f, center[2] };
                    device->gpuDebrisSpawnBurst(bp, 800, 6.0f, 6.0f, 0.06f, 0x1234567u);
                }
                dphys->step(dt);
                demo.update(dt);
                device->setCamera(cam[0], cam[1], cam[2], cam[3], cam[4], 70.0f);
                if (i == kSettle - 1) device->armCapture(outPath.c_str());
                auto frame = device->beginFrame();
                device->gpuDebrisStep(dt);            // GPU compute integrate
                if (frame.valid) demo.render(frame);  // Jolt chunks (existing path)
                if (frame.valid) device->gpuDebrisDraw(frame, debrisTint); // GPU rubble
                device->endFrame(frame);
            }
            const bool wrote = device->captureFrame(outPath.c_str());
            if (wrote) x3::logInfo("--screenshot-destruct: wrote " + outPath +
                                   " (Jolt debris=" + std::to_string(demo.activeDebris()) +
                                   " GPU debris=" + std::to_string(device->gpuDebrisAliveCount()) + ")");
            else       x3::logError("--screenshot-destruct: capture FAILED");
            demo.shutdown();
            dphys->shutdown();
            device->shutdown();
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return wrote ? 0 : 1;
        }

        // ===== Benchmark with active debris (--world destruct --bench [N] [frames]).
        // Spawn a field of crates, break them all, then run with vsync OFF measuring
        // FPS/CPU/GPU while a large pool of convex chunk bodies simulates + tumbles.
        // Reports the active-debris count so the perf is attributable to destruction.
        if (bench) {
            const uint32_t fieldCrates = stressCount > 0 ? std::min(stressCount, 40u) : 12u;
            // Spawn extra crates in a grid (the build() already made 4 along x).
            for (uint32_t n = 0; n < fieldCrates; ++n) {
                float cx = -6.0f + (float)(n % 8) * 2.2f;
                float cz = -6.0f + (float)(n / 8) * 2.2f;
                // Reuse the demo's shared fracture asset for a fresh destructible per cell.
                x3::phys::DestructibleId id = demo.spawnCrate(cx, 0.5f, cz);
                if (id) { float c[3]={cx,0.5f,cz}; demo.explode(c, 1.5f, 30.0f); }
            }
            dphys->step(dt); demo.update(dt);          // apply the breaks

            const float bx = 0.0f, by = 9.0f, bz = 12.0f, byaw = -1.5708f, bpitch = -0.6f;
            device->setCamera(bx, by, bz, byaw, bpitch, 75.0f);
            const uint32_t warmup = std::min<uint32_t>(60, benchFrames / 4);
            double sumCpu = 0.0, sumGpu = 0.0; uint32_t measured = 0;
            double prevT = glfwGetTime();
            x3::rhi::RenderStats last{};
            for (uint32_t f = 0; f < benchFrames && !glfwWindowShouldClose(window); ++f) {
                glfwPollEvents();
                double nowT = glfwGetTime(); double cpuMs = (nowT - prevT) * 1000.0; prevT = nowT;
                dphys->step(dt); demo.update(dt);
                device->setCamera(bx, by, bz, byaw, bpitch, 75.0f);
                auto frame = device->beginFrame();
                if (frame.valid) demo.render(frame);
                device->endFrame(frame);
                last = device->stats();
                if (f >= warmup) { sumCpu += cpuMs; sumGpu += last.gpuFrameMs; ++measured; }
            }
            const double avgCpu = measured ? sumCpu / measured : 0.0;
            const double avgGpu = measured ? sumGpu / measured : 0.0;
            const double avgFps = (avgCpu > 1e-6) ? (1000.0 / avgCpu) : 0.0;
            char rb[256];
            std::snprintf(rb, sizeof(rb),
                "BENCH-DESTRUCT activeDebris=%u draws=%u tris=%u | FPS=%.1f  CPU=%.3f ms  GPU=%.3f ms  (avg over %u frames)",
                demo.activeDebris(), last.drawCalls, last.triangles, avgFps, avgCpu, avgGpu, measured);
            x3::logInfo(rb);
            demo.shutdown(); dphys->shutdown(); device->shutdown();
            if (window) glfwDestroyWindow(window); glfwTerminate();
            return 0;
        }

        // ===== Walkable windowed path: fly-cam + shoot (LMB) / explode (E). =====
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        double lastMX, lastMY; glfwGetCursorPos(window, &lastMX, &lastMY);
        double prevTime = glfwGetTime();
        float fx = -8.0f, fy = 2.2f, fz = 6.0f, fyaw = -0.6f, fpitch = -0.25f;
        bool prevLMB = false, prevE = false;
        x3::logInfo("--world destruct: fly with WASD + mouse, LMB shoot a crate, E explode, Esc to quit");
        int lastWd = (int)W, lastHd = (int)H;
        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();
            if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;
            double now = glfwGetTime();
            float fdt = (float)(now - prevTime); prevTime = now;
            if (fdt > 0.1f) fdt = 0.1f;
            double mx, my; glfwGetCursorPos(window, &mx, &my);
            float ddx = (float)(mx - lastMX), ddy = (float)(my - lastMY);
            lastMX = mx; lastMY = my;
            auto kd = [&](int k){ return glfwGetKey(window, k) == GLFW_PRESS; };
            const float sens = 0.0025f;
            fyaw += ddx * sens; fpitch -= ddy * sens;
            if (fpitch >  1.55f) fpitch =  1.55f;
            if (fpitch < -1.55f) fpitch = -1.55f;
            float dx = std::cos(fpitch)*std::cos(fyaw), dy = std::sin(fpitch), dz = std::cos(fpitch)*std::sin(fyaw);
            float rl = std::sqrt(dx*dx + dz*dz); if (rl < 1e-4f) rl = 1e-4f;
            float rx = -dz/rl, rz = dx/rl;
            float spd = 6.0f * fdt; if (kd(GLFW_KEY_LEFT_SHIFT)) spd *= 3.0f;
            if (kd(GLFW_KEY_W)) { fx += dx*spd; fy += dy*spd; fz += dz*spd; }
            if (kd(GLFW_KEY_S)) { fx -= dx*spd; fy -= dy*spd; fz -= dz*spd; }
            if (kd(GLFW_KEY_D)) { fx += rx*spd; fz += rz*spd; }
            if (kd(GLFW_KEY_A)) { fx -= rx*spd; fz -= rz*spd; }
            if (kd(GLFW_KEY_SPACE)) fy += spd;
            if (kd(GLFW_KEY_LEFT_CONTROL)) fy -= spd;
            // Shoot: ray from the eye along the look dir.
            bool lmb = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
            if (lmb && !prevLMB) { float eye[3]={fx,fy,fz}, dir[3]={dx,dy,dz}; demo.fire(eye, dir, 70.0f); }
            prevLMB = lmb;
            bool eNow = kd(GLFW_KEY_E);
            if (eNow && !prevE) { float c[3]={fx+dx*4.0f, fy+dy*4.0f, fz+dz*4.0f}; demo.explode(c, 5.0f, 45.0f); }
            prevE = eNow;

            dphys->step(fdt);
            demo.update(fdt);

            int cw, ch; glfwGetFramebufferSize(window, &cw, &ch);
            if (cw != lastWd || ch != lastHd) { lastWd=cw; lastHd=ch; if (cw>0&&ch>0) device->onResize((uint32_t)cw,(uint32_t)ch); }
            device->setCamera(fx, fy, fz, fyaw, fpitch, 65.0f);
            auto frame = device->beginFrame();
            if (frame.valid) demo.render(frame);
            device->endFrame(frame);
        }
        demo.shutdown();
        dphys->shutdown();
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return 0;
    }
}

}} // namespace x3::apphost
