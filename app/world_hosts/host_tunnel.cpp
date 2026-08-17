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
#include "host_menu.h"                   // W-MENU: ESC game menu + F4 weather / F5 lighting panels
#include "../engine_console.h"           // registerEngineConsoleCVars — the X3_SHOT_UI staged console
#include "engine/core/IJobSystem.h"
#include "engine/physics/IVehicle.h"
#include "../scene.h"
#include "../terrain.h"
#include "../tunnel_corridor.h"
#include "../road_trees.h"
#include "../town.h"
#include "../forest.h"                   // W-FOREST — the sketch's brown regions
#include "../tunnel_fitout.h"
#include "../tunnel_rooms.h"
#include "../player.h"
#include "../character_anim.h"           // AnimatedCharacter — Jake's shared rig runtime
#include "../weapon.h"                   // Arsenal — the campaign's data-driven weapon core (REUSED)
#include "../fx.h"                       // CombatFx — tracers/muzzle/impact/boom (REUSED)
#include "../thirdperson.h"              // kJakeHandBone + TpGrip table + tpComposeGrip (REUSED)

#include <array>
#include <memory>
#include "../road_network.h"
#include "../summit_lot.h"       // the pad the summit spur climbs to
#include "../ridge_road.h"       // the dirt road along the tops, lot -> the bore's massif
#include "../river_bridge.h"
#include "../river_life.h"       // W-RIVER — fish + AI speedboats on the reach
#include "../traffic.h"          // W-TRAFFIC — AI traffic on the 16-lane freeway
#include "../gas_station.h"      // W-STATIONS — forecourts + the fuel stub
#include "../factory.h"          // W-FACTORY — the Glimvale Works + the golden tickets
#include "../vehicle.h"
#include "../mesh_prims.h"
#include "../asset_root.h"
#include "engine/audio/IAudioSystem.h"   // ENGINE NOTE: RPM-driven loop
#include "../engine_note.h"              // ENGINE NOTE v2: the multi-RPM bank
#include "../weather.h"
#include "../wetness.h"
#include "../storm.h"
#include "../precip_fx.h"
#include "../hud.h"
#include "../world_map.h"        // the M map: camera/waypoint/screen (host_streamed's system)
#include "../input_globals.h"    // g_weaponScroll + scrollCallback -> map wheel zoom
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
#include <functional>  // std::function — the weapons-proof per-frame hook
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

// THE GAUGE-CLUSTER ANCHOR, in ONE place. The interactive HUD and the headless
// proof capture both put the fuel bar under the tach; two copies of this
// arithmetic is the drift that makes a proof shot stop proving anything
// (NO_SLOP rule 4). dial (2R tall) + gap + gate (0.9R) = 3.0R of vertical room.
static void gaugeClusterAnchor(float fw, float fh, float& R, float& gcx, float& gcy) {
    R = 0.150f * fh;
    const float mar = 0.030f * fh;
    const float gateH = R * 0.90f;
    gcx = fw - mar - R;
    gcy = fh - mar - gateH - R * 0.12f - R;
}

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

// ===========================================================================
// THE SKY IS ONE VALUE (NO_SLOP 4). Three sites in this host set SkyParams —
// the boot sky, the HEADLESS capture loop and the interactive loop — and they
// had drifted. The storm branch (cover floor 0.94, the exposure crush, the
// sun-intensity cut) lived ONLY in the interactive loop, so every storm PROOF
// SHOT rendered a cover-0.76 mid-grey deck under a full-brightness sky while
// the played game showed the near-black one: the screenshots were telling a
// different story than the build, which is the exact defect the capture loop's
// own weather comment was written about. Both mappings live here now.
// Receipt: shots_clouds/storm_01_sky.png + storm_02_ground.png (this commit)
// against shots_clouds/before_storm_01_sky.png (the divergent pair).
// ===========================================================================

// Push the sky. The SKYLIGHT that comes with it needs no code here, and that
// is worth writing down because the obvious "fix" is a trap:
//
// COVER DIMS THE FILL, NOT JUST THE SUN — mesh.frag's cloudShadowFactor
// (task #27) takes the DIRECT sun away per-fragment, but the other half of an
// overcast day is that the sky stops being a bright blue dome and stops
// filling the shadows. The engine ALREADY does that half: setSkyParams marks
// the IBL environment dirty (VulkanRenderDevice.cpp — `if (memcmp(&m_sky,...))
// m_iblDirty = true`) and the probe rebakes FROM THE SKY, deck and all, so a
// near-black storm deck bakes a near-black fill.
//
// The trap: setAmbient() looks like the dial and is DEAD in this world —
// mesh.frag's iblAmbient() uses the baked environment whenever one is valid
// and only falls back to the flat `ambient` constant when it is not. MEASURED
// (a scale of 0.74 pushed through setAmbient at cover 0.75): tunnel-portal
// interior 138.54 -> 138.50, shaded grass 155.61 -> 155.61, road 53.64 ->
// 53.64. Nothing. The same probes DO move when the deck itself goes dark
// (cover 1.0: portal 123.55, grass 109.33, road 35.25) — that is the IBL
// rebake, already working. NO_SLOP 1: the wheel was in the engine.
static void applySky(x3::rhi::IRenderDevice& dev,
                     const x3::rhi::IRenderDevice::SkyParams& sp) {
    dev.setSkyParams(sp);
}

// The NO-WEATHER demo sky — ONE builder for its three consumers (the boot
// push, the `wx off` transition, and the F4 panel's live cloud/time refresh).
// Three hand copies of these constants is exactly the drift skyFromWeather's
// header block documents (NO_SLOP rule 4). X3_CLOUD still overrides the cover
// for the cloud-pass perf A/Bs.
static x3::rhi::IRenderDevice::SkyParams tunnelDemoSky() {
    x3::rhi::IRenderDevice::SkyParams sp{};
    sp.enabled = true;
    sp.sunDir[0] = 0.35f; sp.sunDir[1] = 0.92f; sp.sunDir[2] = 0.18f;
    sp.sunColor[0] = 1.0f; sp.sunColor[1] = 0.97f; sp.sunColor[2] = 0.92f;
    sp.sunIntensity = 1.0f; sp.haze = 0.35f; sp.exposure = 1.0f;
    // Scattered fair-weather cumulus. 0 would be the old clear sky exactly.
    sp.cloud = 0.42f;
    if (const char* cv = std::getenv("X3_CLOUD"))
        sp.cloud = std::min(1.0f, std::max(0.0f, (float)std::atof(cv)));
    return sp;
}

// Weather sample -> sky. `flash` is StormSystem::flash() (0 outside a strike).
static x3::rhi::IRenderDevice::SkyParams skyFromWeather(
        const x3::game::WeatherSample& ws, float flash) {
    x3::rhi::IRenderDevice::SkyParams sp = ws.sky;
    sp.enabled = true;
    sp.sunDir[0] = 0.35f; sp.sunDir[1] = 0.92f; sp.sunDir[2] = 0.18f;
    // Cloud cover tracks the haze the state already asked for, so an overcast
    // sky is actually overcast instead of clear-with-fog.
    sp.cloud    = 0.15f + 0.85f * ws.fogDensity;
    // The storm FLASH rides on exposure rather than on the sun: a strike lights
    // the whole cloud deck from inside, so raising the sun would throw hard
    // directional shadows from a light source that is not there.
    sp.exposure = ws.sky.exposure + flash;
    // sunIntensity is the SKY DISK + glow only (IRenderDevice.h) — cutting it
    // keeps a hot disk from punching through an overcast, and costs the ground
    // nothing (that is cloudShadowFactor's job).
    sp.sunIntensity = sp.sunIntensity * (1.0f - 0.65f * std::min(1.0f, sp.cloud));
    if (ws.state == x3::game::WeatherState::Storm) {
        // A storm is not 'cloudy with effects' — the deck goes heavy and the
        // light DIES, which is also what makes every lightning flash read
        // (contrast is the flash's whole currency). 0.94 is the cover
        // sky.frag's gloom curve (smoothstep 0.55..0.95) was CALIBRATED to:
        // below it the deck renders mid-grey no matter what the state says.
        sp.cloud    = std::max(sp.cloud, 0.94f);
        sp.exposure = ws.sky.exposure * 0.52f + flash * 1.35f;
    }
    return sp;
}

// ---------------------------------------------------------------------------
// W-MENU: TIME OF DAY -> THE SUN. Applied ONLY once the player has touched
// wx_hour (the F4 TIME slider or the cvar): the boot look stays byte-identical
// to what every reference capture was tuned against, and the moment you ask
// for 19:00 you actually get a horizon sun (same dusk vector family the
// --screenshot-town dusk gate uses: sun low, warm, sky dimmed).
//
// Mapping: daylight runs 6:00 -> 18:00 (elevation = sin over that arc), the
// sun's azimuth swings east -> south -> west across the day. Night clamps the
// disk just above the horizon and takes the light away instead (intensity +
// exposure), because the engine's sky has no moon to hand the frame to.
// ---------------------------------------------------------------------------
static void applyHourSun(x3::rhi::IRenderDevice::SkyParams& sp, float hour) {
    while (hour < 0.0f)  hour += 24.0f;
    while (hour >= 24.0f) hour -= 24.0f;
    const float dayT = (hour - 6.0f) / 12.0f;            // 0 at 06:00, 1 at 18:00
    const float elev = std::sin(dayT * 3.14159265f);     // <0 = night
    // Azimuth: east at dawn (+X), south mid-day, west at dusk (-X); a gentle
    // +Z lean keeps noon shadows from collapsing straight down.
    const float az = 3.14159265f * (1.0f - std::min(1.0f, std::max(0.0f, dayT)));
    const float horiz = std::cos(std::max(0.055f, elev));
    sp.sunDir[0] = std::cos(az) * horiz;
    sp.sunDir[1] = std::max(0.055f, elev);               // never below the horizon line
    sp.sunDir[2] = 0.33f * horiz;
    // Light follows elevation: full at noon, warm/dim at the horizon, mostly
    // gone at night. Exposure carries the night half (no moon to switch to).
    const float dayLight = std::min(1.0f, std::max(0.0f, elev * 2.2f));
    sp.sunIntensity *= 0.06f + 0.94f * dayLight;
    if (elev < 0.12f) {                                  // dusk/dawn warmth
        sp.sunColor[0] = 1.0f; sp.sunColor[1] = 0.72f; sp.sunColor[2] = 0.48f;
    }
    const float night = std::min(1.0f, std::max(0.0f, -elev * 2.0f));
    sp.exposure *= 1.0f - 0.72f * night;
}

