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
#include "engine/net/INetworkSystem.h"   // netcode Phase 0: --test-net + SimClock
#include "engine/net/SimClock.h"         // deterministic fixed-step accumulator
#include "engine/net/ISnapshotInterpolator.h"  // netcode Phase 0c: --test-netinterp
#include "engine/net/IClientPredictor.h"        // netcode Phase 1: --test-netpredict
#include "engine/ai/INavigation.h"       // GENERAL navigation: nav grid + A* + --test-nav

#include "scene.h"
#include "mesh_prims.h"
#include "asset_root.h"                    // portable assetRoot() (assets-LFS)
#include "asset_manifest_check.h"          // fleet asset-store manifest boot check (Phase A)
#include "audio_root.h"                    // portable resolveAudio() (D: mirror / G: packs)
#include "anim.h"                          // Skinner + --list-clips clip check
#include "thirdperson.h"                    // FP/3P toggle + Jake avatar + held weapon (--test-thirdperson)
#include "level1.h"
#include "level_loader.h"                   // data-driven canonical level loader + per-room PVS cull (--test-canonlevel)
#include "player.h"
#include "monster.h"
#include "level1_game.h"
#include "canon_play.h"                     // --world canonlevel gameplay (sidearm + animated enemies + Martinez + girls)
#include "intro_coldopen.h"                  // --world intro / default lead-in cold-open (shot-down -> captured)
#include "npc_dialog.h"                     // rescued-NPC talk/dialog -> companion (the captive girl)
#include "physprops.h"                      // FEATURE_GOALS §1: hanging cubes / joints (ragdoll foundation)
#include "ragdoll.h"                        // FEATURE_GOALS §2: physics death ragdoll
#include "editor/editor.h"                  // native Level Editor E1 (brain + self-test)
#include "editor/editor_host.h"             // Level Architect editor host (shell + blockout)
#include "barrels.h"                        // explosive barrels (shoot -> chain explosion)
#include "glass_test.h"                      // translucent-glass material (--test-glass)
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
#include "act2_desert.h"                     // EFLZ Act-2 desert depths + Salvari camp L10/L11 (--test-act2desert)
#include "act2_caves.h"                      // EFLZ Act-2 mid biomes L12-15 (--test-act2caves)
#include "tod.h"                             // EFLZ Time-of-Day cycle (sky/sun via SkyParams — --test-tod)
#include "weather.h"                         // EFLZ Weather (7 states, biome-gated, hazard — --test-weather)
#include "world_regions.h"                   // EFLZ open-world surrounding regions + 4 mountain ranges (--test-worldregions)
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

// ---- Shared NIGHT-SKY planet helper -------------------------------------
// One spot to build the UV-sphere + load the 6 FORGE3D planet types (Moon, Ice,
// Gas, Lava, Terrestrial, Sun) — the SAME files / slot order / srgb flags the
// --screenshot-nightsky block has always used — and draw them via drawPlanet().
// Used by BOTH --screenshot-nightsky and the NIGHT --screenshot-showroom path so
// the planet recipe isn't duplicated. Positions/radii are defaults the caller may
// override per-scene (the showroom shifts/scales them to frame over the spire).
struct NightSkyPlanet {
    uint32_t                            typeIndex;   // 0=Moon 1=Ice 2=Gas 3=Lava 4=Terrestrial 8=Sun
    std::vector<x3::rhi::TextureHandle> maps;        // pc.tex[] slot order for the type
    float                               worldPos[3]; // world position (defaults; caller may move)
    float                               radius;      // apparent-size scale
    const char*                         name;        // log label
    // ---- TRANSPARENT glow layers (additive atmosphere / sun corona; alpha ring).
    // Each is OPTIONAL: a valid texture handle enables that layer for this body. The
    // shells reuse the SAME sphere mesh as the body, scaled up; the ring uses a flat
    // annulus mesh (passed separately to drawNightSkyPlanets). Type indices match
    // PlanetType: Atmosphere=9, SunCorona=10, Ring=11.
    x3::rhi::TextureHandle              atmoTex{};   // Atmosphere shell ramp (tex[0]); inflated sphere
    x3::rhi::TextureHandle              coronaTex{}; // SunCorona map (tex[0]); inflated sphere, animated
    x3::rhi::TextureHandle              ringTex{};   // Ring radial strip (tex[0]); flat annulus
};

// Build the UV-sphere (writes `outMesh`) + load every planet's textures, returning
// the list of bodies with their default nightsky positions/radii. `nTexFail` is
// incremented per missing file. The texture cache de-dupes shared maps. `logTag`
// prefixes the load logs so the calling path is clear.
inline std::vector<NightSkyPlanet> loadNightSkyPlanets(
        x3::rhi::IRenderDevice* device, x3::rhi::MeshHandle& outMesh,
        int& nTexFail, const char* logTag,
        x3::rhi::MeshHandle* outRingMesh = nullptr) {
    const std::string kPlanets = "C:/Users/Tim/X3/Assets/FORGE3D/Planets/";
    const std::string kAtmo    = kPlanets + "Atmosphere/";
    const std::string kMisc    = kPlanets + "Misc/Textures/";

    std::unordered_map<std::string, x3::rhi::TextureHandle> texCache;
    auto loadTex = [&](const std::string& path, bool srgb) -> x3::rhi::TextureHandle {
        std::string key = path + (srgb ? "#s" : "#l");
        auto it = texCache.find(key);
        if (it != texCache.end()) return it->second;
        int w = 0, h = 0, comp = 0;
        stbi_uc* px = stbi_load(path.c_str(), &w, &h, &comp, 4);   // force RGBA8
        x3::rhi::TextureHandle handle{};
        if (!px) {
            x3::logError(std::string(logTag) + ": FAILED to load " + path);
            ++nTexFail;
        } else {
            handle = device->createTexture(px, (uint32_t)w, (uint32_t)h, srgb);
            stbi_image_free(px);
            x3::logInfo(std::string(logTag) + ": loaded " + path + " (" + std::to_string(w) + "x" +
                        std::to_string(h) + (srgb ? ", srgb)" : ", linear)"));
        }
        texCache[key] = handle;
        return handle;
    };

    std::vector<NightSkyPlanet> bodies;
    // Moon (type 0): tex[0]=Albedo(s) [1]=Normal(l) [2]=Detail(s) [3]=Spec(l) [4]=Scatter(s)
    {
        const std::string p = kPlanets + "Moon/Textures/";
        bodies.push_back({ 0u, {
            loadTex(p + "moon_02.png",        true),
            loadTex(p + "moon_02_normal.png", false),
            loadTex(p + "moon_01_detail.png", true),
            loadTex(p + "moon_02_spec.png",   false),
            loadTex(kAtmo + "sunset_yellow_05.png", true),
        }, { -42.0f, 30.0f, -100.0f }, 15.0f, "Moon" });
    }
    // Ice (type 1): tex[0]=ColorMap(s) [1]=Normal(l) [2]=Height(l) [3]=Detail(l) [4]=Scatter(s)
    {
        const std::string p = kPlanets + "Ice/Textures/";
        bodies.push_back({ 1u, {
            loadTex(p + "ColorMapSqr.png",  true),
            loadTex(p + "ice_01_normal.png", false),
            loadTex(p + "ice_01.png",        false),
            loadTex(p + "icedetail_01.png",  false),
            loadTex(kAtmo + "sunset_blue_03.png", true),
        }, { 86.0f, 34.0f, -130.0f }, 22.0f, "Ice" });
    }
    // Gas (type 2): tex[0]=HeightBands(s) [1]=UVDistortion(l) [2]=Scatter(s)
    {
        const std::string p = kPlanets + "Gas/Textures/";
        bodies.push_back({ 2u, {
            loadTex(p + "planet_gas_03.png",  true),
            loadTex(p + "planet_gas_08.png",  false),
            loadTex(kAtmo + "sunset_yellow_01.png", true),
        }, { 18.0f, 8.0f, -210.0f }, 42.0f, "Gas" });
    }
    // Lava (type 3): tex[0]=Height(s) [1]=Detail(l) [2]=Magma(l) [3]=Normal(l) [4]=Distortion(s) [5]=Scatter(s)
    {
        const std::string lp = kPlanets + "Lava/Textures/";
        const std::string ip = kPlanets + "Ice/Textures/";
        bodies.push_back({ 3u, {
            loadTex(lp + "lava_01.png",        true),
            loadTex(lp + "lavadetail_01.png",  false),
            loadTex(lp + "lavadetail_01.png",  false),
            loadTex(ip + "ice_04_normal.png",  false),
            loadTex(lp + "lavadistmap.png",    true),
            loadTex(kAtmo + "sunset_red_04.png", true),
        }, { -95.0f, -20.0f, -120.0f }, 20.0f, "Lava" });
    }
    // Terrestrial (type 4): tex[0]=Height(s) [1]=LandMask(l) [2]=Normal(l) [3]=Scatter(s)
    //   [4]=Gradient(l) [5]=CloudsTop(s) [6]=CloudsMiddle(s) [7]=CityLight(l)
    //   [8]=CityLightUV(l) [9]=CityLightMask(l)
    {
        const std::string p = kPlanets + "Terrestrial/Textures/";
        bodies.push_back({ 4u, {
            loadTex(p + "terrestrialdetail_01.png",        true),
            loadTex(p + "landmask_01.png",                 false),
            loadTex(p + "terrestrialdetail_01_normal.png", false),
            loadTex(kAtmo + "sunset_yellow_05.png",        true),
            loadTex(kMisc + "polegradient_01.png",         false),
            loadTex(p + "cloudscap_01.png",                true),
            loadTex(p + "clouds_01.png",                   true),
            loadTex(p + "lights_01.png",                   false),
            loadTex(p + "lights_01_uv.png",                false),
            loadTex(p + "lights_01_mask.png",              false),
        }, { 60.0f, -30.0f, -135.0f }, 26.0f, "Terrestrial" });
    }
    // Sun (type 8): tex[0]=SurfaceMap(l) [1]=DistortionMap(l) — emissive, small+bright.
    {
        const std::string sp = kPlanets + "Sun/Textures/";
        const std::string tp = kPlanets + "Thunderstorm/Textures/";
        bodies.push_back({ 8u, {
            loadTex(sp + "sunsurface_01.png", false),
            loadTex(tp + "storm_02.png",      false),
        }, { -14.0f, 40.0f, -70.0f }, 11.0f, "Sun" });
    }

    // ---- TRANSPARENT glow layers (additive atmosphere + sun corona; alpha ring) ----
    // Load the three extra maps + attach them to the matching bodies so the shells
    // draw with the same world position as their body. Texture roles per the port
    // headers + TEXTURE_MANIFEST:
    //   Atmosphere shell : _AtmosphereSample = Atmosphere/Atmosphere_01.png horizon
    //                      gradient ramp (sRGB, CLAMP_TO_EDGE). On the Terrestrial.
    //   Sun corona       : _CoronaMap = Sun/Textures/suncorona_01.png grayscale flow
    //                      atlas (LINEAR). On the Sun (animated via uTime).
    //   Ring             : _DetailMap = Gas/Textures/ring_01.png radial strip (sRGB,
    //                      CLAMP_TO_EDGE; RGB=color, R=alpha). Around the Gas giant.
    x3::rhi::TextureHandle atmoTex   = loadTex(kAtmo + "Atmosphere_01.png", true);
    x3::rhi::TextureHandle coronaTex = loadTex(kPlanets + "Sun/Textures/suncorona_01.png", false);
    x3::rhi::TextureHandle ringTex   = loadTex(kPlanets + "Gas/Textures/ring_01.png", true);
    for (NightSkyPlanet& b : bodies) {
        if (b.typeIndex == 4u) b.atmoTex   = atmoTex;    // Terrestrial -> atmosphere shell
        if (b.typeIndex == 8u) b.coronaTex = coronaTex;  // Sun         -> corona halo
        if (b.typeIndex == 2u) b.ringTex   = ringTex;    // Gas         -> ring annulus
    }

    // ONE UV-sphere mesh (unit radius; pos == normal for the triplanar shading).
    x3::prims::PrimMesh sphere = x3::prims::makeUVSphere(64, 128);
    outMesh = device->createMesh(
        sphere.verts.data(), (uint32_t)sphere.verts.size(),
        sphere.index.data(),  (uint32_t)sphere.index.size());

    // Flat annulus for the ring (object-space radii match planet_ring.frag's
    // hardcoded inner=1.3 / outer=2.5; the model matrix sizes it to the gas giant).
    if (outRingMesh) {
        x3::prims::PrimMesh ring = x3::prims::makeRing(1.3f, 2.5f, 128);
        *outRingMesh = device->createMesh(
            ring.verts.data(), (uint32_t)ring.verts.size(),
            ring.index.data(),  (uint32_t)ring.index.size());
    }

    return bodies;
}

// Draw every planet for the current frame (call AFTER the scene's own draws so the
// depth buffer occludes correctly). Each body uses its per-type planet pipeline.
inline void drawNightSkyPlanets(x3::rhi::IRenderDevice* device, const x3::rhi::FrameContext& fc,
                                x3::rhi::MeshHandle mesh,
                                const std::vector<NightSkyPlanet>& planets, float uTime,
                                x3::rhi::MeshHandle ringMesh = {}) {
    if (!fc.valid) return;
    // PlanetType transparent indices (see VulkanRenderDevice PlanetType enum).
    constexpr uint32_t kAtmosphere = 9u, kSunCorona = 10u, kRing = 11u;
    for (const NightSkyPlanet& b : planets) {
        const float r = b.radius;
        // OPAQUE body: uniform scale by the apparent radius, translated to world pos.
        const float model[16] = {
            r, 0, 0, 0,
            0, r, 0, 0,
            0, 0, r, 0,
            b.worldPos[0], b.worldPos[1], b.worldPos[2], 1,
        };
        device->drawPlanet(fc, mesh, model, b.typeIndex,
                           b.maps.data(), (uint32_t)b.maps.size(), uTime);

        // --- ADDITIVE atmosphere shell: same sphere, inflated ~1.06x the body. ---
        if (b.atmoTex.valid()) {
            const float s = r * 1.06f;
            const float m[16] = { s,0,0,0, 0,s,0,0, 0,0,s,0,
                                  b.worldPos[0], b.worldPos[1], b.worldPos[2], 1 };
            x3::rhi::TextureHandle t[1] = { b.atmoTex };
            device->drawPlanet(fc, mesh, m, kAtmosphere, t, 1u, uTime);
        }
        // --- ADDITIVE sun corona: same sphere, big shell ~2.2x the Sun, animated. ---
        if (b.coronaTex.valid()) {
            const float s = r * 2.2f;
            const float m[16] = { s,0,0,0, 0,s,0,0, 0,0,s,0,
                                  b.worldPos[0], b.worldPos[1], b.worldPos[2], 1 };
            x3::rhi::TextureHandle t[1] = { b.coronaTex };
            device->drawPlanet(fc, mesh, m, kSunCorona, t, 1u, uTime);
        }
        // --- ALPHA ring: flat annulus, tilted, uniformly scaled by the body radius. ---
        // The ring mesh is authored in object space at inner=1.3 / outer=2.5 (the
        // frag's hardcoded radii). A uniform scale by `r` keeps object-space radii
        // intact (the frag works object-space) and sizes the ring to 1.3r..2.5r in
        // world. A small tilt about X gives the disc some perspective.
        if (b.ringTex.valid() && ringMesh.valid()) {
            const float s = r;
            const float ct = 0.92f, st = 0.39f;   // ~23 deg tilt about X (cos,sin)
            // column-major: tilt(X) * scale(s), translated to the body position.
            const float m[16] = {
                 s,      0,      0,     0,
                 0,    s*ct,   s*st,    0,
                 0,   -s*st,   s*ct,    0,
                 b.worldPos[0], b.worldPos[1], b.worldPos[2], 1,
            };
            x3::rhi::TextureHandle t[1] = { b.ringTex };
            device->drawPlanet(fc, ringMesh, m, kRing, t, 1u, uTime);
        }
    }
}

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
inline void applyShowroomTimeOfDay(
        x3::rhi::IRenderDevice* device, bool day,
        const std::vector<x3::rhi::PointLight>* interiorLights = nullptr) {
    x3::rhi::IRenderDevice::SkyParams sp{};
    sp.enabled = true;
    if (day) {
        // DAY — Unity-match: high winter sun, bright pale winter-blue sky.
        sp.sunDir[0]   = -0.0595f; sp.sunDir[1] = 0.9355f; sp.sunDir[2] = -0.3483f; // TOWARD the sun
        sp.sunColor[0] = 1.00f;  sp.sunColor[1] = 0.98f; sp.sunColor[2] = 0.95f;    // warm-neutral
        sp.sunIntensity = 3.4f;   // bright key (winter midday)
        sp.haze = 0.5f; sp.exposure = 0.92f;   // just under 1.0 so the bright floors don't blow out
        sp.zenith[0]  = 0.20f; sp.zenith[1]  = 0.34f; sp.zenith[2]  = 0.62f;        // pale winter-blue
        sp.horizon[0] = 0.72f; sp.horizon[1] = 0.80f; sp.horizon[2] = 0.92f;        // warm-grey/white haze
        device->setSkyParams(sp);
        device->setAmbient(0.48f, 0.52f, 0.62f);   // BRIGHT cool snow-bounce high-key fill (pulled from 0.55 so floors don't blow)
        device->setBloom(0.12f);                    // low: let white panels bloom only slightly
    } else {
        // NIGHT — UNCHANGED from the original showroom recipe.
        sp.sunDir[0] = 0.6f; sp.sunDir[1] = 0.42f; sp.sunDir[2] = -0.2f;   // low raking MOON
        sp.sunColor[0] = 1.0f; sp.sunColor[1] = 0.96f; sp.sunColor[2] = 0.90f;
        sp.sunIntensity = 0.25f;   // cool moonlight (still casts shadows)
        sp.haze = 0.15f; sp.exposure = 0.62f;
        sp.zenith[0]  = 0.012f; sp.zenith[1]  = 0.012f; sp.zenith[2]  = 0.028f;   // near-black zenith
        sp.horizon[0] = 0.10f;  sp.horizon[1] = 0.13f;  sp.horizon[2] = 0.20f;    // faint cool horizon
        device->setSkyParams(sp);
        device->setAmbient(0.09f, 0.10f, 0.16f);   // cool dim moonlight fill
        device->setBloom(0.22f);                    // HERO glow on the HDR-emissive windows/fixtures
    }
    // Interior point lights: DAY dims them (x0.3) so snow-bounce dominates; NIGHT
    // uses the full-intensity set.  Color channels are pre-multiplied by intensity.
    if (interiorLights) {
        if (day) {
            std::vector<x3::rhi::PointLight> dim = *interiorLights;
            for (x3::rhi::PointLight& pl : dim) {
                pl.color[0] *= 0.3f; pl.color[1] *= 0.3f; pl.color[2] *= 0.3f;
            }
            device->setPointLights(dim.data(), (uint32_t)dim.size());
        } else {
            device->setPointLights(interiorLights->data(), (uint32_t)interiorLights->size());
        }
    }
    // Interior reflection probe: bake the IBL env from the showroom geometry (around the
    // camera) instead of the open sky, so the glossy/metallic Unity panels reflect the
    // dim interior rather than the bright sky (which blows them out to white).
    device->setIblProbe(true);
}

// Read the DAY-vs-NIGHT selection for the SHOWROOM. Default = NIGHT (unchanged).
// DAY is opted into via the env X3_SHOWROOM_DAY=1 (so the headless proofs
// --screenshot-showroom / -fp / -floor2 can capture DAY) OR the in-game 'T'
// toggle (interactive path flips a runtime bool seeded from this).
inline bool showroomDayDefault() {
    const char* e = std::getenv("X3_SHOWROOM_DAY");
    return e != nullptr && e[0] != '0' && e[0] != '\0';
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

// KEYPAD code for the --world showroom HIDDEN HATCH (the concealed floor panel that
// opens the stair down to the glass elevator). The hatch is gated by entering THIS
// code on a keypad (reusing x3::game::KeypadEntry, the same state machine driven by
// --test-doorcode / the Level-1 §6.4 door gate). Deliberately NOT 1127 (that is the
// Spire/Club secret). Themed "ARIA" on a phone keypad (A=2,R=7,I=4,A=2). *** CHANGE
// HERE to re-key the hatch. *** Also exercised headless by --test-hatchcode.
constexpr int kShowroomHatchCode = 2742;

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
// --test-frustumcull : CPU per-object frustum-cull self-test (D15 baseline).
//
// Builds a KNOWN camera frustum (same viewProj convention the render device uses:
// glm::perspective, reverse-Y for Vulkan clip) and a KNOWN set of world spheres at
// known positions, then runs the EXACT engine cull math (engine/rhi/FrustumCull.h:
// extractFrustumPlanes + sphereInFrustum — the same functions VulkanRenderDevice
// calls) and asserts the precise survivor set, incl. the edge cases the GPU
// cull.comp must also honor:
//   * dead-ahead object inside the frustum                -> KEPT
//   * object far off to the side (outside left/right)     -> CULLED
//   * object fully BEHIND the camera (behind near plane)  -> CULLED
//   * sphere STRADDLING a plane (|dist| < radius)         -> KEPT (conservative)
//   * object beyond the far plane                         -> CULLED
//   * ALWAYS_VISIBLE bypass (caller skips the test)       -> KEPT even if outside
// Prints "frustumcull: X/Y passed" and returns true iff all pass. No GPU/window.
static bool runFrustumCullSelfTest() {
    using namespace x3::rhi;
    int passed = 0, total = 0;
    auto check = [&](const char* name, bool ok) {
        ++total; if (ok) ++passed;
        x3::logInfo(std::string("  [") + (ok ? "PASS" : "FAIL") + "] " + name);
    };

    // Camera at origin looking down -Z (right-handed), 90deg FOV, aspect 1, near
    // 0.1, far 100. Reverse-Y exactly as VulkanRenderDevice::prepareFrameData does.
    const glm::vec3 eye(0.0f, 0.0f, 0.0f);
    glm::mat4 view = glm::lookAt(eye, eye + glm::vec3(0, 0, -1), glm::vec3(0, 1, 0));
    glm::mat4 proj = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 100.0f);
    proj[1][1] *= -1.0f;
    const FrustumPlanes fr = extractFrustumPlanes(proj * view);

    auto keep = [&](const glm::vec3& c, float r) {
        return sphereInFrustum(fr, CullSphere(c, r));
    };

    // (1) dead-ahead, well inside -> kept.
    check("ahead-inside kept", keep(glm::vec3(0, 0, -10), 1.0f));
    // (2) far off to the +X side at modest depth (way outside the 90deg cone) -> culled.
    check("far-right outside culled", !keep(glm::vec3(100, 0, -10), 1.0f));
    // (3) fully behind the camera (+Z) -> culled (behind near plane).
    check("behind-camera culled", !keep(glm::vec3(0, 0, 10), 1.0f));
    // (4) center just behind the near plane (z=+0.05) but radius 0.2 reaches across
    //     the near plane -> STRADDLES -> kept (conservative). Distance to near plane
    //     is 0.15 < r, so the test must keep it.
    check("straddle-near kept", keep(glm::vec3(0, 0, 0.05f), 0.2f));
    // (5) beyond the far plane (z=-150, far=100), small radius -> culled.
    check("beyond-far culled", !keep(glm::vec3(0, 0, -150), 1.0f));
    // (6) a tiny sphere exactly ON the right frustum edge at z=-10: at 90deg FOV the
    //     edge is x = |z| = 10. Center slightly OUTSIDE (x=11) with radius 0.5 (does
    //     not reach back to the plane) -> culled; radius 2.0 reaches the plane -> kept.
    check("edge-outside small culled", !keep(glm::vec3(11, 0, -10), 0.5f));
    check("edge-straddle large kept",   keep(glm::vec3(11, 0, -10), 2.0f));
    // (7) ALWAYS_VISIBLE bypass: an object far outside is KEPT because the caller
    //     never runs the test for noCull instances. Model the device's own guard.
    {
        const bool noCull = true;
        const glm::vec3 cOut(100, 100, 100);  // nowhere near the frustum
        const bool drawn = noCull ? true : keep(cOut, 1.0f);
        check("ALWAYS_VISIBLE kept", drawn);
    }
    // (8) world-transform path: a unit-radius local sphere translated to an inside
    //     position via a model matrix, with non-uniform scale (max axis grows the
    //     radius). Mirrors VulkanRenderDevice::worldSphere usage.
    {
        glm::mat4 m(1.0f);
        m = glm::translate(m, glm::vec3(0, 0, -20));
        m = glm::scale(m, glm::vec3(3.0f, 1.0f, 1.0f));  // max scale axis = 3
        const glm::vec3 cW = glm::vec3(m * glm::vec4(0, 0, 0, 1));
        const float rW = 1.0f * 3.0f;                    // local r=1 * maxScale
        check("world-xform inside kept", keep(cW, rW));
    }

    x3::logInfo("frustumcull: " + std::to_string(passed) + "/" +
                std::to_string(total) + " passed");
    return passed == total;
}

