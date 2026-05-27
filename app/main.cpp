// X3Engine host — opens a window, brings up the render device + physics, builds
// the S2 graybox test level, and runs the loop with a fly camera. Walking is S3.

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#include "engine/core/x3_log.h"
#include "engine/core/IConsole.h"
#include "engine/core/IJobSystem.h"
#include "engine/rhi/IRenderDevice.h"
#include "engine/asset/IAssetSource.h"
#include "engine/physics/IPhysicsWorld.h"
#include "engine/physics/Destruction.h"   // K-T0/T1 destructibles + --test-destruction
#include "engine/physics/StructuralCollapse.h" // K-T3 support-graph collapse + --test-collapse
#include "engine/physics/Ragdoll.h"       // Physics §2 ragdoll+blend: --test-ragdoll + --world ragdoll
#include "engine/physics/IVehicle.h"      // vehicle framework: --test-vehicle + --world drive/boat/fly
#include "engine/asset/IModelLoader.h"
#include "engine/audio/IAudioSystem.h"
#include "engine/net/INetworkSystem.h"   // netcode Phase 0: --test-net + SimClock
#include "engine/net/SimClock.h"         // deterministic fixed-step accumulator
#include "engine/net/ISnapshotInterpolator.h"  // netcode Phase 0c: --test-netinterp
#include "engine/net/IClientPredictor.h"        // netcode Phase 1: --test-netpredict
#include "engine/ai/INavigation.h"       // GENERAL navigation: nav grid + A* + --test-nav

#include "scene.h"
#include "mesh_prims.h"
#include "asset_root.h"                    // portable assetRoot() (assets-LFS)
#include "audio_root.h"                    // portable resolveAudio() (D: mirror / G: packs)
#include "anim.h"                          // Skinner + --list-clips clip check
#include "level1.h"
#include "level_loader.h"                   // data-driven canonical level loader + per-room PVS cull (--test-canonlevel)
#include "player.h"
#include "monster.h"
#include "level1_game.h"
#include "canon_play.h"                     // --world canonlevel gameplay (sidearm + animated enemies + Martinez + girls)
#include "npc_dialog.h"                     // rescued-NPC talk/dialog -> companion (the captive girl)
#include "physprops.h"                      // FEATURE_GOALS §1: hanging cubes / joints (ragdoll foundation)
#include "ragdoll.h"                        // FEATURE_GOALS §2: physics death ragdoll
#include "editor/editor.h"                  // native Level Editor E1 (brain + self-test)
#include "barrels.h"                        // explosive barrels (shoot -> chain explosion)
#include "holo_terminal.h"                  // Jake's cell holographic terminal (text + input)
#include "secret_room.h"                    // code-locked trapdoor -> stocked secret room
#include "engine/ecs/Ecs.h"                 // sparse-set ECS core (10k+ entities)
#include "ecs_render.h"                     // ECS -> GPU-driven render feed
#include "spire_mid.h"                      // EFLZ Spire F3/F4/F5 mid-floor content
#include "spire_top.h"                      // EFLZ Spire F6/F7 top-floor content (Act-1 finale)
#include "spire_nexus.h"                    // EFLZ Floor 4.5 Nexus Chamber / The Chorus (off-elevator boss)
#include "spire_sublevels.h"                // EFLZ hidden Floor-7 sub-levels + Dr. Chen Return Mission
#include "timeline.h"                        // EFLZ morality/timeline backbone for the 12 endings (--test-timeline)
#include "act2_world.h"                      // EFLZ Act-2 open-world surface host + L8/L9 (--test-act2)
#include "tod.h"                             // EFLZ Time-of-Day cycle (sky/sun via SkyParams — --test-tod)
#include "weather.h"                         // EFLZ Weather (7 states, biome-gated, hazard — --test-weather)
#include "elevator.h"
#include "club1127.h"
#include "valley.h"                          // Crystal Valleys (Act 2, L15 — --world valley)
#include "cliffs.h"                          // Salvari cliffs finale (--world cliffs)
#include "terrain.h"
#include "fx.h"
#include "hud.h"
#include "ui.h"                              // GENERAL game-UI: menus + production HUD + --test-ui
#include "save.h"                            // GENERAL versioned checkpoint save/load + --test-saveload
#include "dialog.h"                          // AI-dialog + TTS voice on skinned NPCs (§3) + --test-dialog
#include "stress.h"
#include "destruct_demo.h"                 // K-T1 destruction demo (--world destruct)
#include "ragdoll_demo.h"                  // Physics §2 ragdoll demo (--world ragdoll) + blend check
#include "vehicle.h"                       // vehicle demo worlds (--world drive/boat/fly)

#include <memory>
#include <string_view>
#include <string>
#include <cmath>
#include <vector>
#include <unordered_map>   // per-weapon fire-sound cache (name -> SoundHandle)
#include <cstdint>
#include <cstdlib>
#include <algorithm>
#include <filesystem>
#include <cstdio>
#include <thread>     // r_maxfps frame limiter
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
    // Portal flood-fill depth: how many OPEN-doorway hops the canonlevel cull floods out
    // from the player's room. Higher = see further down a hall through open doors (more
    // rooms drawn); 1 = current room + direct neighbours only (tight). The flood is also
    // gated by the camera frustum (only rooms you LOOK at are drawn) + a room budget, so it
    // never falls back to the whole tower. Live-tunable.
    console.registerCVar("r_culldepth", "6", "canonlevel portal flood-fill depth (open-doorway hops; 1 = direct neighbours only)");
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

// Mouse-wheel accumulator (weapon cycling). The scroll callback adds the wheel
// delta; the main loop consumes it once per frame to switch weapons.
static double g_weaponScroll = 0.0;
void scrollCallback(GLFWwindow* /*win*/, double /*xoff*/, double yoff) { g_weaponScroll += yoff; }

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
// ---------------------------------------------------------------------------
// --test-debris : K-T2 GPU-compute persistent debris world self-test.
//
// Drives the REAL Vulkan render device HEADLESS (no window) so the compute path is
// actually exercised on the GPU (not a CPU stand-in). It spawns a burst of N
// fragments above a ground plane, steps the compute sim M frames (each through a
// real beginFrame -> gpuDebrisStep -> gpuDebrisDraw -> endFrame), and asserts:
//   (a) count is correct right after spawn,
//   (b) fragments FALL (minY drops) then SETTLE on the ground (no NaNs, bounded
//       positions, most fragments asleep, ~zero residual speed),
//   (c) LIFETIME expiry FREES fragments back to the pool (alive count drops to 0),
//   (d) NO leaks: every fragment returns to the dead pool, alive == 0 at the end.
// Prints "debris: X/Y passed" and returns true iff all pass.
static bool runDebrisSelfTest() {
    using namespace x3::rhi;
    int passed = 0, total = 0;
    auto check = [&](const char* name, bool ok) {
        ++total; if (ok) ++passed;
        x3::logInfo(std::string("  [debris] ") + (ok ? "PASS " : "FAIL ") + name);
        return ok;
    };

    if (!glfwInit()) { x3::logError("[debris] glfwInit failed"); return false; }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

    std::unique_ptr<IRenderDevice> device(createRenderDevice());
    DeviceDesc desc{};
    desc.width = 640; desc.height = 360; desc.headless = true;
#ifdef _DEBUG
    desc.validation = true;
#endif
    if (!device->init(desc)) { x3::logError("[debris] device init failed"); glfwTerminate(); return false; }

    // Ground plane at y=0; modest gravity world.
    IRenderDevice::GpuDebrisParams p{};
    p.groundY = 0.0f;
    p.restitution = 0.10f;            // low bounce so fragments settle quickly
    p.friction = 0.6f;
    p.linearDamping = 0.6f;
    p.sleepLinSpeed = 0.30f;
    p.sleepAngSpeed = 0.6f;
    p.sleepFrames = 8;
    device->gpuDebrisConfig(p);

    const uint32_t N = 4096;          // far beyond the ~256 Jolt chunk budget
    const float spawnPos[3] = { 0.0f, 6.0f, 0.0f };
    // Lifetime well clear of the settle window below so none expire mid-settle.
    uint32_t spawned = device->gpuDebrisSpawnBurst(spawnPos, N, /*speed*/3.0f,
                                                   /*lifetime*/3.0f, /*halfExtent*/0.1f, /*seed*/12345u);
    check("spawn count == requested", spawned == N);
    check("alive == N right after spawn", device->gpuDebrisAliveCount() == N);
    check("capacity >= N", device->gpuDebrisCapacity() >= N);

    const float tint[4] = { 0.7f, 0.55f, 0.4f, 1.0f };
    const float dt = 1.0f / 60.0f;
    const float white[4] = { 1, 1, 1, 1 };

    // Helper: run one device frame that steps + draws the debris.
    auto stepFrame = [&]() {
        device->setCamera(0.0f, 3.0f, 10.0f, -1.5708f, -0.2f, 60.0f);
        FrameContext fc = device->beginFrame();
        if (!fc.valid) return;
        device->gpuDebrisStep(dt);
        device->gpuDebrisDraw(fc, tint);
        device->endFrame(fc);
    };

    // --- Step a few frames; fragments should be FALLING (minY below spawn). ---
    for (int i = 0; i < 6; ++i) stepFrame();
    IRenderDevice::GpuDebrisStats mid = device->gpuDebrisReadback(1.0e4f);
    check("no NaNs while falling", mid.nanCount == 0);
    check("bounded positions while falling", mid.outOfBounds == 0);
    check("fragments fell below spawn height", mid.minY < spawnPos[1]);
    check("alive unchanged before any expiry", mid.alive == N);

    // --- Settle: step to ~1.9s total so every fragment hits the ground + sleeps.
    //     The shortest lifetime is 0.7*3.0 = 2.1s, so NONE expire in this window. ---
    for (int i = 0; i < 108; ++i) stepFrame();   // 6 + 108 = 114 frames ~ 1.9s
    IRenderDevice::GpuDebrisStats settled = device->gpuDebrisReadback(1.0e4f);
    check("no NaNs after settling", settled.nanCount == 0);
    check("bounded positions after settling", settled.outOfBounds == 0);
    check("rest on/above the ground (minY >= groundY)", settled.minY >= p.groundY - 0.05f);
    check("did not sink far (maxY bounded)", settled.maxY < spawnPos[1] + 1.0f);
    check("most fragments settled to sleep", settled.settled > (N * 3) / 4);
    check("settled debris is ~motionless", settled.maxSpeed < 1.0f);
    check("still alive before lifetime expiry", settled.alive == N);

    // --- Lifetime expiry: keep stepping past the 2.0s max lifetime so EVERY
    //     fragment's life decays to 0 and is freed back to the pool. ---
    for (int i = 0; i < 240; ++i) stepFrame();
    IRenderDevice::GpuDebrisStats expired = device->gpuDebrisReadback(1.0e4f);
    check("lifetime expiry freed all fragments", expired.alive == 0);
    check("alive counter back to 0 (no leak)", device->gpuDebrisAliveCount() == 0);
    check("no NaNs after full recycle", expired.nanCount == 0);

    // --- Re-spawn into the recycled pool to prove slots are reusable (no leak/grow). ---
    uint32_t resp = device->gpuDebrisSpawnBurst(spawnPos, 1000, 3.0f, 1.0f, 0.1f, 777u);
    check("re-spawn into recycled pool", resp == 1000 && device->gpuDebrisAliveCount() == 1000);
    for (int i = 0; i < 120; ++i) stepFrame();
    check("re-spawned batch also expires cleanly", device->gpuDebrisAliveCount() == 0);

    device->shutdown();
    glfwTerminate();

    std::printf("debris: %d/%d passed\n", passed, total);
    x3::logInfo("debris: " + std::to_string(passed) + "/" + std::to_string(total) + " passed");
    return passed == total;
}