// ---------------------------------------------------------------------------
// JAKE'S RIFLE — the tunnel world's one-weapon roster, resolved through the
// campaign's data-driven Arsenal (app/weapon.h, REUSED — grep-first receipt:
// the ammo/mag/reload/cooldown/spread/hitscan machinery and the third-person
// drawCurrentAt hand-socket draw all already existed there; this host adds
// only data + key bindings). Values seeded from the campaign "smg" row in
// weapon.cpp::makeDefaultRoster() — the SAME WeaponRailgun.glb longarm, with
// its MEASURED barrel tip (tools/weapon_muzzle_probe.py) carried over.
// ---------------------------------------------------------------------------
static std::vector<x3::game::WeaponDef> tunnelRifleRoster() {
    x3::game::WeaponDef w;
    w.name        = "rifle";                    // grip row: kTpGripTable "rifle"
    w.kind        = x3::game::FireKind::Hitscan;
    w.automatic   = true;
    w.damage      = 11;
    w.type        = x3::DamageType::Kinetic;
    w.fireRate    = 9.0f;                       // ~540 rpm — a rifle, not a minigun
    w.pellets     = 1;
    w.spreadDeg   = 1.4f;
    w.recoilDeg   = 0.5f;
    w.range       = 120.0f;                     // open-world sightlines (the campaign's 60 m is corridor-scaled)
    w.magSize     = 30;
    w.reserveAmmo = 150;
    // PAIRED VALUE (NO_SLOP rule 4): reloadTime == the Jake_44 "Reloading" clip
    // duration (3.29 s, measured — see jakeClipTable()'s combat block). If the
    // clip is ever re-baked, this number moves with it, or the mag refills
    // while the hands are still working the receiver.
    w.reloadTime  = 3.29f;
    w.viewmodelGlb= "WeaponRailgun.glb";        // PBR-textured longarm (store-served)
    w.vmScale     = 0.24f;
    w.vmMuzzle    = { 0.0f, 0.494f, 0.909f };   // MEASURED barrel tip (weapon.cpp smg row)
    w.muzzleFx    = "muzzle_smg";
    w.impactFx    = "impact_bullet";
    return { w };
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
    std::vector<float> ringRoadY;   // graded datum per ring node — the connector
                                    // pins its landing to it, and the ring ribbon
                                    // rides it (load-bearing where the connector's
                                    // own carve crosses under the ring pavement)
    bool ringOn = false;
    {
        const char* e = std::getenv("X3_RING");
        ringOn = !(e && e[0] == '0');   // DEFAULT ON — X3_RING=0 to disable
        if (ringOn) {
            // The COURSE, not a circle — Tim, from the world map: "its a
            // perfect circle. NO roads do that." makeInnerCourse() is the
            // authored leg list (straights, arcs, S-weaves, the foothill
            // bulge), with its junction straight through the old landing.
            ringSpec = x3::game::makeInnerCourse();
            const x3::game::RoadBuildResult rr = x3::game::registerRoad(ringSpec, &ringRoadY);
            if (!rr.ok) { x3::logError("--world tunnel: ring registration FAILED"); ringOn = false; }
        }
    }
    // THE 31-MILE OUTER TOUR — the four-range loop with its five bores — and
    // THE RIVER CROSSING (X3_RIVER_ROAD=1) — the valley road over Bridge No.1.
    // Registered in the same boot slot, for the same reason: the corridor
    // registry closes at the first height query below.
    x3::game::OuterRingResult outerRing;
    bool outerOn = false;
    {
        const char* e = std::getenv("X3_OUTER_RING");
        // DEFAULT ON again, 2026-08-16. It was switched OFF because one of the
        // five bores was said to build a kilometre-scale floating shell tower
        // over the spawn country. The AABB instrumentation added to
        // TunnelCorridorWorld::build says it was not one bore, it was ALL FIVE,
        // and the cause was not the dressing's arithmetic: build()'s frameAt
        // lambda was still reading the file-scope DEMO tunnel constants
        // (kRouteCX/kRouteDirX/kRouteHalfLen) left over from the one-tunnel era.
        // Every tour bore therefore laid its ribbon, shell and portals on the
        // demo axis over spawn while its CUTTING (which goes through
        // route.worldAt) landed correctly 7 km out on its own chord — and the
        // quads bridging the two stretched across the gap. Measured X extents
        // 3.1-7.1 km; after the fix each bore's AABB sits on its own chord.
        // build() now also REFUSES any bore whose frame strays >150 m from its
        // own spine, so this cannot come back silently.
        //
        // Still true, and still open: the tour is an ISLAND — 2,958 m from the
        // nearest inner-ring point, with no connector yet. X3_OUTER_RING=0 to
        // switch it off.
        outerOn = !(e && e[0] == '0');
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
        riverOn = !(e && e[0] == '0');   // DEFAULT ON — X3_RIVER_ROAD=0 to disable
        if (riverOn) {
            // The ring goes along so both leg ends LAND on it at grade —
            // junction machinery, not stacked pavements (owner: "This is so
            // bad.. at least swoop curves down to it").
            riverRoad = x3::game::registerRiverRoad(
                ringOn ? &ringSpec : nullptr,
                ringOn ? &ringRoadY : nullptr);
            if (!riverRoad.road.ok) {
                x3::logError("--world tunnel: river road registration FAILED");
                riverOn = false;
            }
        }
    }
    // THE SPAWN CONNECTOR (X3_CONNECTOR=0 to disable) — the road the spawn
    // corridor was missing: measured 3,522 m of nothing between the exit portal
    // and the inner tour. Registered LAST among the roads so its junction pins
    // read the ring's graded datum and its natural sweep reads every carve
    // already in. THE SUMMIT SPUR rides on it ("roads that go UP on top of the
    // mountain") — skipped honestly if no peak within reach earns a road.
    x3::game::SpawnConnectorResult connector;
    x3::game::SummitSpurResult summitSpur;
    x3::game::SummitLotResult  summitLot;
    x3::game::RidgeRoadResult  ridgeRoad;
    x3::game::RangeCircuitResult rangeCircuit;
    bool connOn = false, circuitOn = false;
    {
        const char* e = std::getenv("X3_CONNECTOR");
        connOn = ringOn && !(e && e[0] == '0');   // needs a ring to land on
        if (connOn) {
            connector = x3::game::registerSpawnConnector(route, ringSpec, ringRoadY);
            if (!connector.road.ok) {
                x3::logError("--world tunnel: spawn connector registration FAILED");
                connOn = false;
            } else {
                // THE RANGE CIRCUIT (X3_CIRCUIT=0 to disable) — Tim: "31 miles
                // may be way too long. we need a 3-5 mile track around the
                // range in addition." Registered BEFORE the spur so the spur's
                // peak search has to stay off it.
                const char* ce = std::getenv("X3_CIRCUIT");
                circuitOn = !(ce && ce[0] == '0');   // DEFAULT ON
                if (circuitOn) {
                    std::vector<const x3::game::RoadSpec*> avoidC{ &ringSpec };
                    if (outerOn) avoidC.push_back(&outerRing.spec);
                    if (riverOn) avoidC.push_back(&riverRoad.spec);
                    rangeCircuit = x3::game::registerRangeCircuit(connector.spec,
                                                                  connector.roadY,
                                                                  &route, &avoidC);
                    circuitOn = rangeCircuit.built;
                    if (!rangeCircuit.built)
                        x3::logWarn("--world tunnel: range circuit not built");
                }
                // Spur off the connector if its country has a mountain; the
                // measured answer is it does not (rolling lowland), so it falls
                // back to the RING, which skirts the ranges. Either way it must
                // stay off every other registered route's centreline.
                std::vector<const x3::game::RoadSpec*> avoid;
                avoid.push_back(&connector.spec);
                if (outerOn) avoid.push_back(&outerRing.spec);
                if (riverOn) avoid.push_back(&riverRoad.spec);
                if (circuitOn) {
                    avoid.push_back(&rangeCircuit.spec);
                    avoid.push_back(&rangeCircuit.accessSpec);
                }
                summitSpur = x3::game::registerSummitSpur(connector.spec,
                                                          connector.roadY, &route, &avoid);
                if (!summitSpur.built)
                    summitSpur = x3::game::registerSummitSpur(ringSpec, ringRoadY,
                                                              &route, &avoid);
                // THE PLACE THE SPUR GOES TO. Without this the spur is 1.4
                // miles of switchback that ends on a bare hillside — the sketch
                // (ROAD_NETWORK_SKETCH_V2.png) labels that high ground "Parking
                // Lot on Top of Mountain". Registered HERE, at boot, because
                // the pad is a TerrainCorridor carve and the registry closes at
                // the first TerrainStreamer::init() (terrain.h contract); and
                // AFTER the spur because its datum is the spur's last node.
                if (summitSpur.built) {
                    summitLot = x3::game::registerSummitLot(summitSpur);
                    // THE LONG LEG OF THE LOOP — Tim's dirt road over the tops,
                    // lot -> the bore's portal shoulder. Registered here for the
                    // same reason the lot is: the carve registry closes at the
                    // first TerrainStreamer::init(), and this needs the lot (one
                    // end) and the bore (the other) to already exist.
                    // DEFAULT OFF, and that is a statement about the road, not
                    // about the knob. NO_SLOP rule 6 says features ship on; this
                    // one is NOT FINISHED and shipping it on would put a 773 ft
                    // trench across the map. In the live world (more routes
                    // registered than the gate's, so a different line) the
                    // deepest cut lands at mile 4.72 at (-520, 7) — see
                    // ridge_road.h's tuning log and the switchback fix it points
                    // at. Turn it on with X3_RIDGE_ROAD=1 to work on it; turn it
                    // on by default the day --test-ridgeroad is 7/7.
                    const char* rre = std::getenv("X3_RIDGE_ROAD");
                    if (summitLot.built && rre && rre[0] == '1') {
                        std::vector<const x3::game::RoadSpec*> rrAvoid = avoid;
                        rrAvoid.push_back(&ringSpec);
                        rrAvoid.push_back(&summitSpur.spec);
                        ridgeRoad = x3::game::registerRidgeRoad(summitLot, route, &rrAvoid);
                        if (!ridgeRoad.built)
                            x3::logWarn(std::string("--world tunnel: ridge road NOT built — ") +
                                        ridgeRoad.whyNot);
                    }
                    if (!summitLot.built) {
                        char lb[192];
                        std::snprintf(lb, sizeof(lb),
                                      "--world tunnel: summit lot NOT built — %s",
                                      summitLot.whyNot);
                        x3::logWarn(lb);
                    }
                }
            }
        }
    }
    // THE OUTER CONNECTOR — the road that stops the 31-mile tour being an
    // island. Registered after BOTH tours so its end pins can read their graded
    // datums, and last of all the roads for the same reason the spawn connector
    // is: its natural sweep then reads every carve already in.
    x3::game::OuterConnectorResult outerConn;
    bool outerConnOn = false;
    {
        const char* e = std::getenv("X3_OUTER_CONNECTOR");
        outerConnOn = ringOn && outerOn && !(e && e[0] == '0');
        if (outerConnOn) {
            outerConn = x3::game::registerOuterConnector(ringSpec, ringRoadY,
                                                         outerRing.spec, outerRing.roadY);
            if (!outerConn.road.ok) {
                x3::logError("--world tunnel: outer connector registration FAILED");
                outerConnOn = false;
            }
        }
    }

    // ==== W-STATIONS — "places for cars to go, to fuel up" ==================
    // Sited from the routes just registered (the freeway's turnaround
    // crossovers, the town approach, the country crossroads), then CARVED here
    // — this is the last stop before the height-query gate closes at
    // TerrainStreamer::init(), and it must also precede buildRoadRibbon()
    // because registerPads() notes each driveway mouth as a road junction and
    // that is what stops planRoadBarriers() laying a jersey wall across it.
    x3::game::GasStationWorld gasStations;
    {
        const char* e = std::getenv("X3_STATIONS");
        if (!(e && e[0] == '0')) {
            gasStations.plan(ringOn ? &ringSpec : nullptr, ringOn ? &ringRoadY : nullptr,
                             riverOn ? &riverRoad.spec : nullptr,
                             riverOn ? &riverRoad.roadY : nullptr,
                             connOn ? &connector.spec : nullptr,
                             connOn ? &connector.roadY : nullptr);
            gasStations.registerPads();
        }
    }

    // THE GLIMVALE WORKS (X3_FACTORY=0 to disable — the flag is for turning it
    // OFF, NO_SLOP rule 6). Planned and its drive registered HERE, in the boot
    // slot, for the reason every producer above is here: app/terrain.h's
    // registry closes at the first height query, and the streamer inits below.
    // LAST of the roads, so the site scoring reads a field with every other
    // carve already in it and can refuse a pad that lands on somebody's road.
    x3::game::FactoryPlan        facPlan;
    x3::game::FactoryDriveResult facDrive;
    bool facOn = false;
    {
        const char* e = std::getenv("X3_FACTORY");
        facOn = ringOn && !(e && e[0] == '0');       // needs a freeway to be seen from
        if (facOn) {
            facPlan = x3::game::planFactoryWorks(ringSpec, ringRoadY);
            if (facPlan.ok) {
                facDrive = x3::game::registerFactoryDrive(facPlan, ringSpec, ringRoadY);
                facOn = facDrive.ok;
            } else {
                facOn = false;
            }
            if (!facOn) x3::logWarn("--world tunnel: the works was not sited — "
                                    "no landmark, no tickets");
        }
    }
    {
        char cb[128];
        std::snprintf(cb, sizeof(cb), "--world tunnel: corridor registry %u of %u used",
                      x3::game::terrainCorridorCount(), x3::game::kMaxTerrainCorridors);
        x3::logInfo(cb);
    }

    // ==== THE MAP'S ROAD LAYER ==============================================
    // 46 miles of road exist above; this is what lets the player FIND them.
    // The routes just registered are handed to WorldMapSystem (host_streamed's
    // M map) as centreline overlays — no new map system, just the geometry the
    // registries already hold. Solid = open road, dashed = a reach something
    // else owns (a tunnel bore, the bridge deck), which is exactly what
    // RoadSpec::gaps and TunnelRoute::boreS0/S1 already record.
    std::vector<x3::game::MapRouteOverlay> mapRoutes;
    {
        // A TunnelRoute spine, sampled at 25 m: solid approach, dashed bore,
        // solid exit. Used for the spawn corridor AND the outer tour's bores.
        auto addTunnelRoute = [&](const x3::game::TunnelRoute& r, const char* nm) {
            auto span = [&](float s0, float s1, bool dashed) {
                if (s1 - s0 < 5.0f) return;
                x3::game::MapRouteOverlay o; o.name = nm; o.dashed = dashed;
                const float step = 25.0f;
                for (float s = s0; ; s += step) {
                    const float sc = std::min(s, s1);
                    float p[3]; r.posAt(sc, p);
                    o.x.push_back(p[0]); o.z.push_back(p[2]);
                    if (sc >= s1) break;
                }
                if (o.x.size() >= 2) mapRoutes.push_back(std::move(o));
            };
            if (r.boreValid) {
                span(0.0f, r.boreS0, false);
                span(r.boreS0, r.boreS1, true);
                span(r.boreS1, r.totalLen, false);
            } else {
                span(0.0f, r.totalLen, false);
            }
        };
        // A RoadSpec centreline: nodes verbatim, split at its gaps so bored /
        // decked reaches draw dashed. Gaps are authored in ascending node order.
        auto addSpec = [&](const x3::game::RoadSpec& sp, const char* nm) {
            const size_t n = std::min(sp.x.size(), sp.z.size());
            if (n < 2) return;
            auto emit = [&](size_t a, size_t b, bool dashed) {
                if (b >= n) b = n - 1;
                if (b <= a) return;
                x3::game::MapRouteOverlay o; o.name = nm; o.dashed = dashed;
                for (size_t k = a; k <= b; ++k) { o.x.push_back(sp.x[k]); o.z.push_back(sp.z[k]); }
                mapRoutes.push_back(std::move(o));
            };
            size_t at = 0;
            for (const x3::game::RoadSpec::Gap& g : sp.gaps) {
                emit(at, g.i0, false);
                emit(g.i0, g.i1, true);
                at = g.i1;
            }
            emit(at, n - 1, false);
        };
        // MERGE UNION (map2 x roads2): the CAPS names are the map's labels —
        // WorldMapSystem::drawRouteLabels draws them verbatim in condensed
        // white caps along the polyline. "SPAWN ROAD" covers BOTH the demo
        // bore's spine AND the paved connector out to the ring (the connector
        // was drivable but never handed to the map until map2 caught it).
        // roads2's circuit + access + outer connector are labeled here too —
        // routes born after map2's snapshot, named in its convention.
        addTunnelRoute(route, "SPAWN ROAD");
        if (connOn) addSpec(connector.spec, "SPAWN ROAD");
        if (connOn && summitSpur.built) addSpec(summitSpur.spec, "SUMMIT SPUR");
        if (ringOn)  addSpec(ringSpec, "INNER TOUR");
        if (outerOn) addSpec(outerRing.spec, "OUTER TOUR");
        if (riverOn) addSpec(riverRoad.spec, "RIVER ROAD");
        if (circuitOn) {
            addSpec(rangeCircuit.spec, "RANGE CIRCUIT");
            addSpec(rangeCircuit.accessSpec, "RANGE CIRCUIT");
        }
        if (facOn) addSpec(facDrive.spec, "WORKS DRIVE");
        char mb[128];
        std::snprintf(mb, sizeof(mb), "[tunnel] map: %u road overlay polyline(s) staged",
                      (uint32_t)mapRoutes.size());
        x3::logInfo(mb);
    }

    // ---- DRIVING-HUD WAYPOINT CHEVRON (map/HUD wiring) ---------------------
    // The map's one waypoint (app/world_map.h) used to be visible only ON the
    // map screen — set it, close the map, and it vanished until you reopened
    // it. worldToScreen the waypoint into the CURRENT frame; when it lands
    // outside a safe screen rect (off-screen, or the projection gives up
    // because it is behind the camera) clamp a small magenta chevron to the
    // screen edge along the bearing to it, with a distance readout. Clears
    // itself inside 30 m — the point where "point me there" becomes "you're
    // here". Defined here (BEFORE both call sites: the interactive per-frame
    // HUD, and the headless map/HUD proof set below) so they render through
    // the exact same code — a screenshot proof of the interactive path, not a
    // parallel copy that can silently drift from it.
    auto drawWaypointChevron = [device](const x3::rhi::FrameContext& fr,
                                        float wpX, float wpY, float wpZ,
                                        float playerX, float playerY, float playerZ,
                                        float camYawNow) {
        (void)wpY; (void)playerY;
        uint32_t hw3 = 0, hh3 = 0; device->hudSize(hw3, hh3);
        if (!hw3 || !hh3) return;
        const float wpDx = wpX - playerX, wpDz = wpZ - playerZ;
        const float distM = std::sqrt(wpDx * wpDx + wpDz * wpDz);
        if (distM <= 30.0f) return;
        const float fw3 = (float)hw3, fh3 = (float)hh3;
        const float cxp = fw3 * 0.5f, cyp = fh3 * 0.46f;
        const float cmargin = 46.0f;
        float sx = 0.0f, sy = 0.0f, ex, ey, ang;
        const bool proj = device->worldToScreen(wpX, playerY, wpZ, sx, sy);
        if (proj) {
            // worldToScreen allows a 1.3x-NDC overscan window before it gives
            // up, so a point just past the edge still lands here — clamp
            // into the safe rect and point outward from it.
            ex = std::min(std::max(sx, cmargin), fw3 - cmargin);
            ey = std::min(std::max(sy, cmargin), fh3 - cmargin);
            ang = std::atan2(ey - cyp, ex - cxp);
        } else {
            // BEHIND the camera: the projection is undefined there, so fall
            // back to the horizontal bearing off the chase-cam yaw (the same
            // forward angle the map's own player arrow reads) mapped onto a
            // compass ring around center.
            const float toWp = std::atan2(wpDz, wpDx);
            float rel = toWp - camYawNow;
            while (rel >  3.14159265f) rel -= 6.28318531f;
            while (rel < -3.14159265f) rel += 6.28318531f;
            const float ringR = std::min(fw3, fh3) * 0.5f - cmargin;
            ex = cxp + std::sin(rel) * ringR;
            ey = cyp - std::cos(rel) * ringR;
            ang = std::atan2(ey - cyp, ex - cxp);
        }
        // The chevron: two short stamped legs forming a ">" pointing outward
        // (the map's own route-line technique — the HUD layer only has
        // axis-aligned quads). Dark halo pass first, then the magenta core —
        // same blip color as the map's waypoint marker. Sized to read at a
        // glance against a busy driving scene (GTA-legibility pass: the first
        // cut's 15 px legs read as a stray mark, not an arrow).
        const float halo[4] = { 0.02f, 0.03f, 0.06f, 0.60f };
        const float mag[4]  = { 1.00f, 0.30f, 0.95f, 1.0f };
        const float legLen = 30.0f;
        for (int passi = 0; passi < 2; ++passi) {
            const float* col = passi == 0 ? halo : mag;
            const float sz  = passi == 0 ? 7.0f : 4.6f;
            for (int leg = -1; leg <= 1; leg += 2) {
                const float la = ang + 2.55f * (float)leg;
                for (int s = 0; s < 11; ++s) {
                    const float t = (float)s / 10.0f;
                    const float qx = ex + std::cos(la) * legLen * t;
                    const float qy = ey + std::sin(la) * legLen * t;
                    device->drawHudQuad(fr, qx - sz * 0.5f, qy - sz * 0.5f, sz, sz, col);
                }
            }
            // A filled dot AT the point — the vertex reads as a single mark
            // even before the eye resolves the two legs (the map's own
            // waypoint blip does the same: a core plus a wider surround).
            const float dotSz = passi == 0 ? 10.0f : 6.0f;
            device->drawHudQuad(fr, ex - dotSz * 0.5f, ey - dotSz * 0.5f, dotSz, dotSz, col);
        }
        char db[24];
        if (distM >= 1000.0f) std::snprintf(db, sizeof(db), "%.1f km", distM / 1000.0f);
        else                  std::snprintf(db, sizeof(db), "%.0f m", distM);
        const float dpx = 17.0f;
        const float dtw = (float)std::strlen(db) * dpx;
        const float sh4[4] = { 0.0f, 0.0f, 0.0f, 0.8f };
        const float wc4[4] = { 1.0f, 0.55f, 0.95f, 1.0f };
        device->drawHudText(fr, db, ex - dtw * 0.5f + 1.5f, ey + 22.0f + 1.5f, dpx, sh4);
        device->drawHudText(fr, db, ex - dtw * 0.5f,        ey + 22.0f,        dpx, wc4);
    };

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
        // this demo exists to expose. X3_CLOUD: dev override for the NO-WEATHER
        // sky's cover (0..1) — the A/B knob the cloud-pass perf receipts are
        // measured with; with X3_WEATHER on, the weather tick owns cover and
        // it is ignored. (Constants live in tunnelDemoSky — ONE builder.)
        applySky(*device, tunnelDemoSky());   // sky + the fill its cover implies
    }
    device->setCameraFar(4000.0f);

    // PER-OBJECT MOTION VECTORS, ON. The "GHOST SHADOW behind him, same as the
    // red car" (Tim) is TAA ghosting: with camera-only reprojection every MOVING
    // object smears its own history trail — 2010 games had no such ghost because
    // they had no TAA. The engine has the fix built (r_velocity feeds per-object
    // motion vectors into the TAA reprojection) and it defaults OFF for
    // byte-identical capture baselines; a world whose whole subject is a fast
    // car is exactly where it must be on. --set r_velocity 0 restores the old
    // path; applyHostRenderCVars afterwards lets every --set override stick.
    {
        x3::rhi::IRenderDevice::PostFXParams px{};   // engine defaults...
        px.velocity = true;                          // ...plus the one that matters
        device->setPostFX(px);
        applyHostRenderCVars(hc, *device, "tunnel");
    }
    // CASCADED SHADOWS — W-TREES' find: this host NEVER called applyOutdoorCsm,
    // so an outdoor world with a 4 km far plane was running the legacy 45 m
    // camera-locked shadow box; everything beyond it (the mountain, the tour
    // roads, any tree that ever gets planted) cast nothing. Same "compiled in,
    // unreachable" defect the helper's own comment documents for cliffs.
    applyOutdoorCsm(hc, *device, 400.0f, "tunnel");

    // ==== STEP 2 — the streamed terrain ring =================================
    float startPos[3];
    // On the road, out on open ground, far enough back that the whole approach
    // cutting + the portal are ahead of you (and in frame on the approach shot).
    route.posAt(std::max(8.0f, route.boreS0 - 55.0f), startPos);
    if (ringOn && ringSpec.x.size() > 2) {
        // Stand on the ring itself: its first node, lifted to the graded datum.
        startPos[0] = ringSpec.x[0];
        startPos[2] = ringSpec.z[0];
        // IN THE LANES, not on the crossover (Tim, driving the freeway:
        // "Starting point should be moved to the lanes on the right"). The
        // dual route's node 0 is the median centerline — and a turnaround
        // slab sits exactly there. Offset to the RIGHT carriageway's lane 4
        // (right-hand traffic: median on the driver's left), using the same
        // median plan the ribbon was built from.
        if (ringSpec.dualCarriageway && ringRoadY.size() == ringSpec.x.size()) {
            const std::vector<float> mp =
                x3::game::computeMedianPlan(ringSpec, ringRoadY);
            if (!mp.empty()) {
                // Node 0 tangent -> right vector (RH Y-up: right = (-fz, fx)).
                const float fx0 = ringSpec.x[1] - ringSpec.x[0];
                const float fz0 = ringSpec.z[1] - ringSpec.z[0];
                const float fl  = std::sqrt(fx0 * fx0 + fz0 * fz0);
                if (fl > 0.01f) {
                    const float rx = -fz0 / fl, rz = fx0 / fl;
                    const float lat = mp[0] + x3::game::kFwyPavedHalfM
                                    - 0.5f * x3::game::kLaneFt * x3::game::kFtToM;
                    startPos[0] += rx * lat;
                    startPos[2] += rz * lat;
                }
            }
        }
        startPos[1] = x3::game::terrainHeightAtWorld(startPos[0], startPos[2]) + 1.0f;
    }
    // X3_SPAWN=lot|spur|bore — START somewhere other than the ring. THE REVIEW
    // KNOB. The summit lot is 7.5 km from the default spawn, so in practice
    // nobody had ever seen it: not the owner (twenty minutes of driving), not
    // an agent (the --screenshot harness is headless and does not go there).
    // A destination nobody can get to is not finished, and "I did not look at
    // it" is the honest reading of every review that skipped it.
    // DEFAULT OFF and unset spawns exactly as before (NO_SLOP rule 6).
    if (const char* sp = std::getenv("X3_SPAWN")) {
        const std::string w(sp);
        bool moved = false;
        if (w == "lot" && summitLot.built) {
            // On the pad's entry mouth, facing in along the aisle.
            startPos[0] = summitLot.mouthX; startPos[2] = summitLot.mouthZ; moved = true;
        } else if (w == "spur" && summitSpur.built) {
            startPos[0] = summitSpur.spec.x.back();
            startPos[2] = summitSpur.spec.z.back(); moved = true;
        } else if (w == "bore") {
            float p[3]; route.posAt(std::max(8.0f, route.boreS0 - 40.0f), p);
            startPos[0] = p[0]; startPos[2] = p[2]; moved = true;
        }
        if (moved) {
            startPos[1] = x3::game::terrainHeightAtWorld(startPos[0], startPos[2]) + 1.0f;
            char sb[160];
            std::snprintf(sb, sizeof(sb), "--world tunnel: X3_SPAWN=%s — spawning at (%.0f, %.0f, %.0f)",
                          w.c_str(), (double)startPos[0], (double)startPos[1], (double)startPos[2]);
            x3::logInfo(sb);
        } else {
            x3::logWarn(std::string("--world tunnel: X3_SPAWN=") + w +
                        " not available (want lot|spur|bore) — default spawn");
        }
    }

    x3::game::TerrainStreamer streamer;
    const x3::game::TerrainConfig& cfg = x3::game::worldTerrainConfig();
    streamer.setUploadBudget(96);
    streamer.setMaxInFlight(48);
    // PAIRED with the horizon ring's rInner below (NO_SLOP rule 4) — the ring
    // must start INSIDE what the streamer covers or there is a hole between
    // them. Named once so the two can never drift apart again.
    const int kStreamRadiusTiles = headless ? 14 : 9;
    streamer.init(scene, *device, *phys, jobs.get(), cfg,
                  startPos[0], startPos[2], kStreamRadiusTiles);

    // Far country so the hill sits in a landscape and not on a void horizon.
    //
    // THE VOID ANNULUS (found by W-PERF, fixed 2026-08-17). rInner was 470 m
    // flat, and the centre was the ROUTE MIDPOINT while the streamer centres on
    // startPos and follows the player. Streamed coverage is
    // kStreamRadiusTiles * cfg.tileSize — 288 m interactively (radius 9), 448 m
    // headless (radius 14) — so the ground ran out at 288 m and the far country
    // did not begin until 470 m: a 182 m ring of NOTHING all the way round the
    // player, every interactive frame. Headless it was 22 m, which is why no
    // capture ever showed it: --screenshot runs at radius 14 and the harness
    // structurally cannot see the interactive case. Verified by eye in an
    // interactive run, not a capture.
    //
    // Both values now derive from the one radius. The margin is one tile: a
    // tile-aligned streamer guarantees coverage to radius*tileSize from the
    // player only when the player sits at their tile's centre, so back the ring
    // rim a full tile inside it and let the streamed tiles overlap the seam
    // (yBias keeps the ring recessed under them). Same rule host_streamed.cpp
    // keeps at 240 m against its radius-8 / 256 m disc.
    {
        const float streamedR = (float)kStreamRadiusTiles * cfg.tileSize;
        x3::game::HorizonRingDesc hr{};
        // Centre on the STREAMER's centre, not the route midpoint: the hole in
        // the middle of the ring is exactly the disc the streamer fills, so the
        // two must be concentric. (Known limit, shared with every other host:
        // the ring is baked once and does not re-centre on long travel —
        // host_streamed.cpp documents the same follow-up.)
        hr.centerX = startPos[0]; hr.centerZ = startPos[2];
        hr.rInner = streamedR - cfg.tileSize; hr.rOuter = 9000.0f;
        hr.rings = 96; hr.segments = 128; hr.yBias = -3.0f;
        x3::game::addTerrainHorizonRing(scene, *device, streamer.groundTexture(), hr);
        char hb[192];
        std::snprintf(hb, sizeof(hb),
                      "--world tunnel: horizon ring rInner %.0f m inside a %.0f m "
                      "streamed disc (radius %d tiles x %.0f m), concentric at (%.0f, %.0f)",
                      hr.rInner, streamedR, kStreamRadiusTiles, cfg.tileSize,
                      hr.centerX, hr.centerZ);
        x3::logInfo(hb);
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
    // into the cutting the corridor already graded. The ring's ribbon rides its
    // graded DATUM now — the spawn connector's carve crosses under the ring at
    // the junction, and a field-derived ribbon would dip into that cut.
    if (ringOn) x3::game::buildRoadRibbon(ringSpec, scene, *device, *phys,
                                          ringRoadY.empty() ? nullptr : &ringRoadY);
    // The spawn connector + the summit spur, and the junction mouths that BLEND
    // them into the roads they meet (ruled patch lapped over the main pavement
    // + cement flare wings — not a butt joint).
    if (connOn) {
        x3::game::buildRoadRibbon(connector.spec, scene, *device, *phys,
                                  &connector.roadY);
        x3::game::buildJunctionMouth(connector.ringJct, scene, *device, *phys);
        if (circuitOn) {
            // The 3-5 mile lap and its access road, mouthed onto BOTH the
            // connector and the circuit — two junctions, same machinery.
            x3::game::buildRoadRibbon(rangeCircuit.spec, scene, *device, *phys,
                                      &rangeCircuit.roadY);
            x3::game::buildRoadRibbon(rangeCircuit.accessSpec, scene, *device, *phys,
                                      &rangeCircuit.accessRoadY);
            x3::game::buildJunctionMouth(rangeCircuit.connJct, scene, *device, *phys);
            x3::game::buildJunctionMouth(rangeCircuit.circJct, scene, *device, *phys);
        }
        if (summitSpur.built) {
            x3::game::buildRoadRibbon(summitSpur.spec, scene, *device, *phys,
                                      &summitSpur.roadY);
            x3::game::buildJunctionMouth(summitSpur.jct, scene, *device, *phys);
            // ...and the lot it climbs to. Slab and kerb COLLIDE (NO_SLOP rule
            // 11): you drive onto it, you get out, you walk on it.
            if (summitLot.built)
                x3::game::buildSummitLot(summitLot, scene, *device, *phys);
            // ...and the dirt road that leaves it for the mountains.
            if (ridgeRoad.built)
                x3::game::buildRidgeRoad(ridgeRoad, scene, *device, *phys);
        }
    }
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
    // The outer connector's pavement and a junction mouth at EACH end — it is
    // the only road here that lands on two different tours.
    if (outerConnOn) {
        x3::game::buildRoadRibbon(outerConn.spec, scene, *device, *phys,
                                  &outerConn.roadY);
        x3::game::buildJunctionMouth(outerConn.ringJct, scene, *device, *phys);
        x3::game::buildJunctionMouth(outerConn.outerJct, scene, *device, *phys);
    }
    if (riverOn) {
        x3::game::buildRoadRibbon(riverRoad.spec, scene, *device, *phys,
                                  &riverRoad.roadY);
        x3::game::buildRiverBridge(riverRoad.plan, scene, *device, *phys);
        // The two ring landings get the same mouth every other junction has:
        // ruled twist onto the tour's surface + swooping merge fillets both
        // ways. Before this, the valley road's ends just stacked on the ring.
        if (riverRoad.ringJctA.valid)
            x3::game::buildJunctionMouth(riverRoad.ringJctA, scene, *device, *phys);
        if (riverRoad.ringJctB.valid)
            x3::game::buildJunctionMouth(riverRoad.ringJctB, scene, *device, *phys);
    }
    // ---- THE GLIMVALE WORKS ------------------------------------------------
    // Pavement first (so the mouth laps onto a road that exists), then the
    // works itself, which reads the CARVED field for its platform skirt and so
    // has to come after the streamer. See app/factory.h.
    x3::game::FactoryWorks factory;
    x3::game::GoldenTickets tickets;
    if (facOn) {
        x3::game::buildRoadRibbon(facDrive.spec, scene, *device, *phys, &facDrive.roadY);
        if (facDrive.jct.valid)
            x3::game::buildJunctionMouth(facDrive.jct, scene, *device, *phys);
        factory.build(scene, *device, *phys, facPlan);
    }
    // THE GOLDEN TICKETS. Five cards, five landmarks — the list comes from
    // factoryTicketSpots(), the SAME call --test-factory gates, so the world
    // and its test can never hide them in different places (NO_SLOP rule 4).
    {
        x3::game::TicketSpotDef sp[x3::game::kTicketCount];
        const uint32_t ns = x3::game::factoryTicketSpots(facPlan, sp);
        for (uint32_t k = 0; k < ns; ++k)
            tickets.addSpot(sp[k].name, sp[k].x, sp[k].y, sp[k].z);
        tickets.build(scene, *device);
        // X3_TICKETS=n seeds the count. This is the SAVE HOOK standing in for a
        // save file (the hunt has no persistence yet and says so), and it is
        // also the only way a STILL can prove the HUD counter and the open
        // gate — a headless capture cannot walk up and press E. `tickets <n>`
        // in the console does the same thing live.
        if (const char* te = std::getenv("X3_TICKETS")) {
            tickets.setCollected(scene, std::atoi(te));
            if (tickets.allFound() && facOn) factory.openGate();
            char tb2[96];
            std::snprintf(tb2, sizeof(tb2), "tickets: seeded at %d/%d from X3_TICKETS",
                          tickets.collected(), (int)tickets.spotCount());
            x3::logInfo(tb2);
        }
    }

    device->setPointLights(tunnel.lights().data(), (uint32_t)tunnel.lights().size());

    // W-STATIONS — forecourt aprons + driveways (collision: the car drives ON
    // them) and the canopy/pump/kiosk structures. AFTER the ribbons, so the
    // driveway laps pavement that already exists; BEFORE optimizeBroadphase().
    const x3::game::GasStationBuildResult stationBuild =
        gasStations.build(scene, *device, *phys);
    (void)stationBuild;

    // Tall broadleaf groves shading the open-country stretches of the road
    // (Tim 2026-08: "somE Tall Trees!! Shading the road... In some areas").
    // Purely visual; failure = treeless road, never fatal. The showcase camera
    // poses become trunk keep-outs so no crown ever swallows a proof shot (the
    // exit-portal three-quarter pose stands ON the bank inside the planting
    // band). See app/road_trees.h.
    x3::game::RoadTrees trees;
    {
        std::vector<x3::game::RoadTrees::KeepOut> camKeepOut;
        for (int i = 0; i < x3::game::TunnelCorridorWorld::kShowcaseShots; ++i) {
            float cam[5]; tunnel.showcaseCamera(route, i, cam);
            camKeepOut.push_back({ cam[0], cam[2], 12.0f });
        }
        // A tree through a station canopy, or one standing in the driveway
        // mouth, is exactly the defect the keep-out list exists for.
        std::vector<float> stationDiscs;
        gasStations.keepOutDiscs(stationDiscs);
        for (size_t k = 0; k + 2 < stationDiscs.size(); k += 3)
            camKeepOut.push_back({ stationDiscs[k], stationDiscs[k+1], stationDiscs[k+2] });
        // The minBenchY shim (drawn-plane level) is GONE: the drawn river now
        // follows the same worldWaterLevelAt table (task #32 — one truth), so
        // RoadTrees' own water-table check IS the drawn waterline. `phys` stays
        // — that is the trunk collision the owner asked for, unrelated to the
        // shim that shared the call.
        trees.build(*device, route, camKeepOut, -1.0e9f, phys.get());
    }

    // ==== THE SMALL MOUNTAIN TOWN (W-TOWN) ==================================
    // ROAD_NETWORK_SKETCH_V2.png's brown "Small Mountain Town" hangs off a
    // yellow ladder-switchback road, and ROAD_NETWORK_PLAN.md:701 already named
    // the site: "Town 2 — the climb foot, where the inner tour meets the summit
    // road." The SUMMIT SPUR is that ladder — the network's only switchback
    // climb — so main street is laid along its lowest, gentlest reach.
    // DEFAULT ON for the world it was built for (NO_SLOP rule 6); X3_TOWN=0
    // is the door for turning it OFF, not the door it lives behind.
    // Built AFTER the ribbons so terrainHeightAtWorld returns the carved field
    // and the pavement it fronts already exists.
    x3::game::Town town;
    bool townOn = false;
    // --screenshot-town's dusk gate. The settle loop re-pushes SkyParams every
    // frame when weather is on, so a one-shot setSkyParams before the grab
    // would be overwritten sixty times before the capture — the flag is read
    // INSIDE the loop instead.
    bool townDusk = false;
    {
        const char* e = std::getenv("X3_TOWN");
        townOn = summitSpur.built && !(e && e[0] == '0');
        if (townOn) {
            x3::game::Town::Config tc;
            tc.street  = &summitSpur.spec;
            tc.streetY = &summitSpur.roadY;
            tc.startU  = 70.0f;
            tc.endU    = 760.0f;
            // The junction mouth owns the bottom of the spur; nothing stands
            // in it (kJunctionSetbackM + the merge fillets).
            tc.keepOut.push_back({ summitSpur.jct.jx, summitSpur.jct.jz,
                                   x3::game::kJunctionSetbackM });
            townOn = town.build(scene, *device, *phys, tc);
            if (townOn) {
                town.spawnPedestrians(*device, *phys);
                // The tunnel's own fixtures plus the town's lamps and lit
                // windows — ONE setPointLights call owns the array, so the
                // town's have to join the tunnel's rather than replace them.
                std::vector<x3::rhi::PointLight> pl = tunnel.lights();
                pl.insert(pl.end(), town.lights().begin(), town.lights().end());
                device->setPointLights(pl.data(), (uint32_t)pl.size());
            }
        } else if (!summitSpur.built) {
            x3::logWarn("--world tunnel: no summit spur -> no mountain town "
                        "(the town rides the spur; see app/town.h)");
        }
    }

    // THE FORESTS — the owner's brown map regions (ROAD_NETWORK_SKETCH_V2.png:
    // "This Color is All Forest"): north belt, centre-north patch, NE corner,
    // the southern countryside belt walling the tour ("Tthick woods on much of
    // the road!!!!"), both mountain skirts, the river-bank strips. Built HERE —
    // after every registerRoad/registerTunnelCorridor and after RoadTrees — so
    // the keep-outs (corridor footprints, junctions, road_trees' own roadside
    // band) read the final registries. X3_FOREST=0 to disable (the flag is for
    // turning it OFF — NO_SLOP rule 6). See app/forest.h.
    x3::game::WorldForests forests;
    {
        const char* e = std::getenv("X3_FOREST");
        if (!(e && e[0] == '0')) {
            x3::game::WorldForests::Inputs fin;
            fin.demoRoute = &route;
            fin.innerTour = ringOn ? &ringSpec : nullptr;
            forests.build(*device, fin);
        }
    }

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

    // ---- JAKE. The shared AnimatedCharacter module (app/character_anim.h)
    // owns EVERYTHING that used to be hand-wired here — the exact clip
    // lookups, the facing convention, the contact-law feet clamp, the
    // measured directional clips, the jump/fall/turn one-shots. This host is
    // merely the module's first consumer (owner: "THIS ENGINE NEEDS
    // CONSISTENT APPLICATION OF MODEL ANIMATION"). Any other world gets Jake
    // by writing these same few lines.
    x3::game::AnimatedCharacter jake;
    bool jakeTried = false;
    // The river lane's hook: swim/swimIdle clip indices, resolved and ready
    // the moment the rig loads (selection logic already lives in the module;
    // the swim STATE is Player::swimming(), wired via setWaterQuery).
    x3::game::AnimatedCharacter::SwimClipset jakeSwimClipset;
    float jakeCamToast = 0.0f;       // seconds left on the F1 mode banner

    x3::game::Player onFoot;
    // W10 SWIMMING, wired (owner: "we need water.. you can swim in"). The
    // Player has carried the full swim state machine since W10 — buoyancy
    // spring, swim-along-look, Space-up/Ctrl-down, enter/exit hysteresis — it
    // just never got a water feed in THIS host, so Jake hiked the riverbed
    // dry under the water table.
    //
    // THE FEED IS THE SURFACE HE CAN SEE — and since task #32 the drawn
    // surface IS worldWaterLevelAt (the water shader steps the plane down the
    // channel from the same node table), so the old bridge-reach clamp to the
    // flat plane is deleted. What stays is the +0.35 m presentation bias: the
    // Player's buoyancy rests the EYE just above the fed surface, which
    // leaves the drawn head bobbing right AT the waterline — reading as
    // submerged whenever a crest rolls through. A treading human rides higher
    // than his eye line; reporting the surface a hand-span HIGH makes the
    // buoyancy lift him that much further, so head + shoulders clear the
    // drawn crests.
    onFoot.setWaterQuery([](float x, float z) {
        const float w = x3::game::worldWaterLevelAt(x, z);
        if (w <= x3::game::kWorldWaterDry + 1.0f) return w;
        return w + 0.35f;
    });
    bool  driving      = true;
    bool  footSpawned  = false;
    float parkedAt[3]  = { 0, 0, 0 };   // where the car was left, for the re-entry prompt

    // ==== STEP 4 — the car, on the road, outside the entrance ================
    x3::game::DriveDemo car;
    // SPAWN ON TOP OF THE ROAD (Tim, after landing on the dirt BESIDE it):
    // terrain height is right except where the vertical-curve pass floats the
    // ribbon above the graded field (sags float up to ~5 m). The rule, done
    // properly: a raycast straight down takes the TOPMOST collider — the road
    // surface when the road is there, the terrain when it is not — i.e. spawn
    // where a wheel would actually land. Terrain height stays as the fallback
    // if the physics tiles under the spawn have not streamed in yet.
    float spawnGroundY = x3::game::terrainHeightAtWorld(startPos[0], startPos[2]);
    {   char sb[128];
        std::snprintf(sb, sizeof(sb), "[tunnel] SPAWN at (%.1f, %.1f, %.1f)",
                      startPos[0], spawnGroundY, startPos[2]);
        x3::logInfo(sb);
    }
    {
        const x3::phys::RayHit hit = phys->rayCast(
            x3::phys::Vec3{ startPos[0], spawnGroundY + 60.0f, startPos[2] },
            x3::phys::Vec3{ 0.0f, -1.0f, 0.0f }, 120.0f, x3::phys::Layer::Static);
        if (hit.hit && hit.point.y > spawnGroundY - 0.5f)
            spawnGroundY = hit.point.y;
    }
    const bool carBuilt = car.build(*device, *phys, startPos[0], spawnGroundY + 1.4f, startPos[2]);
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
        // AIM ALONG THE ROAD THAT IS ACTUALLY PAINTED THERE. Two wrong
        // attempts taught the lesson: the global chord skewed the car, and the
        // tunnel spine's local tangent STILL skewed it — because the pavement
        // at spawn is the roads-machinery ribbon (smoothed spec polylines),
        // not the spine. So: find the nearest segment across every registered
        // spec polyline within 60 m and face down IT; the spine tangent is
        // only the last-resort fallback.
        float tdx = route.dirX, tdz = route.dirZ;
        {
            float bestD2 = 60.0f * 60.0f;
            auto scanSpec = [&](const x3::game::RoadSpec& sp) {
                const size_t n = std::min(sp.x.size(), sp.z.size());
                for (size_t i = 0; i + 1 < n; ++i) {
                    const float mx2 = 0.5f * (sp.x[i] + sp.x[i + 1]);
                    const float mz2 = 0.5f * (sp.z[i] + sp.z[i + 1]);
                    const float ddx2 = mx2 - startPos[0], ddz2 = mz2 - startPos[2];
                    const float d2 = ddx2 * ddx2 + ddz2 * ddz2;
                    if (d2 < bestD2) {
                        float sx2 = sp.x[i + 1] - sp.x[i], sz2 = sp.z[i + 1] - sp.z[i];
                        const float sl = std::sqrt(sx2 * sx2 + sz2 * sz2);
                        if (sl > 1e-3f) {
                            bestD2 = d2; tdx = sx2 / sl; tdz = sz2 / sl;
                        }
                    }
                }
            };
            if (connOn) scanSpec(connector.spec);
            if (ringOn) scanSpec(ringSpec);
            if (circuitOn) scanSpec(rangeCircuit.accessSpec);
            // Face INTO the corridor, not out of it: if the nearest segment
            // runs against the route direction, flip it.
            if (tdx * route.dirX + tdz * route.dirZ < 0.0f) { tdx = -tdx; tdz = -tdz; }
        }
        const float yaw = -std::atan2(-tdx, -tdz);
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
    // Marks are geometry (rubber lies ON the road); SMOKE is not — it goes
    // through IRenderDevice::submitParticles, the engine's own billboard pass:
    // camera-facing quads, alpha blend, depth-test-no-write, soft-particle
    // depth fade. Exactly the pipeline the Vulkan references Tim sent describe,
    // already built and already carrying the rain and snow — the first cut of
    // this feature drew CUBES because I reached for drawMesh instead of
    // checking what the device offered.
    x3::rhi::MeshHandle fxMarkMesh;
    x3::rhi::TextureHandle fxSkidTex;
    {
        std::vector<x3::rhi::MeshVertex> qv; std::vector<uint32_t> qi;
        x3::prims::makeCube(0.5f, qv, qi);
        fxMarkMesh = device->createMesh(qv.data(), (uint32_t)qv.size(), qi.data(), (uint32_t)qi.size());
        auto sk = x3::prims::makeSolidRGBA(8, 16, 16, 19);
        fxSkidTex = device->createTexture(sk.data(), 8, 8, true);
    }
    struct SpinFx { float x, y, z, age, yaw; uint8_t kind; };  // 0=skid, 1=smoke
    SpinFx fx[512]; uint32_t fxN = 0;
    float fxSpawnAcc = 0.0f;
    std::vector<x3::rhi::IRenderDevice::ParticleInstance> fxPuffs;
    fxPuffs.reserve(512 * 3);

    // ==== WEAPONS ON FOOT (owner, twice tonight: "Does he have his weapons?")
    // GREP-FIRST RECEIPT — nothing here is a new system:
    //   * Arsenal (app/weapon.h): ammo/mag/reload/cooldown/spread/hitscan
    //     resolution + the loaded rifle GLB + drawCurrentAt (3P hand socket).
    //   * CombatFx (app/fx.h): tracers, muzzle bursts, impact sparks + scorch
    //     decals, explosion fireballs — all through submitParticles.
    //   * AnimatedCharacter (app/character_anim.h): the rifle clip states
    //     (Rifleaimingidle/Firingrifle/Reloading/Tossgrenade/Riflerun/
    //     Riflejump) + the boneWorld() hand socket. The module owns the rig;
    //     this host owns only keys, raycasts and composition.
    auto combatFxOwned = std::make_unique<x3::game::CombatFx>(); // ~256 KB scratch — heap (host_space precedent)
    x3::game::CombatFx& combatFx = *combatFxOwned;
    combatFx.init(*device);
    x3::game::Arsenal rifle(tunnelRifleRoster());
    bool rifleVmLoaded = false;      // viewmodel GLB loaded lazily on first draw-out
    bool rifleArmed    = false;      // 1/Q toggles; holstered = unarmed melee stays
    bool rifleAiming   = false;      // RMB held while armed
    uint32_t fireRng   = 0xC0FFEEu;  // deterministic spread stream
    // Transient weapon LIGHTS (muzzle flash / grenade boom), merged in front of
    // the pooled bore lights each frame (uploadTunnelLights extra param).
    float wpnFlashT = 0.0f;  float wpnFlashPos[3] = { 0, 0, 0 };
    float boomLightT = 0.0f; float boomLightPos[3] = { 0, 0, 0 };
    // Grenades: a small ring of REAL Jolt dynamic spheres with fuses.
    struct TunnelGrenade { x3::phys::BodyId id{}; float fuse = 0.0f; bool live = false; };
    TunnelGrenade grenades[8];
    float grenadeReleaseT = -1.0f;   // Tossgrenade start -> ball-leaves-hand delay
    // The existing S7 HUD reticle (Hud::drawCrosshair is stateless — no
    // console binding needed for the crosshair alone).
    x3::game::Hud wpnHud;

    // The held rifle's WORLD matrix (per-weapon grip + scale folded), through
    // the module's hand socket. Same composition as the campaign's
    // ThirdPersonView::drawHeldWeapon (shared tpComposeGrip — the frames
    // cannot drift). False when the gun is not drawable this frame.
    auto heldRifleWorld = [&](float out[16]) -> bool {
        if (!rifleArmed || !rifle.viewmodelsLoaded() || !rifle.currentHasDrawables())
            return false;
        const float yawTrim = (console ? console->getFloat("jake_yaw") : 0.0f) * 0.0174533f;
        const float yTrim   =  console ? console->getFloat("jake_y")   : 0.0f;
        float hand[16];
        if (!jake.boneWorld(x3::game::kJakeHandBone, onFoot, yawTrim, yTrim, hand))
            return false;
        const x3::game::TpGrip& g = x3::game::tpGripFor(rifle.current().name);
        float grip[16], world[16];
        x3::game::tpComposeGrip(g.forward, g.right, g.down,
                                g.yawDeg, g.pitchDeg, g.rollDeg, grip);
        x3::asset::mulMat4(hand, grip, world);
        const float s = rifle.currentViewmodelScale()
                      * x3::game::kTpHeldWeaponScaleMul * g.scaleMul;
        for (int c = 0; c < 3; ++c)
            for (int r = 0; r < 3; ++r) world[c * 4 + r] *= s;
        std::memcpy(out, world, 16 * sizeof(float));
        return true;
    };
    // THE MUZZLE: the MEASURED barrel tip under the SAME matrix the gun draws
    // with (Tim 2026-07-11: "the fire doesn't come from the barrel" — solved
    // once in weapon.cpp; this reuses that measurement, never a guess).
    auto heldRifleMuzzle = [&](x3::phys::Vec3& out) -> bool {
        float w[16];
        if (!heldRifleWorld(w)) return false;
        const x3::phys::Vec3 m = rifle.currentMuzzleLocal();
        out = x3::phys::Vec3{ w[0]*m.x + w[4]*m.y + w[8]*m.z  + w[12],
                              w[1]*m.x + w[5]*m.y + w[9]*m.z  + w[13],
                              w[2]*m.x + w[6]*m.y + w[10]*m.z + w[14] };
        return true;
    };
    // ONE trigger pull: Arsenal gates it (cooldown/mag/reload), the module
    // plays Firingrifle, each resolved ray raycasts the static world, and the
    // FX leave from the TRUE muzzle. Shared verbatim by the live loop and the
    // proof captures so the shots are the shipped code path.
    auto fireRifleOnce = [&]() {
        float ex, ey, ez, cyaw, cpit;
        onFoot.camera(ex, ey, ez, cyaw, cpit);
        const x3::phys::Vec3 eye{ ex, ey, ez };
        const x3::phys::Vec3 dir{ std::cos(cpit) * std::cos(cyaw), std::sin(cpit),
                                  std::cos(cpit) * std::sin(cyaw) };
        const x3::game::ResolvedFire rf = rifle.fire(eye, dir, fireRng);
        if (!rf.fired) return;
        jake.fireOneShot();
        x3::phys::Vec3 muzzle;
        if (!heldRifleMuzzle(muzzle))
            muzzle = x3::phys::Vec3{ ex + dir.x * 0.5f, ey + dir.y * 0.5f,
                                     ez + dir.z * 0.5f };
        const x3::game::WeaponFxKind kind =
            x3::game::fxKindFromId(rifle.current().muzzleFx);
        for (const x3::game::HitscanRay& ray : rf.rays) {
            const x3::phys::RayHit h =
                phys->rayCast(eye, ray.dir, ray.range, x3::phys::Layer::Static);
            const x3::phys::Vec3 to = h.hit ? h.point
                : x3::phys::Vec3{ eye.x + ray.dir.x * ray.range,
                                  eye.y + ray.dir.y * ray.range,
                                  eye.z + ray.dir.z * ray.range };
            combatFx.addTracer(muzzle, to, kind);
            if (h.hit) combatFx.spawnImpact(h.point, h.normal, kind);
            // DAMAGE HOOK (structured for the campaign, stubbed here): no
            // enemies live in this world yet. The campaign sink is
            // MonsterManager::fire(eye, dir, damage, type); when monsters
            // reach the driving world, route ray.damage / ray.type there.
        }
        combatFx.spawnMuzzleFlash(muzzle, dir, kind);
        wpnFlashT = 0.06f;
        wpnFlashPos[0] = muzzle.x; wpnFlashPos[1] = muzzle.y; wpnFlashPos[2] = muzzle.z;
        // Recoil: the Arsenal's resolved pitch kick, applied to the SAME look
        // the camera and the next fire ray read.
        onFoot.setLook(cyaw, cpit + rf.recoilPitchDeg * 0.0174533f);
    };
    // Grenade release (scheduled off the Tossgrenade one-shot so the ball
    // leaves when the ARM swings, not when the key goes down): a real Jolt
    // dynamic sphere lobbed along the camera with an arc.
    auto releaseGrenade = [&]() {
        float ex, ey, ez, cyaw, cpit;
        onFoot.camera(ex, ey, ez, cyaw, cpit);
        const x3::phys::Vec3 dir{ std::cos(cpit) * std::cos(cyaw), std::sin(cpit),
                                  std::cos(cpit) * std::sin(cyaw) };
        for (TunnelGrenade& g : grenades) {
            if (g.live) continue;
            g.id = phys->addSphere(0.08f,
                x3::phys::Vec3{ ex + dir.x * 0.7f, ey + dir.y * 0.7f - 0.15f,
                                ez + dir.z * 0.7f },
                0.4f, x3::phys::Layer::Dynamic);
            const float v[3] = { dir.x * 13.0f, dir.y * 13.0f + 4.5f, dir.z * 13.0f };
            phys->setBodyLinearVelocity(g.id, v);
            g.fuse = 2.2f;
            g.live = true;
            return;
        }
    };
    // Grenade integration: a hot glowing core + smoke trail in flight (the
    // CombatFx Rocket bolt visual — the arc READS with no mesh, so there is no
    // untextured stand-in prop to violate NO_SLOP rule 3), fireball + smoke +
    // a light pulse at detonation. Ticks even while driving, so a tossed
    // grenade still goes off behind you.
    auto tickGrenades = [&](float gdt) {
        for (TunnelGrenade& g : grenades) {
            if (!g.live) continue;
            const x3::phys::Vec3 p = phys->getBodyPosition(g.id);
            float gv[3]; phys->getBodyLinearVelocity(g.id, gv);
            combatFx.boltFx(p, x3::phys::Vec3{ gv[0], gv[1], gv[2] },
                            x3::game::WeaponFxKind::Rocket);
            g.fuse -= gdt;
            if (g.fuse > 0.0f) continue;
            combatFx.spawnExplosion(p, 3.2f);
            combatFx.spawnSmoke(p);
            boomLightT = 0.16f;
            boomLightPos[0] = p.x; boomLightPos[1] = p.y; boomLightPos[2] = p.z;
            // The car feels a near miss — an honest radial shove, not a script.
            if (carBuilt) {
                float vp[3]; car.chassisPos(vp);
                const float bx = vp[0] - p.x, by = vp[1] - p.y, bz = vp[2] - p.z;
                const float d2 = bx * bx + by * by + bz * bz;
                if (d2 < 8.0f * 8.0f && d2 > 0.01f) {
                    const float inv = 1.0f / std::sqrt(d2);
                    const float kick = 5200.0f * (1.0f - std::sqrt(d2) / 8.0f);
                    phys->applyImpulse(car.chassis(),
                        x3::phys::Vec3{ bx * inv * kick, 0.35f * kick, bz * inv * kick });
                }
            }
            phys->removeBody(g.id);
            g.live = false;
        }
    };
    // The transient weapon-light pulses for THIS frame (decayed by dt; up to 2).
    auto weaponLights = [&](float wdt, x3::rhi::PointLight* out) -> uint32_t {
        uint32_t n = 0;
        if (wpnFlashT > 0.0f) {
            wpnFlashT -= wdt;
            const float k = std::max(0.0f, wpnFlashT / 0.06f);
            x3::rhi::PointLight& l = out[n++];
            l.pos[0] = wpnFlashPos[0]; l.pos[1] = wpnFlashPos[1]; l.pos[2] = wpnFlashPos[2];
            l.range = 7.0f;
            l.color[0] = 6.0f * k; l.color[1] = 4.2f * k; l.color[2] = 1.8f * k;
        }
        if (boomLightT > 0.0f) {
            boomLightT -= wdt;
            const float k = std::max(0.0f, boomLightT / 0.16f);
            x3::rhi::PointLight& l = out[n++];
            l.pos[0] = boomLightPos[0]; l.pos[1] = boomLightPos[1]; l.pos[2] = boomLightPos[2];
            l.range = 16.0f;
            l.color[0] = 14.0f * k; l.color[1] = 7.5f * k; l.color[2] = 2.2f * k;
        }
        return n;
    };
    // Arm/holster (1 or Q). The module swaps the rig states; the Arsenal's
    // rifle GLB loads once, on the FIRST draw-out (most runs never leave the
    // car — same lazy discipline as the Jake rig itself).
    auto setRifleArmed = [&](bool want) {
        if (want && !rifleVmLoaded) {
            rifleVmLoaded = true;
            rifle.loadViewmodels(*device, x3::game::assetRoot() + "/rigged_glb");
            if (!rifle.currentHasDrawables())
                x3::logWarn("[tunnel] WeaponRailgun.glb failed to load — no rifle model");
        }
        rifleArmed = want && rifle.currentHasDrawables();
        jake.setArmed(rifleArmed);
        if (!rifleArmed) { rifleAiming = false; jake.setAiming(false); }
    };

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
    // ---- ENGINE NOTE v2: the multi-RPM bank (snd_bank 1, the default). ----
    // Four voices bracket the live RPM between adjacent synthesized flat-six
    // points (900/1500/2500/4000/5500/7000) and equal-power-crossfade them;
    // a smoothed load weight crossfades on-load vs OVERRUN timbre. The old
    // single-loop path below stays wired behind `snd_bank 0` so the owner can
    // A/B the two by ear from the console.
    x3::game::EngineNote engineNote;
    bool bankReady = false;
    x3::audio::SoundHandle whineSnd{}, turboSnd{};   // dedicated whistle assets (bank mode)
    x3::audio::LoopHandle  whineBankLoop{}, turboBankLoop{};
    if (audioOn) {
        const std::string wav =
            (std::filesystem::path(x3::game::assetRoot()) / "audio/vehicles/engine_loop.wav").string();
        engineSnd = audio->load(wav);
        squealSnd = audio->load((std::filesystem::path(x3::game::assetRoot()) / "audio/vehicles/tire_squeal_loop.wav").string());
        const std::string bankDir =
            (std::filesystem::path(x3::game::assetRoot()) / "audio/vehicles/engine_bank").string();
        bankReady = engineNote.init(audio.get(), bankDir, /*redlineRpm=*/7500.0f);
        // Build the reverb insert BEFORE any loop voice starts: loop voices
        // pick their output route at start time, so the chain must exist first
        // (the per-frame skyVis drive below only retunes it).
        audio->setReverbParams(0.3f, 0.05f);
        // The whine/turbo layers used to be pitched-up copies of the SAME
        // engine wav (SND-FABLE finding #3). In bank mode they get their own
        // synthesized whistles; missing files just silence the layers.
        whineSnd = audio->load((std::filesystem::path(bankDir) / "whine_loop.wav").string());
        turboSnd = audio->load((std::filesystem::path(bankDir) / "turbo_whistle_loop.wav").string());
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
    x3::rhi::TextureHandle texDial{}, texNeedle{}, texGate{}, texBoost{}, texNos{};
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
        texNos    = loadPng("gauge_nos.png");   // 32-state curved fill atlas (8x4)
    }

    // ==== RIVER LIFE (W-RIVER): fish + two AI speedboats on the bridge reach.
    // Everything reused: FishSystem, BoatDemo, the crowd-skin driver pattern,
    // submitParticles wakes, startLoop3D outboards. See app/river_life.h.
    x3::game::RiverLife riverLife;
    if (riverOn && riverRoad.plan.ok)
        riverLife.build(scene, *device, *phys,
                        audioOn ? audio.get() : nullptr, riverRoad.plan);

    // ==== FREEWAY TRAFFIC (W-TRAFFIC) =======================================
    // "now that we have a 16 lane freeway.. we will need to fill it with
    // traffic ;->" — kinematic lane-followers on the inner tour's own lane
    // splines (app/traffic.h). DEFAULT ON (NO_SLOP rule 6: the flag is for
    // turning it OFF) — X3_TRAFFIC=0 disables.
    x3::game::FreewayTraffic traffic;
    struct TrafficContactCtx {
        x3::game::FreewayTraffic* t = nullptr;
        x3::phys::IPhysicsWorld*  p = nullptr;
    } trafficCtx;
    {
        const char* e = std::getenv("X3_TRAFFIC");
        const bool trafficOn = ringOn && !(e && e[0] == '0');
        if (trafficOn &&
            traffic.build(ringSpec, ringRoadY, device, phys.get(),
                          x3::game::convertedGlbRoot(), x3::game::TrafficConfig{})) {
            // The ONE global contact callback (nothing else in this host uses
            // it; the canon world's monster facade has its own world). A hard
            // hit converts the struck car to a dynamic body — the work-zone
            // drum pattern, car-sized.
            trafficCtx.t = &traffic;
            trafficCtx.p = phys.get();
            phys->setContactCallback(
                [](x3::phys::BodyId a, x3::phys::BodyId b, const float*,
                   const float*, float impulse, void* user) {
                    auto* c = static_cast<TrafficContactCtx*>(user);
                    c->t->onContact(a, b, impulse, c->p);
                }, &trafficCtx);
        }
    }

    // THE RIVER HOLDS WATER — one lambda, BOTH render paths. The water pass
    // used to be armed only inside the interactive loop, so every headless
    // capture (the proof shots included) rendered a dry river: the gate was
    // fine, the pass was never enabled on that path at all. Tone per the
    // owner's eyes-on: "too bright... reject from echo harbor" — a river under
    // this sun is dark blue-green with a modest glint, so deep/shallow go
    // darker+greener than the sea defaults, specular drops 12->5 and the
    // fresnel floor 0.02->0.012 (less sky mirror face-on). Caustics ride along
    // (the canon undersea pass) so the deepened bed reads THROUGH the surface.
    // (fx, fz) = this frame's focus (camera/player/car XZ): river mode feeds
    // the LOCAL water level there into seaLevel (the shader's underside-view
    // gate) and the caustics plane — the flat bridge-level plane is gone.
    auto applyRiverWater = [&](float t, float fx, float fz) {
        if (!(riverOn && riverRoad.plan.ok)) return;
        x3::rhi::IRenderDevice::WaterParams wpr{};
        wpr.enabled   = true;
        // ONE WATER TRUTH (task #32): the drawn surface follows the SAME node
        // table worldWaterLevelAt interpolates — stepped down the channel per
        // vertex in water.vert, estuary handed off to the real sea. The old
        // single flat plane at plan.waterY stood ~1.2 m/chain-node above the
        // carved table downstream and climbed the banks (receipt: the bench
        // that shipped submerged at (-340,11,-468) while PASSING the
        // worldWaterLevelAt+0.5 check).
        {
            using WP = x3::rhi::IRenderDevice::WaterParams;
            x3::game::WorldRiverNode rn[WP::kMaxRiverNodes];
            const uint32_t n = x3::game::worldRiverRisenNodes(rn, WP::kMaxRiverNodes);
            wpr.riverNodeCount = n;
            for (uint32_t i = 0; i < n; ++i) {
                wpr.riverNodes[i][0] = rn[i].x;
                wpr.riverNodes[i][1] = rn[i].z;
                wpr.riverNodes[i][2] = rn[i].waterY;
            }
            wpr.riverHalfWidth = x3::game::kWorldRiverHalfWidth;
            wpr.basinCenter[0] = x3::game::kWorldOceanBasinX;
            wpr.basinCenter[1] = x3::game::kWorldOceanBasinZ;
            wpr.basinRadius    = x3::game::kWorldOceanBasinR;
            wpr.oceanLevel     = x3::game::kWorldSeaLevel;
        }
        // seaLevel carries the LOCAL level at the focus (underside-view gate +
        // caustics plane); dry land falls back to the bridge's level.
        const float lw = x3::game::worldWaterLevelAt(fx, fz);
        wpr.seaLevel  = (lw > x3::game::kWorldWaterDry + 1.0f)
                      ? lw : riverRoad.plan.waterY;
        wpr.time      = t;
        wpr.amplitude = 0.16f;          // a river swell, not an ocean — and low
                                        // enough that a treading head clears
                                        // the crests instead of strobing them
        wpr.steepness = 0.35f;
        wpr.waveLength= 9.0f;
        wpr.speed     = 0.8f;
        wpr.deepColor[0]    = 0.008f; wpr.deepColor[1]    = 0.030f; wpr.deepColor[2]    = 0.038f;
        wpr.shallowColor[0] = 0.050f; wpr.shallowColor[1] = 0.150f; wpr.shallowColor[2] = 0.140f;
        wpr.specular  = 5.0f;
        wpr.fresnel   = 0.012f;
        // See-through shallows (WaterParams::clarity): the bed, the fish and a
        // swimmer's body read THROUGH face-on water; depth + grazing angles
        // close it back to a surface. 0 would be the legacy opaque plane.
        wpr.clarity   = 0.60f;
        wpr.sunDir[0] = 0.35f; wpr.sunDir[1] = 0.92f; wpr.sunDir[2] = 0.18f;
        device->setWaterParams(wpr);
        x3::rhi::IRenderDevice::CausticsParams cp{};
        cp.enabled = true; cp.waterY = wpr.seaLevel;   // local level, not the flat plane
        cp.time = t; cp.intensity = 0.85f;
        device->setCaustics(cp);
    };
    float riverWaterClock = 0.0f;

    // ==== RAIN RUNOFF (W-WATER, task #23): heavy rain swells the river. ====
    // Owner's scale is rain 0-10; WeatherSample::precipitation is 0-1, so
    // "rain >= 6" = precipitation >= 0.6. At the threshold the reach rises a
    // visible 0.3 m, scaling to kWorldRiverRainRiseMax (0.9 m) in a full
    // storm — and terrain.cpp caps the rise per node at 60% of the levee
    // freeboard, so the swollen river NEVER tops a levee. The level eases in
    // and out (a river lags its rain); both the drawn surface and
    // worldWaterLevelAt consume the same setWorldRiverRainRise state, so swim
    // physics, fish and the visible water rise together (one truth, rule 4).
    float riverRainRise = 0.0f;
    auto tickRiverRise = [&](float d, float precip, bool snow) {
        if (!(riverOn && riverRoad.plan.ok)) return;
        const float target = (!snow && precip >= 0.6f)
            ? 0.3f + (x3::game::kWorldRiverRainRiseMax - 0.3f)
                     * std::min(1.0f, (precip - 0.6f) / 0.4f)
            : 0.0f;
        const float step = 0.06f * d;              // ~15 s swell, same ebb
        riverRainRise += std::clamp(target - riverRainRise, -step, step);
        x3::game::setWorldRiverRainRise(riverRainRise);
    };

    phys->optimizeBroadphase();

    const float dt = 1.0f / 60.0f;

    // ==== HEADLESS: the proof set ===========================================
    if (headless) {
        namespace fs = std::filesystem;
        std::error_code ec;
        const std::string dir = hc.tunnelShot ? hc.tunnelShotDir : std::string("docs/screenshots/tunnel");
        fs::create_directories(dir, ec);

        // SWIM-PROOF HOOKS (X3_SHOT_SWIM): the staged swimmer ticks the Player
        // and draws Jake through these; empty for every ordinary proof shot.
        std::function<void(float)> shotTick;
        std::function<void(const x3::rhi::FrameContext&)> shotDraw;

        // X3_SHOT_PUMP=1 draws the station HUD into the still (see the block
        // inside the settle loop); X3_SHOT_PUMP=2 additionally holds E, so the
        // "REFUELLING..." state and the filling bar can be photographed too.
        const char* shotPumpEnv = std::getenv("X3_SHOT_PUMP");
        const bool shotPump      = shotPumpEnv && shotPumpEnv[0] != '0';
        const bool shotPumpHoldE = shotPumpEnv && shotPumpEnv[0] == '2';
        if (shotPump) {
            // STAGE THE TANK, or the proof photographs the wrong state: a
            // factory-fresh tank is FULL, so the honest prompt under the canopy
            // is "TANK FULL" and neither the E-REFUEL hint nor the flow can
            // ever appear in the still (the first proof run showed exactly
            // that). This is the SAME state the console command `fuel 24`
            // leaves a player in — litres set, gauge armed — staged through the
            // same public FuelTank, and then update()/drawPumpPrompt/
            // drawFuelBar below run unmodified.
            gasStations.fuel().litres = gasStations.fuel().capacityL * 0.35f;
            gasStations.fuel().armed  = true;
        }

        auto settleAndGrab = [&](const float cam[5], const std::string& out) -> bool {
            // The streamer only enqueues the full ring on a focus-tile crossing
            // (host_cliffs.cpp's trick): nudge the focus on frame 1, then hold.
            const int kFrames = 200;
            // PERF RECEIPT (W-FOREST gate): average the settled window's GPU
            // frame time + submitted geometry, logged per capture — the
            // before/after fps evidence is measured, not asserted (same
            // mechanism as geolod_shot.cpp's measured window).
            double perfMsSum = 0.0; int perfN = 0;
            uint64_t perfTris = 0; uint32_t perfDraws = 0, perfObjs = 0;
            // ONE PERF RECEIPT, NOT TWO (NO_SLOP 1/4). W-CLOUDS landed its own
            // [cloud-perf] gpuFrameMs average here in parallel; this one already
            // averages the SAME device timestamp over the SAME settled 60-frame
            // window, so the duplicate is gone and the cloud-pass budget gate
            // (cloud pass + shadows < 10% of frame time, measured X3_CLOUD=0 vs
            // 0.42 at a fixed cam) is read off the [tunnel-perf] line below.
            // Paired: shots_clouds/run_captures.sh greps for it.
            for (int i = 0; i < kFrames; ++i) {
                glfwPollEvents();
                const float fx = (i == 1) ? cam[0] + 40.0f : cam[0];
                streamer.update(scene, *device, *phys, fx, cam[2]);
                // THE RIVER HOLDS WATER IN CAPTURES TOO. This settle loop never
                // armed the water pass (it lived only in the interactive loop),
                // which is why every proof shot showed a dry river no matter
                // what the runtime gate said. Same lambda, same tone, plus the
                // boats/fish so the capture proves the LIVING river.
                riverWaterClock += dt;
                applyRiverWater(riverWaterClock, cam[0], cam[2]);
                device->setSkyTime(riverWaterClock);   // cloud drift (see the interactive loop)
                riverLife.prePhysics(dt);
                // TRAFFIC IN CAPTURES TOO (gotcha 4.1b's lesson: moving
                // content that only ticks in the live loop is invisible in
                // every proof shot). Focus = the capture camera.
                traffic.update(dt, cam, phys.get());
                if (shotTick) shotTick(dt);   // staged swimmer BEFORE the step
                phys->step(dt);
                riverLife.postPhysics(dt, scene, *device, *phys,
                                      audioOn ? audio.get() : nullptr,
                                      x3::phys::Vec3{ cam[0], cam[1], cam[2] });
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
                    tickRiverRise(dt, ws.precipitation, ws.snowfall);
                    storm.tick(dt, ws.state == x3::game::WeatherState::Storm ? ws.hazardLevel : 0.0f,
                               nullptr, cam[0], cam[1], cam[2]);
                    // ONE mapping with the interactive loop (skyFromWeather,
                    // top of file). This site used to carry its own cut-down
                    // copy WITHOUT the storm branch, which is why the storm
                    // proof shots were brighter than the storm you play.
                    applySky(*device, skyFromWeather(ws, storm.flash()));
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

                if (townDusk) {
                    // Sun on the horizon, warm and low, sky dimmed: the town's
                    // windows and lamps become the light in the frame, which is
                    // the whole point of the gate.
                    x3::rhi::IRenderDevice::SkyParams dsp{};
                    dsp.enabled  = true;
                    dsp.sunDir[0] = -0.94f; dsp.sunDir[1] = 0.055f; dsp.sunDir[2] = -0.33f;
                    dsp.cloud    = 0.35f;
                    dsp.exposure = 0.55f;
                    device->setSkyParams(dsp);
                }

                // THE TOWN WALKS IN CAPTURES TOO. ENGINE_GOTCHAS 4.4 is right
                // that a still cannot PROVE motion — but a still of six
                // unticked characters is worse than no still: an AnimatedCharacter
                // that never had update() called is a bind-pose statue, which is
                // exactly the T-pose defect NO_SLOP rule 1 catalogues. The settle
                // loop is a separate loop from the interactive one (the weather
                // and the river both learned this the expensive way, three
                // comments up), so the walk has to be wired into it explicitly.
                if (townOn) town.update(dt, *phys, *device, cam[0], cam[2]);
                // THE WORKS HAS TO LIVE IN THE CAPTURE LOOP TOO. This is the
                // same trap the weather block above documents, and gotcha 4.1b
                // (echotropolis' streamed content, never ticked in the settle
                // loop, so every still showed an empty bay): a plume ticked
                // only in the interactive loop is a plume no screenshot can
                // ever prove. The settle is 200 frames at 1/60 s = 3.3 s of
                // stack time, which is a real column by capture.
                if (facOn) factory.update(scene, dt);

                if (i == kFrames - 1) device->armCapture(out.c_str());
                auto frame = device->beginFrame();
                if (frame.valid) {
                    scene.render(*device, frame);
                    trees.draw(*device, frame);
                    if (townOn) town.draw(*device, frame);
                    gasStations.draw(*device, frame);
                    // forests: camera fwd = (cos yaw, 0, sin yaw) — gotcha 4.1
                    forests.draw(*device, frame, cam,
                                 std::cos(cam[3]), std::sin(cam[3]));
                    if (facOn) factory.drawSmoke(*device, cam);
                    tickets.drawGlints(*device);
                    if (carBuilt) car.render(frame);
                    traffic.render(frame, cam);
                    riverLife.render(*device, frame, scene);
                    if (shotDraw) shotDraw(frame);   // the staged swimmer
                    // The ticket HUD in a STILL, but ONLY once a card has been
                    // taken (X3_TICKETS=n, or the console). At the default 0/5
                    // this draws nothing, so every existing reference capture in
                    // this host is byte-identical — the whole point of gating it
                    // on state rather than on a second env knob.
                    if (tickets.collected() > 0)
                        tickets.drawHud(*device, frame, cam[0], cam[1], cam[2]);
                    if (weatherOn) precip.submit(*device, frame);
                    // ---- X3_SHOT_PUMP=1: the PUMP PROOF. Opt-in and default
                    // off (no existing reference capture moves), for the same
                    // reason ECHO_SHOT_STREAMED exists: the HUD lives only in
                    // the interactive loop, so a still of the forecourt could
                    // never show the hint the player is actually offered, and
                    // "E  REFUEL works" would be an untested claim on a
                    // screenshot. This runs the REAL proximity check against
                    // the camera position and draws the REAL prompt + gauge
                    // through gas_station.h's shared renderers.
                    if (shotPump) {
                        // =2: hold E only over the LAST 90 settle frames. Held
                        // for all 200 the pump moves 200/60*22 = 73 L — more
                        // than the whole tank — so the still lands on TANK
                        // FULL every time; 90 frames moves ~33 L from the
                        // staged 35% and the capture catches the flow mid-fill.
                        const bool holdE = shotPumpHoldE && i >= kFrames - 90;
                        gasStations.update(1.0f / 60.0f, cam[0], cam[2], 0.0f,
                                           0.0f, holdE);
                        x3::game::drawPumpPrompt(*device, frame, gasStations.prompt());
                        uint32_t phw = 0, phh = 0; device->hudSize(phw, phh);
                        float pR = 0.0f, pgx = 0.0f, pgy = 0.0f;
                        gaugeClusterAnchor((float)phw, (float)phh, pR, pgx, pgy);
                        x3::game::drawFuelBar(*device, frame, gasStations.fuel(),
                                              gasStations.refuelling(), pR, pgx, pgy);
                    }
                }

                device->endFrame(frame);
                if (i >= kFrames - 60) {   // the settled window only
                    const x3::rhi::RenderStats st = device->stats();
                    perfMsSum += st.gpuFrameMs; ++perfN;
                    // convergence trend: is the window actually settled, or is
                    // the streamer still uploading tiles through it?
                    if (i == kFrames - 60 || i == kFrames - 40 || i == kFrames - 20 ||
                        i == kFrames - 1) {
                        char tb[96];
                        std::snprintf(tb, sizeof(tb), "[tunnel-perf]   f%03d gpu %.3f ms",
                                      i, st.gpuFrameMs);
                        x3::logInfo(tb);
                    }
                    if (i == kFrames - 1) {
                        perfTris = st.triangles; perfDraws = st.drawCalls;
                        perfObjs = st.objectsDrawn;
                    }
                }
            }
            {
                char pb[256];
                // TRAFFIC COUNT ON THE PERF LINE. The first ON/OFF pair read
                // bit-identical (tris/draws/objs to the digit) because zero
                // cars were live at that camera — a perf "receipt" for a
                // system that never drew. The live count now rides the same
                // line, so an empty frame can never again be reported as a
                // cheap one.
                std::snprintf(pb, sizeof(pb),
                              "[tunnel-perf] %s: gpu %.3f ms avg (settled 60f) "
                              "= %.0f fps | tris %llu draws %u objs %u | traffic %u live (%u loose)",
                              out.c_str(), perfN ? perfMsSum / perfN : 0.0,
                              perfN && perfMsSum > 0.0 ? 1000.0 / (perfMsSum / perfN) : 0.0,
                              (unsigned long long)perfTris, perfDraws, perfObjs,
                              traffic.liveCount(), traffic.looseCount());
                x3::logInfo(pb);
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
        } else if (hc.townShot) {
            // ==== --screenshot-town: the W-TOWN eye gate ====================
            // Five stills the lane is judged on. Every camera is DERIVED from
            // the town's own placement data (Town::showcaseCamera) rather than
            // typed in — ENGINE_GOTCHAS 4.1's rule, learned from cameras
            // embedded in walls.
            const std::string tdir = hc.townShotDir;
            fs::create_directories(tdir, ec);
            if (!townOn) {
                x3::logError("--screenshot-town: no town was built (see the "
                             "summit-spur log above) — nothing to capture");
                ok = false;
            }
            static const char* const kTownShotName[x3::game::Town::kShots] = {
                "01_main_street", "02_shop_front", "03_pedestrians",
                "04_dusk_windows", "05_from_the_valley",
            };
            for (int sIdx = 0; townOn && sIdx < x3::game::Town::kShots; ++sIdx) {
                float cam[5];
                if (!town.showcaseCamera(sIdx, cam)) {
                    x3::logError(std::string("--screenshot-town: no camera for shot ")
                                 + kTownShotName[sIdx]);
                    ok = false;
                    continue;
                }
                // Shot 3 is the DUSK gate: drop the sun to the horizon and dim
                // the sky so the shop windows are the light in the frame. The
                // settle loop re-pushes its own SkyParams every frame when
                // weather is on, so the dusk sky has to be re-armed per frame;
                // that is what the townDusk flag below does.
                townDusk = (sIdx == 3);
                char path[512];
                std::snprintf(path, sizeof(path), "%s/%s.png", tdir.c_str(),
                              kTownShotName[sIdx]);
                char cb[256];
                std::snprintf(cb, sizeof(cb),
                              "--screenshot-town: %s cam=(%.1f, %.1f, %.1f) yaw=%.3f pitch=%.3f",
                              kTownShotName[sIdx], cam[0], cam[1], cam[2], cam[3], cam[4]);
                x3::logInfo(cb);
                ok = settleAndGrab(cam, path) && ok;
            }
            townDusk = false;
        } else if (!hc.jakeShot) {
            float cam[5]; tunnel.showcaseCamera(route, 0, cam);
            if (hc.shotCamOverride) for (int k = 0; k < 5; ++k) cam[k] = hc.shotCam[k];
            {   // Log the resolved camera (parity with the multi-shot branch): a
                // custom --shot-cam is DERIVED from this print, not eyeballed
                // (ENGINE_GOTCHAS 4.1 — derive cameras from data).
                char cb[192];
                std::snprintf(cb, sizeof(cb), "--world tunnel: shot cam=(%.1f, %.1f, %.1f) yaw=%.3f pitch=%.3f",
                              cam[0], cam[1], cam[2], cam[3], cam[4]);
                x3::logInfo(cb);
            }
            // ---- W-MENU: staged UI proofs (X3_SHOT_UI=menu|wx|light|preset).
            // The menu/panels draw only in the interactive loop, so a still
            // has to STAGE them — through the REAL host_menu.h draw code with
            // synthetic input (the X3_SHOT_PUMP pattern), never a mock-up.
            // Opt-in and default-off: no existing reference capture moves.
            const char* shotUi = std::getenv("X3_SHOT_UI");
            if (shotUi && shotUi[0]) {
                // A real console for the panels to read/write — the same two
                // registration calls the interactive shell console makes.
                std::unique_ptr<x3::con::IConsole> uiCon(x3::con::createConsole());
                x3::game::registerEngineConsoleCVars(*uiCon);
                registerWeatherConsole(*uiCon);
                WorldGameMenu uiMenu;
                uiMenu.init(uiCon.get(), device);
                uint32_t shw = 0, shh = 0; device->hudSize(shw, shh);

                if (std::strcmp(shotUi, "preset") == 0) {
                    // THE BEFORE/AFTER PAIR — same camera, the ONLY delta is
                    // what the SUGGESTED SETTINGS button toggles (DDGI on with
                    // kSuggestedLighting's intensity/rays + RT AO in, SSAO
                    // out). Exposure is identical in both halves by design
                    // (kSuggested/kDefault agree), so the pair isolates GI.
                    ok = settleAndGrab(cam, dir + "/ui_preset_before.png");
                    {
                        x3::rhi::IRenderDevice::DdgiParams dg{};
                        dg.enabled      = kSuggestedLighting.ddgi;
                        dg.intensity    = kSuggestedLighting.ddgiIntensity;
                        dg.raysPerProbe = kSuggestedLighting.ddgiRays;
                        // A still centres the volume once where the camera is
                        // (the interactive host scrolls it with the car).
                        dg.originX = cam[0] - 210.0f; dg.originY = cam[1] - 45.0f;
                        dg.originZ = cam[2] - 210.0f;
                        dg.sizeX = 420.0f; dg.sizeY = 150.0f; dg.sizeZ = 420.0f;
                        device->setDdgiParams(dg);
                        x3::rhi::IRenderDevice::RtaoParams rp{};
                        rp.enabled = kSuggestedLighting.rtao;
                        device->setRtaoParams(rp);
                        x3::rhi::IRenderDevice::SsaoParams sp2{};
                        sp2.enabled = kSuggestedLighting.ssao;
                        device->setSsaoParams(sp2);
                        x3::logInfo("[tunnel] X3_SHOT_UI=preset: applied kSuggestedLighting "
                                    "(DDGI on, RT AO in, SSAO out) for the AFTER half");
                    }
                    ok = settleAndGrab(cam, dir + "/ui_preset_after.png") && ok;
                } else {
                    std::string out2 = dir + "/ui_menu.png";
                    x3::ui::UiInput sIn{};
                    if (std::strcmp(shotUi, "wx") == 0) {
                        out2 = dir + "/ui_weather.png";
                        uiMenu.togglePanel(WorldGameMenu::Screen::Weather);
                        // MID-DRAG on the RAIN row: cursor held down on the
                        // track at ~7 of 10 — the slider chases it through the
                        // same applyRainScale the console `rain` command uses.
                        float pxp = 0.0f, pyp = 0.0f;
                        WorldGameMenu::weatherRowTrackPoint((float)shw, (float)shh,
                                                            0, 0.70f, pxp, pyp);
                        sIn.mouseX = pxp; sIn.mouseY = pyp; sIn.mouseDown = true;
                    } else if (std::strcmp(shotUi, "light") == 0) {
                        out2 = dir + "/ui_lighting.png";
                        uiMenu.togglePanel(WorldGameMenu::Screen::Lighting);
                        sIn.mouseX = (float)shw * 0.12f;   // hovering the panel
                        sIn.mouseY = (float)shh * 0.5f;
                    } else {
                        uiMenu.toggleMenu();
                        sIn.mouseX = (float)shw * 0.5f;    // hovering the rows
                        sIn.mouseY = (float)shh * 0.47f;
                    }
                    bool sinFirst = true;
                    shotDraw = [&](const x3::rhi::FrameContext& fr) {
                        x3::ui::UiInput in2 = sIn;
                        in2.mousePressed = sIn.mouseDown && sinFirst;
                        sinFirst = false;
                        uiMenu.draw(fr, in2, dt, 14.0f);
                    };
                    ok = settleAndGrab(cam, out2);
                    shotDraw = nullptr;
                }
            } else {
                const std::string out = screenshot ? screenshotPath : std::string("w_tunnel.png");
                ok = settleAndGrab(cam, out);
            }
        }

        // ==== --screenshot-jake: the ON-FOOT ANIMATED-CHARACTER proof set ====
        // Drives the REAL Player capsule + the shared AnimatedCharacter module
        // (app/character_anim.h) with synthetic input — no key faking, the
        // same code the interactive loop runs — and captures each gate:
        // grounded at origin-height with ZERO trims (the GLB-bake proof),
        // walking TOWARD the camera facing correctly, strafes both ways
        // (mirror check), backpedal, jump mid-flight, fall, and the three F1
        // camera modes.
        if (hc.jakeShot) {
            const std::string jdir = hc.jakeShotDir;
            fs::create_directories(jdir, ec);
            const float jx = startPos[0] + route.dirX * 30.0f;
            const float jz = startPos[2] + route.dirZ * 30.0f;
            auto groundAt = [&](float x, float z) -> float {
                float gy = x3::game::terrainHeightAtWorld(x, z);
                const x3::phys::RayHit rh = phys->rayCast(
                    x3::phys::Vec3{ x, gy + 60.0f, z },
                    x3::phys::Vec3{ 0.0f, -1.0f, 0.0f }, 150.0f,
                    x3::phys::Layer::Static);
                if (rh.hit) gy = std::max(gy, rh.point.y);
                return gy;
            };
            const float jgy = groundAt(jx, jz);
            onFoot.spawn(*phys, jx, jgy + 0.4f, jz);
            jake.load(*device, x3::game::assetRoot() + "/rigged_glb",
                      "Jake_44_actions.glb", x3::game::jakeClipTable());

            auto placeJake = [&](float lift) {
                onFoot.setFeetPosition(*phys,
                    x3::phys::Vec3{ jx, groundAt(jx, jz) + 0.2f + lift, jz });
            };
            // camMode 0/1/2 are the real F1 camera modes (characterCameraEye).
            // kCamGunRig is a CAPTURE-ONLY rig — see its use below.
            constexpr int kCamGunRig = 3;
            // One proof sequence: `frames` sim steps of (moveFwd, moveStrafe,
            // sprint) at a fixed look yaw; jumpAt >= 0 presses Space on that
            // frame; fixedCam (x,y,z,yaw,pitch) or camMode (F1 modes, or
            // kCamGunRig) frames the shot; the LAST frame is captured.
            //
            // `groundedTail` > 0 instead picks the capture frame BY MEASUREMENT
            // (NO_SLOP rule 9) over the last `groundedTail` frames: a locomotion
            // still shot on an arbitrary frame lands in the stride's FLIGHT
            // phase about half the time, and a runner hanging 0.15 m over the
            // tarmac is exactly the read THE CONTACT LAW exists to prevent —
            // the first cut of 21_rifle_run had both boots off the road. First
            // half of the window (longer than the 0.71 s Riflerun cycle, so a
            // foot-strike is guaranteed inside it) only MEASURES the lower toe
            // bone's clearance over the capsule's feet plane; the second half
            // arms on the first frame that returns to within 1.5 cm of the
            // measured minimum, then stops. No magic frame numbers.
            auto jakeSeq = [&](const char* name, int frames, float mf, float ms,
                              bool sprint, int jumpAt, float lookYaw,
                              const float* fixedCam, int camMode,
                              const std::function<void(int)>& act = {},
                              int groundedTail = 0) -> bool {
                char out[512];
                std::snprintf(out, sizeof(out), "%s/%s.png", jdir.c_str(), name);
                const int gWinLo = (groundedTail > 0) ? frames - groundedTail : frames;
                const int gWinMid = gWinLo + groundedTail / 2;
                float gMinClear = 1e9f;
                for (int i = 0; i < frames; ++i) {
                    glfwPollEvents();
                    const x3::phys::Vec3 f0 = onFoot.feet();
                    streamer.update(scene, *device, *phys,
                                    (i == 1) ? f0.x + 40.0f : f0.x, f0.z);
                    x3::game::PlayerInput pin;
                    pin.moveFwd = mf; pin.moveStrafe = ms; pin.sprint = sprint;
                    pin.jumpPressed = (i == jumpAt);
                    onFoot.setLook(lookYaw, 0.0f);
                    onFoot.update(pin, dt, *phys);
                    phys->step(dt);
                    // Weapons-proof hook: fire / reload / toss on chosen frames
                    // through the SAME shipped code paths the live loop binds.
                    if (act) act(i);
                    rifle.tick(dt);
                    tickGrenades(dt);
                    combatFx.update(dt);
                    x3::game::AnimatedCharacter::Intent ji;
                    ji.moveFwd = mf; ji.moveStrafe = ms; ji.sprint = sprint;
                    ji.jumpPressed = pin.jumpPressed;
                    jake.update(onFoot, ji, lookYaw, dt, *phys, *device);
                    float cam[5];
                    if (fixedCam) { for (int k = 0; k < 5; ++k) cam[k] = fixedCam[k]; }
                    else if (camMode == kCamGunRig) {
                        // THE WEAPON-PROOF RIG (camMode 3 — not an F1 mode).
                        // MEASURED defect it exists to kill: at camFront's 12 m
                        // a 0.62 m held rifle is ~20 px wide, and F1 mode 1/2
                        // sit BEHIND the back where Jake's own torso occludes
                        // the gun — the first weapons proof set could not show
                        // whether he was holding a rifle or a brick. This rides
                        // 3.4 m off his FRONT-RIGHT (the rifle hand is +X of a
                        // -Z facing) at chest height, FOLLOWING the capsule so
                        // it works for moving shots too. Framed so the feet
                        // stay in frame: at 3.4 m the 74 deg lens shows 2.88 m
                        // of height, aimed at feet+1.30 -> covers -0.14..2.74.
                        // THE CONTACT LAW must be readable in a weapon shot.
                        const x3::phys::Vec3 fz = onFoot.feet();
                        const float px = fz.x + 2.3f, py = fz.y + 1.55f,
                                    pz = fz.z - 2.5f;
                        const float ddx = fz.x - px, ddy = (fz.y + 1.30f) - py,
                                    ddz = fz.z - pz;
                        cam[0] = px; cam[1] = py; cam[2] = pz;
                        cam[3] = std::atan2(ddz, ddx);
                        cam[4] = std::atan2(ddy, std::sqrt(ddx * ddx + ddz * ddz));
                    }
                    else x3::game::characterCameraEye(onFoot, camMode, cam[0],
                                                      cam[1], cam[2], cam[3], cam[4]);
                    device->setCamera(cam[0], cam[1], cam[2], cam[3], cam[4], 74.0f);
                    // THE CONTACT-FRAME PICKER (see groundedTail above). The
                    // rig is already posed for THIS frame (jake.update ran), so
                    // the toe reading and the capture arm the same image.
                    bool armNow = (i == frames - 1);
                    if (groundedTail > 0 && i >= gWinLo && i < frames - 1) {
                        float lm[16], rm[16];
                        const bool okL = jake.boneWorld("mixamorigLeftToeBase",
                                                        onFoot, 0.0f, 0.0f, lm);
                        const bool okR = jake.boneWorld("mixamorigRightToeBase",
                                                        onFoot, 0.0f, 0.0f, rm);
                        if (okL || okR) {
                            const float fy = onFoot.feet().y;
                            float clear = 1e9f;
                            if (okL) clear = std::min(clear, lm[13] - fy);
                            if (okR) clear = std::min(clear, rm[13] - fy);
                            if (i < gWinMid) gMinClear = std::min(gMinClear, clear);
                            else if (clear <= gMinClear + 0.015f) armNow = true;
                        }
                    }
                    if (armNow) {
                        device->armCapture(out);
                        if (groundedTail > 0)
                            x3::logInfo("--screenshot-jake: " + std::string(name) +
                                        " armed on a FOOT-CONTACT frame (" +
                                        std::to_string(i) + "/" + std::to_string(frames) +
                                        ", min toe clearance " +
                                        std::to_string(gMinClear) + " m)");
                    }
                    auto fr = device->beginFrame();
                    if (fr.valid) {
                        scene.render(*device, fr);
                        trees.draw(*device, fr);
                        gasStations.draw(*device, fr);
                        if (carBuilt) car.render(fr);
                        jake.draw(fr, *device, onFoot, 0.0f, 0.0f,
                                  fixedCam != nullptr || camMode != 0);
                        // The held rifle + combat FX, exactly as the live loop
                        // draws them (no-ops while holstered / pool empty).
                        if (rifleArmed) {
                            float wm[16];
                            if (heldRifleWorld(wm))
                                rifle.drawCurrentAt(*device, fr, wm);
                        }
                        combatFx.draw(*device, fr, cam[0], cam[1], cam[2],
                                      cam[3], cam[4]);
                        combatFx.submit(*device, fr);
                        if (rifleArmed) {
                            char ab[48];
                            const auto& ws = rifle.currentState();
                            if (rifle.isReloading())
                                std::snprintf(ab, sizeof(ab), "RELOADING...");
                            else
                                std::snprintf(ab, sizeof(ab), "RIFLE  %d / %d",
                                              ws.ammoInMag, ws.reserve);
                            uint32_t hw = 0, hh = 0; device->hudSize(hw, hh);
                            if (hw && hh) {
                                const float px = std::floor((float)hh * 0.026f);
                                const float amc[4] = { 1.0f, 0.93f, 0.72f, 1.0f };
                                device->drawHudText(fr, ab, (float)hh * 0.045f,
                                                    (float)hh * 0.92f, px, amc);
                            }
                            if (rifleAiming) wpnHud.drawCrosshair(*device, fr);
                        }
                    }
                    device->endFrame(fr);
                    if (armNow) break;   // the armed frame is the shot; stop here
                }
                const bool wrote = device->captureFrame(out);
                if (wrote) x3::logInfo(std::string("--screenshot-jake: wrote ") + out);
                else       x3::logError(std::string("--screenshot-jake: capture FAILED ") + out);
                return wrote;
            };

            // Camera yaw convention: fwd = (cos y, ., sin y). Jake's default
            // facing is engine -Z, i.e. look yaw -pi/2.
            const float kFace = -1.5707963f;         // player looks -Z
            const float gy0 = groundAt(jx, jz);
            // Front camera: 12 m out on -Z, looking BACK (+Z) at him — far
            // enough that the walk-toward sequence cannot reach the near
            // plane (the first cut at 7 m ended inside his chest).
            const float camFront[5] = { jx, gy0 + 1.3f, jz - 12.0f, +1.5707963f, -0.04f };
            // Side camera: 7 m out on +X, looking -X, for jump/fall arcs.
            const float camSide[5]  = { jx + 7.0f, gy0 + 1.6f, jz, 3.14159265f, -0.03f };

            placeJake(0.0f);
            ok = jakeSeq("10_idle_grounded", 150, 0, 0, false, -1, kFace, camFront, -1) && ok;
            placeJake(0.0f);
            ok = jakeSeq("11_walk_toward",    60, 1, 0, false, -1, kFace, camFront, -1) && ok;
            placeJake(0.0f);
            ok = jakeSeq("12_strafe_D",       70, 0, +1, false, -1, kFace, nullptr, 2) && ok;
            placeJake(0.0f);
            ok = jakeSeq("13_strafe_A",       70, 0, -1, false, -1, kFace, nullptr, 2) && ok;
            placeJake(0.0f);
            ok = jakeSeq("14_backpedal",      80, -1, 0, false, -1, kFace, nullptr, 2) && ok;
            placeJake(0.0f);
            ok = jakeSeq("15_jump_midair",    46, 0, 0, false, 20, kFace, camSide, -1) && ok;
            placeJake(6.0f);                          // drop from 6 m: fall pose
            ok = jakeSeq("16_fall",           40, 0, 0, false, -1, kFace, camSide, -1) && ok;
            placeJake(0.0f);
            ok = jakeSeq("17_cam_first",      80, 1, 0, false, -1, kFace, nullptr, 0) && ok;
            placeJake(0.0f);
            ok = jakeSeq("18_cam_near",       80, 1, 0, false, -1, kFace, nullptr, 1) && ok;
            placeJake(0.0f);
            ok = jakeSeq("19_cam_far",        80, 1, 0, false, -1, kFace, nullptr, 2) && ok;

            // ==== WEAPONS PROOF (owner: "Does he have his weapons?") ========
            // The SAME shipped paths the live loop binds — setRifleArmed /
            // fireRifleOnce / rifle.reload / releaseGrenade — staged frame-
            // accurately. Every shot obeys THE CONTACT LAW (placeJake grounds
            // the capsule; the module clamps every frame).
            setRifleArmed(true);
            if (rifleArmed) {
                // 20: rifle IN HAND, at the ready (Rifleaimingidle). GUN RIG —
                // this is THE shot that answers "does he have his weapons?",
                // so the gun has to be legible: model, texture and which way
                // the barrel points, with the boots still on the road.
                placeJake(0.0f);
                ok = jakeSeq("20_rifle_idle", 120, 0, 0, false, -1, kFace, nullptr, kCamGunRig) && ok;
                // 21: armed run (Riflerun swapped into the blend). Gun rig
                // FOLLOWS him, so the carry pose reads while he is moving, and
                // groundedTail puts the shutter on a FOOT-STRIKE frame — the
                // 70-frame cut landed in the flight phase with both boots
                // 0.15 m over the tarmac (measured off the capture).
                placeJake(0.0f);
                ok = jakeSeq("21_rifle_run",  160, 1, 0, false, -1, kFace, nullptr, kCamGunRig,
                             {}, 100) && ok;
                // 22: RMB aim — over-the-near-shoulder frame + HUD crosshair.
                placeJake(0.0f);
                rifleAiming = true; jake.setAiming(true);
                ok = jakeSeq("22_aim_shoulder", 90, 0, 0, false, -1, kFace, nullptr, 1) && ok;
                // 23: mid-burst — trigger held from frame 60; the LAST frame
                // fires too, so the 0.04 s muzzle flash + tracer are live in
                // the capture.
                ok = jakeSeq("23_fire_muzzle", 90, 0, 0, false, -1, kFace, nullptr, 1,
                             [&](int i) { if (i >= 60) fireRifleOnce(); }) && ok;
                // 23b: the SAME burst from the gun rig. Shot 23 proves the
                // tracer runs to the crosshair; from behind the back it cannot
                // prove the flash leaves the BARREL TIP. This one can — the
                // muzzle FX ride heldRifleMuzzle(), the MEASURED vmMuzzle under
                // the same matrix the gun draws with.
                placeJake(0.0f);
                ok = jakeSeq("23b_fire_close", 90, 0, 0, false, -1, kFace, nullptr, kCamGunRig,
                             [&](int i) { if (i >= 60) fireRifleOnce(); }) && ok;
                rifleAiming = false; jake.setAiming(false);
                // 24: reload mid-clip (Reloading is 3.29 s; begin at frame 10,
                // capture ~1.6 s in — hands at the receiver, "RELOADING..."
                // HUD). GUN RIG: the front cam was too far and the over-the-
                // shoulder cam puts his own back between lens and receiver.
                placeJake(0.0f);
                ok = jakeSeq("24_reload_mid", 106, 0, 0, false, -1, kFace, nullptr, kCamGunRig,
                             [&](int i) {
                                 if (i == 10 && rifle.reload()) jake.reloadOneShot();
                             }) && ok;
                // 25: grenade ARC — toss at 5, ball leaves at the arm swing
                // (69 frames ≈ 1.15 s), captured ~0.75 s into flight. FOLLOW
                // camera (far): the fixed side cam proved the throw exits the
                // frame in under a second — behind Jake the arc flies AHEAD
                // and stays centered (glowing core + smoke trail).
                placeJake(0.0f);
                ok = jakeSeq("25_grenade_arc", 119, 0, 0, false, -1, kFace, nullptr, 2,
                             [&](int i) {
                                 if (i == 5)  jake.grenadeOneShot();
                                 if (i == 74) releaseGrenade();
                             }) && ok;
                // 26: DETONATION — same staging, run through the 2.2 s fuse
                // (boom at frame ~206), captured 6 frames after the fireball
                // ignites, ~20 m ahead on the roadway.
                placeJake(0.0f);
                ok = jakeSeq("26_grenade_boom", 212, 0, 0, false, -1, kFace, nullptr, 2,
                             [&](int i) {
                                 if (i == 5)  jake.grenadeOneShot();
                                 if (i == 74) releaseGrenade();
                             }) && ok;
                // 27: HOLSTERED — back to the unarmed melee layer (hooks combo
                // mid-swing, no rifle in hand). Gun rig: same lens as 20, so
                // "gun there / gun gone" is an A/B a reader can actually make.
                setRifleArmed(false);
                placeJake(0.0f);
                ok = jakeSeq("27_holster_punch", 30, 0, 0, false, -1, kFace, nullptr, kCamGunRig,
                             [&](int i) {
                                 if (i == 6) jake.playOneShot("Backflip_and_Hooks");
                             }) && ok;
            } else {
                x3::logError("--screenshot-jake: rifle failed to arm (GLB missing?) — weapons proofs skipped");
                ok = false;
            }
        }

        // ==== SWIM PROOF (X3_SHOT_SWIM=1) — Jake treading mid-channel, then
        // FULLY SUBMERGED over the 18 ft bed. The REAL Player swim state (the
        // same W10 machine the interactive path runs, water-fed above) and the
        // REAL Jake rig, staged on the headless path so the gate has eyes.
        if (const char* se = std::getenv("X3_SHOT_SWIM");
            se && se[0] == '1' && riverOn && riverRoad.plan.ok) {
            // Mid-channel, ~32 m along the reach from the deck (out from under
            // the bridge shadow): the reach axis is the deck axis rotated 90°.
            const auto& plan = riverRoad.plan;
            const float rdx = plan.dirZ, rdz = -plan.dirX;   // river direction
            const float sx = plan.cx + rdx * 32.0f;
            const float sz = plan.cz + rdz * 32.0f;
            const float wy = plan.waterY;

            // Jake's rig — the SAME shared-module load the interactive exit
            // runs (W-RIVER's hand-rolled Skinner recipe + |restY| yFix are
            // gone: the baked asset has feet at origin, so the module draws
            // at the capsule's feet with ZERO compensation, land or water;
            // Swim/SwimIdle come from the measured clip table).
            jakeTried = true;
            jake.load(*device, x3::game::assetRoot() + "/rigged_glb",
                      "Jake_44_actions.glb", x3::game::jakeClipTable());
            onFoot.spawn(*phys, sx, wy + 0.6f, sz);   // drops in, swim state takes him
            const float downstreamYaw = std::atan2(rdz, rdx);   // camYaw convention
            bool diveHeld = false;
            shotTick = [&](float d) {
                x3::game::PlayerInput pin;            // no move — tread / sink only
                pin.diveHeld = diveHeld;
                onFoot.update(pin, d, *phys);
                x3::game::AnimatedCharacter::Intent ji;   // idle intent
                jake.update(onFoot, ji, downstreamYaw, d, *phys, *device);
            };
            shotDraw = [&](const x3::rhi::FrameContext& fr) {
                jake.draw(fr, *device, onFoot, 0.0f, 0.0f, true);
            };

            // PRE-SETTLE: he spawns just over the surface and drops in; the
            // swim state's buoyancy then floats him to rest at the surface.
            // Give that 6 seconds of physics BEFORE the first framed capture,
            // or the shot catches him mid-sink standing on the bed.
            for (int i = 0; i < 360; ++i) { shotTick(dt); phys->step(dt); }
            {   // The gate's instruments, not vibes: where is he, is the water
                // query wet there, and did the swim state actually engage?
                const x3::phys::Vec3 ft = onFoot.feet();
                const float wq = x3::game::worldWaterLevelAt(ft.x, ft.z);
                char sb[224];
                std::snprintf(sb, sizeof(sb),
                    "[swim-shot] feet=(%.1f, %.2f, %.1f) waterQ=%.2f (plane %.2f) "
                    "depth=%.2f swimming=%d",
                    ft.x, ft.y, ft.z, wq, wy,
                    (wq > x3::game::kWorldWaterDry + 1.0f) ? wq - ft.y : -1.0f,
                    onFoot.swimming() ? 1 : 0);
                x3::logInfo(sb);
            }

            // SHOT A — treading at the surface: head/shoulders above, the body
            // refracting below. Framed off his LIVE feet, chest at the
            // waterline in the image center.
            {
                const x3::phys::Vec3 ft = onFoot.feet();
                const float px = ft.x - rdz * 4.5f, pz = ft.z + rdx * 4.5f;
                const float yawA = std::atan2(ft.z - pz, ft.x - px);
                const float camY = wy + 1.0f;
                const float pitchA = std::atan2((wy - 0.2f) - camY, 4.5f);
                const float camA[5] = { px, camY, pz, yawA, pitchA };
                ok = settleAndGrab(camA, dir + "/10_swim_tread.png") && ok;
            }
            // SHOT B — fully submerged: dive held, camera UNDER the surface
            // beside him — bed ~5.5 m below, surface overhead, green fog on
            // (the interactive camera block owns the fog edge; this staged
            // path sets it directly, then clears it).
            {
                diveHeld = true;
                x3::rhi::IRenderDevice::FogParams fp{};
                fp.enabled  = true;
                fp.color[0] = 0.010f; fp.color[1] = 0.045f; fp.color[2] = 0.055f;
                fp.density  = 0.055f; fp.start = 0.15f; fp.maxOpacity = 0.96f;
                device->setFog(fp);
                // Let the DIVE finish before framing: 240 held frames take him
                // to the bed, THEN the camera is aimed at where he actually is
                // (mid-column, tipped to hold him, the bed and the caustics in
                // one frame with the surface underside closing the top).
                for (int i = 0; i < 240; ++i) { shotTick(dt); phys->step(dt); }
                const x3::phys::Vec3 ft = onFoot.feet();
                char sb[192];
                std::snprintf(sb, sizeof(sb),
                    "[swim-shot] after dive feet=(%.1f, %.2f, %.1f) bedHere=%.2f",
                    ft.x, ft.y, ft.z,
                    x3::game::terrainHeightAtWorld(ft.x, ft.z));
                x3::logInfo(sb);
                const float px = ft.x - rdz * 7.0f, pz = ft.z + rdx * 7.0f;
                const float yawB = std::atan2(ft.z - pz, ft.x - px);
                const float camBY = wy - 1.6f;
                const float pitchB = std::atan2((ft.y + 1.0f) - camBY, 7.0f);
                const float camB[5] = { px, camBY, pz, yawB, pitchB };
                {   char cb2[160];
                    std::snprintf(cb2, sizeof(cb2),
                        "[swim-shot] camB=(%.1f, %.2f, %.1f) yaw=%.3f pitch=%.3f",
                        camB[0], camB[1], camB[2], camB[3], camB[4]);
                    x3::logInfo(cb2); }
                ok = settleAndGrab(camB, dir + "/11_swim_submerged.png") && ok;
                {   // CONTROL: same aim from ABOVE the surface — isolates
                    // "not drawn at the bed" from "not visible to an
                    // underwater camera".
                    const float cY2 = wy + 3.0f;
                    const float pitch2 = std::atan2((ft.y + 1.0f) - cY2, 7.0f);
                    const float camB2[5] = { px, cY2, pz, yawB, pitch2 };
                    ok = settleAndGrab(camB2, dir + "/11b_dive_above.png") && ok;
                }
                diveHeld = false;

                // SHOT C — THE FISH, from under the surface: aimed at the LIVE
                // center of the bream school (the schools drift downstream
                // through all this settling — the seed point is long stale).
                // Real pose-baked fish, the caustic bed and the green column.
                for (uint32_t si = 0; si < riverLife.fish().schoolCount(); ++si) {
                    const x3::game::FishSchool& sc = riverLife.fish().school(si);
                    if (sc.species != x3::game::FishSpecies::Bream) continue;
                    const float fsY = std::min(
                        x3::game::worldWaterLevelAt(sc.cx, sc.cz), wy);
                    const float fpx = sc.cx - rdz * 6.0f, fpz = sc.cz + rdx * 6.0f;
                    const float yawC = std::atan2(sc.cz - fpz, sc.cx - fpx);
                    const float camCY = fsY - 1.6f;
                    const float pitchC = std::atan2((fsY - 2.6f) - camCY, 6.0f);
                    const float camC[5] = { fpx, camCY, fpz, yawC, pitchC };
                    ok = settleAndGrab(camC, dir + "/12_fish_school.png") && ok;
                    break;
                }

                x3::rhi::IRenderDevice::FogParams off{};
                device->setFog(off);

                // SHOT D — A SPEEDBOAT MID-RUN WITH ITS WAKE: quarter-view
                // camera placed abeam of where the LIVE hull will be mid-way
                // through the settle (heading * speed lead — a fixed camera
                // cannot chase an 8 m/s boat, so it ambushes the lane).
                if (riverLife.boatCount() > 0) {
                    const uint32_t bi = riverLife.boatCount() > 1 ? 1u : 0u;
                    float bp[3]; riverLife.boatPos(bi, bp);
                    const float hd = riverLife.boatHeading(bi);
                    const float sp2 = std::max(4.0f, riverLife.boatSpeed(bi));
                    // HIGH quarter view: wide enough that waypoint turns and
                    // prediction error keep the hull in frame, and the foam
                    // trail reads as a LINE behind it.
                    const float lead = std::min(30.0f, sp2 * 1.6f);
                    const float tx2 = bp[0] + std::cos(hd) * lead;
                    const float tz2 = bp[2] + std::sin(hd) * lead;
                    const float cx2 = tx2 - std::cos(hd) * 18.0f - std::sin(hd) * 8.0f;
                    const float cz2 = tz2 - std::sin(hd) * 18.0f + std::cos(hd) * 8.0f;
                    const float camDY = wy + 16.0f;
                    const float dh = std::sqrt(18.0f * 18.0f + 8.0f * 8.0f);
                    const float yawD = std::atan2(tz2 - cz2, tx2 - cx2);
                    const float pitchD = std::atan2(wy - camDY, dh);
                    const float camD[5] = { cx2, camDY, cz2, yawD, pitchD };
                    ok = settleAndGrab(camD, dir + "/13_boat_wake.png") && ok;
                }
            }
            shotTick = nullptr;
            shotDraw = nullptr;
        }

        // ==== SUB PROOF (X3_SHOT_SUB=1) — the patrol submarine, framed off its
        // LIVE hull, twice: from the BRIDGE DECK (the owner's "visible from the
        // bridge") and from IN THE WATER beside it (the swimmer's view). Every
        // shot logs where the hull actually was and how far its top sat under
        // the local surface, because "it's a submarine" is a claim about a
        // NUMBER (submergence > 0) and not about vibes (NO_SLOP rule 9).
        if (const char* be = std::getenv("X3_SHOT_SUB");
            be && be[0] == '1' && riverOn && riverRoad.plan.ok && riverLife.subBuilt()) {
            const auto& plan = riverRoad.plan;
            const float rdx = plan.dirZ, rdz = -plan.dirX;   // downstream unit
            auto subLog = [&](const char* tag) {
                float sp[3]; riverLife.subPos(sp);
                char sb[224];
                std::snprintf(sb, sizeof(sb),
                    "[sub-shot] %s hull=(%.1f, %.2f, %.1f) submergence=%.2f m "
                    "surface=%.2f bed=%.2f",
                    tag, sp[0], sp[1], sp[2], riverLife.subSubmergence(),
                    x3::game::worldWaterLevelAt(sp[0], sp[2]),
                    x3::game::terrainHeightAtWorld(sp[0], sp[2]));
                x3::logInfo(sb);
            };
            subLog("pre-shot");

            // PRE-ROLL to a FRAMEABLE moment. The lane straddles the crossing,
            // so at an arbitrary frame the sub can be directly under the deck —
            // the first cut caught it at 4.4 m range and pitch -83°, a
            // top-down blob. Tick physics (no rendering needed) until the hull
            // is well out on the reach, then frame that. Deterministic: the
            // patrol is a fixed lane at a fixed speed.
            auto advanceSub = [&](float wantDist, int maxSteps) {
                for (int i = 0; i < maxSteps; ++i) {
                    float sp[3]; riverLife.subPos(sp);
                    const float ddx = sp[0] - plan.cx, ddz = sp[2] - plan.cz;
                    if (std::sqrt(ddx * ddx + ddz * ddz) >= wantDist) return;
                    riverLife.prePhysics(dt);
                    phys->step(dt);
                    riverLife.postPhysics(dt, scene, *device, *phys,
                                          audioOn ? audio.get() : nullptr,
                                          x3::phys::Vec3{ sp[0], sp[1], sp[2] });
                }
            };
            // The sub's own travel direction, sampled over 30 steps, so the
            // framing can LEAD it through settleAndGrab's 200-frame settle.
            auto subDir = [&](float& dx, float& dz) {
                float a[3]; riverLife.subPos(a);
                for (int i = 0; i < 30; ++i) {
                    riverLife.prePhysics(dt); phys->step(dt);
                    riverLife.postPhysics(dt, scene, *device, *phys,
                                          audioOn ? audio.get() : nullptr,
                                          x3::phys::Vec3{ a[0], a[1], a[2] });
                }
                float b[3]; riverLife.subPos(b);
                dx = b[0] - a[0]; dz = b[2] - a[2];
                const float l = std::sqrt(dx * dx + dz * dz);
                if (l > 1e-3f) { dx /= l; dz /= l; } else { dx = rdx; dz = rdz; }
            };

            // SHOT E — FROM THE BRIDGE. Standing on the deck at the crossing,
            // looking down the reach at the hull. The camera is on the DECK's
            // own Y (plan.deckY, eye height above it), not a staged crane, so
            // this is the view a player who stops on the bridge actually gets.
            {
                advanceSub(24.0f, 6000);
                float dirX = 0.0f, dirZ = 0.0f; subDir(dirX, dirZ);
                float sp[3]; riverLife.subPos(sp);
                // Lead the hull through the 200-frame settle (~3.3 s at ~1.2
                // m/s). Without this the sub walks a hull-length out of frame.
                sp[0] += dirX * 4.0f; sp[2] += dirZ * 4.0f;
                // Stand at the DOWNSTREAM PARAPET, not mid-deck: the first cut
                // put the eye on the centreline and the deck slab filled the
                // bottom half of the frame (43 ft out-to-out, and the reach
                // runs across the deck's narrow axis). deckHalfWidth + 0.8 m
                // puts the eye at the rail, where a player who stops on the
                // bridge to look at the river actually stands.
                // ...on the sub's OWN side of the deck (the lane straddles the
                // crossing, so the hull is upstream half the time).
                const float sideSgn = ((sp[0] - plan.cx) * rdx +
                                       (sp[2] - plan.cz) * rdz) >= 0.0f ? 1.0f : -1.0f;
                const float edge = (plan.deckHalfWidth + 0.8f) * sideSgn;
                const float ex = plan.cx + rdx * edge, ez = plan.cz + rdz * edge;
                const float ey = plan.deckY + 1.7f;
                const float ddx = sp[0] - ex, ddz = sp[2] - ez;
                const float hdist = std::sqrt(ddx * ddx + ddz * ddz);
                const float yawE = std::atan2(ddz, ddx);
                const float pitchE = std::atan2(sp[1] - ey, std::max(1.0f, hdist));
                const float camE[5] = { ex, ey, ez, yawE, pitchE };
                char cb[192];
                std::snprintf(cb, sizeof(cb),
                    "[sub-shot] bridge cam=(%.1f, %.2f, %.1f) yaw=%.3f pitch=%.3f "
                    "range=%.1f m", ex, ey, ez, yawE, pitchE, hdist);
                x3::logInfo(cb);
                ok = settleAndGrab(camE, dir + "/14_sub_from_bridge.png") && ok;
                subLog("after bridge shot");
            }

            // SHOT F — FROM IN THE WATER. Camera under the surface, abeam of
            // the hull, with the same green column fog the swim shots use (the
            // interactive camera owns that edge; the staged path sets it).
            {
                x3::rhi::IRenderDevice::FogParams fp{};
                fp.enabled  = true;
                fp.color[0] = 0.010f; fp.color[1] = 0.045f; fp.color[2] = 0.055f;
                fp.density  = 0.055f; fp.start = 0.15f; fp.maxOpacity = 0.96f;
                device->setFog(fp);
                // ABEAM OF THE HULL'S OWN HEADING, not of the reach axis: the
                // lane reverses, so half the time "13 m off the reach normal"
                // is dead ahead of the bow and the shot is a nose-on blob.
                // Perpendicular of the travel direction is (-dz, dx).
                float dirX = 0.0f, dirZ = 0.0f; subDir(dirX, dirZ);
                float sp[3]; riverLife.subPos(sp);
                // Lead the hull: the settle runs 200 more frames of physics and
                // a patrolling sub does not wait for the shutter.
                const float lead = 4.0f;
                const float tx = sp[0] + dirX * lead, tz = sp[2] + dirZ * lead;
                const float px = tx - dirZ * 13.0f, pz = tz + dirX * 13.0f;
                const float surf = x3::game::worldWaterLevelAt(px, pz);
                const float camFY = surf - 1.2f;
                const float yawF = std::atan2(tz - pz, tx - px);
                const float pitchF = std::atan2(sp[1] - camFY, 13.0f);
                const float camF[5] = { px, camFY, pz, yawF, pitchF };
                char cb[192];
                std::snprintf(cb, sizeof(cb),
                    "[sub-shot] underwater cam=(%.1f, %.2f, %.1f) yaw=%.3f pitch=%.3f "
                    "surface=%.2f", px, camFY, pz, yawF, pitchF, surf);
                x3::logInfo(cb);
                ok = settleAndGrab(camF, dir + "/15_sub_underwater.png") && ok;
                subLog("after underwater shot");
                x3::rhi::IRenderDevice::FogParams off{};
                device->setFog(off);
            }
        }

        // ==== BANK PROOF (X3_SHOT_BANKS=1) — the ONE WATER TRUTH, with eyes.
        // RB8's station sweep says in NUMBERS that the drawn river never tops a
        // bank anywhere on the run; this is the same claim with a picture, at
        // the WORST station the sweep can find — measured here the same way
        // (crest = highest ground between the waterline and 2x the half-width
        // out, on each side; freeboard = crest - the drawn water level).
        // Shoot it twice: once dry, once with X3_WEATHER=storm, and the pair is
        // the rain-runoff gate (#23) — the river visibly higher, still inside.
        if (const char* ke = std::getenv("X3_SHOT_BANKS");
            ke && ke[0] == '1' && riverOn) {
            uint32_t rn = 0;
            const x3::game::WorldRiverNode* nds = x3::game::worldRiverNodes(rn);
            const uint32_t carved = x3::game::worldRiverCarveCount();
            const char* wxEnv = std::getenv("X3_WEATHER");
            const std::string tag = (wxEnv && wxEnv[0] && std::strcmp(wxEnv, "0") != 0)
                                  ? std::string("_") + wxEnv : std::string("_dry");
            const float HW = x3::game::kWorldRiverHalfWidth;
            // LET THE RUNOFF FINISH ITS RAMP. The swell takes ~15 s to go from
            // 0.3 to 0.9 m and settleAndGrab is 200 frames = 3.3 s, so the
            // first cut of this proof caught the river only 0.20 m up and the
            // "rain-swollen" image looked like the dry one. Tick the weather
            // model alone (no rendering — this is the same integrator the
            // capture loop runs, just fast) for 30 s first.
            if (weatherOn) {
                for (int i = 0; i < 1800; ++i) {
                    weather.tick(dt);
                    const x3::game::WeatherSample& ws = weather.sample();
                    wetness.tick(dt, ws.precipitation, ws.tempC, ws.snowfall);
                    tickRiverRise(dt, ws.precipitation, ws.snowfall);
                }
                char wb[160];
                std::snprintf(wb, sizeof(wb),
                    "[bank-shot] weather pre-rolled 30 s: precipitation %.2f, "
                    "state %d", weather.sample().precipitation,
                    (int)weather.sample().state);
                x3::logInfo(wb);
            }
            // Freeboard at a node, and the side that is tighter.
            auto freeboardAt = [&](uint32_t i, float& outNx, float& outNz,
                                   float& outSide) -> float {
                const uint32_t j = (i + 1 < rn) ? i + 1 : i;
                const uint32_t k = (i > 0) ? i - 1 : i;
                float dx = nds[j].x - nds[k].x, dz = nds[j].z - nds[k].z;
                const float dl = std::sqrt(dx * dx + dz * dz);
                if (dl > 1e-3f) { dx /= dl; dz /= dl; }
                outNx = -dz; outNz = dx;                    // left-hand normal
                float best = 1e9f; outSide = 1.0f;
                for (int s = -1; s <= 1; s += 2) {
                    // The LEVEE BAND, exactly RB8's: half-width+0.5 .. +26 m.
                    // (The first cut swept HW..2*HW and graded the ocean floor
                    // as a river bank — see the estuary skip below.)
                    float crest = -1e9f;
                    for (float o = HW + 0.5f; o <= HW + 26.0f; o += 2.5f) {
                        const float px = nds[i].x + outNx * o * (float)s;
                        const float pz = nds[i].z + outNz * o * (float)s;
                        crest = std::max(crest, x3::game::terrainHeightAtWorld(px, pz));
                    }
                    const float fb = crest - x3::game::worldWaterLevelAt(nds[i].x, nds[i].z);
                    if (fb < best) { best = fb; outSide = (float)s; }
                }
                return best;
            };
            // THE ESTUARY IS NOT A RIVER (RB8's own words): inside the ocean
            // basin disc the shader hands the level to the sea and the basin
            // floor is 190 ft down — there are no banks to hold there.
            auto inBasin = [&](uint32_t i) {
                const float bx = nds[i].x - x3::game::kWorldOceanBasinX;
                const float bz = nds[i].z - x3::game::kWorldOceanBasinZ;
                return bx * bx + bz * bz <
                       x3::game::kWorldOceanBasinR * x3::game::kWorldOceanBasinR;
            };
            // The tightest carved station on the whole run — the one that
            // actually decides the claim.
            uint32_t worst = 1; float worstFb = 1e9f, lastRiver = 1.0f;
            for (uint32_t i = 1; i + 1 < std::max(2u, carved); ++i) {
                if (inBasin(i)) continue;
                lastRiver = (float)i;
                float nx, nz, sd;
                const float fb = freeboardAt(i, nx, nz, sd);
                if (fb < worstFb) { worstFb = fb; worst = i; }
            }
            const uint32_t lastIdx = (uint32_t)lastRiver;
            x3::logInfo("[bank-shot] tightest carved station is node " +
                        std::to_string(worst) + " at freeboard " +
                        std::to_string(worstFb) + " m (last river node " +
                        std::to_string(lastIdx) + ")");
            // FIXED stations, not "whichever is tightest right now": the dry
            // and the rain-swollen runs have to be the SAME camera or the pair
            // proves nothing (the first cut picked node 4 dry and node 7 in the
            // storm and the two images were of different places).
            // Node 2 is where the RISE is visible: the runoff cap is MEASURED
            // per node off the built ground and is 1.7 m there, so the swell
            // can express its full 0.9 m against a real bank. Nodes 4/6/7 are
            // the opposite end — the levee-critical band, cap 0.2 m, the
            // tightest freeboard on the whole run — which is where "never over
            // the levees" is actually at risk ([river-rain] prints the table).
            uint32_t stations[4] = { std::min(2u, lastIdx), std::min(4u, lastIdx),
                                     std::min(6u, lastIdx), std::min(7u, lastIdx) };
            for (int s = 0; s < 4; ++s) {
                const uint32_t i = stations[s];
                float nx, nz, sd;
                const float fb = freeboardAt(i, nx, nz, sd);
                const float wl = x3::game::worldWaterLevelAt(nds[i].x, nds[i].z);
                // Stand back on the DRY side, high enough that both banks and
                // the waterline are in one frame.
                const float back = HW * 2.4f;
                const float px = nds[i].x - nx * sd * back;
                const float pz = nds[i].z - nz * sd * back;
                // Eye ABOVE THE BANK IT STANDS BEHIND. The first cut measured
                // the eye off the water (wl + 9) and at node 2 — where the
                // crest is 22 m up — the camera ended INSIDE the wooded hill
                // and the "bank proof" was a picture of grass with no river in
                // it. Clear the crest, then look down at the waterline.
                const float py = std::max(wl + fb, wl) + 8.0f;
                const float yaw = std::atan2(nds[i].z - pz, nds[i].x - px);
                const float pitch = std::atan2(wl - py, back);
                const float cam[5] = { px, py, pz, yaw, pitch };
                char nm[256], lb[288];
                std::snprintf(nm, sizeof(nm), "%s/16_banks%s_%d.png",
                              dir.c_str(), tag.c_str(), s);
                std::snprintf(lb, sizeof(lb),
                    "[bank-shot] station node %u at (%.1f, %.1f): drawn water %.2f, "
                    "tightest bank crest %.2f, FREEBOARD %.3f m (%s) -> %s",
                    i, nds[i].x, nds[i].z, wl, wl + fb, fb,
                    fb > 0.0f ? "INSIDE ITS BANKS" : "OVER THE BANK",
                    nm);
                x3::logInfo(lb);
                if (fb <= 0.0f)
                    x3::logError("[bank-shot] WATER IS OVER THE BANK CREST at node " +
                                 std::to_string(i));
                ok = settleAndGrab(cam, nm) && ok;

                // THE WATERLINE FRAME (station 0 only) — the rain-runoff A/B.
                // A bank shot from 80 m up cannot show 0.75 m of swell: it is
                // ~9 px. So put the eye 1.4 m over the DRY surface, in the
                // ribbon, looking downstream. `nds[i].waterY` is the BASE table
                // (worldRiverRisenNodes builds the risen copy separately), so
                // the camera is at the SAME world Y in both runs by
                // construction — and the water climbing toward the lens is the
                // proof, measured in the log line below.
                if (s == 0) {
                    const float wx = nds[i].x + nx * sd * (HW - 6.0f);
                    const float wz = nds[i].z + nz * sd * (HW - 6.0f);
                    const float wyEye = nds[i].waterY + 1.4f;
                    const uint32_t j = (i + 1 < rn) ? i + 1 : i;
                    const float wyaw = std::atan2(nds[j].z - wz, nds[j].x - wx);
                    const float wcam[5] = { wx, wyEye, wz, wyaw, -0.045f };
                    char wn[256], wl2[288];
                    std::snprintf(wn, sizeof(wn), "%s/17_waterline%s.png",
                                  dir.c_str(), tag.c_str());
                    std::snprintf(wl2, sizeof(wl2),
                        "[bank-shot] waterline frame: eye Y %.2f (dry surface %.2f "
                        "+ 1.4), water NOW %.2f -> %.2f m under the lens "
                        "(risen %.2f m) -> %s",
                        wyEye, nds[i].waterY, wl, wyEye - wl,
                        wl - nds[i].waterY, wn);
                    x3::logInfo(wl2);
                    ok = settleAndGrab(wcam, wn) && ok;
                }
            }
        }

        // ==== MAP/HUD PROOF SET (map/HUD wiring) — overview / drive-zoom /
        // waypoint / driving-HUD chevron. Uses the engine's OWN
        // armCapture/captureFrame GPU-swapchain readback — the SAME mechanism
        // every --screenshot-* proof in this codebase uses — NOT an OS-level
        // desktop screenshot, so it cannot pick up anything else on the
        // desktop and needs no window-focus/input automation at all. A LOCAL
        // WorldMapSystem (the interactive section's `wmap` doesn't exist on
        // this early-return path) is built from the SAME `mapRoutes` staged
        // at boot, and drives the exact drawScreen()/drawWaypointChevron()
        // the interactive session calls — a proof of the real path, not a
        // parallel render.
        {
            const std::string mapDir = "shots_wmap";
            std::error_code mapEc; fs::create_directories(mapDir, mapEc);

            x3::game::WorldMapSystem mapShotWm;
            mapShotWm.init("", "");
            mapShotWm.setRouteOverlays(mapRoutes);   // copy: mapRoutes isn't read again below

            // Portal + garage markers — same lookup the interactive wiring uses.
            {
                std::vector<x3::game::MapMarker> mk;
                if (route.boreValid) {
                    float pIn[3], pOut[3];
                    route.posAt(route.boreS0, pIn); route.posAt(route.boreS1, pOut);
                    mk.push_back({ "TUNNEL ENTRANCE", "portal", pIn[0], pIn[2] });
                    mk.push_back({ "TUNNEL EXIT",      "portal", pOut[0], pOut[2] });
                }
                {
                    x3::game::FitoutConfig fcfg;
                    x3::game::TunnelFitout fitout;
                    fitout.build(route.boreS0, route.boreS1, fcfg, x3::game::kTunnelFitoutSeed);
                    x3::game::TunnelRoomProgram rooms;
                    rooms.build(route, fitout, x3::game::TunnelTier::A);
                    for (const x3::game::TunnelSpace& sp : rooms.spaces()) {
                        if (sp.kind != x3::game::SpaceKind::Garage) continue;
                        const float sMid = (sp.s0 + sp.s1) * 0.5f;
                        const float latMid = (float)sp.side * (sp.latIn + sp.latOut) * 0.5f;
                        float wx = 0.0f, wz = 0.0f;
                        route.worldAt(sMid, latMid, wx, wz);
                        mk.push_back({ "LNSS GARAGE", "garage", wx, wz });
                        break;
                    }
                }
                mapShotWm.setMapMarkers(std::move(mk));
            }

            x3::game::StoryFlags mapShotFlags;
            x3::ui::UiContext mapShotUi;
            const int fbw2 = (int)W, fbh2 = (int)H;
            const float anchorX = startPos[0], anchorY = startPos[1], anchorZ = startPos[2];

            auto mapShot2 = [&](const char* png, float mcx, float mcz, float mscale,
                                bool setWp, float wpx, float wpz) -> bool {
                mapShotWm.open(anchorX, anchorY, anchorZ, (float)fbw2, (float)fbh2);
                mapShotWm.camera().jumpTo(mcx, mcz, mscale);
                if (setWp) mapShotWm.setWaypoint(wpx, wpz, 0); else mapShotWm.clearWaypoint();
                const std::string path = mapDir + "/" + png;
                for (int i = 0; i < 3; ++i) {   // a couple frames so tile uploads land
                    glfwPollEvents();
                    device->setCamera(anchorX, anchorY + 60.0f, anchorZ, 0.0f, -0.5f, 60.0f);
                    if (i == 2) device->armCapture(path.c_str());
                    auto f = device->beginFrame();
                    if (f.valid) {
                        scene.render(*device, f);
                        // Opaque underlay: the map's own backdrop is 0.97 alpha
                        // (invisible over an interior, but lets ~3% of THIS
                        // world's HDR sky through — the same wash the
                        // interactive wiring's underlay slab fixes).
                        const float mapBg[4] = { 0.014f, 0.025f, 0.045f, 1.0f };
                        device->drawHudQuad(f, 0.0f, 0.0f, (float)fbw2, (float)fbh2, mapBg);
                        x3::ui::UiInput ui0{};
                        ui0.mouseX = fbw2 * 0.5f; ui0.mouseY = fbh2 * 0.5f;
                        mapShotUi.begin(*device, f, ui0);
                        x3::game::WorldMapSystem::ScreenInput msi{};
                        msi.mouseX = ui0.mouseX; msi.mouseY = ui0.mouseY;
                        msi.playerX = anchorX; msi.playerY = anchorY; msi.playerZ = anchorZ;
                        msi.playerYaw = std::atan2(route.dirZ, route.dirX);
                        msi.locationName = "TUNNEL RIDGE - ROAD NETWORK";
                        mapShotWm.drawScreen(mapShotUi, *device, f, msi, mapShotFlags, 0.0f);
                        mapShotUi.end();
                    }
                    device->endFrame(f);
                }
                const bool wrote = device->captureFrame(path.c_str());
                if (wrote) x3::logInfo("[tunnel] map/HUD proof: wrote " + path);
                else       x3::logError("[tunnel] map/HUD proof: capture FAILED " + path);
                return wrote;
            };

            bool mapOk = true;
            // 01: world overview — zoomed all the way out; both tours + the
            // dashed bores should read against the terrain underlay.
            mapOk = mapShot2("01_overview.png", anchorX, anchorZ, 0.06f, false, 0, 0) && mapOk;
            // 02: drive zoom — the same scale the M key opens at in play.
            mapOk = mapShot2("02_drive.png", anchorX, anchorZ, 0.32f, false, 0, 0) && mapOk;
            // 03: waypoint set, at drive zoom — a magenta blip a couple hundred
            // metres off the anchor, same scale as 02.
            mapOk = mapShot2("03_waypoint.png", anchorX, anchorZ, 0.32f,
                             true, anchorX + 30.0f, anchorZ + 220.0f) && mapOk;

            // 04: the driving HUD chevron, rendered through the exact
            // drawWaypointChevron() the interactive loop calls. Map CLOSED.
            // Two variants, both proof of the SAME code, different branches:
            //   04_chevron.png        — waypoint ahead-right, off-screen: the
            //                           common case (worldToScreen succeeds,
            //                           clamps into the safe rect).
            //   04b_chevron_behind.png — waypoint behind the shot camera: the
            //                           harder case (worldToScreen gives up;
            //                           the compass-bearing fallback).
            auto chevronShot = [&](const char* png, float wpX, float wpZ, float camYawShot) -> bool {
                mapShotWm.close();
                const std::string path = mapDir + "/" + png;
                for (int i = 0; i < 3; ++i) {
                    glfwPollEvents();
                    device->setCamera(anchorX, anchorY + 1.6f, anchorZ, camYawShot, -0.05f, 68.0f);
                    if (i == 2) device->armCapture(path.c_str());
                    auto f = device->beginFrame();
                    if (f.valid) {
                        scene.render(*device, f);
                        if (carBuilt) car.render(f);
                        drawWaypointChevron(f, wpX, anchorY, wpZ, anchorX, anchorY, anchorZ, camYawShot);
                    }
                    device->endFrame(f);
                }
                const bool wrote = device->captureFrame(path.c_str());
                if (wrote) x3::logInfo("[tunnel] map/HUD proof: wrote " + path);
                else       x3::logError("[tunnel] map/HUD proof: capture FAILED " + path);
                return wrote;
            };
            {
                const float baseYaw = std::atan2(route.dirZ, route.dirX);
                // Ahead-right: rotate the waypoint bearing ~50 deg off the shot
                // camera's forward so it is off-screen to the right, in front.
                const float aheadX = anchorX + 900.0f * std::cos(baseYaw + 0.9f);
                const float aheadZ = anchorZ + 900.0f * std::sin(baseYaw + 0.9f);
                mapOk = chevronShot("04_chevron.png", aheadX, aheadZ, baseYaw) && mapOk;
                const float behindX = anchorX - 1400.0f, behindZ = anchorZ + 900.0f;
                mapOk = chevronShot("04b_chevron_behind.png", behindX, behindZ, baseYaw + 2.2f) && mapOk;
            }
            // 05 (diagnostic, not one of the 4 required views): centered on
            // the spawn corridor's bore midpoint at a zoom that reads the
            // dashed casing clearly — GTA marks underpasses as a broken line
            // straight through the terrain; this confirms the new bold/dark
            // casing pair still dashes correctly over a bored reach.
            if (route.boreValid) {
                float bp[3]; route.posAt((route.boreS0 + route.boreS1) * 0.5f, bp);
                mapOk = mapShot2("05_dashed_bore.png", bp[0], bp[2], 0.55f, false, 0, 0) && mapOk;
            }
            ok = ok && mapOk;
        }

        if (carBuilt) car.shutdown();
        gasStations.shutdown(*device);
        factory.shutdown(*device);
        tickets.shutdown(*device);
        trees.shutdown(*device);
        if (townOn) town.shutdown(*device);
        forests.shutdown(*device);
        phys->setContactCallback(nullptr, nullptr);   // trafficCtx dies with this scope
        traffic.shutdown(phys.get());
        riverLife.shutdown(audioOn ? audio.get() : nullptr);
        tunnel.shutdown(*device, *phys);
        for (auto& w : tourBores) w->shutdown(*device, *phys);
        // Shared across every bore, so it is released ONCE here rather than by
        // each tunnel's own shutdown (which would free textures its neighbours
        // are still drawing with).
        x3::game::shutdownTunnelSurfaces(*device);
        streamer.shutdown(scene, *device, *phys);
        if (audioOn) {
        engineNote.shutdown();
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
                "M map, ~ console, ESC game menu, F4 weather panel, F5 lighting panel, "
                "SHIFT+ESC quits");

    // ---- DEV SHELL: console, pause menu, FPS -------------------------------
    // The reason the whole vehicle-feel pass was slow: every torque figure, grip
    // scale and centre-of-mass nudge cost an edit-rebuild-relaunch-drive-back
    // cycle, and those are values you have to judge by feel, one at a time. They
    // are all live now.
    // Wheel -> map zoom. Installed BEFORE shell.attach so the shell's own scroll
    // callback (console scrollback) chains to it, same order host_streamed uses.
    glfwSetScrollCallback(window, scrollCallback);
    g_weaponScroll = 0.0;

    // ---- POWER MULTIPLIERS (host-owned; composed each frame) --------------
    // setTorqueBoost is ONE multiplier — vampire and nitrous both feed it, so
    // the host owns the product (NO_SLOP rule 4: paired values are one value).
    bool  vampireOn = false;
    float nosTank   = 1.0f;    // 0..1; ~4 s of continuous spray, slow recharge
    bool  nosActive = false;

    HostShell shell;
    shell.attach(hc);
    shell.setFreezesSim(true);          // this host really does stop the sim on ESC
    console = shell.console();
    // ---- CONSOLE MIRRORS THE BOOT STATE (W-MENU find, NO_SLOP rule 4). The
    // shell's live cvar->device push (applyLiveHostRenderCVars, frame 0) runs
    // pushLiveHostCVarsToDevice from the REGISTERED DEFAULTS — r_csm "0",
    // r_velocity "0" — which silently UNDID this host's own boot pushes
    // (applyOutdoorCsm ON over 400 m; setPostFX velocity=true, the TAA ghost
    // fix this world exists for). Seed the cvars to what the host actually
    // booted, so the first live push re-states the boot instead of reverting
    // it — and the SHADOWS toggle in the new settings screen reads true.
    // `--set` on the command line still wins (attach() replayed it above).
    {
        auto cliHas = [&](const char* n) {
            for (const auto& kv : hc.cliCVars) if (kv.first == n) return true;
            return false;
        };
        if (!cliHas("r_csm"))      console->set("r_csm", "1");
        if (!cliHas("r_csm_dist")) console->set("r_csm_dist", "400");  // == applyOutdoorCsm(hc,..,400,..)
        if (!cliHas("r_velocity")) console->set("r_velocity", "1");    // == the boot setPostFX push
    }

    // ---- WORLD MAP (M) ------------------------------------------------------
    // host_streamed's WorldMapSystem, reused whole: the open/close lifecycle,
    // the cursor-anchored zoom camera, and the click/ENTER waypoint. What this
    // world feeds it is different CONTENT: no POI table, no Spire floors — the
    // road-network overlays staged at boot are the map.
    x3::game::WorldMapSystem wmap;
    wmap.init("", "");                       // empty POI/floor set, logged, not fatal
    wmap.setRouteOverlays(std::move(mapRoutes));
    // ---- MAP MARKERS: the tunnel mouths + the LNSS garage ------------------
    // Not a POI-table entry (no discovery gating — a road world has no
    // StoryFlags fog to lift), just a point label the map always draws. The
    // garage's world position is a pure-data lookup: FitoutConfig/
    // TunnelFitout/TunnelRoomProgram are the same cheap, deterministic build
    // this host already runs twice (the STEP 1.5 terrain-hole pass and the
    // STEP 3b fleet spawn) — a third read-only build here is the SAME pattern,
    // not new machinery, and keeps this block additive-only (no touching the
    // fleet/room code that already exists further down).
    {
        std::vector<x3::game::MapMarker> markers;
        if (route.boreValid) {
            float pIn[3], pOut[3];
            route.posAt(route.boreS0, pIn);
            route.posAt(route.boreS1, pOut);
            markers.push_back({ "TUNNEL ENTRANCE", "portal", pIn[0], pIn[2] });
            markers.push_back({ "TUNNEL EXIT",      "portal", pOut[0], pOut[2] });
        }
        {
            x3::game::FitoutConfig fcfg;
            x3::game::TunnelFitout fitout;
            fitout.build(route.boreS0, route.boreS1, fcfg, x3::game::kTunnelFitoutSeed);
            x3::game::TunnelRoomProgram rooms;
            rooms.build(route, fitout, x3::game::TunnelTier::A);
            for (const x3::game::TunnelSpace& sp : rooms.spaces()) {
                if (sp.kind != x3::game::SpaceKind::Garage) continue;
                const float sMid = (sp.s0 + sp.s1) * 0.5f;
                const float latMid = (float)sp.side * (sp.latIn + sp.latOut) * 0.5f;
                float wx = 0.0f, wz = 0.0f;
                route.worldAt(sMid, latMid, wx, wz);
                markers.push_back({ "LNSS GARAGE", "garage", wx, wz });
                break;
            }
        }
        // THE WORKS + THE FIVE CARDS on the map. MapMarker is the mechanism
        // this host already has and already draws (the LNSS garage above), so
        // the landmark is findable TODAY rather than after another lane lands.
        //
        // TODO (Lane 7, W-MAP): when the agreed `MapPoi { name, x, z, icon }`
        // registration header exists, register these through it instead — the
        // works at (facPlan.cx, facPlan.cz) with icon "factory", and one
        // "ticket" pin per uncollected spot. Nothing else about them moves.
        if (facOn) {
            markers.push_back({ "THE GLIMVALE WORKS", "factory",
                                facPlan.cx, facPlan.cz });
            char fb2[200];
            std::snprintf(fb2, sizeof(fb2),
                "[tunnel] map POI (Lane 7 TODO — staged as a MapMarker for now): "
                "THE GLIMVALE WORKS at (%.0f, %.0f), gate (%.0f, %.0f)",
                facPlan.cx, facPlan.cz, facPlan.gateX, facPlan.gateZ);
            x3::logInfo(fb2);
        }
        for (uint32_t k = 0; k < tickets.spotCount(); ++k) {
            float tp[3]; tickets.spotPos(k, tp);
            markers.push_back({ std::string("TICKET: ") + tickets.spotName(k),
                                "ticket", tp[0], tp[2] });
        }
        char mkb[96];
        std::snprintf(mkb, sizeof(mkb), "[tunnel] map: %u marker(s) staged", (uint32_t)markers.size());
        x3::logInfo(mkb);
        wmap.setMapMarkers(std::move(markers));
    }
    x3::game::StoryFlags mapFlags;           // no POIs yet: nothing to discover/persist
    x3::ui::UiContext wmapUi;
    bool mapOpen = false;
    bool prevMapM = false, prevMapEnter = false, prevMapLmb = false;
    bool mapEsc = false;                     // ESC edge, delivered by the shell handler

    // ---- W-MENU: THE GAME MENU (ESC) + WEATHER (F4) / LIGHTING (F5) --------
    // The main game's menu chrome + the REAL SettingsMenu screen, reused whole
    // (app/world_hosts/host_menu.h), wired through the shell's ESC
    // first-refusal so the console (~), F3 and SHIFT+ESC all keep working.
    // The shell's own three-row fallback menu is superseded here: this handler
    // always consumes ESC, so shell.paused() never goes true in this host.
    // PAUSE CONTRACT: menu/settings FREEZE the sim (title says PAUSED, and it
    // is true); the F4/F5 panels leave the world LIVE — watching the rain
    // arrive or the GI light up is the whole point of on-screen sliders.
    WorldGameMenu gameMenu;
    gameMenu.init(shell.console(), device);
    // ESC FIRST-REFUSAL, layered: the map's confirm prompt, then the map, then
    // the game menu stack, then (nothing else open) OPEN the game menu.
    shell.setEscapeHandler([&]() -> bool {
        if (mapOpen && wmap.confirmOpen()) { mapEsc = true; return true; }
        if (mapOpen) {
            mapOpen = false; wmap.close();
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
            glfwGetCursorPos(window, &lastMX, &lastMY);
            return true;
        }
        if (gameMenu.anyOpen()) { gameMenu.onEscape(); return true; }
        gameMenu.toggleMenu();
        return true;                         // ESC is the game menu now
    });
    if (auto* con = shell.console()) {
        // ---- weather (ONE registration for the console commands AND the F4
        // panel — app/world_hosts/host_menu.h owns the wx cvar catalog + the
        // rain/snow 0-10 scale now, so the slider and the typed command can
        // never drift apart). ----
        registerWeatherConsole(*con);
        // Live trims for Jake's placement, so a wrong-facing or sunk rig is a
        // console line to diagnose instead of a rebuild: degrees added to his
        // travel yaw, metres added to his measured ground compensation.
        con->registerCommand("jake_debug", [&, con](const std::vector<std::string>&) {
            const x3::phys::Vec3 jf = onFoot.feet();
            const float gy = x3::game::terrainHeightAtWorld(jf.x, jf.z);
            const x3::phys::RayHit rh = phys->rayCast(
                x3::phys::Vec3{ jf.x, jf.y + 40.0f, jf.z },
                x3::phys::Vec3{ 0.0f, -1.0f, 0.0f }, 120.0f, x3::phys::Layer::Static);
            char b[256];
            std::snprintf(b, sizeof(b),
                "feet(%.1f, %.2f, %.1f) | terrain %.2f | topmost-static %s%.2f | "
                "driving=%d spawned=%d | yaw=%.1fdeg jake_y=%.2f cam=%d",
                jf.x, jf.y, jf.z, gy, rh.hit ? "" : "(miss) ",
                rh.hit ? rh.point.y : 0.0f, (int)driving, (int)footSpawned,
                (double)(jake.yaw() * 57.29578f),
                (double)console->getFloat("jake_y"), console->getInt("jake_cam"));
            con->print(b);
        }, "print Jake's feet vs terrain vs topmost surface — the burial confession");
        con->registerCommand("wx_debug", [&, con](const std::vector<std::string>&) {
            const x3::game::WeatherSample& ws = weather.sample();
            char b[256];
            std::snprintf(b, sizeof(b),
                "wx='%s' on=%d | sample: state=%d precip=%.2f snow=%d tempC=%.1f | "
                "fed: kind=%d amt=%.2f | live particles=%u",
                console->getString("wx").c_str(), (int)weatherOn, (int)ws.state,
                (double)ws.precipitation, (int)ws.snowfall, (double)ws.tempC,
                (int)precipKind, (double)precipAmt, precip.liveCount());
            con->print(b);
        }, "print the whole rain chain: state -> sample -> fed amount -> live particles");
        // (`rain`/`snow` + wx_precip_mult/wx_cloud/wx_wind/wx_winddir live in
        // registerWeatherConsole above — host_menu.h is the one mapping.)
        con->registerCVar("jake_yaw", "0", "Jake facing trim, degrees (asset owns the truth; default 0)");
        con->registerCVar("jake_y",   "0", "Jake height trim, metres (asset feet sit at origin; default 0)");
        con->registerCVar("jake_cam", "2", "on-foot camera: 0 first person, 1 third near, 2 third far (F1 cycles)");
        // ENGINE NOTE A/B: 1 = the multi-RPM bank (default), 0 = the legacy
        // single re-pitched loop. Flip it live; the owner's ear is the gate.
        con->registerCVar("snd_bank", "1",
            "engine note: 1 = multi-RPM bank (new), 0 = legacy single loop");
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
        // GRIP knobs are MULTIPLIERS over the authored stock compound (1 =
        // stock; vehicle.cpp buildPhysics owns the stock numbers: longitudinal
        // 10 all wheels, lateral 1.70 front / 1.60 rear). Semantics + help
        // fixed 2026-08-16: the old help said "1 = Jolt's economy tyre" while
        // the code composed on top of the compound — `car_grip 5.2` silently
        // meant 52x the economy tyre.
        shell.addFloatCommand("car_grip", "tyre grip multiplier, all wheels (1 = stock compound)",
            [&](float v) { x3::phys::WheeledTuning t; t.gripScale = v; car.applyTuning(t); });
        // NFS TURN-IN BALANCE (see buildPhysics in vehicle.cpp): front > rear =
        // nose bites on turn-in, rear breaks away first (progressive slide);
        // rear > front = stability/understeer. Dial the split live.
        shell.addFloatCommand("car_gripf", "FRONT axle grip multiplier (1 = stock; raise for sharper turn-in)",
            [&](float v) { x3::phys::WheeledTuning t; t.gripScaleFront = v; car.applyTuning(t); });
        shell.addFloatCommand("car_gripr", "REAR axle grip multiplier (1 = stock; lower for looser tail)",
            [&](float v) { x3::phys::WheeledTuning t; t.gripScaleRear = v; car.applyTuning(t); });
        // CORNERING MASTER DIAL: lateral-only grip. CEILING: the ~2.7 g
        // rollover threshold (see the CoM comment in vehicle.cpp buildPhysics)
        // — stock lateral peaks ~1.9 g; ~1.4 here puts cornering AT the tip
        // threshold and beyond it the inside wheels lift.
        shell.addFloatCommand("car_latgrip", "cornering grip multiplier, lateral only (1 = stock ~1.9 g; >1.4 risks tip-up)",
            [&](float v) { x3::phys::WheeledTuning t; t.latGripScale = v; car.applyTuning(t); });
        // Lateral breakaway shape: multiplies the high-slip end of the lateral
        // friction curve. Stock Jolt shape keeps 83% of peak grip in a slide.
        shell.addFloatCommand("car_lattail", "slide grip vs peak: 1 = stock shape, >1 more catchable, <1 more drifty",
            [&](float v) { x3::phys::WheeledTuning t; t.latTail = v; car.applyTuning(t); });
        // ANTI-ROLL BARS (N/m): the corners-FLAT hardware. Stock 8000 front /
        // 6000 rear (vehicle.cpp buildPhysics — paired numbers). Stiffer
        // front = more push/stability, stiffer rear = livelier rotation.
        // HARD CEILING ~12000: above it the 60 Hz solver pumps the roll mode
        // and the car flips (measured; see WheeledVehicleDesc::antiRollFront).
        shell.addFloatCommand("car_arb_f", "front anti-roll bar N/m (stock 8000; KEEP UNDER 12000 or the solver flips the car)",
            [&](float v) { x3::phys::WheeledTuning t; t.antiRollFront = v; car.applyTuning(t); });
        shell.addFloatCommand("car_arb_r", "rear anti-roll bar N/m (stock 6000; KEEP UNDER 12000 or the solver flips the car)",
            [&](float v) { x3::phys::WheeledTuning t; t.antiRollRear = v; car.applyTuning(t); });
        shell.addFloatCommand("car_final", "final-drive ratio (stock 5.2, paired with the 0.50 6th; shorter = punch, taller = top end)",
            [&](float v) { x3::phys::WheeledTuning t; t.finalDrive = v; car.applyTuning(t); });
        // AERO DOWNFORCE — the spoiler (owner: "spoilers for downforce"). Scale
        // over the stock wing: 0.35x weight at 70 mph, capped 1.10x from ~124
        // mph, applied rear-biased (see the DOWNFORCE block in JoltVehicle.cpp
        // preStep — PAIRED numbers, NO_SLOP rule 4).
        shell.addFloatCommand("car_downforce", "spoiler downforce scale (1 = stock: 0.35x weight at 70 mph, cap 1.1x; 0 = off)",
            [&](float v) { x3::phys::WheeledTuning t; t.downforce = std::max(0.0f, v); car.applyTuning(t); });
        // ROLL-RATE DAMPING (flip resistance): bleeds roll RATE while >= 3
        // wheels are grounded. THE safe tool at 60 Hz — do NOT chase flips
        // with stiffer ARBs instead (>= 15000 N/m the solver pumps the roll
        // mode and flips the car; see car_arb_f's ceiling).
        shell.addFloatCommand("car_rolldamp", "roll-rate damping N*m*s/rad (stock 2000; 0 = off)",
            [&](float v) { x3::phys::WheeledTuning t; t.rollDamp = std::max(0.0f, v); car.applyTuning(t); });
        // ---- speed-sensitive steering (see DriveDemo::SteerParams) ----
        shell.addFloatCommand("car_steer_lo", "mph at/below which you get 100% steering lock (stock 25)",
            [&](float v) { car.steerParams().fullLockMph = v; });
        shell.addFloatCommand("car_steer_hi", "mph at/above which lock is fully tightened (stock 95)",
            [&](float v) { car.steerParams().hiSpeedMph = v; });
        shell.addFloatCommand("car_steer_min", "fraction of full lock left at high speed, 0-1 (stock 0.34)",
            [&](float v) { car.steerParams().hiFrac = v; });
        shell.addFloatCommand("car_steer_rate", "steering slew, full-locks per second (stock 7; big = twitchier)",
            [&](float v) { car.steerParams().slewPerSec = v; });
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
        // TIRE SQUASH (render-only; owner: "when Landing hard on pavement, the
        // RUBBER TIRES should deflect visually, a tiny bit" — see
        // DriveDemo::updateTireSquash/squashFactors in vehicle.cpp). Deliberately
        // NOT a WheeledTuning field: this never touches Jolt/the DS-Vehicle
        // session's suspension, it only scales a cosmetic per-wheel render
        // nudge. 0 = off, 1 = full (default); clamped in setTireSquash.
        shell.addFloatCommand("tire_squash", "hard-landing tire squash intensity, 0-1 (visual only, default 1)",
            [&](float v) { car.setTireSquash(v); });
        shell.addFloatCommand("car_torquemult", "flat torque multiplier on top of the turbo (nitrous)",
            [&](float v) { car.setTorqueBoost(v); });
        // ---- turbo ----
        shell.addToggleCommand("turbo", "turbo on/off (off = the curve with no lag, naturally aspirated)",
            [&]{ return car.turboEnabled(); },
            [&](bool on) { car.setTurboEnabled(on); });
        // Help-text numbers below are PAIRED with TurboParams' defaults in
        // vehicle.h (NO_SLOP rule 4) — the old block said "stock 16 psi" under
        // a 35 psi model and "stock 0.45/1800" after the spool retune.
        shell.addFloatCommand("turbo_max", "peak boost, psi (stock 35)",
            [&](float v) { car.turbo().maxPsi = v; });
        shell.addFloatCommand("turbo_spool", "seconds for the compressor to come up (stock 0.30)",
            [&](float v) { car.turbo().spoolTau = v; });
        shell.addFloatCommand("turbo_dump", "seconds to bleed off on a lift (stock 0.11)",
            [&](float v) { car.turbo().dumpTau = v; });
        shell.addFloatCommand("turbo_start", "rpm where the compressor starts to make pressure (stock 1500)",
            [&](float v) { car.turbo().spoolStartRpm = v; });
        shell.addFloatCommand("turbo_full", "rpm for full boost (stock 4200)",
            [&](float v) { car.turbo().spoolFullRpm = v; });
        // turbo_floor REMOVED 2026-08-16: the pressure-ratio model derives the
        // off-boost floor from absolute manifold pressure; the cvar was wired
        // to a field nothing read (NO_SLOP rule 6 — a dead knob is a lie).
        shell.addFloatCommand("turbo_vacuum", "vacuum depth at a closed throttle, psi (stock 8.5)",
            [&](float v) { car.turbo().vacuumPsi = v; });
        shell.addToggleCommand("car_tc", "traction control (also bound to T)",
            [&]{ return car.tractionControl(); },
            [&](bool on) { car.setTractionControl(on); });
        shell.addToggleCommand("climb", "crawl traction for steep terrain (also bound to C)",
            [&]{ return car.climbMode(); },
            [&](bool on) { car.setClimbMode(on); });
        // J&S VAMPIRE (shop-part preview). Per-cylinder knock control lets the
        // engine safely carry more ignition timing; timing is torque everywhere
        // on the curve, so it lands as a flat multiplier that STACKS with the
        // pressure-ratio turbo model. +7% is a real-world street-tune figure.
        // Owned by perfshop.cpp once the parts catalog carries it; the console
        // command is how Tim test-drives the part before the shop sells it.
        shell.addToggleCommand("vampire", "J&S Vampire knock control: +7% torque from timing",
            [&]{ return vampireOn; },
            [&](bool on) { vampireOn = on; });
        // ---- THE GOLDEN TICKETS ------------------------------------------
        // `tickets` reads, `tickets <n>` sets. THE ARG CONVENTION
        // (engine/core/IConsole.h, documented in blood and gate-locked by
        // --test-console): args[0] is the FIRST ARGUMENT, not the command
        // name. Every host command written against args[1] was silently dead
        // for two days.
        con->registerCommand("tickets", [&](const std::vector<std::string>& args) {
            if (!args.empty()) {
                tickets.setCollected(scene, std::atoi(args[0].c_str()));
                if (tickets.allFound()) factory.openGate();
            }
            char b[256];
            std::snprintf(b, sizeof(b), "TICKETS %d/%d  —  the works gate is %s",
                          tickets.collected(), (int)tickets.spotCount(),
                          factory.gateOpen() ? "OPEN" : "SHUT");
            con->print(b);
            for (uint32_t k = 0; k < tickets.spotCount(); ++k) {
                float tp[3]; tickets.spotPos(k, tp);
                char lb[192];
                std::snprintf(lb, sizeof(lb), "  %-18s (%.0f, %.0f)  %s",
                              tickets.spotName(k), tp[0], tp[2],
                              tickets.spotTaken(k) ? "FOUND" : "still hidden");
                con->print(lb);
            }
        }, "golden tickets: 'tickets' reads the count, 'tickets <n>' sets it "
           "(all five opens the works gate)");
        con->registerCommand("factory", [&](const std::vector<std::string>&) {
            if (!facOn || !factory.built()) { con->print("the works was not built this boot"); return; }
            float gp[3]; factory.gatePoint(gp);
            char b[300];
            std::snprintf(b, sizeof(b),
                "THE GLIMVALE WORKS — site (%.0f, %.0f) platform %.1f m, gate "
                "(%.0f, %.0f) %s (slide %.0f%%), %u meshes / %u tris / %u pack "
                "instances, drive %.0f m off the freeway",
                factory.plan().cx, factory.plan().cz, factory.plan().padY,
                gp[0], gp[2], factory.gateOpen() ? "OPEN" : "SHUT",
                factory.gateSlide() * 100.0f, factory.meshCount(),
                factory.triCount(), factory.propCount(), facDrive.road.lengthM);
            con->print(b);
        }, "report the Glimvale Works: site, gate state, build cost");
        con->registerCommand("car_reset", [&](const std::vector<std::string>&) {
            car.setTorqueBoost(1.0f);
            // These ARE the buildPhysics numbers in vehicle.cpp (NO_SLOP rule
            // 4: paired — change both). The old reset was a time capsule from
            // two retunes ago: 2400 Nm (the shipped car is 800 + turbo) and
            // gripScale 5.2 under the broken compose-on-top semantics.
            x3::phys::WheeledTuning t;
            t.maxEngineTorque = 800.0f; t.maxEngineRPM = 7500.0f;
            t.gripScale = 1.0f; t.latGripScale = 1.0f; t.latTail = 1.0f;
            t.massKg = 1083.2f; t.finalDrive = 5.2f;
            t.antiRollFront = 8000.0f; t.antiRollRear = 6000.0f;
            t.downforce = 1.0f; t.rollDamp = 2000.0f;
            car.applyTuning(t);
            car.steerParams() = x3::game::DriveDemo::SteerParams{};
            car.turbo() = x3::game::DriveDemo::TurboParams{};
            con->print("car back to the shipped 992 Turbo S numbers (800 Nm, stock grip, 5.2 final, stock spoiler)");
        }, "restore the stock vehicle tune");
        // tp — put the car somewhere. THE REVIEW TOOL: this world is 46 miles of
        // road and the interesting places (the summit lot at 7.5 km, the far
        // bore) are twenty minutes of driving from spawn, so nobody — human or
        // agent — ever looked at them in an INTERACTIVE run. The --screenshot-*
        // harness is not a substitute: it runs headless, and headless takes
        // different code paths (this host streams at radius 14 headless vs 9
        // interactive — the horizon-ring void annulus lived in exactly that
        // gap and no capture could see it).
        //
        //   tp                -> print where you are
        //   tp <x> <z>        -> world metres, dropped onto the surface
        //   tp lot | bore | spur | ring   -> the named destinations
        con->registerCommand("tp", [&, con](const std::vector<std::string>& args) {
            float tx = 0.0f, tz = 0.0f;
            const x3::phys::BodyId cb = car.controller() ? car.controller()->body()
                                                         : x3::phys::BodyId{};
            if (args.size() == 1) {
                x3::phys::Vec3 p{ 0.0f, 0.0f, 0.0f };
                if (car.controller()) p = phys->getBodyPosition(cb);
                char b[160];
                std::snprintf(b, sizeof(b), "at (%.0f, %.0f, %.0f) — ground %.0f m",
                              (double)p.x, (double)p.y, (double)p.z,
                              (double)x3::game::terrainHeightAtWorld(p.x, p.z));
                con->print(b);
                return;
            }
            if (args.size() == 2) {
                const std::string& w = args[1];
                if (w == "lot" && summitLot.built)        { tx = summitLot.cx;  tz = summitLot.cz; }
                else if (w == "spur" && summitSpur.built) { tx = summitSpur.spec.x.back();
                                                            tz = summitSpur.spec.z.back(); }
                else if (w == "bore") { float p[3]; route.posAt(std::max(8.0f, route.boreS0 - 40.0f), p);
                                        tx = p[0]; tz = p[2]; }
                else if (w == "ring" && ringSpec.x.size() > 2) { tx = ringSpec.x[0]; tz = ringSpec.z[0]; }
                else { con->print("tp <x> <z> | tp lot|spur|bore|ring"); return; }
            } else if (args.size() >= 3) {
                tx = (float)std::atof(args[1].c_str());
                tz = (float)std::atof(args[2].c_str());
            }
            if (!car.controller()) { con->print("no car to move"); return; }
            // Stream the destination in BEFORE dropping the car on it, or it
            // lands on tiles that do not exist yet and falls through the world.
            streamer.update(scene, *device, *phys, tx, tz);
            const float gy = x3::game::terrainHeightAtWorld(tx, tz);
            const x3::phys::Vec3 dst{ tx, gy + 2.0f, tz };
            phys->setBodyPosition(cb, dst);
            const float zero[3] = { 0.0f, 0.0f, 0.0f };
            phys->setBodyLinearVelocity(cb, zero);
            char b[160];
            std::snprintf(b, sizeof(b), "teleported to (%.0f, %.0f, %.0f)",
                          (double)tx, (double)(gy + 2.0f), (double)tz);
            con->print(b);
            // ALSO to the log, not just the console pane: a screenshot of the
            // console does not survive being cropped, and a capture without
            // coordinates cannot prove WHERE it was taken. Receipts.
            x3::logInfo(std::string("tp: ") + b);
        }, "tp [x z | lot|spur|bore|ring] — move the car (review tool)");
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
        // W-STATIONS: the LIVE fuel commands (`fuel`, `fuel_stations`). The
        // pure-data fuel_* cvars are already here — registerEngineConsoleCVars
        // registers them for every host (app/engine_console.cpp).
        gasStations.registerConsole(*con);
    }

    // X3_PERF_LOG=1: rolling avg-FPS to the log every 5 s (host_echotropolis
    // precedent). Exists so a BOUNDED unattended run can measure spawn-view
    // perf before/after a terrain/shader change — the 165->26 fps corridor-pin
    // regression was only caught by an eyeball on the F3 overlay; this gives
    // the same number to a log a lane agent can diff.
    const bool perfLog = [] { const char* e = std::getenv("X3_PERF_LOG"); return e && e[0] == '1'; }();
    double perfT0 = glfwGetTime(); uint32_t perfFrames = 0;

    // ---- W-MENU: THE IN-WORLD CLOCK, hoisted out of the weather branch -----
    // The F4 TIME slider has to work over the demo sky too (weather OFF is the
    // boot state), so the clock ticks in both modes now. wx_hour re-seeds it —
    // and the first post-boot change LATCHES the sun onto it (applyHourSun):
    // until you touch time, the boot sky is byte-identical to every reference
    // capture; the moment you ask for 19:00 the sun actually goes there.
    float todHours     = console ? console->getFloat("wx_hour") : 14.0f;
    float lastHourCvar = todHours;
    bool  todSunActive = false;

    while (!glfwWindowShouldClose(window) && !shell.wantQuit()) {
        // RE-SUBMIT THE BORE LIGHTS EVERY FRAME. They were set exactly ONCE at boot
        // (setPointLights above), which is why the tunnel is lit in headless captures
        // — those render a few frames with nothing else touching the light set — and
        // PITCH BLACK the moment you drive it, both from inside and looking in through
        // the portal from outside. The interactive loop streams tiles and draws other
        // content, and the light array does not survive that. Cheap: 6 cached lights.
        mapEsc = false;   // BEFORE the poll: the escape handler runs inside it
        glfwPollEvents();
        shell.beginFrame();
        if (perfLog) {
            ++perfFrames;
            const double pnow = glfwGetTime();
            if (pnow - perfT0 >= 5.0) {
                char pb[96];
                std::snprintf(pb, sizeof(pb), "[perf] avg FPS %.1f over %.1fs",
                              (double)perfFrames / (pnow - perfT0), pnow - perfT0);
                x3::logInfo(pb);
                perfT0 = pnow; perfFrames = 0;
            }
        }

        // ESC OPENS THE MENU, IT DOES NOT QUIT — the shell owns that now, along
        // with the console and the FPS overlay. This host used to hand-roll the
        // pause by polling glfwGetKey and tracking its own `escWasDown` edge,
        // which drops a press any time a frame runs longer than the keypress.
        // The shell edge-detects in the GLFW key CALLBACK instead, so a press
        // cannot be missed no matter how long the frame took.
        const double now = glfwGetTime();
        float fdt = (float)(now - prevTime); prevTime = now;
        if (fdt > 0.1f) fdt = 0.1f;

        // ---- W-MENU: F4 weather / F5 lighting panel hotkeys. shell.key so
        // typing F4-ish text in the console never toggles them; gated off
        // while the fullscreen map owns the screen.
        {
            static bool f4Was = false, f5Was = false;
            const bool f4Now = !mapOpen && shell.key(GLFW_KEY_F4);
            const bool f5Now = !mapOpen && shell.key(GLFW_KEY_F5);
            if (f4Now && !f4Was) gameMenu.togglePanel(WorldGameMenu::Screen::Weather);
            if (f5Now && !f5Was) gameMenu.togglePanel(WorldGameMenu::Screen::Lighting);
            f4Was = f4Now; f5Was = f5Now;
        }

        // ---- W-MENU: the menu/panel input snapshot (mouse + arrows/enter,
        // with key repeat so holding an arrow walks a slider). Assembled every
        // frame — the frozen branch below and the live panel draw both use it.
        x3::ui::UiInput gmIn{};
        {
            double gmx = 0.0, gmy = 0.0; glfwGetCursorPos(window, &gmx, &gmy);
            gmIn.mouseX = (float)gmx; gmIn.mouseY = (float)gmy;
            const bool uiUp = gameMenu.anyOpen();
            const bool lmbNow = uiUp &&
                glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
            static bool gmLmbWas = false;
            gmIn.mouseDown = lmbNow; gmIn.mousePressed = lmbNow && !gmLmbWas;
            gmLmbWas = lmbNow;
            auto navRepeat = [&](bool down, double& nextAt) -> bool {
                if (!down) { nextAt = 0.0; return false; }
                if (nextAt == 0.0) { nextAt = now + 0.34; return true; }   // rising edge
                if (now >= nextAt) { nextAt = now + 0.07; return true; }   // auto-repeat
                return false;
            };
            auto keyIsDown = [&](int k) {
                return uiUp && !shell.consoleOpen() && glfwGetKey(window, k) == GLFW_PRESS;
            };
            static double rU = 0.0, rD = 0.0, rL = 0.0, rR = 0.0, rE = 0.0;
            gmIn.navUp       = navRepeat(keyIsDown(GLFW_KEY_UP), rU);
            gmIn.navDown     = navRepeat(keyIsDown(GLFW_KEY_DOWN), rD);
            gmIn.navLeft     = navRepeat(keyIsDown(GLFW_KEY_LEFT), rL);
            gmIn.navRight    = navRepeat(keyIsDown(GLFW_KEY_RIGHT), rR);
            // ENTER only — while an F4/F5 panel is open you are still DRIVING,
            // and SPACE is the handbrake.
            gmIn.navActivate = navRepeat(keyIsDown(GLFW_KEY_ENTER) ||
                                         keyIsDown(GLFW_KEY_KP_ENTER), rE);
        }
        // ONE CURSOR RULE for every overlay: menu/panels/map/console show the
        // pointer, gameplay hides it. Reconciled per frame (the shell frees it
        // for the console, the map block frees it for the map — this keeps
        // every combination honest) and the look deltas are re-anchored on any
        // switch so the camera never inherits the pointer's travel.
        {
            const int want = (gameMenu.anyOpen() || mapOpen || shell.consoleOpen())
                                 ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_DISABLED;
            if (glfwGetInputMode(window, GLFW_CURSOR) != want) {
                glfwSetInputMode(window, GLFW_CURSOR, want);
                glfwGetCursorPos(window, &lastMX, &lastMY);
            }
        }

        // MERGE NOTE (integration/complete): everything below keeps the roads
        // lane's weather/Jake/on-foot systems, driven through the vehicle lane's
        // HostShell — one console, edge-detected keys, and a real pause.
        // W-MENU: the game menu + its settings screen freeze the sim exactly
        // like the shell pause did (blocksSim); the F4/F5 panels do NOT — they
        // are live tuning surfaces and take the other path.
        if (shell.paused() || gameMenu.blocksSim()) {
            // Present so the window stays live, but do not advance the sim.
            auto pf = device->beginFrame();
            if (pf.valid) {
                scene.render(*device, pf);
                gasStations.draw(*device, pf);          // the forecourt does not vanish on pause
                if (carBuilt) car.render(pf);
                float pcam[3] = { startPos[0], startPos[1], startPos[2] };
                if (carBuilt) car.chassisPos(pcam);
                traffic.render(pf, pcam);               // traffic holds its pose paused
                riverLife.render(*device, pf, scene);   // boats stay visible paused
                gameMenu.draw(pf, gmIn, fdt, todHours); // the game menu, over the frozen world
                shell.draw(pf, fdt);                    // console stays reachable over the menu
            }
            device->endFrame(pf);
            // Menu-row requests (quit / console). The map row closes the menu
            // and is consumed by the M block on the next, live, frame.
            if (gameMenu.takeQuitRequest()) glfwSetWindowShouldClose(window, GLFW_TRUE);
            if (gameMenu.takeConsoleRequest()) shell.hudForCallbacks().toggleConsole();
            continue;
        }

        // ==== WEATHER TICK ===================================================
        // Chained in dependency order. Note the CLOCK: an in-world day is
        // compressed to ten real minutes, because the diurnal temperature swing
        // is the most interesting thing the model does and nobody is going to
        // sit through twenty-four hours to watch the desert cool off.
        // ---- THE RIVER HAS WATER (Tim: "Can we pour the water in now").
        // One lambda with the headless path — same tone, same clock shape.
        riverWaterClock += fdt;
        {
            // Focus = whoever the camera is following this frame.
            float wfx = startPos[0], wfz = startPos[2];
            if (driving && carBuilt) {
                float cp[3]; car.chassisPos(cp); wfx = cp[0]; wfz = cp[2];
            } else if (footSpawned) {
                const x3::phys::Vec3 ft = onFoot.feet(); wfx = ft.x; wfz = ft.z;
            }
            applyRiverWater(riverWaterClock, wfx, wfz);
        }
        // THE WIND. The sky UBO's time lane (setSkyTime -> sky.params.z) is the
        // drift clock for the cloud deck AND its ground shade (both sample
        // kCloudDrift * t in inc/sky_clouds.glsl). This world never set it, so
        // the deck hung frozen. Same clock the river uses, one line.
        device->setSkyTime(riverWaterClock);
        {   // The clock RUNS, but wx_hour re-seeds it -- so you can jump to the
            // pre-dawn trough to see ice form instead of waiting out the cycle.
            // (Hoisted out of the weather branch: see the todHours declaration.)
            const float hourCvar = console->getFloat("wx_hour");
            if (hourCvar != lastHourCvar) {
                todHours = hourCvar; lastHourCvar = hourCvar;
                todSunActive = true;         // the sun follows the clock from now on
            }
            todHours += fdt * (24.0f / 600.0f);        // 10 real minutes per in-world day
            if (todHours >= 24.0f) todHours -= 24.0f;
        }
        if (weatherOn) {
            weather.tick(fdt);
            weather.setTimeOfDay(todHours);

            const x3::game::WeatherSample& ws = weather.sample();
            wetness.tick(fdt, ws.precipitation, ws.tempC, ws.snowfall);
            tickRiverRise(fdt, ws.precipitation, ws.snowfall);

            // Lightning only under an actual storm; hazardLevel already carries
            // "how bad", so intensity comes free and correct.
            const float stormI = (ws.state == x3::game::WeatherState::Storm)
                               ? ws.hazardLevel : 0.0f;
            float lp[3] = { 0.0f, 0.0f, 0.0f };
            if (carBuilt) car.chassisPos(lp);
            storm.tick(fdt, stormI, audioOn ? audio.get() : nullptr, lp[0], lp[1], lp[2]);

            // Push the sky + the fill its cover implies. THE MAPPING LIVES AT
            // THE TOP OF THIS FILE (skyFromWeather/applySky) and is shared with
            // the headless capture loop — the two used to diverge and the proof
            // shots lied about the storm. CLOUDS COST LIGHT (Tim: "Do we have
            // real clouds that obscure and dim the sun? The ground is way too
            // sunny"): the deck cuts the sky disk here, cloudShadowFactor cuts
            // the direct sun per-fragment (task #27), and applySky() drops the
            // skylight fill — all three off the one cover number.
            // W-MENU layers on top: an explicit wx_cloud (the F4 CLOUD slider)
            // WINS over the state's own deck — the slider is authority, even
            // through a storm — and a touched clock moves the sun.
            {
                x3::rhi::IRenderDevice::SkyParams wsp = skyFromWeather(ws, storm.flash());
                const float cov = console->getFloat("wx_cloud");
                if (cov >= 0.0f) wsp.cloud = std::min(1.0f, cov);
                if (todSunActive) applyHourSun(wsp, todHours);
                applySky(*device, wsp);
            }

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
            precipAmt  = std::min(1.0f, ws.precipitation * console->getFloat("wx_precip_mult"));

        }
        double mx, my; glfwGetCursorPos(window, &mx, &my);
        // Gate the LOOK, not just the camera apply: the deltas also feed the
        // on-foot Player below, and the cursor is released while typing — an
        // ungated delta would spin Jake's view across the screen on the way to
        // the scrollback. Same rule while the MAP or an F4/F5 panel owns the
        // cursor (dragging a slider must not orbit the camera).
        const float look = (shell.inputEnabled() && !mapOpen && !gameMenu.panelOpen())
                               ? 1.0f : 0.0f;
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
                    else {
                        if (wxWant != "clear" && wxWant != "on")
                            console->print("wx: unknown '" + wxWant + "' — off|clear|cloudy|rain|storm|fog|snow (or use: rain 0-10)");
                        else if (wxWant == "on")
                            console->print("wx on = clear skies. You want RAIN: try 'rain 7' or 'wx storm'.");
                        weather.forceState(WS::Clear,  true);
                    }
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
                    // The demo-sky PUSH lives in the live refresh block just
                    // below (it owns cloud/time over the no-weather sky); this
                    // edge only clears the ground state the storm left behind.
                    device->setSnowCover(0.0f);
                    device->setWetness(x3::rhi::IRenderDevice::WetnessParams{});
                    console->print("weather: off (the demo sky — CLOUD/TIME sliders still live)");
                }
            }
        }
        // ---- W-MENU: LIVE DEMO SKY (weather off — which is the boot state).
        // The F4 CLOUD and TIME sliders must work without turning the weather
        // system on, so the no-weather sky is recomputed from the cvars and
        // re-pushed WHEN ITS INPUTS CHANGE: cover at the slider's own 0.05
        // steps, the sun only once the clock is latched and only in 0.1 h
        // steps (~15 real seconds) — every push re-bakes the IBL fill, so the
        // steps are the ration. `wx off` also forces one push (the skylight
        // has to come back after a storm — the old edge push did this).
        {
            static float lastCov = -2.0f, lastQHour = -2.0f;
            static bool  skyWasWeather = false;
            if (!weatherOn) {
                const float cov   = console->getFloat("wx_cloud");
                const float qHour = todSunActive
                                        ? std::floor(todHours * 10.0f) / 10.0f : -1.0f;
                if (skyWasWeather || cov != lastCov || qHour != lastQHour) {
                    lastCov = cov; lastQHour = qHour; skyWasWeather = false;
                    x3::rhi::IRenderDevice::SkyParams sp = tunnelDemoSky();
                    if (cov >= 0.0f)   sp.cloud = std::min(1.0f, cov);
                    if (qHour >= 0.0f) applyHourSun(sp, qHour);
                    applySky(*device, sp);   // sky + the fill its cover implies
                }
            } else {
                skyWasWeather = true;        // force a re-push when wx goes off
            }
        }

        // shell.key(), not glfwGetKey(): false while the console or the menu
        // owns the keyboard, so typing `car_grip 6` no longer also steers
        // right, brakes and applies the handbrake. The MAP gates it too: while
        // it is open the same WASD pans the map (its own raw reads below), and
        // the CAR must not receive it — auto-hold then brings you to a stop.
        auto kd = [&](int k){ return !mapOpen && shell.key(k); };

        // ---- M: THE MAP. shell.key so typing `m` in the console does not
        // toggle it; edge-triggered like E/T/C above. Opens centered on the
        // car (or Jake, on foot) at a drive-scale zoom — wheel zooms out to the
        // whole 46-mile network from there.
        {
            const bool mNow = shell.key(GLFW_KEY_M);
            // The game menu's WORLD MAP row raises a one-frame request (the
            // menu closed itself when it did) — same open path as the M key.
            const bool gmMapReq = gameMenu.takeMapRequest();
            if ((mNow && !prevMapM) || gmMapReq) {
                if (mapOpen) { mapOpen = false; wmap.close(); }
                else {
                    float pp[3] = { startPos[0], startPos[1], startPos[2] };
                    if (carBuilt) car.chassisPos(pp);
                    if (!driving && footSpawned) {
                        const x3::phys::Vec3 ft = onFoot.feet();
                        pp[0] = ft.x; pp[1] = ft.y; pp[2] = ft.z;
                    }
                    int fbw = 0, fbh = 0; glfwGetFramebufferSize(window, &fbw, &fbh);
                    wmap.open(pp[0], pp[1], pp[2], (float)fbw, (float)fbh);
                    // open() lands at interior zoom (6 px/m); a road world reads
                    // at ~2.5 miles across, so re-anchor the camera there.
                    wmap.camera().jumpTo(pp[0], pp[2], 0.32f);
                    mapOpen = true;
                }
                glfwSetInputMode(window, GLFW_CURSOR,
                                 mapOpen ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_DISABLED);
                glfwGetCursorPos(window, &lastMX, &lastMY);
            }
            prevMapM = mNow;
        }

        // ---- F1: ON-FOOT CAMERA CYCLE (Tim: "f1 can cycle... 1st 3rd et").
        // Edge-triggered through shell.key (typing F1-ish text in the console
        // must not swing the camera); only meaningful on foot. The mode
        // persists in jake_cam; a short HUD banner names the new mode.
        {
            static bool f1Was = false;
            const bool f1Now = shell.key(GLFW_KEY_F1);
            if (f1Now && !f1Was && !driving && footSpawned && console) {
                const int m = (console->getInt("jake_cam") + 1)
                              % x3::game::kCharacterCamModes;
                console->set("jake_cam", m == 0 ? "0" : m == 1 ? "1" : "2");
                jakeCamToast = 2.2f;
                x3::logInfo(std::string("[tunnel] camera: ")
                            + x3::game::characterCamModeName(m));
            }
            f1Was = f1Now;
        }

        // ---- W-STATIONS: THE PUMP ----------------------------------------
        // Runs BEFORE the E block because it CLAIMS E: parked under a canopy,
        // E means refuel, not get out. Without that claim, the first press of
        // a held refuel would eject the driver onto the forecourt — the hint
        // says REFUEL and the car says goodbye.
        bool atPump = false;
        {
            if (auto* fcon = shell.console()) gasStations.syncCVars(*fcon);
            float fp[3] = { 0.0f, 0.0f, 0.0f };
            if (carBuilt) car.chassisPos(fp);
            static float lastFx = 0.0f, lastFz = 0.0f; static bool lastFvalid = false;
            const float moved = lastFvalid
                ? std::hypot(fp[0] - lastFx, fp[2] - lastFz) : 0.0f;
            lastFx = fp[0]; lastFz = fp[2]; lastFvalid = true;
            // LOAD, from measured speed rather than the input flag: a car
            // coasting at 30 mph is not drinking like one pinned at 140.
            const float load = carBuilt
                ? std::min(1.0f, std::fabs(car.forwardSpeed()) / 40.0f) : 0.0f;
            gasStations.update(fdt, fp[0], fp[2], driving ? moved : 0.0f, load,
                               driving && kd(GLFW_KEY_E));
            atPump = driving && gasStations.atStation() >= 0;
        }

        // ---- E: GET OUT / GET IN ----------------------------------------
        // Edge-triggered, and re-entry is PROXIMITY gated: you have to walk back
        // to the car. Without that gate E teleports you into a car you left half
        // a mile behind, which is not a vehicle so much as a summoning.
        {
            static bool eWasDown = false;
            const bool eDown = kd(GLFW_KEY_E);   // shell-gated: E while typing is just a letter
            // A GOLDEN TICKET IN REACH OWNS THE PRESS. E already means
            // get-out/get-in, so the two have to be arbitrated somewhere and
            // "the thing you are standing on top of wins" is the only reading
            // that is never surprising: the card is 3.5 m away, the car you
            // would be entering is not. Consuming the edge here is what stops
            // one press both taking the card AND folding you into the driver's
            // seat. The prompt (drawn by GoldenTickets::drawHud) is what makes
            // the rule visible — a control nobody can see is a control nobody
            // has.
            bool eConsumed = false;
            {
                float ppos[3] = { 0, 0, 0 };
                if (!driving && footSpawned) {
                    const x3::phys::Vec3 ft = onFoot.feet();
                    ppos[0] = ft.x; ppos[1] = ft.y; ppos[2] = ft.z;
                } else if (carBuilt) {
                    car.chassisPos(ppos);
                }
                const bool edge = eDown && !eWasDown;
                if (tickets.update(scene, fdt, ppos[0], ppos[1], ppos[2], edge) >= 0) {
                    eConsumed = true;
                    if (tickets.allFound()) factory.openGate();
                }
            }
            // !atPump: at a pump E means REFUEL (W-STATIONS); !eConsumed: a
            // ticket in reach owns the press (W-FACTORY). Both yield to get-in.
            if (eDown && !eWasDown && carBuilt && !atPump && !eConsumed) {
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
                    // FEET ABOVE GROUND — THE LAW (Tim, third strike: "make
                    // his Feet stay ABOVE GROUND"). The old spawn was chassis
                    // arithmetic (+2.4 m left, +1.2 up): midair on an
                    // embankment, INSIDE the hill in a cut — and a capsule
                    // under the heightfield never comes back. Candidates are
                    // tried left / right / behind; each one's Y is a downward
                    // RAYCAST (topmost surface — pavement or dirt), floored by
                    // the terrain height. Feet land ON something, always.
                    const float cand[3][2] = {
                        {  route.dirZ * 2.4f, -route.dirX * 2.4f },   // left
                        { -route.dirZ * 2.4f,  route.dirX * 2.4f },   // right
                        { -route.dirX * 3.2f, -route.dirZ * 3.2f },   // behind
                    };
                    float sx = vp[0], sy = vp[1] + 1.2f, sz = vp[2];
                    for (int ci = 0; ci < 3; ++ci) {
                        const float cx2 = vp[0] + cand[ci][0], cz2 = vp[2] + cand[ci][1];
                        float gy = x3::game::terrainHeightAtWorld(cx2, cz2);
                        const x3::phys::RayHit rh = phys->rayCast(
                            x3::phys::Vec3{ cx2, vp[1] + 30.0f, cz2 },
                            x3::phys::Vec3{ 0.0f, -1.0f, 0.0f }, 90.0f,
                            x3::phys::Layer::Static);
                        if (rh.hit) gy = std::max(gy, rh.point.y);
                        // A candidate more than 4 m below the car is a drop-off
                        // (bridge edge) — try the next side.
                        if (gy > vp[1] - 4.0f) { sx = cx2; sz = cz2; sy = gy + 1.1f; break; }
                    }
                    if (!footSpawned) {
                        onFoot.spawn(*phys, sx, sy, sz);
                        footSpawned = true;
                    } else {
                        onFoot.setFeetPosition(*phys, x3::phys::Vec3{ sx, sy, sz });
                    }
                    driving = false;
                    // Load him ONCE, on the first exit rather than at boot: most
                    // runs of this world never leave the car, and a 1.4 MB rig
                    // plus its textures is not worth paying for on the chance.
                    if (!jakeTried) {
                        jakeTried = true;
                        // THE WHOLE RECIPE lives in AnimatedCharacter now:
                        // exact-name clips from the MEASURED jakeClipTable()
                        // (labels untrusted), root-Y lock, GPU skinning,
                        // contact-law clamp, facing, one-shots. Jake_44_actions
                        // (post tools/jake_bake.py: feet at origin, -Z facing
                        // baked, clips in-place), NOT JakeClone_player (zero
                        // textures, combat clips only — the white statue).
                        if (jake.load(*device, x3::game::assetRoot() + "/rigged_glb",
                                      "Jake_44_actions.glb", x3::game::jakeClipTable()))
                            jakeSwimClipset = jake.swimClipset();   // river lane hook
                        else
                            x3::logWarn("[tunnel] Jake_44_actions.glb failed to load - no body on foot");
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
            // ---- WEAPON KEYS, read FIRST: aiming halves the mouse gain (fine
            // aim) so it must be known before the look deltas are handed to
            // the Player. All shell-gated: typing in the console never fires.
            bool lmb = false, rmb = false;
            bool lmbPressed = false, rmbPressed = false;
            {
                static bool lmbWas = false, rmbWas = false, togWas = false;
                static bool rWas = false, gWas = false;
                // !panelOpen: while an F4/F5 panel is up the mouse belongs to
                // the sliders — dragging RAIN must not also fire the rifle.
                if (shell.inputEnabled() && !gameMenu.panelOpen()) {
                    lmb = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT)  == GLFW_PRESS;
                    rmb = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
                }
                lmbPressed = lmb && !lmbWas;
                rmbPressed = rmb && !rmbWas;
                lmbWas = lmb; rmbWas = rmb;
                // 1 / Q: draw or holster the rifle (owner: "Does he have his
                // weapons?"). Holstered, the unarmed melee below stays.
                const bool togNow = kd(GLFW_KEY_1) || kd(GLFW_KEY_Q);
                if (togNow && !togWas) setRifleArmed(!rifleArmed);
                togWas = togNow;
                if (rifleArmed) {
                    // RMB hold = shoulder aim (module faces the camera; the
                    // camera block pulls over the shoulder; HUD crosshair).
                    rifleAiming = rmb;
                    jake.setAiming(rifleAiming);
                    // R: reload — only if the Arsenal actually began one, so
                    // the hands and the magazine can never disagree.
                    const bool rNow = kd(GLFW_KEY_R);
                    if (rNow && !rWas && rifle.reload()) jake.reloadOneShot();
                    rWas = rNow;
                    // G: grenade toss. The ball leaves at the arm swing
                    // (~1.15 s into Tossgrenade), scheduled below.
                    const bool gNow = kd(GLFW_KEY_G);
                    if (gNow && !gWas && grenadeReleaseT < 0.0f &&
                        jake.grenadeOneShot())
                        grenadeReleaseT = 1.15f;
                    gWas = gNow;
                } else { rWas = gWas = false; }
            }

            x3::game::PlayerInput pin;
            pin.moveFwd    = (kd(GLFW_KEY_W) ? 1.0f : 0.0f) - (kd(GLFW_KEY_S) ? 1.0f : 0.0f);
            pin.moveStrafe = (kd(GLFW_KEY_D) ? 1.0f : 0.0f) - (kd(GLFW_KEY_A) ? 1.0f : 0.0f);
            pin.sprint     = kd(GLFW_KEY_LEFT_SHIFT);
            static bool spaceWas = false;
            const bool spaceNow = kd(GLFW_KEY_SPACE);
            pin.jumpPressed = spaceNow && !spaceWas;
            pin.jumpHeld    = spaceNow;
            spaceWas = spaceNow;
            // W10 swim channels: Space held strokes UP (jumpHeld above), Ctrl/C
            // held dives. Only read while the swim state is active, so dry-land
            // movement is untouched.
            pin.diveHeld = kd(GLFW_KEY_LEFT_CONTROL) || kd(GLFW_KEY_C);
            // FINE AIM: half mouse gain while the rifle is shouldered.
            const float lookGain = rifleAiming ? 0.45f : 1.0f;
            pin.lookDX = ddx * lookGain; pin.lookDY = ddy * lookGain;
            onFoot.update(pin, fdt, *phys);

            // ---- FIRE / MELEE. Armed: LMB fires through the Arsenal (auto —
            // holding it fires at the weapon's rate; the cooldown gates it).
            // Holstered: the unarmed combo one-shots stay exactly as wired
            // (owner: "Punch and kick do not work" — they were never wired).
            if (rifleArmed) {
                // No firing while the hands are reloading or mid grenade toss.
                if (lmb && !rifle.isReloading() && grenadeReleaseT < 0.0f)
                    fireRifleOnce();
            } else if (shell.inputEnabled()) {
                if (lmbPressed)      jake.playOneShot("Backflip_and_Hooks");
                else if (rmbPressed) jake.playOneShot("Backflip_Sweep_Kick");
            }

            // Grenade release: when the toss one-shot reaches the arm swing —
            // or was interrupted — let it go from wherever the hand is.
            if (grenadeReleaseT >= 0.0f) {
                grenadeReleaseT -= fdt;
                if (grenadeReleaseT < 0.0f || !jake.grenadeOneShotActive()) {
                    releaseGrenade();
                    grenadeReleaseT = -1.0f;
                }
            }

            // Everything the rig needs — the CONTACT LAW feet clamp, facing,
            // clip selection (walk/run/backpedal/strafe/turn/jump/fall/idle
            // variation/swim, all MEASURED), and skinning — is the module's
            // job now. The Babylon flip and the -0.9488 yFix are GONE: the
            // asset was baked to the engine convention (tools/jake_bake.py).
            {
                x3::game::AnimatedCharacter::Intent ji;
                ji.moveFwd     = pin.moveFwd;
                ji.moveStrafe  = pin.moveStrafe;
                ji.sprint      = pin.sprint;
                ji.jumpPressed = pin.jumpPressed;
                jake.update(onFoot, ji, camYaw, fdt, *phys, *device);
            }
        }

        // ---- WEAPON TIMERS run EVERY frame: the fire cooldown decays, a
        // reload completes, live grenades cook off even after you climb back
        // into the car, and the FX pool integrates.
        rifle.setBeamHeld(false);   // no charge weapon in this roster
        rifle.tick(fdt);
        tickGrenades(fdt);
        combatFx.update(fdt);

        // ---- JAKE PUSHES THE CAR (Tim: "when jake gets out, he should be
        // able to push the car out of such a situation"). Hold F beside the
        // car: the handbrake releases and a steady ~4 kN shove is applied at
        // ground height toward wherever Jake faces the car from — enough to
        // roll a 1,083 kg car out of a wedge (rolling resistance is ~150 N),
        // nowhere near enough to launch it. Physically honest: one determined
        // human genuinely can push a 993.
        bool pushing = false;
        float pushDir[3] = { 0, 0, 0 };
        if (!driving && footSpawned && carBuilt && kd(GLFW_KEY_F)) {
            float vp[3]; car.chassisPos(vp);
            const x3::phys::Vec3 ft = onFoot.feet();
            const float pdx = vp[0] - ft.x, pdz = vp[2] - ft.z;
            const float d2 = pdx * pdx + pdz * pdz;
            if (d2 < 3.5f * 3.5f && d2 > 0.01f) {
                const float inv = 1.0f / std::sqrt(d2);
                pushing = true;
                pushDir[0] = pdx * inv; pushDir[2] = pdz * inv;
            }
        }
        if (pushing)
            phys->applyImpulse(car.chassis(),
                x3::phys::Vec3{ pushDir[0] * 4000.0f * fdt, 0.0f,
                                pushDir[2] * 4000.0f * fdt });

        x3::phys::VehicleInput in;
        // A PARKED CAR STAYS PARKED — and a car you STEP OUT OF stops. The
        // handbrake alone locks only the rears, so getting out at speed sent
        // the car coasting into the distance on locked rear wheels (Tim: "the
        // car shoots on into the distance when he gets out"). Service brakes
        // on all four until it is actually stationary; then the handbrake
        // holds it and the engine sits at idle (throttle zero IS idle — the
        // audio follows effectiveThrottle, which is unconditionally zeroed).
        // EXCEPT while Jake pushes: a push against the parking brake is a
        // push against a wall, so the brake lifts for exactly as long as F
        // is held in range.
        if (!driving) {
            in = x3::phys::VehicleInput{};
            in.handBrake = pushing ? 0.0f : 1.0f;
            if (!pushing && std::fabs(car.forwardSpeed()) > 0.4f) in.brake = 1.0f;
        }
        else if (carBuilt) {
            in.throttle = (kd(GLFW_KEY_W) ? 1.0f : 0.0f) - (kd(GLFW_KEY_S) ? 1.0f : 0.0f);
            in.steer    = (kd(GLFW_KEY_D) ? 1.0f : 0.0f) - (kd(GLFW_KEY_A) ? 1.0f : 0.0f);
            if (kd(GLFW_KEY_SPACE)) in.handBrake = 1.0f;
            // ---- NITROUS (Tim: "we need NITROUS for the car.. SHIFT will
            // engage it.. That will rocket it to 220mph"). Hold SHIFT: +50%
            // torque on top of vampire and the 35-psi turbo, from a tank —
            // ~4 s of continuous spray, recharging at a quarter rate off the
            // button. A punch you spend, not a cheat you hold.
            {
                const bool wantNos = kd(GLFW_KEY_LEFT_SHIFT) && in.throttle > 0.1f;
                nosActive = wantNos && nosTank > 0.02f;
                static bool nosWasActive = false;
                if (nosActive) {
                    float cq[4]; phys->getBodyRotation(car.chassis(), cq);
                    float fwd[3], up[3];
                    x3::game::vehcam::hullAxes(cq, fwd, up);
                    // THE HIT: the instant the bottle lights, one hard kick —
                    // +2.4 m/s in a frame. That is the seat-slam.
                    if (!nosWasActive)
                        phys->applyImpulse(car.chassis(),
                            x3::phys::Vec3{ fwd[0] * 2600.0f, 0.0f, fwd[2] * 2600.0f });
                    // THE SHOVE: +1.1 g sustained while spraying (was 0.49 —
                    // "This nitrous doesnt" slam; now it does).
                    phys->applyImpulse(car.chassis(),
                        x3::phys::Vec3{ fwd[0] * 12000.0f * fdt, 0.0f,
                                        fwd[2] * 12000.0f * fdt });
                }
                nosWasActive = nosActive;
                nosTank += nosActive ? -fdt / 15.0f : fdt / 20.0f;   // 15 s bottle (Tim), ~20 s recharge
                nosTank = std::min(1.0f, std::max(0.0f, nosTank));
            }
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
        }
        // OUTSIDE the driving/parked split — W-HANDLING's find: setInput lived
        // inside the DRIVING branch only, so every parked-car input this loop
        // so carefully assembled (auto-hold, exit braking, the push's brake
        // release) was dead code; the controller just held its last driving
        // input. The send now covers both branches, every frame.
        if (carBuilt) {
            // Composed power product: vampire (timing) x nitrous (SHIFT). The
            // turbo's own multiplier stacks inside DriveDemo::updateTurbo.
            // NOS = A 200 SHOT: against this tune's peak, +200 hp = x1.19.
            car.setTorqueBoost((vampireOn ? 1.07f : 1.0f) * (nosActive ? 1.19f : 1.0f));
            car.setInput(in);
            car.preStep(fdt);
        }
        float vp[3] = { startPos[0], startPos[1], startPos[2] };
        if (carBuilt) car.chassisPos(vp);
        streamer.update(scene, *device, *phys, vp[0], vp[2]);
        riverLife.prePhysics(fdt);            // boat autopilot BEFORE the step
        {   // TRAFFIC: sim + kinematic march, before the step (the bodies'
            // step-target velocities come from moveKinematic). Focus follows
            // whoever the player currently is — car or Jake on foot.
            float tfoc[3] = { vp[0], vp[1], vp[2] };
            if (!driving && footSpawned) {
                const x3::phys::Vec3 ff = onFoot.feet();
                tfoc[0] = ff.x; tfoc[1] = ff.y; tfoc[2] = ff.z;
            }
            traffic.update(fdt, tfoc, phys.get());
        }
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
        // River life AFTER scene.update (the monster-prop draw contract): boat
        // postStep, driver pose-follow, fish sim, wakes, outboard emitters.
        // Focus = whoever the player currently is (car or Jake) so the schools
        // gate on the real viewpoint.
        {
            x3::phys::Vec3 lifeFocus{ vp[0], vp[1], vp[2] };
            if (!driving && footSpawned) lifeFocus = onFoot.feet();
            riverLife.postPhysics(fdt, scene, *device, *phys,
                                  audioOn ? audio.get() : nullptr, lifeFocus);
        }

        // ---- ENGINE NOTE: re-pitch from live RPM, and move the emitter ------
        // pitch tracks RPM across the powerband; vol fades in off idle so a
        // parked car is not droning at full volume. Both are cheap per-frame
        // parameter updates on ONE voice — no retriggering, so the loop stays
        // seamless through gearchanges.
        if (audioOn && carBuilt) {
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
            const float thr  = std::min(1.0f, std::max(0.0f, car.effectiveThrottle()));
            // ...times what the TURBO is currently delivering. The multiplier
            // runs 0.60 off boost to 1.00 on it, so the note swells over the
            // half-second the compressor takes to come up and drops the instant
            // you lift. That swell is the single most recognisable thing about
            // a turbo car, and it costs one multiply.
            // (Torque curve is EngineNote::torqueCurve — the same table this
            // block used to carry inline, now shared by every wiring site.)
            const float load = thr * x3::game::EngineNote::torqueCurve(frac) * car.turboMult();

            // TURBO SPOOL + BLOWOFF are mode-independent (the psshh one-shot
            // stays regardless of which engine path is sounding).
            const float spoolLag = 0.45f;   // == TurboParams::spoolTau
            if (thr > 0.6f) turboSpool = std::min(1.0f, turboSpool + fdt / spoolLag);
            else            turboSpool = std::max(0.0f, turboSpool - fdt * 2.5f);
            if (prevSpool > 0.55f && thr < 0.2f) {
                audio->playSound2D(engineSnd, 0.45f, 4.2f);   // blowoff psshh
                turboSpool = 0.0f;
            }
            prevSpool = turboSpool;

            const bool bankOn = bankReady && console && console->getFloat("snd_bank") != 0.0f;
            if (bankOn) {
                // ---- ENGINE NOTE v2: the multi-RPM bank -------------------
                // Bracketed pair crossfade + on-load/overrun family fade, all
                // inside EngineNote (which also owns the collapsed off-load
                // floor). Idle-hold feeds the bank's bottom point so the
                // physics' rev hunt never wobbles a parked car's note.
                engineNote.setMuted(false);
                engineNote.update(idling ? 900.0f : rpm, load, fdt, vp[0], vp[1], vp[2]);
                if (engineLoop.valid()) audio->setLoopParams(engineLoop, 0.0f, pitch);
                if (whineLoop.valid()) audio->setLoopParams(whineLoop, 0.0f, 2.4f);
                if (turboLoop.valid()) audio->setLoopParams(turboLoop, 0.0f, 3.0f);

                // Whine + turbo whistle on their OWN synthesized assets
                // (engine_bank/whine_loop.wav, turbo_whistle_loop.wav) instead
                // of pitched-up copies of the engine wav.
                if (whineSnd.valid()) {
                    if (!whineBankLoop.valid()) whineBankLoop = audio->startLoop(whineSnd, 0.0f, 1.0f);
                    if (whineBankLoop.valid())
                        audio->setLoopParams(whineBankLoop, thr * 0.07f, 0.8f + 0.6f * frac);
                }
                if (turboSnd.valid()) {
                    if (!turboBankLoop.valid()) turboBankLoop = audio->startLoop(turboSnd, 0.0f, 1.0f);
                    if (turboBankLoop.valid())
                        audio->setLoopParams(turboBankLoop, turboSpool * 0.08f, 0.7f + 0.6f * turboSpool);
                }
            } else if (engineLoop.valid()) {
                // ---- LEGACY single loop (snd_bank 0 — the A/B reference) ---
                engineNote.setMuted(true);
                if (whineBankLoop.valid()) audio->setLoopParams(whineBankLoop, 0.0f, 1.0f);
                if (turboBankLoop.valid()) audio->setLoopParams(turboBankLoop, 0.0f, 1.0f);
                // Off-throttle is OVERRUN: the engine is being driven by the wheels,
                // so it stays audible and keeps its pitch but drops right back in
                // level. That contrast is most of what makes a car sound driven.
                // OVERRUN IS A DIFFERENT SOUND, NOT THE SAME ONE QUIETER. Measured
                // (SND-FABLE): off-throttle the wheel-locked rpm glides down for
                // seconds while the old 0.16 + 0.10*frac floor kept the loop
                // clearly audible at unchanged timbre — the maximally loop-
                // revealing state ("When I LET OFF... I still hear the Gosh AWful
                // Loop"). Drop the floor hard off-load; the pitch tail is still
                // there, just far behind the tire/wind bed instead of in front.
                const float onLoad = std::min(1.0f, load * 6.0f);   // 0 off-throttle
                const float vol  = 0.05f + 0.11f * onLoad + 0.62f * load
                                 + 0.10f * frac * (0.35f + 0.65f * onLoad);
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
                    audio->setLoopParams(whineLoop, thr * 0.09f, 2.4f + 1.3f * frac);   // halved: same-wav layer (SND-FABLE #3)
                if (!turboLoop.valid()) turboLoop = audio->startLoop(engineSnd, 0.0f, 3.0f);
                if (turboLoop.valid())
                    audio->setLoopParams(turboLoop, turboSpool * 0.09f, 3.0f + 1.2f * turboSpool);   // halved: same-wav layer
            }

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
          // Muzzle-flash / grenade-boom pulses ride the same single upload
          // (a second setPointLights call would overwrite the pool).
          x3::rhi::PointLight wl[2];
          const uint32_t wn = weaponLights(fdt, wl);
          x3::game::uploadTunnelLights(*device, cp, wn ? wl : nullptr, wn); }
        // SPEED FOV. Physical speed alone does not read as fast on a screen —
        // the frame has to widen and the periphery has to rush. 72 deg parked ->
        // 88 flat out, eased so it swells under acceleration instead of snapping.
        // This is the 1990s arcade trick and it is still the highest
        // feel-per-line change available (see TUNNEL_NEXT.md section 2 on NFS).
        {
            const float sp   = carBuilt ? std::fabs(car.forwardSpeed()) : 0.0f;
            const float t    = std::min(1.0f, sp / 55.0f);       // ~123 mph = full
            float want = 72.0f + 16.0f * t * t;                   // eased, not linear
            // NOS FOV PUNCH: the world stretches away while the bottle sprays
            // — fast in (12/s), lazy out (3/s), +10 degrees on top of speed.
            static float nosFov = 0.0f;
            nosFov += ((nosActive ? 10.0f : 0.0f) - nosFov)
                    * std::min(1.0f, fdt * (nosActive ? 12.0f : 3.0f));
            want += nosFov;
            static float fovNow = 72.0f;
            fovNow += (want - fovNow) * std::min(1.0f, fdt * 3.0f);   // smooth
            // ON FOOT the camera IS the player's eye, not a chase rig pulled back
            // off a capsule. The speed-eased FOV above belongs to driving and is
            // deliberately dropped here: a walking FOV that breathes with your
            // pace is nauseating. Both modes still land on ONE setCamera, so the
            // precipitation volume and the sky-visibility ray follow the eye
            // without a second code path to keep in step.
            // NOCLIP (D-CONSOLE fold): seed the freefly from wherever the chase/
            // on-foot camera currently sits, then let it take over the actual
            // setCamera calls below. `noclip` detaches fully — the car keeps
            // driving/parked and Jake keeps standing wherever he was, but
            // neither one drives the VIEW while it is active. `noclip 0`
            // returns to exactly this chase-cam code, unmodified.
            shell.trackCamera(cx, cy, cz, camYaw, camPitch);
            if (shell.overrideCamera(fdt, (!driving && footSpawned) ? 74.0f : fovNow)) {
                shell.flyCamPose(cx, cy, cz, camYaw, camPitch);   // keep precip/audio probes with the free cam
            } else if (!driving && footSpawned) {
                // F1 CAMERA CYCLE (Tim: "he needs to be able to look around
                // with the mouse... f1 can cycle... 1st 3rd et"): first person
                // / third-near (2.5 m) / third-far (5.5 m), all riding the
                // SAME Player look angles so the mouse orbits in every mode.
                // The mode lives in the jake_cam cvar (persisted, dialable).
                const int camMode = console ? console->getInt("jake_cam")
                                            : (int)x3::game::CharacterCamMode::ThirdFar;
                // AIM CAMERA (RMB): pull in over the near shoulder + tighten
                // the lens — the over-the-shoulder fine-aim frame. First
                // person aims where it already is (the eye IS the sight).
                const bool aimCam = rifleAiming &&
                    camMode != (int)x3::game::CharacterCamMode::FirstPerson;
                const int useMode = aimCam
                    ? (int)x3::game::CharacterCamMode::ThirdNear : camMode;
                x3::game::characterCameraEye(onFoot, useMode, cx, cy, cz, camYaw, camPitch);
                device->setCamera(cx, cy, cz, camYaw, camPitch,
                                  aimCam ? 62.0f : 74.0f);
            } else {
                device->setCamera(cx, cy, cz, camYaw, camPitch, fovNow);
            }
            // UNDERWATER TINT (cheap: the engine's own Beer-Lambert fog pass;
            // full underwater rendering is another lane's task). The moment
            // the CAMERA is below the water surface at its own (x,z), the
            // world greens out over ~18 m instead of rendering dry air with a
            // white ceiling. Edge-triggered so the fog lever stays free.
            {
                const float wSurf = x3::game::worldWaterLevelAt(cx, cz);
                const bool under = (wSurf > x3::game::kWorldWaterDry + 1.0f) &&
                                   (cy < wSurf - 0.05f);
                static bool wasUnder = false;
                if (under != wasUnder) {
                    wasUnder = under;
                    x3::rhi::IRenderDevice::FogParams fp{};
                    if (under) {
                        fp.enabled  = true;
                        fp.color[0] = 0.010f; fp.color[1] = 0.045f; fp.color[2] = 0.055f;
                        fp.density  = 0.055f;      // ~18 m of green visibility
                        fp.start    = 0.15f;
                        fp.maxOpacity = 0.96f;
                    }
                    device->setFog(fp);
                }
            }
            // Sky visibility does double duty: precipitation gating AND the
            // room-reverb estimate (SND-OPUS item: the tunnel bore should
            // ECHO). One probe, two consumers — zero new raycast kinds.
            const float skyVis = skyVisibleAt(*phys, cx, cy, cz, route.dirX, route.dirZ);
            if (weatherOn) {
                // W-MENU: the F4 WIND sliders lean the falling columns — the
                // wind lane precip_fx always had, finally fed (rule 6).
                const float wSpd = console->getFloat("wx_wind");
                const float wDir = console->getFloat("wx_winddir") * 0.0174533f;
                precip.update(fdt, precipKind, precipAmt, cx, cy, cz,
                              std::cos(wDir) * wSpd, std::sin(wDir) * wSpd, skyVis);
            }
            // Under open sky: short, nearly-dry (t60 0.3 s, wet 0.05). Deep in
            // the bore: a long concrete tail (t60 2.5 s, wet 0.45). Both are
            // smoothed on the audio thread, so driving through the portal is a
            // swell, not a step. Loop voices (the engine bank) and 3D one-shots
            // all ride the same insert.
            if (audioOn)
                audio->setReverbParams(0.3f + 2.2f * (1.0f - skyVis),
                                       0.05f + 0.40f * (1.0f - skyVis));
        }
        // ---- W-MENU: DDGI SCROLLING VOLUME --------------------------------
        // The device's auto-fit covers ~240 m around wherever GI happened to
        // be switched on; this world is 46 miles of road. While r_ddgi is
        // live, keep an EXPLICIT probe volume centred on the player and
        // re-centre it every 150 m of travel — setDdgiParams refits on an
        // explicit origin change (the engine-side half of this feature) and
        // the warm-up ramp reconverges in a handful of frames. 420 m across
        // 24 probes ≈ 18 m spacing: coarse, and right for outdoor GI (the
        // echotropolis grid ships at ~70 m).
        {
            static float ddgiCx = 0.0f, ddgiCz = 0.0f;
            static bool  ddgiCentered = false;
            const bool ddgiOn = console->getInt("r_ddgi") != 0;
            if (ddgiOn) {
                float fx2 = vp[0], fy2 = vp[1], fz2 = vp[2];
                if (!driving && footSpawned) {
                    const x3::phys::Vec3 ft = onFoot.feet();
                    fx2 = ft.x; fy2 = ft.y; fz2 = ft.z;
                }
                const float dxd = fx2 - ddgiCx, dzd = fz2 - ddgiCz;
                if (!ddgiCentered || dxd * dxd + dzd * dzd > 150.0f * 150.0f) {
                    ddgiCentered = true; ddgiCx = fx2; ddgiCz = fz2;
                    x3::rhi::IRenderDevice::DdgiParams dg{};
                    dg.enabled      = true;
                    dg.debug        = console->getInt("r_ddgi_debug");
                    dg.raysPerProbe = console->getInt("r_ddgi_rays");
                    dg.intensity    = console->getFloat("r_ddgi_intensity");
                    dg.countX       = console->getInt("r_ddgi_nx");
                    dg.countY       = console->getInt("r_ddgi_ny");
                    dg.countZ       = console->getInt("r_ddgi_nz");
                    dg.hysteresis   = console->getFloat("r_ddgi_hyst");
                    dg.originX = fx2 - 210.0f; dg.originY = fy2 - 45.0f; dg.originZ = fz2 - 210.0f;
                    dg.sizeX = 420.0f; dg.sizeY = 150.0f; dg.sizeZ = 420.0f;
                    device->setDdgiParams(dg);
                    char db[128];
                    std::snprintf(db, sizeof(db),
                                  "[tunnel] DDGI volume re-centred on (%.0f, %.0f) — 420x150x420 m",
                                  fx2, fz2);
                    x3::logInfo(db);
                }
            } else {
                ddgiCentered = false;   // re-enable re-centres on the player
            }
        }
        // The town's pedestrians walk their sidewalk loop. Gated on camera
        // distance INSIDE Town::update (kPedActiveM) — outside it the terrain
        // tiles under their feet are not resident and the walk is a free-fall.
        if (townOn) town.update(fdt, *phys, *device, cx, cz);
        // THE WORKS, ALIVE: the tube cores breathe, the plant blocks shake, the
        // gate slides, the stacks make smoke. Cheap — a few entity transforms
        // and one particle integrator.
        if (facOn) factory.update(scene, fdt);

        auto frame = device->beginFrame();
        if (frame.valid) {
            scene.render(*device, frame);
            trees.draw(*device, frame);
            if (townOn) town.draw(*device, frame);
            gasStations.draw(*device, frame);
            // Particle batches are CLEARED by beginFrame, so these submits live
            // INSIDE the frame and are never hoisted (the warning host_echotropolis
            // carries at its own submitParticles site).
            if (facOn) { const float fcam2[3] = { cx, cy, cz }; factory.drawSmoke(*device, fcam2); }
            tickets.drawGlints(*device);
            {   // forests: prune by the live camera (fwd = cos/sin yaw, 4.1)
                const float fcam[3] = { cx, cy, cz };
                forests.draw(*device, frame, fcam,
                             std::cos(camYaw), std::sin(camYaw));
            }
            if (carBuilt) car.render(frame);
            {   const float fcam[3] = { cx, cy, cz };
                traffic.render(frame, fcam);           // the freeway is populated
            }
            riverLife.render(*device, frame, scene);   // boats + drivers + wakes
            // Combat FX: tracers + muzzle boxes (mesh draws), then the
            // particle pool + impact decals (billboards through
            // submitParticles). After the world, before the HUD.
            combatFx.draw(*device, frame, cx, cy, cz, camYaw, camPitch);
            combatFx.submit(*device, frame);
        }

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
                    // TRANSLUCENT, WISPY: three overlapping soft billboards per
                    // puff, deterministically jittered by particle index (no
                    // rand — the LCG discipline precip_fx documents), each low
                    // alpha so wisps come from OVERLAP, not from any one quad.
                    // They grow, rise, drift apart, and thin to nothing.
                    const float t = f.age / 1.6f;
                    const float fade = std::max(0.0f, 1.0f - t);
                    for (int k = 0; k < 3; ++k) {
                        const uint32_t h = (i * 2654435761u) ^ (uint32_t)(k * 40503u);
                        const float jx = (((h >> 3) & 255) / 255.0f - 0.5f) * (0.25f + 0.9f * t);
                        const float jz = (((h >> 11) & 255) / 255.0f - 0.5f) * (0.25f + 0.9f * t);
                        const float jy = (((h >> 19) & 255) / 255.0f) * 0.30f * t;
                        x3::rhi::IRenderDevice::ParticleInstance pi;
                        pi.pos[0] = f.x + jx;
                        pi.pos[1] = f.y + 0.20f + 0.55f * t + jy;
                        pi.pos[2] = f.z + jz;
                        pi.size   = 0.22f + 0.85f * t;
                        pi.color[0] = 0.62f; pi.color[1] = 0.62f; pi.color[2] = 0.65f;
                        pi.color[3] = fade * fade * 0.16f;   // quadratic out — vapor thins fast
                        fxPuffs.push_back(pi);
                    }
                }
            }
            if (!fxPuffs.empty()) {
                device->submitParticles(fxPuffs.data(), (uint32_t)fxPuffs.size(),
                                        x3::rhi::IRenderDevice::ParticleBlend::Alpha);
                fxPuffs.clear();
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
            float R = 0.0f, gcx = 0.0f, gcy = 0.0f;
            gaugeClusterAnchor(fw, fh, R, gcx, gcy);
            const float mar = 0.030f * fh;
            const float gateH = R * 0.90f;

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
                // Under a canopy the pump owns the line AND the key (see the
                // atPump gate on the E block above) — "E  GET OUT" there would
                // advertise the one thing E no longer does.
                if (driving && gasStations.prompt()) prompt = gasStations.prompt();
                else if (driving) prompt = "E  GET OUT";
                else if (footSpawned) {
                    const float dxc = cx - parkedAt[0], dzc = cz - parkedAt[2];
                    prompt = (dxc*dxc + dzc*dzc <= 16.0f)
                                 ? (pushing ? "PUSHING..." : "E  GET IN    F  PUSH")
                                 : "WALK BACK TO THE CAR TO DRIVE";
                }
                // ONE prompt renderer, shared with the headless proof capture
                // (app/gas_station.h drawPumpPrompt): a screenshot of a hint the
                // player is not actually shown proves nothing.
                (void)hw2; (void)hh2;
                x3::game::drawPumpPrompt(*device, frame, prompt);
            }

            // ---- DRAW JAKE. Placement, facing and the yFix that used to live
            // here are the module's job — and the yFix is GONE outright: the
            // asset now carries feet-at-origin, -Z-forward (tools/jake_bake.py),
            // so identity IS correct and jake_yaw/jake_y are pure trims
            // (default 0). Hidden in first person: you ARE the head.
            if (!driving && footSpawned) {
                const bool firstPerson = console &&
                    console->getInt("jake_cam") == (int)x3::game::CharacterCamMode::FirstPerson;
                jake.draw(frame, *device, onFoot,
                          (console ? console->getFloat("jake_yaw") : 0.0f) * 0.0174533f,
                          console ? console->getFloat("jake_y") : 0.0f,
                          !firstPerson);
                // THE RIFLE IN HIS HAND — the Arsenal's loaded Railgun GLB at
                // the module's hand socket (hidden with the body in FP).
                if (rifleArmed && !firstPerson) {
                    float wm[16];
                    if (heldRifleWorld(wm)) rifle.drawCurrentAt(*device, frame, wm);
                }

                // ---- RIFLE HUD: ammo bottom-left; crosshair while aiming
                // (Hud::drawCrosshair — the existing S7 reticle, not a re-draw).
                if (rifleArmed) {
                    uint32_t hw = 0, hh = 0; device->hudSize(hw, hh);
                    if (hw && hh) {
                        char ab[48];
                        const auto& ws = rifle.currentState();
                        if (rifle.isReloading())
                            std::snprintf(ab, sizeof(ab), "RELOADING...");
                        else
                            std::snprintf(ab, sizeof(ab), "RIFLE  %d / %d",
                                          ws.ammoInMag, ws.reserve);
                        const float px = std::floor((float)hh * 0.026f);
                        const float tx = (float)hh * 0.045f;
                        const float ty = (float)hh * 0.92f;
                        const float sh4[4]  = { 0.0f, 0.0f, 0.0f, 0.75f };
                        const float amc[4] = { 1.0f, 0.93f, 0.72f, 1.0f };
                        device->drawHudText(frame, ab, tx + 1.0f, ty + 1.0f, px, sh4);
                        device->drawHudText(frame, ab, tx, ty, px, amc);
                        if (rifleAiming) wpnHud.drawCrosshair(*device, frame);
                    }
                }
            }

            // ---- F1 CAMERA MODE BANNER — a short, centered confirmation so
            // cycling is legible ("what mode am I in?" should never be a
            // guess). Fades out by simply not drawing after 2.2 s.
            if (jakeCamToast > 0.0f) {
                jakeCamToast -= fdt;
                uint32_t hw3 = 0, hh3 = 0; device->hudSize(hw3, hh3);
                if (hw3 && hh3 && console) {
                    const char* nm = x3::game::characterCamModeName(console->getInt("jake_cam"));
                    char tb[64];
                    std::snprintf(tb, sizeof(tb), "CAMERA: %s", nm);
                    const float px = std::floor((float)hh3 * 0.030f);
                    const float tw = (float)std::strlen(tb) * px;
                    const float tx = ((float)hw3 - tw) * 0.5f, ty = (float)hh3 * 0.12f;
                    const float sh[4]  = { 0.0f, 0.0f, 0.0f, 0.75f };
                    const float fgc[4] = { 0.75f, 0.92f, 1.0f, 1.0f };
                    device->drawHudText(frame, tb, tx + 1.0f, ty + 1.0f, px, sh);
                    device->drawHudText(frame, tb, tx, ty, px, fgc);
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

                constexpr float kPsiMin = -10.0f, kPsiMax = 40.0f;   // == the art (35-psi build)
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
                const bool  over = psi >= 30.0f;   // the art's red band
                const float bc[4] = { over ? 1.0f : 0.97f, over ? 0.32f : 0.98f,
                                      over ? 0.24f : 1.0f, 1.0f };
                device->drawHudText(frame, bbuf, bcx - bw * 0.5f,
                                    bcy + R2 * 0.26f, bp, bc);
            }

            if (texNos.valid()) {
                // ---- NOS TANK — SOLID LUMINESCENT CURVED BAR (Tim: "Curving
                // bar like NFS had 20 years ago... not beads. solid
                // luminescent bars"). A 32-state baked-arc atlas (hot core +
                // glow, husk for the spent span); the frame is picked by tank
                // level — the needle-atlas pattern applied to a fill. Drains
                // in ~4 s of spray, RECHARGES off the button in ~16 s.
                const float R2  = R * 0.70f;
                const float bcx = gcx - R - R2 - R * 0.10f;
                const float bcy = gcy + R - R2;
                const int NF2 = 32, AC = 8;
                int fi = (int)(nosTank * (NF2 - 1) + 0.5f);
                fi = fi < 0 ? 0 : (fi > NF2 - 1 ? NF2 - 1 : fi);
                const float u0 = (float)(fi % AC) / (float)AC;
                const float v0 = (float)(fi / AC) / 4.0f;
                // Cell arc radius is 0.86 * half-cell; on screen the arc sits
                // at 1.22 * R2, so the drawn cell spans 2 * 1.22 / 0.86 * R2.
                const float side = 2.837f * R2;
                float tint[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
                if (nosActive) { tint[0] = 1.25f; tint[1] = 1.15f; }   // spray flare
                device->drawHudImage(frame, texNos, bcx - side * 0.5f, bcy - side * 0.5f,
                                     side, side, tint, u0, v0, u0 + 1.0f / AC, v0 + 0.25f);
                const float lp2 = R * 0.085f;
                const float lc2[4] = { 0.55f, 0.85f, 1.0f, 1.0f };
                device->drawHudText(frame, "NOS", bcx - R2 * 1.22f - lp2 * 1.2f,
                                    bcy + R2 * 0.95f, lp2, lc2);
            }

            // ---- THE FUEL GAUGE (W-STATIONS). Drawn by the SAME function the
            // headless proof capture calls (app/gas_station.h), anchored on this
            // cluster's tach so it always rides under the dials.
            x3::game::drawFuelBar(*device, frame, gasStations.fuel(),
                                  gasStations.refuelling(), R, gcx, gcy);

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
            {   // ---- MINIMAP v2 (owner: bigger, WITH roads and water) -----
                const float mmR   = 0.16f * fh;               // half-size, px
                const float mmCx  = fw - mmR - 16.0f;
                const float mmCy  = mmR + 52.0f;
                const float mmRange = 900.0f;                 // metres shown
                const float mmScale = mmR / mmRange;
                const float bgq[4] = { 0.04f, 0.06f, 0.09f, 0.40f };
                device->drawHudQuad(frame, mmCx - mmR, mmCy - mmR, mmR * 2.0f, mmR * 2.0f, bgq);
                const float rim[4] = { 0.55f, 0.65f, 0.75f, 0.55f };
                device->drawHudQuad(frame, mmCx - mmR, mmCy - mmR, mmR * 2.0f, 2.0f, rim);
                device->drawHudQuad(frame, mmCx - mmR, mmCy + mmR - 2.0f, mmR * 2.0f, 2.0f, rim);
                device->drawHudQuad(frame, mmCx - mmR, mmCy - mmR, 2.0f, mmR * 2.0f, rim);
                device->drawHudQuad(frame, mmCx + mmR - 2.0f, mmCy - mmR, 2.0f, mmR * 2.0f, rim);
                auto mmStampLine = [&](float ax, float az, float bx2, float bz2,
                                       float px, const float col[4], bool dashed) {
                    const float segLen = std::sqrt((bx2-ax)*(bx2-ax) + (bz2-az)*(bz2-az));
                    const int steps = std::max(2, (int)(segLen * mmScale / 1.6f));
                    for (int k2 = 0; k2 <= steps; ++k2) {
                        if (dashed && ((k2 / 5) & 1)) continue;
                        const float t2 = (float)k2 / (float)steps;
                        const float px2 = ax + (bx2-ax)*t2, pz2 = az + (bz2-az)*t2;
                        if (px2*px2 + pz2*pz2 > mmRange*mmRange) continue;
                        device->drawHudQuad(frame, mmCx + px2 * mmScale - px * 0.5f,
                                            mmCy + pz2 * mmScale - px * 0.5f, px, px, col);
                    }
                };
                // WATER first (under the roads): the river's own working chain.
                {
                    uint32_t nR = 0;
                    const x3::game::WorldRiverNode* rn = x3::game::worldRiverNodes(nR);
                    const float wcol[4] = { 0.25f, 0.55f, 0.95f, 0.80f };
                    for (uint32_t i2 = 0; rn && i2 + 1 < nR; ++i2)
                        mmStampLine(rn[i2].x - vp[0],   rn[i2].z - vp[2],
                                    rn[i2+1].x - vp[0], rn[i2+1].z - vp[2],
                                    5.0f, wcol, false);
                }
                const float roadc[4] = { 0.95f, 0.96f, 0.99f, 0.92f };
                for (const auto& o : mapRoutes) {
                    const size_t n = std::min(o.x.size(), o.z.size());
                    for (size_t i2 = 0; i2 + 1 < n; ++i2) {
                        const float ax = o.x[i2] - vp[0],    az = o.z[i2] - vp[2];
                        const float bx2 = o.x[i2+1] - vp[0], bz2 = o.z[i2+1] - vp[2];
                        if ((ax*ax + az*az > mmRange*mmRange) &&
                            (bx2*bx2 + bz2*bz2 > mmRange*mmRange)) continue;
                        mmStampLine(ax, az, bx2, bz2, 3.6f, roadc, o.dashed);
                    }
                }
                // the car: bright blip + heading tick
                float cq2[4]; phys->getBodyRotation(car.chassis(), cq2);
                float mfw[3], mup[3];
                x3::game::vehcam::hullAxes(cq2, mfw, mup);
                const float blip[4] = { 1.0f, 0.35f, 0.25f, 1.0f };
                device->drawHudQuad(frame, mmCx - 3.5f, mmCy - 3.5f, 7.0f, 7.0f, blip);
                device->drawHudQuad(frame, mmCx + mfw[0] * 11.0f - 2.0f,
                                    mmCy + mfw[2] * 11.0f - 2.0f, 4.0f, 4.0f, blip);
            }
            {   // Key hints on the glass. A binding nobody can see does not
                // exist: T toggled traction control for a whole session while
                // the only mention of it went to a log file.
                const float hp = R * 0.085f;
                const float hcol[4] = { 0.52f, 0.57f, 0.66f, 1.0f };
                device->drawHudText(frame, "F4 WEATHER  F5 LIGHTS", gcx - R * 0.95f,
                                    gcy - R * 2.00f, hp, hcol);
                device->drawHudText(frame, "~  CONSOLE",      gcx - R * 0.95f,
                                    gcy - R * 1.64f, hp, hcol);
                device->drawHudText(frame, "SHIFT  NITROUS",  gcx - R * 0.95f,
                                    gcy - R * 1.88f, hp, hcol);
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
        // ---- DRIVING-HUD WAYPOINT CHEVRON (map/HUD wiring; M CLOSED) -------
        // drawWaypointChevron (defined near the map's road layer, above) is
        // the SAME function the headless map/HUD proof set calls -- one
        // implementation, not a parallel copy that can drift.
        if (frame.valid && !mapOpen && wmap.waypoint().active) {
            const x3::game::Waypoint& wpv = wmap.waypoint();
            float pPos[3] = { vp[0], vp[1], vp[2] };
            if (!driving && footSpawned) {
                const x3::phys::Vec3 ft = onFoot.feet();
                pPos[0] = ft.x; pPos[1] = ft.y; pPos[2] = ft.z;
            }
            drawWaypointChevron(frame, wpv.x, pPos[1], wpv.z, pPos[0], pPos[1], pPos[2], camYaw);
        }
        // ---- TICKETS n/5, the [E] prompt and the pickup toast --------------
        if (frame.valid && !mapOpen) {
            float pPos2[3] = { vp[0], vp[1], vp[2] };
            if (!driving && footSpawned) {
                const x3::phys::Vec3 ft = onFoot.feet();
                pPos2[0] = ft.x; pPos2[1] = ft.y; pPos2[2] = ft.z;
            }
            tickets.drawHud(*device, frame, pPos2[0], pPos2[1], pPos2[2]);
        }
        // ---- THE MAP SCREEN (M). Drawn over the world and the cluster, under
        // the shell (the console stays reachable over the map). Input assembly
        // is host_streamed's: raw WASD/arrows pan (the car's WASD is gated off
        // above), wheel zooms at the cursor, click/ENTER sets the waypoint,
        // and the ESC edge arrives through the shell's escape handler.
        if (frame.valid && mapOpen) {
            // OPAQUE UNDERLAY. The map's own backdrop is 0.97 alpha, which is
            // invisible over an interior but lets 3% of this world's HDR sky
            // through — enough to wash the whole screen. The map system is
            // shared, so the host lays its own alpha-1 slab under it instead
            // of changing everyone's backdrop.
            {
                int ufw = 0, ufh = 0; glfwGetFramebufferSize(window, &ufw, &ufh);
                const float mapBg[4] = { 0.014f, 0.025f, 0.045f, 1.0f };
                device->drawHudQuad(frame, 0.0f, 0.0f, (float)ufw, (float)ufh, mapBg);
            }
            double cmx = 0.0, cmy = 0.0; glfwGetCursorPos(window, &cmx, &cmy);
            const bool lmb = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
            x3::ui::UiInput ui0{};
            ui0.mouseX = (float)cmx; ui0.mouseY = (float)cmy;
            ui0.mouseDown = lmb; ui0.mousePressed = lmb && !prevMapLmb;
            wmapUi.begin(*device, frame, ui0);
            x3::game::WorldMapSystem::ScreenInput msi{};
            msi.mouseX = ui0.mouseX; msi.mouseY = ui0.mouseY;
            msi.mouseDown = ui0.mouseDown; msi.mousePressed = ui0.mousePressed;
            msi.wheel = (float)g_weaponScroll;
            msi.keyW = glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS || glfwGetKey(window, GLFW_KEY_UP)    == GLFW_PRESS;
            msi.keyS = glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS || glfwGetKey(window, GLFW_KEY_DOWN)  == GLFW_PRESS;
            msi.keyA = glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS || glfwGetKey(window, GLFW_KEY_LEFT)  == GLFW_PRESS;
            msi.keyD = glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS || glfwGetKey(window, GLFW_KEY_RIGHT) == GLFW_PRESS;
            const bool entNow = glfwGetKey(window, GLFW_KEY_ENTER) == GLFW_PRESS ||
                                glfwGetKey(window, GLFW_KEY_KP_ENTER) == GLFW_PRESS;
            msi.enterEdge = entNow && !prevMapEnter;
            prevMapEnter = entNow;
            msi.escEdge = mapEsc;
            // The blip is the CAR (or Jake, on foot), with its real heading.
            float ppx = vp[0], ppy = vp[1], ppz = vp[2];
            float mapYaw = camYaw;
            if (driving && carBuilt) {
                float cq[4]; phys->getBodyRotation(car.chassis(), cq);
                // forward = q * (0,0,-1) (rest forward is -Z, CONVENTIONS §3);
                // the map's arrow wants that forward as a world-XZ angle.
                const float fwdX = -2.0f * (cq[0] * cq[2] + cq[3] * cq[1]);
                const float fwdZ = -(1.0f - 2.0f * (cq[0] * cq[0] + cq[1] * cq[1]));
                mapYaw = std::atan2(fwdZ, fwdX);
            } else if (footSpawned) {
                const x3::phys::Vec3 ft = onFoot.feet();
                ppx = ft.x; ppy = ft.y; ppz = ft.z;
            }
            msi.playerX = ppx; msi.playerY = ppy; msi.playerZ = ppz;
            msi.playerYaw = mapYaw;
            msi.locationName = "TUNNEL RIDGE - ROAD NETWORK";
            wmap.drawScreen(wmapUi, *device, frame, msi, mapFlags, fdt);
            wmapUi.end();
            prevMapLmb = lmb;
        } else {
            prevMapLmb   = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
            prevMapEnter = glfwGetKey(window, GLFW_KEY_ENTER) == GLFW_PRESS;
        }
        g_weaponScroll = 0.0;        // consumed (or discarded) every frame

        // ---- W-MENU: the LIVE F4/F5 panels — over the HUD, under the map and
        // the console. The world keeps simulating behind them (that is their
        // whole point: you SEE the rain arrive / the GI light up as you drag).
        if (frame.valid && !mapOpen && gameMenu.panelOpen())
            gameMenu.draw(frame, gmIn, fdt, todHours);
        if (gameMenu.takeQuitRequest()) glfwSetWindowShouldClose(window, GLFW_TRUE);
        if (gameMenu.takeConsoleRequest()) shell.hudForCallbacks().toggleConsole();

        shell.draw(frame, fdt);      // console + FPS/stats, over everything
        device->endFrame(frame);
    }

    if (audioOn) engineNote.shutdown();          // bank voices before the mixer dies
    phys->setContactCallback(nullptr, nullptr);            // trafficCtx dies with this scope
    traffic.shutdown(phys.get());                          // kinematic boxes out before phys
    factory.shutdown(*device);                   // pack loaders + the smoke pool
    tickets.shutdown(*device);                   // the card mesh/textures it owns
    riverLife.shutdown(audioOn ? audio.get() : nullptr);   // outboard loops + hulls
    wmap.shutdown(*device);                      // no tiles baked here, but symmetric
    gasStations.shutdown(*device);
    trees.shutdown(*device);
    if (townOn) town.shutdown(*device);
    forests.shutdown(*device);
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
