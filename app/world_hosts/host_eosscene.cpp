// --world eos-scene host — the Empires of Shadow grey-box world (native-client
// feasibility spike; charter: epochs-rts/docs/design/NATIVE-CLIENT-SPIKE.md).
//
// Loads the "eos-scene-1" snapshot (manifest.json + scene.bin — see
// app/eos_scene.{h,cpp} and NATIVE-SCENE-FORMAT.md), stands the scene up as
// grey-box geometry, and flies the manifest's CANONICAL 30 s orbit (121
// keyframes, linear, looping) via the roll-capable setCameraBasis. Both
// renderers (this and the browser bench) MUST fly exactly this path — that is
// the benchmark contract.
//
//   Scene dir : EOS_SCENE_DIR env var, else assets/eos-scene (relative cwd).
//   Windowed  : flies the orbit forever; per-lap avg/min/max FPS + frame-time
//               logged each lap AND appended to eos_bench.txt; on-screen HUD
//               line; Esc quits.
//   --bench   : vsync OFF (main() wires desc.vsync = !bench); 1 warmup lap +
//               3 measured laps, then prints THE NUMBER and exits.
//   headless  : (--screenshot) three captures across the orbit at t = 2 / 10 /
//               20 s -> <base>_t2/_t10/_t20.png (base from --screenshot <path>,
//               default eos_scene.png). Mirrors the valley/cliffs grab flow.
//
// Modeled on host_cliffs/host_echotropolis for the lifecycle: no physics, no
// streamer — the loop poses the camera and renders; sky + Gerstner water are
// device-internal passes.
#include "world_host_common.h"
#include "../eos_scene.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace x3 { namespace apphost {

namespace {

// Device dressing shared by the headless + windowed paths: analytic day sky
// (default warm sun), an open-air ambient, the Gerstner water plane at the
// manifest's waterLevel, and shadow bounds framing the whole map.
void applyEosDressing(x3::rhi::IRenderDevice* device, const x3::game::EosSceneWorld& world) {
    x3::rhi::IRenderDevice::SkyParams sky;
    sky.enabled = true;                       // defaults: sun (0.4,1,0.3), clear day
    device->setSkyParams(sky);
    device->setAmbient(0.22f, 0.24f, 0.28f);
    // Far plane: the 200-tile map's far corner from the orbit is ~300 wu; the
    // default 200 m would clip it.
    device->setCameraFar(1200.0f);
    const float cx = (float)world.mapW() * 0.5f;
    const float cz = -(float)world.mapH() * 0.5f;   // engine space (Z negated)
    device->setShadowBounds(cx, 0.0f, cz, 0.8f * (float)std::max(world.mapW(), world.mapH()));
}

// Per-frame water tick (the device does not keep its own clock).
void applyEosWater(x3::rhi::IRenderDevice* device, const x3::game::EosSceneWorld& world,
                   float t) {
    x3::rhi::IRenderDevice::WaterParams wp{};
    wp.enabled = true;
    wp.seaLevel = world.waterLevel();
    wp.time = t;
    // 1 wu = 1 TILE here (not 1 m): default ocean swell would bury the beds.
    // A few cm of chop reads as an RTS river/lake.
    wp.amplitude = 0.05f;
    wp.steepness = 0.35f;
    wp.waveLength = 5.0f;
    wp.speed = 0.8f;
    wp.deepColor[0] = 0.015f; wp.deepColor[1] = 0.05f;  wp.deepColor[2] = 0.10f;
    wp.shallowColor[0] = 0.06f; wp.shallowColor[1] = 0.22f; wp.shallowColor[2] = 0.28f;
    wp.specular = 4.0f;
    device->setWaterParams(wp);
}

// The benchmark FOV. The manifest does not carry one (flagged in the report);
// 45 deg vertical ~= the browser renderer's default Babylon FOV (0.8 rad).
constexpr float kFovDeg = 45.0f;

} // namespace

