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
#include "host_shell.h"                 // console (~), menu (ESC), FPS (F3)
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
            // driveToY: descend to this cab-center Y before the shot (1e9 = shoot in place
            // at the lobby). The task's four beauty frames: the premium cab interior + the
            // holo panel are shot at the top (doors open, panel lit), then the cab descends
            // for the MID-DESCENT strata frame and the CLUB ARRIVAL frame.
            struct Shot { int variant; const char* name; float driveToY; };
            std::vector<Shot> shots;
            const float clubY = x3::game::ElevatorSystem::kDefaultClubFloorY + 0.18f;
            // THE HERO SET PIECE reads best with the doors SEALED and the cab in the dark
            // shaft — at the lobby the open doors show the bright shaft and auto-exposure
            // crushes the whole cab to black (the recipe's "brightest thing in frame" trap).
            // So every beauty frame is shot MID-DESCENT, doors closed, at full disco boost:
            // the Vegas Sphere, the MV glass, the PA and the luxury all sing in the dark.
            if (elevShot) {
                shots = {
                    {0, "elevator_interior.png",    -8.0f},   // 5-star luxury cab, show playing
                    {4, "elevator_sphere.png",     -16.0f},   // the Vegas Sphere wraparound dome
                    {5, "elevator_musicvideo.png", -26.0f},   // music video on holo glass
                    {6, "elevator_concert_pa.png", -36.0f},   // the concert PA line-array
                    {3, "elevator_holo.png",       -50.0f},   // holo directory + depth readout
                    {2, "elevator_descent.png",    -80.0f},   // mid-descent, full show, wide
                    {1, "elevator_arrival.png", clubY + 5.0f},
                };
            } else {
                shots = { {0, screenshot ? "" : "", 1e9f} };   // single --screenshot
            }
            // Settle a few frames so the holo bakes its glass + the show spins up.
            // X3_ELEV_STAY=1: keep the cab PARKED at its start stop (no descent) so a
            // --shot-cam interior framing is deterministic — the in-cab panel QA shots
            // (OLED directory et al) need a cab that holds still under a fixed camera.
            const bool stayParked = std::getenv("X3_ELEV_STAY") != nullptr;
            for (int i = 0; i < 20; ++i) {
                show.update(dt, escene, *device, *ephys);
                const auto& l = show.pointLights(); device->setPointLights(l.data(), (uint32_t)l.size());
                ephys->step(dt); escene.update(*ephys);
            }
            // The beauty set ALWAYS rides the DISCO descent (code 1127) so the show is at
            // full boost + the dreamy 1/4-speed glide (a savorable ride). X3_ELEV_DISCO
            // forces it for the single --screenshot path too.
            const bool disco = elevShot || std::getenv("X3_ELEV_DISCO") != nullptr;
            bool descentIssued = false;
            // Drive the cab down until its center reaches targetY (headless safety cap).
            auto descendTo = [&](float targetY) {
                if (!descentIssued) {
                    if (disco) { show.keypadDigit(1); show.keypadDigit(1); show.keypadDigit(2); show.keypadDigit(7); }
                    else       { show.callClub(); }
                    descentIssued = true;
                }
                for (int i = 0; i < 12000 && show.cabCenter().y > targetY; ++i) {
                    show.update(dt, escene, *device, *ephys);
                    const auto& l = show.pointLights(); device->setPointLights(l.data(), (uint32_t)l.size());
                    ephys->step(dt); escene.update(*ephys);
                }
            };
            if (stayParked) {
                const x3::phys::Vec3 cc = show.cabCenter();
                x3::logInfo("--world elevator: X3_ELEV_STAY parked, cab center " +
                            std::to_string(cc.x) + " " + std::to_string(cc.y) + " " +
                            std::to_string(cc.z));
            }
            bool allOk = true;
            for (const Shot& s : shots) {
                if (!stayParked && s.driveToY < 1e8f) descendTo(s.driveToY);
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
        x3::logInfo("--world elevator: ride THE CORE LIFT — WASD + mouse, [1..7] at a landing = floor "
                    "select, type 1-1-2-7 WHILE RIDING (or numpad anywhere) for the DISCO descent to "
                    "CLUB 1127, C = club, Space jump, Esc quit");
        int lastWe = (int)W, lastHe = (int)H;
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
            auto kd = [&](int k){ return shell.key(k); };   // false while the console/menu owns input

            // Rider craft (JS checkRider parity): digit keys are CAPTURED BY THE
            // KEYPAD while riding (so typing 1-1-2-7 enters the disco code instead
            // of calling floors 1/1/2/7); standing at a landing they stay floor
            // calls. Numpad digits always feed the keypad (both contexts).
            const bool riding = show.playerRiding(eplayer.feet());
            for (int n = 0; n <= 9; ++n) {
                bool now1 = kd(GLFW_KEY_0 + n) || kd(GLFW_KEY_KP_0 + n);
                if (now1 && !prevDigit[n]) {
                    if (riding || kd(GLFW_KEY_KP_0 + n)) {
                        show.keypadDigit(n);   // per-digit pitched click; 1127 = disco
                    } else if (n >= 1 && n <= 7) {
                        int stop = std::min(n - 1, show.stopCount() - 1);
                        show.callTo(stop);
                        x3::logInfo("[elevator] floor button " + std::to_string(n) +
                                    " -> stop " + std::to_string(stop));
                    }
                }
                prevDigit[n] = now1;
            }
            // Walking up to an idle sealed car opens the doors — no button press.
            show.autoOpenFor(eplayer.feet());
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
            shell.draw(frame);
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