// ---------------------------------------------------------------------------
// --test-gpucull : D15 Tier-0 GPU cull EQUIVALENCE test (the soul of D15).
//
// Drives the REAL Vulkan device HEADLESS (validation ON) with the GPU cull path
// active (r_cullpath 1) AND the device's equivalence harness on: every frame the
// CPU evaluates the IDENTICAL cull predicate per instance and the device compares
// the GPU cull.comp's statDrawn readback against it. A grid of cube instances is
// rendered from multiple camera poses (full-cull, no-cull, partial, skewed) and
// the test asserts:
//   (a) the GPU path actually engaged (stats().gpuCullPath == 1),
//   (b) tested == submitted instance count at every sampled pose,
//   (c) drawn + frustumCulled == tested (no instance lost or double-counted),
//   (d) ZERO equivalence mismatches across every compared frame,
//   (e) the ALWAYS_VISIBLE bypass (r_frustumcull 0) draws every instance,
//   (f) toggling back to the CPU path (r_cullpath 0) restores the identity
//       indirection and keeps rendering (no stale compaction).
// ---------------------------------------------------------------------------
static bool runGpuCullSelfTest() {
    using namespace x3::rhi;
    int passed = 0, total = 0;
    auto check = [&](const char* name, bool ok) {
        ++total; if (ok) ++passed;
        x3::logInfo(std::string("  [gpucull] ") + (ok ? "PASS " : "FAIL ") + name);
    };

    if (!glfwInit()) { x3::logError("[gpucull] glfwInit failed"); return false; }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    std::unique_ptr<IRenderDevice> device(createRenderDevice());
    DeviceDesc desc{};
    desc.width = 640; desc.height = 360; desc.headless = true;
    desc.validation = true;   // this test doubles as the Tier-0 validation gate
    if (!device->init(desc)) {
        x3::logError("[gpucull] device init failed");
        glfwTerminate(); return false;
    }

    // Two distinct meshes (two indirect commands) so compaction bases differ:
    // a unit cube and a flat slab.
    auto makeBox = [&](float hx, float hy, float hz) {
        const float px[8] = { -hx,  hx,  hx, -hx, -hx,  hx,  hx, -hx };
        const float py[8] = { -hy, -hy, -hy, -hy,  hy,  hy,  hy,  hy };
        const float pz[8] = { -hz, -hz,  hz,  hz, -hz, -hz,  hz,  hz };
        MeshVertex v[8]{};
        for (int i = 0; i < 8; ++i) {
            v[i].pos[0] = px[i]; v[i].pos[1] = py[i]; v[i].pos[2] = pz[i];
            v[i].normal[1] = 1.0f;
        }
        const uint32_t idx[36] = { 0,1,2, 0,2,3,  4,6,5, 4,7,6,  0,4,5, 0,5,1,
                                   3,2,6, 3,6,7,  1,5,6, 1,6,2,  0,3,7, 0,7,4 };
        return device->createMesh(v, 8, idx, 36);
    };
    MeshHandle cube = makeBox(0.5f, 0.5f, 0.5f);
    MeshHandle slab = makeBox(2.0f, 0.1f, 2.0f);
    if (!cube.valid() || !slab.valid()) {
        x3::logError("[gpucull] mesh create failed");
        device->shutdown(); glfwTerminate(); return false;
    }

    // 24x24 cube grid + a 7x7 slab grid = 625 instances over [-69..69]^2 on XZ.
    constexpr uint32_t kGrid = 24, kSlabGrid = 7;
    constexpr uint32_t kInstances = kGrid * kGrid + kSlabGrid * kSlabGrid;
    const float white[4] = { 1, 1, 1, 1 };
    auto submitScene = [&](const x3::rhi::FrameContext& fc) {
        float m[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        for (uint32_t z = 0; z < kGrid; ++z)
            for (uint32_t x = 0; x < kGrid; ++x) {
                m[12] = -69.0f + 6.0f * x; m[13] = 0.5f; m[14] = -69.0f + 6.0f * z;
                device->drawMesh(fc, cube, {}, white, m);
            }
        for (uint32_t z = 0; z < kSlabGrid; ++z)
            for (uint32_t x = 0; x < kSlabGrid; ++x) {
                m[12] = -60.0f + 20.0f * x; m[13] = 4.0f; m[14] = -60.0f + 20.0f * z;
                device->drawMesh(fc, slab, {}, white, m);
            }
    };
    auto renderFrames = [&](int n, float cx, float cy, float cz, float yaw, float pitch) {
        device->setCamera(cx, cy, cz, yaw, pitch, 70.0f);
        for (int i = 0; i < n; ++i) {
            auto fc = device->beginFrame();
            if (!fc.valid) return false;
            submitScene(fc);
            device->endFrame(fc);
        }
        return true;
    };

    device->setGpuCullEquivalenceCheck(true);
    device->setCullPath(1);                  // Tier 0 (graphics-queue compute)
    device->setFrustumCullEnabled(true);

    // Pose sweep. 4 frames per pose so the frames-in-flight readback latency
    // drains and the sampled stats describe THIS pose.
    struct Pose { const char* name; float x, y, z, yaw, pitch; bool expectSomeCulled, expectSomeDrawn; };
    const Pose poses[] = {
        { "center +X",      0.0f, 2.0f,   0.0f, 0.0f,  0.0f,  true,  true  },
        { "center skewed",  10.0f, 3.0f,  -8.0f, 2.4f, -0.3f, true,  true  },
        { "straight down",  0.0f, 40.0f,  0.0f, 0.0f, -1.55f, true,  true  },
        { "outside looking away", 200.0f, 2.0f, 0.0f, 0.0f, 0.0f, true, false },
        { "far overview (sees all)", 0.0f, 150.0f, 190.0f, -1.5708f, -0.65f, false, true },
    };
    for (const Pose& p : poses) {
        if (!renderFrames(4, p.x, p.y, p.z, p.yaw, p.pitch)) {
            check((std::string(p.name) + ": render frames").c_str(), false);
            continue;
        }
        const RenderStats st = device->stats();
        check((std::string(p.name) + ": gpu path active").c_str(), st.gpuCullPath == 1);
        check((std::string(p.name) + ": tested == submitted").c_str(), st.gpuCullTested == kInstances);
        check((std::string(p.name) + ": drawn + frustumCulled == tested").c_str(),
              st.gpuCullDrawn + st.gpuCullFrustum == st.gpuCullTested && st.gpuCullHzb == 0);
        if (p.expectSomeCulled) check((std::string(p.name) + ": culls something").c_str(), st.gpuCullFrustum > 0);
        if (p.expectSomeDrawn)  check((std::string(p.name) + ": draws something").c_str(), st.gpuCullDrawn > 0);
        else                    check((std::string(p.name) + ": draws nothing").c_str(), st.gpuCullDrawn == 0);
        check((std::string(p.name) + ": GPU drawn == CPU expected").c_str(),
              st.gpuCullDrawn == st.gpuCullExpected);
    }

    // (e) ALWAYS_VISIBLE bypass: r_frustumcull 0 -> every instance survives.
    device->setFrustumCullEnabled(false);
    if (renderFrames(4, 0.0f, 2.0f, 0.0f, 0.0f, 0.0f)) {
        const RenderStats st = device->stats();
        check("bypass (r_frustumcull 0): drawn == tested",
              st.gpuCullDrawn == st.gpuCullTested && st.gpuCullTested == kInstances);
    } else check("bypass render", false);
    device->setFrustumCullEnabled(true);

    // TIER 1 (async compute queue) — same predicate, same equivalence, on the
    // 5090's dedicated compute queue. If the device has no dedicated queue the
    // path resolves back to 1 (also asserted: never 2 without support).
    device->setCullPath(2);
    if (renderFrames(4, 0.0f, 2.0f, 0.0f, 0.0f, 0.0f)) {
        const RenderStats st = device->stats();
        const bool tier1 = st.gpuCullPath == 2;
        check(tier1 ? "tier1: async path active" : "tier1: clamped to tier0 (no dedicated queue)",
              st.gpuCullPath == 2 || st.gpuCullPath == 1);
        check("tier1: tested == submitted", st.gpuCullTested == kInstances);
        check("tier1: drawn + frustumCulled == tested",
              st.gpuCullDrawn + st.gpuCullFrustum == st.gpuCullTested);
        check("tier1: GPU drawn == CPU expected", st.gpuCullDrawn == st.gpuCullExpected);
    } else check("tier1 render", false);
    if (renderFrames(4, 10.0f, 3.0f, -8.0f, 2.4f, -0.3f)) {
        const RenderStats st = device->stats();
        check("tier1 skewed: GPU drawn == CPU expected",
              st.gpuCullDrawn == st.gpuCullExpected && st.gpuCullDrawn > 0);
    } else check("tier1 skewed render", false);
    device->setCullPath(1);

    // (d) zero mismatches across every compared frame so far.
    {
        const RenderStats st = device->stats();
        check("equivalence frames compared > 0", st.gpuCullEquivFrames > 0);
        check("equivalence mismatches == 0", st.gpuCullEquivMismatches == 0);
    }

    // (f) toggle back to the CPU path: identity restored, still renders, and the
    // CPU cull draws the same survivor count the GPU just computed for this pose.
    device->setCullPath(0);
    bool cpuOk = renderFrames(3, 0.0f, 2.0f, 0.0f, 0.0f, 0.0f);
    const RenderStats stCpu = device->stats();
    check("toggle to CPU path renders", cpuOk);
    check("CPU path resolves to 0", stCpu.gpuCullPath == 0);
    check("CPU objectsDrawn > 0 after toggle", stCpu.objectsDrawn > 0);

    device->shutdown();
    device.reset();
    glfwTerminate();

    x3::logInfo("gpucull: " + std::to_string(passed) + "/" + std::to_string(total) + " passed");
    return passed == total;
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
         testBlockout = false,
         testBarrels = false, testGlass = false, testHoloterm = false, testEcs = false, testEcsRender = false,
         testFrustumCull = false,
         testCombat = false, testAudio = false, testLevel1 = false, testJobs = false,
         testPhase2a = false, testPhase2b = false, testAnim = false, testTerrain = false,
         testStreaming = false, testAi = false, testDoorCode = false, testElevator = false,
         testElevatorFsm = false,
         testTerrainPlace = false, testNet = false, testRescue = false, testDestruction = false,
         testNav = false, testWeapons = false, testVehicle = false, testFootIk = false,
         testNetSync = false, testNetInterp = false, testNetPredict = false, testNpcTalk = false,
         testDeathRagdoll = false, testCanonLevel = false, testCanonPlay = false,
         testThirdPerson = false, testHatchCode = false;
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
    // DDGI gate-shot proof (--screenshot-ddgi [outDir]): build a minimal sealed
    // two-room rig (room A holds a point light + an emissive ceiling panel; room B
    // is connected only through a doorway; room C is fully SEALED next to A — the
    // leak canary), render OFF/ON/debug captures headless and exit. Probes converge
    // over ~120 settle frames before each ON capture.
    bool        ddgiShot = false;
    std::string ddgiShotDir = "docs/screenshots/ddgi";
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
        // DDGI flags handled OUTSIDE the big else-if chain below (MSVC C1061:
        // every `else if` nests a block; the chain is at the compiler's limit).
        if (a == "--test-ddgi") { smoketest = true; testDdgi = true; continue; }
        if (a == "--ddgi") { ddgiForce = true; continue; }
        if (a == "--screenshot-ddgi") {
            ddgiShot = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') ddgiShotDir = argv[++i];
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
        else if (a == "--test-blockout") testBlockout = true;
        else if (a == "--test-barrels") testBarrels = true;
        else if (a == "--test-glass") testGlass = true;
        else if (a == "--test-frustumcull") testFrustumCull = true;
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
        else if (a == "--test-act2desert") testAct2Desert = true;
        else if (a == "--test-act2caves") testAct2Caves = true;
        else if (a == "--test-tod") testTod = true;
        else if (a == "--test-weather") testWeather = true;
        else if (a == "--test-worldregions") testWorldRegions = true;
        else if (a == "--test-city") testCity = true;
        else if (a == "--test-oceanbase") testOceanBase = true;
        else if (a == "--test-doorcode") testDoorCode = true;
        else if (a == "--test-hatchcode") testHatchCode = true;
        else if (a == "--test-elevator") testElevator = true;
        else if (a == "--test-elevatorfsm") testElevatorFsm = true;
        else if (a == "--test-net") testNet = true;
        else if (a == "--test-netsync") testNetSync = true;
        else if (a == "--test-netinterp") testNetInterp = true;
        else if (a == "--test-netpredict") testNetPredict = true;
        else if (a == "--test-rescue") testRescue = true;
        else if (a == "--test-thirdperson") testThirdPerson = true;
        else if (a == "--test-npctalk") testNpcTalk = true;
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
        else if (a == "--test-weapons") testWeapons = true;
        else if (a == "--test-vehicle") testVehicle = true;
        else if (a == "--test-footik") testFootIk = true;
        else if (a == "--test-ui") testUi = true;
        else if (a == "--test-loading") testLoading = true;
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
        else if (a == "--editor") editorMode = true;
        else if (a == "--screenshot-editor") {
            editorShot = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') editorShotPath = argv[++i];
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
    if (testBlockout) {
        x3::logInfo("running Level Architect BLOCKOUT (brushes[] JSON / snap / mesh) self-test...");
        return x3::editor::runBlockoutSelfTest() ? 0 : 1;
    }
    if (testBarrels) {
        x3::logInfo("running explosive-barrels self-test...");
        return x3::game::runBarrelSelfTest() ? 0 : 1;
    }
    if (testGlass) {
        x3::logInfo("running translucent-glass material (M1 see-through) self-test...");
        return x3::game::runGlassSelfTest() ? 0 : 1;
    }
    if (testFrustumCull) {
        x3::logInfo("running CPU per-object frustum-cull (D15 baseline) self-test...");
        return runFrustumCullSelfTest() ? 0 : 1;
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
    if (testIntro) {
        x3::logInfo("running intro cold-open self-test (flight -> hit -> whiteout -> titlecard -> handoff; skippable)...");
        return x3::intro::runIntroSelfTest() ? 0 : 1;
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
    if (testDoorCode) {
        x3::logInfo("running door-code keypad (locked coded door) self-test (K1-K6)...");
        return x3::game::runDoorCodeSelfTest() ? 0 : 1;
    }
    if (testHatchCode) {
        // Showroom HIDDEN-HATCH keypad smoke (H1-H4). Drives the SAME KeypadEntry state
        // machine + the SAME submit comparison (value() == kShowroomHatchCode) the
        // --world showroom hatch uses, headlessly: a wrong code is rejected (buffer
        // cleared, hatch stays shut), the correct code (with a typo + backspace fixup)
        // is accepted and "opens" the hatch. Pure logic — no Vulkan/GLFW needed.
        x3::logInfo("running showroom hidden-hatch keypad smoke (H1-H4)...");
        bool ok = true;
        x3::game::KeypadEntry kp;
        bool hatchOpen = false;
        auto submit = [&](){
            if (kp.value() == kShowroomHatchCode) { hatchOpen = true; kp.clear(); return true; }
            kp.clear(); return false;  // wrong -> clear, stay shut (mirrors DENIED)
        };
        // H1: a WRONG code is rejected and the hatch stays shut.
        for (int d : {1,2,3,4}) kp.pushDigit(d);
        bool h1 = (!submit() && !hatchOpen);
        ok = ok && h1; x3::logInfo(std::string("  H1 wrong-code rejected: ") + (h1?"PASS":"FAIL"));
        // H2: digits append + Backspace fixes a typo so the buffer == the code.
        const int code = kShowroomHatchCode;                 // 4-digit (2742)
        kp.clear();
        kp.pushDigit((code/1000)%10);
        kp.pushDigit((code/100)%10);
        kp.pushDigit(9);            // deliberate typo
        kp.backspace();             // ...corrected
        kp.pushDigit((code/10)%10);
        kp.pushDigit(code%10);
        bool h2 = (kp.value() == code);
        ok = ok && h2; x3::logInfo(std::string("  H2 digit/backspace -> code: ") + (h2?"PASS":"FAIL"));
        // H3: submitting the CORRECT code opens the hatch + clears the buffer.
        bool h3 = (submit() && hatchOpen && kp.empty());
        ok = ok && h3; x3::logInfo(std::string("  H3 correct-code opens hatch: ") + (h3?"PASS":"FAIL"));
        // H4: the code is NOT the Spire/Club 1127 secret (guards against re-keying drift).
        bool h4 = (kShowroomHatchCode != 1127);
        ok = ok && h4; x3::logInfo(std::string("  H4 code != 1127 (Spire secret): ") + (h4?"PASS":"FAIL"));
        x3::logInfo(std::string("hatch-keypad smoke: ") + (ok?"ALL PASS":"FAILED"));
        return ok ? 0 : 1;
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
    if (testMeshlet) {
        x3::logInfo("running D15 Tier-2 meshlet builder self-test "
                    "(grid mesh -> buildMeshlets -> assert budgets/locality/sphere/"
                    "cone/triangle-conservation/degenerate-input)...");
        return x3::rhi::runMeshletSelfTest() ? 0 : 1;
    }
    if (testGpuCull) {
        x3::logInfo("running D15 Tier-0 GPU cull equivalence self-test "
                    "(headless device + validation, r_cullpath 1, pose sweep, "
                    "GPU statDrawn vs CPU predicate — must match EXACTLY)...");
        return runGpuCullSelfTest() ? 0 : 1;
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
    if (testLoading) {
        x3::logInfo("running EFLZ loading-screen self-test "
                    "(monotonic progress 0->1 + tip rotation + fade-in/out)...");
        return x3::game::runLoadingSelfTest() ? 0 : 1;
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
    const bool headless = smoketest || screenshot || skyShot || ddgiShot || showroomShot || showroomFpShot || showroomRagdollShot || showroomDeckShot || showroomElevShot || showroomStairShot || showroomFloor2Shot || showroomDoorShot || showroomStrutsShot || showroomGalleryShot || showroomCivShot || planetShot || nightskyShot || terrainShot || oceanShot || captureAi || captureWalk || destructShot || captureFootIk || uiDemo || captureSpire || editorShot;

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
    desc.ssaa = (showroomShot || showroomFpShot || showroomRagdollShot || showroomDeckShot || showroomElevShot || showroomStairShot || showroomFloor2Shot || showroomDoorShot || showroomStrutsShot || showroomGalleryShot || showroomCivShot || planetShot || nightskyShot) ? 4u : 1u;   // 4x supersample the showroom / planet / nightsky still (5090 headless: ~16 samples/px, pristine)
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
        // files / slot order / srgb as --screenshot-nightsky). They get RE-POSITIONED
        // below, AFTER the camera is framed, so they hang HIGH ABOVE/BEHIND the spire in
        // the upper frame (and within the 200 m far plane) — not at the nightsky defaults.
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
        const float camx = cx, camy = cy + ey * 0.12f, camz = bmx[2] + dist;   // in front (+Z face)
        float dx = cx - camx, dy = cy - camy, dz = cz - camz;                  // look toward center
        float len = std::sqrt(dx * dx + dy * dy + dz * dz); if (len < 1e-3f) len = 1e-3f;
        const float pitch = std::asin(dy / len);
        const float yaw   = std::atan2(dz, dx);
        x3::logInfo("--screenshot-showroom: cam(" + std::to_string(camx) + "," + std::to_string(camy) + "," +
            std::to_string(camz) + ") yaw=" + std::to_string(yaw) + " pitch=" + std::to_string(pitch));
        device->setCamera(camx, camy, camz, yaw, pitch, 72.0f);
        // Frame the sun's shadow box on the building (+ surrounding firs) so they cast shadows
        // (the default ~45 m camera-following box sits 100+ m short of the building).
        device->setShadowBounds(cx, cy, cz, 150.0f);

        // --- RE-POSITION the planets HIGH ABOVE/BEHIND the spire, in the camera's view
        // direction (toward -Z, beyond the building) so they sit in the UPPER frame above
        // the building. Computed in the CAMERA basis: forward = look dir, plus a world-up
        // lift + a per-body azimuth fan + an "up into the upper third" elevation. Each is
        // placed at a distance kept WITHIN the 200 m far plane (the planet's NEAR edge must
        // clear it, so distance + radius < ~195). varied radius: Terrestrial + Gas prominent,
        // Moon/Ice mid, Sun smaller + bright. None reach the building/forest (all far + high).
        {
            const float cp = std::cos(pitch), spn = std::sin(pitch);
            const float cyw = std::cos(yaw),   syw = std::sin(yaw);
            const float fwd[3]   = { cp * cyw, spn, cp * syw };          // camera forward (look dir)
            const float right[3] = { -syw, 0.0f, cyw };                 // camera right (world-up plane)
            // Per-body placement: distance (m, within far plane), azimuth fan (m, +=right),
            // and lift (m, world-up) so they ride the upper third of the frame.
            struct Place { const char* name; float dist; float side; float lift; float radius; };
            // Distances + lift/side kept so each body's CENTER distance from the camera
            // (sqrt(dist^2 + lift^2 + side^2)) + its radius stays comfortably < the 200 m
            // far plane (largest here ~187 m), so nothing clips at the far plane.
            const Place places[] = {
                // name           dist   side    lift   radius   (lifts lowered so bodies sit in the
                { "Terrestrial",  120.0f, -42.0f,  44.0f, 28.0f },   // upper third, fully framed (not cropped)
                { "Gas",          125.0f,  48.0f,  52.0f, 34.0f },   // prominent giant, upper-right
                { "Moon",         105.0f,   8.0f,  40.0f, 14.0f },   // mid, high-center
                { "Ice",          115.0f, -28.0f,  42.0f, 16.0f },   // mid, upper-left of center
                { "Lava",         128.0f,  32.0f,  38.0f, 15.0f },   // mid, right
                { "Sun",           98.0f, -16.0f,  48.0f,  9.0f },   // small + bright, high-left
            };
            for (NightSkyPlanet& b : planets) {
                const Place* pl = nullptr;
                for (const Place& q : places) if (std::strcmp(q.name, b.name) == 0) { pl = &q; break; }
                if (!pl) continue;
                b.radius = pl->radius;
                b.worldPos[0] = camx + fwd[0] * pl->dist + right[0] * pl->side;
                b.worldPos[1] = camy + fwd[1] * pl->dist + pl->lift;                // world-up lift
                b.worldPos[2] = camz + fwd[2] * pl->dist + right[2] * pl->side;
                x3::logInfo(std::string("--screenshot-showroom: planet ") + b.name + " pos(" +
                    std::to_string(b.worldPos[0]) + "," + std::to_string(b.worldPos[1]) + "," +
                    std::to_string(b.worldPos[2]) + ") r=" + std::to_string(b.radius) +
                    " dist=" + std::to_string(pl->dist));
            }
        }

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
                    drawNightSkyPlanets(device.get(), frame, planetMesh, planets, 10.0f, ringMesh);
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
        const float sunDir[3] = { 0.4f, 0.25f, 0.6f };   // normalized internally
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

        // --- Camera near origin looking toward the -Z cluster, tilted slightly UP so
        // the dome + stars + the staggered bodies fill the frame.
        const float camx = 0.0f, camy = 6.0f, camz = 18.0f;
        const float yaw   = -1.5708f;   // toward -Z (the cluster)
        const float pitch = 0.22f;      // ~13deg up
        const float fovDeg = 75.0f;     // wide so all 6 bodies fit
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
                drawNightSkyPlanets(device.get(), frame, planetMesh, bodies, kUTime, ringMesh);
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
            const int kSettle = ddgiForce ? 120 : 24;   // advance enough for character skinning + bloom (+ DDGI convergence with --ddgi)
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

    // ---- Interactive SHOWROOM walkthrough (--world showroom) ----------------
    // Walk the baked Unity "3D Showroom Level Kit" (Example_01) at night, with the
    // companion Aria standing a few metres away and the wheeling FORGE3D night sky
    // overhead. Built entirely through the public Scene/device/physics API (like
    // `--world club`) so it stays LOW-CONFLICT with Level 1 / the Spire. Two ways in:
    //   * WALKABLE (windowed): `--world showroom` — the Level-1 walking controller +
    //     physics, an E-to-talk exchange with Aria (she becomes a companion), Esc quit.
    //   * HEADLESS PROOF: `--screenshot-showroom-fp <path>` — same setup, but render
    //     ONE frame from the player spawn eye (first person) + capture a PNG, then exit
    //     (the only way to verify the walkable interior content without live input).
    //
    // EnvArtSystem is PURELY VISUAL (no collision bodies) + its bounds are origin-only,
    // so we SYNTHESIZE a flat ground slab under the building footprint (see floorY note
    // below) + a perimeter wall ring so the player can't walk off the world.
    if (worldMode == "showroom") {
        x3::logInfo("--world showroom: building the interactive night showroom walkthrough");

        // Physics world for the showroom (separate from the Level-1 path below).
        std::unique_ptr<x3::phys::IPhysicsWorld> sphys(x3::phys::createPhysicsWorld());
        if (!sphys->init()) {
            x3::logError("--world showroom: physics init failed");
            device->shutdown();
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return 1;
        }

        x3::game::Scene sscene;

        // Load the baked Unity scene export (same GLB / path as --screenshot-showroom).
        x3::game::EnvArtSystem showroom;
        const bool envOk = showroom.buildFromGlb(*device, x3::game::convertedGlbRoot(),
                                                 "ShowRoom_Vol30/Example_01.glb");
        if (!envOk) x3::logError("--world showroom: scene GLB failed to load (floor + Aria still build)");

        // ---- DAY<->NIGHT toggle state -------------------------------------------------
        // gShowroomDay flips the whole sky/sun/ambient/bloom/interior-point-light recipe
        // via applyShowroomTimeOfDay(). Default = NIGHT (unchanged); X3_SHOWROOM_DAY=1
        // seeds DAY (for the headless proofs); the live loop flips it with the 'T' key.
        bool gShowroomDay = showroomDayDefault();
        // The CIVILIAN proof is a DAY shot by spec (the public-floors look is the
        // bright snow-bounce day grade) — force DAY regardless of the env default.
        if (showroomCivShot) gShowroomDay = true;

        // ---- Night-sky + lighting recipe (mirrors the --screenshot-showroom block) ----
        // The night `sp` is kept in scope as the BASE the ragdoll PROOF brightens (a
        // night-only debug shot). The live look is (re)applied by applyShowroomTimeOfDay
        // below for the chosen state — once for the headless proofs, and again on every
        // 'T' toggle in the interactive loop.
        x3::rhi::IRenderDevice::SkyParams sp{};
        sp.enabled = true;
        sp.sunDir[0] = 0.6f; sp.sunDir[1] = 0.42f; sp.sunDir[2] = -0.2f;   // low raking MOON
        sp.sunColor[0] = 1.0f; sp.sunColor[1] = 0.96f; sp.sunColor[2] = 0.90f;
        sp.sunIntensity = 0.25f;   // cool moonlight (still casts shadows)
        sp.haze = 0.15f; sp.exposure = 0.62f;
        sp.zenith[0]  = 0.012f; sp.zenith[1]  = 0.012f; sp.zenith[2]  = 0.028f;   // near-black zenith
        sp.horizon[0] = 0.10f;  sp.horizon[1] = 0.13f;  sp.horizon[2] = 0.20f;    // faint cool horizon
        // (sky/ambient/bloom are applied via applyShowroomTimeOfDay AFTER the interior
        // point lights are built — see the INTERIOR LIGHTING block.)
        // Disable the SSAO/GI depth PRE-PASS (the showroom uses alpha-cutout foliage that
        // the pre-pass can't discard — would punch sky holes; see the screenshot block).
        { x3::rhi::IRenderDevice::SsaoParams s{}; s.enabled = false; device->setSsaoParams(s); }
        { x3::rhi::IRenderDevice::GiParams   g{}; g.enabled = false; device->setGiParams(g); }

        // Night-sky planets (shared helper; same files/order as --screenshot-nightsky).
        int nPlanetTexFail = 0;
        x3::rhi::MeshHandle planetMesh{};
        x3::rhi::MeshHandle ringMesh{};
        std::vector<NightSkyPlanet> planets =
            loadNightSkyPlanets(device.get(), planetMesh, nPlanetTexFail, "--world showroom", &ringMesh);
        if (nPlanetTexFail > 0)
            x3::logWarn("--world showroom: " + std::to_string(nPlanetTexFail) + " planet texture(s) missing");

        // ---- Building footprint (engine-space) — same kBuild subset as the screenshot block.
        const std::vector<std::string> kBuild = {
            "room", "pilar", "plateform", "platform", "stair", "window", "showcase",
            "table", "chair", "carpet", "tube", "halogen", "cache", "tv_screen" };
        float bmn[3], bmx[3];
        const uint32_t nb = showroom.namedBounds(kBuild, bmn, bmx);
        if (nb == 0) { showroom.worldBounds(bmn, bmx); x3::logWarn("--world showroom: 0 named building nodes; using full scene bounds"); }
        x3::logInfo("--world showroom: building bounds (" + std::to_string(nb) + " nodes) min(" +
            std::to_string(bmn[0]) + "," + std::to_string(bmn[1]) + "," + std::to_string(bmn[2]) +
            ") max(" + std::to_string(bmx[0]) + "," + std::to_string(bmx[1]) + "," + std::to_string(bmx[2]) + ")");

        const float cx = (bmn[0] + bmx[0]) * 0.5f, cz = (bmn[2] + bmx[2]) * 0.5f;
        const float halfX = std::max(8.0f, (bmx[0] - bmn[0]) * 0.5f + 4.0f);   // footprint XZ half-extents (+ margin)
        const float halfZ = std::max(8.0f, (bmx[2] - bmn[2]) * 0.5f + 4.0f);

        // ---- SYNTHESIZE the GROUND floor. EnvArtSystem makes NO collision bodies + its
        // bounds are ORIGIN-only (not the true floor plane), so floorY is a TUNE POINT:
        // start at the building-bounds min Y. One large static slab sized to the footprint
        // XZ, its TOP surface at floorY (so player feet at floorY+eps stand on it). 1 m
        // thick (half-extent 0.5), centered 0.5 below floorY.  *** TUNE floorY HERE ***
        // SOLID now (no holes): the OWNER'S entrance is a HIDDEN WALL DOOR on the 2nd
        // floor — not a ground floor-hatch — and the glass elevator now boards in the
        // upper ATRIUM, so the ground level no longer needs a hatch drop or a shaft pit.
        float floorY = bmn[1];   // <-- empirical start; adjust if the player floats/sinks.
        {
            sphys->addBox({ halfX, 0.5f, halfZ }, { cx, floorY - 0.5f, cz }, 0.0f, x3::phys::Layer::Static);
            x3::logInfo("--world showroom: GROUND floor slab (solid) top floorY=" + std::to_string(floorY) +
                        " center(" + std::to_string(cx) + "," + std::to_string(cz) + ") (TUNE POINT)");
        }
        // Perimeter walls (4 static slabs, 4 m tall) so the player can't walk off the slab.
        {
            const float wallH = 2.0f;   // half-height (4 m wall)
            const float wallT = 0.3f;   // half-thickness
            const x3::phys::Vec3 wc{ cx, floorY + wallH, cz };
            sphys->addBox({ wallT, wallH, halfZ }, { cx - halfX, wc.y, cz }, 0.0f, x3::phys::Layer::Static); // -X
            sphys->addBox({ wallT, wallH, halfZ }, { cx + halfX, wc.y, cz }, 0.0f, x3::phys::Layer::Static); // +X
            sphys->addBox({ halfX, wallH, wallT }, { cx, wc.y, cz - halfZ }, 0.0f, x3::phys::Layer::Static); // -Z
            sphys->addBox({ halfX, wallH, wallT }, { cx, wc.y, cz + halfZ }, 0.0f, x3::phys::Layer::Static); // +Z
        }

        // ---- Player spawn at the building center, feet just above the floor slab.
        const float sx = cx, sy = floorY + 0.05f, sz = cz;
        x3::game::Player splayer;
        splayer.spawn(*sphys, sx, sy, sz);
        x3::logInfo("--world showroom: player spawn feet(" + std::to_string(sx) + "," +
                    std::to_string(sy) + "," + std::to_string(sz) + ")");

        // ---- Companion ARIA: a single RescueVictim a few metres in front (+Z) of spawn,
        // standing on the floor. NEVER activated (hubReached stays false -> no countdown,
        // no boss). AnnaCasual_anim.glb carries Idle/Walk/Run/Talk (retargeted from Jake),
        // so the loco blend engages — she walks/runs while following, not idle-slides.
        const float gx = cx + 3.0f, gz = cz + 4.0f;
        x3::game::RescueVictim girl;
        girl.build(sscene, *device, *sphys, x3::game::riggedGlbRoot(),
                   x3::phys::Vec3{ gx, floorY, gz }, x3::game::VictimId::Aria, "Aria",
                   "AnnaCasual_anim.glb", 1e9f /*huge timer — never expires*/,
                   x3::game::MonsterSystem::Tuning{});
        x3::logInfo("--world showroom: Aria at (" + std::to_string(gx) + "," +
                    std::to_string(floorY) + "," + std::to_string(gz) + ")");

        // ===================================================================
        // ADDITIVE: 2ND-FLOOR hidden wall door -> passage -> stairs -> elevator
        // atrium -> glass elevator -> glass spire-top deck. (OWNER'S VISION.)
        // All geometry below is ADDITIVE to the showroom (does NOT touch the
        // building GLB / Aria / night-sky code). It FOLLOWS THE BUILDING'S OWN
        // ARCHITECTURE — clad in the building's WHITE PANEL material, aligned to
        // its axes / walls / floor levels (derived from the GLB node bounds):
        //   GROUND floor   y = floorY (-9)   : where the player spawns.
        //   2nd FLOOR      y = floor2Y (3)   : top of the GLB Room_01 slab; the
        //                  player CLIMBS here on a synthesized stair approximating
        //                  the GLB "Stair" nodes (left run x~[44,54]).
        //   ATRIUM floor   y = atriumFloorY (9): one flight ABOVE the 2nd floor,
        //                  where the glass elevator now BOARDS.
        //   DECK           y = deckTopY (90) : the glass spire-top deck (unchanged).
        // Feature chain (all WALKABLE, all white-clad, all axis-aligned):
        //   STAGE 1  CLIMB collision: a stair (approximating the GLB Stair run) +
        //            a 2nd-floor slab so the player walks up from ground to y=3.
        //   STAGE 2  a FLUSH HIDDEN WALL DOOR set into a real 2nd-floor wall
        //            (Pilar_02, z~-101), keypad-gated (code 2742 unchanged); the
        //            panel SLIDES ASIDE when unlocked.
        //   STAGE 3  behind the door: an ENTRY PASSAGE (-Z) -> a 90 deg TURN ->
        //            a FLIGHT OF STAIRS UP -> the ELEVATOR ATRIUM (a white room
        //            around the lift). The elevator's LOWER stop is the atrium.
        //   (PHASE 1/2 below keep the GLASS DECK + the glass elevator that rides
        //    atrium<->deck.) Glass is the engine BLEND path (drawMeshPBR
        //    alphaBlend=true), drawn explicitly each frame; the white-panel
        //    opaque geometry goes through the Scene as textured entities.
        // ===================================================================
        const float spireX = cx;          // spire is over the building center X
        const float spireZ = -100.0f;     // spire Z (per the night-showroom blueprint)
        const float deckTopY = 90.0f;     // walkable deck surface height (TUNE vs spire top y~88.5)
        const float shaftX   = spireX + 9.0f;  // elevator shaft just +X of the spire (clear of geo)
        const float shaftZ   = spireZ;
        // ---- Building floor levels (from the GLB node bounds; see tools/glb_node_bounds.py).
        // Room_01 (the 2nd-floor slab) has its TOP at world y=3; the GLB "Stair" nodes climb
        // from the ground (y=-9). The atrium sits one short flight above the 2nd floor.
        const float floor2Y     = 3.0f;   // 2nd-floor walkable surface (Room_01 top)
        // ELEVATOR LEVEL (Y10, in the owner's Y10-14 range): the hidden stair climbs UP
        // INSIDE the back strut to THIS height, where it meets the glass-elevator boarding
        // atrium. The lift's LOWER stop is computed from atriumFloorY below, so setting it
        // here moves the boarding level to the strut-stair landing (OWNER'S vision). Y10 is
        // chosen so the strut's diagonal stepped stair climbs at a walkable ~43 deg (the
        // character controller's max walkable slope is 50 deg; steps are <=0.4 m each).
        const float atriumFloorY = 10.0f; // elevator-atrium floor == strut-stair landing

        // ---- Shared procedural meshes (authored at WORLD center; identity xform).
        // Glass tints reused for deck slab, rails, car, and shaft glints. The
        // alphaBlend draw multiplies baseColorFactor (incl. alpha) onto the texel.
        auto makeWorldMesh = [&](const x3::prims::PrimMesh& g) {
            return device->createMesh(g.verts.data(), (uint32_t)g.verts.size(),
                                      g.index.data(), (uint32_t)g.index.size());
        };
        // A helper to draw one glass box (translucent) at an identity-placed world
        // mesh, OR offset by a model translation for the moving car.
        const float kIdentity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        auto drawGlass = [&](const x3::rhi::FrameContext& fr, x3::rhi::MeshHandle m,
                             const float model[16], float r, float g, float b, float a,
                             float emisStrength) {
            const float bcf[4]  = { r, g, b, a };
            const float emis[4] = { r * 0.6f, g * 0.7f, b * 0.9f, emisStrength };
            device->drawMeshPBR(fr, m, x3::rhi::TextureHandle{}, x3::rhi::TextureHandle{},
                                x3::rhi::TextureHandle{}, bcf, emis, model,
                                /*alphaMask*/false, /*alphaBlend*/true, x3::rhi::TextureHandle{});
        };

        // ===================================================================
        // WHITE-PANEL CLADDING — match the building's wall material.
        // The GLB walls (Room_01/Pilar_01/Pilar_02) all use a "Wall_Atlas..._White"
        // textured material (baseColorFactor white). So every additive surface of
        // the entrance run (climb stair, 2nd-floor slab, hidden door panel, passage,
        // turn, upper stair, atrium) is clad in a procedural WHITE sci-fi PANEL tile
        // tinted near-white, so the whole run reads as part of the structure (NOT
        // grey boxes). One shared tiling texture + a tiny "add a white-clad static
        // box (render entity + collision)" helper keep it uniform + axis-aligned.
        // -------------------------------------------------------------------
        x3::rhi::TextureHandle whitePanelTex{};
        {
            // CLEAN, SMOOTH near-white powder-coat panel — matches the sleek Unity
            // ShowRoom interior (smooth light-grey/white walls + floors). The earlier
            // pass used makeSciFiPanelRGBA tinted ~x2.7, which lifted the panel FACES to
            // white but left the seam grooves / bolts / bevels as a HIGH-CONTRAST grid
            // (heavy dark grout) that clashed with the smooth GLB. makeCleanPanelRGBA is
            // a flat light face with only a WHISPER-FINE 1-px low-contrast seam hairline
            // (no grout, no bolts, no bevels), so the additive cladding reads flush with
            // the imported white walls. 4 panel divisions across the 512 tile.
            std::vector<uint8_t> px = x3::prims::makeCleanPanelRGBA(
                512, /*panels*/4, x3::prims::detail::kNoTint, /*seams*/true);
            whitePanelTex = device->createTexture(px.data(), 512, 512, /*srgb*/true);
        }
        // The white-panel base tint (multiplies the already-white texel). Near-1 so the
        // clean white panels read under the dim moonlight, like the GLB walls.
        const float kWhite[4] = { 0.98f, 0.98f, 1.0f, 1.0f };
        // Add ONE axis-aligned white-clad box: a render Scene entity (white panel
        // texture) PLUS matching Static collision. Returns the Scene entity id.
        // (cx,cy,cz) = center; (hx,hy,hz) = half-extents. collide=false => visual only.
        auto addWhiteBox = [&](float bx, float by, float bz, float hx, float hy, float hz,
                               bool collide = true) -> uint32_t {
            x3::rhi::MeshHandle m = makeWorldMesh(x3::prims::makeBox(hx, hy, hz, bx, by, bz, 0.25f));
            x3::game::Entity e; e.mesh = m; e.tex = whitePanelTex;
            e.baseColor[0]=kWhite[0]; e.baseColor[1]=kWhite[1]; e.baseColor[2]=kWhite[2]; e.baseColor[3]=1.0f;
            const uint32_t id = sscene.add(e);
            if (collide) sphys->addBox({ hx, hy, hz }, { bx, by, bz }, 0.0f, x3::phys::Layer::Static);
            return id;
        };
        // POLISHED-FLOOR material dial (SSR/RT reflections money shot): a 1x1
        // metallic-roughness map (glTF packing: G=roughness, B=metallic, linear)
        // applied to the showroom's WALKED floor slabs so they read as the sleek
        // polished showroom surface they were always meant to be — roughness 0.08
        // (mirror-sharp, inside the SSR full-strength band) + metallic 0.5 (semi-
        // metal powder coat: strong F0 without fully killing the white diffuse).
        // Only entities explicitly polish()-ed change; every other white box keeps
        // the satin dielectric default.
        x3::rhi::TextureHandle polishedMrTex{};
        {
            const uint8_t mr[4] = { 0, 20, 128, 255 };   // R unused, G=rough 0.08, B=metal 0.50
            polishedMrTex = device->createTexture(mr, 1, 1, /*srgb*/false);
        }
        auto polish = [&](uint32_t id) { sscene.get(id).mrTex = polishedMrTex; };

        // ===================================================================
        // STAGE 1 — let the player CLIMB to the 2ND FLOOR (y = floor2Y = 3).
        // The GLB has no collision, so we SYNTHESIZE it, aligned to the building:
        //   * the 2nd-floor SLAB = the GLB Room_01 footprint (x~[29,115], z~[-123,
        //     -99]) with its TOP at floor2Y. Built as two halves around the central
        //     tower-core gap (x~[71,73]) so it matches the real split floor.
        //   * a CLIMB STAIR approximating the GLB "Stair" node (left run, x~[44,54],
        //     running along +Z), EXTENDED so it rises the full ground->2nd-floor
        //     drop (floorY -> floor2Y = 12 m) at a walkable pitch. makeRamp builds
        //     the walkable wedge; we cap it with a white tread plate so it reads as
        //     a clad stair, and a small landing where it meets the 2nd floor.
        // -------------------------------------------------------------------
        // 2nd-floor slab — Room_01 footprint, split around the central tower gap.
        const float r2z0 = -122.8f, r2z1 = -99.2f;            // Room_01 Z span
        const float r2cz = (r2z0 + r2z1) * 0.5f, r2hz = (r2z1 - r2z0) * 0.5f;
        const float slabHY = 0.4f;                            // 0.8 m thick slab; TOP at floor2Y
        const float slabCY = floor2Y - slabHY;
        // Left half x~[29.3,71.2], right half x~[73,114.9]; leave the x~[71.2,73] gap.
        // Both halves POLISHED (SSR money shot: the 2nd-floor walk reflects the
        // building lights + window wall in the floor).
        polish(addWhiteBox((29.3f + 71.2f) * 0.5f, slabCY, r2cz, (71.2f - 29.3f) * 0.5f, slabHY, r2hz));
        polish(addWhiteBox((73.0f + 114.9f) * 0.5f, slabCY, r2cz, (114.9f - 73.0f) * 0.5f, slabHY, r2hz));
        // CLIMB stair: approximate the GLB left "Stair" (x~[43.8,53.7]) but lengthen
        // the run so the 12 m rise is a walkable ~40 deg. Runs along +Z from a low
        // edge on the ground (z=stairLowZ @ floorY) up to a high edge that meets the
        // 2nd-floor slab (z=stairHighZ @ floor2Y).
        const float climbCX = 48.75f;          // GLB left-stair X center
        const float climbHalfW = 4.8f;         // matches the GLB stair width (~9.9 m)
        const float climbRise = floor2Y - floorY;   // 12 m
        const float climbRun  = 15.0f;         // walkable pitch (~39 deg)
        const float stairLowZ  = -119.0f;      // low edge on the ground (inside Room_01 -Z half)
        {
            x3::prims::PrimMesh ramp = x3::prims::makeRamp(climbCX, floorY, stairLowZ,
                                                          climbHalfW, climbRun, climbRise,
                                                          /*axis*/1 /*+Z*/, /*dir*/+1.0f, 0.25f);
            x3::rhi::MeshHandle m = makeWorldMesh(ramp);
            x3::game::Entity e; e.mesh = m; e.tex = whitePanelTex;
            e.baseColor[0]=kWhite[0]; e.baseColor[1]=kWhite[1]; e.baseColor[2]=kWhite[2]; e.baseColor[3]=1.0f;
            sscene.add(e);
            sphys->addStaticMesh(ramp.cverts.data(), (uint32_t)ramp.cverts.size() / 3,
                                 ramp.cindex.data(), (uint32_t)ramp.cindex.size());
            x3::logInfo("--world showroom: STAGE1 climb stair x=" + std::to_string(climbCX) +
                        " run +Z z=" + std::to_string(stairLowZ) + ".." +
                        std::to_string(stairLowZ + climbRun) + " rise " + std::to_string(climbRise) +
                        " m floorY->floor2Y; 2nd-floor slab top y=" + std::to_string(floor2Y));
        }

        // ===================================================================
        // STRUT SET — the building's SYMMETRIC RADIAL "/" blade-fin legs, REBUILT
        // thicker (the GLB struts are fixed thin geometry, so we clad NEW white-panel
        // strut-SHELLS over them). FOUR canted struts at the four footprint corners
        // (back-left/back-right/front-left/front-right), each LEANING INWARD+UP toward
        // the central spire core — a matched radial set (same thickness/width/height,
        // mirrored about the center). ONE strut (the BACK-LEFT, Z~-134, least visible
        // from the central pad per the interior reference) is HOLLOW and carries the
        // HIDDEN STAIR: a keypad door (code 2742) set into its canted outward "/" face
        // at the civilian floor (floorY) climbs UP INSIDE it to the ELEVATOR LEVEL
        // (atriumFloorY=14), where a short bridge meets the glass-elevator atrium.
        //   - Strut layout sampled from the GLB Plateform_05/06 corner fins
        //     (tools/glb_node_bounds.py): the 4 canted disc-edge supports.
        //   - Built via prims::makeCantedStrut (a sheared prism) so they read as the
        //     real leaning legs, clad in the SAME white-panel material as the GLB walls.
        // -------------------------------------------------------------------
        // Add a white-clad procedural mesh (render entity + static collision). Used for
        // the canted struts / stair steps that aren't axis-aligned boxes.
        auto addWhiteMesh = [&](const x3::prims::PrimMesh& g, bool collide = true) -> uint32_t {
            x3::rhi::MeshHandle m = makeWorldMesh(g);
            x3::game::Entity e; e.mesh = m; e.tex = whitePanelTex;
            e.baseColor[0]=kWhite[0]; e.baseColor[1]=kWhite[1]; e.baseColor[2]=kWhite[2]; e.baseColor[3]=1.0f;
            const uint32_t id = sscene.add(e);
            if (collide && !g.cverts.empty())
                sphys->addStaticMesh(g.cverts.data(), (uint32_t)g.cverts.size()/3,
                                     g.cindex.data(), (uint32_t)g.cindex.size());
            return id;
        };

        // --- Matched strut dimensions (ALL four identical -> symmetric). ---
        const float strutBaseY  = floorY;        // strut foot on the civilian floor (-12)
        const float strutTopY   = atriumFloorY;  // strut head at the elevator level (10)
        const float strutHalfW  = 4.0f;          // 8 m wide (tangential) — beefy leg
        const float strutHalfT  = 2.6f;          // 5.2 m thick (radial) — THICKENED
        const float strutHeadR  = 10.0f;         // head pulled IN to ~10 m from the core
                                                 // (distinct legs, just inside the disc spring line;
                                                 //  gives the hollow strut a ~44 deg walkable stair)
        // Base/top XZ centers per corner, derived radially from the center so the four
        // are perfectly mirrored. Base sits OUT at the corner; head pulled IN near the
        // spire core (the "/" inward lean -> the four legs gather toward the spire). The
        // head-near-core lean also gives a long diagonal run (~23 m) so the internal
        // stair climbs the floorY->elevator-level drop at a walkable ~43 deg.
        struct StrutDef { float bx,bz,tx,tz,rox,roz; };
        auto strutFor = [&](float cornerX, float cornerZ) -> StrutDef {
            float dx = cornerX - cx, dz = cornerZ - cz;
            float L = std::sqrt(dx*dx + dz*dz); if (L < 1e-3f) L = 1.0f;
            float ux = dx/L, uz = dz/L;             // radial-OUT unit
            StrutDef s;
            s.bx = cornerX + ux*2.0f;     s.bz = cornerZ + uz*2.0f;     // foot a touch further out
            s.tx = cx + ux*strutHeadR;    s.tz = cz + uz*strutHeadR;    // head near the core
            s.rox = ux; s.roz = uz;
            return s;
        };
        // GLB Plateform_05/06 corner-fin centers (the canted disc-edge supports).
        const StrutDef sBL = strutFor((47.2f+70.0f)*0.5f, (-139.6f-129.7f)*0.5f); // back-left  (Z~-134) HOLLOW
        const StrutDef sBR = strutFor((74.3f+97.0f)*0.5f, (-139.7f-129.8f)*0.5f); // back-right
        const StrutDef sFL = strutFor((47.2f+70.0f)*0.5f, (-92.2f-82.3f)*0.5f);   // front-left
        const StrutDef sFR = strutFor((74.3f+97.1f)*0.5f, (-92.3f-82.4f)*0.5f);   // front-right
        // Build the THREE SOLID struts (BR/FL/FR). The hollow BL one is built below
        // (its walls + door + interior stair).
        auto buildSolidStrut = [&](const StrutDef& s) {
            addWhiteMesh(x3::prims::makeCantedStrut(s.bx, strutBaseY, s.bz, s.tx, strutTopY, s.tz,
                                                    strutHalfW, strutHalfT, s.rox, s.roz,
                                                    /*uvScale*/0.4f, /*hollow*/false));
        };
        buildSolidStrut(sBR); buildSolidStrut(sFL); buildSolidStrut(sFR);
        x3::logInfo("--world showroom: STRUTS x4 canted blades (halfW=" + std::to_string(strutHalfW) +
                    " halfT=" + std::to_string(strutHalfT) + " baseY=" + std::to_string(strutBaseY) +
                    " topY=" + std::to_string(strutTopY) + ") symmetric about (" +
                    std::to_string(cx) + "," + std::to_string(cz) + "); BACK-LEFT is HOLLOW (stair)");

        // ---- The HOLLOW BACK-LEFT strut: four canted WALLS (outward face has the door
        // gap), the internal STAIR climbing the cant, the keypad door, and the top
        // landing + bridge to the elevator atrium. The blade is clad white like the rest.
        // Geometry follows the SAME canted axis (base sBL.b* -> top sBL.t*).
        const float hsRox = sBL.rox, hsRoz = sBL.roz;       // radial-out unit (toward corner)
        const float hsTx  = -hsRoz,  hsTz = hsRox;          // tangential unit
        // Door sits in the OUTWARD canted face at the civilian floor. Door-face center at
        // the base, pushed out to the outward wall plane.
        const float doorFaceX = sBL.bx + hsRox*strutHalfT;
        const float doorFaceZ = sBL.bz + hsRoz*strutHalfT;
        // Keep the proven keypad-host variable NAMES (door*/hatch*) so the interaction +
        // smoke code is untouched; they now address the STRUT-FACE door at civilian level.
        const float doorHalfW   = 1.2f;          // 2.4 m wide opening
        const float doorHalfH   = 1.3f;          // 2.6 m tall
        const float doorPanelHZ = 0.20f;         // panel thickness
        const float doorFloorY  = floorY;        // civilian floor the player stands on
        const float doorX = doorFaceX;           // door center X (proximity + panel)
        const float doorZ = doorFaceZ;           // door center Z
        const float doorCY = doorFloorY + doorHalfH;   // panel center (sill on the floor)
        const float hatchX = doorX, hatchZ = doorZ, hatchHalf = doorHalfW;   // proximity window
        constexpr int HATCH_CODE = kShowroomHatchCode;   // 2742 (UNCHANGED)
        // The hollow strut SHELL (outward wall omitted so the doorway/interior is open).
        // Render + collision: the 3 enclosed faces (inward + both tangential) + caps give
        // the blade its solid read and keep the stair enclosed; the outward face is clad
        // separately below (around the door gap) so a real door-sized hole exists.
        addWhiteMesh(x3::prims::makeCantedStrut(sBL.bx, strutBaseY, sBL.bz, sBL.tx, strutTopY, sBL.tz,
                                                strutHalfW, strutHalfT, hsRox, hsRoz,
                                                /*uvScale*/0.4f, /*hollow*/true));
        // OUTWARD-FACE CLADDING: thin CANTED slabs (matching the blade's lean) covering
        // the full outward face EXCEPT a door-sized gap at the base — so from outside the
        // strut reads as a clean clad "/" blade with a flush door set into it. Built as
        // canted slabs (thin in the radial axis) at the outward face plane:
        //   * the UPPER cladding (above the door lintel, up the whole face),
        //   * a LEFT and RIGHT jamb cladding flanking the door at the base.
        {
            const float cladHT = 0.18f;   // cladding slab thickness (radial)
            // The outward face plane sits at +halfT along radial from the strut axis. The
            // canted slab's own axis runs base->top; we offset both base/top OUT by halfT.
            const float ofbx = sBL.bx + hsRox*strutHalfT, ofbz = sBL.bz + hsRoz*strutHalfT;
            const float oftx = sBL.tx + hsRox*strutHalfT, oftz = sBL.tz + hsRoz*strutHalfT;
            const float doorTopY = doorFloorY + doorHalfH*2.0f;   // top of the 2.6 m door
            // UPPER cladding: from the door top up to the head, full width. Its base sits
            // at the height where the door ends (interpolate the face axis to that height).
            {
                const float tDoorTop = (doorTopY - strutBaseY) / (strutTopY - strutBaseY);
                const float ubx = ofbx + (oftx - ofbx)*tDoorTop, ubz = ofbz + (oftz - ofbz)*tDoorTop;
                addWhiteMesh(x3::prims::makeCantedStrut(ubx, doorTopY, ubz, oftx, strutTopY, oftz,
                                                        strutHalfW, cladHT, hsRox, hsRoz, 0.4f, false));
            }
            // LEFT + RIGHT jamb cladding at the base (door height), flanking the 2.4 m gap.
            const float jambHW = (strutHalfW - doorHalfW) * 0.5f;     // half-width of each jamb panel
            const float jambOff = doorHalfW + jambHW;                 // tangential offset to jamb center
            for (float sgn : { +1.0f, -1.0f }) {
                const float jbx = ofbx + hsTx*jambOff*sgn, jbz = ofbz + hsTz*jambOff*sgn;
                // jamb top tracks the cant up to the door top height.
                const float tDoorTop = (doorTopY - strutBaseY) / (strutTopY - strutBaseY);
                const float jtx = jbx + (oftx - ofbx)*tDoorTop, jtz = jbz + (oftz - ofbz)*tDoorTop;
                addWhiteMesh(x3::prims::makeCantedStrut(jbx, strutBaseY, jbz, jtx, doorTopY, jtz,
                                                        jambHW, cladHT, hsRox, hsRoz, 0.4f, false));
            }
        }
        // Concealed door PANEL: a thin CANTED white slab flush in the outward face,
        // matching the blade's lean + the same white texture so it's invisible until
        // opened. Authored at its WORLD position (door tangential center, sill on the
        // floor) so its entity transform starts at identity; the slide animation then
        // translates it tangentially. Collision is a matching axis-aligned box (removed
        // on open). A faint cyan glow pulses when the player is near + it's still closed.
        const float doorTopY2 = doorFloorY + doorHalfH*2.0f;
        x3::rhi::MeshHandle hatchMesh = makeWorldMesh(x3::prims::makeCantedStrut(
            doorFaceX, doorFloorY, doorFaceZ,
            doorFaceX + (sBL.tx - sBL.bx) * ((doorTopY2-strutBaseY)/(strutTopY-strutBaseY)),
            doorTopY2,
            doorFaceZ + (sBL.tz - sBL.bz) * ((doorTopY2-strutBaseY)/(strutTopY-strutBaseY)),
            doorHalfW, doorPanelHZ, hsRox, hsRoz, 0.4f, false));
        x3::game::Entity hatchEnt; hatchEnt.mesh = hatchMesh; hatchEnt.tex = whitePanelTex;
        hatchEnt.baseColor[0]=kWhite[0]; hatchEnt.baseColor[1]=kWhite[1]; hatchEnt.baseColor[2]=kWhite[2]; hatchEnt.baseColor[3]=1.0f;
        hatchEnt.emissive[0]=0.10f; hatchEnt.emissive[1]=0.45f; hatchEnt.emissive[2]=0.55f; hatchEnt.emissive[3]=0.0f;
        const uint32_t hatchIdx = sscene.add(hatchEnt);   // authored in world -> identity transform
        x3::phys::BodyId hatchLidBody =
            sphys->addBox({ doorHalfW, doorHalfH, doorPanelHZ }, { doorX, doorCY, doorZ }, 0.0f, x3::phys::Layer::Static);
        bool  hatchOpen = false;
        float hatchSlide = 0.0f;
        // The slide-aside direction (tangential, +) for the cosmetic open animation.
        const float hatchSlideX = hsTx, hatchSlideZ = hsTz;

        // ---- INTERNAL STAIR up the hollow strut: many small STEPPED white treads
        // following the canted axis from the door foot (floorY) up to the head landing
        // (atriumFloorY). Each step rises <=0.4 m (the character controller steps up to
        // 0.4 m) so the player WALKS UP every step; collision is on every tread. Each
        // tread is an ORIENTED block (built via makeCantedStrut so its cross-section is
        // aligned to the strut's tangential + radial axes — NOT axis-aligned — so the
        // steps fill the canted blade interior and march along the diagonal cant.
        {
            const float stepRise = 0.4f;                                  // <=0.4 m -> walkable
            const int   nSteps   = (int)std::ceil((strutTopY - strutBaseY) / stepRise);  // ~48
            const float treadHW  = strutHalfW - 0.9f;                     // tread half-width (tangential, inside walls)
            const float treadHT  = strutHalfT - 0.5f;                     // tread half-depth (radial, inside walls)
            for (int i = 0; i < nSteps; ++i) {
                float t0  = (float)(i + 1) / (float)nSteps;               // top of step i
                float cxs = sBL.bx + (sBL.tx - sBL.bx) * t0;
                float czs = sBL.bz + (sBL.tz - sBL.bz) * t0;
                float topY = strutBaseY + (strutTopY - strutBaseY) * t0;  // this tread's TOP
                // ORIENTED riser block: a short vertical prism (cross-section tangential x
                // radial, aligned to the strut) from just below the tread up to its top.
                // (Same XZ for base+top -> a vertical block; height = 0.55 m riser.)
                addWhiteMesh(x3::prims::makeCantedStrut(cxs, topY - 0.55f, czs, cxs, topY, czs,
                                                        treadHW, treadHT, hsRox, hsRoz, 0.4f, false));
            }
            // Top LANDING: a white floor pad at the strut head (atriumFloorY) where the
            // stair tops out and the bridge to the elevator atrium begins.
            addWhiteBox(sBL.tx, strutTopY - 0.25f, sBL.tz, strutHalfW, 0.25f, strutHalfT);
            x3::logInfo("--world showroom: HOLLOW strut stair " + std::to_string(nSteps) +
                        " steps (rise " + std::to_string(stepRise) + " m each) base(" +
                        std::to_string(sBL.bx) + "," + std::to_string(sBL.bz) + ") -> head(" +
                        std::to_string(sBL.tx) + "," + std::to_string(sBL.tz) +
                        ") rise floorY->atriumFloorY=" + std::to_string(strutTopY) +
                        "; door face(" + std::to_string(doorFaceX) + "," + std::to_string(doorFaceZ) +
                        ") at civilian floor y=" + std::to_string(doorFloorY));
        }

        // -------------------------------------------------------------------
        // PHASE 1 — GLASS DECK at the spire top.
        // 14x14 m glass slab (thin), TOP surface at deckTopY, centered over the
        // spire. Four low glass rail boxes around the edge (1.1 m tall). Static
        // collision: the slab floor + the four rails (so you stand + can't fall).
        // -------------------------------------------------------------------
        const float deckHalf = 7.0f;       // 14 m square
        const float deckSlabHalfY = 0.15f; // thin glass slab
        const float deckSlabCY = deckTopY - deckSlabHalfY;   // center so TOP == deckTopY
        x3::rhi::MeshHandle deckMesh = makeWorldMesh(
            x3::prims::makeBox(deckHalf, deckSlabHalfY, deckHalf, spireX, deckSlabCY, spireZ, 0.5f));
        // Rails: 4 thin tall glass boxes hugging each edge (top ~1.1 m above deck).
        const float railH = 0.55f, railT = 0.08f;
        const float railCY = deckTopY + railH;
        x3::rhi::MeshHandle railNZ = makeWorldMesh(x3::prims::makeBox(deckHalf, railH, railT, spireX, railCY, spireZ - deckHalf, 1.0f));
        x3::rhi::MeshHandle railPZ = makeWorldMesh(x3::prims::makeBox(deckHalf, railH, railT, spireX, railCY, spireZ + deckHalf, 1.0f));
        x3::rhi::MeshHandle railNX = makeWorldMesh(x3::prims::makeBox(railT, railH, deckHalf, spireX - deckHalf, railCY, spireZ, 1.0f));
        x3::rhi::MeshHandle railPX = makeWorldMesh(x3::prims::makeBox(railT, railH, deckHalf, spireX + deckHalf, railCY, spireZ, 1.0f));
        // Static collision: deck floor slab + 4 rail slabs.
        sphys->addBox({ deckHalf, deckSlabHalfY, deckHalf }, { spireX, deckSlabCY, spireZ }, 0.0f, x3::phys::Layer::Static);
        sphys->addBox({ deckHalf, railH, railT }, { spireX, railCY, spireZ - deckHalf }, 0.0f, x3::phys::Layer::Static);
        sphys->addBox({ deckHalf, railH, railT }, { spireX, railCY, spireZ + deckHalf }, 0.0f, x3::phys::Layer::Static);
        sphys->addBox({ railT, railH, deckHalf }, { spireX - deckHalf, railCY, spireZ }, 0.0f, x3::phys::Layer::Static);
        sphys->addBox({ railT, railH, deckHalf }, { spireX + deckHalf, railCY, spireZ }, 0.0f, x3::phys::Layer::Static);
        x3::logInfo("--world showroom: PHASE1 glass deck 14x14 top y=" + std::to_string(deckTopY) +
                    " center(" + std::to_string(spireX) + "," + std::to_string(spireZ) + ") + 4 rails");

        // -------------------------------------------------------------------
        // PHASE 2 — GLASS ELEVATOR atrium<->deck (reuses app/elevator.cpp).
        // ElevatorSystem provides the moving Static-layer body that CARRIES the
        // player: update() returns the per-frame vertical delta (the host adds it
        // to a rider's Y) and playerRiding(feet) detects a rider standing on the
        // cab TOP. So the cab is a thin FLOOR PLATFORM the player rides INSIDE a
        // glass box that rises above it. The cab-top is the standable surface:
        //   LOWER stop -> cab top at the ATRIUM floor (atriumFloorY) so you step
        //                 from the atrium into the cab (OWNER'S vision — the lift no
        //                 longer reaches the ground; you climb to it via the door);
        //   UPPER stop -> cab top at the DECK (deckTopY) so you step out onto it.
        // The glass walls (a 2.5 x 3 x 2.5 m box) are a translucent VISUAL drawn
        // around/above the platform each frame (not collision — open so you walk in).
        // -------------------------------------------------------------------
        const float carHX = 1.25f, platHY = 0.12f, carHZ = 1.25f;   // thin platform
        const float carBoxHY = 1.5f;   // glass box half-height (3 m tall walls)
        // Stop centers so the PLATFORM TOP (center + platHY) lands at atriumFloorY / deckTopY.
        const float elevBaseCenterY = atriumFloorY - platHY;   // cab top == atrium floor
        const float elevTopCenterY  = deckTopY      - platHY;   // cab top == deckTopY
        x3::game::ElevatorSystem elev;
        const uint32_t elevEntIdx = sscene.size();   // the cab platform entity lands here
        elev.build(sscene, *device, *sphys, shaftX, shaftZ, carHX, platHY, carHZ,
                   { elevBaseCenterY, elevTopCenterY }, /*startStop*/0);
        elev.setSpeed(14.0f);   // m/s — a brisk readable climb (~7 s over the 99 m shaft)
        // GLASS-BOTTOM cab (OWNER'S vision): hide the opaque plate Entity and draw a
        // translucent glass floor slab at the cab top each frame (in drawAdditiveGlass).
        // Collision is the elevator's moving Static body, so the rider still stands + rides
        // — they just see DOWN through the floor at the gallery falling away as it climbs.
        if (elevEntIdx < sscene.size()) {
            sscene.get(elevEntIdx).visible = false;
        }
        // Glass BOX walls authored centered at ORIGIN; drawn at (cabTop + carBoxHY)
        // each frame so the walls rise from the platform. Hollow-look: a single
        // translucent box reads as a glass cab around the rider.
        x3::rhi::MeshHandle carMesh = makeWorldMesh(
            x3::prims::makeBox(carHX, carBoxHY, carHZ, 0, 0, 0, 0.6f));
        // Glass FLOOR slab (cab footprint, thin) — the see-through bottom the rider stands
        // on; drawn translucent at the cab plate each frame (replaces the opaque plate).
        x3::rhi::MeshHandle carFloorMesh = makeWorldMesh(
            x3::prims::makeBox(carHX, platHY, carHZ, 0, 0, 0, 0.4f));
        // Four slim vertical guide POSTS flanking the shaft (opaque structure), so the
        // shaft reads as built while staying mostly open for the see-through ride. They
        // span from the base floor up to the deck rail height.
        const float postT = 0.10f;
        const float postTopY = deckTopY + 1.0f;                 // up past the deck
        const float postH  = (postTopY - atriumFloorY) * 0.5f;  // half-height (atrium -> deck)
        const float postCY = atriumFloorY + postH;
        x3::rhi::MeshHandle postMesh = makeWorldMesh(
            x3::prims::makeBox(postT, postH, postT, 0, 0, 0, 1.0f));
        auto addPost = [&](float px, float pz) {
            x3::game::Entity e; e.mesh = postMesh;
            e.baseColor[0]=0.20f; e.baseColor[1]=0.22f; e.baseColor[2]=0.28f; e.baseColor[3]=1.0f;
            e.transform[12]=px; e.transform[13]=postCY; e.transform[14]=pz;
            sscene.add(e);
        };
        addPost(shaftX - carHX - 0.25f, shaftZ - carHZ - 0.25f);
        addPost(shaftX + carHX + 0.25f, shaftZ - carHZ - 0.25f);
        addPost(shaftX - carHX - 0.25f, shaftZ + carHZ + 0.25f);
        addPost(shaftX + carHX + 0.25f, shaftZ + carHZ + 0.25f);
        x3::logInfo("--world showroom: PHASE2 glass elevator shaft(" + std::to_string(shaftX) + "," +
                    std::to_string(shaftZ) + ") stops cab-top {atriumFloorY=" + std::to_string(atriumFloorY) +
                    ", deck=" + std::to_string(deckTopY) + "} carry-via ElevatorSystem");

        // -------------------------------------------------------------------
        // ELEVATOR ATRIUM (at the strut-stair landing level, atriumFloorY=14) + a
        // short BRIDGE from the hollow strut's head to the glass-elevator boarding.
        // (This SUPERSEDES the old 2nd-floor partition-door + passage + up-stair: the
        //  new route is keypad door in the strut face -> stair UP inside the strut ->
        //  this atrium -> glass elevator -> deck.) All white-clad, all walkable.
        // -------------------------------------------------------------------
        {
            // Atrium floor pad around the lift shaft (shaftX=cx+9, shaftZ=-100) at Y14.
            const float atX0 = shaftX - 9.0f, atX1 = shaftX + 5.0f;
            const float atZ0 = shaftZ - 9.0f, atZ1 = shaftZ + 5.0f;
            polish(addWhiteBox((atX0+atX1)*0.5f, atriumFloorY - 0.25f, (atZ0+atZ1)*0.5f,
                        (atX1-atX0)*0.5f, 0.25f, (atZ1-atZ0)*0.5f));   // POLISHED (refl money shot)
            // BRIDGE: a white walkway from the strut head landing (sBL.t*) to the atrium
            // edge, both at atriumFloorY, so the player crosses from the strut to the lift.
            {
                const float ax0 = std::min(sBL.tx, atX0) - 1.6f, ax1 = std::max(sBL.tx, atX0) + 1.6f;
                const float az0 = std::min(sBL.tz, atZ0) - 1.6f, az1 = std::max(sBL.tz, atZ0) + 1.6f;
                addWhiteBox((ax0+ax1)*0.5f, atriumFloorY - 0.25f, (az0+az1)*0.5f,
                            (ax1-ax0)*0.5f, 0.25f, (az1-az0)*0.5f);
            }
            x3::logInfo("--world showroom: ELEVATOR ATRIUM floor y=" + std::to_string(atriumFloorY) +
                        " around shaft(" + std::to_string(shaftX) + "," + std::to_string(shaftZ) +
                        ") + bridge from strut head(" + std::to_string(sBL.tx) + "," + std::to_string(sBL.tz) + ")");
        }

        // ===================================================================
        // HIDDEN ANALYST GALLERY (OWNER'S VISION). A secret surveillance level
        // ringing the CENTRAL VOID above the civilian floor, AT THE ELEVATOR
        // LEVEL (galleryY == atriumFloorY) so the strut stair lands ON it and the
        // glass elevator boards FROM it. A walkable white-panel RING (annulus)
        // around an OPEN VOID over the building center (cx,cz); through the void
        // (rimmed with DARK ONE-WAY GLASS) the analysts look DOWN onto the
        // civilians on the ground floor (Y=floorY) + the 2nd floor (Y=floor2Y).
        // HOLOGRAPHIC TERMINALS (reuse holo_terminal.cpp) ring the void facing in;
        // a subset carry idle ANALYST FIGURES (RescueVictim skinned, never rescued).
        //
        // DARK-GLASS BALUSTRADE (real-time, fixed-alpha BLEND path — no per-pixel
        // fresnel): the void edge is treated with an ELEGANT thin parapet (low white
        // kerb + slim cap rail) topped by a band of FLAT DARK-TINTED GLASS held by
        // slim metal mullions — like the Unity interior's glass railings. The dark
        // tint gives the analysts a shaded look-down onto the civilians while the void
        // stays OPEN below the glass band (so the gallery still overlooks the floor).
        // LIMITATION: the tint is a fixed-alpha approximation, not a true angle-
        // dependent one-way material — but it reads sleek + minimal (NOT a lumpy
        // louver/gear). Glass tint/alpha (galGlass* below) are the TUNE POINTS.
        // ===================================================================
        // Gallery ring sits at the elevator level so it connects to the existing
        // strut-stair landing + bridge + elevator boarding (all at atriumFloorY).
        const float galleryY   = atriumFloorY;          // walkable ring surface (== Y10)
        const float voidR      = 9.0f;                  // central VOID radius (open down-look)
        const float galOuterR  = 17.0f;                 // ring outer radius (~8 m wide balcony)
        // Ring built as a fan of trapezoidal SEGMENTS around (cx,cz). Each segment is a
        // white-clad box laid along its mid-radius arc; collision on each so it's walkable.
        const int   galSeg     = 16;                    // ring segments (+ terminal slots)
        const float galMidR    = (voidR + galOuterR) * 0.5f;
        // Persistent gallery glass state (drawn each frame via the BLEND path, like the
        // deck/elevator glass). The slats + rim pane are authored at WORLD positions so
        // their model matrix is a per-slat rotation about the slat center.
        struct GalGlass { x3::rhi::MeshHandle mesh; float model[16]; float r,g,b,a,emis; };
        std::vector<GalGlass> galGlass;
        // Holographic terminals around the ring + the analyst figures at a subset.
        std::vector<x3::game::HoloTerminal> galTerms;
        std::vector<x3::game::RescueVictim> galAnalysts;
        galTerms.reserve(galSeg);
        galAnalysts.reserve(6);
        {
            // ---- (1) WALKABLE RING FLOOR — galSeg trapezoid segments forming an annulus
            // around the void, white-clad + collision. A small thick slab per segment
            // (top at galleryY) tangent to its arc. Gaps at the strut-landing + elevator
            // sides are bridged by the existing atrium/bridge floor (both at atriumFloorY).
            const float ringHalfRad = (galOuterR - voidR) * 0.5f;   // radial half-extent of a segment
            const float ringHY = 0.22f;                              // 0.44 m thick floor slab
            const float ringCY = galleryY - ringHY;                  // center so TOP == galleryY
            for (int s = 0; s < galSeg; ++s) {
                const float ang = (6.2831853f * (s + 0.5f)) / (float)galSeg;
                const float ca = std::cos(ang), sa = std::sin(ang);
                const float segX = cx + ca * galMidR, segZ = cz + sa * galMidR;
                // Tangential half-width sized so adjacent segments overlap into a closed ring.
                const float tanHW = (3.14159265f * galMidR) / (float)galSeg + 0.35f;
                // Author the segment as an axis-aligned box then rotate it to lie along the
                // arc tangent via a yaw model matrix (addWhiteMesh uses world meshes; here we
                // build a rotated box by composing the rotation into vertices is overkill —
                // instead use a radial-aligned box: radial = ringHalfRad, tangential = tanHW,
                // approximated axis-aligned per-segment which is fine at 16 segments).
                // Build the segment in LOCAL (radial=x, tangential=z) then place rotated:
                // a white-clad render box + a matching rotated static collision body.
                x3::prims::PrimMesh seg = x3::prims::makeBox(ringHalfRad, ringHY, tanHW, 0,0,0, 0.3f);
                // Rotate verts by `ang` about Y so radial axis points outward.
                for (auto& v : seg.verts) {
                    const float lx = v.pos[0], lz = v.pos[2];
                    v.pos[0] = lx * ca - lz * sa + segX;
                    v.pos[1] += ringCY;
                    v.pos[2] = lx * sa + lz * ca + segZ;
                    const float nx = v.normal[0], nz = v.normal[2];
                    v.normal[0] = nx * ca - nz * sa; v.normal[2] = nx * sa + nz * ca;
                }
                x3::rhi::MeshHandle m = makeWorldMesh(seg);
                x3::game::Entity e; e.mesh = m; e.tex = whitePanelTex;
                e.baseColor[0]=kWhite[0]; e.baseColor[1]=kWhite[1]; e.baseColor[2]=kWhite[2]; e.baseColor[3]=1.0f;
                sscene.add(e);
                // Collision: a small axis-aligned box at the segment center (a touch larger so
                // the ring is seamlessly walkable; the player capsule never notices the facets).
                sphys->addBox({ tanHW*std::fabs(sa) + ringHalfRad*std::fabs(ca) + 0.1f, ringHY,
                                tanHW*std::fabs(ca) + ringHalfRad*std::fabs(sa) + 0.1f },
                              { segX, ringCY, segZ }, 0.0f, x3::phys::Layer::Static);
            }
            // ---- (2) VOID-EDGE PARAPET + DARK-GLASS BALUSTRADE — an ELEGANT, THIN,
            // sleek treatment of the void rim (replacing the old chunky tilted-slat
            // "louver" ring that read as a crude gear/cog). It mirrors the Unity
            // interior's glass railings: a SMOOTH LOW PARAPET (a thin white kerb + a
            // slim white cap rail) topped by a continuous band of FLAT DARK-TINTED
            // GLASS held by SLIM METAL MULLIONS. The analysts still get a dark
            // look-down onto the civilians (the glass is dark-tinted, see-through
            // looking down through the open void below the glass band), and the
            // railing/safety read is preserved. Minimal geometry: a clean kerb ring,
            // a thin cap ring, slim mullion posts, and ONE merged flat glass band.
            //   * kerb     — a thin white solid ~0.35 m tall at the void edge (the
            //                low parapet base; gives a clean lip + collision).
            //   * cap rail — a slim white bar capping the glass band (the handrail).
            //   * mullions — slim dark metal posts at each segment (hold the glass).
            //   * glass    — ONE merged ring of FLAT vertical dark-tinted panes via the
            //                existing BLEND path (galGlass), set just inboard of the kerb.
            const float kerbH       = 0.35f;                 // low parapet kerb height (m)
            const float kerbHY      = kerbH * 0.5f;
            const float kerbHRad    = 0.07f;                 // thin radial half-thickness
            const float glassH      = 0.62f;                 // dark-glass band height (m)
            const float glassTopY   = galleryY + kerbH + glassH;  // top of the glass = handrail height (~1.0 m)
            const float capHY       = 0.04f;                 // slim cap-rail half-height
            const float capHRad     = 0.10f;                 // slim cap-rail radial half-depth
            const float mullHRad    = 0.045f, mullHTan = 0.045f;  // slim mullion post half-dims
            // Dark-tinted FLAT glass (smoky, low transmission) — same dark tint family
            // the deck/elevator glass uses; alpha = BLEND opacity (dark, see-through down).
            const float galGlassR = 0.030f, galGlassG = 0.040f, galGlassB = 0.065f;
            const float galGlassA = 0.62f;
            // The kerb + cap rails are rotated boxes at each segment (like the ring floor).
            for (int s = 0; s < galSeg; ++s) {
                const float ang = (6.2831853f * (s + 0.5f)) / (float)galSeg;
                const float ca = std::cos(ang), sa = std::sin(ang);
                const float tanHW = (3.14159265f * voidR) / (float)galSeg + 0.25f;
                // Place a white box (radial half=hRad, given height) at radius `rad`,
                // centered at world Y `cy`, rotated to lie along the arc tangent. Adds a
                // render entity (clean white panel) + a matching static collision body.
                auto placeRing = [&](float rad, float cy, float hRad, float hY, float colTanPad) {
                    const float rx = cx + ca * rad, rz = cz + sa * rad;
                    x3::prims::PrimMesh b = x3::prims::makeBox(hRad, hY, tanHW, 0,0,0, 1.0f);
                    for (auto& v : b.verts) {
                        const float lx = v.pos[0], lz = v.pos[2];
                        v.pos[0] = lx * ca - lz * sa + rx;
                        v.pos[1] += cy;
                        v.pos[2] = lx * sa + lz * ca + rz;
                        const float nx = v.normal[0], nz = v.normal[2];
                        v.normal[0] = nx * ca - nz * sa; v.normal[2] = nx * sa + nz * ca;
                    }
                    x3::rhi::MeshHandle m = makeWorldMesh(b);
                    x3::game::Entity e; e.mesh = m; e.tex = whitePanelTex;
                    e.baseColor[0]=kWhite[0]; e.baseColor[1]=kWhite[1]; e.baseColor[2]=kWhite[2]; e.baseColor[3]=1.0f;
                    sscene.add(e);
                    if (colTanPad >= 0.0f)
                        sphys->addBox({ tanHW*std::fabs(sa) + hRad*std::fabs(ca) + colTanPad, hY,
                                        tanHW*std::fabs(ca) + hRad*std::fabs(sa) + colTanPad },
                                      { rx, cy, rz }, 0.0f, x3::phys::Layer::Static);
                };
                // Low parapet KERB at the void edge (collision = the safety barrier).
                placeRing(voidR + 0.10f, galleryY + kerbHY, kerbHRad, kerbHY, 0.05f);
                // Slim CAP RAIL atop the glass band (the handrail; thin collision lid).
                placeRing(voidR + 0.10f, glassTopY + capHY, capHRad, capHY, 0.0f);
            }
            // SLIM MULLIONS — a dark metal post at each segment boundary, spanning kerb
            // top -> cap, holding the glass. Built white-clad-mesh path but tinted dark.
            {
                const float mullBaseY = galleryY + kerbH;
                const float mullHY    = glassH * 0.5f;
                for (int s = 0; s < galSeg; ++s) {
                    const float ang = (6.2831853f * (float)s) / (float)galSeg;   // on segment edges
                    const float ca = std::cos(ang), sa = std::sin(ang);
                    const float rx = cx + ca * (voidR + 0.10f), rz = cz + sa * (voidR + 0.10f);
                    x3::prims::PrimMesh post = x3::prims::makeBox(mullHRad, mullHY, mullHTan, 0,0,0, 1.0f);
                    for (auto& v : post.verts) {
                        const float lx = v.pos[0], lz = v.pos[2];
                        v.pos[0] = lx * ca - lz * sa + rx;
                        v.pos[1] += mullBaseY + mullHY;
                        v.pos[2] = lx * sa + lz * ca + rz;
                        const float nx = v.normal[0], nz = v.normal[2];
                        v.normal[0] = nx * ca - nz * sa; v.normal[2] = nx * sa + nz * ca;
                    }
                    x3::rhi::MeshHandle m = makeWorldMesh(post);
                    x3::game::Entity e; e.mesh = m; e.tex = whitePanelTex;
                    // Slim dark metal mullion tint (cool gunmetal, distinct from the white).
                    e.baseColor[0]=0.16f; e.baseColor[1]=0.18f; e.baseColor[2]=0.22f; e.baseColor[3]=1.0f;
                    sscene.add(e);   // visual only (no collision; the kerb is the barrier)
                }
            }
            // ---- (3) FLAT DARK-GLASS BAND — ONE merged ring of FLAT VERTICAL dark-tinted
            // panes spanning the kerb top -> cap rail, just inboard of the void edge. The
            // dark tint gives the analysts a shaded look-down onto the civilians (and the
            // void is OPEN below the glass band, so the gallery still overlooks the floor);
            // the flat vertical glass reads as a sleek railing pane (NOT a lumpy louver).
            // Drawn via the existing BLEND path (galGlass) as a SINGLE merged draw.
            {
                const int   galGlassSeg = 32;                 // panes around the ring (smooth band)
                const float glassMidY   = galleryY + kerbH + glassH * 0.5f;
                const float glassHY     = glassH * 0.5f;
                const float glassHTan   = (3.14159265f * voidR) / (float)galGlassSeg + 0.10f;
                const float glassHThk   = 0.02f;              // thin flat pane
                x3::prims::PrimMesh glassMerged;
                for (int s = 0; s < galGlassSeg; ++s) {
                    const float ang = (6.2831853f * (s + 0.5f)) / (float)galGlassSeg;
                    const float ca = std::cos(ang), sa = std::sin(ang);
                    const float panX = cx + ca * (voidR + 0.05f), panZ = cz + sa * (voidR + 0.05f);
                    // Flat vertical pane: radial=thin, height=glassHY, tangential=glassHTan;
                    // rotate about Y to lie along the arc tangent (NO tilt -> flat band).
                    x3::prims::PrimMesh pane = x3::prims::makeBox(glassHThk, glassHY, glassHTan, 0,0,0, 1.0f);
                    const uint32_t vb = (uint32_t)glassMerged.verts.size();
                    for (auto v : pane.verts) {
                        const float lx = v.pos[0], lz = v.pos[2];
                        v.pos[0] = lx * ca - lz * sa + panX;
                        v.pos[1] += glassMidY;
                        v.pos[2] = lx * sa + lz * ca + panZ;
                        const float nx = v.normal[0], nz = v.normal[2];
                        v.normal[0] = nx * ca - nz * sa; v.normal[2] = nx * sa + nz * ca;
                        glassMerged.verts.push_back(v);
                    }
                    for (uint32_t idx : pane.index) glassMerged.index.push_back(vb + idx);
                }
                GalGlass gg{}; gg.mesh = makeWorldMesh(glassMerged);
                gg.model[0]=1;gg.model[5]=1;gg.model[10]=1;gg.model[15]=1;   // identity (verts in world)
                gg.r=galGlassR; gg.g=galGlassG; gg.b=galGlassB; gg.a=galGlassA; gg.emis=0.03f;
                galGlass.push_back(gg);
            }

            // ---- (4) HOLOGRAPHIC TERMINALS — reuse holo_terminal.cpp. One per segment
            // slot, on the OUTER side of the ring facing INWARD toward the void (so an
            // analyst standing between the terminal and the rail watches the floor below
            // over the readout). ~10-12 around the ring. Each seeds a surveillance-feed
            // readout. Terminal anchor sits at chest height on the ring floor.
            const int kNumTerms = 11;                 // ~10-12 terminals
            const float termR = galOuterR - 2.2f;     // terminal stands near the outer wall
            const float termY = galleryY + 1.25f;     // screen center at chest/eye height
            for (int t = 0; t < kNumTerms; ++t) {
                const float ang = (6.2831853f * t) / (float)kNumTerms + 0.18f;
                const float ca = std::cos(ang), sa = std::sin(ang);
                const float tx = cx + ca * termR, tz = cz + sa * termR;
                // Face the terminal INWARD (toward the void center). HoloTerminal::build
                // yaws the screen's local +Z front by `yaw`; aim it at (cx,cz) so the
                // readout faces an analyst standing between the terminal and the rail.
                const float inwardYaw = std::atan2(cx - tx, cz - tz);
                galTerms.emplace_back();
                x3::game::HoloTerminal& term = galTerms.back();
                term.build(sscene, *device, x3::phys::Vec3{ tx, termY, tz }, inwardYaw,
                           1.2f, 0.78f, /*ceilingY*/ galleryY + 2.6f);
                // Surveillance-feed readout (line 0 = header title; 1+ = data rows).
                term.setLines({
                    std::string("SURVEILLANCE FEED ") + (char)('A' + (t % 8)) +
                        "-" + std::to_string(10 + t),
                    "ZONE: CIVILIAN ATRIUM",
                    "TRACKING: ACTIVE",
                    std::string("CONTACTS: ") + std::to_string(3 + (t * 5) % 9),
                    "BIOMETRICS: NOMINAL",
                    "ONE-WAY GLASS: ENGAGED",
                });
            }
            x3::logInfo("--world showroom: GALLERY terminals = " + std::to_string(galTerms.size()) +
                        " around void (r=" + std::to_string(termR) + ") facing in");

            // ---- (5) ANALYST FIGURES — a modest subset (4) of skinned idle figures at
            // four spread terminals, facing their terminal/the void. Reuse RescueVictim
            // (the AnnaCasual_anim.glb idle path Aria uses); built as Captive + never
            // rescued + hubReached=false so the timer never runs and there's no follow AI
            // — they just idle (breathe) in place. setFacing aims each at the void center.
            const int kNumAnalysts = 4;
            const int kTermsPerAnalyst = (galTerms.empty() ? 1 : (int)galTerms.size()) / kNumAnalysts;
            for (int aN = 0; aN < kNumAnalysts; ++aN) {
                const int slot = aN * std::max(1, kTermsPerAnalyst);
                const float ang = (6.2831853f * slot) / (float)kNumTerms + 0.18f;
                const float ca = std::cos(ang), sa = std::sin(ang);
                // Stand ~1.4 m IN from the terminal (between it and the rail) on the ring.
                const float aR = galOuterR - 3.6f;
                const float axp = cx + ca * aR, azp = cz + sa * aR;
                galAnalysts.emplace_back();
                x3::game::RescueVictim& an = galAnalysts.back();
                an.build(sscene, *device, *sphys, x3::game::riggedGlbRoot(),
                         x3::phys::Vec3{ axp, galleryY, azp },
                         x3::game::VictimId::Aria, std::string("Analyst") + std::to_string(aN + 1),
                         "AnnaCasual_anim.glb", 1e9f /*never expires*/,
                         x3::game::MonsterSystem::Tuning{});
                // Face the void center (and thus the terminal, which is just outward of it).
                // headingToFace law (CONVENTIONS): yaw = atan2(-dirX,-dirZ) points local -Z
                // along (dirX,dirZ); dir = center - self.
                an.setFacing(std::atan2(-(cx - axp), -(cz - azp)));
                // A cool analyst tint (distinct from Aria's friendly cyan).
                an.setTint(0.78f, 0.82f, 0.92f, 1.0f);
            }
            x3::logInfo("--world showroom: GALLERY analysts = " + std::to_string(galAnalysts.size()) +
                        " (skinned idle, facing the void) + dark-glass parapet = " +
                        std::to_string(galGlass.size()) + " merged BLEND mesh (flat dark-glass balustrade band)");
        }
        x3::logInfo("--world showroom: hidden-trigger = KEYPAD code-entry (press E at the STRUT-FACE door, type code " +
                    std::to_string(HATCH_CODE) + ", Enter to submit) — the white panel slides aside, stair climbs inside the strut");

        // ===================================================================
        // CIVILIAN FIGURES — the public milling on the GROUND floor + 2nd-floor
        // mezzanine (the museum-lobby crowd). EXACT same reuse pattern as the
        // companion Aria + the gallery ANALYSTS: each is a RescueVictim built
        // Captive, AnnaCasual_anim.glb idle, timer 1e9 (never expires),
        // hubReached=false (no countdown, no follow AI) -> they just idle/breathe
        // in place. setFacing() holds each at a natural static heading (small
        // groups facing each other / facing out the glass / toward the blue pad).
        // WARMER, VARIED tints (vs the analysts' cool blue-grey 0.78/0.82/0.92)
        // so they read as the PUBLIC, not staff. Positions are hand-jittered (NOT
        // a grid) on the walkable floors: GROUND at y=floorY (-9) around the
        // central blue pad + lounge, 2ND FLOOR at y=floor2Y (3) on the Room_01
        // mezzanine deck. setFacing law (CONVENTIONS / setFacing doc): yaw =
        // atan2(-dirX,-dirZ) aims the model's local -Z along (dirX,dirZ); to face
        // a target T from self S pass dir = T - S.
        //
        // PERF: each skinned tick() does a GPU readback (vkDeviceWaitIdle) ->
        // costly under 4x headless SSAA. The headless proofs POSE these on the
        // first ~2 frames then render static (see the proof loops). Total skinned
        // figures kept modest: Aria(1) + analysts(4) + civilians(8) = 13.
        std::vector<x3::game::RescueVictim> civilians;
        civilians.reserve(8);
        {
            // A civilian = {x,z, facing-target x,z, tint r,g,b, y-level}. Facing a
            // target point reads more natural than a raw yaw (groups face each
            // other / the pad / out the glass). Warm/varied civilian palette.
            struct Civ { float x, z, tx, tz, r, g, b, y; const char* name; };
            const float padX = cx, padZ = cz;            // central blue pad center (social heart)
            const std::vector<Civ> civDefs = {
                // ---- GROUND floor (y=floorY): ~5 around the blue pad + lounge ----
                // A) a chatting PAIR just off the pad's +X edge, facing each other.
                { padX + 4.6f, padZ + 1.2f,  padX + 6.2f, padZ + 2.0f,  0.86f, 0.52f, 0.46f, floorY, "Civ_PairA" }, // warm terracotta
                { padX + 6.4f, padZ + 2.4f,  padX + 4.6f, padZ + 1.2f,  0.52f, 0.66f, 0.40f, floorY, "Civ_PairB" }, // warm olive
                // B) a lone visitor at the pad's -X lounge edge, gazing OUT the glass (-X).
                { padX - 5.8f, padZ - 1.5f,  padX - 40.0f, padZ - 1.5f, 0.80f, 0.74f, 0.42f, floorY, "Civ_Gazer" }, // warm gold
                // C) a small group of two on the -Z lounge arc, facing IN toward the pad.
                { padX - 1.4f, padZ - 6.2f,  padX,         padZ,        0.74f, 0.50f, 0.70f, floorY, "Civ_TrioA" }, // warm mauve
                { padX + 2.0f, padZ - 7.0f,  padX,         padZ,        0.58f, 0.62f, 0.84f, floorY, "Civ_TrioB" }, // soft periwinkle
                // ---- 2ND-FLOOR mezzanine (y=floor2Y): ~3 on the Room_01 deck ----
                // Deck footprint x~[29..115], z~[-123..-99]; place clear of the central
                // tower gap (x~71..73) + the void, looking along the deck / down at the pad.
                // D) a pair at the +X end of the deck, facing each other near the rail.
                { 96.0f, -114.0f,  93.0f, -112.0f,  0.84f, 0.58f, 0.40f, floor2Y, "Civ_MezzA" }, // warm amber
                { 92.0f, -112.5f,  96.0f, -114.0f,  0.66f, 0.78f, 0.62f, floor2Y, "Civ_MezzB" }, // sage
                // E) a lone figure at the -X end of the deck, looking DOWN toward the pad below.
                { 40.0f, -107.0f,  cx,    cz,        0.82f, 0.66f, 0.50f, floor2Y, "Civ_MezzC" }, // warm tan
            };
            for (const auto& c : civDefs) {
                civilians.emplace_back();
                x3::game::RescueVictim& cv = civilians.back();
                cv.build(sscene, *device, *sphys, x3::game::riggedGlbRoot(),
                         x3::phys::Vec3{ c.x, c.y, c.z },
                         x3::game::VictimId::Aria, c.name,
                         "AnnaCasual_anim.glb", 1e9f /*never expires*/,
                         x3::game::MonsterSystem::Tuning{});
                cv.setFacing(std::atan2(-(c.tx - c.x), -(c.tz - c.z)));
                cv.setTint(c.r, c.g, c.b, 1.0f);
            }
            x3::logInfo("--world showroom: CIVILIANS = " + std::to_string(civilians.size()) +
                        " skinned idle (5 ground @ y=" + std::to_string(floorY) +
                        " around blue pad + lounge, 3 mezzanine @ y=" + std::to_string(floor2Y) +
                        "), warm/varied tint, facing pad/each-other/glass");
        }
#if 0
        // -------------------------------------------------------------------
        // [SUPERSEDED] STAGE 2/3 — HIDDEN 2ND-FLOOR WALL DOOR -> entry passage -> 90 deg
        // turn -> flight of stairs UP -> ELEVATOR ATRIUM. REPLACED by the strut-face
        // keypad door + internal strut stair above. Kept under #if 0 for reference only.
        // Concealed entrance on the 2ND FLOOR: a flush WHITE wall panel set into the
        // GLB Pilar_01/02 left BACK wall (z~-121) — chosen because the player has
        // ample 2nd-floor room IN FRONT of it (interior side, z>-120) to walk up and
        // face it, while CONCEALED space sits behind it (z<-122). On the keypad code
        // (2742) the panel SLIDES ASIDE; you walk -Z into the passage, TURN +X, then
        // a grand FLIGHT OF STAIRS climbs +X-and-up to the ELEVATOR ATRIUM where the
        // glass lift boards. The keypad mechanic + code are UNCHANGED (the hatch*
        // variables below drive the WALL DOOR; the same KeypadEntry + value()==
        // kShowroomHatchCode gate that --test-hatchcode shares).
        // -------------------------------------------------------------------
        // ---- The hidden DOOR (a flush white wall panel) set into a WHITE PARTITION
        // WALL I build across the open 2nd-floor mid-room (clear of the thick GLB
        // structural walls at z~-101 / z~-121, which would otherwise occlude it). The
        // partition is axis-aligned + clad in the same white panels, so it reads as a
        // built interior wall; its sill rests on the 2nd floor (floor2Y). The player,
        // having climbed to the 2nd floor (landing at z~-104), faces -Z toward it.
        const float doorX = 52.0f;             // door center X (left half, near the climb landing)
        const float doorZ = -106.0f;           // partition plane (open mid-room, clear of GLB walls)
        const float doorHalfW = 1.2f;          // 2.4 m wide opening
        const float doorHalfH = 1.25f;         // 2.5 m tall (fits under the GLB Tube vault at y~6)
        const float doorPanelHZ = 0.18f;       // panel thickness (set into the partition)
        const float doorCY = floor2Y + doorHalfH;   // panel center (sill on the 2nd floor)
        // Keep the proven keypad-host variable NAMES (hatch*) so the interaction +
        // smoke code is untouched; they now address the WALL DOOR on the 2nd floor.
        const float hatchX = doorX;            // door center X (proximity test)
        const float hatchZ = doorZ;            // door plane Z (proximity test)
        const float hatchHalf = doorHalfW;     // proximity half-window
        const float doorFloorY = floor2Y;      // the floor the player stands on to use it
        constexpr int HATCH_CODE = kShowroomHatchCode;   // 2742 (UNCHANGED) — themed "ARIA"

        // WHITE PARTITION WALL the door sits in: flanking jambs + a lintel above,
        // floor-to-vault, clad in white panels + solid collision. The door opening is
        // the only gap (the player approaches from +Z, the climb-landing side).
        const float partHalfH = 1.4f;          // partition half-height (~2.8 m, to the vault)
        const float partCY = floor2Y + partHalfH;
        {
            const float jambW = 5.0f;          // jamb half-extent each side
            // -X jamb + +X jamb (the door opening is the gap between them).
            addWhiteBox(doorX - doorHalfW - jambW, partCY, doorZ, jambW, partHalfH, doorPanelHZ);
            addWhiteBox(doorX + doorHalfW + jambW, partCY, doorZ, jambW, partHalfH, doorPanelHZ);
            // Lintel above the opening up to the partition top.
            addWhiteBox(doorX, (floor2Y + doorHalfH*2.0f + (floor2Y + partHalfH*2.0f))*0.5f, doorZ,
                        doorHalfW, ((floor2Y + partHalfH*2.0f) - (floor2Y + doorHalfH*2.0f))*0.5f, doorPanelHZ);
        }

        // Concealed door PANEL: a thin white box flush in the partition, clad in the
        // SAME white panel texture so it is invisible until opened. It slides +X aside
        // on unlock; its Static collision body is REMOVED on open so the player walks
        // through. A faint cyan glow pulses when the player is near + it's still closed.
        x3::rhi::MeshHandle hatchMesh = makeWorldMesh(
            x3::prims::makeBox(doorHalfW, doorHalfH, doorPanelHZ, 0, 0, 0, 0.25f));
        x3::game::Entity hatchEnt; hatchEnt.mesh = hatchMesh; hatchEnt.tex = whitePanelTex;
        hatchEnt.baseColor[0]=kWhite[0]; hatchEnt.baseColor[1]=kWhite[1]; hatchEnt.baseColor[2]=kWhite[2]; hatchEnt.baseColor[3]=1.0f;
        hatchEnt.emissive[0]=0.10f; hatchEnt.emissive[1]=0.45f; hatchEnt.emissive[2]=0.55f; hatchEnt.emissive[3]=0.0f; // glows only when armed
        hatchEnt.transform[12]=doorX; hatchEnt.transform[13]=doorCY; hatchEnt.transform[14]=doorZ;
        const uint32_t hatchIdx = sscene.add(hatchEnt);
        // The panel's solid collision while CLOSED (it seals the wall). Removed on open.
        x3::phys::BodyId hatchLidBody =
            sphys->addBox({ doorHalfW, doorHalfH, doorPanelHZ }, { doorX, doorCY, doorZ }, 0.0f, x3::phys::Layer::Static);
        bool  hatchOpen = false;        // latched once triggered
        float hatchSlide = 0.0f;        // 0=closed .. 1=fully slid aside

        // ---- Aligned interior run, all WHITE-clad + walkable, behind the door:
        //   ENTRY PASSAGE : -Z from the door into the open mid-room (y=floor2Y).
        //   90 deg TURN   : dogleg from -Z to +X.
        //   STAIRS UP     : a grand +X flight climbing floor2Y -> atriumFloorY.
        //   ELEVATOR ATRIUM: a white room at y=atriumFloorY enclosing the lift shaft.
        const float passHalfW   = 1.6f;        // passage/turn corridor half-width
        const float passTopGap  = 2.4f;        // interior head-height (fits under the GLB vault)
        const float passDoorZ   = doorZ - doorPanelHZ;   // -Z (concealed) face of the door
        const float passEndZ    = -113.0f;     // back of the entry passage (the turn corner)
        const float turnCX      = doorX;        // the dogleg corner X (== door X)
        const float upStairLowX = doorX + passHalfW + 1.0f;  // stair low edge (on the 2nd floor)
        const float upStairRun  = 18.0f;       // +X run (gentle grand flight)
        const float upStairRise = atriumFloorY - floor2Y;   // 6 m
        const float stairZ      = passEndZ;     // the +X stair runs along the turn-corner Z line
        const float atriumX0    = 70.0f, atriumX1 = 91.0f;  // atrium X span (encloses shaftX=81)
        const float atriumZ0    = -115.0f, atriumZ1 = -98.5f; // atrium Z span (encloses shaftZ=-100)
        // Helper: a white floor slab (top at topY) of the given XZ rect.
        auto whiteFloor = [&](float x0, float x1, float z0, float z1, float topY) {
            addWhiteBox((x0+x1)*0.5f, topY - 0.25f, (z0+z1)*0.5f, (x1-x0)*0.5f, 0.25f, (z1-z0)*0.5f);
        };
        // Helper: a white ceiling slab (bottom at botY).
        auto whiteCeil = [&](float x0, float x1, float z0, float z1, float botY) {
            addWhiteBox((x0+x1)*0.5f, botY + 0.15f, (z0+z1)*0.5f, (x1-x0)*0.5f, 0.15f, (z1-z0)*0.5f);
        };
        // (A) ENTRY PASSAGE: floor + ceiling + the two side walls, from the door
        // (z=passDoorZ) -Z back to the turn corner (z=passEndZ), centered on doorX.
        {
            const float wallHy = passTopGap * 0.5f, wallCy = floor2Y + wallHy;
            whiteFloor(turnCX - passHalfW - 0.3f, turnCX + passHalfW + 0.3f, passEndZ + passHalfW - 0.3f, passDoorZ, floor2Y);
            whiteCeil (turnCX - passHalfW - 0.3f, turnCX + passHalfW + 0.3f, passEndZ + passHalfW - 0.3f, passDoorZ, floor2Y + passTopGap);
            // -X side wall of the passage (full length). +X side wall ONLY on the door
            // half (the -Z end opens via the turn into the +X stair run).
            addWhiteBox(turnCX - passHalfW - 0.15f, wallCy, (passEndZ + passDoorZ)*0.5f, 0.15f, wallHy, (passDoorZ - passEndZ)*0.5f);
            addWhiteBox(turnCX + passHalfW + 0.15f, wallCy, (passDoorZ + (passEndZ + 2*passHalfW))*0.5f, 0.15f, wallHy, (passDoorZ - (passEndZ + 2*passHalfW))*0.5f);
        }
        // (B) 90 deg TURN corner: a white floor+ceiling patch at (turnCX,passEndZ)
        // bridging the -Z passage into the +X stair run; a -Z back wall seals the corner.
        {
            const float wallHy = passTopGap * 0.5f, wallCy = floor2Y + wallHy;
            whiteFloor(turnCX - passHalfW - 0.3f, upStairLowX + 0.5f, passEndZ - passHalfW - 0.3f, passEndZ + passHalfW + 0.3f, floor2Y);
            whiteCeil (turnCX - passHalfW - 0.3f, upStairLowX + 0.5f, passEndZ - passHalfW - 0.3f, passEndZ + passHalfW + 0.3f, floor2Y + passTopGap);
            addWhiteBox((turnCX + upStairLowX)*0.5f, wallCy, passEndZ - passHalfW - 0.15f, (upStairLowX + passHalfW - turnCX)*0.5f + 0.3f, wallHy, 0.15f); // -Z back wall
        }
        // (C) STAIRS UP: a grand white ramp climbing +X from the 2nd floor (floor2Y) to
        // the atrium (atriumFloorY), centered on stairZ. Matches the building's stairs.
        {
            x3::prims::PrimMesh ramp = x3::prims::makeRamp(upStairLowX, floor2Y, stairZ,
                                                          passHalfW, upStairRun, upStairRise,
                                                          /*axis*/0 /*+X*/, /*dir*/+1.0f, 0.25f);
            x3::rhi::MeshHandle m = makeWorldMesh(ramp);
            x3::game::Entity e; e.mesh = m; e.tex = whitePanelTex;
            e.baseColor[0]=kWhite[0]; e.baseColor[1]=kWhite[1]; e.baseColor[2]=kWhite[2]; e.baseColor[3]=1.0f;
            sscene.add(e);
            sphys->addStaticMesh(ramp.cverts.data(), (uint32_t)ramp.cverts.size() / 3,
                                 ramp.cindex.data(), (uint32_t)ramp.cindex.size());
        }
        // (D) ELEVATOR ATRIUM: a white room at atriumFloorY around the lift shaft. The
        // stair tops out at its -Z/-X corner; the player walks +Z across it to board the
        // cab. Floor slab + a high ceiling + three bounding walls (+Z left open for the
        // shaft view + the deck above). The shaft passes up through the ceiling gap.
        {
            whiteFloor(atriumX0, atriumX1, atriumZ0, atriumZ1, atriumFloorY);
            const float atriumWallHy = 2.2f, atriumWallCy = atriumFloorY + atriumWallHy;  // 4.4 m walls
            // Ceiling with a gap around the shaft so the cab rises through it.
            whiteCeil(atriumX0, shaftX - carHX - 0.6f, atriumZ0, atriumZ1, atriumFloorY + 4.6f); // -X of shaft
            whiteCeil(shaftX + carHX + 0.6f, atriumX1, atriumZ0, atriumZ1, atriumFloorY + 4.6f); // +X of shaft
            // Bounding walls: -X, +X, -Z. (+Z left open toward the front / shaft.)
            addWhiteBox(atriumX0 - 0.15f, atriumWallCy, (atriumZ0+atriumZ1)*0.5f, 0.15f, atriumWallHy, (atriumZ1-atriumZ0)*0.5f); // -X wall
            addWhiteBox(atriumX1 + 0.15f, atriumWallCy, (atriumZ0+atriumZ1)*0.5f, 0.15f, atriumWallHy, (atriumZ1-atriumZ0)*0.5f); // +X wall
            addWhiteBox((atriumX0+atriumX1)*0.5f, atriumWallCy, atriumZ0 - 0.15f, (atriumX1-atriumX0)*0.5f, atriumWallHy, 0.15f); // -Z wall
            x3::logInfo("--world showroom: STAGE2/3 hidden wall door(" + std::to_string(doorX) + "," +
                        std::to_string(doorZ) + ") on the 2nd floor y=" + std::to_string(floor2Y) +
                        " -> passage(-Z to z=" + std::to_string(passEndZ) + ") -> turn(+X) -> stair rise " +
                        std::to_string(upStairRise) + " m -> atrium floor y=" + std::to_string(atriumFloorY) +
                        " (shaft " + std::to_string(shaftX) + "," + std::to_string(shaftZ) + ")");
        }
        x3::logInfo("--world showroom: hidden-trigger = KEYPAD code-entry (press E at the 2nd-floor wall door, type code " +
                    std::to_string(HATCH_CODE) + ", Enter to submit) — the white panel slides aside");
#endif // [SUPERSEDED] old 2nd-floor wall-door entrance

        // ===================================================================
        // INTERIOR LIGHTING (forward POINT LIGHTS) — make the walkable INTERIOR
        // read like a clean, bright, evenly-lit white Unity interior, WITHOUT
        // touching the NIGHT sky/sun/ambient (those stay dark so the sky + planets
        // are unchanged outside). mesh.frag accumulates these on TOP of the dim
        // moonlight sun + cool ambient, so the white-panel slab/passage/stair/
        // atrium catch them and read clean white; the building's GLB emissive
        // fixtures (halogen/tube/showcase/tv_screen) still glow via their material
        // emissive (a small bloom nudge below makes them read as ceiling/strips).
        //
        // ALL lights are COOL-WHITE (color ~(1,1,1.05) pre-multiplied by an
        // intensity) placed near ceiling height in each space, range ~10-16 m,
        // spaced so the floors/walls light evenly (no dark pools, no hot blobs).
        // Shared block => applies to BOTH the interactive --world showroom AND the
        // headless proof flags. Budget: kMaxPointLights = 64.
        //   *** TUNING KNOBS: kPL_I (intensity), kPL_R (range), grid steps below.
        // plights is declared at BLOCK scope (full NIGHT intensity) so the live 'T'
        // toggle can re-push it scaled (DAY x0.3) / full (NIGHT) via the helper.
        // -------------------------------------------------------------------
        std::vector<x3::rhi::PointLight> plights;
        {
            plights.reserve(64);
            // Cool-white tint, slightly blue-biased. The 3 color channels are
            // PRE-MULTIPLIED by the intensity so the shader sees color*intensity.
            const float kPL_I = 3.4f;          // *** master interior intensity (brighter -> clean Unity-white interior, dominates the blue night ambient)
            const float kPL_R = 13.0f;         // *** master range (m); attenuation -> 0 here
            auto addLight = [&](float x, float y, float z, float range, float intensity) {
                if (plights.size() >= 64) return;
                x3::rhi::PointLight pl{};
                pl.pos[0] = x; pl.pos[1] = y; pl.pos[2] = z;
                pl.range  = range;
                pl.color[0] = 1.04f * intensity;   // clean, faintly WARM white (interior fill) —
                pl.color[1] = 1.00f * intensity;   // counters the cold blue night ambient so the
                pl.color[2] = 0.96f * intensity;   // white panels read crisp like the Unity interior
                plights.push_back(pl);
            };

            // (1) GROUND entrance / spawn area — a grid a few metres above the
            // ground floor over the building footprint center, so the spawn room
            // + the foot of the climb stair read lit. floorY ~ -9; ceiling is open,
            // so hang the lights ~6 m up. 3x3 grid centered on (cx,cz), ~14 m step.
            {
                const float gY = floorY + 6.0f;
                const float gStep = 14.0f;
                for (int ix = -1; ix <= 1; ++ix)
                    for (int iz = -1; iz <= 1; ++iz)
                        addLight(cx + ix * gStep, gY, cz + iz * gStep, 16.0f, kPL_I);
                // Extra light at the foot of the climb stair so the ascent reads.
                addLight(climbCX, floorY + 5.0f, stairLowZ + 3.0f, 14.0f, kPL_I);
            }

            // (2) 2ND-FLOOR Room_01 — x[29,115], z[-122.8,-99.2], floor y=3, vaulted
            // ceiling y~6..13.6. Hang lights ~y=9 (under the vault, above head). A
            // grid across the long X span x 2 rows in Z so the whole slab lights.
            {
                const float fY = floor2Y + 6.0f;       // ~y=9, under the vault
                const float xs[] = { 36.0f, 52.0f, 68.0f, 84.0f, 100.0f, 112.0f };
                const float zs[] = { r2z0 + 6.0f, (r2z0 + r2z1) * 0.5f, r2z1 - 6.0f };
                for (float zx : zs)
                    for (float xx : xs)
                        addLight(xx, fY, zx, kPL_R, kPL_I);
            }

            // (3) HOLLOW STRUT INTERIOR — the hidden stair climbing UP inside the
            // back-left strut from the door foot (floorY) to the head (atriumFloorY).
            // Hang a few lights stepping up the canted axis (base sBL.b* -> head sBL.t*)
            // a touch above the treads, so the white strut interior + the stair read
            // bright + even from the keypad door up to the landing.
            {
                const int kSteps = 4;
                for (int s = 0; s <= kSteps; ++s) {
                    const float t = (float)s / (float)kSteps;
                    const float lx = sBL.bx + (sBL.tx - sBL.bx) * t;
                    const float lz = sBL.bz + (sBL.tz - sBL.bz) * t;
                    const float ly = strutBaseY + (strutTopY - strutBaseY) * t + 2.6f;
                    addLight(lx, ly, lz, 10.0f, kPL_I * 0.9f);
                }
            }

            // (4) ELEVATOR ATRIUM / BRIDGE — the boarding level at atriumFloorY(14)
            // around the shaft, plus the bridge from the strut head. Hang lights ~3 m
            // above the floor so the white room + the glass cab read clean white.
            {
                const float aY = atriumFloorY + 3.0f;
                addLight(shaftX,         aY, shaftZ,         12.0f, kPL_I);
                addLight(shaftX - 6.0f,  aY, shaftZ - 6.0f,  12.0f, kPL_I);
                addLight(shaftX - 6.0f,  aY, shaftZ + 3.0f,  12.0f, kPL_I);
                // Over the strut head landing + the bridge mid-span.
                addLight(sBL.tx, atriumFloorY + 2.6f, sBL.tz, 11.0f, kPL_I * 0.95f);
                addLight((sBL.tx + shaftX) * 0.5f, atriumFloorY + 2.6f,
                         (sBL.tz + shaftZ) * 0.5f, 11.0f, kPL_I * 0.95f);
                // One brighter light over the shaft mouth so the boarding cab pops.
                addLight(shaftX, atriumFloorY + 2.0f, shaftZ, 10.0f, kPL_I * 1.1f);
            }

            x3::logInfo("--world showroom: INTERIOR point lights = " + std::to_string(plights.size()) +
                        "/64 (cool-white, intensity " + std::to_string(kPL_I) + ", range ~" +
                        std::to_string((int)kPL_R) + " m) covering ground/2nd-floor/strut-stair/atrium");
        }
        // APPLY the chosen DAY/NIGHT state: sky/sun/ambient + bloom + the interior point
        // lights (full at night, x0.3 by day). DAY = bright cool Unity-match (snow-bounce
        // ambient dominates, point lights dimmed, low bloom); NIGHT = dark planet sky +
        // dim moon + full fixtures + the HERO bloom (0.22) on the GLB emissive fixtures.
        applyShowroomTimeOfDay(device.get(), gShowroomDay, &plights);
        x3::logInfo(std::string("--world showroom: time-of-day = ") + (gShowroomDay ? "DAY" : "NIGHT"));

        // E-to-talk dialog state (the headless-tested NpcDialog).
        x3::game::NpcDialog npcDialog;
        float       npcBarkTimer = 0.0f;
        std::string npcBarkText;

        // Frame the sun's shadow box on the building so it casts shadows.
        device->setShadowBounds(cx, (bmn[1] + bmx[1]) * 0.5f, cz, 150.0f);

        // Position the night-sky planets HIGH ABOVE the building (camera-basis fan), reusing
        // the screenshot-showroom placement, but anchored on the player spawn eye + a fixed
        // look direction (toward +Z / Aria) so they hang in the upper frame from inside.
        auto placePlanets = [&](float eyex, float eyey, float eyez, float yaw, float pitch) {
            const float cp = std::cos(pitch), spn = std::sin(pitch);
            const float cyw = std::cos(yaw),   syw = std::sin(yaw);
            const float fwd[3]   = { cp * cyw, spn, cp * syw };
            const float right[3] = { -syw, 0.0f, cyw };
            struct Place { const char* name; float dist; float side; float lift; float radius; };
            const Place places[] = {
                { "Terrestrial",  120.0f, -42.0f,  44.0f, 28.0f },
                { "Gas",          125.0f,  48.0f,  52.0f, 34.0f },
                { "Moon",         105.0f,   8.0f,  40.0f, 14.0f },
                { "Ice",          115.0f, -28.0f,  42.0f, 16.0f },
                { "Lava",         128.0f,  32.0f,  38.0f, 15.0f },
                { "Sun",           98.0f, -16.0f,  48.0f,  9.0f },
            };
            for (NightSkyPlanet& b : planets) {
                const Place* pl = nullptr;
                for (const Place& q : places) if (std::strcmp(q.name, b.name) == 0) { pl = &q; break; }
                if (!pl) continue;
                b.radius = pl->radius;
                b.worldPos[0] = eyex + fwd[0] * pl->dist + right[0] * pl->side;
                b.worldPos[1] = eyey + fwd[1] * pl->dist + pl->lift;
                b.worldPos[2] = eyez + fwd[2] * pl->dist + right[2] * pl->side;
            }
        };

        // Draw the ADDITIVE translucent glass (deck slab + 4 rails + the riding car)
        // each frame, AFTER the opaque scene/env (the BLEND pass is depth-tested over
        // them). The car follows elev.cabCenter(); deck/rails are world-fixed meshes.
        auto drawAdditiveGlass = [&](const x3::rhi::FrameContext& fr) {
            // Deck slab — cool cyan tint, faint self-glow so it reads at night.
            drawGlass(fr, deckMesh, kIdentity, 0.55f, 0.78f, 0.95f, 0.34f, 0.25f);
            drawGlass(fr, railNZ,   kIdentity, 0.60f, 0.85f, 1.00f, 0.45f, 0.40f);
            drawGlass(fr, railPZ,   kIdentity, 0.60f, 0.85f, 1.00f, 0.45f, 0.40f);
            drawGlass(fr, railNX,   kIdentity, 0.60f, 0.85f, 1.00f, 0.45f, 0.40f);
            drawGlass(fr, railPX,   kIdentity, 0.60f, 0.85f, 1.00f, 0.45f, 0.40f);
            // Glass elevator BOX — walls rise from the cab platform top. cabTop =
            // cabCenter().y + platHY; the box center sits carBoxHY above that.
            const x3::phys::Vec3 cc = elev.cabCenter();
            float carModel[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
            carModel[12] = cc.x; carModel[13] = cc.y + platHY + carBoxHY; carModel[14] = cc.z;
            drawGlass(fr, carMesh, carModel, 0.50f, 0.80f, 1.00f, 0.26f, 0.30f);
            // Glass-bottom FLOOR — the see-through plate the rider stands on (centered at
            // the cab plate; collision is the elevator Static body). Ride up + look DOWN.
            float carFloorModel[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
            carFloorModel[12] = cc.x; carFloorModel[13] = cc.y; carFloorModel[14] = cc.z;
            drawGlass(fr, carFloorMesh, carFloorModel, 0.45f, 0.72f, 0.95f, 0.30f, 0.10f);
            // ANALYST GALLERY dark-glass balustrade: the flat dark-tinted glass band
            // (merged ring, world-space verts + identity model) + dark tint/opacity.
            for (const GalGlass& g : galGlass)
                drawGlass(fr, g.mesh, g.model, g.r, g.g, g.b, g.a, g.emis);
        };

        // ===== HEADLESS first-person proof (--screenshot-showroom-fp): one frame from
        // the spawn eye, looking toward Aria (+Z), settle so she skins + bloom registers.
        // --screenshot-showroom-ragdoll reuses the SAME setup but COLLAPSES Aria into a
        // physics ragdoll and steps the world long enough for her to fall into a heap,
        // then captures one frame — proof the ragdoll drives the skin (she's down, not
        // standing). The camera backs up + looks down a touch so the heap fills the frame.
        if (headless) {
            const bool ragShot  = showroomRagdollShot;
            const bool deckShot = showroomDeckShot;
            const bool elevShot = showroomElevShot;
            const bool stairShot= showroomStairShot;
            const bool floor2Shot = showroomFloor2Shot;
            const bool doorShot = showroomDoorShot;
            const bool strutsShot = showroomStrutsShot;
            const bool galleryShot = showroomGalleryShot;
            // FP shot frames Aria standing from the spawn eye (look +Z, level). Ragdoll
            // shot moves the eye CLOSE to her (2.5 m back on the eye->Aria diagonal, lower
            // eye) and AIMS the camera straight at her floor spot so the collapsed heap
            // fills the frame and reads clearly — she's down, not standing.
            // Camera: the ragdoll shot uses the eye placed a short distance from Aria,
            // AIMED directly at her spot so the heap is centered. The FP shot keeps the
            // proven spawn-eye/level look. Aiming at her floor spot means a STANDING Aria
            // fills the frame center; once collapsed the SAME shot shows a low heap there —
            // a direct A/B proof at one framing.
            // Shared helper: brighten the dim moonlit interior for a PROOF capture so the
            // white-panel run reads clearly (headless only — does not touch the live look).
            // NOTE: the interior is now genuinely lit by the INTERIOR POINT LIGHTS
            // set up in the shared block above, so the proofs no longer raise the
            // night sky/sun/ambient to fake brightness — that would also brighten
            // the sky/planets in the shot. brightenProof is kept as a NO-OP so the
            // interior proofs render with the TRUE night values: the interior reads
            // bright from the point lights while the sky stays dark (an honest test
            // of the interior lighting). (Args ignored; kept for call-site shape.)
            auto brightenProof = [&](float /*amb*/, float /*sun*/, float /*expo*/) {
                // intentionally empty — point lights carry the interior now.
            };

            // ===== HIDDEN ANALYST GALLERY proof (--screenshot-showroom-gallery). Captures
            // MULTIPLE frames in one run from the gallery level:
            //   (a) <path>             — the gallery: terminals glowing + analyst figures,
            //                            standing on the ring looking ALONG it.
            //   (b) <path>_down.png    — from the gallery, looking DOWN through the dark
            //                            one-way glass onto the civilian floor/pad.
            //   X3_SHOWROOM_GALLERY_UP=1 swaps (b) for an UP view from the civilian floor at
            //   the dark-glass ceiling band (should read dark — analysts hidden).
            // Settles a few frames so the skinned analysts pose + the terminal holo bakes,
            // then captures each vantage. Self-contained: builds, captures, exits.
            if (galleryShot) {
                static const bool kUpView = (std::getenv("X3_SHOWROOM_GALLERY_UP") != nullptr);
                const float dt = 1.0f / 60.0f;
                float gelapsed = 10.0f;
                // Helper: drive the gallery sub-systems one settle frame + render one frame
                // from the given eye/look, optionally arming a capture to `outPath`.
                // `tickHeavy`: re-pose the skinned analysts + re-bake the holo terminals.
                // Each skinned tick triggers a GPU readback (vkDeviceWaitIdle) so it is
                // EXPENSIVE under 4x SSAA — for a STILL capture we only need to pose them
                // a couple of frames to seat the idle pose + bake the holo textures ONCE,
                // then SKIP the heavy systems and just re-render the (now static) scene.
                auto galTickAndRender = [&](float ex, float ey, float ez, float gyaw, float gpitch,
                                            bool tickHeavy, const char* outPath) {
                    glfwPollEvents();
                    splayer.update(x3::game::PlayerInput{}, dt, *sphys);
                    sphys->step(dt);
                    sscene.update(*sphys);
                    if (tickHeavy) {
                        girl.tick(dt, false, sscene, *sphys, x3::phys::Vec3{ sx, sy, sz });
                        for (auto& tm : galTerms) tm.update(dt);
                        for (auto& an : galAnalysts)
                            an.tick(dt, /*hubReached*/false, sscene, *sphys, an.pos());
                        for (auto& cv : civilians)
                            cv.tick(dt, /*hubReached*/false, sscene, *sphys, cv.pos());
                    }
                    gelapsed += dt;
                    if (!gShowroomDay) placePlanets(ex, ey, ez, gyaw, gpitch);
                    device->setCamera(ex, ey, ez, gyaw, gpitch, 80.0f);
                    if (!gShowroomDay) device->setSkyTime(gelapsed);
                    if (outPath) device->armCapture(outPath);
                    auto frame = device->beginFrame();
                    if (frame.valid) {
                        sscene.render(*device, frame);
                        showroom.draw(*device, frame);
                        girl.draw(*device, frame, sscene);
                        for (auto& an : galAnalysts) an.draw(*device, frame, sscene);
                        for (auto& cv : civilians) cv.draw(*device, frame, sscene);
                        drawAdditiveGlass(frame);   // incl. the dark one-way gallery glass
                        if (!gShowroomDay)
                            drawNightSkyPlanets(device.get(), frame, planetMesh, planets, gelapsed, ringMesh);
                    }
                    device->endFrame(frame);
                };
                // The "_down" / "_up" output paths derived from the base path.
                std::string base = showroomGalleryShotPath;
                std::string stem = base; std::string ext = ".png";
                { const size_t dot = base.find_last_of('.'); if (dot != std::string::npos) { stem = base.substr(0,dot); ext = base.substr(dot); } }
                const std::string downPath = stem + (kUpView ? "_up" : "_down") + ext;
                // ---- Vantage A: ON the gallery ring, looking ALONG/ACROSS it so a run of
                // terminals + the nearest analyst read, with the void + rail in frame.
                // Stand on the ring at one azimuth, look tangentially toward the next slots.
                const float galY  = atriumFloorY;
                // HERO: a raised 3/4 from the ring's outer edge, angled DOWN across the void
                // so the WHOLE gallery reads — the white ring, the OPEN VOID with its dark
                // one-way glass louver rim, the terminals + analyst figures around it, the
                // dome above. Eye out by the wall; aim past the void center onto the rim/glass.
                const float aAng  = 2.3f;
                const float aex = cx + std::cos(aAng) * 15.5f;
                const float aez = cz + std::sin(aAng) * 15.5f;
                const float aey = galY + 4.2f;                    // raised for the down-across angle
                const float atx = cx - std::cos(aAng) * 5.0f;     // aim past the void center
                const float atz = cz - std::sin(aAng) * 5.0f;
                const float aty = galY - 0.8f;                    // tilt down onto the void rim/glass
                {
                    const float vx = atx - aex, vy = aty - aey, vz = atz - aez;
                    const float vlxz = std::sqrt(vx*vx + vz*vz);
                    const float gyaw = std::atan2(vz, vx), gpitch = std::atan2(vy, vlxz);
                    const int kGalSettle = 4;
                    for (int i = 0; i < kGalSettle; ++i)
                        galTickAndRender(aex, aey, aez, gyaw, gpitch, /*tickHeavy*/ i < 2,
                                         (i == kGalSettle - 1) ? base.c_str() : nullptr);
                    const bool w1 = device->captureFrame(base.c_str());
                    x3::logInfo(std::string("--screenshot-showroom-gallery: ") + (w1 ? "wrote " : "FAILED ") + base);
                }
                // ---- Vantage A2 (<path>_term.png): a PLAYER-height close-up of one analyst
                // at a glowing surveillance terminal, facing the void — proving the terminals
                // render their holo readout + the skinned analyst figures read. Stand just
                // inward of an analyst slot, look outward/along at the analyst + terminal.
                if (!kUpView) {
                    const std::string termPath = stem + "_term" + ext;
                    const float slotAng = (6.2831853f * 2) / 11.0f + 0.18f;   // analyst slot 2 / a terminal
                    const float ca = std::cos(slotAng), sa = std::sin(slotAng);
                    const float t2ex = cx + ca * (17.0f - 6.5f), t2ez = cz + sa * (17.0f - 6.5f);
                    const float t2ey = galY + 1.65f;
                    const float t2tx = cx + ca * (17.0f - 1.5f), t2tz = cz + sa * (17.0f - 1.5f);
                    const float t2ty = galY + 1.2f;
                    const float vx = t2tx - t2ex, vy = t2ty - t2ey, vz = t2tz - t2ez;
                    const float vlxz = std::sqrt(vx*vx + vz*vz);
                    const float gyaw = std::atan2(vz, vx), gpitch = std::atan2(vy, vlxz);
                    for (int i = 0; i < 4; ++i)
                        galTickAndRender(t2ex, t2ey, t2ez, gyaw, gpitch, /*tickHeavy*/ i < 2,
                                         (i == 3) ? termPath.c_str() : nullptr);
                    const bool wT = device->captureFrame(termPath.c_str());
                    x3::logInfo(std::string("--screenshot-showroom-gallery: ") + (wT ? "wrote " : "FAILED ") + termPath);
                }
                // ---- Vantage B: DOWN through the dark glass OR UP at the ceiling band.
                float bex, bey, bez, byaw, bpitch;
                if (kUpView) {
                    // Stand on the CIVILIAN floor UNDER the gallery void + look near-straight
                    // UP at the dark-glass band ringing the void mouth ~19 m overhead (it
                    // should read DARK — the analysts behind it hidden). Aim at the void rim
                    // directly above (a hair off-vertical to avoid gimbal).
                    bex = cx; bez = cz; bey = floorY + 1.6f;
                    const float tx2 = cx + 2.5f, tz2 = cz + 1.0f, ty2 = galleryY - 0.3f;
                    const float vx = tx2 - bex, vy = ty2 - bey, vz = tz2 - bez;
                    const float vlxz = std::sqrt(vx*vx + vz*vz);
                    byaw = std::atan2(vz, vx); bpitch = std::atan2(vy, vlxz);   // steep UP
                } else {
                    // Lean out OVER the void at the rail + look almost straight DOWN through
                    // the dark one-way glass onto the civilian floor/pad ~19 m below. The eye
                    // is nudged just INSIDE the void rim (over the opening) so the downward
                    // sightline clears the gallery floor + the GLB dome shells (which sit over
                    // the spire at Z~-100, away from this void center at Z~cz) and reaches the
                    // ground. Aim at a point on the floor a touch toward center so the pad
                    // reads (not dead-vertical, which would show only floor directly under).
                    // A raised 3/4 over the void rail (away from the GLB dome at Z~-100),
                    // tilted DOWN so the OPEN VOID + its dark one-way glass rim fill the
                    // frame and, through them, the civilian floor + the companion ARIA below
                    // read — proving the analysts watch the civilians through the glass. Eye
                    // lifted + pulled back on the -Z bearing; aim into the void at Aria.
                    bex = cx; bez = cz - 14.0f;          // back on the -Z gallery arc
                    bey = galY + 5.0f;                   // lifted for the down-into-void angle
                    const float tx2 = gx, tz2 = gz, ty2 = floorY + 1.0f;  // aim at Aria below, through the void
                    const float vx = tx2 - bex, vy = ty2 - bey, vz = tz2 - bez;
                    const float vlxz = std::sqrt(vx*vx + vz*vz);
                    byaw = std::atan2(vz, vx); bpitch = std::atan2(vy, vlxz);   // down into the void
                }
                {
                    // The analysts/terminals are already posed+baked from vantage A; just
                    // settle a couple frames at the new camera (light ticks) + capture.
                    for (int i = 0; i < 4; ++i)
                        galTickAndRender(bex, bey, bez, byaw, bpitch, /*tickHeavy*/ false,
                                         (i == 3) ? downPath.c_str() : nullptr);
                    const bool w2 = device->captureFrame(downPath.c_str());
                    x3::logInfo(std::string("--screenshot-showroom-gallery: ") + (w2 ? "wrote " : "FAILED ") + downPath);
                }
                sphys->shutdown();
                device->shutdown();
                if (window) glfwDestroyWindow(window);
                glfwTerminate();
                return 0;
            }

            // ===== CIVILIAN-FLOOR proof (--screenshot-showroom-civilians). DAY shot.
            // Captures TWO frames in one run:
            //   (a) <path>          — wide GROUND floor: the blue pad + lounge civilians.
            //   (b) <path>_mezz.png — the 2nd-floor mezzanine deck civilians.
            // PERF: each skinned tick() does a GPU readback (vkDeviceWaitIdle) — costly
            // under 4x SSAA. So we POSE all skinned figures (Aria + analysts + the 8
            // civilians) on only the FIRST 2 settle frames, then SKIP the heavy tick and
            // re-render the now-static scene for the remaining settle + capture frame.
            if (showroomCivShot) {
                const float dt = 1.0f / 60.0f;
                float celapsed = 10.0f;
                auto civTickAndRender = [&](float ex, float ey, float ez, float cyaw, float cpitch,
                                            bool poseFrame, const char* outPath) {
                    glfwPollEvents();
                    splayer.update(x3::game::PlayerInput{}, dt, *sphys);
                    sphys->step(dt);
                    sscene.update(*sphys);
                    if (poseFrame) {
                        // Seat the idle pose ONCE (first frames of each vantage). Each
                        // tick is a GPU readback — kept to the first 2 frames only.
                        girl.tick(dt, false, sscene, *sphys, x3::phys::Vec3{ sx, sy, sz });
                        for (auto& cv : civilians)
                            cv.tick(dt, /*hubReached*/false, sscene, *sphys, cv.pos());
                    }
                    celapsed += dt;
                    device->setCamera(ex, ey, ez, cyaw, cpitch, 78.0f);
                    if (outPath) device->armCapture(outPath);
                    auto frame = device->beginFrame();
                    if (frame.valid) {
                        sscene.render(*device, frame);
                        showroom.draw(*device, frame);
                        girl.draw(*device, frame, sscene);
                        for (auto& cv : civilians) cv.draw(*device, frame, sscene);
                        drawAdditiveGlass(frame);
                    }
                    device->endFrame(frame);
                };
                // Derive the "_mezz" output path from the base path.
                std::string base = showroomCivShotPath;
                std::string ext = ".png";
                std::string stem = base;
                if (stem.size() > 4 && stem.substr(stem.size() - 4) == ".png") {
                    ext = stem.substr(stem.size() - 4); stem = stem.substr(0, stem.size() - 4);
                }
                const std::string mezzPath = stem + "_mezz" + ext;

                // ---- (a) GROUND floor: eye near the +X/+Z lounge, elevated a touch,
                // looking back across the blue pad (toward -X/-Z) so the pad civilians
                // (the chatting pair, the gazer, the trio) all read on the floor.
                {
                    const float ex = cx + 11.0f, ez = cz + 9.5f, ey = floorY + 1.7f;
                    const float tx = cx + 1.0f, ty = floorY + 0.9f, tz = cz - 1.0f;
                    const float vx = tx - ex, vy = ty - ey, vz = tz - ez;
                    const float vlxz = std::sqrt(vx*vx + vz*vz);
                    const float yw = std::atan2(vz, vx), pt = std::atan2(vy, vlxz);
                    for (int i = 0; i < 5; ++i)
                        civTickAndRender(ex, ey, ez, yw, pt, /*poseFrame*/ i < 2,
                                         (i == 4) ? showroomCivShotPath.c_str() : nullptr);
                    const bool w1 = device->captureFrame(showroomCivShotPath.c_str());
                    x3::logInfo(std::string("--screenshot-showroom-civilians: ") +
                                (w1 ? "wrote " : "FAILED ") + showroomCivShotPath);
                }
                // ---- (b) 2nd-floor mezzanine: eye on the Room_01 deck (y=floor2Y),
                // standing close to the +X civilian pair so they read clearly, looking
                // along the deck at the pair + the down-gazing figure beyond.
                {
                    const float ex = 80.0f, ez = -109.0f, ey = floor2Y + 1.7f;
                    const float tx = 94.0f, ty = floor2Y + 0.9f, tz = -113.5f;
                    const float vx = tx - ex, vy = ty - ey, vz = tz - ez;
                    const float vlxz = std::sqrt(vx*vx + vz*vz);
                    const float yw = std::atan2(vz, vx), pt = std::atan2(vy, vlxz);
                    // Figures already posed from (a) — light settle frames, no heavy tick.
                    for (int i = 0; i < 4; ++i)
                        civTickAndRender(ex, ey, ez, yw, pt, /*poseFrame*/ false,
                                         (i == 3) ? mezzPath.c_str() : nullptr);
                    const bool w2 = device->captureFrame(mezzPath.c_str());
                    x3::logInfo(std::string("--screenshot-showroom-civilians: ") +
                                (w2 ? "wrote " : "FAILED ") + mezzPath);
                }
                sphys->shutdown();
                device->shutdown();
                if (window) glfwDestroyWindow(window);
                glfwTerminate();
                return 0;
            }

            float eyeX, eyeZ, eyeY, yaw, pitch;
            if (strutsShot) {
                // EXTERIOR proof: stand well OUTSIDE the back-left corner, elevated, and
                // look back at the building center so the SYMMETRIC radial set of
                // thickened "/" strut legs all read (the four matched canted blades
                // leaning in toward the spire core).
                eyeX = cx - 60.0f; eyeZ = cz - 60.0f; eyeY = floorY + 40.0f;
                const float tx = cx, ty = floorY + 4.0f, tz = cz;
                const float vx = tx - eyeX, vy = ty - eyeY, vz = tz - eyeZ;
                const float vlxz = std::sqrt(vx*vx + vz*vz);
                yaw = std::atan2(vz, vx); pitch = std::atan2(vy, vlxz);
            } else if (deckShot) {
                // PHASE 1 proof: stand ON the glass deck, near the -X rail, look out
                // ACROSS the deck toward +X/+Z and up a touch at the wheeling sky.
                eyeX = spireX - deckHalf + 1.5f; eyeZ = spireZ; eyeY = deckTopY + 1.6f;
                yaw = 0.35f /*toward +X, slightly +Z*/; pitch = 0.12f /*look up at the sky*/;
            } else if (floor2Shot) {
                // STAGE 1 proof: stand ON the 2nd floor (y=floor2Y) just -X of the climb
                // stair top, looking across the open 2nd-floor slab + DOWN the climb ramp
                // (toward +X/-Z) so the rising ramp, the solid 2nd-floor slab, and the
                // building + night sky all read — proving the player climbed up onto solid
                // 2nd-floor collision (NOT boxed in, NOT underground).
                eyeX = climbCX - 9.0f; eyeZ = stairLowZ + climbRun + 1.0f; eyeY = floor2Y + 1.7f;
                const float tx = climbCX + 3.0f, ty = floorY + 2.0f, tz = stairLowZ + 4.0f;
                const float vx = tx - eyeX, vy = ty - eyeY, vz = tz - eyeZ;
                const float vlxz = std::sqrt(vx*vx + vz*vz);
                yaw = std::atan2(vz, vx); pitch = std::atan2(vy, vlxz);
                brightenProof(0.55f, 1.15f, 1.0f);
            } else if (doorShot) {
                // STAGE 2 proof: stand ON the 2nd floor (interior side, +Z of the door) a
                // few metres back, looking -Z straight at the hidden wall door in the back
                // wall. By default the door is OPEN (slid aside, revealing the dark passage
                // behind); X3_SHOWROOM_DOORCLOSED=1 keeps it CLOSED so the panel reads
                // flush/concealed in the strut face.
                // Stand OUTSIDE the back-left strut at civilian level (floorY), a few
                // metres radially-OUT from the door, looking back IN at the hidden door
                // set into the strut's canted "/" face. OPEN by default reveals the dark
                // stair interior; X3_SHOWROOM_DOORCLOSED=1 keeps it CLOSED so the panel
                // reads flush/concealed in the white strut face.
                eyeX = doorFaceX + hsRox * 4.0f; eyeZ = doorFaceZ + hsRoz * 4.0f;
                eyeY = doorFloorY + doorHalfH;   // level with the door center, head-on
                const float tx = doorFaceX, ty = doorFloorY + doorHalfH, tz = doorFaceZ;
                const float vx = tx - eyeX, vy = ty - eyeY, vz = tz - eyeZ;
                const float vlxz = std::sqrt(vx*vx + vz*vz);
                yaw = std::atan2(vz, vx); pitch = std::atan2(vy, vlxz);
                brightenProof(0.58f, 1.2f, 1.0f);
            } else if (elevShot) {
                // ELEVATOR-LEVEL proof: stand on the atrium floor (y=atriumFloorY=14)
                // beside the parked glass cab, looking at the cab + up the shaft toward the
                // deck — proving the strut stair lands at the boarding level where the lift
                // now BOARDS.
                eyeY = atriumFloorY + 1.7f;
                eyeX = shaftX - 5.5f; eyeZ = shaftZ - 6.0f;
                const float tx = shaftX, ty = atriumFloorY + 2.5f, tz = shaftZ;
                const float vx = tx - eyeX, vy = ty - eyeY, vz = tz - eyeZ;
                const float vlxz = std::sqrt(vx*vx + vz*vz);
                yaw = std::atan2(vz, vx); pitch = std::atan2(vy, vlxz);
                brightenProof(0.52f, 1.1f, 0.98f);
            } else if (stairShot) {
                // STRUT-STAIR proof: stand just inside the strut door at the foot of the
                // hidden stair (at the door face, civilian floor) looking UP the canted
                // axis toward the strut head landing (atriumFloorY) — proving the stair
                // climbs UP INSIDE the strut to the elevator level.
                // Look through the OPEN door (slid aside for this shot) from just outside,
                // head-on + slightly up, framed on the doorway so the ASCENDING STEPS
                // inside the canted blade read climbing up behind the opening.
                eyeX = doorFaceX + hsRox * 3.5f; eyeZ = doorFaceZ + hsRoz * 3.5f;
                eyeY = doorFloorY + 1.3f;
                const float tx = sBL.bx + (sBL.tx - sBL.bx) * 0.22f;
                const float tz = sBL.bz + (sBL.tz - sBL.bz) * 0.22f;
                const float ty = strutBaseY + (strutTopY - strutBaseY) * 0.22f + 0.2f;
                const float vx = tx - eyeX, vy = ty - eyeY, vz = tz - eyeZ;
                const float vlxz = std::sqrt(vx*vx + vz*vz);
                yaw = std::atan2(vz, vx); pitch = std::atan2(vy, vlxz);
                brightenProof(0.55f, 1.15f, 1.0f);
            } else if (ragShot) {
                const float tx = gx, ty = floorY + 0.3f, tz = gz;   // aim at her floor spot
                // A HIGH 3/4 vantage 2.5 m back + 4.0 m up looks STEEPLY DOWN onto the
                // heap, clearing the floor slab's raised near edge (which occludes a flat
                // body from any near-level eye) so the sprawled ragdoll reads clearly
                // against the dark floor (bright proof tint makes it pop).
                float ax = sx - gx, az = sz - gz;                   // Aria -> center (toward the eye)
                const float al = std::sqrt(ax*ax + az*az);
                const float ux = (al > 1e-4f) ? ax/al : 0.0f, uz = (al > 1e-4f) ? az/al : -1.0f;
                const float back = 2.5f;
                eyeX = tx + ux*back; eyeZ = tz + uz*back; eyeY = floorY + 4.0f;
                // Brighten the dim moonlit showroom for the PROOF so the collapsed heap
                // reads clearly against the dark floor (headless capture only — does not
                // touch the interactive --world showroom look). NIGHT-only: in DAY the
                // snow-bounce ambient already reads bright, so leave the DAY state intact.
                if (!gShowroomDay) {
                    device->setAmbient(0.42f, 0.45f, 0.55f);
                    x3::rhi::IRenderDevice::SkyParams sb = sp; sb.sunIntensity = 0.9f; sb.exposure = 0.85f; device->setSkyParams(sb);
                }
                const float vx = tx - eyeX, vy = ty - eyeY, vz = tz - eyeZ;
                const float vlxz = std::sqrt(vx*vx + vz*vz);
                yaw   = std::atan2(vz, vx);            // look at Aria (engine yaw: atan2(dz,dx))
                pitch = std::atan2(vy, vlxz);          // gentle down-tilt onto her
            } else {
                eyeX = sx; eyeZ = sz; eyeY = sy + 1.6f; yaw = 1.5708f /*+Z*/; pitch = -0.05f;
            }
            placePlanets(eyeX, eyeY, eyeZ, yaw, pitch);
            // For the ragdoll proof, tint Aria a bright warm hue so the collapsed body
            // reads clearly against the dark moonlit floor (headless capture only).
            if (ragShot) girl.setTint(1.6f, 0.9f, 0.55f, 1.0f);
            // STRUT-door proof: OPEN the hidden strut-face door (slide the panel aside
            // along the strut's tangential axis + remove its collision) so the interior
            // stair reads. The door/stair shots default OPEN; X3_SHOWROOM_DOORCLOSED=1
            // keeps it concealed flush in the strut face for the door-closed proof.
            static const bool kDoorClosed = (std::getenv("X3_SHOWROOM_DOORCLOSED") != nullptr);
            const bool openDoorForProof = (stairShot || elevShot || (doorShot && !kDoorClosed));
            if (openDoorForProof) {
                hatchOpen = true; hatchSlide = 1.0f;
                sphys->removeBody(hatchLidBody);
                if (hatchIdx < sscene.size()) {
                    x3::game::Entity& he = sscene.get(hatchIdx);
                    // Mesh is authored in WORLD space -> transform translation is a pure
                    // tangential DELTA (0 = closed). Slide one panel-width aside.
                    he.transform[12] = hatchSlideX * hatchHalf * 2.0f;   // slid fully aside (tangential)
                    he.transform[14] = hatchSlideZ * hatchHalf * 2.0f;
                }
            }
            // Ragdoll shot needs ~45 physics steps to fall + settle; FP shot just settles.
            const int kSettle = ragShot ? 50 : (ddgiForce ? 120 : 24);   // --ddgi: probe-field convergence
            const float dt = 1.0f / 60.0f;
            float elapsed = 10.0f;   // non-zero so the starfield/clouds read animated
            const std::string outPath =
                strutsShot ? showroomStrutsShotPath :
                deckShot   ? showroomDeckShotPath   :
                floor2Shot ? showroomFloor2ShotPath :
                doorShot   ? showroomDoorShotPath   :
                elevShot   ? showroomElevShotPath   :
                stairShot  ? showroomStairShotPath  :
                ragShot    ? showroomRagdollShotPath : showroomFpShotPath;
            for (int i = 0; i < kSettle; ++i) {
                glfwPollEvents();
                splayer.update(x3::game::PlayerInput{}, dt, *sphys);
                // STAGE 3 atrium proof: keep the cab parked at its LOWER stop (the atrium
                // boarding level) so the cab + atrium both read; just sync its transform.
                if (elevShot) {
                    elev.update(dt, sscene, *sphys);
                }
                // Collapse Aria after a few settle frames (so her CURRENT idle pose seeds
                // the ragdoll), THEN keep stepping so the bodies fall and her skin flops.
                // Control image (same framing, STANDING): set X3_RAGDOLL_NOCOLLAPSE=1 to
                // skip the collapse so the A/B comparison is at one identical camera.
                static const bool kNoCollapse = (std::getenv("X3_RAGDOLL_NOCOLLAPSE") != nullptr);
                if (ragShot && !kNoCollapse && i == 4) {
                    girl.ragdoll(sscene, *sphys);
                    x3::logInfo("--screenshot-showroom-ragdoll: Aria collapsed (ragdolled=" +
                                std::string(girl.ragdolled() ? "1" : "0") + ")");
                }
                sphys->step(dt);
                sscene.update(*sphys);
                girl.tick(dt, /*hubReached*/false, sscene, *sphys, x3::phys::Vec3{ sx, sy, sz });
                // Civilians: pose-then-static (each tick is a GPU readback — costly under
                // 4x SSAA). Seat their idle pose on the first 2 frames only, then render
                // them static for the rest of the settle + capture.
                if (i < 2)
                    for (auto& cv : civilians)
                        cv.tick(dt, /*hubReached*/false, sscene, *sphys, cv.pos());
                elapsed += dt;
                device->setCamera(eyeX, eyeY, eyeZ, yaw, pitch, ragShot ? 58.0f : 72.0f);
                if (!gShowroomDay) device->setSkyTime(elapsed);   // wheeling sky = NIGHT only
                if (i == kSettle - 1) device->armCapture(outPath.c_str());
                auto frame = device->beginFrame();
                if (frame.valid) {
                    sscene.render(*device, frame);
                    showroom.draw(*device, frame);
                    girl.draw(*device, frame, sscene);
                    for (auto& cv : civilians) cv.draw(*device, frame, sscene);
                    drawAdditiveGlass(frame);   // deck slab + rails + glass car (BLEND)
                    // Planets are a NIGHT feature — never drawn in DAY (the starfield in
                    // sky.frag auto-hides on the bright DAY sky).
                    if (!gShowroomDay)
                        drawNightSkyPlanets(device.get(), frame, planetMesh, planets, elapsed, ringMesh);
                }
                device->endFrame(frame);
            }
            const bool wrote = device->captureFrame(outPath.c_str());
            const char* tag =
                strutsShot ? "--screenshot-showroom-struts"   :
                deckShot   ? "--screenshot-showroom-deck"     :
                floor2Shot ? "--screenshot-showroom-floor2"   :
                doorShot   ? "--screenshot-showroom-door"     :
                elevShot   ? "--screenshot-showroom-elevator" :
                stairShot  ? "--screenshot-showroom-stair"    :
                ragShot    ? "--screenshot-showroom-ragdoll"  : "--screenshot-showroom-fp";
            if (wrote) x3::logInfo(std::string(tag) + ": wrote " + outPath);
            else       x3::logError(std::string(tag) + ": capture FAILED");
            sphys->shutdown();
            device->shutdown();
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return wrote ? 0 : 1;
        }

        // ===== Walkable windowed path: full first-person controller + E-to-talk. =====
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        double lastMX, lastMY; glfwGetCursorPos(window, &lastMX, &lastMY);
        double prevTime = glfwGetTime();
        bool prevSpaceS = false, prevES = false, prevKS = false, prevFS = false, prevTS = false;
        float elapsed = 0.0f;
        // ---- HIDDEN-HATCH keypad host state. Mirrors the Level-1 §6.4 keypad gate
        // (main.cpp ~line 6921): a local KeypadEntry buffer + per-key rising-edge
        // trackers. When the player is near the still-closed hatch and presses E,
        // hatchCodeMode opens: digit keys 0-9 append, Backspace deletes, Enter submits
        // (== HATCH_CODE -> run the existing open logic; else flash DENIED + clear),
        // Esc cancels. Same KeypadEntry state machine exercised by --test-doorcode. ----
        bool                  hatchCodeMode = false;
        x3::game::KeypadEntry hatchKeypad;
        bool hkDigitPrev[10] = {};
        bool hkEnterPrev = false, hkBackPrev = false, hkEscPrev = false;
        float hatchDeniedTimer = 0.0f;   // >0 while the "DENIED" flash is shown
        x3::logInfo("--world showroom: walk the showroom — WASD, mouse look, Space jump, "
                    "LeftShift sprint, E talk to Aria / open the strut-door keypad, F ride elevator, "
                    "T toggle DAY/NIGHT, K ragdoll-collapse Aria, Esc to quit");
        x3::logInfo(std::string("--world showroom: starting in ") + (gShowroomDay ? "DAY" : "NIGHT") +
                    " (press T to toggle; instant switch)");
        x3::logInfo("--world showroom: walk to the BACK-LEFT STRUT LEG, find the HIDDEN DOOR in its canted "
                    "face at floor level — press E to open the KEYPAD, type the code + Enter to slide the panel "
                    "aside, then climb the STAIR UP INSIDE the strut to the ELEVATOR LEVEL, press F to ride the "
                    "glass elevator to the deck.");

        int lastWs = (int)W, lastHs = (int)H;
        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();
            // Esc (edge-detected): while the hatch keypad is up, the FIRST Esc cancels
            // code-entry (mirrors the §6.4 gate, where Esc backs out of codeMode);
            // otherwise Esc quits the walkthrough as before.
            bool escNow = glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS;
            if (escNow && !hkEscPrev) {
                if (hatchCodeMode) { hatchCodeMode = false; hatchKeypad.clear(); }
                else { hkEscPrev = escNow; break; }
            }
            hkEscPrev = escNow;

            double now = glfwGetTime();
            float dt = (float)(now - prevTime); prevTime = now;
            if (dt > 0.1f) dt = 0.1f;
            elapsed += dt;

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

            // ANALYST GALLERY: idle the terminals (holo shimmer) + the analyst figures
            // (breathe in place; Captive + hubReached=false => no timer, no follow AI).
            for (auto& tm : galTerms) tm.update(dt);
            for (auto& an : galAnalysts)
                an.tick(dt, /*hubReached*/false, sscene, *sphys, an.pos());
            // CIVILIANS: idle in place (Captive + hubReached=false => no timer, no follow AI).
            for (auto& cv : civilians)
                cv.tick(dt, /*hubReached*/false, sscene, *sphys, cv.pos());

            // ---- GLASS ELEVATOR: advance the cab + CARRY the rider (reuse elevator.cpp).
            // update() returns the cab's per-frame vertical delta; if the player is on
            // the cab top (playerRiding), add it to the player's feet so they ride up/down.
            {
                const float elevDy = elev.update(dt, sscene, *sphys);
                if (elevDy != 0.0f && elev.playerRiding(splayer.feet())) {
                    x3::phys::Vec3 f = splayer.feet();
                    f.y += elevDy;
                    splayer.setFeetPosition(*sphys, f);
                }
            }

            // T rising-edge: flip DAY<->NIGHT and re-apply the whole lighting STATE
            // (sky/sun/ambient/bloom + interior point lights scaled). Instant switch
            // (v1 — no cross-fade). The planet draw + setSkyTime below are gated to NIGHT.
            bool tNow = kd(GLFW_KEY_T);
            if (tNow && !prevTS) {
                gShowroomDay = !gShowroomDay;
                applyShowroomTimeOfDay(device.get(), gShowroomDay, &plights);
                x3::logInfo(std::string("--world showroom: T pressed — time-of-day = ") +
                            (gShowroomDay ? "DAY (bright cool snow-bounce)" : "NIGHT (planets + dim moon + fixtures)"));
            }
            prevTS = tNow;

            // K rising-edge: collapse Aria into a physics ragdoll (debug/test hook).
            // The shared physics world is stepped above, so once ragdolled her tick()
            // drives the skin from the falling bodies. Idempotent — repeat K is a no-op.
            bool kNow = kd(GLFW_KEY_K);
            if (kNow && !prevKS) {
                girl.ragdoll(sscene, *sphys);
                x3::logInfo("--world showroom: K pressed — Aria ragdoll-collapse");
            }
            prevKS = kNow;

            // ---- HIDDEN 2ND-FLOOR WALL DOOR (KEYPAD-gated) + ELEVATOR call.
            //   * Near the concealed wall panel (still closed) -> E opens the KEYPAD;
            //     digits + Enter submit. The CORRECT code (2742) runs openHatch()
            //     (slides the panel aside + removes its collision so you walk through).
            //   * On/at the elevator cab -> F calls it to the next stop (atrium<->deck).
            // The keypad reuses the SAME KeypadEntry state machine + edge-handling shape
            // as the Level-1 §6.4 door-code gate (main.cpp ~line 6921).
            const x3::phys::Vec3 pf = splayer.feet();
            // Horizontal radial proximity to the strut-face door point (canted door, not
            // axis-aligned) + standing at the door's floor level, while it's still closed.
            const float ndx = pf.x - hatchX, ndz = pf.z - hatchZ;
            const bool nearHatch = (ndx*ndx + ndz*ndz <= (hatchHalf + 2.5f)*(hatchHalf + 2.5f)) &&
                                   (std::fabs(pf.y - doorFloorY) <= 2.6f) && !hatchOpen;
            const bool atElevator = elev.playerRiding(pf);
            // The existing open logic, factored so the keypad-submit path (and the
            // headless smoke) can run it. Removes the panel collision ONCE; idempotent
            // if already open. (Passage/turn/stair/atrium/elevator wiring untouched.)
            auto openHatch = [&](const char* via) {
                if (hatchOpen) return;
                hatchOpen = true;
                sphys->removeBody(hatchLidBody);      // open the doorway
                x3::logInfo(std::string("--world showroom: hidden wall door OPENED (") + via +
                            ") — walk in, turn, take the stair up to the elevator atrium");
            };

            // F: elevator call only (the door does not open on F).
            bool fNow = kd(GLFW_KEY_F);
            if (fNow && !prevFS) {
                if (atElevator) {
                    elev.callNext();                       // atrium <-> deck
                    x3::logInfo("--world showroom: elevator called (F) -> stop " +
                                std::to_string(elev.targetStop()));
                }
            }
            prevFS = fNow;

            // E near the still-closed hatch (and not already entering): open the keypad.
            // Mirrors §6.4 "near a locked coded door + E -> codeMode = true; keypad.clear()".
            bool eHatchNow = kd(GLFW_KEY_E);
            if (eHatchNow && !prevES && nearHatch && !hatchCodeMode) {
                hatchCodeMode = true; hatchKeypad.clear(); hatchDeniedTimer = 0.0f;
                x3::logInfo("--world showroom: hatch keypad — type the code, Enter to submit, Esc to cancel");
            }

            // Hatch keypad edge-handling while active: digits 0-9 append, Backspace
            // deletes, Enter submits. (Esc-cancel is handled in the Esc block above.)
            // Same per-key rising-edge shape as the §6.4 codeMode block.
            if (hatchCodeMode) {
                for (int dgt = 0; dgt < 10; ++dgt) {
                    bool dn = kd(GLFW_KEY_0 + dgt) || kd(GLFW_KEY_KP_0 + dgt);
                    if (dn && !hkDigitPrev[dgt]) hatchKeypad.pushDigit(dgt);
                    hkDigitPrev[dgt] = dn;
                }
                bool backNow = kd(GLFW_KEY_BACKSPACE);
                if (backNow && !hkBackPrev) hatchKeypad.backspace();
                hkBackPrev = backNow;
                bool enterNow = kd(GLFW_KEY_ENTER) || kd(GLFW_KEY_KP_ENTER);
                if (enterNow && !hkEnterPrev) {
                    if (hatchKeypad.value() == HATCH_CODE) {
                        x3::logInfo("--world showroom: hatch keypad ACCEPTED — opening");
                        hatchCodeMode = false; hatchKeypad.clear();
                        openHatch("keypad");
                    } else {
                        x3::logInfo("--world showroom: hatch keypad DENIED");
                        hatchDeniedTimer = 1.5f;          // flash "DENIED"
                        hatchKeypad.clear();              // clear the buffer, stay in entry
                    }
                }
                hkEnterPrev = enterNow;
            }
            if (hatchDeniedTimer > 0.0f) hatchDeniedTimer -= dt;
            // Animate the strut-face panel sliding aside (along the strut tangential
            // axis) once opened (cosmetic; collision is already removed). Slides ~1
            // panel-width over ~0.5 s.
            if (hatchOpen && hatchSlide < 1.0f) {
                hatchSlide = std::min(1.0f, hatchSlide + dt * 2.0f);
                if (hatchIdx < sscene.size()) {
                    x3::game::Entity& he = sscene.get(hatchIdx);
                    // Pure tangential DELTA (mesh authored in world; 0 = closed).
                    he.transform[12] = hatchSlide * hatchSlideX * hatchHalf * 2.0f;
                    he.transform[14] = hatchSlide * hatchSlideZ * hatchHalf * 2.0f;
                }
            }
            // Pulse the door panel's glow when the player is near + it's still closed
            // (a subtle "interactable" tell, per Tim's concealed-entrance vision).
            if (hatchIdx < sscene.size()) {
                x3::game::Entity& he = sscene.get(hatchIdx);
                he.emissive[3] = (nearHatch ? 0.8f : 0.0f);
            }

            float camX, camY, camZ, camYaw, camPitch;
            splayer.camera(camX, camY, camZ, camYaw, camPitch);
            const x3::phys::Vec3 eye{ camX, camY, camZ };
            girl.tick(dt, /*hubReached*/false, sscene, *sphys, eye);

            // E rising-edge: start/advance the Aria exchange; completing it rescues her.
            bool eNow = kd(GLFW_KEY_E);
            std::string talkWho; x3::phys::Vec3 talkPos{};
            const bool talkInRange = girl.captive() && [&]{
                const float dx = eye.x - girl.pos().x, dz = eye.z - girl.pos().z;
                return dx*dx + dz*dz <= x3::game::kTalkReach * x3::game::kTalkReach;
            }();
            if (talkInRange) { talkWho = girl.name(); talkPos = girl.pos(); }
            if (eNow && !prevES && (npcDialog.active() || talkInRange)) {
                const std::string barkName = talkWho.empty() ? npcDialog.partner() : talkWho;
                const bool rescued = npcDialog.interact(
                    talkInRange, talkWho, talkPos, [&]{ return girl.tryRescue(eye); });
                if (rescued) {
                    npcBarkText  = x3::game::companionBark(barkName);
                    npcBarkTimer = 4.0f;
                    x3::logInfo("--world showroom: Aria rescued — now a companion");
                }
            }
            prevES = eNow;
            // Keep the box anchored / cancel if she drifts out of range; age the bark.
            if (npcDialog.active()) {
                if (talkInRange) npcDialog.setAnchor(girl.pos());
                else             npcDialog.cancel();
            }
            if (npcBarkTimer > 0.0f) npcBarkTimer -= dt;

            int cw, ch; glfwGetFramebufferSize(window, &cw, &ch);
            if (cw != lastWs || ch != lastHs) { lastWs = cw; lastHs = ch; if (cw>0&&ch>0) device->onResize((uint32_t)cw,(uint32_t)ch); }

            if (!gShowroomDay) placePlanets(camX, camY, camZ, camYaw, camPitch);
            device->setCamera(camX, camY, camZ, camYaw, camPitch, 72.0f);
            if (!gShowroomDay) device->setSkyTime(elapsed);   // wheeling sky = NIGHT only
            auto frame = device->beginFrame();
            if (frame.valid) {
                sscene.render(*device, frame);
                showroom.draw(*device, frame);
                girl.draw(*device, frame, sscene);
                for (auto& an : galAnalysts) an.draw(*device, frame, sscene);   // gallery analysts
                for (auto& cv : civilians) cv.draw(*device, frame, sscene);     // civilian crowd
                drawAdditiveGlass(frame);   // glass deck + rails + riding car + gallery dark glass (BLEND)
                // Planets are a NIGHT feature — never drawn in DAY (the bright sky carries it).
                if (!gShowroomDay)
                    drawNightSkyPlanets(device.get(), frame, planetMesh, planets, elapsed, ringMesh);

                // ---- HUD: "[E] Talk" prompt over Aria, or the dialog box while talking.
                uint32_t hudW = 0, hudH = 0; device->hudSize(hudW, hudH);
                // Center proximity prompt: "[E] Keypad" at the still-closed hatch (now
                // code-gated), or "[F] Ride elevator" at the cab. Suppressed while the
                // hatch keypad is up (the entry prompt below owns the screen then).
                {
                    const char* fp = (nearHatch && !hatchCodeMode) ? "[E] Keypad"
                                   : atElevator                    ? "[F] Ride elevator" : nullptr;
                    if (fp) {
                        const float fw = device->textAdvance(x3::rhi::FontRole::Menu, fp, 22.0f);
                        const float fx = ((hudW > 0) ? hudW * 0.5f : 640.0f) - fw * 0.5f;
                        const float fy = (hudH > 0) ? hudH * 0.72f : 480.0f;
                        const float fsh[4] = { 0.0f, 0.0f, 0.0f, 0.75f };
                        const float fcl[4] = { 0.62f, 0.92f, 1.0f, 1.0f };
                        device->drawHudTextF(frame, x3::rhi::FontRole::Menu, fp, fx + 1.5f, fy + 1.5f, 22.0f, fsh);
                        device->drawHudTextF(frame, x3::rhi::FontRole::Menu, fp, fx, fy, 22.0f, fcl);
                    }
                }
                // Hatch KEYPAD entry prompt (centered) while code-entry is active —
                // mirrors the §6.4 door-code HUD (KeypadEntry::prompt drives the digits).
                // A "DENIED" flash overrides the buffer line briefly on a wrong code.
                if (hatchCodeMode) {
                    const std::string kp = (hatchDeniedTimer > 0.0f)
                        ? std::string("DOOR LOCKED   DENIED")
                        : (std::string("DOOR LOCKED   ENTER CODE: ") + hatchKeypad.buf + "_");
                    const float kw = device->textAdvance(x3::rhi::FontRole::Menu, kp.c_str(), 26.0f);
                    const float kx = ((hudW > 0) ? hudW * 0.5f : 640.0f) - kw * 0.5f;
                    const float ky = (hudH > 0) ? hudH * 0.5f - 30.0f : 330.0f;
                    const float ksh[4] = { 0.0f, 0.0f, 0.0f, 0.80f };
                    const bool denied = (hatchDeniedTimer > 0.0f);
                    const float kcl[4] = { 1.0f,
                                           denied ? 0.30f : 0.82f,
                                           denied ? 0.26f : 0.18f,
                                           1.0f };                        // red DENIED / amber entry
                    device->drawHudTextF(frame, x3::rhi::FontRole::Menu, kp.c_str(), kx + 1.5f, ky + 1.5f, 26.0f, ksh);
                    device->drawHudTextF(frame, x3::rhi::FontRole::Menu, kp.c_str(), kx, ky, 26.0f, kcl);
                }
                if (npcDialog.active()) {
                    const auto& ln = npcDialog.currentLine();
                    const std::string speaker = ln.speaker.empty() ? npcDialog.partner() : ln.speaker;
                    const float ccx = (hudW > 0) ? hudW * 0.5f : 640.0f;
                    const float boxW = (hudW > 0) ? hudW * 0.66f : 840.0f;
                    const float boxH = 118.0f;
                    const float boxX = ccx - boxW * 0.5f;
                    const float boxY = (hudH > 0) ? hudH - 190.0f : 540.0f;
                    const float panel[4]  = { 0.05f, 0.07f, 0.12f, 0.82f };
                    const float border[4] = { 0.40f, 0.78f, 1.0f, 0.85f };
                    device->drawHudQuad(frame, boxX - 3.0f, boxY - 3.0f, boxW + 6.0f, boxH + 6.0f, border);
                    device->drawHudQuad(frame, boxX, boxY, boxW, boxH, panel);
                    const bool isYou = (speaker == "YOU");
                    const float herCol[4] = { 1.0f, 0.62f, 0.78f, 1.0f };
                    const float youCol[4] = { 0.66f, 0.92f, 1.0f, 1.0f };
                    const float nshadow[4] = { 0.0f, 0.0f, 0.0f, 0.75f };
                    device->drawHudTextF(frame, x3::rhi::FontRole::Menu, (speaker + ":").c_str(),
                                         boxX + 25.5f, boxY + 19.5f, 26.0f, nshadow);
                    device->drawHudTextF(frame, x3::rhi::FontRole::Menu, (speaker + ":").c_str(),
                                         boxX + 24.0f, boxY + 18.0f, 26.0f, isYou ? youCol : herCol);
                    const float lineCol[4] = { 0.96f, 0.97f, 1.0f, 1.0f };
                    const float lshadow[4] = { 0.0f, 0.0f, 0.0f, 0.8f };
                    device->drawHudTextF(frame, x3::rhi::FontRole::Menu, ln.text.c_str(),
                                         boxX + 25.5f, boxY + 59.5f, 30.0f, lshadow);
                    device->drawHudTextF(frame, x3::rhi::FontRole::Menu, ln.text.c_str(),
                                         boxX + 24.0f, boxY + 58.0f, 30.0f, lineCol);
                    const char* hint = (npcDialog.lineIndex() + 1 >= npcDialog.lineCount())
                                       ? "[E] Free her" : "[E] Continue";
                    const float hw = device->textAdvance(x3::rhi::FontRole::Menu, hint, 18.0f);
                    const float hintCol[4] = { 0.75f, 0.85f, 0.95f, 0.85f };
                    device->drawHudTextF(frame, x3::rhi::FontRole::Menu, hint,
                                         boxX + boxW - hw - 22.0f, boxY + boxH - 28.0f, 18.0f, hintCol);
                } else if (talkInRange) {
                    const x3::phys::Vec3 cp = girl.pos();
                    float ssx = 0.0f, ssy = 0.0f;
                    if (device->worldToScreen(cp.x, cp.y + 1.85f, cp.z, ssx, ssy)) {
                        const float shadow[4] = { 0.0f, 0.0f, 0.0f, 0.70f };
                        const float col[4]    = { 1.0f, 0.72f, 0.84f, 1.0f };
                        device->drawHudText(frame, "[E] Talk", ssx - 40.0f + 1.5f, ssy + 1.5f, 18.0f, shadow);
                        device->drawHudText(frame, "[E] Talk", ssx - 40.0f, ssy, 18.0f, col);
                    }
                }
                if (npcBarkTimer > 0.0f && !npcBarkText.empty()) {
                    float a = npcBarkTimer; if (a > 1.0f) a = 1.0f;
                    const float bw = device->textAdvance(x3::rhi::FontRole::Menu, npcBarkText.c_str(), 22.0f);
                    const float bx = ((hudW > 0) ? hudW * 0.5f : 640.0f) - bw * 0.5f;
                    const float by = (hudH > 0) ? hudH * 0.62f : 420.0f;
                    const float bshadow[4] = { 0.0f, 0.0f, 0.0f, 0.7f * a };
                    const float bcol[4]    = { 1.0f, 0.72f, 0.84f, a };
                    device->drawHudTextF(frame, x3::rhi::FontRole::Menu, npcBarkText.c_str(), bx + 1.5f, by + 1.5f, 22.0f, bshadow);
                    device->drawHudTextF(frame, x3::rhi::FontRole::Menu, npcBarkText.c_str(), bx, by, 22.0f, bcol);
                }
            }
            device->endFrame(frame);
        }

        sphys->shutdown();
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
        if (window && introCellWorld) {
            if (!x3::intro::runIntro(*device, window)) {
                // Window was closed during the intro — exit cleanly (mirrors a window-close quit).
                physics->shutdown();
                device->shutdown();
                if (window) glfwDestroyWindow(window);
                glfwTerminate();
                return 0;
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
            x3::game::CanonBuildOpts copts; copts.doors = &canonDoors; copts.lockSecuredRooms = true;
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

    // World geometry + canon room spawns are built — push the heavy build steps onto
    // the loading bar and show a frame so the human sees the bar jump.
    loading.step(x3::game::LoadStep::WorldGeometry, "BUILDING WORLD");
    loading.step(x3::game::LoadStep::Spawns, "SPAWNING ACTORS");
    loadingFrame(kLoadDt);

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

    // --cullpath <n> / --hzb: seed the D15 GPU-cull cvars from the CLI so every
    // headless path (smoketest / screenshots / bench) can run with the GPU cull on.
    if (cullPathArg != INT_MIN) {
        console->set("r_cullpath", std::to_string(cullPathArg));
        x3::logInfo("[cull] r_cullpath seeded from CLI: " + std::to_string(cullPathArg));
    }
    if (hzbArg) { console->set("r_hzb", "1"); x3::logInfo("[cull] r_hzb seeded from CLI: 1"); }

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

                // D15 density demo: with --stress N the capture is the GPU-cull
                // showcase — force the perf/cull stats panel into the still so the
                // tested/drawn/frustum/hzb counters are part of the evidence.
                if (stressCount > 0) hud.drawStats(*device, frame, *console, dt, /*force=*/true);

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
    {
        loading.step(x3::game::LoadStep::Done, "READY");
        // Hold the completed bar briefly so the 100% + final tip read.
        double lprev = glfwGetTime();
        for (int i = 0; i < 8 && !glfwWindowShouldClose(window); ++i) {
            const double now = glfwGetTime();
            const float ldt = (float)(now - lprev); lprev = now;
            loadingFrame(ldt > 0.1f ? 0.1f : ldt);
        }
        // Fade out (clean hand-off). Bounded so a stalled present can't hang the boot.
        loading.beginFadeOut();
        int loadGuard = 0;
        while (!loading.faded() && loadGuard++ < 240 && !glfwWindowShouldClose(window)) {
            const double now = glfwGetTime();
            const float ldt = (float)(now - lprev); lprev = now;
            loadingFrame(ldt > 0.1f ? 0.1f : ldt);
        }
        loading.shutdown(*device);
    }

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
    // The off-elevator Nexus (F4.5 Chorus) is a MultiPodBoss whose rigged pods also
    // spawn skinned death ragdolls — tear those Jolt bodies down too before physics.
    nexus.shutdown();
    if (canonPlay.built()) canonPlay.shutdown();   // --world canonlevel enemy ragdolls
    physics->shutdown();
    device->shutdown();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
