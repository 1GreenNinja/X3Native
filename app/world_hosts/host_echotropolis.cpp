// --world echotropolis host — F1 P1 app skeleton (Echotropolis: the island in 3D).
//
// The strategic ORBIT camera over an open sea. This is a pure CONSUMER of the
// engine: analytic sky (setSkyParams via TimeOfDay), the Gerstner ocean
// (setWaterParams — a device-internal 240m patch that re-centers under the camera
// each frame, so it reads as an infinite sea), and the FPS camera primitive
// (setCamera). P2: the authored island lands as a baked GLB (EnvArtSystem, see
// the island block in hostEchotropolis). P3: a live TimeOfDay cycle drives sky/
// sun/ambient/water through the art bible's four canonical times (golden/dusk/
// night/noon — keys 1-4, T pauses, ECHO_TOD pins headless captures). Modeled on host_valley.cpp for the
// scene/teardown lifecycle, but there is no physics/streamer here — water and sky
// are device-internal passes, so the loop just poses the camera and renders.
//
// ORBIT CAMERA (SPACECRAFT_INTEGRATION §2 doctrine):
//   - orbit a focus point on the sea plane (y=0), camera pitched DOWN toward it;
//   - critically-damped springs on focus XZ, yaw, pitch AND radius (no snapping);
//   - pitch (elevation above the sea) clamped 2°..32°, default 17°;
//   - wheel zoom with momentum (impulse -> zoom velocity -> radius, eased);
//   - hold-RMB free-look: detaches from the orbit (snappy look around the focus)
//     and eases BACK to the orbit angle on release;
//   - MMB-drag or WASD pans the focus across the sea with inertia (glide on release).
//   - Every feel constant lives in one CameraOptions block (the F1 settings scaffold).
#include "world_host_common.h"
#include "../tod.h"
#include "../env_art.h"

