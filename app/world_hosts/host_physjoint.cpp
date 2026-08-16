// --world physjoint host — lifted VERBATIM from main() (#28 deep split).
#include "world_host_common.h"
#include "host_shell.h"                 // console (~), menu (ESC), FPS (F3)
#include "../mesh_prims.h"

namespace x3 { namespace apphost {

int hostPhysJoint(HostContext& hc) {
    auto* device = hc.device;
    GLFWwindow* window = hc.window;
    const bool headless = hc.headless;
    const bool screenshot = hc.screenshot;
    const std::string& screenshotPath = hc.screenshotPath;
    const bool shotCamOverride = hc.shotCamOverride;
    const float* shotCam = hc.shotCam;

    // ==== VERBATIM host body ====
    {
        x3::logInfo("--world physjoint: building the suspended swinging-cube row");
        std::unique_ptr<x3::phys::IPhysicsWorld> pjphys(x3::phys::createPhysicsWorld());
        if (!pjphys->init()) {
            x3::logError("--world physjoint: physics init failed");
            device->shutdown(); if (window) glfwDestroyWindow(window); glfwTerminate(); return 1;
        }
        // Shared cube mesh + textures + a lit ground.
        std::vector<x3::rhi::MeshVertex> cv; std::vector<uint32_t> ci;
        x3::prims::makeCube(0.5f, cv, ci);
        auto cubeMesh = device->createMesh(cv.data(), (uint32_t)cv.size(), ci.data(), (uint32_t)ci.size());
        auto cubeTexD = x3::prims::makeCheckerRGBA(64, 8, 200, 120, 90, 150, 80, 60);
        auto cubeTex  = device->createTexture(cubeTexD.data(), 64, 64, true);
        auto grTexD = x3::prims::makeCheckerRGBA(64, 8, 150, 150, 160, 60, 62, 74);
        auto grTex  = device->createTexture(grTexD.data(), 64, 64, true);
        x3::prims::PrimMesh g = x3::prims::makeBox(12.0f, 0.25f, 12.0f, 0.0f, -0.25f, 0.0f, 0.25f);
        auto grMesh = device->createMesh(g.verts.data(), (uint32_t)g.verts.size(),
                                         g.index.data(), (uint32_t)g.index.size());
        pjphys->addStaticMesh(g.cverts.data(), (uint32_t)(g.cverts.size()/3),
                              g.cindex.data(), (uint32_t)g.cindex.size());

        // A row of N cubes hung from anchors at y=4. Each cube's TOP is pinned to its
        // anchor so it swings as a pendulum about the pin.
        const int N = 5;
        const float anchorY = 4.0f, half = 0.4f;
        struct Hung { x3::phys::BodyId body; float ax, az; };
        std::vector<Hung> hung;
        for (int i = 0; i < N; ++i) {
            float ax = -4.0f + i * 2.0f, az = 0.0f;
            x3::phys::Vec3 center{ ax, anchorY - 1.2f, az };  // hang 1.2 m below the anchor
            x3::phys::BodyId b = pjphys->addBox(x3::phys::Vec3{half,half,half}, center, 4.0f, x3::phys::Layer::Dynamic);
            x3::phys::Vec3 anchor{ ax, anchorY, az };
            x3::phys::Vec3 attach{ ax, center.y + half, az };  // a point near the top of the cube
            pjphys->addPointConstraint(b, anchor, attach);
            pjphys->setBodyDamping(b, 0.15f, 0.15f);
            hung.push_back({ b, ax, az });
        }
        pjphys->optimizeBroadphase();

        { x3::rhi::IRenderDevice::SkyParams sp{}; sp.enabled = true; sp.sunIntensity = 1.3f; sp.haze = 0.3f;
          device->setSkyParams(sp); }
        { x3::rhi::PointLight pl[2];
          pl[0].pos[0]=0; pl[0].pos[1]=4.0f; pl[0].pos[2]=5.0f; pl[0].range=20.0f;
          pl[0].color[0]=5.0f; pl[0].color[1]=4.8f; pl[0].color[2]=4.4f;
          pl[1].pos[0]=0; pl[1].pos[1]=5.0f; pl[1].pos[2]=-3.0f; pl[1].range=18.0f;
          pl[1].color[0]=3.0f; pl[1].color[1]=3.0f; pl[1].color[2]=3.2f;
          device->setPointLights(pl, 2); }

        const float dt = 1.0f / 60.0f;
        auto drawScene = [&](const x3::rhi::FrameContext& frame) {
            const float white[4] = {1,1,1,1};
            const float idG[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
            device->drawMesh(frame, grMesh, grTex, white, idG);
            const float cubeCol[4] = { 0.95f, 0.8f, 0.7f, 1.0f };
            for (const auto& h : hung) {
                x3::phys::Vec3 p = pjphys->getBodyPosition(h.body);
                float q[4]; pjphys->getBodyRotation(h.body, q);
                // Compose a TRS matrix (quat -> 3x3, then scale to the cube size).
                float x=q[0],y=q[1],z=q[2],w=q[3];
                float m[16] = {
                    (1-2*(y*y+z*z)), (2*(x*y+z*w)),   (2*(x*z-y*w)),   0,
                    (2*(x*y-z*w)),   (1-2*(x*x+z*z)), (2*(y*z+x*w)),   0,
                    (2*(x*z+y*w)),   (2*(y*z-x*w)),   (1-2*(x*x+y*y)), 0,
                    p.x, p.y, p.z, 1 };
                const float s = half * 2.0f / 0.5f;
                m[0]*=s;m[1]*=s;m[2]*=s; m[4]*=s;m[5]*=s;m[6]*=s; m[8]*=s;m[9]*=s;m[10]*=s;
                device->drawMesh(frame, cubeMesh, cubeTex, cubeCol, m);
            }
        };

        // ===== Headless capture: push the row, capture mid-swing. =====
        if (headless) {
            float cam[5] = { 0.0f, 3.0f, 9.0f, -1.5708f, -0.18f };
            if (shotCamOverride) for (int k = 0; k < 5; ++k) cam[k] = shotCam[k];
            const std::string outPath = screenshot ? screenshotPath : std::string("G:/X3Native/captures/physjoint.png");
            const int kFrames = 45;
            for (int i = 0; i < kFrames; ++i) {
                glfwPollEvents();
                if (i == 3) for (auto& h : hung) pjphys->applyImpulse(h.body, x3::phys::Vec3{ 18.0f, 0, 0 });
                pjphys->step(dt);
                device->setCamera(cam[0], cam[1], cam[2], cam[3], cam[4], 65.0f);
                if (i == kFrames - 1) device->armCapture(outPath.c_str());
                auto frame = device->beginFrame();
                if (frame.valid) drawScene(frame);
                device->endFrame(frame);
            }
            const bool wrote = device->captureFrame(outPath.c_str());
            if (wrote) x3::logInfo("--world physjoint: wrote " + outPath);
            else       x3::logError("--world physjoint: capture FAILED");
            device->destroyMesh(cubeMesh); device->destroyMesh(grMesh);
            device->destroyTexture(cubeTex); device->destroyTexture(grTex);
            pjphys->shutdown(); device->shutdown();
            if (window) glfwDestroyWindow(window); glfwTerminate();
            return wrote ? 0 : 1;
        }

        // ===== Walkable windowed path: fly-cam; Space pushes the whole row. =====
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        double lastMX, lastMY; glfwGetCursorPos(window, &lastMX, &lastMY);
        double prevTime = glfwGetTime();
        float fx = 0.0f, fy = 2.5f, fz = 9.0f, fyaw = -1.5708f, fpitch = -0.1f;
        bool prevSpace = false;
        x3::logInfo("--world physjoint: fly WASD + mouse, Space to push the cubes, Esc to quit");
        // Console (~), ESC menu and the FPS/stats overlay. See host_shell.h:
        // the engine has had all three for a long time and 28 of ~31 hosts
        // wired none of them, so the worlds you could actually launch and play
        // were the one place in the engine with no developer tools at all.
        HostShell shell;
        shell.attach(hc);

        while (!glfwWindowShouldClose(window) && !shell.wantQuit()) {
            glfwPollEvents();
            shell.beginFrame();   // ESC opens the menu now; SHIFT+ESC quits
            double now = glfwGetTime(); float fdt = (float)(now - prevTime); prevTime = now;
            if (fdt > 0.1f) fdt = 0.1f;
            double mx, my; glfwGetCursorPos(window, &mx, &my);
            float ddx=(float)(mx-lastMX), ddy=(float)(my-lastMY); lastMX=mx; lastMY=my;
            auto kd = [&](int k){ return shell.key(k); };   // false while the console/menu owns input
            fyaw += ddx*0.0025f; fpitch -= ddy*0.0025f;
            if (fpitch> 1.55f) fpitch= 1.55f; if (fpitch<-1.55f) fpitch=-1.55f;
            float dx=std::cos(fpitch)*std::cos(fyaw), dy=std::sin(fpitch), dz=std::cos(fpitch)*std::sin(fyaw);
            float rl=std::sqrt(dx*dx+dz*dz); if (rl<1e-4f) rl=1e-4f;
            float rx=-dz/rl, rz=dx/rl; float spd=6.0f*fdt; if (kd(GLFW_KEY_LEFT_SHIFT)) spd*=3.0f;
            if (kd(GLFW_KEY_W)){fx+=dx*spd;fy+=dy*spd;fz+=dz*spd;}
            if (kd(GLFW_KEY_S)){fx-=dx*spd;fy-=dy*spd;fz-=dz*spd;}
            if (kd(GLFW_KEY_D)){fx+=rx*spd;fz+=rz*spd;}
            if (kd(GLFW_KEY_A)){fx-=rx*spd;fz-=rz*spd;}
            bool sp = kd(GLFW_KEY_SPACE);
            if (sp && !prevSpace) for (auto& h : hung) pjphys->applyImpulse(h.body, x3::phys::Vec3{ 18.0f, 0, 0 });
            prevSpace = sp;
            pjphys->step(fdt);
            int cw, chh; glfwGetFramebufferSize(window, &cw, &chh);
            if (cw>0&&chh>0) device->onResize((uint32_t)cw,(uint32_t)chh);
            device->setCamera(fx, fy, fz, fyaw, fpitch, 65.0f);
            auto frame = device->beginFrame();
            if (frame.valid) drawScene(frame);
            shell.draw(frame);
            device->endFrame(frame);
        }
        device->destroyMesh(cubeMesh); device->destroyMesh(grMesh);
        device->destroyTexture(cubeTex); device->destroyTexture(grTex);
        pjphys->shutdown(); device->shutdown();
        if (window) glfwDestroyWindow(window); glfwTerminate();
        return 0;
    }
}

}} // namespace x3::apphost
