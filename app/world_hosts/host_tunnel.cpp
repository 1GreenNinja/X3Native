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
#include "engine/core/IJobSystem.h"
#include "engine/physics/IVehicle.h"
#include "../scene.h"
#include "../terrain.h"
#include "../tunnel_corridor.h"
#include "../road_network.h"
#include "../vehicle.h"
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

    // Render-pass A/B (`--set r_ssao 0` etc.) is NOT wired here any more. This
    // host used to call its own applyWorldHostRenderCVars(); fold-0812 landed
    // fix/world-host-cvars, which does the same thing for EVERY route from
    // runRoute() in world_hosts.cpp before the host body runs — with a strict
    // superset of the cvars (50 vs 37, none dropped) plus the run-long override
    // latch and the unapplied-cvar report. A per-host call is exactly the trap
    // that generalization removes, so the local one is gone rather than doubled.
    // ==== STEP 1 — REGISTER THE CORRIDOR, BEFORE ANY HEIGHT CONSUMER =========
    // app/terrain.h's contract: "Register corridors at BOOT, BEFORE the first
    // height query / TerrainStreamer::init()". Everything below (the streamer,
    // the horizon ring, the road grading, the car spawn) reads the field AFTER
    // this line, so they all agree by construction.
    const x3::game::TunnelRoute& route = x3::game::registerTunnelCorridor();

    // THE 15-MILE INNER TOUR. X3_RING=1 lays it in this world so it can be
    // driven; off by default so the tunnel demo is untouched. Registered HERE,
    // beside the corridor above, because app/terrain.h's contract is "register
    // before the first height query" and this is the last moment that is true.
    x3::game::RoadSpec ringSpec;
    bool ringOn = false;
    {
        const char* e = std::getenv("X3_RING");
        ringOn = (e && e[0] == '1');
        if (ringOn) {
            ringSpec = x3::game::makeRingRoad("inner tour", -592.0f, -352.0f, 3842.0f, 396);
            ringSpec.halfWidth = x3::game::kPavedHalfM + 1.0f;
            ringSpec.falloff   = 18.0f;
            ringSpec.maxGrade  = 0.07f;
            const x3::game::RoadBuildResult rr = x3::game::registerRoad(ringSpec);
            if (!rr.ok) { x3::logError("--world tunnel: ring registration FAILED"); ringOn = false; }
        }
    }

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
    if (ringOn && ringSpec.x.size() > 2) {
        // Stand on the ring itself: its first node, lifted to the graded datum.
        startPos[0] = ringSpec.x[0];
        startPos[2] = ringSpec.z[0];
        startPos[1] = x3::game::terrainHeightAtWorld(startPos[0], startPos[2]) + 1.0f;
    }

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
    // The streamer's ground texture IS the terrain splat MARKER. Handing it to
    // the tunnel is what lets the BACKFILL LID — the mesh that carries the
    // hillside back over the cut-and-cover bore — shade through the same
    // height/slope splat as the streamed tiles instead of reading as a separate
    // object draped over the hill. Without it the build warns and falls back.
    tunnel.build(scene, *device, *phys, route, streamer.groundTexture());
    // The ribbon: 4 lanes of asphalt plus a 20 ft cement apron each side, laid
    // into the cutting the corridor already graded.
    if (ringOn) x3::game::buildRoadRibbon(ringSpec, scene, *device, *phys);
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
    if (audioOn) {
        const std::string wav =
            (std::filesystem::path(x3::game::assetRoot()) / "audio/vehicles/engine_loop.wav").string();
        engineSnd = audio->load(wav);
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
    x3::rhi::TextureHandle texDial{}, texNeedle{}, texGate{};
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
                { 5, "06_mouth_headon" },
                { 6, "07_inside_looking_out" },
                { 7, "08_exit_portal" },
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
        // Shared across every bore, so it is released ONCE here rather than by
        // each tunnel's own shutdown (which would free textures its neighbours
        // are still drawing with).
        x3::game::shutdownTunnelSurfaces(*device);
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
    x3::logInfo("--world tunnel: WASD drives, Space handbrake, mouse orbits the chase cam, Esc quits");

    while (!glfwWindowShouldClose(window)) {
        // RE-SUBMIT THE BORE LIGHTS EVERY FRAME. They were set exactly ONCE at boot
        // (setPointLights above), which is why the tunnel is lit in headless captures
        // — those render a few frames with nothing else touching the light set — and
        // PITCH BLACK the moment you drive it, both from inside and looking in through
        // the portal from outside. The interactive loop streams tiles and draws other
        // content, and the light array does not survive that. Cheap: 6 cached lights.
        glfwPollEvents();
        // ESC OPENS THE MENU, IT DOES NOT QUIT. Tim was losing his session every
        // time he reached for it. ESC toggles a PAUSE: the sim stops, the cursor
        // is released so the window can be moved/alt-tabbed, and driving resumes
        // on the next press. SHIFT+ESC is the deliberate way out, so quitting
        // stays possible but can no longer happen by reflex.
        //
        // THE OVERLAY IS NOT DECORATION. The first cut of this paused silently —
        // it only logged to a console the player is not looking at — and a paused
        // game is pixel-identical to a hung one. Tim hit ESC, got a live window
        // that ignored the throttle, and reported "The car doesnt move AT ALL".
        // A mode the player cannot see is a freeze with extra steps, so the state
        // is drawn on the glass, along with the keys that leave it.
        {
            static bool escWasDown = false, paused = false;
            const bool escDown = glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS;
            const bool shift = glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS
                            || glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;
            if (escDown && !escWasDown) {
                if (shift) break;                      // SHIFT+ESC = quit
                paused = !paused;
                glfwSetInputMode(window, GLFW_CURSOR,
                                 paused ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_DISABLED);
                x3::logInfo(paused ? "[tunnel] PAUSED — ESC resumes, SHIFT+ESC quits"
                                   : "[tunnel] resumed");
            }
            escWasDown = escDown;
            if (paused) {
                // Present the frame so the window stays live, but do not advance
                // the sim or read drive input.
                auto pf = device->beginFrame();
                if (pf.valid) {
                    scene.render(*device, pf);
                    if (carBuilt) car.render(pf);

                    int pw = 0, ph = 0; glfwGetFramebufferSize(window, &pw, &ph);
                    const float fw = (float)pw, fh = (float)ph;

                    // Dim the world so the menu reads as a layer above it.
                    const float scrim[4] = { 0.0f, 0.0f, 0.0f, 0.55f };
                    device->drawHudQuad(pf, 0.0f, 0.0f, fw, fh, scrim);

                    // Glyph cell is square and fixed for the mono role, so the
                    // legacy N*px centering math is exact.
                    auto centered = [&](const char* s, float px, float y, const float c[4]) {
                        const float w = (float)std::strlen(s) * px;
                        device->drawHudText(pf, s, (fw - w) * 0.5f, y, px, c);
                    };

                    const float title[4] = { 1.00f, 0.93f, 0.72f, 1.0f };
                    const float body [4] = { 0.86f, 0.88f, 0.92f, 1.0f };
                    const float dim  [4] = { 0.55f, 0.58f, 0.64f, 1.0f };

                    const float tp = std::floor(fh * 0.055f);   // title glyph px
                    const float bp = std::floor(fh * 0.026f);   // body  glyph px
                    float y = fh * 0.34f;

                    centered("PAUSED", tp, y, title);            y += tp * 2.2f;
                    centered("ESC          resume driving", bp, y, body); y += bp * 1.8f;
                    centered("SHIFT + ESC  quit to desktop", bp, y, body); y += bp * 2.4f;
                    centered("the sim is stopped - this is not a freeze", bp * 0.85f, y, dim);
                }
                device->endFrame(pf);
                continue;
            }
        }
        const double now = glfwGetTime();
        float fdt = (float)(now - prevTime); prevTime = now;
        if (fdt > 0.1f) fdt = 0.1f;
        double mx, my; glfwGetCursorPos(window, &mx, &my);
        const float ddx = (float)(mx - lastMX), ddy = (float)(my - lastMY);
        lastMX = mx; lastMY = my;
        camYaw += ddx * 0.0025f; camPitch -= ddy * 0.0025f;
        if (camPitch >  1.2f) camPitch =  1.2f;
        if (camPitch < -1.2f) camPitch = -1.2f;
        auto kd = [&](int k){ return glfwGetKey(window, k) == GLFW_PRESS; };

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
            const float rpm    = car.engineRPM();
            const float redline = 7500.0f;                       // matches vd.maxEngineRPM
            const float frac   = std::min(1.0f, std::max(0.0f, rpm / redline));
            // PITCH follows RPM. 1.05 at idle -> 2.80 at the limiter. The first
            // pass ran 0.75 at idle, which stretched the sample DOWN and made it
            // rattle like a diesel (Tim: "a little.. too rattly").
            const float pitch  = 1.05f + frac * 1.75f;

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
            const float load = thr * torqueFrac(frac);
            // Off-throttle is OVERRUN: the engine is being driven by the wheels,
            // so it stays audible and keeps its pitch but drops right back in
            // level. That contrast is most of what makes a car sound driven.
            const float vol  = 0.16f + 0.62f * load + 0.10f * frac;
            audio->setLoopParams(engineLoop, vol, pitch);
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
        // MERGED NEAREST-K TUNNEL LIGHTS, keyed on the camera we are about to
        // render from — not this bore's whole array. A dressed bore spends 8
        // real lights (6 down the barrel + 1 per mouth); four city bores would
        // take 32 and eight network bores 64, the entire legacy budget. You can
        // only be inside one tunnel, so upload the nearest K and let the rest
        // cost nothing. Per-frame, so it also cannot go stale — which is the
        // other half of the "lit in headless capture, black when driven" bug.
        { const float cp[3] = { cx, cy, cz };
          x3::game::uploadTunnelLights(*device, cp); }
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
        // ---- INSTRUMENT CLUSTER (textured) ---------------------------------
        // Three drawHudImage calls plus a little text. The dial and the shift
        // gate are real anti-aliased artwork; the needle is a 64-frame rotation
        // atlas indexed by rpm, so the sweep stays clean at every angle.
        // The previous version approximated the dial with ~400 axis-aligned
        // quads because the brief said "rectangles only" — but drawHudImage
        // takes a TEXTURE with UV sub-rects, so the right reading was "put real
        // art IN the rectangle". Owner's verdict on the quad build: "slop in
        // Carbon esque shape". Art lives in tools/make_gauge_textures.py.
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

            char gbuf[64];
            const int   gnum = car.gear();
            const float mph  = std::fabs(car.forwardSpeed()) * 2.23694f;

            std::snprintf(gbuf, sizeof(gbuf), "%d", (int)(mph + 0.5f));
            {
                const float px = R * 0.34f;
                const float w  = (float)std::strlen(gbuf) * px;
                const float col[4] = { 0.97f, 0.98f, 1.0f, 1.0f };
                device->drawHudText(frame, gbuf, gcx - w * 0.5f, gcy + R * 0.20f, px, col);
                const float lp = R * 0.095f;
                const float lc[4] = { 0.35f, 0.78f, 0.95f, 1.0f };   // cyan, per the reference
                device->drawHudText(frame, "MPH", gcx - 1.5f * lp, gcy + R * 0.58f, lp, lc);
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
            {   // key hints, on screen, because this host has no console
                const float hp = R * 0.085f;
                const float hc[4] = { 0.52f, 0.57f, 0.66f, 1.0f };
                device->drawHudText(frame, "T  TRACTION",  gcx - R * 0.95f,
                                    gcy - R * 1.52f, hp, hc);
                device->drawHudText(frame, "SPACE  HANDBRAKE", gcx - R * 0.95f,
                                    gcy - R * 1.40f, hp, hc);
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
        device->endFrame(frame);
    }

    tunnel.shutdown(*device, *phys);
    x3::game::shutdownTunnelSurfaces(*device);   // shared sets, released once
    streamer.shutdown(scene, *device, *phys);
    jobs->shutdown(); phys->shutdown(); device->shutdown();
    if (window) glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

}} // namespace x3::apphost
