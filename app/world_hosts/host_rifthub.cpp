// --world rifthub host — the RIFTHUB portal hub (Stargate-style gates the player
// walks between to signpost which slice to relaunch into). Clean-room: built only
// from X3Native's own Scene / trigger / mesh_prims + the engine interfaces. NO
// RBDOOM / id Tech / Doom / Quake — or any other game-engine — source consulted.
//
// Structure mirrors host_strata.cpp (Scene + TriggerSystem + Player + point-light
// mood set + headless capture path + windowed walk loop). The ONLY host-specific
// bits are: build the Rifthub, tick() it each frame (portal shimmer + membrane +
// blue core light pulse), forward player-position trigger fires to onTrigger(),
// and surface the HUD prompt via the window title (a dependency-free "HUD line").
#include "world_host_common.h"
#include "host_shell.h"                 // console (~), menu (ESC), FPS (F3)
#include "../scene.h"
#include "../trigger.h"
#include "../rifthub.h"
#include "../player.h"
#include "../ui.h"                          // R8: the console's glowing control surface
#include "../audio_root.h"                  // resolveAudio (committed rifthub SFX)
#include "engine/audio/IAudioSystem.h"      // synth hum/kawoosh/whoosh, 3D-placed

#include <cstdlib>                          // getenv (X3_RIFTHUB_OPEN capture hook)
#include <cmath>                            // shockwave shake
#include <vector>

