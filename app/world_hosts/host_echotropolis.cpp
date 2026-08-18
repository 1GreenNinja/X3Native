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
#include "echo_heightfield.h"         // TIER-2: shared island terrain sampler (hoisted from this file)
#include "echo_sea.h"                 // THE sea datum — kEchoSeaLevelY and everything derived from it
#include "echo_water.h"               // Gerstner swell presets + echoShipPose (LIVING BAY)
#include "echo_roads.h"               // ROADS: curved banked freeway + boulevard + fanned harbor grids
#include "../world_cars.h"           // CARS PILLAR: findable, drivable, hackable vehicles
#include "echo_interiors.h"           // INTERIORS PILLAR: gated cells + vendor stalls
#include "../terrain.h"               // kWorldWaterDry sentinel (water query)
#include "echo_regions.h"             // TIER-2: EchoRegion containers + WorldStreamer bridge (WP-1)
#include "echo_region_builders.h"     // TIER-2: crown/mine/district/harbor builders (WP-2)
#include "echo_woodlands.h"           // TIER-2: 9-cell woodlands + slice self-test (WP-3)
#include "../world_stream.h"          // TIER-2: the residency engine (graph + streamer)
#include "../tod.h"
#include "../env_art.h"
#include "../mine_fx.h"               // GOLD MINE: authentic EoS arch mouth-glow (ported render FX)
#include "../player.h"                // WALK MODE (Phase A): first-person character on the streets
#include "../scene.h"                 // RESIDENTS: crowd entities live in a Scene
#include "../crowd.h"                 // RESIDENTS: wandering citizen agents
#include "../monster.h"              // OH1 HERO HELI: rigged skinned draw via MonsterSystem prop
#include "../crowd_skin.h"            // RESIDENTS: real rigged-GLB characters over the agents
#include "../npc_life.h"              // LIVING CITY: 12-archetype NPCs with daily schedules
#include "../npc_skin.h"              // LIVING CITY: rigged skins over the named citizens
#include "../asset_root.h"            // riggedGlbRoot()
#include "../holo_terminal.h"         // CONTROL ROOM: in-world ops dashboard screen
#include "engine/audio/IAudioSystem.h" // CYBERPUNK AUDIO: rainy-city bed + positional hums + UI SFX
#include "../audio_root.h"           // LIVING-CITY AUDIO: repo-local curated WAVs (works on a fresh clone)
#include "engine/llm/ILlmSystem.h"     // CITIZEN TALK: local LLM conversations with residents
#include "../street_lights.h"          // DISTRICT NIGHT: warm lamp rows through the pack districts
#include "../hackables.h"              // WD2 STACK: city-wide hackables (H = nethack vision, E = hack)
#include "../alert.h"                  // WD2 STACK: every hack raises HEAT (AlertSystem)
#include "../timeline.h"               // WD2 STACK: karma — hacking the vulnerable costs you
#include "../skilltree.h"              // WD2 STACK: the dormant skill stack, live (K screen)
#include "../progression.h"            // WD2 STACK: XP from hacks / drives / DODOGs
#include "../rpg_ui.h"                 // WD2 STACK: skills screen + LV/XP HUD chip
#include "../vehparts.h"               // NFS LAYER: the parts catalog + the build
#include "../perfshop.h"               // NFS LAYER: the dormant performance shop, live
#include "../hud.h"                    // ENGINE CONSOLE: D6 IConsole + Hud drop-down front-end
#include "../engine_console.h"         // MULTI-INSTANCE LANE: r_presentmode / r_bgfps + paceFrame
#include "engine/core/IConsole.h"
#include "engine/core/x3_cpuzones.h"  // LANE 6 REPLAY: host frame-span CPU zones (splits cpu.host_outside)
#include <string>

#include <stb_image.h>   // stbi_load_16_from_memory (impl compiled in engine ModelLoader.cpp)
#include <cstdio>        // sscanf (district .layout parsing)
#include <cstdlib>       // getenv (ECHO_* switches)
#include <cctype>        // tolower (persona prompt)
#include <sstream>       // console command parsing
#include <thread>        // r_maxfps frame limiter (sleep_for)
#include <chrono>
#include <algorithm>     // clamp/max (bubble layout)
#include <filesystem>    // .gguf model resolution
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
// TIER-2 STREAMING (WP-0): Heightfield HOISTED verbatim to
// app/world_hosts/echo_heightfield.h (x3::game::Heightfield) so the region
// packages share the exact type. The alias keeps every host call site
// (`Heightfield hf;`, `hf.heightAt(...)`) reading unchanged.
using Heightfield = x3::game::Heightfield;

// TU-local scroll accumulator (only one --world host ever runs at a time). GLFW
// scroll is event-driven, so we sum deltas here and drain them once per frame.
double g_scrollY = 0.0;
void scrollCB(GLFWwindow*, double /*xoff*/, double yoff) { g_scrollY += yoff; }

// ===== CONSOLE (` key) — the ENGINE console (D6 IConsole + app/hud.cpp
// front-end, same as EFLZ), not a host-local reimplementation. The host
// registers its world commands on it; these overrides are what those commands
// poke (-1/negative = keep the env-var default behaviour).
x3::game::Hud* g_hud = nullptr;          // char-callback target while console open
int   g_volOverride = -1;    // console `vol on|off|auto`: -1 env default, else 0/1
std::string g_playasCapPath; // PLAYAS_DEMO: armed capture to finalize post-endFrame
std::string g_shotPath;      // console `screenshot`: armed capture to finalize post-endFrame
float g_ambScale = 1.0f;     // console `amb <scale>`: city ambient multiplier (Tim's glare knob)
float g_hazeScale = 1.0f;    // console `haze <scale>`: aerial-fog multiplier (distance washout knob)
float g_todSpeed = 1.0f;     // console `todspeed <mult>`: day-clock rate
float g_expMul   = 1.0f;     // r_exposure: post exposure multiplier
float g_bloomMul = 1.0f;     // console `bloom <mult>`: bloom intensity multiplier
float g_flySpeedMul = 1.0f;  // console `speed <mult>`: fly-mode speed multiplier
float g_sensMul  = 1.0f;     // console `sens <mult>`: mouselook sensitivity multiplier

// r_* POST-STACK cvars (the full X3Native family, per Tim: "Should have
// everything X3native has"). applyAtmosphere is the SINGLE setPostFX writer;
// it reads these each frame. Negative float = keep this world's scene-tuned
// dynamic value (the -1 sentinel convention from PostFXParams).
struct EchoPostCv {
    int   tonemap = 1, bloomOn = 1, ae = 1, taa = 1, velocity = 0;
    float bloomIntensity = -1.0f, bloomThreshold = -1.0f;
    float aeSpeed = -1.0f, aeMin = -1.0f, aeMax = -1.0f, aeKey = -1.0f;
    float taaSharpen = -1.0f;
} g_postCv;
float g_sunOverride = -1.0f; // console `sun <scale>|auto`: <0 = auto ramp
void charCB(GLFWwindow*, unsigned int c) {
    if (g_hud && g_hud->consoleOpen()) g_hud->onChar(c);
}

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
        // DUSTY-DAY FIX (Tim 2026-07-28): 0.35 kept the amber blush alive
        // through HALF the day arc (midday elev 0.72) — the sky read tan from
        // mid-morning on. 0.16 confines it to genuine golden hour.
        const float low = clamp01(1.0f - s.sunElevation / 0.16f);
        constexpr float kAmber[3] = { 0.98f, 0.52f, 0.24f };
        for (int i = 0; i < 3; ++i) {
            sky.horizon[i] += (kAmber[i] - sky.horizon[i]) * (0.65f * low);
            sky.zenith[i]  += (kAmber[i] - sky.zenith[i])  * (0.12f * low);
        }
    }
    // ☀ SUN RADIANCE. sky.sunLight defaults to 1.0 and this world never set it, so
    // daylight was no brighter than the ambient floor. Drive it with the day ramp.
    // ⚠ This is a CONTRAST improvement, NOT the "flat towers / no shadows" fix — that
    // was the SUN ELEVATION (see todCfg.middayElevation in hostEchotropolis, and the
    // cosE reconstruction in app/tod.cpp). Measured at golden, aerial cam, ratio of
    // fully-lit to fully-shadowed pixels at equal N.L: sunLight 0/0.5/1/2.3/40 ->
    // 1.04 / 1.27 / 1.42 / 1.71 / 1.58 (40 loses to tonemap clipping). The reason
    // raising this "did nothing" to the eye is AUTO-EXPOSURE: over the same sweep the
    // frame MEAN barely moves (153.8/153.0/151.7/150.0) because AE renormalizes it.
    // Never judge a light change in this world by frame brightness — use --legacypost
    // (now honoured, see applyAtmosphere) or an in-frame lit:shadowed ratio.
    // ECHO_SUNLIGHT overrides for A/B.
    {
        const float sunEnv = [](){ const char* e = std::getenv("ECHO_SUNLIGHT");
                                   return e ? (float)std::atof(e) : -1.0f; }();
        // 3.20 was compensation for the WRONG diagnosis (the real fix was sun
        // ELEVATION, a5a56a6e) — at 3.3x everything washed out to cream (Tim's
        // report). Modest ramp; elevation does the work now.
        sky.sunLight = (g_sunOverride >= 0.0f) ? g_sunOverride
                     : (sunEnv >= 0.0f) ? sunEnv : (0.10f + 1.05f * dayness);
    }
    device->setSkyParams(sky);
    // Night ambient FLOOR: the pure-black sky kills the IBL contribution, so at
    // night the terrain is lit by ambient alone — hold a moonlight minimum so the
    // island stays readable while the sky dome itself stays black.
    // ⚠ GAMMA UNWIND (2026-07-25, docs/HANDOFF §11): 0.11 -> 0.16 -> 0.22 was an
    // ESCALATING COMPENSATION for the un-encoded swapchain (linear written to an sRGB
    // display crushes mid-tones). With *_SRGB encoding restored in vk_targets.cpp,
    // this is back to its ORIGINAL calibrated value. Re-tune from HERE, not from the
    // compensated numbers.
    constexpr float kNightAmbFloor[3] = { 0.11f, 0.12f, 0.17f };
    // DAY sky-fill floor: sun-shadowed city walls get almost no direct sun at noon and
    // live on this fill. Halved from the 0.34/0.38/0.46 compensation (same gamma
    // unwind) — a real value is still needed here, the city is not open terrain.
    constexpr float kDayAmbFloor[3] = { 0.17f, 0.19f, 0.23f };
    float amb[3];
    for (int i = 0; i < 3; ++i) {
        amb[i] = s.ambient[i] + s.auroraTint[i];
        const float floorI = kNightAmbFloor[i] * (1.0f - dayness)
                           + kDayAmbFloor[i]   * dayness;
        if (amb[i] < floorI) amb[i] = floorI;
    }
    device->setAmbient(amb[0] * g_ambScale, amb[1] * g_ambScale, amb[2] * g_ambScale);
}

// ---- SEA CONTINUITY (the "ocean plank", 2026-08-12) -------------------------
// ONE albedo shared by the two surfaces that make up the sea: the baked
// `island_ocean` horizon quad (via EnvArtSystem::setMaterialOverride at island
// load) and the Gerstner patch's edge/horizon fade target (WaterParams::
// horizonColor). They used to disagree — pale desaturated ring vs saturated
// animated patch — and the disagreement drew a hard camera-locked square across
// the water. Tune together or not at all. Linear, NOT sRGB.
// ECHO_SEA_ALBEDO="r,g,b" overrides for a no-rebuild A/B.
bool seaLegacy() {
    static const bool v = [](){ const char* e = std::getenv("ECHO_SEA_LEGACY"); return e && *e == '1'; }();
    return v;
}
const float* seaAlbedo() {
    // ECHO_SEA_LEGACY=1 hands back the bake's own pale ring albedo.
    static float legacy[3] = { 0.285f, 0.34f, 0.42f };
    if (seaLegacy()) return legacy;
    static float v[3] = { 0.052f, 0.108f, 0.150f };   // deep coastal blue-grey
    static bool init = false;
    if (!init) {
        init = true;
        if (const char* e = std::getenv("ECHO_SEA_ALBEDO")) {
            const char* s = e; char* end = nullptr;
            for (int i = 0; i < 3 && s && *s; ++i) {
                v[i] = std::strtof(s, &end);
                s = (end && *end == ',') ? end + 1 : nullptr;
            }
        }
    }
    return v;
}
#define kSeaAlbedo seaAlbedo()

