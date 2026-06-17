// --world ragdoll host — lifted VERBATIM from main() (#28 deep split).
#include "world_host_common.h"
#include "../ragdoll_demo.h"

namespace x3 { namespace apphost {

int hostRagdoll(HostContext& hc) {
    auto* device = hc.device;
    GLFWwindow* window = hc.window;
    const bool headless = hc.headless;
    const bool screenshot = hc.screenshot;
    const std::string& screenshotPath = hc.screenshotPath;
    const bool shotCamOverride = hc.shotCamOverride;
    const float* shotCam = hc.shotCam;

    // ==== VERBATIM host body ====
    {
        x3::logInfo("--world ragdoll: building the ragdoll demo character");
        std::unique_ptr<x3::phys::IPhysicsWorld> rphys(x3::phys::createPhysicsWorld());
        if (!rphys->init()) {
            x3::logError("--world ragdoll: physics init failed");
            device->shutdown(); if (window) glfwDestroyWindow(window); glfwTerminate(); return 1;
        }
        x3::game::RagdollDemo demo;
        demo.build(*device, *rphys);

        { x3::rhi::IRenderDevice::SkyParams sp{}; sp.enabled = true; sp.sunIntensity = 1.3f; sp.haze = 0.3f;
          device->setSkyParams(sp); }
        { x3::rhi::PointLight pl[2];
          pl[0].pos[0]=2.0f; pl[0].pos[1]=3.0f; pl[0].pos[2]=4.0f; pl[0].range=16.0f;
          pl[0].color[0]=5.0f; pl[0].color[1]=4.8f; pl[0].color[2]=4.4f;
          pl[1].pos[0]=-2.0f; pl[1].pos[1]=3.0f; pl[1].pos[2]=2.0f; pl[1].range=16.0f;
          pl[1].color[0]=3.2f; pl[1].color[1]=3.2f; pl[1].color[2]=3.5f;
          device->setPointLights(pl, 2); }

        const float dt = 1.0f / 60.0f;

        // ===== Headless capture: trigger the ragdoll, let it collapse, capture. =====
        if (headless) {
            float cam[5] = { 0.0f, 1.2f, 3.2f, -1.5708f, -0.10f };
            if (shotCamOverride) for (int k = 0; k < 5; ++k) cam[k] = shotCam[k];
            const std::string outPath = screenshot ? screenshotPath : std::string("G:/X3Native/captures/ragdoll.png");
            const int kFrames = 60;   // ~1 s into the collapse
            for (int i = 0; i < kFrames; ++i) {
                glfwPollEvents();
                if (i == 3) demo.ragdollize();
                rphys->step(dt);
                device->setCamera(cam[0], cam[1], cam[2], cam[3], cam[4], 60.0f);
                if (i == kFrames - 1) device->armCapture(outPath.c_str());
                auto frame = device->beginFrame();
                if (frame.valid) demo.render(frame);
                device->endFrame(frame);
            }
            const bool wrote = device->captureFrame(outPath.c_str());
            if (wrote) x3::logInfo("--world ragdoll: wrote " + outPath);
            else       x3::logError("--world ragdoll: capture FAILED");
            demo.shutdown(); rphys->shutdown(); device->shutdown();
            if (window) glfwDestroyWindow(window); glfwTerminate();
            return wrote ? 0 : 1;
        }

        // ===== Walkable windowed path: fly-cam; R ragdolls, T nudges. =====
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        double lastMX, lastMY; glfwGetCursorPos(window, &lastMX, &lastMY);
        double prevTime = glfwGetTime();
        float fx = 0.0f, fy = 1.2f, fz = 3.5f, fyaw = -1.5708f, fpitch = -0.05f;
        bool prevR = false, prevT = false;
        x3::logInfo("--world ragdoll: fly WASD + mouse, R to ragdoll, T to nudge, Esc to quit");
        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();
            if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;
            double now = glfwGetTime(); float fdt = (float)(now - prevTime); prevTime = now;
            if (fdt > 0.1f) fdt = 0.1f;
            double mx, my; glfwGetCursorPos(window, &mx, &my);
            float ddx=(float)(mx-lastMX), ddy=(float)(my-lastMY); lastMX=mx; lastMY=my;
            auto kd = [&](int k){ return glfwGetKey(window, k) == GLFW_PRESS; };
            fyaw += ddx*0.0025f; fpitch -= ddy*0.0025f;
            if (fpitch> 1.55f) fpitch= 1.55f; if (fpitch<-1.55f) fpitch=-1.55f;
            float dx=std::cos(fpitch)*std::cos(fyaw), dy=std::sin(fpitch), dz=std::cos(fpitch)*std::sin(fyaw);
            float rl=std::sqrt(dx*dx+dz*dz); if (rl<1e-4f) rl=1e-4f;
            float rx=-dz/rl, rz=dx/rl; float spd=5.0f*fdt; if (kd(GLFW_KEY_LEFT_SHIFT)) spd*=3.0f;
            if (kd(GLFW_KEY_W)){fx+=dx*spd;fy+=dy*spd;fz+=dz*spd;}
            if (kd(GLFW_KEY_S)){fx-=dx*spd;fy-=dy*spd;fz-=dz*spd;}
            if (kd(GLFW_KEY_D)){fx+=rx*spd;fz+=rz*spd;}
            if (kd(GLFW_KEY_A)){fx-=rx*spd;fz-=rz*spd;}
            bool rNow = kd(GLFW_KEY_R);
            if (rNow && !prevR) demo.ragdollize();
            prevR = rNow;
            bool tNow = kd(GLFW_KEY_T);
            if (tNow && !prevT && demo.ragdoll()) demo.ragdoll()->applyImpulseAll(x3::phys::Vec3{ 2.0f, 1.0f, 0 });
            prevT = tNow;
            rphys->step(fdt);
            int cw, chh; glfwGetFramebufferSize(window, &cw, &chh);
            if (cw>0&&chh>0) device->onResize((uint32_t)cw,(uint32_t)chh);
            device->setCamera(fx, fy, fz, fyaw, fpitch, 60.0f);
            auto frame = device->beginFrame();
            if (frame.valid) demo.render(frame);
            device->endFrame(frame);
        }
        demo.shutdown(); rphys->shutdown(); device->shutdown();
        if (window) glfwDestroyWindow(window); glfwTerminate();
        return 0;
    }
}

}} // namespace x3::apphost
