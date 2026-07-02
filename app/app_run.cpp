// app_run — the DEFAULT HOST (interactive render loop + world build). Lifted
// VERBATIM out of main() (#28 deep split, Phase C). See app_run.h.
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
#include "engine/rhi/Visibility.h"        // vis-unify: r_vis policy + unified stats
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
#include "cell_dressing.h"                   // --world canonlevel opening-space polish (set-dressing + motivated lights)
#include "intro_coldopen.h"                  // --world intro / default lead-in cold-open (shot-down -> captured)
#include "intro_orchestrator.h"              // Phase 3/4: runInteractiveIntro + IntroOutcome (branches the game start)
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
#include "spire_sublevels.h"
#include "strata.h"                          // STRATA descent: facility base -> Club 1127, scenic layers + caves (built LIVE around the elevator shaft)                // EFLZ hidden Floor-7 sub-levels + Dr. Chen Return Mission
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
#include "screenshot_hosts.h"              // #28 deep split: dispatchScreenshotHosts() (headless capture handlers)
#include "app_run.h"                       // #28 deep split: runDefaultHost() (the interactive render loop)
#include "settings_io.h"                   // #28 deep split: window/audio settings persistence (shared)
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
#include "cinematic.h"
#include "showroom_tod.h"   // SHOWROOM DAY/NIGHT helpers (shared with the --world showroom host)
#include "speaking_monster.h"
#include "test_registry.h"   // x3::apphost::TestFlags + dispatchTests (#28 split)
#include "bindings.h"
#include "self_tests.h"
#include "cinematic.h"
#include "showroom_tod.h"
#include "settings_io.h"
#include "boot_audio.h"
#include "input_globals.h"
#include "speaking_monster.h"
#include "bindings.h"
#include "host_context.h"
#include "app_run.h"
#include <future>
#include <utility>