namespace x3 { namespace apphost {

namespace {

// Unity-style critically-damped spring (Game Programming Gems 4). Eases `cur`
// toward `target`, carrying `vel` between frames — no overshoot, no snap. The
// single `smoothTime` (seconds to ~reach target) IS the inertia knob per channel.
inline float smoothDamp(float cur, float target, float& vel, float smoothTime, float dt) {
    if (smoothTime < 1e-4f) smoothTime = 1e-4f;
    const float omega = 2.0f / smoothTime;
    const float x = omega * dt;
    const float expf_ = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
    const float change = cur - target;
    const float temp = (vel + omega * change) * dt;
    vel = (vel - omega * temp) * expf_;
    return target + (change + temp) * expf_;
}

// Shortest-arc damp for an angle (keeps yaw wrapping seamless across +/-pi).
inline float smoothDampAngle(float cur, float target, float& vel, float smoothTime, float dt) {
    float d = target - cur;
    while (d >  3.14159265f) { d -= 6.28318531f; }
    while (d < -3.14159265f) { d += 6.28318531f; }
    return smoothDamp(cur, cur + d, vel, smoothTime, dt);
}

// TU-local scroll accumulator (only one --world host ever runs at a time). GLFW
// scroll is event-driven, so we sum deltas here and drain them once per frame.
double g_scrollY = 0.0;
void scrollCB(GLFWwindow*, double /*xoff*/, double yoff) { g_scrollY += yoff; }

// The F1 camera-settings scaffold: one tunable block every controller reads
// (D1/D3 doctrine). F2 hangs a settings UI + remap table on these fields.
struct CameraOptions {
    float fovDeg        = 60.0f;    // vertical FOV
    float lookSens      = 0.0045f;  // rad per pixel (free-look / orbit rotate)
    float panSpeed      = 0.9f;     // world m/s of focus travel per unit input
    float wasdPan       = 55.0f;    // WASD focus pan speed (m/s at unit radius scale)
    float zoomFrac      = 0.55f;    // radius impulse per wheel notch, as a fraction
                                    // of CURRENT radius (island vistas span 40m..6.5km
                                    // — a fixed step is unusable across that range)
    // Critically-damped smoothTimes (s) — the inertia per channel.
    float smoothFocus   = 0.22f;
    float smoothYaw     = 0.18f;
    float smoothPitch   = 0.18f;
    float smoothRadius  = 0.28f;
    // Decays for the momentum accumulators (per-frame multipliers scaled by dt).
    float zoomDecay     = 4.5f;     // higher = zoom glide stops sooner
    float panDecay      = 3.2f;     // higher = pan glide stops sooner
    // Radius (orbit distance) limits.
    float minRadius     = 40.0f;
    float maxRadius     = 6500.0f;
};

// Orbit rig state (targets + smoothed values + spring velocities).
struct OrbitRig {
    // Targets (driven by input).
    float focusX = 0.0f, focusZ = 0.0f;   // focus point on the sea (y=0)
    float yaw    = 3.93f;                  // orbit azimuth (rad) — from the SE: town shelf
                                           // in front, mesa cliffs behind (tiers readable)
    float pitch  = 0.2443f;                // elevation above sea (rad) — 14deg default
    float radius = 2800.0f;                // orbit distance — frames the whole 4km island
    // Smoothed (what the camera actually uses).
    float sFocusX = 0.0f, sFocusZ = 0.0f;
    float sYaw = 3.93f, sPitch = 0.2443f, sRadius = 2800.0f;
    // Spring velocities.
    float vFocusX = 0.0f, vFocusZ = 0.0f, vYaw = 0.0f, vPitch = 0.0f, vRadius = 0.0f;
    // Momentum accumulators (for glide/inertia after the input stops).
    float panVelX = 0.0f, panVelZ = 0.0f, zoomVel = 0.0f;
    // Free-look (hold-RMB) detach state.
    bool  freeLook = false;
    float freeYaw = 3.93f, freePitch = 0.2443f;
};

constexpr float kPitchMin = 0.0349f;   //  2 degrees
constexpr float kPitchMax = 0.5585f;   // 32 degrees

inline float clampPitch(float p) { return p < kPitchMin ? kPitchMin : (p > kPitchMax ? kPitchMax : p); }

// Pose the engine FPS camera FROM the orbit rig (looking down at the focus).
// `useYaw/usePitch` let the free-look path drive the actual look angles directly
// while the underlying orbit targets stay frozen (so it can ease back on release).
void applyOrbitCamera(x3::rhi::IRenderDevice* device, const OrbitRig& r,
                      float useYaw, float usePitch, float fovDeg) {
    const float lookPitch = -usePitch;             // camera pitches DOWN toward the sea
    const float cp = std::cos(lookPitch), sp = std::sin(lookPitch);
    const float fwdX = cp * std::cos(useYaw);
    const float fwdY = sp;
    const float fwdZ = cp * std::sin(useYaw);
    const float camX = r.sFocusX - fwdX * r.sRadius;
    const float camY =        0.0f - fwdY * r.sRadius;   // focus.y = 0 -> camY = sin(pitch)*radius > 0
    const float camZ = r.sFocusZ - fwdZ * r.sRadius;
    device->setCamera(camX, camY, camZ, useYaw, lookPitch, fovDeg);
}

// ---- P3: the four canonical times (ART_DIRECTION.md lighting doctrine) mapped
// onto TimeOfDay's normalized clock. Dusk spans [0.55,0.75): golden hour sits
// early in it (low warm sun), blue dusk late (sun at the horizon). Night midpoint
// gets the aurora swell + starfield; noon is the clear-day baseline.
constexpr float kTodGolden = 0.715f;  // "The Postcard" — sun elevation ~0.15, warm + low
constexpr float kTodDusk   = 0.75f;   // "Lamps Coming On" — sun ON the horizon, ember line
constexpr float kTodNight  = 0.875f;  // "The Answering Light" — moon fill + stars
constexpr float kTodNoon   = 0.40f;   // "The Clear Day" — bright PNW noon
float canonTodFraction(const char* name) {
    std::string n(name);
    if (n == "golden") return kTodGolden;
    if (n == "dusk")   return kTodDusk;
    if (n == "night")  return kTodNight;
    if (n == "noon")   return kTodNoon;
    const float f = std::strtof(name, nullptr);   // also accepts a raw fraction
    return (f >= 0.0f && f < 1.0f) ? f : kTodGolden;
}

// Push a TOD sample to the device: analytic sky (sun dir/color/haze/exposure —
// this sun ALSO lights the island's PBR meshes) + the ambient fill, with the
// midnight aurora tint folded into the ambient so the night isn't pure black.
//
// ART_DIRECTION.md doctrine layer: TimeOfDay's keyframes animate the sun but
// leave SkyParams' zenith/horizon at their bright DAY defaults, so night skies
// render pale. Grade the dome here by sun elevation — deep blue-black at night,
// and an amber horizon blush when the sun is low but up (golden hour / dawn).
void applyTodSample(x3::rhi::IRenderDevice* device, const x3::game::TodSample& s) {
    x3::rhi::IRenderDevice::SkyParams sky = s.sky;
    sky.enabled = true;

    auto clamp01 = [](float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); };
    // dayness: 1 in full day, ramping to 0 as the sun sinks past the horizon.
    const float dayness = clamp01((s.sunElevation + 0.06f) / 0.30f);
    constexpr float kNightZenith[3]  = { 0.004f, 0.007f, 0.018f };  // near-black blue
    constexpr float kNightHorizon[3] = { 0.012f, 0.018f, 0.040f };  // faint sea-glow
    for (int i = 0; i < 3; ++i) {
        sky.zenith[i]  = kNightZenith[i]  + (sky.zenith[i]  - kNightZenith[i])  * dayness;
        sky.horizon[i] = kNightHorizon[i] + (sky.horizon[i] - kNightHorizon[i]) * dayness;
    }
    // Golden-hour blush: sun above the horizon but LOW -> horizon warms to amber,
    // zenith picks up a touch of it. Peaks at elevation 0, gone by ~0.35.
    if (s.sunElevation > 0.0f) {
        const float low = clamp01(1.0f - s.sunElevation / 0.35f);
        constexpr float kAmber[3] = { 0.98f, 0.52f, 0.24f };
        for (int i = 0; i < 3; ++i) {
            sky.horizon[i] += (kAmber[i] - sky.horizon[i]) * (0.65f * low);
            sky.zenith[i]  += (kAmber[i] - sky.zenith[i])  * (0.12f * low);
        }
    }
    device->setSkyParams(sky);
    device->setAmbient(s.ambient[0] + s.auroraTint[0],
                       s.ambient[1] + s.auroraTint[1],
                       s.ambient[2] + s.auroraTint[2]);
}

// Gerstner ocean at sea level 0, lit by the SAME sun as the sky (doctrine: one
// light). Water color follows the daylight: full color at noon, ember-dark at
// night (the baked GLB ocean ring darkens automatically via the PBR sun).
void applyOcean(x3::rhi::IRenderDevice* device, float t, const x3::game::TodSample& s) {
    x3::rhi::IRenderDevice::WaterParams wp{};
    wp.enabled = true; wp.seaLevel = 0.0f; wp.time = t;
    // Amplitude must stay UNDER the island GLB's baked ocean ring (y=-0.4) or the
    // Gerstner troughs punch through it and checker with the ring (seen in P2).
    wp.amplitude = 0.32f; wp.steepness = 0.5f; wp.waveLength = 14.0f; wp.speed = 1.0f;
    const float dayF = 0.12f + 0.88f * std::max(0.0f, s.sunElevation);
    wp.deepColor[0]    = 0.015f * dayF; wp.deepColor[1]    = 0.055f * dayF; wp.deepColor[2]    = 0.11f * dayF;
    wp.shallowColor[0] = 0.06f  * dayF; wp.shallowColor[1] = 0.24f  * dayF; wp.shallowColor[2] = 0.32f * dayF;
    wp.sunDir[0] = s.sky.sunDir[0]; wp.sunDir[1] = s.sky.sunDir[1]; wp.sunDir[2] = s.sky.sunDir[2];
    wp.specular = 16.0f; wp.fresnel = 0.02f;
    device->setWaterParams(wp);
}

} // namespace

int hostEchotropolis(HostContext& hc) {
    auto* device = hc.device;
    GLFWwindow* window = hc.window;
    const bool headless = hc.headless;
    const bool screenshot = hc.screenshot;
    const std::string& screenshotPath = hc.screenshotPath;
    const bool shotCamOverride = hc.shotCamOverride;
    const float* shotCam = hc.shotCam;

    x3::logInfo("--world echotropolis: F1 P1 skeleton — orbit camera over the open sea");

    CameraOptions opt;
    OrbitRig rig;

    // ---- P3: time-of-day. A full day-night cycle runs in 240s (windowed); the
    // clock starts at (and headless captures) ECHO_TOD = golden|dusk|night|noon
    // or a raw [0,1) fraction. Default: golden hour — the postcard light.
    x3::game::TodConfig todCfg;
    todCfg.dayLengthSeconds = 240.0f;
    x3::game::TimeOfDay tod(todCfg);
    {
        const char* e = std::getenv("ECHO_TOD");
        tod.setDayFraction(canonTodFraction(e ? e : "golden"));
    }
    applyTodSample(device, tod.sample());
    device->setCameraFar(20000.0f);   // far plane covers the GLB's 14km ocean ring corners

    // ---- P2: THE ISLAND. Authored in SimCityLLM2 (gen_heightmap.py seed 20260530),
    // meshed + splat-baked by tools/island_to_glb.py: land mesh (513^2 grid + skirt)
    // with a single 4096^2 blended albedo, PLUS a flat ocean ring to the horizon at
    // y=-0.4 (the engine Gerstner patch only lives near the camera; the ring keeps
    // sea-to-horizon at vista distance). Sea level = world y 0; mesa tops ~179m.
    // Dir overridable for other checkouts: ECHO_ISLAND_DIR env var.
    x3::game::EnvArtSystem island;
    {
        const char* dirEnv = std::getenv("ECHO_ISLAND_DIR");
        const std::string islandDir = dirEnv ? dirEnv : "D:/GameDev/SimCityLLM2/refs/terrain";
        if (island.buildFromGlb(*device, islandDir, "island_20260530.glb"))
            x3::logInfo("--world echotropolis: island GLB loaded from " + islandDir);
        else
            x3::logError("--world echotropolis: island GLB MISSING (" + islandDir +
                         "/island_20260530.glb) — rendering open sea only");
    }

    // ===================== Headless screenshot path =====================
    // Pose the default orbit (17deg, radius 70), settle the waves a few frames so
    // the Gerstner surface isn't flat, then arm+grab. Mirrors host_valley's grab.
    if (headless || screenshot) {
        // Snap the smoothed state onto the targets (no live input to ease from).
        rig.sFocusX = rig.focusX; rig.sFocusZ = rig.focusZ;
        rig.sYaw = rig.yaw; rig.sPitch = rig.pitch; rig.sRadius = rig.radius;

        const std::string outPath = screenshot ? screenshotPath
                                               : std::string("agent_echotropolis.png");
        const int kSettle = 24;
        const float dt = 1.0f / 60.0f;
        const x3::game::TodSample shotTod = tod.sample();   // frozen at ECHO_TOD
        for (int i = 0; i < kSettle; ++i) {
            glfwPollEvents();
            applyTodSample(device, shotTod);
            applyOcean(device, (float)i * dt, shotTod);
            if (shotCamOverride) {
                device->setCamera(shotCam[0], shotCam[1], shotCam[2], shotCam[3], shotCam[4], opt.fovDeg);
            } else {
                applyOrbitCamera(device, rig, rig.sYaw, rig.sPitch, opt.fovDeg);
            }
            if (i == kSettle - 1) device->armCapture(outPath.c_str());
            auto frame = device->beginFrame();
            island.draw(*device, frame);   // the island (sky + water are device-internal)
            device->endFrame(frame);
        }
        const bool wrote = device->captureFrame(outPath.c_str());
        if (wrote) x3::logInfo("--world echotropolis: wrote screenshot " + outPath);
        else       x3::logError("--world echotropolis: capture FAILED");

        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return wrote ? 0 : 1;
    }

    // ===================== Windowed interactive orbit ===================
    glfwSetScrollCallback(window, scrollCB);
    double lastMX, lastMY; glfwGetCursorPos(window, &lastMX, &lastMY);
    double prevTime = glfwGetTime();
    float  waterTime = 0.0f;

    // FPS accounting (log once per second — there is no HUD text API in this host).
    double fpsAccum = 0.0; int fpsFrames = 0;
    // Optional auto-exit for CI / FPS capture: `ECHO_AUTOEXIT_SEC=<n>` env var.
    double autoExitSec = 0.0;
    if (const char* e = std::getenv("ECHO_AUTOEXIT_SEC")) autoExitSec = std::atof(e);
    double runElapsed = 0.0; double runFrames = 0.0; double runSecs = 0.0;

    x3::logInfo("--world echotropolis: LMB/drag or WASD pan, wheel zoom, hold-RMB "
                "free-look; TOD keys 1=golden 2=dusk 3=night 4=noon, T pauses the "
                "cycle; Esc to quit");
    bool todPaused = false, prevT = false;
    x3::game::TodPhase prevPhase = tod.phase();

    int lastW = (int)hc.W, lastH = (int)hc.H;
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;

        double now = glfwGetTime();
        float dt = (float)(now - prevTime); prevTime = now;
        if (dt > 0.1f) dt = 0.1f;
        if (dt < 1e-5f) dt = 1e-5f;

        double mx, my; glfwGetCursorPos(window, &mx, &my);
        float ddx = (float)(mx - lastMX), ddy = (float)(my - lastMY);
        lastMX = mx; lastMY = my;

        auto kd  = [&](int k) { return glfwGetKey(window, k) == GLFW_PRESS; };
        auto mbd = [&](int b) { return glfwGetMouseButton(window, b) == GLFW_PRESS; };

        const bool rmb = mbd(GLFW_MOUSE_BUTTON_RIGHT);
        const bool mmb = mbd(GLFW_MOUSE_BUTTON_LEFT) || mbd(GLFW_MOUSE_BUTTON_MIDDLE);

        // ---- Wheel zoom with momentum: impulse -> zoomVel -> radius target ----
        if (g_scrollY != 0.0) {
            rig.zoomVel -= (float)g_scrollY * opt.zoomFrac * rig.radius;   // scroll up = zoom in
            g_scrollY = 0.0;
        }
        rig.radius += rig.zoomVel * dt;
        rig.zoomVel -= rig.zoomVel * std::min(1.0f, opt.zoomDecay * dt);   // decay glide
        if (rig.radius < opt.minRadius) { rig.radius = opt.minRadius; rig.zoomVel = 0.0f; }
        if (rig.radius > opt.maxRadius) { rig.radius = opt.maxRadius; rig.zoomVel = 0.0f; }

        // ---- Free-look (hold-RMB): detach, drive look angles directly ----------
        if (rmb && !rig.freeLook) {                // press: snapshot current view
            rig.freeLook = true;
            rig.freeYaw = rig.sYaw; rig.freePitch = rig.sPitch;
        } else if (!rmb && rig.freeLook) {         // release: orbit eases back
            rig.freeLook = false;
        }
        if (rig.freeLook) {
            rig.freeYaw   += ddx * opt.lookSens;
            rig.freePitch  = clampPitch(rig.freePitch - ddy * opt.lookSens);
        }

        // ---- Pan the focus across the sea (MMB/LMB drag or WASD) + inertia -----
        float panInX = 0.0f, panInZ = 0.0f;   // requested focus velocity (world XZ)
        // Screen-relative basis from the orbit yaw: forward on the sea + right.
        const float cy = std::cos(rig.sYaw), sy = std::sin(rig.sYaw);
        const float fwdSeaX = cy, fwdSeaZ = sy;      // where the camera looks, flattened
        const float rightX  = -sy, rightZ = cy;
        if (mmb && !rig.freeLook) {
            // Drag: mouse motion pushes the sea under the cursor (inverted feel).
            const float k = opt.panSpeed * rig.sRadius * 0.02f;
            panInX += (-ddx * rightX + ddy * fwdSeaX) * k / std::max(dt, 1e-4f);
            panInZ += (-ddx * rightZ + ddy * fwdSeaZ) * k / std::max(dt, 1e-4f);
        }
        {
            float wf = 0.0f, ws = 0.0f;
            if (kd(GLFW_KEY_W)) wf += 1.0f;
            if (kd(GLFW_KEY_S)) wf -= 1.0f;
            if (kd(GLFW_KEY_D)) ws += 1.0f;
            if (kd(GLFW_KEY_A)) ws -= 1.0f;
            if (wf != 0.0f || ws != 0.0f) {
                const float k = opt.wasdPan * (rig.sRadius / 70.0f);
                panInX += (wf * fwdSeaX + ws * rightX) * k;
                panInZ += (wf * fwdSeaZ + ws * rightZ) * k;
            }
        }
        const bool panning = (panInX != 0.0f || panInZ != 0.0f);
        if (panning) { rig.panVelX = panInX; rig.panVelZ = panInZ; }
        else {         // glide, then drag to a stop (inertia on release)
            rig.panVelX -= rig.panVelX * std::min(1.0f, opt.panDecay * dt);
            rig.panVelZ -= rig.panVelZ * std::min(1.0f, opt.panDecay * dt);
        }
        rig.focusX += rig.panVelX * dt;
        rig.focusZ += rig.panVelZ * dt;

        // Keep the orbit-angle targets sane (elevation clamp is doctrine).
        rig.pitch = clampPitch(rig.pitch);

        // ---- Critically-damped smoothing of every channel (no snapping) -------
        rig.sFocusX = smoothDamp(rig.sFocusX, rig.focusX, rig.vFocusX, opt.smoothFocus, dt);
        rig.sFocusZ = smoothDamp(rig.sFocusZ, rig.focusZ, rig.vFocusZ, opt.smoothFocus, dt);
        rig.sYaw    = smoothDampAngle(rig.sYaw, rig.yaw, rig.vYaw, opt.smoothYaw, dt);
        rig.sPitch  = smoothDamp(rig.sPitch, rig.pitch, rig.vPitch, opt.smoothPitch, dt);
        rig.sRadius = smoothDamp(rig.sRadius, rig.radius, rig.vRadius, opt.smoothRadius, dt);

        // Framebuffer resize passthrough (match host_valley).
        int cw, chh; glfwGetFramebufferSize(window, &cw, &chh);
        if (cw != lastW || chh != lastH) {
            lastW = cw; lastH = chh;
            if (cw > 0 && chh > 0) device->onResize((uint32_t)cw, (uint32_t)chh);
        }

        // ---- P3: advance the day (pause with T; 1-4 jump to the canon times) ----
        {
            const bool tNow = kd(GLFW_KEY_T);
            if (tNow && !prevT) todPaused = !todPaused;
            prevT = tNow;
            if (kd(GLFW_KEY_1)) tod.setDayFraction(kTodGolden);
            if (kd(GLFW_KEY_2)) tod.setDayFraction(kTodDusk);
            if (kd(GLFW_KEY_3)) tod.setDayFraction(kTodNight);
            if (kd(GLFW_KEY_4)) tod.setDayFraction(kTodNoon);
            if (!todPaused) tod.advance(dt);
            if (tod.phase() != prevPhase) {
                prevPhase = tod.phase();
                x3::logInfo(std::string("--world echotropolis: ") +
                            x3::game::todPhaseName(prevPhase));
            }
        }
        const x3::game::TodSample todS = tod.sample();
        applyTodSample(device, todS);

        // Ocean + camera + render.
        waterTime += dt; applyOcean(device, waterTime, todS);
        if (rig.freeLook) applyOrbitCamera(device, rig, rig.freeYaw, rig.freePitch, opt.fovDeg);
        else              applyOrbitCamera(device, rig, rig.sYaw, rig.sPitch, opt.fovDeg);

        auto frame = device->beginFrame();
        island.draw(*device, frame);
        device->endFrame(frame);

        // FPS: log once per second.
        fpsAccum += dt; ++fpsFrames;
        if (fpsAccum >= 1.0) {
            const double fps = fpsFrames / fpsAccum;
            char buf[96];
            std::snprintf(buf, sizeof(buf), "--world echotropolis: FPS %.1f (%.2f ms/frame)",
                          fps, 1000.0 * fpsAccum / fpsFrames);
            x3::logInfo(buf);
            runSecs += fpsAccum; runFrames += fpsFrames;
            fpsAccum = 0.0; fpsFrames = 0;
        }

        if (autoExitSec > 0.0) { runElapsed += dt; if (runElapsed >= autoExitSec) break; }
    }

    if (runSecs > 0.0) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "--world echotropolis: avg FPS %.1f over %.1fs",
                      runFrames / runSecs, runSecs);
        x3::logInfo(buf);
    }

    device->shutdown();
    if (window) glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

}} // namespace x3::apphost
