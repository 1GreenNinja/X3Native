// --world tunnel host — THE TERRAIN CORRIDOR, MADE VISIBLE.
//
// Boots the canonical streamed terrain with ONE registered TerrainCorridor
// (app/terrain.h) carving a graded road corridor through a real hillside, lays
// a drivable road ribbon down it, and roofs the reach that has enough cover
// with an arched tunnel shell. Drive it with the physics car; capture the proof
// set headless with --screenshot-tunnel.
//
// See app/tunnel_corridor.h for the technique + the clean-room BL provenance.
#include "world_host_common.h"
#include "host_shell.h"                  // console (~), pause menu (ESC), FPS (F3)
#include "engine/core/IJobSystem.h"
#include "engine/physics/IVehicle.h"
#include "../scene.h"
#include "../terrain.h"
#include "../tunnel_corridor.h"
#include "../tunnel_fitout.h"
#include "../tunnel_rooms.h"
#include "../player.h"

#include <array>
#include <memory>
#include "../road_network.h"
#include "../river_bridge.h"
#include "../vehicle.h"
#include "../mesh_prims.h"
#include "../asset_root.h"
#include "engine/audio/IAudioSystem.h"   // ENGINE NOTE: RPM-driven loop
#include "../weather.h"
#include "../wetness.h"
#include "../storm.h"
#include "../precip_fx.h"
#include "../hud.h"
#include "engine/asset/IModelLoader.h"
#include "engine/asset/IAssetSource.h"
// stb_image: file-local static copy (the cinematic.cpp / descent_slide.cpp
// recipe — the engine's implementation is file-local in ModelLoader.cpp, so each
// app TU that decodes PNGs instantiates its own).
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#if defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning(disable : 4244 4456 4457)
#endif
#include <stb_image.h>
#if defined(_MSC_VER)
#  pragma warning(pop)
#endif

#include <filesystem>
#include <system_error>
#include <cmath>      // std::floor  (pause-overlay layout)
#include <cstdio>     // std::snprintf (HUD readouts)
#include <cstring>    // std::strlen (pause-overlay centering)

