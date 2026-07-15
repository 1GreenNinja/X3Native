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
//   - pitch (elevation above the sea) clamped 2°..85° (near-top-down RTS view
//     available), default UNCHANGED at 14° (0.2443 rad — the postcard vista);
//   - momentum edge-scroll: cursor in a 12px window-edge band pans the focus that
//     way, velocity-based (tau=0.11s accel/glide), radius-scaled, off while dragging;
//   - Every feel constant lives in one CameraOptions block (the F1 settings scaffold).
#include "world_host_common.h"
#include "../tod.h"
#include "../env_art.h"
#include "../mine_fx.h"               // GOLD MINE: authentic EoS arch mouth-glow (ported render FX)
#include "../player.h"                // WALK MODE (Phase A): first-person character on the streets
#include "../scene.h"                 // RESIDENTS: crowd entities live in a Scene
#include "../crowd.h"                 // RESIDENTS: wandering citizen agents
#include "../monster.h"              // OH1 HERO HELI: rigged skinned draw via MonsterSystem prop
#include "../crowd_skin.h"            // RESIDENTS: real rigged-GLB characters over the agents
#include "../npc_life.h"              // LIVING CITY: 12-archetype NPCs with daily schedules
#include "../asset_root.h"            // riggedGlbRoot()
#include "../holo_terminal.h"         // CONTROL ROOM: in-world ops dashboard screen
#include <string>

#include <stb_image.h>   // stbi_load_16_from_memory (impl compiled in engine ModelLoader.cpp)
#include <fstream>
#include <vector>
#include <memory>
#include <string>

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
    // Zoom smoothing time-constant. EoS uses a dt*10 exponential approach (~0.10s);
    // we keep our critically-damped smoothDamp model (nicer curve, no overshoot) but
    // ALIGN its time-constant to EoS: 0.14s -> 0.10s (adopt-now cross-check, item 3).
    float smoothRadius  = 0.10f;
    // Radius (orbit distance) limits — island stays >= ~1/3 of frame at maxRadius.
    float minRadius     = 100.0f;
    float maxRadius     = 6000.0f;
    // Focus clamp: island half-extent (~2048 m) + 1 km margin, so panning can't
    // drift off into empty sea and lose the island.
    float focusLimit    = 3048.0f;
    // Camera never below waterline + this many metres (also blocks flying under the
    // world and seeing the sea-slab underside / fog dome).
    float minCamHeight  = 5.0f;
    // ---- Momentum edge-scroll (EoS "adopt now") --------------------------------
    // Cursor inside this many px of a window edge pushes the focus that way.
    float edgeMarginPx  = 12.0f;
    // Edge-scroll speed as a FRACTION of the orbit radius per second (radius-scaled
    // like WASD pan, so it feels identical at any zoom — matches wasdPanFrac feel;
    // EoS's 15 wu/s * dist/24 works out to ~0.62 frac, we use the host's 0.5 for
    // consistency with the existing WASD pan).
    float edgeScrollFrac = 0.50f;
    // Velocity accel/decay time-constant for edge-scroll momentum (EoS PAN_SMOOTH_TAU;
    // our focus smoothDamp uses 0.12s critically-damped, comparable — left as-is).
    float panSmoothTau  = 0.11f;
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
    // Edge-scroll momentum velocity (world m/s on the sea plane), eased with panSmoothTau.
    float vEdgeX = 0.0f, vEdgeZ = 0.0f;
};

constexpr float kPitchMin = 0.0349f;   //   2 degrees (see the sky just above the horizon)
constexpr float kPitchMax = 1.4835f;   //  85 degrees (near-top-down RTS city view)

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

// ---- F3 HERO MODE (dive) — HOOK/SPEC ONLY, not implemented (EoS "later" tier). ----
// Port of epochs-rts hero-mode.ts: F3 dives from the orbit pose into an over-the-
// shoulder 3rd-person chase on a walked unit; ESC parks the orbit back on the unit.
// When implemented, add a `bool heroActive` gate around the orbit input/apply below
// and branch to a HeroRig here. Study constants to lift verbatim:
//   ENTER_BLEND_S 0.55 (smoothstep eye+aim RTS->shoulder); FOLLOW_DIST 3.4 (boom),
//   SHOULDER 0.55 (right lateral), ANCHOR_HEIGHT 1.15, AIM_AHEAD 1.6, EYE_CLEARANCE 0.34;
//   LOOK_SENS 0.0028 rad/px, pitch clamp -1.1..1.25 rad, enter pitch -0.08;
//   3-point boom terrain collision (t=0.5/0.8/1.0), eye snaps up instantly / eases
//   down at dt*6; drive movement via sim orders (ORDER_PERIOD 0.22s, LEAD 2.6) to
//   preserve determinism; waterline sub-cam auto-triggers on depth.
// NO IMPLEMENTATION HERE — comment hook only.

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
    // Night ambient FLOOR: the pure-black sky kills the IBL contribution, so at
    // night the terrain is lit by ambient alone — hold a moonlight minimum so the
    // island stays readable while the sky dome itself stays black.
    constexpr float kNightAmbFloor[3] = { 0.11f, 0.12f, 0.17f };
    float amb[3];
    for (int i = 0; i < 3; ++i) {
        amb[i] = s.ambient[i] + s.auroraTint[i];
        const float floorI = kNightAmbFloor[i] * (1.0f - dayness);
        if (amb[i] < floorI) amb[i] = floorI;
    }
    device->setAmbient(amb[0], amb[1], amb[2]);
}

// Gerstner ocean at sea level 0, lit by the SAME sun as the sky (doctrine: one
// light). Water color follows the daylight: full color at noon, ember-dark at
// night (the baked GLB ocean ring darkens automatically via the PBR sun).
void applyOcean(x3::rhi::IRenderDevice* device, float t, const x3::game::TodSample& s) {
    x3::rhi::IRenderDevice::WaterParams wp{};
    const float daynessGate = (s.sunElevation + 0.06f) / 0.30f;
    // DEEP NIGHT: the Gerstner shader's reflection term renders bright white no
    // matter what params we pass (engine-side; flagged to the fleet) — so the
    // patch is DISABLED at night and the island GLB's dark ocean ring owns the
    // water. No patch = no white sheet, no trough checkering, calm night sea.
    if (daynessGate < 0.15f) {
        x3::rhi::IRenderDevice::WaterParams off{};
        off.enabled = false;
        device->setWaterParams(off);
        return;
    }
    wp.enabled = true; wp.seaLevel = 0.0f; wp.time = t;
    // Amplitude must stay UNDER the island GLB's baked ocean ring (y=-0.4) or the
    // Gerstner troughs punch through it and checker with the ring (seen in P2).
    // NOTE the shader's octave sum overshoots the amplitude param — 0.32 still
    // punched through at night (bright patch vs black ring = checkerboard). 0.26
    // leaves real margin.
    wp.amplitude = 0.26f; wp.steepness = 0.5f; wp.waveLength = 14.0f; wp.speed = 1.0f;
    const float dayness = std::min(1.0f, std::max(0.0f, (s.sunElevation + 0.06f) / 0.30f));
    const float dayF = 0.12f + 0.88f * std::max(0.0f, s.sunElevation);
    wp.deepColor[0]    = 0.015f * dayF; wp.deepColor[1]    = 0.055f * dayF; wp.deepColor[2]    = 0.11f * dayF;
    wp.shallowColor[0] = 0.06f  * dayF; wp.shallowColor[1] = 0.24f  * dayF; wp.shallowColor[2] = 0.32f * dayF;
    // A below-horizon sunDir blows the Gerstner glint out to full white (seen at
    // night) — swap in a high dim "moon" and wind the specular down with the day.
    wp.sunDir[0] = s.sky.sunDir[0];
    wp.sunDir[1] = std::max(0.45f, s.sky.sunDir[1]);
    wp.sunDir[2] = s.sky.sunDir[2];
    if (s.sunElevation > 0.10f) wp.sunDir[1] = s.sky.sunDir[1];   // day: the real sun
    wp.specular = 1.0f + 15.0f * dayness;
    wp.fresnel = 0.02f;
    device->setWaterParams(wp);
}

// ============================ ATMOSPHERE (cinematic pass) =====================
// Owner: cinematic-atmosphere session (2026-07-11). Layers Steam-key-art depth on
// TOP of the TOD sky/sun/water (applyTodSample / applyOcean), using ONLY engine
// levers documented in IRenderDevice.h: setFog (aerial perspective), setGrade
// (filmic + vivid split-tone + vignette), setBloom / setExposure / setPostFX
// (ACES tonemap + sun-glint bloom), setShadowBounds (frame the 4km island).
// Deliberately SEPARATE from applyTodSample so the sky/water/TOD session's edits
// never collide — call this AFTER applyTodSample each frame. Engine has NO
// world-cloud layer / skybox-texture / god-ray pass, so those are intentionally
// absent (clouds only exist on procedural planet spheres). Direction: vivid,
// saturated, storybook-postcard (NMS/Aincrad key-art), not muted photorealism.
void applyAtmosphere(x3::rhi::IRenderDevice* device, const x3::game::TodSample& s) {
    auto clamp01 = [](float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); };
    auto mix3 = [](float out[3], const float a[3], const float b[3], float t) {
        for (int i = 0; i < 3; ++i) out[i] = a[i] + (b[i] - a[i]) * t;
    };
    // dayness: 0 deep night .. 1 full day (matches applyTodSample's ramp).
    const float dayness = clamp01((s.sunElevation + 0.06f) / 0.30f);
    // low: golden-hour weight — sun up but near the horizon (peaks at elev 0).
    const float low = (s.sunElevation > 0.0f) ? clamp01(1.0f - s.sunElevation / 0.35f) : 0.0f;

    // ---- 1. AERIAL PERSPECTIVE -------------------------------------------------
    // Beer-Lambert depth fog whose COLOR matches the sky the island melts into:
    // cool blue-white by day, warm gold at golden hour, deep blue at night. With
    // the 2.8km orbit the far island (~4km) hazes to ~0.55 and the 7km ocean-ring
    // horizon to ~0.8, dissolving the hard water/sky line seen in the baseline.
    constexpr float kFogDay[3]    = { 0.58f, 0.72f, 0.90f };  // cool scattering blue-white
    constexpr float kFogGold[3]   = { 1.00f, 0.58f, 0.26f };  // RICH golden-hour haze (saturated)
    constexpr float kFogNight[3]  = { 0.015f, 0.03f, 0.075f };// deep blue dusk/night
    x3::rhi::IRenderDevice::FogParams fog;
    fog.enabled = true;
    mix3(fog.color, kFogNight, kFogDay, dayness);              // night -> day base
    // Warm HARD toward saturated gold at golden hour. High weight (not a 50/50
    // blend) so cool-blue + gold never average into gray — the haze reads amber.
    const float goldW = clamp01(low * 1.35f);
    mix3(fog.color, fog.color, kFogGold, goldW);
    // Tuned after Tim's sea-approach screenshot (2026-07-11): the whole 4km island
    // was drowning in amber soup at golden hour. The haze must live at the HORIZON
    // (10km+) while the island — a 3-6km subject — stays readable. Halve the
    // low-sun density ramp and pull the opacity ceiling down.
    fog.start      = 900.0f;                                   // crisp foreground island
    fog.density    = 0.00010f + 0.00004f * low;               // subtle by day, gentle low-sun
    fog.maxOpacity = 0.78f + 0.08f * low;                     // sky still glows gold out far
    device->setFog(fog);

    // ---- 2. FILMIC + VIVID GRADE (NMS/storybook: colors you can taste) ---------
    // Teal shadows / warm highlights split-tone, saturation pushed ABOVE 1 for the
    // painterly-confident look, warmth + vignette swelling at golden hour.
    x3::rhi::IRenderDevice::GradeParams gr;
    gr.strength = 0.90f;
    gr.shadowTint[0]    = 0.90f; gr.shadowTint[1]    = 1.00f; gr.shadowTint[2]    = 1.08f; // teal shadows
    gr.highlightTint[0] = 1.06f + 0.06f * low;                                             // warm highlights
    gr.highlightTint[1] = 1.00f;
    gr.highlightTint[2] = 0.92f - 0.06f * low;                                             // warmer low-sun
    gr.saturation = 1.14f + 0.10f * low;                       // vivid; extra pop at golden hour
    gr.vignette   = 0.08f + 0.04f * low;                      // gentle focus, more at golden hour
    device->setGrade(gr);

    // ---- 3/4. HDR POST + SUN-GLINT BLOOM --------------------------------------
    // ACES tonemap, a low bright-pass knee so the sun disc + water glint + lantern
    // bloom (not a washout), and a touch of positive exposure at golden hour to
    // make the low sun GLOW. Auto-exposure stays on (bias, not absolute).
    x3::rhi::IRenderDevice::PostFXParams px;   // defaults: ACES, autoexposure, TAA on
    px.bloomThreshold = 1.08f;                 // sun-glint blooms; not the whole water sheet
    px.bloomIntensity = 0.14f + 0.06f * low;   // warm halo at golden hour, no washout
    px.aeMax          = 2.20f + 0.30f * low;   // let the glowing sky adapt brighter
    device->setPostFX(px);
    device->setBloom(0.12f + 0.06f * low);     // composite bloom add (sun/lantern)
    device->setExposure(1.0f + 0.16f * low);   // golden hour glows

    // ---- 5. SHADOWS: frame the ~4km island so the sun casts across it ----------
    device->setShadowBounds(0.0f, 0.0f, 0.0f, 2200.0f);
}

