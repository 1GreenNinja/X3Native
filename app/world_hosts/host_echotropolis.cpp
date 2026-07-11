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
// ORBIT CAMERA (RTS-grade feel — retuned after live playtest):
//   - orbit a focus point on the sea plane (y=0), camera pitched DOWN toward it;
//   - LMB/MMB-drag PANS with a "grab the ground" 1:1 gain that scales with orbit
//     radius (groundPerPixel) so the terrain tracks the cursor at any zoom;
//   - RMB-drag ORBITS (yaw/pitch) directly at ~0.25°/px — no ease-back detach;
//   - wheel zoom is EXPONENTIAL (~12% radius/notch), no momentum;
//   - light critically-damped smoothing (~0.10-0.14s) so nothing snaps or floats;
//   - CLAMPS: radius 100m..6000m, focus to island bounds +1km, camera height never
//     below waterline+5m (also stops flying outside the world / seeing the sea slab);
//   - pitch (elevation above the sea) clamped 2°..32°, default 14°;
//   - Every feel constant lives in one CameraOptions block (the F1 settings scaffold).
#include "world_host_common.h"
#include "../tod.h"
#include "../env_art.h"

#include <stb_image.h>   // stbi_load_16_from_memory (impl compiled in engine ModelLoader.cpp)
#include <fstream>
#include <vector>

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

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// "Grab the ground" pan gain: world metres of GROUND spanned per screen pixel at
// the focus, from the perspective vertical coverage 2*radius*tan(fovV/2)/screenH.
// A drag scales its pixel delta by this so the terrain tracks the cursor ~1:1 (RTS
// map-drag) at any zoom, instead of a fixed rate unusable across the 100m..6km range.
inline float groundPerPixel(float radius, float fovDeg, int screenH) {
    const float fovV = fovDeg * 3.14159265f / 180.0f;
    return (2.0f * radius * std::tan(fovV * 0.5f)) / (float)(screenH > 0 ? screenH : 1);
}

// Host-side sample of the authored island's 16-bit heightmap (the SAME PNG
// island_to_glb.py meshed the GLB from). Lets the orbit PIVOT ride the terrain
// height under the focus so rotating/zooming keeps the ground under the crosshair
// pinned (RTS feel) instead of pivoting around the flat y=0 plane while the mesa
// sweeps past. Optional: if the PNG is absent heightAt() returns 0 and the pivot
// falls back to the water plane. The world mapping mirrors island_to_glb.py exactly
// (extent 4096 m, HEIGHT_SCALE 320, sea level = normalized 0.20 -> world y 0).
struct Heightfield {
    int w = 0, h = 0;
    std::vector<uint16_t> px;                     // 16-bit grayscale, row-major
    static constexpr float kMeters  = 4096.0f;    // world extent (island frame)
    static constexpr float kScale   = 320.0f;     // HEIGHT_SCALE
    static constexpr float kSeaNorm = 0.20f;      // normalized sea level

    bool load(const std::string& path) {
        // The engine's stb build is STBI_NO_STDIO (memory loaders only), so slurp
        // the file ourselves and decode from memory.
        std::ifstream f(path, std::ios::binary);
        if (!f) { w = h = 0; return false; }
        std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(f)),
                                          std::istreambuf_iterator<char>());
        if (bytes.empty()) { w = h = 0; return false; }
        int comp = 0;
        uint16_t* d = stbi_load_16_from_memory(bytes.data(), (int)bytes.size(), &w, &h, &comp, 1);
        if (!d || w < 2 || h < 2) { if (d) stbi_image_free(d); w = h = 0; return false; }
        px.assign(d, d + (size_t)w * h);
        stbi_image_free(d);
        return true;
    }
    bool ok() const { return w >= 2 && h >= 2; }

    // Bilinear world height (metres) at world (x,z). Off-grid clamps to the edge.
    float heightAt(float x, float z) const {
        if (!ok()) return 0.0f;
        float u = clampf((x / kMeters + 0.5f) * (float)(w - 1), 0.0f, (float)(w - 1));
        float v = clampf((z / kMeters + 0.5f) * (float)(h - 1), 0.0f, (float)(h - 1));
        const int x0 = (int)u, z0 = (int)v;
        const int x1 = x0 < w - 1 ? x0 + 1 : x0;
        const int z1 = z0 < h - 1 ? z0 + 1 : z0;
        const float fx = u - (float)x0, fz = v - (float)z0;
        auto S = [&](int cx, int cz) { return (float)px[(size_t)cz * w + cx] / 65535.0f; };
        const float a = S(x0, z0), b = S(x1, z0), c = S(x0, z1), d2 = S(x1, z1);
        const float hn = a + (b - a) * fx + (c - a) * fz + (a - b - c + d2) * fx * fz;
        return (hn - kSeaNorm) * kScale;
    }
};