namespace x3 { namespace apphost {

// ONE upward ray finds the roof over the camera. Cheap (a single static-layer
// query per frame) and GENERAL -- it knows nothing about tunnels, so it will do
// the same job under a bridge, an overpass or a gas-station canopy the day those
// exist, with no new code. Returns a huge value under open sky.
// How much sky is over this point, 0..1 -- and the answer near a portal is not
// zero.
//
// The first cut of this was BINARY: a roof overhead meant no precipitation, full
// stop. That is wrong in the way that is obvious the moment you stand in a real
// tunnel mouth in weather. Snow does not stop at the portal line; the wind
// drives a wedge of it in, and you get flakes in the air and drift on the road
// for the first eighty feet or so before it dies out. Cutting it dead at the
// threshold reads as a rendering boundary, which is exactly what it was.
//
// So when the up-ray IS blocked, march OUTWARD along the travel axis until it
// stops being blocked. The distance to that opening drives the falloff, which
// gives blown-in snow at both mouths tapering inward, and full darkness deep in
// the middle -- with no knowledge of tunnels anywhere in it. The same code puts
// spray under a bridge deck and rain at the lip of a canopy.
//
// Cost is at most kSteps*2 extra static raycasts on frames where you are under
// cover, and none at all under open sky (the common case exits on the first ray).
// (The old file-static g_tunnelHud char-callback trampoline is gone: HostShell
// owns the GLFW callbacks now, and chains to whatever a host installed first.)

static float skyVisibleAt(x3::phys::IPhysicsWorld& phys, float x, float y, float z,
                          float dirX, float dirZ) {
    auto blocked = [&](float px, float pz) {
        return phys.rayCastStrict(x3::phys::Vec3{ px, y + 0.5f, pz },
                                  x3::phys::Vec3{ 0.0f, 1.0f, 0.0f },
                                  60.0f, x3::phys::Layer::Static).hit;
    };
    if (!blocked(x, z)) return 1.0f;              // open sky: one ray, done

    // BLOW-IN RANGE. 25 m (82 ft) is the distance over which a portal's weather
    // gives up; past it a bore is genuinely still air. Marched in 8 steps, which
    // resolves the mouth to about 10 ft -- finer than the eye reads at speed.
    const float kBlowInM = 25.0f;
    const int   kSteps   = 8;
    float nearest = kBlowInM;
    for (int i = 1; i <= kSteps; ++i) {
        const float d = kBlowInM * (float)i / (float)kSteps;
        if (!blocked(x + dirX * d, z + dirZ * d) ||
            !blocked(x - dirX * d, z - dirZ * d)) { nearest = d; break; }
    }
    // Nearer the opening = more gets in. Eased, and capped below 1 because even
    // standing ON the threshold the roof is taking most of it.
    const float t = 1.0f - (nearest / kBlowInM);
    return 0.85f * (t * t * (3.0f - 2.0f * t));
}

int hostTunnel(HostContext& hc) {
    auto* device = hc.device;
    GLFWwindow* window = hc.window;
    const bool headless = hc.headless;
    const bool screenshot = hc.screenshot;
    const std::string& screenshotPath = hc.screenshotPath;
    const uint32_t W = hc.W, H = hc.H;
    (void)W; (void)H;

    x3::logInfo("--world tunnel: terrain-corridor bore demo");

    // Render-pass A/B (`--set r_ssao 0` etc.) is NOT wired here any more. This
    // host used to call its own applyWorldHostRenderCVars(); fold-0812 landed
    // fix/world-host-cvars, which does the same thing for EVERY route from
    // runRoute() in world_hosts.cpp before the host body runs — with a strict
    // superset of the cvars (50 vs 37, none dropped) plus the run-long override
    // latch and the unapplied-cvar report. A per-host call is exactly the trap
    // that generalization removes, so the local one is gone rather than doubled.
    // ==== STEP 1 — REGISTER THE CORRIDOR, BEFORE ANY HEIGHT CONSUMER =========
    // app/terrain.h's contract: "Register corridors at BOOT, BEFORE the first
    // height query / TerrainStreamer::init()". Everything below (the streamer,
    // the horizon ring, the road grading, the car spawn) reads the field AFTER
    // this line, so they all agree by construction.
    const x3::game::TunnelRoute& route = x3::game::registerTunnelCorridor();

    // THE 15-MILE INNER TOUR. X3_RING=1 lays it in this world so it can be
    // driven; off by default so the tunnel demo is untouched. Registered HERE,
    // beside the corridor above, because app/terrain.h's contract is "register
    // before the first height query" and this is the last moment that is true.
    x3::game::RoadSpec ringSpec;
    bool ringOn = false;
    {
        const char* e = std::getenv("X3_RING");
        ringOn = (e && e[0] == '1');
        if (ringOn) {
            ringSpec = x3::game::makeRingRoad("inner tour", -592.0f, -352.0f, 3842.0f, 396);
            ringSpec.halfWidth = x3::game::kPavedHalfM + 1.0f;
            ringSpec.falloff   = 18.0f;
            ringSpec.maxGrade  = 0.07f;
            const x3::game::RoadBuildResult rr = x3::game::registerRoad(ringSpec);
            if (!rr.ok) { x3::logError("--world tunnel: ring registration FAILED"); ringOn = false; }
        }
    }
    // THE 31-MILE OUTER TOUR (X3_OUTER_RING=1) — the four-range loop with its
    // five bores — and THE RIVER CROSSING (X3_RIVER_ROAD=1) — the valley road
    // over Bridge No.1. Registered in the same boot slot, for the same reason:
    // the corridor registry closes at the first height query below.
    x3::game::OuterRingResult outerRing;
    bool outerOn = false;
    {
        const char* e = std::getenv("X3_OUTER_RING");
        outerOn = (e && e[0] == '1');
        if (outerOn) {
            outerRing = x3::game::registerOuterRing();
            if (!outerRing.road.ok) {
                x3::logError("--world tunnel: outer tour registration FAILED");
                outerOn = false;
            }
        }
    }
    x3::game::RiverRoadResult riverRoad;
    bool riverOn = false;
    {
        const char* e = std::getenv("X3_RIVER_ROAD");
        riverOn = (e && e[0] == '1');
        if (riverOn) {
            riverRoad = x3::game::registerRiverRoad();
            if (!riverRoad.road.ok) {
                x3::logError("--world tunnel: river road registration FAILED");
                riverOn = false;
            }
        }
    }

    // ==== STEP 1.5 — THE ROOMS' AIR RIGHTS ==================================
    // Found by the FIRST interior capture (09_garage_lnss): the corridor CARVE
    // does not stop at the bore wall — its 14 m falloff shoulder climbs from
    // trench depth back to the natural hill across lat 10.1..24.1 m, which is
    // exactly the band the service rooms occupy (latIn 12.1 m). The carved
    // STREAMER surface therefore passes through the room volumes — worst in
    // the GARAGE, whose floor is 13 ft below the roadway, where it crossed the
    // bay as a rock wedge at chest-to-truss height, render AND collision.
    //
    // R1's "109.5 ft of cover" is NOT wrong, and that is the trap: it measures
    // tunnelLidHeightAt(), the RESTORED hillside of the cut-and-cover story.
    // The streamed field renders the CARVED surface under that lid. Two
    // surfaces, one word ("the ground"), and the proof was reading the other
    // one. The lid hides the carved shoulder from OUTSIDE; the rooms live
    // inside it.
    //
    // The fix is the machinery terrain.h already ships for exactly this class
    // of defect: a TerrainPortalHole drops terrain triangles (mesh + collision)
    // whose centroid lies in a prism and whose lowest vertex dips under yTop
    // ("no depth profile fixes that; the MESHER has to skip those triangles").
    // MEASURED, not assumed: the room program is rebuilt here (pure data, same
    // route/seed/tier as every other builder of it), the real field is sampled
    // over each space's footprint, and a hole is registered ONLY where the
    // field actually enters a space. On this route that is the garage + its
    // ramp; the road-level rooms stay under the shoulder and register nothing.
    // Every dropped patch sits beneath the backfill lid mesh (which runs to
    // lat 29.1 m), so nothing opens to the sky. MUST run before STEP 2: holes
    // are read at tile generation.
    {
        x3::game::FitoutConfig fcfg;
        x3::game::TunnelFitout fitout;
        fitout.build(route.boreS0, route.boreS1, fcfg, x3::game::kTunnelFitoutSeed);
        x3::game::TunnelRoomProgram rooms;
        rooms.build(route, fitout, x3::game::TunnelTier::A);
        for (const x3::game::TunnelSpace& sp : rooms.spaces()) {
            const float ceilY = sp.floorY + sp.clearH;
            float worstIn = -1e9f;                    // deepest the field dips into the space
            for (float s = sp.s0; s <= sp.s1 + 0.01f; s += 1.0f)
                for (float lat = sp.latIn; lat <= sp.latOut + 0.01f; lat += 1.0f) {
                    float wx = 0.0f, wz = 0.0f;
                    route.worldAt(s, (float)sp.side * lat, wx, wz);
                    const float h = x3::game::terrainHeightAtWorld(wx, wz);
                    if (h < ceilY + 0.3f)             // at/below the ceiling = inside (or under the floor,
                        worstIn = std::max(worstIn, h - sp.floorY);   // which is fine — negative)
                }
            if (worstIn <= 0.05f) continue;           // field stays under the floor: no hole needed
            x3::game::TerrainPortalHole hole;
            // 3 m margins on every side, and this number was CAPTURED, not
            // chosen: with a 0.8 m margin the first probe shot still had a rock
            // band crossing the bay wall, because the drop test is by triangle
            // CENTROID — a full-LOD quad centred 1 m behind the wall reaches
            // ~1 m past it into the room and survives a snug prism. 3 m clears
            // a full-LOD quad from any side. Everything the wider prism drops
            // is still under the backfill lid mesh (which runs to lat 29.1 m,
            // vs latOut + 3 = 28.2 m here), so nothing opens to the sky.
            const float kM = 3.0f;
            route.worldAt(sp.s0 - kM, (float)sp.side * (sp.latIn + sp.latOut) * 0.5f, hole.x0, hole.z0);
            route.worldAt(sp.s1 + kM, (float)sp.side * (sp.latIn + sp.latOut) * 0.5f, hole.x1, hole.z1);
            hole.halfWidth = (sp.latOut - sp.latIn) * 0.5f + kM;
            hole.yTop      = ceilY + 0.3f;
            const bool ok2 = x3::game::registerTerrainPortalHole(hole);
            char hb[240];
            std::snprintf(hb, sizeof(hb),
                "tunnel rooms: carved ground enters the %s %.1f ft above its floor -> %s "
                "(prism %.0f ft long, half-width %.1f ft, ceiling %.1f ft)",
                x3::game::spaceKindName(sp.kind), worstIn * 3.28084f,
                ok2 ? "terrain hole registered" : "HOLE REGISTRY FULL — left intruding",
                (sp.s1 - sp.s0) * 3.28084f, hole.halfWidth * 3.28084f, sp.clearH * 3.28084f);
            if (ok2) x3::logInfo(hb); else x3::logError(hb);
        }
    }

    std::unique_ptr<x3::phys::IPhysicsWorld> phys(x3::phys::createPhysicsWorld());
    if (!phys->init()) {
        x3::logError("--world tunnel: physics init failed");
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }
    std::unique_ptr<x3::jobs::IJobSystem> jobs(x3::jobs::createJobSystem());
    jobs->init(0);
    x3::game::Scene scene;

    // ==== LIVE WEATHER =======================================================
    // The sky below used to be set ONCE at boot and never touched again, which
    // is why it was always the same bright afternoon. These four objects are the
    // whole chain, and they are wired in this order because each one feeds the
    // next:
    //
    //   Weather  -> what the sky is doing, and the AIR TEMPERATURE
    //   Wetness  -> what that does to the road: soak, ice, and now snow DEPTH
    //   Storm    -> lightning flash + thunder, delayed by its own distance
    //   gauge    -> the thermometer, which reads the temperature back out
    //
    // Off by default so the tunnel/road demos keep the deterministic bright sky
    // they were tuned against. X3_WEATHER=1 turns the weather on; X3_WEATHER=
    // storm|rain|snow|clear|fog forces one and holds it, which is the only sane
    // way to actually look at a specific effect instead of waiting for the
    // scheduler to roll it.
    x3::game::Weather weather;
    x3::game::WetnessModel wetness;
    x3::game::StormSystem storm;
    x3::game::PrecipFx precip;
    bool precipInit = false;
    x3::game::PrecipKind precipKind = x3::game::PrecipKind::None;
    float precipAmt = 0.0f;
    bool weatherOn = false;
    {
        const char* e = std::getenv("X3_WEATHER");
        weatherOn = (e && e[0] && std::strcmp(e, "0") != 0);
        if (weatherOn) {
            weather.setBiome(x3::game::Biome::Temperate);
            storm.reset();
            precip.init(x3::game::PrecipConfig{}); precipInit = true;
            if (e && std::strcmp(e, "storm") == 0)      weather.forceState(x3::game::WeatherState::Storm, true);
            else if (std::strcmp(e, "rain")  == 0)      weather.forceState(x3::game::WeatherState::Rain,  true);
            else if (std::strcmp(e, "fog")   == 0)      weather.forceState(x3::game::WeatherState::Fog,   true);
            else if (std::strcmp(e, "clear") == 0)      weather.forceState(x3::game::WeatherState::Clear, true);
            else if (std::strcmp(e, "snow")  == 0) {
                // Snow is not legal in a temperate biome -- the gate is there on
                // purpose. Asking for snow asks for a snowfield.
                weather.setBiome(x3::game::Biome::Snow);
                weather.forceState(x3::game::WeatherState::Snow, true);
            }
            // PRIME THE GROUND. Snow accumulates at an inch an HOUR, which is the
            // right rate and a useless one to start a session on: arriving in a
            // blizzard on bare grass and waiting forty real minutes for it to go
            // white is not a demo, it is a screensaver. So the integrator is
            // fast-forwarded before the first frame -- the same model, the same
            // maths, just run ahead, exactly as loading a save would.
            //
            // It keeps accumulating live from there, which is the point: you
            // arrive somewhere that HAS weather rather than somewhere weather is
            // about to start, and it still deepens while you drive.
            {
                float primeIn = 0.0f;
                if (const char* pe = std::getenv("X3_SNOW_IN")) primeIn = (float)std::atof(pe);
                else if (weather.sample().snowfall) primeIn = 2.6f;   // a settled fall
                if (primeIn > 0.0f) {
                    const x3::game::WeatherSample& p = weather.sample();
                    // 1 s steps: coarse enough to prime a whole night in a blink,
                    // fine enough that the freeze/thaw hysteresis still resolves.
                    for (int i = 0; i < 60 * 60 * 24 && wetness.snowDepthIn() < primeIn; ++i)
                        wetness.tick(1.0f, p.precipitation, p.tempC, p.snowfall);
                    char pb[128];
                    std::snprintf(pb, sizeof(pb), "weather: primed %.1f in of lying snow",
                                  wetness.snowDepthIn());
                    x3::logInfo(pb);
                }
            }
            x3::logInfo(std::string("weather: ON (") +
                        x3::game::weatherStateName(weather.sample().state) + " in " +
                        x3::game::biomeName(weather.biome()) + ")");
        }
    }

    {   // Bright, high sun: the point of the shot is READING THE GROUND, and a
        // low sun would fill the cutting with shadow and hide the very seams
        // this demo exists to expose.
        x3::rhi::IRenderDevice::SkyParams sp{};
        sp.enabled = true;
        sp.sunDir[0] = 0.35f; sp.sunDir[1] = 0.92f; sp.sunDir[2] = 0.18f;
        sp.sunColor[0] = 1.0f; sp.sunColor[1] = 0.97f; sp.sunColor[2] = 0.92f;
        sp.sunIntensity = 1.0f; sp.haze = 0.35f; sp.exposure = 1.0f;
        // Scattered fair-weather cumulus. 0 would be the old clear sky exactly.
        sp.cloud = 0.42f;
        device->setSkyParams(sp);
    }
    device->setCameraFar(4000.0f);

    // ==== STEP 2 — the streamed terrain ring =================================
    float startPos[3];
    // On the road, out on open ground, far enough back that the whole approach
    // cutting + the portal are ahead of you (and in frame on the approach shot).
    route.posAt(std::max(8.0f, route.boreS0 - 55.0f), startPos);
    if (ringOn && ringSpec.x.size() > 2) {
        // Stand on the ring itself: its first node, lifted to the graded datum.
        startPos[0] = ringSpec.x[0];
        startPos[2] = ringSpec.z[0];
        startPos[1] = x3::game::terrainHeightAtWorld(startPos[0], startPos[2]) + 1.0f;
    }

    x3::game::TerrainStreamer streamer;
    const x3::game::TerrainConfig& cfg = x3::game::worldTerrainConfig();
    streamer.setUploadBudget(96);
    streamer.setMaxInFlight(48);
    streamer.init(scene, *device, *phys, jobs.get(), cfg,
                  startPos[0], startPos[2], /*radius=*/headless ? 14 : 9);

    // Far country so the hill sits in a landscape and not on a void horizon.
    {
        float mid[3]; route.posAt(route.totalLen * 0.5f, mid);
        x3::game::HorizonRingDesc hr{};
        hr.centerX = mid[0]; hr.centerZ = mid[2];
        hr.rInner = 470.0f; hr.rOuter = 9000.0f;
        hr.rings = 96; hr.segments = 128; hr.yBias = -3.0f;
        x3::game::addTerrainHorizonRing(scene, *device, streamer.groundTexture(), hr);
    }

    // ==== STEP 3 — the road, the shell, the portals ==========================
    x3::game::TunnelCorridorWorld tunnel;
    // The streamer's ground texture IS the terrain splat MARKER. Handing it to
    // the tunnel is what lets the BACKFILL LID — the mesh that carries the
    // hillside back over the cut-and-cover bore — shade through the same
    // height/slope splat as the streamed tiles instead of reading as a separate
    // object draped over the hill. Without it the build warns and falls back.
    tunnel.build(scene, *device, *phys, route, streamer.groundTexture());
    // The ribbon: 4 lanes of asphalt plus a 20 ft cement apron each side, laid
    // into the cutting the corridor already graded.
    if (ringOn) x3::game::buildRoadRibbon(ringSpec, scene, *device, *phys);
    // The outer tour's pavement + its five dressed bores. The ribbon rides the
    // graded DATUM (not the carved field) so it stays level across the
    // portal-ramp approaches; gap reaches are skipped — each tunnel lays its
    // own road, shell, portals and lights, through the same machinery as the
    // demo bore. Their lights join the merged per-frame pool automatically.
    std::vector<std::unique_ptr<x3::game::TunnelCorridorWorld>> tourBores;
    if (outerOn) {
        x3::game::buildRoadRibbon(outerRing.spec, scene, *device, *phys,
                                  &outerRing.roadY);
        for (const x3::game::TunnelRoute* r : outerRing.bores) {
            if (!r || !r->boreValid) continue;
            auto w = std::make_unique<x3::game::TunnelCorridorWorld>();
            if (w->build(scene, *device, *phys, *r, streamer.groundTexture()))
                tourBores.push_back(std::move(w));
        }
    }
    if (riverOn) {
        x3::game::buildRoadRibbon(riverRoad.spec, scene, *device, *phys,
                                  &riverRoad.roadY);
        x3::game::buildRiverBridge(riverRoad.plan, scene, *device, *phys);
    }
    device->setPointLights(tunnel.lights().data(), (uint32_t)tunnel.lights().size());

    // ---- THE INTERIOR PROGRAM, decided and COUNTED at boot -----------------
    // This is the whole hook the rooms lane needs from the host: the fitout says
    // where the service doors are, the room program says what is behind them,
    // and both are pure data (--test-tunnelfitout / --test-tunnelrooms prove
    // them headless). Nothing is drawn here yet -- the room/hall/stair MESHES
    // belong in tunnel_corridor.cpp beside the shell's MeshBuf/upload/material
    // machinery, and duplicating that machinery to avoid touching one file
    // would be the worse mistake.
    //
    // It is logged because TUNNEL_INTERIOR_PLAN.md B1 is right that a budget
    // nobody logs is a wish, and because the "built but not wired" failure this
    // codebase keeps hitting starts exactly here: a module that decides
    // correctly and silently.
    // ---- THE FLEET AND THE GARAGE ------------------------------------
    // Eleven vehicles converted; six of them stand in the bay. The list is
    // ordered the way a garage would order it -- the one you are driving first,
    // then the rest -- rather than alphabetically, because the first row of a
    // chooser is the one that gets looked at.
    struct FleetCar { const char* file; const char* name; };
    static const FleetCar kFleet[] = {
        { "Vehicles/E46_New.glb", "E46 SPORT"   },
        { "Vehicles/CTR.glb",     "CTR"         },
        { "Vehicles/M3_E36.glb",  "M3 E36"      },
        { "Vehicles/E30.glb",     "E30"         },
        { "Vehicles/Coupe.glb",   "COUPE"       },
        { "Vehicles/Muscle.glb",  "MUSCLE"      },
        { "Vehicles/Skyline_by_BUMSTRUM.glb", "SKYLINE" },
        { "Vehicles/Pickup.glb",  "PICKUP"      },
        { "Vehicles/Jeep.glb",    "JEEP"        },
        { "Vehicles/Truck.glb",   "TRUCK"       },
        { "Vehicles/F1.glb",      "F1"          },
    };
    constexpr int kFleetCount = (int)(sizeof(kFleet) / sizeof(kFleet[0]));
    int  fleetSel   = 0;        // what is being DRIVEN
    int  garageCursor = 0;      // what the chooser is highlighting
    bool garageOpen = false;

    // The display cars standing in the bay. Loaded once, drawn every frame --
    // these are STATIC props, not vehicles: no physics, no controller. A parked
    // car that is a real vehicle body is eleven Jolt rigs idling for scenery.
    struct ParkedCar {
        std::unique_ptr<x3::asset::IAssetSource> src;
        std::unique_ptr<x3::asset::IModelLoader> loader;
        x3::asset::Model model;
        std::vector<x3::asset::ModelDrawable> draw;
        float world[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    };
    std::vector<ParkedCar> parked;

    // Where the plant rooms ended up, so their hums can start once the audio
    // system exists (STEP 3b below).
    std::vector<std::array<float, 3>> plantHumPos;
    {
        x3::game::FitoutConfig fcfg;
        x3::game::TunnelFitout fitout;
        fitout.build(route.boreS0, route.boreS1, fcfg, x3::game::kTunnelFitoutSeed);
        x3::game::TunnelRoomProgram rooms;
        // The demo ridge is the census's one and only Tier A bore -- the
        // showcase. Every other bore in the world is B or C and gets no rooms.
        rooms.build(route, fitout, x3::game::TunnelTier::A);
        char rb[320];
        std::snprintf(rb, sizeof(rb),
            "tunnel interior: %u service doors, %u opening onto a program "
            "(%u spaces, %u entities of the Tier-A budget of 40); least rock over any "
            "room ceiling %.0f ft",
            (uint32_t)rooms.doors().size(), rooms.programmedDoorCount(),
            (uint32_t)rooms.spaces().size(), rooms.entityCount(),
            rooms.worstRockCoverM() * 3.28084f);
        x3::logInfo(rb);

        // ---- PARK THE FLEET. Six bays, nose-in, two rows of three -- the
        // layout the garage was SIZED for, so the cars land where the painted
        // bays are rather than being scattered and hoping.
        for (const x3::game::TunnelSpace& sp : rooms.spaces()) {
            if (sp.kind != x3::game::SpaceKind::Garage) continue;
            const float gLen = sp.s1 - sp.s0, gDep = sp.latOut - sp.latIn;
            for (uint32_t b = 0; b < x3::game::kTrGarageBays && (int)b < kFleetCount; ++b) {
                const uint32_t row = b / 3, bay = b % 3;
                const float bs  = sp.s0 + (0.6f + (float)bay * 3.0f) * (gLen / 10.5f) + 2.4f;
                const float bl  = sp.latIn + (row == 0 ? 2.1f : gDep - 5.5f);
                float wx = 0.0f, wz = 0.0f;
                route.worldAt(bs, (float)sp.side * bl, wx, wz);
                ParkedCar pc;
                pc.src.reset(x3::asset::createAssetSource());
                if (!pc.src || !pc.src->mountDir(x3::game::convertedGlbRoot(), 0)) continue;
                pc.loader.reset(x3::asset::createModelLoader(device, pc.src.get()));
                // Skip whatever is being DRIVEN -- a garage showing you the car
                // you arrived in is a mirror, not a collection.
                const int which = (int)b + 1;
                pc.model = pc.loader->load(kFleet[which % kFleetCount].file);
                if (!pc.model.ok) continue;
                pc.draw = x3::asset::makeDrawables(pc.model);
                // Nose-in: rows face each other across the aisle.
                const float a = std::atan2(route.dirZ, route.dirX)
                              + (row == 0 ? 1.5707963f : -1.5707963f);
                const float ca = std::cos(a), sa = std::sin(a);
                const float m[16] = { ca,0,-sa,0,  0,1,0,0,  sa,0,ca,0,  wx, sp.floorY, wz, 1 };
                for (int k = 0; k < 16; ++k) pc.world[k] = m[k];
                parked.push_back(std::move(pc));
            }
        }
        if (!parked.empty()) {
            char pb2[96];
            std::snprintf(pb2, sizeof(pb2), "garage: %u vehicle(s) parked in the bay",
                          (uint32_t)parked.size());
            x3::logInfo(pb2);
        }

        // The plant rooms want a hum, but the audio system is not created until
        // further down. Carry their POSITIONS out of here rather than reordering
        // engine startup around an ambience detail.
        for (const x3::game::TunnelSpace& sp : rooms.spaces()) {
            if (sp.kind != x3::game::SpaceKind::PlantRoom) continue;
            const float sMid   = (sp.s0 + sp.s1) * 0.5f;
            const float latMid = (float)sp.side * (sp.latIn + sp.latOut) * 0.5f;
            float wx = 0.0f, wz = 0.0f;
            route.worldAt(sMid, latMid, wx, wz);
            plantHumPos.push_back({ wx, sp.floorY + 1.2f, wz });
        }
    }

    // ---- ON FOOT ---------------------------------------------------------
    // E gets you OUT. The bore now has walkways, lay-bys, service doors and
    // eleven rooms behind them, and until this existed every one of those was
    // scenery you drove past at 90 mph and could never touch. A tunnel you can
    // only ever drive through does not need a walkway.
    //
    // The Player controller already existed, complete with capsule, stances and
    // ground handling -- it had simply never been wired into this host. Same
    // shape as the rest of today: the feature was built and the door was shut.
    // ---- THE CONSOLE, and weather on it -------------------------------
    // X3_WEATHER is an env var, which means changing the sky costs a restart --
    // and the whole point of a weather model with a diurnal clock and an
    // accumulating snowpack is watching it CHANGE. A cvar you can retype mid-
    // drive is the difference between a feature you inspect and one you play
    // with. Backtick opens it.
    // ONE console: the HostShell's. This host used to create its own IConsole +
    // Hud here (and learned the hard way that installing a char callback on a
    // null headless window is an access violation — 9b8ad0e8). The shell already
    // solves both: it only installs callbacks when a window exists, and the
    // wx cvars are registered on it right after attach, down in the interactive
    // section. `console` stays a pointer with the same name so the weather code
    // below reads unchanged; it is null on the headless path, which never
    // touches it (verified: the proof-set block drives `weather` directly).
    x3::con::IConsole* console = nullptr;
    std::string wxApplied = "off";

    // ---- JAKE. The on-foot camera was a first-person eye, which is exactly
    // why you could not see him: you were inside his head. A body nobody can see
    // is a body nobody has, so getting out now pulls the camera back and puts
    // the man on screen.
    std::unique_ptr<x3::asset::IAssetSource> jakeSrc;
    std::unique_ptr<x3::asset::IModelLoader> jakeLoader;
    x3::asset::Model jakeModel;
    std::vector<x3::asset::ModelDrawable> jakeDraw;
    bool jakeTried = false;

    x3::game::Player onFoot;
    bool  driving      = true;
    bool  footSpawned  = false;
    float parkedAt[3]  = { 0, 0, 0 };   // where the car was left, for the re-entry prompt

    // ==== STEP 4 — the car, on the road, outside the entrance ================
    x3::game::DriveDemo car;
    const bool carBuilt = car.build(*device, *phys, startPos[0], startPos[1] + 1.4f, startPos[2]);
    if (carBuilt) {
        // E46_New, not CTR. Tim asked for a seat, a passenger seat, a dash and a
        // steering wheel; CTR is an exterior shell -- 34 nodes, none of them
        // interior. Same pack (Realistic Car Controller V4), same wheel node
        // names (Wheel_FL/FR/RL/RR) and the same misspelled `Buttom` underbody,
        // so the skin mapping is unchanged -- but it carries Seats, Dashboard,
        // SteeringWheel, Interior, GearHandle, Wipers, and a pair of live gauge
        // needles (Needle_KM / Needle_RPM) that a later pass can drive off the
        // speedo and tacho the HUD already computes.
        //
        // Checking the pack BEFORE modelling anything is the whole lesson of
        // today: the interior did not need building, it needed finding.
        // BACK TO CTR (2026-08-16). The E46 swap was made for the interior, but
        // the model is not ready to be the hero: its materials trip the
        // "full-metal with no MR texture renders BLACK" rule (the seven [gltf]
        // L5 clamp warnings at boot are exactly this car), and DriveDemo's
        // chassis box + wheel stations are still sized to the CTR, so the E46
        // body sits mis-scaled over CTR-position wheels — Tim's screenshot of
        // the "broken red sedan" is both defects at once. The interior car
        // comes back when it has had the convert_car_glb material pass and its
        // own wheel stations; until then the hero must be the car that is
        // actually finished.
        car.skin(*device, x3::game::convertedGlbRoot(), "Vehicles/CTR.glb");
        // E46_New is the INTERIOR car: Seats, Dashboard, SteeringWheel,
        // Interior, GearHandle and a pair of emissive Needle_KM / Needle_RPM
        // gauges. Same Wheel_FL/FR/RL/RR names and the same misspelled `Buttom`
        // underbody as CTR, so the skin mapping is untouched. Ten more vehicles
        // from the same pack sit beside it in converted_glb/Vehicles.
        // Point it down the corridor.
        // SPAWN YAW — engine forward at rest is -Z (CLAUDE.md AXES / CONVENTIONS
        // §3), so rotating rest forward (0,0,-1) about +Y by theta gives
        // (-sin theta, 0, -cos theta); facing the corridor direction (dirX, dirZ)
        // is theta = atan2(-dirX, -dirZ). The old atan2(dirZ, dirX) measured from
        // +X, not from -Z, which placed the car 90 deg off the road.
        // Tim, 2026-08-14: "The car is PLACED facing the wrong way. I have to TURN
        // it to drive it forward. Controls make the car behave as it should." —
        // the second sentence proves the rig and skin are fine; only spawn was wrong.
        const float yaw = -std::atan2(-route.dirX, -route.dirZ);
        const float q[4] = { 0.0f, std::sin(-yaw * 0.5f), 0.0f, std::cos(-yaw * 0.5f) };
        phys->setBodyRotation(car.chassis(), q);
    } else {
        x3::logWarn("--world tunnel: car build failed — walk/fly only");
    }

    // ---- WHEEL-SPIN FX (skid marks + smoke) --------------------------------
    // Tim, on the first cut: "smoke is square boxes.. and tire marks float".
    // Both were real: the smoke was a CUBE with a flat gray texture, and both
    // effects spawned at worldTransform[13] — the wheel HUB, a wheel-radius
    // above the road. "NFS in 2010 had NO SUCH GHOST" is a fair bar.
    //
    //   * smoke is now a SPHERE with a vertically-noised gray texture, drawn
    //     at low alpha, growing as it rises — a puff, not a crate;
    //   * marks are thin slabs ON the contact patch (hub minus wheel radius),
    //     ORIENTED to the car's heading at the moment they were laid — a mark
    //     laid mid-drift stays skewed on the road the way the tire actually
    //     drew it, instead of snapping to the world axes.
    x3::rhi::MeshHandle fxMarkMesh, fxPuffMesh;
    x3::rhi::TextureHandle fxSkidTex, fxSmokeTex;
    {
        std::vector<x3::rhi::MeshVertex> qv; std::vector<uint32_t> qi;
        x3::prims::makeCube(0.5f, qv, qi);
        fxMarkMesh = device->createMesh(qv.data(), (uint32_t)qv.size(), qi.data(), (uint32_t)qi.size());
        x3::prims::PrimMesh sph = x3::prims::makeUVSphere(12, 18);   // a puff needs no 8k tris
        fxPuffMesh = device->createMesh(sph.verts.data(), (uint32_t)sph.verts.size(),
                                        sph.index.data(), (uint32_t)sph.index.size());
        auto sk = x3::prims::makeSolidRGBA(8, 16, 16, 19);
        fxSkidTex = device->createTexture(sk.data(), 8, 8, true);
        // Smoke texture: gray with soft vertical banding so the sphere reads
        // as vapor with structure instead of a billiard ball.
        std::vector<uint8_t> sm(32 * 32 * 4);
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 32; ++x) {
                const float n = 0.82f + 0.18f * std::sin(y * 0.7f + x * 0.23f);
                uint8_t* p = &sm[(y * 32 + x) * 4];
                p[0] = (uint8_t)(158 * n); p[1] = (uint8_t)(158 * n);
                p[2] = (uint8_t)(163 * n); p[3] = 255;
            }
        fxSmokeTex = device->createTexture(sm.data(), 32, 32, true);
    }
    struct SpinFx { float x, y, z, age, yaw; uint8_t kind; };  // 0=skid, 1=smoke
    SpinFx fx[512]; uint32_t fxN = 0;
    float fxSpawnAcc = 0.0f;

    // ==== ENGINE NOTE =======================================================
    // Everything for this already existed and nothing played it: the sample is
    // committed at assets/audio/vehicles/engine_loop.wav, IAudioSystem has
    // startLoop3D/setLoopParams, and DriveDemo::engineRPM() reports the live
    // crank speed. The only missing piece was host wiring. (Same shape as the
    // shift points: data model present, playback absent.)
    //
    // A 3D loop parented to the car, re-pitched every frame from RPM. 3D rather
    // than 2D so the note attenuates and pans as the chase camera swings around
    // the car, and so it echoes correctly once RtAcoustics is in the path.
    std::unique_ptr<x3::audio::IAudioSystem> audio(x3::audio::createAudioSystem());
    const bool audioOn = audio && audio->init();
    // THUNDER VOICES. Two, not one: a near CRACK and a far ROLL, because air
    // strips the top end out of a strike over distance and one sample played at
    // two volumes does not fake that. Missing files are non-fatal -- the storm
    // then flashes in silence rather than refusing to run, which is the right
    // failure for an effect nobody has recorded yet.
    x3::audio::SoundHandle thunderNear{}, thunderFar{};
    x3::audio::SoundHandle engineSnd{};
    x3::audio::LoopHandle  engineLoop{};
    x3::audio::LoopHandle  whineLoop{};   // supercharger whine (throttle-gated)
    x3::audio::LoopHandle  turboLoop{};   // turbo whistle (spool-gated)
    float turboSpool = 0.0f, prevSpool = 0.0f;
    x3::audio::SoundHandle squealSnd{};
    x3::audio::LoopHandle  squealLoop{};   // tire squeal (slip-gated)
    if (audioOn) {
        const std::string wav =
            (std::filesystem::path(x3::game::assetRoot()) / "audio/vehicles/engine_loop.wav").string();
        engineSnd = audio->load(wav);
        squealSnd = audio->load((std::filesystem::path(x3::game::assetRoot()) / "audio/vehicles/tire_squeal_loop.wav").string());
        if (engineSnd.valid() && carBuilt) {
            float ep[3]; car.chassisPos(ep);
            (void)ep;
            // 2D on purpose. IAudioSystem::startLoop's own contract says 2D is
            // right for "the player's OWN" emitter, and there is no
            // setLoopPosition to follow a moving car with — a 3D loop would stay
            // pinned where the car spawned. The chase cam holds a fixed ~9 m
            // offset anyway, so there is no panning to win.
            engineLoop = audio->startLoop(engineSnd, 0.0f, 1.0f);
            x3::logInfo("[tunnel] engine note online");
        } else if (!engineSnd.valid()) {
            x3::logWarn("[tunnel] engine_loop.wav failed to load — driving stays silent");
        }

        // The two thunder voices. Neither exists in the tree yet, so this is
        // expected to warn once and go quiet; the storm still flashes, and the
        // moment a file lands at either path it is heard with no code change.
        if (weatherOn) {
            const std::string nearWav =
                (std::filesystem::path(x3::game::assetRoot()) / "audio/weather/thunder_crack.wav").string();
            const std::string farWav =
                (std::filesystem::path(x3::game::assetRoot()) / "audio/weather/thunder_roll.wav").string();
            thunderNear = audio->load(nearWav);
            thunderFar  = audio->load(farWav);
            storm.setVoices(thunderNear.id, thunderFar.id);
            if (!thunderNear.valid() && !thunderFar.valid())
                x3::logWarn("[tunnel] no thunder samples at assets/audio/weather/ — "
                            "lightning will flash silently");
            else
                x3::logInfo("[tunnel] thunder online");
        }

        // ---- THE ROOMS MAKE A NOISE ------------------------------------
        // A plant room is pumps and vent plant; the one thing it must never be
        // is silent. startLoop3D rather than a one-shot on a timer: the position
        // is set once and miniaudio re-derives attenuation and panning against
        // the live listener every mix callback, so the hum swells as you walk
        // the hall toward it and falls away behind you. That is the difference
        // between a machine in a room and a sound on a trigger.
        //
        // It is also the ONLY cue that the door you just drove past leads
        // anywhere. Standing in the bore you cannot see a room; you can hear one.
        if (!plantHumPos.empty()) {
            const std::string humWav =
                (std::filesystem::path(x3::game::assetRoot()) / "audio/echotropolis/ambient/mine_hum.wav").string();
            const x3::audio::SoundHandle hum = audio->load(humWav);
            if (hum.valid()) {
                for (const auto& p : plantHumPos)
                    audio->startLoop3D(hum, p[0], p[1], p[2], 0.55f, 0.85f);
                char hb[96];
                std::snprintf(hb, sizeof(hb), "[tunnel] %u plant-room hum(s) running",
                              (uint32_t)plantHumPos.size());
                x3::logInfo(hb);
            } else {
                x3::logWarn("[tunnel] mine_hum.wav missing - the plant rooms stay silent");
            }
        }
    }
    // ==== GAUGE ARTWORK =====================================================
    // Real textures, not quads. The first cut approximated a dial with 121 tiny
    // axis-aligned rectangles because I had told the agent "rectangles only";
    // drawHudImage() takes a TEXTURE with UV sub-rects, so the right reading of
    // that constraint is "put real art in the rectangle". Owner's verdict on the
    // quad version: "slop in Carbon esque shape".
    // Generated by tools/make_gauge_textures.py — rerun it to change the art.
    x3::rhi::TextureHandle texDial{}, texNeedle{}, texGate{}, texBoost{};
    {
        auto loadPng = [&](const char* rel) -> x3::rhi::TextureHandle {
            const std::string p =
                (std::filesystem::path(x3::game::assetRoot()) / "ui" / rel).string();
            int w = 0, h = 0, c = 0;
            stbi_uc* px = stbi_load(p.c_str(), &w, &h, &c, 4);
            if (!px) { x3::logWarn(std::string("[tunnel] gauge art missing: ") + p); return {}; }
            x3::rhi::TextureHandle t = device->createTexture(px, (uint32_t)w, (uint32_t)h, true);
            stbi_image_free(px);
            return t;
        };
        texDial   = loadPng("gauge_dial.png");
        texNeedle = loadPng("gauge_needle.png");
        texGate   = loadPng("gauge_gate.png");
        texBoost  = loadPng("gauge_boost.png");
    }

    phys->optimizeBroadphase();

    const float dt = 1.0f / 60.0f;

    // ==== HEADLESS: the proof set ===========================================
    if (headless) {
        namespace fs = std::filesystem;
        std::error_code ec;
        const std::string dir = hc.tunnelShot ? hc.tunnelShotDir : std::string("docs/screenshots/tunnel");
        fs::create_directories(dir, ec);

        auto settleAndGrab = [&](const float cam[5], const std::string& out) -> bool {
            // The streamer only enqueues the full ring on a focus-tile crossing
            // (host_cliffs.cpp's trick): nudge the focus on frame 1, then hold.
            const int kFrames = 200;
            for (int i = 0; i < kFrames; ++i) {
                glfwPollEvents();
                const float fx = (i == 1) ? cam[0] + 40.0f : cam[0];
                streamer.update(scene, *device, *phys, fx, cam[2]);
                phys->step(dt);
                device->setCamera(cam[0], cam[1], cam[2], cam[3], cam[4], 68.0f);

                // THE CAPTURE LOOP NEEDS THE WEATHER TOO. This settle loop is
                // entirely separate from the interactive one below, so wiring
                // weather into only the latter left every screenshot a clear
                // summer afternoon no matter what was forced -- which is exactly
                // how you ship a feature nobody can see.
                if (weatherOn) {
                    weather.tick(dt);
                    const x3::game::WeatherSample& ws = weather.sample();
                    wetness.tick(dt, ws.precipitation, ws.tempC, ws.snowfall);
                    storm.tick(dt, ws.state == x3::game::WeatherState::Storm ? ws.hazardLevel : 0.0f,
                               nullptr, cam[0], cam[1], cam[2]);
                    x3::rhi::IRenderDevice::SkyParams sp = ws.sky;
                    sp.enabled = true;
                    sp.sunDir[0] = 0.35f; sp.sunDir[1] = 0.92f; sp.sunDir[2] = 0.18f;
                    sp.cloud    = 0.15f + 0.85f * ws.fogDensity;
                    sp.exposure = ws.sky.exposure + storm.flash();
                    device->setSkyParams(sp);
                    x3::rhi::IRenderDevice::WetnessParams wp{};
                    wp.amount = wetness.wetness() * (1.0f - wetness.snowCover());
                    device->setWetness(wp);
                    device->setSnowCover(wetness.snowCover());
                    precip.update(dt,
                                  ws.snowfall ? x3::game::PrecipKind::Snow
                                              : (ws.precipitation > 0.0f ? x3::game::PrecipKind::Rain
                                                                         : x3::game::PrecipKind::None),
                                  ws.precipitation, cam[0], cam[1], cam[2], 0.0f, 0.0f,
                                  skyVisibleAt(*phys, cam[0], cam[1], cam[2], route.dirX, route.dirZ));
                }

                if (i == kFrames - 1) device->armCapture(out.c_str());
                auto frame = device->beginFrame();
                if (frame.valid) {
                    scene.render(*device, frame);
                    if (carBuilt) car.render(frame);
                    if (weatherOn) precip.submit(*device, frame);
                }

                device->endFrame(frame);
            }
            const bool wrote = device->captureFrame(out.c_str());
            if (wrote) x3::logInfo("--world tunnel: wrote " + out);
            else       x3::logError("--world tunnel: capture FAILED " + out);
            return wrote;
        };

        bool ok = true;
        if (hc.tunnelShot) {
            struct Shot { int which; const char* name; };
            const Shot shots[] = {
                { 0, "01_approach"  },
                { 1, "02_inside"    },
                { 2, "03_far_mouth" },
                { 3, "04_saddle"    },
                { 4, "05_portal_detail" },
                { 5, "06_mouth_headon" },
                { 6, "07_inside_looking_out" },
                { 7, "08_exit_portal" },
                { 8, "09_garage_lnss" },   // inside the Late Night Speed bay
            };
            for (const Shot& sh : shots) {
                float cam[5]; tunnel.showcaseCamera(route, sh.which, cam);
                char path[512];
                std::snprintf(path, sizeof(path), "%s/%s.png", dir.c_str(), sh.name);
                char cb[256];
                std::snprintf(cb, sizeof(cb), "--world tunnel: %s cam=(%.1f, %.1f, %.1f) yaw=%.3f pitch=%.3f",
                              sh.name, cam[0], cam[1], cam[2], cam[3], cam[4]);
                x3::logInfo(cb);
                ok = settleAndGrab(cam, path) && ok;
            }
        } else {
            float cam[5]; tunnel.showcaseCamera(route, 0, cam);
            if (hc.shotCamOverride) for (int k = 0; k < 5; ++k) cam[k] = hc.shotCam[k];
            const std::string out = screenshot ? screenshotPath : std::string("w_tunnel.png");
            ok = settleAndGrab(cam, out);
        }

        if (carBuilt) car.shutdown();
        tunnel.shutdown(*device, *phys);
        for (auto& w : tourBores) w->shutdown(*device, *phys);
        // Shared across every bore, so it is released ONCE here rather than by
        // each tunnel's own shutdown (which would free textures its neighbours
        // are still drawing with).
        x3::game::shutdownTunnelSurfaces(*device);
        streamer.shutdown(scene, *device, *phys);
        if (audioOn) {
        if (engineLoop.valid()) audio->stopLoop(engineLoop);
        audio->shutdown();
    }
    jobs->shutdown(); phys->shutdown(); device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return ok ? 0 : 1;
    }

    // ==== INTERACTIVE: drive it =============================================
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
    double lastMX, lastMY; glfwGetCursorPos(window, &lastMX, &lastMY);
    double prevTime = glfwGetTime();
    float camYaw = std::atan2(route.dirZ, route.dirX), camPitch = -0.10f;
    int lastW = (int)W, lastH = (int)H;
    x3::logInfo("--world tunnel: WASD drives, Space handbrake, mouse orbits the chase cam, "
                "~ console, ESC menu, SHIFT+ESC quits");

    // ---- DEV SHELL: console, pause menu, FPS -------------------------------
    // The reason the whole vehicle-feel pass was slow: every torque figure, grip
    // scale and centre-of-mass nudge cost an edit-rebuild-relaunch-drive-back
    // cycle, and those are values you have to judge by feel, one at a time. They
    // are all live now.
    HostShell shell;
    shell.attach(hc);
    shell.setFreezesSim(true);          // this host really does stop the sim on ESC
    console = shell.console();
    if (auto* con = shell.console()) {
        // ---- weather (moved from the old host-local console) ----
        con->registerCVar("wx", "off",
            "weather: off | clear | cloudy | rain | storm | fog | snow");
        con->registerCVar("wx_snow_in", "0",
            "lying snow depth to prime, INCHES (applied when wx changes)");
        con->registerCVar("wx_hour", "14",
            "time of day, 0-24, drives the diurnal temperature swing");
        // Seed from the env vars so the documented X3_WEATHER path still works
        // and the console simply shows what you already asked for.
        {
            const char* e = std::getenv("X3_WEATHER");
            if (e && e[0] && std::strcmp(e, "0") != 0) con->set("wx", e);
            if (const char* si = std::getenv("X3_SNOW_IN")) con->set("wx_snow_in", si);
        }
        shell.addFloatCommand("car_torque", "peak engine torque, ft-lb (stock 590)",
            [&](float v) { x3::phys::WheeledTuning t; t.maxEngineTorque = v * 1.35582f; car.applyTuning(t); });
        shell.addFloatCommand("car_redline", "engine redline, rpm (stock 7500)",
            [&](float v) { x3::phys::WheeledTuning t; t.maxEngineRPM = v; car.applyTuning(t); });
        shell.addFloatCommand("car_grip", "tyre grip multiplier (stock 5.2; 1 = Jolt's economy tyre)",
            [&](float v) { x3::phys::WheeledTuning t; t.gripScale = v; car.applyTuning(t); });
        shell.addFloatCommand("car_mass", "chassis mass, kg (stock 1300)",
            [&](float v) { x3::phys::WheeledTuning t; t.massKg = v; car.applyTuning(t); });
        shell.addFloatCommand("car_brake", "brake torque, Nm, all wheels",
            [&](float v) { x3::phys::WheeledTuning t; t.brakeTorque = v; car.applyTuning(t); });
        shell.addFloatCommand("car_ride", "ride-height delta, m (negative lowers)",
            [&](float v) { x3::phys::WheeledTuning t; t.rideHeightDelta = v; car.applyTuning(t); });
        shell.addFloatCommand("car_springfreq", "suspension spring frequency, Hz",
            [&](float v) { x3::phys::WheeledTuning t; t.suspensionFreq = v; car.applyTuning(t); });
        shell.addFloatCommand("car_springdamp", "suspension damping ratio",
            [&](float v) { x3::phys::WheeledTuning t; t.suspensionDamp = v; car.applyTuning(t); });
        shell.addFloatCommand("car_torquemult", "flat torque multiplier on top of the turbo (nitrous)",
            [&](float v) { car.setTorqueBoost(v); });
        // ---- turbo ----
        shell.addToggleCommand("turbo", "turbo on/off (off = the curve with no lag, naturally aspirated)",
            [&]{ return car.turboEnabled(); },
            [&](bool on) { car.setTurboEnabled(on); });
        shell.addFloatCommand("turbo_max", "peak boost, psi (stock 16)",
            [&](float v) { car.turbo().maxPsi = v; });
        shell.addFloatCommand("turbo_spool", "seconds for the compressor to come up (stock 0.45)",
            [&](float v) { car.turbo().spoolTau = v; });
        shell.addFloatCommand("turbo_dump", "seconds to bleed off on a lift (stock 0.11)",
            [&](float v) { car.turbo().dumpTau = v; });
        shell.addFloatCommand("turbo_start", "rpm where the compressor starts to make pressure (stock 1800)",
            [&](float v) { car.turbo().spoolStartRpm = v; });
        shell.addFloatCommand("turbo_full", "rpm for full boost (stock 4200)",
            [&](float v) { car.turbo().spoolFullRpm = v; });
        shell.addFloatCommand("turbo_floor", "torque fraction with no boost at all (stock 0.60)",
            [&](float v) { car.turbo().floorTorque = v; });
        shell.addFloatCommand("turbo_vacuum", "vacuum depth at a closed throttle, psi (stock 8.5)",
            [&](float v) { car.turbo().vacuumPsi = v; });
        shell.addToggleCommand("car_tc", "traction control (also bound to T)",
            [&]{ return car.tractionControl(); },
            [&](bool on) { car.setTractionControl(on); });
        shell.addToggleCommand("climb", "crawl traction for steep terrain (also bound to C)",
            [&]{ return car.climbMode(); },
            [&](bool on) { car.setClimbMode(on); });
        con->registerCommand("car_reset", [&](const std::vector<std::string>&) {
            car.setTorqueBoost(1.0f);
            x3::phys::WheeledTuning t;
            t.maxEngineTorque = 2400.0f; t.maxEngineRPM = 7500.0f;
            t.gripScale = 5.2f; t.massKg = 1300.0f;
            car.applyTuning(t);
            con->print("car back to the shipped 993 Turbo numbers");
        }, "restore the stock vehicle tune");
        con->registerCommand("car", [&](const std::vector<std::string>&) {
            char b[256];
            std::snprintf(b, sizeof(b),
                          "gear %d  %.0f rpm  %.0f mph  %+.1f psi (x%.2f)  TC %s  turbo %s",
                          car.gear(), (double)car.engineRPM(),
                          (double)(std::fabs(car.forwardSpeed()) * 2.23694f),
                          (double)car.boostPsi(), (double)car.turboMult(),
                          car.tractionControl() ? "on" : "off",
                          car.turboEnabled() ? "on" : "off");
            con->print(b);
        }, "print the car's live state");
    }

    while (!glfwWindowShouldClose(window) && !shell.wantQuit()) {
        // RE-SUBMIT THE BORE LIGHTS EVERY FRAME. They were set exactly ONCE at boot
        // (setPointLights above), which is why the tunnel is lit in headless captures
        // — those render a few frames with nothing else touching the light set — and
        // PITCH BLACK the moment you drive it, both from inside and looking in through
        // the portal from outside. The interactive loop streams tiles and draws other
        // content, and the light array does not survive that. Cheap: 6 cached lights.
        glfwPollEvents();
        shell.beginFrame();

        // ESC OPENS THE MENU, IT DOES NOT QUIT — the shell owns that now, along
        // with the console and the FPS overlay. This host used to hand-roll the
        // pause by polling glfwGetKey and tracking its own `escWasDown` edge,
        // which drops a press any time a frame runs longer than the keypress.
        // The shell edge-detects in the GLFW key CALLBACK instead, so a press
        // cannot be missed no matter how long the frame took.
        const double now = glfwGetTime();
        float fdt = (float)(now - prevTime); prevTime = now;
        if (fdt > 0.1f) fdt = 0.1f;

        // MERGE NOTE (integration/complete): everything below keeps the roads
        // lane's weather/Jake/on-foot systems, driven through the vehicle lane's
        // HostShell — one console, edge-detected keys, and a real pause.
        if (shell.paused()) {
            // Present so the window stays live, but do not advance the sim.
            auto pf = device->beginFrame();
            if (pf.valid) {
                scene.render(*device, pf);
                if (carBuilt) car.render(pf);
                shell.draw(pf, fdt);
            }
            device->endFrame(pf);
            continue;
        }

        // ==== WEATHER TICK ===================================================
        // Chained in dependency order. Note the CLOCK: an in-world day is
        // compressed to ten real minutes, because the diurnal temperature swing
        // is the most interesting thing the model does and nobody is going to
        // sit through twenty-four hours to watch the desert cool off.
        if (weatherOn) {
            weather.tick(fdt);
            // The clock RUNS, but wx_hour re-seeds it -- so you can jump to the
            // pre-dawn trough to see ice form instead of waiting out the cycle.
            static float todHours = 14.0f;
            static float lastHourCvar = -1.0f;
            const float hourCvar = console->getFloat("wx_hour");
            if (hourCvar != lastHourCvar) { todHours = hourCvar; lastHourCvar = hourCvar; }
            todHours += fdt * (24.0f / 600.0f);        // 10 real minutes per in-world day
            if (todHours >= 24.0f) todHours -= 24.0f;
            weather.setTimeOfDay(todHours);

            const x3::game::WeatherSample& ws = weather.sample();
            wetness.tick(fdt, ws.precipitation, ws.tempC, ws.snowfall);

            // Lightning only under an actual storm; hazardLevel already carries
            // "how bad", so intensity comes free and correct.
            const float stormI = (ws.state == x3::game::WeatherState::Storm)
                               ? ws.hazardLevel : 0.0f;
            float lp[3] = { 0.0f, 0.0f, 0.0f };
            if (carBuilt) car.chassisPos(lp);
            storm.tick(fdt, stormI, audioOn ? audio.get() : nullptr, lp[0], lp[1], lp[2]);

            // Push the sky. The storm FLASH rides on exposure rather than on the
            // sun: a strike lights the whole cloud deck from inside, so raising
            // the sun would throw hard directional shadows from a light source
            // that is not there and give the whole thing away.
            x3::rhi::IRenderDevice::SkyParams sp = ws.sky;
            sp.enabled = true;
            sp.sunDir[0] = 0.35f; sp.sunDir[1] = 0.92f; sp.sunDir[2] = 0.18f;
            // Cloud cover tracks the haze the state already asked for, so an
            // overcast sky is actually overcast instead of clear-with-fog.
            sp.cloud    = 0.15f + 0.85f * ws.fogDensity;
            sp.exposure = ws.sky.exposure + storm.flash();
            device->setSkyParams(sp);

            // Wet ground for the renderer. Lying SNOW suppresses the wet look
            // rather than adding to it -- snow is bright and near-matte where
            // water is dark and mirror-like, so handing both over as one "shiny
            // ground" number would make a snowfield glisten like a wet street.
            x3::rhi::IRenderDevice::WetnessParams wp{};
            wp.amount = wetness.wetness() * (1.0f - wetness.snowCover());
            // Ice is glassier than water: it converges to a lower roughness and
            // pools less, because it froze flat.
            wp.minRough = 0.06f - 0.03f * wetness.iciness();
            wp.puddles  = 1.0f - 0.7f * wetness.iciness();
            device->setWetness(wp);

            // LYING SNOW -> the terrain snowline. Brings the white DOWN the
            // range rather than whitening everything at once.
            device->setSnowCover(wetness.snowCover());
            // The falling half is updated further down, once the CAMERA is
            // solved -- the volume must centre on the eye, not on the car, or a
            // chase-cam offset leaves a metre of snow hanging behind your own
            // viewpoint. Stash what it needs.
            precipKind = ws.snowfall ? x3::game::PrecipKind::Snow
                                     : (ws.precipitation > 0.0f ? x3::game::PrecipKind::Rain
                                                                : x3::game::PrecipKind::None);
            precipAmt  = ws.precipitation;

        }
        double mx, my; glfwGetCursorPos(window, &mx, &my);
        // Gate the LOOK, not just the camera apply: the deltas also feed the
        // on-foot Player below, and the cursor is released while typing — an
        // ungated delta would spin Jake's view across the screen on the way to
        // the scrollback.
        const float look = shell.inputEnabled() ? 1.0f : 0.0f;
        const float ddx = (float)(mx - lastMX) * look, ddy = (float)(my - lastMY) * look;
        lastMX = mx; lastMY = my;
        camYaw += ddx * 0.0025f; camPitch -= ddy * 0.0025f;
        if (camPitch >  1.2f) camPitch =  1.2f;
        if (camPitch < -1.2f) camPitch = -1.2f;
        // (The hand-rolled CONSOLE KEYS block is gone: the shell handles the
        // toggle, editing, history, completion and scrollback in the GLFW key
        // callback, where a press cannot be dropped by a long frame.)
        const bool typing = shell.consoleOpen();
        (void)typing;

        // ---- WEATHER FROM THE CONSOLE. Re-read every frame; act only when the
        // string CHANGES, because forcing the state every frame would restart
        // the transition continuously and the sky would never actually arrive.
        {
            const std::string wxWant = console->getString("wx");
            if (wxWant != wxApplied) {
                wxApplied = wxWant;
                weatherOn = (wxWant != "off" && !wxWant.empty());
                if (weatherOn) {
                    if (!precipInit) { precip.init(x3::game::PrecipConfig{}); storm.reset(); precipInit = true; }
                    using WS = x3::game::WeatherState;
                    weather.setBiome(x3::game::Biome::Temperate);
                    if (wxWant == "snow") {
                        weather.setBiome(x3::game::Biome::Snow);
                        weather.forceState(WS::Snow, true);
                    }
                    else if (wxWant == "storm")  weather.forceState(WS::Storm,  true);
                    else if (wxWant == "rain")   weather.forceState(WS::Rain,   true);
                    else if (wxWant == "fog")    weather.forceState(WS::Fog,    true);
                    else if (wxWant == "cloudy") weather.forceState(WS::Cloudy, true);
                    else                          weather.forceState(WS::Clear,  true);
                    // Re-prime the snowpack to whatever depth was asked for. The
                    // model integrates in real time at an inch an hour, so
                    // without this "wx snow" on a bare road stays bare for forty
                    // minutes and reads as broken.
                    // ONE RULE for the starting depth. The boot path primed 2.6 in
                    // when it was snowing; this path then reset it to wx_snow_in's
                    // default of ZERO and wiped it -- two owners of one number,
                    // the same defect as the fitout seed. Snowfall with no depth
                    // asked for gets the settled default; anything else honours
                    // the cvar exactly.
                    float wantIn = console->getFloat("wx_snow_in");
                    if (wantIn <= 0.0f && weather.sample().snowfall) wantIn = 2.6f;
                    wetness.reset();
                    if (wantIn > 0.0f) {
                        const x3::game::WeatherSample& ps = weather.sample();
                        for (int i = 0; i < 60 * 60 * 24 && wetness.snowDepthIn() < wantIn; ++i)
                            wetness.tick(1.0f, ps.precipitation, ps.tempC, ps.snowfall);
                    }
                    char wb[128];
                    std::snprintf(wb, sizeof(wb), "weather: %s, %.1f in lying",
                                  wxWant.c_str(), wetness.snowDepthIn());
                    console->print(wb);
                } else {
                    x3::rhi::IRenderDevice::SkyParams sp{};
                    sp.enabled = true;
                    sp.sunDir[0] = 0.35f; sp.sunDir[1] = 0.92f; sp.sunDir[2] = 0.18f;
                    sp.sunColor[0] = 1.0f; sp.sunColor[1] = 0.97f; sp.sunColor[2] = 0.92f;
                    sp.sunIntensity = 1.0f; sp.haze = 0.35f; sp.exposure = 1.0f; sp.cloud = 0.42f;
                    device->setSkyParams(sp);
                    device->setSnowCover(0.0f);
                    device->setWetness(x3::rhi::IRenderDevice::WetnessParams{});
                    console->print("weather: off (the demo's fixed bright sky)");
                }
            }
        }

        // shell.key(), not glfwGetKey(): false while the console or the menu
        // owns the keyboard, so typing `car_grip 6` no longer also steers
        // right, brakes and applies the handbrake.
        auto kd = [&](int k){ return shell.key(k); };

        // ---- E: GET OUT / GET IN ----------------------------------------
        // Edge-triggered, and re-entry is PROXIMITY gated: you have to walk back
        // to the car. Without that gate E teleports you into a car you left half
        // a mile behind, which is not a vehicle so much as a summoning.
        {
            static bool eWasDown = false;
            const bool eDown = kd(GLFW_KEY_E);   // shell-gated: E while typing is just a letter
            if (eDown && !eWasDown && carBuilt) {
                if (driving) {
                    float vp[3]; car.chassisPos(vp);
                    parkedAt[0] = vp[0]; parkedAt[1] = vp[1]; parkedAt[2] = vp[2];
                    // Step out on the LEFT, a car's width clear of the shell, and
                    // above the floor -- spawning inside the car's own collision
                    // launches the capsule through the roof.
                    // LEFT of travel. tunnel_corridor builds its frame as
                    // right = (-dirZ, 0, dirX), so left is its negation -- taken
                    // from the route rather than the car's own heading so you
                    // always step toward the walkway, even if you stopped skewed.
                    const float ox =  route.dirZ * 2.4f, oz = -route.dirX * 2.4f;
                    if (!footSpawned) {
                        onFoot.spawn(*phys, vp[0] + ox, vp[1] + 1.2f, vp[2] + oz);
                        footSpawned = true;
                    } else {
                        onFoot.setFeetPosition(*phys, x3::phys::Vec3{ vp[0] + ox, vp[1] + 1.2f, vp[2] + oz });
                    }
                    driving = false;
                    // Load him ONCE, on the first exit rather than at boot: most
                    // runs of this world never leave the car, and a 1.4 MB rig
                    // plus its textures is not worth paying for on the chance.
                    if (!jakeTried) {
                        jakeTried = true;
                        jakeSrc.reset(x3::asset::createAssetSource());
                        const std::string glbDir = x3::game::assetRoot() + "/rigged_glb";
                        if (jakeSrc && jakeSrc->mountDir(glbDir, 0)) {
                            jakeLoader.reset(x3::asset::createModelLoader(device, jakeSrc.get()));
                            // JakeClone_player, not Jake_44_actions: same man, and
                            // 1.4 MB against 26 MB. The 44-clip rig earns its size
                            // when something plays those clips; nothing here does
                            // yet, so paying for it would be paying for nothing.
                            jakeModel = jakeLoader->load("JakeClone_player.glb");
                            if (jakeModel.ok) {
                                jakeDraw = x3::asset::makeDrawables(jakeModel);
                                char jb[128];
                                std::snprintf(jb, sizeof(jb), "[tunnel] Jake: %u drawable(s)",
                                              (uint32_t)jakeDraw.size());
                                x3::logInfo(jb);
                            } else {
                                x3::logWarn("[tunnel] JakeClone_player.glb failed to load - no body on foot");
                            }
                        }
                    }
                    x3::logInfo("[tunnel] on foot - E near the car to get back in");
                } else {
                    float fx, fy, fz, fyaw, fpit;
                    onFoot.camera(fx, fy, fz, fyaw, fpit);
                    const float dxc = fx - parkedAt[0], dzc = fz - parkedAt[2];
                    if (dxc*dxc + dzc*dzc <= 16.0f) {          // within 13 ft
                        driving = true;
                        x3::logInfo("[tunnel] back in the car");
                    }
                }
            }
            eWasDown = eDown;
        }

        // ---- ON-FOOT MOVEMENT. The car keeps its own WASD; on foot the same
        // keys drive the capsule, and the mouse deltas already gathered above
        // are handed to the Player so look feels identical in both modes.
        if (!driving && footSpawned) {
            x3::game::PlayerInput pin;
            pin.moveFwd    = (kd(GLFW_KEY_W) ? 1.0f : 0.0f) - (kd(GLFW_KEY_S) ? 1.0f : 0.0f);
            pin.moveStrafe = (kd(GLFW_KEY_D) ? 1.0f : 0.0f) - (kd(GLFW_KEY_A) ? 1.0f : 0.0f);
            pin.sprint     = kd(GLFW_KEY_LEFT_SHIFT);
            static bool spaceWas = false;
            const bool spaceNow = kd(GLFW_KEY_SPACE);
            pin.jumpPressed = spaceNow && !spaceWas;
            pin.jumpHeld    = spaceNow;
            spaceWas = spaceNow;
            pin.lookDX = ddx; pin.lookDY = ddy;
            onFoot.update(pin, fdt, *phys);
        }

        x3::phys::VehicleInput in;
        // A PARKED CAR STAYS PARKED. Leaving the throttle live while you walk
        // away means the handbrake is the only thing between you and a driverless
        // car rolling down the grade you just stopped on.
        if (!driving) { in = x3::phys::VehicleInput{}; in.handBrake = 1.0f; }
        else if (carBuilt) {
            in.throttle = (kd(GLFW_KEY_W) ? 1.0f : 0.0f) - (kd(GLFW_KEY_S) ? 1.0f : 0.0f);
            in.steer    = (kd(GLFW_KEY_D) ? 1.0f : 0.0f) - (kd(GLFW_KEY_A) ? 1.0f : 0.0f);
            if (kd(GLFW_KEY_SPACE)) in.handBrake = 1.0f;
            // T toggles TRACTION CONTROL (Tim asked for an off switch). Edge
            // triggered. TC trims throttle toward a 0.10 slip target and can cut
            // to 15%, which is great for a clean launch and wrong when you want
            // to hang the tail out. Off = the tyres are the only limit.
            {
                static bool tcWasDown = false;
                const bool tcDown = kd(GLFW_KEY_T);
                if (tcDown && !tcWasDown) {
                    car.setTractionControl(!car.tractionControl());
                    x3::logInfo(car.tractionControl() ? "[tunnel] traction control ON"
                                                      : "[tunnel] traction control OFF");
                }
                tcWasDown = tcDown;
            }
            {   // C: CLIMB MODE — crawl traction for the mountainsides. See
                // DriveDemo::setClimbMode: slip held at the friction peak, trim
                // floor near zero, turbo bypassed so crawl torque is instant.
                static bool climbWasDown = false;
                const bool climbDown = kd(GLFW_KEY_C);
                if (climbDown && !climbWasDown) {
                    car.setClimbMode(!car.climbMode());
                    x3::logInfo(car.climbMode() ? "[tunnel] CLIMB mode ON"
                                                : "[tunnel] climb mode off");
                }
                climbWasDown = climbDown;
            }
            if (in.throttle < 0.0f && car.forwardSpeed() > 0.5f) { in.brake = 1.0f; in.throttle = 0.0f; }

            // AUTO-HOLD. Tim, 2026-08-15: "It should be Unable to roll when not
            // accelerating or reversing, there is an E brake."
            // With no throttle the rig had brake 0 AND handbrake 0, i.e. neutral,
            // so the car free-wheeled down every gradient — it "rolled" in the
            // sense of rolling AWAY (not tipping over; that was my misreading).
            // A real car holds: an auto creeps against its brakes and a parked
            // one sits on the handbrake.
            // Braking ramps in as speed falls so coasting still feels like
            // coasting, then locks solid at a standstill. Skipped while the
            // player is on the handbrake so deliberate slides still work.
            if (in.throttle == 0.0f && in.handBrake == 0.0f) {
                const float spd = std::fabs(car.forwardSpeed());   // m/s
                if (spd < 0.35f) {
                    in.brake = 1.0f;          // parked: hold it, full stop
                } else if (spd < 6.0f) {
                    // 0.25 at 6 m/s -> 1.0 approaching rest: settles without a lurch
                    in.brake = 0.25f + 0.75f * (1.0f - spd / 6.0f);
                } else {
                    in.brake = 0.08f;         // light drag, reads as engine braking
                }
            }
            car.setInput(in);
            car.preStep(fdt);
        }
        float vp[3] = { startPos[0], startPos[1], startPos[2] };
        if (carBuilt) car.chassisPos(vp);
        streamer.update(scene, *device, *phys, vp[0], vp[2]);
        phys->step(fdt);
        if (carBuilt) car.postStep(fdt);
        // RE-SAMPLE THE CHASE TARGET AFTER THE STEP.
        // `vp` above was read BEFORE phys->step(), so the camera was aiming at
        // where the car had been one physics step earlier while the car itself
        // draws from its post-step pose. At 30 m/s a 60 Hz step is ~0.5 m, and
        // because the frame delta varies the lag varies with it — so the car
        // appears to oscillate between two positions a few pixels apart every
        // frame. Tim, 2026-08-14: "when accelerating / moving, the car is
        // oscillating between two points several pixels apart, causing a
        // blur/shimmer."
        // The pre-step sample is still the right input for streamer.update()
        // (tile streaming does not need sub-frame precision); only the camera
        // needs the current pose.
        if (carBuilt) car.chassisPos(vp);
        scene.update(*phys);

        // ---- ENGINE NOTE: re-pitch from live RPM, and move the emitter ------
        // pitch tracks RPM across the powerband; vol fades in off idle so a
        // parked car is not droning at full volume. Both are cheap per-frame
        // parameter updates on ONE voice — no retriggering, so the loop stays
        // seamless through gearchanges.
        if (audioOn && engineLoop.valid() && carBuilt) {
            const float rpm    = car.audioRPM();
            const float redline = 7500.0f;                       // matches vd.maxEngineRPM
            const float frac   = std::min(1.0f, std::max(0.0f, rpm / redline));
            // PITCH tracks RPM PROPORTIONALLY — real engine-note frequency scales
            // linearly with crank speed, so the playback rate must too. The old
            // 1.05 + frac*1.75 span (1.05 -> 2.80) never reached the top: Tim,
            // 2026-08-15 — "7500 rpm sounds like 3000 rpm in real life".
            // Calibrated from that: 2.80x == ~3000 rpm, so unity (1.0x) ==
            // ~1071 rpm, and 7500 rpm needs ~7.0x (within the 8.0x clamp). Idle
            // (~800) therefore sits at ~0.75x — a genuinely low idle note. If
            // that reads "rattly", the real fix is a second higher-RPM loop
            // crossfaded in, not compressing the range again.
            const float rawPitch = rpm / 1071.0f;
            // IDLE HOLD. A flat-six idles at a steady ~800 rpm, but the physics
            // engine has no idle governor and hunts around zero throttle — so the
            // note must NOT wobble with it. Parked + off-throttle -> fixed idle
            // pitch; the moment the driver asks for power or the car rolls, it
            // tracks rpm again (overrun still follows rpm, as it should).
            const bool idling = (car.throttleInput() < 0.01f &&
                                 std::fabs(car.forwardSpeed()) < 1.0f);
            const float pitch = idling ? 0.75f : rawPitch;

            // VOLUME follows LOAD, not speed. Tim, 2026-08-15: "In a real car..
            // engine tone shifts with load.. and load changes with torque, and
            // torque is not flat, its a curve."
            // Load = what the driver is asking for, times what the engine can
            // actually make at these revs. Same normalized curve the physics
            // runs — [0,0.78] [0.3,0.97] [0.55,1.0] [0.8,0.95] [1,0.82] — so the
            // note thickens through the midrange and thins at the top exactly
            // where the engine does, instead of just getting louder with rpm.
            auto torqueFrac = [](float f) {
                const float xs[5] = {0.00f, 0.30f, 0.55f, 0.80f, 1.00f};
                const float ys[5] = {0.78f, 0.97f, 1.00f, 0.95f, 0.82f};
                if (f <= xs[0]) return ys[0];
                for (int k = 1; k < 5; ++k)
                    if (f <= xs[k]) {
                        const float t = (f - xs[k-1]) / (xs[k] - xs[k-1]);
                        return ys[k-1] + (ys[k] - ys[k-1]) * t;
                    }
                return ys[4];
            };
            const float thr  = std::min(1.0f, std::max(0.0f, car.effectiveThrottle()));
            // ...times what the TURBO is currently delivering. The multiplier
            // runs 0.60 off boost to 1.00 on it, so the note swells over the
            // half-second the compressor takes to come up and drops the instant
            // you lift. That swell is the single most recognisable thing about
            // a turbo car, and it costs one multiply.
            const float load = thr * torqueFrac(frac) * car.turboMult();
            // Off-throttle is OVERRUN: the engine is being driven by the wheels,
            // so it stays audible and keeps its pitch but drops right back in
            // level. That contrast is most of what makes a car sound driven.
            const float vol  = 0.16f + 0.62f * load + 0.10f * frac;
            // LOW-PASS the note. The physics engine can jitter its RPM (the
            // clutch/gearbox hunt this lane has been chasing), but a real engine
            // note does NOT wobble frame to frame — it glides. One-pole smooth
            // (~0.1 s) so it reads as one continuous engine, not a stutter.
            static float sPitch = 0.75f, sVol = 0.16f;
            const float k = 1.0f - std::exp(-9.0f * fdt);
            sPitch += (pitch - sPitch) * k;
            sVol   += (vol   - sVol)   * k;
            audio->setLoopParams(engineLoop, sVol, sPitch);

            // Supercharger whine + turbo whistle — the old --world drive host's
            // extra layers, pitched variants of the SAME engine loop (Tim: "use
            // the old host drive sounds"). Whine is throttle-gated; whistle rides
            // the spool; lifting off above ~55% spool = a blowoff psshh.
            if (!whineLoop.valid()) whineLoop = audio->startLoop(engineSnd, 0.0f, 2.4f);
            if (whineLoop.valid())
                audio->setLoopParams(whineLoop, thr * 0.20f, 2.4f + 1.3f * frac);

            const float spoolLag = 0.45f;   // == TurboParams::spoolTau
            if (thr > 0.6f) turboSpool = std::min(1.0f, turboSpool + fdt / spoolLag);
            else            turboSpool = std::max(0.0f, turboSpool - fdt * 2.5f);
            if (prevSpool > 0.55f && thr < 0.2f) {
                audio->playSound2D(engineSnd, 0.45f, 4.2f);   // blowoff psshh
                turboSpool = 0.0f;
            }
            prevSpool = turboSpool;
            if (!turboLoop.valid()) turboLoop = audio->startLoop(engineSnd, 0.0f, 3.0f);
            if (turboLoop.valid())
                audio->setLoopParams(turboLoop, turboSpool * 0.18f, 3.0f + 1.2f * turboSpool);

            // (tire squeal removed — the synthesized tone read as a DJ effect;
            //  a real squeal needs a noise-based sample, not a sine sweep)
        }

        // Chase camera.
        const float dx = std::cos(camPitch) * std::cos(camYaw);
        const float dy = std::sin(camPitch);
        const float dz = std::cos(camPitch) * std::sin(camYaw);
        // CHASE-CAM COLLISION ("clipping"). The camera was pure trigonometry with
        // no collision query at all, so it swung straight through the tunnel
        // shell, the cutting walls and the terrain — you could look at the bore
        // from inside the rock. Tim, 2026-08-14: "The Tunnel... should also have
        // clipping" / "looking under the ground makes the asphalt disappear".
        //
        // Cast from the car's head position out along the boom; if anything solid
        // is in the way, pull the camera in to just short of it. Static mask, so
        // the world stops the camera but the car itself and loose props do not.
        // cam_collide 0 disables it (console cvar, see below).
        const float back = 9.0f;
        float cx = vp[0] - dx * back, cy = vp[1] + 3.2f - dy * back, cz = vp[2] - dz * back;

        // CAMERA vs WORLD. Two DIFFERENT rules, because they want different
        // behavior — the first cut used the wall rule for both and Tim
        // (2026-08-14) reported "camera Cannot go down to see under the car
        // anymore.. we need to clamp it AT the ground, but not UNDER the ground."
        //
        // 1) GROUND: do NOT shorten the boom. Keep the full 9 m and just refuse to
        //    go below the surface — the camera SLIDES along the ground, so you can
        //    still pitch right down and look up at the car from grass level. This
        //    is the "clamp at the ground" most games do.
        {
            const float gy = x3::game::terrainHeightAtWorld(cx, cz);
            const float kGroundClear = 0.35f;               // keep the near plane out of the dirt
            // ONLY CLAMP WHEN THE GROUND IS ACTUALLY BELOW YOU.
            // Inside the bore the height field at the camera's XZ is the MOUNTAIN
            // ROOF — a hundred-odd meters up — so an unconditional "stay above
            // the terrain" rule fired the camera straight into the rock. Tim,
            // 2026-08-15, sent a shot from inside the mountain looking at the
            // underside of the world.
            // Under cover the surface overhead is a CEILING, not a floor, and the
            // wall raycast below is the right constraint. Test against the CAR's
            // height, not the camera's: the car is on the carriageway by
            // definition, so terrain far above it means we are in the tunnel or a
            // deep cutting.
            const bool underCover = gy > vp[1] + 2.0f;
            if (!underCover && cy < gy + kGroundClear) cy = gy + kGroundClear;
        }
        // 2) WALLS: a raycast DOES shorten the boom, so the shell, the cutting
        //    faces and the headwall still stop the camera instead of letting it
        //    swim through into the rock. Cast to the ground-clamped position so a
        //    low angle is not mistaken for a wall hit.
        {
            const float pivotY = vp[1] + 1.4f;              // roughly the roof line
            float ox = cx - vp[0], oy = cy - pivotY, oz = cz - vp[2];
            const float len = std::sqrt(ox*ox + oy*oy + oz*oz);
            if (len > 0.05f) {
                ox /= len; oy /= len; oz /= len;
                const x3::phys::RayHit h = phys->rayCast(
                    x3::phys::Vec3{ vp[0], pivotY, vp[2] },
                    x3::phys::Vec3{ ox, oy, oz }, len, x3::phys::Layer::Static);
                if (h.hit) {
                    const float kSkin = 0.45f;
                    const float d = std::max(1.6f, h.distance - kSkin);
                    cx = vp[0] + ox * d; cy = pivotY + oy * d; cz = vp[2] + oz * d;
                    // Re-assert the ground rule after pulling in — the shortened
                    // point can still land under a rise.
                    const float gy2 = x3::game::terrainHeightAtWorld(cx, cz);
                    if (gy2 <= vp[1] + 2.0f && cy < gy2 + 0.35f) cy = gy2 + 0.35f;
                }
            }
        }

        // The listener IS the chase camera, so the note pans and attenuates as
        // you orbit the car and swells correctly inside the bore.
        if (audioOn) audio->setListener(cx, cy, cz, camYaw, camPitch);

        int cw, ch; glfwGetFramebufferSize(window, &cw, &ch);
        if (cw != lastW || ch != lastH) { lastW = cw; lastH = ch; if (cw > 0 && ch > 0) device->onResize((uint32_t)cw, (uint32_t)ch); }
        // MERGED NEAREST-K TUNNEL LIGHTS, keyed on the camera we are about to
        // render from — not this bore's whole array. A dressed bore spends 8
        // real lights (6 down the barrel + 1 per mouth); four city bores would
        // take 32 and eight network bores 64, the entire legacy budget. You can
        // only be inside one tunnel, so upload the nearest K and let the rest
        // cost nothing. Per-frame, so it also cannot go stale — which is the
        // other half of the "lit in headless capture, black when driven" bug.
        { const float cp[3] = { cx, cy, cz };
          x3::game::uploadTunnelLights(*device, cp); }
        // SPEED FOV. Physical speed alone does not read as fast on a screen —
        // the frame has to widen and the periphery has to rush. 72 deg parked ->
        // 88 flat out, eased so it swells under acceleration instead of snapping.
        // This is the 1990s arcade trick and it is still the highest
        // feel-per-line change available (see TUNNEL_NEXT.md section 2 on NFS).
        {
            const float sp   = carBuilt ? std::fabs(car.forwardSpeed()) : 0.0f;
            const float t    = std::min(1.0f, sp / 55.0f);       // ~123 mph = full
            const float want = 72.0f + 16.0f * t * t;            // eased, not linear
            static float fovNow = 72.0f;
            fovNow += (want - fovNow) * std::min(1.0f, fdt * 3.0f);   // smooth
            // ON FOOT the camera IS the player's eye, not a chase rig pulled back
            // off a capsule. The speed-eased FOV above belongs to driving and is
            // deliberately dropped here: a walking FOV that breathes with your
            // pace is nauseating. Both modes still land on ONE setCamera, so the
            // precipitation volume and the sky-visibility ray follow the eye
            // without a second code path to keep in step.
            if (!driving && footSpawned) {
                float ex, ey, ez, fyaw = 0.0f, fpit = 0.0f;
                onFoot.camera(ex, ey, ez, fyaw, fpit);
                camYaw = fyaw; camPitch = fpit;
                // OVER THE SHOULDER. Pulled back along the look vector and offset
                // to the right, the way every third-person game frames a walking
                // character -- dead-centre behind the head means the body hides
                // exactly what you are walking toward.
                const float cp = std::cos(fpit), sp2 = std::sin(fpit);
                const float fx = cp * std::cos(fyaw), fy2 = sp2, fz = cp * std::sin(fyaw);
                const float rx = -std::sin(fyaw), rz = std::cos(fyaw);
                const float back = 3.1f, shoulder = 0.55f;
                cx = ex - fx * back + rx * shoulder;
                cy = ey - fy2 * back + 0.35f;
                cz = ez - fz * back + rz * shoulder;
                device->setCamera(cx, cy, cz, camYaw, camPitch, 74.0f);
            } else {
                device->setCamera(cx, cy, cz, camYaw, camPitch, fovNow);
            }
            if (weatherOn)
                precip.update(fdt, precipKind, precipAmt, cx, cy, cz, 0.0f, 0.0f,
                              skyVisibleAt(*phys, cx, cy, cz, route.dirX, route.dirZ));
        }
        auto frame = device->beginFrame();
        if (frame.valid) { scene.render(*device, frame); if (carBuilt) car.render(frame); }

        // ---- WHEEL-SPIN FX: spawn skid marks + smoke when the rears slip ----
        if (frame.valid && carBuilt) {
            const float slip = car.maxSlip();
            fxSpawnAcc += fdt;
            if (slip > 0.06f && fxSpawnAcc > 0.03f) {
                fxSpawnAcc = 0.0f;
                // The car's heading NOW — baked into the mark at spawn, so a
                // drift leaves skewed rubber the way the tire actually drew it.
                float cq[4]; phys->getBodyRotation(car.chassis(), cq);
                const float carYawNow = std::atan2(2.0f * (cq[3] * cq[1] + cq[0] * cq[2]),
                                                   1.0f - 2.0f * (cq[1] * cq[1] + cq[0] * cq[0]));
                x3::phys::WheelState ws;
                for (uint32_t i = 0; i < car.controller()->wheelCount(); ++i) {
                    if (!car.controller()->wheelState(i, ws)) continue;
                    if (i < 2) continue;                       // rear wheels only
                    if (!ws.hasContact) continue;              // airborne wheels mark nothing
                    if (fxN < 512) {
                        SpinFx& f = fx[fxN++];
                        f.x = ws.worldTransform[12];
                        // CONTACT PATCH, not hub: worldTransform[13] is the wheel
                        // CENTER, a full radius off the ground — the "tire marks
                        // float" bug in one index.
                        f.y = ws.worldTransform[13] - ws.radius;
                        f.z = ws.worldTransform[14];
                        f.age = 0.0f;
                        f.yaw = carYawNow;
                        f.kind = (slip > 0.18f) ? 1 : 0;       // hard spin -> smoke
                    }
                }
            }
            uint32_t w = 0;
            for (uint32_t i = 0; i < fxN; ++i) {
                SpinFx& f = fx[i];
                f.age += fdt;
                if (f.kind == 0) { if (f.age > 12.0f) continue; }
                else { f.y += fdt * 1.1f; if (f.age > 1.6f) continue; }
                fx[w++] = f;
            }
            fxN = w;
            for (uint32_t i = 0; i < fxN; ++i) {
                SpinFx& f = fx[i];
                const float cy = std::cos(f.yaw), sy = std::sin(f.yaw);
                float col[4] = {1,1,1,0};
                if (f.kind == 0) {
                    // A thin slab lying ON the road, long axis down the heading.
                    const float a = std::max(0.0f, 1.0f - f.age / 12.0f) * 0.70f;
                    col[3] = a;
                    const float sx = 0.22f, sz = 1.1f;
                    const float m[16] = {
                         cy * sx, 0.0f, -sy * sx, 0.0f,
                         0.0f,    0.015f, 0.0f,   0.0f,
                         sy * sz, 0.0f,  cy * sz, 0.0f,
                         f.x, f.y + 0.015f, f.z, 1.0f };
                    device->drawMesh(frame, fxMarkMesh, fxSkidTex, col, m);
                } else {
                    // A soft sphere: born at the contact patch, growing as it
                    // rises, gone in ~1.6 s. Low alpha is what sells vapor.
                    const float t = f.age / 1.6f;
                    col[3] = std::max(0.0f, 1.0f - t) * 0.30f;
                    const float s = 0.30f + 1.1f * t;
                    const float m[16] = {
                        s, 0, 0, 0,  0, s, 0, 0,  0, 0, s, 0,
                        f.x, f.y + 0.25f + 0.3f * t, f.z, 1.0f };
                    device->drawMesh(frame, fxPuffMesh, fxSmokeTex, col, m);
                }
            }
        }
        // ---- INSTRUMENT CLUSTER (textured) ---------------------------------
        // Three drawHudImage calls plus a little text. The dial and the shift
        // gate are real anti-aliased artwork; the needle is a 64-frame rotation
        // atlas indexed by rpm, so the sweep stays clean at every angle.
        // The previous version approximated the dial with ~400 axis-aligned
        // quads because the brief said "rectangles only" — but drawHudImage
        // takes a TEXTURE with UV sub-rects, so the right reading was "put real
        // art IN the rectangle". Owner's verdict on the quad build: "slop in
        // Carbon esque shape". Art pipeline: tools/render_gauge_bezel.py renders
        // the chrome rim in Blender (metal IS reflection — 2D fake gloss never
        // convinces), tools/compose_gauge_dial.py draws the scale over it and
        // bakes the needle atlas, tools/make_gauge_textures.py makes the gate.
        // The dial face carries NO text: the gear digit and the MPH readout
        // below own those two strips, and baked labels collided with them.
        if (frame.valid && carBuilt && texDial.valid()) {
            int fbw = 0, fbh = 0; glfwGetFramebufferSize(window, &fbw, &fbh);
            const float fw = (float)fbw, fh = (float)fbh;
            // LAYOUT. The whole cluster is dial (2R tall) + gap + gate (0.9R),
            // so it needs 3.0R of vertical room; the first pass anchored on the
            // dial alone and pushed the gate and the TC line off the bottom of
            // the screen.
            const float R   = 0.150f * fh;
            const float mar = 0.030f * fh;
            const float gateH = R * 0.90f;
            const float gcx = fw - mar - R;
            const float gcy = fh - mar - gateH - R * 0.12f - R;

            const float rpmNow = car.engineRPM();
            const float frac   = std::min(1.0f, std::max(0.0f, rpmNow / 8000.0f));

            // Framerate-independent needle smoothing — raw rpm buzzes at 165 Hz.
            static float shownFrac = 0.0f;
            shownFrac += (frac - shownFrac) * (1.0f - std::exp(-9.0f * fdt));

            const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
            device->drawHudImage(frame, texDial, gcx - R, gcy - R, 2.0f * R, 2.0f * R, white);

            if (texNeedle.valid()) {
                const int NF = 64, AT = 8;
                int fi = (int)(shownFrac * (NF - 1) + 0.5f);
                fi = fi < 0 ? 0 : (fi > NF - 1 ? NF - 1 : fi);
                const float u0 = (float)(fi % AT) / (float)AT;
                const float v0 = (float)(fi / AT) / (float)AT;
                device->drawHudImage(frame, texNeedle, gcx - R, gcy - R, 2.0f * R, 2.0f * R,
                                     white, u0, v0, u0 + 1.0f / AT, v0 + 1.0f / AT);
            }
            if (texGate.valid()) {
                const float gw = gateH * 2.0f, gh = gateH;
                device->drawHudImage(frame, texGate, gcx - gw * 0.5f,
                                     gcy + R + R * 0.12f, gw, gh, white);
            }

            // FALLING SNOW / RAIN. Submitted here, inside the frame: the device
            // adds no particle pass at all when the count is zero, so clear
            // weather costs literally nothing.
            if (weatherOn) precip.submit(*device, frame);

            // ---- THE E PROMPT. A control nobody can see is a control nobody
            // has: the walkways, doors and rooms are only reachable if the player
            // is told they can get out at all. It CHANGES with range, so walking
            // back to the car is a target rather than a guess.
            {
                uint32_t hw2 = 0, hh2 = 0; device->hudSize(hw2, hh2);
                const char* prompt = nullptr;
                if (driving) prompt = "E  GET OUT";
                else if (footSpawned) {
                    const float dxc = cx - parkedAt[0], dzc = cz - parkedAt[2];
                    prompt = (dxc*dxc + dzc*dzc <= 16.0f) ? "E  GET IN"
                                                          : "WALK BACK TO THE CAR TO DRIVE";
                }
                if (prompt && hw2 && hh2) {
                    const float px = std::floor((float)hh2 * 0.026f);
                    const float tw = (float)std::strlen(prompt) * px;
                    const float tx = ((float)hw2 - tw) * 0.5f, ty = (float)hh2 * 0.86f;
                    const float sh[4]  = { 0.0f, 0.0f, 0.0f, 0.75f };
                    const float fgc[4] = { 1.0f, 0.93f, 0.72f, 1.0f };
                    device->drawHudText(frame, prompt, tx + 1.0f, ty + 1.0f, px, sh);
                    device->drawHudText(frame, prompt, tx, ty, px, fgc);
                }
            }

            // ---- DRAW JAKE, at the capsule's feet, facing where he walks.
            // The rig is authored +Z forward and the camera convention is
            // yaw about +Y from +X, so the model yaw is (camYaw + 90 deg).
            if (!driving && footSpawned && !jakeDraw.empty()) {
                const x3::phys::Vec3 ft = onFoot.feet();
                const float a = camYaw + 1.5707963f;
                const float ca = std::cos(a), sa = std::sin(a);
                // Column-major 4x4: rotation about +Y, translation at the feet.
                const float world[16] = {
                     ca, 0.0f, -sa, 0.0f,
                   0.0f, 1.0f, 0.0f, 0.0f,
                     sa, 0.0f,  ca, 0.0f,
                   ft.x, ft.y, ft.z, 1.0f
                };
                for (const x3::asset::ModelDrawable& d : jakeDraw) {
                    const float bc[4] = { d.baseColorFactor[0], d.baseColorFactor[1],
                                          d.baseColorFactor[2], d.baseColorFactor[3] };
                    const float emis[3] = { d.emissiveFactor[0], d.emissiveFactor[1],
                                            d.emissiveFactor[2] };
                    device->drawMeshPBR(frame,
                        x3::rhi::MeshHandle{ d.meshId },
                        x3::rhi::TextureHandle{ d.baseColorTexId },
                        x3::rhi::TextureHandle{ d.normalTexId },
                        x3::rhi::TextureHandle{ d.mrTexId },
                        bc, emis, world, d.alphaMask, d.alphaBlend,
                        x3::rhi::TextureHandle{ d.emissiveTexId },
                        x3::rhi::TextureHandle{ d.detailTexId }, d.detailUvScale,
                        d.clearcoat, d.clearcoatRough);
                }
            }

            // (The old hud.drawConsole call is gone — shell.draw at the end of
            // the frame owns the console panel now.)

            // ---- BOOST GAUGE ----------------------------------------------
            // The ROUND dial, left of the tach at 0.70 of its radius — the
            // secondary instrument, not a second primary. Sunday's build
            // replaced this with a gray segmented bar; the dial art (same
            // Blender bezel and needle atlas as the tach, same sweep, so
            // frame i points at the same angle on both faces) was already in
            // assets/ui and reads as an instrument where the bar read as UI.
            //
            // It reads NEGATIVE off-throttle. A boost gauge pinned at zero
            // whenever you lift is the tell that no manifold model is behind
            // it, and vacuum is where a real one lives most of the time.
            if (texBoost.valid()) {
                const float R2  = R * 0.70f;
                const float bcx = gcx - R - R2 - R * 0.10f;
                const float bcy = gcy + R - R2;              // bottoms line up

                constexpr float kPsiMin = -10.0f, kPsiMax = 20.0f;   // == the art
                const float psi = car.boostPsi();
                const float bf  = std::min(1.0f, std::max(0.0f,
                                    (psi - kPsiMin) / (kPsiMax - kPsiMin)));

                static float shownBoost = 0.0f;
                shownBoost += (bf - shownBoost) * (1.0f - std::exp(-12.0f * fdt));

                device->drawHudImage(frame, texBoost, bcx - R2, bcy - R2,
                                     2.0f * R2, 2.0f * R2, white);
                if (texNeedle.valid()) {
                    const int NF = 64, AT = 8;
                    int bi = (int)(shownBoost * (NF - 1) + 0.5f);
                    bi = bi < 0 ? 0 : (bi > NF - 1 ? NF - 1 : bi);
                    const float u0 = (float)(bi % AT) / (float)AT;
                    const float v0 = (float)(bi / AT) / (float)AT;
                    device->drawHudImage(frame, texNeedle, bcx - R2, bcy - R2,
                                         2.0f * R2, 2.0f * R2, white,
                                         u0, v0, u0 + 1.0f / AT, v0 + 1.0f / AT);
                }
                char bbuf[32];
                std::snprintf(bbuf, sizeof(bbuf), "%+.1f", (double)psi);
                const float bp = R2 * 0.26f;
                const float bw = (float)std::strlen(bbuf) * bp;
                const bool  over = psi >= 16.0f;
                const float bc[4] = { over ? 1.0f : 0.97f, over ? 0.32f : 0.98f,
                                      over ? 0.24f : 1.0f, 1.0f };
                device->drawHudText(frame, bbuf, bcx - bw * 0.5f,
                                    bcy + R2 * 0.26f, bp, bc);
            }

            // THE THERMOMETER. Only when weather is running: a gauge pinned at
            // a constant is worse than no gauge, because it teaches the player
            // to stop looking at it.
            if (weatherOn) {
                x3::game::drawThermometer(
                    *device, frame, weather.sample().tempF(),
                    x3::game::surfaceConditionName(wetness.condition()),
                    wetness.snowDepthIn(),
                    wetness.condition() == x3::game::SurfaceCondition::Ice);
            }

            char gbuf[64];
            const int   gnum = car.gear();
            const float mph  = std::fabs(car.forwardSpeed()) * 2.23694f;

            std::snprintf(gbuf, sizeof(gbuf), "%d", (int)(mph + 0.5f));
            {
                // 0.275R, not 0.34R: at three digits the wider face ran into the
                // "0" and "8" numerals, which sit at x = +-0.455R.
                const float px = R * 0.275f;
                const float w  = (float)std::strlen(gbuf) * px;
                const float col[4] = { 0.97f, 0.98f, 1.0f, 1.0f };
                device->drawHudText(frame, gbuf, gcx - w * 0.5f, gcy + R * 0.235f, px, col);
                const float lp = R * 0.095f;
                const float lc[4] = { 0.35f, 0.78f, 0.95f, 1.0f };   // cyan, per the reference
                device->drawHudText(frame, "MPH", gcx - 1.5f * lp, gcy + R * 0.55f, lp, lc);
            }
            {
                const char* gs = (gnum < 0) ? "R" : (gnum == 0 ? "N" : "123456" + ((gnum - 1) % 6));
                char one[2] = { gs[0], 0 };
                const bool hot = rpmNow > 7312.0f * 0.985f;
                const float px = R * 0.22f;
                const float col[4] = { hot ? 1.0f : 0.35f, hot ? 0.30f : 0.82f,
                                       hot ? 0.22f : 0.98f, 1.0f };
                device->drawHudText(frame, one, gcx - px * 0.5f, gcy - R * 0.46f, px, col);
            }
            {   // shift lights along the top of the bezel
                const int   NL = 8;
                const float lw = R * 0.115f, lh = R * 0.052f, gp = lw * 0.30f;
                const float tot = NL * lw + (NL - 1) * gp;
                const float x0 = gcx - tot * 0.5f, y0 = gcy - R * 1.17f;
                const float lit = std::min(1.0f, std::max(0.0f, (rpmNow - 6000.0f) / 1312.0f));
                const bool  fl  = rpmNow >= 7312.0f && std::fmod((float)now * 4.5f, 1.0f) < 0.5f;
                for (int i = 0; i < NL; ++i) {
                    const bool on = lit >= (float)(i + 1) / (float)NL || fl;
                    const float tt = (float)i / (float)(NL - 1);
                    float c4[4];
                    if (fl)       { c4[0]=1.0f; c4[1]=0.16f; c4[2]=0.12f; c4[3]=1.0f; }
                    else if (!on) { c4[0]=0.12f; c4[1]=0.14f; c4[2]=0.18f; c4[3]=0.8f; }
                    else          { c4[0]=0.25f+0.75f*tt; c4[1]=0.85f-0.58f*tt;
                                    c4[2]=0.98f-0.84f*tt; c4[3]=1.0f; }
                    device->drawHudQuad(frame, x0 + i * (lw + gp), y0, lw, lh, c4);
                }
            }
            {   // Key hints on the glass. A binding nobody can see does not
                // exist: T toggled traction control for a whole session while
                // the only mention of it went to a log file.
                const float hp = R * 0.085f;
                const float hcol[4] = { 0.52f, 0.57f, 0.66f, 1.0f };
                device->drawHudText(frame, "~  CONSOLE",      gcx - R * 0.95f,
                                    gcy - R * 1.64f, hp, hcol);
                device->drawHudText(frame, "T  TRACTION",     gcx - R * 0.95f,
                                    gcy - R * 1.52f, hp, hcol);
                device->drawHudText(frame, "C  CLIMB",        gcx - R * 0.95f,
                                    gcy - R * 1.76f, hp, hcol);
                device->drawHudText(frame, "SPACE  HANDBRAKE", gcx - R * 0.95f,
                                    gcy - R * 1.40f, hp, hcol);
            }
            {
                const bool tcOn = car.tractionControl();
                const float px = R * 0.105f;
                const float c4[4] = { tcOn ? 0.35f : 1.0f, tcOn ? 0.78f : 0.58f,
                                      tcOn ? 0.95f : 0.20f, 1.0f };
                const char* t = car.climbMode() ? "CLIMB" : (tcOn ? "TC" : "TC OFF");
                device->drawHudText(frame, t, gcx - (float)std::strlen(t) * px * 0.5f,
                                    gcy - R * 1.30f, px, c4);
            }
        }
        shell.draw(frame, fdt);      // console + FPS/stats, over everything
        device->endFrame(frame);
    }

    tunnel.shutdown(*device, *phys);
    for (auto& w : tourBores) w->shutdown(*device, *phys);
    x3::game::shutdownTunnelSurfaces(*device);   // shared sets, released once
    streamer.shutdown(scene, *device, *phys);
    jobs->shutdown(); phys->shutdown(); device->shutdown();
    if (window) glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

}} // namespace x3::apphost
