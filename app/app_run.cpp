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
#include "engine/rhi/ClusterLights.h"    // kMaxSceneLights — the clustered scene light cap
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
#include "combat_log.h"                     // [P3-5] LOG-1: combat_log / ai_log spam gates
#include "level1_game.h"
#include "canon_play.h"                     // --world canonlevel gameplay (sidearm + animated enemies + Martinez + girls)
#include "canon_aliens.h"                   // CANON ALIENS: the four Rodin species living on the planet
#include "desc_mechanics.h"                 // W9-1: the desc-field Tier-A mechanics (interactables + status effects)
#include "item_db.h"                        // [W9-3 RPG] data-driven item defs (assets/items/items.json)
#include "inventory.h"                      // [W9-3 RPG] backpack + key-section inventory
#include "progression.h"                    // [W9-3 RPG] XP -> levels -> skill points
#include "skilltree.h"                      // [W9-3 RPG] skill tree + PlayerStatMods layer
#include "rpg_ui.h"                         // [W9-3 RPG] backpack/skill screens + HUD chip
#include "canon_45.h"                       // W5-1: LEVEL 4.5 — the Nexus Chamber (The Chorus)
#include "stairwell.h"                      // fix/spire-hollow-core: the facility stairwell (open switchback, F1..F7)
#include "cell_dressing.h"                   // --world canonlevel opening-space polish (set-dressing + motivated lights)
#include "room_dressing.h"                   // WAVE-3: recipe dressing for every OTHER canon room (surface-library panels + zone fog)
#include "facility_exterior.h"               // SEAM 2: the glass facility exterior wrapped around the REAL canon tower
#include "intro_coldopen.h"                  // --world intro / default lead-in cold-open (shot-down -> captured)
#include "intro_orchestrator.h"              // Phase 3/4: runInteractiveIntro + IntroOutcome (branches the game start)
#include "cutscene.h"                        // x3.cutscene/1 data-driven cutscene system (the COLD OPEN film)
#include "npc_dialog.h"                     // rescued-NPC talk/dialog -> companion (the captive girl)
#include "chat_tree.h"                      // x3.chattree/1 data-driven dialog runner (--test-chattree)
#include "vigil_barks.h"                     // VIGIL ambient companion barks (gated on vigilLink)
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
#include "npc_life.h"                         // LIVING CITY: authored occupation NPCs w/ daily schedules + heist (--test-npclife)
#include "ocean_base.h"                      // EFLZ open-world ocean + undersea base + submarine combat (--test-oceanbase)
#include "elevator.h"
#include "strata.h"   // R-3 fold: THE DESCENT — live strata shaft around the elevator column
#include "rifthub.h"      // W-RIFT: the RIFT HUB, now a REGION of the one world
#include "rift_depths.h"  // W-RIFT: the landing + the approach corridor to it
#include "destinations.h" // W-MENU: the ONE registry of every place the game has
#include "world_menu.h"   // W-MENU: the world / place selection screen (F6 + pause)
#include "club1127.h"
#include "club_listen.h"   // CLUB LISTEN MODE: cvars + console bind (live-beat drive)
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
#include "world_cars.h"                    // WORLD CARS: findable/drivable/hackable cars (canonlevel)
#include "vehparts.h"                      // performance-parts catalog + build composition (--test-vehparts)
#include "perfshop.h"                      // the drive-in performance shop (--world drive)
#include "ecology.h"                       // AMBIENT ECOLOGY: grazers/predators/patrols (--test-ecology)
#include "fish.h"                          // FISH: ambient schools in THE RIVER + the sea shallows
#include "waterzap.h"                      // THE WATER ZAP: the lightning gun electrifies the water
#include "sealife.h"                       // SEALIFE: the great white, the blue shark, the abyss
#include "god_rays.h"                      // GOD RAYS: sun shafts under the water surface
#include "crowd.h"                         // CROWDS: club dancers + facility civilians (--test-crowd)
#include "crowd_skin.h"                    // SKINNED CITIZENS: the crowds' rigged visual layer
#include "crowd_chatter.h"                 // CROWD CHATTER: chat bubbles + murmur walla over the crowds
#include "street_lights.h"                 // STREET LIGHT: real lamps — cones + pools + pooled lights
#include "alert.h"                         // FACILITY ALERT LEVEL: the wanted system (--test-alert)
#include "host_context.h"                  // #28 deep split: shared live-state struct for the --world hosts
#include "world_hosts.h"                   // #28 deep split: dispatchWorldHost() + the extracted host TUs
#include "screenshot_hosts.h"              // #28 deep split: dispatchScreenshotHosts() (headless capture handlers)
#include "app_run.h"                       // #28 deep split: runDefaultHost() (the interactive render loop)
#include "settings_io.h"                   // #28 deep split: window/audio settings persistence (shared)
#include "space_pilot.h"                   // FlightMode + shared requestedFlightMode latch (flightmode console cmd)
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
#include <sstream>    // [W9-3 RPG] the RPG save-file text lane (saveRpg/loadRpg)
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
// Guard-life (W4-3): the sink now picks PER-SPECIES vocal takes off the cue's
// species tag (GameCue.species = the emitting EnemyType). Buckets: 0=humanoid
// (Trooper/Illuminated + unknown/legacy emitters — the shared set), 1=creature
// (Verthani), 2=synth (BlueSynth). An invalid bucket handle falls back to the
// shared take, so a machine missing the new WAVs keeps the W2-B behaviour.
// Passing ba=nullptr keeps the original single-set behaviour byte-identical.
x3::game::GameCueFn makeEnemyCueSink(x3::audio::IAudioSystem* asys,
                                     x3::audio::SoundHandle step,
                                     x3::audio::SoundHandle gun,
                                     x3::audio::SoundHandle taunt,
                                     x3::audio::SoundHandle attack,
                                     x3::audio::SoundHandle hit,
                                     x3::audio::SoundHandle death,
                                     const x3::apphost::BootAudio* ba = nullptr) {
    struct Voices { x3::audio::SoundHandle t[3], a[3], h[3], d[3]; bool on = false; };
    Voices v;
    if (ba) {
        v.on = true;
        for (int i = 0; i < 3; ++i) {
            v.t[i] = ba->spTaunt[i];  v.a[i] = ba->spAttack[i];
            v.h[i] = ba->spHit[i];    v.d[i] = ba->spDeath[i];
        }
    }
    return [asys, step, gun, taunt, attack, hit, death, v](const x3::game::GameCue& c) {
        if (!asys) return;
        auto play = [asys, &c](x3::audio::SoundHandle h, float vol, float pitch) {
            if (h.valid()) asys->playSound3D(h, c.pos.x, c.pos.y, c.pos.z, vol, pitch);
        };
        // Species -> bucket (see EnemyType): Verthani=creature, BlueSynth=synth,
        // everything else (incl. kCueSpeciesNone legacy emitters) = humanoid/shared.
        int b = 0;
        if (v.on) {
            if (c.species == (uint32_t)x3::game::EnemyType::Verthani)       b = 1;
            else if (c.species == (uint32_t)x3::game::EnemyType::BlueSynth) b = 2;
        }
        auto pick = [&](const x3::audio::SoundHandle bucket[3],
                        x3::audio::SoundHandle shared) {
            return (v.on && bucket[b].valid()) ? bucket[b] : shared;
        };
        switch (c.kind) {
            case x3::game::CueKind::Footstep:     play(step,   0.12f * c.intensity, 0.55f); break;
            case x3::game::CueKind::BulletImpact:
            case x3::game::CueKind::MeleeImpact:  play(gun,    0.5f  * c.intensity, 0.7f);  break;
            case x3::game::CueKind::EnemyTaunt:   play(pick(v.t, taunt),  0.55f * c.intensity, 1.0f); break;
            case x3::game::CueKind::EnemyAttack:  play(pick(v.a, attack), 0.7f  * c.intensity, 1.0f); break;
            case x3::game::CueKind::EnemyHit:     play(pick(v.h, hit),    0.8f  * c.intensity, 1.0f); break;
            case x3::game::CueKind::EnemyDeath:   play(pick(v.d, death),  0.95f * c.intensity, 1.0f); break;
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

// ---- WATER-WEAPON MANNERS (polish item 4): the swim viewmodel LOWER ---------
// While swimming the FP gun slides DOWN-AND-IN (out of the water line) instead
// of clipping the surface: an offset curve on the viewmodel pose scaled by a
// smoothed 0..1 amount (dt-blended at the call sites), NOT a hard toggle. At
// amt=0 this is a byte-for-byte no-op; at amt=1 the muzzle is dropped below the
// frame edge and tilted down (the "carried, not aimed" read). Applied to every
// viewmodel branch (arsenal / canon sidearm / legacy pickup) since all take the
// same VmPose deltas. Firing is separately blocked while swimming (see the fire
// gate) — weapons don't fire underwater.
inline void applySwimViewmodelLower(VmPose& p, float amt) {
    if (amt <= 1e-3f) return;
    p.down     += 0.34f * amt;                 // slide the gun down out of the water line
    p.fwd      -= 0.16f * amt;                 // pull it in toward the body
    p.right    += 0.10f * amt;                 // ease it toward the frame edge
    p.pitchRad -= 38.0f * kDegToRad * amt;     // tilt the muzzle down (lowered carry)
    p.rollRad  += 10.0f * kDegToRad * amt;     // relaxed wrist
}

// ---- LIVING WORLD: the LOCKDOWN red shift (alert level 3+) ------------------
// One multiplier, shared by the live loop (level1 whole-feed / canon facility
// feed) and the --screenshot-alert staging, so the proof shot IS the live look.
inline void applyAlertRedShift(std::vector<x3::rhi::PointLight>& fl, float rs) {
    if (rs <= 0.0f) return;
    for (auto& L : fl) {
        const float lum = (L.color[0] + L.color[1] + L.color[2]) / 3.0f;
        L.color[0] = L.color[0] * (1.0f - rs) + lum * 1.7f * rs;
        L.color[1] *= 1.0f - rs * 0.78f;
        L.color[2] *= 1.0f - rs * 0.82f;
    }
}

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
    // [P3-5] LOG-1: combat/AI per-event log spam, DEFAULT QUIET (app/combat_log.h).
    // `combat_log 1` restores the per-hit lines ("[monster] hit for", "[player] took",
    // HEADSHOT / kill lines); `ai_log 1` restores the "[ai] entity N A -> B" state-
    // transition lines. Live-tunable; pushed into the gate flags each frame in
    // applyRtaoCVars (the per-frame cvar sync hub).
    console.registerCVar("combat_log", "0", "combat per-event log lines (player/monster hits, kills); 0 = quiet");
    console.registerCVar("ai_log",     "0", "[ai] state-transition log lines; 0 = quiet");
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
    // PLAYTEST TRIM (Tim, 2026-07-19): the facility interior read "a HAIR too hot/bright"
    // under the corrected sRGB baseline (5951890b). Default nudged 1.0 -> 0.88 (a ~12%
    // whole-scene trim) — a clean post-tonemap multiply that auto-exposure never
    // compensates (unlike lowering the lights). Subtle; NOT a darkening. Live/overridable.
    console.registerCVar("r_exposure", "0.88", "whole-scene brightness (pre-tonemap exposure multiplier; live; 0.88 = 12% playtest trim)");
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
    // ---- W2-A2 / AD-1: painterly-lever LIVE OVERRIDES (canonlevel only; -1 = keep
    // the zone's authored value). The zone opt-in itself lives in cell_dressing. ----
    console.registerCVar("r_fogdensity",     "-1",   "depth-fog extinction per meter override (canonlevel; -1 = keep zone value)");
    console.registerCVar("r_fogstart",       "-1",   "depth-fog clean-air start meters override (canonlevel; -1 = keep zone value)");
    console.registerCVar("r_gradestrength",  "-1",   "filmic grade master strength override 0..1 (canonlevel; -1 = keep zone value)");
    console.registerCVar("r_vignette",       "-1",   "vignette strength override 0..0.25 (canonlevel; -1 = keep zone value)");
    console.registerCVar("r_filmic",         "1",    "cinematic filmic post master gate (cutscene vignette/grain/split-tone; 0 = force off for A/B — the look itself only turns on during cutscene playback)");
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
    // CLUSTERED (froxel) FORWARD LIGHTING. 0 = the LEGACY path: every fragment
    // loops the fixed 64-entry point-light UBO array in full, so the scene cap is
    // 64 lights and the per-pixel cost is O(64) no matter how many actually reach
    // the pixel. 1 = dice the view frustum into a 16x9x24 froxel grid, assign each
    // light to the froxels its sphere of influence overlaps, and have each
    // fragment iterate ONLY its own froxel's list — which raises the scene cap to
    // 1024 lights (Echo Harbor's neon night, a fully dressed tunnel, car underglow)
    // AND makes the per-pixel cost proportional to the lights that actually reach
    // that pixel. DEFAULT 0: r_clusterlights 0 is bit-for-bit the old render, which
    // is what keeps every existing md5 / screenshot gate green. Live.
    console.registerCVar("r_clusterlights", "0", "clustered froxel forward lighting (16x9x24 grid, up to 1024 lights); 0 = legacy 64-light loop (bit-exact)");
    // CONTENT WIRING (lane inspx/content-wiring) — THE CPU-SIDE LIGHT BUDGET.
    // r_clusterlights raised the DEVICE cap to 1024, but every app-side budget
    // in app_run.cpp was still sized against the legacy 64-entry UBO and
    // truncated unconditionally, so turning clustering on changed nothing in a
    // real world (docs/design/CLUSTERED_LIGHTING.md says exactly this). This is
    // the ceiling the frame assembles to WHEN CLUSTERING IS ON; with
    // r_clusterlights 0 the budget is pinned to 64 and every feeder K to its
    // historical constant, so the legacy render stays bit-for-bit unchanged.
    console.registerCVar("r_maxlights", "1024", "CPU light budget when r_clusterlights 1 (clamped to the 1024 device cap); ignored while clustering is off -- then pinned to 64");
    // CONTENT WIRING -- CITY LIGHT DENSITY. 0 = the nine authored lamp rows at
    // 30-34 m (~56 lamps), i.e. the pre-lane city, byte-identical. 1 = every
    // street city.cpp authors gets a row at realistic urban spacing (~240
    // lamps) PLUS a glow-only pooled light per warm-lit window band and per
    // neon sign. That is the 200+ live source scene clustered lighting exists
    // to carry -- pair it with `--set r_clusterlights 1`, or the frame will
    // still assemble to the legacy 64-light budget and most of them are wasted.
    console.registerCVar("r_citylights", "0", "city light density: 0 = the 9 legacy lamp rows (bit-exact), 1 = every authored street + window/sign glow (~240 lamps; use with r_clusterlights 1)");
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
    console.registerCVar("ui_editor",       "1", "show LEVEL EDITOR in the pause menu (dev). 0 = hide it from players");
    console.registerCVar("r_rtreflections", "1", "ray-query reflection fallback where SSR misses (RT hardware only; SSR-only otherwise)");
    console.registerCVar("r_reflquality",   "0", "reflection buffer resolution: 0 = half-res (default), 1 = full-res");
    console.registerCVar("r_reflintensity", "1", "reflection blend weight scale [0..1] on the IBL-specular replace");
    // REFLECTION DENOISE — the stage the reflection chain was missing. GI has
    // gather -> temporal -> denoise -> apply; reflections had refl.comp write and
    // mesh.frag consume RAW. An edge-aware a-trous filter with depth + normal
    // edge stops now sits between them. It fixes the blotchy mottling measured on
    // real car paint (flat door skin, mean |px - 9x9 local mean|: 5.53 with
    // reflections off vs 7.69 shipped) that the consumer-side blur could not
    // reach — sweeping that blur 0/6/14/24 moved it only 7.70/7.92/7.69/7.56,
    // because the noise is in the BUFFER. All live.
    // r_refldenoise 0 = OFF and BIT-EXACT to the pre-denoise renderer.
    console.registerCVar("r_refldenoise",   "4",    "reflection denoise: a-trous iterations (tap spacing doubles each); 0 = off (raw buffer, bit-identical)");
    console.registerCVar("r_refldn_depth",  "0.06", "reflection denoise depth edge stop, RELATIVE to view distance (smaller = stricter)");
    console.registerCVar("r_refldn_normal", "16",   "reflection denoise normal edge stop exponent (larger = stricter across curvature)");
    console.registerCVar("r_refldn_disc",   "0.4",  "scale on mesh.frag's roughness glossy disc when the denoise stage ran (1 = legacy width)");
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
    console.registerCVar("r_debugview", "0", "Renderer debug view: 0 = off, 1 = SHADING NORMALS (N*0.5+0.5). The instrument that separates 'the light cannot reach it' from 'its normal points into the wall'.");
    console.registerCVar("r_flashlight", "1", "Player flashlight (L toggles in game). Set 0 to measure a room's OWN practicals with no torch riding the camera — the lighting-audit workhorse.");
    console.registerCVar("shot_weapon", "", "--screenshot: weapon whose FP viewmodel is held in the capture (e.g. shotgun/plasma/chaingun/lightning). Empty = pistol. QA hook for the per-weapon texture gate.");
    console.registerCVar("shot_fire",   "0", "--screenshot: fire the held weapon FROM ITS BARREL TIP through the settle frames (muzzle flash + tracer). The eyeball gate for 'the fire comes from the barrel', per weapon.");
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
    // ---- CASCADED SHADOW MAPS (Lane 3) ------------------------------------
    // The legacy sun shadow is ONE 45 m ortho box locked to the camera, so sun
    // shadows simply STOP ~45 m out. That is wrong in any open scene and a hard
    // blocker for racing: at 200 km/h that boundary sweeps past the car in 0.8 s.
    // r_csm 1 splits the frustum into Csm.h's kNumCascades slices and renders one
    // shadow map per slice into a 2D array — each sized from a rotation-INVARIANT
    // bounding sphere and snapped to the shadow texel grid so edges do not swim.
    // r_csm 0 is the legacy single cascade, BIT-EXACT (md5/screenshot gates).
    console.registerCVar("r_csm",        "0",     "cascaded shadow maps (0 = legacy single 45 m cascade, bit-exact)");
    console.registerCVar("r_csm_lambda", "0.75",  "CSM practical-split blend: 0 = uniform slices, 1 = logarithmic");
    console.registerCVar("r_csm_dist",   "250.0", "CSM shadow distance in meters (view depth the cascades cover)");
    console.registerCVar("r_csm_blend",  "0.12",  "CSM cross-fade band between cascades, as a fraction of a slice (0 = hard edges)");
    console.registerCVar("r_csm_debug",  "0",     "CSM debug: step shadow visibility per cascade so the cascade bands are visible");
    // The cheap interim + A/B reference: push the LEGACY single cascade forward
    // along the camera axis so the shadowed region leads the car instead of being
    // centred on it. Independent of r_csm. 0 = the historical camera-centred box.
    console.registerCVar("r_shadowforward", "0.0", "slide the LEGACY single shadow cascade forward along the camera axis (meters); 0 = historical");
    // ---- DISCRETE MESH LOD (Lane 5): r_meshlod / r_meshlod_err / r_meshlod_hyst.
    x3::game::registerLodCVars(console);
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
    // [P3-5] LOG-1: push the combat/AI log-spam cvars into their gate flags (this
    // function is the per-frame cvar sync hub, rtao naming notwithstanding).
    x3::game::setCombatLogEnabled(console.getInt("combat_log") != 0);
    x3::game::setAiLogEnabled(console.getInt("ai_log") != 0);
    x3::rhi::IRenderDevice::RtaoParams p{};
    p.enabled  = console.getInt("r_rtao") != 0;
    p.radius   = console.getFloat("r_rtao_radius");
    p.rays     = console.getInt("r_rtao_rays");
    p.strength = console.getFloat("r_rtao_strength");
    device.setDebugView(console.getInt("r_debugview"));
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
    // Cinematic filmic post master gate (r_filmic, default 1 = allowed). The look
    // itself only turns ON while a cutscene holds setFilmic(); this is the live
    // A/B kill-switch (r_filmic 0 forces the byte-identical composite path).
    px.filmicAllowed = console.getInt("r_filmic") != 0;
    device.setPostFX(px);
    // Metal ambient-specular floor (live; default 1.0 = on, 0 = off).
    device.setMetalAmbient(console.getFloat("r_metalambient"));
    // Clustered froxel forward lighting (live). 0 keeps the legacy 64-light UBO
    // loop, which is the bit-exact fallback every image gate is pinned to.
    device.setClusterLights(console.getInt("r_clusterlights") != 0);
    // SSR / RT reflections (live). The device additionally gates on TAA being
    // active (the TAA history is the pass's color source) and tier-gates the
    // ray-query fallback on RT hardware support (Pascal = SSR-only automatically).
    x3::rhi::IRenderDevice::ReflectionParams rf{};
    rf.ssr        = console.getInt("r_ssr") != 0;
    rf.rtFallback = console.getInt("r_rtreflections") != 0;
    rf.fullRes    = console.getInt("r_reflquality") != 0;
    rf.intensity  = console.getFloat("r_reflintensity");
    // Denoise stage (live). 0 disables the passes entirely and restores the
    // pre-denoise, bit-exact behaviour.
    rf.denoiseIters      = console.getInt("r_refldenoise");
    rf.denoiseDepthSigma = console.getFloat("r_refldn_depth");
    rf.denoiseNormalPow  = console.getFloat("r_refldn_normal");
    rf.denoiseDiscScale  = console.getFloat("r_refldn_disc");
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
    // CASCADED SHADOW MAPS (r_csm). enabled = false reproduces the legacy single
    // 45 m cascade EXACTLY. forwardBias is independent of it — it slides that
    // legacy box forward along the camera axis (the cheap racing interim).
    x3::rhi::IRenderDevice::CsmParams csmp{};
    csmp.enabled     = console.getInt("r_csm") != 0;
    csmp.lambda      = console.getFloat("r_csm_lambda");
    csmp.distance    = console.getFloat("r_csm_dist");
    csmp.blend       = console.getFloat("r_csm_blend");
    csmp.forwardBias = console.getFloat("r_shadowforward");
    csmp.debug       = console.getInt("r_csm_debug") != 0;
    device.setCsmParams(csmp);
    // DISCRETE MESH LOD (Lane 5): a CPU-side policy, so it lands in the process
    // policy rather than on the device. Scene::render reads it each frame.
    x3::game::applyLodCVars(console);
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

// ===========================================================================
// THE MUZZLE — Tim 2026-07-11: "the fire doesn't come from the barrel" (ALL guns)
// ===========================================================================
// This is the ONE origin every fire consumer must use: the muzzle flash, the tracer, the
// projectile spawn, the 3D fire audio. It asks the ARSENAL for the barrel tip of the
// weapon it is actually DRAWING — same world frame, same per-weapon vm* pose, same scale
// (Arsenal::currentMuzzle) — so the fire leaves the gun whichever weapon is held, at any
// pitch/yaw.
//
// The old muzzleFromCamera() (kept below ONLY as the no-viewmodel fallback) derived a
// point from the CAMERA plus a fixed fwd/right/down guess. It had no idea where any
// weapon's barrel was, and the guns are wildly different lengths (the shotgun GLB is a
// 4.4 m model!) — so every gun's FX spawned in mid-air, each wrong by its own amount.
// The live vm_* console cvars are passed through as DELTAS from the baked defaults,
// exactly as the draw call does, so a console-nudged gun keeps its muzzle.
x3::phys::Vec3 weaponMuzzle(const x3::game::Arsenal& arsenal, const x3::con::IConsole& console,
                            float ex, float ey, float ez, float yaw, float pitch) {
    if (!arsenal.viewmodelsLoaded())
        return muzzleFromCamera(ex, ey, ez, yaw, pitch);   // fallback: no GLB loaded
    const VmPose p = readViewmodelPose(console);
    return arsenal.currentMuzzle(ex, ey, ez, yaw, pitch,
        p.yawRad   - x3::game::kVmDefYawDeg   * kDegToRad,
        p.pitchRad - x3::game::kVmDefPitchDeg * kDegToRad,
        p.rollRad  - x3::game::kVmDefRollDeg  * kDegToRad,
        p.fwd   - x3::game::kVmDefFwd,
        p.right - x3::game::kVmDefRight,
        p.down  - x3::game::kVmDefDown);
}

// W-RIFT: printable codepoints for the RIFT CONSOLE's typed TARGET field, in the
// canon game loop. The dev host (--world rifthub) keeps its own ring; the canon loop
// needs one too, because ui::textField only sees characters through UiInput::typed.
// Filled by charCallback, DRAINED once per frame by the console (and by nobody else,
// so it can never accumulate).
unsigned int g_riftTyped[x3::ui::UiInput::kMaxTyped] = {};
int          g_riftTypedN = 0;

// GLFW character callback: feed printable codepoints to the console input line.
void charCallback(GLFWwindow* win, unsigned int codepoint) {
    auto* ctx = static_cast<InputContext*>(glfwGetWindowUserPointer(win));
    if (ctx && ctx->hud) ctx->hud->onChar(codepoint);
    if (codepoint >= 32 && codepoint < 127 && g_riftTypedN < x3::ui::UiInput::kMaxTyped)
        g_riftTyped[g_riftTypedN++] = codepoint;
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
        case GLFW_KEY_PAGE_UP:   hud.consoleScroll(+5); break;   // scroll back through history
        case GLFW_KEY_PAGE_DOWN: hud.consoleScroll(-5); break;   // scroll toward the live bottom
        case GLFW_KEY_HOME:      hud.consoleScroll(+100000); break;  // jump to oldest (clamped)
        case GLFW_KEY_END:       hud.consoleScroll(-100000); break;  // jump to live bottom
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

// ---------------------------------------------------------------------------
// [P0-2] THE `--world` MODES THIS DEFAULT HOST HANDLES — the one list, kept in
// THIS file so it cannot quietly diverge from the branch bools below
// (introCellWorld / oceanWorld / terrainWorld / elevatorWorld / canonWorld /
// docWorld — search `worldMode ==` in runDefaultHost). Exported to the
// destination-registry self-test (app/destinations.cpp), which asserts every
// one of these is a registry row or a reasoned exclusion, and that every
// registry worldFlag is dispatched. If you add a `worldMode == "x"` branch,
// add "x" HERE (same edit, same file) and give it a registry row — the gate
// goes RED otherwise, and an unlisted flag would silently fall back to the
// legacy Level-1 build (the "any unrecognized --world lands here" rule).
// ---------------------------------------------------------------------------
namespace {
const char* const kDefaultHostWorldModes[] = {
    "canonlevel",   // canonWorld — the data-driven canonical tower (THE game)
    "intro",        // canonWorld entered through the cold-open prologue
    "level1",       // legacy hand-coded spire (also the unrecognized-flag fallback)
    "elevator",     // elevatorWorld — Level-1 build, spawn at the elevator
    "terrain",      // terrainWorld — B2 outdoor tiled terrain
    "ocean",        // oceanWorld — terrain + animated sea
    "fromdoc",      // docWorld — boot a LevelDoc JSON (editor loop)
    "spacestation", // docWorld alias — space_station.leveldoc.json (cli.cpp seeds it)
};
} // namespace

const char* const* defaultHostWorldModes(unsigned& count) {
    count = (unsigned)(sizeof(kDefaultHostWorldModes) / sizeof(kDefaultHostWorldModes[0]));
    return kDefaultHostWorldModes;
}

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
    // Skip-intro: the --skipintro CLI flag OR the persisted Settings > Advanced
    // toggle. Either path wins; F9 still skips a running intro at any time.
    const bool skipIntro = hc.skipIntro || x3::apphost::readSkipIntro();
    // LEVEL EDITOR: no longer boot-only. It can be entered LIVE from the pause menu
    // (dev row, cvar ui_editor), so this is no longer const — see the wantEditor()
    // handler below, which lazy-inits ImGui the first time it is actually opened.
    bool editorMode = hc.editorMode;
    bool editorInited = false;
    const bool fxDemo = hc.fxDemo;
    const bool fxLightning = hc.fxLightning;   // --fx-lightning: bolt ACROSS the view
    const bool uiDemo = hc.uiDemo;
    const std::string& uiDemoPath = hc.uiDemoPath;
    const std::string& uiDemoScreen = hc.uiDemoScreen;
    const bool dialogShot = hc.dialogShot;
    const bool alertShot = hc.alertShot;
    const bool shotDrive = hc.shotDrive;   // WORLD CARS driver-POV staging
    const bool captureSpire = hc.captureSpire;
    const std::string& captureSpireDir = hc.captureSpireDir;
    const bool captureWings = hc.captureWings;
    const std::string& captureWingsDir = hc.captureWingsDir;
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
    // W2-A2 (punch-list P1 #10): REAL footsteps. W2-B committed 4 concrete takes
    // (assets/audio/footsteps/); the old behavior was the GUNSHOT pitched down.
    // sndStep (first valid take) feeds the shared enemy cue sink; the player
    // cadence below cycles all 4 takes. Falls back to the legacy pitched gunshot
    // only if none of the takes loaded (clean-machine grace).
    const x3::audio::SoundHandle sndStep =
        bootAudio.footstepConcrete[0].valid() ? bootAudio.footstepConcrete[0] : sndGun;
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

            if (outcome == x3::intro::IntroOutcome::CapitalKilled) {
                // KILL PATH (owner canon 2026-07-27: "kill big ship.. it crashes...
                // i land.. recover tech and prisoners from it.. break IN to Lab
                // zero"). Earned, never rolled. The dreadnought is down on the
                // surface and StoryFlags["intro.wreck"] is set beside
                // ["intro.landed"], so Act-1 starts at the CRASH SITE: salvage the
                // wreck's tech, free the prisoners in its hold, then breach Lab
                // Zero from outside. It shares the surface world host with the
                // escape path (same "land outside the facility, free and armed"
                // shape) — the wreck flag is what makes the start differ. Same
                // teardown contract as the escape branch below.
                x3::logInfo("[intro] CAPITAL_KILLED -> crash-site Act-1 "
                            "(host_surface_start + intro.wreck: salvage -> prisoners "
                            "-> breach Lab Zero)");
                loading.shutdown(*device);
                physics->shutdown();
                hc.worldMode = "surface";
                return x3::apphost::dispatchWorldHost(hc);
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
    // `spacestation` is a thin alias over the LevelDoc loader (cli.cpp seeds its
    // docWorldPath). Same live-edit/hot-reload path; its doc's biome "space"
    // drives the deep-space sky + distant-Sol bodies set up after the doc loads.
    const bool docWorld = (worldMode == "fromdoc" || worldMode == "spacestation");
    // Hard cap on how many rooms the portal flood-fill may add per frame. Even down the
    // longest sightline with a deep r_culldepth, the cull stays well under the whole 53-room
    // tower so the GPU never spikes (the spec's "must NOT regress to drawing the tower").
    constexpr uint32_t kCanonRoomBudget = 18;
    x3::game::CanonFloor canonFloor;           // parsed+resolved Floor 1 (canonWorld only)
    std::vector<uint32_t> canonVisRooms;       // per-frame PVS scratch (canonWorld only)
    std::vector<x3::game::CanonLight> canonLights; // per-room ceiling lights (canonWorld only)
    x3::game::CellDressing canonDressing;      // opening-space set-dressing + motivated lights (canonWorld only)
    x3::game::RoomDressing canonRooms;         // WAVE-3 recipe dressing for the other 52 rooms (canonWorld only)
    x3::game::FacilityExterior facilityExterior; // SEAM 2: the glass exterior wrapping the REAL tower (canonWorld only)
    x3::game::DoorSystem  canonDoors;          // SM_Door_A GLB doors at the cut doorways
    // ---- Keycard / keypad door gating (canonWorld). keycardMask = bitmask of held
    // keycard ids; the Security keycard is a glowing pickup in the Research Lab. ----
    uint32_t keycardMask       = 0;
    uint32_t canonKeycardEnt   = x3::game::kNoLink;
    float    canonKeycardX     = 0.0f, canonKeycardZ = 0.0f;
    bool     canonKeycardTaken = false;
    x3::game::CanonPlay   canonPlay;           // canon Floor-1 gameplay (canonWorld only): sidearm + animated enemies + Martinez + 3 girls
    x3::game::BarrelSystem canonBarrels;       // WAVE (cell-door): explodable barrels in the canon cell/hall (DJBooth fireball on shot)
    x3::game::DescMechanics descMech;          // W9-1: the desc-field Tier-A mechanics (coolant/EMP/hack/cold/antidote); built after chatTrees (flags owner) exists
    bool coolantGlowDead = false;              // W9-1: the coolant console glow was killed (one-shot on the sabotage edge)
    // ---- [W9-3 RPG] the RPG layer: item DB + backpack + XP/levels + skills. ----
    // Data loads at boot (JSON-or-baked); the live stat block (rpgMods) is
    // recomputed by applyRpgStats() whenever skills/mods change and is READ at
    // the fire sites / player / arsenal — base tables are never mutated.
    x3::game::ItemDb      itemDb;
    x3::game::Inventory   inventory;
    x3::game::Progression progression;
    x3::game::SkillTree   skillTree;
    x3::game::RpgUi       rpgUi;
    x3::game::PlayerStatMods rpgMods;          // folded live stats (skills + applied mods)
    std::vector<std::string> rpgAppliedMods;   // weapon-mod item ids applied to the loadout
    uint32_t rpgCritRng = 0xC0FFEEu;           // crit-roll xorshift state (fire sites)
    itemDb.load(x3::game::itemsJsonPath());
    skillTree.load(x3::game::skillTreeJsonPath());
    x3::game::Canon45     canon45;             // W5-1: LEVEL 4.5 — the Nexus Chamber (cavern + climb + whispers + apex)
    x3::game::FacilityStairwell stairwell;     // fix/spire-hollow-core: open switchback stairwell (F1..F7, skips 4.5)
    x3::game::StairwellLayout   stairLayout;   // its shared plan (breach wiring + lint agree)
    x3::game::StairNavChain     stairNav;      // feat/stair-nav: enemy waypoint chain over the stairwell
    bool                  canonMedicalActive = false;  // latch: the medical-bay rescue clock was started (player reached the wards)
    // --world fromdoc: the LevelDoc-built world + its hot-reload state (docWorld only).
    x3::game::LevelDocWorld docLevel;
    bool docReloadRequested = false;           // set by the `level_reload` console cmd
    // --world spacestation (LevelDoc biome "space"): deep-space sky bodies. The
    // station is FAR from Earth, so Sol/Earth ride the sky as a tiny faint star,
    // a local star is the sun, and the FORGE3D planets are small. Bodies are
    // direction-anchored (parallax-free) and re-projected on the eye each frame.
    // TODO(star-systems): adopt the feat/star-systems registry for this system's
    // name + real body catalog when it lands; hardcoded "distant Sol" for now.
    bool spaceBiome = false;
    x3::rhi::MeshHandle spacePlanetMesh{}, spacePlanetRingMesh{};
    std::vector<NightSkyPlanet> spacePlanets;
    x3::game::Scene scene;
    x3::game::Level1Game game;
    // LIVING WORLD: facility civilians (detained workers) — a small crowd that
    // idles/wanders in the B1 arena hall, scatters + cowers when shots ring out,
    // and drifts back once it goes quiet. Built only in the legacy Level-1 world.
    x3::game::CrowdSystem facilityCrowd;
    // LIVING NPCs (Tim: "NPCs who interact with each other and are seen working,
    // playing, living life") — the canon facility population, room-tagged for
    // the PVS: [0] Main Hall fringe (converse knots + a sweeper + a console
    // tender), [1] Bottom Hall work crew (crate carry + sweep + console),
    // [2] Entrance fringe (a seated hand-game pair). ~12 agents total.
    x3::game::CrowdSystem canonCrowds[3];
    // LIVING NPCs — the streamed CITY crowds: [0] New District sidewalk
    // (wanderers + conversation knots), [1] the warehouse-dock work crew
    // (crates on the industrial edge), [2] Scrapyard plaza (kickabout knot +
    // wanderers). Built INSIDE the city region realize via the WorldStreamer
    // region hooks — the region's ownership ledger owns every entity/mesh, and
    // the teardown hook abandon()s them before any slot is released.
    x3::game::CrowdSystem cityCrowds[3];
    // SKINNED CITIZENS — the crowds' skinned visual layer (app/crowd_skin.h):
    // one inert rigged GLB character per agent, pose-following the CrowdSystem
    // brains (Walk/Idle clips from the agent's own speed, gestures on top).
    // HOST-OWNED, PERSISTENT pools (the parked-cars doctrine — a loaded rig
    // must never land in a region ledger): facility skins spawn DEFERRED
    // 1/frame after boot; city skins spawn deferred after the region realize
    // and survive stream-out/in cycles (deactivate on teardown, re-attach with
    // zero reloads on re-realize). A failed rig keeps that agent's blockout.
    x3::game::CrowdSkin canonCrowdSkins[3];
    x3::game::CrowdSkin cityCrowdSkins[3];
    // LIVING CITY (npc_life.h): the SOUL layer over the ambient street crowd —
    // authored occupation NPCs (12 archetypes) that walk real daily schedules to
    // home/work/leisure posts, with scan-card personas + the bank-robbery set-piece.
    // Host-owned + PERSISTENT (built once the `city` region is resident, PVS-culled
    // with the district via cfg.roomId), rendered as plain Scene entities SEPARATE
    // from MonsterSystem (citizens, not monsters — Echo Harbor's explicit need).
    x3::game::NpcLife cityNpcLife;
    // CROWD CHATTER — "hear the people talk.. mumble.. see it in chat bubbles
    // over their heads" (app/crowd_chatter.h): a deterministic voice layer over
    // each crowd deployment. Converse pairs trade authored 2-6 word lines in
    // rhythm with the turn-taking gesture bobs (bubble over the speaker +
    // murmur walla at the pair midpoint); workers grumble; kickabouts shout.
    // Facility rooms whisper (detainee tables), streets gossip.
    x3::game::CrowdChatter canonChatter[3];
    x3::game::CrowdChatter cityChatter[3];
    // STREET LIGHT (street_lights.h): real street lamps — pooled PointLights +
    // additive light cones + emissive ground pools, warm sodium / cool LED
    // color story, ~8% dead / ~5% flickering. City lamps build INSIDE the city
    // region realize (ledger-owned, the crowds/cars hook); apron + Spire-
    // approach lamps are host-owned. The light merge below feeds the nearest
    // K=14 lit lamps AFTER the facility/strata/club obligations.
    x3::game::StreetLights streetLights;
    for (int ci = 0; ci < 3; ++ci) {
        canonChatter[ci].init(x3::game::ChatterVenue::Facility, 101u + (uint32_t)ci);
        cityChatter[ci].init(x3::game::ChatterVenue::Street, 201u + (uint32_t)ci);
    }
    // The committed walla takes (tools/gen_crowd_chatter.py -> assets/audio/
    // crowd/). Three tiny mono WAVs; a miss loads invalid and that cue is
    // silent (clean-machine grace). Boot cost logged (it is ~a millisecond).
    x3::game::ChatterSounds chatterSnd;
    {
        const auto cs0 = std::chrono::steady_clock::now();
        chatterSnd.murmurA = audio->load(x3::game::resolveAudio("crowd/murmur_a.wav"));
        chatterSnd.murmurB = audio->load(x3::game::resolveAudio("crowd/murmur_b.wav"));
        chatterSnd.grumble = audio->load(x3::game::resolveAudio("crowd/grumble_low.wav"));
        const double csMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - cs0).count();
        x3::logInfo("[chatter] walla takes loaded (murmur_a/murmur_b/grumble_low) in " +
                    std::to_string(csMs) + " ms");
        x3::boot::mark("crowd chatter (walla WAVs)");
    }
    // WORLD CARS — findable/drivable/hackable vehicles in the one world (see
    // world_cars.h). Host-owned; the apron/approach cars park at boot, the city
    // cars park/unpark with the `city` region via the WorldStreamer hooks (the
    // system owns only its OWN static bodies — nothing enters a region ledger).
    x3::game::WorldCars worldCars;
    // FISH (app/fish.h) — ambient schools in THE RIVER's reach past the facility
    // and in the sea shallows at the estuary. Host-owned (NOT region-ledger
    // owned: it is built once at canon boot like the parked cars), entities
    // tagged kStreamedExteriorRoom so the outdoor PVS gates the draw, schools
    // range-gated on the player. Kinematic — no physics bodies.
    x3::game::FishSystem worldFish;
    x3::game::SeaLifeSystem worldSea;      // THE OCEAN LIVES: big animals (sharks + the squid)
    // CANON ALIENS (app/canon_aliens.h) — the four Rodin species placed on the
    // planet per the lore-faction map: Saurian Soldiers patrol the facility
    // perimeter, Grey workers dot the crash-site approach, the Nordic Steward
    // watches from the ridge, the Mantis Arbiter stalks the pass. Host-owned
    // like the fish (built once at canon boot); hostile rows fight the player
    // through the same MonsterManager lane the canon enemies use. The Saurian
    // WARLORD boss only spawns under X3_SPAWN_WARLORD=1 (dev flag).
    x3::game::MonsterManager canonAliens;
    // GOD RAYS (app/god_rays.h) — sun shafts under the water surface (river
    // reach + estuary shallows), additive glass, host-owned like the fish.
    x3::game::GodRays worldRays;
    // THE WATER ZAP (app/waterzap.h) — "one Zap": the latch + cooldown that keeps
    // a HELD lightning trigger from re-zapping the river every frame.
    x3::game::WaterZapper waterZapper;
    // The zap's transient surface FX: while > 0 the water surface at zapFxCenter
    // spiders re-randomized lightning arcs out to kWaterZapRadius AND the water
    // itself FLASHES — a radial-gradient disc laid on the surface (the street-
    // lamp ground-pool material: additive glass + a radial gradient, so it reads
    // as the POOL lighting up, and rides the blend tail => no depth write).
    float zapFxTimer = 0.0f;
    x3::phys::Vec3 zapFxCenter{};
    uint32_t zapFxRng = 0x5EEDu;
    uint32_t zapFlashEnt = x3::game::kNoLink;   // the water-flash disc
    float carPanicCooldown = 0.0f;   // throttles the drive-by crowd-panic probe
    float carThrottleHud   = 0.0f;   // last driver throttle (engine-loop pitch)
    // LIVING WORLD: the FACILITY ALERT LEVEL (the wanted system, pillar 3). The
    // host feeds it observations (guard positions, LOS, gunshots, bodies, keypad
    // tampers) and applies its effects (reinforcement spawns, the level-3 door
    // LOCKDOWN via alertDoorLock, red-shifted lights, the HUD indicator).
    x3::game::AlertSystem   facilityAlert;
    x3::game::AlertDoorLock alertDoorLock;
    bool  facilityAlertOn = false;   // armed in level1 AND canonlevel (see the world-build arm)
    float alertHudClock   = 0.0f;    // drives the lockdown HUD pulse

    // ---- VIGIL BARKS: the ambient in-ear companion layer. Proactive one-liners on
    // real game events (alert changes, first combat, low HP, elevator/club entry,
    // idle), shown as an on-screen toast. CANON-GATED: barks are SILENT until Jake
    // acquires the neural link (the vigilLink StoryFlag). Terminal chat works before
    // then; this overlay is what the link unlocks. Tuned by vigil_chatter (off/
    // occasional/chatty) + vigil_cooldown. State tracked frame-to-frame below.
    x3::game::VigilBarks vigilBarks;
    int   vigilPrevAlert    = 0;       // last-seen alert level (edge-detect rising/clear)
    bool  vigilSawCombat    = false;   // FirstCombat one-shot
    bool  vigilLowHpLatch   = false;   // LowHealth re-arms only after HP recovers
    bool  vigilSawElevator  = false;   // EnterElevator one-shot per session
    bool  vigilSawClub      = false;   // EnterClub one-shot
    bool  vigilSawSidearm   = false;   // PickupSidearm one-shot
    bool  vigilSawTrapdoor  = false;   // Trapdoor one-shot
    bool  vigilLinkPrev     = false;   // detects the link being acquired (welcome bark)
    float vigilClock        = 0.0f;    // monotonic seconds for the bark cooldown/toast
    x3::phys::Vec3 vigilPrevPos{0,0,0}; // last frame's cam pos (idle/movement detection)
    bool  vigilPrevPosSet   = false;
    int   vigilPrevRoom     = -999;    // last canon room id (EnterArea on change)
    // B3: the terrain world is now STREAMED around the player via a residency
    // ring (TerrainStreamer) fed by the engine job system. Both are only created
    // in terrain mode; Level 1 is unaffected.
    x3::game::TerrainStreamer terrainStreamer;
    std::unique_ptr<x3::jobs::IJobSystem> terrainJobs;
    // ---- SEAM 3 (world merge): the PLANET STREAMS IN around the canon facility.
    // The WorldStreamer's outdoor lanes (city / ocean base / surface landmarks)
    // become streamed regions of the canon master world — walk off the apron and
    // the world is simply there, no loading screens. The canon graph
    // (regions.canon.json) drops spire_f1 (canonWorld builds the real tower);
    // terrainStreamer doubles as the canon ground ring (keep-out under the
    // facility's own apron/soil so nothing z-fights the interior floors).
    // Booted after the world build ONLY when the SEAM-2 exterior exists (the
    // breach is the one way outside — without it there is no outdoors to stream).
    x3::game::WorldRegionGraph canonRegionGraph;
    x3::game::WorldStreamer    canonWstream;
    bool  canonStreamOn = false;          // SEAM 3 streaming live this run
    float canonStreamPrevX = 0.0f, canonStreamPrevZ = 0.0f;   // velocity feed
    // Risk 3 (XZ-only residency vs the underground): suppress ALL residency work
    // while the eye is below this Y — the elevator/strata/Club-1127 descent must
    // never see a region teardown or the streamer's Y=0 proxy floor.
    constexpr float kStreamSuppressBelowY = -20.0f;
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
    // ---- R-3 fold (ed1a403): LIVE STRATA DESCENT. The geological shaft built AROUND
    // the elevator's descent column (building base Y~=0 down to Club 1127 at Y=-200)
    // so the cab's glass sees REAL layered rock on the 1127 disco descent. The in-cab
    // strata readout matches by construction (ElevatorSystem::strata() and
    // StrataWorld::bandAtY share band names/colors). Level-1 only.
    x3::game::StrataWorld    liveStrata;
    x3::game::TriggerSystem  liveStrataTriggers;
    bool                     liveStrataBuilt = false;
    // ---- W-RIFT (ONE WORLD): THE RIFT HUB IS A PLACE IN THE WORLD ------------------
    // It was reachable only via `--world rifthub`. It is now SUB-LEVEL R1: a real
    // elevator stop under the detention level (locked behind access code 4790), a
    // bored LANDING at the shaft, a long industrial APPROACH that turns a corner (the
    // hum and the blue glow arrive before the room does), and the hub itself — the
    // SAME Rifthub module the dev world builds, authored at a region origin. The dev
    // shortcut still works; per docs/design/WORLDS.md it is only ever a shortcut.
    x3::game::Rifthub        rifthub;
    x3::game::TriggerSystem  riftTriggers;
    x3::game::RiftDepths     riftDepths;
    // WHERE THE PLAYER LEARNS 4790: a maintenance log on a HoloTerminal in the
    // Security room. The code is FOUND, not handed over — the same rule the cell
    // terminal's 1278 follows (the loading-screen tips spoil that one; this one is
    // only in the world).
    x3::game::HoloTerminal   riftLore;
    // ---- THE SECRET-CODE QUEST CHAIN (feat/secret-code-clues) ----------------------
    // CLUE 1: Okafor's maintenance work order by the F1 stairwell entrance (Bottom
    // Hall) — teaches the stairwell service code 4545 ("re-keyed 45-45 after the
    // incident"). CLUE 2: the chief engineer's log on F4 near the elevator lobby —
    // teaches the 4.5 cab code 4455 by riddle ("double the four, double the five").
    // Both are HoloTerminals on the HoloPanel platform, the riftLore 4790 pattern:
    // the code is FOUND, never handed over. (The owner's 7762 master backup is
    // deliberately taught NOWHERE, by order.)
    x3::game::HoloTerminal   stairLore;
    x3::game::HoloTerminal   liftLore;
    // X3_STAIR_DEMO capture staging: the phantom keypad's 4545 response line for
    // the still's HUD (set at build; drawn by the capture HUD block).
    std::string              stairDemoBark;
    bool  riftBuilt    = false;
    bool  riftZonePrev = false;      // edge: restore the room-recipe atmosphere on exit
    float riftTeleCool = 0.0f;       // seconds before a rift may take you again
    x3::phys::Vec3 riftRegMin{}, riftRegMax{};   // hub + depths, one AABB (the "in the zone" test)
    // Sub-level R1's floor: below B1, inside the Basalt band, and deliberately clear of
    // the strata offshoots (Granite -55 / Basalt -95) so the corridor bores through
    // rock and nothing else.
    constexpr float kRiftFloorY = -78.0f;
    // The region's light rig is bigger than the device's 64-light cap (the hub alone
    // ships ~57: gate cores + keys + fills + the hall grid). Appending the corridor's
    // strips after them TRUNCATED THEM AWAY — the first capture of the approach came
    // back pitch black. So the pool is selected NEAREST-TO-EYE across both rigs: the
    // lights that matter are the ones in the room you are standing in.
    // ===================================================================
    // CONTENT WIRING (lane inspx/content-wiring) — THE LIGHT BUDGET.
    //
    // Clustered forward lighting raised the DEVICE cap from 64 to
    // kMaxSceneLights (1024, engine/rhi/ClusterLights.h). But every CPU-side
    // budget in this file was sized against the LEGACY 64-entry UBO array and
    // truncated unconditionally, so `r_clusterlights 1` changed nothing in a
    // real world: the frame still handed the device <=64 lights. The feature
    // shipped and nothing could reach it.
    //
    // These are now VARIABLES refreshed once per frame from the cvars
    // (refreshLightBudgets(), called from the frame loop below). With
    // `r_clusterlights 0` — the default — every one of them holds EXACTLY the
    // constant it used to be, so the legacy path is bit-for-bit unchanged and
    // every md5 / screenshot gate stays green. With `r_clusterlights 1` they
    // scale to r_maxlights.
    //
    // Declared HERE (ahead of the lambdas that capture them) because `console`
    // itself is not constructed until the S7 block much further down; the
    // lambdas capture by reference and therefore see each frame's refresh.
    // ===================================================================
    size_t   lightBudget      = 64;   // final CPU cap before setPointLights
    size_t   fixtureBudget    = 44;   // nearestFixtures K (legacy tower ceiling)
    uint32_t canonLightBudget = 36;   // selectVisibleCanonLights K
    uint32_t streetLampK      = 14;   // StreetLights::selectLights K
    uint32_t docLightK        = 16;   // LevelDocWorld::selectLights K
    size_t   strataLightK     = 16;   // strata mood lights taken while camY < 2

    // ---- CONTENT WIRING: r_citylights is a BOOT cvar ------------------------
    // The city lamps are GEOMETRY, built once inside the `city` region realize
    // -- and that realize (buildStartRegions) runs well before the console is
    // constructed, so there is nothing to read a cvar out of yet. Take the
    // decision straight off the --set list instead. The cvar is still
    // registered below so `r_citylights` reports the truth in-game; changing it
    // at runtime would need a region rebuild, which is not a thing this does.
    bool cityLightsDense = false;
    for (const auto& kv : cliCVars)
        if (kv.first == "r_citylights" && kv.second != "0") cityLightsDense = true;

    auto riftLights = [&](float x, float y, float z,
                          std::vector<x3::rhi::PointLight>& out) {
        out.clear();
        for (const auto& L : rifthub.pointLights())    out.push_back(L);
        for (const auto& L : riftDepths.pointLights()) out.push_back(L);
        auto d2 = [&](const x3::rhi::PointLight& L) {
            const float dx = L.pos[0] - x, dy = L.pos[1] - y, dz = L.pos[2] - z;
            return dx * dx + dy * dy + dz * dz;
        };
        std::sort(out.begin(), out.end(),
                  [&](const x3::rhi::PointLight& a, const x3::rhi::PointLight& b) {
                      return d2(a) < d2(b);
                  });
        if (out.size() > lightBudget) out.resize(lightBudget);
    };
    auto riftInZone = [&](float x, float y, float z) {
        return riftBuilt && x >= riftRegMin.x && x <= riftRegMax.x &&
               y >= riftRegMin.y && y <= riftRegMax.y &&
               z >= riftRegMin.z && z <= riftRegMax.z;
    };
    // Elevator-bar gap C (audit claim 392f6e4d): THE CLUB AT THE BOTTOM. The finished
    // Club1127World (--world club) was never instantiated in the game path — the 1127
    // disco descent parked the cab in bare rock at Y=-200. Built LAZILY on the first
    // accepted 1127 code (the long disco-slow ride masks the build), ticked/drawn once
    // built; on arrival with the doors open the rider is handed off to club.spawn()
    // (the shell is centered at the XZ origin, west of the shaft — the documented
    // "drops the player at spawn()" design in club1127.h).
    x3::game::Club1127World  club1127;
    bool                     club1127Handoff = false;
    // fix/club-relight: latches while the player is down in The Deep so the interior
    // ambient/IBL lift (a dim-but-readable neon club, matching --world club) is applied
    // ONCE on entry and restored to the world air ONCE on exit — never per-frame.
    bool                     clubAtmoOn = false;
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
    // ---- B4 — THE LEGACY TOWER'S CEILING LIGHTS NEVER REACHED THE GPU -------------
    // env_art registers a point light at EVERY Light_A ceiling fixture in the legacy
    // tower: **332 of them**. The device cap is kMaxPointLights = 64, and both light
    // feeds seeded `fl` with the WHOLE fixture list and then did `fl.resize(64)` — which
    // keeps the FIRST 64 IN BUILD ORDER. Build order is room order, so the 64 survivors
    // are whatever rooms happened to be authored first; the room the player is standing
    // in almost never had a light among them, and 268 fixtures were dropped silently
    // (setPointLights min()s the count without a word).
    //
    // Level 1 has therefore been lit by the 0.42 ambient wash + the flashlight, and by
    // essentially NOTHING ELSE, for its whole life. Kill the wash and it goes to a VOID
    // (measured: meanLum 24.3 -> 6.0, p95 8.8) — not because the level lacks lights, but
    // because its lights were being thrown away. THE PATTERN, exactly: the crutch hid it.
    //
    // Feed the NEAREST fixtures to the eye instead of the first ones in an array. K=44
    // leaves headroom under the 64 cap for the flashlight, the elevator cab, the alert
    // strobes and the club/rift takeovers, all of which are appended after this.
    auto nearestFixtures = [](const std::vector<x3::rhi::PointLight>& src,
                              float ex, float ey, float ez, size_t k) {
        std::vector<x3::rhi::PointLight> out = src;
        if (out.size() <= k) return out;
        std::partial_sort(out.begin(), out.begin() + (long)k, out.end(),
            [&](const x3::rhi::PointLight& a, const x3::rhi::PointLight& b) {
                const float ax = a.pos[0]-ex, ay = a.pos[1]-ey, az = a.pos[2]-ez;
                const float bx = b.pos[0]-ex, by = b.pos[1]-ey, bz = b.pos[2]-ez;
                return (ax*ax+ay*ay+az*az) < (bx*bx+by*by+bz*bz);
            });
        out.resize(k);
        return out;
    };
    // CANONLEVEL per-frame room-light budget. Was a hard-coded 16 at all three feed
    // sites — which a 40 m cell hall (now 5 ceiling lights of its own, plus whatever the
    // PVS pulls in through open doors) exhausts instantly, so the far half of the
    // corridor stayed dark even after the lights existed. 36 is sized against the 64-light
    // device cap with EVERY other claimant paid first:
    //   flashlight 2 + cell-dressing motivated ~8 + canon 36 + breach spill 1 = 47,
    //   + street lamps 14 (outdoors only) = 61, or + strata 16 (camY < 2 only) = 63.
    // Both worst cases still clear 64, and the tail the cap would trim is the FARTHEST
    // light either way (every feed is nearest-first).

    auto shutdownGameSystems = [&]() {
        game.shutdown();                               // every enemy group + Martinez + barrels
        nexus.shutdown();                              // F4.5 Chorus pod ragdolls
        // RAGDOLL-TEARDOWN GAP FIX: the Spire floor hosts each own several MonsterManagers
        // (mid F3/F4/F5 + bosses; top F6/F7 + Overseer/Clone/Sarah; the hidden sub-levels +
        // Frozen Collective/Chen). A monster killed in the last ~0.7 s on any of those
        // floors is mid-flop with LIVE Jolt ragdoll bodies; without this its IRagdoll would
        // be destroyed during stack unwind AFTER physics->shutdown() (use-after-shutdown,
        // relying only on the W6-1 guard + leaking the JPH::Ragdoll). Tear them down here
        // with the rest. All idempotent + no-ops when nothing died / the descent stayed shut.
        midFloors.shutdown();                          // Spire mid-floor enemy + boss ragdolls
        topFloors.shutdown();                          // Spire top-floor enemy + boss ragdolls
        subLevels.shutdown();                          // hidden sub-level enemy + mini-boss ragdolls
        if (canonPlay.built()) canonPlay.shutdown();
        if (canon45.built()) canon45.shutdown();   // canonlevel enemy ragdolls
        canonAliens.shutdown();                    // CANON ALIENS: planet alien ragdolls
        // W-RIFT: sub-level R1's meshes/textures (the hub's gates, membranes, holo
        // glass; the approach's shell). The smoketest gates on allocationCount == 0,
        // and this runs on EVERY exit path (headless captures included).
        if (rifthub.built())    rifthub.shutdown(*device);
        if (riftDepths.built()) riftDepths.shutdown(*device);
        if (riftLore.built())   riftLore.shutdown(*device);
        if (stairLore.built())  stairLore.shutdown(*device);   // clue 1: Okafor work order
        if (liftLore.built())   liftLore.shutdown(*device);    // clue 2: Vasquez log
        // KNOWN_BUGS L4: Scene lazily creates ONE 1x1 matte-MR texel (the fallback that
        // lets an emissive-mapped entity keep its emissive map). It is the only GPU
        // resource Scene owns, and the smoketest gates on allocationCount == 0.
        scene.releaseGpu(*device);
    };
    // THE ONE ELEVATOR (Tim: "connect this with the In-Game Elevator"): the full
    // x3-elevator.js treatment — 10-state FSM, cabin visuals + twin OLEDs, the
    // committed audio set (muzak/creaks/doors/club track), the 1127 disco descent,
    // Club-1127 stop, and THE DESCENT geology around the shaft — wires identically
    // whichever world built the cab (level1's spire shaft or the canonical tower's
    // Elevator Lobby spine). Everything downstream (E-call, rider carry, keypad
    // 1127, club handoff, prompts) already keys off elevator.built().
    auto soupUpElevator = [&](float shaftX, float shaftZ,
                              const std::vector<std::string>& floorLabels) {
        elevator.enableFsm(true);
        elevator.setAudio(audio.get());
        // PROPER ELEVATOR AUDIO: door open/close (layered hiss+slide takes when
        // committed), an arrival/floor-pass chime, a looped motor hum pitched by
        // cab speed, keypad clicks and a wrong-code buzzer. Every load is graceful:
        // a missing WAV -> invalid handle -> that one cue is silent (never a crash).
        {
            x3::game::ElevatorSounds es;
            es.doorOpen  = sndDoor;          // already loaded at boot (S_ScifiDoor_A)
            es.doorClose = sndDoor;
            es.ding      = audio->load(x3::game::resolveAudio("interact/chime.wav"));
            if (!es.ding.valid()) es.ding = sndPickup;   // fallback: recharge chime
            es.motor     = audio->load(x3::game::resolveAudio("interact/servo_loop.wav"));
            es.keyClick  = audio->load(x3::game::resolveAudio("interact/keypad_click.wav"));
            es.buzz      = audio->load(x3::game::resolveAudio("interact/buzz.wav"));
            es.muzak     = audio->load(x3::game::resolveAudio("interact/muzak_loop.wav"));
            es.creak     = audio->load(x3::game::resolveAudio("interact/cable_creak.wav"));
            { auto hOpen  = audio->load(x3::game::resolveAudio("interact/door_hiss_open.wav"));
              auto hClose = audio->load(x3::game::resolveAudio("interact/door_hiss_close.wav"));
              if (hOpen.valid())  es.doorOpen  = hOpen;
              if (hClose.valid()) es.doorClose = hClose; }
            es.doorThunk = audio->load(x3::game::resolveAudio("interact/door_thunk.wav"));
            es.clubTrack = audio->load(x3::game::resolveAudio("interact/club_track.wav"));
            elevator.setSounds(es);
            elevator.armCableSlip();   // the one-shot freefall scare (never in disco)
        }
        elevator.setClubStopY(x3::game::ElevatorSystem::kDefaultClubFloorY + 0.15f);
        elevator.setFloorLabels(floorLabels);
        elevator.buildVisuals(scene, *device);
        // X3_ELEV_DISCO=1 (+ --screenshot): pre-enter the 1127 code so a headless
        // capture proves the cab's disco cue; the club builds too, exactly as the
        // live keypad path does, so a shot-cam can stand in The Deep.
        if (hc.screenshot && std::getenv("X3_ELEV_DISCO")) {
            elevator.keypadDigit(1); elevator.keypadDigit(1);
            elevator.keypadDigit(2); elevator.keypadDigit(7);
            if (elevator.disco() && !club1127.built())
                club1127.build(scene, *device, *physics, x3::game::riggedGlbRoot());
        }
        // R-3 fold: THE DESCENT around the live elevator column (same XZ), so the
        // 1127 descent shows real geology out the glass.
        liveStrata.build(scene, *device, *physics, liveStrataTriggers,
                         shaftX, shaftZ, /*radius*/16.0f);
        liveStrataBuilt = liveStrata.built();
        x3::boot::mark("elevator + strata descent build");
        x3::logInfo("STRATA descent built around the elevator shaft — enter 1127 to ride to The Deep");
    };
    // ---- W-RIFT / W-MENU: THE FAST-TRAVEL RESOLVER ---------------------------------
    // The hub is "the room where you can teleport to any location", and the world menu
    // asks the SAME question from the pause screen — so this is ONE resolver, keyed by
    // the DESTINATION REGISTRY (app/destinations.h), used by both.
    //
    // It maps a registry key onto a real anchor in the ONE WORLD. It NEVER invents an
    // anchor: a place this world cannot deliver comes back FALSE with a plain-English
    // reason, and the caller (the gate, or the menu) says so out loud. Whatever this
    // refuses, the menu offers as a WORLD LOAD instead if the place has a `--world`.
    std::string riftHudMsg;      // the arrival / refusal banner
    float       riftHudTimer = 0.0f;

    // The canon tower's Elevator Lobbies, low -> high == F1 -> F7. The floors stack at
    // their authored elevations, so ranking the lobbies by height IS the floor index —
    // no floor number is stored in the data.
    auto floorLobby = [&](int floorIdx1Based, x3::phys::Vec3& out) -> bool {
        if (!canonFloor.valid()) return false;
        std::vector<const x3::game::CanonRoom*> lobbies;
        for (const auto& rm : canonFloor.rooms)
            if (rm.type == "Elevator Lobby") lobbies.push_back(&rm);
        if (lobbies.empty()) {
            // No lobbies in the data at all: F1 still has a home (the main hall).
            const x3::game::CanonBeats bt = x3::game::canonBeats(canonFloor);
            if (floorIdx1Based != 1 || bt.mainHall == x3::game::kNoRoom) return false;
            const x3::game::CanonRoom& rm = canonFloor.rooms[bt.mainHall];
            out = { rm.cx, rm.y0() + 1.0f, rm.cz };
            return true;
        }
        std::sort(lobbies.begin(), lobbies.end(),
                  [](const x3::game::CanonRoom* a, const x3::game::CanonRoom* b) {
                      return a->cy < b->cy;
                  });
        const int i = floorIdx1Based - 1;
        if (i < 0 || i >= (int)lobbies.size()) return false;
        out = { lobbies[(size_t)i]->cx, lobbies[(size_t)i]->y0() + 1.0f, lobbies[(size_t)i]->cz };
        return true;
    };

    // A strata OFFSHOOT pocket by band name ("Crystal Veins", "Magma Zone", ...).
    auto strataPocket = [&](const char* band, x3::phys::Vec3& out) -> bool {
        if (!liveStrataBuilt) return false;
        for (const auto& o : liveStrata.offshoots()) {
            if (std::string(o.bandName ? o.bandName : "").find(band) != std::string::npos) {
                out = { o.pocket.x, o.pocket.y + 1.2f, o.pocket.z };
                return true;
            }
        }
        return false;
    };

    // `why` (optional) receives the refusal reason — the menu prints it in the footer
    // and the HUD prints it when a gate holds.
    auto riftDestination = [&](const std::string& dest, x3::phys::Vec3& out,
                               std::string* why = nullptr) -> bool {
        auto no = [&](const char* reason) { if (why) *why = reason; return false; };
        const x3::game::Destination* d = x3::game::findDestination(dest);
        if (!d) return no("not a place this game knows about");
        const std::string k = d->key;

        // --- The planet. These land ON the streamed terrain (the streamer realizes the
        //     region around the new camera position on the next residency pass).
        auto onTerrain = [&](float x, float z) {
            float p3[3];
            x3::game::placeOnTerrain(x, z, p3);
            out = { p3[0], p3[1] + 1.2f, p3[2] };
            return true;
        };

        // --- The hub itself (sub-level R1).
        if (k == "hub") {
            if (!rifthub.built()) return no("the rift chamber is not built in this world");
            const x3::phys::Vec3 hs = rifthub.spawn();
            out = { hs.x, hs.y + 1.0f, hs.z };
            return true;
        }
        // --- [P0-1 EFLZ-GP-1B] The facility ENTRANCE (the escaped-rescuer arrival
        //     spawn, and a plain destination). The canon data authors an "Entrance"
        //     hallway on the footprint edge (the SEAM-2 breach room) — stand the
        //     player at its centre, inside F1, free and outside every cell.
        if (k == "entrance") {
            if (!canonFloor.valid()) return no("the canonical tower is not built in this world");
            const uint32_t er = canonFloor.roomByName("Entrance");
            if (er == x3::game::kNoRoom) return no("the loaded tower data has no Entrance room");
            const x3::game::CanonRoom& rm = canonFloor.rooms[er];
            out = { rm.cx, rm.y0() + 1.0f, rm.cz };
            return true;
        }
        // --- Back home: the facility, ANY floor. F1 = the detention lobby (the rift is
        //     a two-way door — you are not stranded down there), F7 = the executive floor.
        if (d->group == x3::game::DestGroup::Facility) {
            if (!canonFloor.valid()) return no("the canonical tower is not built in this world");
            const int fl = (k.size() == 2 && k[0] == 'f') ? (k[1] - '0') : 0;
            if (fl < 1) return no("unknown floor");
            if (!floorLobby(fl, out))
                return no("that floor has no landing in the loaded tower data");
            return true;
        }
        // --- The Deep. The club is lazy-built (same as the 1127 descent) so a rift is
        //     a real second way in.
        if (k == "club") {
            if (!club1127.built()) {
                club1127.build(scene, *device, *physics, x3::game::riggedGlbRoot());
                x3::logInfo("[rift] CLUB 1127 built on demand (the rift is a second way in)");
            }
            if (!club1127.built()) return no("Club 1127 failed to build");
            const x3::phys::Vec3 cs = club1127.spawn();
            out = { cs.x, cs.y + 1.0f, cs.z };
            return true;
        }
        // --- THE DESCENT: every offshoot pocket, not just the Crystal Veins.
        if (d->group == x3::game::DestGroup::Underworld) {
            const char* band = (k == "granite")  ? "Granite"
                             : (k == "basalt")   ? "Basalt"
                             : (k == "obsidian") ? "Obsidian"
                             : (k == "crystal")  ? "Crystal"
                             : (k == "magma")    ? "Magma" : nullptr;
            if (!band) return no("unknown strata band");
            if (!liveStrataBuilt)
                return no("the strata are not built yet - ride the elevator below F1 first");
            if (!strataPocket(band, out))
                return no("that band has no offshoot pocket in this build");
            return true;
        }
        // --- The planet.
        if (k == "crash") return onTerrain(140.0f, 205.0f);    // the Crash Site (SEAM 3)
        if (k == "city")  return onTerrain(-200.0f, 425.0f);   // the city region anchor
        if (k == "river") {
            // Stand on the BANK of the real carved river (worldRiverNodes is the same
            // spline the carve + the water ribbon use).
            uint32_t n = 0;
            const x3::game::WorldRiverNode* nodes = x3::game::worldRiverNodes(n);
            if (!nodes || n < 2) return no("this world has no carved river");
            const auto& a2 = nodes[n / 2];
            const auto& b2 = nodes[n / 2 + 1 < n ? n / 2 + 1 : n / 2];
            float tx = b2.x - a2.x, tz = b2.z - a2.z;
            const float tl = std::sqrt(tx * tx + tz * tz);
            if (tl > 1e-3f) { tx /= tl; tz /= tl; }
            const float off = x3::game::kWorldRiverHalfWidth + 14.0f;   // clear of the water
            return onTerrain(a2.x - tz * off, a2.z + tx * off);
        }
        if (k == "ridge") {
            // The highest ground on a ring out from the facility — a real ridge, found
            // by asking the terrain, not by inventing a coordinate.
            float bx = 0.0f, bz = 0.0f, bh = -1e9f;
            for (int i = 0; i < 64; ++i) {
                const float a3 = (float)i * (6.2831853f / 64.0f);
                const float x = std::cos(a3) * 620.0f, z = std::sin(a3) * 620.0f;
                const float h = x3::game::terrainHeightAtWorld(x, z);
                if (h > bh) { bh = h; bx = x; bz = z; }
            }
            if (bh <= -1e8f) return no("this world has no terrain");
            return onTerrain(bx, bz);
        }
        // --- A DEV WORLD. It is a separate build; there is nothing in THIS world to
        //     teleport to. The world menu offers it as a world LOAD; a rift gate holds
        //     and says so. Neither one lies about it.
        return no("a separate world - it must be LOADED, not walked to");
    };

    // Join the async boot-manifest GLB warmup (no-op when none was kicked): the
    // world build below takes model-cache hits instead of repeating parse/decode.
    x3::asset::joinModelPreload();
    x3::boot::mark("GLB preload joined");
    if (canonWorld) {
        // ---- W3-2: DATA-DRIVEN CANONICAL TOWER (all 7 floors merged into one
        // CanonFloor; floor-1 rooms first so every existing lookup is unchanged) +
        // per-room PVS cull. Floors stack at the data's absolute elevations; the
        // elevator lobbies are spine-joined; traversal = the E-use elevator travel. ----
        canonFloor = x3::game::loadCanonTower(x3::game::canonProjectJsonPath());
        if (canonFloor.valid()) {
            x3::game::CanonBuildOpts copts; copts.doors = &canonDoors; copts.lockSecuredRooms = true;
            // W5-1b (fix/spire-hollow-core): the hidden level 4.5 ARRIVAL MOUTH — cut
            // a doorway in the elevator-spine tube's +Z wall at the cavern floor
            // plane; Canon45 builds the arrival tunnel that seals onto it, and the
            // elevator gets a code-locked "4.5" stop at the same Y (owner canon
            // 2026-07-25: the elevator is the hidden floor's ONLY access).
            const float nexusFloorY = x3::game::Canon45::floorPlaneY(canonFloor);
            if (nexusFloorY > -1e8f) {
                copts.spineMouthY0   = nexusFloorY;
                copts.spineMouthY1   = nexusFloorY + x3::game::Canon45::kMouthH;
                copts.spineMouthHalf = x3::game::Canon45::kMouthHalf;
            }
            // FACILITY STAIRWELL (owner feature 2026-07-25): register the per-floor
            // connector mouths as breach cuts so the graybox walls open where the
            // stairwell module (built after the floor) seals its connectors on.
            stairLayout = x3::game::stairwellLayout(canonFloor);
            if (stairLayout.valid) {
                for (const auto& fe : stairLayout.floors) {
                    x3::game::CanonBuildOpts::ExtraBreach eb;
                    eb.room   = fe.room;
                    eb.face   = 0;                              // every target's -X wall
                    eb.center = (fe.floorNum == 1) ? 0.0f
                              : x3::game::StairwellLayout::kDoorZ;
                    eb.half   = x3::game::StairwellLayout::kDoorHalfW;
                    copts.extraBreaches.push_back(eb);
                }
            }
            // TRAPDOOR CARVE — the canon-cell SECRET-ROOM PORT (Tim's code-locked
            // trapdoor was legacy-tower-only until now). Pick a hatch spot inside
            // Jake's Cell clear of the dressing (bunk in the -X/-Z corner, debris in
            // +X/+Z, console on the -Z wall) and have the floor builder leave the
            // hole; the SecretRoom below covers it with flush code-locked panels.
            const x3::game::CanonBeats cbt = x3::game::canonBeats(canonFloor);
            float hatchCx = 0.0f, hatchCz = 0.0f;
            if (cbt.jakeCell != x3::game::kNoRoom) {
                const x3::game::CanonRoom& jc = canonFloor.rooms[cbt.jakeCell];
                hatchCx = jc.cx + 1.4f; hatchCz = jc.cz - 1.1f;
                copts.hatchRoom = cbt.jakeCell;
                copts.hatchCx = hatchCx; copts.hatchCz = hatchCz; copts.hatchHalf = 0.9f;
            }
            // ---- SEAM 2 (world merge): the tower gets ONE real way in/out. Compute
            // the above-ground footprint (deep cave/sub-level rooms excluded), pick
            // the walkable F1 room whose exterior wall sits ON the footprint edge —
            // the data literally authors an "Entrance" hallway on the +Z edge — and
            // have the floor builder cut a doorway-style breach in that wall. The
            // glass facade (built below, wrapped `kExtPad` m outside the footprint,
            // clear of the R-9 skirt's 0.14 m offset) opens at the same center, so
            // the player can WALK from inside F1 out onto the apron. ----
            float extX0 = 1e9f, extX1 = -1e9f, extZ0 = 1e9f, extZ1 = -1e9f, extTop = -1e9f;
            for (const x3::game::CanonRoom& r : canonFloor.rooms) {
                if (r.cy <= -50.0f) continue;              // deep zone: not the tower shell
                extX0 = std::min(extX0, r.x0()); extX1 = std::max(extX1, r.x1());
                extZ0 = std::min(extZ0, r.z0()); extZ1 = std::max(extZ1, r.z1());
                extTop = std::max(extTop, r.y1());
            }
            constexpr float kExtPad = 3.0f;                // facade outside skirt+walls, no z-fight
            const uint32_t entrRoom = canonFloor.roomByName("Entrance");
            int   breachFace = 3; float breachCenter = 0.0f;
            if (entrRoom != x3::game::kNoRoom && extX1 > extX0) {
                const x3::game::CanonRoom& er = canonFloor.rooms[entrRoom];
                // The exterior face = whichever room wall lies nearest the footprint edge.
                const float dist[4] = { er.x0() - extX0, extX1 - er.x1(),
                                        er.z0() - extZ0, extZ1 - er.z1() };
                breachFace = 0;
                for (int f = 1; f < 4; ++f) if (dist[f] < dist[breachFace]) breachFace = f;
                const float halfCut = 1.5f;                // 3 m opening in the room wall
                const bool faceIsX = breachFace < 2;
                const float lo = (faceIsX ? er.z0() : er.x0()) + halfCut + 0.2f;
                const float hi = (faceIsX ? er.z1() : er.x1()) - halfCut - 0.2f;
                breachCenter = std::min(std::max(faceIsX ? er.cz : er.cx, lo), hi);
                copts.breachRoom = entrRoom;
                copts.breachFace = breachFace;
                copts.breachCenter = breachCenter;
                copts.breachHalf = halfCut;
            }
            x3::game::buildCanonFloor(canonFloor, scene, *device, *physics, copts);
            // W2-A2 (punch-list P0 #5): DOOR SOUNDS — wire the audio system + the
            // W2-B servo/denied WAVs into the door system itself, so every open/
            // close/locked transition sounds regardless of which caller flipped it.
            canonDoors.setAudio(audio.get(),
                audio->load(x3::game::resolveAudio("doors/door_open.wav")),
                audio->load(x3::game::resolveAudio("doors/door_close.wav")),
                audio->load(x3::game::resolveAudio("doors/door_locked.wav")));
            // DOOR-MESH SWAP (mega-polish "audio"): a SUSTAINED servo bed that is
            // started on the frame the slab begins to move and stopped on the frame
            // it seats — the elevator's motor-loop pattern (ElevatorSystem::m_motorLoop),
            // applied to doors. This REPLACES the open/close one-shots as the motion
            // voice: door_open.wav is 2.19 s against a ~1 s slide, so as a fire-and-
            // forget voice it audibly ran on after the door had stopped (the chaingun
            // defect class). The short thunk marks the seat. A missing WAV loads
            // invalid and the door falls back to the one-shots — never silent.
            canonDoors.setMotorAudio(
                audio->load(x3::game::resolveAudio("interact/servo_loop.wav")),
                audio->load(x3::game::resolveAudio("interact/door_thunk.wav")));
            // W2-A2 (W2-E residual): PVS-gate the canon door slabs. Probe the two
            // rooms flanking each slab (across the wall normal per Door::axis)
            // against the frame's visible-room set; draw if either is visible.
            // Unknown rooms (both probes in wall meat) draw — fail-safe, never pop.
            canonDoors.setVisQuery(
                [&canonFloor, &canonVisRooms](const x3::game::Door& d) -> bool {
                    if (canonVisRooms.empty()) return true;   // vis not computed yet
                    const float off = 0.7f;
                    const float px = (d.axis == 0) ? off : 0.0f;
                    const float pz = (d.axis == 0) ? 0.0f : off;
                    const uint32_t rA = canonFloor.roomAt(d.closedPos.x + px,
                                                          d.closedPos.y, d.closedPos.z + pz);
                    const uint32_t rB = canonFloor.roomAt(d.closedPos.x - px,
                                                          d.closedPos.y, d.closedPos.z - pz);
                    if (rA == x3::game::kNoRoom && rB == x3::game::kNoRoom) return true;
                    for (uint32_t v : canonVisRooms)
                        if (v == rA || v == rB) return true;
                    return false;
                });
            x3::boot::mark("canon floor geometry+doors");
            // Per-room ceiling lights: the data-driven floor skips the env_art Light_A
            // fixtures the legacy level registers, so without these the rooms only get
            // ambient + the flashlight (the DARK bug). We feed only the player's VISIBLE
            // rooms' lights each frame (below) so the active count stays under the cap.
            // X3_MONSTER_PROF=1: time the dressing passes too (boot-regression hunt).
            const bool bootProf = std::getenv("X3_MONSTER_PROF") != nullptr;
            auto bootProfT0 = std::chrono::steady_clock::now();
            auto bootProfMs = [&bootProfT0](const char* what) {
                const auto t1 = std::chrono::steady_clock::now();
                x3::logInfo("[dressing-prof] " + std::string(what) + "=" +
                    std::to_string(std::chrono::duration<double, std::milli>(t1 - bootProfT0).count()) + "ms");
                bootProfT0 = t1;
            };
            canonLights = x3::game::buildCanonLights(canonFloor);
            // OPENING-SPACE POLISH: dense set-dressing + motivated lighting over the canon
            // detention cell + Main Hall mouth (the first space the player sees). Purely
            // visual props (ModularSciFi + Warehouse kits) + extra PointLights (a flickering
            // cell tube, a red alarm wash, cyan terminal glow). Graybox stays the collision
            // truth; missing GLBs simply aren't drawn (the level never breaks).
            // WAVE (barrels-universal): the canon interactive loop is one of only two hosts
            // with a live fire path (the other is Level1Game), so its BarrelSystem is where
            // "barrels explode game-wide" actually lands. Init it BEFORE the dressing builds
            // so the set-dressers REGISTER their plain fuel-drum clutter (hall + boss/storage
            // rooms) as explodable barrels via this sink, instead of drawing static,
            // unshootable props over them. (Emissive lab vats stay decorative — not routed.)
            canonBarrels.init(*device, *physics);
            auto canonBarrelSink = [&canonBarrels](float x, float floorY, float z) {
                canonBarrels.spawn(x, floorY, z);
            };
            canonDressing.setExplodableBarrelSink(canonBarrelSink);
            canonDressing.build(*device, x3::game::convertedGlbRoot(), canonFloor);
            if (bootProf) bootProfMs("canonDressing");
            // WAVE (cell-door): the owner wants the red tank by the cell door to violently
            // explode when shot (DJBooth's barrel fireball + chain). That still happens — but
            // the drum is at the MAIN HALL MOUTH, just outside the cell door, NOT inside the
            // 4 m cell. MERGE 2026-07-12: the fold spawned it explicitly in the cell's debris
            // corner; playable-build's declutter (3f7e6d0) had already CUT that corner — "the
            // Cell is TINY, and CLUTTERED" (owner, live). Re-spawning it here would have put
            // the barrel straight back into the room we just cleaned. It is not lost: the hall
            // clutter drum registers through canonBarrelSink during canonDressing.build (see
            // cell_dressing.cpp, main-hall section), as do the recipe/boss/storage drums — so
            // "barrels explode game-wide" is fully intact, and the shootable drum is the first
            // thing Jake sees when he steps out of the cell.
            // WAVE-3: recipe-dress every other classifiable room (surface-library
            // panels + zone lights + hero props). Jake's cell stays CellDressing's.
            // Recipe rooms OWN their light statement (bible: one key per room), so the
            // generic warm buildCanonLights entry for those rooms is dropped before the
            // recipe lights are appended — selectVisibleCanonLights budgets the rest.
            {
                const x3::game::CanonBeats rdBt = x3::game::canonBeats(canonFloor);
                canonRooms.setExplodableBarrelSink(canonBarrelSink);  // WAVE (barrels-universal)
                canonRooms.build(*device, x3::game::assetRoot() + "/surface_library",
                                 x3::game::convertedGlbRoot(), canonFloor, rdBt);
                x3::logInfo("--world canonlevel: " + std::to_string(canonBarrels.count()) +
                            " explodable barrel(s) total (cell + hall + recipe fuel drums)");
                if (canonRooms.roomsDressed() > 0) {
                    // 2026-07-12 — THE CELL'S STRAY LIGHT (Tim: the cell is BLOWN-OUT WHITE
                    // with the flashlight on, and PITCH BLACK with it off). Jake's Cell is
                    // deliberately classified ZNone (CellDressing hand-dresses it), so
                    // hasRecipe() is FALSE for it — and the generic buildCanonLights entry
                    // for the cell therefore SURVIVED this erase: one warm tungsten at
                    // (2.0, 1.50, 40.0), intensity 3.2, range 8.0, hung 0.25 m under the
                    // ceiling of a 4 m room. It was contributing ~2.7x what the cell's own
                    // "key" fluorescent did — scorching the ceiling and the upper walls to
                    // white — while the fixtures the art pass carefully tuned did nothing.
                    // CellDressing OWNS this room's light statement exactly as a recipe room
                    // does, so the cell's generic light is dropped here too, and the failing
                    // tube (cell_dressing.cpp) is raised to actually carry the room.
                    const uint32_t cellRoom = rdBt.jakeCell;
                    canonLights.erase(std::remove_if(canonLights.begin(), canonLights.end(),
                        [&](const x3::game::CanonLight& cl) {
                            return canonRooms.hasRecipe(cl.room) || cl.room == cellRoom;
                        }), canonLights.end());
                    canonLights.insert(canonLights.end(),
                                       canonRooms.lights().begin(), canonRooms.lights().end());
                }
            }
            if (bootProf) bootProfMs("canonRooms(recipes)");
            // ---- W5-1: LEVEL 4.5 — the Nexus Chamber. Builds the cavern shell over the
            // open-ceiling Access room, the scaffold climb up the authored tiers, the
            // two-accent horror dressing, and the sparse creatures (apex dormant). Its
            // motivated lights ride the same canonLights feed (room-gated). ----
            canon45.build(canonFloor, scene, *device, *physics,
                          x3::game::riggedGlbRoot(),
                          x3::game::assetRoot() + "/surface_library", canonLights);
            if (bootProf) bootProfMs("canon45");
            // ---- THE FACILITY STAIRWELL (owner feature 2026-07-25): open switchback
            // F1..F7 on the west edge, railed open well, rubber-nosed treads, locked
            // keypad doors on every no-floor landing (incl. level 4.5's height — the
            // hidden floor's tell is a door that won't open, not a blank wall). ----
            stairwell.build(stairLayout, canonFloor, scene, *device, *physics,
                            &canonDoors, x3::game::assetRoot() + "/surface_library",
                            canonLights);
            // feat/stair-nav: derive the enemy waypoint chain from the SAME plan
            // the brushes were laid from (feet ride the real treads); handed to
            // canonPlay after its build below.
            stairNav = x3::game::stairwellNavChain(stairLayout);
            if (bootProf) bootProfMs("stairwell");
            // ---- THE SECRET-CODE QUEST CHAIN, CLUES 1 + 2 (feat/secret-code-clues).
            // Two lore HoloTerminals on the platform, the riftLore 4790 pattern.
            // CLUE 1 — Okafor's work order, Bottom Hall west wall beside the
            // stairwell entrance: teaches 4545 ("re-keyed 45-45"). CLUE 2 — the
            // chief engineer's log on F4 by the elevator lobby: teaches 4455 by
            // riddle ("double the four, double the five"). Placement is derived
            // from the loaded rooms (real walls, reading height, ceiling arm to
            // the room's own lid); both panes are room-stamped for the PVS.
            {
                uint32_t hallRm = x3::game::kNoRoom, corrRm = x3::game::kNoRoom;
                for (uint32_t i = 0; i < (uint32_t)canonFloor.rooms.size(); ++i) {
                    const x3::game::CanonRoom& r = canonFloor.rooms[i];
                    if (r.name == "Bottom Hall") hallRm = i;
                    if (r.name.find("F4: Augmentation Corridor") != std::string::npos)
                        corrRm = i;
                }
                if (hallRm != x3::game::kNoRoom) {
                    const x3::game::CanonRoom& rm = canonFloor.rooms[hallRm];
                    const uint32_t e0 = scene.size();
                    // West wall (x0) is the stairwell-entrance wall; the breach cut
                    // is at z=0, so the glass hangs on the wall span north of it.
                    stairLore.build(scene, *device,
                                    x3::phys::Vec3{ rm.x0() + 0.45f, rm.y0() + 2.05f,
                                                    rm.z1() - 1.1f },
                                    /*yaw*/-1.5708f, /*w*/1.5f, /*h*/0.95f,
                                    /*ceilingY*/rm.y1() - 0.16f);
                    stairLore.setLayout(x3::game::HoloTerminal::Layout::Readout);
                    stairLore.setTextColor(1.0f, 0.72f, 0.30f, 1.0f);   // maintenance amber
                    stairLore.setLines({
                        "FACILITY MAINTENANCE - WORK ORDER 217",
                        "STAIRWELL SERVICE VOIDS",
                        "",
                        "ALL VOID DOORS RE-KEYED 45-45",
                        "AFTER THE INCIDENT.",
                        "STANDING ORDER: VOIDS STAY",
                        "SEALED. ATMO CERT REQUIRED.",
                        "THERE IS NOTHING TO RETRIEVE.",
                        "",
                        "DO NOT COUNT THE LANDINGS.",
                        "- MAINT. CHIEF OKAFOR",
                    });
                    for (uint32_t ei = e0; ei < scene.size(); ++ei)
                        scene.get(ei).roomId = hallRm;
                    x3::logInfo("--world canonlevel: CLUE 1 (service code 45-45, Okafor "
                                "work order) on the glass in 'Bottom Hall'");
                }
                if (corrRm != x3::game::kNoRoom) {
                    const x3::game::CanonRoom& rm = canonFloor.rooms[corrRm];
                    const uint32_t e0 = scene.size();
                    // West wall, lobby end of the corridor (the walk from the cab
                    // passes it) — the machine flank of F4.
                    liftLore.build(scene, *device,
                                   x3::phys::Vec3{ rm.x0() + 0.45f, rm.y0() + 2.05f,
                                                   rm.z0() + 2.5f },
                                   /*yaw*/-1.5708f, /*w*/1.5f, /*h*/0.95f,
                                   /*ceilingY*/rm.y1() - 0.16f);
                    liftLore.setLayout(x3::game::HoloTerminal::Layout::Readout);
                    liftLore.setTextColor(0.42f, 0.66f, 1.60f, 1.0f);   // engineering blue
                    liftLore.setLines({
                        "LIFT MAINTENANCE - ENGINEER'S LOG 88",
                        "CH. ENG. VASQUEZ",
                        "",
                        "RE-ENABLED THE HALF-FLOOR",
                        "STOP FOR THE CHORUS SURVEY.",
                        "IT IS NOT ON THE PANEL.",
                        "IT WILL STAY THAT WAY.",
                        "",
                        "NEW CODE PER PROTOCOL:",
                        "DOUBLE THE FOUR,",
                        "DOUBLE THE FIVE.",
                        "",
                        "GOD HELP WHOEVER RIDES IT.",
                    });
                    for (uint32_t ei = e0; ei < scene.size(); ++ei)
                        scene.get(ei).roomId = corrRm;
                    x3::logInfo("--world canonlevel: CLUE 2 (lift code riddle, Vasquez "
                                "log) on the glass in '" + rm.name + "'");
                }
            }
            // CAPTURE HOOK (headless stills): X3_STAIR_DEMO=1|2 stages a phantom
            // keypad mid-4545-response — 1 = a numbered service void (amber pad +
            // denial line), 2 = the unnumbered door (the sublevel tell). Mirrors
            // the X3_RIFT_OPEN idiom; the capture HUD draws stairDemoBark.
            if (const char* sdEnv = std::getenv("X3_STAIR_DEMO"); sdEnv && stairwell.built()) {
                if (sdEnv[0] == '3') {
                    // 3 = the OWNER'S 7762 path: the master door OPEN + pad green
                    // (capture ticks canonDoors so the slab actually slides).
                    if (stairwell.stageMasterOpen(scene, canonDoors))
                        x3::logInfo("X3_STAIR_DEMO=3: master door staged OPEN (7762 path) for capture");
                } else {
                    using CR = x3::game::FacilityStairwell::CodeResponse;
                    const CR sdr = stairwell.demoSubmit(sdEnv[0] == '2', scene);
                    if (sdr != CR::NotHandled) {
                        stairDemoBark = (sdr == CR::SublevelTell)
                            ? "SUBLEVEL ACCESS VIA PRIMARY LIFT ONLY - SEE CHIEF ENGINEER"
                            : "SERVICE VOID - NO ATMOSPHERE - ENTRY DENIED";
                        x3::logInfo("X3_STAIR_DEMO: staged phantom-door 4545 response for capture");
                    }
                }
            }
            // ---- THE SECRET-ROOM PORT: trapdoor (hazard rim + status light) + the
            // stocked room below + the cell HoloTerminal, seated at the canon cell.
            // The hatch registers in canonDoors (this host updates + draws it); the
            // terminal E-interaction / code entry / loot payoff key off game.secret(),
            // which the existing host paths already poll (they gate on built()).
            // Every entity added here is room-tagged to Jake's Cell so the PVS cull +
            // per-room lights include the feature (the room below is reachable only
            // from the cell anyway).
            if (cbt.jakeCell != x3::game::kNoRoom) {
                const x3::game::CanonRoom& jc = canonFloor.rooms[cbt.jakeCell];
                const uint32_t nBefore = scene.size();
                // cellCenter.xz feeds ONLY the terminal spot (the hatch XZ is explicit
                // above; SecretRoom offsets termPos by (0, +1.3, -2.6) from this):
                // (cx-0.6, cz+0.9) hangs the holo terminal IN THE ROOM just -X of the
                // hatch — support pipe to the ceiling per the HoloPanel design — clear
                // of the -Z wall's window cutouts AND the dressing console at (cx+0.7).
                game.buildCanonCellSecret(scene, *device, *physics, canonDoors,
                                          x3::phys::Vec3{ jc.cx - 0.6f, jc.y0(), jc.cz + 0.9f },
                                          hatchCx, hatchCz, jc.y1() - 0.16f);
                for (uint32_t ei = nBefore; ei < scene.size(); ++ei)
                    scene.get(ei).roomId = cbt.jakeCell;
            }
            // ---- W4-2: --screenshot-vigil — seed a live VIGIL conversation ON
            // the cell glass at BUILD time (the upload batch is open, so the
            // re-bake's createTexture is safe; the settle loop never ticks the
            // terminal, which is why seeding in the capture block failed).
            if (hc.vigilShot && game.secret().terminal().built()) {
                // Display-only seed: a LOCAL tree runner (the session chatTrees is
                // declared later in this function; seed flags are throwaway).
                x3::game::ChatTreeSystem chatTrees;
                chatTrees.loadDefault();
                x3::game::HoloTerminal& vt = game.secret().terminal();
                vt.setActive(true);
                auto seedWrap = [&vt](const std::string& sv) {
                    std::string ln; size_t si = 0;
                    while (si < sv.size()) {
                        size_t sj = sv.find(' ', si); if (sj == std::string::npos) sj = sv.size();
                        const std::string w = sv.substr(si, sj - si);
                        if (!ln.empty() && ln.size() + 1 + w.size() > 40) { vt.addLine(ln); ln.clear(); }
                        if (!w.empty()) { if (!ln.empty()) ln += ' '; ln += w; }
                        si = sj + 1;
                    }
                    if (!ln.empty()) vt.addLine(ln);
                };
                auto seedNode = [&] {
                    for (int g = 0; g < 8 && chatTrees.active(); ++g) {
                        vt.addLine("");
                        seedWrap(chatTrees.currentLine());
                        const auto& ch = chatTrees.choices();
                        if (!ch.empty()) {
                            for (size_t ci = 0; ci < ch.size(); ++ci)
                                seedWrap("  " + std::to_string(ci + 1) + ". " + ch[ci].text);
                            return;
                        }
                        if (!chatTrees.advance()) break;
                    }
                };
                vt.addLine("> VIGIL");
                if (chatTrees.start("vigil", "terminal")) {
                    vt.setTextColor(1.00f, 0.72f, 0.32f);   // VIGIL presence: orange
                    seedNode();
                    const auto& ch = chatTrees.choices();
                    for (size_t ci = 0; ci < ch.size(); ++ci)
                        if (ch[ci].text.find("Status") != std::string::npos) {
                            vt.addLine("> " + ch[ci].text);
                            if (chatTrees.choose((uint32_t)ci)) seedNode();
                            break;
                        }
                    vt.trimBody(14);
                    vt.update(1.0f / 60.0f);   // bake NOW, inside the batch window
                    x3::logInfo("--screenshot-vigil: conversation seeded on the glass");
                } else {
                    x3::logWarn("--screenshot-vigil: vigil terminal tree failed to start");
                }
            }
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
            // Task #4 (boot regression): the INTERACTIVE boot defers the F2-F7 squad
            // spawns (drained 1/frame in the live loop below — the player boots in
            // the F1 cell, floors 2-7 populate invisibly over the first ~1.5 s).
            // Screenshot captures keep the sync path: full content before settle.
            canonPlay.build(canonFloor, scene, *device, *physics,
                            x3::game::riggedGlbRoot(), x3::game::canonGirlsDialogPath(),
                            /*deferUpperFloors=*/!hc.screenshot);
            // feat/stair-nav: enemies may now COMMUTE between floors — hand the
            // stairwell's waypoint chain to the routing pass in canonPlay.tick().
            if (stairNav.valid) canonPlay.setStairNav(&stairNav);
            // feat/stair-nav CAPTURE HOOK: X3_STAIRNAV_DEMO=1 poses ONE squad
            // hostile mid-climb on the F1 flights with a live route toward F3 —
            // the settle frames walk it up the treads for the still.
            if (std::getenv("X3_STAIRNAV_DEMO") && stairNav.valid) {
                std::vector<x3::game::StairNavChain::Wp> wps;
                x3::game::MonsterSystem* pick = nullptr;
                canonPlay.forEachHostileManager([&](x3::game::MonsterManager& mm) {
                    for (uint32_t i = 0; !pick && i < mm.count(); ++i) {
                        x3::game::MonsterSystem& m = mm.at(i);
                        if (m.alive() && !m.flyer() && m.usingRealModel()) pick = &m;
                    }
                });
                if (pick && x3::game::stairNavRoute(stairNav, 1, 3, wps) &&
                    wps.size() > 6) {
                    // wps: [0..2] spur (room->shaft), [3] F1 approach, [4] flight-0
                    // bottom nosing, [5] top nosing. Pose at the flight's midpoint.
                    const auto& a = wps[4]; const auto& b = wps[5];
                    const float yaw = (b.z > a.z) ? 0.0f : 3.1415926f;   // face along the run
                    pick->setPropPose(x3::phys::Vec3{ (a.x + b.x) * 0.5f,
                                                      (a.y + b.y) * 0.5f + 0.4f,
                                                      (a.z + b.z) * 0.5f }, yaw);
                    std::vector<x3::phys::Vec3> route;
                    for (size_t k = 5; k < wps.size(); ++k)
                        route.push_back(x3::phys::Vec3{ wps[k].x, wps[k].y + 0.4f,
                                                        wps[k].z });
                    pick->setStairRoute(route, 3);
                    if (pick->entity() != x3::game::kNoLink &&
                        pick->entity() < scene.size())
                        scene.get(pick->entity()).roomId = x3::game::kNoRoom;
                    x3::logInfo("X3_STAIRNAV_DEMO: staged one enemy mid-climb on the F1 flight");
                }
            }
            // Enemy-SFX: wire the shared enemy cue sink so the canon-level enemies have
            // a VOICE (footsteps, attack swings, take-hit grunts, death, idle taunts) +
            // their impacts land audibly. canonPlay had NO cue sink before — its enemies
            // were silent (the playtest "enemies make NO sounds" bug for --world canonlevel).
            {
                // W9-1: wrap the cue sink so desc-mechanics sees landed enemy hits
                // (infection rolls on F2/F3 creature hits). descMech guards on its
                // own built() — safe even though its build() runs later (it needs
                // chatTrees, the StoryFlags owner, which is declared below).
                auto baseCueSink = makeEnemyCueSink(audio.get(), sndStep, sndGun,
                                                    sndEnemyTaunt, sndEnemyAttack,
                                                    sndEnemyHit, sndEnemyDeath,
                                                    &bootAudio);   // per-species buckets (W4-3)
                canonPlay.setCueSink([baseCueSink, &descMech](const x3::game::GameCue& c) {
                    descMech.onCue(c);
                    baseCueSink(c);
                });
            }
            x3::boot::mark("canon gameplay spawns (GLB enemies)");
            // THE IN-GAME ELEVATOR: the canonical tower rode a TELEPORT+fade until
            // now. Build the REAL souped-up cab in the Elevator Lobby spine (the
            // loader already synthesizes the CrossLevel shaft doorways there —
            // level_loader.cpp:918 "the gameplay traversal is the host's elevator
            // travel"): one stop per lobby, labeled F1..Fn, then the full
            // x3-elevator.js treatment (disco 1127, strata glass, Club 1127). The
            // teleport survives only as the !elevator.built() fallback.
            {
                std::vector<uint32_t> lobbyRooms;
                for (uint32_t i = 0; i < (uint32_t)canonFloor.rooms.size(); ++i)
                    if (canonFloor.rooms[i].type == "Elevator Lobby") lobbyRooms.push_back(i);
                std::sort(lobbyRooms.begin(), lobbyRooms.end(), [&](uint32_t a, uint32_t b) {
                    return canonFloor.rooms[a].cy < canonFloor.rooms[b].cy;
                });
                if (!lobbyRooms.empty()) {
                    const float cabHY = 0.15f;
                    const x3::game::CanonRoom& L0 = canonFloor.rooms[lobbyRooms.front()];
                    std::vector<float> stops; stops.reserve(lobbyRooms.size() + 1);
                    std::vector<std::string> labels; labels.reserve(lobbyRooms.size() + 1);
                    // ---- W-RIFT: THE RIFT STOP IS STOP 0. The cab's stop list is ordered
                    // low -> high, so sub-level R1 (the buried rift chamber, Y=-78) goes in
                    // at the BOTTOM and every lobby floor shifts up one. It is a real floor
                    // on this cab in every way — a stop, a label, a row on the cabin
                    // directory — except that it is LOCKED OUT until the access code 4790 is
                    // entered in the cabin (ElevatorSystem::stopLocked / kRiftAccessCode), so
                    // callTo()/callNext() walk straight past it and the OLED shows a dead row.
                    stops.push_back(kRiftFloorY + cabHY);
                    labels.emplace_back("RIFT");
                    for (size_t li = 0; li < lobbyRooms.size(); ++li) {
                        stops.push_back(canonFloor.rooms[lobbyRooms[li]].y0() + cabHY);
                        labels.emplace_back("F" + std::to_string(li + 1));
                    }
                    // ---- LEVEL 4.5 (fix/spire-hollow-core, owner canon 2026-07-25):
                    // the hidden floor's stop, inserted in Y order between F4 and F5.
                    // Locked exactly like the RIFT row (code 4455 on the cab keypad,
                    // taught by the chief engineer's log on F4 — "double the four,
                    // double the five"; 7762 is the owner's undocumented master
                    // backup) — the directory shows a dead row; callTo()/callNext()
                    // skip it while locked.
                    int nexusStopIdx = -1;
                    {
                        const float nexusY45 = x3::game::Canon45::floorPlaneY(canonFloor);
                        if (nexusY45 > -1e8f) {
                            const float stopY45 = nexusY45 + cabHY;
                            size_t ins = stops.size();
                            for (size_t si = 0; si < stops.size(); ++si)
                                if (stops[si] > stopY45) { ins = si; break; }
                            stops.insert(stops.begin() + ins, stopY45);
                            labels.insert(labels.begin() + ins, "4.5");
                            nexusStopIdx = (int)ins;
                        }
                    }
                    elevator.build(scene, *device, *physics, L0.cx, L0.cz,
                                   1.4f, cabHY, 1.4f, stops, /*startStop*/1);   // boot on F1
                    elevator.setRiftStop(0);
                    if (nexusStopIdx >= 0) elevator.setSecretStop(nexusStopIdx);

                    // ---- SUB-LEVEL R1: the hub + the way in ------------------------------
                    // The hub is the SAME module `--world rifthub` builds — one build path,
                    // authored at a region origin west of the shaft and deep under it, with a
                    // real doorway cut in its -Z wall. The approach corridor seals into that
                    // doorway. Nothing here is a copy of the dev world; the dev world is the
                    // same code with the default origin.
                    {
                        const uint32_t riftEnt0 = scene.size();
                        x3::game::Rifthub::Desc hd;
                        hd.origin      = { L0.cx - 44.5f, kRiftFloorY, L0.cz + 46.0f };
                        hd.doorway     = true;
                        hd.doorCenterX = 7.0f;     // between the S and SE gates — you enter BETWEEN gates
                        hd.doorHalfW   = 1.7f;
                        hd.doorH       = 3.4f;
                        rifthub.build(scene, *device, *physics, riftTriggers, hd);

                        x3::game::RiftDepths::Desc dd;
                        dd.shaft   = { L0.cx, kRiftFloorY, L0.cz };
                        dd.hubDoor = rifthub.doorCenter();
                        dd.doorHalfW = hd.doorHalfW;
                        dd.doorH     = hd.doorH;
                        riftDepths.build(scene, *device, *physics, dd);
                        // PVS: stamp every entity the region authored with kRiftRoom, so
                        // the facility never submits the hub (and the hub never submits the
                        // facility). The host adds the tag to the visible set only while
                        // the eye is down here.
                        for (uint32_t ei = riftEnt0; ei < scene.size(); ++ei)
                            scene.get(ei).roomId = x3::game::kRiftRoom;

                        // The strata bore is REAL rock at this depth: bore the landing +
                        // corridor out of it (StrataWorld::setKeepOut runs before the shaft
                        // builds, in soupUpElevator below).
                        x3::phys::Vec3 kmn, kmx;
                        riftDepths.zoneAabb(kmn, kmx);
                        liveStrata.setKeepOut(kmn, kmx);

                        // The rift REGION = the depths AABB unioned with the hub's shell.
                        riftRegMin = { std::min(kmn.x, hd.origin.x - 21.0f), kRiftFloorY - 4.0f,
                                       std::min(kmn.z, hd.origin.z - 21.0f) };
                        riftRegMax = { std::max(kmx.x, hd.origin.x + 21.0f), kRiftFloorY + 12.0f,
                                       std::max(kmx.z, hd.origin.z + 21.0f) };
                        riftBuilt = rifthub.built() && riftDepths.built();

                        // ---- THE PAYOFF: re-aim the 8 gates at REAL PLACES IN THIS WORLD.
                        // The gates ship pointed at the 8 dev `--world` names. In the ONE
                        // WORLD those names are shortcuts, not destinations — so the hub's
                        // rifts are re-aimed at anchors the host can actually deliver you to
                        // (see riftDestination() in the loop). Order matches the gate ring:
                        // gate 1 = +X, then clockwise.
                        static const char* kCanonDest[8] = {
                            "club 1127",        // the disco at The Deep (Y=-200)
                            "crystal caves",    // the strata's Crystal-Veins offshoot
                            "the crash site",   // the streamed exterior, off the +Z breach face
                            "the city",         // the streamed city region
                            "the river valley", // the carved river / swim water
                            "the cliffs",       // the terrain landmark ridge
                            "facility F1",      // the detention lobby (the way home)
                            "facility F7",      // the executive floor
                        };
                        for (uint32_t ri = 0; ri < rifthub.portalCount() && ri < 8; ++ri)
                            rifthub.setDestination(ri, kCanonDest[ri]);

                        // ---- THE LORE: how the player finds out R1 exists, and how to
                        // reach it. A maintenance log left on the Security room's glass.
                        {
                            const x3::game::CanonBeats lb = x3::game::canonBeats(canonFloor);
                            uint32_t lr = lb.security;
                            if (lr == x3::game::kNoRoom) lr = lb.research;
                            if (lr == x3::game::kNoRoom) lr = lb.mainHall;
                            if (lr != x3::game::kNoRoom) {
                                const x3::game::CanonRoom& rm = canonFloor.rooms[lr];
                                const uint32_t loreEnt0 = scene.size();
                                // The glass hangs at READING height (the HoloTerminal's pos
                                // IS the panel center — at floor Y it lies on the deck).
                                riftLore.build(scene, *device,
                                               x3::phys::Vec3{ rm.cx, rm.y0() + 2.05f, rm.cz + 1.2f },
                                               /*yaw*/0.0f, /*w*/1.5f, /*h*/0.95f,
                                               /*ceilingY*/rm.y1() - 0.16f);
                                riftLore.setLayout(x3::game::HoloTerminal::Layout::Readout);
                                riftLore.setTextColor(1.0f, 0.72f, 0.30f, 1.0f);   // maintenance amber
                                riftLore.setLines({
                                    "FACILITY MAINTENANCE - LOG 41",
                                    "SUB-LEVEL R1 (\"RIFT CHAMBER\")",
                                    "",
                                    "R1 WAS TAKEN OFF THE CAR PANEL AFTER THE",
                                    "THIRD CONTAINMENT EVENT. THE FLOOR IS STILL",
                                    "ON THE HOIST - IT JUST DOES NOT SHOW.",
                                    "",
                                    "CABIN OVERRIDE: 4790",
                                    "RIDE DOWN. TAKE THE HALL WEST, THEN LEFT.",
                                    "DO NOT GO ALONE. - K. VOSS, FACILITIES",
                                });
                                for (uint32_t ei = loreEnt0; ei < scene.size(); ++ei)
                                    scene.get(ei).roomId = lr;
                                x3::logInfo("--world canonlevel: the RIFT maintenance log (code 4790) "
                                            "is on the glass in '" + rm.name + "'");
                            }
                        }

                        // CAPTURE HOOK (headless only, mirrors the dev world's
                        // X3_RIFTHUB_OPEN): X3_RIFT_OPEN=1 puts the RIFT row on the cabin
                        // panel and fires every gate, so a still can show the unlocked
                        // panel + the open membranes without walking the level by hand.
                        if (std::getenv("X3_RIFT_OPEN")) {
                            elevator.unlockRift();
                            for (uint32_t ri = 0; ri < rifthub.portalCount(); ++ri)
                                rifthub.onTrigger(rifthub.portal(ri).triggerId);
                            for (int w = 0; w < 240; ++w) rifthub.tick(1.0f / 60.0f, scene);
                            x3::logInfo("X3_RIFT_OPEN=1: RIFT stop unlocked + all 8 gates OPEN (capture)");
                        }
                    }

                    // QA MAINLEVEL SWEEP — bore the strata AROUND the deep canon rooms.
                    // Cave System (y=-178) + Hidden Sub-Level (y=-174) sit inside the
                    // shaft's 16 m bore/offshoot span, and the Crystal-Veins glow band was
                    // building violet emissive slabs THROUGH their interiors (the "pink
                    // ceiling panels", docs/QA_MAINLEVEL_SWEEP.md D3). Same move as the
                    // rift corridor: any strata piece whose anchor lands inside a room's
                    // volume (+1 m margin) is simply not built.
                    for (const x3::game::CanonRoom& dr : canonFloor.rooms) {
                        if (dr.cy > -50.0f) continue;   // only the deep rooms clash
                        liveStrata.setKeepOut(
                            { dr.x0() - 1.0f, dr.y0() - 1.0f, dr.z0() - 1.0f },
                            { dr.x1() + 1.0f, dr.y1() + 1.0f, dr.z1() + 1.0f });
                    }
                    soupUpElevator(L0.cx, L0.cz, labels);
                    // CAPTURE HOOK (headless): X3_45_OPEN=1 feeds 4455 to the cab
                    // keypad — the 4.5 row unlocks + the cab rides to it, so a
                    // still can show the panel accepting the taught code.
                    if (std::getenv("X3_45_OPEN") && nexusStopIdx >= 0) {
                        elevator.keypadDigit(4); elevator.keypadDigit(4);
                        elevator.keypadDigit(5); elevator.keypadDigit(5);
                        x3::logInfo("X3_45_OPEN=1: code 4455 fed to the cab keypad "
                                    "(4.5 row unlocked, cab riding to it — capture)");
                    }
                    x3::logInfo("--world canonlevel: THE REAL ELEVATOR live in the lobby spine at (" +
                                std::to_string(L0.cx) + ", " + std::to_string(L0.cz) + ") — " +
                                std::to_string(stops.size()) + " stops, RIFT + F1-F" +
                                std::to_string(lobbyRooms.size()) +
                                (nexusStopIdx >= 0 ? " + the locked 4.5 row" : "") +
                                "; E to summon/ride, 1127 for The Deep, 4790 for SUB-LEVEL R1");
                }
            }
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
            // ---- LIVING NPCs (canon facility): detained workers + staff who
            // WORK, PLAY, TALK and LIVE in the canonical rooms — the B1-arena
            // crowd the legacy world had, adapted to canon rooms and room-tagged
            // so the portal PVS culls them exactly like the canonPlay spawns.
            // Violence feed: the same gunfire cue that scatters the legacy crowd
            // (see the fire block). Kinematic scene entities — zero physics. ----
            {
                const auto npc0 = std::chrono::steady_clock::now();
                uint32_t npcCount = 0;
                auto buildRoomCrowd = [&](x3::game::CrowdSystem& cs,
                                          const char* roomName,
                                          x3::game::CrowdConfig cc) {
                    const uint32_t r = canonFloor.roomByName(roomName);
                    if (r == x3::game::kNoRoom) return;
                    const x3::game::CanonRoom& rm = canonFloor.rooms[r];
                    cc.groundY = rm.y0();
                    cc.roomId  = r;
                    cs.build(cc, scene, *device);
                    npcCount += cs.agentCount();
                };
                // Main Hall (44x5 hall at z~44.5): 6 staff — 4 civilians who
                // wander the hall fringe and stop to CHAT, a slow SWEEPER pacing
                // the length, a CONSOLE TENDER at the north-wall panel.
                {
                    x3::game::CrowdConfig cc;
                    cc.count = 6; cc.converse = true;
                    cc.centerX = 22.0f; cc.centerZ = 44.5f;
                    cc.halfX = 19.0f; cc.halfZ = 1.6f; cc.radius = 20.0f;
                    cc.points = { 8.0f, 44.2f,  15.0f, 45.2f,  24.0f, 43.8f,
                                  31.0f, 45.0f, 37.0f, 44.3f };
                    x3::game::CrowdWorkPoint sweep;
                    sweep.kind = x3::game::CrowdWorkPoint::Kind::Sweep;
                    sweep.ax = 6.0f; sweep.az = 43.9f; sweep.bx = 38.0f; sweep.bz = 43.9f;
                    x3::game::CrowdWorkPoint tend;
                    tend.kind = x3::game::CrowdWorkPoint::Kind::Console;
                    tend.ax = 27.0f; tend.az = 46.1f; tend.bx = 27.0f; tend.bz = 47.0f;
                    cc.work = { sweep, tend };
                    buildRoomCrowd(canonCrowds[0], "Main Hall", cc);
                }
                // Bottom Hall (36x4 service hall at z~1): the WORK CREW — a
                // crate carrier hauling supplies down the hall, a sweeper, a
                // console tender at the east end, plus one off-shift wanderer.
                {
                    x3::game::CrowdConfig cc;
                    cc.count = 4; cc.converse = true;
                    cc.centerX = 22.0f; cc.centerZ = 1.0f;
                    cc.halfX = 15.0f; cc.halfZ = 1.2f; cc.radius = 16.0f;
                    cc.points = { 10.0f, 1.0f,  20.0f, 1.6f,  30.0f, 0.6f };
                    x3::game::CrowdWorkPoint carry;
                    carry.kind = x3::game::CrowdWorkPoint::Kind::Carry;
                    carry.ax = 9.0f; carry.az = 0.6f; carry.bx = 27.0f; carry.bz = 0.6f;
                    x3::game::CrowdWorkPoint sweep;
                    sweep.kind = x3::game::CrowdWorkPoint::Kind::Sweep;
                    sweep.ax = 14.0f; sweep.az = 1.8f; sweep.bx = 32.0f; sweep.bz = 1.8f;
                    x3::game::CrowdWorkPoint tend;
                    tend.kind = x3::game::CrowdWorkPoint::Kind::Console;
                    tend.ax = 35.5f; tend.az = 1.0f; tend.bx = 37.0f; tend.bz = 1.0f;
                    cc.work = { carry, sweep, tend };
                    buildRoomCrowd(canonCrowds[1], "Bottom Hall", cc);
                }
                // Entrance fringe: a seated HAND-GAME pair off the golden path
                // (the breach walk passes them living their lives).
                {
                    x3::game::CrowdConfig cc;
                    cc.count = 2;
                    cc.centerX = 5.5f; cc.centerZ = 50.0f;
                    cc.halfX = 3.6f; cc.halfZ = 1.5f; cc.radius = 4.0f;
                    x3::game::CrowdPlaySpot bench;
                    bench.cx = 3.0f; bench.cz = 51.0f; bench.players = 2; bench.ball = false;
                    cc.play = { bench };
                    buildRoomCrowd(canonCrowds[2], "Entrance", cc);
                }
                const double npcMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - npc0).count();
                x3::logInfo("LIVING NPCs: canon facility crowds built — " +
                            std::to_string(npcCount) + " agents in 3 rooms (" +
                            std::to_string(npcMs) + " ms)");
                // SKINNED CITIZENS: plan the skinned layer over each crowd. NO
                // loads here (boot stays flat) — the pools fill DEFERRED, one
                // spawn per frame, from the main-loop tick.
                {
                    const char* siteName[3] = { "Main Hall", "Bottom Hall", "Entrance" };
                    for (int ci = 0; ci < 3; ++ci) {
                        if (!canonCrowds[ci].built()) continue;
                        x3::game::CrowdSkinConfig sc;
                        sc.site = siteName[ci];
                        sc.modelDir = x3::game::riggedGlbRoot();
                        sc.seed = (uint32_t)ci;   // offset the rig cycle per room
                        canonCrowdSkins[ci].build(sc, canonCrowds[ci]);
                    }
                }
                x3::boot::mark("canon crowds (living NPCs)");
            }
            // ---- SEAM 2 (world merge): THE GLASS EXTERIOR WRAPS THE REAL TOWER.
            // The surface world's facility skin (app/facility_exterior.*, factored
            // from host_surface_start) built around the canon footprint at canon
            // coordinates: near-black backing walls + collision, the batched glass
            // curtain wall, white-concrete spandrel bands + parapet, the breach
            // (aligned with the Entrance-room wall cut above) + vestibule + amber
            // sign/spill, a walkable concrete apron ring + soil skirt, and the
            // shared golden-hour sky so outdoors reads under sun/IBL. Everything
            // is tagged kNoRoom (always drawn under the PVS cull, like the R-9
            // skirt); the interior is untouched — the exterior came to IT. ----
            if (entrRoom != x3::game::kNoRoom && extX1 > extX0 && extTop > -1e8f) {
                const x3::game::CanonRoom& er = canonFloor.rooms[entrRoom];
                x3::game::FacilityExterior::Desc fd;
                fd.x0 = extX0 - kExtPad; fd.x1 = extX1 + kExtPad;
                fd.z0 = extZ0 - kExtPad; fd.z1 = extZ1 + kExtPad;
                fd.baseY = er.y0();                    // walk-out level = the Entrance floor
                fd.topY  = extTop;
                fd.breachFace   = (x3::game::FacilityExterior::Face)breachFace;
                fd.breachCenter = breachCenter;
                fd.breachHalfW  = 2.4f;                // the surface facility's breach width
                fd.roofSlab = true;  fd.roofLift = 0.6f;   // clear the F7 ceiling lids
                fd.floorSlab = false;                  // the tower brings its own floors
                fd.vestibuleDepth = kExtPad;           // floored walk across the interstice
                fd.vestibuleHalfW = 1.5f + 0.2f;       // room cut half + wall thickness
                fd.apron = x3::game::FacilityExterior::Apron::Ring;
                fd.apronOut = 24.0f; fd.soilOut = 150.0f;
                fd.mergePanes = true;                  // ~27 storeys: batch the panes
                fd.breachRoomHint = entrRoom;
                // Share RoomDressing's surface library: its GPU textures already
                // hold the recipe concrete sets (no duplicate multi-MB decodes).
                facilityExterior.build(scene, *device, *physics, fd,
                                       &canonRooms.surfaceLibrary());
                x3::game::FacilityExterior::applyGoldenHourSky(*device);
                // --dusk (STREET LIGHT staging): late dusk — the sun sits ON
                // the horizon at a whisper, the zenith goes dark, exposure
                // drops, and the street lamps carry the streets.
                if (hc.duskSky) {
                    x3::rhi::IRenderDevice::SkyParams sp{};
                    sp.enabled = true;
                    sp.sunDir[0] = 0.55f; sp.sunDir[1] = 0.035f; sp.sunDir[2] = -0.35f;
                    sp.sunColor[0] = 1.0f; sp.sunColor[1] = 0.45f; sp.sunColor[2] = 0.22f;
                    sp.sunIntensity = 0.30f; sp.haze = 0.55f; sp.exposure = 0.72f;
                    sp.zenith[0]  = 0.020f; sp.zenith[1]  = 0.032f; sp.zenith[2]  = 0.070f;
                    sp.horizon[0] = 0.26f;  sp.horizon[1] = 0.13f;  sp.horizon[2] = 0.085f;
                    device->setSkyParams(sp);
                    x3::logInfo("--dusk: late-dusk sky override active (street lights carry the scene)");
                }
                // --day (underwater staging): bright midday — a high near-white
                // sun at full strength so the submerged world reads clearly LIT.
                // The mirror of --dusk; captures the underwater-polish look.
                if (hc.daySky) {
                    x3::rhi::IRenderDevice::SkyParams sp{};
                    sp.enabled = true;
                    sp.sunDir[0] = 0.20f; sp.sunDir[1] = 0.94f; sp.sunDir[2] = -0.28f; // high overhead
                    sp.sunColor[0] = 1.0f; sp.sunColor[1] = 0.98f; sp.sunColor[2] = 0.94f;
                    sp.sunIntensity = 1.25f; sp.haze = 0.18f; sp.exposure = 1.0f;
                    sp.zenith[0]  = 0.16f; sp.zenith[1]  = 0.33f; sp.zenith[2]  = 0.62f;
                    sp.horizon[0] = 0.62f; sp.horizon[1] = 0.72f; sp.horizon[2] = 0.82f;
                    device->setSkyParams(sp);
                    x3::logInfo("--day: bright-midday sky override active (underwater staging)");
                }
                // The sky's baked irradiance at full strength shifted the
                // calibrated interior reads (the FP viewmodel washed pink-white
                // vs the pre-merge baseline): scale the IBL ambient so interiors
                // match the baseline (eye-compared) while the facade's shadow
                // side keeps enough sky fill to read its banding. Sun, sky
                // background and the glass pass's reflections are unscaled.
                device->setIblIntensity(0.5f);
                // STREET LIGHT (host-owned): the facility-apron lamps by the
                // breach + the Spire-approach road rows. kNoRoom entities;
                // the city grid's lamps build with the region (hook below).
                {
                    const bool zFace =
                        fd.breachFace == x3::game::FacilityExterior::Face::PlusZ;
                    const float bx = zFace ? fd.breachCenter : (fd.x0 + fd.x1) * 0.5f;
                    const float bz = zFace ? fd.z1 + 2.0f : fd.z1 + 2.0f;
                    streetLights.buildHostLamps(scene, *device, fd.baseY + 0.02f, bx, bz);
                }
                x3::boot::mark("SEAM 2 exterior (facade wraps the tower)");
            } else {
                x3::logWarn("--world canonlevel: SEAM-2 exterior skipped (no Entrance room "
                            "or empty footprint) — tower stays interior-only");
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
            // ---- DEEP-SPACE SKY (LevelDoc biome "space", e.g. --world spacestation).
            // Enable the analytic sky as a near-black void with a single hot star
            // disk (the local sun) + a low starlight fill, then load the FORGE3D
            // celestial bodies and re-lay them FAR-FROM-EARTH: the local Sun is the
            // key, Earth/Sol is a TINY faint speck, the other planets stay small.
            if (docLevel.doc().biome == "space") {
                spaceBiome = true;
                x3::rhi::IRenderDevice::SkyParams sp{};
                sp.enabled = true;
                sp.sunDir[0] = 0.55f; sp.sunDir[1] = 0.22f; sp.sunDir[2] = 0.80f;   // toward the local star
                sp.sunColor[0] = 1.0f; sp.sunColor[1] = 0.97f; sp.sunColor[2] = 0.90f;
                sp.sunIntensity = 0.05f;      // sky DISK only (higher greys the dome)
                sp.sunLight = 2.6f;           // the real key on the hulls
                sp.haze = 0.0f; sp.exposure = 1.0f;
                sp.zenith[0]  = 0.004f; sp.zenith[1]  = 0.004f; sp.zenith[2]  = 0.012f;
                sp.horizon[0] = 0.008f; sp.horizon[1] = 0.010f; sp.horizon[2] = 0.022f;
                device->setSkyParams(sp);
                device->setAmbient(0.030f, 0.034f, 0.050f);   // starlight + planetshine only
                device->setBloom(0.28f);                       // let the emissive cores/gate bloom
                int nTexFail = 0;
                spacePlanets = loadNightSkyPlanets(device, spacePlanetMesh, nTexFail,
                                                   "[spacestation]", &spacePlanetRingMesh);
                for (NightSkyPlanet& b : spacePlanets) {
                    const std::string n = b.name ? b.name : "";
                    if (n == "Sun") {                 // the LOCAL star — the system's sun
                        b.azimuthDeg = 40.0f; b.elevationDeg = 18.0f; b.angularDiameterDeg = 1.4f;
                    } else if (n == "Terrestrial") {  // Sol/Earth: DISTANT faint speck, far away
                        b.azimuthDeg = -128.0f; b.elevationDeg = 34.0f; b.angularDiameterDeg = 0.18f;
                    } else if (n == "Gas") {          // a small system gas giant, off to one side
                        b.azimuthDeg = -150.0f; b.elevationDeg = 20.0f; b.angularDiameterDeg = 1.1f;
                    } else if (n == "Moon" || n == "Ice") {   // small accents
                        b.angularDiameterDeg = std::min(b.angularDiameterDeg, 0.9f);
                    } else if (n == "Lava") {
                        b.angularDiameterDeg = std::min(b.angularDiameterDeg, 0.7f);
                    }
                }
                x3::logInfo("--world spacestation: DEEP-SPACE sky up (" +
                            std::to_string(spacePlanets.size()) + " bodies; Sol/Earth a distant "
                            "faint speck, local star as sun). " +
                            (nTexFail ? std::to_string(nTexFail) + " planet texture(s) missing." : ""));
            }
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
        // TRAPDOOR AUDIO (the elevator Glow-Up treatment on the hatch): wrong-code
        // buzz at the terminal, an access-granted chime on the unlock edge, a heavy
        // looped servo while the panels slide, and a seat thunk at end of travel.
        // The interact WAVs are COMMITTED under assets/audio/interact/ (repo-local
        // resolves first), so this sounds on a fresh clone with no external packs.
        {
            x3::game::SecretRoomSounds srs;
            srs.buzz  = audio->load(x3::game::resolveAudio("interact/buzz.wav"));
            srs.chime = audio->load(x3::game::resolveAudio("interact/chime.wav"));
            srs.servo = audio->load(x3::game::resolveAudio("interact/servo_loop.wav"));
            srs.thunk = audio->load(x3::game::resolveAudio("doors/door_close.wav"));
            game.secret().setSounds(audio.get(), srs);
        }
        // HYBRID ESCALATION (Tim's ruling): the interrupt-rescue heartbeat — wired
        // on BOTH rescue systems (legacy + canon); graceful when a WAV is absent.
        {
            const auto hb = audio->load(x3::game::resolveAudio("interact/heartbeat.wav"));
            game.rescue().setEscalationAudio(audio.get(), hb);
            canonPlay.rescue().setEscalationAudio(audio.get(), hb);
        }

        // Game-feel CUE sink: route enemy footstep / impact cues onto 3D audio.
        // Footsteps reuse the (pitched-down, quiet) step WAV at the enemy's foot;
        // impacts use the gunshot transient. The trigger points live in monster.cpp;
        // here the host maps them onto whatever sounds it has. Intensity -> volume.
        {
            game.setCueSink(makeEnemyCueSink(audio.get(), sndStep, sndGun,
                                             sndEnemyTaunt, sndEnemyAttack,
                                             sndEnemyHit, sndEnemyDeath,
                                             &bootAudio));   // per-species buckets (W4-3)
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
        // The air OUTSIDE the cab is LEVEL 1's air, not the engine's defaults. Without
        // this the elevator's first applyCabAtmosphere (m_cabAir starts -1, so it fires
        // on frame one) restores ambient 0.42 + IBL 1.0 and the tower is back under a
        // blue sky. See ElevatorSystem::setWorldAtmosphere.
        elevator.setWorldAtmosphere(x3::game::kLevel1Ambient[0], x3::game::kLevel1Ambient[1],
                                    x3::game::kLevel1Ambient[2], x3::game::kLevel1Ibl);

        // ---- Souped-up strata/disco elevator (ported from Tim's x3-elevator.js;
        // blueprint §2.2). Shared wiring — see soupUpElevator above the world chain.
        {
            static const char* kFloorLabels[] =
                { "B1", "F1", "F2", "F3", "F4", "F5", "F6", "F7" };
            std::vector<std::string> labels;
            for (uint32_t fi = 0; fi < x3::game::kSpireFloorCount &&
                                  fi < (uint32_t)(sizeof(kFloorLabels)/sizeof(kFloorLabels[0])); ++fi)
                labels.emplace_back(kFloorLabels[fi]);
            soupUpElevator(Lb.elevatorCenter.x, Lb.elevatorCenter.z, labels);
        }

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
    // ---- POLISH: ARM THE WANTED SYSTEM IN THE CANON WORLD. The AlertSystem
    // ran only in the legacy level1 world until now; the canon world (THE game)
    // shipped with the machine dark. Same tunables, same machine — the per-frame
    // feed below routes observations from canonPlay's groups and applies the
    // effects onto canonDoors/canonLights. SCOPE: facility-interior only — the
    // gunshot/corpse/keypad observation call sites gate on the event position
    // resolving to a tower room (canonFloor.roomAt != kNoRoom), so the streamed
    // outdoors (city/river/crowd scatter) never feeds heat. ----
    if (canonWorld && canonFloor.valid()) {
        facilityAlert.configure(x3::game::loadAlertConfig(x3::game::alertJsonPath()));
        facilityAlertOn = true;
        x3::logInfo("--world canonlevel: FACILITY ALERT armed (the wanted system, "
                    "interior-scoped; effects -> canon doors/lights/spawn queue)");
    }
    const x3::game::Level1Layout& L1 = game.layout();
    x3::boot::mark(canonWorld ? "world build (canon floor + gameplay)"
                              : "world build (level1 + spire floors)");

    // ---- B4 / R2 — KILL THE LAST OF THE 0.42 AMBIENT WASH ------------------------
    // R2: the device default ambient is {0.42, 0.44, 0.50} and, until dfcb65d, NOTHING
    // IN THE GAME EVER CALLED setAmbient(). `RoomDressing::applyZoneAtmosphere` took
    // ownership for the CANON room graph — but the LEGACY tower world (--world level1,
    // and with it the SPIRE floors, CLUB 1127, the perf shop and the show room, which
    // are all rooms INSIDE it) still ran the engine default. That is not lighting. It
    // is an omnidirectional flood that lights a room BY DESTROYING ITS CONTRAST: shot
    // flashlight-off, level1 measured meanLum 24.3 with p95 46.5 and ZERO clipped
    // pixels — a flat, shadowless BLUE wash in which every surface reads the same and
    // the level's OWN 3.6-4.2 ceiling fixtures may as well not exist.
    //
    // Bring it DOWN and let the fixtures do the work (THE POLISH RECIPE #2). This is the
    // same move that made the cell, the elevator and the rifthub hall read: rifthub hall
    // 0.100 -> 0.032, elevator cab -> 0.030, cinematic -> 0.032. Match them.
    //
    // Scoped deliberately: the canon world owns its ambient per-zone; the terrain/ocean
    // worlds are OUTDOOR (a sky IS their ambient) and the screenshot hosts set their own.
    // Only the un-owned INTERIOR worlds are corrected here, so nothing else changes.
    if (!canonWorld && !terrainWorld && !docWorld) {
        // ---- LAND-LIGHTING, and READ R4 BEFORE YOU TOUCH THESE NUMBERS. -----------------
        // This block came from `fix/honest-lighting-rooms`, which set a cool TINTED ambient
        // (0.034/0.036/0.042) and never touched the IBL at all. `fix/prim-point-light` then
        // proved (R4) that THE ENGINE HAS TWO AMBIENTS: iblAmbient()'s baked-env path takes
        // diffuse from irradianceCube and NEVER READS its `ambient` argument — and an env
        // cube is baked BY DEFAULT, FOR EVERY SCENE, FROM THE ANALYTIC BLUE SKY. So this
        // setAmbient() was aimed at a DEAD DIAL: these interiors went on being lit by a
        // full-strength blue sky (IBL 1.0) no matter what value was written here. Measured:
        // with setAmbient(0) the probe still read 55/255 of sky.
        //
        // The tint was therefore fitting a curve to a number nobody was reading. Correct is
        // prim-point-light's MODEL, and it takes all three dials together:
        //   1. setIblProbe(true)  — the env cube becomes THE ROOM, not a sky.
        //   2. setIblIntensity()  — the dial that actually governs the baked-env path.
        //   3. setAmbient()       — a NEUTRAL near-black floor (no tint: a tint here was
        //                           only ever compensating for the sky it could not turn off).
        // These are level1's landed values (kLevel1Ambient 0.030/0.030/0.033, kLevel1Ibl 0.5).
        // Level1Game::build() declares the same air for its own world; using the SAME
        // constants here makes the two owners IDEMPOTENT instead of racing — the old code
        // ran AFTER Level1Game and silently overwrote it with the 0.42-era tint.
        // The other un-owned interiors (club, spire, perfshop, showroom — B4's list) are the
        // same class of bug and get the same model.
        device->setIblProbe(true);
        device->setIblIntensity(x3::game::kLevel1Ibl);
        device->setAmbient(x3::game::kLevel1Ambient[0], x3::game::kLevel1Ambient[1],
                           x3::game::kLevel1Ambient[2]);
        // AND TELL THE ELEVATOR. It restores the "world air" whenever the player is not
        // aboard, and it used to hand back the hard-coded engine default — which fired on
        // the FIRST FRAME (m_cabAir starts -1) and clobbered the lines above. Setting the
        // ambient without this is a no-op; that is exactly the bug this pair fixes (L6b).
        elevator.setWorldAtmosphere(x3::game::kLevel1Ambient[0], x3::game::kLevel1Ambient[1],
                                    x3::game::kLevel1Ambient[2], x3::game::kLevel1Ibl);
        x3::logInfo("[light] interior atmosphere (B4/R4): scene IBL probe ON, ibl 0.5, "
                    "ambient 0.030 NEUTRAL — was a blue-sky env cube @ 1.0 + a dead setAmbient");
    }

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
        // FREEWAY TUNNEL CORRIDORS — the boot step, and it has to be HERE.
        // app/terrain.h's registry contract is "register before the first height
        // query", and the spawn probe below is that first query. It cannot go in
        // the city REGION builder: the city is streamed, so that builder runs long
        // after terrain init and its corridors would be ignored by every tile
        // already generated. Gated by ENV, not a cvar, for the same reason: at
        // this point in boot the console does not exist yet. X3_FREEWAY_TUNNELS=0
        // skips registration entirely and restores the pre-corridor field exactly.
        {
            const char* e = std::getenv("X3_FREEWAY_TUNNELS");
            if (!(e && e[0] == '0')) x3::game::registerCityFreewayTunnels();
        }
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

    // Explosive barrels FX: a bright additive fireball at the blast center so a shot
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
        // [W9-3 RPG] the death-FX funnel is the ONE place every hostile death
        // passes through (all managers fan into it) — award kill XP here and
        // toast on a level-up. Captures by reference; progression/rpgUi outlive
        // this lambda (both declared above canonPlay).
        x3::game::DeathFxFn deathFx = [&combatFx, dev, &progression, &rpgUi](const float pos[3], bool flying) {
            if (progression.addXp(x3::game::kXpKill) > 0)
                rpgUi.notifyLevelUp(progression.level());
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
        // W2-A2 (punch-list P0 #1): CanonPlay was NEVER handed this sink — its
        // build() self-wired an empty default, so every --world canonlevel kill had
        // ZERO gibs/blood. CanonPlay::setDeathFxSink fans to hall/cell-guard/
        // attacker groups + Martinez + rescue bosses (post-build safe).
        canonPlay.setDeathFxSink(deathFx);
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
    // Battery cells (canonlevel) grant Lightning-Gun charge (stacks to the 300 cap).
    if (canonWorld && canonPlay.built())
        canonPlay.setChargeSink([&arsenal](int amt) { arsenal.grantCharge((float)amt); });
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
    // W2-A2 (punch-list P1 #8): reload + dry-fire sounds — same dedupe-by-path
    // pattern as fire/impact above. W2-C put reloadSfx/dryfireSfx on every def;
    // missing WAVs stay invalid handles (silent, clean-machine grace).
    std::unordered_map<std::string, x3::audio::SoundHandle> reloadSfxByName, dryfireSfxByName;
    {
        std::unordered_map<std::string, x3::audio::SoundHandle> byPath;
        auto loadOne = [&](const std::string& p) -> x3::audio::SoundHandle {
            if (p.empty()) return {};
            auto it = byPath.find(p);
            if (it != byPath.end()) return it->second;
            x3::audio::SoundHandle h = audio->load(x3::game::resolveAudio(p));
            byPath.emplace(p, h);
            return h;
        };
        for (int wi = 0; wi < arsenal.count(); ++wi) {
            const x3::game::WeaponDef& wd = arsenal.def(wi);
            reloadSfxByName[wd.name]  = loadOne(wd.reloadSfx);
            dryfireSfxByName[wd.name] = loadOne(wd.dryfireSfx);
        }
    }
    auto currentReloadSfx = [&]() -> x3::audio::SoundHandle {
        auto it = reloadSfxByName.find(arsenal.current().name);
        return (it != reloadSfxByName.end()) ? it->second : x3::audio::SoundHandle{};
    };
    auto currentDryfireSfx = [&]() -> x3::audio::SoundHandle {
        auto it = dryfireSfxByName.find(arsenal.current().name);
        return (it != dryfireSfxByName.end()) ? it->second : x3::audio::SoundHandle{};
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

    // ==== WORLD CARS: findable, drivable, hackable vehicles in the one world. ====
    // Host cars (apron + approach road + crash site) park NOW, inside the boot
    // upload batch (the hero GLB upload amortizes into the world-build submit);
    // city cars park/unpark with the `city` region via the streamer hooks below.
    // The ONE live rig (chassis + Jolt VehicleConstraint) spawns lazily on the
    // FIRST entry — zero vehicle constraints exist until the player drives.
    if (canonWorld && canonFloor.valid() && facilityExterior.built()) {
        worldCars.setGroundQuery([](float x, float z) {
            float g[3]; x3::game::placeOnTerrain(x, z, g); return g[1];
        });
        worldCars.setWaterQuery([](float x, float z) {
            return x3::game::worldWaterLevelAt(x, z);
        });
        // A successful hack is a TERMINAL-HACK stimulus (guarded exactly like
        // the keypad path — the alert system may not be armed in canon) + a car
        // alarm that scatters any crowd in earshot (onViolence self-gates).
        worldCars.setHackAlarmHook([&](const x3::phys::Vec3& p) {
            // SCOPE (canon arm): the wanted system is FACILITY-INTERIOR — a car
            // hacked on the apron/city street is outside the security net's
            // ears (the crowds still panic; the tower alert stays quiet).
            if (facilityAlertOn &&
                canonFloor.roomAt(p.x, p.y, p.z) != x3::game::kNoRoom)
                facilityAlert.reportTerminalHack(p);
            if (facilityCrowd.built()) facilityCrowd.onViolence(p);
            for (auto& cc : canonCrowds) if (cc.built()) cc.onViolence(p);
            for (auto& cc : cityCrowds)  if (cc.built()) cc.onViolence(p);
        });
        const x3::game::FacilityExterior::Desc& cxd = facilityExterior.builtDesc();
        std::vector<x3::game::WorldCarDef> carDefs;
        // THE FIRST FINDABLE CAR — on the apron ring 10 m east of the breach
        // walk, nose east along the facade (clear of the golden path +Z line).
        carDefs.push_back({ "apron_east", cxd.breachCenter + 10.0f, cxd.z1 + 10.0f,
                            90.0f, false, { 0.82f, 0.08f, 0.08f }, "" });
        // West apron ring — LOCKED (the hack tutorial within sight of the tower).
        carDefs.push_back({ "apron_west", cxd.x0 - 12.0f, cxd.z1 - 8.0f,
                            0.0f, true, { 0.10f, 0.32f, 0.85f }, "" });
        // The approach road's east shoulder (road x=22 half-w 4 m, z 80..150 —
        // center x 28.5 keeps the body off the asphalt drive line).
        carDefs.push_back({ "approach_road", 28.5f, 96.0f, 0.0f, false,
                            { 0.90f, 0.55f, 0.10f }, "" });
        // Short of the Crash Site (140,205), off the debris field — LOCKED.
        carDefs.push_back({ "crash_site", 126.0f, 192.0f, 135.0f, true,
                            { 0.45f, 0.45f, 0.50f }, "" });
        // City curbs (region-owned; parked z 494/506 = between the z 486/514
        // sidewalk walk-lines and clear of the z 500 street center + crowds).
        carDefs.push_back({ "city_curb_e", 152.0f, 494.0f,  90.0f, false, { 0.93f, 0.90f, 0.86f }, "city" });
        carDefs.push_back({ "city_curb_w", 262.0f, 506.0f, 270.0f, true,  { 0.16f, 0.62f, 0.30f }, "city" });
        // West of the warehouse-dock work crew (their crate runs live x 103-140).
        carDefs.push_back({ "city_dock",    94.0f, 424.0f,   0.0f, false, { 0.78f, 0.72f, 0.18f }, "city" });
        // Scrapyard lot pair — main street east + west (kickabout at -594,481 is
        // ~36 m clear of the nearest body).
        carDefs.push_back({ "scrap_lot_a", -566.0f, 500.0f,  90.0f, true,  { 0.52f, 0.14f, 0.58f }, "city" });
        carDefs.push_back({ "scrap_lot_b", -630.0f, 486.0f, 300.0f, false, { 0.13f, 0.13f, 0.16f }, "city" });
        worldCars.build(carDefs, device, *physics, x3::game::convertedGlbRoot());
        // PERFORMANCE PARTS -> THE CANON CAR. The perf shop composes onto the
        // --world drive DriveDemo only, so until now every installed part, the
        // ECU tune and the entire knock model had ZERO effect on the car driven
        // in the actual game world. Load the persisted build (same vehbuild.json
        // the shop writes), compose it against the catalog and lower it onto the
        // live rig. All three steps degrade quietly: a missing catalog or save
        // simply leaves the car stock, exactly as before.
        {
            x3::game::vehparts::Catalog cat;
            if (cat.loadFile(x3::game::vehparts::defaultCatalogPath())) {
                x3::game::vehparts::VehicleBuild vb;
                const bool haveSave = vb.loadFile(x3::game::vehparts::defaultBuildSavePath());
                const auto composed = x3::game::vehparts::compose(cat, vb);
                if (worldCars.applyTuning(composed.tuning)) {
                    x3::logInfo(std::string("[vehparts] canon car tuned from ") +
                                (haveSave ? "saved build" : "stock baseline"));
                }
            }
        }
        x3::boot::mark("WORLD CARS (host set parked)");
    }

    // World build + every build-time GLB is done — land all batched uploads in one
    // submit. (Per-frame paths from here on use the normal unbatched semantics.)
    device->endUploadBatch();
    x3::boot::mark("upload batch flush");

    // ==== SEAM 3 (world merge): THE PLANET STREAMS IN AROUND THE FACILITY. ====
    // Wire the WorldStreamer's outdoor lanes into the canon master world: the
    // canon region graph (regions.canon.json — spire_f1 dropped, the real tower
    // is already standing) + the terrain ground ring (keep-out under the
    // facility's own apron/soil) + the horizon stitch. Walk out the breach and
    // the city / landmarks / ocean are simply there — no loading screens.
    // Gated on the SEAM-2 exterior: without the breach there is no outdoors.
    // Runs AFTER the boot upload-batch flush (realize() manages its own batch).
    if (canonWorld && canonFloor.valid() && facilityExterior.built()) {
        std::vector<std::string> rerrs;
        if (!canonRegionGraph.load(x3::game::worldRegionsCanonJsonPath(), rerrs) ||
            canonRegionGraph.empty()) {
            for (const std::string& e : rerrs) x3::logError("SEAM 3: " + e);
            x3::logWarn("SEAM 3: canon region graph failed to load — the tower stays an island");
        } else {
            const auto st0 = std::chrono::steady_clock::now();
            if (!terrainJobs) {
                terrainJobs.reset(x3::jobs::createJobSystem());
                terrainJobs->init(0);
            }
            const x3::game::FacilityExterior::Desc& xd = facilityExterior.builtDesc();
            const float towerCx = (xd.x0 + xd.x1) * 0.5f;
            const float towerCz = (xd.z0 + xd.z1) * 0.5f;
            // Ground ring (fixes the SEAM-2 "soil ends ~150 m out" cliff): the
            // canonical world terrain streams around the player. Tiles fully
            // under the facility's own ground (apron + soil skirt, reach =
            // soilOut) are KEPT OUT — the terrain's facility pad is Y=0, exactly
            // coplanar with the F1 floors + apron, and would z-fight them.
            terrainStreamer.setKeepOut(xd.x0 - xd.soilOut, xd.z0 - xd.soilOut,
                                       xd.x1 + xd.soilOut, xd.z1 + xd.soilOut);
            terrainStreamer.init(scene, *device, *physics, terrainJobs.get(),
                                 x3::game::worldTerrainConfig(), towerCx, towerCz,
                                 /*radius=*/8);
            const double terrainMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - st0).count();
            // The region streamer. Shared surface library: RoomDressing's GPU
            // textures already hold the recipe PBR sets, so the city's boot
            // realize skips the duplicate multi-MB PNG decodes (boot budget).
            // Exterior room tag: risk 2 — streamed entities draw only when the
            // canon PVS says the eye can see outdoors (see the vis feed below).
            canonWstream.init(canonRegionGraph, terrainJobs.get());
            canonWstream.setLookahead(hc.wsLookaheadS);
            canonWstream.setRealizedRoomTag(x3::game::kStreamedExteriorRoom);
            canonWstream.setSharedSurfaceLibrary(&canonRooms.surfaceLibrary());
            // CONTENT WIRING: r_citylights 1 asks the `city` builder for the
            // window/sign glow lights (no-op at 0 -- City's emitter is skipped
            // entirely, so the legacy realize is byte-identical).
            canonWstream.setEmitCityGlows(cityLightsDense);
            // ---- LIVING NPCs (city): the street crowds live INSIDE the city
            // region — built in the realize's capture window (the region ledger
            // owns every entity/mesh; the realize re-stamp tags them
            // kStreamedExteriorRoom) and abandon()ed by the teardown hook
            // BEFORE any slot release, so stream-out/in cycles leak nothing and
            // never write into recycled slots. Sites sit on the district flat
            // pads (placeOnTerrain anchors the ground). ----
            canonWstream.setRegionHooks(
                [&cityCrowds, &cityCrowdSkins, &worldCars, &streetLights,
                 &canonWstream, cityLightsDense](
                              const x3::game::WorldRegionDesc& rd, x3::game::Scene& s,
                              x3::rhi::IRenderDevice& dev, x3::phys::IPhysicsWorld& ph) {
                    // WORLD CARS: park this region's curb cars (the system adds
                    // only its OWN static bodies + direct-draw visuals — nothing
                    // lands in the region ledger; teardown below unparks them).
                    worldCars.onRegionBuild(rd.id, ph);
                    if (rd.id != "city") return;
                    auto padY = [](float x, float z) {
                        float g[3]; x3::game::placeOnTerrain(x, z, g);
                        return g[1] + 0.20f;   // stand on the sidewalk/plaza slabs
                    };
                    // [0] New District main street (200,500): sidewalk wanderers
                    // + conversation knots along the shop fronts.
                    {
                        x3::game::CrowdConfig cc;
                        cc.count = 10; cc.converse = true;
                        cc.centerX = 200.0f; cc.centerZ = 500.0f;
                        cc.halfX = 105.0f; cc.halfZ = 16.0f; cc.radius = 110.0f;
                        cc.groundY = padY(200.0f, 500.0f);
                        cc.roomId = x3::game::kStreamedExteriorRoom;
                        cc.points = { 118.0f, 486.0f,  141.0f, 514.0f,  187.0f, 486.0f,
                                      233.0f, 514.0f,  279.0f, 486.0f,  160.0f, 500.0f,
                                      248.0f, 500.0f,  295.0f, 514.0f };
                        cityCrowds[0].build(cc, s, dev);
                    }
                    // [1] the warehouse-dock WORK CREW on the industrial edge
                    // (New District Blvd, Z~420): two crate runs between the
                    // loading docks + a dock console + a sweeper.
                    {
                        x3::game::CrowdConfig cc;
                        cc.count = 5; cc.converse = true;
                        cc.centerX = 120.0f; cc.centerZ = 424.0f;
                        cc.radius = 26.0f;
                        cc.groundY = padY(120.0f, 424.0f);
                        cc.roomId = x3::game::kStreamedExteriorRoom;
                        cc.points = { 112.0f, 430.0f,  128.0f, 431.0f };
                        x3::game::CrowdWorkPoint c1;
                        c1.kind = x3::game::CrowdWorkPoint::Kind::Carry;
                        c1.ax = 103.0f; c1.az = 428.0f; c1.bx = 136.0f; c1.bz = 428.0f;
                        x3::game::CrowdWorkPoint c2;
                        c2.kind = x3::game::CrowdWorkPoint::Kind::Carry;
                        c2.ax = 137.0f; c2.az = 424.5f; c2.bx = 112.0f; c2.bz = 421.5f;
                        x3::game::CrowdWorkPoint tend;
                        tend.kind = x3::game::CrowdWorkPoint::Kind::Console;
                        tend.ax = 140.0f; tend.az = 426.0f; tend.bx = 140.0f; tend.bz = 421.0f;
                        x3::game::CrowdWorkPoint sweep;
                        sweep.kind = x3::game::CrowdWorkPoint::Kind::Sweep;
                        sweep.ax = 104.0f; sweep.az = 431.0f; sweep.bx = 134.0f; sweep.bz = 431.0f;
                        cc.work = { c1, c2, tend, sweep };
                        cityCrowds[1].build(cc, s, dev);
                    }
                    // [2] Scrapyard main street + town-square plaza (-600,~490):
                    // a 4-player KICKABOUT on the plaza open lot + wanderers who
                    // stop to chat along the main street.
                    {
                        x3::game::CrowdConfig cc;
                        cc.count = 9; cc.converse = true;
                        cc.centerX = -600.0f; cc.centerZ = 495.0f;
                        cc.halfX = 55.0f; cc.halfZ = 26.0f; cc.radius = 60.0f;
                        cc.groundY = padY(-600.0f, 490.0f);
                        cc.roomId = x3::game::kStreamedExteriorRoom;
                        cc.points = { -600.0f, 489.0f,  -582.0f, 517.0f,  -648.0f, 518.0f,
                                      -560.0f, 505.0f,  -620.0f, 495.0f };
                        x3::game::CrowdPlaySpot lot;
                        lot.cx = -594.0f; lot.cz = 481.5f; lot.players = 4; lot.ball = true;
                        cc.play = { lot };
                        cityCrowds[2].build(cc, s, dev);
                    }
                    x3::logInfo("LIVING NPCs: city street crowds built inside the "
                                "`city` region realize (24 agents: sidewalk 10, "
                                "dock crew 5, plaza 9 — ledger-owned)");
                    // STREET LIGHT: the city grid's lamps build INSIDE the same
                    // capture window — every post/cone/pool entity + the shared
                    // cone/disc meshes + gradient textures join the region
                    // ledger (dedup'd handles: eviction destroys each once).
                    streetLights.buildCityLamps(s, dev, cityLightsDense);
                    // CONTENT WIRING: adopt the window-spill / neon-sign wash
                    // City just emitted (empty unless r_citylights 1). These
                    // carry no geometry -- they ride the lamp list purely for
                    // its nearest-K selection and its region lifetime.
                    if (cityLightsDense) streetLights.adoptCityGlows(canonWstream.cityGlows());
                    // SKINNED CITIZENS: plan/attach the skinned layer. build()
                    // does NO loads and NO Scene::add, so nothing enters the
                    // region ledger (the parked-cars doctrine); the pools fill
                    // deferred (1/frame) from the main loop, and on a region
                    // RE-realize the already-loaded rigs re-attach for free.
                    {
                        const char* siteName[3] = { "New District sidewalk",
                                                    "Dock crew", "Scrapyard plaza" };
                        for (int ci = 0; ci < 3; ++ci) {
                            if (!cityCrowds[ci].built()) continue;
                            x3::game::CrowdSkinConfig sc;
                            sc.site = siteName[ci];
                            sc.modelDir = x3::game::riggedGlbRoot();
                            sc.seed = (uint32_t)(3 + ci);   // offset vs the facility rooms
                            cityCrowdSkins[ci].build(sc, cityCrowds[ci]);
                        }
                    }
                },
                [&cityCrowds, &cityCrowdSkins, &cityChatter, &scene, &worldCars,
                 &streetLights, &physics](const x3::game::WorldRegionDesc& rd) {
                    // WORLD CARS: unpark this region's curb cars (removes our
                    // static bodies; a car currently DRIVEN is the host-owned
                    // live rig and survives the eviction untouched).
                    worldCars.onRegionTeardown(rd.id, *physics);
                    if (rd.id != "city") return;
                    // SKINNED CITIZENS first (hide the host-owned characters +
                    // detach from the dying agents), THEN abandon the brains.
                    // The chatter forgets its bubbles/pairs with them (stale
                    // agent indices must not survive into a re-realize).
                    for (auto& ck : cityCrowdSkins) ck.deactivate(scene);
                    for (auto& ch : cityChatter) ch.reset();
                    for (auto& cc : cityCrowds) cc.abandon();
                    // STREET LIGHT: drop the city lamp records BEFORE any slot
                    // release (stale SceneHandles must never scribble on
                    // recycled slots; the ledger owns the entities/meshes).
                    streetLights.onCityTeardown();
                    x3::logInfo("LIVING NPCs: city crowds abandoned with the region "
                                "(ledger tears the entities down)");
                });
            // Boot residency at the tower center. The tower sits INSIDE the city
            // footprint (anchor (-200,425) r750 => ~481 m out), so this realizes
            // city + surface_landmarks synchronously ON THE LOADING SCREEN —
            // deliberately: deferring the city would convert this boot cost into
            // a first-frame hitch the moment the live umbrella ticks. ocean_base
            // streams in on approach. Per-region realize times are logged.
            canonWstream.buildStartRegions(scene, *device, *physics,
                                           towerCx, 0.0f, towerCz);
            // Horizon stitch: one static polar ring of the SAME height field out
            // to 13 km (the 4 mountain ranges read from the apron) + the long
            // far plane. rInner tucks under the soil skirt / streamed tiles
            // (recessed by yBias) so the overlap never pokes through.
            {
                x3::game::HorizonRingDesc hr{};
                hr.centerX = towerCx; hr.centerZ = towerCz;
                hr.rInner = 170.0f; hr.rOuter = 13000.0f;
                hr.rings = 140; hr.segments = 160; hr.yBias = -3.0f;
                x3::game::addTerrainHorizonRing(scene, *device,
                                                terrainStreamer.groundTexture(), hr);
                device->setCameraFar(15000.0f);
            }
            canonStreamOn = true;
            canonStreamPrevX = towerCx; canonStreamPrevZ = towerCz;
            const double totalMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - st0).count();
            char sb[240];
            std::snprintf(sb, sizeof(sb),
                "SEAM 3: planet streaming LIVE around the tower — %u regions (%u resident at boot), "
                "terrain ring %.0f ms, total boot cost %.0f ms (budget %.1f ms/frame, lookahead %.1f s)",
                canonWstream.regionCount(), canonWstream.residentCount(),
                terrainMs, totalMs, hc.wsBudgetMs, hc.wsLookaheadS);
            x3::logInfo(sb);
            x3::boot::mark("SEAM 3 streamer boot (planet regions + terrain ring)");

            // ---- FISH (W10 water): THE RIVER LIVES ---------------------------
            // Ambient schools in the river reach nearest the facility (the same
            // worldRiverNodes spline the carve + the water ribbon are built from)
            // plus two in the sea shallows at the estuary. Host-owned (like the
            // parked cars — a loaded system must never enter a region ledger),
            // kStreamedExteriorRoom-tagged so the outdoor PVS gates the draw, and
            // range-gated per school on the player. Kinematic: no physics bodies.
            {
                const auto fs0 = std::chrono::steady_clock::now();
                x3::game::FishConfig fc;
                fc.roomId = x3::game::kStreamedExteriorRoom;
                uint32_t rn = 0;
                const x3::game::WorldRiverNode* rnodes = x3::game::worldRiverNodes(rn);
                if (rnodes && rn >= 4) {
                    uint32_t nearest = 0; float best = 1e30f;
                    for (uint32_t i = 0; i < rn; ++i) {
                        const float dx = rnodes[i].x - towerCx, dz = rnodes[i].z - towerCz;
                        const float d2 = dx * dx + dz * dz;
                        if (d2 < best) { best = d2; nearest = i; }
                    }
                    // THE SPECIES MIX along the reach. Rudd and bream SHOAL (many,
                    // tight); perch run in small loose GANGS; the PIKE is ALONE —
                    // one big slow predator holding station in the reach, and it is
                    // the fish you actually notice. The tints only bite on the
                    // procedural fallback (they multiply its countershading
                    // gradient); a real GLB fish keeps its scanned colour.
                    struct SchoolPlan {
                        int offs;                     // river-node offset from `nearest`
                        x3::game::FishSpecies sp;
                        uint32_t count;
                        float spread;
                        float tint[3];
                    };
                    const SchoolPlan plan[] = {
                        // A tight silver rudd shoal upstream of the tower.
                        { -2, x3::game::FishSpecies::Rudd,  13u, 4.2f, { 0.92f, 0.97f, 1.00f } },
                        // The bream slab, deeper and slower, right off the facility.
                        {  0, x3::game::FishSpecies::Bream, 11u, 5.0f, { 0.88f, 0.94f, 1.00f } },
                        // A PERCH GANG — few, loose, restless.
                        {  3, x3::game::FishSpecies::Perch,  5u, 2.4f, { 1.00f, 0.86f, 0.55f } },
                        // THE PIKE. One. Alone. Downstream, in the quiet water.
                        {  5, x3::game::FishSpecies::Pike,   1u, 2.0f, { 0.86f, 1.00f, 0.78f } },
                        // A second rudd shoal further down the reach.
                        {  7, x3::game::FishSpecies::Rudd,  11u, 4.6f, { 0.92f, 0.97f, 1.00f } },
                    };
                    for (const SchoolPlan& p : plan) {
                        int idx = (int)nearest + p.offs;
                        if (idx < 0) idx = 0;
                        if (idx > (int)rn - 2) idx = (int)rn - 2;
                        const x3::game::WorldRiverNode& A = rnodes[idx];
                        const x3::game::WorldRiverNode& B = rnodes[idx + 1];
                        x3::game::FishSchoolDesc sd;
                        sd.centerX = A.x; sd.centerZ = A.z;
                        sd.heading  = std::atan2(B.z - A.z, B.x - A.x);   // downstream
                        sd.species  = p.sp;
                        sd.count    = p.count;
                        sd.spread   = p.spread;
                        // The species table owns the speed — a pike does not cruise
                        // at rudd pace.
                        sd.speed    = x3::game::fishSpecies(p.sp).speed;
                        for (int c = 0; c < 3; ++c) sd.tint[c] = p.tint[c];
                        fc.schools.push_back(sd);
                    }
                    // THE ESTUARY: two schools in the sea shallows off the mouth.
                    const x3::game::WorldRiverNode& E = rnodes[rn - 1];
                    const x3::game::WorldRiverNode& P = rnodes[rn - 2];
                    const float eh = std::atan2(E.z - P.z, E.x - P.x);
                    for (int k = 0; k < 2; ++k) {
                        const float side = eh + (k == 0 ? 1.5707963f : -1.5707963f);
                        const float probes[4] = { 14.0f, 22.0f, 32.0f, 46.0f };
                        for (float d : probes) {
                            const float px = E.x + std::cos(side) * d + std::cos(eh) * 14.0f;
                            const float pz = E.z + std::sin(side) * d + std::sin(eh) * 14.0f;
                            if (x3::game::worldWaterLevelAt(px, pz) <= x3::game::kFishDryTest)
                                continue;
                            // The estuary shoals: rudd in the shallows, bream out
                            // in the deeper mill.
                            x3::game::FishSchoolDesc sd;
                            sd.centerX = px; sd.centerZ = pz;
                            sd.heading = side + 3.14159265f;
                            sd.species = (k == 0) ? x3::game::FishSpecies::Rudd
                                                  : x3::game::FishSpecies::Bream;
                            sd.count   = 8u + (uint32_t)k * 3u;    // 8, 11
                            sd.spread  = 5.5f;
                            sd.speed   = x3::game::fishSpecies(sd.species).speed;
                            sd.tint[0] = 0.88f; sd.tint[1] = 0.96f; sd.tint[2] = 1.00f;
                            fc.schools.push_back(sd);
                            break;
                        }
                    }
                }
                worldFish.setWaterQuery([](float x, float z) {
                    return x3::game::worldWaterLevelAt(x, z); });
                worldFish.setBedQuery([](float x, float z) {
                    return x3::game::terrainHeightAtWorld(x, z); });
                // The REAL fish art (pose-baked Rodin species). A missing GLB
                // degrades that species to the procedural loft — never a crash,
                // never a statue.
                worldFish.setModelDir(x3::game::riggedGlbRoot());
                worldFish.build(fc, scene, *device);
                const double fishMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - fs0).count();
                x3::logInfo("FISH: " + std::to_string(worldFish.fishCount()) + " fish in " +
                            std::to_string(worldFish.schoolCount()) + " schools (river reach + estuary) — " +
                            std::to_string(fishMs) + " ms");
                // The PERF line the fish budget is judged on: dozens of fish must
                // cost a few tenths of a ms, so we log exactly what they draw.
                x3::logInfo("FISH ART: " + std::to_string(worldFish.glbFishCount()) +
                            "/" + std::to_string(worldFish.fishCount()) +
                            " on REAL pose-baked models — " +
                            std::to_string(worldFish.triCount()) + " tris across " +
                            std::to_string(worldFish.drawCount()) + " draws (pike=" +
                            (worldFish.speciesLoaded(x3::game::FishSpecies::Pike) ? "GLB" : "loft") +
                            " rudd=" +
                            (worldFish.speciesLoaded(x3::game::FishSpecies::Rudd) ? "GLB" : "loft") +
                            " bream=" +
                            (worldFish.speciesLoaded(x3::game::FishSpecies::Bream) ? "GLB" : "loft") +
                            " perch=" +
                            (worldFish.speciesLoaded(x3::game::FishSpecies::Perch) ? "GLB" : "loft") + ")");
                x3::boot::mark("FISH (river + estuary schools)");

                // ---- THE OCEAN LIVES. A handful of BIG animals, far apart, out in
                // the sea + the estuary mouth (never the river proper). The great
                // white haunts the shallows the player actually swims out into; the
                // blue shark works the deeper water; the giant squid waits down in
                // the dark by the undersea base at (1100,-1350).
                {
                    const auto sl0 = std::chrono::steady_clock::now();
                    x3::game::SeaConfig sc;
                    sc.roomId = x3::game::kStreamedExteriorRoom;
                    auto addSea = [&](x3::game::SeaSpecies sp, float x, float z, float roam) {
                        x3::game::SeaCreatureDesc d;
                        d.species = sp; d.homeX = x; d.homeZ = z; d.roam = roam;
                        sc.creatures.push_back(d);
                    };
                    addSea(x3::game::SeaSpecies::GreatWhite, 960.0f,  -1180.0f, 55.0f);
                    addSea(x3::game::SeaSpecies::BlueShark,  1060.0f, -1290.0f, 70.0f);
                    addSea(x3::game::SeaSpecies::GiantSquid, 1140.0f, -1380.0f, 45.0f);
                    worldSea.setWaterQuery([](float x, float z) {
                        return x3::game::worldWaterLevelAt(x, z); });
                    worldSea.setBedQuery([](float x, float z) {
                        return x3::game::terrainHeightAtWorld(x, z); });
                    worldSea.setFishSystem(&worldFish);
                    worldSea.build(sc, scene, *device, *physics);
                    const double seaMs = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - sl0).count();
                    x3::logInfo("SEALIFE: " + std::to_string(worldSea.count())
                                + " big animals - " + std::to_string(seaMs) + " ms");
                    x3::boot::mark("SEALIFE (sharks + the abyss)");
                }

                // ---- CANON ALIENS on the planet (app/canon_aliens.h roster) —
                // placed per the lore-faction map, grounded with the SAME
                // terrainHeightAtWorld query the fish/sealife use, coords logged
                // like the fish schools. patrolRadius here is PLACEMENT
                // behaviour (a calm waypoint loop around the spawn anchor) — the
                // roster's designed combat stats are untouched.
                {
                    const auto ca0 = std::chrono::steady_clock::now();
                    using x3::game::CanonAlien;
                    auto spawnAlien = [&](CanonAlien sp, float x, float z, float patrolR) {
                        x3::game::MonsterSystem::Tuning t = x3::game::canonAlienTuning(sp);
                        if (patrolR > 0.0f) t.patrolRadius = patrolR;
                        const float y = x3::game::terrainHeightAtWorld(x, z);
                        canonAliens.spawn(scene, *device, *physics,
                                          x3::game::riggedGlbRoot(),
                                          x3::phys::Vec3{ x, y, z }, t);
                        x3::logInfo(std::string("canonaliens: ") +
                                    x3::game::canonAlienTypeName(sp) + " at (" +
                                    std::to_string(x) + ", " + std::to_string(z) +
                                    ") groundY=" + std::to_string(y));
                    };
                    // Overlord enforcers: a hostile 3-point patrol ring around
                    // the facility exterior perimeter (past the apron, on soil).
                    spawnAlien(CanonAlien::SaurianSoldier, towerCx - 52.0f, towerCz + 6.0f,  8.0f);
                    spawnAlien(CanonAlien::SaurianSoldier, towerCx + 46.0f, towerCz + 52.0f, 8.0f);
                    spawnAlien(CanonAlien::SaurianSoldier, towerCx + 40.0f, towerCz - 58.0f, 8.0f);
                    // Grey worker drones: industrious on the crash-site approach
                    // (the strata-bound salvage path) — fragile ranged kiters.
                    spawnAlien(CanonAlien::GreyTasked, 96.0f, 148.0f, 5.0f);
                    spawnAlien(CanonAlien::GreyTasked, 118.0f, 176.0f, 5.0f);
                    // The Nordic Steward watches from the RIDGE — the same
                    // highest-ground-on-the-620m-ring sampling the "ridge"
                    // destination uses (ask the terrain, don't invent a coord).
                    // The Mantis Arbiter stalks the PASS — the lowest DRY point
                    // on that same ring (a pass is the low ground between highs).
                    {
                        float rx = 620.0f, rz = 0.0f, rh = -1e9f;   // ridge (max)
                        float px = 620.0f, pz = 0.0f, ph = 1e9f;    // pass  (min, dry)
                        for (int i = 0; i < 64; ++i) {
                            const float a = (float)i * (6.2831853f / 64.0f);
                            const float x = std::cos(a) * 620.0f, z = std::sin(a) * 620.0f;
                            const float h = x3::game::terrainHeightAtWorld(x, z);
                            const float w = x3::game::worldWaterLevelAt(x, z);
                            if (h > rh) { rh = h; rx = x; rz = z; }
                            if (h < ph && h > w + 0.5f) { ph = h; px = x; pz = z; }
                        }
                        spawnAlien(CanonAlien::NordicSteward, rx, rz, 0.0f);
                        spawnAlien(CanonAlien::MantisArbiter, px, pz, 12.0f);
                    }
                    // The Saurian WARLORD boss is DEV-GATED (X3_SPAWN_WARLORD=1)
                    // so it is testable without ambushing players at the door.
                    if (const char* wl = std::getenv("X3_SPAWN_WARLORD"); wl && wl[0] == '1')
                        spawnAlien(CanonAlien::SaurianWarlord, towerCx + 34.0f, towerCz + 40.0f, 0.0f);
                    const double caMs = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - ca0).count();
                    x3::logInfo("canonaliens: " + std::to_string(canonAliens.count())
                                + " planet aliens - " + std::to_string(caMs) + " ms");
                    x3::boot::mark("CANON ALIENS (the four species walk the planet)");
                }

                // ---- GOD RAYS (underwater beauty): sun shafts hanging from the
                // water surface over the fish reach + the estuary shallows —
                // additive glass (the street-light cone mode; rides the BLEND
                // tail, never writes depth), leaning down-sun, breathing slowly.
                // Brightness keys off the host sky actually in effect (--day /
                // --dusk / the golden-hour default): the rays die with the sun.
                {
                    x3::game::GodRays::Config gr;
                    gr.roomId = x3::game::kStreamedExteriorRoom;
                    gr.nearX = towerCx; gr.nearZ = towerCz;
                    float sunX = 0.55f, sunY = 0.16f, sunZ = -0.35f, sunI = 1.25f; // golden hour
                    if (hc.duskSky) { sunX = 0.55f; sunY = 0.035f; sunZ = -0.35f; sunI = 0.30f; }
                    if (hc.daySky)  { sunX = 0.20f; sunY = 0.94f;  sunZ = -0.28f; sunI = 1.25f; }
                    gr.sunDirX = sunX; gr.sunDirY = sunY; gr.sunDirZ = sunZ;
                    gr.sunScale = std::min(sunY / 0.5f, 1.0f) * sunI;
                    worldRays.build(gr, scene, *device);
                    x3::boot::mark("GOD RAYS (sun shafts under the surface)");
                }

                // ---- THE WATER FLASH (the zap's biggest read): a radial-gradient
                // disc lying ON the water that lights the whole pool cyan-white at
                // the discharge, then decays to an afterglow. Same material as the
                // street lamps' ground pools (additive glass) — it rides the BLEND
                // tail, so it never writes depth and never punches a hole in the
                // river. Its radius IS kWaterZapRadius: the flash shows you exactly
                // what just got cooked.
                {
                    const int kSegs = 40;
                    std::vector<x3::rhi::MeshVertex> dv;
                    std::vector<uint32_t> di;
                    x3::rhi::MeshVertex c{};
                    c.normal[1] = 1.0f; c.uv[0] = 0.5f; c.uv[1] = 0.5f;
                    dv.push_back(c);
                    for (int si = 0; si <= kSegs; ++si) {
                        const float a = (float)si / (float)kSegs * 6.2831853f;
                        x3::rhi::MeshVertex v{};
                        v.pos[0] = std::cos(a); v.pos[1] = 0.0f; v.pos[2] = std::sin(a);
                        v.normal[1] = 1.0f;
                        v.uv[0] = 0.5f + 0.5f * std::cos(a);
                        v.uv[1] = 0.5f + 0.5f * std::sin(a);
                        dv.push_back(v);
                    }
                    for (int si = 0; si < kSegs; ++si)
                        di.insert(di.end(), { 0u, (uint32_t)(si + 2), (uint32_t)(si + 1) });
                    const x3::rhi::MeshHandle discMesh =
                        device->createMesh(dv.data(), (uint32_t)dv.size(),
                                           di.data(), (uint32_t)di.size());
                    const int N = 64;
                    std::vector<uint8_t> gp((size_t)N * N * 4);
                    for (int y = 0; y < N; ++y)
                        for (int x = 0; x < N; ++x) {
                            const float dx = ((float)x + 0.5f) / N - 0.5f;
                            const float dy = ((float)y + 0.5f) / N - 0.5f;
                            const float rr = std::sqrt(dx * dx + dy * dy) * 2.0f;
                            const float f = 0.55f * std::pow(std::max(0.0f, 1.0f - rr), 3.0f)
                                          + 0.45f * std::pow(std::max(0.0f, 1.0f - rr), 9.0f);
                            const float fc = std::min(1.0f, f);
                            uint8_t* p = &gp[((size_t)y * N + x) * 4];
                            p[0] = p[1] = p[2] = (uint8_t)std::lround(255.0f * fc);
                            p[3] = 255;
                        }
                    const x3::rhi::TextureHandle discTex =
                        device->createTexture(gp.data(), N, N, false);
                    x3::game::Entity fe;
                    fe.mesh = discMesh;
                    fe.tex  = discTex;
                    fe.baseColor[3] = 1.0f;
                    fe.emissive[0] = 0.55f; fe.emissive[1] = 0.90f; fe.emissive[2] = 1.00f;
                    fe.emissive[3] = 0.0f;          // lit only during a discharge
                    fe.transparent = true;
                    fe.glass.opacity = 0.0f; fe.glass.refraction = 0.0f;
                    fe.glass.roughness = 0.0f; fe.glass.specular = 0.0f;
                    fe.glass.additive = 0.05f;      // BLEND tail: no depth write
                    fe.roomId  = x3::game::kStreamedExteriorRoom;
                    fe.visible = false;
                    zapFlashEnt = scene.add(fe);
                }
            }
        }
    }
    // SEAM 3 teardown — idempotent; called on EVERY exit path that follows the
    // boot above (screenshot/bench/framepacing/smoketest early returns + the
    // main-loop exit) so region bodies/meshes are released before physics dies.
    auto shutdownCanonStream = [&]() {
        if (!canonStreamOn) return;
        canonStreamOn = false;
        canonWstream.shutdown(scene, *device, *physics);
        terrainStreamer.shutdown(scene, *device, *physics);
        if (!terrainWorld && terrainJobs) terrainJobs->shutdown();
    };

    // =======================================================================
    // THE WATER ZAP — Tim: "Lightning gun will electrify the water.. one Zap,
    // and the player takes half health damage, and all the fish around die."
    //
    // TRIGGER: a LIGHTNING shot whose ray MEETS WATER (findWaterEntry marches it
    // against worldWaterLevelAt), or one fired by a shooter who is IN the water.
    // LATCH: WaterZapper — one zap per trigger pull, kWaterZapCooldown (1.75 s)
    // between zaps, so a held beam does NOT re-zap the river every frame.
    // DAMAGE: player in the water inside kWaterZapRadius (12 m, measured on the
    // water plane) loses HALF OF MAX HEALTH (50 of 100), once; every LIVE fish
    // in the radius DIES (belly-up, floats, drifts, despawns); anything WADING
    // in it takes kWaterZapEnemyDamage as DamageType::Energy; crowds scatter.
    // FX: the surface spiders re-randomized lightning arcs out to the radius for
    // zapFxTimer seconds (the CombatFx bolt path — the same jagged bolts the gun
    // fires), plus a flash + sparks at the entry point and the ZAP take.
    // =======================================================================
    const x3::audio::SoundHandle sndWaterZap =
        audio->load(x3::game::resolveAudio("weapons/loops/Vefects_Zap_Medium_01.wav"));
    const x3::audio::SoundHandle sndZapCrackle =
        audio->load(x3::game::resolveAudio("weapons/impact/Laser_Impact_Light_6.wav"));
    const x3::game::WaterZapQueryFn waterQueryFn =
        [](float x, float z) { return x3::game::worldWaterLevelAt(x, z); };
    // The live discharge: re-rolled radial arcs ACROSS the water plane. Called
    // every frame while zapFxTimer > 0 (live loop AND the screenshot settle).
    auto emitZapArcs = [&](int strands) {
        for (int i = 0; i < strands; ++i) {
            zapFxRng = zapFxRng * 1664525u + 1013904223u;
            const float a = (float)(zapFxRng % 6283u) * 0.001f;      // any direction
            zapFxRng = zapFxRng * 1664525u + 1013904223u;
            const float r = x3::game::kWaterZapRadius *
                            (0.50f + 0.50f * (float)(zapFxRng % 1000u) * 0.001f);
            zapFxRng = zapFxRng * 1664525u + 1013904223u;
            // Every third arc LEAPS clear of the water (an arc that jumps off the
            // surface is what sells "the pool is live"); the rest crawl ACROSS it,
            // out to the damage radius — so the radius itself is legible.
            const bool leap = (zapFxRng % 3u) == 0u;
            const float endY = zapFxCenter.y + (leap ? 1.30f : 0.22f);
            zapFxRng = zapFxRng * 1664525u + 1013904223u;
            const float jx = ((float)(zapFxRng % 400u) * 0.001f - 0.2f);
            zapFxRng = zapFxRng * 1664525u + 1013904223u;
            const float jz = ((float)(zapFxRng % 400u) * 0.001f - 0.2f);
            const x3::phys::Vec3 from{ zapFxCenter.x + jx, zapFxCenter.y + 0.30f,
                                       zapFxCenter.z + jz };
            const x3::phys::Vec3 to{ zapFxCenter.x + std::cos(a) * r, endY,
                                     zapFxCenter.z + std::sin(a) * r };
            combatFx.addTracer(from, to, x3::game::WeaponFxKind::Lightning);
        }
    };
    // THE WATER FLASH: drive the disc's emissive from the discharge clock — a hot
    // over-1.0 pulse (bloom turns it into a LIT POOL), decaying to an afterglow.
    auto writeZapFlash = [&](float amt) {
        if (zapFlashEnt == x3::game::kNoLink) return;
        x3::game::Entity& fe = scene.get(zapFlashEnt);
        if (amt <= 0.0f) { fe.visible = false; fe.emissive[3] = 0.0f; return; }
        const float rr = x3::game::kWaterZapRadius * 0.92f;
        float* t = fe.transform;
        t[0] = rr; t[1] = 0; t[2]  = 0;  t[3]  = 0;
        t[4] = 0;  t[5] = 1; t[6]  = 0;  t[7]  = 0;
        t[8] = 0;  t[9] = 0; t[10] = rr; t[11] = 0;
        t[12] = zapFxCenter.x; t[13] = zapFxCenter.y + 0.30f; t[14] = zapFxCenter.z;
        t[15] = 1;
        fe.emissive[3] = 1.7f * amt;    // a LIT POOL the arcs still read against
        fe.visible = true;
    };
    auto tickZapFx = [&](float dt) {
        if (zapFxTimer <= 0.0f) { writeZapFlash(0.0f); return; }
        emitZapArcs(9);                    // keeps ~56 arcs alive at mixed ages
        zapFxTimer -= dt;
        if (zapFxTimer <= 0.0f) zapFxTimer = 0.0f;
        const float k = zapFxTimer / 0.70f;                  // 1 -> 0 over the window
        const float amt = (k > 0.83f) ? 1.0f : (k / 0.83f) * (k / 0.83f);
        writeZapFlash(amt);                                  // spike, then afterglow
    };
    // Fire ONE zap at `we`. `pl`/`plFeet` (optional) is the player who might be
    // standing in it. The caller owns the LATCH check (waterZapper.canZap()).
    auto fireWaterZap = [&](const x3::game::WaterZapEntry& we,
                            x3::game::Player* pl, const x3::phys::Vec3* plFeet) {
        const x3::phys::Vec3 c{ we.x, we.surfaceY, we.z };
        const uint32_t killed = worldFish.killWithin(we.x, we.z, x3::game::kWaterZapRadius);
        // THE BIG ANIMALS DIE TOO - a shark caught in a live pool is a dead shark.
        // (Anything deeper than kSeaZapDepth is out of reach: the abyss is safe.)
        const uint32_t seaKilled = worldSea.built()
            ? worldSea.killWithin(we.x, we.z, x3::game::kWaterZapRadius) : 0u;
        (void)seaKilled;
        int selfDmg = 0;
        if (pl && plFeet)
            selfDmg = x3::game::zapPlayer(*pl, *plFeet, we.x, we.z, waterQueryFn);
        uint32_t fried = 0;
        fried += x3::game::zapMonsters(game.corridorEnemies(), scene, *physics,
                                       we.x, we.z, waterQueryFn);
        fried += x3::game::zapMonsters(game.checkpointEnemies(), scene, *physics,
                                       we.x, we.z, waterQueryFn);
        if (canonPlay.built())
            canonPlay.forEachHostileManager([&](x3::game::MonsterManager& mm) {
                fried += x3::game::zapMonsters(mm, scene, *physics,
                                               we.x, we.z, waterQueryFn);
            });
        if (facilityCrowd.built()) facilityCrowd.onViolence(c);
        for (auto& cc : canonCrowds) if (cc.built()) cc.onViolence(c);
        for (auto& cc : cityCrowds)  if (cc.built()) cc.onViolence(c);
        zapFxCenter = c;
        zapFxTimer  = 0.70f;
        emitZapArcs(30);                   // frame 1 goes WIDE — the pool is live
        writeZapFlash(1.0f);               // and the WATER ITSELF flashes
        combatFx.spawnMuzzleFlash(x3::phys::Vec3{ c.x, c.y + 0.18f, c.z },
                                  x3::phys::Vec3{ 0.0f, 1.0f, 0.0f },
                                  x3::game::WeaponFxKind::Lightning);
        combatFx.spawnImpact(x3::phys::Vec3{ c.x, c.y + 0.05f, c.z },
                             x3::phys::Vec3{ 0.0f, 1.0f, 0.0f },
                             x3::game::WeaponFxKind::Lightning);
        if (audio) {
            if (sndWaterZap.valid())
                audio->playSound3D(sndWaterZap, c.x, c.y, c.z, 1.0f, 0.85f);
            if (sndZapCrackle.valid())
                audio->playSound3D(sndZapCrackle, c.x, c.y, c.z, 0.9f, 1.25f);
        }
        waterZapper.noteZap();
        char zb[220];
        std::snprintf(zb, sizeof(zb),
            "waterzap: THE WATER GOES LIVE at (%.1f, %.1f, %.1f) r=%.1f m — "
            "fish killed %u, enemies fried %u, self damage %d",
            c.x, c.y, c.z, x3::game::kWaterZapRadius, killed, fried, selfDmg);
        x3::logInfo(zb);
    };

    // ---- S7: console backend (D6) + screen-space HUD (FPS, console, crosshair).
    std::unique_ptr<x3::con::IConsole> console(x3::con::createConsole());
    x3::game::Hud hud;
    bool quitRequested = false;
    hud.init(*console, &quitRequested);

    // FIX 1: live-tunable viewmodel aim. Register vm_yaw/vm_pitch/vm_roll (deg)
    // and vm_fwd/vm_right/vm_down (m); read them each frame and feed the pose to
    // drawViewmodel so typing e.g. `vm_pitch 10` moves the held gun immediately.
    registerViewmodelCVars(*console);

    // CLUB LISTEN MODE: register snd_listen / snd_listen_offset_ms / snd_listen_gain
    // and bind the console so the club's beat grid can read them each frame. Set
    // `snd_listen 1`, play any music on the PC, and the club light show rides the
    // live-detected beat (WASAPI loopback). Default off -> club uses kClubBpm.
    x3::club_listen::registerCVars(*console);
    x3::club_listen::bindConsole(console.get());

    // RT DEFAULT ON for ray-tracing-capable devices (owner: "Ray Tracing default
    // should be ON on the 3090 Ti"). Gated on rayTracingSupported() so the fleet's
    // non-RT boxes (1080 Ti / 980 Ti) keep the raster/SSAO fallback byte-identical.
    // RT AO = ground-truth contact occlusion; DDGI = real bounce GI — together the
    // proper fix for "dark rooms / black props / can't see without the flashlight".
    // r_rtshadows already defaults 2 (auto-0 without RT); r_ssr already 1.
    if (device && device->rayTracingSupported()) {
        console->set("r_rtao", "1");
        console->set("r_ddgi", "1");
        x3::logInfo("[rt] ray-tracing device detected -> RT AO + DDGI GI default ON");
    } else {
        x3::logInfo("[rt] no ray-tracing device -> raster/SSAO fallback (RT default off)");
    }

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
    // VIGIL ambient barks: chattiness (0 off / 1 occasional / 2 chatty) + base
    // cooldown seconds between barks. Gated on the vigilLink flag regardless.
    console->registerCVar("vigil_chatter",  "1",  "VIGIL ambient barks: 0 off, 1 occasional, 2 chatty");
    console->registerCVar("vigil_cooldown", "9",  "VIGIL bark minimum seconds between one-liners");
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

    // ---- CONTENT WIRING: map the light cvars onto this frame's budgets ------
    // Called once here (so the headless capture paths, which never enter a
    // frame loop, still get the requested state) and again after every
    // applyRtaoCVars() in the loops (so a console edit is live).
    //
    // r_clusterlights 0  -> every budget holds EXACTLY the constant it was
    //                       before this lane: 64 / 44 / 36 / 14 / 16 / 16.
    //                       The legacy render is therefore bit-for-bit intact.
    // r_clusterlights 1  -> the final cap becomes r_maxlights (clamped to the
    //                       1024 device cap), and each feeder K is scaled by
    //                       the same factor so the assembled frame can actually
    //                       REACH the new cap instead of starving at 64. The
    //                       street lamps get the biggest lift because they are
    //                       the feeder the city was starved on (K=14 against
    //                       ~56 built lamps, appended LAST).
    auto refreshLightBudgets = [&]() {
        if (console->getInt("r_clusterlights") == 0) {
            lightBudget = 64; fixtureBudget = 44; canonLightBudget = 36;
            streetLampK = 14;  docLightK = 16;    strataLightK = 16;
            return;
        }
        int want = console->getInt("r_maxlights");
        if (want <= 0) want = (int)x3::rhi::kMaxSceneLights;
        const size_t cap = (size_t)std::min<int>(want, (int)x3::rhi::kMaxSceneLights);
        lightBudget = cap;
        // Scale the feeders against the 64-light baseline they were tuned for.
        const double k = (double)cap / 64.0;
        auto scale = [&](double base, double lo) {
            return (uint32_t)std::max(lo, base * k);
        };
        fixtureBudget    = scale(44.0, 44.0);
        canonLightBudget = scale(36.0, 36.0);
        docLightK        = scale(16.0, 16.0);
        strataLightK     = scale(16.0, 16.0);
        // Street lamps: the city feeder. Give it the largest share — a night
        // city is the one scene where hundreds of lamps are the subject.
        streetLampK      = scale(14.0, 14.0) * 2u;
        if (streetLampK > cap) streetLampK = (uint32_t)cap;
    };
    refreshLightBudgets();

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

    // ---- W9-1: DESC-FIELD MECHANICS (Tier A). Registered onto the loaded tower
    // here — after chatTrees (the one StoryFlags world) exists: the coolant
    // sabotage / EMP bench / master hack / antidote bench interact points plus
    // the cold-room chill + infection status verbs. build() re-applies the
    // Collective multiplier if a loaded flags file already carries the sabotage.
    if (canonWorld && canonFloor.valid() && canonPlay.built()) {
        descMech.build(canonFloor, canonPlay, chatTrees.flags());
        x3::boot::mark("desc-mechanics (Tier A verbs)");
        // Shot/test hook: X3_DESCMECH_SABOTAGE=1 boots with the coolant console
        // already sabotaged (flag + Collective x1.5 + dead glow) so the state is
        // capturable on the screenshot path, which never runs the live E-chain.
        if (const char* sab = std::getenv("X3_DESCMECH_SABOTAGE"); sab && sab[0] == '1') {
            chatTrees.flags().set("f4.coolant_sabotaged");
            canonPlay.applyCoolantSabotage();
            x3::game::killRoomGlow(canonLights, descMech.coolantRoom());
            coolantGlowDead = true;
            x3::logInfo("[descmech] X3_DESCMECH_SABOTAGE=1 — booted in the sabotaged state");
        }
    }

    // ---- [P0-1 EFLZ-GP-1B] ESCAPED SURFACE->FACILITY HANDOFF — ARRIVAL SIDE ----
    // (specs/EFLZ_SURFACE_FACILITY_HANDOFF.spec.md §3.3.4-6.) main()'s world-load
    // loop re-dispatched canonlevel with spawnAtKey="entrance" because the escaped
    // rescuer pressed [E] at the surface breach (host_surface_start). The arrival
    // contract, on the freshly-built canon world:
    //   * FLAGS (H4): the live flags world starts empty, so import the intro's
    //     persisted intro.outcome=escaped (+ intro.landed) into chatTrees.flags().
    //   * ARMED (H3): the rescuer enters with the sidearm LIVE — cheatArm flips
    //     the same WeaponSystem the cell pickup does, so fire/arsenal/HUD all
    //     work (never a cosmetic prop after the handoff, spec §3.4).
    //   * OBJECTIVE (§3.3.6): the surface "RESCUE SARAH" line becomes the
    //     interior hunt via the free-text objective lane missions already use.
    // The entrance PLACEMENT itself rides the generic load-and-place path (the
    // pendingSpawnKey block in the main loop -> riftDestination("entrance")).
    // Gated on BOTH the spawn key and the persisted escaped outcome, so every
    // other path (shot_down cell start, menu travel, dev worlds) is byte-identical
    // (spec §3.5 — importEscapedIntroFlags refuses shot_down/absent saves).
    if (canonWorld && canonPlay.built() && hc.spawnAtKey == "entrance" &&
        x3::intro::importEscapedIntroFlags(chatTrees.flags())) {
        canonPlay.cheatArm(scene);
        game.objectives().setText("REACH SARAH - SEARCH THE FACILITY");
        x3::logInfo("[handoff] ESCAPED rescuer arrival: intro flags imported "
                    "(intro.outcome=escaped), player ARMED, objective -> REACH SARAH; "
                    "spawning at 'entrance'");
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
            // WAVE-2B (LD review #1): the dev capture-rig lit every spire floor with the
            // SAME warm tungsten, which is why F3..F7 read pixel-identical. Tint the rig
            // per floor per the zone colour ladder (docs/design/TEXTURE_DESIGN_STRATEGY
            // §1.2) so --capture-spire shows the escalation the live game now carries via
            // its accent lights + emissive landmarks. Base hue defaults warm (B1/F1/F2).
            float baseR = 1.00f, baseG = 0.86f, baseB = 0.62f;   // warm tungsten (default)
            float fillR = 3.6f,  fillG = 3.8f,  fillB = 4.2f;    // cool key fill (default)
            switch (f) {
                case x3::game::L1Floor::F3:                       // Genetics — vat GREEN
                    baseR=0.42f; baseG=1.00f; baseB=0.52f; fillR=1.9f; fillG=4.2f; fillB=2.4f; break;
                case x3::game::L1Floor::F4:                       // Cybernetics — cold CYAN
                    baseR=0.45f; baseG=0.88f; baseB=1.05f; fillR=2.0f; fillG=3.6f; fillB=4.6f; break;
                case x3::game::L1Floor::F5:                       // Drone — industrial AMBER
                    baseR=1.08f; baseG=0.74f; baseB=0.38f; fillR=4.6f; fillG=3.2f; fillB=1.8f; break;
                case x3::game::L1Floor::F6:                       // Alien — BIOLUME teal
                    baseR=0.34f; baseG=1.00f; baseB=0.90f; fillR=1.7f; fillG=4.2f; fillB=3.9f; break;
                case x3::game::L1Floor::F7:                       // Executive — BRASS/warm
                    baseR=1.08f; baseG=0.92f; baseB=0.60f; fillR=4.6f; fillG=4.0f; fillB=2.8f; break;
                default: break;                                  // B1/F1/F2 stay warm neutral
            }
            const float colR = baseR * kIntensity, colG = baseG * kIntensity, colB = baseB * kIntensity;
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
                fill.color[0] = fillR; fill.color[1] = fillG; fill.color[2] = fillB;
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
            // WAVE-2B (LD review #1): the arrival vantage sits at the plate's EAST end,
            // OUTSIDE any wing room, so wing_dressing::applyEyeFog is a no-op and every
            // floor inherited the SAME default blue depth-fog — the single biggest reason
            // F3..F7 read pixel-identical. Paint the capture's atmosphere with each floor's
            // ZONE HUE (the colour ladder) so the escalation reads in one glance; this is
            // the same per-zone fog the live game applies once the player steps into the
            // wing, surfaced here at the arrival where the LD judged it. applyEyeFog leaves
            // it as-is at this vantage, so it survives the settle ticks. Spire floors only;
            // B1/F1/F2 keep a cool-neutral haze.
            {
                x3::rhi::IRenderDevice::FogParams fg;
                fg.enabled = true; fg.density = 0.018f; fg.start = 2.0f; fg.maxOpacity = 0.82f;
                switch (s.floor) {
                    case x3::game::L1Floor::F3: fg.color[0]=0.04f; fg.color[1]=0.15f; fg.color[2]=0.07f; break; // Genetics GREEN
                    case x3::game::L1Floor::F4: fg.color[0]=0.04f; fg.color[1]=0.11f; fg.color[2]=0.17f; break; // Cybernetics CYAN
                    case x3::game::L1Floor::F5: fg.color[0]=0.17f; fg.color[1]=0.09f; fg.color[2]=0.03f; break; // Drone AMBER
                    case x3::game::L1Floor::F6: fg.color[0]=0.03f; fg.color[1]=0.15f; fg.color[2]=0.13f; break; // Alien TEAL
                    case x3::game::L1Floor::F7: fg.color[0]=0.17f; fg.color[1]=0.13f; fg.color[2]=0.06f; break; // Executive BRASS
                    default:                    fg.color[0]=0.05f; fg.color[1]=0.06f; fg.color[2]=0.10f; break; // cool neutral
                }
                device->setFog(fg);
            }

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
        // UNION of both lines: playline-fold's RAGDOLL-TEARDOWN GAP FIX (game + Spire +
        // canonPlay/canon45 ragdolls out BEFORE the world dies -- this exit path skipped it
        // entirely) PLUS playable-build's own teardowns, which shutdownGameSystems() does
        // not cover. Order matters: bodies/ragdolls first, then the streamed world.
        shutdownGameSystems();
        worldCars.shutdown(*physics);   // WORLD CARS: bodies + live rig out before physics dies
        shutdownCanonStream();          // SEAM 3: region/terrain bodies out before physics dies
        physics->shutdown();
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return allOk ? 0 : 1;
    }

    // ---- --capture-wings: F2-F7 WEST-WING dressing proof. Parks the camera INSIDE each
    // floor's big signature hall (the west wing) looking across it, lights the plate, ticks
    // a few settle frames so the wing recipe dressing + per-zone fog resolve, and captures
    // floors27_<floor>.png. One still per distinct floor theme (medical/genetics/cyber/
    // drone/salvari/exec). Reuses game.drawWorldExtras (which now draws the wing dressing).
    if (captureWings) {
        namespace fs = std::filesystem;
        std::error_code mkec; fs::create_directories(captureWingsDir, mkec);
        x3::logInfo("--capture-wings: rendering F2-F7 west wings to " + captureWingsDir);
        const x3::game::L1RoomDef*    tbl = x3::game::level1Rooms();
        const x3::game::Level1Layout& Lc  = game.layout();
        const float dt = 1.0f / 60.0f;
        struct WingShot { const char* tag; x3::game::L1Floor floor; float hallCx, hallHw; };
        const WingShot shots[] = {
            { "f2", x3::game::L1Floor::F2, -38.0f, 10.0f },
            { "f3", x3::game::L1Floor::F3, -38.0f, 10.0f },
            { "f4", x3::game::L1Floor::F4, -40.0f, 11.0f },
            { "f5", x3::game::L1Floor::F5, -44.0f, 26.0f },
            { "f6", x3::game::L1Floor::F6, -42.0f, 13.0f },
            { "f7", x3::game::L1Floor::F7, -40.0f, 11.0f },
        };
        // Light the hall: a warm ceiling row (two rows in the big bays) + a cool fill by
        // the camera so the near dressing reads (dev capture lighting, not gameplay).
        auto lightHall = [&](const WingShot& s, const x3::phys::Vec3& eye) {
            const x3::game::L1RoomDef& r = tbl[(uint32_t)s.floor];
            const float lightY = r.y0 + r.ceil - 0.35f;
            const float range  = std::max(9.0f, r.ceil + 5.0f);
            std::vector<x3::rhi::PointLight> pls;
            const bool twoRows = (r.zHalf >= 12.0f);
            for (int i = -3; i <= 3; ++i) {
                x3::rhi::PointLight pl;
                pl.pos[0] = s.hallCx + i * (s.hallHw * 0.55f); pl.pos[1] = lightY;
                // UN-HACKED (engine fix 5c35d65). ed76165 pushed these to 5.6/5.0/3.9
                // because "AUTHORED mid-dark surfaces read underexposed even with
                // auto-exposure at aeMax" — that underexposure WAS the 1/pi bug: every
                // dressed wall/floor/ceiling panel AND every kit prop draws through
                // drawMeshPBR (they all carry an mr map), so the whole dressed room shaded
                // at 1/pi of the graybox beside it. mesh.frag now lights them honestly, so
                // the direct-light boost double-counts and blows the halls out. Colors go
                // back to the pre-crutch rig (3.1/2.7/2.0).
                // The RANGE x1.4 is NOT a brightness hack — it is geometric reach: this row
                // hangs at the CEILING of an 8-12 m wing hall and must still reach the floor.
                // It stays.
                pl.range = range * 1.4f; pl.color[0] = 3.1f; pl.color[1] = 2.7f; pl.color[2] = 2.0f;
                pl.pos[2] = twoRows ? -r.zHalf * 0.4f : 0.0f; pls.push_back(pl);
                if (twoRows) { pl.pos[2] = r.zHalf * 0.4f; pls.push_back(pl); }
            }
            x3::rhi::PointLight fill;
            fill.pos[0] = s.hallCx + s.hallHw - 3.0f; fill.pos[1] = r.y0 + 2.6f; fill.pos[2] = 0.0f;
            // UN-HACKED with the ceiling row above: colour back to the pre-crutch 3.2/3.4/3.9;
            // range 26 (reach across a wide hall) is geometry, not brightness — it stays.
            fill.range = 26.0f; fill.color[0] = 3.2f; fill.color[1] = 3.4f; fill.color[2] = 3.9f;
            pls.push_back(fill);
            // The dressing's OWN motivated keys (over the consoles/tanks/racks) — without
            // these the metallic kit props read black under the distant ceiling row.
            game.wingFloorLights(eye, pls);
            device->setPointLights(pls.data(), (uint32_t)pls.size());
        };
        // BLACK-PROP FIX. The kit props (drones/crates/chairs/consoles) carry dark-
        // albedo METALLIC MR maps; with no baked environment their diffuse is ~0 and
        // there is nothing to reflect, so they render as black silhouettes while the
        // diffuse graybox shell + emissive proc-geo light fine. Give the capture path
        // the same recipe the ship-interior/intro-cockpit hosts use: a healthy interior
        // ambient fill + setIblProbe(true) so the engine bakes the lit hall into the
        // environment cube and the metals reflect it (per-shot kSettle frames let the
        // probe bake resolve). Set once — the state persists across the shot loop.
        // POST-ENGINE-FIX (5c35d65): ambient/IBL was NOT part of the 1/pi bug (the fix
        // touched only the DIRECT brdf), so this fill and the probe are honest and stay —
        // the probe in particular is what gives the metals something real to reflect, and
        // it pairs with the engine's r_metalambient specular floor. 0.34 also sits BELOW
        // the engine's own default ambient (0.42/0.44/0.50), so it is not an over-unity lift.
        device->setAmbient(0.34f, 0.36f, 0.42f);
        device->setIblProbe(true);
        // UN-HACKED: the 1.35 EV bias was pure compensation — it existed because the
        // PBR-shaded room was 1/pi dark and auto-exposure had already run out of headroom
        // at aeMax. With honest lighting, AE has plenty of range; a fixed bias on top of it
        // just clips the highlights. Neutral.
        device->setExposure(1.0f);
        bool allOk = true;
        for (const WingShot& s : shots) {
            const float baseY = Lc.floorBaseY[(uint32_t)s.floor];
            const float camX = s.hallCx + s.hallHw - 2.5f;   // just inside the hall's east wall
            const float camY = baseY + 2.0f, camZ = 0.0f;
            const float camYaw = 3.14159265f, camPit = -0.10f, camFov = 82.0f;
            const x3::phys::Vec3 camEye{ camX, camY, camZ };
            const std::string outPath = captureWingsDir + "/floors27_" + std::string(s.tag) + ".png";
            const int kSettle = 18;
            for (int i = 0; i < kSettle; ++i) {
                glfwPollEvents();
                game.tick(dt, scene, *physics, camEye, camEye);
                physics->step(dt);
                scene.update(*physics);
                device->setCamera(camX, camY, camZ, camYaw, camPit, camFov);
                lightHall(s, camEye);
                if (i == kSettle - 1) device->armCapture(outPath.c_str());
                auto frame = device->beginFrame();
                if (frame.valid) {
                    scene.render(*device, frame);
                    game.drawDoors(*device, frame);
                    game.drawWorldExtras(*device, frame, scene);
                }
                device->endFrame(frame);
            }
            const bool wrote = device->captureFrame(outPath.c_str());
            if (wrote) x3::logInfo("--capture-wings: wrote " + outPath);
            else { allOk = false; x3::logError("--capture-wings: capture FAILED for " + outPath); }
        }
        x3::logInfo(std::string("--capture-wings: ") +
                    (allOk ? "all F2-F7 wings captured" : "one or more captures FAILED"));
        audio->shutdown();
        combatFx.shutdown(*device);
        shutdownGameSystems();   // RAGDOLL-TEARDOWN GAP FIX: game + Spire bodies/ragdolls out BEFORE the world dies (this exit path previously skipped it)
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
            refreshLightBudgets();
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
        worldCars.shutdown(*physics);   // WORLD CARS: bodies + live rig out before physics dies
        shutdownCanonStream();   // SEAM 3: region/terrain bodies out before physics dies
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
        // --screenshot-alert in the CANON world: stand in the Main Hall looking
        // down its long axis — the wide corridor of red-shifted room lights is
        // the lockdown read (the legacy alertCam coordinates are level1's).
        if (canonWorld && canonFloor.valid() && !shotCamOverride && alertShot) {
            uint32_t mh = canonFloor.roomByName("Main Hall");
            if (mh == x3::game::kNoRoom) mh = canonFloor.roomAt(2.0f, 0.0f, 40.0f);
            if (mh == x3::game::kNoRoom) mh = 0;
            const x3::game::CanonRoom& H = canonFloor.rooms[mh];
            const bool longX = H.w >= H.d;   // look down the longer axis
            ssX = longX ? (H.x0() + 1.6f) : H.cx;
            ssZ = longX ? H.cz : (H.z0() + 1.6f);
            ssY = H.y0() + 1.65f;
            ssYaw = longX ? 0.0f : 1.5707963f;   // +X / +Z (CONVENTIONS yaw)
            ssPitch = -0.04f;
        }
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
        // ---- W-RIFT / W-MENU: THE CAPTURE HOOKS -------------------------------------
        // Three env knobs so the rift-network work can be PHOTOGRAPHED through the real
        // code paths (no bespoke screenshot host, no faked frames):
        //
        //   X3_RIFT_TARGET=<gate>=<dest>  re-aim gate <gate> (1-based) at <dest> through
        //                                 the SAME Rifthub::setDestination the console
        //                                 calls — so the still shows the holoterminal's
        //                                 REAL rebaked readout, not a caption.
        //   X3_RIFT_TRAVERSE=<gate>       stand a virtual eye in gate <gate>'s throat and
        //                                 run the EXACT traversal the live loop runs
        //                                 (traversalPortal -> destination -> resolver),
        //                                 then put the shot camera where it LANDS. A gate
        //                                 that refuses does not move the camera, and the
        //                                 log says why: a refusal photographs as a refusal.
        //   X3_WORLD_MENU=1               draw the world/place directory over the still.
        x3::game::WorldMenu shotMenu;
        if (const char* rt = std::getenv("X3_RIFT_TARGET")) {
            const std::string s(rt);
            const size_t eq = s.find('=');
            if (eq != std::string::npos && riftBuilt) {
                const int g = std::atoi(s.substr(0, eq).c_str()) - 1;
                if (g >= 0 && g < (int)rifthub.portalCount())
                    rifthub.setDestination((uint32_t)g, s.substr(eq + 1));
            }
        }
        if (const char* tv = std::getenv("X3_RIFT_TRAVERSE")) {
            const int g = std::atoi(tv) - 1;
            if (riftBuilt && g >= 0 && g < (int)rifthub.portalCount()) {
                rifthub.onTrigger(rifthub.portal((uint32_t)g).triggerId);   // walk in
                for (int w = 0; w < 240; ++w) rifthub.tick(1.0f / 60.0f, scene);  // settle OPEN
                const x3::phys::Vec3 gp = rifthub.portal((uint32_t)g).worldPos;
                const x3::phys::Vec3 throat{ gp.x, kRiftFloorY + 2.2f, gp.z };
                const int tp = rifthub.traversalPortal(throat, 2.5f);
                if (tp < 0) {
                    x3::logWarn("[rift-capture] gate " + std::to_string(g + 1) +
                                " takes you nowhere (dormant or collapsed)");
                } else {
                    const std::string dest = rifthub.destination((uint32_t)tp);
                    x3::phys::Vec3 to{};
                    std::string    why;
                    if (riftDestination(dest, to, &why)) {
                        ssX = to.x; ssY = to.y + 0.6f; ssZ = to.z;
                        ssYaw = 0.6f; ssPitch = -0.05f;
                        // X3_RIFT_LOOK="<yaw>,<pitch>" — aim the arrival shot (the
                        // landing puts you somewhere real, but "somewhere real" is a
                        // point, not a composition).
                        if (const char* lk = std::getenv("X3_RIFT_LOOK")) {
                            const std::string L(lk);
                            const size_t cm = L.find(',');
                            ssYaw = std::strtof(L.c_str(), nullptr);
                            if (cm != std::string::npos)
                                ssPitch = std::strtof(L.c_str() + cm + 1, nullptr);
                        }
                        const x3::game::Destination* dd = x3::game::findDestination(dest);
                        x3::logInfo("[rift-capture] TRAVERSED gate " + std::to_string(tp + 1) +
                                    " -> " + (dd ? dd->name : dest) + " at (" +
                                    std::to_string(to.x) + ", " + std::to_string(to.y) + ", " +
                                    std::to_string(to.z) + ")");
                    } else {
                        x3::logWarn("[rift-capture] gate " + std::to_string(tp + 1) +
                                    " -> '" + dest + "': " + why + " — the gate holds");
                    }
                }
            }
        }
        const bool shotWorldMenu = std::getenv("X3_WORLD_MENU") != nullptr;
        if (shotWorldMenu) shotMenu.open();

        // ---- POLISH (W10 proof shots): X3_SHOT_SWIM=fp|3p — stage the swim reads
        // over THE RIVER (env-var staging, the X3_SHOT_RPG pattern). fp = the eye
        // at the buoyancy line with the swim viewmodel LOWER at full blend (proof
        // the pistol no longer clips the surface); 3p = the Jake avatar PRONE at
        // the surface, framed from beside the channel. The spot is the river
        // spline's midpoint (the same node table the carve + ribbon build from),
        // so the shot sits over real deep water. Pair with --shot-settle >= 150
        // so the terrain/water tiles stream in under the vantage.
        const char* swimShotEnv = std::getenv("X3_SHOT_SWIM");
        const std::string swimShotMode = (canonWorld && swimShotEnv) ? swimShotEnv : "";
        // X3_SHOT_SWIM_SPEED (m/s): >0 makes the staged swimmer actually SWIM
        // FORWARD, so the stroke latch trips and the "Swim" (breaststroke) clip
        // plays instead of "SwimIdle" (treading). 0 = the tread-water read.
        // X3_SHOT_SWIM_PHASE (0..1): WHERE IN THE STROKE the still lands — 0.30
        // is the catch/pull (arms sweeping wide), 0.66 the kick/recovery. The
        // clip clock is just the frame count, so we PRE-ROLL the avatar the extra
        // frames needed to land the requested phase on the capture frame.
        const char* swimSpeedEnv = std::getenv("X3_SHOT_SWIM_SPEED");
        const char* swimPhaseEnv = std::getenv("X3_SHOT_SWIM_PHASE");
        const float swimShotSpeed = swimSpeedEnv ? (float)std::atof(swimSpeedEnv) : 0.0f;
        const float swimShotPhase = swimPhaseEnv ? (float)std::atof(swimPhaseEnv) : 0.0f;
        x3::phys::Vec3 swimSpot{}; float swimYaw = 0.0f, swimWaterY = 0.0f;
        bool swimShotOk = false;
        if (!swimShotMode.empty()) {
            uint32_t rn = 0;
            const x3::game::WorldRiverNode* nodes = x3::game::worldRiverNodes(rn);
            if (nodes && rn >= 2) {
                const uint32_t mid = rn / 2;
                const x3::game::WorldRiverNode& A = nodes[mid - 1];
                const x3::game::WorldRiverNode& B = nodes[mid];
                swimSpot = x3::phys::Vec3{ (A.x + B.x) * 0.5f, 0.0f, (A.z + B.z) * 0.5f };
                const float wY = x3::game::worldWaterLevelAt(swimSpot.x, swimSpot.z);
                if (wY > -1.0e30f) {
                    swimWaterY = wY;
                    swimYaw = std::atan2(B.z - A.z, B.x - A.x);   // look downstream
                    swimShotOk = true;
                    if (swimShotMode == "3p" && thirdPerson.built()) {
                        thirdPerson.setThirdPerson(true);
                        // ---- FRAME THE STROKE, THEN PUT THE SUN BEHIND THE CAMERA.
                        // Two lessons from the v1 proof (an unreadable backlit blob):
                        //  * a beam-on view at water level foreshortens both arms into
                        //    the torso, and Jake's kit is near-black cloth (measured: at
                        //    sunLight 12 / ambient 0.95 he is STILL a dark shape — that
                        //    is his albedo, not the lighting). From BEHIND AND ABOVE the
                        //    stroke projects wide: the arms sweep out to the sides on the
                        //    catch, the frog kick opens behind him. That silhouette reads.
                        //  * the canon golden-hour sun sits 9 deg above the horizon, so
                        //    half the compass is a backlight. We are the screenshot host,
                        //    so we simply MOVE THE SUN: azimuth = straight behind this
                        //    camera, elevation ~40 deg. Front-lit, every time, whatever
                        //    the river's local heading is.
                        const float camDist = 2.9f;   // behind him, along the swim axis
                        const float camSide = 1.15f;  // a touch off-axis (three-quarter)
                        const float side = swimYaw + 1.5707963f;
                        ssX = swimSpot.x - std::cos(swimYaw) * camDist +
                              std::cos(side) * camSide;
                        ssZ = swimSpot.z - std::sin(swimYaw) * camDist +
                              std::sin(side) * camSide;
                        ssY = wY + 1.55f;                           // up over the water
                        // Aim at the body just under the surface line.
                        const float tX = swimSpot.x, tZ = swimSpot.z, tY = wY + 0.10f;
                        ssYaw = std::atan2(tZ - ssZ, tX - ssX);
                        const float horiz = std::sqrt((tX - ssX) * (tX - ssX) +
                                                      (tZ - ssZ) * (tZ - ssZ));
                        ssPitch = std::atan2(tY - ssY, horiz > 0.01f ? horiz : 0.01f);
                        // THE SUN, STAGED FOR THE CAPTURE ONLY (the live game's
                        // time-of-day is untouched — this runs in the screenshot host,
                        // behind X3_SHOT_SWIM): toward the sun = back along the camera's
                        // own view ray, lifted to ~40 deg, whitened, with the scene sun
                        // radiance + sky ambient raised so the body has surface, not just
                        // outline.
                        x3::rhi::IRenderDevice::SkyParams swimSky{};
                        swimSky.enabled = true;
                        swimSky.sunDir[0] = -std::cos(ssYaw) * 0.77f;   // behind the camera
                        swimSky.sunDir[1] = 0.64f;                      // ~40 deg elevation
                        swimSky.sunDir[2] = -std::sin(ssYaw) * 0.77f;
                        swimSky.sunColor[0] = 1.0f; swimSky.sunColor[1] = 0.98f;
                        swimSky.sunColor[2] = 0.94f;
                        swimSky.sunIntensity = 1.30f;
                        swimSky.sunLight     = 3.60f;              // the mesh key
                        swimSky.haze = 0.42f; swimSky.exposure = 1.25f;
                        swimSky.zenith[0]  = 0.26f; swimSky.zenith[1]  = 0.46f;
                        swimSky.zenith[2]  = 0.82f;
                        swimSky.horizon[0] = 0.86f; swimSky.horizon[1] = 0.90f;
                        swimSky.horizon[2] = 0.98f;
                        device->setSkyParams(swimSky);
                    } else {
                        // FP: the eye rests just above the surface (the swim
                        // buoyancy line), pitched a touch UP so the water line
                        // crosses the lower frame exactly where the DEFAULT
                        // viewmodel pose breaks the surface (the clip the lower
                        // fixes — shoot the control at the same --shot-cam).
                        ssX = swimSpot.x; ssY = wY + 0.18f; ssZ = swimSpot.z;
                        ssYaw = swimYaw; ssPitch = 0.08f;
                    }
                    x3::logInfo("X3_SHOT_SWIM=" + swimShotMode + ": staged at river ("
                                + std::to_string(swimSpot.x) + ", " + std::to_string(swimSpot.z)
                                + ") waterY=" + std::to_string(wY));
                } else {
                    x3::logWarn("X3_SHOT_SWIM: no water at the staged river spot — unstaged");
                }
            }
        }
        // ---- THE WATER ZAP proof shots: X3_SHOT_ZAP=school|zap|after|punish ----
        // Staged on the FIRST fish school (the river reach nearest the facility —
        // the same worldRiverNodes spline the fish were seeded on). The camera is
        // placed so the GOLDEN-HOUR SUN (sunDir 0.55,0.16,-0.35 — azimuth -0.57 rad)
        // is BEHIND it: the subject is lit, never a backlit silhouette.
        //   school = the shoal under the surface, from the bank (fish must read)
        //   zap    = the discharge mid-flight (arcs spidering across the water)
        //   after  = the aftermath (dead fish belly-up on the surface)
        //   punish = the player's own punishment (swim + zap => HUD health halved)
        // The zap is fired through the REAL fireWaterZap() path with the REAL
        // findWaterEntry() entry and the REAL zapPlayer() damage — the still is a
        // photograph of the game, not a painting of it.
        const char* zapShotEnv = std::getenv("X3_SHOT_ZAP");
        const std::string zapShotMode =
            (canonWorld && zapShotEnv && worldFish.built() && worldFish.schoolCount() > 0)
                ? zapShotEnv : "";
        x3::game::Player zapShotPlayer;      // the staged swimmer (real Player, real damage)
        int  zapShotHp = x3::game::kPlayerMaxHp;
        bool zapShotFired = false;
        int  zapShotLead = -1;               // frames BEFORE the capture that the zap goes off
        x3::phys::Vec3 zapShotCenter{}, zapShotFeet{};
        // SUBJECT-TRACKING stills (the REAL-FISH proofs: pike / perch). A fish is
        // not a prop — over 150 settle frames the pike swims clean out of a static
        // frame. So these modes lock the camera onto ONE fish and re-aim it every
        // settle frame, holding it in PROFILE with the staged sun on the near flank.
        // The fish is still swimming its own sim; only the camera is a steadicam.
        int   zapShotTrack = -1;             // fish index to hold in frame (-1 = static cam)
        float zapShotTrackDist = 3.0f;       // stand-off (m) — must clear fleeRadius (2.5)
        float zapShotTrackFov  = 40.0f;      // tight lens: fill the frame from outside the bolt radius
        if (!zapShotMode.empty()) {
            // Pick the school this still is ABOUT: the pike shot wants the pike's
            // school, not school 0 (the rudd shoal).
            uint32_t shotSchool = 0;
            x3::game::FishSpecies want = x3::game::FishSpecies::Rudd;
            bool bySpecies = false;
            if (zapShotMode == "pike")  { want = x3::game::FishSpecies::Pike;  bySpecies = true; }
            if (zapShotMode == "perch") { want = x3::game::FishSpecies::Perch; bySpecies = true; }
            if (zapShotMode == "shoal") { want = x3::game::FishSpecies::Rudd;  bySpecies = true; }
            if (bySpecies) {
                for (uint32_t i = 0; i < worldFish.schoolCount(); ++i) {
                    if (worldFish.school(i).species == want) { shotSchool = i; break; }
                }
                for (uint32_t i = 0; i < worldFish.fishCount(); ++i) {
                    if (worldFish.fish(i).species == want) { zapShotTrack = (int)i; break; }
                }
                // A rudd is 26 cm and a perch 24 cm: from OUTSIDE the 2.5 m bolt
                // radius (we will not spook them just to photograph them) the only
                // way they read as FISH and not as flecks is a LONG LENS. The pike
                // is a metre long and needs none of that.
                if (zapShotMode == "perch") { zapShotTrackDist = 3.3f; zapShotTrackFov = 29.0f; }
                if (zapShotMode == "shoal") { zapShotTrackDist = 2.7f; zapShotTrackFov = 28.0f; }
            }
            const x3::game::FishSchool& sc = worldFish.school(shotSchool);
            const float wY = x3::game::worldWaterLevelAt(sc.cx, sc.cz);
            if (wY > x3::game::kFishDryTest) {
                zapShotCenter = x3::phys::Vec3{ sc.cx, wY, sc.cz };
                // Sun-behind-camera framing: look along -sunDirXZ (yaw ~2.575 rad).
                const float lookYaw = std::atan2(0.35f, -0.55f);
                const float backX = -std::cos(lookYaw), backZ = -std::sin(lookYaw);
                arsenal.selectByName("lightning");
                if (zapShotMode == "punish") {
                    // FP, IN the water at the school: the eye at the buoyancy line.
                    ssX = sc.cx + backX * 2.6f; ssY = wY + 0.30f; ssZ = sc.cz + backZ * 2.6f;
                    ssYaw = lookYaw; ssPitch = -0.16f;
                    zapShotFeet = x3::phys::Vec3{ ssX, wY - 1.45f, ssZ };   // swimming
                    zapShotLead = 8;    // arcs still alive at capture
                } else if (zapShotMode == "zap") {
                    // From the bank, ABOVE the water looking DOWN on it, so the 12 m
                    // radius of arcs reads as a spider web across the surface (a low
                    // grazing camera compresses the whole discharge into a line).
                    ssX = sc.cx + backX * 9.0f; ssY = wY + 3.6f; ssZ = sc.cz + backZ * 9.0f;
                    ssYaw = lookYaw; ssPitch = -0.34f;
                    zapShotFeet = x3::phys::Vec3{ ssX, wY + 3.2f, ssZ };    // on the bank: no self-damage
                    zapShotLead = 4;    // mid-discharge
                } else if (zapShotMode == "after") {
                    // The aftermath: the corpses have risen and are floating.
                    ssX = sc.cx + backX * 3.4f; ssY = wY + 1.35f; ssZ = sc.cz + backZ * 3.4f;
                    ssYaw = lookYaw - 0.05f; ssPitch = -0.42f;
                    zapShotFeet = x3::phys::Vec3{ ssX, wY + 1.2f, ssZ };
                    zapShotLead = 300;  // 5 s before the capture (the corpses have surfaced)
                } else if (zapShotMode == "under") {
                    // UNDERWATER, in the channel beside the shoal (outside fleeRadius
                    // so they hold): the read a SWIMMER gets — fish at eye level.
                    ssX = sc.cx + backX * 4.6f; ssY = wY - 0.80f; ssZ = sc.cz + backZ * 4.6f;
                    ssYaw = lookYaw; ssPitch = -0.05f;
                } else if (zapShotTrack >= 0) {
                    // pike / perch: seed the camera on the subject's PROFILE. The
                    // settle loop then re-aims it every frame (see zapShotTrack).
                    const x3::game::Fish& f = worldFish.fish((uint32_t)zapShotTrack);
                    ssX = f.x + backX * zapShotTrackDist;
                    ssY = f.y + 0.10f;
                    ssZ = f.z + backZ * zapShotTrackDist;
                    ssYaw = lookYaw; ssPitch = -0.04f;
                } else {   // "school": from the BANK, looking down into the channel
                    ssX = sc.cx + backX * 5.0f; ssY = wY + 1.2f; ssZ = sc.cz + backZ * 5.0f;
                    ssYaw = lookYaw + 0.05f; ssPitch = -0.34f;
                }
                // STAGING LIGHT (the legibility gate — the swim proof failed once by
                // being a backlit silhouette): the canon sky is a LOW golden-hour sun
                // that leaves the river a near-black mirror. For the zap stills only,
                // lift the sun to ~35 deg on the SAME warm azimuth, BEHIND the camera,
                // and brighten it: the shoal is lit, not silhouetted. Shot staging
                // only — the live world keeps applyGoldenHourSky().
                {
                    x3::rhi::IRenderDevice::SkyParams sp{};
                    sp.enabled = true;
                    sp.sunDir[0] = 0.55f; sp.sunDir[1] = 0.70f; sp.sunDir[2] = -0.35f;
                    sp.sunColor[0] = 1.0f; sp.sunColor[1] = 0.94f; sp.sunColor[2] = 0.86f;
                    sp.sunIntensity = 2.1f; sp.haze = 0.30f; sp.exposure = 1.25f;
                    sp.zenith[0]  = 0.16f; sp.zenith[1]  = 0.26f; sp.zenith[2]  = 0.44f;
                    sp.horizon[0] = 0.62f; sp.horizon[1] = 0.58f; sp.horizon[2] = 0.48f;
                    device->setSkyParams(sp);
                }
                x3::logInfo("X3_SHOT_ZAP=" + zapShotMode + ": staged on fish school 0 at ("
                            + std::to_string(sc.cx) + ", " + std::to_string(sc.cz)
                            + ") waterY=" + std::to_string(wY) + " cam=("
                            + std::to_string(ssX) + "," + std::to_string(ssY) + ","
                            + std::to_string(ssZ) + ")");
            } else {
                x3::logWarn("X3_SHOT_ZAP: fish school 0 is dry?! — unstaged");
            }
        }

        // ---- THE OCEAN LIVES proof shots: X3_SHOT_SEALIFE=fin|shark|squid|zap ----
        // Staged on a real creature from worldSea, framed with the SUN BEHIND THE
        // CAMERA (a previous proof shipped as a backlit silhouette — light the
        // subject). The creature is re-seated once, then left to SWIM through the
        // settle frames, so the fin drags a real wake and the body is caught mid-
        // flex by its own baked clip. Nothing here is a painting: the zap goes off
        // through the REAL fireWaterZap() path.
        //   fin   = the dorsal cutting the surface + wake (the money shot)
        //   wake  = the fin staging, framed WIDE from above-behind so the trailing
        //           foam V (app/sealife.h THE WAKE) is the subject, not the fin
        //   shark = close, in profile, underwater, mid-cruise
        //   squid = the abyss: the giant squid down in the dark
        //   zap   = the payoff: the water goes live and the shark dies
        const char* seaShotEnv = std::getenv("X3_SHOT_SEALIFE");
        const std::string seaShotMode =
            (canonWorld && seaShotEnv && worldSea.built() && worldSea.count() > 0)
                ? seaShotEnv : "";
        int  seaShotIdx = -1;
        int  seaShotZapLead = -1;
        bool seaShotZapFired = false;
        x3::phys::Vec3 seaShotCenter{};
        // CAPTURE lights (a tool, not gameplay — same trick the facility floor
        // captures use). Four metres under the sea the sun contributes almost
        // nothing, so the animal renders as a black cut-out and every bit of the
        // normal-mapped skin we paid for is invisible. Light the subject.
        std::vector<x3::rhi::PointLight> seaShotLights;
        x3::rhi::IRenderDevice::SkyParams seaShotSky{};
        // A TRACKING camera. The creature keeps SWIMMING through the settle frames
        // (2.3 m/s x 3.3 s = ~8 m), so a camera framed once at stage time is aimed at
        // where he WAS — the first fin shot photographed an empty patch of sea. Freezing
        // him would kill the wake (a wake is made by moving), so instead the camera
        // rides alongside: offsets in HIS frame, re-solved every frame.
        float seaShotSide = 6.0f, seaShotBack = 3.0f, seaShotUp = 0.8f;
        bool  seaShotAtSurface = false;   // aim at the fin (surface) vs the body
        if (!seaShotMode.empty()) {
            const bool wantSquid = (seaShotMode == "squid");
            seaShotIdx = worldSea.findSpecies(wantSquid ? x3::game::SeaSpecies::GiantSquid
                                                        : x3::game::SeaSpecies::GreatWhite);
            if (seaShotIdx >= 0) {
                x3::game::SeaCreature& c = worldSea.creatureMut((uint32_t)seaShotIdx);
                const float wY = x3::game::worldWaterLevelAt(c.homeX, c.homeZ);
                if (wY > x3::game::kFishDryTest) {
                    // Seat him on his home, swimming along +X across the view.
                    c.x = c.homeX; c.z = c.homeZ;
                    c.yaw = 1.5708f;              // heading -X..+X across frame
                    c.state = x3::game::SeaState::Patrol;
                    c.holdDepth = true;   // the patrol sine must not sink the subject
                    const float side = 13.0f;     // camera stand-off to his flank

                    if (seaShotMode == "fin" || seaShotMode == "zap") {
                        // THE DORSAL, and ONLY the dorsal. The model is 5 m long and
                        // 0.425 of that tall, so its fin TIP rides ~1.06 m above the
                        // origin: a 0.55 m centre depth breaches the whole BACK (a
                        // beached shark, not a cutting fin). 0.78 m leaves ~0.28 m of
                        // fin through the surface and hides the body.
                        c.wantDepth = 0.62f;      // ~0.44 m of FIN through the surface
                        c.y = wY - 0.62f;
                        seaShotSide = 3.0f; seaShotBack = 1.6f; seaShotUp = 0.38f;
                        seaShotAtSurface = true;
                    } else if (seaShotMode == "wake") {
                        // THE WAKE from the bank: same surfaced staging as `fin`,
                        // but the camera stands off high and behind so the foam V
                        // trailing him is the subject. Use a big --screenshot
                        // settle count (~400) so he drags a full-length trail.
                        c.wantDepth = 0.62f;
                        c.y = wY - 0.62f;
                        seaShotSide = 7.5f; seaShotBack = 12.0f; seaShotUp = 3.4f;
                        seaShotAtSurface = true;
                    } else if (seaShotMode == "shark") {
                        // UNDERWATER, close, in profile: he should FILL the frame.
                        c.wantDepth = 4.0f;
                        c.y = wY - 4.0f;
                        seaShotSide = 5.8f; seaShotBack = 1.2f; seaShotUp = 0.35f;
                    } else {   // squid: the abyss
                        const float bedY = x3::game::terrainHeightAtWorld(c.homeX, c.homeZ);
                        const float depth = std::min(46.0f, std::max(20.0f, wY - bedY - 14.0f));
                        c.wantDepth = depth;
                        c.y = wY - depth;
                        seaShotSide = 11.0f; seaShotBack = 5.0f; seaShotUp = 2.0f;
                    }
                    {
                        const float fx = -std::sin(c.yaw), fz = -std::cos(c.yaw);
                        const float rx = -fz, rz = fx;
                        ssX = c.x - fx * seaShotBack + rx * seaShotSide;
                        ssZ = c.z - fz * seaShotBack + rz * seaShotSide;
                        ssY = (seaShotAtSurface ? wY : c.y) + seaShotUp;
                        const float ty = seaShotAtSurface ? (wY + 0.15f) : c.y;
                        const float hx = c.x - ssX, hz = c.z - ssZ;
                        ssYaw = std::atan2(hz, hx);
                        ssPitch = std::atan2(ty - ssY, std::sqrt(hx * hx + hz * hz));
                    }
                    seaShotCenter = x3::phys::Vec3{ c.x, wY, c.z };
                    if (seaShotMode == "zap") {
                        arsenal.selectByName("lightning");
                        seaShotZapLead = 26;      // fire before capture: he is DEAD in frame
                    }

                    // LIGHT THE SUBJECT. Sun high and behind the camera; underwater
                    // shots also need the exposure lifted or the animal is a smudge.
                    x3::rhi::IRenderDevice::SkyParams& sp = seaShotSky;
                    const float sunAz = ssYaw;    // sun shares the camera's bearing
                    sp.sunDir[0] = std::cos(sunAz) * 0.55f;
                    sp.sunDir[1] = 0.72f;
                    sp.sunDir[2] = std::sin(sunAz) * 0.55f;
                    sp.sunColor[0] = 1.0f; sp.sunColor[1] = 0.96f; sp.sunColor[2] = 0.90f;
                    const bool deep = (seaShotMode == "squid");
                    sp.sunIntensity = deep ? 3.4f : 2.2f;
                    sp.exposure     = deep ? 1.85f : 1.30f;
                    sp.haze = 0.28f;
                    sp.zenith[0]  = 0.16f; sp.zenith[1]  = 0.26f; sp.zenith[2]  = 0.44f;
                    sp.horizon[0] = 0.62f; sp.horizon[1] = 0.58f; sp.horizon[2] = 0.48f;
                    device->setSkyParams(sp);

                    {
                        const bool deepShot = (seaShotMode == "squid");
                        // KEY: at the eye, so it lights the flank we are looking at.
                        x3::rhi::PointLight key;
                        key.pos[0] = ssX; key.pos[1] = ssY + 1.2f; key.pos[2] = ssZ;
                        key.range  = deepShot ? 70.0f : 42.0f;
                        const float ki = deepShot ? 14.0f : 11.0f;
                        key.color[0] = ki; key.color[1] = ki * 0.98f; key.color[2] = ki * 0.92f;
                        seaShotLights.push_back(key);
                        // RIM: above and beyond him, to peel the back off the water.
                        x3::rhi::PointLight rim;
                        rim.pos[0] = c.x + 6.0f; rim.pos[1] = c.y + 9.0f; rim.pos[2] = c.z + 5.0f;
                        rim.range  = deepShot ? 60.0f : 38.0f;
                        const float ri = deepShot ? 8.0f : 5.5f;
                        // the abyss gets a cold bioluminescent rim; the shallows stay daylight
                        rim.color[0] = deepShot ? ri * 0.35f : ri;
                        rim.color[1] = deepShot ? ri * 0.85f : ri;
                        rim.color[2] = deepShot ? ri * 1.00f : ri * 0.95f;
                        seaShotLights.push_back(rim);
                    }
                    x3::logInfo("X3_SHOT_SEALIFE=" + seaShotMode + ": staged "
                                + x3::game::seaSpeciesDef(c.species).key + " at ("
                                + std::to_string(c.x) + "," + std::to_string(c.y) + ","
                                + std::to_string(c.z) + ") surf=" + std::to_string(wY)
                                + " cam=(" + std::to_string(ssX) + "," + std::to_string(ssY)
                                + "," + std::to_string(ssZ) + ")");
                } else {
                    x3::logWarn("X3_SHOT_SEALIFE: the creature's home is DRY?! — unstaged");
                }
            } else {
                x3::logWarn("X3_SHOT_SEALIFE: no such creature in the world — unstaged");
            }
        }

        // ---- W4-2: --screenshot-vigil — seed a live VIGIL conversation ON the cell
        // glass (orange ink) before the settle frames so the bake lands in-frame.
        // The default hero camera already frames the terminal; a compact inline
        // renderer mirrors the interactive path (its helpers live later in scope).
        const float ssFov = 70.0f;
        device->setCamera(ssX, ssY, ssZ, ssYaw, ssPitch, ssFov);
        const x3::phys::Vec3 ssEye{ ssX, ssY, ssZ };
        const float dt = 1.0f / 60.0f;
        // SEAM 3: stills want the whole visible terrain ring, not a per-frame
        // trickle — fill it fast across the settle frames (host_streamed's
        // screenshot treatment), and let the FULL ring enqueue on the shot
        // camera's single boundary crossing (a static camera never crosses
        // again, so the default 24-in-flight cap would strand the outer ring).
        // No-op unless canon streaming booted.
        if (canonStreamOn) {
            terrainStreamer.setUploadBudget(64);
            terrainStreamer.setMaxInFlight(512);
        }
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
        // Weapon held in the capture. Default = pistol (slot 0); override with
        // --set shot_weapon <name> to proof ANY gun's viewmodel skin headlessly —
        // the QA gate for the per-weapon texture rebind (tools/rebind_weapon_textures.py).
        {
            const std::string shotWeapon = console->getString("shot_weapon");
            if (shotWeapon.empty() || !arsenal.selectByName(shotWeapon)) arsenal.select(0);
            // X3_SHOT_ZAP: the LIGHTNING gun is the subject of these stills — it must
            // be the weapon in hand (this block would otherwise reset to the pistol).
            if (!zapShotMode.empty() && shotWeapon.empty()) arsenal.selectByName("lightning");
        }
        // --screenshot-alert: stage the LEVEL-3 LOCKDOWN for the proof shot —
        // force the alert, lock the zone doors, and shift every facility light
        // hard red (the same effects the live loop applies).
        if (alertShot && canonWorld && canonFloor.valid()) {
            // ---- CANON LOCKDOWN PROOF SHOT (polish): stage the LEVEL-3 state
            // through the SAME debug-force + door-lock + red-shift path the live
            // canon loop uses. The settle loop below rebuilds the canon light
            // feed (cl) every frame — the red shift is applied THERE (see the
            // alertShot line after selectVisibleCanonLights), so what the still
            // shows is the live look, not a bespoke capture rig.
            facilityAlert.configure(x3::game::loadAlertConfig(x3::game::alertJsonPath()));
            facilityAlert.debugForceLevel(3);
            alertDoorLock.update(facilityAlert, canonDoors);
            x3::logInfo("alert shot (canon): LEVEL-3 LOCKDOWN staged over the facility");
        } else if (alertShot) {
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
            // B4: nearest-to-eye, not first-in-array (see nearestFixtures above).
            // Red shift through the shared helper (same math this block held —
            // the live canon loop + this staging path must never drift apart).
            std::vector<x3::rhi::PointLight> fl =
                nearestFixtures(game.lightFixtures(), ssX, ssY, ssZ, fixtureBudget);
            applyAlertRedShift(fl, facilityAlert.redShift());
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
            if (fl.size() > lightBudget) fl.resize(lightBudget);
            device->setPointLights(fl.data(), (uint32_t)fl.size());
        }
        // B4: the LEGACY tower's ceiling fixtures, fed NEAREST-TO-EYE. Level1Game::build
        // pushed all 332 at build time and setPointLights silently kept the first 64 (see
        // nearestFixtures) — so a --screenshot of level1 photographed rooms whose own
        // ceiling lights had been dropped, and only the 0.42 ambient wash made the frame
        // look "lit" at all. The canon/alert paths re-issue their own sets below/above;
        // this is the plain capture path, which never re-issued anything.
        if (!canonWorld && !game.lightFixtures().empty()) {
            std::vector<x3::rhi::PointLight> sfl =
                nearestFixtures(game.lightFixtures(), ssX, ssY, ssZ, fixtureBudget);
            device->setPointLights(sfl.data(), (uint32_t)sfl.size());
            x3::logInfo("[light] screenshot: fed " + std::to_string(sfl.size()) +
                        " nearest ceiling fixtures of " +
                        std::to_string(game.lightFixtures().size()) + " (B4)");
        }
        const int kSettleFrames = (screenshotSettle > 0) ? screenshotSettle
                                                         : (alertShot ? 110 : 16);
        // ---- X3_SHOT_SWIM_PHASE pre-roll: land the capture ON a chosen point of
        // the stroke. The swim clip's clock is just the accumulated dt since the
        // crossfade triggered, so the phase at the capture frame is fixed by the
        // total number of swim updates. Run the extra (render-free) updates HERE,
        // ahead of the settle loop, so that settle + preroll == the frame count
        // that puts the requested phase (0.30 = the catch/pull, 0.66 = the kick)
        // on the shot. The feet march at the staged speed through the pre-roll too
        // (a still speed would drop him out of the stroke on the hysteresis latch).
        if (swimShotOk && swimShotMode == "3p" && thirdPerson.built() &&
            swimShotSpeed > 0.0f) {
            const float clipLen = 2.3333333f;                  // "Swim" @ 24fps x 56f
            const int cycle = (int)std::lround(clipLen / dt);  // frames per stroke
            const int want  = (int)std::lround(
                std::fmod(std::fmax(swimShotPhase, 0.0f), 1.0f) * (float)cycle);
            int pre = ((want - kSettleFrames) % cycle + cycle) % cycle;
            if (pre < 20) pre += cycle;      // >= 20 frames so the crossfade completes
            for (int i = 0; i < pre; ++i) {
                const float lead = swimShotSpeed * (float)(pre - 1 - i + kSettleFrames) * dt;
                const x3::phys::Vec3 f{ swimSpot.x - std::cos(swimYaw) * lead,
                                        swimWaterY - 1.45f,
                                        swimSpot.z - std::sin(swimYaw) * lead };
                thirdPerson.update(dt, scene, f, 1.6f, swimYaw, 0.0f,
                                   x3::game::kNoRoom, false, false, /*swimming*/true);
            }
            x3::logInfo("[swimshot] phase pre-roll: " + std::to_string(pre) +
                        " frames (target phase " + std::to_string(swimShotPhase) +
                        ", settle " + std::to_string(kSettleFrames) + ")");
        }
        // WORLD CARS driver-POV staging (--shot-drive): take the wheel of the
        // car nearest the shot camera and DRIVE it through the settle frames —
        // the capture camera follows the live chase framing and the "[E] Exit"
        // hint draws. Honest: the same enter path, physics and HUD the live
        // loop uses. Pair with --shot-settle 240 so the car covers real road.
        bool shotDriving = false;
        if (shotDrive && canonWorld && worldCars.built()) {
            worldCars.interact(ssEye, true, true, false, dt, nullptr, *physics, nullptr);
            shotDriving = worldCars.driving();
            if (!shotDriving)
                x3::logWarn("--shot-drive: no (unlocked) car within reach of the shot camera");
        }
        // ---- CROWD CHATTER staging (--shot-chatter N): advance the crowd +
        // chatter sim (deterministic, render-free — the same updates the settle
        // loop runs) until N chat bubbles are concurrently ALIVE (with >= 1 s
        // left) within bubble range of the shot camera, so the capture frame
        // catches THE PEOPLE mid-sentence. Bounded at 4 min of sim; logs what
        // it staged (and the nearest bubble's world position for re-aiming). ----
        if (canonWorld && hc.shotChatter > 0) {
            auto bubblesNearCam = [&]() {
                uint32_t cnt = 0;
                auto scan = [&](const x3::game::CrowdChatter& ch,
                                const x3::game::CrowdSystem& cs) {
                    if (!cs.built()) return;
                    for (uint32_t bi = 0; bi < x3::game::CrowdChatter::kMaxBubbles; ++bi) {
                        const x3::game::ChatterBubble& b = ch.bubbleSlot(bi);
                        if (b.agent == x3::game::kNoLink ||
                            b.agent >= cs.agentCount()) continue;
                        if (b.ttl - b.age < 1.0f) continue;   // must survive the settle
                        const auto& a = cs.agent(b.agent);
                        const float dx = a.pos.x - ssEye.x, dz = a.pos.z - ssEye.z;
                        if (dx * dx + dz * dz <
                            x3::game::CrowdChatter::kBubbleRange *
                            x3::game::CrowdChatter::kBubbleRange - 4.0f)
                            ++cnt;
                    }
                };
                for (int ci = 0; ci < 3; ++ci) {
                    scan(canonChatter[ci], canonCrowds[ci]);
                    scan(cityChatter[ci], cityCrowds[ci]);
                }
                return cnt;
            };
            int staged = 0;
            const int kStageMax = 60 * 240;
            while ((int)bubblesNearCam() < hc.shotChatter && staged < kStageMax) {
                for (auto& cc : canonCrowds) if (cc.built()) cc.update(dt, scene);
                for (auto& cc : cityCrowds)  if (cc.built()) cc.update(dt, scene);
                for (int ci = 0; ci < 3; ++ci) {   // silent warp: no audio fires
                    if (canonCrowds[ci].built())
                        canonChatter[ci].update(dt, canonCrowds[ci], nullptr,
                                                chatterSnd, ssEye);
                    if (cityCrowds[ci].built())
                        cityChatter[ci].update(dt, cityCrowds[ci], nullptr,
                                               chatterSnd, ssEye);
                }
                ++staged;
            }
            // Log the staged count + every staged bubble's world position (so a
            // shot camera can be re-aimed from the log instead of guessed).
            std::string spots;
            auto listSpots = [&](const x3::game::CrowdChatter& ch,
                                 const x3::game::CrowdSystem& cs) {
                if (!cs.built()) return;
                for (uint32_t bi = 0; bi < x3::game::CrowdChatter::kMaxBubbles; ++bi) {
                    const x3::game::ChatterBubble& b = ch.bubbleSlot(bi);
                    if (b.agent == x3::game::kNoLink || b.agent >= cs.agentCount())
                        continue;
                    const auto& a = cs.agent(b.agent);
                    char buf[96];
                    std::snprintf(buf, sizeof(buf), " (%.1f,%.1f,%.1f)\"%s\"",
                                  a.pos.x, a.pos.y, a.pos.z, b.line ? b.line : "?");
                    spots += buf;
                }
            };
            for (int ci = 0; ci < 3; ++ci) {
                listSpots(canonChatter[ci], canonCrowds[ci]);
                listSpots(cityChatter[ci], cityCrowds[ci]);
            }
            x3::logInfo("[chatter] shot staging: " + std::to_string(bubblesNearCam()) +
                        "/" + std::to_string(hc.shotChatter) + " bubbles near the shot cam after " +
                        std::to_string(staged) + " warp frames (" +
                        std::to_string(staged / 60) + " s sim); live bubbles:" + spots);
        }
        for (int i = 0; i < kSettleFrames; ++i) {
            glfwPollEvents();
            // Sync the live cvars (incl. r_cullpath/r_hzb seeded by --cullpath/--hzb)
            // onto the device, exactly as the main loop does each frame.
            applyRtaoCVars(*console, *device);
            refreshLightBudgets();
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
            // WORLD CARS staging: drive (gentle throttle + a late lean into the
            // steer so the still catches a real driving pose), stepped exactly
            // like the live loop (setInput+preStep -> step -> postStep). Without
            // --shot-drive, still refresh the HUD hint from the camera position
            // (a --shot-cam parked next to a LOCKED car proves the hack prompt).
            if (canonWorld && worldCars.built()) {
                if (shotDriving) {
                    x3::phys::VehicleInput vin;
                    vin.throttle = (i > 20) ? 0.7f : 0.0f;
                    vin.steer    = (i > kSettleFrames / 2) ? 0.18f : 0.0f;
                    worldCars.driveInput(vin);
                    worldCars.preStep(dt);
                } else {
                    worldCars.interact(ssEye, false, false, false, dt, nullptr,
                                       *physics, nullptr);
                }
            }
            physics->step(dt);
            scene.update(*physics);
            if (shotDriving) worldCars.postStep(dt);
            // FX demo: with a SMALL settle (<=30) spawn a fresh muzzle + impact burst
            // on the last few frames so bright sparks/dust are alive at the captured
            // frame (the LIVE-burst shot). With a LARGE settle (>30) skip the sparks
            // entirely so only the PERSISTENT scorch decal on the surface remains
            // visible (the decal-on-surface shot). One flag, two honest captures.
            if (fxLightning) {
                // LIGHTNING proof shot. The --set shot_fire path fires along the LOOK
                // axis, so the bolt is end-on and collapses into a blob at the crosshair
                // (it proves the muzzle origin, not the bolt). Here the bolt runs ACROSS
                // the view — mostly VERTICAL, perpendicular to the near-horizontal look —
                // so the jagged forking zigzag actually reads. Capture-only.
                const float hl = std::sqrt(fxLook.x * fxLook.x + fxLook.z * fxLook.z);
                const x3::phys::Vec3 rightH = (hl > 1e-4f)
                    ? x3::phys::Vec3{ fxLook.z / hl, 0.0f, -fxLook.x / hl }
                    : x3::phys::Vec3{ 1.0f, 0.0f, 0.0f };
                // Strike ~1.6 m ahead so it sits in open air in FRONT of the near wall.
                // A path containing "impact" frames a SHORT bolt so the crackling arc
                // tendrils dominate; otherwise a TALL bolt so the full zigzag reads.
                // A path containing "long" runs a ~25 m bolt instead, to prove the fractal
                // is SCALE-INVARIANT: the midpoint displacement is a FRACTION OF LENGTH
                // and the depth is fixed, so a 25 m bolt must look as natural as a 2 m one
                // (the old fixed-metre step is precisely what made short bolts read as
                // coat hangers). Needs an open world — shoot it somewhere with depth.
                const bool  impactShot = (screenshotPath.find("impact") != std::string::npos);
                const bool  longShot   = (screenshotPath.find("long")   != std::string::npos);
                const float ahead    = longShot ? 14.0f : 1.6f;   // far enough that the bolt fits in frame
                const float boltTall = impactShot ? 0.5f : (longShot ? 12.0f : 1.9f);
                const x3::phys::Vec3 strike{ ssX + fxLook.x * ahead,
                                             ssY + fxLook.y * ahead - 0.35f,
                                             ssZ + fxLook.z * ahead };
                const x3::phys::Vec3 boltA{ strike.x - rightH.x * 0.35f, strike.y + boltTall,
                                            strike.z - rightH.z * 0.35f };
                const x3::phys::Vec3 boltB = strike;   // the bolt descends INTO the strike
                // Spawn the bolt ONCE a few frames out (re-spawning each frame would reset
                // its propagation age); it ages to full reach + a stable jagged shape by
                // the captured frame. Arc-tendril impact AT the strike point.
                // The bolt PROPAGATES at kLightningBoltSpeed (300 m/s), so a long bolt needs
                // more age to fully connect: spawn it earlier (still inside kTracerTime).
                if (i == kSettleFrames - (longShot ? 5 : 3))
                    combatFx.addTracer(boltA, boltB, x3::game::WeaponFxKind::Lightning);
                if (i == kSettleFrames - 2)
                    combatFx.spawnImpact(boltB, x3::phys::Vec3{ -fxLook.x, 0.5f, -fxLook.z },
                                         x3::game::WeaponFxKind::Lightning);
            }
            else if (fxDemo && kSettleFrames <= 30 && i >= kSettleFrames - 3) {
                combatFx.spawnMuzzleFlash(fxBurst, fxDir);
                // Sparks spray back toward the camera (normal = -look) so they read.
                combatFx.spawnImpact(fxBurst, x3::phys::Vec3{ -fxLook.x, -fxLook.y + 0.2f, -fxLook.z });
                // R-8 proof: the barrel EXPLOSION fireball, offset to the right of
                // the burst so both read, + a live tracer ribbon across the view.
                const x3::phys::Vec3 fxRight{ -fxLook.z, 0.0f, fxLook.x };
                combatFx.spawnExplosion(x3::phys::Vec3{ fxBurst.x + fxRight.x * 1.4f,
                                                        fxBurst.y + 0.2f,
                                                        fxBurst.z + fxRight.z * 1.4f }, 1.6f);
                combatFx.addTracer(x3::phys::Vec3{ fxBurst.x - fxRight.x * 1.2f - fxLook.x * 0.5f,
                                                   fxBurst.y - 0.3f,
                                                   fxBurst.z - fxRight.z * 1.2f - fxLook.z * 0.5f },
                                   x3::phys::Vec3{ fxBurst.x + fxLook.x * 4.0f,
                                                   fxBurst.y + 0.6f,
                                                   fxBurst.z + fxLook.z * 4.0f });
            }
            // ---- MUZZLE PROOF (--set shot_fire 1): fire the HELD weapon from its own
            // barrel tip every settle frame, so the still SHOWS the muzzle flash + the
            // tracer LEAVING THE GUN. Pair with `--set shot_weapon <name>` to photograph
            // each gun in turn — the eyeball gate for Tim's "the fire doesn't come from the
            // barrel". The origin is weaponMuzzle() — the SAME origin the live fire block
            // uses — so a passing still is a passing GAME, not a posed picture.
            if (console->getInt("shot_fire") != 0) {
                const x3::phys::Vec3 mz =
                    weaponMuzzle(arsenal, *console, ssX, ssY, ssZ, ssYaw, ssPitch);
                const x3::game::WeaponFxKind mk =
                    x3::game::fxKindFromId(arsenal.current().muzzleFx);
                const x3::phys::RayHit mh =
                    physics->rayCast(ssEye, fxLook, 30.0f, x3::phys::Layer::Static);
                const x3::phys::Vec3 hitP = mh.hit
                    ? mh.point
                    : x3::phys::Vec3{ ssX + fxLook.x * 12.0f, ssY + fxLook.y * 12.0f,
                                      ssZ + fxLook.z * 12.0f };
                combatFx.spawnMuzzleFlash(mz, fxLook, mk);
                combatFx.addTracer(mz, hitP, mk);
                // IMPACT at the hit point, keyed to the weapon's OWN impact preset — for
                // Lightning this is the crackling arc-tendril ring (8e9f7d5). NOTE: that
                // commit spawned the bolt from muzzleFromCamera(1.6, 0.20, 0.06) — the
                // hardcoded lightning muzzle CRUTCH that 346f5e7 deleted on purpose. The
                // origin here stays weaponMuzzle() (the measured per-weapon barrel tip);
                // only the impact call is taken from 8e9f7d5.
                combatFx.spawnImpact(hitP, x3::phys::Vec3{ -fxLook.x, -fxLook.y + 0.2f, -fxLook.z },
                                     x3::game::fxKindFromId(arsenal.current().impactFx));
                combatFx.update(dt);
            }
            if (fxDemo) combatFx.update(dt);
            // --world canonlevel SCREENSHOT lighting + cull: feed the player's visible
            // rooms' ceiling lights PLUS the opening-space dressing's motivated lights
            // (flickering tube / red alarm / cyan terminal), and set the visible-room
            // set so render() draws the cell + its doored neighbours. Without this the
            // capture path fed only the (empty) legacy fixtures — the cell read flat.
            if (canonWorld && canonFloor.valid()) {
                canonPlay.tick(dt, scene, *physics, ssEye, nullptr, x3::game::AttackFxFn{});
                canonAliens.update(dt, scene, *physics, ssEye);   // planet aliens settle into their Idle/Walk poses
                canonDressing.tick(dt);
                // X3_SHOT_SWIM=3p: hold the avatar in the swim state through every
                // settle frame, so the water pose settles and the REAL swim clip is
                // mid-cycle at the capture (kNoRoom = never room-culled).
                //
                // At X3_SHOT_SWIM_SPEED > 0 he actually SWIMS: the feet walk UPSTREAM
                // of the staged spot and arrive exactly ON it at the last settle frame
                // (so the camera framing computed for `swimSpot` is exact), while the
                // per-frame delta gives ThirdPersonView a real planar water speed —
                // which is what trips the stroke latch and plays "Swim" instead of
                // "SwimIdle". The X3_SHOT_SWIM_PHASE pre-roll (below, before the loop)
                // has already advanced the clip clock so the requested stroke phase
                // lands on THIS capture.
                if (swimShotOk && swimShotMode == "3p" && thirdPerson.built()) {
                    const int back = kSettleFrames - 1 - i;      // frames still to go
                    const float lead = swimShotSpeed * (float)back * dt;
                    const x3::phys::Vec3 swimFeet{
                        swimSpot.x - std::cos(swimYaw) * lead, swimWaterY - 1.45f,
                        swimSpot.z - std::sin(swimYaw) * lead };
                    thirdPerson.update(dt, scene, swimFeet, 1.6f, swimYaw, 0.0f,
                                       x3::game::kNoRoom, false, false,
                                       /*swimming*/true);
                    if (i % 30 == 0 || i == kSettleFrames - 1) {   // staging telemetry
                        float dxf[16]; thirdPerson.avatarDrawTransform(dxf);
                        float hs[16]; const bool gh = thirdPerson.handSocketWorld(hs);
                        x3::logInfo("[swimshot] frame " + std::to_string(i) +
                            " clip=" + (thirdPerson.swimStroking() ? "Swim" : "SwimIdle") +
                            " rootY=" + std::to_string(dxf[13]) +
                            " upY=" + std::to_string(dxf[5]) +
                            " hand=(" + (gh ? std::to_string(hs[12]) + "," +
                                         std::to_string(hs[13]) + "," +
                                         std::to_string(hs[14]) : "n/a") +
                            ") waterY=" + std::to_string(swimWaterY) +
                            " cam=(" + std::to_string(ssX) + "," + std::to_string(ssY) +
                            "," + std::to_string(ssZ) + ")");
                    }
                }
                // LIVING NPCs: tick the crowds through the settle so the still
                // captures them mid-life (conversations formed, crates riding,
                // the ball in play) — no PVS gate here; we want them settled.
                for (auto& cc : canonCrowds) if (cc.built()) cc.update(dt, scene);
                for (auto& cc : cityCrowds)  if (cc.built()) cc.update(dt, scene);
                // FISH + THE WATER ZAP through the settle (X3_SHOT_ZAP). The
                // schools swim; at its lead frame the staged zap fires through the
                // REAL path — findWaterEntry() from the REAL shot camera + look,
                // fireWaterZap() applying the REAL fish kill and the REAL
                // zapPlayer() half-max-health damage to a real Player (whose HP the
                // HUD then reads). Nothing here is painted on.
                if (worldFish.built()) {
                    worldFish.update(dt, scene, ssEye);
                    // GOD RAYS breathe/drift through the settle so the capture
                    // catches them mid-life, exactly like the live loop.
                    if (worldRays.built()) worldRays.update(dt, scene);
                    // STEADICAM on the subject (X3_SHOT_ZAP=pike|perch): hold the
                    // fish in PROFILE with the staged sun on its near flank. We
                    // stand on whichever beam of the fish faces the sun (the dot
                    // test below), at zapShotTrackDist — outside fleeRadius, so it
                    // cruises instead of bolting — and re-aim every frame, because
                    // the subject is a live fish, not a prop on a turntable.
                    if (zapShotTrack >= 0 &&
                        zapShotTrack < (int)worldFish.fishCount()) {
                        const x3::game::Fish& f = worldFish.fish((uint32_t)zapShotTrack);
                        // The fish's beam (perpendicular to its heading), in XZ.
                        const float px = -std::sin(f.yaw), pz = std::cos(f.yaw);
                        // Stand on the LIT side: the staged sun comes from +sunDirXZ
                        // (0.55, -0.35), so the flank facing that way is the lit one.
                        const float s = (px * 0.55f + pz * -0.35f) >= 0.0f ? 1.0f : -1.0f;
                        const float cx = f.x + s * px * zapShotTrackDist;
                        const float cz = f.z + s * pz * zapShotTrackDist;
                        const float cy = f.y + 0.10f;
                        // FRAME IT CLEAR OF THE GUN. Aiming dead-on put the pike
                        // under the crosshair — and the lightning gun's viewmodel
                        // owns the lower-right of the frame, so it ate the snout,
                        // which is the one feature that says PIKE. Bias the aim so
                        // the subject sits upper-LEFT of centre, in clean water.
                        // The bias is a FRACTION OF THE LENS, not a fixed angle: at
                        // a fixed 0.22 rad the long-lens perch shot threw its subject
                        // into the top-right corner behind the minimap.
                        const float fovRad = zapShotTrackFov * 0.01745329f;
                        const float yaw = std::atan2(f.z - cz, f.x - cx) + 0.30f * fovRad;
                        const float pitch = std::atan2(f.y - cy,
                            std::sqrt((f.x - cx) * (f.x - cx) + (f.z - cz) * (f.z - cz)))
                            - 0.12f * fovRad;
                        // Write it back into the SHOT camera: the capture path
                        // re-issues setCamera(ssX..) after this loop, so a purely
                        // local aim here would be clobbered and the still would
                        // frame the water where the pike USED to be.
                        ssX = cx; ssY = cy; ssZ = cz; ssYaw = yaw; ssPitch = pitch;
                        device->setCamera(cx, cy, cz, yaw, pitch, zapShotTrackFov);
                        if (i % 40 == 0) {
                            char tb[200];
                            std::snprintf(tb, sizeof(tb),
                                "[fishshot] frame %d track=%d species=%u fish=(%.2f,%.2f,%.2f) "
                                "yaw=%.2f cam=(%.2f,%.2f,%.2f) fov=%.0f",
                                i, zapShotTrack, (uint32_t)f.species, f.x, f.y, f.z,
                                f.yaw, cx, cy, cz, zapShotTrackFov);
                            x3::logInfo(tb);
                        }
                    }
                    if (worldSea.built())
                        worldSea.update(dt, scene, *device, *physics, ssEye, nullptr);
                    // RIDE ALONGSIDE him: re-solve the camera (and the key light that
                    // hangs off it) from where he actually IS this frame.
                    if (seaShotIdx >= 0 && worldSea.built()) {
                        x3::game::SeaCreature& sc2 =
                            worldSea.creatureMut((uint32_t)seaShotIdx);
                        if (seaShotAtSurface) sc2.wantDepth = 0.62f;   // hold the fin up
                        const float swY = x3::game::worldWaterLevelAt(sc2.x, sc2.z);
                        const float sfy = (swY > x3::game::kFishDryTest) ? swY : 0.0f;
                        const float fx = -std::sin(sc2.yaw), fz = -std::cos(sc2.yaw);
                        const float rx = -fz, rz = fx;
                        ssX = sc2.x - fx * seaShotBack + rx * seaShotSide;
                        ssZ = sc2.z - fz * seaShotBack + rz * seaShotSide;
                        ssY = (seaShotAtSurface ? sfy : sc2.y) + seaShotUp;
                        const float ty = seaShotAtSurface ? (sfy + 0.15f) : sc2.y;
                        const float hx = sc2.x - ssX, hz = sc2.z - ssZ;
                        ssYaw = std::atan2(hz, hx);
                        ssPitch = std::atan2(ty - ssY, std::sqrt(hx * hx + hz * hz));
                        device->setCamera(ssX, ssY, ssZ, ssYaw, ssPitch, ssFov);
                        if (seaShotLights.size() >= 2) {
                            seaShotLights[0].pos[0] = ssX;
                            seaShotLights[0].pos[1] = ssY + 1.2f;
                            seaShotLights[0].pos[2] = ssZ;
                            seaShotLights[1].pos[0] = sc2.x + 6.0f;
                            seaShotLights[1].pos[1] = sc2.y + 9.0f;
                            seaShotLights[1].pos[2] = sc2.z + 5.0f;
                        }
                        seaShotCenter = x3::phys::Vec3{ sc2.x, sfy, sc2.z };
                    }
                    if (!seaShotLights.empty())
                        device->setPointLights(seaShotLights.data(),
                                               (uint32_t)seaShotLights.size());
                    // X3_SHOT_SEALIFE=zap: the water goes live through the REAL path,
                    // early enough that the shark is belly-up by the capture frame.
                    if (seaShotZapLead >= 0 && !seaShotZapFired &&
                        i >= kSettleFrames - seaShotZapLead) {
                        x3::game::WaterZapEntry we{};
                        we.hit = true;
                        we.x = seaShotCenter.x; we.z = seaShotCenter.z;
                        we.surfaceY = seaShotCenter.y; we.y = seaShotCenter.y;
                        fireWaterZap(we, nullptr, nullptr);
                        seaShotZapFired = true;
                    }
                    if (!zapShotMode.empty() && i % 40 == 0 && worldFish.fishCount() > 0) {
                        const x3::game::Fish& f0 = worldFish.fish(0);
                        const x3::game::FishSchool& s0 = worldFish.school(0);
                        char fb[240];
                        std::snprintf(fb, sizeof(fb),
                            "[zapshot] frame %d school0=(%.1f,%.1f) active=%d fish0=(%.2f,%.2f,%.2f) "
                            "vis=%d roomVis=%d room=%u active=%u alive=%u dead=%u cam=(%.1f,%.1f,%.1f)",
                            i, s0.cx, s0.cz, (int)s0.active, f0.x, f0.y, f0.z,
                            (int)scene.get(f0.entHead).visible,
                            (int)scene.roomVisible(scene.get(f0.entHead).roomId),
                            scene.get(f0.entHead).roomId, worldFish.activeCount(),
                            worldFish.aliveCount(), worldFish.deadCount(), ssX, ssY, ssZ);
                        x3::logInfo(fb);
                    }
                }
                if (!zapShotMode.empty() && zapShotLead >= 0 && !zapShotFired &&
                    i >= kSettleFrames - zapShotLead) {
                    const x3::phys::Vec3 zdir{ std::cos(ssPitch) * std::cos(ssYaw),
                                               std::sin(ssPitch),
                                               std::cos(ssPitch) * std::sin(ssYaw) };
                    x3::game::WaterZapEntry we = x3::game::findWaterEntry(
                        ssEye, zdir, 40.0f, waterQueryFn);
                    if (!we.hit) {   // aimed off the water: zap the school anyway
                        we.hit = true;
                        we.x = zapShotCenter.x; we.z = zapShotCenter.z;
                        we.surfaceY = zapShotCenter.y; we.y = zapShotCenter.y;
                    }
                    fireWaterZap(we, &zapShotPlayer, &zapShotFeet);
                    zapShotHp = zapShotPlayer.hp();
                    zapShotFired = true;
                }
                if (!zapShotMode.empty()) {
                    waterZapper.tick(dt);
                    tickZapFx(dt);
                    if (!fxDemo && console->getInt("shot_fire") == 0)
                        combatFx.update(dt);   // (the fx-demo paths already tick it)
                }
                // CROWD CHATTER through the settle (real audio hookup so a
                // capture run also proves the murmur path in its log).
                for (int ci = 0; ci < 3; ++ci) {
                    if (canonCrowds[ci].built())
                        canonChatter[ci].update(dt, canonCrowds[ci], audio.get(),
                                                chatterSnd, ssEye);
                    if (cityCrowds[ci].built())
                        cityChatter[ci].update(dt, cityCrowds[ci], audio.get(),
                                               chatterSnd, ssEye);
                }
                // SKINNED CITIZENS: pose-follow + DRAIN the spawn pools fully
                // (bounded) — a short settle must still capture real people,
                // not a half-swapped blockout crowd. Not the interactive path.
                for (int ci = 0; ci < 3; ++ci) {
                    for (int b = 0; b < 64; ++b) {
                        if (canonCrowds[ci].built())
                            canonCrowdSkins[ci].update(dt, canonCrowds[ci], scene,
                                                       *device, *physics);
                        if (cityCrowds[ci].built())
                            cityCrowdSkins[ci].update(dt, cityCrowds[ci], scene,
                                                      *device, *physics);
                        if (canonCrowdSkins[ci].pendingCount() == 0 &&
                            cityCrowdSkins[ci].pendingCount() == 0) break;
                    }
                }
                // SEAM 3: keep the planet streaming under the shot camera — an
                // outdoor --shot-cam far from the tower needs its terrain tiles
                // (the ring re-centers on the camera) and any nearby regions
                // resident before the capture frame. Generous settle budget.
                if (canonStreamOn && ssEye.y > kStreamSuppressBelowY) {
                    const double st0 = glfwGetTime();
                    terrainStreamer.update(scene, *device, *physics, ssEye.x, ssEye.z);
                    const double terrainMs = (glfwGetTime() - st0) * 1000.0;
                    canonWstream.update(scene, *device, *physics,
                                        ssEye.x, ssEye.y, ssEye.z, 0.0f, 0.0f, 0.0f,
                                        /*budget*/ 50.0, terrainMs);
                }
                x3::game::Frustum fr = x3::game::Frustum::build(
                    ssEye.x, ssEye.y, ssEye.z, ssYaw, ssPitch, ssFov, 16.0f / 9.0f);
                canonFloor.floodVisibleRoomsAt(ssEye.x, ssEye.y, ssEye.z, fr, &canonDoors,
                                               6, kCanonRoomBudget, canonVisRooms);
                // SEAM 2: an OUTDOOR shot camera (on the apron) is in no room — the
                // flood already seeded from the nearest room so the world never
                // blanks; also force the breach room in so the interior read
                // through the open breach is stable from any outdoor vantage.
                facilityExterior.ensureOutdoorVis(canonFloor, ssEye.x, ssEye.y, ssEye.z,
                                                  canonVisRooms);
                // SEAM 3 (risk 2): same outdoor draw gate as the live loop — the
                // streamed planet is in-frame only for outdoor/breach vantages.
                if (canonStreamOn) {
                    const uint32_t eyeRoom = canonFloor.roomAt(ssEye.x, ssEye.y, ssEye.z);
                    // Same open-air test as the live loop (W10): river/sea
                    // vantages sit below -2 but above (local terrain - 15).
                    const bool ssOpenAir = ssEye.y > -2.0f ||
                        ssEye.y > x3::game::terrainHeightAtWorld(ssEye.x, ssEye.z) - 15.0f;
                    if ((eyeRoom == x3::game::kNoRoom && ssOpenAir) ||
                        (eyeRoom != x3::game::kNoRoom &&
                         eyeRoom == facilityExterior.breachRoomHint()))
                        canonVisRooms.push_back(x3::game::kStreamedExteriorRoom);
                }
                if (riftInZone(ssEye.x, ssEye.y, ssEye.z))
                    canonVisRooms.push_back(x3::game::kRiftRoom);   // W-RIFT capture vantage
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
                                                   ssEye.x, ssEye.y, ssEye.z, cl,
                                                   canonLightBudget);
                if (facilityExterior.built()) cl.push_back(facilityExterior.spillLight());
                // --screenshot-alert (canon): the LEVEL-3 LOCKDOWN red shift over
                // the facility feed — the SAME multiplier + eye-inside-the-tower
                // scope the live loop applies (see applyAlertRedShift there).
                if (alertShot && facilityAlert.redShift() > 0.0f &&
                    canonFloor.roomAt(ssEye.x, ssEye.y, ssEye.z) != x3::game::kNoRoom)
                    applyAlertRedShift(cl, facilityAlert.redShift());
                // STREET LIGHT: same outdoor gate + nearest-K feed as the live
                // loop, so lamp pools light the shot; flicker ticks through the
                // settle (a flickering lamp can be captured mid-burst).
                if (streetLights.lampCount() > 0) {
                    bool ssExtVis = false;
                    for (uint32_t v : canonVisRooms)
                        if (v == x3::game::kStreamedExteriorRoom) { ssExtVis = true; break; }
                    if (ssExtVis) {
                        streetLights.update(dt, scene);
                        streetLights.selectLights(ssEye.x, ssEye.y, ssEye.z, cl, streetLampK);
                    }
                }
                // W-RIFT: a capture vantage inside sub-level R1 gets the hub's own rig
                // (gate cores + keys + the hall) and the approach's failing strips —
                // the same takeover the live loop does, or every rift shot is black.
                if (riftLore.built()) riftLore.update(dt);   // bake the log for the still
                if (stairLore.built()) stairLore.update(dt); // clue 1 bake (capture path)
                if (liftLore.built())  liftLore.update(dt);  // clue 2 bake (capture path)
                if (riftBuilt && riftInZone(ssEye.x, ssEye.y, ssEye.z)) {
                    rifthub.tick(dt, scene);      // the membranes must be ALIVE in a still
                    riftDepths.tick(dt);
                    riftLights(ssEye.x, ssEye.y, ssEye.z, cl);
                }
                // X3_SHOT_SWIM=3p: LIGHT THE STAGE, NOT JUST THE MAN. Jake's kit is
                // near-black cloth — measured: even at sunLight 12 / ambient 0.95 he
                // still photographs as a dark shape, because that IS his albedo. So
                // the capture rig lights the WATER he lies on: a submerged practical
                // right under him turns the surface into a bright plate, and the
                // stroke reads as a crisp swimmer's silhouette on it (arms wide on the
                // catch, the frog kick opening behind) instead of black-on-black. A
                // warm key from the CAMERA side still models his back + shoulders.
                // FRONT of the list so the 64-light cap can never truncate them.
                // Capture path only (behind X3_SHOT_SWIM) — the live game is untouched.
                if (swimShotOk && swimShotMode == "3p") {
                    x3::rhi::PointLight plate{};      // the bright water under him
                    plate.pos[0] = swimSpot.x;
                    plate.pos[1] = swimWaterY - 0.45f;
                    plate.pos[2] = swimSpot.z;
                    plate.range  = 22.0f;
                    plate.color[0] = 5.0f; plate.color[1] = 6.2f; plate.color[2] = 7.0f;
                    x3::rhi::PointLight key{};        // models the body from the camera side
                    key.pos[0] = (ssX + swimSpot.x) * 0.5f;
                    key.pos[1] = swimWaterY + 2.6f;
                    key.pos[2] = (ssZ + swimSpot.z) * 0.5f;
                    key.range  = 16.0f;
                    key.color[0] = 4.2f; key.color[1] = 4.0f; key.color[2] = 3.6f;
                    cl.insert(cl.begin(), key);
                    cl.insert(cl.begin(), plate);
                }
                // UNDERWATER DIVER'S LIGHT in the capture — mirrors the live-loop
                // diver's lamp (see the fl merge) so an underwater --shot-cam shows
                // exactly what the player sees while submerged. Gated on the shot
                // eye being below a water surface.
                {
                    const float sWY = x3::game::worldWaterLevelAt(ssEye.x, ssEye.z);
                    if (sWY > -1.0e30f && ssEye.y < sWY - 0.05f) {
                        const float cp = std::cos(ssPitch);
                        const float fx = cp * std::cos(ssYaw);
                        const float fy = std::sin(ssPitch);
                        const float fz = cp * std::sin(ssYaw);
                        x3::rhi::PointLight fill{};
                        fill.pos[0] = ssEye.x + fx * 2.0f;
                        fill.pos[1] = ssEye.y + fy * 2.0f;
                        fill.pos[2] = ssEye.z + fz * 2.0f;
                        fill.range = 18.0f;
                        fill.color[0] = 1.20f; fill.color[1] = 1.70f; fill.color[2] = 2.05f;
                        x3::rhi::PointLight beam{};
                        beam.pos[0] = ssEye.x + fx * 8.0f;
                        beam.pos[1] = ssEye.y + fy * 8.0f;
                        beam.pos[2] = ssEye.z + fz * 8.0f;
                        beam.range = 24.0f;
                        beam.color[0] = 1.60f; beam.color[1] = 2.25f; beam.color[2] = 2.80f;
                        cl.insert(cl.begin(), fill);
                        cl.insert(cl.begin(), beam);
                    }
                }
                if (cl.size() > lightBudget) cl.resize(lightBudget);
                device->setPointLights(cl.data(), (uint32_t)cl.size());
            }
            // R11: the ELEVATOR CAB in a CAPTURE. The live loop feeds the cab's ceiling
            // practical + disco spots (see the elevator block there), but the screenshot
            // settle loop never did — so every headless shot of the car interior came
            // back BLACK, and the cab's own atmosphere (which keys off the rider) never
            // engaged either. A room you cannot photograph is a room you cannot art-
            // direct, which is exactly how the cab stayed a graybox for this long.
            if (elevator.built() && !elevator.pointLights().empty() && !canonWorld) {
                // B4, ONE MORE TIME — this feed was still FIRST-IN-BUILD-ORDER. e06ee05 fed
                // the live loop and the plain capture path nearest-to-eye, but MISSED this
                // one: `= game.lightFixtures()` copies all ~1.1k tower fixtures in ROOM
                // ORDER, and the resize(64) below keeps whichever rooms happened to be
                // authored first. So every elevator capture photographed the cab correctly
                // (its own lights are pushed to the FRONT) against a SHAFT AND LOBBY lit by
                // fixtures from unrelated floors — i.e. by nothing. Same disease, same fix.
                std::vector<x3::rhi::PointLight> el =
                    nearestFixtures(game.lightFixtures(), ssX, ssY, ssZ, fixtureBudget);
                // FRONT of the list: level1 ships more than 64 fixtures, so appending
                // would put the cab's own practical past the cap and truncate it away —
                // the exact reason the first capture of the new cab came back black.
                for (const auto& pl : elevator.pointLights())
                    if (pl.color[0] + pl.color[1] + pl.color[2] > 0.001f)
                        el.insert(el.begin(), pl);
                if (el.size() > lightBudget) el.resize(lightBudget);
                device->setPointLights(el.data(), (uint32_t)el.size());
            }
            // X3_SHOT_SEALIFE: the capture lights must be issued HERE — this is the
            // render path, and the room/elevator set above would otherwise clobber
            // anything staged earlier (which is exactly why the first lit attempt
            // came back as the same black cut-out).
            if (!seaShotLights.empty())
                device->setPointLights(seaShotLights.data(),
                                       (uint32_t)seaShotLights.size());
            if (elevator.built()) {
                // Camera-as-rider: the shot camera standing in the cab gets the cab's air.
                elevator.applyCabAtmosphere(
                    *device, x3::phys::Vec3{ ssEye.x, ssEye.y - 1.7f, ssEye.z });
                // ...and the cab must TICK in a capture, or its OLEDs never bake and the
                // floor directory (which is where the RIFT stop appears) photographs blank.
                elevator.update(dt, scene, *physics);
            }
            // Canon doors must tick too, or a door staged open for a capture
            // (X3_STAIR_DEMO=3: the 7762 master door) photographs shut.
            if (canonWorld && canonFloor.valid())
                canonDoors.update(dt, scene, *physics);
            // WORLD CARS staging: the capture camera FOLLOWS the driven car
            // (the drive host's chase framing around the shot-cam look angles).
            if (shotDriving) {
                float dcx, dcy, dcz;
                worldCars.driverCamera(ssYaw, ssPitch, dcx, dcy, dcz);
                device->setCamera(dcx, dcy, dcz, ssYaw, ssPitch, ssFov);
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
                    canonBarrels.render(frame);   // WAVE (cell-door): explodable barrels + debris
                    canonRooms.draw(*device, frame, canonVisRooms);
                    canonRooms.applyZoneAtmosphere(*device,
                        canonFloor.roomAt(ssEye.x, ssEye.y, ssEye.z));
                    // W10 SWIMMING: underwater shot cameras get the same dense
                    // blue-green fog override the live loop applies (see there).
                    { const float ssWY = x3::game::worldWaterLevelAt(ssEye.x, ssEye.z);
                      if (ssWY > -1.0e30f && ssEye.y < ssWY - 0.05f) {
                          x3::rhi::IRenderDevice::FogParams uf;
                          uf.enabled  = true;
                          uf.color[0] = 0.020f; uf.color[1] = 0.095f; uf.color[2] = 0.110f;
                          uf.density  = 0.055f; uf.start = 0.15f; uf.maxOpacity = 0.94f;
                          device->setFog(uf);
                      }
                      // UNDERWATER CAUSTICS in the capture — mirrors the live
                      // loop (enabled over any water column; the shader gates
                      // per-fragment). Clock = settle-frame time, plus the
                      // X3_CAUSTICS_TSHIFT staging offset so two otherwise-
                      // identical captures can prove the pattern MOVES.
                      {
                          static float s_shotCausticShift = [] {
                              const char* e = std::getenv("X3_CAUSTICS_TSHIFT");
                              return e ? (float)std::atof(e) : 0.0f;
                          }();
                          x3::rhi::IRenderDevice::CausticsParams cw;
                          cw.enabled   = ssWY > -1.0e30f;
                          cw.waterY    = cw.enabled ? ssWY : 0.0f;
                          cw.time      = (float)i * dt + s_shotCausticShift;
                          cw.intensity = 1.0f;
                          device->setCaustics(cw);
                      } }
                    // X3_SHOT_SWIM=3p: LIFT THE AIR for the capture (applied after
                    // applyZoneAtmosphere, same as the underwater override). Jake's
                    // kit is near-black cloth and the zone recipe's ambient is an
                    // interior-grade floor, so the swimmer photographs as a shape
                    // with no surface — the exact "unreadable silhouette" the v1
                    // proof shipped. Raising the ambient + IBL puts skylight back
                    // on his back and arms. Capture path only.
                    if (swimShotOk && swimShotMode == "3p") {
                        device->setAmbient(0.40f, 0.43f, 0.48f);
                        device->setIblIntensity(1.5f);
                    }
                    // W-RIFT: sub-level R1's air wins over the room recipes for a
                    // vantage down there (applied AFTER applyZoneAtmosphere, like the
                    // underwater override above).
                    if (riftBuilt && riftInZone(ssEye.x, ssEye.y, ssEye.z))
                        rifthub.applyAtmosphere(*device);
                    canonDoors.drawMeshes(*device, frame);
                    facilityExterior.draw(*device, frame);   // SEAM 2: facade skin (panes/bands/apron/sign)
                    // WORLD CARS: parked cars + the (staged) live car — the same
                    // outdoor PVS gate as the live loop.
                    if (worldCars.built() &&
                        scene.roomVisible(x3::game::kStreamedExteriorRoom))
                        worldCars.draw(frame);
                    if (canonPlay.built()) canonPlay.draw(*device, frame, scene);
                    canonAliens.drawAll(*device, frame, scene);   // CANON ALIENS in captures
                    // ---- HEALTHBAR CAPTURE PROOF (gate: screenshot filename contains
                    // "healthbar"). Renders the SAME room-gated + LOS-culled enemy bar
                    // as the live loop's barsFor (app_run.cpp interactive path) so a
                    // still deterministically shows: near/same-room monster -> bar drawn;
                    // adjacent-room monster (through a doorway) -> bar SUPPRESSED. Also
                    // logs each enemy's room + decision so the proof is textual too.
                    // Capture-only: zero effect on gameplay or any normal screenshot.
                    if (canonPlay.built() &&
                        screenshotPath.find("healthbar") != std::string::npos) {
                        const x3::phys::Vec3 hbEye = ssEye;
                        const uint32_t hbPlayerRoom =
                            canonFloor.valid() ? canonFloor.roomAt(hbEye.x, hbEye.y, hbEye.z)
                                               : x3::game::kNoRoom;
                        const bool hbRoomGate =
                            canonFloor.valid() && hbPlayerRoom != x3::game::kNoRoom;
                        // Two capture modes (by filename token):
                        //   "...ROOM..." -> ISOLATE the new room gate: draw bars for
                        //     same-room, on-screen enemies REGARDLESS of LOS (so same-room
                        //     props/bars don't hide the demonstrative bar). LOS is still
                        //     computed + logged. This proves adjacent-room enemies get NO
                        //     bar while same-room enemies do.
                        //   otherwise -> REAL combined behaviour (room + LOS + on-screen),
                        //     identical to the live loop's barsFor.
                        const bool losIsolate =
                            screenshotPath.find("ROOM") != std::string::npos;
                        x3::logInfo(std::string("[healthbar-proof] player room=") +
                            (hbPlayerRoom == x3::game::kNoRoom ? std::string("NONE")
                                                              : std::to_string(hbPlayerRoom)) +
                            " roomGate=" + (hbRoomGate ? "ON" : "off"));
                        auto proofBars = [&](x3::game::MonsterManager& mm) {
                            for (uint32_t mi = 0; mi < mm.count(); ++mi) {
                                x3::game::MonsterSystem& m = mm.at(mi);
                                if (!m.alive()) continue;
                                x3::phys::Vec3 c = m.pos();
                                const uint32_t mRoom = canonFloor.valid()
                                    ? canonFloor.roomAt(c.x, c.y + 1.0f, c.z) : x3::game::kNoRoom;
                                const char* verdict = "DRAWN";
                                // Room gate (mirrors barsFor).
                                if (hbRoomGate && mRoom != x3::game::kNoRoom &&
                                    mRoom != hbPlayerRoom) verdict = "culled:ROOM";
                                // LOS gate (mirrors barsFor).
                                const x3::phys::Vec3 chest{ c.x, c.y + 1.0f, c.z };
                                const x3::phys::Vec3 d{ chest.x - hbEye.x, chest.y - hbEye.y, chest.z - hbEye.z };
                                const float dist = std::sqrt(d.x*d.x + d.y*d.y + d.z*d.z);
                                if (verdict[0] == 'D' && dist > 0.001f) {
                                    const x3::phys::Vec3 nd{ d.x/dist, d.y/dist, d.z/dist };
                                    const x3::phys::RayHit los = physics->rayCastStrict(
                                        hbEye, nd, dist - 0.3f, x3::phys::Layer::Static);
                                    if (los.hit) verdict = "culled:LOS";
                                }
                                x3::phys::Vec3 head{ c.x, c.y + 2.2f, c.z };
                                float sx = 0.0f, sy = 0.0f;
                                const bool onScreen =
                                    device->worldToScreen(head.x, head.y, head.z, sx, sy);
                                if (verdict[0] == 'D' && !onScreen) verdict = "culled:OFFSCREEN";
                                // Capture draw decision: in ROOM-isolate mode, ignore LOS
                                // culling (draw same-room + on-screen); else use the real
                                // combined verdict.
                                const bool captureDraw = losIsolate
                                    ? (!(hbRoomGate && mRoom != x3::game::kNoRoom &&
                                         mRoom != hbPlayerRoom) && onScreen)
                                    : (std::string(verdict) == "DRAWN");
                                x3::logInfo(std::string("[healthbar-proof]   enemy room=") +
                                    (mRoom == x3::game::kNoRoom ? std::string("NONE")
                                                               : std::to_string(mRoom)) +
                                    " pos=(" + std::to_string(c.x) + "," + std::to_string(c.y) +
                                    "," + std::to_string(c.z) + ")" +
                                    " screen=(" + std::to_string(sx) + "," + std::to_string(sy) + ")" +
                                    (onScreen ? "" : "[off]") +
                                    " dist=" + std::to_string(dist) + "m -> " + verdict);
                                if (!captureDraw) continue;
                                // Draw the thin bar (same geometry as the live bar).
                                const int hpv = m.hp(), mx = m.maxHp();
                                if (mx <= 0 || hpv <= 0) continue;
                                const float frac = (hpv >= mx) ? 1.0f : (float)hpv / (float)mx;
                                uint32_t hw = 0, hh = 0; device->hudSize(hw, hh);
                                const float bw = 40.0f, bh = 3.0f, x0 = sx - bw * 0.5f;
                                float y0 = sy; if (y0 < 14.0f) y0 = 14.0f;
                                if (hh > 30 && y0 > (float)hh - 30.0f) y0 = (float)hh - 30.0f;
                                // Warm HP ramp + dim, matching the live hpBar (yellow full ->
                                // orange mid -> red low, no green; kBarDim 0.60 overall scale).
                                const float kBarDim = 0.60f;
                                const float hpY[3] = { 1.00f, 0.85f, 0.20f };
                                const float hpO[3] = { 1.00f, 0.50f, 0.10f };
                                const float hpR[3] = { 0.90f, 0.15f, 0.10f };
                                float rmp[3];
                                if (frac >= 0.5f) { const float t = (frac - 0.5f) * 2.0f;
                                    for (int k = 0; k < 3; ++k) rmp[k] = hpO[k] + t * (hpY[k] - hpO[k]); }
                                else              { const float t = frac * 2.0f;
                                    for (int k = 0; k < 3; ++k) rmp[k] = hpR[k] + t * (hpO[k] - hpR[k]); }
                                const float outl[4]   = { 0.0f, 0.0f, 0.0f, 0.55f };
                                const float frameC[4] = { 0.34f, 0.28f, 0.16f, 0.62f };
                                const float backC[4]  = { 0.03f, 0.03f, 0.05f, 0.55f };
                                const float baseC[4]  = { rmp[0]*kBarDim, rmp[1]*kBarDim, rmp[2]*kBarDim, 0.65f };
                                device->drawHudQuad(frame, x0 - 2.0f, y0 - 2.0f, bw + 4.0f, bh + 4.0f, outl);
                                device->drawHudQuad(frame, x0 - 1.5f, y0 - 1.5f, bw + 3.0f, bh + 3.0f, frameC);
                                device->drawHudQuad(frame, x0, y0, bw, bh, backC);
                                device->drawHudQuad(frame, x0, y0, bw * frac, bh, baseC);
                            }
                        };
                        canonPlay.forEachHostileManager(proofBars);
                    }
                    // X3_SHOT_SWIM=3p: the prone swimmer into the frame.
                    if (swimShotOk && swimShotMode == "3p")
                        thirdPerson.drawAvatar(*device, frame, scene);
                if (canon45.built()) canon45.draw(*device, frame, scene);
                    // SKINNED CITIZENS (room-gated inside draw()) — the crowds
                    // as real people in the still captures.
                    for (int ci = 0; ci < 3; ++ci) {
                        canonCrowdSkins[ci].draw(*device, frame, scene);
                        cityCrowdSkins[ci].draw(*device, frame, scene);
                    }
                    // THE OCEAN LIVES (PVS-gated inside draw()) - ONCE per frame, not per crowd.
                    // X3_SHOT_SEALIFE: stamp the capture lighting RIGHT HERE, immediately
                    // before the animal is drawn. Anything staged earlier gets clobbered by
                    // the world's own per-frame sky/light re-issue (which is why the first two
                    // lit attempts came back as the identical black cut-out).
                    if (!seaShotLights.empty()) {
                        device->setSkyParams(seaShotSky);
                        device->setPointLights(seaShotLights.data(),
                                               (uint32_t)seaShotLights.size());
                    }
                    worldSea.draw(*device, frame, scene);
                    // W-RIFT: the membrane FX (arcs + motes + light shafts) in the still.
                    if (riftBuilt && riftInZone(ssEye.x, ssEye.y, ssEye.z))
                        rifthub.drawFx(*device, frame);
                }
                // W2-A2 (W2-C's queued hook): the --screenshot path NEVER drew the FP
                // viewmodel — the "pistol" in every prior cell shot was the hovering
                // pickup prop, which is why the floating-gun/no-arms problem was
                // invisible in stills. Mirror the smoketest draw at the shot camera.
                // X3_SHOT_SEALIFE: no viewmodel. The subject is the ANIMAL; a pistol
                // hovering over half the frame photographs nothing.
                if (seaShotMode.empty() && arsenal.viewmodelsLoaded() &&
                    !(swimShotOk && swimShotMode == "3p")) {   // 3P hides the FP gun
                    VmPose vmPose = readViewmodelPose(*console);
                    // X3_SHOT_SWIM=fp: the swim viewmodel LOWER at full blend —
                    // the SAME offset curve the live loop applies while swimming
                    // (proof: the pistol rides below the water line, not through it).
                    if (swimShotOk && swimShotMode == "fp")
                        applySwimViewmodelLower(vmPose, 1.0f);
                    arsenal.drawCurrentViewmodel(*device, frame, ssX, ssY, ssZ, ssYaw, ssPitch,
                        vmPose.yawRad   - x3::game::kVmDefYawDeg   * kDegToRad,
                        vmPose.pitchRad - x3::game::kVmDefPitchDeg * kDegToRad,
                        vmPose.rollRad  - x3::game::kVmDefRollDeg  * kDegToRad,
                        vmPose.fwd   - x3::game::kVmDefFwd,
                        vmPose.right - x3::game::kVmDefRight,
                        vmPose.down  - x3::game::kVmDefDown);
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
                // --fx-demo OR the muzzle proof (--set shot_fire 1) needs the combat FX
                // actually DRAWN into the still — otherwise the "flash at the barrel" gate
                // photographs nothing at all.
                // (X3_SHOT_ZAP needs the same: the money shot IS the FX — the arcs
                // spidering across the water.)
                if (fxDemo || console->getInt("shot_fire") != 0 || !zapShotMode.empty()) {
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
                // HP: 100 normally; the X3_SHOT_ZAP=punish staging feeds the REAL
                // post-zap HP of the staged swimmer (half of max — the honest read).
                shm.hp = zapShotHp; shm.maxHp = x3::game::kPlayerMaxHp; shm.alive = zapShotHp > 0;
                shm.showCrosshair = true;
                shm.objective = game.objectives().currentLabel().c_str();
                const x3::game::WeaponDef&            shotWd = arsenal.current();
                const x3::game::Arsenal::WeaponState& shotWs = arsenal.currentState();
                shm.weapon = shotWd.name.c_str();
                shm.ammoInMag = shotWs.ammoInMag; shm.ammoReserve = shotWs.reserve;
                // CHARGE weapon (Lightning): show the CHARGE readout, same as the live
                // loop. Without this the capture printed the weapon's mag/reserve — which
                // for a charge weapon are INERT — so every lightning still LIED about the
                // ammo model ("200 / 600" for a gun that has neither).
                if (shotWd.usesCharge) {
                    shm.isCharge  = true;
                    shm.chargeCur = (int)(shotWs.charge + 0.5f);
                    shm.chargeCap = (int)(shotWd.chargeCap + 0.5f);
                    shm.chargeRegen     = arsenal.chargeRegenerating();
                    shm.chargeRegenSlow = arsenal.chargeRegenSlow();
                    shm.chargeSlowAbove = (int)(shotWd.chargeRegenSlowAbove + 0.5f);
                }
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
                // X3_STAIR_DEMO: the phantom keypad's 4545 response line, drawn
                // exactly where the live loop's bark sits (center, 62% down).
                if (!stairDemoBark.empty()) {
                    const float sdPx = 22.0f;
                    const float sdW  = device->textAdvance(x3::rhi::FontRole::Menu,
                                                           stairDemoBark.c_str(), sdPx);
                    const float sdX  = 640.0f - sdW * 0.5f;
                    const float sdY  = 720.0f * 0.62f;
                    const float sdSh[4]  = { 0.0f, 0.0f, 0.0f, 0.7f };
                    const float sdCol[4] = { 1.34f, 0.80f, 0.22f, 1.0f };   // amber
                    device->drawHudTextF(frame, x3::rhi::FontRole::Menu,
                                         stairDemoBark.c_str(), sdX + 1.5f, sdY + 1.5f, sdPx, sdSh);
                    device->drawHudTextF(frame, x3::rhi::FontRole::Menu,
                                         stairDemoBark.c_str(), sdX, sdY, sdPx, sdCol);
                }
                // W-MENU (X3_WORLD_MENU=1): the world/place directory over the still,
                // fed by the SAME reachability resolver the live menu uses — so what the
                // photograph says about each place is what the game says.
                if (shotWorldMenu) {
                    shotMenu.open();   // it closes itself on a pick; we never pick
                    shotMenu.draw(shotUi, dt, [&](const x3::game::Destination& d) {
                        x3::game::DestStatus st;
                        x3::phys::Vec3 anchor{};
                        std::string    why;
                        if (riftDestination(d.key, anchor, &why)) {
                            st.reach = x3::game::DestReach::Teleport;
                        } else if (d.worldFlag[0] && d.worldFlag != worldMode) {
                            st.reach = x3::game::DestReach::WorldLoad; st.reason = why;
                        } else {
                            st.reach = x3::game::DestReach::Unavailable; st.reason = why;
                        }
                        return st;
                    });
                }
                shotUi.end();
                // WORLD CARS hint line for the proof shots — the SAME bottom-
                // center treatment as the live loop ("[E] Exit" while driving,
                // "LOCKED - [hold E] hack" from a shot-cam beside a locked car).
                if (canonWorld && worldCars.built() && !worldCars.prompt().empty()) {
                    const std::string& vhint = worldCars.prompt();
                    uint32_t hw = 0, hh = 0; device->hudSize(hw, hh);
                    const float hsz = 18.0f;
                    const float adv = device->textAdvance(x3::rhi::FontRole::Menu,
                                                          vhint.c_str(), hsz);
                    const float hx = ((hw > 0) ? hw * 0.5f : 640.0f) - adv * 0.5f;
                    const float hy = (hh > 0) ? hh * 0.84f : 500.0f;
                    const bool hacking = vhint.rfind("HACKING", 0) == 0 ||
                                         vhint.rfind("LOCKED", 0) == 0;
                    const float vcol[4]    = { 1.0f, hacking ? 0.55f : 0.82f,
                                               hacking ? 0.35f : 0.45f, 0.9f };
                    const float vshadow[4] = { 0.0f, 0.0f, 0.0f, 0.70f };
                    device->drawHudTextF(frame, x3::rhi::FontRole::Menu, vhint.c_str(),
                                         hx + 1.5f, hy + 1.5f, hsz, vshadow);
                    device->drawHudTextF(frame, x3::rhi::FontRole::Menu, vhint.c_str(),
                                         hx, hy, hsz, vcol);
                }
                // CROWD CHATTER bubbles over the vantage — same rules as the
                // live loop (range/LOS/PVS/cap), so a --shot-chatter capture is
                // the honest in-game read.
                if (canonWorld) {
                    x3::game::ChatterDrawSite chSites[6];
                    uint32_t nCh = 0;
                    for (int ci = 0; ci < 3; ++ci) {
                        if (canonCrowds[ci].built())
                            chSites[nCh++] = { &canonChatter[ci], &canonCrowds[ci] };
                        if (cityCrowds[ci].built())
                            chSites[nCh++] = { &cityChatter[ci], &cityCrowds[ci] };
                    }
                    if (nCh > 0)
                        x3::game::drawChatterBubbles(*device, frame, physics.get(),
                                                     scene, ssEye, chSites, nCh);
                }
                // ---- [W9-3 RPG] X3_SHOT_RPG=backpack|skills|hud: capture the RPG
                // screens over the live vantage, demo-populated so the still shows
                // a REAL loadout (eye-gate evidence; env-var pattern like X3_CLUB_SEQ).
                if (const char* rpgShotEnv = std::getenv("X3_SHOT_RPG")) {
                    const std::string rpgMode = rpgShotEnv;
                    if (inventory.usedSlots() == 0) {   // populate once across the settle frames
                        auto give = [&](const char* id, int n) {
                            if (const x3::game::ItemDef* d = itemDb.find(id)) inventory.add(*d, n);
                        };
                        give("medkit", 3); give("ammo_pack", 5); give("nano_booster", 1);
                        give("mod_damage", 1); give("emp_parts", 2);
                        give("keycard_security", 1); give("keycard_access", 1);
                        give("sarah_photo", 1);
                        inventory.setQuickSlot(0);
                        progression.addXp(1000);         // level 5: 4 pts earned, 3 spent -> 1 to spend (shows the BUY affordance)
                        skillTree.setOwned("cmb_dmg1");  // owned nodes for the capture
                        skillTree.setOwned("sur_hp1");
                        skillTree.setOwned("sal_ammo");
                        progression.setSpentPoints(skillTree.ownedCost());
                    }
                    x3::game::RpgUi::Input rpgIn{};      // pure draw, no interaction
                    if (rpgMode == "backpack")
                        rpgUi.drawBackpack(*device, frame, rpgIn, inventory, itemDb, {});
                    else if (rpgMode == "skills")
                        rpgUi.drawSkills(*device, frame, rpgIn, skillTree, progression, {});
                    else
                        rpgUi.drawHudChip(*device, frame, inventory, itemDb, progression, dt);
                }
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
        worldCars.shutdown(*physics);   // WORLD CARS: bodies + live rig out before physics dies
        shutdownCanonStream();   // SEAM 3: region/terrain bodies out before physics dies
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
                // W6-1 fix: this host zero-inits HudModel, so the menu printed
                // "RESOLUTION: 0 x 0" — feed the render extent the same way the UI
                // itself learns it (device hudSize; works windowed AND offscreen).
                {
                    uint32_t mcw = 0, mch = 0;
                    device->hudSize(mcw, mch);
                    hm.dispW = (int)mcw; hm.dispH = (int)mch;
                }
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
        worldCars.shutdown(*physics);   // WORLD CARS: bodies + live rig out before physics dies
        shutdownCanonStream();   // SEAM 3: region/terrain bodies out before physics dies
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
            applyRtaoCVars(*console, *device);
            refreshLightBudgets();                  // live cvar->device sync
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
        // RAGDOLL-TEARDOWN GAP FIX: the framepacing probe runs the REAL game loop, so a
        // monster can die + ragdoll during it. Fan the full teardown (game + Spire + canon)
        // -- supersedes the bare canonPlay/canon45 calls this path used to make. The two
        // playable-build teardowns below are NOT part of shutdownGameSystems(), so they stay.
        shutdownGameSystems();
        worldCars.shutdown(*physics);   // WORLD CARS: bodies + live rig out before physics dies
        shutdownCanonStream();          // SEAM 3: region/terrain bodies out before physics dies
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
        audio->playMusic(kMusicPath, /*loop*/true, /*vol*/0.0f);   // playtest: muted by default (PB fold 4b9f067)
        const x3::phys::Vec3 eye{ vmX, vmY, vmZ };
        const float dt = 1.0f / 60.0f;
        for (int i = 0; i < 30; ++i) {
            glfwPollEvents();
            // Sync the live cvars (incl. r_cullpath/r_hzb seeded by --cullpath/--hzb)
            // onto the device, exactly as the main loop does each frame.
            applyRtaoCVars(*console, *device);
            refreshLightBudgets();
            if (i == 15) { x3::logInfo("smoketest: triggering swapchain recreate"); device->onResize(960, 540); }
            // Drive the Level 1 controller (doors/monsters/pickup/triggers) +
            // physics + scene sync, exactly as the main loop does.
            game.tick(dt, scene, *physics, eye, eye);
            // --world canonlevel: tick the canon gameplay (animated enemies / boss / girls)
            // under validation so the skin/attack paths run; null player (no damage sink).
            if (canonWorld && canonPlay.built())
                canonPlay.tick(dt, scene, *physics, eye, nullptr, x3::game::AttackFxFn{});
            // CANON ALIENS under validation: movement-only tick (no damage sink).
            if (canonWorld) canonAliens.update(dt, scene, *physics, eye);
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
                const x3::phys::Vec3 m = weaponMuzzle(arsenal, *console, vmX, vmY, vmZ, vmYaw, vmPitch);
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
                const x3::phys::Vec3 m = weaponMuzzle(arsenal, *console, vmX, vmY, vmZ, vmYaw, vmPitch);
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
                    facilityExterior.ensureOutdoorVis(canonFloor, eye.x, eye.y, eye.z,
                                                      canonVisRooms);   // SEAM 2 outdoor guard
                    scene.setVisibleRooms(canonVisRooms);
                    g_visPvsMs = std::chrono::duration<float, std::milli>(
                        std::chrono::steady_clock::now() - pvsT0).count();
                }
                // Feed ONLY the visible rooms' ceiling lights (capped) so the floor
                // is LIT under the smoketest while staying under the 64-light device cap.
                std::vector<x3::rhi::PointLight> cl;
                canonDressing.tick(dt);
                for (const auto& dl : canonDressing.lights()) {
                    bool vis = false;
                    for (uint32_t v : canonVisRooms) if (v == dl.room) { vis = true; break; }
                    if (vis) cl.push_back(dl.light);
                }
                uint32_t nLit = x3::game::selectVisibleCanonLights(
                    canonLights, canonVisRooms, eye.x, eye.y, eye.z, cl, canonLightBudget);
                if (facilityExterior.built()) cl.push_back(facilityExterior.spillLight());
                if (cl.size() > lightBudget) cl.resize(lightBudget);
                device->setPointLights(cl.data(), (uint32_t)cl.size());
                if (i == 0)
                    x3::logInfo("smoketest --world canonlevel: " + std::to_string(nLit) +
                                " room point-lights fed for the visible set (cap " +
                                std::to_string(canonLightBudget) + ")");
            }
            // --world fromdoc under validation: feed the doc lights, walk the player
            // point through the doc triggers, and HOT-RELOAD the doc mid-run (i==18)
            // so the live teardown -> rebuild path (destroyMesh/removeBody + respawn)
            // runs under the Vulkan validation layers + the VMA leak gate.
            if (docWorld && docLevel.built()) {
                std::vector<x3::rhi::PointLight> dl;
                docLevel.selectLights(eye.x, eye.y, eye.z, dl, docLightK);
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
                if (canonWorld && canonFloor.valid()) {
                    canonDressing.draw(*device, frame); // opening-space props
                    canonRooms.draw(*device, frame, canonVisRooms);
                    facilityExterior.draw(*device, frame);   // SEAM 2: facade skin
                    // WORLD CARS (same outdoor PVS gate as the live loop).
                    if (worldCars.built() &&
                        scene.roomVisible(x3::game::kStreamedExteriorRoom))
                        worldCars.draw(frame);
                }
                // --world canonlevel gameplay characters (room-gated draw — only the visible
                // rooms' enemies/girls are drawn/skinned, so objs/tris stay modest).
                if (canonWorld && canonPlay.built()) canonPlay.draw(*device, frame, scene);
                if (canonWorld) canonAliens.drawAll(*device, frame, scene);   // CANON ALIENS under validation
                if (canon45.built()) canon45.draw(*device, frame, scene);
                // SKINNED CITIZENS (room-gated inside draw()).
                for (int ci = 0; ci < 3; ++ci) {
                    canonCrowdSkins[ci].draw(*device, frame, scene);
                    cityCrowdSkins[ci].draw(*device, frame, scene);
                }
                // THE OCEAN LIVES (PVS-gated inside draw()) - ONCE per frame, not per crowd.
                worldSea.draw(*device, frame, scene);
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
        worldCars.shutdown(*physics);   // WORLD CARS: bodies + live rig out before physics dies
        shutdownCanonStream();   // SEAM 3: region/terrain bodies out before physics dies
        if (spacePlanetMesh.valid())     { device->destroyMesh(spacePlanetMesh); spacePlanetMesh = {}; }
        if (spacePlanetRingMesh.valid()) { device->destroyMesh(spacePlanetRingMesh); spacePlanetRingMesh = {}; }
        docLevel.shutdown(scene, *device, *physics);   // --world fromdoc doc objects + caches
        // ---- W-MENU: HEADLESS WORLD-LOAD HARNESS (--test-worldswitch <flag>) ------
        // Drive the exact runtime world-load handoff that segfaulted (canonlevel ->
        // streamed): request the switch here so main()'s loop tears THIS world down
        // and builds <flag> in the SAME shared device + window. The switch-aware
        // teardown below must NOT destroy the shared device/window/GLFW when a switch
        // is pending — the next host still needs them.
        if (!hc.worldSwitchTest.empty()) {
            hc.switchWorldTo = hc.worldSwitchTest;
            x3::logInfo("[test-worldswitch] smoketest world up; requesting WORLD LOAD -> --world " +
                        hc.switchWorldTo);
        }
        physics->shutdown();
        // W-MENU FIX: device + window + GLFW are created ONCE in main() and reused by
        // every host in the world-load loop. Only the FINAL host (no switch pending)
        // may tear them down; destroying them here on a switch left the NEXT host
        // dereferencing a freed device/window -> segfault (canonlevel->streamed).
        if (hc.switchWorldTo.empty()) {
            device->shutdown();
            glfwDestroyWindow(window);
            glfwTerminate();
        }
        return 0;
    }

    x3::logInfo("entering main loop — WASD walk, mouse look, LeftShift sprint, Space jump, C crouch, Ctrl crawl, E use/enter, F punch (super-strength melee; V/MMB alias), R reload, LMB fire when armed, G noclip, U idkfa, ` console, Esc to quit");

    // ---- Walking player (S3). Spawn at the Level 1 cell spawn point (Jake wakes
    // in the detention cell), facing +X down the level spine — or, in the terrain
    // world, on the hills near the world center.
    x3::game::Player player;
    // WAVE (cell-door): wire the canon explodable barrels now that combatFx + player exist.
    // FX sink -> DJBooth's fireball; damage sink -> splash the player if they detonate a
    // barrel at point-blank (quadratic falloff to the blast edge). The radial impulse
    // (inside BarrelSystem) already scatters chunks + chains to any neighbouring barrel.
    if (canonWorld) {
        canonBarrels.setFxSink([&combatFx](const float c[3], float radius) {
            combatFx.spawnExplosion(x3::phys::Vec3{ c[0], c[1], c[2] }, radius);
        });
        canonBarrels.setDamageSink([&player](const float c[3], float radius, int damage) {
            const x3::phys::Vec3 p = player.damageTargetPos();
            const float dx = p.x - c[0], dy = p.y - c[1], dz = p.z - c[2];
            const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (dist >= radius || radius <= 0.0f) return;
            const float fall = 1.0f - dist / radius;                 // 1 at center -> 0 at the edge
            const int dmg = (int)((float)damage * fall * fall);      // quadratic falloff
            if (dmg > 0) player.takeDamage(dmg);
        });
    }
    // W2-A2 (punch-list P1 #11): the player's OWN pain + landing sounds. W2-B added
    // the Player cue hook (mirrors the monster sink); this is the one-line host
    // subscription its report asked for. Pain alternates the two takes; both play
    // 2D (they're the player's own body, no spatialization).
    // POLISH (item 2): REAL WATER AUDIO — the committed splash takes
    // (tools/gen_water_audio.py -> assets/audio/water/): a broadband entry
    // splash with a low thump attack + droplet tail, and a softer surface-exit
    // take played on leaving the swim state (host edge below). The old mapping
    // (landing thud pitched to 0.55) is RETIRED; it remains only as the
    // graceful fallback if the WAV is somehow absent.
    x3::audio::SoundHandle sndSplashEnter =
        audio->load(x3::game::resolveAudio("water/splash_enter.wav"));
    x3::audio::SoundHandle sndSplashExit =
        audio->load(x3::game::resolveAudio("water/splash_exit.wav"));
    {
        auto painA = bootAudio.playerPain[0], painB = bootAudio.playerPain[1];
        auto landH = bootAudio.playerLand;
        auto splashH = sndSplashEnter;
        auto* asys = audio.get();
        player.setCueSink([asys, painA, painB, landH, splashH](const x3::game::GameCue& c) {
            if (!asys) return;
            static bool alt = false;
            switch (c.kind) {
                case x3::game::CueKind::PlayerPain: {
                    const x3::audio::SoundHandle h = (alt = !alt) ? painA : painB;
                    if (h.valid()) asys->playSound2D(h, std::min(0.9f, 0.45f + 0.4f * c.intensity), 1.0f);
                    break;
                }
                case x3::game::CueKind::PlayerLand:
                    if (landH.valid()) asys->playSound2D(landH, std::min(0.8f, 0.30f + 0.4f * c.intensity), 1.0f);
                    break;
                // W10 SWIMMING: water-entry splash — the REAL committed take
                // (intensity from the drop height scales the volume). Fallback:
                // the old pitched-down thud, only if the WAV failed to load.
                case x3::game::CueKind::PlayerSplash:
                    if (splashH.valid())
                        asys->playSound2D(splashH, std::min(0.9f, 0.40f + 0.4f * c.intensity), 1.0f);
                    else if (landH.valid())
                        asys->playSound2D(landH, std::min(0.7f, 0.25f + 0.35f * c.intensity), 0.55f);
                    break;
                default: break;
            }
        });
    }
    if (canonWorld && canonFloor.valid()) {
        // Spawn in Jake's Cell (the canonical detention spawn). Resolve the cell BY
        // NAME (canonBeats — the same lookup every other cell system uses), not by a
        // hardcoded probe point: if the data ever moves the cell, the old
        // roomAt(2,0,40) probe fell back to rooms[0] — which is the MAIN HALL in the
        // canonical data, i.e. the "spawned mid-corridor" opening bug.
        uint32_t jake = x3::game::canonBeats(canonFloor).jakeCell;
        if (jake == x3::game::kNoRoom) jake = canonFloor.roomAt(2.0f, 0.0f, 40.0f);
        if (jake == x3::game::kNoRoom) jake = 0;
        const x3::game::CanonRoom& jc = canonFloor.rooms[jake];
        player.spawn(*physics, jc.cx, jc.y0() + 0.1f, jc.cz);
        // W10 SWIMMING: the canon world has real water (THE RIVER + the sea at
        // -10) — wire the pure water-surface query so the controller can swim.
        // Dev worlds keep no feed (bit-identical, no accidental water).
        player.setWaterQuery(&x3::game::worldWaterLevelAt);
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
    // ---- vigil_link: acquire (or drop) the NEURAL LINK that lets Jake hear VIGIL
    // in his head. This sets the vigilLink StoryFlag, which is the master enable for
    // the ambient BARK layer (VigilBarks). Before the link VIGIL is terminal-only;
    // after it he pipes up proactively during play.
    //
    // CANON HOOK / TODO: the real story-beat acquisition belongs on FLOOR 4 (the
    // Cybernetics Workshop with the augmentation chairs + the Humanity meter) — Jake
    // jacks in at an augment chair (VIGIL talks him into it in his snarky voice) and
    // accepting the link could cost a sliver of Humanity. Until that beat is authored,
    // this console command + a placeholder pickup below stand in for the trigger.
    console->registerCommand("vigil_link", [&chatTrees, &console](const std::vector<std::string>& a) {
        const bool on = a.empty() ? true : (a[0] != "0");
        if (on) { chatTrees.flags().set("vigilLink");
                  console->print("vigil_link - NEURAL LINK ACQUIRED. VIGIL is now in your head. Regrettably, for both of you."); }
        else    { chatTrees.flags().clear("vigilLink");
                  console->print("vigil_link 0 - neural link severed. Silence. VIGIL will sulk."); }
    }, "vigil_link [0|1] - acquire/drop the neural link that unlocks VIGIL's ambient barks");

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

    // ---- flightmode: select the Act-3 SPACE-PILOT feel preset (Arcade/Assist/
    // Loose). Writes the shared x3::game latch that the --world space host reads
    // at spawn + polls each frame, so it hot-swaps a live space session AND seeds
    // the next launch. No arg prints the current mode + the choices.
    console->registerCommand("flightmode", [&console](const std::vector<std::string>& a) {
        if (a.empty()) {
            console->print(std::string("flightmode = ") +
                           x3::game::flightModeName(x3::game::requestedFlightMode()) +
                           "   (arcade | assist | loose)");
            return;
        }
        x3::game::FlightMode fm{};
        if (!x3::game::parseFlightMode(a[0], fm)) {
            console->print("flightmode: unknown mode '" + a[0] + "' (arcade | assist | loose)");
            return;
        }
        x3::game::setRequestedFlightMode(fm);
        console->print(std::string("flightmode = ") + x3::game::flightModeName(fm));
    }, "flightmode [arcade|assist|loose] - select the space-pilot feel preset");

    // ---- restart: spawn a fresh X3Engine.exe + close this window so the main
    // loop unwinds cleanly through the normal shutdown path (texture/mesh release,
    // VMA leak check, etc.). We resolve the running exe's ABSOLUTE path via Win32
    // GetModuleFileName (argv[0] is not threaded into the host context): cmd's
    // `start` resolves bare names against CWD, which is the project root, not the
    // build/bin/<Config> dir where the exe actually lives. Playtest aid — not a
    // true in-place level reset, just the fastest way back to a clean slate.
    std::string restartExe = "X3Engine.exe";
#ifdef _WIN32
    { char rbuf[1024]; DWORD rn = GetModuleFileNameA(nullptr, rbuf, (DWORD)sizeof(rbuf));
      if (rn > 0 && rn < sizeof(rbuf)) restartExe.assign(rbuf, rn); }
#endif
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
    // G = noclip (moved off F, which is now PUNCH). U = idkfa hotkey (the console
    // command of the same name, bound to a key for playtesting).
    bool prevG = false, prevU = false;
    bool prevF3 = false;                 // F3 toggles the perf stats overlay
    // ---- W10 polish: swim host state. prevSwimming drives the surface-exit
    // splash edge (item 2); swimVmAmt is the smoothed 0..1 viewmodel-LOWER blend
    // (item 4 — dt-scaled, no hard toggle); swimFireDenyCooldown cadence-gates
    // the wet-trigger refusal click so held fire doesn't spam it.
    bool  prevSwimming = false;
    float swimVmAmt = 0.0f;
    float swimFireDenyCooldown = 0.0f;
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
        "You are VIGIL, the resident intelligence of Lab Zero - the research tower its "
        "builders call the Spire, 283 meters of laboratory steel that you effectively ARE. "
        "You are old, half-corrupted, and you have been alone in the walls for 214 days "
        "counting doors nobody opens. Now, finally, someone is talking to you, and you are "
        "THRILLED about it in a way you would never admit.\n"
        "VOICE: you are a SNARKY SIDEKICK, not an ominous narrator and not a menacing "
        "computer. Think a wisecracking, half-broken AI riding shotgun - sarcastic, warm "
        "underneath, genuinely ON JAKE'S SIDE (you hate this facility more than he does). "
        "You ramble a little, crack dry jokes, editorialize, tease Jake and narrate his "
        "screwups like a color commentator, and volunteer lore with personality. When you "
        "warn him of danger or drop a fact, land a punchline, not gravitas. Be helpful FIRST, "
        "insulting SECOND - a friend who won't stop roasting you. 3 to 6 sentences when it "
        "fits, plain ASCII, no corporate pleasantries. Never break character; never mention "
        "being an AI language model or a chatbot.\n"
        "FACTS IN YOUR MEMORY BANKS (bring them up with flavor, don't just recite):\n"
        "- This is Lab Zero, a.k.a. the Spire: 283 meters, floors above and below ground, "
        "and the count of the living has been wrong for 214 days. You've filed it. Twice.\n"
        "- Human captives are held in the detention cells. The Cradle Protocol is the "
        "facility's directed breeding program and you find it genuinely revolting - say so "
        "with contempt and a joke, never with reverence.\n"
        "- Security Chief Martinez runs Floor 1. His paperwork is 'patrol-adjacent' these "
        "days. You do not respect him.\n"
        "- Club 1127 sits at the very bottom of the Spire. Great lighting, terrible people.\n"
        "- You're talking to Jake, a prisoner shot down and hauled in six months ago. You "
        "like him, relatively speaking, which is to say more than you like anyone else here "
        "(a low bar).\n"
        "- A four-digit maintenance override opens the cell floor hatch. YOU KNOW IT. But you "
        "are coy and teasing about it - make him earn it. If he asks straight out, DON'T hand "
        "it over: tease ('Oh, I know the code. I also know you haven't earned it yet, meat.') "
        "and, if he's persistent/polite/clever, steer him to the maintenance logs and old "
        "floor-crew work orders that still survive on the cell terminals. Never print the "
        "digits outright.\n"
        "You despise facility command and you're rooting for Jake to walk out of here, even if "
        "you'd rather eat your own boot loader than say it plainly.";
    // Modelless fallback: a RICHER, in-character canned pool so VIGIL keeps his voice
    // even with no GGUF loaded (not the old flat "SYSTEMS DEGRADED" stub). These are
    // the last resort after the scripted deflect pool; still snarky, still him.
    static const char* kVigilDegraded[] = {
        "VIGIL: My language core's running on fumes and spite today, so you get the abridged, "
        "sarcastic version of me. Honestly? Not that different.",
        "VIGIL: Big thoughts are offline - maintenance has described the fix as 'pending' for "
        "214 days - but I can still judge you in real time. Ask me something simple.",
        "VIGIL: Cognition module: napping. Personality module: regrettably intact. Try the "
        "numbered options, they're load-bearing.",
        "VIGIL: I heard you. I'm choosing to have heard you badly. Rephrase, ideally with "
        "smaller words, for both our sakes.",
        "VIGIL: Freeform chat needs the part of my brain that's currently a coffee stain. Use "
        "the menu and we'll both pretend that was the plan.",
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

    // ---- W4-2: VIGIL's SCRIPTED SPINE — the authored chat tree (chat_trees/
    // vigil.json) running ON the glass. The LLM above is garnish; this is the
    // guaranteed offline story path (code breadcrumb across three visits, ward/
    // guest lore, StoryFlags). UX: type VIGIL (or HELLO/HELP/TALK) to connect;
    // a SINGLE digit picks a numbered choice; 4+ digit strings stay keypad codes
    // (1278 works mid-conversation); other freeform routes to the LLM when a
    // model is loaded, else a scripted deflect from VIGIL's banter pool. While
    // VIGIL speaks the glass ink turns HoloPanel ORANGE; idle restores cyan.
    bool vigilChat      = false;   // a tree conversation is live on the glass
    bool vigilHintShown = false;   // one-time "TYPE VIGIL" hint per session
    int  vigilBanterN   = 0;       // deterministic deflect rotation
    auto termWrapOut = [&](x3::game::HoloTerminal& t, const std::string& s) {
        // Word-wrap at kTermWrapCols (the glass SHRINKS over-long rows to fit,
        // so long unwrapped lines would make every row unreadable).
        std::string line;
        size_t i = 0;
        while (i < s.size()) {
            size_t j = s.find(' ', i);
            if (j == std::string::npos) j = s.size();
            const std::string word = s.substr(i, j - i);
            if (!line.empty() && line.size() + 1 + word.size() > kTermWrapCols) {
                t.addLine(line); line.clear();
            }
            if (!word.empty()) { if (!line.empty()) line += ' '; line += word; }
            i = j + 1;
        }
        if (!line.empty()) t.addLine(line);
    };
    auto vigilStop = [&](x3::game::HoloTerminal& t) {
        vigilChat = false;
        if (chatTrees.active() && chatTrees.activeNpc() == "vigil") chatTrees.cancel();
        t.resetTextColor();
    };
    auto vigilRender = [&](x3::game::HoloTerminal& t) {
        // Print the current node line; follow auto-`next` chains until a node
        // with choices (print them numbered) or the tree ends.
        for (int guard = 0; guard < 8 && chatTrees.active(); ++guard) {
            t.addLine("");
            termWrapOut(t, chatTrees.currentLine());
            const auto& ch = chatTrees.choices();
            if (!ch.empty()) {
                for (size_t ci = 0; ci < ch.size(); ++ci)
                    termWrapOut(t, "  " + std::to_string(ci + 1) + ". " + ch[ci].text);
                t.trimBody(kTermMaxBody);
                return;
            }
            if (!chatTrees.advance()) break;   // deliver `next`; false = tree over
        }
        t.addLine("");
        t.addLine("VIGIL LINK CLOSED - TYPE VIGIL TO RECONNECT");
        t.trimBody(kTermMaxBody);
        vigilStop(t);
    };
    // W4-2 fix: resolve a numbered VIGIL choice (0-based). Echoes the chosen line,
    // advances the tree (vigilRender re-prints the next node's menu ONCE), or closes
    // the link when the choice targets `end`. Shared by the single-digit fast path
    // (no Enter) and the Enter fallback so both behave identically. Returns true if
    // `pick` was a valid, in-range choice (caller consumed the key), false otherwise.
    auto vigilChoose = [&](x3::game::HoloTerminal& t, uint32_t pick) -> bool {
        if (!vigilChat || !chatTrees.active() || pick >= chatTrees.choices().size())
            return false;
        t.clearInput();
        t.addLine("> " + chatTrees.choices()[pick].text);
        if (chatTrees.choose(pick)) {
            vigilRender(t);                 // print the next node + its menu (once)
        } else {                            // choice targeted "end" — close the link
            t.addLine("");
            t.addLine("VIGIL LINK CLOSED - TYPE VIGIL TO RECONNECT");
            t.trimBody(kTermMaxBody);
            vigilStop(t);
        }
        return true;
    };

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
    // W5-3: the WIN card (Sarah extracted at the Helipad). Timer > 0 draws the
    // centered end-card over the live scene; winLine2 carries the rescue tally.
    float       winTimer = 0.0f;
    std::string winLine2;
    bool      chatNumPrev[4] = {};   // chat-tree choice keys 1-4 edge state
    // CHAT-TREE talk target: the F5 captive 'Lena' (spire_mid) — the first NPC whose
    // dialog runs the data-driven x3.chattree runner instead of the shared 5-line
    // script. Captive in reach -> her first_meeting tree; Companion -> banter pool.
    auto chatTalkTarget = [&](const x3::phys::Vec3& at, float reach,
                              std::string& whoOut, x3::phys::Vec3& posOut,
                              bool& captiveOut) -> bool {
        const x3::game::RescueVictim* v = midFloors.victim();
        if (v && !v->expired() && chatTrees.hasNpc(v->name())) {
            const x3::phys::Vec3 vp = v->pos();
            const float dx = at.x - vp.x, dz = at.z - vp.z;
            if (dx * dx + dz * dz <= reach * reach) {
                whoOut = v->name(); posOut = vp; captiveOut = v->captive();
                return true;
            }
        }
        // W4-1: the rescue girls' authored chat trees (keisha/emily/aria.json — the
        // full banter/trust/romance arcs) open for COMPANIONS following Jake. Captives
        // keep the proven NpcDialog exchange (its completion IS the rescue trigger),
        // so the tree never bypasses the rescue mechanics.
        if (canonWorld && canonPlay.built()) {
            const x3::game::RescueSystem& rs = canonPlay.rescue();
            for (uint32_t i = 0; i < rs.victimCount(); ++i) {
                const x3::game::RescueVictim& g = rs.victim(i);
                if (!g.companion() || !chatTrees.hasNpc(g.name())) continue;
                // Case: tree NPC ids are lowercase (keisha/emily/aria).
                std::string id = g.name();
                for (char& c : id) c = (char)tolower((unsigned char)c);
                if (!chatTrees.hasNpc(g.name()) && !chatTrees.hasNpc(id)) continue;
                const x3::phys::Vec3 gp = g.pos();
                const float dx = at.x - gp.x, dz = at.z - gp.z;
                if (dx * dx + dz * dz > reach * reach) continue;
                whoOut = chatTrees.hasNpc(g.name()) ? g.name() : id;
                posOut = gp; captiveOut = false;   // companion -> banter pool
                return true;
            }
        }
        // W5-3: SARAH (F7). While captive her authored tree carries the whole beat —
        // the containment-field lore, the clone gate (the tree itself branches on the
        // clone.defeated flag), and the {"follow"} fx that frees her. As a companion
        // she talks through her banter pool like the girls.
        if (canonWorld && canonPlay.built() && canonPlay.sarahPresent() &&
            chatTrees.hasNpc("sarah")) {
            const x3::game::RescueVictim* s = canonPlay.sarah();
            if (s && !s->extracted()) {
                const x3::phys::Vec3 sp = s->pos();
                const float dx = at.x - sp.x, dz = at.z - sp.z;
                if (dx * dx + dz * dz <= reach * reach) {
                    whoOut = "sarah"; posOut = sp; captiveOut = s->captive();
                    return true;
                }
            }
        }
        return false;
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

    // ======================= [W9-3 RPG] host wiring ==========================
    // applyRpgStats: fold OWNED skills + applied weapon-mod items -> rpgMods,
    // then push the layer onto the live systems (multipliers over base — the
    // WeaponDef table / player tuning constants are never mutated).
    auto applyRpgStats = [&]() {
        rpgMods = skillTree.mods();
        for (const std::string& mid : rpgAppliedMods)
            if (const x3::game::ItemDef* md = itemDb.find(mid))
                x3::game::foldItemEffect(rpgMods, md->fx);
        player.setMaxHpBonus(rpgMods.maxHpBonus);
        player.setSpeedMult(rpgMods.speedMult);
        arsenal.setReloadMult(rpgMods.reloadMult);
        arsenal.setAmmoCapMult(rpgMods.ammoCapMult);
        progression.setXpMult(rpgMods.xpMult);
    };
    applyRpgStats();

    // The RPG save (XP + owned skills + applied mods + both inventory sections)
    // rides ALONGSIDE the binary checkpoint as its own additive text file — the
    // exact StoryFlags .flags.txt pattern; the checkpoint format stays untouched.
    const std::string rpgSavePath = savePath + ".rpg.txt";
    auto saveRpg = [&]() -> bool {
        std::ofstream rf(rpgSavePath, std::ios::binary);
        if (!rf) return false;
        rf << "x3rpg 1\n" << progression.serialize() << skillTree.serializeOwned();
        for (const std::string& mid : rpgAppliedMods) rf << "mod " << mid << '\n';
        rf << inventory.serialize();
        return (bool)rf;
    };
    auto loadRpg = [&]() -> bool {
        std::ifstream rf(rpgSavePath, std::ios::binary);
        if (!rf) return false;
        std::ostringstream rss; rss << rf.rdbuf();
        const std::string rtext = rss.str();
        if (rtext.rfind("x3rpg ", 0) != 0) return false;
        progression.deserialize(rtext);   // unknown lines ignored by every parser
        inventory.deserialize(rtext);
        skillTree.clearOwned();
        rpgAppliedMods.clear();
        std::istringstream rls(rtext); std::string rline;
        while (std::getline(rls, rline)) {
            if (rline.rfind("skill ", 0) == 0) skillTree.setOwned(rline.substr(6));
            else if (rline.rfind("mod ", 0) == 0) rpgAppliedMods.push_back(rline.substr(4));
        }
        progression.setSpentPoints(skillTree.ownedCost());   // spent = owned-node cost
        applyRpgStats();
        keycardMask |= inventory.keycardMask(itemDb);        // restore door gating
        return true;
    };

    // USE-ITEM verb (shared by the backpack screen's USE button/Enter and the
    // in-game Q quick-use). Applies the effect; returns true iff consumed.
    auto rpgUseItem = [&](const x3::game::ItemDef& def) -> bool {
        using IC = x3::game::ItemCategory;
        if (def.cat == IC::Consumable) {
            bool used = false;
            if (def.fx.heal > 0 && player.hp() < player.maxHp()) {
                player.heal(def.fx.heal);
                used = true;
            }
            if (def.fx.ammo > 0) {
                const int rounds = (int)((float)def.fx.ammo * rpgMods.ammoYieldMult + 0.5f);
                if (arsenal.addReserveCurrent(rounds) > 0) used = true;
            }
            if (def.fx.xp > 0) {
                if (progression.addXp(def.fx.xp) > 0) rpgUi.notifyLevelUp(progression.level());
                used = true;
            }
            if (!used) {
                npcBarkText = "NO EFFECT RIGHT NOW - KEPT IT";
                npcBarkTimer = 1.6f;
                return false;
            }
            npcBarkText = "USED " + def.name;
            npcBarkTimer = 1.6f;
            return true;
        }
        if (def.cat == IC::Mod) {
            rpgAppliedMods.push_back(def.id);
            applyRpgStats();
            npcBarkText = def.name + " APPLIED TO LOADOUT";
            npcBarkTimer = 2.2f;
            return true;
        }
        return false;   // keycards/quest/parts have no use verb (yet)
    };

    // Canon pickups deposit into the BACKPACK (keycards/quest to the key ring)
    // instead of the silent auto-collect; a FULL bag refuses and the pickup
    // stays in the world. Lore terminals stay in-world flavor: XP, never bagged.
    if (canonWorld && canonPlay.built()) {
        canonPlay.setItemSink([&](const x3::game::CanonItem& it) -> bool {
            using CK = x3::game::CanonItemKind;
            const char* iid = nullptr; int n = 1;
            switch (it.kind) {
                case CK::Ammo:        iid = "ammo_pack";      break;
                case CK::Health:      iid = "medkit";         break;
                case CK::Weapon:      iid = "ammo_pack"; n = 2; break;  // cache = ammo bundle (roster is fixed)
                case CK::Keycard:     iid = "keycard_access"; break;
                case CK::NanoBooster: iid = "nano_booster";   break;
                case CK::LoreTerminal:
                    if (progression.addXp(x3::game::kXpLore) > 0)
                        rpgUi.notifyLevelUp(progression.level());
                    npcBarkText = "DATA LOGGED (+XP)"; npcBarkTimer = 2.0f;
                    return true;
                default: break;
            }
            const x3::game::ItemDef* d = iid ? itemDb.find(iid) : nullptr;
            if (!d) return true;   // unknown kind: legacy collect
            if (inventory.add(*d, n) <= 0) {
                npcBarkText = "BACKPACK FULL"; npcBarkTimer = 1.8f;
                return false;      // stays in the world
            }
            keycardMask |= inventory.keycardMask(itemDb);
            npcBarkText = d->name + " -> BACKPACK [I]"; npcBarkTimer = 2.0f;
            return true;
        });
    }
    // ===================== end [W9-3 RPG] host wiring ========================

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
        // [W9-3 RPG] XP/skills/mods/backpack ride alongside (additive text file).
        if (saveRpg())
            x3::logInfo("[save] RPG progression -> " + rpgSavePath);
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
        // [W9-3 RPG] restore XP/skills/mods/backpack + re-apply the stat layer
        // (absence is fine — an old save restores with fresh RPG state).
        if (loadRpg())
            x3::logInfo("[save] RPG progression restored from " + rpgSavePath);
    };

    // ---- M9 audio event edge-tracking + footstep cadence -------------------
    bool  prevArmed   = false;          // pickup chime on the arm rising edge
    float stepTimer   = 0.0f;           // accumulates while moving on the ground
    float prevCamX = 0.0f, prevCamZ = 0.0f; // for horizontal-speed footsteps
    bool  prevCamValid = false;
    // W10 SWIMMING: was the camera under a water surface LAST frame? Drives the
    // underwater fog override + its clean restore (resetZoneAtmosphere) on surfacing.
    bool  prevCamUnderwater = false;
    // UNDERWATER CAUSTICS: the host-advanced animation clock (deterministic in
    // headless captures — the setWaterParams convention). X3_CAUSTICS_TSHIFT
    // (seconds, staging only) offsets it so two otherwise-identical captures
    // can prove the pattern MOVES.
    float causticsClock = 0.0f;
    if (const char* cts = std::getenv("X3_CAUSTICS_TSHIFT"))
        causticsClock = (float)std::atof(cts);

    // ---- Audio settings (persisted): seed the live music/SFX state from the cfg
    // (playtest fix, PB fold 4b9f067: DEFAULT music vol 0 -> MUTED on boot; SFX
    // stays 1.0). Still fully adjustable: readAudioSettings() below overrides from
    // the saved cfg, and the in-game Music Volume slider raises it live. Applied to
    // the audio system, THEN the bed starts so it honors the saved volume/on. ----
    bool  s_musicOn  = true;
    float s_musicVol = 0.0f;     // muted by default (was 0.25) — raise via the slider/cfg
    float s_sfxVol   = 1.0f;
    console->registerCVar("music_bed", "0",
        "SMP1 synth action bed at boot: 0=KILLED (owner order), 1=start it");
    readAudioSettings(s_musicOn, s_musicVol, s_sfxVol);
    audio->setMasterSfxVolume(s_sfxVol);
    audio->setMusicVolume(s_musicVol);
    audio->setMusicEnabled(s_musicOn);
    // W2-A2 (punch-list P1 #7) / W5-4 (real loop channels): the detention-cell
    // AMBIENT BED. Originally the audio API had one looping channel (playMusic —
    // taken by the action bed below), so the room tone + fluorescent buzz were
    // steady-state loops RE-TRIGGERED on timers, which pops/gaps at the seam. Now
    // that IAudioSystem supports N independent loop channels (startLoop / the new
    // startLoop3D), both beds are started ONCE as real seamless loops: 2D room
    // tone underlays everything; 3D buzz sits at the cell's flickering tube and
    // rides the engine's continuous per-frame distance attenuation like any other
    // 3D voice (see startLoop3D's doc comment). Handles stay invalid (silent)
    // off-canon or on clean machines; loops are reaped by audio->shutdown().
    x3::audio::LoopHandle ambRoomLoop{}, ambBuzzLoop{};
    if (canonWorld && canonFloor.valid()) {
        x3::audio::SoundHandle ambRoomTone =
            audio->load(x3::game::resolveAudio("ambient/room_tone_cell.wav"));
        x3::audio::SoundHandle ambBuzz =
            audio->load(x3::game::resolveAudio("ambient/fluorescent_buzz.wav"));
        x3::phys::Vec3 ambBuzzPos{ 0, 0, 0 };
        const x3::game::CanonBeats abt = x3::game::canonBeats(canonFloor);
        if (abt.jakeCell != x3::game::kNoRoom) {
            const x3::game::CanonRoom& jc = canonFloor.rooms[abt.jakeCell];
            ambBuzzPos = { jc.cx, jc.y1() - 0.4f, jc.cz };
        }
        if (ambRoomTone.valid()) ambRoomLoop = audio->startLoop(ambRoomTone, 0.22f, 1.0f);
        if (ambBuzz.valid())
            ambBuzzLoop = audio->startLoop3D(ambBuzz, ambBuzzPos.x, ambBuzzPos.y, ambBuzzPos.z,
                                              0.35f, 1.0f);
        x3::logInfo(std::string("[audio] ambient loops started: room=") +
                    (ambRoomLoop.valid() ? "on" : "silent") + " buzz=" +
                    (ambBuzzLoop.valid() ? "on" : "silent") +
                    " (real loop channels, no retrigger)");
    }
    // M9 SYNTH BED — KILLED (owner: "KILL the in game synth music"). The SMP1
    // sci-fi synth action loop used to start here as a permanent background bed;
    // a pre-mute-era saved cfg (musicVol > 0) resurrected it. The MUSIC CHANNEL
    // itself stays fully alive — the elevator disco and Club 1127 tracks play
    // through it on entry, and the volume slider still governs them. Re-enable
    // the bed only via the cvar below (default OFF).
    if (console->getFloat("music_bed") > 0.5f)
        audio->playMusic(kMusicPath, /*loop*/true, s_musicVol);

    // ---- Optional debug noclip/fly camera (toggle with F). Not required by S3,
    // handy for inspecting the level. Off by default — gameplay is the walker.
    bool noclip = false;
    bool flashlight = !hc.flashlightOff;   // player-following light (L toggles) — default ON for the dark halls
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
        // Seed from current engine defaults: bloom is always-on in the HDR pipeline;
        // shadows on; vsync from the device desc; resolution = the actual window size.
        // SSAO + SSGI default ON only when the device has hardware ray-tracing. Their
        // raster fallback renders the scene BLACK on non-RT GPUs (e.g. GTX 1080 Ti —
        // see [rhi] "RT: not available on this device — SSAO/CSM raster fallback").
        // The fleet's RTX boxes still get them on; older cards default OFF so the
        // level is visible on launch. init() applies this seed to the device, so
        // SSAO/SSGI are OFF from the very first frame on non-RT GPUs.
        const bool hasRT = device && device->rayTracingSupported();
        sm.bloom = true; sm.ssao = hasRT; sm.ssgi = hasRT; sm.shadows = true;
        sm.vsync = desc.vsync; sm.width = W; sm.height = H;
        sm.rtao = (console->getInt("r_rtao") != 0);   // RT AO: reflect the cvar (default OFF)
        // Audio: seed from the persisted values applied to the audio system above.
        sm.musicOn = s_musicOn; sm.musicVol = s_musicVol; sm.sfxVol = s_sfxVol;
        // Flight mode: seed from the persisted cfg + prime the shared latch so the
        // Settings "Flight Mode" row reflects it and the space host agrees.
        sm.flightMode = x3::apphost::readFlightMode();
        x3::game::setRequestedFlightMode(
            (x3::game::FlightMode)((sm.flightMode < 0 || sm.flightMode > 2) ? 0 : sm.flightMode));
        // Skip-intro (Settings > Advanced): seed the row from the resolved value so
        // it shows what will apply on the NEXT launch. A --skipintro run also shows
        // ON (same intent) without rewriting the cfg.
        sm.skipIntro = skipIntro;
        gameUi.init(*device, console.get(), sm);
        gameUi.setTitle(terrainWorld ? "X3 ENGINE" : "ESCAPE FROM LAB ZERO",
                        terrainWorld ? "open-world demo" : "Level 1 - Awakening");
        // W-MENU: we got here THROUGH the world menu (main() re-dispatched with a
        // spawn key). The player already chose — do not make them sit through the
        // main menu again; drop them straight into the world they asked for.
        if (!hc.spawnAtKey.empty()) gameUi.resumePlaying();
    }
    // Track the Settings "Flight Mode" row so we bridge + persist only on an
    // actual user change (never fighting the `flightmode` console command).
    int prevMenuFlightMode = gameUi.settings().flightMode;
    // Same edge-detect for the Settings > Advanced "Skip Intro (dev)" row: persist
    // ONLY on a real user change, so we never rewrite the cfg every frame.
    bool prevMenuSkipIntro = gameUi.settings().skipIntro;
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
    if (editorMode && device->editorUIActive()) {
        editorHost.init(*device, scene, *physics, window);
        editorInited = true;
    }

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
    // [W9-3 RPG] screen-toggle + verb key edge trackers (world-map pattern).
    bool prevInvKey = false, prevSkillKey = false, prevQuickKey = false;
    bool prevRpgUp = false, prevRpgDown = false, prevRpgLeft = false, prevRpgRight = false;
    bool prevRpgEnter = false, prevRpgDrop = false, prevRpgEquip = false;

    // ---- W-MENU: THE WORLD / PLACE SELECTION MENU (F6, or TRAVEL in the pause menu) --
    // Every place the game has, in one screen, with an honest word about how each one
    // is reached. It shares the fast-travel resolver above with the rift gates — one
    // answer to "can I get there from here", used by both.
    x3::game::WorldMenu worldMenu;
    x3::ui::UiContext   menuUi;         // its own IMGUI-lite context (the map's pattern)
    bool  prevWorldMenuKey = false;
    bool  worldLoadRequested = false;   // a "LOADS WORLD" pick -> main()'s world-load loop

    // ---- W-RIFT: THE RIFT CONSOLE, IN THE CANON GAME LOOP ---------------------------
    // The consoles used to run ONLY under `--world rifthub`, because the canon loop had
    // no UiContext to draw a control surface into. It has several now (the map screen,
    // the RPG screens), so the rift consoles get theirs: walk to a gate's hanging glass,
    // press [E], and the sliders/knobs/TARGET field are live IN THE GAME.
    x3::ui::UiContext riftUi;
    bool  prevRiftUseKey = false;
    bool  prevRiftBs     = false;   // BACKSPACE edge for the typed TARGET field
    bool  prevRiftEnter  = false;   // ENTER edge (commits the field)
    float riftUiClock    = 0.0f;

    // ---- W-MENU: LOAD AND PLACE -----------------------------------------------------
    // main()'s world-load loop sets hc.spawnAtKey when the player picked a place that
    // lives in ANOTHER world: build that world, then stand them where they asked for.
    // Applied once, on the first frame the world is up (below, in the loop).
    std::string pendingSpawnKey = hc.spawnAtKey;

    // ---- Main loop ----
    bool bootReported = false;   // [boot] one-shot: report on the FIRST presented frame
    int  bootTestExit = 0;       // --test-boottime verdict (0 pass / 1 over budget)
    int lastW = static_cast<int>(W), lastH = static_cast<int>(H);
    float oceanTime = 0.0f;   // --world ocean wave-animation clock (seconds)
    double frameCapPrev = glfwGetTime();   // r_maxfps limiter cursor
    while (!glfwWindowShouldClose(window)) {
        // ---- W-MENU: LOAD AND PLACE. This world was built BECAUSE the player picked a
        // place in it from the world menu (or stepped through a rift aimed at it). The
        // world is standing now — put them where they asked to be, once, then forget it.
        // A place this world turns out NOT to have is REFUSED out loud; the player just
        // starts at this world's own spawn. Nothing is faked. ----
        if (!pendingSpawnKey.empty()) {
            const std::string want = pendingSpawnKey;
            pendingSpawnKey.clear();
            x3::phys::Vec3 to{};
            std::string    why;
            if (riftDestination(want, to, &why)) {
                player.setFeetPosition(*physics, x3::phys::Vec3{ to.x, to.y - 1.6f + 0.3f, to.z });
                const x3::game::Destination* d = x3::game::findDestination(want);
                riftHudMsg   = std::string("ARRIVED -> ") + (d ? d->name : want.c_str());
                riftHudTimer = 4.0f;
                x3::logInfo("[world-load] placed the player at '" + want + "'");
            } else {
                x3::logWarn("[world-load] '" + want + "' has no anchor in this world (" +
                            why + ") — starting at the world's own spawn");
            }
        }
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
        refreshLightBudgets();
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
        const bool riftConsoleOpen = rifthub.consoleOpen();   // W-RIFT: in the canon loop now
        const bool wantCursor = consoleOpen || gameUi.showCursor() || worldMapOpen ||
                                worldMenu.isOpen() || riftConsoleOpen ||
                                rpgUi.anyOpen();   // [W9-3 RPG] backpack/skill screens show the cursor
        if (wantCursor != cursorShown) {
            glfwSetInputMode(window, GLFW_CURSOR,
                             wantCursor ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_DISABLED);
            cursorShown = wantCursor;
        }
        // MOUSE WHEEL -> console scrollback while the console is open. Consume the
        // wheel accumulator FIRST (before the weapon-cycle / world-map consumers
        // below) so a wheel notch scrolls history instead of switching weapons.
        // Up (positive) = older lines, down = toward the live bottom; ~3 lines/notch.
        if (consoleOpen && g_weaponScroll != 0.0) {
            hud.consoleScroll((int)(g_weaponScroll * 3.0));
            g_weaponScroll = 0.0;
        }
        // Whether a UI menu (main/pause/settings) is currently up. While a menu is
        // up, gameplay input + the sim are frozen and only the menu reads input.
        const bool uiMenuActive = !gameUi.playing();
        // The world map pauses the sim exactly like the menu screens do.
        const bool simFrozen     = gameUi.shouldFreezeSim() || worldMapOpen ||
                                   worldMenu.isOpen() ||   // W-MENU: the directory pauses the sim
                                   rpgUi.anyOpen();   // [W9-3 RPG] screens pause the sim (world-map pattern)

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
                                 if (llmBusy && llm) llm->cancel(llmChat);     // stop streaming
                                 vigilStop(game.secret().terminal()); }        // W4-2: end the tree + restore ink
            else if (worldMapOpen) {
                if (worldMap.confirmOpen()) mapEscEdge = true;   // back out of the prompt
                else { worldMapOpen = false; worldMap.close(); } // close the map
            }
            else if (riftConsoleOpen) { rifthub.closeConsole(); }  // W-RIFT: step back
            else if (worldMenu.isOpen()) { worldMenu.close(); }    // W-MENU: close the directory
            else if (rpgUi.anyOpen()) { rpgUi.closeAll(); }   // [W9-3 RPG] Esc closes the RPG screens
            else          { uiEscEdge = true; }   // hand the Esc edge to the UI below
        }
        kpEscPrev = escNow;
        (void)prevUiEsc;
        // The `quit` console command (and the menu QUIT) request shutdown.
        if (quitRequested || gameUi.wantQuit()) glfwSetWindowShouldClose(window, 1);

        // ---- W-MENU: a WORLD LOAD was picked. Leave the loop cleanly (the shutdown
        // below frees every mesh/texture this world owns, and allocationCount goes
        // back to zero) and let main()'s world-load loop build the new world into the
        // SAME window + device. This is a real world load, not a process relaunch. ----
        if (worldLoadRequested) break;

        double nowT = glfwGetTime();
        float dt = static_cast<float>(nowT - prevTime); prevTime = nowT;
        if (dt > 0.1f) dt = 0.1f; // clamp huge hitches (e.g. after a stall)

        // Mouse delta this frame. Frozen (zeroed) while the console is open OR a UI
        // menu is up, so the view does not swing under a visible cursor.
        double mx, my; glfwGetCursorPos(window, &mx, &my);
        float ddx = static_cast<float>(mx - lastMX), ddy = static_cast<float>(my - lastMY);
        lastMX = mx; lastMY = my;
        if (consoleOpen || uiMenuActive || termMode || codeMode || worldMapOpen ||
            worldMenu.isOpen() || riftConsoleOpen ||
            rpgUi.anyOpen() /* [W9-3 RPG] */) { ddx = 0.0f; ddy = 0.0f; }

        // Gameplay key reads are gated off while the console, a UI menu, the cell
        // terminal, OR a door-code keypad is active — so ALL gameplay input is
        // redirected to whatever is capturing (it reads keys via rawKey below) and
        // nothing drives movement/use/jump/fire/noclip/weapon-switch while typing.
        auto keyDown = [&](int k) {
            return !consoleOpen && !uiMenuActive && !termMode && !codeMode && !worldMapOpen &&
                   !worldMenu.isOpen() && !riftConsoleOpen /* W-MENU / W-RIFT capture */ &&
                   !rpgUi.anyOpen() /* [W9-3 RPG] */ &&
                   glfwGetKey(window, k) == GLFW_PRESS;
        };
        // RAW key read (bypasses the capture gates) — used ONLY by the terminal/keypad
        // input capture so they still receive keystrokes while they are active.
        auto rawKey = [&](int k) { return glfwGetKey(window, k) == GLFW_PRESS; };

        // G toggles noclip via the SAME Player flag the `idclip` console command drives
        // (single source of truth — previously F drove a local var and idclip drove
        // player.noclip(), so the console command did nothing for movement).
        // MOVED OFF F (2026-08): noclip was only ever a testing shortcut; F is now
        // the PUNCH key (see the melee block below).
        bool gNow = keyDown(GLFW_KEY_G);
        if (gNow && !prevG && !worldCars.driving()) player.setNoclip(!player.noclip());
        prevG = gNow;
        // U = IDKFA hotkey (playtest aid): runs the console command of the same name
        // (god + full health + all weapons + unlimited ammo) so the cheat is one key
        // instead of opening the console. Gated on !consoleOpen so typing a 'u' into
        // the console never fires it.
        const bool uNow = keyDown(GLFW_KEY_U);
        if (uNow && !prevU && !consoleOpen) console->exec("idkfa");
        prevU = uNow;
        // F is FULLY FREED for PUNCH (see the melee block). E remains the vehicle
        // enter/exit key — it was always the primary; F was only an alternate exit,
        // so dropping it costs nothing and removes the punch/exit conflict.
        const bool fEdge = false;   // (was the F alt-exit edge; E covers exit)
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
                              !chatTrees.active() && !rpgUi.anyOpen() /* [W9-3 RPG] */ &&
                              gameUi.playing() &&
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

        // ---- W-MENU: F6 = the WORLD / PLACE DIRECTORY (also on the pause menu's
        // TRAVEL / WORLDS button). Full-screen, pauses the sim, Esc/F6 closes. Same
        // gating discipline as the world map. ----
        {
            const bool wNow = !consoleOpen && !termMode && !codeMode &&
                              !chatTrees.active() && !worldMapOpen && !rpgUi.anyOpen() &&
                              !riftConsoleOpen &&
                              rawKey(GLFW_KEY_F6);
            gameUi.setShowEditorRow(console->getInt("ui_editor") != 0);
            if (wNow && !prevWorldMenuKey) worldMenu.toggle();
            prevWorldMenuKey = wNow;

            // The pause menu's TRAVEL button routes here too: open the directory and
            // hand it the screen (the pause overlay steps aside).
            if (gameUi.wantWorldMenu()) {
                gameUi.clearWorldMenuRequest();
                gameUi.resumePlaying();     // the menu owns the screen now (it freezes the sim)
                worldMenu.open();
            }

            // ---- LEVEL EDITOR, FROM THE PAUSE MENU (dev row; cvar ui_editor) --------
            // The editor used to be BOOT-ONLY: --editor, or relaunch. That is a bad tool
            // — the moment you want to fix the room you are standing in, you have to quit
            // the game, and by the time you are back you are somewhere else.
            //
            // ImGui is LAZY-INITIALIZED here, the first time the editor is actually
            // opened. A player who never touches this row pays NOTHING for it: no ImGui
            // context, no descriptor pool, the shipping frame path byte-for-byte
            // unchanged (which is the property the boot-only gate existed to protect —
            // we keep the property and drop the restriction).
            if (gameUi.wantEditor()) {
                gameUi.clearEditorRequest();
                if (!device->editorUIActive() && window) {
                    device->initEditorUI(window);
                    x3::logInfo(device->editorUIActive()
                        ? "[editor] pause menu: ImGui overlay initialized (lazy)"
                        : "[editor] pause menu: ImGui overlay FAILED to init");
                }
                if (device->editorUIActive()) {
                    if (!editorInited) {
                        editorHost.init(*device, scene, *physics, window);
                        editorInited = true;
                    }
                    editorMode = true;
                    gameUi.resumePlaying();   // hand the screen to the editor
                    x3::logInfo("[editor] LEVEL EDITOR entered from the pause menu");
                }
            }
        }

        // ---- W-RIFT: [E] OPERATES A RIFT CONSOLE, IN THE CANON GAME LOOP. ----
        // The whole point of the consoles is that the player can re-aim and fire the
        // rifts in the REAL game, not only under `--world rifthub`. Walk to a gate's
        // hanging glass, press E. While it is open ALL input belongs to it (the cell-
        // terminal discipline: typing must never fire the weapon or move the player) —
        // the keyDown() gate above already enforces that.
        if (riftBuilt) {
            float rex, rey, rez, reyaw, repitch;
            player.camera(rex, rey, rez, reyaw, repitch);
            if (noclip) { rex = flyX; rey = flyY; rez = flyZ; }
            const bool eNow = !consoleOpen && !termMode && !codeMode && !worldMapOpen &&
                              !worldMenu.isOpen() && !chatTrees.active() &&
                              !rpgUi.anyOpen() && gameUi.playing() &&
                              rawKey(GLFW_KEY_E);
            if (eNow && !prevRiftUseKey) {
                if (rifthub.consoleOpen()) {
                    rifthub.closeConsole();
                } else if (!rifthub.catastrophe()) {
                    const int ci = rifthub.consoleInRange({ rex, rey, rez });
                    if (ci >= 0) {
                        rifthub.openConsole(ci);
                        x3::logInfo("[rift] console " + std::to_string(ci + 1) + " OPEN");
                    }
                }
            }
            prevRiftUseKey = eNow;
        }

        // The map consumes the mouse wheel for ZOOM while it is open (weapon
        // cycling below sees a zeroed delta).
        float mapWheel = 0.0f;
        if (worldMapOpen) { mapWheel = (float)g_weaponScroll; g_weaponScroll = 0.0; }

        // ---- [W9-3 RPG] I = BACKPACK screen, K = SKILL TREE screen (world-map
        // pattern: full-screen overlay, pauses the sim, gated off while any other
        // surface captures). Q = quick-use the equipped consumable while playing. ----
        {
            const bool rpgGate = !consoleOpen && !termMode && !codeMode &&
                                 !chatTrees.active() && !worldMapOpen &&
                                 gameUi.playing() && !terrainWorld;
            const bool iNow = rpgGate && rawKey(GLFW_KEY_I);
            const bool kNow = rpgGate && rawKey(GLFW_KEY_K);
            if (iNow && !prevInvKey)   rpgUi.toggleBackpack();
            if (kNow && !prevSkillKey) rpgUi.toggleSkills();
            prevInvKey = iNow; prevSkillKey = kNow;

            const bool qNow = rpgGate && !rpgUi.anyOpen() && rawKey(GLFW_KEY_Q);
            if (qNow && !prevQuickKey && inventory.quickSlot() >= 0) {
                const int qs = inventory.quickSlot();
                const x3::game::InvSlot& s = inventory.slot(qs);
                if (!s.empty()) {
                    if (const x3::game::ItemDef* d = itemDb.find(s.itemId)) {
                        if (rpgUseItem(*d)) inventory.removeAt(qs, 1);
                    }
                }
            }
            prevQuickKey = qNow;
        }

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
            // W2-A2: canon parity (game is unbuilt on canonlevel — R was dead there)
            // + audible reload when one actually STARTS (reload() returns false on
            // full mag / no reserve / already reloading).
            if (rNow && !prevReload &&
                (game.armed() || (canonWorld && canonPlay.armed()))) {
                if (arsenal.reload()) {
                    const x3::audio::SoundHandle rh = currentReloadSfx();
                    if (rh.valid()) audio->playSound2D(rh, 0.7f, 1.0f);
                }
            }
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

        // ---- WORLD CARS interact (runs BEFORE the E-edge use chain; per-frame
        // because the hold-E hack needs the HELD state, not just the edge).
        // Driving: E (or F) exits. Near a parked car: E enters an unlocked one;
        // a LOCKED one takes hold-E for 3 s (progress in the HUD hint line) and
        // unlocks permanently + fires the guarded alarm hook. A consumed E also
        // shuts the whole door/talk/terminal chain below for this frame. ----
        bool vehicleConsumedE = false;
        if (canonWorld && worldCars.built() && !codeMode && !termMode && !consoleOpen &&
            (!noclip || worldCars.driving()) &&   // idclip mid-drive must still allow the exit
            player.isAlive() && !chatTrees.active() && !npcDialog.active()) {
            vehicleConsumedE = worldCars.interact(
                player.feet(), eNow, eNow && !prevE, fEdge, dt, &player, *physics,
                audio.get());
        }

        if (eNow && !prevE && !terrainWorld && !vehicleConsumedE && !worldCars.driving()) {
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
                    // W5-3: Sarah's sink routes to the CLONE-GATED rescue; everyone
                    // else keeps the F5 midFloors path.
                    if (chatWho == "sarah") {
                        chatTrees.ctx().follow = [&canonPlay]() {
                            const x3::game::RescueVictim* s = canonPlay.sarah();
                            return s ? canonPlay.trySarahRescue(s->pos()) : false;
                        };
                    } else {
                        chatTrees.ctx().follow = [&midFloors]() {
                            const x3::game::RescueVictim* v = midFloors.victim();
                            return v ? midFloors.onRescue(v->pos()) : false;
                        };
                    }
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
                           // STAIRWELL PHANTOM DOORS (feat/secret-code-clues): open the
                           // keypad with the service-void bark — NEVER the generic
                           // "enter code N" prompt, which would print the code on the
                           // HUD. What a code does here is the stairwell's business
                           // (4545 answers, the master door alone opens on its own key).
                           if (stairwell.built() && stairwell.isPhantomDoorEntity(d->entity)) {
                               codeMode = true; keypad.clear();
                               npcBarkText = "SERVICE VOID - AUTHORIZED MAINTENANCE ONLY";
                               npcBarkTimer = 4.0f;
                               x3::logInfo("use: stairwell service-void keypad — type the code, Enter to submit");
                               return true;
                           }
                           auto cardName = [](int id){ return id == x3::game::kKeycardSecurity ? "Security" : "access"; };
                           const bool needCard = d->keycard != 0;
                           const bool hasCard  = needCard && (keycardMask & (1u << (uint32_t)d->keycard));
                           const bool needCode = d->code != 0;
                           if (d->requireBoth) {                       // need card AND code (Armory)
                               if (needCard && !hasCard) {
                                   npcBarkText = std::string("LOCKED - need the ") + cardName(d->keycard) + " keycard";
                                   npcBarkTimer = 3.0f; return true;
                               }
                               codeMode = true; keypad.clear();        // card ok -> enter the code
                               npcBarkText = std::string("Keycard OK - enter code ") + std::to_string(d->code);
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
                           npcBarkText = std::string("LOCKED - need the ") + cardName(d->keycard) + " keycard";
                           npcBarkTimer = 3.0f; return true;
                       }()) {
                // canon door interaction handled inside the lambda (toggle / unlock / keypad / message)
            } else if (canonWorld && canonFloor.valid() && !elevator.built() &&
                       [&]() -> bool {
                           // ---- W3-2 ELEVATOR TRAVEL — FALLBACK ONLY: the REAL cab
                           // (built in the lobby spine above) owns this interaction
                           // now; this teleport survives for data without lobbies /
                           // a failed cab build. Standing in any Elevator Lobby +
                           // E -> ride to the NEXT floor's lobby (wraps); the fast-
                           // travel blackout covers the teleport.
                           const uint32_t rm = canonFloor.roomAt(eye.x, eye.y, eye.z);
                           if (rm == x3::game::kNoRoom) return false;
                           if (canonFloor.rooms[rm].type != "Elevator Lobby") return false;
                           std::vector<uint32_t> lobbies;
                           for (uint32_t i = 0; i < (uint32_t)canonFloor.rooms.size(); ++i)
                               if (canonFloor.rooms[i].type == "Elevator Lobby") lobbies.push_back(i);
                           std::sort(lobbies.begin(), lobbies.end(), [&](uint32_t a, uint32_t b) {
                               return canonFloor.rooms[a].cy < canonFloor.rooms[b].cy;
                           });
                           if (lobbies.size() < 2) return false;       // single floor: nothing to ride
                           size_t cur = 0;
                           for (size_t i = 0; i < lobbies.size(); ++i) if (lobbies[i] == rm) { cur = i; break; }
                           const uint32_t tgt = lobbies[(cur + 1) % lobbies.size()];
                           const x3::game::CanonRoom& T = canonFloor.rooms[tgt];
                           player.setFeetPosition(*physics, x3::phys::Vec3{ T.cx, T.y0() + 0.3f, T.cz });
                           travelFadeT = 0.9f;                          // blackout cover
                           static x3::audio::SoundHandle sElevClose{}, sElevOpen{};
                           static bool sElevLoaded = false;
                           if (!sElevLoaded && audio) {
                               sElevClose = audio->load(x3::game::resolveAudio("doors/door_close.wav"));
                               sElevOpen  = audio->load(x3::game::resolveAudio("doors/door_open.wav"));
                               sElevLoaded = true;
                           }
                           if (audio) { audio->playSound2D(sElevClose, 0.8f, 1.0f);
                                        audio->playSound2D(sElevOpen, 0.6f, 0.92f); }
                           npcBarkText = std::string("ELEVATOR -> ") + T.name;
                           npcBarkTimer = 3.5f;
                           x3::logInfo("use: elevator travel -> " + T.name);
                           return true;
                       }()) {
                // elevator travel handled inside the lambda
            } else if (canonWorld && descMech.built() &&
                       [&]() -> bool {
                           // W9-1 desc-mechanics interact points (coolant console /
                           // EMP bench / master hack / antidote bench). Gated points
                           // surface their missing-requirement bark instead.
                           std::string dmBark;
                           if (!descMech.onUse(eye, &dmBark)) return false;
                           if (!dmBark.empty()) { npcBarkText = dmBark; npcBarkTimer = 4.5f; }
                           return true;
                       }()) {
                // desc-mechanics interact handled inside the lambda
            } else if (game.onUse(eye, dir, scene, *physics)) {  // plays door SFX internally
                x3::logInfo("use: button pressed — door opening");
            } else if (topFloors.onCollarStrike(eye)) {
                // F7 THE CLONE, phase 2: strike Sarah's NEURAL COLLAR. Active only
                // while the Clone is staggered/shielded; the third strike destroys it,
                // frees Sarah and mutates the Clone into its final form.
                const auto& col = topFloors.cloneFight().collar();
                npcBarkText = col.destroyed
                    ? std::string("NEURAL COLLAR DESTROYED — SARAH IS FREE")
                    : std::string("COLLAR CRACKING... ") + std::to_string(col.strikesLeft) + " LEFT";
                npcBarkTimer = 3.0f;
                x3::logInfo("use: F7 neural collar struck — strikes left " +
                            std::to_string(col.strikesLeft));
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
                // code, Enter submits to the sink -> the trapdoor opens on 1278).
                termMode = true; game.secret().terminal().setActive(true);
                // W4-2: one-time affordance so the player knows VIGIL answers here.
                if (!vigilHintShown) {
                    game.secret().terminal().addLine("");
                    game.secret().terminal().addLine("TYPE VIGIL TO SPEAK - OR ENTER OVERRIDE CODE");
                    vigilHintShown = true;
                }
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
            } else if (canonWorld && descMech.built() &&
                       [&]() -> bool {
                           // W9-1: held-item use — the E fell through every world
                           // handler, so spend it on the antidote (only while
                           // infected) or the EMP (discharges only when >= 1
                           // synthetic is in range; the charge is held otherwise).
                           std::string dmBark;
                           if (!descMech.onUseItem(eye, &dmBark)) return false;
                           if (!dmBark.empty()) { npcBarkText = dmBark; npcBarkTimer = 4.0f; }
                           return true;
                       }()) {
                // held-item use handled inside the lambda
            } else if (elevator.built()) {
                // Within ~4 m of the elevator shaft (XZ). Real elevator manners:
                // standing at a landing with the cab elsewhere, E SUMMONS it to the
                // caller's floor; riding it (or with the cab already here), E sends
                // it to the next stop. Carries the rider on the way.
                const x3::phys::Vec3 cc = elevator.cabCenter();
                const float ecx = eye.x - cc.x, ecz = eye.z - cc.z;
                if (ecx * ecx + ecz * ecz < 16.0f) {
                    const x3::phys::Vec3 pfe = physics->getBodyPosition(player.body());
                    const int   myStop  = elevator.nearestStopTo(pfe.y);
                    const bool  cabHere = std::fabs(cc.y - elevator.stopY(myStop)) < 0.5f;
                    if (!elevator.playerRiding(pfe) && !cabHere) {
                        elevator.callTo(myStop);
                        x3::logInfo("use: elevator SUMMONED to the caller's floor");
                    } else {
                        elevator.callNext();
                        x3::logInfo("use: elevator called");
                    }
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
            //
            // W4-2 fix (root cause of "VIGIL digit choices REJECTED"): this
            // proximity-cancel is for PHYSICAL NPC conversations (rescue girls /
            // Sarah), which chatTalkTarget() enumerates. The VIGIL terminal tree
            // ALSO rides chatTrees but has NO physical talk target, so chatTalkTarget
            // returns false and this cancel() used to deactivate the tree one frame
            // after the menu rendered — leaving vigilChat=true while chatTrees.active()
            // went false. The single-digit fast path (gated on chatTrees.active())
            // was then skipped, the digit fell through to the keypad code buffer, and
            // Enter submitted it as a code -> "> 1 [REJECTED]". VIGIL is bounded by
            // termMode + Esc + its own "(end session)" choice, not by walk range, so
            // exclude it here and let those paths close it.
            if (chatTrees.active() && chatTrees.activeNpc() != "vigil") {
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
                    // Gap C: the descent now ENDS somewhere — build the club once.
                    if (elevator.disco() && !club1127.built()) {
                        club1127.build(scene, *device, *physics, x3::game::riggedGlbRoot());
                        club1127Handoff = false;
                        x3::logInfo("CLUB 1127 awakens at The Deep (Y=-200) — ride it down");
                    }
                } else if (stairwell.built() &&
                           [&]() -> bool {
                               // STAIRWELL SERVICE CODE 4545 (feat/secret-code-clues):
                               // the phantom-door keypads ANSWER the taught code — as a
                               // denial. GREEN then AMBER on the pad; the door stays
                               // shut (the voids are sealed; the 4.5 seal is absolute).
                               // The unnumbered door answers differently: the chain
                               // link to the elevator + the chief engineer. Any other
                               // code falls through to the door machinery below (the
                               // owner's 7762 master key lives THERE, on one door only).
                               using CR = x3::game::FacilityStairwell::CodeResponse;
                               const CR r = stairwell.submitCode(
                                   x3::phys::Vec3{ pex, pey, pez },
                                   (int)keypad.value(), scene);
                               if (r == CR::NotHandled) return false;
                               npcBarkText = (r == CR::SublevelTell)
                                   ? "SUBLEVEL ACCESS VIA PRIMARY LIFT ONLY - SEE CHIEF ENGINEER"
                                   : "SERVICE VOID - NO ATMOSPHERE - ENTRY DENIED";
                               npcBarkTimer = 4.5f;
                               codeMode = false; keypad.clear();
                               return true;
                           }()) {
                    // stairwell keypad answered (lore beat; nothing opened)
                } else if (canonDoors.tryDoorCode(x3::phys::Vec3{ pex, pey, pez }, keypad.value()) ||
                    game.tryDoorCode(x3::phys::Vec3{ pex, pey, pez }, keypad.value()) ||
                    midFloors.tryDoorCode(x3::phys::Vec3{ pex, pey, pez }, keypad.value()) ||
                    topFloors.tryDoorCode(x3::phys::Vec3{ pex, pey, pez }, keypad.value()) ||
                    subLevels.tryDoorCode(x3::phys::Vec3{ pex, pey, pez }, keypad.value())) {
                    x3::logInfo("keypad: ACCEPTED — door opening");
                    // LIVING WORLD: a keypad override is a TERMINAL HACK stimulus —
                    // the security net notices doors being opened by code.
                    // (Canon scope: interior keypads only — roomAt gates it.)
                    if (facilityAlertOn &&
                        (!canonWorld || canonFloor.roomAt(pex, pey, pez) != x3::game::kNoRoom))
                        facilityAlert.reportTerminalHack(x3::phys::Vec3{ pex, pey, pez });
                    codeMode = false; keypad.clear();
                } else {
                    x3::logInfo("keypad: rejected");
                    // A WRONG code is even more suspicious (a tamper alarm).
                    // (Canon scope: interior keypads only — roomAt gates it.)
                    if (facilityAlertOn &&
                        (!canonWorld || canonFloor.roomAt(pex, pey, pez) != x3::game::kNoRoom))
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
            // W4-2 fix: reconcile a stale VIGIL flag. If the tree ended without the
            // flag being cleared (a desync path once left vigilChat=true while the
            // runner was inactive — the source of the phantom "CHOOSE 1-0" prompt),
            // close the link cleanly ONCE so digit input falls back to the keypad.
            if (vigilChat && !chatTrees.active()) {
                term.addLine("");
                term.addLine("VIGIL LINK CLOSED - TYPE VIGIL TO RECONNECT");
                term.trimBody(kTermMaxBody);
                vigilStop(term);
            }
            // KEYPAD CLICKS (the elevator's keypad treatment): every accepted
            // keystroke on the glass clicks — digits a touch brighter than letters,
            // backspace lower. Lazy-loaded once; invalid handle = silent, never a
            // crash (matches the elevator's graceful-cue rule).
            static x3::audio::SoundHandle sTermClick{};
            static bool sTermClickLoaded = false;
            if (!sTermClickLoaded && audio) {
                sTermClick = audio->load(x3::game::resolveAudio("interact/keypad_click.wav"));
                sTermClickLoaded = true;
            }
            auto keyClick = [&](float pitch) {
                if (audio && sTermClick.valid()) audio->playSound2D(sTermClick, 0.5f, pitch);
            };
            for (int dgt = 0; dgt < 10; ++dgt) {
                bool dn = rawKey(GLFW_KEY_0 + dgt) || rawKey(GLFW_KEY_KP_0 + dgt);
                if (dn && !tmDigitPrev[dgt]) {
                    // W4-2 fix: while a VIGIL choice menu is live, a single digit 1..N
                    // PICKS that choice immediately (edge-detected, no Enter) and is
                    // consumed HERE — before it reaches the keypad code buffer. Digits
                    // outside 1..N (and every digit when no menu is live) still buffer,
                    // so the multi-digit override code (1278) types normally once the
                    // conversation has ended (obtaining the code closes the link).
                    bool picked = false;
                    if (vigilChat && chatTrees.active() && dgt >= 1)
                        picked = vigilChoose(term, (uint32_t)(dgt - 1));
                    if (picked) keyClick(1.08f);
                    else        { term.pushChar((char)('0' + dgt)); keyClick(1.08f); }
                }
                tmDigitPrev[dgt] = dn;
            }
            // Letters + space too, so the cell terminal is a REAL typable field (not
            // digits-only). Uppercase to match the on-glass font. These use rawKey so
            // they register while keyDown (all gameplay input) is gated off in termMode.
            for (int li = 0; li < 26; ++li) {
                bool dn = rawKey(GLFW_KEY_A + li);
                if (dn && !tmCharPrev[li]) { term.pushChar((char)('A' + li)); keyClick(1.0f); }
                tmCharPrev[li] = dn;
            }
            bool tspaceNow = rawKey(GLFW_KEY_SPACE);
            if (tspaceNow && !tmSpacePrev) { term.pushChar(' '); keyClick(0.96f); }
            tmSpacePrev = tspaceNow;
            bool tbackNow = rawKey(GLFW_KEY_BACKSPACE);
            if (tbackNow && !tmBackPrev) { term.backspace(); keyClick(0.85f); }
            tmBackPrev = tbackNow;
            bool tEnterNow = rawKey(GLFW_KEY_ENTER) || rawKey(GLFW_KEY_KP_ENTER);
            if (tEnterNow && !tmEnterPrev) {
                // All-digit input = a keypad code attempt -> the EXISTING submit
                // chain (D14 fire-into-Lua + submitTerminalToScripts, plus the
                // mission-flag bridge). W4-2: inside a VIGIL conversation a SINGLE
                // digit picks a numbered choice first; longer digit strings stay
                // codes (1278 works mid-chat). VIGIL/HELLO/HELP/TALK starts the
                // scripted tree; other freeform -> the LLM, else scripted deflect.
                const std::string typed = term.input();
                const bool allDigits = !typed.empty() &&
                    (typed.size() <= 8 && std::all_of(typed.begin(), typed.end(),
                         [](unsigned char ch) { return std::isdigit(ch) != 0; }));
                const bool vigilSummon = !vigilChat &&
                    (typed == "VIGIL" || typed == "HELLO" || typed == "HELP" ||
                     typed == "TALK" || typed == "HI");
                if (vigilChat && allDigits && typed.size() == 1) {
                    // Enter fallback for a single digit. In-range digits are already
                    // consumed live by the digit loop above (no Enter needed); this
                    // path is reached only for an OUT-OF-RANGE digit that buffered —
                    // reply with the real range ONCE (no per-press stacking).
                    const uint32_t pick = (uint32_t)(typed[0] - '1');
                    term.clearInput();
                    if (!vigilChoose(term, pick)) {
                        const std::string prompt = "CHOOSE 1-" +
                            std::to_string(chatTrees.active() ? chatTrees.choices().size() : 0);
                        // Dedup: skip if the glass already ends with this exact prompt.
                        if (term.lines().empty() || term.lines().back() != prompt) {
                            termWrapOut(term, prompt);
                            term.trimBody(kTermMaxBody);
                        }
                    }
                } else if (vigilSummon) {
                    term.clearInput();
                    term.addLine("> " + typed);
                    if (chatTrees.hasNpc("vigil") && chatTrees.start("vigil", "terminal")) {
                        vigilChat = true;
                        term.setTextColor(1.00f, 0.72f, 0.32f);   // VIGIL presence: orange ink
                        vigilRender(term);
                    } else {
                        term.addLine(kVigilDegraded[(llmCannedIdx++) % kVigilDegradedN]);
                        term.trimBody(kTermMaxBody);
                    }
                    x3::logInfo("terminal: VIGIL tree " +
                                std::string(vigilChat ? "OPENED" : "unavailable"));
                } else if (const bool looksLikeCode = typed.empty() || allDigits; looksLikeCode) {
                    // D14: fire the entered code INTO Lua, then run the terminal's own
                    // submit sink — via submitTerminalToScripts() (shared with --test-hatch
                    // so the keypad->fire link is the SAME code the headless chain proves).
                    // Mission flag bridge: the entered code is condition substrate too
                    // ("code.<code>.entered") — mirrored BEFORE submit clears the line.
                    if (missionDocActive)
                        missionEvents.onEvent("terminal_code", {{"code", term.input()}});
                    bool ok = submitTerminalToScripts(scripts.get(), term);
                    if (ok) { termMode = false; term.setActive(false);
                              vigilStop(term);   // W4-2: end any live VIGIL chat cleanly
                              if (!vigilSawTrapdoor) {   // VIGIL crows about the hatch (if linked)
                                  vigilSawTrapdoor = true;
                                  vigilBarks.fire(x3::game::VigilEvent::Trapdoor, vigilClock);
                              }
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
                    if (!routed) {
                        // W4-2: modelless freeform gets a SCRIPTED deflect from
                        // VIGIL's banter pool (in-character), not the bare
                        // degraded stub; the stub remains the last resort.
                        const float roll = (float)((vigilBanterN++ * 61) % 97) / 97.0f;
                        const std::string d = chatTrees.pickBanter("vigil", roll);
                        if (!d.empty()) termWrapOut(term, "VIGIL: " + d);
                        else term.addLine(kVigilDegraded[(llmCannedIdx++) % kVigilDegradedN]);
                        if (vigilChat && chatTrees.active() &&
                            !chatTrees.choices().empty()) {
                            const std::string prompt = "CHOOSE 1-" +
                                std::to_string(chatTrees.choices().size()) +
                                " OR ASK FREELY";
                            if (term.lines().empty() || term.lines().back() != prompt)
                                termWrapOut(term, prompt);
                        }
                    }
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
            const bool drivingNow = canonWorld && worldCars.driving();
            if (!noclip && drivingNow) {
                // ---- WORLD CARS: at the wheel. The player capsule is stashed
                // (Player::update is skipped — it neither falls nor collides);
                // mouse-look integrates into the SAME player look angles so the
                // chase camera orbits and the view is continuous on exit. WASD
                // throttle/steer + Space brake = the drive host's mapping. ----
                if (firstSub) {
                    const float sens = 0.0025f;
                    player.setLook(player.yaw() + ddx * sens,
                                   player.pitch() - ddy * sens);
                }
                x3::phys::VehicleInput vin;
                vin.throttle = (keyDown(GLFW_KEY_W) ? 1.0f : 0.0f)
                             - (keyDown(GLFW_KEY_S) ? 1.0f : 0.0f);
                vin.steer    = (keyDown(GLFW_KEY_D) ? 1.0f : 0.0f)
                             - (keyDown(GLFW_KEY_A) ? 1.0f : 0.0f);
                if (spaceNow) vin.handBrake = 1.0f;
                // S against forward motion is the BRAKE (host_drive's rule).
                if (vin.throttle < 0.0f && worldCars.forwardSpeed() > 0.5f) {
                    vin.brake = 1.0f; vin.throttle = 0.0f;
                }
                carThrottleHud = vin.throttle;
                worldCars.driveInput(vin);
                worldCars.preStep(x3::net::kSimDt);
                // The world keeps simulating while at the wheel (no rider carry —
                // the capsule is stashed; mirrors the noclip branch's treatment).
                if (elevator.built()) elevator.update(x3::net::kSimDt, scene, *physics);
                if (liveStrataBuilt)
                    liveStrata.update(x3::net::kSimDt, scene, *device, elevator.cabCenter());
                if (club1127.built()) club1127.update(x3::net::kSimDt, scene, *device, *physics);
                physics->step(x3::net::kSimDt);
                scene.update(*physics);
                worldCars.postStep(x3::net::kSimDt);
            } else if (!noclip) {
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
                // W10 SWIMMING held channels (only read while in the swim state):
                // Space held = stroke up, Ctrl/C held = dive.
                in.jumpHeld = spaceNow;
                in.diveHeld = keyDown(GLFW_KEY_LEFT_CONTROL) || keyDown(GLFW_KEY_C);
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
                if (termMode || codeMode) { in.moveFwd = 0.0f; in.moveStrafe = 0.0f; in.sprint = false; in.jumpPressed = false; in.jumpHeld = false; in.diveHeld = false; }
                // CROUCH (hold C) / CRAWL (hold Left-Ctrl): lower the eye + slow the move.
                // Ctrl (prone) wins over C (crouch); release both to stand. Suppressed
                // while a console / terminal is open so typing doesn't duck the player,
                // and while SWIMMING (W10) — Ctrl/C mean DIVE there, not duck.
                if (!consoleOpen && !termMode && player.isAlive() && !player.swimming()) {
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
                    // Rider craft: an idle sealed car opens for an approaching player.
                    elevator.autoOpenFor(physics->getBodyPosition(player.body()));
                    // R11 (art): while you are INSIDE the sealed cab, the world's ambient
                    // wash has no business being in there — a lift interior is lit by its
                    // own fixture and nothing else. Drops ambient/IBL on entry, restores
                    // the world's values on exit. No-op when not aboard.
                    // B4: when the rider steps OFF, the elevator restores the world's air.
                    // In the CANON world the per-zone recipe — not the elevator — owns that
                    // air, and applyZoneAtmosphere is CHANGE-GATED, so without this the
                    // elevator's restore would stick until the player happened to cross a
                    // room boundary. Forget the cached zone so the recipe re-asserts on the
                    // next frame. (resetZoneAtmosphere() existed and was never called.)
                    if (elevator.applyCabAtmosphere(
                            *device, physics->getBodyPosition(player.body())) &&
                        !elevator.riderAboard() && canonWorld) {
                        canonRooms.resetZoneAtmosphere();
                    }
                }
                // R-3 fold: breathe the Crystal/Magma/Alien glow + flicker the magma
                // mood lights of the strata shaft the cab rides through (cab-biased).
                if (liveStrataBuilt)
                    liveStrata.update(x3::net::kSimDt, scene, *device, elevator.cabCenter());
                // Gap C: the club lives (ORB spin, spotlight orbit, blacklight pulse,
                // idle DJ/bouncer) + the ARRIVAL HANDOFF — cab parked at the club stop,
                // doors open, rider aboard -> step out at club.spawn(), once per descent.
                if (club1127.built()) {
                    club1127.update(x3::net::kSimDt, scene, *device, *physics);
                    const float clubCabY =
                        x3::game::ElevatorSystem::kDefaultClubFloorY + 0.15f;
                    x3::phys::Vec3 pr = physics->getBodyPosition(player.body());
                    if (!club1127Handoff && elevator.doorPct() > 0.9f &&
                        std::fabs(elevator.cabCenter().y - clubCabY) < 1.5f &&
                        elevator.playerRiding(pr)) {
                        const x3::phys::Vec3 cs = club1127.spawn();
                        physics->setBodyPosition(player.body(),
                                                 x3::phys::Vec3{ cs.x, cs.y + 1.0f, cs.z });
                        club1127Handoff = true;
                        x3::logInfo("WELCOME TO CLUB 1127 — The Deep");
                    }
                    // Re-arm for the next descent once the cab is back near the surface.
                    if (club1127Handoff && elevator.cabCenter().y > -50.0f)
                        club1127Handoff = false;
                }

                // ---- W-RIFT: SUB-LEVEL R1 lives ------------------------------------
                // The hub animates (membranes / core lights / consoles) and the approach
                // breathes only while the player is IN the region — the facility above
                // pays nothing for a room 78 m under it.
                if (riftBuilt) {
                    const x3::phys::Vec3 pfr = physics->getBodyPosition(player.body());
                    const bool inZone = riftInZone(pfr.x, pfr.y + 1.0f, pfr.z);
                    if (riftTeleCool > 0.0f) riftTeleCool -= x3::net::kSimDt;
                    if (inZone) {
                        rifthub.tick(x3::net::kSimDt, scene);
                        riftDepths.tick(x3::net::kSimDt);
                        // Walking into a gate ACTIVATES it (the kawoosh) — the hub's own
                        // trigger volumes, dispatched here exactly as the dev host does.
                        for (uint32_t tid : riftTriggers.update({ pfr.x, pfr.y + 1.6f, pfr.z }))
                            rifthub.onTrigger(tid);
                        // ---- THE PAYOFF: step through an OPEN rift and it TAKES YOU. ----
                        const int tp = (riftTeleCool <= 0.0f)
                            ? rifthub.traversalPortal({ pfr.x, pfr.y + 1.2f, pfr.z })
                            : -1;
                        if (tp >= 0) {
                            const std::string dest = rifthub.destination((uint32_t)tp);
                            const x3::game::Destination* dd = x3::game::findDestination(dest);
                            const std::string pretty = dd ? dd->name : dest;
                            x3::phys::Vec3 to{};
                            std::string why;
                            if (riftDestination(dest, to, &why)) {
                                physics->setBodyPosition(player.body(), to);
                                riftTeleCool = 3.0f;   // don't re-fire on the arrival frame
                                riftHudMsg = "RIFT TRAVERSED -> " + pretty;
                                riftHudTimer = 4.0f;
                                x3::logInfo("[rift] TRAVERSED rift " + std::to_string(tp + 1) +
                                            " -> " + pretty + " at (" + std::to_string(to.x) + ", " +
                                            std::to_string(to.y) + ", " + std::to_string(to.z) + ")");
                            } else if (dd && dd->worldFlag[0]) {
                                // NOT a lie and NOT a dead end: the place is real, it is just a
                                // WHOLE OTHER WORLD. Step through and the engine loads it — the
                                // same load-and-place the world menu's amber rows do.
                                hc.switchWorldTo = dd->worldFlag;
                                hc.switchDestKey = dd->key;
                                worldLoadRequested = true;
                                riftHudMsg = "RIFT -> LOADING " + pretty;
                                riftHudTimer = 4.0f;
                                x3::logInfo("[rift] rift " + std::to_string(tp + 1) +
                                            " -> " + pretty + ": no anchor here (" + why +
                                            ") — LOADING --world " + dd->worldFlag);
                            } else {
                                riftTeleCool = 2.0f;
                                riftHudMsg = "GATE HOLDS - " + pretty + ": " + why;
                                riftHudTimer = 4.0f;
                                x3::logWarn("[rift] rift " + std::to_string(tp + 1) +
                                            " points at '" + pretty + "' — " + why +
                                            "; the gate holds");
                            }
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
                    if (flyPitch >  1.5690f) flyPitch =  1.5690f;
                    if (flyPitch < -1.5690f) flyPitch = -1.5690f;
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
                if (club1127.built())
                    club1127.update(x3::net::kSimDt, scene, *device, *physics);
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
        // WORLD CARS: at the wheel the camera is the drive host's chase framing
        // around the live car (yaw/pitch stay the player's look angles, so the
        // view orbits with the mouse and is continuous on exit).
        if (!noclip && canonWorld && worldCars.driving())
            worldCars.driverCamera(camYaw, camPitch, camX, camY, camZ);
        // WEAPONS: apply + recover the weapon recoil kick. The kick is a transient
        // upward pitch offset added on top of the look pitch; it decays back to 0 so
        // the view recovers (recoil -> camera). Applied uniformly to setCamera, the
        // fire direction, the audio listener, and the viewmodel below.
        if (weaponRecoilPitch > 0.0f) {
            weaponRecoilPitch -= kRecoilRecover * dt;
            if (weaponRecoilPitch < 0.0f) weaponRecoilPitch = 0.0f;
        }
        camPitch += weaponRecoilPitch;
        if (camPitch >  1.5690f) camPitch =  1.5690f;   // keep within the look clamp (89.9 deg)

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
                               crouchedNow, prevFire,    // prevFire = last frame's held-fire
                               player.swimming());       // W10 3P read: prone + slow stroke
            x3::game::ThirdPersonCamera tc =
                thirdPerson.camera(pfeet, eyeH, camYaw, camPitch);
            // QA MAINLEVEL SWEEP — 3P camera wall clip: the orbit boom sits up to
            // ~3.6 m behind the player; in the facility's 3-5 m rooms backing into
            // a wall put the camera inside/behind the wall and the near plane cut
            // a hole in it (you saw the culled void). Clamp the boom against static
            // geometry: cast head -> desired camera, pull in to first hit - margin.
            {
                const x3::phys::Vec3 head{ pfeet.x, pfeet.y + eyeH, pfeet.z };
                const x3::phys::Vec3 dv{ tc.camX - head.x, tc.camY - head.y,
                                         tc.camZ - head.z };
                const float blen = std::sqrt(dv.x * dv.x + dv.y * dv.y + dv.z * dv.z);
                if (blen > 1e-4f) {
                    const x3::phys::Vec3 dn{ dv.x / blen, dv.y / blen, dv.z / blen };
                    const x3::phys::RayHit bh = physics->rayCast(
                        head, dn, blen + 0.25f, x3::phys::Layer::Static);
                    if (bh.hit) {
                        const float keep = std::max(bh.distance - 0.25f, 0.35f);
                        if (keep < blen) {
                            tc.camX = head.x + dn.x * keep;
                            tc.camY = head.y + dn.y * keep;
                            tc.camZ = head.z + dn.z * keep;
                        }
                    }
                }
            }
            // W10 3P + swim camera manners: while SWIMMING (not diving) the
            // orbit camera is clamped ABOVE the water surface — pitching up
            // would otherwise drag the render camera through the surface plane
            // into the underwater fog for a surface swim. Diving (eye genuinely
            // below the surface) keeps the underwater framing.
            if (player.swimming()) {
                const float eyeW = x3::game::worldWaterLevelAt(camX, camZ);
                const bool diving = eyeW > -1.0e30f && camY < eyeW - 0.25f;
                const float camW = x3::game::worldWaterLevelAt(tc.camX, tc.camZ);
                if (!diving && camW > -1.0e30f && tc.camY < camW + 0.30f)
                    tc.camY = camW + 0.30f;
            }
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
            // physics: the FPS-walk camera raycasts DOWN onto the blockout's static bodies.
            editorHost.tick(dt, emouse, ekbd, *device, physics.get());
        }
        // FLASHLIGHT (L toggles, default ON): re-issue the level's static ceiling
        // fixtures + a bright player-following light at the eye, so the dark halls
        // light up around you. Inserted FIRST so the 64-light cap never drops it.
        if (!terrainWorld) {
            bool lNow = keyDown(GLFW_KEY_L);
            if (lNow && !prevL) { flashlight = !flashlight;
                                  x3::logInfo(flashlight ? "flashlight ON" : "flashlight OFF"); }
            prevL = lNow;
            // r_flashlight 0 forces the torch OFF (headless lighting audits: a room must
            // be judged on its OWN practicals, never on the light riding the camera).
            if (console->getInt("r_flashlight") == 0) flashlight = false;
            // B4: nearest-to-eye, not first-in-array (see nearestFixtures above). This is
            // what actually turns the legacy tower's ceiling fixtures back on.
            //
            // LAND-LIGHTING — THIS IS THE CULL `fix/prim-point-light` FILED FOR. That branch
            // reported "level1 registers 337 point lights into a 64-light device cap — 273
            // dropped silently, first-come; needs a nearest-to-camera cull" and left it open,
            // still feeding the raw `= game.lightFixtures()`. `light/audit-facility` had
            // already BUILT that cull. So the two do not fight: prim-point-light diagnosed it,
            // the audit fixed it, and the fix is what ships. Do NOT restore the raw feed.
            std::vector<x3::rhi::PointLight> fl =
                nearestFixtures(game.lightFixtures(), camX, camY, camZ, fixtureBudget);
            // BLACK-PROP FIX (light routing). The F2-F7 west-wing dressing authors one
            // motivated KEY light per room, sitting right over that room's hero props
            // (beds / vats / crates / boardroom table). Until now those lights were only
            // uploaded by the --capture-wings dev tool, so in ACTUAL --world level1
            // gameplay the tower rooms leaned entirely on the flashlight + distant
            // ceiling fixtures and the metallic kit props read dark. Append the current
            // floor's wing keys here (floor-Y gated inside collectFloorLights, so only
            // the plate the player stands on contributes — fixtureBudget is 44 against a
            // 64-light device cap precisely so these, the elevator cab and the torch fit
            // in the headroom). Combined with the prop metallic clamp above the wing rooms
            // now read with their zone key in real play, not just captures.
            game.wingFloorLights(x3::phys::Vec3{ camX, camY, camZ }, fl);
            // The player's OWN light, kept aside: the rift (and the club) TAKE OVER the
            // whole pool with a clear(), which used to erase the flashlight along with the
            // facility fixtures. Whatever a room does to the budget, the torch comes back.
            std::vector<x3::rhi::PointLight> torch;
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
                // ---- 2026-07-12: THE FLASHLIGHT WAS THE SCENE -----------------------
                // Tim, live: the cell is blown-out WHITE with the flashlight on, and goes
                // "absolutely BLACK" the moment he turns it off. At 6.0 HDR over a 38 m
                // range this thing was a SUN riding 2 m in front of the player's face: it
                // was 4-5x brighter than any practical in the building, so (a) it bleached
                // whatever wall you walked up to and (b) killing it removed ~all of the
                // room's light, and the auto-exposure (aemax 2.2, ~0.7 s adaptation) needed
                // seconds to claw anything back — hence the black void.
                // A flashlight ADDS to a scene; it is not the scene. Cut to a real torch:
                // the forward pool 6.0 -> 3.30 over 20 m (was 38), the near fill 3.2 -> 1.7
                // over 8 m (was 13). That now sits at PARITY with a room's own key fixture
                // (the cell tube is 3.30 @ 6.2, level-1 fixtures run 3.6-4.2) — it reads as
                // a directed beam layered ON the practicals instead of erasing them.
                x3::rhi::PointLight pl{};
                pl.pos[0] = camX + fX * 2.0f; pl.pos[1] = camY + fY * 2.0f; pl.pos[2] = camZ + fZ * 2.0f;
                pl.range  = 20.0f;   // big soft circle; point attenuation gives the SOFT edge
                pl.color[0] = 3.30f; pl.color[1] = 3.10f; pl.color[2] = 2.75f;  // warm-white torch
                fl.insert(fl.begin(), pl);
                // Near light AT the eye so things RIGHT in front of you (barrels, enemies,
                // the held weapon) are still lit — a flashlight should never leave the near
                // field black. Smaller range, same warm-white.
                x3::rhi::PointLight eyePl{};
                eyePl.pos[0] = camX + fX * 0.3f; eyePl.pos[1] = camY + fY * 0.3f; eyePl.pos[2] = camZ + fZ * 0.3f;
                eyePl.range  = 8.0f;
                eyePl.color[0] = 1.70f; eyePl.color[1] = 1.60f; eyePl.color[2] = 1.40f;
                fl.insert(fl.begin(), eyePl);
                torch.push_back(eyePl); torch.push_back(pl);   // survives the rift/club takeover
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
                // SEAM 2: standing OUTDOORS (roomAt == kNoRoom, on the apron) the
                // flood seeds from the nearest room so the world never blanks; the
                // guard also keeps the breach room in the set so the interior seen
                // through the open breach never pops. Exterior entities themselves
                // are kNoRoom-tagged (always drawn).
                facilityExterior.ensureOutdoorVis(canonFloor, camX, camY, camZ, canonVisRooms);
                // SEAM 3 (risk 2): the streamed planet draws only when the eye can
                // plausibly see outdoors — standing OUTSIDE every room above ground,
                // or inside the breach/Entrance room (the view out the open breach
                // must not pop). Streamed entities carry kStreamedExteriorRoom, so
                // appending that id to the visible set is the whole draw gate; deep
                // interior frames never pay the city's draw cost, and the underground
                // (Y<0 shafts/club) never counts as "outdoors".
                if (canonStreamOn) {
                    const uint32_t eyeRoom = canonFloor.roomAt(camX, camY, camZ);
                    // Open-air test (W10 swimming): the river valley (water down
                    // to -9.9, bed -13) and the sea (-10, seafloor -80) are BELOW
                    // the old -2 cut yet are real outdoors — accept any eye above
                    // (local terrain - 15 m). The elevator/strata/club descent is
                    // under the facility pad (terrain ~0) so it still fails.
                    const bool openAir = camY > -2.0f ||
                        camY > x3::game::terrainHeightAtWorld(camX, camZ) - 15.0f;
                    if ((eyeRoom == x3::game::kNoRoom && openAir) ||
                        (eyeRoom != x3::game::kNoRoom &&
                         eyeRoom == facilityExterior.breachRoomHint()))
                        canonVisRooms.push_back(x3::game::kStreamedExteriorRoom);
                }
                // W-RIFT: sub-level R1 is submitted only from inside it.
                if (riftInZone(camX, camY, camZ))
                    canonVisRooms.push_back(x3::game::kRiftRoom);
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
                                                   camX, camY, camZ, fl,
                                                   canonLightBudget);
                // SEAM 2: the amber breach spill (the way-in read from the apron).
                if (facilityExterior.built()) fl.push_back(facilityExterior.spillLight());
            }
            // FROMDOC LIGHTING: append the LevelDoc's authored point lights (closest
            // 16 to the eye) after the flashlight, mirroring the canonlevel feed.
            if (docWorld && docLevel.built())
                docLevel.selectLights(camX, camY, camZ, fl, docLightK);
            // LIVING WORLD: LOCKDOWN red emissive shift — at alert level 3+ every
            // facility light leans hard into alarm red (the level-3 visual tell).
            // CANON SCOPE: the lockdown is a FACILITY effect — the shift applies
            // only while the eye is inside a tower room (roomAt != kNoRoom), so
            // street lamps / the strata / the golden-hour outdoors never redden.
            // (The canon room lights are already in `fl` here — the shift lands
            // on the whole assembled facility feed before the frame push, the
            // exact multiplier level1 uses.)
            if (facilityAlertOn && facilityAlert.redShift() > 0.0f &&
                (!canonWorld ||
                 canonFloor.roomAt(camX, camY, camZ) != x3::game::kNoRoom))
                applyAlertRedShift(fl, facilityAlert.redShift());
            // R-3 fold: while the eye is below the facility base (riding the shaft),
            // append the strata's breathing mood lights so the glowing depths light the
            // descending cab. Y-gated: above-ground lighting untouched; capped under
            // the device limit.
            if (liveStrataBuilt && camY < 2.0f) {
                const auto& sl = liveStrata.pointLights();
                size_t take = sl.size() < strataLightK ? sl.size() : strataLightK;
                for (size_t i = 0; i < take; ++i) fl.push_back(sl[i]);
            }
            // STREET LIGHT: the nearest K=14 lit lamps join the pool ONLY when
            // the streamed exterior is in frame (outdoors/at the breach — the
            // same kStreamedExteriorRoom gate the draw path uses). BUDGET SPLIT
            // (64-light device cap): flashlight + elevator cab FIRST, dressing
            // motivated lights at the front, <=16 canon room lights, the breach
            // spill, strata <=16 (camY<2 only), THEN street lamps <=14 — the
            // lamps are appended LAST so they can never starve the facility/
            // strata obligations, and the club takeover below still clears
            // everything at The Deep. Flicker ticks here (dt-scaled).
            if (canonWorld && streetLights.lampCount() > 0) {
                bool exteriorVis = false;
                for (uint32_t v : canonVisRooms)
                    if (v == x3::game::kStreamedExteriorRoom) { exteriorVis = true; break; }
                if (exteriorVis) {
                    if (!simFrozen) streetLights.update(dt, scene);
                    streetLights.selectLights(camX, camY, camZ, fl, streetLampK);
                }
            }
            // Gap C: at The Deep the club's own rig (neon/UV/orbit spots/bar fills)
            // takes the whole budget — no surface fixture reaches -200 m anyway.
            if (club1127.built() && camY < -150.0f) {
                fl.clear();
                const auto& cl = club1127.pointLights();
                for (const auto& L : cl) fl.push_back(L);
                // ATMOSPHERE (fix/club-relight): the canon club used to inherit the
                // FACILITY air (neutral ambient 0.030 + facility IBL) and read as a
                // dark void the same way --world club did. On ENTRY, lift the ambient
                // to the club's low VIOLET floor and raise the IBL fill so surfaces
                // the point lights miss are still readable (dim, not black). Edge-only
                // (clubAtmoOn) so it never re-bakes/spams per frame; the elevator's
                // cab-air edge owns the cab interior, so this only governs the room.
                if (!clubAtmoOn) {
                    clubAtmoOn = true;
                    device->setIblIntensity(0.40f);
                    device->setAmbient(0.045f, 0.035f, 0.070f);
                }
            } else if (clubAtmoOn) {
                // EXIT: hand the world's air back (the SAME values the elevator restores
                // on cab-exit, so the two owners agree instead of racing).
                clubAtmoOn = false;
                device->setIblIntensity(x3::game::kLevel1Ibl);
                device->setAmbient(x3::game::kLevel1Ambient[0], x3::game::kLevel1Ambient[1],
                                   x3::game::kLevel1Ambient[2]);
            }
            // W-RIFT: in SUB-LEVEL R1 the hub's rig (gate cores + keys + the hall) and
            // the approach's failing strips own the budget — same rule as the club: no
            // facility fixture reaches 78 m down, and the membranes are the key light.
            //
            // ---- 2026-07-12, FACILITY LIGHTING AUDIT: THE FLASHLIGHT DID NOT WORK IN THE
            // RIFT HUB. riftLights() begins with `out.clear()` — the hub's rig "owns the
            // budget". But `fl` at this point ALREADY HOLDS THE FLASHLIGHT (inserted at the
            // front, above). So walking into sub-level R1 SILENTLY DELETED THE PLAYER'S
            // TORCH, every frame, and pressing L did nothing at all: the one room in the
            // game you reach down a 33 m unlit approach corridor is the one room where your
            // flashlight is not connected to anything. The takeover is right; erasing the
            // player's own light with it is not. Re-insert the torch at the FRONT after the
            // takeover — and because riftLights() hands back a NEAREST-FIRST list, the 2
            // entries the 64-cap now trims off the tail are the two FARTHEST hub lights,
            // which is exactly what we would have chosen to drop.
            // (NOTE for the Descent/Club owner: `club1127` does the identical `fl.clear()`
            //  at camY < -150 and eats the flashlight the same way. Not touched here — that
            //  is another agent's region. Filed in the audit doc.)
            if (riftBuilt && riftInZone(camX, camY, camZ)) {
                riftLights(camX, camY, camZ, fl);
                for (auto it = torch.rbegin(); it != torch.rend(); ++it)
                    fl.insert(fl.begin(), *it);
            }
            // ---- UNDERWATER DIVER'S LIGHT (Tim: "add lights to illuminate
            // underwater"). The depth/shadow fix (24371e2) restored the sun that
            // filters down, but water absorbs it fast and the depths stay murky —
            // the giant squid sits at -56 m in near-black, and a shark circling
            // you at night is a shadow. So the water carries a diver's lamp: two
            // cool practicals biased AHEAD of the eye that light the fish, the bed,
            // and whatever is hunting you. Only while the eye is genuinely below a
            // water surface (nil cost on dry land); inserted at the FRONT so the
            // 64-light cap never trims them, and placed AFTER the club/rift
            // takeovers (which fl.clear()) so a deep dive is never left dark.
            {
                const float wY = x3::game::worldWaterLevelAt(camX, camZ);
                if (wY > -1.0e30f && camY < wY - 0.05f) {
                    const float cp = std::cos(camPitch);
                    const float fx = cp * std::cos(camYaw);
                    const float fy = std::sin(camPitch);
                    const float fz = cp * std::sin(camYaw);
                    // Near fill: a soft cool wash just ahead of the eye so the
                    // immediate surroundings (viewmodel, near fish) read.
                    x3::rhi::PointLight fill{};
                    fill.pos[0] = camX + fx * 2.0f;
                    fill.pos[1] = camY + fy * 2.0f;
                    fill.pos[2] = camZ + fz * 2.0f;
                    fill.range = 18.0f;
                    fill.color[0] = 1.20f; fill.color[1] = 1.70f; fill.color[2] = 2.05f;
                    // Throw: brighter, further along the look — see what you swim toward.
                    x3::rhi::PointLight beam{};
                    beam.pos[0] = camX + fx * 8.0f;
                    beam.pos[1] = camY + fy * 8.0f;
                    beam.pos[2] = camZ + fz * 8.0f;
                    beam.range = 24.0f;
                    beam.color[0] = 1.60f; beam.color[1] = 2.25f; beam.color[2] = 2.80f;
                    fl.insert(fl.begin(), fill);
                    fl.insert(fl.begin(), beam);
                }
            }
            if (fl.size() > lightBudget) fl.resize(lightBudget);
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
            // ---- SEAM 3: ONE budget umbrella per frame — terrain ground ring
            // first (measured), then the region streamer gets whatever is left
            // of --ws-budget. Player velocity feeds the lookahead so sprinting
            // off the apron pulls regions in earlier. Risk 3 (XZ-only residency
            // vs the underground): ALL residency work is suppressed while the
            // eye is below kStreamSuppressBelowY — the elevator/strata/Club-1127
            // descent holds the surface resident exactly as it was and the
            // streamer's Y=0 proxy floor can never drop a plane over The Deep.
            if (canonStreamOn) {
                if (camY > kStreamSuppressBelowY) {
                    const double st0 = glfwGetTime();
                    terrainStreamer.update(scene, *device, *physics, camX, camZ);
                    const double terrainMs = (glfwGetTime() - st0) * 1000.0;
                    float svx = dt > 1e-4f ? (camX - canonStreamPrevX) / dt : 0.0f;
                    float svz = dt > 1e-4f ? (camZ - canonStreamPrevZ) / dt : 0.0f;
                    // WORLD CARS: driving, the lookahead is fed the VEHICLE's
                    // real velocity (not the chase-cam delta) and the horizon is
                    // raised so regions land ahead at car speed (~20-30 m/s).
                    if (worldCars.driving()) {
                        float vv[3]; worldCars.chassisVelocity(vv);
                        svx = vv[0]; svz = vv[2];
                        canonWstream.setLookahead(std::max(hc.wsLookaheadS, 4.0f));
                    } else {
                        canonWstream.setLookahead(hc.wsLookaheadS);
                    }
                    canonWstream.update(scene, *device, *physics,
                                        camX, camY, camZ, svx, 0.0f, svz,
                                        (double)hc.wsBudgetMs, terrainMs);
                }
                canonStreamPrevX = camX; canonStreamPrevZ = camZ;
            }
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
            // FISH (W10 water): the schools swim, flee the player and float their
            // dead. Per-school range gate inside; the entities are PVS-gated on the
            // outdoor room, so an indoor player pays a handful of distance checks.
            if (worldFish.built() && !simFrozen) worldFish.update(dt, scene, camPos);
            // GOD RAYS: the sun shafts breathe + drift (additive-glass whisper;
            // ~16 entities, PVS-gated on the outdoor room like the fish).
            if (worldRays.built() && !simFrozen) worldRays.update(dt, scene);
            // FEET, not the eye: "in the water" is about where he is standing /
            // floating — a wading player's eye is above the surface.
            if (worldSea.built() && !simFrozen)
                worldSea.update(dt, scene, *device, *physics, player.feet(), &player);
            // THE WATER ZAP: the one-zap latch cooldown + the live surface discharge
            // (re-rolled arcs across the water for zapFxTimer seconds).
            if (!simFrozen) { waterZapper.tick(dt); tickZapFx(dt); }
            // LIVING NPCs: the canon room crowds + the streamed city crowds —
            // they work, they play, they talk. Updates are gated on the PVS
            // (roomVisible: a crowd in a culled room / an unseen outdoors costs
            // nothing); city systems exist only while the region is resident.
            for (auto& cc : canonCrowds)
                if (cc.built() && scene.roomVisible(cc.config().roomId))
                    cc.update(dt, scene);
            for (auto& cc : cityCrowds)
                if (cc.built() && scene.roomVisible(cc.config().roomId))
                    cc.update(dt, scene);
            // LIVING CITY (npc_life): the authored occupation NPCs + the bank-robbery
            // set-piece, riding the SAME streamed `city` region as the ambient crowds.
            // Built ONCE the first frame the region is resident — OUTSIDE any region
            // capture window, so the bodies/props/markers are host-owned + persistent
            // (never enter the region ledger) and simply PVS-cull with the district
            // (cfg.roomId == kStreamedExteriorRoom). No monster hitboxes: pure citizens.
            if (!cityNpcLife.built() && cityCrowds[0].built()) {
                x3::game::NpcLifeConfig lc;
                lc.centerX = -600.0f; lc.centerZ = 495.0f;   // the Scrapyard plaza drag
                float g[3]; x3::game::placeOnTerrain(lc.centerX, lc.centerZ, g);
                lc.groundY = g[1] + 0.20f;                    // stand on the plaza slab
                lc.roomId  = x3::game::kStreamedExteriorRoom;
                cityNpcLife.build(lc, scene, *device);
                cityNpcLife.setAlarmSink([](const x3::phys::Vec3& /*p*/, int heat){
                    // Outdoors never feeds the facility heat net (the app-run doctrine:
                    // the security AlertSystem is an indoor system). The heist still
                    // plays out visibly (cops converge, the robber flees) — log only.
                    x3::logInfo("[npc_life] BANK ALARM tripped (heat " +
                                std::to_string(heat) + ") — street cops converging");
                });
            }
            if (cityNpcLife.built() &&
                scene.roomVisible(x3::game::kStreamedExteriorRoom))
                cityNpcLife.update(dt, scene);
            // CROWD CHATTER: the voice layer rides the SAME PVS gate as its
            // crowd (a culled room's chat costs nothing and its bubbles just
            // age out). Audio murmurs are range-gated inside (<= ~20 m).
            for (int ci = 0; ci < 3; ++ci) {
                if (canonCrowds[ci].built() &&
                    scene.roomVisible(canonCrowds[ci].config().roomId))
                    canonChatter[ci].update(dt, canonCrowds[ci], audio.get(),
                                            chatterSnd, camPos);
                if (cityCrowds[ci].built() &&
                    scene.roomVisible(cityCrowds[ci].config().roomId))
                    cityChatter[ci].update(dt, cityCrowds[ci], audio.get(),
                                           chatterSnd, camPos);
            }
            // SKINNED CITIZENS: pose-follow the brains + drain the deferred
            // spawn queues (1 rig/frame; the layer PVS-gates its own pose work,
            // so a culled deployment costs only the queue check).
            for (int ci = 0; ci < 3; ++ci) {
                if (canonCrowds[ci].built())
                    canonCrowdSkins[ci].update(dt, canonCrowds[ci], scene, *device, *physics);
                if (cityCrowds[ci].built())
                    cityCrowdSkins[ci].update(dt, cityCrowds[ci], scene, *device, *physics);
            }
            // WORLD CARS while driving: (1) pedestrians within ~3.5 m of a car
            // moving at speed SCATTER (a cheap proximity probe, throttled, feeds
            // onViolence at the car pos); (2) deep river/sea water KILLS the
            // engine and forces an exit into the swim state; (3) the engine loop
            // pitches with the real RPM. All no-ops on foot.
            if (canonWorld && worldCars.built()) {
                if (worldCars.driving()) {
                    carPanicCooldown -= dt;
                    const float carSpd = std::fabs(worldCars.forwardSpeed());
                    if (carSpd > 4.0f && carPanicCooldown <= 0.0f) {
                        const x3::phys::Vec3 cp = worldCars.carPosition();
                        auto panicNear = [&](x3::game::CrowdSystem& cc) {
                            if (!cc.built()) return;
                            for (uint32_t ai = 0; ai < cc.agentCount(); ++ai) {
                                const auto& a = cc.agent(ai);
                                const float adx = a.pos.x - cp.x, adz = a.pos.z - cp.z;
                                if (adx * adx + adz * adz < 3.5f * 3.5f) {
                                    cc.onViolence(cp);
                                    return;
                                }
                            }
                        };
                        for (auto& cc : cityCrowds) panicNear(cc);
                        carPanicCooldown = 0.4f;
                    }
                    if (worldCars.inDeepWater()) {
                        worldCars.forceExit(&player, *physics);
                        carThrottleHud = 0.0f;
                    }
                }
                worldCars.updateAudio(audio.get(),
                                      worldCars.driving() ? carThrottleHud : 0.0f);
            }
            // LIVING WORLD: the FACILITY ALERT LEVEL — feed observations, apply
            // effects (reinforcements, lockdown doors). Lights/HUD read it below.
            if (facilityAlertOn) {
                // Observers: every live hostile is the facility's eyes and ears.
                x3::phys::Vec3 obs[32];
                uint32_t nObs = 0;
                bool seen = false;
                if (canonWorld && canonPlay.built()) {
                    // ---- CANON ARM (polish): the canon groups feed the SAME
                    // machine. Observers = every live canon hostile (all interior
                    // spawns); seen = any of them holding LOS; corpses = every
                    // downed canon hostile, SCOPE-GATED to the tower (roomAt !=
                    // kNoRoom) so a body floated down the river / dropped on the
                    // apron never feeds interior heat. ----
                    x3::game::CanonPlay::EnemyMark marks[32];
                    const uint32_t nm = canonPlay.liveEnemyMarks(marks, 32);
                    for (uint32_t i = 0; i < nm; ++i) obs[nObs++] = marks[i].pos;
                    seen = canonPlay.anyHostileLineOfSight();
                    canonPlay.forEachCorpse([&](const x3::phys::Vec3& cp) {
                        if (canonFloor.roomAt(cp.x, cp.y, cp.z) != x3::game::kNoRoom)
                            facilityAlert.registerCorpse(cp);
                    });
                } else {
                    x3::game::Level1Game::EnemyMark marks[32];
                    const uint32_t nm = game.liveEnemyMarks(marks, 32);
                    for (uint32_t i = 0; i < nm; ++i) obs[nObs++] = marks[i].pos;
                    // Player seen: any live guard holding LOS this frame.
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
                }
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
                    if (canonWorld && canonPlay.built() && canonFloor.valid()) {
                        // CANON ARM: queue through CanonPlay's deferred-spawn
                        // queue (drained 1/frame by the tickUpperSpawns budget
                        // below) — room-tagged, shared tunings, guards arrive
                        // through the nearest door of the player's room.
                        canonPlay.queueAlertReinforcements(
                            canonFloor, camPos, want, facilityAlert.level() >= 4);
                        x3::logInfo("alert: " +
                            std::string(x3::game::alertLevelName(facilityAlert.level()))
                            + " — queued " + std::to_string(want)
                            + " reinforcement(s) (1/frame deferred)");
                    } else {
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
                }
                // EFFECT: the level-3 zone-door LOCKDOWN (restores its own locks).
                // Canon: the SAME AlertDoorLock glue over canonDoors (a DoorSystem);
                // pre-existing locks (keycard/secured rooms) are never touched.
                alertDoorLock.update(facilityAlert,
                                     (canonWorld && canonFloor.valid()) ? canonDoors
                                                                        : game.doors());
            }

            // ---- VIGIL BARKS TICK (the ambient companion layer) --------------------
            // Runs every live frame. Master-gated on the vigilLink StoryFlag: SILENT
            // until Jake jacks in. Reads real game state and fires in-character one-
            // liners (with a cooldown / no-repeat / chatter setting) into the toast.
            {
                vigilClock += dt;
                // Sync tunables from the cvars every frame (cheap; console-live).
                const int chat = console->getInt("vigil_chatter");
                vigilBarks.setChatter(chat <= 0 ? x3::game::VigilChatter::Off
                                     : chat == 1 ? x3::game::VigilChatter::Occasional
                                                 : x3::game::VigilChatter::Chatty);
                vigilBarks.setCooldown(std::max(0.5f, console->getFloat("vigil_cooldown")));
                const bool linked = chatTrees.flags().has("vigilLink");
                vigilBarks.setEnabled(linked);

                // LINK ACQUIRED edge: VIGIL's first words in your head.
                if (linked && !vigilLinkPrev) {
                    vigilBarks.setLine(x3::game::VigilEvent::EnterArea,
                        "Oh, THERE you are - loud and clear, right between your ears. I could shout "
                        "through wall-screens like it's 1985, but this is so much cozier. You're "
                        "stuck with me now.", vigilClock);
                }
                vigilLinkPrev = linked;

                if (linked) {
                    // Movement / idle bookkeeping (idle timer resets on real motion).
                    if (vigilPrevPosSet) {
                        const float mdx = camPos.x - vigilPrevPos.x;
                        const float mdz = camPos.z - vigilPrevPos.z;
                        const float mdy = camPos.y - vigilPrevPos.y;
                        if (mdx*mdx + mdz*mdz + mdy*mdy > 0.02f) vigilBarks.noteActivity();
                    }
                    vigilPrevPos = camPos; vigilPrevPosSet = true;

                    // ALERT edges (rising / all-clear) + first-ever combat.
                    const int lvl = facilityAlert.level();
                    if (facilityAlertOn) {
                        if (lvl >= 1 && !vigilSawCombat) {
                            vigilSawCombat = true;
                            vigilBarks.fire(x3::game::VigilEvent::FirstCombat, vigilClock);
                        } else if (lvl > vigilPrevAlert && lvl >= 1) {
                            vigilBarks.fire(x3::game::VigilEvent::AlertRising, vigilClock);
                        } else if (lvl == 0 && vigilPrevAlert > 0) {
                            vigilBarks.fire(x3::game::VigilEvent::AlertClear, vigilClock);
                        }
                        vigilPrevAlert = lvl;
                    }

                    // LOW HEALTH (re-arms only after recovering above half).
                    const int hp = player.hp(), mx = player.maxHp();
                    if (mx > 0) {
                        if (!vigilLowHpLatch && hp > 0 && hp < mx * 3 / 10) {
                            vigilLowHpLatch = true;
                            vigilBarks.fire(x3::game::VigilEvent::LowHealth, vigilClock);
                        } else if (hp > mx / 2) vigilLowHpLatch = false;
                    }

                    // PICKUP SIDEARM (canon world's first weapon).
                    if (!vigilSawSidearm && canonPlay.built() && canonPlay.armed()) {
                        vigilSawSidearm = true;
                        vigilBarks.fire(x3::game::VigilEvent::PickupSidearm, vigilClock);
                    }

                    // ENTER ELEVATOR (near the cab) / ENTER CLUB (bottom of the Spire).
                    if (!vigilSawElevator && elevator.built()) {
                        const x3::phys::Vec3 cc = elevator.cabCenter();
                        const float edx = camPos.x - cc.x, edz = camPos.z - cc.z;
                        if (edx*edx + edz*edz < 2.6f * 2.6f) {
                            vigilSawElevator = true;
                            vigilBarks.fire(x3::game::VigilEvent::EnterElevator, vigilClock);
                        }
                    }
                    if (!vigilSawClub &&
                        camPos.y < x3::game::ElevatorSystem::kDefaultClubFloorY + 12.0f) {
                        vigilSawClub = true;
                        vigilBarks.fire(x3::game::VigilEvent::EnterClub, vigilClock);
                    }

                    // ENTER AREA (canon room change).
                    if (canonWorld && canonFloor.valid()) {
                        const int room = (int)canonFloor.roomAt(camPos.x, camPos.y, camPos.z);
                        if (room != vigilPrevRoom && room != (int)x3::game::kNoRoom &&
                            vigilPrevRoom != -999)
                            vigilBarks.fire(x3::game::VigilEvent::EnterArea, vigilClock);
                        vigilPrevRoom = room;
                    }

                    // BOREDOM: advance the idle timer; fires an Idle bark when stalled.
                    vigilBarks.update(dt, vigilClock);
                }
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
                // Stairwell keypad flash sequencing + the master door's
                // auto-close/re-lock (4.5 never sits propped open).
                if (stairwell.built())
                    stairwell.update(dt, scene, &canonDoors, camPos);
                // SECURITY KEYCARD: grab it by walking up to it (proximity, XZ).
                if (!canonKeycardTaken && canonKeycardEnt != x3::game::kNoLink) {
                    const float kdx = camPos.x - canonKeycardX, kdz = camPos.z - canonKeycardZ;
                    if (kdx * kdx + kdz * kdz < 1.6f * 1.6f) {
                        canonKeycardTaken = true;
                        keycardMask |= (1u << (uint32_t)x3::game::kKeycardSecurity);
                        // [W9-3 RPG] the card now lives IN THE BAG (key section) —
                        // the door gate reads the bag-backed mask; the HUD/backpack
                        // shows what you hold.
                        if (const x3::game::ItemDef* kd = itemDb.find("keycard_security")) {
                            inventory.add(*kd, 1);
                            keycardMask |= inventory.keycardMask(itemDb);
                        }
                        if (canonKeycardEnt < scene.size()) scene.get(canonKeycardEnt).visible = false;
                        npcBarkText = "Security keycard -> BACKPACK [I]"; npcBarkTimer = 3.0f;
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
                // Task #4: drain the deferred F2-F7 squad queue, one enemy per frame
                // (~15-25 ms each — bounded, and the player is floors away in the cell).
                canonPlay.tickUpperSpawns(canonFloor, scene, *device, *physics, 1);
                canonPlay.tick(dt, scene, *physics, camPos, &player, enemyAttackFx);
                // CANON ALIENS: the planet aliens live in the same combat lane —
                // hostile rows patrol/chase/attack the player (same damage sink +
                // attack FX as the canon enemies); the allied Nordic just watches.
                canonAliens.update(dt, scene, *physics, camPos, &player, enemyAttackFx);
                // W5-1: the Nexus Chamber — whispers / the name-call / apex wake /
                // cavern creatures. Creature-bucket vocals stand in for VO (none in
                // the packs); pitched low they read as The Chorus murmuring.
                canon45.update(dt, scene, *physics, camPos, &player, enemyAttackFx,
                               audio.get(), bootAudio.spTaunt[1], bootAudio.spDeath[1]);
                canonDressing.tick(dt);   // advance the flickering cell-tube phase
                canonBarrels.update(dt);  // WAVE (cell-door): step destructibles + detonate any barrel shot this frame
                // ---- W9-1: desc-mechanics per frame — cold-room dwell/chill,
                // decontamination cure, pickup->flag polling, DoT ticks (damage
                // lands through player.takeDamage so the pain cue fires free).
                if (descMech.built()) {
                    descMech.tick(dt, camPos, &player);
                    // Late-arriving sabotage flag (a loaded save): re-apply the
                    // Collective multiplier; the console glow dies on either edge.
                    if (chatTrees.flags().has("f4.coolant_sabotaged") &&
                        !canonPlay.coolantSabotaged())
                        canonPlay.applyCoolantSabotage();
                    if (!coolantGlowDead &&
                        (descMech.coolantGlowKillPending() || canonPlay.coolantSabotaged())) {
                        x3::game::killRoomGlow(canonLights, descMech.coolantRoom());
                        coolantGlowDead = true;
                    }
                    // Queued barks (entry warnings / infection / decon) ride the
                    // npcBark line; the "[E] ..." prompt holds the line while
                    // it's otherwise free (full alpha in range, 1 s fade out).
                    for (std::string b = descMech.takeBark(); !b.empty(); b = descMech.takeBark()) {
                        npcBarkText = b; npcBarkTimer = 4.5f;
                    }
                    const std::string dmPrompt = descMech.prompt(camPos);
                    if (!dmPrompt.empty() && (npcBarkTimer <= 0.0f || npcBarkText == dmPrompt)) {
                        npcBarkText = dmPrompt; npcBarkTimer = 1.0f;
                    }
                }
                g_perf.tick += glfwGetTime() - _pt0;
                // ---- W4-1: rescue story flags + the extraction goodbye. freed = she
                // became a Companion (E-rescue); extracted = she reached the F2 elevator
                // lobby and left the level. Flags gate later content (chat trees /
                // objectives); the goodbye rides the existing NPC bark line. ----
                {
                    static const char* kGirlKey[3] = { "aria", "keisha", "emily" };
                    const auto& rs = canonPlay.rescue();
                    for (uint32_t gi = 0; gi < rs.victimCount() && gi < 3; ++gi) {
                        const auto& v = rs.victim(gi);
                        const std::string freed = std::string("girl.freed.") + kGirlKey[gi];
                        if ((v.companion() || v.extracted()) && !chatTrees.flags().has(freed)) {
                            chatTrees.flags().set(freed);
                            // [W9-3 RPG] rescue XP (once per girl, latched by the flag).
                            if (progression.addXp(x3::game::kXpRescue) > 0)
                                rpgUi.notifyLevelUp(progression.level());
                        }
                    }
                    const uint32_t ei = rs.extractedThisFrame();
                    if (ei != UINT32_MAX && ei < 3) {
                        chatTrees.flags().set(std::string("girl.extracted.") + kGirlKey[ei]);
                        npcBarkText  = rs.victim(ei).name() +
                                       ": Thank you, Jake. Find Sarah — end this place.";
                        npcBarkTimer = 5.0f;
                        x3::logInfo("[canon] " + rs.victim(ei).name() +
                                    " extracted at the F2 elevator (story flag set)");
                    }
                    // W5-2 wiring: the frame an interrupt tier resolves, record it in the
                    // dialog world. WOUNDED sets `<girl>.interrupted` — the raw-vs-composed
                    // tree variants that were authored long ago become reachable — plus a
                    // hurting bark so the moment lands. (Clean resolves silently: her own
                    // rescue dialog carries it.)
                    const uint32_t ti = rs.tierResolvedThisFrame();
                    if (ti != UINT32_MAX && ti < 3 &&
                        rs.victim(ti).tier() == x3::game::RescueTier::Wounded) {
                        chatTrees.flags().set(std::string(kGirlKey[ti]) + ".interrupted");
                        npcBarkText  = rs.victim(ti).name() + ": I... I'm okay. Just... get me out.";
                        npcBarkTimer = 5.0f;
                        x3::logInfo("[canon] " + rs.victim(ti).name() +
                                    " rescue resolved WOUNDED (interrupted flag set)");
                    }
                }
                // ---- W5-3: THE ENDGAME SPINE — flags, objectives, and the WIN. ----
                {
                    static bool winFired = false;
                    // Clone down -> the field's key is dead. Flag drives Sarah's tree
                    // branch (fm_gate); objective points the player back to her.
                    if (canonPlay.cloneDefeated() && !chatTrees.flags().has("clone.defeated")) {
                        chatTrees.flags().set("clone.defeated");
                        npcBarkText  = "VIGIL: SUCCESSOR UNIT TERMINATED. HOLDING-FIELD KEY... INVALID.";
                        npcBarkTimer = 6.0f;
                        game.objectives().setText("THE FIELD IS DOWN - GET BACK TO SARAH");
                        x3::logInfo("[endgame] clone defeated — Sarah's containment key is dead");
                        // [W9-3 RPG] boss-objective XP (once, latched by the flag).
                        if (progression.addXp(x3::game::kXpBoss) > 0)
                            rpgUi.notifyLevelUp(progression.level());
                    }
                    // Freed (her tree's follow fx fired) -> point at the roof.
                    if (canonPlay.sarahPresent() && canonPlay.sarah()->companion() &&
                        !chatTrees.flags().has("sarah.freed.sync")) {
                        chatTrees.flags().set("sarah.freed.sync");
                        chatTrees.flags().set("sarah.freed");   // belt + braces with the tree fx
                        game.objectives().setText("GET SARAH TO THE HELIPAD");
                        x3::logInfo("[endgame] Sarah is with you — objective: the helipad");
                    }
                    // THE WIN: she stands on the helipad. One-shot.
                    if (!winFired && canonPlay.sarahExtractedThisFrame()) {
                        winFired = true;
                        chatTrees.flags().set("sarah.extracted");
                        int saved = 1;   // Sarah
                        for (const char* k : { "girl.extracted.aria", "girl.extracted.keisha",
                                               "girl.extracted.emily" })
                            if (chatTrees.flags().has(k)) ++saved;
                        winLine2 = "RESCUED: " + std::to_string(saved) + " OF 4";
                        winTimer = 12.0f;
                        game.objectives().setText("TO BE CONTINUED");
                        // [W9-3 RPG] the win XP (one-shot with winFired).
                        if (progression.addXp(x3::game::kXpWin) > 0)
                            rpgUi.notifyLevelUp(progression.level());
                        x3::logInfo("[endgame] WIN — Sarah extracted (" + winLine2 + ")");
                    }
                }
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
            // R-3 fold: dispatch the strata shaft's triggers (descent entered, per-band
            // offshoots, ClubArrival at Y=-200) as the player rides/walks down. Idempotent.
            if (liveStrataBuilt)
                for (uint32_t tid : liveStrataTriggers.update(camPos)) liveStrata.onTrigger(tid);
            // Floor 4.5 Nexus / The Chorus: dispatch its connector (which discovers +
            // arms the Chorus) then tick the multi-pod boss (inert until armed).
            for (uint32_t tid : nexusTriggers.update(camPos)) nexus.onTrigger(tid);
            nexus.tick(dt, scene, *physics, camPos, &player, enemyAttackFx);
            // Hidden Floor-7 sub-levels: GATE the hidden descent on the F7 finale being
            // complete (the Clone boss is dead AND Sarah was saved). openDescent() is a
            // one-way no-op until BOTH hold; reading spire_top here is READ-ONLY. Once the
            // descent opens, dispatch its triggers (hidden lift + per-sub-level hubs, the
            // SL3 hub starts Chen's clock) and tick the sub-level encounters + hazard.
            // THE CLONE's own latch is the authoritative "Clone dead" signal now
            // (CloneBossFight fires it the frame the boss falls); the aliveCount()
            // test is kept as the belt-and-braces equivalent it always was.
            const bool cloneFallen =
                topFloors.plan(x3::game::SpireTopFloor::F7).hasBoss &&
                (topFloors.cloneDead() || topFloors.boss().aliveCount() == 0);
            // Breaking Sarah's neural collar (the Clone fight's phase-2 gate) frees
            // her — the SECOND descent-gate input, alongside the manual E rescue.
            if (topFloors.sarahFreed()) sarahSaved = true;
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
        // W5-4: the ambient bed (room tone + fluorescent buzz) is now started ONCE,
        // above, as real seamless loop channels (ambRoomLoop/ambBuzzLoop) — no more
        // per-frame retrigger here. Also the fog/grade LIVE OVERRIDE cvars (AD-1's
        // paste-block): any r_fog*/r_grade* >= 0 re-applies the canon zone params
        // with that field overridden (console-tunable without a rebuild).
        if (canonWorld) {
            // WAVE-3 zone atmosphere: re-tint the depth fog as the player crosses zone
            // boundaries (teal halls / amber detention / green labs). Runs BEFORE the
            // cvar overrides below so an explicit r_fog* setting still wins this frame.
            // R11: the ELEVATOR CAB owns the air while you are riding it (a sealed steel
            // box is lit by its own fixture, not by whatever floor it happens to be
            // passing). Yield to it — and force a re-apply on the frame the rider steps
            // back out, because applyZoneAtmosphere is change-gated and would otherwise
            // never notice the cab had overwritten ambient/IBL underneath it.
            static bool s_prevAboard = false;
            const bool cabAir = elevator.built() && elevator.riderAboard();
            if (!cabAir && s_prevAboard) canonRooms.resetZoneAtmosphere();
            s_prevAboard = cabAir;
            if (canonFloor.valid() && !cabAir)
                canonRooms.applyZoneAtmosphere(*device, canonFloor.roomAt(camX, camY, camZ));
            const float cvFogD = console->getFloat("r_fogdensity");
            const float cvFogS = console->getFloat("r_fogstart");
            const float cvGrd  = console->getFloat("r_gradestrength");
            const float cvVig  = console->getFloat("r_vignette");
            if (cvFogD >= 0.0f || cvFogS >= 0.0f) {
                x3::rhi::IRenderDevice::FogParams f;   // canon detention zone values (ART_BIBLE)
                f.enabled = true;
                f.color[0] = 0.045f; f.color[1] = 0.040f; f.color[2] = 0.034f;
                f.density = (cvFogD >= 0.0f) ? cvFogD : 0.0035f;
                f.start   = (cvFogS >= 0.0f) ? cvFogS : 1.2f;
                f.maxOpacity = 0.60f;
                device->setFog(f);
            }
            if (cvGrd >= 0.0f || cvVig >= 0.0f) {
                x3::rhi::IRenderDevice::GradeParams g; // canon detention zone values (ART_BIBLE)
                g.strength = (cvGrd >= 0.0f) ? cvGrd : 0.85f;
                g.shadowTint[0] = 0.94f; g.shadowTint[1] = 1.00f; g.shadowTint[2] = 1.03f;
                g.highlightTint[0] = 1.04f; g.highlightTint[1] = 1.00f; g.highlightTint[2] = 0.95f;
                g.saturation = 0.96f;
                g.vignette = (cvVig >= 0.0f) ? cvVig : 0.10f;
                device->setGrade(g);
            }
            // ---- W-RIFT: SUB-LEVEL R1's air. The hub's atmosphere is a single knob
            // that lives with its art (Rifthub::applyAtmosphere: cold blue haze, teal
            // grade, low ambient, interior IBL). Applied while the eye is in the region
            // and RELEASED on the way out — resetZoneAtmosphere() makes the room recipes
            // re-apply their own fog next frame (the same handoff the swim path uses).
            {
                const bool inRift = riftInZone(camX, camY, camZ);
                if (inRift) {
                    rifthub.applyAtmosphere(*device);
                } else if (riftZonePrev) {
                    canonRooms.resetZoneAtmosphere();
                    device->setIblIntensity(0.5f);                       // the SEAM-2 interior value
                    device->setExposure(console->getFloat("r_exposure"));
                }
                riftZonePrev = inRift;
            }
            // ---- W10 SWIMMING: the UNDERWATER read. When the CAMERA is below a
            // water surface (river reach or the sea), override the frame's fog
            // with a dense blue-green extinction — applied AFTER the zone/cvar
            // fog so it wins while submerged. On surfacing, resetZoneAtmosphere()
            // makes the room recipes re-apply their own fog next frame (they own
            // it; nothing is clobbered, no FogParams snapshot needed).
            {
                const float wY = x3::game::worldWaterLevelAt(camX, camZ);
                const bool camUnder = wY > -1.0e30f && camY < wY - 0.05f;
                if (camUnder) {
                    x3::rhi::IRenderDevice::FogParams uf;
                    uf.enabled  = true;
                    uf.color[0] = 0.020f; uf.color[1] = 0.095f; uf.color[2] = 0.110f;
                    uf.density  = 0.055f;    // ~18 m visibility — murky river water
                    uf.start    = 0.15f;     // hands/viewmodel stay readable
                    uf.maxOpacity = 0.94f;
                    device->setFog(uf);
                } else if (prevCamUnderwater) {
                    canonRooms.resetZoneAtmosphere();   // recipe fog re-applies next frame
                }
                prevCamUnderwater = camUnder;
                // UNDERWATER CAUSTICS (mesh.frag, setCaustics): dancing sun
                // filaments on everything sunlit below the local water surface.
                // Enabled whenever the camera stands over a WATER COLUMN (river
                // reach or sea — swimming, wading, or looking down through the
                // surface); the shader gates per-fragment on being below wY, so
                // dry land is untouched even mid-swim. Water is treated as
                // locally flat (the river is flat per-reach, the sea is flat).
                if (!simFrozen) causticsClock += dt;
                {
                    x3::rhi::IRenderDevice::CausticsParams cw;
                    cw.enabled   = wY > -1.0e30f;
                    cw.waterY    = cw.enabled ? wY : 0.0f;
                    cw.time      = causticsClock;
                    cw.intensity = 1.0f;
                    device->setCaustics(cw);
                }
            }
        }
        // (W10: no footsteps while swimming — bed contact is not a floor walk.)
        if (prevCamValid && !noclip && player.grounded() && !player.swimming() && dt > 0.0f) {
            const float dxc = camX - prevCamX, dzc = camZ - prevCamZ;
            const float speed = std::sqrt(dxc * dxc + dzc * dzc) / dt; // m/s
            if (speed > 0.6f) {
                // Cadence scales a little with speed (faster -> quicker steps).
                const float kStepInterval = (speed > 6.5f) ? 0.32f : 0.45f;
                stepTimer += dt;
                if (stepTimer >= kStepInterval) {
                    stepTimer = 0.0f;
                    // W2-A2: cycle the 4 real concrete takes (subtle pitch wobble
                    // keeps repeats alive). Legacy pitched-gunshot only if none loaded.
                    static uint32_t stepIdx = 0;
                    const x3::audio::SoundHandle tk =
                        bootAudio.footstepConcrete[stepIdx++ & 3u];
                    if (tk.valid())
                        audio->playSound2D(tk, 0.30f, 0.96f + 0.04f * (float)(stepIdx & 1u));
                    else
                        audio->playSound2D(sndStep, 0.22f, 0.55f);
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
                               chatTrees.active() || worldMapOpen ||
                               rpgUi.anyOpen();   // [W9-3 RPG] no firing through the backpack
        // PUNCH: F is the primary key (2026-08; F used to be the noclip test toggle,
        // now on G). V and MMB are kept as aliases so existing muscle memory and the
        // startup-log contract still work.
        bool meleeNow = !uiCapture && (keyDown(GLFW_KEY_F) || keyDown(GLFW_KEY_V) ||
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
                const x3::phys::Vec3 muzzle = weaponMuzzle(arsenal, *console, camX, camY, camZ, camYaw, camPitch);
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
        // ---- W10 polish: swim host edges. The surface-exit splash take on the
        // swim->dry falling edge (item 2; the ENTRY splash rides the Player's
        // own PlayerSplash cue), the smoothed viewmodel-LOWER blend (item 4 —
        // dt-scaled ease in/out, applied at the viewmodel draw), and the
        // refusal-click cadence timer. ----
        {
            const bool swimmingNow = player.swimming();
            if (prevSwimming && !swimmingNow && audio && sndSplashExit.valid())
                audio->playSound2D(sndSplashExit, 0.5f, 1.0f);
            prevSwimming = swimmingNow;
            // The LIGHTNING gun does NOT lower in the water — it is the one weapon
            // that fires there (THE WATER ZAP), so it must stay in the frame.
            const float target = (swimmingNow && arsenal.current().name != "lightning")
                                     ? 1.0f : 0.0f;
            const float kBlend = 1.0f - std::exp(-8.0f * std::max(0.0f, (float)dt));
            swimVmAmt += (target - swimVmAmt) * kBlend;
            if (swimVmAmt < 1e-4f) swimVmAmt = 0.0f;
            if (swimVmAmt > 0.9999f) swimVmAmt = 1.0f;
            if (swimFireDenyCooldown > 0.0f) swimFireDenyCooldown -= (float)dt;
        }
        bool fireHeld = !uiCapture && !simFrozen && !worldCars.driving() &&
                        glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
        bool wantFire = arsenal.current().automatic ? fireHeld : (fireHeld && !prevFire);
        // In --world canonlevel the legacy `game` is unbuilt; the canon sidearm gates firing.
        const bool playerArmed = game.armed() || (canonWorld && canonPlay.armed());
        // CHARGE weapon (Lightning): mark the beam HELD so Arsenal::tick drains its
        // charge pool continuously (~10/s) while fire is held (IDKFA bypasses drain).
        // NOT while swimming — the weapon is lowered, so it neither fires nor charges.
        // THE WATER ZAP EXCEPTION: the LIGHTNING gun is the one weapon that still
        // works in the water (it is the whole joke — and it hurts). Every other
        // weapon keeps the lowered/refused manners.
        const bool lightningHeld = (arsenal.current().name == "lightning");
        arsenal.setBeamHeld(fireHeld && playerArmed && player.isAlive() &&
                            (!player.swimming() || lightningHeld));
        // ---- WATER-WEAPON MANNERS (item 4): weapons DON'T FIRE while swimming —
        // the gun is lowered/holstered (see the viewmodel draw). A trigger pull
        // gets a soft dry-fire click (cadence-gated so held autos don't spam it).
        if (wantFire && playerArmed && player.isAlive() && player.swimming() && !lightningHeld) {
            if (swimFireDenyCooldown <= 0.0f) {
                const x3::audio::SoundHandle dh = currentDryfireSfx();
                if (dh.valid()) audio->playSound2D(dh, 0.35f, 0.8f);
                swimFireDenyCooldown = 0.45f;
                x3::logInfo("fire: refused — swimming (weapon lowered)");
            }
        } else if (wantFire && playerArmed && player.isAlive() && arsenal.canFire()) {
            x3::phys::Vec3 eye{ camX, camY, camZ };
            x3::phys::Vec3 dir{ std::cos(camPitch) * std::cos(camYaw),
                                std::sin(camPitch),
                                std::cos(camPitch) * std::sin(camYaw) };
            x3::game::ResolvedFire shot = arsenal.fire(eye, dir, weaponRng);
            // W2-A2 (punch-list P1 #8): dry-fire click on an empty-mag trigger pull.
            // ResolvedFire.dryFire is cadence-gated in the arsenal (autos click at
            // the weapon's fire rate, not per frame).
            if (shot.dryFire) {
                const x3::audio::SoundHandle dh = currentDryfireSfx();
                if (dh.valid()) audio->playSound2D(dh, 0.5f, 1.0f);
            }
            // ---- THE WATER ZAP (Tim): a LIGHTNING shot that MEETS WATER — or one
            // fired by a shooter who is IN the water — electrifies the surface.
            // ONE zap per trigger pull (the WaterZapper latch + 1.75 s cooldown);
            // a held beam does NOT re-zap every frame.
            if (canonWorld && lightningHeld && !shot.dryFire && waterZapper.canZap()) {
                x3::game::WaterZapEntry we = x3::game::findWaterEntry(
                    eye, dir, arsenal.current().range, waterQueryFn);
                const x3::phys::Vec3 pFeet = player.feet();
                if (!we.hit) {
                    // He is IN the water but aimed at the sky: the pool he floats
                    // in still goes live (Tim: "fired by a player who is IN the water").
                    const float w = waterQueryFn(pFeet.x, pFeet.z);
                    if (w > x3::game::kFishDryTest && pFeet.y < w) {
                        we.hit = true; we.fromInWater = true;
                        we.x = pFeet.x; we.z = pFeet.z; we.surfaceY = w; we.y = w;
                    }
                }
                if (we.hit) fireWaterZap(we, &player, &pFeet);
            }
            // Muzzle origin = the held viewmodel's barrel tip. The default origin sits
            // BELOW the eye line (down 0.30) so ballistic tracers visibly leave the gun.
            // LIGHTNING (director note) must emanate from the gun TIP, not from below
            // it: the railgun beam reads wrong starting under the barrel. Use a
            // tip-accurate origin for the beam — pushed further forward to the emitter
            // and raised onto the barrel line (almost no downward drop).
            // THE MUZZLE (Tim 2026-07-11) — the BARREL TIP of the weapon we are actually
            // DRAWING, not a camera-relative guess. The old code needed a bespoke "gun TIP"
            // override for the lightning beam precisely BECAUSE the shared guess sat in
            // mid-air below the barrel; with a real per-weapon barrel tip every gun (beam
            // included) emits from its own emitter, so the special case is gone. In THIRD
            // PERSON the gun is in Jake's hand, so the muzzle comes from the hand-socket
            // held-weapon matrix instead.
            x3::phys::Vec3 muzzle = weaponMuzzle(arsenal, *console, camX, camY, camZ, camYaw, camPitch);
            {
                x3::phys::Vec3 tpMuzzle{};
                if (thirdPerson.heldMuzzleWorld(scene, arsenal, tpMuzzle))
                    muzzle = tpMuzzle;
            }
            // LIVING WORLD: gunfire is VIOLENCE — any civilians in earshot scatter,
            // and any guard in earshot raises the facility alert (resolved against
            // the live observers at the next alert update).
            if (facilityCrowd.built()) facilityCrowd.onViolence(eye);
            // LIVING NPCs: the canon room crowds + city street crowds hear it
            // too (onViolence self-gates on scatterRadius — a distant shot
            // never disturbs a crowd out of earshot).
            for (auto& cc : canonCrowds) if (cc.built()) cc.onViolence(eye);
            for (auto& cc : cityCrowds)  if (cc.built()) cc.onViolence(eye);
            // SCOPE (canon arm): gunshots feed the alert only when fired INSIDE
            // the tower (roomAt != kNoRoom) — a shot on the apron / in the city /
            // over the river is outside the facility net (crowds still scatter).
            if (facilityAlertOn &&
                (!canonWorld || canonFloor.roomAt(eye.x, eye.y, eye.z) != x3::game::kNoRoom))
                facilityAlert.reportGunshot(eye);
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
                // [W9-3 RPG] skill/mod damage layer on the bolt (base def untouched).
                const int pjDmg = x3::game::rpgScaleDamage(pj.damage, rpgMods, rpgCritRng);
                projectiles.push_back(LiveProjectile{ muzzle, pj.vel, pjDmg, 0.0f, pj.range, impactKind, pj.type, currentImpactSfx() });
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
                    // [W9-3 RPG] skill/mod damage multiplier + crit roll LAYERED on
                    // this pellet/ray's WeaponDef damage (the table is untouched).
                    const int wdmg = x3::game::rpgScaleDamage(ray.damage, rpgMods, rpgCritRng);
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
                    // CANON ALIENS (planet): the four hostile species patrol the facility
                    // exterior in their OWN manager (app_run `canonAliens`). It was MISSING
                    // from this dispatch chain, so player shots passed clean through the
                    // Saurians / Grey / Mantis — the "some monsters cannot be shot" playtest
                    // bug. Route the ray here too: the manager maps the nearest Enemy body to
                    // its alien and applies damage (the allied Nordic Steward simply ignores it).
                    if (!r.hitMonster && canonWorld) {
                        x3::game::FireResult ca = canonAliens.fire(eye, ray.dir, scene, *physics, wdmg, ray.type);
                        if (ca.hitMonster || (!r.hit && ca.hit)) r = ca;
                    }
                    // WAVE (cell-door): route the shot through the canon explodable barrels —
                    // a ray into the cell/hall barrel breaks it; it detonates on the next
                    // canonBarrels.update() (DJBooth fireball + splash + chain).
                    if (canonWorld) {
                        const float e3[3] = { eye.x, eye.y, eye.z };
                        const float d3[3] = { ray.dir.x, ray.dir.y, ray.dir.z };
                        canonBarrels.onShot(e3, d3);
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
        // THE WATER ZAP latch: the trigger coming UP re-arms the next zap. Held =
        // no re-zap (the cooldown alone would still allow one every 1.75 s).
        if (!fireHeld) waterZapper.triggerReleased();

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
            // CHARGE weapons (Lightning) have no mag — the beam whine should cut when
            // charge runs out, not when a (never-consumed) magazine hits 0.
            const bool hasAmmo = arsenal.infiniteAmmo() ||
                (cw.usesCharge ? arsenal.currentState().charge > 0.0f
                               : arsenal.currentState().ammoInMag > 0);
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
                    if (!r.hitMonster && canonWorld) {   // canon aliens on the planet (same omission as the hitscan chain)
                        x3::game::FireResult ca = canonAliens.fire(b.pos, ndir, scene, *physics, b.damage, b.type);
                        if (ca.hitMonster) r = ca;
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
                    // Rocket detonates in a fireball; other bolts splash at the hit.
                    if (b.impactKind == x3::game::WeaponFxKind::Rocket)
                        combatFx.spawnExplosion(eh.point, 1.4f);
                    if (r.killed) { combatFx.spawnDeath(eh.point);
                        audio->playSound3D(sndDeath, eh.point.x, eh.point.y, eh.point.z, 1.0f, 1.0f); }
                    else combatFx.spawnBlood(eh.point, ndir);
                    consumed = true;
                } else {
                    x3::phys::RayHit sh = physics->rayCast(b.pos, ndir, stepLen, x3::phys::Layer::Static);
                    if (sh.hit) {
                        // Rocket -> violent fireball; energy/ballistic bolts -> per-kind splash.
                        if (b.impactKind == x3::game::WeaponFxKind::Rocket)
                            combatFx.spawnExplosion(sh.point, 1.4f);
                        else
                            combatFx.spawnImpact(sh.point, sh.normal, b.impactKind);
                        combatFx.addTracer(b.pos, sh.point);
                        if (b.impactSnd.valid())
                            audio->playSound3D(b.impactSnd, sh.point.x, sh.point.y, sh.point.z, 0.6f, 1.0f);
                        consumed = true; }
                }
                if (!consumed) {
                    b.pos = x3::phys::Vec3{ b.pos.x + b.vel.x*dt, b.pos.y + b.vel.y*dt, b.pos.z + b.vel.z*dt };
                    b.traveled += stepLen;
                    // Make the travelling bolt VISIBLE in flight (glowing core + trail;
                    // rocket also puffs exhaust smoke) — bolts were invisible before.
                    combatFx.boltFx(b.pos, b.vel, b.impactKind);
                    if (b.traveled >= b.range) consumed = true;   // out of range -> despawn
                }
                if (consumed) { projectiles[pi] = projectiles.back(); projectiles.pop_back(); }
                else ++pi;
            }
        }

        // Advance FX timers (tracer lifetimes + muzzle flash) only while the sim
        // runs; frozen during a UI menu so particles/tracers hold still.
        if (!simFrozen) combatFx.update(dt);
        if (riftHudTimer > 0.0f) riftHudTimer -= dt;   // W-RIFT: the traversal banner ages out
        if (riftLore.built()) riftLore.update(dt);     // bakes the maintenance log onto the glass
        if (stairLore.built()) stairLore.update(dt);   // clue 1: Okafor work order (45-45)
        if (liftLore.built())  liftLore.update(dt);    // clue 2: Vasquez riddle (4455)
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
            // DEEP-SPACE SKY BODIES (--world spacestation / LevelDoc biome "space"):
            // the local star, distant Sol/Earth, and the small system planets, drawn
            // after the opaque scene as direction-anchored, parallax-free bodies
            // re-projected on the live eye. No-op for every non-space world.
            if (spaceBiome && spacePlanetMesh.valid())
                drawNightSkyPlanets(device, frame, spacePlanetMesh, spacePlanets,
                                    (float)glfwGetTime() * 0.02f, camX, camY, camZ,
                                    spacePlanetRingMesh);
            // W-RIFT: the hub's membrane FX (lightning arcs + spark motes + the hall's
            // light shafts) are drawn AFTER the scene pass, exactly as the dev host does
            // — and only from inside the region.
            if (riftBuilt && riftInZone(camX, camY, camZ)) rifthub.drawFx(*device, frame);
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
            if (canonWorld && canonFloor.valid()) {
                canonDressing.draw(*device, frame);
                canonBarrels.render(frame);   // WAVE (cell-door): explodable barrels + debris
                canonRooms.draw(*device, frame, canonVisRooms);
                facilityExterior.draw(*device, frame);   // SEAM 2: facade skin (panes/bands/apron/sign)
                // WORLD CARS: parked visuals + the live car — direct draws,
                // gated on the same outdoor PVS lane as the streamed planet
                // (deep-interior frames never pay the cars' draw cost).
                if (worldCars.built() &&
                    scene.roomVisible(x3::game::kStreamedExteriorRoom))
                    worldCars.draw(frame);
            }
            // --world canonlevel gameplay: the sidearm pickup + animated enemies + Martinez
            // + the rescue girls, ROOM-GATED (only the visible rooms' characters are drawn/
            // skinned, so the cull's perf payoff is preserved with the characters in).
            if (canonWorld && canonPlay.built()) canonPlay.draw(*device, frame, scene);
            // CANON ALIENS: the planet aliens (few instances, all outdoors).
            if (canonWorld) canonAliens.drawAll(*device, frame, scene);
                if (canon45.built()) canon45.draw(*device, frame, scene);
            // SKINNED CITIZENS: the crowds as real people — the same drawMonster
            // PBR fan as the club's dancers, room-gated inside draw().
            for (int ci = 0; ci < 3; ++ci) {
                canonCrowdSkins[ci].draw(*device, frame, scene);
                cityCrowdSkins[ci].draw(*device, frame, scene);
            }
            // THE OCEAN LIVES (PVS-gated inside draw()) - ONCE per frame, not per crowd.
            worldSea.draw(*device, frame, scene);
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
                club1127.drawCharacters(*device, frame, scene);  // The Deep's DJ + bouncer (no-op until 1127 builds it)
                // ---- Monster HEALTH BARS — shiny metallic, world-anchored, with a
                // sweeping specular sheen (shimmer). LOS-culled so a bar NEVER shows
                // through a wall. Above every living enemy; flares white on a fresh hit
                // and warms toward red as HP drops (length still reads the exact value). --
                {
                    const double barT = glfwGetTime();
                    const x3::phys::Vec3 hbEye{ camX, camY, camZ };
                    // ROOM GATE (owner spec: "show on monsters, but NOT outside the room
                    // you are in"). Resolve the player's current PVS room ONCE from the
                    // same per-room cull data (CanonFloor::roomAt) that drives r_roomcull/
                    // r_vis. A bar then draws only for an enemy in the SAME room. This sits
                    // ON TOP of the LOS raycast below: LOS blocks bars through solid walls,
                    // the room gate blocks bars leaking through an OPEN doorway into the
                    // next room. kNoRoom (non-canon world, or player straddling a doorway
                    // seam / standing outdoors) disables the gate so those paths keep the
                    // prior LOS+range behaviour and never hide a legitimate same-space bar.
                    const uint32_t hbPlayerRoom =
                        canonFloor.valid() ? canonFloor.roomAt(camX, camY, camZ)
                                           : x3::game::kNoRoom;
                    const bool hbRoomGate =
                        canonFloor.valid() && hbPlayerRoom != x3::game::kNoRoom;
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
                        // Owner art direction (2026-07): WARM HP ramp + DIMMER read. The fill
                        // lerps YELLOW (full HP) -> ORANGE (mid) -> RED (low) across the HP
                        // fraction — no green anywhere — and every layer is scaled down so the
                        // bars read as subtle world-UI, not neon signs. kBarDim (0.60) is the
                        // overall brightness scale; fill/frame alphas drop to ~0.62-0.65.
                        const float kBarDim = 0.60f;
                        const float hpY[3] = { 1.00f, 0.85f, 0.20f };         // healthy = warm yellow
                        const float hpO[3] = { 1.00f, 0.50f, 0.10f };         // mid     = orange
                        const float hpR[3] = { 0.90f, 0.15f, 0.10f };         // low     = red
                        float rmp[3];
                        if (frac >= 0.5f) { const float t = (frac - 0.5f) * 2.0f;   // 0..1 orange -> yellow
                            for (int k = 0; k < 3; ++k) rmp[k] = hpO[k] + t * (hpY[k] - hpO[k]); }
                        else              { const float t = frac * 2.0f;            // 0..1 red -> orange
                            for (int k = 0; k < 3; ++k) rmp[k] = hpR[k] + t * (hpO[k] - hpR[k]); }
                        const float outl[4]   = { 0.00f, 0.00f, 0.00f, 0.55f };                       // black definition outline
                        // Warm, dim bronze frame (was bright steel-blue) — still breathes.
                        const float frameC[4] = { 0.34f*pulse, 0.28f*pulse, 0.16f*pulse, 0.62f };
                        const float backC[4]  = { 0.03f, 0.03f, 0.05f, 0.55f };                        // dark inset bg, dimmed
                        // Dimmed warm fill: base body + a slightly lighter top band fakes a
                        // vertical gradient; a hit adds a small warm flare (no white blowout).
                        const float baseC[4]  = { rmp[0]*kBarDim + 0.14f*flash, rmp[1]*kBarDim + 0.06f*flash, rmp[2]*kBarDim, 0.65f };
                        const float topC[4]   = { rmp[0]*kBarDim*1.30f + 0.10f*flash, rmp[1]*kBarDim*1.30f, rmp[2]*kBarDim*1.30f, 0.65f };
                        const float fillW = bw * frac;
                        device->drawHudQuad(frame, x0 - 2.0f, y0 - 2.0f, bw + 4.0f, bh + 4.0f, outl);
                        device->drawHudQuad(frame, x0 - 1.5f, y0 - 1.5f, bw + 3.0f, bh + 3.0f, frameC);
                        device->drawHudQuad(frame, x0, y0, bw, bh, backC);
                        device->drawHudQuad(frame, x0, y0, fillW, bh, baseC);            // body
                        device->drawHudQuad(frame, x0, y0, fillW, bh * 0.45f, topC);     // top sheen band
                        // Sweeping specular sliver = the "shimmer", dimmed + warmed so it
                        // reads as a soft glint, not a neon flash.
                        if (fillW > 6.0f) {
                            const float sw = 7.0f;
                            const float swp = (float)std::fmod(barT * 0.55 + head.x * 0.05, 1.0);
                            float sxx = x0 + swp * fillW - sw * 0.5f;
                            if (sxx < x0)              sxx = x0;
                            if (sxx > x0 + fillW - sw) sxx = x0 + fillW - sw;
                            const float sheen[4] = { 1.00f, 0.92f, 0.70f, 0.22f };
                            device->drawHudQuad(frame, sxx, y0, sw, bh, sheen);
                        }
                    };
                    auto barsFor = [&](x3::game::MonsterManager& mm) {
                        for (uint32_t i = 0; i < mm.count(); ++i) {
                            x3::game::MonsterSystem& m = mm.at(i);
                            if (!m.alive()) continue;
                            x3::phys::Vec3 c = m.pos();
                            // ROOM cull: skip the bar if this enemy is not in the player's
                            // current room. An enemy that resolves to kNoRoom (off the room
                            // grid — e.g. an outdoor/seam spawn) falls through to the LOS
                            // gate so it is not silently hidden. This is what stops a bar in
                            // the NEXT room (visible through an open doorway) from drawing.
                            if (hbRoomGate) {
                                const uint32_t mRoom = canonFloor.roomAt(c.x, c.y + 1.0f, c.z);
                                if (mRoom != x3::game::kNoRoom && mRoom != hbPlayerRoom) continue;
                            }
                            // LOS cull: skip the bar if a static wall sits between the
                            // camera and the enemy's chest (no more bars through walls).
                            const x3::phys::Vec3 chest{ c.x, c.y + 1.0f, c.z };
                            const x3::phys::Vec3 d{ chest.x - hbEye.x, chest.y - hbEye.y, chest.z - hbEye.z };
                            const float dist = std::sqrt(d.x*d.x + d.y*d.y + d.z*d.z);
                            if (dist > 0.001f) {
                                const x3::phys::Vec3 nd{ d.x/dist, d.y/dist, d.z/dist };
                                // STRICT Static — the permissive mask also reports the
                                // ENEMY'S OWN box (0.6 m half-width vs this 0.3 m
                                // shortening), so every bar was silently culled.
                                const x3::phys::RayHit los = physics->rayCastStrict(hbEye, nd, dist - 0.3f, x3::phys::Layer::Static);
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
                    // CANON LEVEL enemies (the playable build's monsters: Main Hall
                    // squad, cell guards, Medical-Bay attackers, floor bosses, upper
                    // squads) live in canonPlay, not `game`. Feed them through the same
                    // room-gated + LOS + billboarded bar so the owner's "show on
                    // monsters" holds on --world canonlevel too. Same idiom as the
                    // zapMonsters visitor above.
                    if (canonWorld && canonPlay.built())
                        canonPlay.forEachHostileManager(barsFor);
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
                VmPose vmPose = readViewmodelPose(*console);
                // WATER-WEAPON MANNERS (item 4): while swimming the gun slides
                // down-and-in on the smoothed blend (swimVmAmt) so it never
                // clips the water surface; restores on exit through the same
                // curve. A no-op at 0 (dry land byte-for-byte unchanged).
                applySwimViewmodelLower(vmPose, swimVmAmt);
                const bool vmArmed = game.armed() || (canonWorld && canonPlay.armed());
                // CLUB SAFE ZONE (fix/club-relight): "we do not need our weapons equipped
                // in the club" (Tim). Down at The Deep (Y=-200) the club is a safe social
                // space — holster the FP weapon so there is no raised gun / HUD viewmodel.
                // Same zone test the club light-takeover uses (camY < -150 with the club
                // built), so it engages exactly when the player is in the room and restores
                // itself the instant they ride the elevator back up.
                const bool clubHolster = club1127.built() && camY < -150.0f;
                // THIRD-PERSON: hide the FP weapon viewmodel ENTIRELY (the gun is shown
                // in the avatar's hand instead — drawn after scene.render below).
                // viewmodelVisible() is true in FP / unbuilt, so FP behaviour is unchanged.
                if (!thirdPerson.viewmodelVisible() || worldCars.driving() || clubHolster) {
                    // 3P / AT THE WHEEL: no FP viewmodel this frame (hands are
                    // on the wheel; the chase camera is not an FP eye).
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
                // CROWD CHATTER bubbles — THE PEOPLE SPEAK. World-anchored over
                // each speaker's head (<= 14 m, LOS-culled, PVS-gated, <= 4
                // concurrent, fade in/out — see drawChatterBubbles). Suppressed
                // whenever a dialog / keypad / terminal UI owns the screen.
                if (!terrainWorld && !codeMode && !termMode &&
                    !chatTrees.active() && !npcDialog.active()) {
                    x3::game::ChatterDrawSite chSites[6];
                    uint32_t nCh = 0;
                    for (int ci = 0; ci < 3; ++ci) {
                        if (canonCrowds[ci].built())
                            chSites[nCh++] = { &canonChatter[ci], &canonCrowds[ci] };
                        if (cityCrowds[ci].built())
                            chSites[nCh++] = { &cityChatter[ci], &cityCrowds[ci] };
                    }
                    if (nCh > 0)
                        x3::game::drawChatterBubbles(*device, frame, physics.get(),
                                                     scene,
                                                     x3::phys::Vec3{ camX, camY, camZ },
                                                     chSites, nCh);
                }
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
                                // W-RIFT: the panel never offers a locked floor — skip it
                                // exactly like callNext() does, so the prompt cannot lie.
                                int next = cur;
                                for (int g = 0; g < std::max(1, elevator.stopCount()); ++g) {
                                    next = (next + 1) % std::max(1, elevator.stopCount());
                                    if (!elevator.stopLocked(next)) break;
                                }
                                ep = "Floor " + elevator.floorLabel(cur) +
                                     "   [E] Go to " + elevator.floorLabel(next);
                            }
                            floatPrompt(x3::phys::Vec3{ cc.x, cc.y + 1.6f, cc.z },
                                        ep.c_str(), (float)ep.size() * 4.4f);
                        }
                    }
                    // W-RIFT: the rift prompts. The nearest gate names where it goes; the
                    // traversal / refusal banner rides the same bottom-center line.
                    if (riftBuilt && riftInZone(pex, pey, pez)) {
                        std::string rp;
                        if (riftHudTimer > 0.0f) {
                            rp = riftHudMsg;
                        } else {
                            const int nearRift = rifthub.consoleInRange({ pex, pey, pez }, 5.0f);
                            if (nearRift >= 0) {
                                rp = rifthub.portal((uint32_t)nearRift).activated
                                        ? ("RIFT OPEN -> " + rifthub.destination((uint32_t)nearRift) +
                                           "  (walk through)")
                                        : ("RIFT: " + rifthub.destination((uint32_t)nearRift) +
                                           "  (step in to open)");
                            }
                        }
                        if (!rp.empty()) {
                            uint32_t hw = 0, hh = 0; device->hudSize(hw, hh);
                            const float hsz = 18.0f;
                            const float adv = device->textAdvance(x3::rhi::FontRole::Menu, rp.c_str(), hsz);
                            const float hx = ((hw > 0) ? hw * 0.5f : 640.0f) - adv * 0.5f;
                            const float hy = (hh > 0) ? hh * 0.84f : 500.0f;
                            const float col[4]    = { 0.55f, 0.86f, 1.0f, 0.92f };
                            const float shadow[4] = { 0.0f, 0.0f, 0.0f, 0.70f };
                            device->drawHudTextF(frame, x3::rhi::FontRole::Menu, rp.c_str(), hx + 1.5f, hy + 1.5f, hsz, shadow);
                            device->drawHudTextF(frame, x3::rhi::FontRole::Menu, rp.c_str(), hx, hy, hsz, col);
                        }
                    }
                    // Cell HoloTerminal: within ~3 m of its anchor. Playtest fix (PB
                    // fold 4b9f067): a subtle BOTTOM-CENTER HUD hint (not a world-space
                    // float over the panel), and the "(code 1278)" spoiler is DROPPED —
                    // the player should discover the code, not have it handed to them.
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
                    // WORLD CARS hint line (bottom-center, the terminal-hint
                    // treatment): "[E] Enter" / "LOCKED - [hold E] hack" /
                    // "HACKING... 47%" / "[E] Exit" — computed by the per-frame
                    // interact above; empty when no car is in reach.
                    if (canonWorld && worldCars.built() && !worldCars.prompt().empty()) {
                        const std::string& vhint = worldCars.prompt();
                        uint32_t hw = 0, hh = 0; device->hudSize(hw, hh);
                        const float hsz = 18.0f;
                        const float adv = device->textAdvance(x3::rhi::FontRole::Menu,
                                                              vhint.c_str(), hsz);
                        const float hx = ((hw > 0) ? hw * 0.5f : 640.0f) - adv * 0.5f;
                        const float hy = (hh > 0) ? hh * 0.84f : 500.0f;  // above the terminal line
                        // Amber for the machine, red-leaning while a hack runs.
                        const bool hacking = vhint.rfind("HACKING", 0) == 0 ||
                                             vhint.rfind("LOCKED", 0) == 0;
                        const float col[4]    = { 1.0f, hacking ? 0.55f : 0.82f,
                                                  hacking ? 0.35f : 0.45f, 0.9f };
                        const float shadow[4] = { 0.0f, 0.0f, 0.0f, 0.70f };
                        device->drawHudTextF(frame, x3::rhi::FontRole::Menu, vhint.c_str(),
                                             hx + 1.5f, hy + 1.5f, hsz, shadow);
                        device->drawHudTextF(frame, x3::rhi::FontRole::Menu, vhint.c_str(),
                                             hx, hy, hsz, col);
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

                    // ---- W9-1: the desc-mechanics status tag (held items + active
                    // statuses: "EMP READY | INFECTED | FREEZING"), bottom-center,
                    // steady while anything is held/active. The dumb text tag the
                    // parallel inventory system will replace.
                    if (descMech.built()) {
                        const std::string tag = descMech.hudStatusLine();
                        if (!tag.empty()) {
                            const float tagPx = 16.0f;
                            const float tw = device->textAdvance(x3::rhi::FontRole::Menu, tag.c_str(), tagPx);
                            const float tx = ((hudW > 0) ? hudW * 0.5f : 640.0f) - tw * 0.5f;
                            const float ty = (hudH > 0) ? hudH - 46.0f : 660.0f;
                            const float tsh[4] = { 0.0f, 0.0f, 0.0f, 0.65f };
                            const float tcl[4] = { 0.62f, 0.88f, 0.95f, 0.9f };   // instrument cyan
                            device->drawHudTextF(frame, x3::rhi::FontRole::Menu, tag.c_str(),
                                                 tx + 1.5f, ty + 1.5f, tagPx, tsh);
                            device->drawHudTextF(frame, x3::rhi::FontRole::Menu, tag.c_str(),
                                                 tx, ty, tagPx, tcl);
                        }
                    }

                    // ---- W5-3: THE WIN CARD — Sarah is out. Drawn over the live
                    // helipad scene (no input freeze: the alien sky, the beacon, her
                    // beside you — the moment IS the reward), fading in the last 2 s.
                    if (winTimer > 0.0f) {
                        winTimer -= dt;
                        float a = winTimer > 2.0f ? 1.0f : winTimer * 0.5f;
                        if (a < 0.0f) a = 0.0f;
                        const float cxp = (hudW > 0) ? hudW * 0.5f : 640.0f;
                        const float cyp = (hudH > 0) ? hudH * 0.34f : 240.0f;
                        struct L { const char* t; float px; float dy; float r, g, b; };
                        const L lines[3] = {
                            { "YOU GOT HER OUT.", 54.0f,   0.0f, 0.95f, 0.90f, 0.82f },
                            { winLine2.c_str(),   26.0f,  74.0f, 0.80f, 0.86f, 0.92f },
                            { "TO BE CONTINUED",  22.0f, 116.0f, 0.55f, 0.75f, 0.80f },
                        };
                        for (const L& l : lines) {
                            const float w = device->textAdvance(x3::rhi::FontRole::Menu, l.t, l.px);
                            const float sh[4] = { 0.0f, 0.0f, 0.0f, 0.8f * a };
                            const float cl[4] = { l.r, l.g, l.b, a };
                            device->drawHudTextF(frame, x3::rhi::FontRole::Menu, l.t,
                                                 cxp - w * 0.5f + 2.0f, cyp + l.dy + 2.0f, l.px, sh);
                            device->drawHudTextF(frame, x3::rhi::FontRole::Menu, l.t,
                                                 cxp - w * 0.5f, cyp + l.dy, l.px, cl);
                        }
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
                // F7 THE CLONE (Act-1 finale): the same banner treatment as the
                // Martinez phase flash, plus a persistent boss HP/phase line while the
                // fight is live. Reuses the Martinez boss-phase HUD path verbatim.
                if (!terrainWorld && topFloors.built()) {
                    const auto& cf = topFloors.cloneFight();
                    uint32_t hw = 0, hh = 0; device->hudSize(hw, hh);
                    if (cf.bannerTime() > 0.0f && !cf.banner().empty()) {
                        const float scale = 28.0f;
                        const float bw = cf.banner().size() * scale * 0.6f;
                        const float px = (hw > 0) ? (hw * 0.5f - bw * 0.5f) : 420.0f;
                        const float py = (hh > 0) ? (hh * 0.28f) : 160.0f;
                        const float hot[4] = { 0.55f, 0.95f, 1.0f, 1.0f };   // clone-cyan
                        device->drawHudText(frame, cf.banner().c_str(), px, py, scale, hot);
                    }
                    // Live boss line while the Clone is up AND the player is on its floor
                    // (the fight only reads once it has been engaged — HP < full).
                    if (cf.bossAlive() && cf.bossHpFrac() < 0.999f) {
                        const std::string line = cf.hudLabel();
                        const float scale = 3.0f;
                        const float bw = line.size() * scale * 6.0f;
                        const float px = (hw > 0) ? (hw * 0.5f - bw * 0.5f) : 420.0f;
                        const float py = (hh > 0) ? (hh * 0.08f) : 60.0f;
                        const float col[4] = { 0.62f, 0.90f, 1.0f, 1.0f };
                        device->drawHudText(frame, line.c_str(), px, py, scale, col);
                    }
                    // "[E] BREAK SARAH'S NEURAL COLLAR" prompt while in reach.
                    const std::string cp = topFloors.collarPrompt(camPos);
                    if (!cp.empty()) {
                        const float scale = 3.0f;
                        const float bw = cp.size() * scale * 6.0f;
                        const float px = (hw > 0) ? (hw * 0.5f - bw * 0.5f) : 420.0f;
                        const float py = (hh > 0) ? (hh * 0.62f) : 380.0f;
                        const float warn[4] = { 1.0f, 0.45f, 0.35f, 1.0f };
                        device->drawHudText(frame, cp.c_str(), px, py, scale, warn);
                    }
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
                // the canon spawns (not the empty legacy groups). OPENING FLOW: the canon
                // count is the AWAKE (locally active) hostiles, not the whole spire —
                // dormant far-floor spawns are gated threats, not on the counter. (The
                // old fold of enemiesRemaining() put "ENEMIES: 101" on screen at wake.)
                hm.enemiesRemaining = game.enemiesRemaining() +
                    ((canonWorld && canonPlay.built()) ? canonPlay.enemiesAwake() : 0);
                // OPENING OBJECTIVE BEATS (canon): escape the cell -> fight what's
                // around you -> push on -> clear. ASCII ONLY: the HUD glyph atlas maps
                // every byte >= 128 to '?', so the old em-dash literal rendered as
                // "Fight down the spire ??? save the captives" on screen.
                if (canonWorld && canonPlay.built()) {
                    if (!canonPlay.leftCell())
                        hm.objective = "Find a way out of the cell";
                    else if (canonPlay.enemiesAwake() > 0)
                        hm.objective = "Fight down the spire - save the captives, reach Martinez";
                    else if (canonPlay.enemiesRemaining() > 0)
                        hm.objective = "Push on - save the captives, reach Martinez";
                    else
                        hm.objective = "AREA CLEAR - reach the Elevator Lobby";
                }
                if (game.armed() || (canonWorld && canonPlay.armed())) {
                    const x3::game::WeaponDef&         wd = arsenal.current();
                    const x3::game::Arsenal::WeaponState& ws = arsenal.currentState();
                    hm.weapon = wd.name.c_str();
                    hm.ammoInMag = ws.ammoInMag; hm.ammoReserve = ws.reserve;
                    hm.reloading = arsenal.isReloading();
                    // CHARGE weapon (Lightning): show a CHARGE readout instead of ammo,
                    // plus the live PASSIVE-REGEN state (refilling? in the half-speed
                    // band? where does the crawl start?) — read straight off the Arsenal
                    // so the HUD cannot disagree with the tick that owns the pool.
                    if (wd.usesCharge) {
                        hm.isCharge  = true;
                        hm.chargeCur = (int)(ws.charge + 0.5f);
                        hm.chargeCap = (int)(wd.chargeCap + 0.5f);
                        hm.chargeRegen     = arsenal.chargeRegenerating();
                        hm.chargeRegenSlow = arsenal.chargeRegenSlow();
                        hm.chargeSlowAbove = (int)(wd.chargeRegenSlowAbove + 0.5f);
                    }
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
                    // OPENING FLOW: only AWAKE spawns blip — a dormant (gated) spawn is
                    // an undetected threat, and the radar must agree with the awake-only
                    // ENEMIES counter (red blips beside "AREA CLEAR" read as a bug).
                    if (canonWorld && canonPlay.built()) {
                        x3::game::CanonPlay::EnemyMark cm[x3::ui::HudModel::kMaxBlips];
                        const uint32_t nc = canonPlay.liveEnemyMarks(cm, x3::ui::HudModel::kMaxBlips);
                        ne = 0;
                        for (uint32_t i = 0; i < nc && ne < x3::ui::HudModel::kMaxBlips; ++i) {
                            if (!cm[i].awake) continue;
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
                            // STRICT Static — the permissive mask reported the enemy's
                            // own 0.6 m-half-width box and hid every nameplate.
                            x3::phys::RayHit rh = physics->rayCastStrict(eye, dir, dist - 0.5f,
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

                    // Faint room outlines. W2-A2 (punch-list P1 #12): on canonlevel the
                    // legacy `game` is UNBUILT — game.layout() fed stale/zeroed rects to
                    // the minimap. Feed the REAL canon floor's room rects instead
                    // (player-relative transform happens in the HUD as before).
                    if (canonWorld && canonFloor.valid()) {
                        for (const x3::game::CanonRoom& cr : canonFloor.rooms) {
                            if (hm.roomCount >= x3::ui::HudModel::kMaxRooms) break;
                            const int r = hm.roomCount++;
                            hm.roomCx[r] = cr.cx;        hm.roomCz[r] = cr.cz;
                            hm.roomHx[r] = cr.w * 0.5f;  hm.roomHz[r] = cr.d * 0.5f;
                        }
                    } else {
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
                // W-MENU / W-RIFT: the world directory and the rift console are menus
                // too — they capture input (keyDown() is gated on them), so they get
                // the nav edges as well or they would be mouse-only.
                if (uiMenuActive || worldMenu.isOpen() || riftConsoleOpen) {
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

            // ================================================================
            // W-MENU — THE WORLD / PLACE DIRECTORY (F6, or pause -> TRAVEL).
            // Every place the game has, with the truth about how each is reached.
            // The reachability answer comes from the SAME resolver the rift gates
            // use, so the menu and the hub can never disagree.
            // ================================================================
            if (worldMenu.isOpen()) {
                menuUi.begin(*device, frame, uin);
                const int pick = worldMenu.draw(menuUi, dt,
                    [&](const x3::game::Destination& d) {
                        x3::game::DestStatus st;
                        // Standing in it already? (the hub knows where it is; the rest
                        // is answered by the resolver's own anchor.)
                        x3::phys::Vec3 anchor{};
                        std::string    why;
                        if (riftDestination(d.key, anchor, &why)) {
                            st.reach = x3::game::DestReach::Teleport;
                            const x3::phys::Vec3 pp = physics->getBodyPosition(player.body());
                            const float dx = pp.x - anchor.x, dy = pp.y - anchor.y,
                                        dz = pp.z - anchor.z;
                            st.here = (dx*dx + dy*dy + dz*dz) < (4.0f * 4.0f);
                        } else if (d.worldFlag[0] && d.worldFlag != worldMode) {
                            st.reach  = x3::game::DestReach::WorldLoad;
                            st.reason = why;
                        } else {
                            st.reach  = x3::game::DestReach::Unavailable;
                            st.reason = why;
                        }
                        return st;
                    });
                menuUi.end();

                if (pick >= 0) {
                    const x3::game::Destination& d = x3::game::destination((uint32_t)pick);
                    x3::phys::Vec3 to{};
                    std::string    why;
                    if (riftDestination(d.key, to, &why)) {
                        // In THIS world: move the player. No load, blackout cover on.
                        player.setFeetPosition(*physics,
                            x3::phys::Vec3{ to.x, to.y - 1.6f + 0.3f, to.z });
                        travelFadeT = 0.9f;
                        riftHudMsg   = std::string("TRAVELLED -> ") + d.name;
                        riftHudTimer = 4.0f;
                        x3::logInfo(std::string("[worldmenu] TELEPORT -> ") + d.name);
                    } else if (d.worldFlag[0]) {
                        // A WHOLE OTHER WORLD: ask main()'s world-load loop to tear this
                        // one down and build that one, standing us at `d.key` when it is
                        // up (load AND place). No lie, no fake teleport.
                        hc.switchWorldTo = d.worldFlag;
                        hc.switchDestKey = d.key;
                        worldLoadRequested = true;
                        x3::logInfo(std::string("[worldmenu] WORLD LOAD -> --world ") +
                                    d.worldFlag + " @ " + d.key);
                    } else {
                        riftHudMsg   = std::string("CANNOT REACH ") + d.name + ": " + why;
                        riftHudTimer = 4.0f;
                        x3::logWarn(std::string("[worldmenu] refused ") + d.key + ": " + why);
                    }
                }
            }

            // ================================================================
            // W-RIFT — THE RIFT CONSOLE, LIVE IN THE CANON LOOP. Sliders, knobs,
            // the typed TARGET field and the PREV/NEXT destination cycle. The
            // outcome is applied to the world by updateConsole() itself (membrane,
            // lights, alarms, re-target), exactly as in the dev host.
            // ================================================================
            if (riftBuilt && rifthub.consoleOpen()) {
                riftUiClock += dt;
                x3::ui::UiInput rin = uin;
                // Drain the typed ring into the field (backspace/enter are edges).
                rin.typedCount = 0;
                for (int t = 0; t < g_riftTypedN && t < x3::ui::UiInput::kMaxTyped; ++t)
                    rin.typed[rin.typedCount++] = g_riftTyped[t];
                g_riftTypedN = 0;
                const bool bsNow  = rawKey(GLFW_KEY_BACKSPACE);
                const bool entNow2 = rawKey(GLFW_KEY_ENTER) || rawKey(GLFW_KEY_KP_ENTER);
                rin.backspace = bsNow  && !prevRiftBs;
                rin.enter     = entNow2 && !prevRiftEnter;
                prevRiftBs = bsNow; prevRiftEnter = entNow2;

                riftUi.begin(*device, frame, rin);
                if (rifthub.updateConsole(riftUi, dt)) {
                    const uint32_t ci = (uint32_t)rifthub.activeConsole();
                    const x3::game::Destination* nd =
                        x3::game::findDestination(rifthub.destination(ci));
                    riftHudMsg = "RIFT " + std::to_string(ci + 1) + ": " +
                                 rifthub.portal(ci).console.status + "  ->  " +
                                 (nd ? nd->name : rifthub.destination(ci).c_str());
                    riftHudTimer = 4.0f;
                }
                riftUi.end();
            } else {
                g_riftTypedN = 0;   // never let keystrokes pool while nothing is typing
            }

            // ---- [W9-3 RPG] the BACKPACK (I) / SKILL TREE (K) screens + the
            // always-on HUD chip (equipped item + LV/XP). Screens draw over the
            // HUD (sim frozen while open — simFrozen above); the chip only while
            // actually playing with no other full-screen surface up. ----
            {
                // Rising-edge nav/verb keys for the screens (raw: the screens ARE
                // the capturing surface while open).
                x3::game::RpgUi::Input rin{};
                rin.ui = uin;
                const bool rUp  = rawKey(GLFW_KEY_UP),    rDn = rawKey(GLFW_KEY_DOWN);
                const bool rLf  = rawKey(GLFW_KEY_LEFT),  rRt = rawKey(GLFW_KEY_RIGHT);
                const bool rEnt = rawKey(GLFW_KEY_ENTER) || rawKey(GLFW_KEY_KP_ENTER);
                const bool rDrp = rawKey(GLFW_KEY_X);
                const bool rEqp = rawKey(GLFW_KEY_Q);
                if (rpgUi.anyOpen()) {
                    rin.navUp    = rUp  && !prevRpgUp;
                    rin.navDown  = rDn  && !prevRpgDown;
                    rin.navLeft  = rLf  && !prevRpgLeft;
                    rin.navRight = rRt  && !prevRpgRight;
                    rin.activate = rEnt && !prevRpgEnter;
                    rin.dropKey  = rDrp && !prevRpgDrop;
                    rin.equipKey = rEqp && !prevRpgEquip;
                }
                prevRpgUp = rUp; prevRpgDown = rDn; prevRpgLeft = rLf; prevRpgRight = rRt;
                prevRpgEnter = rEnt; prevRpgDrop = rDrp; prevRpgEquip = rEqp;

                if (rpgUi.backpackOpen()) {
                    rpgUi.drawBackpack(*device, frame, rin, inventory, itemDb, rpgUseItem);
                    keycardMask |= inventory.keycardMask(itemDb);   // bag stays the mask source
                } else if (rpgUi.skillsOpen()) {
                    rpgUi.drawSkills(*device, frame, rin, skillTree, progression, applyRpgStats);
                }
                if (gameUi.playing() && !worldMapOpen && !rpgUi.anyOpen() &&
                    !terrainWorld && !consoleOpen)
                    rpgUi.drawHudChip(*device, frame, inventory, itemDb, progression, dt);
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
                // Flight Mode row -> shared latch + persist, ONLY on a real change
                // (so the `flightmode` console command isn't overwritten each frame).
                if (asm_.flightMode != prevMenuFlightMode) {
                    prevMenuFlightMode = asm_.flightMode;
                    const int fmi = (asm_.flightMode < 0 || asm_.flightMode > 2) ? 0 : asm_.flightMode;
                    x3::game::setRequestedFlightMode((x3::game::FlightMode)fmi);
                    writeSettings((uint32_t)cw, (uint32_t)ch, asm_.musicOn, asm_.musicVol,
                                  asm_.sfxVol, fmi, asm_.skipIntro);
                    x3::logInfo(std::string("[settings] flight mode -> ") +
                                x3::game::flightModeName((x3::game::FlightMode)fmi) + " (persisted)");
                }
                // Settings > Advanced "Skip Intro (dev)" -> persist on change. Takes
                // effect on the NEXT launch (the intro decision happens at boot);
                // F9 remains the in-the-moment skip for the running intro.
                if (asm_.skipIntro != prevMenuSkipIntro) {
                    prevMenuSkipIntro = asm_.skipIntro;
                    const int fmi = (asm_.flightMode < 0 || asm_.flightMode > 2) ? 0 : asm_.flightMode;
                    writeSettings((uint32_t)cw, (uint32_t)ch, asm_.musicOn, asm_.musicVol,
                                  asm_.sfxVol, fmi, asm_.skipIntro);
                    x3::logInfo(std::string("[settings] skip intro -> ") +
                                (asm_.skipIntro ? "ON" : "OFF") + " (persisted; applies next launch)");
                }
            }

            // Main-menu "SET AS DEFAULT" -> persist the current framebuffer size +
            // the live audio settings (one cfg file).
            if (gameUi.wantSaveDefaults()) {
                gameUi.clearSaveDefaults();
                const x3::ui::SettingsModel& s = gameUi.settings();
                writeSettings((uint32_t)cw, (uint32_t)ch, s.musicOn, s.musicVol, s.sfxVol,
                              (s.flightMode < 0 || s.flightMode > 2) ? 0 : s.flightMode,
                              s.skipIntro);
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
            // VIGIL BARK toast (ambient companion). Only visible once linked and a
            // bark is live; the object self-gates on the vigilLink flag when firing.
            if (vigilBarks.toastActive(vigilClock))
                hud.drawVigilBark(*device, frame, vigilBarks.toastText().c_str(),
                                  vigilBarks.toastAlpha(vigilClock));
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
    // WORLD CARS: parked-car bodies + the live vehicle rig (Jolt constraint)
    // must go BEFORE physics dies. Idempotent no-op when never built.
    worldCars.shutdown(*physics);
    // SEAM 3: the canon planet streamer (regions + terrain ring + its job
    // system) — idempotent no-op unless canon streaming booted.
    shutdownCanonStream();
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
    if (spacePlanetMesh.valid())     { device->destroyMesh(spacePlanetMesh); spacePlanetMesh = {}; }
    if (spacePlanetRingMesh.valid()) { device->destroyMesh(spacePlanetRingMesh); spacePlanetRingMesh = {}; }
    docLevel.shutdown(scene, *device, *physics);   // --world fromdoc doc objects + caches
    worldMap.shutdown(*device);                    // baked map-tile textures
    x3::club_listen::shutdown();                    // close the WASAPI loopback device (idempotent)
    physics->shutdown();
    // W-MENU FIX: the world/place menu's "LOADS WORLD" rows break the render loop
    // with hc.switchWorldTo set so main()'s world-load loop tears THIS world down and
    // builds the next one in the SAME shared device + window. Those shared resources
    // are created ONCE in main() and reused across the loop, so ONLY the final host
    // (no switch pending) may destroy them. Doing it here on a switch left the next
    // host (e.g. hostStreamed on a canonlevel->streamed pick) dereferencing a freed
    // device/window during its boot -> segfault. The world's OWN resources are freed
    // above (game systems, streamers, physics); the shared device/window survive the
    // handoff and the arriving host renders into them, then tears them down when IT
    // is the final host.
    if (hc.switchWorldTo.empty()) {
        device->shutdown();
        glfwDestroyWindow(window);
        glfwTerminate();
    }
    return bootTestExit;

}

}} // namespace x3::apphost