// TU-local scroll accumulator (only one --world host ever runs at a time). GLFW
// scroll is event-driven, so we sum deltas here and drain them once per frame.
double g_scrollY = 0.0;
void scrollCB(GLFWwindow*, double /*xoff*/, double yoff) { g_scrollY += yoff; }

// The F1 camera-settings scaffold: one tunable block every controller reads
// (D1/D3 doctrine). F2 hangs a settings UI + remap table on these fields.
struct CameraOptions {
    float fovDeg        = 60.0f;    // vertical FOV
    // RMB orbit-rotate: direct degrees-per-pixel (converted to rad in the loop).
    float orbitDegPerPx = 0.25f;    // ~0.25°/px — RTS look feel, no heavy easing
    // Q/E keyboard orbit-yaw rate (rad/s) — ~90°/s, Q counter-clockwise, E clockwise.
    float keyYawRate    = 1.5708f;  // pi/2 rad/s
    // WASD pan speed as a FRACTION of the orbit radius per second (so keyboard pan
    // covers the frame at a consistent visual rate whether zoomed in or out): 0.5 ->
    // cross the 4km island in ~3s at 3km out, street-scale (~100 m/s) at 200m.
    float wasdPanFrac   = 0.50f;
    // Wheel zoom: EXPONENTIAL — a constant fraction of the radius per notch.
    float zoomPerNotch  = 0.12f;    // 12% radius per wheel notch (multiplicative)
    // Critically-damped smoothTimes (s) — snappy RTS response (no float/overshoot).
    float smoothFocus   = 0.12f;
    float smoothYaw     = 0.10f;
    float smoothPitch   = 0.10f;
    float smoothRadius  = 0.14f;
    // Radius (orbit distance) limits — island stays >= ~1/3 of frame at maxRadius.
    float minRadius     = 100.0f;
    float maxRadius     = 6000.0f;
    // Focus clamp: island half-extent (~2048 m) + 1 km margin, so panning can't
    // drift off into empty sea and lose the island.
    float focusLimit    = 3048.0f;
    // Camera never below waterline + this many metres (also blocks flying under the
    // world and seeing the sea-slab underside / fog dome).
    float minCamHeight  = 5.0f;
};

// Orbit rig state (targets + smoothed values + spring velocities).
struct OrbitRig {
    // Targets (driven by input).
    float focusX = 0.0f, focusZ = 0.0f;   // focus point on the sea (y=0)
    float yaw    = 3.93f;                  // orbit azimuth (rad) — from the SE: town shelf
                                           // in front, mesa cliffs behind (tiers readable)
    float pitch  = 0.2443f;                // elevation above sea (rad) — 14deg default
    float radius = 2800.0f;                // orbit distance — frames the whole 4km island
    // Orbit-pivot height: the terrain height under (focusX,focusZ), so the pivot
    // rides the land (0 over water). Keeps the crosshair point fixed while rotating.
    float pivotY = 0.0f;
    // Smoothed (what the camera actually uses).
    float sFocusX = 0.0f, sFocusZ = 0.0f, sPivotY = 0.0f;
    float sYaw = 3.93f, sPitch = 0.2443f, sRadius = 2800.0f;
    // Spring velocities.
    float vFocusX = 0.0f, vFocusZ = 0.0f, vPivotY = 0.0f, vYaw = 0.0f, vPitch = 0.0f, vRadius = 0.0f;
};

