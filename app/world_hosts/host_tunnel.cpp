// --world tunnel host — THE TERRAIN CORRIDOR, MADE VISIBLE.
//
// Boots the canonical streamed terrain with ONE registered TerrainCorridor
// (app/terrain.h) carving a graded road corridor through a real hillside, lays
// a drivable road ribbon down it, and roofs the reach that has enough cover
// with an arched tunnel shell. Drive it with the physics car; capture the proof
// set headless with --screenshot-tunnel.
//
// See app/tunnel_corridor.h for the technique + the clean-room BL provenance.
#include "world_host_common.h"
#include "host_shell.h"                  // console (~), pause menu (ESC), FPS (F3)
#include "engine/core/IJobSystem.h"
#include "engine/physics/IVehicle.h"
#include "../scene.h"
#include "../terrain.h"
#include "../tunnel_corridor.h"
#include "../vehicle.h"
#include "../mesh_prims.h"
#include "../asset_root.h"
#include "engine/audio/IAudioSystem.h"   // ENGINE NOTE: RPM-driven loop
// stb_image: file-local static copy (the cinematic.cpp / descent_slide.cpp
// recipe — the engine's implementation is file-local in ModelLoader.cpp, so each
// app TU that decodes PNGs instantiates its own).
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#if defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning(disable : 4244 4456 4457)
#endif
#include <stb_image.h>
#if defined(_MSC_VER)
#  pragma warning(pop)
#endif

#include <filesystem>
#include <system_error>
#include <cmath>      // std::floor  (pause-overlay layout)
#include <cstdio>     // std::snprintf (HUD readouts)
#include <cstring>    // std::strlen (pause-overlay centering)

