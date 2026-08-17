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
#include "../road_trees.h"
#include "../tunnel_fitout.h"
#include "../tunnel_rooms.h"
#include "../player.h"
#include "../anim.h"                     // Skinner — Jake's idle/walk/run rig

#include <array>
#include <memory>
#include "../road_network.h"
#include "../river_bridge.h"
#include "../river_life.h"       // W-RIVER — fish + AI speedboats on the reach
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
    device->setPointLights(tunnel.lights().data(), (uint32_t)tunnel.lights().size());

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
        trees.build(*device, route, camKeepOut);
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

    // ---- JAKE. The on-foot camera was a first-person eye, which is exactly
    // why you could not see him: you were inside his head. A body nobody can see
    // is a body nobody has, so getting out now pulls the camera back and puts
    // the man on screen.
    std::unique_ptr<x3::asset::IAssetSource> jakeSrc;
    std::unique_ptr<x3::asset::IModelLoader> jakeLoader;
    x3::asset::Model jakeModel;
    std::vector<x3::asset::ModelDrawable> jakeDraw;
    bool jakeTried = false;
    // ANIMATION (the Sarah recipe, sarah.cpp): a Skinner drives idle/walk/run
    // by planar speed. Tim: "Jake isn't rigged... He needs his textures.. his
    // animations" — he was right on both counts, and both had one cause: the
    // host loaded JakeClone_player.glb, which has 20 COMBAT clips and ZERO
    // textures (that is why he rendered white), while Jake_44_actions.glb —
    // sitting in the same directory — has the baseColor texture AND the
    // Idle / walking / run clips. The 26 MB earns its keep the moment
    // something plays those clips, and now something does.
    x3::anim::Skinner jakeSkin;
    bool  jakeAnimated = false;
    int   jakeJumpClip = -1;
    float jakeJumpT    = -1.0f;      // >=0 while the jump one-shot plays
    int   jakePunchClip = -1, jakeKickClip = -1;
    int   jakeActClip  = -1;         // active combat one-shot
    float jakeActT     = -1.0f;
    float jakeYaw = 0.0f;                    // faces his MOVEMENT, not the camera
    float jakePrevFeet[3] = { 0, 0, 0 };

    x3::game::Player onFoot;
    // W10 SWIMMING, wired (owner: "we need water.. you can swim in"). The
    // Player has carried the full swim state machine since W10 — buoyancy
    // spring, swim-along-look, Space-up/Ctrl-down, enter/exit hysteresis — it
    // just never got a water feed in THIS host, so Jake hiked the riverbed
    // dry under the water table.
    //
    // THE FEED IS THE SURFACE HE CAN SEE. This host draws ONE flat Gerstner
    // plane at the bridge plan's waterY, while worldWaterLevelAt reports the
    // spline's sloping level — 0.26 m below the plane a mere 32 m downstream.
    // Fed the spline, he treads with his head under the drawn surface. So
    // inside the bridge reach the PLANE owns the answer; beyond it (and in
    // the ocean) the spline query stands. Same clamp the fish got.
    {
        const bool  rOn = riverOn && riverRoad.plan.ok;
        const float rcx = rOn ? riverRoad.plan.cx : 0.0f;
        const float rcz = rOn ? riverRoad.plan.cz : 0.0f;
        const float rwy = rOn ? riverRoad.plan.waterY : 0.0f;
        // The +0.35 m bias: the Player's buoyancy rests the EYE just above the
        // fed surface, which leaves the drawn head bobbing right AT the
        // waterline — reading as submerged whenever a crest rolls through. A
        // treading human rides higher than his eye line; reporting the surface
        // a hand-span HIGH makes the buoyancy lift him that much further, so
        // head + shoulders clear the drawn plane.
        onFoot.setWaterQuery([rOn, rcx, rcz, rwy](float x, float z) {
            const float w = x3::game::worldWaterLevelAt(x, z);
            if (!rOn || w <= x3::game::kWorldWaterDry + 1.0f) return w;
            const float dx = x - rcx, dz = z - rcz;
            if (dx * dx + dz * dz < 260.0f * 260.0f) return rwy + 0.35f;
            return w;
        });
    }
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

    // THE RIVER HOLDS WATER — one lambda, BOTH render paths. The water pass
    // used to be armed only inside the interactive loop, so every headless
    // capture (the proof shots included) rendered a dry river: the gate was
    // fine, the pass was never enabled on that path at all. Tone per the
    // owner's eyes-on: "too bright... reject from echo harbor" — a river under
    // this sun is dark blue-green with a modest glint, so deep/shallow go
    // darker+greener than the sea defaults, specular drops 12->5 and the
    // fresnel floor 0.02->0.012 (less sky mirror face-on). Caustics ride along
    // (the canon undersea pass) so the deepened bed reads THROUGH the surface.
    auto applyRiverWater = [&](float t) {
        if (!(riverOn && riverRoad.plan.ok)) return;
        x3::rhi::IRenderDevice::WaterParams wpr{};
        wpr.enabled   = true;
        wpr.seaLevel  = riverRoad.plan.waterY;
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
        cp.enabled = true; cp.waterY = riverRoad.plan.waterY;
        cp.time = t; cp.intensity = 0.85f;
        device->setCaustics(cp);
    };
    float riverWaterClock = 0.0f;

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

        auto settleAndGrab = [&](const float cam[5], const std::string& out) -> bool {
            // The streamer only enqueues the full ring on a focus-tile crossing
            // (host_cliffs.cpp's trick): nudge the focus on frame 1, then hold.
            const int kFrames = 200;
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
                applyRiverWater(riverWaterClock);
                riverLife.prePhysics(dt);
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
                    trees.draw(*device, frame);
                    if (carBuilt) car.render(frame);
                    riverLife.render(*device, frame, scene);
                    if (shotDraw) shotDraw(frame);   // the staged swimmer
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

            // Jake's rig, loaded exactly the way the interactive exit does.
            jakeTried = true;
            jakeSrc.reset(x3::asset::createAssetSource());
            if (jakeSrc && jakeSrc->mountDir(x3::game::assetRoot() + "/rigged_glb", 0)) {
                jakeLoader.reset(x3::asset::createModelLoader(device, jakeSrc.get()));
                jakeModel = jakeLoader->load("Jake_44_actions.glb");
                if (jakeModel.ok) {
                    jakeDraw = x3::asset::makeDrawables(jakeModel);
                    if (jakeSkin.bind(jakeModel)) {
                        jakeSkin.setRootYLock(true);
                        jakeSkin.enableGpuSkinning(*device, jakeModel);
                        int idle = jakeSkin.findClip({ "idle", "stand", "breath" });
                        const int walk = jakeSkin.findClip({ "walking", "walk" });
                        const int run  = jakeSkin.findClip({ "run", "sprint", "jog" });
                        if (idle < 0) idle = 0;
                        jakeSkin.setLocomotionClips(idle, walk, run, 0.2f, 2.0f);
                        jakeAnimated = true;
                        x3::logInfo("[swim-shot] jake bound: rootYLockRestY=" +
                                    std::to_string(jakeSkin.rootYLockRestY()) +
                                    " idle=" + std::to_string(idle));
                    }
                }
            }
            onFoot.spawn(*phys, sx, wy + 0.6f, sz);   // drops in, swim state takes him
            bool diveHeld = false;
            shotTick = [&](float d) {
                x3::game::PlayerInput pin;            // no move — tread / sink only
                pin.diveHeld = diveHeld;
                onFoot.update(pin, d, *phys);
                if (jakeAnimated) {
                    jakeSkin.setLocomotionSpeed(0.0f);
                    jakeSkin.applyLocomotion(jakeModel, *device, d);
                }
            };
            shotDraw = [&](const x3::rhi::FrameContext& fr) {
                if (jakeDraw.empty()) return;
                const x3::phys::Vec3 ft = onFoot.feet();
                static int drawN = 0;
                if ((drawN++ % 200) == 0) {
                    char db[128];
                    std::snprintf(db, sizeof(db), "[swim-shot] draw #%d at (%.1f, %.2f, %.1f)",
                                  drawN - 1, ft.x, ft.y, ft.z);
                    x3::logInfo(db);
                }
                const float a  = std::atan2(rdz, rdx) + 1.5707963f;  // face downstream
                // ARMATURE-OFFSET COMPENSATION — |restY|, calibrated against
                // the staged swimmer (the one place this offset was ever
                // MEASURED against a known capsule): the current
                // Jake_44_actions export measures restY = +1.142 and renders
                // 1.2 m LOW at yFix 0 (he vanished under the riverbed), while
                // the old export measured -0.9488 and needed +0.9488. Both
                // pathologies — and the clean-skeleton ~0 case — resolve to
                // yFix = |restY|.
                const float restY = jakeAnimated ? jakeSkin.rootYLockRestY() : 0.0f;
                const float yFix  = std::fabs(restY);
                const float ca = std::cos(a), sa = std::sin(a);
                const float world[16] = {
                     ca, 0.0f, -sa, 0.0f,
                   0.0f, 1.0f, 0.0f, 0.0f,
                     sa, 0.0f,  ca, 0.0f,
                   ft.x, ft.y + yFix, ft.z, 1.0f
                };
                for (const x3::asset::ModelDrawable& d : jakeDraw) {
                    const float bc[4] = { d.baseColorFactor[0], d.baseColorFactor[1],
                                          d.baseColorFactor[2], d.baseColorFactor[3] };
                    const float emis[3] = { d.emissiveFactor[0], d.emissiveFactor[1],
                                            d.emissiveFactor[2] };
                    device->drawMeshPBR(fr,
                        x3::rhi::MeshHandle{ d.meshId },
                        x3::rhi::TextureHandle{ d.baseColorTexId },
                        x3::rhi::TextureHandle{ d.normalTexId },
                        x3::rhi::TextureHandle{ d.mrTexId },
                        bc, emis, world, d.alphaMask, d.alphaBlend,
                        x3::rhi::TextureHandle{ d.emissiveTexId },
                        x3::rhi::TextureHandle{ d.detailTexId }, d.detailUvScale,
                        d.clearcoat, d.clearcoatRough);
                }
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
        trees.shutdown(*device);
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
                "M map, ~ console, ESC menu, SHIFT+ESC quits");

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
    // ESC FIRST-REFUSAL: close the map's confirm prompt, then the map itself,
    // and only then let the shell open its pause menu (host_streamed's layering).
    shell.setEscapeHandler([&]() -> bool {
        if (mapOpen && wmap.confirmOpen()) { mapEsc = true; return true; }
        if (mapOpen) {
            mapOpen = false; wmap.close();
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
            glfwGetCursorPos(window, &lastMX, &lastMY);
            return true;
        }
        return false;                        // nothing open: shell menu
    });
    if (auto* con = shell.console()) {
        // ---- weather (moved from the old host-local console) ----
        con->registerCVar("wx", "off",
            "weather: off | clear | cloudy | rain | storm | fog | snow");
        con->registerCVar("wx_snow_in", "0",
            "lying snow depth to prime, INCHES (applied when wx changes)");
        con->registerCVar("wx_hour", "14",
            "time of day, 0-24, drives the diurnal temperature swing");
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
                "driving=%d spawned=%d | rigRestY=%.4f jake_y=%.2f",
                jf.x, jf.y, jf.z, gy, rh.hit ? "" : "(miss) ",
                rh.hit ? rh.point.y : 0.0f, (int)driving, (int)footSpawned,
                (double)jakeSkin.rootYLockRestY(), (double)console->getFloat("jake_y"));
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
        con->registerCommand("rain", [con](const std::vector<std::string>& args) {
            if (args.empty()) { con->print("rain 0-10: 0 off | 1-3 sprinkle | 4-6 downpour | 7-8 heavy | 9-10 MONSOON"); return; }
            const float v = std::min(10.0f, std::max(0.0f, (float)std::atof(args[0].c_str())));
            if (v < 0.5f)      { con->set("wx", "off"); con->print("rain: off"); return; }
            con->set("wx", v >= 8.5f ? "storm" : "rain");
            char mb[32]; std::snprintf(mb, sizeof(mb), "%.2f", 0.4f + v * 0.42f);
            con->set("wx_precip_mult", mb);
            con->print(std::string("rain ") + args[0] + (v >= 8.5f ? "  (MONSOON - storm cell, lightning live)"
                        : v >= 6.5f ? "  (heavy)" : v >= 3.5f ? "  (downpour)" : "  (sprinkle)"));
        }, "rain 0-10 — Sprinkle to Downpour to MONSOON, with every in-between");
        con->registerCVar("wx_precip_mult", "2.4",
            "precipitation density multiplier (raises how much of the particle pool a given rain/snow state uses)");
        con->registerCVar("jake_yaw", "0", "Jake facing trim, degrees (rig-forward correction)");
        con->registerCVar("jake_y",   "0", "Jake height trim, metres (on top of the measured armature offset)");
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
        shell.addFloatCommand("car_final", "final-drive ratio (stock 4.6; 4.2 trades punch for ~168 mph top)",
            [&](float v) { x3::phys::WheeledTuning t; t.finalDrive = v; car.applyTuning(t); });
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
        con->registerCommand("car_reset", [&](const std::vector<std::string>&) {
            car.setTorqueBoost(1.0f);
            // These ARE the buildPhysics numbers in vehicle.cpp (NO_SLOP rule
            // 4: paired — change both). The old reset was a time capsule from
            // two retunes ago: 2400 Nm (the shipped car is 800 + turbo) and
            // gripScale 5.2 under the broken compose-on-top semantics.
            x3::phys::WheeledTuning t;
            t.maxEngineTorque = 800.0f; t.maxEngineRPM = 7500.0f;
            t.gripScale = 1.0f; t.latGripScale = 1.0f; t.latTail = 1.0f;
            t.massKg = 1083.2f; t.finalDrive = 4.6f;
            t.antiRollFront = 8000.0f; t.antiRollRear = 6000.0f;
            car.applyTuning(t);
            car.steerParams() = x3::game::DriveDemo::SteerParams{};
            car.turbo() = x3::game::DriveDemo::TurboParams{};
            con->print("car back to the shipped 992 Turbo S numbers (800 Nm, stock grip, 4.6 final)");
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
        mapEsc = false;   // BEFORE the poll: the escape handler runs inside it
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
                riverLife.render(*device, pf, scene);   // boats stay visible paused
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
        // ---- THE RIVER HAS WATER (Tim: "Can we pour the water in now").
        // One lambda with the headless path — same tone, same clock shape.
        riverWaterClock += fdt;
        applyRiverWater(riverWaterClock);
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
            // CLOUDS COST LIGHT (Tim: "Do we have real clouds that obscure
            // and dim the sun? The ground is way too sunny"). The deck was
            // visual-only — full sun through 94% overcast. Sun intensity now
            // falls with cover (an overcast day keeps ~35% direct light) and
            // the light goes flat (ambient-heavy) the way an overcast sky
            // actually lights the ground.
            sp.sunIntensity = sp.sunIntensity * (1.0f - 0.65f * std::min(1.0f, sp.cloud));
            if (ws.state == x3::game::WeatherState::Storm) {
                // A storm is not 'cloudy with effects' — the deck goes heavy
                // and the light DIES, which is also what makes every lightning
                // flash read (contrast is the flash's whole currency).
                sp.cloud    = std::max(sp.cloud, 0.94f);
                sp.exposure = ws.sky.exposure * 0.52f + storm.flash() * 1.35f;
            }
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
            precipAmt  = std::min(1.0f, ws.precipitation * console->getFloat("wx_precip_mult"));

        }
        double mx, my; glfwGetCursorPos(window, &mx, &my);
        // Gate the LOOK, not just the camera apply: the deltas also feed the
        // on-foot Player below, and the cursor is released while typing — an
        // ungated delta would spin Jake's view across the screen on the way to
        // the scrollback. Same rule while the MAP owns the cursor.
        const float look = (shell.inputEnabled() && !mapOpen) ? 1.0f : 0.0f;
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
            if (mNow && !prevMapM) {
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
                        jakeSrc.reset(x3::asset::createAssetSource());
                        const std::string glbDir = x3::game::assetRoot() + "/rigged_glb";
                        if (jakeSrc && jakeSrc->mountDir(glbDir, 0)) {
                            jakeLoader.reset(x3::asset::createModelLoader(device, jakeSrc.get()));
                            // Jake_44_actions, NOT JakeClone_player. The clone is
                            // 1.4 MB but has ZERO textures (the white statue) and
                            // only combat clips; the 44-action rig carries the
                            // baseColor texture and Idle / walking / run.
                            jakeModel = jakeLoader->load("Jake_44_actions.glb");
                            if (jakeModel.ok) {
                                jakeDraw = x3::asset::makeDrawables(jakeModel);
                                // EXACT clip lookup — the fuzzy matcher chose
                                // 'Leftstrafewalking' for walk and
                                // 'BackRight_Run' for run (clip order beat
                                // intent): Jake strafed when walking. Exact
                                // names from the rig's own 44-clip list.
                                auto exactClip = [&](const char* nm) -> int {
                                    for (uint32_t ci2 = 0; ci2 < jakeSkin.clipCount(); ++ci2)
                                        if (jakeSkin.clipName(ci2) == std::string_view(nm)) return (int)ci2;
                                    return -1;
                                };
                                if (jakeSkin.bind(jakeModel)) {
                                    // ROOT-Y LOCK: the Jake clips are the family
                                    // with the -0.9488 armature-offset root Y
                                    // (anim.h setRootYLock documents exactly this
                                    // rig); his world Y is owned by the capsule.
                                    jakeSkin.setRootYLock(true);
                                    jakeSkin.enableGpuSkinning(*device, jakeModel);
                                    int idle = exactClip("Idle");
                                    int walk = exactClip("Walking");
                                    int run  = exactClip("Running");
                                    // 'Jump' preferred over 'Regular_Jump':
                                    // Regular_Jump carries ROOT MOTION — the
                                    // mesh lunges relative to its capsule and
                                    // travels INTO the chase camera (Tim:
                                    // "jumping switches camera to INSIDE
                                    // JAKE"). W-JAKE's measured-motion table
                                    // will pick the truly in-place one.
                                    jakeJumpClip = exactClip("Jump");
                                    if (jakeJumpClip < 0) jakeJumpClip = exactClip("Regular_Jump");
                                    // Unarmed strikes ("Punch and kick do not
                                    // work" — they were never wired): this
                                    // rig's unarmed kit is the flashy pair.
                                    jakePunchClip = exactClip("Backflip_and_Hooks");
                                    jakeKickClip  = exactClip("Backflip_Sweep_Kick");
                                    if (idle < 0) idle = jakeSkin.findClip({ "idle" });
                                    if (idle < 0) idle = 0;
                                    jakeSkin.setLocomotionClips(idle, walk, run, 0.2f, 2.0f);
                                    jakeSkin.setLocomotionSpeed(0.0f);
                                    jakeSkin.applyLocomotion(jakeModel, *device, 0.0f);
                                    jakeAnimated = true;
                                    x3::logInfo("[tunnel] Jake animated: idle=" + std::to_string(idle)
                                                + " walk=" + std::to_string(walk)
                                                + " run=" + std::to_string(run));
                                }
                                char jb[128];
                                std::snprintf(jb, sizeof(jb), "[tunnel] Jake: %u drawable(s)%s",
                                              (uint32_t)jakeDraw.size(),
                                              jakeAnimated ? " (rigged)" : " (STATIC - no skin)");
                                x3::logInfo(jb);
                            } else {
                                x3::logWarn("[tunnel] Jake_44_actions.glb failed to load - no body on foot");
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
            // W10 swim channels: Space held strokes UP (jumpHeld above), Ctrl/C
            // held dives. Only read while the swim state is active, so dry-land
            // movement is untouched.
            pin.diveHeld = kd(GLFW_KEY_LEFT_CONTROL) || kd(GLFW_KEY_C);
            pin.lookDX = ddx; pin.lookDY = ddy;
            onFoot.update(pin, fdt, *phys);
            {   // FEET-ABOVE-GROUND INVARIANT v2. v1 clamped to the TERRAIN
                // height field — but Jake walks on ROADS, which ride ABOVE the
                // field on embankments and decks. Fall through pavement into
                // the gap beneath and v1 was satisfied (feet above dirt) while
                // he was entombed under the road ("Jake is STILL underground").
                // v2 clamps to the TOPMOST WALKABLE SURFACE: max of the height
                // field and a downward raycast from above his head — the same
                // painted-road rule the car spawn learned.
                const x3::phys::Vec3 jf = onFoot.feet();
                float gy = x3::game::terrainHeightAtWorld(jf.x, jf.z);
                const x3::phys::RayHit jrh = phys->rayCast(
                    x3::phys::Vec3{ jf.x, jf.y + 40.0f, jf.z },
                    x3::phys::Vec3{ 0.0f, -1.0f, 0.0f }, 120.0f,
                    x3::phys::Layer::Static);
                if (jrh.hit) gy = std::max(gy, jrh.point.y);
                if (jf.y < gy - 0.25f) {
                    onFoot.setFeetPosition(*phys, x3::phys::Vec3{ jf.x, gy + 0.15f, jf.z });
                    x3::logInfo("[tunnel] CONTACT LAW: Jake lifted onto the surface "
                                "(was below by more than 0.25 m)");
                }
            }

            // Drive the rig from what the capsule actually DID: planar speed
            // picks idle/walk/run (the locomotion blend), and he faces his
            // direction of travel — not the camera — turning smoothly.
            if (jakeAnimated) {
                const x3::phys::Vec3 ft = onFoot.feet();
                const float vx = (ft.x - jakePrevFeet[0]) / std::max(fdt, 1e-4f);
                const float vz = (ft.z - jakePrevFeet[2]) / std::max(fdt, 1e-4f);
                jakePrevFeet[0] = ft.x; jakePrevFeet[1] = ft.y; jakePrevFeet[2] = ft.z;
                const float planar = std::sqrt(vx * vx + vz * vz);
                if (planar > 0.4f) {
                    // THE BABYLON FLIP (Tim: "this is the X3 Babylon thing we
                    // nEVER fixed"): the rig is authored facing +Z, the engine
                    // walks -Z — 180 degrees from the old clone-rig constant.
                    // Baked here; jake_yaw (degrees) remains the live dial —
                    // if any rig still disagrees, dial it, report the number,
                    // and THAT gets baked next.
                    const float want = std::atan2(vz, vx) - 1.5707963f;
                    float d = want - jakeYaw;
                    while (d >  3.14159265f) d -= 6.2831853f;
                    while (d < -3.14159265f) d += 6.2831853f;
                    jakeYaw += d * std::min(1.0f, fdt * 10.0f);
                }
                // COMBAT ONE-SHOTS: LMB = hooks combo, RMB = sweep kick.
                // Animation-only for now; damage lands with the campaign
                // melee system (task 20). Gated on shell input so clicking
                // the console does not throw punches.
                if (shell.inputEnabled() && jakeActT < 0.0f && jakeJumpT < 0.0f) {
                    static bool lmbWas = false, rmbWas = false;
                    const bool lmb = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
                    const bool rmb = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
                    if (lmb && !lmbWas && jakePunchClip >= 0) { jakeActClip = jakePunchClip; jakeActT = 0.0f; }
                    else if (rmb && !rmbWas && jakeKickClip >= 0) { jakeActClip = jakeKickClip; jakeActT = 0.0f; }
                    lmbWas = lmb; rmbWas = rmb;
                }
                // JUMP ONE-SHOT: overrides locomotion for its own duration.
                if (pin.jumpPressed && jakeJumpClip >= 0 && jakeJumpT < 0.0f && jakeActT < 0.0f)
                    jakeJumpT = 0.0f;
                if (jakeActT >= 0.0f && jakeActClip >= 0) {
                    jakeActT += fdt;
                    jakeSkin.apply(jakeModel, *device, (uint32_t)jakeActClip, jakeActT);
                    if (jakeActT >= jakeSkin.clipDuration((uint32_t)jakeActClip))
                        jakeActT = -1.0f;
                } else if (jakeJumpT >= 0.0f && jakeJumpClip >= 0) {
                    jakeJumpT += fdt;
                    jakeSkin.apply(jakeModel, *device, (uint32_t)jakeJumpClip, jakeJumpT);
                    if (jakeJumpT >= jakeSkin.clipDuration((uint32_t)jakeJumpClip))
                        jakeJumpT = -1.0f;
                } else {
                    jakeSkin.setLocomotionSpeed(planar);
                    jakeSkin.applyLocomotion(jakeModel, *device, fdt);
                }
            }
        }

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
          x3::game::uploadTunnelLights(*device, cp); }
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
            if (weatherOn)
                precip.update(fdt, precipKind, precipAmt, cx, cy, cz, 0.0f, 0.0f, skyVis);
            // Under open sky: short, nearly-dry (t60 0.3 s, wet 0.05). Deep in
            // the bore: a long concrete tail (t60 2.5 s, wet 0.45). Both are
            // smoothed on the audio thread, so driving through the portal is a
            // swell, not a step. Loop voices (the engine bank) and 3D one-shots
            // all ride the same insert.
            if (audioOn)
                audio->setReverbParams(0.3f + 2.2f * (1.0f - skyVis),
                                       0.05f + 0.40f * (1.0f - skyVis));
        }
        auto frame = device->beginFrame();
        if (frame.valid) {
            scene.render(*device, frame);
            trees.draw(*device, frame);
            if (carBuilt) car.render(frame);
            riverLife.render(*device, frame, scene);   // boats + drivers + wakes
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
                    prompt = (dxc*dxc + dzc*dzc <= 16.0f)
                                 ? (pushing ? "PUSHING..." : "E  GET IN    F  PUSH")
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
            // The rig is authored +Z forward; jakeYaw tracks his direction of
            // TRAVEL (smoothed in the movement block) — a man who faces his
            // camera instead of his path moonwalks every time you strafe.
            if (!driving && footSpawned && !jakeDraw.empty()) {
                const x3::phys::Vec3 ft = onFoot.feet();
                const float a = (jakeAnimated ? jakeYaw : camYaw + 1.5707963f)
                              + console->getFloat("jake_yaw") * 0.0174533f;
                // THE HARD RULE (Tim: "HARD RULES for where FEET and TIRES can
                // and can NOT BE"): the rig's rest origin goes AT the capsule's
                // feet, compensated by the skeleton's own MEASURED armature
                // offset — |restY|, W-RIVER's calibration: the CURRENT
                // Jake_44_actions export measures restY = +1.142 and renders
                // 1.2 m LOW at zero compensation (measured against the swim
                // capsule — the staged swimmer vanished under the riverbed);
                // the OLD export measured -0.9488 and needed +0.9488. Both
                // pathologies — and the clean-skeleton ~0 case — resolve to
                // |restY|. jake_y trims live.
                const float jRest = jakeAnimated ? jakeSkin.rootYLockRestY() : 0.0f;
                const float yFix = std::fabs(jRest)
                                 + console->getFloat("jake_y");
                const float ca = std::cos(a), sa = std::sin(a);
                // Column-major 4x4: rotation about +Y, translation at the feet.
                const float world[16] = {
                     ca, 0.0f, -sa, 0.0f,
                   0.0f, 1.0f, 0.0f, 0.0f,
                     sa, 0.0f,  ca, 0.0f,
                   ft.x, ft.y + yFix, ft.z, 1.0f
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

        shell.draw(frame, fdt);      // console + FPS/stats, over everything
        device->endFrame(frame);
    }

    if (audioOn) engineNote.shutdown();          // bank voices before the mixer dies
    riverLife.shutdown(audioOn ? audio.get() : nullptr);   // outboard loops + hulls
    wmap.shutdown(*device);                      // no tiles baked here, but symmetric
    trees.shutdown(*device);
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