namespace {

// Enemy-SFX (enemy-SFX pass): build the SHARED cue sink that gives enemies a VOICE.
// Maps every GameCue kind onto a 3D sound: footstep, bullet/melee impact, and the
// new EnemyTaunt / EnemyAttack / EnemyHit / EnemyDeath creature vocalizations. Used
// by BOTH the legacy Level 1 (game) AND --world canonlevel (canonPlay), which were
// previously silent for the canon enemies (canonPlay had no cue sink wired at all —
// the playtest "enemies make NO sounds" bug). Any invalid sound handle plays silent.
x3::game::GameCueFn makeEnemyCueSink(x3::audio::IAudioSystem* asys,
                                     x3::audio::SoundHandle step,
                                     x3::audio::SoundHandle gun,
                                     x3::audio::SoundHandle taunt,
                                     x3::audio::SoundHandle attack,
                                     x3::audio::SoundHandle hit,
                                     x3::audio::SoundHandle death) {
    return [asys, step, gun, taunt, attack, hit, death](const x3::game::GameCue& c) {
        if (!asys) return;
        auto play = [asys, &c](x3::audio::SoundHandle h, float vol, float pitch) {
            if (h.valid()) asys->playSound3D(h, c.pos.x, c.pos.y, c.pos.z, vol, pitch);
        };
        switch (c.kind) {
            case x3::game::CueKind::Footstep:     play(step,   0.12f * c.intensity, 0.55f); break;
            case x3::game::CueKind::BulletImpact:
            case x3::game::CueKind::MeleeImpact:  play(gun,    0.5f  * c.intensity, 0.7f);  break;
            case x3::game::CueKind::EnemyTaunt:   play(taunt,  0.55f * c.intensity, 1.0f);  break;
            case x3::game::CueKind::EnemyAttack:  play(attack, 0.7f  * c.intensity, 1.0f);  break;
            case x3::game::CueKind::EnemyHit:     play(hit,    0.8f  * c.intensity, 1.0f);  break;
            case x3::game::CueKind::EnemyDeath:   play(death,  0.95f * c.intensity, 1.0f);  break;
        }
    };
}

using x3::apphost::NightSkyPlanet;
using x3::apphost::kNightSkyDist;
using x3::apphost::loadNightSkyPlanets;
using x3::apphost::drawNightSkyPlanets;
using x3::apphost::CinActorState;
using x3::apphost::CinematicScene;
using x3::apphost::CinAudioMap;
using x3::apphost::runCutsceneWindowed;
using x3::apphost::applyShowroomTimeOfDay;
using x3::apphost::showroomDayDefault;
using x3::apphost::x3SettingsPath;
using x3::apphost::readWindowSize;
using x3::apphost::readAudioSettings;
using x3::apphost::writeSettings;
using x3::apphost::g_weaponScroll;
using x3::apphost::scrollCallback;
using x3::apphost::SpeakingMonster;
using x3::apphost::loadBootScripts;
using x3::apphost::registerGameBindings;
using x3::apphost::submitTerminalToScripts;
using x3::apphost::BootAudio;
using x3::apphost::makeBootAudio;
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
    // THE ONE visibility cvar (vis-unify; see engine/rhi/Visibility.h policy table):
    // -1 auto-best, 0 CPU frustum only, 1 +room/portal PVS (today's default
    // behaviour), 2 PVS+GPU cull (auto tier), 3 PVS+GPU+HZB occlusion. The legacy
    // cvars (r_roomcull/r_cullpath/r_hzb) remain as COMPAT ALIASES that map onto
    // this one (a deprecation line is logged when they're touched). Default 1 ==
    // byte-identical to the pre-unify defaults (roomcull 1, cullpath 0, hzb 0).
    console.registerCVar("r_vis", "1", "unified visibility policy: -1 auto, 0 cpu, 1 +pvs, 2 pvs+gpu, 3 pvs+gpu+hzb");
    // Per-object motion vectors for TAA reprojection / DLSS input (deferred cvar #4).
    // Default 0 so the determinism basins stay byte-identical to the pre-velocity
    // build; --velocity / `r_velocity 1` enables. No-op when TAA is off / unsupported.
    console.registerCVar("r_velocity", "0", "per-object motion vectors for TAA/DLSS (0 = camera-only reproj, byte-identical)");
    // Skinned-character TLAS refit toggle (deferred cvar #3): visible monsters/NPCs
    // enter the scene TLAS so RT shadows/refl/DDGI/acoustics see them. Default 1 (on).
    console.registerCVar("r_skinnedrt", "1", "add visible skinned characters to the RT scene TLAS (0 = static-only TLAS)");
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

// ---- Unified visibility sync state (vis-unify) -----------------------------
// THE per-frame policy the whole frame consults: applyRtaoCVars resolves it from
// r_vis (+ the legacy alias cvars + device caps) and pushes it onto the device;
// the world loops read g_visPolicy.pvs to gate the room/portal PVS. File-scope
// (like the other render statics) because the PVS gate sites live deep inside the
// render loops below.
static x3::rhi::VisPolicy g_visPolicy{};
// PVS flood-fill CPU ms of the current frame (measured at the flood sites, fed
// to IRenderDevice::setVisHostStats together with Scene::lastRoomCulled()).
static float g_visPvsMs = 0.0f;
struct VisCvarSync {
    bool init = false;
    int  lastVis = 1, lastRoom = 1, lastPath = 0, lastHzb = 0, lastFrustum = 1;
    int  tierForce = -1;       // explicit legacy r_cullpath tier (1/2/3); -1 = auto
    int  lastResolved = -999;  // resolved mode last logged
};
static VisCvarSync g_visSync;

// Read the r_rtao* cvars and push them onto the device (no-op on a non-RT device).
// NON-const console (vis-unify): the alias fold writes r_vis back from the legacy
// r_cullpath/r_hzb cvars through console.set().
void applyRtaoCVars(x3::con::IConsole& console, x3::rhi::IRenderDevice& device) {
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
    // CPU per-object frustum cull (live; default on). NOT an r_vis level: it bypasses
    // the PREDICATE on whichever path is active (the --test-gpucull ALWAYS_VISIBLE
    // harness knob), so it stays a direct passthrough.
    {
        const int fc = console.getInt("r_frustumcull");
        if (g_visSync.init && fc != g_visSync.lastFrustum)
            x3::logInfo("[vis] r_frustumcull is a debug PREDICATE BYPASS now — the unified policy lives in r_vis");
        g_visSync.lastFrustum = fc;
        device.setFrustumCullEnabled(fc != 0);
    }

    // ---- vis-unify: resolve THE ONE policy (r_vis) and apply it ------------
    // The legacy cvars remain compat ALIASES: touching r_cullpath/r_hzb remaps
    // r_vis (with a deprecation line); r_roomcull is the PVS sub-override
    // (0 = noclip/debug draw-all) and does not change the GPU stages.
    {
        int vis  = console.getInt("r_vis");
        const int room = console.getInt("r_roomcull");
        const int path = console.getInt("r_cullpath");
        const int hzb  = console.getInt("r_hzb");

        // Fold the legacy gpu-path aliases onto r_vis (path 0 -> L1, path>0 ->
        // L2/L3 with the explicit tier preserved, path<0 -> auto).
        auto foldAliases = [&]() {
            int v = (path == 0) ? 1 : ((hzb != 0) ? 3 : 2);
            if (path < 0) v = (hzb != 0) ? 3 : -1;
            g_visSync.tierForce = (path >= 1) ? path : -1;
            console.set("r_vis", std::to_string(v));
            vis = v;
            g_visSync.lastVis = v;
        };

        if (!g_visSync.init) {
            g_visSync.init = true;
            g_visSync.lastVis = vis;
            g_visSync.lastRoom = room; g_visSync.lastPath = path; g_visSync.lastHzb = hzb;
            if (path != 0 || hzb != 0) {
                // CLI seeds (--cullpath/--hzb) / cfg set the legacy cvars before
                // the first sync: fold them once so both vocabularies agree.
                foldAliases();
                x3::logInfo("[vis] legacy cull cvars seeded -> r_vis " + std::to_string(vis) +
                            (g_visSync.tierForce >= 1 ? " (tier " + std::to_string(g_visSync.tierForce) + " forced)" : ""));
            }
        } else {
            if (vis != g_visSync.lastVis) {        // the ONE cvar wins when driven
                g_visSync.lastVis = vis;
                g_visSync.tierForce = -1;
            }
            if (path != g_visSync.lastPath || hzb != g_visSync.lastHzb) {
                if (path != g_visSync.lastPath)
                    x3::logInfo("[vis] r_cullpath is DEPRECATED (compat alias) -> mapping onto r_vis");
                if (hzb != g_visSync.lastHzb)
                    x3::logInfo("[vis] r_hzb is DEPRECATED (compat alias) -> mapping onto r_vis");
                g_visSync.lastPath = path; g_visSync.lastHzb = hzb;
                foldAliases();
                x3::logInfo("[vis] aliases mapped -> r_vis " + std::to_string(vis) +
                            (g_visSync.tierForce >= 1 ? " (tier " + std::to_string(g_visSync.tierForce) + " forced)" : ""));
            }
            if (room != g_visSync.lastRoom) {
                g_visSync.lastRoom = room;
                x3::logInfo("[vis] r_roomcull is DEPRECATED (compat alias) -> PVS override under r_vis (0 = draw whole level)");
            }
        }

        // Resolve against the device caps (re-resolved EVERY frame: caps publish
        // after the first frame, and frame-level degradations stay in-device).
        const x3::rhi::RenderStats st = device.stats();
        x3::rhi::VisCaps caps;
        caps.gpuCull   = st.gpuCullSupported;
        caps.asyncCull = st.asyncCullSupported;
        caps.hzb       = st.hzbSupported;
        g_visPolicy = x3::rhi::resolveVisPolicy(vis, caps, (room != 0) ? -1 : 0);
        if (g_visPolicy.cullPath == -1 && g_visSync.tierForce >= 1)
            g_visPolicy.cullPath = g_visSync.tierForce;   // legacy explicit tier
        device.setCullPath(g_visPolicy.cullPath);
        device.setHzbEnabled(g_visPolicy.hzb);
        if (g_visPolicy.mode != g_visSync.lastResolved) {
            g_visSync.lastResolved = g_visPolicy.mode;
            x3::logInfo(std::string("[vis] policy resolved: L") +
                        std::to_string(g_visPolicy.mode) + " (" + g_visPolicy.describe() +
                        "), cullPath " + std::to_string(g_visPolicy.cullPath) +
                        (g_visPolicy.hzb ? " + hzb" : ""));
        }
    }
    // Deferred cvar #3: skinned-character TLAS refit (r_skinnedrt, default on).
    device.setSkinnedRtEnabled(console.getInt("r_skinnedrt") != 0);
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
    // Deferred cvar #4: per-object motion vectors for TAA reprojection / DLSS
    // (r_velocity, default 0 = byte-identical camera-only reproj). The device
    // gates on TAA being active + velocity.spv present (graceful fallback).
    px.velocity   = console.getInt("r_velocity") != 0;
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

} // anon namespace (app_run host-only helpers)

namespace x3 { namespace apphost {

// ---- vis-unify test hooks: forward to the file-local default-host impls -----
void registerViewmodelCVarsForTest(x3::con::IConsole& console) { registerViewmodelCVars(console); }
void applyRtaoCVarsForTest(x3::con::IConsole& console, x3::rhi::IRenderDevice& device) {
    applyRtaoCVars(console, device);
}
void resetVisSyncForTest() { g_visSync = VisCvarSync{}; g_visPolicy = x3::rhi::VisPolicy{}; }
const x3::rhi::VisPolicy& visPolicyForTest() { return g_visPolicy; }

int runDefaultHost(HostContext& hc) {
    auto* device = hc.device;
    GLFWwindow* window = hc.window;
    const std::string& worldMode = hc.worldMode;
    const bool headless = hc.headless;
    const uint32_t W = hc.W;
    const uint32_t H = hc.H;
    const bool screenshot = hc.screenshot;
    const std::string& screenshotPath = hc.screenshotPath;
    const int screenshotSettle = hc.screenshotSettle;
    const bool shotCamOverride = hc.shotCamOverride;
    const float* shotCam = hc.shotCam;
    const uint32_t stressCount = hc.stressCount;
    const bool bench = hc.bench;
    const uint32_t benchFrames = hc.benchFrames;
    const bool smoketest = hc.smoketest;
    const bool testBootTime = hc.testBootTime;
    const bool testFramePacing = hc.testFramePacing;
    const bool testRt = hc.testRt;
    const bool testReflections = hc.testReflections;
    const bool testDdgi = hc.testDdgi;
    const bool testRtShadows = hc.testRtShadows;
    const bool noRtShadows = hc.noRtShadows;
    const int legacyPost = hc.legacyPost;
    const bool noTaa = hc.noTaa;
    const bool noRefl = hc.noRefl;
    const bool skipIntro = hc.skipIntro;
    const bool editorMode = hc.editorMode;
    const bool fxDemo = hc.fxDemo;
    const bool uiDemo = hc.uiDemo;
    const std::string& uiDemoPath = hc.uiDemoPath;
    const std::string& uiDemoScreen = hc.uiDemoScreen;
    const bool dialogShot = hc.dialogShot;
    const bool alertShot = hc.alertShot;
    const bool captureSpire = hc.captureSpire;
    const std::string& captureSpireDir = hc.captureSpireDir;
    const std::string& docWorldPath = hc.docWorldPath;
    const int cullPathArg = hc.cullPathArg;
    const int hzbArg = hc.hzbArg;
    const int visArg = hc.visArg;
    const double bootBudgetMs = hc.bootBudgetMs;
    const std::string& cutsceneFile = hc.cutsceneFile;
    const float cueTime = hc.cueTime;
    auto& cliCVars = hc.cliCVars;
    std::future<BootAudio>& bootAudioFut = *hc.bootAudioFut;
    x3::rhi::DeviceDesc desc{}; desc.vsync = hc.descVsync;
    constexpr uint32_t kHeadlessW = 1280, kHeadlessH = 720;  // fixed headless capture resolution

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
        device);
    bool  rtaHooked = false;        // occlusion provider currently installed?
    float rtaDebugTimer = 0.0f;     // snd_rta_debug log cadence
    // Concrete asset picks (see docs/ASSET_INVENTORY.md). Pack-relative paths with
    // graceful fallback: a missing/undecodable file -> invalid handle -> the
    // corresponding event is simply silent (logged once at load).
    BootAudio bootAudio = bootAudioFut.valid() ? bootAudioFut.get() : makeBootAudio();
    std::unique_ptr<x3::audio::IAudioSystem> audio(std::move(bootAudio.audio));
    // Thread the live audio system into the HostContext so the Intro Orchestrator's
    // cinematic beats get music/SFX again (Phase 5 audio restore — P4 passed nullptr
    // and the intro went silent). Non-owning; `audio` outlives the orchestrator call.
    hc.audio = audio.get();
    const x3::audio::SoundHandle sndGun    = bootAudio.gun;
    const x3::audio::SoundHandle sndDoor   = bootAudio.door;
    const x3::audio::SoundHandle sndPickup = bootAudio.pickup;
    const x3::audio::SoundHandle sndDeath  = bootAudio.death;
    // Enemy creature vocalizations (enemy-SFX pass). Used by the shared enemy cue
    // sink (legacy Level 1 AND --world canonlevel) so enemies taunt / grunt / die
    // audibly. Invalid (absent WAV) handles play silent — graceful on a clean machine.
    const x3::audio::SoundHandle sndEnemyTaunt  = bootAudio.enemyTaunt;
    const x3::audio::SoundHandle sndEnemyAttack = bootAudio.enemyAttack;
    const x3::audio::SoundHandle sndEnemyHit    = bootAudio.enemyHit;
    const x3::audio::SoundHandle sndEnemyDeath  = bootAudio.enemyDeath;
    // Footsteps reuse the gunshot WAV pitched down + quiet (no dedicated footstep
    // WAV in the inventory). It reads as a soft step; replace with a real footstep
    // SFX later if one is added to the pack.
    const x3::audio::SoundHandle sndStep = sndGun;
    // Resolved path for the looping music/ambient bed (started after the world is
    // built, below). Spaceship-ambience-style sci-fi action loop.
    const std::string kMusicPath = x3::game::resolveAudio(
        "Sci-Fi Music Pack 1/Loops/SMP1_LOOP_Zero8 _1.wav");
    // CAB DISCO MUSIC (the 1127 descent): a darker cyberpunk track that takes over
    // the MUSIC bus while the elevator is in disco mode, restoring the ambient bed
    // when disco ends. ROUTED THROUGH the music-volume setting (playMusic at
    // s_musicVol + the live Music Volume slider / setMusicVolume) -- so with the
    // default music vol 0 the cab stays quiet, and the disco comes alive the
    // moment the player raises the volume. Never hardcoded outside that control.
    // Two candidate pack layouts (flat G:-style, nested D:/Assets-style); a miss
    // resolves to a nonexistent path and playMusic simply stays silent (graceful).
    std::string kDiscoMusicPath = x3::game::resolveAudio(
        "Free - Sci-Fi and Cyberpunk Music Pack/03 Descent.wav");
    {
        std::error_code mec;
        if (!std::filesystem::exists(kDiscoMusicPath, mec))
            kDiscoMusicPath = x3::game::resolveAudio(
                "Free - Sci-Fi and Cyberpunk Music Pack/"
                "Free - Sci-Fi and Cyberpunk Music Pack/03 Descent.wav");
    }

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
            // ---- PHASE 4: INTERACTIVE BRANCHING INTRO. The orchestrator (app/
            // intro_orchestrator.*) OWNS the prologue now: it plays the cinematic
            // beats (via the cutscene player) interleaved with bounded interactive
            // space-combat windows, accumulates a skill score, rolls the skill-biased
            // {chance}, writes StoryFlags["intro.outcome"], and RETURNS the branch.
            // We then select the Act-1 build on that outcome:
            //   * shot_down (canon, ~93%) -> fall through to the EXISTING cell start
            //     (level1_game / canon Floor-1), exactly as before.
            //   * escaped (skill-earned)  -> the surface-landing rescuer start. Phase 7
            //     builds the real one; for THIS phase it's a STUB that logs + falls
            //     through to the cell, so the branch is wired + testable now.
            // (--skipintro bypasses this whole block -> the canon cell, unchanged.)
            const x3::intro::IntroOutcome outcome = x3::intro::runInteractiveIntro(hc);

