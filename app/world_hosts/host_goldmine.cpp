// --world goldmine (+ --screenshot) — a minimal, self-contained sandbox for
// visually verifying the gold-mine render port (mine_fx.h/.cpp), modeled on
// host_club.cpp's headless-capture branch. No physics world / audio / crowd —
// GoldMineWorld authors purely static, world-space geometry (no dynamic
// bodies), so Scene::render() alone is enough; there is nothing for
// Scene::update() to sync.
#include "world_host_common.h"
#include "../scene.h"
#include "../mine_fx.h"

namespace x3 { namespace apphost {

int hostGoldMine(HostContext& hc) {
    auto* device = hc.device;
    GLFWwindow* window = hc.window;
    const std::string& worldMode = hc.worldMode;
    const bool headless = hc.headless;
    const bool screenshot = hc.screenshot;
    const std::string& screenshotPath = hc.screenshotPath;
    const bool shotCamOverride = hc.shotCamOverride;
    const float* shotCam = hc.shotCam;

    if (worldMode != "goldmine") return -1;

    x3::logInfo("--world goldmine: building the gold-mine render-port sandbox");

    x3::game::Scene scene;
    x3::game::GoldMineWorld mine;
    mine.build(scene, *device, 0.0f, 0.0f, 0.0f);

    { x3::rhi::IRenderDevice::SkyParams sp{}; sp.enabled = true; device->setSkyParams(sp); }

    float cam[5];
    mine.showcaseCamera(cam);
    if (shotCamOverride) for (int k = 0; k < 5; ++k) cam[k] = shotCam[k];

    if (headless) {
        device->setCamera(cam[0], cam[1], cam[2], cam[3], cam[4], 60.0f);
        const int kSettle = 24;
        const std::string outPath = screenshot ? screenshotPath : std::string("agent_goldmine.png");
        for (int i = 0; i < kSettle; ++i) {
            glfwPollEvents();
            device->setCamera(cam[0], cam[1], cam[2], cam[3], cam[4], 60.0f);
            if (i == kSettle - 1) device->armCapture(outPath.c_str());
            auto frame = device->beginFrame();
            if (frame.valid) scene.render(*device, frame);
            device->endFrame(frame);
        }
        bool wrote = device->captureFrame(outPath.c_str());
        if (wrote) x3::logInfo("--world goldmine: wrote screenshot " + outPath);
        else       x3::logError("--world goldmine: capture FAILED");
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return wrote ? 0 : 1;
    }

    // Walkable windowed path — free-fly camera, no player controller needed
    // (there is no ground collision yet — see mine_fx.h's punch list).
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    double lastMX, lastMY; glfwGetCursorPos(window, &lastMX, &lastMY);
    double prevTime = glfwGetTime();
    float flyX = cam[0], flyY = cam[1], flyZ = cam[2], flyYaw = cam[3], flyPitch = cam[4];
    x3::logInfo("--world goldmine: fly around the gold mine — WASD, mouse look, Esc to quit");

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;

        double now = glfwGetTime();
        float dt = (float)(now - prevTime); prevTime = now;
        if (dt > 0.1f) dt = 0.1f;

        double mx, my; glfwGetCursorPos(window, &mx, &my);
        float ddx = (float)(mx - lastMX), ddy = (float)(my - lastMY);
        lastMX = mx; lastMY = my;
        const float sens = 0.0025f;
        flyYaw += ddx * sens; flyPitch -= ddy * sens;
        if (flyPitch > 1.55f) flyPitch = 1.55f;
        if (flyPitch < -1.55f) flyPitch = -1.55f;

        // Matches the engine's own forward-vector formula EXACTLY (see
        // vk_passes.cpp:1287-1289 / VulkanRenderDevice_internal.h's yaw
        // comment): fwd = (cos(pitch)*cos(yaw), sin(pitch), cos(pitch)*sin(yaw)).
        // yaw is measured from +X, NOT -Z — do not "helpfully" flip signs here.
        float fx = std::cos(flyPitch) * std::cos(flyYaw);
        float fyv = std::sin(flyPitch);
        float fz = std::cos(flyPitch) * std::sin(flyYaw);
        // right = normalize(cross(forward, worldUp)) = normalize((-fz, 0, fx)).
        float rl = std::sqrt(fz * fz + fx * fx); if (rl < 1e-4f) rl = 1e-4f;
        float rx = -fz / rl, rz = fx / rl;
        auto kd = [&](int k) { return glfwGetKey(window, k) == GLFW_PRESS; };
        float spd = 4.0f * dt; if (kd(GLFW_KEY_LEFT_SHIFT)) spd *= 3.0f;
        if (kd(GLFW_KEY_W)) { flyX += fx * spd; flyY += fyv * spd; flyZ += fz * spd; }
        if (kd(GLFW_KEY_S)) { flyX -= fx * spd; flyY -= fyv * spd; flyZ -= fz * spd; }
        if (kd(GLFW_KEY_D)) { flyX += rx * spd; flyZ += rz * spd; }
        if (kd(GLFW_KEY_A)) { flyX -= rx * spd; flyZ -= rz * spd; }

        int cw, ch; glfwGetFramebufferSize(window, &cw, &ch);
        device->setCamera(flyX, flyY, flyZ, flyYaw, flyPitch, 60.0f);
        auto frame = device->beginFrame();
        if (frame.valid) scene.render(*device, frame);
        device->endFrame(frame);
        (void)cw; (void)ch;
    }

    device->shutdown();
    if (window) glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

}} // namespace x3::apphost