namespace x3 { namespace apphost {

int hostTunnel(HostContext& hc) {
    auto* device = hc.device;
    GLFWwindow* window = hc.window;
    const bool headless = hc.headless;
    const bool screenshot = hc.screenshot;
    const std::string& screenshotPath = hc.screenshotPath;
    const uint32_t W = hc.W, H = hc.H;
    (void)W; (void)H;

    x3::logInfo("--world tunnel: terrain-corridor bore demo");

    // ==== STEP 1 — REGISTER THE CORRIDOR, BEFORE ANY HEIGHT CONSUMER =========
    // app/terrain.h's contract: "Register corridors at BOOT, BEFORE the first
    // height query / TerrainStreamer::init()". Everything below (the streamer,
    // the horizon ring, the road grading, the car spawn) reads the field AFTER
    // this line, so they all agree by construction.
    const x3::game::TunnelRoute& route = x3::game::registerTunnelCorridor();

    std::unique_ptr<x3::phys::IPhysicsWorld> phys(x3::phys::createPhysicsWorld());
    if (!phys->init()) {
        x3::logError("--world tunnel: physics init failed");
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }
    std::unique_ptr<x3::jobs::IJobSystem> jobs(x3::jobs::createJobSystem());
    jobs->init(0);
    x3::game::Scene scene;

    {   // Bright, high sun: the point of the shot is READING THE GROUND, and a
        // low sun would fill the cutting with shadow and hide the very seams
        // this demo exists to expose.
        x3::rhi::IRenderDevice::SkyParams sp{};
        sp.enabled = true;
        sp.sunDir[0] = 0.35f; sp.sunDir[1] = 0.92f; sp.sunDir[2] = 0.18f;
        sp.sunColor[0] = 1.0f; sp.sunColor[1] = 0.97f; sp.sunColor[2] = 0.92f;
        sp.sunIntensity = 1.0f; sp.haze = 0.35f; sp.exposure = 1.0f;
        device->setSkyParams(sp);
    }
    device->setCameraFar(4000.0f);

    // ==== STEP 2 — the streamed terrain ring =================================
    float startPos[3];
    // On the road, out on open ground, far enough back that the whole approach
    // cutting + the portal are ahead of you (and in frame on the approach shot).
    route.posAt(std::max(8.0f, route.boreS0 - 55.0f), startPos);

    x3::game::TerrainStreamer streamer;
    const x3::game::TerrainConfig& cfg = x3::game::worldTerrainConfig();
    streamer.setUploadBudget(96);
    streamer.setMaxInFlight(48);
    streamer.init(scene, *device, *phys, jobs.get(), cfg,
                  startPos[0], startPos[2], /*radius=*/headless ? 14 : 9);

    // Far country so the hill sits in a landscape and not on a void horizon.
    {
        float mid[3]; route.posAt(route.totalLen * 0.5f, mid);
        x3::game::HorizonRingDesc hr{};
        hr.centerX = mid[0]; hr.centerZ = mid[2];
        hr.rInner = 470.0f; hr.rOuter = 9000.0f;
        hr.rings = 96; hr.segments = 128; hr.yBias = -3.0f;
        x3::game::addTerrainHorizonRing(scene, *device, streamer.groundTexture(), hr);
    }

    // ==== STEP 3 — the road, the shell, the portals ==========================
    x3::game::TunnelCorridorWorld tunnel;
    tunnel.build(scene, *device, *phys, route);
    device->setPointLights(tunnel.lights().data(), (uint32_t)tunnel.lights().size());

    // ==== STEP 4 — the car, on the road, outside the entrance ================
    x3::game::DriveDemo car;
    const bool carBuilt = car.build(*device, *phys, startPos[0], startPos[1] + 1.4f, startPos[2]);
    if (carBuilt) {
        car.skin(*device, x3::game::convertedGlbRoot(), "Vehicles/CTR.glb");
        // Point it down the corridor.
        // SPAWN YAW — engine forward at rest is -Z (CLAUDE.md AXES / CONVENTIONS
        // §3), so rotating rest forward (0,0,-1) about +Y by theta gives
        // (-sin theta, 0, -cos theta); facing the corridor direction (dirX, dirZ)
        // is theta = atan2(-dirX, -dirZ). The old atan2(dirZ, dirX) measured from
        // +X, not from -Z, which placed the car 90 deg off the road.
        // Tim, 2026-08-14: "The car is PLACED facing the wrong way. I have to TURN
        // it to drive it forward. Controls make the car behave as it should." —
        // the second sentence proves the rig and skin are fine; only spawn was wrong.
        const float yaw = -std::atan2(-route.dirX, -route.dirZ);
        const float q[4] = { 0.0f, std::sin(-yaw * 0.5f), 0.0f, std::cos(-yaw * 0.5f) };
        phys->setBodyRotation(car.chassis(), q);
    } else {
        x3::logWarn("--world tunnel: car build failed — walk/fly only");
    }

    // ---- WHEEL-SPIN FX (skid marks + smoke) --------------------------------
    x3::rhi::MeshHandle fxQuadMesh;
    x3::rhi::TextureHandle fxSkidTex, fxSmokeTex;
    {
        std::vector<x3::rhi::MeshVertex> qv; std::vector<uint32_t> qi;
        x3::prims::makeCube(0.5f, qv, qi);
        fxQuadMesh = device->createMesh(qv.data(), (uint32_t)qv.size(), qi.data(), (uint32_t)qi.size());
        auto sk = x3::prims::makeSolidRGBA(8, 18, 18, 22);
        auto sm = x3::prims::makeSolidRGBA(8, 150, 150, 155);
        fxSkidTex  = device->createTexture(sk.data(), 8, 8, true);
        fxSmokeTex = device->createTexture(sm.data(), 8, 8, true);
    }
    struct SpinFx { float x, y, z, age; uint8_t kind; };  // 0=skid, 1=smoke
    SpinFx fx[512]; uint32_t fxN = 0;
    float fxSpawnAcc = 0.0f;

    // ==== ENGINE NOTE =======================================================
    // Everything for this already existed and nothing played it: the sample is
    // committed at assets/audio/vehicles/engine_loop.wav, IAudioSystem has
    // startLoop3D/setLoopParams, and DriveDemo::engineRPM() reports the live
    // crank speed. The only missing piece was host wiring. (Same shape as the
    // shift points: data model present, playback absent.)
    //
    // A 3D loop parented to the car, re-pitched every frame from RPM. 3D rather
    // than 2D so the note attenuates and pans as the chase camera swings around
    // the car, and so it echoes correctly once RtAcoustics is in the path.
    std::unique_ptr<x3::audio::IAudioSystem> audio(x3::audio::createAudioSystem());
    const bool audioOn = audio && audio->init();
    x3::audio::SoundHandle engineSnd{};
    x3::audio::LoopHandle  engineLoop{};
    x3::audio::LoopHandle  whineLoop{};   // supercharger whine (throttle-gated)
    x3::audio::LoopHandle  turboLoop{};   // turbo whistle (spool-gated)
    float turboSpool = 0.0f, prevSpool = 0.0f;
    x3::audio::SoundHandle squealSnd{};
    x3::audio::LoopHandle  squealLoop{};   // tire squeal (slip-gated)
    if (audioOn) {
        const std::string wav =
            (std::filesystem::path(x3::game::assetRoot()) / "audio/vehicles/engine_loop.wav").string();
        engineSnd = audio->load(wav);
        squealSnd = audio->load((std::filesystem::path(x3::game::assetRoot()) / "audio/vehicles/tire_squeal_loop.wav").string());
        if (engineSnd.valid() && carBuilt) {
            float ep[3]; car.chassisPos(ep);
            (void)ep;
            // 2D on purpose. IAudioSystem::startLoop's own contract says 2D is
            // right for "the player's OWN" emitter, and there is no
            // setLoopPosition to follow a moving car with — a 3D loop would stay
            // pinned where the car spawned. The chase cam holds a fixed ~9 m
            // offset anyway, so there is no panning to win.
            engineLoop = audio->startLoop(engineSnd, 0.0f, 1.0f);
            x3::logInfo("[tunnel] engine note online");
        } else if (!engineSnd.valid()) {
            x3::logWarn("[tunnel] engine_loop.wav failed to load — driving stays silent");
        }
    }
    // ==== GAUGE ARTWORK =====================================================
    // Real textures, not quads. The first cut approximated a dial with 121 tiny
    // axis-aligned rectangles because I had told the agent "rectangles only";
    // drawHudImage() takes a TEXTURE with UV sub-rects, so the right reading of
    // that constraint is "put real art in the rectangle". Owner's verdict on the
    // quad version: "slop in Carbon esque shape".
    // Generated by tools/make_gauge_textures.py — rerun it to change the art.
    x3::rhi::TextureHandle texDial{}, texNeedle{}, texGate{}, texBoost{};
    {
        auto loadPng = [&](const char* rel) -> x3::rhi::TextureHandle {
            const std::string p =
                (std::filesystem::path(x3::game::assetRoot()) / "ui" / rel).string();
            int w = 0, h = 0, c = 0;
            stbi_uc* px = stbi_load(p.c_str(), &w, &h, &c, 4);
            if (!px) { x3::logWarn(std::string("[tunnel] gauge art missing: ") + p); return {}; }
            x3::rhi::TextureHandle t = device->createTexture(px, (uint32_t)w, (uint32_t)h, true);
            stbi_image_free(px);
            return t;
        };
        texDial   = loadPng("gauge_dial.png");
        texNeedle = loadPng("gauge_needle.png");
        texGate   = loadPng("gauge_gate.png");
        texBoost  = loadPng("gauge_boost.png");
    }

    phys->optimizeBroadphase();

    const float dt = 1.0f / 60.0f;

    // ==== HEADLESS: the proof set ===========================================
    if (headless) {
        namespace fs = std::filesystem;
        std::error_code ec;
        const std::string dir = hc.tunnelShot ? hc.tunnelShotDir : std::string("docs/screenshots/tunnel");
        fs::create_directories(dir, ec);

        auto settleAndGrab = [&](const float cam[5], const std::string& out) -> bool {
            // The streamer only enqueues the full ring on a focus-tile crossing
            // (host_cliffs.cpp's trick): nudge the focus on frame 1, then hold.
            const int kFrames = 200;
            for (int i = 0; i < kFrames; ++i) {
                glfwPollEvents();
                const float fx = (i == 1) ? cam[0] + 40.0f : cam[0];
                streamer.update(scene, *device, *phys, fx, cam[2]);
                phys->step(dt);
                device->setCamera(cam[0], cam[1], cam[2], cam[3], cam[4], 68.0f);
                if (i == kFrames - 1) device->armCapture(out.c_str());
                auto frame = device->beginFrame();
                if (frame.valid) { scene.render(*device, frame); if (carBuilt) car.render(frame); }

                device->endFrame(frame);
            }
            const bool wrote = device->captureFrame(out.c_str());
            if (wrote) x3::logInfo("--world tunnel: wrote " + out);
            else       x3::logError("--world tunnel: capture FAILED " + out);
            return wrote;
        };

        bool ok = true;
        if (hc.tunnelShot) {
            struct Shot { int which; const char* name; };
            const Shot shots[] = {
                { 0, "01_approach"  },
                { 1, "02_inside"    },
                { 2, "03_far_mouth" },
                { 3, "04_saddle"    },
                { 4, "05_portal_detail" },
            };
            for (const Shot& sh : shots) {
                float cam[5]; tunnel.showcaseCamera(route, sh.which, cam);
                char path[512];
                std::snprintf(path, sizeof(path), "%s/%s.png", dir.c_str(), sh.name);
                char cb[256];
                std::snprintf(cb, sizeof(cb), "--world tunnel: %s cam=(%.1f, %.1f, %.1f) yaw=%.3f pitch=%.3f",
                              sh.name, cam[0], cam[1], cam[2], cam[3], cam[4]);
                x3::logInfo(cb);
                ok = settleAndGrab(cam, path) && ok;
            }
        } else {
            float cam[5]; tunnel.showcaseCamera(route, 0, cam);
            if (hc.shotCamOverride) for (int k = 0; k < 5; ++k) cam[k] = hc.shotCam[k];
            const std::string out = screenshot ? screenshotPath : std::string("w_tunnel.png");
            ok = settleAndGrab(cam, out);
        }

        if (carBuilt) car.shutdown();
        tunnel.shutdown(*device, *phys);
        streamer.shutdown(scene, *device, *phys);
        if (audioOn) {
        if (engineLoop.valid()) audio->stopLoop(engineLoop);
        audio->shutdown();
    }
    jobs->shutdown(); phys->shutdown(); device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return ok ? 0 : 1;
    }

    // ==== INTERACTIVE: drive it =============================================
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
    double lastMX, lastMY; glfwGetCursorPos(window, &lastMX, &lastMY);
    double prevTime = glfwGetTime();
    float camYaw = std::atan2(route.dirZ, route.dirX), camPitch = -0.10f;
    int lastW = (int)W, lastH = (int)H;
    x3::logInfo("--world tunnel: WASD drives, Space handbrake, mouse orbits the chase cam, "
                "~ console, ESC menu, SHIFT+ESC quits");

    // ---- DEV SHELL: console, pause menu, FPS -------------------------------
    // The reason the whole vehicle-feel pass was slow: every torque figure, grip
    // scale and centre-of-mass nudge cost an edit-rebuild-relaunch-drive-back
    // cycle, and those are values you have to judge by feel, one at a time. They
    // are all live now.
    HostShell shell;
    shell.attach(hc);
    if (auto* con = shell.console()) {
        shell.addFloatCommand("car_torque", "peak engine torque, ft-lb (stock 590)",
            [&](float v) { x3::phys::WheeledTuning t; t.maxEngineTorque = v * 1.35582f; car.applyTuning(t); });
        shell.addFloatCommand("car_redline", "engine redline, rpm (stock 7500)",
            [&](float v) { x3::phys::WheeledTuning t; t.maxEngineRPM = v; car.applyTuning(t); });
        shell.addFloatCommand("car_grip", "tyre grip multiplier (stock 5.2; 1 = Jolt's economy tyre)",
            [&](float v) { x3::phys::WheeledTuning t; t.gripScale = v; car.applyTuning(t); });
        shell.addFloatCommand("car_mass", "chassis mass, kg (stock 1300)",
            [&](float v) { x3::phys::WheeledTuning t; t.massKg = v; car.applyTuning(t); });
        shell.addFloatCommand("car_brake", "brake torque, Nm, all wheels",
            [&](float v) { x3::phys::WheeledTuning t; t.brakeTorque = v; car.applyTuning(t); });
        shell.addFloatCommand("car_ride", "ride-height delta, m (negative lowers)",
            [&](float v) { x3::phys::WheeledTuning t; t.rideHeightDelta = v; car.applyTuning(t); });
        shell.addFloatCommand("car_springfreq", "suspension spring frequency, Hz",
            [&](float v) { x3::phys::WheeledTuning t; t.suspensionFreq = v; car.applyTuning(t); });
        shell.addFloatCommand("car_springdamp", "suspension damping ratio",
            [&](float v) { x3::phys::WheeledTuning t; t.suspensionDamp = v; car.applyTuning(t); });
        shell.addFloatCommand("car_torquemult", "flat torque multiplier on top of the turbo (nitrous)",
            [&](float v) { car.setTorqueBoost(v); });
        // ---- turbo ----
        shell.addToggleCommand("turbo", "turbo on/off (off = the curve with no lag, naturally aspirated)",
            [&]{ return car.turboEnabled(); },
            [&](bool on) { car.setTurboEnabled(on); });
        shell.addFloatCommand("turbo_max", "peak boost, psi (stock 16)",
            [&](float v) { car.turbo().maxPsi = v; });
        shell.addFloatCommand("turbo_spool", "seconds for the compressor to come up (stock 0.45)",
            [&](float v) { car.turbo().spoolTau = v; });
        shell.addFloatCommand("turbo_dump", "seconds to bleed off on a lift (stock 0.11)",
            [&](float v) { car.turbo().dumpTau = v; });
        shell.addFloatCommand("turbo_start", "rpm where the compressor starts to make pressure (stock 1800)",
            [&](float v) { car.turbo().spoolStartRpm = v; });
        shell.addFloatCommand("turbo_full", "rpm for full boost (stock 4200)",
            [&](float v) { car.turbo().spoolFullRpm = v; });
        shell.addFloatCommand("turbo_floor", "torque fraction with no boost at all (stock 0.60)",
            [&](float v) { car.turbo().floorTorque = v; });
        shell.addFloatCommand("turbo_vacuum", "vacuum depth at a closed throttle, psi (stock 8.5)",
            [&](float v) { car.turbo().vacuumPsi = v; });
        shell.addToggleCommand("car_tc", "traction control (also bound to T)",
            [&]{ return car.tractionControl(); },
            [&](bool on) { car.setTractionControl(on); });
        con->registerCommand("car_reset", [&](const std::vector<std::string>&) {
            car.setTorqueBoost(1.0f);
            x3::phys::WheeledTuning t;
            t.maxEngineTorque = 2400.0f; t.maxEngineRPM = 7500.0f;
            t.gripScale = 5.2f; t.massKg = 1300.0f;
            car.applyTuning(t);
            con->print("car back to the shipped 993 Turbo numbers");
        }, "restore the stock vehicle tune");
        con->registerCommand("car", [&](const std::vector<std::string>&) {
            char b[256];
            std::snprintf(b, sizeof(b),
                          "gear %d  %.0f rpm  %.0f mph  %+.1f psi (x%.2f)  TC %s  turbo %s",
                          car.gear(), (double)car.engineRPM(),
                          (double)(std::fabs(car.forwardSpeed()) * 2.23694f),
                          (double)car.boostPsi(), (double)car.turboMult(),
                          car.tractionControl() ? "on" : "off",
                          car.turboEnabled() ? "on" : "off");
            con->print(b);
        }, "print the car's live state");
    }

    while (!glfwWindowShouldClose(window) && !shell.wantQuit()) {
        // RE-SUBMIT THE BORE LIGHTS EVERY FRAME. They were set exactly ONCE at boot
        // (setPointLights above), which is why the tunnel is lit in headless captures
        // — those render a few frames with nothing else touching the light set — and
        // PITCH BLACK the moment you drive it, both from inside and looking in through
        // the portal from outside. The interactive loop streams tiles and draws other
        // content, and the light array does not survive that. Cheap: 6 cached lights.
        device->setPointLights(tunnel.lights().data(), (uint32_t)tunnel.lights().size());
        glfwPollEvents();
        shell.beginFrame();

        // ESC OPENS THE MENU, IT DOES NOT QUIT — the shell owns that now, along
        // with the console and the FPS overlay. This host used to hand-roll the
        // pause by polling glfwGetKey and tracking its own `escWasDown` edge,
        // which drops a press any time a frame runs longer than the keypress.
        // The shell edge-detects in the GLFW key CALLBACK instead, so a press
        // cannot be missed no matter how long the frame took.
        const double now = glfwGetTime();
        float fdt = (float)(now - prevTime); prevTime = now;
        if (fdt > 0.1f) fdt = 0.1f;

        if (shell.paused()) {
            // Present so the window stays live, but do not advance the sim.
            auto pf = device->beginFrame();
            if (pf.valid) {
                scene.render(*device, pf);
                if (carBuilt) car.render(pf);
                shell.draw(pf, fdt);
            }
            device->endFrame(pf);
            continue;
        }

        double mx, my; glfwGetCursorPos(window, &mx, &my);
        const float ddx = (float)(mx - lastMX), ddy = (float)(my - lastMY);
        lastMX = mx; lastMY = my;
        // Do not steer the camera with the mouse while the console has it — the
        // cursor is released to click, and the view would spin as you reach for
        // the scrollback.
        if (!shell.consoleOpen()) {
            camYaw += ddx * 0.0025f; camPitch -= ddy * 0.0025f;
            if (camPitch >  1.2f) camPitch =  1.2f;
            if (camPitch < -1.2f) camPitch = -1.2f;
        }
        // shell.key(), not glfwGetKey(): returns false while the console or the
        // menu owns the keyboard, so typing `car_grip 6` no longer also steers
        // right, brakes and applies the handbrake.
        auto kd = [&](int k){ return shell.key(k); };

        x3::phys::VehicleInput in;
        if (carBuilt) {
            in.throttle = (kd(GLFW_KEY_W) ? 1.0f : 0.0f) - (kd(GLFW_KEY_S) ? 1.0f : 0.0f);
            in.steer    = (kd(GLFW_KEY_D) ? 1.0f : 0.0f) - (kd(GLFW_KEY_A) ? 1.0f : 0.0f);
            if (kd(GLFW_KEY_SPACE)) in.handBrake = 1.0f;
            // T toggles TRACTION CONTROL (Tim asked for an off switch). Edge
            // triggered. TC trims throttle toward a 0.10 slip target and can cut
            // to 15%, which is great for a clean launch and wrong when you want
            // to hang the tail out. Off = the tyres are the only limit.
            {
                static bool tcWasDown = false;
                const bool tcDown = kd(GLFW_KEY_T);
                if (tcDown && !tcWasDown) {
                    car.setTractionControl(!car.tractionControl());
                    x3::logInfo(car.tractionControl() ? "[tunnel] traction control ON"
                                                      : "[tunnel] traction control OFF");
                }
                tcWasDown = tcDown;
            }
            if (in.throttle < 0.0f && car.forwardSpeed() > 0.5f) { in.brake = 1.0f; in.throttle = 0.0f; }

            // AUTO-HOLD. Tim, 2026-08-15: "It should be Unable to roll when not
            // accelerating or reversing, there is an E brake."
            // With no throttle the rig had brake 0 AND handbrake 0, i.e. neutral,
            // so the car free-wheeled down every gradient — it "rolled" in the
            // sense of rolling AWAY (not tipping over; that was my misreading).
            // A real car holds: an auto creeps against its brakes and a parked
            // one sits on the handbrake.
            // Braking ramps in as speed falls so coasting still feels like
            // coasting, then locks solid at a standstill. Skipped while the
            // player is on the handbrake so deliberate slides still work.
            if (in.throttle == 0.0f && in.handBrake == 0.0f) {
                const float spd = std::fabs(car.forwardSpeed());   // m/s
                if (spd < 0.35f) {
                    in.brake = 1.0f;          // parked: hold it, full stop
                } else if (spd < 6.0f) {
                    // 0.25 at 6 m/s -> 1.0 approaching rest: settles without a lurch
                    in.brake = 0.25f + 0.75f * (1.0f - spd / 6.0f);
                } else {
                    in.brake = 0.08f;         // light drag, reads as engine braking
                }
            }
            car.setInput(in);
            car.preStep(fdt);
        }
        float vp[3] = { startPos[0], startPos[1], startPos[2] };
        if (carBuilt) car.chassisPos(vp);
        streamer.update(scene, *device, *phys, vp[0], vp[2]);
        phys->step(fdt);
        if (carBuilt) car.postStep(fdt);
        // RE-SAMPLE THE CHASE TARGET AFTER THE STEP.
        // `vp` above was read BEFORE phys->step(), so the camera was aiming at
        // where the car had been one physics step earlier while the car itself
        // draws from its post-step pose. At 30 m/s a 60 Hz step is ~0.5 m, and
        // because the frame delta varies the lag varies with it — so the car
        // appears to oscillate between two positions a few pixels apart every
        // frame. Tim, 2026-08-14: "when accelerating / moving, the car is
        // oscillating between two points several pixels apart, causing a
        // blur/shimmer."
        // The pre-step sample is still the right input for streamer.update()
        // (tile streaming does not need sub-frame precision); only the camera
        // needs the current pose.
        if (carBuilt) car.chassisPos(vp);
        scene.update(*phys);

        // ---- ENGINE NOTE: re-pitch from live RPM, and move the emitter ------
        // pitch tracks RPM across the powerband; vol fades in off idle so a
        // parked car is not droning at full volume. Both are cheap per-frame
        // parameter updates on ONE voice — no retriggering, so the loop stays
        // seamless through gearchanges.
        if (audioOn && engineLoop.valid() && carBuilt) {
            const float rpm    = car.audioRPM();
            const float redline = 7500.0f;                       // matches vd.maxEngineRPM
            const float frac   = std::min(1.0f, std::max(0.0f, rpm / redline));
            // PITCH tracks RPM PROPORTIONALLY — real engine-note frequency scales
            // linearly with crank speed, so the playback rate must too. The old
            // 1.05 + frac*1.75 span (1.05 -> 2.80) never reached the top: Tim,
            // 2026-08-15 — "7500 rpm sounds like 3000 rpm in real life".
            // Calibrated from that: 2.80x == ~3000 rpm, so unity (1.0x) ==
            // ~1071 rpm, and 7500 rpm needs ~7.0x (within the 8.0x clamp). Idle
            // (~800) therefore sits at ~0.75x — a genuinely low idle note. If
            // that reads "rattly", the real fix is a second higher-RPM loop
            // crossfaded in, not compressing the range again.
            const float rawPitch = rpm / 1071.0f;
            // IDLE HOLD. A flat-six idles at a steady ~800 rpm, but the physics
            // engine has no idle governor and hunts around zero throttle — so the
            // note must NOT wobble with it. Parked + off-throttle -> fixed idle
            // pitch; the moment the driver asks for power or the car rolls, it
            // tracks rpm again (overrun still follows rpm, as it should).
            const bool idling = (car.throttleInput() < 0.01f &&
                                 std::fabs(car.forwardSpeed()) < 1.0f);
            const float pitch = idling ? 0.75f : rawPitch;

            // VOLUME follows LOAD, not speed. Tim, 2026-08-15: "In a real car..
            // engine tone shifts with load.. and load changes with torque, and
            // torque is not flat, its a curve."
            // Load = what the driver is asking for, times what the engine can
            // actually make at these revs. Same normalized curve the physics
            // runs — [0,0.78] [0.3,0.97] [0.55,1.0] [0.8,0.95] [1,0.82] — so the
            // note thickens through the midrange and thins at the top exactly
            // where the engine does, instead of just getting louder with rpm.
            auto torqueFrac = [](float f) {
                const float xs[5] = {0.00f, 0.30f, 0.55f, 0.80f, 1.00f};
                const float ys[5] = {0.78f, 0.97f, 1.00f, 0.95f, 0.82f};
                if (f <= xs[0]) return ys[0];
                for (int k = 1; k < 5; ++k)
                    if (f <= xs[k]) {
                        const float t = (f - xs[k-1]) / (xs[k] - xs[k-1]);
                        return ys[k-1] + (ys[k] - ys[k-1]) * t;
                    }
                return ys[4];
            };
            const float thr  = std::min(1.0f, std::max(0.0f, car.effectiveThrottle()));
            // ...times what the TURBO is currently delivering. The multiplier
            // runs 0.60 off boost to 1.00 on it, so the note swells over the
            // half-second the compressor takes to come up and drops the instant
            // you lift. That swell is the single most recognisable thing about
            // a turbo car, and it costs one multiply.
            const float load = thr * torqueFrac(frac) * car.turboMult();
            // Off-throttle is OVERRUN: the engine is being driven by the wheels,
            // so it stays audible and keeps its pitch but drops right back in
            // level. That contrast is most of what makes a car sound driven.
            const float vol  = 0.16f + 0.62f * load + 0.10f * frac;
            // LOW-PASS the note. The physics engine can jitter its RPM (the
            // clutch/gearbox hunt this lane has been chasing), but a real engine
            // note does NOT wobble frame to frame — it glides. One-pole smooth
            // (~0.1 s) so it reads as one continuous engine, not a stutter.
            static float sPitch = 0.75f, sVol = 0.16f;
            const float k = 1.0f - std::exp(-9.0f * fdt);
            sPitch += (pitch - sPitch) * k;
            sVol   += (vol   - sVol)   * k;
            audio->setLoopParams(engineLoop, sVol, sPitch);

            // Supercharger whine + turbo whistle — the old --world drive host's
            // extra layers, pitched variants of the SAME engine loop (Tim: "use
            // the old host drive sounds"). Whine is throttle-gated; whistle rides
            // the spool; lifting off above ~55% spool = a blowoff psshh.
            if (!whineLoop.valid()) whineLoop = audio->startLoop(engineSnd, 0.0f, 2.4f);
            if (whineLoop.valid())
                audio->setLoopParams(whineLoop, thr * 0.20f, 2.4f + 1.3f * frac);

            const float spoolLag = 0.45f;   // == TurboParams::spoolTau
            if (thr > 0.6f) turboSpool = std::min(1.0f, turboSpool + fdt / spoolLag);
            else            turboSpool = std::max(0.0f, turboSpool - fdt * 2.5f);
            if (prevSpool > 0.55f && thr < 0.2f) {
                audio->playSound2D(engineSnd, 0.45f, 4.2f);   // blowoff psshh
                turboSpool = 0.0f;
            }
            prevSpool = turboSpool;
            if (!turboLoop.valid()) turboLoop = audio->startLoop(engineSnd, 0.0f, 3.0f);
            if (turboLoop.valid())
                audio->setLoopParams(turboLoop, turboSpool * 0.18f, 3.0f + 1.2f * turboSpool);

            // (tire squeal removed — the synthesized tone read as a DJ effect;
            //  a real squeal needs a noise-based sample, not a sine sweep)
        }

        // Chase camera.
        const float dx = std::cos(camPitch) * std::cos(camYaw);
        const float dy = std::sin(camPitch);
        const float dz = std::cos(camPitch) * std::sin(camYaw);
        // CHASE-CAM COLLISION ("clipping"). The camera was pure trigonometry with
        // no collision query at all, so it swung straight through the tunnel
        // shell, the cutting walls and the terrain — you could look at the bore
        // from inside the rock. Tim, 2026-08-14: "The Tunnel... should also have
        // clipping" / "looking under the ground makes the asphalt disappear".
        //
        // Cast from the car's head position out along the boom; if anything solid
        // is in the way, pull the camera in to just short of it. Static mask, so
        // the world stops the camera but the car itself and loose props do not.
        // cam_collide 0 disables it (console cvar, see below).
        const float back = 9.0f;
        float cx = vp[0] - dx * back, cy = vp[1] + 3.2f - dy * back, cz = vp[2] - dz * back;

        // CAMERA vs WORLD. Two DIFFERENT rules, because they want different
        // behavior — the first cut used the wall rule for both and Tim
        // (2026-08-14) reported "camera Cannot go down to see under the car
        // anymore.. we need to clamp it AT the ground, but not UNDER the ground."
        //
        // 1) GROUND: do NOT shorten the boom. Keep the full 9 m and just refuse to
        //    go below the surface — the camera SLIDES along the ground, so you can
        //    still pitch right down and look up at the car from grass level. This
        //    is the "clamp at the ground" most games do.
        {
            const float gy = x3::game::terrainHeightAtWorld(cx, cz);
            const float kGroundClear = 0.35f;               // keep the near plane out of the dirt
            // ONLY CLAMP WHEN THE GROUND IS ACTUALLY BELOW YOU.
            // Inside the bore the height field at the camera's XZ is the MOUNTAIN
            // ROOF — a hundred-odd meters up — so an unconditional "stay above
            // the terrain" rule fired the camera straight into the rock. Tim,
            // 2026-08-15, sent a shot from inside the mountain looking at the
            // underside of the world.
            // Under cover the surface overhead is a CEILING, not a floor, and the
            // wall raycast below is the right constraint. Test against the CAR's
            // height, not the camera's: the car is on the carriageway by
            // definition, so terrain far above it means we are in the tunnel or a
            // deep cutting.
            const bool underCover = gy > vp[1] + 2.0f;
            if (!underCover && cy < gy + kGroundClear) cy = gy + kGroundClear;
        }
        // 2) WALLS: a raycast DOES shorten the boom, so the shell, the cutting
        //    faces and the headwall still stop the camera instead of letting it
        //    swim through into the rock. Cast to the ground-clamped position so a
        //    low angle is not mistaken for a wall hit.
        {
            const float pivotY = vp[1] + 1.4f;              // roughly the roof line
            float ox = cx - vp[0], oy = cy - pivotY, oz = cz - vp[2];
            const float len = std::sqrt(ox*ox + oy*oy + oz*oz);
            if (len > 0.05f) {
                ox /= len; oy /= len; oz /= len;
                const x3::phys::RayHit h = phys->rayCast(
                    x3::phys::Vec3{ vp[0], pivotY, vp[2] },
                    x3::phys::Vec3{ ox, oy, oz }, len, x3::phys::Layer::Static);
                if (h.hit) {
                    const float kSkin = 0.45f;
                    const float d = std::max(1.6f, h.distance - kSkin);
                    cx = vp[0] + ox * d; cy = pivotY + oy * d; cz = vp[2] + oz * d;
                    // Re-assert the ground rule after pulling in — the shortened
                    // point can still land under a rise.
                    const float gy2 = x3::game::terrainHeightAtWorld(cx, cz);
                    if (gy2 <= vp[1] + 2.0f && cy < gy2 + 0.35f) cy = gy2 + 0.35f;
                }
            }
        }

        // The listener IS the chase camera, so the note pans and attenuates as
        // you orbit the car and swells correctly inside the bore.
        if (audioOn) audio->setListener(cx, cy, cz, camYaw, camPitch);

        int cw, ch; glfwGetFramebufferSize(window, &cw, &ch);
        if (cw != lastW || ch != lastH) { lastW = cw; lastH = ch; if (cw > 0 && ch > 0) device->onResize((uint32_t)cw, (uint32_t)ch); }
        // SPEED FOV. Physical speed alone does not read as fast on a screen —
        // the frame has to widen and the periphery has to rush. 72 deg parked ->
        // 88 flat out, eased so it swells under acceleration instead of snapping.
        // This is the 1990s arcade trick and it is still the highest
        // feel-per-line change available (see TUNNEL_NEXT.md section 2 on NFS).
        {
            const float sp   = carBuilt ? std::fabs(car.forwardSpeed()) : 0.0f;
            const float t    = std::min(1.0f, sp / 55.0f);       // ~123 mph = full
            const float want = 72.0f + 16.0f * t * t;            // eased, not linear
            static float fovNow = 72.0f;
            fovNow += (want - fovNow) * std::min(1.0f, fdt * 3.0f);   // smooth
            device->setCamera(cx, cy, cz, camYaw, camPitch, fovNow);
        }
        auto frame = device->beginFrame();
        if (frame.valid) { scene.render(*device, frame); if (carBuilt) car.render(frame); }

        // ---- WHEEL-SPIN FX: spawn skid marks + smoke when the rears slip ----
        if (frame.valid && carBuilt) {
            const float slip = car.maxSlip();
            fxSpawnAcc += fdt;
            if (slip > 0.06f && fxSpawnAcc > 0.03f) {
                fxSpawnAcc = 0.0f;
                x3::phys::WheelState ws;
                for (uint32_t i = 0; i < car.controller()->wheelCount(); ++i) {
                    if (!car.controller()->wheelState(i, ws)) continue;
                    if (i < 2) continue;                       // rear wheels only
                    if (fxN < 512) {
                        SpinFx& f = fx[fxN++];
                        f.x = ws.worldTransform[12]; f.y = ws.worldTransform[13]; f.z = ws.worldTransform[14];
                        f.age = 0.0f;
                        f.kind = (slip > 0.18f) ? 1 : 0;       // hard spin -> smoke
                    }
                }
            }
            uint32_t w = 0;
            for (uint32_t i = 0; i < fxN; ++i) {
                SpinFx& f = fx[i];
                f.age += fdt;
                if (f.kind == 0) { if (f.age > 12.0f) continue; }
                else { f.y += fdt * 1.6f; if (f.age > 1.8f) continue; }
                fx[w++] = f;
            }
            fxN = w;
            for (uint32_t i = 0; i < fxN; ++i) {
                SpinFx& f = fx[i];
                float m[16] = {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,1};
                float col[4] = {1,1,1,0};
                if (f.kind == 0) {
                    const float a = std::max(0.0f, 1.0f - f.age / 12.0f) * 0.75f;
                    col[3] = a;
                    m[0] = 0.22f; m[5] = 0.02f; m[10] = 1.1f;
                    m[12] = f.x; m[13] = f.y + 0.04f; m[14] = f.z;
                    device->drawMesh(frame, fxQuadMesh, fxSkidTex, col, m);
                } else {
                    const float t = f.age / 1.8f;
                    const float a = std::max(0.0f, 1.0f - t) * 0.45f;
                    col[3] = a;
                    const float s = 0.25f + 0.9f * t;
                    m[0] = m[5] = m[10] = s;
                    m[12] = f.x; m[13] = f.y + 0.4f; m[14] = f.z;
                    device->drawMesh(frame, fxQuadMesh, fxSmokeTex, col, m);
                }
            }
        }
        // ---- INSTRUMENT CLUSTER (textured) ---------------------------------
        // Three drawHudImage calls plus a little text. The dial and the shift
        // gate are real anti-aliased artwork; the needle is a 64-frame rotation
        // atlas indexed by rpm, so the sweep stays clean at every angle.
        // The previous version approximated the dial with ~400 axis-aligned
        // quads because the brief said "rectangles only" — but drawHudImage
        // takes a TEXTURE with UV sub-rects, so the right reading was "put real
        // art IN the rectangle". Owner's verdict on the quad build: "slop in
        // Carbon esque shape". Art pipeline: tools/render_gauge_bezel.py renders
        // the chrome rim in Blender (metal IS reflection — 2D fake gloss never
        // convinces), tools/compose_gauge_dial.py draws the scale over it and
        // bakes the needle atlas, tools/make_gauge_textures.py makes the gate.
        // The dial face carries NO text: the gear digit and the MPH readout
        // below own those two strips, and baked labels collided with them.
        if (frame.valid && carBuilt && texDial.valid()) {
            int fbw = 0, fbh = 0; glfwGetFramebufferSize(window, &fbw, &fbh);
            const float fw = (float)fbw, fh = (float)fbh;
            // LAYOUT. The whole cluster is dial (2R tall) + gap + gate (0.9R),
            // so it needs 3.0R of vertical room; the first pass anchored on the
            // dial alone and pushed the gate and the TC line off the bottom of
            // the screen.
            const float R   = 0.150f * fh;
            const float mar = 0.030f * fh;
            const float gateH = R * 0.90f;
            const float gcx = fw - mar - R;
            const float gcy = fh - mar - gateH - R * 0.12f - R;

            const float rpmNow = car.engineRPM();
            const float frac   = std::min(1.0f, std::max(0.0f, rpmNow / 8000.0f));

            // Framerate-independent needle smoothing — raw rpm buzzes at 165 Hz.
            static float shownFrac = 0.0f;
            shownFrac += (frac - shownFrac) * (1.0f - std::exp(-9.0f * fdt));

            const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
            device->drawHudImage(frame, texDial, gcx - R, gcy - R, 2.0f * R, 2.0f * R, white);

            if (texNeedle.valid()) {
                const int NF = 64, AT = 8;
                int fi = (int)(shownFrac * (NF - 1) + 0.5f);
                fi = fi < 0 ? 0 : (fi > NF - 1 ? NF - 1 : fi);
                const float u0 = (float)(fi % AT) / (float)AT;
                const float v0 = (float)(fi / AT) / (float)AT;
                device->drawHudImage(frame, texNeedle, gcx - R, gcy - R, 2.0f * R, 2.0f * R,
                                     white, u0, v0, u0 + 1.0f / AT, v0 + 1.0f / AT);
            }
            if (texGate.valid()) {
                const float gw = gateH * 2.0f, gh = gateH;
                device->drawHudImage(frame, texGate, gcx - gw * 0.5f,
                                     gcy + R + R * 0.12f, gw, gh, white);
            }

            // ---- BOOST GAUGE — vertical segmented bar (NFS style) ----------
            // A slim vertical bar along the LEFT of the tach, outside it, ~60% of
            // the dial height. Discrete segments fill bottom-up: green -> bright
            // orange -> blue at max. "BOOST" label + digital psi. Off-throttle it
            // reads vacuum, which just empties the bar (a real gauge's tell).
            {
                const float barW = R * 0.30f;              // slim
                const float barH = R * 1.20f;              // ~60% of the 2R dial
                const float gap  = R * 0.44f;              // clear of the tach
                const float bx   = gcx - R - gap - barW;   // outside, left
                const float by   = gcy + R - barH;         // bottom-aligned with the dial

                const float psi = car.boostPsi();
                const float bf  = std::min(1.0f, std::max(0.0f, psi / 35.0f)); // 35 psi = full
                static float shownBoost = 0.0f;
                shownBoost += (bf - shownBoost) * (1.0f - std::exp(-12.0f * fdt));

                // Background + thin border (reads as a rounded, finished bezel).
                const float bgc[4] = { 0.05f, 0.07f, 0.09f, 0.90f };
                device->drawHudQuad(frame, bx - 1.5f, by - 1.5f, barW + 3.0f, barH + 3.0f, bgc);
                const float bdc[4] = { 0.42f, 0.47f, 0.52f, 1.0f };
                device->drawHudQuad(frame, bx - 1.5f, by - 1.5f, barW + 3.0f, 1.0f, bdc);
                device->drawHudQuad(frame, bx - 1.5f, by + barH + 0.5f, barW + 3.0f, 1.0f, bdc);
                device->drawHudQuad(frame, bx - 1.5f, by - 1.5f, 1.0f, barH + 3.0f, bdc);
                device->drawHudQuad(frame, bx + barW + 0.5f, by - 1.5f, 1.0f, barH + 3.0f, bdc);

                // Lit segments fill bottom-up: green -> bright orange -> blue.
                constexpr int kSegs = 10;
                const float segH = barH / kSegs;
                const int lit = (int)(shownBoost * kSegs + 0.5f);
                for (int i = 0; i < lit; ++i) {
                    const float p = (float)i / (kSegs - 1);
                    float sc[4];
                    if (p < 0.33f)      { sc[0]=0.20f; sc[1]=0.95f; sc[2]=0.35f; } // green
                    else if (p < 0.66f) { sc[0]=1.00f; sc[1]=0.55f; sc[2]=0.08f; } // bright orange
                    else                { sc[0]=0.28f; sc[1]=0.62f; sc[2]=1.00f; } // blue
                    sc[3] = 1.0f;
                    const float sy = by + barH - (i + 1) * segH;
                    device->drawHudQuad(frame, bx, sy + 0.5f, barW, segH - 1.0f, sc);
                }

                // Digital psi above, "BOOST" label below.
                char bbuf[32];
                std::snprintf(bbuf, sizeof(bbuf), "%+.0f", (double)psi);
                const float bp = R * 0.20f;
                const float bw = (float)std::strlen(bbuf) * bp;
                const float tc[4] = { 0.90f, 0.95f, 1.0f, 1.0f };
                device->drawHudText(frame, bbuf, bx + barW * 0.5f - bw * 0.5f,
                                    by - R * 0.36f, bp, tc);
                const float lp = R * 0.12f;
                device->drawHudText(frame, "BOOST", bx + barW * 0.5f - 2.5f * lp,
                                    by + barH + R * 0.10f, lp, tc);
            }

            char gbuf[64];
            const int   gnum = car.gear();
            const float mph  = std::fabs(car.forwardSpeed()) * 2.23694f;

            std::snprintf(gbuf, sizeof(gbuf), "%d", (int)(mph + 0.5f));
            {
                // 0.275R, not 0.34R: at three digits the wider face ran into the
                // "0" and "8" numerals, which sit at x = +-0.455R.
                const float px = R * 0.275f;
                const float w  = (float)std::strlen(gbuf) * px;
                const float col[4] = { 0.97f, 0.98f, 1.0f, 1.0f };
                device->drawHudText(frame, gbuf, gcx - w * 0.5f, gcy + R * 0.235f, px, col);
                const float lp = R * 0.095f;
                const float lc[4] = { 0.35f, 0.78f, 0.95f, 1.0f };   // cyan, per the reference
                device->drawHudText(frame, "MPH", gcx - 1.5f * lp, gcy + R * 0.55f, lp, lc);
            }
            {
                const char* gs = (gnum < 0) ? "R" : (gnum == 0 ? "N" : "123456" + ((gnum - 1) % 6));
                char one[2] = { gs[0], 0 };
                const bool hot = rpmNow > 7312.0f * 0.985f;
                const float px = R * 0.22f;
                const float col[4] = { hot ? 1.0f : 0.35f, hot ? 0.30f : 0.82f,
                                       hot ? 0.22f : 0.98f, 1.0f };
                device->drawHudText(frame, one, gcx - px * 0.5f, gcy - R * 0.46f, px, col);
            }
            {   // shift lights along the top of the bezel
                const int   NL = 8;
                const float lw = R * 0.115f, lh = R * 0.052f, gp = lw * 0.30f;
                const float tot = NL * lw + (NL - 1) * gp;
                const float x0 = gcx - tot * 0.5f, y0 = gcy - R * 1.17f;
                const float lit = std::min(1.0f, std::max(0.0f, (rpmNow - 6000.0f) / 1312.0f));
                const bool  fl  = rpmNow >= 7312.0f && std::fmod((float)now * 4.5f, 1.0f) < 0.5f;
                for (int i = 0; i < NL; ++i) {
                    const bool on = lit >= (float)(i + 1) / (float)NL || fl;
                    const float tt = (float)i / (float)(NL - 1);
                    float c4[4];
                    if (fl)       { c4[0]=1.0f; c4[1]=0.16f; c4[2]=0.12f; c4[3]=1.0f; }
                    else if (!on) { c4[0]=0.12f; c4[1]=0.14f; c4[2]=0.18f; c4[3]=0.8f; }
                    else          { c4[0]=0.25f+0.75f*tt; c4[1]=0.85f-0.58f*tt;
                                    c4[2]=0.98f-0.84f*tt; c4[3]=1.0f; }
                    device->drawHudQuad(frame, x0 + i * (lw + gp), y0, lw, lh, c4);
                }
            }
            {   // Key hints on the glass. A binding nobody can see does not
                // exist: T toggled traction control for a whole session while
                // the only mention of it went to a log file.
                const float hp = R * 0.085f;
                const float hcol[4] = { 0.52f, 0.57f, 0.66f, 1.0f };
                device->drawHudText(frame, "~  CONSOLE",      gcx - R * 0.95f,
                                    gcy - R * 1.64f, hp, hcol);
                device->drawHudText(frame, "T  TRACTION",     gcx - R * 0.95f,
                                    gcy - R * 1.52f, hp, hcol);
                device->drawHudText(frame, "SPACE  HANDBRAKE", gcx - R * 0.95f,
                                    gcy - R * 1.40f, hp, hcol);
            }
            {
                const bool tcOn = car.tractionControl();
                const float px = R * 0.105f;
                const float c4[4] = { tcOn ? 0.35f : 1.0f, tcOn ? 0.78f : 0.58f,
                                      tcOn ? 0.95f : 0.20f, 1.0f };
                const char* t = tcOn ? "TC" : "TC OFF";
                device->drawHudText(frame, t, gcx - (float)std::strlen(t) * px * 0.5f,
                                    gcy - R * 1.30f, px, c4);
            }
        }
        shell.draw(frame, fdt);      // console + FPS/stats, over everything
        device->endFrame(frame);
    }

    tunnel.shutdown(*device, *phys);
    streamer.shutdown(scene, *device, *phys);
    jobs->shutdown(); phys->shutdown(); device->shutdown();
    if (window) glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

}} // namespace x3::apphost