            // The interactive intro can be aborted by closing the window mid-beat
            // (a window-close quit). Honor that the same way the old film did.
            if (window && glfwWindowShouldClose(window)) {
                physics->shutdown();
                device->shutdown();
                if (window) glfwDestroyWindow(window);
                glfwTerminate();
                return 0;
            }

            if (outcome == x3::intro::IntroOutcome::Escaped) {
                // ESCAPE PATH (Phase 7): the REAL surface-landing Act-1. The ion-pulse
                // descent (Phase 6) set StoryFlags["intro.landed"]; instead of waking
                // Jake a prisoner in the canon cell, hand off to the surface-start host
                // (app/world_hosts/host_surface_start.cpp) — Jake lands OUTSIDE the huge
                // glass facility where Sarah is held, FREE + ARMED, a rescuer (the exact
                // inverse of the cell start). The host owns its own scene/physics and the
                // FULL host teardown (device + window + glfw) per the world-host contract,
                // so we shut down THIS default host's physics first (the device/window are
                // torn down by the host) and return its exit code directly — we do NOT
                // fall through into the cell build below.
                x3::logInfo("[intro] ESCAPED -> surface-landing Act-1 (host_surface_start)");
                loading.shutdown(*device);
                physics->shutdown();
                hc.worldMode = "surface";
                return x3::apphost::dispatchWorldHost(hc);
            }

