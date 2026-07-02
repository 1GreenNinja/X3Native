// --world drive | boat | fly host (+ --screenshot-perfshop) — lifted VERBATIM
// from main() (#28 deep split).
#include "world_host_common.h"
#include "engine/core/IJobSystem.h"
#include "engine/physics/IVehicle.h"
#include "engine/audio/IAudioSystem.h"
#include "../scene.h"
#include "../mesh_prims.h"
#include "../terrain.h"
#include "../vehicle.h"
#include "../vehparts.h"
#include "../perfshop.h"
#include "../asset_root.h"
#include "../audio_root.h"
#include <filesystem>
#include <system_error>

namespace x3 { namespace apphost {

int hostDrive(HostContext& hc) {
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
    const bool perfshopShot = hc.perfshopShot;
    const std::string& perfshopShotDir = hc.perfshopShotDir;

    // ==== VERBATIM host body ====
    if (worldMode == "drive" || worldMode == "boat" || worldMode == "fly") {
        const bool isDrive = (worldMode == "drive");
        const bool isBoat  = (worldMode == "boat");
        const bool isFly   = (worldMode == "fly");
        x3::logInfo("--world " + worldMode + ": building the vehicle-framework demo");

        std::unique_ptr<x3::jobs::IJobSystem> vjobs(x3::jobs::createJobSystem());
        vjobs->init(0);
        std::unique_ptr<x3::phys::IPhysicsWorld> vphys(x3::phys::createPhysicsWorld());
        if (!vphys->init()) {
            x3::logError("--world " + worldMode + ": physics init failed");
            device->shutdown();
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return 1;
        }

        // Sky + sun (outdoor) for all three.
        { x3::rhi::IRenderDevice::SkyParams sp{}; sp.enabled = true;
          sp.sunDir[0]=0.4f; sp.sunDir[1]=1.0f; sp.sunDir[2]=0.3f;
          sp.sunColor[0]=1.0f; sp.sunColor[1]=0.97f; sp.sunColor[2]=0.92f;
          sp.sunIntensity=1.1f; sp.haze=0.4f; sp.exposure=1.0f;
          device->setSkyParams(sp); }

        // --- World ground: drive uses STREAMED terrain; boat/fly use a flat slab. ---
        x3::game::Scene          vscene;
        x3::game::TerrainStreamer vstream;
        const float boatSeaLevel = 8.0f;   // flat ocean plane for the boat demo
        float spawnX = 0.0f, spawnY = 2.0f, spawnZ = 0.0f;

        if (isDrive) {
            const x3::game::TerrainConfig& tcfg = x3::game::worldTerrainConfig();
            // Spawn the car on the surface near the origin, a little above so the
            // wheels settle onto the hill.
            spawnX = 0.0f; spawnZ = 0.0f;
            spawnY = x3::game::terrainHeightAt(tcfg, spawnX, spawnZ) + 1.5f;
            vstream.init(vscene, *device, *vphys, vjobs.get(), tcfg, spawnX, spawnZ, /*radius=*/6);
            vstream.setUploadBudget(64);
        } else {
            // Big flat static slab to bound the boat/fly world (so a raycast/contact
            // has something), well below the boat sea level.
            x3::prims::PrimMesh g = x3::prims::makeBox(400.0f, 0.5f, 400.0f, 0.0f, -0.5f, 0.0f, 0.02f);
            vphys->addStaticMesh(g.cverts.data(), (uint32_t)(g.cverts.size()/3),
                                 g.cindex.data(), (uint32_t)g.cindex.size());
        }
        if (isBoat) {
            // Animated ocean at the sea level.
            x3::rhi::IRenderDevice::WaterParams wp{};
            wp.enabled = true; wp.seaLevel = boatSeaLevel;
            wp.amplitude = 0.35f; wp.steepness = 0.5f; wp.waveLength = 12.0f; wp.speed = 1.0f;
            wp.deepColor[0]=0.015f; wp.deepColor[1]=0.06f; wp.deepColor[2]=0.10f;
            wp.shallowColor[0]=0.10f; wp.shallowColor[1]=0.32f; wp.shallowColor[2]=0.36f;
            wp.sunDir[0]=0.4f; wp.sunDir[1]=1.0f; wp.sunDir[2]=0.3f;
            wp.specular=14.0f; wp.fresnel=0.02f;
            device->setWaterParams(wp);
            spawnX = 0.0f; spawnY = boatSeaLevel + 4.0f; spawnZ = 0.0f; // drop onto the water
        }
        if (isFly) { spawnX = 0.0f; spawnY = 60.0f; spawnZ = 0.0f; }

        // --- Build the vehicle. ---
        x3::game::DriveDemo car;
        x3::game::BoatDemo  boat;
        x3::game::FlyDemo   plane;
        bool built = false;
        if (isDrive) built = car.build(*device, *vphys, spawnX, spawnY, spawnZ);
        else if (isBoat) built = boat.build(*device, *vphys, spawnX, spawnY, spawnZ, boatSeaLevel, /*isSub*/false);
        else built = plane.build(*device, *vphys, spawnX, spawnY, spawnZ);
        if (!built) {
            x3::logError("--world " + worldMode + ": vehicle build failed");
            if (isDrive) vstream.shutdown(vscene, *device, *vphys);
            vphys->shutdown(); device->shutdown();
            if (window) glfwDestroyWindow(window); glfwTerminate();
            return 1;
        }
        if (isFly) { // give the plane initial forward airspeed so lift develops
            const float v0[3] = { 0, 0, -40.0f };
            vphys->setBodyLinearVelocity(plane.airframe(), v0);
        }
        // HERO-CAR GLB skin: render the real clearcoat-painted car instead of the
        // graybox (graybox stays the fallback on a clean checkout without LFS).
        if (isDrive) {
            const bool sk = car.skin(*device, x3::game::convertedGlbRoot(), "Vehicles/CTR.glb");
            x3::logInfo(std::string("--world drive: hero-car GLB skin ") + (sk ? "ON (CTR)" : "absent — graybox"));
        }

        // ---- THE PERFORMANCE SHOP ("LATE NIGHT SPEED") — drive in, build your
        // car. Catalog + persisted VehicleBuild + the LevelDoc-authored garage on
        // a flat site ahead of spawn. Every install/tune re-tunes the LIVE Jolt
        // car (drive out and FEEL it); the build persists in vehbuild.json. ----
        x3::game::vehparts::Catalog partsCat;
        x3::game::vehparts::VehicleBuild carBuild;
        x3::game::PerfShop shop;
        bool shopBuilt = false;
        if (isDrive) {
            if (partsCat.loadFile(x3::game::vehparts::defaultCatalogPath())) {
                if (carBuild.loadFile(x3::game::vehparts::defaultBuildSavePath()))
                    x3::logInfo("--world drive: VehicleBuild loaded (credits " +
                                std::to_string(carBuild.credits) + ")");
                shopBuilt = shop.build(vscene, *device, *vphys, &partsCat, &carBuild,
                                       spawnX, spawnZ - 64.0f);
                if (shopBuilt) shop.recompose(&car);   // persisted build felt at boot
            } else {
                x3::logError("--world drive: parts catalog missing — shop disabled");
            }
        }
        vphys->optimizeBroadphase();

        const float dt = 1.0f / 60.0f;
        auto vpos = [&](float out[3]) {
            if (isDrive) car.chassisPos(out);
            else if (isBoat) boat.hullPos(out);
            else plane.airframePos(out);
        };
        auto vsetInput = [&](const x3::phys::VehicleInput& in) {
            if (isDrive) car.setInput(in); else if (isBoat) boat.setInput(in); else plane.setInput(in);
        };
        auto vpre  = [&](float d){ if (isDrive) car.preStep(d);  else if (isBoat) boat.preStep(d);  else plane.preStep(d); };
        auto vpost = [&](float d){ if (isDrive) car.postStep(d); else if (isBoat) boat.postStep(d); else plane.postStep(d); };
        auto vrender = [&](const x3::rhi::FrameContext& f) {
            if (isDrive) { vscene.render(*device, f); car.render(f); }
            else if (isBoat) boat.render(f);
            else plane.render(f);
        };

        // ===== Headless PERFORMANCE-SHOP proofs (--screenshot-perfshop <dir>). ==
        // Car posed on the lift, shop mode on: bay (car + neon sign), PARTS
        // terminal close-up, DYNO mid-pull. Works on the FULL default post stack
        // (the refl.comp NaN/INF guard + radiance cap bounded the reflection->TAA
        // history feedback loop that used to drive this world path to black).
        // NOTE: the PLAIN `--world drive --screenshot` chase shot (sun-facing cam
        // on the clearcoat car over STREAMED tiles) can still go black
        // INTERMITTENTLY with the RT reflection fallback on — race-shaped,
        // pre-existing; --legacypost (or --notaa/--norefl) sidesteps it there.
        if (perfshopShot) {
            if (!shopBuilt) {
                x3::logError("--screenshot-perfshop: shop build failed");
                car.shutdown(); vstream.shutdown(vscene, *device, *vphys);
                vphys->shutdown(); device->shutdown();
                if (window) glfwDestroyWindow(window); glfwTerminate();
                return 1;
            }
            std::error_code psec;
            std::filesystem::create_directories(perfshopShotDir, psec);
            // A showy build on the lift: turbo + ECU + exhaust (the dyno has
            // something to say) — installed directly for the proof pose.
            carBuild.install("forced_induction", "fi_turbo_small");
            carBuild.install("ecu", "ecu_flash");
            carBuild.install("exhaust", "exh_turboback");
            carBuild.tune = { 1.0f, 1.02f, 0.55f };
            shop.recompose(&car);
            // Teleport the car onto the lift pad + settle.
            float lift[3]; shop.liftCenter(lift);
            vphys->setBodyPosition(car.chassis(), { lift[0], lift[1] + 0.9f, lift[2] });
            { const float v0[3] = { 0, 0, 0 }; vphys->setBodyLinearVelocity(car.chassis(), v0); }
            for (int i = 0; i < 180; ++i) {
                glfwPollEvents();
                x3::phys::VehicleInput in{}; in.handBrake = 1.0f;
                car.setInput(in); car.preStep(dt);
                float cp[3]; car.chassisPos(cp);
                vstream.update(vscene, *device, *vphys, cp[0], cp[2]);
                vphys->step(dt); car.postStep(dt);
                shop.update(dt, &car, nullptr, {});
            }
            shop.setShopMode(true);
            float sOrg[3]; shop.shopOrigin(sOrg);
            std::vector<x3::rhi::PointLight> pls;
            auto takeShot = [&](const float cam[5], const std::string& file) -> bool {
                const std::string path = perfshopShotDir + "/" + file;
                for (int i = 0; i < 12; ++i) {
                    glfwPollEvents();
                    x3::phys::VehicleInput in{}; in.handBrake = 1.0f;
                    car.setInput(in); car.preStep(dt); vphys->step(dt); car.postStep(dt);
                    shop.update(dt, &car, nullptr, {});
                    pls.clear();
                    shop.selectLights(cam[0], cam[1], cam[2], pls, 16);
                    device->setPointLights(pls.empty() ? nullptr : pls.data(), (uint32_t)pls.size());
                    device->setCamera(cam[0], cam[1], cam[2], cam[3], cam[4], 62.0f);
                    if (i == 11) device->armCapture(path.c_str());
                    auto frame = device->beginFrame();
                    if (frame.valid) vrender(frame);
                    device->endFrame(frame);
                }
                const bool ok = device->captureFrame(path.c_str());
                x3::logInfo(std::string("--screenshot-perfshop: ") + (ok ? "wrote " : "FAILED ") + path);
                return ok;
            };
            bool allOk = true;
            // SHOT 1 — the bay: on the apron sighting straight through the bay
            // opening at the car on the lift, sign overhead.
            {
                float cpDbg[3]; car.chassisPos(cpDbg);
                x3::logInfo("--screenshot-perfshop: car at (" + std::to_string(cpDbg[0]) + ", " +
                            std::to_string(cpDbg[1]) + ", " + std::to_string(cpDbg[2]) + "), lift (" +
                            std::to_string(lift[0]) + ", " + std::to_string(lift[1]) + ", " +
                            std::to_string(lift[2]) + ")");
                const float cx = sOrg[0] + 2.1f, cy = sOrg[1] + 2.6f, cz = sOrg[2] + 14.5f;
                const float dx = lift[0] - cx, dy = (sOrg[1] + 2.6f) - cy, dz = lift[2] - cz;
                const float cam[5] = { cx, cy, cz, std::atan2(dz, dx),
                                       std::atan2(dy, std::sqrt(dx*dx + dz*dz)) };
                allOk &= takeShot(cam, "perfshop_bay.png");
            }
            // SHOT 2 — the PARTS terminal (screen at local (-2.8, 2.3, -6.5),
            // facing +Z): camera square-on in front of the glass.
            {
                const float cam[5] = { sOrg[0] - 2.8f, sOrg[1] + 2.3f, sOrg[2] - 3.3f,
                                       -1.5708f, 0.0f };   // look down -Z at the glass
                allOk &= takeShot(cam, "perfshop_parts.png");
            }
            // SHOT 3 — the DYNO mid-pull: switch the terminal, run the sweep to
            // ~55% (curves mid-draw + the RPM needle), capture the glass.
            {
                shop.uiTab();                       // PARTS -> DYNO
                shop.startPull();
                float t = 0.0f;
                while (shop.pullRunning() && shop.pullProgress() < 0.55f && t < 6.0f) {
                    shop.update(dt, &car, nullptr, {}); t += dt;
                }
                const float cam[5] = { sOrg[0] - 2.8f, sOrg[1] + 2.3f, sOrg[2] - 3.3f,
                                       -1.5708f, 0.0f };
                allOk &= takeShot(cam, "perfshop_dyno.png");
            }
            shop.shutdown(vscene, *device, *vphys);
            car.shutdown(); vstream.shutdown(vscene, *device, *vphys);
            vphys->shutdown(); device->shutdown();
            if (window) glfwDestroyWindow(window); glfwTerminate();
            return allOk ? 0 : 1;
        }

        // ===== Headless capture (--world <mode> --screenshot <path>). ==========
        if (headless) {
            const std::string outPath = screenshot ? screenshotPath
                                       : (std::string("vehicle_") + worldMode + ".png");
            // Settle a bit, drive forward, then frame a chase shot. Drive lingers
            // longer so more terrain tiles stream in around the car for the still.
            float waveT = 0.0f;
            const int kSettle = isDrive ? 200 : 120;
            for (int i = 0; i < kSettle; ++i) {
                glfwPollEvents();
                x3::phys::VehicleInput in;
                // Drive: a gentle forward creep (kept near the lit spawn hilltop so
                // the still isn't a shadowed valley) + a touch of steer for a pose.
                if (isDrive) { in.throttle = (i > 40 && i < 110) ? 0.45f : 0.0f; in.steer = 0.25f; }
                else if (isBoat) { in.throttle = (i > 60) ? 0.6f : 0.0f; }
                else { in.throttle = 1.0f; in.pitch = (i > 30 && i < 70) ? 0.3f : 0.0f; }
                vsetInput(in);
                vpre(dt);
                if (isDrive) {
                    // Stream tiles around the CAR (frame 1 nudges across a tile
                    // boundary to trigger the full residency-ring request).
                    float cp[3]; car.chassisPos(cp);
                    float fX = (i == 1) ? (cp[0] + 40.0f) : cp[0];
                    vstream.update(vscene, *device, *vphys, fX, cp[2]);
                }
                vphys->step(dt);
                vpost(dt);
                if (isBoat) {
                    waveT = (float)i * dt;
                    x3::rhi::IRenderDevice::WaterParams wp{};
                    wp.enabled=true; wp.seaLevel=boatSeaLevel; wp.amplitude=0.35f; wp.steepness=0.5f;
                    wp.waveLength=12.0f; wp.speed=1.0f; wp.time=waveT;
                    wp.deepColor[0]=0.015f; wp.deepColor[1]=0.06f; wp.deepColor[2]=0.10f;
                    wp.shallowColor[0]=0.10f; wp.shallowColor[1]=0.32f; wp.shallowColor[2]=0.36f;
                    wp.sunDir[0]=0.4f; wp.sunDir[1]=1.0f; wp.sunDir[2]=0.3f;
                    wp.specular=14.0f; wp.fresnel=0.02f;
                    device->setWaterParams(wp);
                }
                float vp[3]; vpos(vp);
                float cam[5];
                if (isDrive) {
                    // Close 3/4 chase from the SUN side looking back toward the sun
                    // (sunDir XZ = (0.4,0.3)), so the car's lit faces + lit terrain
                    // face the camera. Close in so the car fills the frame on the
                    // lit spawn hilltop (not a distant shadowed valley).
                    const float sunYaw = std::atan2(0.3f, 0.4f);
                    const float back = 7.0f, height = 3.4f;
                    cam[0] = vp[0] - std::cos(sunYaw) * back;
                    cam[1] = vp[1] + height;
                    cam[2] = vp[2] - std::sin(sunYaw) * back;
                    cam[3] = sunYaw;       // look toward the sun (and the car)
                    cam[4] = -0.26f;       // ~15deg down
                } else {
                    // Boat/fly: simple chase trailing the vehicle (behind = +Z).
                    float camH    = isFly ? 4.0f  : 3.0f;
                    float camBack = isFly ? 14.0f : 9.0f;
                    cam[0] = vp[0] + 1.0f; cam[1] = vp[1] + camH; cam[2] = vp[2] + camBack;
                    cam[3] = -1.5708f; cam[4] = isFly ? -0.12f : -0.22f;
                }
                if (shotCamOverride) for (int k=0;k<5;++k) cam[k]=shotCam[k];
                device->setCamera(cam[0], cam[1], cam[2], cam[3], cam[4], 70.0f);
                if (i == kSettle - 1) device->armCapture(outPath.c_str());
                auto frame = device->beginFrame();
                if (frame.valid) vrender(frame);
                device->endFrame(frame);
            }
            const bool wrote = device->captureFrame(outPath.c_str());
            float vp[3]; vpos(vp);
            char rb[256];
            std::snprintf(rb, sizeof(rb),
                "--world %s --screenshot: wrote %s | pos=(%.1f,%.1f,%.1f) fwdSpeed=%.2f",
                worldMode.c_str(), outPath.c_str(), vp[0], vp[1], vp[2],
                isDrive ? car.forwardSpeed() : (isBoat ? 0.0f : plane.forwardSpeed()));
            if (wrote) x3::logInfo(rb); else x3::logError("--world " + worldMode + ": capture FAILED");
            if (isDrive) { car.shutdown(); vstream.shutdown(vscene, *device, *vphys); }
            else if (isBoat) boat.shutdown(); else plane.shutdown();
            vphys->shutdown(); device->shutdown();
            if (window) glfwDestroyWindow(window); glfwTerminate();
            return wrote ? 0 : 1;
        }

        // ===== Interactive windowed: drive/steer with WASD, chase camera. ======
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        double lastMX, lastMY; glfwGetCursorPos(window, &lastMX, &lastMY);
        double prevTime = glfwGetTime();
        float camYaw = -1.5708f, camPitch = -0.22f;
        float waveT = 0.0f;
        if (isDrive) x3::logInfo("--world drive: WASD drive, Space handbrake, E exit/enter on foot, mouse orbits, Esc quit");
        else if (isBoat) x3::logInfo("--world boat: W/S motor, A/D steer, mouse orbits, Esc quit");
        else x3::logInfo("--world fly: W/S throttle, A/D yaw, Up/Down pitch, Q/E roll, mouse orbits, Esc quit");
        int lastWd = (int)W, lastHd = (int)H;

        // ---- DRIVE: E enter/exit (the Riftforged car UX) + engine audio. ----
        // Start AT THE WHEEL (the demo IS the car); E steps out to an on-foot
        // first-person walker on the terrain, E beside the car takes the wheel
        // again. The engine loop is a looping voice pitch-mapped to an RPM proxy
        // (speed + throttle blend) — the placeholder note until the parts system
        // brings per-exhaust notes (G2b).
        bool  inCar = true, prevE = false;
        float footPos[3] = { spawnX + 3.0f, spawnY, spawnZ };
        float fovNow = 70.0f;
        std::unique_ptr<x3::audio::IAudioSystem> vaudio(x3::audio::createAudioSystem());
        vaudio->init();
        x3::audio::SoundHandle engineSnd{};
        x3::audio::LoopHandle  engineLoop{};
        // ---- Shop audio layers: SC whine + turbo whistle are pitched variants of
        // the SAME engine loop (the only loop in-repo — exhaust note tiers likewise
        // ride pitch/timbre offsets on it; see VEHPARTS_FORMAT.md AUDIO). The dyno
        // LIMIT-POP bang reuses a sci-fi gunshot one-shot at low pitch. ----
        x3::audio::SoundHandle bangSnd{};
        x3::audio::LoopHandle  whineLoop{};   // supercharger (throttle-gated)
        x3::audio::LoopHandle  turboLoop{};   // turbo whistle (spool-gated)
        float turboSpool = 0.0f, prevSpool = 0.0f;
        if (isDrive) {
            engineSnd = vaudio->load(x3::game::resolveAudio("vehicles/engine_loop.wav"));
            if (engineSnd.valid()) engineLoop = vaudio->startLoop(engineSnd, 0.35f, 0.8f);
            bangSnd = vaudio->load(x3::game::resolveAudio("weapons/single/Single_Gunshot_Sci-Fi_Gun-30.wav"));
            x3::logInfo(std::string("--world drive: engine audio loop ") +
                        (engineSnd.valid() ? "ON" : "absent (silent)"));
        }
        // Shop-mode key EDGE states (UI nav + dyno sliders + pull/repair/refill).
        bool prevUiKey[12] = {};   // up,down,enter,bksp,tab,1,2,3,4,5,6,space(+R,N handled below)
        bool prevR = false, prevN = false;
        if (isDrive && shopBuilt)
            x3::logInfo("--world drive: PERFORMANCE SHOP ahead (follow -Z ~64 m) — stop on the lift");
        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();
            if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;
            double now = glfwGetTime();
            float fdt = (float)(now - prevTime); prevTime = now;
            if (fdt > 0.1f) fdt = 0.1f;
            double mx, my; glfwGetCursorPos(window, &mx, &my);
            camYaw += (float)(mx - lastMX) * 0.0025f;
            camPitch -= (float)(my - lastMY) * 0.0025f;
            if (camPitch >  1.4f) camPitch =  1.4f;
            if (camPitch < -1.4f) camPitch = -1.4f;
            lastMX = mx; lastMY = my;
            auto kd = [&](int k){ return glfwGetKey(window, k) == GLFW_PRESS; };

            // ---- DRIVE: E toggles in-car <-> on-foot (edge-triggered). ----
            if (isDrive) {
                const bool eNow = kd(GLFW_KEY_E);
                if (eNow && !prevE) {
                    float cp[3]; car.chassisPos(cp);
                    if (inCar) {
                        // Step out beside the driver's door; engine loop stops.
                        inCar = false;
                        footPos[0] = cp[0] + 2.2f; footPos[2] = cp[2];
                        const x3::game::TerrainConfig& tc = x3::game::worldTerrainConfig();
                        footPos[1] = x3::game::terrainHeightAt(tc, footPos[0], footPos[2]);
                        if (engineLoop.valid()) { vaudio->stopLoop(engineLoop); engineLoop = {}; }
                        x3::logInfo("--world drive: exited the car (E beside it to re-enter)");
                    } else {
                        const float ddx = footPos[0]-cp[0], ddz = footPos[2]-cp[2];
                        if (std::sqrt(ddx*ddx + ddz*ddz) <= 3.5f) {
                            inCar = true;
                            if (engineSnd.valid()) engineLoop = vaudio->startLoop(engineSnd, 0.35f, 0.8f);
                            x3::logInfo("--world drive: took the wheel");
                        }
                    }
                }
                prevE = eNow;
            }

            // ---- PERFORMANCE SHOP: lift detection + the terminal UI keys. ----
            if (isDrive && shopBuilt) {
                float cp[3]; car.chassisPos(cp);
                const bool stopped = std::fabs(car.forwardSpeed()) < 0.8f;
                if (!shop.shopMode() && inCar && stopped && shop.onLiftPad(cp)) {
                    shop.setShopMode(true);
                    x3::logInfo("--world drive: car on the lift — SHOP MODE (TAB dyno, W drives out)");
                }
                if (shop.shopMode() && !shop.onLiftPad(cp)) {
                    shop.setShopMode(false);
                    x3::logInfo("--world drive: left the lift — back on the road");
                }
                if (shop.shopMode()) {
                    const int uiKeys[12] = { GLFW_KEY_UP, GLFW_KEY_DOWN, GLFW_KEY_ENTER,
                                             GLFW_KEY_BACKSPACE, GLFW_KEY_TAB,
                                             GLFW_KEY_1, GLFW_KEY_2, GLFW_KEY_3, GLFW_KEY_4,
                                             GLFW_KEY_5, GLFW_KEY_6, GLFW_KEY_SPACE };
                    bool now[12];
                    for (int k = 0; k < 12; ++k) now[k] = kd(uiKeys[k]);
                    if (now[0] && !prevUiKey[0]) shop.uiUp();
                    if (now[1] && !prevUiKey[1]) shop.uiDown();
                    if (now[2] && !prevUiKey[2]) shop.uiSelect();
                    if (now[3] && !prevUiKey[3]) shop.uiBack();
                    if (now[4] && !prevUiKey[4]) shop.uiTab();
                    if (now[5] && !prevUiKey[5]) shop.adjustTune(0, -1);
                    if (now[6] && !prevUiKey[6]) shop.adjustTune(0, +1);
                    if (now[7] && !prevUiKey[7]) shop.adjustTune(1, -1);
                    if (now[8] && !prevUiKey[8]) shop.adjustTune(1, +1);
                    if (now[9] && !prevUiKey[9]) shop.adjustTune(2, -1);
                    if (now[10] && !prevUiKey[10]) shop.adjustTune(2, +1);
                    if (now[11] && !prevUiKey[11]) shop.startPull();
                    for (int k = 0; k < 12; ++k) prevUiKey[k] = now[k];
                    const bool rNow = kd(GLFW_KEY_R), nNow = kd(GLFW_KEY_N);
                    if (rNow && !prevR) shop.repairEngine();
                    if (nNow && !prevN) shop.refillNitrous();
                    prevR = rNow; prevN = nNow;
                }
            }

            x3::phys::VehicleInput in;
            if (isDrive || isBoat) {
                const bool driving = !isDrive || inCar;   // on foot -> car idles, parked
                if (driving) {
                    in.throttle = (kd(GLFW_KEY_W)?1.0f:0.0f) - (kd(GLFW_KEY_S)?1.0f:0.0f);
                    in.steer    = (kd(GLFW_KEY_D)?1.0f:0.0f) - (kd(GLFW_KEY_A)?1.0f:0.0f);
                }
                if (isDrive) {
                    if (!inCar) in.handBrake = 1.0f;      // parking brake while on foot
                    // SPACE = handbrake on the road, the DYNO PULL key in the shop.
                    else if (kd(GLFW_KEY_SPACE) && !(shopBuilt && shop.shopMode())) in.handBrake = 1.0f;
                    if (inCar && in.throttle < 0.0f && car.forwardSpeed() > 0.5f) { in.brake = 1.0f; in.throttle = 0.0f; }
                    // ---- NITROUS: LEFT SHIFT sprays while the tank lasts. ----
                    if (shopBuilt && inCar && !shop.shopMode() &&
                        shop.composed().nitrousMult > 0.0f && carBuild.nitrousRemaining > 0.0f &&
                        kd(GLFW_KEY_LEFT_SHIFT) && in.throttle > 0.0f) {
                        car.setTorqueBoost(shop.composed().nitrousMult);
                        carBuild.nitrousRemaining = std::max(0.0f, carBuild.nitrousRemaining - fdt);
                    } else {
                        car.setTorqueBoost(1.0f);
                    }
                }
            } else { // fly
                in.throttle = (kd(GLFW_KEY_W)?1.0f:0.0f) - (kd(GLFW_KEY_S)?0.5f:0.0f);
                in.steer = (kd(GLFW_KEY_D)?1.0f:0.0f) - (kd(GLFW_KEY_A)?1.0f:0.0f);
                in.pitch = (kd(GLFW_KEY_UP)?1.0f:0.0f) - (kd(GLFW_KEY_DOWN)?1.0f:0.0f);
                in.roll  = (kd(GLFW_KEY_E)?1.0f:0.0f) - (kd(GLFW_KEY_Q)?1.0f:0.0f);
            }
            vsetInput(in);
            vpre(fdt);
            float vp0[3]; vpos(vp0);
            if (isDrive) vstream.update(vscene, *device, *vphys, vp0[0], vp0[2]);
            vphys->step(fdt);
            vpost(fdt);
            if (isBoat) {
                waveT += fdt;
                x3::rhi::IRenderDevice::WaterParams wp{};
                wp.enabled=true; wp.seaLevel=boatSeaLevel; wp.amplitude=0.35f; wp.steepness=0.5f;
                wp.waveLength=12.0f; wp.speed=1.0f; wp.time=waveT;
                wp.deepColor[0]=0.015f; wp.deepColor[1]=0.06f; wp.deepColor[2]=0.10f;
                wp.shallowColor[0]=0.10f; wp.shallowColor[1]=0.32f; wp.shallowColor[2]=0.36f;
                wp.sunDir[0]=0.4f; wp.sunDir[1]=1.0f; wp.sunDir[2]=0.3f;
                wp.specular=14.0f; wp.fresnel=0.02f;
                device->setWaterParams(wp);
            }

            // Camera: chase the vehicle (with a slight SPEED-FOV stretch in the
            // car), or first-person walk when on foot (drive only).
            float vp[3]; vpos(vp);
            float cx, cy, cz;
            float fovTarget = 70.0f;
            if (isDrive && !inCar) {
                // On-foot first-person walker on the streamed terrain.
                const x3::game::TerrainConfig& tc = x3::game::worldTerrainConfig();
                float wdx = std::cos(camYaw), wdz = std::sin(camYaw);
                float wrx = -wdz, wrz = wdx;
                float wspd = (kd(GLFW_KEY_LEFT_SHIFT) ? 9.0f : 4.5f) * fdt;
                if (kd(GLFW_KEY_W)) { footPos[0]+=wdx*wspd; footPos[2]+=wdz*wspd; }
                if (kd(GLFW_KEY_S)) { footPos[0]-=wdx*wspd; footPos[2]-=wdz*wspd; }
                if (kd(GLFW_KEY_D)) { footPos[0]+=wrx*wspd; footPos[2]+=wrz*wspd; }
                if (kd(GLFW_KEY_A)) { footPos[0]-=wrx*wspd; footPos[2]-=wrz*wspd; }
                footPos[1] = x3::game::terrainHeightAt(tc, footPos[0], footPos[2]);
                cx = footPos[0]; cy = footPos[1] + 1.7f; cz = footPos[2];
                vstream.update(vscene, *device, *vphys, footPos[0], footPos[2]);
            } else {
                float dist = isFly ? 16.0f : 10.0f, height = isFly ? 4.0f : 3.5f;
                cx = vp[0] - std::cos(camPitch)*std::cos(camYaw)*dist;
                cy = vp[1] + height - std::sin(camPitch)*dist;
                cz = vp[2] - std::cos(camPitch)*std::sin(camYaw)*dist;
                if (isDrive) {
                    // Speed FOV: widen toward +14deg approaching ~120 km/h.
                    const float sn = std::min(std::fabs(car.forwardSpeed()) / 33.0f, 1.0f);
                    fovTarget = 70.0f + 14.0f * sn;
                }
            }
            // Smooth the FOV change (dt-scaled, never per-frame — the HARD rule).
            fovNow += (fovTarget - fovNow) * std::min(fdt * 6.0f, 1.0f);

            // ---- Shop per-frame: orbit/dyno/texture + pop bang; persist on change. ----
            float viewYaw = camYaw, viewPitch = camPitch;
            if (isDrive && shopBuilt) {
                shop.update(fdt, &car, vaudio.get(), bangSnd);
                if (shop.shopMode()) {
                    float oc[5]; shop.orbitCam(oc);
                    cx = oc[0]; cy = oc[1]; cz = oc[2];
                    viewYaw = oc[3]; viewPitch = oc[4];
                    fovNow = 58.0f;                       // tighter showcase framing
                }
                if (shop.consumeNeedSave()) {
                    if (carBuild.saveFile(x3::game::vehparts::defaultBuildSavePath()))
                        x3::logInfo("--world drive: VehicleBuild saved");
                }
            }

            // Engine audio: pitch tracks the REAL engine RPM (the TC pass made the
            // rev climb + shifts physical), shaped by the EXHAUST tier's note
            // (pitch/timbre offsets) — plus the SC whine / turbo whistle layers.
            if (isDrive && engineLoop.valid()) {
                const auto& cb = shop.composed();
                const float rpmFrac = std::clamp(car.engineRPM() / std::max(1000.0f, cb.maxRpm), 0.0f, 1.0f);
                const float th = std::clamp(in.throttle, 0.0f, 1.0f);
                const float vol   = (0.30f + 0.28f*th + 0.18f*rpmFrac) * (1.0f + 0.25f*cb.exhaustTimbre);
                const float pitch = 0.65f + 1.15f*rpmFrac + 0.15f*th + cb.exhaustPitchOffset;
                vaudio->setLoopParams(engineLoop, vol, pitch);
                // Supercharger whine: throttle-gated pitched layer.
                if (cb.scWhine && engineSnd.valid()) {
                    if (!whineLoop.valid()) whineLoop = vaudio->startLoop(engineSnd, 0.0f, 2.4f);
                    if (whineLoop.valid())
                        vaudio->setLoopParams(whineLoop, th * 0.20f, 2.4f + 1.3f * rpmFrac);
                } else if (whineLoop.valid()) { vaudio->stopLoop(whineLoop); whineLoop = {}; }
                // Turbo: spool rises at wide throttle (lag from the part data),
                // whistle rides the spool; lifting off above 55% spool = BLOWOFF.
                if (cb.turboWhistle && engineSnd.valid()) {
                    const float lag = std::max(0.25f, cb.turboSpoolS);
                    if (th > 0.6f) turboSpool = std::min(1.0f, turboSpool + fdt / lag);
                    else           turboSpool = std::max(0.0f, turboSpool - fdt * 2.5f);
                    if (prevSpool > 0.55f && th < 0.2f) {
                        vaudio->playSound2D(engineSnd, 0.45f, 4.2f);   // psshh
                        turboSpool = 0.0f;
                    }
                    prevSpool = turboSpool;
                    if (!turboLoop.valid()) turboLoop = vaudio->startLoop(engineSnd, 0.0f, 3.0f);
                    if (turboLoop.valid())
                        vaudio->setLoopParams(turboLoop, turboSpool * 0.18f, 3.0f + 1.2f * turboSpool);
                } else if (turboLoop.valid()) { vaudio->stopLoop(turboLoop); turboLoop = {}; }
            }
            vaudio->setListener(cx, cy, cz, viewYaw, viewPitch);
            int cw, ch; glfwGetFramebufferSize(window, &cw, &ch);
            if (cw != lastWd || ch != lastHd) { lastWd=cw; lastHd=ch; if (cw>0&&ch>0) device->onResize((uint32_t)cw,(uint32_t)ch); }
            // Shop point lights nearest the eye (interior work lights + neon wash).
            if (isDrive && shopBuilt) {
                std::vector<x3::rhi::PointLight> pls;
                shop.selectLights(cx, cy, cz, pls, 16);
                device->setPointLights(pls.empty() ? nullptr : pls.data(), (uint32_t)pls.size());
            }
            device->setCamera(cx, cy, cz, viewYaw, viewPitch, fovNow);
            auto frame = device->beginFrame();
            if (frame.valid) vrender(frame);
            device->endFrame(frame);
        }
        if (engineLoop.valid()) vaudio->stopLoop(engineLoop);
        if (whineLoop.valid())  vaudio->stopLoop(whineLoop);
        if (turboLoop.valid())  vaudio->stopLoop(turboLoop);
        vaudio->shutdown();
        if (isDrive && shopBuilt) {
            // Persist the build (incl. the nitrous tank level) on the way out.
            carBuild.saveFile(x3::game::vehparts::defaultBuildSavePath());
            shop.shutdown(vscene, *device, *vphys);
        }
        if (isDrive) { car.shutdown(); vstream.shutdown(vscene, *device, *vphys); }
        else if (isBoat) boat.shutdown(); else plane.shutdown();
        vphys->shutdown(); device->shutdown();
        if (window) glfwDestroyWindow(window); glfwTerminate();
        return 0;
    }
}

}} // namespace x3::apphost
