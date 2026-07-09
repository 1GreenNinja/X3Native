// --world elevator host — THE CENTERPIECE (THICK dark-glass core lift showcase,
// Club 1127 at the bottom). RE-HOMED from playable-build's pre-split main()
// inline block (8b6b123 + 14a3097) into the #28 deep-split world-host registry,
// per the ATTENTION_FableAAA fold contract (R-4). Body VERBATIM except: (1)
// HostContext access (raw device pointer), (2) the showcase receives hc.audio
// instead of the pre-split block's hard nullptr — on HFF the host audio system
// is plumbed and null-safe, so the cab's door/ding/keypad cues (the §4 bar) play
// when audio exists and stay graceful-silent headless; the showcase's own
// defaults still keep MUSIC off per Tim's preference.
#include "world_host_common.h"
#include "../scene.h"
#include "../elevator_showcase.h"
#include "../player.h"

#include <cstdlib>   // getenv (X3_ELEV_DISCO shot hook)

namespace x3 { namespace apphost {

int hostElevator(HostContext& hc) {
    auto* device = hc.device;
    GLFWwindow* window = hc.window;
    const std::string& worldMode = hc.worldMode;
    const bool headless = hc.headless;
    const bool screenshot = hc.screenshot;
    const std::string& screenshotPath = hc.screenshotPath;
    const bool shotCamOverride = hc.shotCamOverride;
    const float* shotCam = hc.shotCam;
    const bool elevShot = hc.elevShot;
    const std::string& elevShotDir = hc.elevShotDir;
    const uint32_t W = hc.W, H = hc.H;
    (void)W; (void)H;

    // ==== VERBATIM host body (re-homed) ====
    // ---- THE CENTERPIECE: self-contained THICK dark-glass elevator (--world elevator) ----
    // A SELF-CONTAINED showcase (does NOT touch level1.cpp / the Spire — that layout is
    // being rebuilt in parallel). Builds the premium multi-floor lift (thick heavy doors
    // + chamfered frames + realistic call-panel keypads + a DARK smoked-glass cab with a
    // holo control panel + accent strips + a glass-floor strata descent) and rides it all
    // the way down to Club 1127. The real level later places it via ElevatorShowcase::build
    // + a PlacementSpec (shaft XZ + a floor list). Two ways in: walkable window, or the
    // headless --screenshot-elevator beauty set.
    if (worldMode == "elevator-showcase") {
        x3::logInfo("--world elevator: building THE CENTERPIECE (THICK dark-glass core lift)");

        std::unique_ptr<x3::phys::IPhysicsWorld> ephys(x3::phys::createPhysicsWorld());
        if (!ephys->init()) {
            x3::logError("--world elevator: physics init failed");
            device->shutdown(); if (window) glfwDestroyWindow(window); glfwTerminate(); return 1;
        }
        x3::game::Scene escene;
        x3::game::ElevatorShowcase show;
        x3::game::PlacementSpec espec;   // default premium tower (Club 1127 at the bottom)
        // Music defaults OFF per Tim's preference; the elevator's procedural audio hooks
        // are no-op stubs when the audio system is null, so the showcase stays silent +
        // self-contained (no boot-audio dependency in this decoupled host block).
        if (!show.build(escene, *device, *ephys, espec, hc.audio)) {
            x3::logError("--world elevator: showcase build failed");
            ephys->shutdown(); device->shutdown(); if (window) glfwDestroyWindow(window); glfwTerminate(); return 1;
        }
        // Dark interior: no sky, a low cool ambient so the glass + glow read rich.
        { x3::rhi::IRenderDevice::SkyParams sp{}; sp.enabled = false; device->setSkyParams(sp); }
        const auto& el0 = show.pointLights();
        device->setPointLights(el0.data(), (uint32_t)el0.size());

        // ===== Headless beauty set: interior / exterior / strata-descent =====
        if (headless) {
            const float dt = 1.0f / 60.0f;
            struct Shot { int variant; const char* name; };
            std::vector<Shot> shots;
            if (elevShot) {
                shots = { {0, "elevator_interior.png"}, {1, "elevator_exterior.png"}, {2, "elevator_strata.png"} };
            } else {
                shots = { {0, screenshot ? "" : ""} };   // single --screenshot
            }
            // Drive the lift partway down so the strata shot has rock layers below.
            // X3_ELEV_DISCO=1: enter the 1127 code instead, so the beauty set proves
            // the DISCO cue (ball glow, strobe, magenta terminal/LED, club descent).
            if (std::getenv("X3_ELEV_DISCO")) {
                show.keypadDigit(1); show.keypadDigit(1); show.keypadDigit(2); show.keypadDigit(7);
            } else {
                show.callClub();
            }
            for (int i = 0; i < 90; ++i) {
                show.update(dt, escene, *device, *ephys);
                const auto& l = show.pointLights(); device->setPointLights(l.data(), (uint32_t)l.size());
                ephys->step(dt); escene.update(*ephys);
            }
            bool allOk = true;
            for (const Shot& s : shots) {
                std::string outPath = elevShot ? (elevShotDir + "/" + s.name) : screenshotPath;
                float cam[5]; show.showcaseCamera(s.variant, cam);
                if (shotCamOverride) for (int k = 0; k < 5; ++k) cam[k] = shotCam[k];
                for (int i = 0; i < 18; ++i) {
                    glfwPollEvents();
                    show.update(dt, escene, *device, *ephys);
                    const auto& l = show.pointLights(); device->setPointLights(l.data(), (uint32_t)l.size());
                    ephys->step(dt); escene.update(*ephys);
                    device->setCamera(cam[0], cam[1], cam[2], cam[3], cam[4], 60.0f);
                    if (i == 17) device->armCapture(outPath.c_str());
                    auto frame = device->beginFrame();
                    if (frame.valid) escene.render(*device, frame);
                    device->endFrame(frame);
                }
                bool wrote = device->captureFrame(outPath.c_str());
                if (wrote) x3::logInfo("--world elevator: wrote " + outPath);
                else { x3::logError("--world elevator: capture FAILED " + outPath); allOk = false; }
                if (!elevShot) break;   // single --screenshot path
            }
            ephys->shutdown(); device->shutdown();
            if (window) glfwDestroyWindow(window); glfwTerminate();
            return allOk ? 0 : 1;
        }

        // ===== Walkable windowed path: ride it, hit floors (1-7), code 1127, F noclip ===
        x3::game::Player eplayer;
        const x3::phys::Vec3 esp = show.spawn();
        eplayer.spawn(*ephys, esp.x, esp.y, esp.z);
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        double lastMX, lastMY; glfwGetCursorPos(window, &lastMX, &lastMY);
        double prevTime = glfwGetTime();
        bool prevSpaceE = false, prevFE = false;
        bool prevDigit[10] = {false};
        bool prevClubKey = false;
        x3::logInfo("--world elevator: ride THE CORE LIFT — WASD + mouse, [1..7] select a floor, "
                    "type 1-1-2-7 for the DISCO descent to CLUB 1127, C = club, Space jump, Esc quit");
        int lastWe = (int)W, lastHe = (int)H;
        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();
            if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;
            double now = glfwGetTime();
            float dt = (float)(now - prevTime); prevTime = now;
            if (dt > 0.1f) dt = 0.1f;
            double mx, my; glfwGetCursorPos(window, &mx, &my);
            float ddx = (float)(mx - lastMX), ddy = (float)(my - lastMY);
            lastMX = mx; lastMY = my;
            auto kd = [&](int k){ return glfwGetKey(window, k) == GLFW_PRESS; };

            // Floor-select number keys 1..7 -> callTo (stop index, clamped).
            for (int n = 1; n <= 7; ++n) {
                bool now1 = kd(GLFW_KEY_0 + n);
                if (now1 && !prevDigit[n]) {
                    int stop = std::min(n - 1, show.stopCount() - 1);
                    show.callTo(stop);
                    x3::logInfo("[elevator] floor button " + std::to_string(n) + " -> stop " + std::to_string(stop));
                }
                prevDigit[n] = now1;
            }
            // Keypad digit capture for the 1127 disco code (top-row 1/2/7).
            { bool d1 = kd(GLFW_KEY_KP_1) || false; (void)d1; }
            // 'C' -> straight to the club.
            bool cNow = kd(GLFW_KEY_C);
            if (cNow && !prevClubKey) { show.callClub(); x3::logInfo("[elevator] C -> CLUB 1127"); }
            prevClubKey = cNow;

            bool spaceNow = kd(GLFW_KEY_SPACE);
            x3::game::PlayerInput in;
            if (kd(GLFW_KEY_W)) in.moveFwd    += 1.0f;
            if (kd(GLFW_KEY_S)) in.moveFwd    -= 1.0f;
            if (kd(GLFW_KEY_D)) in.moveStrafe += 1.0f;
            if (kd(GLFW_KEY_A)) in.moveStrafe -= 1.0f;
            in.sprint = kd(GLFW_KEY_LEFT_SHIFT);
            in.jumpPressed = spaceNow && !prevSpaceE;
            in.lookDX = ddx; in.lookDY = ddy;
            eplayer.update(in, dt, *ephys);
            // CARRY: move the rider with the cab BEFORE the physics step (spec §3).
            float edy = show.update(dt, escene, *device, *ephys);
            if (edy != 0.0f && show.playerRiding(eplayer.feet())) {
                x3::phys::Vec3 f = eplayer.feet(); f.y += edy; eplayer.setFeetPosition(*ephys, f);
            }
            const auto& el = show.pointLights(); device->setPointLights(el.data(), (uint32_t)el.size());
            ephys->step(dt); escene.update(*ephys);
            prevSpaceE = spaceNow; (void)prevFE;

            int cw, ch; glfwGetFramebufferSize(window, &cw, &ch);
            if (cw != lastWe || ch != lastHe) { lastWe = cw; lastHe = ch; if (cw>0&&ch>0) device->onResize((uint32_t)cw,(uint32_t)ch); }
            float camX, camY, camZ, camYaw, camPitch;
            eplayer.camera(camX, camY, camZ, camYaw, camPitch);
            device->setCamera(camX, camY, camZ, camYaw, camPitch, 60.0f);
            auto frame = device->beginFrame();
            if (frame.valid) escene.render(*device, frame);
            device->endFrame(frame);
        }
        ephys->shutdown(); device->shutdown();
        if (window) glfwDestroyWindow(window); glfwTerminate();
        return 0;
    }

    // ---- STRATA descent (--world strata) -----------------------------------
    // The geological descent zone: the facility base (Y~=0) down to Club 1127
    // (Y=-200). PHASE 1 = scenic layered rock bands (Foundation -> Granite ->
    // Basalt -> Obsidian -> glowing Crystal Veins -> Magma -> Alien Substrate) the
    // glass-bottom elevator looks out at. PHASE 2 = walkable offshoot cave tunnels
    // at the layer boundaries + an on-foot ledge route all the way down. Two ways
    // in: WALKABLE (windowed) WASD/mouse/Space/Shift/F-noclip, or HEADLESS
    // (--world strata --screenshot <path>) which poses the showcase vantage of the
    // layered descent + glowing depths. Self-contained / LOW-CONFLICT (own Scene +
    // physics), exactly like --world club.
    return -1;
}

}} // namespace x3::apphost