// --test-gpuskin : GPU compute-skinning self-test (GPU SKINNING OF MODELS).
//
// Drives the REAL Vulkan render device HEADLESS (no window) so the compute skinning
// path is actually exercised on the GPU (not a CPU stand-in). It registers a small
// skinned mesh, sets KNOWN palettes, runs the compute skinning pre-pass through a
// real beginFrame -> setSkinnedPalette -> (graph dispatches skin.comp) -> endFrame,
// reads back the skinned-output buffer, and asserts it matches a CPU linear-blend-
// skinning reference within epsilon:
//   (a) IDENTITY palette  => output == bind pose EXACTLY,
//   (b) a known joint TRANSLATION => weighted verts move by the expected amount,
//   (c) a known joint ROTATION (+ translation) => verts land where the CPU LBS
//       reference (p' = sum_i w_i * J[idx_i] * p) places them.
// Prints "gpuskin: X/Y passed" and returns true iff all pass.
static bool runGpuSkinSelfTest() {
    using namespace x3::rhi;
    int passed = 0, total = 0;
    auto check = [&](const char* name, bool ok) {
        ++total; if (ok) ++passed;
        x3::logInfo(std::string("  [gpuskin] ") + (ok ? "PASS " : "FAIL ") + name);
        return ok;
    };

    // ---- column-major 4x4 helpers (glTF/glm convention) for the CPU reference. ----
    auto trsToMat4 = [](const float t[3], const float q[4], const float s[3], float* m) {
        const float x=q[0], y=q[1], z=q[2], w=q[3];
        const float xx=x*x, yy=y*y, zz=z*z, xy=x*y, xz=x*z, yz=y*z, wx=w*x, wy=w*y, wz=w*z;
        m[0]=(1-2*(yy+zz))*s[0]; m[1]=(2*(xy+wz))*s[0]; m[2]=(2*(xz-wy))*s[0]; m[3]=0;
        m[4]=(2*(xy-wz))*s[1]; m[5]=(1-2*(xx+zz))*s[1]; m[6]=(2*(yz+wx))*s[1]; m[7]=0;
        m[8]=(2*(xz+wy))*s[2]; m[9]=(2*(yz-wx))*s[2]; m[10]=(1-2*(xx+yy))*s[2]; m[11]=0;
        m[12]=t[0]; m[13]=t[1]; m[14]=t[2]; m[15]=1;
    };
    auto xformPoint = [](const float m[16], const float p[3], float o[3]) {
        o[0]=m[0]*p[0]+m[4]*p[1]+m[8] *p[2]+m[12];
        o[1]=m[1]*p[0]+m[5]*p[1]+m[9] *p[2]+m[13];
        o[2]=m[2]*p[0]+m[6]*p[1]+m[10]*p[2]+m[14];
    };
    auto xformDir = [](const float m[16], const float d[3], float o[3]) {
        o[0]=m[0]*d[0]+m[4]*d[1]+m[8] *d[2];
        o[1]=m[1]*d[0]+m[5]*d[1]+m[9] *d[2];
        o[2]=m[2]*d[0]+m[6]*d[1]+m[10]*d[2];
    };

    // ---- A small synthetic skinned mesh: 4 verts, 2 joints. The first two verts are
    // rigidly bound to joint 0, the last two to joint 1, and the MIDDLE-ish weights
    // exercise the blend (a 50/50 vertex). ----
    const uint32_t V = 4;
    const uint32_t J = 2;
    std::vector<MeshVertex> bind(V);
    bind[0] = { {0.0f, 0.0f, 0.0f}, {0,1,0}, {0,0} };
    bind[1] = { {1.0f, 0.0f, 0.0f}, {0,1,0}, {1,0} };
    bind[2] = { {2.0f, 0.0f, 0.0f}, {0,0,1}, {0,1} };
    bind[3] = { {3.0f, 1.0f, 0.0f}, {1,0,0}, {1,1} };   // 50/50 between joint 0 and 1
    std::vector<uint16_t> jidx = {
        0,0,0,0,   // v0 -> joint 0
        0,0,0,0,   // v1 -> joint 0
        1,0,0,0,   // v2 -> joint 1
        0,1,0,0,   // v3 -> 50% joint0 + 50% joint1
    };
    std::vector<float> jwt = {
        1,0,0,0,
        1,0,0,0,
        1,0,0,0,
        0.5f,0.5f,0,0,
    };
    // Index buffer (two tris) — only needed so createMesh succeeds; the test reads
    // back vertices, it does not rasterize.
    std::vector<uint32_t> idx = { 0,1,2, 0,2,3 };

    // CPU LBS reference: skin `bind` with a flat palette of J column-major mat4s.
    auto cpuSkin = [&](const std::vector<float>& palette, std::vector<MeshVertex>& out) {
        out.resize(V);
        for (uint32_t v = 0; v < V; ++v) {
            const float* bp = bind[v].pos;
            const float* bn = bind[v].normal;
            const uint16_t* ji = &jidx[v*4];
            const float* jw = &jwt[v*4];
            float wsum = jw[0]+jw[1]+jw[2]+jw[3];
            float pAcc[3]={0,0,0}, nAcc[3]={0,0,0};
            if (wsum < 1e-6f) { pAcc[0]=bp[0]; pAcc[1]=bp[1]; pAcc[2]=bp[2]; nAcc[0]=bn[0]; nAcc[1]=bn[1]; nAcc[2]=bn[2]; }
            else {
                for (int i = 0; i < 4; ++i) {
                    float w = jw[i]; if (w <= 0.0f) continue;
                    uint16_t j = ji[i]; if (j >= J) continue;
                    const float* jm = &palette[(size_t)j*16];
                    float tp[3], tn[3]; xformPoint(jm, bp, tp); xformDir(jm, bn, tn);
                    pAcc[0]+=w*tp[0]; pAcc[1]+=w*tp[1]; pAcc[2]+=w*tp[2];
                    nAcc[0]+=w*tn[0]; nAcc[1]+=w*tn[1]; nAcc[2]+=w*tn[2];
                }
                float inv = 1.0f/wsum; pAcc[0]*=inv; pAcc[1]*=inv; pAcc[2]*=inv;
            }
            float nl = std::sqrt(nAcc[0]*nAcc[0]+nAcc[1]*nAcc[1]+nAcc[2]*nAcc[2]);
            if (nl > 1e-8f) { nAcc[0]/=nl; nAcc[1]/=nl; nAcc[2]/=nl; }
            out[v] = { {pAcc[0],pAcc[1],pAcc[2]}, {nAcc[0],nAcc[1],nAcc[2]}, {bind[v].uv[0],bind[v].uv[1]} };
        }
    };

    if (!glfwInit()) { x3::logError("[gpuskin] glfwInit failed"); return false; }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

    std::unique_ptr<IRenderDevice> device(createRenderDevice());
    DeviceDesc desc{};
    desc.width = 320; desc.height = 240; desc.headless = true;
#ifdef _DEBUG
    desc.validation = true;
#endif
    if (!device->init(desc)) { x3::logError("[gpuskin] device init failed"); glfwTerminate(); return false; }

    check("device supports GPU skinning", device->supportsGpuSkinning());

    MeshHandle mesh = device->createMesh(bind.data(), V, idx.data(), (uint32_t)idx.size());
    check("createMesh ok", mesh.valid());
    bool reg = device->registerSkinnedMesh(mesh, bind.data(), V, jidx.data(), jwt.data());
    check("registerSkinnedMesh ok", reg);

    // Helper: run one frame that uploads a palette + dispatches the compute skin, then
    // read back + compare to the CPU reference.
    auto runCase = [&](const char* name, const std::vector<float>& palette, float eps) -> bool {
        FrameContext fc = device->beginFrame();
        if (!fc.valid) { check((std::string(name)+": beginFrame").c_str(), false); return false; }
        device->setSkinnedPalette(mesh, palette.data(), J);
        device->endFrame(fc);   // the graph records + executes the skin compute pass

        std::vector<MeshVertex> gpu(V);
        if (!device->readbackSkinnedMesh(mesh, gpu.data(), V)) {
            check((std::string(name)+": readback").c_str(), false); return false;
        }
        std::vector<MeshVertex> ref; cpuSkin(palette, ref);
        float maxErr = 0.0f;
        for (uint32_t v = 0; v < V; ++v) {
            for (int k = 0; k < 3; ++k) maxErr = std::max(maxErr, std::fabs(gpu[v].pos[k]    - ref[v].pos[k]));
            for (int k = 0; k < 3; ++k) maxErr = std::max(maxErr, std::fabs(gpu[v].normal[k] - ref[v].normal[k]));
            for (int k = 0; k < 2; ++k) maxErr = std::max(maxErr, std::fabs(gpu[v].uv[k]     - ref[v].uv[k]));
        }
        x3::logInfo(std::string("    [gpuskin] ") + name + " maxErr=" + std::to_string(maxErr));
        return check((std::string(name)+": GPU == CPU LBS").c_str(), maxErr < eps);
    };

    // (a) IDENTITY palette => output == bind pose EXACTLY.
    {
        std::vector<float> pal((size_t)J*16, 0.0f);
        for (uint32_t j = 0; j < J; ++j) { float* m = &pal[(size_t)j*16]; for (int e=0;e<16;++e) m[e]=(e%5==0)?1.0f:0.0f; }
        // Run the identity case, then ALSO assert it equals the bind pose exactly.
        FrameContext fc = device->beginFrame();
        device->setSkinnedPalette(mesh, pal.data(), J);
        device->endFrame(fc);
        std::vector<MeshVertex> gpu(V);
        bool rb = device->readbackSkinnedMesh(mesh, gpu.data(), V);
        check("identity: readback", rb);
        if (rb) {
            float maxErr = 0.0f;
            for (uint32_t v = 0; v < V; ++v) {
                for (int k=0;k<3;++k) maxErr = std::max(maxErr, std::fabs(gpu[v].pos[k]    - bind[v].pos[k]));
                for (int k=0;k<3;++k) maxErr = std::max(maxErr, std::fabs(gpu[v].normal[k] - bind[v].normal[k]));
            }
            x3::logInfo("    [gpuskin] identity maxErr=" + std::to_string(maxErr));
            check("identity palette => bind pose (exact)", maxErr < 1e-5f);
        }
    }

    // (b) Known joint TRANSLATION: joint 0 translated +Y by 2, joint 1 by -X 1.
    {
        std::vector<float> pal((size_t)J*16, 0.0f);
        float t0[3]={0,2,0}, t1[3]={-1,0,0}, q[4]={0,0,0,1}, s[3]={1,1,1};
        trsToMat4(t0, q, s, &pal[0]);
        trsToMat4(t1, q, s, &pal[16]);
        runCase("translation", pal, 1e-4f);
    }

    // (c) Known joint ROTATION + translation: joint 0 rotated 90deg about +Z, joint 1
    //     rotated -45deg about +X and translated +Z by 0.5 (exercises the upper 3x3
    //     normal transform + the 50/50 blend vertex).
    {
        std::vector<float> pal((size_t)J*16, 0.0f);
        const float a0 = 1.5707963f;            // 90 deg about Z
        float q0[4] = { 0, 0, std::sin(a0*0.5f), std::cos(a0*0.5f) };
        float t0[3] = { 0.0f, 0.0f, 0.0f }, s[3] = {1,1,1};
        trsToMat4(t0, q0, s, &pal[0]);
        const float a1 = -0.7853981f;           // -45 deg about X
        float q1[4] = { std::sin(a1*0.5f), 0, 0, std::cos(a1*0.5f) };
        float t1[3] = { 0.0f, 0.0f, 0.5f };
        trsToMat4(t1, q1, s, &pal[16]);
        runCase("rotation+translation", pal, 1e-4f);
    }

    // Re-run a second translation to prove the per-frame palette is honoured frame to
    // frame (the double-buffered output + descriptor sets work across frames-in-flight).
    {
        std::vector<float> pal((size_t)J*16, 0.0f);
        float t0[3]={3,0,0}, t1[3]={0,0,-2}, q[4]={0,0,0,1}, s[3]={1,1,1};
        trsToMat4(t0, q, s, &pal[0]);
        trsToMat4(t1, q, s, &pal[16]);
        runCase("translation (frame 2)", pal, 1e-4f);
    }

    device->unregisterSkinnedMesh(mesh);
    device->destroyMesh(mesh);
    device->shutdown();
    glfwTerminate();

    std::printf("gpuskin: %d/%d passed\n", passed, total);
    x3::logInfo("gpuskin: " + std::to_string(passed) + "/" + std::to_string(total) + " passed");
    return passed == total;
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
// SpeakingMonster — the HOST adapter that wires the dialog system's speaking
// state onto a SKINNED NPC (X3_WORLD_BLUEPRINT §3, requirement 4). It implements
// x3::dialog::ISpeakingNpc and drives an x3::anim::Skinner READ-ONLY: on
// beginSpeaking it starts a "talk"/idle clip + records the subtitle; each frame
// while speaking it advances a head-bob (a small extra time scrub layered over
// the idle clip so the character reads as "talking"); on endSpeaking it returns
// to rest. It owns its OWN Skinner bound to the NPC's Model so it never mutates
// the MonsterSystem (read-only use of anim). Lip-sync is not required — a talk
// pose / bob is the spec'd behaviour.
//
// Headless-safe: with a non-skinnable / absent model it still tracks the speaking
// lifecycle (begin/tick/end) so the demo + wiring are observable without a device.
class SpeakingMonster final : public x3::dialog::ISpeakingNpc {
public:
    // Bind to a loaded skinned Model (e.g. chief_martinez_anim.glb). The model must
    // outlive this adapter (the demo owns it). Picks a talk/idle clip by name.
    bool bind(const x3::asset::Model& model) {
        m_model = &model;
        m_skinnable = m_skinner.bind(model);
        if (m_skinnable) {
            // Prefer a "talk"/"idle" clip for the speaking pose; fall back to clip 0.
            m_talkClip = m_skinner.findClip({ "talk", "idle" });
            if (m_talkClip < 0 && m_skinner.clipCount() > 0) m_talkClip = 0;
        }
        return m_skinnable;
    }

    bool skinnable()  const { return m_skinnable; }
    bool speaking()   const { return m_speaking; }
    int  beginCount() const { return m_begins; }
    int  endCount()   const { return m_ends; }
    const std::string& subtitle() const { return m_subtitle; }
    // Max per-component change of the joint palette observed between consecutive
    // ticks while speaking — proves the talk-bob actually animated the skeleton.
    float maxPaletteDelta() const { return m_maxDelta; }

    void beginSpeaking(std::string_view line, x3::dialog::VoiceId voice,
                       float estDurationSec) override {
        (void)voice;
        ++m_begins;
        m_speaking = true;
        m_subtitle.assign(line);
        m_animTime = 0.0f;
        m_estDur   = estDurationSec > 0.0f ? estDurationSec : 1.0f;
        m_havePrev = false;
        // Show the subtitle on the console (the HUD path would call
        // IRenderDevice::drawHudText with this string each frame).
        x3::logInfo(std::string("[dialog] ") + std::string(line));
    }

    void tickSpeaking(float dt, float phase01) override {
        if (!m_speaking) return;
        // Talk bob: advance the clip time, modulated by a small sinusoid so the
        // head visibly bobs across the line (peaks mid-line, settles at the end).
        const float bob = 1.0f + 0.6f * std::sin(phase01 * 6.2831853f);
        m_animTime += dt * bob;
        if (m_skinnable && m_model && m_talkClip >= 0) {
            // READ-ONLY anim use: compute the palette at the talk-clip time WITHOUT a
            // device (the demo is headless). A real windowed host would instead call
            // m_skinner.apply(model, device, talkClip, time) to skin + draw.
            m_skinner.computePalette(*m_model, (uint32_t)m_talkClip, m_animTime, m_curPal);
            if (m_havePrev && m_prevPal.size() == m_curPal.size()) {
                float d = 0.0f;
                for (size_t i = 0; i < m_curPal.size(); ++i)
                    d = std::max(d, std::fabs(m_curPal[i] - m_prevPal[i]));
                m_maxDelta = std::max(m_maxDelta, d);
            }
            m_prevPal = m_curPal;
            m_havePrev = true;
        }
    }

    void endSpeaking() override {
        ++m_ends;
        m_speaking = false;
        m_subtitle.clear();
    }

private:
    const x3::asset::Model* m_model = nullptr;
    x3::anim::Skinner       m_skinner;
    bool   m_skinnable = false;
    int    m_talkClip  = -1;
    bool   m_speaking  = false;
    int    m_begins    = 0;
    int    m_ends      = 0;
    float  m_animTime  = 0.0f;
    float  m_estDur    = 1.0f;
    std::string m_subtitle;
    std::vector<float> m_curPal, m_prevPal;
    bool   m_havePrev  = false;
    float  m_maxDelta  = 0.0f;
};
} // namespace