// Gerstner ocean at sea level 0, lit by the SAME sun as the sky (doctrine: one
// light). Water color follows the daylight: full color at noon, ember-dark at
// night (the baked GLB ocean ring darkens automatically via the PBR sun).
void applyOcean(x3::rhi::IRenderDevice* device, float t, const x3::game::TodSample& s,
                float eyeHeight = 0.0f) {
    x3::rhi::IRenderDevice::WaterParams wp{};
    const float daynessGate = (s.sunElevation + 0.06f) / 0.30f;
    // WATER WAVE 1 (Tim: "The square white wave artifact in the water was
    // supposed to be handled?"): from ALTITUDE the engine's 240m Gerstner patch
    // reads as a bright animated SQUARE inside the dark baked ocean ring — the
    // sheet artifact in every aerial capture. The patch only earns its keep
    // near sea level (close-up detail water); above kPatchMaxEye the baked
    // ring owns the ocean alone. Smooth cutover, no pop: the fade band kills
    // it across 60m of climb.
    constexpr float kPatchMaxEye = 140.0f;
    // A/B DIAGNOSTIC (2026-08-12): ECHO_WATER_OFF=1 kills the Gerstner patch at
    // ANY eye height, so a capture shows the baked ring alone. This is how the
    // sea-level "plank" was pinned on the patch boundary rather than on a fog,
    // LOD or depth seam — same frame, patch on vs off.
    static const bool kWaterOff = [](){ const char* e=std::getenv("ECHO_WATER_OFF"); return e && *e=='1'; }();
    if (kWaterOff || eyeHeight > kPatchMaxEye) {
        x3::rhi::IRenderDevice::WaterParams off{};
        off.enabled = false;
        device->setWaterParams(off);
        return;
    }
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
    wp.enabled = true; wp.time = t;
    // SEA DATUM (2026-08-12, sea-level lane). This used to read
    // `wp.seaLevel = 0.10f` — a +0.10 lift bought to keep the Gerstner troughs
    // off the baked ring (the "WATER WAVE 1b" black-shards fight). That lift is
    // what put the DRAWN water 0.10 m above the sea every seat, road, lane clip
    // and swim query in the world was measured against, so a cottage at
    // heightAt = +0.05 passed "above sea level" and then rendered underwater.
    // The patch now rides kEchoSeaLevelY like everything else, and the ring is
    // respected by CAPPING AMPLITUDE instead of by lifting the sea — see
    // echo_sea.h for the full argument and echoMaxAmplitude() for the bound.
    // These five fields ARE kSwellHarbor; taking them from the preset is what
    // stops applyOcean and echo_water.h's table from drifting apart again.
    const x3::game::WaterTuning& swell = x3::game::kSwellHarbor;
    // A/B LEVER (default-off): ECHO_SEA_LEGACY_Y=<m> lifts the DRAWN patch off
    // the datum again, so the pre-unification sea (0.10) and the unified sea
    // can be captured from ONE binary at identical framing. Paired with
    // ECHO_SHIP_FLATBOB=1 it reproduces the exact before-state.
    static const float kLegacyLift = [](){ const char* e=std::getenv("ECHO_SEA_LEGACY_Y");
                                           return e ? (float)std::atof(e) : 0.0f; }();
    wp.seaLevel  = swell.seaLevel + kLegacyLift;
    wp.amplitude = swell.amplitude;
    wp.steepness = swell.steepness;
    wp.waveLength = swell.waveLength;
    wp.speed     = swell.speed;
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
    // PLANK FIX part 2/2 — hand the patch edge the colour the baked 28 km
    // `island_ocean` quad beyond it actually renders as, so the square boundary
    // has no contrast left to draw. Same albedo the island material override
    // uses (seaAlbedo()), scaled by the same daylight factor that drives the
    // patch's own deep/shallow tints, plus the fresnel-lifted sky share a flat
    // up-facing dielectric picks up at grazing view — the ring's shading is
    // constant per view angle, so a constant is the right approximation.
    // ECHO_SEA_LEGACY=1 restores the pre-fix sea EXACTLY (bake albedo on the ring,
    // no horizonColor -> water.frag falls back to its historic 0.82..1.0 sky
    // fade) so the plank can be A/B'd from one binary.
    if (!seaLegacy()) {
        const float* sa = kSeaAlbedo;
        // Coefficients MEASURED, not guessed: with the patch edge fully handed
        // off, the water's boundary pixel IS tonemap(horizonColor), so the pair
        // was swept against the ring's own rendered value at the seam until the
        // two matched (ECHO_SEA_HORIZON="r,g,b" is the sweep knob).
        const float lift = 0.028f * dayF;   // grazing fresnel share of the sky IBL
        for (int i = 0; i < 3; ++i)
            wp.horizonColor[i] = sa[i] * dayF * 1.44f + s.sky.horizon[i] * lift;
        if (const char* e = std::getenv("ECHO_SEA_HORIZON")) {
            const char* p = e; char* end = nullptr;
            for (int i = 0; i < 3 && p && *p; ++i) {
                wp.horizonColor[i] = std::strtof(p, &end);
                p = (end && *end == ',') ? end + 1 : nullptr;
            }
        }
    }
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
// --legacypost / --notaa, latched from HostContext at host start so the per-frame
// setPostFX below cannot clobber them (see the note at that call).
static int  gLegacyPost = 0;
static bool gNoTaa      = false;

void applyAtmosphere(x3::rhi::IRenderDevice* device, const x3::game::TodSample& s) {
    auto clamp01 = [](float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); };
    auto mix3 = [](float out[3], const float a[3], const float b[3], float t) {
        for (int i = 0; i < 3; ++i) out[i] = a[i] + (b[i] - a[i]) * t;
    };
    // dayness: 0 deep night .. 1 full day (matches applyTodSample's ramp).
    const float dayness = clamp01((s.sunElevation + 0.06f) / 0.30f);
    // low: golden-hour weight — sun up but near the horizon (peaks at elev 0).
    const float low = (s.sunElevation > 0.0f) ? clamp01(1.0f - s.sunElevation / 0.16f) : 0.0f;   // DUSTY-DAY FIX: golden hour only
    // night: 1 at deep night, 0 by full day (hoisted — the volumetric block below
    // and the HDR-post block further down both scale by it).
    const float night = 1.0f - dayness;

    // ---- 1. AERIAL PERSPECTIVE -------------------------------------------------
    // Beer-Lambert depth fog whose COLOR matches the sky the island melts into:
    // cool blue-white by day, warm gold at golden hour, deep blue at night. With
    // the 2.8km orbit the far island (~4km) hazes to ~0.55 and the 7km ocean-ring
    // horizon to ~0.8, dissolving the hard water/sky line seen in the baseline.
    constexpr float kFogDay[3]    = { 0.58f, 0.72f, 0.90f };  // cool scattering blue-white
    constexpr float kFogGold[3]   = { 1.00f, 0.58f, 0.26f };  // RICH golden-hour haze (saturated)
    constexpr float kFogNight[3]  = { 0.02f, 0.03f, 0.05f };  // urban light-pollution haze (was rural deep-blue)
    x3::rhi::IRenderDevice::FogParams fog;
    fog.enabled = true;
    mix3(fog.color, kFogNight, kFogDay, dayness);              // night -> day base
    // Warm HARD toward saturated gold at golden hour. High weight (not a 50/50
    // blend) so cool-blue + gold never average into gray — the haze reads amber.
    const float goldW = clamp01(low * 0.9f);   // DUSTY-DAY FIX: amber accents the hour, never owns the day
    mix3(fog.color, fog.color, kFogGold, goldW);
    // Tuned after Tim's sea-approach screenshot (2026-07-11): the whole 4km island
    // was drowning in amber soup at golden hour. The haze must live at the HORIZON
    // (10km+) while the island — a 3-6km subject — stays readable. Halve the
    // low-sun density ramp and pull the opacity ceiling down.
    // Film-review pass (2026-07-15): the near CITY vista had ZERO aerial perspective —
    // near + far towers shared one value, reading as a flat card. Pull the fog START in
    // (900->380) and lift density so mid/far towers desaturate toward the sky with depth,
    // while maxOpacity stays capped so the 4km sea-approach island doesn't drown in soup.
    fog.start      = 380.0f;                                   // depth begins just past the near towers
    fog.density    = (0.00019f + 0.00004f * low) * g_hazeScale; // clear aerial perspective across the city
    fog.maxOpacity = std::min(0.95f, (0.70f + 0.06f * low) * g_hazeScale); // far island hazes, never dissolves

    // ---- 1b. VOLUMETRIC LIGHT SCATTERING (the CP2077/Witcher signature) --------
    // Upgrades the flat extinction above into a raymarched medium: the sun shadow
    // map is sampled along the view ray (god rays through gaps in the skyline) and
    // every street lamp / neon sign in the frame's point-light set scatters a real
    // halo of haze around itself. All of the params above still drive extinction —
    // this only ADDS in-scattering, so the aerial-perspective tuning is untouched.
    //
    // TIME-OF-DAY SCALING (this is an atmosphere, not a constant): the air is
    // busiest at night (light pollution + damp harbour air, and it is the only time
    // the neon reads) and at golden hour (long shadow-ray paths through low sun).
    // Flat midday gets the least — high sun through clean air is the one case where
    // heavy shafts look like a bug, and it would wash the noon city out.
    // ECHO_VOL=0 forces the flat path back on (A/B harness, same env-var convention
    // as ECHO_TOD). With it set, this world renders byte-identical to the pre-
    // volumetric build — that is the test that keeps the opt-in discipline honest.
    // ⚠ PERF: volumetrics shipped UNPROFILED and cost 100ms/frame (10 FPS measured
    // windowed — Tim got a slideshow). 48 steps x <=24 lights per pixel at full res
    // is the bill. DEFAULT OFF until the pass runs at half-res / fewer steps;
    // ECHO_VOL=1 opts in (captures still use it explicitly).
    static const bool kVolEnv = [] {
        const char* e = std::getenv("ECHO_VOL");
        return e && e[0] == '1';
    }();
    fog.volumetric      = (g_volOverride >= 0) ? (g_volOverride != 0) : kVolEnv;
    fog.anisotropy      = 0.76f;    // strongly forward — looking toward a light blooms
    fog.steps           = 48;       // power-distributed; near-camera density where the lamps are
    fog.maxDistance     = 900.0f;   // beyond this the flat aerial term carries the depth
    // Scattering coefficient. Night carries the effect; golden hour adds a lift; a
    // clear high sun keeps only a whisper so noon does not go milky.
    fog.scatterStrength = 0.0020f + 0.0100f * night + 0.0060f * low;
    // Sun shafts need the sun ABOVE the horizon; weight them toward golden hour
    // when the rays rake sideways through the towers instead of straight down.
    // The small coefficient is deliberate: the sun's ray path is the WHOLE march
    // (hundreds of metres) where a lamp's is ~15 m, so equal weights would bury
    // the city under a uniform sun wash.
    fog.sunScatter      = dayness * (0.022f + 0.065f * low);
    // Lamp/neon haze is the money shot in the dark and pointless in daylight.
    fog.lightScatter    = 3.0f + 33.0f * night;
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
    // (`night` is hoisted to the top of this function — the volumetric block uses it too.)
    x3::rhi::IRenderDevice::PostFXParams px;   // defaults: ACES, autoexposure, TAA on
    px.bloomThreshold = 1.08f;                 // sun-glint blooms; not the whole water sheet
    px.bloomIntensity = 0.14f + 0.06f * low + 0.10f * night;  // neon glow at night
    // ⚠ GAMMA UNWIND: the +1.6 aeMax and +0.35 exposure night lifts were fighting the
    // un-encoded swapchain, not real darkness. Halved now that *_SRGB encodes properly;
    // re-tune from here against fresh captures.
    px.aeMax          = 2.20f + 0.30f * low + 0.8f * night;
    // r_* cvar overrides (single-writer rule: only THIS call touches setPostFX;
    // the per-frame cvar sync fills g_postCv; -1 float = keep the scene value).
    px.tonemapMode  = g_postCv.tonemap;
    px.bloomEnabled = g_postCv.bloomOn != 0;
    if (g_postCv.bloomIntensity >= 0.0f) px.bloomIntensity = g_postCv.bloomIntensity;
    if (g_postCv.bloomThreshold >= 0.0f) px.bloomThreshold = g_postCv.bloomThreshold;
    if (!g_postCv.ae) px.autoExposure = false;
    if (g_postCv.aeSpeed >= 0.0f) px.aeSpeed = g_postCv.aeSpeed;
    if (g_postCv.aeMin   >= 0.0f) px.aeMin   = g_postCv.aeMin;
    if (g_postCv.aeMax   >= 0.0f) px.aeMax   = g_postCv.aeMax;
    if (g_postCv.aeKey   >= 0.0f) px.aeKey   = g_postCv.aeKey;
    if (!g_postCv.taa) px.taa = false;
    if (g_postCv.taaSharpen >= 0.0f) px.taaSharpen = g_postCv.taaSharpen;
    px.velocity = g_postCv.velocity != 0;
    // ⚠ This setPostFX runs EVERY FRAME with a DEFAULT-CONSTRUCTED px (autoExposure
    // and taa both true), so it silently overrode main.cpp's --legacypost / --notaa
    // A/B levers: eye adaptation could not be turned off in this world. That is not
    // cosmetic — AE renormalizes the frame, so "raise the sun and see if it changes"
    // reads FLAT (mean 153.8/151.7/150.0 across sunLight 0/1/2.3) while with AE
    // genuinely off the same sweep reads 102.9/132.4/146.3. Honour the CLI flags.
    if (gLegacyPost) { px.autoExposure = false; px.taa = false;
                       if (gLegacyPost > 1) { px.bloomEnabled = false; px.tonemapMode = 0; } }
    if (gNoTaa) px.taa = false;
    device->setPostFX(px);
    device->setBloom((0.12f + 0.06f * low + 0.10f * night) * g_bloomMul);
    device->setExposure((1.0f + 0.16f * low + 0.15f * night) * g_expMul);

    // ---- 5. SHADOWS: focus the single shadow map on the CROWN so it RESOLVES -----
    // Was a 4.4km box centred at origin — one shadow map over 4.4km ~= 2m/texel, so
    // golden-hour shadows never resolved on the buildings (the #1 "no cast shadows"
    // finding). Recentre on the downtown crown (-20,760) and shrink to a 1.64km box
    // (~0.4-0.8m/texel) so the low sun throws crisp long shadows across the city +
    // grounds the mine forest (which sits within the box) with real contact shadow.
    // Metropolis: widened + recentred to also cover the east-flats district pads
    // (x to ~1120) — a pad outside the box reads fully shadowed (fake dark city).
    // ⚠ RESOLUTION MATH (the real constraint): ONE 2048px map over a 2*halfExtent box.
    //   halfExtent 1050 -> 2100m / 2048px = 1.03 m/TEXEL. A bollard is 0.3m wide; a
    //   kerb is 0.15m. Nothing smaller than a building can cast a shadow it can even
    //   resolve — which is exactly the "no cast shadows anywhere" review finding.
    //   This was 820 (0.8 m/texel) and got WIDENED to 1050 to cover the district pads,
    //   trading away the resolution that made shadows work. The real fix is CASCADES
    //   (docs/HANDOFF §5 item 2); until then this is a coverage-vs-sharpness dial.
    // ECHO_SHADOW_EXTENT overrides for A/B (e.g. 260 => 0.25 m/texel, prop-sharp).
    const float shExt = [](){ const char* e = std::getenv("ECHO_SHADOW_EXTENT");
                              return e ? (float)std::atof(e) : 1050.0f; }();
    device->setShadowBounds(140.0f, 0.0f, 980.0f, shExt);
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
    // RT default OFF for this world (AAA-eye pass 2026-07-22): the pack districts'
    // prefab expansion leaves coplanar duplicate shells in the TLAS — RTAO/RT-sun
    // rays self-intersect at ~zero distance and crush whole districts to black.
    // Raster CSM+SSAO renders them correctly. ECHO_RT=1 re-enables for A/B until
    // the dupe-geometry cleanup lands.
    const char* e = std::getenv("ECHO_RT");
    if (!e || e[0] != '1') {
        x3::logInfo("--world echotropolis: RT default-off (district coplanar-dupe self-shadow; ECHO_RT=1 to opt in)");
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

// =============================================================================
// CITIZEN TALK — walk up to a resident, press T, and have a real conversation
// driven by the LOCAL LLM (engine/llm, llama.cpp CPU inference). The reply
// streams into a CHAT BUBBLE floating over that NPC's head.
//
// LATENCY BEATS ELOQUENCE. A street exchange is one or two sentences; a citizen
// who answers in under a second feels ALIVE, one that composes a paragraph over
// four seconds feels broken. So: prefer the SMALLEST .gguf in assets/models/llm
// (0.5B over 3B), keep the persona prompt SHORT (prefill is paid on every chat),
// cap output hard (~48 tokens), and stream tokens into the bubble so the first
// words land almost immediately instead of after the whole reply.
//
// FRAME SAFETY: nothing here ever blocks. startChat/submit enqueue onto the LLM's
// own inference thread; the frame thread only poll()s (non-blocking) and appends.
// The world stays fully playable while a reply generates.
// =============================================================================

// Resolve the .gguf to load. ECHO_LLM_MODEL=<path> wins (A/B model sizes with no
// rebuild); otherwise scan assets/models/llm and take the SMALLEST .gguf — model-
// agnostic (drop any instruct .gguf in and it just works) and speed-first.
std::string resolveTalkModelPath() {
    if (const char* env = std::getenv("ECHO_LLM_MODEL"); env && env[0]) {
        std::error_code ec;
        if (std::filesystem::exists(env, ec)) return std::string(env);
        x3::logWarn(std::string("[talk] ECHO_LLM_MODEL=") + env + " does not exist — falling back to the model dir");
    }
    const std::string dir = x3::game::assetRoot() + "/models/llm";
    std::string best; std::uintmax_t bestSz = 0;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (e.path().extension() != ".gguf") continue;
        std::error_code fec;
        const std::uintmax_t sz = std::filesystem::file_size(e.path(), fec);
        if (fec) continue;
        if (best.empty() || sz < bestSz) { best = e.path().string(); bestSz = sz; }
    }
    return best;   // "" when the dir is empty/absent — the modelless path
}

// The canned player lines (T sends the selected one, [ / ] cycle). Free-text entry
// would need a GLFW char callback + an edit caret; these keep the street exchange
// snappy and never fight the existing key map.
const char* const kTalkPrompts[] = {
    "Who are you?",
    "What's going on around here?",
    "Any trouble lately?",
    "How's the work treating you?",
    "What do you make of Echo Harbor?",
};
constexpr int kTalkPromptCount = (int)(sizeof(kTalkPrompts) / sizeof(kTalkPrompts[0]));

// A natural-language phrase for the NPC's current scheduled activity, so the
// persona prompt says "on your way to work" rather than "ToWork".
const char* talkActivityPhrase(x3::game::NpcActivity a) {
    switch (a) {
        case x3::game::NpcActivity::AtHome:    return "at home";
        case x3::game::NpcActivity::ToWork:    return "on your way to work";
        case x3::game::NpcActivity::AtWork:    return "in the middle of your shift";
        case x3::game::NpcActivity::ToLeisure: return "heading out to unwind";
        case x3::game::NpcActivity::AtLeisure: return "off the clock, killing time";
        case x3::game::NpcActivity::ToHome:    return "walking home";
        default:                               return "out on the street";
    }
}

// Build the persona system prompt from the agent's REAL data. Deliberately TERSE:
// every token here is prefill cost paid on the first reply of every conversation.
std::string talkPersonaPrompt(const x3::game::NpcAgent& a) {
    const auto& pr = x3::game::persona(a.arch);
    const char* det = (a.detailIdx >= 0 && (uint32_t)a.detailIdx < pr.detailCount)
                    ? pr.detail[a.detailIdx] : (pr.detailCount ? pr.detail[0] : "");
    const char* voc = pr.voiceCount ? pr.voice[a.seed % pr.voiceCount] : "";
    std::string role = x3::game::archetypeName(a.arch);
    for (char& c : role) c = (char)std::tolower((unsigned char)c);
    std::string s = "You are " + a.name + ", a " + role +
                    " in Echo Harbor, 2038. You are " + talkActivityPhrase(a.activity) + ".";
    if (det && det[0]) { s += " "; s += det; s += "."; }
    if (voc && voc[0]) { s += " You talk like this: \""; s += voc; s += "\""; }
    s += " Answer in ONE short sentence, in character. No narration, no quotes, no lists.";
    return s;
}

// Tidy a raw model reply for display: strip surrounding whitespace plus the quote
// marks small models like to wrap dialogue in despite being told not to.
std::string talkTidy(const std::string& raw) {
    const size_t b = raw.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return std::string();
    const size_t e = raw.find_last_not_of(" \t\r\n");
    std::string s = raw.substr(b, e - b + 1);
    while (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front())
        s = s.substr(1, s.size() - 2);
    return s;
}

// Word-wrap `text` to at most `maxPx` of rendered width per line (measured with the
// REAL font advance, not a character guess) and at most `maxLines` lines.
void talkWrap(const x3::rhi::IRenderDevice& device, const std::string& text,
              float px, float maxPx, int maxLines, std::vector<std::string>& out) {
    out.clear();
    std::string line, word;
    auto flush = [&]() {
        if (!line.empty() && (int)out.size() < maxLines) out.push_back(line);
        line.clear();
    };
    auto pushWord = [&](const std::string& w) {
        if (w.empty()) return;
        const std::string cand = line.empty() ? w : line + " " + w;
        if (!line.empty() && device.textAdvance(x3::rhi::FontRole::Menu, cand.c_str(), px) > maxPx)
            flush();
        line = line.empty() ? w : line + " " + w;
    };
    for (char c : text) {
        if (c == '\n' || c == '\r') { pushWord(word); word.clear(); flush(); continue; }
        if (c == ' ' || c == '\t')  { pushWord(word); word.clear(); continue; }
        word.push_back(c);
    }
    pushWord(word);
    // Anything past maxLines is dropped by flush() — mark the overflow so a long
    // reply reads as clipped rather than as a sentence that just stops.
    const bool overflow = !line.empty() && (int)out.size() >= maxLines;
    flush();
    if (overflow && !out.empty()) out.back() += " ...";
    if (out.empty()) out.push_back("");
}

// Draw the chat bubble at screen pixel (sx,sy) — the crowd_chatter treatment
// (rounded dark slab from overlapping quads + rim + tail) grown to multi-line.
// `alpha` fades it; `speaker` is drawn as a small gold caption line.
void drawTalkBubble(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                    float sx, float sy, const char* speaker,
                    const std::vector<std::string>& lines, float alpha) {
    if (alpha <= 0.02f) return;
    uint32_t hw = 0, hh = 0; device.hudSize(hw, hh);
    const float px = 15.0f, lineH = 20.0f, pad = 11.0f, capPx = 11.0f;
    float tw = device.textAdvance(x3::rhi::FontRole::Menu, speaker ? speaker : "", capPx);
    for (const auto& l : lines)
        tw = std::max(tw, device.textAdvance(x3::rhi::FontRole::Menu, l.c_str(), px));
    const float w = tw + 2.0f * pad;
    const float h = 2.0f * pad + capPx + 6.0f + lineH * (float)lines.size();
    float x0 = sx - w * 0.5f;
    float y0 = sy - h - 14.0f;                     // slab sits ABOVE the head anchor
    if (x0 < 4.0f) x0 = 4.0f;
    if (hw > 8 && x0 + w > (float)hw - 4.0f) x0 = (float)hw - 4.0f - w;
    if (y0 < 4.0f) y0 = 4.0f;
    if (hh > 44 && y0 > (float)hh - 44.0f) y0 = (float)hh - 44.0f;
    const float a = alpha;
    const float rim[4]   = { 0.55f, 0.62f, 0.72f, 0.55f * a };
    const float panel[4] = { 0.045f, 0.055f, 0.085f, 0.90f * a };
    device.drawHudQuad(frame, x0 - 1.5f, y0 + 1.5f, w + 3.0f, h - 3.0f, rim);
    device.drawHudQuad(frame, x0 + 1.5f, y0 - 1.5f, w - 3.0f, h + 3.0f, rim);
    device.drawHudQuad(frame, x0, y0 + 4.0f, w, h - 8.0f, panel);
    device.drawHudQuad(frame, x0 + 4.0f, y0, w - 8.0f, h, panel);
    // Tail stepping down toward the head.
    const float tailX = std::clamp(sx, x0 + 8.0f, x0 + w - 8.0f);
    device.drawHudQuad(frame, tailX - 6.0f, y0 + h, 12.0f, 5.0f, panel);
    device.drawHudQuad(frame, tailX - 3.0f, y0 + h + 5.0f, 6.0f, 4.0f, panel);
    // Speaker caption (gold) then the wrapped reply (near-white over a shadow).
    const float gold[4]   = { 1.0f, 0.82f, 0.42f, 0.95f * a };
    const float shadow[4] = { 0.0f, 0.0f, 0.0f, 0.75f * a };
    const float ink[4]    = { 0.93f, 0.96f, 1.00f, a };
    if (speaker && speaker[0])
        device.drawHudTextF(frame, x3::rhi::FontRole::Menu, speaker, x0 + pad, y0 + pad - 3.0f, capPx, gold);
    float ty = y0 + pad + capPx + 4.0f;
    for (const auto& l : lines) {
        device.drawHudTextF(frame, x3::rhi::FontRole::Menu, l.c_str(), x0 + pad + 1.2f, ty + 1.2f, px, shadow);
        device.drawHudTextF(frame, x3::rhi::FontRole::Menu, l.c_str(), x0 + pad, ty, px, ink);
        ty += lineH;
    }
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
    gLegacyPost = hc.legacyPost;   // let --legacypost / --notaa survive applyAtmosphere
    gNoTaa      = hc.noTaa;

    x3::game::TodConfig todCfg;
    // Tim's pacing order (2026-07-27): "Keep it in each one for 20-30 min" —
    // a phase (morning/noon/golden/dusk/night...) should hold 20-30 REAL
    // minutes, so the full cycle runs ~2 hours. ECHO_SKY_DAY_SECONDS overrides;
    // console `todspeed` still scales live (todspeed 30 ~= the old 4-min day).
    todCfg.dayLengthSeconds = [](){ const char* e = std::getenv("ECHO_SKY_DAY_SECONDS");
                                    return e ? (float)std::atof(e) : 7200.0f; }();
    // SUN ELEVATION — the "flat towers / no cast shadows" root cause. The engine
    // default (1.0 = sin of the peak elevation) puts midday at the ZENITH: the sun
    // direction is then ~(0, 0.98, 0.2), so every VERTICAL facade sees N.L <= 0.2
    // and renders at its ambient value regardless of orientation, and every cast
    // shadow falls straight down inside the caster's own footprint. Echo Harbor is
    // a PNW fjord city (~lat 47): cap the midday sun at ~46 deg so daylight RAKES
    // the facades and the towers throw real shadows down the streets.
    todCfg.middayElevation = 0.72f;   // sin(46 deg)
    x3::game::TimeOfDay tod(todCfg);
    {
        const char* e = std::getenv("ECHO_TOD");
        tod.setDayFraction(canonTodFraction(e ? e : "golden"));
    }
    applyTodSample(device, tod.sample());
    applyAtmosphere(device, tod.sample());   // ATMOSPHERE: aerial haze + grade + bloom
    applyRayTracing(device);                 // RAY TRACING: soft sun shadows + RT AO (gated; no-op on non-RT)
    // Diagnostic: ECHO_DEBUGVIEW=1 renders SHADING NORMALS (the instrument that
    // separates "light can't reach it" from "its normal points into the wall").
    if (const char* dv = std::getenv("ECHO_DEBUGVIEW")) device->setDebugView(std::atoi(dv));
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
        // ISLAND-REGEN (2026-08-03): the original SimCityLLM2 bakes were LOST —
        // the terrain is now generated IN-REPO by tools/echo_terrain_gen.py
        // (fjord-inlet + harbor + crown plateau, Tim's two-city canon) and the
        // committed LFS copy under assets/island_mesa is the default. The old
        // authoring-box dir still wins if it exists (same slot names), and
        // ECHO_ISLAND_DIR overrides everything (fjord/desert swap tests).
        std::string islandDir = dirEnv ? dirEnv : "D:/GameDev/EchoHarbor/assets/island_mesa";
        if (!dirEnv) {
            std::error_code ec;
            auto hasBake = [&ec](const std::string& d) {
                return std::filesystem::exists(d + "/island_20260530.glb", ec);
            };
            if (!hasBake(islandDir)) {
                // committed regen bake: assetRoot() first, then the repo-run
                // layout (build-ninja/bin is one level shallower than the
                // build/bin/<Config> shape assetRoot()'s heuristic assumes)
                islandDir = x3::game::assetRoot() + "/island_mesa";
                if (!hasBake(islandDir)) islandDir = "assets/island_mesa";
            }
        }
        // ---- THE OCEAN "PLANK" (Tim 2026-08-12: "the ocean plank of white
        // artifact visual mess is there"). MECHANISM, named:
        //   The bake's `island_ocean` primitive is ONE flat 28 km quad at
        //   y = -0.4 carrying no textures and metallicFactor 0, so mesh.frag
        //   shades it on the DIELECTRIC path. Every fragment of that quad shares
        //   a single normal, so its shading is CONSTANT for a given view angle —
        //   and at the grazing angles you get looking out to sea, Schlick
        //   fresnel + the sky IBL drive it to a flat, near-white sheet (measured
        //   217,220,223 sRGB at ~250 m in the repro frame). The animated
        //   Gerstner patch, meanwhile, is a 240 m half-extent square CENTRED ON
        //   THE CAMERA (VulkanRenderDevice_internal.h kWaterPatchHalf). Where
        //   that square ends, the near-white sheet begins — an axis-aligned,
        //   dead-straight, camera-locked boundary drawn across the sea. THAT
        //   BOUNDARY IS THE PLANK. It is not a far-plane, LOD, depth-precision
        //   or reflection-plane seam, and it is not a second water plane: it is
        //   the one water patch ending against the one baked ring.
        //   (Same defect Tim reported from altitude as "the square white wave
        //   artifact"; applyOcean's 2026-07 mitigation only disables the patch
        //   above 140 m eye height, which is exactly why it survived at sea
        //   level, where he is now flying.)
        // FIX part 1/2: pull the ring's albedo down from the bake's pale
        // (0.285,0.34,0.42) to real deep water, so its diffuse + IBL term stops
        // blowing out. Part 2/2 is water.frag fading the patch edge INTO this
        // same colour instead of into the sky (WaterParams::horizonColor below).
        // ECHO_SEA_ALBEDO="r,g,b" (linear) re-tunes without a rebuild.
        {
            x3::game::EnvArtSystem::MaterialOverride sea;
            sea.nameSub = "island_ocean";
            sea.setBaseColor = true;
            sea.baseColor[0] = kSeaAlbedo[0];
            sea.baseColor[1] = kSeaAlbedo[1];
            sea.baseColor[2] = kSeaAlbedo[2];
            sea.baseColor[3] = 1.0f;
            island.setMaterialOverride({ sea });
        }
        if (island.buildFromGlb(*device, islandDir, "island_20260530.glb"))
            x3::logInfo("--world echotropolis: island GLB loaded from " + islandDir);
        else
            x3::logError("--world echotropolis: island GLB MISSING (" + islandDir +
                         "/island_20260530.glb) — rendering open sea only");
        if (hf.load(islandDir + "/island_height_20260530.png")) {
            // TERRAIN/CONTENT SYNC (2026-08-12): bind the sampler to the LAND
            // MESH the GLB above actually draws — tools/echo_terrain_gen.py
            // meshes at N_MESH=513 regardless of whether the PNG is 1025 (the
            // committed regen bake) or 2048 (the older authoring-box bake), so
            // reading the raw PNG meant every road ribbon, junction patch and
            // building seat was placed on a surface nobody renders. See
            // echo_heightfield.h's MESH-MATCHED SAMPLING note. ECHO_RAW_HF=1
            // restores the pre-fix bilinear sampler (A/B lever).
            // AND THE SEA DEPENDS ON IT (fix/echo-sea-level): the sea datum is
            // only meaningful against the surface the player SEES — reading the
            // raw PNG put the sampler up to 18.4 m off the rendered mesh, so
            // unifying sea level against a surface nobody renders would be
            // unifying nothing. Both lanes rely on this one call.
            const bool rawHf = [](){ const char* e = std::getenv("ECHO_RAW_HF");
                                     return e && e[0] && e[0] != '0'; }();
            if (!rawHf) hf.setMeshGrid(513);
            x3::logInfo(std::string("--world echotropolis: heightfield loaded ") +
                        std::to_string(hf.w) + "^2 — sampling " +
                        (hf.meshN ? "the RENDERED 513^2 land mesh (terrain/content in sync)"
                                  : "the RAW PNG (ECHO_RAW_HF — pre-fix, content sits off the mesh)") +
                        "; sea datum y=" + std::to_string(x3::game::kEchoSeaLevelY));
        } else
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
        // Drop the OLD baked "spike-cone" procedural conifers (flora_conifer_0/1/2 +
        // their shared flora_trunk primitive) and the flora_grass_0..3 ground-tuft
        // "dark chip" primitives — echotropolis_props.glb is baked as ONE unnamed node
        // with 78 primitives keyed by MATERIAL name (SimCityLLM2/tools/make_flora_glb.py
        // merges every prop into one Mesh), so there's no per-tree NODE name to match —
        // setNodeSkip() falls back to matching the primitive's MATERIAL name for exactly
        // this case (see env_art.cpp). The WOODLANDS block below replaces the conifers
        // with a real pine-GLB scatter; the meadow/wildflower/reed/bird/hawk primitives
        // are untouched. MUST be called before buildFromGlb().
        props.setNodeSkip({"flora_conifer", "flora_trunk", "flora_grass"});
        const char* pdirEnv = std::getenv("ECHO_PROPS_DIR");
        const std::string propsDir = pdirEnv ? pdirEnv : "D:/GameDev/SimCityLLM2/refs/models";
        if (props.buildFromGlb(*device, propsDir, "echotropolis_props.glb"))
            x3::logInfo("--world echotropolis: P4 props GLB loaded from " + propsDir);
        else
            x3::logWarn("--world echotropolis: P4 props GLB absent (" + propsDir +
                        "/echotropolis_props.glb) — coast undressed");
    }
    // ================= END P4 COAST DRESSING ======================================

    // ===================== TIER-2 REGIONS (WP-0 milestone A) ==============
    // The former host build blocks — houses/towers/streets+metro+subway/condos/
    // hackables/sky-drones (crown), mine site/forest/glow (west_shoulder), the
    // 3 METROPOLIS districts, the harbor boats, and the woodlands (now 9 cells)
    // — live in EchoRegion containers (echo_region_builders.cpp /
    // echo_woodlands.cpp), built here through EchoRegionSet at the SAME point
    // in boot the old blocks ran. MILESTONE A: every region force-built, the
    // streamer wired but NEVER ticked — residency gating is M-B. The 6-cam
    // byte-compare (scripts/echo_stream_ab.ps1) is the gate on this refactor.
    // M-A DEVIATIONS (kept host-side, marked at their builder sites): the
    // lighthouse beam + cityLightsOn gate, streetLamps/lampScene + the
    // per-frame selectLights query, and the vendor carts (need npcLife).
    // ECHO_STREAM=0 is the rollback lever: at M-A both paths are identical
    // (same force-build, streamer inert either way) — it is logged so later
    // milestones can hard-gate on it.
    x3::game::Scene walkScene;   // hoisted (was declared just before the crowd builds):
                                 // EchoRegionCtx binds it; population is unchanged below.
    // #34a CORRIDOR AUDIT: the road network now builds BEFORE the regions so
    // every placement pass can consult EchoRoads::corridorHits — Tim's capture
    // had piers running THROUGH a Recife tower because district layouts never
    // knew the freeway existed. Decl hoisted from the infra block below; the
    // traffic-route build stays down there (it needs the car fleet).
    std::unique_ptr<x3::game::EchoRoads> roads = std::make_unique<x3::game::EchoRoads>();
    // The regenerated fjord puts the harbor city directly under the crown's
    // south wall, which folds the ring at the splice and costs the whole
    // freeway spine. Opt in HERE, not in the builder: it is a fix for THIS
    // landform, and the road graph everywhere else must stay bit-identical.
    roads->setRimSeamClearance(x3::game::EchoRoads::kRimSeamClearRegenIsland);
    if (roads->build(*device, hf)) {
        x3::logInfo("--world echotropolis: ECHO ROADS — curved network live "
                    "(pre-region build; placements audit the corridors)");
    } else {
        roads.reset();
        x3::logWarn("--world echotropolis: EchoRoads build FAILED — no freeway this boot");
    }
    // WD2 STACK: the hackable registry is born BEFORE the regions so builders
    // saturate the city with cameras as they place content (Tim: "just like
    // Watch Dogs 2"); the citizens and street tech join it further down.
    x3::game::HackableRegistry hax;
    x3::game::EchoRegionCtx regionCtx{
        *device, hf, walkScene,
        /*modelsDir*/     "D:/GameDev/SimCityLLM2/refs/models",
        /*districtsTxt*/  "assets/districts/districts.txt",
        /*vegDir*/        "D:/GameDev/EchoHarbor/assets/veg",
        /*houseForgeDir*/ "D:/Assets/_glb/prefab_buildings/HouseForge",
        /*cityDir*/       "",
        /*roads*/         roads.get(),
        /*hax*/           &hax };
    x3::game::WorldRegionGraph regionGraph;
    x3::game::WorldStreamer    regionStreamer;   // wired at M-A, first ticked at M-B
    x3::game::EchoRegionSet    regionSet;
    const bool kStreamOff = [](){ const char* se = std::getenv("ECHO_STREAM");
                                  return se && se[0] == '0'; }();
    {
        const bool streamOff = kStreamOff;
        std::vector<std::string> gerrs;
        if (!regionGraph.load("assets/world/regions.echotropolis.json", gerrs)) {
            for (const auto& e : gerrs) x3::logError("[echoregions] " + e);
            x3::logError("[echoregions] region graph FAILED to load — region content will be MISSING");
        }
        // ECHO_SKIP_REGIONS=id1,id2 — forensic knob: listed regions register NO
        // builder (they build empty). Used to bisect visual deltas per region.
        const std::string skipList = [](){ const char* e = std::getenv("ECHO_SKIP_REGIONS");
                                           return std::string(e ? e : ""); }();
        auto skipped = [&](const char* id){ return skipList.find(id) != std::string::npos; };
        auto reg = [&](const char* id, x3::game::EchoRegionSet::RegionBuilderFn fn) {
            if (skipped(id)) { x3::logWarn(std::string("[echoregions] SKIPPED (env): ") + id); return; }
            regionSet.registerBuilder(id, std::move(fn));
        };
        reg("crown",             x3::game::buildCrown);
        reg("west_shoulder",     x3::game::buildWestShoulder);
        reg("district_urban",    x3::game::buildDistrictUrban);
        reg("district_recife",   x3::game::buildDistrictRecife);
        reg("district_hivemind", x3::game::buildDistrictHivemind);
        reg("harbor_bay",        x3::game::buildHarborBay);
        // INTERIORS PILLAR: streaming interior cells (Lane-B sub-regions with
        // small radii — realize on approach, evict on leave once M-C ticks).
        reg("int_condo_rooms",   x3::game::buildCondoRooms);
        reg("int_noodle_bar",    x3::game::buildNoodleBar);
        reg("int_harbor_shop",   x3::game::buildHarborShop);
        // Woodlands cells: JSON ids <-> (cellIx 0=west..2=east, cellIz 0=south..2=north),
        // the convention echo_woodlands.cpp documents at woodlandsCellRect().
        struct CellId { const char* id; int ix, iz; };
        static const CellId kCells[] = {
            {"woodlands_NW",0,2},{"woodlands_N",1,2},{"woodlands_NE",2,2},
            {"woodlands_W", 0,1},{"woodlands_C",1,1},{"woodlands_E", 2,1},
            {"woodlands_SW",0,0},{"woodlands_S",1,0},{"woodlands_SE",2,0},
        };
        for (const auto& cid : kCells)
            reg(cid.id,
                [ix = cid.ix, iz = cid.iz](x3::game::EchoRegion& r, x3::game::EchoRegionCtx& c) {
                    x3::game::buildWoodlandsCell(ix, iz, r, c);
                });
        regionStreamer.init(regionGraph, nullptr);          // no job pool until M-B ticks it
        regionSet.init(regionCtx, regionStreamer, regionGraph);
        // WP-3's slice proof: the 9-cell union must be bit-identical to the old
        // single woodlands scatter — proven in pure math before any cell builds.
        if (hf.ok())
            x3::logInfo(std::string("[echoregions] woodlands slice self-test: ") +
                        (x3::game::echoWoodlandsSliceSelfTest(hf) ? "PASS" : "FAIL"));
        regionSet.forceAllResident(regionCtx);              // boot-build everything (M-B keeps this)
        regionSet.bindStreamerForDrawGate(&regionStreamer); // M-B: streamer view gates the draws
        x3::logInfo(std::string("[echoregions] M-A force-resident boot, ") +
                    std::to_string(regionSet.regionCount()) + " regions (ECHO_STREAM=" +
                    (streamOff ? "0/rollback-identical" : "on/wired-not-ticking") + ")");
    }

    // (TIER-2 M-A: DOWNTOWN SKYLINE moved to buildCrown — echo_region_builders.cpp.)

    // ===================== GOLD MINE + TRUCK LOT (the 2038 gold rush) =====
    // The physical gold mine SITE — purpose-built in Blender (assets/mine/mine_site.glb):
    // a timbered adit with a glowing gold seam, an A-frame headframe + pulley, an ore
    // cart on rails, tailings + ore chunks. It sits out on the WESTERN OPEN SHOULDER,
    // clear of the downtown towers, with a parked truck lot a short trek SW. The miners
    // are their OWN dedicated crew (built with the residents below) so they stand on the
    // mine's real terrain plane and walk truck-lot <-> seam. Scales are ECHO_*-tunable.
    // Base is at Y=0, ~15x17 m at scale 1 → ECHO_MINE_SCALE defaults ~1.6 (NOT 11).
    // (TIER-2 M-A: mine site + truck lot moved to buildWestShoulder. The kMine*
    // constants STAY — the miners crew build + ops dashboard below read them.)
    const float kMineX = -480.0f, kMineZ = 850.0f;   // mine mouth (Carry point B) — open west shoulder, clear of towers
    const float kLotX  = -556.0f, kLotZ  = 814.0f;   // truck lot   (Carry point A) — short trek SW
    const float kMineGy = hf.ok() ? hf.heightAt(kMineX, kMineZ) : 190.0f;  // real terrain; miners share this plane

    // (TIER-2 M-A: MINE FOREST moved to buildWestShoulder — echo_region_builders.cpp.)

    // (TIER-2 M-A: WOODLANDS moved to the 9 woodlands_* cell regions — echo_woodlands.cpp;
    //  the 9-cell union is proven bit-identical by echoWoodlandsSliceSelfTest at boot.)

    // (TIER-2 M-A: MINE MOUTH GLOW moved to buildWestShoulder — its Scene is region-owned.)

    // ===================== THE UFO (Tim's Grok→Rodin saucer) ============
    // A mothership DRIFTING on a slow circular patrol high over the crown, spinning
    // + bobbing, with an emissive underbelly GLOW and a downward ABDUCTION BEAM
    // (ufo_fx.glb, authored in world metres) tracking it. Both re-posed per frame.
    x3::game::EnvArtSystem ufo, ufoFx;
    bool ufoBuilt = false, ufoFxBuilt = false;
    constexpr float kUfoScale = 120.0f;        // ~228 m across
    // Tim (2026-07-17): "way too low" — lifted 470 -> 780 so it rides HIGH over the
    // island, and the patrol widened to sweep the whole region, not hug downtown.
    constexpr float kUfoCenX = -20.0f, kUfoCenZ = 760.0f, kUfoY = 780.0f;  // patrol centre
    constexpr float kUfoDriftR = 520.0f;       // patrol radius over the island
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
        // ABDUCTION BEAM: no longer a permanent slab (the "solid ice-cream cone"
        // slop). It fires as an EVENT — ~12s out of every 75 — ramping smoothly in
        // and out (sine ease on scale; 0 = hidden), so it reads as the mothership
        // DOING something, not scenery.
        float beamS = 0.0f;
        {
            const float ph = std::fmod(t, 75.0f);
            if (ph < 12.0f) {
                const float u = ph / 12.0f;                       // 0..1 over the event
                beamS = std::sin(u * 3.14159265f);                // ease in-out
            }
        }
        // FX was authored to span hull->ground from the OLD 470m altitude (~270m
        // drop); stretch Y so the fired beam still touches the ground from 780m.
        const float beamY = beamS * ((uy - 200.0f) / 270.0f);
        const float MF[16] = { beamS,0,0,0, 0,beamY,0,0, 0,0,beamS,0, ux, uy, uz, 1.0f };
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
    // 0.38 (was 2.2): heli_body.glb is 42m RAW — 2.2x made a 92m fuselage / 84m
    // rotor (bigger than any aircraft ever flown) that dominated every frame.
    // 0.38 = ~16m fuselage / ~14m rotor, Black Hawk scale (aircraft lane).
    const float kHeliScale  = [](){ const char* e=std::getenv("ECHO_HELI_SCALE");  return e?(float)std::atof(e):0.38f;  }();
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
    addHeli( 120.0f, 620.0f, 285.0f, 295.0f, -0.13f, 2.1f);   // wider+higher: was skimming the crown at ~13m
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
    // Declared ahead of the Car lambdas: poseCar drives the graph (see below).
    // (#34a: `roads` decl+build hoisted ABOVE the region boot — see there.)
    struct Car { std::unique_ptr<x3::game::EnvArtSystem> body;
                 float sx, sz, dx, dz, len, speed, off; };
    std::vector<Car> cars;
    // ---- LANE 1 TRAFFIC v1 (GTA punchlist #1): whole-graph kinematic routing.
    // Each car gets a seeded multi-edge ROUTE over the real RoadGraph (freeway,
    // ramps, avenues, harbor streets) with lane offsets; junction-end signals
    // hold cars on a global 10 s cycle (even edges then odd edges), and the
    // TrafficSignal hack forces ALL-RED — "SIGNAL SPOOFED - GRIDLOCK" is real.
    struct TrafficLeg   { uint32_t edge; bool fwd; float len; bool exitSignalled; };
    struct TrafficRoute { std::vector<TrafficLeg> legs; float total = 0.0f; };
    std::vector<TrafficRoute> carRoutes;         // index == cars index
    std::vector<float> carRouteDist, carLastT;   // signals need integrator state
    std::vector<uint32_t> juncNodes;             // RoadGraph nodes, degree >= 3
    float trafficSpoofT = 0.0f;                  // hack: all-red seconds left
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
        // LANE 1 TRAFFIC v1: the car follows its multi-edge ROUTE over the
        // real RoadGraph (was: one freeway edge). Distance is STATE (not
        // f(t)) because a red signal genuinely holds the car. Lane + class
        // speed derive deterministically; laneCenterOffset does the lane math.
        const size_t ci = (size_t)(&c - cars.data());
        if (roads && ci < carRoutes.size() && !carRoutes[ci].legs.empty()) {
            const auto& g = roads->graph();
            const TrafficRoute& R = carRoutes[ci];
            float dt2 = 0.0f;
            if (carLastT[ci] >= 0.0f) dt2 = std::max(0.0f, t - carLastT[ci]);
            else carRouteDist[ci] = std::fmod(std::max(0.0f, c.off), R.total);
            carLastT[ci] = t;
            // Locate the current leg.
            float d = carRouteDist[ci];
            uint32_t li = 0; float acc = 0.0f;
            while (li + 1 < (uint32_t)R.legs.size() && d > acc + R.legs[li].len) {
                acc += R.legs[li].len; ++li;
            }
            const TrafficLeg& L = R.legs[li];
            const auto& e = g.edges[L.edge];
            const float dl = std::min(std::max(d - acc, 0.0f), L.len);
            // Class speed: freeway pace, cautious ramps, street pace.
            float v = c.speed * (e.cls == x3::game::RoadClass::Freeway ? 1.0f :
                                 e.cls == x3::game::RoadClass::Ramp    ? 0.55f : 0.45f);
            // Signal: hold short of a junction exit on red (or spoofed gridlock).
            if (L.exitSignalled && L.len - dl < 14.0f) {
                const float cyc = std::fmod(t, 10.0f);
                const bool green = trafficSpoofT <= 0.0f &&
                    (((L.edge & 1u) == 0u) ? (cyc < 5.0f) : (cyc >= 5.0f));
                if (!green) v = 0.0f;
            }
            carRouteDist[ci] = std::fmod(carRouteDist[ci] + v * dt2, R.total);
            // Sample the centerline at dl along the leg (direction-aware).
            const size_t n = e.center.size();
            float fi = ((L.fwd ? dl : (L.len - dl)) / e.length) * (float)(n - 1);
            if (fi < 0.0f) fi = 0.0f;
            const size_t i0 = std::min((size_t)fi, n - 2);
            const float  ft = fi - (float)i0;
            const auto&  s0 = e.center[i0];
            const auto&  s1 = e.center[i0 + 1];
            const float  x0 = s0.x + (s1.x - s0.x) * ft;
            const float  y0 = s0.y + (s1.y - s0.y) * ft + 0.35f;
            const float  z0 = s0.z + (s1.z - s0.z) * ft;
            float tx = s0.tx + (s1.tx - s0.tx) * ft, tz = s0.tz + (s1.tz - s0.tz) * ft;
            const float tl = std::sqrt(tx*tx + tz*tz); if (tl > 1e-5f) { tx /= tl; tz /= tl; }
            const int lane = (int)(ci & 1u) < (L.fwd ? e.lanesF : e.lanesB) ? (int)(ci & 1u) : 0;
            const float lat = x3::game::RoadGraph::laneCenterOffset(e, lane, L.fwd);
            const float x = x0 + tz * lat, z = z0 - tx * lat;   // right-perp = (tz,-tx)
            const float dirX = L.fwd ? tx : -tx, dirZ = L.fwd ? tz : -tz;
            const float heading = std::atan2(dirX, dirZ) + kCarYaw;
            const float sc = kCarScale, ch = std::cos(heading), sh = std::sin(heading);
            const float M[16] = { ch*sc,0,-sh*sc,0, 0,sc,0,0, sh*sc,0,ch*sc,0, x, y0, z, 1 };
            c.body->setInstanceTransform(0, M);
            return;
        }
        // Fallback (roads failed to build): the legacy straight lane at crown height.
        const float d = std::fmod(c.off + t * c.speed, c.len);
        const float x = c.sx + c.dx * d;
        const float z = c.sz + c.dz * d;
        const float heading = std::atan2(c.dx, c.dz) + kCarYaw;   // +Z-forward faces travel dir
        const float s = kCarScale, ch = std::cos(heading), sh = std::sin(heading);
        const float M[16] = { ch*s,0,-sh*s,0, 0,s,0,0, sh*s,0,ch*s,0, x, kCarY, z, 1 };
        c.body->setInstanceTransform(0, M);
    };
    // Route construction: seeded walks over the RoadGraph. Called once after
    // roads->build (adjacency comes from the edges' positional node joins).
    auto buildTrafficRoutes = [&](){
        if (!roads) return;
        const auto& g = roads->graph();
        const uint32_t ne = (uint32_t)g.edges.size();
        if (!ne || cars.empty()) return;
        // RoadNodes are POSITIONAL joins (no shared indices across edges) —
        // cluster them within 10 m into supernodes before adjacency.
        std::vector<uint32_t> super(g.nodes.size());
        std::vector<std::pair<float,float>> superPos;
        for (uint32_t ni = 0; ni < (uint32_t)g.nodes.size(); ++ni) {
            uint32_t s = (uint32_t)superPos.size();
            for (uint32_t sj = 0; sj < (uint32_t)superPos.size(); ++sj) {
                const float dx = superPos[sj].first - g.nodes[ni].x;
                const float dz = superPos[sj].second - g.nodes[ni].z;
                if (dx * dx + dz * dz < 100.0f) { s = sj; break; }
            }
            if (s == (uint32_t)superPos.size())
                superPos.push_back({ g.nodes[ni].x, g.nodes[ni].z });
            super[ni] = s;
        }
        std::vector<std::vector<uint32_t>> adj(superPos.size());
        for (uint32_t ei = 0; ei < ne; ++ei) {
            const auto& e = g.edges[ei];
            if (e.center.size() < 3) continue;
            if (e.a < super.size()) adj[super[e.a]].push_back(ei);
            if (e.b < super.size() && super[e.b] != super[e.a])
                adj[super[e.b]].push_back(ei);
        }
        juncNodes.clear();
        for (uint32_t ni = 0; ni < (uint32_t)adj.size(); ++ni)
            if (adj[ni].size() >= 3) juncNodes.push_back(ni);
        carRoutes.assign(cars.size(), {});
        carRouteDist.assign(cars.size(), 0.0f);
        carLastT.assign(cars.size(), -1.0f);
        uint32_t routed = 0;
        for (size_t ci = 0; ci < cars.size(); ++ci) {
            uint32_t rng = (uint32_t)(ci * 2654435761u) | 1u;
            auto rnd = [&rng](){ rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; };
            uint32_t ei = rnd() % ne;
            for (uint32_t k = 0; k < ne && g.edges[ei].center.size() < 3; ++k) ei = (ei + 1) % ne;
            if (g.edges[ei].center.size() < 3) continue;
            bool fwd = (rnd() & 1u) != 0u;
            TrafficRoute R;
            for (int leg = 0; leg < 10 && R.total < 1500.0f; ++leg) {
                const auto& e = g.edges[ei];
                const uint32_t rawExit = fwd ? e.b : e.a;
                const uint32_t exitS = rawExit < super.size() ? super[rawExit]
                                                              : (uint32_t)adj.size();
                const bool sig = exitS < adj.size() && adj[exitS].size() >= 3;
                R.legs.push_back({ ei, fwd, e.length, sig });
                R.total += e.length;
                if (exitS >= (uint32_t)adj.size() || adj[exitS].empty()) break;
                const auto& out = adj[exitS];
                uint32_t next = ei;
                for (int tries = 0; tries < 4; ++tries) {
                    const uint32_t cand = out[rnd() % out.size()];
                    if (cand != ei || out.size() == 1) { next = cand; break; }
                }
                if (next == ei && out.size() > 1) next = (out[0] == ei) ? out[1] : out[0];
                if (next == ei || g.edges[next].center.size() < 3) break;
                fwd = (g.edges[next].a < super.size() &&
                       super[g.edges[next].a] == exitS);
                ei = next;
            }
            if (!R.legs.empty()) { carRoutes[ci] = std::move(R); ++routed; }
        }
        x3::logInfo("[traffic] " + std::to_string(routed) + "/" +
                    std::to_string(cars.size()) + " cars routed over the graph (" +
                    std::to_string(juncNodes.size()) +
                    " junction nodes, 10 s signal cycle; spoof hack = gridlock)");
    };

    // ===================== CITY INFRASTRUCTURE (roads / freeway / metro) =====
    // The cars/couriers used to drive on bare terrain. Lay real surfaces: asphalt +
    // neon-curb streets down the exact car lanes; an ELEVATED concrete freeway on the
    // courier arc; and an ELEVATED metro line with a sliding train — all on pillars so
    // the layers read as real infrastructure from the vista. Flat plane GLBs (assets/
    // infra) scaled per segment; the pillar is a unit cube stretched to the ground.
    std::vector<std::unique_ptr<x3::game::EnvArtSystem>> infra;
    // (TIER-2 M-A: subwayTrain/subwayBuilt/subwayLine moved into buildCrown.)
    const std::string infradir = "D:/GameDev/EchoHarbor/assets/infra";
    // Place a flat plane GLB (local 1x1 in X/Z, +Y up) as a road/deck: centre (cx,cz),
    // height y, oriented yaw=atan2(dir.x,dir.z), scaled width(X) x length(Z).
    auto placeDeck = [&](const char* glb, float cx, float cz, float y, float yaw,
                         float width, float len){
        const float c=std::cos(yaw), s=std::sin(yaw);
        const float T[16] = { c*width,0,-s*width,0,  0,1,0,0,  s*len,0,c*len,0,  cx, y, cz, 1 };
        auto e = std::make_unique<x3::game::EnvArtSystem>();
        if (e->buildFromGlbAt(*device, infradir, glb, T)) infra.push_back(std::move(e));
    };
    // PITCHED deck: rises `dy` over its length so sloped freeway legs read as a
    // continuous graded ribbon instead of stair-stepped flats (whose risers read
    // as gaps from the air — the night lane's "dashes" finding).
    auto placeDeckP = [&](const char* glb, float cx, float cz, float y, float yaw,
                          float width, float len, float dy){
        const float c=std::cos(yaw), s=std::sin(yaw);
        const float L = std::sqrt(len*len + dy*dy);
        const float cp = len / L, sp = dy / L;
        const float T[16] = { c*width, 0.0f, -s*width, 0.0f,
                              -s*sp,   cp,   -c*sp,    0.0f,
                              s*cp*L,  sp*L,  c*cp*L,  0.0f,
                              cx, y, cz, 1.0f };
        auto e = std::make_unique<x3::game::EnvArtSystem>();
        if (e->buildFromGlbAt(*device, infradir, glb, T)) infra.push_back(std::move(e));
    };
    // A concrete support column from ground up to just under deck height topY.
    auto placePillar = [&](float x, float z, float topY, float w){
        const float gy = hf.ok()?hf.heightAt(x,z):kCarY;
        const float h  = std::max(2.0f, topY - gy);
        const float T[16] = { w,0,0,0, 0,h,0,0, 0,0,w,0, x, gy + h*0.5f, z, 1 };
        auto e = std::make_unique<x3::game::EnvArtSystem>();
        if (e->buildFromGlbAt(*device, infradir, "pillar.glb", T)) infra.push_back(std::move(e));
    };
    {
        // (TIER-2 M-A: crown STREETS moved to buildCrown — echo_region_builders.cpp.)
        // RETIRED (Tim: "Roads are still 3rd grade... they curve, they connect,
        // they transition"): the straight-segment FREEWAY NETWORK (placeDeckP
        // loop over kRoute + twin pillar rows) is replaced by the EchoRoads
        // module — a Catmull-Rom banked 2+2 deck over the SAME ten waypoints,
        // with jersey barriers, real lane paint, trumpet interchanges, avenues,
        // the shore-probed Harbor Boulevard, and five fanned harbor grid blocks
        // (Tim's sketch). placeDeckP/placePillar stay for future one-off decks.
        (void)placeDeckP; (void)placePillar;
        // #34a: roads already built pre-regions; only the traffic routes
        // wait until here (they need the car fleet declared above).
        if (roads) buildTrafficRoutes();   // LANE 1: the fleet takes the whole graph
    }

    // (TIER-2 M-A: LIVING CONDOS moved to buildCrown — echo_region_builders.cpp.)

    // (TIER-2 M-A: METROPOLIS DISTRICTS + districtLights + appendDistrictLights
    //  moved to the 3 district_* regions — echo_region_builders.cpp mesh pass +
    //  echo_woodlands.cpp light harvest; per-frame selection is now
    //  regionSet.appendNearLights (same nearest-first partial_sort + 64 budget).)

    // (TIER-2 M-A: HACKABLES props/drone/VTOL + their poses moved to buildCrown.)

    // ===================== DISTRICT STREET LAMPS (night readability) ========
    // The pack districts carry only sparse baked neon — at night they read as
    // black shells. Real street lamps (StreetLights: post+arm+head meshes, fake-
    // volumetric cones, ground pools, flicker) in warm rows down each district's
    // main streets. Nearest lamps feed setPointLights so walls catch the glow.
    x3::game::Scene lampScene;
    x3::game::StreetLights streetLamps;
    {
        // Pools seat on LOCAL terrain (crown drag undulates; row-y buried them).
        streetLamps.setGroundQuery([&hf](float x, float z) {
            return hf.ok() ? hf.heightAt(x, z) : 190.0f;
        });
        auto seatOf = [&](float cx, float cz){
            float gy = hf.ok() ? hf.heightAt(cx, cz) : 190.0f;
            if (hf.ok()) {
                for (int sx = -1; sx <= 1; ++sx) for (int sz = -1; sz <= 1; ++sz)
                    gy = std::max(gy, hf.heightAt(cx + sx*180.0f, cz + sz*180.0f));
                gy += 0.4f;
            }
            return gy;
        };
        const float rs = seatOf(950.0f, 1250.0f);    // Recife pad seat
        const float us = seatOf(700.0f, 350.0f);     // Urban bay pad seat
        const float hs = seatOf(1340.0f, 1000.0f);   // HIVEMIND pad seat
        // Crown drag + harbor boulevard are REAL local terrain (not raised
        // district pads) — the 3x3 max probe seats their lamps meters in the
        // air (first capture: cones floating over a black street). Flat seat.
        const float cs = (hf.ok() ? hf.heightAt(0.0f, 748.0f) : 195.0f) + 0.4f;
        const float hb = (hf.ok() ? hf.heightAt(200.0f, 366.0f) : 0.0f) + 0.4f;
        const float rows[][6] = {
            { 975.0f, 1222.0f, 1090.0f, 1222.0f, rs, 26.0f },   // Recife alley N row
            { 975.0f, 1258.0f, 1090.0f, 1258.0f, rs, 26.0f },   // Recife alley S row
            { 560.0f,  350.0f,  840.0f,  350.0f, us, 30.0f },   // Urban main drag E-W
            { 700.0f,  240.0f,  700.0f,  470.0f, us, 30.0f },   // Urban cross street N-S
            {1240.0f, 1000.0f, 1440.0f, 1000.0f, hs, 30.0f },   // HIVEMIND main street
            // LANE 2 (WD2 punchlist #3): the black-ground night fix — the crown
            // drag/plaza + harbor boulevard carried ZERO lamps; every night
            // capture at the drag stood in the dark.
            { -85.0f,  748.0f,   95.0f,  748.0f, cs, 26.0f },   // CROWN main drag E-W
            {  12.0f,  705.0f,   12.0f,  800.0f, cs, 28.0f },   // CROWN plaza cross N-S
            { 140.0f,  366.0f,  265.0f,  366.0f, hb, 28.0f },   // HARBOR boulevard
        };
        streetLamps.buildDistrictLamps(lampScene, *device, rows, 8);
    }

    // (TIER-2 M-A: HARBOR BOATS + poseBoat moved to buildHarborBay.)

    // (TIER-2 M-A: SKY DRONES + poseDrone moved to buildCrown.)

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

    // ================= CITIZEN TALK: the local LLM brain =====================
    // Created BEFORE the NpcLife build so the living-city layer gets it too (it
    // uses the same system for per-instance detail flavor). MODELLESS-SAFE: if no
    // .gguf is present (or ECHO_TALK=0), `talkLlm` stays null and EVERY other
    // feature in this host — walk mode, ride-along, the resident card, build mode
    // — behaves exactly as before; only the conversation is unavailable.
    std::unique_ptr<x3::llm::ILlmSystem> talkLlm;
    {
        const bool talkOff = [](){ const char* e = std::getenv("ECHO_TALK"); return e && e[0]=='0'; }();
        const std::string mp = talkOff ? std::string() : resolveTalkModelPath();
        if (mp.empty()) {
            x3::logInfo(std::string("--world echotropolis: [talk] LLM OFF — ") +
                        (talkOff ? "ECHO_TALK=0"
                                 : "no .gguf in " + x3::game::assetRoot() +
                                   "/models/llm (see assets/models/llm/README.md)") +
                        "; residents stay silent, everything else unchanged");
        } else {
            // SPEED-FIRST generation opts. 48 output tokens is ~1-2 sentences —
            // exactly the length a street exchange wants — and it hard-bounds the
            // worst-case reply time. 1024 ctx is plenty for a short persona + a
            // handful of turns and keeps the per-chat KV allocation small.
            x3::llm::ModelOpts lo;
            lo.contextTokens   = 1024;
            lo.maxOutputTokens = 48;
            lo.temperature     = 0.85f;
            if (const char* mt = std::getenv("ECHO_TALK_MAXTOK"); mt && mt[0])
                lo.maxOutputTokens = std::max(8, std::atoi(mt));
            const double t0 = glfwGetTime();
            talkLlm = x3::llm::createLlmSystem();
            if (!talkLlm->loadModel(mp, lo)) {
                talkLlm.reset();
                x3::logWarn("--world echotropolis: [talk] model load FAILED (" + mp + ") — residents stay silent");
            } else {
                x3::logInfo("--world echotropolis: [talk] LLM READY  model=" + mp +
                            "  load=" + std::to_string((int)((glfwGetTime() - t0) * 1000.0)) + "ms" +
                            "  maxtok=" + std::to_string(lo.maxOutputTokens) +
                            "  backend=" + talkLlm->backendName());
            }
        }
    }

    // Live conversation state (ONE chat at a time — the concurrency cap).
    struct TalkState {
        int         agent   = -1;      // NpcLife agent index we're talking to
        uint32_t    chat    = 0;       // x3::llm::ChatId (0 == none)
        std::string question;          // what we last asked
        std::string reply;             // streamed reply so far
        bool        pending = false;   // generation in flight
        double      askedAt = 0.0;     // submit timestamp (for TTFT / total)
        double      firstAt = 0.0;     // first-token timestamp (0 == none yet)
        double      endAt   = 0.0;     // when the bubble should start fading out
        int         tokens  = 0;
        int         polls   = 0;   // token-bearing polls (streaming granularity)
    } talk;
    int  talkPromptIdx = 0;
    // AMBIENT CHATTER (ECHO_TALK_AMBIENT=1, default OFF): every so often an idle
    // citizen near you mutters one unprompted line. Default-off on purpose — this
    // is CPU inference sharing the box with the renderer, and the PLAYER's
    // conversation always gets priority (an in-flight ambient line is cancelled the
    // moment you press T).
    const bool ambientOn = [](){ const char* e = std::getenv("ECHO_TALK_AMBIENT"); return e && e[0]=='1'; }();
    struct AmbientState {
        int         agent   = -1;
        uint32_t    chat    = 0;
        std::string line;
        bool        pending = false;
        double      nextAt  = 0.0;   // next spawn attempt
        double      endAt   = 0.0;
    } amb;
    constexpr float kAmbientHoldSec  = 8.0f;
    // Tim: "let them talk — in a FEED — at human texting/speech speed." A mutter
    // every ~20s, revealed at ~22 chars/sec, every line archived to the feed.
    constexpr float kAmbientEverySec = 20.0f;
    std::vector<std::string> talkFeed;          // rolling city-voices feed (last 6)
    int fedAmbAgent = -1; std::string fedAmbLine;
    constexpr float kBubbleHoldSec = 14.0f;   // max on-screen duration after `done`
    constexpr float kBubbleFadeSec = 0.8f;
    constexpr float kTalkDropRange = 70.0f;   // walk this far away and the chat closes

    // ===================== Headless screenshot path =====================
    // Pose the default orbit (17deg, radius 70), settle the waves a few frames so
    // the Gerstner surface isn't flat, then arm+grab. Mirrors host_valley's grab.
    if (headless || screenshot) {
        // --set OVERRIDES IN THE CAPTURE PATH. This host's console — and every
        // registerCVar in it — lives in the INTERACTIVE branch far below, which
        // a screenshot run returns long before reaching. So `--set` has never
        // had ANY effect on a capture from this world: every headless cvar A/B
        // taken here compared an image to itself. Rather than stand a whole
        // console up in the capture path, read the overrides the capture needs.
        auto cliVal = [&](const char* name, float dflt) {
            for (const auto& kv : hc.cliCVars)
                if (kv.first == name) return (float)std::atof(kv.second.c_str());
            return dflt;
        };
        {
            x3::rhi::IRenderDevice::WetnessParams wt{};
            wt.amount   = cliVal("r_wetness", 0.0f);
            wt.porosity = cliVal("r_wetness_porosity", 1.0f);
            wt.puddles  = cliVal("r_wetness_puddles", 1.0f);
            wt.minRough = cliVal("r_wetness_minrough", 0.06f);
            device->setWetness(wt);
            x3::logInfo("--world echotropolis: WETNESS amount " +
                        std::to_string(wt.amount) + " (capture path)");
        }
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
                sNpc.build(lc, sScene, *device, nullptr, talkLlm.get());
                sNpcBuilt = sNpc.built();
                x3::logInfo(std::string("--world echotropolis: SHOT NpcLife ") +
                            (sNpcBuilt ? "built (living-city archetypes)" : "FAILED"));
                x3::logInfo(std::string("--world echotropolis: SHOT residents ") +
                            (sResBuilt ? "built" : "FAILED"));
            }
        }
        // ---- ECHO_TALKDEMO=1: prove the CHAT BUBBLE in a headless capture -------
        // A --shot-cam capture cannot press T, so this force-opens a conversation
        // with a citizen at startup, frames the camera on them, streams the reply
        // through the settle loop and draws the real bubble into the grabbed frame.
        const bool talkDemo = [](){ const char* e = std::getenv("ECHO_TALKDEMO"); return e && e[0]=='1'; }();
        int      demoAgent = -1;
        uint32_t demoChat  = 0;
        std::string demoReply, demoQuestion;
        bool     demoPending = false;
        double   demoAsked = 0.0, demoFirst = 0.0;
        int      demoTokens = 0;
        if (talkDemo && talkLlm && sNpcBuilt && sNpc.agentCount() > 0) {
            // Pick the citizen closest to the district centre (a reliable, lit spot).
            float bd2 = 1e30f;
            for (uint32_t i = 0; i < sNpc.agentCount(); ++i) {
                const auto& a = sNpc.agent(i);
                const float dx = a.pos.x + 20.0f, dz = a.pos.z - 760.0f;
                const float d2 = dx*dx + dz*dz;
                if (d2 < bd2) { bd2 = d2; demoAgent = (int)i; }
            }
            if (demoAgent >= 0) {
                const auto& a = sNpc.agent((uint32_t)demoAgent);
                demoChat = talkLlm->startChat(talkPersonaPrompt(a));
                demoQuestion = kTalkPrompts[0];
                if (demoChat != x3::llm::kInvalidChat && talkLlm->submit(demoChat, demoQuestion)) {
                    demoPending = true; demoAsked = glfwGetTime();
                    x3::logInfo("[talk] TALKDEMO ask " + a.name + " (" +
                                x3::game::archetypeName(a.arch) + "): \"" + demoQuestion + "\"");
                } else demoAgent = -1;
            }
        } else if (talkDemo) {
            x3::logWarn("[talk] TALKDEMO requested but the LLM/NpcLife is unavailable "
                        "(needs ECHO_RESIDENTS=1 and a .gguf in assets/models/llm)");
        }
        int kSettle = shotResidents ? 90 : 24;   // drain skin spawns
        const int kSettleMax = talkDemo ? 4000 : kSettle;   // TALKDEMO waits out the reply
        const float dt = 1.0f / 60.0f;
        // ---- WHY THE BEACHED GALLEON SURVIVED TWO FILINGS -------------------
        // The capture rig cannot see the harbour fleet. regionSet.drawAll()/
        // updateAll() are gated on the WorldStreamer's residency state (M-B,
        // bindStreamerForDrawGate), and the streamer is only ever TICKED in the
        // live loop — never here. So every `--screenshot` of this world renders
        // the streamed regions' content according to a streamer that has never
        // run: the boats are neither posed nor drawn. Every screenshot-based
        // audit of "do vessels cross land" was therefore looking at an empty
        // bay, which is exactly how a flood-fill on paper could disagree with
        // Tim's monitor. Two opt-in levers, both default-off (no existing
        // reference capture moves):
        //   ECHO_SHOT_STREAMED=1  drop the draw gate for the still, so
        //                         force-resident region content actually renders
        //   ECHO_SHOT_T=<sec>     offset the pose clock, so a still can be
        //                         composed anywhere in the traffic cycle instead
        //                         of only at t=0.38 s
        const bool shotStreamed = [](){ const char* e=std::getenv("ECHO_SHOT_STREAMED"); return e && *e=='1'; }();
        const float shotT = [](){ const char* e=std::getenv("ECHO_SHOT_T"); return e?(float)std::atof(e):0.0f; }();
        if (shotStreamed) {
            regionSet.bindStreamerForDrawGate(nullptr);
            x3::logInfo("--world echotropolis: ECHO_SHOT_STREAMED — region draw gate dropped for the capture "
                        "(streamed content is otherwise INVISIBLE in stills; the streamer never ticks here)");
        }
        if (shotT != 0.0f)
            x3::logInfo("--world echotropolis: ECHO_SHOT_T — pose clock offset " +
                        std::to_string(shotT) + " s");
        const x3::game::TodSample shotTod = tod.sample();   // frozen at ECHO_TOD
        for (int i = 0; i < kSettle; ++i) {
            glfwPollEvents();
            applyTodSample(device, shotTod);
            applyAtmosphere(device, shotTod);   // ATMOSPHERE: aerial haze + grade + bloom
            // SAME CLOCK AS THE REGION POSES (see updateAll below, which is fed
            // `shotT + i*dt`). It used to get `i*dt`, so with ECHO_SHOT_T set
            // the hulls were posed on a wave surface the frame did not draw —
            // now that hulls actually ride the surface, that would have made
            // every ECHO_SHOT_T still a lie about where the boats sit.
            applyOcean(device, shotT + (float)i * dt, shotTod, shotCam[1]);   // shot cam height
            if (sResBuilt) { sCrowd.update(dt, sScene); sSkin.update(dt, sCrowd, sScene, *device, *sphys); }
            if (sMinersBuilt) { sMiners.update(dt, sScene); sMinersSkin.update(dt, sMiners, sScene, *device, *sphys); }
            if (sOh1Built) { flyOh1(sOh1, (float)i * dt); sOh1.update(dt, sScene, *sphys, sOh1.pos()); }
            if (sNpcBuilt) sNpc.update(dt, sScene);   // living-city schedules advance
            // TALKDEMO: drain streamed tokens + hold the settle loop open until the
            // reply lands (then a few more frames so the bubble is fully composed).
            if (demoAgent >= 0 && talkLlm && demoPending) {
                const x3::llm::PollResult pr = talkLlm->poll(demoChat);
                if (!pr.newTokens.empty()) {
                    if (demoFirst == 0.0) {
                        demoFirst = glfwGetTime();
                        x3::logInfo("[talk] TALKDEMO first token in " +
                                    std::to_string((int)((demoFirst - demoAsked) * 1000.0)) + " ms");
                    }
                    demoReply += pr.newTokens; demoTokens += pr.newTokenCount;
                }
                if (pr.done) {
                    demoPending = false;
                    const double tot = glfwGetTime() - demoAsked;
                    x3::logInfo("[talk] TALKDEMO reply: " + std::to_string(demoTokens) +
                                " tok in " + std::to_string((int)(tot * 1000.0)) + " ms (" +
                                std::to_string((int)(tot > 0.0 ? (double)demoTokens / tot : 0.0)) +
                                " tok/s)" + (pr.failed ? " [FAILED]" : "") + "  \"" + demoReply + "\"");
                    if (i + 4 > kSettle - 1) kSettle = i + 5;   // a few frames to compose
                } else if (i >= kSettle - 2 && kSettle < kSettleMax) {
                    kSettle = i + 3;                            // keep waiting
                }
            }
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
            if (demoAgent >= 0) {
                // TALKDEMO frames the citizen head-on at conversation distance so the
                // bubble is guaranteed on screen (they walk their schedule, so re-aim).
                const auto& a = sNpc.agent((uint32_t)demoAgent);
                const float cx = a.pos.x + std::cos(a.yaw) * 4.2f;
                const float cz = a.pos.z + std::sin(a.yaw) * 4.2f;
                const float cy = a.pos.y + 1.75f;
                const float tx = a.pos.x, ty = a.pos.y + 1.55f, tz = a.pos.z;
                const float dx = tx - cx, dy = ty - cy, dz = tz - cz;
                const float len = std::sqrt(dx*dx + dy*dy + dz*dz);
                device->setCamera(cx, cy, cz, std::atan2(dz, dx),
                                  len > 1e-4f ? std::asin(dy / len) : 0.0f, opt.fovDeg);
            } else if (shotCamOverride) {
                device->setCamera(shotCam[0], shotCam[1], shotCam[2], shotCam[3], shotCam[4], opt.fovDeg);
            } else {
                applyOrbitCamera(device, rig, rig.sYaw, rig.sPitch, opt.fovDeg, opt.minCamHeight);
            }
            // LIGHTS BEFORE THE FRAME: setPointLights must precede beginFrame or
            // it only reaches draws issued after the call — the district walls
            // (drawn first) got zero lamp light while the lamp posts themselves
            // lit up. Debug view 2 caught it: only the fixtures glowed.
            // Per-frame STATE (lights/camera/sky/fog/...) is read ONCE inside
            // endFrame -> prepareFrameData and blitted to one frame-global UBO
            // that every draw samples, so its position relative to beginFrame and
            // to the draws is a NO-OP (verified by byte-comparing captures).
            // Placed here purely for readability.
            // ⚠ The INVERSE is a real bug: draw/submit calls (drawMesh*, drawHud*,
            // submitParticles/Decals) are CLEARED by beginFrame — never hoist one.
            streetLamps.update(dt, lampScene);                   // flicker machines
            {
                std::vector<x3::rhi::PointLight> pls;
                streetLamps.selectLights(shotCam[0], shotCam[1], shotCam[2], pls, 8);
                regionSet.appendNearLights(shotCam[0], shotCam[2], pls, 64);   // TIER-2: was appendDistrictLights
                device->setPointLights(pls.empty() ? nullptr : pls.data(), (uint32_t)pls.size());
            }
            if (i == kSettle - 1) device->armCapture(outPath.c_str());
            auto frame = device->beginFrame();
            island.draw(*device, frame);   // the island (sky + water are device-internal)
            props.draw(*device, frame);    // P4 coast dressing (lighthouse/dock/boats/skyline)
            // TIER-2 M-A: houses/towers/mine/streets+metro/condos/districts/hackables/
            // mineForest/woodlands/mineGlow/subway/boats/drones draw via the regions.
            // updateAll uses the settle-loop clock EXACTLY like the old pose calls.
            regionSet.updateAll(dt, shotT + (float)i * dt);
            regionSet.drawAll(*device, frame);
            for (auto& r : infra) r->draw(*device, frame);       // freeway decks (host-persistent)
            lampScene.render(*device, frame);                    // posts + cones + pools
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
            if (roads) roads->draw(*device, frame);   // ECHO ROADS (headless parity)
            if (roads && shotTod.cityLightsOn) roads->drawNightGlow(*device, frame);   // V7 glow parity
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
            // TALKDEMO: the real chat bubble over the real citizen's head.
            if (demoAgent >= 0) {
                const auto& a = sNpc.agent((uint32_t)demoAgent);
                float sx = 0.0f, sy = 0.0f;
                if (device->worldToScreen(a.pos.x, a.pos.y + 2.2f, a.pos.z, sx, sy)) {
                    std::string body = talkTidy(demoReply);
                    if (body.empty()) body = demoPending ? "..." : "(silence)";
                    else if (demoPending) body += " ...";
                    std::vector<std::string> wrapped;
                    talkWrap(*device, body, 15.0f, 340.0f, 5, wrapped);
                    drawTalkBubble(*device, frame, sx, sy, a.name.c_str(), wrapped, 1.0f);
                } else if (i == kSettle - 1) {
                    x3::logWarn("[talk] TALKDEMO: worldToScreen rejected the head anchor "
                                "(citizen off-screen/behind camera) — no bubble in this frame");
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
    glfwSetCharCallback(window, charCB);   // console text input (inert while closed)
    double lastMX, lastMY; glfwGetCursorPos(window, &lastMX, &lastMY);
    double prevTime = glfwGetTime();
    float  waterTime = 0.0f;

    // FPS accounting (log once per second — there is no HUD text API in this host).
    double fpsAccum = 0.0; int fpsFrames = 0;
    double hudFps = 0.0;   // last measured FPS, shown in the top HUD bar (Tim's ask)
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

    // ===================== WD2 STACK (Lane 3) ============================
    // City-wide hackables + heat + karma + the RPG progression layer. The
    // registry is declared HERE — before the NpcLife build — so every citizen
    // registers a scan-card into it; street objects are placed after the
    // districts exist (the carDefs pattern). Sinks wire to the REAL systems
    // below (AlertSystem heat, TimelineState karma, StreetLights blackout,
    // treasury credits). H holds the nethack reveal; E (while aiming) hacks;
    // K opens the skill tree (console's frozen-frame modal pattern).
    // (hax hoisted before the region boot — WD2 camera saturation.)
    x3::game::AlertSystem      cityAlert;
    x3::game::TimelineState    cityTimeline;
    x3::game::Progression      progression;
    x3::game::SkillTree        skillTree;
    x3::game::RpgUi            rpgUi;
    x3::game::PlayerStatMods   rpgMods;
    x3::game::Inventory        rpgInv;     // empty v1 — the chip reads LV/XP
    x3::game::ItemDb           rpgItems;
    std::vector<x3::phys::Vec3> hackPropPos;   // hackable entity-id -> world pos
    x3::game::HackResult hackCard;             // last hack result (confirm card)
    float hackCardT = 0.0f;                    // seconds the card stays up
    float driveXpOdo = 0.0f;                   // meters banked toward drive XP
    bool  prevRpgK = false, prevRpgUp = false, prevRpgDown = false,
          prevRpgLeft = false, prevRpgRight = false, prevRpgEnter = false,
          prevHackE = false;
    skillTree.load(x3::game::skillTreeJsonPath());
    static constexpr const char* kRpgSavePath = "echotropolis.rpg.txt";
    static constexpr int kXpHack = 30, kXpDodog = 10, kXpDriveLeg = 25;
    static constexpr float kDriveLegMeters = 400.0f;

    x3::logInfo("--world echotropolis: LMB/MMB-drag or WASD pan (grab-the-ground), "
                "wheel zoom, RMB-drag or Q/E orbit-rotate; TOD keys 1=golden 2=dusk "
                "3=night 4=noon, T pauses the cycle; Esc to quit");
    // Start with the day-night cycle PAUSED so the launch HOLDS its ECHO_TOD light
    // (golden by default) instead of sprinting into night in ~20s — press T to run time.
    // Tim ("sun not moving across the sky"): the sky clock now RUNS by default in
    // interactive play; it boots frozen only when a fixed TOD was requested via
    // ECHO_TOD (the capture/byte-compare harnesses depend on that stillness).
    // T still toggles either way.
    bool todPaused = (std::getenv("ECHO_TOD") != nullptr), prevT = false;
    x3::game::TodPhase prevPhase = tod.phase();

    // ESC opens a PAUSE MENU (it never quits the app — Tim's rule 2026-07-11). Only the
    // menu's QUIT item (click, or press Q) exits. Edge-detected so a held key toggles once.
    bool menuOpen = false, prevEsc = false, prevQ = false, prevEnter = false, prevLmb = false;
    // ENGINE CONSOLE: the D6 backend + the shared Hud front-end (same as EFLZ).
    std::unique_ptr<x3::con::IConsole> console(x3::con::createConsole());
    x3::game::Hud hud;
    bool conQuit = false;
    hud.init(*console, &conQuit);
    g_hud = &hud;
    bool prevGrave = false, prevConBk = false, prevConEnter = false,
         prevConUp = false, prevConDown = false, prevConTab = false;
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

        // v3 ROAD COLLISION (Tim: "you can fall through" the bridge): the roads
        // module exports its asphalt top surface; the body is OURS to create
        // (module INTEGRATION §8) — same static-mesh path as the terrain above.
        // Runs here (not at roads->build) because phys is born in this block.
        if (roads) {
            const auto& rc = roads->collisionMesh();
            if (!rc.verts.empty() && !rc.indices.empty()) {
                phys->addStaticMesh(rc.verts.data(), (uint32_t)(rc.verts.size() / 3),
                                    rc.indices.data(), (uint32_t)rc.indices.size());
                x3::logInfo("--world echotropolis: road collision mesh built (" +
                            std::to_string(rc.indices.size() / 3) + " tris)");
            }
        }

        // SWIM: the surface is kEchoSeaLevelY — the SAME datum the Gerstner
        // patch is drawn at, so the water you swim in and the water you see are
        // the same plane (they were 0.10 m apart). The -0.30 threshold is kept
        // but is now stated as a DEPTH below the datum, not an absolute height:
        // ankle-deep water is not a swim. Deeply-negative elsewhere = dry land.
        player.setWaterQuery([&hf](float x, float z) -> float {
            return (hf.ok() && hf.heightAt(x, z) < x3::game::echoSwimFloorY())
                 ? x3::game::kEchoSeaLevelY : -1.0e30f;
        });
        player.spawn(*phys, kWalkX, kWalkGroundY + 1.0f, kWalkZ);
        player.setLook(2.2f, -0.05f);   // face toward the city cluster
    }

    // ===================== RESIDENTS (the living city) ==================
    // A crowd of citizens wandering the crown, rendered as the REAL rigged-GLB
    // characters (66 in riggedGlbRoot()) via CrowdSkin over the blockout agents.
    // Visible from the orbit vista (watch them move) AND up close in walk mode.
    // These are the lives you will step into (play-as) in a later phase.
    // (TIER-2 M-A: walkScene decl HOISTED to the region boot block —
    //  EchoRegionCtx binds it; everything below populates it unchanged.)
    x3::game::CrowdSystem residents;
    x3::game::CrowdSkin residentsSkin;
    bool residentsBuilt = false;
    x3::game::CrowdSystem miners;          // dedicated GOLD-MINE crew (western shoulder)
    x3::game::CrowdSkin minersSkin;
    bool minersBuilt = false;
    x3::game::MonsterSystem oh1; bool oh1Built = false;   // OH1 hero heli (rigged prop)
    x3::game::HoloTerminal opsScreen;   // CONTROL ROOM: live city-ops dashboard on the crown
    x3::game::NpcLife npcLife; bool npcLifeBuilt = false;   // LIVING CITY: scheduled NPCs
    x3::game::NpcSkin npcSkin;                              // their rigged bodies
    x3::game::WorldCars worldCars;                          // CARS PILLAR: the street fleet
    // NFS LAYER (Lane 1): the EFLZ performance shop, wired to the street
    // fleet. Roll onto the lift pad, stop, and the shop takes the frame:
    // engine/brakes/tires/nitrous tiers land on the live Jolt rig via
    // ComposedBuild::tuning (1:1 WheeledTuning). Treasury is the wallet.
    x3::game::vehparts::Catalog      partsCat;
    x3::game::vehparts::VehicleBuild carBuild;
    x3::game::PerfShop               shop;
    bool shopBuilt = false;
    bool prevShopUp = false, prevShopDown = false, prevShopEnter = false,
         prevShopBack = false, prevShopTab = false, prevShopL = false,
         prevShopR = false, prevShopP = false, prevShopFix = false,
         prevShopN = false;
    const char* vendorPrompt = nullptr;                     // vendor buy-loop HUD line
    const char* doorPrompt = nullptr;                       // LANE 4: interior door line
    int playerInCell = -1;                                  // which cell the player is inside
    float driveCamYaw = 0.0f, driveCamPitch = -0.22f;       // chase-cam mouse orbit
    // NFS-STYLE VIEW CYCLE (Tim: "the mode i usualy drove in since 1992") —
    // C cycles while driving: 0 FAR chase (Tim's default), 1 MID, 2 CLOSE,
    // 3 HOOD (true dashboard view waits on a modeled interior).
    int  carView = 0; bool prevCarC = false;
    static constexpr const char* kCarViewName[4] = { "FAR CHASE", "MID CHASE", "CLOSE", "HOOD" };
    static constexpr float kCarViewDist[3] = { 1.65f, 1.0f, 0.55f };   // scale on the stock boom
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
            // LANE 4: leisure hours draw the social archetypes to the noodle
            // bar counter — patrons arrive on schedule, dwell, and leave.
            lc.leisureMagnet  = true;
            lc.leisureMagnetX = x3::game::kInteriorCells[1].doorX + 0.8f;
            lc.leisureMagnetZ = x3::game::kInteriorCells[1].doorZ + 1.4f;
            // NO LLM here (Tim's call): passing it queued a background generation
            // for EVERY citizen at build time — 23 inferences grinding the CPU
            // under the game. Authored persona details are plenty; the LLM is
            // reserved for the player-facing TALK system (visible NPC, on demand).
            // WD2 STACK: &hax lights the citizen scan-cards (the playability-wave
            // triage passed nullptr here and left them dark — tribunal item #1).
            npcLife.build(lc, walkScene, *device, &hax, nullptr);
            npcLifeBuilt = npcLife.built();
            x3::logInfo(std::string("--world echotropolis: living-city NpcLife ") +
                        (npcLifeBuilt ? "built" : "off"));
            if (npcLifeBuilt) {
                // RIGGED SKINS over the named citizens ("cube box people" fix):
                // cops + vendor wear the paid Meshy rigs, the rest the roster.
                x3::game::NpcSkinConfig nsc;
                nsc.site = "named citizens";
                nsc.spawnsPerFrame = 2;
                npcSkin.build(nsc, npcLife);
            }
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

        // ===================== CARS PILLAR (#26 — "wire the cars!!!") =========
        // The proven world_cars stack (E-enter, Jolt wheels, hold-E hack, chase
        // cam), parked along the NEW road network. All host-owned ("" region)
        // for v1 — resident from boot; region ownership can come with M-C.
        worldCars.setGroundQuery([&hf](float x, float z) {
            return hf.ok() ? hf.heightAt(x, z) : 0.0f;
        });
        worldCars.setWaterQuery([&hf](float x, float z) {
            // Terrain below the datum means water surface above it. Same datum
            // as the swim query and the drawn patch — previously this said 0.0
            // while the visible sea was at 0.10, so a car could drown in water
            // it was rendered as driving beside.
            return (hf.ok() && hf.heightAt(x, z) < x3::game::kEchoSeaLevelY)
                 ? x3::game::kEchoSeaLevelY : x3::game::kWorldWaterDry;
        });
        worldCars.setHackAlarmHook([&](const x3::phys::Vec3& hp) {
            // A hacked car alarm scatters nearby citizens (crowd + npc cops both
            // hear it through the residents crowd's violence stimulus).
            if (residentsBuilt) residents.onViolence(hp);
        });
        {
            std::vector<x3::game::WorldCarDef> carDefs;
            // THE FIRST FINDABLE CAR — crown drag, steps from spawn, unlocked.
            carDefs.push_back({ "drag_curb",   -34.0f, 748.0f,  90.0f, false, { 0.82f, 0.08f, 0.08f }, "" });
            // Crown plaza — LOCKED (the hack tutorial in lamplight).
            carDefs.push_back({ "plaza_lock",   12.0f, 771.0f, 200.0f, true,  { 0.10f, 0.32f, 0.85f }, "" });
            // Mine truck lot (end of the V5 spur cul-de-sac).
            carDefs.push_back({ "mine_lot",   -548.0f, 806.0f, 320.0f, false, { 0.90f, 0.55f, 0.10f }, "" });
            // Harbor boulevard shoulder — LOCKED getaway bait by the water.
            carDefs.push_back({ "harbor_blvd", 210.0f, 372.0f,  95.0f, true,  { 0.16f, 0.62f, 0.30f }, "" });
            // District gates (Urban north + Recife SW approach shoulders).
            carDefs.push_back({ "urban_gate",  706.0f, 396.0f, 180.0f, false, { 0.93f, 0.90f, 0.86f }, "" });
            carDefs.push_back({ "recife_gate", 838.0f, 1104.0f, 45.0f, true,  { 0.52f, 0.14f, 0.58f }, "" });
            worldCars.build(carDefs, device, *phys, x3::game::convertedGlbRoot());
            x3::logInfo("--world echotropolis: WORLD CARS parked (" +
                        std::to_string(carDefs.size()) + " on the streets; E enters, hold-E hacks)");
        }

        // NFS LAYER: the performance shop rises west of the drag. build()
        // picks the flattest candidate near the anchor; catalog missing ->
        // shop disabled with a loud log (host_drive precedent).
        if (partsCat.loadFile(x3::game::vehparts::defaultCatalogPath())) {
            carBuild.loadFile(x3::game::vehparts::defaultBuildSavePath());
            shopBuilt = shop.build(walkScene, *device, *phys, &partsCat, &carBuild,
                                   -130.0f, 726.0f);
            if (shopBuilt) shop.recompose(worldCars.liveCar());
            x3::logInfo(std::string("--world echotropolis: PERF SHOP ") +
                        (shopBuilt ? "OPEN west of the drag (roll onto the lift, stop)"
                                   : "build FAILED"));
        } else {
            x3::logWarn("--world echotropolis: parts catalog missing — perf shop disabled");
        }

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
    // ===================== FLY MODE (the default camera) =====================
    // Tim's spec: mouselook + WASD (A/D strafe), Q/E roll, SPACE up, C down,
    // arrow keys = forward/back + TURN left/right (not strafe). V toggles
    // fly <-> orbit; G (walk) exits it. Starts ON.
    bool flyMode = true, prevV = false, flySeeded = false;
    float flyX = 0.0f, flyY = 0.0f, flyZ = 0.0f;
    float flyYaw = 0.0f, flyPitch = 0.0f, flyRoll = 0.0f;
    bool cityPanelOpen = false, prevTab = false;   // CITY PANEL (TAB toggles)
    int  followIdx = -1, lastPickedIdx = -1; bool prevF = false;   // RIDE-ALONG (F)
    bool playAs = false, prevE = false; float driveYaw = 0.0f;     // PLAY-AS vs SPECTATE (E)
    bool prevTalkLB = false, prevTalkRB = false;                   // CITIZEN TALK prompt cycle ([ / ])

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
    // REAL STREET PROPS (Tim: the blockout cart "doesnt look good or realistic"):
    // textured Meshy props placed at the vendors' posts; the glowing yellow
    // blockout cart entity is hidden only when the real cart actually loads.
    std::vector<std::unique_ptr<x3::game::EnvArtSystem>> streetProps;
    // HouseForge/mine transform (col-major): yaw about Y, uniform scale, terrain-lift.
    auto buildXf = [&](float x, float z, float yaw, float s, float lift, float T[16]){
        const float gy = hf.ok() ? hf.heightAt(x, z) : 190.0f;
        const float c = std::cos(yaw), sn = std::sin(yaw);
        T[0]=c*s; T[1]=0; T[2]=-sn*s; T[3]=0;  T[4]=0; T[5]=s; T[6]=0; T[7]=0;
        T[8]=sn*s; T[9]=0; T[10]=c*s; T[11]=0; T[12]=x; T[13]=gy+lift; T[14]=z; T[15]=1;
    };

    // Place the REAL hot-dog cart (Meshy prop) at each vendor's post. Meshy
    // unrigged outputs are normalized to a 1.9 m cube centered at origin (bbox
    // meaningless): scale 1.3 reads street-furniture right, lift seats the
    // normalized bottom (-0.95 * scale) on the terrain.
    if (npcLifeBuilt) {
        for (uint32_t vi = 0; vi < npcLife.agentCount(); ++vi) {
            const auto& va = npcLife.agent(vi);
            if (va.arch != x3::game::Archetype::HotDogVendor) continue;
            float T[16];
            buildXf(va.pos.x + 1.1f, va.pos.z, va.yaw, 1.3f, 1.25f, T);
            auto cart = std::make_unique<x3::game::EnvArtSystem>();
            if (cart->buildFromGlbAt(*device, x3::game::assetRoot() + "/meshy/props",
                                     "hotdog_cart.glb", T)) {
                if (va.propEntity != x3::game::kNoLink && va.propEntity < walkScene.size())
                    walkScene.get(va.propEntity).visible = false;   // bye, yellow box
                streetProps.push_back(std::move(cart));
            } else {
                x3::logWarn("[cart] hotdog_cart.glb unavailable — blockout cart kept");
                break;
            }
        }
        if (!streetProps.empty())
            x3::logInfo("[cart] " + std::to_string(streetProps.size()) +
                        " real hot-dog cart(s) placed");
    }

    // ===================== CYBERPUNK AUDIO (Cyberpunk Game SFX kit) ==========
    // Echo Harbor was dead silent. Wire a layered soundscape from the 1067-sound
    // kit: a 2D rainy-cyber-city ambient bed + night music, positional 3D hums at
    // the mine / drones / harbor, and UI one-shots fired on the panels/build/ride.
    std::unique_ptr<x3::audio::IAudioSystem> eaudio(x3::audio::createAudioSystem());
    const bool audioOn = eaudio && eaudio->init();
    // Repo-local curated WAVs under assets/audio/echotropolis/** (audio-armory pass):
    // resolveAudio() tries the committed mirror FIRST, so the city sounds on a fresh
    // clone with no external packs — the same portability win the curated GLBs got.
    // Provenance for every file: assets/audio/AUDIO_MANIFEST.md (Echo Harbor section).
    auto snd = [&](const char* rel){ return audioOn
        ? eaudio->load(x3::game::resolveAudio(std::string("echotropolis/") + rel))
        : x3::audio::SoundHandle{}; };
    x3::audio::SoundHandle sfxConfirm, sfxAccept, sfxDeny;
    std::vector<x3::audio::LoopHandle> audioLoops;
    if (audioOn) {
        // LAYER 1 — 2D rainy cyber-city ambient bed (always on) + a low server hum.
        auto bed = snd("ambient/city_bed.wav");
        if (bed.valid()) audioLoops.push_back(eaudio->startLoop(bed, 0.55f, 1.0f));
        auto rain = snd("ambient/rain_urban.wav");
        if (rain.valid()) audioLoops.push_back(eaudio->startLoop(rain, 0.22f, 1.0f));
        // LAYER 2 — night exploration music bed (2D loop, low under the ambience).
        auto mus = snd("music/night_alleyways.wav");
        if (mus.valid()) audioLoops.push_back(eaudio->startLoop(mus, 0.30f, 1.0f));
        // LAYER 3 — positional 3D loops (attenuate against the listener each frame).
        const float mgy = hf.ok()?hf.heightAt(kMineX,kMineZ):190.0f;
        auto mineHum = snd("ambient/mine_hum.wav");
        if (mineHum.valid()) audioLoops.push_back(eaudio->startLoop3D(mineHum, kMineX, mgy+3.0f, kMineZ, 0.85f, 1.0f));
        auto droneHum = snd("ambient/drone_buzz.wav");
        if (droneHum.valid()) audioLoops.push_back(eaudio->startLoop3D(droneHum, -20.0f, 250.0f, 760.0f, 0.6f, 1.0f));
        auto boatHum = snd("ambient/harbor_ship_idle.wav");
        if (boatHum.valid()) audioLoops.push_back(eaudio->startLoop3D(boatHum, -400.0f, 192.0f, 300.0f, 0.7f, 1.0f));
        // LAYER 4 — UI one-shots (played on events).
        sfxConfirm = snd("ui/confirm.wav");
        sfxAccept  = snd("ui/accept.wav");
        sfxDeny    = snd("ui/deny.wav");
        x3::logInfo("--world echotropolis: CYBERPUNK AUDIO on — " +
                    std::to_string(audioLoops.size()) + " loops (bed/rain/music/mine/drone/harbor) + UI SFX");
    }
    auto uiSfx = [&](x3::audio::SoundHandle s, float v){ if (audioOn && s.valid()) eaudio->playSound2D(s, v, 1.0f); };

    // ===================== WD2 STACK WIRING (Lane 3) =====================
    // Everything the sinks touch exists by now: streetLamps, residents,
    // worldCars, npcLife, treasury, player. The registry got the citizens at
    // NpcLife::build; here the STREET tech joins them and the sinks go live.
    auto applyRpgStats = [&]() {
        rpgMods = skillTree.mods();
        player.setMaxHpBonus(rpgMods.maxHpBonus);
        player.setSpeedMult(rpgMods.speedMult);
        progression.setXpMult(rpgMods.xpMult);
    };
    auto grantXp = [&](int amount) {
        if (progression.addXp(amount) > 0) rpgUi.notifyLevelUp(progression.level());
    };
    {
        // Persisted progression (additive text file, the app_run format).
        FILE* rf = std::fopen(kRpgSavePath, "rb");
        if (rf) {
            std::string blob;
            char line[160];
            while (std::fgets(line, sizeof(line), rf)) {
                blob += line;
                char id[96] = {};
                if (std::sscanf(line, "skill %95s", id) == 1) skillTree.setOwned(id);
            }
            std::fclose(rf);
            progression.deserialize(blob);   // reads xp; ignores unknown lines
            progression.setSpentPoints(skillTree.ownedCost());
            x3::logInfo("--world echotropolis: RPG save loaded (LV " +
                        std::to_string(progression.level()) + ", " +
                        std::to_string(skillTree.ownedCount()) + " skills)");
        }
        applyRpgStats();
    }
    if (npcLifeBuilt) {
        // The robbery alarm now rings the REAL city bell: heat + crowd scatter.
        npcLife.setAlarmSink([&](const x3::phys::Vec3& p, int) {
            cityAlert.reportTerminalHack(p);
            if (residentsBuilt) residents.onViolence(p);
        });
    }
    {
        x3::game::HackSinks sinks;
        sinks.onHeat = [&](const x3::phys::Vec3& p, int) {
            cityAlert.reportTerminalHack(p);
        };
        sinks.onKarma = [&](int d) { cityTimeline.adjustKarma(d); };
        sinks.onLightsOut = [&](uint32_t ent) {
            // Junction box: grid cut — nearby lamps go dark, then recover.
            if (ent < hackPropPos.size())
                streetLamps.killNear(lampScene, hackPropPos[ent].x,
                                     hackPropPos[ent].z, 60.0f, 45.0f);
        };
        sinks.onVehicle = [&](uint32_t ent) {
            // Popped vehicle: the alarm scatters the street, same as hold-E.
            if (ent < hackPropPos.size() && residentsBuilt)
                residents.onViolence(hackPropPos[ent]);
        };
        sinks.onResult = [&](const x3::game::HackResult& r) {
            hackCard = r; hackCardT = 5.0f;
            if (r.credits > 0) treasury += (double)r.credits;
            // SIGNAL SPOOFED - GRIDLOCK is real: every signal runs red 20 s.
            if (r.type == x3::game::HackableType::TrafficSignal)
                trafficSpoofT = 20.0f;
            grantXp(kXpHack);
        };
        hax.setSinks(sinks);

        // ---- CITY-WIDE PLACEMENT (the carDefs pattern) ----
        // Cameras watch the gates/plazas, junction boxes feed the lamp rows,
        // ATMs skim, signals spoof. Positions ride the district anchors that
        // already exist (lamp rows, gates, vendors, shop fronts).
        auto addHack = [&](x3::game::HackableType t, float x, float z,
                           float lift, int credits, const char* label) {
            x3::game::HackableObject o;
            o.type = t;
            const float gy = hf.ok() ? hf.heightAt(x, z) : 0.0f;
            o.pos = { x, gy + lift, z };
            o.entity = (uint32_t)hackPropPos.size();
            o.credits = credits;
            o.label = label ? label : "";
            hackPropPos.push_back(o.pos);
            hax.add(o);
        };
        using HT = x3::game::HackableType;
        // Cameras (head height ~4 m).
        addHack(HT::Camera, 18.0f, 764.0f, 4.0f, 0, "PLAZA CAM 01");
        addHack(HT::Camera, -66.0f, 742.0f, 4.0f, 0, "NOODLE CAM");
        addHack(HT::Camera, 155.0f, 368.0f, 4.0f, 0, "HARBOR CAM");
        addHack(HT::Camera, 700.0f, 390.0f, 4.0f, 0, "URBAN GATE CAM");
        addHack(HT::Camera, 832.0f, 1098.0f, 4.0f, 0, "RECIFE GATE CAM");
        addHack(HT::Camera, 1290.0f, 995.0f, 4.0f, 0, "HIVEMIND CAM");
        addHack(HT::Camera, -542.0f, 800.0f, 4.0f, 0, "MINE LOT CAM");
        // Junction boxes (waist height) — each near a real lamp row.
        addHack(HT::JunctionBox, -40.0f, 752.0f, 1.0f, 0, "CROWN GRID");
        addHack(HT::JunctionBox, 700.0f, 346.0f, 1.0f, 0, "URBAN GRID");
        addHack(HT::JunctionBox, 1030.0f, 1226.0f, 1.0f, 0, "RECIFE GRID");
        addHack(HT::JunctionBox, 1390.0f, 1004.0f, 1.0f, 0, "HIVEMIND GRID");
        // ATMs (screen height) — skimmable credits into the treasury.
        addHack(HT::ATM, 6.0f, 766.0f, 1.2f, 240, "PLAZA ATM");
        addHack(HT::ATM, 146.0f, 356.0f, 1.2f, 180, "HARBOR ATM");
        addHack(HT::ATM, 760.0f, 354.0f, 1.2f, 320, "URBAN ATM");
        addHack(HT::ATM, 1010.0f, 1254.0f, 1.2f, 150, "RECIFE ATM");
        // Traffic signals (mast height) — repeatable spoofs at the junctions.
        addHack(HT::TrafficSignal, 700.0f, 350.0f, 4.5f, 0, "URBAN CROSS");
        addHack(HT::TrafficSignal, 706.0f, 402.0f, 4.5f, 0, "URBAN GATE");
        addHack(HT::TrafficSignal, 838.0f, 1110.0f, 4.5f, 0, "RECIFE GATE");
        addHack(HT::TrafficSignal, -20.0f, 748.0f, 4.5f, 0, "CROWN DRAG");
        // The parked street fleet joins the registry (markers + pop effect);
        // hold-E on the door stays the real unlock path.
        if (worldCars.built()) {
            for (uint32_t ci = 0; ci < worldCars.carCount(); ++ci) {
                const x3::phys::Vec3 cp = worldCars.parkedPos(ci);
                x3::game::HackableObject o;
                o.type = HT::Vehicle;
                o.pos = { cp.x, cp.y + 0.8f, cp.z };
                o.entity = (uint32_t)hackPropPos.size();
                o.label = worldCars.def(ci).id;
                hackPropPos.push_back(o.pos);
                hax.add(o);
            }
        }
        // ATM street props: the ctos terminal reads as a street unit.
        static constexpr float kAtmSpots[4][2] = {
            { 6.0f, 766.0f }, { 146.0f, 356.0f },
            { 760.0f, 354.0f }, { 1010.0f, 1254.0f } };
        for (const auto& ap : kAtmSpots) {
            float T[16];
            buildXf(ap[0], ap[1], 0.0f, 1.0f, 0.95f, T);
            auto atm = std::make_unique<x3::game::EnvArtSystem>();
            if (atm->buildFromGlbAt(*device, x3::game::assetRoot() + "/meshy/props",
                                    "ctos_terminal.glb", T))
                streetProps.push_back(std::move(atm));
        }
        // WD2 CAMERA PROPS (Tim: "are we using a great lookin camera model?"):
        // every registered Camera hackable mounts the Recife pack's real CCTV
        // unit at its marker point — visible on the wall before the scanner
        // ever opens. One EnvArt system, shared upload, N instances.
        {
            auto camSys = std::make_unique<x3::game::EnvArtSystem>();
            if (camSys->beginFromDir(*device,
                    "D:/Assets/_glb/prefab_buildings/Cyberpunk City Recife Environment")) {
                int cctv = 0;
                for (uint32_t hi = 0; hi < hax.count(); ++hi) {
                    const auto& o = hax.at(hi);
                    if (o.type != x3::game::HackableType::Camera) continue;
                    // Real unit is 22 cm; x1.6 reads at street distance
                    // (WD2's cams run slightly oversized too).
                    const float cs2 = 1.6f;
                    const float cy2 = std::cos((float)hi * 2.399963f) * cs2;
                    const float sy2 = std::sin((float)hi * 2.399963f) * cs2;
                    const float T[16] = { cy2,0,-sy2,0, 0,cs2,0,0, sy2,0,cy2,0,
                                          o.pos.x, o.pos.y, o.pos.z, 1 };
                    if (camSys->addGlbInstance("SM_Camera_01_Camera.glb", T)) ++cctv;
                }
                if (cctv > 0) {
                    x3::logInfo("--world echotropolis: " + std::to_string(cctv) +
                                " CCTV props mounted (Recife SM_Camera_01)");
                    streetProps.push_back(std::move(camSys));
                }
            }
        }
        x3::logInfo("--world echotropolis: WD2 HACKABLES live — " +
                    std::to_string(hax.count()) + " objects (" +
                    std::to_string(hax.countType(HT::Npc)) + " citizen scan-cards); "
                    "H reveals, E hacks, K opens skills");
    }

    // ================= CITIZEN TALK actions (npcLife + audio exist by now) ======
    // NOTHING here blocks: talkAsk() enqueues onto the LLM's inference thread and
    // returns; talkPoll() drains whatever tokens landed since the last frame.
    auto talkClose = [&]() {
        if (talk.chat && talkLlm) { talkLlm->cancel(talk.chat); talkLlm->endChat(talk.chat); }
        talk = TalkState{};
    };
    auto ambientClose = [&]() {
        if (amb.chat && talkLlm) { talkLlm->cancel(amb.chat); talkLlm->endChat(amb.chat); }
        amb = AmbientState{};
    };
    auto talkAsk = [&](int idx) {
        if (!talkLlm || !npcLifeBuilt || idx < 0 || (uint32_t)idx >= npcLife.agentCount()) return;
        // The PLAYER always wins the single inference worker: drop any ambient
        // mutter that is still generating so this reply starts immediately.
        if (amb.pending) ambientClose();
        const auto& a = npcLife.agent((uint32_t)idx);
        if (talk.agent != idx) {              // a different citizen -> a fresh conversation
            talkClose();
            talk.agent = idx;
            talk.chat  = talkLlm->startChat(talkPersonaPrompt(a));
            if (talk.chat == x3::llm::kInvalidChat) { talk.agent = -1; return; }
        }
        if (talk.pending) return;             // ONE reply in flight at a time (the cap)
        talk.question = kTalkPrompts[talkPromptIdx];
        if (!talkLlm->submit(talk.chat, talk.question)) return;
        talk.reply.clear(); talk.pending = true; talk.tokens = 0; talk.polls = 0;
        talk.askedAt = glfwGetTime(); talk.firstAt = 0.0; talk.endAt = 0.0;
        x3::logInfo("[talk] ask " + a.name + " (" + x3::game::archetypeName(a.arch) +
                    "): \"" + talk.question + "\"");
        uiSfx(sfxConfirm, 0.6f);
    };
    // AMBIENT CHATTER: pick an idle citizen near the player and have them mutter one
    // unprompted line. Never runs while the player's own exchange is generating.
    auto ambientTick = [&](float /*dt*/) {
        if (!ambientOn || !talkLlm || !npcLifeBuilt) return;
        if (!walkMode || !physOk) { if (amb.agent >= 0) ambientClose(); return; }
        const double now = glfwGetTime();
        // Retire a finished mutter once its bubble has had its time.
        if (!amb.pending && amb.agent >= 0 && amb.endAt > 0.0 && now > amb.endAt) ambientClose();
        if (amb.pending || amb.agent >= 0) return;
        if (now < amb.nextAt) return;
        if (talk.pending) { amb.nextAt = now + 2.0; return; }   // player has the worker
        amb.nextAt = now + (double)kAmbientEverySec;
        float ex, ey, ez, eyw, ept; player.camera(ex, ey, ez, eyw, ept);
        // Candidates: 6..34 m out, idle-ish, and not the citizen you're talking to.
        int cand[16]; int nc = 0;
        for (uint32_t i = 0; i < npcLife.agentCount() && nc < 16; ++i) {
            if ((int)i == talk.agent) continue;
            const auto& a = npcLife.agent(i);
            const float dx = a.pos.x - ex, dz = a.pos.z - ez;
            const float d2 = dx*dx + dz*dz;
            if (d2 < 6.0f*6.0f || d2 > 34.0f*34.0f) continue;
            cand[nc++] = (int)i;
        }
        if (nc == 0) return;
        const int idx = cand[(int)((uint32_t)(now * 977.0) % (uint32_t)nc)];
        const auto& a = npcLife.agent((uint32_t)idx);
        amb.chat = talkLlm->startChat(talkPersonaPrompt(a));
        if (amb.chat == x3::llm::kInvalidChat) { amb = AmbientState{}; return; }
        if (!talkLlm->submit(amb.chat, "Mutter one short thing out loud to nobody in particular.")) {
            ambientClose(); return;
        }
        amb.agent = idx; amb.pending = true; amb.line.clear(); amb.endAt = 0.0;
    };
    auto ambientPoll = [&]() {
        if (!talkLlm || amb.chat == x3::llm::kInvalidChat || !amb.pending) return;
        const x3::llm::PollResult pr = talkLlm->poll(amb.chat);
        amb.line += pr.newTokens;
        if (pr.done) {
            amb.pending = false;
            amb.endAt   = glfwGetTime() + (double)kAmbientHoldSec;
            const bool empty = amb.line.find_first_not_of(" \t\r\n") == std::string::npos;
            if (!pr.failed && !empty && npcLifeBuilt && (uint32_t)amb.agent < npcLife.agentCount())
                x3::logInfo("[talk] ambient " + npcLife.agent((uint32_t)amb.agent).name +
                            ": \"" + amb.line + "\"");
            if (pr.failed || empty) ambientClose();
        }
    };
    // ECHO_TALKDEMO=1 in the WINDOWED loop: drop straight into walk mode and open a
    // conversation with the nearest citizen (a --shot-cam capture cannot press T, and
    // neither can an automated smoke run — this is the same code path the T key drives).
    const bool talkDemoWin = [](){ const char* e = std::getenv("ECHO_TALKDEMO"); return e && e[0]=='1'; }();
    bool talkDemoFired = false, talkDemoLoop = false;
    if (talkDemoWin) {
        if (const char* e = std::getenv("ECHO_TALKDEMO_LOOP"); e && e[0]=='1') talkDemoLoop = true;
    }
    auto talkPoll = [&]() {
        if (!talkLlm || talk.chat == x3::llm::kInvalidChat || !talk.pending) return;
        const x3::llm::PollResult pr = talkLlm->poll(talk.chat);
        if (!pr.newTokens.empty()) {
            if (talk.firstAt == 0.0) {        // TIME TO FIRST TOKEN — the metric that matters
                talk.firstAt = glfwGetTime();
                x3::logInfo("[talk] first token in " +
                            std::to_string((int)((talk.firstAt - talk.askedAt) * 1000.0)) + " ms");
            }
            talk.reply  += pr.newTokens;
            talk.tokens += pr.newTokenCount;
            ++talk.polls;
            if (const char* d = std::getenv("ECHO_TALK_DEBUG"); d && d[0]=='1')
                x3::logInfo("[talk]   +" + std::to_string(pr.newTokenCount) + " tok @ " +
                            std::to_string((int)((glfwGetTime() - talk.askedAt) * 1000.0)) + " ms");
        }
        if (pr.done) {
            talk.pending = false;
            const double now = glfwGetTime();
            talk.endAt = now + (double)kBubbleHoldSec;
            const double tot = now - talk.askedAt;
            x3::logInfo("[talk] reply done: " + std::to_string(talk.tokens) + " tok in " +
                        std::to_string((int)(tot * 1000.0)) + " ms (" +
                        std::to_string((int)(tot > 0.0 ? (double)talk.tokens / tot : 0.0)) +
                        " tok/s, " + std::to_string(talk.polls) + " streamed chunks)" + (pr.failed ? " [FAILED]" : "") + "  \"" + talk.reply + "\"");
        }
    };

    // CVars — the FULL X3Native r_* family (Tim: "Should have everything
    // X3native has"), same names + docs as app_run so muscle memory transfers.
    // Type `r_maxfps 120`, `r_ddgi 0`, `r_taa 0`, etc. Synced to the device
    // every frame below; applyAtmosphere stays the single setPostFX writer.
    console->registerCVar("r_maxfps", "240", "frame cap when vsync off (0 = uncapped)");
    // MULTI-INSTANCE LANE (Bug 2): r_presentmode + r_bgfps. This host registers
    // its own catalog rather than the shared one, so it asks for them by name.
    x3::game::registerMultiInstanceCVars(*console);
    console->registerCVar("ws_budget", "2.0", "world-streaming main-thread budget per frame (ms)");
    console->registerCVar("r_exposure", "1.0", "whole-scene brightness (pre-tonemap exposure multiplier; live)");
    console->registerCVar("r_tonemap",        "1",    "tonemap operator: 1 = ACES filmic (default), 0 = passthrough clamp (debug A/B)");
    console->registerCVar("r_bloom",          "1",    "bloom on/off (0 skips the whole downsample/upsample chain)");
    console->registerCVar("r_bloomintensity", "-1",   "bloom strength override; <0 = keep the scene-tuned value (default)");
    console->registerCVar("r_bloomthreshold", "-1",   "bloom bright-pass threshold; <0 = keep the scene-tuned value");
    console->registerCVar("r_autoexposure",   "1",    "auto-exposure (eye adaptation): scene log-luminance drives exposure; r_exposure becomes a bias");
    console->registerCVar("r_aespeed",        "-1",   "auto-exposure adaptation speed (1/s); <0 = engine default");
    console->registerCVar("r_aemin",          "-1",   "auto-exposure clamp floor; <0 = engine default");
    console->registerCVar("r_aemax",          "-1",   "auto-exposure clamp ceiling; <0 = keep the scene-tuned night ramp");
    console->registerCVar("r_aekey",          "-1",   "auto-exposure target middle-grey key; <0 = engine default");
    console->registerCVar("r_taa",        "1",    "temporal AA: 1 = jitter + history resolve (default), 0 = off");
    console->registerCVar("r_taasharpen", "-1",   "post-TAA RCAS-style sharpen; <0 = engine default");
    console->registerCVar("r_velocity",   "0",    "per-object motion vectors for TAA/DLSS (0 = camera-only reproj)");
    console->registerCVar("r_rtao",          "0",    "hardware RT ambient occlusion (ray query); 0 = off (raster/SSAO)");
    console->registerCVar("r_rtao_radius",   "2.0",  "RT AO ray length (meters)");
    console->registerCVar("r_rtao_rays",     "8",    "RT AO hemisphere rays per pixel (1..32)");
    console->registerCVar("r_rtao_strength", "0.85", "RT AO applied darkening (1 = full, 0 = off)");
    console->registerCVar("r_ssr",           "0", "screen-space reflections (needs r_taa 1); 0 = off");
    console->registerCVar("r_rtreflections", "0", "ray-query reflection fallback where SSR misses (RT hardware only)");
    console->registerCVar("r_reflquality",   "0", "reflection buffer resolution: 0 = half-res (default), 1 = full-res");
    console->registerCVar("r_reflintensity", "1", "reflection blend weight scale [0..1] on the IBL-specular replace");
    console->registerCVar("r_wetness",         "0",    "Surface wetness 0..1 (rain soak); 0 = dry (byte-identical)");
    console->registerCVar("r_wetness_porosity","1.0",  "How much materials darken when wet");
    console->registerCVar("r_wetness_puddles", "1.0",  "Cavity/AO pooling strength (0 = uniform coat)");
    console->registerCVar("r_wetness_minrough","0.06", "Roughness a fully-soaked surface converges to");
    console->registerCVar("r_ddgi",           "1",    "DDGI probe-grid GI (this world defaults ON; 0 = flat/IBL ambient)");
    console->registerCVar("r_ddgi_debug",     "0",    "DDGI debug view: 0 = off, 1 = irradiance field, 2 = grid confidence");
    console->registerCVar("r_ddgi_rays",      "96",   "DDGI rays per probe per frame (16..128)");
    console->registerCVar("r_ddgi_intensity", "1.0",  "DDGI applied GI scale on the replaced ambient diffuse");
    console->registerCVar("r_rtshadows",    "0",    "RT soft shadows: 0 = CSM-only, 1 = sun RT, 2 = sun + point lights (district self-shadow known)");
    console->registerCVar("r_rtsun_size",   "0.6",  "RT sun angular radius (degrees) — penumbra width");
    console->registerCVar("r_rtpoint_max",  "4",    "RT point-light shadow rays per pixel budget");
    console->registerCVar("r_rtpoint_size", "0.15", "RT point-light source radius (meters) — penumbra softness");
    console->registerCVar("r_frustumcull", "1", "CPU per-object frustum cull (0 = draw every instance, no cull)");
    console->registerCVar("r_cullpath", "0", "GPU cull path: -1 auto, 0 CPU, 1 tier0 gfx-queue, 2 tier1 async, 3 tier2 meshlets");
    console->registerCVar("r_hzb", "0", "HZB occlusion cull on the GPU path (0 = frustum only)");
    console->registerCVar("r_skinnedrt", "1", "add visible skinned characters to the RT scene TLAS (0 = static-only TLAS)");
    // ECHO_RT=1 (the legacy env opt-in for RT sun shadows + RT AO) seeds the cvars.
    { const char* e = std::getenv("ECHO_RT");
      if (e && e[0] == '1') { console->set("r_rtshadows", "1"); console->set("r_rtao", "1"); } }
    double frameCapPrev = glfwGetTime();

    // ===== DDGI (Tim: "Turn On DDGI!") — probe-grid GI over the city core =====
    // Replaces the flat ambient-diffuse term with a live probe light field: sun
    // bounce off facades, neon spill onto streets, sky light in alleys. Explicit
    // volume (auto-fit clamps to 240 m — useless on a 4 km island): an AABB over
    // the crown city + drag + districts, harbor level up over the towers.
    // Tier-gated in the device (ray query + position fetch; the 5090 qualifies,
    // anything else stores it as a no-op). ECHO_DDGI=0 seeds r_ddgi 0; the
    // enabled/debug/rays/intensity lanes live in the r_ddgi* cvars (synced below).
    x3::rhi::IRenderDevice::DdgiParams ddgiP{};
    {
        const char* e = std::getenv("ECHO_DDGI");
        if (e && e[0] == '0') console->set("r_ddgi", "0");
        ddgiP.enabled = console->getInt("r_ddgi") != 0;
        ddgiP.originX = -740.0f; ddgiP.originY = 0.0f; ddgiP.originZ = 70.0f;
        ddgiP.sizeX   = 1600.0f; ddgiP.sizeY  = 400.0f; ddgiP.sizeZ  = 1600.0f;
        ddgiP.countX  = 24; ddgiP.countY = 8; ddgiP.countZ = 24;
        ddgiP.raysPerProbe = 96;
        device->setDdgiParams(ddgiP);
        x3::logInfo(std::string("--world echotropolis: DDGI ") +
                    (ddgiP.enabled ? "ON" : "off (ECHO_DDGI=0)") +
                    (device->rayTracingSupported() ? " [RT hardware]" : " [no RT — inert]"));
    }

    // --set OVERRIDES. This host builds its OWN console and registers its own
    // cvars above, and until now it never looked at hc.cliCVars — so `--set`
    // was a SILENT NO-OP for every cvar in this world, not just new ones. Any
    // A/B run against --world echotropolis with --set compared a value to
    // itself. Applied HERE, after registration, because registerCVar seeds the
    // default and would otherwise stomp the override.
    for (const auto& kv : hc.cliCVars) {
        console->set(kv.first, kv.second);
        x3::logInfo("--world echotropolis: --set " + kv.first + " " + kv.second);
    }

    // Per-frame cvar -> device sync (the app_run applyRtaoCVars pattern). Cheap:
    // string lookups on ~20 cvars; every setter is a cached-store no-op when
    // unchanged. Runs at the top of the frame so console edits apply same-frame.
    auto syncCVars = [&]() {
        auto cf = [&](const char* n) { return console->getFloat(n); };
        auto ci = [&](const char* n) { return console->getInt(n); };
        g_expMul = std::max(0.05f, cf("r_exposure"));
        g_postCv.tonemap        = ci("r_tonemap");
        g_postCv.bloomOn        = ci("r_bloom");
        g_postCv.bloomIntensity = cf("r_bloomintensity");
        g_postCv.bloomThreshold = cf("r_bloomthreshold");
        g_postCv.ae             = ci("r_autoexposure");
        g_postCv.aeSpeed        = cf("r_aespeed");
        g_postCv.aeMin          = cf("r_aemin");
        g_postCv.aeMax          = cf("r_aemax");
        g_postCv.aeKey          = cf("r_aekey");
        g_postCv.taa            = ci("r_taa");
        g_postCv.taaSharpen     = cf("r_taasharpen");
        g_postCv.velocity       = ci("r_velocity");
        x3::rhi::IRenderDevice::RtaoParams ao{};
        ao.enabled  = ci("r_rtao") != 0;
        ao.radius   = cf("r_rtao_radius");
        ao.rays     = std::clamp(ci("r_rtao_rays"), 1, 32);
        ao.strength = cf("r_rtao_strength");
        device->setRtaoParams(ao);
        // MULTI-INSTANCE LANE: live present-mode switch (no-op when unchanged) +
        // the IBL rebake rate limit that closed Bug 2.
        device->setPresentMode(x3::game::presentModeFromCVar(*console));
        device->setIblRate(cf("r_iblrate"));
        x3::rhi::IRenderDevice::ReflectionParams rf{};
        rf.ssr        = ci("r_ssr") != 0;
        rf.rtFallback = ci("r_rtreflections") != 0;
        rf.fullRes    = ci("r_reflquality") != 0;
        rf.intensity  = cf("r_reflintensity");
        device->setReflectionParams(rf);
        ddgiP.enabled      = ci("r_ddgi") != 0;
        ddgiP.debug        = ci("r_ddgi_debug");
        ddgiP.raysPerProbe = std::clamp(ci("r_ddgi_rays"), 16, 128);
        ddgiP.intensity    = cf("r_ddgi_intensity");
        device->setDdgiParams(ddgiP);
        // SURFACE WETNESS. Echo Harbor is a Pacific-Northwest harbour: rain is
        // its default mood, and a wet street is what makes the reflection and
        // DDGI work above actually visible. The cvar is the manual override;
        // when this world drives Weather it feeds WetnessModel instead.
        x3::rhi::IRenderDevice::WetnessParams wt{};
        wt.amount   = cf("r_wetness");
        wt.porosity = cf("r_wetness_porosity");
        wt.puddles  = cf("r_wetness_puddles");
        wt.minRough = cf("r_wetness_minrough");
        device->setWetness(wt);
        x3::rhi::IRenderDevice::RtShadowParams rs{};
        rs.tier        = ci("r_rtshadows");
        rs.sunSizeDeg  = cf("r_rtsun_size");
        rs.pointMax    = ci("r_rtpoint_max");
        rs.pointRadius = cf("r_rtpoint_size");
        device->setRtShadowParams(rs);
        device->setFrustumCullEnabled(ci("r_frustumcull") != 0);
        device->setCullPath(ci("r_cullpath"));
        device->setHzbEnabled(ci("r_hzb") != 0);
        device->setSkinnedRtEnabled(ci("r_skinnedrt") != 0);
    };

    // ===== CONSOLE world commands (modeled on the Babylon X3Console catalog:
    // tod / tp / pos / camera modes / render toggles; the RPG-side commands
    // come with their systems). help/clear/quit/fps/stats + CVars are already
    // registered by hud.init / the D6 backend. =====
    console->registerCommand("tod", [&](const std::vector<std::string>& a) {
        float f = -1.0f;
        const std::string arg = a.empty() ? "" : a[0];
        if      (arg == "noon")   f = kTodNoon;
        else if (arg == "golden") f = kTodGolden;
        else if (arg == "dusk")   f = kTodDusk;
        else if (arg == "night")  f = kTodNight;
        else if (!arg.empty()) { std::istringstream fs(arg); fs >> f; if (fs.fail()) f = -1.0f; }
        if (f >= 0.0f && f <= 1.0f) { tod.setDayFraction(f); console->print("tod -> " + arg); }
        else console->print("tod noon|golden|dusk|night|<0..1>");
    }, "set time of day");
    console->registerCommand("todpause", [&](const std::vector<std::string>&) {
        todPaused = !todPaused;
        console->print(todPaused ? "day clock PAUSED" : "day clock running");
    }, "toggle the day clock");
    console->registerCommand("fly", [&](const std::vector<std::string>&) {
        flyMode = true; walkMode = false; buildMode = false;
        followIdx = -1; playAs = false;
        if (npcLifeBuilt) npcLife.setControlled(-1);
        console->print("fly mode");
    }, "free camera (WASD+mouse, Q/E roll, SPACE/C up-down)");
    console->registerCommand("orbit", [&](const std::vector<std::string>&) {
        flyMode = false; walkMode = false; console->print("orbit mode");
    }, "strategic orbit camera");
    console->registerCommand("walk", [&](const std::vector<std::string>&) {
        if (physOk) { flyMode = false; walkMode = true; console->print("walk mode"); }
        else console->print("walk: physics unavailable");
    }, "first-person on the streets");
    console->registerCommand("tp", [&](const std::vector<std::string>& a) {
        if (a.size() >= 3) {
            flyMode = true; walkMode = false; flySeeded = true;
            flyX = (float)std::atof(a[0].c_str());
            flyY = (float)std::atof(a[1].c_str());
            flyZ = (float)std::atof(a[2].c_str());
            console->print("tp -> fly cam");
        } else console->print("tp <x> <y> <z>");
    }, "teleport the fly camera");
    console->registerCommand("pos", [&](const std::vector<std::string>&) {
        char b[128];
        if (flyMode) std::snprintf(b, sizeof(b), "fly  %.1f %.1f %.1f  yaw %.2f pitch %.2f",
                                   flyX, flyY, flyZ, flyYaw, flyPitch);
        else if (walkMode && physOk) { float x,y,z,yw,pt; player.camera(x,y,z,yw,pt);
            std::snprintf(b, sizeof(b), "walk %.1f %.1f %.1f  yaw %.2f", x, y, z, yw); }
        else std::snprintf(b, sizeof(b), "orbit focus %.1f %.1f  radius %.0f",
                           rig.focusX, rig.focusZ, rig.radius);
        console->print(b);
    }, "camera position");
    console->registerCommand("vol", [&](const std::vector<std::string>& a) {
        const std::string arg = a.empty() ? "auto" : a[0];
        g_volOverride = (arg == "on") ? 1 : (arg == "off") ? 0 : -1;
        console->print("volumetrics " + arg + (arg == "on" ? "  (COSTLY: ~100ms/frame)" : ""));
    }, "volumetric god rays on|off|auto");
    console->registerCommand("sun", [&](const std::vector<std::string>& a) {
        if (a.empty() || a[0] == "auto") { g_sunOverride = -1.0f; console->print("sun auto"); }
        else { const float f = (float)std::atof(a[0].c_str());
               if (f >= 0.0f) { g_sunOverride = f; console->print("sun " + a[0]); }
               else console->print("sun <scale>|auto"); }
    }, "sun intensity override");
    console->registerCommand("vsync", [&](const std::vector<std::string>& a) {
        const bool on = !a.empty() && a[0] == "on";
        device->setVsync(on); console->print(on ? "vsync on" : "vsync off");
    }, "vsync on|off");
    console->registerCommand("amb", [&](const std::vector<std::string>& a) {
        if (a.empty() || a[0] == "auto") { g_ambScale = 1.0f; console->print("ambient auto"); }
        else { const float f = (float)std::atof(a[0].c_str());
               if (f > 0.0f && f <= 4.0f) { g_ambScale = f; console->print("ambient x " + a[0]); }
               else console->print("amb <0.1..4>|auto"); }
    }, "city ambient-light multiplier (glare knob)");
    console->registerCommand("haze", [&](const std::vector<std::string>& a) {
        if (a.empty() || a[0] == "auto") { g_hazeScale = 1.0f; console->print("haze auto"); }
        else { const float f = (float)std::atof(a[0].c_str());
               if (f >= 0.0f && f <= 4.0f) { g_hazeScale = f; console->print("haze x " + a[0]); }
               else console->print("haze <0..4>|auto  (0 = crystal air)"); }
    }, "aerial-fog multiplier (distance washout knob)");
    console->registerCommand("screenshot", [&](const std::vector<std::string>&) {
        static int shotN = 0;
        char p[96]; std::snprintf(p, sizeof(p), "captures/shot_%03d.png", shotN++);
        device->armCapture(p);
        g_shotPath = p;
        console->print(std::string("capturing -> ") + p);
        hud.closeConsole();   // the shot is of the world, not the console
    }, "save a screenshot to captures/");
    // ---- console wave 3: the Babylon X3Console catalog, every applicable
    // category with REAL wiring (Tim: "the console HAD hundreds of commands!").
    auto argF = [](const std::vector<std::string>& a, float def) {
        return a.empty() ? def : (float)std::atof(a[0].c_str());
    };
    auto mulCmd = [&](float& target, const std::vector<std::string>& a,
                      const char* name, float lo, float hi) {
        if (a.empty() || a[0] == "auto") { target = 1.0f; console->print(std::string(name) + " auto"); }
        else { const float f = argF(a, 1.0f);
               if (f >= lo && f <= hi) { target = f; console->print(std::string(name) + " x " + a[0]); }
               else console->print(std::string(name) + " <" + std::to_string(lo) + ".." + std::to_string(hi) + ">|auto"); }
    };
    console->registerCommand("todspeed", [&, mulCmd](const std::vector<std::string>& a) {
        mulCmd(g_todSpeed, a, "todspeed", 0.0f, 100.0f);
    }, "day-clock rate (0 = frozen, 10 = timelapse)");
    console->registerCommand("exposure", [&](const std::vector<std::string>& a) {
        console->set("r_exposure", a.empty() ? "1.0" : a[0]);
        console->print("exposure " + (a.empty() ? std::string("auto") : a[0]) + "   (alias of r_exposure)");
    }, "post exposure multiplier (alias of r_exposure)");
    console->registerCommand("bloom", [&, mulCmd](const std::vector<std::string>& a) {
        mulCmd(g_bloomMul, a, "bloom", 0.0f, 4.0f);
    }, "bloom intensity multiplier");
    console->registerCommand("speed", [&, mulCmd](const std::vector<std::string>& a) {
        mulCmd(g_flySpeedMul, a, "fly speed", 0.05f, 20.0f);
    }, "fly-mode speed multiplier");
    console->registerCommand("sens", [&, mulCmd](const std::vector<std::string>& a) {
        mulCmd(g_sensMul, a, "sensitivity", 0.1f, 5.0f);
    }, "mouselook sensitivity multiplier");
    console->registerCommand("fov", [&, argF](const std::vector<std::string>& a) {
        const float f = argF(a, 0.0f);
        if (f >= 40.0f && f <= 110.0f) { opt.fovDeg = f; console->print("fov " + a[0]); }
        else console->print("fov <40..110>   (current " + std::to_string((int)opt.fovDeg) + ")");
    }, "camera field of view (degrees)");
    console->registerCommand("ssr", [&](const std::vector<std::string>& a) {
        const bool on = a.empty() || a[0] != "off";
        console->set("r_ssr", on ? "1" : "0");
        console->print(on ? "SSR reflections ON  (r_ssr 1)" : "SSR reflections OFF  (r_ssr 0)");
    }, "screen-space reflections on|off (alias of r_ssr)");
    console->registerCommand("rt", [&](const std::vector<std::string>& a) {
        const bool on = !a.empty() && a[0] == "on";
        console->set("r_rtshadows", on ? "1" : "0");
        console->print(on ? "RT sun shadows ON (known: district coplanar self-shadow)"
                          : "RT shadows off (CSM only)");
    }, "ray-traced sun shadows on|off (alias of r_rtshadows)");
    console->registerCommand("go", [&](const std::vector<std::string>& a) {
        struct Spot { const char* name; float x, y, z, yaw, pitch; };
        static const Spot kSpots[] = {
            { "postcard", -450.0f, 620.0f,  900.0f, -1.02f, -0.28f },
            { "crown",     -20.0f, 260.0f,  760.0f,  1.57f, -0.35f },
            { "drag",      -30.0f, 205.0f,  740.0f,  1.40f, -0.15f },
            { "fissure",   331.0f, 420.0f,  459.0f,  2.40f, -0.30f },
            { "mine",     -480.0f, 260.0f,  850.0f,  0.00f, -0.35f },
            { "harbor",    140.0f,  90.0f,  980.0f,  1.57f, -0.20f },
            { "sea",       -60.0f, 350.0f, 4200.0f, -1.57f, -0.05f },
        };
        const std::string want = a.empty() ? "" : a[0];
        for (const Spot& s : kSpots) {
            if (want == s.name) {
                flyMode = true; walkMode = false; flySeeded = true;
                followIdx = -1; playAs = false;
                if (npcLifeBuilt) npcLife.setControlled(-1);
                flyX = s.x; flyY = s.y; flyZ = s.z;
                flyYaw = s.yaw; flyPitch = s.pitch; flyRoll = 0.0f;
                console->print(std::string("-> ") + s.name);
                hud.closeConsole();
                return;
            }
        }
        std::string names = "go";
        for (const Spot& s : kSpots) { names += " "; names += s.name; }
        console->print(names);
    }, "fly to a landmark (go with no arg lists them)");
    console->registerCommand("npcs", [&](const std::vector<std::string>&) {
        if (!npcLifeBuilt) { console->print("npc life not built"); return; }
        for (uint32_t i = 0; i < npcLife.agentCount(); ++i) {
            const auto& a2 = npcLife.agent(i);
            if (a2.onFreeway) continue;
            char l[160];
            std::snprintf(l, sizeof(l), "%2u  %-18s %-18s %-10s (%.0f, %.0f)",
                          i, a2.name.c_str(), x3::game::archetypeName(a2.arch),
                          x3::game::npcActivityName(a2.activity), a2.pos.x, a2.pos.z);
            console->print(l);
        }
    }, "list the named citizens");
    console->registerCommand("find", [&](const std::vector<std::string>& a) {
        if (!npcLifeBuilt || a.empty()) { console->print("find <name-or-role fragment>"); return; }
        std::string want = a[0];
        for (auto& ch : want) ch = (char)std::tolower((unsigned char)ch);
        for (uint32_t i = 0; i < npcLife.agentCount(); ++i) {
            const auto& a2 = npcLife.agent(i);
            std::string hay = a2.name + " " + x3::game::archetypeName(a2.arch);
            for (auto& ch : hay) ch = (char)std::tolower((unsigned char)ch);
            if (hay.find(want) == std::string::npos) continue;
            flyMode = true; walkMode = false; flySeeded = true;
            flyX = a2.pos.x + 5.0f; flyY = a2.pos.y + 1.8f; flyZ = a2.pos.z + 5.0f;
            flyYaw = std::atan2(a2.pos.z - flyZ, a2.pos.x - flyX); flyPitch = -0.1f; flyRoll = 0.0f;
            console->print("-> " + a2.name + " (" + x3::game::archetypeName(a2.arch) + ")");
            hud.closeConsole();
            return;
        }
        console->print("no citizen matches '" + a[0] + "'");
    }, "fly to a citizen by name or role");
    console->registerCommand("money", [&, argF](const std::vector<std::string>& a) {
        treasury += (double)argF(a, 10000.0f);
        console->print("treasury -> $" + std::to_string((long long)treasury));
    }, "money [amount] — add to the treasury (cheat)");
    console->registerCommand("gold", [&, argF](const std::vector<std::string>& a) {
        goldOz += (double)argF(a, 100.0f);
        console->print("gold -> " + std::to_string((long long)goldOz) + " oz");
    }, "gold [oz] — add mined gold (cheat)");
    console->registerCommand("version", [&](const std::vector<std::string>&) {
        console->print("X3Native — ECHOTROPOLIS  |  Vulkan GPU-driven  |  DDGI + volumetrics + llama.cpp citizens");
        console->print(std::string("RT hardware: ") + (device->rayTracingSupported() ? "YES" : "no"));
    }, "build/engine info");
    console->registerCommand("echo", [&](const std::vector<std::string>& a) {
        std::string s;
        for (const auto& w : a) { if (!s.empty()) s += " "; s += w; }
        console->print(s);
    }, "echo <text>");
    console->registerCommand("gi", [&](const std::vector<std::string>& a) {
        const std::string arg = a.empty() ? "" : a[0];
        if      (arg == "off")   console->set("r_ddgi", "0");
        else if (arg == "debug") { console->set("r_ddgi", "1");
                                   console->set("r_ddgi_debug",
                                       std::to_string((console->getInt("r_ddgi_debug") + 1) % 3)); }
        else                     { console->set("r_ddgi", "1"); console->set("r_ddgi_debug", "0"); }
        const int dbg = console->getInt("r_ddgi_debug");
        console->print(std::string("DDGI ") + (console->getInt("r_ddgi") ? "ON" : "off") +
                       (dbg ? (dbg == 1 ? " [debug: irradiance]" : " [debug: confidence]") : "") +
                       (device->rayTracingSupported() ? "" : "  (no RT hardware — inert)"));
    }, "probe-grid GI on|off|debug (alias of r_ddgi*)");
    console->registerCommand("cull", [&](const std::vector<std::string>& a) {
        const bool on = a.empty() || a[0] != "off";
        console->set("r_frustumcull", on ? "1" : "0");
        console->print(on ? "frustum cull ON" : "frustum cull OFF (diagnostic)");
    }, "frustum culling on|off (alias of r_frustumcull)");

    // ===================== LIVING-CITY AUDIO (audio-armory pass) =============
    // Fill the audible gaps the cyberpunk layer left: the helicopters visibly spin
    // but made no sound, the miners hauled in silence, the crowd never murmured,
    // day and night sounded identical, and the gold economy earned invisibly.
    // Everything below is ADDITIVE — the cyberpunk layer's loops are untouched.
    //
    // Rotor loops: IAudioSystem loops can't be re-positioned after startLoop3D, so
    // the MOVING helis get 2D loops with app-side manual attenuation — each frame
    // vol = base * ref^2/(ref^2 + d^2) against the listener via setLoopParams. No
    // panning, but the thump swells exactly when a bird passes over, which is the
    // audible truth that matters. Slight per-heli pitch detune stops phasing.
    struct RotorVoice { x3::audio::LoopHandle lh; float pitch; };
    std::vector<RotorVoice> rotorVoices;         // [0..2] patrol helis, [3] OH1 hero
    x3::audio::LoopHandle loopDayBirds{};        // day ambience bed (vol driven by sun)
    x3::audio::SoundHandle sfxShovel[2], sfxClank[2], sfxFlyby[3], sfxStep[4];
    x3::audio::SoundHandle sfxGoldCoin, sfxChaChing, sfxBuildTick, sfxPhaseChime;
    if (audioOn) {
        // DAY BED — wind + city birds, faded in by sun elevation (night = silent).
        if (auto s = snd("ambient/city_day_birds.wav"); s.valid()) {
            loopDayBirds = eaudio->startLoop(s, 0.0f, 1.0f);
            audioLoops.push_back(loopDayBirds);
        }
        // HELI ROTORS — one loop per patrol heli + the OH1 hero (variant + detune).
        {
            x3::audio::SoundHandle rotor[3] = { snd("vehicles/heli_rotor_a.wav"),
                                                snd("vehicles/heli_rotor_b.wav"),
                                                snd("vehicles/heli_rotor_c.wav") };
            const float det[4] = { 0.88f, 0.94f, 0.84f, 0.78f };   // low = heavier bird
            for (size_t i = 0; i < helis.size() && i < 3; ++i)
                if (rotor[i % 3].valid()) {
                    RotorVoice v{ eaudio->startLoop(rotor[i % 3], 0.0f, det[i]), det[i] };
                    rotorVoices.push_back(v); audioLoops.push_back(v.lh);
                }
            if (oh1Built && rotor[2].valid()) {   // OH1 hero — deepest detune, tracked via oh1.pos()
                RotorVoice v{ eaudio->startLoop(rotor[2], 0.0f, det[3]), det[3] };
                rotorVoices.push_back(v); audioLoops.push_back(v.lh);
            }
        }
        // CROWD MURMUR — the committed crowd WAVs as fixed 3D loops at the two
        // plaza play-spots + the ops plaza, so the crown sounds inhabited up close.
        {
            auto murA = eaudio->load(x3::game::resolveAudio("crowd/murmur_a.wav"));
            auto murB = eaudio->load(x3::game::resolveAudio("crowd/murmur_b.wav"));
            const float gy = kWalkGroundY;
            if (murA.valid()) audioLoops.push_back(eaudio->startLoop3D(murA, -60.0f, gy + 1.6f, 840.0f, 0.8f, 1.0f));
            if (murB.valid()) audioLoops.push_back(eaudio->startLoop3D(murB,  90.0f, gy + 1.6f, 690.0f, 0.8f, 0.94f));
            if (murA.valid()) audioLoops.push_back(eaudio->startLoop3D(murA, kWalkX, gy + 1.6f, kWalkZ, 0.65f, 1.05f));
        }
        // One-shots: mine work, drone flybys, gold/economy, UI ticks, footsteps.
        sfxShovel[0] = snd("mine/shovel_1.wav");   sfxShovel[1] = snd("mine/shovel_2.wav");
        sfxClank[0]  = snd("mine/ore_clank_1.wav"); sfxClank[1] = snd("mine/ore_clank_2.wav");
        sfxFlyby[0]  = snd("vehicles/drone_flyby_1.wav");
        sfxFlyby[1]  = snd("vehicles/drone_flyby_2.wav");
        sfxFlyby[2]  = snd("vehicles/drone_flyby_3.wav");
        sfxStep[0]   = snd("footsteps/step_street_1.wav");
        sfxStep[1]   = snd("footsteps/step_street_2.wav");
        sfxStep[2]   = snd("footsteps/step_street_3.wav");
        sfxStep[3]   = snd("footsteps/step_street_4.wav");
        sfxGoldCoin   = snd("ui/gold_coin.wav");
        sfxChaChing   = snd("ui/gold_chaching.wav");
        sfxBuildTick  = snd("ui/build_tick.wav");
        sfxPhaseChime = snd("ui/phase_chime.wav");
        x3::logInfo("--world echotropolis: LIVING-CITY AUDIO on — " +
                    std::to_string(rotorVoices.size()) + " rotor voices + day bed + murmur + mine/econ/step one-shots");
    }
    // Living-city audio state (timers/edges advanced in the frame audio block).
    float  mineWorkT = 3.0f;         // next mine work one-shot (s)
    float  flybyT    = 9.0f;         // next drone flyby (s)
    int    mineWorkAlt = 0;          // shovel/clank alternator
    int    goldMilestone = 0;        // last celebrated (int)(goldOz/5)
    int    audPrevDay  = simDay;     // day-rollover edge for the treasury cha-ching
    x3::game::TodPhase audPrevPhase = tod.phase();   // phase-change chime edge
    float  stepAccum = 0.0f; float stepPX = 0.0f, stepPZ = 0.0f; bool stepInit = false;
    uint32_t audRng = 0x9E3779B9u;   // tiny xorshift for variant/pitch jitter
    auto audRand01 = [&audRng](){ audRng ^= audRng << 13; audRng ^= audRng >> 17;
                                  audRng ^= audRng << 5; return (audRng & 0xFFFFFF) / 16777216.0f; };

    int lastW = (int)hc.W, lastH = (int)hc.H;
    while (!glfwWindowShouldClose(window) && !wantQuit) {
        // ===== LANE 6 REPLAY (2026-08): the frame loop is SPAN-TIMED ==========
        // The 0bc0d482 breakdown left 41 % of the frame in an unattributed
        // `cpu.host_outside` row because nothing outside the render device was
        // measured. These five spans + the physics zone below partition the loop
        // body. They are EXCLUSIVE (HostScope subtracts every device zone that
        // fires inside them — drawMesh, hud, beginFrame — so nothing is counted
        // twice), and they are DELIMITED rather than brace-nested because the
        // loop body declares locals that later spans still read. Zero cost with
        // X3_PASSTIMERS=0 / `r_passtimers 0`.
        x3::perf::HostScope zInput(x3::perf::Z_HostInput);
        glfwPollEvents();
        {
            const bool esc = glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS;
            if (esc && !prevEsc) {
                if (hud.consoleOpen()) hud.closeConsole();   // ESC closes console first
                else { menuOpen = !menuOpen; uiSfx(sfxConfirm, 0.4f); }   // toggle, never break
            }
            prevEsc = esc;
            // CONSOLE toggle (` / ~): quake-style drop-down (engine Hud front-end).
            const bool gr = glfwGetKey(window, GLFW_KEY_GRAVE_ACCENT) == GLFW_PRESS;
            if (gr && !prevGrave) hud.toggleConsole();
            prevGrave = gr;
            if (conQuit) wantQuit = true;   // console `quit`
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

        // ===== CONSOLE (`): the engine drop-down over a frozen frame, like the
        // pause menu. MUST run before every game key handler — letters you type
        // (g, v, b...) are console text (fed via charCB), never mode toggles.
        // Commands apply instantly; the world resumes the moment it closes. =====
        if (hud.consoleOpen()) {
            if (glfwGetInputMode(window, GLFW_CURSOR) != GLFW_CURSOR_NORMAL)
                glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
            { const bool bk = kd(GLFW_KEY_BACKSPACE);
              if (bk && !prevConBk) hud.onBackspace();
              prevConBk = bk; }
            { const bool en = kd(GLFW_KEY_ENTER) || kd(GLFW_KEY_KP_ENTER);
              if (en && !prevConEnter) hud.onEnter(*console);
              prevConEnter = en; }
            { const bool up = kd(GLFW_KEY_UP);
              if (up && !prevConUp) hud.historyPrev();
              prevConUp = up; }
            { const bool dn = kd(GLFW_KEY_DOWN);
              if (dn && !prevConDown) hud.historyNext();
              prevConDown = dn; }
            { const bool tb = kd(GLFW_KEY_TAB);
              if (tb && !prevConTab) hud.complete(*console);
              prevConTab = tb; }
            if (kd(GLFW_KEY_PAGE_UP))   hud.consoleScroll(+1);
            if (kd(GLFW_KEY_PAGE_DOWN)) hud.consoleScroll(-1);
            if (g_scrollY != 0.0) { hud.consoleScroll((int)(g_scrollY * 3.0)); g_scrollY = 0.0; }
            auto frame = device->beginFrame();
            island.draw(*device, frame);
            props.draw(*device, frame);
            hud.drawConsole(*device, frame, *console, dt);
            device->endFrame(frame);
            fpsAccum += dt; ++fpsFrames;
            if (fpsAccum >= 1.0) { fpsAccum = 0.0; fpsFrames = 0; }
            continue;   // world (and its key handlers) frozen while typing
        }

        // ===== SKILL TREE (K): the WD2 progression screen over a frozen frame
        // — exactly the console's modal pattern, so no key bleeds into the
        // world while browsing. ESC or K closes. =====
        { const bool kk = kd(GLFW_KEY_K);
          if (kk && !prevRpgK && !menuOpen) rpgUi.toggleSkills();
          prevRpgK = kk; }
        if (rpgUi.anyOpen()) {
            if (glfwGetInputMode(window, GLFW_CURSOR) != GLFW_CURSOR_NORMAL)
                glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
            if (kd(GLFW_KEY_ESCAPE)) rpgUi.closeAll();
            x3::game::RpgUi::Input rin{};
            rin.ui.mouseX = (float)mx; rin.ui.mouseY = (float)my;
            rin.ui.mouseDown = mbd(GLFW_MOUSE_BUTTON_LEFT);
            rin.ui.mousePressed = rin.ui.mouseDown && !prevLmb;
            prevLmb = rin.ui.mouseDown;
            { const bool u = kd(GLFW_KEY_UP),    d2 = kd(GLFW_KEY_DOWN),
                         lf = kd(GLFW_KEY_LEFT), rt = kd(GLFW_KEY_RIGHT),
                         en = kd(GLFW_KEY_ENTER) || kd(GLFW_KEY_KP_ENTER);
              rin.navUp    = u  && !prevRpgUp;    rin.navDown  = d2 && !prevRpgDown;
              rin.navLeft  = lf && !prevRpgLeft;  rin.navRight = rt && !prevRpgRight;
              rin.activate = en && !prevRpgEnter;
              prevRpgUp = u; prevRpgDown = d2; prevRpgLeft = lf;
              prevRpgRight = rt; prevRpgEnter = en; }
            auto frame = device->beginFrame();
            island.draw(*device, frame);
            props.draw(*device, frame);
            rpgUi.drawSkills(*device, frame, rin, skillTree, progression,
                             applyRpgStats);
            device->endFrame(frame);
            fpsAccum += dt; ++fpsFrames;
            if (fpsAccum >= 1.0) { fpsAccum = 0.0; fpsFrames = 0; }
            continue;   // world frozen while the tree is open
        }

        // ---- WALK MODE toggle (G) + first-person character step -------------
        // G toggles GROUND <-> FLIGHT (Tim's spec). Landing drops you onto the
        // terrain right under the fly camera; takeoff lifts from the walk eye.
        { const bool g = kd(GLFW_KEY_G);
          if (g && !prevG) {
              if (flyMode && physOk) {           // land where you hover
                  const float gy = hf.ok() ? hf.heightAt(flyX, flyZ) : 0.0f;
                  player.spawn(*phys, flyX, gy + 1.2f, flyZ);
                  flyMode = false; walkMode = true;
                  uiSfx(sfxConfirm, 0.7f);
              } else if (walkMode) {             // take off from where you stand
                  float px, py, pz, pyw, ppt; player.camera(px, py, pz, pyw, ppt);
                  flyX = px; flyY = py + 2.0f; flyZ = pz;
                  flyYaw = pyw; flyPitch = ppt; flyRoll = 0.0f; flySeeded = true;
                  walkMode = false; flyMode = true;
                  // Taking off ends any ride-along/play-as: otherwise the banner
                  // stays up and flight WASD keeps driving the poor citizen.
                  followIdx = -1; playAs = false;
                  if (npcLifeBuilt) npcLife.setControlled(-1);
                  uiSfx(sfxConfirm, 0.7f);
              } else if (physOk) {               // orbit -> ground at the focus
                  const float gy = hf.ok() ? hf.heightAt(rig.focusX, rig.focusZ) : 0.0f;
                  player.spawn(*phys, rig.focusX, gy + 1.2f, rig.focusZ);
                  walkMode = true;
                  uiSfx(sfxConfirm, 0.7f);
              }
          }
          prevG = g; }
        // FLY MODE toggle (V): fly <-> orbit. Entering fly drops walk/ride/build.
        { const bool v = kd(GLFW_KEY_V);
          if (v && !prevV) {
              flyMode = !flyMode;
              if (flyMode) {
                  walkMode = false; buildMode = false;
                  followIdx = -1; playAs = false;
                  if (npcLifeBuilt) npcLife.setControlled(-1);
              }
              uiSfx(sfxConfirm, 0.6f);
          }
          prevV = v; }
        // FLY MODE captures the cursor for free mouselook; every other mode shows it.
        { const int wantCursor = (flyMode && !menuOpen) ? GLFW_CURSOR_DISABLED
                                                        : GLFW_CURSOR_NORMAL;
          if (glfwGetInputMode(window, GLFW_CURSOR) != wantCursor) {
              glfwSetInputMode(window, GLFW_CURSOR, wantCursor);
              glfwGetCursorPos(window, &mx, &my);   // re-baseline: no look snap
              lastMX = mx; lastMY = my; ddx = 0.0f; ddy = 0.0f;
          } }
        { const bool tb = kd(GLFW_KEY_TAB); if (tb && !prevTab && !(shopBuilt && shop.shopMode())) { cityPanelOpen = !cityPanelOpen; uiSfx(sfxConfirm, 0.75f); } prevTab = tb; }
        // BUILD MODE (B): orbit-only; entering walk mode drops it.
        { const bool b = kd(GLFW_KEY_B); if (b && !prevB && !walkMode && !flyMode) { buildMode = !buildMode; uiSfx(sfxConfirm, 0.6f); } prevB = b; }
        if (walkMode) buildMode = false;
        // RIDE-ALONG (F): attach the camera to the citizen you're inspecting; F again releases.
        { const bool f = kd(GLFW_KEY_F);
          if (f && !prevF) {
              if (followIdx >= 0) {                       // release
                  followIdx = -1; playAs = false;
                  if (npcLifeBuilt) npcLife.setControlled(-1);
                  uiSfx(sfxConfirm, 0.6f);
              } else if (walkMode && npcLifeBuilt && lastPickedIdx >= 0) {  // enter (spectate)
                  followIdx = lastPickedIdx; playAs = false; npcLife.setControlled(-1);
                  driveYaw = npcLife.agent((uint32_t)followIdx).yaw;
                  uiSfx(sfxConfirm, 0.7f);
              }
          }
          prevF = f; }
        // PLAY-AS toggle (E): while riding, take the controls or hand them back (spectate).
        { const bool e = kd(GLFW_KEY_E);
          if (e && !prevE && followIdx >= 0 && npcLifeBuilt) {
              playAs = !playAs;
              if (playAs) { npcLife.setControlled(followIdx);
                            driveYaw = npcLife.agent((uint32_t)followIdx).yaw; }
              else npcLife.setControlled(-1);
              uiSfx(sfxAccept, 0.7f);
          }
          prevE = e; }
        // ECHO_TALKDEMO (windowed): force walk mode + talk to the nearest citizen.
        if (talkDemoWin && talkLlm && npcLifeBuilt && physOk &&
            (!talkDemoFired || (talkDemoLoop && !talk.pending && talk.endAt > 0.0))) {
            walkMode = true; flyMode = false;
            float ex, ey, ez, eyw, ept; player.camera(ex, ey, ez, eyw, ept);
            int best = -1; float bd2 = 1e30f;
            for (uint32_t i = 0; i < npcLife.agentCount(); ++i) {
                const auto& a = npcLife.agent(i);
                const float dx = a.pos.x - ex, dz = a.pos.z - ez;
                const float d2 = dx*dx + dz*dz;
                if (d2 < bd2) { bd2 = d2; best = (int)i; }
            }
            if (best >= 0 && bd2 < kTalkDropRange * kTalkDropRange) {
                if (talkDemoFired) talkPromptIdx = (talkPromptIdx + 1) % kTalkPromptCount;
                talkAsk(best);
                talkDemoFired = true;
            }
        }
        // ECHO_PLAYAS_DEMO=1: after settle, ride + play-as agent 0 and spin a slow
        // full circle, capturing frames — the yaw-arc visibility repro harness for
        // Tim's live report ("model only visible for a few degrees of rotation").
        static const bool playasDemo = [](){ const char* e = std::getenv("ECHO_PLAYAS_DEMO");
                                             return e && e[0]=='1'; }();
        static int playasFrame = 0;
        if (playasDemo && npcLifeBuilt) {
            ++playasFrame;
            if (playasFrame == 240 && followIdx < 0) {
                flyMode = false; walkMode = false;
                followIdx = 0; playAs = true;
                npcLife.setControlled(0);
                driveYaw = 0.0f;
            }
            if (followIdx >= 0 && playasFrame <= 240 + 10*60) {
                // ECHO_PLAYAS_SPIN=0 freezes the yaw (static camera -> TAA
                // converges -> clean captures for orientation forensics).
                // ECHO_PLAYAS_FLYCAM=1 swaps the ride-along view for a direct
                // fly-camera inspection 5m from the agent (isolates the follow-
                // camera math from the character rendering).
                static const bool playasSpin = [](){ const char* e = std::getenv("ECHO_PLAYAS_SPIN");
                                                     return !(e && e[0]=='0'); }();
                static const bool playasFlyCam = [](){ const char* e = std::getenv("ECHO_PLAYAS_FLYCAM");
                                                       return e && e[0]=='1'; }();
                if (playasSpin) driveYaw += dt * (6.2831853f / 8.0f);   // full turn in 8 s
                if (playasFlyCam) {
                    const auto& a0 = npcLife.agent(0);
                    flyMode = true; flySeeded = true;
                    flyX = a0.pos.x + 5.0f; flyY = a0.pos.y + 1.6f; flyZ = a0.pos.z + 5.0f;
                    flyYaw = std::atan2(a0.pos.z - flyZ, a0.pos.x - flyX);
                    flyPitch = -0.12f; flyRoll = 0.0f;
                }
                if ((playasFrame % 45) == 0) {
                    char cap[128];
                    std::snprintf(cap, sizeof(cap), "captures/playas_%03d.png", playasFrame);
                    device->armCapture(cap);           // arms the swapchain copy...
                    g_playasCapPath = cap;             // ...written after endFrame below
                }
                if ((playasFrame % 60) == 0) {
                    // DIAGNOSTIC: does the skinned body sit where the agent is?
                    const auto& a0 = npcLife.agent(0);
                    const auto* m0 = npcSkin.character(0);
                    char db[192];
                    std::snprintf(db, sizeof(db),
                        "[playas-demo] agent0 (%.1f,%.1f,%.1f yaw %.2f) monster0 %s(%.1f,%.1f,%.1f) skinned=%d",
                        a0.pos.x, a0.pos.y, a0.pos.z, a0.yaw,
                        m0 ? "" : "NULL", m0 ? m0->pos().x : 0.0f,
                        m0 ? m0->pos().y : 0.0f, m0 ? m0->pos().z : 0.0f,
                        (int)npcSkin.agentSkinned(0));
                    x3::logInfo(db);
                }
            }
        }
        // CITIZEN TALK prompt select ([ / ]) — walk mode only, where build mode (which
        // owns these two keys) is forced off, so the existing binding never collides.
        if (walkMode && talkLlm) {
            { const bool lb = kd(GLFW_KEY_LEFT_BRACKET);
              if (lb && !prevTalkLB) talkPromptIdx = (talkPromptIdx + kTalkPromptCount - 1) % kTalkPromptCount;
              prevTalkLB = lb; }
            { const bool rb = kd(GLFW_KEY_RIGHT_BRACKET);
              if (rb && !prevTalkRB) talkPromptIdx = (talkPromptIdx + 1) % kTalkPromptCount;
              prevTalkRB = rb; }
        }
        // Drop the conversation when you leave walk mode or walk away from the citizen.
        if (talk.agent >= 0) {
            bool drop = !walkMode || !npcLifeBuilt || (uint32_t)talk.agent >= npcLife.agentCount();
            if (!drop && physOk) {
                float ex, ey, ez, eyw, ept; player.camera(ex, ey, ez, eyw, ept);
                const auto& ta = npcLife.agent((uint32_t)talk.agent);
                const float dx = ta.pos.x - ex, dz = ta.pos.z - ez;
                if (dx*dx + dz*dz > kTalkDropRange * kTalkDropRange) drop = true;
            }
            // Bubble expiry: a finished reply lingers kBubbleHoldSec then closes out.
            if (!drop && !talk.pending && talk.endAt > 0.0 &&
                glfwGetTime() > talk.endAt + (double)kBubbleFadeSec) drop = true;
            if (drop) talkClose();
        }
        // PLAY-AS drive: tank-style third-person steering (A/D turn, W/S move, Shift run).
        // Never while flying — flight owns WASD (the citizen must not be dragged along).
        if (followIdx >= 0 && playAs && !flyMode && npcLifeBuilt && (uint32_t)followIdx < npcLife.agentCount()) {
            const auto& a = npcLife.agent((uint32_t)followIdx);
            // Tim (live play test): A/D were REVERSED — from the trailing camera,
            // +yaw (world CCW) reads as a RIGHT turn, so D gets the +.
            const float turn = (kd(GLFW_KEY_D) ? 1.0f : 0.0f) - (kd(GLFW_KEY_A) ? 1.0f : 0.0f);
            driveYaw += turn * 2.2f * dt;
            const float mv  = (kd(GLFW_KEY_W) ? 1.0f : 0.0f) - (kd(GLFW_KEY_S) ? 0.6f : 0.0f);
            const float spd = kd(GLFW_KEY_LEFT_SHIFT) ? 7.0f : 3.6f;
            const float nx = a.pos.x + std::cos(driveYaw) * mv * spd * dt;
            const float nz = a.pos.z + std::sin(driveYaw) * mv * spd * dt;
            const float ny = hf.ok() ? hf.heightAt(nx, nz) : a.pos.y;
            npcLife.driveControlled(nx, ny, nz, driveYaw);
        }
        // ---- CARS: interaction + the drive loop --------------------------
        // E enters / hold-E hacks a parked car (walk mode, not riding a citizen);
        // while DRIVING, worldCars owns the physics step and WASD becomes
        // throttle/steer (host_drive mapping), F or E exits at the door.
        if (physOk && worldCars.built() && (walkMode || worldCars.driving()) && followIdx < 0) {
            static bool prevCarE = false, prevCarF = false;
            const bool e2 = kd(GLFW_KEY_E), f2 = kd(GLFW_KEY_F);
            float pxx, pyy, pzz, pyw2, ppt2; player.camera(pxx, pyy, pzz, pyw2, ppt2);
            worldCars.interact({ pxx, pyy - 1.2f, pzz }, e2, e2 && !prevCarE,
                               f2 && !prevCarF, dt, &player, *phys,
                               audioOn ? eaudio.get() : nullptr);
            // VENDOR BUY LOOP (interiors pillar): cars keep first claim on E;
            // if no car consumed it and a vendor is in radius, E transacts.
            vendorPrompt = nullptr;
            if (!worldCars.driving() && worldCars.prompt().empty()) {
                for (const auto& v : x3::game::kVendorInteractions) {
                    const float dvx = v.x - pxx, dvz = v.z - pzz;
                    if (dvx*dvx + dvz*dvz > v.radius * v.radius) continue;
                    vendorPrompt = v.promptLine;
                    if (e2 && !prevCarE) {
                        treasury += (double)v.price;   // negative = the player pays
                        uiSfx(sfxAccept, 0.8f);
                        // The DODOG economy (echo_interiors spec): buying local
                        // is a small karma tick — and XP toward the next level.
                        cityTimeline.adjustKarma(+1);
                        grantXp(kXpDodog);
                    }
                    break;
                }
            }
            // LANE 4 DOOR PORTALS v1: the interior cells greet the player at
            // the kiosk — E steps in, E steps back out (teleport past the
            // door; the real swinging door is M-C-era polish per the header).
            doorPrompt = nullptr;
            if (!worldCars.driving() && worldCars.prompt().empty() && !vendorPrompt &&
                !kd(GLFW_KEY_H)) {
                static constexpr struct { int cell; float inX, inZ;
                                          const char* enter; const char* leave; }
                kDoorIn[] = {
                    { 0, -0.5f,  3.0f, "[E] ENTER CONDO LOBBY", "[E] LEAVE CONDO LOBBY" },
                    { 2,  2.0f, -4.0f, "[E] ENTER HARBOR SHOP", "[E] LEAVE HARBOR SHOP" },
                };
                for (const auto& din : kDoorIn) {
                    const auto& cell = x3::game::kInteriorCells[din.cell];
                    const float ddx2 = cell.doorX - pxx, ddz2 = cell.doorZ - pzz;
                    const float rr = cell.radius + 4.0f;   // inside point stays in reach
                    if (ddx2 * ddx2 + ddz2 * ddz2 > rr * rr) continue;
                    const bool inside = (playerInCell == din.cell);
                    doorPrompt = inside ? din.leave : din.enter;
                    if (e2 && !prevCarE) {
                        const float ix = inside ? cell.doorX - din.inX * 0.6f
                                                : cell.doorX + din.inX;
                        const float iz = inside ? cell.doorZ - din.inZ * 0.6f
                                                : cell.doorZ + din.inZ;
                        const float gy2 = hf.ok() ? hf.heightAt(ix, iz) : pyy;
                        player.spawn(*phys, ix, gy2 + 1.2f, iz);
                        playerInCell = inside ? -1 : din.cell;
                        uiSfx(sfxConfirm, 0.7f);
                    }
                    break;
                }
            }
            prevCarE = e2; prevCarF = f2;
        }
        // ---- WD2 NETHACK: hold H to reveal, aim + E to hack ---------------
        // Cars keep first claim on E, vendors second; the hack verb fires only
        // when neither consumed it. Works on foot (walk mode, not riding).
        uint32_t hackAim = x3::game::kNoLink;
        {
            const bool hHeld = kd(GLFW_KEY_H) && walkMode && followIdx < 0;
            hax.setHighlight(hHeld);
            if (hHeld) {
                float hex, hey, hez, hyw, hpt;
                player.camera(hex, hey, hez, hyw, hpt);
                const x3::phys::Vec3 eye{ hex, hey, hez };
                const x3::phys::Vec3 fwd{ std::cos(hpt) * std::cos(hyw),
                                          std::sin(hpt),
                                          std::cos(hpt) * std::sin(hyw) };
                hackAim = hax.lookTarget(eye, fwd, 30.0f, 0.966f);
                const bool he = kd(GLFW_KEY_E);
                if (he && !prevHackE && hackAim != x3::game::kNoLink &&
                    !worldCars.driving() && worldCars.prompt().empty() &&
                    !vendorPrompt) {
                    const bool wasNpc =
                        hax.at(hackAim).type == x3::game::HackableType::Npc;
                    const x3::game::HackResult hr = hax.hack(hackAim);
                    if (hr.ok) {
                        if (wasNpc && npcLifeBuilt) npcLife.notifyHacked(hackAim);
                        uiSfx(sfxAccept, 0.8f);
                    } else {
                        uiSfx(sfxDeny, 0.7f);
                    }
                }
                prevHackE = he;
            } else {
                prevHackE = false;
            }
        }
        if (worldCars.driving() && physOk) {
            driveCamYaw   += ddx * 0.0028f * g_sensMul;
            driveCamPitch  = clampf(driveCamPitch - ddy * 0.0028f * g_sensMul, -0.9f, 0.5f);
            x3::phys::VehicleInput vin{};
            const float fwd2 = (kd(GLFW_KEY_W) ? 1.0f : 0.0f) - (kd(GLFW_KEY_S) ? 1.0f : 0.0f);
            float vel2[3]; worldCars.chassisVelocity(vel2);
            const float spd2 = std::sqrt(vel2[0]*vel2[0] + vel2[2]*vel2[2]);
            // host_drive rule: S while rolling forward = BRAKE, from rest = reverse.
            if (fwd2 < 0.0f && spd2 > 1.5f) { vin.brake = 1.0f; }
            else vin.throttle = fwd2;
            vin.steer     = (kd(GLFW_KEY_D) ? 1.0f : 0.0f) - (kd(GLFW_KEY_A) ? 1.0f : 0.0f);
            vin.handBrake = kd(GLFW_KEY_SPACE) ? 1.0f : 0.0f;
            { const bool c2 = kd(GLFW_KEY_C);   // NFS view cycle
              if (c2 && !prevCarC) carView = (carView + 1) % 4;
              prevCarC = c2; }
            if (shopBuilt && shop.shopMode()) { vin = {}; vin.brake = 1.0f; }
            worldCars.driveInput(vin);
            worldCars.preStep(dt);
            { X3_HOST_ZONE(Z_HostPhysics); phys->step(dt); }
            worldCars.postStep(dt);
            if (audioOn) worldCars.updateAudio(eaudio.get(), vin.throttle, dt);
            // Drive XP: every leg of real road banked earns progression.
            driveXpOdo += spd2 * dt;
            if (driveXpOdo >= kDriveLegMeters) {
                driveXpOdo -= kDriveLegMeters;
                grantXp(kXpDriveLeg);
            }
        }
        // ---- NFS PERF SHOP: lift pad, purchase UI, live tuning ------------
        if (shopBuilt && physOk) {
            if (worldCars.driving()) {
                const x3::phys::Vec3 scp = worldCars.carPosition();
                const float sp3[3] = { scp.x, scp.y, scp.z };
                const float fspd = std::fabs(worldCars.forwardSpeed());
                if (!shop.shopMode() && fspd < 0.8f && shop.onLiftPad(sp3)) {
                    shop.setShopMode(true);
                    carBuild.credits = (int)treasury;   // treasury IS the wallet
                    uiSfx(sfxConfirm, 0.7f);
                } else if (shop.shopMode() && !shop.onLiftPad(sp3)) {
                    shop.setShopMode(false);
                }
            } else if (shop.shopMode()) {
                shop.setShopMode(false);
            }
            if (shop.shopMode()) {
                const bool su = kd(GLFW_KEY_UP),        sd = kd(GLFW_KEY_DOWN),
                           se = kd(GLFW_KEY_ENTER) || kd(GLFW_KEY_KP_ENTER),
                           sb = kd(GLFW_KEY_BACKSPACE), st = kd(GLFW_KEY_TAB),
                           sl = kd(GLFW_KEY_LEFT),      sr = kd(GLFW_KEY_RIGHT),
                           sp2 = kd(GLFW_KEY_P),        sf = kd(GLFW_KEY_R),
                           sn = kd(GLFW_KEY_N);
                if (su && !prevShopUp)    shop.uiUp();
                if (sd && !prevShopDown)  shop.uiDown();
                if (se && !prevShopEnter) shop.uiSelect();
                if (sb && !prevShopBack)  shop.uiBack();
                if (st && !prevShopTab)   shop.uiTab();
                if (sl && !prevShopL)     shop.adjustTune(0, -1);
                if (sr && !prevShopR)     shop.adjustTune(0, +1);
                if (sp2 && !prevShopP)    shop.startPull();
                if (sf && !prevShopFix)   shop.repairEngine();
                if (sn && !prevShopN)     shop.refillNitrous();
                prevShopUp = su; prevShopDown = sd; prevShopEnter = se;
                prevShopBack = sb; prevShopTab = st; prevShopL = sl;
                prevShopR = sr; prevShopP = sp2; prevShopFix = sf; prevShopN = sn;
                treasury = (double)carBuild.credits;   // purchases settle live
            }
            shop.update(dt, worldCars.liveCar(),
                        audioOn ? eaudio.get() : nullptr, sfxDeny);
            if (shop.consumeNeedSave())
                carBuild.saveFile(x3::game::vehparts::defaultBuildSavePath());
            // Nitrous rides the composed build while actually driving.
            if (worldCars.driving() && !shop.shopMode() &&
                shop.composed().nitrousMult > 0.0f && carBuild.nitrousRemaining > 0.0f)
                if (auto* lc2 = worldCars.liveCar()) lc2->setTorqueBoost(shop.composed().nitrousMult);
        } else if (walkMode && physOk && followIdx < 0) {   // frozen while riding along
            x3::game::PlayerInput in{};
            in.moveFwd    = (kd(GLFW_KEY_W) ? 1.0f : 0.0f) - (kd(GLFW_KEY_S) ? 1.0f : 0.0f);
            in.moveStrafe = (kd(GLFW_KEY_D) ? 1.0f : 0.0f) - (kd(GLFW_KEY_A) ? 1.0f : 0.0f);
            in.sprint      = kd(GLFW_KEY_LEFT_SHIFT);
            in.jumpPressed = kd(GLFW_KEY_SPACE);
            in.jumpHeld    = kd(GLFW_KEY_SPACE);              // swim: stroke up
            in.diveHeld    = kd(GLFW_KEY_LEFT_CONTROL) || kd(GLFW_KEY_C);  // swim: dive
            in.lookDX = ddx; in.lookDY = ddy;
            player.update(in, dt, *phys);
            { X3_HOST_ZONE(Z_HostPhysics); phys->step(dt); }
        }
        // ---- FLY MODE step: free camera over the city (Tim's spec) ----------
        if (flyMode) {
            if (!flySeeded) {   // first entry: aerial approach vantage on the city
                // --shot-cam now SEEDS the live fly camera too when no capture was
                // requested. It used to be honoured only by the screenshot path, so
                // there was no way to start a live run at a named vantage — which is
                // exactly what an A/B (perf or visual) at a reported viewpoint needs.
                // Additive: no --shot-cam == the historic aerial approach, unchanged.
                if (shotCamOverride) {
                    flyX = shotCam[0]; flyY = shotCam[1]; flyZ = shotCam[2];
                    flyYaw = shotCam[3]; flyPitch = shotCam[4]; flyRoll = 0.0f;
                } else {
                    flyX = -900.0f; flyY = 320.0f; flyZ = 1400.0f;
                    flyYaw = std::atan2(-flyZ, -flyX);   // look at the island centre
                    flyPitch = -0.12f; flyRoll = 0.0f;
                }
                flySeeded = true;
            }
            flyYaw   += ddx * 0.0028f * g_sensMul;   // mouselook
            flyPitch -= ddy * 0.0028f * g_sensMul;
            // Arrow LEFT/RIGHT turn ("not strafing like A and D do").
            const float turnK = (kd(GLFW_KEY_RIGHT) ? 1.0f : 0.0f) - (kd(GLFW_KEY_LEFT) ? 1.0f : 0.0f);
            flyYaw += turnK * 1.6f * dt;
            // Q/E roll; gently self-rights while neither key is held.
            const float rollK = (kd(GLFW_KEY_E) ? 1.0f : 0.0f) - (kd(GLFW_KEY_Q) ? 1.0f : 0.0f);
            if (rollK != 0.0f) flyRoll += rollK * 1.2f * dt;
            else               flyRoll *= std::exp(-dt / 2.5f);
            flyPitch = clampf(flyPitch, -1.45f, 1.45f);
            flyRoll  = clampf(flyRoll,  -2.8f,  2.8f);
            // Move basis: full-3D forward (fly where you look), flat strafe right.
            const float cp = std::cos(flyPitch), sp = std::sin(flyPitch);
            const float cyw = std::cos(flyYaw),  syw = std::sin(flyYaw);
            const float mvF = (kd(GLFW_KEY_W)  ? 1.0f : 0.0f) - (kd(GLFW_KEY_S)    ? 1.0f : 0.0f)
                            + (kd(GLFW_KEY_UP) ? 1.0f : 0.0f) - (kd(GLFW_KEY_DOWN) ? 1.0f : 0.0f);
            const float mvR = (kd(GLFW_KEY_D) ? 1.0f : 0.0f) - (kd(GLFW_KEY_A) ? 1.0f : 0.0f);
            const float mvU = (kd(GLFW_KEY_SPACE) ? 1.0f : 0.0f) - (kd(GLFW_KEY_C) ? 1.0f : 0.0f);
            const float spd = (kd(GLFW_KEY_LEFT_SHIFT) ? 240.0f : 60.0f) * g_flySpeedMul;
            flyX += (cp * cyw * mvF - syw * mvR) * spd * dt;
            flyZ += (cp * syw * mvF + cyw * mvR) * spd * dt;
            flyY += (sp * mvF + mvU) * spd * dt;
            if (hf.ok()) flyY = std::max(flyY, hf.heightAt(flyX, flyZ) + 1.5f);  // stay out of the dirt
            flyY = clampf(flyY, 1.5f, 4000.0f);
        }

        // ===== PAUSE MENU: world + camera frozen, menu drawn, only QUIT exits.
        // Styled panel (Tim: "Menu is text based ugly") built from the same two
        // HUD primitives everything else uses — quads fake the panel/accents,
        // hover states light the buttons. Same keys as before: ESC/ENTER resume,
        // Q or click quits. =====
        if (menuOpen) {
            uint32_t hw = 0, hh = 0; device->hudSize(hw, hh);
            const float cx = hw * 0.5f, cy = hh * 0.5f;
            // Panel geometry: a centered card with two full-width buttons.
            const float pw = 520.0f, ph = 430.0f;
            const float px2 = cx - pw * 0.5f, py2 = cy - ph * 0.5f;
            const float bw = pw - 96.0f, bh = 54.0f;
            const float bx = cx - bw * 0.5f;
            const float byResume = py2 + 178.0f;
            const float byQuit   = byResume + bh + 18.0f;
            const bool overResume = (mx >= bx && mx <= bx + bw && my >= byResume && my <= byResume + bh);
            const bool overQuit   = (mx >= bx && mx <= bx + bw && my >= byQuit   && my <= byQuit   + bh);
            const bool lmb = mbd(GLFW_MOUSE_BUTTON_LEFT);
            const bool qkey = kd(GLFW_KEY_Q);
            if ((lmb && !prevLmb && overQuit) || (qkey && !prevQ)) wantQuit = true;
            const bool ent = kd(GLFW_KEY_ENTER);
            if ((ent && !prevEnter) || (lmb && !prevLmb && overResume)) menuOpen = false;
            prevLmb = lmb; prevQ = qkey; prevEnter = ent;

            auto frame = device->beginFrame();
            island.draw(*device, frame);
            props.draw(*device, frame);
            if (tod.sample().cityLightsOn) { beam.draw(*device, frame); fissure.draw(*device, frame); }

            // Palette (the HUD's own language: navy glass, gold title, cyan accents).
            const float dim[4]    = { 0.01f, 0.02f, 0.04f, 0.72f };   // world dim
            const float glow[4]   = { 0.10f, 0.55f, 0.70f, 0.10f };   // panel halo
            const float panel[4]  = { 0.045f, 0.065f, 0.105f, 0.96f };
            const float edge[4]   = { 0.16f, 0.60f, 0.74f, 0.85f };   // cyan accent
            const float gold[4]   = { 1.0f, 0.82f, 0.45f, 1.0f };
            const float white[4]  = { 0.92f, 0.95f, 1.0f, 1.0f };
            const float dimtxt[4] = { 0.55f, 0.61f, 0.72f, 1.0f };
            const float faint[4]  = { 0.10f, 0.14f, 0.20f, 1.0f };    // divider

            device->drawHudQuad(frame, 0, 0, (float)hw, (float)hh, dim);
            // Halo -> panel -> top accent bar -> bottom hairline.
            device->drawHudQuad(frame, px2 - 10.0f, py2 - 10.0f, pw + 20.0f, ph + 20.0f, glow);
            device->drawHudQuad(frame, px2, py2, pw, ph, panel);
            device->drawHudQuad(frame, px2, py2, pw, 4.0f, edge);
            device->drawHudQuad(frame, px2, py2 + ph - 2.0f, pw, 2.0f, faint);

            // Title block: ECHO HARBOR / ECHOTROPOLIS 2038 / PAUSED chip.
            device->drawHudText(frame, "ECHO  HARBOR", cx - 11 * 16.0f, py2 + 34.0f, 32.0f, gold);
            device->drawHudText(frame, "E C H O T R O P O L I S   //   2 0 3 8",
                                cx - 19 * 8.2f, py2 + 78.0f, 13.0f, dimtxt);
            { const float chip[4] = { 0.12f, 0.16f, 0.24f, 1.0f };
              device->drawHudQuad(frame, cx - 52.0f, py2 + 106.0f, 104.0f, 26.0f, chip);
              device->drawHudText(frame, "PAUSED", cx - 6 * 7.0f, py2 + 112.0f, 14.0f, white); }
            device->drawHudQuad(frame, px2 + 48.0f, py2 + 152.0f, pw - 96.0f, 1.0f, faint);

            // Buttons: hover fills + a left accent tick on the hot one.
            const float dangerEdge[4] = { 1.0f, 0.45f, 0.35f, 1.0f };
            auto button = [&](float byB, const char* label, bool hot, bool danger){
                const float base[4] = { danger ? 0.16f : 0.07f, danger ? 0.05f : 0.11f,
                                        danger ? 0.06f : 0.16f, 1.0f };
                const float hotc[4] = { danger ? 0.34f : 0.10f, danger ? 0.09f : 0.34f,
                                        danger ? 0.10f : 0.42f, 1.0f };
                device->drawHudQuad(frame, bx, byB, bw, bh, hot ? hotc : base);
                if (hot) device->drawHudQuad(frame, bx, byB, 4.0f, bh, danger ? dangerEdge : edge);
                device->drawHudText(frame, label, bx + 26.0f, byB + 18.0f, 16.0f,
                                    hot ? white : dimtxt);
            };
            button(byResume, "RESUME                                    ENTER", overResume, false);
            button(byQuit,   "QUIT  TO  DESKTOP                             Q", overQuit,   true);

            // Key hints: two aligned columns under the buttons.
            const float hintsY = byQuit + bh + 26.0f;
            device->drawHudText(frame, "1-4  time of day\n5-8  camera views\nT    run clock",
                                px2 + 64.0f, hintsY, 13.0f, dimtxt);
            device->drawHudText(frame, "WASD / drag  pan\nwheel  zoom\n`    console",
                                cx + 30.0f, hintsY, 13.0f, dimtxt);
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
              if (lb && !prevLB) { buildSel = (buildSel + kBuildCount - 1) % kBuildCount; uiSfx(sfxBuildTick, 0.5f); } prevLB = lb; }
            { const bool rb = kd(GLFW_KEY_RIGHT_BRACKET);
              if (rb && !prevRB) { buildSel = (buildSel + 1) % kBuildCount; uiSfx(sfxBuildTick, 0.5f); } prevRB = rb; }
            { const bool r = kd(GLFW_KEY_R);
              if (r && !prevRk) { buildYaw += 0.7853982f; uiSfx(sfxBuildTick, 0.45f); } prevRk = r; }   // +45 deg
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
                          uiSfx(sfxAccept, 0.85f);
                      }
                  } else uiSfx(sfxDeny, 0.7f);
              }
              prevPlace = pl; }
            // UNDO (Backspace): remove + refund the last placed lot.
            { const bool bk = kd(GLFW_KEY_BACKSPACE);
              if (bk && !prevBk && !placed.empty()) {
                  treasury += (double)placedCost.back();
                  placed.pop_back(); placedCost.pop_back();
                  uiSfx(sfxDeny, 0.65f);
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
            // T is CONTEXT-SENSITIVE: in walk mode with a citizen under the crosshair
            // and a live LLM it TALKS to them; otherwise it keeps its original job of
            // pausing the day clock (orbit mode / no pick / modelless are unchanged).
            const bool tNow = kd(GLFW_KEY_T);
            if (tNow && !prevT) {
                const bool canTalk = walkMode && npcLifeBuilt && talkLlm && lastPickedIdx >= 0;
                if (canTalk) talkAsk(lastPickedIdx);
                else         todPaused = !todPaused;
            }
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
            if (!todPaused) tod.advance(dt * g_todSpeed);
            if (tod.phase() != prevPhase) {
                prevPhase = tod.phase();
                x3::logInfo(std::string("--world echotropolis: ") +
                            x3::game::todPhaseName(prevPhase));
            }
        }
        zInput.stop();                                   // LANE 6 span boundary
        x3::perf::HostScope zStream(x3::perf::Z_HostStream);
        // ===== TIER-2 MILESTONE B: the streamer TICKS ========================
        // Wants come from the ACTIVE CAMERA (plan 6.1: in this world the camera
        // IS the player), with the VISTA RULE keeping the whole island resident
        // whenever the view could see it all: high above ground, wide orbit, or
        // console-boosted speed. 5s hysteresis on leaving vista so dips don't
        // thrash. ECHO_STREAM=0 = the rollback lever (never ticks; M-A behavior).
        if (!kStreamOff && physOk) {
            float scx, scy, scz;
            if (flyMode) { scx = flyX; scy = flyY; scz = flyZ; }
            else if (followIdx >= 0 && npcLifeBuilt && (uint32_t)followIdx < npcLife.agentCount()) {
                const auto& sa = npcLife.agent((uint32_t)followIdx);
                scx = sa.pos.x; scy = sa.pos.y + 2.0f; scz = sa.pos.z;
            } else if (walkMode) {
                float yw2, pt2; player.camera(scx, scy, scz, yw2, pt2);
            } else {
                scx = rig.sFocusX; scz = rig.sFocusZ;
                scy = rig.sPivotY + rig.sRadius * std::sin(rig.sPitch);
            }
            static float spx = 0, spy = 0, spz = 0; static bool sSeed = false;
            float svx = 0, svy = 0, svz = 0;
            if (sSeed && dt > 1e-5f) { svx = (scx-spx)/dt; svy = (scy-spy)/dt; svz = (scz-spz)/dt; }
            spx = scx; spy = scy; spz = scz; sSeed = true;
            const float sGround = hf.ok() ? hf.heightAt(scx, scz) : 0.0f;
            const float sSpeed  = std::sqrt(svx*svx + svy*svy + svz*svz);
            const bool orbitNow = !flyMode && !walkMode && followIdx < 0;
            const bool wantVista = (scy - sGround > 250.0f) ||
                                   (orbitNow && rig.sRadius > 900.0f) ||
                                   (sSpeed > 600.0f);
            static double vistaUntil = -1e18;  // no pending vista; boot's aerial spawn earns it naturally
            if (wantVista) vistaUntil = now + 5.0;
            const bool vista = wantVista || now < vistaUntil;
            regionSet.setVistaMode(vista);
            if (!vista) {
                const float wsB = std::max(0.25f, console->getFloat("ws_budget"));
                regionStreamer.update(walkScene, *device, *phys,
                                      scx, scy, scz, svx, svy, svz, (double)wsB);
            }
        }

        zStream.stop();                                  // LANE 6 span boundary
        x3::perf::HostScope zSim(x3::perf::Z_HostSim);
        // HOST-SIM LANE 2026-08: cpu.host_sim was 52 % of the frame in ONE row. The
        // X3_HOST_ZONE spans below are EXCLUSIVE (HostScope), nest inside zSim, and
        // therefore turn zSim itself into the residual. They are all strictly inside
        // the span — none straddles beginFrame()'s frameAccum().reset(), which would
        // underflow the subtraction and silently report 0.000 (see zSim.stop() below).
        {   X3_HOST_ZONE(Z_SimTod);
            syncCVars();   // r_* console cvars -> device (before the atmosphere writes post)
        }
        const x3::game::TodSample todS = tod.sample();
        {   X3_HOST_ZONE(Z_SimTod);
            applyTodSample(device, todS);
            applyAtmosphere(device, todS);   // ATMOSPHERE: aerial haze + grade + bloom
        }

        // RESIDENTS: the crowd lives every frame (orbit or walk); the skinned layer
        // drains its deferred spawn queue and pose-follows the agents.
        if (residentsBuilt) {
            { X3_HOST_ZONE(Z_SimResidents); residents.update(dt, walkScene); }
            if (npcLifeBuilt) {
                // living-city schedules advance
                { X3_HOST_ZONE(Z_SimNpcLife); npcLife.update(dt, walkScene); }
                // rigged bodies follow
                { X3_HOST_ZONE(Z_SimNpcSkin); npcSkin.update(dt, npcLife, walkScene, *device, *phys); }
            }
            { X3_HOST_ZONE(Z_SimResSkin); residentsSkin.update(dt, residents, walkScene, *device, *phys); }
        }
        // WD2 HEAT: the city's alert state decays toward calm each frame; the
        // street cops are its eyes (they hear hacks city-wide already).
        {
            X3_HOST_ZONE(Z_SimAlert);
            float apx = flyX, apy = flyY, apz = flyZ;
            if (walkMode) { float ayw, apt; player.camera(apx, apy, apz, ayw, apt); }
            cityAlert.update(dt, { apx, apy, apz }, nullptr, 0, false);
        }
        if (hackCardT > 0.0f) hackCardT -= dt;
        if (trafficSpoofT > 0.0f) trafficSpoofT -= dt;
        {   X3_HOST_ZONE(Z_SimTalk);
            talkPoll();       // CITIZEN TALK: drain streamed reply tokens (non-blocking, every frame)
            ambientTick(dt);  // AMBIENT CHATTER: occasionally an idle citizen mutters (opt-in)
            ambientPoll();
        }
        if (minersBuilt) {                 // GOLD-MINE crew lives + hauls every frame
            X3_HOST_ZONE(Z_SimMiners);
            miners.update(dt, walkScene);
            minersSkin.update(dt, miners, walkScene, *device, *phys);
        }
        if (oh1Built) { X3_HOST_ZONE(Z_SimOh1); flyOh1(oh1, waterTime); oh1.update(dt, walkScene, *phys, oh1.pos()); }
        if (opsBuilt) {                 // CONTROL ROOM: refresh the live dashboard
            X3_HOST_ZONE(Z_SimOps);
            opsScreen.setLines(opsLines(todS));
            opsScreen.update(dt);
        }

        // Ocean + camera + render. WALK MODE poses the first-person eye camera from
        // the physics character; ORBIT MODE keeps the strategic vista camera.
        waterTime += dt;
        {   X3_HOST_ZONE(Z_SimOcean);
            // WATER: the Gerstner patch gates on the ACTIVE eye's height (see
            // applyOcean) — fly camera, walk eye, ride-along, or orbit pivot.
            float wex = 0, wey = 0, wez = 0, wyw = 0, wpt = 0;
            if (flyMode) { wey = flyY; }
            else if (walkMode && physOk) { player.camera(wex, wey, wez, wyw, wpt); }
            else if (followIdx >= 0 && npcLifeBuilt && (uint32_t)followIdx < npcLife.agentCount())
                wey = npcLife.agent((uint32_t)followIdx).pos.y + 2.0f;
            else wey = rig.sPivotY + rig.sRadius * std::sin(rig.sPitch);
            applyOcean(device, waterTime, todS, wey);
        }
        // Roll is per-frame device state like setCamera: latch it every frame so
        // leaving fly mode always returns an upright horizon.
        // (Delimited span — the if/else camera chain below is not a single block.)
        x3::perf::HostScope zCam(x3::perf::Z_SimCamera);
        device->setCameraRoll(flyMode ? flyRoll : 0.0f);
        if (worldCars.driving()) {
            // NFS VIEW CYCLE: the stock chase boom scaled per view; HOOD rides
            // the body (no interior yet — dashboard view lands with the
            // interiors pillar). Mouse orbits every view.
            const x3::phys::Vec3 cp2 = worldCars.carPosition();
            if (shopBuilt && shop.shopMode()) {
                float oc[5]; shop.orbitCam(oc);   // the shop owns the frame
                device->setCamera(oc[0], oc[1], oc[2], oc[3], oc[4], opt.fovDeg);
            } else if (carView == 3) {
                const float hx = cp2.x + std::cos(driveCamYaw) * 0.9f;
                const float hz = cp2.z + std::sin(driveCamYaw) * 0.9f;
                device->setCamera(hx, cp2.y + 1.25f, hz, driveCamYaw, driveCamPitch * 0.4f, opt.fovDeg + 6.0f);
            } else {
                float dcx, dcy, dcz;
                worldCars.driverCamera(driveCamYaw, driveCamPitch, dcx, dcy, dcz);
                const float k2 = kCarViewDist[carView];
                device->setCamera(cp2.x + (dcx - cp2.x) * k2,
                                  cp2.y + (dcy - cp2.y) * k2 + (carView == 0 ? 0.8f : 0.0f),
                                  cp2.z + (dcz - cp2.z) * k2,
                                  driveCamYaw, driveCamPitch, opt.fovDeg);
            }
        } else if (flyMode) {
            device->setCamera(flyX, flyY, flyZ, flyYaw, flyPitch, opt.fovDeg);
        } else if (followIdx >= 0 && npcLifeBuilt && (uint32_t)followIdx < npcLife.agentCount()) {
            // RIDE-ALONG: trail the citizen third-person, looking at their head as they
            // walk their scheduled day. Camera sits back+up along their facing.
            const auto& a = npcLife.agent((uint32_t)followIdx);
            const float back = 4.6f, up = 2.7f, headY = 1.5f;
            const float camx = a.pos.x - std::cos(a.yaw) * back;
            const float camz = a.pos.z - std::sin(a.yaw) * back;
            // TERRAIN CLEARANCE (the "model only visible in a narrow arc" bug):
            // it wasn't culling — a terrain rise between the trailing camera and
            // the citizen blocked the LINE OF SIGHT at most yaws (the fly-cam
            // inspection saw her fine from a clear bearing). Crane the camera up
            // until the whole cam->head ray clears the heightfield.
            float camy = a.pos.y + up;
            if (hf.ok()) {
                camy = std::max(camy, hf.heightAt(camx, camz) + 0.6f);
                const float ty = a.pos.y + headY;
                for (int s2 = 1; s2 <= 6; ++s2) {
                    const float t = (float)s2 / 7.0f;
                    const float sx = camx + (a.pos.x - camx) * t;
                    const float sz = camz + (a.pos.z - camz) * t;
                    const float g  = hf.heightAt(sx, sz) + 0.35f;
                    if (camy + (ty - camy) * t < g)
                        camy = std::max(camy, (g - ty * t) / (1.0f - t));
                }
            }
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
        zCam.stop();
        // AUDIO listener rides the active camera so positional hums pan/attenuate.
        if (audioOn) {
            X3_HOST_ZONE(Z_SimAudio);
            float lx, ly, lz, lyaw, lpit;
            if (followIdx >= 0 && npcLifeBuilt && (uint32_t)followIdx < npcLife.agentCount()) {
                const auto& a = npcLife.agent((uint32_t)followIdx);
                lx=a.pos.x; ly=a.pos.y+2.0f; lz=a.pos.z; lyaw=a.yaw; lpit=0.0f;
            } else if (flyMode) {
                lx=flyX; ly=flyY; lz=flyZ; lyaw=flyYaw; lpit=flyPitch;
            } else if (walkMode && physOk) {
                player.camera(lx, ly, lz, lyaw, lpit);
            } else {
                lx=rig.sFocusX; ly=rig.sPivotY+40.0f; lz=rig.sFocusZ; lyaw=rig.sYaw; lpit=rig.sPitch;
            }
            eaudio->setListener(lx, ly, lz, lyaw, lpit);

            // ---- LIVING-CITY AUDIO frame step (additive; see decl block) ----
            // Day bed: birds fade in with the sun (silent at night, full by mid-day).
            const float dayness = clampf(todS.sunElevation * 4.0f, 0.0f, 1.0f);
            if (loopDayBirds.valid()) eaudio->setLoopParams(loopDayBirds, 0.34f * dayness, 1.0f);

            // Rotor thump: manual attenuation of each 2D rotor loop against the
            // listener (loops can't be re-positioned; see decl comment). ref=60 m.
            {
                const float ref2 = 60.0f * 60.0f;
                auto rotorVol = [&](float x, float y, float z, float base){
                    const float dx = x - lx, dy = y - ly, dz = z - lz;
                    const float d2 = dx*dx + dy*dy + dz*dz;
                    return base * (ref2 / (ref2 + d2));
                };
                for (size_t i = 0; i < rotorVoices.size(); ++i) {
                    float hx, hy, hz;
                    if (i < helis.size()) {          // patrol helis: circle pose (poseHeli math)
                        const Heli& h = helis[i];
                        const float a = h.phase + waterTime * h.w;
                        hx = h.cx + std::cos(a) * h.r; hz = h.cz + std::sin(a) * h.r;
                        hy = h.y + std::sin(waterTime * 0.5f + h.phase) * 3.0f;
                    } else if (oh1Built) {           // OH1 hero: live prop position
                        const auto p = oh1.pos(); hx = p.x; hy = p.y; hz = p.z;
                    } else break;
                    eaudio->setLoopParams(rotorVoices[i].lh,
                                          rotorVol(hx, hy, hz, 0.9f), rotorVoices[i].pitch);
                }
            }

            // Mine work: shovel bites + ore clanks at the seam while the crew hauls.
            if (minersBuilt) {
                mineWorkT -= dt;
                if (mineWorkT <= 0.0f) {
                    mineWorkT = 2.2f + audRand01() * 2.6f;
                    const auto& s = (mineWorkAlt++ & 1)
                        ? sfxClank[audRng & 1] : sfxShovel[audRng & 1];
                    if (s.valid())
                        eaudio->playSound3D(s, kMineX + (audRand01() - 0.5f) * 10.0f,
                                            kMineGy + 1.0f, kMineZ + (audRand01() - 0.5f) * 10.0f,
                                            0.85f, 0.92f + audRand01() * 0.16f);
                }
            }

            // Drone flybys: an occasional pass fired AT a live drone's position.
            //
            // BUILD FIX (sync-val lane): the audio commit that added this block was
            // written against the host-local `drones` vector, which the earlier
            // TIER-2 region split (a99b6ed9) had ALREADY moved into
            // echo_region_builders.cpp as the function-local `dronePoses` — so main
            // did not compile. The orbits are deterministic constants, so mirror
            // them here (verbatim from buildCrown's addDrone calls) and use the
            // SAME pose formula the region builder animates them with. Positions
            // are therefore identical to the live drones; nothing else changed.
            // If dronePoses is ever exposed through EchoRegionCtx, delete this
            // table and read it instead.
            struct DroneOrbit { float cx, cz, r, y, w, phase; };
            static constexpr DroneOrbit kDroneOrbits[] = {
                { -20.0f,  760.0f, 150.0f, 210.0f,  0.26f, 0.0f },
                { 110.0f,  660.0f, 120.0f, 250.0f, -0.32f, 1.7f },
                { -160.0f, 840.0f, 180.0f, 190.0f,  0.22f, 3.1f },
                { -60.0f,  900.0f, 130.0f, 285.0f,  0.30f, 4.4f },
                {  60.0f,  800.0f, 200.0f, 165.0f, -0.24f, 5.5f },
                { -220.0f, 700.0f, 110.0f, 300.0f, -0.28f, 2.3f },
            };
            {
                flybyT -= dt;
                if (flybyT <= 0.0f) {
                    flybyT = 12.0f + audRand01() * 14.0f;
                    const DroneOrbit& d = kDroneOrbits[audRng % (sizeof(kDroneOrbits) / sizeof(kDroneOrbits[0]))];
                    const float a = d.phase + waterTime * d.w;
                    const auto& s = sfxFlyby[audRng % 3];
                    if (s.valid())
                        eaudio->playSound3D(s, d.cx + std::cos(a) * d.r,
                                            d.y, d.cz + std::sin(a) * d.r, 0.55f, 1.0f);
                }
            }

            // Gold economy: a coin clink AT THE MINE every 5 oz banked; the treasury
            // cha-ching on each day rollover (the daily net lands).
            if ((int)(goldOz / 5.0) > goldMilestone) {
                goldMilestone = (int)(goldOz / 5.0);
                if (sfxGoldCoin.valid())
                    eaudio->playSound3D(sfxGoldCoin, kMineX, kMineGy + 1.5f, kMineZ, 0.8f, 1.0f);
            }
            if (simDay != audPrevDay) {
                audPrevDay = simDay;
                if (sfxChaChing.valid()) eaudio->playSound2D(sfxChaChing, 0.7f, 1.0f);
            }
            // Time-of-day phase change: one soft chime (lamps-on, dawn, ...).
            if (todS.phase != audPrevPhase) {
                audPrevPhase = todS.phase;
                if (sfxPhaseChime.valid()) eaudio->playSound2D(sfxPhaseChime, 0.45f, 1.0f);
            }

            // Walk-mode footsteps: distance-driven street steps (your own feet, 2D).
            if (walkMode && physOk && followIdx < 0) {
                float px_, py_, pz_, pyaw_, ppit_;
                player.camera(px_, py_, pz_, pyaw_, ppit_);
                if (!stepInit) { stepPX = px_; stepPZ = pz_; stepInit = true; }
                const float ddx2 = px_ - stepPX, ddz2 = pz_ - stepPZ;
                stepAccum += std::sqrt(ddx2*ddx2 + ddz2*ddz2);
                stepPX = px_; stepPZ = pz_;
                if (stepAccum >= 2.1f) {
                    stepAccum = 0.0f;
                    const auto& s = sfxStep[audRng & 3];
                    if (s.valid()) eaudio->playSound2D(s, 0.32f, 0.95f + audRand01() * 0.1f);
                }
            } else { stepInit = false; stepAccum = 0.0f; }

            eaudio->update(dt);
        }

        // Per-frame state — read once at endFrame (see the headless path note);
        // position vs beginFrame/draws is a no-op. Draw/submit calls are NOT.
        { X3_HOST_ZONE(Z_SimLamps); streetLamps.update(dt, lampScene); }   // flicker machines
        {
            X3_HOST_ZONE(Z_SimLights);
            float lx = rig.sFocusX, ly = rig.sPivotY + 20.0f, lz = rig.sFocusZ;
            if (flyMode) { lx = flyX; ly = flyY; lz = flyZ; }
            else if (walkMode && physOk) { float yw, pt; player.camera(lx, ly, lz, yw, pt); }
            std::vector<x3::rhi::PointLight> pls;
            streetLamps.selectLights(lx, ly, lz, pls, 8);
            if (shopBuilt) shop.selectLights(lx, ly, lz, pls, 8);   // shop interior glow
            regionSet.appendNearLights(lx, lz, pls, 64);   // TIER-2: was appendDistrictLights
            // ECHO ROADS lamps: nearest-first from the static slice, ONLY at night
            // (the module bakes no emissive by design — the day/night gate lives
            // here, same as the street lamps; 64-light device cap respected).
            if (roads && tod.sample().cityLightsOn && pls.size() < 64) {
                X3_HOST_ZONE(Z_SimRoadLights);
                const auto& rl = roads->lights();
                std::vector<std::pair<float,const x3::rhi::PointLight*>> near2;
                near2.reserve(rl.size());
                for (const auto& L2 : rl) {
                    const float dx2 = L2.pos[0] - lx, dz2 = L2.pos[2] - lz;
                    near2.push_back({ dx2*dx2 + dz2*dz2, &L2 });
                }
                std::sort(near2.begin(), near2.end(),
                          [](const auto& a2, const auto& b2){ return a2.first < b2.first; });
                for (const auto& pr2 : near2) {
                    if (pls.size() >= 64) break;
                    pls.push_back(*pr2.second);
                }
            }
            device->setPointLights(pls.empty() ? nullptr : pls.data(), (uint32_t)pls.size());
        }

        // LANE 6 SPAN BOUNDARY — and it MUST be exactly here. beginFrame() calls
        // accumulateCpuZones() + frameAccum().reset() (VulkanRenderDevice.cpp:1049):
        // that is the accumulator's frame edge. A host span that STRADDLES it sees
        // `attributed` snap back to 0 and its exclusive subtraction underflows, so
        // the span silently reports 0.000 ms forever. (First cut of this wiring did
        // exactly that and cpu.host_drawfans read 0.000 in every window — the
        // instrumentation catching its own bug for the second time.) So: zSim ends
        // BEFORE the reset, zFans begins AFTER it, and neither crosses.
        zSim.stop();
        auto frame = device->beginFrame();
        x3::perf::HostScope zFans(x3::perf::Z_HostDrawFans);
        island.draw(*device, frame);
        props.draw(*device, frame);    // P4 coast dressing (lighthouse/dock/boats/skyline)
        // TIER-2 M-A: houses/towers/mine/streets+metro/condos/districts/hackables/
        // mineForest/woodlands/mineGlow/subway/boats/drones draw via the regions.
        // updateAll(waterTime) == the old pose*(waterTime) calls verbatim.
        regionSet.updateAll(dt, waterTime);
        regionSet.drawAll(*device, frame);
        for (auto& r : infra) r->draw(*device, frame);       // freeway decks (host-persistent)
        lampScene.render(*device, frame);                    // posts + cones + pools
        for (auto& p : placed) p->draw(*device, frame);      // BUILD MENU: player-placed lots
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
        if (oh1Built) oh1.drawMonster(*device, frame, walkScene);   // OH1 hero heli
        if (residentsBuilt) {          // the citizens (blockout agents + rigged skins)
            walkScene.render(*device, frame);
            residentsSkin.draw(*device, frame, walkScene);
        } else if (minersBuilt) {      // miners share walkScene; render it if residents didn't
            walkScene.render(*device, frame);
        }
        if (npcLifeBuilt) npcSkin.draw(*device, frame, walkScene);   // named-citizen rigs
        for (auto& sp : streetProps) sp->draw(*device, frame);       // real vendor carts
        if (worldCars.built()) worldCars.draw(frame);                // CARS: parked fleet + live rig
        if (roads && todS.cityLightsOn) roads->drawNightGlow(*device, frame);   // V7 lamp heads (night)
        if (roads) roads->draw(*device, frame);                      // ECHO ROADS curved network
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
            // ONE CLOCK OWNS TIME (Tim's 12:02-noon-HUD-under-midnight-stars
            // shot): the HUD clock now READS the sky's day fraction instead of
            // running its own kSimDayLen race. Economy pacing below still uses
            // kSimDayLen as its accrual rate — money speed is a design knob,
            // display time is the sky's truth. Day increments on the sky's
            // midnight wrap.
            { const float prevSim = simClock;
              simClock = tod.dayFraction();
              if (simClock < prevSim - 0.5f) ++simDay; }
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
                "%d  %s DAY %d    %02d:%02d    POP %u    MINERS %u    GOLD %0.0f oz    $%0.0f    %d FPS",
                simYear, kDow[(simDay - 1) % 7], simDay, hour, minute, pop, miners, goldOz, treasury,
                (int)(hudFps + 0.5));
            const float pad = 14.0f, glyph = 14.0f, barH = 34.0f;
            const float barW = std::min((float)hw - 24.0f, 1120.0f);
            const float bg[4]   = { 0.04f, 0.06f, 0.10f, 0.82f };
            const float gold[4] = { 1.0f, 0.82f, 0.42f, 1.0f };
            device->drawHudQuad(frame, 12.0f, 12.0f, barW, barH, bg);
            device->drawHudText(frame, bar, 12.0f + pad, 12.0f + (barH - glyph) * 0.5f, glyph, gold);
            {   // MODE LABEL (Tim: "visual on screen, silver text"): always shown.
                const float silver[4] = { 0.80f, 0.83f, 0.88f, 0.95f };
                const char* mode =
                    worldCars.driving() ? (carView == 0 ? "DRIVING [FAR CHASE]   WASD drive   SPACE handbrake   C view   F exit"
                                        :  carView == 1 ? "DRIVING [MID CHASE]   WASD drive   SPACE handbrake   C view   F exit"
                                        :  carView == 2 ? "DRIVING [CLOSE]   WASD drive   SPACE handbrake   C view   F exit"
                                                        : "DRIVING [HOOD]   WASD drive   SPACE handbrake   C view   F exit")
                  : flyMode ? "FLIGHT   WASD + mouse   Q/E roll   SPACE/C up-down   ARROWS fwd + turn   SHIFT fast   G ground   V orbit"
                  : (followIdx >= 0) ? (playAs ? "PLAY AS   WASD drive   E spectate   F release"
                                               : "RIDE-ALONG   E take controls   F release")
                  : walkMode ? "GROUND   WASD + mouse   SHIFT run   SPACE jump   G flight   T talk   H scan   K skills"
                  : "ORBIT   drag pan   RMB rotate   wheel zoom   G ground   V flight";
                device->drawHudText(frame, mode, 12.0f + pad, 12.0f + barH + 20.0f, 12.0f, silver);
                if (worldCars.built() && !worldCars.prompt().empty()) {
                    const float pgold[4] = { 1.0f, 0.85f, 0.45f, 1.0f };
                    device->drawHudText(frame, worldCars.prompt().c_str(),
                                        12.0f + pad, 12.0f + barH + 40.0f, 14.0f, pgold);
                } else if (vendorPrompt) {
                    const float pgold[4] = { 1.0f, 0.85f, 0.45f, 1.0f };
                    device->drawHudText(frame, vendorPrompt,
                                        12.0f + pad, 12.0f + barH + 40.0f, 14.0f, pgold);
                } else if (doorPrompt) {
                    const float pgold[4] = { 0.75f, 0.95f, 1.0f, 1.0f };
                    device->drawHudText(frame, doorPrompt,
                                        12.0f + pad, 12.0f + barH + 40.0f, 14.0f, pgold);
                } else if (hackAim != x3::game::kNoLink && hax.highlight()) {
                    const auto& ho = hax.at(hackAim);
                    const std::string hp = std::string("[E] HACK - ") +
                        x3::game::hackableEffectVerb(ho.type) +
                        (ho.label.empty() ? "" : ("   (" + ho.label + ")")) +
                        (ho.hacked ? "   [SPENT]" : "");
                    const float pcyan[4] = { 0.45f, 0.95f, 1.0f, 1.0f };
                    device->drawHudText(frame, hp.c_str(),
                                        12.0f + pad, 12.0f + barH + 40.0f, 14.0f, pcyan);
                }
            }

            // ---- WD2 NETHACK OVERLAY: hold H — every hackable in 60 m gets a
            // through-wall marker (HUD pass has no depth test — the wallhack
            // comes free). Aimed target draws hot; the rest cool cyan.
            if (hax.highlight() && walkMode) {
                float mex, mey, mez, myw, mpt;
                player.camera(mex, mey, mez, myw, mpt);
                std::vector<uint32_t> near2;
                hax.nearby({ mex, mey, mez }, 60.0f, near2);
                for (uint32_t hi : near2) {
                    const auto& o = hax.at(hi);
                    float sx = 0.0f, sy = 0.0f;
                    if (!device->worldToScreen(o.pos.x, o.pos.y, o.pos.z, sx, sy))
                        continue;
                    const bool aimed = (hi == hackAim);
                    const float a2 = o.hacked ? 0.35f : (aimed ? 1.0f : 0.75f);
                    float mc[4] = { 0.45f, 0.95f, 1.0f, a2 };          // cool cyan
                    if (aimed) { mc[0] = 1.0f; mc[1] = 0.85f; mc[2] = 0.3f; }
                    const float s2 = aimed ? 10.0f : 6.0f;
                    device->drawHudQuad(frame, sx - s2 * 0.5f, sy - s2 * 0.5f,
                                        s2, s2, mc);
                    if (aimed || o.type == x3::game::HackableType::Npc) {
                        const float dxm = o.pos.x - mex, dzm = o.pos.z - mez;
                        const int dm = (int)std::sqrt(dxm*dxm + dzm*dzm);
                        std::string tag = std::string(
                            x3::game::hackableTypeName(o.type)) +
                            " " + std::to_string(dm) + "m";
                        device->drawHudText(frame, tag.c_str(),
                                            sx + 8.0f, sy - 6.0f, 10.0f, mc);
                    }
                }
            }

            // ---- WD2 HACK CARD: the scan-card / effect confirm (5 s hold).
            if (hackCardT > 0.0f) {
                uint32_t hw2 = 0, hh2 = 0; device->hudSize(hw2, hh2);
                const float cw = 420.0f, chh = hackCard.scanName.empty() ? 96.0f : 150.0f;
                const float cx2 = hw2 * 0.5f - cw * 0.5f;
                const float cy2 = hh2 - chh - 96.0f;
                const float fade = hackCardT < 0.8f ? hackCardT / 0.8f : 1.0f;
                const float bg[4] = { 0.03f, 0.07f, 0.09f, 0.82f * fade };
                const float rim[4] = { 0.45f, 0.95f, 1.0f, 0.9f * fade };
                const float ink[4] = { 0.75f, 0.95f, 1.0f, fade };
                const float gold[4] = { 1.0f, 0.85f, 0.45f, fade };
                device->drawHudQuad(frame, cx2, cy2, cw, chh, bg);
                device->drawHudQuad(frame, cx2, cy2, cw, 2.0f, rim);
                std::string head = std::string(
                    x3::game::hackableTypeName(hackCard.type)) + " HACKED";
                device->drawHudText(frame, head.c_str(), cx2 + 16.0f, cy2 + 14.0f, 13.0f, rim);
                device->drawHudText(frame, hackCard.effect.c_str(),
                                    cx2 + 16.0f, cy2 + 36.0f, 12.0f, ink);
                float ly = cy2 + 58.0f;
                if (!hackCard.scanName.empty()) {
                    device->drawHudText(frame, hackCard.scanName.c_str(),
                                        cx2 + 16.0f, ly, 14.0f, gold); ly += 22.0f;
                    device->drawHudText(frame, hackCard.scanOccupation.c_str(),
                                        cx2 + 16.0f, ly, 11.0f, ink); ly += 18.0f;
                    device->drawHudText(frame, hackCard.scanDetail.c_str(),
                                        cx2 + 16.0f, ly, 11.0f, ink); ly += 18.0f;
                }
                if (hackCard.credits > 0) {
                    const std::string cr = "+$" + std::to_string(hackCard.credits) + " SKIMMED";
                    device->drawHudText(frame, cr.c_str(), cx2 + 16.0f, ly, 12.0f, gold);
                }
            }

            // ---- WD2 HEAT + KARMA readout (top-right, only when non-calm).
            {
                uint32_t hw3 = 0, hh3 = 0; device->hudSize(hw3, hh3);
                const int alvl = cityAlert.level();
                if (alvl > 0) {
                    const float hot[4] = { 1.0f, 0.35f + 0.1f * (3 - std::min(alvl, 3)),
                                           0.25f, 1.0f };
                    std::string ht = std::string("HEAT ") +
                        x3::game::alertLevelName(alvl);
                    device->drawHudText(frame, ht.c_str(),
                                        hw3 - 14.0f * (float)ht.size() - 24.0f,
                                        58.0f, 14.0f, hot);
                }
            }

            // ---- RPG chip: LV + XP sliver (walking/driving, no other surface).
            if ((walkMode || worldCars.driving()) && !menuOpen && !buildMode)
                rpgUi.drawHudChip(*device, frame, rpgInv, rpgItems, progression, dt);

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
                // Riding along: show whom you're following + the mode/controls banner.
                const float ban[4] = { 0.10f, 0.14f, 0.20f, 0.92f };
                const float gold[4] = { 1.0f, 0.82f, 0.42f, 1.0f };
                const char* bl = playAs ? "PLAY AS   //   WASD move   E spectate   F release"
                                        : "SPECTATING   //   E play as   F release";
                const float bw = 470.0f;
                device->drawHudQuad(frame, hw*0.5f - bw*0.5f, 54.0f, bw, 30.0f, ban);
                device->drawHudText(frame, bl, hw*0.5f - bw*0.5f + 18.0f, 61.0f, 13.0f, gold);
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
                    if (talkLlm) {
                        // CITIZEN TALK affordance: the line you'll send + the cycle keys.
                        char th[192];
                        std::snprintf(th, sizeof th, "F  ride along   |   T  say: \"%s\"   [ ] change",
                                      kTalkPrompts[talkPromptIdx]);
                        device->drawHudText(frame, th, 24.0f + 18.0f,
                                            (float)hh - 176.0f - 22.0f, 12.0f, rc);
                    } else {
                        device->drawHudText(frame, "F  ride along  /  play as",
                                            24.0f + 18.0f, (float)hh - 176.0f - 22.0f, 12.0f, rc);
                    }
                }
            } else {
                lastPickedIdx = -1;
            }

            // ---------------- CITIZEN TALK: the floating chat bubble ----------------
            // Anchored 2.2 m over the agent's feet and projected with worldToScreen
            // (one frame stale by design — imperceptible for a bubble). Hidden when
            // the citizen is behind the camera / off-screen (projection fails).
            if (talk.agent >= 0 && npcLifeBuilt && (uint32_t)talk.agent < npcLife.agentCount()) {
                const auto& ta = npcLife.agent((uint32_t)talk.agent);
                float sx = 0.0f, sy = 0.0f;
                if (device->worldToScreen(ta.pos.x, ta.pos.y + 2.2f, ta.pos.z, sx, sy)) {
                    // Fade in on open, out at the end of the hold window.
                    float alpha = 1.0f;
                    if (!talk.pending && talk.endAt > 0.0) {
                        const double left = talk.endAt - glfwGetTime();
                        if (left < (double)kBubbleFadeSec)
                            alpha = std::clamp((float)left / kBubbleFadeSec, 0.0f, 1.0f);
                    }
                    // Trim the model's leading whitespace so the first line sits flush.
                    std::string body = talkTidy(talk.reply);
                    if (body.empty()) body = talk.pending ? "..." : "(silence)";
                    else if (talk.pending) body += " ...";   // streaming/thinking indicator
                    std::vector<std::string> wrapped;
                    talkWrap(*device, body, 15.0f, 340.0f, 5, wrapped);
                    drawTalkBubble(*device, frame, sx, sy, ta.name.c_str(), wrapped, alpha);
                }
            }
            // AMBIENT CHATTER bubble — dimmer than the player's conversation so the
            // one you are actually having always reads as the primary.
            if (amb.agent >= 0 && !amb.pending && npcLifeBuilt &&
                (uint32_t)amb.agent < npcLife.agentCount()) {
                const auto& aa = npcLife.agent((uint32_t)amb.agent);
                float sx = 0.0f, sy = 0.0f;
                if (device->worldToScreen(aa.pos.x, aa.pos.y + 2.2f, aa.pos.z, sx, sy)) {
                    const double now2 = glfwGetTime();
                    const double left = amb.endAt - now2;
                    const float alpha = 0.8f * std::clamp((float)left / kBubbleFadeSec, 0.0f, 1.0f);
                    std::string body = talkTidy(amb.line);
                    // HUMAN SPEECH PACING (Tim: instant lines read as machine-gun).
                    // Reveal ~22 chars/sec from when the bubble opened.
                    const double talking = now2 - (amb.endAt - (double)kAmbientHoldSec);
                    const size_t reveal = (size_t)std::max(0.0, talking * 22.0);
                    if (reveal < body.size()) body = body.substr(0, reveal) + "...";
                    if (!body.empty()) {
                        std::vector<std::string> wrapped;
                        talkWrap(*device, body, 15.0f, 300.0f, 3, wrapped);
                        drawTalkBubble(*device, frame, sx, sy, aa.name.c_str(), wrapped, alpha);
                    }
                }
                // CITY FEED (Tim's ask): every finished line joins a rolling feed.
                if (amb.agent != fedAmbAgent || amb.line != fedAmbLine) {
                    fedAmbAgent = amb.agent; fedAmbLine = amb.line;
                    std::string ln = aa.name + ": " + amb.line;
                    if (ln.size() > 88) ln = ln.substr(0, 85) + "...";
                    talkFeed.push_back(ln);
                    if (talkFeed.size() > 6) talkFeed.erase(talkFeed.begin());
                }
            }
            // THE FEED — bottom-right, last 6 city voices, dim console style.
            if (!talkFeed.empty()) {
                const float fw = 470.0f, lh = 18.0f;
                const float fx = (float)hw - fw - 20.0f;
                float fy = (float)hh - 30.0f - lh * (float)talkFeed.size();
                const float fbg[4] = { 0.03f, 0.05f, 0.09f, 0.55f };
                const float ftx[4] = { 0.75f, 0.85f, 0.95f, 0.85f };
                device->drawHudQuad(frame, fx - 10.0f, fy - 8.0f, fw + 20.0f,
                                    lh * (float)talkFeed.size() + 16.0f, fbg);
                for (const std::string& ln : talkFeed) {
                    device->drawHudText(frame, ln.c_str(), fx, fy, 11.0f, ftx);
                    fy += lh;
                }
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
        zFans.stop();                                    // LANE 6 span boundary
        device->endFrame(frame);
        if (!g_playasCapPath.empty()) {   // PLAYAS_DEMO: finalize the armed copy
            device->captureFrame(g_playasCapPath.c_str());
            g_playasCapPath.clear();
        }
        if (!g_shotPath.empty()) {        // console `screenshot`: same finalize
            device->captureFrame(g_shotPath.c_str());
            g_shotPath.clear();
        }

        // ---- Frame cap (r_maxfps, app_run pattern): sleep out the remainder of
        // the frame budget so vsync-off doesn't churn the GPU on invisible
        // frames. Sleep most of the wait, spin the last ~1 ms for accuracy. ----
        {
            // LANE 6: named, not hidden. frameCpu is wall-clock endFrame-to-endFrame,
            // so a binding frame cap would inflate the CPU frame time and land in the
            // host residual. This row MUST read ~0 for a breakdown to be believed.
            X3_HOST_ZONE(Z_HostFrameCap);
            // MULTI-INSTANCE LANE (Bug 2): same sleep/spin as before, moved into
            // x3::game::paceFrame() and given ONE new behaviour — an UNFOCUSED
            // window caps at `r_bgfps` instead of `r_maxfps`, so a background
            // instance releases the GPU to the window the owner is playing in.
            x3::game::paceFrame(*console, window, frameCapPrev);
        }

        // FPS: log once per second.
        fpsAccum += dt; ++fpsFrames;
        if (fpsAccum >= 1.0) {
            const double fps = fpsFrames / fpsAccum;
            hudFps = fps;
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

    if (shopBuilt && phys) shop.shutdown(walkScene, *device, *phys);   // shop bodies out first
    // WD2 STACK: persist progression (the app_run additive-text format).
    {
        FILE* wf = std::fopen(kRpgSavePath, "wb");
        if (wf) {
            std::fputs("x3rpg 1\n", wf);
            std::fputs(progression.serialize().c_str(), wf);
            std::fputs(skillTree.serializeOwned().c_str(), wf);
            std::fclose(wf);
        }
    }
    if (worldCars.built() && phys) worldCars.shutdown(*phys);   // bodies out before physics dies
    g_hud = nullptr;   // charCB must never touch the dying Hud
    if (audioOn) { for (auto l : audioLoops) eaudio->stopLoop(l); eaudio->shutdown(); }
    device->shutdown();
    if (window) glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

}} // namespace x3::apphost
