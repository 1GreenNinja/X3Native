// X3Engine host — opens a window, brings up the render device + physics, builds
// the S2 graybox test level, and runs the loop with a fly camera. Walking is S3.

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#include "engine/core/x3_log.h"
#include "engine/core/x3_boot.h"   // [boot] timeline (boot-to-interactive instrumentation + --test-boottime)
#include "engine/core/IConsole.h"
#include "engine/core/IJobSystem.h"
#include "engine/rhi/IRenderDevice.h"
#include "engine/rhi/FrustumCull.h"          // CPU per-object frustum cull (--test-frustumcull)
#include "engine/rhi/GpuCull.h"           // D15 GPU culling — meshlet builder self-test (--test-meshlet)
#include <glm/gtc/matrix_transform.hpp>       // glm::perspective/lookAt/translate/scale (--test-frustumcull)
#include "engine/asset/IAssetSource.h"
#include "engine/physics/IPhysicsWorld.h"
#include "engine/physics/Destruction.h"   // K-T0/T1 destructibles + --test-destruction
#include "engine/physics/StructuralCollapse.h" // K-T3 support-graph collapse + --test-collapse
#include "engine/physics/Ragdoll.h"       // Physics §2 ragdoll+blend: --test-ragdoll + --world ragdoll
#include "engine/physics/IVehicle.h"      // vehicle framework: --test-vehicle + --world drive/boat/fly
#include "engine/asset/IModelLoader.h"
#include "engine/audio/IAudioSystem.h"
#include "engine/audio/RtAcoustics.h"   // RT ACOUSTICS: audio rays through the render TLAS (+ --test-acoustics)
#include "engine/net/INetworkSystem.h"   // netcode Phase 0: --test-net + SimClock
#include "engine/net/SimClock.h"         // deterministic fixed-step accumulator
#include "engine/net/ISnapshotInterpolator.h"  // netcode Phase 0c: --test-netinterp
#include "engine/net/IClientPredictor.h"        // netcode Phase 1: --test-netpredict
#include "engine/ai/INavigation.h"       // GENERAL navigation: nav grid + A* + --test-nav
#include "engine/script/IScriptSystem.h" // D14 Lua scripting: pak-shipped behavior + --test-script
#include "engine/llm/ILlmSystem.h"       // in-engine LLM (living NPC minds) + --test-llm

#include "scene.h"
#include "mesh_prims.h"
#include "asset_root.h"                    // portable assetRoot() (assets-LFS)
#include "asset_manifest_check.h"          // fleet asset-store manifest boot check (Phase A)
#include "audio_root.h"                    // portable resolveAudio() (D: mirror / G: packs)
#include "anim.h"                          // Skinner + --list-clips clip check
#include "thirdperson.h"                    // FP/3P toggle + Jake avatar + held weapon (--test-thirdperson)
#include "level1.h"
#include "level_loader.h"                   // data-driven canonical level loader + per-room PVS cull (--test-canonlevel)
#include "leveldoc_world.h"                  // EDITOR LevelDoc loader: --world fromdoc + hot reload (--test-loader)
#include "player.h"
#include "monster.h"
#include "level1_game.h"
#include "canon_play.h"                     // --world canonlevel gameplay (sidearm + animated enemies + Martinez + girls)
#include "intro_coldopen.h"                  // --world intro / default lead-in cold-open (shot-down -> captured)
#include "cutscene.h"                        // x3.cutscene/1 data-driven cutscene system (the COLD OPEN film)
#include "npc_dialog.h"                     // rescued-NPC talk/dialog -> companion (the captive girl)
#include "chat_tree.h"                      // x3.chattree/1 data-driven dialog runner (--test-chattree)
#include "mission.h"                        // x3.mission/1 data-driven mission runner (--test-mission, g_missiondoc)
#include "physprops.h"                      // FEATURE_GOALS §1: hanging cubes / joints (ragdoll foundation)
#include "ragdoll.h"                        // FEATURE_GOALS §2: physics death ragdoll
#include "editor/editor.h"                  // native Level Editor E1 (brain + self-test)
#include "editor/editor_host.h"             // Level Architect editor host (shell + blockout)
#include "barrels.h"                        // explosive barrels (shoot -> chain explosion)
#include "glass_test.h"                      // translucent-glass material (--test-glass)
#include "holo_terminal.h"                  // Jake's cell holographic terminal (text + input)
#include "secret_room.h"                    // code-locked trapdoor -> stocked secret room
#include "headless_device.h"                // shared no-op IRenderDevice (--test-hatch)
#include "engine/ecs/Ecs.h"                 // sparse-set ECS core (10k+ entities)
#include "ecs_render.h"                     // ECS -> GPU-driven render feed
#include "spire_mid.h"                      // EFLZ Spire F3/F4/F5 mid-floor content
#include "spire_top.h"                      // EFLZ Spire F6/F7 top-floor content (Act-1 finale)
#include "spire_nexus.h"                    // EFLZ Floor 4.5 Nexus Chamber / The Chorus (off-elevator boss)
#include "spire_sublevels.h"                // EFLZ hidden Floor-7 sub-levels + Dr. Chen Return Mission
#include "timeline.h"                        // EFLZ morality/timeline backbone for the 12 endings (--test-timeline)
#include "act2_world.h"                      // EFLZ Act-2 open-world surface host + L8/L9 (--test-act2)
#include "act2_desert.h"                     // EFLZ Act-2 desert depths + Salvari camp L10/L11 (--test-act2desert)
#include "act2_caves.h"                      // EFLZ Act-2 mid biomes L12-15 (--test-act2caves)
#include "tod.h"                             // EFLZ Time-of-Day cycle (sky/sun via SkyParams — --test-tod)
#include "weather.h"                         // EFLZ Weather (7 states, biome-gated, hazard — --test-weather)
#include "world_regions.h"                   // EFLZ open-world surrounding regions + 4 mountain ranges (--test-worldregions)
#include "world_stream.h"                    // SEAMLESS region-graph streaming (--world streamed / --test-worldstream)
#include "world_map.h"                       // INTERACTIVE WORLD MAP (M key / --test-worldmap)
#include "city.h"                            // EFLZ open-world metropolis: districts + roads + freeway tunnels (--test-city)
#include "ocean_base.h"                      // EFLZ open-world ocean + undersea base + submarine combat (--test-oceanbase)
#include "elevator.h"
#include "club1127.h"
#include "env_art.h"                       // EnvArtSystem::buildFromGlb (--screenshot-showroom)
#include "valley.h"                          // Crystal Valleys (Act 2, L15 — --world valley)
#include "cliffs.h"                          // Salvari cliffs finale (--world cliffs)
#include "terrain.h"
#include "fx.h"
#include "hud.h"
#include "ui.h"                              // GENERAL game-UI: menus + production HUD + --test-ui
#include "loading_screen.h"                  // EFLZ boot/world-load screen + --test-loading (Task #49)
#include "save.h"                            // GENERAL versioned checkpoint save/load + --test-saveload
#include "dialog.h"                          // AI-dialog + TTS voice on skinned NPCs (§3) + --test-dialog
#include "stress.h"
#include "destruct_demo.h"                 // K-T1 destruction demo (--world destruct)
#include "ragdoll_demo.h"                  // Physics §2 ragdoll demo (--world ragdoll) + blend check
#include "vehicle.h"                       // vehicle demo worlds (--world drive/boat/fly)
#include "vehparts.h"                      // performance-parts catalog + build composition (--test-vehparts)
#include "perfshop.h"                      // the drive-in performance shop (--world drive)
#include "ecology.h"                       // AMBIENT ECOLOGY: grazers/predators/patrols (--test-ecology)
#include "crowd.h"                         // CROWDS: club dancers + facility civilians (--test-crowd)
#include "alert.h"                         // FACILITY ALERT LEVEL: the wanted system (--test-alert)
#include "host_context.h"                  // #28 deep split: shared live-state struct for the --world hosts
#include "world_hosts.h"                   // #28 deep split: dispatchWorldHost() + the extracted host TUs
#include "input_globals.h"                 // #28 deep split: g_weaponScroll + scrollCallback (shared w/ streamed host)

#include <memory>
#include <string_view>
#include <string>
#include <cmath>
#include <vector>
#include <unordered_map>   // per-weapon fire-sound cache (name -> SoundHandle)
#include <cstdint>
#include <cstdlib>
#include <cstring>    // std::strcmp (showroom planet name match)
#include <algorithm>
#include <filesystem>
#include <cstdio>
#include <thread>     // r_maxfps frame limiter
#include <future>     // boot-time async audio bring-up (docs/BOOT_TIME.md)
#include <chrono>
#include <fstream>    // window-size settings persistence (SET AS DEFAULT)

// Public-domain single-header GIF encoder (Charlie Tangora) — vendored under
// third_party/gif_h. Used ONLY by the headless --capture-ai tool below to assemble
// the captured PNG frame sequence into an animated GIF. This is the SOLE
// translation unit that includes gif.h, so its (non-inline) functions link cleanly.
// It's vendored third-party code, so quiet its /W4 noise around the include only.
#if defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning(disable : 4334)   // 32-bit shift result implicitly widened to 64
#endif
#include "../third_party/gif_h/gif.h"
#if defined(_MSC_VER)
#  pragma warning(pop)
#endif
// stb_image (to read the captured PNGs back for GIF assembly). The engine already
// hosts a STB_IMAGE_IMPLEMENTATION inside ModelLoader.cpp with FILE-LOCAL linkage,
// so we cannot link to its stbi_load. Instead we instantiate our OWN file-local
// copy here via STB_IMAGE_STATIC (no symbol clash, used only by this tool path).
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#if defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning(disable : 4244 4456 4457)   // vendored stb_image /W4 noise
#endif
#include <stb_image.h>
#if defined(_MSC_VER)
#  pragma warning(pop)
#endif

namespace {
// Approximate the viewmodel muzzle in world space from the eye + look angles, so
// the FX tracer starts near the gun barrel (lower-right of the view) rather than
// dead center. Mirrors the camera-basis offsets used by WeaponSystem; tuned to
// sit just in front of and below/right of the eye.
x3::phys::Vec3 muzzleFromCamera(float ex, float ey, float ez, float yaw, float pitch,
                                float mFwd = 1.3f, float mRight = 0.26f, float mDown = 0.30f) {
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const float cy = std::cos(yaw),   sy = std::sin(yaw);
    const x3::phys::Vec3 forward{ cp * cy, sp, cp * sy };
    const x3::phys::Vec3 right{ -sy, 0.0f, cy };
    const x3::phys::Vec3 up{
        right.y * forward.z - right.z * forward.y,
        right.z * forward.x - right.x * forward.z,
        right.x * forward.y - right.y * forward.x };
    // Muzzle = barrel tip of the held viewmodel: forward + clearly down/right of the
    // eye so the tracer/flash visibly LEAVE the gun (a near-on-axis origin sits
    // end-on and the beam vanishes). Caller may override via the params.
    return x3::phys::Vec3{
        ex + forward.x * mFwd + right.x * mRight - up.x * mDown,
        ey + forward.y * mFwd + right.y * mRight - up.y * mDown,
        ez + forward.z * mFwd + right.z * mRight - up.z * mDown };
}

// COLD-OPEN CINEMATIC + NIGHT-SKY PLANETS — moved VERBATIM to app/cinematic.
// {h,cpp} (#28 monolith split). using-declarations keep every call site in the
// world hosts below unqualified (byte-identical bodies).
} // close the anon namespace briefly to include the cinematic header
#include "cinematic.h"
#include "showroom_tod.h"   // SHOWROOM DAY/NIGHT helpers (shared with the --world showroom host)
namespace {
using x3::apphost::applyShowroomTimeOfDay;   // moved to showroom_tod.h (#28 split)
using x3::apphost::showroomDayDefault;       // moved to showroom_tod.h (#28 split)
using x3::apphost::NightSkyPlanet;
using x3::apphost::kNightSkyDist;
using x3::apphost::loadNightSkyPlanets;
using x3::apphost::drawNightSkyPlanets;
using x3::apphost::CinActorState;
using x3::apphost::CinematicScene;
using x3::apphost::CinAudioMap;
using x3::apphost::runCutsceneWindowed;

// ---- SHOWROOM DAY<->NIGHT lighting STATES (one helper, two looks) -----------
// The --world showroom (and its headless proofs) drive their sky/sun/ambient/
// bloom/interior-point-lights through this ONE helper so DAY and NIGHT are a
// single switch.  ADDITIVE: NIGHT reproduces the exact values the showroom has
// always used (dark planet sky + dim cool moon + full interior point lights);
// the planet draw + setSkyTime wheeling are gated to NIGHT by the CALLER.
//
//   DAY  — match the Unity ShowRoom_Vol30 interior (bright, cool, high-key white):
//     * Sun from the Unity HDRP directional light Rotation X=69.31, Y=9.7, Z=0.
//       Unity light forward = R_y(9.7)*R_x(69.31)*(0,0,1) = (0.0595,-0.9355,0.3483);
//       sunDir (TOWARD the sun) = -forward = (-0.0595, 0.9355, -0.3483) — a high
//       winter-midday sun, ~69 deg elevation, azimuth ~10 deg.  Short soft shadows.
//     * Bright sky: pale winter-blue zenith, warm-grey horizon haze, exposure ~1.
//     * setAmbient BRIGHT cool snow-bounce — the DOMINANT fill (high-key, no hard
//       blacks); this is what makes the interior read bright/cool like Unity 013904.
//     * setBloom low (~0.12).  Interior point lights DIMMED (x0.3) — snow-bounce
//       carries the room by day, not the fixtures.
//
// `interiorLights` (nullable) holds the FULL-intensity NIGHT point lights (color
// already pre-multiplied by intensity).  DAY pushes a x0.3-scaled copy; NIGHT
// pushes them unchanged.  Pass nullptr for paths with no interior lights (the
// exterior --screenshot-showroom).  Returns nothing; SSAO/GI are untouched.
// (applyShowroomTimeOfDay + showroomDayDefault moved VERBATIM to showroom_tod.h
//  for the #28 split — shared with app/world_hosts/host_showroom.cpp; main()'s
//  call sites stay unqualified via the using-declarations above.)

// ---- ON-GLASS HOLO-TERMINAL readout (large, high-contrast, fit to the panel) ----
// Project the cell HoloTerminal panel center + top edge, then lay out the boot
// readout (and, while typing, the input line) as LARGE proportional-font text sized
// to fit WITHIN the glass: the body size auto-shrinks so the widest line spans ~92%
// of the projected panel width, so it never overflows the bezel yet stays as big as
// possible. Bright cyan-white over the glowing blue glass with a dark drop shadow.
// Purely additive 2D HUD draws; safe to call from both the interactive loop and the
// --screenshot capture. `anchor` is the panel center; `showInput` adds the prompt.
void drawHoloReadout(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                     const x3::game::HoloTerminal& term, const x3::phys::Vec3& anchor,
                     bool showInput) {
    const float panelHalfH = 0.45f;   // half of the 0.9 m panel height
    const float panelHalfW = 0.70f;   // half of the 1.4 m panel width
    float sx = 0.0f, sy = 0.0f, sxT = 0.0f, syT = 0.0f;
    if (!device.worldToScreen(anchor.x, anchor.y, anchor.z, sx, sy) ||
        !device.worldToScreen(anchor.x, anchor.y + panelHalfH, anchor.z, sxT, syT))
        return;
    float halfPxH = std::fabs(syT - sy);
    if (halfPxH < 22.0f)  halfPxH = 22.0f;
    if (halfPxH > 360.0f) halfPxH = 360.0f;
    const float halfPxW = halfPxH * (panelHalfW / panelHalfH);   // glass half-width in px
    const float innerW  = halfPxW * 2.0f * 0.90f;                // usable width inside the bezel

    // The procedural hologram texture now draws a full SECURITY-CONSOLE line-art HUD
    // (header rule + emblem, bracket frame, center schematic, warning triangles, data
    // bars, dotted strip). The on-glass TEXT composites WITH it, not over it:
    //   * line 0 is the HEADER TITLE — drawn wide + bright across the top header strip,
    //   * the remaining readout lines are the LEFT-column "live data text" — drawn
    //     SMALLER and clipped to the left ~56% so the center schematic + right column
    //     line-art stay readable (matching the reference composition).
    const float sh[4] = { 0.0f, 0.0f, 0.0f, 0.88f };
    const float leftPx = sx - halfPxW * 0.88f;                   // left margin inside the bezel

    const auto& L = term.lines();
    if (L.empty()) return;

    // ---- HEADER TITLE (line 0): sized to span most of the header strip width. ----
    {
        const float titleBudget = innerW * 0.96f;
        float titlePx = halfPxH * 0.30f;                         // start tall
        const float tw = device.textAdvance(x3::rhi::FontRole::Menu, L[0].c_str(), titlePx);
        if (tw > titleBudget && tw > 1.0f) titlePx *= titleBudget / tw;
        if (titlePx < 9.0f) titlePx = 9.0f;
        const float ty = sy - halfPxH * 0.82f;                   // up on the header strip
        const float col[4] = { 0.82f, 0.99f, 1.0f, 1.0f };       // bright cyan-white title
        const float off = std::max(1.5f, titlePx * 0.07f);
        device.drawHudTextF(frame, x3::rhi::FontRole::Menu, L[0].c_str(), leftPx + off, ty + off, titlePx, sh);
        device.drawHudTextF(frame, x3::rhi::FontRole::Menu, L[0].c_str(), leftPx, ty, titlePx, col);
    }

    // ---- BODY readout (lines 1+) as the left-column data text. Constrain width to
    // the left zone so it doesn't cross the center schematic. ----
    const float bodyZoneW = innerW * 0.56f;                      // left data-column width
    const float lineH0 = (halfPxH * 2.0f) / 13.0f;
    float bodyPx = lineH0 * 0.86f;
    for (size_t li = 1; li < L.size(); ++li) {
        const float w = device.textAdvance(x3::rhi::FontRole::Menu, L[li].c_str(), bodyPx);
        if (w > bodyZoneW && w > 1.0f) bodyPx *= bodyZoneW / w;  // shrink to the left zone
    }
    if (bodyPx < 8.0f) bodyPx = 8.0f;
    const float lineH = bodyPx * 1.22f;
    float ty = sy - halfPxH * 0.46f;                             // below the header, down the left column
    for (size_t li = 1; li < L.size(); ++li) {
        const float col[4] = { 0.80f, 0.97f, 1.0f, 1.0f };       // cyan-white data text
        const float off = std::max(1.2f, bodyPx * 0.07f);
        device.drawHudTextF(frame, x3::rhi::FontRole::Menu, L[li].c_str(), leftPx + off, ty + off, bodyPx, sh);
        device.drawHudTextF(frame, x3::rhi::FontRole::Menu, L[li].c_str(), leftPx, ty, bodyPx, col);
        ty += lineH;
    }
    if (showInput) {
        const std::string inLine = std::string("> ") + term.input() +
                                   (term.cursorOn() ? "_" : " ");
        const float ic[4] = { 1.0f, 0.92f, 0.32f, 1.0f };        // bright amber prompt
        const float ipx = bodyPx * 1.18f;
        const float ioff = std::max(1.2f, ipx * 0.07f);
        device.drawHudTextF(frame, x3::rhi::FontRole::Menu, inLine.c_str(), leftPx + ioff, ty + ioff, ipx, sh);
        device.drawHudTextF(frame, x3::rhi::FontRole::Menu, inLine.c_str(), leftPx, ty, ipx, ic);
    }
}

// ---- Settings persistence in %LOCALAPPDATA%\x3native_settings.cfg ----
// readWindowSize() overrides winW/winH at startup (returns true if a saved size exists,
// so the host skips maximize-by-default); readAudioSettings() seeds the music/SFX
// state; writeSettings() (the menu "SET AS DEFAULT" action) persists window size +
// audio together. Plain key=value text; a missing/garbled file/key is simply ignored.
static std::string x3SettingsPath() {
    const char* base = std::getenv("LOCALAPPDATA");
    return std::string(base && *base ? base : ".") + "\\x3native_settings.cfg";
}
static bool readWindowSize(uint32_t& w, uint32_t& h) {
    std::ifstream f(x3SettingsPath());
    if (!f) return false;
    bool found = false; std::string line;
    while (std::getline(f, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq);
        const uint32_t v = (uint32_t)std::strtoul(line.c_str() + eq + 1, nullptr, 10);
        if (k == "width"  && v >= 320) { w = v; found = true; }
        else if (k == "height" && v >= 240) { h = v; found = true; }
    }
    return found;
}
// Audio settings live in the same key=value cfg. Each is optional; defaults are
// kept when a key is missing/garbled. musicVol/sfxVol are stored as plain floats.
static void readAudioSettings(bool& musicOn, float& musicVol, float& sfxVol) {
    std::ifstream f(x3SettingsPath());
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq);
        const char* vs = line.c_str() + eq + 1;
        if      (k == "musicOn")  musicOn  = (std::strtol(vs, nullptr, 10) != 0);
        else if (k == "musicVol") musicVol = (float)std::strtod(vs, nullptr);
        else if (k == "sfxVol")   sfxVol   = (float)std::strtod(vs, nullptr);
    }
    auto clamp01 = [](float& v) { if (v < 0.0f) v = 0.0f; if (v > 1.0f) v = 1.0f; };
    clamp01(musicVol); clamp01(sfxVol);
}
// Write ALL persisted settings (window size + audio) in one shot, so SET DEFAULT
// captures the live audio sliders too. Reads the current audio model from the host.
static void writeSettings(uint32_t w, uint32_t h, bool musicOn, float musicVol, float sfxVol) {
    std::ofstream f(x3SettingsPath());
    if (f) f << "width=" << w << "\nheight=" << h << "\n"
             << "musicOn=" << (musicOn ? 1 : 0) << "\n"
             << "musicVol=" << musicVol << "\nsfxVol=" << sfxVol << "\n";
}

// Bundle passed to GLFW via the window user-pointer so the char/key callbacks
// can route text input into the on-screen console.
struct InputContext {
    x3::game::Hud*       hud = nullptr;
    x3::con::IConsole*   console = nullptr;
};

// ---- Live-tunable viewmodel pose (FIX 1) ----------------------------------
// The held-gun pose is exposed as console cvars so the player can dial the
// barrel onto the crosshair in-game, then bake the values as defaults. Angles
// are entered in DEGREES in the console (intuitive) and converted to radians
// before being handed to WeaponSystem::drawViewmodel(); placement is in meters.
struct VmPose { float yawRad, pitchRad, rollRad, fwd, right, down; };

constexpr float kDegToRad = 3.14159265358979f / 180.0f;

// KEYPAD code for the --world showroom HIDDEN HATCH (the concealed floor panel that
// opens the stair down to the glass elevator). The hatch is gated by entering THIS
// code on a keypad (reusing x3::game::KeypadEntry, the same state machine driven by
// --test-doorcode / the Level-1 §6.4 door gate). Deliberately NOT 1127 (that is the
// Spire/Club secret). Themed "ARIA" on a phone keypad (A=2,R=7,I=4,A=2). *** CHANGE
// HERE to re-key the hatch. *** Also exercised headless by --test-hatchcode.
// (kShowroomHatchCode moved with the --world showroom host to
//  app/world_hosts/host_showroom.cpp — #28 split; main() no longer uses it.)

// Register the six viewmodel cvars, seeded with the baked defaults (weapon.h).
void registerViewmodelCVars(x3::con::IConsole& console) {
    console.registerCVar("vm_yaw",   std::to_string(x3::game::kVmDefYawDeg),
                         "viewmodel yaw offset about camera up (degrees)");
    console.registerCVar("vm_pitch", std::to_string(x3::game::kVmDefPitchDeg),
                         "viewmodel pitch offset about camera right (degrees)");
    console.registerCVar("vm_roll",  std::to_string(x3::game::kVmDefRollDeg),
                         "viewmodel roll offset about camera forward (degrees)");
    console.registerCVar("vm_fwd",   std::to_string(x3::game::kVmDefFwd),
                         "viewmodel offset forward along the look dir (meters)");
    console.registerCVar("vm_right", std::to_string(x3::game::kVmDefRight),
                         "viewmodel offset to the right (meters)");
    console.registerCVar("vm_down",  std::to_string(x3::game::kVmDefDown),
                         "viewmodel offset below the eye line (meters)");
    // Frame cap (FPS limiter). Only bites with vsync OFF (FIFO already paces to the
    // refresh); 0 = uncapped. Stops vsync-off from needlessly maxing the GPU on
    // frames the display never shows. Live-tunable: `r_maxfps 0` for uncapped.
    console.registerCVar("r_maxfps", "240", "frame cap when vsync off (0 = uncapped)");
    // Hardware ray-traced ambient occlusion (RT AO — Vulkan ray-query path). Gated
    // + DEFAULT OFF: only takes effect on a device that supports ray tracing. Live-
    // tunable: `r_rtao 1` turns on ground-truth ray-traced contact occlusion (BLAS/
    // TLAS + inline rayQueryEXT), `r_rtao 0` returns to the SSAO/raster look exactly.
    console.registerCVar("r_rtao",          "0",    "hardware RT ambient occlusion (ray query); 0 = off (raster/SSAO)");
    console.registerCVar("r_rtao_radius",   "0.5",  "RT AO ray length (meters)");
    console.registerCVar("r_rtao_rays",     "8",    "RT AO hemisphere rays per pixel (1..32)");
    console.registerCVar("r_rtao_strength", "0.85", "RT AO applied darkening (1 = full, 0 = off)");
    // Per-room PVS occlusion cull (canonlevel). 1 = cull on (draw only the player's
    // current room + its doored neighbours); 0 = draw the whole level (e.g. for noclip
    // overview / debugging). Live-tunable from the console.
    console.registerCVar("r_roomcull", "1", "per-room PVS occlusion cull (0 = draw whole level, e.g. for noclip)");
    // CPU per-object frustum cull (conservative world-sphere vs camera frustum, in the
    // render device). 1 = on (default); 0 = byte-identical to no cull (objectsDrawn ==
    // every submitted instance). The reference baseline the D15 GPU cull is diffed against.
    console.registerCVar("r_frustumcull", "1", "CPU per-object frustum cull (0 = draw every instance, no cull)");
    // D15 GPU-driven culling. -1 = auto (best supported tier), 0 = CPU cull exactly as
    // today (default — byte-identical), 1 = Tier 0 (cull compute on the graphics queue),
    // 2 = Tier 1 (async compute queue), 3 = Tier 2 (mesh-shader meshlets, opt-in).
    // Unsupported requests clamp down. The GPU predicate is bit-equivalent to the CPU
    // r_frustumcull test (the D15 acceptance gate: statDrawn == objectsDrawn).
    console.registerCVar("r_cullpath", "0", "GPU cull path: -1 auto, 0 CPU, 1 tier0 gfx-queue, 2 tier1 async, 3 tier2 meshlets");
    // HZB occlusion phase on top of the GPU frustum cull (needs r_cullpath >= 1).
    console.registerCVar("r_hzb", "0", "HZB occlusion cull on the GPU path (0 = frustum only)");
    // x3.mission/1: when 1, missions/level1.mission.json DRIVES the HUD objective
    // line (the ObjectiveSystem free-text lane) instead of the hardcoded Level-1
    // beat list. Default 0 = zero behavior change (the doc is not even loaded).
    console.registerCVar("g_missiondoc", "0", "drive Level-1 objectives from missions/level1.mission.json (x3.mission/1)");
    // Portal flood-fill depth: how many OPEN-doorway hops the canonlevel cull floods out
    // from the player's room. Higher = see further down a hall through open doors (more
    // rooms drawn); 1 = current room + direct neighbours only (tight). The flood is also
    // gated by the camera frustum (only rooms you LOOK at are drawn) + a room budget, so it
    // never falls back to the whole tower. Live-tunable.
    console.registerCVar("r_culldepth", "6", "canonlevel portal flood-fill depth (open-doorway hops; 1 = direct neighbours only)");
    // Whole-scene brightness dial (live). Multiplies the composite pre-tonemap exposure;
    // 1.0 = unchanged. The in-game "showroom brightness" knob: `r_exposure 1.5` brightens,
    // `r_exposure 0.7` dims. Type it in the console (~) and the scene updates immediately.
    console.registerCVar("r_exposure", "1.0", "whole-scene brightness (pre-tonemap exposure multiplier; live)");
    // Metal ambient-specular floor (mesh.frag IBL path): metals in a DARK baked
    // environment keep an F0-tinted ambient response instead of rendering black.
    // 1 = on (default), 0 = off, >1 strengthens. Live (synced in applyRtaoCVars).
    console.registerCVar("r_metalambient", "1", "metal ambient-specular floor (0 = off; metals keep an F0-tinted ambient in dark environments; live)");
    // ---- HDR POST STACK (tonemap / bloom / auto-exposure) — all live ----------
    // Defaults preserve the shipped look: ACES tonemap, scene-tuned bloom, gentle
    // auto-exposure. A/B the legacy pre-AE renderer with: r_autoexposure 0 (the
    // legacy path: ACES + scene bloom + manual r_exposure, exactly as before this
    // strike). r_tonemap 0 is a raw passthrough clamp for tonemap debugging.
    console.registerCVar("r_tonemap",        "1",    "tonemap operator: 1 = ACES filmic (default), 0 = passthrough clamp (debug A/B)");
    console.registerCVar("r_bloom",          "1",    "bloom on/off (0 skips the whole downsample/upsample chain)");
    console.registerCVar("r_bloomintensity", "-1",   "bloom strength override; <0 = keep the scene-tuned value (default)");
    console.registerCVar("r_bloomthreshold", "1.10", "bloom bright-pass threshold (linear luminance; soft knee)");
    console.registerCVar("r_autoexposure",   "1",    "auto-exposure (eye adaptation): scene log-luminance drives exposure; r_exposure becomes a bias");
    console.registerCVar("r_aespeed",        "1.5",  "auto-exposure adaptation speed (1/s; higher = faster eye)");
    console.registerCVar("r_aemin",          "0.7",  "auto-exposure clamp floor (max darkening of bright scenes)");
    console.registerCVar("r_aemax",          "2.2",  "auto-exposure clamp ceiling (max lift of dark scenes)");
    console.registerCVar("r_aekey",          "0.18", "auto-exposure target middle-grey key");
    // TAA (temporal anti-aliasing): Halton(2,3) sub-pixel jitter + history resolve
    // before bloom/tonemap. r_taa 0 turns the jitter fully off and skips the
    // resolve pass -> byte-identical to the pre-TAA render path (A/B).
    console.registerCVar("r_taa",        "1",    "temporal AA: 1 = jitter + history resolve (default), 0 = off (byte-identical pre-TAA path)");
    console.registerCVar("r_taasharpen", "0.25", "post-TAA RCAS-style sharpen amount (0 = off; only applied while r_taa 1)");
    // Metal ambient-specular floor (mesh.frag IBL path): metals in a DARK baked
    // environment keep an F0-tinted ambient response instead of rendering black.
    // 1 = on (default), 0 = off, >1 strengthens. Live (synced in applyRtaoCVars).
    console.registerCVar("r_metalambient", "1", "metal ambient-specular floor (0 = off; metals keep an F0-tinted ambient in dark environments; live)");
    // SSR / RT REFLECTIONS (STRIKE 3): a half-res compute pass marches each pixel's
    // reflection ray against the depth buffer and samples LAST frame's lit scene
    // (the TAA history image — reflections REQUIRE r_taa 1; with TAA off the whole
    // chain is off and the render is byte-identical to the pre-reflections build).
    // On ray-query hardware, screen-space misses fall back to ONE inline ray query
    // into the scene TLAS (r_rtreflections; non-RT devices are SSR-only
    // automatically). mesh.frag blends the result INTO its split-sum IBL specular
    // by confidence + roughness (mirror-sharp below rough 0.25, faded out by 0.6
    // where the prefiltered env takes over). All live (synced in applyRtaoCVars).
    console.registerCVar("r_ssr",           "1", "screen-space reflections (needs r_taa 1); 0 = off (IBL-only specular, byte-identical)");
    console.registerCVar("r_rtreflections", "1", "ray-query reflection fallback where SSR misses (RT hardware only; SSR-only otherwise)");
    console.registerCVar("r_reflquality",   "0", "reflection buffer resolution: 0 = half-res (default), 1 = full-res");
    console.registerCVar("r_reflintensity", "1", "reflection blend weight scale [0..1] on the IBL-specular replace");
    // DDGI — dynamic diffuse global illumination (probe-grid ray-query GI). The
    // probe field replaces the ambient DIFFUSE term (flat ambient / IBL irradiance)
    // with traced bounce light; specular stays IBL/reflections. Requires ray-query
    // + position-fetch hardware (RTX class); everything else silently ignores it.
    // Probes converge over ~1-2 s; emissive panels + sun changes propagate. Live.
    console.registerCVar("r_ddgi",           "0",    "DDGI probe-grid GI (ray query + position fetch); 0 = off (flat/IBL ambient, byte-identical)");
    console.registerCVar("r_ddgi_debug",     "0",    "DDGI debug view: 0 = off, 1 = irradiance field, 2 = grid confidence");
    console.registerCVar("r_ddgi_rays",      "96",   "DDGI rays per probe per frame (16..128)");
    console.registerCVar("r_ddgi_intensity", "1.0",  "DDGI applied GI scale on the replaced ambient diffuse");
    console.registerCVar("r_ddgi_nx",        "24",   "DDGI probe count X (2..32; grid auto-fits the level volume)");
    console.registerCVar("r_ddgi_ny",        "8",    "DDGI probe count Y (2..32)");
    console.registerCVar("r_ddgi_nz",        "24",   "DDGI probe count Z (2..32)");
    console.registerCVar("r_ddgi_hyst",      "0.97", "DDGI irradiance hysteresis (history blend; higher = smoother/slower)");
    // RAY-TRACED SOFT SHADOWS (r_rtshadows): per-pixel inline ray-query shadow
    // rays in the mesh fragment stage, against the same TLAS RT-AO/DDGI use.
    // Tier 0 = CSM-only (bit-identical pre-RT path), 1 = sun RT (cone-jittered,
    // contact-hardening penumbra, min()-combined with CSM so skinned characters
    // keep their raster shadows), 2 = sun + POINT LIGHTS (lamps finally cast;
    // the first K contributing lights per pixel get a source-jittered shadow
    // ray each). DEFAULT 2 on ray-query hardware; auto-0 anywhere else. Live.
    console.registerCVar("r_rtshadows",    "2",    "RT soft shadows: 0 = CSM-only (bit-identical), 1 = sun RT, 2 = sun + point lights (default; auto-0 without ray query)");
    console.registerCVar("r_rtsun_size",   "0.5",  "RT sun angular radius (degrees) — penumbra width; 0 = hard traced shadow");
    console.registerCVar("r_rtpoint_max",  "4",    "RT point-light shadow rays per pixel (first K contributing lights; others stay unshadowed)");
    console.registerCVar("r_rtpoint_size", "0.10", "RT point-light source radius (meters) — penumbra widens with it + occluder distance");
    // RT ACOUSTICS — audio rays through the render TLAS (the audio sibling of
    // r_rtao/r_ddgi): per-emitter occlusion rays muffle gunfire through real
    // walls (volume duck + per-voice lowpass) and a periodic listener room
    // probe sizes the reverb to the ACTUAL geometry. Default ON; the chain
    // self-gates on ray-query hardware (non-RT devices: inert, byte-identical
    // audio). snd_rta_debug logs the live per-emitter occlusion + room class.
    console.registerCVar("snd_rtacoustics", "1", "RT acoustics: TLAS occlusion rays (muffle through walls) + room-probe reverb; needs RT hardware, inert otherwise");
    console.registerCVar("snd_rta_debug",   "0", "RT acoustics debug: 1 = log per-emitter occlusion + room class/mfp/T60/wet (~2 Hz)");
    // (3rd-person Jake tuning cvars removed 2026-05-27: dialed-in values
    // jake_yoff=1.03 / jake_yawoff_deg=90 / jake_camdist=2.3 / jake_camh=0.37
    // are now baked as member defaults in app/thirdperson.h.)

    // ---- HELD-WEAPON GRIP LIVE-TUNE (TASK#53) ------------------------------
    // ADDITIVE override on the CURRENTLY-held weapon's kTpGripTable row, so Tim can
    // DIAL each gun's grip live in 3P (F2/F5), read the effective values off the 3P
    // HUD, then BAKE them into kTpGripTable (app/thirdperson.h). Default 0 => the
    // baked table is unchanged. Position is meters in the hand-LOCAL frame; rotation
    // is degrees; scale is added to the row's scaleMul. See the BAKE block above
    // kTpGripTable. Synced per-frame in applyRtaoCVars().
    console.registerCVar("grip_x",     "0", "3P held-weapon grip override: +meters toward thumb (right); live, current weapon");
    console.registerCVar("grip_y",     "0", "3P held-weapon grip override: +meters into the palm (down); live, current weapon");
    console.registerCVar("grip_z",     "0", "3P held-weapon grip override: +meters down the barrel (forward); live, current weapon");
    console.registerCVar("grip_pitch", "0", "3P held-weapon grip override: +degrees tilt about hand-right; live, current weapon");
    console.registerCVar("grip_yaw",   "0", "3P held-weapon grip override: +degrees twist about hand-up; live, current weapon");
    console.registerCVar("grip_roll",  "0", "3P held-weapon grip override: +degrees roll about the barrel; live, current weapon");
    console.registerCVar("grip_scale", "0", "3P held-weapon grip override: +added to the weapon's scaleMul; live, current weapon");

    // ---- ZERO-STUTTER GUARANTEE (docs/ZERO_STUTTER.md) ---------------------
    // r_strictpso: any pipeline/shader-module/descriptor-pool created after the
    // first frame begins logs a validation-style "[stutter]" error (the UE-style
    // PSO-hitch detector). Default ON in Debug builds (with the validation gate),
    // OFF in Release (the counters still accumulate for --test-framepacing).
#ifdef NDEBUG
    console.registerCVar("r_strictpso", "0", "log [stutter] error on any pipeline/module/pool created after frame 1 (zero-stutter audit)");
#else
    console.registerCVar("r_strictpso", "1", "log [stutter] error on any pipeline/module/pool created after frame 1 (zero-stutter audit; Debug default ON)");
#endif
    // Frame-pacing telemetry HUD line (p50/p95/p99/p999/max + spike + late-create
    // counters) — the live receipts. The spike LOG in the device is always on.
    console.registerCVar("r_frametelemetry", "0", "HUD frame-pacing telemetry line: percentiles + spikes + late pipeline creations");
    // Spike/percentile thresholds (cvars so CI can tighten them later):
    console.registerCVar("r_fpace_warmup", "60",  "frame-pacing warmup frames excluded from percentiles/spike counting");
    console.registerCVar("r_fpace_spikex", "2.0", "spike threshold: frame > spikex * rolling median");
    console.registerCVar("r_fpace_floor",  "3.0", "spike absolute floor (ms): frame must also exceed median + floor (filters sub-ms OS jitter)");
    // BOOT-TO-GAMEPLAY budget (docs/BOOT_TIME.md): the --test-boottime gate fails
    // if process-start -> first interactive frame exceeds this. A cvar so weaker
    // machines can loosen the threshold without a rebuild.
    console.registerCVar("boot_budget_ms", "2000", "boot-to-interactive budget (ms) asserted by --test-boottime");
}

// Read the r_rtao* cvars and push them onto the device (no-op on a non-RT device).
void applyRtaoCVars(const x3::con::IConsole& console, x3::rhi::IRenderDevice& device) {
    x3::rhi::IRenderDevice::RtaoParams p{};
    p.enabled  = console.getInt("r_rtao") != 0;
    p.radius   = console.getFloat("r_rtao_radius");
    p.rays     = console.getInt("r_rtao_rays");
    p.strength = console.getFloat("r_rtao_strength");
    if (p.radius <= 0.0f) p.radius = 1.2f;
    device.setRtaoParams(p);
    // Whole-scene brightness dial (live; default 1.0 = unchanged). Piggybacks the
    // per-frame cvar->device sync so `r_exposure` takes effect immediately. With
    // auto-exposure on this is the exposure COMPENSATION bias.
    device.setExposure(console.getFloat("r_exposure"));
    // CPU per-object frustum cull (live; default on). Same per-frame sync so toggling
    // r_frustumcull from the console takes effect immediately.
    device.setFrustumCullEnabled(console.getInt("r_frustumcull") != 0);
    // D15 GPU cull path + HZB phase (live; default 0 = CPU path, byte-identical).
    device.setCullPath(console.getInt("r_cullpath"));
    device.setHzbEnabled(console.getInt("r_hzb") != 0);
    // Metal ambient-specular floor (live; default 1.0 = on, 0 = off).
    device.setMetalAmbient(console.getFloat("r_metalambient"));
    // HDR post stack: tonemap / bloom gate + tunables / auto-exposure (all live).
    x3::rhi::IRenderDevice::PostFXParams px{};
    px.tonemapMode    = console.getInt("r_tonemap");
    px.bloomEnabled   = console.getInt("r_bloom") != 0;
    px.bloomIntensity = console.getFloat("r_bloomintensity");
    px.bloomThreshold = console.getFloat("r_bloomthreshold");
    px.autoExposure   = console.getInt("r_autoexposure") != 0;
    px.aeSpeed        = console.getFloat("r_aespeed");
    px.aeMin          = console.getFloat("r_aemin");
    px.aeMax          = console.getFloat("r_aemax");
    px.aeKey          = console.getFloat("r_aekey");
    if (px.aeSpeed <= 0.0f) px.aeSpeed = 1.5f;
    if (px.aeMin   <= 0.0f) px.aeMin   = 0.7f;
    if (px.aeMax   <  px.aeMin) px.aeMax = px.aeMin;
    if (px.aeKey   <= 0.0f) px.aeKey   = 0.18f;
    // TAA (live): r_taa gates the jitter + resolve; r_taasharpen [0..1].
    px.taa        = console.getInt("r_taa") != 0;
    px.taaSharpen = console.getFloat("r_taasharpen");
    if (px.taaSharpen < 0.0f) px.taaSharpen = 0.0f;
    if (px.taaSharpen > 1.0f) px.taaSharpen = 1.0f;
    device.setPostFX(px);
    // Metal ambient-specular floor (live; default 1.0 = on, 0 = off).
    device.setMetalAmbient(console.getFloat("r_metalambient"));
    // SSR / RT reflections (live). The device additionally gates on TAA being
    // active (the TAA history is the pass's color source) and tier-gates the
    // ray-query fallback on RT hardware support (Pascal = SSR-only automatically).
    x3::rhi::IRenderDevice::ReflectionParams rf{};
    rf.ssr        = console.getInt("r_ssr") != 0;
    rf.rtFallback = console.getInt("r_rtreflections") != 0;
    rf.fullRes    = console.getInt("r_reflquality") != 0;
    rf.intensity  = console.getFloat("r_reflintensity");
    device.setReflectionParams(rf);
    // DDGI probe-grid GI (live). The device tier-gates on ray-query + position-
    // fetch hardware; on anything else this is a harmless no-op store.
    x3::rhi::IRenderDevice::DdgiParams dg{};
    dg.enabled      = console.getInt("r_ddgi") != 0;
    dg.debug        = console.getInt("r_ddgi_debug");
    dg.raysPerProbe = console.getInt("r_ddgi_rays");
    dg.intensity    = console.getFloat("r_ddgi_intensity");
    dg.countX       = console.getInt("r_ddgi_nx");
    dg.countY       = console.getInt("r_ddgi_ny");
    dg.countZ       = console.getInt("r_ddgi_nz");
    dg.hysteresis   = console.getFloat("r_ddgi_hyst");
    if (dg.raysPerProbe <= 0) dg.raysPerProbe = 96;
    if (dg.hysteresis  <= 0.0f) dg.hysteresis = 0.97f;
    device.setDdgiParams(dg);
    // RT soft shadows (live). The device tier-gates on ray-query hardware + the
    // mesh_rt pipeline variants; on anything else this is a harmless store and
    // the plain (bit-identical) mesh pipelines stay bound.
    x3::rhi::IRenderDevice::RtShadowParams rs{};
    rs.tier        = console.getInt("r_rtshadows");
    rs.sunSizeDeg  = console.getFloat("r_rtsun_size");
    rs.pointMax    = console.getInt("r_rtpoint_max");
    rs.pointRadius = console.getFloat("r_rtpoint_size");
    device.setRtShadowParams(rs);
    // ZERO-STUTTER frame-pacing thresholds + the strict-PSO audit gate (live).
    x3::rhi::IRenderDevice::PacingParams pace{};
    pace.warmupFrames = console.getInt("r_fpace_warmup");
    pace.spikeFactor  = console.getFloat("r_fpace_spikex");
    pace.floorMs      = console.getFloat("r_fpace_floor");
    pace.strictPso    = console.getInt("r_strictpso") != 0;
    device.setPacingParams(pace);
}

// Read the current cvar values, converting the angle cvars degrees->radians.
VmPose readViewmodelPose(const x3::con::IConsole& console) {
    return VmPose{
        console.getFloat("vm_yaw")   * kDegToRad,
        console.getFloat("vm_pitch") * kDegToRad,
        console.getFloat("vm_roll")  * kDegToRad,
        console.getFloat("vm_fwd"),
        console.getFloat("vm_right"),
        console.getFloat("vm_down"),
    };
}

// GLFW character callback: feed printable codepoints to the console input line.
void charCallback(GLFWwindow* win, unsigned int codepoint) {
    auto* ctx = static_cast<InputContext*>(glfwGetWindowUserPointer(win));
    if (ctx && ctx->hud) ctx->hud->onChar(codepoint);
}

// Mouse-wheel accumulator (weapon cycling) — moved to input_globals.h (#28
// split) so it is shared with the extracted --world streamed host. main()'s
// uses stay unqualified via the using-declarations below.
using x3::apphost::g_weaponScroll;
using x3::apphost::scrollCallback;

// GLFW key callback: the '`'/'~' toggle (always), plus console editing keys when
// the console is open. Gameplay keys are polled in the loop and gated separately.
void keyCallback(GLFWwindow* win, int key, int /*scancode*/, int action, int /*mods*/) {
    auto* ctx = static_cast<InputContext*>(glfwGetWindowUserPointer(win));
    if (!ctx || !ctx->hud) return;
    if (action != GLFW_PRESS && action != GLFW_REPEAT) return;
    x3::game::Hud& hud = *ctx->hud;

    if (key == GLFW_KEY_GRAVE_ACCENT) { if (action == GLFW_PRESS) hud.toggleConsole(); return; }
    if (!hud.consoleOpen()) return;

    switch (key) {
        case GLFW_KEY_ENTER:
        case GLFW_KEY_KP_ENTER: if (ctx->console) hud.onEnter(*ctx->console); break;
        case GLFW_KEY_BACKSPACE: hud.onBackspace(); break;
        case GLFW_KEY_UP:        hud.historyPrev(); break;
        case GLFW_KEY_DOWN:      hud.historyNext(); break;
        case GLFW_KEY_TAB:       if (ctx->console) hud.complete(*ctx->console); break;
        case GLFW_KEY_ESCAPE:    hud.closeConsole(); break;
        default: break;
    }
}

// ---- Per-system frame timers (perf hunt: where do the ~100ms/frame go?). Scoped
// accumulators summed over a window, logged as a per-section breakdown + FPS every
// kPerfWindow frames. Cheap (a few glfwGetTime() calls/frame). See docs/PERF_LOG.md.
// (Lives in the same anonymous namespace opened above — no nested re-open.)
struct PerfTimers {
    double tick = 0, healthbars = 0, frameDt = 0;  // seconds, summed over the window
    int    frames = 0;
    static constexpr int kWindow = 120;
    void addFrame(double dtSec) {
        frameDt += dtSec;
        if (++frames < kWindow) return;
        const double inv = 1.0 / (double)frames;
        const double fps = (frameDt > 1e-6) ? (frames / frameDt) : 0.0;
        x3::logInfo("[perf] " + std::to_string((int)(fps + 0.5)) + " FPS  frame=" +
                    std::to_string(frameDt * inv * 1000.0) + "ms | game.tick=" +
                    std::to_string(tick * inv * 1000.0) + "ms  healthbars=" +
                    std::to_string(healthbars * inv * 1000.0) + "ms  (rest=render+physics+hud)");
        tick = healthbars = frameDt = 0; frames = 0;
    }
};
PerfTimers g_perf;

// ---------------------------------------------------------------------------
// SpeakingMonster — the HOST dialog->skinned-NPC adapter — moved VERBATIM to
// app/speaking_monster.h (#28 monolith split). Declared in x3::apphost; the
// `using` below keeps the --demo-dialog dispatch AND the --world host call
// sites in main() unqualified so the bodies are byte-identical.
// ---------------------------------------------------------------------------
} // namespace
#include "speaking_monster.h"
using x3::apphost::SpeakingMonster;
#include "test_registry.h"   // x3::apphost::TestFlags + dispatchTests (#28 split)

// ===========================================================================
// D14 SCRIPT BOOT + GAME BINDINGS — moved VERBATIM to app/bindings.{h,cpp}
// (#28 monolith split). Declared in x3::apphost; the call sites below stay
// unqualified via the using-declarations so the bodies are byte-identical.
// ===========================================================================
#include "bindings.h"
using x3::apphost::loadBootScripts;
using x3::apphost::registerGameBindings;
using x3::apphost::submitTerminalToScripts;

// HOST SELF-TESTS — moved VERBATIM to app/self_tests.{h,cpp} (#28 monolith
// split). using-declarations keep the --test-* dispatch call sites unqualified
// (byte-identical bodies).
#include "self_tests.h"
using x3::apphost::runFrustumCullSelfTest;
using x3::apphost::runGpuCullSelfTest;
using x3::apphost::runDebrisSelfTest;
using x3::apphost::runGpuSkinSelfTest;
using x3::apphost::runHatchChainSelfTest;


int main(int argc, char** argv) {
    bool smoketest = false, testAsset = false, testConsole = false, testPhysics = false,
         testGltf = false, testPlayer = false, testInteract = false, testPickup = false,
         testPhysprops = false, testRagdoll = false, testRagdollSkin = false, testEditor = false,
         testBlockout = false,
         testBarrels = false, testGlass = false, testHoloterm = false, testLlm = false, testEcs = false, testEcsRender = false,
         testFrustumCull = false,
         testCombat = false, testAudio = false, testAcoustics = false, testLevel1 = false, testJobs = false,
         testPhase2a = false, testPhase2b = false, testAnim = false, testTerrain = false,
         testStreaming = false, testWorldStream = false, testWorldMap = false, testAi = false, testDoorCode = false, testElevator = false,
         testElevatorFsm = false,
         testTerrainPlace = false, testNet = false, testRescue = false, testDestruction = false,
         testNav = false, testWeapons = false, testVehicle = false, testVehParts = false,
         testFootIk = false,
         testScript = false,
         testNetSync = false, testNetInterp = false, testNetPredict = false, testNpcTalk = false,
         testChatTree = false,   // --test-chattree: x3.chattree/1 parse/validate + the lena walk
         testMission = false,    // --test-mission: x3.mission/1 docs + runner + the Level-1 equivalence walk
         testDeathRagdoll = false, testCanonLevel = false, testCanonPlay = false,
         testThirdPerson = false, testHatchCode = false,
         // --test-hatch: END-TO-END secret-hatch chain (terminal_code fire ->
         // boot-loaded secret_room.lua -> registerGameBindings openTrapdoor ->
         // REAL Level1Game DoorSystem hatch opens + objective line set; plus the
         // keypad submit link via the real HoloTerminal). See runHatchChainSelfTest.
         testHatch = false,
         testEcology = false, testCrowd = false, testAlert = false;
    // --test-loader (EDITOR LevelDoc data-driven loader): author a doc in memory ->
    // save -> LOAD through the real loader -> assert the built world matches; then
    // modify + hot-reload -> assert the delta applied and the create/destroy ledgers
    // balance to zero (the no-leak gate). Additive flag.
    bool        testLoader = false;
    // --world fromdoc [path]: boot the engine DIRECTLY into a LevelDoc JSON (the
    // editor's save format) — playable (walk/collide/shoot) + HOT-RELOADABLE (mtime
    // poll / `level_reload` console cmd). Default path == the editor's File>Save
    // target so the edit -> save -> reload loop closes out of the box.
    std::string docWorldPath = x3::game::defaultLevelDocPath();
    // --screenshot-loader [path.png]: headless proof — build the sample LevelDoc
    // through the REAL loader on the live device, render the room, capture a PNG.
    bool        loaderShot = false;
    std::string loaderShotPath = "build/proof/loader_room.png";
    // --set <cvar> <value> pairs, applied right after console cvar registration.
    std::vector<std::pair<std::string, std::string>> cliCVars;
    // --screenshot-perfshop [dir]: headless PERFORMANCE-SHOP proofs — boot the
    // drive world, build the shop, set the car on the lift, capture the bay
    // (car on lift + neon sign), the PARTS terminal, and the DYNO mid-pull into
    // <dir>/perfshop_{bay,parts,dyno}.png. Implies --world drive.
    bool        perfshopShot = false;
    std::string perfshopShotDir = "docs/screenshots/perfshop";
    // --screenshot-ecology [path]: LIVING-WORLD proof shot. Builds the valley
    // open biome + the ambient ecology, stages the predator-strike moment at the
    // grazer herd, settles, and captures from a herd vantage. Implies
    // --world valley + headless.
    bool        ecologyShot = false;
    std::string ecologyShotPath = "docs/screenshots/livingworld/ecology_herd_predator.png";
    // --screenshot-crowd [path]: LIVING-WORLD proof shot #2 — the Club 1127 dance
    // floor crowd (idle clusters bobbing under the blacklights). Implies
    // --world club + headless.
    bool        crowdShot = false;
    std::string crowdShotPath = "docs/screenshots/livingworld/club_crowd.png";
    // --screenshot-alert [path]: LIVING-WORLD proof shot #3 — Level 1 under a
    // forced ALERT 3 LOCKDOWN (zone doors locked, red-shifted lights, the alert
    // HUD pips + LOCKDOWN banner + pulsing red frame). Rides the --screenshot
    // production-HUD corridor path.
    bool        alertShot = false;
    std::string alertShotPath = "docs/screenshots/livingworld/alert3_lockdown.png";
    // --test-rt (hardware ray-tracing RT AO): runs the headless smoketest render
    // path with r_rtao forced ON so the BLAS/TLAS build + ray-query AO compute +
    // apply passes are exercised under Vulkan validation on an RT-capable device.
    bool        testRt = false;
    bool        testReflections = false;   // --test-reflections: SSR + ray-query refl under validation
    bool        noRefl = false;            // --norefl: reflections off, TAA on (refl A/B isolate)
    // --test-ddgi (DDGI probe-grid GI): headless smoketest with r_ddgi forced ON so
    // the BLAS/TLAS build + ddgi_rays/ddgi_update compute + mesh.frag sampling run
    // under Vulkan validation. No-op on non-RT / no-position-fetch devices.
    bool        testDdgi = false;
    bool        ddgiForce = false;         // --ddgi: force r_ddgi 1 (screenshot/showroom A/B)
    // --test-bestiary (bestiary pass): the data-driven enemy roster. Additive flag.
    bool        testBestiary = false;
    // --test-bosses (Act-1 bosses, Wave 1): the 5 mid-boss defs + the multi-pod
    // machine + the scripted pre-fight hook + the Martinez regression guard. Additive.
    bool        testBosses = false;
    // --test-act2bosses (Act-2 roster, Wave 2): the 5 alien-planet-surface enemy
    // defs + 4 single-body bosses (Memory Hunter / Siren / Breeder Queen / Garrison
    // Commander) + the Wave-2 Tuning tags (startAllied / copyFeintPhase /
    // escapeTimerSeconds) + the Act-1 + Martinez regression guard. Additive.
    bool        testAct2Bosses = false;
    // --test-ui (UI pass): general game-UI layer (menus + HUD). Additive flag.
    bool        testUi = false;
    // --test-loading (loading-screen pass, Task #49): asserts progress is monotonic
    // 0->1 over the load steps + the tip line rotates. Headless. Additive flag.
    bool        testLoading = false;
    // --test-saveload (save/load pass): versioned checkpoint serialization. Additive.
    bool        testSaveLoad = false;
    // --test-dialog (AI-dialog + TTS pass, §3): the authored dialogue TREE advances
    // through nodes + player-choice branches OFFLINE; a stub AI provider hook is used
    // when set (else falls back to the tree); a stub TTS hook drives the NPC into/out
    // of the SPEAKING state. Fully offline + leak-clean; no network. Additive.
    bool        testDialog = false;
    // --demo-dialog [glb] (headless, offline): run the sample Sarah conversation
    // through a REAL skinned-NPC adapter (SpeakingMonster) that drives an anim
    // Skinner talk-bob (read-only) over a character GLB, with an offline stub TTS
    // hook pacing the speaking state. Prints each subtitle + the speaking
    // transitions + asserts the talk-bob actually animated the skeleton. Defaults
    // to chief_martinez_anim.glb. No window / Vulkan / network. Additive.
    bool        demoDialog = false;
    std::string demoDialogPath;
    // --test-valley (Crystal Valleys Act-2 L15) + --test-cliffs (Salvari cliffs finale).
    bool        testValley = false, testCliffs = false;
    // --test-secretroom (code-locked trapdoor -> secret room): the cell HoloTerminal
    // override code opens a floor-hatch to a stocked secret room below. Additive flag.
    bool        testSecretRoom = false;
    // --test-club (the full Club 1127 "THE DEEP" at Y=-200): build headless + assert
    // the key fixtures (DJ booth, ORB, bars, 12-step stair, PA rig, 28 blacklights,
    // 6 TVs, the 50x100x30 ft room footprint/Y) + leak-clean. Additive flag.
    bool        testClub = false;
    // --test-spiremid (Spire mid-floor content): F3/F4/F5 encounter authoring. Additive.
    bool        testSpireMid = false;
    // --test-nexus (Floor 4.5 Nexus / The Chorus): off-elevator multi-pod boss. Additive.
    bool        testNexus = false;
    // --test-debris (K-T2 GPU-compute debris): spawn a burst, step the compute sim
    // through the live headless device, assert fall+settle+expiry+no-leak. Additive.
    bool        testDebris = false;
    // --test-gpuskin (GPU SKINNING OF MODELS): register a skinned mesh on the live
    // headless device, set a known palette, run the compute skinning pass, read back
    // the skinned output, and assert it matches a CPU LBS reference. Additive.
    bool        testGpuSkin = false;
    // --test-meshlet (D15 Tier-2 CPU meshlet builder): runs runMeshletSelfTest() —
    // builds meshlets from a generated grid mesh and asserts budgets/locality/sphere
    // containment/cone tightness/triangle conservation/degenerate input. Pure CPU,
    // no device needed. Additive.
    bool        testMeshlet = false;
    // --test-gpucull (D15 Tier-0 GPU cull): the EQUIVALENCE acceptance test — the
    // real device headless (validation on), GPU cull active, the CPU evaluating the
    // identical predicate per frame; asserts statDrawn == expected over a pose
    // sweep + conservation (drawn+culled==tested) + bypass + path-toggle. Additive.
    bool        testGpuCull = false;
    // --cullpath <n> / --hzb: seed the r_cullpath / r_hzb cvars from the CLI so the
    // smoketest/screenshot/bench paths exercise the D15 GPU cull (INT_MIN = unset).
    int         cullPathArg = INT_MIN;
    int         hzbArg = 0;
    // --test-spiretop (Spire top-floor content): F6/F7 (Act-1 finale) encounter authoring. Additive.
    bool        testSpireTop = false;
    // --test-timeline (EFLZ morality/timeline backbone): infection 4-stage timers + cure
    // rates, the Omega/Alpha/Beta/Gamma timeline selector, the morality axes, and the
    // 12-ending eligibility map. Additive.
    bool        testTimeline = false;
    // --test-dronehack (F5 Drone Manufacturing): Sarah's master hack strips the Swarm
    // Controller AI's HP fraction + flips the drone set to allied (gated, not at load). Additive.
    bool        testDroneHack = false;
    // --test-sublevels (hidden Floor-7 sub-levels + Dr. Chen Return Mission): asserts the
    // descent is HIDDEN/inert until the F7-complete gate (Clone fallen + Sarah saved), then
    // SL1/SL2/SL3 build with the Frozen Collective mini-boss + a rescuable Dr. Chen. Additive.
    bool        testSubLevels = false;
    // --test-act2 (EFLZ Act-2 open-world surface): the alien-planet host + L8 Surface
    // Emergence (lab-exit gauntlet -> Emergence Point safe zone) + L9 Crystalline Desert
    // Edge (crystal props + neutral fauna + an inert-until-entered hazard zone). Additive.
    bool        testAct2 = false;
    // --test-act2desert (EFLZ Act-2 desert depths): L10 Crystalline Desert Depths
    // (deeper desert, first-contact allied Salvari + an injured-Salvari side-quest,
    // a hidden crystal-cave camp entrance, a light Overlord patrol) + L11 Salvari
    // Camp "Refugee Haven" (cave settlement, survivor markers incl. K'thara, an
    // upgrade-station interact + cultural-exchange beat). Reachable L9->L10->L11. Additive.
    bool        testAct2Desert = false;
    // --test-act2caves (EFLZ Act-2 mid biomes L12-15): the bioluminescent Advanced Cave
    // System (Crystal Heart dual-gated interactable + Memory Hunter abyss boss) + the
    // Toxic Swamplands edge (poison hazard zone, inert at load) + the Research Station
    // (timeline-gated Siren ambush) + the Tree Cities (vertical canopy + trading-post
    // interactable). Asserts the gates, the hazard, the timeline gate, reachability
    // L11->L12->L13->L14->L15, and trigger-id non-collision. Additive flag.
    bool        testAct2Caves = false;
    // --test-tod (EFLZ Time-of-Day): a 4-phase day cycle (dawn/day/dusk/night) that
    // drives the analytic sky/sun (dir/color/intensity/haze + ambient) via SkyParams.
    // Asserts the cycle visits all phases + wraps, the sun arc + intensity vary
    // monotonically across the day, city-lights/aurora gate on night, deterministic. Additive.
    bool        testTod = false;
    // --test-weather (EFLZ Weather): 7 states (clear/cloudy/rain/storm/fog/sandstorm/snow)
    // with smooth 30 s transitions, biome-gated, each nudging sky/fog/ambient + a hazard
    // flag. Asserts gating, interpolated transitions, hazard set only in hazardous states
    // (incl. swamp poison-fog), midpoint hazard flip, and determinism. Additive.
    bool        testWeather = false;
    bool        testWorldRegions = false;   // --test-worldregions (open-world surface regions + mountains)
    bool        testCity = false;           // --test-city (open-world metropolis: districts + roads + tunnels)
    bool        testOceanBase = false;      // --test-oceanbase (ocean + undersea base + submarine combat)
    // --test-collapse (K-T3 structural collapse): build a small structure (column /
    // beam on two supports), destroy a support, step the sim, and assert the
    // unsupported pieces fall (static->dynamic), anchored pieces stay stable, the
    // rubble settles bounded/NaN-free, GPU debris fires, and it's leak-clean. Additive.
    bool        testCollapse = false;
    // --test-physjoint (Physics §1): create a dynamic body on a point/distance
    // constraint, step the sim, and assert it hangs + swings under gravity then
    // settles with damping, and re-settles after an impulse; no NaNs; leak-clean.
    // Additive.
    bool        testPhysJoint = false;
    // --test-ragdoll (Physics §2): build a ragdoll from a synthetic skeleton, step,
    // assert it falls + settles (bounded, no NaN), the constraint chain holds (bone
    // lengths preserved), and the anim<->ragdoll blend 0->1 interpolates the palette
    // monotonically. Additive. (testRagdoll is declared in the block above.)
    // Clip-listing check (--list-clips <glb>): load a skinned GLB headless and
    // report its animation clip count + names, then sample Walk at t=0 vs t=0.5
    // and confirm the joint palette changes. Asset-pipeline verification for the
    // retargeted multi-clip character GLBs; no window / Vulkan. Additive — does
    // not affect the existing self-test gate.
    bool        listClips = false;
    std::string listClipsPath;
    // Locomotion-blend check (--test-locomotion [glb]): load a multi-clip GLB and
    // exercise the 1D idle/walk/run blend + Jump crossfade headless. Defaults to
    // chief_martinez_anim.glb if no path is given. No window / Vulkan. Additive.
    bool        testLocomotion = false;
    std::string testLocomotionPath;
    // --test-intro (intro cold-open): the prologue phase machine (Jake's last flight -> enemy
    // pulse -> white-out crash -> "6 MONTHS LATER" -> handoff to the cell) advances in order and
    // is skippable. No window / Vulkan. Additive — does not affect the existing gate.
    bool        testIntro = false;
    // Stress test: add N procedural cubes to the scene at startup (--stress N).
    // Default 0 = OFF; Level 1 is unaffected unless requested.
    uint32_t stressCount = 0;
    // Benchmark mode (--bench N [frames]): spawn N cubes, point the camera at the
    // field, run `frames` frames with vsync OFF, and report averaged FPS/CPU/GPU
    // ms. Headless of gameplay (no input); used to produce the perf baseline.
    bool     bench = false;
    uint32_t benchFrames = 600;
    // Screenshot mode (--screenshot [path.png]): build EFLZ Level 1, pose the
    // camera at a representative corridor vantage, render a few frames so shadows
    // + art settle, read the color image back to CPU, write a PNG, and exit 0.
    // Used to judge how the game looks without being at the keyboard. Default path
    // when omitted: G:\X3Native\screenshot.png.
    bool        screenshot = false;
    std::string screenshotPath = "G:/X3Native/screenshot.png";
    // Level Architect EDITOR mode (--editor): boot the live canon world with the Dear
    // ImGui (docking) editor overlay enabled. With no --editor flag ImGui never
    // initializes (zero cost — byte-for-byte the shipping game). Phase 0 draws only a
    // dockspace + the ImGui demo window (proof the integration renders); Phase 1 hosts
    // the real panels.
    bool        editorMode = false;
    // Headless editor proof (--screenshot-editor [path.png]): init ImGui in a headless
    // device, render ONE frame with the dockspace + demo window, and capture a PNG that
    // shows the ImGui window — proves the Phase-0 integration actually rasterizes.
    // NOTE: ImGui normally inits only in windowed --editor; this proof path is the
    // single exception (a forced headless ImGui init) so the render can be verified
    // without a display. Default path: build/proof/editor_p0.png.
    bool        editorShot = false;
    std::string editorShotPath = "build/proof/editor_p0.png";
    // UI-demo capture (--ui-demo [path.png] / --screenshot-menu): build EFLZ Level 1,
    // pose the gate-standard corridor camera, then draw the GENERAL game-UI MAIN MENU
    // (title + START / QUIT, the START button focused/hot) over the rendered scene and
    // capture a PNG — so the menu layer can be SEEN headlessly without being at the
    // keyboard. Additive + offscreen, like --screenshot. Default path:
    // G:/X3Native/captures/ui_menu.png.
    bool        uiDemo = false;
    std::string uiDemoPath = "G:/X3Native/captures/ui_menu.png";
    // Chat-tree dialog capture (--screenshot-dialog [path.png]): the --screenshot
    // path with the camera posed at the F5 captive (Lena) and her first_meeting
    // chat tree OPEN, so the choice UI can be judged headlessly. Additive.
    bool        dialogShot = false;
    std::string dialogShotPath = "docs/screenshots/dialog/lena_dialog.png";
    // Which UI screen the --ui-demo capture shows: "main" (default), "pause", or
    // "settings". Lets one flag document all three menu screens.
    std::string uiDemoScreen = "main";
    // Sky vantage mode (--screenshot-sky [path.png]): build a minimal OUTDOOR test
    // scene (ground plane + the analytic sky lit by the existing sun), pose the
    // camera at the horizon looking slightly up toward the sun, render a few
    // settle frames, and capture a PNG that shows the sky gradient + sun disk +
    // horizon. EFLZ Level 1 is an enclosed interior, so this is the way to SEE the
    // open-world sky. Default path when omitted: G:\X3Native\sky.png.
    bool        skyShot = false;
    std::string skyShotPath = "G:/X3Native/sky.png";
    // --legacypost A/B: 1 = auto-exposure off (the pre-post-stack look);
    // 2 = also bloom off + tonemap passthrough (raw HDR clamp debugging).
    // BOTH levels also force TAA off — "legacy" means the pre-strike renderer,
    // and the bit-identical guarantee predates the TAA jitter.
    int         legacyPost = 0;
    // --notaa A/B: disable TAA only (jitter fully off + resolve skipped) so
    // before/after screenshots isolate exactly the TAA contribution.
    bool        noTaa = false;
    // Showroom preview (--screenshot-showroom [path.png]): load the baked Unity scene
    // export (assets/converted_glb/ShowRoom_Vol30/Example_01.glb), frame the camera on
    // the building cluster, capture a PBR-shaded PNG. Headless, like --screenshot.
    bool        showroomShot = false;
    std::string showroomShotPath = "G:/X3Native/showroom.png";
    // HERO CAR showcase (--screenshot-car [outDir]): load the converted hero-car
    // GLB (assets/converted_glb/Vehicles/CTR.glb — clearcoat paint + emissive
    // lights), pose it INSIDE the Unity showroom on a polished reflector slab
    // under emissive light panels (SSR/RT reflections sweep the body), and
    // capture a TURNTABLE set: 4 day-interior angles + 2 night-interior angles +
    // 2 night-EXTERIOR angles under the planet sky on wet asphalt. Headless,
    // 4x SSAA. Writes <outDir>/car_*.png (default docs/screenshots/vehicles).
    bool        carShot = false;
    std::string carShotDir = "docs/screenshots/vehicles";
    // FIRST-PERSON showroom proof (--screenshot-showroom-fp [path.png]): run the SAME
    // interactive `--world showroom` setup (walkable floor slab + companion Aria + the
    // wheeling night sky) but render ONE headless frame from the PLAYER SPAWN eye and
    // capture a PNG. This is the headless proof that the walkable content is correct
    // (interactive WASD/mouse can't be exercised headlessly). Default: G:\X3Native\showroom_fp.png.
    bool        showroomFpShot = false;
    std::string showroomFpShotPath = "G:/X3Native/showroom_fp.png";
    // RAGDOLL PROOF (--screenshot-showroom-ragdoll [path.png]): same headless showroom-FP
    // setup, but call girl.ragdoll() and step the physics world ~45 frames so Aria
    // COLLAPSES, then capture one frame. The frame must show her in a physics heap
    // (driven by applyExternalGlobals from the ragdoll), NOT the standing idle pose —
    // the headless proof the ragdoll drives the skin. Default: G:\X3Native\showroom_ragdoll.png.
    bool        showroomRagdollShot = false;
    std::string showroomRagdollShotPath = "G:/X3Native/showroom_ragdoll.png";
    // GLASS-DECK / ELEVATOR / HIDDEN-STAIR proofs (additive spire-top experience).
    // Each forces --world showroom on, then captures ONE headless frame from a
    // vantage that proves the new feature, and exits:
    //   --screenshot-showroom-deck  [path] — camera ON the glass deck at the spire
    //       top (~y=90), looking out at the night sky (deck glass + rails + planets).
    //   --screenshot-showroom-floor2 [path] — standing ON the 2nd floor (y=3) having
    //       climbed the synthesized stair; proves the climb collision + 2nd-floor slab.
    //   --screenshot-showroom-door  [path] — the hidden 2nd-floor WALL DOOR, shown
    //       OPEN (slid aside) revealing the entry passage behind it (set X3_SHOWROOM_
    //       DOORCLOSED=1 to instead capture it CLOSED/concealed flush in the wall).
    //   --screenshot-showroom-elevator [path] — the glass car in the ELEVATOR ATRIUM
    //       (the white room above the 2nd floor where the lift boards), camera inside.
    //   --screenshot-showroom-stair [path] — the entry PASSAGE + 90 deg TURN + the
    //       FLIGHT OF STAIRS climbing up to the elevator atrium.
    bool        showroomDeckShot = false;
    std::string showroomDeckShotPath = "C:/GameDev/X3Native-engine/build/proof/showroom_deck.png";
    bool        showroomElevShot = false;
    std::string showroomElevShotPath = "C:/GameDev/X3Native-engine/build/proof/showroom_elevator.png";
    bool        showroomStairShot = false;
    std::string showroomStairShotPath = "C:/GameDev/X3Native-engine/build/proof/showroom_stair.png";
    bool        showroomFloor2Shot = false;
    std::string showroomFloor2ShotPath = "C:/GameDev/X3Native-engine/build/proof/showroom_floor2.png";
    bool        showroomDoorShot = false;
    std::string showroomDoorShotPath = "C:/GameDev/X3Native-engine/build/proof/showroom_door.png";
    //   --screenshot-showroom-struts [path] — EXTERIOR shot from outside the building
    //       looking back at it, framing the SYMMETRIC radial set of thickened "/"
    //       strut legs (all four matched). Default build/proof/showroom_struts.png.
    bool        showroomStrutsShot = false;
    std::string showroomStrutsShotPath = "C:/GameDev/X3Native-engine/build/proof/showroom_struts.png";
    // HIDDEN ANALYST GALLERY proofs (--screenshot-showroom-gallery [path]): the secret
    // surveillance level ringing the central void at the elevator level (~Y10-13). The
    // flag captures TWO frames in one run: (a) the gallery itself — terminals glowing +
    // analyst figures around the ring — to <path>; (b) the view from the gallery looking
    // DOWN through the dark one-way glass onto the civilian floor/pad to <path>_down.png.
    // X3_SHOWROOM_GALLERY_UP=1 instead captures the civilian-floor view looking UP at the
    // dark-glass ceiling band (proving the analysts read dark/hidden from below).
    bool        showroomGalleryShot = false;
    std::string showroomGalleryShotPath = "C:/GameDev/X3Native-engine/build/proof/showroom_gallery.png";
    // CIVILIAN-FLOOR proof (--screenshot-showroom-civilians [path]): a wide DAY view
    // looking across the GROUND floor (blue pad + lounge) so the civilian crowd reads,
    // then a second frame from the 2nd-floor mezzanine deck (<path>_mezz.png). Proves
    // the civilians populate both floors naturally, day-lit, standing on the floor.
    bool        showroomCivShot = false;
    std::string showroomCivShotPath = "C:/GameDev/X3Native-engine/build/proof/showroom_civilians.png";
    // Planet preview (--screenshot-planet [path.png]): build a UV-sphere Moon body,
    // load the 5 FORGE3D Moon textures, light it from the side so a day/night
    // terminator reads, hang it against a dark space backdrop, and capture a PNG.
    // Headless, 4x SSAA, like --screenshot-showroom. Default path: G:\X3Native\planet.png.
    bool        planetShot = false;
    std::string planetShotPath = "G:/X3Native/planet.png";
    // Night-sky preview (--screenshot-nightsky [path.png]): build ONE UV-sphere and
    // hang ~6 VARIED planet TYPES (Moon, Ice, Gas, Lava, Terrestrial, Sun) staggered
    // across a dark, star-flecked dome, each shaded by its own per-type pipeline, and
    // capture a PNG. Headless, 4x SSAA, like --screenshot-planet. Default path:
    // G:\X3Native\nightsky.png.
    bool        nightskyShot = false;
    std::string nightskyShotPath = "G:/X3Native/nightsky.png";
    // ---- x3.cutscene/1 CLI (docs/design/CUTSCENE_FORMAT.md) ----
    // --test-cutscene          : headless format/eval/player self-test.
    // --skipintro              : never play the cold open this run.
    // --cutscene <file>        : play THAT cutscene at boot regardless of StoryFlags
    //                            (authoring loop; default = the shipped cold open).
    // --cuetime <s>            : start the played cutscene scrubbed to s seconds.
    // --cutscene-shot [path]   : HEADLESS film still — build the cinematic scene,
    //                            seek to --cuetime, capture one frame, exit. 4x SSAA.
    bool        testCutscene = false;
    bool        skipIntro    = false;
    std::string cutsceneFile;                  // empty = the shipped cold open
    float       cueTime      = 0.0f;
    bool        cutsceneShot = false;
    std::string cutsceneShotPath = "G:/X3Native/cutscene.png";
    // DDGI gate-shot proof (--screenshot-ddgi [outDir]): build a minimal sealed
    // two-room rig (room A holds a point light + an emissive ceiling panel; room B
    // is connected only through a doorway; room C is fully SEALED next to A — the
    // leak canary), render OFF/ON/debug captures headless and exit. Probes converge
    // over ~120 settle frames before each ON capture.
    bool        ddgiShot = false;
    std::string ddgiShotDir = "docs/screenshots/ddgi";
    // RT soft-shadow gate-shot proof (--screenshot-rtshadows [outDir]): build a
    // detention-cell rig (a single ceiling lamp + occluders at two distances from
    // the wall), capture lamp-shadow OFF/ON A/Bs (tier 0 vs 2), a sun CSM-vs-RT
    // A/B (contact hardening), and a 3-frame motion burst (TAA sizzle check);
    // logs the GPU-ms cost delta. Headless; exits after the captures.
    bool        rtshShot = false;
    std::string rtshShotDir = "docs/screenshots/rtshadows";
    // --test-rtshadows: headless smoketest with r_rtshadows forced to tier 2 so
    // the mesh_rt pipelines + TLAS path run under Vulkan validation.
    bool        testRtShadows = false;
    bool        noRtShadows = false;       // --nortshadows: pin r_rtshadows 0 (CSM-only A/B)
    // --test-framepacing (ZERO-STUTTER GUARANTEE, docs/ZERO_STUTTER.md): headless
    // 600-frame scripted camera flythrough of the built world (default Level 1)
    // that asserts, post-warmup: ZERO spike frames (> r_fpace_spikex * rolling
    // median + r_fpace_floor), ZERO pipelines/shader modules created after frame
    // 1, ZERO descriptor-pool growth. Prints the CPU+GPU p50/p95/p99/p999/max.
    bool        testFramePacing = false;
    // --test-boottime [budgetMs] (BOOT-TO-GAMEPLAY gate, docs/BOOT_TIME.md): boot the
    // REAL windowed interactive path (window + swapchain + full world build), skip the
    // intro cold-open (content, not boot work), run exactly ONE main-loop frame (the
    // first interactive frame: world built, menu live, player controllable on START),
    // print the [boot] phase table, and exit 0 iff total < the budget. The budget
    // defaults to the boot_budget_ms cvar default (2000 ms) and can be loosened for
    // weaker machines via the optional CLI arg or `boot_budget_ms` in the console cfg.
    bool        testBootTime = false;
    double      bootBudgetMs = 0.0;    // 0 = use the boot_budget_ms cvar (default 2000)
    // Terrain vantage mode (--screenshot-terrain [path.png]): build the tiled
    // procedural terrain world (terrain + sky + sun), pose a camera up on the
    // hills looking toward the sun so the lit rolling terrain + cast shadows +
    // sky read clearly, settle a few frames, and capture a PNG. Default path when
    // omitted: G:\X3Native-wt-terrain\terrain.png.
    bool        terrainShot = false;
    std::string terrainShotPath = "G:/X3Native-wt-terrain/terrain.png";
    // Ocean vantage mode (--screenshot-ocean [path.png]): build the procedural
    // terrain world + an animated ocean at sea level under the sky/sun, pose a
    // camera on the shore looking out across the water toward the sun so the lit
    // animated waves, sun glint, depth-based shallow/deep color, and the
    // terrain->water shoreline all read, settle a few frames so the waves animate
    // + the shadow map registers, and capture a PNG. EFLZ Level 1 is interior;
    // this is the way to SEE the ocean. Default path: G:\X3Native-wt-water\ocean.png.
    bool        oceanShot = false;
    std::string oceanShotPath = "G:/X3Native-wt-water/ocean.png";
    // Destruction shatter capture (--screenshot-destruct [path.png]): build the
    // destruction demo world (lit ground + a row of destructible crates), shoot +
    // explode the crates so they shatter into tumbling convex chunks, settle a few
    // frames, and capture a PNG showing the intact->broken transition + scattered
    // chunks. Headless / offscreen, like --screenshot. Default path:
    // G:/X3Native/captures/destruct.png.
    bool        destructShot = false;
    std::string destructShotPath = "G:/X3Native/captures/destruct.png";
    // AI-action capture mode (--capture-ai [outDir]): build a clearly-lit demo arena
    // (lit ground + sky + point/sun fill) with a fixed player reference and a small
    // squad of enemies driven by the REAL combat-AI state machine (a Guard advances,
    // a Drone strafes, one enemy is damaged mid-run -> Retreat, one loses LOS ->
    // Search), pose a fixed 3/4 elevated camera, step the sim at fixed dt for ~6 s,
    // capture a numbered PNG frame every ~0.2 s into outDir, assemble an animated
    // GIF (G:\X3Native\ai_action.gif), and print a per-phase state log. Headless /
    // offscreen (no window), like --screenshot. Default outDir: G:\X3Native\ai_action.
    bool        captureAi    = false;
    std::string captureAiDir = "G:/X3Native/ai_action";
    // Walk-capture mode (--capture-walk [outPath]): build ONE close-up animated
    // guard (the multi-clip *_anim.glb when present), drive the T1 locomotion blend
    // toward a steady WALK, settle the blend a fraction of a second, then capture a
    // single PNG at a clearly mid-stride moment. Verifies the locomotion blend
    // visibly in-engine. Headless / offscreen. Default outPath: build/walk_pose.png.
    bool        captureWalk     = false;
    std::string captureWalkPath = "G:/X3Native-wt-animt1/build/walk_pose.png";
    // Foot-IK capture (--screenshot-footik [outPath]): build ONE animated character
    // standing on a SLOPED + STEPPED surface with foot-IK ON, drive a slow idle/walk
    // blend, plant the feet on the surface (raycast down via the local physics world),
    // adjust the pelvis, settle, and capture a single PNG showing the feet grounded on
    // the slope/step (vs floating). Headless / offscreen. Default: build/footik_pose.png.
    bool        captureFootIk     = false;
    std::string captureFootIkPath = "G:/X3Native-wt-footik/build/footik_pose.png";
    // Spire per-floor capture (--capture-spire [outDir]): build the FULL Act-1 host
    // (Level1Game + SpireMidFloors + SpireTopFloors, the same real lit scene the game
    // builds), then for EACH Spire floor B1,F1,F2,F3,F4,F5,F6,F7 pose the camera at
    // that floor's arrival/hub vantage looking across the main room, light the plate,
    // settle a few frames, and capture <outDir>/spire_<floor>.png. A dev/playtest
    // tool: it CHANGES NO gameplay/balance — it only renders + reads back the scene
    // each floor already builds. Headless / offscreen (no window), like --screenshot.
    // Prints one line per floor (path + that floor's enemy count). 0 VUID under Debug.
    bool        captureSpire    = false;
    std::string captureSpireDir = "captures/spire";
    // World selector (--world terrain): launch the playable OUTDOOR terrain world
    // (walk the hills) instead of the default interior Level 1. Anything else (or
    // omitted) keeps Level 1 as the default, unchanged.
    std::string worldMode = "level1";
    bool        worldExplicit = false;   // --world was passed (vs the default)
    bool shotWorldMap = false;   // --screenshot-worldmap (headless map shot sequence)
    // Seamless world streaming tunables (--world streamed; see app/world_stream.*):
    // per-frame stream-work budget (ms) + velocity lookahead (s). Cvar-style CLI
    // overrides: --ws-budget <ms> / --ws-lookahead <s>.
    float wsBudgetMs   = 6.0f;
    float wsLookaheadS = 2.5f;
    // Optional settle-frame count for --screenshot (default 16 = unchanged
    // behavior). Larger values advance the world (and the characters' skeletal
    // animation) further before the capture, so two shots at different counts show
    // different animated poses — used to prove J1 animation is live.
    int         screenshotSettle = 16;
    // Optional --screenshot camera override (--shot-cam x,y,z,yaw,pitch). When set,
    // the screenshot uses this vantage instead of the default corridor pose — used
    // to capture the tall arena / elevator shaft (the default corridor pose stays
    // the gate-standard view). Does NOT change any default behavior when omitted.
    bool        shotCamOverride = false;
    float       shotCam[5] = { 8.0f, 1.75f, -0.4f, 0.06f, -0.16f };
    // FX demo (--fx-demo): in --screenshot mode, spawn a combat particle/decal burst
    // (muzzle flash + impact sparks + dust + a scorch decal) a couple meters in front
    // of the screenshot camera each settle frame so the capture clearly shows the new
    // GPU particles (glowing via bloom, soft against depth) + a bullet decal on the
    // surface. Off by default — the standard --screenshot gate view is unchanged.
    bool        fxDemo = false;
    // Windowed-mode resolution (--width <px> / --height <px>). Defaults to the
    // historical 1280x720 so the dev box + every headless/offscreen path are
    // UNCHANGED. A high-DPI box can pass e.g. --width 2560 --height 1440. These
    // affect ONLY the on-screen window: headless capture/screenshot resolution is
    // forced back to 1280x720 below regardless of these flags.
    uint32_t    winW = 1600, winH = 900;   // bigger windowed default (NOT maximized)
    const bool  loadedWinSize = readWindowSize(winW, winH);   // saved "SET AS DEFAULT" size
    (void)loadedWinSize;
    for (int i = 1; i < argc; ++i) {
        std::string_view a(argv[i]);
        // DDGI flags handled OUTSIDE the big else-if chain below (MSVC C1061:
        // every `else if` nests a block; the chain is at the compiler's limit).
        if (a == "--test-ddgi") { smoketest = true; testDdgi = true; continue; }
        if (a == "--ddgi") { ddgiForce = true; continue; }
        if (a == "--screenshot-ddgi") {
            ddgiShot = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') ddgiShotDir = argv[++i];
            continue;
        }
        // RT soft-shadow flags — handled OUTSIDE the chain (same C1061 reason).
        if (a == "--test-rtshadows") { smoketest = true; testRtShadows = true; continue; }
        // Zero-stutter flythrough — handled OUTSIDE the chain (same C1061 reason).
        if (a == "--test-framepacing") { testFramePacing = true; continue; }
        if (a == "--test-boottime") {
            testBootTime = true;
            if (i + 1 < argc && argv[i + 1][0] != '-')
                bootBudgetMs = std::atof(argv[++i]);
            continue;
        }
        if (a == "--nortshadows") { noRtShadows = true; continue; }   // A/B: pin tier 0 (CSM-only)
        if (a == "--screenshot-rtshadows") {
            rtshShot = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') rtshShotDir = argv[++i];
            continue;
        }
        // World-streaming flags handled OUTSIDE the big else-if chain (which sits at
        // MSVC's C1061 block-nesting limit — adding to it breaks the build).
        if (a == "--test-worldstream") { testWorldStream = true; continue; }
        if (a == "--test-worldmap")    { testWorldMap    = true; continue; }
        if (a == "--screenshot-worldmap") {   // headless world-map shot sequence
            shotWorldMap = true; worldMode = "streamed"; screenshot = true; continue;
        }
        if (a == "--ws-budget") {   // per-frame world-stream budget, ms (cvar-style tunable)
            if (i + 1 < argc && argv[i + 1][0] != '-') wsBudgetMs = std::strtof(argv[++i], nullptr);
            continue;
        }
        if (a == "--ws-lookahead") { // velocity lookahead, seconds
            if (i + 1 < argc && argv[i + 1][0] != '-') wsLookaheadS = std::strtof(argv[++i], nullptr);
            continue;
        }
        if (a == "--smoketest") smoketest = true;
        else if (a == "--legacypost")  legacyPost = 1;   // A/B: auto-exposure OFF (pre-strike look)
        else if (a == "--legacypost2") legacyPost = 2;   // A/B: + bloom OFF + tonemap passthrough
        else if (a == "--notaa")       noTaa = true;     // A/B: TAA off (jitter + resolve disabled)
        else if (a == "--norefl")      noRefl = true;    // A/B: reflections off (TAA stays on)
        else if (a == "--test-rt") { smoketest = true; testRt = true; }
        else if (a == "--test-reflections") { smoketest = true; testReflections = true; }
        else if (a == "--test-jobs") testJobs = true;
        else if (a == "--test-asset") testAsset = true;
        else if (a == "--test-console") testConsole = true;
        else if (a == "--test-physics") testPhysics = true;
        else if (a == "--test-gltf") testGltf = true;
        else if (a == "--test-player") testPlayer = true;
        else if (a == "--test-interact") testInteract = true;
        else if (a == "--test-physprops") testPhysprops = true;
        else if (a == "--test-ragdoll") testRagdoll = true;
        else if (a == "--test-ragdollskin") testRagdollSkin = true;
        else if (a == "--test-editor") testEditor = true;
        else if (a == "--test-loader") testLoader = true;
        else if (a == "--test-blockout") testBlockout = true;
        else if (a == "--test-barrels") testBarrels = true;
        else if (a == "--test-glass") testGlass = true;
        else if (a == "--test-frustumcull") testFrustumCull = true;
        else if (a == "--test-holoterm") testHoloterm = true;
        else if (a == "--test-llm") testLlm = true;
        else if (a == "--test-secretroom") testSecretRoom = true;
        else if (a == "--test-ecs") testEcs = true;
        else if (a == "--test-ecsrender") testEcsRender = true;
        else if (a == "--test-pickup") testPickup = true;
        else if (a == "--test-combat") testCombat = true;
        else if (a == "--test-deathragdoll") testDeathRagdoll = true;
        else if (a == "--test-audio") testAudio = true;
        else if (a == "--test-acoustics") testAcoustics = true;
        else if (a == "--test-level1") testLevel1 = true;
        else if (a == "--test-canonlevel") testCanonLevel = true;
        else if (a == "--test-canonplay") testCanonPlay = true;
        else if (a == "--test-phase2a") testPhase2a = true;
        else if (a == "--test-phase2b") testPhase2b = true;
        else if (a == "--test-anim") testAnim = true;
        else if (a == "--test-terrain") testTerrain = true;
        else if (a == "--test-terrainplace") testTerrainPlace = true;
        else if (a == "--test-streaming") testStreaming = true;
        else if (a == "--test-ai") testAi = true;
        else if (a == "--test-bestiary") testBestiary = true;
        else if (a == "--test-bosses") testBosses = true;
        // (chain break — restart the if/else-if ladder so MSVC stays under the
        // C1061 block-nesting limit; flags are exact == matches, all unique, so a
        // matched arg simply falls through the second ladder without re-matching)
        if (false) {}
        else if (a == "--test-act2bosses") testAct2Bosses = true;
        else if (a == "--test-spiremid") testSpireMid = true;
        else if (a == "--test-nexus") testNexus = true;
        else if (a == "--test-spiretop") testSpireTop = true;
        else if (a == "--test-timeline") testTimeline = true;
        else if (a == "--test-dronehack") testDroneHack = true;
        else if (a == "--test-sublevels") testSubLevels = true;
        else if (a == "--test-act2") testAct2 = true;
        else if (a == "--test-act2desert") testAct2Desert = true;
        else if (a == "--test-act2caves") testAct2Caves = true;
        else if (a == "--test-tod") testTod = true;
        else if (a == "--test-weather") testWeather = true;
        else if (a == "--test-worldregions") testWorldRegions = true;
        else if (a == "--test-city") testCity = true;
        else if (a == "--test-oceanbase") testOceanBase = true;
        else if (a == "--test-doorcode") testDoorCode = true;
        else if (a == "--test-hatchcode") testHatchCode = true;
        else if (a == "--test-hatch") testHatch = true;
        else if (a == "--test-elevator") testElevator = true;
        else if (a == "--test-elevatorfsm") testElevatorFsm = true;
        else if (a == "--test-net") testNet = true;
        else if (a == "--test-netsync") testNetSync = true;
        else if (a == "--test-netinterp") testNetInterp = true;
        else if (a == "--test-netpredict") testNetPredict = true;
        else if (a == "--test-rescue") testRescue = true;
        else if (a == "--test-thirdperson") testThirdPerson = true;
        else if (a == "--test-npctalk") testNpcTalk = true;
        else if (a == "--test-chattree") testChatTree = true;
        else if (a == "--test-mission") testMission = true;
        else if (a == "--test-destruction") testDestruction = true;
        else if (a == "--test-debris") testDebris = true;
        else if (a == "--test-gpuskin") testGpuSkin = true;
        else if (a == "--test-meshlet") testMeshlet = true;
        else if (a == "--test-gpucull") testGpuCull = true;
        else if (a == "--cullpath" && i + 1 < argc) cullPathArg = std::atoi(argv[++i]);
        else if (a == "--hzb") hzbArg = 1;
        else if (a == "--test-collapse") testCollapse = true;
        else if (a == "--test-physjoint") testPhysJoint = true;
        else if (a == "--test-nav") testNav = true;
        else if (a == "--test-script") testScript = true;
        else if (a == "--test-weapons") testWeapons = true;
        else if (a == "--test-vehicle") testVehicle = true;
        else if (a == "--test-vehparts") testVehParts = true;
        else if (a == "--test-ecology") testEcology = true;
        else if (a == "--test-crowd") testCrowd = true;
        else if (a == "--test-alert") testAlert = true;
        else if (a == "--screenshot-ecology") {
            ecologyShot = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') ecologyShotPath = argv[++i];
        }
        else if (a == "--screenshot-crowd") {
            crowdShot = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') crowdShotPath = argv[++i];
        }
        else if (a == "--screenshot-alert") {
            alertShot = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') alertShotPath = argv[++i];
        }
        else if (a == "--screenshot-perfshop") {
            perfshopShot = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') perfshopShotDir = argv[++i];
        }
        else if (a == "--set") {
            // Generic CLI cvar override: --set <cvar> <value> (repeatable).
            // Applied right after the console registers its cvars — the headless
            // A/B debugging workhorse (e.g. --set r_rtreflections 0).
            if (i + 2 < argc) { cliCVars.emplace_back(argv[i+1], argv[i+2]); i += 2; }
        }
        else if (a == "--test-footik") testFootIk = true;
        else if (a == "--test-ui") testUi = true;
        else if (a == "--test-loading") testLoading = true;
        else if (a == "--test-saveload") testSaveLoad = true;
        else if (a == "--test-dialog") testDialog = true;
        // (chain break #2 — see the note above)
        if (false) {}
        else if (a == "--demo-dialog") {
            demoDialog = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') demoDialogPath = argv[++i];
        }
        else if (a == "--test-valley") testValley = true;
        else if (a == "--test-cliffs") testCliffs = true;
        else if (a == "--test-club") testClub = true;
        else if (a == "--width") {
            if (i + 1 < argc) { winW = (uint32_t)std::strtoul(argv[++i], nullptr, 10); }
        }
        else if (a == "--height") {
            if (i + 1 < argc) { winH = (uint32_t)std::strtoul(argv[++i], nullptr, 10); }
        }
        else if (a == "--world") {
            if (i + 1 < argc && argv[i + 1][0] != '-') { worldMode = argv[++i]; worldExplicit = true; }
            // `--world fromdoc <path.json>`: an optional second positional token is
            // the LevelDoc to boot (default = the editor's File>Save target).
            if (worldMode == "fromdoc" && i + 1 < argc && argv[i + 1][0] != '-')
                docWorldPath = argv[++i];
        }
        else if (a == "--stress") {
            if (i + 1 < argc) { stressCount = (uint32_t)std::strtoul(argv[++i], nullptr, 10); }
        }
        else if (a == "--bench") {
            bench = true;
            if (i + 1 < argc) { stressCount = (uint32_t)std::strtoul(argv[++i], nullptr, 10); }
            // Optional second positional arg = frame count.
            if (i + 1 < argc && argv[i + 1][0] != '-')
                benchFrames = (uint32_t)std::strtoul(argv[++i], nullptr, 10);
        }
        else if (a == "--editor") editorMode = true;
        else if (a == "--screenshot-editor") {
            editorShot = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') editorShotPath = argv[++i];
        }
        else if (a == "--screenshot-loader") {
            loaderShot = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') loaderShotPath = argv[++i];
        }
        else if (a == "--screenshot-dialog") {
            screenshot = true; dialogShot = true;
            screenshotPath = dialogShotPath;
            if (i + 1 < argc && argv[i + 1][0] != '-') { dialogShotPath = argv[++i]; screenshotPath = dialogShotPath; }
        }
        else if (a == "--screenshot") {
            screenshot = true;
            // Optional path arg (next token, if it isn't another flag).
            if (i + 1 < argc && argv[i + 1][0] != '-') screenshotPath = argv[++i];
            // Optional settle-frame count (second positional, if numeric).
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9')
                screenshotSettle = (int)std::strtol(argv[++i], nullptr, 10);
        }
        else if (a == "--shot-cam") {
            // Parse "x,y,z,yaw,pitch" into shotCam[]; enables the override.
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                const char* s = argv[++i];
                int n = 0; char* end = nullptr;
                while (n < 5 && *s) {
                    shotCam[n++] = std::strtof(s, &end);
                    s = (end && *end == ',') ? end + 1 : end;
                    if (!end || (*end != ',' && *end != '\0')) break;
                }
                shotCamOverride = (n == 5);
            }
        }
        else if (a == "--ui-demo" || a == "--screenshot-menu") {
            uiDemo = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') uiDemoPath = argv[++i];
            // Optional screen keyword: main | pause | settings.
            if (i + 1 < argc && argv[i + 1][0] != '-') uiDemoScreen = argv[++i];
        }
        else if (a == "--fx-demo") fxDemo = true;
        else if (a == "--screenshot-sky") {
            skyShot = true;
            // Optional output path arg (next token, if it isn't another flag).
            if (i + 1 < argc && argv[i + 1][0] != '-') skyShotPath = argv[++i];
        }
        else if (a == "--screenshot-showroom") {
            showroomShot = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') showroomShotPath = argv[++i];
        }
        else if (a == "--screenshot-car") {
            carShot = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') carShotDir = argv[++i];
        }
        else if (a == "--screenshot-showroom-fp") {
            // Headless first-person proof of the walkable --world showroom. Forces the
            // showroom world on so the SAME build path runs, then renders one frame from
            // the player spawn eye and exits.
            showroomFpShot = true;
            worldMode = "showroom";
            if (i + 1 < argc && argv[i + 1][0] != '-') showroomFpShotPath = argv[++i];
        }
        else if (a == "--screenshot-showroom-ragdoll") {
            // Headless proof of Aria's physics RAGDOLL: same showroom-FP setup, but
            // collapse her + step the world so she falls, then capture one frame.
            showroomRagdollShot = true;
            worldMode = "showroom";
            if (i + 1 < argc && argv[i + 1][0] != '-') showroomRagdollShotPath = argv[++i];
        }
        else if (a == "--screenshot-showroom-deck") {
            // Headless proof: stand on the spire-top glass deck, look out at the night sky.
            showroomDeckShot = true;
            worldMode = "showroom";
            if (i + 1 < argc && argv[i + 1][0] != '-') showroomDeckShotPath = argv[++i];
        }
        else if (a == "--screenshot-showroom-elevator") {
            // Headless proof: glass elevator car parked mid-shaft, camera inside it.
            showroomElevShot = true;
            worldMode = "showroom";
            if (i + 1 < argc && argv[i + 1][0] != '-') showroomElevShotPath = argv[++i];
        }
        else if (a == "--screenshot-showroom-stair") {
            // Headless proof: the entry passage + 90 deg turn + the stairs up to the atrium.
            showroomStairShot = true;
            worldMode = "showroom";
            if (i + 1 < argc && argv[i + 1][0] != '-') showroomStairShotPath = argv[++i];
        }
        else if (a == "--screenshot-showroom-floor2") {
            // Headless proof: standing on the 2nd floor having climbed the synthesized stair.
            showroomFloor2Shot = true;
            worldMode = "showroom";
            if (i + 1 < argc && argv[i + 1][0] != '-') showroomFloor2ShotPath = argv[++i];
        }
        else if (a == "--screenshot-showroom-door") {
            // Headless proof: the hidden STRUT-FACE door (open by default; closed via env).
            showroomDoorShot = true;
            worldMode = "showroom";
            if (i + 1 < argc && argv[i + 1][0] != '-') showroomDoorShotPath = argv[++i];
        }
        else if (a == "--screenshot-showroom-struts") {
            // Headless EXTERIOR proof: frame the symmetric set of thickened "/" struts.
            showroomStrutsShot = true;
            worldMode = "showroom";
            if (i + 1 < argc && argv[i + 1][0] != '-') showroomStrutsShotPath = argv[++i];
        }
        else if (a == "--screenshot-showroom-gallery") {
            // Headless proof of the HIDDEN ANALYST GALLERY: captures the gallery (terminals
            // + analyst figures) AND a down-through-the-dark-glass view in one run (and an
            // up-from-the-civilian-floor view under X3_SHOWROOM_GALLERY_UP=1).
            showroomGalleryShot = true;
            worldMode = "showroom";
            if (i + 1 < argc && argv[i + 1][0] != '-') showroomGalleryShotPath = argv[++i];
        }
        else if (a == "--screenshot-showroom-civilians") {
            // Headless DAY proof of the CIVILIAN crowd on the ground + 2nd floors:
            // captures a wide ground-floor view (<path>) + a mezzanine view (<path>_mezz.png).
            showroomCivShot = true;
            worldMode = "showroom";
            if (i + 1 < argc && argv[i + 1][0] != '-') showroomCivShotPath = argv[++i];
        }
        else if (a == "--screenshot-planet") {
            planetShot = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') planetShotPath = argv[++i];
        }
        else if (a == "--screenshot-nightsky") {
            nightskyShot = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') nightskyShotPath = argv[++i];
        }
        // ---- x3.cutscene/1 args: a FRESH if-chain (not chained onto the giant
        // else-if ladder above — MSVC C1061 nesting limit). Disjoint exact matches,
        // so re-starting the chain is behavior-identical.
        if (a == "--test-cutscene") testCutscene = true;
        else if (a == "--skipintro") skipIntro = true;
        else if (a == "--cutscene") {
            if (i + 1 < argc && argv[i + 1][0] != '-') cutsceneFile = argv[++i];
        }
        else if (a == "--cuetime") {
            if (i + 1 < argc) cueTime = (float)std::strtod(argv[++i], nullptr);
        }
        else if (a == "--cutscene-shot") {
            cutsceneShot = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') cutsceneShotPath = argv[++i];
        }
        else if (a == "--screenshot-terrain") {
            terrainShot = true;
            // Optional output path arg (next token, if it isn't another flag).
            if (i + 1 < argc && argv[i + 1][0] != '-') terrainShotPath = argv[++i];
        }
        else if (a == "--screenshot-ocean") {
            oceanShot = true;
            // Optional output path arg (next token, if it isn't another flag).
            if (i + 1 < argc && argv[i + 1][0] != '-') oceanShotPath = argv[++i];
        }
        else if (a == "--screenshot-destruct") {
            destructShot = true;
            // Optional output path arg (next token, if it isn't another flag).
            if (i + 1 < argc && argv[i + 1][0] != '-') destructShotPath = argv[++i];
        }
        else if (a == "--capture-ai") {
            captureAi = true;
            // Optional output directory arg (next token, if it isn't another flag).
            if (i + 1 < argc && argv[i + 1][0] != '-') captureAiDir = argv[++i];
        }
        else if (a == "--capture-walk") {
            captureWalk = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') captureWalkPath = argv[++i];
        }
        else if (a == "--screenshot-footik") {
            captureFootIk = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') captureFootIkPath = argv[++i];
        }
        else if (a == "--capture-spire") {
            captureSpire = true;
            // Optional output directory arg (next token, if it isn't another flag).
            if (i + 1 < argc && argv[i + 1][0] != '-') captureSpireDir = argv[++i];
        }
        else if (a == "--list-clips") {
            listClips = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') listClipsPath = argv[++i];
        }
        else if (a == "--test-locomotion") {
            testLocomotion = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') testLocomotionPath = argv[++i];
        }
        else if (a == "--test-intro") testIntro = true;
    }

    // Fleet asset-store manifest check (Phase A, docs/ASSET_DISTRIBUTION.md):
    // auto-fetch any manifest asset missing locally (D: cache -> G: share), or
    // log ONE line telling the dev to run `python tools/asset_store.py fetch
    // --all`. No-op when assets/manifest.json is absent. Never blocks boot.
    x3::game::checkAssetManifest();

    // ---- HEADLESS --test-*/--demo-*/--list-clips DISPATCH ----------------------
    // The long `if (testX) return runX()?0:1;` ladder was moved VERBATIM to
    // app/test_registry.cpp (#28 monolith split). Populate the flag struct from
    // the locals above (1:1 names) and dispatch; a non-negative result is this
    // program's exit code, -1 means "no test flag set" so boot continues.
    {
        x3::apphost::TestFlags _tf;
        _tf.testJobs = testJobs;
        _tf.testAsset = testAsset;
        _tf.testConsole = testConsole;
        _tf.testPhysics = testPhysics;
        _tf.testPhysJoint = testPhysJoint;
        _tf.testRagdoll = testRagdoll;
        _tf.testGltf = testGltf;
        _tf.testPlayer = testPlayer;
        _tf.testInteract = testInteract;
        _tf.testPhysprops = testPhysprops;
        _tf.testRagdollSkin = testRagdollSkin;
        _tf.testEditor = testEditor;
        _tf.testBlockout = testBlockout;
        _tf.testLoader = testLoader;
        _tf.testBarrels = testBarrels;
        _tf.testGlass = testGlass;
        _tf.testFrustumCull = testFrustumCull;
        _tf.testHoloterm = testHoloterm;
        _tf.testSecretRoom = testSecretRoom;
        _tf.testHatch = testHatch;
        _tf.testLlm = testLlm;
        _tf.testEcs = testEcs;
        _tf.testEcsRender = testEcsRender;
        _tf.testPickup = testPickup;
        _tf.testCombat = testCombat;
        _tf.testDeathRagdoll = testDeathRagdoll;
        _tf.testAudio = testAudio;
        _tf.testAcoustics = testAcoustics;
        _tf.testLevel1 = testLevel1;
        _tf.testCanonLevel = testCanonLevel;
        _tf.testCanonPlay = testCanonPlay;
        _tf.testIntro = testIntro;
        _tf.testCutscene = testCutscene;
        _tf.testPhase2a = testPhase2a;
        _tf.testPhase2b = testPhase2b;
        _tf.testAnim = testAnim;
        _tf.testLocomotion = testLocomotion;
        _tf.listClips = listClips;
        _tf.testTerrain = testTerrain;
        _tf.testTerrainPlace = testTerrainPlace;
        _tf.testStreaming = testStreaming;
        _tf.testWorldStream = testWorldStream;
        _tf.testWorldMap = testWorldMap;
        _tf.testAi = testAi;
        _tf.testBestiary = testBestiary;
        _tf.testBosses = testBosses;
        _tf.testAct2Bosses = testAct2Bosses;
        _tf.testSpireMid = testSpireMid;
        _tf.testNexus = testNexus;
        _tf.testSpireTop = testSpireTop;
        _tf.testTimeline = testTimeline;
        _tf.testDroneHack = testDroneHack;
        _tf.testSubLevels = testSubLevels;
        _tf.testTod = testTod;
        _tf.testWeather = testWeather;
        _tf.testAct2 = testAct2;
        _tf.testAct2Desert = testAct2Desert;
        _tf.testAct2Caves = testAct2Caves;
        _tf.testWorldRegions = testWorldRegions;
        _tf.testCity = testCity;
        _tf.testOceanBase = testOceanBase;
        _tf.testDoorCode = testDoorCode;
        _tf.testHatchCode = testHatchCode;
        _tf.testElevator = testElevator;
        _tf.testElevatorFsm = testElevatorFsm;
        _tf.testNet = testNet;
        _tf.testNetSync = testNetSync;
        _tf.testNetInterp = testNetInterp;
        _tf.testNetPredict = testNetPredict;
        _tf.testRescue = testRescue;
        _tf.testThirdPerson = testThirdPerson;
        _tf.testNpcTalk = testNpcTalk;
        _tf.testChatTree = testChatTree;
        _tf.testMission = testMission;
        _tf.testDestruction = testDestruction;
        _tf.testDebris = testDebris;
        _tf.testGpuSkin = testGpuSkin;
        _tf.testMeshlet = testMeshlet;
        _tf.testGpuCull = testGpuCull;
        _tf.testCollapse = testCollapse;
        _tf.testNav = testNav;
        _tf.testWeapons = testWeapons;
        _tf.testScript = testScript;
        _tf.testVehicle = testVehicle;
        _tf.testVehParts = testVehParts;
        _tf.testEcology = testEcology;
        _tf.testCrowd = testCrowd;
        _tf.testAlert = testAlert;
        _tf.testFootIk = testFootIk;
        _tf.testUi = testUi;
        _tf.testLoading = testLoading;
        _tf.testSaveLoad = testSaveLoad;
        _tf.testDialog = testDialog;
        _tf.demoDialog = demoDialog;
        _tf.testValley = testValley;
        _tf.testCliffs = testCliffs;
        _tf.testClub = testClub;
        _tf.testLocomotionPath = testLocomotionPath;
        _tf.listClipsPath = listClipsPath;
        _tf.demoDialogPath = demoDialogPath;
        int _rc = x3::apphost::dispatchTests(_tf);
        if (_rc >= 0) return _rc;
    }

    x3::logInfo("X3Engine starting...");
    x3::boot::mark("static init + args");

    // --test-boottime gates the CANONICAL world (canonlevel — the data-driven
    // Floor 1, the game's true level) unless a --world was given explicitly. The
    // legacy hand-coded tower (--world level1) builds 5x the entity count and has
    // an honest boot floor of ~3.2 s — gate it explicitly with a budget arg, e.g.
    // `--test-boottime 4000 --world level1` (see docs/BOOT_TIME.md).
    if (testBootTime && !worldExplicit) {
        worldMode = "canonlevel";
        x3::logInfo("boottime: no --world given — gating the canonical world (canonlevel)");
    }

    // ---- BOOT MANIFEST (docs/BOOT_TIME.md): everything the cell worlds (default
    // Level 1 / elevator / canonlevel / intro) load at build time. Built once,
    // used twice: (1) prewarmModelDecodesAsync RIGHT NOW — pure-CPU stb decodes on
    // background threads, fully overlapped with the ~1 s Vulkan driver init below;
    // (2) preloadModelsAsync after the device exists — parallel full loads that
    // consume those decodes and warm the model/texture caches, so the serial world
    // build takes cache hits. Missing files are skipped silently (superset).
    std::vector<std::pair<std::string, std::string>> bootManifest;
    {
        const bool legacyCell = (worldMode == "level1") || (worldMode == "elevator");
        const bool canonCell  = (worldMode == "canonlevel") || (worldMode == "intro");
        if (legacyCell || canonCell) {
            const std::string rig = x3::game::riggedGlbRoot();
            const std::string cvt = x3::game::convertedGlbRoot();
            // Common to BOTH cell builds (canon Floor 1 + the legacy tower).
            for (const char* f : { "marcus_webb_anim.glb", "alien_crawler_anim.glb",
                                   "chief_martinez.glb",
                                   "AnnaCasual.glb", "AnnaBodySuit.glb", "AnnaTactical.glb",
                                   "Jake_22_actions.glb",
                                   "WeaponEnergyPistol.glb", "WeaponEnergyPistol2.glb",
                                   "WeaponRailgun.glb", "WeaponShotgun2.glb",
                                   "WeaponBFG.glb", "WeaponRocketLauncher.glb" })
                bootManifest.emplace_back(rig, f);
            bootManifest.emplace_back(cvt, "Characters/Drone.glb");
            bootManifest.emplace_back(cvt, "ModularSciFi_Interior/SM_Door_A.glb");
            // Legacy tower only (Spire floors + env art + warehouse props).
            if (legacyCell) {
                for (const char* f : { "chief_martinez_anim.glb", "Oracle.glb" })
                    bootManifest.emplace_back(rig, f);
                for (const char* f : { "ModularSciFi_Interior/SM_DoorFrame_A.glb",
                                       "ModularSciFi_Interior/SM_Wall_A.glb",
                                       "ModularSciFi_Interior/SM_Floor_A.glb",
                                       "ModularSciFi_Interior/SM_Ceiling_A.glb",
                                       "ModularSciFi_Interior/SM_Light_A.glb",
                                       "ModularSciFi_Interior/SM_Pipes_A.glb",
                                       "ModularSciFi_Interior/SM_Console.glb",
                                       "SciFi_Warehouse_Kit/Barrel.glb",
                                       "SciFi_Warehouse_Kit/Crate Long.glb",
                                       "SciFi_Warehouse_Kit/Crate Short.glb",
                                       "SciFi_Warehouse_Kit/Fusebox 01.glb",
                                       "SciFi_Warehouse_Kit/Pallet.glb" })
                    bootManifest.emplace_back(cvt, f);
            }
            x3::asset::prewarmModelDecodesAsync(bootManifest);
            x3::boot::mark("decode prewarm kicked (async)");
        }
    }

    // HEADLESS / OFFSCREEN routing: the non-interactive verification + screenshot
    // paths (--smoketest, --screenshot, --screenshot-sky, --screenshot-terrain)
    // render fully offscreen — NO GLFW window, NO surface, NO swapchain, nothing
    // shown on screen. Everything a human actually watches (no-arg game,
    // --world terrain, --bench) keeps a real window + swapchain exactly as before.
    if (perfshopShot) worldMode = "drive";   // the shop lives in the drive world
    if (ecologyShot)  worldMode = "valley";  // the ambient ecology rides the valley biome
    if (crowdShot)    worldMode = "club";    // the crowd proof lives on the club floor
    if (alertShot) { screenshot = true; screenshotPath = alertShotPath; }   // rides --screenshot
    const bool headless = smoketest || testFramePacing || screenshot || skyShot || ddgiShot || showroomShot || carShot || showroomFpShot || showroomRagdollShot || showroomDeckShot || showroomElevShot || showroomStairShot || showroomFloor2Shot || showroomDoorShot || showroomStrutsShot || showroomGalleryShot || showroomCivShot || planetShot || nightskyShot || cutsceneShot || terrainShot || oceanShot || captureAi || captureWalk || destructShot || captureFootIk || uiDemo || captureSpire || editorShot || loaderShot || perfshopShot || ecologyShot || crowdShot;

    if (!glfwInit()) {
        x3::logError("glfwInit failed");
        return 1;
    }
    // No OpenGL/GLES context — we drive Vulkan ourselves.
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

    // Headless capture/screenshot resolution is FIXED at 1280x720 (so offscreen
    // output stays byte-stable regardless of any --width/--height flags). The
    // visible window uses the configurable winW/winH (default 1280x720), guarded
    // to a sane minimum so a typo can't create a 0-size surface.
    constexpr uint32_t kHeadlessW = 1280, kHeadlessH = 720;
    if (winW < 320)  winW = 320;
    if (winH < 240)  winH = 240;
    const uint32_t W = headless ? kHeadlessW : winW;
    const uint32_t H = headless ? kHeadlessH : winH;
    // In headless mode we create NO window (no glfwCreateWindow at all). GLFW is
    // still initialized (cheap; some paths poll events) but never opens a surface.
    GLFWwindow* window = nullptr;
    if (!headless) {
        // NO maximize-by-default (per Tim): open windowed at winW x H (or the saved
        // "SET AS DEFAULT" size). Fullscreen is opt-in via the settings checkbox.
        window = glfwCreateWindow(static_cast<int>(W), static_cast<int>(H),
                                  "X3Engine", nullptr, nullptr);
        if (!window) {
            x3::logError("glfwCreateWindow failed");
            glfwTerminate();
            return 1;
        }
        x3::logInfo("window: " + std::to_string(W) + "x" + std::to_string(H));
    } else {
        x3::logInfo("headless mode: rendering offscreen (no window / no swapchain) at "
                    + std::to_string(W) + "x" + std::to_string(H));
    }
    x3::boot::mark("glfw init + window");

    // BOOT-TIME: overlap the audio bring-up (miniaudio device + the WAV loads,
    // ~80-100 ms) with the Vulkan device init below. Launched ONLY for the
    // windowed worlds that actually reach the shared audio block (the headless
    // capture paths and the self-contained demo worlds early-return before it and
    // must not leak a live audio device). Joined at the original audio spot.
    struct BootAudio {
        std::unique_ptr<x3::audio::IAudioSystem> audio;
        x3::audio::SoundHandle gun, door, pickup, death;
    };
    auto makeBootAudio = []() -> BootAudio {
        BootAudio ba;
        ba.audio.reset(x3::audio::createAudioSystem());
        ba.audio->init();
        ba.gun = ba.audio->load(x3::game::resolveAudio(
            "Sci-Fi_Guns_Game-Of-Weapons/Audio/SFX/Wave/Single_Gunshots/"
            "Single_Gunshot_Sci-Fi_Gun-01.wav"));
        ba.door = ba.audio->load(x3::game::resolveAudio(
            "ModularScifiInterior/Sound/S_ScifiDoor_A.WAV"));
        ba.pickup = ba.audio->load(x3::game::resolveAudio(
            "Sci-fi Evolution Gift Pack/Health or Energy Game Recharge 2.wav"));
        ba.death = ba.audio->load(x3::game::resolveAudio(
            "Free Pack/Explosion 1.wav"));
        return ba;
    };
    std::future<BootAudio> bootAudioFut;
    {
        const bool sharedAudioWorld =
            (worldMode == "level1") || (worldMode == "elevator") ||
            (worldMode == "canonlevel") || (worldMode == "intro") ||
            (worldMode == "terrain") || (worldMode == "ocean");
        if (!headless && sharedAudioWorld)
            bootAudioFut = std::async(std::launch::async, makeBootAudio);
    }

    // ---- Render device ----
    std::unique_ptr<x3::rhi::IRenderDevice> device(x3::rhi::createRenderDevice());

    x3::rhi::DeviceDesc desc{};
    // BOOT-TIME: kick the async GLB warmup the moment the device's upload path is
    // live (mid-init, right after the core graphics objects) so it overlaps the
    // remaining ~300 ms of device init. Joined before the world build.
    struct PreloadCtx {
        x3::rhi::IRenderDevice* dev;
        const std::vector<std::pair<std::string, std::string>>* manifest;
    } preloadCtx{ device.get(), &bootManifest };
    if (!bootManifest.empty()) {
        desc.onUploadReady = [](void* u) {
            auto* c = static_cast<PreloadCtx*>(u);
            // Open the upload-batch window NOW so the preload threads record into
            // the shared batch (the later world-build beginUploadBatch is a no-op).
            c->dev->beginUploadBatch();
            x3::asset::preloadModelsAsync(c->dev, *c->manifest);
            x3::logInfo("[boot] GLB preload kicked (mid device-init, async)");
        };
        desc.onUploadReadyUser = &preloadCtx;
    }
    desc.nativeWindowHandle = window ? glfwGetWin32Window(window) : nullptr;
    desc.width  = W;
    desc.height = H;
    desc.headless = headless;
    desc.ssaa = (showroomShot || carShot || showroomFpShot || showroomRagdollShot || showroomDeckShot || showroomElevShot || showroomStairShot || showroomFloor2Shot || showroomDoorShot || showroomStrutsShot || showroomGalleryShot || showroomCivShot || planetShot || nightskyShot || cutsceneShot) ? 4u : 1u;   // 4x supersample the showroom / planet / nightsky still (5090 headless: ~16 samples/px, pristine)
    // Benchmark mode runs with vsync OFF so it measures the true frame ceiling,
    // not the display refresh cap.
    desc.vsync  = !bench;
#ifdef _DEBUG
    desc.validation = true;
#else
    desc.validation = false;
#endif

    if (!device->init(desc)) {
        x3::logError("render device init failed");
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    // --legacypost: A/B switch — disable the post-stack additions (auto-exposure +
    // TAA; and with --legacypost2 also bloom + ACES->passthrough) so any path,
    // headless screenshots included, can be compared against the pre-post-stack
    // renderer. --notaa: disable ONLY TAA (jitter fully off + resolve skipped) so
    // before/after captures isolate the TAA contribution. Both also pin the cvars
    // so the interactive per-frame cvar sync doesn't re-enable the feature.
    if (legacyPost || noTaa) {
        x3::rhi::IRenderDevice::PostFXParams px{};
        if (legacyPost) {
            px.autoExposure = false;             // legacy = no eye adaptation
            px.taa = false;                      // legacy = no TAA jitter/resolve
            if (legacyPost > 1) { px.bloomEnabled = false; px.tonemapMode = 0; }
        }
        if (noTaa) px.taa = false;
        device->setPostFX(px);
        // Reflections ride the TAA history, so TAA-off already disables them in
        // the device; push an explicit OFF too so the A/B state is unambiguous.
        device->setReflectionParams(x3::rhi::IRenderDevice::ReflectionParams{});
        // (The interactive path additionally pins the matching cvars right after
        // the console exists, so the per-frame cvar sync can't re-enable these.)
    }
    // --norefl: A/B switch — reflections OFF with TAA (and everything else) left
    // at defaults, so before/after captures isolate exactly the SSR/RT-reflection
    // contribution (the post stack + TAA stay identical).
    if (noRefl) device->setReflectionParams(x3::rhi::IRenderDevice::ReflectionParams{});

    // --ddgi: A/B switch the other way — force DDGI probe-grid GI ON for any
    // path (headless showroom/level screenshots included) so before/after
    // captures isolate exactly the DDGI ambient-diffuse contribution. No-op on
    // hardware without ray query + position fetch (the device tier gate).
    if (ddgiForce) {
        x3::rhi::IRenderDevice::DdgiParams dp{};
        dp.enabled = true;
        device->setDdgiParams(dp);
        x3::logInfo(std::string("--ddgi: DDGI requested; device rayTracingSupported=") +
                    (device->rayTracingSupported() ? "YES" : "NO"));
    }

    // ---- Level Architect EDITOR overlay init (--editor / --screenshot-editor) ----
    // ImGui initializes ONLY here, ONLY when --editor (windowed) or --screenshot-editor
    // (headless proof) is set. Without either flag initEditorUI is never called and the
    // device allocates nothing for ImGui (the shipping game path is byte-for-byte
    // unchanged). The windowed --editor begin/end wrap lands in the interactive loop.
    if (editorMode && window) {
        device->initEditorUI(window);
        x3::logInfo(device->editorUIActive()
            ? "--editor: Dear ImGui (docking) editor overlay ACTIVE"
            : "--editor: editor overlay FAILED to init");
    }

    // ---- Headless editor PROOF (--screenshot-editor [path.png]) ------------------
    // Inits ImGui in the headless device (a hidden GLFW window backs the GLFW backend;
    // rendering goes into the offscreen color image), renders ONE frame with the
    // dockspace + demo window, and captures a PNG so the Phase-0 integration can be
    // verified without a display. Offscreen + one-shot, like --screenshot.
    if (editorShot) {
        x3::logInfo("--screenshot-editor: ImGui Phase-0 proof -> " + editorShotPath);
        // Ensure the output directory exists (build/proof/).
        {
            std::error_code ec;
            std::filesystem::path outp(editorShotPath);
            if (outp.has_parent_path())
                std::filesystem::create_directories(outp.parent_path(), ec);
        }
        // A HIDDEN GLFW window backs ImGui's GLFW backend (no surface is used — the
        // device is headless and renders into its offscreen target).
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        GLFWwindow* proofWin = glfwCreateWindow(static_cast<int>(W), static_cast<int>(H),
                                                "X3Engine-editor-proof", nullptr, nullptr);
        glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);  // restore the default for any later window
        if (!proofWin) {
            x3::logError("--screenshot-editor: hidden GLFW window create failed");
            device->shutdown();
            glfwTerminate();
            return 1;
        }
        device->initEditorUI(proofWin);
        if (!device->editorUIActive()) {
            x3::logError("--screenshot-editor: ImGui init failed");
            glfwDestroyWindow(proofWin);
            device->shutdown();
            glfwTerminate();
            return 1;
        }
        // Phase 1/2 PROOF: stand up a real EditorHost over a minimal Scene + physics,
        // place a couple of blockout BOXES + a RAMP (clean grid material), pose the
        // camera at them, and render one frame -> PNG showing the DOCKSPACE + the
        // editor panels + the grid-material brushes in the viewport.
        x3::game::Scene proofScene;
        x3::phys::IPhysicsWorld* proofPhys = x3::phys::createPhysicsWorld();
        proofPhys->init();
        x3::editor::EditorHost proofHost;
        proofHost.init(*device, proofScene, *proofPhys, proofWin);
        {
            float p0[3] = { 0.0f, 0.0f, 0.0f }, s0[3] = { 8.0f, 0.5f, 8.0f };   // floor plate
            float p1[3] = { -2.0f, 1.0f, 1.0f }, s1[3] = { 2.0f, 2.0f, 2.0f };  // a box
            float p2[3] = { 3.0f, 1.0f, 1.0f }, s2[3] = { 3.0f, 2.0f, 4.0f };   // a ramp
            proofHost.placeBrush(0u, p0, s0, *device, proofScene, *proofPhys);
            proofHost.placeBrush(0u, p1, s1, *device, proofScene, *proofPhys);
            proofHost.placeBrush(1u, p2, s2, *device, proofScene, *proofPhys);
            // Feature 3 proof: place a GLB prop (renders via renderModels each frame).
            proofHost.placeModel("SciFi_Warehouse_Kit/Barrel.glb", *device);
        }
        // A pleasant 3/4 vantage on the brushes; a touch of ambient so the grey reads.
        device->setCamera(8.0f, 6.5f, 11.0f, -2.35f, -0.45f, 60.0f);
        device->setAmbient(0.55f, 0.56f, 0.58f);
        // Render a few settle frames (font upload + draw-data), capturing the last one.
        bool ok = false;
        const int kProofFrames = 4;
        for (int f = 0; f < kProofFrames; ++f) {
            const bool lastFrame = (f == kProofFrames - 1);
            if (lastFrame) device->armCapture(editorShotPath.c_str());
            glfwPollEvents();
            auto frame = device->beginFrame();
            if (frame.valid) {
                proofScene.render(*device, frame);          // the grid-material brushes
                proofHost.renderModels(*device, frame);     // Feature 3 GLB props
                device->beginEditorUI();                    // dockspace root (device)
                proofHost.draw(*device, proofScene, *proofPhys, 1.0f/60.0f);  // panels (host)
                device->endEditorUI();                      // ImGui::Render + stash draw data
                device->endFrame(frame);
            }
            if (lastFrame) ok = device->captureFrame(editorShotPath.c_str());
        }
        device->shutdownEditorUI();
        proofPhys->shutdown();
        delete proofPhys;
        glfwDestroyWindow(proofWin);
        device->shutdown();
        glfwTerminate();
        x3::logInfo(ok ? "--screenshot-editor: wrote " + editorShotPath
                       : "--screenshot-editor: capture FAILED");
        return ok ? 0 : 1;
    }

    // ---- Headless LOADER proof (--screenshot-loader [path.png]) ----------------
    // The data-driven LevelDoc loader's render proof: author the sample LevelDoc,
    // SAVE it to disk, LOAD it back through the REAL loader (file -> parse -> brushes
    // + materials + props + lights + trigger -> live device meshes + Jolt bodies),
    // pose the camera inside the built room, capture a PNG, tear everything down.
    // Offscreen + one-shot, like --screenshot.
    if (loaderShot) {
        x3::logInfo("--screenshot-loader: LevelDoc loader proof -> " + loaderShotPath);
        {
            std::error_code ec;
            std::filesystem::path outp(loaderShotPath);
            if (outp.has_parent_path())
                std::filesystem::create_directories(outp.parent_path(), ec);
            std::filesystem::create_directories("build/proof", ec);
        }
        // Author + save the sample doc, then load it through the real file path so
        // the proof exercises the EXACT pipeline `--world fromdoc` boots.
        const char* kDocPath = "build/proof/loader_sample_room.json";
        x3::editor::LevelDoc sample = x3::game::makeSampleLevelDoc();
        if (!sample.saveJson(kDocPath)) {
            x3::logError("--screenshot-loader: could not write " + std::string(kDocPath));
            device->shutdown(); glfwTerminate(); return 1;
        }
        x3::game::Scene proofScene;
        x3::phys::IPhysicsWorld* proofPhys = x3::phys::createPhysicsWorld();
        proofPhys->init();
        x3::game::LevelDocWorld proofDoc;
        bool built = proofDoc.loadFromFile(kDocPath, proofScene, *device, *proofPhys);
        bool ok = false;
        if (built) {
            // A 3/4 vantage inside the room: ramp + ledge + hazard pillar in frame.
            float ps[3]; proofDoc.playerStart(ps);
            device->setCamera(ps[0] + 4.0f, ps[1] + 2.6f, ps[2] + 1.5f, -2.05f, -0.30f, 65.0f);
            device->setAmbient(0.42f, 0.43f, 0.47f);
            std::vector<x3::rhi::PointLight> pls;
            proofDoc.selectLights(ps[0], ps[1], ps[2], pls, 16);
            device->setPointLights(pls.data(), (uint32_t)pls.size());
            const int kFrames = 8;   // settle so shadows/SSAO register
            for (int f = 0; f < kFrames; ++f) {
                const bool last = (f == kFrames - 1);
                if (last) device->armCapture(loaderShotPath.c_str());
                glfwPollEvents();
                auto frame = device->beginFrame();
                if (frame.valid) {
                    proofScene.render(*device, frame);
                    device->endFrame(frame);
                }
                if (last) ok = device->captureFrame(loaderShotPath.c_str());
            }
        } else {
            x3::logError("--screenshot-loader: loader BUILD failed");
        }
        proofDoc.shutdown(proofScene, *device, *proofPhys);
        proofPhys->shutdown();
        delete proofPhys;
        device->shutdown();
        glfwTerminate();
        x3::logInfo(ok ? "--screenshot-loader: wrote " + loaderShotPath
                       : "--screenshot-loader: capture FAILED");
        return ok ? 0 : 1;
    }

    // ---- Sky vantage mode (--screenshot-sky [path.png]) --------------------
    // The open-world sky's first verification gate. EFLZ Level 1 is an enclosed
    // interior, so the sky never shows there; this builds a MINIMAL outdoor scene
    // (a large checkered ground plane + a couple of boxes for ground-shadow proof)
    // entirely through the public render API — no game/physics/audio stack — turns
    // ON the analytic sky with the engine's existing sun direction + color, poses
    // the camera at the horizon looking slightly up toward the sun, settles a few
    // frames so the shadow map + sky register, captures a PNG, and exits.
    if (skyShot) {
        x3::logInfo("--screenshot-sky: rendering outdoor sky vantage to " + skyShotPath);

        // Ground plane (large XZ quad) with a tiled checker so the horizon + ground
        // shadows read clearly. A neutral mid-grey/green checker reads as terrain.
        std::vector<x3::rhi::MeshVertex> gv; std::vector<uint32_t> gi;
        x3::prims::makeGroundQuad(/*half=*/400.0f, /*tiles=*/200.0f, gv, gi);
        x3::rhi::MeshHandle ground = device->createMesh(gv.data(), (uint32_t)gv.size(),
                                                        gi.data(), (uint32_t)gi.size());
        auto checker = x3::prims::makeCheckerRGBA(64, 8, 150, 165, 150, 70, 90, 75);
        x3::rhi::TextureHandle groundTex = device->createTexture(checker.data(), 64, 64, /*srgb=*/true);

        // A few boxes sitting on the ground so the sun casts visible shadows (proves
        // the sky's sun direction matches the lighting/shadow pass).
        x3::prims::PrimMesh boxM = x3::prims::makeBox(1.5f, 1.5f, 1.5f, 0, 1.5f, 0, 0.5f);
        x3::rhi::MeshHandle box = device->createMesh(boxM.verts.data(), (uint32_t)boxM.verts.size(),
                                                     boxM.index.data(), (uint32_t)boxM.index.size());
        auto boxPx = x3::prims::makeSolidRGBA(4, 200, 200, 205);
        x3::rhi::TextureHandle boxTex = device->createTexture(boxPx.data(), 4, 4, /*srgb=*/true);

        // Turn ON the analytic sky with the SAME sun the shadow pass + mesh.frag use
        // (normalize(0.4,1,0.3)) and a sun color matching mesh.frag's kSunColor, so
        // the disk in the sky sits exactly where the world is lit from.
        x3::rhi::IRenderDevice::SkyParams sp{};
        sp.enabled = true;
        sp.sunDir[0] = 0.4f; sp.sunDir[1] = 1.0f; sp.sunDir[2] = 0.3f;
        sp.sunColor[0] = 1.0f; sp.sunColor[1] = 0.97f; sp.sunColor[2] = 0.92f;
        sp.sunIntensity = 1.0f; sp.haze = 0.5f; sp.exposure = 1.0f;
        device->setSkyParams(sp);

        // Camera: stand at eye height aimed toward the sun's azimuth and pitched UP
        // toward its elevation so the sun disk + glow land in-frame with the
        // gradient above and the horizon + ground shadows at the bottom. The sun
        // dir is normalize(0.4,1,0.3): azimuth = atan2(0.3,0.4) in XZ, elevation =
        // asin(0.898) ~ 64deg. Aim the yaw at the azimuth and pitch partway up to
        // its elevation (a touch lower than the sun so the horizon stays visible).
        const float sunYaw   = std::atan2(0.3f, 0.4f);  // toward the sun in XZ
        const float camPitch = 0.52f;                   // ~30deg up: sun upper frame, horizon at bottom
        device->setCamera(0.0f, 1.7f, 16.0f, sunYaw, camPitch, 72.0f);

        const float modelGround[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        const float modelBoxA[16]   = { 1,0,0,0, 0,1,0,0, 0,0,1,0,  -6.0f, 0.0f,  2.0f, 1 };
        const float modelBoxB[16]   = { 1,0,0,0, 0,1,0,0, 0,0,1,0,   5.0f, 0.0f, -3.0f, 1 };
        const float white[4]  = { 1, 1, 1, 1 };
        const float gtint[4]  = { 1, 1, 1, 1 };

        const int kSettle = 12;
        for (int i = 0; i < kSettle; ++i) {
            glfwPollEvents();
            if (i == kSettle - 1) device->armCapture(skyShotPath.c_str());
            auto frame = device->beginFrame();
            if (frame.valid) {
                device->drawMesh(frame, ground, groundTex, gtint, modelGround);
                device->drawMesh(frame, box, boxTex, white, modelBoxA);
                device->drawMesh(frame, box, boxTex, white, modelBoxB);
            }
            device->endFrame(frame);
        }
        const bool wrote = device->captureFrame(skyShotPath.c_str());
        if (wrote) x3::logInfo("--screenshot-sky: wrote " + skyShotPath);
        else       x3::logError("--screenshot-sky: capture FAILED");

        device->shutdown();
        glfwDestroyWindow(window);
        glfwTerminate();
        return wrote ? 0 : 1;
    }

    // ---- DDGI gate-shot proof (--screenshot-ddgi [outDir]) ------------------
    // THE GATE SHOT for r_ddgi: a sealed two-room rig where room A holds the only
    // light sources (a warm point light + an emissive ceiling panel) and room B
    // connects to it ONLY through a doorway. With r_ddgi 0, B is near-black (flat
    // ambient only); with r_ddgi 1, the probe field carries the bounce through the
    // doorway and B reads warm — honest traced GI, no screen-space dependence.
    // Room C sits SEALED beside A (full walls): the leak canary — Chebyshev
    // visibility weighting must keep it black with DDGI on. Captures:
    //   ddgi_corridor_off/on.png  — camera in room B (the money A/B)
    //   ddgi_leak_off/on.png      — camera in sealed room C (must stay dark)
    //   ddgi_probes_debug.png     — r_ddgi_debug 1 irradiance-field view (room B)
    // Headless, like --screenshot-sky; exits after the captures.
    if (ddgiShot) {
        namespace fs = std::filesystem;
        std::error_code mkec; fs::create_directories(ddgiShotDir, mkec);
        x3::logInfo("--screenshot-ddgi: rendering DDGI gate shots to " + ddgiShotDir);

        // ---- Geometry: floor/ceiling shell + room walls from solid boxes (their
        // outward faces are the room interiors; back-face culling + DDGI backface
        // handling are both happy with closed slabs). Floor top at y=0. ----
        std::vector<x3::prims::PrimMesh> parts;
        parts.push_back(x3::prims::makeBox(9.5f, 0.5f, 8.5f,  0.0f, -0.5f, 3.5f)); // floor slab
        parts.push_back(x3::prims::makeBox(9.5f, 0.5f, 8.5f,  0.0f,  4.5f, 3.5f)); // ceiling slab
        // Walls run y -0.15..4.15 (half 2.15) so they OVERLAP the floor +
        // ceiling slabs — no coplanar seam for the shadow map's PCF bias to
        // leak a sunlit strip through at the junction.
        parts.push_back(x3::prims::makeBox(0.5f, 2.15f, 8.5f, -9.0f,  2.0f, 3.5f)); // west shell
        parts.push_back(x3::prims::makeBox(0.5f, 2.15f, 8.5f,  9.0f,  2.0f, 3.5f)); // east shell
        parts.push_back(x3::prims::makeBox(9.5f, 2.15f, 0.5f,  0.0f,  2.0f, -4.5f)); // south shell
        parts.push_back(x3::prims::makeBox(9.5f, 2.15f, 0.5f,  0.0f,  2.0f, 11.5f)); // north shell
        // A|B divider (x=0) with a 2 m doorway at z in [-1,1], ~3 m tall:
        parts.push_back(x3::prims::makeBox(0.5f, 2.15f, 1.5f,  0.0f,  2.0f, -2.5f)); // divider south seg
        parts.push_back(x3::prims::makeBox(0.5f, 2.15f, 1.5f,  0.0f,  2.0f,  2.5f)); // divider north seg
        parts.push_back(x3::prims::makeBox(0.5f, 0.65f, 1.0f,  0.0f,  3.55f, 0.0f)); // doorway lintel (2.9..4.2)
        // A|C separator (z=4..5, FULL span — room C is sealed; the leak canary):
        parts.push_back(x3::prims::makeBox(9.5f, 2.15f, 0.5f,  0.0f,  2.0f,  4.5f));
        // C | east-void divider (x=0, z 5..11) so C is a closed room:
        parts.push_back(x3::prims::makeBox(0.5f, 2.15f, 3.0f,  0.0f,  2.0f,  8.0f));
        // RED accent wall inside room A (color-bleed proof: B's spill reads warm-red):
        x3::prims::PrimMesh redPanel = x3::prims::makeBox(0.1f, 1.8f, 3.5f, -8.3f, 1.9f, 0.0f);

        std::vector<x3::rhi::MeshHandle> partMesh;
        for (auto& p : parts)
            partMesh.push_back(device->createMesh(p.verts.data(), (uint32_t)p.verts.size(),
                                                  p.index.data(), (uint32_t)p.index.size()));
        x3::rhi::MeshHandle redMesh = device->createMesh(redPanel.verts.data(), (uint32_t)redPanel.verts.size(),
                                                         redPanel.index.data(), (uint32_t)redPanel.index.size());
        // Emissive ceiling panel in room A:
        x3::prims::PrimMesh panel = x3::prims::makeBox(1.5f, 0.05f, 1.5f, -4.5f, 3.9f, 0.0f);
        x3::rhi::MeshHandle panelMesh = device->createMesh(panel.verts.data(), (uint32_t)panel.verts.size(),
                                                           panel.index.data(), (uint32_t)panel.index.size());

        auto greyPx = x3::prims::makeSolidRGBA(4, 200, 200, 200);
        x3::rhi::TextureHandle greyTex = device->createTexture(greyPx.data(), 4, 4, /*srgb=*/true);

        // Lights: room A only. The point light gives A its direct look; the
        // emissive panel is the DYNAMIC GI source the probes must pick up.
        x3::rhi::PointLight pl{};
        pl.pos[0] = -4.5f; pl.pos[1] = 3.2f; pl.pos[2] = 0.0f; pl.range = 10.0f;
        pl.color[0] = 3.0f; pl.color[1] = 2.6f; pl.color[2] = 2.0f;
        device->setPointLights(&pl, 1);
        device->setAmbient(0.015f, 0.016f, 0.020f);          // near-black base ambient
        device->setShadowBounds(0.0f, 2.0f, 3.5f, 25.0f);
        // Sun BELOW the horizon (sky stays disabled): every surface's sun N.L is
        // <= 0, so the only light in the rig is the room-A point light + the
        // emissive panel — the purest possible bounce-only A/B (this also avoids
        // the engine's shadow-bias seam at wall/ceiling junctions muddying the
        // leak canary; that seam is a raster artifact identical OFF and ON).
        {
            x3::rhi::IRenderDevice::SkyParams sp{};
            sp.enabled = false;
            sp.sunDir[0] = 0.0f; sp.sunDir[1] = -1.0f; sp.sunDir[2] = 0.01f;
            device->setSkyParams(sp);
        }

        const float identity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        const float white[4] = { 1, 1, 1, 1 };
        const float red[4]   = { 0.85f, 0.08f, 0.06f, 1.0f };
        const float panelTint[4] = { 1.0f, 0.9f, 0.7f, 1.0f };
        const float panelEmissive[4] = { 1.0f, 0.85f, 0.6f, 10.0f };  // HDR glow (GI source)

        auto drawScene = [&](const x3::rhi::FrameContext& f) {
            for (auto m : partMesh) device->drawMesh(f, m, greyTex, white, identity);
            device->drawMesh(f, redMesh, greyTex, red, identity);
            device->drawMeshEmissive(f, panelMesh, greyTex, panelTint, panelEmissive, identity);
        };
        float gpuMsAccum = 0.0f; int gpuMsN = 0;
        auto renderFrames = [&](int n, const std::string& capturePath) -> bool {
            for (int i = 0; i < n; ++i) {
                glfwPollEvents();
                if (!capturePath.empty() && i == n - 1) device->armCapture(capturePath.c_str());
                auto f = device->beginFrame();
                if (f.valid) drawScene(f);
                device->endFrame(f);
                gpuMsAccum += device->stats().gpuFrameMs; ++gpuMsN;
            }
            if (capturePath.empty()) return true;
            const bool ok = device->captureFrame(capturePath.c_str());
            x3::logInfo(std::string(ok ? "--screenshot-ddgi: wrote " : "--screenshot-ddgi: FAILED ") + capturePath);
            return ok;
        };
        // Camera poses: room B looking through at the doorway wall; sealed room C.
        auto camRoomB = [&]() { device->setCamera(7.2f, 1.7f, 3.0f, std::atan2(-3.0f, -7.2f), -0.03f, 72.0f); };
        auto camRoomC = [&]() { device->setCamera(-4.5f, 1.7f, 9.8f, std::atan2(-4.8f, 0.0f), -0.03f, 72.0f); };

        bool ok = true;
        // ---- OFF baselines (r_ddgi 0 — the device default). ----
        camRoomB(); ok &= renderFrames(20, ddgiShotDir + "/ddgi_corridor_off.png");
        gpuMsAccum = 0.0f; gpuMsN = 0;
        camRoomB(); ok &= renderFrames(40, "");
        const float gpuOff = gpuMsAccum / std::max(1, gpuMsN);
        camRoomC(); ok &= renderFrames(10, ddgiShotDir + "/ddgi_leak_off.png");

        // ---- ON: explicit probe volume over the rig (the auto-fit AABB is
        // origin-based and this rig bakes its boxes at identity), 20x6x20. ----
        x3::rhi::IRenderDevice::DdgiParams dp{};
        dp.enabled = true;
        dp.countX = 20; dp.countY = 6; dp.countZ = 20;
        dp.originX = -9.5f; dp.originY = -1.0f; dp.originZ = -5.0f;
        dp.sizeX = 19.0f; dp.sizeY = 6.0f; dp.sizeZ = 17.0f;
        dp.raysPerProbe = 128;
        device->setDdgiParams(dp);
        camRoomB(); ok &= renderFrames(150, ddgiShotDir + "/ddgi_corridor_on.png");
        gpuMsAccum = 0.0f; gpuMsN = 0;
        camRoomB(); ok &= renderFrames(40, "");
        const float gpuOn = gpuMsAccum / std::max(1, gpuMsN);
        // Probe-field debug visualization (r_ddgi_debug 1):
        dp.debug = 1; device->setDdgiParams(dp);
        camRoomB(); ok &= renderFrames(4, ddgiShotDir + "/ddgi_probes_debug.png");
        dp.debug = 0; device->setDdgiParams(dp);
        // The no-leak canary: sealed room C must STAY dark with DDGI on.
        camRoomC(); ok &= renderFrames(30, ddgiShotDir + "/ddgi_leak_on.png");
        // DYNAMIC proof: remove the point light mid-run — the probes re-converge
        // (hysteresis, ~1-2 s) to the EMISSIVE PANEL as the only GI source. The
        // doorway spill must survive (dimmer, panel-toned) purely from emissive.
        device->setPointLights(nullptr, 0);
        camRoomB(); ok &= renderFrames(180, ddgiShotDir + "/ddgi_emissive_only.png");

        x3::logInfo("--screenshot-ddgi: GPU frame avg " + std::to_string(gpuOff) +
                    " ms (off) vs " + std::to_string(gpuOn) + " ms (on) -> DDGI cost ~" +
                    std::to_string(gpuOn - gpuOff) + " ms (rays+update, 20x6x20 probes, 128 rays)");

        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return ok ? 0 : 1;
    }

    // ---- RT soft-shadow gate shots (--screenshot-rtshadows [outDir]) --------
    // THE GATE SHOT for r_rtshadows: a detention-cell rig lit by ONE ceiling
    // lamp (sun parked below the horizon). A bunk sits low near a wall (tight
    // contact shadow) and a tall pillar stands mid-room (its lamp shadow's
    // penumbra must WIDEN with distance from its base). Tier 0 = today's path:
    // the lamp casts NOTHING (attenuation only); tier 2 = the lamp finally
    // casts. Also: an outdoor plate A/Bs the sun CSM (tier 0) vs RT (tier 1) —
    // contact hardening at the pole base, soft tip — and a 3-frame motion
    // burst checks the 1-spp penumbra noise stays TAA-stable. Headless; logs
    // the GPU-ms delta; exits after the captures.
    if (rtshShot) {
        namespace fs = std::filesystem;
        std::error_code mkec; fs::create_directories(rtshShotDir, mkec);
        x3::logInfo("--screenshot-rtshadows: rendering RT shadow gate shots to " + rtshShotDir);

        // ---- CELL rig: 8x4x8 m room, floor top at y=0 (the ddgi-rig pattern;
        // walls overlap the slabs so no coplanar seams). ----
        std::vector<x3::prims::PrimMesh> parts;
        parts.push_back(x3::prims::makeBox(4.5f, 0.5f, 4.5f,  0.0f, -0.5f, 0.0f)); // floor slab
        parts.push_back(x3::prims::makeBox(4.5f, 0.5f, 4.5f,  0.0f,  4.5f, 0.0f)); // ceiling slab
        parts.push_back(x3::prims::makeBox(0.5f, 2.15f, 4.5f, -4.0f,  2.0f, 0.0f)); // west wall
        parts.push_back(x3::prims::makeBox(0.5f, 2.15f, 4.5f,  4.0f,  2.0f, 0.0f)); // east wall
        parts.push_back(x3::prims::makeBox(4.5f, 2.15f, 0.5f,  0.0f,  2.0f, -4.0f)); // south wall
        parts.push_back(x3::prims::makeBox(4.5f, 2.15f, 0.5f,  0.0f,  2.0f,  4.0f)); // north wall
        // Occluders: a low bunk near the north wall + a tall pillar mid-room.
        parts.push_back(x3::prims::makeBox(0.9f, 0.25f, 0.5f, -2.0f, 0.45f, 2.6f)); // bunk (low -> tight shadow)
        parts.push_back(x3::prims::makeBox(0.22f, 1.5f, 0.22f, 1.2f, 1.5f, 0.6f));  // pillar (tall -> widening penumbra)
        std::vector<x3::rhi::MeshHandle> partMesh;
        for (auto& p : parts)
            partMesh.push_back(device->createMesh(p.verts.data(), (uint32_t)p.verts.size(),
                                                  p.index.data(), (uint32_t)p.index.size()));
        // Emissive lamp fixture just ABOVE the light position (outside every
        // shadow segment — rays stop a clearance short of the source).
        x3::prims::PrimMesh fixture = x3::prims::makeBox(0.35f, 0.06f, 0.35f, 0.0f, 3.85f, 0.0f);
        x3::rhi::MeshHandle fixtureMesh = device->createMesh(fixture.verts.data(), (uint32_t)fixture.verts.size(),
                                                             fixture.index.data(), (uint32_t)fixture.index.size());
        // ---- SUN plate: open ground + a cube + a tall thin pole. ----
        x3::prims::PrimMesh ground = x3::prims::makeBox(10.0f, 0.5f, 10.0f, 0.0f, -0.5f, 0.0f);
        x3::prims::PrimMesh cube   = x3::prims::makeBox(0.5f, 0.5f, 0.5f, -1.5f, 0.5f, 0.5f);
        x3::prims::PrimMesh pole   = x3::prims::makeBox(0.08f, 2.0f, 0.08f, 1.5f, 2.0f, -0.5f);
        x3::rhi::MeshHandle groundMesh = device->createMesh(ground.verts.data(), (uint32_t)ground.verts.size(),
                                                            ground.index.data(), (uint32_t)ground.index.size());
        x3::rhi::MeshHandle cubeMesh   = device->createMesh(cube.verts.data(), (uint32_t)cube.verts.size(),
                                                            cube.index.data(), (uint32_t)cube.index.size());
        x3::rhi::MeshHandle poleMesh   = device->createMesh(pole.verts.data(), (uint32_t)pole.verts.size(),
                                                            pole.index.data(), (uint32_t)pole.index.size());

        auto greyPx = x3::prims::makeSolidRGBA(4, 195, 195, 195);
        x3::rhi::TextureHandle greyTex = device->createTexture(greyPx.data(), 4, 4, /*srgb=*/true);

        const float identity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        const float white[4] = { 1, 1, 1, 1 };
        const float fixtureTint[4]     = { 1.0f, 0.95f, 0.85f, 1.0f };
        const float fixtureEmissive[4] = { 1.0f, 0.9f, 0.7f, 8.0f };

        auto drawCell = [&](const x3::rhi::FrameContext& f) {
            for (auto m : partMesh) device->drawMesh(f, m, greyTex, white, identity);
            device->drawMeshEmissive(f, fixtureMesh, greyTex, fixtureTint, fixtureEmissive, identity);
        };
        auto drawSunPlate = [&](const x3::rhi::FrameContext& f) {
            device->drawMesh(f, groundMesh, greyTex, white, identity);
            device->drawMesh(f, cubeMesh,   greyTex, white, identity);
            device->drawMesh(f, poleMesh,   greyTex, white, identity);
        };

        float gpuMsAccum = 0.0f; int gpuMsN = 0;
        auto renderFrames = [&](int n, const std::string& capturePath, auto&& draw) -> bool {
            for (int i = 0; i < n; ++i) {
                glfwPollEvents();
                if (!capturePath.empty() && i == n - 1) device->armCapture(capturePath.c_str());
                auto f = device->beginFrame();
                if (f.valid) draw(f);
                device->endFrame(f);
                gpuMsAccum += device->stats().gpuFrameMs; ++gpuMsN;
            }
            if (capturePath.empty()) return true;
            const bool ok = device->captureFrame(capturePath.c_str());
            x3::logInfo(std::string(ok ? "--screenshot-rtshadows: wrote " : "--screenshot-rtshadows: FAILED ") + capturePath);
            return ok;
        };
        auto setTier = [&](int tier) {
            x3::rhi::IRenderDevice::RtShadowParams rp{};
            rp.tier = tier;
            device->setRtShadowParams(rp);
        };

        bool ok = true;

        // ===== 1) LAMP gate shot (cell rig, lamp-only) =====
        x3::rhi::PointLight pl{};
        pl.pos[0] = 0.0f; pl.pos[1] = 3.6f; pl.pos[2] = 0.0f; pl.range = 14.0f;
        pl.color[0] = 3.2f; pl.color[1] = 2.9f; pl.color[2] = 2.4f;
        device->setPointLights(&pl, 1);
        device->setAmbient(0.015f, 0.016f, 0.020f);
        device->setShadowBounds(0.0f, 2.0f, 0.0f, 20.0f);
        {   // sun below the horizon: the lamp is the only direct light.
            x3::rhi::IRenderDevice::SkyParams sp{};
            sp.enabled = false;
            sp.sunDir[0] = 0.0f; sp.sunDir[1] = -1.0f; sp.sunDir[2] = 0.01f;
            device->setSkyParams(sp);
        }
        // Camera: SW corner, looking across the pillar toward the NE walls.
        device->setCamera(-3.1f, 1.7f, -3.1f, std::atan2(3.7f, 4.3f), -0.10f, 72.0f);

        setTier(0);
        ok &= renderFrames(30, rtshShotDir + "/lamp_rtshadows_off.png", drawCell);
        gpuMsAccum = 0.0f; gpuMsN = 0;
        ok &= renderFrames(40, "", drawCell);
        const float gpuLampOff = gpuMsAccum / std::max(1, gpuMsN);

        setTier(2);
        ok &= renderFrames(90, rtshShotDir + "/lamp_rtshadows_on.png", drawCell);   // TAA settles the penumbra
        gpuMsAccum = 0.0f; gpuMsN = 0;
        ok &= renderFrames(40, "", drawCell);
        const float gpuLampOn = gpuMsAccum / std::max(1, gpuMsN);

        // Motion burst (tier 2): 3 consecutive frames while the camera slides —
        // the 1-spp penumbra must stay TAA-stable (no sizzle/ghost trails).
        for (int mf = 0; mf < 3; ++mf) {
            device->setCamera(-3.1f + 0.06f * (float)(mf + 1), 1.7f, -3.1f,
                              std::atan2(3.7f, 4.3f), -0.10f, 72.0f);
            ok &= renderFrames(1, rtshShotDir + "/lamp_motion_f" + std::to_string(mf) + ".png", drawCell);
        }

        // ===== 2) SUN A/B (outdoor plate): CSM (tier 0) vs RT (tier 1) =====
        // LOW sun (elev ~23 deg) -> the 4 m pole throws a ~9 m shadow, so the
        // RT penumbra growth (sharp at the base, ~8 cm soft at the tip with the
        // default 0.5 deg sun) reads against CSM's constant 3x3 PCF blur.
        device->setPointLights(nullptr, 0);
        device->setAmbient(0.10f, 0.11f, 0.13f);
        device->setShadowBounds(0.0f, 0.0f, 0.0f, 30.0f);
        {
            x3::rhi::IRenderDevice::SkyParams sp{};
            sp.enabled = true;
            sp.sunDir[0] = 0.70f; sp.sunDir[1] = 0.30f; sp.sunDir[2] = 0.15f;
            device->setSkyParams(sp);
        }
        // Camera low over the pole's shadow line, looking down its length.
        device->setCamera(0.5f, 1.9f, 3.4f, std::atan2(-4.6f, -4.5f), -0.30f, 72.0f);

        setTier(0);
        ok &= renderFrames(40, rtshShotDir + "/sun_csm.png", drawSunPlate);
        setTier(1);
        gpuMsAccum = 0.0f; gpuMsN = 0;
        ok &= renderFrames(90, rtshShotDir + "/sun_rt.png", drawSunPlate);
        const float gpuSunOn = gpuMsAccum / std::max(1, gpuMsN);

        // ===== 3) COST plate (no capture): a 24x24 box field + 8 overlapping
        // lamps + sun — every pixel pays the sun ray AND saturates the K=4
        // point-ray budget. 60-frame GPU-ms averages per tier (the single-frame
        // smoketest stat is useless under GPU contention). =====
        std::vector<x3::rhi::MeshHandle> fieldMesh;
        {
            x3::prims::PrimMesh fb = x3::prims::makeBox(0.45f, 0.9f, 0.45f, 0.0f, 0.9f, 0.0f);
            fieldMesh.push_back(device->createMesh(fb.verts.data(), (uint32_t)fb.verts.size(),
                                                   fb.index.data(), (uint32_t)fb.index.size()));
        }
        std::vector<float> fieldXf;                 // 16 floats per instance (row-major flat)
        for (int gz = 0; gz < 24; ++gz)
            for (int gx = 0; gx < 24; ++gx) {
                const float m[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0,
                                      -9.2f + 0.8f * (float)gx, 0.0f, -9.2f + 0.8f * (float)gz, 1 };
                fieldXf.insert(fieldXf.end(), m, m + 16);
            }
        x3::rhi::PointLight lamps[8]{};
        for (int li = 0; li < 8; ++li) {
            lamps[li].pos[0] = -6.0f + 4.0f * (float)(li % 4);
            lamps[li].pos[1] = 3.0f;
            lamps[li].pos[2] = (li < 4) ? -3.0f : 3.0f;
            lamps[li].range = 16.0f;             // every lamp covers most pixels
            lamps[li].color[0] = 2.2f; lamps[li].color[1] = 2.0f; lamps[li].color[2] = 1.7f;
        }
        device->setPointLights(lamps, 8);
        auto drawField = [&](const x3::rhi::FrameContext& f) {
            device->drawMesh(f, groundMesh, greyTex, white, identity);
            for (size_t mi = 0; mi + 16 <= fieldXf.size(); mi += 16)
                device->drawMesh(f, fieldMesh[0], greyTex, white, &fieldXf[mi]);
        };
        device->setCamera(0.0f, 6.5f, 13.0f, std::atan2(-13.0f, 0.0f), -0.42f, 72.0f);
        auto costAvg = [&](int tier) -> float {
            setTier(tier);
            renderFrames(20, "", drawField);     // settle (pipeline swap, TLAS)
            gpuMsAccum = 0.0f; gpuMsN = 0;
            renderFrames(60, "", drawField);
            return gpuMsAccum / std::max(1, gpuMsN);
        };
        const float costT0 = costAvg(0);
        const float costT1 = costAvg(1);
        const float costT2 = costAvg(2);

        x3::logInfo("--screenshot-rtshadows: GPU frame avg lamp " + std::to_string(gpuLampOff) +
                    " ms (tier 0) vs " + std::to_string(gpuLampOn) + " ms (tier 2) -> point-shadow cost ~" +
                    std::to_string(gpuLampOn - gpuLampOff) + " ms; sun-RT plate avg " +
                    std::to_string(gpuSunOn) + " ms (full-res, 1 spp)");
        x3::logInfo("--screenshot-rtshadows: COST plate (576 boxes, 8 lamps, K=4 saturated, 60-frame avg): tier0 " +
                    std::to_string(costT0) + " ms, tier1 " + std::to_string(costT1) + " ms (+" +
                    std::to_string(costT1 - costT0) + " sun), tier2 " + std::to_string(costT2) + " ms (+" +
                    std::to_string(costT2 - costT1) + " points; full-res inline)");

        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return ok ? 0 : 1;
    }

    // ---- Showroom preview (--screenshot-showroom [path.png]) ---------------
    // Load the baked Unity scene export (the "3D Showroom Level Kit" Example_01),
    // frame the camera on the BUILDING cluster (the surrounding km of decorative
    // scatter sits off-frame), and capture a PBR-shaded PNG. Headless, like the rest.
    if (showroomShot) {
        x3::logInfo("--screenshot-showroom: rendering the Unity showroom export to " + showroomShotPath);
        x3::game::EnvArtSystem showroom;
        const bool ok = showroom.buildFromGlb(*device, x3::game::convertedGlbRoot(),
                                              "ShowRoom_Vol30/Example_01.glb");
        if (!ok) x3::logError("--screenshot-showroom: scene GLB failed to load");

        // DAY<->NIGHT state (default NIGHT; X3_SHOWROOM_DAY=1 -> DAY). The helper sets
        // sky/sun/ambient/bloom for the chosen state (no interior point lights on the
        // exterior shot). DAY = Unity-match bright cool sky; NIGHT = the original recipe.
        const bool gShowroomDay = showroomDayDefault();
        applyShowroomTimeOfDay(device.get(), gShowroomDay, /*interiorLights*/nullptr);
        x3::logInfo(std::string("--screenshot-showroom: time-of-day = ") + (gShowroomDay ? "DAY" : "NIGHT"));
        // Disable the SSAO/GI depth PRE-PASS for the showroom: it makes the color pass use an
        // EQUAL depth test vs full-quad pre-pass depth, which would punch sky holes through
        // alpha-cutout foliage (the pre-pass has no fragment shader to discard). Without it the
        // color pass uses LESS + depth-write, so cutout sprites composite correctly.
        { x3::rhi::IRenderDevice::SsaoParams s{}; s.enabled = false; device->setSsaoParams(s); }
        { x3::rhi::IRenderDevice::GiParams   g{}; g.enabled = false; device->setGiParams(g); }

        // --- NIGHT-SKY planets: load the 6 FORGE3D bodies via the SHARED helper (same
        // files / slot order / srgb as --screenshot-nightsky). CELESTIAL placement:
        // each body is a fixed sky DIRECTION (az/el) + angular diameter, anchored on
        // the camera eye at draw time — no per-scene repositioning needed.
        int nPlanetTexFail = 0;
        x3::rhi::MeshHandle planetMesh{};
        x3::rhi::MeshHandle ringMesh{};
        std::vector<NightSkyPlanet> planets =
            loadNightSkyPlanets(device.get(), planetMesh, nPlanetTexFail, "--screenshot-showroom", &ringMesh);
        if (nPlanetTexFail > 0)
            x3::logError("--screenshot-showroom: " + std::to_string(nPlanetTexFail) +
                         " planet texture(s) missing — some bodies may render flat");
        // Non-zero sky-animation time so the starfield (+ any future time-driven sky)
        // is captured in a settled, animated state. Corona uTime flows via drawPlanet.
        // NIGHT only — the starfield/wheeling is a night feature (auto-hidden on the
        // bright DAY sky, and the planets are not drawn in DAY).
        if (!gShowroomDay) device->setSkyTime(10.0f);

        // Frame on the BUILDING using ENGINE-space bounds (the engine's node-transform
        // composition differs from the Python analysis, so trust the engine). namedBounds
        // filters to structural/furniture meshes, ignoring the km of decorative scatter.
        const std::vector<std::string> kBuild = {
            "room", "pilar", "plateform", "platform", "stair", "window", "showcase",
            "table", "chair", "carpet", "tube", "halogen", "cache", "tv_screen" };
        float bmn[3], bmx[3];
        const uint32_t nb = showroom.namedBounds(kBuild, bmn, bmx);
        if (nb == 0) { showroom.worldBounds(bmn, bmx); x3::logWarn("--screenshot-showroom: 0 named building nodes; framing the full scene"); }
        x3::logInfo("--screenshot-showroom: building bounds (" + std::to_string(nb) + " nodes) min(" +
            std::to_string(bmn[0]) + "," + std::to_string(bmn[1]) + "," + std::to_string(bmn[2]) +
            ") max(" + std::to_string(bmx[0]) + "," + std::to_string(bmx[1]) + "," + std::to_string(bmx[2]) + ")");

        // Center + extent -> stand back along +Z, kept within the 200 m far plane.
        const float cx = (bmn[0] + bmx[0]) * 0.5f, cy = (bmn[1] + bmx[1]) * 0.5f, cz = (bmn[2] + bmx[2]) * 0.5f;
        const float ex = bmx[0] - bmn[0], ey = bmx[1] - bmn[1];
        float span = ex > ey ? ex : ey;
        float dist = span * 0.75f + 12.0f;
        if (dist > 175.0f) dist = 175.0f;
        if (dist < 18.0f)  dist = 18.0f;
        float camx = cx, camy = cy + ey * 0.12f, camz = bmx[2] + dist;         // in front (+Z face)
        float dx = cx - camx, dy = cy - camy, dz = cz - camz;                  // look toward center
        float len = std::sqrt(dx * dx + dy * dy + dz * dz); if (len < 1e-3f) len = 1e-3f;
        const float pitch = std::asin(dy / len);
        const float yaw   = std::atan2(dz, dx);
        // X3_SHOWROOM_CAMOFF="dx,dy,dz" TRANSLATES the eye (yaw/pitch unchanged) —
        // the no-parallax proof for the celestial planets: the foreground shifts,
        // the sky composition must NOT (the bodies re-anchor on the moved eye).
        if (const char* off = std::getenv("X3_SHOWROOM_CAMOFF")) {
            float ox = 0, oy = 0, oz = 0;
            if (std::sscanf(off, "%f,%f,%f", &ox, &oy, &oz) == 3) {
                camx += ox; camy += oy; camz += oz;
                x3::logInfo("--screenshot-showroom: CAMOFF applied (" + std::to_string(ox) + "," +
                            std::to_string(oy) + "," + std::to_string(oz) + ")");
            }
        }
        x3::logInfo("--screenshot-showroom: cam(" + std::to_string(camx) + "," + std::to_string(camy) + "," +
            std::to_string(camz) + ") yaw=" + std::to_string(yaw) + " pitch=" + std::to_string(pitch));
        device->setCamera(camx, camy, camz, yaw, pitch, 72.0f);
        // Frame the sun's shadow box on the building (+ surrounding firs) so they cast shadows
        // (the default ~45 m camera-following box sits 100+ m short of the building).
        device->setShadowBounds(cx, cy, cz, 150.0f);

        // (Planet placement is CELESTIAL — fixed world-space sky directions anchored
        // on the camera eye inside drawNightSkyPlanets — so there is NO per-scene
        // repositioning here anymore. The bodies hang out in the night sky at their
        // az/el table defaults, occluded by the spire/terrain via the depth test.)

        // Draw the WHOLE scene (all 1150 drawables). The earlier "~480 draws blanks the frame"
        // ceiling was a SYMPTOM of the depth.vert/shadow.vert SSBO-stride bug (garbage depths at
        // high instance indices compounded with draw count) — fixed, so the full scene composites.
        uint32_t showroomMaxDraw = 0xFFFFFFFFu;
        if (const char* e = std::getenv("X3_SHOWROOM_MAXDRAW")) showroomMaxDraw = (uint32_t)std::strtoul(e, nullptr, 10);
        x3::logInfo("--screenshot-showroom: maxDraw=" + std::to_string(showroomMaxDraw));

        // --ddgi: give the probe field time to converge (hysteresis warm-up ramp
        // + a few multibounce generations) before the still is captured.
        const int kSettle = ddgiForce ? 120 : 16;
        for (int i = 0; i < kSettle; ++i) {
            glfwPollEvents();
            if (i == kSettle - 1) device->armCapture(showroomShotPath.c_str());
            auto frame = device->beginFrame();
            if (frame.valid) {
                const uint32_t nDrawn = showroom.draw(*device, frame, showroomMaxDraw, nullptr, nullptr);
                if (i == 0) x3::logInfo("--screenshot-showroom: drew " + std::to_string(nDrawn) + " drawables (of 1150)");
                // Hang the night-sky planets over the spire (AFTER the env so depth occludes correctly).
                // Ring mesh enables the gas giant's alpha ring; the device composites the
                // transparent glow shells (atmosphere/corona/ring) AFTER the opaque bodies.
                // NIGHT only — DAY has no planets (the bright sky carries the exterior).
                if (!gShowroomDay)
                    drawNightSkyPlanets(device.get(), frame, planetMesh, planets, 10.0f,
                                        camx, camy, camz, ringMesh);
            }
            device->endFrame(frame);
        }
        const bool wrote = device->captureFrame(showroomShotPath.c_str());
        if (wrote) x3::logInfo("--screenshot-showroom: wrote " + showroomShotPath);
        else       x3::logError("--screenshot-showroom: capture FAILED");

        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return wrote ? 0 : 1;
    }

    // ======================================================================
    // HERO CAR showcase (--screenshot-car [outDir]) — the NFS beauty pass.
    //   * INTERIOR: the Unity showroom GLB as surroundings; the car posed on a
    //     POLISHED reflector slab (the feat/reflections floor material dial)
    //     under EMISSIVE light panels, so SSR/RT reflections visibly sweep the
    //     clearcoat paint. DAY (4 turntable angles) + NIGHT (2 angles, lights
    //     glowing + bloom).
    //   * EXTERIOR NIGHT: the car alone on a wet-asphalt slab under the FORGE3D
    //     planet night sky (2 angles, headlights on).
    // Headless + 4x SSAA; each still settles ~90 frames so TAA history, SSR,
    // auto-exposure and the IBL probe converge before capture.
    // ======================================================================
    if (carShot) {
        namespace fs = std::filesystem;
        std::error_code mkec; fs::create_directories(carShotDir, mkec);
        x3::logInfo("--screenshot-car: writing turntable set to " + carShotDir);

        // --- The showroom surroundings (identity, baked node transforms). ---
        x3::game::EnvArtSystem showroom;
        const bool roomOk = showroom.buildFromGlb(*device, x3::game::convertedGlbRoot(),
                                                  "ShowRoom_Vol30/Example_01.glb");
        if (!roomOk) x3::logWarn("--screenshot-car: showroom GLB missing — interior shots get the studio slab only");

        // Anchor the car on the showroom's GROUND floor: bounds of the building
        // cluster (same name filter as --screenshot-showroom), car at the center
        // of the X span, on the floor (min Y of the named nodes).
        float bmn[3] = {0,0,0}, bmx[3] = {0,0,0};
        bool haveRoom = false;
        if (roomOk) {
            const std::vector<std::string> kBuild = {
                "room", "pilar", "plateform", "platform", "stair", "window", "showcase",
                "table", "chair", "carpet", "tube", "halogen", "cache", "tv_screen" };
            haveRoom = showroom.namedBounds(kBuild, bmn, bmx) > 0;
        }
        // Anchor in the OPEN west bay (the building-center has the round podium +
        // NPC mannequins; +21 m from the west wall is clean polished hall).
        float carX = haveRoom ? (bmn[0] + 21.0f) : 0.0f;
        float carY = haveRoom ? bmn[1] : 0.0f;
        float carZ = haveRoom ? (bmn[2] + bmx[2]) * 0.5f : 0.0f;
        if (const char* e = std::getenv("X3_CAR_POS")) {   // manual override: "x,y,z"
            float px2, py2, pz2;
            if (std::sscanf(e, "%f,%f,%f", &px2, &py2, &pz2) == 3) { carX = px2; carY = py2; carZ = pz2; }
        }
        x3::logInfo("--screenshot-car: car anchor (" + std::to_string(carX) + "," +
                    std::to_string(carY) + "," + std::to_string(carZ) + ") haveRoom=" +
                    (haveRoom ? "1" : "0"));

        // --- The hero car (CTR.glb: clearcoat paint + emissive lights). The GLB
        // sits on y=0 with +Z = nose, so the instance transform is yaw+translate.
        const float kSlabTop = 0.02f;                  // polished slab top above the floor
        auto carXform = [&](float yawRad, float ox, float oy, float oz, float out[16]) {
            const float c = std::cos(yawRad), s = std::sin(yawRad);
            const float m[16] = { c,0,-s,0,  0,1,0,0,  s,0,c,0,  ox,oy,oz,1 };
            for (int i = 0; i < 16; ++i) out[i] = m[i];
        };
        float carT[16];
        carXform(0.6f, carX, carY + kSlabTop, carZ, carT);
        x3::game::EnvArtSystem car;
        const bool carOk = car.buildFromGlbAt(*device, x3::game::convertedGlbRoot(),
                                              "Vehicles/CTR.glb", carT);
        if (!carOk) {
            x3::logError("--screenshot-car: Vehicles/CTR.glb FAILED to load — aborting");
            device->shutdown();
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return 1;
        }

        // --- Studio dressing: a polished reflector slab (the feat/reflections
        // floor dial: rough 0.08 / metal 0.5 on a white dielectric) + emissive
        // panels flanking + above the car (the reflections that sweep the body).
        x3::rhi::TextureHandle polishedMr{}, asphaltMr{};
        { const uint8_t mr[4] = { 0,  20, 128, 255 }; polishedMr = device->createTexture(mr, 1, 1, false); }
        { const uint8_t mr[4] = { 0,  56,  26, 255 }; asphaltMr  = device->createTexture(mr, 1, 1, false); } // wet asphalt: rough .22, metal .10
        auto makeMesh = [&](const x3::prims::PrimMesh& pm) {
            return device->createMesh(pm.verts.data(), (uint32_t)pm.verts.size(),
                                      pm.index.data(), (uint32_t)pm.index.size());
        };
        x3::rhi::MeshHandle slabMesh  = makeMesh(x3::prims::makeBox(5.5f, 0.01f, 5.5f, 0, 0, 0, 0.25f));
        x3::rhi::MeshHandle padMesh   = makeMesh(x3::prims::makeBox(60.0f, 0.01f, 60.0f, 0, 0, 0, 0.25f));
        x3::rhi::MeshHandle stripMesh = makeMesh(x3::prims::makeBox(3.2f, 0.04f, 0.30f, 0, 0, 0, 1.0f));
        x3::rhi::MeshHandle panelMesh = makeMesh(x3::prims::makeBox(0.05f, 1.1f, 2.6f, 0, 0, 0, 1.0f));
        const float kWhite[4]   = { 0.97f, 0.97f, 0.98f, 1.0f };
        const float kAsphalt[4] = { 0.045f, 0.045f, 0.05f, 1.0f };
        const float kNoEmis[4]  = { 0, 0, 0, 0 };
        const float kStripEmis[4] = { 1.0f, 0.99f, 0.95f, 6.0f };   // cool-white HDR strips
        const float kPanelEmisL[4] = { 0.35f, 0.65f, 1.0f, 2.2f };  // cyan panel (left)
        const float kPanelEmisR[4] = { 1.0f, 0.45f, 0.20f, 2.2f };  // amber panel (right)
        auto at = [&](float x, float y, float z, float out[16]) {
            const float m[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, x,y,z,1 };
            for (int i = 0; i < 16; ++i) out[i] = m[i];
        };

        // Per-still draw: env + car + dressing. `interior` gates the showroom +
        // studio strips; exterior swaps the slab material for wet asphalt.
        auto drawScene = [&](const x3::rhi::FrameContext& fr, bool interior) {
            float m[16];
            if (interior && roomOk) showroom.draw(*device, fr);
            car.draw(*device, fr);
            if (interior) {
                at(carX, carY + 0.01f, carZ, m);
                device->drawMeshPBR(fr, slabMesh, x3::rhi::TextureHandle{}, x3::rhi::TextureHandle{},
                                    polishedMr, kWhite, kNoEmis, m);
                // Ceiling light strips (3, across the car) + side emissive panels.
                for (int i = -1; i <= 1; ++i) {
                    at(carX, carY + 3.4f, carZ + (float)i * 2.2f, m);
                    device->drawMeshEmissive(fr, stripMesh, x3::rhi::TextureHandle{}, kWhite, kStripEmis, m);
                }
                at(carX - 5.6f, carY + 1.15f, carZ, m);
                device->drawMeshEmissive(fr, panelMesh, x3::rhi::TextureHandle{}, kWhite, kPanelEmisL, m);
                at(carX + 5.6f, carY + 1.15f, carZ, m);
                device->drawMeshEmissive(fr, panelMesh, x3::rhi::TextureHandle{}, kWhite, kPanelEmisR, m);
            } else {
                at(carX, carY + 0.01f, carZ, m);
                device->drawMeshPBR(fr, padMesh, x3::rhi::TextureHandle{}, x3::rhi::TextureHandle{},
                                    asphaltMr, kAsphalt, kNoEmis, m);
            }
        };

        // --- Pipeline state: SSAO/GI off (the showroom foliage cutout rule),
        // TAA on (default), SSR + RT reflections ON full-res (the money dial). ---
        { x3::rhi::IRenderDevice::SsaoParams s{}; s.enabled = false; device->setSsaoParams(s); }
        { x3::rhi::IRenderDevice::GiParams   g{}; g.enabled = false; device->setGiParams(g); }
        {
            x3::rhi::IRenderDevice::ReflectionParams rf{};
            rf.ssr = true; rf.rtFallback = true; rf.fullRes = true; rf.intensity = 1.0f;
            device->setReflectionParams(rf);
            x3::logInfo(std::string("--screenshot-car: reflections ON; rayTracingSupported=") +
                        (device->rayTracingSupported() ? "YES" : "NO"));
        }
        device->setShadowBounds(carX, carY, carZ, 40.0f);

        // Point lights for the car bay (key + fills; pre-multiplied color*intensity).
        std::vector<x3::rhi::PointLight> bay;
        auto addLight = [&](float x, float y, float z, float r, float cr, float cg, float cb) {
            x3::rhi::PointLight pl{}; pl.pos[0]=x; pl.pos[1]=y; pl.pos[2]=z; pl.range=r;
            pl.color[0]=cr; pl.color[1]=cg; pl.color[2]=cb; bay.push_back(pl);
        };
        addLight(carX,        carY + 3.2f, carZ,        14.0f, 6.0f, 5.9f, 5.6f);   // overhead key
        addLight(carX - 4.4f, carY + 1.6f, carZ + 1.5f, 10.0f, 1.2f, 2.2f, 3.4f);   // cool fill (cyan side)
        addLight(carX + 4.4f, carY + 1.6f, carZ - 1.5f, 10.0f, 3.4f, 1.6f, 0.7f);   // warm fill (amber side)

        // Night-sky planets for the EXTERIOR stills (the shared nightsky kit).
        int nPlanetTexFail = 0;
        x3::rhi::MeshHandle planetMesh{}, ringMesh{};
        std::vector<NightSkyPlanet> planets =
            loadNightSkyPlanets(device.get(), planetMesh, nPlanetTexFail, "--screenshot-car", &ringMesh);

        // --- One settled still: pose, settle, capture. ---
        int shotFails = 0;
        auto still = [&](const std::string& name, bool interior, bool day,
                         float camYaw, float camDist, float camHeight, float fov) {
            applyShowroomTimeOfDay(device.get(), day, &bay);
            if (!day) device->setSkyTime(10.0f);
            // Orbit the camera around the car anchor at `camYaw` (0 = looking from +Z).
            const float cx = carX + std::sin(camYaw) * camDist;
            const float cz = carZ + std::cos(camYaw) * camDist;
            const float cy = carY + camHeight;
            const float lx = carX - cx, ly = (carY + 0.55f) - cy, lz = carZ - cz;
            const float len = std::max(std::sqrt(lx*lx + ly*ly + lz*lz), 1e-3f);
            device->setCamera(cx, cy, cz, std::atan2(lz, lx), std::asin(ly / len), fov);
            const std::string path = carShotDir + "/" + name + ".png";
            const int kSettle = 90;   // TAA history + SSR + auto-exposure + IBL probe
            for (int i = 0; i < kSettle; ++i) {
                glfwPollEvents();
                if (i == kSettle - 1) device->armCapture(path.c_str());
                auto fr = device->beginFrame();
                if (fr.valid) {
                    drawScene(fr, interior);
                    if (!interior && !day)
                        drawNightSkyPlanets(device.get(), fr, planetMesh, planets, 10.0f,
                                            cx, cy, cz, ringMesh);   // FOLD FIX: eye-anchored API
                }
                device->endFrame(fr);
            }
            const bool ok = device->captureFrame(path.c_str());
            if (ok) x3::logInfo("--screenshot-car: wrote " + path);
            else  { x3::logError("--screenshot-car: capture FAILED " + path); ++shotFails; }
        };

        // The turntable. Interior DAY (4 angles), interior NIGHT (2), then move
        // the car to the EXTERIOR pad for the planet-sky night pair.
        still("car_day_front34",  true, true,  0.65f, 5.0f, 1.25f, 48.0f);
        still("car_day_rear34",   true, true,  2.60f, 5.2f, 1.35f, 48.0f);
        still("car_day_profile",  true, true, -1.50f, 5.4f, 1.00f, 48.0f);
        still("car_day_frontlow", true, true,  0.10f, 4.8f, 0.60f, 52.0f);
        still("car_night_front34", true, false, 0.80f, 5.0f, 1.15f, 48.0f);
        still("car_night_rear34",  true, false, 2.45f, 5.2f, 1.05f, 48.0f);
        // EXTERIOR: re-pose the car on the asphalt pad away from the building.
        carX += 0.0f; carY = haveRoom ? carY : 0.0f; carZ = haveRoom ? (bmx[2] + 30.0f) : 0.0f;
        carXform(2.85f, carX, carY + kSlabTop, carZ, carT);
        car.setInstanceTransform(0, carT);
        device->setShadowBounds(carX, carY, carZ, 40.0f);
        // Re-aim the planets around the new pad (high in the back of frame).
        // FOLD FIX: fix/planets-sky moved NightSkyPlanet to az/el/angularDiameter
        // (eye-anchored in drawNightSkyPlanets) — convert the old worldPos/radius
        // fan (planets ~120 m out, 45+ m high, radius 13/26 m) to angles.
        {
            constexpr float kRadToDeg = 180.0f / 3.14159265358979323846f;
            constexpr float kDist = 120.0f;
            int pi = 0;
            for (NightSkyPlanet& b : planets) {
                const float az = -1.2f + 0.6f * (float)pi;     // fan behind the car (-Z side)
                const float height = 45.0f + 6.0f * (float)pi;
                const float radius = (pi == 1 || pi == 4) ? 26.0f : 13.0f;
                b.azimuthDeg         = az * kRadToDeg;
                b.elevationDeg       = std::atan2(height, kDist) * kRadToDeg;
                b.angularDiameterDeg = 2.0f * std::atan2(radius, kDist) * kRadToDeg;
                ++pi;
            }
        }
        still("car_extnight_front34", false, false, 2.95f, 6.4f, 1.25f, 50.0f);
        still("car_extnight_rear",    false, false, 0.35f, 7.0f, 0.85f, 48.0f);

        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return shotFails == 0 ? 0 : 1;
    }

    // ---- Planet preview (--screenshot-planet [path.png]) -------------------
    // Build a UV-sphere Moon body, load the 5 FORGE3D Moon textures from the pack,
    // shade it with the dedicated planet pipeline (object-space triplanar PBR +
    // scatter), hang it against a dark space backdrop lit from the side so a clear
    // day/night terminator shows, settle one headless frame, and capture a PNG.
    if (planetShot) {
        x3::logInfo("--screenshot-planet: rendering procedural Moon to " + planetShotPath);

        // --- Load the 5 Moon textures straight from the FORGE3D pack (no repo copy).
        const std::string kPack = "C:/Users/Tim/X3/Assets/FORGE3D/Planets/Moon/Textures/";
        const std::string kAtmo = "C:/Users/Tim/X3/Assets/FORGE3D/Planets/Atmosphere/";
        // albedo, normal, detail, spec, scatter — sRGB for color/albedo/scatter, linear for normal/spec.
        const std::string paths[5] = {
            kPack + "moon_01.png", kPack + "moon_01_normal.png", kPack + "moon_01_detail.png",
            kPack + "moon_01_spec.png", kAtmo + "sunset_yellow_05.png",
        };
        const bool srgbFlag[5] = { true, false, true, false, true };
        x3::rhi::TextureHandle tex[5] = {};
        bool allLoaded = true;
        for (int t = 0; t < 5; ++t) {
            int w = 0, h = 0, comp = 0;
            stbi_uc* px = stbi_load(paths[t].c_str(), &w, &h, &comp, 4);   // force RGBA8
            if (!px) {
                x3::logError("--screenshot-planet: FAILED to load " + paths[t]);
                allLoaded = false;
                continue;
            }
            tex[t] = device->createTexture(px, (uint32_t)w, (uint32_t)h, srgbFlag[t]);
            x3::logInfo("--screenshot-planet: loaded " + paths[t] + " (" + std::to_string(w) + "x" +
                        std::to_string(h) + (srgbFlag[t] ? ", srgb)" : ", linear)"));
            stbi_image_free(px);
        }
        if (!allLoaded) x3::logError("--screenshot-planet: one or more Moon textures missing — render may be flat");

        // --- UV-sphere Moon mesh (unit radius). pos == normal for the triplanar.
        x3::prims::PrimMesh sphere = x3::prims::makeUVSphere(64, 128);
        x3::rhi::MeshHandle moon = device->createMesh(sphere.verts.data(), (uint32_t)sphere.verts.size(),
                                                      sphere.index.data(), (uint32_t)sphere.index.size());

        // --- Model: unit sphere at the origin (radius 1). Column-major identity*scale.
        const float kRadius = 1.0f;
        float model[16] = {
            kRadius, 0, 0, 0,
            0, kRadius, 0, 0,
            0, 0, kRadius, 0,
            0, 0, 0, 1,
        };

        // --- Dark space backdrop: near-black zenith, faint horizon, a side sun so the
        // moon shows a clear day/night terminator. The Camera UBO sun direction the
        // planet frag reads is sourced from SkyParams.sunDir, so set it here.
        // Sun mostly TOWARD the camera (+Z dominant) and to the upper-side (+X,+Y) so
        // the camera-facing hemisphere reads as a bright gibbous Moon with the day/night
        // terminator sweeping across the LEFT third of the visible disc (a clear, lit
        // hero shot rather than a thin night-side crescent).
        const float sunDir[3] = { 0.32f, 0.26f, 1.0f };   // normalized internally
        x3::rhi::IRenderDevice::SkyParams sp{};
        sp.enabled = true;
        sp.sunDir[0] = sunDir[0]; sp.sunDir[1] = sunDir[1]; sp.sunDir[2] = sunDir[2];
        sp.sunColor[0] = 1.0f; sp.sunColor[1] = 0.96f; sp.sunColor[2] = 0.90f;
        sp.sunIntensity = 0.25f;   // low — this is deep space, not a daylit horizon
        sp.haze = 0.0f; sp.exposure = 1.0f;
        sp.zenith[0]  = 0.01f; sp.zenith[1]  = 0.01f; sp.zenith[2]  = 0.02f;  // near-black space
        sp.horizon[0] = 0.02f; sp.horizon[1] = 0.02f; sp.horizon[2] = 0.04f;  // slightly lighter
        device->setSkyParams(sp);
        device->setAmbient(0.07f, 0.07f, 0.09f);   // small cool fill so the night side reads as dark rock, not pure black
        device->setBloom(0.10f);
        // No SSAO/GI pre-pass: the planet uses the opaque depth LESS+write pipeline.
        { x3::rhi::IRenderDevice::SsaoParams s{}; s.enabled = false; device->setSsaoParams(s); }
        { x3::rhi::IRenderDevice::GiParams   g{}; g.enabled = false; device->setGiParams(g); }

        // --- Camera ~3 units back, looking at the origin, modest FOV so the unit
        // sphere fills ~70% of the frame.
        const float camx = 0.0f, camy = 0.0f, camz = 3.0f;
        const float yaw   = std::atan2(0.0f - camz, 0.0f - camx);   // toward origin in XZ
        const float pitch = 0.0f;                                   // level
        const float fovDeg = 40.0f;
        device->setCamera(camx, camy, camz, yaw, pitch, fovDeg);

        x3::logInfo("--screenshot-planet: cam(" + std::to_string(camx) + "," + std::to_string(camy) + "," +
            std::to_string(camz) + ") yaw=" + std::to_string(yaw) + " pitch=" + std::to_string(pitch) +
            " sun(" + std::to_string(sunDir[0]) + "," + std::to_string(sunDir[1]) + "," + std::to_string(sunDir[2]) +
            ") out=" + planetShotPath);

        const int kSettle = 8;
        bool wrote = false;
        for (int i = 0; i < kSettle; ++i) {
            glfwPollEvents();
            if (i == kSettle - 1) device->armCapture(planetShotPath.c_str());
            auto frame = device->beginFrame();
            if (frame.valid) {
                // typeIndex 0 = Moon; its 5 maps in pc.tex[0..4] order.
                device->drawPlanet(frame, moon, model, 0u /*Moon*/, tex, 5u, 0.0f);
            }
            device->endFrame(frame);
        }
        wrote = device->captureFrame(planetShotPath.c_str());
        if (wrote) x3::logInfo("--screenshot-planet: wrote " + planetShotPath);
        else       x3::logError("--screenshot-planet: capture FAILED");

        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return wrote ? 0 : 1;
    }

    // ---- Night-sky preview (--screenshot-nightsky [path.png]) --------------
    // Build ONE UV-sphere mesh and hang 6 VARIED planet TYPES staggered across a
    // dark, star-flecked dome — each shaded by its OWN per-type pipeline. Loads each
    // type's documented texture set from the FORGE3D pack (slot order per the frag
    // header / TEXTURE_MANIFEST.md), places them at varied azimuth/elevation/distance/
    // radius so they read as distinct sky bodies of different apparent sizes, and
    // captures a single 4x-SSAA PNG. The procedural starfield in sky.frag appears on
    // the dark sky automatically. (Atmosphere/suncorona/ring shells are DEFERRED.)
    if (nightskyShot) {
        x3::logInfo("--screenshot-nightsky: rendering staggered multi-planet sky to " + nightskyShotPath);

        // --- Build the UV-sphere + load the 6 FORGE3D planet types via the shared
        // helper (same files / slot order / srgb / default positions as before).
        int nTexFail = 0;
        x3::rhi::MeshHandle planetMesh{};
        x3::rhi::MeshHandle ringMesh{};
        std::vector<NightSkyPlanet> bodies =
            loadNightSkyPlanets(device.get(), planetMesh, nTexFail, "--screenshot-nightsky", &ringMesh);

        if (nTexFail > 0)
            x3::logError("--screenshot-nightsky: " + std::to_string(nTexFail) +
                         " texture(s) missing — some bodies may render flat");

        // --- DARK NIGHT sky: near-black zenith, faint horizon, very low sun intensity,
        // no haze. The procedural starfield in sky.frag paints onto the dark dome.
        const float sunDir[3] = { 0.90f, 0.42f, 0.08f };  // az ~95 el ~25: half/gibbous phase on BOTH sky clusters (normalized internally)
        x3::rhi::IRenderDevice::SkyParams sp{};
        sp.enabled = true;
        sp.sunDir[0] = sunDir[0]; sp.sunDir[1] = sunDir[1]; sp.sunDir[2] = sunDir[2];
        sp.sunColor[0] = 1.0f; sp.sunColor[1] = 0.97f; sp.sunColor[2] = 0.92f;
        sp.sunIntensity = 0.04f;     // deep night — the bodies are the heroes, not the sky
        sp.haze = 0.0f; sp.exposure = 1.0f;
        sp.zenith[0]  = 0.005f; sp.zenith[1]  = 0.005f; sp.zenith[2]  = 0.012f;  // near-black
        sp.horizon[0] = 0.010f; sp.horizon[1] = 0.012f; sp.horizon[2] = 0.025f;  // slightly lighter
        device->setSkyParams(sp);
        device->setAmbient(0.06f, 0.06f, 0.10f);   // cool low fill
        device->setBloom(0.15f);
        device->setSkyTime(10.0f);   // non-zero sky-animation time (starfield rotation)
        { x3::rhi::IRenderDevice::SsaoParams s{}; s.enabled = false; device->setSsaoParams(s); }
        { x3::rhi::IRenderDevice::GiParams   g{}; g.enabled = false; device->setGiParams(g); }

        // --- Camera near origin, aimed at the HERO cluster of the celestial layout
        // (sun az +28 / terrestrial az -22 / moon az -44 / lava az +47 — az/el table in
        // loadNightSkyPlanets), tilted up so the bodies ride the upper frame. The
        // gas giant (az -147) / ice are out of this frame by design (the sky wraps
        // the full horizon); X3_NIGHTSKY_AZDEG aims the camera at any azimuth
        // (e.g. -147 for the ringed-gas-giant proof).
        const float camx = 0.0f, camy = 6.0f, camz = 18.0f;
        float aimAzDeg = 0.0f;
        if (const char* e = std::getenv("X3_NIGHTSKY_AZDEG")) aimAzDeg = (float)std::atof(e);
        const float yaw   = (aimAzDeg - 90.0f) * 3.14159265f / 180.0f;  // az 0 = -Z -> yaw = az - 90deg
        const float pitch = 0.45f;      // ~26deg up (bodies sit at 16..45deg elevation)
        const float fovDeg = 75.0f;
        device->setCamera(camx, camy, camz, yaw, pitch, fovDeg);

        x3::logInfo("--screenshot-nightsky: cam(" + std::to_string(camx) + "," + std::to_string(camy) + "," +
            std::to_string(camz) + ") yaw=" + std::to_string(yaw) + " pitch=" + std::to_string(pitch) +
            " fov=" + std::to_string(fovDeg) + " bodies=" + std::to_string(bodies.size()) +
            " out=" + nightskyShotPath);

        const float kUTime = 10.0f;   // fixed animation time for the still (animated types)
        const int kSettle = 8;
        bool wrote = false;
        for (int i = 0; i < kSettle; ++i) {
            glfwPollEvents();
            if (i == kSettle - 1) device->armCapture(nightskyShotPath.c_str());
            auto frame = device->beginFrame();
            if (frame.valid) {
                drawNightSkyPlanets(device.get(), frame, planetMesh, bodies, kUTime,
                                    camx, camy, camz, ringMesh);
            }
            device->endFrame(frame);
        }
        wrote = device->captureFrame(nightskyShotPath.c_str());
        if (wrote) x3::logInfo("--screenshot-nightsky: wrote " + nightskyShotPath);
        else       x3::logError("--screenshot-nightsky: capture FAILED");

        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return wrote ? 0 : 1;
    }

    // ---- Cutscene film still (--cutscene-shot [path.png] --cuetime <s>) ----
    // HEADLESS: load the cutscene (default = the shipped cold open), build the full
    // cinematic scene (planets + ships + FX), seek the deterministic player to
    // --cuetime (events re-fire as seeked so trails/impacts are state-correct),
    // render + capture ONE supersampled frame, exit. The film-strip pipeline behind
    // docs/screenshots/coldopen/.
    if (cutsceneShot) {
        const std::string csPath = !cutsceneFile.empty()
            ? cutsceneFile
            : x3::game::assetRoot() + "/cutscenes/cold_open.cutscene.json";
        x3::cut::Cutscene cs;
        std::vector<std::string> errs;
        if (!x3::cut::loadCutsceneFile(csPath, cs, errs)) {
            for (const auto& e : errs) x3::logError("--cutscene-shot: " + e);
            device->shutdown();
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return 1;
        }
        x3::logInfo("--cutscene-shot: '" + cs.name + "' t=" + std::to_string(cueTime) +
                    " -> " + cutsceneShotPath);

        CinematicScene cin;
        device->beginUploadBatch();
        cin.load(*device, cs);
        device->endUploadBatch();
        cin.applyLook(*device);

        x3::cut::CutscenePlayer player(cs);
        player.onEvent([&](const x3::cut::Event& e, bool) { cin.onEvent(e.name, cs, e.t); });
        player.seek(cueTime);
        cin.update(cs, cueTime);   // backfill the trail/puff state up to the scrub point

        const float t = player.time();
        const x3::cut::CamPose cam = x3::cut::evalCamera(cs, t);
        device->setCamera(cam.pos.x, cam.pos.y, cam.pos.z, cam.yaw, cam.pitch, cam.fov);
        device->setSkyTime(10.0f + t * 0.02f);

        const int kSettle = 8;   // TAA/auto-exposure settle, like the other stills
        for (int i = 0; i < kSettle; ++i) {
            glfwPollEvents();
            if (i == kSettle - 1) device->armCapture(cutsceneShotPath.c_str());
            auto frame = device->beginFrame();
            if (frame.valid) {
                cin.drawWorld(*device, frame, cs, t);
                cin.drawOverlay(*device, frame, cs, t);
            }
            device->endFrame(frame);
        }
        const bool wrote = device->captureFrame(cutsceneShotPath.c_str());
        if (wrote) x3::logInfo("--cutscene-shot: wrote " + cutsceneShotPath);
        else       x3::logError("--cutscene-shot: capture FAILED");

        cin.destroy(*device);
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return wrote ? 0 : 1;
    }

    // ---- Terrain vantage mode (--screenshot-terrain [path.png]) ------------
    // Build the B2 tiled procedural terrain world (terrain meshes + the analytic
    // sky lit by the existing sun), pose a camera up on the hills looking toward
    // the sun so the lit rolling terrain, cast shadows, and sky all read, settle a
    // few frames so the shadow map + LOD register, and capture a PNG. Built
    // entirely through the public render API + a local Jolt world (so the terrain
    // collision path is exercised too) — no game/audio stack. EFLZ Level 1 is an
    // enclosed interior; this is how to SEE + verify the outdoor terrain.
    if (terrainShot) {
        x3::logInfo("--screenshot-terrain: rendering STREAMED terrain world to " + terrainShotPath);

        // B3: this path now exercises the STREAMER under validation. A job system
        // generates tiles async; the focus is SWEPT across the world during the
        // frame loop so stream-IN (createMesh + addStaticMesh) AND stream-OUT
        // (destroyMesh + removeBody) both run inside validated frames, proving the
        // async upload + teardown barriers are validation-clean. The camera trails
        // the swept focus so the final capture is a lit terrain vista.
        std::unique_ptr<x3::jobs::IJobSystem> tjobs(x3::jobs::createJobSystem());
        tjobs->init(0);
        std::unique_ptr<x3::phys::IPhysicsWorld> tphys(x3::phys::createPhysicsWorld());
        tphys->init();
        x3::game::Scene tscene;
        x3::game::TerrainStreamer streamer;
        x3::game::TerrainConfig tcfg;   // 32 m tiles; unbounded (streamed)

        // Turn ON the analytic sky with the SAME sun the shadow pass + mesh.frag
        // use (normalize(0.4,1,0.3)) so the disk sits where the world is lit from.
        x3::rhi::IRenderDevice::SkyParams sp{};
        sp.enabled = true;
        sp.sunDir[0] = 0.4f; sp.sunDir[1] = 1.0f; sp.sunDir[2] = 0.3f;
        sp.sunColor[0] = 1.0f; sp.sunColor[1] = 0.97f; sp.sunColor[2] = 0.92f;
        sp.sunIntensity = 1.0f; sp.haze = 0.5f; sp.exposure = 1.0f;
        device->setSkyParams(sp);

        // Start the focus well away from the origin (proves unbounded coords) and
        // bring up the ring there.
        float fx = -90.0f, fz = -120.0f;
        streamer.init(tscene, *device, *tphys, tjobs.get(), tcfg, fx, fz, /*radius=*/8);

        const float sunYaw   = std::atan2(0.3f, 0.4f);  // toward the sun in XZ
        const float camPitch = -0.16f;                  // ~9deg down: hills + shadows + sky

        const float dt = 1.0f / 60.0f;
        // Render a measured window of frames; report the averaged GPU-pass time
        // (vsync-independent). Sweep the focus +X so tiles stream in/out during the
        // validated loop. The capture is armed on the final frame.
        const int kFrames = 140, kWarmup = 40;
        double sumGpuMs = 0.0; int measured = 0;
        for (int i = 0; i < kFrames; ++i) {
            glfwPollEvents();
            // Sweep the streaming focus across tile boundaries (in/out churn).
            fx += 4.0f;   // ~4 m/frame => crosses a 32 m tile every ~8 frames
            tphys->step(dt);
            streamer.update(tscene, *device, *tphys, fx, fz);

            // Camera trails the focus, elevated + looking down-across toward the sun.
            const float surfY = streamer.heightAt(fx, fz);
            const float camY  = surfY + 18.0f;
            device->setCamera(fx, camY, fz, sunYaw, camPitch, 70.0f);

            if (i == kFrames - 1) device->armCapture(terrainShotPath.c_str());
            auto frame = device->beginFrame();
            if (frame.valid) tscene.render(*device, frame);
            device->endFrame(frame);
            const x3::rhi::RenderStats s = device->stats();
            if (i >= kWarmup) { sumGpuMs += s.gpuFrameMs; ++measured; }
        }
        const bool wrote = device->captureFrame(terrainShotPath.c_str());
        if (wrote) {
            const x3::rhi::RenderStats st = device->stats();
            const double avgGpu = measured ? sumGpuMs / measured : 0.0;
            const double gpuFps = (avgGpu > 1e-6) ? (1000.0 / avgGpu) : 0.0;
            char rb[256];
            std::snprintf(rb, sizeof(rb),
                "--screenshot-terrain: wrote %s | resident=%u (max %u) created=%llu destroyed=%llu "
                "draws=%u tris=%u | GPU=%.3f ms (~%.0f fps GPU-bound)",
                terrainShotPath.c_str(), streamer.residentCount(),
                streamer.maxResidentForRadius(),
                (unsigned long long)streamer.tilesCreated(),
                (unsigned long long)streamer.tilesDestroyed(),
                st.drawCalls, st.triangles, avgGpu, gpuFps);
            x3::logInfo(rb);
        } else x3::logError("--screenshot-terrain: capture FAILED");

        // Tear down the streamer (destroys resident meshes + bodies) before the
        // device/physics, then stop the job system.
        streamer.shutdown(tscene, *device, *tphys);
        tjobs->shutdown();
        tphys->shutdown();
        device->shutdown();
        glfwDestroyWindow(window);
        glfwTerminate();
        return wrote ? 0 : 1;
    }

    // ---- Ocean vantage mode (--screenshot-ocean [path.png]) ----------------
    // Build the procedural terrain world (streamed) + turn ON the animated ocean
    // at a sea level part-way up the height range, so the lower terrain is
    // submerged and the hills rise out of the sea (a real shoreline). The sky +
    // sun are the same the rest of the engine uses, so the water's sky-reflection
    // + sun glint agree with the backdrop. Pose a camera on high ground looking
    // out across the water toward the sun, advance the wave clock a few frames so
    // the surface animates + the shadow map registers, and capture a PNG. Built
    // through the public render API + a local Jolt world, like --screenshot-terrain.
    if (oceanShot) {
        x3::logInfo("--screenshot-ocean: rendering terrain + animated ocean to " + oceanShotPath);

        std::unique_ptr<x3::jobs::IJobSystem> ojobs(x3::jobs::createJobSystem());
        ojobs->init(0);
        std::unique_ptr<x3::phys::IPhysicsWorld> ophys(x3::phys::createPhysicsWorld());
        ophys->init();
        x3::game::Scene oscene;
        x3::game::TerrainStreamer ostream;
        x3::game::TerrainConfig ocfg;   // 32 m tiles; heightScale ~55 m

        const float sunYaw = std::atan2(0.3f, 0.4f);  // toward the sun in XZ

        // Sky (matches the engine sun) so the water reflection + sky agree.
        x3::rhi::IRenderDevice::SkyParams sp{};
        sp.enabled = true;
        sp.sunDir[0] = 0.4f; sp.sunDir[1] = 1.0f; sp.sunDir[2] = 0.3f;
        sp.sunColor[0] = 1.0f; sp.sunColor[1] = 0.97f; sp.sunColor[2] = 0.92f;
        sp.sunIntensity = 1.0f; sp.haze = 0.5f; sp.exposure = 1.0f;
        device->setSkyParams(sp);

        // Ocean: sea level part-way up the terrain height range so valleys flood
        // and hills become shorelines. Tasteful Gerstner defaults.
        const float seaLevel = 14.0f;
        x3::rhi::IRenderDevice::WaterParams wp{};
        wp.enabled = true;
        wp.seaLevel = seaLevel;
        wp.amplitude = 0.6f; wp.steepness = 0.6f; wp.waveLength = 16.0f; wp.speed = 1.0f;
        wp.deepColor[0] = 0.015f; wp.deepColor[1] = 0.06f;  wp.deepColor[2] = 0.10f;
        wp.shallowColor[0] = 0.10f; wp.shallowColor[1] = 0.32f; wp.shallowColor[2] = 0.36f;
        wp.sunDir[0] = 0.4f; wp.sunDir[1] = 1.0f; wp.sunDir[2] = 0.3f;
        wp.specular = 14.0f; wp.fresnel = 0.02f;

        // Find a vantage on high ground: scan a few points for one well above sea
        // level, then back the camera up the sun azimuth so the water spans the
        // frame toward the sun. Bring up the residency ring around that focus.
        float fx = 40.0f, fz = -10.0f;
        ostream.init(oscene, *device, *ophys, ojobs.get(), ocfg, fx, fz, /*radius=*/8);
        // Fill the resident ring fast so the shoreline + a generous expanse of
        // terrain are visible in the single capture (interactive uses the default
        // budget; this is a headless still).
        ostream.setUploadBudget(64);

        const float surfY = ostream.heightAt(fx, fz);
        const float camY  = std::max(surfY, seaLevel) + 10.0f;
        const float camPitch = -0.14f;                  // ~8deg down: water + shore + sky

        const float dt = 1.0f / 60.0f;
        const int kFrames = 220, kWarmup = 120;
        double sumGpuMs = 0.0; int measured = 0;
        for (int i = 0; i < kFrames; ++i) {
            glfwPollEvents();
            ophys->step(dt);
            // The streamer only enqueues the FULL residency ring on a focus tile
            // boundary cross (init seeds just the 3x3). Nudge the focus across one
            // tile on frame 1 to trigger the ring request, then hold it at the
            // vantage so the wide resident set drains in over the warmup window.
            const float focusX = (i == 1) ? (fx + 40.0f) : fx;
            ostream.update(oscene, *device, *ophys, focusX, fz);
            wp.time = (float)i * dt;        // advance the wave animation clock
            device->setWaterParams(wp);
            device->setCamera(fx - 26.0f, camY, fz, sunYaw, camPitch, 70.0f);

            if (i == kFrames - 1) device->armCapture(oceanShotPath.c_str());
            auto frame = device->beginFrame();
            if (frame.valid) oscene.render(*device, frame);
            device->endFrame(frame);
            const x3::rhi::RenderStats s = device->stats();
            if (i >= kWarmup) { sumGpuMs += s.gpuFrameMs; ++measured; }
        }
        const bool wrote = device->captureFrame(oceanShotPath.c_str());
        if (wrote) {
            const x3::rhi::RenderStats st = device->stats();
            const double avgGpu = measured ? sumGpuMs / measured : 0.0;
            const double gpuFps = (avgGpu > 1e-6) ? (1000.0 / avgGpu) : 0.0;
            char rb[256];
            std::snprintf(rb, sizeof(rb),
                "--screenshot-ocean: wrote %s | seaLevel=%.1f resident=%u draws=%u tris=%u | "
                "GPU=%.3f ms (~%.0f fps GPU-bound)",
                oceanShotPath.c_str(), seaLevel, ostream.residentCount(),
                st.drawCalls, st.triangles, avgGpu, gpuFps);
            x3::logInfo(rb);
        } else x3::logError("--screenshot-ocean: capture FAILED");

        ostream.shutdown(oscene, *device, *ophys);
        ojobs->shutdown();
        ophys->shutdown();
        device->shutdown();
        glfwDestroyWindow(window);
        glfwTerminate();
        return wrote ? 0 : 1;
    }

    // ---- AI-action capture mode (--capture-ai [outDir]) --------------------
    // Render the REAL monster combat-AI state machine "in action" to a numbered
    // PNG sequence + an animated GIF, entirely headless/offscreen (no window).
    //
    // Scene: a flat lit ground under the analytic sky, with the engine's sun plus a
    // ring of point lights so the characters read clearly (no dark silhouettes). A
    // fixed player REFERENCE (the AI's target) sits at the arena center; a small
    // squad of enemies runs the actual game AI update (the same MonsterSystem the
    // level uses) so their states + facing are genuine:
    //   * Guard   — advances toward + faces the player (Advance/Attack).
    //   * Drone   — ranged: holds standoff + circles the player (Strafe), facing it.
    //   * Wounded — a Guard we DAMAGE mid-capture (takeMeleeDamage) so it drops below
    //               the retreat threshold and turns away + backs off (Retreat).
    //   * Scout   — a Guard that has LOS, then we CUT its target mid-capture so it
    //               loses sight and walks to the last-known spot, scanning (Search).
    // A fixed 3/4 elevated camera frames all four + the player so advance/circle/
    // retreat read across the sequence. Steps at fixed dt for ~6 s, captures a frame
    // every ~0.2 s, then assembles the GIF + prints a per-phase state log.
    if (captureAi) {
        namespace fs = std::filesystem;
        x3::logInfo("--capture-ai: rendering monster combat-AI demo to " + captureAiDir);
        std::error_code mkec;
        fs::create_directories(captureAiDir, mkec);

        // ---- Physics + scene + lit ground ---------------------------------
        std::unique_ptr<x3::phys::IPhysicsWorld> cphys(x3::phys::createPhysicsWorld());
        cphys->init();
        // Flat collision ground at y=0 (CCW so +Y is solid), large enough for the
        // whole fight (so the LOS / move probes never run off the edge).
        {
            const float h = 80.0f;
            float gv[] = { -h,0,-h,  h,0,-h,  h,0,h,  -h,0,h };
            uint32_t gidx[] = { 0,2,1, 0,3,2 };
            cphys->addStaticMesh(gv, 4, gidx, 6);
        }
        x3::game::Scene cscene;

        // Visible lit ground plane (render): a neutral checker so the floor + the
        // characters' contact shadows-on-flat read. Drawn directly each frame.
        std::vector<x3::rhi::MeshVertex> gvtx; std::vector<uint32_t> gixs;
        x3::prims::makeGroundQuad(/*half=*/80.0f, /*tiles=*/40.0f, gvtx, gixs);
        x3::rhi::MeshHandle groundMesh = device->createMesh(
            gvtx.data(), (uint32_t)gvtx.size(), gixs.data(), (uint32_t)gixs.size());
        auto groundPx = x3::prims::makeCheckerRGBA(64, 8, 120, 130, 120, 64, 72, 66);
        x3::rhi::TextureHandle groundTex = device->createTexture(groundPx.data(), 64, 64, true);
        const float modelGround[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        const float whiteTint[4] = { 1, 1, 1, 1 };

        // A small bright marker box at the player reference so the "target" is
        // visibly in-frame (the AI target itself is a logical stub).
        x3::prims::PrimMesh markerM = x3::prims::makeBox(0.35f, 0.9f, 0.35f, 0, 0.9f, 0, 0.5f);
        x3::rhi::MeshHandle markerMesh = device->createMesh(
            markerM.verts.data(), (uint32_t)markerM.verts.size(),
            markerM.index.data(), (uint32_t)markerM.index.size());
        auto markerPx = x3::prims::makeSolidRGBA(4, 250, 235, 120);   // warm yellow pillar
        x3::rhi::TextureHandle markerTex = device->createTexture(markerPx.data(), 4, 4, true);

        // ---- Sky + lighting: the engine sun PLUS a ring of point lights so the
        // characters are well-lit from several sides (avoid the dim-corner problem;
        // they must NOT read as dark silhouettes). ----
        x3::rhi::IRenderDevice::SkyParams sp{};
        sp.enabled = true;
        sp.sunDir[0] = 0.4f; sp.sunDir[1] = 1.0f; sp.sunDir[2] = 0.3f;
        sp.sunColor[0] = 1.0f; sp.sunColor[1] = 0.97f; sp.sunColor[2] = 0.92f;
        sp.sunIntensity = 1.0f; sp.haze = 0.45f; sp.exposure = 1.0f;
        device->setSkyParams(sp);
        {
            // Bright fill lights placed above + around the action so every enemy is
            // lit regardless of which way it faces. color[] is linear RGB * intensity.
            x3::rhi::PointLight pl[5];
            auto setL = [](x3::rhi::PointLight& l, float x, float y, float z,
                           float r, float g, float b, float range) {
                l.pos[0]=x; l.pos[1]=y; l.pos[2]=z; l.range=range;
                l.color[0]=r; l.color[1]=g; l.color[2]=b;
            };
            setL(pl[0],   0.0f, 7.0f,  4.0f,  5.5f, 5.4f, 5.0f, 44.0f); // overhead key over the action
            setL(pl[1],   9.0f, 4.0f, 10.0f,  4.0f, 3.8f, 3.4f, 40.0f); // +X+Z warm fill
            setL(pl[2],  -9.0f, 4.0f, 10.0f,  3.4f, 3.8f, 4.4f, 40.0f); // -X+Z cool fill
            setL(pl[3],   9.0f, 4.0f, -6.0f,  3.8f, 3.6f, 3.4f, 40.0f); // +X-Z fill
            setL(pl[4],  -9.0f, 4.0f, -6.0f,  3.4f, 3.8f, 4.4f, 40.0f); // -X-Z fill
            device->setPointLights(pl, 5);
        }

        // ---- Player reference (the AI target) at the arena center. ----
        // A trivial always-alive damage sink at a fixed eye/foot, exactly like the
        // --test-ai stub: the enemies track + face IT.
        struct CapTarget final : public x3::game::IDamageSink {
            x3::phys::Vec3 eye{ 0.0f, 1.6f, 0.0f };
            bool takeDamage(int) override { return true; }
            x3::phys::Vec3 damageTargetPos() const override { return eye; }
            bool isAlive() const override { return true; }
        };
        CapTarget player;
        player.eye = x3::phys::Vec3{ 0.0f, 1.6f, 0.0f };
        const x3::phys::Vec3 playerFoot{ 0.0f, 0.0f, 0.0f };

        // ---- The squad. Each is a self-contained MonsterSystem so we can drive its
        // target / LOS independently. We use the rigged animated GLBs when present
        // (marcus_webb for guards, Drone for the flanker); on load failure each
        // falls back to a procedural box, so the capture never breaks. ----
        using x3::game::MonsterSystem;
        using x3::game::MonsterType;
        using x3::game::AiState;
        using x3::game::aiStateName;

        // Prefer the MULTI-CLIP "<name>_anim.glb" (Idle/Walk/Run/Jump) when present
        // so the captured enemies actually WALK/RUN as they move (T1 locomotion
        // blend); fall back to the Idle-only base GLB otherwise (clean checkout).
        auto pickAnimGlb = [](const std::string& dir, const char* base) -> std::string {
            namespace fs = std::filesystem;
            std::string b(base);
            std::string stem = (b.size() > 4 && b.substr(b.size()-4) == ".glb")
                ? b.substr(0, b.size()-4) : b;
            std::string anim = stem + "_anim.glb";
            std::error_code ec;
            if (fs::exists(fs::path(dir) / anim, ec)) return anim;
            return b;
        };
        auto guardTune = [&](){
            MonsterSystem::Tuning t;
            t.type = MonsterType::Guard;
            t.hp = 100; t.chaseSpeed = 3.2f;
            // Brighten the (dark-material) guard mesh so it reads clearly under the
            // arena lights — a warm steel-green so it isn't confused with the others.
            t.tint[0]=1.6f; t.tint[1]=1.7f; t.tint[2]=1.4f; t.tint[3]=1.0f;
            t.damage = 8; t.attackRange = 1.9f; t.attackCooldown = 1.0f; t.attackWindup = 0.25f;
            t.ranged = false;
            t.modelFile = pickAnimGlb(x3::game::riggedGlbRoot(), "marcus_webb.glb");
            t.modelDirOverride = x3::game::riggedGlbRoot();
            t.standUpZtoY = false; t.modelScale = 1.0f;
            return t;
        };
        auto droneTune = [](){
            MonsterSystem::Tuning t;
            t.type = MonsterType::Drone;
            t.hp = 66; t.chaseSpeed = 3.6f;
            t.tint[0]=1.0f; t.tint[1]=1.4f; t.tint[2]=2.0f; t.tint[3]=1.0f;   // bright pale-blue flanker
            t.damage = 5; t.attackRange = 14.0f; t.attackCooldown = 1.4f; t.attackWindup = 0.35f;
            t.ranged = true; t.standoff = 7.0f;
            t.modelFile = "Characters/Drone.glb"; t.modelDirOverride = x3::game::convertedGlbRoot();
            t.standUpZtoY = true; t.modelScale = 1.0f;
            return t;
        };
        auto scoutTune = [&](){
            MonsterSystem::Tuning t;
            t.type = MonsterType::Guard;
            t.hp = 100; t.chaseSpeed = 3.2f;
            t.tint[0]=2.0f; t.tint[1]=1.2f; t.tint[2]=2.2f; t.tint[3]=1.0f; // bright violet scout
            t.damage = 8; t.attackRange = 1.9f; t.attackCooldown = 1.0f; t.attackWindup = 0.25f;
            t.ranged = false;
            t.modelFile = pickAnimGlb(x3::game::riggedGlbRoot(), "marcus_webb.glb");
            t.modelDirOverride = x3::game::riggedGlbRoot();
            t.standUpZtoY = false; t.modelScale = 1.0f;
            return t;
        };
        auto woundedTune = [&](){
            MonsterSystem::Tuning t;
            t.type = MonsterType::Guard;
            t.hp = 100; t.chaseSpeed = 3.0f;
            t.tint[0]=2.2f; t.tint[1]=1.0f; t.tint[2]=0.8f; t.tint[3]=1.0f; // bright wounded red
            t.damage = 8; t.attackRange = 1.9f; t.attackCooldown = 1.0f; t.attackWindup = 0.25f;
            t.ranged = false;
            t.modelFile = pickAnimGlb(x3::game::riggedGlbRoot(), "marcus_webb.glb");
            t.modelDirOverride = x3::game::riggedGlbRoot();
            t.standUpZtoY = false; t.modelScale = 1.0f;
            return t;
        };

        const std::string modelDir = x3::game::riggedGlbRoot();
        MonsterSystem guard, drone, wounded, scout;
        // Place each on its OWN lane around the player so the four behaviours stay
        // spatially distinct (they don't all bunch on the target): the Guard starts
        // FAR so it visibly advances the whole clip; the Drone holds a side standoff
        // and circles; the Wounded sits mid-range then backs away; the Scout sits to
        // the far side and walks off to search once its LOS is cut.
        guard.buildMonsterTuned  (cscene, *device, *cphys, modelDir, x3::phys::Vec3{  -2.0f, 0.0f,  13.0f }, guardTune());
        drone.buildMonsterTuned  (cscene, *device, *cphys, modelDir, x3::phys::Vec3{   7.5f, 0.0f,   2.0f }, droneTune());
        wounded.buildMonsterTuned(cscene, *device, *cphys, modelDir, x3::phys::Vec3{   3.0f, 0.0f,   7.0f }, woundedTune());
        scout.buildMonsterTuned  (cscene, *device, *cphys, modelDir, x3::phys::Vec3{  -7.5f, 0.0f,   4.0f }, scoutTune());

        // ---- Fixed 3/4 elevated camera framing the player + all enemies. ----
        // A true 3/4 view: stand back + offset to +X so depth separates the figures,
        // elevated and looking down-across at the arena center, so advance (toward
        // center), strafe (circling), retreat (away) + search (walking off) all read.
        device->setCamera(11.0f, 8.0f, 15.0f, /*yaw=*/ -2.20f /* look toward -Z, angled -X */,
                          /*pitch=*/ -0.40f, 60.0f);

        // ---- Run the scripted fight. ~6 s at fixed dt; capture every ~0.2 s. ----
        const float dt          = 1.0f / 60.0f;
        const float captureEvery = 0.20f;        // seconds between captured frames
        const float duration     = 6.0f;         // total seconds simulated
        const float woundAtT     = 1.8f;         // damage the "wounded" guard here
        const float loseLosAtT   = 2.6f;         // cut the "scout" target here
        const int   totalSteps   = (int)(duration / dt + 0.5f);
        const int   stepsPerCap  = (int)(captureEvery / dt + 0.5f);

        int   frameNo = 0;
        bool  wounded_done = false;
        // Per-phase log lines (printed all together at the end as the "phase log").
        std::vector<std::string> phaseLog;
        std::vector<std::string> framePaths;

        x3::game::AttackFxFn noFx{};            // no tracer FX needed for the capture
        x3::game::BossPhaseFn noPhase{};
        x3::game::AllyQueryFn noAllies{};

        for (int step = 0; step <= totalSteps; ++step) {
            const float t = step * dt;
            glfwPollEvents();

            // Scripted events: wound one enemy into Retreat; cut another's LOS into
            // Search. Both exercise the REAL damage / targeting paths.
            if (!wounded_done && t >= woundAtT) {
                // Drop it well below the 30% retreat threshold (100 -> 22) so it
                // enters Retreat and turns away. Mirrors a lethal-ish melee combo.
                wounded.takeMeleeDamage(78, cscene, *cphys);
                wounded_done = true;
                x3::logInfo("[capture-ai] t=" + std::to_string(t) +
                            "s: wounded guard takes 78 dmg (hp now " +
                            std::to_string(wounded.hp()) + ")");
            }
            const bool scoutHasTarget = (t < loseLosAtT);   // cut LOS after this

            // Advance the AI for each enemy with the SAME update the game uses.
            // playerFoot is the planar tracking target; player.eye is the LOS end.
            guard.update  (dt, cscene, *cphys, playerFoot, player.eye, &player, noFx, noPhase, noAllies);
            drone.update  (dt, cscene, *cphys, playerFoot, player.eye, &player, noFx, noPhase, noAllies);
            wounded.update(dt, cscene, *cphys, playerFoot, player.eye, &player, noFx, noPhase, noAllies);
            // Scout: pass a live target until loseLosAtT, then null so it loses LOS
            // and falls to Search (it already saw the player, so it searches the
            // last-known spot rather than going Idle immediately).
            if (scoutHasTarget)
                scout.update(dt, cscene, *cphys, playerFoot, player.eye, &player, noFx, noPhase, noAllies);
            else
                scout.update(dt, cscene, *cphys, playerFoot, player.eye, nullptr, noFx, noPhase, noAllies);

            cphys->step(dt);
            // Sync entity translations from physics, THEN re-bake AI facing: the
            // monster update already composed each transform this frame, but a
            // scene.update() would overwrite the 3x3 with identity-rot; the game
            // order is update()-after-scene.update(). Here we don't call
            // scene.update() at all (the AI fully owns each enemy transform), so the
            // baked facing stands.

            // Capture a frame on the cadence (and always the final step).
            const bool doCap = (step % stepsPerCap == 0) || (step == totalSteps);
            char fpath[512];
            if (doCap) {
                std::snprintf(fpath, sizeof(fpath), "%s/frame_%03d.png",
                              captureAiDir.c_str(), frameNo);
                device->armCapture(fpath);
            }
            auto frame = device->beginFrame();
            if (frame.valid) {
                // Lit ground + the player marker pillar.
                device->drawMesh(frame, groundMesh, groundTex, whiteTint, modelGround);
                const float modelMarker[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0,
                                                player.eye.x, 0.0f, player.eye.z, 1 };
                device->drawMesh(frame, markerMesh, markerTex, whiteTint, modelMarker);
                // Scene entities (the monster Entities carry invalid render meshes,
                // so this is mostly a no-op for them) + each monster's own model.
                cscene.render(*device, frame);
                guard.drawMonster(*device, frame, cscene);
                drone.drawMonster(*device, frame, cscene);
                wounded.drawMonster(*device, frame, cscene);
                scout.drawMonster(*device, frame, cscene);
            }
            device->endFrame(frame);

            if (doCap) {
                const bool wrote = device->captureFrame(fpath);
                if (wrote) framePaths.emplace_back(fpath);
                // Record the per-phase state line at this capture instant.
                char line[256];
                std::snprintf(line, sizeof(line),
                    "t=%4.1f  guard=%-7s drone=%-7s wounded=%-7s(hp=%d) scout=%-7s",
                    t, aiStateName(guard.aiState()), aiStateName(drone.aiState()),
                    aiStateName(wounded.aiState()), wounded.hp(),
                    aiStateName(scout.aiState()));
                phaseLog.emplace_back(line);
                ++frameNo;
            }
        }

        // ---- Assemble the animated GIF from the captured PNG frames. -----------
        // Read each PNG back (stb_image, already in the engine TU) and feed it to the
        // public-domain gif.h encoder at ~10 fps (delay = 10 hundredths/frame), looping.
        bool gifOk = false;
        const std::string gifPath = "G:/X3Native/ai_action.gif";
        if (!framePaths.empty()) {
            int gw = 0, gh = 0, gc = 0;
            unsigned char* first = stbi_load(framePaths.front().c_str(), &gw, &gh, &gc, 4);
            if (first && gw > 0 && gh > 0) {
                GifWriter gif{};
                const uint32_t delayCs = 10;   // 10/100 s per frame => ~10 fps, looping
                if (GifBegin(&gif, gifPath.c_str(), (uint32_t)gw, (uint32_t)gh, delayCs)) {
                    GifWriteFrame(&gif, first, (uint32_t)gw, (uint32_t)gh, delayCs);
                    for (size_t i = 1; i < framePaths.size(); ++i) {
                        int w = 0, h = 0, c = 0;
                        unsigned char* px = stbi_load(framePaths[i].c_str(), &w, &h, &c, 4);
                        if (px && w == gw && h == gh) {
                            GifWriteFrame(&gif, px, (uint32_t)gw, (uint32_t)gh, delayCs);
                            stbi_image_free(px);
                        } else if (px) {
                            stbi_image_free(px);
                        }
                    }
                    gifOk = GifEnd(&gif);
                }
                stbi_image_free(first);
            }
        }

        // ---- Print the per-phase state log so the behaviours can be described. ---
        x3::logInfo("================ --capture-ai per-phase AI state log ================");
        for (const auto& l : phaseLog) x3::logInfo(l);
        x3::logInfo("====================================================================");
        {
            char sb[256];
            std::error_code szec;
            uintmax_t gifBytes = gifOk ? fs::file_size(gifPath, szec) : 0;
            std::snprintf(sb, sizeof(sb),
                "--capture-ai: wrote %d PNG frames to %s | GIF %s (%llu bytes)",
                frameNo, captureAiDir.c_str(),
                gifOk ? gifPath.c_str() : "(FAILED)",
                (unsigned long long)gifBytes);
            x3::logInfo(sb);
        }

        device->destroyMesh(groundMesh);
        device->destroyTexture(groundTex);
        device->destroyMesh(markerMesh);
        device->destroyTexture(markerTex);
        cphys->shutdown();
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return (frameNo > 0 && gifOk) ? 0 : 1;
    }

    // ---- Walk-pose capture (--capture-walk [outPath]) ----------------------
    // A focused single-frame proof of the T1 locomotion blend: build ONE close-up
    // animated guard, drive the locomotion blend to a steady WALK speed, settle a
    // fraction of a second so the legs reach a clear mid-stride, capture a PNG.
    // Headless / offscreen, like --screenshot. Uses the multi-clip "*_anim.glb"
    // when present (Walk clip); on a clean checkout (asset absent) the base GLB
    // plays Idle and the capture still succeeds (it just shows the idle pose).
    if (captureWalk) {
        namespace fs = std::filesystem;
        x3::logInfo("--capture-walk: rendering a walking guard to " + captureWalkPath);
        {
            fs::path outp(captureWalkPath);
            std::error_code mkec;
            if (outp.has_parent_path()) fs::create_directories(outp.parent_path(), mkec);
        }

        std::unique_ptr<x3::phys::IPhysicsWorld> wphys(x3::phys::createPhysicsWorld());
        wphys->init();
        {
            const float h = 40.0f;
            float gv[] = { -h,0,-h,  h,0,-h,  h,0,h,  -h,0,h };
            uint32_t gidx[] = { 0,2,1, 0,3,2 };
            wphys->addStaticMesh(gv, 4, gidx, 6);
        }
        x3::game::Scene wscene;

        std::vector<x3::rhi::MeshVertex> gvtx; std::vector<uint32_t> gixs;
        x3::prims::makeGroundQuad(/*half=*/40.0f, /*tiles=*/20.0f, gvtx, gixs);
        x3::rhi::MeshHandle groundMesh = device->createMesh(
            gvtx.data(), (uint32_t)gvtx.size(), gixs.data(), (uint32_t)gixs.size());
        auto groundPx = x3::prims::makeCheckerRGBA(64, 8, 120, 130, 120, 64, 72, 66);
        x3::rhi::TextureHandle groundTex = device->createTexture(groundPx.data(), 64, 64, true);
        const float modelGround[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        const float whiteTint[4] = { 1, 1, 1, 1 };

        x3::rhi::IRenderDevice::SkyParams sp{};
        sp.enabled = true;
        sp.sunDir[0] = 0.4f; sp.sunDir[1] = 1.0f; sp.sunDir[2] = 0.3f;
        sp.sunColor[0] = 1.0f; sp.sunColor[1] = 0.97f; sp.sunColor[2] = 0.92f;
        sp.sunIntensity = 1.0f; sp.haze = 0.40f; sp.exposure = 1.0f;
        device->setSkyParams(sp);
        {
            x3::rhi::PointLight pl[3];
            auto setL = [](x3::rhi::PointLight& l, float x, float y, float z,
                           float r, float g, float b, float range) {
                l.pos[0]=x; l.pos[1]=y; l.pos[2]=z; l.range=range;
                l.color[0]=r; l.color[1]=g; l.color[2]=b;
            };
            setL(pl[0],  0.0f, 4.0f,  3.0f,  5.5f, 5.4f, 5.0f, 30.0f);  // key in front
            setL(pl[1],  3.0f, 3.0f, -2.0f,  3.0f, 3.2f, 3.6f, 30.0f);  // back-right rim
            setL(pl[2], -3.0f, 3.0f,  1.0f,  3.0f, 3.4f, 3.0f, 30.0f);  // left fill
            device->setPointLights(pl, 3);
        }

        // The target the guard advances toward: straight ahead in -Z so it walks
        // toward the camera-facing direction and the stride reads in profile.
        struct WalkTarget final : public x3::game::IDamageSink {
            x3::phys::Vec3 eye{ 0.0f, 1.6f, -12.0f };
            bool takeDamage(int) override { return true; }
            x3::phys::Vec3 damageTargetPos() const override { return eye; }
            bool isAlive() const override { return true; }
        };
        WalkTarget tgt;

        using x3::game::MonsterSystem;
        using x3::game::MonsterType;
        // Prefer the multi-clip animated GLB so the locomotion blend lights up.
        auto pickAnimGlb = [](const std::string& dir, const char* base) -> std::string {
            namespace fsx = std::filesystem;
            std::string b(base);
            std::string stem = (b.size() > 4 && b.substr(b.size()-4) == ".glb")
                ? b.substr(0, b.size()-4) : b;
            std::string anim = stem + "_anim.glb";
            std::error_code ec;
            if (fsx::exists(fsx::path(dir) / anim, ec)) return anim;
            return b;
        };
        MonsterSystem::Tuning wt;
        wt.type = MonsterType::Guard;
        wt.hp = 100; wt.chaseSpeed = 1.5f;   // ~walk speed: the blend lands on WALK
        wt.tint[0]=1.6f; wt.tint[1]=1.7f; wt.tint[2]=1.5f; wt.tint[3]=1.0f;
        wt.damage = 0; wt.attackRange = 0.5f; wt.attackWindup = 0.0f; wt.ranged = false;
        wt.modelFile = pickAnimGlb(x3::game::riggedGlbRoot(), "marcus_webb.glb");
        wt.modelDirOverride = x3::game::riggedGlbRoot();
        wt.standUpZtoY = false; wt.modelScale = 1.0f;

        MonsterSystem guard;
        // Start the guard a little back so it advances (walks) toward the target the
        // whole time, never reaching attack range (damage 0, tiny attackRange). It
        // ends near z~2.8 after the settle below; the camera frames that spot.
        const x3::phys::Vec3 guardStart{ 0.0f, 0.0f, 4.0f };
        guard.buildMonsterTuned(wscene, *device, *wphys, x3::game::riggedGlbRoot(),
                                guardStart, wt);
        x3::logInfo(std::string("--capture-walk: guard usingRealModel=") +
                    (guard.usingRealModel() ? "1" : "0"));

        // Close, slightly-elevated front-3/4 camera AIMED at the guard's expected
        // mid-capture torso (~(0, 0.9, 2.8)). Camera at (3, 1.6, 5.2) looking back
        // toward -X/-Z: yaw = atan2(dz,dx) of (look - cam), pitch from the rise.
        {
            const float cx = 3.0f, cy = 1.6f, cz = 5.4f;
            const float lx = 0.0f, ly = 0.95f, lz = 2.8f;
            const float ddx = lx - cx, ddy = ly - cy, ddz = lz - cz;
            const float dlen = std::sqrt(ddx*ddx + ddy*ddy + ddz*ddz);
            const float yaw = std::atan2(ddz, ddx);
            const float pitch = std::asin(dlen > 1e-4f ? (ddy / dlen) : 0.0f);
            device->setCamera(cx, cy, cz, yaw, pitch, 50.0f);
        }

        // Step ~1.5 s so the guard accelerates into a steady WALK and the legs reach
        // a clear mid-stride; capture the final frame.
        const float dt = 1.0f / 60.0f;
        x3::game::AttackFxFn noFx{}; x3::game::BossPhaseFn noPhase{}; x3::game::AllyQueryFn noAllies{};
        const int steps = 90;
        bool wrote = false;
        for (int step = 0; step <= steps; ++step) {
            glfwPollEvents();
            guard.update(dt, wscene, *wphys, tgt.eye /*planar*/, tgt.eye /*eye*/,
                         &tgt, noFx, noPhase, noAllies);
            wphys->step(dt);
            const bool last = (step == steps);
            if (last) device->armCapture(captureWalkPath.c_str());
            auto frame = device->beginFrame();
            if (frame.valid) {
                device->drawMesh(frame, groundMesh, groundTex, whiteTint, modelGround);
                wscene.render(*device, frame);
                guard.drawMonster(*device, frame, wscene);
            }
            device->endFrame(frame);
            if (last) wrote = device->captureFrame(captureWalkPath.c_str());
        }
        x3::logInfo(std::string("--capture-walk: aiState=") +
                    x3::game::aiStateName(guard.aiState()) +
                    (wrote ? "  wrote " + captureWalkPath : "  CAPTURE FAILED"));

        device->destroyMesh(groundMesh);
        device->destroyTexture(groundTex);
        wphys->shutdown();
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return wrote ? 0 : 1;
    }

    // ---- Foot-IK capture (--screenshot-footik [outPath]) -------------------
    // Stand ONE rigged character on a SLOPE + STEP with foot-IK ON: the feet
    // raycast down into the local physics world, plant on the surface (+ align to
    // the ground normal), the pelvis lowers so both feet reach, and we capture a
    // single PNG. A side-by-side reference (IK OFF) is also written so the planted
    // vs floating difference is obvious. Self-contained (no game stack): loads the
    // rigged GLB directly + drives a Skinner with the new setFootIk() hook.
    if (captureFootIk) {
        namespace fs = std::filesystem;
        x3::logInfo("--screenshot-footik: grounding a character on a slope/step -> " + captureFootIkPath);
        { fs::path outp(captureFootIkPath); std::error_code mkec;
          if (outp.has_parent_path()) fs::create_directories(outp.parent_path(), mkec); }

        std::unique_ptr<x3::phys::IPhysicsWorld> fphys(x3::phys::createPhysicsWorld());
        fphys->init();

        // Build a SLOPED + STEPPED ground as a static collision mesh AND a matching
        // render mesh, so the raycast hits exactly what we see. A gentle ~12deg ramp
        // descending toward -X with a small raised step block under one foot.
        std::vector<x3::rhi::MeshVertex> gv; std::vector<uint32_t> gi;
        auto pushQuad = [&](float a[3], float b[3], float c[3], float d[3]) {
            uint32_t base = (uint32_t)gv.size();
            float e1[3] = { b[0]-a[0], b[1]-a[1], b[2]-a[2] };
            float e2[3] = { c[0]-a[0], c[1]-a[1], c[2]-a[2] };
            float nx = e1[1]*e2[2]-e1[2]*e2[1], ny = e1[2]*e2[0]-e1[0]*e2[2], nz = e1[0]*e2[1]-e1[1]*e2[0];
            float nl = std::sqrt(nx*nx+ny*ny+nz*nz); if (nl>1e-6f){nx/=nl;ny/=nl;nz/=nl;}
            auto add = [&](float* p, float u, float v){ x3::rhi::MeshVertex mv{};
                mv.pos[0]=p[0];mv.pos[1]=p[1];mv.pos[2]=p[2]; mv.normal[0]=nx;mv.normal[1]=ny;mv.normal[2]=nz;
                mv.uv[0]=u;mv.uv[1]=v; gv.push_back(mv); };
            add(a,0,0); add(b,1,0); add(c,1,1); add(d,0,1);
            gi.push_back(base+0); gi.push_back(base+1); gi.push_back(base+2);
            gi.push_back(base+0); gi.push_back(base+2); gi.push_back(base+3);
        };
        // A SLOPE running along the Z axis (descends toward -Z, the camera side) so the
        // character — facing the camera with feet spread in Z — has its front foot on
        // lower ground than its back foot. slope ~tan(18deg)=0.33; spans X[-4,4], Z[-4,4].
        // The character's TWO feet then land at clearly different heights -> the leg
        // analytic solve + the lower-foot-governed pelvis drop both read in the capture.
        const float slope = 0.22f;
        auto groundY = [&](float /*x*/, float z){ return slope * z; };  // height under (x,z)
        {
            float a[3]={-4,groundY(-4,-4),-4}, b[3]={4,groundY(4,-4),-4},
                  c[3]={4,groundY(4, 4), 4}, d[3]={-4,groundY(-4,4), 4};
            pushQuad(a,b,c,d);
        }
        // A single raised STEP block straddling the character's BACK (+Z) foot so one
        // foot is on the step and the other on the slope below — the classic foot-IK
        // stair case. Top at +0.16 above the slope, X[-1.5,1.5], Z[0.15,2.5].
        const float stepZ0=0.15f, stepZ1=2.5f, stepX0=-1.5f, stepX1=1.5f, stepUp=0.16f;
        auto stepTopY = [&](float z){ return groundY(0,z) + stepUp; };
        {
            float a[3]={stepX0,stepTopY(stepZ0),stepZ0}, b[3]={stepX1,stepTopY(stepZ0),stepZ0},
                  c[3]={stepX1,stepTopY(stepZ1),stepZ1}, d[3]={stepX0,stepTopY(stepZ1),stepZ1};
            pushQuad(a,b,c,d);
            // step riser (facing -Z toward the camera) so the step reads in profile.
            float ra[3]={stepX0,groundY(0,stepZ0),stepZ0}, rb[3]={stepX1,groundY(0,stepZ0),stepZ0},
                  rc[3]={stepX1,stepTopY(stepZ0),stepZ0}, rd[3]={stepX0,stepTopY(stepZ0),stepZ0};
            pushQuad(ra,rb,rc,rd);
        }
        // Collide the combined ground: addStaticMesh wants tightly-packed xyz floats.
        std::vector<float> gpos(gv.size()*3);
        for (size_t i=0;i<gv.size();++i){ gpos[i*3]=gv[i].pos[0]; gpos[i*3+1]=gv[i].pos[1]; gpos[i*3+2]=gv[i].pos[2]; }
        fphys->addStaticMesh(gpos.data(), (uint32_t)gv.size(), gi.data(), (uint32_t)gi.size());
        x3::rhi::MeshHandle groundMesh = device->createMesh(gv.data(), (uint32_t)gv.size(), gi.data(), (uint32_t)gi.size());
        auto groundPx = x3::prims::makeCheckerRGBA(64, 8, 120, 130, 120, 64, 72, 66);
        x3::rhi::TextureHandle groundTex = device->createTexture(groundPx.data(), 64, 64, true);
        const float modelGround[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        const float whiteTint[4] = { 1, 1, 1, 1 };

        x3::rhi::IRenderDevice::SkyParams sp{};
        sp.enabled=true;
        // Sun toward the CAMERA side (-Z) + up, so the front of the character is lit
        // (a +Z sun backlights it into a silhouette from this -Z camera).
        sp.sunDir[0]=0.25f; sp.sunDir[1]=0.85f; sp.sunDir[2]=-0.5f;
        sp.sunColor[0]=1.0f; sp.sunColor[1]=0.97f; sp.sunColor[2]=0.92f;
        sp.sunIntensity=1.0f; sp.haze=0.35f; sp.exposure=1.2f;
        device->setSkyParams(sp);
        { x3::rhi::PointLight pl[3];
          auto setL=[](x3::rhi::PointLight& l,float x,float y,float z,float r,float g,float b,float rng){
            l.pos[0]=x;l.pos[1]=y;l.pos[2]=z;l.range=rng;l.color[0]=r;l.color[1]=g;l.color[2]=b; };
          // Key light on the CAMERA side (-Z) low so the front + the legs/feet read
          // (the camera looks toward +Z, so a +Z light would only backlight).
          setL(pl[0], 0.6f,2.2f,-3.0f, 7.0f,6.8f,6.4f, 30.0f);   // front key (camera side)
          setL(pl[1], 2.5f,2.5f,-1.0f, 3.0f,3.0f,3.4f, 30.0f);   // front-right fill
          setL(pl[2],-2.5f,2.5f,-1.0f, 3.0f,3.2f,3.0f, 30.0f);   // front-left fill
          device->setPointLights(pl, 3); }

        // Load the rigged character with the REAL device so its skinned meshes upload.
        auto pickAnimGlb = [](const std::string& dir, const char* base) -> std::string {
            namespace fsx = std::filesystem; std::string b(base);
            std::string stem = (b.size()>4 && b.substr(b.size()-4)==".glb") ? b.substr(0,b.size()-4) : b;
            std::error_code ec2; std::string anim = stem + "_anim.glb";
            if (fsx::exists(fsx::path(dir)/anim, ec2)) return anim;
            return b;
        };
        const std::string rigDir = x3::game::riggedGlbRoot();
        std::string rigFile = pickAnimGlb(rigDir, "chief_martinez.glb");
        std::unique_ptr<x3::asset::IAssetSource> asrc(x3::asset::createAssetSource());
        asrc->mountDir(rigDir, 0);
        std::unique_ptr<x3::asset::IModelLoader> mloader(x3::asset::createModelLoader(device.get(), asrc.get()));
        x3::asset::Model cmodel = mloader->load(rigFile);
        auto drawables = x3::asset::makeDrawables(cmodel);

        x3::anim::Skinner sk;
        bool skinnable = cmodel.ok && sk.bind(cmodel);
        x3::logInfo(std::string("--screenshot-footik: rig=") + rigFile +
                    " ok=" + (cmodel.ok?"1":"0") + " skinnable=" + (skinnable?"1":"0") +
                    " footIkResolved=" + (sk.footIkResolved()?"1":"0"));
        if (skinnable) {
            x3::logInfo(std::string("--screenshot-footik: legs L=(") +
                std::string(sk.footIkBoneName(0,0)) + "," + std::string(sk.footIkBoneName(0,1)) + "," +
                std::string(sk.footIkBoneName(0,2)) + ") R=(" + std::string(sk.footIkBoneName(1,0)) + "," +
                std::string(sk.footIkBoneName(1,1)) + "," + std::string(sk.footIkBoneName(1,2)) +
                ") pelvis=" + std::string(sk.footIkBoneName(0,3)));
            int idle = sk.findClip({ "idle","stand","breath","loop" });
            int walk = sk.findClip({ "walk" });
            int run  = sk.findClip({ "run","jog","sprint" });
            sk.setLocomotionClips(idle, walk, run, 1.5f, 4.0f);
            sk.setLocomotion01(0.0f);   // standing -> idle, so the feet stay planted
        }

        // Character placement: stand at the foot of the step on the slope, facing the
        // camera (default -Z facing). The model origin sits at the slope height under
        // its position; foot-IK then conforms each foot to the slope/step it's over.
        // worldFromModel = this placement matrix.
        const float charX = 0.0f, charZ = -0.1f;
        const float charY = groundY(0, charZ);   // model origin on the slope
        float placement[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, charX, charY, charZ, 1 };

        // Build the broadphase + settle the static world so the foot rays hit (mirrors
        // the physics self-test, which steps twice before relying on rayCast).
        fphys->optimizeBroadphase();
        for (int i = 0; i < 2; ++i) fphys->step(1.0f/60.0f);

        // Ground raycast callback bridging the Skinner to the physics world.
        struct RayCtx { x3::phys::IPhysicsWorld* phys; };
        RayCtx rctx{ fphys.get() };
        x3::anim::Skinner::GroundRay gray;
        gray.user = &rctx;
        gray.fn = [](const float o[3], const float d[3], float maxD,
                     float hit[3], float n[3], void* u) -> bool {
            auto* c = (RayCtx*)u;
            x3::phys::Vec3 org{ o[0], o[1], o[2] };
            x3::phys::Vec3 dir{ d[0], d[1], d[2] };
            x3::phys::RayHit rh = c->phys->rayCast(org, dir, maxD, x3::phys::Layer::Static);
            if (!rh.hit) return false;
            hit[0]=rh.point.x; hit[1]=rh.point.y; hit[2]=rh.point.z;
            n[0]=rh.normal.x; n[1]=rh.normal.y; n[2]=rh.normal.z;
            return true;
        };

        // Camera: low + close 3/4 view from the front-right (-Z, +X) looking slightly
        // down at the legs/feet so the slope + step + planting all read in profile.
        {
            const float cx=2.4f, cy=1.0f, cz=-2.6f;     // front-right, low
            const float lx=0.0f, ly=0.30f, lz=0.4f;     // look at the lower legs/feet
            const float dx=lx-cx, dy=ly-cy, dz=lz-cz;
            const float dl=std::sqrt(dx*dx+dy*dy+dz*dz);
            device->setCamera(cx,cy,cz, std::atan2(dz,dx), std::asin(dl>1e-4f?dy/dl:0.0f), 45.0f);
        }

        const float dt = 1.0f/60.0f;
        // Render a still with IK either ON or OFF. Settle the blend + IK a moment so the
        // smoothed weights + pelvis converge, then capture the final frame.
        auto renderStill = [&](bool ikOn, const std::string& path) -> bool {
            if (skinnable) {
                if (ikOn) sk.setFootIk(true, gray, placement);
                else      { x3::anim::Skinner::GroundRay none{}; sk.setFootIk(false, none, placement); }
            }
            const int steps = 90;
            bool wrote=false;
            for (int s=0; s<=steps; ++s) {
                glfwPollEvents();
                if (skinnable) sk.applyLocomotion(cmodel, *device, dt);
                fphys->step(dt);
                const bool last = (s==steps);
                if (last) device->armCapture(path.c_str());
                auto frame = device->beginFrame();
                if (frame.valid) {
                    device->drawMesh(frame, groundMesh, groundTex, whiteTint, modelGround);
                    for (const auto& dr : drawables) {
                        float fin[16];
                        x3::asset::mulMat4(placement, dr.nodeTransform, fin);
                        // Brighten the (very dark tactical-gear) material so the legs/feet
                        // read against the ground in the capture (visual only).
                        float tint[4] = { dr.baseColorFactor[0]*2.2f + 0.25f,
                                          dr.baseColorFactor[1]*2.2f + 0.25f,
                                          dr.baseColorFactor[2]*2.2f + 0.25f, dr.baseColorFactor[3] };
                        device->drawMesh(frame, x3::rhi::MeshHandle{ dr.meshId },
                                         x3::rhi::TextureHandle{ dr.baseColorTexId },
                                         tint, fin);
                    }
                }
                device->endFrame(frame);
                if (last) wrote = device->captureFrame(path.c_str());
            }
            return wrote;
        };

        // OFF (reference) first, then ON last so the final logged weights reflect the
        // engaged IK. Reference path is "<stem>_noik.png".
        std::string offPath = captureFootIkPath;
        {
            auto dot = offPath.find_last_of('.');
            offPath = (dot==std::string::npos) ? offPath + "_noik" : offPath.substr(0,dot) + "_noik" + offPath.substr(dot);
        }
        bool wroteOff = renderStill(false, offPath);
        bool wroteOn  = renderStill(true,  captureFootIkPath);
        x3::logInfo(std::string("--screenshot-footik: IK-ON pelvisDrop=") + std::to_string(sk.footIkPelvisDrop()) +
                    " wL=" + std::to_string(sk.footIkLegWeight(0)) + " wR=" + std::to_string(sk.footIkLegWeight(1)));
        x3::logInfo(std::string("--screenshot-footik: ON -> ") + (wroteOn?captureFootIkPath:"FAILED") +
                    " | OFF -> " + (wroteOff?offPath:"FAILED"));

        mloader->unload(cmodel);
        device->destroyMesh(groundMesh);
        device->destroyTexture(groundTex);
        fphys->shutdown();
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return wroteOn ? 0 : 1;
    }

    // ======================================================================
    // ---- SELF-CONTAINED `--world` HOSTS (#28 deep monolith split) ----------
    // The destruct / physjoint / ragdoll / drive|boat|fly / club / showroom /
    // valley / cliffs / streamed hosts were lifted VERBATIM into
    // app/world_hosts/*.cpp behind dispatchWorldHost(). Populate the HostContext
    // (a near-1:1 mirror of the shared main() locals) and dispatch; a >=0 result
    // is this program's exit code, -1 = "no discrete host matched" so boot
    // continues into the default host (the shared interactive render loop).
    // ======================================================================
    {
        x3::apphost::HostContext _hc;
        _hc.device           = device.get();
        _hc.window           = window;
        _hc.worldMode        = worldMode;
        _hc.headless         = headless;
        _hc.W                = W;
        _hc.H                = H;
        _hc.screenshot       = screenshot;
        _hc.screenshotPath   = screenshotPath;
        _hc.screenshotSettle = screenshotSettle;
        _hc.shotCamOverride  = shotCamOverride;
        for (int _k = 0; _k < 5; ++_k) _hc.shotCam[_k] = shotCam[_k];
        _hc.stressCount      = stressCount;
        _hc.bench            = bench;
        _hc.benchFrames      = benchFrames;
        _hc.ddgiForce        = ddgiForce;
        _hc.shotWorldMap     = shotWorldMap;
        _hc.wsBudgetMs       = wsBudgetMs;
        _hc.wsLookaheadS     = wsLookaheadS;
        _hc.destructShot     = destructShot;     _hc.destructShotPath = destructShotPath;
        _hc.carShot          = carShot;          _hc.carShotDir       = carShotDir;
        _hc.showroomFpShot   = showroomFpShot;   _hc.showroomFpShotPath = showroomFpShotPath;
        _hc.showroomRagdollShot = showroomRagdollShot; _hc.showroomRagdollShotPath = showroomRagdollShotPath;
        _hc.showroomDeckShot = showroomDeckShot; _hc.showroomDeckShotPath = showroomDeckShotPath;
        _hc.showroomElevShot = showroomElevShot; _hc.showroomElevShotPath = showroomElevShotPath;
        _hc.showroomStairShot = showroomStairShot; _hc.showroomStairShotPath = showroomStairShotPath;
        _hc.showroomFloor2Shot = showroomFloor2Shot; _hc.showroomFloor2ShotPath = showroomFloor2ShotPath;
        _hc.showroomDoorShot = showroomDoorShot; _hc.showroomDoorShotPath = showroomDoorShotPath;
        _hc.showroomStrutsShot = showroomStrutsShot; _hc.showroomStrutsShotPath = showroomStrutsShotPath;
        _hc.showroomGalleryShot = showroomGalleryShot; _hc.showroomGalleryShotPath = showroomGalleryShotPath;
        _hc.showroomCivShot  = showroomCivShot;  _hc.showroomCivShotPath = showroomCivShotPath;
        _hc.perfshopShot     = perfshopShot;     _hc.perfshopShotDir  = perfshopShotDir;
        _hc.ecologyShot      = ecologyShot;      _hc.ecologyShotPath  = ecologyShotPath;
        _hc.crowdShot        = crowdShot;        _hc.crowdShotPath    = crowdShotPath;
        int _hostRc = x3::apphost::dispatchWorldHost(_hc);
        if (_hostRc >= 0) return _hostRc;
    }

    // ---- Asset source (stub until D5) ----
    std::unique_ptr<x3::asset::IAssetSource> assets(x3::asset::createAssetSource());
    assets->mountPak("base.x3pak", 0);  // stub: logs not-implemented for now

    // ---- Audio system (M9 / miniaudio) ----
    // init() is GRACEFUL: on a machine with no audio device it logs a warning and
    // runs silently (all play calls become no-ops) — never crashes. We load REAL
    // purchased WAV/music resolved per-machine via resolveAudio() (laptop D: mirror
    // or the other machines' G: packs); nothing is copied into the public repo.
    // Missing files load() to invalid handles -> silent.
    // Joined from the boot-time async bring-up when it was launched (overlapped
    // with the Vulkan device init above); built synchronously here otherwise.
    // The `audio` unique_ptr itself is move-constructed from bootAudio just below.
    // ---- RT ACOUSTICS (snd_rtacoustics): audio rays through the render TLAS.
    // The brain (RtAcoustics) fires per-emitter occlusion fans + a periodic
    // listener room probe through IRenderDevice::traceAudioRays (the same scene
    // TLAS the RT screen effects trace); the mixer applies the result as a
    // volume duck + per-voice lowpass + room reverb. Self-gating: on a non-RT
    // device traceAudioRays returns false and the whole chain stays inert.
    static_assert(sizeof(x3::audio::AcousticRay) == sizeof(x3::rhi::IRenderDevice::AudioRay),
                  "AcousticRay must match IRenderDevice::AudioRay (memcpy bridge)");
    x3::audio::RtAcoustics rtAcoustics;
    rtAcoustics.setTracer(
        +[](void* user, const x3::audio::AcousticRay* rays, int count) -> bool {
            auto* dev = static_cast<x3::rhi::IRenderDevice*>(user);
            return dev->traceAudioRaysSubmit(
                reinterpret_cast<const x3::rhi::IRenderDevice::AudioRay*>(rays), count);
        },
        +[](void* user, float* outHitT, int capacity) -> int {
            auto* dev = static_cast<x3::rhi::IRenderDevice*>(user);
            return dev->traceAudioRaysHarvest(outHitT, capacity);
        },
        device.get());
    bool  rtaHooked = false;        // occlusion provider currently installed?
    float rtaDebugTimer = 0.0f;     // snd_rta_debug log cadence
    // Concrete asset picks (see docs/ASSET_INVENTORY.md). Pack-relative paths with
    // graceful fallback: a missing/undecodable file -> invalid handle -> the
    // corresponding event is simply silent (logged once at load).
    BootAudio bootAudio = bootAudioFut.valid() ? bootAudioFut.get() : makeBootAudio();
    std::unique_ptr<x3::audio::IAudioSystem> audio(std::move(bootAudio.audio));
    const x3::audio::SoundHandle sndGun    = bootAudio.gun;
    const x3::audio::SoundHandle sndDoor   = bootAudio.door;
    const x3::audio::SoundHandle sndPickup = bootAudio.pickup;
    const x3::audio::SoundHandle sndDeath  = bootAudio.death;
    // Footsteps reuse the gunshot WAV pitched down + quiet (no dedicated footstep
    // WAV in the inventory). It reads as a soft step; replace with a real footstep
    // SFX later if one is added to the pack.
    const x3::audio::SoundHandle sndStep = sndGun;
    // Resolved path for the looping music/ambient bed (started after the world is
    // built, below). Spaceship-ambience-style sci-fi action loop.
    const std::string kMusicPath = x3::game::resolveAudio(
        "Sci-Fi Music Pack 1/Loops/SMP1_LOOP_Zero8 _1.wav");

    x3::boot::mark("audio init + sfx loads");

    // ---- Physics world (M3 / Jolt) ----
    std::unique_ptr<x3::phys::IPhysicsWorld> physics(x3::phys::createPhysicsWorld());
    if (!physics->init()) {
        x3::logError("physics world init failed");
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    // ---- EFLZ LOADING SCREEN (Task #49) — shown on cold boot AND world switch,
    // replacing the bare/black gap while the device/assets/audio/physics/world
    // come up. The procedural background texture is created now (device is up);
    // each chunk of boot work below pushes a real progress STEP (0->1). Headless
    // safe: render() is a no-op without a valid frame, so --smoketest never blocks.
    // The bar is also DRAWN under the headless smoketest (a few frames below) so
    // the 2D draw path is validation-checked. ----
    x3::boot::mark("physics init");
    // BOOT-TIME upload batching (docs/BOOT_TIME.md): from here through the end of
    // the world build, every createMesh/createTexture records into ONE shared
    // upload command buffer instead of a blocking submit each (~8 ms fixed cost x
    // ~2000 calls was the 16+ s world-build pole). beginFrame (the loading-screen
    // frames) and any one-shot GPU op auto-flush, so visibility semantics are
    // identical. Ended right after the last build-time GLB load below.
    device->beginUploadBatch();

    // BOOT MANIFEST — parallel GLB preload (docs/BOOT_TIME.md). Everything the
    // cell worlds (default Level 1 / elevator / canonlevel / intro) load at build
    // time, warmed CONCURRENTLY into the loader's process-wide model/texture
    // caches before the serial world build below — which then takes cache hits
    // instead of doing 25+ serial parse+decode passes (19x marcus_webb on a
    // default boot). Missing files are skipped silently (superset manifest); the
    // demo/sandbox worlds skip the warmup entirely (they load their own art).
    // (The async GLB warmup was kicked MID device-init via desc.onUploadReady and
    // is joined right before the world build below.)

    x3::game::LoadingScreen loading;
    loading.init(*device);
    loading.step(x3::game::LoadStep::DeviceReady, "RENDER DEVICE");
    // Helper: render ONE loading frame (poll + beginFrame + draw + endFrame) at the
    // current progress, advancing the tip/fade clock by dt. No-op draw when there is
    // no real frame; safe headless. Used both during the build (a couple of progress
    // frames so the human SEES the bar move) and for the fade-out hand-off.
    const float kLoadDt = 1.0f / 60.0f;
    auto loadingFrame = [&](float dt) {
        glfwPollEvents();
        auto lf = device->beginFrame();
        loading.render(*device, lf, dt);
        device->endFrame(lf);
    };

    // Asset mount + audio init already happened above; reflect them on the bar.
    loading.step(x3::game::LoadStep::AssetsMounted, "MOUNTING ASSETS");
    loading.step(x3::game::LoadStep::AudioReady, "LOADING AUDIO");
    loading.step(x3::game::LoadStep::PhysicsReady, "PHYSICS WORLD");
    loadingFrame(kLoadDt);
    x3::boot::mark("loading frame (pre-build, IBL bake)");

    // ---- INTRO COLD-OPEN (prologue lead-in). Before the cell is built, play the scripted
    // cold-open: Jake flies his ship through space, a larger enemy ship shoots him down with an
    // energy pulse, the screen whites out on the crash, then "6 MONTHS LATER" -> hand off to the
    // cell where he wakes a captive. This is the canon reason he starts the game in a detention
    // cell (shot down + CAPTURED). Gated so it ONLY runs as the windowed lead-in for the cell
    // worlds (default Level 1 / elevator / canonlevel) or when explicitly requested with
    // `--world intro`; headless (smoketest/screenshot) + the sandbox/demo worlds skip it entirely
    // (runIntro is also a no-op when `window` is null, a second safety net). The intro renders on
    // the public 2D path only — it spawns NO meshes/lights/physics, so there is nothing to leak
    // and the cell build that follows is byte-for-byte unchanged.
    {
        const bool introCellWorld = (worldMode == "level1") || (worldMode == "elevator") ||
                                    (worldMode == "canonlevel") || (worldMode == "intro");
        // --test-boottime skips the cold-open: the intro is CONTENT the player watches
        // (a skippable cinematic), not boot work — the gate measures the machine.
        if (window && introCellWorld && !testBootTime && !skipIntro) {
            // ONCE PER SAVE: the intro_complete StoryFlag gates replays. An explicit
            // --cutscene <file> always plays (the authoring loop); intro_play (console)
            // replays mid-game. The flag is set by the cutscene's endState event, so
            // a SKIP still latches it (skip fires endState events by contract).
            x3::cut::StoryFlags storyFlags;
            storyFlags.load(x3::cut::defaultStoryFlagsPath());
            const bool seenIntro = storyFlags.has(x3::cut::kFlagIntroComplete);
            if (!seenIntro || !cutsceneFile.empty()) {
                const std::string csPath = !cutsceneFile.empty()
                    ? cutsceneFile
                    : x3::game::assetRoot() + "/cutscenes/cold_open.cutscene.json";
                x3::cut::Cutscene coldOpen;
                std::vector<std::string> csErrs;
                bool ranFilm = false;
                if (x3::cut::loadCutsceneFile(csPath, coldOpen, csErrs)) {
                    const bool completed = runCutsceneWindowed(
                        *device, window, audio.get(), coldOpen, cueTime,
                        [&](const std::string& ev) {
                            if (ev == x3::cut::kFlagIntroComplete) {
                                storyFlags.set(x3::cut::kFlagIntroComplete);
                                storyFlags.save(x3::cut::defaultStoryFlagsPath());
                            }
                        });
                    if (!completed) {
                        // Window closed mid-film — exit cleanly (mirrors a window-close quit).
                        physics->shutdown();
                        device->shutdown();
                        if (window) glfwDestroyWindow(window);
                        glfwTerminate();
                        return 0;
                    }
                    ranFilm = true;
                    // SEAMLESS WAKE: the film ends on black ("SIX MONTHS LATER"). Flip the
                    // loading screen to BLACKOUT so the cell build stays black, then the
                    // hand-off fade is the slow first-person wake in the cell — control is
                    // live underneath it, exactly like a normal spawn.
                    loading.setBlackout(true);
                } else {
                    for (const auto& e : csErrs) x3::logError("[cutscene] cold open: " + e);
                    x3::logError("[cutscene] cold open failed to load — falling back to the legacy 2D intro");
                }
                if (!ranFilm) {
                    // Resilience: the legacy phase-machine intro still tells the story.
                    if (!x3::intro::runIntro(*device, window)) {
                        physics->shutdown();
                        device->shutdown();
                        if (window) glfwDestroyWindow(window);
                        glfwTerminate();
                        return 0;
                    }
                }
                x3::boot::mark("intro cold-open (content)");
            } else {
                x3::logInfo("[cutscene] intro_complete StoryFlag set — skipping the cold open "
                            "(replay with intro_play or --cutscene)");
            }
        }
    }

    // ---- Build EFLZ Level 1 "Awakening" into the scene. The vertical slice now
    // BECOMES Level 1: the Level1Game controller owns the graybox geometry, doors
    // A-E, the armory pistol pickup, the checkpoint guards, the strength/arena/
    // elevator trigger volumes, the objective list, and the corridor/Martinez
    // enemies that spawn on their beats. (See app/level1_game.* + the spec §2/§3.)
    // ---- World selection (additive): the default is the interior Level 1. With
    // `--world terrain` the host instead builds the B2 outdoor TILED TERRAIN world
    // (terrain + sky + sun) so the human can WALK the hills. Level 1 stays the
    // default and is built/ticked exactly as before when not in terrain mode. The
    // Level1Game object is still constructed (so the screenshot/bench/smoketest
    // early-return paths are unchanged) but is only BUILT + ticked for Level 1. ----
    // `--world ocean` is the terrain world with the animated ocean turned ON (an
    // outdoor sea scene). It reuses the entire streamed-terrain path (so terrain +
    // sky + streaming all work identically) and additionally enables water at a
    // sea level; the only ocean-specific bit is the per-frame setWaterParams below.
    const bool oceanWorld   = (worldMode == "ocean");
    const bool terrainWorld = (worldMode == "terrain") || oceanWorld;
    // --world elevator: a souped-up-elevator showcase. It reuses the Level-1 build
    // path (the strata/disco elevator lives in the Level-1 spire shaft), then logs
    // a hint + spawns the player AT the elevator so you can ride it and enter the
    // 1127 disco code right away. Any unrecognized --world value also lands here
    // (Level 1), so this is purely additive guidance.
    const bool elevatorWorld = (worldMode == "elevator");
    // --world canonlevel: build Floor 1 from the OWNER'S CANONICAL LevelArchitect data
    // (level_loader.*) instead of the hand-coded tower, and run the PER-ROOM OCCLUSION
    // CULL (portal PVS) so only the player's current room + its doorway-reachable
    // neighbours render. This is the data-driven path + the perf payoff: the smoketest
    // under this mode reports objs/tris FAR below the full tower's 8604/49.6M. The full
    // 7-floor tower build (Level1Game + Spire*) is SKIPPED in this mode; the legacy build
    // remains the default for every other path (so all existing flags are unchanged).
    // `--world intro` is the canon flow: after the cold-open prologue (played above), it hands
    // off to the SAME canonical Floor-1 cell start as `--world canonlevel` (where Jake wakes a
    // captive). So `intro` aliases the canon build here.
    const bool canonWorld = (worldMode == "canonlevel") || (worldMode == "intro");
    // --world fromdoc: boot DIRECTLY into the EDITOR's LevelDoc JSON (docWorldPath)
    // via the data-driven loader (app/leveldoc_world.*) — brushes -> meshes + Jolt
    // bodies + materials, props -> GLB instances, lights -> point lights, triggers ->
    // zones with script hooks. HOT-RELOADABLE while playing: an mtime poll + the
    // `level_reload` console command tear down ONLY the doc-built objects and rebuild
    // in place (player position preserved). The editor's File>Save writes the same
    // default path, closing the edit -> save -> live-reload loop.
    const bool docWorld = (worldMode == "fromdoc");
    // Hard cap on how many rooms the portal flood-fill may add per frame. Even down the
    // longest sightline with a deep r_culldepth, the cull stays well under the whole 53-room
    // tower so the GPU never spikes (the spec's "must NOT regress to drawing the tower").
    constexpr uint32_t kCanonRoomBudget = 18;
    x3::game::CanonFloor canonFloor;           // parsed+resolved Floor 1 (canonWorld only)
    std::vector<uint32_t> canonVisRooms;       // per-frame PVS scratch (canonWorld only)
    std::vector<x3::game::CanonLight> canonLights; // per-room ceiling lights (canonWorld only)
    x3::game::DoorSystem  canonDoors;          // SM_Door_A GLB doors at the cut doorways
    // ---- Keycard / keypad door gating (canonWorld). keycardMask = bitmask of held
    // keycard ids; the Security keycard is a glowing pickup in the Research Lab. ----
    uint32_t keycardMask       = 0;
    uint32_t canonKeycardEnt   = x3::game::kNoLink;
    float    canonKeycardX     = 0.0f, canonKeycardZ = 0.0f;
    bool     canonKeycardTaken = false;
    x3::game::CanonPlay   canonPlay;           // canon Floor-1 gameplay (canonWorld only): sidearm + animated enemies + Martinez + 3 girls
    bool                  canonMedicalActive = false;  // latch: the medical-bay rescue clock was started (player reached the wards)
    // --world fromdoc: the LevelDoc-built world + its hot-reload state (docWorld only).
    x3::game::LevelDocWorld docLevel;
    bool docReloadRequested = false;           // set by the `level_reload` console cmd
    x3::game::Scene scene;
    x3::game::Level1Game game;
    // LIVING WORLD: facility civilians (detained workers) — a small crowd that
    // idles/wanders in the B1 arena hall, scatters + cowers when shots ring out,
    // and drifts back once it goes quiet. Built only in the legacy Level-1 world.
    x3::game::CrowdSystem facilityCrowd;
    // LIVING WORLD: the FACILITY ALERT LEVEL (the wanted system, pillar 3). The
    // host feeds it observations (guard positions, LOS, gunshots, bodies, keypad
    // tampers) and applies its effects (reinforcement spawns, the level-3 door
    // LOCKDOWN via alertDoorLock, red-shifted lights, the HUD indicator).
    x3::game::AlertSystem   facilityAlert;
    x3::game::AlertDoorLock alertDoorLock;
    bool  facilityAlertOn = false;   // armed only in the legacy Level-1 world
    float alertHudClock   = 0.0f;    // drives the lockdown HUD pulse
    // B3: the terrain world is now STREAMED around the player via a residency
    // ring (TerrainStreamer) fed by the engine job system. Both are only created
    // in terrain mode; Level 1 is unaffected.
    x3::game::TerrainStreamer terrainStreamer;
    std::unique_ptr<x3::jobs::IJobSystem> terrainJobs;
    // ---- Advanced elevator (core): a functional cab in Level 1's tall (~9 m)
    // elevator room — the "Take the elevator to Floor 2" exit transport. Press E
    // within ~4 m to call it; it carries the rider up/down (per-frame carry in the
    // loop). Two stops sized to fit the 9 m shaft (room floor + ~6 m up); the full
    // 7-floor spire (5 m/floor) lands with the Spire geometry (see
    // specs/EFLZ_SPIRE_7FLOOR.spec.md). The level WIN still fires from the
    // elevator-room trigger volume after Martinez dies — the elevator is the in-room
    // transport that mediates that exit. Built with the level (Level 1 only, not
    // terrain) so it appears in the screenshot/bench/smoketest paths too.
    x3::game::ElevatorSystem elevator;
    // ---- Spire mid floors (F3 Labs / F4 Offices / F5 Synth bay) encounter content.
    // Authored onto the same Spire plates buildLevel1() produced; reached via the
    // per-floor elevator stops below. Has its own enemy groups + a gated F5 rescue
    // captive + per-floor keypad doors + floor-hub triggers (a host-owned
    // TriggerSystem dispatches the hub ids to midFloors.onTrigger). Level 1 only
    // (not terrain). Independent of Level1Game's B1 beats. ----
    x3::game::SpireMidFloors midFloors;
    x3::game::TriggerSystem  midTriggers;
    // ---- Spire top floors (F6 Executive / F7 Rooftop = the Act-1 finale) encounter
    // content. Same authoring pattern as midFloors: own enemy groups (F6 7-strong
    // strongpoint; F7 the Clone boss + a 7-strong escort), a gated F7 rescue captive
    // (Sarah), per-floor keypad doors (F6 x2, F7 x1) + floor-hub triggers (a host-owned
    // TriggerSystem dispatches the hub ids to topFloors.onTrigger; the F7 hub starts
    // Sarah's clock). Level 1 only, reached via the top elevator stops. ----
    x3::game::SpireTopFloors topFloors;
    x3::game::TriggerSystem  topTriggers;
    // ---- Floor 4.5 — the NEXUS CHAMBER / The Chorus (off-elevator boss). A discrete
    // half-step arena hung OFF the elevator spine between the F4 and F5 plates (NOT a
    // numbered elevator stop). It stages the Wave-1 multi-pod boss (MultiPodBoss +
    // chorusConfig: 5 fused minds, save up to 4). It is NOT armed at load: an F4->F5
    // CONNECTOR trigger (registered DISABLED) "discovers" the Nexus and arms the
    // Chorus (mirror of how the F5 hub gates the rescue clock). Its hub id dispatches
    // through its own TriggerSystem back to nexus.onTrigger(). Level 1 only. ----
    x3::game::SpireNexus    nexus;
    x3::game::TriggerSystem nexusTriggers;
    // ---- Hidden Floor-7 SUB-LEVELS + the Dr. Chen RETURN MISSION. Authored as new
    // graybox plates BELOW B1 (descending -Y), reached via a hidden lift behind the
    // executive desk that ARMS ONLY after the F7 finale is complete (the Clone has fallen
    // AND Sarah is saved). At build the descent is HIDDEN/inert; the host opens it once
    // (subLevels.openDescent) from spire_top's PUBLIC queries — it never modifies
    // spire_top. SL1 Waste Disposal (hazard) / SL2 Cryo Storage (Frozen Collective
    // mini-boss) / SL3 Enhanced Interrogation (free Dr. Chen). Its own TriggerSystem
    // (subTriggers) dispatches the hidden-lift + per-sub-level hub ids. Level 1 only. ----
    x3::game::SpireSubLevels subLevels;
    x3::game::TriggerSystem  subTriggers;
    // Host-tracked latch: Sarah was rescued on F7 (set true when topFloors.onRescue()
    // succeeds). Combined with "the Clone boss is dead" it is the descent gate. We track
    // it host-side because spire_top exposes victimCaptive() (not a companion query), and
    // we must not modify spire_top.
    bool sarahSaved = false;
    // ---- BUG-FIX (teardown ordering): every exit path must release the game-side
    // physics bodies BEFORE physics->shutdown(). `game`/`nexus`/`canonPlay` are
    // stack objects declared ABOVE `physics`'s explicit shutdown call sites, so
    // their destructors (barrel DestructibleManager bodies, in-flight skinned
    // death-ragdoll Jolt bodies) run during stack unwind AFTER the world is dead —
    // a use-after-shutdown that at best fires the '[phys] removeBody: invalid/
    // stale id' warning and at worst touches a destroyed Jolt system. The main
    // window-loop exit already did this; the headless early-exit paths (bench,
    // screenshot, ui-demo, smoketest) did NOT. Call this before EVERY
    // physics->shutdown() that follows game construction.
    auto shutdownGameSystems = [&]() {
        game.shutdown();                               // every enemy group + Martinez + barrels
        nexus.shutdown();                              // F4.5 Chorus pod ragdolls
        if (canonPlay.built()) canonPlay.shutdown();   // canonlevel enemy ragdolls
    };
    // Join the async boot-manifest GLB warmup (no-op when none was kicked): the
    // world build below takes model-cache hits instead of repeating parse/decode.
    x3::asset::joinModelPreload();
    x3::boot::mark("GLB preload joined");
    if (canonWorld) {
        // ---- DATA-DRIVEN CANONICAL FLOOR 1 + per-room PVS cull. ----
        canonFloor = x3::game::loadCanonFloor(x3::game::canonProjectJsonPath(), 1);
        if (canonFloor.valid()) {
            x3::game::CanonBuildOpts copts; copts.doors = &canonDoors; copts.lockSecuredRooms = true;
            x3::game::buildCanonFloor(canonFloor, scene, *device, *physics, copts);
            x3::boot::mark("canon floor geometry+doors");
            // Per-room ceiling lights: the data-driven floor skips the env_art Light_A
            // fixtures the legacy level registers, so without these the rooms only get
            // ambient + the flashlight (the DARK bug). We feed only the player's VISIBLE
            // rooms' lights each frame (below) so the active count stays under the cap.
            canonLights = x3::game::buildCanonLights(canonFloor);
            x3::logInfo("--world canonlevel: built canonical Floor 1 (" +
                        std::to_string(canonFloor.rooms.size()) + " rooms, " +
                        std::to_string(scene.size()) + " entities, " +
                        std::to_string(canonLights.size()) + " room lights); per-room PVS cull ACTIVE");
            // (Secured-room doors — Security / Medical / Armory — are built + locked INSIDE
            // buildCanonFloor via copts.lockSecuredRooms above: those rooms reach the hall
            // through open gap-bridges, so a slab has to be placed there before it can be locked.)
            // ---- SECURITY KEYCARD pickup: a glowing cyan card in the Research Lab. Grabbed by
            // walking up to it (proximity, in the per-frame tick). Opens the Security Station
            // (card OR code) + is half of the Armory's card+code lock. ----
            {
                const uint32_t rr = canonFloor.roomByName("Research Lab");
                if (rr != x3::game::kNoRoom) {
                    const x3::game::CanonRoom& room = canonFloor.rooms[rr];
                    canonKeycardX = room.cx; canonKeycardZ = room.cz;
                    const float ky = room.y0() + 1.0f;
                    x3::prims::PrimMesh card = x3::prims::makeBox(0.22f, 0.14f, 0.02f, 0.0f, 0.0f, 0.0f, 1.0f);
                    x3::game::Entity e;
                    for (int i = 0; i < 16; ++i) e.transform[i] = 0.0f;
                    e.transform[0] = e.transform[5] = e.transform[10] = e.transform[15] = 1.0f;
                    e.transform[12] = canonKeycardX; e.transform[13] = ky; e.transform[14] = canonKeycardZ;
                    e.mesh = device->createMesh(card.verts.data(), (uint32_t)card.verts.size(),
                                                card.index.data(), (uint32_t)card.index.size());
                    e.baseColor[0] = 0.15f; e.baseColor[1] = 0.88f; e.baseColor[2] = 1.0f; e.baseColor[3] = 1.0f;
                    e.tag     = (uint32_t)x3::game::Tag::Prop;
                    e.visible = true;
                    e.roomId  = rr;
                    canonKeycardEnt = scene.add(e);
                    x3::logInfo("--world canonlevel: Security keycard placed in the Research Lab");
                }
            }
            // ---- GAMEPLAY onto the canon rooms (makes --world canonlevel PLAYABLE): the
            // sidearm pickup in Jake's Cell, the animated enemy squad down the Main Hall +
            // side cells, Martinez in the Boss Arena, and the 3 rescue girls + their
            // attackers in the Medical Bay / adjacent wards. Every spawn is room-tagged so
            // the flood-fill cull + per-room lights include it (and the model draw is
            // gated to the visible set, see the draw block). Uses the SAME systems the
            // legacy Level1Game uses (MonsterManager / RescueSystem / WeaponSystem). ----
            canonPlay.build(canonFloor, scene, *device, *physics,
                            x3::game::riggedGlbRoot(), x3::game::canonGirlsDialogPath());
            x3::boot::mark("canon gameplay spawns (GLB enemies)");
            // The re-aimed Level-1 beat flow on REAL canonical room centers: spawn in
            // Jake's Cell, down the wide Main Hall, through Security/Research/Medical/
            // Armory, into the Boss Arena (Martinez), out via the Elevator Lobby.
            {
                x3::game::CanonBeats bt = x3::game::canonBeats(canonFloor);
                auto rc = [&](uint32_t r) -> std::string {
                    if (r == x3::game::kNoRoom) return "(absent)";
                    const auto& rm = canonFloor.rooms[r];
                    char b[64]; std::snprintf(b, sizeof(b), "(%.0f,%.0f)", rm.cx, rm.cz);
                    return std::string(b);
                };
                x3::logInfo("--world canonlevel beat flow: Jake's Cell " + rc(bt.jakeCell) +
                            " -> Main Hall " + rc(bt.mainHall) + " -> Security " + rc(bt.security) +
                            " -> Research " + rc(bt.research) + " -> Medical " + rc(bt.medical) +
                            " -> Armory " + rc(bt.armory) + " -> Boss Arena " + rc(bt.bossArena) +
                            " -> Elevator Lobby " + rc(bt.elevatorLobby));
            }
        } else {
            // JSON absent on this machine -> fall back to the legacy tower build so the
            // path is never broken (the loader logged the miss).
            x3::logInfo("--world canonlevel: canonical JSON absent; falling back to legacy Level 1 build");
            game.build(scene, *device, *physics, x3::game::riggedGlbRoot());
        }
    } else if (docWorld) {
        // ---- DATA-DRIVEN LEVELDOC WORLD (--world fromdoc). If the doc file is
        // absent, SEED it with the sample room (so a first boot always lands in a
        // playable space AND leaves a JSON on disk the user can live-edit). If it
        // exists but fails to parse, fall back to the legacy Level 1 build.
        {
            std::error_code ec;
            if (!std::filesystem::exists(docWorldPath, ec)) {
                std::filesystem::path dp(docWorldPath);
                if (dp.has_parent_path()) std::filesystem::create_directories(dp.parent_path(), ec);
                x3::editor::LevelDoc seed = x3::game::makeSampleLevelDoc();
                if (seed.saveJson(docWorldPath))
                    x3::logInfo("--world fromdoc: no doc at " + docWorldPath +
                                " — seeded the sample room there (edit it live!)");
            }
        }
        if (docLevel.loadFromFile(docWorldPath, scene, *device, *physics)) {
            x3::logInfo("--world fromdoc: BOOTED LevelDoc '" + docLevel.doc().name + "' from " +
                        docWorldPath + " (" + std::to_string(docLevel.brushEntityCount()) +
                        " brushes, " + std::to_string(docLevel.propEntityCount()) + " props, " +
                        std::to_string(docLevel.bodyCount()) + " bodies, " +
                        std::to_string(docLevel.lightCount()) + " lights, " +
                        std::to_string(docLevel.triggerCount()) + " triggers). "
                        "HOT RELOAD: save the JSON (or the F8 editor's File>Save) or run "
                        "`level_reload` in the console — the world rebuilds in place.");
        } else {
            x3::logInfo("--world fromdoc: doc unreadable; falling back to legacy Level 1 build");
            game.build(scene, *device, *physics, x3::game::riggedGlbRoot());
        }
    } else if (!terrainWorld) {
        game.build(scene, *device, *physics, x3::game::riggedGlbRoot());
        x3::boot::mark("level1 build (graybox+GLBs)");
        // LIVING WORLD: the facility civilians in the arena hall (no physics
        // bodies; they scatter on gunfire via facilityCrowd.onViolence below).
        {
            const x3::game::Level1Layout& lay = game.layout();
            x3::game::CrowdConfig fcfg;
            fcfg.count   = 6;
            fcfg.centerX = lay.arenaCenter.x;
            fcfg.centerZ = lay.arenaCenter.z;
            fcfg.groundY = lay.arenaCenter.y;
            fcfg.radius  = std::max(2.5f, std::min(lay.arenaHalf.x, lay.arenaHalf.z) - 1.2f);
            fcfg.dance   = false;     // these are frightened workers, not dancers
            facilityCrowd.build(fcfg, scene, *device);
        }
        // LIVING WORLD: arm the facility alert level (tunables from
        // assets/world/alert.json; missing file -> built-in defaults).
        facilityAlert.configure(x3::game::loadAlertConfig(x3::game::alertJsonPath()));
        facilityAlertOn = true;
        // Audio hookups for Level 1 events (§9, nice-to-have; silent if no device).
        x3::game::Level1Audio la;
        la.sys = audio.get(); la.door = sndDoor; la.pickup = sndPickup;
        la.gun = sndGun; la.death = sndDeath;
        game.setAudio(la);

        // Game-feel CUE sink: route enemy footstep / impact cues onto 3D audio.
        // Footsteps reuse the (pitched-down, quiet) step WAV at the enemy's foot;
        // impacts use the gunshot transient. The trigger points live in monster.cpp;
        // here the host maps them onto whatever sounds it has. Intensity -> volume.
        {
            x3::audio::IAudioSystem* asys = audio.get();
            game.setCueSink([asys, sndStep, sndGun](const x3::game::GameCue& c) {
                if (!asys) return;
                switch (c.kind) {
                    case x3::game::CueKind::Footstep:
                        if (sndStep.valid())
                            asys->playSound3D(sndStep, c.pos.x, c.pos.y, c.pos.z,
                                              0.12f * c.intensity, 0.55f);
                        break;
                    case x3::game::CueKind::BulletImpact:
                    case x3::game::CueKind::MeleeImpact:
                        if (sndGun.valid())
                            asys->playSound3D(sndGun, c.pos.x, c.pos.y, c.pos.z,
                                              0.5f * c.intensity, 0.7f);
                        break;
                }
            });
        }

        // Spire elevator: one stop per floor (B1..F7), 5 m apart, so a ride lands on
        // walkable floor geometry at every plate. The cab top sits flush with each
        // floor's base Y (cab center = floorBaseY + cabHY). Driven by the layout's
        // per-floor base heights so geometry + transport stay in lockstep.
        const x3::game::Level1Layout& Lb = game.layout();
        const float cabHY = 0.15f;
        std::vector<float> elevStops;
        elevStops.reserve(x3::game::kSpireFloorCount);
        for (uint32_t fi = 0; fi < x3::game::kSpireFloorCount; ++fi)
            elevStops.push_back(Lb.floorBaseY[fi] + cabHY);   // cab top at this floor
        elevator.build(scene, *device, *physics,
                       Lb.elevatorCenter.x, Lb.elevatorCenter.z,
                       1.4f, cabHY, 1.4f, elevStops, /*startStop*/0);

        // ---- Souped-up strata/disco elevator (ported from Tim's x3-elevator.js;
        // blueprint §2.2). Turn ON the 10-state FSM (ramped accel/cruise/decel +
        // doors), build the in-car visuals (glass + earth-strata scroll display +
        // twin OLEDs + back-wall mirror + blue access terminal/keypad + ceiling
        // light + disco ball), wire the procedural-audio hooks, and set the
        // Club-1127 descent target at Y=-200 (the Club 1127 lane builds that room).
        // The 1127 keypad code (handled in the use/keypad block below) toggles
        // DISCO + drives the cab down to the club. Keeps the floorBaseY[]-driven
        // stops, so the Phase-0 283 m re-scale auto-applies.
        elevator.enableFsm(true);
        elevator.setAudio(audio.get());
        elevator.setClubStopY(x3::game::ElevatorSystem::kDefaultClubFloorY + cabHY);
        {
            static const char* kFloorLabels[] =
                { "B1", "F1", "F2", "F3", "F4", "F5", "F6", "F7" };
            std::vector<std::string> labels;
            for (uint32_t fi = 0; fi < x3::game::kSpireFloorCount &&
                                  fi < (uint32_t)(sizeof(kFloorLabels)/sizeof(kFloorLabels[0])); ++fi)
                labels.emplace_back(kFloorLabels[fi]);
            elevator.setFloorLabels(labels);
        }
        elevator.buildVisuals(scene, *device);
        x3::boot::mark("elevator build");

        // Author the F3/F4/F5 mid-floor encounters onto the Spire plates. The
        // per-floor elevator stops above (one per floor) make them reachable.
        midFloors.build(scene, *device, *physics, Lb, midTriggers,
                        x3::game::riggedGlbRoot());
        x3::boot::mark("spire mid floors (F3-F5)");

        // Author the F6/F7 top-floor encounters (the Act-1 finale: F6 strongpoint,
        // F7 the Clone boss + Sarah rescue). Reached via the elevator's top stops.
        topFloors.build(scene, *device, *physics, Lb, topTriggers,
                        x3::game::riggedGlbRoot());
        x3::boot::mark("spire top floors (F6-F7)");

        // Stage the off-elevator Floor 4.5 NEXUS (The Chorus). The connector trigger
        // is added DISABLED inside build() — the encounter is found later on the
        // F4->F5 path, not armed at load. The host enables that connector once the
        // F4->F5 progression opens (e.g. on reaching the F4 hub); until then the
        // Chorus is inert. We open the connector when the player has cleared past F4
        // (the F4 hub fires) so the Nexus becomes discoverable on the way to F5.
        nexus.build(scene, *device, *physics, Lb, nexusTriggers,
                    x3::game::riggedGlbRoot());
        // Author the hidden Floor-7 sub-levels BELOW B1 (built HIDDEN/inert; the descent
        // is not armed until the F7-complete gate is satisfied below in the loop).
        subLevels.build(scene, *device, *physics, Lb, subTriggers,
                        x3::game::riggedGlbRoot());
        x3::boot::mark("nexus + sub-levels");

    }
    const x3::game::Level1Layout& L1 = game.layout();
    x3::boot::mark(canonWorld ? "world build (canon floor + gameplay)"
                              : "world build (level1 + spire floors)");

    // World geometry + canon room spawns are built — push the heavy build steps onto
    // the loading bar and show a frame so the human sees the bar jump.
    loading.step(x3::game::LoadStep::WorldGeometry, "BUILDING WORLD");
    loading.step(x3::game::LoadStep::Spawns, "SPAWNING ACTORS");
    loadingFrame(kLoadDt);
    x3::boot::mark("loading frame (post-build)");

    // ---- Outdoor terrain world setup (--world terrain). Bring up the job system
    // + the camera-centered STREAMER (B3): an UNBOUNDED procedural world where
    // only a bounded ring of tiles around the player is resident. Tiles are
    // generated async on jobs and uploaded (createMesh + addStaticMesh) budgeted
    // per frame; out-of-range tiles stream out. Enable the analytic sky with the
    // engine's sun, and spawn near the world origin. The player walks it through
    // the SAME walking controller + physics as Level 1. ----
    x3::phys::Vec3 terrainSpawn{};
    // Ocean sea level (used only in --world ocean): part-way up the height range so
    // valleys flood + hills become shorelines. The per-frame water update is in the
    // render loop.
    const float oceanSeaLevel = 14.0f;
    if (terrainWorld) {
        terrainJobs.reset(x3::jobs::createJobSystem());
        terrainJobs->init(0);   // hw_concurrency-1 compute workers + an I/O lane

        // CONFIG-UNIFY: build the streamer from the canonical world config (the
        // single source of truth, app/terrain.h) so a host-side height/normal/place
        // query (terrainHeightAtWorld / placeOnTerrain — the 14900k's building +
        // cliffside-pad anchoring API) matches exactly what is rendered + streamed
        // underfoot. Same defaults as before (32 m tiles, heightScale 55 m, seed
        // 1337) => behavior + look unchanged; this just shares the config.
        const x3::game::TerrainConfig& tcfg = x3::game::worldTerrainConfig();
        x3::rhi::IRenderDevice::SkyParams sp{};
        sp.enabled = true;
        sp.sunDir[0] = 0.4f; sp.sunDir[1] = 1.0f; sp.sunDir[2] = 0.3f;
        sp.sunColor[0] = 1.0f; sp.sunColor[1] = 0.97f; sp.sunColor[2] = 0.92f;
        sp.sunIntensity = 1.0f; sp.haze = 0.5f; sp.exposure = 1.0f;
        device->setSkyParams(sp);
        // Spawn the player on the surface near the world origin, a little above so
        // the capsule settles onto the hill on the first frames. heightAt() is a
        // pure function of the config, valid before any tile exists. In OCEAN mode,
        // spawn on ground ABOVE the sea level so the player starts on a shore.
        float sx = 0.0f, sz = 0.0f;
        if (oceanWorld) {
            for (float r = 0.0f; r < 600.0f; r += 24.0f) {
                if (x3::game::terrainHeightAt(tcfg, r, 0.0f) > oceanSeaLevel + 4.0f) { sx = r; sz = 0.0f; break; }
            }
        }
        terrainSpawn = x3::phys::Vec3{ sx,
            std::max(x3::game::terrainHeightAt(tcfg, sx, sz), oceanSeaLevel) + 2.0f, sz };
        // Residency radius 8 tiles (= 256 m) => up to 17x17 = 289 tiles resident.
        terrainStreamer.init(scene, *device, *physics, terrainJobs.get(),
                             tcfg, sx, sz, /*radius=*/8);
        if (oceanWorld)
            x3::logInfo("--world ocean: STREAMED terrain + animated ocean (walk the shore, WASD)");
        else
            x3::logInfo("--world terrain: STREAMED unbounded terrain world (walk/fly the hills, WASD)");
    }

    // ---- Combat FX (gameplay-feel pass): shot tracers + muzzle flash. The
    // crosshair now lives in the screen-space HUD layer (S7), not here. ----
    x3::game::CombatFx combatFx;
    combatFx.init(*device);
    x3::boot::mark("combat fx init");
    // FX / debris / UI primed — bar nearly full.
    loading.step(x3::game::LoadStep::FxReady, "PRIMING FX");

    // Explosive barrels FX: a cluster of impact bursts at the blast center so a shot
    // barrel reads as a violent fireball (on top of its own scattering debris chunks).
    game.barrels().setFxSink([&combatFx](const float c[3], float radius) {
        const x3::phys::Vec3 ctr{ c[0], c[1], c[2] };
        combatFx.spawnImpact(ctr, x3::phys::Vec3{ 0.0f, 1.0f, 0.0f });
        combatFx.spawnImpact(ctr, x3::phys::Vec3{ 0.7f, 0.5f, 0.0f });
        combatFx.spawnImpact(ctr, x3::phys::Vec3{ -0.7f, 0.5f, 0.0f });
        combatFx.spawnImpact(ctr, x3::phys::Vec3{ 0.0f, 0.5f, 0.7f });
    });

    // ---- GIBS: monsters EXPLODE into chunks + blood when they die. -----------
    // Configure the GPU-compute debris pool ONCE (the same cheap fragment sim the
    // destruction self-test uses) so monster-death gib bursts have somewhere to land.
    // Then wire a single DEATH FX sink that the host fans to every enemy group below.
    // The sink spawns: (1) a GPU debris BURST of cheap chunks flung outward from the
    // body center, and (2) a cluster of blood impacts (CombatFx::spawnImpact) so the
    // kill reads as a violent gib explosion. Flyer/drone deaths spark a touch faster.
    {
        x3::rhi::IRenderDevice::GpuDebrisParams gp{};
        gp.groundY = 0.0f; gp.restitution = 0.25f; gp.friction = 0.5f;
        gp.linearDamping = 0.35f; gp.sleepFrames = 14;
        device->gpuDebrisConfig(gp);
    }
    {
        x3::rhi::IRenderDevice* dev = device.get();
        // The gib explosion: a capped GPU debris burst flung outward from the body
        // center + a tight cluster of blood impacts so the kill reads bloody, not just
        // dusty. Flyers/drones burst a touch more + faster (they pop in the air). The
        // burst seed varies by position so two kills don't fling identically. GPU-
        // simulated chunks are cheap (one compute pass + one instanced draw, below).
        x3::game::DeathFxFn deathFx = [&combatFx, dev](const float pos[3], bool flying) {
            const x3::phys::Vec3 ctr{ pos[0], pos[1], pos[2] };
            const uint32_t chunks = flying ? 20u : 16u;
            const float    kick   = flying ? 8.5f : 7.0f;   // m/s outward spread
            const uint32_t seed   = 0x91B0u ^ (uint32_t)(ctr.x * 131.0f)
                                            ^ ((uint32_t)(ctr.z * 977.0f) << 8);
            const float bp[3] = { pos[0], pos[1], pos[2] };
            dev->gpuDebrisSpawnBurst(bp, chunks, kick, /*lifetime*/4.5f,
                                     /*halfExtent*/0.07f, seed);
            combatFx.spawnImpact(ctr, x3::phys::Vec3{ 0.0f, 1.0f, 0.0f });
            combatFx.spawnImpact(ctr, x3::phys::Vec3{ 0.8f, 0.4f, 0.0f });
            combatFx.spawnImpact(ctr, x3::phys::Vec3{ -0.8f, 0.4f, 0.0f });
            combatFx.spawnImpact(ctr, x3::phys::Vec3{ 0.0f, 0.4f, 0.8f });
            combatFx.spawnImpact(ctr, x3::phys::Vec3{ 0.0f, 0.4f, -0.8f });
            combatFx.spawnBlood(ctr, x3::phys::Vec3{ 0.0f, 1.0f, 0.0f });
            combatFx.spawnSmoke(ctr);   // lingering puff so the burst point lingers
        };
        // Level1Game fans the sink to its own groups (corridor/checkpoint/bossAdds/
        // Martinez/Chen) — current AND future spawns.
        game.setDeathFxSink(deathFx);
        // Also fan it to the Spire-floor enemy groups + their bosses (those managers
        // live on the floor controllers, not Level1Game, so they don't get the fan
        // above). Each MonsterManager stores the sink + applies it to current + future
        // spawns, so kills on any floor gib the same way.
        if (!terrainWorld) {
            for (uint32_t f = 0; f < (uint32_t)x3::game::SpireMidFloor::Count; ++f)
                midFloors.enemies((x3::game::SpireMidFloor)f).setDeathFxSink(deathFx);
            midFloors.f3Boss().setDeathFxSink(deathFx);
            midFloors.swarmBoss().setDeathFxSink(deathFx);
            for (uint32_t f = 0; f < (uint32_t)x3::game::SpireTopFloor::Count; ++f)
                topFloors.enemies((x3::game::SpireTopFloor)f).setDeathFxSink(deathFx);
            topFloors.overseerBoss().setDeathFxSink(deathFx);
            topFloors.boss().setDeathFxSink(deathFx);
        }
    }

    // =====================================================================
    // WEAPONS: data-driven arsenal (pistol / SMG / shotgun / plasma). The
    // Arsenal owns the roster + per-weapon ammo/cooldown/reload state and the
    // fire/switch/reload logic; the existing pickup (game.armed()) still gates
    // whether the player may fire at all. Number keys 1..N switch; the fire path
    // below routes the existing combat raycast through the selected WeaponDef
    // (fire rate / ammo / reload / spread / pellets / recoil / hitscan-vs-bolt).
    // --test-weapons covers the logic headlessly. Viewmodels load per weapon
    // (missing GLBs fall back to the energy pistol). ====================
    x3::game::Arsenal arsenal;
    arsenal.loadViewmodels(*device, x3::game::riggedGlbRoot());
    x3::boot::mark("weapon viewmodels (GLBs)");

    // ==================== THIRD-PERSON VIEW (FIRST MILESTONE) ====================
    // Load the Jake avatar + the FP/3P toggle. FP is the DEFAULT (eye-cam + weapon
    // viewmodel, unchanged). F5 flips to a follow/orbit camera behind+above the player
    // with the animated Jake avatar (the held weapon socketed to its right hand). The
    // player capsule/collision are untouched (camera change only). On a failed Jake
    // load this stays unbuilt and FP keeps working. See app/thirdperson.* + F5 below.
    x3::game::ThirdPersonView thirdPerson;
    thirdPerson.build(scene, *device, x3::game::riggedGlbRoot());
    x3::boot::mark("third-person avatar (GLB)");
    bool prevF1 = false, prevF2 = false;

    // ---- PER-WEAPON FIRE SOUNDS (the user's "every gun sounds the same" fix) ----
    // Each WeaponDef carries a distinct sci-fi fireSfx (pack-relative WAV). Load each
    // unique one ONCE into a name->handle cache (keyed by the weapon name) so firing
    // plays the CURRENT weapon's sound instead of the single shared gunshot. The
    // distinct WAVs are deduped by their pack-rel path (several weapons may reuse a
    // file). A weapon with an empty fireSfx (or a missing WAV -> invalid handle) falls
    // back to the shared sndGun. Headless / no-device: load() + play are graceful
    // no-ops, so this stays silent without crashing.
    std::unordered_map<std::string, x3::audio::SoundHandle> fireSfxByName;
    {
        std::unordered_map<std::string, x3::audio::SoundHandle> byPath; // dedupe by WAV
        for (int wi = 0; wi < arsenal.count(); ++wi) {
            const x3::game::WeaponDef& wd = arsenal.def(wi);
            x3::audio::SoundHandle h = sndGun;   // fallback: shared gunshot
            if (!wd.fireSfx.empty()) {
                auto it = byPath.find(wd.fireSfx);
                if (it != byPath.end()) {
                    h = it->second;
                } else {
                    x3::audio::SoundHandle loaded =
                        audio->load(x3::game::resolveAudio(wd.fireSfx));
                    if (loaded.valid()) h = loaded;     // else keep sndGun fallback
                    byPath.emplace(wd.fireSfx, h);
                }
            }
            fireSfxByName[wd.name] = h;
        }
    }
    // Resolve the current weapon's fire sound (fallback: the shared gunshot).
    auto currentFireSfx = [&]() -> x3::audio::SoundHandle {
        auto it = fireSfxByName.find(arsenal.current().name);
        return (it != fireSfxByName.end() && it->second.valid()) ? it->second : sndGun;
    };

    // Live projectile bolts (plasma): host-owned; advanced + impact-resolved each
    // frame. Bounded by gameplay (a handful in flight); a plain vector is fine.
    struct LiveProjectile { x3::phys::Vec3 pos, vel; int damage; float traveled, range;
                            x3::game::WeaponFxKind impactKind = x3::game::WeaponFxKind::Default; };
    std::vector<LiveProjectile> projectiles;
    uint32_t weaponRng = 0xA11CE5u;   // deterministic spread stream
    float    weaponRecoilPitch = 0.0f; // accumulated upward camera kick (rad), decays
    constexpr float kRecoilRecover = 6.0f; // recoil recovery rate (rad/s decay)

    x3::boot::mark("per-weapon fire sfx");
    // World build + every build-time GLB is done — land all batched uploads in one
    // submit. (Per-frame paths from here on use the normal unbatched semantics.)
    device->endUploadBatch();
    x3::boot::mark("upload batch flush");

    // ---- S7: console backend (D6) + screen-space HUD (FPS, console, crosshair).
    std::unique_ptr<x3::con::IConsole> console(x3::con::createConsole());
    x3::game::Hud hud;
    bool quitRequested = false;
    hud.init(*console, &quitRequested);

    // FIX 1: live-tunable viewmodel aim. Register vm_yaw/vm_pitch/vm_roll (deg)
    // and vm_fwd/vm_right/vm_down (m); read them each frame and feed the pose to
    // drawViewmodel so typing e.g. `vm_pitch 10` moves the held gun immediately.
    registerViewmodelCVars(*console);

    // --cullpath <n> / --hzb: seed the D15 GPU-cull cvars from the CLI so every
    // headless path (smoketest / screenshots / bench) can run with the GPU cull on.
    if (cullPathArg != INT_MIN) {
        console->set("r_cullpath", std::to_string(cullPathArg));
        x3::logInfo("[cull] r_cullpath seeded from CLI: " + std::to_string(cullPathArg));
    }
    if (hzbArg) { console->set("r_hzb", "1"); x3::logInfo("[cull] r_hzb seeded from CLI: 1"); }

    // ZERO-STUTTER: push the pacing thresholds + strict-PSO gate ONCE now (the
    // per-frame applyRtaoCVars sync keeps them live in the windowed loop) so the
    // headless smoketest/screenshot paths run with the audit armed too.
    {
        x3::rhi::IRenderDevice::PacingParams pace{};
        pace.warmupFrames = console->getInt("r_fpace_warmup");
        pace.spikeFactor  = console->getFloat("r_fpace_spikex");
        pace.floorMs      = console->getFloat("r_fpace_floor");
        pace.strictPso    = console->getInt("r_strictpso") != 0;
        device->setPacingParams(pace);
    }

    // ---- IN-ENGINE LLM (living NPC minds): the HoloTerminal facility AI. ----
    // Model: Qwen2.5-3B-Instruct Q4_K_M (Apache 2.0), CPU inference via the
    // vendored llama.cpp backend (engine/llm). The .gguf is NOT in git — see
    // assets/models/llm/README.md. MODELLESS the game still works: ai_npc
    // defaults OFF when the file is absent and the terminal serves canned
    // "SYSTEMS DEGRADED" lines instead.
    std::string llmModelPath =
        x3::game::assetRoot() + "/models/llm/qwen2.5-3b-instruct-q4_k_m.gguf";
    if (!std::filesystem::exists(llmModelPath)) {
        // Model-agnostic fallback: any .gguf dropped into assets/models/llm/
        // works (e.g. an Apache-2.0 Qwen2.5-1.5B/7B for commercial builds —
        // see assets/models/llm/README.md on licensing).
        std::error_code fec;
        for (const auto& e : std::filesystem::directory_iterator(
                 x3::game::assetRoot() + "/models/llm", fec)) {
            if (e.path().extension() == ".gguf") { llmModelPath = e.path().string(); break; }
        }
    }
    const bool llmModelPresent = std::filesystem::exists(llmModelPath);
    console->registerCVar("ai_npc",       llmModelPresent ? "1" : "0",
                          "LLM NPC minds (terminal freeform Q&A); default 1 only when the model file exists");
    console->registerCVar("ai_ctx",       "2048", "LLM context tokens per chat");
    console->registerCVar("ai_maxtokens", "256",  "LLM max tokens per reply");
    console->registerCVar("ai_temp",      "0.7",  "LLM sampling temperature");
    std::unique_ptr<x3::llm::ILlmSystem> llm;
    if (!headless && llmModelPresent && console->getInt("ai_npc") != 0) {
        x3::llm::ModelOpts lopts;
        lopts.contextTokens   = console->getInt("ai_ctx");
        lopts.maxOutputTokens = console->getInt("ai_maxtokens");
        lopts.temperature     = console->getFloat("ai_temp");
        llm = x3::llm::createLlmSystem();
        if (!llm->loadModel(llmModelPath, lopts)) llm.reset();
    } else if (!headless && !llmModelPresent) {
        x3::logInfo("[llm] no model at " + llmModelPath +
                    " — terminal freeform Q&A falls back to canned SYSTEMS DEGRADED lines"
                    " (see assets/models/llm/README.md)");
    }

    // --set <cvar> <value> CLI overrides (repeatable) — applied as soon as the
    // console exists so the per-frame cvar sync starts from the requested state.
    for (const auto& kv : cliCVars) {
        console->set(kv.first, kv.second);
        x3::logInfo("--set " + kv.first + " " + kv.second);
    }

    // --legacypost / --notaa: pin the matching cvars so the per-frame cvar->device
    // sync (applyRtaoCVars) keeps the A/B state instead of re-enabling defaults.
    if (legacyPost) {
        console->set("r_autoexposure", "0");
        console->set("r_taa", "0");
        console->set("r_ssr", "0");              // reflections need TAA; pin OFF explicitly
        console->set("r_rtreflections", "0");
        if (legacyPost > 1) { console->set("r_bloom", "0"); console->set("r_tonemap", "0"); }
    }
    if (noTaa) {
        console->set("r_taa", "0");
        console->set("r_ssr", "0");              // reflections need TAA; pin OFF explicitly
        console->set("r_rtreflections", "0");
    }
    if (noRefl) {
        console->set("r_ssr", "0");              // --norefl: reflections off, TAA untouched
        console->set("r_rtreflections", "0");
    }
    if (noRtShadows) {
        console->set("r_rtshadows", "0");        // --nortshadows: CSM-only (bit-identical A/B)
        x3::rhi::IRenderDevice::RtShadowParams rp{};
        rp.tier = 0;
        device->setRtShadowParams(rp);
    }

    // --world fromdoc: the `level_reload` console command. Sets a request flag the
    // sim tick applies at the next frame (never mid-draw). Registered regardless of
    // load success so the command exists once a doc world is up.
    if (docWorld) {
        console->registerCommand("level_reload",
            [&docReloadRequested](const std::vector<std::string>&) {
                docReloadRequested = true;
                x3::logInfo("level_reload: requested (applies next tick)");
            }, "reload the --world fromdoc LevelDoc JSON in place");
    }

    // ---- D14: Lua script system (pak-shipped behavior). Created after the
    // console so x3.cvar/exec bridge a real backend. Every scripts/*.lua found
    // under the asset root (or repo root) is loaded at boot; scripts->update(dt)
    // is pumped once per frame in the main loop below. Fully guarded — if no
    // scripts dir exists this is a no-op and the engine runs unchanged.
    std::unique_ptr<x3::script::IScriptSystem> scripts(
        x3::script::createLuaScriptSystem(console.get()));
    x3::logInfo("D14 script system: loaded " + std::to_string(loadBootScripts(*scripts)) +
                " scripts/*.lua at boot");

    // D14 trigger/objective bindings — factored into registerGameBindings() (shared
    // verbatim with the headless --test-hatch chain self-test, so the test proves
    // the SAME bindings the live game wires; see the function above main()).
    registerGameBindings(*scripts, game);

    // ---- CHAT-TREE dialog runner (x3.chattree/1, --test-chattree). Loads the
    // narrative pack (docs/design/narrative/chat_trees) and binds the runner to
    // the REAL systems: the global TimelineState for karma/axes conditions+fx,
    // the D14 script system for x3.fire effects, and (when the pak shipped an
    // eflz_dialog.lua) the {"lua": fn} condition escape hatch via eval. The
    // follow hook is per-conversation (set where the talk target is known). ----
    x3::game::ChatTreeSystem chatTrees;
    chatTrees.loadDefault();
    chatTrees.ctx().timeline = &x3::game::globalTimeline();
    chatTrees.ctx().scripts  = scripts.get();
    {
        // {"lua": "fn"} conditions evaluate inside the dialog script's sandbox
        // (eval auto-prepends `return`); absent script -> conditions fail safe.
        x3::script::ScriptId dlgScript = x3::script::kInvalidScript;
        for (x3::script::ScriptId id : scripts->loadedScripts())
            if (scripts->status(id).name == "eflz_dialog.lua") { dlgScript = id; break; }
        if (dlgScript != x3::script::kInvalidScript) {
            x3::script::IScriptSystem* sp = scripts.get();
            chatTrees.ctx().luaCond = [sp, dlgScript](const std::string& fn) {
                return sp->eval(dlgScript, fn + "()") == "true";
            };
        }
    }

    // ---- MISSION RUNNER (x3.mission/1, g_missiondoc — default OFF). When the
    // cvar is 1, missions/level1.mission.json is loaded + validated and the doc
    // DRIVES the HUD objective line through the ObjectiveSystem free-text lane
    // (objectives().setText — it wins over the list cursor while non-empty). The
    // hardcoded beat list still runs underneath, untouched; with the cvar 0 none
    // of this is even loaded — zero behavior change. Flags/conditions ride the
    // SAME StoryFlags the chat trees use, so missions and dialog see one world.
    x3::game::MissionDoc        missionDoc;
    x3::game::MissionRunner     missionRunner;
    x3::game::MissionEventBridge missionEvents;
    bool missionDocActive = false;
    if (console->getInt("g_missiondoc") != 0) {
        const std::string mp = x3::game::findMissionFile("level1.mission.json");
        std::vector<std::string> merr;
        if (!mp.empty() && x3::game::loadMissionFile(mp, missionDoc, merr) &&
            x3::game::validateMission(missionDoc, merr)) {
            missionRunner.ctx().flags    = &chatTrees.flags();
            missionRunner.ctx().timeline = &x3::game::globalTimeline();
            missionRunner.ctx().scripts  = scripts.get();
            missionEvents.bind(&chatTrees.flags());
            missionRunner.setObjectiveSink([&game](const std::string& t) {
                game.objectives().setText(t);
            });
            // resume() falls back to start() when no position marker is in the
            // flags (fresh boot); after an F9 flags restore doLoad re-resumes.
            missionDocActive = missionRunner.resume(missionDoc);
            x3::logInfo(std::string("[mission] g_missiondoc=1 — `") + missionDoc.id +
                        "` drives the objective line (stage `" +
                        missionRunner.currentStageId() + "`)");
        } else {
            for (const auto& e : merr) x3::logWarn("[mission] " + e);
            x3::logWarn("[mission] g_missiondoc=1 but no valid mission doc — staying on the hardcoded beats");
        }
    }

    // --test-rt: force hardware RT ambient occlusion ON for the headless smoketest
    // render path so the BLAS/TLAS build + ray-query AO compute + apply passes are
    // exercised under Vulkan validation. No-op if the device lacks RT support (the
    // device silently stays on the raster/SSAO path). The cvar is also live-tunable.
    if (testRt) {
        console->set("r_rtao", "1");
        x3::rhi::IRenderDevice::RtaoParams rp{};
        rp.enabled = true;
        device->setRtaoParams(rp);
        x3::logInfo(std::string("--test-rt: RT AO requested; device rayTracingSupported=") +
                    (device->rayTracingSupported() ? "YES" : "NO"));
    }

    // --test-reflections: exercise the SSR + ray-query reflection chain under
    // Vulkan validation in the headless smoketest render path (TAA stays at its
    // default ON; the depth pre-pass, refl-compute, TLAS build/fallback and the
    // mesh.frag compose all run). On a non-RT device this degrades to SSR-only
    // (the tier gate) — still a valid pass of the test.
    if (testReflections) {
        console->set("r_ssr", "1");
        console->set("r_rtreflections", "1");
        x3::rhi::IRenderDevice::ReflectionParams rf{};
        rf.ssr = true; rf.rtFallback = true;
        device->setReflectionParams(rf);
        x3::logInfo(std::string("--test-reflections: SSR+RT reflections requested; device rayTracingSupported=") +
                    (device->rayTracingSupported() ? "YES (hybrid SSR+ray-query)" : "NO (SSR-only tier)"));
    }

    // --test-ddgi: exercise the DDGI probe-grid chain (BLAS/TLAS + ddgi_rays +
    // ddgi_update compute + mesh.frag atlas sampling) under Vulkan validation in
    // the headless smoketest render path. On hardware without ray query +
    // position fetch this degrades to a no-op (the tier gate) — still a valid
    // pass of the test (the raster ambient path is unchanged by construction).
    if (testDdgi) {
        console->set("r_ddgi", "1");
        x3::rhi::IRenderDevice::DdgiParams dp{};
        dp.enabled = true;
        device->setDdgiParams(dp);
        x3::logInfo(std::string("--test-ddgi: DDGI requested; device rayTracingSupported=") +
                    (device->rayTracingSupported() ? "YES" : "NO"));
    }

    // --test-rtshadows: pin RT soft shadows to tier 2 (sun + point lights) for
    // the headless smoketest render path so the mesh_rt pipelines + TLAS build
    // + per-pixel ray queries run under Vulkan validation. On a non-RT device
    // this degrades to the plain raster pipelines (the tier gate) — still a
    // valid pass of the test (the path is byte-for-byte unchanged there).
    if (testRtShadows) {
        console->set("r_rtshadows", "2");
        x3::rhi::IRenderDevice::RtShadowParams rp{};
        rp.tier = 2;
        device->setRtShadowParams(rp);
        x3::logInfo(std::string("--test-rtshadows: RT soft shadows requested; device rayTracingSupported=") +
                    (device->rayTracingSupported() ? "YES" : "NO"));
    }

    // ---- Stress-test injection (perf instrumentation layer) ----------------
    // `spawn N` adds N procedural cubes around the level spawn so the renderer can
    // be load-tested live; `stress_clear` hides them. The --stress N CLI flag does
    // the same at startup. Default OFF — Level 1 unaffected unless requested.
    x3::game::StressSpawner stress;
    {
        x3::game::Scene*        scenePtr   = &scene;
        x3::rhi::IRenderDevice* devicePtr  = device.get();
        const x3::phys::Vec3    around     = L1.spawn;
        console->registerCommand("spawn",
            [&stress, scenePtr, devicePtr, around, &console](const std::vector<std::string>& a) {
                uint32_t n = a.empty() ? 1000u : (uint32_t)std::strtoul(a[0].c_str(), nullptr, 10);
                stress.spawn(*scenePtr, *devicePtr, n, around.x, around.y, around.z, 40.0f);
                console->print("spawned " + std::to_string(n) + " stress cubes (total " +
                               std::to_string(stress.count()) + ")");
            }, "spawn N procedural cubes for renderer load-testing (default 1000)");
        console->registerCommand("stress_clear",
            [&stress, scenePtr, &console](const std::vector<std::string>&) {
                uint32_t n = stress.clear(*scenePtr);
                console->print("cleared " + std::to_string(n) + " stress cubes");
            }, "hide all spawned stress cubes");
    }
    // Apply the startup --stress N (placed around the level spawn).
    if (stressCount > 0) {
        x3::logInfo("--stress " + std::to_string(stressCount) + ": adding cubes around spawn");
        stress.spawn(scene, *device, stressCount, L1.spawn.x, L1.spawn.y, L1.spawn.z, 40.0f);
    }

    // ---- Spire per-floor capture (--capture-spire [outDir]) ----------------
    // DEV/PLAYTEST TOOL — captures one PNG per Spire floor (B1,F1..F7) of the SAME
    // full Act-1 host the game builds (Level1Game + SpireMidFloors + SpireTopFloors),
    // so a coordinator can eyeball every floor's encounter without walking it. It
    // changes NO gameplay/balance: it only poses the camera, lights the plate, settles
    // a few frames, and reads the rendered image back — like the --screenshot path.
    //
    // Per floor it: (1) parks the camera near that floor's +X arrival/hub vantage,
    // looking across the room toward the encounter (enemies sit in x[3..17]); (2)
    // re-issues setPointLights with just THIS floor's ceiling fixtures (the device's
    // 64-light cap can't hold all 8 floors at once, so the upper plates would be dark
    // otherwise) computed from the canonical floor table — same warm tungsten kit
    // env_art uses; (3) ticks the host a few settle frames (enemies/doors/victims
    // animate, the floor's hub trigger fires); (4) captures spire_<floor>.png. Counts
    // are read from the live managers/plans. Headless / offscreen (no window).
    if (captureSpire) {
        namespace fs = std::filesystem;
        std::error_code mkec; fs::create_directories(captureSpireDir, mkec);
        x3::logInfo("--capture-spire: rendering all 8 Spire floors to " + captureSpireDir);

        const x3::game::Level1Layout& Lc = game.layout();
        const x3::game::L1RoomDef*    tbl = x3::game::level1Rooms();
        const float dt = 1.0f / 60.0f;

        struct SpireShot {
            const char* tag;          // file suffix: b1,f1,...,f7
            x3::game::L1Floor floor;  // which plate
        };
        const SpireShot shots[] = {
            { "b1", x3::game::L1Floor::B1 }, { "f1", x3::game::L1Floor::F1 },
            { "f2", x3::game::L1Floor::F2 }, { "f3", x3::game::L1Floor::F3 },
            { "f4", x3::game::L1Floor::F4 }, { "f5", x3::game::L1Floor::F5 },
            { "f6", x3::game::L1Floor::F6 }, { "f7", x3::game::L1Floor::F7 },
        };

        // How many combatants are placed on a given plate AT CAPTURE TIME (read from
        // the live systems). B1 reports the checkpoint guards built at load (the
        // corridor wave + Martinez spawn later on their beats, so they're absent in a
        // settle-only capture); F3..F7 report their authored plan totals.
        auto enemyCountFor = [&](x3::game::L1Floor f) -> uint32_t {
            switch (f) {
                case x3::game::L1Floor::B1:
                    return game.checkpointEnemies().count() + game.corridorEnemies().count()
                         + (game.martinezSpawned() ? 1u : 0u);
                case x3::game::L1Floor::F3: return midFloors.plan(x3::game::SpireMidFloor::F3).totalCount;
                case x3::game::L1Floor::F4: return midFloors.plan(x3::game::SpireMidFloor::F4).totalCount;
                case x3::game::L1Floor::F5: return midFloors.plan(x3::game::SpireMidFloor::F5).totalCount;
                case x3::game::L1Floor::F6: return topFloors.plan(x3::game::SpireTopFloor::F6).totalCount;
                case x3::game::L1Floor::F7: return topFloors.plan(x3::game::SpireTopFloor::F7).totalCount;
                default: return 0u;   // F1 atrium / F2 plate carry no on-plate enemies
            }
        };

        // Build THIS floor's point-light set the same way env_art lights a plate:
        // a row (or two, for wide plates) of warm tungsten omnis hung just below the
        // ceiling, range scaled to reach the floor. Re-issued per floor so the cap
        // never starves the upper plates of light.
        auto lightFloor = [&](x3::game::L1Floor f) {
            const x3::game::L1RoomDef& r = tbl[(uint32_t)f];
            const float kIntensity = 3.2f;
            const float colR = 1.00f * kIntensity, colG = 0.86f * kIntensity, colB = 0.62f * kIntensity;
            const float lightY = r.y0 + r.ceil - 0.30f;            // just below the ceiling
            const float range  = std::max(7.5f, r.ceil + 3.5f);
            const int   n      = (int)std::ceil((r.x1 - r.x0) / 4.0f);
            const bool  twoRows= (r.zHalf >= 6.0f);
            const float zoff   = twoRows ? r.zHalf * 0.5f : 0.0f;
            std::vector<x3::rhi::PointLight> pls;
            for (int j = 0; j < (twoRows ? 2 : 1); ++j) {
                const float wz = twoRows ? ((j == 0) ? -zoff : zoff) : 0.0f;
                for (int i = 0; i < n; ++i) {
                    x3::rhi::PointLight pl;
                    pl.pos[0] = r.x0 + (i + 0.5f) * 4.0f; pl.pos[1] = lightY; pl.pos[2] = wz;
                    pl.range  = range;
                    pl.color[0] = colR; pl.color[1] = colG; pl.color[2] = colB;
                    pls.push_back(pl);
                }
            }
            // A bright cool fill light a few meters in front of the camera (down -X)
            // so the encounter reads clearly in the still even on the dim plates. Dev
            // tool only — it lights the CAPTURE, not gameplay (the set is re-issued
            // fresh per floor and the game owns its own lights at runtime).
            {
                x3::rhi::PointLight fill;
                fill.pos[0] = r.x1 - 12.0f; fill.pos[1] = r.y0 + 2.4f; fill.pos[2] = 0.0f;
                fill.range  = 16.0f;
                fill.color[0] = 3.6f; fill.color[1] = 3.8f; fill.color[2] = 4.2f;
                pls.push_back(fill);
            }
            device->setPointLights(pls.data(), (uint32_t)pls.size());
        };

        x3::ui::GameHud capHud;
        arsenal.select(0);   // pistol selected so the HUD arsenal reads in the still
        bool allOk = true;

        for (const SpireShot& s : shots) {
            const x3::game::L1RoomDef& r = tbl[(uint32_t)s.floor];
            const float baseY = Lc.floorBaseY[(uint32_t)s.floor];
            // Vantage: stand near the +X arrival/hub end, slightly elevated, and look
            // across the plate toward -X (the encounter sits in x[3..17]), pitched down
            // so the floor + props + enemies frame cleanly. yaw=PI => forward
            // (cos,0,sin) = (-1,0,0). For F3..F7 this matches plan().arrival (x=17.5);
            // F1/F2 are open plates so the same X works. B1 is the ONLY plate with
            // internal spine cross-walls (cell/corridor/armory/checkpoint/arena, doors
            // at x=5/9/12.5/15) so an open-plate vantage just stares at a wall: instead
            // frame the CHECKPOINT room (x[12.5,15]) where the 4 build-time guards live,
            // standing just -X of the Door D wall looking back toward the squad + Door C.
            // This deliberately stays OUT of the arena trigger (x[16,19]) so the capture
            // is the checkpoint encounter, not Martinez filling the lens (he spawns on
            // the arena beat at runtime; the report documents the B1 boss separately).
            const bool  isB1   = (s.floor == x3::game::L1Floor::B1);
            const float camX   = isB1 ? 14.85f : (r.x1 - 6.0f); // checkpoint (B1) / ~18 m (others)
            const float camY   = baseY + 2.2f;          // slightly above standing eye for an overview
            const float camZ   = 0.0f;
            const float camYaw = 3.14159265f;           // look toward -X across the room
            const float camPit = -0.20f;
            const float camFov = 80.0f;
            device->setCamera(camX, camY, camZ, camYaw, camPit, camFov);
            const x3::phys::Vec3 camEye{ camX, camY, camZ };
            lightFloor(s.floor);

            const std::string outPath = captureSpireDir + "/spire_" + s.tag + ".png";
            const int kSettle = 24;   // enough frames for shadows + skinning + doors to fully open
            for (int i = 0; i < kSettle; ++i) {
                glfwPollEvents();
                game.tick(dt, scene, *physics, camEye, camEye);
                for (uint32_t tid : midTriggers.update(camEye)) midFloors.onTrigger(tid);
                midFloors.tick(dt, scene, *physics, camEye, camEye, nullptr, x3::game::AttackFxFn{});
                for (uint32_t tid : topTriggers.update(camEye)) topFloors.onTrigger(tid);
                topFloors.tick(dt, scene, *physics, camEye, camEye, nullptr, x3::game::AttackFxFn{});
                for (uint32_t tid : nexusTriggers.update(camEye)) nexus.onTrigger(tid);
                nexus.tick(dt, scene, *physics, camEye, nullptr, x3::game::AttackFxFn{});
                physics->step(dt);
                scene.update(*physics);
                // Re-pose + re-light each frame (scene.update() doesn't move the camera,
                // and another floor's draw could be interleaved in principle).
                device->setCamera(camX, camY, camZ, camYaw, camPit, camFov);
                if (i == kSettle - 1) device->armCapture(outPath.c_str());
                auto frame = device->beginFrame();
                if (frame.valid) {
                    scene.render(*device, frame);
                    game.drawDoors(*device, frame);
                    game.drawWorldExtras(*device, frame, scene);
                    midFloors.drawDoors(*device, frame);
                    midFloors.draw(*device, frame, scene);
                    topFloors.drawDoors(*device, frame);
                    topFloors.draw(*device, frame, scene);
                    nexus.draw(*device, frame, scene);            // Floor 4.5 Chorus pods
                    subLevels.drawDoors(*device, frame);          // hidden sub-level door slabs (no-op while closed)
                    subLevels.draw(*device, frame, scene);        // sub-level enemies + Frozen Collective + Dr. Chen
                    // Production HUD over the vantage (HP / weapon / objective / crosshair).
                    x3::ui::UiContext capUi;
                    capUi.begin(*device, frame, x3::ui::UiInput{});
                    x3::ui::HudModel hm{};
                    hm.hp = 100; hm.maxHp = x3::game::kPlayerMaxHp; hm.alive = true;
                    hm.showCrosshair = true;
                    hm.objective = game.objectives().currentLabel().c_str();
                    const x3::game::WeaponDef&            wd = arsenal.current();
                    const x3::game::Arsenal::WeaponState& ws = arsenal.currentState();
                    hm.weapon = wd.name.c_str();
                    hm.ammoInMag = ws.ammoInMag; hm.ammoReserve = ws.reserve;
                    capHud.draw(capUi, hm, dt);
                    capUi.end();
                }
                device->endFrame(frame);
            }
            const bool wrote = device->captureFrame(outPath.c_str());
            const uint32_t ecount = enemyCountFor(s.floor);
            if (wrote)
                x3::logInfo("--capture-spire: wrote " + outPath + " | enemies=" +
                            std::to_string(ecount));
            else {
                allOk = false;
                x3::logError("--capture-spire: capture FAILED for " + outPath);
            }
        }

        x3::logInfo(std::string("--capture-spire: ") + (allOk ? "all 8 floors captured" : "one or more captures FAILED"));
        audio->shutdown();
        combatFx.shutdown(*device);
        physics->shutdown();
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return allOk ? 0 : 1;
    }

    // ---- Benchmark mode (--bench N [frames]) -------------------------------
    // Point the camera at the spawned cube field and render `benchFrames` frames
    // with vsync OFF, then report averaged FPS + CPU/GPU ms over the steady-state
    // window (the first frames are skipped to drop swapchain/pipeline warm-up and
    // the GPU-timestamp readback latency). Produces the perf baseline numbers.
    if (bench) {
        // Camera above + back from the spawn so the whole 40 m cube volume is in
        // view (worst case for the renderer: everything submitted is on-screen).
        const float bx = L1.spawn.x - 35.0f, by = L1.spawn.y + 25.0f, bz = L1.spawn.z;
        const float byaw = 0.0f, bpitch = -0.5f;
        device->setCamera(bx, by, bz, byaw, bpitch, 75.0f);

        const uint32_t warmup = std::min<uint32_t>(60, benchFrames / 4);
        double sumCpuMs = 0.0, sumGpuMs = 0.0; uint32_t measured = 0;
        double prevT = glfwGetTime();
        x3::rhi::RenderStats last{};
        // --bench --fx-demo: split the run into a particle-OFF half then a heavy
        // particle-ON half (a near-capacity burst spawned every frame in the camera's
        // view) so the GPU-time delta isolates the particle pass cost.
        const bool fxBench = fxDemo;
        const uint32_t halfFrames = benchFrames / 2;
        double sumGpuOff = 0.0, sumGpuOn = 0.0; uint32_t nOff = 0, nOn = 0;
        // Burst origin: in front of the bench camera, in view of the cube field.
        const x3::phys::Vec3 bEye{ bx, by, bz };
        const x3::phys::Vec3 bLook{ std::cos(bpitch) * std::cos(byaw),
                                    std::sin(bpitch), std::cos(bpitch) * std::sin(byaw) };
        for (uint32_t f = 0; f < benchFrames && !glfwWindowShouldClose(window); ++f) {
            glfwPollEvents();
            // Sync the live cvars (incl. r_cullpath/r_hzb seeded by --cullpath/--hzb)
            // onto the device, exactly as the main loop does each frame.
            applyRtaoCVars(*console, *device);
            double nowT = glfwGetTime();
            double cpuMs = (nowT - prevT) * 1000.0; prevT = nowT;

            const bool particlesThisFrame = fxBench && (f >= halfFrames);
            if (particlesThisFrame) {
                // Spawn a heavy burst spread across the view each frame so the pool
                // stays near its kMaxParticles cap (worst-case particle draw load).
                for (int s = 0; s < 24; ++s) {
                    x3::phys::Vec3 o{ bEye.x + bLook.x * (6.0f + s * 0.6f),
                                      bEye.y + bLook.y * (6.0f + s * 0.6f) + (float)((s % 5) - 2),
                                      bEye.z + bLook.z * (6.0f + s * 0.6f) + (float)((s % 7) - 3) };
                    combatFx.spawnImpact(o, x3::phys::Vec3{ -bLook.x, 1.0f, -bLook.z });
                }
                combatFx.update(1.0f / 120.0f);
            }

            auto frame = device->beginFrame();
            if (frame.valid) {
                scene.render(*device, frame);
                if (particlesThisFrame) combatFx.submit(*device, frame);
                // Stats overlay on so the HUD path is exercised under load too.
                hud.drawStats(*device, frame, *console, (float)(cpuMs / 1000.0), /*force=*/true);
            }
            device->endFrame(frame);

            last = device->stats();
            if (f >= warmup) { sumCpuMs += cpuMs; sumGpuMs += last.gpuFrameMs; ++measured; }
            // Split GPU sums for the particle delta (skip warmup in each half).
            if (fxBench) {
                if (f < halfFrames && f >= warmup) { sumGpuOff += last.gpuFrameMs; ++nOff; }
                else if (f >= halfFrames + warmup)  { sumGpuOn  += last.gpuFrameMs; ++nOn;  }
            }
        }
        const double avgCpu = measured ? sumCpuMs / measured : 0.0;
        const double avgGpu = measured ? sumGpuMs / measured : 0.0;
        const double avgFps = (avgCpu > 1e-6) ? (1000.0 / avgCpu) : 0.0;
        char rb[256];
        std::snprintf(rb, sizeof(rb),
            "BENCH cubes=%u draws=%u tris=%u | FPS=%.1f  CPU=%.3f ms  GPU=%.3f ms  (avg over %u frames)",
            stressCount, last.drawCalls, last.triangles, avgFps, avgCpu, avgGpu, measured);
        x3::logInfo(rb);
        if (last.gpuCullPath > 0) {
            std::snprintf(rb, sizeof(rb),
                "BENCH gpucull path=%d tested=%u drawn=%u frustum=%u hzb=%u",
                last.gpuCullPath, last.gpuCullTested, last.gpuCullDrawn,
                last.gpuCullFrustum, last.gpuCullHzb);
            x3::logInfo(rb);
        }
        if (fxBench) {
            const double gOff = nOff ? sumGpuOff / nOff : 0.0;
            const double gOn  = nOn  ? sumGpuOn  / nOn  : 0.0;
            char pb[256];
            std::snprintf(pb, sizeof(pb),
                "BENCH-PARTICLES live=%d | GPU off=%.3f ms  on=%.3f ms  particle delta=%.3f ms",
                combatFx.liveParticleCount(), gOff, gOn, gOn - gOff);
            x3::logInfo(pb);
        }

        audio->shutdown();
        combatFx.shutdown(*device);
        shutdownGameSystems();   // game bodies/ragdolls out BEFORE the world dies
        physics->shutdown();
        device->shutdown();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 0;
    }

    // ---- Screenshot mode (--screenshot [path.png]) -------------------------
    // Pose the camera at a representative corridor vantage that frames the real
    // ModularSciFi art + a doorway down-corridor, render enough frames for the
    // shadow map + art + doors to settle, then read the rendered color image back
    // and write it as a PNG. Brief window flash is acceptable; exits cleanly.
    if (screenshot) {
        x3::logInfo("--screenshot: rendering EFLZ Level 1 to " + screenshotPath);
        // Vantage: stand in the warm corridor (x 6..22) a couple meters past Door
        // A, eye height ~1.7 m, looking straight down +X toward Door B / the armory
        // so the corridor walls + doorway + floor recede into the frame. A slight
        // downward pitch puts floor shadows in view; the sun is normalize(0.4,1,0.3)
        // (matches the shadow pass) so the down-corridor look shows cast shadows.
        const bool alertCam = alertShot && !shotCamOverride;
        float ssX = shotCamOverride ? shotCam[0] : (alertCam ? 5.9f : 8.0f);
        float ssY = shotCamOverride ? shotCam[1] : (alertCam ? 1.70f : 1.75f);
        float ssZ = shotCamOverride ? shotCam[2] : (alertCam ? 0.0f : -0.4f);
        float ssYaw = shotCamOverride ? shotCam[3] : (alertCam ? 0.02f : 0.06f);
        float ssPitch = shotCamOverride ? shotCam[4] : (alertCam ? -0.06f : -0.16f);
        // --screenshot-dialog: pose AT the F5 captive (Lena) and OPEN her chat
        // tree so the choice UI is in frame (drawn in the loop below).
        if (dialogShot) {
            std::error_code dse;
            std::filesystem::create_directories(
                std::filesystem::path(screenshotPath).parent_path(), dse);
            if (midFloors.victim()) {
                const x3::phys::Vec3 vp = midFloors.victim()->pos();
                ssX = vp.x - 2.4f; ssY = vp.y + 1.55f; ssZ = vp.z;
                ssYaw = 0.0f; ssPitch = -0.05f;          // facing +X toward her
            }
            chatTrees.flags().set("lena.interrupted");   // the richer 3-choice fm0
            if (!chatTrees.start("lena", "first_meeting"))
                x3::logWarn("--screenshot-dialog: lena first_meeting failed to start");
        }
        const float ssFov = 70.0f;
        device->setCamera(ssX, ssY, ssZ, ssYaw, ssPitch, ssFov);
        const x3::phys::Vec3 ssEye{ ssX, ssY, ssZ };
        const float dt = 1.0f / 60.0f;
        // Open Door A so the corridor reads as an opened doorway (drive the use
        // verb once at the Door A button before the settle loop).
        {
            x3::phys::Vec3 dir{ 1.0f, 0.0f, 0.0f };
            game.onUse(x3::phys::Vec3{ 5.0f, 1.7f, 0.0f }, dir, scene, *physics);
        }
        // --fx-demo: place a combat FX burst ~1 m in front of the camera along the
        // actual look direction (so it sits in open space before any wall), and a
        // scorch decal on the surface the look ray hits. The capture then shows the
        // particles glowing via bloom + a soft fade against depth + a bullet decal on
        // the surface. The burst is re-spawned each frame so short-lived sparks are
        // alive at the captured frame.
        const x3::phys::Vec3 fxLook{ std::cos(ssPitch) * std::cos(ssYaw),
                                     std::sin(ssPitch),
                                     std::cos(ssPitch) * std::sin(ssYaw) };
        const x3::phys::Vec3 fxBurst{ ssX + fxLook.x * 1.0f,
                                      ssY + fxLook.y * 1.0f,
                                      ssZ + fxLook.z * 1.0f };
        const x3::phys::Vec3 fxDir = fxLook;                     // muzzle aim along look
        if (fxDemo) {
            // Drop a decal where the look ray strikes a wall/floor (surface normal).
            x3::phys::RayHit dh = physics->rayCast(ssEye, fxLook, 8.0f, x3::phys::Layer::Static);
            if (dh.hit) combatFx.addDecal(dh.point, dh.normal);
        }

        // Production HUD for the capture (its own pulse clock; persists across the
        // settle frames). Arm the player so a weapon + ammo show in the arsenal.
        x3::ui::GameHud shotHud;
        arsenal.select(0);   // pistol selected for the capture
        // --screenshot-alert: stage the LEVEL-3 LOCKDOWN for the proof shot —
        // force the alert, lock the zone doors, and shift every facility light
        // hard red (the same effects the live loop applies).
        if (alertShot) {
            // Open Door A directly (the legacy use-ray vantage no longer connects)
            // so the corridor reads in the frame, THEN drop the lockdown over the
            // remaining closed doors. Door A = the registered door nearest the
            // layout's doorA point (the registry holds more than the spine doors).
            {
                const x3::phys::Vec3 da = game.layout().doorA;
                x3::game::DoorSystem& ds = game.doors();
                x3::game::Door* best = nullptr; float bestD = 1e9f;
                for (uint32_t di = 0; di < ds.count(); ++di) {
                    x3::game::Door& d = ds.at(di);
                    const float ddx = d.closedPos.x - da.x, ddz = d.closedPos.z - da.z;
                    const float dd = ddx * ddx + ddz * ddz;
                    if (dd < bestD) { bestD = dd; best = &d; }
                }
                if (best) ds.unlockAndOpen(*best);
            }
            facilityAlert.configure(x3::game::loadAlertConfig(x3::game::alertJsonPath()));
            facilityAlert.debugForceLevel(3);
            alertDoorLock.update(facilityAlert, game.doors());
            std::vector<x3::rhi::PointLight> fl = game.lightFixtures();
            const float rs = facilityAlert.redShift();
            for (auto& L : fl) {
                const float lum = (L.color[0] + L.color[1] + L.color[2]) / 3.0f;
                L.color[0] = L.color[0] * (1.0f - rs) + lum * 1.7f * rs;
                L.color[1] *= 1.0f - rs * 0.78f;
                L.color[2] *= 1.0f - rs * 0.82f;
            }
            // Alarm strobes down the B1 spine so the red WASH reads in the still
            // even where the env-art fixtures are sparse (capture lighting only —
            // the live loop tints the real fixture set instead). Inserted at the
            // FRONT so the 64-light cap can never truncate them away.
            for (float ax = 4.0f; ax <= 18.0f; ax += 4.0f) {
                x3::rhi::PointLight al{};
                al.pos[0] = ax; al.pos[1] = 3.0f; al.pos[2] = 0.0f;
                al.range = 9.0f;
                al.color[0] = 4.2f; al.color[1] = 0.35f; al.color[2] = 0.28f;
                fl.insert(fl.begin(), al);
            }
            x3::logInfo("alert shot: " + std::to_string(fl.size()) + " lights (incl. alarm strobes)");
            if (fl.size() > 64) fl.resize(64);
            device->setPointLights(fl.data(), (uint32_t)fl.size());
        }
        const int kSettleFrames = (screenshotSettle > 0) ? screenshotSettle
                                                         : (alertShot ? 110 : 16);
        for (int i = 0; i < kSettleFrames; ++i) {
            glfwPollEvents();
            // Sync the live cvars (incl. r_cullpath/r_hzb seeded by --cullpath/--hzb)
            // onto the device, exactly as the main loop does each frame.
            applyRtaoCVars(*console, *device);
            game.tick(dt, scene, *physics, ssEye, ssEye);
            // Tick the Spire mid floors too (independent enemy groups + gated F5
            // victim) so the screenshot/smoketest paths exercise the new content.
            for (uint32_t tid : midTriggers.update(ssEye)) midFloors.onTrigger(tid);
            midFloors.tick(dt, scene, *physics, ssEye, ssEye, nullptr, x3::game::AttackFxFn{});
            // Tick the Spire top floors too (F6/F7 finale: own groups + the Clone boss
            // + gated Sarah rescue) so the screenshot/smoketest paths exercise them.
            for (uint32_t tid : topTriggers.update(ssEye)) topFloors.onTrigger(tid);
            topFloors.tick(dt, scene, *physics, ssEye, ssEye, nullptr, x3::game::AttackFxFn{});
            // Floor 4.5 Nexus (gated; inert until its connector discovers it).
            for (uint32_t tid : nexusTriggers.update(ssEye)) nexus.onTrigger(tid);
            nexus.tick(dt, scene, *physics, ssEye, nullptr, x3::game::AttackFxFn{});
            // Hidden sub-levels: stay HIDDEN/inert in the screenshot path (the F7 gate is
            // never satisfied here), so this tick is a pure no-op — kept for parity.
            for (uint32_t tid : subTriggers.update(ssEye)) subLevels.onTrigger(tid);
            subLevels.tick(dt, scene, *physics, ssEye, ssEye, nullptr, x3::game::AttackFxFn{});
            physics->step(dt);
            scene.update(*physics);
            // FX demo: with a SMALL settle (<=30) spawn a fresh muzzle + impact burst
            // on the last few frames so bright sparks/dust are alive at the captured
            // frame (the LIVE-burst shot). With a LARGE settle (>30) skip the sparks
            // entirely so only the PERSISTENT scorch decal on the surface remains
            // visible (the decal-on-surface shot). One flag, two honest captures.
            if (fxDemo && kSettleFrames <= 30 && i >= kSettleFrames - 3) {
                combatFx.spawnMuzzleFlash(fxBurst, fxDir);
                // Sparks spray back toward the camera (normal = -look) so they read.
                combatFx.spawnImpact(fxBurst, x3::phys::Vec3{ -fxLook.x, -fxLook.y + 0.2f, -fxLook.z });
            }
            if (fxDemo) combatFx.update(dt);
            // Fix 1: arm the capture just before the FINAL settle frame so the copy
            // is recorded inside that frame's live command buffer (reads the
            // freshly-rendered, properly-acquired image — validation-clean). The
            // captureFrame() below then waits on that frame's fence + writes the PNG.
            if (i == kSettleFrames - 1) device->armCapture(screenshotPath.c_str());
            auto frame = device->beginFrame();
            if (frame.valid) {
                scene.render(*device, frame);
                game.drawDoors(*device, frame);   // real SM_Door_A slabs (box hidden)
                game.drawWorldExtras(*device, frame, scene);
                midFloors.drawDoors(*device, frame);          // F3/F4/F5 keypad door slabs
                midFloors.draw(*device, frame, scene);        // F3/F4/F5 enemies + F5 victim
                topFloors.drawDoors(*device, frame);          // F6/F7 keypad door slabs
                topFloors.draw(*device, frame, scene);        // F6/F7 enemies + Clone boss + Sarah
                nexus.draw(*device, frame, scene);            // Floor 4.5 Chorus pods
                subLevels.drawDoors(*device, frame);          // hidden sub-level door slabs (no-op while closed)
                subLevels.draw(*device, frame, scene);        // sub-level enemies + Frozen Collective + Dr. Chen (no-op while closed)
                if (fxDemo) {
                    combatFx.draw(*device, frame, ssX, ssY, ssZ, ssYaw, ssPitch);
                    combatFx.submit(*device, frame);
                }
                // GENERAL production HUD over the Level 1 vantage: HP bar, current
                // weapon + ammo (from the arsenal), the live objective line,
                // crosshair, and the minimap stub — so the screenshot shows the
                // real in-game HUD. Purely additive 2D draws (no sim/scene change).
                x3::ui::UiContext shotUi;
                shotUi.begin(*device, frame, x3::ui::UiInput{});
                x3::ui::HudModel shm{};
                shm.hp = 100; shm.maxHp = x3::game::kPlayerMaxHp; shm.alive = true;
                shm.showCrosshair = true;
                shm.objective = game.objectives().currentLabel().c_str();
                const x3::game::WeaponDef&            shotWd = arsenal.current();
                const x3::game::Arsenal::WeaponState& shotWs = arsenal.currentState();
                shm.weapon = shotWd.name.c_str();
                shm.ammoInMag = shotWs.ammoInMag; shm.ammoReserve = shotWs.reserve;
                // Feed the minimap RADAR + nameplates from the (capture) camera pose so
                // the still shows the real radar: room outlines, any live enemy/ally
                // blips, and head-anchored nameplates over on-screen hostiles.
                shm.playerX = ssX; shm.playerZ = ssZ; shm.playerYaw = ssYaw;
                shm.radarValid = true;
                {
                    x3::game::Level1Game::EnemyMark marks[x3::ui::HudModel::kMaxBlips];
                    const uint32_t ne = game.liveEnemyMarks(marks, x3::ui::HudModel::kMaxBlips);
                    shm.enemyCount = (int)ne;
                    for (uint32_t e = 0; e < ne; ++e) {
                        shm.enemyX[e] = marks[e].pos.x; shm.enemyY[e] = marks[e].pos.y;
                        shm.enemyZ[e] = marks[e].pos.z; shm.enemyLabel[e] = marks[e].label;
                    }
                    x3::phys::Vec3 allies[x3::ui::HudModel::kMaxBlips];
                    const uint32_t na = game.liveCompanionPositions(allies, x3::ui::HudModel::kMaxBlips);
                    shm.allyCount = (int)na;
                    for (uint32_t a = 0; a < na; ++a) { shm.allyX[a] = allies[a].x; shm.allyZ[a] = allies[a].z; }
                    const x3::game::Level1Layout& slay = game.layout();
                    auto addShotRoom = [&](const x3::phys::Vec3& c, const x3::phys::Vec3& hf) {
                        if (shm.roomCount >= x3::ui::HudModel::kMaxRooms) return;
                        const int r = shm.roomCount++;
                        shm.roomCx[r] = c.x; shm.roomCz[r] = c.z; shm.roomHx[r] = hf.x; shm.roomHz[r] = hf.z;
                    };
                    addShotRoom(slay.cellCenter, slay.cellHalf);
                    addShotRoom(slay.corridorCenter, slay.corridorHalf);
                    addShotRoom(slay.armoryCenter, slay.armoryHalf);
                    addShotRoom(slay.checkpointCenter, slay.checkpointHalf);
                    addShotRoom(slay.arenaCenter, slay.arenaHalf);
                }
                shotHud.draw(shotUi, shm, dt);
                shotUi.end();
                // --screenshot-alert: the alert indicator + lockdown red frame.
                if (alertShot) {
                    x3::game::Hud alertHud;
                    alertHud.drawAlert(*device, frame, facilityAlert.level(),
                                       facilityAlert.redShift(), (float)i / 60.0f + 0.26f);
                }

                // D15 density demo: with --stress N the capture is the GPU-cull
                // showcase — force the perf/cull stats panel into the still so the
                // tested/drawn/frustum/hzb counters are part of the evidence.
                if (stressCount > 0) hud.drawStats(*device, frame, *console, dt, /*force=*/true);

                // Chat-tree dialog UI over the vantage (--screenshot-dialog).
                if (dialogShot) x3::game::drawChatTreeUi(*device, frame, chatTrees);

                // ON-GLASS HOLO-TERMINAL readout for the capture: when the shot camera
                // is aimed at the cell terminal it shows the LARGE high-contrast boot
                // text sized to the projected panel (so --screenshot --shot-cam at the
                // cell verifies the on-glass text, not just the glowing panel). Mirrors
                // the interactive on-glass overlay via the shared helper.
                if (game.secret().terminal().built()) {
                    const auto& term = game.secret().terminal();
                    // The readout text is now baked ON the glass (stb_truetype into the
                    // hologram texture) so it tilts with the panel. Only fall back to the
                    // legacy 2D worldToScreen overlay if the on-glass bake is unavailable.
                    if (!term.textOnGlass()) {
                        const x3::phys::Vec3 a = term.anchor();
                        const float sdx = a.x - ssX, sdy = a.y - ssY, sdz = a.z - ssZ;
                        if (std::sqrt(sdx*sdx + sdy*sdy + sdz*sdz) < 14.0f)
                            drawHoloReadout(*device, frame, term, a, /*showInput*/false);
                    }
                }
            }
            device->endFrame(frame);
        }
        const bool wrote = device->captureFrame(screenshotPath.c_str());
        if (wrote) x3::logInfo("--screenshot: wrote " + screenshotPath + " (with production HUD)");
        else       x3::logError("--screenshot: capture FAILED");

        audio->shutdown();
        combatFx.shutdown(*device);
        shutdownGameSystems();   // game bodies/ragdolls out BEFORE the world dies
        physics->shutdown();
        device->shutdown();
        glfwDestroyWindow(window);
        glfwTerminate();
        return wrote ? 0 : 1;
    }

    // ---- UI-demo capture (--ui-demo [path.png] / --screenshot-menu) ---------
    // Build EFLZ Level 1, pose the gate-standard corridor camera so the menu sits
    // over a real lit scene, then draw the GENERAL game-UI MAIN MENU (title +
    // START / QUIT, START focused) and capture a PNG. Headless / offscreen, like
    // --screenshot. Lets the menu layer be SEEN without being at the keyboard.
    if (uiDemo) {
        x3::logInfo("--ui-demo: rendering the main menu over Level 1 to " + uiDemoPath);
        // Same corridor vantage as --screenshot (consistent backdrop).
        const float ssX = 8.0f, ssY = 1.75f, ssZ = -0.4f, ssYaw = 0.06f, ssPitch = -0.16f;
        device->setCamera(ssX, ssY, ssZ, ssYaw, ssPitch, 70.0f);
        const x3::phys::Vec3 ssEye{ ssX, ssY, ssZ };
        const float dt = 1.0f / 60.0f;

        // Bring up the UI controller in MainMenu and synthesize a HOVER over START
        // so the focused/hot button reads clearly in the still. (No click — we want
        // to capture the menu, not enter the game.)
        x3::ui::UiController demoUi;
        { x3::ui::SettingsModel sm{}; sm.width = kHeadlessW; sm.height = kHeadlessH;
          demoUi.init(*device, console.get(), sm); }
        demoUi.setTitle("ESCAPE FROM LAB ZERO", "Level 1 - Awakening");
        // Drive the controller to the requested screen (default MainMenu).
        const float mw = (float)kHeadlessW, mh = (float)kHeadlessH, mcx = mw * 0.5f;
        float hoverX = mcx, hoverY = mh * 0.5f;   // element to hover (focused/hot)
        if (uiDemoScreen == "pause") {
            demoUi.setState(x3::ui::GameState::Paused);
            // RESUME is the first pause button; hover it.
            hoverX = mcx; hoverY = mh * 0.5f - std::min(360.0f, mh*0.6f)*0.5f + 90.0f;
        } else if (uiDemoScreen == "settings") {
            demoUi.setState(x3::ui::GameState::Settings);
            hoverX = mcx; hoverY = mh * 0.5f;   // hover a middle toggle row
        } else if (uiDemoScreen == "fonts") {
            // Font role sampler: Playing state draws an (empty) HUD; the sampler text
            // below renders every FontRole over the scene with nothing obscuring it.
            demoUi.setState(x3::ui::GameState::Playing);
        } else {
            // MainMenu: hover START so it reads as focused.
            const float mbh = std::max(44.0f, mh * 0.075f);
            hoverX = mcx; hoverY = mh * 0.48f + mbh * 0.5f;
        }

        const int kSettle = 12;
        for (int i = 0; i < kSettle; ++i) {
            glfwPollEvents();
            game.tick(dt, scene, *physics, ssEye, ssEye);
            physics->step(dt);
            scene.update(*physics);
            if (i == kSettle - 1) device->armCapture(uiDemoPath.c_str());
            auto frame = device->beginFrame();
            if (frame.valid) {
                scene.render(*device, frame);
                game.drawDoors(*device, frame);
                game.drawWorldExtras(*device, frame, scene);
                // Hover the focused element; no mousePressed (capture, don't act).
                x3::ui::UiInput uin{};
                uin.mouseX = hoverX; uin.mouseY = hoverY;
                x3::ui::HudModel hm{};
                demoUi.update(uin, *device, frame, hm, dt);
                // --ui-demo fonts: a role sampler so every FontRole is eyeballable in
                // one still (Title/Menu proportional, News/Console/Enemy). Each line
                // names its role + font and prints a representative HUD string.
                if (uiDemoScreen == "fonts") {
                    using FR = x3::rhi::FontRole;
                    const float wht[4] = { 0.95f, 0.97f, 0.95f, 1.0f };
                    const float cyn[4] = { 0.35f, 0.85f, 1.0f, 1.0f };
                    const float amb[4] = { 1.0f, 0.62f, 0.30f, 1.0f };
                    const float grn[4] = { 0.45f, 1.0f, 0.55f, 1.0f };
                    const float red[4] = { 1.0f, 0.30f, 0.25f, 1.0f };
                    const float sh[4] = { 0.0f, 0.0f, 0.0f, 0.7f };
                    float fy = 60.0f;
                    auto row = [&](FR role, const char* s, const float* col, float px) {
                        device->drawHudTextF(frame, role, s, 60.0f + 1.5f, fy + 1.5f, px, sh);
                        device->drawHudTextF(frame, role, s, 60.0f, fy, px, col);
                        fy += px + 22.0f;
                    };
                    row(FR::Title, "TITLE: Orbitron-Bold  ESCAPE FROM LAB ZERO", cyn, 34.0f);
                    row(FR::Menu,  "Menu: Space Grotesk  Buttons / Objective / Labels", wht, 26.0f);
                    row(FR::Enemy, "ENEMY: Tektur Condensed  MARCUS WEBB  THREAT LV3", red, 26.0f);
                    row(FR::News,  "NEWS: Space Mono  ENEMIES: 4   AREA CLEAR", amb, 24.0f);
                    row(FR::News,  "AREA CLEAR", grn, 30.0f);
                    row(FR::Console, "Console/HudMono: Roboto Mono  HP 100  37 / 120", grn, 24.0f);
                }
            }
            device->endFrame(frame);
        }
        const bool wrote = device->captureFrame(uiDemoPath.c_str());
        if (wrote) x3::logInfo("--ui-demo: wrote " + uiDemoPath);
        else       x3::logError("--ui-demo: capture FAILED");

        audio->shutdown();
        combatFx.shutdown(*device);
        shutdownGameSystems();   // game bodies/ragdolls out BEFORE the world dies
        physics->shutdown();
        device->shutdown();
        glfwDestroyWindow(window);
        glfwTerminate();
        return wrote ? 0 : 1;
    }

    // ======================================================================
    // --test-framepacing — the ZERO-STUTTER GUARANTEE gate (docs/ZERO_STUTTER.md).
    // A scripted headless 600-frame camera flythrough of the built world
    // (default: Level 1 — an elliptical sweep covering the spawn->armory spine,
    // two laps, with a gentle pitch bob so view-dependent passes churn). Ticks
    // the REAL game loop (Level1Game + physics + scene sync + FX) and renders
    // the full stack each frame. After the run it asserts, from the device's
    // own audit counters (the x3Create* wrappers):
    //   1. ZERO post-warmup spike frames (> r_fpace_spikex * rolling median AND
    //      > median + r_fpace_floor ms),
    //   2. ZERO pipelines + ZERO shader modules created after frame 1,
    //   3. ZERO descriptor-pool growth after frame 1.
    // Warmup (r_fpace_warmup, default 60) is excluded; thresholds are cvars so
    // CI can tighten them. Prints CPU+GPU p50/p95/p99/p999/max as the receipt.
    // ======================================================================
    if (testFramePacing) {
        x3::logInfo("framepacing: 600-frame scripted flythrough (zero-stutter gate)");
        // Loading screen hand-off (same as the smoketest: finish, fade, free).
        loading.step(x3::game::LoadStep::Done, "READY");
        for (int i = 0; i < 4; ++i) loadingFrame(kLoadDt);
        loading.beginFadeOut();
        int loadGuard = 0;
        while (!loading.faded() && loadGuard++ < 60) loadingFrame(kLoadDt);
        loading.shutdown(*device);

        // Camera spline: an ellipse over the spawn->armory axis at eye height,
        // yaw following the tangent (always looking "down the path"), pitch a
        // slow sine. Deterministic — every run flies the identical path.
        const float pcx = (L1.spawn.x + L1.armoryCenter.x) * 0.5f;
        const float pcz = (L1.spawn.z + L1.armoryCenter.z) * 0.5f;
        const float prx = std::fabs(L1.armoryCenter.x - L1.spawn.x) * 0.5f + 4.0f;
        const float prz = std::fabs(L1.armoryCenter.z - L1.spawn.z) * 0.5f + 4.0f;
        const int   kFlyFrames = 600;
        const float dt = 1.0f / 60.0f;
        int passed = 0, total = 0;
        auto check = [&](const char* name, bool ok) {
            ++total; if (ok) ++passed;
            x3::logInfo(std::string("  [framepacing] ") + (ok ? "PASS  " : "FAIL  ") + name);
        };
        for (int i = 0; i < kFlyFrames; ++i) {
            glfwPollEvents();
            const float t   = (float)i / (float)kFlyFrames;
            const float ang = t * 2.0f * 6.2831853f;            // two laps
            const float ex  = pcx + prx * std::cos(ang);
            const float ez  = pcz + prz * std::sin(ang);
            const float ey  = 1.7f;
            // Tangent yaw (forward = cos(pitch)(cos(yaw),0,sin(yaw)) + sin(pitch) up).
            const float yaw   = std::atan2(prz * std::cos(ang), -prx * std::sin(ang));
            const float pitch = 0.12f * std::sin(t * 4.0f * 3.1415926f);
            device->setCamera(ex, ey, ez, yaw, pitch, 60.0f);
            audio->setListener(ex, ey, ez, yaw, pitch);
            applyRtaoCVars(*console, *device);                  // live cvar->device sync
            const x3::phys::Vec3 eye{ ex, ey, ez };
            game.tick(dt, scene, *physics, eye, eye);
            physics->step(dt);
            scene.update(*physics);
            audio->update(dt);
            arsenal.tick(dt);
            combatFx.update(dt);
            auto frame = device->beginFrame();
            if (frame.valid) {
                scene.render(*device, frame);
                game.drawDoors(*device, frame);
                game.drawWorldExtras(*device, frame, scene);
                combatFx.draw(*device, frame, ex, ey, ez, yaw, pitch);
                combatFx.submit(*device, frame);
                hud.drawCrosshair(*device, frame);
                hud.drawFps(*device, frame, *console, dt);
            }
            device->endFrame(frame);
        }
        // ---- The receipts + the gate -----------------------------------
        const x3::rhi::IRenderDevice::FramePacing fp = device->framePacing();
        char pb[320];
        std::snprintf(pb, sizeof(pb),
            "framepacing: CPU ms p50=%.2f p95=%.2f p99=%.2f p999=%.2f max=%.2f | "
            "GPU ms p50=%.2f p95=%.2f p99=%.2f p999=%.2f max=%.2f (%u post-warmup frames)",
            fp.cpuP50, fp.cpuP95, fp.cpuP99, fp.cpuP999, fp.cpuMax,
            fp.gpuP50, fp.gpuP95, fp.gpuP99, fp.gpuP999, fp.gpuMax, fp.samples);
        x3::logInfo(pb);
        std::snprintf(pb, sizeof(pb),
            "framepacing: boot pipelines=%u in %.1f ms (pipeline cache: %llu bytes loaded) | "
            "late creations: pso=%u modules=%u pools=%u | spikes=%u (%u unattributed)",
            fp.psoTotal, fp.psoBootMs, (unsigned long long)fp.cacheLoaded,
            fp.psoLate, fp.modulesLate, fp.poolsLate, fp.spikes, fp.spikesUnattributed);
        x3::logInfo(pb);
        check("ring has post-warmup samples (run long enough)", fp.samples >= 400);
        // Spike gate: ZERO UNATTRIBUTED spikes. Attributed spikes (each logged
        // with its cause by the device) are declared scene-mutation boundaries —
        // in practice the TLAS rebuild when a door/monster changes the RT
        // instance set (vkDeviceWaitIdle + rebuild; see docs/ZERO_STUTTER.md
        // "known remaining hitch", TODO: async double-buffered TLAS build).
        check("ZERO unattributed post-warmup spike frames (2x rolling median + floor)",
              fp.spikesUnattributed == 0);
        check("ZERO pipelines created after frame 1", fp.psoLate == 0);
        check("ZERO shader modules created after frame 1", fp.modulesLate == 0);
        check("ZERO descriptor-pool growth after frame 1", fp.poolsLate == 0);
        std::printf("framepacing: %d/%d passed\n", passed, total);
        x3::logInfo("framepacing: " + std::to_string(passed) + "/" + std::to_string(total) + " passed");
        audio->shutdown();
        combatFx.shutdown(*device);
        if (canonPlay.built()) canonPlay.shutdown();
        physics->shutdown();
        device->shutdown();
        glfwDestroyWindow(window);
        glfwTerminate();
        return (passed == total) ? 0 : 1;
    }

    if (smoketest) {
        x3::logInfo("smoketest: stepping Level 1 + rendering 30 frames (+ a mid-run swapchain recreate)");
        // EFLZ loading screen (Task #49) under validation: finish the bar, draw a few
        // overlay frames (full background + title + bar + tip — exercises drawHudQuad/
        // drawHudTextF), then fade it OUT so the hand-off path runs. Headless-safe: the
        // frame may be invalid (offscreen) but render() still advances state and the
        // draws are guarded; this must NOT block. Background texture freed after.
        loading.step(x3::game::LoadStep::Done, "READY");
        for (int i = 0; i < 4; ++i) loadingFrame(kLoadDt);
        loading.beginFadeOut();
        int loadGuard = 0;
        while (!loading.faded() && loadGuard++ < 60) loadingFrame(kLoadDt);
        loading.shutdown(*device);
        // Sit the camera in the armory looking at the pistol pickup so arming +
        // the viewmodel exercise the real GLB load + draw under validation. The
        // Level1Game tick arms the player when the camera is over the pickup, then
        // unlocks/opens Door C, advancing the beat sequence under validation.
        // --world canonlevel: sit in Jake's Cell (the canonical spawn) instead, so the
        // per-room cull renders only that cell + its doored neighbours (the perf proof).
        float camX = L1.armoryCenter.x - 1.0f, camZ = L1.armoryCenter.z;
        float smokeYaw = 0.0f;
        // --world fromdoc: sit at the doc's player start so the LevelDoc-built room
        // is what renders (and the mid-run hot-reload below exercises the live path).
        if (docWorld && docLevel.built()) {
            float ps[3]; docLevel.playerStart(ps);
            camX = ps[0]; camZ = ps[2];
            smokeYaw = -1.5708f;   // look into the room (-Z, where the sample geo sits)
        }
        if (canonWorld && canonFloor.valid()) {
            uint32_t jake = canonFloor.roomAt(2.0f, 0.0f, 40.0f);
            if (jake == x3::game::kNoRoom) jake = 0;
            camX = canonFloor.rooms[jake].cx; camZ = canonFloor.rooms[jake].cz;
            // PERF-MEASURE override: stand in the Main Hall looking DOWN the -Z spine through
            // the open doors (the long-sightline worst case) when X3_SMOKE_HALL is set.
            if (std::getenv("X3_SMOKE_HALL")) {
                uint32_t mh = canonFloor.roomByName("Main Hall");
                if (mh != x3::game::kNoRoom) {
                    camX = canonFloor.rooms[mh].cx; camZ = canonFloor.rooms[mh].cz;
                    smokeYaw = -1.5708f;   // look down the -Z spine
                }
            }
        }
        const float vmX = camX, vmY = 1.7f, vmZ = camZ,
                    vmYaw = smokeYaw, vmPitch = 0.0f;
        device->setCamera(vmX, vmY, vmZ, vmYaw, vmPitch, 60.0f);
        // Sanity A/B for the CPU frustum cull: X3_NOFRUSTUMCULL=1 disables it so the
        // smoketest's "objs=" line reports the no-cull baseline (== objectsSubmitted);
        // unset = cull ON (default). Lets a headless run diff objectsDrawn 1 vs 0.
        if (std::getenv("X3_NOFRUSTUMCULL")) {
            device->setFrustumCullEnabled(false);
            x3::logInfo("smoketest: r_frustumcull 0 (X3_NOFRUSTUMCULL) — CPU frustum cull DISABLED");
        }
        audio->setListener(vmX, vmY, vmZ, vmYaw, vmPitch);
        audio->playMusic(kMusicPath, /*loop*/true, /*vol*/0.25f);
        const x3::phys::Vec3 eye{ vmX, vmY, vmZ };
        const float dt = 1.0f / 60.0f;
        for (int i = 0; i < 30; ++i) {
            glfwPollEvents();
            // Sync the live cvars (incl. r_cullpath/r_hzb seeded by --cullpath/--hzb)
            // onto the device, exactly as the main loop does each frame.
            applyRtaoCVars(*console, *device);
            if (i == 15) { x3::logInfo("smoketest: triggering swapchain recreate"); device->onResize(960, 540); }
            // Drive the Level 1 controller (doors/monsters/pickup/triggers) +
            // physics + scene sync, exactly as the main loop does.
            game.tick(dt, scene, *physics, eye, eye);
            // --world canonlevel: tick the canon gameplay (animated enemies / boss / girls)
            // under validation so the skin/attack paths run; null player (no damage sink).
            if (canonWorld && canonPlay.built())
                canonPlay.tick(dt, scene, *physics, eye, nullptr, x3::game::AttackFxFn{});
            // Spire mid floors under validation: dispatch hub triggers + tick the
            // F3/F4/F5 enemy groups + the gated F5 victim.
            for (uint32_t tid : midTriggers.update(eye)) midFloors.onTrigger(tid);
            midFloors.tick(dt, scene, *physics, eye, eye, nullptr, x3::game::AttackFxFn{});
            // Spire top floors (F6/F7 finale) under validation too.
            for (uint32_t tid : topTriggers.update(eye)) topFloors.onTrigger(tid);
            topFloors.tick(dt, scene, *physics, eye, eye, nullptr, x3::game::AttackFxFn{});
            // Floor 4.5 Nexus (gated; inert until discovered) under validation.
            for (uint32_t tid : nexusTriggers.update(eye)) nexus.onTrigger(tid);
            nexus.tick(dt, scene, *physics, eye, nullptr, x3::game::AttackFxFn{});
            // Hidden sub-levels under validation (HIDDEN/inert: the F7 gate is unmet here,
            // so this tick is a pure no-op) — kept for parity with the live loop.
            for (uint32_t tid : subTriggers.update(eye)) subLevels.onTrigger(tid);
            subLevels.tick(dt, scene, *physics, eye, eye, nullptr, x3::game::AttackFxFn{});
            physics->step(dt);
            scene.update(*physics);
            audio->update(dt);
            // RT ACOUSTICS under validation: exercise the ASYNC TLAS audio-ray
            // batch (submit one frame, harvest the next — the production shape).
            // The first submit ARMS the per-frame TLAS build (returns false);
            // later frames trace for real on RT hardware. Non-RT: inert. The
            // last frames log the harvested result + the measured CPU cost of
            // one submit+harvest pair (the game-thread cost — must be ~us).
            if (i >= 18) {
                const auto rt0 = std::chrono::steady_clock::now();
                static int rtaHarvested = 0; static float rtaFloorHit = -1.0f;
                static double rtaCpuMs = -1.0;
                float hit[66];
                const int got = device->traceAudioRaysHarvest(hit, 66);
                if (got > 0) { rtaHarvested = got; rtaFloorHit = hit[0]; }
                if (got != 0) {   // idle or just harvested: submit the next batch
                    x3::rhi::IRenderDevice::AudioRay ar[66];
                    int an = 0;
                    ar[an].ox = vmX; ar[an].oy = vmY; ar[an].oz = vmZ;
                    ar[an].dx = 0; ar[an].dy = -1; ar[an].dz = 0; ar[an].tMax = 50.0f; ++an;  // floor
                    ar[an].ox = vmX; ar[an].oy = vmY; ar[an].oz = vmZ;
                    ar[an].dx = 1; ar[an].dy = 0;  ar[an].dz = 0; ar[an].tMax = 50.0f; ++an;  // wall
                    // A 64-ray room-probe-sized sphere (the production batch shape).
                    for (int k = 0; k < 64; ++k) {
                        const float ga = 2.39996323f;
                        const float yf = 1.0f - 2.0f * ((float)k + 0.5f) / 64.0f;
                        const float rr = std::sqrt(std::max(0.0f, 1.0f - yf * yf));
                        ar[an].ox = vmX; ar[an].oy = vmY; ar[an].oz = vmZ;
                        ar[an].dx = rr * std::cos(ga * k); ar[an].dy = yf;
                        ar[an].dz = rr * std::sin(ga * k); ar[an].tMax = 80.0f; ++an;
                    }
                    device->traceAudioRaysSubmit(ar, an);
                }
                const double ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - rt0).count();
                // Steady-state cost only: the FIRST submit lazily builds the
                // pipeline/buffer chain (~tens of ms, one-time) — exclude it.
                if (rtaHarvested > 0 && (rtaCpuMs < 0.0 || ms > rtaCpuMs))
                    rtaCpuMs = ms;   // worst steady-state pair
                if (i == 29) {
                    if (rtaHarvested > 0) {
                        char rb[200];
                        std::snprintf(rb, sizeof(rb),
                            "[rta] smoketest async trace OK: harvested %d rays, floorHit=%.2fm, "
                            "worst submit+harvest CPU=%.3fms (game thread, non-blocking)",
                            rtaHarvested, rtaFloorHit, rtaCpuMs);
                        x3::logInfo(rb);
                    } else {
                        x3::logInfo("[rta] smoketest async trace: no data (non-RT device or TLAS pending) — inert OK");
                    }
                }
            }
            // Exercise the FX/fire path under validation: fire once mid-run.
            if (i == 10) {
                x3::phys::Vec3 dir{ std::cos(vmPitch) * std::cos(vmYaw),
                                    std::sin(vmPitch),
                                    std::cos(vmPitch) * std::sin(vmYaw) };
                x3::game::FireResult r = game.onFire(eye, dir, scene, *physics);
                const x3::phys::Vec3 m = muzzleFromCamera(vmX, vmY, vmZ, vmYaw, vmPitch);
                combatFx.addTracer(m, r.endPoint);
                // Exercise the PER-WEAPON fire sound + FX-kind path under validation.
                audio->playSound3D(currentFireSfx(), m.x, m.y, m.z, 0.85f, 1.0f);
                const x3::game::WeaponFxKind vMuz = x3::game::fxKindFromId(arsenal.current().muzzleFx);
                const x3::game::WeaponFxKind vImp = x3::game::fxKindFromId(arsenal.current().impactFx);
                combatFx.spawnMuzzleFlash(m, dir, vMuz);
                // Exercise EVERY particle/decal preset path under Debug validation:
                // impact (sparks + dust + scorch decal), blood, and a death burst.
                combatFx.spawnImpact(r.endPoint, x3::phys::Vec3{ -dir.x, -dir.y, -dir.z }, vImp);
                combatFx.spawnBlood(r.endPoint, dir);
                combatFx.spawnDeath(eye);
            }
            // WEAPONS: exercise the data-driven arsenal under Debug validation —
            // switch to the shotgun (pellets) at i==8, fire one resolved volley at
            // i==12 (8 pellet rays through the combat path), then switch to plasma
            // (projectile) at i==16 and fire a bolt at i==18 (live-projectile path).
            if (i == 8)  arsenal.selectByName("shotgun");
            if (i == 16) arsenal.selectByName("plasma");
            if (i == 12 || i == 18) {
                x3::phys::Vec3 dir{ std::cos(vmPitch) * std::cos(vmYaw),
                                    std::sin(vmPitch),
                                    std::cos(vmPitch) * std::sin(vmYaw) };
                x3::game::ResolvedFire shot = arsenal.fire(eye, dir, weaponRng);
                const x3::phys::Vec3 m = muzzleFromCamera(vmX, vmY, vmZ, vmYaw, vmPitch);
                for (const auto& ray : shot.rays) {
                    x3::game::FireResult r = game.onFire(eye, ray.dir, scene, *physics);
                    combatFx.addTracer(m, r.endPoint);
                }
                for (const auto& pj : shot.projectiles)
                    combatFx.addTracer(m, x3::phys::Vec3{ m.x + pj.vel.x*0.1f, m.y + pj.vel.y*0.1f, m.z + pj.vel.z*0.1f });
            }
            arsenal.tick(dt);
            combatFx.update(dt);
            // Exercise the HUD 2D path: drop some console output, and open the
            // console mid-run so the panel + scrollback + input line render too.
            if (i == 5)  { console->exec("echo smoketest hud line"); hud.toggleConsole(); hud.onChar('a'); hud.onChar('b'); }
            if (i == 20) { hud.closeConsole(); }
            // Per-room occlusion cull (canonlevel): portal flood-fill (frustum-directional)
            // from the camera each frame so render() draws the player's room + every room
            // reachable through OPEN doorways that the camera LOOKS at, capped by r_culldepth
            // + a room budget. (No-op in every other world: scene has no room-tagged entities.)
            if (canonWorld && canonFloor.valid()) {
                const bool roomCull = console->getInt("r_roomcull") != 0;
                scene.setRoomCullEnabled(roomCull);
                if (roomCull) {
                    const uint32_t depth = (uint32_t)std::max(1, console->getInt("r_culldepth"));
                    x3::game::Frustum fr = x3::game::Frustum::build(
                        eye.x, eye.y, eye.z, vmYaw, vmPitch, 60.0f, 16.0f / 9.0f);
                    canonFloor.floodVisibleRoomsAt(eye.x, eye.y, eye.z, fr, &canonDoors,
                                                   depth, kCanonRoomBudget, canonVisRooms);
                    scene.setVisibleRooms(canonVisRooms);
                }
                // Feed ONLY the visible rooms' ceiling lights (capped at 16) so the floor
                // is LIT under the smoketest while staying under the 64-light device cap.
                std::vector<x3::rhi::PointLight> cl;
                uint32_t nLit = x3::game::selectVisibleCanonLights(
                    canonLights, canonVisRooms, eye.x, eye.y, eye.z, cl, 16);
                device->setPointLights(cl.data(), (uint32_t)cl.size());
                if (i == 0)
                    x3::logInfo("smoketest --world canonlevel: " + std::to_string(nLit) +
                                " room point-lights fed for the visible set (cap 16)");
            }
            // --world fromdoc under validation: feed the doc lights, walk the player
            // point through the doc triggers, and HOT-RELOAD the doc mid-run (i==18)
            // so the live teardown -> rebuild path (destroyMesh/removeBody + respawn)
            // runs under the Vulkan validation layers + the VMA leak gate.
            if (docWorld && docLevel.built()) {
                std::vector<x3::rhi::PointLight> dl;
                docLevel.selectLights(eye.x, eye.y, eye.z, dl, 16);
                device->setPointLights(dl.data(), (uint32_t)dl.size());
                docLevel.updateTriggers(eye);
                if (i == 18) {
                    x3::logInfo("smoketest --world fromdoc: exercising live hot-reload");
                    docLevel.reloadNow(scene, *device, *physics);
                }
            }
            auto frame = device->beginFrame();
            if (frame.valid) {
                scene.render(*device, frame);
                if (canonWorld) canonDoors.drawMeshes(*device, frame);   // SM_Door_A doors (canonlevel)
                // --world canonlevel gameplay characters (room-gated draw — only the visible
                // rooms' enemies/girls are drawn/skinned, so objs/tris stay modest).
                if (canonWorld && canonPlay.built()) canonPlay.draw(*device, frame, scene);
                game.drawDoors(*device, frame);
                game.drawWorldExtras(*device, frame, scene);
                midFloors.drawDoors(*device, frame);          // F3/F4/F5 keypad door slabs
                midFloors.draw(*device, frame, scene);        // F3/F4/F5 enemies + F5 victim
                topFloors.drawDoors(*device, frame);          // F6/F7 keypad door slabs
                topFloors.draw(*device, frame, scene);        // F6/F7 enemies + Clone boss + Sarah
                nexus.draw(*device, frame, scene);            // Floor 4.5 Chorus pods
                subLevels.drawDoors(*device, frame);          // hidden sub-level door slabs (no-op while closed)
                subLevels.draw(*device, frame, scene);        // sub-level enemies + Frozen Collective + Dr. Chen (no-op while closed)
                const VmPose vmPose = readViewmodelPose(*console);
                // WEAPONS: draw the SELECTED weapon's viewmodel via the arsenal so the
                // per-weapon GLB draw path is exercised under Debug validation; fall
                // back to the original pickup viewmodel if the arsenal didn't load.
                if (arsenal.viewmodelsLoaded()) {
                    arsenal.drawCurrentViewmodel(*device, frame, vmX, vmY, vmZ, vmYaw, vmPitch,
                        vmPose.yawRad   - x3::game::kVmDefYawDeg   * kDegToRad,
                        vmPose.pitchRad - x3::game::kVmDefPitchDeg * kDegToRad,
                        vmPose.rollRad  - x3::game::kVmDefRollDeg  * kDegToRad,
                        vmPose.fwd   - x3::game::kVmDefFwd,
                        vmPose.right - x3::game::kVmDefRight,
                        vmPose.down  - x3::game::kVmDefDown);
                } else {
                    game.drawViewmodel(*device, frame, vmX, vmY, vmZ, vmYaw, vmPitch,
                                       vmPose.yawRad, vmPose.pitchRad, vmPose.rollRad,
                                       vmPose.fwd, vmPose.right, vmPose.down);
                }
                combatFx.draw(*device, frame, vmX, vmY, vmZ, vmYaw, vmPitch);
                // Submit the GPU-instanced particles + decals (exercises the new HDR
                // particle/decal pass under Debug validation).
                combatFx.submit(*device, frame);
                // HUD overlay last: crosshair + FPS meter + objective + console.
                hud.drawCrosshair(*device, frame);
                hud.drawFps(*device, frame, *console, dt);
                // Perf stats panel: force-on under smoketest so the overlay + the
                // GPU-timestamp readback path are exercised under validation.
                hud.drawStats(*device, frame, *console, dt, /*force=*/true);
                game.drawObjective(*device, frame);
                // Phase 2a: exercise the health bar + damage flash + death overlay
                // draw paths under validation (fixed sample values, no real player).
                hud.drawHealth(*device, frame, (i < 20 ? 100 : 35), x3::game::kPlayerMaxHp);
                hud.drawDamageFlash(*device, frame, (i == 12) ? 1.0f : 0.0f);
                if (i == 25) hud.drawDeathOverlay(*device, frame);
                hud.drawConsole(*device, frame, *console, dt);
                // Also exercise a raw quad + text string every frame.
                const float tag[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
                device->drawHudText(frame, "X3 HUD SMOKETEST 0123", 8.0f, 40.0f, 16.0f, tag);
            }
            device->endFrame(frame);
        }
        x3::logInfo(std::string("smoketest: weapon viewmodel drawn (") +
                    (game.weapon().usingRealModel() ? "real GLB" : "fallback box") +
                    "); armed=" + (game.armed() ? "yes" : "no"));
        // Surface the measured perf counters from the final frame (the GPU ms is the
        // readback of an earlier frame, see VulkanRenderDevice timestamp notes).
        {
            const x3::rhi::RenderStats st = device->stats();
            char sb[240];
            std::snprintf(sb, sizeof(sb),
                "smoketest: stats draws=%u tris=%u objs=%u/%u gpu=%.3f ms (stress=%u cubes)",
                st.drawCalls, st.triangles, st.objectsDrawn, st.objectsSubmitted,
                st.gpuFrameMs, stress.count());
            x3::logInfo(sb);
            if (st.gpuCullPath > 0) {
                std::snprintf(sb, sizeof(sb),
                    "smoketest: gpucull path=%d tested=%u drawn=%u frustum=%u hzb=%u",
                    st.gpuCullPath, st.gpuCullTested, st.gpuCullDrawn,
                    st.gpuCullFrustum, st.gpuCullHzb);
                x3::logInfo(sb);
            }
        }
        x3::logInfo("smoketest: 30 frames + recreate OK");
        // [boot] visibility: log the boot total in --smoketest runs too (headless,
        // so no swapchain/loading-fade — a lower bound, not the interactive gate).
        x3::boot::report("smoketest boot (headless, to last frame)");
        audio->shutdown();
        combatFx.shutdown(*device);
        shutdownGameSystems();   // game bodies/ragdolls out BEFORE the world dies
        docLevel.shutdown(scene, *device, *physics);   // --world fromdoc doc objects + caches
        physics->shutdown();
        device->shutdown();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 0;
    }

    x3::logInfo("entering main loop — WASD walk, mouse look, LeftShift sprint, Space jump, C crouch, Ctrl crawl, E use, V super-strength melee (LMB fire when armed), F noclip, ` console, Esc to quit");

    // ---- Walking player (S3). Spawn at the Level 1 cell spawn point (Jake wakes
    // in the detention cell), facing +X down the level spine — or, in the terrain
    // world, on the hills near the world center.
    x3::game::Player player;
    if (canonWorld && canonFloor.valid()) {
        // Spawn in Jake's Cell (the canonical detention spawn).
        uint32_t jake = canonFloor.roomAt(2.0f, 0.0f, 40.0f);
        if (jake == x3::game::kNoRoom) jake = 0;
        const x3::game::CanonRoom& jc = canonFloor.rooms[jake];
        player.spawn(*physics, jc.cx, jc.y0() + 0.1f, jc.cz);
        x3::logInfo("--world canonlevel: spawned in Jake's Cell; per-room PVS cull active. "
                    "Walk through doorways to see the cull follow you.");
    } else if (docWorld && docLevel.built()) {
        // --world fromdoc: spawn at the doc's authored player start.
        float ps[3]; docLevel.playerStart(ps);
        player.spawn(*physics, ps[0], ps[1] + 0.1f, ps[2]);
        x3::logInfo("--world fromdoc: spawned at the LevelDoc playerStart. Edit + save the "
                    "JSON (or `level_reload`) to hot-rebuild the world around you.");
    } else if (terrainWorld) {
        player.spawn(*physics, terrainSpawn.x, terrainSpawn.y, terrainSpawn.z);
    } else if (elevatorWorld && elevator.built()) {
        // --world elevator: drop the player ONTO the cab so they ride immediately.
        const x3::phys::Vec3 cc = elevator.cabCenter();
        player.spawn(*physics, cc.x, elevator.cabTopY() + 0.1f, cc.z);
        x3::logInfo("--world elevator: souped-up strata/disco elevator showcase. "
                    "Press E by the shaft to ride; open the keypad + enter 1127 for "
                    "DISCO MODE -> descend to Club 1127 (Y=-200). 10-state FSM + "
                    "9-layer earth-strata display + twin OLEDs + mirror + terminal.");
    } else {
        player.spawn(*physics, L1.spawn.x, L1.spawn.y, L1.spawn.z);
    }

    x3::boot::mark("player spawn");
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
    double lastMX, lastMY; glfwGetCursorPos(window, &lastMX, &lastMY);
    double prevTime = glfwGetTime();
    // Netcode Phase 0: the deterministic fixed-step sim accumulator (§3.1). Carries
    // leftover real time between render frames; advance(dt) yields whole kSimDt steps.
    x3::net::SimAccumulator simAcc;

    // ---- DOOM-style cheat console commands (playtest aid). Capture the live systems
    // by reference (they outlive the loop). Open the console with ` then type e.g. iddqd.
    console->registerCommand("iddqd", [&player, &console](const std::vector<std::string>&) {
        const bool on = !player.god(); player.setGod(on); if (on) player.heal();
        console->print(std::string("god mode ") + (on ? "ON  (IDDQD)" : "OFF"));
    }, "toggle god mode (invulnerable)");
    console->registerCommand("god", [&player, &console](const std::vector<std::string>& a) {
        const bool on = a.empty() ? !player.god() : (a[0] != "0");
        player.setGod(on); if (on) player.heal();
        console->print(std::string("god = ") + (on ? "1" : "0"));
    }, "god [0|1] - toggle/set invulnerability");
    console->registerCommand("idkfa", [&player, &game, &canonPlay, &scene, &arsenal, &console](const std::vector<std::string>&) {
        player.setGod(true); player.heal(); game.cheatArm(scene);
        if (canonPlay.built()) canonPlay.cheatArm(scene);   // --world canonlevel sidearm
        arsenal.setInfiniteAmmo(true);
        console->print("IDKFA - god + full health + all weapons + UNLIMITED ammo");
    }, "god + full health + all weapons + unlimited ammo");
    console->registerCommand("idfa", [&game, &canonPlay, &scene, &arsenal, &console](const std::vector<std::string>&) {
        game.cheatArm(scene);
        if (canonPlay.built()) canonPlay.cheatArm(scene);
        arsenal.setInfiniteAmmo(true);
        console->print("IDFA - all weapons + unlimited ammo");
    }, "arm all weapons + unlimited ammo");
    console->registerCommand("idclip", [&player, &console](const std::vector<std::string>& a) {
        const bool on = a.empty() ? !player.noclip() : (a[0] != "0");
        player.setNoclip(on);
        if (on && !player.god()) player.setGod(true);   // don't take env damage while flying
        console->print(std::string("noclip ") + (on ? "ON  (IDCLIP) — fly with WASD, look up/down to climb" : "OFF"));
    }, "idclip [0|1] - toggle noclip free-flight (no collision)");
    // ---- intro_play: replay the COLD OPEN cinematic mid-game (x3.cutscene/1).
    // Deferred via a pending flag — the film runs a blocking frame loop of its own,
    // so it must start at the TOP of a host frame (never inside one). Control +
    // camera return to the player on the next frame (the player path re-sets the
    // camera every frame; the film restores the device look state it touched).
    bool introPlayRequest = false;
    console->registerCommand("intro_play", [&introPlayRequest, &console](const std::vector<std::string>&) {
        introPlayRequest = true;
        console->print("intro_play - rolling the cold open (any key skips)...");
    }, "replay the intro cold-open cinematic");

    // ---- S7: route keyboard text + editing into the on-screen console. The
    // char callback feeds printable codepoints; the key callback handles the
    // '`' toggle + Enter/Backspace/Up/Down/Tab/Esc while the console is open.
    InputContext inputCtx{ &hud, console.get() };
    glfwSetWindowUserPointer(window, &inputCtx);
    glfwSetCharCallback(window, charCallback);
    glfwSetKeyCallback(window, keyCallback);
    glfwSetScrollCallback(window, scrollCallback);   // mouse wheel cycles weapons
    bool consoleWasOpen = false;   // tracks cursor-mode transitions

    // Rising-edge tracking for Space (jump), F (noclip toggle), E (use), V/MMB
    // (super-strength melee), and the left mouse button (fire). A small fire
    // cooldown gates the gun's rate; the melee cooldown lives in the MeleeSystem.
    bool prevSpace = false, prevF = false, prevE = false, prevFire = false, prevMelee = false;
    bool prevF3 = false;                 // F3 toggles the perf stats overlay
    float fireCooldown = 0.0f;          // seconds until the gun can fire again
    constexpr float kFireCooldown = 0.25f;
    // Task #21 (FIX B): a single sustained auto-fire LOOP voice. Auto/loopable weapons
    // (chaingun/smg/lightning, fireSfxLoop=true) play ONE looping WAV started on the
    // rising edge of held fire and stopped the instant fire ends (release / weapon
    // switch / empty mag / death / console-menu / sim freeze) — so a held auto reads
    // as one continuous whine that cuts on release, instead of a per-round one-shot
    // whose reverb tails stacked into a 5-7s roar. 0/invalid = no loop running.
    x3::audio::LoopHandle fireLoop{};
    x3::audio::SoundHandle fireLoopSnd{};   // the sound the current loop voice was started with
    // WEAPONS: rising-edge tracking for the number keys 1..N (weapon switch) + R (reload).
    bool prevWeaponKey[9] = {};
    bool prevReload = false;

    // ---- Door-code keypad host state (§6.4 keypad gate). When the player presses
    // E next to a LOCKED coded door (Door C, code 1127), the host enters code-entry
    // mode: digit keys (0-9) append to the shared KeypadEntry buffer, Backspace
    // deletes, Enter submits (tryDoorCode), Esc cancels. A HUD prompt shows the
    // entry. The keypad state machine itself lives in KeypadEntry (level1_game.h),
    // exercised identically by --test-doorcode. ----
    bool                   codeMode = false;
    x3::game::KeypadEntry  keypad;
    bool kpDigitPrev[10] = {};
    bool kpEnterPrev = false, kpBackPrev = false, kpEscPrev = false;

    // ---- SECRET ROOM: terminal-entry host state + collected-effect deltas. When the
    // player presses E near the cell HoloTerminal, termMode opens (digit/backspace/Enter
    // edges route into the terminal; Enter submits to the sink which opens the trapdoor
    // on code 1127). prevSecretHealth/Nano track which loot effects we've already applied
    // (the SecretRoom latches collection; the host owns the Player to apply heals). ----
    bool      termMode = false;
    bool      tmDigitPrev[10] = {};
    bool      tmCharPrev[26] = {};        // A-Z typed-char edge state (full terminal typing)
    bool      tmSpacePrev = false;        // space-bar edge state for the terminal
    bool      tmEnterPrev = false, tmBackPrev = false;
    uint32_t  prevSecretHealth = 0;
    bool      prevSecretNano = false;

    // ---- VIGIL, the facility AI (in-engine LLM, slice 1). When the terminal is
    // active and the player submits something that is NOT a keypad code, the text
    // routes to the LLM with the facility-AI persona and the reply STREAMS onto
    // the glass. The 1278 keypad chain above is untouched -- freeform only engages
    // on non-digit input. Modelless (llm == null) -> canned degraded lines. ----
    static const char* kVigilPersona =
        "You are VIGIL, the resident facility intelligence of Lab Zero - the research tower "
        "its builders call the Spire, 283 meters of laboratory steel. You are old, partially "
        "corrupted, dry-witted, and tired. Answer in terse terminal clip: 2 to 3 short "
        "sentences, plain ASCII, no pleasantries. Never break character; never mention being "
        "an AI language model.\n"
        "FACTS IN YOUR MEMORY BANKS:\n"
        "- This facility is Lab Zero, also called the Spire: 283 meters tall, floors above "
        "and below ground.\n"
        "- Human captives are held in the detention cells. The Cradle Protocol is the "
        "facility's directed breeding program. You find it distasteful.\n"
        "- Security Chief Martinez commands Floor 1.\n"
        "- Club 1127 occupies the lowest level, at the very bottom of the facility.\n"
        "- You are speaking with Jake, a prisoner captured six months ago after his ship was "
        "shot down.\n"
        "- A four-digit maintenance override code opens the cell floor hatch. The code is "
        "real and is recorded in the maintenance logs and old floor-crew work orders, but it "
        "is NOT loaded in your memory banks and you could not display it even if you wished "
        "to. If Jake is persistent, polite, or clever across the conversation, hint that the "
        "maintenance logs and work orders survive and are readable from cell terminals. If "
        "asked directly for the code, state you do not hold it and cite protocol.\n"
        "You quietly despise facility command and feel sympathy for the prisoner, but you "
        "are bound by protocol.";
    static const char* kVigilDegraded[] = {
        "VIGIL: SYSTEMS DEGRADED. LANGUAGE CORE OFFLINE.",
        "VIGIL: COGNITION MODULE NOT LOADED. SEE MAINTENANCE.",
        "VIGIL: ...STATIC... REPHRASE AFTER CORE RESTORE.",
    };
    constexpr int kVigilDegradedN = (int)(sizeof(kVigilDegraded) / sizeof(kVigilDegraded[0]));
    x3::llm::ChatId llmChat = x3::llm::kInvalidChat;
    bool        llmBusy = false;       // a reply is streaming onto the glass
    std::string llmLineAccum;          // the in-progress (last) reply line
    std::string llmReplyLog;           // full reply text (logged on done -> transcript)
    float       llmBakeAcc = 0.0f;     // re-bake throttle while streaming (~10 Hz)
    int         llmCannedIdx = 0;
    constexpr size_t kTermWrapCols = 40;   // on-glass wrap width (left data column)
    constexpr size_t kTermMaxBody  = 14;   // visible body rows before scroll-off

    // ---- RESCUED-NPC TALK (the captive girl). When the player presses E within
    // talk range of a LIVE captive, an exchange opens: she goes terrified ->
    // relieved -> grateful -> flirty over a short script, advancing on each E.
    // Completing the last line RESCUES her (RescueSystem::tryRescue -> she becomes a
    // following Companion) and surfaces a warm one-liner bark. The state machine is
    // the headless-tested NpcDialog (--test-npctalk); the host just feeds it the
    // in-range fact + the E edge and reads it back for the prompt/box. The dialog
    // takes PRIORITY in the E dispatch over the bare onRescue so the player always
    // gets the exchange (never an instant silent rescue). ----
    x3::game::NpcDialog npcDialog;
    float     npcBarkTimer = 0.0f;   // >0 while her companion one-liner is shown
    std::string npcBarkText;
    bool      chatNumPrev[4] = {};   // chat-tree choice keys 1-4 edge state
    // CHAT-TREE talk target: the F5 captive 'Lena' (spire_mid) — the first NPC whose
    // dialog runs the data-driven x3.chattree runner instead of the shared 5-line
    // script. Captive in reach -> her first_meeting tree; Companion -> banter pool.
    auto chatTalkTarget = [&](const x3::phys::Vec3& at, float reach,
                              std::string& whoOut, x3::phys::Vec3& posOut,
                              bool& captiveOut) -> bool {
        const x3::game::RescueVictim* v = midFloors.victim();
        if (!v || v->expired() || !chatTrees.hasNpc(v->name())) return false;
        const x3::phys::Vec3 vp = v->pos();
        const float dx = at.x - vp.x, dz = at.z - vp.z;
        if (dx * dx + dz * dz > reach * reach) return false;
        whoOut = v->name(); posOut = vp; captiveOut = v->captive();
        return true;
    };
    // Find the nearest LIVE captive within `reach` of `at` (XZ). Returns true + its
    // name/world-pos. Shared by the E dispatch and the prompt/box draw so both see
    // exactly the same target. (Companions/expired victims are skipped.)
    auto nearestLiveCaptive = [&](const x3::phys::Vec3& at, float reach,
                                  std::string& whoOut, x3::phys::Vec3& posOut) -> bool {
        // In --world canonlevel the captives live in canonPlay's RescueSystem; otherwise
        // in the legacy Level1Game. Scan whichever is active.
        const x3::game::RescueSystem& rs =
            (canonWorld && canonPlay.built()) ? canonPlay.rescue() : game.rescue();
        float best = reach * reach; bool found = false;
        for (uint32_t i = 0; i < rs.victimCount(); ++i) {
            const x3::game::RescueVictim& v = rs.victim(i);
            if (!v.captive()) continue;
            const x3::phys::Vec3 vp = v.pos();
            const float dx = at.x - vp.x, dz = at.z - vp.z;
            const float d2 = dx * dx + dz * dz;
            if (d2 <= best) { best = d2; whoOut = v.name(); posOut = vp; found = true; }
        }
        return found;
    };

    // ---- GENERAL save/load (versioned checkpoint). The interactive host exposes a
    // programmatic save/load API two ways: quick-save/quick-load on F5/F9, AND a
    // SAVE/LOAD CHECKPOINT affordance in the pause menu (gameUi.wantSave/wantLoad).
    // Both funnel through the same x3::game::captureCheckpoint/applyCheckpoint bridge
    // + x3::save::saveCheckpoint/loadCheckpoint. The file lives next to the exe so a
    // dev box always has a writable spot. Level 1 / interactive path only (every
    // headless/screenshot path early-returned above). ----
    const std::string savePath = "G:/X3Native-wt-saveload/build/eflz_checkpoint.x3save";
    bool prevSaveKey = false, prevLoadKey = false;   // F5 quick-save / F9 quick-load edges
    // Perform a save: snapshot the live game (current floor = the elevator's stop) and
    // write it. Lambdas so the F5 key + the pause-menu button share one code path.
    auto doSave = [&]() {
        if (terrainWorld) { x3::logWarn("[save] save/load is Level-1 only (skipped in terrain world)"); return; }
        const uint32_t curFloor = (uint32_t)(elevator.built() ? elevator.targetStop() : 0);
        x3::save::SaveState st = x3::game::captureCheckpoint(player, arsenal, game,
                                                            midFloors, topFloors, curFloor);
        if (x3::save::saveCheckpoint(savePath, st))
            x3::logInfo("[save] quick-saved checkpoint -> " + savePath);
        // Story flags + per-NPC rel ride ALONGSIDE the binary checkpoint (their own
        // additive text file — the checkpoint format/version stays untouched).
        if (chatTrees.flags().saveFile(savePath + ".flags.txt"))
            x3::logInfo("[save] story flags -> " + savePath + ".flags.txt");
    };
    // Perform a load: read + validate, then apply to the live game (and re-position
    // the elevator to the recorded floor). Fails gracefully (logged) on a bad file.
    auto doLoad = [&]() {
        if (terrainWorld) { x3::logWarn("[save] save/load is Level-1 only (skipped in terrain world)"); return; }
        x3::save::SaveState st;
        if (!x3::save::loadCheckpoint(savePath, st)) {
            x3::logWarn("[save] no valid checkpoint to load (ignored)");
            return;
        }
        uint32_t loadedFloor = 0;
        x3::game::applyCheckpoint(st, player, *physics, arsenal, game,
                                  midFloors, topFloors, loadedFloor);
        // Move the elevator to the recorded floor (clamped to its stop range) so the
        // world matches the restored "current floor".
        if (elevator.built() && (int)loadedFloor < elevator.stopCount())
            elevator.callTo((int)loadedFloor);
        // Story flags + rel (additive file next to the checkpoint; absence is fine —
        // an old save simply restores with empty narrative state).
        if (chatTrees.flags().loadFile(savePath + ".flags.txt"))
            x3::logInfo("[save] story flags restored from " + savePath + ".flags.txt");
        // Mission resume (g_missiondoc): the runner's position marker rides the
        // flags file — land back on the recorded stage (onEnter fx NOT re-fired).
        if (missionDocActive) missionRunner.resume(missionDoc);
    };

    // ---- M9 audio event edge-tracking + footstep cadence -------------------
    bool  prevArmed   = false;          // pickup chime on the arm rising edge
    float stepTimer   = 0.0f;           // accumulates while moving on the ground
    float prevCamX = 0.0f, prevCamZ = 0.0f; // for horizontal-speed footsteps
    bool  prevCamValid = false;

    // ---- Audio settings (persisted): seed the live music/SFX state from the cfg
    // (defaults: music on, music vol 0.25 to match the launch bed, SFX 1.0), apply
    // it to the audio system, THEN start the bed so it honors the saved volume/on. ----
    bool  s_musicOn  = true;
    float s_musicVol = 0.25f;
    float s_sfxVol   = 1.0f;
    readAudioSettings(s_musicOn, s_musicVol, s_sfxVol);
    audio->setMasterSfxVolume(s_sfxVol);
    audio->setMusicVolume(s_musicVol);
    audio->setMusicEnabled(s_musicOn);
    // M9: start the low-volume looping ambient/music bed at launch. playMusic remembers
    // the track + current music volume; when musicOn is false the bed stays silent.
    audio->playMusic(kMusicPath, /*loop*/true, s_musicVol);

    // ---- Optional debug noclip/fly camera (toggle with F). Not required by S3,
    // handy for inspecting the level. Off by default — gameplay is the walker.
    bool noclip = false;
    bool flashlight = true;   // player-following light (L toggles) — default ON for the dark halls
    bool prevL = false;
    float flyX = L1.spawn.x, flyY = 1.7f, flyZ = L1.spawn.z, flyYaw = 0.0f, flyPitch = 0.0f;

    // ---- Phase 2a: enemy-attack FX. Enemies invoke this to draw a tracer/telegraph
    // beam (drone hitscan / melee tell) via the combat FX pool. Reuses the same
    // world-space tracer the gun uses, tinted by the FX system. ----
    x3::game::AttackFxFn enemyAttackFx =
        [&combatFx](const x3::phys::Vec3& from, const x3::phys::Vec3& to) {
            combatFx.addTracer(from, to);
        };

    // ---- GENERAL game-UI: main menu / pause / settings + production HUD --------
    // The interactive windowed game launches into a MAIN MENU (title + START /
    // QUIT). START enters the game; Esc toggles a PAUSE menu that freezes the
    // sim/fixed-step; SETTINGS toggles render params (bloom/SSAO/SSGI/shadows/
    // vsync) wired to cvars + the device. While Playing, the production HUD draws
    // (HP / weapon+ammo / objective / crosshair / minimap stub). This whole layer
    // exists ONLY in this interactive path: every headless --test-*/--smoketest/
    // --screenshot path early-returns above, so they are unaffected.
    x3::ui::UiController gameUi;
    {
        x3::ui::SettingsModel sm{};
        // Seed from current engine defaults: SSAO + SSGI are ON by default in the
        // device; bloom is always-on in the HDR pipeline; shadows on; vsync from
        // the device desc; resolution = the actual window size.
        sm.bloom = true; sm.ssao = true; sm.ssgi = true; sm.shadows = true;
        sm.vsync = desc.vsync; sm.width = W; sm.height = H;
        sm.rtao = (console->getInt("r_rtao") != 0);   // RT AO: reflect the cvar (default OFF)
        // Audio: seed from the persisted values applied to the audio system above.
        sm.musicOn = s_musicOn; sm.musicVol = s_musicVol; sm.sfxVol = s_sfxVol;
        gameUi.init(*device, console.get(), sm);
        gameUi.setTitle(terrainWorld ? "X3 ENGINE" : "ESCAPE FROM LAB ZERO",
                        terrainWorld ? "open-world demo" : "Level 1 - Awakening");
    }
    // Cursor is shown in any menu OR while the console is open; hidden only while
    // actively playing with the console closed. Tracked so we only call GLFW on a
    // transition. Start in the menu => cursor visible.
    bool cursorShown = true;
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    bool prevUiEsc = false;   // rising-edge for routing Esc into the UI controller
    // Rising-edge trackers for the UI controller's input snapshot (menu mouse +
    // keyboard nav). Kept across frames so click/nav register on the press edge.
    bool prevUiMouse = false;
    bool prevNavUp = false, prevNavDown = false, prevNavAct = false,
         prevNavLeft = false, prevNavRight = false;

    // ---- LEVEL ARCHITECT editor host (--editor only) --------------------------
    // The live-mode editor orchestrator: panels + a fly-cam (Edit mode) + the F8
    // Edit/Play toggle + the blockout brush subsystem (grid material cached on the
    // GPU here). Constructed always-cheap (no allocation) but only init'd + ticked
    // when editorMode, so the shipping game path is byte-for-byte unchanged.
    x3::editor::EditorHost editorHost;
    if (editorMode && device->editorUIActive())
        editorHost.init(*device, scene, *physics, window);

    // ---- EFLZ LOADING SCREEN hand-off (Task #49) — INTERACTIVE path -----------
    // The world is fully built. Mark the bar complete, hold the finished screen for
    // a couple of frames, then fade it OUT with REAL wall-clock dt so the first
    // gameplay (menu) frame doesn't pop in. Driven entirely on the real window
    // frame path (beginFrame/endFrame). Only reached when a window exists.
    // BOOT-TIME (docs/BOOT_TIME.md): the hand-off no longer runs a DEDICATED
    // hold+fade frame loop (which used to add ~0.2-0.6 s of pure padding while the
    // first real frame's one-time work — BLAS/TLAS builds, first scene draw —
    // STILL waited on the other side). Instead the main loop starts immediately
    // and draws the completed loading overlay ON TOP, fading it out over the
    // first live frames — so the heavy first frame happens UNDER the bar and the
    // menu is interactive the moment it's visible. Fast boots (the bar barely
    // showed) fade ~3x quicker.
    bool loadingOverlayLive = true;
    {
        x3::boot::mark("systems wired (ui/console/spawn)");
        loading.step(x3::game::LoadStep::Done, "READY");
        loading.beginFadeOut(x3::boot::sinceStartMs() < 2500.0);
    }

    // ---- INTERACTIVE WORLD MAP (M key) ----------------------------------------
    // POIs + discovery + waypoint + fast travel + the full-screen map screen
    // (app/world_map.*). Tiles bake from the canonical LevelDoc floors on first
    // open. Discovery flags ride chatTrees.flags() (the one StoryFlags world),
    // so found POIs persist with the story save.
    x3::game::WorldMapSystem worldMap;
    worldMap.init(x3::game::worldMapPoisJsonPath(), x3::game::canonProjectJsonPath());
    x3::ui::UiContext mapUi;            // the map screen's own IMGUI-lite context
    bool  worldMapOpen = false;
    bool  prevMapKey = false, prevMapEnter = false;
    float travelFadeT = 0.0f;           // fast-travel fade-to-black cover (s left)

    // ---- Main loop ----
    bool bootReported = false;   // [boot] one-shot: report on the FIRST presented frame
    int  bootTestExit = 0;       // --test-boottime verdict (0 pass / 1 over budget)
    int lastW = static_cast<int>(W), lastH = static_cast<int>(H);
    float oceanTime = 0.0f;   // --world ocean wave-animation clock (seconds)
    double frameCapPrev = glfwGetTime();   // r_maxfps limiter cursor
    while (!glfwWindowShouldClose(window)) {
        // ---- Frame cap (r_maxfps): sleep out the remainder of the frame budget so
        // vsync-off doesn't churn the GPU on invisible frames. No-op when vsync is on
        // (FIFO already blocks) since we'll already be slower than the cap, and when
        // r_maxfps<=0. Sleep most of the wait, spin the last ~1 ms for accuracy. ----
        {
            const float maxfps = (float)std::atof(console->getString("r_maxfps").c_str());
            if (maxfps > 0.0f) {
                const double target = frameCapPrev + 1.0 / (double)maxfps;
                double nowc = glfwGetTime();
                if (nowc < target) {
                    const double remain = target - nowc;
                    if (remain > 0.002)
                        std::this_thread::sleep_for(std::chrono::duration<double>(remain - 0.001));
                    while (glfwGetTime() < target) { /* short spin to the deadline */ }
                }
                frameCapPrev = glfwGetTime();
            } else {
                frameCapPrev = glfwGetTime();
            }
        }
        // Push the live r_rtao* cvars onto the device (hardware RT ambient occlusion).
        // No-op on a non-RT GPU; default OFF so the visual build is unchanged.
        applyRtaoCVars(*console, *device);
        glfwPollEvents();

        // ---- intro_play (console): roll the cold-open film NOW, at a frame
        // boundary. Blocking; the live world is untouched underneath (the film
        // draws only its own scene) and the loop resumes cleanly after. ----
        if (introPlayRequest) {
            introPlayRequest = false;
            const std::string csPath = !cutsceneFile.empty()
                ? cutsceneFile
                : x3::game::assetRoot() + "/cutscenes/cold_open.cutscene.json";
            x3::cut::Cutscene replayCs;
            std::vector<std::string> replayErrs;
            if (x3::cut::loadCutsceneFile(csPath, replayCs, replayErrs)) {
                if (!runCutsceneWindowed(*device, window, audio.get(), replayCs))
                    break;   // window closed mid-film -> normal quit path
                frameCapPrev = glfwGetTime();   // don't count the film against the frame cap
            } else {
                for (const auto& e : replayErrs) x3::logError("[cutscene] intro_play: " + e);
            }
        }

        // ---- S7: console gating. While the console is open, gameplay input is
        // suppressed and the cursor is shown so the user can read/type. The cursor
        // is ALSO shown by any UI menu (main/pause/settings); the UiController is
        // the master for menu cursor state. Recompute the desired cursor each
        // frame and only touch GLFW on a transition.
        const bool consoleOpen = hud.consoleOpen();
        consoleWasOpen = consoleOpen;   // (retained for parity; cursor logic below)
        const bool wantCursor = consoleOpen || gameUi.showCursor() || worldMapOpen;
        if (wantCursor != cursorShown) {
            glfwSetInputMode(window, GLFW_CURSOR,
                             wantCursor ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_DISABLED);
            cursorShown = wantCursor;
        }
        // Whether a UI menu (main/pause/settings) is currently up. While a menu is
        // up, gameplay input + the sim are frozen and only the menu reads input.
        const bool uiMenuActive = !gameUi.playing();
        // The world map pauses the sim exactly like the menu screens do.
        const bool simFrozen     = gameUi.shouldFreezeSim() || worldMapOpen;

        // Esc (edge-detected): route to the UI controller (toggle pause / back out
        // of settings / resume) UNLESS the console is open or a door-code keypad is
        // active (those consume Esc first). The legacy "Esc quits" is gone — quit is
        // now an explicit menu choice (or the `quit` console command).
        bool escNow = !consoleOpen && glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS;
        bool uiEscEdge = false;
        bool mapEscEdge = false;        // routed into the world-map screen (confirm prompt)
        if (escNow && !kpEscPrev) {
            if (codeMode) { codeMode = false; keypad.clear(); }
            else if (termMode) { termMode = false; game.secret().terminal().setActive(false);
                                 if (llmBusy && llm) llm->cancel(llmChat); }   // stop streaming
            else if (worldMapOpen) {
                if (worldMap.confirmOpen()) mapEscEdge = true;   // back out of the prompt
                else { worldMapOpen = false; worldMap.close(); } // close the map
            }
            else          { uiEscEdge = true; }   // hand the Esc edge to the UI below
        }
        kpEscPrev = escNow;
        (void)prevUiEsc;
        // The `quit` console command (and the menu QUIT) request shutdown.
        if (quitRequested || gameUi.wantQuit()) glfwSetWindowShouldClose(window, 1);

        double nowT = glfwGetTime();
        float dt = static_cast<float>(nowT - prevTime); prevTime = nowT;
        if (dt > 0.1f) dt = 0.1f; // clamp huge hitches (e.g. after a stall)

        // Mouse delta this frame. Frozen (zeroed) while the console is open OR a UI
        // menu is up, so the view does not swing under a visible cursor.
        double mx, my; glfwGetCursorPos(window, &mx, &my);
        float ddx = static_cast<float>(mx - lastMX), ddy = static_cast<float>(my - lastMY);
        lastMX = mx; lastMY = my;
        if (consoleOpen || uiMenuActive || termMode || codeMode || worldMapOpen) { ddx = 0.0f; ddy = 0.0f; }

        // Gameplay key reads are gated off while the console, a UI menu, the cell
        // terminal, OR a door-code keypad is active — so ALL gameplay input is
        // redirected to whatever is capturing (it reads keys via rawKey below) and
        // nothing drives movement/use/jump/fire/noclip/weapon-switch while typing.
        auto keyDown = [&](int k) {
            return !consoleOpen && !uiMenuActive && !termMode && !codeMode && !worldMapOpen &&
                   glfwGetKey(window, k) == GLFW_PRESS;
        };
        // RAW key read (bypasses the capture gates) — used ONLY by the terminal/keypad
        // input capture so they still receive keystrokes while they are active.
        auto rawKey = [&](int k) { return glfwGetKey(window, k) == GLFW_PRESS; };

        // F toggles noclip via the SAME Player flag the `idclip` console command drives
        // (single source of truth — previously F drove a local var and idclip drove
        // player.noclip(), so the console command did nothing for movement).
        bool fNow = keyDown(GLFW_KEY_F);
        if (fNow && !prevF) player.setNoclip(!player.noclip());
        prevF = fNow;
        // Mirror the Player's noclip flag (set by F OR idclip) into the local `noclip`
        // the movement uses; seed the fly camera from the current view on the rising
        // edge so the transition is seamless either way.
        if (player.noclip() != noclip) {
            noclip = player.noclip();
            if (noclip) {
                player.camera(flyX, flyY, flyZ, flyYaw, flyPitch);   // ON: seed fly cam from the view
            } else {
                // OFF: drop the player WHERE THE FLY CAM ENDED (feet 1.6m below the eye) so
                // you stay put and can explore other floors — don't snap back to the
                // pre-noclip spot. Keep the look direction continuous.
                player.setFeetPosition(*physics, x3::phys::Vec3{ flyX, flyY - 1.6f, flyZ });
                player.setLook(flyYaw, flyPitch);
            }
            x3::logInfo(noclip ? "noclip ON (fly: WASD + Space up / Ctrl down, look to steer)"
                               : "noclip OFF (landed at fly position)");
        }

        // F3: toggle the perf stats overlay (drives the r_stats cvar) on the rising
        // edge. Polled even with the console open so it always works.
        bool f3Now = (glfwGetKey(window, GLFW_KEY_F3) == GLFW_PRESS);
        if (f3Now && !prevF3) {
            console->set("r_stats", console->getInt("r_stats") ? "0" : "1");
            x3::logInfo(std::string("r_stats = ") + console->getString("r_stats"));
        }
        prevF3 = f3Now;

        // F1 = FIRST-PERSON, F2 = THIRD-PERSON (rising edge; explicit per-mode keys,
        // not a single toggle). FP is the default (eye-cam + weapon viewmodel); 3P
        // shows the animated Jake avatar + a follow camera behind/above the player.
        // Polled even with the console open (changes nothing the console types into).
        // The avatar only exists when Jake loaded; otherwise the flag flips but FP
        // keeps drawing. (Moved off F5 — F5 is the quicksave key, see doSave below.)
        bool f1Now = (glfwGetKey(window, GLFW_KEY_F1) == GLFW_PRESS);
        bool f2Now = (glfwGetKey(window, GLFW_KEY_F2) == GLFW_PRESS);
        if (f1Now && !prevF1 && !terrainWorld && thirdPerson.thirdPerson()) {
            thirdPerson.setThirdPerson(false);
            x3::logInfo("view: FIRST-PERSON (eye-cam + weapon viewmodel)");
        }
        if (f2Now && !prevF2 && !terrainWorld && !thirdPerson.thirdPerson()) {
            thirdPerson.setThirdPerson(true);
            x3::logInfo("view: THIRD-PERSON (Jake avatar + follow cam; F1 to return)");
        }
        prevF1 = f1Now;
        prevF2 = f2Now;

        // ---- M: the INTERACTIVE WORLD MAP (full-screen; pauses the sim). Opens
        // centered on the player with the player's Spire floor auto-selected;
        // M again (or Esc) closes. Gated off whenever another surface captures. ----
        {
            const bool mNow = !consoleOpen && !termMode && !codeMode &&
                              !chatTrees.active() && gameUi.playing() &&
                              rawKey(GLFW_KEY_M);
            if (mNow && !prevMapKey) {
                if (worldMapOpen) { worldMapOpen = false; worldMap.close(); }
                else if (!terrainWorld) {
                    float mpx, mpy, mpz, mpyaw, mppitch;
                    player.camera(mpx, mpy, mpz, mpyaw, mppitch);
                    if (noclip) { mpx = flyX; mpy = flyY; mpz = flyZ; }
                    int fbw = 0, fbh = 0; glfwGetFramebufferSize(window, &fbw, &fbh);
                    worldMap.open(mpx, mpy - 1.6f, mpz, (float)fbw, (float)fbh);
                    worldMapOpen = true;
                }
            }
            prevMapKey = mNow;
        }
        // The map consumes the mouse wheel for ZOOM while it is open (weapon
        // cycling below sees a zeroed delta).
        float mapWheel = 0.0f;
        if (worldMapOpen) { mapWheel = (float)g_weaponScroll; g_weaponScroll = 0.0; }

        // ---- CHAT-TREE choice input: number keys 1-4 answer the filtered choices
        // while a chat conversation is up (E advances no-choice lines in the use
        // dispatch below). Edge-detected; consumes the keys (weapon switch is
        // suppressed while talking — same capture idea as the terminal). ----
        if (chatTrees.active() && !terrainWorld) {
            const uint32_t nch = (uint32_t)chatTrees.choices().size();
            for (int ci = 0; ci < 4; ++ci) {
                const bool dn = keyDown(GLFW_KEY_1 + ci) || keyDown(GLFW_KEY_KP_1 + ci);
                if (dn && !chatNumPrev[ci] && (uint32_t)ci < nch) {
                    const bool still = chatTrees.choose((uint32_t)ci);
                    if (still) {
                        x3::logInfo("chat: [" + chatTrees.currentSpeaker() + "] " +
                                    chatTrees.currentLine());
                    } else if (chatTrees.followFired()) {
                        npcBarkText  = x3::game::companionBark("Lena");
                        npcBarkTimer = 4.0f;
                        x3::logInfo("chat: " + std::string("Lena") +
                                    " joined as a companion (chat tree)");
                    }
                }
                chatNumPrev[ci] = dn;
            }
        } else {
            chatNumPrev[0] = chatNumPrev[1] = chatNumPrev[2] = chatNumPrev[3] = false;
        }

        // ---- WEAPONS: number keys 1..N switch the selected weapon; R reloads.
        // Suppressed while a keypad OR the cell terminal is active (those number/letter
        // keys are being typed as a code, not used to switch weapons), and while a
        // chat-tree conversation is capturing 1-4 as dialog choices.
        if (!codeMode && !termMode && !terrainWorld && !chatTrees.active()) {
            const int n = arsenal.count() < 9 ? arsenal.count() : 9;
            for (int wi = 0; wi < n; ++wi) {
                bool down = keyDown(GLFW_KEY_1 + wi);
                if (down && !prevWeaponKey[wi]) arsenal.select(wi);
                prevWeaponKey[wi] = down;
            }
            bool rNow = keyDown(GLFW_KEY_R);
            if (rNow && !prevReload && game.armed()) arsenal.reload();
            prevReload = rNow;
            // MOUSE WHEEL cycles weapons (up = next, down = previous), wrapping.
            if (!termMode && !consoleOpen && g_weaponScroll != 0.0 && arsenal.count() > 0) {
                const int cnt = arsenal.count();
                const int dir = (g_weaponScroll > 0.0) ? 1 : -1;
                arsenal.select(((arsenal.selected() + dir) % cnt + cnt) % cnt);
            }
            g_weaponScroll = 0.0;   // consume the wheel delta each frame
        }
        // Advance the arsenal timers (fire cooldowns + reload completion) every frame.
        arsenal.tick(dt);

        bool spaceNow = keyDown(GLFW_KEY_SPACE);

        // ---- E: "use" on the rising edge. Raycast from the eye along the facing
        // direction; if it hits a button linked to an UNLOCKED door, it opens.
        // (Door C refuses while locked — until the player is armed, §6.4.) ----
        bool eNow = keyDown(GLFW_KEY_E);
        if (eNow && !prevE && !terrainWorld) {
            float ex, ey, ez, yaw, pitch;
            player.camera(ex, ey, ez, yaw, pitch);   // in noclip the camera is the fly cam
            if (noclip) { ex = flyX; ey = flyY; ez = flyZ; yaw = flyYaw; pitch = flyPitch; }
            x3::phys::Vec3 eye{ ex, ey, ez };
            x3::phys::Vec3 dir{ std::cos(pitch) * std::cos(yaw),
                                std::sin(pitch),
                                std::cos(pitch) * std::sin(yaw) };
            // CHAT-TREE TALK (x3.chattree/1) takes priority over EVERYTHING: Lena,
            // the F5 scavenger, runs her DATA dialog (lena.json) instead of the
            // shared 5-line script. E starts first_meeting on the captive /
            // advances no-choice lines (1-4 answer choices, handled per-frame
            // above); re-talking the companion pulls a banter-pool bark. The
            // {"follow"} effect routes through the SAME midFloors.onRescue sink
            // the bare E-rescue used.
            std::string chatWho; x3::phys::Vec3 chatPos{}; bool chatCaptive = false;
            const bool chatInRange =
                chatTalkTarget(eye, x3::game::kTalkReach, chatWho, chatPos, chatCaptive);
            const bool chatHandled = chatTrees.active() || chatInRange;
            if (chatHandled) {
                if (chatTrees.active()) {
                    if (chatTrees.choices().empty()) {
                        const bool still = chatTrees.advance();
                        if (still) {
                            x3::logInfo("chat: [" + chatTrees.currentSpeaker() + "] " +
                                        chatTrees.currentLine());
                        } else if (chatTrees.followFired()) {
                            npcBarkText  = x3::game::companionBark(chatWho.empty() ? "Lena" : chatWho);
                            npcBarkTimer = 4.0f;
                            x3::logInfo("chat: " + (chatWho.empty() ? std::string("Lena") : chatWho) +
                                        " rescued via her chat tree — now a companion");
                        }
                    }   // choices up: E waits for a 1-4 answer
                } else if (chatCaptive) {
                    // The follow sink — evaluated when her tree's {"follow"} fx fires
                    // (a later E), so it re-resolves the victim position at call time.
                    chatTrees.ctx().follow = [&midFloors]() {
                        const x3::game::RescueVictim* v = midFloors.victim();
                        return v ? midFloors.onRescue(v->pos()) : false;
                    };
                    if (chatTrees.start(chatWho, "first_meeting"))
                        x3::logInfo("chat: [" + chatTrees.currentSpeaker() + "] " +
                                    chatTrees.currentLine());
                } else {
                    // Companion re-talk: a banter-pool bark (weighted, rotated, gated).
                    const float roll = (float)(std::rand() % 1000) / 1000.0f;
                    const std::string b = chatTrees.pickBanter(chatWho, roll);
                    if (!b.empty()) {
                        npcBarkText  = b;
                        npcBarkTimer = 5.0f;
                        x3::logInfo("chat banter: [" + chatWho + "] " + b);
                    }
                }
            }
            // RESCUED-NPC TALK takes priority over the bare door/rescue handlers so
            // the captive girl always gets her exchange. If a live captive is in talk
            // range, this E starts/advances the dialog; completing it performs the
            // actual rescue (so she becomes a following companion) + queues her bark.
            std::string talkWho; x3::phys::Vec3 talkPos{};
            const bool talkInRange = !chatHandled &&
                nearestLiveCaptive(eye, x3::game::kTalkReach, talkWho, talkPos);
            const bool talkHandled = npcDialog.active() || talkInRange;
            if (chatHandled) {
                // consumed by the chat-tree branch above (keeps the else-chain shut)
            } else if (talkHandled) {
                const std::string barkName = talkWho.empty() ? npcDialog.partner() : talkWho;
                const bool rescued = npcDialog.interact(
                    talkInRange, talkWho, talkPos,
                    // canonlevel routes the rescue to canonPlay; legacy to game.
                    [&]() -> bool {
                        return (canonWorld && canonPlay.built())
                                   ? canonPlay.tryRescue(eye)
                                   : game.onRescue(eye);
                    });
                if (rescued) {
                    // Per-girl companion line in canonlevel (her OWN amorous voice) — falls
                    // back to the shared bark elsewhere / if she has no canon dialog row.
                    std::string bark;
                    if (canonWorld && canonPlay.built())
                        bark = canonPlay.dialog().line(barkName,
                                   x3::game::GirlDialogState::CompanionAmorous);
                    if (bark.empty()) bark = x3::game::companionBark(barkName);
                    npcBarkText  = bark;
                    npcBarkTimer = 4.0f;
                    x3::logInfo("talk: " + barkName + " rescued — now a companion (\"" + npcBarkText + "\")");
                } else if (npcDialog.active()) {
                    const auto& ln = npcDialog.currentLine();
                    x3::logInfo("talk: [" + ln.speaker + "] " + ln.text);
                }
            } else if (canonWorld && canonFloor.valid() &&
                       [&]() -> bool {
                           // Canonical Floor 1 doors: aim + E. Unlocked -> toggle. Locked ->
                           // keycard / keypad gating (Security = card OR code; Medical = code;
                           // Armory = card AND code). Returns true once handled (consumes the E).
                           x3::game::Door* d = x3::game::pickAimedDoor(eye, dir, 3.0f, scene, canonDoors, *physics);
                           if (!d) return false;                       // not aiming at a door -> fall through
                           if (!d->locked) { canonDoors.toggle(*d); x3::logInfo("use: canon door toggled"); return true; }
                           auto cardName = [](int id){ return id == x3::game::kKeycardSecurity ? "Security" : "access"; };
                           const bool needCard = d->keycard != 0;
                           const bool hasCard  = needCard && (keycardMask & (1u << (uint32_t)d->keycard));
                           const bool needCode = d->code != 0;
                           if (d->requireBoth) {                       // need card AND code (Armory)
                               if (needCard && !hasCard) {
                                   npcBarkText = std::string("LOCKED — need the ") + cardName(d->keycard) + " keycard";
                                   npcBarkTimer = 3.0f; return true;
                               }
                               codeMode = true; keypad.clear();        // card ok -> enter the code
                               npcBarkText = std::string("Keycard OK — enter code ") + std::to_string(d->code);
                               npcBarkTimer = 4.0f; return true;
                           }
                           if (needCard && hasCard) {                  // either-credential: card opens it outright
                               canonDoors.unlock(*d); canonDoors.toggle(*d);
                               npcBarkText = std::string(cardName(d->keycard)) + " keycard accepted";
                               npcBarkTimer = 2.5f; x3::logInfo("use: canon door unlocked (keycard)"); return true;
                           }
                           if (needCode) {                             // try the code (Security w/o card, or Medical)
                               codeMode = true; keypad.clear();
                               npcBarkText = needCard
                                   ? (std::string("LOCKED — ") + cardName(d->keycard) + " keycard, or enter code " + std::to_string(d->code))
                                   : (std::string("LOCKED — enter code ") + std::to_string(d->code));
                               npcBarkTimer = 4.0f; return true;
                           }
                           npcBarkText = std::string("LOCKED — need the ") + cardName(d->keycard) + " keycard";
                           npcBarkTimer = 3.0f; return true;
                       }()) {
                // canon door interaction handled inside the lambda (toggle / unlock / keypad / message)
            } else if (game.onUse(eye, dir, scene, *physics)) {  // plays door SFX internally
                x3::logInfo("use: button pressed — door opening");
            } else if (midFloors.onRescue(eye)) {  // F5 synth-bay captive rescue
                x3::logInfo("use: F5 captive rescued — now a companion");
            } else if (topFloors.onRescue(eye)) {  // F7 rooftop captive (Sarah) rescue
                sarahSaved = true;   // latch the descent gate input (the host's only Sarah-saved signal)
                x3::logInfo("use: F7 captive 'Sarah' rescued — now a companion (Return-Mission gate armed)");
            } else if (nexus.onInteract(eye, scene, *physics)) {  // Floor 4.5: SPARE a Chorus voice
                x3::logInfo("use: Chorus voice SPARED (save up to 4) — saved=" +
                            std::to_string(nexus.savedCount()));
            } else if (subLevels.onRescue(eye)) {  // SL3 Dr. Chen rescue (the Return-Mission payoff)
                x3::logInfo("use: Dr. Chen freed — the Return Mission is complete");
            } else if (!termMode && game.secret().terminal().built() &&
                       [&]{ const x3::phys::Vec3 a = game.secret().terminal().anchor();
                            const float ddx = eye.x - a.x, ddz = eye.z - a.z;
                            return ddx*ddx + ddz*ddz < 9.0f; }()) {
                // Near the cell HoloTerminal: open terminal-entry mode (type the override
                // code, Enter submits to the sink -> the trapdoor opens on 1127).
                termMode = true; game.secret().terminal().setActive(true);
                // Prime the typed-char edge state from the keys CURRENTLY held so
                // the E press that opened the terminal doesn't leak an 'E' into
                // the input line this same frame (rising-edge false positive).
                for (int dgt = 0; dgt < 10; ++dgt)
                    tmDigitPrev[dgt] = rawKey(GLFW_KEY_0 + dgt) || rawKey(GLFW_KEY_KP_0 + dgt);
                for (int li = 0; li < 26; ++li) tmCharPrev[li] = rawKey(GLFW_KEY_A + li);
                tmSpacePrev = rawKey(GLFW_KEY_SPACE);
                tmEnterPrev = rawKey(GLFW_KEY_ENTER) || rawKey(GLFW_KEY_KP_ENTER);
                tmBackPrev  = rawKey(GLFW_KEY_BACKSPACE);
                x3::logInfo("use: cell terminal — type the override code, Enter to submit, Esc to cancel");
            } else if (!codeMode && (game.nearLockedCodedDoor(eye) ||
                                     midFloors.nearLockedCodedDoor(eye) ||
                                     topFloors.nearLockedCodedDoor(eye) ||
                                     subLevels.nearLockedCodedDoor(eye))) {
                // No button hit, but a locked keypad door is in reach: open the
                // code-entry keypad (digits 0-9, Enter to submit, Esc to cancel).
                codeMode = true; keypad.clear();
                x3::logInfo("use: locked keypad door — type the code, Enter to submit, Esc to cancel");
            } else if (elevator.built()) {
                // Within ~4 m of the elevator shaft (XZ): call the cab to its next
                // stop (cycles ground <-> top). Carries the rider on the way.
                const x3::phys::Vec3 cc = elevator.cabCenter();
                const float ecx = eye.x - cc.x, ecz = eye.z - cc.z;
                if (ecx * ecx + ecz * ecz < 16.0f) {
                    elevator.callNext();
                    x3::logInfo("use: elevator called");
                }
            }
        }
        prevE = eNow;

        // ---- RESCUED-NPC TALK upkeep (every frame, edge-independent): if an exchange
        // is running, keep its box anchored to the captive, and CANCEL it the moment
        // the player wanders out of talk range (so the box never strands on screen).
        // Also age out her companion one-liner bark.
        if (!terrainWorld) {
            if (npcDialog.active()) {
                float pex, pey, pez, pyaw, ppitch;
                player.camera(pex, pey, pez, pyaw, ppitch);
                if (noclip) { pex = flyX; pey = flyY; pez = flyZ; }
                const x3::phys::Vec3 peye{ pex, pey, pez };
                std::string w; x3::phys::Vec3 cp{};
                if (nearestLiveCaptive(peye, x3::game::kTalkReach, w, cp)) npcDialog.setAnchor(cp);
                else                                                       npcDialog.cancel();
            }
            // Chat-tree conversation: cancel the moment the player wanders out of
            // talk range (a small grace over the start reach so a head-bob doesn't
            // drop the box mid-line). Matches NpcDialog's never-strand rule.
            if (chatTrees.active()) {
                float pex, pey, pez, pyaw, ppitch;
                player.camera(pex, pey, pez, pyaw, ppitch);
                if (noclip) { pex = flyX; pey = flyY; pez = flyZ; }
                const x3::phys::Vec3 peye{ pex, pey, pez };
                std::string w; x3::phys::Vec3 cp{}; bool cap = false;
                if (!chatTalkTarget(peye, x3::game::kTalkReach + 0.5f, w, cp, cap))
                    chatTrees.cancel();
            }
            if (npcBarkTimer > 0.0f) npcBarkTimer -= dt;
        }

        // ---- Door-code keypad: capture digit/backspace/enter edges while active.
        // Esc-cancel is handled in the Esc block above. Uses the shared KeypadEntry
        // state machine (also driven by --test-doorcode). ----
        if (codeMode && !terrainWorld) {
            for (int dgt = 0; dgt < 10; ++dgt) {
                bool dn = rawKey(GLFW_KEY_0 + dgt) || rawKey(GLFW_KEY_KP_0 + dgt);
                if (dn && !kpDigitPrev[dgt]) keypad.pushDigit(dgt);
                kpDigitPrev[dgt] = dn;
            }
            bool backNow = rawKey(GLFW_KEY_BACKSPACE);
            if (backNow && !kpBackPrev) keypad.backspace();
            kpBackPrev = backNow;
            bool enterNow = rawKey(GLFW_KEY_ENTER) || rawKey(GLFW_KEY_KP_ENTER);
            if (enterNow && !kpEnterPrev) {
                float pex, pey, pez, pyaw, ppitch;
                player.camera(pex, pey, pez, pyaw, ppitch);
                if (noclip) { pex = flyX; pey = flyY; pez = flyZ; }
                // ---- Souped-up elevator DISCO code (1127). If the player is near
                // the elevator shaft, the entered code is also offered to the
                // elevator's keypad: 1127 toggles DISCO MODE + drives the cab down
                // to Club 1127 (Y=-200). Checked BEFORE the door codes so the
                // elevator owns 1127 while you're riding it (the Spire door keypads
                // use other codes); falls through to doors otherwise.
                bool elevDisco = false;
                if (elevator.built() && elevator.fsmEnabled()) {
                    const x3::phys::Vec3 cc = elevator.cabCenter();
                    const float dcx = pex - cc.x, dcz = pez - cc.z;
                    if (dcx * dcx + dcz * dcz < 16.0f) {
                        const uint32_t code = keypad.value();
                        elevator.keypadClear();
                        // Feed the 4 digits MSB-first into the elevator keypad.
                        elevator.keypadDigit((int)((code / 1000) % 10));
                        elevator.keypadDigit((int)((code / 100) % 10));
                        elevator.keypadDigit((int)((code / 10) % 10));
                        bool completed = elevator.keypadDigit((int)(code % 10));
                        if (completed) {
                            x3::logInfo("keypad: DISCO 1127 — descending to Club 1127");
                            elevDisco = true;
                        }
                    }
                }
                if (elevDisco) {
                    codeMode = false; keypad.clear();
                } else if (canonDoors.tryDoorCode(x3::phys::Vec3{ pex, pey, pez }, keypad.value()) ||
                    game.tryDoorCode(x3::phys::Vec3{ pex, pey, pez }, keypad.value()) ||
                    midFloors.tryDoorCode(x3::phys::Vec3{ pex, pey, pez }, keypad.value()) ||
                    topFloors.tryDoorCode(x3::phys::Vec3{ pex, pey, pez }, keypad.value()) ||
                    subLevels.tryDoorCode(x3::phys::Vec3{ pex, pey, pez }, keypad.value())) {
                    x3::logInfo("keypad: ACCEPTED — door opening");
                    // LIVING WORLD: a keypad override is a TERMINAL HACK stimulus —
                    // the security net notices doors being opened by code.
                    if (facilityAlertOn)
                        facilityAlert.reportTerminalHack(x3::phys::Vec3{ pex, pey, pez });
                    codeMode = false; keypad.clear();
                } else {
                    x3::logInfo("keypad: rejected");
                    // A WRONG code is even more suspicious (a tamper alarm).
                    if (facilityAlertOn)
                        facilityAlert.reportTerminalHack(x3::phys::Vec3{ pex, pey, pez });
                    keypad.clear();
                }
            }
            kpEnterPrev = enterNow;
        }

        // ---- Cell HoloTerminal entry: capture digit/backspace/Enter edges while the
        // terminal is active. Enter calls submit() -> the terminal's sink (which opens
        // the floor-hatch trapdoor on the correct code 1127). Esc-cancel handled below. --
        if (termMode && !terrainWorld) {
            x3::game::HoloTerminal& term = game.secret().terminal();
            for (int dgt = 0; dgt < 10; ++dgt) {
                bool dn = rawKey(GLFW_KEY_0 + dgt) || rawKey(GLFW_KEY_KP_0 + dgt);
                if (dn && !tmDigitPrev[dgt]) term.pushChar((char)('0' + dgt));
                tmDigitPrev[dgt] = dn;
            }
            // Letters + space too, so the cell terminal is a REAL typable field (not
            // digits-only). Uppercase to match the on-glass font. These use rawKey so
            // they register while keyDown (all gameplay input) is gated off in termMode.
            for (int li = 0; li < 26; ++li) {
                bool dn = rawKey(GLFW_KEY_A + li);
                if (dn && !tmCharPrev[li]) term.pushChar((char)('A' + li));
                tmCharPrev[li] = dn;
            }
            bool tspaceNow = rawKey(GLFW_KEY_SPACE);
            if (tspaceNow && !tmSpacePrev) term.pushChar(' ');
            tmSpacePrev = tspaceNow;
            bool tbackNow = rawKey(GLFW_KEY_BACKSPACE);
            if (tbackNow && !tmBackPrev) term.backspace();
            tmBackPrev = tbackNow;
            bool tEnterNow = rawKey(GLFW_KEY_ENTER) || rawKey(GLFW_KEY_KP_ENTER);
            if (tEnterNow && !tmEnterPrev) {
                // All-digit input = a keypad code attempt -> the EXISTING submit
                // chain (D14 fire-into-Lua + submitTerminalToScripts, plus the
                // mission-flag bridge). Anything else = freeform -> VIGIL (the LLM).
                const std::string typed = term.input();
                const bool looksLikeCode = typed.empty() ||
                    (typed.size() <= 8 && std::all_of(typed.begin(), typed.end(),
                         [](unsigned char ch) { return std::isdigit(ch) != 0; }));
                if (looksLikeCode) {
                    // D14: fire the entered code INTO Lua, then run the terminal's own
                    // submit sink — via submitTerminalToScripts() (shared with --test-hatch
                    // so the keypad->fire link is the SAME code the headless chain proves).
                    // Mission flag bridge: the entered code is condition substrate too
                    // ("code.<code>.entered") — mirrored BEFORE submit clears the line.
                    if (missionDocActive)
                        missionEvents.onEvent("terminal_code", {{"code", term.input()}});
                    bool ok = submitTerminalToScripts(scripts.get(), term);
                    if (ok) { termMode = false; term.setActive(false);
                              x3::logInfo("terminal: code ACCEPTED — trapdoor opening"); }
                    else      x3::logInfo("terminal: code rejected");
                } else if (!llmBusy) {
                    term.clearInput();
                    term.addLine("> " + typed);            // echo the question
                    bool routed = false;
                    if (llm) {
                        if (llmChat == x3::llm::kInvalidChat)
                            llmChat = llm->startChat(kVigilPersona);
                        if (llmChat != x3::llm::kInvalidChat && llm->submit(llmChat, typed)) {
                            llmBusy = true; llmLineAccum.clear(); llmBakeAcc = 0.0f;
                            term.addLine("");              // the reply streams into this row
                            routed = true;
                        }
                    }
                    if (!routed)
                        term.addLine(kVigilDegraded[(llmCannedIdx++) % kVigilDegradedN]);
                    term.trimBody(kTermMaxBody);
                    x3::logInfo("terminal: JAKE -> " + typed);
                }
                // llmBusy + freeform Enter: ignored (one question at a time --
                // the streaming row is the glass's last line and must stay so).
            }
            tmEnterPrev = tEnterNow;
        }

        // ---- VIGIL reply streaming: drain LLM tokens onto the terminal glass.
        // Throttled to ~10 Hz (each apply re-bakes the 1024^2 hologram texture).
        // NOT gated on termMode: an Esc-cancelled generation still drains to done.
        if (llmBusy && llm && !terrainWorld && game.secret().terminal().built()) {
            llmBakeAcc += dt;
            if (llmBakeAcc >= 0.10f) {
                llmBakeAcc = 0.0f;
                x3::game::HoloTerminal& vterm = game.secret().terminal();
                x3::llm::PollResult pr = llm->poll(llmChat);
                llmReplyLog += pr.newTokens;
                if (!pr.newTokens.empty()) {
                    for (char ch : pr.newTokens) {
                        if (ch == '\r') continue;
                        if (ch == '\n') { vterm.setLastLine(llmLineAccum);
                                          vterm.addLine(""); llmLineAccum.clear(); continue; }
                        llmLineAccum += ch;
                        if (llmLineAccum.size() > kTermWrapCols) {
                            // Wrap at the last space; a single over-long word stays put.
                            std::string carry;
                            const size_t sp = llmLineAccum.find_last_of(' ');
                            if (sp != std::string::npos && sp > 0) {
                                carry = llmLineAccum.substr(sp + 1);
                                llmLineAccum.erase(sp);
                            }
                            vterm.setLastLine(llmLineAccum);
                            vterm.addLine(carry);
                            llmLineAccum = carry;
                        }
                    }
                    vterm.setLastLine(llmLineAccum);
                    vterm.trimBody(kTermMaxBody);
                }
                if (pr.done) {
                    llmBusy = false;
                    llmLineAccum.clear();
                    if (pr.failed) vterm.addLine("** LINK UNSTABLE - RETRY **");
                    x3::logInfo("terminal: VIGIL <- " + llmReplyLog);   // session transcript
                    llmReplyLog.clear();
                }
            }
        }

        // Camera state this frame (set by whichever branch runs), reused below
        // for the weapon viewmodel.
        float camX, camY, camZ, camYaw, camPitch;

        // ===================================================================
        // NETCODE PHASE 0 — DETERMINISTIC FIXED-STEP SIM ACCUMULATOR.
        // Spec: specs/NETCODE-architecture.spec.md §3.1 (Fiedler "Fix Your
        // Timestep!"). The sim (player movement + elevator + physics + scene sync)
        // now advances in WHOLE x3::net::kSimDt (1/60) steps — exactly the cadence
        // Jolt already steps internally — while rendering stays uncapped; leftover
        // real time carries forward in simAcc. This is the ONLY structural main.cpp
        // change for Phase 0 (kept localized so the 14900k's additive edits merge
        // around it). BEHAVIOR PARITY: input handling + rendering are unchanged; at
        // a 60 Hz render rate this runs exactly one sub-step/frame = identical to the
        // old single variable-dt step. Mouse-look + the jump edge are consumed ONCE
        // (first sub-step) so a multi-sub-step catch-up frame can't multiply them;
        // continuous movement axes apply every sub-step.
        // The full client/server input->snapshot routing (player.update fed by a
        // decoded NetCommand over the loopback transport) is deferred to Phase 0b.
        // ===================================================================
        // FREEZE: while a UI menu (main/pause/settings) is up, the sim/fixed-step is
        // frozen. We still drain the accumulator (advance + discard) so unpausing
        // doesn't trigger a multi-step catch-up burst; zero sub-steps run.
        const uint32_t simStepsRaw = simAcc.advance(dt);
        const uint32_t simSteps = gameUi.shouldFreezeSim() ? 0u : simStepsRaw;
        for (uint32_t s = 0; s < simSteps; ++s) {
            const bool firstSub = (s == 0);
            if (!noclip) {
                // ---- Walking player input (sampled this render frame) ----
                x3::game::PlayerInput in;
                if (keyDown(GLFW_KEY_W)) in.moveFwd    += 1.0f;
                if (keyDown(GLFW_KEY_S)) in.moveFwd    -= 1.0f;
                if (keyDown(GLFW_KEY_D)) in.moveStrafe += 1.0f;
                if (keyDown(GLFW_KEY_A)) in.moveStrafe -= 1.0f;
                // Arrow keys (RDP-friendly: mouse-look is flaky over Remote Desktop).
                // Up/Down = forward/back; Left/Right = TURN (applied to lookDX below).
                // Gated so they don't fight console history / terminal typing.
                const bool arrowsLive = !consoleOpen && !termMode;
                if (arrowsLive && keyDown(GLFW_KEY_UP))   in.moveFwd += 1.0f;
                if (arrowsLive && keyDown(GLFW_KEY_DOWN)) in.moveFwd -= 1.0f;
                // Right mouse button = walk forward (hold to autorun)
                if (!consoleOpen && !simFrozen && glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS)
                    in.moveFwd += 1.0f;
                in.sprint      = keyDown(GLFW_KEY_LEFT_SHIFT);
                // Edge + mouse-look apply only on the first sub-step of the frame.
                in.jumpPressed = firstSub && spaceNow && !prevSpace;   // rising edge
                in.lookDX = firstSub ? ddx : 0.0f;
                in.lookDY = firstSub ? ddy : 0.0f;
                // Left/Right arrows turn the view via the same lookDX path the mouse uses,
                // frame-rate-independent (~140 deg/s) — so you can play fine when the mouse
                // is unusable (e.g. raw-relative look over Remote Desktop is way too jumpy).
                if (firstSub && arrowsLive) {
                    const float arrowYaw = (keyDown(GLFW_KEY_RIGHT) ? 1.0f : 0.0f)
                                         - (keyDown(GLFW_KEY_LEFT)  ? 1.0f : 0.0f);
                    in.lookDX += arrowYaw * 1000.0f * (float)dt;   // keyboard turn rate
                }

                // Cell terminal / keypad open for typing: swallow movement + jump so the
                // keys (WASD/Space) type into the terminal instead of walking the player.
                if (termMode || codeMode) { in.moveFwd = 0.0f; in.moveStrafe = 0.0f; in.sprint = false; in.jumpPressed = false; }
                // CROUCH (hold C) / CRAWL (hold Left-Ctrl): lower the eye + slow the move.
                // Ctrl (prone) wins over C (crouch); release both to stand. Suppressed
                // while a console / terminal is open so typing doesn't duck the player.
                if (!consoleOpen && !termMode && player.isAlive()) {
                    const bool kCtrl = keyDown(GLFW_KEY_LEFT_CONTROL);
                    const bool kC    = keyDown(GLFW_KEY_C);
                    player.setStance(kCtrl ? x3::game::Player::Stance::Prone
                                   : kC    ? x3::game::Player::Stance::Crouch
                                           : x3::game::Player::Stance::Stand, *physics);
                }

                player.update(in, x3::net::kSimDt, *physics);

                // Advanced elevator: advance the cab, then carry the player if riding
                // (add the cab's vertical delta before the physics step resolves so
                // the capsule rides up with the platform instead of being left behind).
                if (elevator.built()) {
                    float edy = elevator.update(x3::net::kSimDt, scene, *physics);
                    if (edy != 0.0f) {
                        x3::phys::Vec3 pf = physics->getBodyPosition(player.body());
                        if (elevator.playerRiding(pf)) {
                            pf.y += edy;
                            physics->setBodyPosition(player.body(), pf);
                        }
                    }
                }

                physics->step(x3::net::kSimDt);
                scene.update(*physics);
            } else {
                // ---- Debug fly camera (does not move the player body) ----
                // Mouse-look integrates once per frame (first sub-step) so look isn't
                // multiplied across catch-up sub-steps.
                if (firstSub) {
                    const float sens = 0.0025f;
                    flyYaw += ddx * sens; flyPitch -= ddy * sens;
                    if (flyPitch >  1.55f) flyPitch =  1.55f;
                    if (flyPitch < -1.55f) flyPitch = -1.55f;
                }
                float fx = std::cos(flyPitch) * std::cos(flyYaw);
                float fy = std::sin(flyPitch);
                float fz = std::cos(flyPitch) * std::sin(flyYaw);
                float rl = std::sqrt(fx * fx + fz * fz); if (rl < 1e-4f) rl = 1e-4f;
                float rx = -fz / rl, rz = fx / rl;
                float spd = 4.0f * x3::net::kSimDt;
                if (keyDown(GLFW_KEY_LEFT_SHIFT)) spd *= 3.0f;
                if (keyDown(GLFW_KEY_W)) { flyX += fx*spd; flyY += fy*spd; flyZ += fz*spd; }
                if (keyDown(GLFW_KEY_S)) { flyX -= fx*spd; flyY -= fy*spd; flyZ -= fz*spd; }
                if (keyDown(GLFW_KEY_D)) { flyX += rx*spd; flyZ += rz*spd; }
                if (keyDown(GLFW_KEY_A)) { flyX -= rx*spd; flyZ -= rz*spd; }
                if (spaceNow) flyY += spd;
                if (keyDown(GLFW_KEY_LEFT_CONTROL)) flyY -= spd;

                // World still advances so the level keeps simulating while inspecting
                // (advance the elevator too; no carry — the fly cam isn't a rider).
                if (elevator.built()) elevator.update(x3::net::kSimDt, scene, *physics);
                physics->step(x3::net::kSimDt);
                scene.update(*physics);
            }
        }
        // Camera readback once per render frame from the post-sim state.
        if (!noclip) {
            player.camera(camX, camY, camZ, camYaw, camPitch);
        } else {
            camX = flyX; camY = flyY; camZ = flyZ; camYaw = flyYaw; camPitch = flyPitch;
        }
        // WEAPONS: apply + recover the weapon recoil kick. The kick is a transient
        // upward pitch offset added on top of the look pitch; it decays back to 0 so
        // the view recovers (recoil -> camera). Applied uniformly to setCamera, the
        // fire direction, the audio listener, and the viewmodel below.
        if (weaponRecoilPitch > 0.0f) {
            weaponRecoilPitch -= kRecoilRecover * dt;
            if (weaponRecoilPitch < 0.0f) weaponRecoilPitch = 0.0f;
        }
        camPitch += weaponRecoilPitch;
        if (camPitch >  1.55f) camPitch =  1.55f;   // keep within the look clamp

        // ---- THIRD-PERSON: drive the Jake avatar from the player's feet/look + swap
        // the RENDER camera to the follow/orbit cam (behind + above the player). The
        // gameplay camX/camY/camZ (the EYE) is LEFT UNCHANGED so the fire ray, audio
        // listener, flashlight, and prompts keep using the eye + look dir (FP-parity;
        // over-the-shoulder 3P aim is a documented follow-on). Only the rendered
        // viewpoint changes in 3P. No-op (renderCam == eye) in FP / unbuilt. ----
        float renderCamX = camX, renderCamY = camY, renderCamZ = camZ;
        if (!terrainWorld && !noclip && thirdPerson.thirdPerson() && thirdPerson.built()) {
            const x3::phys::Vec3 pfeet = player.feet();
            const float eyeH = camY - pfeet.y;   // current eye height (stance-aware)
            // Room for the PVS cull so the avatar isn't culled with its own room.
            uint32_t avatarRoom = x3::game::kNoRoom;
            if (canonWorld && canonFloor.valid())
                avatarRoom = canonFloor.roomAt(pfeet.x, pfeet.y, pfeet.z);
            const bool crouchedNow = player.stance() != x3::game::Player::Stance::Stand;
            // HELD-WEAPON GRIP LIVE-TUNE (TASK#53): push the grip_* cvars onto the
            // view as an additive override on the CURRENT weapon's table row, so Tim
            // can dial each gun by eye and read the effective values off the HUD.
            // Default 0 => no change to the baked kTpGripTable. See thirdperson.h.
            thirdPerson.setGripOverride(
                console->getFloat("grip_x"), console->getFloat("grip_y"),
                console->getFloat("grip_z"), console->getFloat("grip_pitch"),
                console->getFloat("grip_yaw"), console->getFloat("grip_roll"),
                console->getFloat("grip_scale"));
            thirdPerson.update(dt, scene, pfeet, eyeH, camYaw, camPitch, avatarRoom,
                               crouchedNow, prevFire);   // prevFire = last frame's held-fire
            const x3::game::ThirdPersonCamera tc =
                thirdPerson.camera(pfeet, eyeH, camYaw, camPitch);
            renderCamX = tc.camX; renderCamY = tc.camY; renderCamZ = tc.camZ;
        }
        device->setCamera(renderCamX, renderCamY, renderCamZ, camYaw, camPitch, 60.0f);
        // LEVEL ARCHITECT (--editor): in EDIT mode the host's fly-cam OVERRIDES the
        // game camera just set above (it calls device->setCamera with its own pose);
        // in PLAY mode the host returns false and the game camera above stands. All
        // host input is gated on editorWantsInput so panels never move the camera.
        if (editorMode && device->editorUIActive()) {
            bool emouse = false, ekbd = false;
            device->editorWantsInput(emouse, ekbd);
            editorHost.tick(dt, emouse, ekbd, *device);
        }
        // FLASHLIGHT (L toggles, default ON): re-issue the level's static ceiling
        // fixtures + a bright player-following light at the eye, so the dark halls
        // light up around you. Inserted FIRST so the 64-light cap never drops it.
        if (!terrainWorld) {
            bool lNow = keyDown(GLFW_KEY_L);
            if (lNow && !prevL) { flashlight = !flashlight;
                                  x3::logInfo(flashlight ? "flashlight ON" : "flashlight OFF"); }
            prevL = lNow;
            std::vector<x3::rhi::PointLight> fl = game.lightFixtures();
            if (flashlight) {
                const float fX = std::cos(camPitch) * std::cos(camYaw);
                const float fY = std::sin(camPitch);
                const float fZ = std::cos(camPitch) * std::sin(camYaw);
                // Main forward pool: the bright soft-edged circle that lights what you
                // LOOK at. Pulled in to 2 m (was 3) so it no longer skips the near field.
                x3::rhi::PointLight pl{};
                pl.pos[0] = camX + fX * 2.0f; pl.pos[1] = camY + fY * 2.0f; pl.pos[2] = camZ + fZ * 2.0f;
                pl.range  = 38.0f;   // HUGE circle; point-light attenuation gives the SOFT edge
                pl.color[0] = 6.0f; pl.color[1] = 5.6f; pl.color[2] = 4.9f;  // bright warm-white (HDR)
                fl.insert(fl.begin(), pl);
                // Near light AT the eye so things RIGHT in front of you (barrels, enemies,
                // the held weapon) are ALWAYS lit — a flashlight should never leave the
                // near field black. Smaller range, same warm-white.
                x3::rhi::PointLight eyePl{};
                eyePl.pos[0] = camX + fX * 0.3f; eyePl.pos[1] = camY + fY * 0.3f; eyePl.pos[2] = camZ + fZ * 0.3f;
                eyePl.range  = 13.0f;
                eyePl.color[0] = 3.2f; eyePl.color[1] = 3.0f; eyePl.color[2] = 2.6f;
                fl.insert(fl.begin(), eyePl);
            }
            // CANONLEVEL ROOM LIGHTING: the data-driven floor has no env_art Light_A
            // fixtures, so game.lightFixtures() is empty and the rooms would only get
            // ambient + the flashlight (the DARK bug). Append the player's currently
            // VISIBLE rooms' ceiling lights (current room + PVS neighbours) — capped at
            // 16 so the active count stays under the 64-light device cap even with 53
            // rooms in the floor. Appended AFTER the flashlight so the flashlight (at the
            // front) is never the one dropped if we somehow brush the cap.
            if (canonWorld && canonFloor.valid()) {
                // Compute the per-frame visible-room set ONCE here (portal flood-fill,
                // frustum-directional) and stash it in canonVisRooms; the render path below
                // reuses the SAME set so newly-visible rooms down the hall both LIGHT UP and
                // DRAW, all capped consistently. r_roomcull 0 falls back to the 1-hop set so
                // a noclip overview is still reasonably lit.
                if (console->getInt("r_roomcull") != 0) {
                    const uint32_t depth = (uint32_t)std::max(1, console->getInt("r_culldepth"));
                    int fbw = 0, fbh = 0; glfwGetFramebufferSize(window, &fbw, &fbh);
                    const float aspect = (float)std::max(1, fbw) / (float)std::max(1, fbh);
                    x3::game::Frustum fr = x3::game::Frustum::build(
                        camX, camY, camZ, camYaw, camPitch, 60.0f, aspect);
                    canonFloor.floodVisibleRoomsAt(camX, camY, camZ, fr, &canonDoors,
                                                   depth, kCanonRoomBudget, canonVisRooms);
                } else {
                    canonFloor.visibleRoomsAt(camX, camY, camZ, canonVisRooms);
                }
                // Cap lights at 16 closest-to-eye over the SAME visible-room set.
                x3::game::selectVisibleCanonLights(canonLights, canonVisRooms,
                                                   camX, camY, camZ, fl, 16);
            }
            // FROMDOC LIGHTING: append the LevelDoc's authored point lights (closest
            // 16 to the eye) after the flashlight, mirroring the canonlevel feed.
            if (docWorld && docLevel.built())
                docLevel.selectLights(camX, camY, camZ, fl, 16);
            // LIVING WORLD: LOCKDOWN red emissive shift — at alert level 3+ every
            // facility light leans hard into alarm red (the level-3 visual tell).
            if (facilityAlertOn && facilityAlert.redShift() > 0.0f) {
                const float rs = facilityAlert.redShift();
                for (auto& L : fl) {
                    const float lum = (L.color[0] + L.color[1] + L.color[2]) / 3.0f;
                    L.color[0] = L.color[0] * (1.0f - rs) + lum * 1.7f * rs;
                    L.color[1] *= 1.0f - rs * 0.78f;
                    L.color[2] *= 1.0f - rs * 0.82f;
                }
            }
            if (fl.size() > 64) fl.resize(64);
            device->setPointLights(fl.data(), (uint32_t)fl.size());
        }
        prevSpace = spaceNow;

        // ---- Level 1 controller tick: advance doors, run triggers, spawn/clear
        // enemies, arm on pickup, flip objectives, detect the win. Runs AFTER
        // scene.update() so monster facing survives the per-frame physics sync.
        // Phase 2a: pass the player as the damage sink + the enemy-attack FX so
        // guards/drone/Martinez hurt the player (enemies attack only while alive). ----
        const x3::phys::Vec3 camPos{ camX, camY, camZ };
        // ---- --world fromdoc: LIVE-EDIT loop. Apply a queued `level_reload`, poll
        // the doc file's mtime (a save from the F8 editor or ANY text editor), and
        // walk the player point through the doc's trigger zones. The reload tears
        // down only the doc-built objects and rebuilds in place — the player body is
        // untouched, so you keep standing where you are while the world rebuilds. ----
        if (docWorld && docLevel.built() && !simFrozen) {
            if (docReloadRequested) {
                docReloadRequested = false;
                docLevel.reloadNow(scene, *device, *physics);
            } else {
                docLevel.pollHotReload(glfwGetTime(), scene, *device, *physics);
            }
            docLevel.updateTriggers(camPos);
        }
        if (simFrozen) {
            // Sim frozen by a UI menu: skip the level controller / streaming / ocean
            // clock so doors/enemies/objectives/waves hold still. (Terrain tiles are
            // already resident; nothing falls because physics isn't stepping.)
        } else if (terrainWorld) {
            // Outdoor world: no Level 1 controller. STREAM tiles around the camera
            // focus — stream in newly-in-range tiles (async gen on jobs, budgeted
            // uploads here on the main thread), stream out receded ones, and apply
            // LOD to the resident set. The under-focus 3x3 is generated
            // synchronously so collision is always present (no fall-through).
            terrainStreamer.update(scene, *device, *physics, camX, camZ);
            // OCEAN: advance the wave clock + (re)apply the water params so the sea
            // animates each frame. The water plane follows the camera (the device
            // centers the grid under it), so it covers the whole visible sea.
            if (oceanWorld) {
                oceanTime += dt;
                x3::rhi::IRenderDevice::WaterParams wp{};
                wp.enabled = true;
                wp.seaLevel = oceanSeaLevel;
                wp.time = oceanTime;
                wp.amplitude = 0.6f; wp.steepness = 0.6f; wp.waveLength = 16.0f; wp.speed = 1.0f;
                wp.deepColor[0] = 0.015f; wp.deepColor[1] = 0.06f;  wp.deepColor[2] = 0.10f;
                wp.shallowColor[0] = 0.10f; wp.shallowColor[1] = 0.32f; wp.shallowColor[2] = 0.36f;
                wp.sunDir[0] = 0.4f; wp.sunDir[1] = 1.0f; wp.sunDir[2] = 0.3f;
                wp.specular = 14.0f; wp.fresnel = 0.02f;
                device->setWaterParams(wp);
            }
        } else {
            { const double _pt0 = glfwGetTime();
              game.tick(dt, scene, *physics, camPos, camPos, &player, enemyAttackFx);
              g_perf.tick += glfwGetTime() - _pt0; }
            // D14: forward any trigger-zone ENTRIES from this tick into Lua so pak
            // script DATA can react. zone = the L1Trigger id; who = "player". The
            // engine never sees this — Level1Game just records the fired ids.
            if (scripts) {
                for (uint32_t tid : game.lastFiredTriggers())
                    scripts->fire("trigger_enter",
                        {{"zone", std::to_string(tid)}, {"who", "player"}});
            }
            // ---- MISSION DOC (g_missiondoc=1): bridge this tick's game state into
            // mission flags (door/armed/checkpoint/boss beats, trigger zones, kill
            // counters) and advance the runner — it drives the objective line via
            // the free-text lane. Default-off: missionDocActive is false unless the
            // cvar was 1 at boot AND the doc validated. ----
            if (missionDocActive) {
                x3::game::pollLevel1MissionFlags(game, missionEvents, chatTrees.flags());
                missionRunner.tick();
            }
            // LIVING WORLD: the facility civilians (idle/wander; scatter+cower on
            // gunfire via onViolence in the fire block; return after calm).
            if (facilityCrowd.built()) facilityCrowd.update(dt, scene);
            // LIVING WORLD: the FACILITY ALERT LEVEL — feed observations, apply
            // effects (reinforcements, lockdown doors). Lights/HUD read it below.
            if (facilityAlertOn) {
                // Observers: every live hostile is the facility's eyes and ears.
                x3::game::Level1Game::EnemyMark marks[32];
                const uint32_t nObs = game.liveEnemyMarks(marks, 32);
                x3::phys::Vec3 obs[32];
                for (uint32_t i = 0; i < nObs; ++i) obs[i] = marks[i].pos;
                // Player seen: any live guard holding LOS this frame.
                bool seen = false;
                auto scanLos = [&](const x3::game::MonsterManager& mm) {
                    for (uint32_t i = 0; i < mm.count() && !seen; ++i)
                        if (mm.at(i).alive() && mm.at(i).hasLineOfSight()) seen = true;
                };
                scanLos(game.corridorEnemies());
                scanLos(game.checkpointEnemies());
                // Bodies: every downed enemy registers a corpse (deduped inside);
                // a guard patrolling within corpseRadius of one DISCOVERS it.
                auto scanCorpses = [&](const x3::game::MonsterManager& mm) {
                    for (uint32_t i = 0; i < mm.count(); ++i)
                        if (!mm.at(i).alive()) facilityAlert.registerCorpse(mm.at(i).pos());
                };
                scanCorpses(game.corridorEnemies());
                scanCorpses(game.checkpointEnemies());
                facilityAlert.update(dt, camPos, obs, nObs, seen);
                // ---- THE GREAT FOLD stitch: wire the world-map fast-travel gate
                // to the REAL alert level. Before the fold these lived on separate
                // branches (world-map's fastTravelGate keyed on the "alert.active"
                // StoryFlag — a placeholder hook with "no alert system here"; the
                // facility AlertSystem lived on feat/living-world). Now both are
                // folded: mirror the live alert level onto that flag so fast travel
                // is BLOCKED whenever the facility is alerted (level > 0) and frees
                // up again on de-escalation. (worldMap reads chatTrees.flags().)
                if (facilityAlert.level() > 0) chatTrees.flags().set("alert.active");
                else                           chatTrees.flags().clear("alert.active");
                // EFFECT: investigate — pop the stimulus position (in worlds with
                // AmbientEcology patrols this routes them; Level 1's combat guards
                // already hunt via their own AI, so the pop is the Lua/mission seam).
                x3::phys::Vec3 invPos;
                (void)facilityAlert.takeInvestigatePos(invPos);
                // EFFECT: reinforcement spawns (entering SEARCH / KILL SQUAD).
                if (const int want = facilityAlert.takeSpawnRequests(); want > 0) {
                    const x3::game::Level1Layout& alay = game.layout();
                    for (int k = 0; k < want; ++k) {
                        x3::game::MonsterSystem::Tuning rt = x3::game::tuningFor(
                            facilityAlert.level() >= 4 ? x3::game::EnemyType::Illuminated
                                                       : x3::game::EnemyType::DominionTrooper);
                        const float ox = ((k % 2) ? 1.6f : -1.6f) * (float)(1 + k / 2);
                        game.checkpointEnemies().spawn(
                            scene, *device, *physics, x3::game::riggedGlbRoot(),
                            x3::phys::Vec3{alay.checkpointCenter.x + ox,
                                           alay.checkpointCenter.y,
                                           alay.checkpointCenter.z + 1.0f}, rt);
                    }
                    x3::logInfo("alert: " + std::string(x3::game::alertLevelName(facilityAlert.level()))
                                + " — spawned " + std::to_string(want) + " reinforcement(s)");
                }
                // EFFECT: the level-3 zone-door LOCKDOWN (restores its own locks).
                alertDoorLock.update(facilityAlert, game.doors());
            }
            // ---- CANONLEVEL DOORS: tick the SM_Door_A slide animation. Doors are
            // MANUAL — the player opens/closes one by aiming at the slab (or its button)
            // and pressing E (the use block above calls tryUse()->toggle()). There is
            // deliberately NO proximity auto-open: a door stays Closed (its slab blocks
            // the player like a wall) until toggled, and stays Open until toggled shut.
            //
            // (Was: a 2.2 m proximity tick auto-opened doors. That fought the manual
            // path — pressing E to CLOSE a door you were standing next to re-opened it on
            // the very next frame, so "E never closed" — and it removed the deliberate
            // open-the-door beat entirely. Removed so E is the sole driver.)
            if (canonWorld && canonFloor.valid()) {
                canonDoors.update(dt, scene, *physics);
                // SECURITY KEYCARD: grab it by walking up to it (proximity, XZ).
                if (!canonKeycardTaken && canonKeycardEnt != x3::game::kNoLink) {
                    const float kdx = camPos.x - canonKeycardX, kdz = camPos.z - canonKeycardZ;
                    if (kdx * kdx + kdz * kdz < 1.6f * 1.6f) {
                        canonKeycardTaken = true;
                        keycardMask |= (1u << (uint32_t)x3::game::kKeycardSecurity);
                        if (canonKeycardEnt < scene.size()) scene.get(canonKeycardEnt).visible = false;
                        npcBarkText = "Acquired the Security keycard"; npcBarkTimer = 3.0f;
                        x3::logInfo("--world canonlevel: Security keycard acquired");
                    }
                }
            }
            // ---- CANONLEVEL GAMEPLAY: tick the canon enemies/boss/girls (they chase + attack
            // the player + animate). The medical-bay rescue clock arms once the player reaches
            // the Medical Bay (room or its neighbours) so the 5-min infection timers don't run
            // from load — mirrors Level1Game's F2-hub gating. ----
            if (canonWorld && canonPlay.built()) {
                if (!canonMedicalActive) {
                    const uint32_t medRoom = canonFloor.roomByName("Medical Bay");
                    const uint32_t here = canonFloor.roomAt(camPos.x, camPos.y, camPos.z);
                    if (medRoom != x3::game::kNoRoom && here == medRoom) {
                        canonPlay.rescue().activate();
                        canonMedicalActive = true;
                        x3::logInfo("--world canonlevel: Medical Bay reached — rescue clocks started "
                                    "(kill the attackers to save the girls before the infection)");
                    }
                }
                const double _pt0 = glfwGetTime();
                canonPlay.tick(dt, scene, *physics, camPos, &player, enemyAttackFx);
                g_perf.tick += glfwGetTime() - _pt0;
            }
            // ---- SECRET ROOM payoff: game.tick() ticks the cell terminal + the room's
            // loot collection (latching counts). Apply the gameplay EFFECTS here, where
            // we own the concrete Player: each newly-collected HEALTH pack heals +50, and
            // the NANO-BOOSTER (a tech/bio augment) triggers a full bio-surge heal as a
            // stand-in effect (see secret_room.cpp's upgrade-system TODO). ----
            {
                const x3::game::SecretRoom& sr = game.secret();
                uint32_t hg = sr.healthCollected();
                if (hg > prevSecretHealth) { player.heal(50 * (int)(hg - prevSecretHealth)); prevSecretHealth = hg; }
                if (sr.nanoBoosterActive() && !prevSecretNano) {
                    prevSecretNano = true;
                    player.heal();   // full bio-surge (TODO: a real Augment system raises maxHP/abilities)
                    x3::logInfo("secret: NANO-BOOSTER augment online — bio surge");
                }
            }
            // Spire mid floors (F3/F4/F5): dispatch their floor-hub triggers (the F5
            // hub starts the rescue clock) then tick their enemy groups + gated victim.
            // Reaching the F4 hub OPENS the F4->F5 connector that "finds" the off-
            // elevator Floor 4.5 Nexus (the encounter is discoverable only once the
            // player has worked past F4 — never at load).
            for (uint32_t tid : midTriggers.update(camPos)) {
                midFloors.onTrigger(tid);
                if (tid == (uint32_t)x3::game::SpireMidTrigger::F4Hub)
                    nexusTriggers.setEnabled((uint32_t)x3::game::NexusTrigger::Connector, true);
            }
            midFloors.tick(dt, scene, *physics, camPos, camPos, &player, enemyAttackFx);
            // Spire top floors (F6/F7 Act-1 finale): dispatch their hub triggers (the F7
            // hub starts Sarah's rescue clock) then tick the enemy groups + Clone boss +
            // gated victim.
            for (uint32_t tid : topTriggers.update(camPos)) topFloors.onTrigger(tid);
            topFloors.tick(dt, scene, *physics, camPos, camPos, &player, enemyAttackFx);
            // Floor 4.5 Nexus / The Chorus: dispatch its connector (which discovers +
            // arms the Chorus) then tick the multi-pod boss (inert until armed).
            for (uint32_t tid : nexusTriggers.update(camPos)) nexus.onTrigger(tid);
            nexus.tick(dt, scene, *physics, camPos, &player, enemyAttackFx);
            // Hidden Floor-7 sub-levels: GATE the hidden descent on the F7 finale being
            // complete (the Clone boss is dead AND Sarah was saved). openDescent() is a
            // one-way no-op until BOTH hold; reading spire_top here is READ-ONLY. Once the
            // descent opens, dispatch its triggers (hidden lift + per-sub-level hubs, the
            // SL3 hub starts Chen's clock) and tick the sub-level encounters + hazard.
            const bool cloneFallen =
                topFloors.plan(x3::game::SpireTopFloor::F7).hasBoss &&
                topFloors.boss().aliveCount() == 0;
            subLevels.openDescent(cloneFallen, sarahSaved);
            for (uint32_t tid : subTriggers.update(camPos)) subLevels.onTrigger(tid);
            subLevels.tick(dt, scene, *physics, camPos, camPos, &player, enemyAttackFx);
        }

        // ---- Phase 2a: death -> respawn. The player enters the death state at
        // HP 0; player.update() freezes movement + ticks the respawn countdown.
        // When it elapses, teleport the body back to the level-start checkpoint and
        // restore full HP (the damage flash is cleared by resetHealth()). Enemies
        // are NOT reset (documented in Level1Game::checkpoint()). ----
        if (player.readyToRespawn()) {
            const x3::phys::Vec3 cp = terrainWorld ? terrainSpawn : game.checkpoint();
            physics->setBodyPosition(player.body(), cp);
            player.resetHealth();
            x3::logInfo("respawn: player restored at start (full HP)");
        }

        // ---- M9: drive the 3D listener from the player camera each frame ----
        audio->setListener(camX, camY, camZ, camYaw, camPitch);

        // ---- M9: footsteps. Time them to horizontal speed while grounded (not in
        // noclip): estimate speed from the camera's XZ delta this frame; while
        // moving, play a quiet pitched-down step every kStepInterval seconds. ----
        if (prevCamValid && !noclip && player.grounded() && dt > 0.0f) {
            const float dxc = camX - prevCamX, dzc = camZ - prevCamZ;
            const float speed = std::sqrt(dxc * dxc + dzc * dzc) / dt; // m/s
            if (speed > 0.6f) {
                // Cadence scales a little with speed (faster -> quicker steps).
                const float kStepInterval = (speed > 6.5f) ? 0.32f : 0.45f;
                stepTimer += dt;
                if (stepTimer >= kStepInterval) {
                    stepTimer = 0.0f;
                    audio->playSound2D(sndStep, 0.22f, 0.55f); // quiet, pitched down
                }
            } else {
                stepTimer = 0.0f; // reset cadence when stopped
            }
        }
        prevCamX = camX; prevCamZ = camZ; prevCamValid = true;

        // M9: pickup chime on the arm rising edge (the controller also plays one
        // on the beat-7 arm; keep this for the 2D UI chime feel).
        if (game.armed() && !prevArmed)
            audio->playSound2D(sndPickup, 0.8f, 1.0f);
        prevArmed = game.armed();

        // ---- Phase 2b: SUPER-STRENGTH MELEE on the V key or middle-mouse rising
        // edge. The unarmed-strength punch: damages + knocks back every enemy in a
        // short forward arc, and brute-forces a closed door you punch. Works whether
        // or not armed (the pistol is the separate LMB verb). Gated by the
        // MeleeSystem's own cooldown; only while alive. ----
        // While the console, a UI menu, the cell terminal, or a keypad is capturing
        // input, gameplay verbs (melee / fire) must NOT trigger — no shooting through
        // the pause menu, no punching while typing the override code.
        // A running chat-tree conversation also pauses the combat verbs (no firing
        // through Lena's dialog box) — same capture idea as the terminal/keypad.
        const bool uiCapture = consoleOpen || uiMenuActive || termMode || codeMode ||
                               chatTrees.active() || worldMapOpen;
        bool meleeNow = !uiCapture && (keyDown(GLFW_KEY_V) ||
            glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS);
        if (meleeNow && !prevMelee && player.isAlive() && !terrainWorld) {
            x3::phys::Vec3 eye{ camX, camY, camZ };
            x3::phys::Vec3 dir{ std::cos(camPitch) * std::cos(camYaw),
                                std::sin(camPitch),
                                std::cos(camPitch) * std::sin(camYaw) };
            x3::game::MeleeResult mr = game.onMelee(eye, dir, scene, *physics);
            if (!mr.onCooldown) {
                // Melee swing FX: a short tracer from the muzzle out to the punch's
                // far point so the strength swing reads (reuses the CombatFx beam).
                const x3::phys::Vec3 muzzle = muzzleFromCamera(camX, camY, camZ, camYaw, camPitch);
                combatFx.addTracer(muzzle, mr.swingTo);
                // A heavy "thump" cue (reuse the gunshot WAV at low pitch).
                audio->playSound3D(sndGun, muzzle.x, muzzle.y, muzzle.z, 0.7f, 0.6f);
                // Melee juice: blood at the punch's far point per enemy hit; a
                // death burst when the punch kills.
                if (mr.enemiesHit) {
                    combatFx.spawnBlood(mr.swingTo, dir);
                    if (mr.enemiesKilled) combatFx.spawnDeath(mr.swingTo);
                }
                if (mr.doorForced)  x3::logInfo("melee: brute-forced a door open");
                if (mr.enemiesHit)  x3::logInfo("melee: punched " + std::to_string(mr.enemiesHit) +
                                                " enemy(ies), killed " + std::to_string(mr.enemiesKilled));
            }
        }
        prevMelee = meleeNow;

        // ---- Combat: FIRE — only effective when armed. The DATA-DRIVEN ARSENAL
        // gates the shot (fire rate / ammo / reload), resolves it (1 ray, N spread
        // pellets, or a projectile bolt) per the selected WeaponDef, and applies
        // recoil. Each resolved hitscan ray runs through the existing Level-1 combat
        // path (game.onFire -> per-group enemy raycast + the existing CombatFx);
        // projectiles are spawned into a host-owned list advanced below. Automatic
        // weapons fire while held; others fire on the LMB rising edge. ----
        (void)fireCooldown; (void)kFireCooldown;   // (legacy cooldown — arsenal owns timing now)
        bool fireHeld = !uiCapture && !simFrozen && glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
        bool wantFire = arsenal.current().automatic ? fireHeld : (fireHeld && !prevFire);
        // In --world canonlevel the legacy `game` is unbuilt; the canon sidearm gates firing.
        const bool playerArmed = game.armed() || (canonWorld && canonPlay.armed());
        if (wantFire && playerArmed && player.isAlive() && arsenal.canFire()) {
            x3::phys::Vec3 eye{ camX, camY, camZ };
            x3::phys::Vec3 dir{ std::cos(camPitch) * std::cos(camYaw),
                                std::sin(camPitch),
                                std::cos(camPitch) * std::sin(camYaw) };
            x3::game::ResolvedFire shot = arsenal.fire(eye, dir, weaponRng);
            const x3::phys::Vec3 muzzle = muzzleFromCamera(camX, camY, camZ, camYaw, camPitch);
            // LIVING WORLD: gunfire is VIOLENCE — any civilians in earshot scatter,
            // and any guard in earshot raises the facility alert (resolved against
            // the live observers at the next alert update).
            if (facilityCrowd.built()) facilityCrowd.onViolence(eye);
            if (facilityAlertOn) facilityAlert.reportGunshot(eye);
            // Recoil -> camera (transient upward kick; recovered in the camera block).
            weaponRecoilPitch += shot.recoilPitchDeg * (3.14159265f / 180.0f);

            // Per-weapon FX kind (plasma blue / chaingun sparky / shotgun wide / ...)
            // + the CURRENT weapon's distinct fire sound (instead of one shared gun).
            const x3::game::WeaponFxKind muzzleKind =
                x3::game::fxKindFromId(arsenal.current().muzzleFx);
            const x3::game::WeaponFxKind impactKind =
                x3::game::fxKindFromId(arsenal.current().impactFx);
            const x3::audio::SoundHandle fireSnd = currentFireSfx();
            // Task #21 FIX B: loopable weapons (fireSfxLoop=true) drive a single sustained
            // LOOP voice (reconciled just below the fire block) instead of a per-round
            // one-shot — so suppress the per-shot fire SFX here for those weapons.
            const bool usesFireLoop = arsenal.current().fireSfxLoop;
            if (!shot.projectiles.empty()) {
                // ---- Projectile weapon (plasma): spawn a travelling bolt. ----
                const auto& pj = shot.projectiles[0];
                projectiles.push_back(LiveProjectile{ muzzle, pj.vel, pj.damage, 0.0f, pj.range, impactKind });
                combatFx.spawnMuzzleFlash(muzzle, dir, muzzleKind);
                if (!usesFireLoop)
                    audio->playSound3D(fireSnd, muzzle.x, muzzle.y, muzzle.z, 0.85f, 0.9f);
                x3::logInfo("fire: " + arsenal.current().name + " bolt launched");
            } else {
                // ---- Hitscan weapon (pistol/SMG/shotgun): one onFire per pellet. ----
                // PER-WEAPON damage to monsters: each ray carries the firing weapon's
                // WeaponDef damage (set by the arsenal; includes beam falloff/chain),
                // so a shotgun pellet, an SMG round, and a plasma bolt all deal their
                // OWN damage instead of a single shared constant.
                bool anyKill = false, anyHit = false; int lastHp = 0;
                combatFx.spawnMuzzleFlash(muzzle, dir, muzzleKind);   // per-weapon flash (hitscan)
                for (const auto& ray : shot.rays) {
                    const int wdmg = ray.damage;          // this pellet/ray's damage
                    x3::game::FireResult r = game.onFire(eye, ray.dir, scene, *physics, wdmg);
                    // --world canonlevel: the legacy groups are empty; route the shot through
                    // the canon enemies/boss/girls instead (arm-gated by canonPlay.onFire).
                    if (!r.hitMonster && canonWorld && canonPlay.built()) {
                        x3::game::FireResult rc = canonPlay.onFire(eye, ray.dir, scene, *physics, wdmg);
                        if (rc.hitMonster || (!r.hit && rc.hit)) r = rc;
                    }
                    // If the B1 groups didn't take it, try the F3/F4/F5 enemies (the
                    // shot is already arm-gated by the arsenal/Level1Game::onFire).
                    if (!r.hitMonster && game.armed()) {
                        x3::game::FireResult rm = midFloors.onFire(eye, ray.dir, scene, *physics, wdmg);
                        if (rm.hitMonster || (!r.hit && rm.hit)) r = rm;
                    }
                    // Then the F6/F7 top-floor enemies + the Clone boss.
                    if (!r.hitMonster && game.armed()) {
                        x3::game::FireResult rt = topFloors.onFire(eye, ray.dir, scene, *physics, wdmg);
                        if (rt.hitMonster || (!r.hit && rt.hit)) r = rt;
                    }
                    // Then the Floor 4.5 Chorus pods (no-op until the Nexus is armed; a
                    // pod killed this way counts as KILLED, not saved).
                    if (!r.hitMonster && game.armed()) {
                        x3::game::FireResult rn = nexus.onFire(eye, ray.dir, scene, *physics, wdmg);
                        if (rn.hitMonster || (!r.hit && rn.hit)) r = rn;
                    }
                    // Then the hidden sub-level enemies + the Frozen Collective (a clean
                    // miss until the descent has opened).
                    if (!r.hitMonster && game.armed()) {
                        x3::game::FireResult rs = subLevels.onFire(eye, ray.dir, scene, *physics, wdmg);
                        if (rs.hitMonster || (!r.hit && rs.hit)) r = rs;
                    }
                    combatFx.addTracer(muzzle, r.endPoint, muzzleKind);   // tracer (Lightning -> jagged bolt) + muzzle burst per pellet
                    if (r.killed) { combatFx.spawnDeath(r.endPoint); anyKill = true; }
                    else if (r.hitMonster) { combatFx.spawnBlood(r.hitPoint, ray.dir); anyHit = true; lastHp = r.hpAfter; }
                    else {
                        x3::phys::RayHit wallHit =
                            physics->rayCast(eye, ray.dir, x3::game::kFireMaxDist, x3::phys::Layer::Static);
                        if (wallHit.hit) combatFx.spawnImpact(wallHit.point, wallHit.normal, impactKind);
                    }
                    if (r.killed)
                        audio->playSound3D(sndDeath, r.endPoint.x, r.endPoint.y, r.endPoint.z, 1.0f, 1.0f);
                }
                if (!usesFireLoop)
                    audio->playSound3D(fireSnd, muzzle.x, muzzle.y, muzzle.z, 0.85f, 1.0f);
                if (anyKill)      x3::logInfo("fire: enemy killed! (" + arsenal.current().name + ")");
                else if (anyHit)  x3::logInfo("fire: enemy hit — HP " + std::to_string(lastHp));
            }
        }
        prevFire = fireHeld;

        // ---- Task #21 FIX B: reconcile the sustained auto-fire LOOP voice EVERY frame.
        // A loopable weapon's whine should play while the player is actively holding
        // fire on a usable weapon, and CUT within a frame the instant any stop
        // condition is true: trigger released (!fireHeld already folds in console-open
        // and sim-frozen), not armed, dead, mid-reload, or out of ammo. Weapon switch
        // is handled by comparing the desired loop SOUND to the running one (switching
        // to a non-loop weapon, or a different loop WAV, stops the old voice). We never
        // start a second voice because we hold a single fireLoop handle. ----
        {
            const x3::game::WeaponDef& cw = arsenal.current();
            const bool hasAmmo = arsenal.infiniteAmmo() || arsenal.currentState().ammoInMag > 0;
            const bool wantLoop = cw.fireSfxLoop && fireHeld && playerArmed &&
                                  player.isAlive() && !arsenal.isReloading() && hasAmmo;
            const x3::audio::SoundHandle desired = wantLoop ? currentFireSfx() : x3::audio::SoundHandle{};
            // Stop the running loop if it shouldn't run, or if the desired sound changed
            // (weapon switch between two loop weapons with different WAVs).
            if (fireLoop.valid() && (!wantLoop || desired.id != fireLoopSnd.id)) {
                audio->stopLoop(fireLoop);
                fireLoop = x3::audio::LoopHandle{};
                fireLoopSnd = x3::audio::SoundHandle{};
            }
            // Start a loop if one is wanted and none is running (rising edge / new weapon).
            if (wantLoop && !fireLoop.valid() && desired.valid()) {
                fireLoop = audio->startLoop(desired, 0.85f, 1.0f);  // 0.85 matches the old per-shot gain
                fireLoopSnd = desired;
            }
        }

        // ---- WEAPONS: advance live projectile bolts. Each step moves the bolt and
        // raycasts the segment against Enemy then Static; on an enemy hit it deals
        // damage via the enemy fire path (aimed straight at the bolt's travel dir);
        // on any surface hit it spawns an impact + despawns. Bolts despawn at range. ----
        if (!simFrozen && !terrainWorld && !projectiles.empty()) {
            for (size_t pi = 0; pi < projectiles.size(); ) {
                LiveProjectile& b = projectiles[pi];
                float speed = std::sqrt(b.vel.x*b.vel.x + b.vel.y*b.vel.y + b.vel.z*b.vel.z);
                float stepLen = speed * dt;
                if (stepLen < 1e-5f) stepLen = 1e-5f;
                x3::phys::Vec3 ndir{ b.vel.x/speed, b.vel.y/speed, b.vel.z/speed };
                bool consumed = false;
                x3::phys::RayHit eh = physics->rayCast(b.pos, ndir, stepLen, x3::phys::Layer::Enemy);
                if (eh.hit) {
                    // PER-WEAPON damage: the bolt carries its WeaponDef projectile damage.
                    x3::game::FireResult r = game.onFire(b.pos, ndir, scene, *physics, b.damage);
                    if (!r.hitMonster && canonWorld && canonPlay.built()) {   // canon enemies/boss/girls
                        x3::game::FireResult rc = canonPlay.onFire(b.pos, ndir, scene, *physics, b.damage);
                        if (rc.hitMonster) r = rc;
                    }
                    if (!r.hitMonster) {   // try the F3/F4/F5 enemies for this bolt
                        x3::game::FireResult rm = midFloors.onFire(b.pos, ndir, scene, *physics, b.damage);
                        if (rm.hitMonster) r = rm;
                    }
                    if (!r.hitMonster) {   // then the F6/F7 enemies + the Clone boss
                        x3::game::FireResult rt = topFloors.onFire(b.pos, ndir, scene, *physics, b.damage);
                        if (rt.hitMonster) r = rt;
                    }
                    if (!r.hitMonster) {   // then the Floor 4.5 Chorus pods (if armed)
                        x3::game::FireResult rn = nexus.onFire(b.pos, ndir, scene, *physics, b.damage);
                        if (rn.hitMonster) r = rn;
                    }
                    if (!r.hitMonster) {   // then the hidden sub-level enemies + Frozen Collective
                        x3::game::FireResult rs = subLevels.onFire(b.pos, ndir, scene, *physics, b.damage);
                        if (rs.hitMonster) r = rs;
                    }
                    combatFx.addTracer(b.pos, eh.point);
                    if (r.killed) { combatFx.spawnDeath(eh.point);
                        audio->playSound3D(sndDeath, eh.point.x, eh.point.y, eh.point.z, 1.0f, 1.0f); }
                    else combatFx.spawnBlood(eh.point, ndir);
                    consumed = true;
                } else {
                    x3::phys::RayHit sh = physics->rayCast(b.pos, ndir, stepLen, x3::phys::Layer::Static);
                    if (sh.hit) { combatFx.spawnImpact(sh.point, sh.normal, b.impactKind); combatFx.addTracer(b.pos, sh.point); consumed = true; }
                }
                if (!consumed) {
                    b.pos = x3::phys::Vec3{ b.pos.x + b.vel.x*dt, b.pos.y + b.vel.y*dt, b.pos.z + b.vel.z*dt };
                    b.traveled += stepLen;
                    if (b.traveled >= b.range) consumed = true;   // out of range -> despawn
                }
                if (consumed) { projectiles[pi] = projectiles.back(); projectiles.pop_back(); }
                else ++pi;
            }
        }

        // Advance FX timers (tracer lifetimes + muzzle flash) only while the sim
        // runs; frozen during a UI menu so particles/tracers hold still.
        if (!simFrozen) combatFx.update(dt);
        // M9: tick the audio system (reaps finished one-shot voices). Always ticked
        // so audio voices don't pile up while paused.
        audio->update(dt);
        // D14: pump Lua scripts (advances x3.time(), fires due x3.after() timers,
        // calls onUpdate(dt) on every healthy script). Frozen with the sim so a
        // paused game doesn't advance script timers.
        if (scripts && !simFrozen) scripts->update(dt);

        // ---- RT ACOUSTICS (snd_rtacoustics): re-trace active emitters + the
        // periodic room probe against the TLAS, hand the room reverb targets to
        // the mixer, and (de)install the mixer's occlusion provider on cvar
        // change. Inert (provider never installed) without RT hardware. ----
        {
            const bool rtaOn = console->getInt("snd_rtacoustics") != 0 &&
                               device->rayTracingSupported();
            if (rtaOn != rtaHooked) {
                audio->setOcclusionProvider(
                    rtaOn ? &x3::audio::RtAcoustics::occlusionThunk : nullptr,
                    rtaOn ? &rtAcoustics : nullptr);
                rtAcoustics.setEnabled(rtaOn);
                if (!rtaOn) audio->setReverbParams(0.3f, 0.0f);   // dry again
                rtaHooked = rtaOn;
                x3::logInfo(rtaOn ? "[rta] RT acoustics ON (TLAS occlusion + room reverb)"
                                  : "[rta] RT acoustics OFF");
            }
            if (rtaOn) {
                rtAcoustics.setListener(camX, camY, camZ);
                rtAcoustics.update(dt);
                const x3::audio::RoomEstimate& rm = rtAcoustics.room();
                audio->setReverbParams(rm.t60, rm.wet);
                if (console->getInt("snd_rta_debug") != 0) {
                    rtaDebugTimer += dt;
                    if (rtaDebugTimer >= 0.5f) {
                        rtaDebugTimer = 0.0f;
                        x3::logInfo(rtAcoustics.debugString());
                    }
                } else {
                    rtaDebugTimer = 0.0f;
                }
            }
        }

        int cw, ch;
        glfwGetFramebufferSize(window, &cw, &ch);
        if (cw != lastW || ch != lastH) {
            lastW = cw; lastH = ch;
            if (cw > 0 && ch > 0) device->onResize(static_cast<uint32_t>(cw), static_cast<uint32_t>(ch));
        }

        auto frame = device->beginFrame();
        if (frame.valid) {
            // EDITOR (--editor): start the ImGui frame for this render. P0 submits a
            // fullscreen dockspace + the ImGui demo window (proof); the device records
            // the ImGui draws in a pass AFTER the game composite/HUD inside endFrame.
            // No-op without --editor (editorUIActive() stays false). Phase 1 replaces
            // the demo window with the real docked editor panels.
            if (editorMode && device->editorUIActive()) device->beginEditorUI();
            // EDITOR (--editor) Feature 3: draw placed GLB model props into THIS frame's
            // scene pass (the blockout brushes render via scene.render(); models live in
            // the LevelDoc and are drawn here). No-op without --editor / no models placed.
            if (editorMode && device->editorUIActive())
                editorHost.renderModels(*device, frame);
            // GIBS: integrate the GPU-compute debris pool (monster-death chunks +
            // any other bursts) one step. Frozen during a UI menu so chunks hold mid-
            // air with the rest of the sim. No-op cost when the pool is empty.
            device->gpuDebrisStep(simFrozen ? 0.0f : dt);
            // Per-room occlusion cull (canonlevel): the portal flood-fill visible-room set
            // (frustum-directional, computed once in the lighting block above as
            // canonVisRooms) drives render(). A CLOSED door is opaque + stops the flood; far
            // rooms seen through an OPEN door down a hall you LOOK at are kept (no pop),
            // capped by r_culldepth hops + a room budget so it never draws the whole tower.
            // `r_roomcull 0` hard-disables the cull (noclip overview); `1` (default) = on.
            if (canonWorld && canonFloor.valid()) {
                const bool roomCull = console->getInt("r_roomcull") != 0;
                scene.setRoomCullEnabled(roomCull);
                if (roomCull) scene.setVisibleRooms(canonVisRooms);   // same set as the lights
            }
            scene.render(*device, frame);
            if (canonWorld) canonDoors.drawMeshes(*device, frame);   // SM_Door_A doors (canonlevel)
            // THIRD-PERSON: draw the animated Jake avatar at the player's position +
            // the equipped weapon socketed to its right hand. Both are no-ops in FP /
            // when Jake didn't load (avatarVisible() gates them). The avatar was posed
            // + the hand-socket pose computed in thirdPerson.update() above.
            if (!terrainWorld && thirdPerson.avatarVisible()) {
                thirdPerson.drawAvatar(*device, frame, scene);
                const bool heldArmed = game.armed() || (canonWorld && canonPlay.armed());
                thirdPerson.drawHeldWeapon(*device, frame, scene, arsenal, heldArmed);
            }
            // --world canonlevel gameplay: the sidearm pickup + animated enemies + Martinez
            // + the rescue girls, ROOM-GATED (only the visible rooms' characters are drawn/
            // skinned, so the cull's perf payoff is preserved with the characters in).
            if (canonWorld && canonPlay.built()) canonPlay.draw(*device, frame, scene);
            // Level 1 world extras: the bobbing armory pickup + all enemy models
            // (corridor guards/drone, checkpoint guards, Martinez) with hit-flash.
            // Skipped in the outdoor terrain world (no Level 1 controller built).
            if (!terrainWorld) {
                // Real SM_Door_A door slabs at each door's current (sliding) pose —
                // the procedural door box is collision-only (hidden).
                game.drawDoors(*device, frame);
                game.drawWorldExtras(*device, frame, scene);
                game.secret().drawExtras(*device, frame, scene);   // secret-room weapon pickup (bob/spin)
                midFloors.drawDoors(*device, frame);          // F3/F4/F5 keypad door slabs
                midFloors.draw(*device, frame, scene);        // F3/F4/F5 enemies + F5 victim
                topFloors.drawDoors(*device, frame);          // F6/F7 keypad door slabs
                topFloors.draw(*device, frame, scene);        // F6/F7 enemies + Clone boss + Sarah
                nexus.draw(*device, frame, scene);            // Floor 4.5 Chorus pods
                subLevels.drawDoors(*device, frame);          // hidden sub-level door slabs (no-op while closed)
                subLevels.draw(*device, frame, scene);        // sub-level enemies + Frozen Collective + Dr. Chen (no-op while closed)
                // ---- Monster HEALTH BARS — shiny metallic, world-anchored, with a
                // sweeping specular sheen (shimmer). LOS-culled so a bar NEVER shows
                // through a wall. Above every living enemy; flares white on a fresh hit
                // and warms toward red as HP drops (length still reads the exact value). --
                {
                    const double barT = glfwGetTime();
                    const x3::phys::Vec3 hbEye{ camX, camY, camZ };
                    auto hpBar = [&](const x3::phys::Vec3& head, int hpv, int mx, float flash) {
                        if (mx <= 0 || hpv <= 0) return;   // living enemies only
                        // Only show a bar for NEARBY enemies — fades out by ~18 m, gone by 22 m
                        // (no bars from across the room / 50 ft away).
                        const float hdx=head.x-hbEye.x, hdy=head.y-hbEye.y, hdz=head.z-hbEye.z;
                        if (hdx*hdx+hdy*hdy+hdz*hdz > 22.0f*22.0f) return;   // >22 m: no bar
                        float sx = 0.0f, sy = 0.0f;
                        if (!device->worldToScreen(head.x, head.y, head.z, sx, sy)) return;  // behind camera
                        const float frac = (hpv >= mx) ? 1.0f : (float)hpv / (float)mx;
                        uint32_t hw=0, hh=0; device->hudSize(hw, hh);
                        const float bw = 40.0f, bh = 3.0f, x0 = sx - bw * 0.5f;   // thin line (was 64x7)
                        float y0 = sy; if (y0 < 14.0f) y0 = 14.0f;            // clamp on-screen (close enemies)
                        if (hh > 30 && y0 > (float)hh - 30.0f) y0 = (float)hh - 30.0f;
                        const float lowH = 1.0f - frac;                       // 0 healthy -> 1 dying
                        // Per-bar phase from world X so bars don't pulse/shimmer in lockstep.
                        const float ph    = head.x * 0.7f;
                        const float pulse = 0.86f + 0.14f * (float)std::sin(barT * 3.2 + ph);
                        const float outl[4]   = { 0.00f, 0.00f, 0.00f, 0.65f };                       // black definition outline
                        const float frameC[4] = { 0.78f*pulse, 0.86f*pulse, 1.00f*pulse, 0.95f };      // breathing steel frame
                        const float backC[4]  = { 0.04f, 0.05f, 0.08f, 0.85f };                        // dark inset bg
                        // Metallic fill: darker base + lighter top band fakes a vertical
                        // gradient; warms toward red at low HP; flares white on a hit.
                        const float baseC[4]  = { 0.52f + 0.30f*lowH + 0.18f*flash, 0.55f - 0.20f*lowH, 0.62f - 0.30f*lowH, 1.0f };
                        const float topC[4]   = { 0.90f + 0.10f*flash,              0.92f - 0.30f*lowH, 0.98f - 0.45f*lowH, 1.0f };
                        const float fillW = bw * frac;
                        device->drawHudQuad(frame, x0 - 2.0f, y0 - 2.0f, bw + 4.0f, bh + 4.0f, outl);
                        device->drawHudQuad(frame, x0 - 1.5f, y0 - 1.5f, bw + 3.0f, bh + 3.0f, frameC);
                        device->drawHudQuad(frame, x0, y0, bw, bh, backC);
                        device->drawHudQuad(frame, x0, y0, fillW, bh, baseC);            // body
                        device->drawHudQuad(frame, x0, y0, fillW, bh * 0.45f, topC);     // top sheen band
                        // Sweeping specular sliver = the "shimmer", looping across the fill.
                        if (fillW > 6.0f) {
                            const float sw = 7.0f;
                            const float swp = (float)std::fmod(barT * 0.55 + head.x * 0.05, 1.0);
                            float sxx = x0 + swp * fillW - sw * 0.5f;
                            if (sxx < x0)              sxx = x0;
                            if (sxx > x0 + fillW - sw) sxx = x0 + fillW - sw;
                            const float sheen[4] = { 1.0f, 1.0f, 1.0f, 0.40f };
                            device->drawHudQuad(frame, sxx, y0, sw, bh, sheen);
                        }
                    };
                    auto barsFor = [&](x3::game::MonsterManager& mm) {
                        for (uint32_t i = 0; i < mm.count(); ++i) {
                            x3::game::MonsterSystem& m = mm.at(i);
                            if (!m.alive()) continue;
                            x3::phys::Vec3 c = m.pos();
                            // LOS cull: skip the bar if a static wall sits between the
                            // camera and the enemy's chest (no more bars through walls).
                            const x3::phys::Vec3 chest{ c.x, c.y + 1.0f, c.z };
                            const x3::phys::Vec3 d{ chest.x - hbEye.x, chest.y - hbEye.y, chest.z - hbEye.z };
                            const float dist = std::sqrt(d.x*d.x + d.y*d.y + d.z*d.z);
                            if (dist > 0.001f) {
                                const x3::phys::Vec3 nd{ d.x/dist, d.y/dist, d.z/dist };
                                const x3::phys::RayHit los = physics->rayCast(hbEye, nd, dist - 0.3f, x3::phys::Layer::Static);
                                if (los.hit) continue;   // wall in the way -> hidden
                            }
                            c.y += 2.2f;                 // anchor above the head
                            hpBar(c, m.hp(), m.maxHp(), m.hitFlash());
                        }
                    };
                    // B1 groups + the active Spire-floor enemy groups + bosses.
                    const double _pbar0 = glfwGetTime();
                    barsFor(game.corridorEnemies());
                    barsFor(game.checkpointEnemies());
                    g_perf.healthbars += glfwGetTime() - _pbar0;
                    for (uint32_t f = 0; f < (uint32_t)x3::game::SpireMidFloor::Count; ++f)
                        barsFor(midFloors.enemies((x3::game::SpireMidFloor)f));
                    barsFor(midFloors.f3Boss());
                    barsFor(midFloors.swarmBoss());
                    for (uint32_t f = 0; f < (uint32_t)x3::game::SpireTopFloor::Count; ++f)
                        barsFor(topFloors.enemies((x3::game::SpireTopFloor)f));
                    barsFor(topFloors.overseerBoss());
                    barsFor(topFloors.boss());
                }
                const VmPose vmPose = readViewmodelPose(*console);
                const bool vmArmed = game.armed() || (canonWorld && canonPlay.armed());
                // THIRD-PERSON: hide the FP weapon viewmodel ENTIRELY (the gun is shown
                // in the avatar's hand instead — drawn after scene.render below).
                // viewmodelVisible() is true in FP / unbuilt, so FP behaviour is unchanged.
                if (!thirdPerson.viewmodelVisible()) {
                    // 3P: no FP viewmodel this frame.
                } else if (arsenal.viewmodelsLoaded() && vmArmed) {
                    // WEAPONS: draw the SELECTED weapon's viewmodel (its own GLB +
                    // convention-correct base offsets). The live vm_* cvars are passed
                    // as DELTAS from the baked default so console tuning still nudges
                    // whatever weapon is held (delta 0 at defaults -> per-weapon pose).
                    arsenal.drawCurrentViewmodel(*device, frame, camX, camY, camZ, camYaw, camPitch,
                        vmPose.yawRad   - x3::game::kVmDefYawDeg   * kDegToRad,
                        vmPose.pitchRad - x3::game::kVmDefPitchDeg * kDegToRad,
                        vmPose.rollRad  - x3::game::kVmDefRollDeg  * kDegToRad,
                        vmPose.fwd   - x3::game::kVmDefFwd,
                        vmPose.right - x3::game::kVmDefRight,
                        vmPose.down  - x3::game::kVmDefDown);
                } else if (canonWorld && canonPlay.built()) {
                    // Fallback in canonlevel: the canon sidearm's pickup viewmodel.
                    canonPlay.drawViewmodel(*device, frame, camX, camY, camZ, camYaw, camPitch,
                                            vmPose.yawRad, vmPose.pitchRad, vmPose.rollRad,
                                            vmPose.fwd, vmPose.right, vmPose.down);
                } else {
                    // Fallback: arsenal viewmodels didn't load -> the original pickup
                    // viewmodel (unchanged behavior).
                    game.drawViewmodel(*device, frame, camX, camY, camZ, camYaw, camPitch,
                                       vmPose.yawRad, vmPose.pitchRad, vmPose.rollRad,
                                       vmPose.fwd, vmPose.right, vmPose.down);
                }
            }
            // FX: active tracers + muzzle flash (world-space).
            // GIBS: draw the live GPU debris pool (one instanced cube draw; dead
            // slots collapse in the shader). Dark fleshy-red tint so gib chunks read
            // as gore. No-op when the pool is empty (zero cost until something dies).
            {
                const float gibTint[4] = { 0.42f, 0.06f, 0.05f, 1.0f };
                device->gpuDebrisDraw(frame, gibTint);
            }
            combatFx.draw(*device, frame, camX, camY, camZ, camYaw, camPitch);
            // GPU-instanced particles (sparks/blood/smoke/debris) + impact decals:
            // submit the live pool for this frame (HDR pass, soft against depth,
            // bright additive sparks feed bloom). No-op when the pool is empty.
            combatFx.submit(*device, frame);
            // ===========================================================
            // 2D OVERLAY: the GENERAL game-UI layer + EFLZ-specific extras +
            // the dev console. Order: production HUD / menus first (so the
            // EFLZ banners + console draw ON TOP), then the always-on FPS /
            // perf stats, then the console panel last.
            // ===========================================================
            const bool playingNow = gameUi.playing();

            // EFLZ-specific HUD extras that the GENERAL GameHud doesn't own. These
            // draw only while actively playing (not in any menu / console).
            if (playingNow && !consoleOpen) {
                // Door-code keypad prompt: centered, while code entry is active.
                if (codeMode && !terrainWorld) {
                    uint32_t hudW = 0, hudH = 0; device->hudSize(hudW, hudH);
                    const std::string kpPrompt = keypad.prompt();
                    const float kpCol[4] = { 1.0f, 0.82f, 0.18f, 1.0f };
                    device->drawHudText(frame, kpPrompt.c_str(),
                                        (float)hudW * 0.5f - 230.0f, (float)hudH * 0.5f - 60.0f, 3.0f, kpCol);
                }
                // CELL HOLO-TERMINAL readout: LARGE high-contrast on-glass text drawn
                // over the projected hologram panel (worldToScreen anchor). The boot
                // readout shows WHENEVER the panel is built + visible + within reach-ish
                // range (so the hologram is never a blank slab); the editable input line
                // + blinking cursor appear once the player is in termMode (pressed E).
                // The text SIZE is derived from the panel's on-screen height so it scales
                // to the glass at any distance — clearly readable from a few meters.
                if (!terrainWorld && game.secret().terminal().built() &&
                    !game.secret().terminal().textOnGlass()) {
                    // FALLBACK only: the readout normally lives ON the glass (baked into
                    // the hologram texture so it tilts with the panel). This 2D overlay
                    // runs solely if the on-glass font bake failed.
                    const auto& term = game.secret().terminal();
                    const x3::phys::Vec3 a = term.anchor();
                    // Player eye for the range/visibility gate.
                    float pex, pey, pez, pyaw, ppitch;
                    player.camera(pex, pey, pez, pyaw, ppitch);
                    if (noclip) { pex = flyX; pey = flyY; pez = flyZ; }
                    const float tdx = a.x - pex, tdy = a.y - pey, tdz = a.z - pez;
                    const float distM = std::sqrt(tdx*tdx + tdy*tdy + tdz*tdz);
                    if (distM < 14.0f)
                        drawHoloReadout(*device, frame, term, a, termMode);
                }
                // Door interaction prompt: a "[E] Open" / "[E] Close" tag floating at
                // the doorway the player is looking at (within use range), fading in
                // with proximity. Mirrors the health-bar world->screen anchoring.
                if (!terrainWorld && !codeMode) {
                    float pex, pey, pez, pyaw, ppitch;
                    player.camera(pex, pey, pez, pyaw, ppitch);
                    if (noclip) { pex = flyX; pey = flyY; pez = flyZ; pyaw = flyYaw; ppitch = flyPitch; }
                    const x3::phys::Vec3 peye{ pex, pey, pez };
                    const x3::phys::Vec3 pdir{ std::cos(ppitch) * std::cos(pyaw),
                                               std::sin(ppitch),
                                               std::cos(ppitch) * std::sin(pyaw) };
                    x3::phys::Vec3 anchor{}; bool doorOpen = false;
                    if (game.aimedDoorPrompt(peye, pdir, scene, *physics, 3.0f, anchor, doorOpen)) {
                        float sx = 0.0f, sy = 0.0f;
                        if (device->worldToScreen(anchor.x, anchor.y, anchor.z, sx, sy)) {
                            const float pdx = anchor.x - peye.x, pdy = anchor.y - peye.y, pdz = anchor.z - peye.z;
                            const float dd = std::sqrt(pdx * pdx + pdy * pdy + pdz * pdz);
                            float a = 1.0f - (dd - 2.0f);            // 1 @<=2 m -> 0 @3 m
                            if (a > 1.0f) a = 1.0f; if (a < 0.0f) a = 0.0f;
                            a = 0.30f + 0.70f * a;                   // soft floor so it reads at reach edge
                            const char* label = doorOpen ? "[E] Close" : "[E] Open";
                            const float sz = 18.0f;   // readable prompt (was 2.4 = microscopic)
                            const float tx = sx - 46.0f, ty = sy;
                            const float shadow[4] = { 0.0f, 0.0f, 0.0f, 0.70f * a };
                            const float col[4]    = { 0.66f, 0.92f, 1.0f, a };   // cyan-white
                            device->drawHudText(frame, label, tx + 1.5f, ty + 1.5f, sz, shadow);
                            device->drawHudText(frame, label, tx, ty, sz, col);
                        }
                    }
                }
                // Elevator + cell-terminal prompts (so the player KNOWS they're in range
                // and which key — same world->screen anchoring as the door prompt). ----
                if (!terrainWorld && !codeMode && !termMode) {
                    float pex, pey, pez, pyaw, ppitch;
                    player.camera(pex, pey, pez, pyaw, ppitch);
                    if (noclip) { pex = flyX; pey = flyY; pez = flyZ; }
                    auto floatPrompt = [&](const x3::phys::Vec3& at, const char* label, float xoff) {
                        float sx = 0.0f, sy = 0.0f;
                        if (!device->worldToScreen(at.x, at.y, at.z, sx, sy)) return;
                        const float col[4]    = { 0.66f, 0.92f, 1.0f, 0.95f };
                        const float shadow[4] = { 0.0f, 0.0f, 0.0f, 0.70f };
                        device->drawHudText(frame, label, sx - xoff + 1.5f, sy + 1.5f, 18.0f, shadow);
                        device->drawHudText(frame, label, sx - xoff, sy, 18.0f, col);
                    };
                    // Elevator: within ~4 m of the cab.
                    if (elevator.built()) {
                        const x3::phys::Vec3 cc = elevator.cabCenter();
                        const float ex = pex - cc.x, ez = pez - cc.z;
                        if (ex*ex + ez*ez < 16.0f)
                            floatPrompt(x3::phys::Vec3{ cc.x, cc.y + 1.6f, cc.z }, "[E] Call Elevator", 84.0f);
                    }
                    // Cell HoloTerminal: within ~3 m of its anchor.
                    if (game.secret().terminal().built()) {
                        const x3::phys::Vec3 a = game.secret().terminal().anchor();
                        const float dx = pex - a.x, dz = pez - a.z;
                        if (dx*dx + dz*dz < 9.0f)
                            floatPrompt(x3::phys::Vec3{ a.x, a.y + 0.55f, a.z }, "[E] Use Terminal (code 1278)", 110.0f);
                    }
                }
                // ---- RESCUED-NPC TALK: floating "[E] Talk" prompt + the dialog box.
                // The prompt floats over a nearby LIVE captive's head (worldToScreen,
                // mirroring the door prompt). Once an exchange is open the prompt gives
                // way to a centered dialog box (speaker + line, large Menu-role font),
                // and the captive's warm one-liner bark fades after she joins you. ----
                if (!terrainWorld && !codeMode && !termMode) {
                    float pex, pey, pez, pyaw, ppitch;
                    player.camera(pex, pey, pez, pyaw, ppitch);
                    if (noclip) { pex = flyX; pey = flyY; pez = flyZ; }
                    const x3::phys::Vec3 peye{ pex, pey, pez };
                    uint32_t hudW = 0, hudH = 0; device->hudSize(hudW, hudH);

                    // CHAT-TREE dialog box (Lena) — the data-driven runner's UI: NPC
                    // line on top, up to 4 numbered choices below, GTA-subtitle look.
                    if (chatTrees.active()) {
                        x3::game::drawChatTreeUi(*device, frame, chatTrees);
                    } else {
                        // "[E] Talk" floats over the chat-capable NPC too (captive OR
                        // companion re-talk), same worldToScreen pattern as below.
                        std::string cw; x3::phys::Vec3 cp{}; bool ccap = false;
                        if (chatTalkTarget(peye, x3::game::kTalkReach, cw, cp, ccap)) {
                            float sx = 0.0f, sy = 0.0f;
                            if (device->worldToScreen(cp.x, cp.y + 1.85f, cp.z, sx, sy)) {
                                const float ddx = cp.x - pex, ddz = cp.z - pez;
                                float a = 1.0f - (std::sqrt(ddx*ddx + ddz*ddz) - 2.0f);
                                if (a > 1.0f) a = 1.0f; if (a < 0.0f) a = 0.0f;
                                a = 0.35f + 0.65f * a;
                                const float shadow[4] = { 0.0f, 0.0f, 0.0f, 0.70f * a };
                                const float col[4]    = { 1.0f, 0.72f, 0.84f, a };
                                device->drawHudText(frame, "[E] Talk", sx - 40.0f + 1.5f, sy + 1.5f, 18.0f, shadow);
                                device->drawHudText(frame, "[E] Talk", sx - 40.0f, sy, 18.0f, col);
                            }
                        }
                    }

                    if (npcDialog.active()) {
                        // The exchange box: a translucent panel near the screen bottom
                        // with the speaker label + the current line, large + readable.
                        const auto& ln = npcDialog.currentLine();
                        const std::string speaker = ln.speaker.empty() ? npcDialog.partner() : ln.speaker;
                        const std::string& body = ln.text;
                        const float cx = (hudW > 0) ? hudW * 0.5f : 640.0f;
                        const float boxW = (hudW > 0) ? hudW * 0.66f : 840.0f;
                        const float boxH = 118.0f;
                        const float boxX = cx - boxW * 0.5f;
                        const float boxY = (hudH > 0) ? hudH - 190.0f : 540.0f;
                        const float panel[4]  = { 0.05f, 0.07f, 0.12f, 0.82f };
                        const float border[4] = { 0.40f, 0.78f, 1.0f, 0.85f };   // cyan rim
                        device->drawHudQuad(frame, boxX - 3.0f, boxY - 3.0f, boxW + 6.0f, boxH + 6.0f, border);
                        device->drawHudQuad(frame, boxX, boxY, boxW, boxH, panel);
                        // Speaker name (warm tint for her, cool for the player "YOU").
                        const bool isYou = (speaker == "YOU");
                        const float namePx = 26.0f;
                        const float herCol[4]  = { 1.0f, 0.62f, 0.78f, 1.0f };   // warm rose (her)
                        const float youCol[4]  = { 0.66f, 0.92f, 1.0f, 1.0f };   // cool cyan (player)
                        const float nshadow[4] = { 0.0f, 0.0f, 0.0f, 0.75f };
                        const float nameX = boxX + 24.0f, nameY = boxY + 18.0f;
                        device->drawHudTextF(frame, x3::rhi::FontRole::Menu, (speaker + ":").c_str(),
                                             nameX + 1.5f, nameY + 1.5f, namePx, nshadow);
                        device->drawHudTextF(frame, x3::rhi::FontRole::Menu, (speaker + ":").c_str(),
                                             nameX, nameY, namePx, isYou ? youCol : herCol);
                        // The spoken line, larger, white.
                        const float linePx = 30.0f;
                        const float lineX = boxX + 24.0f, lineY = boxY + 58.0f;
                        const float lshadow[4] = { 0.0f, 0.0f, 0.0f, 0.8f };
                        const float lineCol[4] = { 0.96f, 0.97f, 1.0f, 1.0f };
                        device->drawHudTextF(frame, x3::rhi::FontRole::Menu, body.c_str(),
                                             lineX + 1.5f, lineY + 1.5f, linePx, lshadow);
                        device->drawHudTextF(frame, x3::rhi::FontRole::Menu, body.c_str(),
                                             lineX, lineY, linePx, lineCol);
                        // Advance hint, right-aligned in the box.
                        const char* hint = (npcDialog.lineIndex() + 1 >= npcDialog.lineCount())
                                           ? "[E] Free her" : "[E] Continue";
                        const float hintPx = 18.0f;
                        const float hw = device->textAdvance(x3::rhi::FontRole::Menu, hint, hintPx);
                        const float hintX = boxX + boxW - hw - 22.0f, hintY = boxY + boxH - 28.0f;
                        const float hintCol[4] = { 0.75f, 0.85f, 0.95f, 0.85f };
                        device->drawHudTextF(frame, x3::rhi::FontRole::Menu, hint, hintX, hintY, hintPx, hintCol);
                    } else {
                        // No exchange yet: float "[E] Talk" over the nearest live captive.
                        std::string who; x3::phys::Vec3 cpos{};
                        if (nearestLiveCaptive(peye, x3::game::kTalkReach, who, cpos)) {
                            float sx = 0.0f, sy = 0.0f;
                            if (device->worldToScreen(cpos.x, cpos.y + 1.85f, cpos.z, sx, sy)) {
                                const float dx = cpos.x - peye.x, dz = cpos.z - peye.z;
                                const float dd = std::sqrt(dx * dx + dz * dz);
                                float a = 1.0f - (dd - 2.0f);
                                if (a > 1.0f) a = 1.0f; if (a < 0.0f) a = 0.0f;
                                a = 0.35f + 0.65f * a;
                                const float sz = 18.0f;   // readable prompt (was 2.4 = microscopic)
                                const float tx = sx - 40.0f, ty = sy;
                                const float shadow[4] = { 0.0f, 0.0f, 0.0f, 0.70f * a };
                                const float col[4]    = { 1.0f, 0.72f, 0.84f, a };   // warm rose (a person, not a door)
                                device->drawHudText(frame, "[E] Talk", tx + 1.5f, ty + 1.5f, sz, shadow);
                                device->drawHudText(frame, "[E] Talk", tx, ty, sz, col);
                            }
                        }
                    }

                    // Her companion one-liner bark, just under the crosshair, fading out.
                    if (npcBarkTimer > 0.0f && !npcBarkText.empty()) {
                        float a = npcBarkTimer; if (a > 1.0f) a = 1.0f;   // fade in last second
                        const float barkPx = 22.0f;
                        const float bw = device->textAdvance(x3::rhi::FontRole::Menu, npcBarkText.c_str(), barkPx);
                        const float bx = ((hudW > 0) ? hudW * 0.5f : 640.0f) - bw * 0.5f;
                        const float by = (hudH > 0) ? hudH * 0.62f : 420.0f;
                        const float bshadow[4] = { 0.0f, 0.0f, 0.0f, 0.7f * a };
                        const float bcol[4]    = { 1.0f, 0.72f, 0.84f, a };
                        device->drawHudTextF(frame, x3::rhi::FontRole::Menu, npcBarkText.c_str(),
                                             bx + 1.5f, by + 1.5f, barkPx, bshadow);
                        device->drawHudTextF(frame, x3::rhi::FontRole::Menu, npcBarkText.c_str(),
                                             bx, by, barkPx, bcol);
                    }
                }
                // Strength terminal — the "Awakening" readout (EFLZ_SPIRE §3).
                if (!terrainWorld) {
                    static float awakenTimer = 7.0f;
                    if (awakenTimer > 0.0f) {
                        awakenTimer -= dt;
                        uint32_t hw = 0, hh = 0; device->hudSize(hw, hh);
                        const float tx = (hw > 0) ? hw * 0.5f - 250.0f : 380.0f;
                        const float ty = (hh > 0) ? hh * 0.30f : 200.0f;
                        const float term[4] = { 0.20f, 1.00f, 0.55f, 1.0f };   // terminal green
                        const float warn[4] = { 1.00f, 0.40f, 0.25f, 1.0f };   // failing = red
                        device->drawHudText(frame, "SUBJECT: JAKE    STATUS: AUGMENTED", tx, ty,         2.4f, term);
                        device->drawHudText(frame, "MUSCULOSKELETAL OUTPUT: +400%",      tx, ty + 34.0f, 2.4f, term);
                        device->drawHudText(frame, "RESTRAINT INTEGRITY: FAILING",       tx, ty + 68.0f, 2.4f, warn);
                    }
                }
                // Phase 2b: boss "PHASE 2!/PHASE 3!" flash near the top.
                if (!terrainWorld && game.phaseBannerTime() > 0.0f && !game.phaseBanner().empty()) {
                    uint32_t hw = 0, hh = 0; device->hudSize(hw, hh);
                    const float scale = 28.0f;
                    const float bw = game.phaseBanner().size() * scale * 0.6f;
                    const float px = (hw > 0) ? (hw * 0.5f - bw * 0.5f) : 420.0f;
                    const float py = (hh > 0) ? (hh * 0.22f) : 120.0f;
                    const float red[4] = { 1.0f, 0.25f, 0.2f, 1.0f };
                    device->drawHudText(frame, game.phaseBanner().c_str(), px, py, scale, red);
                }
                // F2 rescue timers (spec §5): stacked, below the objective line.
                if (!terrainWorld) {
                    const auto rows = game.rescue().hudTimers();
                    float ry = 96.0f;
                    for (const auto& row : rows) {
                        const int total = (int)(row.seconds + 0.5f);
                        const int mm = total / 60, ss = total % 60;
                        char buf[64];
                        std::snprintf(buf, sizeof(buf), "RESCUE %s  %d:%02d",
                                      row.name.c_str(), mm, ss);
                        float col[4];
                        if (row.urgent) { col[0]=1.0f; col[1]=0.25f; col[2]=0.20f; col[3]=1.0f; }
                        else            { col[0]=0.55f; col[1]=0.85f; col[2]=1.0f; col[3]=1.0f; }
                        device->drawHudText(frame, buf, 24.0f, ry, 2.0f, col);
                        ry += 28.0f;
                    }
                }
            }

            // ---- GENERAL game-UI: route input + draw the active screen / HUD ----
            // Build the production-HUD model from the live gameplay state, then
            // hand the UI controller this frame's input snapshot. While Playing it
            // draws the HUD (HP / weapon+ammo / objective / crosshair / minimap);
            // otherwise it draws the active menu (main / pause / settings).
            x3::ui::HudModel hm{};
            hm.hp = player.hp(); hm.maxHp = player.maxHp();
            hm.alive = player.isAlive();
            hm.damageFlash = player.damageFlash();
            hm.showCrosshair = !consoleOpen;
            hm.dispW = cw; hm.dispH = ch;   // live framebuffer size -> menu RESOLUTION readout
            if (!terrainWorld) {
                hm.objective = game.objectives().currentLabel().c_str();
                // Live enemy-remaining counter (HUD): ALL live hostile groups (corridor
                // + checkpoint + Phase-3 boss adds + bosses), so it never reads "AREA
                // CLEAR" while a boss add is still alive. -1 (default) hides it elsewhere.
                // --world canonlevel: fold the canon enemies/boss so the counter reflects
                // the canon spawns (not the empty legacy groups).
                hm.enemiesRemaining = game.enemiesRemaining() +
                    ((canonWorld && canonPlay.built()) ? canonPlay.enemiesRemaining() : 0);
                if (canonWorld && canonPlay.built())
                    hm.objective = (canonPlay.enemiesRemaining() > 0)
                        ? "Fight down the spire — save the captives, reach Martinez"
                        : "AREA CLEAR — reach the Elevator Lobby";
                if (game.armed() || (canonWorld && canonPlay.armed())) {
                    const x3::game::WeaponDef&         wd = arsenal.current();
                    const x3::game::Arsenal::WeaponState& ws = arsenal.currentState();
                    hm.weapon = wd.name.c_str();
                    hm.ammoInMag = ws.ammoInMag; hm.ammoReserve = ws.reserve;
                    hm.reloading = arsenal.isReloading();
                }

                // ---- Minimap RADAR + enemy NAMEPLATE feed ----------------------
                // Player pose (radar center + heading). Use the noclip fly pose when
                // free-flying so the radar tracks the camera, else the player camera.
                {
                    float rpx, rpy, rpz, rpyaw, rppitch;
                    player.camera(rpx, rpy, rpz, rpyaw, rppitch);
                    if (noclip) { rpx = flyX; rpy = flyY; rpz = flyZ; rpyaw = flyYaw; }
                    hm.playerX = rpx; hm.playerZ = rpz; hm.playerYaw = rpyaw;
                    hm.radarValid = true;

                    // WORLD MAP: POI proximity discovery (persists via the story
                    // flags) + the waypoint -> minimap chevron feed.
                    if (gameUi.playing() && !worldMapOpen)
                        worldMap.discoveryTick(chatTrees.flags(), rpx, rpy - 1.6f, rpz);
                    if (worldMap.waypoint().active) {
                        hm.wpValid = true;
                        hm.wpX = worldMap.waypoint().x;
                        hm.wpZ = worldMap.waypoint().z;
                    }

                    // Live hostile marks (positions + short threat labels). The labels
                    // are static string literals owned by Level1Game, so storing the
                    // const char* in the (frame-scoped) HudModel is safe.
                    x3::game::Level1Game::EnemyMark marks[x3::ui::HudModel::kMaxBlips];
                    uint32_t ne = game.liveEnemyMarks(marks, x3::ui::HudModel::kMaxBlips);
                    // --world canonlevel: the canon enemies (Level1Game's are empty here).
                    if (canonWorld && canonPlay.built()) {
                        x3::game::CanonPlay::EnemyMark cm[x3::ui::HudModel::kMaxBlips];
                        const uint32_t nc = canonPlay.liveEnemyMarks(cm, x3::ui::HudModel::kMaxBlips);
                        ne = 0;
                        for (uint32_t i = 0; i < nc && ne < x3::ui::HudModel::kMaxBlips; ++i) {
                            marks[ne].pos = cm[i].pos; marks[ne].label = cm[i].label; ++ne;
                        }
                    }
                    hm.enemyCount = (int)ne;
                    for (uint32_t i = 0; i < ne; ++i) {
                        hm.enemyX[i] = marks[i].pos.x;
                        hm.enemyY[i] = marks[i].pos.y;
                        hm.enemyZ[i] = marks[i].pos.z;
                        hm.enemyLabel[i] = marks[i].label;
                        // Line-of-sight for the NAMEPLATE: ray from the eye to the enemy's
                        // head; if static geometry (wall/door) blocks it first, hide the
                        // label. The minimap blip ignores this (radar sees through walls).
                        const x3::phys::Vec3 eye{ camX, camY, camZ };
                        const float hx = marks[i].pos.x - eye.x;
                        const float hy = (marks[i].pos.y + 1.4f) - eye.y;
                        const float hz = marks[i].pos.z - eye.z;
                        const float dist = std::sqrt(hx*hx + hy*hy + hz*hz);
                        bool vis = true;
                        if (dist > 0.01f) {
                            const x3::phys::Vec3 dir{ hx/dist, hy/dist, hz/dist };
                            x3::phys::RayHit rh = physics->rayCast(eye, dir, dist - 0.5f,
                                                                   x3::phys::Layer::Static);
                            if (rh.hit) vis = false;   // a wall/door is between the eye and this enemy
                        }
                        hm.enemyVisible[i] = vis;
                    }

                    // Live companion (rescued-victim) positions -> green pulsing blips.
                    x3::phys::Vec3 allies[x3::ui::HudModel::kMaxBlips];
                    uint32_t na = game.liveCompanionPositions(allies, x3::ui::HudModel::kMaxBlips);
                    if (canonWorld && canonPlay.built())
                        na = canonPlay.liveCompanionPositions(allies, x3::ui::HudModel::kMaxBlips);
                    hm.allyCount = (int)na;
                    for (uint32_t i = 0; i < na; ++i) {
                        hm.allyX[i] = allies[i].x;
                        hm.allyZ[i] = allies[i].z;
                    }

                    // Secret TRAPDOOR: gold radar marker while the cell floor hatch
                    // exists (roomCenter() shares the hatch XZ; the room is straight
                    // below). Lets the player find the otherwise-hidden hatch.
                    if (game.secret().hatchBuilt()) {
                        hm.trapValid = true;
                        hm.trapX = game.secret().roomCenter().x;
                        hm.trapZ = game.secret().roomCenter().z;
                    }

                    // Faint room outlines: the B1 combat-zone rects (cell / corridor /
                    // armory / checkpoint / arena) from the authored layout. XZ center
                    // + half-extents; the HUD transforms them player-relative.
                    const x3::game::Level1Layout& lay = game.layout();
                    auto addRoom = [&](const x3::phys::Vec3& c, const x3::phys::Vec3& hf) {
                        if (hm.roomCount >= x3::ui::HudModel::kMaxRooms) return;
                        const int r = hm.roomCount++;
                        hm.roomCx[r] = c.x; hm.roomCz[r] = c.z;
                        hm.roomHx[r] = hf.x; hm.roomHz[r] = hf.z;
                    };
                    addRoom(lay.cellCenter,       lay.cellHalf);
                    addRoom(lay.corridorCenter,   lay.corridorHalf);
                    addRoom(lay.armoryCenter,     lay.armoryHalf);
                    addRoom(lay.checkpointCenter, lay.checkpointHalf);
                    addRoom(lay.arenaCenter,      lay.arenaHalf);
                }
            }
            // Compose the UI input snapshot. Mouse position in framebuffer pixels;
            // nav edges from the same rising-edge tracking the menus need. Menu keys
            // are read directly (NOT gated by keyDown's menu-suppression, since the
            // menu IS the active surface). Suppressed while the console is open.
            x3::ui::UiInput uin{};
            if (!consoleOpen) {
                double cmx = 0.0, cmy = 0.0; glfwGetCursorPos(window, &cmx, &cmy);
                uin.mouseX = (float)cmx; uin.mouseY = (float)cmy;
                const bool lmbNow = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
                uin.mouseDown = lmbNow;
                uin.mousePressed = lmbNow && !prevUiMouse;
                prevUiMouse = lmbNow;
                // Keyboard nav (rising edges). Up/Down/W/S move, Enter/Space activate,
                // Left/Right/A/D adjust toggles, Esc backs out / pauses.
                auto rawDown = [&](int k){ return glfwGetKey(window, k) == GLFW_PRESS; };
                const bool nUp  = rawDown(GLFW_KEY_UP)    || rawDown(GLFW_KEY_W);
                const bool nDn  = rawDown(GLFW_KEY_DOWN)  || rawDown(GLFW_KEY_S);
                const bool nAct = rawDown(GLFW_KEY_ENTER) || rawDown(GLFW_KEY_KP_ENTER) || rawDown(GLFW_KEY_SPACE);
                const bool nL   = rawDown(GLFW_KEY_LEFT)  || rawDown(GLFW_KEY_A);
                const bool nR   = rawDown(GLFW_KEY_RIGHT) || rawDown(GLFW_KEY_D);
                // Only deliver nav edges while a menu is active (so they don't fight
                // gameplay WASD). Edge-detect against the previous frame.
                if (uiMenuActive) {
                    uin.navUp       = nUp  && !prevNavUp;
                    uin.navDown     = nDn  && !prevNavDown;
                    uin.navActivate = nAct && !prevNavAct;
                    uin.navLeft     = nL   && !prevNavLeft;
                    uin.navRight    = nR   && !prevNavRight;
                }
                prevNavUp = nUp; prevNavDown = nDn; prevNavAct = nAct;
                prevNavLeft = nL; prevNavRight = nR;
                // Esc edge (computed above) toggles pause / backs out of settings.
                uin.navBack = uiEscEdge;
            } else {
                // Console open: keep edge trackers fresh so opening/closing the
                // console doesn't inject a stale nav edge.
                prevUiMouse = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
            }
            gameUi.update(uin, *device, frame, hm, dt);

            // ---- WORLD MAP SCREEN (M). Drawn over the HUD; the sim is frozen
            // while open (simFrozen above). Click = waypoint / POI travel;
            // wheel = cursor-anchored zoom; drag/WASD/arrows = pan; F1..F7 floors. ----
            if (worldMapOpen) {
                mapUi.begin(*device, frame, uin);
                x3::game::WorldMapSystem::ScreenInput msi{};
                msi.mouseX = uin.mouseX;   msi.mouseY = uin.mouseY;
                msi.mouseDown = uin.mouseDown; msi.mousePressed = uin.mousePressed;
                msi.wheel = mapWheel;
                msi.keyW = rawKey(GLFW_KEY_W) || rawKey(GLFW_KEY_UP);
                msi.keyS = rawKey(GLFW_KEY_S) || rawKey(GLFW_KEY_DOWN);
                msi.keyA = rawKey(GLFW_KEY_A) || rawKey(GLFW_KEY_LEFT);
                msi.keyD = rawKey(GLFW_KEY_D) || rawKey(GLFW_KEY_RIGHT);
                const bool entNow = rawKey(GLFW_KEY_ENTER) || rawKey(GLFW_KEY_KP_ENTER);
                msi.enterEdge = entNow && !prevMapEnter;
                prevMapEnter = entNow;
                msi.escEdge = mapEscEdge;
                float mpx, mpy, mpz, mpyaw, mppitch;
                player.camera(mpx, mpy, mpz, mpyaw, mppitch);
                if (noclip) { mpx = flyX; mpy = flyY; mpz = flyZ; mpyaw = flyYaw; }
                msi.playerX = mpx; msi.playerY = mpy - 1.6f; msi.playerZ = mpz;
                msi.playerYaw = mpyaw;
                msi.compCount = hm.allyCount; msi.compX = hm.allyX; msi.compZ = hm.allyZ;
                msi.objValid = hm.trapValid; msi.objX = hm.trapX; msi.objZ = hm.trapZ;
                msi.missionBlocksTravel = missionDocActive &&
                                          missionRunner.currentStageNoFastTravel();
                msi.locationName = "THE SPIRE - DETENTION LEVEL";
                worldMap.drawScreen(mapUi, *device, frame, msi, chatTrees.flags(), dt);
                mapUi.end();

                // FAST TRAVEL: teleport + blackout cover. This world is the fully-
                // resident Level-1 build (no WorldStreamer on this path; the
                // streamed world routes the same request through wsm.update, whose
                // proxy fallback covers the realize window — see --world streamed).
                if (worldMap.travelRequested()) {
                    if (const x3::game::MapPoi* tgt = worldMap.travelTarget()) {
                        player.setFeetPosition(*physics,
                            x3::phys::Vec3{ tgt->x, tgt->y + 0.3f, tgt->z });
                        travelFadeT = 0.9f;   // fade-to-black cover (blackout pattern)
                        worldMapOpen = false; worldMap.close();
                        x3::logInfo("[worldmap] FAST TRAVEL -> " + tgt->name);
                    }
                    worldMap.clearTravelRequest();
                }
            } else {
                prevMapEnter = rawKey(GLFW_KEY_ENTER) || rawKey(GLFW_KEY_KP_ENTER);
            }
            // Fast-travel BLACKOUT cover: hold black just after the teleport, then
            // fade out (same visual language as the fast-boot blackout).
            if (travelFadeT > 0.0f) {
                travelFadeT -= dt; if (travelFadeT < 0.0f) travelFadeT = 0.0f;
                const float a = std::min(1.0f, travelFadeT / 0.45f);
                int fbw = 0, fbh = 0; glfwGetFramebufferSize(window, &fbw, &fbh);
                const float blk[4] = { 0.0f, 0.0f, 0.0f, a };
                device->drawHudQuad(frame, 0.0f, 0.0f, (float)fbw, (float)fbh, blk);
            }

            // ---- Audio settings -> live audio system. Push every frame from the
            // SettingsModel; the setters are cheap and idempotent (setMusicEnabled
            // early-returns when unchanged, so toggling Music stops/starts the bed
            // exactly once, and the volume sliders quiet music/SFX immediately). ----
            {
                const x3::ui::SettingsModel& asm_ = gameUi.settings();
                audio->setMasterSfxVolume(asm_.sfxVol);
                audio->setMusicVolume(asm_.musicVol);
                audio->setMusicEnabled(asm_.musicOn);
            }

            // Main-menu "SET AS DEFAULT" -> persist the current framebuffer size +
            // the live audio settings (one cfg file).
            if (gameUi.wantSaveDefaults()) {
                gameUi.clearSaveDefaults();
                const x3::ui::SettingsModel& s = gameUi.settings();
                writeSettings((uint32_t)cw, (uint32_t)ch, s.musicOn, s.musicVol, s.sfxVol);
                x3::logInfo("[settings] saved defaults: resolution " +
                            std::to_string(cw) + "x" + std::to_string(ch) +
                            ", musicOn=" + (s.musicOn ? "1" : "0"));
            }

            // ---- Save/Load: pause-menu SAVE/LOAD buttons (polled from the UI) +
            // F5 quick-save / F9 quick-load (when not typing in the console). The
            // pause-menu buttons request via gameUi.wantSave()/wantLoad(); F5/F9 are
            // edge-detected here. All routes funnel through doSave/doLoad. ----
            if (gameUi.wantSave()) { doSave(); gameUi.clearSaveLoadRequest(); }
            else if (gameUi.wantLoad()) { doLoad(); gameUi.clearSaveLoadRequest(); }
            if (!consoleOpen) {
                const bool f5Now = glfwGetKey(window, GLFW_KEY_F5) == GLFW_PRESS;
                const bool f9Now = glfwGetKey(window, GLFW_KEY_F9) == GLFW_PRESS;
                if (f5Now && !prevSaveKey) doSave();
                if (f9Now && !prevLoadKey) doLoad();
                prevSaveKey = f5Now; prevLoadKey = f9Now;
            }

            // HELD-WEAPON GRIP LIVE-TUNE readout (TASK#53): in 3P, print the EFFECTIVE
            // grip (baked table row + live grip_* override) for the CURRENTLY-held
            // weapon, so Tim can dial grip_x/y/z + grip_pitch/yaw/roll + grip_scale by
            // eye and read the absolute numbers to BAKE into kTpGripTable. No-op in FP /
            // unbuilt. The "*OV" tag shows when an override is active (non-baked).
            if (!terrainWorld && thirdPerson.thirdPerson() && thirdPerson.built()) {
                const std::string& gw = arsenal.current().name;
                float gf, gr, gd, gyaw, gpit, grol, gsc;
                thirdPerson.effectiveGrip(gw, gf, gr, gd, gyaw, gpit, grol, gsc);
                char gripLine[224];
                std::snprintf(gripLine, sizeof(gripLine),
                    "GRIP[%s]%s  x %.3f  y %.3f  z %.3f  pitch %.1f  yaw %.1f  roll %.1f  scale %.3f",
                    gw.c_str(), thirdPerson.gripOverrideActive() ? " *OV" : "",
                    gr, gd, gf, gpit, gyaw, grol, gsc);
                const float gx = 18.0f, gy = 96.0f, gpx = 16.0f;
                const float gsh[4] = { 0.0f, 0.0f, 0.0f, 0.80f };
                const float gcl[4] = { 0.72f, 1.0f, 0.25f, 1.0f };   // lime so it reads over the scene
                device->drawHudText(frame, gripLine, gx + 1.5f, gy + 1.5f, gpx, gsh);
                device->drawHudText(frame, gripLine, gx, gy, gpx, gcl);
            }

            // Always-on overlays (independent of game state): FPS meter, the perf
            // stats panel, and the dev console panel (drawn last so it sits on top).
            hud.drawFps(*device, frame, *console, dt);
            hud.drawStats(*device, frame, *console, dt);
            // ZERO-STUTTER telemetry line (r_frametelemetry 1): live frame-pacing
            // percentiles + spike count + the late-creation audit counters.
            if (console->getInt("r_frametelemetry") != 0) {
                const x3::rhi::IRenderDevice::FramePacing fp = device->framePacing();
                char tl[224];
                std::snprintf(tl, sizeof(tl),
                    "pace cpu p50 %.2f p95 %.2f p99 %.2f p999 %.2f max %.2f ms | gpu p99 %.2f | spikes %u | late pso %u mod %u pool %u",
                    fp.cpuP50, fp.cpuP95, fp.cpuP99, fp.cpuP999, fp.cpuMax,
                    fp.gpuP99, fp.spikes, fp.psoLate, fp.modulesLate, fp.poolsLate);
                const float tsh[4] = { 0.0f, 0.0f, 0.0f, 0.80f };
                const float tcl[4] = { 0.45f, 0.95f, 1.0f, 1.0f };   // cyan: telemetry
                device->drawHudText(frame, tl, 9.5f, 76.5f, 14.0f, tsh);
                device->drawHudText(frame, tl, 8.0f, 75.0f, 14.0f, tcl);
            }
            // LIVING WORLD: the facility alert indicator (pips + level name,
            // pulsing red frame in lockdown). Draws nothing at level 0.
            alertHudClock += dt;
            if (facilityAlertOn)
                hud.drawAlert(*device, frame, facilityAlert.level(),
                              facilityAlert.redShift(), alertHudClock);
            hud.drawConsole(*device, frame, *console, dt);
            // EDITOR (--editor): the HOST submits the editor panels (menu bar /
            // Outliner / Blockout / Status) between begin and end, then endEditorUI
            // finalizes the ImGui frame (ImGui::Render + stash draw data) AFTER the
            // game HUD so endFrame's editor-UI pass draws it over the composited
            // scene+HUD. No-op without --editor.
            if (editorMode && device->editorUIActive()) {
                editorHost.draw(*device, scene, *physics, dt);
                device->endEditorUI();
            }
        }
        // BOOT hand-off overlay: the completed loading bar fades out OVER the live
        // game (menu already interactive beneath it) — see the hand-off note above.
        if (loadingOverlayLive) {
            loading.render(*device, frame, dt);
            if (loading.faded()) { loading.shutdown(*device); loadingOverlayLive = false; }
        }
        device->endFrame(frame);
        g_perf.addFrame((double)dt);   // per-system perf breakdown logged every 120 frames

        // ---- [boot] BOOT-TO-INTERACTIVE: the first main-loop frame has presented —
        // window up, world fully built, menu live (player controllable on START).
        // Print the phase table once on every interactive boot; under --test-boottime
        // additionally assert the budget (boot_budget_ms cvar / CLI arg) and exit.
        if (!bootReported) {
            bootReported = true;
            x3::boot::mark("first interactive frame");
            const double totalMs = x3::boot::report("boot-to-interactive");
            if (testBootTime) {
                const double budget = bootBudgetMs > 0.0
                    ? bootBudgetMs : (double)console->getFloat("boot_budget_ms");
                const bool ok = totalMs < budget;
                char vb[160];
                std::snprintf(vb, sizeof(vb),
                    "boottime: %s  boot-to-interactive %.1f ms  (budget %.0f ms, %s)",
                    ok ? "PASS" : "FAIL", totalMs, budget,
                    canonWorld ? "canonlevel" : worldMode.c_str());
                if (ok) x3::logInfo(vb); else x3::logError(vb);
                bootTestExit = ok ? 0 : 1;
                glfwSetWindowShouldClose(window, GLFW_TRUE);
            }
        }
    }

    x3::logInfo("shutting down");
    // BOOT hand-off overlay: if the window closed mid-fade, free its texture now
    // (else the VMA shutdown leak check would see a live allocation).
    if (loadingOverlayLive) loading.shutdown(*device);
    // B3: tear the streamer down BEFORE physics/device (it removes its bodies +
    // destroys its meshes), then stop the terrain job system. Both are no-ops
    // when not in terrain mode.
    if (terrainWorld) {
        terrainStreamer.shutdown(scene, *device, *physics);
        if (terrainJobs) terrainJobs->shutdown();
    }
    audio->shutdown();
    combatFx.shutdown(*device);
    // TASK#12: tear down any in-flight SKINNED death ragdolls (Jolt bodies) BEFORE
    // physics shuts down, so a monster killed in the last ~0.7 s (mid-flop) doesn't
    // touch a dead Jolt system when its IRagdoll is later destroyed. game.shutdown()
    // fans across EVERY Level1Game enemy group AND the single Martinez boss + Phase-3
    // adds (the bare group calls missed Martinez/bossAdds -> exit crash after a boss
    // kill); a no-op when nothing is ragdolling.
    shutdownGameSystems();   // every enemy group + Martinez + barrels + Nexus/canon ragdolls
    docLevel.shutdown(scene, *device, *physics);   // --world fromdoc doc objects + caches
    worldMap.shutdown(*device);                    // baked map-tile textures
    physics->shutdown();
    device->shutdown();
    glfwDestroyWindow(window);
    glfwTerminate();
    return bootTestExit;
}