int main(int argc, char** argv) {
    bool smoketest = false, testAsset = false, testConsole = false, testPhysics = false,
         testGltf = false, testPlayer = false, testInteract = false, testPickup = false,
         testPhysprops = false, testRagdoll = false, testRagdollSkin = false, testEditor = false,
         testBarrels = false, testHoloterm = false, testEcs = false, testEcsRender = false,
         testCombat = false, testAudio = false, testLevel1 = false, testJobs = false,
         testPhase2a = false, testPhase2b = false, testAnim = false, testTerrain = false,
         testStreaming = false, testAi = false, testDoorCode = false, testElevator = false,
         testElevatorFsm = false,
         testTerrainPlace = false, testNet = false, testRescue = false, testDestruction = false,
         testNav = false, testWeapons = false, testVehicle = false, testFootIk = false,
         testNetSync = false, testNetInterp = false, testNetPredict = false, testNpcTalk = false,
         testDeathRagdoll = false, testCanonLevel = false, testCanonPlay = false;
    // --test-rt (hardware ray-tracing RT AO): runs the headless smoketest render
    // path with r_rtao forced ON so the BLAS/TLAS build + ray-query AO compute +
    // apply passes are exercised under Vulkan validation on an RT-capable device.
    bool        testRt = false;
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
    // UI-demo capture (--ui-demo [path.png] / --screenshot-menu): build EFLZ Level 1,
    // pose the gate-standard corridor camera, then draw the GENERAL game-UI MAIN MENU
    // (title + START / QUIT, the START button focused/hot) over the rendered scene and
    // capture a PNG — so the menu layer can be SEEN headlessly without being at the
    // keyboard. Additive + offscreen, like --screenshot. Default path:
    // G:/X3Native/captures/ui_menu.png.
    bool        uiDemo = false;
    std::string uiDemoPath = "G:/X3Native/captures/ui_menu.png";
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
        if (a == "--smoketest") smoketest = true;
        else if (a == "--test-rt") { smoketest = true; testRt = true; }
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
        else if (a == "--test-barrels") testBarrels = true;
        else if (a == "--test-holoterm") testHoloterm = true;
        else if (a == "--test-secretroom") testSecretRoom = true;
        else if (a == "--test-ecs") testEcs = true;
        else if (a == "--test-ecsrender") testEcsRender = true;
        else if (a == "--test-pickup") testPickup = true;
        else if (a == "--test-combat") testCombat = true;
        else if (a == "--test-deathragdoll") testDeathRagdoll = true;
        else if (a == "--test-audio") testAudio = true;
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
        else if (a == "--test-act2bosses") testAct2Bosses = true;
        else if (a == "--test-spiremid") testSpireMid = true;
        else if (a == "--test-nexus") testNexus = true;
        else if (a == "--test-spiretop") testSpireTop = true;
        else if (a == "--test-timeline") testTimeline = true;
        else if (a == "--test-dronehack") testDroneHack = true;
        else if (a == "--test-sublevels") testSubLevels = true;
        else if (a == "--test-act2") testAct2 = true;
        else if (a == "--test-tod") testTod = true;
        else if (a == "--test-weather") testWeather = true;
        else if (a == "--test-doorcode") testDoorCode = true;
        else if (a == "--test-elevator") testElevator = true;
        else if (a == "--test-elevatorfsm") testElevatorFsm = true;
        else if (a == "--test-net") testNet = true;
        else if (a == "--test-netsync") testNetSync = true;
        else if (a == "--test-netinterp") testNetInterp = true;
        else if (a == "--test-netpredict") testNetPredict = true;
        else if (a == "--test-rescue") testRescue = true;
        else if (a == "--test-npctalk") testNpcTalk = true;
        else if (a == "--test-destruction") testDestruction = true;
        else if (a == "--test-debris") testDebris = true;
        else if (a == "--test-gpuskin") testGpuSkin = true;
        else if (a == "--test-collapse") testCollapse = true;
        else if (a == "--test-physjoint") testPhysJoint = true;
        else if (a == "--test-nav") testNav = true;
        else if (a == "--test-weapons") testWeapons = true;
        else if (a == "--test-vehicle") testVehicle = true;
        else if (a == "--test-footik") testFootIk = true;
        else if (a == "--test-ui") testUi = true;
        else if (a == "--test-saveload") testSaveLoad = true;
        else if (a == "--test-dialog") testDialog = true;
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
            if (i + 1 < argc && argv[i + 1][0] != '-') worldMode = argv[++i];
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
    }

    // Headless self-tests (no window / Vulkan needed)
    if (testJobs) {
        x3::logInfo("running job system (Subsystem A) self-test...");
        return x3::jobs::runJobSystemSelfTest() ? 0 : 1;
    }
    if (testAsset) {
        x3::logInfo("running asset (D5) self-test...");
        return x3::asset::runAssetSelfTest() ? 0 : 1;
    }
    if (testConsole) {
        x3::logInfo("running console (D6) self-test...");
        return x3::con::runConsoleSelfTest() ? 0 : 1;
    }
    if (testPhysics) {
        x3::logInfo("running physics (M3) self-test...");
        return x3::phys::runPhysicsSelfTest() ? 0 : 1;
    }
    if (testPhysJoint) {
        x3::logInfo("running Physics §1 suspended/constrained-body (--test-physjoint) self-test...");
        return x3::phys::runPhysJointSelfTest() ? 0 : 1;
    }
    if (testRagdoll) {
        x3::logInfo("running Physics §2 ragdoll+blend (--test-ragdoll) self-test...");
        // Engine-side: the Jolt ragdoll fall/settle/chain-hold + blend-math check.
        bool engineOk = x3::phys::runRagdollSelfTest();
        // App-side: drive the REAL anim::Skinner ragdoll-blend across weight 0->1
        // over a synthetic skinned model and assert the palette interpolates
        // monotonically (the §2 skin-follows-ragdoll acceptance, end to end).
        int bPass = 0, bTotal = 0;
        bool blendOk = x3::game::runRagdollBlendCheck(bPass, bTotal);
        x3::logInfo("ragdoll-blend: " + std::to_string(bPass) + "/" +
                    std::to_string(bTotal) + " passed");
        // App-side physics-death ragdoll (this session's app/ragdoll.cpp path).
        bool deathOk = x3::game::runRagdollSelfTest();
        return (engineOk && blendOk && deathOk) ? 0 : 1;
    }
    if (testGltf) {
        x3::logInfo("running glTF/GLB model loader (M2) self-test...");
        return x3::asset::runModelLoaderSelfTest() ? 0 : 1;
    }
    if (testPlayer) {
        x3::logInfo("running player/character-controller (S3) self-test...");
        return x3::game::runPlayerSelfTest() ? 0 : 1;
    }
    if (testInteract) {
        x3::logInfo("running button->door interaction (S4) self-test...");
        return x3::game::runInteractSelfTest() ? 0 : 1;
    }
    if (testPhysprops) {
        x3::logInfo("running physics-props (hanging cubes / joints) self-test...");
        return x3::game::runPhysPropsSelfTest() ? 0 : 1;
    }
    if (testRagdollSkin) {
        x3::logInfo("running ragdoll-skin (rigid bone attach) self-test...");
        return x3::game::runRagdollSkinSelfTest() ? 0 : 1;
    }
    if (testEditor) {
        x3::logInfo("running Level Editor E1 (JSON/pick/gizmo) self-test...");
        return x3::editor::runEditorSelfTest() ? 0 : 1;
    }
    if (testBarrels) {
        x3::logInfo("running explosive-barrels self-test...");
        return x3::game::runBarrelSelfTest() ? 0 : 1;
    }
    if (testHoloterm) {
        x3::logInfo("running holo-terminal (text + input) self-test...");
        return x3::game::runHoloTerminalSelfTest() ? 0 : 1;
    }
    if (testSecretRoom) {
        x3::logInfo("running secret-room (code-locked trapdoor) self-test...");
        return x3::game::runSecretRoomSelfTest() ? 0 : 1;
    }
    if (testEcs) {
        x3::logInfo("running ECS (sparse-set, 50k entities) self-test...");
        return x3::ecs::runEcsSelfTest() ? 0 : 1;
    }
    if (testEcsRender) {
        x3::logInfo("running ECS->GPU render-feed self-test...");
        return x3::game::runEcsRenderSelfTest() ? 0 : 1;
    }
    if (testPickup) {
        x3::logInfo("running weapon pickup + arming (S5) self-test...");
        return x3::game::runPickupSelfTest() ? 0 : 1;
    }
    if (testCombat) {
        x3::logInfo("running shoot-monster combat (S6) self-test...");
        return x3::game::runCombatSelfTest() ? 0 : 1;
    }
    if (testDeathRagdoll) {
        x3::logInfo("running skinned death-ragdoll (TASK#12) self-test...");
        return x3::game::runDeathRagdollSelfTest() ? 0 : 1;
    }
    if (testAudio) {
        x3::logInfo("running audio (M9) self-test...");
        return x3::audio::runAudioSelfTest() ? 0 : 1;
    }
    if (testLevel1) {
        x3::logInfo("running EFLZ Level 1 (Awakening) self-test (T1-T6)...");
        return x3::game::runLevel1SelfTest() ? 0 : 1;
    }
    if (testCanonLevel) {
        x3::logInfo("running EFLZ data-driven canonical-level self-test (C1-C8)...");
        return x3::game::runCanonLevelSelfTest() ? 0 : 1;
    }
    if (testCanonPlay) {
        x3::logInfo("running EFLZ canon Floor-1 gameplay self-test (P1-P9)...");
        return x3::game::runCanonPlaySelfTest() ? 0 : 1;
    }
    if (testPhase2a) {
        x3::logInfo("running EFLZ Phase 2a (player health + enemies fight back) self-test...");
        return x3::game::runPhase2aSelfTest() ? 0 : 1;
    }
    if (testPhase2b) {
        x3::logInfo("running EFLZ Phase 2b (super-strength melee + Martinez boss phases) self-test...");
        return x3::game::runPhase2bSelfTest() ? 0 : 1;
    }
    if (testAnim) {
        x3::logInfo("running J1 skeletal animation + CPU skinning self-test...");
        return x3::anim::runAnimSelfTest() ? 0 : 1;
    }
    if (testLocomotion) {
        x3::logInfo("running T1 locomotion-blend (idle/walk/run + crossfade) self-test...");
        return x3::anim::runLocomotionSelfTest(testLocomotionPath) ? 0 : 1;
    }
    if (listClips) {
        // Asset-pipeline check for the retargeted multi-clip character GLBs:
        // load the GLB headless, list its clips, and confirm Walk sampled at
        // t=0 vs t=0.5 changes the joint palette (proves a real, distinct clip).
        if (listClipsPath.empty()) {
            x3::logError("--list-clips: need a GLB path");
            return 1;
        }
        namespace fs = std::filesystem;
        fs::path p(listClipsPath);
        if (!fs::exists(p)) { x3::logError("--list-clips: no such file: " + listClipsPath); return 1; }
        std::unique_ptr<x3::asset::IAssetSource> src(x3::asset::createAssetSource());
        src->mountDir(p.parent_path().string(), 0);
        std::unique_ptr<x3::asset::IModelLoader> loader(
            x3::asset::createModelLoader(nullptr, src.get()));   // headless
        x3::asset::Model model = loader->load(p.filename().string());
        if (!model.ok) { x3::logError("--list-clips: load failed"); return 1; }
        x3::logInfo("--list-clips: " + listClipsPath + " has " +
                    std::to_string(model.animations.size()) + " animation clip(s):");
        for (size_t c = 0; c < model.animations.size(); ++c)
            x3::logInfo("  clip[" + std::to_string(c) + "] = \"" +
                        model.animations[c].name + "\"  (" +
                        std::to_string(model.animations[c].duration) + "s)");
        x3::anim::Skinner sk;
        bool bound = sk.bind(model);
        if (!bound) { x3::logError("--list-clips: model not skinnable"); loader->unload(model); return 1; }
        int idle = sk.findClip({ "idle" });
        int walk = sk.findClip({ "walk" });
        int run  = sk.findClip({ "run" });
        x3::logInfo("--list-clips: findClip idle=" + std::to_string(idle) +
                    " walk=" + std::to_string(walk) + " run=" + std::to_string(run));
        bool ok = sk.clipCount() > 1 && walk >= 0 && run >= 0;
        // Confirm Walk is a live clip: palette differs between t=0 and t=0.5,
        // and (when an idle clip exists) Walk@0 differs from Idle@0.
        if (walk >= 0) {
            std::vector<float> w0, w5, i0;
            sk.computePalette(model, (uint32_t)walk, 0.0f, w0);
            sk.computePalette(model, (uint32_t)walk, 0.5f, w5);
            float dWalk = 0.0f;
            for (size_t i = 0; i < w0.size() && i < w5.size(); ++i)
                dWalk = std::max(dWalk, std::fabs(w0[i] - w5[i]));
            x3::logInfo("--list-clips: Walk palette max-delta t0->t0.5 = " + std::to_string(dWalk));
            ok = ok && dWalk > 1e-3f;
            if (idle >= 0) {
                sk.computePalette(model, (uint32_t)idle, 0.0f, i0);
                float dVsIdle = 0.0f;
                for (size_t i = 0; i < w0.size() && i < i0.size(); ++i)
                    dVsIdle = std::max(dVsIdle, std::fabs(w0[i] - i0[i]));
                x3::logInfo("--list-clips: Walk@0 vs Idle@0 max-delta = " + std::to_string(dVsIdle));
                ok = ok && dVsIdle > 1e-3f;
            }
        }
        loader->unload(model);
        x3::logInfo(std::string("--list-clips: ") + (ok ? "PASS (>1 clip, Walk+Run present, Walk animates)"
                                                        : "FAIL"));
        return ok ? 0 : 1;
    }
    if (testTerrain) {
        x3::logInfo("running B2 tiled terrain world self-test (settle + LOD)...");
        return x3::game::runTerrainSelfTest() ? 0 : 1;
    }
    if (testTerrainPlace) {
        x3::logInfo("running terrain placement API self-test (height/normal/place)...");
        return x3::game::runTerrainPlaceSelfTest() ? 0 : 1;
    }
    if (testStreaming) {
        x3::logInfo("running B3 world-streaming self-test (residency ring + async gen)...");
        return x3::game::runStreamingSelfTest() ? 0 : 1;
    }
    if (testAi) {
        x3::logInfo("running D-ai monster combat behaviour state-machine self-test...");
        return x3::game::runAiSelfTest() ? 0 : 1;
    }
    if (testBestiary) {
        x3::logInfo("running data-driven enemy bestiary roster self-test...");
        return x3::game::runBestiarySelfTest() ? 0 : 1;
    }
    if (testBosses) {
        x3::logInfo("running EFLZ Act-1 mid-boss roster + machine-extension "
                    "(multi-pod + scripted pre-fight hook) self-test...");
        return x3::game::runBossesSelfTest() ? 0 : 1;
    }
    if (testAct2Bosses) {
        x3::logInfo("running EFLZ Act-2 roster (5 alien-planet-surface enemies + "
                    "4 single-body bosses on the existing phase machine) self-test...");
        return x3::game::runAct2BossesSelfTest() ? 0 : 1;
    }
    if (testSpireMid) {
        x3::logInfo("running EFLZ Spire mid-floor (F3 Labs / F4 Offices / F5 Synth bay) "
                    "encounter-content self-test...");
        return x3::game::runSpireMidSelfTest() ? 0 : 1;
    }
    if (testNexus) {
        x3::logInfo("running EFLZ Floor 4.5 Nexus Chamber / The Chorus "
                    "(off-elevator multi-pod boss) self-test...");
        return x3::game::runNexusSelfTest() ? 0 : 1;
    }
    if (testSpireTop) {
        x3::logInfo("running EFLZ Spire top-floor (F6 Alien Technology Lab / F7 Executive "
                    "Laboratory Act-1 finale) encounter-content self-test...");
        return x3::game::runSpireTopSelfTest() ? 0 : 1;
    }
    if (testTimeline) {
        x3::logInfo("running EFLZ morality/timeline backbone (infection 4-stage timers + "
                    "cure rates, Omega/Alpha/Beta/Gamma selector, morality axes, 12-ending "
                    "eligibility) self-test...");
        return x3::game::runTimelineSelfTest() ? 0 : 1;
    }
    if (testDroneHack) {
        x3::logInfo("running EFLZ F5 Drone Manufacturing — Sarah's master hack "
                    "(strip Swarm AI HP + flip the drone army) self-test...");
        return x3::game::runDroneHackSelfTest() ? 0 : 1;
    }
    if (testSubLevels) {
        x3::logInfo("running EFLZ hidden Floor-7 sub-levels (Waste Disposal / Cryo Storage "
                    "[Frozen Collective] / Enhanced Interrogation -> Dr. Chen Return Mission) "
                    "self-test...");
        return x3::game::runSubLevelsSelfTest() ? 0 : 1;
    }
    if (testTod) {
        x3::logInfo("running EFLZ Time-of-Day cycle (4-phase dawn/day/dusk/night driving "
                    "sky/sun dir+color+intensity+haze+ambient via SkyParams; deterministic) "
                    "self-test...");
        return x3::game::runTodSelfTest() ? 0 : 1;
    }
    if (testWeather) {
        x3::logInfo("running EFLZ Weather (7 states clear/cloudy/rain/storm/fog/sandstorm/snow; "
                    "smooth timed transitions; biome-gated; hazard flag for HazardZone) "
                    "self-test...");
        return x3::game::runWeatherSelfTest() ? 0 : 1;
    }
    if (testAct2) {
        x3::logInfo("running EFLZ Act-2 open-world surface (L8 Surface Emergence "
                    "+ L9 Crystalline Desert Edge: alien terrain/sky host, lab-exit "
                    "gauntlet, Emergence-Point companions, crystal desert + hazard zone) "
                    "self-test...");
        return x3::game::runAct2WorldSelfTest() ? 0 : 1;
    }
    if (testDoorCode) {
        x3::logInfo("running door-code keypad (locked coded door) self-test (K1-K6)...");
        return x3::game::runDoorCodeSelfTest() ? 0 : 1;
    }
    if (testElevator) {
        x3::logInfo("running advanced elevator (call/travel/carry) self-test (E1-E6)...");
        return x3::game::runElevatorSelfTest() ? 0 : 1;
    }
    if (testElevatorFsm) {
        x3::logInfo("running souped-up strata/disco elevator FSM self-test "
                    "(10-state FSM + strata + 1127 disco -> Club 1127)...");
        return x3::game::runElevatorFsmSelfTest() ? 0 : 1;
    }
    if (testNet) {
        x3::logInfo("running netcode (Subsystem N, Phase 0) self-test "
                    "(loopback round-trip + generation-stale reject + fixed-step determinism)...");
        return x3::net::runNetworkSelfTest() ? 0 : 1;
    }
    if (testNetSync) {
        x3::logInfo("running netcode (Subsystem N, Phase 0b) client/server "
                    "input->snapshot routing self-test "
                    "(command send -> server apply+sim -> snapshot -> client mirror)...");
        return x3::net::runNetSyncSelfTest() ? 0 : 1;
    }
    if (testNetInterp) {
        x3::logInfo("running netcode (Subsystem N, Phase 0c) client snapshot "
                    "interpolation + jitter-buffer self-test "
                    "(jittered snapshots -> bracketed lerp/slerp -> smooth render)...");
        return x3::net::runNetInterpSelfTest() ? 0 : 1;
    }
    if (testNetPredict) {
        x3::logInfo("running netcode (Subsystem N, Phase 1) client prediction + "
                    "server reconciliation self-test "
                    "(predict immediately -> lagged authority+ack -> rollback/resim)...");
        return x3::net::runNetPredictSelfTest() ? 0 : 1;
    }
    if (testRescue) {
        x3::logInfo("running F2 rescue (victim/companion/transform) self-test (R0-R5)...");
        return x3::game::runRescueSelfTest() ? 0 : 1;
    }
    if (testNpcTalk) {
        x3::logInfo("running rescued-NPC talk/dialog -> companion self-test (T1-T7)...");
        return x3::game::runNpcTalkSelfTest() ? 0 : 1;
    }
    if (testDestruction) {
        x3::logInfo("running K-T0/T1 destruction (fracture/impact/hit/explosion) self-test...");
        return x3::phys::runDestructionSelfTest() ? 0 : 1;
    }
    if (testDebris) {
        x3::logInfo("running K-T2 GPU-compute persistent debris world self-test "
                    "(spawn burst -> compute integrate -> fall/settle/sleep -> lifetime free)...");
        return runDebrisSelfTest() ? 0 : 1;
    }
    if (testGpuSkin) {
        x3::logInfo("running GPU compute-skinning self-test (register skinned mesh -> "
                    "set known palette -> compute skin -> readback -> assert vs CPU LBS)...");
        return runGpuSkinSelfTest() ? 0 : 1;
    }
    if (testCollapse) {
        x3::logInfo("running K-T3 structural collapse (support graph) self-test "
                    "(destroy a support -> unsupported sub-graph falls, anchored stays, "
                    "rubble settles, GPU debris fires)...");
        return x3::phys::runCollapseSelfTest() ? 0 : 1;
    }
    if (testNav) {
        x3::logInfo("running GENERAL navigation (nav grid + A* + path-follow) self-test...");
        return x3::ai::runNavSelfTest() ? 0 : 1;
    }
    if (testWeapons) {
        x3::logInfo("running data-driven weapon arsenal (switch/fire/reload/spread) self-test...");
        return x3::game::runWeaponsSelfTest() ? 0 : 1;
    }
    if (testVehicle) {
        x3::logInfo("running vehicle framework self-test "
                    "(wheeled accel/steer + buoyancy waterline + flight thrust/lift)...");
        return x3::phys::runVehicleSelfTest() ? 0 : 1;
    }
    if (testFootIk) {
        x3::logInfo("running foot-IK (two-bone + plant + pelvis) self-test...");
        return x3::anim::runFootIkSelfTest() ? 0 : 1;
    }
    if (testUi) {
        x3::logInfo("running GENERAL game-UI self-test "
                    "(button hit-test + Menu<->Playing<->Paused transitions + settings cvar wiring)...");
        return x3::ui::runUiSelfTest() ? 0 : 1;
    }
    if (testSaveLoad) {
        x3::logInfo("running GENERAL versioned checkpoint save/load self-test "
                    "(round-trip field-by-field + magic/version/checksum/truncation reject)...");
        return x3::save::runSaveLoadSelfTest() ? 0 : 1;
    }
    if (testDialog) {
        x3::logInfo("running AI-dialog + TTS self-test "
                    "(offline tree advance + branches; stub AI provider used/fallback; "
                    "stub TTS drives NPC speaking state; no network; leak-clean)...");
        return x3::dialog::runDialogSelfTest() ? 0 : 1;
    }
    if (demoDialog) {
        // Headless, fully offline demo: drive the sample Sarah conversation onto a
        // REAL skinned NPC (chief_martinez_anim.glb) via the SpeakingMonster adapter
        // + an offline stub TTS hook. Proves requirement 4 end-to-end against the
        // actual anim Skinner (read-only) without a window / device / network.
        namespace fs = std::filesystem;
        std::string glb = demoDialogPath.empty()
            ? (x3::game::riggedGlbRoot() + "/chief_martinez_anim.glb")
            : demoDialogPath;
        x3::logInfo("--demo-dialog: NPC model = " + glb);

        // Load the skinned model headlessly (loader pattern from --list-clips).
        x3::asset::Model model;
        std::unique_ptr<x3::asset::IAssetSource> src;
        std::unique_ptr<x3::asset::IModelLoader> loader;
        bool haveModel = false;
        if (fs::exists(glb)) {
            fs::path p(glb);
            src.reset(x3::asset::createAssetSource());
            src->mountDir(p.parent_path().string(), 0);
            loader.reset(x3::asset::createModelLoader(nullptr, src.get())); // headless
            model = loader->load(p.filename().string());
            haveModel = model.ok;
        }
        if (!haveModel) {
            x3::logInfo("--demo-dialog: model absent/failed (clean checkout) — running "
                        "subtitle-only (the conversation + speaking lifecycle still run).");
        }

        SpeakingMonster npc;
        if (haveModel) {
            bool sk = npc.bind(model);
            x3::logInfo(std::string("--demo-dialog: NPC skinnable = ") + (sk ? "yes" : "no"));
        }

        // Offline stub TTS: deterministic clip duration from the line length; NO
        // file I/O, NO network. (Tim's real voice vendor drops in here as the hook.)
        auto stubTts = [](const std::string& line, x3::dialog::VoiceId v) {
            x3::dialog::AudioClip c;
            c.path = std::string("stub://voice/") + x3::dialog::voiceName(v);
            c.durationSec = 0.6f + 0.012f * (float)line.size();
            return c;
        };

        std::unique_ptr<x3::dialog::IDialogSystem> d(x3::dialog::createDialogSystem());
        d->setTtsProvider(stubTts);
        d->setSpeakingNpc(&npc);
        // No AI provider set -> the authored tree is used (offline baseline). Mode
        // stays Tree; the demo shows the guaranteed path.
        x3::dialog::Tree tree = x3::dialog::sampleSarahTree();

        if (!d->start(tree)) { x3::logError("--demo-dialog: tree failed to start"); return 1; }

        // Drive the conversation: tick each line to completion, print the choices,
        // auto-pick choice 0 (deterministic), until the conversation ends.
        int safety = 0;
        while (d->active() && safety++ < 64) {
            // Tick the current line to completion (the speaking state runs here).
            int t = 0;
            while (d->speaking() && t++ < 4000) { d->update(0.02f); }
            const auto& ch = d->choices();
            if (ch.empty()) {
                // Terminal node: the line finished, conversation will end on the next
                // active() check (the system ended it inside the final exitSpeaking()).
                break;
            }
            x3::logInfo("  [choices]");
            for (size_t k = 0; k < ch.size(); ++k)
                x3::logInfo("    " + std::to_string(k) + ") " + ch[k].text);
            d->choose(0);   // auto-advance down the first branch
        }

        bool ok = npc.beginCount() >= 1 && npc.beginCount() == npc.endCount() && !d->active();
        x3::logInfo("--demo-dialog: lines spoken = " + std::to_string(npc.beginCount()) +
                    ", speaking enter==exit = " + (npc.beginCount() == npc.endCount() ? "yes" : "NO") +
                    ", conversation ended = " + (!d->active() ? "yes" : "NO"));
        if (haveModel && npc.skinnable()) {
            // The talk-bob must have actually moved the skeleton while speaking.
            bool moved = npc.maxPaletteDelta() > 1e-4f;
            x3::logInfo("--demo-dialog: talk-bob palette max-delta = " +
                        std::to_string(npc.maxPaletteDelta()) + (moved ? " (animated)" : " (STATIC!)"));
            ok = ok && moved;
        }
        if (loader && haveModel) loader->unload(model);
        x3::logInfo(std::string("--demo-dialog: ") + (ok ? "PASS" : "FAIL"));
        return ok ? 0 : 1;
    }
    if (testValley) {
        x3::logInfo("running Crystal Valleys (Act 2, L15) self-test "
                    "(terrain placement + crash/K'thara on surface + Dominion + water)...");
        return x3::game::runValleySelfTest() ? 0 : 1;
    }
    if (testCliffs) {
        x3::logInfo("running Salvari cliffs finale self-test (pad/sea/placement/streaming)...");
        return x3::game::runCliffsSelfTest() ? 0 : 1;
    }
    if (testClub) {
        x3::logInfo("running Club 1127 (\"THE DEEP\") self-test "
                    "(build at Y=-200; assert DJ booth/ORB/bars/stair/PA/blacklights/TVs/footprint; leak-clean)...");
        return x3::game::runClubSelfTest() ? 0 : 1;
    }

    x3::logInfo("X3Engine starting...");

    // HEADLESS / OFFSCREEN routing: the non-interactive verification + screenshot
    // paths (--smoketest, --screenshot, --screenshot-sky, --screenshot-terrain)
    // render fully offscreen — NO GLFW window, NO surface, NO swapchain, nothing
    // shown on screen. Everything a human actually watches (no-arg game,
    // --world terrain, --bench) keeps a real window + swapchain exactly as before.
    const bool headless = smoketest || screenshot || skyShot || terrainShot || oceanShot || captureAi || captureWalk || destructShot || captureFootIk || uiDemo || captureSpire;

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

    // ---- Render device ----
    std::unique_ptr<x3::rhi::IRenderDevice> device(x3::rhi::createRenderDevice());

    x3::rhi::DeviceDesc desc{};
    desc.nativeWindowHandle = window ? glfwGetWin32Window(window) : nullptr;
    desc.width  = W;
    desc.height = H;
    desc.headless = headless;
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

    // ---- Destruction demo (--world destruct / --screenshot-destruct) -------
    // The K-T1 marquee showcase: a lit ground + a row of destructible crates the
    // player can SHOOT (left mouse -> weapon ray -> DestructibleManager::applyHit)
    // or BLOW UP (E -> applyRadialImpulse). Each crate is one intact dynamic
    // compound body; on a break above threshold it shatters into convex chunks with
    // split linear+angular velocity (the K-T1 fracture). Chunks render straight from
    // the manager's live transforms so they visibly tumble. Self-contained world
    // (DestructDemo, app/destruct_demo.h) — low-conflict with Level 1.
    if (worldMode == "destruct" || destructShot) {
        x3::logInfo("--world destruct: building the destructible-crate showcase");
        std::unique_ptr<x3::phys::IPhysicsWorld> dphys(x3::phys::createPhysicsWorld());
        if (!dphys->init()) {
            x3::logError("--world destruct: physics init failed");
            device->shutdown();
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return 1;
        }
        x3::game::DestructDemo demo;
        demo.build(*device, *dphys, /*numCrates*/4);

        // Outdoor lighting: turn the analytic sky on (backdrop + sun disk) and add
        // bright fill point lights ALONG the crate row + on the camera side so the
        // crate faces toward the vantage + the scattered chunks read clearly (the
        // built-in directional sun comes from +X+Y+Z, so the camera-side faces need
        // fill). Lights span the row at x = -3..4.5, z = 0.
        { x3::rhi::IRenderDevice::SkyParams sp{}; sp.enabled = true; sp.sunIntensity = 1.4f; sp.haze = 0.35f;
          device->setSkyParams(sp); }
        { x3::rhi::PointLight pl[4];
          // Two strong fills on the CAMERA side (-X/+Z) lighting the faces we see.
          pl[0].pos[0]=-2.0f; pl[0].pos[1]=2.5f; pl[0].pos[2]=3.0f; pl[0].range=14.0f;
          pl[0].color[0]=5.0f; pl[0].color[1]=5.0f; pl[0].color[2]=5.4f;
          pl[1].pos[0]= 3.0f; pl[1].pos[1]=2.5f; pl[1].pos[2]=3.0f; pl[1].range=14.0f;
          pl[1].color[0]=5.0f; pl[1].color[1]=4.6f; pl[1].color[2]=4.0f;
          // Two overhead lights so the tumbling chunks catch light from above.
          pl[2].pos[0]=-1.0f; pl[2].pos[1]=4.0f; pl[2].pos[2]=0.0f; pl[2].range=12.0f;
          pl[2].color[0]=3.5f; pl[2].color[1]=3.5f; pl[2].color[2]=3.5f;
          pl[3].pos[0]= 4.0f; pl[3].pos[1]=4.0f; pl[3].pos[2]=0.0f; pl[3].range=12.0f;
          pl[3].color[0]=3.5f; pl[3].color[1]=3.5f; pl[3].color[2]=3.5f;
          device->setPointLights(pl, 4); }

        const float dt = 1.0f / 60.0f;

        // ===== Headless capture: shoot + explode the crates, settle, grab. ======
        if (headless) {
            // Vantage: close + low, framing the crate row (crates span x=-3..4.5 at
            // z=0, y=0.5) from off to the camera side so the intact crates (left) and
            // the freshly shattered, mid-air tumbling chunks (right) both read big.
            float cam[5] = { -5.5f, 1.8f, 3.2f, -0.46f, -0.10f };
            if (shotCamOverride) for (int k = 0; k < 5; ++k) cam[k] = shotCam[k];
            device->setCamera(cam[0], cam[1], cam[2], cam[3], cam[4], 70.0f);
            const std::string outPath = destructShot ? destructShotPath
                                       : (screenshot ? screenshotPath : destructShotPath);

            // ADDITIVE GPU-compute debris layer (K-T2): wire the cheap, large-scale
            // GPU rubble onto the SAME fracture/explosion events that drive the Jolt
            // chunks (the Jolt chunk path is untouched). Each break ALSO emits a GPU
            // debris burst at the impact, simulated + drawn entirely on the GPU. This
            // proves the compute path in the real windowed/screenshot render loop.
            { x3::rhi::IRenderDevice::GpuDebrisParams gp{};
              gp.groundY = 0.0f; gp.restitution = 0.2f; gp.friction = 0.5f;
              gp.linearDamping = 0.3f; gp.sleepFrames = 16;
              device->gpuDebrisConfig(gp); }
            const float debrisTint[4] = { 0.78f, 0.55f, 0.36f, 1.0f };

            // Break the RIGHT crates (3rd + 4th) so the left two stay intact for the
            // before/after contrast, and capture while the chunks are still scattering
            // (modest kicks so the debris stays in frame, not launched to the horizon).
            const int kSettle = 30;       // ~0.5 s after the break: chunks mid-air, near the crates
            for (int i = 0; i < kSettle; ++i) {
                glfwPollEvents();
                // Frame 4: shoot the 3rd crate from the left along +X.
                if (i == 4 && demo.crates().size() >= 3) {
                    const auto& cr = demo.crates();
                    float eye[3] = { cr[2].center[0] - 3.0f, cr[2].center[1], cr[2].center[2] };
                    float dir[3] = { 1.0f, 0.0f, 0.0f };
                    demo.fire(eye, dir, 45.0f);
                    // Additive GPU rubble burst at the crate (hundreds of cheap fragments).
                    float bp[3] = { cr[2].center[0], cr[2].center[1], cr[2].center[2] };
                    device->gpuDebrisSpawnBurst(bp, 600, 4.0f, 6.0f, 0.06f, 0xC0FFEEu);
                }
                // Frame 8: blow up the rightmost crate with an explosion right under it.
                if (i == 8 && demo.crates().size() >= 4) {
                    const auto& cr = demo.crates();
                    float center[3] = { cr[cr.size()-1].center[0], 0.5f, 0.0f };
                    demo.explode(center, 3.0f, 28.0f);
                    float bp[3] = { center[0], 0.6f, center[2] };
                    device->gpuDebrisSpawnBurst(bp, 800, 6.0f, 6.0f, 0.06f, 0x1234567u);
                }
                dphys->step(dt);
                demo.update(dt);
                device->setCamera(cam[0], cam[1], cam[2], cam[3], cam[4], 70.0f);
                if (i == kSettle - 1) device->armCapture(outPath.c_str());
                auto frame = device->beginFrame();
                device->gpuDebrisStep(dt);            // GPU compute integrate
                if (frame.valid) demo.render(frame);  // Jolt chunks (existing path)
                if (frame.valid) device->gpuDebrisDraw(frame, debrisTint); // GPU rubble
                device->endFrame(frame);
            }
            const bool wrote = device->captureFrame(outPath.c_str());
            if (wrote) x3::logInfo("--screenshot-destruct: wrote " + outPath +
                                   " (Jolt debris=" + std::to_string(demo.activeDebris()) +
                                   " GPU debris=" + std::to_string(device->gpuDebrisAliveCount()) + ")");
            else       x3::logError("--screenshot-destruct: capture FAILED");
            demo.shutdown();
            dphys->shutdown();
            device->shutdown();
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return wrote ? 0 : 1;
        }

        // ===== Benchmark with active debris (--world destruct --bench [N] [frames]).
        // Spawn a field of crates, break them all, then run with vsync OFF measuring
        // FPS/CPU/GPU while a large pool of convex chunk bodies simulates + tumbles.
        // Reports the active-debris count so the perf is attributable to destruction.
        if (bench) {
            const uint32_t fieldCrates = stressCount > 0 ? std::min(stressCount, 40u) : 12u;
            // Spawn extra crates in a grid (the build() already made 4 along x).
            for (uint32_t n = 0; n < fieldCrates; ++n) {
                float cx = -6.0f + (float)(n % 8) * 2.2f;
                float cz = -6.0f + (float)(n / 8) * 2.2f;
                // Reuse the demo's shared fracture asset for a fresh destructible per cell.
                x3::phys::DestructibleId id = demo.spawnCrate(cx, 0.5f, cz);
                if (id) { float c[3]={cx,0.5f,cz}; demo.explode(c, 1.5f, 30.0f); }
            }
            dphys->step(dt); demo.update(dt);          // apply the breaks

            const float bx = 0.0f, by = 9.0f, bz = 12.0f, byaw = -1.5708f, bpitch = -0.6f;
            device->setCamera(bx, by, bz, byaw, bpitch, 75.0f);
            const uint32_t warmup = std::min<uint32_t>(60, benchFrames / 4);
            double sumCpu = 0.0, sumGpu = 0.0; uint32_t measured = 0;
            double prevT = glfwGetTime();
            x3::rhi::RenderStats last{};
            for (uint32_t f = 0; f < benchFrames && !glfwWindowShouldClose(window); ++f) {
                glfwPollEvents();
                double nowT = glfwGetTime(); double cpuMs = (nowT - prevT) * 1000.0; prevT = nowT;
                dphys->step(dt); demo.update(dt);
                device->setCamera(bx, by, bz, byaw, bpitch, 75.0f);
                auto frame = device->beginFrame();
                if (frame.valid) demo.render(frame);
                device->endFrame(frame);
                last = device->stats();
                if (f >= warmup) { sumCpu += cpuMs; sumGpu += last.gpuFrameMs; ++measured; }
            }
            const double avgCpu = measured ? sumCpu / measured : 0.0;
            const double avgGpu = measured ? sumGpu / measured : 0.0;
            const double avgFps = (avgCpu > 1e-6) ? (1000.0 / avgCpu) : 0.0;
            char rb[256];
            std::snprintf(rb, sizeof(rb),
                "BENCH-DESTRUCT activeDebris=%u draws=%u tris=%u | FPS=%.1f  CPU=%.3f ms  GPU=%.3f ms  (avg over %u frames)",
                demo.activeDebris(), last.drawCalls, last.triangles, avgFps, avgCpu, avgGpu, measured);
            x3::logInfo(rb);
            demo.shutdown(); dphys->shutdown(); device->shutdown();
            if (window) glfwDestroyWindow(window); glfwTerminate();
            return 0;
        }

        // ===== Walkable windowed path: fly-cam + shoot (LMB) / explode (E). =====
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        double lastMX, lastMY; glfwGetCursorPos(window, &lastMX, &lastMY);
        double prevTime = glfwGetTime();
        float fx = -8.0f, fy = 2.2f, fz = 6.0f, fyaw = -0.6f, fpitch = -0.25f;
        bool prevLMB = false, prevE = false;
        x3::logInfo("--world destruct: fly with WASD + mouse, LMB shoot a crate, E explode, Esc to quit");
        int lastWd = (int)W, lastHd = (int)H;
        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();
            if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;
            double now = glfwGetTime();
            float fdt = (float)(now - prevTime); prevTime = now;
            if (fdt > 0.1f) fdt = 0.1f;
            double mx, my; glfwGetCursorPos(window, &mx, &my);
            float ddx = (float)(mx - lastMX), ddy = (float)(my - lastMY);
            lastMX = mx; lastMY = my;
            auto kd = [&](int k){ return glfwGetKey(window, k) == GLFW_PRESS; };
            const float sens = 0.0025f;
            fyaw += ddx * sens; fpitch -= ddy * sens;
            if (fpitch >  1.55f) fpitch =  1.55f;
            if (fpitch < -1.55f) fpitch = -1.55f;
            float dx = std::cos(fpitch)*std::cos(fyaw), dy = std::sin(fpitch), dz = std::cos(fpitch)*std::sin(fyaw);
            float rl = std::sqrt(dx*dx + dz*dz); if (rl < 1e-4f) rl = 1e-4f;
            float rx = -dz/rl, rz = dx/rl;
            float spd = 6.0f * fdt; if (kd(GLFW_KEY_LEFT_SHIFT)) spd *= 3.0f;
            if (kd(GLFW_KEY_W)) { fx += dx*spd; fy += dy*spd; fz += dz*spd; }
            if (kd(GLFW_KEY_S)) { fx -= dx*spd; fy -= dy*spd; fz -= dz*spd; }
            if (kd(GLFW_KEY_D)) { fx += rx*spd; fz += rz*spd; }
            if (kd(GLFW_KEY_A)) { fx -= rx*spd; fz -= rz*spd; }
            if (kd(GLFW_KEY_SPACE)) fy += spd;
            if (kd(GLFW_KEY_LEFT_CONTROL)) fy -= spd;
            // Shoot: ray from the eye along the look dir.
            bool lmb = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
            if (lmb && !prevLMB) { float eye[3]={fx,fy,fz}, dir[3]={dx,dy,dz}; demo.fire(eye, dir, 70.0f); }
            prevLMB = lmb;
            bool eNow = kd(GLFW_KEY_E);
            if (eNow && !prevE) { float c[3]={fx+dx*4.0f, fy+dy*4.0f, fz+dz*4.0f}; demo.explode(c, 5.0f, 45.0f); }
            prevE = eNow;

            dphys->step(fdt);
            demo.update(fdt);

            int cw, ch; glfwGetFramebufferSize(window, &cw, &ch);
            if (cw != lastWd || ch != lastHd) { lastWd=cw; lastHd=ch; if (cw>0&&ch>0) device->onResize((uint32_t)cw,(uint32_t)ch); }
            device->setCamera(fx, fy, fz, fyaw, fpitch, 65.0f);
            auto frame = device->beginFrame();
            if (frame.valid) demo.render(frame);
            device->endFrame(frame);
        }
        demo.shutdown();
        dphys->shutdown();
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return 0;
    }

    // ======================================================================
    // ---- Physics §1 demo (--world physjoint) ------------------------------
    // A row of cubes each hung from a fixed anchor above by a Jolt PointConstraint
    // so they hang + swing like pendulums under gravity. Fly into them (or, in the
    // headless capture, a scripted sweep) imparts a sideways impulse and they swing;
    // damping settles them. Self-contained world built on the public IPhysicsWorld
    // constraint API. Headless `--world physjoint --screenshot <path>` captures a
    // still mid-swing. Jolt (MIT) only.
    if (worldMode == "physjoint") {
        x3::logInfo("--world physjoint: building the suspended swinging-cube row");
        std::unique_ptr<x3::phys::IPhysicsWorld> pjphys(x3::phys::createPhysicsWorld());
        if (!pjphys->init()) {
            x3::logError("--world physjoint: physics init failed");
            device->shutdown(); if (window) glfwDestroyWindow(window); glfwTerminate(); return 1;
        }
        // Shared cube mesh + textures + a lit ground.
        std::vector<x3::rhi::MeshVertex> cv; std::vector<uint32_t> ci;
        x3::prims::makeCube(0.5f, cv, ci);
        auto cubeMesh = device->createMesh(cv.data(), (uint32_t)cv.size(), ci.data(), (uint32_t)ci.size());
        auto cubeTexD = x3::prims::makeCheckerRGBA(64, 8, 200, 120, 90, 150, 80, 60);
        auto cubeTex  = device->createTexture(cubeTexD.data(), 64, 64, true);
        auto grTexD = x3::prims::makeCheckerRGBA(64, 8, 150, 150, 160, 60, 62, 74);
        auto grTex  = device->createTexture(grTexD.data(), 64, 64, true);
        x3::prims::PrimMesh g = x3::prims::makeBox(12.0f, 0.25f, 12.0f, 0.0f, -0.25f, 0.0f, 0.25f);
        auto grMesh = device->createMesh(g.verts.data(), (uint32_t)g.verts.size(),
                                         g.index.data(), (uint32_t)g.index.size());
        pjphys->addStaticMesh(g.cverts.data(), (uint32_t)(g.cverts.size()/3),
                              g.cindex.data(), (uint32_t)g.cindex.size());

        // A row of N cubes hung from anchors at y=4. Each cube's TOP is pinned to its
        // anchor so it swings as a pendulum about the pin.
        const int N = 5;
        const float anchorY = 4.0f, half = 0.4f;
        struct Hung { x3::phys::BodyId body; float ax, az; };
        std::vector<Hung> hung;
        for (int i = 0; i < N; ++i) {
            float ax = -4.0f + i * 2.0f, az = 0.0f;
            x3::phys::Vec3 center{ ax, anchorY - 1.2f, az };  // hang 1.2 m below the anchor
            x3::phys::BodyId b = pjphys->addBox(x3::phys::Vec3{half,half,half}, center, 4.0f, x3::phys::Layer::Dynamic);
            x3::phys::Vec3 anchor{ ax, anchorY, az };
            x3::phys::Vec3 attach{ ax, center.y + half, az };  // a point near the top of the cube
            pjphys->addPointConstraint(b, anchor, attach);
            pjphys->setBodyDamping(b, 0.15f, 0.15f);
            hung.push_back({ b, ax, az });
        }
        pjphys->optimizeBroadphase();

        { x3::rhi::IRenderDevice::SkyParams sp{}; sp.enabled = true; sp.sunIntensity = 1.3f; sp.haze = 0.3f;
          device->setSkyParams(sp); }
        { x3::rhi::PointLight pl[2];
          pl[0].pos[0]=0; pl[0].pos[1]=4.0f; pl[0].pos[2]=5.0f; pl[0].range=20.0f;
          pl[0].color[0]=5.0f; pl[0].color[1]=4.8f; pl[0].color[2]=4.4f;
          pl[1].pos[0]=0; pl[1].pos[1]=5.0f; pl[1].pos[2]=-3.0f; pl[1].range=18.0f;
          pl[1].color[0]=3.0f; pl[1].color[1]=3.0f; pl[1].color[2]=3.2f;
          device->setPointLights(pl, 2); }

        const float dt = 1.0f / 60.0f;
        auto drawScene = [&](const x3::rhi::FrameContext& frame) {
            const float white[4] = {1,1,1,1};
            const float idG[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
            device->drawMesh(frame, grMesh, grTex, white, idG);
            const float cubeCol[4] = { 0.95f, 0.8f, 0.7f, 1.0f };
            for (const auto& h : hung) {
                x3::phys::Vec3 p = pjphys->getBodyPosition(h.body);
                float q[4]; pjphys->getBodyRotation(h.body, q);
                // Compose a TRS matrix (quat -> 3x3, then scale to the cube size).
                float x=q[0],y=q[1],z=q[2],w=q[3];
                float m[16] = {
                    (1-2*(y*y+z*z)), (2*(x*y+z*w)),   (2*(x*z-y*w)),   0,
                    (2*(x*y-z*w)),   (1-2*(x*x+z*z)), (2*(y*z+x*w)),   0,
                    (2*(x*z+y*w)),   (2*(y*z-x*w)),   (1-2*(x*x+y*y)), 0,
                    p.x, p.y, p.z, 1 };
                const float s = half * 2.0f / 0.5f;
                m[0]*=s;m[1]*=s;m[2]*=s; m[4]*=s;m[5]*=s;m[6]*=s; m[8]*=s;m[9]*=s;m[10]*=s;
                device->drawMesh(frame, cubeMesh, cubeTex, cubeCol, m);
            }
        };

        // ===== Headless capture: push the row, capture mid-swing. =====
        if (headless) {
            float cam[5] = { 0.0f, 3.0f, 9.0f, -1.5708f, -0.18f };
            if (shotCamOverride) for (int k = 0; k < 5; ++k) cam[k] = shotCam[k];
            const std::string outPath = screenshot ? screenshotPath : std::string("G:/X3Native/captures/physjoint.png");
            const int kFrames = 45;
            for (int i = 0; i < kFrames; ++i) {
                glfwPollEvents();
                if (i == 3) for (auto& h : hung) pjphys->applyImpulse(h.body, x3::phys::Vec3{ 18.0f, 0, 0 });
                pjphys->step(dt);
                device->setCamera(cam[0], cam[1], cam[2], cam[3], cam[4], 65.0f);
                if (i == kFrames - 1) device->armCapture(outPath.c_str());
                auto frame = device->beginFrame();
                if (frame.valid) drawScene(frame);
                device->endFrame(frame);
            }
            const bool wrote = device->captureFrame(outPath.c_str());
            if (wrote) x3::logInfo("--world physjoint: wrote " + outPath);
            else       x3::logError("--world physjoint: capture FAILED");
            device->destroyMesh(cubeMesh); device->destroyMesh(grMesh);
            device->destroyTexture(cubeTex); device->destroyTexture(grTex);
            pjphys->shutdown(); device->shutdown();
            if (window) glfwDestroyWindow(window); glfwTerminate();
            return wrote ? 0 : 1;
        }

        // ===== Walkable windowed path: fly-cam; Space pushes the whole row. =====
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        double lastMX, lastMY; glfwGetCursorPos(window, &lastMX, &lastMY);
        double prevTime = glfwGetTime();
        float fx = 0.0f, fy = 2.5f, fz = 9.0f, fyaw = -1.5708f, fpitch = -0.1f;
        bool prevSpace = false;
        x3::logInfo("--world physjoint: fly WASD + mouse, Space to push the cubes, Esc to quit");
        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();
            if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;
            double now = glfwGetTime(); float fdt = (float)(now - prevTime); prevTime = now;
            if (fdt > 0.1f) fdt = 0.1f;
            double mx, my; glfwGetCursorPos(window, &mx, &my);
            float ddx=(float)(mx-lastMX), ddy=(float)(my-lastMY); lastMX=mx; lastMY=my;
            auto kd = [&](int k){ return glfwGetKey(window, k) == GLFW_PRESS; };
            fyaw += ddx*0.0025f; fpitch -= ddy*0.0025f;
            if (fpitch> 1.55f) fpitch= 1.55f; if (fpitch<-1.55f) fpitch=-1.55f;
            float dx=std::cos(fpitch)*std::cos(fyaw), dy=std::sin(fpitch), dz=std::cos(fpitch)*std::sin(fyaw);
            float rl=std::sqrt(dx*dx+dz*dz); if (rl<1e-4f) rl=1e-4f;
            float rx=-dz/rl, rz=dx/rl; float spd=6.0f*fdt; if (kd(GLFW_KEY_LEFT_SHIFT)) spd*=3.0f;
            if (kd(GLFW_KEY_W)){fx+=dx*spd;fy+=dy*spd;fz+=dz*spd;}
            if (kd(GLFW_KEY_S)){fx-=dx*spd;fy-=dy*spd;fz-=dz*spd;}
            if (kd(GLFW_KEY_D)){fx+=rx*spd;fz+=rz*spd;}
            if (kd(GLFW_KEY_A)){fx-=rx*spd;fz-=rz*spd;}
            bool sp = kd(GLFW_KEY_SPACE);
            if (sp && !prevSpace) for (auto& h : hung) pjphys->applyImpulse(h.body, x3::phys::Vec3{ 18.0f, 0, 0 });
            prevSpace = sp;
            pjphys->step(fdt);
            int cw, chh; glfwGetFramebufferSize(window, &cw, &chh);
            if (cw>0&&chh>0) device->onResize((uint32_t)cw,(uint32_t)chh);
            device->setCamera(fx, fy, fz, fyaw, fpitch, 65.0f);
            auto frame = device->beginFrame();
            if (frame.valid) drawScene(frame);
            device->endFrame(frame);
        }
        device->destroyMesh(cubeMesh); device->destroyMesh(grMesh);
        device->destroyTexture(cubeTex); device->destroyTexture(grTex);
        pjphys->shutdown(); device->shutdown();
        if (window) glfwDestroyWindow(window); glfwTerminate();
        return 0;
    }

    // ---- Physics §2 demo (--world ragdoll) --------------------------------
    // A humanoid built from the canonical 11-bone Jolt ragdoll rig stands on a lit
    // ground; press R to RAGDOLL (the Jolt ragdoll takes over and it collapses
    // naturally), T to nudge it again. Each bone is drawn as a scaled box at its
    // physics world transform. Headless `--world ragdoll --screenshot <path>`
    // triggers the ragdoll + captures the mid-collapse still. Jolt (MIT) only.
    if (worldMode == "ragdoll") {
        x3::logInfo("--world ragdoll: building the ragdoll demo character");
        std::unique_ptr<x3::phys::IPhysicsWorld> rphys(x3::phys::createPhysicsWorld());
        if (!rphys->init()) {
            x3::logError("--world ragdoll: physics init failed");
            device->shutdown(); if (window) glfwDestroyWindow(window); glfwTerminate(); return 1;
        }
        x3::game::RagdollDemo demo;
        demo.build(*device, *rphys);

        { x3::rhi::IRenderDevice::SkyParams sp{}; sp.enabled = true; sp.sunIntensity = 1.3f; sp.haze = 0.3f;
          device->setSkyParams(sp); }
        { x3::rhi::PointLight pl[2];
          pl[0].pos[0]=2.0f; pl[0].pos[1]=3.0f; pl[0].pos[2]=4.0f; pl[0].range=16.0f;
          pl[0].color[0]=5.0f; pl[0].color[1]=4.8f; pl[0].color[2]=4.4f;
          pl[1].pos[0]=-2.0f; pl[1].pos[1]=3.0f; pl[1].pos[2]=2.0f; pl[1].range=16.0f;
          pl[1].color[0]=3.2f; pl[1].color[1]=3.2f; pl[1].color[2]=3.5f;
          device->setPointLights(pl, 2); }

        const float dt = 1.0f / 60.0f;

        // ===== Headless capture: trigger the ragdoll, let it collapse, capture. =====
        if (headless) {
            float cam[5] = { 0.0f, 1.2f, 3.2f, -1.5708f, -0.10f };
            if (shotCamOverride) for (int k = 0; k < 5; ++k) cam[k] = shotCam[k];
            const std::string outPath = screenshot ? screenshotPath : std::string("G:/X3Native/captures/ragdoll.png");
            const int kFrames = 60;   // ~1 s into the collapse
            for (int i = 0; i < kFrames; ++i) {
                glfwPollEvents();
                if (i == 3) demo.ragdollize();
                rphys->step(dt);
                device->setCamera(cam[0], cam[1], cam[2], cam[3], cam[4], 60.0f);
                if (i == kFrames - 1) device->armCapture(outPath.c_str());
                auto frame = device->beginFrame();
                if (frame.valid) demo.render(frame);
                device->endFrame(frame);
            }
            const bool wrote = device->captureFrame(outPath.c_str());
            if (wrote) x3::logInfo("--world ragdoll: wrote " + outPath);
            else       x3::logError("--world ragdoll: capture FAILED");
            demo.shutdown(); rphys->shutdown(); device->shutdown();
            if (window) glfwDestroyWindow(window); glfwTerminate();
            return wrote ? 0 : 1;
        }

        // ===== Walkable windowed path: fly-cam; R ragdolls, T nudges. =====
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        double lastMX, lastMY; glfwGetCursorPos(window, &lastMX, &lastMY);
        double prevTime = glfwGetTime();
        float fx = 0.0f, fy = 1.2f, fz = 3.5f, fyaw = -1.5708f, fpitch = -0.05f;
        bool prevR = false, prevT = false;
        x3::logInfo("--world ragdoll: fly WASD + mouse, R to ragdoll, T to nudge, Esc to quit");
        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();
            if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;
            double now = glfwGetTime(); float fdt = (float)(now - prevTime); prevTime = now;
            if (fdt > 0.1f) fdt = 0.1f;
            double mx, my; glfwGetCursorPos(window, &mx, &my);
            float ddx=(float)(mx-lastMX), ddy=(float)(my-lastMY); lastMX=mx; lastMY=my;
            auto kd = [&](int k){ return glfwGetKey(window, k) == GLFW_PRESS; };
            fyaw += ddx*0.0025f; fpitch -= ddy*0.0025f;
            if (fpitch> 1.55f) fpitch= 1.55f; if (fpitch<-1.55f) fpitch=-1.55f;
            float dx=std::cos(fpitch)*std::cos(fyaw), dy=std::sin(fpitch), dz=std::cos(fpitch)*std::sin(fyaw);
            float rl=std::sqrt(dx*dx+dz*dz); if (rl<1e-4f) rl=1e-4f;
            float rx=-dz/rl, rz=dx/rl; float spd=5.0f*fdt; if (kd(GLFW_KEY_LEFT_SHIFT)) spd*=3.0f;
            if (kd(GLFW_KEY_W)){fx+=dx*spd;fy+=dy*spd;fz+=dz*spd;}
            if (kd(GLFW_KEY_S)){fx-=dx*spd;fy-=dy*spd;fz-=dz*spd;}
            if (kd(GLFW_KEY_D)){fx+=rx*spd;fz+=rz*spd;}
            if (kd(GLFW_KEY_A)){fx-=rx*spd;fz-=rz*spd;}
            bool rNow = kd(GLFW_KEY_R);
            if (rNow && !prevR) demo.ragdollize();
            prevR = rNow;
            bool tNow = kd(GLFW_KEY_T);
            if (tNow && !prevT && demo.ragdoll()) demo.ragdoll()->applyImpulseAll(x3::phys::Vec3{ 2.0f, 1.0f, 0 });
            prevT = tNow;
            rphys->step(fdt);
            int cw, chh; glfwGetFramebufferSize(window, &cw, &chh);
            if (cw>0&&chh>0) device->onResize((uint32_t)cw,(uint32_t)chh);
            device->setCamera(fx, fy, fz, fyaw, fpitch, 60.0f);
            auto frame = device->beginFrame();
            if (frame.valid) demo.render(frame);
            device->endFrame(frame);
        }
        demo.shutdown(); rphys->shutdown(); device->shutdown();
        if (window) glfwDestroyWindow(window); glfwTerminate();
        return 0;
    }

    // ======================================================================
    // ---- VEHICLE FRAMEWORK demos (--world drive / boat / fly) -------------
    // GENERAL vehicle/flight/buoyancy framework (engine/physics/IVehicle.h) on
    // Jolt. Each demo = a dynamic rigid body + one IVehicleController:
    //   * drive : a wheeled car (Jolt VehicleConstraint) on the STREAMED terrain
    //             — WASD throttle/brake/steer, Space handbrake, chase cam.
    //   * boat  : a buoyant hull floating on a flat ocean (water plane) — the
    //             buoyancy controller settles it at the waterline; WASD motors it.
    //   * fly   : an aircraft (thrust + lift + drag + pitch/yaw/roll) — W/S throttle,
    //             arrows/mouse attitude. Same framework, simple force model.
    // Self-contained worlds (app/vehicle.*); low-conflict with Level 1. Headless
    // `--world drive --screenshot <path>` / `--world boat --screenshot <path>`
    // capture the gate stills. This is a small, clearly-marked flag block.
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
        if (isDrive) x3::logInfo("--world drive: W/S throttle, A/D steer, Space handbrake, mouse orbits, Esc quit");
        else if (isBoat) x3::logInfo("--world boat: W/S motor, A/D steer, mouse orbits, Esc quit");
        else x3::logInfo("--world fly: W/S throttle, A/D yaw, Up/Down pitch, Q/E roll, mouse orbits, Esc quit");
        int lastWd = (int)W, lastHd = (int)H;
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

            x3::phys::VehicleInput in;
            if (isDrive || isBoat) {
                in.throttle = (kd(GLFW_KEY_W)?1.0f:0.0f) - (kd(GLFW_KEY_S)?1.0f:0.0f);
                in.steer    = (kd(GLFW_KEY_D)?1.0f:0.0f) - (kd(GLFW_KEY_A)?1.0f:0.0f);
                if (isDrive) {
                    if (kd(GLFW_KEY_SPACE)) in.handBrake = 1.0f;
                    if (in.throttle < 0.0f && car.forwardSpeed() > 0.5f) { in.brake = 1.0f; in.throttle = 0.0f; }
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

            // Orbit/chase camera around the vehicle.
            float vp[3]; vpos(vp);
            float dist = isFly ? 16.0f : 10.0f, height = isFly ? 4.0f : 3.5f;
            float cx = vp[0] - std::cos(camPitch)*std::cos(camYaw)*dist;
            float cy = vp[1] + height - std::sin(camPitch)*dist;
            float cz = vp[2] - std::cos(camPitch)*std::sin(camYaw)*dist;
            int cw, ch; glfwGetFramebufferSize(window, &cw, &ch);
            if (cw != lastWd || ch != lastHd) { lastWd=cw; lastHd=ch; if (cw>0&&ch>0) device->onResize((uint32_t)cw,(uint32_t)ch); }
            device->setCamera(cx, cy, cz, camYaw, camPitch, 70.0f);
            auto frame = device->beginFrame();
            if (frame.valid) vrender(frame);
            device->endFrame(frame);
        }
        if (isDrive) { car.shutdown(); vstream.shutdown(vscene, *device, *vphys); }
        else if (isBoat) boat.shutdown(); else plane.shutdown();
        vphys->shutdown(); device->shutdown();
        if (window) glfwDestroyWindow(window); glfwTerminate();
        return 0;
    }

    // ---- Club 1127 + cave/tunnel network (--world club) --------------------
    // A NEW self-contained area (the hidden neon HUB + flooded caves, lore code
    // 1127). Built entirely through the public Scene/device/physics API by
    // Club1127World (app/club1127.*) so it stays LOW-CONFLICT with Level 1 / the
    // Spire. Two ways in:
    //   * WALKABLE (windowed): `--world club` — WASD / mouse-look / Space jump /
    //     F noclip, exactly the Level-1 walking controller + physics.
    //   * SCREENSHOT (headless): `--world club --screenshot <path>` — pose the
    //     showcase camera, settle a few frames (so the characters skin + the bloom
    //     registers), capture the PNG, exit.
    //
    // CODE-1127 HOOK POINT (Spire link, intentionally not fully wired to avoid a
    // level1.cpp conflict): the Spire's keypad already accepts 1127 (see
    // level1_game.cpp tryDoorCode + the codeMode block in this loop). To make the
    // in-game secret entry land here, on a successful 1127 at the *secret club*
    // keypad the host would build a Club1127World + teleport the player to
    // club.spawn() instead of opening Door C. The `--world club` flag below is the
    // standalone build/verify path for that same area.
    if (worldMode == "club") {
        x3::logInfo("--world club: building the full Club 1127 (\"THE DEEP\") at Y=-200");

        // Physics world for the club area (separate from the Level-1 path below).
        std::unique_ptr<x3::phys::IPhysicsWorld> cphys(x3::phys::createPhysicsWorld());
        if (!cphys->init()) {
            x3::logError("--world club: physics init failed");
            device->shutdown();
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return 1;
        }

        x3::game::Scene cscene;
        x3::game::Club1127World club;
        club.build(cscene, *device, *cphys, x3::game::riggedGlbRoot());

        // Apply the neon/UV point-light set once (the orbiting spot/ring lights are
        // re-pushed each frame by club.update()). The club has NO sky (deep interior).
        const auto& clights = club.pointLights();
        device->setPointLights(clights.data(), (uint32_t)clights.size());
        { x3::rhi::IRenderDevice::SkyParams sp{}; sp.enabled = false; device->setSkyParams(sp); }

        const x3::phys::Vec3 spawn = club.spawn();

        // ===== Headless screenshot path: pose the showcase camera, settle, grab. =
        if (headless) {
            float cam[5]; club.showcaseCamera(cam);
            // Allow an explicit --shot-cam x,y,z,yaw,pitch override (handy for
            // capturing the caves/boss arena from a custom vantage during verify).
            if (shotCamOverride) for (int k = 0; k < 5; ++k) cam[k] = shotCam[k];
            device->setCamera(cam[0], cam[1], cam[2], cam[3], cam[4], 60.0f);
            const int kSettle = 24;   // advance enough for character skinning + bloom
            const float dt = 1.0f / 60.0f;
            const std::string outPath = screenshot ? screenshotPath
                                                   : std::string("C:/GameDev/X3Native-engine/agent_club.png");
            for (int i = 0; i < kSettle; ++i) {
                glfwPollEvents();
                club.update(dt, cscene, *device, *cphys);   // ORB spin + spotlight orbit + blacklight pulse + idle props
                cphys->step(dt);
                cscene.update(*cphys);
                // Re-pose each frame (scene.update doesn't move the camera).
                device->setCamera(cam[0], cam[1], cam[2], cam[3], cam[4], 60.0f);
                if (i == kSettle - 1) device->armCapture(outPath.c_str());
                auto frame = device->beginFrame();
                if (frame.valid) {
                    cscene.render(*device, frame);
                    club.drawCharacters(*device, frame, cscene);
                }
                device->endFrame(frame);
            }
            const bool wrote = device->captureFrame(outPath.c_str());
            if (wrote) x3::logInfo("--world club: wrote screenshot " + outPath);
            else       x3::logError("--world club: capture FAILED");
            cphys->shutdown();
            device->shutdown();
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return wrote ? 0 : 1;
        }

        // ===== Walkable windowed path: full first-person controller + physics. ===
        x3::game::Player cplayer;
        cplayer.spawn(*cphys, spawn.x, spawn.y, spawn.z);

        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        double lastMX, lastMY; glfwGetCursorPos(window, &lastMX, &lastMY);
        double prevTime = glfwGetTime();
        bool prevSpaceC = false, prevFC = false;
        bool noclipC = false;
        float flyXc = spawn.x, flyYc = spawn.y + 1.6f, flyZc = spawn.z, flyYawC = 3.14159f, flyPitchC = -0.2f;
        x3::logInfo("--world club: walk THE DEEP at Y=-200 — WASD, mouse look, Space jump, LeftShift sprint, F noclip, Esc to quit");

        int lastWc = (int)W, lastHc = (int)H;
        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();
            if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;

            double now = glfwGetTime();
            float dt = (float)(now - prevTime); prevTime = now;
            if (dt > 0.1f) dt = 0.1f;

            double mx, my; glfwGetCursorPos(window, &mx, &my);
            float ddx = (float)(mx - lastMX), ddy = (float)(my - lastMY);
            lastMX = mx; lastMY = my;

            auto kd = [&](int k) { return glfwGetKey(window, k) == GLFW_PRESS; };
            bool spaceNow = kd(GLFW_KEY_SPACE);
            bool fNow = kd(GLFW_KEY_F);
            if (fNow && !prevFC) {
                noclipC = !noclipC;
                if (noclipC) { float yy, pp; cplayer.camera(flyXc, flyYc, flyZc, yy, pp); flyYawC = yy; flyPitchC = pp; }
            }
            prevFC = fNow;

            float camX, camY, camZ, camYaw, camPitch;
            if (!noclipC) {
                x3::game::PlayerInput in;
                if (kd(GLFW_KEY_W)) in.moveFwd    += 1.0f;
                if (kd(GLFW_KEY_S)) in.moveFwd    -= 1.0f;
                if (kd(GLFW_KEY_D)) in.moveStrafe += 1.0f;
                if (kd(GLFW_KEY_A)) in.moveStrafe -= 1.0f;
                in.sprint      = kd(GLFW_KEY_LEFT_SHIFT);
                in.jumpPressed = spaceNow && !prevSpaceC;
                in.lookDX = ddx; in.lookDY = ddy;
                cplayer.update(in, dt, *cphys);
                club.update(dt, cscene, *device, *cphys);   // ORB spin + spotlight orbit + blacklight pulse + idle props
                cphys->step(dt);
                cscene.update(*cphys);
                cplayer.camera(camX, camY, camZ, camYaw, camPitch);
            } else {
                const float sens = 0.0025f;
                flyYawC += ddx * sens; flyPitchC -= ddy * sens;
                if (flyPitchC >  1.55f) flyPitchC =  1.55f;
                if (flyPitchC < -1.55f) flyPitchC = -1.55f;
                float fx = std::cos(flyPitchC) * std::cos(flyYawC);
                float fy = std::sin(flyPitchC);
                float fz = std::cos(flyPitchC) * std::sin(flyYawC);
                float rl = std::sqrt(fx*fx + fz*fz); if (rl < 1e-4f) rl = 1e-4f;
                float rx = -fz/rl, rz = fx/rl;
                float spd = 6.0f * dt; if (kd(GLFW_KEY_LEFT_SHIFT)) spd *= 3.0f;
                if (kd(GLFW_KEY_W)) { flyXc += fx*spd; flyYc += fy*spd; flyZc += fz*spd; }
                if (kd(GLFW_KEY_S)) { flyXc -= fx*spd; flyYc -= fy*spd; flyZc -= fz*spd; }
                if (kd(GLFW_KEY_D)) { flyXc += rx*spd; flyZc += rz*spd; }
                if (kd(GLFW_KEY_A)) { flyXc -= rx*spd; flyZc -= rz*spd; }
                if (spaceNow) flyYc += spd;
                if (kd(GLFW_KEY_LEFT_CONTROL)) flyYc -= spd;
                club.update(dt, cscene, *device, *cphys);   // ORB spin + spotlight orbit + blacklight pulse + idle props
                cphys->step(dt);
                cscene.update(*cphys);
                camX = flyXc; camY = flyYc; camZ = flyZc; camYaw = flyYawC; camPitch = flyPitchC;
            }
            prevSpaceC = spaceNow;

            int cw, ch; glfwGetFramebufferSize(window, &cw, &ch);
            if (cw != lastWc || ch != lastHc) { lastWc = cw; lastHc = ch; if (cw>0&&ch>0) device->onResize((uint32_t)cw,(uint32_t)ch); }

            device->setCamera(camX, camY, camZ, camYaw, camPitch, 60.0f);
            auto frame = device->beginFrame();
            if (frame.valid) {
                cscene.render(*device, frame);
                club.drawCharacters(*device, frame, cscene);
            }
            device->endFrame(frame);
        }

        cphys->shutdown();
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return 0;
    }

    // ---- Crystal Valleys — Act 2, Level 15 (--world valley) ----------------
    // The FIRST open surface biome of Act 2, AFTER the cliffs finale of Act 1. A NEW
    // self-contained world (app/valley.*), kept LOW-CONFLICT exactly like `--world
    // club`: it does NOT touch level1.cpp / the Spire. It REUSES the streamed terrain
    // path (TerrainStreamer + analytic sky, like `--world terrain`) and places its
    // content — the crashed Salvari ship, K'thara (ally), the Dominion patrol, the
    // crystal formations — ONTO that surface, plus a lake via setWaterParams. Two ways
    // in (mirrors club): WALKABLE (windowed) + SCREENSHOT (headless).
    if (worldMode == "valley") {
        x3::logInfo("--world valley: building Crystal Valleys (Act 2, L15 — open biome)");

        std::unique_ptr<x3::phys::IPhysicsWorld> vphys(x3::phys::createPhysicsWorld());
        if (!vphys->init()) {
            x3::logError("--world valley: physics init failed");
            device->shutdown();
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return 1;
        }

        // Streamed terrain around the valley + analytic sky (same as --world terrain).
        std::unique_ptr<x3::jobs::IJobSystem> vjobs(x3::jobs::createJobSystem());
        vjobs->init(0);
        x3::game::Scene vscene;
        const x3::game::TerrainConfig& vcfg = x3::game::worldTerrainConfig();
        {
            x3::rhi::IRenderDevice::SkyParams sp{};
            sp.enabled = true;
            sp.sunDir[0] = 0.4f; sp.sunDir[1] = 1.0f; sp.sunDir[2] = 0.3f;
            sp.sunColor[0] = 1.0f; sp.sunColor[1] = 0.97f; sp.sunColor[2] = 0.92f;
            sp.sunIntensity = 1.0f; sp.haze = 0.5f; sp.exposure = 1.0f;
            device->setSkyParams(sp);
        }
        x3::game::TerrainStreamer vstream;
        // Seed the residency ring at the origin so the valley content has ground.
        vstream.init(vscene, *device, *vphys, vjobs.get(), vcfg, 0.0f, 0.0f, /*radius=*/8);

        // Build the valley content onto the streamed terrain.
        x3::game::ValleyWorld valley;
        valley.build(vscene, *device, *vphys, x3::game::riggedGlbRoot());

        // Crystal point lights, and the lake water plane.
        const auto& vlights = valley.pointLights();
        device->setPointLights(vlights.data(), (uint32_t)vlights.size());
        const float vSeaLevel = valley.waterSeaLevel();
        auto applyWater = [&](float t) {
            x3::rhi::IRenderDevice::WaterParams wp{};
            wp.enabled = true; wp.seaLevel = vSeaLevel; wp.time = t;
            wp.amplitude = 0.4f; wp.steepness = 0.5f; wp.waveLength = 12.0f; wp.speed = 1.0f;
            wp.deepColor[0] = 0.02f; wp.deepColor[1] = 0.08f; wp.deepColor[2] = 0.12f;
            wp.shallowColor[0] = 0.10f; wp.shallowColor[1] = 0.34f; wp.shallowColor[2] = 0.40f;
            wp.sunDir[0] = 0.4f; wp.sunDir[1] = 1.0f; wp.sunDir[2] = 0.3f;
            wp.specular = 14.0f; wp.fresnel = 0.02f;
            device->setWaterParams(wp);
        };

        const x3::phys::Vec3 vspawn = valley.spawn();

        // ===== Headless screenshot path: pose the showcase camera, settle, grab. =
        if (headless) {
            float cam[5]; valley.showcaseCamera(cam);
            if (shotCamOverride) for (int k = 0; k < 5; ++k) cam[k] = shotCam[k];
            const int kSettle = 48;   // let the ring stream in + characters skin
            const float dt = 1.0f / 60.0f;
            const std::string outPath = screenshot ? screenshotPath
                                                   : std::string("agent_valley.png");
            vstream.setUploadBudget(64);   // fill the visible ring fast for the still
            for (int i = 0; i < kSettle; ++i) {
                glfwPollEvents();
                // Nudge the focus across one tile early to trigger the full ring.
                const float focusX = (i == 1) ? 32.0f : cam[0];
                vstream.update(vscene, *device, *vphys, focusX, cam[2]);
                valley.update(dt, vscene, *vphys, vspawn, nullptr);
                vphys->step(dt);
                vscene.update(*vphys);
                applyWater((float)i * dt);
                device->setCamera(cam[0], cam[1], cam[2], cam[3], cam[4], 60.0f);
                if (i == kSettle - 1) device->armCapture(outPath.c_str());
                auto frame = device->beginFrame();
                if (frame.valid) {
                    vscene.render(*device, frame);
                    valley.drawCharacters(*device, frame, vscene);
                }
                device->endFrame(frame);
            }
            const bool wrote = device->captureFrame(outPath.c_str());
            if (wrote) x3::logInfo("--world valley: wrote screenshot " + outPath);
            else       x3::logError("--world valley: capture FAILED");
            vstream.shutdown(vscene, *device, *vphys);
            vphys->shutdown();
            device->shutdown();
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return wrote ? 0 : 1;
        }

        // ===== Walkable windowed path: full first-person controller + physics. ===
        x3::game::Player vplayer;
        vplayer.spawn(*vphys, vspawn.x, vspawn.y, vspawn.z);

        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        double lastMX, lastMY; glfwGetCursorPos(window, &lastMX, &lastMY);
        double prevTime = glfwGetTime();
        bool prevSpaceV = false, prevFV = false, noclipV = false;
        float flyXv = vspawn.x, flyYv = vspawn.y + 1.6f, flyZv = vspawn.z, flyYawV = 0.0f, flyPitchV = -0.2f;
        float vWaterTime = 0.0f;
        x3::logInfo("--world valley: WASD walk, mouse look, Space jump, LeftShift sprint, F noclip, Esc to quit");

        int lastWv = (int)W, lastHv = (int)H;
        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();
            if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;

            double now = glfwGetTime();
            float dt = (float)(now - prevTime); prevTime = now;
            if (dt > 0.1f) dt = 0.1f;

            double mx, my; glfwGetCursorPos(window, &mx, &my);
            float ddx = (float)(mx - lastMX), ddy = (float)(my - lastMY);
            lastMX = mx; lastMY = my;

            auto kd = [&](int k) { return glfwGetKey(window, k) == GLFW_PRESS; };
            bool spaceNow = kd(GLFW_KEY_SPACE);
            bool fNow = kd(GLFW_KEY_F);
            if (fNow && !prevFV) {
                noclipV = !noclipV;
                if (noclipV) { float yy, pp; vplayer.camera(flyXv, flyYv, flyZv, yy, pp); flyYawV = yy; flyPitchV = pp; }
            }
            prevFV = fNow;

            float camX, camY, camZ, camYaw, camPitch;
            if (!noclipV) {
                x3::game::PlayerInput in;
                if (kd(GLFW_KEY_W)) in.moveFwd    += 1.0f;
                if (kd(GLFW_KEY_S)) in.moveFwd    -= 1.0f;
                if (kd(GLFW_KEY_D)) in.moveStrafe += 1.0f;
                if (kd(GLFW_KEY_A)) in.moveStrafe -= 1.0f;
                in.sprint      = kd(GLFW_KEY_LEFT_SHIFT);
                in.jumpPressed = spaceNow && !prevSpaceV;
                in.lookDX = ddx; in.lookDY = ddy;
                vplayer.update(in, dt, *vphys);
                vplayer.camera(camX, camY, camZ, camYaw, camPitch);
            } else {
                const float sens = 0.0025f;
                flyYawV += ddx * sens; flyPitchV -= ddy * sens;
                if (flyPitchV >  1.55f) flyPitchV =  1.55f;
                if (flyPitchV < -1.55f) flyPitchV = -1.55f;
                float fxv = std::cos(flyPitchV) * std::cos(flyYawV);
                float fyv = std::sin(flyPitchV);
                float fzv = std::cos(flyPitchV) * std::sin(flyYawV);
                float rl = std::sqrt(fxv*fxv + fzv*fzv); if (rl < 1e-4f) rl = 1e-4f;
                float rx = -fzv/rl, rz = fxv/rl;
                float spd = 6.0f * dt; if (kd(GLFW_KEY_LEFT_SHIFT)) spd *= 3.0f;
                if (kd(GLFW_KEY_W)) { flyXv += fxv*spd; flyYv += fyv*spd; flyZv += fzv*spd; }
                if (kd(GLFW_KEY_S)) { flyXv -= fxv*spd; flyYv -= fyv*spd; flyZv -= fzv*spd; }
                if (kd(GLFW_KEY_D)) { flyXv += rx*spd; flyZv += rz*spd; }
                if (kd(GLFW_KEY_A)) { flyXv -= rx*spd; flyZv -= rz*spd; }
                if (spaceNow) flyYv += spd;
                if (kd(GLFW_KEY_LEFT_CONTROL)) flyYv -= spd;
                camX = flyXv; camY = flyYv; camZ = flyZv; camYaw = flyYawV; camPitch = flyPitchV;
            }
            prevSpaceV = spaceNow;

            // Stream terrain around the camera, tick the valley NPCs (hostile chase
            // the player), step physics, sync the scene, animate the lake.
            vstream.update(vscene, *device, *vphys, camX, camZ);
            const x3::phys::Vec3 vp{ camX, camY, camZ };
            valley.update(dt, vscene, *vphys, vp, &vplayer);
            vphys->step(dt);
            vscene.update(*vphys);
            vWaterTime += dt; applyWater(vWaterTime);

            int cw, chh; glfwGetFramebufferSize(window, &cw, &chh);
            if (cw != lastWv || chh != lastHv) { lastWv = cw; lastHv = chh; if (cw>0&&chh>0) device->onResize((uint32_t)cw,(uint32_t)chh); }

            device->setCamera(camX, camY, camZ, camYaw, camPitch, 60.0f);
            auto frame = device->beginFrame();
            if (frame.valid) {
                vscene.render(*device, frame);
                valley.drawCharacters(*device, frame, vscene);
            }
            device->endFrame(frame);
        }

        vstream.shutdown(vscene, *device, *vphys);
        vphys->shutdown();
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return 0;
    }

    // ---- Salvari cliffs finale (--world cliffs) ----------------------------
    // The snowy mountain exterior past the F7 rooftop (Act 1 §2d): the STREAMED
    // terrain heightfield, a flat landing PAD planted on it, the SALVARI SHIP set
    // down on the pad, K'thara + a couple of troopers anchored to the terrain, and
    // the OCEAN well below the cliff-top pad. Self-contained (CliffsArea, app/cliffs.*).
    //   * SCREENSHOT (headless): `--world cliffs --screenshot <path>`.
    //   * WALKABLE (windowed):  `--world cliffs` — fly the cliffs with WASD + mouse.
    if (worldMode == "cliffs") {
        x3::logInfo("--world cliffs: building the above-ground Salvari cliffs finale");
        std::unique_ptr<x3::jobs::IJobSystem> cjobs(x3::jobs::createJobSystem());
        cjobs->init(0);
        std::unique_ptr<x3::phys::IPhysicsWorld> cphys(x3::phys::createPhysicsWorld());
        if (!cphys->init()) {
            x3::logError("--world cliffs: physics init failed");
            device->shutdown();
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return 1;
        }
        x3::game::Scene cscene;
        x3::game::CliffsArea cliffs;
        cliffs.build(cscene, *device, *cphys, cjobs.get());

        const float dt = 1.0f / 60.0f;

        // ===== Headless capture: warm the ring + waves, pose the vantage, grab. ==
        if (headless) {
            const std::string outPath = screenshot ? screenshotPath
                                                   : std::string("w_cliffs.png");
            float eye[3]; float camYaw = 0.0f, camPitch = 0.0f;
            cliffs.suggestCamera(eye, camYaw, camPitch);
            if (shotCamOverride) {
                eye[0]=shotCam[0]; eye[1]=shotCam[1]; eye[2]=shotCam[2];
                camYaw=shotCam[3]; camPitch=shotCam[4];
            }
            const float focusX = cliffs.padCenter()[0], focusZ = cliffs.padCenter()[2];
            const int kFrames = 220;
            for (int i = 0; i < kFrames; ++i) {
                glfwPollEvents();
                cphys->step(dt);
                // The streamer only enqueues the FULL ring on a focus-tile boundary
                // cross (init seeds the 3x3). Nudge the focus across a tile on frame 1
                // to trigger the ring request, then hold it at the pad so the wide
                // resident set drains in over the warmup window.
                const float fX = (i == 1) ? (focusX + 40.0f) : focusX;
                cliffs.update(cscene, *device, *cphys, dt, fX, focusZ);
                device->setCamera(eye[0], eye[1], eye[2], camYaw, camPitch, 70.0f);
                if (i == kFrames - 1) device->armCapture(outPath.c_str());
                auto frame = device->beginFrame();
                if (frame.valid) cliffs.render(*device, frame, cscene);
                device->endFrame(frame);
            }
            const bool wrote = device->captureFrame(outPath.c_str());
            if (wrote) {
                const x3::rhi::RenderStats st = device->stats();
                char rb[256];
                std::snprintf(rb, sizeof(rb),
                    "--world cliffs: wrote %s | seaLevel=%.1f padY=%.1f actors=%u "
                    "resident=%u draws=%u tris=%u ship=%s",
                    outPath.c_str(), cliffs.seaLevel(), cliffs.padCenter()[1],
                    cliffs.actorCount(), cliffs.residentTiles(), st.drawCalls,
                    st.triangles, cliffs.shipReal() ? "REAL" : "fallback");
                x3::logInfo(rb);
            } else x3::logError("--world cliffs: capture FAILED");

            cliffs.shutdown(cscene, *device, *cphys);
            cjobs->shutdown();
            cphys->shutdown();
            device->shutdown();
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return wrote ? 0 : 1;
        }

        // ===== Walkable windowed path: fly-cam over the cliffs. =================
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        double lastMX, lastMY; glfwGetCursorPos(window, &lastMX, &lastMY);
        double prevTime = glfwGetTime();
        float ceye[3]; float fyaw = 0.0f, fpitch = 0.0f;
        cliffs.suggestCamera(ceye, fyaw, fpitch);
        float fx = ceye[0], fy = ceye[1], fz = ceye[2];
        x3::logInfo("--world cliffs: fly with WASD + mouse, Space/Ctrl up-down, Shift sprint, Esc to quit");
        int lastWc = (int)W, lastHc = (int)H;
        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();
            if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;
            double now = glfwGetTime();
            float fdt = (float)(now - prevTime); prevTime = now;
            if (fdt > 0.1f) fdt = 0.1f;
            double mx, my; glfwGetCursorPos(window, &mx, &my);
            float ddx = (float)(mx - lastMX), ddy = (float)(my - lastMY);
            lastMX = mx; lastMY = my;
            auto kd = [&](int k){ return glfwGetKey(window, k) == GLFW_PRESS; };
            const float sens = 0.0025f;
            fyaw += ddx * sens; fpitch -= ddy * sens;
            if (fpitch >  1.55f) fpitch =  1.55f;
            if (fpitch < -1.55f) fpitch = -1.55f;
            float dx = std::cos(fpitch)*std::cos(fyaw), dy = std::sin(fpitch), dz = std::cos(fpitch)*std::sin(fyaw);
            float rl = std::sqrt(dx*dx + dz*dz); if (rl < 1e-4f) rl = 1e-4f;
            float rx = -dz/rl, rz = dx/rl;
            float spd = 8.0f * fdt; if (kd(GLFW_KEY_LEFT_SHIFT)) spd *= 3.0f;
            if (kd(GLFW_KEY_W)) { fx += dx*spd; fy += dy*spd; fz += dz*spd; }
            if (kd(GLFW_KEY_S)) { fx -= dx*spd; fy -= dy*spd; fz -= dz*spd; }
            if (kd(GLFW_KEY_D)) { fx += rx*spd; fz += rz*spd; }
            if (kd(GLFW_KEY_A)) { fx -= rx*spd; fz -= rz*spd; }
            if (kd(GLFW_KEY_SPACE)) fy += spd;
            if (kd(GLFW_KEY_LEFT_CONTROL)) fy -= spd;

            cphys->step(fdt);
            cliffs.update(cscene, *device, *cphys, fdt, fx, fz);

            int cw, ch; glfwGetFramebufferSize(window, &cw, &ch);
            if (cw != lastWc || ch != lastHc) { lastWc=cw; lastHc=ch; if (cw>0&&ch>0) device->onResize((uint32_t)cw,(uint32_t)ch); }
            device->setCamera(fx, fy, fz, fyaw, fpitch, 70.0f);
            auto frame = device->beginFrame();
            if (frame.valid) cliffs.render(*device, frame, cscene);
            device->endFrame(frame);
        }
        cliffs.shutdown(cscene, *device, *cphys);
        cjobs->shutdown();
        cphys->shutdown();
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return 0;
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
    std::unique_ptr<x3::audio::IAudioSystem> audio(x3::audio::createAudioSystem());
    audio->init();
    // Concrete asset picks (see docs/ASSET_INVENTORY.md). Pack-relative paths with
    // graceful fallback: a missing/undecodable file -> invalid handle -> the
    // corresponding event is simply silent (logged once at load).
    const x3::audio::SoundHandle sndGun = audio->load(x3::game::resolveAudio(
        "Sci-Fi_Guns_Game-Of-Weapons/Audio/SFX/Wave/Single_Gunshots/"
        "Single_Gunshot_Sci-Fi_Gun-01.wav"));
    const x3::audio::SoundHandle sndDoor = audio->load(x3::game::resolveAudio(
        "ModularScifiInterior/Sound/S_ScifiDoor_A.WAV"));
    const x3::audio::SoundHandle sndPickup = audio->load(x3::game::resolveAudio(
        "Sci-fi Evolution Gift Pack/Health or Energy Game Recharge 2.wav"));
    const x3::audio::SoundHandle sndDeath = audio->load(x3::game::resolveAudio(
        "Free Pack/Explosion 1.wav"));
    // Footsteps reuse the gunshot WAV pitched down + quiet (no dedicated footstep
    // WAV in the inventory). It reads as a soft step; replace with a real footstep
    // SFX later if one is added to the pack.
    const x3::audio::SoundHandle sndStep = sndGun;
    // Resolved path for the looping music/ambient bed (started after the world is
    // built, below). Spaceship-ambience-style sci-fi action loop.
    const std::string kMusicPath = x3::game::resolveAudio(
        "Sci-Fi Music Pack 1/Loops/SMP1_LOOP_Zero8 _1.wav");

    // ---- Physics world (M3 / Jolt) ----
    std::unique_ptr<x3::phys::IPhysicsWorld> physics(x3::phys::createPhysicsWorld());
    if (!physics->init()) {
        x3::logError("physics world init failed");
        device->shutdown();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
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
    const bool canonWorld = (worldMode == "canonlevel");
    // Hard cap on how many rooms the portal flood-fill may add per frame. Even down the
    // longest sightline with a deep r_culldepth, the cull stays well under the whole 53-room
    // tower so the GPU never spikes (the spec's "must NOT regress to drawing the tower").
    constexpr uint32_t kCanonRoomBudget = 18;
    x3::game::CanonFloor canonFloor;           // parsed+resolved Floor 1 (canonWorld only)
    std::vector<uint32_t> canonVisRooms;       // per-frame PVS scratch (canonWorld only)
    std::vector<x3::game::CanonLight> canonLights; // per-room ceiling lights (canonWorld only)
    x3::game::DoorSystem  canonDoors;          // SM_Door_A GLB doors at the cut doorways
    x3::game::CanonPlay   canonPlay;           // canon Floor-1 gameplay (canonWorld only): sidearm + animated enemies + Martinez + 3 girls
    bool                  canonMedicalActive = false;  // latch: the medical-bay rescue clock was started (player reached the wards)
    x3::game::Scene scene;
    x3::game::Level1Game game;
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
    if (canonWorld) {
        // ---- DATA-DRIVEN CANONICAL FLOOR 1 + per-room PVS cull. ----
        canonFloor = x3::game::loadCanonFloor(x3::game::canonProjectJsonPath(), 1);
        if (canonFloor.valid()) {
            x3::game::CanonBuildOpts copts; copts.doors = &canonDoors;
            x3::game::buildCanonFloor(canonFloor, scene, *device, *physics, copts);
            // Per-room ceiling lights: the data-driven floor skips the env_art Light_A
            // fixtures the legacy level registers, so without these the rooms only get
            // ambient + the flashlight (the DARK bug). We feed only the player's VISIBLE
            // rooms' lights each frame (below) so the active count stays under the cap.
            canonLights = x3::game::buildCanonLights(canonFloor);
            x3::logInfo("--world canonlevel: built canonical Floor 1 (" +
                        std::to_string(canonFloor.rooms.size()) + " rooms, " +
                        std::to_string(scene.size()) + " entities, " +
                        std::to_string(canonLights.size()) + " room lights); per-room PVS cull ACTIVE");
            // ---- GAMEPLAY onto the canon rooms (makes --world canonlevel PLAYABLE): the
            // sidearm pickup in Jake's Cell, the animated enemy squad down the Main Hall +
            // side cells, Martinez in the Boss Arena, and the 3 rescue girls + their
            // attackers in the Medical Bay / adjacent wards. Every spawn is room-tagged so
            // the flood-fill cull + per-room lights include it (and the model draw is
            // gated to the visible set, see the draw block). Uses the SAME systems the
            // legacy Level1Game uses (MonsterManager / RescueSystem / WeaponSystem). ----
            canonPlay.build(canonFloor, scene, *device, *physics,
                            x3::game::riggedGlbRoot(), x3::game::canonGirlsDialogPath());
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
    } else if (!terrainWorld) {
        game.build(scene, *device, *physics, x3::game::riggedGlbRoot());
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

        // Author the F3/F4/F5 mid-floor encounters onto the Spire plates. The
        // per-floor elevator stops above (one per floor) make them reachable.
        midFloors.build(scene, *device, *physics, Lb, midTriggers,
                        x3::game::riggedGlbRoot());

        // Author the F6/F7 top-floor encounters (the Act-1 finale: F6 strongpoint,
        // F7 the Clone boss + Sarah rescue). Reached via the elevator's top stops.
        topFloors.build(scene, *device, *physics, Lb, topTriggers,
                        x3::game::riggedGlbRoot());

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

    }
    const x3::game::Level1Layout& L1 = game.layout();

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

    // ---- S7: console backend (D6) + screen-space HUD (FPS, console, crosshair).
    std::unique_ptr<x3::con::IConsole> console(x3::con::createConsole());
    x3::game::Hud hud;
    bool quitRequested = false;
    hud.init(*console, &quitRequested);

    // FIX 1: live-tunable viewmodel aim. Register vm_yaw/vm_pitch/vm_roll (deg)
    // and vm_fwd/vm_right/vm_down (m); read them each frame and feed the pose to
    // drawViewmodel so typing e.g. `vm_pitch 10` moves the held gun immediately.
    registerViewmodelCVars(*console);

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
        const float ssX = shotCamOverride ? shotCam[0] : 8.0f;
        const float ssY = shotCamOverride ? shotCam[1] : 1.75f;
        const float ssZ = shotCamOverride ? shotCam[2] : -0.4f;
        const float ssYaw = shotCamOverride ? shotCam[3] : 0.06f;
        const float ssPitch = shotCamOverride ? shotCam[4] : -0.16f;
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
        const int kSettleFrames = (screenshotSettle > 0) ? screenshotSettle : 16;
        for (int i = 0; i < kSettleFrames; ++i) {
            glfwPollEvents();
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
        physics->shutdown();
        device->shutdown();
        glfwDestroyWindow(window);
        glfwTerminate();
        return wrote ? 0 : 1;
    }

    if (smoketest) {
        x3::logInfo("smoketest: stepping Level 1 + rendering 30 frames (+ a mid-run swapchain recreate)");
        // Sit the camera in the armory looking at the pistol pickup so arming +
        // the viewmodel exercise the real GLB load + draw under validation. The
        // Level1Game tick arms the player when the camera is over the pickup, then
        // unlocks/opens Door C, advancing the beat sequence under validation.
        // --world canonlevel: sit in Jake's Cell (the canonical spawn) instead, so the
        // per-room cull renders only that cell + its doored neighbours (the perf proof).
        float camX = L1.armoryCenter.x - 1.0f, camZ = L1.armoryCenter.z;
        float smokeYaw = 0.0f;
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
        audio->setListener(vmX, vmY, vmZ, vmYaw, vmPitch);
        audio->playMusic(kMusicPath, /*loop*/true, /*vol*/0.25f);
        const x3::phys::Vec3 eye{ vmX, vmY, vmZ };
        const float dt = 1.0f / 60.0f;
        for (int i = 0; i < 30; ++i) {
            glfwPollEvents();
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
            char sb[160];
            std::snprintf(sb, sizeof(sb),
                "smoketest: stats draws=%u tris=%u objs=%u/%u gpu=%.3f ms (stress=%u cubes)",
                st.drawCalls, st.triangles, st.objectsDrawn, st.objectsSubmitted,
                st.gpuFrameMs, stress.count());
            x3::logInfo(sb);
        }
        x3::logInfo("smoketest: 30 frames + recreate OK");
        audio->shutdown();
        combatFx.shutdown(*device);
        if (canonPlay.built()) canonPlay.shutdown();   // --world canonlevel enemy ragdolls
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
    bool      tmEnterPrev = false, tmBackPrev = false;
    uint32_t  prevSecretHealth = 0;
    bool      prevSecretNano = false;

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

    // ---- Main loop ----
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

        // ---- S7: console gating. While the console is open, gameplay input is
        // suppressed and the cursor is shown so the user can read/type. The cursor
        // is ALSO shown by any UI menu (main/pause/settings); the UiController is
        // the master for menu cursor state. Recompute the desired cursor each
        // frame and only touch GLFW on a transition.
        const bool consoleOpen = hud.consoleOpen();
        consoleWasOpen = consoleOpen;   // (retained for parity; cursor logic below)
        const bool wantCursor = consoleOpen || gameUi.showCursor();
        if (wantCursor != cursorShown) {
            glfwSetInputMode(window, GLFW_CURSOR,
                             wantCursor ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_DISABLED);
            cursorShown = wantCursor;
        }
        // Whether a UI menu (main/pause/settings) is currently up. While a menu is
        // up, gameplay input + the sim are frozen and only the menu reads input.
        const bool uiMenuActive = !gameUi.playing();
        const bool simFrozen     = gameUi.shouldFreezeSim();

        // Esc (edge-detected): route to the UI controller (toggle pause / back out
        // of settings / resume) UNLESS the console is open or a door-code keypad is
        // active (those consume Esc first). The legacy "Esc quits" is gone — quit is
        // now an explicit menu choice (or the `quit` console command).
        bool escNow = !consoleOpen && glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS;
        bool uiEscEdge = false;
        if (escNow && !kpEscPrev) {
            if (codeMode) { codeMode = false; keypad.clear(); }
            else if (termMode) { termMode = false; game.secret().terminal().setActive(false); }
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
        if (consoleOpen || uiMenuActive) { ddx = 0.0f; ddy = 0.0f; }

        // Gameplay key reads are gated off while the console is open OR a UI menu is
        // up so typing/navigation doesn't drive movement/use/jump/fire/noclip.
        auto keyDown = [&](int k) {
            return !consoleOpen && !uiMenuActive && glfwGetKey(window, k) == GLFW_PRESS;
        };

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

        // ---- WEAPONS: number keys 1..N switch the selected weapon; R reloads.
        // Suppressed while a door-code keypad is active (digits go to the keypad).
        if (!codeMode && !terrainWorld) {
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
            // RESCUED-NPC TALK takes priority over the bare door/rescue handlers so
            // the captive girl always gets her exchange. If a live captive is in talk
            // range, this E starts/advances the dialog; completing it performs the
            // actual rescue (so she becomes a following companion) + queues her bark.
            std::string talkWho; x3::phys::Vec3 talkPos{};
            const bool talkInRange = nearestLiveCaptive(eye, x3::game::kTalkReach, talkWho, talkPos);
            const bool talkHandled = npcDialog.active() || talkInRange;
            if (talkHandled) {
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
                       x3::game::tryUse(eye, dir, 3.0f, scene, canonDoors, *physics)) {
                // Canonical Floor 1: E toggles whatever SM_Door_A slab the player is
                // aiming at (open if closed, close if open). Proximity also auto-opens
                // (handled in the per-frame tick), so this is the deliberate manual path.
                x3::logInfo("use: canon door toggled");
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
            if (npcBarkTimer > 0.0f) npcBarkTimer -= dt;
        }

        // ---- Door-code keypad: capture digit/backspace/enter edges while active.
        // Esc-cancel is handled in the Esc block above. Uses the shared KeypadEntry
        // state machine (also driven by --test-doorcode). ----
        if (codeMode && !terrainWorld) {
            for (int dgt = 0; dgt < 10; ++dgt) {
                bool dn = keyDown(GLFW_KEY_0 + dgt) || keyDown(GLFW_KEY_KP_0 + dgt);
                if (dn && !kpDigitPrev[dgt]) keypad.pushDigit(dgt);
                kpDigitPrev[dgt] = dn;
            }
            bool backNow = keyDown(GLFW_KEY_BACKSPACE);
            if (backNow && !kpBackPrev) keypad.backspace();
            kpBackPrev = backNow;
            bool enterNow = keyDown(GLFW_KEY_ENTER) || keyDown(GLFW_KEY_KP_ENTER);
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
                } else if (game.tryDoorCode(x3::phys::Vec3{ pex, pey, pez }, keypad.value()) ||
                    midFloors.tryDoorCode(x3::phys::Vec3{ pex, pey, pez }, keypad.value()) ||
                    topFloors.tryDoorCode(x3::phys::Vec3{ pex, pey, pez }, keypad.value()) ||
                    subLevels.tryDoorCode(x3::phys::Vec3{ pex, pey, pez }, keypad.value())) {
                    x3::logInfo("keypad: ACCEPTED — door opening");
                    codeMode = false; keypad.clear();
                } else {
                    x3::logInfo("keypad: rejected");
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
                bool dn = keyDown(GLFW_KEY_0 + dgt) || keyDown(GLFW_KEY_KP_0 + dgt);
                if (dn && !tmDigitPrev[dgt]) term.pushChar((char)('0' + dgt));
                tmDigitPrev[dgt] = dn;
            }
            bool tbackNow = keyDown(GLFW_KEY_BACKSPACE);
            if (tbackNow && !tmBackPrev) term.backspace();
            tmBackPrev = tbackNow;
            bool tEnterNow = keyDown(GLFW_KEY_ENTER) || keyDown(GLFW_KEY_KP_ENTER);
            if (tEnterNow && !tmEnterPrev) {
                bool ok = term.submit();   // fires the sink -> opens the trapdoor on 1127
                if (ok) { termMode = false; term.setActive(false);
                          x3::logInfo("terminal: code ACCEPTED — trapdoor opening"); }
                else      x3::logInfo("terminal: code rejected");
            }
            tmEnterPrev = tEnterNow;
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
        device->setCamera(camX, camY, camZ, camYaw, camPitch, 60.0f);
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
            // ---- CANONLEVEL DOORS: tick the SM_Door_A slide animation, and PROXIMITY
            // AUTO-OPEN — a door within ~2.2 m of the player opens; once the player walks
            // past (>~3.2 m, hysteresis so it doesn't chatter at the threshold) it closes
            // again. This is the data-driven floor's door behaviour (E-use also toggles
            // any door the player aims at, handled in the use block above). The slabs
            // block the player while Closed and slide UP clear of the lintel when Open.
            if (canonWorld && canonFloor.valid()) {
                for (uint32_t di = 0; di < canonDoors.count(); ++di) {
                    x3::game::Door& d = canonDoors.at(di);
                    const float dx = camPos.x - d.closedPos.x;
                    const float dz = camPos.z - d.closedPos.z;
                    const float d2 = dx * dx + dz * dz;
                    if (d2 < 2.2f * 2.2f) {
                        if (d.state == x3::game::DoorState::Closed) canonDoors.startOpening(d);
                    } else if (d2 > 3.2f * 3.2f) {
                        if (d.state == x3::game::DoorState::Open) canonDoors.toggle(d);   // Open -> Closing
                    }
                }
                canonDoors.update(dt, scene, *physics);
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
        bool meleeNow = (keyDown(GLFW_KEY_V) ||
            (!consoleOpen && glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS));
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
        bool fireHeld = !consoleOpen && !simFrozen && glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
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
                    combatFx.addTracer(muzzle, r.endPoint);   // tracer + muzzle burst per pellet
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

        int cw, ch;
        glfwGetFramebufferSize(window, &cw, &ch);
        if (cw != lastW || ch != lastH) {
            lastW = cw; lastH = ch;
            if (cw > 0 && ch > 0) device->onResize(static_cast<uint32_t>(cw), static_cast<uint32_t>(ch));
        }

        auto frame = device->beginFrame();
        if (frame.valid) {
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
                if (arsenal.viewmodelsLoaded() && vmArmed) {
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

            // Always-on overlays (independent of game state): FPS meter, the perf
            // stats panel, and the dev console panel (drawn last so it sits on top).
            hud.drawFps(*device, frame, *console, dt);
            hud.drawStats(*device, frame, *console, dt);
            hud.drawConsole(*device, frame, *console, dt);
        }
        device->endFrame(frame);
        g_perf.addFrame((double)dt);   // per-system perf breakdown logged every 120 frames
    }

    x3::logInfo("shutting down");
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
    game.shutdown();
    if (canonPlay.built()) canonPlay.shutdown();   // --world canonlevel enemy ragdolls
    physics->shutdown();
    device->shutdown();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
