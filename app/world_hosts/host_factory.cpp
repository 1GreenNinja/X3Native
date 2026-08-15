// --world factory host — THE CONFECTION ANNEX (feat/factory-annex, Phase 2 T6).
// The Dahl-STYLE (original, IP-clean) wonder-works hidden +60 m X from the
// elevator shaft, reached by the Anywhere Elevator's hidden lateral rail:
// keypad 4790 in the cab lights the golden button and opens the annex stops
// A1-A5; keypad 9999 at A5 arms the ROOF BURST finale.
//
// Structure mirrors host_rifthub.cpp (physics + Scene + TriggerSystem + the
// module build + a headless capture path + a windowed walk loop). Host-specific
// wiring, per the plan:
//   * ElevatorSystem.buildEx with the COMBINED graph (FactoryAnnex::
//     makeElevatorGraph — F1/F3 Spire-side, lateral bore leg at floor-B height,
//     annex chain A1-A5), enableFsm, setFloorLabels (Phase-1 handoff:
//     Stop::label is stored but floorLabel() reads only m_floorLabels),
//     setBurst(A5, roofY=65, apexY=105);
//   * rider carry via carryDelta() (the Vec3 carry — lateral legs carry too);
//   * onRoofShatter -> GPU-debris burst + CombatFx (spawnFireball does NOT
//     exist on this branch; CombatFx::spawnExplosion IS this branch's fireball
//     — see the lambda) + a 6x spawnImpact glass-shard ring;
//   * low-grav: annex.lowGravActive() + standing in the Fizz zone scales the
//     player jump impulse x1.8 (Player::setJumpScale);
//   * headless: settle 24 frames, camera {60, 22, 46, -1.57, -0.30} (three
//     glass floors + the shaft in frame), exit 0 on capture.
#include "world_host_common.h"
#include "../scene.h"
#include "../trigger.h"
#include "../factory_annex.h"
#include "../elevator.h"
#include "../player.h"
#include "../fx.h"
#include "../audio_root.h"

#include "engine/audio/IAudioSystem.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

