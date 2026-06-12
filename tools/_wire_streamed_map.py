# One-shot wiring script for the streamed-world map integration (deleted after use).
import io

p = 'app/main.cpp'
s = io.open(p, encoding='utf-8', newline='').read()
NL = '\r\n'


def rep(old, new, count=1):
    global s
    old = old.replace('\n', NL)
    new = new.replace('\n', NL)
    assert s.count(old) == count, (old[:70], s.count(old))
    s = s.replace(old, new)


# C) Windowed streamed loop: map system setup after the player spawn.
rep('''        x3::game::Player wplayer;
        wplayer.spawn(*wphys, sax, sgr[1] + 2.0f, saz);

        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);''',
'''        x3::game::Player wplayer;
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

        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);''')

# D) Esc + kd gating + M toggle in the loop head.
rep('''        while (!glfwWindowShouldClose(window)) {
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
            bool fNow = kd(GLFW_KEY_F);''',
'''        while (!glfwWindowShouldClose(window)) {
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
            bool fNow = kd(GLFW_KEY_F);''')

# E) Frame: map draw + discovery + fast travel + fade, before endFrame.
rep('''            device->setCamera(camX, camY, camZ, camYaw, camPitch, 60.0f);
            auto frame = device->beginFrame();
            if (frame.valid) wscene.render(*device, frame);
            device->endFrame(frame);
        }

        wsm.shutdown(wscene, *device, *wphys);''',
'''            device->setCamera(camX, camY, camZ, camYaw, camPitch, 60.0f);
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
        wsm.shutdown(wscene, *device, *wphys);''')

io.open(p, 'w', encoding='utf-8', newline='').write(s)
print('ok streamed loop')