            // SEAMLESS WAKE (canon cell): the intro ends on black ("SIX MONTHS LATER").
            // Flip the loading screen to BLACKOUT so the cell build stays black, then
            // the hand-off fade is the slow first-person wake in the cell — control is
            // live underneath it, exactly like a normal spawn. (The stub escape path
            // also lands in the cell, so this applies to both for now.)
            loading.setBlackout(true);
            x3::boot::mark("intro cold-open (content)");
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
    x3::game::CellDressing canonDressing;      // opening-space set-dressing + motivated lights (canonWorld only)
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
    // ---- LIVE STRATA DESCENT (THE DESCENT, app/strata.*). The geological shaft
    // built AROUND the elevator's descent column (between the building base Y~=0 and
    // Club 1127 at Y=-200) so when the cab rides DOWN past the floors (the 1127 disco
    // descent) its glass-bottom / observation wall sees the REAL layered rock bands --
    // earth -> granite -> basalt -> obsidian -> glowing Crystal Veins -> Magma ->
    // Alien Substrate -> The Deep. The in-cab strata display (ElevatorSystem::strata())
    // already matches by construction (same band names/colors as StrataWorld::bandAtY),
    // so the cab readout and the real geometry the glass sees agree. Built once with
    // the level (Level 1 only); its mood lights are fed to the device per-frame ONLY
    // while the cab/eye is in the strata zone (below the facility base), so the normal
    // above-ground lighting/cull is untouched. ----
    x3::game::StrataWorld    liveStrata;
    x3::game::TriggerSystem  liveStrataTriggers;
    bool                     liveStrataBuilt = false;   // true once built (Level 1 only)
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
        // ---- DATA-DRIVEN CANONICAL FACILITY + per-room PVS cull. Load the WHOLE BUILDING
        // (all 7 stacked floors fused into one CanonFloor, connected by the synthesized
        // elevator shaft) so the player can ride/climb the tower; Floor 1's rooms come
        // FIRST (ids 0..52) so every name-based hook (beats, keycard, gameplay spawns)
        // still resolves to the detention level. --world canonfloor1 keeps the old
        // single-floor build for A/B + the geometry self-test. ----
        std::vector<uint32_t> canonFloorBase;
        canonFloor = x3::game::loadCanonBuilding(x3::game::canonProjectJsonPath(), 7, &canonFloorBase);
        if (canonFloor.valid()) {
            x3::game::CanonBuildOpts copts; copts.doors = &canonDoors; copts.lockSecuredRooms = true;
            x3::game::buildCanonFloor(canonFloor, scene, *device, *physics, copts);
            x3::boot::mark("canon floor geometry+doors");
            // Per-room ceiling lights: the data-driven floor skips the env_art Light_A
            // fixtures the legacy level registers, so without these the rooms only get
            // ambient + the flashlight (the DARK bug). We feed only the player's VISIBLE
            // rooms' lights each frame (below) so the active count stays under the cap.
            canonLights = x3::game::buildCanonLights(canonFloor);
            // OPENING-SPACE POLISH: dense set-dressing + motivated lighting over the canon
            // detention cell + Main Hall mouth (the first space the player sees). Purely
            // visual props (ModularSciFi + Warehouse kits) + extra PointLights (a flickering
            // cell tube, a red alarm wash, cyan terminal glow). Graybox stays the collision
            // truth; missing GLBs simply aren't drawn (the level never breaks).
            canonDressing.build(*device, x3::game::convertedGlbRoot(), canonFloor);
            x3::logInfo("--world canonlevel: built CANONICAL FACILITY (" +
                        std::to_string(canonFloorBase.size()) + " floors, " +
                        std::to_string(canonFloor.rooms.size()) + " rooms, " +
                        std::to_string(scene.size()) + " entities, " +
                        std::to_string(canonLights.size()) + " room lights); elevator shaft connects the floors; per-room PVS cull ACTIVE");
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
            // Enemy-SFX: wire the shared enemy cue sink so the canon-level enemies have
            // a VOICE (footsteps, attack swings, take-hit grunts, death, idle taunts) +
            // their impacts land audibly. canonPlay had NO cue sink before — its enemies
            // were silent (the playtest "enemies make NO sounds" bug for --world canonlevel).
            canonPlay.setCueSink(makeEnemyCueSink(audio.get(), sndStep, sndGun,
                                                  sndEnemyTaunt, sndEnemyAttack,
                                                  sndEnemyHit, sndEnemyDeath));
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
            game.setCueSink(makeEnemyCueSink(audio.get(), sndStep, sndGun,
                                             sndEnemyTaunt, sndEnemyAttack,
                                             sndEnemyHit, sndEnemyDeath));
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
        // PROPER ELEVATOR AUDIO: door open/close (the modular-scifi door WAV, reused
        // from sndDoor), an arrival/floor-pass chime (the recharge bling), a looped
        // motor/cable hum pitched by cab speed (Deep Processor Mech Drone), keypad
        // clicks (a UI button) and a wrong-code/alarm buzzer. Every load is graceful:
        // a missing WAV -> invalid handle -> that one cue is silent (never a crash).
        {
            x3::game::ElevatorSounds es;
            es.doorOpen  = sndDoor;          // already loaded at boot (S_ScifiDoor_A)
            es.doorClose = sndDoor;
            es.ding      = audio->load(x3::game::resolveAudio(
                "Sci-fi Evolution Gift Pack/Energy Bling.wav"));
            if (!es.ding.valid()) es.ding = sndPickup;   // fallback: recharge chime
            es.motor     = audio->load(x3::game::resolveAudio(
                "Sci-fi Evolution Gift Pack/Deep Processor Mech Drone.wav"));
            es.keyClick  = audio->load(x3::game::resolveAudio(
                "Sci-fi Evolution Gift Pack/Ceramic Menu Button.wav"));
            es.buzz      = audio->load(x3::game::resolveAudio(
                "Sci-fi Evolution Gift Pack/Negative Analog Computer Tone 2.wav"));
            elevator.setSounds(es);
        }
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

        // ---- THE DESCENT: build the STRATA shaft AROUND the live elevator column.
        // The same XZ as the elevator shaft (Lb.elevatorCenter) so the cab descends
        // THROUGH it; the geological bands span the building base (Y~=0) down to Club
        // 1127 (Y=-200). When the player rides the 1127 disco descent, the cab's
        // glass-bottom / observation wall now looks OUT at this REAL layered rock
        // (earth -> granite -> basalt -> obsidian -> glowing Crystal/Magma/Alien ->
        // The Deep) instead of an empty void. Reuses the strata module's build path
        // (NOT a duplicate) so the geometry + offshoots + on-foot route + triggers are
        // identical to `--world strata`. The bore radius (16 m) comfortably clears the
        // cab. Built ONCE.
        liveStrata.build(scene, *device, *physics, liveStrataTriggers,
                         Lb.elevatorCenter.x, Lb.elevatorCenter.z, /*radius*/16.0f);
        liveStrataBuilt = liveStrata.built();
        x3::boot::mark("strata descent build");
        x3::logInfo("--world level1/canon: STRATA descent built around the elevator shaft (" +
                    std::to_string(Lb.elevatorCenter.x) + "," + std::to_string(Lb.elevatorCenter.z) +
                    ") Y 0..-200 -- ride the elevator + enter 1127 to descend through it to The Deep");

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
        // Playtest "barrels look like red boxes" fix: a bright ADDITIVE orange
        // fireball (Explosion style) so a shot barrel reads as a violent blast,
        // not just scattered red chunks. Sized by the blast radius.
        combatFx.spawnExplosion(ctr, radius);
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
        x3::rhi::IRenderDevice* dev = device;
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
    // Per-weapon IMPACT sound cache (parallel to the fire cache): the WAV played 3D at
    // the hit point when a shot strikes a surface. Deduped by WAV path; a weapon with
    // no impactSfx (or a missing WAV) maps to an invalid handle -> the host plays no
    // dedicated impact sound (the visual impact FX still spawns). Built once at init.
    std::unordered_map<std::string, x3::audio::SoundHandle> impactSfxByName;
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
            // Impact sound (independent dedupe set; invalid handle if none/missing).
            x3::audio::SoundHandle ih{};
            if (!wd.impactSfx.empty()) {
                auto it = byPath.find(wd.impactSfx);
                if (it != byPath.end()) {
                    ih = it->second;
                } else {
                    x3::audio::SoundHandle loaded =
                        audio->load(x3::game::resolveAudio(wd.impactSfx));
                    ih = loaded;                         // invalid stays invalid (silent)
                    byPath.emplace(wd.impactSfx, ih);
                }
            }
            impactSfxByName[wd.name] = ih;
        }
    }
    // Resolve the current weapon's fire sound (fallback: the shared gunshot).
    auto currentFireSfx = [&]() -> x3::audio::SoundHandle {
        auto it = fireSfxByName.find(arsenal.current().name);
        return (it != fireSfxByName.end() && it->second.valid()) ? it->second : sndGun;
    };
    // Resolve the current weapon's impact sound (invalid handle if the weapon has
    // none -> the caller skips the 3D play and only spawns the visual impact FX).
    auto currentImpactSfx = [&]() -> x3::audio::SoundHandle {
        auto it = impactSfxByName.find(arsenal.current().name);
        return (it != impactSfxByName.end()) ? it->second : x3::audio::SoundHandle{};
    };