constexpr float kPitchMin = 0.0349f;   //  2 degrees
constexpr float kPitchMax = 0.5585f;   // 32 degrees

inline float clampPitch(float p) { return p < kPitchMin ? kPitchMin : (p > kPitchMax ? kPitchMax : p); }

// Pose the engine FPS camera FROM the orbit rig (looking down at the focus). The
// pitch is raised if needed so the camera height (sin(pitch)*radius, since focus.y
// = 0) never drops below the waterline + minCamHeight — this both keeps the horizon
// composed and blocks flying under the world at low pitch / small radius.
void applyOrbitCamera(x3::rhi::IRenderDevice* device, const OrbitRig& r,
                      float useYaw, float usePitch, float fovDeg, float minCamHeight) {
    const float minPitch = std::asin(clampf(minCamHeight / std::max(r.sRadius, 1e-3f), 0.0f, 1.0f));
    if (usePitch < minPitch) usePitch = minPitch;
    const float lookPitch = -usePitch;             // camera pitches DOWN toward the sea
    const float cp = std::cos(lookPitch), sp = std::sin(lookPitch);
    const float fwdX = cp * std::cos(useYaw);
    const float fwdY = sp;
    const float fwdZ = cp * std::sin(useYaw);
    const float camX = r.sFocusX  - fwdX * r.sRadius;
    const float camY = r.sPivotY  - fwdY * r.sRadius;   // pivot on terrain -> camY = pivotY + sin(pitch)*radius
    const float camZ = r.sFocusZ  - fwdZ * r.sRadius;
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
    constexpr float kNightZenith[3]  = { 0.0f, 0.0f, 0.0f };        // DARK BLACK (Tim's canon)
    constexpr float kNightHorizon[3] = { 0.0f, 0.0f, 0.0f };        // stars own the night
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
    Heightfield hf;   // orbit-pivot terrain sampler (windowed only; headless keeps y=0)
    {
        const char* dirEnv = std::getenv("ECHO_ISLAND_DIR");
        const std::string islandDir = dirEnv ? dirEnv : "D:/GameDev/SimCityLLM2/refs/terrain";
        if (island.buildFromGlb(*device, islandDir, "island_20260530.glb"))
            x3::logInfo("--world echotropolis: island GLB loaded from " + islandDir);
        else
            x3::logError("--world echotropolis: island GLB MISSING (" + islandDir +
                         "/island_20260530.glb) — rendering open sea only");
        if (hf.load(islandDir + "/island_height_20260530.png"))
            x3::logInfo("--world echotropolis: heightfield loaded — orbit pivot rides the terrain");
        else
            x3::logWarn("--world echotropolis: heightfield PNG absent — orbit pivot uses the y=0 plane");
    }

    // ---- P4 slice (sibling lane): the NIGHT LIGHTS. Two systems, both night-gated:
    //   * lighthouse_beam.glb — sweeping beam cone (apex at origin, +X), re-posed per
    //     frame via setInstanceTransform, mounted on the PROPS lighthouse's lantern
    //     (tower itself lives in echotropolis_props.glb — integrator's; placement from
    //     refs/models/props_placement.json, lantern center = base + 25.75m);
    //   * fissure_glow.glb — GrokCity3 ember quads lining the fjord walls, world-baked.
    // Missing GLBs degrade gracefully to "not drawn".
    constexpr float kLightX = -493.24f, kLightY = -0.156f, kLightZ = 789.39f;
    constexpr float kLanternY = 25.75f;     // beam pivot above the props tower base
    constexpr float kBeamRate = 0.35f;      // rad/s sweep
    x3::game::EnvArtSystem beam, fissure;
    {
        const char* mEnv = std::getenv("ECHO_MODELS_DIR");
        const std::string mDir = mEnv ? mEnv : "D:/GameDev/SimCityLLM2/refs/models";
        const float T[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, kLightX,kLightY,kLightZ,1 };
        if (beam.buildFromGlbAt(*device, mDir, "lighthouse_beam.glb", T))
            x3::logInfo("--world echotropolis: lighthouse beam armed (night-gated)");
        if (fissure.buildFromGlb(*device, mDir, "fissure_glow.glb"))
            x3::logInfo("--world echotropolis: fissure ember-glow loaded (night-gated)");
    }
    auto poseBeam = [&](float theta) {
        const float c = std::cos(theta), s = std::sin(theta);
        const float M[16] = { c,0,-s,0, 0,1,0,0, s,0,c,0, kLightX, kLightY + kLanternY, kLightZ, 1 };
        beam.setInstanceTransform(0, M);
    };

    // ================= P4 COAST DRESSING (props) — added by the P4 session =========
    // Lighthouse + dock + fishing boats + mesa skyline hint, all baked at their
    // WORLD positions into one GLB by tools/place_props.py (positions DERIVED from
    // the heightmap: bay-arm tip / flat shelf / calm basin / mesa-top-near-fjord —
    // re-derivable as the landform changes; NOT hand-tuned to a fixed coastline).
    // Loaded at identity exactly like the island GLB (world-space verts). Purely
    // visual, no physics. Dir overridable via ECHO_PROPS_DIR. Missing file = no-op
    // (the island still renders). Iterating placement/scale = rebake only, no rebuild.
    x3::game::EnvArtSystem props;
    {
        const char* pdirEnv = std::getenv("ECHO_PROPS_DIR");
        const std::string propsDir = pdirEnv ? pdirEnv : "D:/GameDev/SimCityLLM2/refs/models";
        if (props.buildFromGlb(*device, propsDir, "echotropolis_props.glb"))
            x3::logInfo("--world echotropolis: P4 props GLB loaded from " + propsDir);
        else
            x3::logWarn("--world echotropolis: P4 props GLB absent (" + propsDir +
                        "/echotropolis_props.glb) — coast undressed");
    }
    // ================= END P4 COAST DRESSING ======================================

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
                applyOrbitCamera(device, rig, rig.sYaw, rig.sPitch, opt.fovDeg, opt.minCamHeight);
            }
            if (i == kSettle - 1) device->armCapture(outPath.c_str());
            auto frame = device->beginFrame();
            island.draw(*device, frame);   // the island (sky + water are device-internal)
            props.draw(*device, frame);    // P4 coast dressing (lighthouse/dock/boats/skyline)
            if (shotTod.cityLightsOn) {    // P4 night lights: beam aimed over the bay + embers
                poseBeam(-2.13f);
                beam.draw(*device, frame);
                fissure.draw(*device, frame);
            }
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

    x3::logInfo("--world echotropolis: LMB/MMB-drag or WASD pan (grab-the-ground), "
                "wheel zoom, RMB-drag or Q/E orbit-rotate; TOD keys 1=golden 2=dusk "
                "3=night 4=noon, T pauses the cycle; Esc to quit");
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

        // ---- Wheel zoom: EXPONENTIAL (a constant fraction of radius per notch),
        // no momentum. scroll up (yoff>0) zooms in -> radius shrinks. ----
        if (g_scrollY != 0.0) {
            rig.radius *= std::pow(1.0f - opt.zoomPerNotch, (float)g_scrollY);
            g_scrollY = 0.0;
        }
        rig.radius = clampf(rig.radius, opt.minRadius, opt.maxRadius);

        // Screen-relative ground basis from the orbit yaw: forward (into screen) +
        // right, both flattened onto the sea plane.
        const float cy = std::cos(rig.sYaw), sy = std::sin(rig.sYaw);
        const float fwdSeaX = cy, fwdSeaZ = sy;
        const float rightX  = -sy, rightZ = cy;

        // ---- RMB-drag ORBIT-ROTATE: direct ~0.25°/px, no ease-back detach. ----
        const float radPerPx = opt.orbitDegPerPx * (3.14159265f / 180.0f);
        if (rmb) {
            rig.yaw   += ddx * radPerPx;
            rig.pitch  = clampPitch(rig.pitch - ddy * radPerPx);
        }

        // ---- Q/E keyboard orbit-yaw (RTS convention): Q counter-clockwise, E
        // clockwise, constant ~90°/s while held; the light yaw damping stops it. ----
        if (kd(GLFW_KEY_Q)) rig.yaw -= opt.keyYawRate * dt;
        if (kd(GLFW_KEY_E)) rig.yaw += opt.keyYawRate * dt;

        // ---- LMB/MMB-drag PAN: "grab the ground" — the terrain tracks the cursor
        // ~1:1. Horizontal is exact (groundPerPixel); the forward axis is stretched
        // by the view foreshortening (÷sin(pitch), bounded) so shallow-angle vertical
        // drags don't crawl. Direct target move, no inertia. (Suppressed while RMB
        // is held so orbit-rotate and pan never fight.) ----
        if (mmb && !rmb) {
            const float gpp = groundPerPixel(rig.sRadius, opt.fovDeg, lastH);
            const float fwdGain = gpp / clampf(std::sin(rig.sPitch), 0.35f, 1.0f);
            rig.focusX -= ddx * rightX * gpp + ddy * fwdSeaX * fwdGain;
            rig.focusZ -= ddx * rightZ * gpp + ddy * fwdSeaZ * fwdGain;
        }

        // ---- WASD pan: radius-scaled so it covers the frame at a steady rate. ----
        {
            float wf = 0.0f, ws = 0.0f;
            if (kd(GLFW_KEY_W)) wf += 1.0f;
            if (kd(GLFW_KEY_S)) wf -= 1.0f;
            if (kd(GLFW_KEY_D)) ws += 1.0f;
            if (kd(GLFW_KEY_A)) ws -= 1.0f;
            if (wf != 0.0f || ws != 0.0f) {
                const float step = opt.wasdPanFrac * rig.sRadius * dt;
                rig.focusX += (wf * fwdSeaX + ws * rightX) * step;
                rig.focusZ += (wf * fwdSeaZ + ws * rightZ) * step;
            }
        }

        // Clamp targets to the playable envelope (island bounds + 1km; pitch 2..32°).
        rig.focusX = clampf(rig.focusX, -opt.focusLimit, opt.focusLimit);
        rig.focusZ = clampf(rig.focusZ, -opt.focusLimit, opt.focusLimit);
        rig.pitch  = clampPitch(rig.pitch);
        // Pivot rides the terrain height under the focus (0 over water) so the point
        // under the crosshair stays fixed while rotating/zooming.
        rig.pivotY = std::max(hf.heightAt(rig.focusX, rig.focusZ), 0.0f);

        // ---- Critically-damped smoothing of every channel (no snapping) -------
        rig.sFocusX = smoothDamp(rig.sFocusX, rig.focusX, rig.vFocusX, opt.smoothFocus, dt);
        rig.sFocusZ = smoothDamp(rig.sFocusZ, rig.focusZ, rig.vFocusZ, opt.smoothFocus, dt);
        rig.sPivotY = smoothDamp(rig.sPivotY, rig.pivotY, rig.vPivotY, opt.smoothFocus, dt);
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
        applyOrbitCamera(device, rig, rig.sYaw, rig.sPitch, opt.fovDeg, opt.minCamHeight);

        auto frame = device->beginFrame();
        island.draw(*device, frame);
        props.draw(*device, frame);    // P4 coast dressing (lighthouse/dock/boats/skyline)
        if (todS.cityLightsOn) {       // P4 night lights: sweeping beam + fissure embers
            poseBeam(waterTime * kBeamRate);
            beam.draw(*device, frame);
            fissure.draw(*device, frame);
        }
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