// ============================ RAY TRACING (hardware RT pass) ==================
// Owner: RT-enable session (2026-07-11). Opts Echotropolis into the engine's
// EXISTING hardware ray-query features (IRenderDevice.h ~L505-650). SEPARATE from
// applyAtmosphere/applyTodSample/applyOcean so it never collides with the sky/
// water/TOD lane. The device CACHES + re-applies each param every frame, so a
// single call at host start persists across both the headless and windowed loops.
//
// ALL of this is gated on rayTracingSupported(): on a non-RT GPU (Pascal / the
// 1080Ti boxes / headless-no-RT) this is a pure no-op and the raster/CSM/SSAO
// path is byte-for-byte identical to before — exactly today's fallback.
//
// What we turn on (value order), and what we deliberately don't:
//   1. RT SOFT SUN SHADOWS (tier 1) — the postcard win: cliff faces, the 446
//      city buildings, and the lighthouse get soft distance-scaled penumbra under
//      the golden-hour sun (min()-combined with CSM; skinned chars keep raster).
//      This host has NO point lights (§3 below), so tier 1 (sun-only) is exactly
//      the useful work — tier 2 would loop for point occluders that don't exist.
//   2. RT AO — ground-truth contact occlusion multiplies the HDR scene so the
//      cottages/props read as SITTING ON the terrain (amphitheater contact), not
//      floating. Radius widened past the 0.5 m contact default because the city
//      is viewed from a ~2.8 km orbit — sub-metre AO would be sub-pixel there.
//   3. RT POINT-LIGHT SHADOWS — SKIPPED ON PURPOSE. The city windows, lighthouse
//      lantern, and fissure embers are PURELY emissive-material + bloom (this host
//      calls setPointLights zero times) — there are no real point lights to cast,
//      so tier-2 point shadows have nothing to shadow. If a future pass promotes
//      the lantern to a real light, bump tier to 2 and it casts for free.
//   4. DDGI — DEFERRED (not half-done). Its probe grid would have to span the ~4 km
//      island; a 24x8x24 auto-fit = ~170 m spacing, far too coarse for window-glow-
//      onto-streets bounce, and a useful grid needs an AUTHORED tight volume over
//      the night-city district (like screenshot_hosts' explicit 20x6x20). Deferred
//      pending that authored AABB rather than shipped coarse.
//   5. RT REFLECTIONS — NOT touched here (water is the sky/water lane's). The API
//      is setReflectionParams(ssr/rtFallback/fullRes/intensity); available for that
//      session to enable on the Gerstner ocean when they choose.
void applyRayTracing(x3::rhi::IRenderDevice* device) {
    if (!device->rayTracingSupported()) {
        x3::logInfo("--world echotropolis: RT unsupported on this device — raster/CSM/SSAO fallback (byte-identical)");
        return;
    }
    // Dev A/B opt-out: ECHO_RT=0 forces the raster/CSM/SSAO path even on RT hardware
    // (same-content baseline capture + Pascal-parity check). Default: RT ON.
    if (const char* e = std::getenv("ECHO_RT"); e && e[0] == '0') {
        x3::logInfo("--world echotropolis: ECHO_RT=0 — RT force-disabled (raster/CSM/SSAO baseline)");
        return;
    }
    // 1. RT soft SUN shadows (sun-only; no point lights in this host).
    x3::rhi::IRenderDevice::RtShadowParams rs{};
    rs.tier       = 1;      // sun RT (cone-jittered penumbra, min()-combined w/ CSM)
    rs.sunSizeDeg = 0.6f;   // golden-hour: a touch softer than the 0.5 deg default
    device->setRtShadowParams(rs);
    // 2. RT ambient occlusion (contact grounding; radius widened for orbit vista).
    x3::rhi::IRenderDevice::RtaoParams ao{};
    ao.enabled  = true;
    ao.radius   = 2.0f;     // broader than 0.5 m contact — visible from the orbit
    ao.rays     = 8;        // half-res + depth-aware upsample keeps this cheap
    ao.strength = 0.90f;
    ao.power    = 1.5f;
    device->setRtaoParams(ao);
    x3::logInfo("--world echotropolis: RT ENABLED — soft sun shadows (tier1, 0.6deg) + RT AO (r2.0, 8 rays); "
                "point shadows skipped (emissive city, no point lights), DDGI deferred (4km island), "
                "reflections left to water lane");
}
// ============================ END RAY TRACING ================================

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
    applyAtmosphere(device, tod.sample());   // ATMOSPHERE: aerial haze + grade + bloom
    applyRayTracing(device);                 // RAY TRACING: soft sun shadows + RT AO (gated; no-op on non-RT)
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

    // ===================== REAL BUILDINGS (Phase B) =====================
    // The first REAL textured GLB buildings in Echo Harbor. The Unity packs ship
    // Draco-compressed (KHR_draco_mesh_compression) — the engine loader now DECODES
    // Draco natively (fleet's feature/draco-decode, 2db0ebb, merged into main), so we
    // load the RAW packs directly. Unity cm → scale 0.01; each house lifted by its own
    // base offset so it sits on the terrain.
    std::vector<std::unique_ptr<x3::game::EnvArtSystem>> houses;
    {
        const std::string hdir = "D:/Assets/_glb/prefab_buildings/HouseForge";
        auto addHouse = [&](const char* glb, float x, float z, float yaw, float lift) {
            const float gy = hf.ok() ? hf.heightAt(x, z) : 190.0f;
            const float s = 0.01f, c = std::cos(yaw), sn = std::sin(yaw);
            const float T[16] = { c*s, 0.0f, -sn*s, 0.0f,  0.0f, s, 0.0f, 0.0f,
                                  sn*s, 0.0f,  c*s, 0.0f,   x, gy + lift, z, 1.0f };
            auto h = std::make_unique<x3::game::EnvArtSystem>();
            if (h->buildFromGlbAt(*device, hdir, glb, T)) houses.push_back(std::move(h));
        };
        // Five hero houses (hand-placed on the crown foothills).
        addHouse("PF_MetalHouse01.glb",      60.0f, 700.0f, 0.4f, 11.73f);
        addHouse("PF_MetalHouse02.glb",     125.0f, 745.0f, 2.1f,  2.13f);
        addHouse("PF_PrimitiveHouse01.glb",  10.0f, 675.0f, 3.6f,  3.17f);
        addHouse("PF_PrimitiveHouse02.glb", 150.0f, 690.0f, 5.0f,  1.00f);
        addHouse("PF_PrimitiveHouse03.glb",  85.0f, 640.0f, 1.2f,  8.59f);

        // NEIGHBOURHOOD DRAPE: multiply the proven, correctly-scaled HouseForge
        // GLBs into concentric rings around the crown so the island reads as a
        // lived-in town from the RTS vista. Each GLB carries its own base lift.
        // Deterministic hash jitter (no rand → identical every launch/capture).
        // Houses whose ground sample is at/near the shoreline are skipped so none
        // land in the water.
        struct HouseDef { const char* glb; float lift; };
        static const HouseDef kCat[5] = {
            {"PF_MetalHouse01.glb", 11.73f}, {"PF_MetalHouse02.glb", 2.13f},
            {"PF_PrimitiveHouse01.glb", 3.17f}, {"PF_PrimitiveHouse02.glb", 1.00f},
            {"PF_PrimitiveHouse03.glb", 8.59f},
        };
        auto h01 = [](uint32_t n) {
            n = (n ^ 61u) ^ (n >> 16); n *= 9u; n ^= n >> 4; n *= 0x27d4eb2du;
            n ^= n >> 15; return (float)(n & 0xffffffu) / (float)0x1000000;
        };
        const float ringR[4] = { 135.0f, 215.0f, 300.0f, 395.0f };
        int neigh = 0;
        for (int r = 0; r < 4; ++r) {
            const int cnt = 7 + r * 3;                     // fuller outer rings
            for (int k = 0; k < cnt; ++k) {
                const uint32_t seed = (uint32_t)(r * 101 + k);
                const float ang = ((float)k + h01(seed) * 0.7f) * (6.2831853f / cnt);
                const float rr  = ringR[r] + (h01(seed * 7u + 3u) - 0.5f) * 46.0f;
                const float x = -20.0f + std::cos(ang) * rr;
                const float z = 760.0f + std::sin(ang) * rr;
                const float gy = hf.ok() ? hf.heightAt(x, z) : 190.0f;
                if (gy < 34.0f) continue;                  // shoreline / water → skip
                const HouseDef& hd = kCat[(uint32_t)(r + k * 2) % 5u];
                const float yaw = ang + 1.5708f + (h01(seed * 13u + 5u) - 0.5f) * 1.2f;
                addHouse(hd.glb, x, z, yaw, hd.lift);
                ++neigh;
            }
        }
        x3::logInfo("--world echotropolis: REAL BUILDINGS — " +
                    std::to_string(houses.size()) + " textured HouseForge houses (" +
                    std::to_string(neigh) + " draped into neighbourhoods)");
    }

    // ===================== DOWNTOWN SKYLINE (Urban Night City) =============
    // The 37 "Urban Night City - Open World" building GLBs each bake their ORIGINAL
    // scene position in their node translation (base at world Y=0) — so drawing every
    // building through ONE shared transform reconstructs the artist-designed downtown
    // as a single unit. We scale that whole layout and drop it centred on the crown
    // so Echo Harbor gets a real textured skyline behind the procedural towers.
    // Scene footprint ~1941×1204 (centre 120,175); heights 44–792 m at the baked
    // 0.01 node scale. ECHO_TOWER_* env-tunable (scale/lift/centre) → recompose live.
    std::vector<std::unique_ptr<x3::game::EnvArtSystem>> towers;
    {
        const std::string cdir =
            "D:/Assets/_glb/tech/Urban Night City - Open World/Assets/GeeZyyGames/buildings/FBX";
        const float ts = [](){ const char* e=std::getenv("ECHO_TOWER_SCALE"); return e?(float)std::atof(e):0.34f; }();
        const float tlift = [](){ const char* e=std::getenv("ECHO_TOWER_LIFT"); return e?(float)std::atof(e):0.0f; }();
        const float tcx = [](){ const char* e=std::getenv("ECHO_TOWER_X"); return e?(float)std::atof(e):-20.0f; }();
        const float tcz = [](){ const char* e=std::getenv("ECHO_TOWER_Z"); return e?(float)std::atof(e):760.0f; }();
        const float sceneCX = 120.5f, sceneCZ = 174.9f;      // baked layout centre
        const float gy = hf.ok() ? hf.heightAt(tcx, tcz) : 190.0f;
        // Uniform scale + translate so the layout centre lands on (tcx,tcz) at ground.
        const float Tx = tcx - ts * sceneCX;
        const float Tz = tcz - ts * sceneCZ;
        const float M[16] = { ts,0,0,0,  0,ts,0,0,  0,0,ts,0,  Tx, gy + tlift, Tz, 1 };
        static const char* kBld[] = {
            "Building 01","Building 02","Building 03","Building 04","Building 05",
            "Building 06","Building 07","Building 08","Building 09","Building 10",
            "Building 11","Building 12","Building 14","Building 15","Building 16",
            "Building 17","Building 19","Building 20","Building 21","Building 22",
            "Building 23","Building 24","Building 25","Building 26","Building 27",
            "Building 28","Building 29","Building 33","Building 34","Building 35",
            "Building 36","Building 38","Building 39","Building 40","Building 41",
            "Building 43",
        };
        for (const char* b : kBld) {
            auto t = std::make_unique<x3::game::EnvArtSystem>();
            if (t->buildFromGlbAt(*device, cdir, std::string(b) + ".glb", M))
                towers.push_back(std::move(t));
        }
        x3::logInfo("--world echotropolis: DOWNTOWN SKYLINE — " +
                    std::to_string(towers.size()) + " Urban Night City towers on the crown");
    }

    // ===================== GOLD MINE + TRUCK LOT (the 2038 gold rush) =====
    // The physical gold mine SITE — purpose-built in Blender (assets/mine/mine_site.glb):
    // a timbered adit with a glowing gold seam, an A-frame headframe + pulley, an ore
    // cart on rails, tailings + ore chunks. It sits out on the WESTERN OPEN SHOULDER,
    // clear of the downtown towers, with a parked truck lot a short trek SW. The miners
    // are their OWN dedicated crew (built with the residents below) so they stand on the
    // mine's real terrain plane and walk truck-lot <-> seam. Scales are ECHO_*-tunable.
    // Base is at Y=0, ~15x17 m at scale 1 → ECHO_MINE_SCALE defaults ~1.6 (NOT 11).
    const float kMineX = -480.0f, kMineZ = 850.0f;   // mine mouth (Carry point B) — open west shoulder, clear of towers
    const float kLotX  = -556.0f, kLotZ  = 814.0f;   // truck lot   (Carry point A) — short trek SW
    const float kMineGy = hf.ok() ? hf.heightAt(kMineX, kMineZ) : 190.0f;  // real terrain; miners share this plane
    const float kMineScale  = [](){ const char* e=std::getenv("ECHO_MINE_SCALE");  return e?(float)std::atof(e):3.2f; }();
    const float kMineYaw    = [](){ const char* e=std::getenv("ECHO_MINE_YAW");    return e?(float)std::atof(e):2.35f; }();
    std::vector<std::unique_ptr<x3::game::EnvArtSystem>> mineProps;
    {
        const float kTruckScale = [](){ const char* e=std::getenv("ECHO_TRUCK_SCALE"); return e?(float)std::atof(e):1.0f;  }();
        const float kMineLift   = [](){ const char* e=std::getenv("ECHO_MINE_LIFT");   return e?(float)std::atof(e):0.0f;  }();
        auto place = [&](const std::string& dir, const char* glb, float x, float z,
                         float yaw, float s, float lift) {
            const float gy = kMineGy;
            const float c = std::cos(yaw), sn = std::sin(yaw);
            const float T[16] = { c*s, 0, -sn*s, 0,  0, s, 0, 0,  sn*s, 0, c*s, 0,
                                  x, gy + lift, z, 1 };
            auto e = std::make_unique<x3::game::EnvArtSystem>();
            if (e->buildFromGlbAt(*device, dir, glb, T)) mineProps.push_back(std::move(e));
        };
        place("D:/GameDev/EchoHarbor/assets/mine", "mine_site.glb",
              kMineX, kMineZ, kMineYaw, kMineScale, kMineLift);
        place("D:/Assets/_glb/tech/Industrial Small Truck Free/Assets/IndustrialSmallTruck/Art/fbx",
              "SmallTruck_1.glb", kLotX, kLotZ, 1.2f, kTruckScale, 0.0f);
        place("D:/Assets/_glb/tech/Mini Cargo Truck/Assets/MiniCargoTruck/FBX",
              "Truck1.glb", kLotX - 9.0f, kLotZ + 7.0f, 2.4f, kTruckScale, 0.0f);
        x3::logInfo("--world echotropolis: GOLD MINE site + truck lot — " +
                    std::to_string(mineProps.size()) + " props");
    }

    // ===================== MINE FOREST (thick woods around the pit) =========
    // Tim wants the gold mine nestled DEEP in the trees. Scatter Quaternius pines
    // (assets/veg, CC0 — woodBarkDark/leafsDark, base at Y=0) into a dense stand
    // ringing the mine — THICKEST behind it (away from the city) — pinned to real
    // terrain, jittered in place/scale/species. A clear inner pad keeps the headframe
    // + glowing seam readable; the approach wedge toward the city stays thinner so the
    // glow still reads from the vista. Each pine is one cached EnvArtSystem instance.
    std::vector<std::unique_ptr<x3::game::EnvArtSystem>> mineForest;
    {
        static const char* kPines[] = {
            "tree_pineTallA.glb", "tree_pineTallB.glb", "tree_pineTallC.glb",
            "tree_pineDefaultA.glb", "tree_pineDefaultB.glb", "tree_pineRoundB.glb",
        };
        auto hh = [](uint32_t n){ n=(n^61u)^(n>>16); n*=9u; n^=n>>4; n*=0x27d4eb2du; n^=n>>15;
                                  return (n & 0xffffffu) / (float)0x1000000; };
        const std::string vdir = "D:/GameDev/EchoHarbor/assets/veg";
        const float cx = kMineX, cz = kMineZ;
        const float behind = std::atan2(cz - 760.0f, cx - (-20.0f));   // WNW, away from city
        const float toCity = std::atan2(760.0f - cz, -20.0f - cx);     // approach wedge centre
        auto plant = [&](float x, float z, float sc, float yaw, int variant){
            const float gy = hf.ok() ? hf.heightAt(x, z) : kMineGy;        // match mine plane when no heightfield
            if (hf.ok() && gy < 4.0f) return;                              // skip the sea only with real terrain
            const float dxm=x-cx, dzm=z-cz; if (dxm*dxm+dzm*dzm < 13.0f*13.0f) return;   // clear pad
            const float dxl=x-kLotX, dzl=z-kLotZ; if (dxl*dxl+dzl*dzl < 11.0f*11.0f) return; // clear lot
            const float c=std::cos(yaw), s=std::sin(yaw);
            const float T[16] = { c*sc,0,-s*sc,0, 0,sc,0,0, s*sc,0,c*sc,0, x, gy, z, 1 };
            auto e = std::make_unique<x3::game::EnvArtSystem>();
            if (e->buildFromGlbAt(*device, vdir, kPines[variant % 6], T)) mineForest.push_back(std::move(e));
        };
        uint32_t seed = 0;
        auto emit = [&](int count, float aCenter, float aSpread, float rMin, float rMax){
            for (int i=0;i<count;++i,++seed){
                float a = aCenter + (hh(seed*3u+1u)*2.0f-1.0f)*aSpread;
                float r = rMin + hh(seed*3u+2u)*(rMax-rMin);
                float x = cx + r*std::cos(a), z = cz + r*std::sin(a);
                float da = std::fabs(std::atan2(std::sin(a-toCity), std::cos(a-toCity)));
                if (da < 0.55f && r < 55.0f && hh(seed*3u+7u) < 0.7f) continue;  // thin the city-side approach
                float sc  = 13.0f + hh(seed*7u+5u)*7.0f;                         // 20-30 m pines
                float yaw = hh(seed*7u+3u)*6.2831853f;
                plant(x, z, sc, yaw, (int)(hh(seed*7u+9u)*6.0f));
            }
        };
        emit(74, 0.0f,   3.15159f, 15.0f,  80.0f);   // all-around inner ring (full circle)
        emit(64, behind, 1.9f,     20.0f, 125.0f);   // THICK behind (away from the city)
        emit(44, 0.0f,   3.15159f, 78.0f, 150.0f);   // outer belt, full circle
        x3::logInfo("--world echotropolis: MINE FOREST — " +
                    std::to_string(mineForest.size()) + " pines ringing the pit");
    }

    // ===================== MINE MOUTH GLOW (authentic EoS arch) ============
    // Graft Empires of Shadow's real arch-SDF mouth glow (ported render FX in
    // mine_fx.cpp — "light licking the tunnel walls around a dark throat", gold
    // [1.0,0.80,0.16]) onto mine_site.glb's adit. A gold emissiveTex quad seated
    // at the mine mouth via the mine's OWN place() transform (so it tracks the
    // ECHO_MINE_* pose); all seat offsets are ECHO_GLOW_* tunable.
    x3::game::Scene mineGlowScene;
    x3::game::GoldMineWorld mineGlow;
    {
        auto envf = [](const char* k, float d){ const char* e=std::getenv(k); return e?(float)std::atof(e):d; };
        const float lY  = envf("ECHO_GLOW_LY", 1.70f);    // mouth-centre height (GLB units)
        const float lZ  = envf("ECHO_GLOW_LZ", 0.90f);    // mouth depth (front of the adit = +Z local)
        const float lHW = envf("ECHO_GLOW_HW", 1.30f);    // half width  (GLB units)
        const float lHH = envf("ECHO_GLOW_HH", 1.55f);    // half height (GLB units)
        const float c = std::cos(kMineYaw), sn = std::sin(kMineYaw), s = kMineScale;
        const float gx = kMineX + s * (sn * lZ);          // same rotation as the mine place() transform
        const float gy = kMineGy + s * lY;
        const float gz = kMineZ + s * (c * lZ);
        const float gYaw = envf("ECHO_GLOW_YAW", kMineYaw);   // face outward along the mouth (+Z local)
        mineGlow.buildMouthGlow(mineGlowScene, *device, gx, gy, gz, s * lHW, s * lHH, gYaw);
        x3::logInfo("--world echotropolis: MINE MOUTH GLOW (EoS arch) seated at mouth");
    }

    // ===================== THE UFO (Tim's Grok→Rodin saucer) ============
    // A mothership DRIFTING on a slow circular patrol high over the crown, spinning
    // + bobbing, with an emissive underbelly GLOW and a downward ABDUCTION BEAM
    // (ufo_fx.glb, authored in world metres) tracking it. Both re-posed per frame.
    x3::game::EnvArtSystem ufo, ufoFx;
    bool ufoBuilt = false, ufoFxBuilt = false;
    constexpr float kUfoScale = 120.0f;        // ~228 m across
    constexpr float kUfoCenX = -20.0f, kUfoCenZ = 760.0f, kUfoY = 470.0f;  // patrol centre
    constexpr float kUfoDriftR = 360.0f;       // patrol radius over the city
    {
        const std::string mdir = "D:/GameDev/SimCityLLM2/refs/models";
        const float T[16] = { kUfoScale,0,0,0, 0,kUfoScale,0,0, 0,0,kUfoScale,0, kUfoCenX,kUfoY,kUfoCenZ,1 };
        if (ufo.buildFromGlbAt(*device, mdir, "ufo.glb", T)) {
            ufoBuilt = true;
            x3::logInfo("--world echotropolis: THE UFO patrols the crown");
        }
        const float I[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, kUfoCenX,kUfoY,kUfoCenZ,1 };
        if (ufoFx.buildFromGlbAt(*device, mdir, "ufo_fx.glb", I)) ufoFxBuilt = true;
    }
    auto poseUfo = [&](float t) {
        const float w = t * 0.045f;                     // slow orbit
        const float ux = kUfoCenX + kUfoDriftR * std::cos(w);
        const float uz = kUfoCenZ + kUfoDriftR * std::sin(w);
        const float uy = kUfoY + std::sin(t * 0.30f) * 8.0f;   // bob
        const float spin = t * 0.22f, c = std::cos(spin), sn = std::sin(spin);
        const float s = kUfoScale;
        const float MU[16] = { c*s, 0.0f, -sn*s, 0.0f,  0.0f, s, 0.0f, 0.0f,
                               sn*s, 0.0f,  c*s, 0.0f,   ux, uy, uz, 1.0f };
        ufo.setInstanceTransform(0, MU);
        // FX authored in world metres: translate under the hull (no spin/scale).
        const float MF[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, ux, uy, uz, 1.0f };
        if (ufoFxBuilt) ufoFx.setInstanceTransform(0, MF);
    };

    // ===================== AIRCRAFT: helicopters + planes ================
    // Real converted GLBs (Low Poly Helicopters + Civil Transport plane, FBX→GLB
    // via tools/convert_fbx_glb.py). Helis fly slow patrol circles low over the
    // city with a SEPARATELY spun main rotor (heli_body.glb + hub-centred
    // heli_rotor.glb — both re-posed per frame, the two-system trick the UFO uses);
    // planes make high straight passes and wrap. Scale/yaw are ECHO_*-tunable so
    // they can be dialled without a rebuild; worldBounds is logged so the true size
    // is visible in the launch log. kRotorHubY is the rotor hub's up-offset (GLB
    // metres) measured when the mesh was split.
    const std::string adir = "D:/GameDev/EchoHarbor/assets/aircraft";
    const float kHeliScale  = [](){ const char* e=std::getenv("ECHO_HELI_SCALE");  return e?(float)std::atof(e):2.2f;  }();
    const float kPlaneScale = [](){ const char* e=std::getenv("ECHO_PLANE_SCALE"); return e?(float)std::atof(e):0.8f;  }();
    const float kHeliYaw    = [](){ const char* e=std::getenv("ECHO_HELI_YAW");    return e?(float)std::atof(e):0.0f;  }();
    const float kPlaneYaw   = [](){ const char* e=std::getenv("ECHO_PLANE_YAW");   return e?(float)std::atof(e):0.0f;  }();
    const float kRotorRpm   = [](){ const char* e=std::getenv("ECHO_ROTOR_RPM");   return e?(float)std::atof(e):9.0f;  }();
    const float kNavScale   = [](){ const char* e=std::getenv("ECHO_NAV_SCALE");   return e?(float)std::atof(e):1.6f;  }();
    constexpr float kRotorHubY = 8.32f;   // GLB-space up offset of the rotor hub (from the split)

    // Helicopter = heli_body.glb + hub-centred heli_rotor.glb as TWO posed instances
    // (the UFO two-system trick). The MAIN ROTOR SPINS: heading + spin are both Y
    // rotations so they compose to one Y rotation; the rotor rides kRotorHubY*s above
    // the body. This is the reliable, cheap animation (no skinning) — scales to any
    // heli count. (The rigged OH-1's bone-driven rotor is a separate hero pass.)
    // NIGHT NAV-LIGHTS: red (port) + green (starboard) steady + a white anti-collision
    // strobe, each a tiny emissive dot GLB (assets/aircraft/navlight_*.glb) posed at a
    // body-local offset every frame and drawn ONLY at night (todS.cityLightsOn).
    struct Heli  { std::unique_ptr<x3::game::EnvArtSystem> body, rotor, navR, navG, navW;
                   float cx, cz, r, y, w, phase; };
    struct Plane { std::unique_ptr<x3::game::EnvArtSystem> body, navR, navG, navW;
                   float x0, z0, dx, dz, len, y, speed, phase; };
    std::vector<Heli> helis;
    std::vector<Plane> planes;

    auto loadNav = [&](const char* glb) {
        auto e = std::make_unique<x3::game::EnvArtSystem>();
        const float I[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        if (!e->buildFromGlbAt(*device, adir, glb, I)) e.reset();
        return e;
    };
    auto addHeli = [&](float cx, float cz, float r, float y, float w, float phase) {
        Heli h;
        h.body  = std::make_unique<x3::game::EnvArtSystem>();
        h.rotor = std::make_unique<x3::game::EnvArtSystem>();
        const float I[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, cx, y, cz, 1 };
        const bool okB = h.body->buildFromGlbAt(*device, adir, "heli_body.glb", I);
        const bool okR = h.rotor->buildFromGlbAt(*device, adir, "heli_rotor.glb", I);
        if (okB && okR) { h.cx=cx; h.cz=cz; h.r=r; h.y=y; h.w=w; h.phase=phase;
                          h.navR = loadNav("navlight_red.glb");
                          h.navG = loadNav("navlight_green.glb");
                          h.navW = loadNav("navlight_white.glb");
                          helis.push_back(std::move(h)); }
    };
    addHeli(-20.0f,  760.0f, 250.0f, 275.0f,  0.10f, 0.0f);
    addHeli( 120.0f, 620.0f, 185.0f, 250.0f, -0.13f, 2.1f);
    addHeli(-170.0f, 850.0f, 300.0f, 300.0f,  0.08f, 4.2f);

    auto addPlane = [&](float x0, float z0, float dx, float dz, float len,
                        float y, float speed, float phase) {
        Plane p; p.body = std::make_unique<x3::game::EnvArtSystem>();
        const float I[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, x0, y, z0, 1 };
        if (p.body->buildFromGlbAt(*device, adir, "plane_1.glb", I)) {
            const float L = std::sqrt(dx*dx + dz*dz);
            p.x0=x0; p.z0=z0; p.dx=dx/L; p.dz=dz/L; p.len=len;
            p.y=y; p.speed=speed; p.phase=phase;
            p.navR = loadNav("navlight_red.glb");
            p.navG = loadNav("navlight_green.glb");
            p.navW = loadNav("navlight_white.glb");
            planes.push_back(std::move(p));
        }
    };
    addPlane(-620.0f, 500.0f,  1.0f,  0.28f, 1500.0f, 420.0f, 60.0f,   0.0f);
    addPlane( 720.0f, 1050.0f,-1.0f, -0.42f, 1500.0f, 470.0f, 46.0f, 720.0f);

    x3::logInfo("--world echotropolis: AIRCRAFT — " +
        std::to_string(helis.size()) + " helis + " + std::to_string(planes.size()) + " planes");

    // Helis orbit + bob, nose tangent to the circle; MAIN ROTOR SPINS.
    auto poseHeli = [&](Heli& h, float t) {
        const float a = h.phase + t * h.w;
        const float x = h.cx + std::cos(a) * h.r;
        const float z = h.cz + std::sin(a) * h.r;
        const float y = h.y + std::sin(t * 0.5f + h.phase) * 3.0f;
        const float heading = a + (h.w > 0.0f ? 1.5708f : -1.5708f) + kHeliYaw;
        const float s = kHeliScale;
        const float ch = std::cos(heading), sh = std::sin(heading);
        const float MB[16] = { ch*s,0,-sh*s,0,  0,s,0,0,  sh*s,0,ch*s,0,  x, y, z, 1 };
        h.body->setInstanceTransform(0, MB);
        // rotor: heading + fast spin are both about Y → compose to one Y rotation;
        // rides kRotorHubY*s above the body (offset on Y, unchanged by the heading yaw).
        const float hs = heading + t * kRotorRpm;
        const float chs = std::cos(hs), shs = std::sin(hs);
        const float hub = kRotorHubY * s;
        const float MR[16] = { chs*s,0,-shs*s,0, 0,s,0,0, shs*s,0,chs*s,0, x, y+hub, z, 1 };
        h.rotor->setInstanceTransform(0, MR);
        // NAV-LIGHTS: pose each dot at a body-local offset (ox right, oy up, oz fwd),
        // world = pos + R(heading)*bodyScale*offset, drawn as a translated emissive dot.
        auto poseLight = [&](x3::game::EnvArtSystem* e, float ox, float oy, float oz, float ns){
            if (!e) return;
            const float wx = x + s*(ch*ox + sh*oz);
            const float wy = y + s*oy;
            const float wz = z + s*(-sh*ox + ch*oz);
            const float M[16] = { ns,0,0,0, 0,ns,0,0, 0,0,ns,0, wx, wy, wz, 1 };
            e->setInstanceTransform(0, M);
        };
        poseLight(h.navR.get(), -2.7f, 0.2f,  0.4f, kNavScale);   // port  (red)
        poseLight(h.navG.get(),  2.7f, 0.2f,  0.4f, kNavScale);   // starboard (green)
        const bool strobeOn = std::fmod(t + h.phase, 1.25f) < 0.11f;   // anti-collision strobe
        poseLight(h.navW.get(), 0.0f, 1.3f, -3.9f, strobeOn ? kNavScale*1.35f : 0.0f);
    };
    auto posePlane = [&](Plane& p, float t) {
        const float d = std::fmod(p.phase + t * p.speed, p.len);
        const float x = p.x0 + p.dx * d;
        const float z = p.z0 + p.dz * d;
        const float heading = std::atan2(p.dx, p.dz) + kPlaneYaw;
        const float s = kPlaneScale;
        const float ch = std::cos(heading), sh = std::sin(heading);
        const float M[16] = { ch*s,0,-sh*s,0,  0,s,0,0,  sh*s,0,ch*s,0,  x, p.y, z, 1 };
        p.body->setInstanceTransform(0, M);
        auto poseLight = [&](x3::game::EnvArtSystem* e, float ox, float oy, float oz, float ns){
            if (!e) return;
            const float wx = x + s*(ch*ox + sh*oz);
            const float wy = p.y + s*oy;
            const float wz = z + s*(-sh*ox + ch*oz);
            const float MM[16] = { ns,0,0,0, 0,ns,0,0, 0,0,ns,0, wx, wy, wz, 1 };
            e->setInstanceTransform(0, MM);
        };
        poseLight(p.navR.get(), -11.0f, 0.0f,  0.0f, kNavScale*1.4f);  // port  wingtip (red)
        poseLight(p.navG.get(),  11.0f, 0.0f,  0.0f, kNavScale*1.4f);  // starboard wingtip (green)
        const bool strobeOn = std::fmod(t + p.phase, 1.6f) < 0.10f;    // tail strobe
        poseLight(p.navW.get(), 0.0f, 1.0f, -9.0f, strobeOn ? kNavScale*1.6f : 0.0f);
    };

    // ===================== STREET TRAFFIC (Modular Cyber Racing Cars) =====
    // Low-poly cyberpunk cars (Draco GLBs from D:/Assets — the engine decodes Draco)
    // cruising straight avenue loops across the downtown crown at ground height. Each
    // car cycles a Car_01..15 variant, faces its travel direction (+Z-forward models),
    // and wraps its lane. Purely visual, drawn every frame day + night.
    const std::string cardir =
        "D:/Assets/_glb/tech/Modular Cyber Racing Cars - Low Poly 3D Models/Assets/ithappy/Modular_Cyber_Racing_Cars/Meshes";
    const float kCarScale = [](){ const char* e=std::getenv("ECHO_CAR_SCALE"); return e?(float)std::atof(e):1.4f; }();
    const float kCarYaw   = [](){ const char* e=std::getenv("ECHO_CAR_YAW");   return e?(float)std::atof(e):0.0f; }();
    const float kCarY = hf.ok() ? hf.heightAt(-20.0f, 760.0f) : 190.0f;   // crown ground (= tower bases)
    struct Car { std::unique_ptr<x3::game::EnvArtSystem> body;
                 float sx, sz, dx, dz, len, speed, off; };
    std::vector<Car> cars;
    auto addCar = [&](int variant, float sx, float sz, float dx, float dz, float len,
                      float speed, float off){
        const float L = std::sqrt(dx*dx + dz*dz); dx/=L; dz/=L;
        const std::string vs = (variant < 10 ? "0" : "") + std::to_string(variant);
        const std::string name = "Car_" + vs + "/Car_" + vs + ".glb";
        Car c; c.body = std::make_unique<x3::game::EnvArtSystem>();
        const float I[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, sx, kCarY, sz, 1 };
        if (c.body->buildFromGlbAt(*device, cardir, name, I)) {
            c.sx=sx; c.sz=sz; c.dx=dx; c.dz=dz; c.len=len; c.speed=speed; c.off=off;
            cars.push_back(std::move(c));
        }
    };
    {
        struct Lane { float sx, sz, dx, dz, len, speed; int n; };
        const Lane lanes[] = {
            { -330.0f, 702.0f,  1.0f,  0.0f, 620.0f, 34.0f, 5 },   // E-W south, eastbound
            {  290.0f, 742.0f, -1.0f,  0.0f, 620.0f, 30.0f, 5 },   // E-W, westbound
            { -330.0f, 818.0f,  1.0f,  0.0f, 620.0f, 32.0f, 4 },   // E-W north, eastbound
            {    2.0f, 560.0f,  0.0f,  1.0f, 400.0f, 28.0f, 4 },   // N-S, northbound
            { -150.0f, 960.0f,  0.0f, -1.0f, 400.0f, 26.0f, 4 },   // N-S, southbound
        };
        int variant = 1;
        for (const Lane& ln : lanes)
            for (int k = 0; k < ln.n; ++k) {
                addCar(variant, ln.sx, ln.sz, ln.dx, ln.dz, ln.len, ln.speed,
                       ln.len * (float)k / (float)ln.n);
                variant = (variant % 15) + 1;
            }
        x3::logInfo("--world echotropolis: STREET TRAFFIC — " + std::to_string(cars.size()) + " cars");
    }
    auto poseCar = [&](Car& c, float t){
        const float d = std::fmod(c.off + t * c.speed, c.len);
        const float x = c.sx + c.dx * d;
        const float z = c.sz + c.dz * d;
        const float heading = std::atan2(c.dx, c.dz) + kCarYaw;   // +Z-forward faces travel dir
        const float s = kCarScale, ch = std::cos(heading), sh = std::sin(heading);
        const float M[16] = { ch*s,0,-sh*s,0, 0,s,0,0, sh*s,0,ch*s,0, x, kCarY, z, 1 };
        c.body->setInstanceTransform(0, M);
    };

    // ===================== HARBOR BOATS ==================================
    // Low-poly boats (Boats Pack, Draco GLBs) cruising straight lanes on the open
    // SEA at water level (y~0), wrapping their lane with a gentle bob. Only visible
    // over real water (a boat over land is buried underground) — lanes sit off the
    // island's coast. +Z-forward hull; heading = atan2(dx,dz).
    const std::string boatdir = "D:/Assets/_glb/tech/Boats Pack/Assets/Boats/Models";
    const float kBoatScale = [](){ const char* e=std::getenv("ECHO_BOAT_SCALE"); return e?(float)std::atof(e):6.5f; }();
    const float kBoatYaw   = [](){ const char* e=std::getenv("ECHO_BOAT_YAW");   return e?(float)std::atof(e):0.0f; }();
    const float kBoatY     = [](){ const char* e=std::getenv("ECHO_BOAT_Y");     return e?(float)std::atof(e):0.6f; }();
    struct Boat { std::unique_ptr<x3::game::EnvArtSystem> body; float sx,sz,dx,dz,len,speed,off; };
    std::vector<Boat> boats;
    auto addBoat = [&](const char* glb, float sx, float sz, float dx, float dz,
                       float len, float speed, float off){
        const float L = std::sqrt(dx*dx + dz*dz); dx/=L; dz/=L;
        Boat b; b.body = std::make_unique<x3::game::EnvArtSystem>();
        const float I[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, sx, kBoatY, sz, 1 };
        if (b.body->buildFromGlbAt(*device, boatdir, glb, I)) {
            b.sx=sx; b.sz=sz; b.dx=dx; b.dz=dz; b.len=len; b.speed=speed; b.off=off;
            boats.push_back(std::move(b));
        }
    };
    {
        static const char* kB[] = { "Boat_1.glb","Boat_2.glb","Boat_3.glb","Boat_4.glb" };
        struct BL { float sx,sz,dx,dz,len,speed; int n; };
        const BL bl[] = {
            { -400.0f,  330.0f,  1.0f,  0.0f, 760.0f, 15.0f, 4 },   // south bay, eastbound
            {  340.0f,  240.0f, -1.0f,  0.0f, 760.0f, 13.0f, 4 },   // south bay, westbound
            { -560.0f,  260.0f,  0.0f,  1.0f, 420.0f, 12.0f, 3 },   // SW inlet, northbound
        };
        int vi = 0;
        for (const BL& l : bl)
            for (int k = 0; k < l.n; ++k) {
                addBoat(kB[vi % 4], l.sx, l.sz, l.dx, l.dz, l.len, l.speed, l.len * (float)k / (float)l.n);
                ++vi;
            }
        x3::logInfo("--world echotropolis: HARBOR BOATS — " + std::to_string(boats.size()) + " boats");
    }
    auto poseBoat = [&](Boat& b, float t){
        const float d = std::fmod(b.off + t * b.speed, b.len);
        const float x = b.sx + b.dx * d, z = b.sz + b.dz * d;
        const float y = kBoatY + std::sin(t * 0.7f + b.off) * 0.35f;   // gentle bob
        const float heading = std::atan2(b.dx, b.dz) + kBoatYaw;
        const float s = kBoatScale, ch = std::cos(heading), sh = std::sin(heading);
        const float M[16] = { ch*s,0,-sh*s,0, 0,s,0,0, sh*s,0,ch*s,0, x, y, z, 1 };
        b.body->setInstanceTransform(0, M);
    };

    // ===================== SKY DRONES (cyberpunk patrol/delivery) =========
    // Small sci-fi drones (Draco GLBs) weaving slow patrol circles over the crown at
    // varied mid-altitudes — vertical life between the towers. Bob + face tangent.
    const std::string dronedirA = "D:/Assets/_glb/tech/Sci-Fi-Drone/Assets/scifi-drone/mesh";
    const std::string dronedirB = "D:/Assets/_glb/tech/Sci fi Drones/Assets/Sci_fi_Drones/Models";
    const float kDroneScale = [](){ const char* e=std::getenv("ECHO_DRONE_SCALE"); return e?(float)std::atof(e):7.0f; }();
    struct Drone { std::unique_ptr<x3::game::EnvArtSystem> body; float cx,cz,r,y,w,phase; };
    std::vector<Drone> drones;
    auto addDrone = [&](const std::string& dir, const char* glb, float cx, float cz,
                        float r, float y, float w, float phase){
        Drone d; d.body = std::make_unique<x3::game::EnvArtSystem>();
        const float I[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, cx, y, cz, 1 };
        if (d.body->buildFromGlbAt(*device, dir, glb, I)) {
            d.cx=cx; d.cz=cz; d.r=r; d.y=y; d.w=w; d.phase=phase;
            drones.push_back(std::move(d));
        }
    };
    addDrone(dronedirA, "drone.glb",                    -20.0f,  760.0f, 150.0f, 210.0f,  0.26f, 0.0f);
    addDrone(dronedirA, "drone.glb",                    110.0f,  660.0f, 120.0f, 250.0f, -0.32f, 1.7f);
    addDrone(dronedirB, "Robot_Scout_HyperX_Unity.glb", -160.0f, 840.0f, 180.0f, 190.0f,  0.22f, 3.1f);
    addDrone(dronedirA, "drone.glb",                    -60.0f,  900.0f, 130.0f, 285.0f,  0.30f, 4.4f);
    addDrone(dronedirB, "Robot_Scout_HyperX_Unity.glb",  60.0f,  800.0f, 200.0f, 165.0f, -0.24f, 5.5f);
    addDrone(dronedirA, "drone.glb",                    -220.0f, 700.0f, 110.0f, 300.0f, -0.28f, 2.3f);
    x3::logInfo("--world echotropolis: SKY DRONES — " + std::to_string(drones.size()) + " drones");
    auto poseDrone = [&](Drone& d, float t){
        const float a = d.phase + t * d.w;
        const float x = d.cx + std::cos(a) * d.r;
        const float z = d.cz + std::sin(a) * d.r;
        const float y = d.y + std::sin(t * 1.3f + d.phase) * 6.0f;   // hover bob
        const float heading = a + (d.w > 0.0f ? 1.5708f : -1.5708f);
        const float s = kDroneScale, ch = std::cos(heading), sh = std::sin(heading);
        const float M[16] = { ch*s,0,-sh*s,0, 0,s,0,0, sh*s,0,ch*s,0, x, y, z, 1 };
        d.body->setInstanceTransform(0, M);
    };

    // ===================== OH1 HERO HELICOPTER (rigged, bone-driven rotor) ==
    // The hero bird: the rigged OH1.glb flown as an INERT MonsterSystem prop — the
    // proven skinned-draw path (MonsterSystem owns the model load + Skinner + the
    // direct drawMonster fan). We drive its world pose each frame along a patrol
    // circle and loop its "Rotor" clip via the calm-loop hook, so the MAIN ROTOR
    // spins from its ACTUAL bones (not a rigid re-pose). Built in BOTH the windowed
    // loop and the headless capture (each owns its own MonsterSystem + scene/physics).
    const float kOh1Scale = [](){ const char* e=std::getenv("ECHO_OH1_SCALE"); return e?(float)std::atof(e):0.03f; }();
    const float kOh1Yaw   = [](){ const char* e=std::getenv("ECHO_OH1_YAW");   return e?(float)std::atof(e):0.0f;  }();
    constexpr float kOh1CenX=-20.0f, kOh1CenZ=760.0f, kOh1R=215.0f, kOh1Alt=235.0f, kOh1W=0.09f;
    auto buildOh1 = [&](x3::game::MonsterSystem& m, x3::game::Scene& sc, x3::phys::IPhysicsWorld& ph){
        x3::game::MonsterSystem::Tuning t;
        t.type = x3::game::MonsterType::Guard;
        t.chaseSpeed = 0.0f; t.damage = 0; t.noBody = true;   // inert visual prop
        t.modelFile = "OH1.glb"; t.modelDirOverride = x3::game::riggedGlbRoot();
        t.standUpZtoY = false; t.modelScale = kOh1Scale;
        const x3::phys::Vec3 p0{ kOh1CenX + kOh1R, kOh1Alt, kOh1CenZ };
        m.buildMonsterTuned(sc, *device, ph, x3::game::riggedGlbRoot(), p0, t);
        m.setCalmLoop("Rotor");                               // spin the MAIN rotor clip
        x3::logInfo(std::string("--world echotropolis: OH1 buildOh1 real=") +
                    (m.usingRealModel()?"1":"0") + " skinnable=" + (m.skinnable()?"1":"0") +
                    " rotorClip=" + (m.calmLoopActive()?"FOUND":"MISSING"));
        return m.usingRealModel();
    };
    auto flyOh1 = [&](x3::game::MonsterSystem& m, float t){
        const float a = t * kOh1W;
        const x3::phys::Vec3 p{ kOh1CenX + kOh1R*std::cos(a),
                                kOh1Alt + std::sin(t*0.5f)*4.0f,
                                kOh1CenZ + kOh1R*std::sin(a) };
        m.setPropPose(p, a + 1.5708f + kOh1Yaw);              // nose tangent to the circle
        m.setPropMotion(0.0f, 0.10f);                         // speed 0 => rotor calm-loop plays
    };

    // RESIDENTS: character height (5-6 ft). Env-tunable (ECHO_PED_SCALE) so it can be
    // dialed live without a rebuild; default 1.7 → ~5.9 ft citizens. (The citizens
    // ARE real textured animated people — the earlier "green blobs" were the grass-
    // tuft flora props in echotropolis_props.glb, misread as residents.)
    const float kPedScale = [](){ const char* e = std::getenv("ECHO_PED_SCALE");
                                  return e ? (float)std::atof(e) : 1.7f; }();

    // ===================== CITY PANEL (web-parity dashboard) =============
    // The toggleable right-side City panel (TAB) — the native counterpart to the web
    // Echo Harbor's city view: a population breakdown, the treasury/gold economy, and
    // a Harbor Herald news feed generated from the LIVE city numbers. Pure HUD
    // (drawHudQuad/drawHudText); rendered in the windowed loop when open and in
    // captures when ECHO_PANEL=1. All data is passed in so both paths share one draw.
    auto drawCityPanel = [&](const x3::rhi::FrameContext& fc, uint32_t hw, uint32_t hh,
                             int year, const char* dow, int day, int hour, int minute,
                             uint32_t pop, uint32_t working, uint32_t leisure,
                             double treasury, double goldOz, uint32_t miners, double dailyNet){
        const float pw = 452.0f, pad = 20.0f;
        const float ph = std::min((float)hh - 96.0f, 548.0f);
        const float px = (float)hw - pw - 24.0f, py = 56.0f;
        const float bg[4]   = { 0.03f, 0.05f, 0.09f, 0.90f };
        const float hstr[4] = { 0.10f, 0.14f, 0.20f, 0.96f };
        const float hdr[4]  = { 1.0f, 0.82f, 0.42f, 1.0f };     // gold
        const float lab[4]  = { 0.60f, 0.70f, 0.82f, 1.0f };    // cool grey-blue
        const float val[4]  = { 0.92f, 0.96f, 1.0f, 1.0f };     // near-white
        const float sep[4]  = { 0.30f, 0.42f, 0.55f, 0.55f };
        const float vig[4]  = { 1.0f, 0.62f, 0.18f, 1.0f };     // vigil orange (news bullets)
        device->drawHudQuad(fc, px, py, pw, ph, bg);
        device->drawHudQuad(fc, px, py, pw, 40.0f, hstr);
        device->drawHudText(fc, "ECHO HARBOR  //  CITY", px + pad, py + 12.0f, 16.0f, hdr);
        float ty = py + 52.0f;
        char ln[160];
        auto row = [&](const char* label, const char* value){
            device->drawHudText(fc, label, px + pad, ty, 14.0f, lab);
            const float vx = px + pw - pad - (float)std::strlen(value) * 14.0f;
            device->drawHudText(fc, value, vx, ty, 14.0f, val);
            ty += 26.0f;
        };
        auto head = [&](const char* label){
            ty += 6.0f;
            device->drawHudQuad(fc, px + pad, ty + 9.0f, pw - 2.0f*pad, 1.5f, sep);
            ty += 16.0f;
            device->drawHudText(fc, label, px + pad, ty, 13.0f, hdr);
            ty += 24.0f;
        };
        std::snprintf(ln, sizeof ln, "%d   %s   DAY %d    %02d:%02d", year, dow, day, hour, minute);
        device->drawHudText(fc, ln, px + pad, ty, 13.0f, lab); ty += 30.0f;
        const uint32_t about = (pop > working + leisure) ? pop - working - leisure : 0u;
        head("POPULATION");
        std::snprintf(ln, sizeof ln, "%u", pop);     row("RESIDENTS",     ln);
        std::snprintf(ln, sizeof ln, "%u", working); row("  ON SHIFT",    ln);
        std::snprintf(ln, sizeof ln, "%u", leisure); row("  AT LEISURE",  ln);
        std::snprintf(ln, sizeof ln, "%u", about);   row("  ABOUT TOWN",  ln);
        head("TREASURY");
        std::snprintf(ln, sizeof ln, "$%0.0f", treasury);       row("BALANCE",    ln);
        std::snprintf(ln, sizeof ln, "%+0.0f/day", dailyNet);   row("NET FLOW",   ln);
        std::snprintf(ln, sizeof ln, "%0.0f oz", goldOz);       row("GOLD MINED", ln);
        std::snprintf(ln, sizeof ln, "%u", miners);             row("MINE CREW",  ln);
        head("HARBOR HERALD");
        static const char* kFlavor[] = {
            "Night market draws record crowds on the crown.",
            "New arrivals swell the harbour districts.",
            "Skyline drones reroute around tower 34.",
            "Fishing fleet returns heavy from the south bay.",
            "Vigil reports a quiet watch over Echo Harbor.",
        };
        char nz[3][140];
        std::snprintf(nz[0], 140, "Gold rush swells the vaults: %0.0f oz banked.", goldOz);
        std::snprintf(nz[1], 140, "%u on shift, %u out about the crown today.", working, about);
        std::snprintf(nz[2], 140, "%s", kFlavor[(day + hour) % 5]);
        // Clip each line to the panel's inner width so it never runs off the edge.
        const int maxNews = (int)((pw - 2.0f*pad - 16.0f) / 12.0f);   // glyph advance ~12px
        for (int i = 0; i < 3; ++i) {
            if ((int)std::strlen(nz[i]) > maxNews && maxNews > 2) {
                nz[i][maxNews - 2] = '.'; nz[i][maxNews - 1] = '.'; nz[i][maxNews] = '\0';
            }
            device->drawHudText(fc, "-",   px + pad,        ty, 13.0f, vig);
            device->drawHudText(fc, nz[i], px + pad + 16.0f, ty, 12.0f, val);
            ty += 22.0f;
        }
        device->drawHudText(fc, "TAB  close", px + pw - pad - 10.0f*11.0f, py + ph - 24.0f, 11.0f, lab);
    };

    // RESIDENT INSPECTOR — the web "click a resident" card. In walk mode the citizen
    // nearest the crosshair gets a bottom-left life card: name, role, what they're doing
    // right now, one telling detail, and a voice line. Fed from the live NpcAgent.
    auto drawResidentCard = [&](const x3::rhi::FrameContext& fc, uint32_t /*hw*/, uint32_t hh,
                                const char* name, const char* role, const char* activity,
                                const char* detail, const char* voice, float dist){
        const float cw = 470.0f, pad = 18.0f;
        const float cx = 24.0f, cy = (float)hh - 176.0f, ch = 152.0f;
        const float bg[4]  = { 0.03f, 0.05f, 0.09f, 0.90f };
        const float hstr[4]= { 0.10f, 0.14f, 0.20f, 0.96f };
        const float hdr[4] = { 1.0f, 0.82f, 0.42f, 1.0f };
        const float lab[4] = { 0.60f, 0.70f, 0.82f, 1.0f };
        const float val[4] = { 0.92f, 0.96f, 1.0f, 1.0f };
        const float vig[4] = { 1.0f, 0.62f, 0.18f, 1.0f };
        // Clip a line to the card inner width at the given glyph size.
        auto clip = [&](char* s, float glyph, float indent){
            const int mx = (int)((cw - 2.0f*pad - indent) / (glyph * 0.92f));
            if ((int)std::strlen(s) > mx && mx > 2) { s[mx-2]='.'; s[mx-1]='.'; s[mx]='\0'; }
        };
        device->drawHudQuad(fc, cx, cy, cw, ch, bg);
        device->drawHudQuad(fc, cx, cy, cw, 34.0f, hstr);
        char nm[96]; std::snprintf(nm, sizeof nm, "%s", name ? name : "RESIDENT");
        clip(nm, 15.0f, 0.0f);
        device->drawHudText(fc, nm, cx + pad, cy + 9.0f, 15.0f, hdr);
        char rl[64]; std::snprintf(rl, sizeof rl, "%s", role ? role : "");
        device->drawHudText(fc, rl, cx + cw - pad - (float)std::strlen(rl)*11.0f, cy + 11.0f, 11.0f, lab);
        float ty = cy + 44.0f;
        char act[96]; std::snprintf(act, sizeof act, "NOW: %s   %0.0fm", activity ? activity : "", dist);
        clip(act, 13.0f, 0.0f);
        device->drawHudText(fc, act, cx + pad, ty, 13.0f, val); ty += 26.0f;
        if (detail && detail[0]) {
            char dl[140]; std::snprintf(dl, sizeof dl, "- %s", detail); clip(dl, 12.0f, 0.0f);
            device->drawHudText(fc, dl, cx + pad, ty, 12.0f, lab); ty += 24.0f;
        }
        if (voice && voice[0]) {
            char vl[140]; std::snprintf(vl, sizeof vl, "\"%s\"", voice); clip(vl, 13.0f, 0.0f);
            device->drawHudText(fc, vl, cx + pad, ty, 13.0f, vig);
        }
    };

    // ===================== Headless screenshot path =====================
    // Pose the default orbit (17deg, radius 70), settle the waves a few frames so
    // the Gerstner surface isn't flat, then arm+grab. Mirrors host_valley's grab.
    if (headless || screenshot) {
        // Snap the smoothed state onto the targets (no live input to ease from).
        rig.sFocusX = rig.focusX; rig.sFocusZ = rig.focusZ;
        rig.sYaw = rig.yaw; rig.sPitch = rig.pitch; rig.sRadius = rig.radius;

        const std::string outPath = screenshot ? screenshotPath
                                               : std::string("agent_echotropolis.png");
        // ECHO_RESIDENTS=1 → build the citizen crowd + rigged skins for the capture
        // (verify/showcase the living city in a still). Extra settle frames let the
        // deferred skin spawns drain. Local instances (headless never runs the loop).
        const bool shotResidents = [](){ const char* e = std::getenv("ECHO_RESIDENTS"); return e && e[0]=='1'; }();
        std::unique_ptr<x3::phys::IPhysicsWorld> sphys;
        x3::game::Scene sScene; x3::game::CrowdSystem sCrowd; x3::game::CrowdSkin sSkin;
        x3::game::CrowdSystem sMiners; x3::game::CrowdSkin sMinersSkin; bool sMinersBuilt = false;
        x3::game::MonsterSystem sOh1; bool sOh1Built = false;   // OH1 hero heli (capture)
        x3::game::HoloTerminal sOps; bool sOpsBuilt = false;
        x3::game::NpcLife sNpc; bool sNpcBuilt = false;   // living-city NPCs (schedules/archetypes)
        bool sResBuilt = false;
        if (shotResidents && hf.ok()) {
            sphys.reset(x3::phys::createPhysicsWorld());
            if (sphys && sphys->init()) {
                x3::game::CrowdConfig cc; cc.count = 40;
                cc.centerX = -20.0f; cc.centerZ = 760.0f;
                cc.groundY = hf.heightAt(-20.0f, 760.0f); cc.radius = 340.0f;
                cc.walkSpeed = 1.3f; cc.converse = true;
                sCrowd.build(cc, sScene, *device);
                // GOLD-MINE CREW (capture): dedicated crowd at the western mine so stills
                // verify the miners hauling the truck-lot -> seam route on the mine plane.
                {
                    x3::game::CrowdConfig mc; mc.count = 9;
                    mc.centerX = kMineX; mc.centerZ = kMineZ;
                    mc.groundY = kMineGy; mc.radius = 26.0f;
                    mc.walkSpeed = 1.15f; mc.converse = false;
                    using MKs = x3::game::CrowdWorkPoint;
                    mc.work = {
                        { MKs::Kind::Carry, kLotX,        kLotZ,        kMineX,        kMineZ        },
                        { MKs::Kind::Carry, kLotX - 6.0f, kLotZ + 4.0f, kMineX + 6.0f, kMineZ - 4.0f },
                        { MKs::Kind::Carry, kLotX + 5.0f, kLotZ - 5.0f, kMineX - 5.0f, kMineZ + 5.0f },
                        { MKs::Kind::Carry, kLotX - 3.0f, kLotZ - 3.0f, kMineX + 3.0f, kMineZ + 6.0f },
                        { MKs::Kind::Carry, kLotX + 8.0f, kLotZ + 6.0f, kMineX - 7.0f, kMineZ - 2.0f },
                    };
                    sMiners.build(mc, sScene, *device);
                    x3::game::CrowdSkinConfig msc; msc.site = "miners-shot";
                    msc.modelDir = x3::game::riggedGlbRoot(); msc.spawnsPerFrame = 4;
                    msc.scale = kPedScale;
                    sMinersSkin.build(msc, sMiners);
                    sMinersBuilt = sMiners.built();
                }
                sOh1Built = buildOh1(sOh1, sScene, *sphys);   // OH1 hero heli (capture)
                // CONTROL ROOM ops dashboard (verify in captures)
                sOps.build(sScene, *device,
                           x3::phys::Vec3{ -14.0f, hf.heightAt(-20.0f, 760.0f) + 2.2f, 752.0f },
                           0.0f, 6.4f, 3.6f);
                sOps.setLayout(x3::game::HoloTerminal::Layout::Readout);
                sOps.setTextColor(1.0f, 0.62f, 0.18f, 1.0f);
                sOpsBuilt = sOps.built();
                x3::game::CrowdSkinConfig sc; sc.site = "residents-shot";
                sc.modelDir = x3::game::riggedGlbRoot(); sc.spawnsPerFrame = 4;
                sc.scale = kPedScale;   // 5-6 ft citizens
                sSkin.build(sc, sCrowd);
                sResBuilt = sCrowd.built();
                // LIVING CITY: the 12-archetype NPCs with real daily schedules on the crown.
                x3::game::NpcLifeConfig lc;
                lc.centerX = -20.0f; lc.centerZ = 760.0f;
                lc.groundY = hf.heightAt(-20.0f, 760.0f);
                lc.freewayMovers = 0;            // no freeway in the crown
                sNpc.build(lc, sScene, *device);
                sNpcBuilt = sNpc.built();
                x3::logInfo(std::string("--world echotropolis: SHOT NpcLife ") +
                            (sNpcBuilt ? "built (living-city archetypes)" : "FAILED"));
                x3::logInfo(std::string("--world echotropolis: SHOT residents ") +
                            (sResBuilt ? "built" : "FAILED"));
            }
        }
        const int kSettle = shotResidents ? 90 : 24;   // drain skin spawns
        const float dt = 1.0f / 60.0f;
        const x3::game::TodSample shotTod = tod.sample();   // frozen at ECHO_TOD
        for (int i = 0; i < kSettle; ++i) {
            glfwPollEvents();
            applyTodSample(device, shotTod);
            applyAtmosphere(device, shotTod);   // ATMOSPHERE: aerial haze + grade + bloom
            applyOcean(device, (float)i * dt, shotTod);
            if (sResBuilt) { sCrowd.update(dt, sScene); sSkin.update(dt, sCrowd, sScene, *device, *sphys); }
            if (sMinersBuilt) { sMiners.update(dt, sScene); sMinersSkin.update(dt, sMiners, sScene, *device, *sphys); }
            if (sOh1Built) { flyOh1(sOh1, (float)i * dt); sOh1.update(dt, sScene, *sphys, sOh1.pos()); }
            if (sNpcBuilt) sNpc.update(dt, sScene);   // living-city schedules advance
            if (sOpsBuilt) {
                sOps.setLines({
                    "ECHO HARBOR   //   CITY OPS", "",
                    "POPULATION        40",
                    "   WORKING          4",
                    "   AT LEISURE       7",
                    "   ABOUT TOWN      29", "",
                    std::string("TIME OF DAY     ") + x3::game::todPhaseName(shotTod.phase),
                    "POWER GRID      ONLINE",
                    "HARBOR WATCH    NOMINAL",
                    "VIGIL           MONITORING",
                });
                sOps.update(dt);
            }
            if (shotCamOverride) {
                device->setCamera(shotCam[0], shotCam[1], shotCam[2], shotCam[3], shotCam[4], opt.fovDeg);
            } else {
                applyOrbitCamera(device, rig, rig.sYaw, rig.sPitch, opt.fovDeg, opt.minCamHeight);
            }
            if (i == kSettle - 1) device->armCapture(outPath.c_str());
            auto frame = device->beginFrame();
            island.draw(*device, frame);   // the island (sky + water are device-internal)
            props.draw(*device, frame);    // P4 coast dressing (lighthouse/dock/boats/skyline)
            for (auto& h : houses) h->draw(*device, frame);   // real textured buildings (decoded)
            for (auto& t : towers) t->draw(*device, frame);   // Urban Night City downtown skyline
            for (auto& m : mineProps) m->draw(*device, frame);   // gold mine + truck lot
        for (auto& t : mineForest) t->draw(*device, frame);  // thick pines ringing the mine
            mineGlowScene.render(*device, frame);            // authentic EoS arch mouth-glow
            if (ufoBuilt) { poseUfo((float)i * dt); ufo.draw(*device, frame);
                            if (ufoFxBuilt) ufoFx.draw(*device, frame); }   // saucer + glow + beam
            for (auto& h : helis) { poseHeli(h, (float)i * dt);
                                    h.body->draw(*device, frame); h.rotor->draw(*device, frame);
                                    if (shotTod.cityLightsOn) { if(h.navR)h.navR->draw(*device,frame);
                                        if(h.navG)h.navG->draw(*device,frame); if(h.navW)h.navW->draw(*device,frame); } }
            for (auto& p : planes) { posePlane(p, (float)i * dt); p.body->draw(*device, frame);
                                    if (shotTod.cityLightsOn) { if(p.navR)p.navR->draw(*device,frame);
                                        if(p.navG)p.navG->draw(*device,frame); if(p.navW)p.navW->draw(*device,frame); } }
            for (auto& c : cars) { poseCar(c, (float)i * dt); c.body->draw(*device, frame); }  // street traffic
            for (auto& b : boats) { poseBoat(b, (float)i * dt); b.body->draw(*device, frame); }  // harbor boats
            for (auto& d : drones) { poseDrone(d, (float)i * dt); d.body->draw(*device, frame); }  // sky drones
            if (sResBuilt) { sScene.render(*device, frame); sSkin.draw(*device, frame, sScene); }
            else if (sMinersBuilt) sScene.render(*device, frame);
            if (sMinersBuilt) sMinersSkin.draw(*device, frame, sScene);
            if (sOh1Built) sOh1.drawMonster(*device, frame, sScene);   // OH1 hero heli (capture)
            if (shotTod.cityLightsOn) {    // P4 night lights: beam aimed over the bay + embers
                poseBeam(-2.13f);
                beam.draw(*device, frame);
                fissure.draw(*device, frame);
            }
            if (sResBuilt) {   // LIVING CITY HUD in populated captures (matches the live top bar)
                uint32_t hw = 0, hh = 0; device->hudSize(hw, hh);
                char bar[224];
                std::snprintf(bar, sizeof(bar),
                    "2038  MON DAY 23    07:12    POP %u    MINERS 8    GOLD 128 oz    $11748",
                    sCrowd.agentCount());
                const float bg[4] = { 0.04f, 0.06f, 0.10f, 0.82f };
                const float gold[4] = { 1.0f, 0.82f, 0.42f, 1.0f };
                device->drawHudQuad(frame, 12.0f, 12.0f, std::min((float)hw - 24.0f, 1120.0f), 34.0f, bg);
                device->drawHudText(frame, bar, 26.0f, 21.5f, 14.0f, gold);
                if (const char* pe = std::getenv("ECHO_PANEL"); pe && pe[0] == '1')  // CITY PANEL in captures
                    drawCityPanel(frame, hw, hh, 2038, "MON", 23, 7, 12,
                                  sCrowd.agentCount(), 18, 9, 11748.0, 128.0, 8, 83.6);
                if (const char* ie = std::getenv("ECHO_INSPECT"); ie && ie[0] == '1') {  // RESIDENT CARD
                    const auto& pr = x3::game::persona(x3::game::Archetype::Baker);
                    drawResidentCard(frame, hw, hh, "MARGO OKONKWO",
                                     x3::game::archetypeName(x3::game::Archetype::Baker),
                                     x3::game::npcActivityName(x3::game::NpcActivity::AtWork),
                                     pr.detailCount ? pr.detail[0] : "",
                                     pr.voiceCount ? pr.voice[0] : "", 8.0f);
                }
                if (const char* be = std::getenv("ECHO_BUILD"); be && be[0] == '1') {  // BUILD PALETTE
                    struct BD { const char* label; int cost; };
                    static const BD pal[] = { {"STEEL LOFT",600},{"STEEL FLAT",550},
                        {"COTTAGE A",400},{"COTTAGE B",400},{"COTTAGE C",450} };
                    const int nP = 5, sel = 2; const double treas = 11748.0;
                    const float pw=300.0f, pad=16.0f, rowH=26.0f, px=24.0f, py=54.0f;
                    const float ph=96.0f+nP*rowH;
                    const float bg[4]={0.03f,0.05f,0.09f,0.92f}, hstr[4]={0.10f,0.14f,0.20f,0.96f};
                    const float hcol[4]={1.0f,0.82f,0.42f,1.0f}, lab[4]={0.60f,0.70f,0.82f,1.0f};
                    const float val[4]={0.92f,0.96f,1.0f,1.0f}, selc[4]={0.14f,0.24f,0.18f,0.95f};
                    device->drawHudQuad(frame,px,py,pw,ph,bg);
                    device->drawHudQuad(frame,px,py,pw,34.0f,hstr);
                    device->drawHudText(frame,"BUILD   //   place a lot",px+pad,py+9.0f,14.0f,hcol);
                    float ty=py+46.0f;
                    for(int i=0;i<nP;++i){ if(i==sel) device->drawHudQuad(frame,px+8.0f,ty-3.0f,pw-16.0f,rowH-2.0f,selc);
                        device->drawHudText(frame,pal[i].label,px+pad,ty,13.0f,i==sel?val:lab);
                        char cz[24]; std::snprintf(cz,sizeof cz,"$%d",pal[i].cost);
                        device->drawHudText(frame,cz,px+pw-pad-(float)std::strlen(cz)*12.0f,ty,12.0f,lab); ty+=rowH; }
                    ty+=8.0f;
                    device->drawHudText(frame,"[ ] cycle   R rotate   ENTER place   BKSP undo",px+pad,ty,11.0f,lab); ty+=20.0f;
                    char tz[64]; std::snprintf(tz,sizeof tz,"TREASURY  $%0.0f     BUILT %d",treas,3);
                    device->drawHudText(frame,tz,px+pad,ty,12.0f,hcol);
                }
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

    // ===================== LIVING CITY SIM (Phase 1 HUD spine) =====================
    // The 3D world becomes a city sim: a day counter + a 24h clock advance in real
    // time, and a treasury accrues from the working residents. Surfaced in the top-bar
    // HUD each frame (drawHudQuad/drawHudText), driven by the real NpcLife counts —
    // the same "watch the living economy" hook as the web build. (Phase 2 wires the
    // echo_core conserved economy + city panels + build menu.)
    const float kSimDayLen = [](){ const char* e=std::getenv("ECHO_DAY_SECONDS");
                                   return e ? (float)std::atof(e) : 300.0f; }();  // 1 day = 5 min
    int    simDay   = 23;            // pick up near where the web save sits
    float  simClock = 0.30f;         // day fraction [0,1); 0.30 ≈ 07:12
    double treasury = 11748.0;       // seed to match the familiar figure
    const  int simYear = 2038;       // near-future setting (Tim's gold-rush era)
    double goldOz   = 0.0;           // cumulative gold mined by the miner crew
    const  int kMiners = 8;          // resident gold-mining crew (work the seam)
    static const char* kDow[7] = { "MON","TUE","WED","THU","FRI","SAT","SUN" };

    x3::logInfo("--world echotropolis: LMB/MMB-drag or WASD pan (grab-the-ground), "
                "wheel zoom, RMB-drag or Q/E orbit-rotate; TOD keys 1=golden 2=dusk "
                "3=night 4=noon, T pauses the cycle; Esc to quit");
    // Start with the day-night cycle PAUSED so the launch HOLDS its ECHO_TOD light
    // (golden by default) instead of sprinting into night in ~20s — press T to run time.
    bool todPaused = true, prevT = false;
    x3::game::TodPhase prevPhase = tod.phase();

    // ESC opens a PAUSE MENU (it never quits the app — Tim's rule 2026-07-11). Only the
    // menu's QUIT item (click, or press Q) exits. Edge-detected so a held key toggles once.
    bool menuOpen = false, prevEsc = false, prevQ = false, prevEnter = false, prevLmb = false;
    bool wantQuit = false;

    // ===================== WALK MODE (Phase A) ==========================
    // Press G to drop from the orbit vista INTO a first-person character who WALKS
    // the REAL island (WASD, mouse look, Shift sprint, Space jump) — down the
    // cliffs to the harbor and INTO the sea to swim. G returns to the orbit view.
    std::unique_ptr<x3::phys::IPhysicsWorld> phys(x3::phys::createPhysicsWorld());
    const bool physOk = phys && phys->init();
    const float kWalkX = -20.0f, kWalkZ = 760.0f;             // crown spawn (+195 m)
    const float kWalkGroundY = hf.ok() ? hf.heightAt(kWalkX, kWalkZ) : 190.0f;
    x3::game::Player player;
    if (physOk && hf.ok()) {
        // REAL TERRAIN COLLISION: a Jolt static mesh sampled from the SAME height-
        // field the island GLB was meshed from, so the player collides with the
        // actual landform — cliffs, shelves, the harbor bowl. Covers the central
        // 2600 m (the land); the ocean skirt beyond is water. 512 grid = ~5 m cells.
        const int N = 512; const float EXT = 2600.0f;
        std::vector<float> verts; verts.reserve((size_t)(N + 1) * (N + 1) * 3);
        std::vector<uint32_t> idx; idx.reserve((size_t)N * N * 6);
        for (int r = 0; r <= N; ++r)
            for (int c = 0; c <= N; ++c) {
                const float x = -EXT * 0.5f + EXT * (float)c / N;
                const float z = -EXT * 0.5f + EXT * (float)r / N;
                verts.push_back(x); verts.push_back(hf.heightAt(x, z)); verts.push_back(z);
            }
        auto vid = [&](int r, int c) { return (uint32_t)(r * (N + 1) + c); };
        for (int r = 0; r < N; ++r)
            for (int c = 0; c < N; ++c) {
                idx.push_back(vid(r, c)); idx.push_back(vid(r + 1, c)); idx.push_back(vid(r + 1, c + 1));
                idx.push_back(vid(r, c)); idx.push_back(vid(r + 1, c + 1)); idx.push_back(vid(r, c + 1));
            }
        phys->addStaticMesh(verts.data(), (uint32_t)(verts.size() / 3),
                            idx.data(), (uint32_t)idx.size());
        x3::logInfo("--world echotropolis: terrain collision mesh built (" +
                    std::to_string(idx.size() / 3) + " tris)");

        // SWIM: sea + basin surface sits at y=0; anywhere the terrain is below the
        // waterline the player can wade in and swim (Player runs its swim state off
        // this feed). Deeply-negative elsewhere = dry land (never swims on land).
        player.setWaterQuery([&hf](float x, float z) -> float {
            return (hf.ok() && hf.heightAt(x, z) < -0.30f) ? 0.0f : -1.0e30f;
        });
        player.spawn(*phys, kWalkX, kWalkGroundY + 1.0f, kWalkZ);
        player.setLook(2.2f, -0.05f);   // face toward the city cluster
    }

    // ===================== RESIDENTS (the living city) ==================
    // A crowd of citizens wandering the crown, rendered as the REAL rigged-GLB
    // characters (66 in riggedGlbRoot()) via CrowdSkin over the blockout agents.
    // Visible from the orbit vista (watch them move) AND up close in walk mode.
    // These are the lives you will step into (possess) in a later phase.
    x3::game::Scene walkScene;
    x3::game::CrowdSystem residents;
    x3::game::CrowdSkin residentsSkin;
    bool residentsBuilt = false;
    x3::game::CrowdSystem miners;          // dedicated GOLD-MINE crew (western shoulder)
    x3::game::CrowdSkin minersSkin;
    bool minersBuilt = false;
    x3::game::MonsterSystem oh1; bool oh1Built = false;   // OH1 hero heli (rigged prop)
    x3::game::HoloTerminal opsScreen;   // CONTROL ROOM: live city-ops dashboard on the crown
    x3::game::NpcLife npcLife; bool npcLifeBuilt = false;   // LIVING CITY: scheduled NPCs
    bool opsBuilt = false;
    if (physOk) {
        x3::game::CrowdConfig cc;
        cc.count   = 40;
        cc.centerX = kWalkX; cc.centerZ = kWalkZ;
        cc.groundY = kWalkGroundY;
        cc.radius  = 340.0f;             // spread across the crown plateau
        cc.walkSpeed = 1.3f;
        cc.converse  = true;             // they pair up and chat
        cc.scale     = 1.0f;
        // AUTONOMOUS LIFE: jobs + play so residents have PURPOSE, not just wander.
        // Per-workpoint a Worker loops its task (Carry/Console/Sweep); each play spot
        // gets its Gamers; the rest are Civilians who wander + converse. A basic
        // daily-life read on top of the crown until the full NpcLife schedule system
        // (feat/living-city) is rebased onto main and merged.
        cc.work = {
            { x3::game::CrowdWorkPoint::Kind::Carry,   -120.0f, 700.0f, -60.0f, 700.0f },
            { x3::game::CrowdWorkPoint::Kind::Console,    40.0f, 820.0f,  40.0f, 820.0f },
            { x3::game::CrowdWorkPoint::Kind::Sweep,      80.0f, 700.0f, 140.0f, 760.0f },
            { x3::game::CrowdWorkPoint::Kind::Carry,    -140.0f, 800.0f, -80.0f, 800.0f },
        };
        // GOLD MINERS live in their OWN dedicated crew crowd out at the western mine
        // (built just after the residents below) — NOT here — so they stand on the mine's
        // real terrain plane (kMineGy) and cluster at the seam instead of wandering town.
        cc.play = {
            { -60.0f, 840.0f, 4, true },
            {  90.0f, 690.0f, 3, true },
        };
        residents.build(cc, walkScene, *device);
        // LIVING CITY: the 12-archetype NPCs with real daily schedules (home/work/leisure)
        // + hackable scan-cards — the "real lives" layer over the crowd.
        {
            x3::game::NpcLifeConfig lc;
            lc.centerX = kWalkX; lc.centerZ = kWalkZ;
            lc.groundY = kWalkGroundY;
            lc.freewayMovers = 0;
            npcLife.build(lc, walkScene, *device);
            npcLifeBuilt = npcLife.built();
            x3::logInfo(std::string("--world echotropolis: living-city NpcLife ") +
                        (npcLifeBuilt ? "built" : "off"));
        }
        x3::game::CrowdSkinConfig sc;
        sc.site = "Echo Harbor residents";
        sc.modelDir = x3::game::riggedGlbRoot();
        sc.spawnsPerFrame = 2;
        sc.scale = kPedScale;   // 5-6 ft citizens
        residentsSkin.build(sc, residents);
        residentsBuilt = residents.built();
        x3::logInfo(std::string("--world echotropolis: residents ") +
                    (residentsBuilt ? "built (40 citizens, rigged skins loading)" : "FAILED"));

        // GOLD-MINE CREW: a dedicated crowd out at the western mine. Miners trek the
        // truck-lot -> seam Carry route on the mine's OWN terrain plane (kMineGy) and
        // cluster in a tight radius so they read as a work gang, not city wanderers.
        {
            x3::game::CrowdConfig mc;
            mc.count   = 9;
            mc.centerX = kMineX; mc.centerZ = kMineZ;
            mc.groundY = kMineGy;
            mc.radius  = 26.0f;              // clustered at the pit
            mc.walkSpeed = 1.15f;
            mc.converse  = false;           // heads-down hauling, not chatting
            mc.scale     = 1.0f;
            using MK = x3::game::CrowdWorkPoint;
            mc.work = {
                { MK::Kind::Carry, kLotX,        kLotZ,        kMineX,        kMineZ        },
                { MK::Kind::Carry, kLotX - 6.0f, kLotZ + 4.0f, kMineX + 6.0f, kMineZ - 4.0f },
                { MK::Kind::Carry, kLotX + 5.0f, kLotZ - 5.0f, kMineX - 5.0f, kMineZ + 5.0f },
                { MK::Kind::Carry, kLotX - 3.0f, kLotZ - 3.0f, kMineX + 3.0f, kMineZ + 6.0f },
                { MK::Kind::Carry, kLotX + 8.0f, kLotZ + 6.0f, kMineX - 7.0f, kMineZ - 2.0f },
                { MK::Kind::Carry, kLotX + 2.0f, kLotZ + 9.0f, kMineX - 2.0f, kMineZ - 6.0f },
            };
            miners.build(mc, walkScene, *device);
            x3::game::CrowdSkinConfig msc;
            msc.site = "Echo Harbor miners";
            msc.modelDir = x3::game::riggedGlbRoot();
            msc.spawnsPerFrame = 2;
            msc.scale = kPedScale;          // 5-6 ft miners
            minersSkin.build(msc, miners);
            minersBuilt = miners.built();
            x3::logInfo(std::string("--world echotropolis: GOLD-MINE crew ") +
                        (minersBuilt ? "built (9 miners hauling ore)" : "FAILED"));
        }

        // OH1 HERO HELI: the rigged bird, flown as an inert MonsterSystem prop.
        oh1Built = buildOh1(oh1, walkScene, *phys);
        x3::logInfo(std::string("--world echotropolis: OH1 hero heli ") +
                    (oh1Built ? (oh1.skinnable()
                        ? (oh1.calmLoopActive() ? "built (rigged, rotor spinning)"
                                                : "built (rigged, NO rotor clip — check name)")
                        : "built (static, unrigged)") : "FAILED to load"));

        // CONTROL ROOM: a live ops-dashboard screen on the crown plaza (HoloTerminal
        // Readout, Vigil-orange ink). Renders through walkScene like the residents.
        opsScreen.build(walkScene, *device,
                        x3::phys::Vec3{ kWalkX + 6.0f, kWalkGroundY + 2.2f, kWalkZ - 8.0f },
                        0.0f, 6.4f, 3.6f);
        opsScreen.setLayout(x3::game::HoloTerminal::Layout::Readout);
        opsScreen.setTextColor(1.0f, 0.62f, 0.18f, 1.0f);   // VIGIL orange
        opsBuilt = opsScreen.built();
    }
    // Live ops-dashboard lines from the current city state (per-role crowd split +
    // time of day). The deep economy KPIs arrive when echo_core is wired in.
    auto opsLines = [&](const x3::game::TodSample& s) {
        const int pop = 40, workers = 4, gamers = 7;
        return std::vector<std::string>{
            "ECHO HARBOR   //   CITY OPS",
            "",
            "POPULATION        " + std::to_string(pop),
            "   WORKING         " + std::to_string(workers),
            "   AT LEISURE      " + std::to_string(gamers),
            "   ABOUT TOWN      " + std::to_string(pop - workers - gamers),
            "",
            std::string("TIME OF DAY     ") + x3::game::todPhaseName(s.phase),
            "POWER GRID      ONLINE",
            "HARBOR WATCH    NOMINAL",
            "VIGIL           MONITORING",
        };
    };
    bool walkMode = false, prevG = false;
    bool cityPanelOpen = false, prevTab = false;   // CITY PANEL (TAB toggles)
    int  followIdx = -1, lastPickedIdx = -1; bool prevF = false;   // RIDE-ALONG (F)

    // ===================== BUILD MENU (B) =====================
    // Place real textured buildings on the island at the orbit cursor (screen-centre
    // ground point). Each placed lot = its own cached EnvArtSystem, drawn like houses.
    // Cost is drawn from the live treasury; Backspace refunds the last placement.
    struct BuildDef { const char* label; const char* dir; const char* glb;
                      float scale; float lift; int cost; };
    static const BuildDef kBuild[] = {
        { "STEEL LOFT", "D:/Assets/_glb/prefab_buildings/HouseForge", "PF_MetalHouse01.glb",     0.01f, 11.73f, 600 },
        { "STEEL FLAT", "D:/Assets/_glb/prefab_buildings/HouseForge", "PF_MetalHouse02.glb",     0.01f,  2.13f, 550 },
        { "COTTAGE A",  "D:/Assets/_glb/prefab_buildings/HouseForge", "PF_PrimitiveHouse01.glb", 0.01f,  3.17f, 400 },
        { "COTTAGE B",  "D:/Assets/_glb/prefab_buildings/HouseForge", "PF_PrimitiveHouse02.glb", 0.01f,  1.00f, 400 },
        { "COTTAGE C",  "D:/Assets/_glb/prefab_buildings/HouseForge", "PF_PrimitiveHouse03.glb", 0.01f,  8.59f, 450 },
    };
    const int kBuildCount = (int)(sizeof(kBuild) / sizeof(kBuild[0]));
    std::vector<std::unique_ptr<x3::game::EnvArtSystem>> placed;   // player-built lots
    std::vector<int> placedCost;                                   // for Backspace refund
    std::vector<std::unique_ptr<x3::game::EnvArtSystem>> buildPreview(kBuildCount);  // ghosts, lazy
    bool buildMode = false, prevB = false, prevLB = false, prevRB = false,
         prevRk = false, prevBk = false, prevPlace = false;
    int  buildSel = 0; float buildYaw = 0.0f;
    // HouseForge/mine transform (col-major): yaw about Y, uniform scale, terrain-lift.
    auto buildXf = [&](float x, float z, float yaw, float s, float lift, float T[16]){
        const float gy = hf.ok() ? hf.heightAt(x, z) : 190.0f;
        const float c = std::cos(yaw), sn = std::sin(yaw);
        T[0]=c*s; T[1]=0; T[2]=-sn*s; T[3]=0;  T[4]=0; T[5]=s; T[6]=0; T[7]=0;
        T[8]=sn*s; T[9]=0; T[10]=c*s; T[11]=0; T[12]=x; T[13]=gy+lift; T[14]=z; T[15]=1;
    };

    int lastW = (int)hc.W, lastH = (int)hc.H;
    while (!glfwWindowShouldClose(window) && !wantQuit) {
        glfwPollEvents();
        {
            const bool esc = glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS;
            if (esc && !prevEsc) menuOpen = !menuOpen;   // toggle, never break
            prevEsc = esc;
        }

        double now = glfwGetTime();
        float dt = (float)(now - prevTime); prevTime = now;
        if (dt > 0.1f) dt = 0.1f;
        if (dt < 1e-5f) dt = 1e-5f;

        double mx, my; glfwGetCursorPos(window, &mx, &my);
        float ddx = (float)(mx - lastMX), ddy = (float)(my - lastMY);
        lastMX = mx; lastMY = my;

        auto kd  = [&](int k) { return glfwGetKey(window, k) == GLFW_PRESS; };
        auto mbd = [&](int b) { return glfwGetMouseButton(window, b) == GLFW_PRESS; };

        // ---- WALK MODE toggle (G) + first-person character step -------------
        { const bool g = kd(GLFW_KEY_G); if (g && !prevG && physOk) walkMode = !walkMode; prevG = g; }
        { const bool tb = kd(GLFW_KEY_TAB); if (tb && !prevTab) cityPanelOpen = !cityPanelOpen; prevTab = tb; }
        // BUILD MODE (B): orbit-only; entering walk mode drops it.
        { const bool b = kd(GLFW_KEY_B); if (b && !prevB && !walkMode) buildMode = !buildMode; prevB = b; }
        if (walkMode) buildMode = false;
        // RIDE-ALONG (F): possess the camera onto the citizen you're inspecting; F again releases.
        { const bool f = kd(GLFW_KEY_F);
          if (f && !prevF) {
              if (followIdx >= 0) followIdx = -1;
              else if (walkMode && npcLifeBuilt && lastPickedIdx >= 0) followIdx = lastPickedIdx;
          }
          prevF = f; }
        if (walkMode && physOk && followIdx < 0) {   // frozen while riding along
            x3::game::PlayerInput in{};
            in.moveFwd    = (kd(GLFW_KEY_W) ? 1.0f : 0.0f) - (kd(GLFW_KEY_S) ? 1.0f : 0.0f);
            in.moveStrafe = (kd(GLFW_KEY_D) ? 1.0f : 0.0f) - (kd(GLFW_KEY_A) ? 1.0f : 0.0f);
            in.sprint      = kd(GLFW_KEY_LEFT_SHIFT);
            in.jumpPressed = kd(GLFW_KEY_SPACE);
            in.jumpHeld    = kd(GLFW_KEY_SPACE);              // swim: stroke up
            in.diveHeld    = kd(GLFW_KEY_LEFT_CONTROL) || kd(GLFW_KEY_C);  // swim: dive
            in.lookDX = ddx; in.lookDY = ddy;
            player.update(in, dt, *phys);
            phys->step(dt);
        }

        // ===== PAUSE MENU: world + camera frozen, menu drawn, only QUIT exits =====
        if (menuOpen) {
            uint32_t hw = 0, hh = 0; device->hudSize(hw, hh);
            const float cx = hw * 0.5f, cy = hh * 0.5f;
            // Quit button rect (centered). Q key or a click inside it quits.
            const float bw = 260.0f, bh = 56.0f;
            const float bx = cx - bw * 0.5f, by = cy + 40.0f;
            const bool overQuit = (mx >= bx && mx <= bx + bw && my >= by && my <= by + bh);
            const bool lmb = mbd(GLFW_MOUSE_BUTTON_LEFT);
            const bool qkey = kd(GLFW_KEY_Q);
            if ((lmb && !prevLmb && overQuit) || (qkey && !prevQ)) wantQuit = true;
            const bool ent = kd(GLFW_KEY_ENTER);
            if (ent && !prevEnter) menuOpen = false;   // Enter also resumes
            prevLmb = lmb; prevQ = qkey; prevEnter = ent;

            auto frame = device->beginFrame();
            island.draw(*device, frame);
            props.draw(*device, frame);
            if (tod.sample().cityLightsOn) { beam.draw(*device, frame); fissure.draw(*device, frame); }
            const float dim[4]   = { 0.02f, 0.03f, 0.05f, 0.66f };
            device->drawHudQuad(frame, 0, 0, (float)hw, (float)hh, dim);
            const float gold[4]  = { 1.0f, 0.82f, 0.45f, 1.0f };
            const float white[4] = { 0.90f, 0.93f, 1.0f, 1.0f };
            const float dimtxt[4]= { 0.62f, 0.66f, 0.76f, 1.0f };
            device->drawHudText(frame, "ECHO  HARBOR", cx - 11 * 19.0f, cy - 150.0f, 38.0f, gold);
            device->drawHudText(frame, "PAUSED", cx - 6 * 11.0f, cy - 96.0f, 22.0f, dimtxt);
            const float qcol[4] = { overQuit ? 0.35f : 0.18f, overQuit ? 0.10f : 0.06f,
                                    overQuit ? 0.12f : 0.07f, 1.0f };
            device->drawHudQuad(frame, bx, by, bw, bh, qcol);
            device->drawHudText(frame, "QUIT  TO  DESKTOP", bx + 22.0f, by + 18.0f, 17.0f, white);
            device->drawHudText(frame,
                "ESC / ENTER  resume        1-4 time of day    5-8 camera views\n"
                "WASD / drag  pan           wheel  zoom         Q  quit         T  run clock",
                cx - 34 * 8.5f, cy + 120.0f, 15.0f, dimtxt);
            device->endFrame(frame);

            fpsAccum += dt; ++fpsFrames;
            if (fpsAccum >= 1.0) { fpsAccum = 0.0; fpsFrames = 0; }
            continue;   // skip all world sim while paused in the menu
        }

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

        // ---- Momentum EDGE-SCROLL pan (EoS "adopt now"): cursor inside a 12px band
        // at any window edge pushes the focus that way. Velocity eases toward the
        // radius-scaled target with tau=panSmoothTau (accel-in + glide-to-stop);
        // diagonals normalized. DISABLED while any mouse button is held (velocity
        // hard-zeroed) so it never fights drag-pan/orbit; gated to cursor-in-window. ----
        {
            const bool anyBtn = rmb || mmb;   // mmb already folds in LMB || MMB
            float exd = 0.0f, ezd = 0.0f;     // screen push: +x=right, +z=forward(top)
            const bool inWin = (mx >= 0.0 && my >= 0.0 &&
                                mx <= (double)lastW && my <= (double)lastH);
            if (!anyBtn && inWin) {
                const float m = opt.edgeMarginPx;
                if (mx < m)               exd -= 1.0f;   // left  edge -> pan -right
                if (mx > (double)lastW-m) exd += 1.0f;   // right edge -> pan +right
                if (my < m)               ezd += 1.0f;   // top   edge -> pan +forward
                if (my > (double)lastH-m) ezd -= 1.0f;   // bottom edge-> pan -forward
            }
            float tvx = 0.0f, tvz = 0.0f;                // target world velocity (m/s)
            if (exd != 0.0f || ezd != 0.0f) {
                const float inv   = 1.0f / std::sqrt(exd*exd + ezd*ezd);
                const float speed = opt.edgeScrollFrac * rig.sRadius;
                tvx = (exd * rightX + ezd * fwdSeaX) * inv * speed;
                tvz = (exd * rightZ + ezd * fwdSeaZ) * inv * speed;
            }
            if (anyBtn) {                 // never fight drag-pan: kill momentum outright
                rig.vEdgeX = 0.0f; rig.vEdgeZ = 0.0f;
            } else {
                const float k = 1.0f - std::exp(-dt / std::max(opt.panSmoothTau, 1e-4f));
                rig.vEdgeX += (tvx - rig.vEdgeX) * k;
                rig.vEdgeZ += (tvz - rig.vEdgeZ) * k;
                if (tvx == 0.0f && std::fabs(rig.vEdgeX) < 0.5f) rig.vEdgeX = 0.0f;
                if (tvz == 0.0f && std::fabs(rig.vEdgeZ) < 0.5f) rig.vEdgeZ = 0.0f;
                rig.focusX += rig.vEdgeX * dt;
                rig.focusZ += rig.vEdgeZ * dt;
            }
        }

        // Clamp targets to the playable envelope (island bounds + 1km; pitch 2..85°).
        rig.focusX = clampf(rig.focusX, -opt.focusLimit, opt.focusLimit);
        rig.focusZ = clampf(rig.focusZ, -opt.focusLimit, opt.focusLimit);
        rig.pitch  = clampPitch(rig.pitch);
        // Pivot rides the terrain height under the focus (0 over water) so the point
        // under the crosshair stays fixed while rotating/zooming.
        rig.pivotY = std::max(hf.heightAt(rig.focusX, rig.focusZ), 0.0f);

        // ---- BUILD MENU interaction: cursor = orbit focus on the ground ----
        if (buildMode) {
            { const bool lb = kd(GLFW_KEY_LEFT_BRACKET);
              if (lb && !prevLB) buildSel = (buildSel + kBuildCount - 1) % kBuildCount; prevLB = lb; }
            { const bool rb = kd(GLFW_KEY_RIGHT_BRACKET);
              if (rb && !prevRB) buildSel = (buildSel + 1) % kBuildCount; prevRB = rb; }
            { const bool r = kd(GLFW_KEY_R);
              if (r && !prevRk) buildYaw += 0.7853982f; prevRk = r; }   // +45 deg
            // PLACE (Enter): drop a permanent lot at the cursor if affordable.
            { const bool pl = kd(GLFW_KEY_ENTER);
              if (pl && !prevPlace) {
                  const BuildDef& d = kBuild[buildSel];
                  if (treasury >= (double)d.cost) {
                      float T[16]; buildXf(rig.focusX, rig.focusZ, buildYaw, d.scale, d.lift, T);
                      auto e = std::make_unique<x3::game::EnvArtSystem>();
                      if (e->buildFromGlbAt(*device, d.dir, d.glb, T)) {
                          placed.push_back(std::move(e)); placedCost.push_back(d.cost);
                          treasury -= (double)d.cost;
                      }
                  }
              }
              prevPlace = pl; }
            // UNDO (Backspace): remove + refund the last placed lot.
            { const bool bk = kd(GLFW_KEY_BACKSPACE);
              if (bk && !prevBk && !placed.empty()) {
                  treasury += (double)placedCost.back();
                  placed.pop_back(); placedCost.pop_back();
              }
              prevBk = bk; }
        }

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
            // ---- P5 camera bookmarks: the four signature shots (keys 5-8). Set
            // the orbit TARGETS; the critically-damped springs fly there smoothly.
            auto bookmark = [&](float fx, float fz, float yw, float pt, float rd) {
                rig.focusX = fx; rig.focusZ = fz;
                rig.yaw = yw; rig.pitch = clampPitch(pt); rig.radius = rd;
            };
            if (kd(GLFW_KEY_5)) bookmark(-450.0f,  900.0f, -1.02f, 0.31f, 1400.0f); // THE POSTCARD
            if (kd(GLFW_KEY_6)) bookmark( 412.0f, -106.0f,  3.93f, 0.60f,  950.0f); // crown promenade
            if (kd(GLFW_KEY_7)) bookmark( 331.0f,  459.0f,  2.40f, 0.35f,  750.0f); // fissure rim
            if (kd(GLFW_KEY_8)) bookmark(   0.0f,    0.0f,  5.50f, 0.10f, 4400.0f); // sea approach
            if (!todPaused) tod.advance(dt);
            if (tod.phase() != prevPhase) {
                prevPhase = tod.phase();
                x3::logInfo(std::string("--world echotropolis: ") +
                            x3::game::todPhaseName(prevPhase));
            }
        }
        const x3::game::TodSample todS = tod.sample();
        applyTodSample(device, todS);
        applyAtmosphere(device, todS);   // ATMOSPHERE: aerial haze + grade + bloom

        // RESIDENTS: the crowd lives every frame (orbit or walk); the skinned layer
        // drains its deferred spawn queue and pose-follows the agents.
        if (residentsBuilt) {
            residents.update(dt, walkScene);
            if (npcLifeBuilt) npcLife.update(dt, walkScene);   // living-city schedules advance
            residentsSkin.update(dt, residents, walkScene, *device, *phys);
        }
        if (minersBuilt) {                 // GOLD-MINE crew lives + hauls every frame
            miners.update(dt, walkScene);
            minersSkin.update(dt, miners, walkScene, *device, *phys);
        }
        if (oh1Built) { flyOh1(oh1, waterTime); oh1.update(dt, walkScene, *phys, oh1.pos()); }
        if (opsBuilt) {                 // CONTROL ROOM: refresh the live dashboard
            opsScreen.setLines(opsLines(todS));
            opsScreen.update(dt);
        }

        // Ocean + camera + render. WALK MODE poses the first-person eye camera from
        // the physics character; ORBIT MODE keeps the strategic vista camera.
        waterTime += dt; applyOcean(device, waterTime, todS);
        if (followIdx >= 0 && npcLifeBuilt && (uint32_t)followIdx < npcLife.agentCount()) {
            // RIDE-ALONG: trail the citizen third-person, looking at their head as they
            // walk their scheduled day. Camera sits back+up along their facing.
            const auto& a = npcLife.agent((uint32_t)followIdx);
            const float back = 4.6f, up = 2.7f, headY = 1.5f;
            const float camx = a.pos.x - std::cos(a.yaw) * back;
            const float camz = a.pos.z - std::sin(a.yaw) * back;
            const float camy = a.pos.y + up;
            const float dx = a.pos.x - camx, dyy = (a.pos.y + headY) - camy, dz = a.pos.z - camz;
            const float yaw = std::atan2(dz, dx);
            const float pitch = std::atan2(dyy, std::sqrt(dx*dx + dz*dz));
            device->setCamera(camx, camy, camz, yaw, pitch, opt.fovDeg);
        } else if (walkMode && physOk) {
            float px, py, pz, pyaw, ppit;
            player.camera(px, py, pz, pyaw, ppit);
            device->setCamera(px, py, pz, pyaw, ppit, opt.fovDeg);
        } else {
            applyOrbitCamera(device, rig, rig.sYaw, rig.sPitch, opt.fovDeg, opt.minCamHeight);
        }

        auto frame = device->beginFrame();
        island.draw(*device, frame);
        props.draw(*device, frame);    // P4 coast dressing (lighthouse/dock/boats/skyline)
        for (auto& h : houses) h->draw(*device, frame);   // real textured buildings (decoded)
        for (auto& t : towers) t->draw(*device, frame);   // Urban Night City downtown skyline
        for (auto& m : mineProps) m->draw(*device, frame);   // gold mine + truck lot
        for (auto& t : mineForest) t->draw(*device, frame);  // thick pines ringing the mine
        for (auto& p : placed) p->draw(*device, frame);      // BUILD MENU: player-placed lots
        mineGlowScene.render(*device, frame);                // authentic EoS arch mouth-glow
        // BUILD GHOST: the selected building previewed at the cursor (lazily loaded).
        if (buildMode) {
            if (!buildPreview[buildSel]) {
                const BuildDef& d = kBuild[buildSel];
                float T[16]; buildXf(rig.focusX, rig.focusZ, buildYaw, d.scale, d.lift, T);
                auto e = std::make_unique<x3::game::EnvArtSystem>();
                if (e->buildFromGlbAt(*device, d.dir, d.glb, T)) buildPreview[buildSel] = std::move(e);
            }
            if (buildPreview[buildSel]) {
                const BuildDef& d = kBuild[buildSel];
                float T[16]; buildXf(rig.focusX, rig.focusZ, buildYaw, d.scale, d.lift, T);
                buildPreview[buildSel]->setInstanceTransform(0, T);
                buildPreview[buildSel]->draw(*device, frame);
            }
        }
        if (ufoBuilt) { poseUfo(waterTime); ufo.draw(*device, frame);
                        if (ufoFxBuilt) ufoFx.draw(*device, frame); }   // saucer + glow + beam
        for (auto& h : helis) { poseHeli(h, waterTime);
                                h.body->draw(*device, frame); h.rotor->draw(*device, frame);
                                if (todS.cityLightsOn) { if(h.navR)h.navR->draw(*device,frame);
                                    if(h.navG)h.navG->draw(*device,frame); if(h.navW)h.navW->draw(*device,frame); } }
        for (auto& p : planes) { posePlane(p, waterTime); p.body->draw(*device, frame);
                                if (todS.cityLightsOn) { if(p.navR)p.navR->draw(*device,frame);
                                    if(p.navG)p.navG->draw(*device,frame); if(p.navW)p.navW->draw(*device,frame); } }
        for (auto& c : cars) { poseCar(c, waterTime); c.body->draw(*device, frame); }  // street traffic
        for (auto& b : boats) { poseBoat(b, waterTime); b.body->draw(*device, frame); }  // harbor boats
        for (auto& d : drones) { poseDrone(d, waterTime); d.body->draw(*device, frame); }  // sky drones
        if (oh1Built) oh1.drawMonster(*device, frame, walkScene);   // OH1 hero heli
        if (residentsBuilt) {          // the citizens (blockout agents + rigged skins)
            walkScene.render(*device, frame);
            residentsSkin.draw(*device, frame, walkScene);
        } else if (minersBuilt) {      // miners share walkScene; render it if residents didn't
            walkScene.render(*device, frame);
        }
        if (minersBuilt) minersSkin.draw(*device, frame, walkScene);   // GOLD-MINE crew skins
        if (todS.cityLightsOn) {       // P4 night lights: sweeping beam + fissure embers
            poseBeam(waterTime * kBeamRate);
            beam.draw(*device, frame);
            fissure.draw(*device, frame);
        }

        // ===================== LIVING CITY HUD (top bar) =====================
        // Advance the day clock + accrue the treasury from working residents, then
        // draw the web-style readout: MON · DAY N · HH:MM · residents · employed ·
        // treasury. Population/employment come from the real NpcLife schedules.
        {
            simClock += dt / kSimDayLen;
            while (simClock >= 1.0f) { simClock -= 1.0f; ++simDay; }
            uint32_t pop = 0, atWork = 0;
            if (npcLifeBuilt) {
                pop = npcLife.agentCount();
                atWork = npcLife.countActivity(x3::game::NpcActivity::AtWork)
                       + npcLife.countActivity(x3::game::NpcActivity::ToWork);
            } else if (residentsBuilt) {
                pop = residents.agentCount();
            }
            // Gold rush: up to kMiners of the on-shift crew work the seam — each mines
            // ~6 oz/day, sold into the treasury alongside ordinary wages (minus upkeep).
            const double dayStep = (double)(dt / kSimDayLen);
            const uint32_t miners = std::min<uint32_t>((uint32_t)kMiners, atWork);
            goldOz   += miners * 6.0 * dayStep;
            treasury += (atWork * 3.2 - pop * 0.35 + miners * 5.0) * dayStep;

            uint32_t hw = 0, hh = 0; device->hudSize(hw, hh);
            const int hour = (int)(simClock * 24.0f) % 24;
            const int minute = (int)(simClock * 24.0f * 60.0f) % 60;
            char bar[224];
            std::snprintf(bar, sizeof(bar),
                "%d  %s DAY %d    %02d:%02d    POP %u    MINERS %u    GOLD %0.0f oz    $%0.0f",
                simYear, kDow[(simDay - 1) % 7], simDay, hour, minute, pop, miners, goldOz, treasury);
            const float pad = 14.0f, glyph = 14.0f, barH = 34.0f;
            const float barW = std::min((float)hw - 24.0f, 1120.0f);
            const float bg[4]   = { 0.04f, 0.06f, 0.10f, 0.82f };
            const float gold[4] = { 1.0f, 0.82f, 0.42f, 1.0f };
            device->drawHudQuad(frame, 12.0f, 12.0f, barW, barH, bg);
            device->drawHudText(frame, bar, 12.0f + pad, 12.0f + (barH - glyph) * 0.5f, glyph, gold);

            // CITY PANEL (TAB): the web-parity dashboard, fed by the live counts.
            if (cityPanelOpen) {
                uint32_t leisure = 0;
                if (npcLifeBuilt)
                    leisure = npcLife.countActivity(x3::game::NpcActivity::AtLeisure)
                            + npcLife.countActivity(x3::game::NpcActivity::ToLeisure);
                const double net = (double)atWork * 3.2 - (double)pop * 0.35 + (double)miners * 5.0;
                drawCityPanel(frame, hw, hh, simYear, kDow[(simDay - 1) % 7], simDay, hour, minute,
                              pop, atWork, leisure, treasury, goldOz, miners, net);
            }

            // RESIDENT INSPECTOR / RIDE-ALONG card, fed from the live NpcAgent.
            auto cardFor = [&](uint32_t i, float dist){
                const auto& a = npcLife.agent(i);
                const auto& pr = x3::game::persona(a.arch);
                const char* det = (a.detailIdx >= 0 && (uint32_t)a.detailIdx < pr.detailCount)
                                ? pr.detail[a.detailIdx] : (pr.detailCount ? pr.detail[0] : "");
                const char* voc = pr.voiceCount ? pr.voice[a.seed % pr.voiceCount] : "";
                drawResidentCard(frame, hw, hh, a.name.c_str(),
                                 x3::game::archetypeName(a.arch),
                                 x3::game::npcActivityName(a.activity), det, voc, dist);
            };
            if (followIdx >= 0 && npcLifeBuilt && (uint32_t)followIdx < npcLife.agentCount()) {
                // Riding along: show whom you're following + a release banner.
                const float ban[4] = { 0.10f, 0.14f, 0.20f, 0.92f };
                const float gold[4] = { 1.0f, 0.82f, 0.42f, 1.0f };
                device->drawHudQuad(frame, hw*0.5f - 150.0f, 54.0f, 300.0f, 30.0f, ban);
                device->drawHudText(frame, "RIDE-ALONG   //   F to release",
                                    hw*0.5f - 150.0f + 20.0f, 61.0f, 13.0f, gold);
                cardFor((uint32_t)followIdx, 0.0f);
            } else if (walkMode && physOk && npcLifeBuilt) {
                // The citizen nearest the crosshair gets the life card — the web
                // "click a resident" hook.
                float ex, ey, ez, eyaw, epit; player.camera(ex, ey, ez, eyaw, epit);
                const float fX = std::cos(eyaw), fZ = std::sin(eyaw);
                int best = -1; float bestDot = 0.94f, bestDist = 0.0f;   // ~20deg cone, 45m reach
                const uint32_t n = npcLife.agentCount();
                for (uint32_t i = 0; i < n; ++i) {
                    const auto& a = npcLife.agent(i);
                    const float dx = a.pos.x - ex, dz = a.pos.z - ez;
                    const float d2 = dx*dx + dz*dz;
                    if (d2 < 1.0f || d2 > 45.0f*45.0f) continue;
                    const float inv = 1.0f / std::sqrt(d2);
                    const float dot = (dx*fX + dz*fZ) * inv;
                    if (dot > bestDot) { bestDot = dot; best = (int)i; bestDist = std::sqrt(d2); }
                }
                lastPickedIdx = best;
                const float rc[4] = { 1.0f, 0.82f, 0.42f, best >= 0 ? 0.95f : 0.45f };
                device->drawHudQuad(frame, hw*0.5f - 7.0f, hh*0.5f - 1.0f, 14.0f, 2.0f, rc);
                device->drawHudQuad(frame, hw*0.5f - 1.0f, hh*0.5f - 7.0f, 2.0f, 14.0f, rc);
                if (best >= 0) {
                    cardFor((uint32_t)best, bestDist);
                    device->drawHudText(frame, "F  ride along",
                                        24.0f + 18.0f, (float)hh - 176.0f - 22.0f, 12.0f, rc);
                }
            } else {
                lastPickedIdx = -1;
            }

            // BUILD MENU palette (left panel) — the web "place a building" tool.
            if (buildMode) {
                const float pw = 300.0f, pad = 16.0f, rowH = 26.0f, px = 24.0f, py = 54.0f;
                const float ph = 96.0f + kBuildCount * rowH;
                const float bg[4]  = { 0.03f, 0.05f, 0.09f, 0.92f };
                const float hstr[4]= { 0.10f, 0.14f, 0.20f, 0.96f };
                const float hdr[4] = { 1.0f, 0.82f, 0.42f, 1.0f };
                const float lab[4] = { 0.60f, 0.70f, 0.82f, 1.0f };
                const float val[4] = { 0.92f, 0.96f, 1.0f, 1.0f };
                const float selc[4]= { 0.14f, 0.24f, 0.18f, 0.95f };
                const float red[4] = { 0.95f, 0.45f, 0.35f, 1.0f };
                device->drawHudQuad(frame, px, py, pw, ph, bg);
                device->drawHudQuad(frame, px, py, pw, 34.0f, hstr);
                device->drawHudText(frame, "BUILD   //   place a lot", px + pad, py + 9.0f, 14.0f, hdr);
                float ty = py + 46.0f;
                for (int i = 0; i < kBuildCount; ++i) {
                    const BuildDef& d = kBuild[i];
                    if (i == buildSel) device->drawHudQuad(frame, px + 8.0f, ty - 3.0f, pw - 16.0f, rowH - 2.0f, selc);
                    device->drawHudText(frame, d.label, px + pad, ty, 13.0f, i == buildSel ? val : lab);
                    char cz[24]; std::snprintf(cz, sizeof cz, "$%d", d.cost);
                    device->drawHudText(frame, cz, px + pw - pad - (float)std::strlen(cz) * 12.0f, ty, 12.0f,
                                        treasury >= (double)d.cost ? lab : red);
                    ty += rowH;
                }
                ty += 8.0f;
                device->drawHudText(frame, "[ ] cycle   R rotate   ENTER place   BKSP undo",
                                    px + pad, ty, 11.0f, lab); ty += 20.0f;
                char tz[64];
                std::snprintf(tz, sizeof tz, "TREASURY  $%0.0f     BUILT %d", treasury, (int)placed.size());
                device->drawHudText(frame, tz, px + pad, ty, 12.0f, hdr);
            }
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
