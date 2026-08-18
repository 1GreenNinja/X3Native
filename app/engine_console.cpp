#include "engine_console.h"

#include "engine/core/x3_log.h"
#include "weapon.h"        // kVmDef* viewmodel defaults
#include "mesh_lod.h"      // registerLodCVars (Lane 5 discrete LOD cvars)
#include "gas_station.h"   // registerFuelCVars (W-STATIONS fuel_* cvars)
#include "space_pilot.h"   // flightModeName / parseFlightMode / (get|set)RequestedFlightMode

#include <GLFW/glfw3.h>
#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <windows.h>
#endif

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <vector>

namespace x3::game {

// ============================================================================
// registerEngineConsoleCVars — moved VERBATIM out of app_run.cpp's file-local
// registerViewmodelCVars() (#28-lineage fold; D-CONSOLE). Same registrations,
// same defaults, same order. app_run.cpp's registerViewmodelCVars is now a
// one-line forwarder (see app_run.cpp) so every existing call site
// (registerViewmodelCVarsForTest, geolod_shot.cpp, screenshot_hosts.cpp,
// self_tests.cpp) keeps working unchanged.
// ============================================================================
void registerEngineConsoleCVars(x3::con::IConsole& console) {
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
    // SSAO / SSGI (W-MENU find, 2026-08-17). These two groups were the ONLY
    // watched cvars (world_host_common.h hashLiveHostCVars) missing from this
    // catalog — UiController::init registers them for the CAMPAIGN console,
    // and nothing registered them for a --world HostShell console. Unregistered
    // reads return 0, so the shell's frame-0 live push (pushLiveHostCVarsToDevice)
    // silently pushed SsaoParams{enabled=0,...}/GiParams{enabled=0,...} and
    // TURNED BOTH CHAINS OFF in every interactive world host, while headless
    // proof shots (no shell) kept them on: the interactive game was flatter
    // than every screenshot said it was. Defaults = IRenderDevice.h's struct
    // defaults (PAIRED — change both).
    console.registerCVar("r_ssao",           "1",     "screen-space ambient occlusion chain on/off");
    console.registerCVar("r_ssao_radius",    "0.5",   "SSAO view-space hemisphere radius (meters)");
    console.registerCVar("r_ssao_bias",      "0.025", "SSAO depth-compare bias (view-space units)");
    console.registerCVar("r_ssao_intensity", "1.0",   "SSAO raw occlusion scale");
    console.registerCVar("r_ssao_power",     "1.5",   "SSAO contrast exponent on the final AO");
    console.registerCVar("r_ssao_strength",  "0.9",   "SSAO applied strength (1 = full, 0 = off)");
    console.registerCVar("r_ssgi",           "1",     "screen-space one-bounce GI (indirect diffuse) on/off");
    console.registerCVar("r_ssgi_intensity", "1.0",   "SSGI gathered-radiance scale");
    console.registerCVar("r_ssgi_strength",  "1.0",   "SSGI applied strength (HDR, final knob)");
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
    console.registerCVar("r_wetness",         "0",    "Surface wetness 0..1 (rain soak): darkens diffuse, smooths roughness, raises F0; 0 = off (byte-identical)");
    console.registerCVar("r_wetness_porosity","1.0",  "How much this world's materials darken when wet (0 = no darkening)");
    console.registerCVar("r_wetness_puddles", "1.0",  "Strength of cavity/AO pooling (0 = uniform coat, no puddles)");
    console.registerCVar("r_wetness_minrough","0.06", "Roughness a fully-soaked surface converges to (0 = mirror)");
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
    console.registerCVar("r_torchlegacy", "0", "Flashlight A/B: 1 restores the pre-2026-08-18 torch EXACTLY (two omni bulbs 2 m downrange, 3.30 @ 20 m + 1.70 @ 8 m) — the hard white disc, reproducible on demand. 0 = the spot-cone torch.");
    console.registerCVar("r_flashlight", "0", "Player flashlight — DEFAULT OFF since 2026-08-18 (Tim: \"I would like it OFF by default\"). L toggles it in game; setting this cvar live drives the torch either way (1 = on, 0 = off), so it is still the lighting-audit workhorse for measuring a room's OWN practicals.");
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
    // ---- FUEL (W-STATIONS): fuel_on / fuel_burn / fuel_cap / fuel_rate. Pure
    // data, so it belongs in the ONE shared catalog — `fuel_on` is discoverable
    // in every host, not only the one that happens to drive. The commands that
    // need live tank state (`fuel`, `fuel_stations`) are registered by
    // GasStationWorld::registerConsole from the driving host.
    x3::game::registerFuelCVars(console);
}

namespace {

void printCampaignOnly(x3::con::IConsole& console, const char* name) {
    console.print(std::string(name) +
                  ": campaign only (no live save/mission/player state on this console)");
}

// Buckets console.complete("") (every registered command+cvar, already sorted)
// into the groups the owner named: r_* (renderer), car_* (vehicle tuning),
// wx_*/rain_* (weather), turbo_* (forced-induction), everything else -> misc.
// Names that do not exist yet on this console (no car_/turbo_/wx_ registry
// today) simply produce an empty bucket — this is future-proofed for when
// those land, not faked now.
struct HelpGroup { const char* label; std::vector<std::string> names; };

std::vector<HelpGroup> buildHelpGroups(const x3::con::IConsole& console) {
    std::vector<HelpGroup> groups = {
        { "RENDER (r_*)",        {} },
        { "VEHICLE (car_*)",     {} },
        { "WEATHER (wx_*/rain_*)", {} },
        { "TURBO (turbo_*)",     {} },
        { "FUEL (fuel*)",        {} },
        { "MISC",                {} },
    };
    auto starts = [](const std::string& s, const char* p) { return s.rfind(p, 0) == 0; };
    for (const std::string& n : console.complete("")) {
        if      (starts(n, "r_"))                         groups[0].names.push_back(n);
        else if (starts(n, "car_"))                       groups[1].names.push_back(n);
        else if (starts(n, "wx_") || starts(n, "rain_"))   groups[2].names.push_back(n);
        else if (starts(n, "turbo_"))                      groups[3].names.push_back(n);
        else if (starts(n, "fuel"))                        groups[4].names.push_back(n);
        else                                                groups[5].names.push_back(n);
    }
    return groups;
}

// 'help' — the catalog is long (~170 names once render + cheat + campaign
// commands are all registered on one console), so it PAGES: `help` alone
// prints page 1, `help <page>` jumps to a page, `help all` dumps everything.
// Grouped in the owner's own vocabulary: r_*, car_*, wx/rain, turbo_*, misc.
void printHelp(x3::con::IConsole& console, const std::vector<std::string>& args) {
    const auto groups = buildHelpGroups(console);
    size_t total = 0;
    for (const auto& g : groups) total += g.names.size();

    const bool all = !args.empty() && args[0] == "all";
    constexpr size_t kPageLines = 24;
    size_t page = 1;
    if (!args.empty() && !all) {
        char* endp = nullptr;
        long p = std::strtol(args[0].c_str(), &endp, 10);
        if (endp != args[0].c_str() && p >= 1) page = (size_t)p;
    }

    // Flatten to (label-or-null, name) rows so paging can cut anywhere,
    // re-printing a group header if a page starts mid-group.
    struct Row { const char* header; std::string name; };
    std::vector<Row> rows;
    for (const auto& g : groups) {
        if (g.names.empty()) continue;
        rows.push_back({ g.label, "" });
        for (const auto& n : g.names) rows.push_back({ nullptr, n });
    }

    const size_t pages = all ? 1 : std::max<size_t>(1, (rows.size() + kPageLines - 1) / kPageLines);
    if (page > pages) page = pages;
    const size_t begin = all ? 0 : (page - 1) * kPageLines;
    const size_t end   = all ? rows.size() : std::min(rows.size(), begin + kPageLines);

    console.print("=== ENGINE CONSOLE HELP  (" + std::to_string(total) + " commands/cvars, " +
                  (all ? std::string("all") : ("page " + std::to_string(page) + "/" + std::to_string(pages))) +
                  ") ===");
    for (size_t i = begin; i < end; ++i) {
        const Row& r = rows[i];
        if (r.header) console.print(std::string("-- ") + r.header + " --");
        else           console.print("  " + r.name);
    }
    if (!all && pages > 1)
        console.print("-- type 'help <page>' (1.." + std::to_string(pages) + ") or 'help all' --");
}

} // namespace

// ============================================================================
// registerEngineConsoleCommands — the cheat/utility set + 'help'. Same command
// NAMES and, where a hook is supplied, the SAME behaviour app_run.cpp's
// inline registrations had (idclip's exact print strings etc.) — this is a
// relocation, not a rewrite, for every path a hook covers.
// ============================================================================
void registerEngineConsoleCommands(x3::con::IConsole& console, GLFWwindow* window,
                                   const EngineConsoleHooks& h) {
    // ---- noclip / idclip: the command the owner's screenshot showed missing.
    // REQUIRED hook — every caller (campaign Player, or a --world host's
    // HostShell freefly) supplies a real toggle. Registered under BOTH names:
    // 'idclip' (the DOOM-cheat spelling app_run always had) and 'noclip' (the
    // name the owner actually typed).
    auto noclipCmd = [&console, h](const std::vector<std::string>& a) {
        if (!h.setNoclip || !h.getNoclip) {
            console.print("noclip: unavailable on this console (no camera hook wired)");
            return;
        }
        const bool on = a.empty() ? !h.getNoclip() : (a[0] != "0");
        h.setNoclip(on);
        console.print(std::string("noclip ") +
                      (on ? "ON — fly free with WASD + mouse (Shift = fast); noclip 0 to return"
                          : "OFF"));
    };
    console.registerCommand("idclip", noclipCmd,
                            "idclip [0|1] - toggle noclip free-flight (no collision)");
    console.registerCommand("noclip", noclipCmd,
                            "noclip [0|1] - alias of idclip: toggle the freefly camera");

    // ---- iddqd / god: invulnerability. ----
    if (h.setGod && h.getGod) {
        console.registerCommand("iddqd", [&console, h](const std::vector<std::string>&) {
            const bool on = !h.getGod();
            h.setGod(on);
            if (on && h.healPlayer) h.healPlayer();
            console.print(std::string("god mode ") + (on ? "ON  (IDDQD)" : "OFF"));
        }, "toggle god mode (invulnerable)");
        console.registerCommand("god", [&console, h](const std::vector<std::string>& a) {
            const bool on = a.empty() ? !h.getGod() : (a[0] != "0");
            h.setGod(on);
            if (on && h.healPlayer) h.healPlayer();
            console.print(std::string("god = ") + (on ? "1" : "0"));
        }, "god [0|1] - toggle/set invulnerability");
    } else {
        console.registerCommand("iddqd", [&console](const std::vector<std::string>&) {
            printCampaignOnly(console, "iddqd");
        }, "toggle god mode (invulnerable) [campaign only]");
        console.registerCommand("god", [&console](const std::vector<std::string>&) {
            printCampaignOnly(console, "god");
        }, "god [0|1] - toggle/set invulnerability [campaign only]");
    }

    // ---- idkfa / idfa: arm every weapon + unlimited ammo (idkfa also gods + heals). ----
    if (h.armAllWeapons) {
        console.registerCommand("idfa", [&console, h](const std::vector<std::string>&) {
            h.armAllWeapons();
            if (h.setInfiniteAmmo) h.setInfiniteAmmo(true);
            console.print("IDFA - all weapons + unlimited ammo");
        }, "arm all weapons + unlimited ammo");
        console.registerCommand("idkfa", [&console, h](const std::vector<std::string>&) {
            if (h.setGod) h.setGod(true);
            if (h.healPlayer) h.healPlayer();
            h.armAllWeapons();
            if (h.setInfiniteAmmo) h.setInfiniteAmmo(true);
            console.print("IDKFA - god + full health + all weapons + UNLIMITED ammo");
        }, "god + full health + all weapons + unlimited ammo");
    } else {
        console.registerCommand("idfa", [&console](const std::vector<std::string>&) {
            printCampaignOnly(console, "idfa");
        }, "arm all weapons + unlimited ammo [campaign only]");
        console.registerCommand("idkfa", [&console](const std::vector<std::string>&) {
            printCampaignOnly(console, "idkfa");
        }, "god + full health + all weapons + unlimited ammo [campaign only]");
    }

    // ---- vigil_link: acquire/drop the neural link (VIGIL ambient barks). ----
    if (h.vigilLink) {
        console.registerCommand("vigil_link", [&console, h](const std::vector<std::string>& a) {
            const bool on = a.empty() ? true : (a[0] != "0");
            h.vigilLink(on);
            console.print(on ? "vigil_link - NEURAL LINK ACQUIRED. VIGIL is now in your head. Regrettably, for both of you."
                             : "vigil_link 0 - neural link severed. Silence. VIGIL will sulk.");
        }, "vigil_link [0|1] - acquire/drop the neural link that unlocks VIGIL's ambient barks");
    } else {
        console.registerCommand("vigil_link", [&console](const std::vector<std::string>&) {
            printCampaignOnly(console, "vigil_link");
        }, "vigil_link [0|1] - acquire/drop VIGIL's neural link [campaign only]");
    }

    // ---- intro_play: replay the cold-open cinematic mid-game. ----
    if (h.introPlay) {
        console.registerCommand("intro_play", [&console, h](const std::vector<std::string>&) {
            h.introPlay();
            console.print("intro_play - rolling the cold open (any key skips)...");
        }, "replay the intro cold-open cinematic");
    } else {
        console.registerCommand("intro_play", [&console](const std::vector<std::string>&) {
            printCampaignOnly(console, "intro_play");
        }, "replay the intro cold-open cinematic [campaign only]");
    }

    // ---- flightmode: the Act-3 space-pilot feel preset. Free function (no
    // Player/save state), so this is REAL on every console, not a stub. ----
    console.registerCommand("flightmode", [&console](const std::vector<std::string>& a) {
        if (a.empty()) {
            console.print(std::string("flightmode = ") +
                          x3::game::flightModeName(x3::game::requestedFlightMode()) +
                          "   (arcade | assist | loose)");
            return;
        }
        x3::game::FlightMode fm{};
        if (!x3::game::parseFlightMode(a[0], fm)) {
            console.print("flightmode: unknown mode '" + a[0] + "' (arcade | assist | loose)");
            return;
        }
        x3::game::setRequestedFlightMode(fm);
        console.print(std::string("flightmode = ") + x3::game::flightModeName(fm));
    }, "flightmode [arcade|assist|loose] - select the space-pilot feel preset");

    // ---- restart: spawn a fresh X3Engine.exe + close this window. Needs only
    // the window handle (generic — real on every console, campaign or host). ----
    console.registerCommand("restart", [&console, window](const std::vector<std::string>&) {
        if (!window) { console.print("restart: no window on this console (headless)"); return; }
        std::string exe = "X3Engine.exe";
#ifdef _WIN32
        char rbuf[1024];
        DWORD rn = GetModuleFileNameA(nullptr, rbuf, (DWORD)sizeof(rbuf));
        if (rn > 0 && rn < sizeof(rbuf)) exe.assign(rbuf, rn);
#endif
        const std::string cmd = std::string("start \"\" \"") + exe + "\"";
        console.print(std::string("restart: spawning ") + exe);
        const int rc = std::system(cmd.c_str());
        console.print(std::string("restart: spawn rc=") + std::to_string(rc) + " — closing this window...");
        glfwSetWindowShouldClose(window, GLFW_TRUE);
    }, "restart - spawn a fresh X3Engine + exit this one");

    // ---- help: grouped + paged (replaces the D6 built-in one-liner). ----
    console.registerCommand("help", [&console](const std::vector<std::string>& a) {
        printHelp(console, a);
    }, "help [page|all] - list every command/cvar, grouped (r_*/car_*/wx-rain/turbo_*/misc), paged");
}

void registerEngineConsole(x3::con::IConsole& console, GLFWwindow* window,
                           const EngineConsoleHooks& hooks) {
    registerEngineConsoleCVars(console);
    registerEngineConsoleCommands(console, window, hooks);
}

bool runEngineConsoleSelfTest() {
    bool ok = true;
    std::unique_ptr<x3::con::IConsole> console(x3::con::createConsole());

    bool noclipState = false;
    EngineConsoleHooks hooks{};
    hooks.setNoclip = [&](bool on) { noclipState = on; };
    hooks.getNoclip = [&]() { return noclipState; };
    // Everything else left null on purpose: this is the "bare --world host"
    // shape (no campaign state), the exact shape the owner's screenshot was
    // taken against.
    registerEngineConsole(*console, nullptr, hooks);

    const size_t namesBefore = console->complete("").size();
    if (namesBefore < 50) {
        x3::logError("[engineconsole-test] FAIL: only " + std::to_string(namesBefore) +
                    " names registered (expected the full ~90+ cvar/command catalog)");
        ok = false;
    }

    console->exec("r_exposure 1.5");
    if (console->getFloat("r_exposure") < 1.49f || console->getFloat("r_exposure") > 1.51f) {
        x3::logError("[engineconsole-test] FAIL: r_exposure did not take 1.5");
        ok = false;
    }

    console->exec("noclip 1");
    if (!noclipState) {
        x3::logError("[engineconsole-test] FAIL: noclip 1 did not toggle the hook");
        ok = false;
    }
    console->exec("noclip 0");
    if (noclipState) {
        x3::logError("[engineconsole-test] FAIL: noclip 0 did not clear the hook");
        ok = false;
    }

    console->exec("idclip 1");
    if (!noclipState) {
        x3::logError("[engineconsole-test] FAIL: idclip 1 (the alias) did not toggle noclip");
        ok = false;
    }
    console->exec("idclip 0");

    // A campaign-only command must be a STUB, never "unknown: <cmd>".
    console->exec("idkfa");
    {
        const auto& lines = console->outputLines();
        bool sawCampaignOnly = !lines.empty() && lines.back().find("campaign only") != std::string::npos;
        bool sawUnknown = !lines.empty() && lines.back().rfind("unknown:", 0) == 0;
        if (sawUnknown || !sawCampaignOnly) {
            x3::logError("[engineconsole-test] FAIL: idkfa (no hook) did not print a campaign-only stub");
            ok = false;
        }
    }

    console->exec("help");
    console->exec("help all");

    x3::logInfo(std::string("[engineconsole-test] ") + (ok ? "ALL PASS" : "FAILURES") +
               " — " + std::to_string(namesBefore) + " names registered");
    return ok;
}

} // namespace x3::game
