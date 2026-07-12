// --world club host (+ --screenshot-crowd) — lifted VERBATIM from main() (#28).
#include "world_host_common.h"
#include "../scene.h"
#include "../club1127.h"
#include "../crowd.h"
#include "../player.h"
#include "../asset_root.h"
#include "../audio_root.h"                 // resolveAudio (the committed club track)
#include <cstdlib>                          // getenv (X3_CLUB_SEQ clip capture)
#include <cstdio>                           // snprintf (clip frame paths)
#include "engine/audio/IAudioSystem.h"

namespace x3 { namespace apphost {

int hostClub(HostContext& hc) {
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
    const bool ddgiForce = hc.ddgiForce;
    const bool crowdShot = hc.crowdShot;
    const std::string& crowdShotPath = hc.crowdShotPath;

    // ==== VERBATIM host body ====
    if (worldMode == "club") {
        x3::logInfo("--world club: building the full Club 1127 (\"THE DEEP\") at Y=-200");

        // Physics world for the club area (separate from the Level-1 path below).
        std::unique_ptr<x3::phys::IPhysicsWorld> cphys(x3::phys::createPhysicsWorld());
        if (!cphys->init()) {
            x3::logError("--world club: physics init failed");
            device->shutdown();
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return 1;
        }

        x3::game::Scene cscene;
        x3::game::Club1127World club;
        club.build(cscene, *device, *cphys, x3::game::riggedGlbRoot());

        // LIVING WORLD: the dance-floor crowd — 14 club-goers in neon-tinted
        // knots around the floor/bars, bobbing to the beat (CrowdSystem).
        x3::game::CrowdSystem clubCrowd;
        {
            const auto& cs = club.stats();
            x3::game::CrowdConfig ccfg;
            ccfg.count   = 14;
            ccfg.centerX = (cs.roomMinX + cs.roomMaxX) * 0.5f;
            ccfg.centerZ = (cs.roomMinZ + cs.roomMaxZ) * 0.5f;
            ccfg.groundY = x3::game::Club1127World::kClubY;
            ccfg.radius  = std::min(cs.roomMaxX - cs.roomMinX,
                                    cs.roomMaxZ - cs.roomMinZ) * 0.5f - 1.4f;
            // Hangout knots: dance-floor center + toward the DJ end + the bars.
            ccfg.points = { ccfg.centerX,        ccfg.centerZ,
                            ccfg.centerX,        ccfg.centerZ - 7.0f,
                            ccfg.centerX,        ccfg.centerZ + 7.0f,
                            ccfg.centerX - 4.0f, ccfg.centerZ,
                            ccfg.centerX + 4.0f, ccfg.centerZ - 3.0f };
            ccfg.dance    = true;     // they sway/bob to the beat
            // MAX-OUT (Tim: 'work on those dancers'): the pastel BOX agents are
            // retired from the dance floor — Club1127World now owns ten real
            // skinned GLB dancers with beat choreography. Keep a THIN box crowd
            // (4) as dim far-corner wallflowers at the south end so the room
            // feels populated beyond the floor (facility civilians still use
            // the full CrowdSystem elsewhere).
            // Box agents retired ENTIRELY in the club (they wandered back onto
            // the floor as day-glo boxes next to the real dancers).
            ccfg.count = 0;
            (void)clubCrowd;
        }

        // Apply the neon/UV point-light set once (the orbiting spot/ring lights are
        // re-pushed each frame by club.update()). The club has NO sky (deep interior).
        const auto& clights = club.pointLights();
        device->setPointLights(clights.data(), (uint32_t)clights.size());
        { x3::rhi::IRenderDevice::SkyParams sp{}; sp.enabled = false; device->setSkyParams(sp); }

        const x3::phys::Vec3 spawn = club.spawn();

        // ===== Headless screenshot path: pose the showcase camera, settle, grab. =
        if (headless) {
            float cam[5]; club.showcaseCamera(cam);
            // Allow an explicit --shot-cam x,y,z,yaw,pitch override (handy for
            // capturing the caves/boss arena from a custom vantage during verify).
            if (shotCamOverride) for (int k = 0; k < 5; ++k) cam[k] = shotCam[k];
            device->setCamera(cam[0], cam[1], cam[2], cam[3], cam[4], 60.0f);
            // The CROWD proof needs a longer settle so the dancers desync + drift
            // into readable knots before the capture.
            const int kSettle = ddgiForce ? 120 : (crowdShot ? 150 : 24);
            const float dt = 1.0f / 60.0f;
            // Fallback (headless with no --screenshot, i.e. `--smoketest --world club`):
            // a loose scratch grab in the REPO ROOT, which .gitignore already covers.
            // It used to be an absolute "C:/GameDev/X3Native-engine/..." — a path from
            // before the move to D:, so the write always failed, the host reported
            // "capture FAILED", and `--smoketest --world club` exited 1 on a perfectly
            // healthy club. The smoketest gate could not pass. (Pre-existing; found
            // while running the gates for the OLED pass.)
            const std::string outPath = crowdShot   ? crowdShotPath
                                      : screenshot  ? screenshotPath
                                                    : std::string("agent_club.png");
            // X3_CLUB_SEQ=N: after the settle, capture N CONSECUTIVE frames as
            // <out>_0000.png.. for a motion clip (dancers, beat thump, orb spin) —
            // assembled offline into a GIF/MP4. 0/unset = the single still as before.
            int seqFrames = 0;
            if (const char* sq = std::getenv("X3_CLUB_SEQ")) seqFrames = std::atoi(sq);
            for (int i = 0; i < kSettle; ++i) {
                glfwPollEvents();
                club.update(dt, cscene, *device, *cphys);   // ORB spin + spotlight orbit + blacklight pulse + idle props
                clubCrowd.update(dt, cscene);               // the dance-floor crowd
                cphys->step(dt);
                cscene.update(*cphys);
                // Re-pose each frame (scene.update doesn't move the camera).
                device->setCamera(cam[0], cam[1], cam[2], cam[3], cam[4], 60.0f);
                if (i == kSettle - 1 && seqFrames == 0) device->armCapture(outPath.c_str());
                auto frame = device->beginFrame();
                if (frame.valid) {
                    cscene.render(*device, frame);
                    club.drawCharacters(*device, frame, cscene);
                }
                device->endFrame(frame);
            }
            bool wrote = true;
            for (int f = 0; f < seqFrames; ++f) {
                char fp[512];
                std::snprintf(fp, sizeof(fp), "%s_%04d.png",
                              outPath.substr(0, outPath.find_last_of('.')).c_str(), f);
                glfwPollEvents();
                club.update(dt, cscene, *device, *cphys);
                clubCrowd.update(dt, cscene);
                cphys->step(dt);
                cscene.update(*cphys);
                device->setCamera(cam[0], cam[1], cam[2], cam[3], cam[4], 60.0f);
                device->armCapture(fp);
                auto frame = device->beginFrame();
                if (frame.valid) {
                    cscene.render(*device, frame);
                    club.drawCharacters(*device, frame, cscene);
                }
                device->endFrame(frame);
                wrote = device->captureFrame(fp) && wrote;
            }
            if (seqFrames == 0) wrote = device->captureFrame(outPath.c_str());
            if (wrote) x3::logInfo("--world club: wrote screenshot " + outPath);
            else       x3::logError("--world club: capture FAILED");
            cphys->shutdown();
            device->shutdown();
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return wrote ? 0 : 1;
        }

        // ===== Walkable windowed path: full first-person controller + physics. ===
        // THE MUSIC (max-out): the real club track (the
        // tempo every beat-locked pulse in club1127.cpp rides; Descent, ~85.5 BPM) at
        // house volume. Graceful: no device / missing WAV -> silent club.
        std::unique_ptr<x3::audio::IAudioSystem> caudio(x3::audio::createAudioSystem());
        x3::audio::LoopHandle clubTrack{};
        if (caudio && caudio->init()) {
            auto h = caudio->load(x3::game::resolveAudio("music/club_descent.wav"));
            if (h.valid()) clubTrack = caudio->startLoop(h, 0.75f, 1.0f);
        }
        x3::game::Player cplayer;
        cplayer.spawn(*cphys, spawn.x, spawn.y, spawn.z);

        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        double lastMX, lastMY; glfwGetCursorPos(window, &lastMX, &lastMY);
        double prevTime = glfwGetTime();
        bool prevSpaceC = false, prevFC = false;
        bool noclipC = false;
        float flyXc = spawn.x, flyYc = spawn.y + 1.6f, flyZc = spawn.z, flyYawC = 3.14159f, flyPitchC = -0.2f;
        x3::logInfo("--world club: walk THE DEEP at Y=-200 — WASD, mouse look, Space jump, LeftShift sprint, F noclip, Esc to quit");

        int lastWc = (int)W, lastHc = (int)H;
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
            if (fNow && !prevFC) {
                noclipC = !noclipC;
                if (noclipC) { float yy, pp; cplayer.camera(flyXc, flyYc, flyZc, yy, pp); flyYawC = yy; flyPitchC = pp; }
            }
            prevFC = fNow;

            float camX, camY, camZ, camYaw, camPitch;
            if (!noclipC) {
                x3::game::PlayerInput in;
                if (kd(GLFW_KEY_W)) in.moveFwd    += 1.0f;
                if (kd(GLFW_KEY_S)) in.moveFwd    -= 1.0f;
                if (kd(GLFW_KEY_D)) in.moveStrafe += 1.0f;
                if (kd(GLFW_KEY_A)) in.moveStrafe -= 1.0f;
                in.sprint      = kd(GLFW_KEY_LEFT_SHIFT);
                in.jumpPressed = spaceNow && !prevSpaceC;
                in.lookDX = ddx; in.lookDY = ddy;
                cplayer.update(in, dt, *cphys);
                club.update(dt, cscene, *device, *cphys);   // ORB spin + spotlight orbit + blacklight pulse + idle props
                clubCrowd.update(dt, cscene);               // the dance-floor crowd
                cphys->step(dt);
                cscene.update(*cphys);
                cplayer.camera(camX, camY, camZ, camYaw, camPitch);
            } else {
                const float sens = 0.0025f;
                flyYawC += ddx * sens; flyPitchC -= ddy * sens;
                if (flyPitchC >  1.55f) flyPitchC =  1.55f;
                if (flyPitchC < -1.55f) flyPitchC = -1.55f;
                float fx = std::cos(flyPitchC) * std::cos(flyYawC);
                float fy = std::sin(flyPitchC);
                float fz = std::cos(flyPitchC) * std::sin(flyYawC);
                float rl = std::sqrt(fx*fx + fz*fz); if (rl < 1e-4f) rl = 1e-4f;
                float rx = -fz/rl, rz = fx/rl;
                float spd = 6.0f * dt; if (kd(GLFW_KEY_LEFT_SHIFT)) spd *= 3.0f;
                if (kd(GLFW_KEY_W)) { flyXc += fx*spd; flyYc += fy*spd; flyZc += fz*spd; }
                if (kd(GLFW_KEY_S)) { flyXc -= fx*spd; flyYc -= fy*spd; flyZc -= fz*spd; }
                if (kd(GLFW_KEY_D)) { flyXc += rx*spd; flyZc += rz*spd; }
                if (kd(GLFW_KEY_A)) { flyXc -= rx*spd; flyZc -= rz*spd; }
                if (spaceNow) flyYc += spd;
                if (kd(GLFW_KEY_LEFT_CONTROL)) flyYc -= spd;
                club.update(dt, cscene, *device, *cphys);   // ORB spin + spotlight orbit + blacklight pulse + idle props
                clubCrowd.update(dt, cscene);               // the dance-floor crowd
                cphys->step(dt);
                cscene.update(*cphys);
                camX = flyXc; camY = flyYc; camZ = flyZc; camYaw = flyYawC; camPitch = flyPitchC;
            }
            prevSpaceC = spaceNow;

            int cw, ch; glfwGetFramebufferSize(window, &cw, &ch);
            if (cw != lastWc || ch != lastHc) { lastWc = cw; lastHc = ch; if (cw>0&&ch>0) device->onResize((uint32_t)cw,(uint32_t)ch); }

            device->setCamera(camX, camY, camZ, camYaw, camPitch, 60.0f);
            auto frame = device->beginFrame();
            if (frame.valid) {
                cscene.render(*device, frame);
                club.drawCharacters(*device, frame, cscene);
            }
            device->endFrame(frame);
        }

        cphys->shutdown();
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return 0;
    }
}

}} // namespace x3::apphost
