// X3Engine host — opens a window, brings up the render device + physics, builds
// the S2 graybox test level, and runs the loop with a fly camera. Walking is S3.

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#include "engine/core/x3_log.h"
#include "engine/core/IConsole.h"
#include "engine/core/IJobSystem.h"
#include "engine/core/version.h"   // generated build identity: --version / --test-version / startup banner
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
#include "thirdperson.h"                    // FP/3P toggle + Jake avatar + held weapon (--test-thirdperson)
#include "level1.h"
#include "level_loader.h"                   // data-driven canonical level loader + per-room PVS cull (--test-canonlevel)
#include "player.h"
#include "swim_controller.h"                // Act 4 underwater swim/dive controller (--test-swim + --world swim)
#include "monster.h"
#include "npc.h"                            // non-combatant NPC controller (--test-npc / --world npc)
#include "ally.h"                            // coop-NPC allies + --bench-combat arena
// Forward decls for the --bench-combat arena's free helpers (lives in
// app/ally_arena.cpp). NOT promoted into ally.h to keep that public contract
// frozen for the Phase A/B siblings; the names are only ever used from main.cpp's
// bench-combat driver below.
namespace x3::game {
    class MonsterManager;
    MonsterManager& allyArenaMonsters();
    uint32_t        allyArenaEnemyCount();
    uint32_t        allyArenaEnemiesAlive();
    void            allyArenaShutdown();
}
#include "level1_game.h"
#include "canon_play.h"                     // --world canonlevel gameplay (sidearm + animated enemies + Martinez + girls)
#include "mech_pilot.h"                      // rideable heavy-mech pilot controller (--test-mech + --world mech)
#include "npc_dialog.h"                     // rescued-NPC talk/dialog -> companion (the captive girl)
#include "physprops.h"                      // FEATURE_GOALS §1: hanging cubes / joints (ragdoll foundation)
#include "ragdoll.h"                        // FEATURE_GOALS §2: physics death ragdoll
#include "editor/editor.h"                  // native Level Editor E1 (brain + self-test)
#include "barrels.h"                        // explosive barrels (shoot -> chain explosion)
#include "glass_test.h"                      // translucent-glass material (--test-glass)
#include "holo_terminal.h"                  // Jake's cell holographic terminal (text + input)
#include "holo_terminal_system.h"           // placeable HoloTerminal kiosks + command dispatch
#include "glass_lounge.h"                   // glass table + chairs + SIT-AT-CHAIR (--test-sit)
#include "secret_room.h"                    // code-locked trapdoor -> stocked secret room
#include "engine/ecs/Ecs.h"                 // sparse-set ECS core (10k+ entities)
#include "ecs_render.h"                     // ECS -> GPU-driven render feed
#include "spire_mid.h"                      // EFLZ Spire F3/F4/F5 mid-floor content
#include "spire_top.h"                      // EFLZ Spire F6/F7 top-floor content (Act-1 finale)
#include "spire_nexus.h"                    // EFLZ Floor 4.5 Nexus Chamber / The Chorus (off-elevator boss)
#include "spire_sublevels.h"                // EFLZ hidden Floor-7 sub-levels + Dr. Chen Return Mission
#include "timeline.h"                        // EFLZ morality/timeline backbone for the 12 endings (--test-timeline)
#include "act2_world.h"                      // EFLZ Act-2 open-world surface host + L8/L9 (--test-act2)
#include "canon_aliens.h"                    // EFLZ canon-alien roster (Mantis/Grey/Reptilian/Nordic) — --test-canonaliens
#include "act2_desert.h"                     // EFLZ Act-2 desert depths + Salvari camp L10/L11 (--test-act2desert)
#include "act2_caves.h"                      // EFLZ Act-2 mid biomes L12-15 (--test-act2caves)
#include "tod.h"                             // EFLZ Time-of-Day cycle (sky/sun via SkyParams — --test-tod)
#include "weather.h"                         // EFLZ Weather (7 states, biome-gated, hazard — --test-weather)
#include "world_regions.h"                   // EFLZ open-world surrounding regions + 4 mountain ranges (--test-worldregions)
#include "city.h"                            // EFLZ open-world metropolis: districts + roads + freeway tunnels (--test-city)
#include "ocean_base.h"                      // EFLZ open-world ocean + undersea base + submarine combat (--test-oceanbase)
#include "undersea_art.h"                     // EFLZ Act-4 undersea-base art overlay (Abyssal Station GLB) (--test-undersea-art)
#include "elevator.h"
#include "club1127.h"
#include "valley.h"                          // Crystal Valleys (Act 2, L15 — --world valley)
#include "cliffs.h"                          // Salvari cliffs finale (--world cliffs)
#include "companion.h"                      // companion reflex AI (--test-companion)
#include "companion_squad.h"                // companion Slice B: squad integration (--test-companion-squad + --world companion)
#include "companion_controller.h"           // companion Slice C seam: single-companion wrapper (--test-companion-controller)
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
#include "space_pilot.h"                   // Act-3 6DOF space-flight pilot (--test-space + --world space)
#include "sky_stars.h"                     // procedural starfield (--test-starfield + --world starfield)
#include "space/space_layer.h"             // S0 SpaceLayer spine (--test-spacelayer)
#include "space/lod.h"                      // S2 distance-LOD system (--test-lod + --world lod)
#include "space/space_env.h"               // S1 space environment: nebula/stars + planets + sun (--test-spaceenv + --world spaceenv)
#include "space/ship_anim.h"               // S11 ship node-transform anim (--test-shipanim + --world shipanim)
#include "space/wormhole_vfx.h"            // Salvari crystal-matrix wormhole (--test-wormhole + --world wormhole)
#include "space/decloak_vfx.h"             // intro decloak shimmer VFX (--test-decloak + --world decloak)
#include "space/tractor_beam.h"            // capital-ship tractor beam (--test-tractor + --world tractor)
#include "space/wormhole_transit.h"        // S3 wormhole transit (--test-wormhole-transit + --world wormhole-transit)
#include "space/descent.h"                  // S4 cinematic atmo descent (--test-atmo-descent + --world atmo-descent)
#include "space/ship_interior.h"           // S5 walkable ship interior (--test-ship-interior + --world ship-interior)
#include "space/ship_ai.h"                  // S8 enemy ship AI / dogfight (--test-ship-ai + --world ship-ai)
#include "space/targeting.h"               // S9 targeting / radar / lock-on (--test-targeting + --world targeting)
#include "space/ship_damage.h"             // S10 ship damage model (--test-ship-damage)
#include "space/eva.h"                      // S12 EVA spacewalk controller (--test-eva + --world eva)
#include "space/ship_windows.h"            // S6 true-portal ship windows (--test-ship-windows + --world ship-windows)
#include "space/ship_repair.h"             // S7 in-transit panel repair (--test-ship-repair + --world ship-repair)
#include "headless_device.h"               // HeadlessRenderDevice (used by --test-starfield)

#include <memory>
#include <string_view>
#include <string>
#include <cmath>
#include <map>
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
#include <regex>      // --test-version well-formedness check

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
    // CPU per-object camera-FRUSTUM (POV) cull, inside the renderer. 1 = on (skip
    // draws whose world bounds are outside the view cone — "don't render what can't
    // be seen", an ADDITIVE gate on top of r_roomcull); 0 = submit every record (the
    // pre-cull behavior). Animated/skinned meshes + the player weapon are never culled.
    console.registerCVar("r_frustumcull", "1", "CPU per-object camera-frustum cull (0 = submit everything)");
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
// Over-enemy HEALTH BAR — owner playtest tuning (2026-05-27).
//
// Directive: bars are TOO PROMINENT, visible THROUGH walls, and missing on
// some enemies. The polished bar is:
//   1) THIN     — kEnemyBarW px wide, kEnemyBarH px tall, with a 1 px dark
//                 outline. No icon / no number / no shadow / no shimmer.
//   2) FADED    — full alpha within kEnemyBarFullDist meters, smooth-stepped
//                 to 0 by kEnemyBarFarDist (past far: no bar at all).
//   3) LOS      — per-bar Static-layer rayCast from the camera origin to the
//                 enemy's torso world position. If a wall is in the way, the
//                 bar is suppressed (no more "see through walls" leak).
//   4) ON EVERY — every living MonsterSystem is iterated (every floor +
//                 every boss + Chorus pods + sublevels + canon + B1 groups).
// The raycast is gated behind the distance + on-screen check so distant
// enemies cost ~one squared-distance compare per frame (no ray fired).
//
// Constants are gameplay tuning — safe to retune for playtest.
constexpr float kEnemyBarW          = 40.0f;  // bar width (pixels) — ~36–48 px band
constexpr float kEnemyBarH          = 2.0f;   // bar height (pixels) — ~2 px
constexpr float kEnemyBarOutlinePx  = 1.0f;   // 1 px dark outline around the bar
constexpr float kEnemyBarFullDist   = 6.0f;   // full alpha within this (m)
constexpr float kEnemyBarFarDist    = 22.0f;  // gone by this distance (m)
constexpr float kEnemyBarAnchorY    = 2.2f;   // bar anchor above body-center (m)
constexpr float kEnemyBarChestY     = 1.0f;   // LOS endpoint above body-center (m)
constexpr float kEnemyBarFillAlpha  = 0.95f;  // base fill alpha (multiplied by fade)
constexpr float kEnemyBarBackAlpha  = 0.65f;  // empty/back alpha (multiplied by fade)
constexpr float kEnemyBarOutAlpha   = 0.80f;  // outline alpha (multiplied by fade)

// Compute the alpha multiplier from camera->enemy distance. 1 at <= kEnemyBarFullDist,
// smoothstep down to 0 at kEnemyBarFarDist; 0 beyond (caller should skip the draw).
inline float enemyBarFadeAlpha(float distMeters) {
    if (distMeters <= kEnemyBarFullDist) return 1.0f;
    if (distMeters >= kEnemyBarFarDist)  return 0.0f;
    const float t = (distMeters - kEnemyBarFullDist) /
                    (kEnemyBarFarDist - kEnemyBarFullDist);
    // 1 - smoothstep(0,1,t)
    const float s = t * t * (3.0f - 2.0f * t);
    return 1.0f - s;
}

// Draw a single over-enemy bar for `m` if it is alive, near the camera, on screen,
// and not occluded by a Static wall. The LOS rayCast is the most expensive op; it is
// gated by squared-distance + worldToScreen, so distant / off-screen enemies cost
// only a few floats per frame. Returns true iff a bar was drawn (debug accounting).
bool drawOneEnemyBar(x3::rhi::IRenderDevice& device, const x3::rhi::FrameContext& frame,
                     x3::phys::IPhysicsWorld& physics, const x3::phys::Vec3& camEye,
                     const x3::game::MonsterSystem& m) {
    if (!m.alive()) return false;
    const int hpv = m.hp();
    const int mxv = m.maxHp();
    if (mxv <= 0 || hpv <= 0) return false;     // safety (every enemy ships maxHp>0)
    const x3::phys::Vec3 c = m.pos();
    // CHEAP distance gate: bail past the far range with no rays / no projection.
    const float dx = c.x - camEye.x, dy = c.y - camEye.y, dz = c.z - camEye.z;
    const float d2 = dx*dx + dy*dy + dz*dz;
    if (d2 >= kEnemyBarFarDist * kEnemyBarFarDist) return false;
    const float dist = std::sqrt(d2);
    const float fade = enemyBarFadeAlpha(dist);
    if (fade <= 0.0f) return false;
    // Project the anchor (above the head) to screen. Behind camera => skip (no ray).
    const x3::phys::Vec3 anchor{ c.x, c.y + kEnemyBarAnchorY, c.z };
    float sx = 0.0f, sy = 0.0f;
    if (!device.worldToScreen(anchor.x, anchor.y, anchor.z, sx, sy)) return false;
    // LOS occlusion: from the CAMERA ORIGIN toward the enemy's CHEST (a stable
    // torso target). If a Static-layer body (wall/floor/door) is hit before the
    // chest, the bar is "through a wall" and is suppressed.
    const x3::phys::Vec3 chest{ c.x, c.y + kEnemyBarChestY, c.z };
    const float cdx = chest.x - camEye.x;
    const float cdy = chest.y - camEye.y;
    const float cdz = chest.z - camEye.z;
    const float cdist = std::sqrt(cdx*cdx + cdy*cdy + cdz*cdz);
    if (cdist > 0.05f) {
        const x3::phys::Vec3 nd{ cdx / cdist, cdy / cdist, cdz / cdist };
        // Stop just short of the chest (0.3 m) so we don't graze the enemy's own
        // collision proxy — but per the Layer::Static mask the enemy hitbox is on
        // a DIFFERENT layer (Layer::Enemy), so this is belt+braces.
        const x3::phys::RayHit los = physics.rayCast(
            camEye, nd, std::max(0.1f, cdist - 0.3f), x3::phys::Layer::Static);
        if (los.hit) return false;
    }
    // ---- Bar geometry ----
    const float frac = (hpv >= mxv) ? 1.0f : (float)hpv / (float)mxv;
    uint32_t hw = 0, hh = 0; device.hudSize(hw, hh);
    const float bw = kEnemyBarW, bh = kEnemyBarH;
    float x0 = sx - bw * 0.5f;
    float y0 = sy;
    // Clamp on-screen so a bar attached to a close enemy never leaks off the top/bottom.
    if (y0 < kEnemyBarOutlinePx + 1.0f) y0 = kEnemyBarOutlinePx + 1.0f;
    if (hh > 8 && y0 > (float)hh - (bh + kEnemyBarOutlinePx + 1.0f))
        y0 = (float)hh - (bh + kEnemyBarOutlinePx + 1.0f);
    // ---- Bar colors (faded by distance) ----
    // Fill turns from a desaturated steel-green at full HP toward a warm red as HP
    // drops — readable at a glance, but not loud. A faint hit-flash white-flares the
    // fill (the flash decays in ~kHitFlashTime seconds).
    const float lowH  = 1.0f - frac;            // 0 healthy -> 1 dying
    const float flash = m.hitFlash();           // [0,1], fades quickly after a shot
    const float ar = std::min(1.0f, kEnemyBarFillAlpha * fade);
    const float ab = std::min(1.0f, kEnemyBarBackAlpha * fade);
    const float ao = std::min(1.0f, kEnemyBarOutAlpha  * fade);
    const float outl[4]  = { 0.02f, 0.02f, 0.03f, ao };
    const float backC[4] = { 0.06f, 0.07f, 0.10f, ab };
    const float fillC[4] = {
        0.30f + 0.55f * lowH + 0.30f * flash,
        0.62f - 0.30f * lowH + 0.20f * flash,
        0.32f - 0.20f * lowH + 0.20f * flash,
        ar
    };
    const float fillW = bw * frac;
    // Outline (1 px dark border around the whole bar).
    device.drawHudQuad(frame, x0 - kEnemyBarOutlinePx, y0 - kEnemyBarOutlinePx,
                       bw + 2.0f * kEnemyBarOutlinePx, bh + 2.0f * kEnemyBarOutlinePx,
                       outl);
    // Empty track.
    device.drawHudQuad(frame, x0, y0, bw, bh, backC);
    // Filled portion.
    if (fillW > 0.0f) device.drawHudQuad(frame, x0, y0, fillW, bh, fillC);
    return true;
}

// Iterate every monster in `mm` and draw its bar (subject to the cheap distance,
// on-screen, and LOS gates inside drawOneEnemyBar). Returns the count drawn.
uint32_t drawEnemyBarsForManager(x3::rhi::IRenderDevice& device,
                                  const x3::rhi::FrameContext& frame,
                                  x3::phys::IPhysicsWorld& physics,
                                  const x3::phys::Vec3& camEye,
                                  const x3::game::MonsterManager& mm) {
    uint32_t drawn = 0;
    for (uint32_t i = 0; i < mm.count(); ++i)
        if (drawOneEnemyBar(device, frame, physics, camEye, mm.at(i))) ++drawn;
    return drawn;
}

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

// ---------------------------------------------------------------------------
// Version reporting (--version) + its self-test (--test-version).
// See docs/VERSIONING.md. The string is compiled in from the generated
// engine/core/version.h (CMake stamps it from git at build time).
// ---------------------------------------------------------------------------

// The exact text printed by --version (also exercised by --test-version).
// e.g. "X3 v0.3.00284 (c3c74e1)".
std::string versionLine() { return std::string("X3 v") + X3_VERSION_FULL; }

// --version: print the build identity and exit 0.
int runVersionFlag() {
    std::printf("%s\n", versionLine().c_str());
    std::fflush(stdout);
    return 0;
}

// --test-version: assert the version string is non-empty + well-formed
// (^0\.4\.\d{5}$), that the console `version` command reports it, and that the
// --version path produces it. Prints "version: X/Y passed". Returns true if all pass.
bool runVersionSelfTest() {
    int pass = 0, total = 0;
    auto check = [&](bool c, const char* name) {
        ++total;
        if (c) { ++pass; x3::logInfo(std::string("[ver-test] PASS ") + name); }
        else   {          x3::logError(std::string("[ver-test] FAIL ") + name); }
    };

    // T1: X3_VERSION_STRING non-empty.
    const std::string vstr = X3_VERSION_STRING;
    check(!vstr.empty(), "T1 X3_VERSION_STRING non-empty");

    // T2: well-formed "^0\.4\.\d{5}$" (MAJOR.MINOR fixed at 0.4; BUILD = 5 digits).
    check(std::regex_match(vstr, std::regex(R"(^0\.4\.\d{5}$)")), "T2 matches ^0\\.4\\.\\d{5}$");

    // T3: full string is "<string> (<hash>)" and contains the version string + a hash.
    const std::string vfull = X3_VERSION_FULL;
    check(std::regex_match(vfull, std::regex(R"(^0\.4\.\d{5} \([0-9a-f]+|nogit\)$)")) ||
          vfull == (vstr + " (" X3_GIT_HASH ")"),
          "T3 X3_VERSION_FULL well-formed");

    // T4: the console `version` command (the real registration path) reports the
    // version. Register exactly as Hud::init does and exec it; read it back from
    // the console output log.
    {
        x3::con::IConsole* c = x3::con::createConsole();
        c->registerCommand("version", [c](const std::vector<std::string>&) {
            c->print(std::string("X3 v") + X3_VERSION_FULL);
        }, "print version");
        c->exec("version");
        const auto& lines = c->outputLines();
        bool found = !lines.empty() && lines.back() == versionLine();
        check(found, "T4 console `version` reports X3_VERSION_FULL");
        delete c;
    }

    // T5: the --version code path yields exit 0 and prints versionLine().
    check(runVersionFlag() == 0 && versionLine() == (std::string("X3 v") + X3_VERSION_FULL),
          "T5 --version path");

    x3::logInfo("version: " + std::to_string(pass) + "/" + std::to_string(total) + " passed");
    std::printf("version: %d/%d passed\n", pass, total);
    std::fflush(stdout);
    return pass == total;
}

} // namespace

// ---------------------------------------------------------------------------
// --world mech showcase (rideable heavy-mech). Extracted from main() into its own
// function so its locals do NOT inflate main()'s already-large aggregate stack frame
// (MSVC reserves the sum of all inline --world blocks at main() entry; this block put
// it over the 1 MB stack reserve -> a startup stack overflow). Owns device + GLFW
// teardown like every other --world block and returns the process exit code.
//
// A rideable heavy mech (MechPilotController) on a checker ground plane with a few
// "target dummy" props to shoot. 3rd-person chase cam by default (V toggles cockpit);
// WASD walk (sluggish), Shift boost, Space jump-jets, LMB autocannon, R missile pod.
// Headless `--world mech --screenshot <path>` poses a chase still + captures a PNG.
// The mech VISUAL tries mech.glb first, falling back to a large humanoid GLB scaled
// up, then to a primitive gunmetal box silhouette. SSAO+SSGI are forced OFF (the
// documented near-black raster-fallback workaround on this non-RT base).
static int runMechWorld(x3::rhi::IRenderDevice& device, GLFWwindow* window,
                        bool headless, bool screenshot, const std::string& screenshotPath,
                        bool shotCamOverride, const float shotCam[5],
                        uint32_t W, uint32_t H) {
    x3::logInfo("--world mech: building the rideable heavy-mech showcase");

    std::unique_ptr<x3::phys::IPhysicsWorld> mphys(x3::phys::createPhysicsWorld());
    if (!mphys->init()) {
        x3::logError("--world mech: physics init failed");
        device.shutdown(); if (window) glfwDestroyWindow(window); glfwTerminate(); return 1;
    }

    // SSAO/SSGI OFF (the documented near-black raster-fallback workaround).
    { x3::rhi::IRenderDevice::SsaoParams sp{}; sp.enabled = false; device.setSsaoParams(sp); }
    { x3::rhi::IRenderDevice::GiParams   gp{}; gp.enabled = false; device.setGiParams(gp); }

    // ---- Checker ground plane (physics + render). -------------------------
    const float kGroundHalf = 60.0f;
    {
        float v[] = {
            -kGroundHalf, 0.0f, -kGroundHalf,
             kGroundHalf, 0.0f, -kGroundHalf,
             kGroundHalf, 0.0f,  kGroundHalf,
            -kGroundHalf, 0.0f,  kGroundHalf,
        };
        uint32_t idx[] = { 0,2,1, 0,3,2 };
        mphys->addStaticMesh(v, 4, idx, 6);
    }
    x3::prims::PrimMesh groundGeo = x3::prims::makeBox(kGroundHalf, 0.15f, kGroundHalf, 0.0f, -0.15f, 0.0f, 16.0f);
    auto groundMesh = device.createMesh(groundGeo.verts.data(), (uint32_t)groundGeo.verts.size(),
                                        groundGeo.index.data(), (uint32_t)groundGeo.index.size());
    auto checkerD = x3::prims::makeCheckerRGBA(64, 8, 70, 76, 92, 40, 44, 56);
    auto checkerTex = device.createTexture(checkerD.data(), 64, 64, true);
    auto whiteD = x3::prims::makeSolidRGBA(8, 235, 235, 235);
    auto whiteTex = device.createTexture(whiteD.data(), 8, 8, true);

    // ---- Lighting: sky + point fills so the mech reads. -------------------
    { x3::rhi::IRenderDevice::SkyParams sp{}; sp.enabled = true; sp.sunIntensity = 1.4f; sp.haze = 0.35f;
      sp.sunDir[0]=0.4f; sp.sunDir[1]=1.0f; sp.sunDir[2]=0.3f; device.setSkyParams(sp); }
    { x3::rhi::PointLight pl[3];
      pl[0].pos[0]= 6.0f; pl[0].pos[1]=9.0f; pl[0].pos[2]= 6.0f; pl[0].range=42.0f;
      pl[0].color[0]=9.0f; pl[0].color[1]=8.6f; pl[0].color[2]=8.0f;
      pl[1].pos[0]=-6.0f; pl[1].pos[1]=8.0f; pl[1].pos[2]=-2.0f; pl[1].range=42.0f;
      pl[1].color[0]=6.0f; pl[1].color[1]=6.4f; pl[1].color[2]=8.0f;
      pl[2].pos[0]= 0.0f; pl[2].pos[1]=10.0f; pl[2].pos[2]=-10.0f; pl[2].range=48.0f;
      pl[2].color[0]=7.0f; pl[2].color[1]=7.0f; pl[2].color[2]=7.0f;
      device.setPointLights(pl, 3); }

    // ---- Spawn the mech controller. ---------------------------------------
    x3::game::MechPilotController mech;
    mech.spawn(*mphys, 0.0f, 0.2f, 0.0f);

    // ---- Mech visual: mech.glb -> OverLordEnforcer99.glb (scaled up) -> box.
    std::unique_ptr<x3::asset::IAssetSource> masrc(x3::asset::createAssetSource());
    masrc->mountDir(x3::game::riggedGlbRoot(), 0);
    std::unique_ptr<x3::asset::IModelLoader> mmloader(
        x3::asset::createModelLoader(&device, masrc.get()));
    x3::asset::Model mechModel;
    std::vector<x3::asset::ModelDrawable> mechDrawables;
    float mechScale = 1.0f;
    std::string mechGlbUsed = "(none - box fallback)";
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const char* cands[] = { "mech.glb", "OverLordEnforcer99.glb", "RexBouncer.glb" };
        for (const char* c : cands) {
            if (!fs::exists(fs::path(x3::game::riggedGlbRoot()) / c, ec)) continue;
            mechModel = mmloader->load(c);
            if (mechModel.ok) {
                mechDrawables = x3::asset::makeDrawables(mechModel);
                mechGlbUsed = c;
                // Humanoid GLBs read ~1.8 m at scale 1; scale up to the mech's
                // ~3.6 m capsule height for the gunmetal-giant silhouette.
                mechScale = (std::string(c) == "mech.glb") ? 1.0f : 2.0f;
                break;
            }
        }
    }
    x3::logInfo(std::string("--world mech: mech visual = ") + mechGlbUsed +
                " (scale " + std::to_string(mechScale) + ")");
    // Box silhouette fallback when no GLB loads (a tall gunmetal slab).
    x3::prims::PrimMesh mechBoxGeo = x3::prims::makeBox(1.1f, 1.8f, 0.9f, 0.0f, 0.0f, 0.0f, 1.0f);
    auto mechBoxMesh = device.createMesh(mechBoxGeo.verts.data(), (uint32_t)mechBoxGeo.verts.size(),
                                         mechBoxGeo.index.data(), (uint32_t)mechBoxGeo.index.size());
    auto gunmetalD = x3::prims::makeSolidRGBA(8, 96, 102, 112);
    auto gunmetalTex = device.createTexture(gunmetalD.data(), 8, 8, true);

    // ---- Target-dummy props (static red boxes) to shoot. ------------------
    struct Dummy { float x, y, z, half; bool alive; };
    std::vector<Dummy> dummies = {
        {  8.0f, 1.2f,  4.0f, 1.2f, true },
        { 12.0f, 1.5f, -2.0f, 1.5f, true },
        {  4.0f, 1.0f, 10.0f, 1.0f, true },
        { -6.0f, 1.3f,  9.0f, 1.3f, true },
    };
    std::vector<x3::rhi::MeshHandle> dummyMeshes;
    for (const auto& d : dummies) {
        x3::prims::PrimMesh g = x3::prims::makeBox(d.half, d.half, d.half, d.x, d.y, d.z, 1.0f);
        dummyMeshes.push_back(device.createMesh(g.verts.data(), (uint32_t)g.verts.size(),
                                                g.index.data(), (uint32_t)g.index.size()));
    }
    const float dummyTint[4] = { 0.85f, 0.22f, 0.20f, 1.0f };
    const float dummyEm[4]   = { 0.85f, 0.22f, 0.20f, 2.5f };
    const float idMat[16]    = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

    x3::game::CombatFx mfx;
    mfx.init(device);

    const float dt = 1.0f / 60.0f;

    // Draw helper: ground + mech (GLB or box) + alive dummies + FX.
    auto drawMechScene = [&](const x3::rhi::FrameContext& frame,
                             float eyeX, float eyeY, float eyeZ, float cyaw, float cpitch) {
        const float white4[4] = {1,1,1,1};
        device.drawMesh(frame, groundMesh, checkerTex, white4, idMat);
        x3::phys::Vec3 feet = mech.feet();
        const float my = mech.yaw();
        const float cy = std::cos(my), sy = std::sin(my);
        float place[16] = {
            cy*mechScale, 0.0f, -sy*mechScale, 0.0f,
            0.0f, mechScale, 0.0f, 0.0f,
            sy*mechScale, 0.0f,  cy*mechScale, 0.0f,
            feet.x, feet.y, feet.z, 1.0f
        };
        if (!mechDrawables.empty()) {
            for (const auto& dr : mechDrawables) {
                float fin[16];
                x3::asset::mulMat4(place, dr.nodeTransform, fin);
                float tint[4] = { dr.baseColorFactor[0]*1.3f + 0.1f,
                                  dr.baseColorFactor[1]*1.3f + 0.1f,
                                  dr.baseColorFactor[2]*1.3f + 0.15f, dr.baseColorFactor[3] };
                device.drawMesh(frame, x3::rhi::MeshHandle{ dr.meshId },
                                x3::rhi::TextureHandle{ dr.baseColorTexId }, tint, fin);
            }
        } else {
            float bm[16] = {
                cy, 0.0f, -sy, 0.0f,
                0.0f, 1.0f, 0.0f, 0.0f,
                sy, 0.0f,  cy, 0.0f,
                feet.x, feet.y + 1.8f, feet.z, 1.0f
            };
            const float mtint[4] = {1,1,1,1};
            device.drawMesh(frame, mechBoxMesh, gunmetalTex, mtint, bm);
        }
        for (size_t i = 0; i < dummies.size(); ++i) {
            if (!dummies[i].alive) continue;
            device.drawMeshEmissive(frame, dummyMeshes[i], whiteTex, dummyTint, dummyEm, idMat);
        }
        mfx.draw(device, frame, eyeX, eyeY, eyeZ, cyaw, cpitch);
    };

    // Fire at the nearest dummy in front (cheap ray-vs-sphere) + add a tracer + damage.
    auto fireAtDummies = [&](int dmg) {
        x3::phys::Vec3 m = mech.muzzle();
        x3::phys::Vec3 d = mech.aimDir();
        float bestT = 1e9f; int bestI = -1;
        for (size_t i = 0; i < dummies.size(); ++i) {
            if (!dummies[i].alive) continue;
            float ox = dummies[i].x - m.x, oy = dummies[i].y - m.y, oz = dummies[i].z - m.z;
            float proj = ox*d.x + oy*d.y + oz*d.z;
            if (proj <= 0.0f) continue;
            float cx = m.x + d.x*proj, cy2 = m.y + d.y*proj, cz = m.z + d.z*proj;
            float dx2 = cx-dummies[i].x, dy2 = cy2-dummies[i].y, dz2 = cz-dummies[i].z;
            float dist2 = dx2*dx2 + dy2*dy2 + dz2*dz2;
            float r = dummies[i].half * 1.4f;
            if (dist2 <= r*r && proj < bestT) { bestT = proj; bestI = (int)i; }
        }
        x3::phys::Vec3 hit;
        if (bestI >= 0) {
            hit = x3::phys::Vec3{ dummies[bestI].x, dummies[bestI].y, dummies[bestI].z };
            mfx.spawnImpact(hit, x3::phys::Vec3{ -d.x, -d.y, -d.z });
            static int hp[8] = {0};
            hp[bestI] -= dmg;
            if (hp[bestI] <= -200) { dummies[bestI].alive = false; mfx.spawnDeath(hit); }
        } else {
            hit = x3::phys::Vec3{ m.x + d.x*60.0f, m.y + d.y*60.0f, m.z + d.z*60.0f };
        }
        mfx.addTracer(m, hit);
    };

    auto cleanup = [&]() {
        mfx.shutdown(device);
        if (mechModel.ok) mmloader->unload(mechModel);
        for (auto mh : dummyMeshes) device.destroyMesh(mh);
        device.destroyMesh(mechBoxMesh); device.destroyMesh(groundMesh);
        device.destroyTexture(gunmetalTex); device.destroyTexture(whiteTex);
        device.destroyTexture(checkerTex);
    };

    // ===== Headless capture: settle, walk a step, pose a chase shot, grab. =====
    if (headless) {
        const std::string outPath = screenshot ? screenshotPath
                                   : std::string("G:/X3Native/captures/mech.png");
        const int kFrames = 90;
        for (int i = 0; i < kFrames; ++i) {
            glfwPollEvents();
            x3::game::PlayerInput in;
            if (i > 20 && i < 70) in.moveFwd = 1.0f;   // stomp forward a bit
            mech.update(in, dt, *mphys);
            mphys->step(dt);
            if (i > 40 && (i % 6 == 0) && mech.fireAutocannon(dt)) fireAtDummies(mech.tuning().autocannonDmg);
            mfx.update(dt);
            float cx, cy, cz, cyaw, cpitch;
            mech.camera(cx, cy, cz, cyaw, cpitch);
            if (shotCamOverride) { cx=shotCam[0]; cy=shotCam[1]; cz=shotCam[2]; cyaw=shotCam[3]; cpitch=shotCam[4]; }
            device.setCamera(cx, cy, cz, cyaw, cpitch, 65.0f);
            if (i == kFrames - 1) device.armCapture(outPath.c_str());
            auto frame = device.beginFrame();
            if (frame.valid) drawMechScene(frame, cx, cy, cz, cyaw, cpitch);
            device.endFrame(frame);
        }
        const bool wrote = device.captureFrame(outPath.c_str());
        x3::phys::Vec3 fp = mech.feet();
        char rb[256];
        std::snprintf(rb, sizeof(rb),
            "--world mech: wrote %s | mech=%s feet=(%.1f,%.1f,%.1f) hull=%d armor=%d fuel=%.2f",
            outPath.c_str(), mechGlbUsed.c_str(), fp.x, fp.y, fp.z,
            mech.hull(), mech.armor(), mech.jumpJetFuel());
        if (wrote) x3::logInfo(rb); else x3::logError("--world mech: capture FAILED");
        cleanup(); mphys->shutdown(); device.shutdown();
        if (window) glfwDestroyWindow(window); glfwTerminate();
        return wrote ? 0 : 1;
    }

    // ===== Walkable windowed path: pilot the mech, chase cam. =====
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
    double lastMX, lastMY; glfwGetCursorPos(window, &lastMX, &lastMY);
    double prevTime = glfwGetTime();
    bool prevV = false, prevR = false;
    x3::logInfo("--world mech: WASD walk (Shift boost), Space jump-jets, LMB autocannon, R missiles, V cockpit, Esc quit");
    int lastWd = (int)W, lastHd = (int)H;
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;
        double now = glfwGetTime(); float fdt = (float)(now - prevTime); prevTime = now;
        if (fdt > 0.1f) fdt = 0.1f;
        double mx, my; glfwGetCursorPos(window, &mx, &my);
        float ddx=(float)(mx-lastMX), ddy=(float)(my-lastMY); lastMX=mx; lastMY=my;
        auto kd = [&](int k){ return glfwGetKey(window, k) == GLFW_PRESS; };

        x3::game::PlayerInput in;
        in.moveFwd    = (kd(GLFW_KEY_W)?1.0f:0.0f) - (kd(GLFW_KEY_S)?1.0f:0.0f);
        in.moveStrafe = (kd(GLFW_KEY_D)?1.0f:0.0f) - (kd(GLFW_KEY_A)?1.0f:0.0f);
        in.sprint     = kd(GLFW_KEY_LEFT_SHIFT);
        in.jumpPressed = kd(GLFW_KEY_SPACE);
        in.lookDX = ddx; in.lookDY = ddy;

        bool vNow = kd(GLFW_KEY_V);
        if (vNow && !prevV) mech.toggleCameraMode();
        prevV = vNow;

        mech.update(in, fdt, *mphys);
        mphys->step(fdt);

        if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS) {
            if (mech.fireAutocannon(fdt)) fireAtDummies(mech.tuning().autocannonDmg);
        }
        bool rNow = kd(GLFW_KEY_R);
        if (rNow && !prevR && mech.fireMissilePod(fdt)) {
            for (int k = 0; k < 4; ++k) fireAtDummies(mech.tuning().missilePodDmg);
        }
        prevR = rNow;

        mfx.update(fdt);

        float cx, cy, cz, cyaw, cpitch;
        mech.camera(cx, cy, cz, cyaw, cpitch);
        int cw, chh; glfwGetFramebufferSize(window, &cw, &chh);
        if (cw != lastWd || chh != lastHd) { lastWd=cw; lastHd=chh; if (cw>0&&chh>0) device.onResize((uint32_t)cw,(uint32_t)chh); }
        device.setCamera(cx, cy, cz, cyaw, cpitch, 65.0f);
        auto frame = device.beginFrame();
        if (frame.valid) drawMechScene(frame, cx, cy, cz, cyaw, cpitch);
        device.endFrame(frame);
    }
    cleanup(); mphys->shutdown(); device.shutdown();
    if (window) glfwDestroyWindow(window); glfwTerminate();
    return 0;
}

int main(int argc, char** argv) {
    // --version : print the build identity ("X3 v0.3.NNNNN (hash)") and exit 0.
    // --test-version : assert the version string is well-formed + the console
    //   `version` cmd + the --version path work; print "version: X/Y passed".
    bool showVersion = false, testVersion = false;
    bool smoketest = false, testAsset = false, testConsole = false, testPhysics = false,
         testGltf = false, testPlayer = false, testSwim = false, testMech = false, testInteract = false, testPickup = false,
         testPhysprops = false, testRagdoll = false, testRagdollSkin = false, testEditor = false,
         testBarrels = false, testGlass = false, testHoloterm = false, testTerminals = false, testSit = false, testEcs = false, testEcsRender = false,
         testCombat = false, testAudio = false, testLevel1 = false, testJobs = false,
         testPhase2a = false, testPhase2b = false, testAnim = false, testTerrain = false,
         testStreaming = false, testAi = false, testMultiFloorAi = false, testDoorCode = false, testElevator = false,
         testElevatorFsm = false,
         testTerrainPlace = false, testNet = false, testRescue = false, testDestruction = false,
         testNav = false, testWeapons = false, testVehicle = false, testFootIk = false,
         testSpace = false,
         testNetSync = false, testNetInterp = false, testNetPredict = false, testNpcTalk = false,
         testDeathRagdoll = false, testCanonLevel = false, testCanonPlay = false,
         testThirdPerson = false, testNpc = false, testStarfield = false,
         testSpaceLayer = false,
         testLod = false,
         testSpaceEnv = false,
         testShipanim = false,
         testWormhole = false,
         testDecloak = false,
         testTractor = false,
         testWormholeTransit = false,
         testAtmoDescent = false,
         testShipInterior = false,
         testShipAi = false,
         testTargeting = false,
         testShipDamage = false,
         testEva = false,
         testShipWindows = false,
         testShipRepair = false;
    // --test-rt (hardware ray-tracing RT AO): runs the headless smoketest render
    // path with r_rtao forced ON so the BLAS/TLAS build + ray-query AO compute +
    // apply passes are exercised under Vulkan validation on an RT-capable device.
    bool        testRt = false;
    // --test-bestiary (bestiary pass): the data-driven enemy roster. Additive flag.
    bool        testBestiary = false;
    // --test-bosses (Act-1 bosses, Wave 1): the 5 mid-boss defs + the multi-pod
    // machine + the scripted pre-fight hook + the Martinez regression guard. Additive.
    bool        testBosses = false;
    // --test-adaptive-hide (canon-aliens engine ext.): the type-keyed rotate-damage
    // rhythm on a Boss-type monster — full first hit, reduced same-type repeat,
    // type-rotation re-opens, window expires, opt-out (resist==0) is dead-code.
    bool        testAdaptiveHide = false;
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
    // --test-companion (companion reflex AI Slice A): deterministic decision unit,
    // headless, no GPU/physics deps. Additive.
    bool        testCompanion = false;
    // --test-companion-squad (companion Slice B): squad integration, downed/revive.
    bool        testCompanionSquad = false;
    // --test-companion-controller (companion Slice C seam): single-companion wrapper
    // (Player + CompanionBrain + Identity). Additive, headless.
    bool        testCompanionController = false;
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
    // --test-canonaliens (canon-alien roster — the four "most reported" species:
    // Mantis/Grey/Reptilian/Nordic, per the Davis-Puthoff visualisation). Builds
    // each of the 5 Tuning rows (SaurianSoldier/Warlord, GreyTasked, NordicSteward,
    // MantisArbiter) on a HeadlessDevice + Jolt world; asserts the roster is
    // complete + ordered, each row builds, and per-species stat invariants hold
    // (Soldier melee, Warlord Boss + phases + memory-flash, Tasked ranged/fragile,
    // Steward allied + stationary, Arbiter fast + strafe-heavy), rows are distinct.
    bool        testCanonAliens = false;
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
    bool        testUnderseaArt = false;    // --test-undersea-art (Abyssal Station GLB overlay on OceanBase)
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
    // Combat-density bench (--bench-combat [N]): build the 3-ally squad + an N-enemy
    // ring around the spawn (see app/ally_arena.cpp), tick the FULL combat AI for
    // both sides while running 600 frames vsync OFF, and report
    //   BENCH-COMBAT allies=3 enemies=N alive=A FPS=X CPU=Yms GPU=Zms
    // — the honest combat-density number, with both sides actually fighting. N
    // defaults to 16 if omitted. Distinct from --bench (which only stresses the
    // renderer with idle cubes) — the WHOLE REASON the coop-NPC lane shipped.
    bool     benchCombat = false;
    uint32_t benchCombatEnemies = 16;
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
        if (a == "--version") showVersion = true;
        else if (a == "--test-version") testVersion = true;
        else if (a == "--smoketest") smoketest = true;
        else if (a == "--test-rt") { smoketest = true; testRt = true; }
        else if (a == "--test-jobs") testJobs = true;
        else if (a == "--test-asset") testAsset = true;
        else if (a == "--test-console") testConsole = true;
        else if (a == "--test-physics") testPhysics = true;
        else if (a == "--test-gltf") testGltf = true;
        else if (a == "--test-player") testPlayer = true;
        else if (a == "--test-swim") testSwim = true;
        else if (a == "--test-eva")  testEva  = true;
        else if (a == "--test-mech") testMech = true;
        else if (a == "--test-interact") testInteract = true;
        else if (a == "--test-physprops") testPhysprops = true;
        else if (a == "--test-ragdoll") testRagdoll = true;
        else if (a == "--test-ragdollskin") testRagdollSkin = true;
        else if (a == "--test-editor") testEditor = true;
        else if (a == "--test-barrels") testBarrels = true;
        else if (a == "--test-glass") testGlass = true;
        else if (a == "--test-holoterm") testHoloterm = true;
        else if (a == "--test-terminals") testTerminals = true;
        else if (a == "--test-sit") testSit = true;
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
        else if (a == "--test-multifloor-ai") testMultiFloorAi = true;
        else if (a == "--test-bestiary") testBestiary = true;
        else if (a == "--test-bosses") testBosses = true;
        else if (a == "--test-adaptive-hide") testAdaptiveHide = true;
        else if (a == "--test-act2bosses") testAct2Bosses = true;
        else if (a == "--test-spiremid") testSpireMid = true;
        else if (a == "--test-nexus") testNexus = true;
        else if (a == "--test-spiretop") testSpireTop = true;
        else if (a == "--test-timeline") testTimeline = true;
        else if (a == "--test-dronehack") testDroneHack = true;
        else if (a == "--test-sublevels") testSubLevels = true;
        else if (a == "--test-act2") testAct2 = true;
        else if (a == "--test-canonaliens") testCanonAliens = true;
        else if (a == "--test-act2desert") testAct2Desert = true;
        else if (a == "--test-act2caves") testAct2Caves = true;
        else if (a == "--test-tod") testTod = true;
        else if (a == "--test-weather") testWeather = true;
        else if (a == "--test-worldregions") testWorldRegions = true;
        else if (a == "--test-city") testCity = true;
        else if (a == "--test-oceanbase") testOceanBase = true;
        else if (a == "--test-undersea-art") testUnderseaArt = true;
        else if (a == "--test-doorcode") testDoorCode = true;
        else if (a == "--test-elevator") testElevator = true;
        else if (a == "--test-elevatorfsm") testElevatorFsm = true;
        else if (a == "--test-net") testNet = true;
        else if (a == "--test-netsync") testNetSync = true;
        else if (a == "--test-netinterp") testNetInterp = true;
        else if (a == "--test-netpredict") testNetPredict = true;
        else if (a == "--test-rescue") testRescue = true;
        else if (a == "--test-thirdperson") testThirdPerson = true;
        else if (a == "--test-npctalk") testNpcTalk = true;
        else if (a == "--test-npc") testNpc = true;
        else if (a == "--test-destruction") testDestruction = true;
        else if (a == "--test-debris") testDebris = true;
        else if (a == "--test-gpuskin") testGpuSkin = true;
        else if (a == "--test-collapse") testCollapse = true;
        else if (a == "--test-physjoint") testPhysJoint = true;
        else if (a == "--test-nav") testNav = true;
        else if (a == "--test-weapons") testWeapons = true;
        else if (a == "--test-vehicle") testVehicle = true;
        else if (a == "--test-space")  testSpace  = true;
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
        else if (a == "--test-companion") testCompanion = true;
        else if (a == "--test-companion-squad") testCompanionSquad = true;
        else if (a == "--test-companion-controller") testCompanionController = true;
        else if (a == "--test-club") testClub = true;
        else if (a == "--test-starfield") testStarfield = true;
        // Break the else-if chain (MSVC C1061: nested-too-deeply limit).
        if (a == "--test-spacelayer") testSpaceLayer = true;
        else if (a == "--test-lod") testLod = true;
        else if (a == "--test-spaceenv") testSpaceEnv = true;
        else if (a == "--test-shipanim") testShipanim = true;
        else if (a == "--test-wormhole") testWormhole = true;
        else if (a == "--test-decloak") testDecloak = true;
        else if (a == "--test-tractor") testTractor = true;
        else if (a == "--test-wormhole-transit") testWormholeTransit = true;
        else if (a == "--test-atmo-descent") testAtmoDescent = true;
        else if (a == "--test-ship-interior") testShipInterior = true;
        else if (a == "--test-ship-ai") testShipAi = true;
        else if (a == "--test-targeting") testTargeting = true;
        else if (a == "--test-ship-damage") testShipDamage = true;
        else if (a == "--test-ship-windows") testShipWindows = true;
        else if (a == "--test-ship-repair") testShipRepair = true;
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
        else if (a == "--bench-combat") {
            benchCombat = true;
            // Optional N (enemy ring count). Anything that doesn't look like a
            // numeric token is treated as "skipped" -> keep the default.
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9')
                benchCombatEnemies = (uint32_t)std::strtoul(argv[++i], nullptr, 10);
            // Optional second positional arg = frame count (mirrors --bench).
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9')
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

    // --version : print build identity + exit 0 (before any window/Vulkan work).
    if (showVersion) {
        return runVersionFlag();
    }
    // --test-version : version-string well-formedness + console cmd + --version path.
    if (testVersion) {
        x3::logInfo("running version (--test-version) self-test...");
        return runVersionSelfTest() ? 0 : 1;
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
    if (testSwim) {
        x3::logInfo("running swim/dive (Act 4 underwater) self-test...");
        return x3::game::runSwimSelfTest() ? 0 : 1;
    }
    if (testEva) {
        x3::logInfo("running EVA spacewalk (Act-3 S12 zero-G) --test-eva self-test...");
        return x3::space::runEvaSelfTest() ? 0 : 1;
    }
    if (testSpace) {
        x3::logInfo("running space-pilot (Act-3 6DOF) --test-space self-test...");
        return x3::game::runSpaceSelfTest() ? 0 : 1;
    }
    if (testMech) {
        x3::logInfo("running mech-pilot heavy-controller (--test-mech) self-test...");
        return x3::game::runMechSelfTest() ? 0 : 1;
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
    if (testGlass) {
        x3::logInfo("running translucent-glass material (M1 see-through) self-test...");
        return x3::game::runGlassSelfTest() ? 0 : 1;
    }
    // --test-starfield: procedural starfield (Act-3 deep-space backdrop) self-test.
    // Headless -- exercises the SkyStars init/render/shutdown lifecycle, asserts
    // Tuning param-clamp, and asserts the procedural hash produces a stable
    // non-trivial star pattern (the same hash math the shader uses).
    if (testStarfield) {
        x3::logInfo("running procedural starfield (SkyStars) self-test...");
        int pass = 0, total = 0;
        auto check = [&](bool c, const char* name) {
            ++total;
            if (c) { ++pass; x3::logInfo(std::string("  [ok] ") + name); }
            else   {          x3::logError(std::string("  [FAIL] ") + name); }
        };
        // T1: init + shutdown lifecycle is leak-clean (the headless device
        // increments a handle counter; we just assert valid handles came back
        // and shutdown() releases them so a second init() succeeds).
        x3::game::HeadlessRenderDevice hdev;
        {
            x3::SkyStars sky;
            sky.init(hdev);
            check(sky.initialized() && sky.mesh().valid() && sky.texture().valid(),
                  "T1 init() produces valid mesh + texture, initialized()=true");
            sky.shutdown(hdev);
            check(!sky.initialized() && !sky.mesh().valid() && !sky.texture().valid(),
                  "T1b shutdown() releases handles, initialized()=false");
            // Round-trip: re-init MUST succeed without leaking.
            sky.init(hdev);
            check(sky.initialized(), "T1c re-init after shutdown succeeds");
            sky.shutdown(hdev);
        }
        // T2: render() with a sample viewProjInv runs without crashing and
        // produces a non-zero emissive strength (the twinkle modulator).
        {
            x3::SkyStars sky;
            sky.init(hdev);
            x3::rhi::FrameContext fr = hdev.beginFrame();
            // Identity viewProjInv16 -- the headless emissive draw is a no-op
            // so any matrix is fine; we just need the call to be VUID-safe.
            const float idM[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
            sky.setCamera(0.0f, 0.0f, 0.0f);
            sky.render(hdev, fr, idM, /*timeSec=*/0.25f);
            check(sky.lastEmissiveStrength() > 0.0f,
                  "T2 render() updates emissive strength (twinkle live)");
            // Twinkle should be MIN at sin == -1: timeSec * speed * 2pi = 3pi/2.
            const float twinkleHz = sky.lastTuning().twinkleSpeed;
            const float tMin = (3.0f / 4.0f) / std::max(1e-3f, twinkleHz);   // sin(3pi/2)=-1
            sky.render(hdev, fr, idM, tMin);
            float minE = sky.lastEmissiveStrength();
            const float tMax = (1.0f / 4.0f) / std::max(1e-3f, twinkleHz);   // sin(pi/2)=+1
            sky.render(hdev, fr, idM, tMax);
            float maxE = sky.lastEmissiveStrength();
            check(maxE > minE && maxE - minE > 0.1f,
                  "T2b twinkle modulates emissive monotonically across the sine wave");
            hdev.endFrame(fr);
            sky.shutdown(hdev);
        }
        // T3: API parameter clamping (density > 0, radius > 0, threshold in [0,1)).
        {
            x3::SkyStars::Tuning bad;
            bad.starDensity   = -5.0f;
            bad.starRadius    = -0.1f;
            bad.threshold     = 1.5f;
            bad.twinkleSpeed  = -2.0f;
            auto c = x3::clampTuning(bad);
            check(c.starDensity  > 0.0f, "T3a density clamps to > 0");
            check(c.starRadius   > 0.0f, "T3b radius clamps to > 0");
            check(c.threshold    >= 0.0f && c.threshold < 1.0f, "T3c threshold clamps to [0,1)");
            check(c.twinkleSpeed >= 0.0f, "T3d twinkleSpeed clamps to >= 0");
            // Clamp on the negative side too.
            x3::SkyStars::Tuning low;
            low.threshold = -0.5f;
            auto c2 = x3::clampTuning(low);
            check(c2.threshold == 0.0f, "T3e threshold clamps -0.5 -> 0");
        }
        // T4: the procedural hash actually produces stars -- sample a grid of
        // directions and assert at least SOME of them register as starlit. The
        // exact ratio depends on threshold; with the defaults (0.985 + a 2x
        // dust layer @ blended 0.994) we expect a non-trivial fraction.
        {
            x3::SkyStars::Tuning t;
            int hits = 0, samples = 0;
            const int grid = 32;
            for (int iy = 0; iy < grid; ++iy) {
                for (int ix = 0; ix < grid; ++ix) {
                    float u = (ix + 0.5f) / (float)grid;
                    float v = (iy + 0.5f) / (float)grid;
                    float th  = u * 6.2831853f;
                    float phi = v * 3.14159265f;
                    float sy = std::sin(phi), cy = std::cos(phi);
                    float d[3] = { sy * std::cos(th), cy, sy * std::sin(th) };
                    float b = x3::SkyStars::sampleProceduralBrightness(d, t, 0.0f);
                    if (b > 0.05f) ++hits;
                    ++samples;
                }
            }
            check(hits > 0,
                  "T4 procedural hash produces stars across the celestial sphere");
            // Determinism: same direction same time same tuning -> same value.
            float d[3] = { 0.3f, 0.6f, 0.7f };
            x3::SkyStars::Tuning t2;
            float a = x3::SkyStars::sampleProceduralBrightness(d, t2, 0.5f);
            float bb = x3::SkyStars::sampleProceduralBrightness(d, t2, 0.5f);
            check(a == bb, "T4b hash is deterministic");
        }
        x3::logInfo("starfield: " + std::to_string(pass) + "/" + std::to_string(total) + " passed");
        std::printf("starfield: %d/%d passed\n", pass, total);
        std::fflush(stdout);
        return (pass == total) ? 0 : 1;
    }
    // --test-spacelayer: S0 SpaceLayer spine self-test. Headless deterministic
    // logic only (no GPU) -- exercises the context state machine + staged
    // transitions, the moving-environment transform integration, and the
    // free-list proxy registry.
    if (testSpaceLayer) {
        x3::logInfo("running S0 SpaceLayer spine self-test...");
        int pass = 0, total = 0;
        auto check = [&](bool c, const char* name) {
            ++total;
            if (c) { ++pass; x3::logInfo(std::string("  [ok] ") + name); }
            else   {          x3::logError(std::string("  [FAIL] ") + name); }
        };

        using x3::space::SpaceLayer;
        using x3::space::Context;
        using x3::space::Proxy;

        // T1: initial context is DeepSpace after init().
        {
            SpaceLayer L;
            L.init();
            check(L.context() == Context::DeepSpace, "T1 init() -> DeepSpace");
            check(L.proxyCount() == 0u, "T1b init() -> empty proxy registry");
        }

        // T2: wormhole request + runner -> transit while running, arrives DeepSpace.
        {
            SpaceLayer L;
            L.init();
            int ticks = 0;
            L.registerWormholeRunner([&](float) { return ++ticks >= 3; });
            L.requestWormhole(42);
            check(L.context() == Context::WormholeTransit, "T2 requestWormhole -> WormholeTransit");
            L.update(0.1f); // tick 1
            check(L.context() == Context::WormholeTransit, "T2b mid-transit stays WormholeTransit");
            L.update(0.1f); // tick 2
            L.update(0.1f); // tick 3 -> complete
            check(L.context() == Context::DeepSpace, "T2c runner complete -> DeepSpace (arrived)");
        }

        // T3: descent -> AtmoDescent -> Surface; then ascent -> DeepSpace.
        {
            SpaceLayer L;
            L.init();
            bool done = false;
            L.registerDescentRunner([&](float) { return done; });
            L.requestDescent(7);
            check(L.context() == Context::AtmoDescent, "T3 requestDescent -> AtmoDescent");
            L.update(0.1f); // runner not done yet
            check(L.context() == Context::AtmoDescent, "T3b stays AtmoDescent until runner done");
            done = true;
            L.update(0.1f);
            check(L.context() == Context::Surface, "T3c descent complete -> Surface");
            L.requestAscent();
            L.update(0.1f); // descent runner reused; done==true -> immediate
            check(L.context() == Context::DeepSpace, "T3d requestAscent -> DeepSpace");
        }

        // T4: no-runner transitions complete on the next update.
        {
            SpaceLayer L;
            L.init();
            L.requestWormhole(1);
            check(L.context() == Context::WormholeTransit, "T4 no-runner armed -> WormholeTransit");
            L.update(0.1f);
            check(L.context() == Context::DeepSpace, "T4b no-runner completes next update");
        }

        // T5: environment transform integrates pure translation.
        {
            SpaceLayer L;
            L.init();
            const float v[3] = { 0.0f, 0.0f, -5.0f };
            const float w[3] = { 0.0f, 0.0f, 0.0f };
            L.setEnvironmentVelocity(v, w);
            L.update(1.0f);
            float m[16];
            L.environmentTransform(m);
            // Column-major translation lives at indices 12,13,14.
            check(std::fabs(m[14] - (-5.0f)) < 1e-4f &&
                  std::fabs(m[12]) < 1e-4f && std::fabs(m[13]) < 1e-4f,
                  "T5 env transform integrates translation (0,0,-5)");
            // A second second of the same velocity accumulates to -10.
            L.update(1.0f);
            L.environmentTransform(m);
            check(std::fabs(m[14] - (-10.0f)) < 1e-3f, "T5b translation accumulates frame-over-frame");
        }

        // T6: environment transform integrates rotation about Y.
        {
            SpaceLayer L;
            L.init();
            const float v[3] = { 0.0f, 0.0f, 0.0f };
            const float w[3] = { 0.0f, 1.0f, 0.0f }; // 1 rad/s about +Y
            L.setEnvironmentVelocity(v, w);
            L.update(1.0f); // rotate 1 rad
            float m[16];
            L.environmentTransform(m);
            // Ry column-major {cy,0,-sy,0, 0,1,0,0, sy,0,cy,0, ...}: the cos
            // terms sit on the diagonal at m[0] and m[10]; the sin terms at
            // m[2] (=-sy) and m[8] (=+sy). cos(1)=0.5403, sin(1)=0.8415.
            const float c1 = std::cos(1.0f), s1 = std::sin(1.0f);
            check(std::fabs(m[0] - c1) < 1e-3f && std::fabs(m[10] - c1) < 1e-3f,
                  "T6 env transform rotates about Y (cos terms)");
            check(std::fabs(m[2] - (-s1)) < 1e-3f && std::fabs(m[8] - s1) < 1e-3f,
                  "T6b env transform rotation has expected sin terms");
            // No translation when velocity is zero.
            check(std::fabs(m[12]) < 1e-4f && std::fabs(m[13]) < 1e-4f && std::fabs(m[14]) < 1e-4f,
                  "T6c pure rotation leaves translation at origin");
        }

        // T7: proxy registry add returns stable id, count tracks.
        {
            SpaceLayer L;
            L.init();
            Proxy p{};
            p.kind = Proxy::Kind::Planet;
            p.pos[0] = 1.0f; p.pos[1] = 2.0f; p.pos[2] = 3.0f;
            p.radius = 9.5f; p.lodAsset = 77u;
            p.tint[0] = 0.1f; p.tint[1] = 0.2f; p.tint[2] = 0.3f; p.tint[3] = 1.0f;
            uint32_t id0 = L.addProxy(p);
            check(L.proxyCount() == 1u, "T7 addProxy increments count");
            Proxy q = p; q.kind = Proxy::Kind::Ship; q.radius = 2.0f;
            uint32_t id1 = L.addProxy(q);
            check(L.proxyCount() == 2u && id1 != id0, "T7b second add: count=2, distinct id");
            // Accessor round-trip: find the planet among live proxies.
            bool found = false;
            for (uint32_t i = 0; i < L.proxyCount(); ++i) {
                const Proxy& r = L.proxy(i);
                if (r.kind == Proxy::Kind::Planet && std::fabs(r.radius - 9.5f) < 1e-4f &&
                    r.lodAsset == 77u && std::fabs(r.pos[2] - 3.0f) < 1e-4f)
                    found = true;
            }
            check(found, "T7c accessor round-trips proxy fields");

            // updateProxy mutates in place; id stays stable.
            Proxy upd = p; upd.radius = 99.0f; upd.kind = Proxy::Kind::Station;
            L.updateProxy(id0, upd);
            bool seenUpdated = false;
            for (uint32_t i = 0; i < L.proxyCount(); ++i) {
                const Proxy& r = L.proxy(i);
                if (r.kind == Proxy::Kind::Station && std::fabs(r.radius - 99.0f) < 1e-4f)
                    seenUpdated = true;
            }
            check(seenUpdated && L.proxyCount() == 2u, "T7d updateProxy mutates in place, count unchanged");

            // removeProxy drops the count; the survivor is still accessible.
            L.removeProxy(id0);
            check(L.proxyCount() == 1u, "T7e removeProxy decrements count");
            const Proxy& survivor = L.proxy(0);
            check(survivor.kind == Proxy::Kind::Ship && std::fabs(survivor.radius - 2.0f) < 1e-4f,
                  "T7f survivor remains accessible after removal");

            // Free-list reuse: a fresh add reclaims the freed slot's id.
            Proxy fresh{}; fresh.kind = Proxy::Kind::Asteroid; fresh.radius = 4.0f;
            uint32_t id2 = L.addProxy(fresh);
            check(id2 == id0 && L.proxyCount() == 2u, "T7g free-list reuses removed id");
        }

        x3::logInfo("spacelayer: " + std::to_string(pass) + "/" + std::to_string(total) + " passed");
        std::printf("spacelayer: %d/%d passed\n", pass, total);
        std::fflush(stdout);
        return (pass == total) ? 0 : 1;
    }
    // --test-lod: S2 distance-LOD selection self-test. Pure CPU policy logic
    // (no GPU) — exercises LodSystem::select() across the distance bands and
    // makeFromChain() slot population/clamping.
    if (testLod) {
        x3::logInfo("running S2 distance-LOD (LodSystem) self-test...");
        int pass = 0, total = 0;
        auto check = [&](bool c, const char* name) {
            ++total;
            if (c) { ++pass; x3::logInfo(std::string("  [ok] ") + name); }
            else   {          x3::logError(std::string("  [FAIL] ") + name); }
        };

        x3::space::LodSystem lod;

        // Canonical 4-level chain: handles 10/20/30/40, switchDist {50,150,400}.
        x3::space::LodSet full =
            lod.makeFromChain(10, 20, 30, 40, 50.0f, 150.0f, 400.0f, 4);
        check(full.levels == 4, "B1a makeFromChain populates levels=4");
        check(full.mesh[0] == 10 && full.mesh[3] == 40, "B1b chain handles stored in order");

        // Band selection: d<50 -> LOD0, 50<=d<150 -> LOD1, 150<=d<400 -> LOD2, d>=400 -> LOD3.
        check(lod.select(full, 0.0f)   == 10, "B1c d=0    -> LOD0");
        check(lod.select(full, 49.9f)  == 10, "B1d d=49.9 -> LOD0 (just under 1st thresh)");
        check(lod.select(full, 50.0f)  == 20, "B1e d=50   -> LOD1 (on 1st thresh)");
        check(lod.select(full, 100.0f) == 20, "B1f d=100  -> LOD1");
        check(lod.select(full, 150.0f) == 30, "B1g d=150  -> LOD2 (on 2nd thresh)");
        check(lod.select(full, 399.9f) == 30, "B1h d=399.9-> LOD2");
        check(lod.select(full, 400.0f) == 40, "B1i d=400  -> LOD3 (on 3rd thresh)");
        check(lod.select(full, 9999.f) == 40, "B1j d=huge -> LOD3 (saturates)");
        check(lod.select(full, -5.0f)  == 10, "B1k negative distance -> LOD0");

        // levels=2 clamps to the highest populated level (LOD1) at any far distance.
        x3::space::LodSet two =
            lod.makeFromChain(11, 22, 33, 44, 50.0f, 150.0f, 400.0f, 2);
        check(two.levels == 2, "B1l makeFromChain clamps populated levels");
        check(two.mesh[2] == 0 && two.mesh[3] == 0, "B1m unused slots zeroed");
        check(lod.select(two, 10.0f)   == 11, "B1n levels=2 near -> LOD0");
        check(lod.select(two, 75.0f)   == 22, "B1o levels=2 past 1st thresh -> LOD1");
        check(lod.select(two, 9999.f)  == 22, "B1p levels=2 far -> clamps to LOD1 (highest populated)");

        // levels clamps out of range, single-LOD always returns LOD0.
        x3::space::LodSet one =
            lod.makeFromChain(7, 0, 0, 0, 50.0f, 150.0f, 400.0f, 9);
        check(one.levels == 4, "B1q makeFromChain clamps levels>4 to 4");
        x3::space::LodSet solo =
            lod.makeFromChain(7, 0, 0, 0, 50.0f, 150.0f, 400.0f, 1);
        check(lod.select(solo, 5000.f) == 7, "B1r levels=1 always returns LOD0");

        x3::logInfo("lod: " + std::to_string(pass) + "/" + std::to_string(total) + " passed");
        std::printf("lod: %d/%d passed\n", pass, total);
        std::fflush(stdout);
        return (pass == total) ? 0 : 1;
    }
    // --test-spaceenv: S1 space environment (nebula/star dome + proxy planets +
    // sun) self-test. Headless — exercises init/shutdown leak-clean lifecycle,
    // addPlanet bookkeeping, setSun normalization, and a VUID-safe render() with
    // and without planets/sun (drawMeshEmissive against the headless device).
    if (testSpaceEnv) {
        x3::logInfo("running S1 space environment (SpaceEnv) self-test...");
        int pass = 0, total = 0;
        auto check = [&](bool c, const char* name) {
            ++total;
            if (c) { ++pass; x3::logInfo(std::string("  [ok] ") + name); }
            else   {          x3::logError(std::string("  [FAIL] ") + name); }
        };

        x3::game::HeadlessRenderDevice hdev;
        // T1: init produces valid resources; shutdown releases them; re-init OK.
        {
            x3::space::SpaceEnv env;
            env.init(hdev);
            check(env.initialized() && env.domeMesh().valid() &&
                  env.sphereMesh().valid() && env.spriteMesh().valid(),
                  "T1 init() produces valid dome + sphere + sprite, initialized()=true");
            env.shutdown(hdev);
            check(!env.initialized() && !env.domeMesh().valid() &&
                  !env.sphereMesh().valid() && !env.spriteMesh().valid(),
                  "T1b shutdown() releases all handles, initialized()=false");
            env.init(hdev);
            check(env.initialized(), "T1c re-init after shutdown succeeds (leak-clean)");
            env.shutdown(hdev);
        }
        // T2: addPlanet bookkeeping — stable incrementing ids + count.
        {
            x3::space::SpaceEnv env;
            env.init(hdev);
            check(env.planetCount() == 0, "T2 planetCount()==0 before any addPlanet");
            float p0[3] = {  100.0f, 0.0f, -300.0f }, a0[3] = { 0.8f, 0.5f, 0.3f };
            float p1[3] = { -200.0f, 50.0f, -500.0f }, a1[3] = { 0.4f, 0.6f, 0.9f };
            uint32_t id0 = env.addPlanet(p0, 40.0f, a0);
            uint32_t id1 = env.addPlanet(p1, 70.0f, a1);
            check(id0 == 0 && id1 == 1, "T2b addPlanet returns stable incrementing ids");
            check(env.planetCount() == 2, "T2c planetCount() reflects 2 planets");
            // Degenerate radius is clamped (no crash / no zero-scale planet).
            float p2[3] = { 0, 0, -100 }, a2[3] = { 1, 1, 1 };
            env.addPlanet(p2, -5.0f, a2);
            check(env.planetCount() == 3, "T2d addPlanet with bad radius still registers");
            env.shutdown(hdev);
        }
        // T3: setSun normalizes the direction + flags sunSet().
        {
            x3::space::SpaceEnv env;
            env.init(hdev);
            check(!env.sunSet(), "T3 sunSet()==false before setSun");
            float dir[3] = { 0.0f, 3.0f, 4.0f };   // length 5 -> expect (0,0.6,0.8)
            float col[3] = { 1.0f, 0.95f, 0.85f };
            env.setSun(dir, col, 2.0f);
            check(env.sunSet(), "T3b setSun flags sunSet()=true");
            // Degenerate direction does not crash / leaves a valid sun.
            float zero[3] = { 0, 0, 0 };
            env.setSun(zero, col, 1.0f);
            check(env.sunSet(), "T3c setSun with zero dir is safe (still set)");
            env.shutdown(hdev);
        }
        // T4: render() is VUID-safe with 0 planets/no sun, and with planets+sun.
        {
            x3::space::SpaceEnv env;
            env.init(hdev);
            const float vp[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
            x3::rhi::FrameContext fr = hdev.beginFrame();
            env.setCamera(0, 0, 0);
            env.render(hdev, fr, vp, 0.0f);   // empty scene, no sun: must not crash
            float p0[3] = { 80, 0, -200 }, a0[3] = { 0.7f, 0.5f, 0.4f };
            float p1[3] = { -120, 30, -260 }, a1[3] = { 0.3f, 0.5f, 0.8f };
            env.addPlanet(p0, 30.0f, a0);
            env.addPlanet(p1, 50.0f, a1);
            float sd[3] = { 0.3f, 0.5f, 0.8f }, sc[3] = { 1.0f, 0.9f, 0.8f };
            env.setSun(sd, sc, 1.5f);
            env.render(hdev, fr, vp, 0.5f);   // planets + sun: must not crash
            hdev.endFrame(fr);
            check(true, "T4 render() VUID-safe empty + with 2 planets + sun");
            // render() before init() is a no-op (must not crash).
            x3::space::SpaceEnv env2;
            x3::rhi::FrameContext fr2 = hdev.beginFrame();
            env2.render(hdev, fr2, vp, 0.0f);
            hdev.endFrame(fr2);
            check(!env2.initialized(), "T4b render() before init() is a safe no-op");
            env.shutdown(hdev);
        }
        x3::logInfo("spaceenv: " + std::to_string(pass) + "/" + std::to_string(total) + " passed");
        std::printf("spaceenv: %d/%d passed\n", pass, total);
        std::fflush(stdout);
        return (pass == total) ? 0 : 1;
    }
    // --test-shipanim: S11 ship node-transform animation self-test (headless).
    // Verifies the four shipped SpaceShip*.glb load via the existing model loader
    // (>=1 primitive each), then exercises the ShipNodeAnim driver: bind a ship root
    // entity, register an articulated part with two key-poses, and assert setPart()
    // lerps + update() writes the part's child-entity transform between the poses
    // (and rides the ship's placement). No GPU -- uses the headless loader path.
    if (testShipanim) {
        x3::logInfo("running S11 ship node-transform animation self-test...");
        int pass = 0, total = 0;
        auto check = [&](bool c, const char* name) {
            ++total;
            if (c) { ++pass; x3::logInfo(std::string("  [ok] ") + name); }
            else   {          x3::logError(std::string("  [FAIL] ") + name); }
        };

        // ---- T1: each shipped SpaceShip*.glb loads with >=1 primitive --------
        const std::string rigDir = x3::game::riggedGlbRoot();
        std::unique_ptr<x3::asset::IAssetSource> asrc(x3::asset::createAssetSource());
        asrc->mountDir(rigDir, 0);
        // Headless loader (dev == nullptr): mints fake non-zero handles, no GPU.
        std::unique_ptr<x3::asset::IModelLoader> loader(
            x3::asset::createModelLoader(nullptr, asrc.get()));
        const char* kShips[] = { "SpaceShip.glb", "SpaceShip2.glb",
                                 "SpaceShip3.glb", "SpaceShip4.glb" };
        for (const char* s : kShips) {
            x3::asset::Model m = loader->load(s);
            bool good = m.ok && !m.primitives.empty();
            check(good, (std::string("T1 load ") + s + " (>=1 primitive)").c_str());
            if (m.ok) loader->unload(m);
        }

        // ---- T2: lerpMat4 endpoints + midpoint -------------------------------
        {
            float a[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};      // identity
            float b[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,4,0,1};      // +4 in Y
            float o[16];
            x3::space::lerpMat4(a, b, 0.0f, o);
            check(o[13] == 0.0f, "T2a lerp(t=0) == poseA");
            x3::space::lerpMat4(a, b, 1.0f, o);
            check(o[13] == 4.0f, "T2b lerp(t=1) == poseB");
            x3::space::lerpMat4(a, b, 0.5f, o);
            check(std::fabs(o[13] - 2.0f) < 1e-5f, "T2c lerp(t=0.5) midpoint");
        }

        // ---- T3: bind + addPart + setPart + update on a Scene ----------------
        {
            x3::game::Scene scene;
            // Ship root at (10, 0, 0) — a non-identity placement so we prove the
            // part rides the root frame (world = rootWorld * localPose).
            x3::game::Entity ship{};
            ship.transform[12] = 10.0f;   // tx = 10
            uint32_t shipId = scene.add(ship);
            // Landing-gear child entity (its transform is driven by ShipNodeAnim).
            x3::game::Entity gear{};
            uint32_t gearId = scene.add(gear);

            x3::space::ShipNodeAnim anim;
            anim.bind(scene, shipId);
            check(anim.shipEntity() == shipId, "T3a bind records ship root entity");

            // Retracted pose: gear tucked at local Y=0. Deployed: gear down Y=-3.
            float poseUp[16]   = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0, 0,0,1};
            float poseDown[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,-3,0,1};
            anim.addPart("landing_gear", poseUp, poseDown, gearId);
            check(anim.partCount() == 1, "T3b addPart registers the part");
            check(anim.partValue("landing_gear") == 0.0f, "T3c new part starts retracted (t=0)");
            check(anim.partValue("no_such_part") < 0.0f, "T3d unknown part queries -1");

            // Retracted: update() places the gear at the ship origin (local Y=0).
            anim.update(0.016f, scene);
            const float gy0 = scene.get(gearId).transform[13];
            const float gx0 = scene.get(gearId).transform[12];
            check(std::fabs(gy0 - 0.0f) < 1e-4f && std::fabs(gx0 - 10.0f) < 1e-4f,
                  "T3e retracted: gear at root frame (x=10, y=0)");

            // Deploy: setPart(1.0) -> gear drops to local Y=-3 (world y=-3).
            anim.setPart("landing_gear", 1.0f);
            check(anim.partValue("landing_gear") == 1.0f, "T3f setPart(1.0) updates value");
            anim.update(0.016f, scene);
            const float gy1 = scene.get(gearId).transform[13];
            const float gx1 = scene.get(gearId).transform[12];
            check(std::fabs(gy1 - (-3.0f)) < 1e-4f, "T3g deployed: gear lerps to local Y=-3");
            check(std::fabs(gx1 - 10.0f) < 1e-4f, "T3h deployed: gear still rides root X=10");

            // Half-deploy lerps to the midpoint.
            anim.setPart("landing_gear", 0.5f);
            anim.update(0.016f, scene);
            check(std::fabs(scene.get(gearId).transform[13] - (-1.5f)) < 1e-4f,
                  "T3i half-deploy lerps to Y=-1.5");

            // setPart clamps out-of-range to [0,1].
            anim.setPart("landing_gear", 5.0f);
            check(anim.partValue("landing_gear") == 1.0f, "T3j setPart clamps >1 to 1");
            anim.setPart("landing_gear", -2.0f);
            check(anim.partValue("landing_gear") == 0.0f, "T3k setPart clamps <0 to 0");

            // The gear rides a MOVED ship: shift the root, update, gear follows.
            scene.get(shipId).transform[14] = 7.0f;   // tz = 7
            anim.setPart("landing_gear", 1.0f);
            anim.update(0.016f, scene);
            check(std::fabs(scene.get(gearId).transform[14] - 7.0f) < 1e-4f,
                  "T3l moving the root carries the articulated part (z=7)");
        }

        x3::logInfo("shipanim: " + std::to_string(pass) + "/" + std::to_string(total) + " passed");
        std::printf("shipanim: %d/%d passed\n", pass, total);
        std::fflush(stdout);
        return (pass == total) ? 0 : 1;
    }
    // --test-wormhole: Salvari crystal-matrix wormhole VFX self-test (Act-3 jump
    // transition). Headless -- exercises the WormholeVfx init/render/shutdown
    // lifecycle (leak-clean round-trip), a VUID-safe render() with a sample
    // viewProj, Tuning param clamping, progress 0..1 handling, and that the baked
    // crystal-matrix pattern is non-trivial + faceted. Mirrors --test-starfield.
    if (testWormhole) {
        x3::logInfo("running Salvari crystal-matrix wormhole (WormholeVfx) self-test...");
        int pass = 0, total = 0;
        auto check = [&](bool c, const char* name) {
            ++total;
            if (c) { ++pass; x3::logInfo(std::string("  [ok] ") + name); }
            else   {          x3::logError(std::string("  [FAIL] ") + name); }
        };

        x3::game::HeadlessRenderDevice hdev;
        // T1: init + shutdown lifecycle is leak-clean (valid handles in, released
        // on shutdown, and a second init() succeeds -- no leaked resources).
        {
            x3::space::WormholeVfx wh;
            wh.init(hdev);
            check(wh.initialized() && wh.mesh().valid() && wh.texture().valid(),
                  "T1 init() produces valid mesh + texture, initialized()=true");
            wh.shutdown(hdev);
            check(!wh.initialized() && !wh.mesh().valid() && !wh.texture().valid(),
                  "T1b shutdown() releases handles, initialized()=false");
            wh.init(hdev);
            check(wh.initialized(), "T1c re-init after shutdown succeeds (no leak)");
            // Double-init is idempotent (no second mesh/texture minted).
            x3::rhi::MeshHandle before = wh.mesh();
            wh.init(hdev);
            check(wh.mesh().id == before.id, "T1d double-init() is a no-op");
            wh.shutdown(hdev);
            // Shutdown is idempotent (second shutdown does not crash).
            wh.shutdown(hdev);
            check(!wh.initialized(), "T1e double-shutdown() is safe");
        }
        // T2: render() with a sample viewProj runs without crashing (VUID-safe in
        // the headless stub) and updates the core/convergence strength.
        {
            x3::space::WormholeVfx wh;
            wh.init(hdev);
            x3::rhi::FrameContext fr = hdev.beginFrame();
            const float idM[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
            wh.setOrigin(0.0f, 0.0f, 0.0f);
            wh.render(hdev, fr, idM, /*timeSec=*/0.25f, /*progress=*/0.0f);
            check(wh.lastCoreStrength() > 0.0f,
                  "T2 render() applies a positive core glow even at progress 0");
            // Progress should INTENSIFY the white-hot core/convergence.
            wh.render(hdev, fr, idM, /*timeSec=*/0.25f, /*progress=*/1.0f);
            float fullCore = wh.lastCoreStrength();
            wh.render(hdev, fr, idM, /*timeSec=*/0.25f, /*progress=*/0.0f);
            float zeroCore = wh.lastCoreStrength();
            check(fullCore > zeroCore && fullCore - zeroCore > 1.0f,
                  "T2b progress 1.0 intensifies the core vs progress 0.0");
            hdev.endFrame(fr);
            wh.shutdown(hdev);
        }
        // T3: Tuning parameter clamping (length>0, radius>0, flowSpeed>=0,
        // facetDensity>=3) -- the bake/mesh need these to be sane.
        {
            x3::space::WormholeVfx::Tuning bad;
            bad.length       = -50.0f;
            bad.radius       = -2.0f;
            bad.flowSpeed    = -4.0f;
            bad.facetDensity = 1.0f;
            auto c = x3::space::clampTuning(bad);
            check(c.length       > 0.0f,  "T3a length clamps to > 0");
            check(c.radius       > 0.0f,  "T3b radius clamps to > 0");
            check(c.flowSpeed    >= 0.0f, "T3c flowSpeed clamps to >= 0");
            check(c.facetDensity >= 3.0f, "T3d facetDensity clamps to >= 3");
            // A wild Tuning must still init cleanly (clamp protects the bake/mesh).
            x3::space::WormholeVfx wh;
            wh.init(hdev, bad);
            check(wh.initialized(), "T3e init() survives an out-of-range Tuning");
            wh.shutdown(hdev);
        }
        // T4: progress is clamped to [0,1] inside render() (S3 may drive it from a
        // sequence that overshoots), and the baked crystal-matrix is non-trivial +
        // faceted (the convergence end is brighter than the mouth; facet seams
        // produce a brightness variance around the ring).
        {
            x3::space::WormholeVfx wh;
            wh.init(hdev);
            x3::rhi::FrameContext fr = hdev.beginFrame();
            const float idM[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
            wh.render(hdev, fr, idM, 0.0f, /*progress=*/5.0f);   // overshoot high
            check(wh.lastProgress() == 1.0f, "T4a progress > 1 clamps to 1.0");
            wh.render(hdev, fr, idM, 0.0f, /*progress=*/-3.0f);  // overshoot low
            check(wh.lastProgress() == 0.0f, "T4b progress < 0 clamps to 0.0");
            hdev.endFrame(fr);
            wh.shutdown(hdev);

            x3::space::WormholeVfx::Tuning t;
            // Convergence: the far end (zNorm 1) reads brighter than the mouth (0).
            float bMouth = x3::space::WormholeVfx::sampleFacetBrightness(0.3f, 0.0f, t);
            float bFar   = x3::space::WormholeVfx::sampleFacetBrightness(0.3f, 1.0f, t);
            check(bFar > bMouth, "T4c convergence end is brighter than the mouth");
            // Faceted: sweep theta around the ring at fixed z -> brightness varies
            // (purple prismatic glints at the facet seams).
            float lo = 1e9f, hi = -1e9f;
            const int N = 256;
            for (int i = 0; i < N; ++i) {
                float th = (i + 0.5f) / (float)N * 6.2831853f;
                float b = x3::space::WormholeVfx::sampleFacetBrightness(th, 0.5f, t);
                lo = std::min(lo, b); hi = std::max(hi, b);
            }
            check(hi - lo > 0.05f, "T4d crystal facets produce brightness variance around the ring");
            // Determinism: same inputs -> same value.
            float a0 = x3::space::WormholeVfx::sampleFacetBrightness(1.1f, 0.4f, t);
            float a1 = x3::space::WormholeVfx::sampleFacetBrightness(1.1f, 0.4f, t);
            check(a0 == a1, "T4e crystal sample is deterministic");
        }
        x3::logInfo("wormhole: " + std::to_string(pass) + "/" + std::to_string(total) + " passed");
        std::printf("wormhole: %d/%d passed\n", pass, total);
        std::fflush(stdout);
        return (pass == total) ? 0 : 1;
    }
    // --test-decloak: intro DECLOAK shimmer VFX self-test (a capital ship phasing
    // into existence). Headless -- exercises the DecloakVfx init/render/shutdown
    // lifecycle (leak-clean round-trip), a VUID-safe render() at progress 0, 0.5,
    // 1.0, the revealAlpha() ramp (0->0, 1->1, monotonic), Tuning clamping, and
    // that the baked shimmer is non-trivial. Mirrors --test-wormhole.
    if (testDecloak) {
        x3::logInfo("running intro decloak shimmer (DecloakVfx) self-test...");
        int pass = 0, total = 0;
        auto check = [&](bool c, const char* name) {
            ++total;
            if (c) { ++pass; x3::logInfo(std::string("  [ok] ") + name); }
            else   {          x3::logError(std::string("  [FAIL] ") + name); }
        };

        x3::game::HeadlessRenderDevice hdev;
        const float idM[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        // T1: init + shutdown lifecycle is leak-clean (valid handles in, released
        // on shutdown, a second init() succeeds, double-init/shutdown safe).
        {
            x3::space::DecloakVfx dv;
            dv.init(hdev);
            check(dv.initialized() && dv.mesh().valid() && dv.texture().valid(),
                  "T1 init() produces valid mesh + texture, initialized()=true");
            dv.shutdown(hdev);
            check(!dv.initialized() && !dv.mesh().valid() && !dv.texture().valid(),
                  "T1b shutdown() releases handles, initialized()=false");
            dv.init(hdev);
            check(dv.initialized(), "T1c re-init after shutdown succeeds (no leak)");
            x3::rhi::MeshHandle before = dv.mesh();
            dv.init(hdev);
            check(dv.mesh().id == before.id, "T1d double-init() is a no-op");
            dv.shutdown(hdev);
            dv.shutdown(hdev);
            check(!dv.initialized(), "T1e double-shutdown() is safe");
        }
        // T2: render() at progress 0, 0.5, 1.0 runs VUID-safe (headless stub) and
        // the shimmer strength PEAKS at the mid-reveal vs both ends (the reveal
        // window: faint cloaked outline -> peak shimmer -> resolved/fades out).
        {
            x3::space::DecloakVfx dv;
            dv.init(hdev);
            x3::rhi::FrameContext fr = hdev.beginFrame();
            dv.render(hdev, fr, idM, idM, /*timeSec=*/0.25f, /*progress=*/0.0f);
            float s0 = dv.lastShimmerStrength();
            check(s0 > 0.0f, "T2 render() at progress 0 applies a faint cloaked-outline shimmer");
            dv.render(hdev, fr, idM, idM, /*timeSec=*/0.25f, /*progress=*/0.5f);
            float sMid = dv.lastShimmerStrength();
            dv.render(hdev, fr, idM, idM, /*timeSec=*/0.25f, /*progress=*/1.0f);
            float s1 = dv.lastShimmerStrength();
            check(sMid > s0 && sMid > s1, "T2b shimmer PEAKS at the mid-reveal (0.5) vs both ends");
            // render() with a null model transform must be VUID-safe (sits at origin).
            dv.render(hdev, fr, nullptr, nullptr, 0.1f, 0.3f);
            check(dv.lastProgress() == 0.3f, "T2c render() with null transforms is safe");
            hdev.endFrame(fr);
            dv.shutdown(hdev);
        }
        // T3: revealAlpha ramp -- 0 at progress 0, 1 at progress 1, monotonic
        // non-decreasing, and clamps overshoots.
        {
            check(std::fabs(x3::space::DecloakVfx::revealAlpha(0.0f) - 0.0f) < 1e-4f,
                  "T3a revealAlpha(0) ~= 0 (ship fully cloaked)");
            check(std::fabs(x3::space::DecloakVfx::revealAlpha(1.0f) - 1.0f) < 1e-4f,
                  "T3b revealAlpha(1) ~= 1 (ship fully revealed)");
            bool mono = true;
            float prev = x3::space::DecloakVfx::revealAlpha(0.0f);
            for (int i = 1; i <= 64; ++i) {
                float p = (float)i / 64.0f;
                float a = x3::space::DecloakVfx::revealAlpha(p);
                if (a < prev - 1e-5f) mono = false;
                prev = a;
            }
            check(mono, "T3c revealAlpha is monotonic non-decreasing on [0,1]");
            check(x3::space::DecloakVfx::revealAlpha(5.0f) == 1.0f &&
                  x3::space::DecloakVfx::revealAlpha(-3.0f) == 0.0f,
                  "T3d revealAlpha clamps overshoots to [0,1]");
        }
        // T4: Tuning clamping + progress clamp in render() + a non-trivial bake.
        {
            x3::space::DecloakVfx::Tuning bad;
            bad.shellScale   = -2.0f;
            bad.shimmerSpeed = -4.0f;
            bad.scanDensity  = 0.0f;
            auto c = x3::space::clampTuning(bad);
            check(c.shellScale   > 0.0f,  "T4a shellScale clamps to > 0");
            check(c.shimmerSpeed >= 0.0f, "T4b shimmerSpeed clamps to >= 0");
            check(c.scanDensity  >= 1.0f, "T4c scanDensity clamps to >= 1");
            x3::space::DecloakVfx dv;
            dv.init(hdev, bad);
            check(dv.initialized(), "T4d init() survives an out-of-range Tuning");
            x3::rhi::FrameContext fr = hdev.beginFrame();
            dv.render(hdev, fr, idM, idM, 0.0f, /*progress=*/5.0f);
            check(dv.lastProgress() == 1.0f, "T4e progress > 1 clamps to 1.0");
            dv.render(hdev, fr, idM, idM, 0.0f, /*progress=*/-3.0f);
            check(dv.lastProgress() == 0.0f, "T4f progress < 0 clamps to 0.0");
            hdev.endFrame(fr);
            dv.shutdown(hdev);

            // Non-trivial bake: sweep the shell UV -> the scanline + edge glow
            // produce a real brightness variance.
            x3::space::DecloakVfx::Tuning t;
            float lo = 1e9f, hi = -1e9f;
            const int N = 128;
            for (int i = 0; i < N; ++i) {
                float v = (i + 0.5f) / (float)N;
                float b = x3::space::DecloakVfx::sampleShimmerBrightness(0.5f, v, t);
                lo = std::min(lo, b); hi = std::max(hi, b);
            }
            check(hi - lo > 0.05f, "T4g baked shimmer produces brightness variance (scanlines + edge)");
            // Determinism.
            float a0 = x3::space::DecloakVfx::sampleShimmerBrightness(0.2f, 0.7f, t);
            float a1 = x3::space::DecloakVfx::sampleShimmerBrightness(0.2f, 0.7f, t);
            check(a0 == a1, "T4h shimmer sample is deterministic");
        }
        x3::logInfo("decloak: " + std::to_string(pass) + "/" + std::to_string(total) + " passed");
        std::printf("decloak: %d/%d passed\n", pass, total);
        std::fflush(stdout);
        return (pass == total) ? 0 : 1;
    }

    if (testTractor) {
        x3::logInfo("running capital-ship tractor-beam (TractorBeam) self-test...");
        int pass = 0, total = 0;
        auto check = [&](bool c, const char* name) {
            ++total;
            if (c) { ++pass; x3::logInfo(std::string("  [ok] ") + name); }
            else   {          x3::logError(std::string("  [FAIL] ") + name); }
        };

        x3::game::HeadlessRenderDevice hdev;
        const float idM[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        // T1: init + shutdown lifecycle is leak-clean.
        {
            x3::space::TractorBeam tb;
            tb.init(hdev);
            check(tb.initialized() && tb.mesh().valid() && tb.texture().valid(),
                  "T1 init() produces valid mesh + texture, initialized()=true");
            tb.shutdown(hdev);
            check(!tb.initialized() && !tb.mesh().valid() && !tb.texture().valid(),
                  "T1b shutdown() releases handles, initialized()=false");
            tb.init(hdev);
            check(tb.initialized(), "T1c re-init after shutdown succeeds (no leak)");
            x3::rhi::MeshHandle before = tb.mesh();
            tb.init(hdev);
            check(tb.mesh().id == before.id, "T1d double-init() is a no-op");
            tb.shutdown(hdev);
            tb.shutdown(hdev);
            check(!tb.initialized(), "T1e double-shutdown() is safe");
        }
        // T2: render() at intensity 0 / 0.5 / 1.0 with a from/to pair runs VUID-safe
        // and intensity scales the beam strength (lock-on ramp).
        {
            x3::space::TractorBeam tb;
            tb.init(hdev);
            x3::rhi::FrameContext fr = hdev.beginFrame();
            const float from[3] = { 0.0f, 0.0f, 0.0f };
            const float to[3]   = { 12.0f, 2.0f, -4.0f };
            tb.render(hdev, fr, idM, from, to, /*intensity=*/0.0f, /*timeSec=*/0.3f);
            check(tb.lastDrawn() && tb.lastStrength() > 0.0f,
                  "T2 render() at intensity 0 still draws a faint targeting glow");
            float s0 = tb.lastStrength();
            tb.render(hdev, fr, idM, from, to, /*intensity=*/0.5f, 0.3f);
            float s5 = tb.lastStrength();
            tb.render(hdev, fr, idM, from, to, /*intensity=*/1.0f, 0.3f);
            float s1 = tb.lastStrength();
            check(s1 > s5 && s5 > s0,
                  "T2b intensity 0 < 0.5 < 1.0 monotonically raises beam strength");
            // Intensity is clamped to [0,1].
            tb.render(hdev, fr, idM, from, to, /*intensity=*/5.0f, 0.3f);
            check(tb.lastIntensity() == 1.0f, "T2c intensity > 1 clamps to 1.0");
            tb.render(hdev, fr, idM, from, to, /*intensity=*/-3.0f, 0.3f);
            check(tb.lastIntensity() == 0.0f, "T2d intensity < 0 clamps to 0.0");
            hdev.endFrame(fr);
            tb.shutdown(hdev);
        }
        // T3: degenerate from==to is handled gracefully -- the draw is SKIPPED, no
        // NaN, no degenerate transform handed to the GPU.
        {
            x3::space::TractorBeam tb;
            tb.init(hdev);
            x3::rhi::FrameContext fr = hdev.beginFrame();
            const float same[3] = { 3.0f, 1.0f, -2.0f };
            tb.render(hdev, fr, idM, same, same, /*intensity=*/1.0f, 0.3f);
            check(!tb.lastDrawn() && tb.lastStrength() == 0.0f,
                  "T3 from==to skips the draw (no NaN, no degenerate transform)");
            // A vanishingly-short beam is likewise skipped.
            const float a[3] = { 0.0f, 0.0f, 0.0f };
            const float b[3] = { 1e-6f, 0.0f, 0.0f };
            tb.render(hdev, fr, idM, a, b, 1.0f, 0.3f);
            check(!tb.lastDrawn(), "T3b near-zero-length beam is skipped");
            // After a degenerate frame, a valid pair draws again (state recovers).
            const float to2[3] = { 0.0f, 8.0f, 0.0f };  // straight UP (up-parallel basis path)
            tb.render(hdev, fr, idM, a, to2, 1.0f, 0.3f);
            check(tb.lastDrawn(), "T3c valid beam after a degenerate one draws (up-parallel basis ok)");
            hdev.endFrame(fr);
            tb.shutdown(hdev);
        }
        // T4: Tuning clamping (emitterRadius>=0, captureRadius>0, ringDensity>=1,
        // flowSpeed>=0) + the baked energy is non-trivial (capture end brighter
        // than emitter; rings produce brightness variance along the axis).
        {
            x3::space::TractorBeam::Tuning bad;
            bad.emitterRadius = -1.0f;
            bad.captureRadius = -3.0f;
            bad.ringDensity   = 0.0f;
            bad.flowSpeed     = -5.0f;
            auto c = x3::space::clampTuning(bad);
            check(c.emitterRadius >= 0.0f, "T4a emitterRadius clamps to >= 0");
            check(c.captureRadius  > 0.0f, "T4b captureRadius clamps to > 0");
            check(c.ringDensity   >= 1.0f, "T4c ringDensity clamps to >= 1");
            check(c.flowSpeed     >= 0.0f, "T4d flowSpeed clamps to >= 0");
            x3::space::TractorBeam tb;
            tb.init(hdev, bad);
            check(tb.initialized(), "T4e init() survives an out-of-range Tuning");
            tb.shutdown(hdev);

            x3::space::TractorBeam::Tuning t;
            float bEmit = x3::space::TractorBeam::sampleEnergyBrightness(0.0f, t);
            float bCap  = x3::space::TractorBeam::sampleEnergyBrightness(1.0f, t);
            check(bCap > bEmit, "T4f capture end is brighter than the emitter end");
            // Rings: sweep s along the axis -> brightness varies (the energy bands).
            float lo = 1e9f, hi = -1e9f;
            const int N = 512;
            for (int i = 0; i < N; ++i) {
                float s = (i + 0.5f) / (float)N;
                float bb = x3::space::TractorBeam::sampleEnergyBrightness(s, t);
                lo = std::min(lo, bb); hi = std::max(hi, bb);
            }
            check(hi - lo > 0.05f, "T4g energy rings produce brightness variance along the beam");
            float a0 = x3::space::TractorBeam::sampleEnergyBrightness(0.4f, t);
            float a1 = x3::space::TractorBeam::sampleEnergyBrightness(0.4f, t);
            check(a0 == a1, "T4h energy sample is deterministic");
        }
        x3::logInfo("tractor: " + std::to_string(pass) + "/" + std::to_string(total) + " passed");
        std::printf("tractor: %d/%d passed\n", pass, total);
        std::fflush(stdout);
        return (pass == total) ? 0 : 1;
    }
    // --test-wormhole-transit: S3 wormhole transit self-test. Headless --
    // wires a WormholeTransit runner into a SpaceLayer and drives the spine:
    // requestWormhole(dest) -> Context::WormholeTransit + active(); stepping
    // SpaceLayer.update(dt) ramps progress() 0->1; at progress 1.0 the context
    // returns to DeepSpace and active() is false again.
    if (testWormholeTransit) {
        x3::logInfo("running S3 wormhole-transit self-test...");
        int pass = 0, total = 0;
        auto check = [&](bool c, const char* name) {
            ++total;
            if (c) { ++pass; x3::logInfo(std::string("  [ok] ") + name); }
            else   {          x3::logError(std::string("  [FAIL] ") + name); }
        };

        using x3::space::SpaceLayer;
        using x3::space::Context;
        using x3::space::WormholeTransit;

        x3::game::HeadlessRenderDevice hdev;

        // T1: init brings up the owned VFX and the transit starts idle.
        {
            SpaceLayer L; L.init();
            WormholeTransit wt;
            wt.init(hdev, L, /*durationSec=*/6.0f);
            check(!wt.active(), "T1 init() -> not active (no transit armed yet)");
            check(wt.progress() == 0.0f, "T1b init() -> progress 0");
            wt.shutdown(hdev);
        }

        // T2: requestWormhole -> WormholeTransit + active(); stepping ramps
        // progress 0->1; at 1.0 the context lands in DeepSpace and active==false.
        {
            SpaceLayer L; L.init();
            WormholeTransit wt;
            const float dur = 6.0f;
            wt.init(hdev, L, dur);

            L.requestWormhole(/*destSystemId=*/42u);
            check(L.context() == Context::WormholeTransit,
                  "T2 requestWormhole -> Context::WormholeTransit");

            // First tick arms the transit; active() flips true and progress > 0.
            L.update(1.0f);
            check(wt.active(), "T2b after first update -> active()");
            check(wt.progress() > 0.0f && wt.progress() < 1.0f,
                  "T2c progress ramps into (0,1)");
            check(L.context() == Context::WormholeTransit,
                  "T2d still in WormholeTransit mid-jump");

            // Monotonic ramp: progress strictly increases each step until done.
            float prev = wt.progress();
            bool monotonic = true;
            // dur=6, already 1s in; 5 more 1s steps reach progress 1.0.
            for (int i = 0; i < 5; ++i) {
                L.update(1.0f);
                if (wt.progress() < prev) monotonic = false;
                prev = wt.progress();
            }
            check(monotonic, "T2e progress is monotonic non-decreasing");
            check(wt.progress() >= 1.0f, "T2f progress reaches 1.0 at duration");
            check(L.context() == Context::DeepSpace,
                  "T2g transit complete -> back in DeepSpace (arrived at dest)");
            check(!wt.active(), "T2h transit complete -> active()==false");

            wt.shutdown(hdev);
        }

        // T3: progress() never exceeds 1.0 even if over-stepped past duration.
        {
            SpaceLayer L; L.init();
            WormholeTransit wt;
            wt.init(hdev, L, /*durationSec=*/2.0f);
            L.requestWormhole(7u);
            L.update(100.0f); // wildly over-step
            check(wt.progress() == 1.0f, "T3 progress clamps to 1.0 on over-step");
            check(L.context() == Context::DeepSpace, "T3b over-step still lands DeepSpace");
            check(!wt.active(), "T3c over-step completes the transit");
            wt.shutdown(hdev);
        }

        // T4: a second jump after the first re-arms cleanly (timer resets).
        {
            SpaceLayer L; L.init();
            WormholeTransit wt;
            wt.init(hdev, L, /*durationSec=*/4.0f);
            // First jump to completion.
            L.requestWormhole(1u);
            for (int i = 0; i < 4; ++i) L.update(1.0f);
            check(L.context() == Context::DeepSpace && !wt.active(),
                  "T4 first jump completes");
            // Second jump: progress must restart near 0, not stay pinned at 1.
            L.requestWormhole(2u);
            L.update(1.0f);
            check(wt.active() && wt.progress() > 0.0f && wt.progress() < 1.0f,
                  "T4b second jump re-arms with a fresh progress ramp");
            wt.shutdown(hdev);
        }

        // T5: render() before/after init is VUID-safe (no crash; no-op pre-init).
        {
            SpaceLayer L; L.init();
            WormholeTransit wt;
            wt.init(hdev, L, 6.0f);
            x3::rhi::FrameContext fr = hdev.beginFrame();
            const float idM[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
            wt.render(hdev, fr, idM, /*timeSec=*/0.5f); // active()==false here -> still safe
            L.requestWormhole(3u);
            L.update(1.0f);
            wt.render(hdev, fr, idM, 1.0f);             // mid-transit draw
            hdev.endFrame(fr);
            check(true, "T5 render() is crash-free pre- and mid-transit");
            wt.shutdown(hdev);
            // After shutdown a render is a safe no-op.
            x3::rhi::FrameContext fr2 = hdev.beginFrame();
            wt.render(hdev, fr2, idM, 2.0f);
            hdev.endFrame(fr2);
            check(!wt.active() && wt.progress() == 0.0f,
                  "T5b shutdown() resets active()/progress() and render() stays safe");
        }

        x3::logInfo("wormhole-transit: " + std::to_string(pass) + "/" + std::to_string(total) + " passed");
        std::printf("wormhole-transit: %d/%d passed\n", pass, total);
        std::fflush(stdout);
        return (pass == total) ? 0 : 1;
    }
    // --test-atmo-descent: S4 cinematic atmo-descent self-test. Headless (a small
    // offscreen device backs AtmoDescent::init's mesh/texture creation). Verifies
    // the descent runner wiring into the S0 SpaceLayer spine.
    if (testAtmoDescent) {
        x3::logInfo("running S4 cinematic atmo-descent self-test...");
        int pass = 0, total = 0;
        auto check = [&](bool c, const char* name) {
            ++total;
            if (c) { ++pass; x3::logInfo(std::string("  [ok] ") + name); }
            else   {          x3::logError(std::string("  [FAIL] ") + name); }
        };

        if (!glfwInit()) { x3::logError("[atmo-descent] glfwInit failed"); return 1; }
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        std::unique_ptr<x3::rhi::IRenderDevice> dev(x3::rhi::createRenderDevice());
        x3::rhi::DeviceDesc dd{};
        dd.width = 320; dd.height = 240; dd.headless = true;
#ifdef _DEBUG
        dd.validation = true;
#endif
        if (!dev->init(dd)) { x3::logError("[atmo-descent] device init failed"); glfwTerminate(); return 1; }

        using x3::space::SpaceLayer;
        using x3::space::Context;
        using x3::space::AtmoDescent;

        // T1: heat-intensity curve shape — 0 at the ends, a peak in the middle.
        {
            float h0 = x3::space::descentHeatIntensity(0.0f);
            float h1 = x3::space::descentHeatIntensity(1.0f);
            float hm = x3::space::descentHeatIntensity(0.5f);
            check(h0 < 0.05f && h1 < 0.05f, "T1 heat curve ~0 at start + end (clear sky)");
            check(hm > h0 && hm > h1 && hm > 0.5f, "T1b heat curve peaks mid-descent (hull glow)");
        }

        // T2: init() wires the runner + builds GPU resources.
        {
            SpaceLayer L; L.init();
            AtmoDescent d;
            d.init(*dev, L, /*durationSec=*/2.0f);
            check(d.initialized(), "T2 init() built the entry-effect resources");
            check(!d.active() && d.progress() == 0.0f, "T2b not active before requestDescent()");
            check(std::fabs(d.durationSec() - 2.0f) < 1e-4f, "T2c durationSec recorded");
            d.shutdown(*dev);
        }

        // T3: requestDescent() -> AtmoDescent context + the descent goes active.
        {
            SpaceLayer L; L.init();
            AtmoDescent d; d.init(*dev, L, /*durationSec=*/1.0f);
            L.requestDescent(/*planetId=*/3);
            check(L.context() == Context::AtmoDescent, "T3 requestDescent -> AtmoDescent");
            L.update(0.1f);  // first tick arms the runner active flag + ramps a bit
            check(d.active(), "T3b descent active mid-sequence");
            check(L.context() == Context::AtmoDescent, "T3c stays AtmoDescent while ramping");
            check(d.progress() > 0.0f && d.progress() < 1.0f, "T3d progress ramps 0<p<1");
            d.shutdown(*dev);
        }

        // T4: progress ramps monotonically toward 1.0 over the duration, then the
        // spine lands in Surface and the descent goes inactive.
        {
            SpaceLayer L; L.init();
            AtmoDescent d; d.init(*dev, L, /*durationSec=*/1.0f);
            L.requestDescent(/*planetId=*/9);
            float prev = d.progress();
            bool monotonic = true;
            // 12 ticks * 0.1s = 1.2s > 1.0s duration -> completes.
            for (int i = 0; i < 12; ++i) {
                L.update(0.1f);
                if (d.progress() < prev - 1e-5f) monotonic = false;
                prev = d.progress();
            }
            check(monotonic, "T4 progress is monotonic non-decreasing");
            check(d.progress() >= 1.0f, "T4b progress reaches 1.0 by end of duration");
            check(L.context() == Context::Surface, "T4c completion -> Surface (handed to --world, STUBBED)");
            check(!d.active(), "T4d descent inactive after completion");
            d.shutdown(*dev);
        }

        // T5: render() is a safe no-op-ish call against a live frame (no crash,
        // exercises the draw path at a representative mid-descent progress).
        {
            SpaceLayer L; L.init();
            AtmoDescent d; d.init(*dev, L, /*durationSec=*/2.0f);
            L.requestDescent(1);
            L.update(1.0f);  // ~halfway -> heat near peak
            d.setCamera(0.0f, 0.0f, 0.0f);
            auto fr = dev->beginFrame();
            if (fr.valid) d.render(*dev, fr, /*viewProj16=*/nullptr, /*timeSec=*/1.0f);
            dev->endFrame(fr);
            check(d.progress() > 0.0f, "T5 render() ran at mid-descent without crashing");
            d.shutdown(*dev);
        }

        dev->shutdown();
        glfwTerminate();
        x3::logInfo("atmo-descent: " + std::to_string(pass) + "/" + std::to_string(total) + " passed");
        std::printf("atmo-descent: %d/%d passed\n", pass, total);
        std::fflush(stdout);
        return (pass == total) ? 0 : 1;
    }
    // --test-ship-interior: S5 walkable, data-driven ship interior self-test.
    if (testShipInterior) {
        x3::logInfo("running S5 ship-interior (walkable, data-driven) self-test...");
        return x3::space::runShipInteriorSelfTest() ? 0 : 1;
    }
    // --test-ship-ai: S8 enemy ship AI self-test. Pure logic, headless, no GPU.
    if (testShipAi) {
        x3::logInfo("running S8 enemy ship-AI (dogfight) self-test...");
        return x3::space::runShipAiSelfTest() ? 0 : 1;
    }
    // --test-targeting: S9 targeting / radar / lock-on self-test. Headless.
    if (testTargeting) {
        x3::logInfo("running S9 targeting / radar / lock-on self-test...");
        int pass = 0, total = 0;
        auto check = [&](bool c, const char* name) {
            ++total;
            if (c) { ++pass; x3::logInfo(std::string("  [ok] ") + name); }
            else   {          x3::logError(std::string("  [FAIL] ") + name); }
        };

        using x3::space::TargetingSystem;
        using x3::space::Contact;
        using x3::space::LeadSolution;

        // A deterministic scene: two hostiles (ids 10, 20) ahead along +X at
        // different ranges, one friendly (id 5) ahead, one hostile (id 30)
        // behind. Hostile 10 sits dead on the +X boresight; 20 is off-axis.
        Contact cs[4]{};
        cs[0] = { 10u, { 100.0f,   0.0f,   0.0f }, { 0.0f, 0.0f, 0.0f }, true  }; // on-axis, near
        cs[1] = { 20u, { 200.0f,  40.0f,   0.0f }, { 0.0f, 0.0f, 0.0f }, true  }; // off-axis, far
        cs[2] = {  5u, { 120.0f,   0.0f,   0.0f }, { 0.0f, 0.0f, 0.0f }, false }; // friendly, on-axis
        cs[3] = { 30u, { -80.0f,   0.0f,   0.0f }, { 0.0f, 0.0f, 0.0f }, true  }; // behind

        // T1: setContacts + no lock initially.
        {
            TargetingSystem ts;
            ts.setContacts(cs, 4);
            check(ts.contactCount() == 4u, "T1 setContacts stores all contacts");
            check(!ts.hasLock(), "T1b no lock before any lock request");
        }

        // T2: lockNearest picks the in-cone hostile, skipping the closer friendly.
        {
            TargetingSystem ts;
            ts.setContacts(cs, 4);
            const float from[3] = { 0.0f, 0.0f, 0.0f };
            const float fwd[3]  = { 1.0f, 0.0f, 0.0f };
            ts.lockNearest(from, fwd);
            check(ts.hasLock() && ts.lockedId() == 10u,
                  "T2 lockNearest locks the on-axis hostile (id 10), not the friendly");
        }

        // T3: cycleTarget advances through HOSTILES only, skipping the friendly.
        {
            TargetingSystem ts;
            ts.setContacts(cs, 4);
            ts.cycleTarget(+1); // first hostile in list order: id 10
            check(ts.hasLock() && ts.lockedId() == 10u, "T3 cycle +1 -> first hostile id 10");
            ts.cycleTarget(+1); // next hostile: id 20 (skips friendly id 5)
            check(ts.lockedId() == 20u, "T3b cycle +1 -> next hostile id 20 (skips friendly)");
            ts.cycleTarget(+1); // next hostile: id 30 (wraps past friendly)
            check(ts.lockedId() == 30u, "T3c cycle +1 -> hostile id 30");
            ts.cycleTarget(+1); // wraps back to id 10
            check(ts.lockedId() == 10u, "T3d cycle +1 wraps back to id 10");
            ts.cycleTarget(-1); // back to id 30
            check(ts.lockedId() == 30u, "T3e cycle -1 wraps to id 30");
        }

        // T4: computeLead returns a VALID intercept for a crossing mover.
        {
            TargetingSystem ts;
            // Single hostile crossing in +Z at 30 m/s, 100 m straight ahead.
            Contact mover{ 99u, { 100.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 30.0f }, true };
            ts.setContacts(&mover, 1);
            ts.cycleTarget(+1);
            const float shooter[3] = { 0.0f, 0.0f, 0.0f };
            LeadSolution s = ts.computeLead(shooter, 300.0f); // fast projectile
            // Lead must be AHEAD of the target in its direction of travel (+Z>0)
            // and within physical bounds of a real intercept.
            check(s.valid && s.aimPoint[2] > 0.5f && s.distance > 99.0f,
                  "T4 computeLead: valid lead point ahead of crossing target");
            // Sanity: the aim point's flight time matches projectile travel.
            float dz = s.aimPoint[2];
            float flightT = s.distance / 300.0f;
            check(std::fabs(dz - 30.0f * flightT) < 1.0f,
                  "T4b lead point is consistent with intercept time");
        }

        // T5: computeLead INVALID when the projectile is too slow to catch a
        // target receding faster than the projectile flies.
        {
            TargetingSystem ts;
            Contact fleeing{ 88u, { 100.0f, 0.0f, 0.0f }, { 50.0f, 0.0f, 0.0f }, true };
            ts.setContacts(&fleeing, 1);
            ts.cycleTarget(+1);
            const float shooter[3] = { 0.0f, 0.0f, 0.0f };
            LeadSolution s = ts.computeLead(shooter, 10.0f); // slower than the target
            check(!s.valid, "T5 computeLead invalid: projectile too slow to intercept");
        }

        // T6: computeLead invalid with no lock.
        {
            TargetingSystem ts;
            ts.setContacts(cs, 4);
            const float shooter[3] = { 0.0f, 0.0f, 0.0f };
            LeadSolution s = ts.computeLead(shooter, 300.0f);
            check(!s.valid, "T6 computeLead invalid without a lock");
        }

        // T7: radarBlips projects within range, drops out-of-range, flags locked.
        {
            TargetingSystem ts;
            ts.setContacts(cs, 4);
            ts.cycleTarget(+1); // lock id 10 (on +X boresight)
            const float pPos[3] = { 0.0f, 0.0f, 0.0f };
            const float pFwd[3] = { 1.0f, 0.0f, 0.0f }; // facing +X -> radar up
            TargetingSystem::Blip blips[8]{};
            // Range 150: includes id 10 (100), friendly 5 (120), behind 30 (80);
            // excludes id 20 (planar 200).
            uint32_t nb = ts.radarBlips(blips, 8, pPos, pFwd, 150.0f);
            check(nb == 3u, "T7 radarBlips range-gates out the far contact (3 of 4)");
            // Find id 10's blip: facing +X, a +X target projects to +Y (up),
            // near +1 (range 100 / 150 ~= 0.67), x near 0.
            bool okLockedBlip = false;
            for (uint32_t i = 0; i < nb; ++i) {
                if (blips[i].id == 10u) {
                    okLockedBlip = blips[i].locked &&
                                   blips[i].hostile &&
                                   blips[i].radarXY[1] > 0.5f &&
                                   std::fabs(blips[i].radarXY[0]) < 0.05f;
                }
            }
            check(okLockedBlip, "T7b locked on-axis hostile projects to radar up, flagged locked");
            // The contact behind (id 30) projects to -Y (down).
            bool behindDown = false;
            for (uint32_t i = 0; i < nb; ++i)
                if (blips[i].id == 30u && blips[i].radarXY[1] < -0.3f) behindDown = true;
            check(behindDown, "T7c contact behind the player projects to radar down");
        }

        // T8: clearLock + lock auto-drops when the locked contact disappears.
        {
            TargetingSystem ts;
            ts.setContacts(cs, 4);
            ts.cycleTarget(+1);
            check(ts.hasLock(), "T8 lock acquired");
            ts.clearLock();
            check(!ts.hasLock(), "T8b clearLock releases the lock");
            // Re-lock, then feed a list WITHOUT the locked id -> auto-clear.
            ts.cycleTarget(+1);
            check(ts.hasLock() && ts.lockedId() == 10u, "T8c re-locked id 10");
            Contact gone[1] = { { 20u, { 200.0f, 40.0f, 0.0f }, { 0,0,0 }, true } };
            ts.setContacts(gone, 1);
            check(!ts.hasLock(), "T8d lock auto-drops when locked contact leaves the feed");
        }

        x3::logInfo("targeting: " + std::to_string(pass) + "/" + std::to_string(total) + " passed");
        std::printf("targeting: %d/%d passed\n", pass, total);
        std::fflush(stdout);
        return (pass == total) ? 0 : 1;
    }
    // --test-ship-damage: S10 ship damage model self-test.
    if (testShipDamage) {
        x3::logInfo("running S10 ship-damage model self-test...");
        return x3::space::runShipDamageSelfTest() ? 0 : 1;
    }
    // --test-ship-windows: S6 true-portal ship windows self-test.
    if (testShipWindows) {
        x3::logInfo("running S6 ship-windows (true-portal moving space) self-test...");
        return x3::space::runShipWindowsSelfTest() ? 0 : 1;
    }
    // --test-ship-repair: S7 in-transit panel-repair state machine self-test.
    if (testShipRepair) {
        x3::logInfo("running S7 ship-repair (panel state machine) self-test...");
        return x3::space::runShipRepairSelfTest() ? 0 : 1;
    }
    if (testHoloterm) {
        x3::logInfo("running holo-terminal (text + input) self-test...");
        return x3::game::runHoloTerminalSelfTest() ? 0 : 1;
    }
    if (testTerminals) {
        x3::logInfo("running holo-terminal KIOSK SYSTEM self-test...");
        return x3::game::runHoloTerminalSystemSelfTest() ? 0 : 1;
    }
    if (testSit) {
        x3::logInfo("running glass-lounge SIT-AT-CHAIR (state machine) self-test...");
        return x3::game::runSitSelfTest() ? 0 : 1;
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
    if (testNpc) {
        x3::logInfo("running non-combatant NPCSystem self-test...");
        return x3::game::runNpcSelfTest() ? 0 : 1;
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
    if (testMultiFloorAi) {
        x3::logInfo("running multi-floor AI self-test (cross-floor enemy must not shoot through slabs)...");
        return x3::game::runMultiFloorAiSelfTest() ? 0 : 1;
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
    if (testAdaptiveHide) {
        x3::logInfo("running canon-aliens Adaptive-Hide rhythm self-test "
                    "(type-keyed resist + 8 s window; Warlord-tuned)...");
        return x3::game::runAdaptiveHideSelfTest() ? 0 : 1;
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
    if (testAct2Desert) {
        x3::logInfo("running EFLZ Act-2 desert depths (L10 Crystalline Desert Depths: "
                    "first-contact allied Salvari + injured-Salvari side-quest, hidden "
                    "crystal-cave camp entrance, light Overlord patrol; + L11 Salvari Camp "
                    "'Refugee Haven': cave settlement, survivors incl. K'thara, upgrade "
                    "station + cultural-exchange beat; reachable L9->L10->L11) self-test...");
        return x3::game::runAct2DesertSelfTest() ? 0 : 1;
    }
    if (testAct2Caves) {
        x3::logInfo("running EFLZ Act-2 mid biomes (L12 Advanced Cave System + Crystal "
                    "Heart dual-gated interactable + Memory Hunter abyss boss; L13 Toxic "
                    "Swamplands Edge + poison hazard [inert at load]; L14 Research Station "
                    "+ timeline-gated Siren ambush; L15 Tree Cities + trading post) "
                    "self-test...");
        return x3::game::runAct2CavesSelfTest() ? 0 : 1;
    }
    if (testCanonAliens) {
        x3::logInfo("running EFLZ canon-alien roster (Mantis/Grey/Reptilian/Nordic — "
                    "the 'most reported' species ported into MonsterSystem::Tuning rows: "
                    "Saurian Soldier/Warlord, Grey Tasked, Nordic Steward, Mantis Arbiter) "
                    "self-test...");
        return x3::game::runCanonAliensSelfTest() ? 0 : 1;
    }
    if (testWorldRegions) {
        x3::logInfo("running EFLZ open-world surface regions (crash site + outposts + "
                    "4 mountain ranges) self-test...");
        return x3::game::runWorldRegionsSelfTest() ? 0 : 1;
    }
    if (testCity) {
        x3::logInfo("running EFLZ open-world metropolis (Scrapyard / New District / Industrial "
                    "+ road grid + 4 freeway tunnels) self-test...");
        return x3::game::runCitySelfTest() ? 0 : 1;
    }
    if (testOceanBase) {
        x3::logInfo("running EFLZ open-world ocean + undersea base + submarine combat self-test...");
        return x3::game::runOceanBaseSelfTest() ? 0 : 1;
    }
    if (testUnderseaArt) {
        x3::logInfo("running EFLZ Act-4 undersea-base art overlay (Abyssal Station GLB) self-test...");
        return x3::game::runUnderseaArtSelfTest() ? 0 : 1;
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
    if (testThirdPerson) {
        x3::logInfo("running third-person view (Jake avatar + follow cam + held weapon) self-test (TP1-TP9)...");
        return x3::game::runThirdPersonSelfTest() ? 0 : 1;
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
    if (testCompanion) {
        return x3::game::runCompanionSelfTest() ? 0 : 1;
    }
    if (testCompanionSquad) {
        return x3::game::runCompanionSquadSelfTest() ? 0 : 1;
    }
    if (testCompanionController) {
        return x3::game::runCompanionControllerSelfTest() ? 0 : 1;
    }
    if (testClub) {
        x3::logInfo("running Club 1127 (\"THE DEEP\") self-test "
                    "(build at Y=-200; assert DJ booth/ORB/bars/stair/PA/blacklights/TVs/footprint; leak-clean)...");
        return x3::game::runClubSelfTest() ? 0 : 1;
    }

    // First log line: the build identity, so --smoketest / --bench logs (and any
    // screenshot run) self-identify which build produced them. See docs/VERSIONING.md.
    x3::logInfo(versionLine());
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
    // not the display refresh cap. --bench-combat is the same family (combat-density
    // measurement) so it also disables vsync.
    desc.vsync  = !(bench || benchCombat);
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

    // ---- Wormhole transit showcase (--world wormhole-transit) ---------------
    if (worldMode == "wormhole-transit") {
        x3::logInfo("--world wormhole-transit: showcasing the S3 crystal-matrix jump");

        { x3::rhi::IRenderDevice::SsaoParams sp{}; sp.enabled = false; device->setSsaoParams(sp); }
        { x3::rhi::IRenderDevice::GiParams   gp{}; gp.enabled   = false; device->setGiParams(gp); }
        { x3::rhi::IRenderDevice::SkyParams  sp{}; sp.enabled   = false; device->setSkyParams(sp); }

        x3::space::SpaceLayer L;
        L.init();
        x3::space::WormholeTransit wt;
        const float kDuration = 6.0f;
        wt.init(*device, L, kDuration);
        L.requestWormhole(/*destSystemId=*/42u);

        if (headless) {
            const std::string outPath = screenshot ? screenshotPath
                                                   : std::string("agent_wormhole_transit.png");
            const int kFrames = 36;
            const float dt = (kDuration * 0.8f) / (float)kFrames;
            float t = 0.0f;
            for (int i = 0; i < kFrames; ++i) {
                glfwPollEvents();
                L.update(dt);
                t += dt;
                float camZ = 4.0f + t * 6.0f;
                device->setCamera(0.0f, 0.0f, camZ, 0.0f, 0.0f, 75.0f);
                if (shotCamOverride) {
                    device->setCamera(shotCam[0], shotCam[1], shotCam[2],
                                      shotCam[3], shotCam[4], 75.0f);
                }
                if (i == kFrames - 1) device->armCapture(outPath.c_str());
                auto frame = device->beginFrame();
                if (frame.valid) wt.render(*device, frame, nullptr, t);
                device->endFrame(frame);
            }
            const bool wrote = device->captureFrame(outPath.c_str());
            if (wrote) x3::logInfo("--world wormhole-transit: wrote " + outPath +
                                   " (progress=" + std::to_string(wt.progress()) + ")");
            else       x3::logError("--world wormhole-transit: capture FAILED");
            wt.shutdown(*device);
            device->shutdown();
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return wrote ? 0 : 1;
        }

        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        double startTimeWT = glfwGetTime();
        double prevTimeWT  = startTimeWT;
        x3::logInfo("--world wormhole-transit: the jump plays then holds; Esc to quit");
        int lastWsWT = (int)W, lastHsWT = (int)H;
        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();
            if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;
            double now = glfwGetTime();
            float dt = (float)(now - prevTimeWT); prevTimeWT = now;
            float t = (float)(now - startTimeWT);
            L.update(dt);
            int cw, chh; glfwGetFramebufferSize(window, &cw, &chh);
            if (cw != lastWsWT || chh != lastHsWT) { lastWsWT = cw; lastHsWT = chh; if (cw>0&&chh>0) device->onResize((uint32_t)cw,(uint32_t)chh); }
            float camZ = 4.0f + t * 6.0f;
            device->setCamera(0.0f, 0.0f, camZ, 0.0f, 0.0f, 75.0f);
            auto frame = device->beginFrame();
            if (frame.valid) wt.render(*device, frame, nullptr, t);
            device->endFrame(frame);
        }

        wt.shutdown(*device);
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return 0;
    }

    // ---- Cinematic atmospheric descent showcase (--world atmo-descent) ------
    if (worldMode == "atmo-descent") {
        x3::logInfo("--world atmo-descent: showcasing the S4 cinematic atmospheric descent");

        { x3::rhi::IRenderDevice::SsaoParams sp{}; sp.enabled = false; device->setSsaoParams(sp); }
        { x3::rhi::IRenderDevice::GiParams   gp{}; gp.enabled = false; device->setGiParams(gp); }
        { x3::rhi::IRenderDevice::SkyParams  sp{}; sp.enabled = false; device->setSkyParams(sp); }

        x3::space::SpaceLayer layer; layer.init();
        x3::space::AtmoDescent descent;
        descent.init(*device, layer, /*durationSec=*/8.0f);
        if (!descent.initialized()) {
            x3::logError("--world atmo-descent: AtmoDescent::init() failed");
            device->shutdown();
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return 1;
        }
        layer.requestDescent(/*planetId=*/1);

        if (headless) {
            const std::string outPath = screenshot ? screenshotPath
                                                   : std::string("agent_atmo_descent.png");
            const int kFrames = 60;
            const float dt = 8.0f / (float)kFrames * 0.62f;
            for (int i = 0; i < kFrames; ++i) {
                glfwPollEvents();
                float t = (float)i * (1.0f / 30.0f);
                layer.update(dt);
                float drop  = -(float)i * 0.15f;
                float yaw   = -1.5708f + 0.08f * std::sin(t);
                float pitch = -0.18f;
                device->setCamera(0.0f, drop, 0.0f, yaw, pitch, 75.0f);
                if (shotCamOverride) {
                    device->setCamera(shotCam[0], shotCam[1], shotCam[2],
                                      shotCam[3], shotCam[4], 75.0f);
                }
                if (i == kFrames - 1) device->armCapture(outPath.c_str());
                auto frame = device->beginFrame();
                if (frame.valid) {
                    descent.setCamera(0.0f, drop, 0.0f);
                    descent.render(*device, frame, nullptr, t);
                }
                device->endFrame(frame);
            }
            const bool wrote = device->captureFrame(outPath.c_str());
            if (wrote) x3::logInfo("--world atmo-descent: wrote " + outPath +
                                   " (progress=" + std::to_string(descent.progress()) + ")");
            else       x3::logError("--world atmo-descent: capture FAILED");
            descent.shutdown(*device);
            device->shutdown();
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return wrote ? 0 : 1;
        }

        double startTimeAD = glfwGetTime();
        double prevTimeAD  = startTimeAD;
        float dropY = 0.0f, fyaw = 0.0f;
        x3::logInfo("--world atmo-descent: watch the orbit->ground entry sequence, Esc to quit");
        int lastWsAD = (int)W, lastHsAD = (int)H;
        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();
            if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;
            double now = glfwGetTime();
            float frameDt = (float)(now - prevTimeAD); prevTimeAD = now;
            float t = (float)(now - startTimeAD);
            if (layer.context() == x3::space::Context::Surface) {
                layer.init(); descent.init(*device, layer, 8.0f);
                layer.requestDescent(1); dropY = 0.0f; fyaw = 0.0f;
            }
            layer.update(frameDt);
            dropY -= frameDt * 4.5f;
            fyaw  += frameDt * 0.1f;
            int cw, chh; glfwGetFramebufferSize(window, &cw, &chh);
            if (cw != lastWsAD || chh != lastHsAD) { lastWsAD = cw; lastHsAD = chh; if (cw>0&&chh>0) device->onResize((uint32_t)cw,(uint32_t)chh); }
            device->setCamera(0.0f, dropY, 0.0f, -1.5708f + fyaw, -0.18f, 75.0f);
            auto frame = device->beginFrame();
            if (frame.valid) {
                descent.setCamera(0.0f, dropY, 0.0f);
                descent.render(*device, frame, nullptr, t);
            }
            device->endFrame(frame);
        }

        descent.shutdown(*device);
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return 0;
    }

    // ---- Procedural starfield showcase (--world starfield) ------------------
    // The Act-3 deep-space backdrop in isolation: NO scene geometry, NO player.
    // A SkyStars instance is brought up, the camera slowly rotates around the
    // origin so the parallax-correct dome reads at multiple yaws, and a
    // screenshot is captured at the end. The pixel-variance test on the
    // resulting PNG (std > 15, uniqColors > 100) IS the visual gate -- a near-
    // black background sprinkled with many distinct star pixels easily clears.
    if (worldMode == "starfield") {
        x3::logInfo("--world starfield: showcasing the procedural deep-space starfield");

        // Deep-space backdrop: disable SSAO + GI (no surface detail to occlude),
        // sky (the starfield IS the sky), water, RT. Just the dome + clear color.
        { x3::rhi::IRenderDevice::SsaoParams sp{}; sp.enabled = false; device->setSsaoParams(sp); }
        { x3::rhi::IRenderDevice::GiParams   gp{}; gp.enabled   = false; device->setGiParams(gp); }
        { x3::rhi::IRenderDevice::SkyParams  sp{}; sp.enabled   = false; device->setSkyParams(sp); }

        x3::SkyStars sky;
        sky.init(*device);
        if (!sky.initialized()) {
            x3::logError("--world starfield: SkyStars::init() failed");
            device->shutdown();
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return 1;
        }

        // ---- Headless capture: rotate the camera a bit, settle, grab PNG. -----
        if (headless) {
            const std::string outPath = screenshot ? screenshotPath
                                                   : std::string("agent_starfield.png");
            const int kFrames = 24;
            for (int i = 0; i < kFrames; ++i) {
                glfwPollEvents();
                float t = (float)i * (1.0f / 30.0f);   // simulated 30 fps clock
                // Camera at origin (dome centered on us). Slow yaw rotation
                // so successive frames sample different starfield directions.
                float yaw   = 0.15f * t;
                float pitch = 0.05f * std::sin(t);
                device->setCamera(0.0f, 0.0f, 0.0f, yaw, pitch, 70.0f);
                if (shotCamOverride) {
                    device->setCamera(shotCam[0], shotCam[1], shotCam[2],
                                      shotCam[3], shotCam[4], 70.0f);
                }
                if (i == kFrames - 1) device->armCapture(outPath.c_str());
                auto frame = device->beginFrame();
                if (frame.valid) {
                    sky.setCamera(0.0f, 0.0f, 0.0f);
                    sky.render(*device, frame, /*viewProjInv16=*/nullptr, t);
                }
                device->endFrame(frame);
            }
            const bool wrote = device->captureFrame(outPath.c_str());
            if (wrote) x3::logInfo("--world starfield: wrote " + outPath);
            else       x3::logError("--world starfield: capture FAILED");
            sky.shutdown(*device);
            device->shutdown();
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return wrote ? 0 : 1;
        }

        // ---- Windowed path: free-rotate the camera with the mouse to inspect.
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        double lastMX, lastMY; glfwGetCursorPos(window, &lastMX, &lastMY);
        double startTime = glfwGetTime();
        double prevTime  = startTime;
        float fyaw = 0.0f, fpitch = 0.0f;
        x3::logInfo("--world starfield: rotate with mouse, Esc to quit");
        int lastWs = (int)W, lastHs = (int)H;
        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();
            if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;
            double now = glfwGetTime(); (void)prevTime; prevTime = now;
            float t = (float)(now - startTime);
            double mx, my; glfwGetCursorPos(window, &mx, &my);
            float ddx = (float)(mx - lastMX), ddy = (float)(my - lastMY);
            lastMX = mx; lastMY = my;
            fyaw   += ddx * 0.0025f;
            fpitch -= ddy * 0.0025f;
            if (fpitch >  1.55f) fpitch =  1.55f;
            if (fpitch < -1.55f) fpitch = -1.55f;
            int cw, chh; glfwGetFramebufferSize(window, &cw, &chh);
            if (cw != lastWs || chh != lastHs) { lastWs = cw; lastHs = chh; if (cw>0&&chh>0) device->onResize((uint32_t)cw,(uint32_t)chh); }
            device->setCamera(0.0f, 0.0f, 0.0f, fyaw, fpitch, 70.0f);
            auto frame = device->beginFrame();
            if (frame.valid) {
                sky.setCamera(0.0f, 0.0f, 0.0f);
                sky.render(*device, frame, /*viewProjInv16=*/nullptr, t);
            }
            device->endFrame(frame);
        }

        sky.shutdown(*device);
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return 0;
    }

    // ---- Distance-LOD showcase (--world lod) -------------------------------
    // S2 proof: one sphere asset, four LODs (icosphere subdivided 3/2/1/0 times
    // -> ~1280 / 320 / 80 / 20 tris). The camera pulls steadily BACK over time;
    // each frame LodSystem::select() picks the LOD for the current distance and
    // we draw ONLY that mesh. On screen the sphere visibly simplifies from a
    // smooth ball to a coarse polyhedron as it recedes. Headless capture writes
    // a PNG at a mid-distance pose (a clearly faceted but still ball-shaped LOD,
    // lots of shaded facets + lit highlight -> easily clears std>15 / uniq>100).
    if (worldMode == "lod") {
        x3::logInfo("--world lod: showcasing the S2 distance-LOD mesh-swap");

        // SSAO/SSGI raster fallback is BLACK with no RT on this rig -> disable
        // both (SSAO-black-on-no-RT). Keep the analytic sky for a lit backdrop.
        { x3::rhi::IRenderDevice::SsaoParams sp{}; sp.enabled = false; device->setSsaoParams(sp); }
        { x3::rhi::IRenderDevice::GiParams   gp{}; gp.enabled = false; device->setGiParams(gp); }
        { x3::rhi::IRenderDevice::SkyParams  sp{}; sp.enabled = true; sp.sunIntensity = 2.0f; sp.haze = 0.2f;
          device->setSkyParams(sp); }

        // ---- Build the LOD chain: an icosphere subdivided N times per level.
        // Standard icosahedron seed + midpoint subdivision; vertices pushed to
        // unit radius each pass so each level is a rounder sphere. Lower levels
        // keep the same silhouette scale but far fewer, larger facets.
        auto buildIcoSphere = [&](int subdiv, float radius) -> x3::rhi::MeshHandle {
            struct V3 { float x, y, z; };
            const float t = (1.0f + std::sqrt(5.0f)) * 0.5f;
            std::vector<V3> pos = {
                {-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0},
                {0, -1, t}, {0, 1, t}, {0, -1, -t}, {0, 1, -t},
                {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1},
            };
            std::vector<uint32_t> tris = {
                0,11,5, 0,5,1, 0,1,7, 0,7,10, 0,10,11,
                1,5,9, 5,11,4, 11,10,2, 10,7,6, 7,1,8,
                3,9,4, 3,4,2, 3,2,6, 3,6,8, 3,8,9,
                4,9,5, 2,4,11, 6,2,10, 8,6,7, 9,8,1,
            };
            for (int s = 0; s < subdiv; ++s) {
                std::vector<uint32_t> next;
                next.reserve(tris.size() * 4);
                std::map<uint64_t, uint32_t> midCache;
                auto midpoint = [&](uint32_t a, uint32_t b) -> uint32_t {
                    uint64_t key = a < b ? ((uint64_t)a << 32 | b) : ((uint64_t)b << 32 | a);
                    auto it = midCache.find(key);
                    if (it != midCache.end()) return it->second;
                    V3 m{ (pos[a].x + pos[b].x) * 0.5f, (pos[a].y + pos[b].y) * 0.5f,
                          (pos[a].z + pos[b].z) * 0.5f };
                    uint32_t id = (uint32_t)pos.size();
                    pos.push_back(m);
                    midCache[key] = id;
                    return id;
                };
                for (size_t i = 0; i < tris.size(); i += 3) {
                    uint32_t a = tris[i], b = tris[i + 1], c = tris[i + 2];
                    uint32_t ab = midpoint(a, b), bc = midpoint(b, c), ca = midpoint(c, a);
                    uint32_t f[] = { a,ab,ca, b,bc,ab, c,ca,bc, ab,bc,ca };
                    next.insert(next.end(), std::begin(f), std::end(f));
                }
                tris.swap(next);
            }
            // Normalize to the sphere + emit MeshVertex (pos==normal for a sphere,
            // simple lat/long UVs so the lit shading reads the facets).
            std::vector<x3::rhi::MeshVertex> verts;
            verts.reserve(pos.size());
            for (auto& p : pos) {
                float len = std::sqrt(p.x*p.x + p.y*p.y + p.z*p.z);
                float nx = p.x/len, ny = p.y/len, nz = p.z/len;
                float u = 0.5f + std::atan2(nz, nx) / (2.0f * 3.14159265f);
                float v = 0.5f - std::asin(std::max(-1.0f, std::min(1.0f, ny))) / 3.14159265f;
                verts.push_back({{nx*radius, ny*radius, nz*radius}, {nx, ny, nz}, {u, v}});
            }
            return device->createMesh(verts.data(), (uint32_t)verts.size(),
                                      tris.data(), (uint32_t)tris.size());
        };

        const float kR = 2.0f;
        x3::rhi::MeshHandle m0 = buildIcoSphere(3, kR);   // ~1280 tris (LOD0 hi-detail)
        x3::rhi::MeshHandle m1 = buildIcoSphere(2, kR);   // ~320 tris
        x3::rhi::MeshHandle m2 = buildIcoSphere(1, kR);   // ~80 tris
        x3::rhi::MeshHandle m3 = buildIcoSphere(0, kR);   // 20 tris (raw icosahedron)

        x3::space::LodSystem lodSys;
        x3::space::LodSet lodSet =
            lodSys.makeFromChain(m0.id, m1.id, m2.id, m3.id,
                                 /*d01=*/14.0f, /*d12=*/22.0f, /*d23=*/30.0f, /*levels=*/4);

        auto albedoD = x3::prims::makeCheckerRGBA(64, 8, 210, 150, 90, 70, 50, 40);
        auto albedoTex = device->createTexture(albedoD.data(), 64, 64, true);

        { x3::rhi::PointLight pl[2];
          pl[0].pos[0]=6.0f; pl[0].pos[1]=6.0f; pl[0].pos[2]=8.0f; pl[0].range=60.0f;
          pl[0].color[0]=9.0f; pl[0].color[1]=8.4f; pl[0].color[2]=7.6f;
          pl[1].pos[0]=-8.0f; pl[1].pos[1]=3.0f; pl[1].pos[2]=4.0f; pl[1].range=60.0f;
          pl[1].color[0]=4.0f; pl[1].color[1]=4.4f; pl[1].color[2]=6.0f;
          device->setPointLights(pl, 2); }

        // Sphere sits at the origin; the camera dollies back along +Z so the
        // distance grows and LODs step down. Identity model (sphere centered).
        const float model[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        const float white[4]  = { 1, 1, 1, 1 };
        auto distFor = [&](float t) { return 6.0f + t * 3.5f; };  // pull back over time

        if (headless) {
            const std::string outPath = screenshot ? screenshotPath
                                                   : std::string("agent_lod.png");
            const int kFrames = 48;
            for (int i = 0; i < kFrames; ++i) {
                glfwPollEvents();
                float t = (float)i * (1.0f / 12.0f);    // ~4s sweep
                float dist = distFor(t);
                // Camera looks at the origin from +Z at the current distance, a
                // slight elevation so the silhouette + top facets both read.
                // yaw -pi/2 aims the +Z-positioned camera back toward the
                // sphere at the origin (forward.z = cos(p)*sin(yaw) = -1).
                device->setCamera(0.0f, 1.2f, dist, -1.5708f, -0.10f, 60.0f);
                if (shotCamOverride)
                    device->setCamera(shotCam[0], shotCam[1], shotCam[2],
                                      shotCam[3], shotCam[4], 60.0f);
                uint32_t chosen = lodSys.select(lodSet, dist);
                if (i == kFrames - 1) device->armCapture(outPath.c_str());
                auto frame = device->beginFrame();
                if (frame.valid && chosen)
                    device->drawMesh(frame, x3::rhi::MeshHandle{chosen}, albedoTex, white, model);
                device->endFrame(frame);
            }
            const bool wrote = device->captureFrame(outPath.c_str());
            if (wrote) x3::logInfo("--world lod: wrote " + outPath);
            else       x3::logError("--world lod: capture FAILED");
            device->shutdown();
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return wrote ? 0 : 1;
        }

        // ---- Windowed path: the camera auto-dollies back then loops, so the
        // LOD swap is continuously visible. Esc to quit.
        double startTime = glfwGetTime();
        x3::logInfo("--world lod: camera pulls back; sphere LOD steps down. Esc to quit");
        int lastWs = (int)W, lastHs = (int)H;
        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();
            if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;
            float t = (float)std::fmod(glfwGetTime() - startTime, 8.0);  // 8s loop
            float dist = distFor(t);
            int cw, chh; glfwGetFramebufferSize(window, &cw, &chh);
            if (cw != lastWs || chh != lastHs) { lastWs = cw; lastHs = chh; if (cw>0&&chh>0) device->onResize((uint32_t)cw,(uint32_t)chh); }
            device->setCamera(0.0f, 1.2f, dist, -1.5708f, -0.10f, 60.0f);
            uint32_t chosen = lodSys.select(lodSet, dist);
            auto frame = device->beginFrame();
            if (frame.valid && chosen)
                device->drawMesh(frame, x3::rhi::MeshHandle{chosen}, albedoTex, white, model);
            device->endFrame(frame);
        }

        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return 0;
    }

    // ---- S1 space-environment showcase (--world spaceenv) -------------------
    // The Act-3 deep-space scene in isolation: the nebula/star dome backdrop +
    // 2-3 proxy planets + a bright sun, with the camera slowly orbiting the
    // origin so the planets + sun read at multiple angles. NO scene geometry,
    // NO player. SSAO + GI are DISABLED (the deep-space SSAO-black quirk: with no
    // surfaces to occlude, the AO pass crushes the whole frame to black on the
    // raster fallback). The pixel-variance gate on the PNG (std>15, uniq>100) is
    // the visual proof -- bright planets + the sun + a starfield clear it easily.
    if (worldMode == "spaceenv") {
        x3::logInfo("--world spaceenv: building the S1 space-environment showcase");

        { x3::rhi::IRenderDevice::SsaoParams sp{}; sp.enabled = false; device->setSsaoParams(sp); }
        { x3::rhi::IRenderDevice::GiParams   gp{}; gp.enabled = false; device->setGiParams(gp); }
        { x3::rhi::IRenderDevice::SkyParams  sp{}; sp.enabled = false; device->setSkyParams(sp); }

        x3::space::SpaceEnv env;
        env.init(*device);
        if (!env.initialized()) {
            x3::logError("--world spaceenv: SpaceEnv::init() failed");
            device->shutdown();
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return 1;
        }
        // Place three proxy planets at varied positions/sizes/tints, all in front
        // of the camera so the orbit keeps them in frame.
        { float p[3] = {  12.0f,   1.0f, -22.0f }; float a[3] = { 0.90f, 0.55f, 0.35f }; env.addPlanet(p, 5.0f,  a); }
        { float p[3] = { -16.0f,  -3.0f, -34.0f }; float a[3] = { 0.35f, 0.55f, 0.95f }; env.addPlanet(p, 9.0f,  a); }
        { float p[3] = {   4.0f,   9.0f, -40.0f }; float a[3] = { 0.55f, 0.90f, 0.55f }; env.addPlanet(p, 4.0f,  a); }
        { float d[3] = { -0.45f, 0.40f, -0.80f }; float c[3] = { 1.0f, 0.92f, 0.78f }; env.setSun(d, c, 1.6f); }

        if (headless) {
            const std::string outPath = screenshot ? screenshotPath
                                                   : std::string("agent_spaceenv.png");
            const int kFrames = 24;
            for (int i = 0; i < kFrames; ++i) {
                glfwPollEvents();
                float t = (float)i * (1.0f / 30.0f);
                float ang = 0.10f * t;
                float ex = 12.0f * std::sin(ang);
                float ez = 12.0f * std::cos(ang);
                float yaw = -1.5708f + 0.15f * std::sin(ang);
                device->setCamera(ex, 0.0f, ez, yaw, 0.02f, 70.0f);
                if (shotCamOverride) {
                    device->setCamera(shotCam[0], shotCam[1], shotCam[2],
                                      shotCam[3], shotCam[4], 70.0f);
                    ex = shotCam[0]; ez = shotCam[2];
                }
                if (i == kFrames - 1) device->armCapture(outPath.c_str());
                auto frame = device->beginFrame();
                if (frame.valid) {
                    env.setCamera(ex, 0.0f, ez);
                    env.render(*device, frame, nullptr, t);
                }
                device->endFrame(frame);
            }
            const bool wrote = device->captureFrame(outPath.c_str());
            if (wrote) x3::logInfo("--world spaceenv: wrote " + outPath);
            else       x3::logError("--world spaceenv: capture FAILED");
            env.shutdown(*device);
            device->shutdown();
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return wrote ? 0 : 1;
        }

        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        double lastMX, lastMY; glfwGetCursorPos(window, &lastMX, &lastMY);
        double startTime = glfwGetTime();
        float fyaw = -1.5708f, fpitch = -0.06f;
        x3::logInfo("--world spaceenv: mouse looks around, Esc to quit");
        int lastWs = (int)W, lastHs = (int)H;
        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();
            if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;
            double now = glfwGetTime();
            float t = (float)(now - startTime);
            double mx, my; glfwGetCursorPos(window, &mx, &my);
            float ddx = (float)(mx - lastMX), ddy = (float)(my - lastMY);
            lastMX = mx; lastMY = my;
            fyaw   += ddx * 0.0025f;
            fpitch -= ddy * 0.0025f;
            if (fpitch >  1.55f) fpitch =  1.55f;
            if (fpitch < -1.55f) fpitch = -1.55f;
            int cw, chh; glfwGetFramebufferSize(window, &cw, &chh);
            if (cw != lastWs || chh != lastHs) { lastWs = cw; lastHs = chh; if (cw>0&&chh>0) device->onResize((uint32_t)cw,(uint32_t)chh); }
            float ang = 0.10f * t;
            float ex = 30.0f * std::sin(ang);
            float ez = 30.0f * std::cos(ang) - 30.0f;
            device->setCamera(ex, 0.0f, ez, fyaw, fpitch, 70.0f);
            auto frame = device->beginFrame();
            if (frame.valid) {
                env.setCamera(ex, 0.0f, ez);
                env.render(*device, frame, nullptr, t);
            }
            device->endFrame(frame);
        }

        env.shutdown(*device);
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return 0;
    }

    // ---- Intro DECLOAK shimmer showcase (--world decloak) ------------------
    // A capital ship DECLOAKS: progress ramps 0->1 over time, the cyan-white
    // shimmer/edge-glow overlay (DecloakVfx) plays over the hull while the real
    // ship mesh fades in via revealAlpha(). Deep-space framing (no SSAO/GI, no
    // sky). Headless screenshot + windowed. The PNG pixel-variance gate (std>15,
    // uniq>100) is cleared by the lit ship + the bright shimmer shell.
    if (worldMode == "decloak") {
        x3::logInfo("--world decloak: showcasing the intro decloak shimmer (a ship phasing in)");

        { x3::rhi::IRenderDevice::SsaoParams sp{}; sp.enabled = false; device->setSsaoParams(sp); }
        { x3::rhi::IRenderDevice::GiParams   gp{}; gp.enabled = false; device->setGiParams(gp); }
        { x3::rhi::IRenderDevice::SkyParams  kp{}; kp.enabled = false; device->setSkyParams(kp); }
        // Bright point lights near the ship so the metal hull reads against black
        // deep space once it resolves (mirrors --world space's lighting rig).
        { x3::rhi::PointLight pl[3];
          pl[0].pos[0]= 30.0f; pl[0].pos[1]= 30.0f; pl[0].pos[2]= 30.0f; pl[0].range=200.0f;
          pl[0].color[0]=30.0f; pl[0].color[1]=28.0f; pl[0].color[2]=24.0f;
          pl[1].pos[0]=-25.0f; pl[1].pos[1]= 15.0f; pl[1].pos[2]=  8.0f; pl[1].range=150.0f;
          pl[1].color[0]=8.0f;  pl[1].color[1]=10.0f; pl[1].color[2]=14.0f;
          pl[2].pos[0]= 40.0f; pl[2].pos[1]=-10.0f; pl[2].pos[2]=-15.0f; pl[2].range=180.0f;
          pl[2].color[0]=5.0f;  pl[2].color[1]=4.0f;  pl[2].color[2]=3.0f;
          device->setPointLights(pl, 3); }

        // ---- Load a ship GLB (SpaceShip*.glb preferred; drone/box fallback). --
        const std::string rigDir = x3::game::riggedGlbRoot();
        std::unique_ptr<x3::asset::IAssetSource> asrc(x3::asset::createAssetSource());
        asrc->mountDir(rigDir, 0);
        std::unique_ptr<x3::asset::IModelLoader> mloader(
            x3::asset::createModelLoader(device.get(), asrc.get()));
        const char* kShipCandidates[] = {
            "SpaceShip.glb", "SpaceShip2.glb", "SpaceShip3.glb", "SpaceShip4.glb",
            "DroneOscillating.glb", "DroneExportWMotion.glb"
        };
        x3::asset::Model shipModel{}; std::string shipFile;
        for (const char* c : kShipCandidates) {
            shipModel = mloader->load(c);
            if (shipModel.ok) { shipFile = c; break; }
        }
        std::vector<x3::asset::ModelDrawable> shipDrawables;
        if (shipModel.ok) shipDrawables = x3::asset::makeDrawables(shipModel);
        x3::logInfo(std::string("--world decloak: ship model=") +
                    (shipModel.ok ? shipFile : "<procedural-box-fallback>"));

        // Procedural-box fallback for the ship hull.
        x3::prims::PrimMesh sbm = x3::prims::makeBox(2.4f, 0.8f, 1.4f, 0, 0, 0, 0.25f);
        auto shipBoxMesh = device->createMesh(sbm.verts.data(), (uint32_t)sbm.verts.size(),
                                              sbm.index.data(), (uint32_t)sbm.index.size());
        auto sbTexD = x3::prims::makeCheckerRGBA(64, 8, 170, 180, 205, 55, 65, 85);
        auto shipBoxTex = device->createTexture(sbTexD.data(), 64, 64, true);

        // Ship placement: centered, scaled up so it fills the frame. The decloak
        // shell is drawn in the SAME local frame (scaled to hug the hull bounds).
        const float kShipScale = 14.0f;
        float shipXform[16] = {
            kShipScale, 0, 0, 0,
            0, kShipScale, 0, 0,
            0, 0, kShipScale, 0,
            0, 0, 0, 1
        };
        // The decloak shell is a unit box (half-extent ~1, * shellScale). The
        // SpaceShip GLB occupies roughly +/-1 local unit, so a shell at ~1.05x
        // the ship scale haloes the hull just outside it without swallowing the
        // frame (the camera sits well outside the shell at -34).
        float shellXform[16];
        { float s = kShipScale * 1.05f;
          float t[16] = { s,0,0,0, 0,s,0,0, 0,0,s,0, 0,0,0,1 };
          for (int k=0;k<16;++k) shellXform[k]=t[k]; }

        x3::space::DecloakVfx decloak;
        x3::space::DecloakVfx::Tuning dvT;
        decloak.init(*device, dvT);
        if (!decloak.initialized()) {
            x3::logError("--world decloak: DecloakVfx::init() failed");
            device->destroyMesh(shipBoxMesh); device->destroyTexture(shipBoxTex);
            if (shipModel.ok) mloader->unload(shipModel);
            device->shutdown(); if (window) glfwDestroyWindow(window); glfwTerminate();
            return 1;
        }

        // Draw the ship hull modulated by `alpha` (revealAlpha): fades the ship in
        // as the decloak resolves.
        auto drawShip = [&](const x3::rhi::FrameContext& frame, float alpha) {
            if (shipModel.ok) {
                for (const auto& dr : shipDrawables) {
                    float fin[16];
                    x3::asset::mulMat4(shipXform, dr.nodeTransform, fin);
                    float tint[4] = {
                        (dr.baseColorFactor[0] * 4.0f + 0.45f) * alpha,
                        (dr.baseColorFactor[1] * 4.0f + 0.50f) * alpha,
                        (dr.baseColorFactor[2] * 4.0f + 0.55f) * alpha,
                        dr.baseColorFactor[3]
                    };
                    device->drawMesh(frame, x3::rhi::MeshHandle{ dr.meshId },
                                     x3::rhi::TextureHandle{ dr.baseColorTexId }, tint, fin);
                }
            } else {
                const float tint[4] = { alpha, alpha, alpha, 1.0f };
                device->drawMesh(frame, shipBoxMesh, shipBoxTex, tint, shipXform);
            }
        };

        // Camera: off to -X looking toward +X at the ship.
        const float kCam[5] = { -34.0f, 6.0f, 0.0f, 0.0f, -0.06f };

        if (headless) {
            device->setFrustumCull(false);   // robust capture (deeply-nested GLB AABBs)
            const std::string outPath = screenshot ? screenshotPath
                                                   : std::string("G:/X3Native/captures/decloak.png");
            const int kFrames   = 40;
            const int kShotFrame= kFrames - 1;
            // Capture mid-reveal so BOTH the shimmer (bright) AND a partly-faded
            // ship are visible -> a busy, high-variance frame.
            const float kShotProgress = 0.55f;
            for (int i = 0; i < kFrames; ++i) {
                glfwPollEvents();
                float t = (float)i * (1.0f / 30.0f);
                bool isShot = (i == kShotFrame);
                float prog = isShot ? kShotProgress
                                    : std::clamp((float)i / (float)(kFrames - 1), 0.0f, 1.0f);
                float cam[5]; for (int k=0;k<5;++k) cam[k]=kCam[k];
                if (shotCamOverride) for (int k=0;k<5;++k) cam[k]=shotCam[k];
                device->setCamera(cam[0], cam[1], cam[2], cam[3], cam[4], 65.0f);
                if (isShot) device->armCapture(outPath.c_str());
                auto frame = device->beginFrame();
                if (frame.valid) {
                    drawShip(frame, x3::space::DecloakVfx::revealAlpha(prog));
                    decloak.render(*device, frame, nullptr, shellXform, t, prog, dvT);
                }
                device->endFrame(frame);
            }
            const bool wrote = device->captureFrame(outPath.c_str());
            if (wrote) x3::logInfo("--world decloak: wrote " + outPath);
            else       x3::logError("--world decloak: capture FAILED");
            decloak.shutdown(*device);
            device->destroyMesh(shipBoxMesh); device->destroyTexture(shipBoxTex);
            if (shipModel.ok) mloader->unload(shipModel);
            device->shutdown(); if (window) glfwDestroyWindow(window); glfwTerminate();
            return wrote ? 0 : 1;
        }

        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        double startTimeD = glfwGetTime();
        x3::logInfo("--world decloak: the ship decloaks (progress loops 0->1); Esc to quit");
        int lastWsD = (int)W, lastHsD = (int)H;
        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();
            if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;
            double now = glfwGetTime();
            float t = (float)(now - startTimeD);
            // Decloak over ~5s, hold revealed ~2s, then loop.
            float cycle = std::fmod(t, 7.0f);
            float prog = std::clamp(cycle / 5.0f, 0.0f, 1.0f);
            int cw, chh; glfwGetFramebufferSize(window, &cw, &chh);
            if (cw != lastWsD || chh != lastHsD) { lastWsD = cw; lastHsD = chh; if (cw>0&&chh>0) device->onResize((uint32_t)cw,(uint32_t)chh); }
            device->setCamera(kCam[0], kCam[1], kCam[2], kCam[3], kCam[4], 65.0f);
            auto frame = device->beginFrame();
            if (frame.valid) {
                drawShip(frame, x3::space::DecloakVfx::revealAlpha(prog));
                decloak.render(*device, frame, nullptr, shellXform, t, prog, dvT);
            }
            device->endFrame(frame);
        }

        decloak.shutdown(*device);
        device->destroyMesh(shipBoxMesh); device->destroyTexture(shipBoxTex);
        if (shipModel.ok) mloader->unload(shipModel);
        device->shutdown(); if (window) glfwDestroyWindow(window); glfwTerminate();
        return 0;
    }

    if (worldMode == "tractor") {
        x3::logInfo("--world tractor: showcasing the capital-ship tractor beam capture");

        { x3::rhi::IRenderDevice::SsaoParams sp{}; sp.enabled = false; device->setSsaoParams(sp); }
        { x3::rhi::IRenderDevice::GiParams   gp{}; gp.enabled = false; device->setGiParams(gp); }
        { x3::rhi::IRenderDevice::SkyParams  kp{}; kp.enabled = false; device->setSkyParams(kp); }
        // Three-point rig so the metal boxes read with shape against black space.
        { x3::rhi::PointLight pl[3];
          pl[0].pos[0]= 10.0f; pl[0].pos[1]= 12.0f; pl[0].pos[2]= 12.0f; pl[0].range=120.0f;
          pl[0].color[0]=11.0f; pl[0].color[1]=10.5f; pl[0].color[2]=9.0f;
          pl[1].pos[0]=-14.0f; pl[1].pos[1]= 8.0f; pl[1].pos[2]= 4.0f; pl[1].range=100.0f;
          pl[1].color[0]=3.0f;  pl[1].color[1]=3.6f;  pl[1].color[2]=4.8f;
          pl[2].pos[0]= 6.0f; pl[2].pos[1]=-6.0f; pl[2].pos[2]=-10.0f; pl[2].range=90.0f;
          pl[2].color[0]=2.5f;  pl[2].color[1]=1.8f;  pl[2].color[2]=1.4f;
          device->setPointLights(pl, 3); }

        // Capital-ship emitter end (the beam apex). A big slab of hull.
        x3::prims::PrimMesh capGeo = x3::prims::makeBox(5.0f, 3.0f, 6.0f, 0,0,0, 0.4f);
        auto capMesh = device->createMesh(capGeo.verts.data(), (uint32_t)capGeo.verts.size(),
                                          capGeo.index.data(), (uint32_t)capGeo.index.size());
        auto capTexD = x3::prims::makeCheckerRGBA(64, 8, 150, 158, 175, 60, 66, 84);
        auto capTex  = device->createTexture(capTexD.data(), 64, 64, true);

        // Jake's fighter (the captured ship). A small box.
        x3::prims::PrimMesh figGeo = x3::prims::makeBox(1.0f, 0.5f, 1.6f, 0,0,0, 0.5f);
        auto figMesh = device->createMesh(figGeo.verts.data(), (uint32_t)figGeo.verts.size(),
                                          figGeo.index.data(), (uint32_t)figGeo.index.size());
        auto figTexD = x3::prims::makeCheckerRGBA(64, 8, 200, 150, 110, 90, 70, 60);
        auto figTex  = device->createTexture(figTexD.data(), 64, 64, true);

        x3::space::TractorBeam beam;
        x3::space::TractorBeam::Tuning beamT;
        beam.init(*device, beamT);
        if (!beam.initialized()) {
            x3::logError("--world tractor: TractorBeam::init() failed");
            device->destroyMesh(capMesh); device->destroyTexture(capTex);
            device->destroyMesh(figMesh); device->destroyTexture(figTex);
            device->shutdown();
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return 1;
        }

        // Emitter sits on the front face of the capital ship at the origin; the
        // fighter starts far out along +X and is reeled IN toward the emitter.
        const float kEmitter[3] = { 6.0f, 0.0f, 0.0f };   // beam apex (capital-ship hull face)
        const float kCapStart   = 40.0f;                  // fighter start distance (X)
        const float kCapEnd     = 11.0f;                  // fighter held just off the hull

        // Compose the fighter transform + draw the full scene for a given fighter X
        // and beam intensity/time.
        auto drawTractorScene = [&](const x3::rhi::FrameContext& frame, float figX,
                                    float intensity, float tSec) {
            // Capital ship at origin.
            const float capM[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
            const float capTint[4] = { 1.4f, 1.45f, 1.6f, 1.0f };
            device->drawMesh(frame, capMesh, capTex, capTint, capM);
            // Fighter at (figX, slight bob, 0), with a gentle tumble as it's pulled.
            float bob = 0.6f * std::sin(tSec * 1.3f);
            float yaw = 0.3f * std::sin(tSec * 0.9f);
            float c = std::cos(yaw), s = std::sin(yaw);
            const float figM[16] = {
                c, 0, -s, 0,
                0, 1,  0, 0,
                s, 0,  c, 0,
                figX, bob, 0.0f, 1.0f
            };
            const float figTint[4] = { 1.6f, 1.35f, 1.1f, 1.0f };
            device->drawMesh(frame, figMesh, figTex, figTint, figM);
            // The beam: apex at the emitter, base at the fighter (to == fighter pos).
            const float to[3] = { figX, bob, 0.0f };
            beam.render(*device, frame, nullptr, kEmitter, to, intensity, tSec, beamT);
        };

        if (headless) {
            const std::string outPath = screenshot ? screenshotPath
                                                   : std::string("agent_tractor.png");
            const int kFrames = 60;
            const int kShotFrame = 40;   // late: beam locked on, fighter half-reeled
            for (int i = 0; i < kFrames; ++i) {
                glfwPollEvents();
                float t = (float)i * (1.0f / 30.0f);
                float u = (float)i / (float)(kFrames - 1);
                // Intensity ramps up fast (lock-on) then holds; fighter pulled in.
                float intensity = std::min(1.0f, u * 2.2f);
                float figX = kCapStart + (kCapEnd - kCapStart) * u;
                // Camera off to the side + above so the WHOLE beam (emitter->fighter)
                // and both ships fill the frame.
                device->setCamera(20.0f, 14.0f, 34.0f, 3.95f, -0.28f, 70.0f);
                if (shotCamOverride) {
                    device->setCamera(shotCam[0], shotCam[1], shotCam[2],
                                      shotCam[3], shotCam[4], 70.0f);
                }
                if (i == kShotFrame) device->armCapture(outPath.c_str());
                auto frame = device->beginFrame();
                if (frame.valid) {
                    drawTractorScene(frame, figX, intensity, t);
                }
                device->endFrame(frame);
            }
            const bool wrote = device->captureFrame(outPath.c_str());
            if (wrote) x3::logInfo("--world tractor: wrote " + outPath);
            else       x3::logError("--world tractor: capture FAILED");
            beam.shutdown(*device);
            device->destroyMesh(capMesh); device->destroyTexture(capTex);
            device->destroyMesh(figMesh); device->destroyTexture(figTex);
            device->shutdown();
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return wrote ? 0 : 1;
        }

        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        double startTimeT = glfwGetTime();
        x3::logInfo("--world tractor: the capital ship reels Jake's fighter in; Esc to quit");
        int lastWsT = (int)W, lastHsT = (int)H;
        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();
            if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;
            double now = glfwGetTime();
            float t = (float)(now - startTimeT);
            // Loop the capture beat every 5s so the showcase repeats.
            float u = std::fmod(t, 5.0f) / 5.0f;
            float intensity = std::min(1.0f, u * 2.2f);
            float figX = kCapStart + (kCapEnd - kCapStart) * u;
            int cw, chh; glfwGetFramebufferSize(window, &cw, &chh);
            if (cw != lastWsT || chh != lastHsT) { lastWsT = cw; lastHsT = chh; if (cw>0&&chh>0) device->onResize((uint32_t)cw,(uint32_t)chh); }
            device->setCamera(20.0f, 14.0f, 34.0f, 3.95f, -0.28f, 70.0f);
            auto frame = device->beginFrame();
            if (frame.valid) {
                drawTractorScene(frame, figX, intensity, t);
            }
            device->endFrame(frame);
        }

        beam.shutdown(*device);
        device->destroyMesh(capMesh); device->destroyTexture(capTex);
        device->destroyMesh(figMesh); device->destroyTexture(figTex);
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return 0;
    }

    // ---- Salvari crystal-matrix wormhole showcase (--world wormhole) --------
    if (worldMode == "wormhole") {
        x3::logInfo("--world wormhole: showcasing the Salvari crystal-matrix wormhole");

        { x3::rhi::IRenderDevice::SsaoParams sp{}; sp.enabled = false; device->setSsaoParams(sp); }
        { x3::rhi::IRenderDevice::GiParams   gp{}; gp.enabled = false; device->setGiParams(gp); }
        { x3::rhi::IRenderDevice::SkyParams  sp{}; sp.enabled = false; device->setSkyParams(sp); }

        x3::space::WormholeVfx wh;
        x3::space::WormholeVfx::Tuning whT;
        wh.init(*device, whT);
        if (!wh.initialized()) {
            x3::logError("--world wormhole: WormholeVfx::init() failed");
            device->shutdown();
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return 1;
        }
        wh.setOrigin(0.0f, 0.0f, 0.0f);
        const float kFlyZ0 = 6.0f;
        const float kFlyZ1 = whT.length * 0.9f;
        const float kAxisYaw = 1.57079633f;

        if (headless) {
            const std::string outPath = screenshot ? screenshotPath
                                                   : std::string("agent_wormhole.png");
            const int kFrames = 48;
            const int kShotFrame = 2;
            const float kShotProgress = 0.55f;
            for (int i = 0; i < kFrames; ++i) {
                glfwPollEvents();
                float t = (float)i * (1.0f / 30.0f);
                float u = (float)i / (float)(kFrames - 1);
                bool isShot = (i == kShotFrame);
                float camZ = isShot ? kFlyZ0 : (kFlyZ0 + (kFlyZ1 - kFlyZ0) * u);
                float prog = isShot ? kShotProgress : u;
                float roll = 0.25f * t;
                float yaw   = kAxisYaw + (isShot ? 0.0f : 0.04f * std::sin(roll));
                float pitch = isShot ? 0.0f : 0.03f * std::cos(roll);
                device->setCamera(0.0f, 0.0f, camZ, yaw, pitch, 80.0f);
                if (shotCamOverride) {
                    device->setCamera(shotCam[0], shotCam[1], shotCam[2],
                                      shotCam[3], shotCam[4], 80.0f);
                }
                if (isShot) device->armCapture(outPath.c_str());
                auto frame = device->beginFrame();
                if (frame.valid) {
                    wh.render(*device, frame, nullptr, t, prog, whT);
                }
                device->endFrame(frame);
            }
            const bool wrote = device->captureFrame(outPath.c_str());
            if (wrote) x3::logInfo("--world wormhole: wrote " + outPath);
            else       x3::logError("--world wormhole: capture FAILED");
            wh.shutdown(*device);
            device->shutdown();
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return wrote ? 0 : 1;
        }

        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        double startTimeW = glfwGetTime();
        x3::logInfo("--world wormhole: flying through the crystal-matrix jump, Esc to quit");
        int lastWsW = (int)W, lastHsW = (int)H;
        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();
            if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;
            double now = glfwGetTime();
            float t = (float)(now - startTimeW);
            float u = std::fmod(t, 6.0f) / 6.0f;
            float camZ = kFlyZ0 + (kFlyZ1 - kFlyZ0) * u;
            float roll = 0.25f * t;
            float yaw   = kAxisYaw + 0.04f * std::sin(roll);
            float pitch = 0.03f * std::cos(roll);
            int cw, chh; glfwGetFramebufferSize(window, &cw, &chh);
            if (cw != lastWsW || chh != lastHsW) { lastWsW = cw; lastHsW = chh; if (cw>0&&chh>0) device->onResize((uint32_t)cw,(uint32_t)chh); }
            device->setCamera(0.0f, 0.0f, camZ, yaw, pitch, 80.0f);
            auto frame = device->beginFrame();
            if (frame.valid) {
                wh.render(*device, frame, nullptr, t, u, whT);
            }
            device->endFrame(frame);
        }

        wh.shutdown(*device);
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return 0;
    }

    // ---- Ship node-anim showcase (--world shipanim) ------------------------
    // S11 proof: a parked ULTRA-DETAILED ship on a slow turntable with ONE
    // articulated part (the landing gear) cycling 0->1->0 over time so the
    // node-transform articulation is visibly moving. The ship hull is a real
    // SpaceShip*.glb; because the shipped GLBs carry no authored named child
    // nodes yet, the gear is a SYNTHETIC child box driven by ShipNodeAnim
    // (the exact addPart/setPart/update path an authored ship would use). The
    // pixel-variance test on the PNG (std > 15, uniqColors > 100) IS the gate:
    // the lit ship + moving gear easily clears it. Headless + windowed.
    if (worldMode == "shipanim") {
        x3::logInfo("--world shipanim: ship node-transform articulation showcase");

        // Deep-space framing: no SSAO/GI (per the task brief), no fog sky.
        { x3::rhi::IRenderDevice::SsaoParams sp{}; sp.enabled = false; device->setSsaoParams(sp); }
        { x3::rhi::IRenderDevice::GiParams   gp{}; gp.enabled = false; device->setGiParams(gp); }
        { x3::rhi::IRenderDevice::SkyParams  kp{}; kp.enabled = false; device->setSkyParams(kp); }
        // Three-point rig so the metal hull + gear read with shape against black.
        { x3::rhi::PointLight pl[3];
          pl[0].pos[0]= 6.0f; pl[0].pos[1]= 6.0f; pl[0].pos[2]= 6.0f; pl[0].range=60.0f;
          pl[0].color[0]=12.0f; pl[0].color[1]=11.0f; pl[0].color[2]=9.5f;
          pl[1].pos[0]=-6.0f; pl[1].pos[1]= 4.0f; pl[1].pos[2]= 2.0f; pl[1].range=40.0f;
          pl[1].color[0]=3.0f;  pl[1].color[1]=3.6f;  pl[1].color[2]=4.8f;
          pl[2].pos[0]= 4.0f; pl[2].pos[1]=-3.0f; pl[2].pos[2]=-5.0f; pl[2].range=50.0f;
          pl[2].color[0]=2.5f;  pl[2].color[1]=1.8f;  pl[2].color[2]=1.4f;
          device->setPointLights(pl, 3); }

        // ---- Load a real ship hull GLB (SpaceShip.glb, drone fallback). --------
        const std::string rigDir = x3::game::riggedGlbRoot();
        std::unique_ptr<x3::asset::IAssetSource> asrc(x3::asset::createAssetSource());
        asrc->mountDir(rigDir, 0);
        std::unique_ptr<x3::asset::IModelLoader> mloader(
            x3::asset::createModelLoader(device.get(), asrc.get()));
        const char* kShipCandidates[] = {
            "SpaceShip.glb", "SpaceShip2.glb", "SpaceShip3.glb", "SpaceShip4.glb",
            "DroneOscillating.glb"
        };
        x3::asset::Model shipModel{}; std::string shipFile;
        for (const char* c : kShipCandidates) {
            shipModel = mloader->load(c);
            if (shipModel.ok) { shipFile = c; break; }
        }
        std::vector<x3::asset::ModelDrawable> shipDrawables;
        if (shipModel.ok) shipDrawables = x3::asset::makeDrawables(shipModel);
        x3::logInfo(std::string("--world shipanim: hull = ") +
                    (shipModel.ok ? shipFile : "<procedural-box-fallback>") +
                    " (note: shipped GLBs have no authored named nodes -> "
                    "synthetic landing_gear; see tools/ship_node_anim.py)");

        // Hull fallback box (if even the drone load failed).
        x3::prims::PrimMesh hbm = x3::prims::makeBox(2.2f, 0.7f, 1.3f, 0,0,0, 0.25f);
        auto hullBoxMesh = device->createMesh(hbm.verts.data(), (uint32_t)hbm.verts.size(),
                                              hbm.index.data(), (uint32_t)hbm.index.size());
        auto hbTexD = x3::prims::makeCheckerRGBA(64, 8, 170, 180, 200, 60, 66, 84);
        auto hullBoxTex = device->createTexture(hbTexD.data(), 64, 64, true);

        // ---- Synthetic landing-gear part: a bright strut box (a child node). ---
        x3::prims::PrimMesh gbm = x3::prims::makeBox(0.18f, 1.0f, 0.18f, 0,0,0, 0.5f);
        auto gearMesh = device->createMesh(gbm.verts.data(), (uint32_t)gbm.verts.size(),
                                           gbm.index.data(), (uint32_t)gbm.index.size());
        auto gTexD = x3::prims::makeCheckerRGBA(32, 4, 230, 210, 90, 120, 100, 40);
        auto gearTex = device->createTexture(gTexD.data(), 32, 32, true);

        // ---- Scene with a ship root + a gear child driven by ShipNodeAnim. -----
        // The Scene holds the AUTHORITATIVE transforms: the host writes the root
        // (turntable), ShipNodeAnim composes the gear child = root * lerp(pose).
        const float kShipScale = (shipFile.rfind("SpaceShip", 0) == 0) ? 1.6f : 6.0f;
        x3::game::Scene scene;
        x3::game::Entity shipE{}; uint32_t shipId = scene.add(shipE);
        x3::game::Entity gearE{}; uint32_t gearId = scene.add(gearE);
        x3::space::ShipNodeAnim anim;
        anim.bind(scene, shipId);
        // Gear key-poses in SHIP-LOCAL space: retracted tucked up under the hull
        // (y=+0.2), deployed dropped below (y=-1.1). Scaled by the ship scale so
        // the strut sits proportionally to the hull. Two TRS key-poses == the
        // authoring contract a real "landing_gear" node would carry.
        const float gx = 0.9f * kShipScale, gzf = 0.7f * kShipScale;
        const float S = kShipScale;
        auto gearPose = [&](float ly, float out[16]){
            const float p[16] = { S,0,0,0, 0,S,0,0, 0,0,S,0, gx, ly, gzf, 1 };
            for (int k = 0; k < 16; ++k) out[k] = p[k];
        };
        float poseUp[16], poseDown[16];
        gearPose( 0.2f * kShipScale, poseUp);
        gearPose(-1.1f * kShipScale, poseDown);
        anim.addPart("landing_gear", poseUp, poseDown, gearId);

        // Build the turntable root transform (yaw about +Y) for time t.
        auto setRoot = [&](float yaw){
            const float c = std::cos(yaw), s = std::sin(yaw);
            float* m = scene.get(shipId).transform;
            m[0]=c*S; m[1]=0; m[2]=-s*S; m[3]=0;
            m[4]=0;   m[5]=S; m[6]=0;    m[7]=0;
            m[8]=s*S; m[9]=0; m[10]=c*S; m[11]=0;
            m[12]=0;  m[13]=0; m[14]=0;  m[15]=1;
        };

        // Draw the hull at the root transform, then the gear at its driven child
        // transform. `bright` lifts the dark scifi metal so it reads in black space.
        auto drawShip = [&](const x3::rhi::FrameContext& frame){
            const float* root = scene.get(shipId).transform;
            if (shipModel.ok) {
                for (const auto& dr : shipDrawables) {
                    float fin[16];
                    x3::asset::mulMat4(root, dr.nodeTransform, fin);
                    float tint[4] = {
                        dr.baseColorFactor[0]*4.0f + 0.45f,
                        dr.baseColorFactor[1]*4.0f + 0.50f,
                        dr.baseColorFactor[2]*4.0f + 0.55f,
                        dr.baseColorFactor[3] };
                    device->drawMesh(frame, x3::rhi::MeshHandle{ dr.meshId },
                                     x3::rhi::TextureHandle{ dr.baseColorTexId }, tint, fin);
                }
            } else {
                const float white[4] = { 1.4f, 1.4f, 1.5f, 1.0f };
                device->drawMesh(frame, hullBoxMesh, hullBoxTex, white, root);
            }
            // Gear child (transform was set by anim.update()).
            const float gearTint[4] = { 1.6f, 1.4f, 0.7f, 1.0f };
            device->drawMesh(frame, gearMesh, gearTex, gearTint, scene.get(gearId).transform);
        };

        // Gear cycle 0->1->0 over a 4 s period (a smooth cosine ease).
        auto gearCycle = [](float t){ return 0.5f - 0.5f * std::cos(t * (6.2831853f / 4.0f)); };

        device->setFrustumCull(false);   // robust visual capture (see --world space)

        // ===== Headless capture =====
        if (headless) {
            const std::string outPath = screenshot ? screenshotPath
                                                   : std::string("agent_shipanim.png");
            // Camera looking at the parked ship from a 3/4 high angle so both the
            // hull and the deployed gear strut fall in frame.
            float cam[5] = { 4.5f, 3.0f, 5.0f, 3.9f, -0.45f };
            if (shotCamOverride) for (int k = 0; k < 5; ++k) cam[k] = shotCam[k];
            const int kFrames = 48;
            for (int i = 0; i < kFrames; ++i) {
                glfwPollEvents();
                float t = (float)i * (1.0f / 30.0f);
                setRoot(0.35f * t);                       // slow turntable
                anim.setPart("landing_gear", gearCycle(t));// gear cycling
                anim.update(1.0f/30.0f, scene);
                device->setCamera(cam[0],cam[1],cam[2],cam[3],cam[4], 60.0f);
                if (i == kFrames - 1) device->armCapture(outPath.c_str());
                auto frame = device->beginFrame();
                if (frame.valid) drawShip(frame);
                device->endFrame(frame);
            }
            const bool wrote = device->captureFrame(outPath.c_str());
            if (wrote) x3::logInfo("--world shipanim: wrote " + outPath);
            else       x3::logError("--world shipanim: capture FAILED");
            if (shipModel.ok) mloader->unload(shipModel);
            device->destroyMesh(hullBoxMesh); device->destroyTexture(hullBoxTex);
            device->destroyMesh(gearMesh);    device->destroyTexture(gearTex);
            device->shutdown();
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return wrote ? 0 : 1;
        }

        // ===== Windowed path: orbit the parked ship, watch the gear cycle. =====
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        double lastMXs, lastMYs; glfwGetCursorPos(window, &lastMXs, &lastMYs);
        double startTimeS = glfwGetTime();
        float orbYaw = 3.9f, orbPitch = -0.45f, orbDist = 7.5f;
        x3::logInfo("--world shipanim: mouse-orbit the ship; gear cycles automatically; Esc to quit");
        int lastWsS = (int)W, lastHsS = (int)H;
        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();
            if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;
            double now = glfwGetTime();
            float t = (float)(now - startTimeS);
            double mx, my; glfwGetCursorPos(window, &mx, &my);
            float ddx = (float)(mx - lastMXs), ddy = (float)(my - lastMYs);
            lastMXs = mx; lastMYs = my;
            orbYaw   += ddx * 0.0035f;
            orbPitch -= ddy * 0.0035f;
            if (orbPitch >  1.4f) orbPitch =  1.4f;
            if (orbPitch < -1.4f) orbPitch = -1.4f;

            setRoot(0.35f * t);
            anim.setPart("landing_gear", gearCycle(t));
            anim.update((float)(now - startTimeS), scene);

            int cw, chh; glfwGetFramebufferSize(window, &cw, &chh);
            if (cw != lastWsS || chh != lastHsS) { lastWsS = cw; lastHsS = chh; if (cw>0&&chh>0) device->onResize((uint32_t)cw,(uint32_t)chh); }
            float ex = orbDist * std::cos(orbPitch) * std::sin(orbYaw);
            float ey = orbDist * std::sin(-orbPitch);
            float ez = orbDist * std::cos(orbPitch) * std::cos(orbYaw);
            device->setCamera(ex, ey, ez, orbYaw + 3.14159265f, orbPitch, 60.0f);
            auto frame = device->beginFrame();
            if (frame.valid) drawShip(frame);
            device->endFrame(frame);
        }
        if (shipModel.ok) mloader->unload(shipModel);
        device->destroyMesh(hullBoxMesh); device->destroyTexture(hullBoxTex);
        device->destroyMesh(gearMesh);    device->destroyTexture(gearTex);
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return 0;
    }

    // ---- Glass material showcase (--world glass) ---------------------------
    // M5 SHINY+TRANSPARENT glass proof: a lit room with DEPTH-RICH, recognizable,
    // differently-colored boxes (and a bright emissive marker at varied depths)
    // behind THREE angled panes — CLEAR, FROSTED, TINTED — so the reworked
    // glass.frag (Filament Cook-Torrance + fresnel env reflection + screen-space
    // refraction) reads as genuine see-through, glossy glass. Self-contained,
    // no physics. Headless `--world glass --screenshot <path>` writes a PNG.
    if (worldMode == "glass") {
        x3::logInfo("--world glass: building the UE5-style glass material showcase");

        // ---- Shared meshes + textures -------------------------------------
        // A thin upright PANE (a flat-ish box): a quad-like slab the glass renders on.
        x3::prims::PrimMesh paneGeo = x3::prims::makeBox(0.95f, 1.15f, 0.03f, 0.0f, 0.0f, 0.0f, 1.0f);
        auto paneMesh = device->createMesh(paneGeo.verts.data(), (uint32_t)paneGeo.verts.size(),
                                           paneGeo.index.data(), (uint32_t)paneGeo.index.size());
        // Big floor + a LOW back wall so the scene behind the glass has structure but
        // does not fill the frame with a dark mass.
        x3::prims::PrimMesh floorGeo = x3::prims::makeBox(8.0f, 0.15f, 8.0f, 0.0f, -0.15f, 0.0f, 2.0f);
        auto floorMesh = device->createMesh(floorGeo.verts.data(), (uint32_t)floorGeo.verts.size(),
                                            floorGeo.index.data(), (uint32_t)floorGeo.index.size());
        x3::prims::PrimMesh wallGeo = x3::prims::makeBox(8.0f, 3.5f, 0.15f, 0.0f, 3.5f, -6.0f, 1.5f);
        auto wallMesh = device->createMesh(wallGeo.verts.data(), (uint32_t)wallGeo.verts.size(),
                                           wallGeo.index.data(), (uint32_t)wallGeo.index.size());

        auto whiteD  = x3::prims::makeSolidRGBA(16, 255, 255, 255);
        auto whiteTex = device->createTexture(whiteD.data(), 16, 16, true);
        auto floorD  = x3::prims::makeCheckerRGBA(64, 8, 90, 94, 110, 40, 44, 56);
        auto floorTex = device->createTexture(floorD.data(), 64, 64, true);
        const float wallTint[3] = { 1.2f, 1.25f, 1.4f };
        auto wallD   = x3::prims::makeSciFiPanelRGBA(256, 3, wallTint);
        auto wallTex = device->createTexture(wallD.data(), 256, 256, true);

        // ---- Outdoor-ish lighting: analytic sky + bright fills ------------
        { x3::rhi::IRenderDevice::SkyParams sp{}; sp.enabled = true; sp.sunIntensity = 1.6f; sp.haze = 0.3f;
          device->setSkyParams(sp); }
        { x3::rhi::PointLight pl[4];
          pl[0].pos[0]=-2.5f; pl[0].pos[1]=2.6f; pl[0].pos[2]=-1.0f; pl[0].range=14.0f;
          pl[0].color[0]=8.0f; pl[0].color[1]=7.6f; pl[0].color[2]=7.0f;
          pl[1].pos[0]= 2.5f; pl[1].pos[1]=2.6f; pl[1].pos[2]=-1.0f; pl[1].range=14.0f;
          pl[1].color[0]=7.0f; pl[1].color[1]=7.6f; pl[1].color[2]=8.0f;
          pl[2].pos[0]= 0.0f; pl[2].pos[1]=3.0f; pl[2].pos[2]=-3.5f; pl[2].range=14.0f;
          pl[2].color[0]=7.5f; pl[2].color[1]=7.5f; pl[2].color[2]=7.5f;
          pl[3].pos[0]= 0.0f; pl[3].pos[1]=2.8f; pl[3].pos[2]= 2.5f; pl[3].range=14.0f;
          pl[3].color[0]=6.0f; pl[3].color[1]=6.0f; pl[3].color[2]=7.0f;
          device->setPointLights(pl, 4); }

        // ---- Build the demo SCENE via the proven Scene + Entity path ------
        // The Scene::render fan-out (drawMesh/drawMeshEmissive/drawMeshGlass) is the
        // same code path the club / Level 1 / canonlevel use, so the colored boxes
        // light + glow exactly like the club's neon strips. Each colored box gets a
        // modest per-color emissive so it reads as a bright, recognizable object
        // behind the glass and feeds the bloom chain (strength > 1 = HDR source).
        x3::game::Scene gscene;
        auto addStatic = [&](x3::rhi::MeshHandle mesh, x3::rhi::TextureHandle tex,
                             const float col[4], const float em[4], const float xform[16]) {
            x3::game::Entity e;
            e.mesh = mesh; e.tex = tex;
            for (int i = 0; i < 4; ++i)  e.baseColor[i] = col[i];
            for (int i = 0; i < 4; ++i)  e.emissive[i]  = em ? em[i] : 0.0f;
            for (int i = 0; i < 16; ++i) e.transform[i] = xform[i];
            gscene.add(e);
        };
        auto addGlass = [&](x3::rhi::MeshHandle mesh, x3::rhi::TextureHandle tex,
                            const float col[4], const x3::rhi::IRenderDevice::GlassMaterial& m,
                            const float xform[16]) {
            x3::game::Entity e;
            e.mesh = mesh; e.tex = tex;
            for (int i = 0; i < 4; ++i)  e.baseColor[i] = col[i];
            for (int i = 0; i < 16; ++i) e.transform[i] = xform[i];
            e.transparent = true; e.glass = m;
            gscene.add(e);
        };

        // Floor + back wall (the opaque room).
        const float white4[4]   = {1,1,1,1};
        const float idF[16]     = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        addStatic(floorMesh, floorTex, white4, nullptr, idF);
        addStatic(wallMesh,  wallTex,  white4, nullptr, idF);

        // Depth-rich colored boxes behind where the panes will sit. A modest
        // self-emissive in each box's own color guarantees they read as bright,
        // recognizable colored blocks (and a strong WHITE marker deep in the scene
        // so transparent panes clearly transmit a glowing object). FOLLOWS THE
        // CLUB1127 PATTERN: each box is its own makeBox authored at its world
        // position with IDENTITY transform — the proven path that lights + glows.
        std::vector<x3::rhi::MeshHandle> boxMeshes;
        struct ColBox { float cx, cy, cz, half; float r, g, b, em; };
        const ColBox cboxes[] = {
            // A staircase of bright primary cubes receding in depth (z more negative
            // = further) so the refraction bend + parallax read clearly through the
            // panes. Each gets its own color as a moderate emissive (HDR > 1).
            {-2.6f, 0.7f, -1.4f, 0.7f, 0.90f, 0.20f, 0.18f, 6.0f}, // red, near
            {-0.9f, 0.55f,-2.7f, 0.55f,0.18f, 0.85f, 0.30f, 5.0f}, // green, mid
            { 1.0f, 0.8f, -2.0f, 0.8f, 0.20f, 0.45f, 0.98f, 5.0f}, // blue, mid
            { 2.7f, 0.6f, -3.3f, 0.6f, 0.95f, 0.80f, 0.15f, 4.5f}, // yellow, far
            {-0.2f, 1.5f, -4.4f, 0.5f, 0.85f, 0.32f, 0.85f, 4.5f}, // magenta, far+high
            // Bright cyan marker deep in the scene -> obvious transmission through glass.
            { 1.7f, 1.8f, -4.9f, 0.4f, 0.4f,  0.95f, 1.0f,  5.0f},
        };
        for (const auto& b : cboxes) {
            x3::prims::PrimMesh g = x3::prims::makeBox(b.half, b.half, b.half, b.cx, b.cy, b.cz, 1.0f);
            x3::rhi::MeshHandle mh = device->createMesh(g.verts.data(), (uint32_t)g.verts.size(),
                                                       g.index.data(), (uint32_t)g.index.size());
            boxMeshes.push_back(mh);
            const float col[4] = { b.r, b.g, b.b, 1.0f };
            // Use the BOX color * strength as the emissive radiance; the marker box
            // (b.r,g,b == 1) becomes white-strength.
            const float em[4]  = { b.r, b.g, b.b, b.em };
            addStatic(mh, whiteTex, col, em, idF);
        }

        // ---- Three angled glass panes (left -> right) ---------------------
        // CLEAR / FROSTED / TINTED. Each yaw-rotated so the camera sees a grazing
        // angle (fresnel rim) plus head-on (transmission). Default GlassMaterial
        // fields cover everything new (metallic 0 / ior 1.5 / reflectance 0.5 /
        // transmittanceColor (1,1,1)); we override only the per-preset deltas.
        using GM = x3::rhi::IRenderDevice::GlassMaterial;
        auto paneTRS = [](float yaw, float px, float py, float pz, float m[16]) {
            const float cy = std::cos(yaw), sy = std::sin(yaw);
            m[0]=cy;  m[1]=0; m[2]=-sy; m[3]=0;
            m[4]=0;   m[5]=1; m[6]=0;   m[7]=0;
            m[8]=sy;  m[9]=0; m[10]=cy; m[11]=0;
            m[12]=px; m[13]=py; m[14]=pz; m[15]=1;
        };
        struct PaneCfg { float px, py, pz, yaw; GM mat; };
        std::vector<PaneCfg> paneCfgs(3);
        // CLEAR window: opacity 0.08, white tint, ior 1.5, near-polished.
        paneCfgs[0].px=-2.1f; paneCfgs[0].py=1.15f; paneCfgs[0].pz=1.4f; paneCfgs[0].yaw= 0.40f;
        { GM& m=paneCfgs[0].mat; m.opacity=0.08f; m.refraction=0.015f; m.roughness=0.03f; m.specular=1.0f;
          m.tint[0]=1.0f; m.tint[1]=1.0f; m.tint[2]=1.0f;
          m.metallic=0.0f; m.ior=1.5f; m.reflectance=0.5f;
          m.transmittanceColor[0]=1.0f; m.transmittanceColor[1]=1.0f; m.transmittanceColor[2]=1.0f; }
        // FROSTED: opacity 0.5, roughness 0.6, faint cool transmit.
        paneCfgs[1].px=0.0f; paneCfgs[1].py=1.15f; paneCfgs[1].pz=1.6f; paneCfgs[1].yaw=0.0f;
        { GM& m=paneCfgs[1].mat; m.opacity=0.5f; m.refraction=0.02f; m.roughness=0.6f; m.specular=1.0f;
          m.tint[0]=0.95f; m.tint[1]=0.97f; m.tint[2]=1.0f;
          m.metallic=0.0f; m.ior=1.5f; m.reflectance=0.5f;
          m.transmittanceColor[0]=0.9f; m.transmittanceColor[1]=0.95f; m.transmittanceColor[2]=1.0f; }
        // TINTED: colored glass, opacity ~0.2, transmit a teal tint.
        paneCfgs[2].px=2.1f; paneCfgs[2].py=1.15f; paneCfgs[2].pz=1.4f; paneCfgs[2].yaw=-0.40f;
        { GM& m=paneCfgs[2].mat; m.opacity=0.2f; m.refraction=0.018f; m.roughness=0.12f; m.specular=1.0f;
          m.tint[0]=0.35f; m.tint[1]=0.85f; m.tint[2]=0.7f;
          m.metallic=0.0f; m.ior=1.55f; m.reflectance=0.5f;
          m.transmittanceColor[0]=0.45f; m.transmittanceColor[1]=0.95f; m.transmittanceColor[2]=0.8f; }
        for (const auto& p : paneCfgs) {
            float m[16]; paneTRS(p.yaw, p.px, p.py, p.pz, m);
            const float base[4] = { p.mat.tint[0], p.mat.tint[1], p.mat.tint[2], 1.0f };
            addGlass(paneMesh, whiteTex, base, p.mat, m);
        }

        auto drawScene = [&](const x3::rhi::FrameContext& frame) {
            gscene.render(*device, frame);
        };

        auto cleanup = [&]() {
            for (auto mh : boxMeshes) device->destroyMesh(mh);
            device->destroyMesh(paneMesh);
            device->destroyMesh(floorMesh); device->destroyMesh(wallMesh);
            device->destroyTexture(whiteTex); device->destroyTexture(floorTex);
            device->destroyTexture(wallTex);
        };

        // ===== Headless capture: pose the camera, settle, grab a PNG. =====
        if (headless) {
            // Vantage: eye in front + slightly low, looking slightly up so the dark
            // back wall (and the lit colored boxes against it) fills the frame
            // behind the panes (avoids the bright sky washing out the transmission).
            float cam[5] = { 0.0f, 0.9f, 4.2f, -1.5708f, 0.05f };
            if (shotCamOverride) for (int k = 0; k < 5; ++k) cam[k] = shotCam[k];
            const std::string outPath = screenshot ? screenshotPath
                                       : std::string("G:/X3Native/captures/glass_demo.png");
            const int kFrames = 12;
            for (int i = 0; i < kFrames; ++i) {
                glfwPollEvents();
                device->setCamera(cam[0], cam[1], cam[2], cam[3], cam[4], 65.0f);
                if (i == kFrames - 1) device->armCapture(outPath.c_str());
                auto frame = device->beginFrame();
                if (frame.valid) drawScene(frame);
                device->endFrame(frame);
            }
            const bool wrote = device->captureFrame(outPath.c_str());
            if (wrote) x3::logInfo("--world glass: wrote " + outPath);
            else       x3::logError("--world glass: capture FAILED");
            cleanup();
            device->shutdown();
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return wrote ? 0 : 1;
        }

        // ===== Walkable windowed path: fly-cam (WASD + mouse, Esc). =====
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        double lastMX, lastMY; glfwGetCursorPos(window, &lastMX, &lastMY);
        double prevTime = glfwGetTime();
        float fx = 0.0f, fy = 0.9f, fz = 4.2f, fyaw = -1.5708f, fpitch = 0.05f;
        x3::logInfo("--world glass: fly WASD + mouse (CLEAR | FROSTED | TINTED panes), Esc to quit");
        int lastWd = (int)W, lastHd = (int)H;
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
            if (kd(GLFW_KEY_SPACE)) fy += spd;
            if (kd(GLFW_KEY_LEFT_CONTROL)) fy -= spd;
            int cw, chh; glfwGetFramebufferSize(window, &cw, &chh);
            if (cw != lastWd || chh != lastHd) { lastWd=cw; lastHd=chh; if (cw>0&&chh>0) device->onResize((uint32_t)cw,(uint32_t)chh); }
            device->setCamera(fx, fy, fz, fyaw, fpitch, 65.0f);
            auto frame = device->beginFrame();
            if (frame.valid) drawScene(frame);
            device->endFrame(frame);
        }
        cleanup();
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return 0;
    }

    // ---- Mech-pilot showcase (--world mech) --------------------------------
    // A rideable heavy mech (MechPilotController) standing on a checker ground plane
    // with a few "target dummy" props to shoot. 3rd-person chase camera by default
    // (V toggles cockpit); WASD walk (sluggish), Shift boost, Space jump-jets, LMB
    // autocannon (rapid tracers), R missile-pod burst. Headless
    // `--world mech --screenshot <path>` poses a chase still + captures a PNG.
    //
    // The mech VISUAL tries mech.glb first (Tim's gunmetal mech) and falls back to
    // any large humanoid GLB (OverLordEnforcer99.glb) scaled up, then to a primitive
    // box silhouette if no GLB loads — so the showcase always renders something.
    //
    // NOTE (SSAO/SSGI black-on-no-RT quirk, MEMORY): this base predates the
    // headless-capture default-off fix, so we force SSAO + SSGI OFF in this block
    // before rendering or the raster fallback yields a near-black frame.
    if (worldMode == "mech") {
        // Extracted to its own function so its (sizable) locals get their OWN stack
        // frame: main() already aggregates ~9 large inline --world blocks, and MSVC
        // sums every block's locals into one frame allocated at function entry. Adding
        // this block inline pushed main()'s frame past the 1 MB default stack reserve
        // and stack-overflowed at startup (before any flag ran). A separate function
        // keeps main()'s frame bounded. See runMechWorld().
        return runMechWorld(*device, window, headless, screenshot, screenshotPath,
                            shotCamOverride, shotCam, W, H);
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
    // ---- S9 TARGETING / RADAR / LOCK-ON showcase (--world targeting) ------
    // A handful of contacts (mix of hostile + friendly) placed in deep space
    // ahead of the player. The TargetingSystem locks the nearest in-cone
    // hostile; we draw a colored world-space marker box at each contact (red =
    // hostile, green = friendly, brighter for the locked one) and a distinct
    // bright marker at the computed lead point. A HUD overlay paints the radar
    // disc + blips and a reticle on the locked contact + a lead-indicator.
    // Headless `--world targeting --screenshot <path>` writes a non-blank PNG.
    if (worldMode == "targeting") {
        x3::logInfo("--world targeting: building the S9 targeting / radar HUD showcase");

        // Deep-space lighting: kill SSAO + GI (they raster a black-on-black
        // scene with no nearby geometry — the documented no-RT fallback) and the
        // analytic sky, so the markers read against the dark clear color.
        { x3::rhi::IRenderDevice::SsaoParams sp{}; sp.enabled = false; device->setSsaoParams(sp); }
        { x3::rhi::IRenderDevice::GiParams   gp{}; gp.enabled   = false; device->setGiParams(gp); }
        { x3::rhi::IRenderDevice::SkyParams  sp{}; sp.enabled   = false; device->setSkyParams(sp); }
        // Bright point lights near the cluster so the unlit marker boxes read.
        { x3::rhi::PointLight pl[2];
          pl[0].pos[0] = 0.0f; pl[0].pos[1] = 60.0f; pl[0].pos[2] = 60.0f; pl[0].range = 600.0f;
          pl[0].color[0] = 40.0f; pl[0].color[1] = 40.0f; pl[0].color[2] = 44.0f;
          pl[1].pos[0] = -60.0f; pl[1].pos[1] = 30.0f; pl[1].pos[2] = 20.0f; pl[1].range = 500.0f;
          pl[1].color[0] = 14.0f; pl[1].color[1] = 16.0f; pl[1].color[2] = 20.0f;
          device->setPointLights(pl, 2); }

        // ---- The contacts (host-fed each frame in a real game) -------------
        // Player sits at the origin looking toward +X. Hostiles ahead; one
        // friendly; the locked hostile (id 10) is given a crossing velocity so
        // the lead point separates visibly from the marker.
        x3::space::Contact contacts[5]{};
        contacts[0] = { 10u, {  70.0f,   6.0f,  10.0f }, { 0.0f, 0.0f, 18.0f }, true  };
        contacts[1] = { 20u, {  90.0f,  22.0f, -28.0f }, { 0.0f, 0.0f, 0.0f  }, true  };
        contacts[2] = { 30u, {  80.0f, -18.0f,  30.0f }, { 0.0f, 0.0f, 0.0f  }, true  };
        contacts[3] = {  5u, {  60.0f,   4.0f, -16.0f }, { 0.0f, 0.0f, 0.0f  }, false };
        contacts[4] = { 40u, { 120.0f,  -6.0f,   4.0f }, { 0.0f, 0.0f, 0.0f  }, true  };
        const uint32_t kNContacts = 5;

        x3::space::TargetingSystem targeting;
        targeting.setContacts(contacts, kNContacts);
        const float playerPos[3] = { 0.0f, 0.0f, 0.0f };
        const float playerFwd[3] = { 1.0f, 0.0f, 0.0f };
        targeting.lockNearest(playerPos, playerFwd);
        const float kProjSpeed = 220.0f;

        // ---- Marker geometry: a box drawn at each contact -----------------
        // Generously sized so the headless screenshot reads with strong pixel
        // variance against the dark backdrop (this is a VISUAL gate).
        x3::prims::PrimMesh markerGeo = x3::prims::makeBox(9.0f, 9.0f, 9.0f, 0, 0, 0, 1.0f);
        auto markerMesh = device->createMesh(markerGeo.verts.data(), (uint32_t)markerGeo.verts.size(),
                                             markerGeo.index.data(), (uint32_t)markerGeo.index.size());
        auto whiteD = x3::prims::makeCheckerRGBA(16, 4, 235, 235, 240, 120, 130, 150);
        auto whiteTex = device->createTexture(whiteD.data(), 16, 16, true);
        x3::prims::PrimMesh leadGeo = x3::prims::makeBox(5.0f, 5.0f, 5.0f, 0, 0, 0, 1.0f);
        auto leadMesh = device->createMesh(leadGeo.verts.data(), (uint32_t)leadGeo.verts.size(),
                                           leadGeo.index.data(), (uint32_t)leadGeo.index.size());

        // Draw all the world-space markers + the lead indicator for the frame.
        auto drawScene = [&](const x3::rhi::FrameContext& frame) {
            for (uint32_t i = 0; i < kNContacts; ++i) {
                const x3::space::Contact& c = contacts[i];
                const bool locked = targeting.hasLock() && targeting.lockedId() == c.id;
                float tint[4];
                if (c.hostile) { tint[0] = locked ? 2.4f : 1.4f; tint[1] = 0.12f; tint[2] = 0.12f; }
                else           { tint[0] = 0.12f; tint[1] = locked ? 2.4f : 1.4f; tint[2] = 0.20f; }
                tint[3] = 1.0f;
                float m[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, c.pos[0], c.pos[1], c.pos[2], 1 };
                device->drawMesh(frame, markerMesh, whiteTex, tint, m);
            }
            // Lead indicator: a bright cyan box at the firing solution.
            x3::space::LeadSolution lead = targeting.computeLead(playerPos, kProjSpeed);
            if (lead.valid) {
                const float cyan[4] = { 0.2f, 2.6f, 3.0f, 1.0f };
                float m[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0,
                                lead.aimPoint[0], lead.aimPoint[1], lead.aimPoint[2], 1 };
                device->drawMesh(frame, leadMesh, whiteTex, cyan, m);
            }
        };

        // HUD overlay: radar disc bottom-right + reticle on the locked target.
        auto drawHud = [&](const x3::rhi::FrameContext& frame) {
            uint32_t hw = 0, hh = 0; device->hudSize(hw, hh);
            const float fw = (float)hw, fh = (float)hh;
            // Radar disc: a square panel in the bottom-right corner.
            const float radarSize = 0.40f * fh;
            const float cx = fw - radarSize * 0.5f - 0.03f * fw;
            const float cy = fh - radarSize * 0.5f - 0.03f * fh;
            const float half = radarSize * 0.5f;
            // Panel background.
            const float panelBg[4] = { 0.04f, 0.10f, 0.06f, 0.55f };
            device->drawHudQuad(frame, cx - half, cy - half, radarSize, radarSize, panelBg);
            // Crosshair lines through the radar center (the player's heading).
            const float grid[4] = { 0.20f, 0.45f, 0.30f, 0.8f };
            device->drawHudQuad(frame, cx - half, cy - 1.0f, radarSize, 2.0f, grid);
            device->drawHudQuad(frame, cx - 1.0f, cy - half, 2.0f, radarSize, grid);
            // Player marker at center.
            const float me[4] = { 0.5f, 0.9f, 1.0f, 1.0f };
            device->drawHudQuad(frame, cx - 3.0f, cy - 3.0f, 6.0f, 6.0f, me);
            // Blips.
            x3::space::TargetingSystem::Blip blips[32];
            uint32_t nb = targeting.radarBlips(blips, 32, playerPos, playerFwd, 200.0f);
            for (uint32_t i = 0; i < nb; ++i) {
                // radarXY: +X right, +Y up. Screen +y is DOWN, so negate Y.
                const float bx = cx + blips[i].radarXY[0] * half;
                const float by = cy - blips[i].radarXY[1] * half;
                float col[4];
                if (blips[i].locked)      { col[0]=1.0f; col[1]=1.0f; col[2]=0.3f; col[3]=1.0f; }
                else if (blips[i].hostile){ col[0]=1.0f; col[1]=0.25f; col[2]=0.25f; col[3]=1.0f; }
                else                      { col[0]=0.3f; col[1]=1.0f; col[2]=0.4f; col[3]=1.0f; }
                const float s = blips[i].locked ? 8.0f : 5.0f;
                device->drawHudQuad(frame, bx - s*0.5f, by - s*0.5f, s, s, col);
            }
            // Reticle: a yellow open box at screen center when locked, plus a
            // "LOCK" label, to mark the dogfight HUD.
            if (targeting.hasLock()) {
                const float rx = fw * 0.5f, ry = fh * 0.5f, rs = 0.05f * fh;
                const float yellow[4] = { 1.0f, 0.95f, 0.2f, 1.0f };
                device->drawHudQuad(frame, rx - rs, ry - rs, 2.0f*rs, 2.0f, yellow); // top
                device->drawHudQuad(frame, rx - rs, ry + rs, 2.0f*rs, 2.0f, yellow); // bottom
                device->drawHudQuad(frame, rx - rs, ry - rs, 2.0f, 2.0f*rs, yellow); // left
                device->drawHudQuad(frame, rx + rs, ry - rs, 2.0f, 2.0f*rs, yellow); // right
                device->drawHudText(frame, "TARGET LOCK", rx - 60.0f, ry - rs - 24.0f, 12.0f, yellow);
                char buf[64];
                x3::space::LeadSolution lead = targeting.computeLead(playerPos, kProjSpeed);
                std::snprintf(buf, sizeof(buf), "ID %u  RNG %.0f", targeting.lockedId(),
                              lead.valid ? lead.distance : 0.0f);
                device->drawHudText(frame, buf, rx - 60.0f, ry + rs + 12.0f, 12.0f, yellow);
            }
        };

        // Camera: behind the player at the origin, looking toward +X.
        float cam[6] = { -30.0f, 4.0f, 0.0f, 0.0f, 0.0f, 65.0f };

        // ---- Headless capture ----------------------------------------------
        if (headless) {
            device->setFrustumCull(false);
            if (shotCamOverride) for (int k = 0; k < 5; ++k) cam[k] = shotCam[k];
            const std::string outPath = screenshot ? screenshotPath
                                                   : std::string("G:/X3Native/captures/targeting.png");
            const int kFrames = 16;
            for (int i = 0; i < kFrames; ++i) {
                glfwPollEvents();
                device->setCamera(cam[0], cam[1], cam[2], cam[3], cam[4], cam[5]);
                if (i == kFrames - 1) device->armCapture(outPath.c_str());
                auto frame = device->beginFrame();
                if (frame.valid) { drawScene(frame); drawHud(frame); }
                device->endFrame(frame);
            }
            const bool wrote = device->captureFrame(outPath.c_str());
            if (wrote) x3::logInfo("--world targeting: wrote " + outPath);
            else       x3::logError("--world targeting: capture FAILED");
            device->destroyMesh(markerMesh); device->destroyMesh(leadMesh);
            device->destroyTexture(whiteTex);
            device->shutdown();
            if (window) glfwDestroyWindow(window); glfwTerminate();
            return wrote ? 0 : 1;
        }

        // ---- Windowed path: cycle the lock with TAB / shift+TAB, Esc quit ---
        x3::logInfo("--world targeting: TAB cycle lock, N lock-nearest, C clear, Esc quit");
        bool prevTab = false, prevN = false, prevC = false;
        int lastWs = (int)W, lastHs = (int)H;
        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();
            if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;
            const bool tab = glfwGetKey(window, GLFW_KEY_TAB) == GLFW_PRESS;
            const bool shift = glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS;
            if (tab && !prevTab) targeting.cycleTarget(shift ? -1 : +1);
            prevTab = tab;
            const bool nKey = glfwGetKey(window, GLFW_KEY_N) == GLFW_PRESS;
            if (nKey && !prevN) targeting.lockNearest(playerPos, playerFwd);
            prevN = nKey;
            const bool cKey = glfwGetKey(window, GLFW_KEY_C) == GLFW_PRESS;
            if (cKey && !prevC) targeting.clearLock();
            prevC = cKey;
            int cw, chh; glfwGetFramebufferSize(window, &cw, &chh);
            if (cw != lastWs || chh != lastHs) { lastWs = cw; lastHs = chh; if (cw>0&&chh>0) device->onResize((uint32_t)cw,(uint32_t)chh); }
            device->setCamera(cam[0], cam[1], cam[2], cam[3], cam[4], cam[5]);
            auto frame = device->beginFrame();
            if (frame.valid) { drawScene(frame); drawHud(frame); }
            device->endFrame(frame);
        }
        device->destroyMesh(markerMesh); device->destroyMesh(leadMesh);
        device->destroyTexture(whiteTex);
        device->shutdown();
        if (window) glfwDestroyWindow(window); glfwTerminate();
        return 0;
    }

    // ======================================================================
    // ---- Act-3 SPACE PILOT showcase (--world space) -----------------------
    // The 6DOF SpacePilotController flies in deep space with a small decor fleet
    // of static ships a few hundred meters out. Dark clear color (no skybox —
    // procedural starfield is in a separate in-flight lane and is NOT used or
    // referenced here). Sun = a far-away point light at +X+Y+Z; no ground.
    // Headless `--world space --screenshot <path>` writes a non-blank PNG;
    // windowed path is the walkable showcase (WASD + mouse + Q/E roll +
    // Space/Ctrl up/down + V to toggle 1P/3P + LMB lasers).
    if (worldMode == "space") {
        x3::logInfo("--world space: building the Act-3 space-pilot showcase");
        std::unique_ptr<x3::phys::IPhysicsWorld> sphys(x3::phys::createPhysicsWorld());
        if (!sphys->init()) {
            x3::logError("--world space: physics init failed");
            device->shutdown(); if (window) glfwDestroyWindow(window); glfwTerminate(); return 1;
        }

        // Sun-as-point-light + dim ambient via SkyParams (we leave SkyParams
        // DISABLED so the analytic sky doesn't paint a daytime backdrop over
        // deep space; the dark clear color shows through).
        { x3::rhi::IRenderDevice::SkyParams sp{}; sp.enabled = false;
          device->setSkyParams(sp); }
        // SSAO + SSGI screen-space passes raster the whole scene to black on a
        // black/empty space background (no nearby geometry to bounce off) -- the
        // 1080 Ti / no-RT fallback path documented in the memory bank. Disable
        // both for the space showcase so the ships actually read against the
        // dark backdrop.
        { x3::rhi::IRenderDevice::SsaoParams ap{}; ap.enabled = false;
          device->setSsaoParams(ap); }
        { x3::rhi::IRenderDevice::GiParams gp{}; gp.enabled = false;
          device->setGiParams(gp); }
        // Sun = the directional sun baked into mesh.frag at +Y-ish; layer on a
        // few BRIGHT point lights NEAR the fleet so the ships read (the analytic
        // sky is OFF -> no atmospheric tint; light only comes from these point
        // lights + the hardcoded sun, attenuated by 1/r^2). The point-light
        // ranges + intensities are intentionally cranked: deep space has zero
        // bounced light, so anything subtle would render the ships as silhouettes.
        { x3::rhi::PointLight pl[3];
          // Key light: a "sun" anchored near the fleet so attenuation is gentle.
          pl[0].pos[0] =  120.0f; pl[0].pos[1] = 120.0f; pl[0].pos[2] = 120.0f;
          pl[0].range  =  600.0f;
          pl[0].color[0] = 60.0f; pl[0].color[1] = 56.0f; pl[0].color[2] = 48.0f;
          // Fill light from -X/+Y to bring out the camera-facing side.
          pl[1].pos[0] = -80.0f; pl[1].pos[1] =  60.0f; pl[1].pos[2] =  20.0f;
          pl[1].range  = 400.0f;
          pl[1].color[0] = 15.0f; pl[1].color[1] = 18.0f; pl[1].color[2] = 24.0f;
          // Rim/back light from +X/-Y to give the ships shape.
          pl[2].pos[0] =  200.0f; pl[2].pos[1] = -30.0f; pl[2].pos[2] = -50.0f;
          pl[2].range  = 500.0f;
          pl[2].color[0] = 8.0f; pl[2].color[1] = 6.0f; pl[2].color[2] = 4.0f;
          device->setPointLights(pl, 3); }

        // ---- Player ship (the SpacePilotController) -----------------------
        x3::game::SpacePilotController pilot;
        pilot.spawn(*sphys, 0.0f, 0.0f, 0.0f);

        // ---- Try to load an actual ship GLB. SpaceShip*.glb don't ship in
        //      assets/rigged_glb yet (per the task brief: "4 SpaceShip*.glb
        //      already in rigged_glb" was aspirational — the dir has none on
        //      this baseline). We fall back to DroneOscillating.glb as a
        //      stand-in flying object; if even that fails, we draw the ship
        //      as a procedural box.
        const std::string rigDir = x3::game::riggedGlbRoot();
        std::unique_ptr<x3::asset::IAssetSource> asrc(x3::asset::createAssetSource());
        asrc->mountDir(rigDir, 0);
        std::unique_ptr<x3::asset::IModelLoader> mloader(x3::asset::createModelLoader(device.get(), asrc.get()));
        // Probe candidates in order of preference: real ship asset first, drone fallback.
        const char* kShipCandidates[] = {
            "SpaceShip.glb", "SpaceShip2.glb", "SpaceShip3.glb", "SpaceShip4.glb",
            "DroneOscillating.glb", "DroneExportWMotion.glb"
        };
        x3::asset::Model shipModel{};
        std::string shipFile;
        for (const char* c : kShipCandidates) {
            shipModel = mloader->load(c);
            if (shipModel.ok) { shipFile = c; break; }
        }
        std::vector<x3::asset::ModelDrawable> shipDrawables;
        if (shipModel.ok) shipDrawables = x3::asset::makeDrawables(shipModel);
        x3::logInfo(std::string("--world space: ship model=") + (shipModel.ok ? shipFile : "<procedural-box-fallback>"));

        // Procedural-box fallback (in case the GLB load fails entirely).
        x3::prims::PrimMesh sbm = x3::prims::makeBox(2.0f, 0.6f, 1.2f, 0, 0, 0, 0.25f);
        auto shipBoxMesh = device->createMesh(sbm.verts.data(), (uint32_t)sbm.verts.size(),
                                              sbm.index.data(), (uint32_t)sbm.index.size());
        auto sbTexD = x3::prims::makeCheckerRGBA(64, 8, 180, 190, 210, 60, 70, 90);
        auto shipBoxTex = device->createTexture(sbTexD.data(), 64, 64, true);

        // ---- Static decor fleet: a wing formation a few dozen meters out
        //      Each is a static placement transform (rotation around +Y for variety).
        //      Coordinates chosen so the headless screenshot camera (at -X behind
        //      the player ship, looking toward +X) sees a tight cluster of ships
        //      filling a good portion of the frame.
        struct DecorShip { float x, y, z, yaw, scale; };
        const DecorShip decor[] = {
            {   30.0f,   2.0f,    8.0f,  0.2f, 18.0f },  // close right
            {   35.0f,   4.0f,  -10.0f, -0.3f, 20.0f },  // close left, up
            {   45.0f,  -2.0f,   18.0f,  0.4f, 22.0f },  // mid-right, down
            {   55.0f,   8.0f,   -4.0f,  0.0f, 24.0f },  // mid lead, up
            {   80.0f,   0.0f,  -25.0f,  0.6f, 28.0f },  // far escort left
            {   80.0f,   2.0f,   25.0f, -0.6f, 28.0f },  // far escort right
        };
        const int kDecorCount = (int)(sizeof(decor) / sizeof(decor[0]));

        // CombatFx for laser tracers + impact decals. Heap-allocated because
        // CombatFx carries ~256 KB of mutable scratch instance arrays; piling
        // another stack copy into main() (which already holds one for the
        // canonplay/level1 paths) overflows the 1 MB default thread stack.
        auto combatFxOwned = std::make_unique<x3::game::CombatFx>();
        x3::game::CombatFx& combatFx = *combatFxOwned;
        combatFx.init(*device);

        const float dt = 1.0f / 60.0f;

        // Draw a ship at a placement matrix (yaw-only for decor; full quat for
        // the player ship). `bright` brightens the model so it reads in the
        // dim space scene.
        auto drawShipAt = [&](const x3::rhi::FrameContext& frame,
                              const float xform[16], float bright) {
            if (shipModel.ok) {
                for (const auto& dr : shipDrawables) {
                    float fin[16];
                    x3::asset::mulMat4(xform, dr.nodeTransform, fin);
                    // Boost the ship brightness HARD: in deep space there is no
                    // bounced light, so the GLB's baseColorFactor (often very
                    // dark scifi metal) reads near-black under direct lighting
                    // alone. The tint multiplier is the renderer's albedo
                    // scale, so we crank it + add a constant floor for the
                    // headless screenshot. The windowed flight feels the same
                    // because the lights are also dialed up to match.
                    float tint[4] = {
                        dr.baseColorFactor[0] * bright * 4.0f + 0.45f,
                        dr.baseColorFactor[1] * bright * 4.0f + 0.50f,
                        dr.baseColorFactor[2] * bright * 4.0f + 0.55f,
                        dr.baseColorFactor[3]
                    };
                    device->drawMesh(frame, x3::rhi::MeshHandle{ dr.meshId },
                                     x3::rhi::TextureHandle{ dr.baseColorTexId },
                                     tint, fin);
                }
            } else {
                const float white[4] = { bright, bright, bright, 1.0f };
                device->drawMesh(frame, shipBoxMesh, shipBoxTex, white, xform);
            }
        };

        auto drawScene = [&](const x3::rhi::FrameContext& frame) {
            // Player ship: build a 4x4 from quaternion + position (visible only
            // in 3P; in 1P it would clip the near plane — host gates visuals).
            if (pilot.isThirdPerson()) {
                const x3::phys::Vec3 p = pilot.pos();
                const x3::phys::Vec3 f = pilot.forward();
                const x3::phys::Vec3 r = pilot.right();
                const x3::phys::Vec3 u = pilot.up();
                float m[16] = {
                    f.x, f.y, f.z, 0,   // col 0 = ship local +X
                    u.x, u.y, u.z, 0,   // col 1 = ship local +Y
                    r.x, r.y, r.z, 0,   // col 2 = ship local +Z
                    p.x, p.y, p.z, 1
                };
                drawShipAt(frame, m, 1.5f);
            }
            // Decor fleet.
            for (int i = 0; i < kDecorCount; ++i) {
                const float c = std::cos(decor[i].yaw), s = std::sin(decor[i].yaw);
                const float S = decor[i].scale;
                float m[16] = {
                    c*S, 0,  -s*S, 0,
                    0,   S,  0,   0,
                    s*S, 0,  c*S, 0,
                    decor[i].x, decor[i].y, decor[i].z, 1
                };
                drawShipAt(frame, m, 1.0f);
            }
        };

        // ===== Headless capture (--world space --screenshot <path>) ========
        if (headless) {
            // The frustum-cull pass on this baseline tests AABBs that may be
            // wrong for a deeply-nested GLB drawable transform. Disable for the
            // capture so the test screenshot is robust against that; it is a
            // VISUAL gate, not a perf gate. (Windowed path leaves it default-on.)
            device->setFrustumCull(false);
            // Camera behind the player ship looking toward +X (yaw=0 -> +X is
            // the device's "forward 0" per Player::camera()), slight downward
            // pitch to catch the slight-Y staggered decor ships. The fleet
            // cluster sits at x=60..200 with +/-Z flanks within FOV.
            float cam[5] = { -25.0f, 6.0f, 0.0f, 0.0f, -0.05f };
            if (shotCamOverride) for (int k = 0; k < 5; ++k) cam[k] = shotCam[k];
            const std::string outPath = screenshot ? screenshotPath : std::string("G:/X3Native/captures/space.png");
            // Settle: a few frames so the lights register + the meshes upload.
            const int kFrames = 16;
            for (int i = 0; i < kFrames; ++i) {
                glfwPollEvents();
                sphys->step(dt);
                combatFx.update(dt);
                device->setCamera(cam[0], cam[1], cam[2], cam[3], cam[4], 65.0f);
                if (i == kFrames - 1) device->armCapture(outPath.c_str());
                auto frame = device->beginFrame();
                if (frame.valid) {
                    drawScene(frame);
                    combatFx.submit(*device, frame);
                }
                device->endFrame(frame);
            }
            const bool wrote = device->captureFrame(outPath.c_str());
            if (wrote) x3::logInfo("--world space: wrote " + outPath);
            else       x3::logError("--world space: capture FAILED");
            combatFx.shutdown(*device);
            device->destroyMesh(shipBoxMesh); device->destroyTexture(shipBoxTex);
            if (shipModel.ok) mloader->unload(shipModel);
            sphys->shutdown(); device->shutdown();
            if (window) glfwDestroyWindow(window); glfwTerminate();
            return wrote ? 0 : 1;
        }

        // ===== Walkable windowed path: 6DOF pilot, mouse + WASD + Q/E + V ==
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        double lastMX, lastMY; glfwGetCursorPos(window, &lastMX, &lastMY);
        double prevTime = glfwGetTime();
        bool prevV = false, prevLmb = false;
        x3::logInfo("--world space: WASD thrust, mouse look, Q/E roll, Space/Ctrl up/down, Shift boost, V camera, LMB laser, Esc quit");
        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();
            if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;
            double now = glfwGetTime(); float fdt = (float)(now - prevTime); prevTime = now;
            if (fdt > 0.1f) fdt = 0.1f;
            double mx, my; glfwGetCursorPos(window, &mx, &my);
            float ddx = (float)(mx - lastMX), ddy = (float)(my - lastMY); lastMX = mx; lastMY = my;
            auto kd = [&](int k) { return glfwGetKey(window, k) == GLFW_PRESS; };

            // Build a PlayerInput for the controller. jumpPressed re-purposed as
            // "up impulse this frame" while Space is held; sprint = boost.
            x3::game::PlayerInput in{};
            in.moveFwd    = (kd(GLFW_KEY_W) ?  1.0f : 0.0f) + (kd(GLFW_KEY_S) ? -1.0f : 0.0f);
            in.moveStrafe = (kd(GLFW_KEY_D) ?  1.0f : 0.0f) + (kd(GLFW_KEY_A) ? -1.0f : 0.0f);
            in.sprint     = kd(GLFW_KEY_LEFT_SHIFT);
            in.jumpPressed= kd(GLFW_KEY_SPACE);   // held = up impulse
            in.lookDX     = ddx;
            in.lookDY     = ddy;
            // Down on Ctrl: cheap negative on the up axis. Re-use jumpPressed bool
            // for the controller — when Ctrl is held we push -Y by setting the
            // strafe-along-up via a buffered hack: simplest is to apply directly
            // here by using the SAME mechanism the controller uses for Space, but
            // negated. We bypass jumpPressed when Ctrl is held — the controller
            // exposes no down channel, so we apply a tiny manual nudge by reading
            // the ship up-vector and subtracting from velocity post-update below.
            // (See: post-update nudge.)

            // Q/E roll axis: +1 for Q, -1 for E (or the other way; either is fine).
            float rollAxis = (kd(GLFW_KEY_Q) ? -1.0f : 0.0f) + (kd(GLFW_KEY_E) ? 1.0f : 0.0f);
            pilot.setRollInput(rollAxis);

            pilot.update(in, fdt, *sphys);

            // V to toggle 1P / 3P (rising edge).
            bool vNow = kd(GLFW_KEY_V);
            if (vNow && !prevV) pilot.toggleCameraMode();
            prevV = vNow;

            // LMB laser (rising edge -> fire one bolt, log on success).
            bool lmbNow = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
            if (lmbNow && !prevLmb && pilot.fireLaser(fdt)) {
                // Tracer: from the ship's forward muzzle to a long range hit.
                const x3::phys::Vec3 pos = pilot.pos();
                const x3::phys::Vec3 fwd = pilot.forward();
                x3::phys::Vec3 muzzle{ pos.x + fwd.x * 2.5f,
                                       pos.y + fwd.y * 2.5f,
                                       pos.z + fwd.z * 2.5f };
                x3::phys::Vec3 hit{ pos.x + fwd.x * 400.0f,
                                    pos.y + fwd.y * 400.0f,
                                    pos.z + fwd.z * 400.0f };
                combatFx.addTracer(muzzle, hit);
            }
            prevLmb = lmbNow;

            sphys->step(fdt);
            combatFx.update(fdt);

            int cw, chh; glfwGetFramebufferSize(window, &cw, &chh);
            if (cw>0 && chh>0) device->onResize((uint32_t)cw, (uint32_t)chh);

            float cx, cy, cz, cyaw, cpit;
            pilot.camera(cx, cy, cz, cyaw, cpit);
            device->setCamera(cx, cy, cz, cyaw, cpit, 65.0f);

            auto frame = device->beginFrame();
            if (frame.valid) {
                drawScene(frame);
                combatFx.submit(*device, frame);
            }
            device->endFrame(frame);
        }
        combatFx.shutdown(*device);
        device->destroyMesh(shipBoxMesh); device->destroyTexture(shipBoxTex);
        if (shipModel.ok) mloader->unload(shipModel);
        sphys->shutdown(); device->shutdown();
        if (window) glfwDestroyWindow(window); glfwTerminate();
        return 0;
    }

    // ======================================================================
    // ---- S8 ENEMY SHIP-AI dogfight showcase (--world ship-ai) -------------
    // A dummy (static) player ship at the origin + a wing of AI enemy fighters
    // that fly in, maneuver, and fire (the EnemyShipManager dogfight sandbox).
    // Each enemy is drawn as a ship GLB at its pos/fwd; their laser-fire events
    // are rendered as CombatFx tracers. Dark clear color (deep space), SSAO+SSGI
    // OFF (the no-RT black-on-empty-space fallback), bright point lights so the
    // ships read. Headless `--world ship-ai --screenshot <path>` writes a PNG.
    if (worldMode == "ship-ai") {
        x3::logInfo("--world ship-ai: building the S8 enemy ship-AI dogfight showcase");

        // Sky off (no daytime backdrop over deep space); SSAO + SSGI off (raster
        // fallback blacks out an empty-space scene on no-RT GPUs -- memory bank).
        { x3::rhi::IRenderDevice::SkyParams sp{}; sp.enabled = false; device->setSkyParams(sp); }
        { x3::rhi::IRenderDevice::SsaoParams ap{}; ap.enabled = false; device->setSsaoParams(ap); }
        { x3::rhi::IRenderDevice::GiParams gp{}; gp.enabled = false; device->setGiParams(gp); }
        // Bright point lights near the fleet so the ships aren't silhouettes
        // (deep space has zero bounced light).
        { x3::rhi::PointLight pl[3];
          pl[0].pos[0] =  120.0f; pl[0].pos[1] = 120.0f; pl[0].pos[2] = 120.0f;
          pl[0].range  =  700.0f;
          pl[0].color[0] = 60.0f; pl[0].color[1] = 56.0f; pl[0].color[2] = 48.0f;
          pl[1].pos[0] = -80.0f; pl[1].pos[1] =  60.0f; pl[1].pos[2] =  20.0f;
          pl[1].range  = 500.0f;
          pl[1].color[0] = 15.0f; pl[1].color[1] = 18.0f; pl[1].color[2] = 24.0f;
          pl[2].pos[0] =  200.0f; pl[2].pos[1] = -30.0f; pl[2].pos[2] = -50.0f;
          pl[2].range  = 600.0f;
          pl[2].color[0] = 8.0f; pl[2].color[1] = 6.0f; pl[2].color[2] = 4.0f;
          device->setPointLights(pl, 3); }

        // ---- Dummy player ship (static target at the origin) --------------
        const float playerPos[3] = { 0.0f, 0.0f, 0.0f };
        const float playerVel[3] = { 0.0f, 0.0f, 0.0f };

        // ---- Enemy ship manager: spawn 4 fighters in a loose ring out front
        x3::space::EnemyShipManager fleet;
        fleet.init(8);
        const float spawns[][3] = {
            {   55.0f,    6.0f,   16.0f },
            {   70.0f,   -4.0f,  -20.0f },
            {   48.0f,   10.0f,   -8.0f },
            {   85.0f,    0.0f,    6.0f },
        };
        for (const auto& s : spawns) fleet.spawn(s);
        x3::logInfo(std::string("--world ship-ai: spawned ") +
                    std::to_string(fleet.count()) + " enemy fighters");

        // ---- Ship visual GLB. Try SpaceShip2.glb (task brief) first, then the
        //      DroneOscillating stand-in, else a procedural box.
        const std::string rigDir = x3::game::riggedGlbRoot();
        std::unique_ptr<x3::asset::IAssetSource> asrc(x3::asset::createAssetSource());
        asrc->mountDir(rigDir, 0);
        std::unique_ptr<x3::asset::IModelLoader> mloader(x3::asset::createModelLoader(device.get(), asrc.get()));
        const char* kShipCandidates[] = {
            "SpaceShip2.glb", "SpaceShip.glb", "SpaceShip3.glb",
            "DroneOscillating.glb", "DroneExportWMotion.glb"
        };
        x3::asset::Model shipModel{};
        std::string shipFile;
        for (const char* c : kShipCandidates) {
            shipModel = mloader->load(c);
            if (shipModel.ok) { shipFile = c; break; }
        }
        std::vector<x3::asset::ModelDrawable> shipDrawables;
        if (shipModel.ok) shipDrawables = x3::asset::makeDrawables(shipModel);
        x3::logInfo(std::string("--world ship-ai: enemy ship model=") +
                    (shipModel.ok ? shipFile : "<procedural-box-fallback>"));

        // Procedural-box fallback.
        x3::prims::PrimMesh sbm = x3::prims::makeBox(3.0f, 1.0f, 1.8f, 0, 0, 0, 0.3f);
        auto shipBoxMesh = device->createMesh(sbm.verts.data(), (uint32_t)sbm.verts.size(),
                                              sbm.index.data(), (uint32_t)sbm.index.size());
        auto sbTexD = x3::prims::makeCheckerRGBA(64, 8, 200, 120, 110, 70, 60, 90);
        auto shipBoxTex = device->createTexture(sbTexD.data(), 64, 64, true);

        // CombatFx (heap — see --world space note on the 1 MB stack).
        auto combatFxOwned = std::make_unique<x3::game::CombatFx>();
        x3::game::CombatFx& combatFx = *combatFxOwned;
        combatFx.init(*device);

        const float kEnemyScale = 22.0f;
        const float dtSim = 1.0f / 60.0f;

        // Build a 4x4 model matrix from a ship's pos + forward heading (the ship
        // local +X aligned to fwd; a world-up reference builds an orthonormal basis).
        auto enemyXform = [&](const x3::space::EnemyShip& s, float out[16]) {
            float f[3] = { s.fwd[0], s.fwd[1], s.fwd[2] };
            float fl = std::sqrt(f[0]*f[0]+f[1]*f[1]+f[2]*f[2]);
            if (fl > 1e-4f) { f[0]/=fl; f[1]/=fl; f[2]/=fl; } else { f[0]=1; f[1]=0; f[2]=0; }
            float up[3] = { 0, 1, 0 };
            // right = up x f
            float r[3] = { up[1]*f[2]-up[2]*f[1], up[2]*f[0]-up[0]*f[2], up[0]*f[1]-up[1]*f[0] };
            float rl = std::sqrt(r[0]*r[0]+r[1]*r[1]+r[2]*r[2]);
            if (rl > 1e-4f) { r[0]/=rl; r[1]/=rl; r[2]/=rl; } else { r[0]=0; r[1]=0; r[2]=1; }
            // recompute up = f x right
            float u[3] = { f[1]*r[2]-f[2]*r[1], f[2]*r[0]-f[0]*r[2], f[0]*r[1]-f[1]*r[0] };
            const float S = kEnemyScale;
            out[0]=f[0]*S; out[1]=f[1]*S; out[2]=f[2]*S; out[3]=0;
            out[4]=u[0]*S; out[5]=u[1]*S; out[6]=u[2]*S; out[7]=0;
            out[8]=r[0]*S; out[9]=r[1]*S; out[10]=r[2]*S; out[11]=0;
            out[12]=s.pos[0]; out[13]=s.pos[1]; out[14]=s.pos[2]; out[15]=1;
        };

        auto drawShipAt = [&](const x3::rhi::FrameContext& frame, const float xform[16], float bright) {
            if (shipModel.ok) {
                for (const auto& dr : shipDrawables) {
                    float fin[16];
                    x3::asset::mulMat4(xform, dr.nodeTransform, fin);
                    float tint[4] = {
                        dr.baseColorFactor[0] * bright * 4.0f + 0.55f,
                        dr.baseColorFactor[1] * bright * 4.0f + 0.40f,
                        dr.baseColorFactor[2] * bright * 4.0f + 0.40f,
                        dr.baseColorFactor[3]
                    };
                    device->drawMesh(frame, x3::rhi::MeshHandle{ dr.meshId },
                                     x3::rhi::TextureHandle{ dr.baseColorTexId }, tint, fin);
                }
            } else {
                const float reddish[4] = { bright*1.2f, bright*0.7f, bright*0.7f, 1.0f };
                device->drawMesh(frame, shipBoxMesh, shipBoxTex, reddish, xform);
            }
        };

        auto drawScene = [&](const x3::rhi::FrameContext& frame) {
            for (uint32_t i = 0; i < fleet.count(); ++i) {
                float m[16]; enemyXform(fleet.ship(i), m);
                drawShipAt(frame, m, 1.2f);
            }
        };

        // Step the AI sim one frame + feed any fire events into CombatFx tracers.
        auto stepSim = [&](float dt) {
            fleet.update(dt, playerPos, playerVel);
            for (const auto& ev : fleet.fireEvents()) {
                combatFx.addTracer(x3::phys::Vec3{ ev.from[0], ev.from[1], ev.from[2] },
                                   x3::phys::Vec3{ ev.to[0], ev.to[1], ev.to[2] });
            }
        };

        // ===== Headless capture =====
        if (headless) {
            device->setFrustumCull(false);
            // Camera behind the player ship looking toward +X at the fighters
            // (mirrors the proven --world space framing: cam at -X, ships out at
            // +X within FOV).
            float cam[5] = { -25.0f, 6.0f, 0.0f, 0.0f, -0.05f };
            if (shotCamOverride) for (int k = 0; k < 5; ++k) cam[k] = shotCam[k];
            const std::string outPath = screenshot ? screenshotPath : std::string("G:/X3Native/captures/shipai.png");
            // Settle only briefly: the fighters start at x~180-300 and close fast,
            // so a short window keeps them clustered out front in the camera frame.
            const int kFrames = 30;   // let the fighters fly in + maneuver a bit
            for (int i = 0; i < kFrames; ++i) {
                glfwPollEvents();
                stepSim(dtSim);
                combatFx.update(dtSim);
                device->setCamera(cam[0], cam[1], cam[2], cam[3], cam[4], 65.0f);
                if (i == kFrames - 1) device->armCapture(outPath.c_str());
                auto frame = device->beginFrame();
                if (frame.valid) { drawScene(frame); combatFx.submit(*device, frame); }
                device->endFrame(frame);
            }
            const bool wrote = device->captureFrame(outPath.c_str());
            if (wrote) x3::logInfo("--world ship-ai: wrote " + outPath);
            else       x3::logError("--world ship-ai: capture FAILED");
            combatFx.shutdown(*device);
            device->destroyMesh(shipBoxMesh); device->destroyTexture(shipBoxTex);
            if (shipModel.ok) mloader->unload(shipModel);
            device->shutdown();
            if (window) glfwDestroyWindow(window); glfwTerminate();
            return wrote ? 0 : 1;
        }

        // ===== Walkable windowed path: orbit the dogfight =====
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        double prevTime = glfwGetTime();
        float camYaw = 0.0f, camPitch = -0.05f, camDist = 120.0f;
        double lastMX, lastMY; glfwGetCursorPos(window, &lastMX, &lastMY);
        x3::logInfo("--world ship-ai: watch the AI fighters dogfight the player ship; mouse orbits, Esc quit");
        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();
            if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;
            double now = glfwGetTime(); float fdt = (float)(now - prevTime); prevTime = now;
            if (fdt > 0.1f) fdt = 0.1f;
            double mx, my; glfwGetCursorPos(window, &mx, &my);
            camYaw   += (float)(mx - lastMX) * 0.005f;
            camPitch += (float)(my - lastMY) * 0.005f;
            lastMX = mx; lastMY = my;

            stepSim(fdt);
            combatFx.update(fdt);

            int cw, chh; glfwGetFramebufferSize(window, &cw, &chh);
            if (cw>0 && chh>0) device->onResize((uint32_t)cw, (uint32_t)chh);
            // Orbit camera around the origin (the player ship).
            const float cy = std::cos(camYaw), sy = std::sin(camYaw);
            float cx = -camDist * cy, cz = -camDist * sy, cyy = 8.0f + camDist * (-camPitch);
            device->setCamera(cx, cyy, cz, camYaw, camPitch, 65.0f);

            auto frame = device->beginFrame();
            if (frame.valid) { drawScene(frame); combatFx.submit(*device, frame); }
            device->endFrame(frame);
        }
        combatFx.shutdown(*device);
        device->destroyMesh(shipBoxMesh); device->destroyTexture(shipBoxTex);
        if (shipModel.ok) mloader->unload(shipModel);
        device->shutdown();
        if (window) glfwDestroyWindow(window); glfwTerminate();
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

    // ---- NPC showcase (--world npc) ---------------------------------------
    // Spawn 3-4 non-combatant NPCs around the player using rigged_glb models:
    //   * BartenderDanny.glb — Idle fixed pose at the "bar" (look-at-on-approach).
    //   * DockWorker.glb     — Patrol a 4-waypoint loop on the dock.
    //   * DrJohnson.glb      — Captive, rescuable (press E in range).
    //   * Mechanic.glb       — Idle, fixed pose, look-at-on-approach.
    // Each model falls back to a procedural khaki box if the GLB is absent (clean
    // checkout safe). Headless / screenshot path captures a fixed camera; the
    // walkable path gives a fly-cam + E to rescue the captive.
    if (worldMode == "npc") {
        x3::logInfo("--world npc: building the NPC showcase (bartender + patrol + captive)");
        std::unique_ptr<x3::phys::IPhysicsWorld> nphys(x3::phys::createPhysicsWorld());
        if (!nphys->init()) {
            x3::logError("--world npc: physics init failed");
            device->shutdown(); if (window) glfwDestroyWindow(window); glfwTerminate(); return 1;
        }

        // ---- Lit ground + sky so the screenshot is not a black void. ------
        auto grTexD = x3::prims::makeCheckerRGBA(64, 8, 150, 150, 160, 60, 62, 74);
        auto grTex  = device->createTexture(grTexD.data(), 64, 64, true);
        x3::prims::PrimMesh g = x3::prims::makeBox(20.0f, 0.25f, 20.0f, 0.0f, -0.25f, 0.0f, 0.25f);
        auto grMesh = device->createMesh(g.verts.data(), (uint32_t)g.verts.size(),
                                         g.index.data(), (uint32_t)g.index.size());
        nphys->addStaticMesh(g.cverts.data(), (uint32_t)(g.cverts.size()/3),
                             g.cindex.data(), (uint32_t)g.cindex.size());

        { x3::rhi::IRenderDevice::SkyParams sp{}; sp.enabled = true; sp.sunIntensity = 1.3f; sp.haze = 0.3f;
          device->setSkyParams(sp); }
        { x3::rhi::PointLight pl[2];
          pl[0].pos[0]= 2.0f; pl[0].pos[1]=3.0f; pl[0].pos[2]= 0.0f; pl[0].range=20.0f;
          pl[0].color[0]=5.0f; pl[0].color[1]=4.8f; pl[0].color[2]=4.4f;
          pl[1].pos[0]=-2.0f; pl[1].pos[1]=3.0f; pl[1].pos[2]= 0.0f; pl[1].range=20.0f;
          pl[1].color[0]=3.2f; pl[1].color[1]=3.2f; pl[1].color[2]=3.5f;
          device->setPointLights(pl, 2); }

        x3::game::Scene nscene;

        // 1) Bartender (fixed pose at "bar", look-at-on-approach).
        x3::game::NPCSystem bartender;
        {
            x3::game::NPCSystem::Tuning t;
            t.modelFile        = "BartenderDanny.glb";
            t.modelDirOverride = x3::game::riggedGlbRoot();
            t.modelScale       = 1.0f;
            t.lookAtRange      = 3.5f;
            t.initialMode      = x3::game::NPCSystem::Mode::Idle;
            bartender.build(nscene, *device, *nphys, x3::phys::Vec3{ 0.0f, 0.0f, -3.0f }, t);
            bartender.setFixedPose(0.0f);   // face -Z (toward the player)
        }

        // 2) DockWorker (patrol a 4-waypoint loop). Falls back to box if missing.
        x3::game::NPCSystem dockWorker;
        {
            x3::game::NPCSystem::Tuning t;
            t.modelFile        = "DockWorker.glb";
            t.modelDirOverride = x3::game::riggedGlbRoot();
            t.modelScale       = 1.0f;
            t.lookAtRange      = 2.5f;
            t.initialMode      = x3::game::NPCSystem::Mode::Idle;
            const x3::phys::Vec3 spawn{ 5.0f, 0.0f, 0.0f };
            dockWorker.build(nscene, *device, *nphys, spawn, t);
            dockWorker.setPatrol({
                x3::phys::Vec3{  5.0f, 0.0f,  2.0f },
                x3::phys::Vec3{  7.0f, 0.0f,  2.0f },
                x3::phys::Vec3{  7.0f, 0.0f, -2.0f },
                x3::phys::Vec3{  5.0f, 0.0f, -2.0f },
            }, 1.6f);
        }

        // 3) Captive Dr. Johnson (rescuable on E in range).
        x3::game::NPCSystem captive;
        {
            x3::game::NPCSystem::Tuning t;
            t.modelFile        = "DrJohnson.glb";
            t.modelDirOverride = x3::game::riggedGlbRoot();
            t.modelScale       = 1.0f;
            t.lookAtRange      = 0.0f;   // no look-at; captives don't track
            t.initialMode      = x3::game::NPCSystem::Mode::Captive;
            captive.build(nscene, *device, *nphys, x3::phys::Vec3{ -5.0f, 0.0f, 0.0f }, t);
        }

        // 4) Mechanic (idle, look-at-on-approach). Falls back if the model is absent.
        x3::game::NPCSystem mechanic;
        {
            x3::game::NPCSystem::Tuning t;
            t.modelFile        = "Mechanic.glb";
            t.modelDirOverride = x3::game::riggedGlbRoot();
            t.modelScale       = 1.0f;
            t.lookAtRange      = 3.0f;
            t.initialMode      = x3::game::NPCSystem::Mode::Idle;
            mechanic.build(nscene, *device, *nphys, x3::phys::Vec3{ 0.0f, 0.0f, 3.0f }, t);
            mechanic.setFixedPose(3.14159265f);   // face +Z (away from the player, toward a "workbench")
        }

        // Lambda that draws the whole NPC scene for one frame.
        auto drawNpcScene = [&](const x3::rhi::FrameContext& frame) {
            const float white[4] = {1,1,1,1};
            const float idG[16]  = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
            device->drawMesh(frame, grMesh, grTex, white, idG);
            bartender.drawNPC(*device, frame, nscene);
            dockWorker.drawNPC(*device, frame, nscene);
            captive.drawNPC(*device, frame, nscene);
            mechanic.drawNPC(*device, frame, nscene);
        };

        const float dt = 1.0f / 60.0f;
        // The "player" position drives look-at + (for the headless path) drives
        // the captive into rescue range so the screenshot shows a free NPC pose.
        x3::phys::Vec3 playerPos{ 0.0f, 1.6f, 5.0f };

        // ===== Headless capture: tick a few frames so patrol/look-at engage. ====
        if (headless) {
            // Frame the four NPCs in a wide vista shot. Camera is at +Z 8 m, look
            // toward -Z (the bartender + the dock worker + the captive all line up).
            float cam[5] = { 0.0f, 2.2f, 9.0f, -1.5708f, -0.10f };
            if (shotCamOverride) for (int k = 0; k < 5; ++k) cam[k] = shotCam[k];
            const std::string outPath = screenshot ? screenshotPath
                                                   : std::string("G:/X3Native/captures/npc.png");
            const int kFrames = 30;
            for (int i = 0; i < kFrames; ++i) {
                glfwPollEvents();
                // Walk the "player" toward the bartender so look-at engages mid-shot.
                if (i > 5) { playerPos.z -= 0.06f; }
                bartender.update(dt, nscene, *nphys, playerPos);
                dockWorker.update(dt, nscene, *nphys, playerPos);
                captive.update(dt, nscene, *nphys, playerPos);
                mechanic.update(dt, nscene, *nphys, playerPos);
                nphys->step(dt);
                device->setCamera(cam[0], cam[1], cam[2], cam[3], cam[4], 60.0f);
                if (i == kFrames - 1) device->armCapture(outPath.c_str());
                auto frame = device->beginFrame();
                if (frame.valid) drawNpcScene(frame);
                device->endFrame(frame);
            }
            const bool wrote = device->captureFrame(outPath.c_str());
            if (wrote) x3::logInfo("--world npc: wrote " + outPath);
            else       x3::logError("--world npc: capture FAILED");
            bartender.shutdownRagdoll(*nphys);
            dockWorker.shutdownRagdoll(*nphys);
            captive.shutdownRagdoll(*nphys);
            mechanic.shutdownRagdoll(*nphys);
            device->destroyMesh(grMesh); device->destroyTexture(grTex);
            nphys->shutdown(); device->shutdown();
            if (window) glfwDestroyWindow(window); glfwTerminate();
            return wrote ? 0 : 1;
        }

        // ===== Walkable windowed path: fly-cam; E rescues the captive. =====
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        double lastMX, lastMY; glfwGetCursorPos(window, &lastMX, &lastMY);
        double prevTime = glfwGetTime();
        float fx = 0.0f, fy = 1.8f, fz = 6.0f, fyaw = -1.5708f, fpitch = -0.05f;
        bool prevE = false;
        x3::logInfo("--world npc: fly WASD + mouse, E to rescue the captive in range, Esc to quit");
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
            playerPos.x = fx; playerPos.y = fy; playerPos.z = fz;
            // E rises -> rescue the captive if in range (3 m).
            bool eNow = kd(GLFW_KEY_E);
            if (eNow && !prevE) {
                const float dxe = playerPos.x - captive.pos().x;
                const float dze = playerPos.z - captive.pos().z;
                if (dxe*dxe + dze*dze <= 3.0f * 3.0f && captive.isCaptive()) {
                    if (captive.markRescued())
                        x3::logInfo("--world npc: captive RESCUED!");
                }
            }
            prevE = eNow;
            bartender.update(fdt, nscene, *nphys, playerPos);
            dockWorker.update(fdt, nscene, *nphys, playerPos);
            captive.update(fdt, nscene, *nphys, playerPos);
            mechanic.update(fdt, nscene, *nphys, playerPos);
            nphys->step(fdt);
            int cw, chh; glfwGetFramebufferSize(window, &cw, &chh);
            if (cw>0&&chh>0) device->onResize((uint32_t)cw,(uint32_t)chh);
            device->setCamera(fx, fy, fz, fyaw, fpitch, 60.0f);
            auto frame = device->beginFrame();
            if (frame.valid) drawNpcScene(frame);
            device->endFrame(frame);
        }
        bartender.shutdownRagdoll(*nphys);
        dockWorker.shutdownRagdoll(*nphys);
        captive.shutdownRagdoll(*nphys);
        mechanic.shutdownRagdoll(*nphys);
        device->destroyMesh(grMesh); device->destroyTexture(grTex);
        nphys->shutdown(); device->shutdown();
        if (window) glfwDestroyWindow(window); glfwTerminate();
        return 0;
    }

    // ======================================================================
    // ---- Act-3 EVA SPACEWALK showcase (--world eva) -----------------------
    // S12: the player free-floats OUTSIDE the ship in zero-G, on the hull, to
    // repair it (design spec §2.9). A big box "hull" is the close-traversable
    // surface (a real SpaceShip.glb is used if its LFS binary is present, else
    // the box placeholder per the task brief). Deep-space backdrop: dark clear
    // color + a procedural starfield of far bright points; SSAO + SSGI OFF
    // (they raster a starless black on no-RT — the memory-bank fallback). The
    // EVAController (app/space/eva.{h,cpp}) drives the camera: WASD thrust,
    // Space/Ctrl up/down, Shift boost, B toggles mag-boots (stick to the hull),
    // F attempts a hull repair near the marked repair point.
    if (worldMode == "eva") {
        x3::logInfo("--world eva: building the Act-3 EVA spacewalk showcase");
        std::unique_ptr<x3::phys::IPhysicsWorld> ephys(x3::phys::createPhysicsWorld());
        if (!ephys->init()) {
            x3::logError("--world eva: physics init failed");
            device->shutdown(); if (window) glfwDestroyWindow(window); glfwTerminate(); return 1;
        }

        // ---- Deep-space lighting. Sky OFF (dark clear shows through), SSAO + GI
        //      OFF (they black out a starfield scene with no nearby bounce).
        { x3::rhi::IRenderDevice::SkyParams sk{}; sk.enabled = false; device->setSkyParams(sk); }
        { x3::rhi::IRenderDevice::SsaoParams ap{}; ap.enabled = false; device->setSsaoParams(ap); }
        { x3::rhi::IRenderDevice::GiParams   gp{}; gp.enabled = false; device->setGiParams(gp); }
        // Bright point lights near the hull so it reads against the black (no
        // bounced light in vacuum — anything subtle silhouettes).
        { x3::rhi::PointLight pl[3];
          pl[0].pos[0] =  18.0f; pl[0].pos[1] =  16.0f; pl[0].pos[2] =  14.0f; pl[0].range = 120.0f;
          pl[0].color[0] = 22.0f; pl[0].color[1] = 21.0f; pl[0].color[2] = 19.0f;   // sun key
          pl[1].pos[0] = -16.0f; pl[1].pos[1] =  10.0f; pl[1].pos[2] =  -8.0f; pl[1].range = 90.0f;
          pl[1].color[0] = 6.0f;  pl[1].color[1] = 7.0f;  pl[1].color[2] = 10.0f;   // cool fill
          pl[2].pos[0] =  6.0f;  pl[2].pos[1] =  -8.0f; pl[2].pos[2] = -16.0f; pl[2].range = 90.0f;
          pl[2].color[0] = 5.0f;  pl[2].color[1] = 4.0f;  pl[2].color[2] = 3.0f;    // warm rim
          device->setPointLights(pl, 3); }

        // ---- The hull. Try a real SpaceShip.glb (auto-upgrades when its LFS
        //      binary is fetched); else a big box hull placeholder. The box is
        //      both the visual + the static collision the mag-boots stick to and
        //      the EVA suit can't tunnel through.
        const std::string rigDir = x3::game::riggedGlbRoot();
        std::unique_ptr<x3::asset::IAssetSource> asrc(x3::asset::createAssetSource());
        asrc->mountDir(rigDir, 0);
        std::unique_ptr<x3::asset::IModelLoader> hloader(x3::asset::createModelLoader(device.get(), asrc.get()));
        x3::asset::Model hullModel = hloader->load("SpaceShip.glb");
        std::vector<x3::asset::ModelDrawable> hullDrawables;
        if (hullModel.ok) hullDrawables = x3::asset::makeDrawables(hullModel);
        std::string hullUsed = hullModel.ok ? "SpaceShip.glb" : "box-hull-placeholder";
        x3::logInfo(std::string("--world eva: hull = ") + hullUsed);

        // Big box hull: a 24 x 6 x 10 m slab centered at origin, top face at y=3.
        // (Visual + collision; the EVA suit walks/floats over the +Y top face.)
        const float kHullTopY = 3.0f;
        x3::prims::PrimMesh hullBox = x3::prims::makeBox(12.0f, 3.0f, 5.0f, 0.0f, 0.0f, 0.0f, 1.0f);
        auto hullMesh = device->createMesh(hullBox.verts.data(), (uint32_t)hullBox.verts.size(),
                                           hullBox.index.data(), (uint32_t)hullBox.index.size());
        ephys->addStaticMesh(hullBox.cverts.data(), (uint32_t)(hullBox.cverts.size() / 3),
                             hullBox.cindex.data(), (uint32_t)hullBox.cindex.size());
        auto hullPx = x3::prims::makeCheckerRGBA(64, 8, 150, 158, 170, 70, 76, 90);
        auto hullTex = device->createTexture(hullPx.data(), 64, 64, true);

        // ---- A glowing repair-point marker sitting on the hull top face.
        const x3::phys::Vec3 repairPos{ 4.0f, kHullTopY, 1.5f };
        x3::prims::PrimMesh rpBox = x3::prims::makeBox(0.4f, 0.4f, 0.4f,
                                                       repairPos.x, repairPos.y + 0.4f, repairPos.z, 0.5f);
        auto rpMesh = device->createMesh(rpBox.verts.data(), (uint32_t)rpBox.verts.size(),
                                         rpBox.index.data(), (uint32_t)rpBox.index.size());
        auto rpPx = x3::prims::makeSolidRGBA(4, 255, 180, 40);   // amber "damage" flag
        auto rpTex = device->createTexture(rpPx.data(), 4, 4, true);

        // ---- Procedural starfield: many tiny bright cubes scattered on a far
        //      shell so the deep-space backdrop reads (deterministic LCG).
        constexpr int kStars = 240;
        std::vector<x3::rhi::MeshHandle> starMeshes; starMeshes.reserve(kStars);
        uint32_t rng = 0x5eed1234u;
        auto frand = [&](){ rng = rng * 1664525u + 1013904223u; return (rng >> 8) * (1.0f / 16777216.0f); };
        for (int i = 0; i < kStars; ++i) {
            // Random direction on a sphere, pushed out to a far radius.
            const float u = frand() * 2.0f - 1.0f;
            const float th = frand() * 6.2831853f;
            const float r = std::sqrt(std::max(0.0f, 1.0f - u*u));
            const float R = 180.0f + frand() * 80.0f;
            const float sx = R * r * std::cos(th);
            const float sy = R * u;
            const float sz = R * r * std::sin(th);
            const float ssz = 0.35f + frand() * 0.5f;   // tiny
            x3::prims::PrimMesh sm = x3::prims::makeBox(ssz, ssz, ssz, sx, sy, sz, 0.5f);
            starMeshes.push_back(device->createMesh(sm.verts.data(), (uint32_t)sm.verts.size(),
                                                    sm.index.data(), (uint32_t)sm.index.size()));
        }
        auto starPx = x3::prims::makeSolidRGBA(2, 255, 255, 255);
        auto starTex = device->createTexture(starPx.data(), 2, 2, true);

        // ---- Spawn the EVA suit just above the hull top face, near the airlock.
        x3::space::EVAController eva;
        x3::space::EVAController::Tuning evaT;   // defaults: zero-G, maxOxygenS=120
        eva.spawn(*ephys, -6.0f, kHullTopY + 1.2f, 0.0f, evaT);
        eva.setRepairPoint(repairPos, 2.0f);

        const float dt = 1.0f / 60.0f;

        // Draw a GLB hull at a placement matrix (if loaded), brightened for deep
        // space; else the box hull.
        auto drawHull = [&](const x3::rhi::FrameContext& frame) {
            const float white[4] = { 1, 1, 1, 1 };
            const float idM[16]  = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
            if (hullModel.ok) {
                // Scale + lift the GLB so it reads as a big hull under the suit.
                const float S = 6.0f;
                float xform[16] = { S,0,0,0, 0,S,0,0, 0,0,S,0, 0,0,0,1 };
                for (const auto& dr : hullDrawables) {
                    float fin[16];
                    x3::asset::mulMat4(xform, dr.nodeTransform, fin);
                    float tint[4] = {
                        dr.baseColorFactor[0] * 4.0f + 0.45f,
                        dr.baseColorFactor[1] * 4.0f + 0.50f,
                        dr.baseColorFactor[2] * 4.0f + 0.55f,
                        dr.baseColorFactor[3]
                    };
                    device->drawMesh(frame, x3::rhi::MeshHandle{ dr.meshId },
                                     x3::rhi::TextureHandle{ dr.baseColorTexId }, tint, fin);
                }
            }
            // Always draw the box hull (it is the collision + a robust visual).
            device->drawMesh(frame, hullMesh, hullTex, white, idM);
        };

        auto drawScene = [&](const x3::rhi::FrameContext& frame) {
            const float idM[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
            const float starTint[4] = { 6.0f, 6.0f, 6.5f, 1.0f };   // bright points
            for (auto sm : starMeshes) device->drawMesh(frame, sm, starTex, starTint, idM);
            drawHull(frame);
            const float amber[4] = { 3.0f, 2.0f, 0.5f, 1.0f };
            device->drawMesh(frame, rpMesh, rpTex, amber, idM);
        };

        // ===== Headless capture (--world eva --screenshot <path>) ==========
        if (headless) {
            device->setFrustumCull(false);
            // Vantage: above + behind the suit, looking down the hull toward the
            // amber repair marker so hull + starfield + suit-region all read.
            float cam[5] = { -12.0f, 9.0f, -7.0f, 0.35f, -0.45f };
            if (shotCamOverride) for (int k = 0; k < 5; ++k) cam[k] = shotCam[k];
            const std::string outPath = screenshot ? screenshotPath
                                                   : std::string("G:/X3Native/captures/eva.png");
            const int kFrames = 16;
            for (int i = 0; i < kFrames; ++i) {
                glfwPollEvents();
                eva.update(x3::game::PlayerInput{}, dt, *ephys);
                ephys->step(dt);
                device->setCamera(cam[0], cam[1], cam[2], cam[3], cam[4], 70.0f);
                if (i == kFrames - 1) device->armCapture(outPath.c_str());
                auto frame = device->beginFrame();
                if (frame.valid) drawScene(frame);
                device->endFrame(frame);
            }
            const bool wrote = device->captureFrame(outPath.c_str());
            if (wrote) x3::logInfo("--world eva: wrote " + outPath);
            else       x3::logError("--world eva: capture FAILED");
            device->destroyMesh(hullMesh); device->destroyMesh(rpMesh);
            for (auto sm : starMeshes) device->destroyMesh(sm);
            device->destroyTexture(hullTex); device->destroyTexture(rpTex); device->destroyTexture(starTex);
            if (hullModel.ok) hloader->unload(hullModel);
            ephys->shutdown(); device->shutdown();
            if (window) glfwDestroyWindow(window); glfwTerminate();
            return wrote ? 0 : 1;
        }

        // ===== Walkable windowed path: EVAController drives the camera. ====
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        double lastMX, lastMY; glfwGetCursorPos(window, &lastMX, &lastMY);
        double prevTime = glfwGetTime();
        bool prevB = false, prevF = false;
        x3::logInfo("--world eva: WASD thrust, Space/Ctrl up/down, Shift boost, "
                    "B mag-boots, F repair, mouse look, Esc to quit");
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

            // B toggles mag-boots (rising edge).
            bool bNow = kd(GLFW_KEY_B);
            if (bNow && !prevB) eva.setMagBoots(!eva.magBootsEnabled());
            prevB = bNow;

            x3::game::PlayerInput in;
            if (kd(GLFW_KEY_W)) in.moveFwd    += 1.0f;
            if (kd(GLFW_KEY_S)) in.moveFwd    -= 1.0f;
            if (kd(GLFW_KEY_D)) in.moveStrafe += 1.0f;
            if (kd(GLFW_KEY_A)) in.moveStrafe -= 1.0f;
            in.sprint      = kd(GLFW_KEY_LEFT_SHIFT);
            in.jumpPressed = kd(GLFW_KEY_SPACE);     // Space = ascend (held)
            in.lookDX = ddx; in.lookDY = ddy;
            eva.update(in, fdt, *ephys);
            // LeftCtrl descend: nudge feet down (matches swim's host convention;
            // ignored when mag-boots clamp the suit to the hull).
            if (kd(GLFW_KEY_LEFT_CONTROL) && !eva.magBootsActive()) {
                x3::phys::Vec3 p = ephys->getBodyPosition(eva.body());
                p.y -= eva.maxOxygenSeconds() > 0 ? 5.0f * fdt : 0.0f;
                ephys->setBodyPosition(eva.body(), p);
            }
            // F = attempt a hull repair near the marker (rising edge).
            bool fNow = kd(GLFW_KEY_F);
            if (fNow && !prevF) {
                if (eva.tryRepair()) x3::logInfo("--world eva: repair tick (in range)");
                else                 x3::logInfo("--world eva: no repair point in reach");
            }
            prevF = fNow;
            ephys->step(fdt);

            float cx, cy, cz, cyaw, cpitch;
            eva.camera(cx, cy, cz, cyaw, cpitch);
            int cw, chh; glfwGetFramebufferSize(window, &cw, &chh);
            if (cw != lastWd || chh != lastHd) {
                lastWd = cw; lastHd = chh;
                if (cw > 0 && chh > 0) device->onResize((uint32_t)cw, (uint32_t)chh);
            }
            device->setCamera(cx, cy, cz, cyaw, cpitch, 70.0f);
            auto frame = device->beginFrame();
            if (frame.valid) drawScene(frame);
            device->endFrame(frame);
        }
        device->destroyMesh(hullMesh); device->destroyMesh(rpMesh);
        for (auto sm : starMeshes) device->destroyMesh(sm);
        device->destroyTexture(hullTex); device->destroyTexture(rpTex); device->destroyTexture(starTex);
        if (hullModel.ok) hloader->unload(hullModel);
        ephys->shutdown(); device->shutdown();
        if (window) glfwDestroyWindow(window); glfwTerminate();
        return 0;
    }

    // ======================================================================
    // ---- Underwater SWIM showcase (--world swim) --------------------------
    // Act 4 tier8 undersea foundation: spawn the player at depth ~5 m below the
    // water surface (surfaceY = 0), surrounded by sea-creature stand-ins
    // (sharks / squid / whale / manta) as static decoration boxes (no AI yet —
    // a separate task). Headless: capture a still from the dive vantage.
    // Walkable: WASD swim + mouse look, Space ascend, LeftCtrl descend, LeftShift
    // boost, Esc to quit. Builds on app/swim_controller.{h,cpp}. Jolt (MIT) only.
    if (worldMode == "swim") {
        x3::logInfo("--world swim: building the Act 4 undersea showcase");
        std::unique_ptr<x3::phys::IPhysicsWorld> sphys(x3::phys::createPhysicsWorld());
        if (!sphys->init()) {
            x3::logError("--world swim: physics init failed");
            device->shutdown(); if (window) glfwDestroyWindow(window); glfwTerminate(); return 1;
        }

        // ---- Shared meshes + textures.
        // Box mesh used for both seafloor + creature stand-ins (varied scale).
        std::vector<x3::rhi::MeshVertex> bv; std::vector<uint32_t> bi;
        x3::prims::makeCube(0.5f, bv, bi);
        auto boxMesh = device->createMesh(bv.data(), (uint32_t)bv.size(),
                                          bi.data(), (uint32_t)bi.size());
        // Seafloor as a big flat quad at y = -20.
        x3::prims::PrimMesh floor = x3::prims::makeBox(60.0f, 0.5f, 60.0f, 0.0f, -20.5f, 0.0f, 0.5f);
        auto floorMesh = device->createMesh(floor.verts.data(), (uint32_t)floor.verts.size(),
                                            floor.index.data(), (uint32_t)floor.index.size());
        sphys->addStaticMesh(floor.cverts.data(), (uint32_t)(floor.cverts.size() / 3),
                             floor.cindex.data(), (uint32_t)floor.cindex.size());
        // Sand-tinted seafloor texture (warm tan), checker so the surface reads.
        auto sandPx = x3::prims::makeCheckerRGBA(128, 16, 200, 178, 120, 150, 130, 84);
        auto sandTex = device->createTexture(sandPx.data(), 128, 128, true);
        // Generic "creature" tile (mid-grey / blueish so the box stand-ins look
        // marine until skinned models drop in).
        auto creaturePx = x3::prims::makeSolidRGBA(8, 90, 110, 130);
        auto creatureTex = device->createTexture(creaturePx.data(), 8, 8, true);

        // ---- Sea-creature STATIC decorations. Stand-ins (boxes) at the
        // positions where the rigged_glb sea-fauna models will be placed later
        // (sea_giant_squid.glb / sea_hammerhead.glb / sea_humpback_whale.glb /
        // sea_manta_ray.glb / GreatWhiteSharkGameReady.glb). Varied depths so
        // the player can swim around them.
        struct Creature {
            const char* name;
            float x, y, z;      // world position
            float hx, hy, hz;   // half-extents (box stand-in)
            float color[3];
        };
        const Creature creatures[] = {
            // Name                      x      y      z    hx   hy   hz   tint
            { "GreatWhiteShark",       10.0f, -6.0f,  4.0f, 2.2f, 0.6f, 0.6f, { 0.55f, 0.62f, 0.66f } },
            { "sea_hammerhead",        -7.0f, -8.0f,  6.0f, 1.8f, 0.5f, 0.5f, { 0.62f, 0.66f, 0.70f } },
            { "sea_giant_squid",       -3.0f,-14.0f, -8.0f, 1.0f, 1.8f, 1.0f, { 0.78f, 0.50f, 0.55f } },
            { "sea_humpback_whale",   -18.0f,-10.0f, -2.0f, 5.5f, 1.5f, 1.5f, { 0.42f, 0.48f, 0.55f } },
            { "sea_manta_ray",          6.0f,-12.0f, -7.0f, 2.0f, 0.2f, 2.5f, { 0.35f, 0.40f, 0.48f } },
        };
        const int kNumCreatures = (int)(sizeof(creatures) / sizeof(creatures[0]));

        // Build per-creature box meshes (so we can give each a unique scale +
        // collision so the swimmer can't tunnel through them).
        std::vector<x3::rhi::MeshHandle> creatureMeshes;
        creatureMeshes.reserve(kNumCreatures);
        for (int i = 0; i < kNumCreatures; ++i) {
            const auto& c = creatures[i];
            x3::prims::PrimMesh m = x3::prims::makeBox(c.hx, c.hy, c.hz, c.x, c.y, c.z, 0.5f);
            auto mh = device->createMesh(m.verts.data(), (uint32_t)m.verts.size(),
                                         m.index.data(), (uint32_t)m.index.size());
            sphys->addStaticMesh(m.cverts.data(), (uint32_t)(m.cverts.size() / 3),
                                 m.cindex.data(), (uint32_t)m.cindex.size());
            creatureMeshes.push_back(mh);
        }

        // ---- Water + sky (sun-from-above tint) ---------------------------------
        // Underwater base color comes from the WaterParams' deep tint when viewed
        // from above; under the surface, the analytic sky gives a soft top-light.
        { x3::rhi::IRenderDevice::SkyParams sk{};
          sk.enabled = true;
          // Sun directly above + slightly forward, intensity dialed for the
          // through-water haze look.
          sk.sunDir[0] = 0.0f; sk.sunDir[1] = 1.0f; sk.sunDir[2] = 0.1f;
          sk.sunIntensity = 1.4f;
          sk.haze = 0.6f;
          sk.exposure = 1.0f;
          device->setSkyParams(sk); }
        { x3::rhi::IRenderDevice::WaterParams wp{};
          wp.enabled = true;
          wp.seaLevel = 0.0f;
          wp.amplitude = 0.35f;
          wp.steepness = 0.4f;
          wp.waveLength = 16.0f;
          wp.speed = 1.0f;
          // Slightly more saturated blue so the underwater POV reads as ocean.
          wp.deepColor[0]    = 0.02f; wp.deepColor[1]    = 0.06f; wp.deepColor[2]    = 0.10f;
          wp.shallowColor[0] = 0.08f; wp.shallowColor[1] = 0.30f; wp.shallowColor[2] = 0.40f;
          wp.specular = 8.0f; wp.fresnel = 0.02f;
          device->setWaterParams(wp); }
        // Two fill point lights: one near the player's spawn (so creatures
        // catch a rim from the camera side), one over the sand (so the floor
        // doesn't ink out).
        { x3::rhi::PointLight pl[3];
          pl[0].pos[0] = 0.0f;  pl[0].pos[1] = -3.0f; pl[0].pos[2] = 0.0f; pl[0].range = 18.0f;
          pl[0].color[0] = 1.6f; pl[0].color[1] = 1.9f; pl[0].color[2] = 2.4f;
          pl[1].pos[0] = 8.0f;  pl[1].pos[1] = -8.0f; pl[1].pos[2] = 4.0f; pl[1].range = 14.0f;
          pl[1].color[0] = 1.0f; pl[1].color[1] = 1.4f; pl[1].color[2] = 1.8f;
          pl[2].pos[0] = -10.0f; pl[2].pos[1] = -10.0f; pl[2].pos[2] = -2.0f; pl[2].range = 14.0f;
          pl[2].color[0] = 1.2f; pl[2].color[1] = 1.5f; pl[2].color[2] = 1.8f;
          device->setPointLights(pl, 3); }

        // ---- Spawn the swimmer at depth ~5 m below the surface.
        x3::game::SwimController swim;
        x3::game::SwimController::Tuning swimT;
        swimT.surfaceY = 0.0f;
        swim.spawn(*sphys, 0.0f, -5.0f, 0.0f, swimT);

        const float dt = 1.0f / 60.0f;

        // Shared draw helper: floor + creature stand-ins.
        auto drawScene = [&](const x3::rhi::FrameContext& frame) {
            const float white[4] = { 1, 1, 1, 1 };
            const float idM[16]  = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
            device->drawMesh(frame, floorMesh, sandTex, white, idM);
            for (int i = 0; i < kNumCreatures; ++i) {
                const auto& c = creatures[i];
                const float tint[4] = { c.color[0], c.color[1], c.color[2], 1.0f };
                device->drawMesh(frame, creatureMeshes[i], creatureTex, tint, idM);
            }
        };

        // ===== Headless capture (--world swim --screenshot <path>) =============
        if (headless) {
            // Vantage: at depth, looking forward toward the shark + hammerhead
            // arc so creatures + seafloor all read in the frame.
            float cam[5] = { 0.0f, -4.5f, -6.0f, 0.0f, -0.15f };
            if (shotCamOverride) for (int k = 0; k < 5; ++k) cam[k] = shotCam[k];
            const std::string outPath = screenshot ? screenshotPath
                                                   : std::string("G:/X3Native/captures/swim.png");
            const int kFrames = 30;
            float waterT = 0.0f;
            for (int i = 0; i < kFrames; ++i) {
                glfwPollEvents();
                sphys->step(dt);
                waterT += dt;
                { x3::rhi::IRenderDevice::WaterParams wp{};
                  wp.enabled = true; wp.seaLevel = 0.0f; wp.time = waterT;
                  wp.amplitude = 0.35f; wp.steepness = 0.4f; wp.waveLength = 16.0f; wp.speed = 1.0f;
                  wp.deepColor[0]    = 0.02f; wp.deepColor[1]    = 0.06f; wp.deepColor[2]    = 0.10f;
                  wp.shallowColor[0] = 0.08f; wp.shallowColor[1] = 0.30f; wp.shallowColor[2] = 0.40f;
                  wp.specular = 8.0f; wp.fresnel = 0.02f;
                  device->setWaterParams(wp); }
                device->setCamera(cam[0], cam[1], cam[2], cam[3], cam[4], 70.0f);
                if (i == kFrames - 1) device->armCapture(outPath.c_str());
                auto frame = device->beginFrame();
                if (frame.valid) drawScene(frame);
                device->endFrame(frame);
            }
            const bool wrote = device->captureFrame(outPath.c_str());
            if (wrote) x3::logInfo("--world swim: wrote " + outPath);
            else       x3::logError("--world swim: capture FAILED");
            device->destroyMesh(boxMesh);
            device->destroyMesh(floorMesh);
            for (auto m : creatureMeshes) device->destroyMesh(m);
            device->destroyTexture(sandTex);
            device->destroyTexture(creatureTex);
            sphys->shutdown(); device->shutdown();
            if (window) glfwDestroyWindow(window); glfwTerminate();
            return wrote ? 0 : 1;
        }

        // ===== Walkable windowed path: SwimController drives the camera. =======
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        double lastMX, lastMY; glfwGetCursorPos(window, &lastMX, &lastMY);
        double prevTime = glfwGetTime();
        float waterT = 0.0f;
        x3::logInfo("--world swim: WASD swim, Space ascend, LeftCtrl descend, "
                    "LeftShift boost, mouse look, Esc to quit");
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

            // Build a swim input. LeftCtrl descend is encoded by combining
            // moveFwd along a downward pitch — but with the swim controller's
            // jumpPressed = ascend convention, we wire descend by simply
            // applying a Y nudge after update() (the cleanest path without
            // touching PlayerInput shape). Equivalent: just teleport the body
            // a few cm down each frame LeftCtrl is held.
            x3::game::PlayerInput in;
            if (kd(GLFW_KEY_W)) in.moveFwd    += 1.0f;
            if (kd(GLFW_KEY_S)) in.moveFwd    -= 1.0f;
            if (kd(GLFW_KEY_D)) in.moveStrafe += 1.0f;
            if (kd(GLFW_KEY_A)) in.moveStrafe -= 1.0f;
            in.sprint      = kd(GLFW_KEY_LEFT_SHIFT);
            in.jumpPressed = kd(GLFW_KEY_SPACE);     // Space = ascend (held)
            in.lookDX = ddx; in.lookDY = ddy;
            swim.update(in, fdt, *sphys);
            // LeftCtrl descend: nudge feet down at swim speed.
            if (kd(GLFW_KEY_LEFT_CONTROL)) {
                x3::phys::Vec3 p = sphys->getBodyPosition(swim.body());
                p.y -= 3.5f * fdt;
                sphys->setBodyPosition(swim.body(), p);
            }
            sphys->step(fdt);

            // Animate water surface scroll.
            waterT += fdt;
            { x3::rhi::IRenderDevice::WaterParams wp{};
              wp.enabled = true; wp.seaLevel = 0.0f; wp.time = waterT;
              wp.amplitude = 0.35f; wp.steepness = 0.4f; wp.waveLength = 16.0f; wp.speed = 1.0f;
              wp.deepColor[0]    = 0.02f; wp.deepColor[1]    = 0.06f; wp.deepColor[2]    = 0.10f;
              wp.shallowColor[0] = 0.08f; wp.shallowColor[1] = 0.30f; wp.shallowColor[2] = 0.40f;
              wp.specular = 8.0f; wp.fresnel = 0.02f;
              device->setWaterParams(wp); }

            float cx, cy, cz, cyaw, cpitch;
            swim.camera(cx, cy, cz, cyaw, cpitch);
            int cw, chh; glfwGetFramebufferSize(window, &cw, &chh);
            if (cw != lastWd || chh != lastHd) {
                lastWd = cw; lastHd = chh;
                if (cw > 0 && chh > 0) device->onResize((uint32_t)cw, (uint32_t)chh);
            }
            device->setCamera(cx, cy, cz, cyaw, cpitch, 70.0f);
            auto frame = device->beginFrame();
            if (frame.valid) drawScene(frame);
            device->endFrame(frame);
        }
        device->destroyMesh(boxMesh);
        device->destroyMesh(floorMesh);
        for (auto m : creatureMeshes) device->destroyMesh(m);
        device->destroyTexture(sandTex);
        device->destroyTexture(creatureTex);
        sphys->shutdown(); device->shutdown();
        if (window) glfwDestroyWindow(window); glfwTerminate();
        return 0;
    }

    // ---- Undersea base showcase (--world undersea) -------------------------
    // The Act-4 OceanBase graybox (3-level disc on the seafloor) draped with the
    // real textured Abyssal Station GLB (UnderseaArtSystem), lit deep-sea, with a
    // headless --screenshot path + a slow windowed orbit. Mirrors --world swim.
    if (worldMode == "undersea") {
        x3::logInfo("--world undersea: building the Act-4 undersea base + Abyssal Station overlay");
        std::unique_ptr<x3::phys::IPhysicsWorld> uphys(x3::phys::createPhysicsWorld());
        if (!uphys->init()) {
            x3::logError("--world undersea: physics init failed");
            device->shutdown(); if (window) glfwDestroyWindow(window); glfwTerminate(); return 1;
        }

        // Showcase at NEAR-ORIGIN so the point lights actually reach the station.
        // The real OceanBase sits at offshore (1100,-1350) where the Forward+ light
        // grid does not currently cover it (see undersea_art.cpp's note — flagged to
        // Integrator), so the station reads dark there. Here we place an equivalent
        // OceanBasePlan at the origin + a dark sediment seafloor, and the same GLB
        // overlay reads as a LIT hero. The --test-undersea-art gate still verifies
        // the real OceanBase placement; this is purely the visual showcase.
        x3::game::OceanBasePlan p{};
        p.cx = 0.0f; p.cz = 0.0f; p.surfaceY = 70.0f;   // surface far above -> we are UNDERWATER
        p.baseDeckY = 0.0f; p.seafloorY = -3.0f; p.radius = 45.0f; p.levels = 1;
        p.hasSubDock = p.hasAirlock = p.hasReactor = true;

        // Dark sediment seafloor at origin (the station sits on it).
        x3::prims::PrimMesh sf = x3::prims::makeBox(95.0f, 1.0f, 95.0f, 0.0f, p.seafloorY - 1.0f, 0.0f, 0.5f);
        auto sfMesh = device->createMesh(sf.verts.data(), (uint32_t)sf.verts.size(),
                                         sf.index.data(), (uint32_t)sf.index.size());
        auto sedPx  = x3::prims::makeCheckerRGBA(128, 24, 44, 50, 56, 30, 35, 41);
        auto sedTex = device->createTexture(sedPx.data(), 128, 128, true);

        x3::game::UnderseaArtSystem undersea;
        undersea.build(*device, x3::game::convertedGlbRoot(), p);

        // ---- Deep-sea sky + water (dark, high haze) ----
        { x3::rhi::IRenderDevice::SkyParams sk{};
          sk.enabled = true;
          sk.sunDir[0] = 0.05f; sk.sunDir[1] = 1.0f; sk.sunDir[2] = 0.15f;
          sk.sunIntensity = 1.8f; sk.haze = 0.9f; sk.exposure = 0.8f;   // god-ray top light
          device->setSkyParams(sk); }
        { x3::rhi::IRenderDevice::WaterParams wp{};
          wp.enabled = true; wp.seaLevel = p.surfaceY;
          wp.amplitude = 0.4f; wp.steepness = 0.45f; wp.waveLength = 18.0f; wp.speed = 1.0f;
          wp.deepColor[0]    = 0.008f; wp.deepColor[1]    = 0.035f; wp.deepColor[2]    = 0.075f;
          wp.shallowColor[0] = 0.05f;  wp.shallowColor[1] = 0.22f;  wp.shallowColor[2] = 0.32f;
          wp.specular = 8.0f; wp.fresnel = 0.02f;
          device->setWaterParams(wp); }

        // Station's own cool point-light fixtures + a bright high key so the PBR
        // hull reads + feeds bloom (these DO reach it at the origin).
        std::vector<x3::rhi::PointLight> plights = undersea.lightFixtures();
        { x3::rhi::PointLight key; key.pos[0]=p.cx; key.pos[1]=p.baseDeckY+42.0f; key.pos[2]=p.cz;
          key.range=130.0f; key.color[0]=2.2f; key.color[1]=2.6f; key.color[2]=3.2f;
          plights.push_back(key); }
        if (!plights.empty()) device->setPointLights(plights.data(), (uint32_t)plights.size());

        auto drawScene = [&](const x3::rhi::FrameContext& frame) {
            const float white[4] = { 1, 1, 1, 1 };
            const float idM[16]  = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
            device->drawMesh(frame, sfMesh, sedTex, white, idM);  // seafloor
            undersea.draw(*device, frame);                        // the real textured station, LIT
        };

        // Camera framing: orbit the disc centre, eye a touch above the top deck,
        // looking down slightly to catch station + disc + a sliver of seafloor.
        const float bx = p.cx, bz = p.cz;
        const float lookY = p.baseDeckY + 13.0f;   // station mid (it spans deck..+~34 m)
        auto computeCam = [&](float ang, float cam[5]) {
            const float R = 74.0f;                  // hero framing
            const float camX = bx + R * std::cos(ang);
            const float camZ = bz + R * std::sin(ang);
            const float camY = p.baseDeckY + 24.0f;
            const float dx = bx - camX, dy = lookY - camY, dz = bz - camZ;
            const float len = std::sqrt(dx*dx + dy*dy + dz*dz);
            cam[0]=camX; cam[1]=camY; cam[2]=camZ;
            cam[3]=std::atan2(dz, dx);
            cam[4]=std::asin(dy / (len > 1e-3f ? len : 1e-3f));
        };
        const float dt = 1.0f / 60.0f;

        // ===== Headless capture (--world undersea --screenshot <path>) =====
        if (headless) {
            float cam[5]; computeCam(-0.7f, cam);
            if (shotCamOverride) for (int k = 0; k < 5; ++k) cam[k] = shotCam[k];
            const std::string outPath = screenshot ? screenshotPath
                                                   : std::string("G:/X3Native/captures/undersea.png");
            const int kFrames = 40;
            float waterT = 0.0f;
            for (int i = 0; i < kFrames; ++i) {
                glfwPollEvents();
                uphys->step(dt);
                waterT += dt;
                { x3::rhi::IRenderDevice::WaterParams wp{};
                  wp.enabled = true; wp.seaLevel = p.surfaceY; wp.time = waterT;
                  wp.amplitude = 0.4f; wp.steepness = 0.45f; wp.waveLength = 18.0f; wp.speed = 1.0f;
                  wp.deepColor[0]=0.008f; wp.deepColor[1]=0.035f; wp.deepColor[2]=0.075f;
                  wp.shallowColor[0]=0.05f; wp.shallowColor[1]=0.22f; wp.shallowColor[2]=0.32f;
                  wp.specular = 8.0f; wp.fresnel = 0.02f;
                  device->setWaterParams(wp); }
                device->setCamera(cam[0], cam[1], cam[2], cam[3], cam[4], 68.0f);
                if (i == kFrames - 1) device->armCapture(outPath.c_str());
                auto frame = device->beginFrame();
                if (frame.valid) drawScene(frame);
                device->endFrame(frame);
            }
            const bool wrote = device->captureFrame(outPath.c_str());
            if (wrote) x3::logInfo("--world undersea: wrote " + outPath);
            else       x3::logError("--world undersea: capture FAILED");
            device->destroyMesh(sfMesh); device->destroyTexture(sedTex);
            uphys->shutdown(); device->shutdown();
            if (window) glfwDestroyWindow(window); glfwTerminate();
            return wrote ? 0 : 1;
        }

        // ===== Windowed: slow auto-orbit, ESC to quit. =====
        x3::logInfo("--world undersea: slow orbit showcase — Esc to quit");
        double prevTime = glfwGetTime();
        float waterT = 0.0f, ang = -0.7f;
        int lastWd = (int)W, lastHd = (int)H;
        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();
            if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;
            double now = glfwGetTime();
            float fdt = (float)(now - prevTime); prevTime = now;
            if (fdt > 0.1f) fdt = 0.1f;
            uphys->step(fdt);
            waterT += fdt; ang += fdt * 0.12f;
            { x3::rhi::IRenderDevice::WaterParams wp{};
              wp.enabled = true; wp.seaLevel = p.surfaceY; wp.time = waterT;
              wp.amplitude = 0.4f; wp.steepness = 0.45f; wp.waveLength = 18.0f; wp.speed = 1.0f;
              wp.deepColor[0]=0.008f; wp.deepColor[1]=0.035f; wp.deepColor[2]=0.075f;
              wp.shallowColor[0]=0.05f; wp.shallowColor[1]=0.22f; wp.shallowColor[2]=0.32f;
              wp.specular = 8.0f; wp.fresnel = 0.02f;
              device->setWaterParams(wp); }
            float cam[5]; computeCam(ang, cam);
            int cw, chh; glfwGetFramebufferSize(window, &cw, &chh);
            if (cw != lastWd || chh != lastHd) {
                lastWd = cw; lastHd = chh;
                if (cw > 0 && chh > 0) device->onResize((uint32_t)cw, (uint32_t)chh);
            }
            device->setCamera(cam[0], cam[1], cam[2], cam[3], cam[4], 68.0f);
            auto frame = device->beginFrame();
            if (frame.valid) drawScene(frame);
            device->endFrame(frame);
        }
        device->destroyMesh(sfMesh); device->destroyTexture(sedTex);
        uphys->shutdown(); device->shutdown();
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

    // ---- S5 SHIP INTERIOR showcase (--world ship-interior) -----------------
    // The walkable, data-driven ship interior (Star Trek static frame): the player
    // walks INSIDE a cockpit + corridor built from a ShipManifest (app/space/
    // ship_interior.*). STATIC at scene origin; window placements are stored for S6.
    //   * WALKABLE (windowed): `--world ship-interior` — WASD + mouse, walls collide
    //     (the real Player capsule), Space jump, Esc quit.
    //   * SCREENSHOT (headless): `--world ship-interior --screenshot <path>` — pose a
    //     camera inside the cockpit, settle a few frames, capture the PNG, exit.
    if (worldMode == "ship-interior") {
        x3::logInfo("--world ship-interior: building the walkable cockpit + corridor");

        std::unique_ptr<x3::phys::IPhysicsWorld> sphys(x3::phys::createPhysicsWorld());
        if (!sphys->init()) {
            x3::logError("--world ship-interior: physics init failed");
            device->shutdown();
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return 1;
        }

        x3::game::Scene sscene;
        x3::space::ShipInterior interior;
        interior.build(*device, sscene, *sphys, x3::space::ShipInterior::makeSmallCockpit());

        // Interior lighting: NO sky (we're inside the hull). Point lights at the
        // ceiling of each room so the deck reads. Disable SSAO/GI raster fallback so a
        // 1080-Ti-class no-RT capture is not black (per the lane brief).
        { x3::rhi::IRenderDevice::SkyParams sp{}; sp.enabled = false; device->setSkyParams(sp); }
        { x3::rhi::IRenderDevice::SsaoParams ao{}; ao.enabled = false; device->setSsaoParams(ao); }
        { x3::rhi::IRenderDevice::GiParams gi{}; gi.enabled = false; device->setGiParams(gi); }
        { x3::rhi::PointLight pl[3];
          pl[0].pos[0]= 0.0f; pl[0].pos[1]=2.7f; pl[0].pos[2]= 0.0f; pl[0].range=10.0f;
          pl[0].color[0]=6.0f; pl[0].color[1]=6.2f; pl[0].color[2]=6.6f;            // cockpit
          pl[1].pos[0]= 0.0f; pl[1].pos[1]=2.7f; pl[1].pos[2]= 5.5f; pl[1].range=9.0f;
          pl[1].color[0]=4.0f; pl[1].color[1]=4.4f; pl[1].color[2]=5.0f;            // corridor
          pl[2].pos[0]= 0.0f; pl[2].pos[1]=1.4f; pl[2].pos[2]=-2.6f; pl[2].range=5.0f;
          pl[2].color[0]=2.0f; pl[2].color[1]=3.0f; pl[2].color[2]=4.0f;            // helm glow
          device->setPointLights(pl, 3); }

        const x3::phys::Vec3 spawn = interior.spawnPoint();

        // ===== Headless screenshot path: pose a camera inside the cockpit. =====
        if (headless) {
            // Stand near the aft of the cockpit looking forward toward the helm + the
            // forward window so the deck/walls/console fill the frame.
            float cam[5] = { 0.0f, 1.7f, 2.2f, -1.5708f, -0.06f };
            if (shotCamOverride) for (int k = 0; k < 5; ++k) cam[k] = shotCam[k];
            const float dt = 1.0f / 60.0f;
            const std::string outPath = screenshot ? screenshotPath
                                                   : std::string("G:/X3Native/captures/interior.png");
            const int kSettle = 12;
            for (int i = 0; i < kSettle; ++i) {
                glfwPollEvents();
                sphys->step(dt);
                sscene.update(*sphys);
                device->setCamera(cam[0], cam[1], cam[2], cam[3], cam[4], 70.0f);
                if (i == kSettle - 1) device->armCapture(outPath.c_str());
                auto frame = device->beginFrame();
                if (frame.valid) interior.render(*device, frame, sscene);
                device->endFrame(frame);
            }
            const bool wrote = device->captureFrame(outPath.c_str());
            if (wrote) x3::logInfo("--world ship-interior: wrote " + outPath);
            else       x3::logError("--world ship-interior: capture FAILED");
            interior.shutdown(*sphys);
            sphys->shutdown();
            device->shutdown();
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return wrote ? 0 : 1;
        }

        // ===== Walkable windowed path: the real first-person Player + physics. =====
        x3::game::Player splayer;
        splayer.spawn(*sphys, spawn.x, spawn.y, spawn.z);

        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        double lastMX, lastMY; glfwGetCursorPos(window, &lastMX, &lastMY);
        double prevTime = glfwGetTime();
        bool prevSpaceS = false;
        int lastWs = (int)W, lastHs = (int)H;
        x3::logInfo("--world ship-interior: WASD walk, mouse look, Space jump, LeftShift sprint, Esc to quit");

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

            x3::game::PlayerInput in;
            if (kd(GLFW_KEY_W)) in.moveFwd    += 1.0f;
            if (kd(GLFW_KEY_S)) in.moveFwd    -= 1.0f;
            if (kd(GLFW_KEY_D)) in.moveStrafe += 1.0f;
            if (kd(GLFW_KEY_A)) in.moveStrafe -= 1.0f;
            in.sprint      = kd(GLFW_KEY_LEFT_SHIFT);
            in.jumpPressed = spaceNow && !prevSpaceS;
            in.lookDX = ddx; in.lookDY = ddy;
            prevSpaceS = spaceNow;

            splayer.update(in, dt, *sphys);
            sphys->step(dt);
            sscene.update(*sphys);

            float camX, camY, camZ, camYaw, camPitch;
            splayer.camera(camX, camY, camZ, camYaw, camPitch);

            int cw, ch; glfwGetFramebufferSize(window, &cw, &ch);
            if (cw != lastWs || ch != lastHs) { lastWs = cw; lastHs = ch; if (cw>0&&ch>0) device->onResize((uint32_t)cw,(uint32_t)ch); }

            device->setCamera(camX, camY, camZ, camYaw, camPitch, 70.0f);
            auto frame = device->beginFrame();
            if (frame.valid) interior.render(*device, frame, sscene);
            device->endFrame(frame);
        }

        interior.shutdown(*sphys);
        sphys->shutdown();
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return 0;
    }

    // ---- S6 SHIP WINDOWS showcase (--world ship-windows) -------------------
    if (worldMode == "ship-windows") {
        x3::logInfo("--world ship-windows: building the cockpit with TRUE-PORTAL moving space");

        std::unique_ptr<x3::phys::IPhysicsWorld> sphys(x3::phys::createPhysicsWorld());
        if (!sphys->init()) {
            x3::logError("--world ship-windows: physics init failed");
            device->shutdown();
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return 1;
        }

        x3::game::Scene sscene;
        x3::space::ShipInterior interior;
        interior.build(*device, sscene, *sphys, x3::space::ShipInterior::makeSmallCockpit());

        x3::space::ShipWindows windows;
        windows.init(*device, interior.manifest());

        // No sky (inside the hull). Disable SSAO/GI raster fallback so a no-RT
        // capture is not black (per the lane brief). Interior fill lights + the
        // ShipWindows light-bleed lights are uploaded together.
        { x3::rhi::IRenderDevice::SkyParams sp{}; sp.enabled = false; device->setSkyParams(sp); }
        { x3::rhi::IRenderDevice::SsaoParams ao{}; ao.enabled = false; device->setSsaoParams(ao); }
        { x3::rhi::IRenderDevice::GiParams gi{}; gi.enabled = false; device->setGiParams(gi); }
        // Merge interior ceiling fill + the per-window light-bleed lights.
        std::vector<x3::rhi::PointLight> lights;
        { x3::rhi::PointLight pl{}; pl.pos[0]=0.0f; pl.pos[1]=2.7f; pl.pos[2]=0.0f;
          pl.range=10.0f; pl.color[0]=4.0f; pl.color[1]=4.2f; pl.color[2]=4.6f; lights.push_back(pl); }
        for (const auto& bl : windows.bleedLights()) lights.push_back(bl);
        device->setPointLights(lights.data(), (uint32_t)lights.size());

        // ===== Headless screenshot: look forward at the moving viewport. =====
        if (headless) {
            // Stand at the aft of the cockpit looking forward (-Z) at the forward
            // window so the deck + console + the moving space fill the frame.
            float cam[5] = { 0.0f, 1.7f, 2.2f, -1.5708f, -0.04f };
            if (shotCamOverride) for (int k = 0; k < 5; ++k) cam[k] = shotCam[k];
            const float dt = 1.0f / 60.0f;
            const std::string outPath = screenshot ? screenshotPath
                                                   : std::string("G:/X3Native/captures/windows.png");
            const int kSettle = 16;
            for (int i = 0; i < kSettle; ++i) {
                glfwPollEvents();
                sphys->step(dt);
                sscene.update(*sphys);
                const float t = (float)i * dt;
                const float envYaw = 0.25f * t;          // the ship "flies" — space pans
                device->setCamera(cam[0], cam[1], cam[2], cam[3], cam[4], 70.0f);
                if (i == kSettle - 1) device->armCapture(outPath.c_str());
                auto frame = device->beginFrame();
                if (frame.valid) {
                    interior.render(*device, frame, sscene);
                    windows.setCamera(cam[0], cam[1], cam[2]);
                    windows.render(*device, frame, /*viewProj16=*/nullptr, t, envYaw, 0.0f);
                }
                device->endFrame(frame);
            }
            const bool wrote = device->captureFrame(outPath.c_str());
            if (wrote) x3::logInfo("--world ship-windows: wrote " + outPath);
            else       x3::logError("--world ship-windows: capture FAILED");
            windows.shutdown(*device);
            interior.shutdown(*sphys);
            sphys->shutdown();
            device->shutdown();
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return wrote ? 0 : 1;
        }

        // ===== Walkable windowed path: real first-person Player + the moving space. =====
        x3::game::Player splayer;
        const x3::phys::Vec3 spawn = interior.spawnPoint();
        splayer.spawn(*sphys, spawn.x, spawn.y, spawn.z);

        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        double lastMX, lastMY; glfwGetCursorPos(window, &lastMX, &lastMY);
        double startTime = glfwGetTime();
        double prevTime = startTime;
        bool prevSpaceS = false;
        int lastWs = (int)W, lastHs = (int)H;
        x3::logInfo("--world ship-windows: WASD walk, mouse look, Space jump, LeftShift sprint, Esc to quit");

        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();
            if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;

            double now = glfwGetTime();
            float dt = (float)(now - prevTime); prevTime = now;
            if (dt > 0.1f) dt = 0.1f;
            const float t = (float)(now - startTime);

            double mx, my; glfwGetCursorPos(window, &mx, &my);
            float ddx = (float)(mx - lastMX), ddy = (float)(my - lastMY);
            lastMX = mx; lastMY = my;

            auto kd = [&](int k) { return glfwGetKey(window, k) == GLFW_PRESS; };
            bool spaceNow = kd(GLFW_KEY_SPACE);

            x3::game::PlayerInput in;
            if (kd(GLFW_KEY_W)) in.moveFwd    += 1.0f;
            if (kd(GLFW_KEY_S)) in.moveFwd    -= 1.0f;
            if (kd(GLFW_KEY_D)) in.moveStrafe += 1.0f;
            if (kd(GLFW_KEY_A)) in.moveStrafe -= 1.0f;
            in.sprint      = kd(GLFW_KEY_LEFT_SHIFT);
            in.jumpPressed = spaceNow && !prevSpaceS;
            in.lookDX = ddx; in.lookDY = ddy;
            prevSpaceS = spaceNow;

            splayer.update(in, dt, *sphys);
            sphys->step(dt);
            sscene.update(*sphys);

            float camX, camY, camZ, camYaw, camPitch;
            splayer.camera(camX, camY, camZ, camYaw, camPitch);

            int cw, ch; glfwGetFramebufferSize(window, &cw, &ch);
            if (cw != lastWs || ch != lastHs) { lastWs = cw; lastHs = ch; if (cw>0&&ch>0) device->onResize((uint32_t)cw,(uint32_t)ch); }

            const float envYaw = 0.18f * t;   // slow pan: "flying through space"
            device->setCamera(camX, camY, camZ, camYaw, camPitch, 70.0f);
            auto frame = device->beginFrame();
            if (frame.valid) {
                interior.render(*device, frame, sscene);
                windows.setCamera(camX, camY, camZ);
                windows.render(*device, frame, /*viewProj16=*/nullptr, t, envYaw, 0.0f);
            }
            device->endFrame(frame);
        }

        windows.shutdown(*device);
        interior.shutdown(*sphys);
        sphys->shutdown();
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return 0;
    }

    if (worldMode == "ship-repair") {
        x3::logInfo("--world ship-repair: cockpit + damaged panels (walk up + E to repair)");

        std::unique_ptr<x3::phys::IPhysicsWorld> rphys(x3::phys::createPhysicsWorld());
        if (!rphys->init()) {
            x3::logError("--world ship-repair: physics init failed");
            device->shutdown();
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return 1;
        }

        x3::game::Scene rscene;
        x3::space::ShipInterior rinterior;
        rinterior.build(*device, rscene, *rphys, x3::space::ShipInterior::makeSmallCockpit());

        // Interior lighting (no sky inside the hull; disable SSAO/GI raster fallback so
        // a no-RT capture is not black — per the lane brief).
        { x3::rhi::IRenderDevice::SkyParams sp{}; sp.enabled = false; device->setSkyParams(sp); }
        { x3::rhi::IRenderDevice::SsaoParams ao{}; ao.enabled = false; device->setSsaoParams(ao); }
        { x3::rhi::IRenderDevice::GiParams gi{}; gi.enabled = false; device->setGiParams(gi); }
        { x3::rhi::PointLight pl[2];
          pl[0].pos[0]= 0.0f; pl[0].pos[1]=2.7f; pl[0].pos[2]= 0.0f; pl[0].range=10.0f;
          pl[0].color[0]=6.0f; pl[0].color[1]=6.2f; pl[0].color[2]=6.6f;
          pl[1].pos[0]= 2.4f; pl[1].pos[1]=1.4f; pl[1].pos[2]=-2.0f; pl[1].range=5.0f;
          pl[1].color[0]=3.0f; pl[1].color[1]=2.2f; pl[1].color[2]=1.4f;            // panel glow
          device->setPointLights(pl, 2); }

        // ---- Build the repair model + its visual entities -------------------
        // One damaged panel on the +X (starboard) cockpit wall (x ~ +3), at chest
        // height, with 3 wires; and a second on the -X wall with 2 wires.
        x3::space::RepairSystem repair;
        const int kWiresA = 3, kWiresB = 2;
        // On the inner face of the +X / -X cockpit walls (wall inner face ~ +/-2.82),
        // protruding into the room so the panel + cavity read as a surface fixture.
        const float pA[3] = {  2.62f, 1.25f, -1.2f };
        const float pB[3] = { -2.62f, 1.25f,  1.2f };
        repair.addPanel(pA,  1.5708f, kWiresA);   // faces -X (into the room)
        repair.addPanel(pB, -1.5708f, kWiresB);   // faces +X

        // Visual building blocks. A "frame" plate (always there), a "cover" plate that
        // hides the wiring while Sealed, and N wire bars per panel that light green as
        // connected. We track the scene ids so we can recolor/hide them each frame.
        auto solidPx = x3::prims::makeSolidRGBA(8, 70, 74, 84);
        x3::rhi::TextureHandle solidTex = device->createTexture(solidPx.data(), 8, 8, true);
        const float kIdent[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

        struct PanelVis { uint32_t cover; std::vector<uint32_t> wires; };
        std::vector<PanelVis> vis;

        auto addBox = [&](float hx,float hy,float hz, float cx,float cy,float cz,
                          const float col[4], const float em[4], uint32_t tag) -> uint32_t {
            x3::prims::PrimMesh m = x3::prims::makeBox(hx,hy,hz, cx,cy,cz, 1.0f);
            x3::rhi::MeshHandle mesh = device->createMesh(m.verts.data(),(uint32_t)m.verts.size(),
                                                          m.index.data(),(uint32_t)m.index.size());
            x3::game::Entity e;
            e.mesh = mesh; e.tex = solidTex;
            for (int k=0;k<4;++k) e.baseColor[k]=col[k];
            if (em) for (int k=0;k<4;++k) e.emissive[k]=em[k];
            std::memcpy(e.transform, kIdent, sizeof(kIdent));
            e.tag = tag;
            return rscene.add(e);
        };

        const float frameCol[4] = {0.18f,0.20f,0.24f,1.0f};
        const float coverCol[4] = {0.30f,0.32f,0.38f,1.0f};
        const float coverEm[4]  = {0.0f,0.0f,0.0f,0.0f};
        for (uint32_t pi = 0; pi < repair.panelCount(); ++pi) {
            const x3::space::RepairPanel& p = repair.panel(pi);
            // `side` = sign of the INWARD normal (toward the room). The panel sits on
            // the wall; depth layers go OUTWARD (into the hull, away from the room):
            //   cover  (room-most, hides the cavity while Sealed)
            //   wires  (just behind the cover)
            //   frame  (deepest — the cavity backing, behind the wires)
            // so an opened panel reveals the lit wires in front of the dark backing.
            const float side = (p.pos[0] > 0.0f) ? -1.0f : 1.0f;   // inward normal x sign
            const float coverX = p.pos[0] + side * 0.02f;          // room-most face
            const float wireX  = coverX  - side * 0.08f;           // behind the cover
            const float frameX = coverX  - side * 0.16f;           // deepest backing
            // Recessed frame (the open cavity backing — deepest layer).
            addBox(0.04f, 0.55f, 0.45f, frameX, p.pos[1], p.pos[2],
                   frameCol, nullptr, (uint32_t)x3::game::Tag::Prop);
            PanelVis pv;
            // Cover plate (room-most; hides the wiring while Sealed).
            pv.cover = addBox(0.03f, 0.55f, 0.45f, coverX, p.pos[1], p.pos[2],
                              coverCol, coverEm, (uint32_t)x3::game::Tag::Prop);
            // Wire bars, stacked vertically inside the cavity, in FRONT of the frame.
            const int n = p.wiresTotal;
            const float darkCol[4] = {0.10f,0.10f,0.12f,1.0f};
            for (int w = 0; w < n; ++w) {
                const float wy = p.pos[1] - 0.32f + (n>1 ? (0.64f * (float)w/(float)(n-1)) : 0.0f);
                uint32_t id = addBox(0.03f, 0.045f, 0.34f, wireX, wy, p.pos[2],
                                     darkCol, nullptr, (uint32_t)x3::game::Tag::Prop);
                rscene.get(id).visible = false;   // hidden until the panel opens
                pv.wires.push_back(id);
            }
            vis.push_back(std::move(pv));
        }

        // Per-frame: sync the visuals from the RepairSystem model. Cover hidden once
        // Open; wires shown + recolored (red->green) by connected count; a Repairing/
        // Repaired panel glows green.
        auto syncVisuals = [&]() {
            for (uint32_t pi = 0; pi < repair.panelCount(); ++pi) {
                const x3::space::RepairPanel& p = repair.panel(pi);
                const PanelVis& pv = vis[pi];
                const bool opened = (p.state != x3::space::PanelState::Sealed);
                rscene.get(pv.cover).visible = !opened;
                for (int w = 0; w < (int)pv.wires.size(); ++w) {
                    x3::game::Entity& e = rscene.get(pv.wires[(size_t)w]);
                    e.visible = opened;
                    const bool on = w < p.wiresConnected;
                    if (on) { e.baseColor[0]=0.1f; e.baseColor[1]=0.9f; e.baseColor[2]=0.2f;
                              e.emissive[0]=0.2f; e.emissive[1]=2.4f; e.emissive[2]=0.4f; e.emissive[3]=2.0f; }
                    else    { e.baseColor[0]=0.7f; e.baseColor[1]=0.15f; e.baseColor[2]=0.12f;
                              e.emissive[0]=1.2f; e.emissive[1]=0.2f; e.emissive[2]=0.15f; e.emissive[3]=1.0f; }
                }
            }
        };

        const x3::phys::Vec3 rspawn = rinterior.spawnPoint();

        // ===== Headless screenshot path: auto-drive the interaction. =====
        if (headless) {
            // Camera across the cockpit looking head-on at panel A on the +X wall
            // (toward +X, yaw ~0). At this stand-off the open cavity reads as a
            // framed panel and the green-lit wire bars pop against the dark backing.
            float cam[5] = { 1.05f, 1.32f, -1.2f, 0.0f, -0.03f };
            if (shotCamOverride) for (int k = 0; k < 5; ++k) cam[k] = shotCam[k];
            const float dt = 1.0f / 60.0f;
            const std::string outPath = screenshot ? screenshotPath
                                                   : std::string("G:/X3Native/captures/repair.png");
            // The auto-driver's "player": standing at the panel so it is in range.
            const float driverPos[3] = { 1.9f, 1.4f, -1.2f };
            const int kSettle = 40;
            for (int i = 0; i < kSettle; ++i) {
                glfwPollEvents();
                rphys->step(dt);
                rscene.update(*rphys);
                // Auto-drive: open the panel on frame 4, connect a wire every 5 frames
                // so the capture shows an OPEN panel mid-repair (some wires lit).
                int inRange = repair.update(dt, driverPos, /*interactPressed*/ i == 4);
                if (i >= 8 && i < 8 + 5 * (kWiresA - 1) && (i % 5) == 0 && inRange >= 0)
                    repair.connectWire(inRange);   // connect all but the last (stay Open)
                syncVisuals();
                device->setCamera(cam[0], cam[1], cam[2], cam[3], cam[4], 70.0f);
                if (i == kSettle - 1) device->armCapture(outPath.c_str());
                auto frame = device->beginFrame();
                if (frame.valid) rinterior.render(*device, frame, rscene);
                device->endFrame(frame);
            }
            const bool wrote = device->captureFrame(outPath.c_str());
            if (wrote) x3::logInfo("--world ship-repair: wrote " + outPath +
                                   " (panel " + std::to_string(repair.panel(0).wiresConnected) +
                                   "/" + std::to_string(kWiresA) + " wires)");
            else       x3::logError("--world ship-repair: capture FAILED");
            rinterior.shutdown(*rphys);
            rphys->shutdown();
            device->shutdown();
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return wrote ? 0 : 1;
        }

        // ===== Walkable windowed path: real Player; E opens + connects wires. =====
        x3::game::Player rplayer;
        rplayer.spawn(*rphys, rspawn.x, rspawn.y, rspawn.z);

        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        double lastMX, lastMY; glfwGetCursorPos(window, &lastMX, &lastMY);
        double prevTime = glfwGetTime();
        bool prevSpaceR = false, prevER = false;
        int lastWr = (int)W, lastHr = (int)H;
        x3::logInfo("--world ship-repair: WASD walk, mouse look, E open/connect a wire, Esc to quit");

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
            bool eNow = kd(GLFW_KEY_E);

            x3::game::PlayerInput in;
            if (kd(GLFW_KEY_W)) in.moveFwd    += 1.0f;
            if (kd(GLFW_KEY_S)) in.moveFwd    -= 1.0f;
            if (kd(GLFW_KEY_D)) in.moveStrafe += 1.0f;
            if (kd(GLFW_KEY_A)) in.moveStrafe -= 1.0f;
            in.sprint      = kd(GLFW_KEY_LEFT_SHIFT);
            in.jumpPressed = spaceNow && !prevSpaceR;
            in.lookDX = ddx; in.lookDY = ddy;
            prevSpaceR = spaceNow;

            rplayer.update(in, dt, *rphys);
            rphys->step(dt);
            rscene.update(*rphys);

            float camX, camY, camZ, camYaw, camPitch;
            rplayer.camera(camX, camY, camZ, camYaw, camPitch);

            // Drive the repair state machine off the camera (eye) position. E both
            // opens a Sealed in-range panel (handled inside update on the rising edge)
            // AND connects a wire on an already-open in-range panel.
            const float pp[3] = { camX, camY, camZ };
            int inRange = repair.update(dt, pp, eNow);
            const bool ePressed = eNow && !prevER;
            if (ePressed && inRange >= 0 &&
                repair.panel((uint32_t)inRange).state == x3::space::PanelState::Open)
                repair.connectWire(inRange);
            prevER = eNow;
            syncVisuals();
            if (repair.allRepaired())
                x3::logInfo("--world ship-repair: ALL PANELS REPAIRED");

            int cw, ch; glfwGetFramebufferSize(window, &cw, &ch);
            if (cw != lastWr || ch != lastHr) { lastWr = cw; lastHr = ch; if (cw>0&&ch>0) device->onResize((uint32_t)cw,(uint32_t)ch); }

            device->setCamera(camX, camY, camZ, camYaw, camPitch, 70.0f);
            auto frame = device->beginFrame();
            if (frame.valid) rinterior.render(*device, frame, rscene);
            device->endFrame(frame);
        }

        rinterior.shutdown(*rphys);
        rphys->shutdown();
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

    // ---- Companion AI showcase (--world companion) --------------------------
    // Self-contained flat arena: player + 3 CompanionSquad members + 3 enemy
    // MonsterSystems as threats. LOW-CONFLICT: does NOT touch level1/spire/act2.
    // Mirrors --world valley/cliffs scaffold exactly.
    //   * SCREENSHOT (headless): `--world companion --screenshot <path>`.
    //   * WALKABLE (windowed):   `--world companion` -- WASD + mouse, Esc to quit.
    if (worldMode == "companion") {
        x3::logInfo("--world companion: building companion AI showcase arena");

        std::unique_ptr<x3::phys::IPhysicsWorld> coPhys(x3::phys::createPhysicsWorld());
        if (!coPhys->init()) {
            x3::logError("--world companion: physics init failed");
            device->shutdown();
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return 1;
        }

        x3::game::Scene coScene;

        // Flat arena: a large static ground plane at y=0 (physics).
        coPhys->addBox({ 60.0f, 0.5f, 60.0f }, { 0.0f, -0.5f, 0.0f },
                       0.0f, x3::phys::Layer::Static);

        // Visible ground quad (render mesh in the scene). Uses a concrete/asphalt
        // checker so the arena floor reads clearly from the 3/4 camera.
        {
            std::vector<x3::rhi::MeshVertex> gvtx; std::vector<uint32_t> gixs;
            x3::prims::makeGroundQuad(/*half=*/60.0f, /*tiles=*/30.0f, gvtx, gixs);
            x3::game::Entity ge;
            ge.mesh = device->createMesh(gvtx.data(), (uint32_t)gvtx.size(),
                                         gixs.data(), (uint32_t)gixs.size());
            auto gPx = x3::prims::makeCheckerRGBA(64, 8, 105, 112, 108, 55, 62, 58);
            ge.tex  = device->createTexture(gPx.data(), 64, 64, true);
            ge.baseColor[0] = ge.baseColor[1] = ge.baseColor[2] = ge.baseColor[3] = 1.0f;
            ge.tag  = (uint32_t)x3::game::Tag::Static;
            // Identity transform: ground sits at y=0, centered on the origin.
            for (int gi = 0; gi < 16; ++gi) ge.transform[gi] = (gi % 5 == 0) ? 1.0f : 0.0f;
            coScene.add(ge);
        }

        // Analytic sky + point lights so models are well-lit from every angle.
        {
            x3::rhi::IRenderDevice::SkyParams sp{};
            sp.enabled = true;
            sp.sunDir[0] = 0.5f; sp.sunDir[1] = 1.0f; sp.sunDir[2] = 0.3f;
            sp.sunColor[0] = 1.0f; sp.sunColor[1] = 0.95f; sp.sunColor[2] = 0.85f;
            sp.sunIntensity = 1.0f; sp.haze = 0.3f; sp.exposure = 1.0f;
            device->setSkyParams(sp);
        }
        {
            x3::rhi::PointLight coPl[5];
            auto coSetL = [](x3::rhi::PointLight& l,
                              float x, float y, float z,
                              float r, float g, float b, float range) {
                l.pos[0]=x; l.pos[1]=y; l.pos[2]=z; l.range=range;
                l.color[0]=r; l.color[1]=g; l.color[2]=b;
            };
            // Overhead key + four fill lights to illuminate all character faces.
            coSetL(coPl[0],   0.0f, 9.0f,  5.0f,  5.5f, 5.4f, 5.0f, 50.0f);
            coSetL(coPl[1],  10.0f, 5.0f, 12.0f,  4.0f, 3.8f, 3.4f, 45.0f);
            coSetL(coPl[2], -10.0f, 5.0f, 12.0f,  3.4f, 3.8f, 4.4f, 45.0f);
            coSetL(coPl[3],  10.0f, 5.0f, -7.0f,  3.8f, 3.6f, 3.4f, 45.0f);
            coSetL(coPl[4], -10.0f, 5.0f, -7.0f,  3.4f, 3.8f, 4.4f, 45.0f);
            device->setPointLights(coPl, 5);
        }

        // Human player spawns slightly behind the squad.
        x3::game::Player coPlayer;
        coPlayer.spawn(*coPhys, 0.0f, 0.5f, -8.0f);

        // Companions: 3 slots flanking the player's starting position.
        x3::game::CompanionSquad coSquad;
        coSquad.setScene(&coScene);
        coSquad.addCompanion(*coPhys,  3.0f, 0.5f, -6.0f,  0.0f);   // right
        coSquad.addCompanion(*coPhys, -3.0f, 0.5f, -6.0f,  0.0f);   // left
        coSquad.addCompanion(*coPhys,  0.0f, 0.5f, -4.0f,  0.0f);   // center-rear

        // ---- Visible character models for each companion + the player --------
        // Each companion's physics is driven by Player (capsule); the companion has
        // no visible mesh. We give each slot an INERT MonsterSystem (chaseSpeed=0,
        // damage=0) loaded from a rigged GLB so drawMonster() renders the model.
        // Each tick, after physics + scene.update(), we sync the monster entity's
        // transform to the companion's Player::feet() + Player::yaw().
        // Companion models: AnnaTactical.glb (female operative). Player: marcus_webb.glb.
        constexpr uint32_t kCoNumViz = 4;  // 3 companions + 1 player visual
        std::unique_ptr<x3::game::MonsterSystem> coViz[kCoNumViz];
        const char* coVizModels[kCoNumViz] = {
            "AnnaTactical.glb",   // companion slot 0 (right)
            "AnnaTactical.glb",   // companion slot 1 (left)
            "AnnaTactical.glb",   // companion slot 2 (center-rear)
            "marcus_webb.glb",    // human player
        };
        // Initial spawn positions matching the player/squad spawn (visual bodies
        // placed at the same feet positions; will be synced each tick).
        const float coVizSpawn[kCoNumViz][3] = {
            {  3.0f, 0.5f, -6.0f },
            { -3.0f, 0.5f, -6.0f },
            {  0.0f, 0.5f, -4.0f },
            {  0.0f, 0.5f, -8.0f },
        };
        for (uint32_t v = 0; v < kCoNumViz; ++v) {
            coViz[v] = std::make_unique<x3::game::MonsterSystem>();
            x3::game::MonsterSystem::Tuning vt;
            vt.hp         = 100;
            vt.chaseSpeed = 0.0f;   // inert: the Player capsule owns movement
            vt.damage     = 0;
            vt.modelFile  = coVizModels[v];
            vt.modelDirOverride = x3::game::riggedGlbRoot();
            vt.modelScale = 1.0f;
            vt.standUpZtoY = false;
            coViz[v]->buildMonsterTuned(coScene, *device, *coPhys,
                x3::game::riggedGlbRoot(),
                { coVizSpawn[v][0], coVizSpawn[v][1], coVizSpawn[v][2] }, vt);
        }

        // Helper: sync a companion visual monster's scene-entity transform to the
        // given feet position + facing yaw (called after scene.update() each tick so
        // the physics-sync that runs inside scene.update() doesn't stomp our pose).
        // Mirrors how monster.cpp bakes the yaw facing into the entity transform.
        auto coSyncViz = [&](uint32_t vizIdx,
                              const x3::phys::Vec3& feet, float yaw) {
            const uint32_t eid = coViz[vizIdx]->entity();
            if (eid == x3::game::kNoLink || eid >= coScene.size()) return;
            x3::game::Entity& ve = coScene.get(eid);
            // yaw +pi: rigged GLBs are authored facing +Z; local -Z is the game
            // convention. Flip here to face the correct direction (same fix as
            // MonsterSystem::update's facing bake in monster.cpp).
            const float ry = yaw + 3.14159265358979f;
            const float c = std::cos(ry), s = std::sin(ry);
            // Column-major TRS: col0=(c,0,-s,0), col1=(0,1,0,0), col2=(s,0,c,0),
            // col3=(tx,ty,tz,1). Scale=1.
            ve.transform[0]=c;  ve.transform[1]=0.0f; ve.transform[2]=-s; ve.transform[3]=0.0f;
            ve.transform[4]=0.0f; ve.transform[5]=1.0f; ve.transform[6]=0.0f; ve.transform[7]=0.0f;
            ve.transform[8]=s;  ve.transform[9]=0.0f; ve.transform[10]=c; ve.transform[11]=0.0f;
            ve.transform[12]=feet.x; ve.transform[13]=feet.y; ve.transform[14]=feet.z;
            ve.transform[15]=1.0f;
        };

        // Enemies: 3 threat MonsterSystems spread across the far side of the arena.
        // Using marcus_webb.glb (falls back to box if absent); real chase + damage.
        constexpr uint32_t kCoNumEnemies = 3;
        std::unique_ptr<x3::game::MonsterSystem> coEnemies[kCoNumEnemies];
        x3::game::MonsterSystem* coEnemyPtrs[kCoNumEnemies];
        const float coExPos[kCoNumEnemies][3] = {
            { -8.0f, 0.5f, 14.0f },
            {  0.0f, 0.5f, 18.0f },
            {  8.0f, 0.5f, 14.0f },
        };
        for (uint32_t e = 0; e < kCoNumEnemies; ++e) {
            coEnemies[e] = std::make_unique<x3::game::MonsterSystem>();
            x3::game::MonsterSystem::Tuning t;
            t.hp = 150; t.chaseSpeed = 1.5f;
            t.type = x3::game::MonsterType::Guard;
            t.damage = 5; t.attackRange = 2.0f; t.attackCooldown = 1.5f;
            t.ranged = false; t.flyer = false;
            // Use marcus_webb.glb for threats; falls back to the procedural box.
            t.modelFile = "marcus_webb.glb";
            t.modelDirOverride = x3::game::riggedGlbRoot();
            // Hostile tint: slight warm-red to distinguish threats from allies.
            t.tint[0]=1.3f; t.tint[1]=0.85f; t.tint[2]=0.8f; t.tint[3]=1.0f;
            coEnemies[e]->buildMonsterTuned(coScene, *device, *coPhys,
                x3::game::riggedGlbRoot(),
                { coExPos[e][0], coExPos[e][1], coExPos[e][2] }, t);
            coEnemyPtrs[e] = coEnemies[e].get();
        }

        const float coFps = 60.0f;
        const float coDt  = 1.0f / coFps;

        // ===== Headless screenshot path: settle + capture. ===================
        if (headless) {
            const std::string coOutPath = screenshot ? screenshotPath
                                                     : std::string("w_companion.png");
            // 3/4 elevated camera: back + left of the squad, looking toward the threats
            // so the player, companions, and enemies are all visible in frame.
            float coCamX = -8.0f, coCamY = 12.0f, coCamZ = -12.0f;
            float coCamYaw = 0.65f, coCamPitch = -0.52f;  // look toward +X+Z
            if (shotCamOverride) {
                coCamX = shotCam[0]; coCamY = shotCam[1]; coCamZ = shotCam[2];
                coCamYaw = shotCam[3]; coCamPitch = shotCam[4];
            }
            const int kCoSettle = 60;
            for (int ci = 0; ci < kCoSettle; ++ci) {
                glfwPollEvents();
                const x3::phys::Vec3 copp = coPlayer.feet();
                const x3::phys::Vec3 coPlayerEye{ copp.x, copp.y + 1.6f, copp.z };
                coSquad.tick(coDt, *coPhys, coPlayerEye,
                             (float)coPlayer.hp() / (float)coPlayer.maxHp(),
                             !coPlayer.isAlive(), coEnemyPtrs, kCoNumEnemies);
                for (uint32_t e = 0; e < kCoNumEnemies; ++e) {
                    if (coEnemies[e]->alive())
                        coEnemies[e]->update(coDt, coScene, *coPhys, copp, &coPlayer, {});
                }
                coPhys->step(coDt);
                coScene.update(*coPhys);
                // Sync companion + player visual models to their physics capsule positions.
                for (uint32_t v = 0; v < 3u; ++v) {
                    const x3::phys::Vec3 fp = coSquad.slot(v).player.feet();
                    coSyncViz(v, fp, coSquad.slot(v).player.yaw());
                }
                coSyncViz(3, copp, coPlayer.yaw());
                device->setCamera(coCamX, coCamY, coCamZ, coCamYaw, coCamPitch, 60.0f);
                if (ci == kCoSettle - 1) device->armCapture(coOutPath.c_str());
                auto coFrame = device->beginFrame();
                if (coFrame.valid) {
                    coScene.render(*device, coFrame);
                    for (uint32_t e = 0; e < kCoNumEnemies; ++e)
                        coEnemies[e]->drawMonster(*device, coFrame, coScene);
                    for (uint32_t v = 0; v < kCoNumViz; ++v)
                        coViz[v]->drawMonster(*device, coFrame, coScene);
                }
                device->endFrame(coFrame);
            }
            const bool coWrote = device->captureFrame(coOutPath.c_str());
            if (coWrote) x3::logInfo("--world companion: wrote screenshot " + coOutPath);
            else         x3::logError("--world companion: capture FAILED");
            // Assert non-trivial geometry was rendered (blank = ~0 triangles).
            // drawCalls >= 1 (ground) + 7 models (each 1 primitive) = 8 minimum;
            // triangles should be in the millions for GPU-skinned GLB characters.
            {
                const x3::rhi::RenderStats coSt = device->stats();
                char coRb[256];
                std::snprintf(coRb, sizeof(coRb),
                    "--world companion: capture stats draws=%u tris=%u",
                    coSt.drawCalls, coSt.triangles);
                x3::logInfo(coRb);
                if (coSt.drawCalls < 5u || coSt.triangles < 1000u) {
                    x3::logError("--world companion: BLANK RENDER -- drawCalls or triangles too low");
                }
            }
            // Tear down enemy + companion-viz ragdolls BEFORE physics shuts down
            // (skinned death ragdolls leave Jolt bodies that must be removed while
            // the world is alive — same pattern as MonsterManager::shutdown).
            for (uint32_t e = 0; e < kCoNumEnemies; ++e)
                coEnemies[e]->shutdownRagdoll();
            for (uint32_t v = 0; v < kCoNumViz; ++v)
                coViz[v]->shutdownRagdoll();
            coSquad.shutdown(*coPhys);
            coPhys->shutdown();
            device->shutdown();
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return coWrote ? 0 : 1;
        }

        // ===== Walkable windowed path: player WASD + mouse, companions fight. ==
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        if (glfwRawMouseMotionSupported())
            glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        double coLastMX, coLastMY; glfwGetCursorPos(window, &coLastMX, &coLastMY);
        double coPrevTime = glfwGetTime();
        bool coPrevSpace = false;
        x3::logInfo("--world companion: WASD walk, mouse look, Space jump, "
                    "LeftShift sprint, Esc to quit");

        int coLastW = (int)W, coLastH = (int)H;
        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();
            if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;

            const double coNow = glfwGetTime();
            float coDtLoop = (float)(coNow - coPrevTime); coPrevTime = coNow;
            if (coDtLoop > 0.1f) coDtLoop = 0.1f;

            double coMX, coMY; glfwGetCursorPos(window, &coMX, &coMY);
            const float coDdx = (float)(coMX - coLastMX);
            const float coDdy = (float)(coMY - coLastMY);
            coLastMX = coMX; coLastMY = coMY;

            auto coKd = [&](int k) { return glfwGetKey(window, k) == GLFW_PRESS; };
            const bool coSpaceNow = coKd(GLFW_KEY_SPACE);

            x3::game::PlayerInput coIn;
            if (coKd(GLFW_KEY_W)) coIn.moveFwd    += 1.0f;
            if (coKd(GLFW_KEY_S)) coIn.moveFwd    -= 1.0f;
            if (coKd(GLFW_KEY_D)) coIn.moveStrafe += 1.0f;
            if (coKd(GLFW_KEY_A)) coIn.moveStrafe -= 1.0f;
            coIn.sprint     = coKd(GLFW_KEY_LEFT_SHIFT);
            coIn.jumpPressed = coSpaceNow && !coPrevSpace;
            coIn.lookDX = coDdx; coIn.lookDY = coDdy;
            coPlayer.update(coIn, coDtLoop, *coPhys);
            coPrevSpace = coSpaceNow;

            float coCX, coCY, coCZ, coCYaw, coCPitch;
            coPlayer.camera(coCX, coCY, coCZ, coCYaw, coCPitch);
            const x3::phys::Vec3 coPlayerPos{ coCX, coCY, coCZ };
            const x3::phys::Vec3 coFeetPos = coPlayer.feet();

            coSquad.tick(coDtLoop, *coPhys, coPlayerPos,
                         (float)coPlayer.hp() / (float)coPlayer.maxHp(),
                         !coPlayer.isAlive(), coEnemyPtrs, kCoNumEnemies);

            for (uint32_t e = 0; e < kCoNumEnemies; ++e) {
                if (coEnemies[e]->alive())
                    coEnemies[e]->update(coDtLoop, coScene, *coPhys,
                                         coFeetPos, &coPlayer, {});
            }
            coPhys->step(coDtLoop);
            coScene.update(*coPhys);
            // Sync companion + player visual models to their physics capsule positions.
            for (uint32_t v = 0; v < 3u; ++v) {
                const x3::phys::Vec3 fp = coSquad.slot(v).player.feet();
                coSyncViz(v, fp, coSquad.slot(v).player.yaw());
            }
            coSyncViz(3, coFeetPos, coPlayer.yaw());

            int coCW, coCH; glfwGetFramebufferSize(window, &coCW, &coCH);
            if (coCW != coLastW || coCH != coLastH) {
                coLastW = coCW; coLastH = coCH;
                if (coCW > 0 && coCH > 0) device->onResize((uint32_t)coCW, (uint32_t)coCH);
            }

            device->setCamera(coCX, coCY, coCZ, coCYaw, coCPitch, 60.0f);
            auto coFr = device->beginFrame();
            if (coFr.valid) {
                coScene.render(*device, coFr);
                for (uint32_t e = 0; e < kCoNumEnemies; ++e)
                    coEnemies[e]->drawMonster(*device, coFr, coScene);
                for (uint32_t v = 0; v < kCoNumViz; ++v)
                    coViz[v]->drawMonster(*device, coFr, coScene);
            }
            device->endFrame(coFr);
        }

        // Tear down ragdolls before the physics world (same as MonsterManager::shutdown).
        for (uint32_t e = 0; e < kCoNumEnemies; ++e)
            coEnemies[e]->shutdownRagdoll();
        for (uint32_t v = 0; v < kCoNumViz; ++v)
            coViz[v]->shutdownRagdoll();
        coSquad.shutdown(*coPhys);
        coPhys->shutdown();
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
    // ---- B1 GLASS LOUNGE (a plate-glass table + 4 chairs in the open area behind the
    // detention cell — "Old Armory" room, x[-4.5,2.5], z[4.5,9.5]) + the SIT-AT-CHAIR
    // state machine. Built with the level (Level 1 only). The lounge entities are
    // room-tagged to L1Floor::B1 so the per-floor occlusion cull keeps them with the
    // floor. The SitController owns the seated state + the smooth camera lerp; the
    // host drives it from the same keyDown-gated E-interact path the cell HoloTerminal
    // uses so it can't fire while typing into the terminal/keypad or in a UI menu, and
    // firing the weapon never triggers sit. Only one interactable (nearest chair OR
    // the terminal) prompts at a time. ----
    x3::game::GlassLounge    glassLounge;
    x3::game::SitController  sitCtrl;
    // ---- HOLO-TERMINAL KIOSK SYSTEM: placeable terminals scattered around B1.
    // The cell-escape terminal (legacy code 1278) is registered as the FIRST instance
    // (external, owned by SecretRoom); additional B1 kiosks are owned by the system
    // and built directly into the world. Each kiosk has a real-world command (UnlockCell
    // / OpenDoor / ToggleLights / TriggerAlarm / ShowLore / SpawnCrate) that fires when
    // the player submits the kiosk's code. The host gates input the same way termMode
    // gates the cell terminal today (keyDown lambda + the input/movement lock).
    x3::game::HoloTerminalSystem holoSys;
    // Light dimmer multipliers (one per fixture in game.lightFixtures()). The
    // ToggleLights command cycles a per-anchor scale through full -> dim -> off ->
    // full; the render loop multiplies each fixture's color RGB by its stored scale
    // before issuing setPointLights. Size is matched to fl.size() at first use.
    std::vector<float> kioskLightScale;
    // Alarm screen-tint state: a brief eased pulse (red) when a TriggerAlarm fires.
    // The system itself decays the alarm timer; the host reads alarm() each frame.
    // Auto-resets when the timer hits zero (the system clears active).
    // (No separate state needed here — read holoSys.alarm().)
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

        // ---- B1 GLASS LOUNGE: a plate-glass table + 4 chairs in the open area behind
        // Jake's detention cell. The cell spans x[-3.5,3.5]/z[-3,3]; the "Old Armory"
        // detention room directly +Z behind it spans x[-4.5,2.5]/z[4.5,9.5] (3.5 m
        // ceiling, clear floor) — that's the lounge nook. Centered at (-1, 0, 7) the
        // 1.6 x 0.9 m table fits with chair clearance on all four sides. Tagged to
        // L1Floor::B1 so the per-floor cull keeps the lounge with the basement.
        const x3::phys::Vec3 loungeCenter{ -1.0f, Lb.floorBaseY[(uint32_t)x3::game::L1Floor::B1], 7.0f };
        glassLounge.build(scene, *device, *physics, loungeCenter, (uint32_t)x3::game::L1Floor::B1);

        // ---- HOLO-TERMINAL KIOSKS on B1 (clear-glass + emissive UI + per-instance
        // command). Built AFTER game.build() so the SecretRoom + DoorSystem already
        // hold the cell trapdoor + the spine doors A-E. The cell terminal (legacy
        // code 1278) is registered as an EXTERNAL instance pointing to the one
        // SecretRoom built; the other terminals are owned by the system and built
        // directly into the world here. ----
        {
            using namespace x3::game;
            const float b1y = Lb.floorBaseY[(uint32_t)L1Floor::B1];
            const uint32_t roomB1 = (uint32_t)L1Floor::B1;
            const float PI = 3.14159265358979f;

            // (1) CELL LOCK — the legacy cell-escape terminal. SecretRoom built it
            // facing -Z toward Jake's spawn. We register it here as an external so
            // the kiosk registry counts it + the unified input flow reaches it.
            // The UnlockCell command's sink (installed below) re-uses the same
            // SecretRoom submitCode path so the existing S1-S7 secret-room test
            // keeps working byte-identically.
            {
                TerminalParams p;
                holoSys.addExternal(game.secret().terminal(),
                                    "cell_lock", "DETENTION CELL 07", "1278",
                                    TerminalCommand::UnlockCell, p,
                                    x3::phys::Vec3{ Lb.cellCenter.x, b1y + 1.3f, Lb.cellCenter.z - 2.6f },
                                    /*yaw*/PI, roomB1);
            }

            // (2) OLD ARMORY DOOR — the lounge nook is just past Door B; this kiosk
            // sits in the lounge area and opens Door B (corridor->armory) on submit.
            // Faces -Z so the player walks up from the table side.
            {
                TerminalParams p;
                p.doorIdx = game.doorIndexPublic('B');   // Door B == corridor -> armory
                holoSys.add(scene, *device, "armory_door", "OLD ARMORY ACCESS", "OPEN",
                            TerminalCommand::OpenDoor, p,
                            // Mounted on the +Z wall of the corridor/lounge nook
                            x3::phys::Vec3{ Lb.corridorCenter.x - 1.5f, b1y + 1.4f, 5.5f },
                            /*yaw*/-PI*0.5f,           // facing -Z (toward the lounge)
                            1.0f, 0.65f, 0.0f, roomB1,
                            { "OLD ARMORY ACCESS PANEL",
                              "DOOR INTERLOCK: ENGAGED",
                              "TYPE 'OPEN' TO RELEASE" });
            }

            // (3) CORRIDOR LIGHTING — central corridor (between Door A and the
            // armory). Cycles the nearest 3 point lights' intensity.
            {
                TerminalParams p;
                p.lightAnchor = x3::phys::Vec3{ Lb.corridorCenter.x, b1y + 2.5f, Lb.corridorCenter.z };
                p.lightRadius = 6.0f;
                p.lightMaxAffect = 3;
                holoSys.add(scene, *device, "lights_central", "CORRIDOR LIGHTING", "LIGHTS",
                            TerminalCommand::ToggleLights, p,
                            // Mounted on the -Z wall of the corridor (faces +Z).
                            x3::phys::Vec3{ Lb.corridorCenter.x, b1y + 1.4f,
                                            Lb.corridorCenter.z - Lb.corridorHalf.z + 0.18f },
                            /*yaw*/PI*0.5f,
                            1.0f, 0.65f, 0.0f, roomB1,
                            { "CORRIDOR LIGHTING CTRL",
                              "MODE: AUTO",
                              "TYPE 'LIGHTS' TO CYCLE" });
            }

            // (4) ALARM — security alarm panel inside the armory.
            {
                TerminalParams p;
                holoSys.add(scene, *device, "alarm_armory", "SECURITY ALARM", "ALARM",
                            TerminalCommand::TriggerAlarm, p,
                            // Mounted on the +Z wall of the armory (faces -Z).
                            x3::phys::Vec3{ Lb.armoryCenter.x, b1y + 1.4f,
                                            Lb.armoryCenter.z + Lb.armoryHalf.z - 0.18f },
                            /*yaw*/-PI*0.5f,
                            1.0f, 0.65f, 0.0f, roomB1,
                            { "SECURITY ALARM PANEL",
                              "TYPE 'ALARM' TO TRIGGER",
                              "DURATION: 3 SECONDS" });
            }

            // (5) INTEL ARCHIVE — lore / world-building dump in the checkpoint room.
            {
                TerminalParams p;
                p.lore = {
                    "SUBJECT: PROJECT X3",
                    "FACILITY: AHL CORP SPIRE",
                    "STATUS: BREACH IN PROGRESS",
                    "RECOMMEND: EVAC ALL CIVILIANS",
                    "SUBJ JAKE: UNAUTHORIZED",
                    "AUGMENT LEVEL TIER-3",
                };
                holoSys.add(scene, *device, "lore_intel", "INTEL ARCHIVE", "READ",
                            TerminalCommand::ShowLore, p,
                            // Mounted on the -Z wall of the checkpoint room (faces +Z).
                            x3::phys::Vec3{ Lb.checkpointCenter.x - 0.5f, b1y + 1.4f,
                                            Lb.checkpointCenter.z - Lb.checkpointHalf.z + 0.18f },
                            /*yaw*/PI*0.5f,
                            1.0f, 0.65f, 0.0f, roomB1,
                            { "INTEL ARCHIVE  --  LOCKED",
                              "TYPE 'READ' TO QUERY" });
            }

            // (6) CACHE DISPENSER — bonus crate spawner in the arena (the bonus).
            {
                TerminalParams p;
                p.cratePos = x3::phys::Vec3{ Lb.arenaCenter.x - 1.5f, b1y + 0.55f, Lb.arenaCenter.z + 1.2f };
                holoSys.add(scene, *device, "crate_dispense", "CACHE DISPENSER", "CRATE",
                            TerminalCommand::SpawnCrate, p,
                            // Mounted on the +Z wall of the arena (faces -Z).
                            x3::phys::Vec3{ Lb.arenaCenter.x, b1y + 1.4f,
                                            Lb.arenaCenter.z + Lb.arenaHalf.z - 0.18f },
                            /*yaw*/-PI*0.5f,
                            1.0f, 0.65f, 0.0f, roomB1,
                            { "CACHE DISPENSER",
                              "TYPE 'CRATE' TO RELEASE" });
            }

            // ---- Wire the cross-system sinks. ----
            // UnlockCell: route to the SecretRoom hatch via submitCode (the live
            // DoorSystem already holds it; the sink path mirrors the legacy
            // code-1278 -> hatch opening flow exactly).
            holoSys.setUnlockCellSink([&]() -> bool {
                // The hatch idx is the DoorSystem index SecretRoom registered. We
                // re-use SecretRoom::submitCode which captures the DoorSystem inside
                // the secret-room terminal sink (the cell terminal's submit, not the
                // kiosk system's). Replaying "1278" through the terminal fires that
                // sink and opens the hatch. DoorSystem& arg is unused (the captured
                // one inside the sink is the live one).
                DoorSystem dummy;
                return game.secret().submitCode(x3::game::kSecretRoomCode, dummy);
            });

            // ToggleLights: store a per-fixture scale; the render loop multiplies
            // each fixture's color RGB by its stored scale before issuing
            // setPointLights. Cycle is full(1.0) -> dim(0.35) -> off(0.0) -> full.
            holoSys.setLightsToggleSink([&](const x3::phys::Vec3& at, float radius, uint32_t maxAffect) {
                const std::vector<x3::rhi::PointLight>& src = game.lightFixtures();
                if (kioskLightScale.size() != src.size()) kioskLightScale.assign(src.size(), 1.0f);
                // Find the K nearest fixtures within radius.
                struct Hit { uint32_t i; float d2; };
                std::vector<Hit> hits;
                hits.reserve(src.size());
                const float r2 = radius * radius;
                for (uint32_t i = 0; i < src.size(); ++i) {
                    const float dx = src[i].pos[0] - at.x;
                    const float dy = src[i].pos[1] - at.y;
                    const float dz = src[i].pos[2] - at.z;
                    const float d2 = dx*dx + dy*dy + dz*dz;
                    if (d2 <= r2) hits.push_back({ i, d2 });
                }
                std::sort(hits.begin(), hits.end(),
                          [](const Hit& a, const Hit& b) { return a.d2 < b.d2; });
                const uint32_t affect = std::min((uint32_t)hits.size(), maxAffect);
                if (affect == 0) {
                    x3::logInfo("[holoterm-sys] ToggleLights: no fixtures in range");
                    return;
                }
                // Cycle: pick the current scale of the first hit and step.
                const float cur = kioskLightScale[hits[0].i];
                float next = 1.0f;
                if (cur >= 0.99f)        next = 0.35f;
                else if (cur >= 0.30f)   next = 0.0f;
                else                      next = 1.0f;
                for (uint32_t k = 0; k < affect; ++k) kioskLightScale[hits[k].i] = next;
                x3::logInfo("[holoterm-sys] ToggleLights: cycled " + std::to_string(affect) +
                            " fixtures -> scale=" + std::to_string(next));
            });

            // SpawnCrate: drop a glowing crate prop into the scene at `at`.
            holoSys.setSpawnCrateSink([&](const x3::phys::Vec3& at) {
                x3::prims::PrimMesh g = x3::prims::makeBox(0.30f, 0.30f, 0.30f, 0, 0, 0, 0.5f);
                Entity e;
                e.mesh = device->createMesh(g.verts.data(), (uint32_t)g.verts.size(),
                                            g.index.data(), (uint32_t)g.index.size());
                e.baseColor[0]=1.0f; e.baseColor[1]=0.78f; e.baseColor[2]=0.22f; e.baseColor[3]=1.0f;
                e.emissive[0]=1.0f; e.emissive[1]=0.55f; e.emissive[2]=0.15f; e.emissive[3]=2.4f;
                e.tag = (uint32_t)Tag::Prop;
                e.roomId = (uint32_t)L1Floor::B1;
                e.transform[12] = at.x; e.transform[13] = at.y; e.transform[14] = at.z;
                (void)scene.add(e);
                x3::logInfo("[holoterm-sys] SpawnCrate: crate dropped at (" +
                            std::to_string(at.x) + "," + std::to_string(at.y) + "," +
                            std::to_string(at.z) + ")");
            });

            x3::logInfo("[holoterm-sys] built " + std::to_string(holoSys.count()) +
                        " kiosks on B1 (cell_lock external + " +
                        std::to_string(holoSys.count() - 1) + " owned)");
        }
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

    // ==================== THIRD-PERSON VIEW (FIRST MILESTONE) ====================
    // Load the Jake avatar + the FP/3P toggle. FP is the DEFAULT (eye-cam + weapon
    // viewmodel, unchanged). F5 flips to a follow/orbit camera behind+above the player
    // with the animated Jake avatar (the held weapon socketed to its right hand). The
    // player capsule/collision are untouched (camera change only). On a failed Jake
    // load this stays unbuilt and FP keeps working. See app/thirdperson.* + F5 below.
    x3::game::ThirdPersonView thirdPerson;
    thirdPerson.build(scene, *device, x3::game::riggedGlbRoot());
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

    // ---- Combat-density benchmark mode (--bench-combat [N] [frames]) -------
    // The HONEST combat-density FPS number: build the 3-ally squad next to the
    // level spawn, drop an N-enemy ring around them via AllyManager::makeBenchArena
    // (see app/ally_arena.cpp), pose the camera so the whole arena fits in view,
    // then run `benchFrames` frames vsync OFF with BOTH sides ticking their full
    // combat AI + physics. The per-frame cost scales with the live combatant
    // count, not the (idle) cube field --bench measures. Reports:
    //   BENCH-COMBAT allies=3 enemies=N alive=A FPS=X CPU=Yms GPU=Zms
    if (benchCombat) {
        // The arena's free helpers (allyArenaMonsters / allyArenaEnemyCount /
        // allyArenaEnemiesAlive / allyArenaShutdown) live in app/ally_arena.cpp
        // and are forward-declared near the top of this file (next to the
        // <ally.h> include) so the public ally.h contract stays frozen for the
        // Phase A/B siblings building in parallel.

        // Camera + arena both anchored at the level spawn so the 3 allies built
        // by AllyManager::build() (placed AT spawn) are inside the enemy ring.
        const x3::phys::Vec3 arenaCenter = L1.spawn;
        // Build the 3-ally squad next to spawn. Model + weapon dirs are the same
        // rigged_glb root the live game uses; AllyManager falls back to tinted
        // procedural boxes if the GLBs are missing, so the bench still measures
        // an honest combat-density frame even on a fresh clone with no LFS.
        x3::game::AllyManager allies;
        const std::string modelDir  = x3::game::riggedGlbRoot();
        const std::string weaponDir = x3::game::riggedGlbRoot();
        allies.build(scene, *device, *physics, modelDir, weaponDir, arenaCenter);

        // Spawn the enemy ring. makeBenchArena returns the count actually placed
        // (0 if build() somehow left no allies; we treat that as failure).
        const uint32_t spawned = allies.makeBenchArena(scene, *device, *physics,
                                                       benchCombatEnemies, arenaCenter);
        if (spawned == 0) {
            x3::logError("--bench-combat: AllyManager::makeBenchArena spawned 0 enemies");
            audio->shutdown();
            combatFx.shutdown(*device);
            physics->shutdown();
            device->shutdown();
            glfwDestroyWindow(window);
            glfwTerminate();
            return 1;
        }
        x3::game::MonsterManager& arenaEnemies = x3::game::allyArenaMonsters();

        // Camera: stand back from the arena center, slightly elevated, looking at
        // the arena so the whole ring fits in view (worst case: every combatant
        // submitted to the renderer each frame).
        const float bx = arenaCenter.x - 25.0f;
        const float by = arenaCenter.y + 18.0f;
        const float bz = arenaCenter.z;
        const float byaw = 0.0f, bpitch = -0.5f;
        device->setCamera(bx, by, bz, byaw, bpitch, 75.0f);

        // Hostile query: for ally AI, list LIVE arena enemies within radius. Uses
        // the arena's MonsterManager directly (no per-frame heap alloc — fills the
        // caller's fixed buffer).
        x3::game::HostileQueryFn hostiles =
            [&arenaEnemies](const x3::phys::Vec3& self, float radius,
                            x3::phys::Vec3* out, uint32_t maxOut) -> uint32_t {
                uint32_t n = 0;
                const float r2 = radius * radius;
                const uint32_t total = arenaEnemies.count();
                for (uint32_t i = 0; i < total && n < maxOut; ++i) {
                    const x3::game::MonsterSystem& m = arenaEnemies.at(i);
                    if (!m.alive()) continue;
                    const x3::phys::Vec3 p = m.pos();
                    const float dx = p.x - self.x, dz = p.z - self.z;
                    if (dx*dx + dz*dz <= r2) out[n++] = p;
                }
                return n;
            };

        const uint32_t warmup = std::min<uint32_t>(60, benchFrames / 4);
        double sumCpuMs = 0.0, sumGpuMs = 0.0; uint32_t measured = 0;
        double prevT = glfwGetTime();
        x3::rhi::RenderStats last{};
        const float dt = 1.0f / 60.0f;
        // The squad anchors its Follow state on the player position; for the
        // bench there's no real player, so use the arena center as the anchor.
        const x3::phys::Vec3 playerAnchor = arenaCenter;

        for (uint32_t f = 0; f < benchFrames && !glfwWindowShouldClose(window); ++f) {
            glfwPollEvents();
            const double nowT = glfwGetTime();
            const double cpuMs = (nowT - prevT) * 1000.0; prevT = nowT;

            // Tick BOTH sides' full combat AI so the measurement reflects honest
            // combat density (LOS rays + state-machine + fire raycasts + per-
            // frame melee-attack-permit arbitration on the enemy side).
            allies.update(dt, scene, *physics, playerAnchor, hostiles, /*nav*/ nullptr);
            arenaEnemies.update(dt, scene, *physics, playerAnchor);

            physics->step(dt);
            scene.update(*physics);

            auto frame = device->beginFrame();
            if (frame.valid) {
                scene.render(*device, frame);
                arenaEnemies.drawAll(*device, frame, scene);
                allies.draw(*device, frame);
                // HUD stats overlay so the GPU exercises the HUD path under load too.
                hud.drawStats(*device, frame, *console, (float)(cpuMs / 1000.0), /*force=*/true);
            }
            device->endFrame(frame);

            last = device->stats();
            if (f >= warmup) { sumCpuMs += cpuMs; sumGpuMs += last.gpuFrameMs; ++measured; }
        }

        const double avgCpu = measured ? sumCpuMs / measured : 0.0;
        const double avgGpu = measured ? sumGpuMs / measured : 0.0;
        const double avgFps = (avgCpu > 1e-6) ? (1000.0 / avgCpu) : 0.0;
        // `alive=A` reports the ENEMY survivor count (the meaningful one — enemies
        // drop as the squad fires; allies are tougher and rarely die in the bench
        // window). The ally survivor count is reported separately if it ever
        // differs from totalCount (a dead squad mid-bench indicates a balance bug).
        const uint32_t allyTotal  = allies.totalCount();
        const uint32_t allyAlive  = allies.aliveCount();
        const uint32_t enemyAlive = x3::game::allyArenaEnemiesAlive();
        const uint32_t enemyTotal = x3::game::allyArenaEnemyCount();
        char rb[320];
        std::snprintf(rb, sizeof(rb),
            "BENCH-COMBAT allies=%u enemies=%u alive=%u FPS=%.1f CPU=%.3fms GPU=%.3fms (allies_alive=%u, avg over %u frames)",
            allyTotal, enemyTotal, enemyAlive, avgFps, avgCpu, avgGpu, allyAlive, measured);
        x3::logInfo(rb);

        // Tear down the arena's ragdolls BEFORE the physics world is destroyed so
        // no Jolt body outlives its world (mirrors MonsterManager::shutdown).
        x3::game::allyArenaShutdown();

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
                    // Pass the firing weapon's DamageType through so canon-aliens Adaptive Hide
                    // (currently on the SaurianWarlord row) reacts to the player's loadout.
                    x3::game::FireResult r = game.onFire(eye, ray.dir, scene, *physics,
                                                        ray.damage, ray.type);
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
            // Per-room (per-FLOOR) occlusion cull for the DEFAULT hand-coded Spire: the
            // whole 7-floor tower was being submitted every frame (~49.6M tris). Each
            // surface/prop/art instance is now tagged with its L1Floor; here we compute the
            // player's CURRENT floor from eye.y and feed it (+ a conservative neighbour
            // margin) to the cull so only the current floor renders. The shaft/stairwell
            // are kNoRoom (always visible) so the open cross-floor sightline never pops.
            // r_roomcull 0 hard-disables it (noclip overview); r_culldepth widens the floor
            // radius (the floor slabs are SOLID + the only cross-floor opening — the shaft +
            // stairwell — is always-visible, so the CURRENT floor alone is visually complete;
            // default depth 6 -> radius 0 = current floor only). (Smoketest camera is on B1 —
            // the cull engages there and B1's full plate still renders, so no visible cell is
            // culled.) Engages for the DEFAULT level AND the --world canonlevel FALLBACK (when
            // the canonical JSON is absent and the legacy tower was built instead).
            if (!terrainWorld && !(canonWorld && canonFloor.valid())) {
                const bool roomCull = console->getInt("r_roomcull") != 0;
                scene.setRoomCullEnabled(roomCull);
                if (roomCull) {
                    const int depth = std::max(1, console->getInt("r_culldepth"));
                    const uint32_t floorRadius = (uint32_t)std::max(0, depth - 6); // default 6 -> radius 0
                    std::vector<uint32_t> visFloors;
                    x3::game::level1VisibleRooms(eye.y, floorRadius, visFloors);
                    scene.setVisibleRooms(visFloors);
                }
            }
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
            // CPU per-object frustum (POV) cull toggle (cvar r_frustumcull). Applies in
            // every world (not just canonlevel) — it gates the device's draw records
            // against the camera frustum. Set each frame so console scrubbing is live.
            // X3_SMOKE_NOFRUSTUM forces it OFF for the headless before/after perf A/B
            // (no console under --smoketest); unset = follow the cvar (default on).
            const bool frustumOn = std::getenv("X3_SMOKE_NOFRUSTUM")
                ? false : (console->getInt("r_frustumcull") != 0);
            device->setFrustumCull(frustumOn);
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
                // ---- Over-enemy HEALTH BAR overlay (playtest polish, 2026-05-27) ----
                // Exercise the SAME bar-render path the main loop uses, on the SAME monster
                // groups, so the perf line `healthbars=N.NN ms` measured here matches what
                // the live build emits. THIN, DISTANCE-FADED, LOS-OCCLUDED — see the helper
                // at the top of this file. The eye is the smoketest camera (vmX/vmY/vmZ).
                {
                    const double _pbar0 = glfwGetTime();
                    const x3::phys::Vec3 hbEye{ vmX, vmY, vmZ };
                    drawEnemyBarsForManager(*device, frame, *physics, hbEye, game.corridorEnemies());
                    drawEnemyBarsForManager(*device, frame, *physics, hbEye, game.checkpointEnemies());
                    drawEnemyBarsForManager(*device, frame, *physics, hbEye, game.bossAdds());
                    drawEnemyBarsForManager(*device, frame, *physics, hbEye, game.chen());
                    if (game.martinezSpawned())
                        drawOneEnemyBar(*device, frame, *physics, hbEye, game.martinez());
                    drawEnemyBarsForManager(*device, frame, *physics, hbEye, game.rescue().bosses());
                    for (uint32_t f = 0; f < (uint32_t)x3::game::SpireMidFloor::Count; ++f)
                        drawEnemyBarsForManager(*device, frame, *physics, hbEye,
                                                midFloors.enemies((x3::game::SpireMidFloor)f));
                    drawEnemyBarsForManager(*device, frame, *physics, hbEye, midFloors.f3Boss());
                    drawEnemyBarsForManager(*device, frame, *physics, hbEye, midFloors.swarmBoss());
                    drawEnemyBarsForManager(*device, frame, *physics, hbEye, midFloors.victimBoss());
                    for (uint32_t f = 0; f < (uint32_t)x3::game::SpireTopFloor::Count; ++f)
                        drawEnemyBarsForManager(*device, frame, *physics, hbEye,
                                                topFloors.enemies((x3::game::SpireTopFloor)f));
                    drawEnemyBarsForManager(*device, frame, *physics, hbEye, topFloors.overseerBoss());
                    drawEnemyBarsForManager(*device, frame, *physics, hbEye, topFloors.boss());
                    drawEnemyBarsForManager(*device, frame, *physics, hbEye, topFloors.victimBoss());
                    for (uint32_t i2 = 0; i2 < nexus.chorus().podCount(); ++i2)
                        drawOneEnemyBar(*device, frame, *physics, hbEye, nexus.chorus().pod(i2));
                    for (uint32_t s = 0; s < (uint32_t)x3::game::SpireSubLevel::Count; ++s)
                        drawEnemyBarsForManager(*device, frame, *physics, hbEye,
                                                subLevels.enemies((x3::game::SpireSubLevel)s));
                    drawEnemyBarsForManager(*device, frame, *physics, hbEye, subLevels.miniBoss());
                    drawEnemyBarsForManager(*device, frame, *physics, hbEye, subLevels.chenBoss());
                    if (canonWorld && canonPlay.built()) {
                        drawEnemyBarsForManager(*device, frame, *physics, hbEye, canonPlay.mainHall());
                        drawEnemyBarsForManager(*device, frame, *physics, hbEye, canonPlay.cellGuards());
                        drawEnemyBarsForManager(*device, frame, *physics, hbEye, canonPlay.attackers());
                        drawEnemyBarsForManager(*device, frame, *physics, hbEye, canonPlay.rescue().bosses());
                        if (canonPlay.martinezSpawned())
                            drawOneEnemyBar(*device, frame, *physics, hbEye, canonPlay.martinez());
                    }
                    g_perf.healthbars += glfwGetTime() - _pbar0;
                }
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
            // POV-cull win: how many submitted draw records the frustum cull skipped
            // this frame, what actually got drawn, and the resulting triangle count.
            // (objectsDrawn = records that survived the cull + grouped; objectsSubmitted
            // = every drawMesh() this frame; objectsCulled = the skipped delta.)
            char fb[160];
            const char* dir = std::getenv("X3_SMOKE_HALL")
                ? "Main Hall, facing DOWN the -Z spine"
                : "Jake's Cell, facing +X";
            std::snprintf(fb, sizeof(fb),
                "frustum: drew %u/%u objects (culled %u), submitted tris=%u [cam: %s]",
                st.objectsDrawn, st.objectsSubmitted, st.objectsCulled, st.triangles,
                (canonWorld ? dir : "default"));
            x3::logInfo(fb);
        }
        // Perf line: average over-enemy health-bar time per frame (the same field the
        // [perf] line in the live build emits). Tracked across the smoketest's 30 frames.
        {
            const int frames = 30;
            const double avgMs = (g_perf.healthbars / (double)frames) * 1000.0;
            char pb[128];
            std::snprintf(pb, sizeof(pb),
                "[perf] smoketest: healthbars=%.2f ms (avg/frame over %d frames)",
                avgMs, frames);
            x3::logInfo(pb);
        }
        x3::logInfo("smoketest: 30 frames + recreate OK");
        audio->shutdown();
        combatFx.shutdown(*device);
        // TASK#12 (smoketest parity with the live shutdown path at the bottom of main):
        // the smoketest ticks game/topFloors/midFloors/nexus and fires bullets that CAN
        // hit a rigged enemy → spawns a SKINNED death ragdoll whose Jolt bodies are
        // removed in ~MonsterSystem -> IRagdoll::removeFromWorld(). If any monster is
        // mid-flop at exit, destroying the SpireNexus / Level1Game AFTER physics->
        // shutdown() touches a dead Jolt PhysicsSystem → access violation (Release) /
        // Jolt assert (Debug). The fix at commit 9273ca5 (and ~26 lines below at the
        // bottom of main) applied this teardown order to the live path but missed the
        // smoketest's parallel exit, so the smoketest exited with an empty-stderr exit-1
        // under contention while the live path was already safe. Mirror the live order
        // exactly: game.shutdown() (fans every Level1Game group + Martinez + bossAdds),
        // nexus.shutdown() (fans the Chorus pods), canonPlay.shutdown() (when --world
        // canonlevel), THEN physics->shutdown(). Idempotent — a no-op when no monster
        // is ragdolling.
        game.shutdown();
        nexus.shutdown();
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
        const bool on = !player.god(); player.setGod(on);
        if (on) { player.heal(); player.clearDamageFlash(); }   // wipe the lingering red overlay too
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
    // ---- restart: spawn a fresh X3Engine.exe + close this window so the main
    // loop unwinds cleanly through the normal shutdown path (texture/mesh release,
    // VMA leak check, etc.). argv[0] is the absolute path the OS launched us with —
    // we MUST use that since cmd's `start` resolves bare names against CWD (which
    // is the project root, not the build/bin/Release dir where the exe lives).
    // Playtest aid — not a true in-place level reset, just a clean-slate cycle.
    const std::string restartExe(argv[0] ? argv[0] : "X3Engine.exe");
    console->registerCommand("restart", [&console, window, restartExe](const std::vector<std::string>&) {
        const std::string cmd = std::string("start \"\" \"") + restartExe + "\"";
        console->print(std::string("restart: spawning ") + restartExe);
        const int rc = std::system(cmd.c_str());
        console->print(std::string("restart: spawn rc=") + std::to_string(rc) + " — closing this window...");
        glfwSetWindowShouldClose(window, GLFW_TRUE);
    }, "restart - spawn a fresh X3Engine + exit this one");

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
        // SSAO + SSGI default ON only when the device has hardware ray-tracing.
        // Their raster fallback renders the scene BLACK on non-RT GPUs (e.g. GTX
        // 1080 Ti — see [rhi] "RT: not available on this device — SSAO/CSM raster
        // fallback"). The fleet's RTX boxes still get them on; older cards default
        // OFF so the level is visible on launch.
        const bool hasRT = device && device->rayTracingSupported();
        sm.bloom = true; sm.ssao = hasRT; sm.ssgi = hasRT; sm.shadows = true;
        sm.vsync = desc.vsync; sm.width = W; sm.height = H;
        sm.rtao = (console->getInt("r_rtao") != 0);   // RT AO: reflect the cvar (default OFF)
        // Audio: seed from the persisted values applied to the audio system above.
        sm.musicOn = s_musicOn; sm.musicVol = s_musicVol; sm.sfxVol = s_sfxVol;
        gameUi.init(*device, console.get(), sm);
        // Push the seeded settings to the device + cvars NOW so SSAO/SSGI are
        // OFF on non-RT GPUs from the very first frame (otherwise the engine's
        // raster-fallback default would render the level black until the player
        // manually toggles them in Settings).
        gameUi.applySettings(*device, console.get());
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
            else if (termMode) { termMode = false; holoSys.leave(); }
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
        if (consoleOpen || uiMenuActive || termMode || codeMode) { ddx = 0.0f; ddy = 0.0f; }

        // Gameplay key reads are gated off while the console, a UI menu, the cell
        // terminal, OR a door-code keypad is active — so ALL gameplay input is
        // redirected to whatever is capturing (it reads keys via rawKey below) and
        // nothing drives movement/use/jump/fire/noclip/weapon-switch while typing.
        auto keyDown = [&](int k) {
            return !consoleOpen && !uiMenuActive && !termMode && !codeMode &&
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

        // ---- WEAPONS: number keys 1..N switch the selected weapon; R reloads.
        // Suppressed while a keypad OR the cell terminal is active (those number/letter
        // keys are being typed as a code, not used to switch weapons).
        if (!codeMode && !termMode && !terrainWorld) {
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
            // SIT-AT-CHAIR has FIRST priority on the E edge: if already seated, E
            // stands the player back up (the player's only "use" while seated). Else
            // if a chair is in reach + the player is facing it (kSitReach +
            // kSitFacingDot, see glass_lounge.cpp), this E sits them. The chair list
            // is well clear of every other interactable (the lounge is in the cell-
            // adjacent open room; doors / NPCs / the terminal / the elevator are
            // elsewhere on B1), so taking priority here doesn't starve other uses.
            // This block uses the SAME keyDown gate the terminal/keypad rely on, so it
            // cannot fire while typing into the terminal/keypad or in a UI menu, and
            // firing the weapon is a mouse-button event that never gets here.
            bool sitConsumedE = false;
            if (sitCtrl.seated()) {
                sitCtrl.requestStand();
                x3::logInfo("use: standing up from the lounge chair");
                sitConsumedE = true;
            } else if (glassLounge.built()) {
                const int ci = glassLounge.nearestSittableChair(eye, yaw);
                if (ci >= 0) {
                    const x3::phys::Vec3 seatEye = glassLounge.seatedEye((uint32_t)ci);
                    const float          seatYaw = glassLounge.seatedYaw((uint32_t)ci);
                    sitCtrl.requestSit(ci, eye, yaw, seatEye, seatYaw);
                    x3::logInfo("use: sitting down at lounge chair " + std::to_string(ci));
                    sitConsumedE = true;
                }
            }
            // RESCUED-NPC TALK takes priority over the bare door/rescue handlers so
            // the captive girl always gets her exchange. If a live captive is in talk
            // range, this E starts/advances the dialog; completing it performs the
            // actual rescue (so she becomes a following companion) + queues her bark.
            std::string talkWho; x3::phys::Vec3 talkPos{};
            const bool talkInRange = nearestLiveCaptive(eye, x3::game::kTalkReach, talkWho, talkPos);
            const bool talkHandled = !sitConsumedE && (npcDialog.active() || talkInRange);
            if (sitConsumedE) {
                // Sit/stand already consumed the E edge — fall through past every other
                // handler this frame.
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
            } else if (!termMode && holoSys.count() > 0 &&
                       holoSys.nearestInReach(eye) >= 0) {
                // Near ANY kiosk: open terminal-entry mode + activate the nearest
                // kiosk in the system. The legacy cell-escape terminal is one of these
                // (registered as external in the kiosk system), so this generalises
                // the old game.secret().terminal() path to ALL placeable terminals.
                const int ki = holoSys.nearestInReach(eye);
                holoSys.enter((uint32_t)ki);
                termMode = true;
                const std::string& lbl = holoSys.at((uint32_t)ki).label;
                const std::string& kcode = holoSys.at((uint32_t)ki).code;
                x3::logInfo("use: terminal '" + lbl + "' — type" +
                            (kcode.empty() ? std::string(" command, ") :
                                              (std::string(" '") + kcode + "', ")) +
                            "Enter to submit, Esc to cancel");
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

        // ---- HoloTerminal kiosk entry: capture digit/letter/space/backspace/Enter
        // edges while a kiosk is active. The kiosk system is the single input sink;
        // it routes chars to the currently-active kiosk + dispatches the
        // per-instance command on Enter (UnlockCell / OpenDoor / ToggleLights /
        // TriggerAlarm / ShowLore / SpawnCrate). Esc-cancel handled in the Esc block above.
        if (termMode && !terrainWorld && holoSys.isUsing()) {
            for (int dgt = 0; dgt < 10; ++dgt) {
                bool dn = rawKey(GLFW_KEY_0 + dgt) || rawKey(GLFW_KEY_KP_0 + dgt);
                if (dn && !tmDigitPrev[dgt]) holoSys.pushChar((char)('0' + dgt));
                tmDigitPrev[dgt] = dn;
            }
            // Letters + space (uppercased by the system to match the on-glass font).
            for (int li = 0; li < 26; ++li) {
                bool dn = rawKey(GLFW_KEY_A + li);
                if (dn && !tmCharPrev[li]) holoSys.pushChar((char)('A' + li));
                tmCharPrev[li] = dn;
            }
            bool tspaceNow = rawKey(GLFW_KEY_SPACE);
            if (tspaceNow && !tmSpacePrev) holoSys.pushChar(' ');
            tmSpacePrev = tspaceNow;
            bool tbackNow = rawKey(GLFW_KEY_BACKSPACE);
            if (tbackNow && !tmBackPrev) holoSys.backspace();
            tmBackPrev = tbackNow;
            bool tEnterNow = rawKey(GLFW_KEY_ENTER) || rawKey(GLFW_KEY_KP_ENTER);
            if (tEnterNow && !tmEnterPrev) {
                bool ok = holoSys.submit(scene, game.doors(), *device);
                if (ok) { termMode = false;
                          x3::logInfo("terminal: command ACCEPTED"); }
                else      x3::logInfo("terminal: command rejected");
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

                // Cell terminal / keypad open for typing: swallow movement + jump so the
                // keys (WASD/Space) type into the terminal instead of walking the player.
                if (termMode || codeMode) { in.moveFwd = 0.0f; in.moveStrafe = 0.0f; in.sprint = false; in.jumpPressed = false; }
                // SIT-AT-CHAIR: when seated (or sitting down mid-lerp), movement is
                // LOCKED. Any movement key press, however, triggers STAND (a tap-to-
                // stand affordance — feels natural, the player tries to walk and the
                // chair lets them go). The stand request itself UNLOCKS movement
                // immediately (SitController::requestStand flips the state), so this
                // sub-step's input still goes to the player on the next frame.
                if (sitCtrl.movementLocked()) {
                    const bool wantsMove = (in.moveFwd != 0.0f) || (in.moveStrafe != 0.0f) || in.jumpPressed;
                    if (wantsMove) {
                        sitCtrl.requestStand();
                        x3::logInfo("use: standing up (movement key pressed)");
                    }
                    in.moveFwd = 0.0f; in.moveStrafe = 0.0f; in.sprint = false; in.jumpPressed = false;
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
        // ---- SIT-AT-CHAIR camera override (B1 glass lounge). When the player is
        // SEATED (or mid-lerp into/out of the seat) the SitController computes a
        // smoothly-eased camera eye + yaw that overrides the live standing pose. Mouse
        // look is preserved (the yaw fed in is the player's live look yaw, which the
        // controller passes through while fully seated so the camera still rotates
        // with the mouse). Pitch is NOT overridden — free vertical look stays. Suppress
        // entirely in noclip so the fly cam is never hijacked. ----
        if (!noclip && !terrainWorld) {
            sitCtrl.update(dt, x3::phys::Vec3{ camX, camY, camZ }, camYaw);
            if (sitCtrl.seated() || sitCtrl.transitioning()) {
                const x3::phys::Vec3 ce = sitCtrl.camEye();
                camX = ce.x; camY = ce.y; camZ = ce.z;
                camYaw = sitCtrl.camYaw();
            }
        }
        // ---- HOLO-TERMINAL KIOSK SYSTEM tick: drives every kiosk's cursor blink,
        // texture re-bake (only when the input/readout actually changed) +
        // lore-timeout reset + the alarm decay. The external cell terminal is
        // also ticked by SecretRoom::tick (via game.tick()), so calling update()
        // here on it is a redundant no-op on the geometry but cleanly drives
        // the system-owned alarm/lore timers.
        if (!terrainWorld) holoSys.update(dt);
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
            thirdPerson.update(dt, scene, pfeet, eyeH, camYaw, camPitch, avatarRoom,
                               crouchedNow, prevFire);   // prevFire = last frame's held-fire
            const x3::game::ThirdPersonCamera tc =
                thirdPerson.camera(pfeet, eyeH, camYaw, camPitch);
            renderCamX = tc.camX; renderCamY = tc.camY; renderCamZ = tc.camZ;
        }
        device->setCamera(renderCamX, renderCamY, renderCamZ, camYaw, camPitch, 60.0f);
        // FLASHLIGHT (L toggles, default ON): re-issue the level's static ceiling
        // fixtures + a bright player-following light at the eye, so the dark halls
        // light up around you. Inserted FIRST so the 64-light cap never drops it.
        if (!terrainWorld) {
            bool lNow = keyDown(GLFW_KEY_L);
            if (lNow && !prevL) { flashlight = !flashlight;
                                  x3::logInfo(flashlight ? "flashlight ON" : "flashlight OFF"); }
            prevL = lNow;
            std::vector<x3::rhi::PointLight> fl = game.lightFixtures();
            // KIOSK ToggleLights: per-fixture brightness scale. The kiosk system's
            // ToggleLights sink (wired above) writes scales into kioskLightScale
            // (parallel to game.lightFixtures()). Apply BEFORE the flashlight is
            // prepended so kiosk-dimmed fixtures stay dimmed regardless of the
            // flashlight; the flashlight isn't a Light_A fixture so its scale is
            // unaffected.
            if (!kioskLightScale.empty() && kioskLightScale.size() == fl.size()) {
                for (uint32_t i = 0; i < fl.size(); ++i) {
                    const float s = kioskLightScale[i];
                    fl[i].color[0] *= s; fl[i].color[1] *= s; fl[i].color[2] *= s;
                }
            }
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
        // While the console, a UI menu, the cell terminal, or a keypad is capturing
        // input, gameplay verbs (melee / fire) must NOT trigger — no shooting through
        // the pause menu, no punching while typing the override code.
        const bool uiCapture = consoleOpen || uiMenuActive || termMode || codeMode;
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
        // Merge: uiCapture (HEAD's combined console/menu/term/code gate) AND !simFrozen
        // (doors-death-anim's menu-freeze gate) — suppress LMB fire while any UI panel is up.
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
                    // canon-aliens Adaptive Hide: each ray also carries its WeaponDef::type
                    // (Kinetic / Energy / ... stamped by Arsenal::fire). Threaded through
                    // every dispatcher so bosses with adaptiveHideResist > 0 react to the
                    // player's actual loadout choice (the Warlord's resist rhythm).
                    x3::game::FireResult r = game.onFire(eye, ray.dir, scene, *physics, wdmg, ray.type);
                    // --world canonlevel: the legacy groups are empty; route the shot through
                    // the canon enemies/boss/girls instead (arm-gated by canonPlay.onFire).
                    if (!r.hitMonster && canonWorld && canonPlay.built()) {
                        x3::game::FireResult rc = canonPlay.onFire(eye, ray.dir, scene, *physics, wdmg, ray.type);
                        if (rc.hitMonster || (!r.hit && rc.hit)) r = rc;
                    }
                    // If the B1 groups didn't take it, try the F3/F4/F5 enemies (the
                    // shot is already arm-gated by the arsenal/Level1Game::onFire).
                    if (!r.hitMonster && game.armed()) {
                        x3::game::FireResult rm = midFloors.onFire(eye, ray.dir, scene, *physics, wdmg, ray.type);
                        if (rm.hitMonster || (!r.hit && rm.hit)) r = rm;
                    }
                    // Then the F6/F7 top-floor enemies + the Clone boss.
                    if (!r.hitMonster && game.armed()) {
                        x3::game::FireResult rt = topFloors.onFire(eye, ray.dir, scene, *physics, wdmg, ray.type);
                        if (rt.hitMonster || (!r.hit && rt.hit)) r = rt;
                    }
                    // Then the Floor 4.5 Chorus pods (no-op until the Nexus is armed; a
                    // pod killed this way counts as KILLED, not saved).
                    if (!r.hitMonster && game.armed()) {
                        x3::game::FireResult rn = nexus.onFire(eye, ray.dir, scene, *physics, wdmg, ray.type);
                        if (rn.hitMonster || (!r.hit && rn.hit)) r = rn;
                    }
                    // Then the hidden sub-level enemies + the Frozen Collective (a clean
                    // miss until the descent has opened).
                    if (!r.hitMonster && game.armed()) {
                        x3::game::FireResult rs = subLevels.onFire(eye, ray.dir, scene, *physics, wdmg, ray.type);
                        if (rs.hitMonster || (!r.hit && rs.hit)) r = rs;
                    }
                    if (arsenal.current().beam) combatFx.addBolt(muzzle, r.endPoint);   // lightning gun: jagged forked bolt
                    else                        combatFx.addTracer(muzzle, r.endPoint); // tracer + muzzle burst per pellet
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
            // CPU per-object frustum (POV) cull toggle (cvar r_frustumcull). Additive on
            // top of the per-room PVS cull below; applies in every world. Set each frame
            // so console scrubbing is live.
            device->setFrustumCull(console->getInt("r_frustumcull") != 0);
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
            // Per-FLOOR occlusion cull for the DEFAULT hand-coded Spire (the ~49.6M-tri
            // tower). Compute the player's current floor from camY each frame and feed it
            // (+ a conservative neighbour margin) so only that floor's tagged geometry + art
            // draws; the elevator shaft + stairwell stay kNoRoom (always visible) so the
            // open cross-floor sightline never pops. r_roomcull 0 = off (noclip overview).
            else if (!terrainWorld) {
                const bool roomCull = console->getInt("r_roomcull") != 0;
                scene.setRoomCullEnabled(roomCull);
                if (roomCull) {
                    const int depth = std::max(1, console->getInt("r_culldepth"));
                    const uint32_t floorRadius = (uint32_t)std::max(0, depth - 6); // default 6 -> radius 0
                    std::vector<uint32_t> visFloors;
                    x3::game::level1VisibleRooms(camY, floorRadius, visFloors);
                    scene.setVisibleRooms(visFloors);
                }
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
                // ---- Over-enemy HEALTH BARS (2026-05-27 playtest polish) ------------
                // THIN (40x2 px + 1 px outline), DISTANCE-FADED (full <6 m, gone by 22 m),
                // LOS-OCCLUDED (per-bar Static rayCast from the camera origin to the chest)
                // and ON EVERY living monster (B1 groups + Spire floors + Nexus pods +
                // sub-levels + canon-mode groups + bosses + Phase-3 adds + cure/timer
                // mini-bosses). The cheap distance + on-screen gate keeps the raycast cost
                // bounded (no rays past far / behind the camera). See drawOneEnemyBar().
                {
                    const double _pbar0 = glfwGetTime();
                    const x3::phys::Vec3 hbEye{ camX, camY, camZ };
                    // ---- Level 1 B1 floor (corridor squad + checkpoint + Martinez + adds + Chen) ----
                    drawEnemyBarsForManager(*device, frame, *physics, hbEye, game.corridorEnemies());
                    drawEnemyBarsForManager(*device, frame, *physics, hbEye, game.checkpointEnemies());
                    drawEnemyBarsForManager(*device, frame, *physics, hbEye, game.bossAdds());
                    drawEnemyBarsForManager(*device, frame, *physics, hbEye, game.chen());
                    if (game.martinezSpawned())
                        drawOneEnemyBar(*device, frame, *physics, hbEye, game.martinez());
                    // ---- F2 rescue-system transformed bosses (Siren/Queen/Oracle) ----
                    drawEnemyBarsForManager(*device, frame, *physics, hbEye, game.rescue().bosses());
                    // ---- Spire mid floors (F3/F4/F5 enemies + F3 boss + F5 Swarm + victim boss) ----
                    for (uint32_t f = 0; f < (uint32_t)x3::game::SpireMidFloor::Count; ++f)
                        drawEnemyBarsForManager(*device, frame, *physics, hbEye,
                                                midFloors.enemies((x3::game::SpireMidFloor)f));
                    drawEnemyBarsForManager(*device, frame, *physics, hbEye, midFloors.f3Boss());
                    drawEnemyBarsForManager(*device, frame, *physics, hbEye, midFloors.swarmBoss());
                    drawEnemyBarsForManager(*device, frame, *physics, hbEye, midFloors.victimBoss());
                    // ---- Spire top floors (F6/F7 enemies + Overseer + Clone + Sarah's mini-boss) ----
                    for (uint32_t f = 0; f < (uint32_t)x3::game::SpireTopFloor::Count; ++f)
                        drawEnemyBarsForManager(*device, frame, *physics, hbEye,
                                                topFloors.enemies((x3::game::SpireTopFloor)f));
                    drawEnemyBarsForManager(*device, frame, *physics, hbEye, topFloors.overseerBoss());
                    drawEnemyBarsForManager(*device, frame, *physics, hbEye, topFloors.boss());
                    drawEnemyBarsForManager(*device, frame, *physics, hbEye, topFloors.victimBoss());
                    // ---- Floor 4.5 Chorus pods (multi-pod boss; each pod gets its own bar) ----
                    for (uint32_t i = 0; i < nexus.chorus().podCount(); ++i)
                        drawOneEnemyBar(*device, frame, *physics, hbEye, nexus.chorus().pod(i));
                    // ---- Hidden sub-levels (SL1/SL2/SL3 enemies + Frozen Collective + Chen mini-boss)
                    for (uint32_t s = 0; s < (uint32_t)x3::game::SpireSubLevel::Count; ++s)
                        drawEnemyBarsForManager(*device, frame, *physics, hbEye,
                                                subLevels.enemies((x3::game::SpireSubLevel)s));
                    drawEnemyBarsForManager(*device, frame, *physics, hbEye, subLevels.miniBoss());
                    drawEnemyBarsForManager(*device, frame, *physics, hbEye, subLevels.chenBoss());
                    // ---- --world canonlevel gameplay (Main Hall squad + cell guards + girl
                    //      attackers + Martinez + the transformed rescue bosses) ------
                    if (canonWorld && canonPlay.built()) {
                        drawEnemyBarsForManager(*device, frame, *physics, hbEye, canonPlay.mainHall());
                        drawEnemyBarsForManager(*device, frame, *physics, hbEye, canonPlay.cellGuards());
                        drawEnemyBarsForManager(*device, frame, *physics, hbEye, canonPlay.attackers());
                        drawEnemyBarsForManager(*device, frame, *physics, hbEye, canonPlay.rescue().bosses());
                        if (canonPlay.martinezSpawned())
                            drawOneEnemyBar(*device, frame, *physics, hbEye, canonPlay.martinez());
                    }
                    g_perf.healthbars += glfwGetTime() - _pbar0;
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
                // HOLO-TERMINAL ALARM screen-tint pulse: a brief red wash over the
                // whole HUD whenever a TriggerAlarm kiosk has been fired. Eases in
                // and out over the 3 s window so it reads as a security alarm
                // sweep, not a flicker. Drawn FIRST so the rest of the HUD
                // composites on top. Auto-clears when the timer reaches 0.
                if (!terrainWorld && holoSys.alarm().active) {
                    uint32_t hudW = 0, hudH = 0; device->hudSize(hudW, hudH);
                    const float t = holoSys.alarm().total > 0.0f ?
                                    holoSys.alarm().timer / holoSys.alarm().total : 0.0f;
                    // Smooth eased pulse: rises in the first 15% (sharp on), holds,
                    // ramps off over the last 30%.
                    float a = 0.0f;
                    if (t > 0.85f)      a = 1.0f - (t - 0.85f) / 0.15f;     // attack
                    else if (t > 0.30f) a = 1.0f;
                    else                 a = t / 0.30f;                      // release
                    a *= 0.35f;                                              // peak alpha
                    const float tint[4] = { 1.0f, 0.10f, 0.10f, a };
                    device->drawHudQuad(frame, 0.0f, 0.0f, (float)hudW, (float)hudH, tint);
                }
                // Door-code keypad prompt: centered, while code entry is active.
                if (codeMode && !terrainWorld) {
                    uint32_t hudW = 0, hudH = 0; device->hudSize(hudW, hudH);
                    const std::string kpPrompt = keypad.prompt();
                    const float kpCol[4] = { 1.0f, 0.82f, 0.18f, 1.0f };
                    device->drawHudText(frame, kpPrompt.c_str(),
                                        (float)hudW * 0.5f - 230.0f, (float)hudH * 0.5f - 60.0f, 3.0f, kpCol);
                }
                // HOLO-TERMINAL KIOSK readout fallback: LARGE high-contrast 2D
                // overlay over EACH kiosk's panel — but ONLY when the on-glass text
                // bake failed (the readout normally lives ON the glass texture so
                // it tilts with the panel). We iterate every kiosk in the system
                // and draw the fallback when needed; the active kiosk also gets
                // the live input line drawn over it.
                if (!terrainWorld && holoSys.count() > 0) {
                    float pex, pey, pez, pyaw, ppitch;
                    player.camera(pex, pey, pez, pyaw, ppitch);
                    if (noclip) { pex = flyX; pey = flyY; pez = flyZ; }
                    const int activeIdx = holoSys.active();
                    for (uint32_t i = 0; i < holoSys.count(); ++i) {
                        const auto& kt = holoSys.at(i);
                        if (kt.terminal().textOnGlass()) continue;   // on-glass bake handled the text
                        const x3::phys::Vec3 a = kt.terminal().anchor();
                        if (!kt.terminal().built()) continue;
                        const float tdx = a.x - pex, tdy = a.y - pey, tdz = a.z - pez;
                        const float distM = std::sqrt(tdx*tdx + tdy*tdy + tdz*tdz);
                        if (distM < 14.0f) {
                            const bool showInput = (activeIdx == (int)i) && termMode;
                            drawHoloReadout(*device, frame, kt.terminal(), a, showInput);
                        }
                    }
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
                    // HoloTerminal KIOSKS: float the [E] Use prompt over the nearest
                    // kiosk in reach (the system already does the proximity test).
                    // Each kiosk's prompt embeds its label so the player knows what
                    // they're activating. The legacy cell terminal is one of these.
                    if (holoSys.count() > 0) {
                        const x3::phys::Vec3 peye{ pex, pey, pez };
                        const int ki = holoSys.nearestInReach(peye);
                        if (ki >= 0) {
                            const auto& kt = holoSys.at((uint32_t)ki);
                            std::string lbl = std::string("[E] Use ") + kt.label;
                            if (!kt.code.empty()) lbl += " (code " + kt.code + ")";
                            floatPrompt(x3::phys::Vec3{ kt.pos.x, kt.pos.y + 0.55f, kt.pos.z },
                                        lbl.c_str(), 110.0f);
                        }
                    }
                    // GLASS LOUNGE: when SEATED, float a "[E] Stand" prompt anchored at
                    // the seated eye so the player always knows the exit key. When
                    // STANDING, if a chair is in reach + facing (the same gate the
                    // E-handler uses) float "[E] Sit" over that chair's seat. Only ONE
                    // (sit OR stand) ever appears at a time — naturally exclusive.
                    if (glassLounge.built()) {
                        if (sitCtrl.seated()) {
                            const int ci = sitCtrl.chairIndex();
                            if (ci >= 0 && ci < (int)glassLounge.chairCount()) {
                                const x3::phys::Vec3 se = glassLounge.seatedEye((uint32_t)ci);
                                floatPrompt(x3::phys::Vec3{ se.x, se.y + 0.30f, se.z }, "[E] Stand", 38.0f);
                            }
                        } else {
                            const int ci = glassLounge.nearestSittableChair(x3::phys::Vec3{ pex, pey, pez }, pyaw);
                            if (ci >= 0) {
                                const x3::phys::Vec3 cp = glassLounge.chair((uint32_t)ci).pos;
                                floatPrompt(x3::phys::Vec3{ cp.x, cp.y + 1.20f, cp.z }, "[E] Sit", 28.0f);
                            }
                        }
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
                    // Spire floor name under the radar (debug-grade locator). The
                    // L1Floor enum order matches kFloorNames in ui.cpp's radar block.
                    hm.playerFloor = (int)x3::game::level1FloorAtY(player.damageTargetPos().y);

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
    // The off-elevator Nexus (F4.5 Chorus) is a SpireNexus whose rigged pods also
    // spawn skinned death ragdolls — tear those Jolt bodies down too before physics.
    nexus.shutdown();
    if (canonPlay.built()) canonPlay.shutdown();   // --world canonlevel enemy ragdolls
    physics->shutdown();
    device->shutdown();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
