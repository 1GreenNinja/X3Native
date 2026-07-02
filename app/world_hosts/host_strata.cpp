// --world strata host — THE DESCENT: the geological descent zone (facility base
// Y~=0 down to Club 1127 at Y=-200), lifted VERBATIM from the playable-build
// main() inline block (unified-launch merge; reaches shared state via HostContext).
#include "world_host_common.h"
#include "../scene.h"
#include "../strata.h"
#include "../trigger.h"
#include "../player.h"

namespace x3 { namespace apphost {

int hostStrata(HostContext& hc) {
    auto* device = hc.device;
    GLFWwindow* window = hc.window;
    const bool headless = hc.headless;
    const bool screenshot = hc.screenshot;
    const std::string& screenshotPath = hc.screenshotPath;
    const bool shotCamOverride = hc.shotCamOverride;
    const float* shotCam = hc.shotCam;
    const uint32_t W = hc.W;
    const uint32_t H = hc.H;

    // ==== VERBATIM host body ====
    // PHASE 1 = scenic layered rock bands (Foundation -> Granite -> Basalt ->
    // Obsidian -> glowing Crystal Veins -> Magma -> Alien Substrate) the
    // glass-bottom elevator looks out at. PHASE 2 = walkable offshoot cave tunnels
    // at the layer boundaries + an on-foot ledge route all the way down. Two ways
    // in: WALKABLE (windowed) WASD/mouse/Space/Shift/F-noclip, or HEADLESS
    // (--world strata --screenshot <path>) which poses the showcase vantage of the
    // layered descent + glowing depths. Self-contained / LOW-CONFLICT (own Scene +
    // physics), exactly like --world club.
    x3::logInfo("--world strata: building the STRATA descent (facility base Y=0 -> Club 1127 Y=-200)");

    std::unique_ptr<x3::phys::IPhysicsWorld> stphys(x3::phys::createPhysicsWorld());
    if (!stphys->init()) {
        x3::logError("--world strata: physics init failed");
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    x3::game::Scene stscene;
    x3::game::TriggerSystem sttrig;
    x3::game::StrataWorld strata;
    // Build around a shaft at the origin (matches the elevator shaft XZ in the
    // canonical Spire; the elevator descends THROUGH this column + its glass
    // observation wall sees the real bands built here).
    strata.build(stscene, *device, *stphys, sttrig, /*shaftX*/0.0f, /*shaftZ*/0.0f, /*radius*/14.0f);

    // Apply the per-band mood lights (the glow breathes via strata.update()).
    // No sky — this is a deep interior bore.
    const auto& stlights = strata.pointLights();
    device->setPointLights(stlights.data(), (uint32_t)stlights.size());
    { x3::rhi::IRenderDevice::SkyParams sp{}; sp.enabled = false; device->setSkyParams(sp); }

    const x3::phys::Vec3 stspawn = strata.spawn();

    // ===== Headless screenshot path: pose the showcase vantage, settle, grab. =
    if (headless) {
        float cam[5]; strata.showcaseCamera(cam);
        if (shotCamOverride) for (int k = 0; k < 5; ++k) cam[k] = shotCam[k];
        device->setCamera(cam[0], cam[1], cam[2], cam[3], cam[4], 60.0f);
        const int kSettle = 30;
        const float dt = 1.0f / 60.0f;
        const std::string outPath = screenshot ? screenshotPath
                                               : std::string("build/proof/agent_strata.png");
        for (int i = 0; i < kSettle; ++i) {
            glfwPollEvents();
            strata.update(dt, stscene, *device, { cam[0], cam[1], cam[2] });
            stphys->step(dt);
            stscene.update(*stphys);
            device->setCamera(cam[0], cam[1], cam[2], cam[3], cam[4], 60.0f);
            if (i == kSettle - 1) device->armCapture(outPath.c_str());
            auto frame = device->beginFrame();
            if (frame.valid) stscene.render(*device, frame);
            device->endFrame(frame);
        }
        const bool wrote = device->captureFrame(outPath.c_str());
        if (wrote) x3::logInfo("--world strata: wrote screenshot " + outPath);
        else       x3::logError("--world strata: capture FAILED");
        stphys->shutdown();
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return wrote ? 0 : 1;
    }

    // ===== Walkable windowed path. ==========================================
    x3::game::Player stplayer;
    stplayer.spawn(*stphys, stspawn.x, stspawn.y, stspawn.z);

    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
    double lastMX, lastMY; glfwGetCursorPos(window, &lastMX, &lastMY);
    double prevTime = glfwGetTime();
    bool prevSpaceS = false, prevFS = false;
    bool noclipS = false;
    float flyXs = stspawn.x, flyYs = stspawn.y + 1.6f, flyZs = stspawn.z, flyYawS = 3.14159f, flyPitchS = -0.3f;
    x3::logInfo("--world strata: descend THE STRATA — WASD, mouse look, Space jump, LeftShift sprint, F noclip, Esc to quit");
    int lastWs = (int)W, lastHs = (int)H;
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;

        double now = glfwGetTime();
        float dt = (float)(now - prevTime); prevTime = now;
        if (dt > 0.1f) dt = 0.1f;

        double mx, my; glfwGetCursorPos(window, &mx, &my);
        float ddx = (float)(mx - lastMX), ddy = (float)(my - lastMY);
        lastMX = mx; lastMY = my;

        auto kd = [&](int k) { return glfwGetKey(window, k) == GLFW_PRESS; };
        bool spaceNow = kd(GLFW_KEY_SPACE);
        bool fNow = kd(GLFW_KEY_F);
        if (fNow && !prevFS) {
            noclipS = !noclipS;
            if (noclipS) { float yy, pp; stplayer.camera(flyXs, flyYs, flyZs, yy, pp); flyYawS = yy; flyPitchS = pp; }
        }
        prevFS = fNow;

        float camX, camY, camZ, camYaw, camPitch;
        if (!noclipS) {
            x3::game::PlayerInput in;
            if (kd(GLFW_KEY_W)) in.moveFwd    += 1.0f;
            if (kd(GLFW_KEY_S)) in.moveFwd    -= 1.0f;
            if (kd(GLFW_KEY_D)) in.moveStrafe += 1.0f;
            if (kd(GLFW_KEY_A)) in.moveStrafe -= 1.0f;
            in.sprint      = kd(GLFW_KEY_LEFT_SHIFT);
            in.jumpPressed = spaceNow && !prevSpaceS;
            in.lookDX = ddx; in.lookDY = ddy;
            stplayer.update(in, dt, *stphys);
            stplayer.camera(camX, camY, camZ, camYaw, camPitch);
            strata.update(dt, stscene, *device, { camX, camY, camZ });
            stphys->step(dt);
            stscene.update(*stphys);
        } else {
            const float sens = 0.0025f;
            flyYawS += ddx * sens; flyPitchS -= ddy * sens;
            if (flyPitchS >  1.55f) flyPitchS =  1.55f;
            if (flyPitchS < -1.55f) flyPitchS = -1.55f;
            float fx = std::cos(flyPitchS) * std::cos(flyYawS);
            float fy = std::sin(flyPitchS);
            float fz = std::cos(flyPitchS) * std::sin(flyYawS);
            float rl = std::sqrt(fx*fx + fz*fz); if (rl < 1e-4f) rl = 1e-4f;
            float rx = -fz/rl, rz = fx/rl;
            float spd = 6.0f * dt; if (kd(GLFW_KEY_LEFT_SHIFT)) spd *= 3.0f;
            if (kd(GLFW_KEY_W)) { flyXs += fx*spd; flyYs += fy*spd; flyZs += fz*spd; }
            if (kd(GLFW_KEY_S)) { flyXs -= fx*spd; flyYs -= fy*spd; flyZs -= fz*spd; }
            if (kd(GLFW_KEY_D)) { flyXs += rx*spd; flyZs += rz*spd; }
            if (kd(GLFW_KEY_A)) { flyXs -= rx*spd; flyZs -= rz*spd; }
            if (spaceNow) flyYs += spd;
            if (kd(GLFW_KEY_LEFT_CONTROL)) flyYs -= spd;
            camX = flyXs; camY = flyYs; camZ = flyZs; camYaw = flyYawS; camPitch = flyPitchS;
            strata.update(dt, stscene, *device, { camX, camY, camZ });
            stphys->step(dt);
            stscene.update(*stphys);
        }
        prevSpaceS = spaceNow;

        // Forward the player position to the descent triggers (offshoots / club arrival).
        for (uint32_t id : sttrig.update({ camX, camY, camZ })) strata.onTrigger(id);

        int cw, ch; glfwGetFramebufferSize(window, &cw, &ch);
        if (cw != lastWs || ch != lastHs) { lastWs = cw; lastHs = ch; if (cw>0&&ch>0) device->onResize((uint32_t)cw,(uint32_t)ch); }

        device->setCamera(camX, camY, camZ, camYaw, camPitch, 60.0f);
        auto frame = device->beginFrame();
        if (frame.valid) stscene.render(*device, frame);
        device->endFrame(frame);
    }

    stphys->shutdown();
    device->shutdown();
    if (window) glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

}} // namespace x3::apphost