    // Live projectile bolts (plasma): host-owned; advanced + impact-resolved each
    // frame. Bounded by gameplay (a handful in flight); a plain vector is fine.
    struct LiveProjectile { x3::phys::Vec3 pos, vel; int damage; float traveled, range;
                            x3::game::WeaponFxKind impactKind = x3::game::WeaponFxKind::Default;
                            // canon-aliens Adaptive Hide: carry the firing WeaponDef's DamageType
                            // (Kinetic / Energy / Explosive / ...) along the bolt so the on-impact
                            // dispatch passes it to MonsterManager::fire — closes the projectile
                            // half of the resist-rhythm loop (plasma bolts read as Energy, etc).
                            x3::DamageType type = x3::DamageType::Kinetic;
                            x3::audio::SoundHandle impactSnd{}; };  // per-bolt impact SFX (weapon may have switched mid-flight)
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
    // --vis <n>: seed THE unified visibility cvar (vis-unify). Wins over the legacy
    // seeds above when both are given (it is the one cvar going forward).
    if (visArg != INT_MIN) {
        console->set("r_vis", std::to_string(visArg));
        // Direct r_vis seed: neutralize the legacy aliases so the first cvar sync
        // doesn't fold them back over the explicit unified request.
        if (cullPathArg == INT_MIN) console->set("r_cullpath", "0");
        x3::logInfo("[vis] r_vis seeded from CLI: " + std::to_string(visArg));
    }

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
    // ai_gpu: 1 = CUDA (offload all layers to the RTX 5090), 0 = CPU. Default 1
    // — the backend auto-falls back to CPU (one log line) if the build has no GPU
    // backend, so 1 is safe on any box. CUDA is a separate API from the engine's
    // Vulkan device — no renderer entanglement.
    console->registerCVar("ai_gpu", "1",
                          "LLM GPU inference (CUDA, all layers offloaded); auto-falls back to CPU if unavailable");
    console->registerCVar("ai_ctx",       "2048", "LLM context tokens per chat");
    console->registerCVar("ai_maxtokens", "256",  "LLM max tokens per reply");
    console->registerCVar("ai_temp",      "0.7",  "LLM sampling temperature");
    console->registerCVar("ai_threads",   "0",    "CPU inference threads (0=auto: half the cores, cap 16). Lower it to protect frame pacing on the CPU-fallback path; ignored on the GPU path.");
    console->registerCVar("ai_fpace_gen", "0",    "framepacing gate: drive continuous VIGIL generation during the flythrough (frame-impact benchmark)");

    // --set <cvar> <value> CLI overrides (repeatable) — applied HERE, before the
    // LLM loads, so ai_gpu / ai_threads / ai_ctx / ai_temp take effect for model
    // selection and load (and the per-frame cvar sync starts from the requested
    // state). Every cvar is registered by this point except ai_npc (below).
    for (const auto& kv : cliCVars) {
        console->set(kv.first, kv.second);
        x3::logInfo("--set " + kv.first + " " + kv.second);
    }

    const bool aiGpu = console->getInt("ai_gpu") != 0;

    // MODEL SELECTION RULE: scan assets/models/llm for *.gguf and pick by size —
    // the LARGEST GGUF when ai_gpu=1 (the 7B is far smarter and fits in VRAM at
    // GPU speed), the SMALLEST when on CPU (the 3B stays conversational at ~17
    // tok/s; a 7B on CPU would crawl). Falls back to the pinned 3B filename if
    // the scan finds nothing. Any Apache-2.0 GGUF dropped in works commercially
    // (see assets/models/llm/README.md on licensing).
    const std::string llmDir = x3::game::assetRoot() + "/models/llm";
    std::string llmModelPath;
    {
        std::error_code fec;
        std::uintmax_t bestSize = aiGpu ? 0u : static_cast<std::uintmax_t>(-1);
        for (const auto& e : std::filesystem::directory_iterator(llmDir, fec)) {
            if (e.path().extension() != ".gguf") continue;
            std::error_code sec;
            const std::uintmax_t sz = std::filesystem::file_size(e.path(), sec);
            if (sec) continue;
            if ((aiGpu && sz > bestSize) || (!aiGpu && sz < bestSize)) {
                bestSize      = sz;
                llmModelPath  = e.path().string();
            }
        }
        if (llmModelPath.empty())
            llmModelPath = llmDir + "/qwen2.5-3b-instruct-q4_k_m.gguf";
    }
    const bool llmModelPresent = std::filesystem::exists(llmModelPath);
    console->registerCVar("ai_npc",       llmModelPresent ? "1" : "0",
                          "LLM NPC minds (terminal freeform Q&A); default 1 only when the model file exists");
    // ai_npc registers after model selection (its default reflects presence), so
    // re-apply any --set ai_npc override that ran before it existed.
    for (const auto& kv : cliCVars) if (kv.first == "ai_npc") console->set(kv.first, kv.second);
    std::unique_ptr<x3::llm::ILlmSystem> llm;
    // Load in interactive mode, OR in the framepacing gate ONLY when the
    // ai_fpace_gen frame-impact benchmark is requested (--set applied above, so
    // this reflects the CLI). Normal framepacing CI stays model-free and fast.
    const bool fpaceGenRun = testFramePacing && console->getInt("ai_fpace_gen") != 0;
    if ((!headless || fpaceGenRun) && llmModelPresent && console->getInt("ai_npc") != 0) {
        x3::llm::ModelOpts lopts;
        lopts.contextTokens   = console->getInt("ai_ctx");
        lopts.maxOutputTokens = console->getInt("ai_maxtokens");
        lopts.temperature     = console->getFloat("ai_temp");
        lopts.threads         = console->getInt("ai_threads");   // 0 = auto
        lopts.gpuLayers       = aiGpu ? 99 : 0;   // 99 = all layers on the GPU
        x3::logInfo("[llm] selected model " + llmModelPath +
                    (aiGpu ? " (ai_gpu=1, GPU-preferred)" : " (ai_gpu=0, CPU)"));
        llm = x3::llm::createLlmSystem();
        if (!llm->loadModel(llmModelPath, lopts)) llm.reset();
    } else if (!headless && !llmModelPresent) {
        x3::logInfo("[llm] no model at " + llmModelPath +
                    " — terminal freeform Q&A falls back to canned SYSTEMS DEGRADED lines"
                    " (see assets/models/llm/README.md)");
    }