int hostEosScene(HostContext& hc) {
    auto* device = hc.device;
    GLFWwindow* window = hc.window;
    const bool headless = hc.headless;

    x3::logInfo("--world eos-scene: EoS grey-box world (native-client spike)");

    // ---- load + build ----
    const char* dirEnv = std::getenv("EOS_SCENE_DIR");
    const std::string sceneDir = dirEnv ? dirEnv : "assets/eos-scene";
    x3::game::EosSceneWorld world;
    if (!world.load(sceneDir)) {
        x3::logError("--world eos-scene: scene load FAILED from '" + sceneDir +
                     "' (set EOS_SCENE_DIR or export with epochs-rts "
                     "tools/export-native-scene.ts)");
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }
    device->beginUploadBatch();
    world.build(*device);
    device->endUploadBatch();
    applyEosDressing(device, world);

    const float dur = world.cameraDuration();   // 30 s canonical orbit

    auto poseCamera = [&](float t) {
        float pos[3], fwd[3];
        world.cameraAt(t, pos, fwd);
        const float up[3] = { 0.0f, 1.0f, 0.0f };
        device->setCameraBasis(pos[0], pos[1], pos[2], fwd, up, kFovDeg);
    };

    // ===================== Headless: 3 captures across the orbit ============
    if (headless) {
        const std::string base = hc.screenshot && !hc.screenshotPath.empty()
                                     ? hc.screenshotPath : std::string("eos_scene.png");
        const std::string stem = (base.size() > 4 && base.rfind(".png") == base.size() - 4)
                                     ? base.substr(0, base.size() - 4) : base;
        const float shotT[3] = { 2.0f, 10.0f, 20.0f };
        bool allWrote = true;
        const float dt = 1.0f / 60.0f;
        for (int s = 0; s < 3; ++s) {
            char pathBuf[512];
            std::snprintf(pathBuf, sizeof(pathBuf), "%s_t%d.png", stem.c_str(), (int)shotT[s]);
            const int kSettle = 40;   // TAA/HZB/auto-exposure settle
            for (int i = 0; i < kSettle; ++i) {
                glfwPollEvents();
                applyEosWater(device, world, shotT[s] + (float)i * dt);
                poseCamera(shotT[s]);
                if (i == kSettle - 1) device->armCapture(pathBuf);
                auto frame = device->beginFrame();
                if (frame.valid) world.draw(*device, frame);
                device->endFrame(frame);
            }
            const bool wrote = device->captureFrame(pathBuf);
            if (wrote) {
                const x3::rhi::RenderStats st = device->stats();
                char rb[256];
                std::snprintf(rb, sizeof(rb),
                              "--world eos-scene: wrote %s | t=%.0fs draws=%u tris=%u",
                              pathBuf, shotT[s], st.drawCalls, st.triangles);
                x3::logInfo(rb);
            } else {
                x3::logError(std::string("--world eos-scene: capture FAILED: ") + pathBuf);
                allWrote = false;
            }
        }
        world.shutdown(*device);
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return allWrote ? 0 : 1;
    }

    // ===================== Windowed: fly the canonical orbit ================
    // --bench: 1 warmup lap + kBenchLaps measured laps, then report + exit.
    const bool bench = hc.bench;
    int benchLaps = 3;
    if (const char* e = std::getenv("EOS_LAPS")) benchLaps = std::max(1, std::atoi(e));

    x3::logInfo(std::string("--world eos-scene: flying the canonical ") +
                std::to_string((int)dur) + " s orbit (" +
                (bench ? "BENCH: 1 warmup + " + std::to_string(benchLaps) + " laps"
                       : "free-running; Esc quits") + ")");

    double prevTime = glfwGetTime();
    double elapsed = 0.0;                    // orbit clock (wall time)
    int    lap = 0;                          // current lap index (0 = warmup)
    int    lapFrames = 0;
    double lapTime = 0.0, lapMinDt = 1e9, lapMaxDt = 0.0;
    // Measured-lap aggregate (laps >= 1) for the bench summary.
    long   aggFrames = 0; double aggTime = 0.0, aggMinDt = 1e9, aggMaxDt = 0.0;
    char   hud[160]; hud[0] = '\0';
    std::ofstream benchFile("eos_bench.txt", std::ios::app);

    int lastW = (int)hc.W, lastH = (int)hc.H;
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;

        const double now = glfwGetTime();
        double dt = now - prevTime; prevTime = now;
        if (dt > 0.25) dt = 0.25;            // debugger/stall clamp
        elapsed += dt;

        // ---- lap accounting ----
        ++lapFrames; lapTime += dt;
        if (dt < lapMinDt) lapMinDt = dt;
        if (dt > lapMaxDt) lapMaxDt = dt;
        const int lapNow = (int)(elapsed / dur);
        if (lapNow != lap) {
            const double avgFps = lapFrames / std::max(1e-6, lapTime);
            const double avgMs = 1000.0 * lapTime / std::max(1, lapFrames);
            char line[256];
            std::snprintf(line, sizeof(line),
                          "[eos-bench] lap %d%s: %d frames in %.2f s | avg %.1f FPS "
                          "(%.3f ms) | frame min %.3f ms / max %.3f ms",
                          lap, lap == 0 ? " (warmup)" : "", lapFrames, lapTime,
                          avgFps, avgMs, 1000.0 * lapMinDt, 1000.0 * lapMaxDt);
            x3::logInfo(line);
            if (benchFile) benchFile << line << "\n" << std::flush;
            if (lap >= 1) {
                aggFrames += lapFrames; aggTime += lapTime;
                if (lapMinDt < aggMinDt) aggMinDt = lapMinDt;
                if (lapMaxDt > aggMaxDt) aggMaxDt = lapMaxDt;
            }
            lap = lapNow;
            lapFrames = 0; lapTime = 0.0; lapMinDt = 1e9; lapMaxDt = 0.0;
            if (bench && lap > benchLaps) break;
        }

        // ---- resize ----
        int cw, ch; glfwGetFramebufferSize(window, &cw, &ch);
        if (cw != lastW || ch != lastH) {
            lastW = cw; lastH = ch;
            if (cw > 0 && ch > 0) device->onResize((uint32_t)cw, (uint32_t)ch);
        }

        // ---- pose + render ----
        applyEosWater(device, world, (float)elapsed);
        poseCamera((float)elapsed);
        auto frame = device->beginFrame();
        if (frame.valid) {
            world.draw(*device, frame);
            // On-screen metrics line (updated ~4x/s to stay readable).
            if ((lapFrames & 15) == 1) {
                const x3::rhi::RenderStats st = device->stats();
                std::snprintf(hud, sizeof(hud),
                              "EoS %s seed %d | lap %d t=%4.1fs | %6.1f FPS %5.2f ms | draws %u tris %.2fM",
                              world.mapScript().c_str(), world.seed(), lap,
                              std::fmod(elapsed, (double)dur),
                              dt > 0.0 ? 1.0 / dt : 0.0, dt * 1000.0,
                              st.drawCalls, st.triangles * 1e-6);
            }
            const float rgba[4] = { 1.0f, 0.95f, 0.75f, 0.95f };
            device->drawHudText(frame, hud, 12.0f, 12.0f, 9.0f, rgba);
        }
        device->endFrame(frame);
    }

    if (aggFrames > 0) {
        const double avgFps = aggFrames / std::max(1e-6, aggTime);
        char line[256];
        std::snprintf(line, sizeof(line),
                      "[eos-bench] TOTAL (%d measured lap%s): %ld frames in %.2f s | "
                      "avg %.1f FPS (%.3f ms) | frame min %.3f ms / max %.3f ms | "
                      "%ux%u vsync %s",
                      std::max(0, lap - 1), (lap - 1) == 1 ? "" : "s", aggFrames, aggTime,
                      avgFps, 1000.0 * aggTime / std::max(1L, aggFrames),
                      1000.0 * aggMinDt, 1000.0 * aggMaxDt,
                      (uint32_t)lastW, (uint32_t)lastH, bench ? "OFF" : "on");
        x3::logInfo(line);
        if (benchFile) benchFile << line << "\n" << std::flush;
    }

    world.shutdown(*device);
    device->shutdown();
    if (window) glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

}} // namespace x3::apphost