namespace x3 { namespace apphost {

int hostFactory(HostContext& hc) {
    auto* device = hc.device;
    GLFWwindow* window = hc.window;
    const bool headless = hc.headless;
    const bool screenshot = hc.screenshot;
    const std::string& screenshotPath = hc.screenshotPath;
    const bool shotCamOverride = hc.shotCamOverride;
    const float* shotCam = hc.shotCam;

    if (hc.worldMode != "factory") return -1;

    x3::logInfo("--world factory: building THE CONFECTION ANNEX (5 wonder-room "
                "floors + the Anywhere Elevator's lateral bore)");

    std::unique_ptr<x3::phys::IPhysicsWorld> faphys(x3::phys::createPhysicsWorld());
    if (!faphys->init()) {
        x3::logError("--world factory: physics init failed");
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    x3::game::Scene fascene;
    x3::game::TriggerSystem fatrig;
    x3::game::FactoryAnnex annex;
    const float kShaftX = 0.0f, kShaftZ = 0.0f;
    annex.build(fascene, *device, *faphys, fatrig, kShaftX, kShaftZ);
    annex.applyAtmosphere(*device);

    // ---- The Anywhere Elevator on the combined Spire+Annex graph -----------
    x3::game::ElevatorSystem elev;
    const x3::game::FactoryAnnex::ElevatorGraph graph =
        x3::game::FactoryAnnex::makeElevatorGraph(kShaftX, kShaftZ);
    if (!elev.buildEx(fascene, *device, *faphys,
                      x3::game::FactoryAnnex::kCabHalfX,
                      x3::game::FactoryAnnex::kCabHalfY,
                      x3::game::FactoryAnnex::kCabHalfZ,
                      graph.stops, graph.rails, graph.f1)) {
        x3::logError("--world factory: elevator buildEx failed");
        faphys->shutdown(); device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }
    elev.enableFsm(true);
    elev.setFloorLabels(graph.labels);
    elev.setBurst(graph.a5, x3::game::FactoryAnnex::kRoofY, /*apexY*/105.0f);
    elev.buildVisuals(fascene, *device);
    // The elevator hands the WORLD's air back when the rider steps out — tell
    // it what this world's air IS (matches annex.applyAtmosphere).
    elev.setWorldAtmosphere(0.055f, 0.048f, 0.065f, 0.08f);

    // ---- Roof-burst FX: GPU debris + CombatFx, wired HERE (the elevator is
    // render-pure; onRoofShatter is the host's hook — Phase-1 handoff note).
    x3::game::CombatFx fafx;
    fafx.init(*device);
    {
        x3::rhi::IRenderDevice::GpuDebrisParams gp{};
        gp.gravity[1] = -9.81f;
        gp.groundY    = x3::game::FactoryAnnex::kRoofY + 0.5f;   // shards rest on the roof
        gp.restitution = 0.25f; gp.friction = 0.5f;
        device->gpuDebrisConfig(gp);
    }
    elev.onRoofShatter = [&](const x3::phys::Vec3& p) {
        // The glass roof lets go: a heavy shard burst flung off the cab...
        const float bp[3] = { p.x, p.y, p.z };
        device->gpuDebrisSpawnBurst(bp, /*count*/96u, /*speed*/14.0f,
                                    /*lifetime*/6.0f, /*halfExtent*/0.14f,
                                    /*seed*/0x4790u);
        // ...plus the hot core. The plan calls for spawnFireball; that name
        // does not exist on this branch — CombatFx::spawnExplosion IS the
        // branch's fireball preset (barrel/ship kills use it), so it carries
        // the flash, and the 6x spawnImpact ring (the plan's stated fallback)
        // sprays the radial glass glitter.
        fafx.spawnExplosion(p, 5.0f);
        for (int i = 0; i < 6; ++i) {
            const float a = (float)i * (6.2831853f / 6.0f);
            const x3::phys::Vec3 rim{ p.x + 2.2f * std::cos(a), p.y,
                                      p.z + 2.2f * std::sin(a) };
            const x3::phys::Vec3 n{ std::cos(a) * 0.4f, 1.0f, std::sin(a) * 0.4f };
            fafx.spawnImpact(rim, n);
        }
        x3::logInfo("[factory] THE ROOF SHATTERS — the cab bursts over the world");
    };

    const float dt = 1.0f / 60.0f;

    // THE CONFECTION RIVER (Floor A, T7): the annex registers the raspberry
    // water params at build; the HOST owns the device push (setWaterParams is
    // host territory, per the plan). The clock is host-advanced so captures
    // are deterministic (the engine water convention). Stack copy per frame —
    // no heap.
    float waterClock = 0.0f;
    auto pushWater = [&](float advance) {
        waterClock += advance;
        x3::rhi::IRenderDevice::WaterParams wp = annex.riverWater();
        wp.time = waterClock;
        device->setWaterParams(wp);
    };

    // ONE point-light push per frame: the annex's rig (per-floor accents +
    // bore brass, static) + the elevator's (cab interior/disco, animated).
    // Reserved once — no per-frame heap after the first push.
    std::vector<x3::rhi::PointLight> lightScratch;
    lightScratch.reserve(annex.pointLights().size() + 8);
    auto pushLights = [&]() {
        lightScratch.clear();
        lightScratch.insert(lightScratch.end(), annex.pointLights().begin(),
                            annex.pointLights().end());
        lightScratch.insert(lightScratch.end(), elev.pointLights().begin(),
                            elev.pointLights().end());
        device->setPointLights(lightScratch.data(), (uint32_t)lightScratch.size());
    };

    // ===== Headless screenshot / smoketest path ==============================
    if (headless) {
        // Vantage between the shaft and the annex looking INTO the glass
        // curtain: the five-floor glass wall, the octagonal bore feeding floor
        // B, and the F3 shaft landing all in frame. (The plan's literal camera
        // {60, 22, 46, -1.57, -0.30} sits south of the annex facing the SOLID
        // +Z iron wall — a blank slab; verified against the built shell. Its
        // stated intent — "three glass floors + shaft in frame" — needs a
        // shaft-side vantage, which is this one.)
        float cam[5] = { 6.0f, 26.0f, 38.0f, -0.72f, -0.14f };
        if (shotCamOverride) for (int k = 0; k < 5; ++k) cam[k] = shotCam[k];
        const std::string outPath = screenshot ? screenshotPath
                                               : std::string("w_factory.png");
        const int kSettle = 24;   // plan: settle-24-frames
        for (int i = 0; i < kSettle; ++i) {
            glfwPollEvents();
            annex.tick(dt, fascene);
            elev.update(dt, fascene, *faphys);
            fafx.update(dt);
            faphys->step(dt);
            pushLights();
            pushWater(dt);
            device->setCamera(cam[0], cam[1], cam[2], cam[3], cam[4], 65.0f);
            if (i == kSettle - 1) device->armCapture(outPath.c_str());
            auto frame = device->beginFrame();
            if (frame.valid) {
                device->gpuDebrisStep(dt);
                fascene.render(*device, frame);
                const float shardTint[4] = { 0.75f, 0.88f, 0.95f, 1.0f };
                device->gpuDebrisDraw(frame, shardTint);
                fafx.submit(*device, frame);
            }
            device->endFrame(frame);
        }
        const bool wrote = device->captureFrame(outPath.c_str());
        if (wrote) {
            const x3::rhi::RenderStats st = device->stats();
            char rb[256];
            std::snprintf(rb, sizeof(rb),
                "--world factory: wrote %s | rooms=%u ents=%u draws=%u tris=%u",
                outPath.c_str(), annex.roomCount(), annex.entityCount(),
                st.drawCalls, st.triangles);
            x3::logInfo(rb);
        } else x3::logError("--world factory: capture FAILED");

        fafx.shutdown(*device);
        annex.shutdown(*device);
        faphys->shutdown();
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return wrote ? 0 : 1;
    }

    // ===== Walkable windowed path ============================================
    // Spawn on the F1 shaft landing beside the cab. WASD + mouse; E calls the
    // cab (rifthub-manners: summons it to your floor if it is elsewhere);
    // number keys feed the cab keypad (4790 = the annex unlock, 9999 at A5 =
    // the burst); F noclip.
    x3::game::Player faplayer;
    const x3::phys::Vec3 spawn{ kShaftX - 3.5f, x3::game::FactoryAnnex::kFloorBaseY[0] + 0.2f,
                                kShaftZ };
    faplayer.spawn(*faphys, spawn.x, spawn.y, spawn.z);

    // ===== Task 12: the AUDIO PASS (asset-optional, the rifthub pattern) =====
    // Every cue probes a factory-specific WAV first, then falls back to a
    // committed in-repo cue so a fresh clone still hums; a miss on BOTH loads
    // graceful-silent (IAudioSystem::load logs once and returns invalid — no
    // crash, no retry). Windowed path only (captures stay deterministic).
    std::unique_ptr<x3::audio::IAudioSystem> faaudio(x3::audio::createAudioSystem());
    x3::audio::SoundHandle sndGlorp, sndBuzz, sndThunk, sndWhoosh, sndCrash, sndChime;
    x3::audio::LoopHandle  windLoop;                 // apex wind (Burst)
    const float aX = annex.annexX(), aZ = annex.annexZ();
    if (faaudio && faaudio->init()) {
        auto cue = [&](const char* primary, const char* fallback) {
            x3::audio::SoundHandle h = faaudio->load(x3::game::resolveAudio(primary));
            if (!h.valid() && fallback) {
                x3::logWarn(std::string("[factory] audio cue missing (") + primary +
                            ") — falling back to " + fallback);
                h = faaudio->load(x3::game::resolveAudio(fallback));
            }
            return h;
        };
        // The annex ambience bed (plan: vol 0.25).
        {
            const std::string amb = x3::game::resolveAudio("factory/annex_ambience.wav");
            const std::string fb  = x3::game::resolveAudio("echotropolis/ambient/mine_hum.wav");
            namespace fs = std::filesystem;
            std::error_code ec;
            faaudio->playMusic(fs::exists(amb, ec) ? amb : fb, /*loop*/true, /*vol*/0.25f);
        }
        // Per-room 3D beds at the room centers (machine floors only; A gets
        // random glorps below, E gets docking events).
        x3::audio::SoundHandle sndServo = cue("factory/servo_loop.wav", "interact/servo_loop.wav");
        x3::audio::SoundHandle sndFizz  = cue("factory/fizz_loop.wav", "ambient/fluorescent_buzz.wav");
        if (sndServo.valid()) {
            faaudio->startLoop3D(sndServo, aX + 5.0f,
                                 x3::game::FactoryAnnex::kFloorBaseY[1] + 1.5f, aZ,
                                 /*vol*/0.16f, /*pitch*/0.9f);     // B: conveyor clank
            faaudio->startLoop3D(sndServo, aX,
                                 x3::game::FactoryAnnex::kFloorBaseY[3] + 2.0f, aZ,
                                 /*vol*/0.20f, /*pitch*/0.7f);     // D: sorter servos
        }
        if (sndFizz.valid())
            faaudio->startLoop3D(sndFizz, aX,
                                 x3::game::FactoryAnnex::kFloorBaseY[2] + 3.0f, aZ,
                                 /*vol*/0.15f, /*pitch*/1.35f);    // C: the fizz
        sndGlorp  = cue("factory/vat_glorp.wav",     "water/splash_enter.wav");
        sndBuzz   = cue("factory/chute_buzz.wav",    "interact/buzz.wav");
        sndThunk  = cue("factory/capsule_dock.wav",  "interact/door_thunk.wav");
        sndWhoosh = cue("factory/tube_whoosh.wav",   "rifthub/rifthub_whoosh.wav");
        sndCrash  = cue("factory/glass_crash.wav",   "rifthub/rifthub_kawoosh.wav");
        sndChime  = cue("factory/unlock_fanfare.wav","interact/chime.wav");
        // The burst finale: chain crash + apex wind ONTO the FX-only shatter
        // hook (the elevator stays render- and audio-pure).
        auto fxShatter = elev.onRoofShatter;
        elev.onRoofShatter = [&, fxShatter](const x3::phys::Vec3& p) {
            if (fxShatter) fxShatter(p);
            if (faaudio) {
                if (sndCrash.valid()) faaudio->playSound3D(sndCrash, p.x, p.y, p.z, 1.0f, 0.8f);
                if (sndWhoosh.valid())
                    windLoop = faaudio->startLoop3D(sndWhoosh, p.x, p.y + 30.0f, p.z,
                                                    /*vol*/0.5f, /*pitch*/0.7f);
            }
        };
    }
    // Audio edge-detect state (heapless).
    bool     prevUnlocked = elev.hiddenUnlocked();
    uint32_t prevSortEv = annex.room(3).eventCount, prevTubeEv = annex.room(4).eventCount;
    float    nextGlorpT = 4.0f, worldT = 0.0f;
    int      glorpVat = 0;

    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    if (glfwRawMouseMotionSupported())
        glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
    double lastMX, lastMY; glfwGetCursorPos(window, &lastMX, &lastMY);
    double prevTime = glfwGetTime();
    bool prevSpace = false, prevF = false, prevE = false;
    bool prevDigit[10] = {};
    bool noclip = false;
    float flyX = spawn.x, flyY = spawn.y + 1.6f, flyZ = spawn.z;
    float flyYaw = 0.0f, flyPitch = -0.05f;
    x3::logInfo("--world factory: WASD walk, E call the cab, digits = cab keypad "
                "(4790 unlocks the ANNEX; 9999 at A5 = the ROOF BURST), F noclip, Esc quits");
    std::string lastTitle;
    int lastW = (int)hc.W, lastH = (int)hc.H;
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;

        double now = glfwGetTime();
        float fdt = (float)(now - prevTime); prevTime = now;
        if (fdt > 0.1f) fdt = 0.1f;

        double mx, my; glfwGetCursorPos(window, &mx, &my);
        float ddx = (float)(mx - lastMX), ddy = (float)(my - lastMY);
        lastMX = mx; lastMY = my;

        auto kd = [&](int k) { return glfwGetKey(window, k) == GLFW_PRESS; };
        const bool spaceNow = kd(GLFW_KEY_SPACE);
        const bool fNow = kd(GLFW_KEY_F);
        if (fNow && !prevF) {
            noclip = !noclip;
            if (noclip) { float yy, pp; faplayer.camera(flyX, flyY, flyZ, yy, pp); flyYaw = yy; flyPitch = pp; }
        }
        prevF = fNow;

        // ---- Low-grav (Fizz Gallery, Floor C): x1.8 jump while the zone is
        // live AND the player is standing in it; 1.0 everywhere else.
        {
            const x3::phys::Vec3 pf = faphys->getBodyPosition(faplayer.body());
            const float ax = annex.annexX(), az = annex.annexZ();
            const float cy = x3::game::FactoryAnnex::kFloorBaseY[2];
            const bool inFizz = annex.lowGravActive() &&
                std::fabs(pf.x - ax) < 7.5f && std::fabs(pf.z - az) < 7.5f &&
                pf.y > cy - 0.5f && pf.y < cy + 4.0f;
            faplayer.setJumpScale(inFizz ? 1.8f : 1.0f);
        }

        float camX, camY, camZ, camYaw, camPitch;
        if (!noclip) {
            x3::game::PlayerInput in;
            if (kd(GLFW_KEY_W)) in.moveFwd    += 1.0f;
            if (kd(GLFW_KEY_S)) in.moveFwd    -= 1.0f;
            if (kd(GLFW_KEY_D)) in.moveStrafe += 1.0f;
            if (kd(GLFW_KEY_A)) in.moveStrafe -= 1.0f;
            in.sprint      = kd(GLFW_KEY_LEFT_SHIFT);
            in.jumpPressed = spaceNow && !prevSpace;
            in.lookDX = ddx; in.lookDY = ddy;
            faplayer.update(in, fdt, *faphys);
            faplayer.camera(camX, camY, camZ, camYaw, camPitch);
        } else {
            const float sens = 0.0025f;
            flyYaw += ddx * sens; flyPitch -= ddy * sens;
            if (flyPitch >  1.55f) flyPitch =  1.55f;
            if (flyPitch < -1.55f) flyPitch = -1.55f;
            float fx = std::cos(flyPitch) * std::cos(flyYaw);
            float fy = std::sin(flyPitch);
            float fz = std::cos(flyPitch) * std::sin(flyYaw);
            float rl = std::sqrt(fx*fx + fz*fz); if (rl < 1e-4f) rl = 1e-4f;
            float rx = -fz/rl, rz = fx/rl;
            float spd = 7.0f * fdt; if (kd(GLFW_KEY_LEFT_SHIFT)) spd *= 3.0f;
            if (kd(GLFW_KEY_W)) { flyX += fx*spd; flyY += fy*spd; flyZ += fz*spd; }
            if (kd(GLFW_KEY_S)) { flyX -= fx*spd; flyY -= fy*spd; flyZ -= fz*spd; }
            if (kd(GLFW_KEY_D)) { flyX += rx*spd; flyZ += rz*spd; }
            if (kd(GLFW_KEY_A)) { flyX -= rx*spd; flyZ -= rz*spd; }
            if (spaceNow) flyY += spd;
            if (kd(GLFW_KEY_LEFT_CONTROL)) flyY -= spd;
            camX = flyX; camY = flyY; camZ = flyZ; camYaw = flyYaw; camPitch = flyPitch;
        }
        prevSpace = spaceNow;

        // ---- E: call the cab (summon-to-caller manners, the app_run idiom).
        {
            const bool eNow = kd(GLFW_KEY_E);
            if (eNow && !prevE) {
                const x3::phys::Vec3 pf = faphys->getBodyPosition(faplayer.body());
                const int myStop = elev.nearestStopTo(pf.y);
                const bool cabHere = std::fabs(elev.stopY(myStop) - elev.cabCenter().y) < 0.6f;
                if (!elev.playerRiding(pf) && !cabHere) elev.callTo(myStop);
                else                                     elev.callNext();
            }
            prevE = eNow;
        }
        // ---- Cab keypad: digits 0-9 (top row + numpad). 4790 / 9999 / 1127.
        for (int d = 0; d <= 9; ++d) {
            const bool dNow = kd(GLFW_KEY_0 + d) || kd(GLFW_KEY_KP_0 + d);
            if (dNow && !prevDigit[d]) elev.keypadDigit(d);
            prevDigit[d] = dNow;
        }

        // ---- Advance the world: annex animation, cab, RIDER CARRY (Vec3 —
        // the lateral bore leg carries sideways; carryDelta is the whole
        // point of Phase 1's T2), triggers, FX.
        annex.tick(fdt, fascene);
        elev.update(fdt, fascene, *faphys);
        {
            x3::phys::Vec3 pf = faphys->getBodyPosition(faplayer.body());
            const x3::phys::Vec3& d = elev.carryDelta();
            if ((d.x != 0.0f || d.y != 0.0f || d.z != 0.0f) && elev.playerRiding(pf)) {
                pf.x += d.x; pf.y += d.y; pf.z += d.z;
                faphys->setBodyPosition(faplayer.body(), pf);
            }
            elev.autoOpenFor(faphys->getBodyPosition(faplayer.body()));
            elev.applyCabAtmosphere(*device, faphys->getBodyPosition(faplayer.body()));
        }
        fafx.update(fdt);
        faphys->step(fdt);
        fascene.update(*faphys);
        for (uint32_t id : fatrig.update(faphys->getBodyPosition(faplayer.body())))
            annex.onTrigger(id);

        pushLights();
        pushWater(fdt);

        // ---- Task 12 audio: listener + event edges (heapless per frame).
        if (faaudio) {
            worldT += fdt;
            faaudio->setListener(camX, camY, camZ, camYaw, camPitch);
            if (!prevUnlocked && elev.hiddenUnlocked()) {   // 4790: golden fanfare
                if (sndChime.valid()) faaudio->playSound2D(sndChime, 0.85f, 1.0f);
                prevUnlocked = true;
            }
            const uint32_t se = annex.room(3).eventCount;
            if (se != prevSortEv) {          // the hatch lets go: the buzz of judgment
                const float* p = annex.room(3).eventPos;
                if (sndBuzz.valid()) faaudio->playSound3D(sndBuzz, p[0], p[1], p[2], 0.9f, 0.85f);
                prevSortEv = se;
            }
            const uint32_t te = annex.room(4).eventCount;
            if (te != prevTubeEv) {          // capsule docks: thunk + whoosh tail
                const float* p = annex.room(4).eventPos;
                if (sndThunk.valid())  faaudio->playSound3D(sndThunk, p[0], p[1], p[2], 0.8f, 1.05f);
                if (sndWhoosh.valid()) faaudio->playSound3D(sndWhoosh, p[0], p[1], p[2], 0.35f, 1.3f);
                prevTubeEv = te;
            }
            if (worldT >= nextGlorpT) {      // vat glorp, ~0.12 Hz, wandering vats
                const float vats[3][2] = { { 13.0f, 0.0f }, { 0.0f, -13.0f }, { 2.0f, 13.0f } };
                if (sndGlorp.valid())
                    faaudio->playSound3D(sndGlorp, aX + vats[glorpVat][0],
                                         x3::game::FactoryAnnex::kFloorBaseY[0] + 4.0f,
                                         aZ + vats[glorpVat][1], 0.35f,
                                         0.55f + 0.15f * (float)glorpVat);
                glorpVat = (glorpVat + 1) % 3;
                nextGlorpT = worldT + 7.0f + 2.5f * std::sin(worldT * 1.7f);
            }
            // The apex wind dies with the Burst (Freefall home = quiet fall).
            if (windLoop.valid() && elev.state() != x3::game::ElevState::Burst) {
                faaudio->stopLoop(windLoop);
                windLoop = x3::audio::LoopHandle{};
            }
        }

        // HUD line (dependency-free): the window title.
        {
            std::string title = "CONFECTION ANNEX  |  " +
                elev.floorLabel(elev.currentStop());
            if (!elev.hiddenUnlocked())      title += "  |  the cab keypad knows more than the panel does";
            else if (annex.burstDaisVisited() && !elev.burstFired())
                                             title += "  |  the roof is not the limit - 9999";
            else if (elev.burstFired())      title += "  |  the sky remembers the roof";
            if (title != lastTitle) { glfwSetWindowTitle(window, title.c_str()); lastTitle = title; }
        }

        int cw, ch; glfwGetFramebufferSize(window, &cw, &ch);
        if (cw != lastW || ch != lastH) { lastW = cw; lastH = ch; if (cw > 0 && ch > 0) device->onResize((uint32_t)cw, (uint32_t)ch); }

        device->setCamera(camX, camY, camZ, camYaw, camPitch, 65.0f);
        auto frame = device->beginFrame();
        if (frame.valid) {
            device->gpuDebrisStep(fdt);
            fascene.render(*device, frame);
            const float shardTint[4] = { 0.75f, 0.88f, 0.95f, 1.0f };
            device->gpuDebrisDraw(frame, shardTint);
            fafx.draw(*device, frame, camX, camY, camZ, camYaw, camPitch);
            fafx.submit(*device, frame);
        }
        device->endFrame(frame);
    }

    if (faaudio) faaudio->shutdown();
    fafx.shutdown(*device);
    annex.shutdown(*device);
    faphys->shutdown();
    device->shutdown();
    if (window) glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

}} // namespace x3::apphost