    // (--set CLI overrides were applied above, before the LLM loaded.)

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
        x3::rhi::IRenderDevice* devicePtr  = device;
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
        // --world canonlevel default vantage: stand INSIDE Jake's Cell looking toward the
        // +X doorway/hall so the dressed opening space (bunk, terminal, pipes, debris, the
        // flickering tube) fills the frame. Overridden by --shot-cam for the other angles.
        if (canonWorld && canonFloor.valid() && !shotCamOverride && !alertShot) {
            uint32_t jc = canonFloor.roomAt(2.0f, 0.0f, 40.0f);
            if (jc == x3::game::kNoRoom) jc = 0;
            const x3::game::CanonRoom& C = canonFloor.rooms[jc];
            // Stand in the +X/+Z corner and look diagonally back across the cell toward the
            // -X/-Z corner so the hero shot frames the bunk, the wall terminal, the overhead
            // pipes + the flickering tube together (the dressed opening space at a glance).
            ssX = C.x1() - 1.2f;
            ssY = C.y0() + 1.65f;          // eye height above the cell floor
            ssZ = C.z1() - 1.2f;
            ssYaw = 3.6f;                  // look toward the -X/-Z corner
            ssPitch = -0.05f;
        }
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
            // --world canonlevel SCREENSHOT lighting + cull: feed the player's visible
            // rooms' ceiling lights PLUS the opening-space dressing's motivated lights
            // (flickering tube / red alarm / cyan terminal), and set the visible-room
            // set so render() draws the cell + its doored neighbours. Without this the
            // capture path fed only the (empty) legacy fixtures — the cell read flat.
            if (canonWorld && canonFloor.valid()) {
                canonPlay.tick(dt, scene, *physics, ssEye, nullptr, x3::game::AttackFxFn{});
                canonDressing.tick(dt);
                x3::game::Frustum fr = x3::game::Frustum::build(
                    ssEye.x, ssEye.y, ssEye.z, ssYaw, ssPitch, ssFov, 16.0f / 9.0f);
                canonFloor.floodVisibleRoomsAt(ssEye.x, ssEye.y, ssEye.z, fr, &canonDoors,
                                               6, kCanonRoomBudget, canonVisRooms);
                scene.setRoomCullEnabled(true);
                scene.setVisibleRooms(canonVisRooms);
                std::vector<x3::rhi::PointLight> cl;
                // Dressing lights FIRST (inserted ahead so the cap never truncates the
                // motivated key lights), then the room ceiling lights fill in.
                for (const auto& dl : canonDressing.lights()) {
                    bool vis = false;
                    for (uint32_t v : canonVisRooms) if (v == dl.room) { vis = true; break; }
                    if (vis) cl.push_back(dl.light);
                }
                x3::game::selectVisibleCanonLights(canonLights, canonVisRooms,
                                                   ssEye.x, ssEye.y, ssEye.z, cl, 16);
                if (cl.size() > 64) cl.resize(64);
                device->setPointLights(cl.data(), (uint32_t)cl.size());
            }
            // Fix 1: arm the capture just before the FINAL settle frame so the copy
            // is recorded inside that frame's live command buffer (reads the
            // freshly-rendered, properly-acquired image — validation-clean). The
            // captureFrame() below then waits on that frame's fence + writes the PNG.
            if (i == kSettleFrames - 1) device->armCapture(screenshotPath.c_str());
            auto frame = device->beginFrame();
            if (frame.valid) {
                scene.render(*device, frame);
                // --world canonlevel: the opening-space dressing props + doors + gameplay
                // characters (room-gated by the visible set above).
                if (canonWorld && canonFloor.valid()) {
                    canonDressing.draw(*device, frame);
                    canonDoors.drawMeshes(*device, frame);
                    if (canonPlay.built()) canonPlay.draw(*device, frame, scene);
                }
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
        // Normal gate = 600 frames. When benchmarking VIGIL generation
        // (ai_fpace_gen=1) run longer so real token DECODE (not just prompt eval)
        // overlaps the flythrough — the flythrough is unthrottled (~1 ms/frame),
        // shorter than a cold model's first-token latency otherwise.
        const bool  fpaceGenWanted = (llm && console->getInt("ai_fpace_gen") != 0);
        const int   kFlyFrames = fpaceGenWanted ? 6000 : 600;
        const float dt = 1.0f / 60.0f;

        // ZERO-STUTTER-UNDER-GENERATION benchmark (ai_fpace_gen=1): keep VIGIL
        // generating continuously for the whole flythrough so the frame p99 below
        // reflects real inference contention. Inference is on its own thread; the
        // frame thread only drains tokens with poll() (a cheap string append).
        // Run the gate twice (ai_fpace_gen 0 then 1) and compare the two p99s.
        x3::llm::ChatId fpaceChat = x3::llm::kInvalidChat;
        long long fpaceTokens = 0;
        const bool fpaceGen = fpaceGenWanted;
        if (fpaceGen) {
            fpaceChat = llm->startChat(
                "You are VIGIL, the facility intelligence of Lab Zero. Answer the "
                "prisoner tersely, in character.");
            llm->submit(fpaceChat, "Describe every floor of this facility in exhaustive detail.");
            x3::logInfo("framepacing: ai_fpace_gen=1 — VIGIL generating during flythrough "
                        "(backend " + std::string(llm->backendName()) + ")");
        }
        int passed = 0, total = 0;
        auto check = [&](const char* name, bool ok) {
            ++total; if (ok) ++passed;
            x3::logInfo(std::string("  [framepacing] ") + (ok ? "PASS  " : "FAIL  ") + name);
        };
        for (int i = 0; i < kFlyFrames; ++i) {
            glfwPollEvents();
            // Drain VIGIL tokens on the frame thread (cheap) and keep it busy:
            // when a reply finishes, immediately queue another so generation
            // spans the entire flythrough.
            if (fpaceGen) {
                x3::llm::PollResult pr = llm->poll(fpaceChat);
                fpaceTokens += pr.newTokenCount;
                if (pr.done)
                    llm->submit(fpaceChat, "Continue in more detail, room by room.");
            }
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
        if (fpaceGen) {
            llm->endChat(fpaceChat);
            x3::logInfo("framepacing: VIGIL generated " + std::to_string(fpaceTokens) +
                        " tokens during the 600-frame flythrough");
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
        audio->playMusic(kMusicPath, /*loop*/true, /*vol*/0.0f);   // playtest: muted by default
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
            // Per-room occlusion cull (canonlevel): portal flood-fill (frustum-directional)
            // from the camera each frame so render() draws the player's room + every room
            // reachable through OPEN doorways that the camera LOOKS at, capped by r_culldepth
            // + a room budget. (No-op in every other world: scene has no room-tagged entities.)
            if (canonWorld && canonFloor.valid()) {
                // Unified policy (r_vis): the orchestrator decides whether the PVS
                // prefilter runs; its room-visible SET is what render() submits —
                // i.e. the GPU cull's INPUT when the GPU path is on.
                const bool roomCull = g_visPolicy.pvs;
                scene.setRoomCullEnabled(roomCull);
                g_visPvsMs = 0.0f;
                if (roomCull) {
                    const auto pvsT0 = std::chrono::steady_clock::now();
                    const uint32_t depth = (uint32_t)std::max(1, console->getInt("r_culldepth"));
                    x3::game::Frustum fr = x3::game::Frustum::build(
                        eye.x, eye.y, eye.z, vmYaw, vmPitch, 60.0f, 16.0f / 9.0f);
                    canonFloor.floodVisibleRoomsAt(eye.x, eye.y, eye.z, fr, &canonDoors,
                                                   depth, kCanonRoomBudget, canonVisRooms);
                    scene.setVisibleRooms(canonVisRooms);
                    g_visPvsMs = std::chrono::duration<float, std::milli>(
                        std::chrono::steady_clock::now() - pvsT0).count();
                }
                // Feed ONLY the visible rooms' ceiling lights (capped at 16) so the floor
                // is LIT under the smoketest while staying under the 64-light device cap.
                std::vector<x3::rhi::PointLight> cl;
                canonDressing.tick(dt);
                for (const auto& dl : canonDressing.lights()) {
                    bool vis = false;
                    for (uint32_t v : canonVisRooms) if (v == dl.room) { vis = true; break; }
                    if (vis) cl.push_back(dl.light);
                }
                uint32_t nLit = x3::game::selectVisibleCanonLights(
                    canonLights, canonVisRooms, eye.x, eye.y, eye.z, cl, 16);
                if (cl.size() > 64) cl.resize(64);
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
                // Unified vis stats: PVS submission skips + flood ms for this frame.
                device->setVisHostStats(scene.lastRoomCulled(), g_visPvsMs);
                if (canonWorld) canonDoors.drawMeshes(*device, frame);   // SM_Door_A doors (canonlevel)
                if (canonWorld && canonFloor.valid()) canonDressing.draw(*device, frame); // opening-space props
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
            // ONE unified vis line (vis-unify): the whole conserving chain
            // rooms -> frustum -> hzb -> drawn with per-stage times.
            x3::logInfo("smoketest: " +
                x3::rhi::formatVisLine(x3::rhi::assembleVisStats(st, g_visPolicy.mode)));
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
    // (playtest fix: DEFAULT music vol 0 -> MUTED on boot; SFX stays 1.0). Still
    // fully adjustable: readAudioSettings() below overrides from the saved cfg, and
    // the in-game Music Volume slider / setMusicVolume() raise it live. Applied to
    // the audio system, THEN the bed starts so it honors the saved volume/on. ----
    bool  s_musicOn  = true;
    float s_musicVol = 0.0f;     // muted by default (was 0.25) -- raise via the slider/cfg
    bool  s_prevDisco = false;   // elevator disco-mode edge (cab music swap, below)
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
                // THE DESCENT: breathe the Crystal/Magma/Alien glow + flicker the
                // magma mood lights of the strata shaft the cab rides through. Biased
                // to the cab so the nearest bands' lights win when over the device cap.
                if (liveStrataBuilt) {
                    const x3::phys::Vec3 cc = elevator.cabCenter();
                    liveStrata.update(x3::net::kSimDt, scene, *device, cc);
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
                if (liveStrataBuilt)
                    liveStrata.update(x3::net::kSimDt, scene, *device,
                                      x3::phys::Vec3{ flyX, flyY, flyZ });
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
            // ELEVATOR INTERIOR LIGHTING: the cab's ceiling fill + (in disco mode) the
            // 4 colored spots. Without this the car interior was unlit and disco never
            // rendered. Only added when the player is near/in the cab so they don't eat
            // the 64-light budget elsewhere; the disco cue animates their color in
            // elevator.update().
            if (elevator.built() && !elevator.pointLights().empty()) {
                const x3::phys::Vec3 cc = elevator.cabCenter();
                const float lex = camX - cc.x, lez = camZ - cc.z, ley = camY - cc.y;
                if (lex*lex + lez*lez < 100.0f && std::fabs(ley) < 12.0f) {
                    for (const auto& pl : elevator.pointLights()) {
                        // Skip fully-dark disco spots (color all 0 until disco mode).
                        if (pl.color[0] + pl.color[1] + pl.color[2] > 0.001f)
                            fl.push_back(pl);
                    }
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
                // DRAW, all capped consistently. The unified policy (r_vis / g_visPolicy)
                // gates the PVS; with it off the 1-hop set keeps a noclip overview lit.
                g_visPvsMs = 0.0f;
                if (g_visPolicy.pvs) {
                    const auto pvsT0 = std::chrono::steady_clock::now();
                    const uint32_t depth = (uint32_t)std::max(1, console->getInt("r_culldepth"));
                    int fbw = 0, fbh = 0; glfwGetFramebufferSize(window, &fbw, &fbh);
                    const float aspect = (float)std::max(1, fbw) / (float)std::max(1, fbh);
                    x3::game::Frustum fr = x3::game::Frustum::build(
                        camX, camY, camZ, camYaw, camPitch, 60.0f, aspect);
                    canonFloor.floodVisibleRoomsAt(camX, camY, camZ, fr, &canonDoors,
                                                   depth, kCanonRoomBudget, canonVisRooms);
                    g_visPvsMs = std::chrono::duration<float, std::milli>(
                        std::chrono::steady_clock::now() - pvsT0).count();
                } else {
                    canonFloor.visibleRoomsAt(camX, camY, camZ, canonVisRooms);
                }
                // OPENING-SPACE motivated lights (flickering cell tube / red alarm / cyan
                // terminal) for the visible dressed rooms — inserted at the FRONT so the cap
                // never drops these key lights, then the room ceiling lights fill in.
                for (const auto& dl : canonDressing.lights()) {
                    bool vis = false;
                    for (uint32_t v : canonVisRooms) if (v == dl.room) { vis = true; break; }
                    if (vis) fl.insert(fl.begin(), dl.light);
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
            // THE DESCENT lighting: while the eye is in the strata zone (below the
            // facility base, i.e. riding the cab down the shaft), append the strata's
            // breathing Crystal/Magma/Alien mood lights so the glowing depths actually
            // light up around the descending cab. Gated on Y so the above-ground
            // facility lighting is untouched; closest-16 keeps under the device cap.
            if (liveStrataBuilt && camY < 2.0f) {
                const auto& sl = liveStrata.pointLights();
                size_t take = sl.size() < 16 ? sl.size() : 16;
                for (size_t i = 0; i < take; ++i) fl.push_back(sl[i]);
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
                canonDressing.tick(dt);   // advance the flickering cell-tube phase
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
            // THE DESCENT: dispatch the strata shaft's triggers (descent entered,
            // per-band offshoots, and the ClubArrival beat at Y=-200 -- reaching The
            // Deep) as the player rides/walks down through it. Latches the strata
            // story beats; idempotent (each fires once).
            if (liveStrataBuilt)
                for (uint32_t tid : liveStrataTriggers.update(camPos)) liveStrata.onTrigger(tid);
            // CAB DISCO MUSIC: on the disco-mode edge swap the MUSIC bus between the
            // ambient bed and the 1127 descent track. Always at s_musicVol (default 0
            // -> silent until the player raises the Music Volume slider); the live
            // slider's setMusicVolume keeps applying to whichever track is playing.
            if (elevator.built() && elevator.disco() != s_prevDisco) {
                s_prevDisco = elevator.disco();
                audio->playMusic(s_prevDisco ? kDiscoMusicPath : kMusicPath,
                                 /*loop*/true, s_musicVol);
                x3::logInfo(std::string("[elevator] cab music -> ") +
                            (s_prevDisco ? "1127 DISCO DESCENT track" : "ambient bed") +
                            " (music bus, vol honors the Music Volume setting)");
            }
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
            // Muzzle origin = the held viewmodel's barrel tip. The default origin sits
            // BELOW the eye line (down 0.30) so ballistic tracers visibly leave the gun.
            // LIGHTNING (director note) must emanate from the gun TIP, not from below
            // it: the railgun beam reads wrong starting under the barrel. Use a
            // tip-accurate origin for the beam — pushed further forward to the emitter
            // and raised onto the barrel line (almost no downward drop).
            const bool isLightningWeapon =
                (x3::game::fxKindFromId(arsenal.current().muzzleFx) == x3::game::WeaponFxKind::Lightning);
            const x3::phys::Vec3 muzzle = isLightningWeapon
                ? muzzleFromCamera(camX, camY, camZ, camYaw, camPitch,
                                   /*fwd*/1.6f, /*right*/0.20f, /*down*/0.06f)   // gun TIP
                : muzzleFromCamera(camX, camY, camZ, camYaw, camPitch);
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
                projectiles.push_back(LiveProjectile{ muzzle, pj.vel, pj.damage, 0.0f, pj.range, impactKind, pj.type, currentImpactSfx() });
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
                // Per-weapon IMPACT sound: play ONCE per volley (not per pellet — a
                // shotgun's 10 pellets / a beam's chained rays shouldn't stack 10 voices)
                // at the first surface hit. Invalid handle (weapon has no impactSfx) -> skip.
                const x3::audio::SoundHandle impactSnd = currentImpactSfx();
                bool impactSndPlayed = false;
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
                    combatFx.addTracer(muzzle, r.endPoint, muzzleKind);   // tracer (Lightning -> jagged bolt) + muzzle burst per pellet
                    if (r.killed) { combatFx.spawnDeath(r.endPoint); anyKill = true; }
                    else if (r.hitMonster) { combatFx.spawnBlood(r.hitPoint, ray.dir); anyHit = true; lastHp = r.hpAfter; }
                    else {
                        x3::phys::RayHit wallHit =
                            physics->rayCast(eye, ray.dir, x3::game::kFireMaxDist, x3::phys::Layer::Static);
                        if (wallHit.hit) {
                            combatFx.spawnImpact(wallHit.point, wallHit.normal, impactKind);
                            if (impactSnd.valid() && !impactSndPlayed) {
                                audio->playSound3D(impactSnd, wallHit.point.x, wallHit.point.y,
                                                   wallHit.point.z, 0.6f, 1.0f);
                                impactSndPlayed = true;   // one impact voice per volley
                            }
                        }
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
                    // PER-WEAPON damage + DamageType: the bolt carries its WeaponDef projectile
                    // damage AND its canon-aliens DamageType all the way to the impact dispatch
                    // (so plasma bolts hit as Energy, future rockets as Explosive, etc).
                    x3::game::FireResult r = game.onFire(b.pos, ndir, scene, *physics, b.damage, b.type);
                    if (!r.hitMonster && canonWorld && canonPlay.built()) {   // canon enemies/boss/girls
                        x3::game::FireResult rc = canonPlay.onFire(b.pos, ndir, scene, *physics, b.damage, b.type);
                        if (rc.hitMonster) r = rc;
                    }
                    if (!r.hitMonster) {   // try the F3/F4/F5 enemies for this bolt
                        x3::game::FireResult rm = midFloors.onFire(b.pos, ndir, scene, *physics, b.damage, b.type);
                        if (rm.hitMonster) r = rm;
                    }
                    if (!r.hitMonster) {   // then the F6/F7 enemies + the Clone boss
                        x3::game::FireResult rt = topFloors.onFire(b.pos, ndir, scene, *physics, b.damage, b.type);
                        if (rt.hitMonster) r = rt;
                    }
                    if (!r.hitMonster) {   // then the Floor 4.5 Chorus pods (if armed)
                        x3::game::FireResult rn = nexus.onFire(b.pos, ndir, scene, *physics, b.damage, b.type);
                        if (rn.hitMonster) r = rn;
                    }
                    if (!r.hitMonster) {   // then the hidden sub-level enemies + Frozen Collective
                        x3::game::FireResult rs = subLevels.onFire(b.pos, ndir, scene, *physics, b.damage, b.type);
                        if (rs.hitMonster) r = rs;
                    }
                    combatFx.addTracer(b.pos, eh.point);
                    if (r.killed) { combatFx.spawnDeath(eh.point);
                        audio->playSound3D(sndDeath, eh.point.x, eh.point.y, eh.point.z, 1.0f, 1.0f); }
                    else combatFx.spawnBlood(eh.point, ndir);
                    consumed = true;
                } else {
                    x3::phys::RayHit sh = physics->rayCast(b.pos, ndir, stepLen, x3::phys::Layer::Static);
                    if (sh.hit) { combatFx.spawnImpact(sh.point, sh.normal, b.impactKind); combatFx.addTracer(b.pos, sh.point);
                        if (b.impactSnd.valid())
                            audio->playSound3D(b.impactSnd, sh.point.x, sh.point.y, sh.point.z, 0.6f, 1.0f);
                        consumed = true; }
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
            // `r_vis 0` / the r_roomcull-0 alias disable it (noclip overview).
            if (canonWorld && canonFloor.valid()) {
                const bool roomCull = g_visPolicy.pvs;
                scene.setRoomCullEnabled(roomCull);
                if (roomCull) scene.setVisibleRooms(canonVisRooms);   // same set as the lights
            }
            scene.render(*device, frame);
            // Unified vis stats: feed the PVS stage's numbers (submission skips +
            // flood-fill ms) so stats() carries the conserving rooms -> frustum ->
            // hzb -> drawn chain. Zeros outside the canon world (no room tags).
            device->setVisHostStats(scene.lastRoomCulled(), g_visPvsMs);
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
            // --world canonlevel OPENING-SPACE dressing: the bunk / terminal / pipes / debris
            // props over the cell + hall mouth (room-gated via the visible set already set on
            // the scene). Drawn before the characters so they sit in the dressed space.
            if (canonWorld && canonFloor.valid()) canonDressing.draw(*device, frame);
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
                    // Elevator: within ~4 m of the cab. Show a STATUS-AWARE prompt so the
                    // player understands the lift — current floor + what E will do (call
                    // to the next floor when idle, or "moving to F<x>" while travelling).
                    if (elevator.built()) {
                        const x3::phys::Vec3 cc = elevator.cabCenter();
                        const float ex = pex - cc.x, ez = pez - cc.z;
                        if (ex*ex + ez*ez < 16.0f) {
                            std::string ep;
                            const int cur = elevator.currentStop();
                            const int tgt = elevator.targetStop();
                            if (elevator.moving()) {
                                ep = "Elevator -> " + elevator.floorLabel(tgt) +
                                     "  (" + elevator.stateName() + ")";
                            } else {
                                const int next = (cur + 1) % std::max(1, elevator.stopCount());
                                ep = "Floor " + elevator.floorLabel(cur) +
                                     "   [E] Go to " + elevator.floorLabel(next);
                            }
                            floatPrompt(x3::phys::Vec3{ cc.x, cc.y + 1.6f, cc.z },
                                        ep.c_str(), (float)ep.size() * 4.4f);
                        }
                    }
                    // Cell HoloTerminal: within ~3 m of its anchor. Playtest fix:
                    // render as a subtle BOTTOM-CENTER HUD hint (not a world-space
                    // float over the panel) and DROP the "(code 1278)" spoiler -- the
                    // player should discover the code, not have it handed to them.
                    if (game.secret().terminal().built()) {
                        const x3::phys::Vec3 a = game.secret().terminal().anchor();
                        const float dx = pex - a.x, dz = pez - a.z;
                        if (dx*dx + dz*dz < 9.0f) {
                            uint32_t hw = 0, hh = 0; device->hudSize(hw, hh);
                            const char* hint = "[E] Use Terminal";
                            const float hsz = 18.0f;
                            const float adv = device->textAdvance(x3::rhi::FontRole::Menu, hint, hsz);
                            const float hx = ((hw > 0) ? hw * 0.5f : 640.0f) - adv * 0.5f;
                            const float hy = (hh > 0) ? hh * 0.88f : 520.0f;   // bottom-center
                            const float col[4]    = { 0.66f, 0.92f, 1.0f, 0.85f };
                            const float shadow[4] = { 0.0f, 0.0f, 0.0f, 0.70f };
                            device->drawHudTextF(frame, x3::rhi::FontRole::Menu, hint, hx + 1.5f, hy + 1.5f, hsz, shadow);
                            device->drawHudTextF(frame, x3::rhi::FontRole::Menu, hint, hx, hy, hsz, col);
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

}} // namespace x3::apphost
