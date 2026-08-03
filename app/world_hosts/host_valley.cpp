// --world valley host (+ --screenshot-ecology) — lifted VERBATIM from main() (#28).
#include "world_host_common.h"
#include "../scene.h"
#include "../player.h"
#include "../terrain.h"
#include "../valley.h"
#include "../ecology.h"
#include "../tod.h"
#include "../asset_root.h"

namespace x3 { namespace apphost {

int hostValley(HostContext& hc) {
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
    const bool ecologyShot = hc.ecologyShot;
    const std::string& ecologyShotPath = hc.ecologyShotPath;

    // ==== VERBATIM host body ====
    if (worldMode == "valley") {
        x3::logInfo("--world valley: building Crystal Valleys (Act 2, L15 — open biome)");

        std::unique_ptr<x3::phys::IPhysicsWorld> vphys(x3::phys::createPhysicsWorld());
        if (!vphys->init()) {
            x3::logError("--world valley: physics init failed");
            device->shutdown();
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return 1;
        }

        // Streamed terrain around the valley + analytic sky (same as --world terrain).
        std::unique_ptr<x3::jobs::IJobSystem> vjobs(x3::jobs::createJobSystem());
        vjobs->init(0);
        x3::game::Scene vscene;
        const x3::game::TerrainConfig& vcfg = x3::game::worldTerrainConfig();
        {
            x3::rhi::IRenderDevice::SkyParams sp{};
            sp.enabled = true;
            sp.sunDir[0] = 0.4f; sp.sunDir[1] = 1.0f; sp.sunDir[2] = 0.3f;
            sp.sunColor[0] = 1.0f; sp.sunColor[1] = 0.97f; sp.sunColor[2] = 0.92f;
            sp.sunIntensity = 1.0f; sp.haze = 0.5f; sp.exposure = 1.0f;
            device->setSkyParams(sp);
        }
        // CONTENT WIRING: cascades for the outdoor view depth. `r_csm` was only
        // ever pushed to the device from runDefaultHost, which a --world host
        // REPLACES -- so this world had cascaded shadows compiled in and
        // unreachable, and everything past the legacy 45 m box cast nothing.
        // `--set r_csm 0` restores that legacy single cascade exactly.
        applyOutdoorCsm(hc, *device, 350.0f, "valley");
        x3::game::TerrainStreamer vstream;
        // Seed the residency ring at the origin so the valley content has ground.
        vstream.init(vscene, *device, *vphys, vjobs.get(), vcfg, 0.0f, 0.0f, /*radius=*/8);

        // Build the valley content onto the streamed terrain.
        x3::game::ValleyWorld valley;
        valley.build(vscene, *device, *vphys, x3::game::riggedGlbRoot());

        // LIVING WORLD: the ambient ecology rides the valley's open biome —
        // grazer herds + crystal stalkers + Dominion patrol shifts, anchored
        // around the valley spawn, riding the streamed-terrain height field.
        x3::game::AmbientEcology vecology;
        x3::game::TimeOfDay vtod;   // drives the patrol day/night shift schedule
        {
            x3::game::EcoConfig ecoCfg =
                x3::game::loadEcologyConfig(x3::game::ecologyJsonPath());
            const x3::phys::Vec3 anchor = valley.spawn();
            for (auto& sp : ecoCfg.species) {
                sp.regionX += anchor.x; sp.regionZ += anchor.z;
                for (size_t w = 0; w + 1 < sp.waypoints.size(); w += 2) {
                    sp.waypoints[w + 0] += anchor.x;
                    sp.waypoints[w + 1] += anchor.z;
                }
            }
            vecology.setGroundFn([](float x, float z) {
                return x3::game::terrainHeightAtWorld(x, z);
            });
            vecology.build(ecoCfg, vscene, *device);
            vtod.setDayFraction(0.35f);   // start mid-morning: day patrol on duty
        }

        // Crystal point lights, and the lake water plane.
        const auto& vlights = valley.pointLights();
        device->setPointLights(vlights.data(), (uint32_t)vlights.size());
        const float vSeaLevel = valley.waterSeaLevel();
        auto applyWater = [&](float t) {
            x3::rhi::IRenderDevice::WaterParams wp{};
            wp.enabled = true; wp.seaLevel = vSeaLevel; wp.time = t;
            wp.amplitude = 0.4f; wp.steepness = 0.5f; wp.waveLength = 12.0f; wp.speed = 1.0f;
            wp.deepColor[0] = 0.02f; wp.deepColor[1] = 0.08f; wp.deepColor[2] = 0.12f;
            wp.shallowColor[0] = 0.10f; wp.shallowColor[1] = 0.34f; wp.shallowColor[2] = 0.40f;
            wp.sunDir[0] = 0.4f; wp.sunDir[1] = 1.0f; wp.sunDir[2] = 0.3f;
            wp.specular = 14.0f; wp.fresnel = 0.02f;
            device->setWaterParams(wp);
        };

        const x3::phys::Vec3 vspawn = valley.spawn();

        // ===== Headless screenshot path: pose the showcase camera, settle, grab. =
        if (headless) {
            float cam[5]; valley.showcaseCamera(cam);
            // The ECOLOGY proof shot frames the grazer herd from a low vantage
            // and stages the predator strike mid-settle so the capture catches
            // the kill + the herd scattering (the living-world demo moment).
            float herdX = 0.0f, herdZ = 0.0f;
            uint32_t stagedPredator = 0xFFFFFFFFu;
            if (ecologyShot) {
                const x3::phys::Vec3 anchor = valley.spawn();
                herdX = anchor.x + 30.0f; herdZ = anchor.z;   // default-cast herd region
                const float hy = x3::game::terrainHeightAtWorld(herdX, herdZ);
                cam[0] = herdX - 9.0f; cam[1] = hy + 4.5f; cam[2] = herdZ + 9.0f;
                cam[3] = std::atan2(herdZ - cam[2], herdX - cam[0]);   // yaw toward the herd
                cam[4] = -0.30f;
                // Park every predator far outside the soft radius (inactive) so
                // the herd SETTLES during the stream-in; one is staged back in
                // late to produce the strike right at the capture frame.
                for (uint32_t k = 0; k < vecology.agentCount(); ++k)
                    if (vecology.config().species[vecology.agent(k).species].archetype
                            == x3::game::EcoArchetype::Predator) {
                        vecology.debugPlaceAgent(k, x3::phys::Vec3{herdX + 800.0f, 0.0f,
                                                                   herdZ + 800.0f});
                        if (stagedPredator == 0xFFFFFFFFu) stagedPredator = k;
                    }
            }
            if (shotCamOverride) for (int k = 0; k < 5; ++k) cam[k] = shotCam[k];
            const int kSettle = ecologyShot ? 180 : 48;   // ring stream-in (+ the staged hunt)
            const float dt = 1.0f / 60.0f;
            const std::string outPath = ecologyShot ? ecologyShotPath
                                      : screenshot  ? screenshotPath
                                                    : std::string("agent_valley.png");
            vstream.setUploadBudget(64);   // fill the visible ring fast for the still
            for (int i = 0; i < kSettle; ++i) {
                glfwPollEvents();
                // Nudge the focus across one tile early to trigger the full ring.
                const float focusX = (i == 1) ? 32.0f : cam[0];
                vstream.update(vscene, *device, *vphys, focusX, cam[2]);
                valley.update(dt, vscene, *vphys, vspawn, nullptr);
                const x3::phys::Vec3 camPos{cam[0], cam[1], cam[2]};
                vecology.update(dt, vscene, camPos, vtod.phase());
                if (ecologyShot && i == 135 && stagedPredator != 0xFFFFFFFFu) {
                    float cx = herdX, cz = herdZ;
                    vecology.herdCentroid(0, cx, cz);
                    vecology.debugPlaceAgent(stagedPredator,
                                             x3::phys::Vec3{cx + 6.0f, 0.0f, cz});
                }
                vphys->step(dt);
                vscene.update(*vphys);
                applyWater((float)i * dt);
                device->setCamera(cam[0], cam[1], cam[2], cam[3], cam[4], 60.0f);
                if (i == kSettle - 1) device->armCapture(outPath.c_str());
                auto frame = device->beginFrame();
                if (frame.valid) {
                    vscene.render(*device, frame);
                    valley.drawCharacters(*device, frame, vscene);
                }
                device->endFrame(frame);
            }
            const bool wrote = device->captureFrame(outPath.c_str());
            if (wrote) x3::logInfo("--world valley: wrote screenshot " + outPath);
            else       x3::logError("--world valley: capture FAILED");
            vstream.shutdown(vscene, *device, *vphys);
            vphys->shutdown();
            device->shutdown();
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return wrote ? 0 : 1;
        }

        // ===== Walkable windowed path: full first-person controller + physics. ===
        x3::game::Player vplayer;
        vplayer.spawn(*vphys, vspawn.x, vspawn.y, vspawn.z);

        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        double lastMX, lastMY; glfwGetCursorPos(window, &lastMX, &lastMY);
        double prevTime = glfwGetTime();
        bool prevSpaceV = false, prevFV = false, noclipV = false;
        float flyXv = vspawn.x, flyYv = vspawn.y + 1.6f, flyZv = vspawn.z, flyYawV = 0.0f, flyPitchV = -0.2f;
        float vWaterTime = 0.0f;
        x3::logInfo("--world valley: WASD walk, mouse look, Space jump, LeftShift sprint, F noclip, Esc to quit");

        int lastWv = (int)W, lastHv = (int)H;
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
            if (fNow && !prevFV) {
                noclipV = !noclipV;
                if (noclipV) { float yy, pp; vplayer.camera(flyXv, flyYv, flyZv, yy, pp); flyYawV = yy; flyPitchV = pp; }
            }
            prevFV = fNow;

            float camX, camY, camZ, camYaw, camPitch;
            if (!noclipV) {
                x3::game::PlayerInput in;
                if (kd(GLFW_KEY_W)) in.moveFwd    += 1.0f;
                if (kd(GLFW_KEY_S)) in.moveFwd    -= 1.0f;
                if (kd(GLFW_KEY_D)) in.moveStrafe += 1.0f;
                if (kd(GLFW_KEY_A)) in.moveStrafe -= 1.0f;
                in.sprint      = kd(GLFW_KEY_LEFT_SHIFT);
                in.jumpPressed = spaceNow && !prevSpaceV;
                in.lookDX = ddx; in.lookDY = ddy;
                vplayer.update(in, dt, *vphys);
                vplayer.camera(camX, camY, camZ, camYaw, camPitch);
            } else {
                const float sens = 0.0025f;
                flyYawV += ddx * sens; flyPitchV -= ddy * sens;
                if (flyPitchV >  1.55f) flyPitchV =  1.55f;
                if (flyPitchV < -1.55f) flyPitchV = -1.55f;
                float fxv = std::cos(flyPitchV) * std::cos(flyYawV);
                float fyv = std::sin(flyPitchV);
                float fzv = std::cos(flyPitchV) * std::sin(flyYawV);
                float rl = std::sqrt(fxv*fxv + fzv*fzv); if (rl < 1e-4f) rl = 1e-4f;
                float rx = -fzv/rl, rz = fxv/rl;
                float spd = 6.0f * dt; if (kd(GLFW_KEY_LEFT_SHIFT)) spd *= 3.0f;
                if (kd(GLFW_KEY_W)) { flyXv += fxv*spd; flyYv += fyv*spd; flyZv += fzv*spd; }
                if (kd(GLFW_KEY_S)) { flyXv -= fxv*spd; flyYv -= fyv*spd; flyZv -= fzv*spd; }
                if (kd(GLFW_KEY_D)) { flyXv += rx*spd; flyZv += rz*spd; }
                if (kd(GLFW_KEY_A)) { flyXv -= rx*spd; flyZv -= rz*spd; }
                if (spaceNow) flyYv += spd;
                if (kd(GLFW_KEY_LEFT_CONTROL)) flyYv -= spd;
                camX = flyXv; camY = flyYv; camZ = flyZv; camYaw = flyYawV; camPitch = flyPitchV;
            }
            prevSpaceV = spaceNow;

            // Stream terrain around the camera, tick the valley NPCs (hostile chase
            // the player), step physics, sync the scene, animate the lake.
            vstream.update(vscene, *device, *vphys, camX, camZ);
            const x3::phys::Vec3 vp{ camX, camY, camZ };
            valley.update(dt, vscene, *vphys, vp, &vplayer);
            vtod.advance(dt);
            vecology.update(dt, vscene, vp, vtod.phase());
            vphys->step(dt);
            vscene.update(*vphys);
            vWaterTime += dt; applyWater(vWaterTime);

            int cw, chh; glfwGetFramebufferSize(window, &cw, &chh);
            if (cw != lastWv || chh != lastHv) { lastWv = cw; lastHv = chh; if (cw>0&&chh>0) device->onResize((uint32_t)cw,(uint32_t)chh); }

            device->setCamera(camX, camY, camZ, camYaw, camPitch, 60.0f);
            auto frame = device->beginFrame();
            if (frame.valid) {
                vscene.render(*device, frame);
                valley.drawCharacters(*device, frame, vscene);
            }
            device->endFrame(frame);
        }

        vstream.shutdown(vscene, *device, *vphys);
        vphys->shutdown();
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return 0;
    }
}

}} // namespace x3::apphost