namespace x3 { namespace apphost {

int hostRifthub(HostContext& hc) {
    auto* device = hc.device;
    GLFWwindow* window = hc.window;
    const std::string& worldMode = hc.worldMode;
    const bool headless = hc.headless;
    const bool screenshot = hc.screenshot;
    const std::string& screenshotPath = hc.screenshotPath;
    const bool shotCamOverride = hc.shotCamOverride;
    const float* shotCam = hc.shotCam;
    const uint32_t W = hc.W, H = hc.H;
    (void)W; (void)H;

    if (worldMode != "rifthub") return -1;

    x3::logInfo("--world rifthub: building the RIFTHUB portal hub (8 Stargate rifts on a 14 m ring)");

    std::unique_ptr<x3::phys::IPhysicsWorld> rhphys(x3::phys::createPhysicsWorld());
    if (!rhphys->init()) {
        x3::logError("--world rifthub: physics init failed");
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    x3::game::Scene rhscene;
    x3::game::TriggerSystem rhtrig;
    x3::game::Rifthub rifthub;
    rifthub.build(rhscene, *device, *rhphys, rhtrig);

    // Interior hall — no sky/sun; the gates' blue cores + the hall's cool
    // overheads light the space (set each frame after tick()). The hub's own
    // atmosphere (fog / grade / ambient / IBL probe / exposure bias) is a
    // single knob in rifthub.cpp so the light balance lives with the art.
    { x3::rhi::IRenderDevice::SkyParams sp{}; sp.enabled = false; device->setSkyParams(sp); }
    rifthub.applyAtmosphere(*device);

    const x3::phys::Vec3 rhspawn = rifthub.spawn();
    const float dt = 1.0f / 60.0f;

    // ===== Headless capture / smoketest path: pose a vantage, warm the anim, grab. =
    if (headless) {
        // A vantage INSIDE the hall (phase C sealed the hub in a 40 m shell —
        // the old z=-22 spot is behind the south wall) looking across the hub
        // center so several gates + the hall dressing are all in frame.
        float cam[5] = { -6.7f, 3.2f, -16.2f, 1.18f, -0.07f };   // between the S/SW gates
        if (shotCamOverride) for (int k = 0; k < 5; ++k) cam[k] = shotCam[k];
        const std::string outPath = screenshot ? screenshotPath
                                               : std::string("w_rifthub.png");
        // OPEN-STATE CAPTURE HOOK (round 5 bug hunt): X3_RIFTHUB_OPEN=1 fires every
        // gate's trigger before the warm-up, so the headless shot lands in the OPEN
        // (throat) state instead of IDLE — the only way to eyeball the activated
        // membrane without walking the level. The warm-up then runs long enough for
        // the 1.6 s kawoosh to fully decay (OPEN, not SURGE). Off by default: the
        // smoketest path is byte-identical without the env var.
        const bool forceOpen = [] {
            const char* v = std::getenv("X3_RIFTHUB_OPEN");
            return v && v[0] == '1';
        }();
        // ROUND 6: X3_RIFTHUB_SURGE=1 fires the same triggers but stops the warm-up
        // MID-KAWOOSH, so the shot lands in the SURGE state (the reference video's
        // vortex-ring flipbook, played once across the 1.6 s flash). The only way to
        // eyeball the escalation without walking into a gate at the right moment.
        const bool forceSurge = [] {
            const char* v = std::getenv("X3_RIFTHUB_SURGE");
            return v && v[0] == '1';
        }();
        if (forceOpen || forceSurge) {
            for (uint32_t i = 0; i < rifthub.portalCount(); ++i)
                rifthub.onTrigger(rifthub.portal(i).triggerId);
            x3::logInfo(forceSurge
                ? "--world rifthub: X3_RIFTHUB_SURGE=1 -> all 8 gates fired (MID-SURGE capture)"
                : "--world rifthub: X3_RIFTHUB_OPEN=1 -> all 8 gates activated (OPEN throat capture)");
        }
        // ROUND 8: X3_RIFTHUB_IMPLODE=1 dials rift 0's console into the killing
        // combination (full power, no containment) and ENGAGES it, then stops the
        // warm-up mid-collapse — the only way to eyeball the implosion without
        // standing at the console and doing it by hand.
        const bool forceImplode = [] {
            const char* v = std::getenv("X3_RIFTHUB_IMPLODE");
            return v && v[0] == '1';
        }();
        if (forceImplode) {
            rifthub.onTrigger(rifthub.portal(0).triggerId);   // open it first
            for (int i = 0; i < 150; ++i) rifthub.tick(dt, rhscene);   // let it settle
            rifthub.applyOutcome(0, x3::game::RiftOutcome::Implosion);
            x3::logInfo("--world rifthub: X3_RIFTHUB_IMPLODE=1 -> rift 1 is collapsing");
        }
        // 240 @ 60 Hz = 4 s > kKawooshDur (settled OPEN); 48 = 0.8 s = mid-surge;
        // 105 = 1.75 s into the 2.4 s collapse (the membrane is eating itself).
        const int kFrames = forceImplode ? 105
                          : (forceSurge ? 48 : (forceOpen ? 240 : 60));
        for (int i = 0; i < kFrames; ++i) {
            glfwPollEvents();
            rifthub.tick(dt, rhscene);
            device->setPointLights(rifthub.pointLights().data(),
                                   (uint32_t)rifthub.pointLights().size());
            rhphys->step(dt);
            device->setCamera(cam[0], cam[1], cam[2], cam[3], cam[4], 65.0f);
            if (i == kFrames - 1) device->armCapture(outPath.c_str());
            auto frame = device->beginFrame();
            if (frame.valid) {
                rhscene.render(*device, frame);
                rifthub.drawFx(*device, frame);   // membrane lightning arcs + spark motes
            }
            device->endFrame(frame);
        }
        const bool wrote = device->captureFrame(outPath.c_str());
        if (wrote) {
            const x3::rhi::RenderStats st = device->stats();
            char rb[256];
            std::snprintf(rb, sizeof(rb),
                "--world rifthub: wrote %s | portals=%u draws=%u tris=%u",
                outPath.c_str(), rifthub.portalCount(), st.drawCalls, st.triangles);
            x3::logInfo(rb);
        } else x3::logError("--world rifthub: capture FAILED");

        rifthub.shutdown(*device);   // free portal meshes BEFORE device shutdown (allocationCount=0)
        rhphys->shutdown();
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return wrote ? 0 : 1;
    }

    // ===== Walkable windowed path: walk between the rifts, step in to activate. ==
    x3::game::Player rhplayer;
    rhplayer.spawn(*rhphys, rhspawn.x, rhspawn.y, rhspawn.z);

    // ===== Audio: synth hum bed + kawoosh/whoosh events (step 5) =====
    // Own an audio system (mirrors host_club/host_drive). The three SFX are
    // procedurally-synthesized, committed WAVs under assets/audio/rifthub/ —
    // resolveAudio finds them on a fresh clone; a missing device / WAV loads
    // graceful-silent (no crash). Each portal gets a subtle 3D idle HUM loop
    // (the dormant gate); the kawoosh fires on activation, the whoosh on
    // re-entering an already-activated gate (the relaunch latch).
    std::unique_ptr<x3::audio::IAudioSystem> rhaudio(x3::audio::createAudioSystem());
    x3::audio::SoundHandle sndKawoosh, sndWhoosh;
    std::vector<x3::audio::LoopHandle> humLoops;
    if (rhaudio && rhaudio->init()) {
        x3::audio::SoundHandle sndHum = rhaudio->load(x3::game::resolveAudio("rifthub/rifthub_hum.wav"));
        sndKawoosh = rhaudio->load(x3::game::resolveAudio("rifthub/rifthub_kawoosh.wav"));
        sndWhoosh  = rhaudio->load(x3::game::resolveAudio("rifthub/rifthub_whoosh.wav"));
        if (sndHum.valid()) {
            humLoops.reserve(rifthub.portalCount());
            for (uint32_t i = 0; i < rifthub.portalCount(); ++i) {
                const auto& pp = rifthub.portal(i);
                humLoops.push_back(rhaudio->startLoop3D(sndHum, pp.worldPos.x, /*ring Y*/1.8f,
                                                        pp.worldPos.z, /*vol*/0.22f, /*pitch*/1.0f));
            }
        }
    }
    int rhInside = -1;   // portal index the player is currently standing in (-1 = none)

    // ===== ROUND 8: the RIFT CONSOLE =========================================
    // The player walks up to a rift's hanging holoterminal, presses [E], and gets a
    // control surface: glowing sliders + rotary dials + a typed TARGET field.
    //
    // INPUT DISCIPLINE (the cell-terminal rule, and it is not optional): while the
    // console is OPEN, ALL input belongs to it. The player does not move, does not
    // look, does not shoot — typing "club" into TARGET must not walk you into the
    // portal. Everything below is gated on `conOpen`.
    x3::ui::UiContext rhui;
    bool conOpen = false, prevE = false, prevBack = false, prevEnter = false;
    bool prevMouse = false;
    // GLFW pushes printable characters through a char callback; stash them in a
    // per-frame buffer the UI drains (the same shape as the cell terminal's entry).
    static char s_typed[x3::ui::UiInput::kMaxTyped];
    static int  s_typedN = 0;
    glfwSetCharCallback(window, [](GLFWwindow*, unsigned int cp) {
        if (cp >= 32 && cp < 127 && s_typedN < x3::ui::UiInput::kMaxTyped)
            s_typed[s_typedN++] = (char)cp;
    });

    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
    double lastMX, lastMY; glfwGetCursorPos(window, &lastMX, &lastMY);
    double prevTime = glfwGetTime();
    bool prevSpaceR = false, prevFR = false;
    bool noclipR = false;
    float flyXr = rhspawn.x, flyYr = rhspawn.y + 1.6f, flyZr = rhspawn.z, flyYawR = 0.0f, flyPitchR = -0.05f;
    x3::logInfo("--world rifthub: walk the RIFTHUB — WASD, mouse look, Space jump, LeftShift sprint, F noclip, Esc to quit");
    std::string lastTitle;
    int lastWr = (int)W, lastHr = (int)H;
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

        auto kd = [&](int k) { return glfwGetKey(window, k) == GLFW_PRESS; };
        bool spaceNow = kd(GLFW_KEY_SPACE);
        bool fNow = kd(GLFW_KEY_F);
        if (fNow && !prevFR) {
            noclipR = !noclipR;
            if (noclipR) { float yy, pp; rhplayer.camera(flyXr, flyYr, flyZr, yy, pp); flyYawR = yy; flyPitchR = pp; }
        }
        prevFR = fNow;

        float camX, camY, camZ, camYaw, camPitch;
        if (!noclipR) {
            x3::game::PlayerInput in;
            // ---- INPUT REDIRECT: while the console is OPEN it owns EVERYTHING.
            // No WASD, no mouse look, no jump, no sprint. Typing "club" into the
            // TARGET field must not walk the player into the portal. (Same rule the
            // cell holo-terminal / keypad already enforces.)
            if (!conOpen) {
                if (kd(GLFW_KEY_W)) in.moveFwd    += 1.0f;
                if (kd(GLFW_KEY_S)) in.moveFwd    -= 1.0f;
                if (kd(GLFW_KEY_D)) in.moveStrafe += 1.0f;
                if (kd(GLFW_KEY_A)) in.moveStrafe -= 1.0f;
                in.sprint      = kd(GLFW_KEY_LEFT_SHIFT);
                in.jumpPressed = spaceNow && !prevSpaceR;
                in.lookDX = ddx; in.lookDY = ddy;
            }
            rhplayer.update(in, fdt, *rhphys);
            rhplayer.camera(camX, camY, camZ, camYaw, camPitch);
        } else if (conOpen) {
            camX = flyXr; camY = flyYr; camZ = flyZr; camYaw = flyYawR; camPitch = flyPitchR;
        } else {
            const float sens = 0.0025f;
            flyYawR += ddx * sens; flyPitchR -= ddy * sens;
            if (flyPitchR >  1.55f) flyPitchR =  1.55f;
            if (flyPitchR < -1.55f) flyPitchR = -1.55f;
            float fx = std::cos(flyPitchR) * std::cos(flyYawR);
            float fy = std::sin(flyPitchR);
            float fz = std::cos(flyPitchR) * std::sin(flyYawR);
            float rl = std::sqrt(fx*fx + fz*fz); if (rl < 1e-4f) rl = 1e-4f;
            float rx = -fz/rl, rz = fx/rl;
            float spd = 6.0f * fdt; if (kd(GLFW_KEY_LEFT_SHIFT)) spd *= 3.0f;
            if (kd(GLFW_KEY_W)) { flyXr += fx*spd; flyYr += fy*spd; flyZr += fz*spd; }
            if (kd(GLFW_KEY_S)) { flyXr -= fx*spd; flyYr -= fy*spd; flyZr -= fz*spd; }
            if (kd(GLFW_KEY_D)) { flyXr += rx*spd; flyZr += rz*spd; }
            if (kd(GLFW_KEY_A)) { flyXr -= rx*spd; flyZr -= rz*spd; }
            if (spaceNow) flyYr += spd;
            if (kd(GLFW_KEY_LEFT_CONTROL)) flyYr -= spd;
            camX = flyXr; camY = flyYr; camZ = flyZr; camYaw = flyYawR; camPitch = flyPitchR;
        }
        prevSpaceR = spaceNow;

        // ---- [E]: open / close the rift console in front of you ---------------
        {
            const bool eNow = kd(GLFW_KEY_E);
            if (eNow && !prevE) {
                if (conOpen) {
                    rifthub.closeConsole();
                    conOpen = false;
                    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
                } else {
                    const int idx = rifthub.consoleInRange({ camX, camY, camZ });
                    if (idx >= 0) {
                        rifthub.openConsole(idx);
                        conOpen = true;
                        // Free the cursor: the console is a DRAGGABLE control surface
                        // (sliders + rotary dials), so the player needs a pointer.
                        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
                        glfwGetCursorPos(window, &lastMX, &lastMY);
                    }
                }
            }
            prevE = eNow;
        }

        // Animate the portals, then push the per-frame lights (the gate cores + the
        // hall rig, which the ALARM strobes red during a catastrophe).
        rifthub.tick(fdt, rhscene);
        device->setPointLights(rifthub.pointLights().data(),
                               (uint32_t)rifthub.pointLights().size());
        // ---- TEMPORAL RIFT: the SIM runs on distorted time ---------------------
        // The rift's own countdown lives on the RAW dt inside tick(); everything the
        // player and the world DO runs on this one. Slow-motion with a stutter.
        const float simDt = fdt * rifthub.timeScale();
        rhphys->step(simDt);
        rhscene.update(*rhphys);

        // Forward the player position to the rift triggers (latch "rift activated").
        // A trigger fires ONCE (first entry) -> that is the KAWOOSH (activation).
        for (uint32_t id : conOpen ? std::vector<uint32_t>{}
                                   : rhtrig.update({ camX, camY, camZ })) {
            rifthub.onTrigger(id);
            if (rhaudio && sndKawoosh.valid()) {
                for (uint32_t i = 0; i < rifthub.portalCount(); ++i) {
                    const auto& pp = rifthub.portal(i);
                    if (pp.triggerId == id) {
                        rhaudio->playSound3D(sndKawoosh, pp.worldPos.x, 1.8f, pp.worldPos.z, /*vol*/0.9f);
                        break;
                    }
                }
            }
        }

        // Re-entry WHOOSH (the relaunch latch): rising edge of stepping INTO an
        // ALREADY-activated gate (the trigger won't re-fire, so detect it here).
        {
            int nowInside = -1;
            const float kInsideR2 = 2.5f * 2.5f;   // matches the trigger footprint
            for (uint32_t i = 0; i < rifthub.portalCount(); ++i) {
                const auto& pp = rifthub.portal(i);
                const float dx = camX - pp.worldPos.x, dz = camZ - pp.worldPos.z;
                if (dx*dx + dz*dz < kInsideR2) { nowInside = (int)i; break; }
            }
            if (nowInside != rhInside) {
                if (nowInside >= 0 && rifthub.portal((uint32_t)nowInside).activated &&
                    rhaudio && sndWhoosh.valid()) {
                    const auto& pp = rifthub.portal((uint32_t)nowInside);
                    rhaudio->playSound3D(sndWhoosh, pp.worldPos.x, 1.8f, pp.worldPos.z, /*vol*/0.8f);
                }
                rhInside = nowInside;
            }
        }

        // Drive the audio listener from the camera + pump the mixer.
        if (rhaudio) { rhaudio->setListener(camX, camY, camZ, camYaw, camPitch); rhaudio->update(fdt); }

        // HUD line (dependency-free): the nearest-rift prompt as the window title.
        std::string prompt;
        std::string title;
        if (!rifthub.alarm().empty()) {
            title = "RIFTHUB  |  *** " + rifthub.alarm() + " ***";
        } else if (conOpen) {
            title = "RIFTHUB  |  CONSOLE OPEN  -  [E] to step back";
        } else if (rifthub.consoleInRange({ camX, camY, camZ }) >= 0) {
            title = "RIFTHUB  |  [E] operate the rift console";
        } else {
            title = rifthub.hudPromptForEye({ camX, camY, camZ }, prompt)
                      ? ("RIFTHUB  |  " + prompt)
                      : std::string("RIFTHUB  |  walk to a rift");
        }
        if (title != lastTitle) { glfwSetWindowTitle(window, title.c_str()); lastTitle = title; }

        int cw, ch; glfwGetFramebufferSize(window, &cw, &ch);
        if (cw != lastWr || ch != lastHr) { lastWr = cw; lastHr = ch; if (cw>0&&ch>0) device->onResize((uint32_t)cw,(uint32_t)ch); }

        // ---- ROOM WARP's lens + the IMPLOSION shockwave -----------------------
        const float fov = 65.0f + rifthub.fovOffset();
        const float sh  = rifthub.shake();
        float shx = 0.0f, shy = 0.0f, shz = 0.0f;
        if (sh > 0.0f) {
            const float t = (float)now * 47.0f;
            shx = std::sin(t * 1.7f) * sh;
            shy = std::sin(t * 2.3f) * sh * 0.7f;
            shz = std::cos(t * 1.9f) * sh;
        }
        device->setCamera(camX + shx, camY + shy, camZ + shz, camYaw, camPitch, fov);
        auto frame = device->beginFrame();
        if (frame.valid) {
            rhscene.render(*device, frame);
            rifthub.drawFx(*device, frame);   // membrane lightning arcs + spark motes

            // ---- THE CONSOLE (drawn over the live frame) ----------------------
            x3::ui::UiInput uin{};
            double cmx, cmy; glfwGetCursorPos(window, &cmx, &cmy);
            uin.mouseX = (float)cmx;
            uin.mouseY = (float)cmy;
            const bool mDown = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
            uin.mouseDown    = conOpen && mDown;
            uin.mousePressed = conOpen && mDown && !prevMouse;
            prevMouse = mDown;
            if (conOpen) {
                for (int k = 0; k < s_typedN; ++k) uin.typed[k] = s_typed[k];
                uin.typedCount = s_typedN;
                const bool bNow = kd(GLFW_KEY_BACKSPACE);
                const bool eNow2 = kd(GLFW_KEY_ENTER);
                uin.backspace = bNow  && !prevBack;
                uin.enter     = eNow2 && !prevEnter;
                prevBack = bNow; prevEnter = eNow2;
                rhui.begin(*device, frame, uin);
                rifthub.updateConsole(rhui, fdt);
                rhui.end();
            }
            s_typedN = 0;   // drain the char queue every frame, open or not

            // ---- Damage flash (the implosion's shockwave HURTS) ---------------
            const float df = rifthub.damageFlash();
            if (df > 0.0f) {
                uint32_t hw = 0, hh = 0;
                device->hudSize(hw, hh);
                const float red[4] = { 0.85f, 0.06f, 0.04f, df * 0.55f };
                device->drawHudQuad(frame, 0, 0, (float)hw, (float)hh, red);
            }
        }
        shell.draw(frame);
        device->endFrame(frame);
    }

    if (rhaudio) {
        for (auto h : humLoops) rhaudio->stopLoop(h);
        rhaudio->shutdown();
    }
    rifthub.shutdown(*device);
    rhphys->shutdown();
    device->shutdown();
    if (window) glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

}} // namespace x3::apphost
