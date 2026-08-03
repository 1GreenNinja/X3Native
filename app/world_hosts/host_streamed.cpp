// --world streamed host — lifted VERBATIM from main() (#28 deep split).
#include "world_host_common.h"
#include "../scene.h"
#include "../player.h"
#include "../terrain.h"
#include "../world_stream.h"
#include "../world_regions.h"
#include "../world_map.h"
#include "../save.h"
#include "../asset_root.h"
#include "../input_globals.h"   // g_weaponScroll + scrollCallback (shared globals)

namespace x3 { namespace apphost {

int hostStreamed(HostContext& hc) {
    auto* device = hc.device;
    GLFWwindow* window = hc.window;
    const std::string& worldMode = hc.worldMode;
    const bool headless = hc.headless;
    const bool screenshot = hc.screenshot;
    const std::string& screenshotPath = hc.screenshotPath;
    const uint32_t W = hc.W;
    const uint32_t H = hc.H;
    const bool shotWorldMap = hc.shotWorldMap;
    const float wsBudgetMs = hc.wsBudgetMs;
    const float wsLookaheadS = hc.wsLookaheadS;

    // ==== VERBATIM host body ====
    if (worldMode == "streamed") {
        x3::logInfo("--world streamed: booting the seamless region-graph world");

        std::unique_ptr<x3::phys::IPhysicsWorld> wphys(x3::phys::createPhysicsWorld());
        if (!wphys->init()) {
            x3::logError("--world streamed: physics init failed");
            device->shutdown();
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return 1;
        }
        std::unique_ptr<x3::jobs::IJobSystem> wjobs(x3::jobs::createJobSystem());
        wjobs->init(0);
        x3::game::Scene wscene;
        const x3::game::TerrainConfig& wcfg = x3::game::worldTerrainConfig();
        {
            x3::rhi::IRenderDevice::SkyParams sp{};
            sp.enabled = true;
            sp.sunDir[0] = 0.4f; sp.sunDir[1] = 1.0f; sp.sunDir[2] = 0.3f;
            sp.sunColor[0] = 1.0f; sp.sunColor[1] = 0.97f; sp.sunColor[2] = 0.92f;
            sp.sunIntensity = 1.0f; sp.haze = 0.5f; sp.exposure = 1.0f;
            device->setSkyParams(sp);
        }
        // CONTENT WIRING: cascades for the outdoor view depth. `r_csm` was only
        // ever pushed to the device from runDefaultHost, which a --world host
        // REPLACES -- so this world had cascaded shadows compiled in and
        // unreachable, and everything past the legacy 45 m box cast nothing.
        // `--set r_csm 0` restores that legacy single cascade exactly.
        // The widest view depth in the game (15 km far plane, streamed city +
        // ranges), so the cascades get the longest distance of any world.
        applyOutdoorCsm(hc, *device, 600.0f, "streamed");

        // The region graph (data) + the residency manager.
        x3::game::WorldRegionGraph wgraph;
        {
            std::vector<std::string> errs;
            if (!wgraph.load(x3::game::worldRegionsJsonPath(), errs) || wgraph.empty()) {
                for (const std::string& e : errs) x3::logError("--world streamed: " + e);
                x3::logError("--world streamed: region graph failed to load — aborting");
                wjobs->shutdown(); wphys->shutdown(); device->shutdown();
                if (window) glfwDestroyWindow(window);
                glfwTerminate();
                return 1;
            }
        }
        x3::game::WorldStreamer wsm;
        wsm.init(wgraph, wjobs.get());
        wsm.setLookahead(wsLookaheadS);
        x3::logInfo("--world streamed: region graph `" + x3::game::worldRegionsJsonPath() +
                    "` (" + std::to_string(wgraph.regions.size()) + " regions), budget " +
                    std::to_string(wsBudgetMs) + " ms/frame, lookahead " +
                    std::to_string(wsLookaheadS) + " s");

        x3::game::TerrainStreamer wstream;

        // ===== WORLD-MAP SHOT SEQUENCE (--screenshot-worldmap, headless): boot
        // the streamed world, pre-discover the POI table (the shots show the
        // explored map), bake the region tiles from the LIVE ledgers + the Spire
        // floors from the LevelDoc, then capture the map screen at world-overview /
        // region / room-detail zooms + the floor selector + a waypoint + the
        // fast-travel confirm. =====
        if (headless && shotWorldMap) {
            namespace fs = std::filesystem;
            const fs::path outDir = "docs/screenshots/worldmap";
            std::error_code ec; fs::create_directories(outDir, ec);

            x3::game::WorldMapSystem wmap;
            wmap.init(x3::game::worldMapPoisJsonPath(), x3::game::canonProjectJsonPath());
            x3::game::StoryFlags wflags;
            for (const x3::game::MapPoi& pp : wmap.pois().pois) {
                wflags.set(x3::game::poiFoundFlag(pp.id));
                if (!pp.region.empty()) wflags.set(x3::game::regionSeenFlag(pp.region));
            }

            const float sax = wgraph.regions[0].anchor[0], saz = wgraph.regions[0].anchor[2];
            wstream.init(wscene, *device, *wphys, wjobs.get(), wcfg, sax, saz, /*radius=*/8);
            wstream.setUploadBudget(64);
            wsm.buildStartRegions(wscene, *device, *wphys, sax, 0.0f, saz);
            const float dt = 1.0f / 60.0f;
            for (int i = 0; i < 240; ++i) {     // settle: terrain ring + neighbors land
                glfwPollEvents();
                wstream.update(wscene, *device, *wphys, sax, saz);
                wsm.update(wscene, *device, *wphys, sax, 0.0f, saz, 0.0f, 0.0f, 0.0f, 50.0, 0.0);
                wphys->step(dt);
                wscene.update(*wphys);
                device->setCamera(sax, 60.0f, saz, 0.0f, -0.5f, 60.0f);
                auto f = device->beginFrame();
                if (f.valid) wscene.render(*device, f);
                device->endFrame(f);
            }
            // Bake builder-region tiles from the live ownership ledgers.
            for (uint32_t ri = 0; ri < wsm.regionCount(); ++ri) {
                const x3::game::WorldRegionDesc& rd = wsm.desc(ri);
                if (!rd.levelDoc.empty()) continue;   // the Spire bakes from its LevelDoc
                if (wsm.state(ri) != x3::game::RegionState::Resident) continue;
                const float rr = std::min(rd.radius, 1200.0f);
                wmap.ensureRegionTile(*device, wscene, rd.id, wsm.ownedEntities(ri),
                                      rd.anchor[0] - rr, rd.anchor[2] - rr,
                                      rd.anchor[0] + rr, rd.anchor[2] + rr,
                                      rd.anchor[1] - 80.0f, rd.anchor[1] + 90.0f);
            }
            x3::ui::UiContext mui;
            const int fbw = (int)W, fbh = (int)H;
            auto mapShot = [&](const char* png, float mcx, float mcz, float mscale, int floor,
                               bool wp, float wpx, float wpz, const char* confirmPoi) -> bool {
                wmap.open(sax, 1.7f, saz, (float)fbw, (float)fbh);
                wmap.camera().jumpTo(mcx, mcz, mscale);
                if (floor > 0) wmap.selectFloor(floor);
                if (wp) wmap.setWaypoint(wpx, wpz, floor); else wmap.clearWaypoint();
                if (confirmPoi) wmap.openConfirm(wmap.pois().indexOf(confirmPoi));
                const std::string path = (outDir / png).string();
                for (int i = 0; i < 3; ++i) {   // a couple frames so tile uploads land
                    glfwPollEvents();
                    device->setCamera(sax, 60.0f, saz, 0.0f, -0.5f, 60.0f);
                    if (i == 2) device->armCapture(path.c_str());
                    auto f = device->beginFrame();
                    if (f.valid) {
                        wscene.render(*device, f);
                        x3::ui::UiInput ui0{};
                        ui0.mouseX = fbw * 0.5f; ui0.mouseY = fbh * 0.5f;
                        mui.begin(*device, f, ui0);
                        x3::game::WorldMapSystem::ScreenInput msi{};
                        msi.mouseX = ui0.mouseX; msi.mouseY = ui0.mouseY;
                        msi.playerX = sax; msi.playerY = 0.0f; msi.playerZ = saz;
                        msi.playerYaw = 0.8f;
                        msi.locationName = "KETH'ZAR - SEAMLESS WORLD";
                        wmap.drawScreen(mui, *device, f, msi, wflags, 0.0f);
                        mui.end();
                    }
                    device->endFrame(f);
                }
                const bool ok = device->captureFrame(path.c_str());
                if (ok) x3::logInfo("--screenshot-worldmap: wrote " + path);
                else    x3::logError("--screenshot-worldmap: capture FAILED: " + path);
                wmap.closeConfirm();
                return ok;
            };
            bool all = true;
            all &= mapShot("01_world_overview.png",          110.0f, -460.0f, 0.32f,  0, false, 0, 0, nullptr);
            all &= mapShot("02_region_city.png",            -200.0f,  425.0f, 0.85f,  0, false, 0, 0, nullptr);
            all &= mapShot("03_room_detail_spire_f1.png",     22.0f,   10.0f, 11.0f,  1, false, 0, 0, nullptr);
            all &= mapShot("04_spire_floor_selector_f3.png",  22.0f,   10.0f, 8.0f,   3, false, 0, 0, nullptr);
            all &= mapShot("05_waypoint_set.png",             22.0f,   18.0f, 6.0f,   1, true, 22.0f, 14.0f, nullptr);
            all &= mapShot("06_fasttravel_confirm.png",       22.0f,   18.0f, 6.0f,   1, false, 0, 0, "armory");
            wmap.shutdown(*device);
            wsm.shutdown(wscene, *device, *wphys);
            wstream.shutdown(wscene, *device, *wphys);
            wjobs->shutdown();
            wphys->shutdown();
            device->shutdown();
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return all ? 0 : 1;
        }

        // ===== Headless traversal-shot sequence (region boundary before/after). =
        if (headless) {
            namespace fs = std::filesystem;
            const fs::path outDir = "docs/screenshots/streaming";
            std::error_code ec; fs::create_directories(outDir, ec);
            const float dt = 1.0f / 60.0f;
            // Approach the CITY from the west at vehicle speed: at leg A the city
            // is beyond its unload radius (NOT resident — bare terrain + the
            // mountain backdrop); by leg B the load radius has tripped and the
            // city has streamed in; leg C ends inside Scrapyard City.
            struct ShotLeg { float x, z, camH, pitch; int settle; const char* png; };
            const ShotLeg legs[3] = {
                { -1500.0f, 500.0f, 70.0f, -0.35f, 120, "01_city_not_resident.png" },
                {  -820.0f, 500.0f, 70.0f, -0.35f, 150, "02_city_streamed_in.png"  },
                {  -620.0f, 500.0f, 32.0f, -0.20f, 150, "03_inside_city.png"       },
            };
            wstream.init(wscene, *device, *wphys, wjobs.get(), wcfg,
                         legs[0].x, legs[0].z, /*radius=*/8);
            wstream.setUploadBudget(64);   // fill the visible ring fast for stills
            // W8-3: horizon ring + long far plane so the mountain backdrop the
            // leg-A caption promises actually draws behind the bare terrain.
            {
                x3::game::HorizonRingDesc hr{};
                hr.centerX = -600.0f; hr.centerZ = 500.0f;   // the drive corridor
                hr.rInner = 240.0f; hr.rOuter = 13000.0f;
                hr.rings = 140; hr.segments = 160; hr.yBias = -3.0f;
                x3::game::addTerrainHorizonRing(wscene, *device, wstream.groundTexture(), hr);
                device->setCameraFar(15000.0f);
            }
            wsm.buildStartRegions(wscene, *device, *wphys, legs[0].x, 0.0f, legs[0].z);

            float cx = legs[0].x, cz = legs[0].z;
            float camH = legs[0].camH, camPit = legs[0].pitch;
            const float kDriveSpeed = 40.0f;   // m/s — the vehicle-traversal case
            auto tickFrame = [&](float vx, float vz, const char* arm) {
                glfwPollEvents();
                const double t0 = glfwGetTime();
                wstream.update(wscene, *device, *wphys, cx, cz);
                const double terrainMs = (glfwGetTime() - t0) * 1000.0;
                wsm.update(wscene, *device, *wphys, cx, 0.0f, cz, vx, 0.0f, vz,
                           /*budget*/ 24.0, terrainMs);
                wphys->step(dt);
                wscene.update(*wphys);
                float ground[3]; x3::game::placeOnTerrain(cx, cz, ground);
                device->setCamera(cx, ground[1] + camH, cz, 0.0f, camPit, 60.0f);
                if (arm) device->armCapture(arm);
                auto frame = device->beginFrame();
                if (frame.valid) wscene.render(*device, frame);
                device->endFrame(frame);
            };
            bool allWrote = true;
            // Trigger the FULL terrain residency ring at the start point: the
            // streamer enqueues stream-in on tile-boundary CROSSINGS, so a static
            // focus only has its synchronous 3x3 (the valley still uses the same
            // one-frame nudge).
            cx += wcfg.tileSize; tickFrame(0.0f, 0.0f, nullptr);
            cx -= wcfg.tileSize; tickFrame(0.0f, 0.0f, nullptr);
            for (const ShotLeg& leg : legs) {
                camH = leg.camH; camPit = leg.pitch;
                // Drive to the leg point (region streaming runs the whole way).
                for (int guard = 0; guard < 20000; ++guard) {
                    const float dx = leg.x - cx, dz = leg.z - cz;
                    const float d = std::sqrt(dx * dx + dz * dz);
                    if (d < 0.5f) break;
                    const float ux = dx / d, uz = dz / d;
                    const float step = std::min(d, kDriveSpeed * dt);
                    cx += ux * step; cz += uz * step;
                    tickFrame(ux * kDriveSpeed, uz * kDriveSpeed, nullptr);
                }
                // Settle (let terrain + region uploads land), then capture.
                const std::string outPath = (outDir / leg.png).string();
                for (int i = 0; i < leg.settle; ++i)
                    tickFrame(0.0f, 0.0f, i == leg.settle - 1 ? outPath.c_str() : nullptr);
                const bool wrote = device->captureFrame(outPath.c_str());
                if (wrote) x3::logInfo("--world streamed: wrote " + outPath);
                else { x3::logError("--world streamed: capture FAILED: " + outPath); allWrote = false; }
            }
            wsm.shutdown(wscene, *device, *wphys);
            wstream.shutdown(wscene, *device, *wphys);
            wjobs->shutdown();
            wphys->shutdown();
            device->shutdown();
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
            return allWrote ? 0 : 1;
        }

        // ===== Walkable windowed path. Spawn ON the terrain surface at the first
        // region's anchor (the Spire); the regions the spawn is inside build
        // synchronously (boot), everything else streams as you move. =====
        const float sax = wgraph.regions[0].anchor[0], saz = wgraph.regions[0].anchor[2];
        float sgr[3]; x3::game::placeOnTerrain(sax, saz, sgr);
        wstream.init(wscene, *device, *wphys, wjobs.get(), wcfg, sax, saz, /*radius=*/8);
        wsm.buildStartRegions(wscene, *device, *wphys, sax, sgr[1], saz);
        // W8-3: the horizon stitch for the walkable world — one static ring from
        // the same field (recessed under the streamed tiles) + a long far plane,
        // so the 4 mountain ranges read from anywhere in the central map. Known
        // limit: ring cells coarsen ~3%/ring with radius; near the ranges
        // themselves (7 km+ from the spawn center) the local ring is coarse —
        // recentering/re-baking the ring on long travel is a follow-up.
        {
            x3::game::HorizonRingDesc hr{};
            hr.centerX = sax; hr.centerZ = saz;
            hr.rInner = 240.0f; hr.rOuter = 13000.0f;
            hr.rings = 140; hr.segments = 160; hr.yBias = -3.0f;
            x3::game::addTerrainHorizonRing(wscene, *device, wstream.groundTexture(), hr);
            device->setCameraFar(15000.0f);
        }

        x3::game::Player wplayer;
        wplayer.spawn(*wphys, sax, sgr[1] + 2.0f, saz);

        // ---- WORLD MAP (M) in the streamed world: POIs + discovery + waypoint +
        // FAST TRAVEL THROUGH THE STREAMER (teleport; wsm.update's proxy fallback
        // covers the realize window -- no loading screen, just the blackout fade).
        // Discovery flags persist to save/worldmap_streamed.flags.
        x3::game::WorldMapSystem wmap;
        wmap.init(x3::game::worldMapPoisJsonPath(), x3::game::canonProjectJsonPath());
        x3::game::StoryFlags wflags;
        { std::error_code fec; std::filesystem::create_directories("save", fec); }
        wflags.loadFile("save/worldmap_streamed.flags");
        x3::ui::UiContext wmapUi;
        bool wmapOpen = false;
        bool prevMW = false, prevEnterW = false, prevEscW = false, prevLmbW = false;
        float wTravelFade = 0.0f;
        glfwSetScrollCallback(window, scrollCallback);   // wheel -> map zoom
        g_weaponScroll = 0.0;

        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        double lastMX, lastMY; glfwGetCursorPos(window, &lastMX, &lastMY);
        double prevTime = glfwGetTime();
        bool prevSpaceW = false, prevFW = false, noclipW = false;
        float flyXw = sax, flyYw = sgr[1] + 1.6f, flyZw = saz, flyYawW = 0.0f, flyPitchW = -0.1f;
        float prevPX = sax, prevPZ = saz;
        x3::logInfo("--world streamed: WASD walk, mouse look, Space jump, LeftShift sprint, "
                    "F noclip, Esc to quit — regions stream around you (watch the log)");

        int lastWw = (int)W, lastHw = (int)H;
        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();
            // Esc: close the map first (back out of the confirm prompt, then the
            // map), only then quit the streamed world.
            const bool escNowW = glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS;
            const bool escEdgeW = escNowW && !prevEscW;
            prevEscW = escNowW;
            bool mapEscW = false;
            if (escEdgeW) {
                if (wmapOpen && wmap.confirmOpen()) mapEscW = true;
                else if (wmapOpen) {
                    wmapOpen = false; wmap.close();
                    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
                    glfwGetCursorPos(window, &lastMX, &lastMY);
                } else break;
            }

            double now = glfwGetTime();
            float dt = (float)(now - prevTime); prevTime = now;
            if (dt > 0.1f) dt = 0.1f;

            double mx, my; glfwGetCursorPos(window, &mx, &my);
            float ddx = (float)(mx - lastMX), ddy = (float)(my - lastMY);
            lastMX = mx; lastMY = my;
            if (wmapOpen) { ddx = 0.0f; ddy = 0.0f; }   // no look-swing under the cursor

            // Gameplay keys are captured by the map while it is open (the map does
            // its own raw W/A/S/D pan reads).
            auto kd = [&](int k) { return !wmapOpen && glfwGetKey(window, k) == GLFW_PRESS; };

            // M toggles the world map (cursor shown while open; sim input frozen).
            {
                const bool mNowW = glfwGetKey(window, GLFW_KEY_M) == GLFW_PRESS;
                if (mNowW && !prevMW) {
                    if (wmapOpen) { wmapOpen = false; wmap.close(); }
                    else {
                        float ppx, ppy, ppz, pyw, ppt;
                        wplayer.camera(ppx, ppy, ppz, pyw, ppt);
                        if (noclipW) { ppx = flyXw; ppy = flyYw; ppz = flyZw; }
                        int fbw = 0, fbh = 0; glfwGetFramebufferSize(window, &fbw, &fbh);
                        wmap.open(ppx, ppy - 1.6f, ppz, (float)fbw, (float)fbh);
                        wmapOpen = true;
                    }
                    glfwSetInputMode(window, GLFW_CURSOR,
                                     wmapOpen ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_DISABLED);
                    glfwGetCursorPos(window, &lastMX, &lastMY);
                }
                prevMW = mNowW;
            }

            bool spaceNow = kd(GLFW_KEY_SPACE);
            bool fNow = kd(GLFW_KEY_F);
            if (fNow && !prevFW) {
                noclipW = !noclipW;
                if (noclipW) { float yy, pp; wplayer.camera(flyXw, flyYw, flyZw, yy, pp); flyYawW = yy; flyPitchW = pp; }
            }
            prevFW = fNow;

            float camX, camY, camZ, camYaw, camPitch;
            if (!noclipW) {
                x3::game::PlayerInput in;
                if (kd(GLFW_KEY_W)) in.moveFwd    += 1.0f;
                if (kd(GLFW_KEY_S)) in.moveFwd    -= 1.0f;
                if (kd(GLFW_KEY_D)) in.moveStrafe += 1.0f;
                if (kd(GLFW_KEY_A)) in.moveStrafe -= 1.0f;
                in.sprint      = kd(GLFW_KEY_LEFT_SHIFT);
                in.jumpPressed = spaceNow && !prevSpaceW;
                in.lookDX = ddx; in.lookDY = ddy;
                wplayer.update(in, dt, *wphys);
                wplayer.camera(camX, camY, camZ, camYaw, camPitch);
            } else {
                const float sens = 0.0025f;
                flyYawW += ddx * sens; flyPitchW -= ddy * sens;
                if (flyPitchW >  1.55f) flyPitchW =  1.55f;
                if (flyPitchW < -1.55f) flyPitchW = -1.55f;
                float fxw = std::cos(flyPitchW) * std::cos(flyYawW);
                float fyw = std::sin(flyPitchW);
                float fzw = std::cos(flyPitchW) * std::sin(flyYawW);
                float rl = std::sqrt(fxw*fxw + fzw*fzw); if (rl < 1e-4f) rl = 1e-4f;
                float rx = -fzw/rl, rz = fxw/rl;
                float spd = 8.0f * dt; if (kd(GLFW_KEY_LEFT_SHIFT)) spd *= 6.0f;
                if (kd(GLFW_KEY_W)) { flyXw += fxw*spd; flyYw += fyw*spd; flyZw += fzw*spd; }
                if (kd(GLFW_KEY_S)) { flyXw -= fxw*spd; flyYw -= fyw*spd; flyZw -= fzw*spd; }
                if (kd(GLFW_KEY_D)) { flyXw += rx*spd; flyZw += rz*spd; }
                if (kd(GLFW_KEY_A)) { flyXw -= rx*spd; flyZw -= rz*spd; }
                if (spaceNow) flyYw += spd;
                if (kd(GLFW_KEY_LEFT_CONTROL)) flyYw -= spd;
                camX = flyXw; camY = flyYw; camZ = flyZw; camYaw = flyYawW; camPitch = flyPitchW;
            }
            prevSpaceW = spaceNow;

            // ---- ONE budget umbrella: terrain tiles first (measured), then the
            // region streamer gets whatever is left of --ws-budget this frame.
            // Player velocity feeds the lookahead so sprint/vehicle speeds pull
            // regions in earlier. ----
            const double t0s = glfwGetTime();
            wstream.update(wscene, *device, *wphys, camX, camZ);
            const double terrainMs = (glfwGetTime() - t0s) * 1000.0;
            const float velX = dt > 1e-4f ? (camX - prevPX) / dt : 0.0f;
            const float velZ = dt > 1e-4f ? (camZ - prevPZ) / dt : 0.0f;
            prevPX = camX; prevPZ = camZ;
            wsm.update(wscene, *device, *wphys, camX, camY, camZ, velX, 0.0f, velZ,
                       (double)wsBudgetMs, terrainMs);

            wphys->step(dt);
            wscene.update(*wphys);

            int cw, chw; glfwGetFramebufferSize(window, &cw, &chw);
            if (cw != lastWw || chw != lastHw) { lastWw = cw; lastHw = chw; if (cw>0&&chw>0) device->onResize((uint32_t)cw,(uint32_t)chw); }

            device->setCamera(camX, camY, camZ, camYaw, camPitch, 60.0f);
            auto frame = device->beginFrame();
            if (frame.valid) {
                wscene.render(*device, frame);

                // POI proximity discovery (persisted flags).
                wmap.discoveryTick(wflags, camX, camY - 1.6f, camZ);

                if (wmapOpen) {
                    // Bake-or-fetch resident builder-region tiles from the LIVE
                    // ownership ledgers (the map IS the world).
                    for (uint32_t ri = 0; ri < wsm.regionCount(); ++ri) {
                        const x3::game::WorldRegionDesc& rd = wsm.desc(ri);
                        if (!rd.levelDoc.empty()) continue;
                        if (wsm.state(ri) != x3::game::RegionState::Resident) continue;
                        if (wmap.regionTile(rd.id)) continue;
                        const float rr = std::min(rd.radius, 1200.0f);
                        wmap.ensureRegionTile(*device, wscene, rd.id, wsm.ownedEntities(ri),
                                              rd.anchor[0] - rr, rd.anchor[2] - rr,
                                              rd.anchor[0] + rr, rd.anchor[2] + rr,
                                              rd.anchor[1] - 80.0f, rd.anchor[1] + 90.0f);
                    }
                    double cmx = 0.0, cmy = 0.0; glfwGetCursorPos(window, &cmx, &cmy);
                    const bool lmbW = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
                    x3::ui::UiInput ui0{};
                    ui0.mouseX = (float)cmx; ui0.mouseY = (float)cmy;
                    ui0.mouseDown = lmbW; ui0.mousePressed = lmbW && !prevLmbW;
                    wmapUi.begin(*device, frame, ui0);
                    x3::game::WorldMapSystem::ScreenInput msi{};
                    msi.mouseX = ui0.mouseX; msi.mouseY = ui0.mouseY;
                    msi.mouseDown = ui0.mouseDown; msi.mousePressed = ui0.mousePressed;
                    msi.wheel = (float)g_weaponScroll;
                    msi.keyW = glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS || glfwGetKey(window, GLFW_KEY_UP) == GLFW_PRESS;
                    msi.keyS = glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS || glfwGetKey(window, GLFW_KEY_DOWN) == GLFW_PRESS;
                    msi.keyA = glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS || glfwGetKey(window, GLFW_KEY_LEFT) == GLFW_PRESS;
                    msi.keyD = glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS || glfwGetKey(window, GLFW_KEY_RIGHT) == GLFW_PRESS;
                    const bool entNowW = glfwGetKey(window, GLFW_KEY_ENTER) == GLFW_PRESS ||
                                         glfwGetKey(window, GLFW_KEY_KP_ENTER) == GLFW_PRESS;
                    msi.enterEdge = entNowW && !prevEnterW;
                    prevEnterW = entNowW;
                    msi.escEdge = mapEscW;
                    msi.playerX = camX; msi.playerY = camY - 1.6f; msi.playerZ = camZ;
                    msi.playerYaw = camYaw;
                    msi.locationName = "KETH'ZAR - SEAMLESS WORLD";
                    wmap.drawScreen(wmapUi, *device, frame, msi, wflags, dt);
                    wmapUi.end();
                    prevLmbW = lmbW;

                    // FAST TRAVEL: snap the player; the NEXT wsm.update tick sees
                    // the new position -- if the region has not realized yet the
                    // PROXY collision floor engages (soft fallback) and releases
                    // when the content lands. The blackout fade covers the window.
                    if (wmap.travelRequested()) {
                        if (const x3::game::MapPoi* tgt = wmap.travelTarget()) {
                            float tg[3]; x3::game::placeOnTerrain(tgt->x, tgt->z, tg);
                            const float ty = (tgt->y != 0.0f ? tgt->y : tg[1]) + 2.0f;
                            wplayer.setFeetPosition(*wphys, x3::phys::Vec3{ tgt->x, ty, tgt->z });
                            if (noclipW) { flyXw = tgt->x; flyYw = ty + 1.6f; flyZw = tgt->z; }
                            prevPX = tgt->x; prevPZ = tgt->z;   // no teleport-spike velocity
                            wTravelFade = 0.9f;
                            wmapOpen = false; wmap.close();
                            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
                            glfwGetCursorPos(window, &lastMX, &lastMY);
                            wflags.saveFile("save/worldmap_streamed.flags");
                            x3::logInfo("[worldmap] FAST TRAVEL -> " + tgt->name);
                        }
                        wmap.clearTravelRequest();
                    }
                } else {
                    prevEnterW = glfwGetKey(window, GLFW_KEY_ENTER) == GLFW_PRESS;
                    prevLmbW = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
                }
                g_weaponScroll = 0.0;   // consumed (or discarded) every frame

                // Fast-travel blackout cover.
                if (wTravelFade > 0.0f) {
                    wTravelFade -= dt; if (wTravelFade < 0.0f) wTravelFade = 0.0f;
                    const float fa = std::min(1.0f, wTravelFade / 0.45f);
                    int fbw = 0, fbh = 0; glfwGetFramebufferSize(window, &fbw, &fbh);
                    const float blk[4] = { 0.0f, 0.0f, 0.0f, fa };
                    device->drawHudQuad(frame, 0.0f, 0.0f, (float)fbw, (float)fbh, blk);
                }
            }
            device->endFrame(frame);
        }

        wflags.saveFile("save/worldmap_streamed.flags");
        wmap.shutdown(*device);
        wsm.shutdown(wscene, *device, *wphys);
        wstream.shutdown(wscene, *device, *wphys);
        wjobs->shutdown();
        wphys->shutdown();
        device->shutdown();
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return 0;
    }
}

}} // namespace x3::apphost
